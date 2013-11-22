/* Copyright 2005 by Hans Reiser, licensing governed by
 * reiser4/README */

#include "../inode.h"

/* return true, iff @coord points to the valid directory item that is part of
 * @inode directory. */
static int is_valid_dir_coord(struct inode *inode, coord_t *coord)
{
	return plugin_of_group(item_plugin_by_coord(coord),
			       DIR_ENTRY_ITEM_TYPE) &&
	       inode_file_plugin(inode)->owns_item(inode, coord);
}

/* compare two logical positions within the same directory */
static cmp_t dir_pos_cmp(const struct dir_pos *p1, const struct dir_pos *p2)
{
	cmp_t result;

	assert("nikita-2534", p1 != NULL);
	assert("nikita-2535", p2 != NULL);

	result = de_id_cmp(&p1->dir_entry_key, &p2->dir_entry_key);
	if (result == EQUAL_TO) {
		int diff;

		diff = p1->pos - p2->pos;
		result =
		    (diff < 0) ? LESS_THAN : (diff ? GREATER_THAN : EQUAL_TO);
	}
	return result;
}

/* see comment before reiser4_readdir_common() for overview of why "adjustment"
 * is necessary. */
static void
adjust_dir_pos(struct file *dir, struct readdir_pos *readdir_spot,
	       const struct dir_pos *mod_point, int adj)
{
	struct dir_pos *pos;

	/*
	 * new directory entry was added (adj == +1) or removed (adj == -1) at
	 * the @mod_point. Directory file descriptor @dir is doing readdir and
	 * is currently positioned at @readdir_spot. Latter has to be updated
	 * to maintain stable readdir.
	 */
	/* directory is positioned to the beginning. */
	if (readdir_spot->entry_no == 0)
		return;

	pos = &readdir_spot->position;
	switch (dir_pos_cmp(mod_point, pos)) {
	case LESS_THAN:
		/* @mod_pos is _before_ @readdir_spot, that is, entry was
		 * added/removed on the left (in key order) of current
		 * position. */
		/* logical number of directory entry readdir is "looking" at
		 * changes */
		readdir_spot->entry_no += adj;
		assert("nikita-2577",
		       ergo(dir != NULL, reiser4_get_dir_fpos(dir) + adj >= 0));
		if (de_id_cmp(&pos->dir_entry_key,
			      &mod_point->dir_entry_key) == EQUAL_TO) {
			assert("nikita-2575", mod_point->pos < pos->pos);
			/*
			 * if entry added/removed has the same key as current
			 * for readdir, update counter of duplicate keys in
			 * @readdir_spot.
			 */
			pos->pos += adj;
		}
		break;
	case GREATER_THAN:
		/* directory is modified after @pos: nothing to do. */
		break;
	case EQUAL_TO:
		/* cannot insert an entry readdir is looking at, because it
		   already exists. */
		assert("nikita-2576", adj < 0);
		/* directory entry to which @pos points to is being
		   removed.

		   NOTE-NIKITA: Right thing to do is to update @pos to point
		   to the next entry. This is complex (we are under spin-lock
		   for one thing). Just rewind it to the beginning. Next
		   readdir will have to scan the beginning of
		   directory. Proper solution is to use semaphore in
		   spin lock's stead and use rewind_right() here.

		   NOTE-NIKITA: now, semaphore is used, so...
		 */
		memset(readdir_spot, 0, sizeof *readdir_spot);
	}
}

/* scan all file-descriptors for this directory and adjust their
   positions respectively. Should be used by implementations of
   add_entry and rem_entry of dir plugin */
void reiser4_adjust_dir_file(struct inode *dir, const struct dentry *de,
			     int offset, int adj)
{
	reiser4_file_fsdata *scan;
	struct dir_pos mod_point;

	assert("nikita-2536", dir != NULL);
	assert("nikita-2538", de != NULL);
	assert("nikita-2539", adj != 0);

	build_de_id(dir, &de->d_name, &mod_point.dir_entry_key);
	mod_point.pos = offset;

	spin_lock_inode(dir);

	/*
	 * new entry was added/removed in directory @dir. Scan all file
	 * descriptors for @dir that are currently involved into @readdir and
	 * update them.
	 */

	list_for_each_entry(scan, get_readdir_list(dir), dir.linkage)
		adjust_dir_pos(scan->back, &scan->dir.readdir, &mod_point, adj);

	spin_unlock_inode(dir);
}

