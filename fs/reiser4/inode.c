/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
   reiser4/README */

/* Inode specific operations. */

#include "forward.h"
#include "debug.h"
#include "key.h"
#include "kassign.h"
#include "coord.h"
#include "seal.h"
#include "dscale.h"
#include "plugin/item/item.h"
#include "plugin/security/perm.h"
#include "plugin/plugin.h"
#include "plugin/object.h"
#include "znode.h"
#include "vfs_ops.h"
#include "inode.h"
#include "super.h"
#include "reiser4.h"

#include <linux/fs.h>		/* for struct super_block,  address_space */

/* return reiser4 internal tree which inode belongs to */
/* Audited by: green(2002.06.17) */
reiser4_tree *reiser4_tree_by_inode(const struct inode *inode/* inode queried*/)
{
	assert("nikita-256", inode != NULL);
	assert("nikita-257", inode->i_sb != NULL);
	return reiser4_get_tree(inode->i_sb);
}

/* return reiser4-specific inode flags */
static inline unsigned long *inode_flags(const struct inode *const inode)
{
	assert("nikita-2842", inode != NULL);
	return &reiser4_inode_data(inode)->flags;
}

/* set reiser4-specific flag @f in @inode */
void reiser4_inode_set_flag(struct inode *inode, reiser4_file_plugin_flags f)
{
	assert("nikita-2248", inode != NULL);
	set_bit((int)f, inode_flags(inode));
}

/* clear reiser4-specific flag @f in @inode */
void reiser4_inode_clr_flag(struct inode *inode, reiser4_file_plugin_flags f)
{
	assert("nikita-2250", inode != NULL);
	clear_bit((int)f, inode_flags(inode));
}

/* true if reiser4-specific flag @f is set in @inode */
int reiser4_inode_get_flag(const struct inode *inode,
			   reiser4_file_plugin_flags f)
{
	assert("nikita-2251", inode != NULL);
	return test_bit((int)f, inode_flags(inode));
}

/* convert oid to inode number */
ino_t oid_to_ino(oid_t oid)
{
	return (ino_t) oid;
}

/* convert oid to user visible inode number */
ino_t oid_to_uino(oid_t oid)
{
	/* reiser4 object is uniquely identified by oid which is 64 bit
	   quantity. Kernel in-memory inode is indexed (in the hash table) by
	   32 bit i_ino field, but this is not a problem, because there is a
	   way to further distinguish inodes with identical inode numbers
	   (find_actor supplied to iget()).

	   But user space expects unique 32 bit inode number. Obviously this
	   is impossible. Work-around is to somehow hash oid into user visible
	   inode number.
	 */
	oid_t max_ino = (ino_t) ~0;

	if (REISER4_INO_IS_OID || (oid <= max_ino))
		return oid;
	else
		/* this is remotely similar to algorithm used to find next pid
		   to use for process: after wrap-around start from some
		   offset rather than from 0. Idea is that there are some long
		   living objects with which we don't want to collide.
		 */
		return REISER4_UINO_SHIFT + ((oid - max_ino) & (max_ino >> 1));
}

/* check that "inode" is on reiser4 file-system */
int is_reiser4_inode(const struct inode *inode/* inode queried */)
{
	return inode != NULL && is_reiser4_super(inode->i_sb);
}

/* Maximal length of a name that can be stored in directory @inode.

   This is used in check during file creation and lookup. */
int reiser4_max_filename_len(const struct inode *inode/* inode queried */)
{
	assert("nikita-287", is_reiser4_inode(inode));
	assert("nikita-1710", inode_dir_item_plugin(inode));
	if (inode_dir_item_plugin(inode)->s.dir.max_name_len)
		return inode_dir_item_plugin(inode)->s.dir.max_name_len(inode);
	else
		return 255;
}

