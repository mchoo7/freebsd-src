/*
 * Copyright (c) 2026 FreeBSD Foundation
 *
 * This software was developed by Minsoo Choo under sponsorship from the
 * FreeBSD Foundation.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

/*
 * EEVDF (Earliest Eligible Virtual Deadline First) scheduler.
 *
 * Uses struct tdq per CPU containing:
 *   - struct runq for realtime and interrupt threads (priority-bitmap)
 *   - struct runq for idle threads
 *   - EEVDF augmented rb-tree for timeshare threads
 *
 * The EEVDF algorithm replaces ULE's interactivity heuristics and
 * priority-decay mechanism for timeshare scheduling with a
 * mathematically grounded proportional-share approach.
 *
 * Realtime (SCHED_FIFO, SCHED_RR) and idle threads continue to use
 * the existing struct runq priority-bitmap queues.
 */

#include "opt_hwpmc_hooks.h"
#include "opt_hwt_hooks.h"
#include "opt_sched.h"

#include <sys/systm.h>
#include <sys/kdb.h>
#include <sys/kernel.h>
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
#include <sys/time.h>
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

#define	KTR_EEVDF	0

#define	TS_NAME_LEN	(MAXCOMLEN + sizeof(" td ") + sizeof(__XSTRING(UINT_MAX)))
#define	TDQ_NAME_LEN	(sizeof("sched lock ") + sizeof(__XSTRING(MAXCPU)))
#define	TDQ_LOADNAME_LEN (sizeof("CPU ") + sizeof(__XSTRING(MAXCPU)) - 1 + \
			  sizeof(" load"))

/*
 * Augmented red-black tree for EEVDF.
 *
 * Keyed by ts_vruntime.  Augmented with ts_min_deadline for O(log n)
 * eligible-earliest-deadline queries (paper Appendix B).
 */

#define	RB_RED		0
#define	RB_BLACK	1

struct rb_node {
	struct rb_node	*rb_left;
	struct rb_node	*rb_right;
	struct rb_node	*rb_parent;
	int		rb_color;
};

struct rb_root {
	struct rb_node	*rb_node;
	struct rb_node	*rb_leftmost;
	int		rb_count;
};

#define	RB_ROOT_INITIALIZER	{ NULL, NULL, 0 }

static inline bool
rb_empty(struct rb_root *root)
{
	return (root->rb_node == NULL);
}

/*
 * In-order successor for tree traversal (used by steal).
 */
static struct rb_node *
rb_next(struct rb_node *node)
{
	struct rb_node *parent;

	if (node->rb_right != NULL) {
		node = node->rb_right;
		while (node->rb_left != NULL)
			node = node->rb_left;
		return (node);
	}
	while ((parent = node->rb_parent) != NULL && node == parent->rb_right)
		node = parent;
	return (parent);
}

#define	EEVDF_BASE_SLICE_NS	(3000000LL)	/* 3ms base time slice. */
#define	EEVDF_MIN_GRANULARITY_NS (300000LL)	/* 300us min before preempt. */

#define	EEVDF_NICE_0_WEIGHT	1024
#define	EEVDF_NICE_MIN		(-20)
#define	EEVDF_NICE_MAX		20
#define	EEVDF_NICE_RANGE	(EEVDF_NICE_MAX - EEVDF_NICE_MIN + 1)

#define	EEVDF_DEQUEUE_SLEEP	0x01

/* Flags kept in td_flags. */
#define	TDF_PICKCPU	TDF_SCHED0	/* Thread should pick new CPU. */
#define	TDF_SLICEEND	TDF_SCHED2	/* Thread time slice is over. */

static int sched_quantum __read_mostly = 8;

#ifdef PREEMPTION
#ifdef FULL_PREEMPTION
static int __read_mostly preempt_thresh = PRI_MAX_IDLE;
#else
static int __read_mostly preempt_thresh = PRI_MIN_KERN;
#endif
#else
static int __read_mostly preempt_thresh = 0;
#endif

static const int eevdf_prio_to_weight[EEVDF_NICE_RANGE] = {
 /* -20 */ 88761, 71755, 56483, 46273, 36291,
 /* -15 */ 29154, 23254, 18705, 14949, 11916,
 /* -10 */  9548,  7620,  6100,  4904,  3906,
 /*  -5 */  3121,  2501,  1991,  1586,  1277,
 /*   0 */  1024,   820,   655,   526,   423,
 /*   5 */   335,   272,   215,   172,   137,
 /*  10 */   110,    87,    70,    56,    45,
 /*  15 */    36,    29,    23,    18,    15,
 /*  20 */    20
};

static const uint32_t eevdf_prio_to_wmult[EEVDF_NICE_RANGE] = {
 /* -20 */    48388,    59856,    76040,    92818,   118348,
 /* -15 */   147320,   184698,   229616,   287308,   360437,
 /* -10 */   449829,   563644,   704093,   875809,  1099582,
 /*  -5 */  1376151,  1717300,  2157191,  2708050,  3363326,
 /*   0 */  4194304,  5237765,  6557202,  8165337, 10153587,
 /*   5 */ 12820798, 15790321, 19976592, 24970740, 31350126,
 /*  10 */ 39045157, 49367440, 61356676, 76695844, 95443717,
 /*  15 */119304647,148102320,186737708,238609294,286331153,
 /*  20 */357913941
};

struct td_sched {
	/* EEVDF tree linkage and timeshare scheduling state. */
	struct rb_node	ts_rb_node;
	int64_t			ts_vruntime;
	int64_t			ts_deadline;
	int64_t			ts_min_deadline;
	int64_t			ts_sum_exec;
	int64_t			ts_prev_sum_exec;
	int64_t			ts_vlag;
	int			ts_weight;
	uint32_t		ts_inv_weight;
	int64_t			ts_eevdf_slice;
	int			ts_nice;
	int			ts_on_rq;

	/* Per-thread state used by all scheduling classes. */
	short			ts_flags;
	int			ts_cpu;
	u_int			ts_rltick;
	int			ts_slice;
	u_int			ts_ltick;
	u_int			ts_ftick;
	u_int			ts_ticks;
#ifdef KTR
	char			ts_name[TS_NAME_LEN];
#endif
};

#define	TSF_BOUND	0x0001		/* Thread can not migrate. */
#define	TSF_XFERABLE	0x0002		/* Thread was added as transferable. */

_Static_assert(sizeof(struct thread) + sizeof(struct td_sched) <=
    sizeof(struct thread0_storage),
    "increase struct thread0_storage.t0st_sched size");

#define	ts_to_td(ts)	(&((struct thread *)(ts))[-1])

#define	THREAD_CAN_MIGRATE(td)	((td)->td_pinned == 0)
#define	THREAD_CAN_SCHED(td, cpu)	\
    CPU_ISSET((cpu), &(td)->td_cpuset->cs_mask)

/*
 * EEVDF augmented red-black tree operations
 */

#define	TS_FROM_RB_NODE(node)					\
	__containerof(node, struct td_sched, ts_rb_node)

static inline struct td_sched *
rb_node_to_ts(struct rb_node *node)
{
	if (node == NULL)
		return (NULL);
	return (TS_FROM_RB_NODE(node));
}

static bool
eevdf_rb_update_min_vd(struct td_sched *entity)
{
	int64_t old_min, new_min;
	struct td_sched *left, *right;

	old_min = entity->ts_min_deadline;
	new_min = entity->ts_deadline;

	left = rb_node_to_ts(entity->ts_rb_node.rb_left);
	right = rb_node_to_ts(entity->ts_rb_node.rb_right);

	if (left != NULL && left->ts_min_deadline < new_min)
		new_min = left->ts_min_deadline;
	if (right != NULL && right->ts_min_deadline < new_min)
		new_min = right->ts_min_deadline;

	entity->ts_min_deadline = new_min;
	return (new_min != old_min);
}

static void
eevdf_rb_propagate(struct td_sched *entity, struct rb_root *root)
{
	struct rb_node *node;

	for (node = &entity->ts_rb_node; node != NULL;
	    node = node->rb_parent) {
		if (!eevdf_rb_update_min_vd(TS_FROM_RB_NODE(node)))
			break;
	}
}

/*
 * Rotations with augmentation.
 */

static void
rb_rotate_left(struct rb_root *root, struct rb_node *x)
{
	struct rb_node *y = x->rb_right;

	x->rb_right = y->rb_left;
	if (y->rb_left != NULL)
		y->rb_left->rb_parent = x;
	y->rb_parent = x->rb_parent;
	if (x->rb_parent == NULL)
		root->rb_node = y;
	else if (x == x->rb_parent->rb_left)
		x->rb_parent->rb_left = y;
	else
		x->rb_parent->rb_right = y;
	y->rb_left = x;
	x->rb_parent = y;

	eevdf_rb_update_min_vd(TS_FROM_RB_NODE(x));
	eevdf_rb_update_min_vd(TS_FROM_RB_NODE(y));
}

static void
rb_rotate_right(struct rb_root *root, struct rb_node *y)
{
	struct rb_node *x = y->rb_left;

	y->rb_left = x->rb_right;
	if (x->rb_right != NULL)
		x->rb_right->rb_parent = y;
	x->rb_parent = y->rb_parent;
	if (y->rb_parent == NULL)
		root->rb_node = x;
	else if (y == y->rb_parent->rb_right)
		y->rb_parent->rb_right = x;
	else
		y->rb_parent->rb_left = x;
	x->rb_right = y;
	y->rb_parent = x;

	eevdf_rb_update_min_vd(TS_FROM_RB_NODE(y));
	eevdf_rb_update_min_vd(TS_FROM_RB_NODE(x));
}

/*
 * Insert fixup.
 */

