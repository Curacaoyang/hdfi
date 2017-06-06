# RUN: llvm-mc -triple s390x-linux-gnu -show-encoding %s | FileCheck %s

#CHECK: algfi	%r0, 0                  # encoding: [0xc2,0x0a,0x00,0x00,0x00,0x00]
#CHECK: algfi	%r0, 4294967295         # encoding: [0xc2,0x0a,0xff,0xff,0xff,0xff]
#CHECK: algfi	%r15, 0                 # encoding: [0xc2,0xfa,0x00,0x00,0x00,0x00]

	algfi	%r0, 0
	algfi	%r0, (1 << 32) - 1
	algfi	%r15, 0