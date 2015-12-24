/* Copyright 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

/* This file contains size/offset translators, modulators
   and other helper functions. */

#if !defined(__FS_REISER4_CLUSTER_H__)
#define __FS_REISER4_CLUSTER_H__

#include "../inode.h"

static inline int inode_cluster_shift(struct inode *inode)
{
	assert("edward-92", inode != NULL);
	assert("edward-93", reiser4_inode_data(inode) != NULL);

	return inode_cluster_plugin(inode)->shift;
}

static inline unsigned cluster_nrpages_shift(struct inode *inode)
{
	return inode_cluster_shift(inode) - PAGE_CACHE_SHIFT;
}

/* cluster size in page units */
static inline unsigned cluster_nrpages(struct inode *inode)
{
	return 1U << cluster_nrpages_shift(inode);
}

static inline size_t inode_cluster_size(struct inode *inode)
{
	assert("edward-96", inode != NULL);

	return 1U << inode_cluster_shift(inode);
}

static inline cloff_t pg_to_clust(pgoff_t idx, struct inode *inode)
{
	return idx >> cluster_nrpages_shift(inode);
}

static inline pgoff_t clust_to_pg(cloff_t idx, struct inode *inode)
{
	return idx << cluster_nrpages_shift(inode);
}

static inline pgoff_t pg_to_clust_to_pg(pgoff_t idx, struct inode *inode)
{
	return clust_to_pg(pg_to_clust(idx, inode), inode);
}

static inline pgoff_t off_to_pg(loff_t off)
{
	return (off >> PAGE_CACHE_SHIFT);
}

static inline loff_t pg_to_off(pgoff_t idx)
{
	return ((loff_t) (idx) << PAGE_CACHE_SHIFT);
}

static inline cloff_t off_to_clust(loff_t off, struct inode *inode)
{
	return off >> inode_cluster_shift(inode);
}

static inline loff_t clust_to_off(cloff_t idx, struct inode *inode)
{
	return (loff_t) idx << inode_cluster_shift(inode);
}

static inline loff_t off_to_clust_to_off(loff_t off, struct inode *inode)
{
	return clust_to_off(off_to_clust(off, inode), inode);
}

static inline pgoff_t off_to_clust_to_pg(loff_t off, struct inode *inode)
{
	return clust_to_pg(off_to_clust(off, inode), inode);
}

static inline unsigned off_to_pgoff(loff_t off)
{
	return off & (PAGE_CACHE_SIZE - 1);
}

static inline unsigned off_to_cloff(loff_t off, struct inode *inode)
{
	return off & ((loff_t) (inode_cluster_size(inode)) - 1);
}

static inline  pgoff_t offset_in_clust(struct page *page)
{
	assert("edward-1488", page != NULL);
	assert("edward-1489", page->mapping != NULL);

	return page_index(page) & ((cluster_nrpages(page->mapping->host)) - 1);
}

static inline int first_page_in_cluster(struct page *page)
{
	return offset_in_clust(page) == 0;
}

static inline int last_page_in_cluster(struct page *page)
{
	return offset_in_clust(page) ==
		cluster_nrpages(page->mapping->host) - 1;
}

static inline unsigned
pg_to_off_to_cloff(unsigned long idx, struct inode *inode)
{
	return off_to_cloff(pg_to_off(idx), inode);
}

/*********************** Size translators **************************/

/* Translate linear size.
 * New units are (1 << @blk_shift) times larger, then old ones.
 * In other words, calculate number of logical blocks, occupied
 * by @count elements
 */
static inline unsigned long size_in_blocks(loff_t count, unsigned blkbits)
{
	return (count + (1UL << blkbits) - 1) >> blkbits;
}

/* size in pages */
static inline pgoff_t size_in_pages(loff_t size)
{
	return size_in_blocks(size, PAGE_CACHE_SHIFT);
}

/* size in logical clusters */
static inline cloff_t size_in_lc(loff_t size, struct inode *inode)
{
	return size_in_blocks(size, inode_cluster_shift(inode));
}

/* size in pages to the size in page clusters */
static inline cloff_t sp_to_spcl(pgoff_t size, struct inode *inode)
{
	return size_in_blocks(size, cluster_nrpages_shift(inode));
}