#if REISER4_USE_COLLISION_LIMIT
/* Maximal number of hash collisions for this directory. */
int max_hash_collisions(const struct inode *dir/* inode queried */)
{
	assert("nikita-1711", dir != NULL);
	return reiser4_inode_data(dir)->plugin.max_collisions;
}
#endif  /*  REISER4_USE_COLLISION_LIMIT  */

/* Install file, inode, and address_space operation on @inode, depending on
   its mode. */
int setup_inode_ops(struct inode *inode /* inode to intialize */ ,
		    reiser4_object_create_data * data	/* parameters to create
							 * object */ )
{
	reiser4_super_info_data *sinfo;
	file_plugin *fplug;
	dir_plugin *dplug;

	fplug = inode_file_plugin(inode);
	dplug = inode_dir_plugin(inode);

	sinfo = get_super_private(inode->i_sb);

	switch (inode->i_mode & S_IFMT) {
	case S_IFSOCK:
	case S_IFBLK:
	case S_IFCHR:
	case S_IFIFO:
		{
			dev_t rdev;	/* to keep gcc happy */

			assert("vs-46", fplug != NULL);
			/* ugly hack with rdev */
			if (data == NULL) {
				rdev = inode->i_rdev;
				inode->i_rdev = 0;
			} else
				rdev = data->rdev;
			inode->i_blocks = 0;
			assert("vs-42", fplug->h.id == SPECIAL_FILE_PLUGIN_ID);
			inode->i_op = file_plugins[fplug->h.id].inode_ops;
			/* initialize inode->i_fop and inode->i_rdev for block
			   and char devices */
			init_special_inode(inode, inode->i_mode, rdev);
			/* all address space operations are null */
			inode->i_mapping->a_ops =
			    file_plugins[fplug->h.id].as_ops;
			break;
		}
	case S_IFLNK:
		assert("vs-46", fplug != NULL);
		assert("vs-42", fplug->h.id == SYMLINK_FILE_PLUGIN_ID);
		inode->i_op = file_plugins[fplug->h.id].inode_ops;
		inode->i_fop = NULL;
		/* all address space operations are null */
		inode->i_mapping->a_ops = file_plugins[fplug->h.id].as_ops;
		break;
	case S_IFDIR:
		assert("vs-46", dplug != NULL);
		assert("vs-43", (dplug->h.id == HASHED_DIR_PLUGIN_ID ||
				 dplug->h.id == SEEKABLE_HASHED_DIR_PLUGIN_ID));
		inode->i_op = dir_plugins[dplug->h.id].inode_ops;
		inode->i_fop = dir_plugins[dplug->h.id].file_ops;
		inode->i_mapping->a_ops = dir_plugins[dplug->h.id].as_ops;
		break;
	case S_IFREG:
		assert("vs-46", fplug != NULL);
		assert("vs-43", (fplug->h.id == UNIX_FILE_PLUGIN_ID ||
				 fplug->h.id == CRYPTCOMPRESS_FILE_PLUGIN_ID));
		inode->i_op = file_plugins[fplug->h.id].inode_ops;
		inode->i_fop = file_plugins[fplug->h.id].file_ops;
		inode->i_mapping->a_ops = file_plugins[fplug->h.id].as_ops;
		break;
	default:
		warning("nikita-291", "wrong file mode: %o for %llu",
			inode->i_mode,
			(unsigned long long)get_inode_oid(inode));
		reiser4_make_bad_inode(inode);
		return RETERR(-EINVAL);
	}
	return 0;
}

/* Initialize inode from disk data. Called with inode locked.
   Return inode locked. */
