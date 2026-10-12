/*-
 * Copyright (c) 2014 EMC Corp.
 * Author: Conrad Meyer <conrad.meyer@isilon.com>
 * Copyright (c) 2015 The FreeBSD Foundation.
 * All rights reserved.
 *
 * Portions of this software were developed by Andrew Turner
 * under sponsorship from the FreeBSD Foundation
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

#define	DUMP_ARCH_VERSION	KERNELDUMP_AARCH64_VERSION
#define	EM_VALUE		EM_AARCH64
/* XXX: I suppose 20 should be enough. */
#define	DUMP_FULL_MD_PA_NPAIRS	20
#define	DUMP_FULL_NUM_AUX_HDRS	1

/* How often to check the dump progress bar? */
#define	DUMP_PB_CHECK_BITS	22	/* Every 4MB */

void dump_full_wbinv_all(void);
int dump_full_write_aux_headers(struct dumperinfo *di);

static inline void
dump_full_pa_init(void)
{
	dump_full_gen_pa_init();
}

static inline struct dump_pa *
dump_full_pa_next(struct dump_pa *p)
{
	return (dump_full_gen_pa_next(p));
}

static inline void
dump_full_unmap_chunk(vm_paddr_t pa, size_t s, void *va)
{
	dump_full_gen_unmap_chunk(pa, s, va);
}

#endif  /* !_MACHINE_DUMP_H_ */
