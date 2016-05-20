/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Routines and macros to:

   get_left_neighbor()

   get_right_neighbor()

   get_parent()

   get_first_child()

   get_last_child()

   various routines to walk the whole tree and do things to it like
   repack it, or move it to tertiary storage.  Please make them as
   generic as is reasonable.

*/

#include "forward.h"
#include "debug.h"
#include "dformat.h"
#include "coord.h"
#include "plugin/item/item.h"
#include "jnode.h"
#include "znode.h"
#include "tree_walk.h"
#include "tree.h"
#include "super.h"

/* These macros are used internally in tree_walk.c in attempt to make
   lock_neighbor() code usable to build lock_parent(), lock_right_neighbor,
   lock_left_neighbor */
#define GET_NODE_BY_PTR_OFFSET(node, off) (*(znode**)(((unsigned long)(node)) + (off)))
#define FIELD_OFFSET(name)  offsetof(znode, name)
#define PARENT_PTR_OFFSET FIELD_OFFSET(in_parent.node)
#define LEFT_PTR_OFFSET   FIELD_OFFSET(left)
#define RIGHT_PTR_OFFSET  FIELD_OFFSET(right)

/* This is the generic procedure to get and lock `generic' neighbor (left or
    right neighbor or parent). It implements common algorithm for all cases of
    getting lock on neighbor node, only znode structure field is different in
    each case. This is parameterized by ptr_offset argument, which is byte
    offset for the pointer to the desired neighbor within the current node's
    znode structure. This function should be called with the tree lock held */
static int lock_neighbor(
				/* resulting lock handle */
				lock_handle * result,
				/* znode to lock */
				znode * node,
				/* pointer to neighbor (or parent) znode field offset, in bytes from
				   the base address of znode structure  */
				int ptr_offset,
				/* lock mode for longterm_lock_znode call */
				znode_lock_mode mode,
				/* lock request for longterm_lock_znode call */
				znode_lock_request req,
				/* GN_* flags */
				int flags, int rlocked)
{
	reiser4_tree *tree = znode_get_tree(node);
	znode *neighbor;
	int ret;

	assert("umka-236", node != NULL);
	assert("umka-237", tree != NULL);
	assert_rw_locked(&(tree->tree_lock));

	if (flags & GN_TRY_LOCK)
		req |= ZNODE_LOCK_NONBLOCK;
	if (flags & GN_SAME_ATOM)
		req |= ZNODE_LOCK_DONT_FUSE;

	/* get neighbor's address by using of sibling link, quit while loop
	   (and return) if link is not available. */
	while (1) {
		neighbor = GET_NODE_BY_PTR_OFFSET(node, ptr_offset);

		/* return -E_NO_NEIGHBOR if parent or side pointer is NULL or if
		 * node pointed by it is not connected.
		 *
		 * However, GN_ALLOW_NOT_CONNECTED option masks "connected"
		 * check and allows passing reference to not connected znode to
		 * subsequent longterm_lock_znode() call.  This kills possible
		 * busy loop if we are trying to get longterm lock on locked but
		 * not yet connected parent node. */
		if (neighbor == NULL || !((flags & GN_ALLOW_NOT_CONNECTED)
					  || znode_is_connected(neighbor))) {
			return RETERR(-E_NO_NEIGHBOR);
		}

		/* protect it from deletion. */
		zref(neighbor);

		rlocked ? read_unlock_tree(tree) : write_unlock_tree(tree);

		ret = longterm_lock_znode(result, neighbor, mode, req);

		/* The lock handle obtains its own reference, release the one from above. */
		zput(neighbor);

		rlocked ? read_lock_tree(tree) : write_lock_tree(tree);

		/* restart if node we got reference to is being
		   invalidated. we should not get reference to this node
		   again. */
		if (ret == -EINVAL)
			continue;
		if (ret)
			return ret;

		/* check if neighbor link still points to just locked znode;
		   the link could have been changed while the process slept. */
		if (neighbor == GET_NODE_BY_PTR_OFFSET(node, ptr_offset))
			return 0;

		/* znode was locked by mistake; unlock it and restart locking
		   process from beginning. */
		rlocked ? read_unlock_tree(tree) : write_unlock_tree(tree);
		longterm_unlock_znode(result);
		rlocked ? read_lock_tree(tree) : write_lock_tree(tree);
	}
}

