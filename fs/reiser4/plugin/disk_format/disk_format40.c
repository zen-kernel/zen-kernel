/* Copyright 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

#include "../../debug.h"
#include "../../dformat.h"
#include "../../key.h"
#include "../node/node.h"
#include "../space/space_allocator.h"
#include "disk_format40.h"
#include "../plugin.h"
#include "../../txnmgr.h"
#include "../../jnode.h"
#include "../../tree.h"
#include "../../super.h"
#include "../../wander.h"
#include "../../inode.h"
#include "../../ktxnmgrd.h"
#include "../../status_flags.h"

#include <linux/types.h>	/* for __u??  */
#include <linux/fs.h>		/* for struct super_block  */
#include <linux/buffer_head.h>

/* reiser 4.0 default disk layout */

/* Amount of free blocks needed to perform release_format40 when fs gets
   mounted RW: 1 for SB, 1 for non-leaves in overwrite set, 2 for tx header
   & tx record. */
#define RELEASE_RESERVED 4

/* The greatest supported format40 version number */
#define FORMAT40_VERSION PLUGIN_LIBRARY_VERSION

/* This flag indicates that backup should be updated
   (the update is performed by fsck) */
#define FORMAT40_UPDATE_BACKUP (1 << 31)

/* functions to access fields of format40_disk_super_block */
static __u64 get_format40_block_count(const format40_disk_super_block * sb)
{
	return le64_to_cpu(get_unaligned(&sb->block_count));
}

static __u64 get_format40_free_blocks(const format40_disk_super_block * sb)
{
	return le64_to_cpu(get_unaligned(&sb->free_blocks));
}

static __u64 get_format40_root_block(const format40_disk_super_block * sb)
{
	return le64_to_cpu(get_unaligned(&sb->root_block));
}

static __u16 get_format40_tree_height(const format40_disk_super_block * sb)
{
	return le16_to_cpu(get_unaligned(&sb->tree_height));
}

static __u64 get_format40_file_count(const format40_disk_super_block * sb)
{
	return le64_to_cpu(get_unaligned(&sb->file_count));
}

static __u64 get_format40_oid(const format40_disk_super_block * sb)
{
	return le64_to_cpu(get_unaligned(&sb->oid));
}

static __u32 get_format40_mkfs_id(const format40_disk_super_block * sb)
{
	return le32_to_cpu(get_unaligned(&sb->mkfs_id));
}

static __u64 get_format40_flags(const format40_disk_super_block * sb)
{
	return le64_to_cpu(get_unaligned(&sb->flags));
}

static __u32 get_format40_version(const format40_disk_super_block * sb)
{
	return le32_to_cpu(get_unaligned(&sb->version)) &
		~FORMAT40_UPDATE_BACKUP;
}

static int update_backup_version(const format40_disk_super_block * sb)
{
	return (le32_to_cpu(get_unaligned(&sb->version)) &
		FORMAT40_UPDATE_BACKUP);
}

static int update_disk_version(const format40_disk_super_block * sb)
{
	return (get_format40_version(sb) < FORMAT40_VERSION);
}

static int incomplete_compatibility(const format40_disk_super_block * sb)
{
	return (get_format40_version(sb) > FORMAT40_VERSION);
}

static format40_super_info *get_sb_info(struct super_block *super)
{
	return &get_super_private(super)->u.format40;
}

static int consult_diskmap(struct super_block *s)
{
	format40_super_info *info;
	journal_location *jloc;

	info = get_sb_info(s);
	jloc = &get_super_private(s)->jloc;
	/* Default format-specific locations, if there is nothing in
	 * diskmap */
	jloc->footer = FORMAT40_JOURNAL_FOOTER_BLOCKNR;
	jloc->header = FORMAT40_JOURNAL_HEADER_BLOCKNR;
	info->loc.super = FORMAT40_OFFSET / s->s_blocksize;
#ifdef CONFIG_REISER4_BADBLOCKS
	reiser4_get_diskmap_value(FORMAT40_PLUGIN_DISKMAP_ID, FORMAT40_JF,
				  &jloc->footer);
	reiser4_get_diskmap_value(FORMAT40_PLUGIN_DISKMAP_ID, FORMAT40_JH,
				  &jloc->header);
	reiser4_get_diskmap_value(FORMAT40_PLUGIN_DISKMAP_ID, FORMAT40_SUPER,
				  &info->loc.super);
#endif
	return 0;
}

