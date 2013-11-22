/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
   reiser4/README */

/* The design document for this file is at http://www.namesys.com/v4/v4.html. */

#include "forward.h"
#include "debug.h"
#include "dformat.h"
#include "key.h"
#include "coord.h"
#include "plugin/item/item.h"
#include "plugin/plugin.h"
#include "plugin/object.h"
#include "txnmgr.h"
#include "jnode.h"
#include "znode.h"
#include "block_alloc.h"
#include "tree_walk.h"
#include "carry.h"
#include "tree.h"
#include "vfs_ops.h"
#include "inode.h"
#include "page_cache.h"
#include "wander.h"
#include "super.h"
#include "entd.h"
#include "reiser4.h"
#include "flush.h"
#include "writeout.h"

#include <asm/atomic.h>
#include <linux/fs.h>		/* for struct super_block  */
#include <linux/mm.h>		/* for struct page */
#include <linux/bio.h>		/* for struct bio */
#include <linux/pagemap.h>
#include <linux/blkdev.h>

/* IMPLEMENTATION NOTES */

/* PARENT-FIRST: Some terminology: A parent-first traversal is a way of
   assigning a total order to the nodes of the tree in which the parent is
   placed before its children, which are ordered (recursively) in left-to-right
   order. When we speak of a "parent-first preceder", it describes the node that
   "came before in forward parent-first order". When we speak of a "parent-first
   follower", it describes the node that "comes next in parent-first order"
   (alternatively the node that "came before in reverse parent-first order").

   The following pseudo-code prints the nodes of a tree in forward parent-first
   order:

   void parent_first (node)
   {
     print_node (node);
     if (node->level > leaf) {
       for (i = 0; i < num_children; i += 1) {
	 parent_first (node->child[i]);
       }
     }
   }
*/

/* JUST WHAT ARE WE TRYING TO OPTIMIZE, HERE?  The idea is to optimize block
   allocation so that a left-to-right scan of the tree's data (i.e., the leaves
   in left-to-right order) can be accomplished with sequential reads, which
   results in reading nodes in their parent-first order. This is a
   read-optimization aspect of the flush algorithm, and there is also a
   write-optimization aspect, which is that we wish to make large sequential
   writes to the disk by allocating or reallocating blocks so that they can be
   written in sequence. Sometimes the read-optimization and write-optimization
   goals conflict with each other, as we discuss in more detail below.
*/

/* STATE BITS: The flush code revolves around the state of the jnodes it covers.
   Here are the relevant jnode->state bits and their relevence to flush:

     JNODE_DIRTY: If a node is dirty, it must be flushed. But in order to be
     written it must be allocated first. In order to be considered allocated,
     the jnode must have exactly one of { JNODE_OVRWR, JNODE_RELOC } set. These
     two bits are exclusive, and all dirtied jnodes eventually have one of these
     bits set during each transaction.

     JNODE_CREATED: The node was freshly created in its transaction and has no
     previous block address, so it is unconditionally assigned to be relocated,
     although this is mainly for code-convenience. It is not being 'relocated'
     from anything, but in almost every regard it is treated as part of the
     relocate set. The JNODE_CREATED bit remains set even after JNODE_RELOC is
     set, so the actual relocate can be distinguished from the
     created-and-allocated set easily: relocate-set members (belonging to the
     preserve-set) have (JNODE_RELOC) set and created-set members which have no
     previous location to preserve have (JNODE_RELOC | JNODE_CREATED) set.

     JNODE_OVRWR: The node belongs to atom's overwrite set. The flush algorithm
     made the decision to maintain the pre-existing location for this node and
     it will be written to the wandered-log.

     JNODE_RELOC: The flush algorithm made the decision to relocate this block
     (if it was not created, see note above). A block with JNODE_RELOC set is
     eligible for early-flushing and may be submitted during flush_empty_queues.
     When the JNODE_RELOC bit is set on a znode, the parent node's internal item
     is modified and the znode is rehashed.

     JNODE_SQUEEZABLE: Before shifting everything left, the flush algorithm
     scans the node and calls plugin->f.squeeze() method for its items. By this
     technology we update disk clusters of cryptcompress objects. Also if
     leftmost point that was found by flush scan has this flag (races with
     write(), rare case) the flush algorythm makes the decision to pass it to
     squalloc() in spite of its flushprepped status for squeezing, not for
     repeated allocation.

     JNODE_FLUSH_QUEUED: This bit is set when a call to flush enters the jnode
     into its flush queue. This means the jnode is not on any clean or dirty
     list, instead it is moved to one of the flush queue (see flush_queue.h)
     object private list. This prevents multiple concurrent flushes from
     attempting to start flushing from the same node.

     (DEAD STATE BIT) JNODE_FLUSH_BUSY: This bit was set during the bottom-up
     squeeze-and-allocate on a node while its children are actively being
     squeezed and allocated. This flag was created to avoid submitting a write
     request for a node while its children are still being allocated and
     squeezed. Then flush queue was re-implemented to allow unlimited number of
     nodes be queued. This flag support was commented out in source code because
     we decided that there was no reason to submit queued nodes before
     jnode_flush() finishes.  However, current code calls fq_write() during a
     slum traversal and may submit "busy nodes" to disk. Probably we can
     re-enable the JNODE_FLUSH_BUSY bit support in future.

   With these state bits, we describe a test used frequently in the code below,
   jnode_is_flushprepped()(and the spin-lock-taking jnode_check_flushprepped()).
   The test for "flushprepped" returns true if any of the following are true:

     - The node is not dirty
     - The node has JNODE_RELOC set
     - The node has JNODE_OVRWR set

   If either the node is not dirty or it has already been processed by flush
   (and assigned JNODE_OVRWR or JNODE_RELOC), then it is prepped. If
   jnode_is_flushprepped() returns true then flush has work to do on that node.
*/

/* FLUSH_PREP_ONCE_PER_TRANSACTION: Within a single transaction a node is never
   flushprepped twice (unless an explicit call to flush_unprep is made as
   described in detail below). For example a node is dirtied, allocated, and
   then early-flushed to disk and set clean. Before the transaction commits, the
   page is dirtied again and, due to memory pressure, the node is flushed again.
   The flush algorithm will not relocate the node to a new disk location, it
   will simply write it to the same, previously relocated position again.
*/

/* THE BOTTOM-UP VS. TOP-DOWN ISSUE: This code implements a bottom-up algorithm
   where we start at a leaf node and allocate in parent-first order by iterating
   to the right. At each step of the iteration, we check for the right neighbor.
   Before advancing to the right neighbor, we check if the current position and
   the right neighbor share the same parent. If they do not share the same
   parent, the parent is allocated before the right neighbor.

   This process goes recursively up the tree and squeeze nodes level by level as
   long as the right neighbor and the current position have different parents,
   then it allocates the right-neighbors-with-different-parents on the way back
   down. This process is described in more detail in
   flush_squalloc_changed_ancestor and the recursive function
   squalloc_one_changed_ancestor. But the purpose here is not to discuss the
   specifics of the bottom-up approach as it is to contrast the bottom-up and
   top-down approaches.

   The top-down algorithm was implemented earlier (April-May 2002). In the
   top-down approach, we find a starting point by scanning left along each level
   past dirty nodes, then going up and repeating the process until the left node
   and the parent node are clean. We then perform a parent-first traversal from
   the starting point, which makes allocating in parent-first order trivial.
   After one subtree has been allocated in this manner, we move to the right,
   try moving upward, then repeat the parent-first traversal.

   Both approaches have problems that need to be addressed. Both are
   approximately the same amount of code, but the bottom-up approach has
   advantages in the order it acquires locks which, at the very least, make it
   the better approach. At first glance each one makes the other one look
   simpler, so it is important to remember a few of the problems with each one.

   Main problem with the top-down approach: When you encounter a clean child
   during the parent-first traversal, what do you do? You would like to avoid
   searching through a large tree of nodes just to find a few dirty leaves at
   the bottom, and there is not an obvious solution. One of the advantages of
   the top-down approach is that during the parent-first traversal you check
   every child of a parent to see if it is dirty. In this way, the top-down
   approach easily handles the main problem of the bottom-up approach:
   unallocated children.

   The unallocated children problem is that before writing a node to disk we
   must make sure that all of its children are allocated. Otherwise, the writing
   the node means extra I/O because the node will have to be written again when
   the child is finally allocated.

   WE HAVE NOT YET ELIMINATED THE UNALLOCATED CHILDREN PROBLEM. Except for bugs,
   this should not cause any file system corruption, it only degrades I/O
   performance because a node may be written when it is sure to be written at
   least one more time in the same transaction when the remaining children are
   allocated. What follows is a description of how we will solve the problem.
*/

/* HANDLING UNALLOCATED CHILDREN: During flush we may allocate a parent node,
   then proceeding in parent first order, allocate some of its left-children,
   then encounter a clean child in the middle of the parent. We do not allocate
   the clean child, but there may remain unallocated (dirty) children to the
   right of the clean child. If we were to stop flushing at this moment and
   write everything to disk, the parent might still contain unallocated
   children.

   We could try to allocate all the descendents of every node that we allocate,
   but this is not necessary. Doing so could result in allocating the entire
   tree: if the root node is allocated then every unallocated node would have to
   be allocated before flushing. Actually, we do not have to write a node just
   because we allocate it. It is possible to allocate but not write a node
   during flush, when it still has unallocated children. However, this approach
   is probably not optimal for the following reason.

   The flush algorithm is designed to allocate nodes in parent-first order in an
   attempt to optimize reads that occur in the same order. Thus we are
   read-optimizing for a left-to-right scan through all the leaves in the
   system, and we are hoping to write-optimize at the same time because those
   nodes will be written together in batch. What happens, however, if we assign
   a block number to a node in its read-optimized order but then avoid writing
   it because it has unallocated children? In that situation, we lose out on the
   write-optimization aspect because a node will have to be written again to the
   its location on the device, later, which likely means seeking back to that
   location.

   So there are tradeoffs. We can choose either:

   A. Allocate all unallocated children to preserve both write-optimization and
   read-optimization, but this is not always desirable because it may mean
   having to allocate and flush very many nodes at once.

   B. Defer writing nodes with unallocated children, keep their read-optimized
   locations, but sacrifice write-optimization because those nodes will be
   written again.

   C. Defer writing nodes with unallocated children, but do not keep their
   read-optimized locations. Instead, choose to write-optimize them later, when
   they are written. To facilitate this, we "undo" the read-optimized allocation
   that was given to the node so that later it can be write-optimized, thus
   "unpreparing" the flush decision. This is a case where we disturb the
   FLUSH_PREP_ONCE_PER_TRANSACTION rule described above. By a call to
   flush_unprep() we will: if the node was wandered, unset the JNODE_OVRWR bit;
   if the node was relocated, unset the JNODE_RELOC bit, non-deferred-deallocate
   its block location, and set the JNODE_CREATED bit, effectively setting the
   node back to an unallocated state.

   We will take the following approach in v4.0: for twig nodes we will always
   finish allocating unallocated children (A).  For nodes with (level > TWIG)
   we will defer writing and choose write-optimization (C).

   To summarize, there are several parts to a solution that avoids the problem
   with unallocated children:

   FIXME-ZAM: Still no one approach is implemented to eliminate the
   "UNALLOCATED CHILDREN" problem because there was an experiment which was done
   showed that we have 1-2 nodes with unallocated children for thousands of
   written nodes. The experiment was simple like coping/deletion of linux kernel
   sources. However the problem can arise in more complex tests. I think we have
   jnode_io_hook to insert a check for unallocated children and see what kind of
   problem we have.

   1. When flush reaches a stopping point (e.g. a clean node) it should continue
   calling squeeze-and-allocate on any remaining unallocated children.
   FIXME: Difficulty to implement: should be simple -- amounts to adding a while
   loop to jnode_flush, see comments in that function.

   2. When flush reaches flush_empty_queue(), some of the (level > TWIG) nodes
   may still have unallocated children. If the twig level has unallocated
   children it is an assertion failure. If a higher-level node has unallocated
   children, then it should be explicitly de-allocated by a call to
   flush_unprep().
   FIXME: Difficulty to implement: should be simple.

   3. (CPU-Optimization) Checking whether a node has unallocated children may
   consume more CPU cycles than we would like, and it is possible (but medium
   complexity) to optimize this somewhat in the case where large sub-trees are
   flushed. The following observation helps: if both the left- and
   right-neighbor of a node are processed by the flush algorithm then the node
   itself is guaranteed to have all of its children allocated. However, the cost
   of this check may not be so expensive after all: it is not needed for leaves
   and flush can guarantee this property for twigs. That leaves only (level >
   TWIG) nodes that have to be checked, so this optimization only helps if at
   least three (level > TWIG) nodes are flushed in one pass, and the savings
   will be very small unless there are many more (level > TWIG) nodes. But if
   there are many (level > TWIG) nodes then the number of blocks being written
   will be very large, so the savings may be insignificant. That said, the idea
   is to maintain both the left and right edges of nodes that are processed in
   flush.  When flush_empty_queue() is called, a relatively simple test will
   tell whether the (level > TWIG) node is on the edge. If it is on the edge,
   the slow check is necessary, but if it is in the interior then it can be
   assumed to have all of its children allocated. FIXME: medium complexity to
   implement, but simple to verify given that we must have a slow check anyway.

   4. (Optional) This part is optional, not for v4.0--flush should work
   independently of whether this option is used or not. Called RAPID_SCAN, the
   idea is to amend the left-scan operation to take unallocated children into
   account. Normally, the left-scan operation goes left as long as adjacent
   nodes are dirty up until some large maximum value (FLUSH_SCAN_MAXNODES) at
   which point it stops and begins flushing. But scan-left may stop at a
   position where there are unallocated children to the left with the same
   parent. When RAPID_SCAN is enabled, the ordinary scan-left operation stops
   after FLUSH_RELOCATE_THRESHOLD, which is much smaller than
   FLUSH_SCAN_MAXNODES, then procedes with a rapid scan. The rapid scan skips
   all the interior children of a node--if the leftmost child of a twig is
   dirty, check its left neighbor (the rightmost child of the twig to the left).
   If the left neighbor of the leftmost child is also dirty, then continue the
   scan at the left twig and repeat.  This option will cause flush to allocate
   more twigs in a single pass, but it also has the potential to write many more
   nodes than would otherwise be written without the RAPID_SCAN option.
   RAPID_SCAN was partially implemented, code removed August 12, 2002 by JMACD.
*/

/* FLUSH CALLED ON NON-LEAF LEVEL. Most of our design considerations assume that
   the starting point for flush is a leaf node, but actually the flush code
   cares very little about whether or not this is true.  It is possible that all
   the leaf nodes are flushed and dirty parent nodes still remain, in which case
   jnode_flush() is called on a non-leaf argument. Flush doesn't care--it treats
   the argument node as if it were a leaf, even when it is not. This is a simple
   approach, and there may be a more optimal policy but until a problem with
   this approach is discovered, simplest is probably best.

   NOTE: In this case, the ordering produced by flush is parent-first only if
   you ignore the leaves. This is done as a matter of simplicity and there is
   only one (shaky) justification. When an atom commits, it flushes all leaf
   level nodes first, followed by twigs, and so on. With flushing done in this
   order, if flush is eventually called on a non-leaf node it means that
   (somehow) we reached a point where all leaves are clean and only internal
   nodes need to be flushed. If that it the case, then it means there were no
   leaves that were the parent-first preceder/follower of the parent. This is
   expected to be a rare case, which is why we do nothing special about it.
   However, memory pressure may pass an internal node to flush when there are
   still dirty leaf nodes that need to be flushed, which could prove our
   original assumptions "inoperative". If this needs to be fixed, then
   scan_left/right should have special checks for the non-leaf levels. For
   example, instead of passing from a node to the left neighbor, it should pass
   from the node to the left neighbor's rightmost descendent (if dirty).

*/

