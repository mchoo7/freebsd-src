/*-
 * SPDX-License-Identifier: BSD-3-Clause AND BSD-2-Clause
 *
 * Copyright (c) 1982, 1986, 1990, 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 * (c) UNIX System Laboratories, Inc.
 * All or some portions of this file are derived from material licensed
 * to the University of California by American Telephone and Telegraph
 * Co. or Unix System Laboratories, Inc. and are reproduced herein with
 * the permission of UNIX System Laboratories, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Copyright (c) 2002-2007, Jeffrey Roberson <jeff@freebsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
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

#include "opt_hwpmc_hooks.h"
#include "opt_hwt_hooks.h"
#include "opt_sched.h"

#include <sys/systm.h>
#include <sys/kdb.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/ktr.h>
#include <sys/limits.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/resource.h>
#include <sys/resourcevar.h>
#include <sys/runq.h>
#include <sys/sched.h>
#include <sys/sdt.h>
#include <sys/smp.h>
#include <sys/sx.h>
#include <sys/sysctl.h>
#include <sys/sysproto.h>
#include <sys/turnstile.h>
#include <sys/umtxvar.h>
#include <sys/vmmeter.h>
#include <sys/cpuset.h>
#include <sys/sbuf.h>

#ifdef HWPMC_HOOKS
#include <sys/pmckern.h>
#endif

#ifdef HWT_HOOKS
#include <dev/hwt/hwt_hook.h>
#endif

#include <machine/cpu.h>
#include <machine/smp.h>

#define	TS_NAME_LEN (MAXCOMLEN + sizeof(" td ") + sizeof(__XSTRING(UINT_MAX)))
#define	TDQ_NAME_LEN	(sizeof("sched lock ") + sizeof(__XSTRING(MAXCPU)))
#define	TDQ_LOADNAME_LEN	(sizeof("CPU ") + sizeof(__XSTRING(MAXCPU)) - 1 + sizeof(" load"))

/*
 * Thread scheduler specific section.  All fields are protected
 * by the thread lock.
 */
struct td_sched {
	short		ts_flags;	/* TSF_* flags. */
	int		ts_cpu;		/* CPU we are on, or were last on. */
	u_int		ts_rltick;	/* Real last tick, for affinity. */
	u_int		ts_slice;	/* Ticks of slice passed. */
	fixpt_t		ts_pctcpu;	/* %cpu during p_swtime. */
	u_int		ts_estcpu;	/* Estimated cpu utilization. */
	int		ts_cpticks;	/* Ticks of cpu time. */
	int		ts_slptime;	/* Seconds !RUNNING. */
#ifdef KTR
	char		ts_name[TS_NAME_LEN];
#endif
};
/* flags kept in ts_flags */
#define	TSF_BOUND	0x0001		/* Thread can not migrate. */
#define	TSF_XFERABLE	0x0002		/* Thread was added as transferable. */

#define	THREAD_CAN_MIGRATE(td)	((td)->td_pinned == 0)
#define	THREAD_CAN_SCHED(td, cpu)	\
    CPU_ISSET((cpu), &(td)->td_cpuset->cs_mask)

_Static_assert(sizeof(struct thread) + sizeof(struct td_sched) <=
    sizeof(struct thread0_storage),
    "increase struct thread0_storage.t0st_sched size");

/*
 * Estimated CPU utilization computation macros and defines.
 *
 * SCHED_INVERSE_ESTCPU_WEIGHT is only suitable for statclock() frequencies in
 * the range 100-256 Hz (approximately).
 */
#define	SCHED_INVERSE_ESTCPU_WEIGHT	8	/* 1 / (priorities per estcpu level). */
#define	SCHED_NICE_WEIGHT		1	/* Priorities per nice level. */
_Static_assert(SCHED_NICE_WEIGHT * (PRIO_MAX - PRIO_MIN)
    <= PRI_MAX_TIMESHARE - PRI_MIN_TIMESHARE,
    "nice weight * nice range should be less than or equal to timeshare range");
#define	SCHED_ESTCPULIM(e)							\
	min((e), SCHED_INVERSE_ESTCPU_WEIGHT *				\
	    (PRI_MAX_TIMESHARE - PRI_MIN_TIMESHARE -			\
	    (PRIO_MAX - PRIO_MIN) * SCHED_NICE_WEIGHT)			\
	    + SCHED_INVERSE_ESTCPU_WEIGHT - 1)

/*
 * Constants for digital decay and forget:
 *	90% of (ts_estcpu) usage in 5 * loadav time
 *	95% of (ts_pctcpu) usage in 60 seconds (load insensitive)
 *          Note that, as ps(1) mentions, this can let percentages
 *          total over 100% (I've seen 137.9% for 3 processes).
 *
 * Note that sched_4bsd_clock() updates ts_estcpu and p_cpticks asynchronously.
 *
 * We wish to decay away 90% of ts_estcpu in (5 * loadavg) seconds.
 * That is, the system wants to compute a value of decay such
 * that the following for loop:
 * 	for (i = 0; i < (5 * loadavg); i++)
 * 		ts_estcpu *= decay;
 * will compute
 * 	ts_estcpu *= 0.1;
 * for all values of loadavg:
 *
 * Mathematically this loop can be expressed by saying:
 * 	decay ** (5 * loadavg) ~= .1
 *
 * The system computes decay as:
 * 	decay = (2 * loadavg) / (2 * loadavg + 1)
 *
 * We wish to prove that the system's computation of decay
 * will always fulfill the equation:
 * 	decay ** (5 * loadavg) ~= .1
 *
 * If we compute b as:
 * 	b = 2 * loadavg
 * then
 * 	decay = b / (b + 1)
 *
 * We now need to prove two things:
 *	1) Given factor ** (5 * loadavg) ~= .1, prove factor == b/(b+1)
 *	2) Given b/(b+1) ** power ~= .1, prove power == (5 * loadavg)
 *
 * Facts:
 *         For x close to zero, exp(x) =~ 1 + x, since
 *              exp(x) = 0! + x**1/1! + x**2/2! + ... .
 *              therefore exp(-1/b) =~ 1 - (1/b) = (b-1)/b.
 *         For x close to zero, ln(1+x) =~ x, since
 *              ln(1+x) = x - x**2/2 + x**3/3 - ...     -1 < x < 1
 *              therefore ln(b/(b+1)) = ln(1 - 1/(b+1)) =~ -1/(b+1).
 *         ln(.1) =~ -2.30
 *
 * Proof of (1):
 *    Solve (factor)**(power) =~ .1 given power (5*loadav):
 *	solving for factor,
 *      ln(factor) =~ (-2.30/5*loadav), or
 *      factor =~ exp(-1/((5/2.30)*loadav)) =~ exp(-1/(2*loadav)) =
 *          exp(-1/b) =~ (b-1)/b =~ b/(b+1).                    QED
 *
 * Proof of (2):
 *    Solve (factor)**(power) =~ .1 given factor == (b/(b+1)):
 *	solving for power,
 *      power*ln(b/(b+1)) =~ -2.30, or
 *      power =~ 2.3 * (b + 1) = 4.6*loadav + 2.3 =~ 5*loadav.  QED
 *
 * Actual power values for the implemented algorithm are as follows:
 *      loadav: 1       2       3       4
 *      power:  5.68    10.32   14.94   19.55
 */

/* calculations for digital decay to forget 90% of usage in 5*loadav sec */
#define	loadfactor(loadav)	(2 * (loadav))
#define	decay_cpu(loadfac, cpu)	(((loadfac) * (cpu)) / ((loadfac) + FSCALE))

extern fixpt_t ccpu;

/*
 * If `ccpu' is not equal to `exp(-1/20)' and you still want to use the
 * faster/more-accurate formula, you'll have to estimate CCPU_SHIFT below
 * and possibly adjust FSHIFT in "param.h" so that (FSHIFT >= CCPU_SHIFT).
 *
 * To estimate CCPU_SHIFT for exp(-1/20), the following formula was used:
 *	1 - exp(-1/20) ~= 0.0487 ~= 0.0488 == 1 (fixed pt, *11* bits).
 *
 * If you don't want to bother with the faster/more-accurate formula, you
 * can set CCPU_SHIFT to (FSHIFT + 1) which will use a slower/less-accurate
 * (more general) method of calculating the %age of CPU used by a process.
 */
#define	CCPU_SHIFT	11

/*
 * These parameters determine the slice behavior.
 */
#define	SCHED_SLICE_DEFAULT_DIVISOR	10	/* ~94 ms, 12 stathz ticks. */
#define	SCHED_SLICE_MIN_DIVISOR		6	/* DEFAULT/MIN = ~16 ms. */

/* Flags kept in td_flags. */
#define	TDF_PICKCPU	TDF_SCHED0	/* Thread should pick new CPU. */
#define TDF_DIDRUN	TDF_SCHED1	/* thread actually ran. */
#define	TDF_SLICEEND	TDF_SCHED2	/* Thread time slice is over. */

/*
 * realstathz:		stathz is sometimes 0 and run off of hz.
 * sched_slice:		Thread run time before rescheduling.
 * sched_slice_min:	Minimum thread run time regardless of tdq_sysload.
 */
static int __read_mostly realstathz = 127;	/* reset during boot. */
static int __read_mostly sched_slice = 12;	/* reset during boot. */
static int __read_mostly sched_slice_min = 1;	/* reset during boot. */
static int __read_mostly sched_idlespins = 10000;
static int __read_mostly sched_idlespinthresh = -1;

/*
 * tdq - per processor runqs and statistics.  A mutex synchronizes access to
 * most fields.  Some fields are loaded or modified without the mutex.
 *
 * Locking protocols:
 * (c)  constant after initialization
 * (f)  flag, set with the tdq lock held, cleared on local CPU
 * (l)  all accesses are CPU-local
 * (ls) stores are performed by the local CPU, loads may be lockless
 * (t)  all accesses are protected by the tdq mutex
 * (ts) stores are serialized by the tdq mutex, loads may be lockless
 */
struct tdq {
	/*
	 * Ordered to improve efficiency of cpu_search() and switch().
	 * tdq_lock is padded to avoid false sharing with tdq_load and
	 * tdq_cpu_idle.
	 */
	struct mtx_padalign tdq_lock;	/* run queue lock. */
	struct cpu_group *tdq_cg;	/* (c) Pointer to cpu topology. */
	struct thread	*tdq_curthread;	/* (t) Current executing thread. */
	int		tdq_load;	/* (ts) Aggregate load. */
	int		tdq_sysload;	/* (ts) For loadavg, !ITHD load. */
	int		tdq_cpu_idle;	/* (ls) cpu_idle() is active. */
	int		tdq_transferable; /* (ts) Transferable thread count. */
	short		tdq_switchcnt;	/* (l) Switches this tick. */
	short		tdq_oldswitchcnt; /* (l) Switches last tick. */
	u_char		tdq_lowpri;	/* (ts) Lowest priority thread. */
	u_char		tdq_owepreempt;	/* (f) Remote preemption pending. */
	/*
	 * (t) Number of (stathz) ticks since last offset incrementation
	 * correction.
	 */
	int		tdq_id;		/* (c) cpuid. */
	struct runq	tdq_runq;	/* (t) Run queue. */
	char		tdq_name[TDQ_NAME_LEN];
#ifdef KTR
	char		tdq_loadname[TDQ_LOADNAME_LEN];
#endif
};

/* Idle thread states and config. */
#define	TDQ_RUNNING	1
#define	TDQ_IDLE	2

/* Lockless accessors. */
#define	TDQ_LOAD(tdq)		atomic_load_int(&(tdq)->tdq_load)
#define	TDQ_TRANSFERABLE(tdq)	atomic_load_int(&(tdq)->tdq_transferable)
#define	TDQ_SWITCHCNT(tdq)	(atomic_load_short(&(tdq)->tdq_switchcnt) + \
				 atomic_load_short(&(tdq)->tdq_oldswitchcnt))
#define	TDQ_SWITCHCNT_INC(tdq)	(atomic_store_short(&(tdq)->tdq_switchcnt, \
				 atomic_load_short(&(tdq)->tdq_switchcnt) + 1))

#ifdef SMP

#define	SCHED_AFFINITY_DEFAULT	(max(1, hz / 1000))
/*
 * This inequality has to be written with a positive difference of ticks to
 * correctly handle wraparound.
 */
#define	SCHED_AFFINITY(ts, t)	((u_int)ticks - (ts)->ts_rltick < (t) * affinity)

/*
 * Run-time tunables.
 */
static int rebalance = 1;
static int balance_interval = 128;	/* Default set in sched_initticks(). */
static int __read_mostly affinity;
static int __read_mostly steal_idle = 1;
static int __read_mostly steal_thresh = 2;
static int __read_mostly always_steal = 0;
static int __read_mostly trysteal_limit = 2;

/*
 * One thread queue per processor.
 */
static struct tdq __read_mostly *balance_tdq;
static int balance_ticks;
DPCPU_DEFINE_STATIC(struct tdq, tdq);
DPCPU_DEFINE_STATIC(uint32_t, randomval);

#define	TDQ_SELF()	((struct tdq *)PCPU_GET(sched))
#define	TDQ_CPU(x)	(DPCPU_ID_PTR((x), tdq))
#define	TDQ_ID(x)	((x)->tdq_id)
#else	/* !SMP */
static struct tdq	tdq_cpu;

