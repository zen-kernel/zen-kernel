/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
   reiser4/README */
/* Functions to "carry" tree modification(s) upward. */
/* Tree is modified one level at a time. As we modify a level we accumulate a
   set of changes that need to be propagated to the next level.  We manage
   node locking such that any searches that collide with carrying are
   restarted, from the root if necessary.

   Insertion of a new item may result in items being moved among nodes and
   this requires the delimiting key to be updated at the least common parent
   of the nodes modified to preserve search tree invariants. Also, insertion
   may require allocation of a new node. A pointer to the new node has to be
   inserted into some node on the parent level, etc.

   Tree carrying is meant to be analogous to arithmetic carrying.

   A carry operation is always associated with some node (&carry_node).

   Carry process starts with some initial set of operations to be performed
   and an initial set of already locked nodes.  Operations are performed one
   by one. Performing each single operation has following possible effects:

    - content of carry node associated with operation is modified
    - new carry nodes are locked and involved into carry process on this level
    - new carry operations are posted to the next level

   After all carry operations on this level are done, process is repeated for
   the accumulated sequence on carry operations for the next level. This
   starts by trying to lock (in left to right order) all carry nodes
   associated with carry operations on the parent level. After this, we decide
   whether more nodes are required on the left of already locked set. If so,
   all locks taken on the parent level are released, new carry nodes are
   added, and locking process repeats.

   It may happen that balancing process fails owing to unrecoverable error on
   some of upper levels of a tree (possible causes are io error, failure to
   allocate new node, etc.). In this case we should unmount the filesystem,
   rebooting if it is the root, and possibly advise the use of fsck.

   USAGE:

    int some_tree_operation( znode *node, ... )
    {
       // Allocate on a stack pool of carry objects: operations and nodes.
       // Most carry processes will only take objects from here, without
       // dynamic allocation.

I feel uneasy about this pool.  It adds to code complexity, I understand why it
exists, but.... -Hans

       carry_pool  pool;
       carry_level lowest_level;
       carry_op   *op;

       init_carry_pool( &pool );
       init_carry_level( &lowest_level, &pool );

       // operation may be one of:
       //   COP_INSERT    --- insert new item into node
       //   COP_CUT       --- remove part of or whole node
       //   COP_PASTE     --- increase size of item
       //   COP_DELETE    --- delete pointer from parent node
       //   COP_UPDATE    --- update delimiting key in least
       //                     common ancestor of two

       op = reiser4_post_carry( &lowest_level, operation, node, 0 );
       if( IS_ERR( op ) || ( op == NULL ) ) {
		handle error
       } else {
	// fill in remaining fields in @op, according to carry.h:carry_op
		result = carry(&lowest_level, NULL);
       }
       done_carry_pool(&pool);
    }

   When you are implementing node plugin method that participates in carry
   (shifting, insertion, deletion, etc.), do the following:

   int foo_node_method(znode * node, ..., carry_level * todo)
   {
       carry_op   *op;

       ....

	// note, that last argument to reiser4_post_carry() is non-null
	// here, because @op is to be applied to the parent of @node, rather
	// than to the @node itself as in the previous case.

       op = node_post_carry(todo, operation, node, 1);
       // fill in remaining fields in @op, according to carry.h:carry_op

       ....

   }

   BATCHING:

   One of the main advantages of level-by-level balancing implemented here is
   ability to batch updates on a parent level and to peform them more
   efficiently as a result.

   Description To Be Done (TBD).

   DIFFICULTIES AND SUBTLE POINTS:

   1. complex plumbing is required, because:

       a. effective allocation through pools is needed

       b. target of operation is not exactly known when operation is
       posted. This is worked around through bitfields in &carry_node and
       logic in lock_carry_node()

       c. of interaction with locking code: node should be added into sibling
       list when pointer to it is inserted into its parent, which is some time
       after node was created. Between these moments, node is somewhat in
       suspended state and is only registered in the carry lists

    2. whole balancing logic is implemented here, in particular, insertion
    logic is coded in make_space().

    3. special cases like insertion (reiser4_add_tree_root()) or deletion
    (reiser4_kill_tree_root()) of tree root and morphing of paste into insert
    (insert_paste()) have to be handled.

    4. there is non-trivial interdependency between allocation of new nodes
    and almost everything else. This is mainly due to the (1.c) above. I shall
    write about this later.

*/

#include "forward.h"
#include "debug.h"
#include "key.h"
#include "coord.h"
#include "plugin/item/item.h"
#include "plugin/item/extent.h"
#include "plugin/node/node.h"
#include "jnode.h"
#include "znode.h"
#include "tree_mod.h"
#include "tree_walk.h"
#include "block_alloc.h"
#include "pool.h"
#include "tree.h"
#include "carry.h"
#include "carry_ops.h"
#include "super.h"
#include "reiser4.h"

#include <linux/types.h>

/* level locking/unlocking */
static int lock_carry_level(carry_level * level);
static void unlock_carry_level(carry_level * level, int failure);
static void done_carry_level(carry_level * level);
static void unlock_carry_node(carry_level * level, carry_node * node, int fail);

int lock_carry_node(carry_level * level, carry_node * node);
int lock_carry_node_tail(carry_node * node);

/* carry processing proper */
static int carry_on_level(carry_level * doing, carry_level * todo);

static carry_op *add_op(carry_level * level, pool_ordering order,
			carry_op * reference);

