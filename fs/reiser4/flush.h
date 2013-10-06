/* Copyright 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

/* DECLARATIONS: */

#if !defined(__REISER4_FLUSH_H__)
#define __REISER4_FLUSH_H__

#include "plugin/cluster.h"

/* The flush_scan data structure maintains the state of an in-progress
   flush-scan on a single level of the tree. A flush-scan is used for counting
   the number of adjacent nodes to flush, which is used to determine whether we
   should relocate, and it is also used to find a starting point for flush. A
   flush-scan object can scan in both right and left directions via the
   scan_left() and scan_right() interfaces. The right- and left-variations are
   similar but perform different functions. When scanning left we (optionally
   perform rapid scanning and then) longterm-lock the endpoint node. When
   scanning right we are simply counting the number of adjacent, dirty nodes. */
struct flush_scan {

	/* The current number of nodes scanned on this level. */
	unsigned count;

	/* There may be a maximum number of nodes for a scan on any single
	   level. When going leftward, max_count is determined by
	   FLUSH_SCAN_MAXNODES (see reiser4.h) */
	unsigned max_count;

	/* Direction: Set to one of the sideof enumeration:
	   { LEFT_SIDE, RIGHT_SIDE }. */
	sideof direction;

	/* Initially @stop is set to false then set true once some condition
	   stops the search (e.g., we found a clean node before reaching
	   max_count or we found a node belonging to another atom). */
	int stop;

	/* The current scan position.  If @node is non-NULL then its reference
	   count has been incremented to reflect this reference. */
	jnode *node;

	/* A handle for zload/zrelse of current scan position node. */
	load_count node_load;

	/* During left-scan, if the final position (a.k.a. endpoint node) is
	   formatted the node is locked using this lock handle. The endpoint
	   needs to be locked for transfer to the flush_position object after
	   scanning finishes. */
	lock_handle node_lock;

	/* When the position is unformatted, its parent, coordinate, and parent
	   zload/zrelse handle. */
	lock_handle parent_lock;
	coord_t parent_coord;
	load_count parent_load;

	/* The block allocator preceder hint.  Sometimes flush_scan determines
	   what the preceder is and if so it sets it here, after which it is
	   copied into the flush_position. Otherwise, the preceder is computed
	   later. */
	reiser4_block_nr preceder_blk;
};

struct convert_item_info {
	dc_item_stat d_cur;	/* disk cluster state of the current item */
	dc_item_stat d_next;	/* disk cluster state of the next slum item */
	int cluster_shift;      /* disk cluster shift */
	flow_t flow;            /* disk cluster data */
};

struct convert_info {
	int count;		/* for squalloc terminating */
	item_plugin *iplug;	/* current item plugin */
	struct convert_item_info *itm;	/* current item info */
	struct cluster_handle clust;	/* transform cluster */
};

typedef enum flush_position_state {
	POS_INVALID,		/* Invalid or stopped pos, do not continue slum
				 * processing */
	POS_ON_LEAF,		/* pos points to already prepped, locked
				 * formatted node at leaf level */
	POS_ON_EPOINT,		/* pos keeps a lock on twig level, "coord" field
				 * is used to traverse unformatted nodes */
	POS_TO_LEAF,		/* pos is being moved to leaf level */
	POS_TO_TWIG,		/* pos is being moved to twig level */
	POS_END_OF_TWIG,	/* special case of POS_ON_TWIG, when coord is
				 * after rightmost unit of the current twig */
	POS_ON_INTERNAL		/* same as POS_ON_LEAF, but points to internal
				 * node */
} flushpos_state_t;

/* An encapsulation of the current flush point and all the parameters that are
   passed through the entire squeeze-and-allocate stage of the flush routine.
   A single flush_position object is constructed after left- and right-scanning
   finishes. */
struct flush_position {
	flushpos_state_t state;

	coord_t coord;		/* coord to traverse unformatted nodes */
	lock_handle lock;	/* current lock we hold */
	load_count load;	/* load status for current locked formatted node
				*/
	jnode *child;		/* for passing a reference to unformatted child
				 * across pos state changes */

	reiser4_blocknr_hint preceder;	/* The flush 'hint' state. */
	int leaf_relocate;	/* True if enough leaf-level nodes were
				 * found to suggest a relocate policy. */
	int alloc_cnt;		/* The number of nodes allocated during squeeze
				   and allococate. */
	int prep_or_free_cnt;	/* The number of nodes prepared for write
				   (allocate) or squeezed and freed. */
	flush_queue_t *fq;
	long *nr_written;	/* number of nodes submitted to disk */
	int flags;		/* a copy of jnode_flush flags argument */

	znode *prev_twig;	/* previous parent pointer value, used to catch
				 * processing of new twig node */
	struct convert_info *sq;	/* convert info */

	unsigned long pos_in_unit;	/* for extents only. Position
					   within an extent unit of first
					   jnode of slum */
	long nr_to_write;	/* number of unformatted nodes to handle on
				   flush */
};