/* get parent node with longterm lock, accepts GN* flags. */
int reiser4_get_parent_flags(lock_handle * lh /* resulting lock handle */ ,
			     znode * node /* child node */ ,
			     znode_lock_mode mode
			     /* type of lock: read or write */ ,
			     int flags /* GN_* flags */ )
{
	int result;

	read_lock_tree(znode_get_tree(node));
	result = lock_neighbor(lh, node, PARENT_PTR_OFFSET, mode,
			       ZNODE_LOCK_HIPRI, flags, 1);
	read_unlock_tree(znode_get_tree(node));
	return result;
}

/* wrapper function to lock right or left neighbor depending on GN_GO_LEFT
   bit in @flags parameter  */
/* Audited by: umka (2002.06.14) */
static inline int
lock_side_neighbor(lock_handle * result,
		   znode * node, znode_lock_mode mode, int flags, int rlocked)
{
	int ret;
	int ptr_offset;
	znode_lock_request req;

	if (flags & GN_GO_LEFT) {
		ptr_offset = LEFT_PTR_OFFSET;
		req = ZNODE_LOCK_LOPRI;
	} else {
		ptr_offset = RIGHT_PTR_OFFSET;
		req = ZNODE_LOCK_HIPRI;
	}

	ret =
	    lock_neighbor(result, node, ptr_offset, mode, req, flags, rlocked);

	if (ret == -E_NO_NEIGHBOR)	/* if we walk left or right -E_NO_NEIGHBOR does not
					 * guarantee that neighbor is absent in the
					 * tree; in this case we return -ENOENT --
					 * means neighbor at least not found in
					 * cache */
		return RETERR(-ENOENT);

	return ret;
}

#if REISER4_DEBUG

int check_sibling_list(znode * node)
{
	znode *scan;
	znode *next;

	assert("nikita-3283", LOCK_CNT_GTZ(write_locked_tree));

	if (node == NULL)
		return 1;

	if (ZF_ISSET(node, JNODE_RIP))
		return 1;

	assert("nikita-3270", node != NULL);
	assert_rw_write_locked(&(znode_get_tree(node)->tree_lock));

	for (scan = node; znode_is_left_connected(scan); scan = next) {
		next = scan->left;
		if (next != NULL && !ZF_ISSET(next, JNODE_RIP)) {
			assert("nikita-3271", znode_is_right_connected(next));
			assert("nikita-3272", next->right == scan);
		} else
			break;
	}
	for (scan = node; znode_is_right_connected(scan); scan = next) {
		next = scan->right;
		if (next != NULL && !ZF_ISSET(next, JNODE_RIP)) {
			assert("nikita-3273", znode_is_left_connected(next));
			assert("nikita-3274", next->left == scan);
		} else
			break;
	}
	return 1;
}

#endif

/* Znode sibling pointers maintenence. */

/* Znode sibling pointers are established between any neighbored nodes which are
   in cache.  There are two znode state bits (JNODE_LEFT_CONNECTED,
   JNODE_RIGHT_CONNECTED), if left or right sibling pointer contains actual
   value (even NULL), corresponded JNODE_*_CONNECTED bit is set.

   Reiser4 tree operations which may allocate new znodes (CBK, tree balancing)
   take care about searching (hash table lookup may be required) of znode
   neighbors, establishing sibling pointers between them and setting
   JNODE_*_CONNECTED state bits. */

/* adjusting of sibling pointers and `connected' states for two
   neighbors; works if one neighbor is NULL (was not found). */

