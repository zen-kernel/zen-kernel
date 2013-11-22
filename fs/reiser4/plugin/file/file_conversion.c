/* Copyright 2001, 2002, 2003 by Hans Reiser,
   licensing governed by reiser4/README */

/**
 * This file contains dispatching hooks, and conversion methods, which
 * implement transitions in the FILE interface.
 *
 * Dispatching hook makes a decision (at dispatching point) about the
 * most reasonable plugin. Such decision is made in accordance with some
 * O(1)-heuristic.
 *
 * We implement a transition CRYPTCOMPRESS -> UNIX_FILE for files with
 * incompressible data. Current heuristic to estimate compressibility is
 * very simple: if first complete logical cluster (64K by default) of a
 * file is incompressible, then we make a decision, that the whole file
 * is incompressible.
 *
 * To enable dispatching we install a special "magic" compression mode
 * plugin CONVX_COMPRESSION_MODE_ID at file creation time.
 *
 * Note, that we don't perform back conversion (UNIX_FILE->CRYPTCOMPRESS)
 * because of compatibility reasons).
 *
 * In conversion time we protect CS, the conversion set (file's (meta)data
 * and plugin table (pset)) via special per-inode rw-semaphore (conv_sem).
 * The methods which implement conversion are CS writers. The methods of FS
 * interface (file_operations, inode_operations, address_space_operations)
 * are CS readers.
 */

#include "../../inode.h"
#include "../cluster.h"
#include "file.h"

#define conversion_enabled(inode)                                      \
	 (inode_compression_mode_plugin(inode) ==		       \
	  compression_mode_plugin_by_id(CONVX_COMPRESSION_MODE_ID))

/**
 * Located sections (readers and writers of @pset) are not permanently
 * critical: cryptcompress file can be converted only if the conversion
 * is enabled (see the macrio above). Also we don't perform back
 * conversion. The following helper macro is a sanity check to decide
 * if we need the protection (locks are always additional overheads).
 */
#define should_protect(inode)						\
	(inode_file_plugin(inode) ==					\
	 file_plugin_by_id(CRYPTCOMPRESS_FILE_PLUGIN_ID) &&		\
	 conversion_enabled(inode))
/**
 * To avoid confusion with read/write file operations, we'll speak about
 * "passive" protection for FCS readers and "active" protection for FCS
 * writers. All methods with active or passive protection have suffix
 * "careful".
 */
/**
 * Macros for passive protection.
 *
 * Construct invariant operation to be supplied to VFS.
 * The macro accepts the following lexemes:
 * @type - type of the value represented by the compound statement;
 * @method - name of an operation to be supplied to VFS (reiser4 file
 * plugin also should contain a method with such name).
 */
#define PROT_PASSIVE(type, method, args)				\
({							                \
	type _result;							\
	struct rw_semaphore * guard =					\
		&reiser4_inode_data(inode)->conv_sem;			\
									\
	if (should_protect(inode)) {					\
		down_read(guard);					\
		if (!should_protect(inode))				\
			up_read(guard);					\
	}								\
	_result = inode_file_plugin(inode)->method args;		\
	if (should_protect(inode))					\
		up_read(guard);						\
	_result;							\
})

#define PROT_PASSIVE_VOID(method, args)					\
({							                \
	struct rw_semaphore * guard =					\
		&reiser4_inode_data(inode)->conv_sem;			\
									\
	if (should_protect(inode)) {					\
		down_read(guard);					\
		if (!should_protect(inode))				\
			up_read(guard);					\
	}								\
	inode_file_plugin(inode)->method args;				\
									\
	if (should_protect(inode))					\
		up_read(guard);						\
})

