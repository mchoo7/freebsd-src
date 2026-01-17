/*
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * This software was developed by Minsoo Choo under sponsorship from the
 * FreeBSD Foundation.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef _SYS_TAILQ_H_
#define _SYS_TAILQ_H_

#include <machine/atomic.h>
#include <sys/stddef.h>
#include <sys/types.h>

/*
 * This is a non-macro implementation of TAILQ_* equivalent to queue(3).
 *
 * A tail queue is headed by a pair of pointers, one to the head of the
 * queue and the other to the tail of the queue. The nodes are doubly
 * linked so that an arbitrary node can be removed without a need to
 * traverse the queue. New nodes can be added to the queue before or
 * after an existing node, at the head of the queue, or at the end of
 * the queue. A tail queue may be traversed in either direction.
 */

struct tailq_node {
	struct tailq_node *next;	/* next node */
	struct tailq_node **prev;	/* address of previous next node */
};

struct tailq {
	struct tailq_node *first;	/* first node */
	struct tailq_node **last;	/* addr of last next node */
};

/*
 * Tail queue functions and macros.
 */

static inline bool tailq_empty(const struct tailq *);
static inline void tailq_init(struct tailq *);

static inline void
tailq_concat(struct tailq *queue1, struct tailq *queue2)
{
	if (!tailq_empty(queue2)) {
		*queue1->last = queue2->first;
		queue2->first->prev = queue1->last;
		queue1->last = queue2->last;
		tailq_init(queue2);
	}
}

static inline bool
tailq_empty(const struct tailq *queue)
{
	return (queue->first == NULL);
}

static inline bool
tailq_empty_atomic(const struct tailq *queue)
{
	return (atomic_load_ptr(&queue->first) == NULL);
}

static inline struct tailq_node *
ailq_first(const struct tailq *queue)
{
	return queue->first;
}


#define tailq_foreach(var, queue)					\
	for ((var) = tailq_first(queue);				\
	    (var) != NULL;						\
	    (var) = tailq_next(var))

#define tailq_foreach_from(var, queue)					\
	for ((var) = ((var) != NULL ? (var) : tailq_first(queue));	\
	    (var) != NULL;						\
	    (var) = tailq_next(var))

#define tailq_foreach_safe(var, queue, tvar)				\
	for ((var) = tailq_first(queue);				\
	    (var) != NULL && ((tvar) = tailq_next(var), 1);		\
	    (var) = (tvar))

#define tailq_foreach_from_safe(var, queue, tvar)			\
	for ((var) = ((var) != NULL ? (var) : tailq_first(queue));	\
	    (var) != NULL && ((tvar) = tailq_next(var), 1);		\
	    (var) = (tvar))

#define tailq_foreach_reverse(var, queue)				\
	for ((var) = tailq_last(queue);					\
	    (var) != NULL;						\
	    (var) = tailq_prev(var, queue))

#define tailq_foreach_reverse_from(var, queue)				\
	for ((var) = ((var) != NULL ? (var) : tailq_last(queue));	\
	    (var) != NULL;						\
	    (var) = tailq_prev(var, queue))

#define tailq_foreach_reverse_safe(var, queue, tvar) \
	for ((var) = tailq_last(queue); \
	    (var) != NULL && ((tvar) = tailq_prev(var, queue), 1); \
	    (var) = (tvar))

#define tailq_foreach_reverse_from_safe(var, queue, tvar) \
	for ((var) = ((var) != NULL ? (var) : tailq_last(queue)); \
	    (var) != NULL && ((tvar) = tailq_prev(var, queue), 1); \
	    (var) = (tvar))

static inline void
tailq_init(struct tailq *queue)
{
	queue->first = NULL;
	queue->last = &queue->first;
}

static inline void
tailq_insert_after(struct tailq *queue, struct tailq_node *qnode,
                   struct tailq_node *node)
{
	if ((node->next = qnode->next) != NULL)
		node->next->prev = &node->next;
	else
		queue->last = &node->next;
	qnode->next = node;
	node->prev = &qnode->next;
}

static inline void
tailq_insert_before(struct tailq_node *qnode, struct tailq_node *node)
{
	node->prev = qnode->prev;
	node->next = qnode;
	*qnode->prev = node;
	qnode->prev = &node->next;
}

static inline void
tailq_insert_head(struct tailq *queue, struct tailq_node *node)
{
	if ((node->next = queue->first) != NULL)
		queue->first->prev = &node->next;
	else
		queue->last = &node->next;
	queue->first = node;
	node->prev = &queue->first;
}

static inline void
tailq_insert_tail(struct tailq *queue, struct tailq_node *node)
{
	node->next = NULL;
	node->prev = queue->last;
	*queue->last = node;
	queue->last = &node->next;
}

static inline struct tailq_node *
tailq_last(const struct tailq *queue)
{
	return tailq_empty(queue) ? NULL :
	    (struct tailq_node *)((char *)queue->last -
	        offsetof(struct tailq_node, next));
}

static inline struct tailq_node *
tailq_next(const struct tailq_node *node)
{
	return node->next;
}

static inline struct tailq_node *
tailq_prev(const struct tailq_node *node, const struct tailq *queue)
{
	return node->prev == &queue->first ? NULL :
	    (struct tailq_node *)((char *)node->prev -
	        offsetof(struct tailq_node, next));
}

static inline void
tailq_remove(struct tailq *queue, struct tailq_node *node)
{
	if (node->next != NULL)
		node->next->prev = node->prev;
	else
		queue->last = node->prev;
	*node->prev = node->next;
}

static inline void
tailq_remove_head(struct tailq *queue)
{
	tailq_remove(queue, queue->first);
}

static inline void
tailq_replace(struct tailq *queue, struct tailq_node *node,
              struct tailq_node *node2)
{
	node2->next = node->next;
	if (node2->next != NULL)
		node2->next->prev = &node2->next;
	else
		queue->last = &node2->next;
	node2->prev = node->prev;
	*node2->prev = node2;
}

static inline void
tailq_split_after(struct tailq *queue, struct tailq_node *node,
                  struct tailq *rest)
{
	if (node->next == NULL) {
		/* 'node' is the last node in 'queue'. */
		tailq_init(rest);
	} else {
		rest->first = node->next;
		rest->last = queue->last;
		node->next->prev = &rest->first;
		node->next = NULL;
		queue->last = &node->next;
	}
}

static inline void
tailq_swap(struct tailq *queue1, struct tailq *queue2)
{
	struct tailq_node *swap_first = queue1->first;
	struct tailq_node **swap_last = queue1->last;
	queue1->first = queue2->first;
	queue1->last = queue2->last;
	queue2->first = swap_first;
	queue2->last = swap_last;
	if ((swap_first = queue1->first) != NULL)
		swap_first->prev = &queue1->first;
	else
		queue1->last = &queue1->first;
	if ((swap_first = queue2->first) != NULL)
		swap_first->prev = &queue2->first;
	else
		queue2->last = &queue2->first;
}

#endif /* !_SYS_TAILQ_H_ */
