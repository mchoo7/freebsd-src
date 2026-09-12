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

	.cfi_sections .eh_frame

#define	CFI_PC		32
#define	CFI_CPSR	33

.macro	trap_cfi_entry name, state=empty
	.cfi_startproc
	.cfi_signal_frame
	.cfi_return_column CFI_PC
	.cfi_def_cfa sp, 0
	.cfi_undefined CFI_PC
.ifc \state,full
	trap_cfi_full
.endif
.ifc \state,terminal
	.cfi_def_cfa sp, TF_SIZE
.endif
.endm

.macro	trap_cfi_end name
	.ifnb \name
	.ltorg
	.endif
	.cfi_endproc
	.ifnb \name
	.size \name, . - \name
	.endif
.endm

.macro	trap_cfi_saved reg, off
	.cfi_offset \reg, \off - TF_SIZE
.endm

.macro	trap_cfi_full
	.cfi_def_cfa sp, TF_SIZE
	trap_cfi_saved 31, TF_SP
	trap_cfi_saved x30, TF_LR
	trap_cfi_saved CFI_PC, TF_ELR
	trap_cfi_saved CFI_CPSR, TF_SPSR
	.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29
	trap_cfi_saved x\n, TF_X + \n * 8
	.endr
.endm

#endif /* _MACHINE_TRAP_CFI_H_ */
