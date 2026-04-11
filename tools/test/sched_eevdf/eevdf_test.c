/* SPDX-License-Identifier: BSD-2-Clause */
/* Kernel service fixture; EEVDF functions are inserted by eevdf_test.py. */
#include "priority.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/queue.h>
#include <sys/types.h>

#define SMP 1
#define __read_mostly
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#define __containerof(p, t, m) ((t *)((char *)(p) - offsetof(t, m)))
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#define imax(a, b) max(a, b)
#define KASSERT(e, msg) assert(e)
#define MPASS(e) assert(e)
#define MA_OWNED 1
#define KTR_POINT3(...) ((void)0)
#define KTR_STATE1(...) ((void)0)
#define KTR_STATE2(...) ((void)0)
#define KTR_COUNTER0(...) ((void)0)
#define SDT_PROBE2(...) ((void)0)
#define SDT_PROBE3(...) ((void)0)
#define SDT_PROBE4(...) ((void)0)
#define SCHED_STAT_INC(...) ((void)0)
#define RQ_NQS 256
#define RQ_PPQ 1
#define SRQ_YIELDING 1
#define SRQ_BORING 0
#define SRQ_BORROWING 16
#define SRQ_PREEMPTED 8
#define TS_NAME_LEN 32
#define TDQ_NAME_LEN 32
#define TDQ_LOADNAME_LEN 32
#define FSCALE 2048
#define TDF_BORROWING 1
#define TDF_NOLOAD 2
#define TDF_SCHED0 4
#define TDF_SCHED2 8
#define TDA_SCHED 1
#define IPI_AST 1
#define IPI_PREEMPT 2
#define KERNEL_PANICKED() false
#define TD_SET_RUNQ(td) ((td)->state = 1)
#define TD_SET_CAN_RUN(td) ((td)->state = 0)
#define TD_ON_RUNQ(td) ((td)->state == 1)
#define TD_IS_RUNNING(td) ((td)->state == 2)
#define TD_IS_INHIBITED(td) ((td)->state == 3)
#define TD_IS_IDLETHREAD(td) ((td)->idle)
#define TD_ON_LOCK(td) false
#define THREAD_CAN_MIGRATE(td) ((td)->td_pinned == 0)
#define THREAD_CAN_SCHED(td, cpu) (((td)->cpumask & (1 << (cpu))) != 0)
#define FOREACH_THREAD_IN_PROC(p, td)                                          \
	for ((td) = (p)->first; (td); (td) = (td)->next)
#define PROC_LOCK_ASSERT(...) ((void)0)
#define atomic_load_int(p) (*(p))
#define atomic_store_int(p, v) (*(p) = (v))
#define atomic_thread_fence_seq_cst() ((void)0)
#define ts_to_td(ts) (&((struct thread *)(ts))[-1])

typedef uint32_t fixpt_t;
struct mtx {
	int held;
};
struct mtx_padalign {
	int held;
};
struct cpu_group;
struct proc;
struct thread {
	TAILQ_ENTRY(thread) td_runq;
	struct mtx *td_lock;
	struct proc *td_proc;
	struct thread *next;
	int td_pri_class, td_priority, td_base_pri, td_base_user_pri;
	int td_user_pri, td_lend_user_pri, td_base_ithread_pri;
	int td_oncpu, td_pinned, td_flags, td_owepreempt, td_slptick;
	int state, idle, ast, cpumask, td_rqindex;
};
struct proc {
	struct thread *first;
	int p_nice;
};
TAILQ_HEAD(rq_queue, thread);
struct runq {
	struct rq_queue rq_queues[RQ_NQS];
};

/* EEVDF_TYPES */
/* EEVDF_TREE */

struct test_thread {
	struct thread td;
	struct td_sched ts;
};
_Static_assert(offsetof(struct test_thread, ts) == sizeof(struct thread),
	       "fixture must match td_get_sched layout");