/* handlers for carry operations. */

static void fatal_carry_error(carry_level * doing, int ecode);
static int add_new_root(carry_level * level, carry_node * node, znode * fake);

static void print_level(const char *prefix, carry_level * level);

#if REISER4_DEBUG
typedef enum {
	CARRY_TODO,
	CARRY_DOING
} carry_queue_state;
static int carry_level_invariant(carry_level * level, carry_queue_state state);
#endif

/* main entry point for tree balancing.

   Tree carry performs operations from @doing and while doing so accumulates
   information about operations to be performed on the next level ("carried"
   to the parent level). Carried operations are performed, causing possibly
   more operations to be carried upward etc. carry() takes care about
   locking and pinning znodes while operating on them.

   For usage, see comment at the top of fs/reiser4/carry.c

*/
int reiser4_carry(carry_level * doing /* set of carry operations to be
				       * performed */ ,
		  carry_level * done  /* set of nodes, already performed
				       *  at the previous level.
				       * NULL in most cases */)
{
	int result = 0;
	/* queue of new requests */
	carry_level *todo;
	ON_DEBUG(STORE_COUNTERS);

	assert("nikita-888", doing != NULL);
	BUG_ON(done != NULL);

	todo = doing + 1;
	init_carry_level(todo, doing->pool);

	/* queue of requests preformed on the previous level */
	done = todo + 1;
	init_carry_level(done, doing->pool);

	/* iterate until there is nothing more to do */
	while (result == 0 && doing->ops_num > 0) {
		carry_level *tmp;

		/* at this point @done is locked. */
		/* repeat lock/do/unlock while

		   (1) lock_carry_level() fails due to deadlock avoidance, or

		   (2) carry_on_level() decides that more nodes have to
		   be involved.

		   (3) some unexpected error occurred while balancing on the
		   upper levels. In this case all changes are rolled back.

		 */
		while (1) {
			result = lock_carry_level(doing);
			if (result == 0) {
				/* perform operations from @doing and
				   accumulate new requests in @todo */
				result = carry_on_level(doing, todo);
				if (result == 0)
					break;
				else if (result != -E_REPEAT ||
					 !doing->restartable) {
					warning("nikita-1043",
						"Fatal error during carry: %i",
						result);
					print_level("done", done);
					print_level("doing", doing);
					print_level("todo", todo);
					/* do some rough stuff like aborting
					   all pending transcrashes and thus
					   pushing tree back to the consistent
					   state. Alternatvely, just panic.
					 */
					fatal_carry_error(doing, result);
					return result;
				}
			} else if (result != -E_REPEAT) {
				fatal_carry_error(doing, result);
				return result;
			}
			unlock_carry_level(doing, 1);
		}
		/* at this point @done can be safely unlocked */
		done_carry_level(done);

		/* cyclically shift queues */
		tmp = done;
		done = doing;
		doing = todo;
		todo = tmp;
		init_carry_level(todo, doing->pool);

		/* give other threads chance to run */
		reiser4_preempt_point();
	}
	done_carry_level(done);

	/* all counters, but x_refs should remain the same. x_refs can change
	   owing to transaction manager */
	ON_DEBUG(CHECK_COUNTERS);
	return result;
}

/* perform carry operations on given level.

   Optimizations proposed by pooh:

   (1) don't lock all nodes from queue at the same time. Lock nodes lazily as
   required;

   (2) unlock node if there are no more operations to be performed upon it and
   node didn't add any operation to @todo. This can be implemented by
   attaching to each node two counters: counter of operaions working on this
   node and counter and operations carried upward from this node.

*/
static int carry_on_level(carry_level * doing	/* queue of carry operations to
						 * do on this level */ ,
			  carry_level * todo	/* queue where new carry
						 * operations to be performed on
						 * the * parent level are
						 * accumulated during @doing
						 * processing. */ )
{
	int result;
	int (*f) (carry_op *, carry_level *, carry_level *);
	carry_op *op;
	carry_op *tmp_op;

	assert("nikita-1034", doing != NULL);
	assert("nikita-1035", todo != NULL);

	/* @doing->nodes are locked. */

	/* This function can be split into two phases: analysis and modification

	   Analysis calculates precisely what items should be moved between
	   nodes. This information is gathered in some structures attached to
	   each carry_node in a @doing queue. Analysis also determines whether
	   new nodes are to be allocated etc.

	   After analysis is completed, actual modification is performed. Here
	   we can take advantage of "batch modification": if there are several
	   operations acting on the same node, modifications can be performed
	   more efficiently when batched together.

	   Above is an optimization left for the future.
	 */
	/* Important, but delayed optimization: it's possible to batch
	   operations together and perform them more efficiently as a
	   result. For example, deletion of several neighboring items from a
	   node can be converted to a single ->cut() operation.

	   Before processing queue, it should be scanned and "mergeable"
	   operations merged.
	 */
	result = 0;
	for_all_ops(doing, op, tmp_op) {
		carry_opcode opcode;

		assert("nikita-1041", op != NULL);
		opcode = op->op;
		assert("nikita-1042", op->op < COP_LAST_OP);
		f = op_dispatch_table[op->op].handler;
		result = f(op, doing, todo);
		/* locking can fail with -E_REPEAT. Any different error is fatal
		   and will be handled by fatal_carry_error() sledgehammer.
		 */
		if (result != 0)
			break;
	}
	if (result == 0) {
		carry_plugin_info info;
		carry_node *scan;
		carry_node *tmp_scan;

		info.doing = doing;
		info.todo = todo;

		assert("nikita-3002",
		       carry_level_invariant(doing, CARRY_DOING));
		for_all_nodes(doing, scan, tmp_scan) {
			znode *node;

			node = reiser4_carry_real(scan);
			assert("nikita-2547", node != NULL);
			if (node_is_empty(node)) {
				result =
				    node_plugin_by_node(node)->
				    prepare_removal(node, &info);
				if (result != 0)
					break;
			}
		}
	}
	return result;
}

