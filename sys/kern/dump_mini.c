/*-
 * Copyright (c) 2002 Marcel Moolenaar
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/kerneldump.h>
#include <sys/malloc.h>
#include <sys/msgbuf.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <vm/vm_dumpset.h>

int
dump_mini(struct dumperinfo *di, bool livedump)
{
#if MINIDUMP_PAGE_TRACKING == 1
	struct minidumpstate state;
	struct msgbuf mb_copy;
	char *msg_ptr;
	int error;

	if (!dump_mini_supported())
		return (EOPNOTSUPP);

	if (livedump) {
		KASSERT(!dumping, ("live dump invoked from incorrect context"));

		/*
		 * Before invoking dump_mini_machdep() on the live system, we
		 * must snapshot some required global state: the message
		 * buffer, and the page dump bitset. They may be modified at
		 * any moment, so for the sake of the live dump it is best to
		 * have an unchanging snapshot to work with. Both are included
		 * as part of the dump and consumed by userspace tools.
		 *
		 * Other global state important to the minidump code is the
		 * dump_avail array and the kernel's page tables, but snapshots
		 * are not taken of these. For one, dump_avail[] is expected
		 * not to change after boot. Snapshotting the kernel page
		 * tables would involve an additional walk, so this is avoided
		 * too.
		 *
		 * This means live dumps are best effort, and the result may or
		 * may not be usable; there are no guarantees about the
		 * consistency of the dump's contents. Any of the following
		 * (and likely more) may affect the live dump:
		 *
		 *  - Data may be modified, freed, or remapped during the
		 *    course of the dump, such that the contents written out
		 *    are partially or entirely unrecognizable. This means
		 *    valid references may point to destroyed/mangled objects,
		 *    and vice versa.
		 *
		 *  - The dumped context of any threads that ran during the
		 *    dump process may be unreliable.
		 *
		 *  - The set of kernel page tables included in the dump likely
		 *    won't correspond exactly to the copy of the dump bitset.
		 *    This means some pages will be dumped without any way to
		 *    locate them, and some pages may not have been dumped
		 *    despite appearing as if they should.
		 */
		msg_ptr = malloc(msgbufsize, M_TEMP, M_WAITOK);
		msgbuf_duplicate(msgbufp, &mb_copy, msg_ptr);
		state.msgbufp = &mb_copy;

		state.dump_bitset = BITSET_ALLOC(vm_page_dump_pages, M_TEMP,
		    M_WAITOK);
		BIT_COPY_STORE_REL(vm_page_dump_pages, vm_page_dump,
		    state.dump_bitset);
	} else {
		KASSERT(dumping, ("minidump invoked outside of doadump()"));

		/* Use the globals. */
		state.msgbufp = msgbufp;
		state.dump_bitset = vm_page_dump;
	}

	error = dump_mini_machdep(di, &state);
	if (livedump) {
		free(msg_ptr, M_TEMP);
		BITSET_FREE(state.dump_bitset, M_TEMP);
	}

	return (error);
#else
	/* 32-bit PowerPC minidumps use the ELF dump path. */
	if (livedump || !dump_mini_supported())
		return (EOPNOTSUPP);
	return (dump_full(di));
#endif /* MINIDUMP_PAGE_TRACKING == 1 */
}
