/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

#include "../../inode.h"
#include "../../super.h"
#include "../../page_cache.h"
#include "../../carry.h"
#include "../../safe_link.h"
#include "../../vfs_ops.h"

#include <linux/writeback.h>

/* this file contains:
   tail2extent and extent2tail */

/* exclusive access to a file is acquired when file state changes: tail2extent, empty2tail, extent2tail, etc */
void get_exclusive_access(struct unix_file_info * uf_info)
{
	assert("nikita-3028", reiser4_schedulable());
	assert("nikita-3047", LOCK_CNT_NIL(inode_sem_w));
	assert("nikita-3048", LOCK_CNT_NIL(inode_sem_r));
	/*
	 * "deadlock avoidance": sometimes we commit a transaction under
	 * rw-semaphore on a file. Such commit can deadlock with another
	 * thread that captured some block (hence preventing atom from being
	 * committed) and waits on rw-semaphore.
	 */
	reiser4_txn_restart_current();
	LOCK_CNT_INC(inode_sem_w);
	down_write(&uf_info->latch);
	uf_info->exclusive_use = 1;
	assert("vs-1713", uf_info->ea_owner == NULL);
	assert("vs-1713", atomic_read(&uf_info->nr_neas) == 0);
	ON_DEBUG(uf_info->ea_owner = current);
}

void drop_exclusive_access(struct unix_file_info * uf_info)
{
	assert("vs-1714", uf_info->ea_owner == current);
	assert("vs-1715", atomic_read(&uf_info->nr_neas) == 0);
	ON_DEBUG(uf_info->ea_owner = NULL);
	uf_info->exclusive_use = 0;
	up_write(&uf_info->latch);
	assert("nikita-3049", LOCK_CNT_NIL(inode_sem_r));
	assert("nikita-3049", LOCK_CNT_GTZ(inode_sem_w));
	LOCK_CNT_DEC(inode_sem_w);
	reiser4_txn_restart_current();
}

/**
 * nea_grabbed - do something when file semaphore is down_read-ed
 * @uf_info:
 *
 * This is called when nonexclisive access is obtained on file. All it does is
 * for debugging purposes.
 */
static void nea_grabbed(struct unix_file_info *uf_info)
{
#if REISER4_DEBUG
	LOCK_CNT_INC(inode_sem_r);
	assert("vs-1716", uf_info->ea_owner == NULL);
	atomic_inc(&uf_info->nr_neas);
	uf_info->last_reader = current;
#endif
}

/**
 * get_nonexclusive_access - get nonexclusive access to a file
 * @uf_info: unix file specific part of inode to obtain access to
 *
 * Nonexclusive access is obtained on a file before read, write, readpage.
 */
void get_nonexclusive_access(struct unix_file_info *uf_info)
{
	assert("nikita-3029", reiser4_schedulable());
	assert("nikita-3361", get_current_context()->trans->atom == NULL);

	down_read(&uf_info->latch);
	nea_grabbed(uf_info);
}

/**
 * try_to_get_nonexclusive_access - try to get nonexclusive access to a file
 * @uf_info: unix file specific part of inode to obtain access to
 *
 * Non-blocking version of nonexclusive access obtaining.
 */
int try_to_get_nonexclusive_access(struct unix_file_info *uf_info)
{
	int result;

	result = down_read_trylock(&uf_info->latch);
	if (result)
		nea_grabbed(uf_info);
	return result;
}

void drop_nonexclusive_access(struct unix_file_info * uf_info)
{
	assert("vs-1718", uf_info->ea_owner == NULL);
	assert("vs-1719", atomic_read(&uf_info->nr_neas) > 0);
	ON_DEBUG(atomic_dec(&uf_info->nr_neas));

	up_read(&uf_info->latch);

	LOCK_CNT_DEC(inode_sem_r);
	reiser4_txn_restart_current();
}

/* part of tail2extent. Cut all items covering @count bytes starting from
   @offset */