/* UNIMPLEMENTED AS YET: REPACKING AND RESIZING. We walk the tree in 4MB-16MB
   chunks, dirtying everything and putting it into a transaction. We tell the
   allocator to allocate the blocks as far as possible towards one end of the
   logical device--the left (starting) end of the device if we are walking from
   left to right, the right end of the device if we are walking from right to
   left.  We then make passes in alternating directions, and as we do this the
   device becomes sorted such that tree order and block number order fully
   correlate.

   Resizing is done by shifting everything either all the way to the left or all
   the way to the right, and then reporting the last block.
*/

/* RELOCATE DECISIONS: The code makes a decision to relocate in several places.
   This descibes the policy from the highest level:

   The FLUSH_RELOCATE_THRESHOLD parameter: If we count this many consecutive
   nodes on the leaf level during flush-scan (right, left), then we
   unconditionally decide to relocate leaf nodes.

   Otherwise, there are two contexts in which we make a decision to relocate:

   1. The REVERSE PARENT-FIRST context: Implemented in reverse_relocate_test().
   During the initial stages of flush, after scan-right completes, we want to
   ask the question: should we relocate this leaf node and thus dirty the parent
   node. Then if the node is a leftmost child its parent is its own parent-first
   preceder, thus we repeat the question at the next level up, and so on. In
   these cases we are moving in the reverse-parent first direction.

   There is another case which is considered the reverse direction, which comes
   at the end of a twig in reverse_relocate_end_of_twig(). As we finish
   processing a twig we may reach a point where there is a clean twig to the
   right with a dirty leftmost child. In this case, we may wish to relocate the
   child by testing if it should be relocated relative to its parent.

   2. The FORWARD PARENT-FIRST context: Testing for forward relocation is done
   in allocate_znode. What distinguishes the forward parent-first case from the
   reverse-parent first case is that the preceder has already been allocated in
   the forward case, whereas in the reverse case we don't know what the preceder
   is until we finish "going in reverse". That simplifies the forward case
   considerably, and there we actually use the block allocator to determine
   whether, e.g., a block closer to the preceder is available.
*/

/* SQUEEZE_LEFT_EDGE: Unimplemented idea for future consideration. The idea is,
   once we finish scan-left and find a starting point, if the parent's left
   neighbor is dirty then squeeze the parent's left neighbor and the parent.
   This may change the flush-starting-node's parent. Repeat until the child's
   parent is stable. If the child is a leftmost child, repeat this left-edge
   squeezing operation at the next level up. Note that we cannot allocate
   extents during this or they will be out of parent-first order. There is also
   some difficult coordinate maintenence issues.  We can't do a tree search to
   find coordinates again (because we hold locks), we have to determine them
   from the two nodes being squeezed. Looks difficult, but has potential to
   increase space utilization. */

/* Flush-scan helper functions. */
static void scan_init(flush_scan * scan);
static void scan_done(flush_scan * scan);

/* Flush-scan algorithm. */
static int scan_left(flush_scan * scan, flush_scan * right, jnode * node,
		     unsigned limit);
static int scan_right(flush_scan * scan, jnode * node, unsigned limit);
static int scan_common(flush_scan * scan, flush_scan * other);
static int scan_formatted(flush_scan * scan);
static int scan_unformatted(flush_scan * scan, flush_scan * other);
static int scan_by_coord(flush_scan * scan);

/* Initial flush-point ancestor allocation. */
static int alloc_pos_and_ancestors(flush_pos_t *pos);
static int alloc_one_ancestor(const coord_t *coord, flush_pos_t *pos);
static int set_preceder(const coord_t *coord_in, flush_pos_t *pos);

/* Main flush algorithm.
   Note on abbreviation: "squeeze and allocate" == "squalloc". */
static int squalloc(flush_pos_t *pos);

/* Flush squeeze implementation. */
static int squeeze_right_non_twig(znode * left, znode * right);
static int shift_one_internal_unit(znode * left, znode * right);

/* Flush reverse parent-first relocation routines. */
static int reverse_relocate_if_close_enough(const reiser4_block_nr * pblk,
					    const reiser4_block_nr * nblk);
static int reverse_relocate_test(jnode * node, const coord_t *parent_coord,
				 flush_pos_t *pos);
static int reverse_relocate_check_dirty_parent(jnode * node,
					       const coord_t *parent_coord,
					       flush_pos_t *pos);

/* Flush allocate write-queueing functions: */
static int allocate_znode(znode * node, const coord_t *parent_coord,
			  flush_pos_t *pos);
static int allocate_znode_update(znode * node, const coord_t *parent_coord,
				 flush_pos_t *pos);
static int lock_parent_and_allocate_znode(znode *, flush_pos_t *);

/* Flush helper functions: */
static int jnode_lock_parent_coord(jnode * node,
				   coord_t *coord,
				   lock_handle * parent_lh,
				   load_count * parent_zh,
				   znode_lock_mode mode, int try);
static int neighbor_in_slum(znode * node, lock_handle * right_lock, sideof side,
			   znode_lock_mode mode, int check_dirty, int expected);
static int znode_same_parents(znode * a, znode * b);

static int znode_check_flushprepped(znode * node)
{
	return jnode_check_flushprepped(ZJNODE(node));
}
static void update_znode_dkeys(znode * left, znode * right);

/* Flush position functions */
static void pos_init(flush_pos_t *pos);
static int pos_valid(flush_pos_t *pos);
static void pos_done(flush_pos_t *pos);
static int pos_stop(flush_pos_t *pos);

/* check that @org is first jnode extent unit, if extent is unallocated,
 * because all jnodes of unallocated extent are dirty and of the same atom. */
#define checkchild(scan)						\
assert("nikita-3435",							\
       ergo(scan->direction == LEFT_SIDE &&				\
	    (scan->parent_coord.node->level == TWIG_LEVEL) &&           \
	    jnode_is_unformatted(scan->node) &&				\
	    extent_is_unallocated(&scan->parent_coord),			\
	    extent_unit_index(&scan->parent_coord) == index_jnode(scan->node)))

/* This flush_cnt variable is used to track the number of concurrent flush
   operations, useful for debugging. It is initialized in txnmgr.c out of
   laziness (because flush has no static initializer function...) */
ON_DEBUG(atomic_t flush_cnt;
    )

/* check fs backing device for write congestion */
static int check_write_congestion(void)
{
	struct super_block *sb;
	struct backing_dev_info *bdi;

	sb = reiser4_get_current_sb();
	bdi = reiser4_get_super_fake(sb)->i_mapping->backing_dev_info;
	return bdi_write_congested(bdi);
}

/* conditionally write flush queue */
static int write_prepped_nodes(flush_pos_t *pos)
{
	int ret;

	assert("zam-831", pos);
	assert("zam-832", pos->fq);

	if (!(pos->flags & JNODE_FLUSH_WRITE_BLOCKS))
		return 0;

	if (check_write_congestion())
		return 0;

	ret = reiser4_write_fq(pos->fq, pos->nr_written,
		       WRITEOUT_SINGLE_STREAM | WRITEOUT_FOR_PAGE_RECLAIM);
	return ret;
}

/* Proper release all flush pos. resources then move flush position to new
   locked node */
static void move_flush_pos(flush_pos_t *pos, lock_handle * new_lock,
			   load_count * new_load, const coord_t *new_coord)
{
	assert("zam-857", new_lock->node == new_load->node);

	if (new_coord) {
		assert("zam-858", new_coord->node == new_lock->node);
		coord_dup(&pos->coord, new_coord);
	} else {
		coord_init_first_unit(&pos->coord, new_lock->node);
	}

	if (pos->child) {
		jput(pos->child);
		pos->child = NULL;
	}

	move_load_count(&pos->load, new_load);
	done_lh(&pos->lock);
	move_lh(&pos->lock, new_lock);
}

/* delete empty node which link from the parent still exists. */
static int delete_empty_node(znode * node)
{
	reiser4_key smallest_removed;

	assert("zam-1019", node != NULL);
	assert("zam-1020", node_is_empty(node));
	assert("zam-1023", znode_is_wlocked(node));

	return reiser4_delete_node(node, &smallest_removed, NULL, 1);
}

/* Prepare flush position for alloc_pos_and_ancestors() and squalloc() */
static int prepare_flush_pos(flush_pos_t *pos, jnode * org)
{
	int ret;
	load_count load;
	lock_handle lock;

	init_lh(&lock);
	init_load_count(&load);

	if (jnode_is_znode(org)) {
		ret = longterm_lock_znode(&lock, JZNODE(org),
					  ZNODE_WRITE_LOCK, ZNODE_LOCK_HIPRI);
		if (ret)
			return ret;

		ret = incr_load_count_znode(&load, JZNODE(org));
		if (ret)
			return ret;

		pos->state =
		    (jnode_get_level(org) ==
		     LEAF_LEVEL) ? POS_ON_LEAF : POS_ON_INTERNAL;
		move_flush_pos(pos, &lock, &load, NULL);
	} else {
		coord_t parent_coord;
		ret = jnode_lock_parent_coord(org, &parent_coord, &lock,
					      &load, ZNODE_WRITE_LOCK, 0);
		if (ret)
			goto done;
		if (!item_is_extent(&parent_coord)) {
			/* file was converted to tail, org became HB, we found
			   internal item */
			ret = -EAGAIN;
			goto done;
		}

		pos->state = POS_ON_EPOINT;
		move_flush_pos(pos, &lock, &load, &parent_coord);
		pos->child = jref(org);
		if (extent_is_unallocated(&parent_coord)
		    && extent_unit_index(&parent_coord) != index_jnode(org)) {
			/* @org is not first child of its parent unit. This may
			   happen because longerm lock of its parent node was
			   released between scan_left and scan_right. For now
			   work around this having flush to repeat */
			ret = -EAGAIN;
		}
	}

done:
	done_load_count(&load);
	done_lh(&lock);
	return ret;
}

/* TODO LIST (no particular order): */
/* I have labelled most of the legitimate FIXME comments in this file with
   letters to indicate which issue they relate to. There are a few miscellaneous
   FIXMEs with specific names mentioned instead that need to be
   inspected/resolved. */
/* B. There is an issue described in reverse_relocate_test having to do with an
   imprecise is_preceder? check having to do with partially-dirty extents. The
   code that sets preceder hints and computes the preceder is basically
   untested. Careful testing needs to be done that preceder calculations are
   done correctly, since if it doesn't affect correctness we will not catch this
   stuff during regular testing. */
/* C. EINVAL, E_DEADLOCK, E_NO_NEIGHBOR, ENOENT handling. It is unclear which of
   these are considered expected but unlikely conditions. Flush currently
   returns 0 (i.e., success but no progress, i.e., restart) whenever it receives
   any of these in jnode_flush(). Many of the calls that may produce one of
   these return values (i.e., longterm_lock_znode, reiser4_get_parent,
   reiser4_get_neighbor, ...) check some of these values themselves and, for
   instance, stop flushing instead of resulting in a restart. If any of these
   results are true error conditions then flush will go into a busy-loop, as we
   noticed during testing when a corrupt tree caused find_child_ptr to return
   ENOENT. It needs careful thought and testing of corner conditions.
*/
/* D. Atomicity of flush_prep against deletion and flush concurrency. Suppose a
   created block is assigned a block number then early-flushed to disk. It is
   dirtied again and flush is called again. Concurrently, that block is deleted,
   and the de-allocation of its block number does not need to be deferred, since
   it is not part of the preserve set (i.e., it didn't exist before the
   transaction). I think there may be a race condition where flush writes the
   dirty, created block after the non-deferred deallocated block number is
   re-allocated, making it possible to write deleted data on top of non-deleted
   data. Its just a theory, but it needs to be thought out. */
/* F. bio_alloc() failure is not handled gracefully. */
/* G. Unallocated children. */
/* H. Add a WANDERED_LIST to the atom to clarify the placement of wandered
   blocks. */
/* I. Rename flush-scan to scan-point, (flush-pos to flush-point?) */

/* JNODE_FLUSH: MAIN ENTRY POINT */
/* This is the main entry point for flushing a jnode and its dirty neighborhood
   (dirty neighborhood is named "slum"). Jnode_flush() is called if reiser4 has
   to write dirty blocks to disk, it happens when Linux VM decides to reduce
   number of dirty pages or as a part of transaction commit.

   Our objective here is to prep and flush the slum the jnode belongs to. We
   want to squish the slum together, and allocate the nodes in it as we squish
   because allocation of children affects squishing of parents.

   The "argument" @node tells flush where to start. From there, flush finds the
   left edge of the slum, and calls squalloc (in which nodes are squeezed and
   allocated). To find a "better place" to start squalloc first we perform a
   flush_scan.

   Flush-scanning may be performed in both left and right directions, but for
   different purposes. When scanning to the left, we are searching for a node
   that precedes a sequence of parent-first-ordered nodes which we will then
   flush in parent-first order. During flush-scanning, we also take the
   opportunity to count the number of consecutive leaf nodes. If this number is
   past some threshold (FLUSH_RELOCATE_THRESHOLD), then we make a decision to
   reallocate leaf nodes (thus favoring write-optimization).

   Since the flush argument node can be anywhere in a sequence of dirty leaves,
   there may also be dirty nodes to the right of the argument. If the scan-left
   operation does not count at least FLUSH_RELOCATE_THRESHOLD nodes then we
   follow it with a right-scan operation to see whether there is, in fact,
   enough nodes to meet the relocate threshold. Each right- and left-scan
   operation uses a single flush_scan object.

   After left-scan and possibly right-scan, we prepare a flush_position object
   with the starting flush point or parent coordinate, which was determined
   using scan-left.

   Next we call the main flush routine, squalloc, which iterates along the leaf
   level, squeezing and allocating nodes (and placing them into the flush
   queue).

   After squalloc returns we take extra steps to ensure that all the children
   of the final twig node are allocated--this involves repeating squalloc
   until we finish at a twig with no unallocated children.

   Finally, we call flush_empty_queue to submit write-requests to disk. If we
   encounter any above-twig nodes during flush_empty_queue that still have
   unallocated children, we flush_unprep them.

   Flush treats several "failure" cases as non-failures, essentially causing
   them to start over. E_DEADLOCK is one example.
   FIXME:(C) EINVAL, E_NO_NEIGHBOR, ENOENT: these should probably be handled
   properly rather than restarting, but there are a bunch of cases to audit.
*/