#define	TDQ_ID(x)	(0)
#define	TDQ_SELF()	(&tdq_cpu)
#define	TDQ_CPU(x)	(&tdq_cpu)
#endif

#define	TDQ_LOCK_ASSERT(t, type)	mtx_assert(TDQ_LOCKPTR((t)), (type))
#define	TDQ_LOCK(t)		mtx_lock_spin(TDQ_LOCKPTR((t)))
#define	TDQ_LOCK_FLAGS(t, f)	mtx_lock_spin_flags(TDQ_LOCKPTR((t)), (f))
#define	TDQ_TRYLOCK(t)		mtx_trylock_spin(TDQ_LOCKPTR((t)))
#define	TDQ_TRYLOCK_FLAGS(t, f)	mtx_trylock_spin_flags(TDQ_LOCKPTR((t)), (f))
#define	TDQ_UNLOCK(t)		mtx_unlock_spin(TDQ_LOCKPTR((t)))
#define	TDQ_LOCKPTR(t)		((struct mtx *)(&(t)->tdq_lock))

static void sched_update(void);
static void sched_update_estcpu(struct thread *);
static void sched_setpreempt(int);
static void sched_priority(struct thread *);
static void sched_user_priority(struct thread *);
static void sched_thread_priority(struct thread *, u_char);

/* Operations on per processor queues */
static struct thread *tdq_choose(struct tdq *);

static void tdq_setup(struct tdq *, int i);
static void tdq_load_add(struct tdq *, struct thread *);
static void tdq_load_rem(struct tdq *, struct thread *);
static inline void tdq_runq_add(struct tdq *, struct thread *, int);
static inline void tdq_runq_rem(struct tdq *, struct thread *);
static inline int sched_shouldpreempt(int, int);
static void tdq_print(int cpu);
static void runq_print(struct runq *rq);
static int tdq_add(struct tdq *, struct thread *, int);
#ifdef SMP
static int tdq_move(struct tdq *, struct tdq *);
static int tdq_idled(struct tdq *);
static void tdq_notify(struct tdq *, int lowpri);

static bool runq_steal_pred(const int idx, struct rq_queue *const q,
    void *const data);
static inline struct thread *runq_steal(struct runq *const rq, int cpu);
static struct thread *tdq_steal(struct tdq *, int);

static int sched_pickcpu(struct thread *, int);
static void sched_balance(void);
static bool sched_balance_pair(struct tdq *, struct tdq *);
static inline struct tdq *sched_setcpu(struct thread *, int, int);
static inline void thread_unblock_switch(struct thread *, struct mtx *);
#endif

/*
 * Print the threads waiting on a run-queue.
 */
static void
runq_print(struct runq *rq)
{
	struct rq_queue *rqq;
	struct thread *td;
	int pri;
	int j;
	int i;

	for (i = 0; i < RQSW_NB; i++) {
		printf("\t\trunq bits %d %#lx\n",
		    i, rq->rq_status.rq_sw[i]);
		for (j = 0; j < RQSW_BPW; j++)
			if (rq->rq_status.rq_sw[i] & (1ul << j)) {
				pri = RQSW_TO_QUEUE_IDX(i, j);
				rqq = &rq->rq_queues[pri];
				TAILQ_FOREACH(td, rqq, td_runq) {
					printf("\t\t\ttd %p(%s) priority %d rqindex %d pri %d\n",
					    td, td->td_name, td->td_priority,
					    td->td_rqindex, pri);
				}
			}
	}
}

/*
 * Print the status of a per-cpu thread queue.  Should be a ddb show cmd.
 */
static void __unused
tdq_print(int cpu)
{
	struct tdq *tdq;

	tdq = TDQ_CPU(cpu);

	printf("tdq %d:\n", TDQ_ID(tdq));
	printf("\tlock               %p\n", TDQ_LOCKPTR(tdq));
	printf("\tLock name:         %s\n", tdq->tdq_name);
	printf("\tload:              %d\n", tdq->tdq_load);
	printf("\tswitch cnt:        %d\n", tdq->tdq_switchcnt);
	printf("\told switch cnt:    %d\n", tdq->tdq_oldswitchcnt);
	printf("\tload transferable: %d\n", tdq->tdq_transferable);
	printf("\tlowest priority:   %d\n", tdq->tdq_lowpri);
	printf("\trunq:\n");
	runq_print(&tdq->tdq_runq);
}

static inline int
sched_shouldpreempt(int pri, int cpri)
{
#ifdef PREEMPTION
	/*
	 * If the new priority is not better than the current priority there is
	 * nothing to do.
	 */
	if (pri >= cpri)
		return (0);
#ifndef FULL_PREEMPTION
	/*
	 *  - If the new thread's priority is not a interrupt priority and
	 *    the current thread's priority is not an idle priority and
	 *    FULL_PREEMPTION is disabled.
	 */
	if (pri > PRI_MAX_ITHD && cpri < PRI_MIN_IDLE)
		return (0);
#endif

	CTR0(KTR_PROC, "sched_shouldpreempt: scheduling preemption");
	return (1);
#else
	return (0);
#endif
}

/*
 * Add a thread to the actual run-queue.  Keeps transferable counts up to
 * date with what is actually on the run-queue.
 */
static inline void
tdq_runq_add(struct tdq *tdq, struct thread *td, int flags)
{
	struct td_sched *ts;

	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	THREAD_LOCK_BLOCKED_ASSERT(td, MA_OWNED);

	ts = td_get_sched(td);
	TD_SET_RUNQ(td);
	if (THREAD_CAN_MIGRATE(td)) {
		tdq->tdq_transferable++;
		ts->ts_flags |= TSF_XFERABLE;
	}
	runq_add(&tdq->tdq_runq, td, flags);
}

/*
 * Remove a thread from a run-queue.  This typically happens when a thread
 * is selected to run.  Running threads are not on the queue and the
 * transferable count does not reflect them.
 */
static inline void
tdq_runq_rem(struct tdq *tdq, struct thread *td)
{
	struct td_sched *ts;

	ts = td_get_sched(td);
	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	THREAD_LOCK_BLOCKED_ASSERT(td, MA_OWNED);
	if (ts->ts_flags & TSF_XFERABLE) {
		tdq->tdq_transferable--;
		ts->ts_flags &= ~TSF_XFERABLE;
	}
	runq_remove(&tdq->tdq_runq, td);
}

/*
 * Load is maintained for all threads RUNNING and ON_RUNQ.  Add the load
 * for this thread to the referenced thread queue.
 */
static void
tdq_load_add(struct tdq *tdq, struct thread *td)
{

	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	THREAD_LOCK_BLOCKED_ASSERT(td, MA_OWNED);

	tdq->tdq_load++;
	if ((td->td_flags & TDF_NOLOAD) == 0)
		tdq->tdq_sysload++;
	KTR_COUNTER0(KTR_SCHED, "load", tdq->tdq_loadname, tdq->tdq_load);
	SDT_PROBE2(sched, , , load__change, (int)TDQ_ID(tdq), tdq->tdq_load);
}

/*
 * Remove the load from a thread that is transitioning to a sleep state or
 * exiting.
 */
static void
tdq_load_rem(struct tdq *tdq, struct thread *td)
{

	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	THREAD_LOCK_BLOCKED_ASSERT(td, MA_OWNED);
	KASSERT(tdq->tdq_load != 0,
	    ("tdq_load_rem: Removing with 0 load on queue %d", TDQ_ID(tdq)));

	tdq->tdq_load--;
	if ((td->td_flags & TDF_NOLOAD) == 0)
		tdq->tdq_sysload--;
	KTR_COUNTER0(KTR_SCHED, "load", tdq->tdq_loadname, tdq->tdq_load);
	SDT_PROBE2(sched, , , load__change, (int)TDQ_ID(tdq), tdq->tdq_load);
}

/*
 * Bound timeshare latency by decreasing slice size as load increases.  We
 * consider the maximum latency as the sum of the threads waiting to run
 * aside from curthread and target no more than sched_slice latency but
 * no less than sched_slice_min runtime.
 */
static inline u_int
tdq_slice(struct tdq *tdq)
{
	int load;

	/*
	 * It is safe to use sys_load here because this is called from
	 * contexts where timeshare threads are running and so there
	 * cannot be higher priority load in the system.
	 */
	load = tdq->tdq_sysload - 1;
	if (load <= 1)
		return (sched_slice);
	return (imax(sched_slice_min, sched_slice / load));
}

/*
 * Set lowpri to its exact value by searching the run-queue and
 * evaluating curthread.  curthread may be passed as an optimization.
 */
static void
tdq_setlowpri(struct tdq *tdq, struct thread *ctd)
{
	struct thread *td;

	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	if (ctd == NULL)
		ctd = tdq->tdq_curthread;
	td = tdq_choose(tdq);
	if (td == NULL || td->td_priority > ctd->td_priority)
		tdq->tdq_lowpri = ctd->td_priority;
	else
		tdq->tdq_lowpri = td->td_priority;
}

#ifdef SMP
/*
 * We need some randomness. Implement a classic Linear Congruential
 * Generator X_{n+1}=(aX_n+c) mod m. These values are optimized for
 * m = 2^32, a = 69069 and c = 5. We only return the upper 16 bits
 * of the random state (in the low bits of our answer) to keep
 * the maximum randomness.
 */
static uint32_t
sched_random(void)
{
	uint32_t *rndptr;

	rndptr = DPCPU_PTR(randomval);
	*rndptr = *rndptr * 69069 + 5;

	return (*rndptr >> 16);
}

struct cpu_search {
	cpuset_t *cs_mask;	/* The mask of allowed CPUs to choose from. */
	int	cs_prefer;	/* Prefer this CPU and groups including it. */
	int	cs_running;	/* The thread is now running at cs_prefer. */
	int	cs_pri;		/* Min priority for low. */
	int	cs_load;	/* Max load for low, min load for high. */
	int	cs_trans;	/* Min transferable load for high. */
};

struct cpu_search_res {
	int	csr_cpu;	/* The best CPU found. */
	int	csr_load;	/* The load of cs_cpu. */
};

/*
 * Search the tree of cpu_groups for the lowest or highest loaded CPU.
 * These routines actually compare the load on all paths through the tree
 * and find the least loaded cpu on the least loaded path, which may differ
 * from the least loaded cpu in the system.  This balances work among caches
 * and buses.
 */
static int
cpu_search_lowest(const struct cpu_group *cg, const struct cpu_search *s,
    struct cpu_search_res *r)
{
	struct cpu_search_res lr;
	struct tdq *tdq;
	int c, bload, l, load, p, total;

	total = 0;
	bload = INT_MAX;
	r->csr_cpu = -1;

	/* Loop through children CPU groups if there are any. */
	if (cg->cg_children > 0) {
		for (c = cg->cg_children - 1; c >= 0; c--) {
			load = cpu_search_lowest(&cg->cg_child[c], s, &lr);
			total += load;

			/*
			 * When balancing do not prefer SMT groups with load >1.
			 * It allows round-robin between SMT groups with equal
			 * load within parent group for more fair scheduling.
			 */
			if (__predict_false(s->cs_running) &&
			    (cg->cg_child[c].cg_flags & CG_FLAG_THREAD) &&
			    load >= 128 && (load & 128) != 0)
				load += 128;

			if (lr.csr_cpu >= 0 && (load < bload ||
			    (load == bload && lr.csr_load < r->csr_load))) {
				bload = load;
				r->csr_cpu = lr.csr_cpu;
				r->csr_load = lr.csr_load;
			}
		}
		return (total);
	}

	/* Loop through children CPUs otherwise. */
	for (c = cg->cg_last; c >= cg->cg_first; c--) {
		if (!CPU_ISSET(c, &cg->cg_mask))
			continue;
		tdq = TDQ_CPU(c);
		l = TDQ_LOAD(tdq);
		if (c == s->cs_prefer) {
			if (__predict_false(s->cs_running))
				l--;
			p = 128;
		} else
			p = 0;
		load = l * 256;
		total += load - p;

		/*
		 * Check this CPU is acceptable.
		 * If the threads is already on the CPU, don't look on the TDQ
		 * priority, since it can be the priority of the thread itself.
		 */
		if (l > s->cs_load ||
		    (atomic_load_char(&tdq->tdq_lowpri) <= s->cs_pri &&
		     (!s->cs_running || c != s->cs_prefer)) ||
		    !CPU_ISSET(c, s->cs_mask))
			continue;

		/*
		 * When balancing do not prefer CPUs with load > 1.
		 * It allows round-robin between CPUs with equal load
		 * within the CPU group for more fair scheduling.
		 */
		if (__predict_false(s->cs_running) && l > 0)
			p = 0;

		load -= sched_random() % 128;
		if (bload > load - p) {
			bload = load - p;
			r->csr_cpu = c;
			r->csr_load = load;
		}
	}
	return (total);
}

static int
cpu_search_highest(const struct cpu_group *cg, const struct cpu_search *s,
    struct cpu_search_res *r)
{
	struct cpu_search_res lr;
	struct tdq *tdq;
	int c, bload, l, load, total;

	total = 0;
	bload = INT_MIN;
	r->csr_cpu = -1;

	/* Loop through children CPU groups if there are any. */
	if (cg->cg_children > 0) {
		for (c = cg->cg_children - 1; c >= 0; c--) {
			load = cpu_search_highest(&cg->cg_child[c], s, &lr);
			total += load;
			if (lr.csr_cpu >= 0 && (load > bload ||
			    (load == bload && lr.csr_load > r->csr_load))) {
				bload = load;
				r->csr_cpu = lr.csr_cpu;
				r->csr_load = lr.csr_load;
			}
		}
		return (total);
	}