/*********************** Size modulators ***************************/

/*
  Modulate linear size by nominated block size and offset.

  The "finite" function (which is zero almost everywhere).
  How much is a height of the figure at a position @pos,
  when trying to construct rectangle of height (1 << @blkbits),
  and square @size.

  ******
  *******
  *******
  *******
  ----------> pos
*/
static inline unsigned __mbb(loff_t size, unsigned long pos, int blkbits)
{
	unsigned end = size >> blkbits;
	if (pos < end)
		return 1U << blkbits;
	if (unlikely(pos > end))
		return 0;
	return size & ~(~0ull << blkbits);
}

/* the same as above, but block size is page size */
static inline unsigned __mbp(loff_t size, pgoff_t pos)
{
	return __mbb(size, pos, PAGE_CACHE_SHIFT);
}

/* number of file's bytes in the nominated logical cluster */
static inline unsigned lbytes(cloff_t index, struct inode *inode)
{
	return __mbb(i_size_read(inode), index, inode_cluster_shift(inode));
}

/* number of file's bytes in the nominated page */
static inline unsigned pbytes(pgoff_t index, struct inode *inode)
{
	return __mbp(i_size_read(inode), index);
}

/**
 * number of pages occuped by @win->count bytes starting from
 * @win->off at logical cluster defined by @win. This is exactly
 * a number of pages to be modified and dirtied in any cluster operation.
 */
static inline pgoff_t win_count_to_nrpages(struct reiser4_slide * win)
{
	return ((win->off + win->count +
		 (1UL << PAGE_CACHE_SHIFT) - 1) >> PAGE_CACHE_SHIFT) -
		off_to_pg(win->off);
}

/* return true, if logical cluster is not occupied by the file */
static inline int new_logical_cluster(struct cluster_handle *clust,
				      struct inode *inode)
{
	return clust_to_off(clust->index, inode) >= i_size_read(inode);
}

/* return true, if pages @p1 and @p2 are of the same page cluster */
static inline int same_page_cluster(struct page *p1, struct page *p2)
{
	assert("edward-1490", p1 != NULL);
	assert("edward-1491", p2 != NULL);
	assert("edward-1492", p1->mapping != NULL);
	assert("edward-1493", p2->mapping != NULL);

	return (pg_to_clust(page_index(p1), p1->mapping->host) ==
		pg_to_clust(page_index(p2), p2->mapping->host));
}

static inline int cluster_is_complete(struct cluster_handle *clust,
				      struct inode *inode)
{
	return clust->tc.lsize == inode_cluster_size(inode);
}

static inline void reiser4_slide_init(struct reiser4_slide *win)
{
	assert("edward-1084", win != NULL);
	memset(win, 0, sizeof *win);
}

static inline tfm_action
cluster_get_tfm_act(struct tfm_cluster *tc)
{
	assert("edward-1356", tc != NULL);
	return tc->act;
}

static inline void
cluster_set_tfm_act(struct tfm_cluster *tc, tfm_action act)
{
	assert("edward-1356", tc != NULL);
	tc->act = act;
}

static inline void cluster_init_act(struct cluster_handle *clust,
				    tfm_action act,
				    struct reiser4_slide *window)
{
	assert("edward-84", clust != NULL);
	memset(clust, 0, sizeof *clust);
	cluster_set_tfm_act(&clust->tc, act);
	clust->dstat = INVAL_DISK_CLUSTER;
	clust->win = window;
}

static inline void cluster_init_read(struct cluster_handle *clust,
				     struct reiser4_slide *window)
{
	cluster_init_act(clust, TFMA_READ, window);
}

static inline void cluster_init_write(struct cluster_handle *clust,
				      struct reiser4_slide *window)
{
	cluster_init_act(clust, TFMA_WRITE, window);
}

/* true if @p1 and @p2 are items of the same disk cluster */
static inline int same_disk_cluster(const coord_t *p1, const coord_t *p2)
{
	/* drop this if you have other items to aggregate */
	assert("edward-1494", item_id_by_coord(p1) == CTAIL_ID);

	return item_plugin_by_coord(p1)->b.mergeable(p1, p2);
}

static inline int dclust_get_extension_dsize(hint_t *hint)
{
	return hint->ext_coord.extension.ctail.dsize;
}