static int
jnode_flush(jnode * node, long nr_to_write, long *nr_written,
	    flush_queue_t *fq, int flags)
{
	long ret = 0;
	flush_scan *right_scan;
	flush_scan *left_scan;
	flush_pos_t *flush_pos;
	int todo;
	struct super_block *sb;
	reiser4_super_info_data *sbinfo;
	jnode *leftmost_in_slum = NULL;

	assert("jmacd-76619", lock_stack_isclean(get_current_lock_stack()));
	assert("nikita-3022", reiser4_schedulable());

	assert("nikita-3185",
	       get_current_super_private()->delete_mutex_owner != current);

	/* allocate right_scan, left_scan and flush_pos */
	right_scan =
	    kmalloc(2 * sizeof(*right_scan) + sizeof(*flush_pos),
		    reiser4_ctx_gfp_mask_get());
	if (right_scan == NULL)
		return RETERR(-ENOMEM);
	left_scan = right_scan + 1;
	flush_pos = (flush_pos_t *) (left_scan + 1);

	sb = reiser4_get_current_sb();
	sbinfo = get_super_private(sb);

	/* Flush-concurrency debug code */
#if REISER4_DEBUG
	atomic_inc(&flush_cnt);
#endif

	reiser4_enter_flush(sb);

	/* Initialize a flush position. */
	pos_init(flush_pos);

	flush_pos->nr_written = nr_written;
	flush_pos->fq = fq;
	flush_pos->flags = flags;
	flush_pos->nr_to_write = nr_to_write;

	scan_init(right_scan);
	scan_init(left_scan);

	/* First scan left and remember the leftmost scan position. If the
	   leftmost position is unformatted we remember its parent_coord. We
	   scan until counting FLUSH_SCAN_MAXNODES.

	   If starting @node is unformatted, at the beginning of left scan its
	   parent (twig level node, containing extent item) will be long term
	   locked and lock handle will be stored in the
	   @right_scan->parent_lock. This lock is used to start the rightward
	   scan without redoing the tree traversal (necessary to find parent)
	   and, hence, is kept during leftward scan. As a result, we have to
	   use try-lock when taking long term locks during the leftward scan.
	 */
	ret = scan_left(left_scan, right_scan,
			node, sbinfo->flush.scan_maxnodes);
	if (ret != 0)
		goto failed;

	leftmost_in_slum = jref(left_scan->node);
	scan_done(left_scan);

	/* Then possibly go right to decide if we will use a policy of
	   relocating leaves. This is only done if we did not scan past (and
	   count) enough nodes during the leftward scan. If we do scan right,
	   we only care to go far enough to establish that at least
	   FLUSH_RELOCATE_THRESHOLD number of nodes are being flushed. The scan
	   limit is the difference between left_scan.count and the threshold. */

	todo = sbinfo->flush.relocate_threshold - left_scan->count;
	/* scan right is inherently deadlock prone, because we are
	 * (potentially) holding a lock on the twig node at this moment.
	 * FIXME: this is incorrect comment: lock is not held */
	if (todo > 0) {
		ret = scan_right(right_scan, node, (unsigned)todo);
		if (ret != 0)
			goto failed;
	}

	/* Only the right-scan count is needed, release any rightward locks
	   right away. */
	scan_done(right_scan);

	/* ... and the answer is: we should relocate leaf nodes if at least
	   FLUSH_RELOCATE_THRESHOLD nodes were found. */
	flush_pos->leaf_relocate = JF_ISSET(node, JNODE_REPACK) ||
	    (left_scan->count + right_scan->count >=
	     sbinfo->flush.relocate_threshold);

	/* Funny business here.  We set the 'point' in the flush_position at
	   prior to starting squalloc regardless of whether the first point is
	   formatted or unformatted. Without this there would be an invariant,
	   in the rest of the code, that if the flush_position is unformatted
	   then flush_position->point is NULL and
	   flush_position->parent_{lock,coord} is set, and if the flush_position
	   is formatted then flush_position->point is non-NULL and no parent
	   info is set.

	   This seems lazy, but it makes the initial calls to
	   reverse_relocate_test (which ask "is it the pos->point the leftmost
	   child of its parent") much easier because we know the first child
	   already.  Nothing is broken by this, but the reasoning is subtle.
	   Holding an extra reference on a jnode during flush can cause us to
	   see nodes with HEARD_BANSHEE during squalloc, because nodes are not
	   removed from sibling lists until they have zero reference count.
	   Flush would never observe a HEARD_BANSHEE node on the left-edge of
	   flush, nodes are only deleted to the right. So if nothing is broken,
	   why fix it?

	   NOTE-NIKITA actually, flush can meet HEARD_BANSHEE node at any
	   point and in any moment, because of the concurrent file system
	   activity (for example, truncate). */

	/* Check jnode state after flush_scan completed. Having a lock on this
	   node or its parent (in case of unformatted) helps us in case of
	   concurrent flushing. */
	if (jnode_check_flushprepped(leftmost_in_slum)
	    && !jnode_convertible(leftmost_in_slum)) {
		ret = 0;
		goto failed;
	}

	/* Now setup flush_pos using scan_left's endpoint. */
	ret = prepare_flush_pos(flush_pos, leftmost_in_slum);
	if (ret)
		goto failed;

	if (znode_get_level(flush_pos->coord.node) == LEAF_LEVEL
	    && node_is_empty(flush_pos->coord.node)) {
		znode *empty = flush_pos->coord.node;

		assert("zam-1022", !ZF_ISSET(empty, JNODE_HEARD_BANSHEE));
		ret = delete_empty_node(empty);
		goto failed;
	}

	if (jnode_check_flushprepped(leftmost_in_slum)
	    && !jnode_convertible(leftmost_in_slum)) {
		ret = 0;
		goto failed;
	}

	/* Set pos->preceder and (re)allocate pos and its ancestors if it is
	   needed  */
	ret = alloc_pos_and_ancestors(flush_pos);
	if (ret)
		goto failed;

	/* Do the main rightward-bottom-up squeeze and allocate loop. */
	ret = squalloc(flush_pos);
	pos_stop(flush_pos);
	if (ret)
		goto failed;

	/* FIXME_NFQUCMPD: Here, handle the twig-special case for unallocated
	   children. First, the pos_stop() and pos_valid() routines should be
	   modified so that pos_stop() sets a flush_position->stop flag to 1
	   without releasing the current position immediately--instead release
	   it in pos_done(). This is a better implementation than the current
	   one anyway.

	   It is not clear that all fields of the flush_position should not be
	   released, but at the very least the parent_lock, parent_coord, and
	   parent_load should remain held because they are hold the last twig
	   when pos_stop() is called.

	   When we reach this point in the code, if the parent_coord is set to
	   after the last item then we know that flush reached the end of a twig
	   (and according to the new flush queueing design, we will return now).
	   If parent_coord is not past the last item, we should check if the
	   current twig has any unallocated children to the right (we are not
	   concerned with unallocated children to the left--in that case the
	   twig itself should not have been allocated). If the twig has
	   unallocated children to the right, set the parent_coord to that
	   position and then repeat the call to squalloc.

	   Testing for unallocated children may be defined in two ways: if any
	   internal item has a fake block number, it is unallocated; if any
	   extent item is unallocated then all of its children are unallocated.
	   But there is a more aggressive approach: if there are any dirty
	   children of the twig to the right of the current position, we may
	   wish to relocate those nodes now. Checking for potential relocation
	   is more expensive as it requires knowing whether there are any dirty
	   children that are not unallocated. The extent_needs_allocation should
	   be used after setting the correct preceder.

	   When we reach the end of a twig at this point in the code, if the
	   flush can continue (when the queue is ready) it will need some
	   information on the future starting point. That should be stored away
	   in the flush_handle using a seal, I believe. Holding a jref() on the
	   future starting point may break other code that deletes that node.
	 */

	/* FIXME_NFQUCMPD: Also, we don't want to do any flushing when flush is
	   called above the twig level.  If the VM calls flush above the twig
	   level, do nothing and return (but figure out why this happens). The
	   txnmgr should be modified to only flush its leaf-level dirty list.
	   This will do all the necessary squeeze and allocate steps but leave
	   unallocated branches and possibly unallocated twigs (when the twig's
	   leftmost child is not dirty). After flushing the leaf level, the
	   remaining unallocated nodes should be given write-optimized
	   locations. (Possibly, the remaining unallocated twigs should be
	   allocated just before their leftmost child.)
	 */

	/* Any failure reaches this point. */
failed:

	switch (ret) {
	case -E_REPEAT:
	case -EINVAL:
	case -E_DEADLOCK:
	case -E_NO_NEIGHBOR:
	case -ENOENT:
		/* FIXME(C): Except for E_DEADLOCK, these should probably be
		   handled properly in each case. They already are handled in
		   many cases. */
		/* Something bad happened, but difficult to avoid... Try again!
		*/
		ret = 0;
	}

	if (leftmost_in_slum)
		jput(leftmost_in_slum);

	pos_done(flush_pos);
	scan_done(left_scan);
	scan_done(right_scan);
	kfree(right_scan);

	ON_DEBUG(atomic_dec(&flush_cnt));

	reiser4_leave_flush(sb);

	return ret;
}

/* The reiser4 flush subsystem can be turned into "rapid flush mode" means that
 * flusher should submit all prepped nodes immediately without keeping them in
 * flush queues for long time.  The reason for rapid flush mode is to free
 * memory as fast as possible. */

#if REISER4_USE_RAPID_FLUSH

/**
 * submit all prepped nodes if rapid flush mode is set,
 * turn rapid flush mode off.
 */

static int rapid_flush(flush_pos_t *pos)
{
	if (!wbq_available())
		return 0;

	return write_prepped_nodes(pos);
}

#else

#define rapid_flush(pos) (0)

#endif				/* REISER4_USE_RAPID_FLUSH */

static jnode *find_flush_start_jnode(jnode *start, txn_atom * atom,
				     flush_queue_t *fq, int *nr_queued,
				     int flags)
{
	jnode * node;

	if (start != NULL) {
		spin_lock_jnode(start);
		if (!jnode_is_flushprepped(start)) {
			assert("zam-1056", start->atom == atom);
			node = start;
			goto enter;
		}
		spin_unlock_jnode(start);
	}
	/*
	 * In this loop we process all already prepped (RELOC or OVRWR) and
	 * dirtied again nodes. The atom spin lock is not released until all
	 * dirty nodes processed or not prepped node found in the atom dirty
	 * lists.
	 */
	while ((node = find_first_dirty_jnode(atom, flags))) {
		spin_lock_jnode(node);
enter:
		assert("zam-881", JF_ISSET(node, JNODE_DIRTY));
		assert("zam-898", !JF_ISSET(node, JNODE_OVRWR));

		if (JF_ISSET(node, JNODE_WRITEBACK)) {
			/* move node to the end of atom's writeback list */
			list_move_tail(&node->capture_link, ATOM_WB_LIST(atom));

			/*
			 * jnode is not necessarily on dirty list: if it was
			 * dirtied when it was on flush queue - it does not get
			 * moved to dirty list
			 */
			ON_DEBUG(count_jnode(atom, node, NODE_LIST(node),
					     WB_LIST, 1));

		} else if (jnode_is_znode(node)
			   && znode_above_root(JZNODE(node))) {
			/*
			 * A special case for znode-above-root. The above-root
			 * (fake) znode is captured and dirtied when the tree
			 * height changes or when the root node is relocated.
			 * This causes atoms to fuse so that changes at the root
			 * are serialized.  However, this node is never flushed.
			 * This special case used to be in lock.c to prevent the
			 * above-root node from ever being captured, but now
			 * that it is captured we simply prevent it from
			 * flushing. The log-writer code relies on this to
			 * properly log superblock modifications of the tree
			 * height.
			 */
			jnode_make_wander_nolock(node);
		} else if (JF_ISSET(node, JNODE_RELOC)) {
			queue_jnode(fq, node);
			++(*nr_queued);
		} else
			break;

		spin_unlock_jnode(node);
	}
	return node;
}

/* Flush some nodes of current atom, usually slum, return -E_REPEAT if there are
 * more nodes to flush, return 0 if atom's dirty lists empty and keep current
 * atom locked, return other errors as they are. */
int
flush_current_atom(int flags, long nr_to_write, long *nr_submitted,
		   txn_atom ** atom, jnode *start)
{
	reiser4_super_info_data *sinfo = get_current_super_private();
	flush_queue_t *fq = NULL;
	jnode *node;
	int nr_queued;
	int ret;

	assert("zam-889", atom != NULL && *atom != NULL);
	assert_spin_locked(&((*atom)->alock));
	assert("zam-892", get_current_context()->trans->atom == *atom);

	nr_to_write = LONG_MAX;
	while (1) {
		ret = reiser4_fq_by_atom(*atom, &fq);
		if (ret != -E_REPEAT)
			break;
		*atom = get_current_atom_locked();
	}
	if (ret)
		return ret;

	assert_spin_locked(&((*atom)->alock));

	/* parallel flushers limit */
	if (sinfo->tmgr.atom_max_flushers != 0) {
		while ((*atom)->nr_flushers >= sinfo->tmgr.atom_max_flushers) {
			/* An reiser4_atom_send_event() call is inside
			   reiser4_fq_put_nolock() which is called when flush is
			   finished and nr_flushers is decremented. */
			reiser4_atom_wait_event(*atom);
			*atom = get_current_atom_locked();
		}
	}

	/* count ourself as a flusher */
	(*atom)->nr_flushers++;

	writeout_mode_enable();

	nr_queued = 0;
	node = find_flush_start_jnode(start, *atom, fq, &nr_queued, flags);

	if (node == NULL) {
		if (nr_queued == 0) {
			(*atom)->nr_flushers--;
			reiser4_fq_put_nolock(fq);
			reiser4_atom_send_event(*atom);
			/* current atom remains locked */
			writeout_mode_disable();
			return 0;
		}
		spin_unlock_atom(*atom);
	} else {
		jref(node);
		BUG_ON((*atom)->super != node->tree->super);
		spin_unlock_atom(*atom);
		spin_unlock_jnode(node);
		BUG_ON(nr_to_write == 0);
		ret = jnode_flush(node, nr_to_write, nr_submitted, fq, flags);
		jput(node);
	}

	ret =
	    reiser4_write_fq(fq, nr_submitted,
		     WRITEOUT_SINGLE_STREAM | WRITEOUT_FOR_PAGE_RECLAIM);

	*atom = get_current_atom_locked();
	(*atom)->nr_flushers--;
	reiser4_fq_put_nolock(fq);
	reiser4_atom_send_event(*atom);
	spin_unlock_atom(*atom);

	writeout_mode_disable();

	if (ret == 0)
		ret = -E_REPEAT;

	return ret;
}

/* REVERSE PARENT-FIRST RELOCATION POLICIES */

/* This implements the is-it-close-enough-to-its-preceder? test for relocation
   in the reverse parent-first relocate context. Here all we know is the
   preceder and the block number. Since we are going in reverse, the preceder
   may still be relocated as well, so we can't ask the block allocator "is there
   a closer block available to relocate?" here. In the _forward_ parent-first
   relocate context (not here) we actually call the block allocator to try and
   find a closer location. */
static int
reverse_relocate_if_close_enough(const reiser4_block_nr * pblk,
				 const reiser4_block_nr * nblk)
{
	reiser4_block_nr dist;

	assert("jmacd-7710", *pblk != 0 && *nblk != 0);
	assert("jmacd-7711", !reiser4_blocknr_is_fake(pblk));
	assert("jmacd-7712", !reiser4_blocknr_is_fake(nblk));

	/* Distance is the absolute value. */
	dist = (*pblk > *nblk) ? (*pblk - *nblk) : (*nblk - *pblk);

	/* If the block is less than FLUSH_RELOCATE_DISTANCE blocks away from
	   its preceder block, do not relocate. */
	if (dist <= get_current_super_private()->flush.relocate_distance)
		return 0;

	return 1;
}

/* This function is a predicate that tests for relocation. Always called in the
   reverse-parent-first context, when we are asking whether the current node
   should be relocated in order to expand the flush by dirtying the parent level
   (and thus proceeding to flush that level). When traversing in the forward
   parent-first direction (not here), relocation decisions are handled in two
   places: allocate_znode() and extent_needs_allocation(). */
static int
reverse_relocate_test(jnode * node, const coord_t *parent_coord,
		      flush_pos_t *pos)
{
	reiser4_block_nr pblk = 0;
	reiser4_block_nr nblk = 0;

	assert("jmacd-8989", !jnode_is_root(node));

	/*
	 * This function is called only from the
	 * reverse_relocate_check_dirty_parent() and only if the parent
	 * node is clean. This implies that the parent has the real (i.e., not
	 * fake) block number, and, so does the child, because otherwise the
	 * parent would be dirty.
	 */

	/* New nodes are treated as if they are being relocated. */
	if (JF_ISSET(node, JNODE_CREATED) ||
	    (pos->leaf_relocate && jnode_get_level(node) == LEAF_LEVEL))
		return 1;

	/* Find the preceder. FIXME(B): When the child is an unformatted,
	   previously existing node, the coord may be leftmost even though the
	   child is not the parent-first preceder of the parent. If the first
	   dirty node appears somewhere in the middle of the first extent unit,
	   this preceder calculation is wrong.
	   Needs more logic in here. */
	if (coord_is_leftmost_unit(parent_coord)) {
		pblk = *znode_get_block(parent_coord->node);
	} else {
		pblk = pos->preceder.blk;
	}
	check_preceder(pblk);

	/* If (pblk == 0) then the preceder isn't allocated or isn't known:
	   relocate. */
	if (pblk == 0)
		return 1;

	nblk = *jnode_get_block(node);

	if (reiser4_blocknr_is_fake(&nblk))
		/* child is unallocated, mark parent dirty */
		return 1;

	return reverse_relocate_if_close_enough(&pblk, &nblk);
}

