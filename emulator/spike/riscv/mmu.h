// See LICENSE for license details.

#ifndef _RISCV_MMU_H
#define _RISCV_MMU_H

#include "decode.h"
#include "trap.h"
#include "common.h"
#include "config.h"
#include "processor.h"
#include "memtracer.h"
#include <stdlib.h>
#include <vector>

// virtual memory configuration
#define PGSHIFT 12
const reg_t PGSIZE = 1 << PGSHIFT;

struct insn_fetch_t
{
  insn_func_t func;
  insn_t insn;
};

struct icache_entry_t {
  reg_t tag;
  reg_t pad;
  insn_fetch_t data;
};

// this class implements a processor's port into the virtual memory system.
// an MMU and instruction cache are maintained for simulator performance.
class mmu_t
{
public:
  mmu_t(char* _mem, size_t _memsz);
  ~mmu_t();

  // template for functions that load an aligned value from memory
  #define load_func(type) \
    type##_t load_##type(reg_t addr) __attribute__((always_inline)) { \
      void* paddr = translate(addr, sizeof(type##_t), false, false); \
      return *(type##_t*)paddr; \
    }

  // load value from memory at aligned address; zero extend to register width
  load_func(uint8)
  load_func(uint16)
  load_func(uint32)
  load_func(uint64)

  // load value from memory at aligned address; sign extend to register width
  load_func(int8)
  load_func(int16)
  load_func(int32)
  load_func(int64)

  // check tag and load
  int64_t load_int64_with_tag(reg_t addr, uint8_t tag) __attribute__((always_inline)) {
    void* paddr = translate(addr, sizeof(int64_t), false, false);
    if (!check_tag(paddr, tag))
      throw trap_load_tag_mismatch(addr);
    return *(int64_t*)paddr;
  }

  // template for functions that store an aligned value to memory
  // default tag = 0
  #define store_func(type) \
    void store_##type(reg_t addr, type##_t val) { \
      void* paddr = translate(addr, sizeof(type##_t), true, false); \
      *(type##_t*)paddr = val; \
      set_tag(paddr, 0); \
    }

  // store value to memory at aligned address
  store_func(uint8)
  store_func(uint16)
  store_func(uint32)
  store_func(uint64)

  // store and set tag
  void store_uint64_with_tag(reg_t addr, uint64_t val, uint8_t tag) {
    void* paddr = translate(addr, sizeof(uint64_t), true, false);
    *(uint64_t*)paddr = val;
    set_tag(paddr, tag);
  }

  static const reg_t ICACHE_ENTRIES = 1024;

  inline size_t icache_index(reg_t addr)
  {
    return (addr / PC_ALIGN) % ICACHE_ENTRIES;
  }

  inline icache_entry_t* refill_icache(reg_t addr, icache_entry_t* entry)
  {
    char* iaddr = (char*)translate(addr, 1, false, true);
    insn_bits_t insn = *(uint16_t*)iaddr;
    int length = insn_length(insn);

    if (likely(length == 4)) {
      if (likely(addr % PGSIZE < PGSIZE-2))
        insn |= (insn_bits_t)*(int16_t*)(iaddr + 2) << 16;
      else
        insn |= (insn_bits_t)*(int16_t*)translate(addr + 2, 1, false, true) << 16;
    } else if (length == 2) {
      insn = (int16_t)insn;
    } else if (length == 6) {
      insn |= (insn_bits_t)*(int16_t*)translate(addr + 4, 1, false, true) << 32;
      insn |= (insn_bits_t)*(uint16_t*)translate(addr + 2, 1, false, true) << 16;
    } else {
      static_assert(sizeof(insn_bits_t) == 8, "insn_bits_t must be uint64_t");
      insn |= (insn_bits_t)*(int16_t*)translate(addr + 6, 1, false, true) << 48;
      insn |= (insn_bits_t)*(uint16_t*)translate(addr + 4, 1, false, true) << 32;
      insn |= (insn_bits_t)*(uint16_t*)translate(addr + 2, 1, false, true) << 16;
    }

    insn_fetch_t fetch = {proc->decode_insn(insn), insn};
    entry->tag = addr;
    entry->data = fetch;

    reg_t paddr = iaddr - mem;
    if (tracer.interested_in_range(paddr, paddr + 1, false, true)) {
      entry->tag = -1;
      tracer.trace(paddr, length, false, true);
    }
    return entry;
  }

  inline icache_entry_t* access_icache(reg_t addr)
  {
    icache_entry_t* entry = &icache[icache_index(addr)];
    if (likely(entry->tag == addr))
      return entry;
    return refill_icache(addr, entry);
  }

  inline insn_fetch_t load_insn(reg_t addr)
  {
    return access_icache(addr)->data;
  }

  void set_processor(processor_t* p) { proc = p; flush_tlb(); }

  void flush_tlb();
  void flush_icache();

  void register_memtracer(memtracer_t*);

private:
  char* mem;
  size_t memsz;
  processor_t* proc;
  memtracer_list_t tracer;
  char* mem_tags;

  // implement an instruction cache for simulator performance
  icache_entry_t icache[ICACHE_ENTRIES];

  // implement a TLB for simulator performance
  static const reg_t TLB_ENTRIES = 256;
  char* tlb_data[TLB_ENTRIES];
  reg_t tlb_insn_tag[TLB_ENTRIES];
  reg_t tlb_load_tag[TLB_ENTRIES];
  reg_t tlb_store_tag[TLB_ENTRIES];

  // finish translation on a TLB miss and upate the TLB
  void* refill_tlb(reg_t addr, reg_t bytes, bool store, bool fetch);

  // perform a page table walk for a given VA; set referenced/dirty bits
  reg_t walk(reg_t addr, bool supervisor, bool store, bool fetch);

  // translate a virtual address to a physical address
  void* translate(reg_t addr, reg_t bytes, bool store, bool fetch)
    __attribute__((always_inline))
  {
    reg_t idx = (addr >> PGSHIFT) % TLB_ENTRIES;
    reg_t expected_tag = addr >> PGSHIFT;
    reg_t* tags = fetch ? tlb_insn_tag : store ? tlb_store_tag :tlb_load_tag;
    reg_t tag = tags[idx];
    void* data = tlb_data[idx] + addr;

    if (unlikely(addr & (bytes-1)))
      store ? throw trap_store_address_misaligned(addr) :
      fetch ? throw trap_instruction_address_misaligned(addr) :
      throw trap_load_address_misaligned(addr);

    if (likely(tag == expected_tag))
      return data;

    return refill_tlb(addr, bytes, store, fetch);
  }

  // initialize memory tags
  void init_tags();

  // check memory tag
  bool check_tag(void* paddr, uint8_t tag)
    __attribute__((always_inline))
  {
    reg_t offset = (reg_t)paddr - (reg_t)mem;
    reg_t index, mask;

    index = offset >> 6;
    mask = 1 << ((offset >> 3) & 0x7);
    tag <<= ((offset >> 3) & 0x7);

    return ((mem_tags[index] & mask) == tag);
  }

  // set memory tag
  void set_tag(void* paddr, uint8_t tag)
    __attribute__((always_inline))
  {
    reg_t offset = (reg_t)(paddr) - (reg_t)mem;
    reg_t index, mask;

    index = offset >> 6;
    mask = 1 << ((offset >> 3) & 0x7);

    if (tag)
      mem_tags[index] |= mask;
    else
      mem_tags[index] &= ~mask;
  }
  
  friend class processor_t;
};

#endif