static int init_inode(struct inode *inode /* inode to intialise */ ,
		      coord_t *coord/* coord of stat data */)
{
	int result;
	item_plugin *iplug;
	void *body;
	int length;
	reiser4_inode *state;

	assert("nikita-292", coord != NULL);
	assert("nikita-293", inode != NULL);

	coord_clear_iplug(coord);
	result = zload(coord->node);
	if (result)
		return result;
	iplug = item_plugin_by_coord(coord);
	body = item_body_by_coord(coord);
	length = item_length_by_coord(coord);

	assert("nikita-295", iplug != NULL);
	assert("nikita-296", body != NULL);
	assert("nikita-297", length > 0);

	/* inode is under I_LOCK now */

	state = reiser4_inode_data(inode);
	/* call stat-data plugin method to load sd content into inode */
	result = iplug->s.sd.init_inode(inode, body, length);
	set_plugin(&state->pset, PSET_SD, item_plugin_to_plugin(iplug));
	if (result == 0) {
		result = setup_inode_ops(inode, NULL);
		if (result == 0 && inode->i_sb->s_root &&
		    inode->i_sb->s_root->d_inode)
			result = finish_pset(inode);
	}
	zrelse(coord->node);
	return result;
}

/* read `inode' from the disk. This is what was previously in
   reiserfs_read_inode2().

   Must be called with inode locked. Return inode still locked.
*/
static int read_inode(struct inode *inode /* inode to read from disk */ ,
		      const reiser4_key * key /* key of stat data */ ,
		      int silent)
{
	int result;
	lock_handle lh;
	reiser4_inode *info;
	coord_t coord;

	assert("nikita-298", inode != NULL);
	assert("nikita-1945", !is_inode_loaded(inode));

	info = reiser4_inode_data(inode);
	assert("nikita-300", info->locality_id != 0);

	coord_init_zero(&coord);
	init_lh(&lh);
	/* locate stat-data in a tree and return znode locked */
	result = lookup_sd(inode, ZNODE_READ_LOCK, &coord, &lh, key, silent);
	assert("nikita-301", !is_inode_loaded(inode));
	if (result == 0) {
		/* use stat-data plugin to load sd into inode. */
		result = init_inode(inode, &coord);
		if (result == 0) {
			/* initialize stat-data seal */
			spin_lock_inode(inode);
			reiser4_seal_init(&info->sd_seal, &coord, key);
			info->sd_coord = coord;
			spin_unlock_inode(inode);

			/* call file plugin's method to initialize plugin
			 * specific part of inode */
			if (inode_file_plugin(inode)->init_inode_data)
				inode_file_plugin(inode)->init_inode_data(inode,
									  NULL,
									  0);
			/* load detached directory cursors for stateless
			 * directory readers (NFS). */
			reiser4_load_cursors(inode);

			/* Check the opened inode for consistency. */
			result =
			    get_super_private(inode->i_sb)->df_plug->
			    check_open(inode);
		}
	}
	/* lookup_sd() doesn't release coord because we want znode
	   stay read-locked while stat-data fields are accessed in
	   init_inode() */
	done_lh(&lh);

	if (result != 0)
		reiser4_make_bad_inode(inode);
	return result;
}

/* initialise new reiser4 inode being inserted into hash table. */
static int init_locked_inode(struct inode *inode /* new inode */ ,
			     void *opaque	/* key of stat data passed to
						* the iget5_locked as cookie */)
{
	reiser4_key *key;

	assert("nikita-1995", inode != NULL);
	assert("nikita-1996", opaque != NULL);
	key = opaque;
	set_inode_oid(inode, get_key_objectid(key));
	reiser4_inode_data(inode)->locality_id = get_key_locality(key);
	return 0;
}