/* This function calls reverse_relocate_test to make a reverse-parent-first
   relocation decision and then, if yes, it marks the parent dirty. */
static int
reverse_relocate_check_dirty_parent(jnode * node, const coord_t *parent_coord,
				    flush_pos_t *pos)
{
	int ret;

	if (!JF_ISSET(ZJNODE(parent_coord->node), JNODE_DIRTY)) {

		ret = reverse_relocate_test(node, parent_coord, pos);
		if (ret < 0)
			return ret;

		/* FIXME-ZAM
		  if parent is already relocated - we do not want to grab space,
		  right? */
		if (ret == 1) {
			int grabbed;

			grabbed = get_current_context()->grabbed_blocks;
			if (reiser4_grab_space_force((__u64) 1, BA_RESERVED) !=
			    0)
				reiser4_panic("umka-1250",
					      "No space left during flush.");

			assert("jmacd-18923",
			       znode_is_write_locked(parent_coord->node));
			znode_make_dirty(parent_coord->node);
			grabbed2free_mark(grabbed);
		}
	}

	return 0;
}

/* INITIAL ALLOCATE ANCESTORS STEP (REVERSE PARENT-FIRST ALLOCATION BEFORE
   FORWARD PARENT-FIRST LOOP BEGINS) */

/* Get the leftmost child for given coord. */
static int get_leftmost_child_of_unit(const coord_t *coord, jnode ** child)
{
	int ret;

	ret = item_utmost_child(coord, LEFT_SIDE, child);

	if (ret)
		return ret;

	if (IS_ERR(*child))
		return PTR_ERR(*child);

	return 0;
}

/* This step occurs after the left- and right-scans are completed, before
   starting the forward parent-first traversal. Here we attempt to allocate
   ancestors of the starting flush point, which means continuing in the reverse
   parent-first direction to the parent, grandparent, and so on (as long as the
   child is a leftmost child). This routine calls a recursive process,
   alloc_one_ancestor, which does the real work, except there is special-case
   handling here for the first ancestor, which may be a twig. At each level
   (here and alloc_one_ancestor), we check for relocation and then, if the child
   is a leftmost child, repeat at the next level. On the way back down (the
   recursion), we allocate the ancestors in parent-first order. */
static int alloc_pos_and_ancestors(flush_pos_t *pos)
{
	int ret = 0;
	lock_handle plock;
	load_count pload;
	coord_t pcoord;

	if (znode_check_flushprepped(pos->lock.node))
		return 0;

	coord_init_invalid(&pcoord, NULL);
	init_lh(&plock);
	init_load_count(&pload);

	if (pos->state == POS_ON_EPOINT) {
		/* a special case for pos on twig level, where we already have
		   a lock on parent node. */
		/* The parent may not be dirty, in which case we should decide
		   whether to relocate the child now. If decision is made to
		   relocate the child, the parent is marked dirty. */
		ret =
		    reverse_relocate_check_dirty_parent(pos->child, &pos->coord,
							pos);
		if (ret)
			goto exit;

		/* FIXME_NFQUCMPD: We only need to allocate the twig (if child
		   is leftmost) and the leaf/child, so recursion is not needed.
		   Levels above the twig will be allocated for
		   write-optimization before the transaction commits.  */

		/* Do the recursive step, allocating zero or more of our
		 * ancestors. */
		ret = alloc_one_ancestor(&pos->coord, pos);

	} else {
		if (!znode_is_root(pos->lock.node)) {
			/* all formatted nodes except tree root */
			ret =
			    reiser4_get_parent(&plock, pos->lock.node,
					       ZNODE_WRITE_LOCK);
			if (ret)
				goto exit;

			ret = incr_load_count_znode(&pload, plock.node);
			if (ret)
				goto exit;

			ret =
			    find_child_ptr(plock.node, pos->lock.node, &pcoord);
			if (ret)
				goto exit;

			ret =
			    reverse_relocate_check_dirty_parent(ZJNODE
								(pos->lock.
								 node), &pcoord,
								pos);
			if (ret)
				goto exit;

			ret = alloc_one_ancestor(&pcoord, pos);
			if (ret)
				goto exit;
		}

		ret = allocate_znode(pos->lock.node, &pcoord, pos);
	}
exit:
	done_load_count(&pload);
	done_lh(&plock);
	return ret;
}

/* This is the recursive step described in alloc_pos_and_ancestors, above.
   Ignoring the call to set_preceder, which is the next function described, this
   checks if the child is a leftmost child and returns if it is not. If the
   child is a leftmost child it checks for relocation, possibly dirtying the
   parent. Then it performs the recursive step. */
static int alloc_one_ancestor(const coord_t *coord, flush_pos_t *pos)
{
	int ret = 0;
	lock_handle alock;
	load_count aload;
	coord_t acoord;

	/* As we ascend at the left-edge of the region to flush, take this
	   opportunity at the twig level to find our parent-first preceder
	   unless we have already set it. */
	if (pos->preceder.blk == 0) {
		ret = set_preceder(coord, pos);
		if (ret != 0)
			return ret;
	}

	/* If the ancestor is clean or already allocated, or if the child is not
	   a leftmost child, stop going up, even leaving coord->node not
	   flushprepped. */
	if (znode_check_flushprepped(coord->node)
	    || !coord_is_leftmost_unit(coord))
		return 0;

	init_lh(&alock);
	init_load_count(&aload);
	coord_init_invalid(&acoord, NULL);

	/* Only ascend to the next level if it is a leftmost child, but
	   write-lock the parent in case we will relocate the child. */
	if (!znode_is_root(coord->node)) {

		ret =
		    jnode_lock_parent_coord(ZJNODE(coord->node), &acoord,
					    &alock, &aload, ZNODE_WRITE_LOCK,
					    0);
		if (ret != 0) {
			/* FIXME(C): check EINVAL, E_DEADLOCK */
			goto exit;
		}

		ret =
		    reverse_relocate_check_dirty_parent(ZJNODE(coord->node),
							&acoord, pos);
		if (ret != 0)
			goto exit;

		/* Recursive call. */
		if (!znode_check_flushprepped(acoord.node)) {
			ret = alloc_one_ancestor(&acoord, pos);
			if (ret)
				goto exit;
		}
	}

	/* Note: we call allocate with the parent write-locked (except at the
	   root) in case we relocate the child, in which case it will modify the
	   parent during this call. */
	ret = allocate_znode(coord->node, &acoord, pos);

exit:
	done_load_count(&aload);
	done_lh(&alock);
	return ret;
}

/* During the reverse parent-first alloc_pos_and_ancestors process described
   above there is a call to this function at the twig level. During
   alloc_pos_and_ancestors we may ask: should this node be relocated (in reverse
   parent-first context)?  We repeat this process as long as the child is the
   leftmost child, eventually reaching an ancestor of the flush point that is
   not a leftmost child. The preceder of that ancestors, which is not a leftmost
   child, is actually on the leaf level. The preceder of that block is the
   left-neighbor of the flush point. The preceder of that block is the rightmost
   child of the twig on the left. So, when alloc_pos_and_ancestors passes upward
   through the twig level, it stops momentarily to remember the block of the
   rightmost child of the twig on the left and sets it to the flush_position's
   preceder_hint.

   There is one other place where we may set the flush_position's preceder hint,
   which is during scan-left.
*/
static int set_preceder(const coord_t *coord_in, flush_pos_t *pos)
{
	int ret;
	coord_t coord;
	lock_handle left_lock;
	load_count left_load;

	coord_dup(&coord, coord_in);

	init_lh(&left_lock);
	init_load_count(&left_load);

	/* FIXME(B): Same FIXME as in "Find the preceder" in
	   reverse_relocate_test. coord_is_leftmost_unit is not the right test
	   if the unformatted child is in the middle of the first extent unit.*/
	if (!coord_is_leftmost_unit(&coord)) {
		coord_prev_unit(&coord);
	} else {
		ret =
		    reiser4_get_left_neighbor(&left_lock, coord.node,
					      ZNODE_READ_LOCK, GN_SAME_ATOM);
		if (ret) {
			/* If we fail for any reason it doesn't matter because
			   the preceder is only a hint. We are low-priority at
			   this point, so this must be the case. */
			if (ret == -E_REPEAT || ret == -E_NO_NEIGHBOR ||
			    ret == -ENOENT || ret == -EINVAL
			    || ret == -E_DEADLOCK)
				ret = 0;
			goto exit;
		}

		ret = incr_load_count_znode(&left_load, left_lock.node);
		if (ret)
			goto exit;

		coord_init_last_unit(&coord, left_lock.node);
	}

	ret =
	    item_utmost_child_real_block(&coord, RIGHT_SIDE,
					 &pos->preceder.blk);
exit:
	check_preceder(pos->preceder.blk);
	done_load_count(&left_load);
	done_lh(&left_lock);
	return ret;
}

/* MAIN SQUEEZE AND ALLOCATE LOOP (THREE BIG FUNCTIONS) */

/* This procedure implements the outer loop of the flush algorithm. To put this
   in context, here is the general list of steps taken by the flush routine as a
   whole:

   1. Scan-left
   2. Scan-right (maybe)
   3. Allocate initial flush position and its ancestors
   4. <handle extents>
   5. <squeeze and next position and its ancestors to-the-right,
       then update position to-the-right>
   6. <repeat from #4 until flush is stopped>

   This procedure implements the loop in steps 4 through 6 in the above listing.

   Step 4: if the current flush position is an extent item (position on the twig
   level), it allocates the extent (allocate_extent_item_in_place) then shifts
   to the next coordinate. If the next coordinate's leftmost child needs
   flushprep, we will continue. If the next coordinate is an internal item, we
   descend back to the leaf level, otherwise we repeat a step #4 (labeled
   ALLOC_EXTENTS below). If the "next coordinate" brings us past the end of the
   twig level, then we call reverse_relocate_end_of_twig to possibly dirty the
   next (right) twig, prior to step #5 which moves to the right.

   Step 5: calls squalloc_changed_ancestors, which initiates a recursive call up
   the tree to allocate any ancestors of the next-right flush position that are
   not also ancestors of the current position. Those ancestors (in top-down
   order) are the next in parent-first order. We squeeze adjacent nodes on the
   way up until the right node and current node share the same parent, then
   allocate on the way back down. Finally, this step sets the flush position to
   the next-right node.  Then repeat steps 4 and 5.
*/

/* SQUEEZE CODE */

/* squalloc_right_twig helper function, cut a range of extent items from
   cut node to->node from the beginning up to coord @to. */
static int squalloc_right_twig_cut(coord_t *to, reiser4_key * to_key,
				   znode * left)
{
	coord_t from;
	reiser4_key from_key;

	coord_init_first_unit(&from, to->node);
	item_key_by_coord(&from, &from_key);

	return cut_node_content(&from, to, &from_key, to_key, NULL);
}

/* Copy as much of the leading extents from @right to @left, allocating
   unallocated extents as they are copied.  Returns SQUEEZE_TARGET_FULL or
   SQUEEZE_SOURCE_EMPTY when no more can be shifted.  If the next item is an
   internal item it calls shift_one_internal_unit and may then return
   SUBTREE_MOVED. */
static int squeeze_right_twig(znode * left, znode * right, flush_pos_t *pos)
{
	int ret = SUBTREE_MOVED;
	coord_t coord;		/* used to iterate over items */
	reiser4_key stop_key;
	reiser4_tree *tree;

	assert("jmacd-2008", !node_is_empty(right));
	coord_init_first_unit(&coord, right);

	/* FIXME: can be optimized to cut once */
	while (!node_is_empty(coord.node) && item_is_extent(&coord)) {
		ON_DEBUG(void *vp);

		assert("vs-1468", coord_is_leftmost_unit(&coord));
		ON_DEBUG(vp = shift_check_prepare(left, coord.node));

		/* stop_key is used to find what was copied and what to cut */
		stop_key = *reiser4_min_key();
		ret = squalloc_extent(left, &coord, pos, &stop_key);
		if (ret != SQUEEZE_CONTINUE) {
			ON_DEBUG(kfree(vp));
			break;
		}
		assert("vs-1465", !keyeq(&stop_key, reiser4_min_key()));

		/* Helper function to do the cutting. */
		set_key_offset(&stop_key, get_key_offset(&stop_key) - 1);
		check_me("vs-1466",
			 squalloc_right_twig_cut(&coord, &stop_key, left) == 0);

		ON_DEBUG(shift_check(vp, left, coord.node));
	}
	/*
	 * @left and @right nodes participated in the
	 * implicit shift, determined by the pair of
	 * functions:
	 * . squalloc_extent() - append units to the @left
	 * . squalloc_right_twig_cut() - cut the units from @right
	 * so update their delimiting keys
	 */
	tree = znode_get_tree(left);
	write_lock_dk(tree);
	update_znode_dkeys(left, right);
	write_unlock_dk(tree);

	if (node_is_empty(coord.node))
		ret = SQUEEZE_SOURCE_EMPTY;

	if (ret == SQUEEZE_TARGET_FULL)
		goto out;

	if (node_is_empty(right)) {
		/* The whole right node was copied into @left. */
		assert("vs-464", ret == SQUEEZE_SOURCE_EMPTY);
		goto out;
	}

	coord_init_first_unit(&coord, right);

	if (!item_is_internal(&coord)) {
		/* we do not want to squeeze anything else to left neighbor
		   because "slum" is over */
		ret = SQUEEZE_TARGET_FULL;
		goto out;
	}
	assert("jmacd-433", item_is_internal(&coord));

	/* Shift an internal unit.  The child must be allocated before shifting
	   any more extents, so we stop here. */
	ret = shift_one_internal_unit(left, right);

out:
	assert("jmacd-8612", ret < 0 || ret == SQUEEZE_TARGET_FULL
	       || ret == SUBTREE_MOVED || ret == SQUEEZE_SOURCE_EMPTY);

	if (ret == SQUEEZE_TARGET_FULL) {
		/* We submit prepped nodes here and expect that this @left twig
		 * will not be modified again during this jnode_flush() call. */
		int ret1;

		/* NOTE: seems like io is done under long term locks. */
		ret1 = write_prepped_nodes(pos);
		if (ret1 < 0)
			return ret1;
	}

	return ret;
}

#if REISER4_DEBUG
static void item_convert_invariant(flush_pos_t *pos)
{
	assert("edward-1225", coord_is_existing_item(&pos->coord));
	if (chaining_data_present(pos)) {
		item_plugin *iplug = item_convert_plug(pos);

		assert("edward-1000",
		       iplug == item_plugin_by_coord(&pos->coord));
		assert("edward-1001", iplug->f.convert != NULL);
	} else
		assert("edward-1226", pos->child == NULL);
}
#else

#define item_convert_invariant(pos) noop

#endif

/* Scan node items starting from the first one and apply for each
   item its flush ->convert() method (if any). This method may
   resize/kill the item so the tree will be changed.
*/
static int convert_node(flush_pos_t *pos, znode * node)
{
	int ret = 0;
	item_plugin *iplug;

	assert("edward-304", pos != NULL);
	assert("edward-305", pos->child == NULL);
	assert("edward-475", znode_convertible(node));
	assert("edward-669", znode_is_wlocked(node));
	assert("edward-1210", !node_is_empty(node));

	if (znode_get_level(node) != LEAF_LEVEL)
		/* unsupported */
		goto exit;

	coord_init_first_unit(&pos->coord, node);

	while (1) {
		ret = 0;
		coord_set_to_left(&pos->coord);
		item_convert_invariant(pos);

		iplug = item_plugin_by_coord(&pos->coord);
		assert("edward-844", iplug != NULL);

		if (iplug->f.convert) {
			ret = iplug->f.convert(pos);
			if (ret)
				goto exit;
		}
		assert("edward-307", pos->child == NULL);

		if (coord_next_item(&pos->coord)) {
			/* node is over */

			if (!chaining_data_present(pos))
				/* finished this node */
				break;
			if (should_chain_next_node(pos)) {
				/* go to next node */
				move_chaining_data(pos, 0/* to next node */);
				break;
			}
			/* repeat this node */
			move_chaining_data(pos, 1/* this node */);
			continue;
		}
		/* Node is not over.
		   Check if there is attached convert data.
		   If so roll one item position back and repeat
		   on this node
		 */
		if (chaining_data_present(pos)) {

			if (iplug != item_plugin_by_coord(&pos->coord))
				set_item_convert_count(pos, 0);

			ret = coord_prev_item(&pos->coord);
			assert("edward-1003", !ret);

			move_chaining_data(pos, 1/* this node */);
		}
	}
	JF_CLR(ZJNODE(node), JNODE_CONVERTIBLE);
	znode_make_dirty(node);
exit:
	assert("edward-1004", !ret);
	return ret;
}

