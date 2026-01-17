/*
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * This software was developed by Minsoo Choo under sponsorship from the
 * FreeBSD Foundation.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef _SYS_SLIST_H_
#define _SYS_SLIST_H_

#include <machine/atomic.h>
#include <sys/stddef.h>
#include <sys/types.h>

/*
 * This is a non-macro implementation of SLIST_* equivalent to queue(3).
 *
 * A singly-linked list is headed by a single forward pointer. The nodes
 * are singly linked for minimum space and pointer manipulation overhead at
 * the expense of O(n) removal for arbitrary nodes. New nodes can be
 * added to the list after an existing node or at the head of the list.
 * Nodes being removed from the head of the list should use the explicit
 * macro for this purpose for optimum efficiency. A singly-linked list may
 * only be traversed in the forward direction.  Singly-linked lists are ideal
 * for applications with large datasets and few or no removals or for
 * implementing a LIFO queue.
 */

/*
 * Singly-linked List declarations.
 */

struct slist_node {
	slist_node *next;	/* next node */
};

struct slist {
	slist_node *first;	/* first node */
};

/*
 * Singly-linked list functions and macros.
 */

static inline void
slist_concat(struct slist *list1, struct slist *list2)
{
	slist_node *curr;

	if (list1->first == NULL) {
		list1->first = list2->first;
	} else if (list2->first != NULL) {
		curr = list1->first;
		while (curr->next != NULL)
			curr = curr->next;
		curr->next = list2->first;
	}

	list2->first = NULL;
}

static inline bool
slist_empty(const struct slist *list)
{
	return list->first == NULL;
}

static inline bool
slist_empty_atomic(const struct slist *list)
{
	return (atomic_load_ptr(&(list)->first) == NULL);
}

static inline struct slist_node *
slist_first(const struct slist *list)
{
	return list->first;
}

#define slist_foreach(var, list)					\
	for ((var) = slist_first(list);					\
	    (var) != NULL;						\
	    (var) = slist_next(var))

#define slist_foreach_from(var, list)					\
	for ((var) = ((var) != NULL ? (var) : slist_first(list));	\
	    (var) != NULL; \
	    (var) = slist_next(var))

#define slist_foreach_safe(var, list, tvar)				\
	for ((var) = slist_first(list);					\
	    (var) != NULL && ((tvar) = slist_next(var), 1);		\
	    (var) = (tvar))

#define slist_foreach_from_safe(var, list, tvar)			\
	for ((var) = ((var) != NULL ? (var) : slist_first(list));	\
	    (var) != NULL && ((tvar) = slist_next(var), 1);	\
	    (var) = (tvar))

inline void
slist_init(struct slist *list)
{
	list->first = NULL;
}

static inline void
slist_insert_after(struct slist_node *slistnode, struct slist_node *node)
{
	node->next = slistnode->next;
	slistnode->next = node;
}

static inline void
slist_insert_head(struct slist *list, struct slist_node *node)
{
	node->next = list->first;
	list->first = node;
}

static inline struct slist_node *
slist_next(const struct slist_node *node)
{
	return node->next;
}

static inline void
slist_remove_after(struct slist_node *node)
{
	node->next = node->next->next;
}

static inline void
slist_remove_head(struct slist *list)
{
	list->first = list->first->next;
}

static inline void
slist_remove(struct slist *list, struct slist_node *node)
{
	if (list->first == node) {
		slist_remove_head(list);
	} else {
		struct slist_node *curnode = list->first;
		while (curnode->next != node)
			curnode = curnode->next;
		slist_remove_after(curnode);
	}
}

static inline void
slist_remove_prevptr(struct slist_node **prevp, struct slist_node *node)
{
	*prevp = node->next;
}

static inline void
slist_split_after(struct slist *list, struct slist_node *node, struct slist *rest)
{
	rest->first = node->next;
	node->next = NULL;
}

static inline void
slist_swap(struct slist *list1, struct slist *list2)
{
	struct slist_node *swap_first = list1->first;
	list1->first = list2->first;
	list2->first = swap_first;
}

#endif /* !_SYS_SLIST_H_ */