/* post carry operation

   This is main function used by external carry clients: node layout plugins
   and tree operations to create new carry operation to be performed on some
   level.

   New operation will be included in the @level queue. To actually perform it,
   call carry( level, ... ). This function takes write lock on @node. Carry
   manages all its locks by itself, don't worry about this.

   This function adds operation and node at the end of the queue. It is up to
   caller to guarantee proper ordering of node queue.

*/
carry_op * reiser4_post_carry(carry_level * level /* queue where new operation
						   * is to be posted at */ ,
			      carry_opcode op /* opcode of operation */ ,
			      znode * node	/* node on which this operation
						 * will operate */ ,
			      int apply_to_parent_p /* whether operation will
						     * operate directly on @node
						     * or on it parent. */)
{
	carry_op *result;
	carry_node *child;

	assert("nikita-1046", level != NULL);
	assert("nikita-1788", znode_is_write_locked(node));

	result = add_op(level, POOLO_LAST, NULL);
	if (IS_ERR(result))
		return result;
	child = reiser4_add_carry(level, POOLO_LAST, NULL);
	if (IS_ERR(child)) {
		reiser4_pool_free(&level->pool->op_pool, &result->header);
		return (carry_op *) child;
	}
	result->node = child;
	result->op = op;
	child->parent = apply_to_parent_p;
	if (ZF_ISSET(node, JNODE_ORPHAN))
		child->left_before = 1;
	child->node = node;
	return result;
}

/* initialize carry queue */
void init_carry_level(carry_level * level /* level to initialize */ ,
		      carry_pool * pool	/* pool @level will allocate objects
					 * from */ )
{
	assert("nikita-1045", level != NULL);
	assert("nikita-967", pool != NULL);

	memset(level, 0, sizeof *level);
	level->pool = pool;

	INIT_LIST_HEAD(&level->nodes);
	INIT_LIST_HEAD(&level->ops);
}

/* allocate carry pool and initialize pools within queue */
carry_pool *init_carry_pool(int size)
{
	carry_pool *pool;

	assert("", size >= sizeof(carry_pool) + 3 * sizeof(carry_level));
	pool = kmalloc(size, reiser4_ctx_gfp_mask_get());
	if (pool == NULL)
		return ERR_PTR(RETERR(-ENOMEM));

	reiser4_init_pool(&pool->op_pool, sizeof(carry_op), CARRIES_POOL_SIZE,
			  (char *)pool->op);
	reiser4_init_pool(&pool->node_pool, sizeof(carry_node),
			  NODES_LOCKED_POOL_SIZE, (char *)pool->node);
	return pool;
}

/* finish with queue pools */
void done_carry_pool(carry_pool * pool/* pool to destroy */)
{
	reiser4_done_pool(&pool->op_pool);
	reiser4_done_pool(&pool->node_pool);
	kfree(pool);
}

/* add new carry node to the @level.

   Returns pointer to the new carry node allocated from pool.  It's up to
   callers to maintain proper order in the @level. Assumption is that if carry
   nodes on one level are already sorted and modifications are peroformed from
   left to right, carry nodes added on the parent level will be ordered
   automatically. To control ordering use @order and @reference parameters.

*/
carry_node *reiser4_add_carry_skip(carry_level * level	/* &carry_level to add
							 * node to */ ,
				   pool_ordering order	/* where to insert:
							 * at the beginning of
							 * @level,
							 * before @reference,
							 * after @reference,
							 * at the end of @level
							 */ ,
				   carry_node * reference/* reference node for
							  * insertion */)
{
	ON_DEBUG(carry_node * orig_ref = reference);

	if (order == POOLO_BEFORE) {
		reference = find_left_carry(reference, level);
		if (reference == NULL)
			reference = list_entry(level->nodes.next, carry_node,
					       header.level_linkage);
		else
			reference = list_entry(reference->header.level_linkage.next,
					       carry_node, header.level_linkage);
	} else if (order == POOLO_AFTER) {
		reference = find_right_carry(reference, level);
		if (reference == NULL)
			reference = list_entry(level->nodes.prev, carry_node,
					       header.level_linkage);
		else
			reference = list_entry(reference->header.level_linkage.prev,
					       carry_node, header.level_linkage);
	}
	assert("nikita-2209",
	       ergo(orig_ref != NULL,
		    reiser4_carry_real(reference) ==
		    reiser4_carry_real(orig_ref)));
	return reiser4_add_carry(level, order, reference);
}

carry_node *reiser4_add_carry(carry_level * level,   /* carry_level to add
							node to */
			      pool_ordering order,   /* where to insert:
						      * at the beginning of
						      * @level;
						      * before @reference;
						      * after @reference;
						      * at the end of @level
						      */
			      carry_node * reference /* reference node for
						      * insertion */)
{
	carry_node *result;

	result =
	    (carry_node *) reiser4_add_obj(&level->pool->node_pool,
					   &level->nodes,
					   order, &reference->header);
	if (!IS_ERR(result) && (result != NULL))
		++level->nodes_num;
	return result;
}