/* Pass management to the unix-file plugin with "notail" policy */
static int __cryptcompress2unixfile(struct file *file, struct inode * inode)
{
	int result;
	reiser4_inode *info;
	struct unix_file_info * uf;
	info = reiser4_inode_data(inode);

	result = aset_set_unsafe(&info->pset,
			    PSET_FILE,
			    (reiser4_plugin *)
			    file_plugin_by_id(UNIX_FILE_PLUGIN_ID));
	if (result)
		return result;
	result = aset_set_unsafe(&info->pset,
			    PSET_FORMATTING,
			    (reiser4_plugin *)
			    formatting_plugin_by_id(NEVER_TAILS_FORMATTING_ID));
	if (result)
		return result;
	/* get rid of non-standard plugins */
	info->plugin_mask &= ~cryptcompress_mask;
	/* get rid of plugin stat-data extension */
	info->extmask &= ~(1 << PLUGIN_STAT);

	reiser4_inode_clr_flag(inode, REISER4_SDLEN_KNOWN);

	/* FIXME use init_inode_data_unix_file() instead,
	   but aviod init_inode_ordering() */
	/* Init unix-file specific part of inode */
	uf = unix_file_inode_data(inode);
	uf->container = UF_CONTAINER_UNKNOWN;
	init_rwsem(&uf->latch);
	uf->tplug = inode_formatting_plugin(inode);
	uf->exclusive_use = 0;
#if REISER4_DEBUG
	uf->ea_owner = NULL;
	atomic_set(&uf->nr_neas, 0);
#endif
	/**
	 * we was carefull for file_ops, inode_ops and as_ops
	 * to be invariant for plugin conversion, so there is
	 * no need to update ones already installed in the
	 * vfs's residence.
	 */
	return 0;
}

#if REISER4_DEBUG
static int disabled_conversion_inode_ok(struct inode * inode)
{
	__u64 extmask = reiser4_inode_data(inode)->extmask;
	__u16 plugin_mask = reiser4_inode_data(inode)->plugin_mask;

	return ((extmask & (1 << LIGHT_WEIGHT_STAT)) &&
		(extmask & (1 << UNIX_STAT)) &&
		(extmask & (1 << LARGE_TIMES_STAT)) &&
		(extmask & (1 << PLUGIN_STAT)) &&
		(plugin_mask & (1 << PSET_COMPRESSION_MODE)));
}
#endif

/**
 * Disable future attempts to schedule/convert file plugin.
 * This function is called by plugin schedule hooks.
 *
 * To disable conversion we assign any compression mode plugin id
 * different from CONVX_COMPRESSION_MODE_ID.
 */
static int disable_conversion(struct inode * inode)
{
	int result;
	result =
	       force_plugin_pset(inode,
				 PSET_COMPRESSION_MODE,
				 (reiser4_plugin *)compression_mode_plugin_by_id
				 (LATTD_COMPRESSION_MODE_ID));
	assert("edward-1500",
	       ergo(!result, disabled_conversion_inode_ok(inode)));
	return result;
}

/**
 * Check if we really have achieved plugin scheduling point
 */
static int check_dispatch_point(struct inode * inode,
				loff_t pos /* position in the
					      file to write from */,
				struct cluster_handle * clust,
				struct dispatch_context * cont)
{
	assert("edward-1505", conversion_enabled(inode));
	/*
	 * if file size is more then cluster size, then compressible
	 * status must be figured out (i.e. compression was disabled,
	 * or file plugin was converted to unix_file)
	 */
	assert("edward-1506", inode->i_size <= inode_cluster_size(inode));

	if (pos > inode->i_size)
		/* first logical cluster will contain a (partial) hole */
		return disable_conversion(inode);
	if (pos < inode_cluster_size(inode))
		/* writing to the first logical cluster */
		return 0;
	/*
	 * here we have:
	 * cluster_size <= pos <= i_size <= cluster_size,
	 * and, hence,  pos == i_size == cluster_size
	 */
	assert("edward-1498",
	       pos == inode->i_size &&
	       pos == inode_cluster_size(inode));
	assert("edward-1539", cont != NULL);
	assert("edward-1540", cont->state == DISPATCH_INVAL_STATE);

	cont->state = DISPATCH_POINT;
	return 0;
}