/* find any valid super block of disk_format40 (even if the first
   super block is destroyed), will change block numbers of actual journal header/footer (jf/jh)
   if needed */
static struct buffer_head *find_a_disk_format40_super_block(struct super_block
							    *s)
{
	struct buffer_head *super_bh;
	format40_disk_super_block *disk_sb;
	format40_super_info *info;

	assert("umka-487", s != NULL);

	info = get_sb_info(s);

	super_bh = sb_bread(s, info->loc.super);
	if (super_bh == NULL)
		return ERR_PTR(RETERR(-EIO));

	disk_sb = (format40_disk_super_block *) super_bh->b_data;
	if (strncmp(disk_sb->magic, FORMAT40_MAGIC, sizeof(FORMAT40_MAGIC))) {
		brelse(super_bh);
		return ERR_PTR(RETERR(-EINVAL));
	}

	reiser4_set_block_count(s, le64_to_cpu(get_unaligned(&disk_sb->block_count)));
	reiser4_set_data_blocks(s, le64_to_cpu(get_unaligned(&disk_sb->block_count)) -
				le64_to_cpu(get_unaligned(&disk_sb->free_blocks)));
	reiser4_set_free_blocks(s, le64_to_cpu(get_unaligned(&disk_sb->free_blocks)));

	return super_bh;
}

/* find the most recent version of super block. This is called after journal is
   replayed */
static struct buffer_head *read_super_block(struct super_block *s UNUSED_ARG)
{
	/* Here the most recent superblock copy has to be read. However, as
	   journal replay isn't complete, we are using
	   find_a_disk_format40_super_block() function. */
	return find_a_disk_format40_super_block(s);
}

static int get_super_jnode(struct super_block *s)
{
	reiser4_super_info_data *sbinfo = get_super_private(s);
	jnode *sb_jnode;
	int ret;

	sb_jnode = reiser4_alloc_io_head(&get_sb_info(s)->loc.super);

	ret = jload(sb_jnode);

	if (ret) {
		reiser4_drop_io_head(sb_jnode);
		return ret;
	}

	pin_jnode_data(sb_jnode);
	jrelse(sb_jnode);

	sbinfo->u.format40.sb_jnode = sb_jnode;

	return 0;
}

static void done_super_jnode(struct super_block *s)
{
	jnode *sb_jnode = get_super_private(s)->u.format40.sb_jnode;

	if (sb_jnode) {
		unpin_jnode_data(sb_jnode);
		reiser4_drop_io_head(sb_jnode);
	}
}

typedef enum format40_init_stage {
	NONE_DONE = 0,
	CONSULT_DISKMAP,
	FIND_A_SUPER,
	INIT_JOURNAL_INFO,
	INIT_STATUS,
	JOURNAL_REPLAY,
	READ_SUPER,
	KEY_CHECK,
	INIT_OID,
	INIT_TREE,
	JOURNAL_RECOVER,
	INIT_SA,
	INIT_JNODE,
	ALL_DONE
} format40_init_stage;

static format40_disk_super_block *copy_sb(const struct buffer_head *super_bh)
{
	format40_disk_super_block *sb_copy;

	sb_copy = kmalloc(sizeof(format40_disk_super_block),
			  reiser4_ctx_gfp_mask_get());
	if (sb_copy == NULL)
		return ERR_PTR(RETERR(-ENOMEM));
	memcpy(sb_copy, ((format40_disk_super_block *) super_bh->b_data),
	       sizeof(format40_disk_super_block));
	return sb_copy;
}

static int check_key_format(const format40_disk_super_block *sb_copy)
{
	if (!equi(REISER4_LARGE_KEY,
		  get_format40_flags(sb_copy) & (1 << FORMAT40_LARGE_KEYS))) {
		warning("nikita-3228", "Key format mismatch. "
			"Only %s keys are supported.",
			REISER4_LARGE_KEY ? "large" : "small");
		return RETERR(-EINVAL);
	}
	return 0;
}

/**
 * try_init_format40
 * @super:
 * @stage:
 *
 */