/**
 * add new carry operation to the @level.
 *
 * Returns pointer to the new carry operations allocated from pool. It's up to
 * callers to maintain proper order in the @level. To control ordering use
 * @order and @reference parameters.
 */
static carry_op *add_op(carry_level * level, /* &carry_level to add node to */
			pool_ordering order, /* where to insert:
					      * at the beginning of @level;
					      * before @reference;
					      * after @reference;
					      * at the end of @level */
			carry_op * reference /* reference node for insertion */)
{
	carry_op *result;

	result =
	    (carry_op *) reiser4_add_obj(&level->pool->op_pool, &level->ops,
					 order, &reference->header);
	if (!IS_ERR(result) && (result != NULL))
		++level->ops_num;
	return result;
}

/**
 * Return node on the right of which @node was created.
 *
 * Each node is created on the right of some existing node (or it is new root,
 * which is special case not handled here).
 *
 * @node is new node created on some level, but not yet inserted into its
 * parent, it has corresponding bit (JNODE_ORPHAN) set in zstate.
 */
static carry_node *find_begetting_brother(carry_node * node,/* node to start
								search from */
					  carry_level * kin UNUSED_ARG
					                    /* level to scan */)
{
	carry_node *scan;

	assert("nikita-1614", node != NULL);
	assert("nikita-1615", kin != NULL);
	assert("nikita-1616", LOCK_CNT_GTZ(rw_locked_tree));
	assert("nikita-1619", ergo(reiser4_carry_real(node) != NULL,
				   ZF_ISSET(reiser4_carry_real(node),
					    JNODE_ORPHAN)));
	for (scan = node;;
	     scan = list_entry(scan->header.level_linkage.prev, carry_node,
			       header.level_linkage)) {
		assert("nikita-1617", &kin->nodes != &scan->header.level_linkage);
		if ((scan->node != node->node) &&
		    !ZF_ISSET(scan->node, JNODE_ORPHAN)) {
			assert("nikita-1618", reiser4_carry_real(scan) != NULL);
			break;
		}
	}
	return scan;
}

static cmp_t
carry_node_cmp(carry_level * level, carry_node * n1, carry_node * n2)
{
	assert("nikita-2199", n1 != NULL);
	assert("nikita-2200", n2 != NULL);

	if (n1 == n2)
		return EQUAL_TO;
	while (1) {
		n1 = carry_node_next(n1);
		if (carry_node_end(level, n1))
			return GREATER_THAN;
		if (n1 == n2)
			return LESS_THAN;
	}
	impossible("nikita-2201", "End of level reached");
}

carry_node *find_carry_node(carry_level * level, const znode * node)
{
	carry_node *scan;
	carry_node *tmp_scan;

	assert("nikita-2202", level != NULL);
	assert("nikita-2203", node != NULL);

	for_all_nodes(level, scan, tmp_scan) {
		if (reiser4_carry_real(scan) == node)
			return scan;
	}
	return NULL;
}

znode *reiser4_carry_real(const carry_node * node)
{
	assert("nikita-3061", node != NULL);

	return node->lock_handle.node;
}

carry_node *insert_carry_node(carry_level * doing, carry_level * todo,
			      const znode * node)
{
	carry_node *base;
	carry_node *scan;
	carry_node *tmp_scan;
	carry_node *proj;

	base = find_carry_node(doing, node);
	assert("nikita-2204", base != NULL);

	for_all_nodes(todo, scan, tmp_scan) {
		proj = find_carry_node(doing, scan->node);
		assert("nikita-2205", proj != NULL);
		if (carry_node_cmp(doing, proj, base) != LESS_THAN)
			break;
	}
	return scan;
}

static carry_node *add_carry_atplace(carry_level * doing, carry_level * todo,
				     znode * node)
{
	carry_node *reference;

	assert("nikita-2994", doing != NULL);
	assert("nikita-2995", todo != NULL);
	assert("nikita-2996", node != NULL);

	reference = insert_carry_node(doing, todo, node);
	assert("nikita-2997", reference != NULL);

	return reiser4_add_carry(todo, POOLO_BEFORE, reference);
}

/* like reiser4_post_carry(), but designed to be called from node plugin
   methods. This function is different from reiser4_post_carry() in that it
   finds proper place to insert node in the queue. */
carry_op *node_post_carry(carry_plugin_info * info	/* carry parameters
							 * passed down to node
							 * plugin */ ,
			  carry_opcode op /* opcode of operation */ ,
			  znode * node	/* node on which this
					 * operation will operate */ ,
			  int apply_to_parent_p	/* whether operation will
						 * operate directly on @node
						 * or on it parent. */ )
{
	carry_op *result;
	carry_node *child;

	assert("nikita-2207", info != NULL);
	assert("nikita-2208", info->todo != NULL);

	if (info->doing == NULL)
		return reiser4_post_carry(info->todo, op, node,
					  apply_to_parent_p);

	result = add_op(info->todo, POOLO_LAST, NULL);
	if (IS_ERR(result))
		return result;
	child = add_carry_atplace(info->doing, info->todo, node);
	if (IS_ERR(child)) {
		reiser4_pool_free(&info->todo->pool->op_pool, &result->header);
		return (carry_op *) child;
	}
	result->node = child;
	result->op = op;
	child->parent = apply_to_parent_p;
	if (ZF_ISSET(node, JNODE_ORPHAN))
		child->left_before = 1;
	child->node = node;
	return result;
}