/* Audited by: green(2002.06.15) */
static int cut_formatting_items(struct inode *inode, loff_t offset, int count)
{
	reiser4_key from, to;

	/* AUDIT: How about putting an assertion here, what would check
	   all provided range is covered by tail items only? */
	/* key of first byte in the range to be cut  */
	inode_file_plugin(inode)->key_by_inode(inode, offset, &from);

	/* key of last byte in that range */
	to = from;
	set_key_offset(&to, (__u64) (offset + count - 1));

	/* cut everything between those keys */
	return reiser4_cut_tree(reiser4_tree_by_inode(inode), &from, &to,
				inode, 0);
}

static void release_all_pages(struct page **pages, unsigned nr_pages)
{
	unsigned i;

	for (i = 0; i < nr_pages; i++) {
		if (pages[i] == NULL) {
#if REISER4_DEBUG
			unsigned j;
			for (j = i + 1; j < nr_pages; j++)
				assert("vs-1620", pages[j] == NULL);
#endif
			break;
		}
		page_cache_release(pages[i]);
		pages[i] = NULL;
	}
}

/* part of tail2extent. replace tail items with extent one. Content of tail
   items (@count bytes) being cut are copied already into
   pages. extent_writepage method is called to create extents corresponding to
   those pages */
static int replace(struct inode *inode, struct page **pages, unsigned nr_pages, int count)
{
	int result;
	unsigned i;
	STORE_COUNTERS;

	if (nr_pages == 0)
		return 0;

	assert("vs-596", pages[0]);

	/* cut copied items */
	result = cut_formatting_items(inode, page_offset(pages[0]), count);
	if (result)
		return result;

	CHECK_COUNTERS;

	/* put into tree replacement for just removed items: extent item, namely */
	for (i = 0; i < nr_pages; i++) {
		result = add_to_page_cache_lru(pages[i], inode->i_mapping,
					       pages[i]->index,
					       mapping_gfp_mask(inode->
								i_mapping));
		if (result)
			break;
		unlock_page(pages[i]);
		result = find_or_create_extent(pages[i]);
		if (result) {
			/*
			 * Unsuccess in critical place:
			 * tail has been removed,
			 * but extent hasn't been created
			 */
			warning("edward-1572",
			"Report the error code %i to developers. Run FSCK",
				result);
			break;
		}
		SetPageUptodate(pages[i]);
	}
	return result;
}

#define TAIL2EXTENT_PAGE_NUM 3	/* number of pages to fill before cutting tail
				 * items */

static int reserve_tail2extent_iteration(struct inode *inode)
{
	reiser4_block_nr unformatted_nodes;
	reiser4_tree *tree;

	tree = reiser4_tree_by_inode(inode);

	/* number of unformatted nodes which will be created */
	unformatted_nodes = TAIL2EXTENT_PAGE_NUM;

	/*
	 * space required for one iteration of extent->tail conversion:
	 *
	 *     1. kill N tail items
	 *
	 *     2. insert TAIL2EXTENT_PAGE_NUM unformatted nodes
	 *
	 *     3. insert TAIL2EXTENT_PAGE_NUM (worst-case single-block
	 *     extents) extent units.
	 *
	 *     4. drilling to the leaf level by coord_by_key()
	 *
	 *     5. possible update of stat-data
	 *
	 */
	grab_space_enable();
	return reiser4_grab_space
	    (2 * tree->height +
	     TAIL2EXTENT_PAGE_NUM +
	     TAIL2EXTENT_PAGE_NUM * estimate_one_insert_into_item(tree) +
	     1 + estimate_one_insert_item(tree) +
	     inode_file_plugin(inode)->estimate.update(inode), BA_CAN_COMMIT);
}

/* clear stat data's flag indicating that conversion is being converted */
static int complete_conversion(struct inode *inode)
{
	int result;

	grab_space_enable();
	result =
	    reiser4_grab_space(inode_file_plugin(inode)->estimate.update(inode),
			       BA_CAN_COMMIT);
	if (result == 0) {
		reiser4_inode_clr_flag(inode, REISER4_PART_MIXED);
		result = reiser4_update_sd(inode);
	}
	if (result)
		warning("vs-1696", "Failed to clear converting bit of %llu: %i",
			(unsigned long long)get_inode_oid(inode), result);
	return 0;
}

