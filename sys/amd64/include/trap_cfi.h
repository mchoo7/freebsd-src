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

	.macro	trap_cfi_entry name, state=empty
	.cfi_sections .eh_frame
	.cfi_startproc
	.cfi_signal_frame
	.cfi_return_column %rip
	.cfi_def_cfa %rsp, 0
	.cfi_undefined %rip
.ifc \state,full
	trap_cfi_full
.endif
.ifc \state,terminal
	.cfi_def_cfa %rsp, TF_SIZE
.endif
	.endm

	.macro	trap_cfi_end name
	.cfi_endproc
	.endm

	.macro	trap_cfi_machine has_err=0
.if \has_err
	.cfi_def_cfa %rsp, 48
.else
	.cfi_def_cfa %rsp, 40
.endif
	.cfi_offset %rip, -40
	.cfi_offset %cs, -32
	.cfi_offset 49, -24	/* rflags */
	.cfi_offset %rsp, -16
	.cfi_offset %ss, -8
	.endm

	.macro	trap_cfi_saved reg, off
	.cfi_offset \reg, \off - TF_SIZE
	.endm

	.macro	trap_cfi_full
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
	trap_cfi_saved %cs, TF_CS
	trap_cfi_saved 49, TF_RFLAGS
	trap_cfi_saved %rsp, TF_RSP
	trap_cfi_saved %ss, TF_SS
	.endm

	.macro	trap_cfi_gprs_live
	.irp reg,%rdi,%rsi,%rdx,%rcx,%r8,%r9,%rax,%rbx,%rbp,%r10,%r11,%r12,%r13,%r14,%r15
	.cfi_same_value \reg
	.endr
	.endm

#endif /* _MACHINE_TRAP_CFI_H_ */