/* lock all carry nodes in @level */
static int lock_carry_level(carry_level * level/* level to lock */)
{
	int result;
	carry_node *node;
	carry_node *tmp_node;

	assert("nikita-881", level != NULL);
	assert("nikita-2229", carry_level_invariant(level, CARRY_TODO));

	/* lock nodes from left to right */
	result = 0;
	for_all_nodes(level, node, tmp_node) {
		result = lock_carry_node(level, node);
		if (result != 0)
			break;
	}
	return result;
}

/* Synchronize delimiting keys between @node and its left neighbor.

   To reduce contention on dk key and simplify carry code, we synchronize
   delimiting keys only when carry ultimately leaves tree level (carrying
   changes upward) and unlocks nodes at this level.

   This function first finds left neighbor of @node and then updates left
   neighbor's right delimiting key to conincide with least key in @node.

*/

ON_DEBUG(extern atomic_t delim_key_version;
    )

static void sync_dkeys(znode * spot/* node to update */)
{
	reiser4_key pivot;
	reiser4_tree *tree;

	assert("nikita-1610", spot != NULL);
	assert("nikita-1612", LOCK_CNT_NIL(rw_locked_dk));

	tree = znode_get_tree(spot);
	read_lock_tree(tree);
	write_lock_dk(tree);

	assert("nikita-2192", znode_is_loaded(spot));

	/* sync left delimiting key of @spot with key in its leftmost item */
	if (node_is_empty(spot))
		pivot = *znode_get_rd_key(spot);
	else
		leftmost_key_in_node(spot, &pivot);

	znode_set_ld_key(spot, &pivot);

	/* there can be sequence of empty nodes pending removal on the left of
	   @spot. Scan them and update their left and right delimiting keys to
	   match left delimiting key of @spot. Also, update right delimiting
	   key of first non-empty left neighbor.
	 */
	while (1) {
		if (!ZF_ISSET(spot, JNODE_LEFT_CONNECTED))
			break;

		spot = spot->left;
		if (spot == NULL)
			break;

		znode_set_rd_key(spot, &pivot);
		/* don't sink into the domain of another balancing */
		if (!znode_is_write_locked(spot))
			break;
		if (ZF_ISSET(spot, JNODE_HEARD_BANSHEE))
			znode_set_ld_key(spot, &pivot);
		else
			break;
	}

	write_unlock_dk(tree);
	read_unlock_tree(tree);
}

/* unlock all carry nodes in @level */
static void unlock_carry_level(carry_level * level /* level to unlock */ ,
			       int failure	/* true if unlocking owing to
						 * failure */ )
{
	carry_node *node;
	carry_node *tmp_node;

	assert("nikita-889", level != NULL);

	if (!failure) {
		znode *spot;

		spot = NULL;
		/* update delimiting keys */
		for_all_nodes(level, node, tmp_node) {
			if (reiser4_carry_real(node) != spot) {
				spot = reiser4_carry_real(node);
				sync_dkeys(spot);
			}
		}
	}

	/* nodes can be unlocked in arbitrary order.  In preemptible
	   environment it's better to unlock in reverse order of locking,
	   though.
	 */
	for_all_nodes_back(level, node, tmp_node) {
		/* all allocated nodes should be already linked to their
		   parents at this moment. */
		assert("nikita-1631",
		       ergo(!failure, !ZF_ISSET(reiser4_carry_real(node),
						JNODE_ORPHAN)));
		ON_DEBUG(check_dkeys(reiser4_carry_real(node)));
		unlock_carry_node(level, node, failure);
	}
	level->new_root = NULL;
}

/* finish with @level

   Unlock nodes and release all allocated resources */
static void done_carry_level(carry_level * level/* level to finish */)
{
	carry_node *node;
	carry_node *tmp_node;
	carry_op *op;
	carry_op *tmp_op;

	assert("nikita-1076", level != NULL);

	unlock_carry_level(level, 0);
	for_all_nodes(level, node, tmp_node) {
		assert("nikita-2113", list_empty_careful(&node->lock_handle.locks_link));
		assert("nikita-2114", list_empty_careful(&node->lock_handle.owners_link));
		reiser4_pool_free(&level->pool->node_pool, &node->header);
	}
	for_all_ops(level, op, tmp_op)
	    reiser4_pool_free(&level->pool->op_pool, &op->header);
}

/* helper function to complete locking of carry node

   Finish locking of carry node. There are several ways in which new carry
   node can be added into carry level and locked. Normal is through
   lock_carry_node(), but also from find_{left|right}_neighbor(). This
   function factors out common final part of all locking scenarios. It
   supposes that @node -> lock_handle is lock handle for lock just taken and
   fills ->real_node from this lock handle.

*/
int lock_carry_node_tail(carry_node * node/* node to complete locking of */)
{
	assert("nikita-1052", node != NULL);
	assert("nikita-1187", reiser4_carry_real(node) != NULL);
	assert("nikita-1188", !node->unlock);

	node->unlock = 1;
	/* Load node content into memory and install node plugin by
	   looking at the node header.

	   Most of the time this call is cheap because the node is
	   already in memory.

	   Corresponding zrelse() is in unlock_carry_node()
	 */
	return zload(reiser4_carry_real(node));
}