	/* Loop through children CPUs otherwise. */
	for (c = cg->cg_last; c >= cg->cg_first; c--) {
		if (!CPU_ISSET(c, &cg->cg_mask))
			continue;
		tdq = TDQ_CPU(c);
		l = TDQ_LOAD(tdq);
		load = l * 256;
		total += load;

		/*
		 * Check this CPU is acceptable.
		 */
		if (l < s->cs_load || TDQ_TRANSFERABLE(tdq) < s->cs_trans ||
		    !CPU_ISSET(c, s->cs_mask))
			continue;

		load -= sched_random() % 256;
		if (load > bload) {
			bload = load;
			r->csr_cpu = c;
		}
	}
	r->csr_load = bload;
	return (total);
}

/*
 * Find the cpu with the least load via the least loaded path that has a
 * lowpri greater than pri.  A pri of -1 indicates any priority is
 * acceptable.
 */
static inline int
sched_lowest(const struct cpu_group *cg, cpuset_t *mask, int pri, int maxload,
    int prefer, int running)
{
	struct cpu_search s;
	struct cpu_search_res r;

	s.cs_prefer = prefer;
	s.cs_running = running;
	s.cs_mask = mask;
	s.cs_pri = pri;
	s.cs_load = maxload;
	cpu_search_lowest(cg, &s, &r);
	return (r.csr_cpu);
}

/*
 * Find the cpu with the highest load via the highest loaded path.
 */
static inline int
sched_highest(const struct cpu_group *cg, cpuset_t *mask, int minload,
    int mintrans)
{
	struct cpu_search s;
	struct cpu_search_res r;

	s.cs_mask = mask;
	s.cs_load = minload;
	s.cs_trans = mintrans;
	cpu_search_highest(cg, &s, &r);
	return (r.csr_cpu);
}

static void
sched_balance_group(struct cpu_group *cg)
{
	struct tdq *tdq;
	struct thread *td;
	cpuset_t hmask, lmask;
	int high, low, anylow;

	CPU_FILL(&hmask);
	for (;;) {
		high = sched_highest(cg, &hmask, 1, 0);
		/* Stop if there is no more CPU with transferrable threads. */
		if (high == -1)
			break;
		CPU_CLR(high, &hmask);
		CPU_COPY(&hmask, &lmask);
		/* Stop if there is no more CPU left for low. */
		if (CPU_EMPTY(&lmask))
			break;
		tdq = TDQ_CPU(high);
		if (TDQ_LOAD(tdq) == 1) {
			/*
			 * There is only one running thread.  We can't move
			 * it from here, so tell it to pick new CPU by itself.
			 */
			TDQ_LOCK(tdq);
			td = tdq->tdq_curthread;
			if (td->td_lock == TDQ_LOCKPTR(tdq) &&
			    (td->td_flags & TDF_IDLETD) == 0 &&
			    THREAD_CAN_MIGRATE(td)) {
				td->td_flags |= TDF_PICKCPU;
				ast_sched_locked(td, TDA_SCHED);
				if (high != curcpu)
					ipi_cpu(high, IPI_AST);
			}
			TDQ_UNLOCK(tdq);
			break;
		}
		anylow = 1;
nextlow:
		if (TDQ_TRANSFERABLE(tdq) == 0)
			continue;
		low = sched_lowest(cg, &lmask, -1, TDQ_LOAD(tdq) - 1, high, 1);
		/* Stop if we looked well and found no less loaded CPU. */
		if (anylow && low == -1)
			break;
		/* Go to next high if we found no less loaded CPU. */
		if (low == -1)
			continue;
		/* Transfer thread from high to low. */
		if (sched_balance_pair(tdq, TDQ_CPU(low))) {
			/* CPU that got thread can no longer be a donor. */
			CPU_CLR(low, &hmask);
		} else {
			/*
			 * If failed, then there is no threads on high
			 * that can run on this low. Drop low from low
			 * mask and look for different one.
			 */
			CPU_CLR(low, &lmask);
			anylow = 0;
			goto nextlow;
		}
	}
}

static void
sched_balance(void)
{
	struct tdq *tdq;

	balance_ticks = max(balance_interval / 2, 1) +
	    (sched_random() % balance_interval);
	tdq = TDQ_SELF();
	TDQ_UNLOCK(tdq);
	sched_balance_group(cpu_top);
	TDQ_LOCK(tdq);
}

/*
 * Lock two thread queues using their address to maintain lock order.
 */
static void
tdq_lock_pair(struct tdq *one, struct tdq *two)
{
	if (one < two) {
		TDQ_LOCK(one);
		TDQ_LOCK_FLAGS(two, MTX_DUPOK);
	} else {
		TDQ_LOCK(two);
		TDQ_LOCK_FLAGS(one, MTX_DUPOK);
	}
}

/*
 * Unlock two thread queues.  Order is not important here.
 */
static void
tdq_unlock_pair(struct tdq *one, struct tdq *two)
{
	TDQ_UNLOCK(one);
	TDQ_UNLOCK(two);
}

/*
 * Transfer load between two imbalanced thread queues.  Returns true if a thread
 * was moved between the queues, and false otherwise.
 */
static bool
sched_balance_pair(struct tdq *high, struct tdq *low)
{
	int cpu, lowpri;
	bool ret;

	ret = false;
	tdq_lock_pair(high, low);

	/*
	 * Transfer a thread from high to low.
	 */
	if (high->tdq_transferable != 0 && high->tdq_load > low->tdq_load) {
		lowpri = tdq_move(high, low);
		if (lowpri != -1) {
			/*
			 * In case the target isn't the current CPU notify it of
			 * the new load, possibly sending an IPI to force it to
			 * reschedule.  Otherwise maybe schedule a preemption.
			 */
			cpu = TDQ_ID(low);
			if (cpu != PCPU_GET(cpuid))
				tdq_notify(low, lowpri);
			else
				sched_setpreempt(low->tdq_lowpri);
			ret = true;
		}
	}
	tdq_unlock_pair(high, low);
	return (ret);
}

/*
 * Move a thread from one thread queue to another.  Returns -1 if the source
 * queue was empty, else returns the maximum priority of all threads in
 * the destination queue prior to the addition of the new thread.  In the latter
 * case, this priority can be used to determine whether an IPI needs to be
 * delivered.
 */
static int
tdq_move(struct tdq *from, struct tdq *to)
{
	struct thread *td;
	int cpu;

	TDQ_LOCK_ASSERT(from, MA_OWNED);
	TDQ_LOCK_ASSERT(to, MA_OWNED);

	cpu = TDQ_ID(to);
	td = tdq_steal(from, cpu);
	if (td == NULL)
		return (-1);

	/*
	 * Although the run queue is locked the thread may be
	 * blocked.  We can not set the lock until it is unblocked.
	 */
	thread_lock_block_wait(td);
	sched_rem(td);
	THREAD_LOCKPTR_ASSERT(td, TDQ_LOCKPTR(from));
	td->td_lock = TDQ_LOCKPTR(to);
	td_get_sched(td)->ts_cpu = cpu;
	return (tdq_add(to, td, SRQ_YIELDING));
}

/*
 * This tdq has idled.  Try to steal a thread from another cpu and switch
 * to it.
 */
static int
tdq_idled(struct tdq *tdq)
{
	struct cpu_group *cg, *parent;
	struct tdq *steal;
	cpuset_t mask;
	int cpu, switchcnt, group;

	if (smp_started == 0 || steal_idle == 0 || tdq->tdq_cg == NULL)
		return (1);
	CPU_FILL(&mask);
	CPU_CLR(PCPU_GET(cpuid), &mask);
restart:
	switchcnt = TDQ_SWITCHCNT(tdq);
	for (cg = tdq->tdq_cg, group = 0; ; ) {
		cpu = sched_highest(cg, &mask, steal_thresh, 1);
		/*
		 * We were assigned a thread but not preempted.  Returning
		 * 0 here will cause our caller to switch to it.
		 */
		if (TDQ_LOAD(tdq))
			return (0);

		/*
		 * We found no CPU to steal from in this group.  Escalate to
		 * the parent and repeat.  But if parent has only two children
		 * groups we can avoid searching this group again by searching
		 * the other one specifically and then escalating two levels.
		 */
		if (cpu == -1) {
			if (group) {
				cg = cg->cg_parent;
				group = 0;
			}
			parent = cg->cg_parent;
			if (parent == NULL)
				return (1);
			if (parent->cg_children == 2) {
				if (cg == &parent->cg_child[0])
					cg = &parent->cg_child[1];
				else
					cg = &parent->cg_child[0];
				group = 1;
			} else
				cg = parent;
			continue;
		}
		steal = TDQ_CPU(cpu);
		/*
		 * The data returned by sched_highest() is stale and
		 * the chosen CPU no longer has an eligible thread.
		 *
		 * Testing this ahead of tdq_lock_pair() only catches
		 * this situation about 20% of the time on an 8 core
		 * 16 thread Ryzen 7, but it still helps performance.
		 */
		if (TDQ_LOAD(steal) < steal_thresh ||
		    TDQ_TRANSFERABLE(steal) == 0)
			goto restart;
		/*
		 * Try to lock both queues. If we are assigned a thread while
		 * waited for the lock, switch to it now instead of stealing.
		 * If we can't get the lock, then somebody likely got there
		 * first so continue searching.
		 */
		TDQ_LOCK(tdq);
		if (tdq->tdq_load > 0) {
			mi_switch(SW_VOL | SWT_IDLE);
			return (0);
		}
		if (TDQ_TRYLOCK_FLAGS(steal, MTX_DUPOK) == 0) {
			TDQ_UNLOCK(tdq);
			CPU_CLR(cpu, &mask);
			continue;
		}
		/*
		 * The data returned by sched_highest() is stale and
		 * the chosen CPU no longer has an eligible thread, or
		 * we were preempted and the CPU loading info may be out
		 * of date.  The latter is rare.  In either case restart
		 * the search.
		 */
		if (TDQ_LOAD(steal) < steal_thresh ||
		    TDQ_TRANSFERABLE(steal) == 0 ||
		    switchcnt != TDQ_SWITCHCNT(tdq)) {
			tdq_unlock_pair(tdq, steal);
			goto restart;
		}
		/*
		 * Steal the thread and switch to it.
		 */
		if (tdq_move(steal, tdq) != -1)
			break;
		/*
		 * We failed to acquire a thread even though it looked
		 * like one was available.  This could be due to affinity
		 * restrictions or for other reasons.  Loop again after
		 * removing this CPU from the set.  The restart logic
		 * above does not restore this CPU to the set due to the
		 * likelyhood of failing here again.
		 */
		CPU_CLR(cpu, &mask);
		tdq_unlock_pair(tdq, steal);
	}
	TDQ_UNLOCK(steal);
	mi_switch(SW_VOL | SWT_IDLE);
	return (0);
}

/*
 * Notify a remote cpu of new work.  Sends an IPI if criteria are met.
 *
 * "lowpri" is the minimum scheduling priority among all threads on
 * the queue prior to the addition of the new thread.
 */
static void
tdq_notify(struct tdq *tdq, int lowpri)
{
	int cpu;

	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	KASSERT(tdq->tdq_lowpri <= lowpri,
	    ("tdq_notify: lowpri %d > tdq_lowpri %d", lowpri, tdq->tdq_lowpri));

	if (tdq->tdq_owepreempt)
		return;

	/*
	 * Check to see if the newly added thread should preempt the one
	 * currently running.
	 */
	if (!sched_shouldpreempt(tdq->tdq_lowpri, lowpri))
		return;

	/*
	 * Make sure that our caller's earlier update to tdq_load is
	 * globally visible before we read tdq_cpu_idle.  Idle thread
	 * accesses both of them without locks, and the order is important.
	 */
	atomic_thread_fence_seq_cst();

	/*
	 * Try to figure out if we can signal the idle thread instead of sending
	 * an IPI.  This check is racy; at worst, we will deliever an IPI
	 * unnecessarily.
	 */
	cpu = TDQ_ID(tdq);
	if (TD_IS_IDLETHREAD(tdq->tdq_curthread) &&
	    (atomic_load_int(&tdq->tdq_cpu_idle) == 0 || cpu_idle_wakeup(cpu)))
		return;

	/*
	 * The run queues have been updated, so any switch on the remote CPU
	 * will satisfy the preemption request.
	 */
	tdq->tdq_owepreempt = 1;
	ipi_cpu(cpu, IPI_PREEMPT);
}

struct runq_steal_pred_data {
	struct thread	*td;
	int		cpu;
};

static bool
runq_steal_pred(const int idx, struct rq_queue *const q, void *const data)
{
	struct runq_steal_pred_data *const d = data;
	struct thread *td;

	TAILQ_FOREACH(td, q, td_runq) {
		if (THREAD_CAN_MIGRATE(td) && THREAD_CAN_SCHED(td, d->cpu)) {
			d->td = td;
			return (true);
		}
	}

	return (false);
}

/*
 * Steals load contained in queues
 */
static inline struct thread *
runq_steal(struct runq *const rq, int cpu)
{
	struct runq_steal_pred_data data = {
		.td = NULL,
		.cpu = cpu,
	};
	int idx;

	idx = runq_findq(rq, PRI_MIN_ITHD, PRI_MAX_IDLE, &runq_steal_pred, &data);
	if (idx != -1) {
		MPASS(data.td != NULL);
		return (data.td);
	}

	MPASS(data.td == NULL);
	return (NULL);
}