/* FIXME-VS: this is unstatic-ed to use in tree.c in prepare_twig_cut */
void link_left_and_right(znode * left, znode * right)
{
	assert("nikita-3275", check_sibling_list(left));
	assert("nikita-3275", check_sibling_list(right));

	if (left != NULL) {
		if (left->right == NULL) {
			left->right = right;
			ZF_SET(left, JNODE_RIGHT_CONNECTED);

			ON_DEBUG(left->right_version =
				 atomic_inc_return(&delim_key_version);
			    );

		} else if (ZF_ISSET(left->right, JNODE_HEARD_BANSHEE)
			   && left->right != right) {

			ON_DEBUG(left->right->left_version =
				 atomic_inc_return(&delim_key_version);
				 left->right_version =
				 atomic_inc_return(&delim_key_version););

			left->right->left = NULL;
			left->right = right;
			ZF_SET(left, JNODE_RIGHT_CONNECTED);
		} else
			/*
			 * there is a race condition in renew_sibling_link()
			 * and assertions below check that it is only one
			 * there. Thread T1 calls renew_sibling_link() without
			 * GN_NO_ALLOC flag. zlook() doesn't find neighbor
			 * node, but before T1 gets to the
			 * link_left_and_right(), another thread T2 creates
			 * neighbor node and connects it. check for
			 * left->right == NULL above protects T1 from
			 * overwriting correct left->right pointer installed
			 * by T2.
			 */
			assert("nikita-3302",
			       right == NULL || left->right == right);
	}
	if (right != NULL) {
		if (right->left == NULL) {
			right->left = left;
			ZF_SET(right, JNODE_LEFT_CONNECTED);

			ON_DEBUG(right->left_version =
				 atomic_inc_return(&delim_key_version);
			    );

		} else if (ZF_ISSET(right->left, JNODE_HEARD_BANSHEE)
			   && right->left != left) {

			ON_DEBUG(right->left->right_version =
				 atomic_inc_return(&delim_key_version);
				 right->left_version =
				 atomic_inc_return(&delim_key_version););

			right->left->right = NULL;
			right->left = left;
			ZF_SET(right, JNODE_LEFT_CONNECTED);

		} else
			assert("nikita-3303",
			       left == NULL || right->left == left);
	}
	assert("nikita-3275", check_sibling_list(left));
	assert("nikita-3275", check_sibling_list(right));
}

/* Audited by: umka (2002.06.14) */
static void link_znodes(znode * first, znode * second, int to_left)
{
	if (to_left)
		link_left_and_right(second, first);
	else
		link_left_and_right(first, second);
}

/* getting of next (to left or to right, depend on gn_to_left bit in flags)
   coord's unit position in horizontal direction, even across node
   boundary. Should be called under tree lock, it protects nonexistence of
   sibling link on parent level, if lock_side_neighbor() fails with
   -ENOENT. */
static int far_next_coord(coord_t * coord, lock_handle * handle, int flags)
{
	int ret;
	znode *node;
	reiser4_tree *tree;

	assert("umka-243", coord != NULL);
	assert("umka-244", handle != NULL);
	assert("zam-1069", handle->node == NULL);

	ret =
	    (flags & GN_GO_LEFT) ? coord_prev_unit(coord) :
	    coord_next_unit(coord);
	if (!ret)
		return 0;

	ret =
	    lock_side_neighbor(handle, coord->node, ZNODE_READ_LOCK, flags, 0);
	if (ret)
		return ret;

	node = handle->node;
	tree = znode_get_tree(node);
	write_unlock_tree(tree);

	coord_init_zero(coord);

	/* We avoid synchronous read here if it is specified by flag. */
	if ((flags & GN_ASYNC) && znode_page(handle->node) == NULL) {
		ret = jstartio(ZJNODE(handle->node));
		if (!ret)
			ret = -E_REPEAT;
		goto error_locked;
	}

	/* corresponded zrelse() should be called by the clients of
	   far_next_coord(), in place when this node gets unlocked. */
	ret = zload(handle->node);
	if (ret)
		goto error_locked;

	if (flags & GN_GO_LEFT)
		coord_init_last_unit(coord, node);
	else
		coord_init_first_unit(coord, node);

	if (0) {
	      error_locked:
		longterm_unlock_znode(handle);
	}
	write_lock_tree(tree);
	return ret;
}

/* Very significant function which performs a step in horizontal direction
   when sibling pointer is not available.  Actually, it is only function which
   does it.
   Note: this function does not restore locking status at exit,
   caller should does care about proper unlocking and zrelsing */
