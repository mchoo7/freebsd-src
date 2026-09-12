/*
 * Copyright (c) 2026 FreeBSD Foundation
 *
 * This software was developed by Minsoo Choo under sponsorship from the
 * FreeBSD Foundation.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef _MACHINE_TRAP_CFI_H_
#define	_MACHINE_TRAP_CFI_H_

#include "assym.inc"

	.macro	trap_cfi_entry state=empty
	.cfi_signal_frame
	.cfi_return_column %rip
	.cfi_def_cfa %rsp, 0
	.cfi_undefined %rip
.ifc \state,trapframe
	trap_cfi_trapframe
.endif
.ifc \state,terminal
	.cfi_def_cfa %rsp, TF_SIZE
.endif
	.endm

	.macro	trap_cfi_machine has_err=0
.if \has_err
	.cfi_def_cfa %rsp, 48
.else
	.cfi_def_cfa %rsp, 40
.endif
	.cfi_offset %rip, -40
#if !defined(__clang__) || (__clang_major__ >= 17)
	.cfi_offset %cs, -32
	.cfi_offset %rflags, -24
#endif
	.cfi_offset %rsp, -16
#if !defined(__clang__) || (__clang_major__ >= 17)
	.cfi_offset %ss, -8
#endif
	.endm

	.macro	trap_cfi_saved reg, off
	.cfi_offset \reg, \off - TF_SIZE
	.endm

	.macro	trap_cfi_trapframe
	.cfi_def_cfa %rsp, TF_SIZE
	trap_cfi_saved %rdi, TF_RDI
	trap_cfi_saved %rsi, TF_RSI
	trap_cfi_saved %rdx, TF_RDX
	trap_cfi_saved %rcx, TF_RCX
	trap_cfi_saved %r8, TF_R8
	trap_cfi_saved %r9, TF_R9
	trap_cfi_saved %rax, TF_RAX
	trap_cfi_saved %rbx, TF_RBX
	trap_cfi_saved %rbp, TF_RBP
	trap_cfi_saved %r10, TF_R10
	trap_cfi_saved %r11, TF_R11
	trap_cfi_saved %r12, TF_R12
	trap_cfi_saved %r13, TF_R13
	trap_cfi_saved %r14, TF_R14
	trap_cfi_saved %r15, TF_R15
	trap_cfi_saved %rip, TF_RIP
#if !defined(__clang__) || (__clang_major__ >= 17)
	trap_cfi_saved %cs, TF_CS
	trap_cfi_saved %rflags, TF_RFLAGS
#endif
	trap_cfi_saved %rsp, TF_RSP
#if !defined(__clang__) || (__clang_major__ >= 17)
	trap_cfi_saved %ss, TF_SS
#endif
	.endm

	.macro	trap_cfi_gprs_live
	.irp reg,%rdi,%rsi,%rdx,%rcx,%r8,%r9,%rax,%rbx,%rbp,%r10,%r11,%r12,%r13,%r14,%r15
	.cfi_same_value \reg
	.endr
	.endm

#endif /* _MACHINE_TRAP_CFI_H_ */