/*
 * Attempt to steal a thread in priority order from a thread queue.
 */
static struct thread *
tdq_steal(struct tdq *tdq, int cpu)
{
	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	return (runq_steal(&tdq->tdq_runq, cpu));
}

/*
 * Sets the thread lock and ts_cpu to match the requested cpu.  Unlocks the
 * current lock and returns with the assigned queue locked.
 */
static inline struct tdq *
sched_setcpu(struct thread *td, int cpu, int flags)
{

	struct tdq *tdq;
	struct mtx *mtx;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	tdq = TDQ_CPU(cpu);
	td_get_sched(td)->ts_cpu = cpu;
	/*
	 * If the lock matches just return the queue.
	 */
	if (td->td_lock == TDQ_LOCKPTR(tdq)) {
		KASSERT((flags & SRQ_HOLD) == 0,
		    ("sched_setcpu: Invalid lock for SRQ_HOLD"));
		return (tdq);
	}

	/*
	 * The hard case, migration, we need to block the thread first to
	 * prevent order reversals with other cpus locks.
	 */
	spinlock_enter();
	mtx = thread_lock_block(td);
	if ((flags & SRQ_HOLD) == 0)
		mtx_unlock_spin(mtx);
	TDQ_LOCK(tdq);
	thread_lock_unblock(td, TDQ_LOCKPTR(tdq));
	spinlock_exit();
	return (tdq);
}

SCHED_STAT_DEFINE(pickcpu_intrbind, "Soft interrupt binding");
SCHED_STAT_DEFINE(pickcpu_idle_affinity, "Picked idle cpu based on affinity");
SCHED_STAT_DEFINE(pickcpu_affinity, "Picked cpu based on affinity");
SCHED_STAT_DEFINE(pickcpu_lowest, "Selected lowest load");
SCHED_STAT_DEFINE(pickcpu_local, "Migrated to current cpu");
SCHED_STAT_DEFINE(pickcpu_migration, "Selection may have caused migration");

static int
sched_pickcpu(struct thread *td, int flags)
{
	struct cpu_group *cg, *ccg;
	struct td_sched *ts;
	struct tdq *tdq;
	cpuset_t *mask;
	int cpu, pri, r, self, intr;

	self = PCPU_GET(cpuid);
	ts = td_get_sched(td);
	KASSERT(!CPU_ABSENT(ts->ts_cpu), ("sched_pickcpu: Start scheduler on "
	    "absent CPU %d for thread %s.", ts->ts_cpu, td->td_name));
	if (smp_started == 0)
		return (self);
	/*
	 * Don't migrate a running thread from sched_switch().
	 */
	if ((flags & SRQ_OURSELF) || !THREAD_CAN_MIGRATE(td))
		return (ts->ts_cpu);
	/*
	 * Prefer to run interrupt threads on the processors that generate
	 * the interrupt.
	 */
	if (td->td_priority <= PRI_MAX_ITHD && THREAD_CAN_SCHED(td, self) &&
	    curthread->td_intr_nesting_level) {
		tdq = TDQ_SELF();
		if (tdq->tdq_lowpri >= PRI_MIN_IDLE) {
			SCHED_STAT_INC(pickcpu_idle_affinity);
			return (self);
		}
		ts->ts_cpu = self;
		intr = 1;
		cg = tdq->tdq_cg;
		goto llc;
	} else {
		intr = 0;
		tdq = TDQ_CPU(ts->ts_cpu);
		cg = tdq->tdq_cg;
	}
	/*
	 * If the thread can run on the last cpu and the affinity has not
	 * expired and it is idle, run it there.
	 */
	if (THREAD_CAN_SCHED(td, ts->ts_cpu) &&
	    atomic_load_char(&tdq->tdq_lowpri) >= PRI_MIN_IDLE &&
	    SCHED_AFFINITY(ts, CG_SHARE_L2)) {
		if (cg->cg_flags & CG_FLAG_THREAD) {
			/* Check all SMT threads for being idle. */
			for (cpu = cg->cg_first; cpu <= cg->cg_last; cpu++) {
				pri =
				    atomic_load_char(&TDQ_CPU(cpu)->tdq_lowpri);
				if (CPU_ISSET(cpu, &cg->cg_mask) &&
				    pri < PRI_MIN_IDLE)
					break;
			}
			if (cpu > cg->cg_last) {
				SCHED_STAT_INC(pickcpu_idle_affinity);
				return (ts->ts_cpu);
			}
		} else {
			SCHED_STAT_INC(pickcpu_idle_affinity);
			return (ts->ts_cpu);
		}
	}
llc:
	/*
	 * Search for the last level cache CPU group in the tree.
	 * Skip SMT, identical groups and caches with expired affinity.
	 * Interrupt threads affinity is explicit and never expires.
	 */
	for (ccg = NULL; cg != NULL; cg = cg->cg_parent) {
		if (cg->cg_flags & CG_FLAG_THREAD)
			continue;
		if (cg->cg_children == 1 || cg->cg_count == 1)
			continue;
		if (cg->cg_level == CG_SHARE_NONE ||
		    (!intr && !SCHED_AFFINITY(ts, cg->cg_level)))
			continue;
		ccg = cg;
	}
	/* Found LLC shared by all CPUs, so do a global search. */
	if (ccg == cpu_top)
		ccg = NULL;
	cpu = -1;
	mask = &td->td_cpuset->cs_mask;
	pri = td->td_priority;
	r = TD_IS_RUNNING(td);
	/*
	 * Try hard to keep interrupts within found LLC.  Search the LLC for
	 * the least loaded CPU we can run now.  For NUMA systems it should
	 * be within target domain, and it also reduces scheduling overhead.
	 */
	if (ccg != NULL && intr) {
		cpu = sched_lowest(ccg, mask, pri, INT_MAX, ts->ts_cpu, r);
		if (cpu >= 0)
			SCHED_STAT_INC(pickcpu_intrbind);
	} else
	/* Search the LLC for the least loaded idle CPU we can run now. */
	if (ccg != NULL) {
		cpu = sched_lowest(ccg, mask, max(pri, PRI_MAX_TIMESHARE),
		    INT_MAX, ts->ts_cpu, r);
		if (cpu >= 0)
			SCHED_STAT_INC(pickcpu_affinity);
	}
	/* Search globally for the least loaded CPU we can run now. */
	if (cpu < 0) {
		cpu = sched_lowest(cpu_top, mask, pri, INT_MAX, ts->ts_cpu, r);
		if (cpu >= 0)
			SCHED_STAT_INC(pickcpu_lowest);
	}
	/* Search globally for the least loaded CPU. */
	if (cpu < 0) {
		cpu = sched_lowest(cpu_top, mask, -1, INT_MAX, ts->ts_cpu, r);
		if (cpu >= 0)
			SCHED_STAT_INC(pickcpu_lowest);
	}
	KASSERT(cpu >= 0, ("sched_pickcpu: Failed to find a cpu."));
	KASSERT(!CPU_ABSENT(cpu), ("sched_pickcpu: Picked absent CPU %d.", cpu));
	/*
	 * Compare the lowest loaded cpu to current cpu.
	 */
	tdq = TDQ_CPU(cpu);
	if (THREAD_CAN_SCHED(td, self) && TDQ_SELF()->tdq_lowpri > pri &&
	    atomic_load_char(&tdq->tdq_lowpri) < PRI_MIN_IDLE &&
	    TDQ_LOAD(TDQ_SELF()) <= TDQ_LOAD(tdq) + 1) {
		SCHED_STAT_INC(pickcpu_local);
		cpu = self;
	}
	if (cpu != ts->ts_cpu)
		SCHED_STAT_INC(pickcpu_migration);
	return (cpu);
}
#endif

/*
 * Pick the highest priority task we have and return it.
 */
static struct thread *
tdq_choose(struct tdq *tdq)
{
	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	return (runq_choose(&tdq->tdq_runq));
}

/*
 * Initialize a thread queue.
 */
static void
tdq_setup(struct tdq *tdq, int id)
{

	if (bootverbose)
		printf("4BSD: setup cpu %d\n", id);
	runq_init(&tdq->tdq_runq);
	tdq->tdq_id = id;
	snprintf(tdq->tdq_name, sizeof(tdq->tdq_name),
	    "sched lock %d", (int)TDQ_ID(tdq));
	mtx_init(&tdq->tdq_lock, tdq->tdq_name, "sched lock", MTX_SPIN);
#ifdef KTR
	snprintf(tdq->tdq_loadname, sizeof(tdq->tdq_loadname),
	    "CPU %d load", (int)TDQ_ID(tdq));
#endif
}

#ifdef SMP
static void
sched_setup_smp(void)
{
	struct tdq *tdq;
	int i;

	CPU_FOREACH(i) {
		tdq = DPCPU_ID_PTR(i, tdq);
		tdq_setup(tdq, i);
		tdq->tdq_cg = smp_topo_find(cpu_top, i);
		if (tdq->tdq_cg == NULL)
			panic("Can't find cpu group for %d\n", i);
		DPCPU_ID_SET(i, randomval, i * 69069 + 5);
	}
	PCPU_SET(sched, DPCPU_PTR(tdq));
	balance_tdq = TDQ_SELF();
}
#endif

/*
 * Setup the thread queues and initialize the topology based on MD
 * information.
 */
static void
sched_4bsd_setup(void)
{
	struct tdq *tdq;

	/*
	 * Decay 95% of `ts_pctcpu' in 60 seconds; see CCPU_SHIFT
	 * before changing.
	 */
	ccpu = 0.95122942450071400909 * FSCALE;	/* exp(-1/20) */

#ifdef SMP
	sched_setup_smp();
#else
	tdq_setup(TDQ_SELF(), 0);
#endif
	tdq = TDQ_SELF();

	/* Add thread0's load since it's running. */
	TDQ_LOCK(tdq);
	thread0.td_lock = TDQ_LOCKPTR(tdq);
	tdq_load_add(tdq, &thread0);
	tdq->tdq_curthread = &thread0;
	tdq->tdq_lowpri = thread0.td_priority;
	TDQ_UNLOCK(tdq);
}

/*
 * This routine determines time constants after stathz and hz are setup.
 */
static void
sched_4bsd_initticks(void)
{
	realstathz = stathz ? stathz : hz;
	sched_slice = realstathz / SCHED_SLICE_DEFAULT_DIVISOR;
	sched_slice_min = sched_slice / SCHED_SLICE_MIN_DIVISOR;
	hogticks = imax(1, (2 * hz * sched_slice + realstathz / 2) /
	    realstathz);

#ifdef SMP
	/*
	 * Set the default balance interval now that we know
	 * what realstathz is.
	 */
	balance_interval = realstathz;
	balance_ticks = balance_interval;
	affinity = SCHED_AFFINITY_DEFAULT;
#endif
	if (sched_idlespinthresh < 0)
		sched_idlespinthresh = 2 * max(10000, 6 * hz) / realstathz;
}

/*
 * Recalculate the priority of a process after it has slept for a while.
 * For all load averages >= 1 and max ts_estcpu of 255, sleeping for at
 * least six times the loadfactor will decay ts_estcpu to zero.
 */
static void
sched_update_estcpu(struct thread *td)
{
	struct td_sched *ts;
	fixpt_t loadfac;
	unsigned int newcpu;

	ts = td_get_sched(td);
	loadfac = loadfactor(averunnable.ldavg[0]);
	if (ts->ts_slptime > 5 * loadfac)
		ts->ts_estcpu = 0;
	else {
		newcpu = ts->ts_estcpu;
		/* first slptime was handled in schedcpu() */
		ts->ts_slptime--;
		while (newcpu && --ts->ts_slptime)
			newcpu = decay_cpu(loadfac, newcpu);
		ts->ts_estcpu = newcpu;
	}
}

/*
 * Compute the priority of a process when running in user mode.
 */
static void
sched_priority(struct thread *td)
{
	u_int newpriority;

	if (td->td_pri_class != PRI_TIMESHARE)
		return;
	newpriority = PRI_MIN_TIMESHARE +
	    td_get_sched(td)->ts_estcpu / SCHED_INVERSE_ESTCPU_WEIGHT +
	    SCHED_NICE_WEIGHT * (td->td_proc->p_nice - PRIO_MIN);
	KASSERT(PRI_MIN_TIMESHARE <= newpriority &&
	    newpriority <= PRI_MAX_TIMESHARE,
	    ("Out-of-bounds priority, probably 'ts_estcpu' not clamped "
	    "correctly, see SCHED_ESTCPULIM()"));
	sched_user_prio(td, newpriority);
}

/*
 * Update the thread's priority when the associated process's user
 * priority changes.
 */
static void
sched_user_priority(struct thread *td)
{
	/* Only change threads with a time sharing user priority. */
	if (td->td_priority < PRI_MIN_TIMESHARE ||
	    td->td_priority > PRI_MAX_TIMESHARE)
		return;

	sched_prio(td, td->td_user_pri);
}

/*
 * Recompute process priorities, every hz ticks.
 * MP-safe, called without the Giant mutex.
 */