static inline void dclust_set_extension_dsize(hint_t *hint, int dsize)
{
	hint->ext_coord.extension.ctail.dsize = dsize;
}

static inline int dclust_get_extension_shift(hint_t *hint)
{
	return hint->ext_coord.extension.ctail.shift;
}

static inline int dclust_get_extension_ncount(hint_t *hint)
{
	return hint->ext_coord.extension.ctail.ncount;
}

static inline void dclust_inc_extension_ncount(hint_t *hint)
{
	hint->ext_coord.extension.ctail.ncount++;
}

static inline void dclust_init_extension(hint_t *hint)
{
	memset(&hint->ext_coord.extension.ctail, 0,
	       sizeof(hint->ext_coord.extension.ctail));
}

static inline int hint_is_unprepped_dclust(hint_t *hint)
{
	assert("edward-1451", hint_is_valid(hint));
	return dclust_get_extension_shift(hint) == (int)UCTAIL_SHIFT;
}

static inline void coord_set_between_clusters(coord_t *coord)
{
#if REISER4_DEBUG
	int result;
	result = zload(coord->node);
	assert("edward-1296", !result);
#endif
	if (!coord_is_between_items(coord)) {
		coord->between = AFTER_ITEM;
		coord->unit_pos = 0;
	}
#if REISER4_DEBUG
	zrelse(coord->node);
#endif
}

int reiser4_inflate_cluster(struct cluster_handle *, struct inode *);
int find_disk_cluster(struct cluster_handle *, struct inode *, int read,
		      znode_lock_mode mode);
int checkout_logical_cluster(struct cluster_handle *, jnode * , struct inode *);
int reiser4_deflate_cluster(struct cluster_handle *, struct inode *);
void truncate_complete_page_cluster(struct inode *inode, cloff_t start,
					 int even_cows);
void invalidate_hint_cluster(struct cluster_handle *clust);
int get_disk_cluster_locked(struct cluster_handle *clust, struct inode *inode,
			    znode_lock_mode lock_mode);
void reset_cluster_params(struct cluster_handle *clust);
int set_cluster_by_page(struct cluster_handle *clust, struct page *page,
			int count);
int prepare_page_cluster(struct inode *inode, struct cluster_handle *clust,
			 rw_op rw);
void __put_page_cluster(int from, int count, struct page **pages,
			struct inode *inode);
void put_page_cluster(struct cluster_handle *clust,
		      struct inode *inode, rw_op rw);
void put_cluster_handle(struct cluster_handle *clust);
int grab_tfm_stream(struct inode *inode, struct tfm_cluster *tc,
		    tfm_stream_id id);
int tfm_cluster_is_uptodate(struct tfm_cluster *tc);
void tfm_cluster_set_uptodate(struct tfm_cluster *tc);
void tfm_cluster_clr_uptodate(struct tfm_cluster *tc);

/* move cluster handle to the target position
   specified by the page of index @pgidx */
static inline void move_cluster_forward(struct cluster_handle *clust,
					struct inode *inode,
					pgoff_t pgidx)
{
	assert("edward-1297", clust != NULL);
	assert("edward-1298", inode != NULL);

	reset_cluster_params(clust);
	if (clust->index_valid &&
	    /* Hole in the indices. Hint became invalid and can not be
	       used by find_cluster_item() even if seal/node versions
	       will coincide */
	    pg_to_clust(pgidx, inode) != clust->index + 1) {
		reiser4_unset_hint(clust->hint);
		invalidate_hint_cluster(clust);
	}
	clust->index = pg_to_clust(pgidx, inode);
	clust->index_valid = 1;
}

static inline int alloc_clust_pages(struct cluster_handle *clust,
				    struct inode *inode)
{
	assert("edward-791", clust != NULL);
	assert("edward-792", inode != NULL);
	clust->pages =
		kmalloc(sizeof(*clust->pages) << inode_cluster_shift(inode),
			reiser4_ctx_gfp_mask_get());
	if (!clust->pages)
		return -ENOMEM;
	return 0;
}

static inline void free_clust_pages(struct cluster_handle *clust)
{
	kfree(clust->pages);
}

#endif				/* __FS_REISER4_CLUSTER_H__ */

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