/*
 * traverse tree to start/continue readdir from the readdir position @pos.
 */
static int dir_go_to(struct file *dir, struct readdir_pos *pos, tap_t *tap)
{
	reiser4_key key;
	int result;
	struct inode *inode;

	assert("nikita-2554", pos != NULL);

	inode = dir->f_dentry->d_inode;
	result = inode_dir_plugin(inode)->build_readdir_key(dir, &key);
	if (result != 0)
		return result;
	result = reiser4_object_lookup(inode,
				       &key,
				       tap->coord,
				       tap->lh,
				       tap->mode,
				       FIND_EXACT,
				       LEAF_LEVEL, LEAF_LEVEL,
				       0, &tap->ra_info);
	if (result == CBK_COORD_FOUND)
		result = rewind_right(tap, (int)pos->position.pos);
	else {
		tap->coord->node = NULL;
		done_lh(tap->lh);
		result = RETERR(-EIO);
	}
	return result;
}

/*
 * handling of non-unique keys: calculate at what ordinal position within
 * sequence of directory items with identical keys @pos is.
 */
static int set_pos(struct inode *inode, struct readdir_pos *pos, tap_t *tap)
{
	int result;
	coord_t coord;
	lock_handle lh;
	tap_t scan;
	de_id *did;
	reiser4_key de_key;

	coord_init_zero(&coord);
	init_lh(&lh);
	reiser4_tap_init(&scan, &coord, &lh, ZNODE_READ_LOCK);
	reiser4_tap_copy(&scan, tap);
	reiser4_tap_load(&scan);
	pos->position.pos = 0;

	did = &pos->position.dir_entry_key;

	if (is_valid_dir_coord(inode, scan.coord)) {

		build_de_id_by_key(unit_key_by_coord(scan.coord, &de_key), did);

		while (1) {

			result = go_prev_unit(&scan);
			if (result != 0)
				break;

			if (!is_valid_dir_coord(inode, scan.coord)) {
				result = -EINVAL;
				break;
			}

			/* get key of directory entry */
			unit_key_by_coord(scan.coord, &de_key);
			if (de_id_key_cmp(did, &de_key) != EQUAL_TO) {
				/* duplicate-sequence is over */
				break;
			}
			pos->position.pos++;
		}
	} else
		result = RETERR(-ENOENT);
	reiser4_tap_relse(&scan);
	reiser4_tap_done(&scan);
	return result;
}

/*
 * "rewind" directory to @offset, i.e., set @pos and @tap correspondingly.
 */
static int dir_rewind(struct file *dir, loff_t *fpos, struct readdir_pos *pos, tap_t *tap)
{
	__u64 destination;
	__s64 shift;
	int result;
	struct inode *inode;
	loff_t dirpos;

	assert("nikita-2553", dir != NULL);
	assert("nikita-2548", pos != NULL);
	assert("nikita-2551", tap->coord != NULL);
	assert("nikita-2552", tap->lh != NULL);

	dirpos = reiser4_get_dir_fpos(dir, *fpos);
	shift = dirpos - pos->fpos;
	/* this is logical directory entry within @dir which we are rewinding
	 * to */
	destination = pos->entry_no + shift;

	inode = dir->f_dentry->d_inode;
	if (dirpos < 0)
		return RETERR(-EINVAL);
	else if (destination == 0ll || dirpos == 0) {
		/* rewind to the beginning of directory */
		memset(pos, 0, sizeof *pos);
		return dir_go_to(dir, pos, tap);
	} else if (destination >= inode->i_size)
		return RETERR(-ENOENT);

	if (shift < 0) {
		/* I am afraid of negative numbers */
		shift = -shift;
		/* rewinding to the left */
		if (shift <= (int)pos->position.pos) {
			/* destination is within sequence of entries with
			   duplicate keys. */
			result = dir_go_to(dir, pos, tap);
		} else {
			shift -= pos->position.pos;
			while (1) {
				/* repetitions: deadlock is possible when
				   going to the left. */
				result = dir_go_to(dir, pos, tap);
				if (result == 0) {
					result = rewind_left(tap, shift);
					if (result == -E_DEADLOCK) {
						reiser4_tap_done(tap);
						continue;
					}
				}
				break;
			}
		}
	} else {
		/* rewinding to the right */
		result = dir_go_to(dir, pos, tap);
		if (result == 0)
			result = rewind_right(tap, shift);
	}
	if (result == 0) {
		result = set_pos(inode, pos, tap);
		if (result == 0) {
			/* update pos->position.pos */
			pos->entry_no = destination;
			pos->fpos = dirpos;
		}
	}
	return result;
}