static void
sched_update(void)
{
	fixpt_t loadfac = loadfactor(averunnable.ldavg[0]);
	struct thread *td;
	struct proc *p;
	struct td_sched *ts;
	int awake;

	sx_slock(&allproc_lock);
	FOREACH_PROC_IN_SYSTEM(p) {
		PROC_LOCK(p);
		if (p->p_state == PRS_NEW) {
			PROC_UNLOCK(p);
			continue;
		}
		FOREACH_THREAD_IN_PROC(p, td) {
			awake = 0;
			ts = td_get_sched(td);
			thread_lock(td);
			/*
			 * Increment sleep time (if sleeping).  We
			 * ignore overflow, as above.
			 */
			if (TD_IS_IDLETHREAD(td)) {
				awake = 1;
			} else if (TD_ON_RUNQ(td)) {
				awake = 1;
				td->td_flags &= ~TDF_DIDRUN;
			} else if (TD_IS_RUNNING(td)) {
				awake = 1;
				/* Do not clear TDF_DIDRUN */
			} else if (td->td_flags & TDF_DIDRUN) {
				awake = 1;
				td->td_flags &= ~TDF_DIDRUN;
			}

			/*
			 * ts_pctcpu is only for ps and ttyinfo().
			 */
			ts->ts_pctcpu = (ts->ts_pctcpu * ccpu) >> FSHIFT;
			if (ts->ts_cpticks != 0) {
#if	(FSHIFT >= CCPU_SHIFT)
				ts->ts_pctcpu += (realstathz == 100)
				    ? ((fixpt_t) ts->ts_cpticks) <<
				    (FSHIFT - CCPU_SHIFT) :
				    100 * (((fixpt_t) ts->ts_cpticks)
				    << (FSHIFT - CCPU_SHIFT)) / realstathz;
#else
				ts->ts_pctcpu += ((FSCALE - ccpu) *
				    (ts->ts_cpticks *
				    FSCALE / realstathz)) >> FSHIFT;
#endif
				ts->ts_cpticks = 0;
			}

			if (awake) {
				/*
				 * Whoever woke us up from the long sleep
				 * should have unwound the slptime and reset
				 * our priority before we run at the stale
				 * priority.
				 */
				KASSERT(ts->ts_slptime <= 1,
				    ("schedcpu: ts_slptime (%d) not unwound after long sleep for td %p",
				    ts->ts_slptime, td));
				ts->ts_slptime = 0;
			} else
				ts->ts_slptime++;

			/*
			 * If the td_sched has been idle the entire second,
			 * stop recalculating its priority until
			 * it wakes up.
			 */
			if (ts->ts_slptime > 1) {
				thread_unlock(td);
				continue;
			}
			ts->ts_estcpu = decay_cpu(loadfac, ts->ts_estcpu);
			sched_priority(td);
			sched_user_priority(td);
			thread_unlock(td);
		}
		PROC_UNLOCK(p);
	}
	sx_sunlock(&allproc_lock);
}

/*
 * Called from proc0_init() to setup the scheduler fields.
 */
static void
sched_4bsd_init(void)
{
	struct td_sched *ts0;

	/*
	 * Set up the scheduler specific parts of thread0.
	 */
	ts0 = td_get_sched(&thread0);
	ts0->ts_slice = 0;
	ts0->ts_cpu = curcpu;	/* set valid CPU number */
}

/*
 * schedinit_ap() is needed prior to calling sched_throw(NULL) to ensure that
 * the pcpu requirements are met for any calls in the period between curthread
 * initialization and sched_throw().  One can safely add threads to the queue
 * before sched_throw(), for instance, as long as the thread lock is setup
 * correctly.
 *
 * TDQ_SELF() relies on the below sched pcpu setting; it may be used only
 * after schedinit_ap().
 */
static void
sched_4bsd_init_ap(void)
{

#ifdef SMP
	PCPU_SET(sched, DPCPU_PTR(tdq));
#endif
	PCPU_GET(idlethread)->td_lock = TDQ_LOCKPTR(TDQ_SELF());
}

/*
 * This is only somewhat accurate since given many processes of the same
 * priority they will switch when their slices run out, which will be
 * at most sched_slice stathz ticks.
 */
static int
sched_4bsd_rr_interval(void)
{

	/* Convert sched_slice from stathz to hz. */
	return (imax(1, (sched_slice * hz + realstathz / 2) / realstathz));
}

/*
 * Adjust the priority of a thread.  Move it to the appropriate run-queue
 * if necessary.  This is the back-end for several priority related
 * functions.
 */
static void
sched_thread_priority(struct thread *td, u_char prio)
{
	struct tdq *tdq;
	int oldpri;

	KTR_POINT3(KTR_SCHED, "thread", sched_tdname(td), "prio",
	    "prio:%d", td->td_priority, "new prio:%d", prio,
	    KTR_ATTR_LINKED, sched_tdname(curthread));
	SDT_PROBE3(sched, , , change__pri, td, td->td_proc, prio);
	if (td != curthread && prio < td->td_priority) {
		KTR_POINT3(KTR_SCHED, "thread", sched_tdname(curthread),
		    "lend prio", "prio:%d", td->td_priority, "new prio:%d",
		    prio, KTR_ATTR_LINKED, sched_tdname(td));
		SDT_PROBE4(sched, , , lend__pri, td, td->td_proc, prio,
		    curthread);
	}
	THREAD_LOCK_ASSERT(td, MA_OWNED);
	if (td->td_priority == prio)
		return;
	oldpri = td->td_priority;
	/*
	 * An improved priority must be re-added so sched_add() can notify
	 * the owning CPU.  A worsened priority must be re-added when it
	 * belongs to a different run-queue bucket.  Otherwise the thread
	 * would continue to be selected according to its old priority.
	 */
	if (TD_ON_RUNQ(td) && (prio < oldpri ||
	    td->td_rqindex != RQ_PRI_TO_QUEUE_IDX(prio))) {
		sched_rem(td);
		td->td_priority = prio;
		sched_add(td, SRQ_BORING | SRQ_HOLDTD);
		return;
	}
	/*
	 * Keep the exact queue priority up to date when the priority changed
	 * within a single run-queue bucket.
	 */
	if (TD_ON_RUNQ(td)) {
		tdq = TDQ_CPU(td_get_sched(td)->ts_cpu);
		td->td_priority = prio;
		if (prio < tdq->tdq_lowpri)
			tdq->tdq_lowpri = prio;
		else if (tdq->tdq_lowpri == oldpri)
			tdq_setlowpri(tdq, NULL);
		return;
	}
	/*
	 * If the thread is currently running we may have to adjust the lowpri
	 * information so other cpus are aware of our current priority.
	 */
	if (TD_IS_RUNNING(td)) {
		tdq = TDQ_CPU(td_get_sched(td)->ts_cpu);
		td->td_priority = prio;
		if (prio < tdq->tdq_lowpri)
			tdq->tdq_lowpri = prio;
		else if (tdq->tdq_lowpri == oldpri)
			tdq_setlowpri(tdq, td);
		return;
	}
	td->td_priority = prio;
}

/*
 * Update a thread's priority when it is lent another thread's
 * priority.
 */
static void
sched_4bsd_lend_prio(struct thread *td, u_char prio)
{

	td->td_flags |= TDF_BORROWING;
	sched_thread_priority(td, prio);
}

/*
 * Restore a thread's priority when priority propagation is
 * over.  The prio argument is the minimum priority the thread
 * needs to have to satisfy other possible priority lending
 * requests.  If the thread's regular priority is less
 * important than prio, the thread will keep a priority boost
 * of prio.
 */
static void
sched_4bsd_unlend_prio(struct thread *td, u_char prio)
{
	u_char base_pri;

	if (td->td_base_pri >= PRI_MIN_TIMESHARE &&
	    td->td_base_pri <= PRI_MAX_TIMESHARE)
		base_pri = td->td_user_pri;
	else
		base_pri = td->td_base_pri;
	if (prio >= base_pri) {
		td->td_flags &= ~TDF_BORROWING;
		sched_thread_priority(td, base_pri);
	} else
		sched_lend_prio(td, prio);
}

/*
 * Standard entry for setting the priority to an absolute value.
 */
static void
sched_4bsd_prio(struct thread *td, u_char prio)
{
	u_char oldprio;

	/* First, update the base priority. */
	td->td_base_pri = prio;

	/*
	 * If the thread is borrowing another thread's priority, don't
	 * ever lower the priority.
	 */
	if (td->td_flags & TDF_BORROWING && td->td_priority < prio)
		return;

	/* Change the real priority. */
	oldprio = td->td_priority;
	sched_thread_priority(td, prio);

	/*
	 * If the thread is on a turnstile, then let the turnstile update
	 * its state.
	 */
	if (TD_ON_LOCK(td) && oldprio != prio)
		turnstile_adjust(td, oldprio);
}

/*
 * Set the base interrupt thread priority.
 */
static void
sched_4bsd_ithread_prio(struct thread *td, u_char prio)
{
	THREAD_LOCK_ASSERT(td, MA_OWNED);
	MPASS(td->td_pri_class == PRI_ITHD);
	td->td_base_ithread_pri = prio;
	sched_prio(td, prio);
}

/*
 * Set the base user priority, does not effect current running priority.
 */
static void
sched_4bsd_user_prio(struct thread *td, u_char prio)
{

	td->td_base_user_pri = prio;
	if (td->td_lend_user_pri <= prio)
		return;
	td->td_user_pri = prio;
}

static void
sched_4bsd_lend_user_prio(struct thread *td, u_char prio)
{

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	td->td_lend_user_pri = prio;
	td->td_user_pri = min(prio, td->td_base_user_pri);
	if (td->td_priority > td->td_user_pri)
		sched_prio(td, td->td_user_pri);
	else if (td->td_priority != td->td_user_pri)
		ast_sched_locked(td, TDA_SCHED);
}

/*
 * Like the above but first check if there is anything to do.
 */
static void
sched_4bsd_lend_user_prio_cond(struct thread *td, u_char prio)
{

	if (td->td_lend_user_pri == prio)
		return;

	thread_lock(td);
	sched_lend_user_prio(td, prio);
	thread_unlock(td);
}

#ifdef SMP
/*
 * This tdq is about to idle.  Try to steal a thread from another CPU before
 * choosing the idle thread.
 */
static void
tdq_trysteal(struct tdq *tdq)
{
	struct cpu_group *cg, *parent;
	struct tdq *steal;
	cpuset_t mask;
	int cpu, i, group;

	if (smp_started == 0 || steal_idle == 0 || trysteal_limit == 0 ||
	    tdq->tdq_cg == NULL)
		return;
	CPU_FILL(&mask);
	CPU_CLR(PCPU_GET(cpuid), &mask);
	/* We don't want to be preempted while we're iterating. */
	spinlock_enter();
	TDQ_UNLOCK(tdq);
	for (i = 1, cg = tdq->tdq_cg, group = 0; ; ) {
		cpu = sched_highest(cg, &mask, steal_thresh, 1);
		/*
		 * If a thread was added while interrupts were disabled don't
		 * steal one here.
		 */
		if (TDQ_LOAD(tdq) > 0) {
			TDQ_LOCK(tdq);
			break;
		}

		/*
		 * We found no CPU to steal from in this group.  Escalate to
		 * the parent and repeat.  But if parent has only two children
		 * groups we can avoid searching this group again by searching
		 * the other one specifically and then escalating two levels.
		 */
		if (cpu == -1) {
			if (group) {
				cg = cg->cg_parent;
				group = 0;
			}
			if (++i > trysteal_limit) {
				TDQ_LOCK(tdq);
				break;
			}
			parent = cg->cg_parent;
			if (parent == NULL) {
				TDQ_LOCK(tdq);
				break;
			}
			if (parent->cg_children == 2) {
				if (cg == &parent->cg_child[0])
					cg = &parent->cg_child[1];
				else
					cg = &parent->cg_child[0];
				group = 1;
			} else
				cg = parent;
			continue;
		}
		steal = TDQ_CPU(cpu);
		/*
		 * The data returned by sched_highest() is stale and
		 * the chosen CPU no longer has an eligible thread.
		 * At this point unconditionally exit the loop to bound
		 * the time spent in the critcal section.
		 */
		if (TDQ_LOAD(steal) < steal_thresh ||
		    TDQ_TRANSFERABLE(steal) == 0)
			continue;
		/*
		 * Try to lock both queues. If we are assigned a thread while
		 * waited for the lock, switch to it now instead of stealing.
		 * If we can't get the lock, then somebody likely got there
		 * first.
		 */
		TDQ_LOCK(tdq);
		if (tdq->tdq_load > 0)
			break;
		if (TDQ_TRYLOCK_FLAGS(steal, MTX_DUPOK) == 0)
			break;
		/*
		 * The data returned by sched_highest() is stale and
                 * the chosen CPU no longer has an eligible thread.
		 */
		if (TDQ_LOAD(steal) < steal_thresh ||
		    TDQ_TRANSFERABLE(steal) == 0) {
			TDQ_UNLOCK(steal);
			break;
		}
		/*
		 * If we fail to acquire one due to affinity restrictions,
		 * bail out and let the idle thread to a more complete search
		 * outside of a critical section.
		 */
		if (tdq_move(steal, tdq) == -1) {
			TDQ_UNLOCK(steal);
			break;
		}
		TDQ_UNLOCK(steal);
		break;
	}
	spinlock_exit();
}
#endif

/*
 * Handle migration from sched_switch().  This happens only for
 * cpu binding.
 */
