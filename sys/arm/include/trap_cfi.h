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

#define	CFI_TF_SIZE	(TF_PC + 4)
#define	CFI_CPSR	16

.macro	trap_cfi_entry name, state=empty
	.cfi_startproc
	.cfi_signal_frame
	.cfi_return_column 15
	.cfi_def_cfa sp, 0
	.cfi_undefined 15
.ifc \state,terminal
	.cfi_def_cfa sp, CFI_TF_SIZE
.endif
.endm

.macro	trap_cfi_end name
	.cfi_endproc
.endm

.macro	trap_cfi_saved reg, off
	.cfi_offset \reg, \off - CFI_TF_SIZE
.endm

.macro	trap_cfi_full mode
	.cfi_def_cfa sp, CFI_TF_SIZE
	trap_cfi_saved CFI_CPSR, TF_SPSR
	.irp n,0,1,2,3,4,5,6,7,8,9,10,11,12
	trap_cfi_saved r\n, TF_R0 + \n * 4
	.endr
.ifc \mode,user
	trap_cfi_saved sp, TF_R0 + 13 * 4
	trap_cfi_saved lr, TF_R0 + 14 * 4
.else
	trap_cfi_saved sp, TF_R0 + 15 * 4
	trap_cfi_saved lr, TF_R0 + 16 * 4
.endif
	trap_cfi_saved 15, TF_PC
.endm

#endif /* _MACHINE_TRAP_CFI_H_ */