/*
 * Function that is called by common_readdir() on each directory entry while
 * doing readdir. ->filldir callback may block, so we had to release long term
 * lock while calling it. To avoid repeating tree traversal, seal is used. If
 * seal is broken, we return -E_REPEAT. Node is unlocked in this case.
 *
 * Whether node is unlocked in case of any other error is undefined. It is
 * guaranteed to be still locked if success (0) is returned.
 *
 * When ->filldir() wants no more, feed_entry() returns 1, and node is
 * unlocked.
 */
static int
feed_entry(tap_t *tap, struct dir_context *context)
{
	item_plugin *iplug;
	char *name;
	reiser4_key sd_key;
	int result;
	char buf[DE_NAME_BUF_LEN];
	char name_buf[32];
	char *local_name;
	unsigned file_type;
	seal_t seal;
	coord_t *coord;
	reiser4_key entry_key;

	coord = tap->coord;
	iplug = item_plugin_by_coord(coord);

	/* pointer to name within the node */
	name = iplug->s.dir.extract_name(coord, buf);
	assert("nikita-1371", name != NULL);

	/* key of object the entry points to */
	if (iplug->s.dir.extract_key(coord, &sd_key) != 0)
		return RETERR(-EIO);

	/* we must release longterm znode lock before calling filldir to avoid
	   deadlock which may happen if filldir causes page fault. So, copy
	   name to intermediate buffer */
	if (strlen(name) + 1 > sizeof(name_buf)) {
		local_name = kmalloc(strlen(name) + 1,
				     reiser4_ctx_gfp_mask_get());
		if (local_name == NULL)
			return RETERR(-ENOMEM);
	} else
		local_name = name_buf;

	strcpy(local_name, name);
	file_type = iplug->s.dir.extract_file_type(coord);

	unit_key_by_coord(coord, &entry_key);
	reiser4_seal_init(&seal, coord, &entry_key);

	longterm_unlock_znode(tap->lh);

	/*
	 * send information about directory entry to the ->filldir() filler
	 * supplied to us by caller (VFS).
	 *
	 * ->filldir is entitled to do weird things. For example, ->filldir
	 * supplied by knfsd re-enters file system. Make sure no locks are
	 * held.
	 */
	assert("nikita-3436", lock_stack_isclean(get_current_lock_stack()));

	reiser4_txn_restart_current();
	if (!dir_emit(context, name, (int)strlen(name),
		      /* inode number of object bounden by this entry */
		      oid_to_uino(get_key_objectid(&sd_key)), file_type))
		/* ->filldir() is satisfied. (no space in buffer, IOW) */
		result = 1;
	else
		result = reiser4_seal_validate(&seal, coord, &entry_key,
					       tap->lh, tap->mode,
					       ZNODE_LOCK_HIPRI);

	if (local_name != name_buf)
		kfree(local_name);

	return result;
}

static void move_entry(struct readdir_pos *pos, coord_t *coord)
{
	reiser4_key de_key;
	de_id *did;

	/* update @pos */
	++pos->entry_no;
	did = &pos->position.dir_entry_key;

	/* get key of directory entry */
	unit_key_by_coord(coord, &de_key);

	if (de_id_key_cmp(did, &de_key) == EQUAL_TO)
		/* we are within sequence of directory entries
		   with duplicate keys. */
		++pos->position.pos;
	else {
		pos->position.pos = 0;
		build_de_id_by_key(&de_key, did);
	}
	++pos->fpos;
}

