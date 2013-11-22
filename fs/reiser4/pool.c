/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Fast pool allocation.

   There are situations when some sub-system normally asks memory allocator
   for only few objects, but under some circumstances could require much
   more. Typical and actually motivating example is tree balancing. It needs
   to keep track of nodes that were involved into it, and it is well-known
   that in reasonable packed balanced tree most (92.938121%) percent of all
   balancings end up after working with only few nodes (3.141592 on
   average). But in rare cases balancing can involve much more nodes
   (3*tree_height+1 in extremal situation).

   On the one hand, we don't want to resort to dynamic allocation (slab,
    malloc(), etc.) to allocate data structures required to keep track of
   nodes during balancing. On the other hand, we cannot statically allocate
   required amount of space on the stack, because first: it is useless wastage
   of precious resource, and second: this amount is unknown in advance (tree
   height can change).

   Pools, implemented in this file are solution for this problem:

    - some configurable amount of objects is statically preallocated on the
    stack

    - if this preallocated pool is exhausted and more objects is requested
    they are allocated dynamically.

   Pools encapsulate distinction between statically and dynamically allocated
   objects. Both allocation and recycling look exactly the same.

   To keep track of dynamically allocated objects, pool adds its own linkage
   to each object.

   NOTE-NIKITA This linkage also contains some balancing-specific data. This
   is not perfect. On the other hand, balancing is currently the only client
   of pool code.

   NOTE-NIKITA Another desirable feature is to rewrite all pool manipulation
   functions in the style of tslist/tshash, i.e., make them unreadable, but
   type-safe.

*/

#include "debug.h"
#include "pool.h"
#include "super.h"

#include <linux/types.h>
#include <linux/err.h>

/* initialize new pool object @h */
static void reiser4_init_pool_obj(struct reiser4_pool_header *h)
{
	INIT_LIST_HEAD(&h->usage_linkage);
	INIT_LIST_HEAD(&h->level_linkage);
	INIT_LIST_HEAD(&h->extra_linkage);
}

/* initialize new pool */
void reiser4_init_pool(struct reiser4_pool *pool /* pool to initialize */ ,
		       size_t obj_size /* size of objects in @pool */ ,
		       int num_of_objs /* number of preallocated objects */ ,
		       char *data/* area for preallocated objects */)
{
	struct reiser4_pool_header *h;
	int i;

	assert("nikita-955", pool != NULL);
	assert("nikita-1044", obj_size > 0);
	assert("nikita-956", num_of_objs >= 0);
	assert("nikita-957", data != NULL);

	memset(pool, 0, sizeof *pool);
	pool->obj_size = obj_size;
	pool->data = data;
	INIT_LIST_HEAD(&pool->free);
	INIT_LIST_HEAD(&pool->used);
	INIT_LIST_HEAD(&pool->extra);
	memset(data, 0, obj_size * num_of_objs);
	for (i = 0; i < num_of_objs; ++i) {
		h = (struct reiser4_pool_header *) (data + i * obj_size);
		reiser4_init_pool_obj(h);
		/* add pool header to the end of pool's free list */
		list_add_tail(&h->usage_linkage, &pool->free);
	}
}

/* release pool resources

   Release all resources acquired by this pool, specifically, dynamically
   allocated objects.

*/
void reiser4_done_pool(struct reiser4_pool *pool UNUSED_ARG)
{
}

/* allocate carry object from @pool

   First, try to get preallocated object. If this fails, resort to dynamic
   allocation.

*/
static void *reiser4_pool_alloc(struct reiser4_pool *pool)
{
	struct reiser4_pool_header *result;

	assert("nikita-959", pool != NULL);

	if (!list_empty(&pool->free)) {
		struct list_head *linkage;

		linkage = pool->free.next;
		list_del(linkage);
		INIT_LIST_HEAD(linkage);
		result = list_entry(linkage, struct reiser4_pool_header,
				    usage_linkage);
		BUG_ON(!list_empty(&result->level_linkage) ||
		       !list_empty(&result->extra_linkage));
	} else {
		/* pool is empty. Extra allocations don't deserve dedicated
		   slab to be served from, as they are expected to be rare. */
		result = kmalloc(pool->obj_size, reiser4_ctx_gfp_mask_get());
		if (result != 0) {
			reiser4_init_pool_obj(result);
			list_add(&result->extra_linkage, &pool->extra);
		} else
			return ERR_PTR(RETERR(-ENOMEM));
		BUG_ON(!list_empty(&result->usage_linkage) ||
		       !list_empty(&result->level_linkage));
	}
	++pool->objs;
	list_add(&result->usage_linkage, &pool->used);
	memset(result + 1, 0, pool->obj_size - sizeof *result);
	return result;
}

/* return object back to the pool */
void reiser4_pool_free(struct reiser4_pool *pool,
		       struct reiser4_pool_header *h)
{
	assert("nikita-961", h != NULL);
	assert("nikita-962", pool != NULL);

	--pool->objs;
	assert("nikita-963", pool->objs >= 0);

	list_del_init(&h->usage_linkage);
	list_del_init(&h->level_linkage);

	if (list_empty(&h->extra_linkage))
		/*
		 * pool header is not an extra one. Push it onto free list
		 * using usage_linkage
		 */
		list_add(&h->usage_linkage, &pool->free);
	else {
		/* remove pool header from pool's extra list and kfree it */
		list_del(&h->extra_linkage);
		kfree(h);
	}
}

/* add new object to the carry level list

   Carry level is FIFO most of the time, but not always. Complications arise
   when make_space() function tries to go to the left neighbor and thus adds
   carry node before existing nodes, and also, when updating delimiting keys
   after moving data between two nodes, we want left node to be locked before
   right node.

   Latter case is confusing at the first glance. Problem is that COP_UPDATE
   opration that updates delimiting keys is sometimes called with two nodes
   (when data are moved between two nodes) and sometimes with only one node
   (when leftmost item is deleted in a node). In any case operation is
   supplied with at least node whose left delimiting key is to be updated
   (that is "right" node).

   @pool - from which to allocate new object;
   @list - where to add object;
   @reference - after (or before) which existing object to add
*/
struct reiser4_pool_header *reiser4_add_obj(struct reiser4_pool *pool,
					 struct list_head *list,
					 pool_ordering order,
					 struct reiser4_pool_header *reference)
{
	struct reiser4_pool_header *result;

	assert("nikita-972", pool != NULL);

	result = reiser4_pool_alloc(pool);
	if (IS_ERR(result))
		return result;

	assert("nikita-973", result != NULL);

	switch (order) {
	case POOLO_BEFORE:
		__list_add(&result->level_linkage,
			   reference->level_linkage.prev,
			   &reference->level_linkage);
		break;
	case POOLO_AFTER:
		__list_add(&result->level_linkage,
			   &reference->level_linkage,
			   reference->level_linkage.next);
		break;
	case POOLO_LAST:
		list_add_tail(&result->level_linkage, list);
		break;
	case POOLO_FIRST:
		list_add(&result->level_linkage, list);
		break;
	default:
		wrong_return_value("nikita-927", "order");
	}
	return result;
}

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