static inline int item_convert_count(flush_pos_t *pos)
{
	return pos->sq->count;
}
static inline void inc_item_convert_count(flush_pos_t *pos)
{
	pos->sq->count++;
}
static inline void set_item_convert_count(flush_pos_t *pos, int count)
{
	pos->sq->count = count;
}
static inline item_plugin *item_convert_plug(flush_pos_t *pos)
{
	return pos->sq->iplug;
}

static inline struct convert_info *convert_data(flush_pos_t *pos)
{
	return pos->sq;
}

static inline struct convert_item_info *item_convert_data(flush_pos_t *pos)
{
	assert("edward-955", convert_data(pos));
	return pos->sq->itm;
}

static inline struct tfm_cluster *tfm_cluster_sq(flush_pos_t *pos)
{
	return &pos->sq->clust.tc;
}

static inline struct tfm_stream *tfm_stream_sq(flush_pos_t *pos,
						tfm_stream_id id)
{
	assert("edward-854", pos->sq != NULL);
	return get_tfm_stream(tfm_cluster_sq(pos), id);
}

static inline int chaining_data_present(flush_pos_t *pos)
{
	return convert_data(pos) && item_convert_data(pos);
}

/* Returns true if next node contains next item of the disk cluster
   so item convert data should be moved to the right slum neighbor.
*/
static inline int should_chain_next_node(flush_pos_t *pos)
{
	int result = 0;

	assert("edward-1007", chaining_data_present(pos));

	switch (item_convert_data(pos)->d_next) {
	case DC_CHAINED_ITEM:
		result = 1;
		break;
	case DC_AFTER_CLUSTER:
		break;
	default:
		impossible("edward-1009", "bad state of next slum item");
	}
	return result;
}

/* update item state in a disk cluster to assign conversion mode */
static inline void
move_chaining_data(flush_pos_t *pos, int this_node/* where is next item */)
{

	assert("edward-1010", chaining_data_present(pos));

	if (this_node == 0) {
		/* next item is on the right neighbor */
		assert("edward-1011",
		       item_convert_data(pos)->d_cur == DC_FIRST_ITEM ||
		       item_convert_data(pos)->d_cur == DC_CHAINED_ITEM);
		assert("edward-1012",
		       item_convert_data(pos)->d_next == DC_CHAINED_ITEM);

		item_convert_data(pos)->d_cur = DC_CHAINED_ITEM;
		item_convert_data(pos)->d_next = DC_INVALID_STATE;
	} else {
		/* next item is on the same node */
		assert("edward-1013",
		       item_convert_data(pos)->d_cur == DC_FIRST_ITEM ||
		       item_convert_data(pos)->d_cur == DC_CHAINED_ITEM);
		assert("edward-1227",
		       item_convert_data(pos)->d_next == DC_AFTER_CLUSTER ||
		       item_convert_data(pos)->d_next == DC_INVALID_STATE);

		item_convert_data(pos)->d_cur = DC_AFTER_CLUSTER;
		item_convert_data(pos)->d_next = DC_INVALID_STATE;
	}
}

static inline int should_convert_node(flush_pos_t *pos, znode * node)
{
	return znode_convertible(node);
}

/* true if there is attached convert item info */
static inline int should_convert_next_node(flush_pos_t *pos)
{
	return convert_data(pos) && item_convert_data(pos);
}

#define SQUALLOC_THRESHOLD 256

static inline int should_terminate_squalloc(flush_pos_t *pos)
{
	return convert_data(pos) &&
	    !item_convert_data(pos) &&
	    item_convert_count(pos) >= SQUALLOC_THRESHOLD;
}

#if 1
#define check_convert_info(pos)						\
do {							        	\
	if (unlikely(should_convert_next_node(pos))) {			\
		warning("edward-1006", "unprocessed chained data");	\
		printk("d_cur = %d, d_next = %d, flow.len = %llu\n",	\
		       item_convert_data(pos)->d_cur,			\
		       item_convert_data(pos)->d_next,			\
		       item_convert_data(pos)->flow.length);		\
	}								\
} while (0)
#else
#define check_convert_info(pos)
#endif /* REISER4_DEBUG */

void free_convert_data(flush_pos_t *pos);
/* used in extent.c */
int scan_set_current(flush_scan * scan, jnode * node, unsigned add_size,
		     const coord_t *parent);
int reiser4_scan_finished(flush_scan * scan);
int reiser4_scanning_left(flush_scan * scan);
int reiser4_scan_goto(flush_scan * scan, jnode * tonode);
txn_atom *atom_locked_by_fq(flush_queue_t *fq);
int reiser4_alloc_extent(flush_pos_t *flush_pos);
squeeze_result squalloc_extent(znode *left, const coord_t *, flush_pos_t *,
			       reiser4_key *stop_key);
extern int reiser4_init_fqs(void);
extern void reiser4_done_fqs(void);

#if REISER4_DEBUG

extern void reiser4_check_fq(const txn_atom *atom);
extern atomic_t flush_cnt;

#define check_preceder(blk) \
assert("nikita-2588", blk < reiser4_block_count(reiser4_get_current_sb()));
extern void check_pos(flush_pos_t *pos);
#else
#define check_preceder(b) noop
#define check_pos(pos) noop
#endif

/* __REISER4_FLUSH_H__ */
#endif

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