static int try_init_format40(struct super_block *super,
			     format40_init_stage *stage)
{
	int result;
	struct buffer_head *super_bh;
	reiser4_super_info_data *sbinfo;
	format40_disk_super_block *sb_copy;
	tree_level height;
	reiser4_block_nr root_block;
	node_plugin *nplug;

	assert("vs-475", super != NULL);
	assert("vs-474", get_super_private(super));

	*stage = NONE_DONE;

	result = consult_diskmap(super);
	if (result)
		return result;
	*stage = CONSULT_DISKMAP;

	super_bh = find_a_disk_format40_super_block(super);
	if (IS_ERR(super_bh))
		return PTR_ERR(super_bh);
	brelse(super_bh);
	*stage = FIND_A_SUPER;

	/* ok, we are sure that filesystem format is a format40 format */

	/* map jnodes for journal control blocks (header, footer) to disk  */
	result = reiser4_init_journal_info(super);
	if (result)
		return result;
	*stage = INIT_JOURNAL_INFO;

	/* ok, we are sure that filesystem format is a format40 format */
	/* Now check it's state */
	result = reiser4_status_init(FORMAT40_STATUS_BLOCKNR);
	if (result != 0 && result != -EINVAL)
		/* -EINVAL means there is no magic, so probably just old
		 * fs. */
		return result;
	*stage = INIT_STATUS;

	result = reiser4_status_query(NULL, NULL);
	if (result == REISER4_STATUS_MOUNT_WARN)
		notice("vpf-1363", "Warning: mounting %s with errors.",
		       super->s_id);
	if (result == REISER4_STATUS_MOUNT_RO)
		notice("vpf-1364", "Warning: mounting %s with fatal errors,"
		       " forcing read-only mount.", super->s_id);
	result = reiser4_journal_replay(super);
	if (result)
		return result;
	*stage = JOURNAL_REPLAY;

	super_bh = read_super_block(super);
	if (IS_ERR(super_bh))
		return PTR_ERR(super_bh);
	*stage = READ_SUPER;

	/* allocate and make a copy of format40_disk_super_block */
	sb_copy = copy_sb(super_bh);
	brelse(super_bh);

	if (IS_ERR(sb_copy))
		return PTR_ERR(sb_copy);
	printk("reiser4: %s: found disk format 4.0.%u.\n",
	       super->s_id,
	       get_format40_version(sb_copy));
	if (incomplete_compatibility(sb_copy))
		printk("reiser4: Warning: The last completely supported "
		       "version of disk format40 is %u. Some objects of "
		       "the semantic tree can be unaccessible.\n",
		       FORMAT40_VERSION);
	/* make sure that key format of kernel and filesystem match */
	result = check_key_format(sb_copy);
	if (result) {
		kfree(sb_copy);
		return result;
	}
	*stage = KEY_CHECK;

	result = oid_init_allocator(super, get_format40_file_count(sb_copy),
				    get_format40_oid(sb_copy));
	if (result) {
		kfree(sb_copy);
		return result;
	}
	*stage = INIT_OID;

	/* get things necessary to init reiser4_tree */
	root_block = get_format40_root_block(sb_copy);
	height = get_format40_tree_height(sb_copy);
	nplug = node_plugin_by_id(NODE40_ID);

	/* initialize reiser4_super_info_data */
	sbinfo = get_super_private(super);
	assert("", sbinfo->tree.super == super);
	/* init reiser4_tree for the filesystem */
	result = reiser4_init_tree(&sbinfo->tree, &root_block, height, nplug);
	if (result) {
		kfree(sb_copy);
		return result;
	}
	*stage = INIT_TREE;

	/*
	 * initialize reiser4_super_info_data with data from format40 super
	 * block
	 */
	sbinfo->default_uid = 0;
	sbinfo->default_gid = 0;
	sbinfo->mkfs_id = get_format40_mkfs_id(sb_copy);
	/* number of blocks in filesystem and reserved space */
	reiser4_set_block_count(super, get_format40_block_count(sb_copy));
	sbinfo->blocks_free = get_format40_free_blocks(sb_copy);
	sbinfo->version = get_format40_version(sb_copy);
	kfree(sb_copy);

	if (update_backup_version(sb_copy))
		printk("reiser4: Warning: metadata backup is not updated. "
		       "Please run 'fsck.reiser4 --fix' on %s.\n",
		       super->s_id);

	sbinfo->fsuid = 0;
	sbinfo->fs_flags |= (1 << REISER4_ADG);	/* hard links for directories
						 * are not supported */
	sbinfo->fs_flags |= (1 << REISER4_ONE_NODE_PLUGIN);	/* all nodes in
								 * layout 40 are
								 * of one
								 * plugin */
	/* sbinfo->tmgr is initialized already */

	/* recover sb data which were logged separately from sb block */

	/* NOTE-NIKITA: reiser4_journal_recover_sb_data() calls
	 * oid_init_allocator() and reiser4_set_free_blocks() with new
	 * data. What's the reason to call them above? */
	result = reiser4_journal_recover_sb_data(super);
	if (result != 0)
		return result;
	*stage = JOURNAL_RECOVER;

	/*
	 * Set number of used blocks.  The number of used blocks is not stored
	 * neither in on-disk super block nor in the journal footer blocks.  At
	 * this moment actual values of total blocks and free block counters
	 * are set in the reiser4 super block (in-memory structure) and we can
	 * calculate number of used blocks from them.
	 */
	reiser4_set_data_blocks(super,
				reiser4_block_count(super) -
				reiser4_free_blocks(super));

#if REISER4_DEBUG
	sbinfo->min_blocks_used = 16 /* reserved area */  +
		2 /* super blocks */  +
		2 /* journal footer and header */ ;
#endif

	/* init disk space allocator */
	result = sa_init_allocator(reiser4_get_space_allocator(super),
				   super, NULL);
	if (result)
		return result;
	*stage = INIT_SA;

	result = get_super_jnode(super);
	if (result == 0)
		*stage = ALL_DONE;
	return result;
}

