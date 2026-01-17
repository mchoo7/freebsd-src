/*
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * This software was developed by Minsoo Choo under sponsorship from the
 * FreeBSD Foundation.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef _SYS_LIST_H_
#define _SYS_LIST_H_

#include <machine/atomic.h>
#include <sys/stddef.h>
#include <sys/types.h>

/*
 * This is a non-macro implementation of LIST_* equivalent to queue(3).
 *
 * A list is headed by a single forward pointer (or an array of forward
 * pointers for a hash table header). The nodes are doubly linked
 * so that an arbitrary node can be removed without a need to
 * traverse the list. New nodes can be added to the list before
 * or after an existing node or at the head of the list. A list
 * may be traversed in either direction.
 */

/*
 * List declarations.
 */

struct list_node {
	struct list_node *next;		/* next node */
	struct list_node **prev;	/* address of previous next node */
};

struct list {
	struct list_node *first;	/* first node */
};

/*
 * List functions and macros.
 */

static inline bool list_empty(const struct list *);
static inline void list_init(struct list *);

static inline void
list_concat(struct list *list1, struct list *list2)
{
	struct list_node *curnode = list1->first;
	if (curnode == NULL) {
		if ((list1->first = list2->first) != NULL) {
			list2->first->prev = &list1->first;
			list_init(list2);
		}
	} else if (list2->first != NULL) {
		while (curnode->next != NULL)
			curnode = curnode->next;
		curnode->next = list2->first;
		list2->first->prev = &curnode->next;
		list_init(list2);
	}
}

static inline bool
list_empty(const struct list *list)
{
	return list->first == NULL;
}

static inline bool
list_empty_atomic(const struct list *list)
{
	return atomic_load_ptr(&list->first) == NULL;
}

static inline struct list_node *
list_first(const struct list *list)
{
	return list->first;
}

#define list_foreach(var, list)						\
	for ((var) = list_first(list);					\
	    (var) != NULL;						\
	    (var) = list_next(var))

#define list_foreach_from(var, list)					\
	for ((var) = ((var) != NULL ? (var) : list_first(list));	\
	    (var) != NULL;						\
	    (var) = list_next(var))

#define list_foreach_safe(var, list, tvar)				\
	for ((var) = list_first(list);					\
	    (var) != NULL && ((tvar) = list_next(var), 1);		\
	    (var) = (tvar))

#define list_foreach_from_safe(var, list, tvar)				\
	for ((var) = ((var) != NULL ? (var) : list_first(list));	\
	    (var) != NULL && ((tvar) = list_next(var), 1);		\
	    (var) = (tvar))

static inline void
list_init(struct list *list)
{
	list->first = NULL;
}

static inline void
list_insert_after(struct list_node *listnode, struct list_node *node)
{
	if ((node->next = listnode->next) != NULL)
		listnode->next->prev = &node->next;
	listnode->next = node;
	node->prev = &listnode->next;
}

static inline void
list_insert_before(struct list_node *listnode, struct list_node *node)
{
	node->prev = listnode->prev;
	node->next = listnode;
	*listnode->prev = node;
	listnode->prev = &node->next;
}

static inline void
list_insert_head(struct list *list, struct list_node *node)
{
	if ((node->next = list->first) != NULL)
		list->first->prev = &node->next;
	list->first = node;
	node->prev = &list->first;
}

static inline struct list_node *
list_next(const struct list_node *node)
{
	return node->next;
}

static inline struct list_node *
list_prev(const struct list_node *node, const struct list *list)
{
	return node->prev == &list->first ? NULL :
	    (struct list_node *)((char *)node->prev -
	        offsetof(struct list_node, next));
}

static inline void
list_remove(struct list_node *node)
{
	if (node->next != NULL)
		node->next->prev = node->prev;
	*node->prev = node->next;
}

static inline void
list_replace(struct list_node *node, struct list_node *node2)
{
	node2->next = node->next;
	if (node2->next != NULL)
		node2->next->prev = &node2->next;
	node2->prev = node->prev;
	*node2->prev = node2;
}

static inline void
list_split_after(struct list *list, struct list_node *node, struct list *rest)
{
	if (node->next == NULL) {
		/* 'node' is the last node in 'list'. */
		list_init(rest);
	} else {
		rest->first = node->next;
		node->next->prev = &rest->first;
		node->next = NULL;
	}
}

static inline void
list_swap(struct list *list1, struct list *list2)
{
	struct list_node *swap_tmp = list1->first;
	list1->first = list2->first;
	list2->first = swap_tmp;
	if ((swap_tmp = list1->first) != NULL)
		swap_tmp->prev = &list1->first;
	if ((swap_tmp = list2->first) != NULL)
		swap_tmp->prev = &list2->first;
}

#endif /* !_SYS_LIST_H_ */
