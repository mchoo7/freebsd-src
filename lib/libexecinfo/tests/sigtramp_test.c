/*-
 * Copyright (c) 2023 Dmitry Chagin <dchagin@FreeBSD.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>

#include <errno.h>
#include <execinfo.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atf-c.h>

#define	BT_FUNCTIONS		32

static void *addresses[BT_FUNCTIONS];
static size_t naddresses;
static uintptr_t altstack_base;
static size_t altstack_size;
static volatile sig_atomic_t handler_calls;
static volatile sig_atomic_t handler_on_altstack;

void	handler(int);
void	signal_caller(void);

__noinline void
handler(int signum __unused)
{
	uintptr_t sp;
	int saved_errno;
	char marker;

	saved_errno = errno;
	sp = (uintptr_t)&marker;
	handler_on_altstack = sp >= altstack_base &&
	    sp - altstack_base < altstack_size;
	naddresses = backtrace(addresses, nitems(addresses));
	handler_calls++;
	errno = saved_errno;
}

/* Keep a known frame below the signal trampoline, including at -O2. */
__noinline void
signal_caller(void)
{

	/* The result check prevents a tail call to raise(). */
	ATF_REQUIRE(raise(SIGUSR1) == 0);
}

static void
check_backtrace(void)
{
	char **symbols;
	size_t caller_frame, handler_frame, i;

#if !defined(__amd64__)
	/*
	 * https://reviews.llvm.org is deprecated and
	 * this review is never going to be updated or completed
	 */
	atf_tc_expect_fail("https://reviews.llvm.org/D155066 and https://bugs.freebsd.org/289096");
#endif

	ATF_REQUIRE_MSG(naddresses > 1, "backtrace returned %zu frames",
	    naddresses);
	symbols = backtrace_symbols_fmt(addresses, naddresses, "%n");
	ATF_REQUIRE(symbols != NULL);

	handler_frame = caller_frame = naddresses;
	for (i = 0; i < naddresses; i++) {
		printf("%zu: %p, %s\n", i, addresses[i], symbols[i]);
		if (strcmp(symbols[i], "handler") == 0)
			handler_frame = i;
		if (strcmp(symbols[i], "signal_caller") == 0)
			caller_frame = i;
	}
	ATF_REQUIRE_MSG(handler_frame < naddresses,
	    "signal handler missing from backtrace");
	ATF_REQUIRE_MSG(caller_frame < naddresses,
	    "backtrace did not reach the interrupted caller");
	ATF_CHECK_MSG(caller_frame > handler_frame + 1,
	    "no signal trampoline frame between handler and caller");
	free(symbols);
}

static void
run_sigtramp_test(bool use_altstack)
{
	struct sigaction act, oldact;
	stack_t ss, oldss;
	sigset_t mask, oldmask;

	altstack_base = 0;
	altstack_size = 0;
	if (use_altstack) {
		memset(&ss, 0, sizeof(ss));
		ss.ss_size = SIGSTKSZ;
		ss.ss_sp = malloc(ss.ss_size);
		ATF_REQUIRE(ss.ss_sp != NULL);
		ATF_REQUIRE(sigaltstack(&ss, &oldss) == 0);
		altstack_base = (uintptr_t)ss.ss_sp;
		altstack_size = ss.ss_size;
	}

	memset(&act, 0, sizeof(act));
	act.sa_handler = handler;
	if (use_altstack)
		act.sa_flags = SA_ONSTACK;
	ATF_REQUIRE(sigemptyset(&act.sa_mask) == 0);
	ATF_REQUIRE(sigaction(SIGUSR1, &act, &oldact) == 0);
	ATF_REQUIRE(sigemptyset(&mask) == 0);
	ATF_REQUIRE(sigaddset(&mask, SIGUSR1) == 0);
	ATF_REQUIRE(sigprocmask(SIG_UNBLOCK, &mask, &oldmask) == 0);

	handler_calls = 0;
	handler_on_altstack = 0;
	naddresses = 0;
	signal_caller();

	ATF_REQUIRE(sigprocmask(SIG_SETMASK, &oldmask, NULL) == 0);
	ATF_REQUIRE(sigaction(SIGUSR1, &oldact, NULL) == 0);
	if (use_altstack) {
		ATF_REQUIRE(sigaltstack(&oldss, NULL) == 0);
		free(ss.ss_sp);
	}

	ATF_REQUIRE_EQ(handler_calls, 1);
	ATF_REQUIRE_EQ(handler_on_altstack, use_altstack);
	check_backtrace();
}

ATF_TC_WITHOUT_HEAD(test_backtrace_sigtramp);
ATF_TC_BODY(test_backtrace_sigtramp, tc)
{

	run_sigtramp_test(false);
}

ATF_TC_WITHOUT_HEAD(test_backtrace_sigtramp_altstack);
ATF_TC_BODY(test_backtrace_sigtramp_altstack, tc)
{

	run_sigtramp_test(true);
}

ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, test_backtrace_sigtramp);
	ATF_TP_ADD_TC(tp, test_backtrace_sigtramp_altstack);

	return (atf_no_error());
}