/* Squeeze and allocate the right neighbor.  This is called after @left and
   its current children have been squeezed and allocated already.  This
   procedure's job is to squeeze and items from @right to @left.

   If at the leaf level, use the shift_everything_left memcpy-optimized
   version of shifting (squeeze_right_leaf).

   If at the twig level, extents are allocated as they are shifted from @right
   to @left (squalloc_right_twig).

   At any other level, shift one internal item and return to the caller
   (squalloc_parent_first) so that the shifted-subtree can be processed in
   parent-first order.

   When unit of internal item is moved, squeezing stops and SUBTREE_MOVED is
   returned.  When all content of @right is squeezed, SQUEEZE_SOURCE_EMPTY is
   returned.  If nothing can be moved into @left anymore, SQUEEZE_TARGET_FULL
   is returned.
*/

static int squeeze_right_neighbor(flush_pos_t *pos, znode * left,
				  znode * right)
{
	int ret;

	/* FIXME it is possible to see empty hasn't-heard-banshee node in a
	 * tree owing to error (for example, ENOSPC) in write */
	/* assert("jmacd-9321", !node_is_empty(left)); */
	assert("jmacd-9322", !node_is_empty(right));
	assert("jmacd-9323", znode_get_level(left) == znode_get_level(right));

	switch (znode_get_level(left)) {
	case TWIG_LEVEL:
		/* Shift with extent allocating until either an internal item
		   is encountered or everything is shifted or no free space
		   left in @left */
		ret = squeeze_right_twig(left, right, pos);
		break;

	default:
		/* All other levels can use shift_everything until we implement
		   per-item flush plugins. */
		ret = squeeze_right_non_twig(left, right);
		break;
	}

	assert("jmacd-2011", (ret < 0 ||
			      ret == SQUEEZE_SOURCE_EMPTY
			      || ret == SQUEEZE_TARGET_FULL
			      || ret == SUBTREE_MOVED));
	return ret;
}

static int squeeze_right_twig_and_advance_coord(flush_pos_t *pos,
						znode * right)
{
	int ret;

	ret = squeeze_right_twig(pos->lock.node, right, pos);
	if (ret < 0)
		return ret;
	if (ret > 0) {
		coord_init_after_last_item(&pos->coord, pos->lock.node);
		return ret;
	}

	coord_init_last_unit(&pos->coord, pos->lock.node);
	return 0;
}

/* forward declaration */
static int squalloc_upper_levels(flush_pos_t *, znode *, znode *);

/* do a fast check for "same parents" condition before calling
 * squalloc_upper_levels() */
static inline int check_parents_and_squalloc_upper_levels(flush_pos_t *pos,
							  znode * left,
							  znode * right)
{
	if (znode_same_parents(left, right))
		return 0;

	return squalloc_upper_levels(pos, left, right);
}

/* Check whether the parent of given @right node needs to be processes
   ((re)allocated) prior to processing of the child.  If @left and @right do not
   share at least the parent of the @right is after the @left but before the
   @right in parent-first order, we have to (re)allocate it before the @right
   gets (re)allocated. */
static int squalloc_upper_levels(flush_pos_t *pos, znode * left, znode * right)
{
	int ret;

	lock_handle left_parent_lock;
	lock_handle right_parent_lock;

	load_count left_parent_load;
	load_count right_parent_load;

	init_lh(&left_parent_lock);
	init_lh(&right_parent_lock);

	init_load_count(&left_parent_load);
	init_load_count(&right_parent_load);

	ret = reiser4_get_parent(&left_parent_lock, left, ZNODE_WRITE_LOCK);
	if (ret)
		goto out;

	ret = reiser4_get_parent(&right_parent_lock, right, ZNODE_WRITE_LOCK);
	if (ret)
		goto out;

	/* Check for same parents */
	if (left_parent_lock.node == right_parent_lock.node)
		goto out;

	if (znode_check_flushprepped(right_parent_lock.node)) {
		/* Keep parent-first order.  In the order, the right parent node
		   stands before the @right node.  If it is already allocated,
		   we set the preceder (next block search start point) to its
		   block number, @right node should be allocated after it.

		   However, preceder is set only if the right parent is on twig
		   level. The explanation is the following: new branch nodes are
		   allocated over already allocated children while the tree
		   grows, it is difficult to keep tree ordered, we assume that
		   only leaves and twings are correctly allocated. So, only
		   twigs are used as a preceder for allocating of the rest of
		   the slum. */
		if (znode_get_level(right_parent_lock.node) == TWIG_LEVEL) {
			pos->preceder.blk =
			    *znode_get_block(right_parent_lock.node);
			check_preceder(pos->preceder.blk);
		}
		goto out;
	}

	ret = incr_load_count_znode(&left_parent_load, left_parent_lock.node);
	if (ret)
		goto out;

	ret = incr_load_count_znode(&right_parent_load, right_parent_lock.node);
	if (ret)
		goto out;

	ret =
	    squeeze_right_neighbor(pos, left_parent_lock.node,
				   right_parent_lock.node);
	/* We stop if error. We stop if some items/units were shifted (ret == 0)
	 * and thus @right changed its parent. It means we have not process
	 * right_parent node prior to processing of @right. Positive return
	 * values say that shifting items was not happen because of "empty
	 * source" or "target full" conditions. */
	if (ret <= 0)
		goto out;

	/* parent(@left) and parent(@right) may have different parents also. We
	 * do a recursive call for checking that. */
	ret =
	    check_parents_and_squalloc_upper_levels(pos, left_parent_lock.node,
						    right_parent_lock.node);
	if (ret)
		goto out;

	/* allocate znode when going down */
	ret = lock_parent_and_allocate_znode(right_parent_lock.node, pos);

out:
	done_load_count(&left_parent_load);
	done_load_count(&right_parent_load);

	done_lh(&left_parent_lock);
	done_lh(&right_parent_lock);

	return ret;
}

/* Check the leftmost child "flushprepped" status, also returns true if child
 * node was not found in cache.  */
static int leftmost_child_of_unit_check_flushprepped(const coord_t *coord)
{
	int ret;
	int prepped;

	jnode *child;

	ret = get_leftmost_child_of_unit(coord, &child);

	if (ret)
		return ret;

	if (child) {
		prepped = jnode_check_flushprepped(child);
		jput(child);
	} else {
		/* We consider not existing child as a node which slum
		   processing should not continue to.  Not cached node is clean,
		   so it is flushprepped. */
		prepped = 1;
	}

	return prepped;
}

/* (re)allocate znode with automated getting parent node */
static int lock_parent_and_allocate_znode(znode * node, flush_pos_t *pos)
{
	int ret;
	lock_handle parent_lock;
	load_count parent_load;
	coord_t pcoord;

	assert("zam-851", znode_is_write_locked(node));

	init_lh(&parent_lock);
	init_load_count(&parent_load);

	ret = reiser4_get_parent(&parent_lock, node, ZNODE_WRITE_LOCK);
	if (ret)
		goto out;

	ret = incr_load_count_znode(&parent_load, parent_lock.node);
	if (ret)
		goto out;

	ret = find_child_ptr(parent_lock.node, node, &pcoord);
	if (ret)
		goto out;

	ret = allocate_znode(node, &pcoord, pos);

out:
	done_load_count(&parent_load);
	done_lh(&parent_lock);
	return ret;
}

/* Process nodes on leaf level until unformatted node or rightmost node in the
 * slum reached.  */
static int handle_pos_on_formatted(flush_pos_t *pos)
{
	int ret;
	lock_handle right_lock;
	load_count right_load;

	init_lh(&right_lock);
	init_load_count(&right_load);

	if (should_convert_node(pos, pos->lock.node)) {
		ret = convert_node(pos, pos->lock.node);
		if (ret)
			return ret;
	}

	while (1) {
		int expected;
		expected = should_convert_next_node(pos);
		ret = neighbor_in_slum(pos->lock.node, &right_lock, RIGHT_SIDE,
				       ZNODE_WRITE_LOCK, !expected, expected);
		if (ret) {
			if (expected)
				warning("edward-1495",
				"Expected neighbor not found (ret = %d). Fsck?",
					ret);
			break;
		}

		/* we don't prep(allocate) nodes for flushing twice. This can be
		 * suboptimal, or it can be optimal. For now we choose to live
		 * with the risk that it will be suboptimal because it would be
		 * quite complex to code it to be smarter. */
		if (znode_check_flushprepped(right_lock.node)
		    && !znode_convertible(right_lock.node)) {
			assert("edward-1005", !should_convert_next_node(pos));
			pos_stop(pos);
			break;
		}

		ret = incr_load_count_znode(&right_load, right_lock.node);
		if (ret)
			break;
		if (should_convert_node(pos, right_lock.node)) {
			ret = convert_node(pos, right_lock.node);
			if (ret)
				break;
			if (node_is_empty(right_lock.node)) {
				/* node became empty after converting, repeat */
				done_load_count(&right_load);
				done_lh(&right_lock);
				continue;
			}
		}

		/* squeeze _before_ going upward. */
		ret =
		    squeeze_right_neighbor(pos, pos->lock.node,
					   right_lock.node);
		if (ret < 0)
			break;

		if (znode_check_flushprepped(right_lock.node)) {
			if (should_convert_next_node(pos)) {
				/* in spite of flushprepped status of the node,
				   its right slum neighbor should be converted*/
				assert("edward-953", convert_data(pos));
				assert("edward-954", item_convert_data(pos));

				if (node_is_empty(right_lock.node)) {
					done_load_count(&right_load);
					done_lh(&right_lock);
				} else
					move_flush_pos(pos, &right_lock,
						       &right_load, NULL);
				continue;
			}
			pos_stop(pos);
			break;
		}

		if (node_is_empty(right_lock.node)) {
			/* repeat if right node was squeezed completely */
			done_load_count(&right_load);
			done_lh(&right_lock);
			continue;
		}

		/* parent(right_lock.node) has to be processed before
		 * (right_lock.node) due to "parent-first" allocation order. */
		ret =
		    check_parents_and_squalloc_upper_levels(pos, pos->lock.node,
							    right_lock.node);
		if (ret)
			break;
		/* (re)allocate _after_ going upward */
		ret = lock_parent_and_allocate_znode(right_lock.node, pos);
		if (ret)
			break;
		if (should_terminate_squalloc(pos)) {
			set_item_convert_count(pos, 0);
			break;
		}

		/* advance the flush position to the right neighbor */
		move_flush_pos(pos, &right_lock, &right_load, NULL);

		ret = rapid_flush(pos);
		if (ret)
			break;
	}
	check_convert_info(pos);
	done_load_count(&right_load);
	done_lh(&right_lock);

	/* This function indicates via pos whether to stop or go to twig or
	 * continue on current level. */
	return ret;

}

/* Process nodes on leaf level until unformatted node or rightmost node in the
 * slum reached.  */
static int handle_pos_on_leaf(flush_pos_t *pos)
{
	int ret;

	assert("zam-845", pos->state == POS_ON_LEAF);

	ret = handle_pos_on_formatted(pos);

	if (ret == -E_NO_NEIGHBOR) {
		/* cannot get right neighbor, go process extents. */
		pos->state = POS_TO_TWIG;
		return 0;
	}

	return ret;
}

/* Process slum on level > 1 */
static int handle_pos_on_internal(flush_pos_t *pos)
{
	assert("zam-850", pos->state == POS_ON_INTERNAL);
	return handle_pos_on_formatted(pos);
}

/* check whether squalloc should stop before processing given extent */
static int squalloc_extent_should_stop(flush_pos_t *pos)
{
	assert("zam-869", item_is_extent(&pos->coord));

	/* pos->child is a jnode handle_pos_on_extent() should start with in
	 * stead of the first child of the first extent unit. */
	if (pos->child) {
		int prepped;

		assert("vs-1383", jnode_is_unformatted(pos->child));
		prepped = jnode_check_flushprepped(pos->child);
		pos->pos_in_unit =
		    jnode_get_index(pos->child) -
		    extent_unit_index(&pos->coord);
		assert("vs-1470",
		       pos->pos_in_unit < extent_unit_width(&pos->coord));
		assert("nikita-3434",
		       ergo(extent_is_unallocated(&pos->coord),
			    pos->pos_in_unit == 0));
		jput(pos->child);
		pos->child = NULL;

		return prepped;
	}

	pos->pos_in_unit = 0;
	if (extent_is_unallocated(&pos->coord))
		return 0;

	return leftmost_child_of_unit_check_flushprepped(&pos->coord);
}

/* Handle the case when regular reiser4 tree (znodes connected one to its
 * neighbors by sibling pointers) is interrupted on leaf level by one or more
 * unformatted nodes.  By having a lock on twig level and use extent code
 * routines to process unformatted nodes we swim around an irregular part of
 * reiser4 tree. */
static int handle_pos_on_twig(flush_pos_t *pos)
{
	int ret;

	assert("zam-844", pos->state == POS_ON_EPOINT);
	assert("zam-843", item_is_extent(&pos->coord));

	/* We decide should we continue slum processing with current extent
	   unit: if leftmost child of current extent unit is flushprepped
	   (i.e. clean or already processed by flush) we stop squalloc().  There
	   is a fast check for unallocated extents which we assume contain all
	   not flushprepped nodes. */
	/* FIXME: Here we implement simple check, we are only looking on the
	   leftmost child. */
	ret = squalloc_extent_should_stop(pos);
	if (ret != 0) {
		pos_stop(pos);
		return ret;
	}

	while (pos_valid(pos) && coord_is_existing_unit(&pos->coord)
	       && item_is_extent(&pos->coord)) {
		ret = reiser4_alloc_extent(pos);
		if (ret)
			break;
		coord_next_unit(&pos->coord);
	}

	if (coord_is_after_rightmost(&pos->coord)) {
		pos->state = POS_END_OF_TWIG;
		return 0;
	}
	if (item_is_internal(&pos->coord)) {
		pos->state = POS_TO_LEAF;
		return 0;
	}

	assert("zam-860", item_is_extent(&pos->coord));

	/* "slum" is over */
	pos->state = POS_INVALID;
	return 0;
}

/* When we about to return flush position from twig to leaf level we can process
 * the right twig node or move position to the leaf.  This processes right twig
 * if it is possible and jump to leaf level if not. */