/* lock carry node

   "Resolve" node to real znode, lock it and mark as locked.
   This requires recursive locking of znodes.

   When operation is posted to the parent level, node it will be applied to is
   not yet known. For example, when shifting data between two nodes,
   delimiting has to be updated in parent or parents of nodes involved. But
   their parents is not yet locked and, moreover said nodes can be reparented
   by concurrent balancing.

   To work around this, carry operation is applied to special "carry node"
   rather than to the znode itself. Carry node consists of some "base" or
   "reference" znode and flags indicating how to get to the target of carry
   operation (->real_node field of carry_node) from base.

*/
int lock_carry_node(carry_level * level /* level @node is in */ ,
		    carry_node * node/* node to lock */)
{
	int result;
	znode *reference_point;
	lock_handle lh;
	lock_handle tmp_lh;
	reiser4_tree *tree;

	assert("nikita-887", level != NULL);
	assert("nikita-882", node != NULL);

	result = 0;
	reference_point = node->node;
	init_lh(&lh);
	init_lh(&tmp_lh);
	if (node->left_before) {
		/* handling of new nodes, allocated on the previous level:

		   some carry ops were propably posted from the new node, but
		   this node neither has parent pointer set, nor is
		   connected. This will be done in ->create_hook() for
		   internal item.

		   No then less, parent of new node has to be locked. To do
		   this, first go to the "left" in the carry order. This
		   depends on the decision to always allocate new node on the
		   right of existing one.

		   Loop handles case when multiple nodes, all orphans, were
		   inserted.

		   Strictly speaking, taking tree lock is not necessary here,
		   because all nodes scanned by loop in
		   find_begetting_brother() are write-locked by this thread,
		   and thus, their sibling linkage cannot change.

		 */
		tree = znode_get_tree(reference_point);
		read_lock_tree(tree);
		reference_point = find_begetting_brother(node, level)->node;
		read_unlock_tree(tree);
		assert("nikita-1186", reference_point != NULL);
	}
	if (node->parent && (result == 0)) {
		result =
		    reiser4_get_parent(&tmp_lh, reference_point,
				       ZNODE_WRITE_LOCK);
		if (result != 0) {
			;	/* nothing */
		} else if (znode_get_level(tmp_lh.node) == 0) {
			assert("nikita-1347", znode_above_root(tmp_lh.node));
			result = add_new_root(level, node, tmp_lh.node);
			if (result == 0) {
				reference_point = level->new_root;
				move_lh(&lh, &node->lock_handle);
			}
		} else if ((level->new_root != NULL)
			   && (level->new_root !=
			       znode_parent_nolock(reference_point))) {
			/* parent of node exists, but this level aready
			   created different new root, so */
			warning("nikita-1109",
				/* it should be "radicis", but tradition is
				   tradition.  do banshees read latin? */
				"hodie natus est radici frater");
			result = -EIO;
		} else {
			move_lh(&lh, &tmp_lh);
			reference_point = lh.node;
		}
	}
	if (node->left && (result == 0)) {
		assert("nikita-1183", node->parent);
		assert("nikita-883", reference_point != NULL);
		result =
		    reiser4_get_left_neighbor(&tmp_lh, reference_point,
					      ZNODE_WRITE_LOCK,
					      GN_CAN_USE_UPPER_LEVELS);
		if (result == 0) {
			done_lh(&lh);
			move_lh(&lh, &tmp_lh);
			reference_point = lh.node;
		}
	}
	if (!node->parent && !node->left && !node->left_before) {
		result =
		    longterm_lock_znode(&lh, reference_point, ZNODE_WRITE_LOCK,
					ZNODE_LOCK_HIPRI);
	}
	if (result == 0) {
		move_lh(&node->lock_handle, &lh);
		result = lock_carry_node_tail(node);
	}
	done_lh(&tmp_lh);
	done_lh(&lh);
	return result;
}

/* release a lock on &carry_node.

   Release if necessary lock on @node. This opearion is pair of
   lock_carry_node() and is idempotent: you can call it more than once on the
   same node.

*/
static void
unlock_carry_node(carry_level * level,
		  carry_node * node /* node to be released */ ,
		  int failure	/* 0 if node is unlocked due
				 * to some error */ )
{
	znode *real_node;

	assert("nikita-884", node != NULL);

	real_node = reiser4_carry_real(node);
	/* pair to zload() in lock_carry_node_tail() */
	zrelse(real_node);
	if (node->unlock && (real_node != NULL)) {
		assert("nikita-899", real_node == node->lock_handle.node);
		longterm_unlock_znode(&node->lock_handle);
	}
	if (failure) {
		if (node->deallocate && (real_node != NULL)) {
			/* free node in bitmap

			   Prepare node for removal. Last zput() will finish
			   with it.
			 */
			ZF_SET(real_node, JNODE_HEARD_BANSHEE);
		}
		if (node->free) {
			assert("nikita-2177",
			       list_empty_careful(&node->lock_handle.locks_link));
			assert("nikita-2112",
			       list_empty_careful(&node->lock_handle.owners_link));
			reiser4_pool_free(&level->pool->node_pool,
					  &node->header);
		}
	}
}