static int
renew_sibling_link(coord_t * coord, lock_handle * handle, znode * child,
		   tree_level level, int flags, int *nr_locked)
{
	int ret;
	int to_left = flags & GN_GO_LEFT;
	reiser4_block_nr da;
	/* parent of the neighbor node; we set it to parent until not sharing
	   of one parent between child and neighbor node is detected */
	znode *side_parent = coord->node;
	reiser4_tree *tree = znode_get_tree(child);
	znode *neighbor = NULL;

	assert("umka-245", coord != NULL);
	assert("umka-246", handle != NULL);
	assert("umka-247", child != NULL);
	assert("umka-303", tree != NULL);

	init_lh(handle);
	write_lock_tree(tree);
	ret = far_next_coord(coord, handle, flags);

	if (ret) {
		if (ret != -ENOENT) {
			write_unlock_tree(tree);
			return ret;
		}
	} else {
		item_plugin *iplug;

		if (handle->node != NULL) {
			(*nr_locked)++;
			side_parent = handle->node;
		}

		/* does coord object points to internal item? We do not
		   support sibling pointers between znode for formatted and
		   unformatted nodes and return -E_NO_NEIGHBOR in that case. */
		iplug = item_plugin_by_coord(coord);
		if (!item_is_internal(coord)) {
			link_znodes(child, NULL, to_left);
			write_unlock_tree(tree);
			/* we know there can't be formatted neighbor */
			return RETERR(-E_NO_NEIGHBOR);
		}
		write_unlock_tree(tree);

		iplug->s.internal.down_link(coord, NULL, &da);

		if (flags & GN_NO_ALLOC) {
			neighbor = zlook(tree, &da);
		} else {
			neighbor =
			    zget(tree, &da, side_parent, level,
				 reiser4_ctx_gfp_mask_get());
		}

		if (IS_ERR(neighbor)) {
			ret = PTR_ERR(neighbor);
			return ret;
		}

		if (neighbor)
			/* update delimiting keys */
			set_child_delimiting_keys(coord->node, coord, neighbor);

		write_lock_tree(tree);
	}

	if (likely(neighbor == NULL ||
		   (znode_get_level(child) == znode_get_level(neighbor)
		    && child != neighbor)))
		link_znodes(child, neighbor, to_left);
	else {
		warning("nikita-3532",
			"Sibling nodes on the different levels: %i != %i\n",
			znode_get_level(child), znode_get_level(neighbor));
		ret = RETERR(-EIO);
	}

	write_unlock_tree(tree);

	/* if GN_NO_ALLOC isn't set we keep reference to neighbor znode */
	if (neighbor != NULL && (flags & GN_NO_ALLOC))
		/* atomic_dec(&ZJNODE(neighbor)->x_count); */
		zput(neighbor);

	return ret;
}

/* This function is for establishing of one side relation. */
/* Audited by: umka (2002.06.14) */
static int connect_one_side(coord_t * coord, znode * node, int flags)
{
	coord_t local;
	lock_handle handle;
	int nr_locked;
	int ret;

	assert("umka-248", coord != NULL);
	assert("umka-249", node != NULL);

	coord_dup_nocheck(&local, coord);

	init_lh(&handle);

	ret =
	    renew_sibling_link(&local, &handle, node, znode_get_level(node),
			       flags | GN_NO_ALLOC, &nr_locked);

	if (handle.node != NULL) {
		/* complementary operations for zload() and lock() in far_next_coord() */
		zrelse(handle.node);
		longterm_unlock_znode(&handle);
	}

	/* we catch error codes which are not interesting for us because we
	   run renew_sibling_link() only for znode connection. */
	if (ret == -ENOENT || ret == -E_NO_NEIGHBOR)
		return 0;

	return ret;
}

/* if @child is not in `connected' state, performs hash searches for left and
   right neighbor nodes and establishes horizontal sibling links */
