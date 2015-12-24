/* Copyright 2005 by Hans Reiser, licensing governed by
   reiser4/README */

/* this file contains typical implementations for some of methods of
   struct file_operations and of struct address_space_operations
*/

#include "../inode.h"
#include "object.h"

/* file operations */

/* implementation of vfs's llseek method of struct file_operations for
   typical directory can be found in file_ops_readdir.c
*/
loff_t reiser4_llseek_dir_common(struct file *, loff_t, int origin);

/* implementation of vfs's iterate method of struct file_operations for
   typical directory can be found in file_ops_readdir.c
*/
int reiser4_iterate_common(struct file *, struct dir_context *);

/**
 * reiser4_release_dir_common - release of struct file_operations
 * @inode: inode of released file
 * @file: file to release
 *
 * Implementation of release method of struct file_operations for typical
 * directory. All it does is freeing of reiser4 specific file data.
*/
int reiser4_release_dir_common(struct inode *inode, struct file *file)
{
	reiser4_context *ctx;

	ctx = reiser4_init_context(inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	reiser4_free_file_fsdata(file);
	reiser4_exit_context(ctx);
	return 0;
}

/* this is common implementation of vfs's fsync method of struct
   file_operations
*/
int reiser4_sync_common(struct file *file, loff_t start,
			loff_t end, int datasync)
{
	reiser4_context *ctx;
	int result;
	struct dentry *dentry = file->f_path.dentry;

	ctx = reiser4_init_context(dentry->d_inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	result = txnmgr_force_commit_all(dentry->d_inode->i_sb, 0);

	context_set_commit_async(ctx);
	reiser4_exit_context(ctx);
	return result;
}

/*
 * common sync method for regular files.
 *
 * We are trying to be smart here. Instead of committing all atoms (original
 * solution), we scan dirty pages of this file and commit all atoms they are
 * part of.
 *
 * Situation is complicated by anonymous pages: i.e., extent-less pages
 * dirtied through mmap. Fortunately sys_fsync() first calls
 * filemap_fdatawrite() that will ultimately call reiser4_writepages_dispatch,
 * insert all missing extents and capture anonymous pages.
 */
int reiser4_sync_file_common(struct file *file, loff_t start, loff_t end, int datasync)
{
	reiser4_context *ctx;
	txn_atom *atom;
	reiser4_block_nr reserve;
	struct dentry *dentry = file->f_path.dentry;
	struct inode *inode = file->f_mapping->host;

	int err = filemap_write_and_wait_range(file->f_mapping->host->i_mapping, start, end);
	if (err)
		return err;

	ctx = reiser4_init_context(dentry->d_inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	mutex_lock(&inode->i_mutex);

	reserve = estimate_update_common(dentry->d_inode);
	if (reiser4_grab_space(reserve, BA_CAN_COMMIT)) {
		reiser4_exit_context(ctx);
		mutex_unlock(&inode->i_mutex);
		return RETERR(-ENOSPC);
	}
	write_sd_by_inode_common(dentry->d_inode);

	atom = get_current_atom_locked();
	spin_lock_txnh(ctx->trans);
	force_commit_atom(ctx->trans);
	reiser4_exit_context(ctx);
	mutex_unlock(&inode->i_mutex);

	return 0;
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