/**
 * find_start
 * @inode:
 * @id:
 * @offset:
 *
 * this is used by tail2extent and extent2tail to detect where previous
 * uncompleted conversion stopped
 */
static int find_start(struct inode *inode, reiser4_plugin_id id, __u64 *offset)
{
	int result;
	lock_handle lh;
	coord_t coord;
	struct unix_file_info *ufo;
	int found;
	reiser4_key key;

	ufo = unix_file_inode_data(inode);
	init_lh(&lh);
	result = 0;
	found = 0;
	inode_file_plugin(inode)->key_by_inode(inode, *offset, &key);
	do {
		init_lh(&lh);
		result = find_file_item_nohint(&coord, &lh, &key,
					       ZNODE_READ_LOCK, inode);

		if (result == CBK_COORD_FOUND) {
			if (coord.between == AT_UNIT) {
				/*coord_clear_iplug(&coord); */
				result = zload(coord.node);
				if (result == 0) {
					if (item_id_by_coord(&coord) == id)
						found = 1;
					else
						item_plugin_by_coord(&coord)->s.
						    file.append_key(&coord,
								    &key);
					zrelse(coord.node);
				}
			} else
				result = RETERR(-ENOENT);
		}
		done_lh(&lh);
	} while (result == 0 && !found);
	*offset = get_key_offset(&key);
	return result;
}

/**
 * tail2extent
 * @uf_info:
 *
 *
 */