/* Audited by: umka (2002.06.14), umka (2002.06.15) */
int connect_znode(coord_t * parent_coord, znode * child)
{
	reiser4_tree *tree = znode_get_tree(child);
	int ret = 0;

	assert("zam-330", parent_coord != NULL);
	assert("zam-331", child != NULL);
	assert("zam-332", parent_coord->node != NULL);
	assert("umka-305", tree != NULL);

	/* it is trivial to `connect' root znode because it can't have
	   neighbors */
	if (znode_above_root(parent_coord->node)) {
		child->left = NULL;
		child->right = NULL;
		ZF_SET(child, JNODE_LEFT_CONNECTED);
		ZF_SET(child, JNODE_RIGHT_CONNECTED);

		ON_DEBUG(child->left_version =
			 atomic_inc_return(&delim_key_version);
			 child->right_version =
			 atomic_inc_return(&delim_key_version););

		return 0;
	}

	/* load parent node */
	coord_clear_iplug(parent_coord);
	ret = zload(parent_coord->node);

	if (ret != 0)
		return ret;

	/* protect `connected' state check by tree_lock */
	read_lock_tree(tree);

	if (!znode_is_right_connected(child)) {
		read_unlock_tree(tree);
		/* connect right (default is right) */
		ret = connect_one_side(parent_coord, child, GN_NO_ALLOC);
		if (ret)
			goto zrelse_and_ret;

		read_lock_tree(tree);
	}

	ret = znode_is_left_connected(child);

	read_unlock_tree(tree);

	if (!ret) {
		ret =
		    connect_one_side(parent_coord, child,
				     GN_NO_ALLOC | GN_GO_LEFT);
	} else
		ret = 0;

      zrelse_and_ret:
	zrelse(parent_coord->node);

	return ret;
}

/* this function is like renew_sibling_link() but allocates neighbor node if
   it doesn't exist and `connects' it. It may require making two steps in
   horizontal direction, first one for neighbor node finding/allocation,
   second one is for finding neighbor of neighbor to connect freshly allocated
   znode. */
/* Audited by: umka (2002.06.14), umka (2002.06.15) */
static int
renew_neighbor(coord_t * coord, znode * node, tree_level level, int flags)
{
	coord_t local;
	lock_handle empty[2];
	reiser4_tree *tree = znode_get_tree(node);
	znode *neighbor = NULL;
	int nr_locked = 0;
	int ret;

	assert("umka-250", coord != NULL);
	assert("umka-251", node != NULL);
	assert("umka-307", tree != NULL);
	assert("umka-308", level <= tree->height);

	/* umka (2002.06.14)
	   Here probably should be a check for given "level" validness.
	   Something like assert("xxx-yyy", level < REAL_MAX_ZTREE_HEIGHT);
	 */

	coord_dup(&local, coord);

	ret =
	    renew_sibling_link(&local, &empty[0], node, level,
			       flags & ~GN_NO_ALLOC, &nr_locked);
	if (ret)
		goto out;

	/* tree lock is not needed here because we keep parent node(s) locked
	   and reference to neighbor znode incremented */
	neighbor = (flags & GN_GO_LEFT) ? node->left : node->right;

	read_lock_tree(tree);
	ret = znode_is_connected(neighbor);
	read_unlock_tree(tree);
	if (ret) {
		ret = 0;
		goto out;
	}

	ret =
	    renew_sibling_link(&local, &empty[nr_locked], neighbor, level,
			       flags | GN_NO_ALLOC, &nr_locked);
	/* second renew_sibling_link() call is used for znode connection only,
	   so we can live with these errors */
	if (-ENOENT == ret || -E_NO_NEIGHBOR == ret)
		ret = 0;

      out:

	for (--nr_locked; nr_locked >= 0; --nr_locked) {
		zrelse(empty[nr_locked].node);
		longterm_unlock_znode(&empty[nr_locked]);
	}

	if (neighbor != NULL)
		/* decrement znode reference counter without actually
		   releasing it. */
		atomic_dec(&ZJNODE(neighbor)->x_count);

	return ret;
}

/*
   reiser4_get_neighbor() -- lock node's neighbor.

   reiser4_get_neighbor() locks node's neighbor (left or right one, depends on
   given parameter) using sibling link to it. If sibling link is not available
   (i.e. neighbor znode is not in cache) and flags allow read blocks, we go one
   level up for information about neighbor's disk address. We lock node's
   parent, if it is common parent for both 'node' and its neighbor, neighbor's
   disk address is in next (to left or to right) down link from link that points
   to original node. If not, we need to lock parent's neighbor, read its content
   and take first(last) downlink with neighbor's disk address.  That locking
   could be done by using sibling link and lock_neighbor() function, if sibling
   link exists. In another case we have to go level up again until we find
   common parent or valid sibling link. Then go down
   allocating/connecting/locking/reading nodes until neighbor of first one is
   locked.

   @neighbor:  result lock handle,
   @node: a node which we lock neighbor of,
   @lock_mode: lock mode {LM_READ, LM_WRITE},
   @flags: logical OR of {GN_*} (see description above) subset.

   @return: 0 if success, negative value if lock was impossible due to an error
   or lack of neighbor node.
*/