static int handle_pos_end_of_twig(flush_pos_t *pos)
{
	int ret;
	lock_handle right_lock;
	load_count right_load;
	coord_t at_right;
	jnode *child = NULL;

	assert("zam-848", pos->state == POS_END_OF_TWIG);
	assert("zam-849", coord_is_after_rightmost(&pos->coord));

	init_lh(&right_lock);
	init_load_count(&right_load);

	/* We get a lock on the right twig node even it is not dirty because
	 * slum continues or discontinues on leaf level not on next twig. This
	 * lock on the right twig is needed for getting its leftmost child. */
	ret =
	    reiser4_get_right_neighbor(&right_lock, pos->lock.node,
				       ZNODE_WRITE_LOCK, GN_SAME_ATOM);
	if (ret)
		goto out;

	ret = incr_load_count_znode(&right_load, right_lock.node);
	if (ret)
		goto out;

	/* right twig could be not dirty */
	if (JF_ISSET(ZJNODE(right_lock.node), JNODE_DIRTY)) {
		/* If right twig node is dirty we always attempt to squeeze it
		 * content to the left... */
became_dirty:
		ret =
		    squeeze_right_twig_and_advance_coord(pos, right_lock.node);
		if (ret <= 0) {
			/* pos->coord is on internal item, go to leaf level, or
			 * we have an error which will be caught in squalloc()
			 */
			pos->state = POS_TO_LEAF;
			goto out;
		}

		/* If right twig was squeezed completely we wave to re-lock
		 * right twig. now it is done through the top-level squalloc
		 * routine. */
		if (node_is_empty(right_lock.node))
			goto out;

		/* ... and prep it if it is not yet prepped */
		if (!znode_check_flushprepped(right_lock.node)) {
			/* As usual, process parent before ... */
			ret =
			    check_parents_and_squalloc_upper_levels(pos,
								    pos->lock.
								    node,
								    right_lock.
								    node);
			if (ret)
				goto out;

			/* ... processing the child */
			ret =
			    lock_parent_and_allocate_znode(right_lock.node,
							   pos);
			if (ret)
				goto out;
		}
	} else {
		coord_init_first_unit(&at_right, right_lock.node);

		/* check first child of next twig, should we continue there ? */
		ret = get_leftmost_child_of_unit(&at_right, &child);
		if (ret || child == NULL || jnode_check_flushprepped(child)) {
			pos_stop(pos);
			goto out;
		}

		/* check clean twig for possible relocation */
		if (!znode_check_flushprepped(right_lock.node)) {
			ret =
			    reverse_relocate_check_dirty_parent(child,
								&at_right, pos);
			if (ret)
				goto out;
			if (JF_ISSET(ZJNODE(right_lock.node), JNODE_DIRTY))
				goto became_dirty;
		}
	}

	assert("zam-875", znode_check_flushprepped(right_lock.node));

	/* Update the preceder by a block number of just processed right twig
	 * node. The code above could miss the preceder updating because
	 * allocate_znode() could not be called for this node. */
	pos->preceder.blk = *znode_get_block(right_lock.node);
	check_preceder(pos->preceder.blk);

	coord_init_first_unit(&at_right, right_lock.node);
	assert("zam-868", coord_is_existing_unit(&at_right));

	pos->state = item_is_extent(&at_right) ? POS_ON_EPOINT : POS_TO_LEAF;
	move_flush_pos(pos, &right_lock, &right_load, &at_right);

out:
	done_load_count(&right_load);
	done_lh(&right_lock);

	if (child)
		jput(child);

	return ret;
}

/* Move the pos->lock to leaf node pointed by pos->coord, check should we
 * continue there. */
static int handle_pos_to_leaf(flush_pos_t *pos)
{
	int ret;
	lock_handle child_lock;
	load_count child_load;
	jnode *child;

	assert("zam-846", pos->state == POS_TO_LEAF);
	assert("zam-847", item_is_internal(&pos->coord));

	init_lh(&child_lock);
	init_load_count(&child_load);

	ret = get_leftmost_child_of_unit(&pos->coord, &child);
	if (ret)
		return ret;
	if (child == NULL) {
		pos_stop(pos);
		return 0;
	}

	if (jnode_check_flushprepped(child)) {
		pos->state = POS_INVALID;
		goto out;
	}

	ret =
	    longterm_lock_znode(&child_lock, JZNODE(child), ZNODE_WRITE_LOCK,
				ZNODE_LOCK_LOPRI);
	if (ret)
		goto out;

	ret = incr_load_count_znode(&child_load, JZNODE(child));
	if (ret)
		goto out;

	ret = allocate_znode(JZNODE(child), &pos->coord, pos);
	if (ret)
		goto out;

	/* move flush position to leaf level */
	pos->state = POS_ON_LEAF;
	move_flush_pos(pos, &child_lock, &child_load, NULL);

	if (node_is_empty(JZNODE(child))) {
		ret = delete_empty_node(JZNODE(child));
		pos->state = POS_INVALID;
	}
out:
	done_load_count(&child_load);
	done_lh(&child_lock);
	jput(child);

	return ret;
}

/* move pos from leaf to twig, and move lock from leaf to twig. */
/* Move pos->lock to upper (twig) level */
static int handle_pos_to_twig(flush_pos_t *pos)
{
	int ret;

	lock_handle parent_lock;
	load_count parent_load;
	coord_t pcoord;

	assert("zam-852", pos->state == POS_TO_TWIG);

	init_lh(&parent_lock);
	init_load_count(&parent_load);

	ret =
	    reiser4_get_parent(&parent_lock, pos->lock.node, ZNODE_WRITE_LOCK);
	if (ret)
		goto out;

	ret = incr_load_count_znode(&parent_load, parent_lock.node);
	if (ret)
		goto out;

	ret = find_child_ptr(parent_lock.node, pos->lock.node, &pcoord);
	if (ret)
		goto out;

	assert("zam-870", item_is_internal(&pcoord));
	coord_next_item(&pcoord);

	if (coord_is_after_rightmost(&pcoord))
		pos->state = POS_END_OF_TWIG;
	else if (item_is_extent(&pcoord))
		pos->state = POS_ON_EPOINT;
	else {
		/* Here we understand that getting -E_NO_NEIGHBOR in
		 * handle_pos_on_leaf() was because of just a reaching edge of
		 * slum */
		pos_stop(pos);
		goto out;
	}

	move_flush_pos(pos, &parent_lock, &parent_load, &pcoord);

out:
	done_load_count(&parent_load);
	done_lh(&parent_lock);

	return ret;
}

typedef int (*pos_state_handle_t) (flush_pos_t *);
static pos_state_handle_t flush_pos_handlers[] = {
	/* process formatted nodes on leaf level, keep lock on a leaf node */
	[POS_ON_LEAF] = handle_pos_on_leaf,
	/* process unformatted nodes, keep lock on twig node, pos->coord points
	 * to extent currently being processed */
	[POS_ON_EPOINT] = handle_pos_on_twig,
	/* move a lock from leaf node to its parent for further processing of
	   unformatted nodes */
	[POS_TO_TWIG] = handle_pos_to_twig,
	/* move a lock from twig to leaf level when a processing of unformatted
	 * nodes finishes, pos->coord points to the leaf node we jump to */
	[POS_TO_LEAF] = handle_pos_to_leaf,
	/* after processing last extent in the twig node, attempting to shift
	 * items from the twigs right neighbor and process them while shifting*/
	[POS_END_OF_TWIG] = handle_pos_end_of_twig,
	/* process formatted nodes on internal level, keep lock on an internal
	   node */
	[POS_ON_INTERNAL] = handle_pos_on_internal
};

/* Advance flush position horizontally, prepare for flushing ((re)allocate,
 * squeeze, encrypt) nodes and their ancestors in "parent-first" order */
static int squalloc(flush_pos_t *pos)
{
	int ret = 0;

	/* maybe needs to be made a case statement with handle_pos_on_leaf as
	 * first case, for greater CPU efficiency? Measure and see.... -Hans */
	while (pos_valid(pos)) {
		ret = flush_pos_handlers[pos->state] (pos);
		if (ret < 0)
			break;

		ret = rapid_flush(pos);
		if (ret)
			break;
	}

	/* any positive value or -E_NO_NEIGHBOR are legal return codes for
	   handle_pos* routines, -E_NO_NEIGHBOR means that slum edge was
	   reached */
	if (ret > 0 || ret == -E_NO_NEIGHBOR)
		ret = 0;

	return ret;
}

static void update_ldkey(znode * node)
{
	reiser4_key ldkey;

	assert_rw_write_locked(&(znode_get_tree(node)->dk_lock));
	if (node_is_empty(node))
		return;

	znode_set_ld_key(node, leftmost_key_in_node(node, &ldkey));
}

/* this is to be called after calling of shift node's method to shift data from
   @right to @left. It sets left delimiting keys of @left and @right to keys of
   first items of @left and @right correspondingly and sets right delimiting key
   of @left to first key of @right */
static void update_znode_dkeys(znode * left, znode * right)
{
	assert_rw_write_locked(&(znode_get_tree(right)->dk_lock));
	assert("vs-1629", (znode_is_write_locked(left) &&
			   znode_is_write_locked(right)));

	/* we need to update left delimiting of left if it was empty before
	   shift */
	update_ldkey(left);
	update_ldkey(right);
	if (node_is_empty(right))
		znode_set_rd_key(left, znode_get_rd_key(right));
	else
		znode_set_rd_key(left, znode_get_ld_key(right));
}

/* try to shift everything from @right to @left. If everything was shifted -
   @right is removed from the tree.  Result is the number of bytes shifted. */
static int
shift_everything_left(znode * right, znode * left, carry_level * todo)
{
	coord_t from;
	node_plugin *nplug;
	carry_plugin_info info;

	coord_init_after_last_item(&from, right);

	nplug = node_plugin_by_node(right);
	info.doing = NULL;
	info.todo = todo;
	return nplug->shift(&from, left, SHIFT_LEFT,
			    1 /* delete @right if it becomes empty */ ,
			    1
			    /* move coord @from to node @left if everything will
			       be shifted */
			    ,
			    &info);
}

/* Shift as much as possible from @right to @left using the memcpy-optimized
   shift_everything_left.  @left and @right are formatted neighboring nodes on
   leaf level. */
static int squeeze_right_non_twig(znode * left, znode * right)
{
	int ret;
	carry_pool *pool;
	carry_level *todo;

	assert("nikita-2246", znode_get_level(left) == znode_get_level(right));

	if (!JF_ISSET(ZJNODE(left), JNODE_DIRTY) ||
	    !JF_ISSET(ZJNODE(right), JNODE_DIRTY))
		return SQUEEZE_TARGET_FULL;

	pool = init_carry_pool(sizeof(*pool) + 3 * sizeof(*todo));
	if (IS_ERR(pool))
		return PTR_ERR(pool);
	todo = (carry_level *) (pool + 1);
	init_carry_level(todo, pool);

	ret = shift_everything_left(right, left, todo);
	if (ret > 0) {
		/* something was shifted */
		reiser4_tree *tree;
		__u64 grabbed;

		znode_make_dirty(left);
		znode_make_dirty(right);

		/* update delimiting keys of nodes which participated in
		   shift. FIXME: it would be better to have this in shift
		   node's operation. But it can not be done there. Nobody
		   remembers why, though */
		tree = znode_get_tree(left);
		write_lock_dk(tree);
		update_znode_dkeys(left, right);
		write_unlock_dk(tree);

		/* Carry is called to update delimiting key and, maybe, to
		   remove empty node. */
		grabbed = get_current_context()->grabbed_blocks;
		ret = reiser4_grab_space_force(tree->height, BA_RESERVED);
		assert("nikita-3003", ret == 0);	/* reserved space is
							exhausted. Ask Hans. */
		ret = reiser4_carry(todo, NULL/* previous level */);
		grabbed2free_mark(grabbed);
	} else {
		/* Shifting impossible, we return appropriate result code */
		ret =
		    node_is_empty(right) ? SQUEEZE_SOURCE_EMPTY :
		    SQUEEZE_TARGET_FULL;
	}

	done_carry_pool(pool);

	return ret;
}

#if REISER4_DEBUG
static int sibling_link_is_ok(const znode *left, const znode *right)
{
	int result;

	read_lock_tree(znode_get_tree(left));
	result = (left->right == right && left == right->left);
	read_unlock_tree(znode_get_tree(left));
	return result;
}
#endif

/* Shift first unit of first item if it is an internal one.  Return
   SQUEEZE_TARGET_FULL if it fails to shift an item, otherwise return
   SUBTREE_MOVED. */
static int shift_one_internal_unit(znode * left, znode * right)
{
	int ret;
	carry_pool *pool;
	carry_level *todo;
	coord_t *coord;
	carry_plugin_info *info;
	int size, moved;

	assert("nikita-2247", znode_get_level(left) == znode_get_level(right));
	assert("nikita-2435", znode_is_write_locked(left));
	assert("nikita-2436", znode_is_write_locked(right));
	assert("nikita-2434", sibling_link_is_ok(left, right));

	pool = init_carry_pool(sizeof(*pool) + 3 * sizeof(*todo) +
			       sizeof(*coord) + sizeof(*info)
#if REISER4_DEBUG
			       + sizeof(*coord) + 2 * sizeof(reiser4_key)
#endif
	    );
	if (IS_ERR(pool))
		return PTR_ERR(pool);
	todo = (carry_level *) (pool + 1);
	init_carry_level(todo, pool);

	coord = (coord_t *) (todo + 3);
	coord_init_first_unit(coord, right);
	info = (carry_plugin_info *) (coord + 1);

#if REISER4_DEBUG
	if (!node_is_empty(left)) {
		coord_t *last;
		reiser4_key *right_key;
		reiser4_key *left_key;

		last = (coord_t *) (info + 1);
		right_key = (reiser4_key *) (last + 1);
		left_key = right_key + 1;
		coord_init_last_unit(last, left);

		assert("nikita-2463",
		       keyle(item_key_by_coord(last, left_key),
			     item_key_by_coord(coord, right_key)));
	}
#endif

	assert("jmacd-2007", item_is_internal(coord));

	size = item_length_by_coord(coord);
	info->todo = todo;
	info->doing = NULL;

	ret = node_plugin_by_node(left)->shift(coord, left, SHIFT_LEFT,
					       1
					       /* delete @right if it becomes
						  empty */
					       ,
					       0
					       /* do not move coord @coord to
						  node @left */
					       ,
					       info);

	/* If shift returns positive, then we shifted the item. */
	assert("vs-423", ret <= 0 || size == ret);
	moved = (ret > 0);

	if (moved) {
		/* something was moved */
		reiser4_tree *tree;
		int grabbed;

		znode_make_dirty(left);
		znode_make_dirty(right);
		tree = znode_get_tree(left);
		write_lock_dk(tree);
		update_znode_dkeys(left, right);
		write_unlock_dk(tree);

		/* reserve space for delimiting keys after shifting */
		grabbed = get_current_context()->grabbed_blocks;
		ret = reiser4_grab_space_force(tree->height, BA_RESERVED);
		assert("nikita-3003", ret == 0);	/* reserved space is
							exhausted. Ask Hans. */

		ret = reiser4_carry(todo, NULL/* previous level */);
		grabbed2free_mark(grabbed);
	}

	done_carry_pool(pool);

	if (ret != 0) {
		/* Shift or carry operation failed. */
		assert("jmacd-7325", ret < 0);
		return ret;
	}

	return moved ? SUBTREE_MOVED : SQUEEZE_TARGET_FULL;
}

/* Make the final relocate/wander decision during forward parent-first squalloc
   for a znode. For unformatted nodes this is done in
   plugin/item/extent.c:extent_needs_allocation(). */