static struct mtx *
sched_switch_migrate(struct tdq *tdq, struct thread *td, int flags)
{
	struct tdq *tdn;
#ifdef SMP
	int lowpri;
#endif

	KASSERT(THREAD_CAN_MIGRATE(td) ||
	    (td_get_sched(td)->ts_flags & TSF_BOUND) != 0,
	    ("Thread %p shouldn't migrate", td));
	KASSERT(!CPU_ABSENT(td_get_sched(td)->ts_cpu), ("sched_switch_migrate: "
	    "thread %s queued on absent CPU %d.", td->td_name,
	    td_get_sched(td)->ts_cpu));
	tdn = TDQ_CPU(td_get_sched(td)->ts_cpu);
#ifdef SMP
	tdq_load_rem(tdq, td);
	/*
	 * Do the lock dance required to avoid LOR.  We have an
	 * extra spinlock nesting from sched_switch() which will
	 * prevent preemption while we're holding neither run-queue lock.
	 */
	TDQ_UNLOCK(tdq);
	TDQ_LOCK(tdn);
	lowpri = tdq_add(tdn, td, flags);
	tdq_notify(tdn, lowpri);
	TDQ_UNLOCK(tdn);
	TDQ_LOCK(tdq);
#endif
	return (TDQ_LOCKPTR(tdn));
}

/*
 * thread_lock_unblock() that does not assume td_lock is blocked.
 */
static inline void
thread_unblock_switch(struct thread *td, struct mtx *mtx)
{
	atomic_store_rel_ptr((volatile uintptr_t *)&td->td_lock,
	    (uintptr_t)mtx);
}

/*
 * Switch threads.  This function has to handle threads coming in while
 * blocked for some reason, running, or idle.  It also must deal with
 * migrating a thread from one queue to another as running threads may
 * be assigned elsewhere via binding.
 */
static void
sched_4bsd_sswitch(struct thread *td, int flags)
{
	struct thread *newtd;
	struct tdq *tdq;
	struct td_sched *ts;
	struct mtx *mtx;
	int srqflag;
	int cpuid, preempted;
#ifdef SMP
	int pickcpu;
#endif

	THREAD_LOCK_ASSERT(td, MA_OWNED);

	cpuid = PCPU_GET(cpuid);
	tdq = TDQ_SELF();
	ts = td_get_sched(td);
#ifdef SMP
	pickcpu = (td->td_flags & TDF_PICKCPU) != 0;
	if (pickcpu)
		ts->ts_rltick = (u_int)ticks - affinity * MAX_CACHE_LEVELS;
	else
		ts->ts_rltick = (u_int)ticks;
#endif
	td->td_lastcpu = td->td_oncpu;
	preempted = (td->td_flags & TDF_SLICEEND) == 0 &&
	    (flags & SW_PREEMPT) != 0;
	td->td_flags &= ~(TDF_PICKCPU | TDF_SLICEEND);
	ast_unsched_locked(td, TDA_SCHED);
	td->td_owepreempt = 0;
	atomic_store_char(&tdq->tdq_owepreempt, 0);
	if (!TD_IS_IDLETHREAD(td))
		TDQ_SWITCHCNT_INC(tdq);

	/*
	 * Always block the thread lock so we can drop the tdq lock early.
	 */
	mtx = thread_lock_block(td);
	spinlock_enter();
	if (TD_IS_IDLETHREAD(td)) {
		MPASS(mtx == TDQ_LOCKPTR(tdq));
		TD_SET_CAN_RUN(td);
	} else if (TD_IS_RUNNING(td)) {
		MPASS(mtx == TDQ_LOCKPTR(tdq));
		srqflag = SRQ_OURSELF | SRQ_YIELDING |
		    (preempted ? SRQ_PREEMPTED : 0);
#ifdef SMP
		if (THREAD_CAN_MIGRATE(td) && (!THREAD_CAN_SCHED(td, ts->ts_cpu)
		    || pickcpu))
			ts->ts_cpu = sched_pickcpu(td, 0);
#endif
		if (ts->ts_cpu == cpuid)
			tdq_runq_add(tdq, td, srqflag);
		else
			mtx = sched_switch_migrate(tdq, td, srqflag);
	} else {
		/* This thread must be going to sleep. */
		if (mtx != TDQ_LOCKPTR(tdq)) {
			mtx_unlock_spin(mtx);
			TDQ_LOCK(tdq);
		}
		tdq_load_rem(tdq, td);
#ifdef SMP
		if (tdq->tdq_load == 0)
			tdq_trysteal(tdq);
#endif
	}

#if (KTR_COMPILE & KTR_SCHED) != 0
	if (TD_IS_IDLETHREAD(td))
		KTR_STATE1(KTR_SCHED, "thread", sched_tdname(td), "idle",
		    "prio:%d", td->td_priority);
	else
		KTR_STATE3(KTR_SCHED, "thread", sched_tdname(td), KTDSTATE(td),
		    "prio:%d", td->td_priority, "wmesg:\"%s\"", td->td_wmesg,
		    "lockname:\"%s\"", td->td_lockname);
#endif

	/*
	 * We enter here with the thread blocked and assigned to the
	 * appropriate cpu run-queue or sleep-queue and with the current
	 * thread-queue locked.
	 */
	TDQ_LOCK_ASSERT(tdq, MA_OWNED | MA_NOTRECURSED);
	MPASS(td == tdq->tdq_curthread);
	newtd = choosethread();
	TDQ_UNLOCK(tdq);

	/*
	 * Call the MD code to switch contexts if necessary.
	 */
	if (td != newtd) {
#ifdef	HWPMC_HOOKS
		if (PMC_PROC_IS_USING_PMCS(td->td_proc))
			PMC_SWITCH_CONTEXT(td, PMC_FN_CSW_OUT);
#endif
		SDT_PROBE2(sched, , , off__cpu, newtd, newtd->td_proc);

#ifdef KDTRACE_HOOKS
		/*
		 * If DTrace has set the active vtime enum to anything
		 * other than INACTIVE (0), then it should have set the
		 * function to call.
		 */
		if (dtrace_vtime_active)
			(*dtrace_vtime_switch_func)(newtd);
#endif

#ifdef HWT_HOOKS
		HWT_CALL_HOOK(td, HWT_SWITCH_OUT, NULL);
		HWT_CALL_HOOK(newtd, HWT_SWITCH_IN, NULL);
#endif

		td->td_oncpu = NOCPU;
		cpu_switch(td, newtd, mtx);
		cpuid = td->td_oncpu = PCPU_GET(cpuid);

		SDT_PROBE0(sched, , , on__cpu);
#ifdef	HWPMC_HOOKS
		if (PMC_PROC_IS_USING_PMCS(td->td_proc))
			PMC_SWITCH_CONTEXT(td, PMC_FN_CSW_IN);
#endif
	} else {
		thread_unblock_switch(td, mtx);
		SDT_PROBE0(sched, , , remain__cpu);
	}
	KASSERT(curthread->td_md.md_spinlock_count == 1,
	    ("invalid count %d", curthread->td_md.md_spinlock_count));

	KTR_STATE1(KTR_SCHED, "thread", sched_tdname(td), "running",
	    "prio:%d", td->td_priority);
}

/*
 * Adjust thread priorities as a result of a nice request.
 */
static void
sched_4bsd_nice(struct proc *p, int nice)
{
	struct thread *td;

	PROC_LOCK_ASSERT(p, MA_OWNED);

	p->p_nice = nice;
	FOREACH_THREAD_IN_PROC(p, td) {
		thread_lock(td);
		sched_priority(td);
		sched_user_priority(td);
		thread_unlock(td);
	}
}

/*
 * Record the sleep time in ticks.
 */
static void
sched_4bsd_sleep(struct thread *td, int prio)
{

	THREAD_LOCK_ASSERT(td, MA_OWNED);

	td->td_slptick = ticks;
	td_get_sched(td)->ts_slptime = 0;
	if (PRI_BASE(td->td_pri_class) != PRI_TIMESHARE)
		return;
	if (prio != 0 && PRI_BASE(td->td_pri_class) == PRI_TIMESHARE)
		sched_prio(td, prio);
}

/*
 * Schedule a thread to resume execution and reset its sleep-related
 * varibles and slice. We also update the priority.
 *
 * Requires the thread lock on entry, drops on exit.
 */
static void
sched_4bsd_wakeup(struct thread *td, int srqflags)
{
	struct td_sched *ts;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	ts = td_get_sched(td);
	if (ts->ts_slptime > 1) {
		sched_update_estcpu(td);
		sched_priority(td);
	}
	td->td_slptick = 0;
	ts->ts_slptime = 0;

	/*
	 * When resuming an idle ithread, restore its base ithread
	 * priority.
	 */
	if (PRI_BASE(td->td_pri_class) == PRI_ITHD &&
	    td->td_priority != td->td_base_ithread_pri)
		sched_prio(td, td->td_base_ithread_pri);

	/*
	 * Reset the slice value since we slept and advanced the round-robin.
	 */
	ts->ts_slice = 0;
	sched_add(td, SRQ_BORING | srqflags);
}

/*
 * Penalize the parent for creating a new child and initialize the child's
 * priority.
 */
static void
sched_4bsd_fork(struct thread *td, struct thread *child)
{
	THREAD_LOCK_ASSERT(td, MA_OWNED);
	sched_fork_thread(td, child);
}

/*
 * Fork a new thread, may be within the same process.
 */
static void
sched_4bsd_fork_thread(struct thread *td, struct thread *child)
{
	struct td_sched *ts;
	struct td_sched *ts2;
	struct tdq *tdq;

	tdq = TDQ_SELF();
	THREAD_LOCK_ASSERT(td, MA_OWNED);
	/*
	 * Initialize child.
	 */
	ts = td_get_sched(td);
	ts2 = td_get_sched(child);
	child->td_oncpu = NOCPU;
	child->td_lastcpu = NOCPU;
	child->td_lock = TDQ_LOCKPTR(tdq);
	child->td_cpuset = cpuset_ref(td->td_cpuset);
	child->td_domain.dr_policy = td->td_cpuset->cs_domain;
	ts2->ts_cpu = ts->ts_cpu;
	ts2->ts_flags = 0;
	/*
	 * Do not inherit any borrowed priority from the parent.
	 */
	child->td_priority = child->td_base_pri;
	ts2->ts_estcpu = ts->ts_estcpu;
	/* Give a new thread one tick in its initial slice. */
	ts2->ts_slice = tdq_slice(tdq) - 1;
#ifdef KTR
	bzero(ts2->ts_name, sizeof(ts2->ts_name));
#endif
}

/*
 * Adjust the priority class of a thread.
 */
static void
sched_4bsd_class(struct thread *td, int class)
{

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	if (td->td_pri_class == class)
		return;
	td->td_pri_class = class;
}

/*
 * Return some of the child's priority.
 */
static void
sched_4bsd_exit(struct proc *p, struct thread *child)
{
	struct thread *td;

	KTR_STATE1(KTR_SCHED, "thread", sched_tdname(child), "proc exit",
	    "prio:%d", child->td_priority);
	PROC_LOCK_ASSERT(p, MA_OWNED);
	td = FIRST_THREAD_IN_PROC(p);
	sched_exit_thread(td, child);
}

/*
 * Return child thread's estcpu to its parent.
 */
static void
sched_4bsd_exit_thread(struct thread *td, struct thread *child)
{

	KTR_STATE1(KTR_SCHED, "thread", sched_tdname(child), "thread exit",
	    "prio:%d", child->td_priority);
	thread_lock(td);
	td_get_sched(td)->ts_estcpu = SCHED_ESTCPULIM(td_get_sched(td)->ts_estcpu +
	    td_get_sched(child)->ts_estcpu);
	thread_unlock(td);
}

static void
sched_4bsd_preempt(struct thread *td)
{
	struct tdq *tdq;
	int flags;

	SDT_PROBE2(sched, , , surrender, td, td->td_proc);

	thread_lock(td);
	tdq = TDQ_SELF();
	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	if (td->td_priority > tdq->tdq_lowpri) {
		if (td->td_critnest == 1) {
			flags = SW_INVOL | SW_PREEMPT;
			flags |= TD_IS_IDLETHREAD(td) ? SWT_REMOTEWAKEIDLE :
			    SWT_REMOTEPREEMPT;
			mi_switch(flags);
			/* Switch dropped thread lock. */
			return;
		}
		td->td_owepreempt = 1;
	} else {
		tdq->tdq_owepreempt = 0;
	}
	thread_unlock(td);
}

/*
 * Fix priorities on return to user-space.  Priorities may be elevated due
 * to static priorities in msleep() or similar.
 */
static void
sched_4bsd_userret_slowpath(struct thread *td)
{

	thread_lock(td);
	td->td_priority = td->td_user_pri;
	td->td_base_pri = td->td_user_pri;
	tdq_setlowpri(TDQ_SELF(), td);
	thread_unlock(td);
}

/*
 * Return time slice for a given thread.  For ithreads this is
 * sched_slice.  For other threads it is tdq_slice(tdq).
 */
static inline u_int
td_slice(struct thread *td, struct tdq *tdq)
{
	if (PRI_BASE(td->td_pri_class) == PRI_ITHD)
		return (sched_slice);
	return (tdq_slice(tdq));
}