int tail2extent(struct unix_file_info *uf_info)
{
	int result;
	reiser4_key key;	/* key of next byte to be moved to page */
	char *p_data;		/* data of page */
	unsigned page_off = 0,	/* offset within the page where to copy data */
	    count;		/* number of bytes of item which can be
				 * copied to page */
	struct page *pages[TAIL2EXTENT_PAGE_NUM];
	struct page *page;
	int done;		/* set to 1 when all file is read */
	char *item;
	int i;
	struct inode *inode;
	int first_iteration;
	int bytes;
	__u64 offset;

	assert("nikita-3362", ea_obtained(uf_info));
	inode = unix_file_info_to_inode(uf_info);
	assert("nikita-3412", !IS_RDONLY(inode));
	assert("vs-1649", uf_info->container != UF_CONTAINER_EXTENTS);
	assert("", !reiser4_inode_get_flag(inode, REISER4_PART_IN_CONV));

	offset = 0;
	first_iteration = 1;
	result = 0;
	if (reiser4_inode_get_flag(inode, REISER4_PART_MIXED)) {
		/*
		 * file is marked on disk as there was a conversion which did
		 * not complete due to either crash or some error. Find which
		 * offset tail conversion stopped at
		 */
		result = find_start(inode, FORMATTING_ID, &offset);
		if (result == -ENOENT) {
			/* no tail items found, everything is converted */
			uf_info->container = UF_CONTAINER_EXTENTS;
			complete_conversion(inode);
			return 0;
		} else if (result != 0)
			/* some other error */
			return result;
		first_iteration = 0;
	}

	reiser4_inode_set_flag(inode, REISER4_PART_IN_CONV);

	/* get key of first byte of a file */
	inode_file_plugin(inode)->key_by_inode(inode, offset, &key);

	done = 0;
	while (done == 0) {
		memset(pages, 0, sizeof(pages));
		result = reserve_tail2extent_iteration(inode);
		if (result != 0) {
			reiser4_inode_clr_flag(inode, REISER4_PART_IN_CONV);
			goto out;
		}
		if (first_iteration) {
			reiser4_inode_set_flag(inode, REISER4_PART_MIXED);
			reiser4_update_sd(inode);
			first_iteration = 0;
		}
		bytes = 0;
		for (i = 0; i < sizeof_array(pages) && done == 0; i++) {
			assert("vs-598",
			       (get_key_offset(&key) & ~PAGE_CACHE_MASK) == 0);
			page = alloc_page(reiser4_ctx_gfp_mask_get());
			if (!page) {
				result = RETERR(-ENOMEM);
				goto error;
			}

			page->index =
			    (unsigned long)(get_key_offset(&key) >>
					    PAGE_CACHE_SHIFT);
			/*
			 * usually when one is going to longterm lock znode (as
			 * find_file_item does, for instance) he must not hold
			 * locked pages. However, there is an exception for
			 * case tail2extent. Pages appearing here are not
			 * reachable to everyone else, they are clean, they do
			 * not have jnodes attached so keeping them locked do
			 * not risk deadlock appearance
			 */
			assert("vs-983", !PagePrivate(page));
			reiser4_invalidate_pages(inode->i_mapping, page->index,
						 1, 0);

			for (page_off = 0; page_off < PAGE_CACHE_SIZE;) {
				coord_t coord;
				lock_handle lh;

				/* get next item */
				/* FIXME: we might want to readahead here */
				init_lh(&lh);
				result =
				    find_file_item_nohint(&coord, &lh, &key,
							  ZNODE_READ_LOCK,
							  inode);
				if (result != CBK_COORD_FOUND) {
					/*
					 * error happened of not items of file
					 * were found
					 */
					done_lh(&lh);
					page_cache_release(page);
					goto error;
				}

				if (coord.between == AFTER_UNIT) {
					/*
					 * end of file is reached. Padd page
					 * with zeros
					 */
					done_lh(&lh);
					done = 1;
					p_data = kmap_atomic(page);
					memset(p_data + page_off, 0,
					       PAGE_CACHE_SIZE - page_off);
					kunmap_atomic(p_data);
					break;
				}

				result = zload(coord.node);
				if (result) {
					page_cache_release(page);
					done_lh(&lh);
					goto error;
				}
				assert("vs-856", coord.between == AT_UNIT);
				item = ((char *)item_body_by_coord(&coord)) +
					coord.unit_pos;

				/* how many bytes to copy */
				count =
				    item_length_by_coord(&coord) -
				    coord.unit_pos;
				/* limit length of copy to end of page */
				if (count > PAGE_CACHE_SIZE - page_off)
					count = PAGE_CACHE_SIZE - page_off;

				/*
				 * copy item (as much as will fit starting from
				 * the beginning of the item) into the page
				 */
				p_data = kmap_atomic(page);
				memcpy(p_data + page_off, item, count);
				kunmap_atomic(p_data);

				page_off += count;
				bytes += count;
				set_key_offset(&key,
					       get_key_offset(&key) + count);

				zrelse(coord.node);
				done_lh(&lh);
			} /* end of loop which fills one page by content of
			   * formatting items */

			if (page_off) {
				/* something was copied into page */
				pages[i] = page;
			} else {
				page_cache_release(page);
				assert("vs-1648", done == 1);
				break;
			}
		} /* end of loop through pages of one conversion iteration */

		if (i > 0) {
			result = replace(inode, pages, i, bytes);
			release_all_pages(pages, sizeof_array(pages));
			if (result)
				goto error;
			/*
			 * We have to drop exclusive access to avoid deadlock
			 * which may happen because called by reiser4_writepages
			 * capture_unix_file requires to get non-exclusive
			 * access to a file. It is safe to drop EA in the middle
			 * of tail2extent conversion because write_unix_file,
			 * setattr_unix_file(truncate), mmap_unix_file,
			 * release_unix_file(extent2tail) checks if conversion
			 * is not in progress (see comments before
			 * get_exclusive_access_careful().
			 * Other processes that acquire non-exclusive access
			 * (read_unix_file, reiser4_writepages, etc) should work
			 * on partially converted files.
			 */
			drop_exclusive_access(uf_info);
			/* throttle the conversion */
			reiser4_throttle_write(inode);
			get_exclusive_access(uf_info);

			/*
			 * nobody is allowed to complete conversion but a
			 * process which started it
			 */
			assert("", reiser4_inode_get_flag(inode,
							  REISER4_PART_MIXED));
		}
	}
	if (result == 0) {
		/* file is converted to extent items */
		reiser4_inode_clr_flag(inode, REISER4_PART_IN_CONV);
		assert("vs-1697", reiser4_inode_get_flag(inode,
							 REISER4_PART_MIXED));

		uf_info->container = UF_CONTAINER_EXTENTS;
		complete_conversion(inode);
	} else {
		/*
		 * conversion is not complete. Inode was already marked as
		 * REISER4_PART_MIXED and stat-data were updated at the first
		 * iteration of the loop above.
		 */
	error:
		release_all_pages(pages, sizeof_array(pages));
		reiser4_inode_clr_flag(inode, REISER4_PART_IN_CONV);
		warning("edward-1548", "Partial conversion of %llu: %i",
			(unsigned long long)get_inode_oid(inode), result);
	}

 out:
	/* this flag should be cleared, otherwise get_exclusive_access_careful()
	   will fall into infinite loop */
	assert("edward-1549", !reiser4_inode_get_flag(inode,
						      REISER4_PART_IN_CONV));
	return result;
}