/* reiser4_inode_find_actor() - "find actor" supplied by reiser4 to
   iget5_locked().

   This function is called by iget5_locked() to distinguish reiser4 inodes
   having the same inode numbers. Such inodes can only exist due to some error
   condition. One of them should be bad. Inodes with identical inode numbers
   (objectids) are distinguished by their packing locality.

*/
static int reiser4_inode_find_actor(struct inode *inode	/* inode from hash table
							 * to check */ ,
				    void *opaque        /* "cookie" passed to
						         * iget5_locked(). This
							 * is stat-data key */)
{
	reiser4_key *key;

	key = opaque;
	return
	    /* oid is unique, so first term is enough, actually. */
	    get_inode_oid(inode) == get_key_objectid(key) &&
	    /*
	     * also, locality should be checked, but locality is stored in
	     * the reiser4-specific part of the inode, and actor can be
	     * called against arbitrary inode that happened to be in this
	     * hash chain. Hence we first have to check that this is
	     * reiser4 inode at least. is_reiser4_inode() is probably too
	     * early to call, as inode may have ->i_op not yet
	     * initialised.
	     */
	    is_reiser4_super(inode->i_sb) &&
	    /*
	     * usually objectid is unique, but pseudo files use counter to
	     * generate objectid. All pseudo files are placed into special
	     * (otherwise unused) locality.
	     */
	    reiser4_inode_data(inode)->locality_id == get_key_locality(key);
}

/* hook for kmem_cache_create */
void loading_init_once(reiser4_inode * info)
{
	mutex_init(&info->loading);
}

/* for reiser4_alloc_inode */
void loading_alloc(reiser4_inode * info)
{
	assert("vs-1717", !mutex_is_locked(&info->loading));
}

/* for reiser4_destroy */
void loading_destroy(reiser4_inode * info)
{
	assert("vs-1717a", !mutex_is_locked(&info->loading));
}

static void loading_begin(reiser4_inode * info)
{
	mutex_lock(&info->loading);
}

static void loading_end(reiser4_inode * info)
{
	mutex_unlock(&info->loading);
}

/**
 * reiser4_iget - obtain inode via iget5_locked, read from disk if necessary
 * @super: super block of filesystem
 * @key: key of inode's stat-data
 * @silent:
 *
 * This is our helper function a la iget(). This is be called by
 * lookup_common() and reiser4_read_super(). Return inode locked or error
 * encountered.
 */
struct inode *reiser4_iget(struct super_block *super, const reiser4_key *key,
			   int silent)
{
	struct inode *inode;
	int result;
	reiser4_inode *info;

	assert("nikita-302", super != NULL);
	assert("nikita-303", key != NULL);

	result = 0;

	/* call iget(). Our ->read_inode() is dummy, so this will either
	   find inode in cache or return uninitialised inode */
	inode = iget5_locked(super,
			     (unsigned long)get_key_objectid(key),
			     reiser4_inode_find_actor,
			     init_locked_inode, (reiser4_key *) key);
	if (inode == NULL)
		return ERR_PTR(RETERR(-ENOMEM));
	if (is_bad_inode(inode)) {
		warning("nikita-304", "Bad inode found");
		reiser4_print_key("key", key);
		iput(inode);
		return ERR_PTR(RETERR(-EIO));
	}

	info = reiser4_inode_data(inode);

	/* Reiser4 inode state bit REISER4_LOADED is used to distinguish fully
	   loaded and initialized inode from just allocated inode. If
	   REISER4_LOADED bit is not set, reiser4_iget() completes loading under
	   info->loading.  The place in reiser4 which uses not initialized inode
	   is the reiser4 repacker, see repacker-related functions in
	   plugin/item/extent.c */
	if (!is_inode_loaded(inode)) {
		loading_begin(info);
		if (!is_inode_loaded(inode)) {
			/* locking: iget5_locked returns locked inode */
			assert("nikita-1941", !is_inode_loaded(inode));
			assert("nikita-1949",
			       reiser4_inode_find_actor(inode,
							(reiser4_key *) key));
			/* now, inode has objectid as ->i_ino and locality in
			   reiser4-specific part. This is enough for
			   read_inode() to read stat data from the disk */
			result = read_inode(inode, key, silent);
		} else
			loading_end(info);
	}

	if (inode->i_state & I_NEW)
		unlock_new_inode(inode);

	if (is_bad_inode(inode)) {
		assert("vs-1717", result != 0);
		loading_end(info);
		iput(inode);
		inode = ERR_PTR(result);
	} else if (REISER4_DEBUG) {
		reiser4_key found_key;

		assert("vs-1717", result == 0);
		build_sd_key(inode, &found_key);
		if (!keyeq(&found_key, key)) {
			warning("nikita-305", "Wrong key in sd");
			reiser4_print_key("sought for", key);
			reiser4_print_key("found", &found_key);
		}
		if (inode->i_nlink == 0) {
			warning("nikita-3559", "Unlinked inode found: %llu\n",
				(unsigned long long)get_inode_oid(inode));
		}
	}
	return inode;
}