/*
 * Handle a stathz tick.  This is really only relevant for timeshare
 * and interrupt threads.
 *
 * We adjust the priority of the current process.  The priority of a
 * process gets worse as it accumulates CPU time.  The cpu usage
 * estimator (ts_estcpu) is increased here.  sched_reset_priority() will
 * compute a different priority each time ts_estcpu increases by
 * SCHED_INVERSE_ESTCPU_WEIGHT (until PRI_MAX_TIMESHARE is reached).  The
 * cpu usage estimator ramps up quite quickly when the process is
 * running (linearly), and decays away exponentially, at a rate which
 * is proportionally slower when the system is busy.  The basic
 * principle is that the system will 90% forget that the process used
 * a lot of CPU time in 5 * loadav seconds.  This causes the system to
 * favor processes which haven't run much recently, and to round-robin
 * among other processes.
 */
static void
sched_4bsd_clock(struct thread *td, int cnt)
{
	struct tdq *tdq;
	struct td_sched *ts;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	tdq = TDQ_SELF();
#ifdef SMP
	/*
	 * We run the long term load balancer infrequently on the first cpu.
	 */
	if (balance_tdq == tdq && smp_started != 0 && rebalance != 0 &&
	    balance_ticks != 0) {
		balance_ticks -= cnt;
		if (balance_ticks <= 0)
			sched_balance();
	}
#endif
	/*
	 * Save the old switch count so we have a record of the last ticks
	 * activity.   Initialize the new switch count based on our load.
	 * If there is some activity seed it to reflect that.
	 */
	tdq->tdq_oldswitchcnt = tdq->tdq_switchcnt;
	tdq->tdq_switchcnt = tdq->tdq_load;

	ts = td_get_sched(td);

	ts->ts_cpticks += cnt;
	for (int i = cnt; i > 0; i--) {
		ts->ts_estcpu = SCHED_ESTCPULIM(ts->ts_estcpu + 1);
		if ((ts->ts_estcpu % SCHED_INVERSE_ESTCPU_WEIGHT) == 0) {
			sched_priority(td);
			sched_user_priority(td);
		}
	}

	/*
	 * Force a context switch if the current thread has used up a full
	 * time slice (default is 100ms).
	 */
	ts->ts_slice += cnt;
	if (!TD_IS_IDLETHREAD(td) && ts->ts_slice >= td_slice(td, tdq)) {
		ts->ts_slice = 0;

		/*
		 * If an ithread uses a full quantum, demote its
		 * priority and preempt it.
		 */
		if (PRI_BASE(td->td_pri_class) == PRI_ITHD) {
			SCHED_STAT_INC(ithread_preemptions);
			td->td_owepreempt = 1;
			if (td->td_base_pri + RQ_PPQ < PRI_MAX_ITHD) {
				SCHED_STAT_INC(ithread_demotions);
				sched_prio(td, td->td_base_pri + RQ_PPQ);
			}
		} else {
			td->td_flags |= TDF_SLICEEND;
			ast_sched_locked(td, TDA_SCHED);
		}
	}
}

static u_int
sched_4bsd_estcpu(struct thread *td)
{
	return (td_get_sched(td)->ts_estcpu);
}

/*
 * Return whether the current CPU has runnable tasks.  Used for in-kernel
 * cooperative idle threads.
 */
static bool
sched_4bsd_runnable(void)
{
	struct tdq *tdq;

	tdq = TDQ_SELF();
	return (TDQ_LOAD(tdq) > (TD_IS_IDLETHREAD(curthread) ? 0 : 1));
}

/*
 * Choose the highest priority thread to run.  The thread is removed from
 * the run-queue while running however the load remains.
 */
static struct thread *
sched_4bsd_choose(void)
{
	struct thread *td;
	struct tdq *tdq;

	tdq = TDQ_SELF();
	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	td = tdq_choose(tdq);
	if (td != NULL) {
		tdq_runq_rem(tdq, td);
		tdq->tdq_lowpri = td->td_priority;
		td->td_flags |= TDF_DIDRUN;
	} else {
		tdq->tdq_lowpri = PRI_MAX_IDLE;
		td = PCPU_GET(idlethread);
	}
	tdq->tdq_curthread = td;
	return (td);
}

/*
 * Set owepreempt if the currently running thread has lower priority than "pri".
 * Preemption never happens directly in 4BSD, we always request it once we exit a
 * critical section.
 */
static void
sched_setpreempt(int pri)
{
	struct thread *ctd;
	int cpri;

	ctd = curthread;
	THREAD_LOCK_ASSERT(ctd, MA_OWNED);

	cpri = ctd->td_priority;
	if (pri < cpri)
		ast_sched_locked(ctd, TDA_SCHED);
	if (KERNEL_PANICKED() || pri >= cpri || cold || TD_IS_INHIBITED(ctd))
		return;
	if (!sched_shouldpreempt(pri, cpri))
		return;
	ctd->td_owepreempt = 1;
}

/*
 * Add a thread to a thread queue.  Select the appropriate runq and add the
 * thread to it.  This is the internal function called when the tdq is
 * predetermined.
 */
static int
tdq_add(struct tdq *tdq, struct thread *td, int flags)
{
	int lowpri;

	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	THREAD_LOCK_BLOCKED_ASSERT(td, MA_OWNED);
	KASSERT((td->td_inhibitors == 0),
	    ("sched_add: trying to run inhibited thread"));
	KASSERT((TD_CAN_RUN(td) || TD_IS_RUNNING(td)),
	    ("sched_add: bad thread state"));
	KASSERT(td->td_flags & TDF_INMEM,
	    ("sched_add: thread swapped out"));

	lowpri = tdq->tdq_lowpri;
	if (td->td_priority < lowpri)
		tdq->tdq_lowpri = td->td_priority;
	tdq_runq_add(tdq, td, flags);
	tdq_load_add(tdq, td);
	return (lowpri);
}

/*
 * Select the target thread queue and add a thread to it.  Request
 * preemption or IPI a remote processor if required.
 *
 * Requires the thread lock on entry, drops on exit.
 */
static void
sched_4bsd_add(struct thread *td, int flags)
{
	struct td_sched *ts;
	struct tdq *tdq;
#ifdef SMP
	int cpu, lowpri;
#endif

	ts = td_get_sched(td);
	THREAD_LOCK_ASSERT(td, MA_OWNED);
	KASSERT((td->td_inhibitors == 0),
	    ("sched_add: trying to run inhibited thread"));
	KASSERT((TD_CAN_RUN(td) || TD_IS_RUNNING(td)),
	    ("sched_add: bad thread state"));
	KASSERT(td->td_flags & TDF_INMEM,
	    ("sched_add: thread swapped out"));

	KTR_STATE2(KTR_SCHED, "thread", sched_tdname(td), "runq add",
	    "prio:%d", td->td_priority, KTR_ATTR_LINKED,
	    sched_tdname(curthread));
	KTR_POINT1(KTR_SCHED, "thread", sched_tdname(curthread), "wokeup",
	    KTR_ATTR_LINKED, sched_tdname(td));
	SDT_PROBE4(sched, , , enqueue, td, td->td_proc, NULL, 
	    flags & SRQ_PREEMPTED);
	THREAD_LOCK_ASSERT(td, MA_OWNED);
	/*
	 * Recalculate the priority before we select the target cpu or
	 * run-queue.
	 */
	if (ts->ts_slptime > 1) {
		sched_update_estcpu(td);
		sched_priority(td);
	}
	td->td_slptick = 0;
	ts->ts_slptime = 0;
#ifdef SMP
	/*
	 * Pick the destination cpu and if it isn't ours transfer to the
	 * target cpu.
	 */
	cpu = sched_pickcpu(td, flags);
	tdq = sched_setcpu(td, cpu, flags);
	lowpri = tdq_add(tdq, td, flags);
	if (cpu != PCPU_GET(cpuid))
		tdq_notify(tdq, lowpri);
	else if (!(flags & SRQ_YIELDING))
		sched_setpreempt(td->td_priority);
#else
	tdq = TDQ_SELF();
	/*
	 * Now that the thread is moving to the run-queue, set the lock
	 * to the scheduler's lock.
	 */
	if (td->td_lock != TDQ_LOCKPTR(tdq)) {
		TDQ_LOCK(tdq);
		if ((flags & SRQ_HOLD) != 0)
			td->td_lock = TDQ_LOCKPTR(tdq);
		else
			thread_lock_set(td, TDQ_LOCKPTR(tdq));
	}
	(void)tdq_add(tdq, td, flags);
	if (!(flags & SRQ_YIELDING))
		sched_setpreempt(td->td_priority);
#endif
	if (!(flags & SRQ_HOLDTD))
		thread_unlock(td);
}

/*
 * Remove a thread from a run-queue without running it.  This is used
 * when we're stealing a thread from a remote queue.  Otherwise all threads
 * exit by calling sched_exit_thread() and sched_throw() themselves.
 */
static void
sched_4bsd_rem(struct thread *td)
{
	struct tdq *tdq;

	KTR_STATE1(KTR_SCHED, "thread", sched_tdname(td), "runq rem",
	    "prio:%d", td->td_priority);
	SDT_PROBE3(sched, , , dequeue, td, td->td_proc, NULL);
	tdq = TDQ_CPU(td_get_sched(td)->ts_cpu);
	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	MPASS(td->td_lock == TDQ_LOCKPTR(tdq));
	KASSERT(TD_ON_RUNQ(td),
	    ("sched_rem: thread not on run queue"));
	tdq_runq_rem(tdq, td);
	tdq_load_rem(tdq, td);
	TD_SET_CAN_RUN(td);
	if (td->td_priority == tdq->tdq_lowpri)
		tdq_setlowpri(tdq, NULL);
}

/*
 * Fetch cpu utilization information.  Updates on demand.
 */
static fixpt_t
sched_4bsd_pctcpu(struct thread *td)
{
	struct td_sched *ts;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	ts = td_get_sched(td);
	return (ts->ts_pctcpu);
}

/*
 * Enforce affinity settings for a thread.  Called after adjustments to
 * cpumask.
 */
static void
sched_4bsd_affinity(struct thread *td)
{
#ifdef SMP
	struct td_sched *ts;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	ts = td_get_sched(td);
	if (THREAD_CAN_SCHED(td, ts->ts_cpu))
		return;
	if (TD_ON_RUNQ(td)) {
		sched_rem(td);
		sched_add(td, SRQ_BORING | SRQ_HOLDTD);
		return;
	}
	if (!TD_IS_RUNNING(td))
		return;
	/*
	 * Force a switch before returning to userspace.  If the
	 * target thread is not running locally send an ipi to force
	 * the issue.
	 */
	ast_sched_locked(td, TDA_SCHED);
	if (td != curthread)
		ipi_cpu(ts->ts_cpu, IPI_PREEMPT);
#endif
}

/*
 * Bind a thread to a target cpu.
 */
static void
sched_4bsd_bind(struct thread *td, int cpu)
{
	struct td_sched *ts;

	THREAD_LOCK_ASSERT(td, MA_OWNED|MA_NOTRECURSED);
	KASSERT(td == curthread, ("sched_bind: can only bind curthread"));
	ts = td_get_sched(td);
	if (ts->ts_flags & TSF_BOUND)
		sched_unbind(td);
	KASSERT(THREAD_CAN_MIGRATE(td), ("%p must be migratable", td));
	ts->ts_flags |= TSF_BOUND;
	sched_pin();
	if (PCPU_GET(cpuid) == cpu)
		return;
	ts->ts_cpu = cpu;
	/* When we return from mi_switch we'll be on the correct cpu. */
	mi_switch(SW_VOL | SWT_BIND);
	thread_lock(td);
}

/*
 * Release a bound thread.
 */
static void
sched_4bsd_unbind(struct thread *td)
{
	struct td_sched *ts;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	KASSERT(td == curthread, ("sched_unbind: can only bind curthread"));
	ts = td_get_sched(td);
	if ((ts->ts_flags & TSF_BOUND) == 0)
		return;
	ts->ts_flags &= ~TSF_BOUND;
	sched_unpin();
}

static int
sched_4bsd_is_bound(struct thread *td)
{
	THREAD_LOCK_ASSERT(td, MA_OWNED);
	return (td_get_sched(td)->ts_flags & TSF_BOUND);
}

/*
 * Basic yield call.
 */
static void
sched_4bsd_relinquish(struct thread *td)
{
	thread_lock(td);
	mi_switch(SW_VOL | SWT_RELINQUISH);
}

/*
 * Return the total system load.
 */
static int
sched_4bsd_load(void)
{
#ifdef SMP
	int total;
	int i;

	total = 0;
	CPU_FOREACH(i)
		total += atomic_load_int(&TDQ_CPU(i)->tdq_sysload);
	return (total);
#else
	return (atomic_load_int(&TDQ_SELF()->tdq_sysload));
#endif
}

static int
sched_4bsd_sizeof_proc(void)
{
	return (sizeof(struct proc));
}

static int
sched_4bsd_sizeof_thread(void)
{
	return (sizeof(struct thread) + sizeof(struct td_sched));
}

#ifdef SMP
#define	TDQ_IDLESPIN(tdq)						\
    ((tdq)->tdq_cg != NULL && ((tdq)->tdq_cg->cg_flags & CG_FLAG_THREAD) == 0)
#else
#define	TDQ_IDLESPIN(tdq)	1
#endif

/*
 * The actual idle process.
 */