static void
rb_insert_fixup(struct rb_root *root, struct rb_node *node)
{
	struct rb_node *parent, *gparent, *uncle;

	while ((parent = node->rb_parent) != NULL &&
	    parent->rb_color == RB_RED) {
		gparent = parent->rb_parent;
		if (parent == gparent->rb_left) {
			uncle = gparent->rb_right;
			if (uncle != NULL &&
			    uncle->rb_color == RB_RED) {
				parent->rb_color = RB_BLACK;
				uncle->rb_color = RB_BLACK;
				gparent->rb_color = RB_RED;
				node = gparent;
				continue;
			}
			if (node == parent->rb_right) {
				rb_rotate_left(root, parent);
				node = parent;
				parent = node->rb_parent;
			}
			parent->rb_color = RB_BLACK;
			gparent->rb_color = RB_RED;
			rb_rotate_right(root, gparent);
		} else {
			uncle = gparent->rb_left;
			if (uncle != NULL &&
			    uncle->rb_color == RB_RED) {
				parent->rb_color = RB_BLACK;
				uncle->rb_color = RB_BLACK;
				gparent->rb_color = RB_RED;
				node = gparent;
				continue;
			}
			if (node == parent->rb_left) {
				rb_rotate_right(root, parent);
				node = parent;
				parent = node->rb_parent;
			}
			parent->rb_color = RB_BLACK;
			gparent->rb_color = RB_RED;
			rb_rotate_left(root, gparent);
		}
	}
	root->rb_node->rb_color = RB_BLACK;
}

/*
 * Delete fixup.
 */

static void
rb_delete_fixup(struct rb_root *root, struct rb_node *node,
    struct rb_node *parent)
{
	struct rb_node *sibling;

	while ((node == NULL || node->rb_color == RB_BLACK) &&
	    node != root->rb_node) {
		if (node == parent->rb_left) {
			sibling = parent->rb_right;
			if (sibling->rb_color == RB_RED) {
				sibling->rb_color = RB_BLACK;
				parent->rb_color = RB_RED;
				rb_rotate_left(root, parent);
				sibling = parent->rb_right;
			}
			if ((sibling->rb_left == NULL ||
			    sibling->rb_left->rb_color == RB_BLACK) &&
			    (sibling->rb_right == NULL ||
			    sibling->rb_right->rb_color == RB_BLACK)) {
				sibling->rb_color = RB_RED;
				node = parent;
				parent = node->rb_parent;
			} else {
				if (sibling->rb_right == NULL ||
				    sibling->rb_right->rb_color ==
				    RB_BLACK) {
					if (sibling->rb_left != NULL)
						sibling->rb_left->rb_color =
						    RB_BLACK;
					sibling->rb_color = RB_RED;
					rb_rotate_right(root, sibling);
					sibling = parent->rb_right;
				}
				sibling->rb_color = parent->rb_color;
				parent->rb_color = RB_BLACK;
				if (sibling->rb_right != NULL)
					sibling->rb_right->rb_color =
					    RB_BLACK;
				rb_rotate_left(root, parent);
				node = root->rb_node;
				break;
			}
		} else {
			sibling = parent->rb_left;
			if (sibling->rb_color == RB_RED) {
				sibling->rb_color = RB_BLACK;
				parent->rb_color = RB_RED;
				rb_rotate_right(root, parent);
				sibling = parent->rb_left;
			}
			if ((sibling->rb_right == NULL ||
			    sibling->rb_right->rb_color == RB_BLACK) &&
			    (sibling->rb_left == NULL ||
			    sibling->rb_left->rb_color == RB_BLACK)) {
				sibling->rb_color = RB_RED;
				node = parent;
				parent = node->rb_parent;
			} else {
				if (sibling->rb_left == NULL ||
				    sibling->rb_left->rb_color ==
				    RB_BLACK) {
					if (sibling->rb_right != NULL)
						sibling->rb_right->rb_color =
						    RB_BLACK;
					sibling->rb_color = RB_RED;
					rb_rotate_left(root, sibling);
					sibling = parent->rb_left;
				}
				sibling->rb_color = parent->rb_color;
				parent->rb_color = RB_BLACK;
				if (sibling->rb_left != NULL)
					sibling->rb_left->rb_color =
					    RB_BLACK;
				rb_rotate_right(root, parent);
				node = root->rb_node;
				break;
			}
		}
	}
	if (node != NULL)
		node->rb_color = RB_BLACK;
}

/*
 * Public RB-tree API.
 */

static void
eevdf_rb_init(struct rb_root *root)
{
	root->rb_node = NULL;
	root->rb_leftmost = NULL;
	root->rb_count = 0;
}

static void
eevdf_rb_insert(struct rb_root *root, struct td_sched *entity)
{
	struct rb_node **link, *parent;
	struct td_sched *entry;
	bool leftmost;

	link = &root->rb_node;
	parent = NULL;
	leftmost = true;
	entity->ts_min_deadline = entity->ts_deadline;

	while (*link != NULL) {
		parent = *link;
		entry = TS_FROM_RB_NODE(parent);

		if (entry->ts_min_deadline > entity->ts_deadline)
			entry->ts_min_deadline = entity->ts_deadline;

		if (entity->ts_vruntime < entry->ts_vruntime) {
			link = &parent->rb_left;
		} else {
			link = &parent->rb_right;
			leftmost = false;
		}
	}

	entity->ts_rb_node.rb_left = NULL;
	entity->ts_rb_node.rb_right = NULL;
	entity->ts_rb_node.rb_parent = parent;
	entity->ts_rb_node.rb_color = RB_RED;
	*link = &entity->ts_rb_node;

	if (leftmost)
		root->rb_leftmost = &entity->ts_rb_node;
	root->rb_count++;

	rb_insert_fixup(root, &entity->ts_rb_node);
	eevdf_rb_propagate(entity, root);
}

static void
eevdf_rb_remove(struct rb_root *root, struct td_sched *entity)
{
	struct rb_node *node, *child, *parent, *successor;
	struct td_sched *propagate_from;
	int color;

	node = &entity->ts_rb_node;

	/* Update cached leftmost. */
	if (root->rb_leftmost == node) {
		struct rb_node *next;
		if (node->rb_right != NULL) {
			next = node->rb_right;
			while (next->rb_left != NULL)
				next = next->rb_left;
		} else {
			next = node->rb_parent;
		}
		root->rb_leftmost = next;
	}

	if (node->rb_left == NULL || node->rb_right == NULL) {
		child = (node->rb_left != NULL) ?
		    node->rb_left : node->rb_right;
		parent = node->rb_parent;
		color = node->rb_color;

		if (child != NULL)
			child->rb_parent = parent;
		if (parent != NULL) {
			if (parent->rb_left == node)
				parent->rb_left = child;
			else
				parent->rb_right = child;
		} else {
			root->rb_node = child;
		}
		propagate_from = rb_node_to_ts(parent);
	} else {
		successor = node->rb_right;
		while (successor->rb_left != NULL)
			successor = successor->rb_left;

		child = successor->rb_right;
		color = successor->rb_color;

		if (successor->rb_parent == node) {
			parent = successor;
		} else {
			parent = successor->rb_parent;
			if (child != NULL)
				child->rb_parent = parent;
			parent->rb_left = child;
			successor->rb_right = node->rb_right;
			node->rb_right->rb_parent = successor;
		}

		successor->rb_left = node->rb_left;
		node->rb_left->rb_parent = successor;
		successor->rb_parent = node->rb_parent;
		successor->rb_color = node->rb_color;

		if (node->rb_parent != NULL) {
			if (node->rb_parent->rb_left == node)
				node->rb_parent->rb_left = successor;
			else
				node->rb_parent->rb_right = successor;
		} else {
			root->rb_node = successor;
		}
		propagate_from = rb_node_to_ts(parent);
	}

	root->rb_count--;

	if (color == RB_BLACK && root->rb_node != NULL)
		rb_delete_fixup(root, child, parent);
	if (propagate_from != NULL)
		eevdf_rb_propagate(propagate_from, root);

	node->rb_left = NULL;
	node->rb_right = NULL;
	node->rb_parent = NULL;
}

static struct td_sched *
eevdf_rb_first(struct rb_root *root)
{
	if (root->rb_leftmost == NULL)
		return (NULL);
	return (TS_FROM_RB_NODE(root->rb_leftmost));
}

/*
 * Core EEVDF pick: find eligible entity with earliest virtual deadline.
 */
static struct td_sched *
eevdf_rb_pick(struct rb_root *root, int64_t current_vt)
{
	struct rb_node *node;
	struct td_sched *se, *path_best, *st_best, *left_se;

	node = root->rb_node;
	path_best = NULL;
	st_best = NULL;

	while (node != NULL) {
		se = TS_FROM_RB_NODE(node);
		if (se->ts_vruntime <= current_vt) {
			if (path_best == NULL ||
			    se->ts_deadline < path_best->ts_deadline)
				path_best = se;
			left_se = rb_node_to_ts(node->rb_left);
			if (left_se != NULL && (st_best == NULL ||
			    left_se->ts_min_deadline <
			    st_best->ts_min_deadline))
				st_best = left_se;
			node = node->rb_right;
		} else {
			node = node->rb_left;
		}
	}

	if (st_best == NULL || (path_best != NULL &&
	    path_best->ts_deadline <= st_best->ts_min_deadline))
		return (path_best);

	node = &st_best->ts_rb_node;
	while (node != NULL) {
		se = TS_FROM_RB_NODE(node);
		if (se->ts_deadline == st_best->ts_min_deadline)
			return (se);
		left_se = rb_node_to_ts(node->rb_left);
		if (left_se != NULL &&
		    left_se->ts_min_deadline == st_best->ts_min_deadline) {
			node = node->rb_left;
			continue;
		}
		node = node->rb_right;
	}
	return (path_best);
}

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
	struct mtx_padalign tdq_lock;		/* (t) run queue lock. */
	struct cpu_group *tdq_cg;		/* (c) cpu topology. */
	struct thread	*tdq_curthread;		/* (t) current thread. */
	int		tdq_load;		/* (ts) aggregate load. */
	int		tdq_sysload;		/* (ts) !ITHD load. */
	int		tdq_cpu_idle;		/* (ls) cpu_idle() active. */
	int		tdq_transferable;	/* (ts) transferable count. */
	short		tdq_switchcnt;		/* (l) switches this tick. */
	short		tdq_oldswitchcnt;	/* (l) switches last tick. */
	u_char		tdq_lowpri;		/* (ts) lowest pri thread. */
	u_char		tdq_owepreempt;		/* (f) remote preempt pending. */

	/* Priority-bitmap queues for non-timeshare classes. */
	struct runq	tdq_realtime;		/* (t) RT and interrupt. */
	struct runq	tdq_idle;		/* (t) idle-class. */

	/* EEVDF tree for timeshare threads. */
	struct rb_root tdq_eevdf;		/* (t) */
	struct td_sched	*tdq_curr;		/* (t) running ts entity. */

	/* Virtual time tracking. */
	int64_t		tdq_avg_vruntime;	/* (t) */
	int64_t		tdq_total_weight;	/* (t) */
	int64_t		tdq_min_vruntime;	/* (t) */
	int		tdq_nr_timeshare;	/* (t) */

	/* Clock. */
	int64_t		tdq_clock;		/* (t) nanosecond clock. */
	int64_t		tdq_clock_task;		/* (t) */

	/* Per-CPU. */
	int		tdq_id;			/* (c) cpuid. */
	char		tdq_name[TDQ_NAME_LEN];