/* reiser4_iget() may return not fully initialized inode, this function should
 * be called after one completes reiser4 inode initializing. */
void reiser4_iget_complete(struct inode *inode)
{
	assert("zam-988", is_reiser4_inode(inode));

	if (!is_inode_loaded(inode)) {
		reiser4_inode_set_flag(inode, REISER4_LOADED);
		loading_end(reiser4_inode_data(inode));
	}
}

void reiser4_make_bad_inode(struct inode *inode)
{
	assert("nikita-1934", inode != NULL);

	/* clear LOADED bit */
	reiser4_inode_clr_flag(inode, REISER4_LOADED);
	make_bad_inode(inode);
	return;
}

file_plugin *inode_file_plugin(const struct inode *inode)
{
	assert("nikita-1997", inode != NULL);
	return reiser4_inode_data(inode)->pset->file;
}

dir_plugin *inode_dir_plugin(const struct inode *inode)
{
	assert("nikita-1998", inode != NULL);
	return reiser4_inode_data(inode)->pset->dir;
}

formatting_plugin *inode_formatting_plugin(const struct inode *inode)
{
	assert("nikita-2000", inode != NULL);
	return reiser4_inode_data(inode)->pset->formatting;
}

hash_plugin *inode_hash_plugin(const struct inode *inode)
{
	assert("nikita-2001", inode != NULL);
	return reiser4_inode_data(inode)->pset->hash;
}

fibration_plugin *inode_fibration_plugin(const struct inode *inode)
{
	assert("nikita-2001", inode != NULL);
	return reiser4_inode_data(inode)->pset->fibration;
}

cipher_plugin *inode_cipher_plugin(const struct inode *inode)
{
	assert("edward-36", inode != NULL);
	return reiser4_inode_data(inode)->pset->cipher;
}

compression_plugin *inode_compression_plugin(const struct inode *inode)
{
	assert("edward-37", inode != NULL);
	return reiser4_inode_data(inode)->pset->compression;
}

compression_mode_plugin *inode_compression_mode_plugin(const struct inode *
						       inode)
{
	assert("edward-1330", inode != NULL);
	return reiser4_inode_data(inode)->pset->compression_mode;
}

cluster_plugin *inode_cluster_plugin(const struct inode *inode)
{
	assert("edward-1328", inode != NULL);
	return reiser4_inode_data(inode)->pset->cluster;
}

file_plugin *inode_create_plugin(const struct inode *inode)
{
	assert("edward-1329", inode != NULL);
	return reiser4_inode_data(inode)->pset->create;
}

digest_plugin *inode_digest_plugin(const struct inode *inode)
{
	assert("edward-86", inode != NULL);
	return reiser4_inode_data(inode)->pset->digest;
}

item_plugin *inode_sd_plugin(const struct inode *inode)
{
	assert("vs-534", inode != NULL);
	return reiser4_inode_data(inode)->pset->sd;
}

item_plugin *inode_dir_item_plugin(const struct inode *inode)
{
	assert("vs-534", inode != NULL);
	return reiser4_inode_data(inode)->pset->dir_item;
}

file_plugin *child_create_plugin(const struct inode *inode)
{
	assert("edward-1329", inode != NULL);
	return reiser4_inode_data(inode)->hset->create;
}

