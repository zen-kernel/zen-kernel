/*
 * Copyright (C) 2016 Alfred Chen.
 *
 * Code based on Con Kolivas's skip list implementation for BFS, and
 * which is based on example originally by William Pugh.
 *
 * Skip Lists are a probabilistic alternative to balanced trees, as
 * described in the June 1990 issue of CACM and were invented by
 * William Pugh in 1987.
 *
 * A couple of comments about this implementation:
 *
 * This file only provides a infrastructure of skip list.
 *
 * skiplist_node is embedded into container data structure, to get rid
 * the dependency of kmalloc/kfree operation in scheduler code.
 *
 * A customized search function should be defined using DEFINE_SKIPLIST_INSERT
 * macro and be used for skip list insert operation.
 *
 * Random Level is also not defined in this file, instead, it should be
 * customized implemented and set to node->level then pass to the customized
 * skiplist_insert function.
 *
 * Levels start at zero and go up to (NUM_SKIPLIST_LEVEL -1)
 *
 * NUM_SKIPLIST_LEVEL in this implementation is 8 instead of origin 16,
 * considering that there will be 256 entries to enable the top level when using
 * random level p=0.5, and that number is more than enough for a run queue usage
 * in a scheduler usage. And it also help to reduce the memory usage of the
 * embedded skip list node in task_struct to about 50%.
 *
 * The insertion routine has been implemented so as to use the
 * dirty hack described in the CACM paper: if a random level is
 * generated that is more than the current maximum level, the
 * current maximum level plus one is used instead.
 *
 * BFS Notes: In this implementation of skiplists, there are bidirectional
 * next/prev pointers and the insert function returns a pointer to the actual
 * node the value is stored. The key here is chosen by the scheduler so as to
 * sort tasks according to the priority list requirements and is no longer used
 * by the scheduler after insertion. The scheduler lookup, however, occurs in
 * O(1) time because it is always the first item in the level 0 linked list.
 * Since the task struct stores a copy of the node pointer upon skiplist_insert,
 * it can also remove it much faster than the original implementation with the
 * aid of prev<->next pointer manipulation and no searching.
 */
#ifndef _LINUX_SKIP_LIST_H
#define _LINUX_SKIP_LIST_H

#include <linux/kernel.h>

#define NUM_SKIPLIST_LEVEL (8)

struct skiplist_node {
	int level;	/* Levels in this node */
	struct skiplist_node *next[NUM_SKIPLIST_LEVEL];
	struct skiplist_node *prev[NUM_SKIPLIST_LEVEL];
};

#define SKIPLIST_NODE_INIT(name) { 0,\
				   {&name, &name, &name, &name,\
				    &name, &name, &name, &name},\
				   {&name, &name, &name, &name,\
				    &name, &name, &name, &name},\
				 }

/**
 * INIT_SKIPLIST_NODE -- init a skiplist_node, expecially for header
 * @node: the skip list node to be inited.
 */
static inline void INIT_SKIPLIST_NODE(struct skiplist_node *node)
{
	int i;

	node->level = 0;
	for (i = 0; i < NUM_SKIPLIST_LEVEL; i++) {
		WRITE_ONCE(node->next[i], node);
		node->prev[i] = node;
	}
}

/**
 * skiplist_entry - get the struct for this entry
 * @ptr: the &struct skiplist_node pointer.
 * @type:       the type of the struct this is embedded in.
 * @member:     the name of the skiplist_node within the struct.
 */
#define skiplist_entry(ptr, type, member) \
	container_of(ptr, type, member)

/**
 * DEFINE_SKIPLIST_INSERT_FUNC -- macro to define a customized skip list insert
 * function, which takes two parameters, first one is the header node of the
 * skip list, second one is the skip list node to be inserted
 * @func_name: the customized skip list insert function name
 * @search_func: the search function to be used, which takes two parameters,
 * 1st one is the itrator of skiplist_node in the list, the 2nd is the skip list
 * node to be inserted, the function should return true if search should be
 * continued, otherwise return false.
 * Returns 1 if @node is inserted as the first item of skip list at level zero,
 * otherwise 0
 */
#define DEFINE_SKIPLIST_INSERT_FUNC(func_name, search_func)\
static inline int func_name(struct skiplist_node *head, struct skiplist_node *node)\
{\
	struct skiplist_node *p, *q;\
	unsigned int k = head->level;\
	unsigned int l = node->level;\
\
	p = head;\
	if (l > k) {\
		l = node->level = ++head->level;\
\
		node->next[l] = head;\
		node->prev[l] = head;\
		head->next[l] = node;\
		head->prev[l] = node;\
\
		do {\
			while (q = p->next[k], q != head && search_func(q, node))\
				p = q;\
\
			node->prev[k] = p;\
			node->next[k] = q;\
			q->prev[k] = node;\
			p->next[k] = node;\
		} while (k--);\
\
		return (p == head);\
	}\
\
	while (k > l) {\
		while (q = p->next[k], q != head && search_func(q, node))\
			p = q;\
		k--;\
	}\
\
	do {\
		while (q = p->next[k], q != head && search_func(q, node))\
			p = q;\
\
		node->prev[k] = p;\
		node->next[k] = q;\
		q->prev[k] = node;\
		p->next[k] = node;\
	} while (k--);\
\
	return (p == head);\
}

/**
 * skiplist_del_init -- delete skip list node from a skip list and reset it's
 * init state
 * @head: the header node of the skip list to be deleted from.
 * @node: the skip list node to be deleted, the caller need to ensure @node is
 * in skip list which @head represent.
 * Returns 1 if @node is the first item of skip level at level zero, otherwise 0
 */
static inline int
skiplist_del_init(struct skiplist_node *head, struct skiplist_node *node)
{
	unsigned int i, level = node->level;

	for (i = 0; i <= level; i++) {
		node->prev[i]->next[i] = node->next[i];
		node->next[i]->prev[i] = node->prev[i];
	}
	if (level == head->level && level) {
		while (head->next[level] == head && level)
			level--;
		head->level = level;
	}

	return (node->prev[0] == head);
}
#endif /* _LINUX_SKIP_LIST_H */