/* Audited by: umka (2002.06.14), umka (2002.06.15) */
int
reiser4_get_neighbor(lock_handle * neighbor, znode * node,
		     znode_lock_mode lock_mode, int flags)
{
	reiser4_tree *tree = znode_get_tree(node);
	lock_handle path[REAL_MAX_ZTREE_HEIGHT];

	coord_t coord;

	tree_level base_level;
	tree_level h = 0;
	int ret;

	assert("umka-252", tree != NULL);
	assert("umka-253", neighbor != NULL);
	assert("umka-254", node != NULL);

	base_level = znode_get_level(node);

	assert("umka-310", base_level <= tree->height);

	coord_init_zero(&coord);

      again:
	/* first, we try to use simple lock_neighbor() which requires sibling
	   link existence */
	read_lock_tree(tree);
	ret = lock_side_neighbor(neighbor, node, lock_mode, flags, 1);
	read_unlock_tree(tree);
	if (!ret) {
		/* load znode content if it was specified */
		if (flags & GN_LOAD_NEIGHBOR) {
			ret = zload(node);
			if (ret)
				longterm_unlock_znode(neighbor);
		}
		return ret;
	}

	/* only -ENOENT means we may look upward and try to connect
	   @node with its neighbor (if @flags allow us to do it) */
	if (ret != -ENOENT || !(flags & GN_CAN_USE_UPPER_LEVELS))
		return ret;

	/* before establishing of sibling link we lock parent node; it is
	   required by renew_neighbor() to work.  */
	init_lh(&path[0]);
	ret = reiser4_get_parent(&path[0], node, ZNODE_READ_LOCK);
	if (ret)
		return ret;
	if (znode_above_root(path[0].node)) {
		longterm_unlock_znode(&path[0]);
		return RETERR(-E_NO_NEIGHBOR);
	}

	while (1) {
		znode *child = (h == 0) ? node : path[h - 1].node;
		znode *parent = path[h].node;

		ret = zload(parent);
		if (ret)
			break;

		ret = find_child_ptr(parent, child, &coord);

		if (ret) {
			zrelse(parent);
			break;
		}

		/* try to establish missing sibling link */
		ret = renew_neighbor(&coord, child, h + base_level, flags);

		zrelse(parent);

		switch (ret) {
		case 0:
			/* unlocking of parent znode prevents simple
			   deadlock situation */
			done_lh(&path[h]);

			/* depend on tree level we stay on we repeat first
			   locking attempt ...  */
			if (h == 0)
				goto again;

			/* ... or repeat establishing of sibling link at
			   one level below. */
			--h;
			break;

		case -ENOENT:
			/* sibling link is not available -- we go
			   upward. */
			init_lh(&path[h + 1]);
			ret =
			    reiser4_get_parent(&path[h + 1], parent,
					       ZNODE_READ_LOCK);
			if (ret)
				goto fail;
			++h;
			if (znode_above_root(path[h].node)) {
				ret = RETERR(-E_NO_NEIGHBOR);
				goto fail;
			}
			break;

		case -E_DEADLOCK:
			/* there was lock request from hi-pri locker. if
			   it is possible we unlock last parent node and
			   re-lock it again. */
			for (; reiser4_check_deadlock(); h--) {
				done_lh(&path[h]);
				if (h == 0)
					goto fail;
			}

			break;

		default:	/* other errors. */
			goto fail;
		}
	}
      fail:
	ON_DEBUG(check_lock_node_data(node));
	ON_DEBUG(check_lock_data());

	/* unlock path */
	do {
		/* FIXME-Zam: when we get here from case -E_DEADLOCK's goto
		   fail; path[0] is already done_lh-ed, therefore
		   longterm_unlock_znode(&path[h]); is not applicable */
		done_lh(&path[h]);
		--h;
	} while (h + 1 != 0);

	return ret;
}