static void
sched_4bsd_idletd(void *dummy)
{
	struct thread *td;
	struct tdq *tdq;
	int oldswitchcnt, switchcnt;
	int i;

	mtx_assert(&Giant, MA_NOTOWNED);
	td = curthread;
	tdq = TDQ_SELF();
	THREAD_NO_SLEEPING();
	oldswitchcnt = -1;
	for (;;) {
		if (TDQ_LOAD(tdq)) {
			thread_lock(td);
			mi_switch(SW_VOL | SWT_IDLE);
		}
		switchcnt = TDQ_SWITCHCNT(tdq);
#ifdef SMP
		if (always_steal || switchcnt != oldswitchcnt) {
			oldswitchcnt = switchcnt;
			if (tdq_idled(tdq) == 0)
				continue;
		}
		switchcnt = TDQ_SWITCHCNT(tdq);
#else
		oldswitchcnt = switchcnt;
#endif
		/*
		 * If we're switching very frequently, spin while checking
		 * for load rather than entering a low power state that
		 * may require an IPI.  However, don't do any busy
		 * loops while on SMT machines as this simply steals
		 * cycles from cores doing useful work.
		 */
		if (TDQ_IDLESPIN(tdq) && switchcnt > sched_idlespinthresh) {
			for (i = 0; i < sched_idlespins; i++) {
				if (TDQ_LOAD(tdq))
					break;
				cpu_spinwait();
			}
		}

		/* If there was context switch during spin, restart it. */
		switchcnt = TDQ_SWITCHCNT(tdq);
		if (TDQ_LOAD(tdq) != 0 || switchcnt != oldswitchcnt)
			continue;

		/* Run main MD idle handler. */
		atomic_store_int(&tdq->tdq_cpu_idle, 1);
		/*
		 * Make sure that the tdq_cpu_idle update is globally visible
		 * before cpu_idle() reads tdq_load.  The order is important
		 * to avoid races with tdq_notify().
		 */
		atomic_thread_fence_seq_cst();
		/*
		 * Checking for again after the fence picks up assigned
		 * threads often enough to make it worthwhile to do so in
		 * order to avoid calling cpu_idle().
		 */
		if (TDQ_LOAD(tdq) != 0) {
			atomic_store_int(&tdq->tdq_cpu_idle, 0);
			continue;
		}
		cpu_idle(switchcnt * 4 > sched_idlespinthresh);
		atomic_store_int(&tdq->tdq_cpu_idle, 0);

		/*
		 * Account thread-less hardware interrupts and
		 * other wakeup reasons equal to context switches.
		 */
		switchcnt = TDQ_SWITCHCNT(tdq);
		if (switchcnt != oldswitchcnt)
			continue;
		TDQ_SWITCHCNT_INC(tdq);
		oldswitchcnt++;
	}
}

/*
 * sched_throw_grab() chooses a thread from the queue to switch to
 * next.  It returns with the tdq lock dropped in a spinlock section to
 * keep interrupts disabled until the CPU is running in a proper threaded
 * context.
 */
static struct thread *
sched_throw_grab(struct tdq *tdq)
{
	struct thread *newtd;

	newtd = choosethread();
	spinlock_enter();
	TDQ_UNLOCK(tdq);
	KASSERT(curthread->td_md.md_spinlock_count == 1,
	    ("invalid count %d", curthread->td_md.md_spinlock_count));
	return (newtd);
}

/*
 * A CPU is entering for the first time.
 */
static void
sched_4bsd_ap_entry(void)
{
	struct thread *newtd;
	struct tdq *tdq;

	tdq = TDQ_SELF();

	/* This should have been setup in schedinit_ap(). */
	THREAD_LOCKPTR_ASSERT(curthread, TDQ_LOCKPTR(tdq));

	TDQ_LOCK(tdq);
	/* Correct spinlock nesting. */
	spinlock_exit();
	PCPU_SET(switchtime, cpu_ticks());
	PCPU_SET(switchticks, ticks);

	newtd = sched_throw_grab(tdq);

#ifdef HWT_HOOKS
	HWT_CALL_HOOK(newtd, HWT_SWITCH_IN, NULL);
#endif

	/* doesn't return */
	cpu_throw(NULL, newtd);
}

/*
 * A thread is exiting.
 */
static void
sched_4bsd_throw(struct thread *td)
{
	struct thread *newtd;
	struct tdq *tdq;

	tdq = TDQ_SELF();

	MPASS(td != NULL);
	THREAD_LOCK_ASSERT(td, MA_OWNED);
	THREAD_LOCKPTR_ASSERT(td, TDQ_LOCKPTR(tdq));

	tdq_load_rem(tdq, td);
	td->td_lastcpu = td->td_oncpu;
	td->td_oncpu = NOCPU;
	thread_lock_block(td);

	newtd = sched_throw_grab(tdq);

#ifdef HWT_HOOKS
	HWT_CALL_HOOK(newtd, HWT_SWITCH_IN, NULL);
#endif

	/* doesn't return */
	cpu_switch(td, newtd, TDQ_LOCKPTR(tdq));
}

/*
 * This is called from fork_exit().  Just acquire the correct locks and
 * let fork do the rest of the work.
 */
static void
sched_4bsd_fork_exit(struct thread *td)
{
	struct tdq *tdq;
	int cpuid;

	/*
	 * Finish setting up thread glue so that it begins execution in a
	 * non-nested critical section with the scheduler lock held.
	 */
	KASSERT(curthread->td_md.md_spinlock_count == 1,
	    ("invalid count %d", curthread->td_md.md_spinlock_count));
	cpuid = PCPU_GET(cpuid);
	tdq = TDQ_SELF();
	TDQ_LOCK(tdq);
	spinlock_exit();
	MPASS(td->td_lock == TDQ_LOCKPTR(tdq));
	td->td_oncpu = cpuid;
	KTR_STATE1(KTR_SCHED, "thread", sched_tdname(td), "running",
	    "prio:%d", td->td_priority);
	SDT_PROBE0(sched, , , on__cpu);
}

/*
 * Create on first use to catch odd startup conditions.
 */
static char *
sched_4bsd_tdname(struct thread *td)
{
#ifdef KTR
	struct td_sched *ts;

	ts = td_get_sched(td);
	if (ts->ts_name[0] == '\0')
		snprintf(ts->ts_name, sizeof(ts->ts_name),
		    "%s tid %d", td->td_name, td->td_tid);
	return (ts->ts_name);
#else
	return (td->td_name);
#endif
}

static void
sched_4bsd_clear_tdname(struct thread *td)
{
#ifdef KTR
	struct td_sched *ts;

	ts = td_get_sched(td);
	ts->ts_name[0] = '\0';
#endif
}

/*
 * Main loop for a kthread that executes sched_update once a second.
 */
static void
sched_update_kthread(void)
{
	for (;;) {
		sched_update();
		pause("-", hz);
	}
}

static struct kproc_desc sched_kp = {
        "sched_update",
        sched_update_kthread,
        NULL
};

static void
sched_4bsd_schedcpu(void)
{
	kproc_start(&sched_kp);
}

#ifdef SMP
static int
sched_4bsd_find_child_with_core(int cpu, struct cpu_group *grp)
{
	int i;

	if (grp->cg_children == 0)
		return (-1);

	MPASS(grp->cg_child);
	for (i = 0; i < grp->cg_children; i++) {
		if (CPU_ISSET(cpu, &grp->cg_child[i].cg_mask))
			return (i);
	}

	return (-1);
}

static int
sched_4bsd_find_l2_neighbor(int cpu)
{
	struct cpu_group *grp;
	int i;

	grp = cpu_top;
	if (grp == NULL)
		return (-1);

	/*
	 * Find the smallest CPU group that contains the given core.
	 */
	i = 0;
	while ((i = sched_4bsd_find_child_with_core(cpu, grp)) != -1) {
		/*
		 * If the smallest group containing the given CPU has less
		 * than two members, we conclude the given CPU has no
		 * L2 neighbor.
		 */
		if (grp->cg_child[i].cg_count <= 1)
			return (-1);
		grp = &grp->cg_child[i];
	}

	/* Must share L2. */
	if (grp->cg_level > CG_SHARE_L2 || grp->cg_level == CG_SHARE_NONE)
		return (-1);

	/*
	 * Select the first member of the set that isn't the reference
	 * CPU, which at this point is guaranteed to exist.
	 */
	for (i = 0; i < CPU_SETSIZE; i++) {
		if (CPU_ISSET(i, &grp->cg_mask) && i != cpu)
			return (i);
	}

	/* Should never be reached */
	return (-1);
}
#else
static int
sched_4bsd_find_l2_neighbor(int cpu)
{
	return (-1);
}
#endif

struct sched_instance sched_4bsd_instance = {
#define	SLOT(name) .name = sched_4bsd_##name
	SLOT(load),
	SLOT(rr_interval),
	SLOT(runnable),
	SLOT(exit),
	SLOT(fork),
	SLOT(fork_exit),
	SLOT(class),
	SLOT(nice),
	SLOT(ap_entry),
	SLOT(exit_thread),
	SLOT(estcpu),
	SLOT(fork_thread),
	SLOT(ithread_prio),
	SLOT(lend_prio),
	SLOT(lend_user_prio),
	SLOT(lend_user_prio_cond),
	SLOT(pctcpu),
	SLOT(prio),
	SLOT(sleep),
	SLOT(sswitch),
	SLOT(throw),
	SLOT(unlend_prio),
	SLOT(user_prio),
	SLOT(userret_slowpath),
	SLOT(add),
	SLOT(choose),
	SLOT(clock),
	SLOT(idletd),
	SLOT(preempt),
	SLOT(relinquish),
	SLOT(rem),
	SLOT(wakeup),
	SLOT(bind),
	SLOT(unbind),
	SLOT(is_bound),
	SLOT(affinity),
	SLOT(sizeof_proc),
	SLOT(sizeof_thread),
	SLOT(tdname),
	SLOT(clear_tdname),
	SLOT(find_l2_neighbor),
	SLOT(init),
	SLOT(init_ap),
	SLOT(setup),
	SLOT(initticks),
	SLOT(schedcpu),
#undef SLOT
};
DECLARE_SCHEDULER(fourbsd_sched_selector, "4BSD", &sched_4bsd_instance);

static int
sysctl_kern_quantum(SYSCTL_HANDLER_ARGS)
{
	int error, new_val, period;

	period = 1000000 / realstathz;
	new_val = period * sched_slice;
	error = sysctl_handle_int(oidp, &new_val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (new_val <= 0)
		return (EINVAL);
	sched_slice = imax(1, (new_val + period / 2) / period);
	sched_slice_min = imax(1, sched_slice / SCHED_SLICE_MIN_DIVISOR);
	hogticks = imax(1, (2 * hz * sched_slice + realstathz / 2) /
	    realstathz);
	return (0);
}

static int
sysctl_kern_slice(SYSCTL_HANDLER_ARGS)
{
	int error, new_val;

	new_val = sched_slice;
	error = sysctl_handle_int(oidp, &new_val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	if (new_val <= 0)
		return (EINVAL);
	sched_slice = imax(1, new_val);
	sched_slice_min = imax(1, sched_slice / SCHED_SLICE_MIN_DIVISOR);
	hogticks = imax(1, (2 * hz * sched_slice + realstathz / 2) /
	    realstathz);
	return (0);
}

SYSCTL_NODE(_kern_sched, OID_AUTO, 4bsd, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "4BSD Scheduler");

SYSCTL_PROC(_kern_sched_4bsd, OID_AUTO, quantum,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0,
    sysctl_kern_quantum, "I",
    "Quantum for timeshare threads in microseconds");
SYSCTL_PROC(_kern_sched_4bsd, OID_AUTO, slice,
    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, NULL, 0,
    sysctl_kern_slice, "I",
    "Quantum for timeshare threads in stathz ticks");
SYSCTL_INT(_kern_sched_4bsd, OID_AUTO, idlespins, CTLFLAG_RWTUN,
    &sched_idlespins, 0,
    "Number of times idle thread will spin waiting for new work");
SYSCTL_INT(_kern_sched_4bsd, OID_AUTO, idlespinthresh, CTLFLAG_RW,
    &sched_idlespinthresh, 0,
    "Threshold before we will permit idle thread spinning");
#ifdef SMP
SYSCTL_INT(_kern_sched_4bsd, OID_AUTO, affinity, CTLFLAG_RW, &affinity, 0,
    "Number of hz ticks to keep thread affinity for");
SYSCTL_INT(_kern_sched_4bsd, OID_AUTO, balance, CTLFLAG_RWTUN, &rebalance, 0,
    "Enables the long-term load balancer");
SYSCTL_INT(_kern_sched_4bsd, OID_AUTO, balance_interval, CTLFLAG_RW,
    &balance_interval, 0,
    "Average period in stathz ticks to run the long-term balancer");
SYSCTL_INT(_kern_sched_4bsd, OID_AUTO, steal_idle, CTLFLAG_RWTUN,
    &steal_idle, 0,
    "Attempts to steal work from other cores before idling");
SYSCTL_INT(_kern_sched_4bsd, OID_AUTO, steal_thresh, CTLFLAG_RWTUN,
    &steal_thresh, 0,
    "Minimum load on remote CPU before we'll steal");
SYSCTL_INT(_kern_sched_4bsd, OID_AUTO, trysteal_limit, CTLFLAG_RWTUN,
    &trysteal_limit, 0,
    "Topological distance limit for stealing threads in sched_switch()");
SYSCTL_INT(_kern_sched_4bsd, OID_AUTO, always_steal, CTLFLAG_RWTUN,
    &always_steal, 0,
    "Always run the stealer from the idle thread");
#endif