/* fatal_carry_error() - all-catching error handling function

   It is possible that carry faces unrecoverable error, like unability to
   insert pointer at the internal level. Our simple solution is just panic in
   this situation. More sophisticated things like attempt to remount
   file-system as read-only can be implemented without much difficlties.

   It is believed, that:

   1. in stead of panicking, all current transactions can be aborted rolling
   system back to the consistent state.

Umm, if you simply panic without doing anything more at all, then all current
transactions are aborted and the system is rolled back to a consistent state,
by virtue of the design of the transactional mechanism. Well, wait, let's be
precise.  If an internal node is corrupted on disk due to hardware failure,
then there may be no consistent state that can be rolled back to, so instead
we should say that it will rollback the transactions, which barring other
factors means rolling back to a consistent state.

# Nikita: there is a subtle difference between panic and aborting
# transactions: machine doesn't reboot. Processes aren't killed. Processes
# don't using reiser4 (not that we care about such processes), or using other
# reiser4 mounts (about them we do care) will simply continue to run. With
# some luck, even application using aborted file system can survive: it will
# get some error, like EBADF, from each file descriptor on failed file system,
# but applications that do care about tolerance will cope with this (squid
# will).

It would be a nice feature though to support rollback without rebooting
followed by remount, but this can wait for later versions.

   2. once isolated transactions will be implemented it will be possible to
   roll back offending transaction.

2. is additional code complexity of inconsistent value (it implies that a
broken tree should be kept in operation), so we must think about it more
before deciding if it should be done.  -Hans

*/
static void fatal_carry_error(carry_level * doing UNUSED_ARG	/* carry level
								 * where
								 * unrecoverable
								 * error
								 * occurred */ ,
			      int ecode/* error code */)
{
	assert("nikita-1230", doing != NULL);
	assert("nikita-1231", ecode < 0);

	reiser4_panic("nikita-1232", "Carry failed: %i", ecode);
}

/**
 * Add new root to the tree
 *
 * This function itself only manages changes in carry structures and delegates
 * all hard work (allocation of znode for new root, changes of parent and
 * sibling pointers) to the reiser4_add_tree_root().
 *
 * Locking: old tree root is locked by carry at this point. Fake znode is also
 * locked.
 */
static int add_new_root(carry_level * level,/* carry level in context of which
					     * operation is performed */
			carry_node * node,  /* carry node for existing root */
			znode * fake	    /* "fake" znode already locked by
					     * us */)
{
	int result;

	assert("nikita-1104", level != NULL);
	assert("nikita-1105", node != NULL);

	assert("nikita-1403", znode_is_write_locked(node->node));
	assert("nikita-1404", znode_is_write_locked(fake));

	/* trying to create new root. */
	/* @node is root and it's already locked by us. This
	   means that nobody else can be trying to add/remove
	   tree root right now.
	 */
	if (level->new_root == NULL)
		level->new_root = reiser4_add_tree_root(node->node, fake);
	if (!IS_ERR(level->new_root)) {
		assert("nikita-1210", znode_is_root(level->new_root));
		node->deallocate = 1;
		result =
		    longterm_lock_znode(&node->lock_handle, level->new_root,
					ZNODE_WRITE_LOCK, ZNODE_LOCK_LOPRI);
		if (result == 0)
			zput(level->new_root);
	} else {
		result = PTR_ERR(level->new_root);
		level->new_root = NULL;
	}
	return result;
}

/* allocate new znode and add the operation that inserts the
   pointer to it into the parent node into the todo level

   Allocate new znode, add it into carry queue and post into @todo queue
   request to add pointer to new node into its parent.

   This is carry related routing that calls reiser4_new_node() to allocate new
   node.
*/
carry_node *add_new_znode(znode * brother	/* existing left neighbor of new
						 * node */ ,
			  carry_node * ref	/* carry node after which new
						 * carry node is to be inserted
						 * into queue. This affects
						 * locking. */ ,
			  carry_level * doing	/* carry queue where new node is
						 * to be added */ ,
			  carry_level * todo	/* carry queue where COP_INSERT
						 * operation to add pointer to
						 * new node will ne added */ )
{
	carry_node *fresh;
	znode *new_znode;
	carry_op *add_pointer;
	carry_plugin_info info;

	assert("nikita-1048", brother != NULL);
	assert("nikita-1049", todo != NULL);

	/* There is a lot of possible variations here: to what parent
	   new node will be attached and where. For simplicity, always
	   do the following:

	   (1) new node and @brother will have the same parent.

	   (2) new node is added on the right of @brother

	 */

	fresh =	reiser4_add_carry_skip(doing,
				       ref ? POOLO_AFTER : POOLO_LAST, ref);
	if (IS_ERR(fresh))
		return fresh;

	fresh->deallocate = 1;
	fresh->free = 1;

	new_znode = reiser4_new_node(brother, znode_get_level(brother));
	if (IS_ERR(new_znode))
		/* @fresh will be deallocated automatically by error
		   handling code in the caller. */
		return (carry_node *) new_znode;

	/* new_znode returned znode with x_count 1. Caller has to decrease
	   it. make_space() does. */

	ZF_SET(new_znode, JNODE_ORPHAN);
	fresh->node = new_znode;

	while (ZF_ISSET(reiser4_carry_real(ref), JNODE_ORPHAN)) {
		ref = carry_node_prev(ref);
		assert("nikita-1606", !carry_node_end(doing, ref));
	}

	info.todo = todo;
	info.doing = doing;
	add_pointer = node_post_carry(&info, COP_INSERT,
				      reiser4_carry_real(ref), 1);
	if (IS_ERR(add_pointer)) {
		/* no need to deallocate @new_znode here: it will be
		   deallocated during carry error handling. */
		return (carry_node *) add_pointer;
	}

	add_pointer->u.insert.type = COPT_CHILD;
	add_pointer->u.insert.child = fresh;
	add_pointer->u.insert.brother = brother;
	/* initially new node spawns empty key range */
	write_lock_dk(znode_get_tree(brother));
	znode_set_ld_key(new_znode,
			 znode_set_rd_key(new_znode,
					  znode_get_rd_key(brother)));
	write_unlock_dk(znode_get_tree(brother));
	return fresh;
}

