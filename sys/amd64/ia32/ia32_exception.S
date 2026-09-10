/*-
 * Copyright (c) 2003 Peter Wemm
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <machine/asmacros.h>

#include "assym.inc"

	.text
/*
 * Call gate entry for FreeBSD ELF and Linux/NetBSD syscall (int 0x80)
 *
 * This is a SDT_SYSIDT entry point (unlike the i386 port) so that we
 * can do a swapgs before enabling interrupts.  This is critical because
 * if we took an interrupt before swapgs, the interrupt code would see
 * that it originated in supervisor mode and skip the swapgs.
 */
	SUPERALIGN_TEXT
IDTVEC(int0x80_syscall_pti)
	trap_cfi_entry ., empty
	trap_cfi_machine
	PTI_UENTRY has_err=0, cfi=1
	jmp	int0x80_syscall_common
	trap_cfi_end .
	SUPERALIGN_TEXT
IDTVEC(int0x80_syscall)
	trap_cfi_entry ., empty
	trap_cfi_machine
	swapgs
	trap_cfi_end .
int0x80_syscall_common:
	trap_cfi_entry ., empty
	trap_cfi_machine
	pushq	$2			/* sizeof "int 0x80" */
	.cfi_adjust_cfa_offset 8
	subq	$TF_ERR,%rsp		/* skip over tf_trapno */
	.cfi_def_cfa_offset TF_SIZE
	movq	%rdi,TF_RDI(%rsp)
	trap_cfi_saved %rdi, TF_RDI
	movq	PCPU(CURPCB),%rdi
	andl	$~PCB_FULL_IRET,PCB_FLAGS(%rdi)
	SAVE_SEGS
	movq	%rax,TF_RAX(%rsp)
	trap_cfi_saved %rax, TF_RAX
	movq	%rdx,TF_RDX(%rsp)
	trap_cfi_saved %rdx, TF_RDX
	movq	%rcx,TF_RCX(%rsp)
	trap_cfi_saved %rcx, TF_RCX
	call	handle_ibrs_entry
	sti
	movq	%rsi,TF_RSI(%rsp)
	trap_cfi_saved %rsi, TF_RSI
	movq	%r8,TF_R8(%rsp)
	trap_cfi_saved %r8, TF_R8
	movq	%r9,TF_R9(%rsp)
	trap_cfi_saved %r9, TF_R9
	movq	%rbx,TF_RBX(%rsp)
	trap_cfi_saved %rbx, TF_RBX
	movq	%rbp,TF_RBP(%rsp)
	trap_cfi_saved %rbp, TF_RBP
	movq	%r10,TF_R10(%rsp)
	trap_cfi_saved %r10, TF_R10
	movq	%r11,TF_R11(%rsp)
	trap_cfi_saved %r11, TF_R11
	movq	%r12,TF_R12(%rsp)
	trap_cfi_saved %r12, TF_R12
	movq	%r13,TF_R13(%rsp)
	trap_cfi_saved %r13, TF_R13
	movq	%r14,TF_R14(%rsp)
	trap_cfi_saved %r14, TF_R14
	movq	%r15,TF_R15(%rsp)
	trap_cfi_saved %r15, TF_R15
	movl	$TF_HASSEGS,TF_FLAGS(%rsp)
	pushfq
	andq	$~(PSL_D | PSL_AC),(%rsp)
	popfq
	movq	%rsp, %rdi
	call	ia32_syscall
	jmp	doreti
	trap_cfi_end .
