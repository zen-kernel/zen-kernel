/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Interface to VFS. Reiser4 {super|export|dentry}_operations are defined
   here. */

#include "forward.h"
#include "debug.h"
#include "dformat.h"
#include "coord.h"
#include "plugin/item/item.h"
#include "plugin/file/file.h"
#include "plugin/security/perm.h"
#include "plugin/disk_format/disk_format.h"
#include "plugin/plugin.h"
#include "plugin/plugin_set.h"
#include "plugin/object.h"
#include "txnmgr.h"
#include "jnode.h"
#include "znode.h"
#include "block_alloc.h"
#include "tree.h"
#include "vfs_ops.h"
#include "inode.h"
#include "page_cache.h"
#include "ktxnmgrd.h"
#include "super.h"
#include "reiser4.h"
#include "entd.h"
#include "status_flags.h"
#include "flush.h"
#include "dscale.h"

#include <linux/profile.h>
#include <linux/types.h>
#include <linux/mount.h>
#include <linux/vfs.h>
#include <linux/mm.h>
#include <linux/buffer_head.h>
#include <linux/dcache.h>
#include <linux/list.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/seq_file.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/quotaops.h>
#include <linux/security.h>
#include <linux/reboot.h>
#include <linux/rcupdate.h>

/* update inode stat-data by calling plugin */
int reiser4_update_sd(struct inode *object)
{
	file_plugin *fplug;

	assert("nikita-2338", object != NULL);
	/* check for read-only file system. */
	if (IS_RDONLY(object))
		return 0;

	fplug = inode_file_plugin(object);
	assert("nikita-2339", fplug != NULL);
	return fplug->write_sd_by_inode(object);
}

/* helper function: increase inode nlink count and call plugin method to save
   updated stat-data.

   Used by link/create and during creation of dot and dotdot in mkdir
*/
int reiser4_add_nlink(struct inode *object /* object to which link is added */ ,
		      struct inode *parent /* parent where new entry will be */
		      ,
		      int write_sd_p	/* true if stat-data has to be
					 * updated */ )
{
	file_plugin *fplug;
	int result;

	assert("nikita-1351", object != NULL);

	fplug = inode_file_plugin(object);
	assert("nikita-1445", fplug != NULL);

	/* ask plugin whether it can add yet another link to this
	   object */
	if (!fplug->can_add_link(object))
		return RETERR(-EMLINK);

	assert("nikita-2211", fplug->add_link != NULL);
	/* call plugin to do actual addition of link */
	result = fplug->add_link(object, parent);

	/* optionally update stat data */
	if (result == 0 && write_sd_p)
		result = fplug->write_sd_by_inode(object);
	return result;
}

/* helper function: decrease inode nlink count and call plugin method to save
   updated stat-data.

   Used by unlink/create
*/
int reiser4_del_nlink(struct inode *object	/* object from which link is
						 * removed */ ,
		      struct inode *parent /* parent where entry was */ ,
		      int write_sd_p	/* true is stat-data has to be
					 * updated */ )
{
	file_plugin *fplug;
	int result;

	assert("nikita-1349", object != NULL);

	fplug = inode_file_plugin(object);
	assert("nikita-1350", fplug != NULL);
	assert("nikita-1446", object->i_nlink > 0);
	assert("nikita-2210", fplug->rem_link != NULL);

	/* call plugin to do actual deletion of link */
	result = fplug->rem_link(object, parent);

	/* optionally update stat data */
	if (result == 0 && write_sd_p)
		result = fplug->write_sd_by_inode(object);
	return result;
}

/* Release reiser4 dentry. This is d_op->d_release() method. */
static void reiser4_d_release(struct dentry *dentry /* dentry released */ )
{
	reiser4_free_dentry_fsdata(dentry);
}

/*
 * Called by reiser4_sync_inodes(), during speculative write-back (through
 * pdflush, or balance_dirty_pages()).
 */
void reiser4_writeout(struct super_block *sb, struct writeback_control *wbc)
{
	long written = 0;
	int repeats = 0;
	int result;
	struct address_space *mapping;

	/*
	 * Performs early flushing, trying to free some memory. If there
	 * is nothing to flush, commits some atoms.
	 *
	 * Commit all atoms if reiser4_writepages_dispatch() is called
	 * from sys_sync() or sys_fsync()
	 */
	if (wbc->sync_mode != WB_SYNC_NONE) {
		txnmgr_force_commit_all(sb, 0);
		return;
	}

	BUG_ON(reiser4_get_super_fake(sb) == NULL);
	mapping = reiser4_get_super_fake(sb)->i_mapping;
	do {
		long nr_submitted = 0;
		jnode *node = NULL;

		/* do not put more requests to overload write queue */
		if (bdi_write_congested(mapping->backing_dev_info)) {
			//blk_flush_plug(current);
			break;
		}
		repeats++;
		BUG_ON(wbc->nr_to_write <= 0);

		if (get_current_context()->entd) {
			entd_context *ent = get_entd_context(sb);

			if (ent->cur_request->node)
				/*
				 * this is ent thread and it managed to capture
				 * requested page itself - start flush from
				 * that page
				 */
				node = ent->cur_request->node;
		}

		result = flush_some_atom(node, &nr_submitted, wbc,
					 JNODE_FLUSH_WRITE_BLOCKS);
		if (result != 0)
			warning("nikita-31001", "Flush failed: %i", result);
		if (node)
			/* drop the reference aquired
			   in find_or_create_extent() */
			jput(node);
		if (!nr_submitted)
			break;

		wbc->nr_to_write -= nr_submitted;
		written += nr_submitted;
	} while (wbc->nr_to_write > 0);
}

/* tell VM how many pages were dirtied */
void reiser4_throttle_write(struct inode *inode)
{
	reiser4_context *ctx;

	ctx = get_current_context();
	reiser4_txn_restart(ctx);
	current->journal_info = NULL;
	balance_dirty_pages_ratelimited(inode->i_mapping);
	current->journal_info = ctx;
}

const char *REISER4_SUPER_MAGIC_STRING = "ReIsEr4";
const int REISER4_MAGIC_OFFSET = 16 * 4096;	/* offset to magic string from the
						 * beginning of device */

/*
 * Reiser4 initialization/shutdown.
 *
 * Code below performs global reiser4 initialization that is done either as
 * part of kernel initialization (when reiser4 is statically built-in), or
 * during reiser4 module load (when compiled as module).
 */

void reiser4_handle_error(void)
{
	struct super_block *sb = reiser4_get_current_sb();

	if (!sb)
		return;
	reiser4_status_write(REISER4_STATUS_DAMAGED, 0,
			     "Filesystem error occured");
	switch (get_super_private(sb)->onerror) {
	case 0:
		reiser4_panic("foobar-42", "Filesystem error occured\n");
	case 1:
	default:
		if (sb->s_flags & MS_RDONLY)
			return;
		sb->s_flags |= MS_RDONLY;
		break;
	}
}

struct dentry_operations reiser4_dentry_operations = {
	.d_revalidate = NULL,
	.d_hash = NULL,
	.d_compare = NULL,
	.d_delete = NULL,
	.d_release = reiser4_d_release,
	.d_iput = NULL,
};

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