/* DEBUGGING FUNCTIONS.

   Probably we also should leave them on even when
   debugging is turned off to print dumps at errors.
*/
#if REISER4_DEBUG
static int carry_level_invariant(carry_level * level, carry_queue_state state)
{
	carry_node *node;
	carry_node *tmp_node;

	if (level == NULL)
		return 0;

	if (level->track_type != 0 &&
	    level->track_type != CARRY_TRACK_NODE &&
	    level->track_type != CARRY_TRACK_CHANGE)
		return 0;

	/* check that nodes are in ascending order */
	for_all_nodes(level, node, tmp_node) {
		znode *left;
		znode *right;

		reiser4_key lkey;
		reiser4_key rkey;

		if (node != carry_node_front(level)) {
			if (state == CARRY_TODO) {
				right = node->node;
				left = carry_node_prev(node)->node;
			} else {
				right = reiser4_carry_real(node);
				left = reiser4_carry_real(carry_node_prev(node));
			}
			if (right == NULL || left == NULL)
				continue;
			if (node_is_empty(right) || node_is_empty(left))
				continue;
			if (!keyle(leftmost_key_in_node(left, &lkey),
				   leftmost_key_in_node(right, &rkey))) {
				warning("", "wrong key order");
				return 0;
			}
		}
	}
	return 1;
}
#endif

/* get symbolic name for boolean */
static const char *tf(int boolean/* truth value */)
{
	return boolean ? "t" : "f";
}

/* symbolic name for carry operation */
static const char *carry_op_name(carry_opcode op/* carry opcode */)
{
	switch (op) {
	case COP_INSERT:
		return "COP_INSERT";
	case COP_DELETE:
		return "COP_DELETE";
	case COP_CUT:
		return "COP_CUT";
	case COP_PASTE:
		return "COP_PASTE";
	case COP_UPDATE:
		return "COP_UPDATE";
	case COP_EXTENT:
		return "COP_EXTENT";
	case COP_INSERT_FLOW:
		return "COP_INSERT_FLOW";
	default:{
			/* not mt safe, but who cares? */
			static char buf[20];

			sprintf(buf, "unknown op: %x", op);
			return buf;
		}
	}
}

/* dump information about carry node */
static void print_carry(const char *prefix /* prefix to print */ ,
			carry_node * node/* node to print */)
{
	if (node == NULL) {
		printk("%s: null\n", prefix);
		return;
	}
	printk
	    ("%s: %p parent: %s, left: %s, unlock: %s, free: %s, dealloc: %s\n",
	     prefix, node, tf(node->parent), tf(node->left), tf(node->unlock),
	     tf(node->free), tf(node->deallocate));
}

/* dump information about carry operation */
static void print_op(const char *prefix /* prefix to print */ ,
		     carry_op * op/* operation to print */)
{
	if (op == NULL) {
		printk("%s: null\n", prefix);
		return;
	}
	printk("%s: %p carry_opcode: %s\n", prefix, op, carry_op_name(op->op));
	print_carry("\tnode", op->node);
	switch (op->op) {
	case COP_INSERT:
	case COP_PASTE:
		print_coord("\tcoord",
			    op->u.insert.d ? op->u.insert.d->coord : NULL, 0);
		reiser4_print_key("\tkey",
				  op->u.insert.d ? op->u.insert.d->key : NULL);
		print_carry("\tchild", op->u.insert.child);
		break;
	case COP_DELETE:
		print_carry("\tchild", op->u.delete.child);
		break;
	case COP_CUT:
		if (op->u.cut_or_kill.is_cut) {
			print_coord("\tfrom",
				    op->u.cut_or_kill.u.kill->params.from, 0);
			print_coord("\tto", op->u.cut_or_kill.u.kill->params.to,
				    0);
		} else {
			print_coord("\tfrom",
				    op->u.cut_or_kill.u.cut->params.from, 0);
			print_coord("\tto", op->u.cut_or_kill.u.cut->params.to,
				    0);
		}
		break;
	case COP_UPDATE:
		print_carry("\tleft", op->u.update.left);
		break;
	default:
		/* do nothing */
		break;
	}
}

/* dump information about all nodes and operations in a @level */
static void print_level(const char *prefix /* prefix to print */ ,
			carry_level * level/* level to print */)
{
	carry_node *node;
	carry_node *tmp_node;
	carry_op *op;
	carry_op *tmp_op;

	if (level == NULL) {
		printk("%s: null\n", prefix);
		return;
	}
	printk("%s: %p, restartable: %s\n",
	       prefix, level, tf(level->restartable));

	for_all_nodes(level, node, tmp_node)
	    print_carry("\tcarry node", node);
	for_all_ops(level, op, tmp_op)
	    print_op("\tcarry op", op);
}

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   scroll-step: 1
   End:
*/