/* plugin->u.format.get_ready */
int init_format_format40(struct super_block *s, void *data UNUSED_ARG)
{
	int result;
	format40_init_stage stage;

	result = try_init_format40(s, &stage);
	switch (stage) {
	case ALL_DONE:
		assert("nikita-3458", result == 0);
		break;
	case INIT_JNODE:
		done_super_jnode(s);
	case INIT_SA:
		sa_destroy_allocator(reiser4_get_space_allocator(s), s);
	case JOURNAL_RECOVER:
	case INIT_TREE:
		reiser4_done_tree(&get_super_private(s)->tree);
	case INIT_OID:
	case KEY_CHECK:
	case READ_SUPER:
	case JOURNAL_REPLAY:
	case INIT_STATUS:
		reiser4_status_finish();
	case INIT_JOURNAL_INFO:
		reiser4_done_journal_info(s);
	case FIND_A_SUPER:
	case CONSULT_DISKMAP:
	case NONE_DONE:
		break;
	default:
		impossible("nikita-3457", "init stage: %i", stage);
	}

	if (!rofs_super(s) && reiser4_free_blocks(s) < RELEASE_RESERVED)
		return RETERR(-ENOSPC);

	return result;
}

static void pack_format40_super(const struct super_block *s, char *data)
{
	format40_disk_super_block *super_data =
	    (format40_disk_super_block *) data;

	reiser4_super_info_data *sbinfo = get_super_private(s);

	assert("zam-591", data != NULL);

	put_unaligned(cpu_to_le64(reiser4_free_committed_blocks(s)),
		      &super_data->free_blocks);

	put_unaligned(cpu_to_le64(sbinfo->tree.root_block),
		      &super_data->root_block);

	put_unaligned(cpu_to_le64(oid_next(s)),
		      &super_data->oid);

	put_unaligned(cpu_to_le64(oids_used(s)),
		      &super_data->file_count);

	put_unaligned(cpu_to_le16(sbinfo->tree.height),
		      &super_data->tree_height);

	if (update_disk_version(super_data)) {
		__u32 version = FORMAT40_VERSION | FORMAT40_UPDATE_BACKUP;

		put_unaligned(cpu_to_le32(version), &super_data->version);
	}
}

/* plugin->u.format.log_super
   return a jnode which should be added to transaction when the super block
   gets logged */