static int
allocate_znode_loaded(znode * node,
		      const coord_t *parent_coord, flush_pos_t *pos)
{
	int ret;
	reiser4_super_info_data *sbinfo = get_current_super_private();
	/* FIXME(D): We have the node write-locked and should have checked for !
	   allocated() somewhere before reaching this point, but there can be a
	   race, so this assertion is bogus. */
	assert("jmacd-7987", !jnode_check_flushprepped(ZJNODE(node)));
	assert("jmacd-7988", znode_is_write_locked(node));
	assert("jmacd-7989", coord_is_invalid(parent_coord)
	       || znode_is_write_locked(parent_coord->node));

	if (ZF_ISSET(node, JNODE_REPACK) || ZF_ISSET(node, JNODE_CREATED) ||
	    znode_is_root(node) ||
	    /* We have enough nodes to relocate no matter what. */
	    (pos->leaf_relocate != 0 && znode_get_level(node) == LEAF_LEVEL)) {
		/* No need to decide with new nodes, they are treated the same
		   as relocate. If the root node is dirty, relocate. */
		if (pos->preceder.blk == 0) {
			/* preceder is unknown and we have decided to relocate
			   node -- using of default value for search start is
			   better than search from block #0. */
			get_blocknr_hint_default(&pos->preceder.blk);
			check_preceder(pos->preceder.blk);
		}

		goto best_reloc;

	} else if (pos->preceder.blk == 0) {
		/* If we don't know the preceder, leave it where it is. */
		jnode_make_wander(ZJNODE(node));
	} else {
		/* Make a decision based on block distance. */
		reiser4_block_nr dist;
		reiser4_block_nr nblk = *znode_get_block(node);

		assert("jmacd-6172", !reiser4_blocknr_is_fake(&nblk));
		assert("jmacd-6173", !reiser4_blocknr_is_fake(&pos->preceder.blk));
		assert("jmacd-6174", pos->preceder.blk != 0);

		if (pos->preceder.blk == nblk - 1) {
			/* Ideal. */
			jnode_make_wander(ZJNODE(node));
		} else {

			dist =
			    (nblk <
			     pos->preceder.blk) ? (pos->preceder.blk -
						   nblk) : (nblk -
							    pos->preceder.blk);

			/* See if we can find a closer block
			   (forward direction only). */
			pos->preceder.max_dist =
			    min((reiser4_block_nr) sbinfo->flush.
				relocate_distance, dist);
			pos->preceder.level = znode_get_level(node);

			ret = allocate_znode_update(node, parent_coord, pos);

			pos->preceder.max_dist = 0;

			if (ret && (ret != -ENOSPC))
				return ret;

			if (ret == 0) {
				/* Got a better allocation. */
				znode_make_reloc(node, pos->fq);
			} else if (dist < sbinfo->flush.relocate_distance) {
				/* The present allocation is good enough. */
				jnode_make_wander(ZJNODE(node));
			} else {
				/* Otherwise, try to relocate to the best
				   position. */
best_reloc:
				ret =
				    allocate_znode_update(node, parent_coord,
							  pos);
				if (ret != 0)
					return ret;

				/* set JNODE_RELOC bit _after_ node gets
				   allocated */
				znode_make_reloc(node, pos->fq);
			}
		}
	}

	/* This is the new preceder. */
	pos->preceder.blk = *znode_get_block(node);
	check_preceder(pos->preceder.blk);
	pos->alloc_cnt += 1;

	assert("jmacd-4277", !reiser4_blocknr_is_fake(&pos->preceder.blk));

	return 0;
}

static int
allocate_znode(znode * node, const coord_t *parent_coord, flush_pos_t *pos)
{
	/*
	 * perform znode allocation with znode pinned in memory to avoid races
	 * with asynchronous emergency flush (which plays with
	 * JNODE_FLUSH_RESERVED bit).
	 */
	return WITH_DATA(node, allocate_znode_loaded(node, parent_coord, pos));
}

/* A subroutine of allocate_znode, this is called first to see if there is a
   close position to relocate to. It may return ENOSPC if there is no close
   position. If there is no close position it may not relocate. This takes care
   of updating the parent node with the relocated block address. */
static int
allocate_znode_update(znode * node, const coord_t *parent_coord,
		      flush_pos_t *pos)
{
	int ret;
	reiser4_block_nr blk;
	lock_handle uber_lock;
	int flush_reserved_used = 0;
	int grabbed;
	reiser4_context *ctx;
	reiser4_super_info_data *sbinfo;

	init_lh(&uber_lock);

	ctx = get_current_context();
	sbinfo = get_super_private(ctx->super);

	grabbed = ctx->grabbed_blocks;

	/* discard e-flush allocation */
	ret = zload(node);
	if (ret)
		return ret;

	if (ZF_ISSET(node, JNODE_CREATED)) {
		assert("zam-816", reiser4_blocknr_is_fake(znode_get_block(node)));
		pos->preceder.block_stage = BLOCK_UNALLOCATED;
	} else {
		pos->preceder.block_stage = BLOCK_GRABBED;

		/* The disk space for relocating the @node is already reserved
		 * in "flush reserved" counter if @node is leaf, otherwise we
		 * grab space using BA_RESERVED (means grab space from whole
		 * disk not from only 95%). */
		if (znode_get_level(node) == LEAF_LEVEL) {
			/*
			 * earlier (during do_jnode_make_dirty()) we decided
			 * that @node can possibly go into overwrite set and
			 * reserved block for its wandering location.
			 */
			txn_atom *atom = get_current_atom_locked();
			assert("nikita-3449",
			       ZF_ISSET(node, JNODE_FLUSH_RESERVED));
			flush_reserved2grabbed(atom, (__u64) 1);
			spin_unlock_atom(atom);
			/*
			 * we are trying to move node into relocate
			 * set. Allocation of relocated position "uses"
			 * reserved block.
			 */
			ZF_CLR(node, JNODE_FLUSH_RESERVED);
			flush_reserved_used = 1;
		} else {
			ret = reiser4_grab_space_force((__u64) 1, BA_RESERVED);
			if (ret != 0)
				goto exit;
		}
	}

	/* We may do not use 5% of reserved disk space here and flush will not
	   pack tightly. */
	ret = reiser4_alloc_block(&pos->preceder, &blk,
				  BA_FORMATTED | BA_PERMANENT);
	if (ret)
		goto exit;

	if (!ZF_ISSET(node, JNODE_CREATED) &&
	    (ret =
	     reiser4_dealloc_block(znode_get_block(node), 0,
				   BA_DEFER | BA_FORMATTED)))
		goto exit;

	if (likely(!znode_is_root(node))) {
		item_plugin *iplug;

		iplug = item_plugin_by_coord(parent_coord);
		assert("nikita-2954", iplug->f.update != NULL);
		iplug->f.update(parent_coord, &blk);

		znode_make_dirty(parent_coord->node);

	} else {
		reiser4_tree *tree = znode_get_tree(node);
		znode *uber;

		/* We take a longterm lock on the fake node in order to change
		   the root block number.  This may cause atom fusion. */
		ret = get_uber_znode(tree, ZNODE_WRITE_LOCK, ZNODE_LOCK_HIPRI,
				     &uber_lock);
		/* The fake node cannot be deleted, and we must have priority
		   here, and may not be confused with ENOSPC. */
		assert("jmacd-74412",
		       ret != -EINVAL && ret != -E_DEADLOCK && ret != -ENOSPC);

		if (ret)
			goto exit;

		uber = uber_lock.node;

		write_lock_tree(tree);
		tree->root_block = blk;
		write_unlock_tree(tree);

		znode_make_dirty(uber);
	}

	ret = znode_rehash(node, &blk);
exit:
	if (ret) {
		/* Get flush reserved block back if something fails, because
		 * callers assume that on error block wasn't relocated and its
		 * flush reserved block wasn't used. */
		if (flush_reserved_used) {
			/*
			 * ok, we failed to move node into relocate
			 * set. Restore status quo.
			 */
			grabbed2flush_reserved((__u64) 1);
			ZF_SET(node, JNODE_FLUSH_RESERVED);
		}
	}
	zrelse(node);
	done_lh(&uber_lock);
	grabbed2free_mark(grabbed);
	return ret;
}

/* JNODE INTERFACE */

/* Lock a node (if formatted) and then get its parent locked, set the child's
   coordinate in the parent.  If the child is the root node, the above_root
   znode is returned but the coord is not set.  This function may cause atom
   fusion, but it is only used for read locks (at this point) and therefore
   fusion only occurs when the parent is already dirty. */
/* Hans adds this note: remember to ask how expensive this operation is vs.
   storing parent pointer in jnodes. */
static int
jnode_lock_parent_coord(jnode * node,
			coord_t *coord,
			lock_handle * parent_lh,
			load_count * parent_zh,
			znode_lock_mode parent_mode, int try)
{
	int ret;

	assert("edward-53", jnode_is_unformatted(node) || jnode_is_znode(node));
	assert("edward-54", jnode_is_unformatted(node)
	       || znode_is_any_locked(JZNODE(node)));

	if (!jnode_is_znode(node)) {
		reiser4_key key;
		tree_level stop_level = TWIG_LEVEL;
		lookup_bias bias = FIND_EXACT;

		assert("edward-168", !(jnode_get_type(node) == JNODE_BITMAP));

		/* The case when node is not znode, but can have parent coord
		   (unformatted node, node which represents cluster page,
		   etc..).  Generate a key for the appropriate entry, search
		   in the tree using coord_by_key, which handles locking for
		   us. */

		/*
		 * nothing is locked at this moment, so, nothing prevents
		 * concurrent truncate from removing jnode from inode. To
		 * prevent this spin-lock jnode. jnode can be truncated just
		 * after call to the jnode_build_key(), but this is ok,
		 * because coord_by_key() will just fail to find appropriate
		 * extent.
		 */
		spin_lock_jnode(node);
		if (!JF_ISSET(node, JNODE_HEARD_BANSHEE)) {
			jnode_build_key(node, &key);
			ret = 0;
		} else
			ret = RETERR(-ENOENT);
		spin_unlock_jnode(node);

		if (ret != 0)
			return ret;

		if (jnode_is_cluster_page(node))
			stop_level = LEAF_LEVEL;

		assert("jmacd-1812", coord != NULL);

		ret = coord_by_key(jnode_get_tree(node), &key, coord, parent_lh,
				   parent_mode, bias, stop_level, stop_level,
				   CBK_UNIQUE, NULL/*ra_info */);
		switch (ret) {
		case CBK_COORD_NOTFOUND:
			assert("edward-1038",
			       ergo(jnode_is_cluster_page(node),
				    JF_ISSET(node, JNODE_HEARD_BANSHEE)));
			if (!JF_ISSET(node, JNODE_HEARD_BANSHEE))
				warning("nikita-3177", "Parent not found");
			return ret;
		case CBK_COORD_FOUND:
			if (coord->between != AT_UNIT) {
				/* FIXME: comment needed */
				done_lh(parent_lh);
				if (!JF_ISSET(node, JNODE_HEARD_BANSHEE)) {
					warning("nikita-3178",
						"Found but not happy: %i",
						coord->between);
				}
				return RETERR(-ENOENT);
			}
			ret = incr_load_count_znode(parent_zh, parent_lh->node);
			if (ret != 0)
				return ret;
			/* if (jnode_is_cluster_page(node)) {
			   races with write() are possible
			   check_child_cluster (parent_lh->node);
			   }
			 */
			break;
		default:
			return ret;
		}

	} else {
		int flags;
		znode *z;

		z = JZNODE(node);
		/* Formatted node case: */
		assert("jmacd-2061", !znode_is_root(z));

		flags = GN_ALLOW_NOT_CONNECTED;
		if (try)
			flags |= GN_TRY_LOCK;

		ret =
		    reiser4_get_parent_flags(parent_lh, z, parent_mode, flags);
		if (ret != 0)
			/* -E_REPEAT is ok here, it is handled by the caller. */
			return ret;

		/* Make the child's position "hint" up-to-date.  (Unless above
		   root, which caller must check.) */
		if (coord != NULL) {

			ret = incr_load_count_znode(parent_zh, parent_lh->node);
			if (ret != 0) {
				warning("jmacd-976812386",
					"incr_load_count_znode failed: %d",
					ret);
				return ret;
			}

			ret = find_child_ptr(parent_lh->node, z, coord);
			if (ret != 0) {
				warning("jmacd-976812",
					"find_child_ptr failed: %d", ret);
				return ret;
			}
		}
	}

	return 0;
}

/* Get the (locked) next neighbor of a znode which is dirty and a member of the
   same atom. If there is no next neighbor or the neighbor is not in memory or
   if there is a neighbor but it is not dirty or not in the same atom,
   -E_NO_NEIGHBOR is returned. In some cases the slum may include nodes which
   are not dirty, if so @check_dirty should be 0 */
static int neighbor_in_slum(znode * node,	/* starting point */
			    lock_handle * lock,	/* lock on starting point */
			    sideof side,	/* left or right direction we
						   seek the next node in */
			    znode_lock_mode mode, /* kind of lock we want */
			    int check_dirty, 	/* true if the neighbor should
						   be dirty */
			    int use_upper_levels /* get neighbor by going though
						    upper levels */)
{
	int ret;
	int flags;

	assert("jmacd-6334", znode_is_connected(node));

	flags =  GN_SAME_ATOM | (side == LEFT_SIDE ? GN_GO_LEFT : 0);
	if (use_upper_levels)
		flags |= GN_CAN_USE_UPPER_LEVELS;

	ret = reiser4_get_neighbor(lock, node, mode, flags);
	if (ret) {
		/* May return -ENOENT or -E_NO_NEIGHBOR. */
		/* FIXME(C): check EINVAL, E_DEADLOCK */
		if (ret == -ENOENT)
			ret = RETERR(-E_NO_NEIGHBOR);
		return ret;
	}
	if (!check_dirty)
		return 0;
	/* Check dirty bit of locked znode, no races here */
	if (JF_ISSET(ZJNODE(lock->node), JNODE_DIRTY))
		return 0;

	done_lh(lock);
	return RETERR(-E_NO_NEIGHBOR);
}

/* Return true if two znodes have the same parent.  This is called with both
   nodes write-locked (for squeezing) so no tree lock is needed. */
static int znode_same_parents(znode * a, znode * b)
{
	int result;

	assert("jmacd-7011", znode_is_write_locked(a));
	assert("jmacd-7012", znode_is_write_locked(b));

	/* We lock the whole tree for this check.... I really don't like whole
	 * tree locks... -Hans */
	read_lock_tree(znode_get_tree(a));
	result = (znode_parent(a) == znode_parent(b));
	read_unlock_tree(znode_get_tree(a));
	return result;
}

/* FLUSH SCAN */

/* Initialize the flush_scan data structure. */
static void scan_init(flush_scan * scan)
{
	memset(scan, 0, sizeof(*scan));
	init_lh(&scan->node_lock);
	init_lh(&scan->parent_lock);
	init_load_count(&scan->parent_load);
	init_load_count(&scan->node_load);
	coord_init_invalid(&scan->parent_coord, NULL);
}

/* Release any resources held by the flush scan, e.g. release locks,
   free memory, etc. */
static void scan_done(flush_scan * scan)
{
	done_load_count(&scan->node_load);
	if (scan->node != NULL) {
		jput(scan->node);
		scan->node = NULL;
	}
	done_load_count(&scan->parent_load);
	done_lh(&scan->parent_lock);
	done_lh(&scan->node_lock);
}

/* Returns true if flush scanning is finished. */
int reiser4_scan_finished(flush_scan * scan)
{
	return scan->stop || (scan->direction == RIGHT_SIDE &&
			      scan->count >= scan->max_count);
}

/* Return true if the scan should continue to the @tonode. True if the node
   meets the same_slum_check condition. If not, deref the "left" node and stop
   the scan. */
int reiser4_scan_goto(flush_scan * scan, jnode * tonode)
{
	int go = same_slum_check(scan->node, tonode, 1, 0);

	if (!go) {
		scan->stop = 1;
		jput(tonode);
	}

	return go;
}

/* Set the current scan->node, refcount it, increment count by the @add_count
   (number to count, e.g., skipped unallocated nodes), deref previous current,
   and copy the current parent coordinate. */
int
scan_set_current(flush_scan * scan, jnode * node, unsigned add_count,
		 const coord_t *parent)
{
	/* Release the old references, take the new reference. */
	done_load_count(&scan->node_load);

	if (scan->node != NULL)
		jput(scan->node);
	scan->node = node;
	scan->count += add_count;

	/* This next stmt is somewhat inefficient.  The reiser4_scan_extent()
	   code could delay this update step until it finishes and update the
	   parent_coord only once. It did that before, but there was a bug and
	   this was the easiest way to make it correct. */
	if (parent != NULL)
		coord_dup(&scan->parent_coord, parent);

	/* Failure may happen at the incr_load_count call, but the caller can
	   assume the reference is safely taken. */
	return incr_load_count_jnode(&scan->node_load, node);
}

/* Return true if scanning in the leftward direction. */
int reiser4_scanning_left(flush_scan * scan)
{
	return scan->direction == LEFT_SIDE;
}