/*
 *     STATELESS READDIR
 *
 * readdir support in reiser4 relies on ability to update readdir_pos embedded
 * into reiser4_file_fsdata on each directory modification (name insertion and
 * removal), see reiser4_readdir_common() function below. This obviously doesn't
 * work when reiser4 is accessed over NFS, because NFS doesn't keep any state
 * across client READDIR requests for the same directory.
 *
 * To address this we maintain a "pool" of detached reiser4_file_fsdata
 * (d_cursor). Whenever NFS readdir request comes, we detect this, and try to
 * find detached reiser4_file_fsdata corresponding to previous readdir
 * request. In other words, additional state is maintained on the
 * server. (This is somewhat contrary to the design goals of NFS protocol.)
 *
 * To efficiently detect when our ->readdir() method is called by NFS server,
 * dentry is marked as "stateless" in reiser4_decode_fh() (this is checked by
 * file_is_stateless() function).
 *
 * To find out d_cursor in the pool, we encode client id (cid) in the highest
 * bits of NFS readdir cookie: when first readdir request comes to the given
 * directory from the given client, cookie is set to 0. This situation is
 * detected, global cid_counter is incremented, and stored in highest bits of
 * all direntry offsets returned to the client, including last one. As the
 * only valid readdir cookie is one obtained as direntry->offset, we are
 * guaranteed that next readdir request (continuing current one) will have
 * current cid in the highest bits of starting readdir cookie. All d_cursors
 * are hashed into per-super-block hash table by (oid, cid) key.
 *
 * In addition d_cursors are placed into per-super-block radix tree where they
 * are keyed by oid alone. This is necessary to efficiently remove them during
 * rmdir.
 *
 * At last, currently unused d_cursors are linked into special list. This list
 * is used d_cursor_shrink to reclaim d_cursors on memory pressure.
 *
 */

/*
 * prepare for readdir.
 *
 * NOTE: @f->f_pos may be out-of-date (iterate() vs readdir()).
 *       @fpos is effective position.
 */
static int dir_readdir_init(struct file *f, loff_t* fpos, tap_t *tap,
			    struct readdir_pos **pos)
{
	struct inode *inode;
	reiser4_file_fsdata *fsdata;
	int result;

	assert("nikita-1359", f != NULL);
	inode = f->f_dentry->d_inode;
	assert("nikita-1360", inode != NULL);

	if (!S_ISDIR(inode->i_mode))
		return RETERR(-ENOTDIR);

	/* try to find detached readdir state */
	result = reiser4_attach_fsdata(f, fpos, inode);
	if (result != 0)
		return result;

	fsdata = reiser4_get_file_fsdata(f);
	assert("nikita-2571", fsdata != NULL);
	if (IS_ERR(fsdata))
		return PTR_ERR(fsdata);

	/* add file descriptor to the readdir list hanging of directory
	 * inode. This list is used to scan "readdirs-in-progress" while
	 * inserting or removing names in the directory. */
	spin_lock_inode(inode);
	if (list_empty_careful(&fsdata->dir.linkage))
		list_add(&fsdata->dir.linkage, get_readdir_list(inode));
	*pos = &fsdata->dir.readdir;
	spin_unlock_inode(inode);

	/* move @tap to the current position */
	return dir_rewind(f, fpos, *pos, tap);
}

