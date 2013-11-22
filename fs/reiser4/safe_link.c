/* Copyright 2003, 2004 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Safe-links. */

/*
 * Safe-links are used to maintain file system consistency during operations
 * that spawns multiple transactions. For example:
 *
 *     1. Unlink. UNIX supports "open-but-unlinked" files, that is files
 *     without user-visible names in the file system, but still opened by some
 *     active process. What happens here is that unlink proper (i.e., removal
 *     of the last file name) and file deletion (truncate of file body to zero
 *     and deletion of stat-data, that happens when last file descriptor is
 *     closed), may belong to different transactions T1 and T2. If a crash
 *     happens after T1 commit, but before T2 commit, on-disk file system has
 *     a file without name, that is, disk space leak.
 *
 *     2. Truncate. Truncate of large file may spawn multiple transactions. If
 *     system crashes while truncate was in-progress, file is left partially
 *     truncated, which violates "atomicity guarantees" of reiser4, viz. that
 *     every system is atomic.
 *
 * Safe-links address both above cases. Basically, safe-link is a way post
 * some operation to be executed during commit of some other transaction than
 * current one. (Another way to look at the safe-link is to interpret it as a
 * logical logging.)
 *
 * Specifically, at the beginning of unlink safe-link in inserted in the
 * tree. This safe-link is normally removed by file deletion code (during
 * transaction T2 in the above terms). Truncate also inserts safe-link that is
 * normally removed when truncate operation is finished.
 *
 * This means, that in the case of "clean umount" there are no safe-links in
 * the tree. If safe-links are observed during mount, it means that (a) system
 * was terminated abnormally, and (b) safe-link correspond to the "pending"
 * (i.e., not finished) operations that were in-progress during system
 * termination. Each safe-link record enough information to complete
 * corresponding operation, and mount simply "replays" them (hence, the
 * analogy with the logical logging).
 *
 * Safe-links are implemented as blackbox items (see
 * plugin/item/blackbox.[ch]).
 *
 * For the reference: ext3 also has similar mechanism, it's called "an orphan
 * list" there.
 */

#include "safe_link.h"
#include "debug.h"
#include "inode.h"

#include "plugin/item/blackbox.h"

#include <linux/fs.h>

/*
 * On-disk format of safe-link.
 */
typedef struct safelink {
	reiser4_key sdkey;	/* key of stat-data for the file safe-link is
				 * for */
	d64 size;		/* size to which file should be truncated */
} safelink_t;

/*
 * locality where safe-link items are stored. Next to the objectid of root
 * directory.
 */
static oid_t safe_link_locality(reiser4_tree * tree)
{
	return get_key_objectid(get_super_private(tree->super)->df_plug->
				root_dir_key(tree->super)) + 1;
}

/*
  Construct a key for the safe-link. Key has the following format:

|        60     | 4 |        64        | 4 |      60       |         64       |
+---------------+---+------------------+---+---------------+------------------+
|   locality    | 0 |        0         | 0 |   objectid    |     link type    |
+---------------+---+------------------+---+---------------+------------------+
|                   |                  |                   |                  |
|     8 bytes       |     8 bytes      |      8 bytes      |      8 bytes     |

   This is in large keys format. In small keys format second 8 byte chunk is
   out. Locality is a constant returned by safe_link_locality(). objectid is
   an oid of a file on which operation protected by this safe-link is
   performed. link-type is used to distinguish safe-links for different
   operations.

 */
static reiser4_key *build_link_key(reiser4_tree * tree, oid_t oid,
				   reiser4_safe_link_t link, reiser4_key * key)
{
	reiser4_key_init(key);
	set_key_locality(key, safe_link_locality(tree));
	set_key_objectid(key, oid);
	set_key_offset(key, link);
	return key;
}

/*
 * how much disk space is necessary to insert and remove (in the
 * error-handling path) safe-link.
 */
static __u64 safe_link_tograb(reiser4_tree * tree)
{
	return
	    /* insert safe link */
	    estimate_one_insert_item(tree) +
	    /* remove safe link */
	    estimate_one_item_removal(tree) +
	    /* drill to the leaf level during insertion */
	    1 + estimate_one_insert_item(tree) +
	    /*
	     * possible update of existing safe-link. Actually, if
	     * safe-link existed already (we failed to remove it), then no
	     * insertion is necessary, so this term is already "covered",
	     * but for simplicity let's left it.
	     */
	    1;
}

/*
 * grab enough disk space to insert and remove (in the error-handling path)
 * safe-link.
 */
int safe_link_grab(reiser4_tree * tree, reiser4_ba_flags_t flags)
{
	int result;

	grab_space_enable();
	/* The sbinfo->delete_mutex can be taken here.
	 * safe_link_release() should be called before leaving reiser4
	 * context. */
	result =
	    reiser4_grab_reserved(tree->super, safe_link_tograb(tree), flags);
	grab_space_enable();
	return result;
}

/*
 * release unused disk space reserved by safe_link_grab().
 */
void safe_link_release(reiser4_tree * tree)
{
	reiser4_release_reserved(tree->super);
}

/*
 * insert into tree safe-link for operation @link on inode @inode.
 */