static int reserve_extent2tail_iteration(struct inode *inode)
{
	reiser4_tree *tree;

	tree = reiser4_tree_by_inode(inode);
	/*
	 * reserve blocks for (in this order):
	 *
	 *     1. removal of extent item
	 *
	 *     2. insertion of tail by insert_flow()
	 *
	 *     3. drilling to the leaf level by coord_by_key()
	 *
	 *     4. possible update of stat-data
	 */
	grab_space_enable();
	return reiser4_grab_space
	    (estimate_one_item_removal(tree) +
	     estimate_insert_flow(tree->height) +
	     1 + estimate_one_insert_item(tree) +
	     inode_file_plugin(inode)->estimate.update(inode), BA_CAN_COMMIT);
}

/* for every page of file: read page, cut part of extent pointing to this page,
   put data of page tree by tail item */
int extent2tail(struct file * file, struct unix_file_info *uf_info)
{
	int result;
	struct inode *inode;
	struct page *page;
	unsigned long num_pages, i;
	unsigned long start_page;
	reiser4_key from;
	reiser4_key to;
	unsigned count;
	__u64 offset;

	assert("nikita-3362", ea_obtained(uf_info));
	inode = unix_file_info_to_inode(uf_info);
	assert("nikita-3412", !IS_RDONLY(inode));
	assert("vs-1649", uf_info->container != UF_CONTAINER_TAILS);
	assert("", !reiser4_inode_get_flag(inode, REISER4_PART_IN_CONV));

	offset = 0;
	if (reiser4_inode_get_flag(inode, REISER4_PART_MIXED)) {
		/*
		 * file is marked on disk as there was a conversion which did
		 * not complete due to either crash or some error. Find which
		 * offset tail conversion stopped at
		 */
		result = find_start(inode, EXTENT_POINTER_ID, &offset);
		if (result == -ENOENT) {
			/* no extent found, everything is converted */
			uf_info->container = UF_CONTAINER_TAILS;
			complete_conversion(inode);
			return 0;
		} else if (result != 0)
			/* some other error */
			return result;
	}
	reiser4_inode_set_flag(inode, REISER4_PART_IN_CONV);

	/* number of pages in the file */
	num_pages =
	    (inode->i_size + - offset + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT;
	start_page = offset >> PAGE_CACHE_SHIFT;

	inode_file_plugin(inode)->key_by_inode(inode, offset, &from);
	to = from;

	result = 0;
	for (i = 0; i < num_pages; i++) {
		__u64 start_byte;

		result = reserve_extent2tail_iteration(inode);
		if (result != 0)
			break;
		if (i == 0 && offset == 0) {
			reiser4_inode_set_flag(inode, REISER4_PART_MIXED);
			reiser4_update_sd(inode);
		}

		page = read_mapping_page(inode->i_mapping,
					 (unsigned)(i + start_page), NULL);
		if (IS_ERR(page)) {
			result = PTR_ERR(page);
			warning("edward-1569",
				"Can not read page %lu of %lu: %i",
				i, num_pages, result);
			break;
		}

		wait_on_page_locked(page);

		if (!PageUptodate(page)) {
			page_cache_release(page);
			result = RETERR(-EIO);
			break;
		}

		/* cut part of file we have read */
		start_byte = (__u64) ((i + start_page) << PAGE_CACHE_SHIFT);
		set_key_offset(&from, start_byte);
		set_key_offset(&to, start_byte + PAGE_CACHE_SIZE - 1);
		/*
		 * reiser4_cut_tree_object() returns -E_REPEAT to allow atom
		 * commits during over-long truncates. But
		 * extent->tail conversion should be performed in one
		 * transaction.
		 */
		result = reiser4_cut_tree(reiser4_tree_by_inode(inode), &from,
					  &to, inode, 0);

		if (result) {
			page_cache_release(page);
			warning("edward-1570",
				"Can not delete converted chunk: %i",
				result);
			break;
		}

		/* put page data into tree via tail_write */
		count = PAGE_CACHE_SIZE;
		if ((i == (num_pages - 1)) &&
		    (inode->i_size & ~PAGE_CACHE_MASK))
			/* last page can be incompleted */
			count = (inode->i_size & ~PAGE_CACHE_MASK);
		while (count) {
			loff_t pos = start_byte;

			assert("edward-1537",
			       file != NULL && file->f_dentry != NULL);
			assert("edward-1538",
			       file->f_dentry->d_inode == inode);

			result = reiser4_write_tail_noreserve(file, inode,
						 (char __user *)kmap(page),
							      count, &pos);
			kunmap(page);
			/* FIXME:
			   may be put_file_hint() instead ? */
			reiser4_free_file_fsdata(file);
			if (result <= 0) {
				/*
				 * Unsuccess in critical place:
				 * extent has been removed,
				 * but tail hasn't been created
				 */
				warning("edward-1571",
			"Report the error code %i to developers. Run FSCK",
					result);
				page_cache_release(page);
				reiser4_inode_clr_flag(inode,
						       REISER4_PART_IN_CONV);
				return result;
			}
			count -= result;
		}

		/* release page */
		lock_page(page);
		/* page is already detached from jnode and mapping. */
		assert("vs-1086", page->mapping == NULL);
		assert("nikita-2690",
		       (!PagePrivate(page) && jprivate(page) == 0));
		/* waiting for writeback completion with page lock held is
		 * perfectly valid. */
		wait_on_page_writeback(page);
		reiser4_drop_page(page);
		/* release reference taken by read_cache_page() above */
		page_cache_release(page);

		drop_exclusive_access(uf_info);
		/* throttle the conversion */
		reiser4_throttle_write(inode);
		get_exclusive_access(uf_info);
		/*
		 * nobody is allowed to complete conversion but a process which
		 * started it
		 */
		assert("", reiser4_inode_get_flag(inode, REISER4_PART_MIXED));
	}

	reiser4_inode_clr_flag(inode, REISER4_PART_IN_CONV);

	if (i == num_pages) {
		/* file is converted to formatted items */
		assert("vs-1698", reiser4_inode_get_flag(inode,
							 REISER4_PART_MIXED));
		assert("vs-1260",
		       inode_has_no_jnodes(reiser4_inode_data(inode)));

		uf_info->container = UF_CONTAINER_TAILS;
		complete_conversion(inode);
		return 0;
	}
	/*
	 * conversion is not complete. Inode was already marked as
	 * REISER4_PART_MIXED and stat-data were updated at the first
	 * iteration of the loop above.
	 */
	warning("nikita-2282",
		"Partial conversion of %llu: %lu of %lu: %i",
		(unsigned long long)get_inode_oid(inode), i,
		num_pages, result);

	/* this flag should be cleared, otherwise get_exclusive_access_careful()
	   will fall into infinite loop */
	assert("edward-1550", !reiser4_inode_get_flag(inode,
						      REISER4_PART_IN_CONV));
	return result;
}

/*
 * Local variables:
 * c-indentation-style: "K&R"
 * mode-name: "LC"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 79
 * scroll-step: 1
 * End:
 */