static void start_check_compressibility(struct inode * inode,
					struct cluster_handle * clust,
					hint_t * hint)
{
	assert("edward-1507", clust->index == 1);
	assert("edward-1508", !tfm_cluster_is_uptodate(&clust->tc));
	assert("edward-1509", cluster_get_tfm_act(&clust->tc) == TFMA_READ);

	hint_init_zero(hint);
	clust->hint = hint;
	clust->index --;
	clust->nr_pages = size_in_pages(lbytes(clust->index, inode));

	/* first logical cluster (of index #0) must be complete */
	assert("edward-1510", lbytes(clust->index, inode) ==
	       inode_cluster_size(inode));
}

static void finish_check_compressibility(struct inode * inode,
					 struct cluster_handle * clust,
					 hint_t * hint)
{
	reiser4_unset_hint(clust->hint);
	clust->hint = hint;
	clust->index ++;
}

#if REISER4_DEBUG
static int prepped_dclust_ok(hint_t * hint)
{
	reiser4_key key;
	coord_t * coord = &hint->ext_coord.coord;

	item_key_by_coord(coord, &key);
	return (item_id_by_coord(coord) == CTAIL_ID &&
		!coord_is_unprepped_ctail(coord) &&
		(get_key_offset(&key) + nr_units_ctail(coord) ==
		 dclust_get_extension_dsize(hint)));
}
#endif

#define fifty_persent(size) (size >> 1)
/* evaluation of data compressibility */
#define data_is_compressible(osize, isize)		\
	(osize < fifty_persent(isize))

/**
 * A simple O(1)-heuristic for compressibility.
 * This is called not more then one time per file's life.
 * Read first logical cluster (of index #0) and estimate its compressibility.
 * Save estimation result in @cont.
 */
static int read_check_compressibility(struct inode * inode,
				      struct cluster_handle * clust,
				      struct dispatch_context * cont)
{
	int i;
	int result;
	size_t dst_len;
	hint_t tmp_hint;
	hint_t * cur_hint = clust->hint;
	assert("edward-1541", cont->state == DISPATCH_POINT);

	start_check_compressibility(inode, clust, &tmp_hint);

	reset_cluster_pgset(clust, cluster_nrpages(inode));
	result = grab_page_cluster(inode, clust, READ_OP);
	if (result)
		return result;
	/* Read page cluster here */
	for (i = 0; i < clust->nr_pages; i++) {
		struct page *page = clust->pages[i];
		lock_page(page);
		result = do_readpage_ctail(inode, clust, page,
					   ZNODE_READ_LOCK);
		unlock_page(page);
		if (result)
			goto error;
	}
	tfm_cluster_clr_uptodate(&clust->tc);

	cluster_set_tfm_act(&clust->tc, TFMA_WRITE);

	if (hint_is_valid(&tmp_hint) && !hint_is_unprepped_dclust(&tmp_hint)) {
		/* lenght of compressed data is known, no need to compress */
		assert("edward-1511",
		       znode_is_any_locked(tmp_hint.lh.node));
		assert("edward-1512",
		       WITH_DATA(tmp_hint.ext_coord.coord.node,
				 prepped_dclust_ok(&tmp_hint)));
		dst_len = dclust_get_extension_dsize(&tmp_hint);
	}
	else {
		struct tfm_cluster * tc = &clust->tc;
		compression_plugin * cplug = inode_compression_plugin(inode);
		result = grab_tfm_stream(inode, tc, INPUT_STREAM);
		if (result)
			goto error;
		for (i = 0; i < clust->nr_pages; i++) {
			char *data;
			lock_page(clust->pages[i]);
			BUG_ON(!PageUptodate(clust->pages[i]));
			data = kmap(clust->pages[i]);
			memcpy(tfm_stream_data(tc, INPUT_STREAM) + pg_to_off(i),
			       data, PAGE_CACHE_SIZE);
			kunmap(clust->pages[i]);
			unlock_page(clust->pages[i]);
		}
		result = grab_tfm_stream(inode, tc, OUTPUT_STREAM);
		if (result)
			goto error;
		result = grab_coa(tc, cplug);
		if (result)
			goto error;
		tc->len = tc->lsize = lbytes(clust->index, inode);
		assert("edward-1513", tc->len == inode_cluster_size(inode));
		dst_len = tfm_stream_size(tc, OUTPUT_STREAM);
		cplug->compress(get_coa(tc, cplug->h.id, tc->act),
				tfm_input_data(clust), tc->len,
				tfm_output_data(clust), &dst_len);
		assert("edward-1514",
		       dst_len <= tfm_stream_size(tc, OUTPUT_STREAM));
	}
	finish_check_compressibility(inode, clust, cur_hint);
	cont->state =
		(data_is_compressible(dst_len, inode_cluster_size(inode)) ?
		 DISPATCH_REMAINS_OLD :
		 DISPATCH_ASSIGNED_NEW);
	return 0;
 error:
	put_page_cluster(clust, inode, READ_OP);
	return result;
}

