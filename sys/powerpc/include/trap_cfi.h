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

#ifdef __powerpc64__
#define	CFI_WORD_SIZE	8
#define	CFI_TF_PREFIX	48
#if defined(__LITTLE_ENDIAN__) || \
    (defined(_CALL_ELF) && _CALL_ELF == 2)
#define	CFI_CR		68
#define	CFI_XER		76
#define	CFI_LR		65
#define	CFI_CTR		66
#define	CFI_PC		118
#else
#define	CFI_CR		64
#define	CFI_XER		100
#define	CFI_LR		108
#define	CFI_CTR		109
#define	CFI_PC		357
#endif
#else
#define	CFI_WORD_SIZE	4
#define	CFI_TF_PREFIX	8
#define	CFI_CR		64
#define	CFI_XER		101
#define	CFI_LR		108
#define	CFI_CTR		109
#define	CFI_PC		110
#endif

#define	CFI_TF_SIZE	(CFI_TF_PREFIX + 42 * CFI_WORD_SIZE)

	.cfi_sections .eh_frame

.macro	trap_cfi_entry name, state=empty
.ifnc \state,attached
	.cfi_startproc
.endif
	.cfi_signal_frame
	.cfi_return_column CFI_PC
	.cfi_def_cfa %r1, 0
	.cfi_undefined CFI_PC
.ifc \state,full
	trap_cfi_full
.endif
.ifc \state,terminal
	.cfi_def_cfa %r1, CFI_TF_SIZE
.endif
.endm

.macro	trap_cfi_end name
	.cfi_endproc
.endm

.macro	trap_cfi_saved reg, off
	.cfi_offset \reg, CFI_TF_PREFIX + \off - CFI_TF_SIZE
.endm

.macro	trap_cfi_full
	.cfi_def_cfa %r1, CFI_TF_SIZE
	.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
	.cfi_offset \n, CFI_TF_PREFIX + \n * CFI_WORD_SIZE - CFI_TF_SIZE
	.endr
	trap_cfi_saved CFI_LR, FRAME_LR
	trap_cfi_saved CFI_CR, FRAME_CR
	trap_cfi_saved CFI_XER, FRAME_XER
	trap_cfi_saved CFI_CTR, FRAME_CTR
	trap_cfi_saved CFI_PC, FRAME_SRR0
.endm

#endif /* _MACHINE_TRAP_CFI_H_ */