jnode *log_super_format40(struct super_block *s)
{
	jnode *sb_jnode;

	sb_jnode = get_super_private(s)->u.format40.sb_jnode;

	jload(sb_jnode);

	pack_format40_super(s, jdata(sb_jnode));

	jrelse(sb_jnode);

	return sb_jnode;
}

/* plugin->u.format.release */
int release_format40(struct super_block *s)
{
	int ret;
	reiser4_super_info_data *sbinfo;

	sbinfo = get_super_private(s);
	assert("zam-579", sbinfo != NULL);

	if (!rofs_super(s)) {
		ret = reiser4_capture_super_block(s);
		if (ret != 0)
			warning("vs-898",
				"reiser4_capture_super_block failed: %d",
				ret);

		ret = txnmgr_force_commit_all(s, 1);
		if (ret != 0)
			warning("jmacd-74438", "txn_force failed: %d", ret);

		all_grabbed2free();
	}

	sa_destroy_allocator(&sbinfo->space_allocator, s);
	reiser4_done_journal_info(s);
	done_super_jnode(s);

	rcu_barrier();
	reiser4_done_tree(&sbinfo->tree);
	/* call finish_rcu(), because some znode were "released" in
	 * reiser4_done_tree(). */
	rcu_barrier();

	return 0;
}

#define FORMAT40_ROOT_LOCALITY 41
#define FORMAT40_ROOT_OBJECTID 42

/* plugin->u.format.root_dir_key */
const reiser4_key *root_dir_key_format40(const struct super_block *super
					 UNUSED_ARG)
{
	static const reiser4_key FORMAT40_ROOT_DIR_KEY = {
		.el = {
			__constant_cpu_to_le64((FORMAT40_ROOT_LOCALITY << 4) | KEY_SD_MINOR),
#if REISER4_LARGE_KEY
			ON_LARGE_KEY(0ull,)
#endif
			__constant_cpu_to_le64(FORMAT40_ROOT_OBJECTID),
			0ull
		}
	};

	return &FORMAT40_ROOT_DIR_KEY;
}

/* plugin->u.format.check_open.
   Check the opened object for validness. For now it checks for the valid oid &
   locality only, can be improved later and it its work may depend on the mount
   options. */
int check_open_format40(const struct inode *object)
{
	oid_t max, oid;

	max = oid_next(object->i_sb) - 1;

	/* Check the oid. */
	oid = get_inode_oid(object);
	if (oid > max) {
		warning("vpf-1360", "The object with the oid %llu "
			"greater then the max used oid %llu found.",
			(unsigned long long)oid, (unsigned long long)max);

		return RETERR(-EIO);
	}

	/* Check the locality. */
	oid = reiser4_inode_data(object)->locality_id;
	if (oid > max) {
		warning("vpf-1361", "The object with the locality %llu "
			"greater then the max used oid %llu found.",
			(unsigned long long)oid, (unsigned long long)max);

		return RETERR(-EIO);
	}

	return 0;
}

/* plugin->u.format.version_update.
   Perform all version update operations from the on-disk
   format40_disk_super_block.version on disk to FORMAT40_VERSION.
 */
int version_update_format40(struct super_block *super) {
	txn_handle * trans;
	lock_handle lh;
	txn_atom *atom;
	int ret;

	/* Nothing to do if RO mount or the on-disk version is not less. */
	if (super->s_flags & MS_RDONLY)
 		return 0;

	if (get_super_private(super)->version >= FORMAT40_VERSION)
		return 0;

	printk("reiser4: Updating disk format to 4.0.%u. The reiser4 metadata "
	       "backup is left unchanged. Please run 'fsck.reiser4 --fix' "
	       "on %s to update it too.\n", FORMAT40_VERSION, super->s_id);

	/* Mark the uber znode dirty to call log_super on write_logs. */
	init_lh(&lh);
	ret = get_uber_znode(reiser4_get_tree(super), ZNODE_WRITE_LOCK,
			     ZNODE_LOCK_HIPRI, &lh);
	if (ret != 0)
		return ret;

	znode_make_dirty(lh.node);
	done_lh(&lh);

	/* Update the backup blocks. */

	/* Force write_logs immediately. */
	trans = get_current_context()->trans;
	atom = get_current_atom_locked();
	assert("vpf-1906", atom != NULL);

	spin_lock_txnh(trans);
	return force_commit_atom(trans);
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
