/*
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * This software was developed by Minsoo Choo under sponsorship from the
 * FreeBSD Foundation.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef _SYS_STAILQ_H_
#define _SYS_STAILQ_H_

#include <machine/atomic.h>
#include <sys/stddef.h>
#include <sys/types.h>

/*
 * This is a non-macro implementation of STAILQ_* equivalent to queue(3).
 *
 * A singly-linked tail queue is headed by a pair of pointers, one to the
 * head of the queue and the other to the tail of the queue. The nodes are
 * singly linked for minimum space and pointer manipulation overhead at the
 * expense of O(n) removal for arbitrary nodes. New nodes can be added
 * to the queue after an existing node, at the head of the queue, or at the
 * end of the queue. Nodes being removed from the head of the tail queue
 * should use the explicit macro for this purpose for optimum efficiency.
 * A singly-linked tail queue may only be traversed in the forward direction.
 * Singly-linked tail queues are ideal for applications with large datasets
 * and few or no removals or for implementing a FIFO queue.
 */

/*
 * Singly-linked tail queue functions.
 */

struct stailq_node {
	struct stailq_node *next;	/* next node */
};

struct stailq {
	struct stailq_node *first;	/* first node */
	struct stailq_node **last;	/* addr of last next node */
};

/*
 * Singly-linked Tail queue functions and macros.
 */

static inline bool stailq_empty(const struct stailq *);
static inline void stailq_init(struct stailq *);

static inline void
stailq_concat(struct stailq *queue1, struct stailq *queue2)
{
	if (!stailq_empty(queue2)) {
		*queue1->last = queue2->first;
		queue1->last = queue2->last;
		stailq_init(queue2);
	}
}

static inline bool
stailq_empty(const struct stailq *queue)
{
	return (queue->first == NULL);
}

static inline bool
stailq_empty_atomic(const struct stailq *queue)
{
	return (atomic_load_ptr(&queue->first) == NULL);
}

static inline struct stailq_node *
stailq_first(const struct stailq *queue)
{
	return queue->first;
}

#define stailq_foreach(var, queue)					\
	for ((var) = stailq_first(queue);				\
	    (var) != NULL;						\
	    (var) = stailq_next(var))

#define stailq_foreach_from(var, queue)					\
	for ((var) = ((var) != NULL ? (var) : stailq_first(queue));	\
	    (var) != NULL;						\
	    (var) = stailq_next(var))

#define stailq_foreach_safe(var, queue, tvar)				\
	for ((var) = stailq_first(queue);				\
	    (var) != NULL && ((tvar) = stailq_next(var), 1);		\
	    (var) = (tvar))

#define stailq_foreach_from_safe(var, queue, tvar)			\
	for ((var) = ((var) != NULL ? (var) : stailq_first(queue));	\
	    (var) != NULL && ((tvar) = stailq_next(var), 1);		\
	    (var) = (tvar))

static inline void
stailq_init(struct stailq *queue)
{
	queue->first = NULL;
	queue->last = &queue->first;
}

static inline void
stailq_insert_after(struct stailq *queue, struct stailq_node *qnode,
                    struct stailq_node *node)
{
	if ((node->next = qnode->next) == NULL)
		queue->last = &node->next;
	qnode->next = node;
}

static inline void
stailq_insert_head(struct stailq *queue, struct stailq_node *node)
{
	if ((node->next = queue->first) == NULL)
		queue->last = &node->next;
	queue->first = node;
}

static inline void
stailq_insert_tail(struct stailq *queue, struct stailq_node *node)
{
	node->next = NULL;
	*queue->last = node;
	queue->last = &node->next;
}

static inline struct stailq_node *
stailq_last(struct stailq *queue)
{
	return stailq_empty(queue) ? NULL :
	    (struct stailq_node *)((char *)queue->last -
	        offsetof(struct stailq_node, next));
}

static inline struct stailq_node *
stailq_next(const struct stailq_node *node)
{
	return node->next;
}

static inline void
stailq_remove_after(struct stailq *queue, struct stailq_node *node)
{
	if ((node->next = node->next->next) == NULL)
		queue->last = &node->next;
}

static inline void
stailq_remove_head(struct stailq *queue)
{
	if ((queue->first = queue->first->next) == NULL)
		queue->last = &queue->first;
}

static inline void
stailq_remove(struct stailq *queue, struct stailq_node *node)
{
	if (queue->first == node) {
		stailq_remove_head(queue);
	} else {
		struct stailq_node *curnode = queue->first;
		while (curnode->next != node)
			curnode = curnode->next;
		stailq_remove_after(queue, curnode);
	}
}

static inline void
stailq_split_after(struct stailq *queue, struct stailq_node *node,
                   struct stailq *rest)
{
	if (node->next == NULL) {
		/* 'node' is the last node in 'queue'. */
		stailq_init(rest);
	} else {
		rest->first = node->next;
		rest->last = queue->last;
		node->next = NULL;
		queue->last = &node->next;
	}
}

static inline void
stailq_swap(struct stailq *queue1, struct stailq *queue2)
{
	struct stailq_node *swap_first = queue1->first;
	struct stailq_node **swap_last = queue1->last;
	queue1->first = queue2->first;
	queue1->last = queue2->last;
	queue2->first = swap_first;
	queue2->last = swap_last;
	if (queue1->first == NULL)
		queue1->last = &queue1->first;
	if (queue2->first == NULL)
		queue2->last = &queue2->first;
}

static inline void
stailq_reverse(struct stailq *queue)
{
	if (stailq_empty(queue))
		return;
	struct stailq_node *var, *varp, *varn;
	for (var = queue->first, varp = NULL; var != NULL; ) {
		varn = var->next;
		var->next = varp;
		varp = var;
		var = varn;
	}
	queue->last = &queue->first->next;
	queue->first = varp;
}

#endif /* !_SYS_STAILQ_H_ */