/* this is implementation of vfs's llseek method of struct file_operations for
   typical directory
   See comment before reiser4_iterate_common() for explanation.
*/
loff_t reiser4_llseek_dir_common(struct file *file, loff_t off, int origin)
{
	reiser4_context *ctx;
	loff_t result;
	struct inode *inode;

	inode = file->f_dentry->d_inode;

	ctx = reiser4_init_context(inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	mutex_lock(&inode->i_mutex);

	/* update ->f_pos */
	result = default_llseek_unlocked(file, off, origin);
	if (result >= 0) {
		int ff;
		coord_t coord;
		lock_handle lh;
		tap_t tap;
		struct readdir_pos *pos;

		coord_init_zero(&coord);
		init_lh(&lh);
		reiser4_tap_init(&tap, &coord, &lh, ZNODE_READ_LOCK);

		ff = dir_readdir_init(file, &file->f_pos, &tap, &pos);
		reiser4_detach_fsdata(file);
		if (ff != 0)
			result = (loff_t) ff;
		reiser4_tap_done(&tap);
	}
	reiser4_detach_fsdata(file);
	mutex_unlock(&inode->i_mutex);

	reiser4_exit_context(ctx);
	return result;
}

/* this is common implementation of vfs's readdir method of struct
   file_operations

   readdir problems:

   readdir(2)/getdents(2) interface is based on implicit assumption that
   readdir can be restarted from any particular point by supplying file system
   with off_t-full of data. That is, file system fills ->d_off field in struct
   dirent and later user passes ->d_off to the seekdir(3), which is, actually,
   implemented by glibc as lseek(2) on directory.

   Reiser4 cannot restart readdir from 64 bits of data, because two last
   components of the key of directory entry are unknown, which given 128 bits:
   locality and type fields in the key of directory entry are always known, to
   start readdir() from given point objectid and offset fields have to be
   filled.

   Traditional UNIX API for scanning through directory
   (readdir/seekdir/telldir/opendir/closedir/rewindir/getdents) is based on the
   assumption that directory is structured very much like regular file, in
   particular, it is implied that each name within given directory (directory
   entry) can be uniquely identified by scalar offset and that such offset is
   stable across the life-time of the name is identifies.

   This is manifestly not so for reiser4. In reiser4 the only stable unique
   identifies for the directory entry is its key that doesn't fit into
   seekdir/telldir API.

   solution:

   Within each file descriptor participating in readdir-ing of directory
   plugin/dir/dir.h:readdir_pos is maintained. This structure keeps track of
   the "current" directory entry that file descriptor looks at. It contains a
   key of directory entry (plus some additional info to deal with non-unique
   keys that we wouldn't dwell onto here) and a logical position of this
   directory entry starting from the beginning of the directory, that is
   ordinal number of this entry in the readdir order.

   Obviously this logical position is not stable in the face of directory
   modifications. To work around this, on each addition or removal of directory
   entry all file descriptors for directory inode are scanned and their
   readdir_pos are updated accordingly (adjust_dir_pos()).
*/
int reiser4_iterate_common(struct file *f /* directory file being read */,
			   struct dir_context *context /* callback data passed to us by VFS */)
{
	reiser4_context *ctx;
	int result;
	struct inode *inode;
	coord_t coord;
	lock_handle lh;
	tap_t tap;
	struct readdir_pos *pos;

	assert("nikita-1359", f != NULL);
	inode = f->f_dentry->d_inode;
	assert("nikita-1360", inode != NULL);

	if (!S_ISDIR(inode->i_mode))
		return RETERR(-ENOTDIR);

	ctx = reiser4_init_context(inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	coord_init_zero(&coord);
	init_lh(&lh);
	reiser4_tap_init(&tap, &coord, &lh, ZNODE_READ_LOCK);

	reiser4_readdir_readahead_init(inode, &tap);

repeat:
	result = dir_readdir_init(f, &context->pos, &tap, &pos);
	if (result == 0) {
		result = reiser4_tap_load(&tap);
		/* scan entries one by one feeding them to @filld */
		while (result == 0) {
			coord_t *coord;

			coord = tap.coord;
			assert("nikita-2572", coord_is_existing_unit(coord));
			assert("nikita-3227", is_valid_dir_coord(inode, coord));

			result = feed_entry(&tap, context);
			if (result > 0) {
				break;
			} else if (result == 0) {
				++context->pos;
				result = go_next_unit(&tap);
				if (result == -E_NO_NEIGHBOR ||
				    result == -ENOENT) {
					result = 0;
					break;
				} else if (result == 0) {
					if (is_valid_dir_coord(inode, coord))
						move_entry(pos, coord);
					else
						break;
				}
			} else if (result == -E_REPEAT) {
				/* feed_entry() had to restart. */
				++context->pos;
				reiser4_tap_relse(&tap);
				goto repeat;
			} else
				warning("vs-1617",
					"reiser4_readdir_common: unexpected error %d",
					result);
		}
		reiser4_tap_relse(&tap);

		if (result >= 0)
			f->f_version = inode->i_version;
	} else if (result == -E_NO_NEIGHBOR || result == -ENOENT)
		result = 0;
	reiser4_tap_done(&tap);
	reiser4_detach_fsdata(f);

	/* try to update directory's atime */
	if (reiser4_grab_space_force(inode_file_plugin(inode)->estimate.update(inode),
			       BA_CAN_COMMIT) != 0)
		warning("", "failed to update atime on readdir: %llu",
			get_inode_oid(inode));
	else
		file_accessed(f);

	context_set_commit_async(ctx);
	reiser4_exit_context(ctx);

	return (result <= 0) ? result : 0;
}

/*
 * Local variables:
 * c-indentation-style: "K&R"
 * mode-name: "LC"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 79
 * End:
 */
