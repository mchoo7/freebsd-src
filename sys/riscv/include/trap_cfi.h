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

/* DWARF register 64 is the RISC-V alternate frame return column. */
#define	CFI_RA_COLUMN	64
#define	CFI_TF_SIZE	(TF_SCAUSE + 8)

.macro trap_cfi_entry name, state=empty
	.cfi_startproc
	.cfi_signal_frame
	.cfi_return_column CFI_RA_COLUMN
	.cfi_def_cfa sp, 0
	.cfi_undefined CFI_RA_COLUMN
.ifc \state,full
	trap_cfi_full
.endif
.ifc \state,terminal
	.cfi_def_cfa sp, CFI_TF_SIZE
.endif
.endm

.macro trap_cfi_end name
	.cfi_endproc
.endm

.macro trap_cfi_saved reg, off
	.cfi_offset \reg, \off - CFI_TF_SIZE
.endm

.macro trap_cfi_full
	.cfi_def_cfa sp, CFI_TF_SIZE
	.cfi_same_value zero
	trap_cfi_saved ra, TF_RA
	trap_cfi_saved sp, TF_SP
	trap_cfi_saved gp, TF_GP
	trap_cfi_saved tp, TF_TP
	trap_cfi_saved t0, TF_T + 0 * 8
	trap_cfi_saved t1, TF_T + 1 * 8
	trap_cfi_saved t2, TF_T + 2 * 8
	trap_cfi_saved t3, TF_T + 3 * 8
	trap_cfi_saved t4, TF_T + 4 * 8
	trap_cfi_saved t5, TF_T + 5 * 8
	trap_cfi_saved t6, TF_T + 6 * 8
	trap_cfi_saved s0, TF_S + 0 * 8
	trap_cfi_saved s1, TF_S + 1 * 8
	trap_cfi_saved s2, TF_S + 2 * 8
	trap_cfi_saved s3, TF_S + 3 * 8
	trap_cfi_saved s4, TF_S + 4 * 8
	trap_cfi_saved s5, TF_S + 5 * 8
	trap_cfi_saved s6, TF_S + 6 * 8
	trap_cfi_saved s7, TF_S + 7 * 8
	trap_cfi_saved s8, TF_S + 8 * 8
	trap_cfi_saved s9, TF_S + 9 * 8
	trap_cfi_saved s10, TF_S + 10 * 8
	trap_cfi_saved s11, TF_S + 11 * 8
	trap_cfi_saved a0, TF_A + 0 * 8
	trap_cfi_saved a1, TF_A + 1 * 8
	trap_cfi_saved a2, TF_A + 2 * 8
	trap_cfi_saved a3, TF_A + 3 * 8
	trap_cfi_saved a4, TF_A + 4 * 8
	trap_cfi_saved a5, TF_A + 5 * 8
	trap_cfi_saved a6, TF_A + 6 * 8
	trap_cfi_saved a7, TF_A + 7 * 8
	.cfi_offset CFI_RA_COLUMN, TF_SEPC - CFI_TF_SIZE
.endm

#endif /* _MACHINE_TRAP_CFI_H_ */