static struct tdq queues[2];
static struct test_thread idles[2];
static struct thread *curthread;
static int current_cpu, ticks, hz = 1000, cold, smp_started;
static int balance_ticks, rebalance, last_ipi;
static struct tdq *balance_tdq;
static int64_t now;
static void
panic(const char *message, ...)
{
	fputs(message, stderr);
	abort();
}
#define TDQ_SELF() (&queues[current_cpu])
#define TDQ_CPU(cpu) (&queues[cpu])
#define TDQ_ID(q) ((q)->tdq_id)
#define TDQ_LOCKPTR(q) ((struct mtx *)&(q)->tdq_lock)
#define TDQ_LOCK_ASSERT(q, mode) assert((q)->tdq_lock.held)
#define THREAD_LOCK_ASSERT(td, mode) assert((td)->td_lock->held)
#define THREAD_LOCKPTR_ASSERT(td, lock) assert((td)->td_lock == (lock))
#define TDQ_LOCK(q)                                                            \
	do {                                                                   \
		assert(!(q)->tdq_lock.held);                                   \
		(q)->tdq_lock.held = 1;                                        \
	} while (0)
#define TDQ_UNLOCK(q)                                                          \
	do {                                                                   \
		assert((q)->tdq_lock.held);                                    \
		(q)->tdq_lock.held = 0;                                        \
	} while (0)
#define PCPU_GET(field) PCPU_##field
#define PCPU_cpuid current_cpu
#define PCPU_idlethread (&idles[current_cpu].td)
static struct td_sched *
td_get_sched(struct thread *td)
{
	return ((struct td_sched *)(td + 1));
}
static void
thread_lock(struct thread *td)
{
	assert(td->td_lock->held);
}
static void
thread_unlock(struct thread *td)
{
	assert(td->td_lock->held);
}
static void
thread_lock_block_wait(struct thread *td)
{
	assert(td->td_lock->held);
}
static void
ast_sched_locked(struct thread *td, int ast)
{
	THREAD_LOCK_ASSERT(td, 1);
	td->ast = ast;
}
static void
turnstile_adjust(struct thread *td, int pri)
{
	(void)td;
	(void)pri;
}
static void
sched_balance(void)
{
	abort();
}
static void
ipi_cpu(int cpu, int ipi)
{
	(void)cpu;
	last_ipi = ipi;
}
static bool
cpu_idle_wakeup(int cpu)
{
	(void)cpu;
	return (false);
}
struct bintime {
	int64_t sec;
	uint64_t frac;
};
static void
binuptime(struct bintime *bt)
{
	bt->sec = now / 1000000000LL;
	bt->frac = ((__uint128_t)(now % 1000000000LL) << 64) / 1000000000LL;
}
static void
runq_init(struct runq *rq)
{
	for (int i = 0; i < RQ_NQS; i++)
		TAILQ_INIT(&rq->rq_queues[i]);
}
static void
runq_add(struct runq *rq, struct thread *td, int flags)
{
	td->td_rqindex = td->td_priority;
	if (flags & SRQ_PREEMPTED)
		TAILQ_INSERT_HEAD(&rq->rq_queues[td->td_rqindex], td, td_runq);
	else
		TAILQ_INSERT_TAIL(&rq->rq_queues[td->td_rqindex], td, td_runq);
}
static void
runq_remove(struct runq *rq, struct thread *td)
{
	struct thread *it;
	bool found = false;
	TAILQ_FOREACH(it, &rq->rq_queues[td->td_rqindex], td_runq)
	if (it == td) found = true;
	assert(found);
	TAILQ_REMOVE(&rq->rq_queues[td->td_rqindex], td, td_runq);
}
static struct thread *
runq_choose(struct runq *rq)
{
	for (int i = 0; i < RQ_NQS; i++)
		if (!TAILQ_EMPTY(&rq->rq_queues[i]))
			return (TAILQ_FIRST(&rq->rq_queues[i]));
	return (NULL);
}
#define sched_prio sched_eevdf_prio
#define sched_user_prio sched_eevdf_user_prio
#define sched_lend_prio sched_eevdf_lend_prio
#define sched_rem sched_eevdf_rem
static void sched_add(struct thread *td, int flags);

