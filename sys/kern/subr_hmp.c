/*
 * Copyright (c) 2026 FreeBSD Foundation
 *
 * This software was developed by Minsoo Choo under sponsorship from the
 * FreeBSD Foundation.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/systm.h>
#include <sys/hmp.h>
#include <sys/kernel.h>
#include <sys/pcpu.h>
#include <sys/smp.h>
#include <sys/sysctl.h>

/* System-wide CPU capability information */
struct hmp hmp_state = {
	.total_capacity	= 0,
	.has_scores	= false,
};

/* Per-CPU HMP state */
DPCPU_DEFINE(struct hmp_pcpu, hmp_pcpu);

/*
 * Initialization
 */

static void
hmp_init(void *arg __unused)
{
	/* Choose and call provider */
}
SYSINIT(hmp, SI_SUB_SMP + 1, SI_ORDER_ANY, hmp_init, NULL);

/*
 * Helper functions for scheduler
 */

/*
 * Fall back for hmp_best_cpu in case processor doesn't support dynamic
 * score update. Takes O(n).
 *
 * TODO: Precalculate cpu with highest capacity on boot after initialization.
 */
int
hmp_highest_capacity_cpu(const cpuset_t *mask)
{
	struct hmp_pcpu *hp;
	hmp_score_t best_cap;
	int best_cpu, cpu;

	best_cpu = -1;
	best_cap = 0;

	CPU_FOREACH(cpu) {
		if (mask != NULL && !CPU_ISSET(cpu, mask))
			continue;
		hp = DPCPU_ID_PTR(cpu, hmp_pcpu);
		if (hp->capacity > best_cap) {
			best_cap = hp->capacity;
			best_cpu = cpu;
		}
	}

	return (best_cpu);
}

int
hmp_lowest_capacity_cpu(const cpuset_t *mask)
{
	struct hmp_pcpu *hp;
	hmp_score_t best_cap;
	int best_cpu, cpu;

	best_cpu = -1;
	best_cap = 0;

	CPU_FOREACH(cpu) {
		if (mask != NULL && !CPU_ISSET(cpu, mask))
			continue;
		hp = DPCPU_ID_PTR(cpu, hmp_pcpu);
		if (best_cpu == -1 || hp->capacity < best_cap) {
			best_cap = hp->capacity;
			best_cpu = cpu;
		}
	}

	return (best_cpu);
}

/*
 * Find CPU with best score for given class and capability for thread
 * placement. Fall backs to capacity if scores are not provided. Takes O(n).
 *
 * It is possible that a score of previously read cpu is updated by a provider
 * while this function is still traversing remaining cpus, but the effect is
 * negligible.
 *
 * TODO: If this brings severe performance degradation, score providers should
 *       maintain and update index everytime new information is fed and
 *       the scheduler should use the index which takes O(1).
 */
int
hmp_best_cpu(const cpuset_t *mask, enum score_type st)
{
	struct hmp_pcpu *hp;
	hmp_score_t best_score;
	int best_cpu, cpu;

	if (!hmp_state.has_scores) {
		switch (st) {
		case HMP_SCORE_PERF:
			return (hmp_highest_capacity_cpu(mask));
		case HMP_SCORE_EFF:
			return (hmp_lowest_capacity_cpu(mask));
		default:
			panic("invalid score type");
		}
	}

	best_cpu = -1;
	best_score = 0;

	CPU_FOREACH(cpu) {
		if (mask != NULL && !CPU_ISSET(cpu, mask))
			continue;
		hp = DPCPU_ID_PTR(cpu, hmp_pcpu);
		if (hp->scores[st] > best_score) {
			best_score = hp->scores[st];
			best_cpu = cpu;
		}
	}

	return (best_cpu);
}

/*
 * Sysctls
 */

static int
hmp_sysctl_capacity(SYSCTL_HANDLER_ARGS)
{
	struct hmp_pcpu *hp;
	unsigned int v;

	hp = DPCPU_ID_PTR(arg2, hmp_pcpu);
	v = hp->capacity;
	return (sysctl_handle_int(oidp, &v, 0, req));
}

static int
hmp_sysctl_capacity_percent(SYSCTL_HANDLER_ARGS)
{
	struct hmp_pcpu *hp;
	unsigned int v;

	hp = DPCPU_ID_PTR(arg2, hmp_pcpu);
	v = HMP_CAPACITY_NORMAL_TO_PERCENT(hp->capacity);
	return (sysctl_handle_int(oidp, &v, 0, req));
}

