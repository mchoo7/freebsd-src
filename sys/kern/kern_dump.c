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
#include <sys/kernel.h>
#include <sys/kerneldump.h>
#include <sys/sysctl.h>

#include <vm/vm.h>
#include <vm/vm_param.h>

#include <machine/dump.h>

int dump_format = DUMP_FULL;

static void
format_init(void *arg __unused)
{
	dump_format = dump_mini_supported() ? DUMP_MINI : DUMP_FULL;
}
SYSINIT(dump_format, SI_SUB_VM, SI_ORDER_ANY, format_init, NULL);

SYSCTL_NODE(_debug, OID_AUTO, dump, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "Kernel crash dumps");

static int
set_format(int format)
{
	if (format == DUMP_MINI && !dump_mini_supported())
		return (EOPNOTSUPP);
	dump_format = format;
	return (0);
}

static int
sysctl_minidump(SYSCTL_HANDLER_ARGS)
{
	int error, minidump;

	minidump = dump_format == DUMP_MINI;
	error = sysctl_handle_int(oidp, &minidump, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);

	return (set_format(minidump != 0 ? DUMP_MINI : DUMP_FULL));
}
SYSCTL_PROC(_debug, OID_AUTO, minidump,
    CTLTYPE_INT | CTLFLAG_RWTUN | CTLFLAG_MPSAFE, NULL, 0,
    sysctl_minidump, "I",
    "Enable mini crash dumps (compatibility alias for debug.dump.format)");

static int
sysctl_dump_format(SYSCTL_HANDLER_ARGS)
{
	char format[16];
	int error;

	strlcpy(format, dump_format == DUMP_MINI ? "mini" : "full",
	    sizeof(format));
	error = sysctl_handle_string(oidp, format, sizeof(format), req);
	if (error != 0 || req->newptr == NULL)
		return (error);

	if (strcmp(format, "full") == 0)
		return (set_format(DUMP_FULL));
	else if (strcmp(format, "mini") == 0)
		return (set_format(DUMP_MINI));
	else
		return (EINVAL);
}
SYSCTL_PROC(_debug_dump, OID_AUTO, format,
    CTLTYPE_STRING | CTLFLAG_RWTUN | CTLFLAG_MPSAFE, NULL, 0,
    sysctl_dump_format, "A", "Kernel crash dump format (full or mini)");

int
dump(struct dumperinfo *di)
{
	switch (dump_format) {
	case DUMP_FULL:
		return (dump_full(di));
	case DUMP_MINI:
		return (dump_mini(di, false));
	default:
		return (EINVAL);
	}
}

/* Dump progress bar. */
static struct {
	const int min_per;
	const int max_per;
	bool visited;
} progress_track[11] = {
	{   0,   9, false},
	{  10,  19, false},
	{  20,  29, false},
	{  30,  39, false},
	{  40,  49, false},
	{  50,  59, false},
	{  60,  69, false},
	{  70,  79, false},
	{  80,  89, false},
	{  90,  99, false},
	{ 100, 100, false},
};

static uint64_t dump_pb_size;
static uint64_t dump_pb_remaining;
static uint64_t dump_pb_check;

/* Reset the progress bar for a dump of dumpsize. */
void
dump_pb_init(uint64_t dumpsize)
{
	int i;

	dump_pb_size = dump_pb_remaining = dumpsize;
	dump_pb_check = 0;

	for (i = 0; i < nitems(progress_track); i++)
		progress_track[i].visited = false;
}

/*
 * Update the progress according to the delta bytes that were written out.
 * Check and print the progress percentage.
 */
void
dump_pb_update(size_t delta)
{
	int sofar, i;

	dump_pb_remaining -= delta;
	dump_pb_check += delta;

	/*
	 * To save time while dumping, only loop through progress_track
	 * occasionally.
	 */
	if ((dump_pb_check >> DUMP_PB_CHECK_BITS) == 0)
		return;
	else
		dump_pb_check &= (1 << DUMP_PB_CHECK_BITS) - 1;

	sofar = 100 - ((dump_pb_remaining * 100) / dump_pb_size);
	for (i = 0; i < nitems(progress_track); i++) {
		if (sofar < progress_track[i].min_per ||
		    sofar > progress_track[i].max_per)
			continue;
		if (!progress_track[i].visited) {
			progress_track[i].visited = true;
			printf("..%d%%", sofar);
		}
		break;
	}
}