/* Performs leftward scanning starting from either kind of node. Counts the
   starting node. The right-scan object is passed in for the left-scan in order
   to copy the parent of an unformatted starting position. This way we avoid
   searching for the unformatted node's parent when scanning in each direction.
   If we search for the parent once it is set in both scan objects. The limit
   parameter tells flush-scan when to stop.

   Rapid scanning is used only during scan_left, where we are interested in
   finding the 'leftpoint' where we begin flushing. We are interested in
   stopping at the left child of a twig that does not have a dirty left
   neighbour. THIS IS A SPECIAL CASE. The problem is finding a way to flush only
   those nodes without unallocated children, and it is difficult to solve in the
   bottom-up flushing algorithm we are currently using. The problem can be
   solved by scanning left at every level as we go upward, but this would
   basically bring us back to using a top-down allocation strategy, which we
   already tried (see BK history from May 2002), and has a different set of
   problems. The top-down strategy makes avoiding unallocated children easier,
   but makes it difficult to propertly flush dirty children with clean parents
   that would otherwise stop the top-down flush, only later to dirty the parent
   once the children are flushed. So we solve the problem in the bottom-up
   algorithm with a special case for twigs and leaves only.

   The first step in solving the problem is this rapid leftward scan.  After we
   determine that there are at least enough nodes counted to qualify for
   FLUSH_RELOCATE_THRESHOLD we are no longer interested in the exact count, we
   are only interested in finding the best place to start the flush.

   We could choose one of two possibilities:

   1. Stop at the leftmost child (of a twig) that does not have a dirty left
   neighbor. This requires checking one leaf per rapid-scan twig

   2. Stop at the leftmost child (of a twig) where there are no dirty children
   of the twig to the left. This requires checking possibly all of the in-memory
   children of each twig during the rapid scan.

   For now we implement the first policy.
*/
static int
scan_left(flush_scan * scan, flush_scan * right, jnode * node, unsigned limit)
{
	int ret = 0;

	scan->max_count = limit;
	scan->direction = LEFT_SIDE;

	ret = scan_set_current(scan, jref(node), 1, NULL);
	if (ret != 0)
		return ret;

	ret = scan_common(scan, right);
	if (ret != 0)
		return ret;

	/* Before rapid scanning, we need a lock on scan->node so that we can
	   get its parent, only if formatted. */
	if (jnode_is_znode(scan->node)) {
		ret = longterm_lock_znode(&scan->node_lock, JZNODE(scan->node),
					  ZNODE_WRITE_LOCK, ZNODE_LOCK_LOPRI);
	}

	/* Rapid_scan would go here (with limit set to FLUSH_RELOCATE_THRESHOLD)
	*/
	return ret;
}

/* Performs rightward scanning... Does not count the starting node. The limit
   parameter is described in scan_left. If the starting node is unformatted then
   the parent_coord was already set during scan_left. The rapid_after parameter
   is not used during right-scanning.

   scan_right is only called if the scan_left operation does not count at least
   FLUSH_RELOCATE_THRESHOLD nodes for flushing.  Otherwise, the limit parameter
   is set to the difference between scan-left's count and
   FLUSH_RELOCATE_THRESHOLD, meaning scan-right counts as high as
   FLUSH_RELOCATE_THRESHOLD and then stops. */
static int scan_right(flush_scan * scan, jnode * node, unsigned limit)
{
	int ret;

	scan->max_count = limit;
	scan->direction = RIGHT_SIDE;

	ret = scan_set_current(scan, jref(node), 0, NULL);
	if (ret != 0)
		return ret;

	return scan_common(scan, NULL);
}

/* Common code to perform left or right scanning. */
static int scan_common(flush_scan * scan, flush_scan * other)
{
	int ret;

	assert("nikita-2376", scan->node != NULL);
	assert("edward-54", jnode_is_unformatted(scan->node)
	       || jnode_is_znode(scan->node));

	/* Special case for starting at an unformatted node. Optimization: we
	   only want to search for the parent (which requires a tree traversal)
	   once. Obviously, we shouldn't have to call it once for the left scan
	   and once for the right scan. For this reason, if we search for the
	   parent during scan-left we then duplicate the coord/lock/load into
	   the scan-right object. */
	if (jnode_is_unformatted(scan->node)) {
		ret = scan_unformatted(scan, other);
		if (ret != 0)
			return ret;
	}
	/* This loop expects to start at a formatted position and performs
	   chaining of formatted regions */
	while (!reiser4_scan_finished(scan)) {

		ret = scan_formatted(scan);
		if (ret != 0)
			return ret;
	}

	return 0;
}

static int scan_unformatted(flush_scan * scan, flush_scan * other)
{
	int ret = 0;
	int try = 0;

	if (!coord_is_invalid(&scan->parent_coord))
		goto scan;

	/* set parent coord from */
	if (!jnode_is_unformatted(scan->node)) {
		/* formatted position */

		lock_handle lock;
		assert("edward-301", jnode_is_znode(scan->node));
		init_lh(&lock);

		/*
		 * when flush starts from unformatted node, first thing it
		 * does is tree traversal to find formatted parent of starting
		 * node. This parent is then kept lock across scans to the
		 * left and to the right. This means that during scan to the
		 * left we cannot take left-ward lock, because this is
		 * dead-lock prone. So, if we are scanning to the left and
		 * there is already lock held by this thread,
		 * jnode_lock_parent_coord() should use try-lock.
		 */
		try = reiser4_scanning_left(scan)
		    && !lock_stack_isclean(get_current_lock_stack());
		/* Need the node locked to get the parent lock, We have to
		   take write lock since there is at least one call path
		   where this znode is already write-locked by us. */
		ret =
		    longterm_lock_znode(&lock, JZNODE(scan->node),
					ZNODE_WRITE_LOCK,
					reiser4_scanning_left(scan) ?
					ZNODE_LOCK_LOPRI :
					ZNODE_LOCK_HIPRI);
		if (ret != 0)
			/* EINVAL or E_DEADLOCK here mean... try again!  At this
			   point we've scanned too far and can't back out, just
			   start over. */
			return ret;

		ret = jnode_lock_parent_coord(scan->node,
					      &scan->parent_coord,
					      &scan->parent_lock,
					      &scan->parent_load,
					      ZNODE_WRITE_LOCK, try);

		/* FIXME(C): check EINVAL, E_DEADLOCK */
		done_lh(&lock);
		if (ret == -E_REPEAT) {
			scan->stop = 1;
			return 0;
		}
		if (ret)
			return ret;

	} else {
		/* unformatted position */

		ret =
		    jnode_lock_parent_coord(scan->node, &scan->parent_coord,
					    &scan->parent_lock,
					    &scan->parent_load,
					    ZNODE_WRITE_LOCK, try);

		if (IS_CBKERR(ret))
			return ret;

		if (ret == CBK_COORD_NOTFOUND)
			/* FIXME(C): check EINVAL, E_DEADLOCK */
			return ret;

		/* parent was found */
		assert("jmacd-8661", other != NULL);
		/* Duplicate the reference into the other flush_scan. */
		coord_dup(&other->parent_coord, &scan->parent_coord);
		copy_lh(&other->parent_lock, &scan->parent_lock);
		copy_load_count(&other->parent_load, &scan->parent_load);
	}
scan:
	return scan_by_coord(scan);
}

/* Performs left- or rightward scanning starting from a formatted node. Follow
   left pointers under tree lock as long as:

   - node->left/right is non-NULL
   - node->left/right is connected, dirty
   - node->left/right belongs to the same atom
   - scan has not reached maximum count
*/
static int scan_formatted(flush_scan * scan)
{
	int ret;
	znode *neighbor = NULL;

	assert("jmacd-1401", !reiser4_scan_finished(scan));

	do {
		znode *node = JZNODE(scan->node);

		/* Node should be connected, but if not stop the scan. */
		if (!znode_is_connected(node)) {
			scan->stop = 1;
			break;
		}

		/* Lock the tree, check-for and reference the next sibling. */
		read_lock_tree(znode_get_tree(node));

		/* It may be that a node is inserted or removed between a node
		   and its left sibling while the tree lock is released, but the
		   flush-scan count does not need to be precise. Thus, we
		   release the tree lock as soon as we get the neighboring node.
		*/
		neighbor =
			reiser4_scanning_left(scan) ? node->left : node->right;
		if (neighbor != NULL)
			zref(neighbor);

		read_unlock_tree(znode_get_tree(node));

		/* If neighbor is NULL at the leaf level, need to check for an
		   unformatted sibling using the parent--break in any case. */
		if (neighbor == NULL)
			break;

		/* Check the condition for going left, break if it is not met.
		   This also releases (jputs) the neighbor if false. */
		if (!reiser4_scan_goto(scan, ZJNODE(neighbor)))
			break;

		/* Advance the flush_scan state to the left, repeat. */
		ret = scan_set_current(scan, ZJNODE(neighbor), 1, NULL);
		if (ret != 0)
			return ret;

	} while (!reiser4_scan_finished(scan));

	/* If neighbor is NULL then we reached the end of a formatted region, or
	   else the sibling is out of memory, now check for an extent to the
	   left (as long as LEAF_LEVEL). */
	if (neighbor != NULL || jnode_get_level(scan->node) != LEAF_LEVEL
	    || reiser4_scan_finished(scan)) {
		scan->stop = 1;
		return 0;
	}
	/* Otherwise, calls scan_by_coord for the right(left)most item of the
	   left(right) neighbor on the parent level, then possibly continue. */

	coord_init_invalid(&scan->parent_coord, NULL);
	return scan_unformatted(scan, NULL);
}

/* NOTE-EDWARD:
   This scans adjacent items of the same type and calls scan flush plugin for
   each one. Performs left(right)ward scanning starting from a (possibly)
   unformatted node. If we start from unformatted node, then we continue only if
   the next neighbor is also unformatted. When called from scan_formatted, we
   skip first iteration (to make sure that right(left)most item of the
   left(right) neighbor on the parent level is of the same type and set
   appropriate coord). */
static int scan_by_coord(flush_scan * scan)
{
	int ret = 0;
	int scan_this_coord;
	lock_handle next_lock;
	load_count next_load;
	coord_t next_coord;
	jnode *child;
	item_plugin *iplug;

	init_lh(&next_lock);
	init_load_count(&next_load);
	scan_this_coord = (jnode_is_unformatted(scan->node) ? 1 : 0);

	/* set initial item id */
	iplug = item_plugin_by_coord(&scan->parent_coord);

	for (; !reiser4_scan_finished(scan); scan_this_coord = 1) {
		if (scan_this_coord) {
			/* Here we expect that unit is scannable. it would not
			 * be so due to race with extent->tail conversion.  */
			if (iplug->f.scan == NULL) {
				scan->stop = 1;
				ret = -E_REPEAT;
				/* skip the check at the end. */
				goto race;
			}

			ret = iplug->f.scan(scan);
			if (ret != 0)
				goto exit;

			if (reiser4_scan_finished(scan)) {
				checkchild(scan);
				break;
			}
		} else {
			/* the same race against truncate as above is possible
			 * here, it seems */

			/* NOTE-JMACD: In this case, apply the same end-of-node
			   logic but don't scan the first coordinate. */
			assert("jmacd-1231",
			       item_is_internal(&scan->parent_coord));
		}

		if (iplug->f.utmost_child == NULL
		    || znode_get_level(scan->parent_coord.node) != TWIG_LEVEL) {
			/* stop this coord and continue on parrent level */
			ret =
			    scan_set_current(scan,
					     ZJNODE(zref
						    (scan->parent_coord.node)),
					     1, NULL);
			if (ret != 0)
				goto exit;
			break;
		}

		/* Either way, the invariant is that scan->parent_coord is set
		   to the parent of scan->node. Now get the next unit. */
		coord_dup(&next_coord, &scan->parent_coord);
		coord_sideof_unit(&next_coord, scan->direction);

		/* If off-the-end of the twig, try the next twig. */
		if (coord_is_after_sideof_unit(&next_coord, scan->direction)) {
			/* We take the write lock because we may start flushing
			 * from this coordinate. */
			ret = neighbor_in_slum(next_coord.node,
					       &next_lock,
					       scan->direction,
					       ZNODE_WRITE_LOCK,
					       1 /* check dirty */,
					       0 /* don't go though upper
						    levels */);
			if (ret == -E_NO_NEIGHBOR) {
				scan->stop = 1;
				ret = 0;
				break;
			}

			if (ret != 0)
				goto exit;

			ret = incr_load_count_znode(&next_load, next_lock.node);
			if (ret != 0)
				goto exit;

			coord_init_sideof_unit(&next_coord, next_lock.node,
					       sideof_reverse(scan->direction));
		}

		iplug = item_plugin_by_coord(&next_coord);

		/* Get the next child. */
		ret =
		    iplug->f.utmost_child(&next_coord,
					  sideof_reverse(scan->direction),
					  &child);
		if (ret != 0)
			goto exit;
		/* If the next child is not in memory, or, item_utmost_child
		   failed (due to race with unlink, most probably), stop
		   here. */
		if (child == NULL || IS_ERR(child)) {
			scan->stop = 1;
			checkchild(scan);
			break;
		}

		assert("nikita-2374", jnode_is_unformatted(child)
		       || jnode_is_znode(child));

		/* See if it is dirty, part of the same atom. */
		if (!reiser4_scan_goto(scan, child)) {
			checkchild(scan);
			break;
		}

		/* If so, make this child current. */
		ret = scan_set_current(scan, child, 1, &next_coord);
		if (ret != 0)
			goto exit;

		/* Now continue.  If formatted we release the parent lock and
		   return, then proceed. */
		if (jnode_is_znode(child))
			break;

		/* Otherwise, repeat the above loop with next_coord. */
		if (next_load.node != NULL) {
			done_lh(&scan->parent_lock);
			move_lh(&scan->parent_lock, &next_lock);
			move_load_count(&scan->parent_load, &next_load);
		}
	}

	assert("jmacd-6233",
	       reiser4_scan_finished(scan) || jnode_is_znode(scan->node));
exit:
	checkchild(scan);
race:			/* skip the above check  */
	if (jnode_is_znode(scan->node)) {
		done_lh(&scan->parent_lock);
		done_load_count(&scan->parent_load);
	}

	done_load_count(&next_load);
	done_lh(&next_lock);
	return ret;
}

/* FLUSH POS HELPERS */

/* Initialize the fields of a flush_position. */
static void pos_init(flush_pos_t *pos)
{
	memset(pos, 0, sizeof *pos);

	pos->state = POS_INVALID;
	coord_init_invalid(&pos->coord, NULL);
	init_lh(&pos->lock);
	init_load_count(&pos->load);

	reiser4_blocknr_hint_init(&pos->preceder);
}

/* The flush loop inside squalloc periodically checks pos_valid to determine
   when "enough flushing" has been performed. This will return true until one
   of the following conditions is met:

   1. the number of flush-queued nodes has reached the kernel-supplied
   "int *nr_to_flush" parameter, meaning we have flushed as many blocks as the
   kernel requested. When flushing to commit, this parameter is NULL.

   2. pos_stop() is called because squalloc discovers that the "next" node in
   the flush order is either non-existant, not dirty, or not in the same atom.
*/

static int pos_valid(flush_pos_t *pos)
{
	return pos->state != POS_INVALID;
}

/* Release any resources of a flush_position. Called when jnode_flush
   finishes. */
static void pos_done(flush_pos_t *pos)
{
	pos_stop(pos);
	reiser4_blocknr_hint_done(&pos->preceder);
	if (convert_data(pos))
		free_convert_data(pos);
}

/* Reset the point and parent.  Called during flush subroutines to terminate the
   squalloc loop. */
static int pos_stop(flush_pos_t *pos)
{
	pos->state = POS_INVALID;
	done_lh(&pos->lock);
	done_load_count(&pos->load);
	coord_init_invalid(&pos->coord, NULL);

	if (pos->child) {
		jput(pos->child);
		pos->child = NULL;
	}

	return 0;
}

/* Return the flush_position's block allocator hint. */
reiser4_blocknr_hint *reiser4_pos_hint(flush_pos_t *pos)
{
	return &pos->preceder;
}

flush_queue_t *reiser4_pos_fq(flush_pos_t *pos)
{
	return pos->fq;
}

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 90
   LocalWords:  preceder
   End:
*/
