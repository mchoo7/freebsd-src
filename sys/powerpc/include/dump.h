/*-
 * Copyright (c) 2014 EMC Corp.
 * Author: Conrad Meyer <conrad.meyer@isilon.com>
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

#ifndef _MACHINE_DUMP_H_
#define	_MACHINE_DUMP_H_

#define	DUMP_ARCH_VERSION	KERNELDUMP_POWERPC_VERSION
#define	EM_VALUE		ELF_ARCH /* Defined in powerpc/include/elf.h */
#define	DUMP_FULL_MD_PA_NPAIRS	(PHYS_AVAIL_SZ + 1)
#define	DUMP_FULL_NUM_AUX_HDRS	0

/* How often to check the dump progress bar? */
#define	DUMP_PB_CHECK_BITS	20	/* Every 1MB */

void dump_full_pa_init(void);
void dump_full_unmap_chunk(vm_paddr_t, size_t, void *);
size_t dump_mini_scan_pmap(struct bitset *);
void *dump_mini_pmap_init(unsigned blkpgs);
void *dump_mini_pmap(void *ctx, void *buf, u_long *nbytes);

static inline struct dump_pa *
dump_full_pa_next(struct dump_pa *p)
{
	return (dump_full_gen_pa_next(p));
}

static inline void
dump_full_wbinv_all(void)
{
	dump_full_gen_wbinv_all();
}

static inline int
dump_full_write_aux_headers(struct dumperinfo *di)
{
	return (dump_full_gen_write_aux_headers(di));
}

#endif  /* !_MACHINE_DUMP_H_ */