void inode_set_extension(struct inode *inode, sd_ext_bits ext)
{
	reiser4_inode *state;

	assert("nikita-2716", inode != NULL);
	assert("nikita-2717", ext < LAST_SD_EXTENSION);
	assert("nikita-3491", spin_inode_is_locked(inode));

	state = reiser4_inode_data(inode);
	state->extmask |= 1 << ext;
	/* force re-calculation of stat-data length on next call to
	   update_sd(). */
	reiser4_inode_clr_flag(inode, REISER4_SDLEN_KNOWN);
}

void inode_clr_extension(struct inode *inode, sd_ext_bits ext)
{
	reiser4_inode *state;

	assert("vpf-1926", inode != NULL);
	assert("vpf-1927", ext < LAST_SD_EXTENSION);
	assert("vpf-1928", spin_inode_is_locked(inode));

	state = reiser4_inode_data(inode);
	state->extmask &= ~(1 << ext);
	/* force re-calculation of stat-data length on next call to
	   update_sd(). */
	reiser4_inode_clr_flag(inode, REISER4_SDLEN_KNOWN);
}

void inode_check_scale_nolock(struct inode *inode, __u64 old, __u64 new)
{
	assert("edward-1287", inode != NULL);
	if (!dscale_fit(old, new))
		reiser4_inode_clr_flag(inode, REISER4_SDLEN_KNOWN);
	return;
}

void inode_check_scale(struct inode *inode, __u64 old, __u64 new)
{
	assert("nikita-2875", inode != NULL);
	spin_lock_inode(inode);
	inode_check_scale_nolock(inode, old, new);
	spin_unlock_inode(inode);
}

/*
 * initialize ->ordering field of inode. This field defines how file stat-data
 * and body is ordered within a tree with respect to other objects within the
 * same parent directory.
 */
void
init_inode_ordering(struct inode *inode,
		    reiser4_object_create_data * crd, int create)
{
	reiser4_key key;

	if (create) {
		struct inode *parent;

		parent = crd->parent;
		assert("nikita-3224", inode_dir_plugin(parent) != NULL);
		inode_dir_plugin(parent)->build_entry_key(parent,
							  &crd->dentry->d_name,
							  &key);
	} else {
		coord_t *coord;

		coord = &reiser4_inode_data(inode)->sd_coord;
		coord_clear_iplug(coord);
		/* safe to use ->sd_coord, because node is under long term
		 * lock */
		WITH_DATA(coord->node, item_key_by_coord(coord, &key));
	}

	set_inode_ordering(inode, get_key_ordering(&key));
}

znode *inode_get_vroot(struct inode *inode)
{
	reiser4_block_nr blk;
	znode *result;

	spin_lock_inode(inode);
	blk = reiser4_inode_data(inode)->vroot;
	spin_unlock_inode(inode);
	if (!disk_addr_eq(&UBER_TREE_ADDR, &blk))
		result = zlook(reiser4_tree_by_inode(inode), &blk);
	else
		result = NULL;
	return result;
}

void inode_set_vroot(struct inode *inode, znode *vroot)
{
	spin_lock_inode(inode);
	reiser4_inode_data(inode)->vroot = *znode_get_block(vroot);
	spin_unlock_inode(inode);
}

#if REISER4_DEBUG

void reiser4_inode_invariant(const struct inode *inode)
{
	assert("nikita-3077", spin_inode_is_locked(inode));
}

int inode_has_no_jnodes(reiser4_inode * r4_inode)
{
	return jnode_tree_by_reiser4_inode(r4_inode)->rnode == NULL &&
		r4_inode->nr_jnodes == 0;
}

#endif

/* true if directory is empty (only contains dot and dotdot) */
/* FIXME: shouldn't it be dir plugin method? */
int is_dir_empty(const struct inode *dir)
{
	assert("nikita-1976", dir != NULL);

	/* rely on our method to maintain directory i_size being equal to the
	   number of entries. */
	return dir->i_size <= 2 ? 0 : RETERR(-ENOTEMPTY);
}

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