int safe_link_add(struct inode *inode, reiser4_safe_link_t link)
{
	reiser4_key key;
	safelink_t sl;
	int length;
	int result;
	reiser4_tree *tree;

	build_sd_key(inode, &sl.sdkey);
	length = sizeof sl.sdkey;

	if (link == SAFE_TRUNCATE) {
		/*
		 * for truncate we have to store final file length also,
		 * expand item.
		 */
		length += sizeof(sl.size);
		put_unaligned(cpu_to_le64(inode->i_size), &sl.size);
	}
	tree = reiser4_tree_by_inode(inode);
	build_link_key(tree, get_inode_oid(inode), link, &key);

	result = store_black_box(tree, &key, &sl, length);
	if (result == -EEXIST)
		result = update_black_box(tree, &key, &sl, length);
	return result;
}

/*
 * remove safe-link corresponding to the operation @link on inode @inode from
 * the tree.
 */
int safe_link_del(reiser4_tree * tree, oid_t oid, reiser4_safe_link_t link)
{
	reiser4_key key;

	return kill_black_box(tree, build_link_key(tree, oid, link, &key));
}

/*
 * in-memory structure to keep information extracted from safe-link. This is
 * used to iterate over all safe-links.
 */
struct safe_link_context {
	reiser4_tree *tree;	/* internal tree */
	reiser4_key key;	/* safe-link key */
	reiser4_key sdkey;	/* key of object stat-data */
	reiser4_safe_link_t link;	/* safe-link type */
	oid_t oid;		/* object oid */
	__u64 size;		/* final size for truncate */
};

/*
 * start iterating over all safe-links.
 */
static void safe_link_iter_begin(reiser4_tree * tree,
				 struct safe_link_context *ctx)
{
	ctx->tree = tree;
	reiser4_key_init(&ctx->key);
	set_key_locality(&ctx->key, safe_link_locality(tree));
	set_key_objectid(&ctx->key, get_key_objectid(reiser4_max_key()));
	set_key_offset(&ctx->key, get_key_offset(reiser4_max_key()));
}

/*
 * return next safe-link.
 */
static int safe_link_iter_next(struct safe_link_context *ctx)
{
	int result;
	safelink_t sl;

	result = load_black_box(ctx->tree, &ctx->key, &sl, sizeof sl, 0);
	if (result == 0) {
		ctx->oid = get_key_objectid(&ctx->key);
		ctx->link = get_key_offset(&ctx->key);
		ctx->sdkey = sl.sdkey;
		if (ctx->link == SAFE_TRUNCATE)
			ctx->size = le64_to_cpu(get_unaligned(&sl.size));
	}
	return result;
}

/*
 * check are there any more safe-links left in the tree.
 */
static int safe_link_iter_finished(struct safe_link_context *ctx)
{
	return get_key_locality(&ctx->key) != safe_link_locality(ctx->tree);
}

/*
 * finish safe-link iteration.
 */
static void safe_link_iter_end(struct safe_link_context *ctx)
{
	/* nothing special */
}

/*
 * process single safe-link.
 */
static int process_safelink(struct super_block *super, reiser4_safe_link_t link,
			    reiser4_key * sdkey, oid_t oid, __u64 size)
{
	struct inode *inode;
	int result;

	/*
	 * obtain object inode by reiser4_iget(), then call object plugin
	 * ->safelink() method to do actual work, then delete safe-link on
	 * success.
	 */
	inode = reiser4_iget(super, sdkey, 1);
	if (!IS_ERR(inode)) {
		file_plugin *fplug;

		fplug = inode_file_plugin(inode);
		assert("nikita-3428", fplug != NULL);
		assert("", oid == get_inode_oid(inode));
		if (fplug->safelink != NULL) {
			/* reiser4_txn_restart_current is not necessary because
			 * mounting is signle thread. However, without it
			 * deadlock detection code will complain (see
			 * nikita-3361). */
			reiser4_txn_restart_current();
			result = fplug->safelink(inode, link, size);
		} else {
			warning("nikita-3430",
				"Cannot handle safelink for %lli",
				(unsigned long long)oid);
			reiser4_print_key("key", sdkey);
			result = 0;
		}
		if (result != 0) {
			warning("nikita-3431",
				"Error processing safelink for %lli: %i",
				(unsigned long long)oid, result);
		}
		reiser4_iget_complete(inode);
		iput(inode);
		if (result == 0) {
			result = safe_link_grab(reiser4_get_tree(super),
						BA_CAN_COMMIT);
			if (result == 0)
				result =
				    safe_link_del(reiser4_get_tree(super), oid,
						  link);
			safe_link_release(reiser4_get_tree(super));
			/*
			 * restart transaction: if there was large number of
			 * safe-links, their processing may fail to fit into
			 * single transaction.
			 */
			if (result == 0)
				reiser4_txn_restart_current();
		}
	} else
		result = PTR_ERR(inode);
	return result;
}

/*
 * iterate over all safe-links in the file-system processing them one by one.
 */
int process_safelinks(struct super_block *super)
{
	struct safe_link_context ctx;
	int result;

	if (rofs_super(super))
		/* do nothing on the read-only file system */
		return 0;
	safe_link_iter_begin(&get_super_private(super)->tree, &ctx);
	result = 0;
	do {
		result = safe_link_iter_next(&ctx);
		if (safe_link_iter_finished(&ctx) || result == -ENOENT) {
			result = 0;
			break;
		}
		if (result == 0)
			result = process_safelink(super, ctx.link,
						  &ctx.sdkey, ctx.oid,
						  ctx.size);
	} while (result == 0);
	safe_link_iter_end(&ctx);
	return result;
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