/* remove node from sibling list */
/* Audited by: umka (2002.06.14) */
void sibling_list_remove(znode * node)
{
	reiser4_tree *tree;

	tree = znode_get_tree(node);
	assert("umka-255", node != NULL);
	assert_rw_write_locked(&(tree->tree_lock));
	assert("nikita-3275", check_sibling_list(node));

	write_lock_dk(tree);
	if (znode_is_right_connected(node) && node->right != NULL &&
	    znode_is_left_connected(node) && node->left != NULL) {
		assert("zam-32245",
		       keyeq(znode_get_rd_key(node),
			     znode_get_ld_key(node->right)));
		znode_set_rd_key(node->left, znode_get_ld_key(node->right));
	}
	write_unlock_dk(tree);

	if (znode_is_right_connected(node) && node->right != NULL) {
		assert("zam-322", znode_is_left_connected(node->right));
		node->right->left = node->left;
		ON_DEBUG(node->right->left_version =
			 atomic_inc_return(&delim_key_version);
		    );
	}
	if (znode_is_left_connected(node) && node->left != NULL) {
		assert("zam-323", znode_is_right_connected(node->left));
		node->left->right = node->right;
		ON_DEBUG(node->left->right_version =
			 atomic_inc_return(&delim_key_version);
		    );
	}

	ZF_CLR(node, JNODE_LEFT_CONNECTED);
	ZF_CLR(node, JNODE_RIGHT_CONNECTED);
	ON_DEBUG(node->left = node->right = NULL;
		 node->left_version = atomic_inc_return(&delim_key_version);
		 node->right_version = atomic_inc_return(&delim_key_version););
	assert("nikita-3276", check_sibling_list(node));
}

/* disconnect node from sibling list */
void sibling_list_drop(znode * node)
{
	znode *right;
	znode *left;

	assert("nikita-2464", node != NULL);
	assert("nikita-3277", check_sibling_list(node));

	right = node->right;
	if (right != NULL) {
		assert("nikita-2465", znode_is_left_connected(right));
		right->left = NULL;
		ON_DEBUG(right->left_version =
			 atomic_inc_return(&delim_key_version);
		    );
	}
	left = node->left;
	if (left != NULL) {
		assert("zam-323", znode_is_right_connected(left));
		left->right = NULL;
		ON_DEBUG(left->right_version =
			 atomic_inc_return(&delim_key_version);
		    );
	}
	ZF_CLR(node, JNODE_LEFT_CONNECTED);
	ZF_CLR(node, JNODE_RIGHT_CONNECTED);
	ON_DEBUG(node->left = node->right = NULL;
		 node->left_version = atomic_inc_return(&delim_key_version);
		 node->right_version = atomic_inc_return(&delim_key_version););
}

/* Insert new node into sibling list. Regular balancing inserts new node
   after (at right side) existing and locked node (@before), except one case
   of adding new tree root node. @before should be NULL in that case. */
void sibling_list_insert_nolock(znode * new, znode * before)
{
	assert("zam-334", new != NULL);
	assert("nikita-3298", !znode_is_left_connected(new));
	assert("nikita-3299", !znode_is_right_connected(new));
	assert("nikita-3300", new->left == NULL);
	assert("nikita-3301", new->right == NULL);
	assert("nikita-3278", check_sibling_list(new));
	assert("nikita-3279", check_sibling_list(before));

	if (before != NULL) {
		assert("zam-333", znode_is_connected(before));
		new->right = before->right;
		new->left = before;
		ON_DEBUG(new->right_version =
			 atomic_inc_return(&delim_key_version);
			 new->left_version =
			 atomic_inc_return(&delim_key_version););
		if (before->right != NULL) {
			before->right->left = new;
			ON_DEBUG(before->right->left_version =
				 atomic_inc_return(&delim_key_version);
			    );
		}
		before->right = new;
		ON_DEBUG(before->right_version =
			 atomic_inc_return(&delim_key_version);
		    );
	} else {
		new->right = NULL;
		new->left = NULL;
		ON_DEBUG(new->right_version =
			 atomic_inc_return(&delim_key_version);
			 new->left_version =
			 atomic_inc_return(&delim_key_version););
	}
	ZF_SET(new, JNODE_LEFT_CONNECTED);
	ZF_SET(new, JNODE_RIGHT_CONNECTED);
	assert("nikita-3280", check_sibling_list(new));
	assert("nikita-3281", check_sibling_list(before));
}

/*
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 80
   End:
*/