/* EEVDF_FUNCTIONS */

static void
sched_add(struct thread *td, int flags)
{
	tdq_add(TDQ_CPU(td_get_sched(td)->ts_cpu), td, flags);
}
static void
reset(void)
{
	memset(queues, 0, sizeof(queues));
	memset(idles, 0, sizeof(idles));
	current_cpu = ticks = last_ipi = 0;
	now = 0;
	for (int i = 0; i < 2; i++) {
		struct tdq *q = &queues[i];
		q->tdq_lock.held = 1;
		q->tdq_id = i;
		runq_init(&q->tdq_realtime);
		runq_init(&q->tdq_idle);
		idles[i].td = (struct thread){.td_lock = TDQ_LOCKPTR(q),
					      .idle = 1,
					      .state = 2,
					      .td_priority = PRI_MAX_IDLE,
					      .td_oncpu = i};
		q->tdq_curthread = &idles[i].td;
		q->tdq_lowpri = PRI_MAX_IDLE;
	}
	curthread = &idles[0].td;
}
static void
init_thread(struct test_thread *t, int cpu, int nice)
{
	memset(t, 0, sizeof(*t));
	t->td = (struct thread){.td_lock = TDQ_LOCKPTR(&queues[cpu]),
				.cpumask = 3,
				.td_pri_class = PRI_TIMESHARE,
				.td_priority = nice_to_pri(nice),
				.td_base_pri = nice_to_pri(nice),
				.td_base_user_pri = nice_to_pri(nice),
				.td_user_pri = nice_to_pri(nice),
				.td_lend_user_pri = PRI_MAX,
				.td_oncpu = cpu};
	t->ts.ts_cpu = cpu;
	t->ts.ts_slice = sched_quantum;
	t->ts.ts_eevdf_slice = EEVDF_BASE_SLICE_NS;
	entity_set_weight(&t->ts, nice);
}
static struct thread *
choose(void)
{
	curthread = sched_eevdf_choose();
	curthread->state = 2;
	curthread->td_oncpu = current_cpu;
	return (curthread);
}
static void
requeue_current(void)
{
	tdq_runq_add(TDQ_SELF(), curthread, 0);
	TDQ_SELF()->tdq_curr = NULL;
}
static uint32_t random_state = 1;
static uint32_t
random_number(void)
{
	random_state = random_state * 1664525 + 1013904223;
	return (random_state);
}