/* Cut disk cluster of index @idx */
static int cut_disk_cluster(struct inode * inode, cloff_t idx)
{
	reiser4_key from, to;
	assert("edward-1515", inode_file_plugin(inode) ==
	       file_plugin_by_id(CRYPTCOMPRESS_FILE_PLUGIN_ID));
	key_by_inode_cryptcompress(inode, clust_to_off(idx, inode), &from);
	to = from;
	set_key_offset(&to,
		       get_key_offset(&from) + inode_cluster_size(inode) - 1);
	return reiser4_cut_tree(reiser4_tree_by_inode(inode),
				&from, &to, inode, 0);
}

static int reserve_cryptcompress2unixfile(struct inode *inode)
{
	reiser4_block_nr unformatted_nodes;
	reiser4_tree *tree;

	tree = reiser4_tree_by_inode(inode);

	/* number of unformatted nodes which will be created */
	unformatted_nodes = cluster_nrpages(inode); /* N */

	/*
	 * space required for one iteration of extent->tail conversion:
	 *
	 *     1. kill ctail items
	 *
	 *     2. insert N unformatted nodes
	 *
	 *     3. insert N (worst-case single-block
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
		 unformatted_nodes  +
		 unformatted_nodes * estimate_one_insert_into_item(tree) +
		 1 + estimate_one_insert_item(tree) +
		 inode_file_plugin(inode)->estimate.update(inode),
		 BA_CAN_COMMIT);
}

/**
 * Convert cryptcompress file plugin to unix_file plugin.
 */
static int cryptcompress2unixfile(struct file *file, struct inode *inode,
				  struct dispatch_context *cont)
{
	int i;
	int result = 0;
	struct cryptcompress_info *cr_info;
	struct unix_file_info *uf_info;
	assert("edward-1516", cont->pages[0]->index == 0);

	/* release all cryptcompress-specific resources */
	cr_info = cryptcompress_inode_data(inode);
	result = reserve_cryptcompress2unixfile(inode);
	if (result)
		goto out;
	/* tell kill_hook to not truncate pages */
	reiser4_inode_set_flag(inode, REISER4_FILE_CONV_IN_PROGRESS);
	result = cut_disk_cluster(inode, 0);
	if (result)
		goto out;
	/* captured jnode of cluster and assotiated resources (pages,
	   reserved disk space) were released by ->kill_hook() method
	   of the item plugin */

	result = __cryptcompress2unixfile(file, inode);
	if (result)
		goto out;
	/* At this point file is managed by unix file plugin */

	uf_info = unix_file_inode_data(inode);

	assert("edward-1518",
	       ergo(jprivate(cont->pages[0]),
		    !jnode_is_cluster_page(jprivate(cont->pages[0]))));
	for(i = 0; i < cont->nr_pages; i++) {
		assert("edward-1519", cont->pages[i]);
		assert("edward-1520", PageUptodate(cont->pages[i]));

		result = find_or_create_extent(cont->pages[i]);
		if (result)
			break;
	}
	if (unlikely(result))
		goto out;
	uf_info->container = UF_CONTAINER_EXTENTS;
	result = reiser4_update_sd(inode);
 out:
	all_grabbed2free();
	return result;
}

#define convert_file_plugin cryptcompress2unixfile

/**
 * This is called by ->write() method of a cryptcompress file plugin.
 * Make a decision about the most reasonable file plugin id to manage
 * the file.
 */
int write_dispatch_hook(struct file *file, struct inode *inode,
			loff_t pos, struct cluster_handle *clust,
			struct dispatch_context *cont)
{
	int result;
	if (!conversion_enabled(inode))
		return 0;
	result = check_dispatch_point(inode, pos, clust, cont);
	if (result || cont->state != DISPATCH_POINT)
		return result;
	result = read_check_compressibility(inode, clust, cont);
	if (result)
		return result;
	if (cont->state == DISPATCH_REMAINS_OLD) {
		put_page_cluster(clust, inode, READ_OP);
		return disable_conversion(inode);
	}
	assert("edward-1543", cont->state == DISPATCH_ASSIGNED_NEW);
	/*
	 * page cluster is grabbed and uptodate. It will be
	 * released with a pgset after plugin conversion is
	 * finished, see put_dispatch_context().
	 */
	reiser4_unset_hint(clust->hint);
	move_cluster_pgset(clust, &cont->pages, &cont->nr_pages);
	return 0;
}

/**
 * This is called by ->setattr() method of cryptcompress file plugin.
 */
int setattr_dispatch_hook(struct inode * inode)
{
	if (conversion_enabled(inode))
		return disable_conversion(inode);
	return 0;
}

static inline void init_dispatch_context(struct dispatch_context * cont)
{
	memset(cont, 0, sizeof(*cont));
}

static inline void done_dispatch_context(struct dispatch_context * cont,
					 struct inode * inode)
{
	if (cont->pages) {
		__put_page_cluster(0, cont->nr_pages, cont->pages, inode);
		kfree(cont->pages);
	}
}

/*
 * ->write() VFS file operation
 *
 * performs "intelligent" conversion in the FILE interface.
 * Write a file in 3 steps (2d and 3d steps are optional).
 */
ssize_t reiser4_write_dispatch(struct file *file, const char __user *buf,
			       size_t count, loff_t *off)
{
	int result;
	reiser4_context *ctx;
	ssize_t written_old = 0; /* bytes written with initial plugin */
	ssize_t written_new = 0; /* bytes written with new plugin */
	struct dispatch_context cont;
	struct inode * inode = file->f_dentry->d_inode;

	ctx = reiser4_init_context(inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	current->backing_dev_info = inode->i_mapping->backing_dev_info;
	init_dispatch_context(&cont);
	mutex_lock(&inode->i_mutex);
	/**
	 * First step.
	 * Start write with initial file plugin.
	 * Keep a plugin schedule status at @cont (if any).
	 */
	written_old = inode_file_plugin(inode)->write(file,
						      buf,
						      count,
						      off,
						      &cont);
	if (cont.state != DISPATCH_ASSIGNED_NEW || written_old < 0)
		goto exit;
	/**
	 * Second step.
	 * New file plugin has been scheduled.
	 * Perform conversion to the new plugin.
	 */
	down_read(&reiser4_inode_data(inode)->conv_sem);
	result = convert_file_plugin(file, inode, &cont);
	up_read(&reiser4_inode_data(inode)->conv_sem);
	if (result) {
		warning("edward-1544",
			"Inode %llu: file plugin conversion failed (%d)",
			(unsigned long long)get_inode_oid(inode),
			result);
		context_set_commit_async(ctx);
		goto exit;
	}
	reiser4_txn_restart(ctx);
	/**
	 * Third step:
	 * Finish write with the new file plugin.
	 */
	assert("edward-1536",
	       inode_file_plugin(inode) ==
	       file_plugin_by_id(UNIX_FILE_PLUGIN_ID));

	written_new = inode_file_plugin(inode)->write(file,
						      buf + written_old,
						      count - written_old,
						      off,
						      NULL);
 exit:
	mutex_unlock(&inode->i_mutex);
	done_dispatch_context(&cont, inode);
	current->backing_dev_info = NULL;
	context_set_commit_async(ctx);
	reiser4_exit_context(ctx);

	return written_old + (written_new < 0 ? 0 : written_new);
}

/*
 * Dispatchers with "passive" protection for:
 *
 * ->open();
 * ->read();
 * ->ioctl();
 * ->mmap();
 * ->release();
 * ->bmap().
 */

int reiser4_open_dispatch(struct inode *inode, struct file *file)
{
	return PROT_PASSIVE(int, open, (inode, file));
}

ssize_t reiser4_read_dispatch(struct file * file, char __user * buf,
			      size_t size, loff_t * off)
{
	struct inode * inode = file->f_dentry->d_inode;
	return PROT_PASSIVE(ssize_t, read, (file, buf, size, off));
}

long reiser4_ioctl_dispatch(struct file *filp, unsigned int cmd,
			    unsigned long arg)
{
	struct inode * inode = filp->f_dentry->d_inode;
	return PROT_PASSIVE(int, ioctl, (filp, cmd, arg));
}

int reiser4_mmap_dispatch(struct file *file, struct vm_area_struct *vma)
{
	struct inode *inode = file->f_dentry->d_inode;
	return PROT_PASSIVE(int, mmap, (file, vma));
}

int reiser4_release_dispatch(struct inode *inode, struct file *file)
{
	return PROT_PASSIVE(int, release, (inode, file));
}

sector_t reiser4_bmap_dispatch(struct address_space * mapping, sector_t lblock)
{
	struct inode *inode = mapping->host;
	return PROT_PASSIVE(sector_t, bmap, (mapping, lblock));
}

/**
 * NOTE: The following two methods are
 * used only for loopback functionality.
 * reiser4_write_end() can not cope with
 * short writes for now.
 */
int reiser4_write_begin_dispatch(struct file *file,
				 struct address_space *mapping,
				 loff_t pos,
				 unsigned len,
				 unsigned flags,
				 struct page **pagep,
				 void **fsdata)
{
	int ret = 0;
	struct page *page;
	pgoff_t index;
	reiser4_context *ctx;
	struct inode * inode = file->f_dentry->d_inode;

	index = pos >> PAGE_CACHE_SHIFT;
	page = grab_cache_page_write_begin(mapping, index,
					   flags & AOP_FLAG_NOFS);
	*pagep = page;
	if (!page)
		return -ENOMEM;

	ctx = reiser4_init_context(file->f_dentry->d_inode->i_sb);
	if (IS_ERR(ctx)) {
		ret = PTR_ERR(ctx);
		goto err2;
	}
	ret = reiser4_grab_space_force(/* for update_sd:
					* one when updating file size and
					* one when updating mtime/ctime */
				       2 * estimate_update_common(inode),
				       BA_CAN_COMMIT);
	if (ret)
		goto err1;
	ret = PROT_PASSIVE(int, write_begin, (file, page, pos, len, fsdata));
	if (unlikely(ret))
		goto err1;
	/* Success. Resorces will be released in write_end_dispatch */
	return 0;
 err1:
	reiser4_exit_context(ctx);
 err2:
	unlock_page(page);
	page_cache_release(page);
	return ret;
}

int reiser4_write_end_dispatch(struct file *file,
			       struct address_space *mapping,
			       loff_t pos,
			       unsigned len,
			       unsigned copied,
			       struct page *page,
			       void *fsdata)
{
	int ret;
	reiser4_context *ctx;
	struct inode *inode = page->mapping->host;

	assert("umka-3101", file != NULL);
	assert("umka-3102", page != NULL);
	assert("umka-3093", PageLocked(page));

	ctx = get_current_context();

	SetPageUptodate(page);
	set_page_dirty_notag(page);

	ret = PROT_PASSIVE(int, write_end, (file, page, pos, copied, fsdata));
	page_cache_release(page);

	/* don't commit transaction under inode semaphore */
	context_set_commit_async(ctx);
	reiser4_exit_context(ctx);
	return ret == 0 ? copied : ret;
}

/*
 * Dispatchers without protection
 */
int reiser4_setattr_dispatch(struct dentry *dentry, struct iattr *attr)
{
	return inode_file_plugin(dentry->d_inode)->setattr(dentry, attr);
}

/*
  Local variables:
  c-indentation-style: "K&R"
  mode-name: "LC"
  c-basic-offset: 8
  tab-width: 8
  fill-column: 80
  scroll-step: 1
  End:
*/
