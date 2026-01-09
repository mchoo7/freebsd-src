/*
 * Copyright (c) 2026 FreeBSD Foundation
 *
 * This software was developed by Minsoo Choo under sponsorship from the
 * FreeBSD Foundation.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef	_SYS_HMP_H_
#define	_SYS_HMP_H_

#ifdef	_KERNEL
#ifndef	LOCORE

#include <sys/types.h>
#include <sys/pcpu.h>
#include <sys/queue.h>

#include <machine/atomic.h>

#ifdef	HMP

#define HMP_CAPACITY_SCALE	1024
#define	HMP_CAPACITY_MAX	HMP_CAPACITY_SCALE
#define HMP_CAPACITY_DEFAULT	HMP_CAPACITY_MAX

/* Capacity normalization macro functions */
#define HMP_CAPACITY_NORMAL_FROM(x, y)	(((x) * HMP_CAPACITY_SCALE) / (y))
#define HMP_CAPACITY_NORMAL_FROM_255(x) HMP_CAPACITY_NORMAL_FROM((x), 255)
#define HMP_CAPACITY_NORMAL_FROM_1024(x) HMP_CAPACITY_NORMAL_FROM((x), 1024)

#define HMP_CAPACITY_NORMAL_TO(x, y) (((x) * (y)) / HMP_CAPACITY_SCALE)
#define HMP_CAPACITY_NORMAL_TO_PERCENT(x) HMP_CAPACITY_NORMAL_TO((x), 100)

/*
 * Score type
 */
enum score_type {
	HMP_SCORE_PERF,
	HMP_SCORE_EFF,
};

/*
 * CPU capacity type
 * This value should be normalized to 0-1024.
 *
 * Whenever there is a new capability score scheme where highest score excceds
 * 1024, HMP_CAPACITY_SCALE should be bumped to the new highest score for
 * fine-grained score management on a new architecture.
 */
typedef uint32_t hmp_capacity_t;

/*
 * CPU score type
 * This doesn't need to be normalized.
 */
typedef uint32_t hmp_score_t;

/*
 * System-wide CPU capability state - initialized on boot
 *
 * total_capacity is populated once the capacity provider finishes init().
 * has_scores is set to true once a score provider has won selection and
 * finished init(); when false, the scheduler must fall back to capacity-only
 * placement decisions.
 */
struct hmp {
	hmp_capacity_t	total_capacity;		/* Precalculated for scheduler */
	bool		has_scores;		/* Runtime updates available */
};
extern struct hmp hmp_state;

/*
 * Per-CPU HMP state
 */
struct hmp_pcpu {
	hmp_capacity_t	capacity;
	hmp_score_t	scores[2];
};
DPCPU_DECLARE(struct hmp_pcpu, hmp_pcpu);

/*
 * Accessors
 */
static inline hmp_score_t
hmp_get_score(struct hmp_pcpu *hp, enum score_type st)
{
	return atomic_load_acq_32(&hp->scores[st]);
}

/*
 * Setters
 */
static inline void
hmp_set_score(struct hmp_pcpu *hp, enum score_type st, hmp_score_t score)
{
	atomic_store_rel_32(&hp->scores[st], score);
}

/*
 * Provider interfaces
 *
 * HMP is fed by two independent provider paths:
 *
 * probe() must not touch hmp_pcpu state.
 * init() is called once, only on the winning provider of each path.
 */

/*
 * Capacity provider
 *
 * Populate the static per-CPU capacity field and hmp_state.total_capacity.
 * Exactly one capacity provider wins at boot (highest priority among
 * those whose probe() returns true).
 */
struct hmp_capacity_provider {
	const char	*name;
	int		 priority;	/* higher wins */
	int		(*probe)(void);
	int		(*init)(void);

	/* Filled in by registration; do not set manually. */
	bool		 probed;
	bool		 active;

	SLIST_ENTRY(hmp_capacity_provider) link;
};

/*
 * Score provider
 *
 * Populate and maintain per-CPU perf/eff scores and flags.
 * Exactly one score provider wins at boot. Score provider can be absent
 * (hmp_state.has_scores stays false and the scheduler uses capacity-only
 * placement).
 */
struct hmp_score_provider {
	const char	*name;
	int		 priority;	/* higher wins */
	int		(*probe)(void);
	int		(*init)(void);

	/* Filled in by registration; do not set manually. */
	bool		 probed;
	bool		 active;

	SLIST_ENTRY(hmp_score_provider) link;
};

void	hmp_capacity_provider_register(struct hmp_capacity_provider *p);
void	hmp_score_provider_register(struct hmp_score_provider *p);

#define	HMP_CAPACITY_PROVIDER_DECLARE(name, provider)			\
	DATA_SET(hmp_capacity_provider_set, provider);			\
	static void hmp_cap_provider_##name##_register(void *arg __unused) \
	{								\
		hmp_capacity_provider_register(&(provider));		\
	}								\
	SYSINIT(hmp_cap_provider_##name, SI_SUB_SMP, SI_ORDER_ANY,	\
	    hmp_cap_provider_##name##_register, NULL)

#define	HMP_SCORE_PROVIDER_DECLARE(name, provider)			\
	DATA_SET(hmp_score_provider_set, provider);			\
	static void hmp_score_provider_##name##_register(void *arg __unused) \
	{								\
		hmp_score_provider_register(&(provider));		\
	}								\
	SYSINIT(hmp_score_provider_##name, SI_SUB_SMP, SI_ORDER_ANY,	\
	    hmp_score_provider_##name##_register, NULL)

/*
 * Helper functions for scheduler
 */
int hmp_highest_capacity_cpu(const cpuset_t *mask);
int hmp_lowest_capacity_cpu(const cpuset_t *mask);
int hmp_best_cpu(const cpuset_t *mask, enum score_type st);

#endif	/* HMP */
#endif	/* !LOCORE */
#endif	/* _KERNEL */
#endif	/* _SYS_HMP_H_ */