#ifdef KTR
	char		tdq_loadname[TDQ_LOADNAME_LEN];
#endif
};

/* Idle thread states. */
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
static int __read_mostly rebalance = 1;
static int balance_interval = 128;	/* Default set in sched_initticks(). */
static int __read_mostly affinity;
static int __read_mostly steal_idle = 1;
static int __read_mostly steal_thresh = 2;
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

static void sched_setpreempt(int);
static void sched_thread_priority(struct thread *, u_char);
static uint32_t sched_random(void);
static void tdq_print(int cpu);
static void runq_print(struct runq *rq);

static void eevdf_tree_enqueue(struct tdq *, struct td_sched *);
static void eevdf_tree_dequeue(struct tdq *, struct td_sched *);
static struct thread *tdq_choose(struct tdq *);

static void tdq_setup(struct tdq *, int);
static void tdq_load_add(struct tdq *, struct thread *);
static void tdq_load_rem(struct tdq *, struct thread *);
static inline int sched_shouldpreempt(int, int, int);
static void tdq_setlowpri(struct tdq *, struct thread *);
static int tdq_add(struct tdq *, struct thread *, int);

#ifdef SMP
static int tdq_move(struct tdq *, struct tdq *);
static int tdq_idled(struct tdq *);
static void tdq_notify(struct tdq *, int);
static struct thread *tdq_steal(struct tdq *, int);
static void tdq_trysteal(struct tdq *);
static int sched_pickcpu(struct thread *, int);
static void sched_balance(void);
static bool sched_balance_pair(struct tdq *, struct tdq *);
static inline struct tdq *sched_setcpu(struct thread *, int, int);
#endif
static inline void thread_unblock_switch(struct thread *, struct mtx *);

/* Statistics. */
SCHED_STAT_DEFINE(eevdf_switches, "Context switches");
SCHED_STAT_DEFINE(eevdf_preemptions, "Preemptions");
#ifdef SMP
SCHED_STAT_DEFINE(pickcpu_idle_affinity, "Picked idle cpu based on affinity");
SCHED_STAT_DEFINE(pickcpu_affinity, "Picked cpu based on affinity");
SCHED_STAT_DEFINE(pickcpu_lowest, "Selected lowest load");
SCHED_STAT_DEFINE(pickcpu_local, "Migrated to current cpu");
SCHED_STAT_DEFINE(pickcpu_migration, "Selection may have caused migration");
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
	printf("\tlock:              %p\n", TDQ_LOCKPTR(tdq));
	printf("\tLock name:         %s\n", tdq->tdq_name);
	printf("\tload:              %d\n", tdq->tdq_load);
	printf("\tswitch cnt:        %d\n", tdq->tdq_switchcnt);
	printf("\told switch cnt:    %d\n", tdq->tdq_oldswitchcnt);
	printf("\ttransferrable:     %d\n", tdq->tdq_transferable);
	printf("\tlowest priority:   %d\n", tdq->tdq_lowpri);
	printf("\tnr_timeshare:      %d\n", tdq->tdq_nr_timeshare);
	printf("\tavg_vruntime:      %jd\n", (intmax_t)tdq->tdq_avg_vruntime);
	printf("\tmin_vruntime:      %jd\n", (intmax_t)tdq->tdq_min_vruntime);
	printf("\ttotal_weight:      %jd\n", (intmax_t)tdq->tdq_total_weight);
	printf("\teevdf tree count:  %d\n", tdq->tdq_eevdf.rb_count);
	printf("\trealtime runq:\n");
	runq_print(&tdq->tdq_realtime);
	printf("\tidle runq:\n");
	runq_print(&tdq->tdq_idle);
}

static inline int64_t
calc_delta_fair(int64_t delta, int weight, uint32_t inv_weight)
{
	if (weight == EEVDF_NICE_0_WEIGHT)
		return (delta);
	return ((int64_t)(((uint64_t)delta * EEVDF_NICE_0_WEIGHT *
	    (uint64_t)inv_weight) >> 32));
}

static inline int64_t
tdq_avg_vruntime(struct tdq *tdq)
{
	if (tdq->tdq_total_weight == 0)
		return (tdq->tdq_min_vruntime);
	return (tdq->tdq_avg_vruntime / tdq->tdq_total_weight);
}

static inline bool
entity_eligible(struct tdq *tdq, struct td_sched *ts)
{
	return (ts->ts_vruntime <= tdq_avg_vruntime(tdq));
}

static inline void
entity_update_deadline(struct td_sched *ts)
{
	ts->ts_deadline = ts->ts_vruntime +
	    calc_delta_fair(ts->ts_eevdf_slice, ts->ts_weight,
	    ts->ts_inv_weight);
}

static inline void
entity_set_weight(struct td_sched *ts, int nice)
{
	int idx;

	idx = nice - EEVDF_NICE_MIN;
	if (idx < 0) idx = 0;
	if (idx >= EEVDF_NICE_RANGE) idx = EEVDF_NICE_RANGE - 1;
	ts->ts_nice = nice;
	ts->ts_weight = eevdf_prio_to_weight[idx];
	ts->ts_inv_weight = eevdf_prio_to_wmult[idx];
}

static int
nice_to_pri(int nice)
{
	return (PRI_MIN_TIMESHARE +
	    (nice - EEVDF_NICE_MIN) *
	    (PRI_MAX_TIMESHARE - PRI_MIN_TIMESHARE) /
	    (EEVDF_NICE_RANGE - 1));
}

static int
pri_to_nice(int pri)
{
	if (pri <= PRI_MIN_TIMESHARE) return (EEVDF_NICE_MIN);
	if (pri >= PRI_MAX_TIMESHARE) return (EEVDF_NICE_MAX);
	return (EEVDF_NICE_MIN +
	    (pri - PRI_MIN_TIMESHARE) * (EEVDF_NICE_RANGE - 1) /
	    (PRI_MAX_TIMESHARE - PRI_MIN_TIMESHARE));
}

/*
 * EEVDF tree add/remove with weighted average maintenance
 *
 * These handle only the EEVDF tree operations and timeshare
 * counters.  Aggregate load tracking is done by tdq_load_add/rem.
 */

static void
avg_vruntime_add(struct tdq *tdq, struct td_sched *ts)
{
	tdq->tdq_avg_vruntime += (int64_t)ts->ts_weight * ts->ts_vruntime;
	tdq->tdq_total_weight += ts->ts_weight;
}

static void
avg_vruntime_sub(struct tdq *tdq, struct td_sched *ts)
{
	tdq->tdq_avg_vruntime -= (int64_t)ts->ts_weight * ts->ts_vruntime;
	tdq->tdq_total_weight -= ts->ts_weight;
}

static void
update_min_vruntime(struct tdq *tdq)
{
	int64_t vrt;

	vrt = tdq_avg_vruntime(tdq);
	if (vrt > tdq->tdq_min_vruntime)
		tdq->tdq_min_vruntime = vrt;
}

/*
 * Add a timeshare entity to the EEVDF tree.
 */
static void
eevdf_tree_enqueue(struct tdq *tdq, struct td_sched *ts)
{

	KASSERT(!ts->ts_on_rq, ("eevdf_tree_enqueue: already on rq"));

	if (ts->ts_deadline <= ts->ts_vruntime)
		entity_update_deadline(ts);

	ts->ts_on_rq = 1;
	avg_vruntime_add(tdq, ts);
	eevdf_rb_insert(&tdq->tdq_eevdf, ts);
	tdq->tdq_nr_timeshare++;
	if (THREAD_CAN_MIGRATE(ts_to_td(ts))) {
		tdq->tdq_transferable++;
		ts->ts_flags |= TSF_XFERABLE;
	}
	update_min_vruntime(tdq);
}

/*
 * Remove a timeshare entity from the EEVDF tree.
 */
static void
eevdf_tree_dequeue(struct tdq *tdq, struct td_sched *ts)
{

	KASSERT(ts->ts_on_rq, ("eevdf_tree_dequeue: not on rq"));

	ts->ts_on_rq = 0;
	avg_vruntime_sub(tdq, ts);
	eevdf_rb_remove(&tdq->tdq_eevdf, ts);
	tdq->tdq_nr_timeshare--;
	if (ts->ts_flags & TSF_XFERABLE) {
		tdq->tdq_transferable--;
		ts->ts_flags &= ~TSF_XFERABLE;
	}
	update_min_vruntime(tdq);
}

/*
 * Set the load for the thread to the referenced thread queue.
 */
static void
tdq_load_add(struct tdq *tdq, struct thread *td)
{

	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
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
	KASSERT(tdq->tdq_load != 0,
	    ("tdq_load_rem: 0 load on queue %d", TDQ_ID(tdq)));
	tdq->tdq_load--;
	if ((td->td_flags & TDF_NOLOAD) == 0)
		tdq->tdq_sysload--;
	KTR_COUNTER0(KTR_SCHED, "load", tdq->tdq_loadname, tdq->tdq_load);
	SDT_PROBE2(sched, , , load__change, (int)TDQ_ID(tdq), tdq->tdq_load);
}

/*
 * Set lowpri to its exact value by searching the run-queue and
 * comparing against the current thread.
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

/*
 * Add a thread to its appropriate queue and update load.
 * Returns the previous lowpri value (for IPI decisions).
 */