static int
hmp_sysctl_score(SYSCTL_HANDLER_ARGS)
{
	struct hmp_pcpu *hp;
	int cpu = arg2 & 0xffff;
	enum score_type st = (arg2 >> 16) & 0xffff;
	unsigned int v;

	hp = DPCPU_ID_PTR(cpu, hmp_pcpu);
	v = hmp_get_score(hp, st);
	return (sysctl_handle_int(oidp, &v, 0, req));
}

static SYSCTL_NODE(_kern, OID_AUTO, hmp, CTLFLAG_RD | CTLFLAG_MPSAFE, 0,
    "Heterogeneous multi-processing state");

SYSCTL_U32(_kern_hmp, OID_AUTO, total_capacity, CTLFLAG_RD,
    &hmp_state.total_capacity, 0,
    "Sum of per-CPU capacities (normalized to HMP_CAPACITY_SCALE)");

SYSCTL_BOOL(_kern_hmp, OID_AUTO, has_scores, CTLFLAG_RD,
    &hmp_state.has_scores, 0,
    "True if a score provider is supplying dynamic perf/eff scores");

static void
hmp_sysctl_init(void *arg __unused)
{
	struct sysctl_oid *cpu_root, *cpu_node, *cpu_score_node;
	struct sysctl_oid_list *cpu_root_children, *cpu_children;
	struct sysctl_oid_list *cpu_score_children;
	char name[8];
	int cpu;

	/*
	 * Per-CPU tree: kern.hmp.cpu.<N>.{capacity,score.perf,score.eff,...}
	 */
	cpu_root = SYSCTL_ADD_NODE(NULL, SYSCTL_STATIC_CHILDREN(_kern_hmp),
	    OID_AUTO, "cpu", CTLFLAG_RD | CTLFLAG_MPSAFE, NULL,
	    "Per-CPU HMP state");
	if (cpu_root == NULL)
		return;
	cpu_root_children = SYSCTL_CHILDREN(cpu_root);

	CPU_FOREACH(cpu) {
		snprintf(name, sizeof(name), "%d", cpu);
		cpu_node = SYSCTL_ADD_NODE(NULL, cpu_root_children, OID_AUTO,
		    name, CTLFLAG_RD | CTLFLAG_MPSAFE, NULL,
		    "Per-CPU HMP state");
		if (cpu_node == NULL)
			continue;
		cpu_children = SYSCTL_CHILDREN(cpu_node);

		SYSCTL_ADD_PROC(NULL, cpu_children, OID_AUTO, "capacity",
		    CTLTYPE_UINT | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, cpu,
		    hmp_sysctl_capacity, "IU",
		    "Static capacity (0-HMP_CAPACITY_SCALE)");

		SYSCTL_ADD_PROC(NULL, cpu_children, OID_AUTO, "capacity_percent",
		    CTLTYPE_UINT | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL, cpu,
		    hmp_sysctl_capacity_percent, "IU",
		    "Static capacity as a percentage of the scale");

		cpu_score_node = SYSCTL_ADD_NODE(NULL, cpu_children, OID_AUTO,
		    "score", CTLFLAG_RD | CTLFLAG_MPSAFE, NULL,
		    "Per-CPU dynamic scores");
		if (cpu_score_node != NULL) {
			cpu_score_children = SYSCTL_CHILDREN(cpu_score_node);

			SYSCTL_ADD_PROC(NULL, cpu_score_children, OID_AUTO,
			    "perf",
			    CTLTYPE_UINT | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL,
			    cpu | (HMP_SCORE_PERF << 16), hmp_sysctl_score,
			    "IU",
			    "Current performance score (dynamic)");

			SYSCTL_ADD_PROC(NULL, cpu_score_children, OID_AUTO,
			    "eff",
			    CTLTYPE_UINT | CTLFLAG_RD | CTLFLAG_MPSAFE, NULL,
			    cpu | (HMP_SCORE_EFF << 16), hmp_sysctl_score,
			    "IU",
			    "Current efficiency score (dynamic)");
		}
	}
}
SYSINIT(hmp_sysctl, SI_SUB_SMP + 2, SI_ORDER_ANY, hmp_sysctl_init, NULL);