static int
check_tree(struct rb_node *n, struct rb_node *parent, int64_t *last,
	   int64_t *minimum, int *count)
{
	int64_t lm = INT64_MAX, rm = INT64_MAX;
	if (n == NULL) {
		*minimum = INT64_MAX;
		return (1);
	}
	struct td_sched *ts = TS_FROM_RB_NODE(n);
	assert(n->rb_parent == parent);
	int left = check_tree(n->rb_left, n, last, &lm, count);
	assert(ts->ts_vruntime >= *last);
	*last = ts->ts_vruntime;
	(*count)++;
	int right = check_tree(n->rb_right, n, last, &rm, count);
	assert(left == right);
	if (n->rb_color == RB_RED) {
		assert(n->rb_left == NULL || n->rb_left->rb_color == RB_BLACK);
		assert(n->rb_right == NULL ||
		       n->rb_right->rb_color == RB_BLACK);
	}
	*minimum = min(ts->ts_deadline, min(lm, rm));
	assert(ts->ts_min_deadline == *minimum);
	return (left + (n->rb_color == RB_BLACK));
}
static void
test_tree(void)
{
	/* Regression: a two-child transplant lost its subtree's minimum. */
	struct td_sched regression[9] = {0};
	int64_t values[9][2] = {{49, 123}, {30, 103}, {40, 106},
				{42, 130}, {27, 57},  {3, 73},
				{57, 118}, {28, 120}, {72, 171}};
	struct rb_root reduced = RB_ROOT_INITIALIZER;
	for (int i = 0; i < 9; i++) {
		regression[i].ts_vruntime = values[i][0];
		regression[i].ts_deadline = values[i][1];
		eevdf_rb_insert(&reduced, &regression[i]);
	}
	eevdf_rb_remove(&reduced, &regression[4]);
	assert(eevdf_rb_pick(&reduced, 40)->ts_deadline == 73);
	struct td_sched entities[64] = {0};
	bool active[64] = {0};
	struct rb_root root = RB_ROOT_INITIALIZER;
	for (int step = 0; step < 30000; step++) {
		int i = random_number() % 64;
		if (active[i]) {
			eevdf_rb_remove(&root, &entities[i]);
			active[i] = false;
		} else {
			entities[i].ts_vruntime = random_number() % 1000;
			entities[i].ts_deadline = entities[i].ts_vruntime + 1 +
						  random_number() % 1000;
			eevdf_rb_insert(&root, &entities[i]);
			active[i] = true;
		}
		int count = 0;
		int64_t last = INT64_MIN, minimum;
		check_tree(root.rb_node, NULL, &last, &minimum, &count);
		assert(count == root.rb_count);
		assert(root.rb_node == NULL ||
		       root.rb_node->rb_color == RB_BLACK);
		struct rb_node *first = root.rb_node;
		while (first != NULL && first->rb_left != NULL)
			first = first->rb_left;
		assert(first == root.rb_leftmost);
		for (int query = 0; query < 12; query++) {
			int64_t V = random_number() % 2000, best = INT64_MAX;
			for (int j = 0; j < 64; j++)
				if (active[j] && entities[j].ts_vruntime <= V)
					best =
					    min(best, entities[j].ts_deadline);
			struct td_sched *picked = eevdf_rb_pick(&root, V);
			assert((picked == NULL) == (best == INT64_MAX));
			if (picked != NULL)
				assert(picked->ts_deadline == best &&
				       picked->ts_vruntime <= V);
		}
	}
	puts("PASS: randomized RB invariants and exhaustive deadline oracle");
}
static void
test_accounting(void)
{
	reset();
	struct test_thread a, b;
	init_thread(&a, 0, 0);
	init_thread(&b, 0, 0);
	a.ts.ts_flags = b.ts.ts_flags = TSF_EEVDF;
	b.ts.ts_vruntime = 6000000;
	entity_update_deadline(&a.ts);
	entity_update_deadline(&b.ts);
	tdq_add(&queues[0], &a.td, 0);
	tdq_add(&queues[0], &b.td, 0);
	assert(tdq_avg_vruntime(&queues[0]) == 3000000);
	assert(choose() == &a.td);
	queues[0].tdq_clock = 2000000;
	update_curr(&queues[0]);
	assert(tdq_avg_vruntime(&queues[0]) == 4000000);
	assert(!entity_eligible(&queues[0], &b.ts));
	assert(pick_eevdf(&queues[0]) == NULL);
	assert(queues[0].tdq_transferable == 1 && queues[0].tdq_load == 2);
	entity_save_lag(&queues[0], &a.ts);
	queues[0].tdq_curr = NULL;
	int64_t remaining = a.ts.ts_deadline;
	place_entity(&queues[0], &a.ts);
	tdq_runq_add(&queues[0], &a.td, 0);
	assert(tdq_avg_vruntime(&queues[0]) - a.ts.ts_vruntime == 2000000);
	assert(a.ts.ts_deadline - a.ts.ts_vruntime == remaining);
	puts("PASS: current eligibility, lag conservation, and remaining "
	     "request");
}
static void
test_sleep_debt_and_reweight(void)
{
	reset();
	struct test_thread a, b;
	struct proc p = {0};
	init_thread(&a, 0, 0);
	init_thread(&b, 0, 0);
	a.td.td_proc = &p;
	p.first = &a.td;
	tdq_add(&queues[0], &a.td, 0);
	tdq_add(&queues[0], &b.td, 0);
	assert(choose() == &a.td);
	now = 2000000;
	sched_eevdf_nice(&p, 10);
	assert(llabs(a.ts.ts_sum_exec - 2000000) <= 1);
	int64_t debt =
	    (tdq_avg_vruntime(&queues[0]) - a.ts.ts_vruntime) * a.ts.ts_weight;
	assert(llabs(debt + 1024000000LL) < 1024);
	entity_save_lag(&queues[0], &a.ts);
	int64_t request = a.ts.ts_deadline;
	queues[0].tdq_curr = NULL;
	a.td.state = 3;
	tdq_load_rem(&queues[0], &a.td);
	sched_eevdf_wakeup(&a.td, 0);
	assert(a.ts.ts_slice == sched_quantum);
	assert(!entity_eligible(&queues[0], &a.ts));
	assert(llabs((tdq_avg_vruntime(&queues[0]) - a.ts.ts_vruntime) *
			 a.ts.ts_weight -
		     debt) < 1024);
	assert(a.ts.ts_deadline - a.ts.ts_vruntime == request);
	assert(!a.ts.ts_on_tree || a.ts.ts_runq == TSQ_EEVDF);
	puts("PASS: charge before reweight and preserve negative lag across "
	     "wakeup");
}
static void
test_arithmetic(void)
{
	for (int nice = -20; nice <= 20; nice++) {
		struct td_sched ts = {0};
		entity_set_weight(&ts, nice);
		if (nice > -20)
			assert(eevdf_prio_to_weight[nice + 20] <
			       eevdf_prio_to_weight[nice + 19]);
		for (int64_t delta = 1; delta <= 1000000000000LL; delta *= 10)
			assert(
			    calc_delta_fair(delta, ts.ts_weight) ==
			    (int64_t)((__int128)delta * 1024 / ts.ts_weight));
	}
	reset();
	queues[0].tdq_min_vruntime = 3600000000000LL;
	struct td_sched ts = {.ts_vruntime = 3600000000000LL,
			      .ts_weight = 1024};
	for (int i = 0; i < 4096; i++)
		avg_vruntime_add(&queues[0], &ts);
	assert(tdq_avg_vruntime(&queues[0]) == ts.ts_vruntime);
	assert(queues[0].tdq_avg_vruntime == 0);
	puts("PASS: all nice weights, large fair deltas, and long-uptime "
	     "average");
}
static void
test_classes(void)
{
	reset();
	struct test_thread a;
	init_thread(&a, 0, 0);
	tdq_add(&queues[0], &a.td, 0);
	sched_eevdf_class(&a.td, PRI_REALTIME);
	sched_prio(&a.td, PRI_MIN_REALTIME);
	assert(a.ts.ts_runq == TSQ_REALTIME && queues[0].tdq_nr_timeshare == 0);
	sched_eevdf_class(&a.td, PRI_IDLE);
	sched_user_prio(&a.td, PRI_MIN_IDLE);
	sched_prio(&a.td, PRI_MIN_IDLE);
	assert(a.ts.ts_runq == TSQ_IDLE &&
	       runq_choose(&queues[0].tdq_realtime) == NULL);
	sched_eevdf_class(&a.td, PRI_TIMESHARE);
	sched_user_prio(&a.td, nice_to_pri(0));
	sched_prio(&a.td, nice_to_pri(0));
	assert(a.ts.ts_runq == TSQ_EEVDF &&
	       runq_choose(&queues[0].tdq_idle) == NULL);
	assert(queues[0].tdq_transferable == 1 && queues[0].tdq_load == 1);
	assert(choose() == &a.td);
	sched_eevdf_class(&a.td, PRI_REALTIME);
	sched_user_prio(&a.td, PRI_MIN_REALTIME);
	sched_prio(&a.td, PRI_MIN_REALTIME);
	assert(queues[0].tdq_curr == NULL);
	sched_eevdf_class(&a.td, PRI_TIMESHARE);
	sched_user_prio(&a.td, nice_to_pri(0));
	sched_prio(&a.td, nice_to_pri(0));
	assert(queues[0].tdq_curr == &a.ts && a.ts.ts_runq == TSQ_NONE);
	puts("PASS: queued and running RT/idle/timeshare transitions");
}
static void
test_lending_and_nice(void)
{
	reset();
	struct test_thread a, rt;
	struct proc p = {0};
	init_thread(&a, 0, 0);
	init_thread(&rt, 0, 0);
	a.td.td_proc = &p;
	p.first = &a.td;
	rt.td.td_pri_class = PRI_REALTIME;
	rt.td.td_priority = PRI_MIN_REALTIME + 8;
	tdq_add(&queues[0], &a.td, 0);
	tdq_add(&queues[0], &rt.td, 0);
	sched_lend_prio(&a.td, PRI_MIN_REALTIME);
	assert(tdq_choose(&queues[0]) == &a.td && a.ts.ts_runq == TSQ_REALTIME);
	sched_eevdf_nice(&p, 10);
	assert(a.td.td_priority == PRI_MIN_REALTIME);
	sched_eevdf_unlend_prio(&a.td, PRI_MAX);
	assert(a.ts.ts_runq == TSQ_EEVDF && !(a.td.td_flags & TDF_BORROWING));
	sched_eevdf_lend_user_prio(&a.td, PRI_MIN_REALTIME);
	assert(a.ts.ts_runq == TSQ_REALTIME);
	sched_eevdf_nice(&p, 0);
	assert(a.td.td_priority == PRI_MIN_REALTIME);
	sched_eevdf_lend_user_prio(&a.td, PRI_MAX);
	sched_prio(&a.td, a.td.td_user_pri);
	assert(a.ts.ts_runq == TSQ_EEVDF);
	p.first = &rt.td;
	rt.td.td_proc = &p;
	rt.td.td_base_pri = rt.td.td_priority;
	rt.td.td_user_pri = rt.td.td_base_user_pri = rt.td.td_priority;
	sched_eevdf_nice(&p, 20);
	assert(rt.td.td_priority == PRI_MIN_REALTIME + 8);
	puts("PASS: kernel/user priority inheritance and renice preservation");
}
static void
test_migration(void)
{
	reset();
	struct test_thread a, b;
	init_thread(&a, 0, 0);
	init_thread(&b, 1, 0);
	queues[0].tdq_min_vruntime = 1000000000000LL;
	queues[1].tdq_min_vruntime = 2000000000;
	tdq_add(&queues[0], &a.td, 0);
	tdq_add(&queues[1], &b.td, 0);
	assert(tdq_move(&queues[0], &queues[1]) != -1);
	assert(a.ts.ts_cpu == 1 && a.td.td_lock == TDQ_LOCKPTR(&queues[1]));
	assert(a.ts.ts_vruntime == b.ts.ts_vruntime && queues[0].tdq_load == 0);
	current_cpu = 1;
	curthread = &idles[1].td;
	assert(choose() == &b.td);
	current_cpu = 0;
	curthread = &idles[0].td;
	struct test_thread c;
	init_thread(&c, 0, 0);
	tdq_add(&queues[0], &c.td, 0);
	assert(choose() == &c.td);
	int64_t request = c.ts.ts_deadline - c.ts.ts_vruntime;
	c.ts.ts_cpu = 1;
	queues[1].tdq_lock.held = 0;
	assert(sched_switch_migrate(&queues[0], &c.td, 0) ==
	       TDQ_LOCKPTR(&queues[1]));
	assert(c.ts.ts_vruntime == b.ts.ts_vruntime);
	assert(c.ts.ts_deadline - c.ts.ts_vruntime == request);
	assert(queues[0].tdq_curr == NULL && queues[0].tdq_load == 0);
	puts("PASS: queued/running migration across different virtual-time "
	     "origins");
}
static void
test_clock_and_stats(void)
{
	reset();
	struct test_thread a;
	init_thread(&a, 0, 0);
	a.td.td_pri_class = PRI_IDLE;
	a.td.td_priority = PRI_MIN_IDLE;
	tdq_add(&queues[0], &a.td, 0);
	assert(choose() == &a.td);
	sched_eevdf_clock(&a.td, sched_quantum);
	assert(a.td.ast && (a.td.td_flags & TDF_SLICEEND));
	a.td.ast = 0;
	a.td.td_pri_class = PRI_FIFO;
	sched_eevdf_clock(&a.td, sched_quantum);
	assert(a.td.ast == 0);
	realstathz = 127;
	assert(sched_eevdf_rr_interval() == 63);
	a.td.td_pri_class = PRI_ITHD;
	a.td.td_base_pri = PRI_MIN_ITHD;
	a.td.td_priority = PRI_MIN_ITHD;
	a.ts.ts_slice = sched_quantum;
	sched_eevdf_clock(&a.td, sched_quantum);
	assert(a.td.td_owepreempt && a.td.td_priority == PRI_MIN_ITHD + RQ_PPQ);
	a.ts.ts_ticks = a.ts.ts_ftick = a.ts.ts_ltick = 0;
	for (ticks = 1; ticks <= hz; ticks++)
		sched_pctcpu_update(&a.ts, 1);
	assert(sched_eevdf_pctcpu(&a.td) == FSCALE);
	a.td.state = 3;
	ticks += 11 * hz;
	assert(sched_eevdf_pctcpu(&a.td) == 0);
	puts("PASS: idle/interrupt quanta, FIFO exclusion, RR units, and CPU "
	     "decay");
}
static void
test_wakeup_preemption(void)
{
	reset();
	struct test_thread a, b;
	init_thread(&a, 0, 0);
	init_thread(&b, 0, 0);
	tdq_add(&queues[0], &a.td, 0);
	assert(choose() == &a.td);
	queues[0].tdq_clock = 1000000;
	update_curr(&queues[0]);
	b.ts.ts_flags = TSF_EEVDF | TSF_VLAG;
	b.ts.ts_deadline =
	    500000; /* Waking with a partially consumed request. */
	tdq_add(&queues[0], &b.td, 0);
	assert(a.td.td_priority == b.td.td_priority &&
	       tdq_need_resched(&queues[0]));
	sched_setpreempt(&queues[0]);
	assert(a.td.ast);
	current_cpu = 1;
	curthread = &idles[1].td;
	tdq_notify(&queues[0]);
	assert(last_ipi == IPI_AST);
	queues[0].tdq_owepreempt = 0;
	preempt_thresh = PRI_MAX_IDLE;
	tdq_notify(&queues[0]);
	assert(last_ipi == IPI_PREEMPT);
	preempt_thresh = 0;
	puts("PASS: equal-priority deadline wakeups and remote preemption "
	     "threshold");
}
static void
test_fair_shares(void)
{
	reset();
	struct test_thread threads[3];
	int nice[3] = {-5, 0, 10};
	int64_t service[3] = {0}, total = 0, weight = 0;
	for (int i = 0; i < 3; i++) {
		init_thread(&threads[i], 0, nice[i]);
		tdq_add(&queues[0], &threads[i].td, 0);
		weight += threads[i].ts.ts_weight;
	}
	choose();
	for (int step = 0; step < 100000; step++) {
		for (int i = 0; i < 3; i++)
			if (curthread == &threads[i].td)
				service[i] += 100000;
		total += 100000;
		queues[0].tdq_clock += 100000;
		update_curr(&queues[0]);
		if (queues[0].tdq_curr->ts_vruntime >=
			queues[0].tdq_curr->ts_deadline ||
		    tdq_need_resched(&queues[0])) {
			requeue_current();
			choose();
		}
	}
	for (int i = 0; i < 3; i++) {
		int64_t expected = total * threads[i].ts.ts_weight / weight;
		assert(llabs(service[i] - expected) < 9000000);
	}
	puts("PASS: CPU-bound proportional service for unequal nice weights");
}
int
main(void)
{
	test_tree();
	test_accounting();
	test_sleep_debt_and_reweight();
	test_arithmetic();
	test_classes();
	test_lending_and_nice();
	test_migration();
	test_clock_and_stats();
	test_wakeup_preemption();
	test_fair_shares();
	return (0);
}