static int
tdq_add(struct tdq *tdq, struct thread *td, int flags)
{
	struct td_sched *ts;
	int lowpri;

	TDQ_LOCK_ASSERT(tdq, MA_OWNED);

	lowpri = tdq->tdq_lowpri;
	if (td->td_priority < lowpri)
		tdq->tdq_lowpri = td->td_priority;

	ts = td_get_sched(td);
	if (PRI_BASE(td->td_pri_class) == PRI_TIMESHARE) {
		if (!ts->ts_on_rq)
			eevdf_tree_enqueue(tdq, ts);
		TD_SET_RUNQ(td);
	} else if (PRI_BASE(td->td_pri_class) == PRI_REALTIME ||
	    td->td_pri_class == PRI_ITHD) {
		runq_add(&tdq->tdq_realtime, td, flags);
	} else {
		runq_add(&tdq->tdq_idle, td, flags);
	}

	tdq_load_add(tdq, td);
	return (lowpri);
}

static inline int
sched_shouldpreempt(int pri, int cpri, int remote)
{
	if (pri >= cpri)
		return (0);
	if (cpri >= PRI_MIN_IDLE)
		return (1);
	if (preempt_thresh == 0)
		return (0);
	if (pri <= preempt_thresh)
		return (1);
	return (0);
}

/*
 * Set owepreempt if the currently running thread has lower priority than
 * "pri".
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
	if (!sched_shouldpreempt(pri, cpri, 0))
		return;
	ctd->td_owepreempt = 1;
}

#ifdef SMP

/*
 * We need some randomness.  Implement a classic Linear Congruential
 * Generator X_{n+1} = (aX_n + c) mod m.
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
	cpuset_t *cs_mask;
	int	cs_prefer;
	int	cs_running;
	int	cs_pri;
	int	cs_load;
	int	cs_trans;
};

struct cpu_search_res {
	int	csr_cpu;
	int	csr_load;
};

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

	if (cg->cg_children > 0) {
		for (c = cg->cg_children - 1; c >= 0; c--) {
			load = cpu_search_lowest(&cg->cg_child[c], s, &lr);
			total += load;
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
		if (l > s->cs_load ||
		    (atomic_load_char(&tdq->tdq_lowpri) <= s->cs_pri &&
		     (!s->cs_running || c != s->cs_prefer)) ||
		    !CPU_ISSET(c, s->cs_mask))
			continue;
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

	for (c = cg->cg_last; c >= cg->cg_first; c--) {
		if (!CPU_ISSET(c, &cg->cg_mask))
			continue;
		tdq = TDQ_CPU(c);
		l = TDQ_LOAD(tdq);
		load = l * 256;
		total += load;
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
 * lowpri greater than pri pri.  A pri of -1 indicates any thread is ok.
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
 * Steal a thread from a remote EEVDF tree that can run on 'cpu'.
 * Walk in-order from leftmost (most deserving) to find a migratable one.
 * Also checks RT and idle runqs via runq_choose.
 */
static struct thread *
tdq_steal(struct tdq *tdq, int cpu)
{
	struct rb_node *node;
	struct td_sched *ts;
	struct thread *td;

	TDQ_LOCK_ASSERT(tdq, MA_OWNED);

	/* Try the EEVDF tree first (timeshare). */
	for (node = tdq->tdq_eevdf.rb_leftmost; node != NULL;
	    node = rb_next(node)) {
		ts = TS_FROM_RB_NODE(node);
		td = ts_to_td(ts);
		if (THREAD_CAN_MIGRATE(td) && THREAD_CAN_SCHED(td, cpu))
			return (td);
	}

	/* Try RT queue. */
	/* Note: not implementing runq_steal for RT/idle for simplicity;
	 * timeshare threads are the primary balancing target. */
	return (NULL);
}

/*
 * Move a thread from one queue to another.  Returns -1 if no thread
 * found, else returns the previous lowpri of the destination.
 */
static int
tdq_move(struct tdq *from, struct tdq *to)
{
	struct thread *td;
	struct td_sched *ts;
	int cpu;

	TDQ_LOCK_ASSERT(from, MA_OWNED);
	TDQ_LOCK_ASSERT(to, MA_OWNED);

	cpu = TDQ_ID(to);
	td = tdq_steal(from, cpu);
	if (td == NULL)
		return (-1);

	ts = td_get_sched(td);

	/*
	 * Wait for the thread lock to become unblocked, then migrate.
	 */
	thread_lock_block_wait(td);
	sched_rem(td);
	THREAD_LOCKPTR_ASSERT(td, TDQ_LOCKPTR(from));

	/*
	 * Adjust vruntime for destination queue to preserve fairness.
	 */
	if (PRI_BASE(td->td_pri_class) == PRI_TIMESHARE) {
		int64_t vlag;
		vlag = tdq_avg_vruntime(from) - ts->ts_vruntime;
		ts->ts_vlag = vlag;
		ts->ts_vruntime = to->tdq_min_vruntime - ts->ts_vlag;
		entity_update_deadline(ts);
	}

	td->td_lock = TDQ_LOCKPTR(to);
	ts->ts_cpu = cpu;
	return (tdq_add(to, td, SRQ_YIELDING));
}

/*
 * Topology-aware load balancing within a CPU group.
 */
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
		if (high == -1)
			break;
		CPU_CLR(high, &hmask);
		CPU_COPY(&hmask, &lmask);
		if (CPU_EMPTY(&lmask))
			break;
		tdq = TDQ_CPU(high);
		if (TDQ_LOAD(tdq) == 1) {
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
		low = sched_lowest(cg, &lmask, -1, TDQ_LOAD(tdq) - 1,
		    high, 1);
		if (anylow && low == -1)
			break;
		if (low == -1)
			continue;
		if (sched_balance_pair(tdq, TDQ_CPU(low))) {
			CPU_CLR(low, &hmask);
		} else {
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
 * Transfer load between two imbalanced thread queues.  Returns true if a
 * thread was moved.
 */
static bool
sched_balance_pair(struct tdq *high, struct tdq *low)
{
	int cpu, lowpri;
	bool ret;

	ret = false;
	tdq_lock_pair(high, low);
	if (high->tdq_transferable != 0 && high->tdq_load > low->tdq_load) {
		lowpri = tdq_move(high, low);
		if (lowpri != -1) {
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
 * Notify a remote cpu of new work.  Sends an IPI if criteria are met.
 */
static void
tdq_notify(struct tdq *tdq, int lowpri)
{
	int cpu;

	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	KASSERT(tdq->tdq_lowpri <= lowpri,
	    ("tdq_notify: lowpri %d > tdq_lowpri %d", lowpri,
	    tdq->tdq_lowpri));

	if (tdq->tdq_owepreempt)
		return;
	if (!sched_shouldpreempt(tdq->tdq_lowpri, lowpri, 1))
		return;

	atomic_thread_fence_seq_cst();

	cpu = TDQ_ID(tdq);
	if (TD_IS_IDLETHREAD(tdq->tdq_curthread) &&
	    (atomic_load_int(&tdq->tdq_cpu_idle) == 0 ||
	    cpu_idle_wakeup(cpu)))
		return;

	tdq->tdq_owepreempt = 1;
	ipi_cpu(cpu, IPI_PREEMPT);
}

/*
 * Idle thread work stealing using topology.
 */
static int
tdq_idled(struct tdq *tdq)
{
	struct cpu_group *cg, *parent;
	struct tdq *steal;
	cpuset_t mask;
	int cpu, switchcnt, goup;

	if (smp_started == 0 || steal_idle == 0 || tdq->tdq_cg == NULL)
		return (1);
	CPU_FILL(&mask);
	CPU_CLR(PCPU_GET(cpuid), &mask);
restart:
	switchcnt = TDQ_SWITCHCNT(tdq);
	for (cg = tdq->tdq_cg, goup = 0; ; ) {
		cpu = sched_highest(cg, &mask, steal_thresh, 1);
		if (TDQ_LOAD(tdq))
			return (0);
		if (cpu == -1) {
			if (goup) {
				cg = cg->cg_parent;
				goup = 0;
			}
			parent = cg->cg_parent;
			if (parent == NULL)
				return (1);
			if (parent->cg_children == 2) {
				if (cg == &parent->cg_child[0])
					cg = &parent->cg_child[1];
				else
					cg = &parent->cg_child[0];
				goup = 1;
			} else
				cg = parent;
			continue;
		}
		steal = TDQ_CPU(cpu);
		if (TDQ_LOAD(steal) < steal_thresh ||
		    TDQ_TRANSFERABLE(steal) == 0)
			goto restart;
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
		if (TDQ_LOAD(steal) < steal_thresh ||
		    TDQ_TRANSFERABLE(steal) == 0 ||
		    switchcnt != TDQ_SWITCHCNT(tdq)) {
			tdq_unlock_pair(tdq, steal);
			goto restart;
		}
		if (tdq_move(steal, tdq) != -1)
			break;
		CPU_CLR(cpu, &mask);
		tdq_unlock_pair(tdq, steal);
	}
	TDQ_UNLOCK(steal);
	mi_switch(SW_VOL | SWT_IDLE);
	return (0);
}

/*
 * Try to steal work before going idle in sched_switch.
 */
static void
tdq_trysteal(struct tdq *tdq)
{
	struct cpu_group *cg, *parent;
	struct tdq *steal;
	cpuset_t mask;
	int cpu, i, goup;

	if (smp_started == 0 || steal_idle == 0 || trysteal_limit == 0 ||
	    tdq->tdq_cg == NULL)
		return;
	CPU_FILL(&mask);
	CPU_CLR(PCPU_GET(cpuid), &mask);
	spinlock_enter();
	TDQ_UNLOCK(tdq);
	for (i = 1, cg = tdq->tdq_cg, goup = 0; ; ) {
		cpu = sched_highest(cg, &mask, steal_thresh, 1);
		if (TDQ_LOAD(tdq) > 0) {
			TDQ_LOCK(tdq);
			break;
		}
		if (cpu == -1) {
			if (goup) {
				cg = cg->cg_parent;
				goup = 0;
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
				goup = 1;
			} else
				cg = parent;
			continue;
		}
		steal = TDQ_CPU(cpu);
		if (TDQ_LOAD(steal) < steal_thresh ||
		    TDQ_TRANSFERABLE(steal) == 0)
			continue;
		TDQ_LOCK(tdq);
		if (tdq->tdq_load > 0)
			break;
		if (TDQ_TRYLOCK_FLAGS(steal, MTX_DUPOK) == 0)
			break;
		if (TDQ_LOAD(steal) < steal_thresh ||
		    TDQ_TRANSFERABLE(steal) == 0) {
			TDQ_UNLOCK(steal);
			break;
		}
		if (tdq_move(steal, tdq) == -1) {
			TDQ_UNLOCK(steal);
			break;
		}
		TDQ_UNLOCK(steal);
		break;
	}
	spinlock_exit();
}

/*
 * Set the thread lock and ts_cpu to match the requested cpu.
 * Unlocks the current lock and returns with the assigned queue locked.
 */
static inline struct tdq *
sched_setcpu(struct thread *td, int cpu, int flags)
{
	struct tdq *tdq;
	struct mtx *mtx;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	tdq = TDQ_CPU(cpu);
	td_get_sched(td)->ts_cpu = cpu;

	if (td->td_lock == TDQ_LOCKPTR(tdq)) {
		KASSERT((flags & SRQ_HOLD) == 0,
		    ("sched_setcpu: Invalid lock for SRQ_HOLD"));
		return (tdq);
	}

	spinlock_enter();
	mtx = thread_lock_block(td);
	if ((flags & SRQ_HOLD) == 0)
		mtx_unlock_spin(mtx);
	TDQ_LOCK(tdq);
	thread_lock_unblock(td, TDQ_LOCKPTR(tdq));
	spinlock_exit();
	return (tdq);
}

static int
sched_pickcpu(struct thread *td, int flags)
{
	struct cpu_group *cg, *ccg;
	struct td_sched *ts;
	struct tdq *tdq;
	cpuset_t *mask;
	int cpu, pri, r, self;

	self = PCPU_GET(cpuid);
	ts = td_get_sched(td);
	KASSERT(!CPU_ABSENT(ts->ts_cpu), ("sched_pickcpu: absent CPU %d",
	    ts->ts_cpu));
	if (smp_started == 0)
		return (self);
	if ((flags & SRQ_OURSELF) || !THREAD_CAN_MIGRATE(td))
		return (ts->ts_cpu);

	/*
	 * Prefer to run interrupt threads on the generating processor.
	 */
	if (td->td_priority <= PRI_MAX_ITHD && THREAD_CAN_SCHED(td, self) &&
	    curthread->td_intr_nesting_level) {
		tdq = TDQ_SELF();
		if (tdq->tdq_lowpri >= PRI_MIN_IDLE) {
			SCHED_STAT_INC(pickcpu_idle_affinity);
			return (self);
		}
		ts->ts_cpu = self;
		cg = tdq->tdq_cg;
		goto llc;
	}

	tdq = TDQ_CPU(ts->ts_cpu);
	cg = tdq->tdq_cg;

	/*
	 * If the thread can run on the last cpu and it is idle, run there.
	 */
	if (THREAD_CAN_SCHED(td, ts->ts_cpu) &&
	    atomic_load_char(&tdq->tdq_lowpri) >= PRI_MIN_IDLE &&
	    SCHED_AFFINITY(ts, CG_SHARE_L2)) {
		if (cg->cg_flags & CG_FLAG_THREAD) {
			for (cpu = cg->cg_first; cpu <= cg->cg_last; cpu++) {
				pri = atomic_load_char(
				    &TDQ_CPU(cpu)->tdq_lowpri);
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
	for (ccg = NULL; cg != NULL; cg = cg->cg_parent) {
		if (cg->cg_flags & CG_FLAG_THREAD)
			continue;
		if (cg->cg_children == 1 || cg->cg_count == 1)
			continue;
		if (cg->cg_level == CG_SHARE_NONE ||
		    !SCHED_AFFINITY(ts, cg->cg_level))
			continue;
		ccg = cg;
	}
	if (ccg == cpu_top)
		ccg = NULL;
	cpu = -1;
	mask = &td->td_cpuset->cs_mask;
	pri = td->td_priority;
	r = TD_IS_RUNNING(td);

	if (ccg != NULL) {
		cpu = sched_lowest(ccg, mask, max(pri, PRI_MAX_TIMESHARE),
		    INT_MAX, ts->ts_cpu, r);
		if (cpu >= 0)
			SCHED_STAT_INC(pickcpu_affinity);
	}
	if (cpu < 0) {
		cpu = sched_lowest(cpu_top, mask, pri, INT_MAX, ts->ts_cpu, r);
		if (cpu >= 0)
			SCHED_STAT_INC(pickcpu_lowest);
	}
	if (cpu < 0) {
		cpu = sched_lowest(cpu_top, mask, -1, INT_MAX, ts->ts_cpu, r);
		if (cpu >= 0)
			SCHED_STAT_INC(pickcpu_lowest);
	}
	KASSERT(cpu >= 0, ("sched_pickcpu: Failed to find a cpu."));

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

/*
 * Handle migration from sched_switch().
 */
static struct mtx *
sched_switch_migrate(struct tdq *tdq, struct thread *td, int flags)
{
	struct tdq *tdn;
	int lowpri;

	KASSERT(THREAD_CAN_MIGRATE(td) ||
	    (td_get_sched(td)->ts_flags & TSF_BOUND) != 0,
	    ("Thread %p shouldn't migrate", td));
	tdn = TDQ_CPU(td_get_sched(td)->ts_cpu);

	tdq_load_rem(tdq, td);
	TDQ_UNLOCK(tdq);
	TDQ_LOCK(tdn);
	lowpri = tdq_add(tdn, td, flags);
	tdq_notify(tdn, lowpri);
	TDQ_UNLOCK(tdn);
	TDQ_LOCK(tdq);
	return (TDQ_LOCKPTR(tdn));
}

#endif /* SMP */

static void
update_clock(struct tdq *tdq)
{
	struct bintime bt;
	int64_t now;

	binuptime(&bt);
	now = (int64_t)bt.sec * 1000000000LL +
	    (int64_t)(((uint64_t)bt.frac >> 32) * 1000000000ULL >> 32);
	if (now > tdq->tdq_clock)
		tdq->tdq_clock = now;
}

static void
update_curr(struct tdq *tdq)
{
	struct td_sched *curr;
	int64_t delta, delta_fair;

	curr = tdq->tdq_curr;
	if (curr == NULL)
		return;

	delta = tdq->tdq_clock - tdq->tdq_clock_task;
	if (delta <= 0)
		return;

	tdq->tdq_clock_task = tdq->tdq_clock;
	curr->ts_sum_exec += delta;

	delta_fair = calc_delta_fair(delta, curr->ts_weight,
	    curr->ts_inv_weight);
	curr->ts_vruntime += delta_fair;

	if (curr->ts_on_rq)
		tdq->tdq_avg_vruntime += (int64_t)curr->ts_weight * delta_fair;

	update_min_vruntime(tdq);
}

static void
place_entity(struct tdq *tdq, struct td_sched *ts, int initial)
{
	int64_t vruntime;

	vruntime = tdq->tdq_min_vruntime;

	if (!initial && ts->ts_vlag != 0) {
		int64_t lag_based, thresh;

		lag_based = tdq_avg_vruntime(tdq) - ts->ts_vlag;
		thresh = calc_delta_fair(ts->ts_eevdf_slice * 2,
		    ts->ts_weight, ts->ts_inv_weight);
		if (lag_based < vruntime - thresh)
			lag_based = vruntime - thresh;
		if (lag_based > vruntime)
			ts->ts_vruntime = lag_based;
		else
			ts->ts_vruntime = vruntime;
	} else {
		ts->ts_vruntime = vruntime;
	}

	entity_update_deadline(ts);
}

static struct td_sched *
pick_eevdf(struct tdq *tdq)
{
	struct td_sched *ts;
	int64_t avg_vrt;

	if (tdq->tdq_nr_timeshare == 0)
		return (NULL);

	avg_vrt = tdq_avg_vruntime(tdq);
	ts = eevdf_rb_pick(&tdq->tdq_eevdf, avg_vrt);
	if (ts == NULL)
		ts = eevdf_rb_first(&tdq->tdq_eevdf);
	return (ts);
}

static struct thread *
tdq_choose(struct tdq *tdq)
{
	struct thread *td;

	TDQ_LOCK_ASSERT(tdq, MA_OWNED);

	/* 1. Realtime / interrupt threads. */
	td = runq_choose(&tdq->tdq_realtime);
	if (td != NULL)
		return (td);

	/* 2. Timeshare via EEVDF. */
	if (tdq->tdq_nr_timeshare > 0) {
		struct td_sched *ts = pick_eevdf(tdq);
		if (ts != NULL)
			return (ts_to_td(ts));
	}

	/* 3. Idle threads. */
	td = runq_choose(&tdq->tdq_idle);
	return (td);
}

/*
 * Check if new_td should preempt the current timeshare thread.
 */
static bool
check_preempt_eevdf(struct tdq *tdq, struct td_sched *new_ts)
{
	struct td_sched *curr;

	curr = tdq->tdq_curr;
	if (curr == NULL)
		return (true);
	if (curr->ts_vruntime >= curr->ts_deadline)
		return (true);
	if (entity_eligible(tdq, new_ts) &&
	    new_ts->ts_deadline < curr->ts_deadline)
		return (true);
	return (false);
}

/*
 * Adjust the priority of a thread.  Move it to the appropriate run-queue
 * if necessary.
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

	if (TD_ON_RUNQ(td) && prio < td->td_priority) {
		sched_rem(td);
		td->td_priority = prio;
		sched_add(td, SRQ_BORROWING | SRQ_HOLDTD);
		return;
	}
	if (TD_IS_RUNNING(td)) {
		tdq = TDQ_CPU(td_get_sched(td)->ts_cpu);
		oldpri = td->td_priority;
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
 * Initialize a thread queue.
 */
static void
tdq_setup(struct tdq *tdq, int id)
{

	if (bootverbose)
		printf("EEVDF: setup cpu %d\n", id);
	runq_init(&tdq->tdq_realtime);
	runq_init(&tdq->tdq_idle);
	eevdf_rb_init(&tdq->tdq_eevdf);
	tdq->tdq_id = id;
	snprintf(tdq->tdq_name, sizeof(tdq->tdq_name),
	    "sched lock %d", (int)TDQ_ID(tdq));
	mtx_init(&tdq->tdq_lock, tdq->tdq_name, "sched lock", MTX_SPIN);
#ifdef KTR
	snprintf(tdq->tdq_loadname, sizeof(tdq->tdq_loadname),
	    "CPU %d load", (int)TDQ_ID(tdq));
#endif
}

/*
 * Called from proc0_init() to setup the scheduler fields.
 */
static void
sched_eevdf_init(void)
{
	struct td_sched *ts0;

	ts0 = td_get_sched(&thread0);
	memset(ts0, 0, sizeof(*ts0));
	ts0->ts_eevdf_slice = EEVDF_BASE_SLICE_NS;
	entity_set_weight(ts0, 0);
	ts0->ts_ftick = (u_int)ticks;
	ts0->ts_ltick = ts0->ts_ftick;
	ts0->ts_slice = 0;
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
sched_eevdf_setup(void)
{
	struct tdq *tdq;

#ifdef SMP
	sched_setup_smp();
#else
	tdq_setup(TDQ_SELF(), 0);
#endif
	tdq = TDQ_SELF();

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
sched_eevdf_initticks(void)
{
#ifdef SMP
	balance_interval = max(1, hz);
	balance_ticks = balance_interval;
	affinity = SCHED_AFFINITY_DEFAULT;
#endif
}

/*
 * schedinit_ap() is needed prior to calling sched_throw(NULL) to ensure that
 * the correct sched is returned by tdq_self().
 */
static void
sched_eevdf_init_ap(void)
{
#ifdef SMP
	PCPU_SET(sched, DPCPU_PTR(tdq));
#endif
	PCPU_GET(idlethread)->td_lock = TDQ_LOCKPTR(TDQ_SELF());
}

static void
sched_eevdf_schedcpu(void)
{
	/* ULE does nothing here; balancing is triggered from sched_clock. */
}

/*
 * Return the total system load.
 */
static int
sched_eevdf_load(void)
{
#ifdef SMP
	int total, i;

	total = 0;
	CPU_FOREACH(i)
		total += atomic_load_int(&TDQ_CPU(i)->tdq_sysload);
	return (total);
#else
	return (atomic_load_int(&TDQ_SELF()->tdq_sysload));
#endif
}

static int
sched_eevdf_rr_interval(void)
{
	return (sched_quantum);
}

/*
 * Return whether the current CPU has runnable tasks.  Used for in-kernel
 * cooperative idle threads.
 */
static bool
sched_eevdf_runnable(void)
{
	struct tdq *tdq;

	tdq = TDQ_SELF();
	return (TDQ_LOAD(tdq) > (TD_IS_IDLETHREAD(curthread) ? 0 : 1));
}

/*
 * Return some of the child's priority and interactivity to the parent.
 */
static void
sched_eevdf_exit(struct proc *p, struct thread *childtd)
{
	struct thread *td;

	KTR_STATE1(KTR_SCHED, "thread", sched_tdname(childtd), "proc exit",
	    "prio:%d", childtd->td_priority);
	PROC_LOCK_ASSERT(p, MA_OWNED);
	td = FIRST_THREAD_IN_PROC(p);
	sched_exit_thread(td, childtd);
}

/*
 * Penalize the parent for creating a new child and initialize the child's
 * priority.
 */
static void
sched_eevdf_fork(struct thread *td, struct thread *childtd)
{
	THREAD_LOCK_ASSERT(td, MA_OWNED);
	sched_fork_thread(td, childtd);
}

/*
 * This is called from fork_exit().  Just acquire the correct locks and
 * let it continue on.
 */
static void
sched_eevdf_fork_exit(struct thread *td)
{
	struct tdq *tdq;
	int cpuid;

	KASSERT(curthread->td_md.md_spinlock_count == 1,
	    ("invalid count %d", curthread->td_md.md_spinlock_count));
	cpuid = PCPU_GET(cpuid);
	tdq = TDQ_SELF();
	TDQ_LOCK(tdq);
	spinlock_exit();
	MPASS(td->td_lock == TDQ_LOCKPTR(tdq));
	td->td_oncpu = cpuid;

	/* Set up EEVDF state for the new thread. */
	{
		struct td_sched *ts = td_get_sched(td);
		tdq->tdq_curr = ts;
		tdq->tdq_clock_task = tdq->tdq_clock;
		ts->ts_prev_sum_exec = ts->ts_sum_exec;
	}

	KTR_STATE1(KTR_SCHED, "thread", sched_tdname(td), "running",
	    "prio:%d", td->td_priority);
	SDT_PROBE0(sched, , , on__cpu);
}

/*
 * Adjust the priority class of a thread.
 */
static void
sched_eevdf_class(struct thread *td, int class)
{
	THREAD_LOCK_ASSERT(td, MA_OWNED);
	if (td->td_pri_class == class)
		return;
	td->td_pri_class = class;
}

/*
 * Adjust thread priorities as a result of a nice request.
 */
static void
sched_eevdf_nice(struct proc *p, int nice)
{
	struct thread *td;
	struct td_sched *ts;
	struct tdq *tdq;

	PROC_LOCK_ASSERT(p, MA_OWNED);

	p->p_nice = nice;
	FOREACH_THREAD_IN_PROC(p, td) {
		thread_lock(td);
		ts = td_get_sched(td);
		tdq = TDQ_CPU(ts->ts_cpu);

		if (ts->ts_on_rq) {
			eevdf_tree_dequeue(tdq, ts);
			entity_set_weight(ts, nice);
			place_entity(tdq, ts, 0);
			eevdf_tree_enqueue(tdq, ts);
		} else {
			entity_set_weight(ts, nice);
		}
		td->td_base_pri = nice_to_pri(nice);
		td->td_base_user_pri = td->td_base_pri;
		td->td_user_pri = td->td_base_pri;
		sched_thread_priority(td, td->td_base_pri);
		thread_unlock(td);
	}
}

/*
 * A CPU is entering for the first time.
 */
static void
sched_eevdf_ap_entry(void)
{
	struct thread *newtd;
	struct tdq *tdq;

	tdq = TDQ_SELF();
	THREAD_LOCKPTR_ASSERT(curthread, TDQ_LOCKPTR(tdq));

	TDQ_LOCK(tdq);
	spinlock_exit();
	PCPU_SET(switchtime, cpu_ticks());
	PCPU_SET(switchticks, ticks);

	newtd = choosethread();
	spinlock_enter();
	TDQ_UNLOCK(tdq);

#ifdef HWT_HOOKS
	hwt_hook_switch_in(newtd);
#endif

	/* doesn't return */
	cpu_throw(NULL, newtd);
}

/*
 * Penalize another thread for the time spent on this one.  This helps to
 * worsen the priority of processes that are hogs.
 */
static void
sched_eevdf_exit_thread(struct thread *td, struct thread *child)
{
	KTR_STATE1(KTR_SCHED, "thread", sched_tdname(child), "thread exit",
	    "prio:%d", child->td_priority);
}

static u_int
sched_eevdf_estcpu(struct thread *td __unused)
{
	return (0);
}

/*
 * Fork a new thread, may be within the same process.
 */
static void
sched_eevdf_fork_thread(struct thread *td, struct thread *child)
{
	struct td_sched *ts, *ts2;
	struct tdq *tdq;

	tdq = TDQ_SELF();
	THREAD_LOCK_ASSERT(td, MA_OWNED);

	ts = td_get_sched(td);
	ts2 = td_get_sched(child);
	child->td_oncpu = NOCPU;
	child->td_lastcpu = NOCPU;
	child->td_lock = TDQ_LOCKPTR(tdq);
	child->td_cpuset = cpuset_ref(td->td_cpuset);
	child->td_domain.dr_policy = td->td_cpuset->cs_domain;

	memset(ts2, 0, sizeof(*ts2));
	ts2->ts_eevdf_slice = EEVDF_BASE_SLICE_NS;
	entity_set_weight(ts2, ts->ts_nice);
	ts2->ts_vruntime = ts->ts_vruntime;
	ts2->ts_cpu = ts->ts_cpu;
	ts2->ts_flags = 0;
	ts2->ts_ticks = ts->ts_ticks;
	ts2->ts_ltick = ts->ts_ltick;
	ts2->ts_ftick = ts->ts_ftick;
	ts2->ts_slice = sched_quantum;

	child->td_priority = child->td_base_pri;
#ifdef KTR
	bzero(ts2->ts_name, sizeof(ts2->ts_name));
#endif
}

/*
 * Set the base interrupt thread priority.
 */
static void
sched_eevdf_ithread_prio(struct thread *td, u_char prio)
{
	THREAD_LOCK_ASSERT(td, MA_OWNED);
	MPASS(td->td_pri_class == PRI_ITHD);
	td->td_base_ithread_pri = prio;
	sched_prio(td, prio);
}

/*
 * Update a thread's priority when it is lent another thread's
 * priority.
 */
static void
sched_eevdf_lend_prio(struct thread *td, u_char prio)
{
	td->td_flags |= TDF_BORROWING;
	sched_thread_priority(td, prio);
}

/*
 * Restore a thread's priority when priority propagation is
 * over.  The prio argument is the minimum priority among all
 * donors.
 */
static void
sched_eevdf_unlend_prio(struct thread *td, u_char prio)
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

static void
sched_eevdf_lend_user_prio(struct thread *td, u_char prio)
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
sched_eevdf_lend_user_prio_cond(struct thread *td, u_char prio)
{
	if (td->td_lend_user_pri == prio)
		return;
	thread_lock(td);
	sched_lend_user_prio(td, prio);
	thread_unlock(td);
}

/*
 * Fetch cpu utilization information.  Updates on demand.
 */
static fixpt_t
sched_eevdf_pctcpu(struct thread *td)
{
	struct td_sched *ts;
	fixpt_t pctcpu;
	u_int t, delta;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	ts = td_get_sched(td);

	/* Update ticks if running. */
	if (TD_IS_RUNNING(td)) {
		t = (u_int)ticks;
		delta = t - ts->ts_ltick;
		ts->ts_ticks += delta;
		ts->ts_ltick = t;
	}

	if (ts->ts_ticks == 0)
		return (0);

	pctcpu = (fixpt_t)min((int64_t)ts->ts_ticks * FSCALE / hz,
	    (int64_t)FSCALE);
	return (pctcpu);
}

/*
 * Standard entry for setting the priority to an absolute value.
 */
static void
sched_eevdf_prio(struct thread *td, u_char prio)
{
	u_char oldprio;

	td->td_base_pri = prio;
	if (td->td_flags & TDF_BORROWING && td->td_priority < prio)
		return;
	oldprio = td->td_priority;
	sched_thread_priority(td, prio);
	if (TD_ON_LOCK(td) && oldprio != prio)
		turnstile_adjust(td, oldprio);
}

/*
 * Record the sleep time for the interactivity scorer.
 */
static void
sched_eevdf_sleep(struct thread *td, int prio)
{
	THREAD_LOCK_ASSERT(td, MA_OWNED);
	td->td_slptick = ticks;
}

/*
 * Switch threads.  Follows ULE's lock protocol:
 *  1. Block the thread lock
 *  2. Enter spinlock section
 *  3. Handle re-enqueue, sleep, or migration
 *  4. Choose new thread
 *  5. Unlock TDQ
 *  6. cpu_switch
 */
/*
 * thread_lock_unblock() that does not assume td_lock is blocked.
 */
static inline void
thread_unblock_switch(struct thread *td, struct mtx *mtx)
{
	atomic_store_rel_ptr((volatile uintptr_t *)&td->td_lock,
	    (uintptr_t)mtx);
}

static void
sched_eevdf_sswitch(struct thread *td, int flags)
{
	struct thread *newtd;
	struct tdq *tdq;
	struct td_sched *ts;
	struct mtx *mtx;
	int cpuid, preempted;
	int srqflag;
#ifdef SMP
	int pickcpu;
#endif

	THREAD_LOCK_ASSERT(td, MA_OWNED);

	cpuid = PCPU_GET(cpuid);
	tdq = TDQ_SELF();
	ts = td_get_sched(td);

	update_clock(tdq);
	update_curr(tdq);

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
	 * Block the thread lock so we can drop the tdq lock early.
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
		if (ts->ts_cpu == cpuid) {
			/*
			 * Re-enqueue on local queue.
			 * For timeshare: update deadline if expired, re-insert.
			 */
			if (PRI_BASE(td->td_pri_class) == PRI_TIMESHARE) {
				if (ts->ts_vruntime >= ts->ts_deadline)
					entity_update_deadline(ts);
				if (!ts->ts_on_rq)
					eevdf_tree_enqueue(tdq, ts);
				TD_SET_RUNQ(td);
			} else if (PRI_BASE(td->td_pri_class) == PRI_REALTIME ||
			    td->td_pri_class == PRI_ITHD) {
				runq_add(&tdq->tdq_realtime, td, srqflag);
			} else {
				runq_add(&tdq->tdq_idle, td, srqflag);
			}
		} else {
#ifdef SMP
			mtx = sched_switch_migrate(tdq, td, srqflag);
#endif
		}
	} else {
		/* Thread is going to sleep. */
		if (mtx != TDQ_LOCKPTR(tdq)) {
			mtx_unlock_spin(mtx);
			TDQ_LOCK(tdq);
		}
		/* Preserve lag for EEVDF fairness across sleep. */
		if (PRI_BASE(td->td_pri_class) == PRI_TIMESHARE &&
		    ts->ts_on_rq) {
			int64_t vlag, limit;
			vlag = tdq_avg_vruntime(tdq) - ts->ts_vruntime;
			limit = calc_delta_fair(ts->ts_eevdf_slice * 2,
			    ts->ts_weight, ts->ts_inv_weight);
			ts->ts_vlag = (vlag > limit) ? limit :
			    (vlag < -limit) ? -limit : vlag;
			eevdf_tree_dequeue(tdq, ts);
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
		KTR_STATE3(KTR_SCHED, "thread", sched_tdname(td),
		    KTDSTATE(td),
		    "prio:%d", td->td_priority,
		    "wmesg:\"%s\"", td->td_wmesg,
		    "lockname:\"%s\"", td->td_lockname);
#endif

	TDQ_LOCK_ASSERT(tdq, MA_OWNED | MA_NOTRECURSED);
	MPASS(td == tdq->tdq_curthread);
	newtd = choosethread();
	TDQ_UNLOCK(tdq);

	if (td != newtd) {
		SCHED_STAT_INC(eevdf_switches);
#ifdef HWPMC_HOOKS
		if (PMC_PROC_IS_USING_PMCS(td->td_proc))
			PMC_SWITCH_CONTEXT(td, PMC_FN_CSW_OUT);
#endif
		SDT_PROBE2(sched, , , off__cpu, newtd, newtd->td_proc);

#ifdef HWT_HOOKS
		HWT_CALL_HOOK(td, HWT_SWITCH_OUT, NULL);
		HWT_CALL_HOOK(newtd, HWT_SWITCH_IN, NULL);
#endif

		td->td_oncpu = NOCPU;
		cpu_switch(td, newtd, mtx);
		cpuid = td->td_oncpu = PCPU_GET(cpuid);

		SDT_PROBE0(sched, , , on__cpu);
#ifdef HWPMC_HOOKS
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
 * sched_throw_grab() chooses a thread and drops the TDQ lock in a
 * spinlock section.
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
 * A thread is exiting.
 */
static void
sched_eevdf_throw(struct thread *td)
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
 * Set the base user priority, does not effect current running priority.
 */
static void
sched_eevdf_user_prio(struct thread *td, u_char prio)
{
	td->td_base_user_pri = prio;
	if (td->td_lend_user_pri <= prio)
		return;
	td->td_user_pri = prio;
}

/*
 * Fix priorities on return to user-space.  Priorities may be elevated due
 * to static priority actions.
 */
static void
sched_eevdf_userret_slowpath(struct thread *td)
{
	thread_lock(td);
	td->td_priority = td->td_user_pri;
	td->td_base_pri = td->td_user_pri;
	tdq_setlowpri(TDQ_SELF(), td);
	thread_unlock(td);
}

/*
 * Add a thread to a queue.  Picks CPU, migrates lock, notifies.
 * Requires thread lock on entry, drops on exit (unless SRQ_HOLDTD).
 */
static void
sched_eevdf_add(struct thread *td, int flags)
{
	struct tdq *tdq;
	struct td_sched *ts;
#ifdef SMP
	int cpu, lowpri;
#endif

	KTR_STATE2(KTR_SCHED, "thread", sched_tdname(td), "runq add",
	    "prio:%d", td->td_priority, KTR_ATTR_LINKED,
	    sched_tdname(curthread));
	SDT_PROBE4(sched, , , enqueue, td, td->td_proc, NULL,
	    flags & SRQ_PREEMPTED);
	THREAD_LOCK_ASSERT(td, MA_OWNED);

	ts = td_get_sched(td);

	/* Place EEVDF entity before adding. */
	if (PRI_BASE(td->td_pri_class) == PRI_TIMESHARE && !ts->ts_on_rq) {
		/*
		 * We need the target TDQ to place the entity. For SMP,
		 * pickcpu determines the target. Use ts_runq_cpu.
		 */
		update_clock(TDQ_CPU(ts->ts_cpu));
		place_entity(TDQ_CPU(ts->ts_cpu), ts,
		    (flags & SRQ_BORING) == 0);
	}

#ifdef SMP
	cpu = sched_pickcpu(td, flags);
	tdq = sched_setcpu(td, cpu, flags);
	lowpri = tdq_add(tdq, td, flags);
	if (cpu != PCPU_GET(cpuid))
		tdq_notify(tdq, lowpri);
	else if (!(flags & SRQ_YIELDING))
		sched_setpreempt(td->td_priority);
#else
	tdq = TDQ_SELF();
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
 * Choose the highest priority thread to run.  The thread is removed
 * from the queue while running; load remains.
 */
static struct thread *
sched_eevdf_choose(void)
{
	struct thread *td;
	struct tdq *tdq;
	struct td_sched *ts;

	tdq = TDQ_SELF();
	TDQ_LOCK_ASSERT(tdq, MA_OWNED);

	td = tdq_choose(tdq);
	if (td != NULL) {
		ts = td_get_sched(td);
		/* Remove from tree/runq (load stays). */
		if (PRI_BASE(td->td_pri_class) == PRI_TIMESHARE) {
			if (ts->ts_on_rq)
				eevdf_tree_dequeue(tdq, ts);
			tdq->tdq_curr = ts;
			tdq->tdq_clock_task = tdq->tdq_clock;
			ts->ts_prev_sum_exec = ts->ts_sum_exec;
		} else {
			if (PRI_BASE(td->td_pri_class) == PRI_REALTIME ||
			    td->td_pri_class == PRI_ITHD)
				runq_remove(&tdq->tdq_realtime, td);
			else
				runq_remove(&tdq->tdq_idle, td);
			tdq->tdq_curr = NULL;
		}
		tdq->tdq_lowpri = td->td_priority;
	} else {
		tdq->tdq_lowpri = PRI_MAX_IDLE;
		td = PCPU_GET(idlethread);
		tdq->tdq_curr = NULL;
	}
	tdq->tdq_curthread = td;
	return (td);
}

/*
 * Handle a stathz tick.  This is really only relevant for timeshare
 * and interrupt threads.
 */
static void
sched_eevdf_clock(struct thread *td, int cnt)
{
	struct tdq *tdq;
	struct td_sched *ts, *curr;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	tdq = TDQ_SELF();

#ifdef SMP
	/*
	 * Run the long term load balancer infrequently on the first cpu.
	 */
	if (balance_tdq == tdq && smp_started != 0 && rebalance != 0 &&
	    balance_ticks != 0) {
		balance_ticks -= cnt;
		if (balance_ticks <= 0)
			sched_balance();
	}
#endif

	tdq->tdq_oldswitchcnt = tdq->tdq_switchcnt;
	tdq->tdq_switchcnt = tdq->tdq_load;

	update_clock(tdq);
	update_curr(tdq);

	ts = td_get_sched(td);
	ts->ts_ticks += cnt;
	ts->ts_ltick = (u_int)ticks;

	if ((td->td_pri_class & PRI_FIFO_BIT) || TD_IS_IDLETHREAD(td))
		return;

	/* Handle realtime RR quantum expiry. */
	if (PRI_BASE(td->td_pri_class) == PRI_REALTIME) {
		ts->ts_slice -= cnt;
		if (ts->ts_slice <= 0) {
			ts->ts_slice = sched_quantum;
			ast_sched_locked(td, TDA_SCHED);
			td->td_flags |= TDF_SLICEEND;
		}
		return;
	}

	if (PRI_BASE(td->td_pri_class) != PRI_TIMESHARE)
		return;

	curr = tdq->tdq_curr;
	if (curr == NULL)
		return;

	/* Deadline exceeded: reschedule. */
	if (curr->ts_vruntime >= curr->ts_deadline) {
		ast_sched_locked(td, TDA_SCHED);
		td->td_flags |= TDF_SLICEEND;
		return;
	}

	/* Tick preemption with minimum granularity. */
	if (curr->ts_sum_exec - curr->ts_prev_sum_exec >=
	    EEVDF_MIN_GRANULARITY_NS) {
		struct td_sched *next = pick_eevdf(tdq);
		if (next != NULL && next != curr &&
		    next->ts_deadline < curr->ts_deadline) {
			ast_sched_locked(td, TDA_SCHED);
			td->td_flags |= TDF_SLICEEND;
		}
	}
}

#ifdef SMP
#define	TDQ_IDLESPIN(tdq)						\
    ((tdq)->tdq_cg != NULL && ((tdq)->tdq_cg->cg_flags & CG_FLAG_THREAD) == 0)
#else
#define	TDQ_IDLESPIN(tdq)	1
#endif

static int __read_mostly sched_idlespins = 10000;
static int __read_mostly sched_idlespinthresh = -1;

/*
 * The actual idle process.
 */
static void
sched_eevdf_idletd(void *dummy)
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
		if (switchcnt != oldswitchcnt) {
			oldswitchcnt = switchcnt;
			if (tdq_idled(tdq) == 0)
				continue;
		}
		switchcnt = TDQ_SWITCHCNT(tdq);
#else
		oldswitchcnt = switchcnt;
#endif
		if (TDQ_IDLESPIN(tdq) && switchcnt > sched_idlespinthresh) {
			for (i = 0; i < sched_idlespins; i++) {
				if (TDQ_LOAD(tdq))
					break;
				cpu_spinwait();
			}
		}

		switchcnt = TDQ_SWITCHCNT(tdq);
		if (TDQ_LOAD(tdq) != 0 || switchcnt != oldswitchcnt)
			continue;

		atomic_store_int(&tdq->tdq_cpu_idle, 1);
		atomic_thread_fence_seq_cst();
		if (TDQ_LOAD(tdq) != 0) {
			atomic_store_int(&tdq->tdq_cpu_idle, 0);
			continue;
		}
		cpu_idle(switchcnt * 4 > sched_idlespinthresh);
		atomic_store_int(&tdq->tdq_cpu_idle, 0);

		switchcnt = TDQ_SWITCHCNT(tdq);
		if (switchcnt != oldswitchcnt)
			continue;
		TDQ_SWITCHCNT_INC(tdq);
		oldswitchcnt++;
	}
}

static void
sched_eevdf_preempt(struct thread *td)
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
			return;
		}
		td->td_owepreempt = 1;
	} else {
		tdq->tdq_owepreempt = 0;
	}
	thread_unlock(td);
}

/*
 * Basic yield call.
 */
static void
sched_eevdf_relinquish(struct thread *td)
{
	thread_lock(td);
	mi_switch(SW_VOL | SWT_RELINQUISH);
}

/*
 * Remove a thread from a run-queue without running it.  This is used
 * when we're stealing a thread from a remote queue.  Otherwise all
 * removals are done through choosetd().
 */
static void
sched_eevdf_rem(struct thread *td)
{
	struct tdq *tdq;
	struct td_sched *ts;

	KTR_STATE1(KTR_SCHED, "thread", sched_tdname(td), "runq rem",
	    "prio:%d", td->td_priority);
	SDT_PROBE3(sched, , , dequeue, td, td->td_proc, NULL);

	ts = td_get_sched(td);
	tdq = TDQ_CPU(ts->ts_cpu);
	TDQ_LOCK_ASSERT(tdq, MA_OWNED);
	MPASS(td->td_lock == TDQ_LOCKPTR(tdq));
	KASSERT(TD_ON_RUNQ(td),
	    ("sched_rem: thread not on run queue"));

	if (PRI_BASE(td->td_pri_class) == PRI_TIMESHARE) {
		if (ts->ts_on_rq)
			eevdf_tree_dequeue(tdq, ts);
	} else if (PRI_BASE(td->td_pri_class) == PRI_REALTIME ||
	    td->td_pri_class == PRI_ITHD) {
		runq_remove(&tdq->tdq_realtime, td);
	} else {
		runq_remove(&tdq->tdq_idle, td);
	}
	tdq_load_rem(tdq, td);
	TD_SET_CAN_RUN(td);
	if (td->td_priority == tdq->tdq_lowpri)
		tdq_setlowpri(tdq, NULL);
}

/*
 * Schedule a thread to resume execution and record how long it voluntarily
 * slept.
 */
static void
sched_eevdf_wakeup(struct thread *td, int srqflags)
{
	struct td_sched *ts;

	THREAD_LOCK_ASSERT(td, MA_OWNED);
	ts = td_get_sched(td);

	/*
	 * When resuming an idle ithread, restore its base priority.
	 */
	if (PRI_BASE(td->td_pri_class) == PRI_ITHD &&
	    td->td_priority != td->td_base_ithread_pri)
		sched_prio(td, td->td_base_ithread_pri);

	ts->ts_slice = 0;
	sched_add(td, SRQ_BORING | srqflags);
}

/*
 * Bind a thread to a target cpu.
 */
static void
sched_eevdf_bind(struct thread *td, int cpu)
{
	struct td_sched *ts;

	THREAD_LOCK_ASSERT(td, MA_OWNED | MA_NOTRECURSED);
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
	mi_switch(SW_VOL | SWT_BIND);
	thread_lock(td);
}

/*
 * Release a bound thread.
 */
static void
sched_eevdf_unbind(struct thread *td)
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
sched_eevdf_is_bound(struct thread *td)
{
	THREAD_LOCK_ASSERT(td, MA_OWNED);
	return (td_get_sched(td)->ts_flags & TSF_BOUND);
}

/*
 * Enforce affinity settings for a thread.  Called after adjustments to
 * the thread's CPU affinity mask.
 */
static void
sched_eevdf_affinity(struct thread *td)
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
	ast_sched_locked(td, TDA_SCHED);
	if (td != curthread)
		ipi_cpu(ts->ts_cpu, IPI_PREEMPT);
#endif
}

static int
sched_eevdf_sizeof_proc(void)
{
	return (sizeof(struct proc));
}

static int
sched_eevdf_sizeof_thread(void)
{
	return (sizeof(struct thread) + sizeof(struct td_sched));
}

/*
 * Create on first use to catch odd startup conditions.
 */
static char *
sched_eevdf_tdname(struct thread *td)
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
sched_eevdf_clear_tdname(struct thread *td)
{
#ifdef KTR
	struct td_sched *ts;

	ts = td_get_sched(td);
	ts->ts_name[0] = '\0';
#endif
}

static bool
sched_eevdf_do_timer_accounting(void)
{
	return (true);
}

#ifdef SMP
static int
eevdf_find_child_with_core(int cpu, struct cpu_group *grp)
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
sched_eevdf_find_l2_neighbor(int cpu)
{
	struct cpu_group *grp;
	int i;

	grp = cpu_top;
	if (grp == NULL)
		return (-1);

	i = 0;
	while ((i = eevdf_find_child_with_core(cpu, grp)) != -1) {
		if (grp->cg_child[i].cg_count <= 1)
			return (-1);
		grp = &grp->cg_child[i];
	}

	if (grp->cg_level > CG_SHARE_L2 || grp->cg_level == CG_SHARE_NONE)
		return (-1);

	for (i = 0; i < CPU_SETSIZE; i++) {
		if (CPU_ISSET(i, &grp->cg_mask) && i != cpu)
			return (i);
	}
	return (-1);
}
#else
static int
sched_eevdf_find_l2_neighbor(int cpu)
{
	return (-1);
}
#endif

static const struct sched_instance sched_eevdf_instance = {
#define	SLOT(name) .name = sched_eevdf_##name
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
	SLOT(do_timer_accounting),
	SLOT(find_l2_neighbor),
	SLOT(init),
	SLOT(init_ap),
	SLOT(setup),
	SLOT(initticks),
	SLOT(schedcpu),
#undef SLOT
};

DECLARE_SCHEDULER(eevdf_sched_selector, "EEVDF", &sched_eevdf_instance);

static const char *sched_name = "eevdf";
SYSCTL_STRING(_kern_sched, OID_AUTO, name, CTLFLAG_RD,
    &sched_name, 0, "Scheduler name");
SYSCTL_INT(_kern_sched, OID_AUTO, quantum, CTLFLAG_RW, &sched_quantum, 0,
    "Quantum in ticks for realtime round-robin");

SYSCTL_NODE(_kern_sched, OID_AUTO, eevdf, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "EEVDF Scheduler");
SYSCTL_INT(_kern_sched_eevdf, OID_AUTO, idlespins, CTLFLAG_RWTUN,
    &sched_idlespins, 0,
    "Number of times idle thread will spin waiting for new work");
SYSCTL_INT(_kern_sched_eevdf, OID_AUTO, idlespinthresh, CTLFLAG_RW,
    &sched_idlespinthresh, 0,
    "Threshold before we will permit idle thread spinning");
#ifdef SMP
SYSCTL_INT(_kern_sched_eevdf, OID_AUTO, affinity, CTLFLAG_RW, &affinity, 0,
    "Number of hz ticks to keep thread affinity for");
SYSCTL_INT(_kern_sched_eevdf, OID_AUTO, balance, CTLFLAG_RWTUN, &rebalance, 0,
    "Enables the long-term load balancer");
SYSCTL_INT(_kern_sched_eevdf, OID_AUTO, balance_interval, CTLFLAG_RW,
    &balance_interval, 0,
    "Average period in stathz ticks to run the long-term balancer");
SYSCTL_INT(_kern_sched_eevdf, OID_AUTO, steal_idle, CTLFLAG_RWTUN,
    &steal_idle, 0,
    "Attempts to steal work from other cores before idling");
SYSCTL_INT(_kern_sched_eevdf, OID_AUTO, steal_thresh, CTLFLAG_RWTUN,
    &steal_thresh, 0,
    "Minimum load on remote CPU before we'll steal");
SYSCTL_INT(_kern_sched_eevdf, OID_AUTO, trysteal_limit, CTLFLAG_RWTUN,
    &trysteal_limit, 0,
    "Topological distance limit for stealing threads in sched_switch()");
#endif
