/* Copyright 2005 by Hans Reiser, licensing governed by
   reiser4/README */

/* this file contains typical implementations for most of methods of
   directory plugin
*/

#include "../inode.h"

int reiser4_find_entry(struct inode *dir, struct dentry *name,
	       lock_handle * , znode_lock_mode, reiser4_dir_entry_desc *);
int reiser4_lookup_name(struct inode *parent, struct dentry *dentry,
			reiser4_key * key);
void check_light_weight(struct inode *inode, struct inode *parent);

/* this is common implementation of get_parent method of dir plugin
   this is used by NFS kernel server to "climb" up directory tree to
   check permissions
 */
struct dentry *get_parent_common(struct inode *child)
{
	struct super_block *s;
	struct inode *parent;
	struct dentry dotdot;
	struct dentry *dentry;
	reiser4_key key;
	int result;

	/*
	 * lookup dotdot entry.
	 */

	s = child->i_sb;
	memset(&dotdot, 0, sizeof(dotdot));
	dotdot.d_name.name = "..";
	dotdot.d_name.len = 2;
	dotdot.d_op = &get_super_private(s)->ops.dentry;

	result = reiser4_lookup_name(child, &dotdot, &key);
	if (result != 0)
		return ERR_PTR(result);

	parent = reiser4_iget(s, &key, 1);
	if (!IS_ERR(parent)) {
		/*
		 * FIXME-NIKITA dubious: attributes are inherited from @child
		 * to @parent. But:
		 *
		 *     (*) this is the only this we can do
		 *
		 *     (*) attributes of light-weight object are inherited
		 *     from a parent through which object was looked up first,
		 *     so it is ambiguous anyway.
		 *
		 */
		check_light_weight(parent, child);
		reiser4_iget_complete(parent);
		dentry = d_obtain_alias(parent);
		if (!IS_ERR(dentry))
			dentry->d_op = &get_super_private(s)->ops.dentry;
	} else if (PTR_ERR(parent) == -ENOENT)
		dentry = ERR_PTR(RETERR(-ESTALE));
	else
		dentry = (void *)parent;
	return dentry;
}

/* this is common implementation of is_name_acceptable method of dir
   plugin
 */
int is_name_acceptable_common(const struct inode *inode, /* directory to check*/
			      const char *name UNUSED_ARG, /* name to check */
			      int len/* @name's length */)
{
	assert("nikita-733", inode != NULL);
	assert("nikita-734", name != NULL);
	assert("nikita-735", len > 0);

	return len <= reiser4_max_filename_len(inode);
}

/* there is no common implementation of build_entry_key method of dir
   plugin. See plugin/dir/hashed_dir.c:build_entry_key_hashed() or
   plugin/dir/seekable.c:build_entry_key_seekable() for example
*/

/* this is common implementation of build_readdir_key method of dir
   plugin
   see reiser4_readdir_common for more details
*/
int build_readdir_key_common(struct file *dir /* directory being read */ ,
			     reiser4_key * result/* where to store key */)
{
	reiser4_file_fsdata *fdata;
	struct inode *inode;

	assert("nikita-1361", dir != NULL);
	assert("nikita-1362", result != NULL);
	assert("nikita-1363", dir->f_path.dentry != NULL);
	inode = file_inode(dir);
	assert("nikita-1373", inode != NULL);

	fdata = reiser4_get_file_fsdata(dir);
	if (IS_ERR(fdata))
		return PTR_ERR(fdata);
	assert("nikita-1364", fdata != NULL);
	return extract_key_from_de_id(get_inode_oid(inode),
				      &fdata->dir.readdir.position.
				      dir_entry_key, result);

}

void reiser4_adjust_dir_file(struct inode *, const struct dentry *, int offset,
			     int adj);

/* this is common implementation of add_entry method of dir plugin
*/
int reiser4_add_entry_common(struct inode *object, /* directory to add new name
						    * in */
			     struct dentry *where,	/* new name */
			     reiser4_object_create_data * data, /* parameters of
								*  new object */
			     reiser4_dir_entry_desc * entry /* parameters of
							     * new directory
							     * entry */)
{
	int result;
	coord_t *coord;
	lock_handle lh;
	struct reiser4_dentry_fsdata *fsdata;
	reiser4_block_nr reserve;

	assert("nikita-1114", object != NULL);
	assert("nikita-1250", where != NULL);

	fsdata = reiser4_get_dentry_fsdata(where);
	if (unlikely(IS_ERR(fsdata)))
		return PTR_ERR(fsdata);

	reserve = inode_dir_plugin(object)->estimate.add_entry(object);
	if (reiser4_grab_space(reserve, BA_CAN_COMMIT))
		return RETERR(-ENOSPC);

	init_lh(&lh);
	coord = &fsdata->dec.entry_coord;
	coord_clear_iplug(coord);

	/* check for this entry in a directory. This is plugin method. */
	result = reiser4_find_entry(object, where, &lh, ZNODE_WRITE_LOCK,
				    entry);
	if (likely(result == -ENOENT)) {
		/* add new entry. Just pass control to the directory
		   item plugin. */
		assert("nikita-1709", inode_dir_item_plugin(object));
		assert("nikita-2230", coord->node == lh.node);
		reiser4_seal_done(&fsdata->dec.entry_seal);
		result =
		    inode_dir_item_plugin(object)->s.dir.add_entry(object,
								   coord, &lh,
								   where,
								   entry);
		if (result == 0) {
			reiser4_adjust_dir_file(object, where,
						fsdata->dec.pos + 1, +1);
			INODE_INC_FIELD(object, i_size);
		}
	} else if (result == 0) {
		assert("nikita-2232", coord->node == lh.node);
		result = RETERR(-EEXIST);
	}
	done_lh(&lh);

	return result;
}

/**
 * rem_entry - remove entry from directory item
 * @dir:
 * @dentry:
 * @entry:
 * @coord:
 * @lh:
 *
 * Checks that coordinate @coord is set properly and calls item plugin
 * method to cut entry.
 */
static int
rem_entry(struct inode *dir, struct dentry *dentry,
	  reiser4_dir_entry_desc * entry, coord_t *coord, lock_handle * lh)
{
	item_plugin *iplug;
	struct inode *child;

	iplug = inode_dir_item_plugin(dir);
	child = dentry->d_inode;
	assert("nikita-3399", child != NULL);

	/* check that we are really destroying an entry for @child */
	if (REISER4_DEBUG) {
		int result;
		reiser4_key key;

		result = iplug->s.dir.extract_key(coord, &key);
		if (result != 0)
			return result;
		if (get_key_objectid(&key) != get_inode_oid(child)) {
			warning("nikita-3397",
				"rem_entry: %#llx != %#llx\n",
				get_key_objectid(&key),
				(unsigned long long)get_inode_oid(child));
			return RETERR(-EIO);
		}
	}
	return iplug->s.dir.rem_entry(dir, &dentry->d_name, coord, lh, entry);
}

/**
 * reiser4_rem_entry_common - remove entry from a directory
 * @dir: directory to remove entry from
 * @where: name that is being removed
 * @entry: description of entry being removed
 *
 * This is common implementation of rem_entry method of dir plugin.
 */
int reiser4_rem_entry_common(struct inode *dir,
			     struct dentry *dentry,
			     reiser4_dir_entry_desc * entry)
{
	int result;
	coord_t *coord;
	lock_handle lh;
	struct reiser4_dentry_fsdata *fsdata;
	__u64 tograb;

	assert("nikita-1124", dir != NULL);
	assert("nikita-1125", dentry != NULL);

	tograb = inode_dir_plugin(dir)->estimate.rem_entry(dir);
	result = reiser4_grab_space(tograb, BA_CAN_COMMIT | BA_RESERVED);
	if (result != 0)
		return RETERR(-ENOSPC);

	init_lh(&lh);

	/* check for this entry in a directory. This is plugin method. */
	result = reiser4_find_entry(dir, dentry, &lh, ZNODE_WRITE_LOCK, entry);
	fsdata = reiser4_get_dentry_fsdata(dentry);
	if (IS_ERR(fsdata)) {
		done_lh(&lh);
		return PTR_ERR(fsdata);
	}

	coord = &fsdata->dec.entry_coord;

	assert("nikita-3404",
	       get_inode_oid(dentry->d_inode) != get_inode_oid(dir) ||
	       dir->i_size <= 1);

	coord_clear_iplug(coord);
	if (result == 0) {
		/* remove entry. Just pass control to the directory item
		   plugin. */
		assert("vs-542", inode_dir_item_plugin(dir));
		reiser4_seal_done(&fsdata->dec.entry_seal);
		reiser4_adjust_dir_file(dir, dentry, fsdata->dec.pos, -1);
		result =
		    WITH_COORD(coord,
			       rem_entry(dir, dentry, entry, coord, &lh));
		if (result == 0) {
			if (dir->i_size >= 1)
				INODE_DEC_FIELD(dir, i_size);
			else {
				warning("nikita-2509", "Dir %llu is runt",
					(unsigned long long)
					get_inode_oid(dir));
				result = RETERR(-EIO);
			}

			assert("nikita-3405", dentry->d_inode->i_nlink != 1 ||
			       dentry->d_inode->i_size != 2 ||
			       inode_dir_plugin(dentry->d_inode) == NULL);
		}
	}
	done_lh(&lh);

	return result;
}

static reiser4_block_nr estimate_init(struct inode *parent,
				      struct inode *object);
static int create_dot_dotdot(struct inode *object, struct inode *parent);

/* this is common implementation of init method of dir plugin
   create "." and ".." entries
*/
int reiser4_dir_init_common(struct inode *object,	/* new directory */
			    struct inode *parent,	/* parent directory */
			    reiser4_object_create_data * data /* info passed
							       * to us, this
							       * is filled by
							       * reiser4()
							       * syscall in
							       * particular */)
{
	reiser4_block_nr reserve;

	assert("nikita-680", object != NULL);
	assert("nikita-681", S_ISDIR(object->i_mode));
	assert("nikita-682", parent != NULL);
	assert("nikita-684", data != NULL);
	assert("nikita-686", data->id == DIRECTORY_FILE_PLUGIN_ID);
	assert("nikita-687", object->i_mode & S_IFDIR);

	reserve = estimate_init(parent, object);
	if (reiser4_grab_space(reserve, BA_CAN_COMMIT))
		return RETERR(-ENOSPC);

	return create_dot_dotdot(object, parent);
}

/* this is common implementation of done method of dir plugin
   remove "." entry
*/
int reiser4_dir_done_common(struct inode *object/* object being deleted */)
{
	int result;
	reiser4_block_nr reserve;
	struct dentry goodby_dots;
	reiser4_dir_entry_desc entry;

	assert("nikita-1449", object != NULL);

	if (reiser4_inode_get_flag(object, REISER4_NO_SD))
		return 0;

	/* of course, this can be rewritten to sweep everything in one
	   reiser4_cut_tree(). */
	memset(&entry, 0, sizeof entry);

	/* FIXME: this done method is called from reiser4_delete_dir_common
	 * which reserved space already */
	reserve = inode_dir_plugin(object)->estimate.rem_entry(object);
	if (reiser4_grab_space(reserve, BA_CAN_COMMIT | BA_RESERVED))
		return RETERR(-ENOSPC);

	memset(&goodby_dots, 0, sizeof goodby_dots);
	entry.obj = goodby_dots.d_inode = object;
	goodby_dots.d_name.name = ".";
	goodby_dots.d_name.len = 1;
	result = reiser4_rem_entry_common(object, &goodby_dots, &entry);
	reiser4_free_dentry_fsdata(&goodby_dots);
	if (unlikely(result != 0 && result != -ENOMEM && result != -ENOENT))
		warning("nikita-2252", "Cannot remove dot of %lli: %i",
			(unsigned long long)get_inode_oid(object), result);
	return 0;
}

/* this is common implementation of attach method of dir plugin
*/
int reiser4_attach_common(struct inode *child UNUSED_ARG,
			  struct inode *parent UNUSED_ARG)
{
	assert("nikita-2647", child != NULL);
	assert("nikita-2648", parent != NULL);

	return 0;
}

/* this is common implementation of detach method of dir plugin
   remove "..", decrease nlink on parent
*/
int reiser4_detach_common(struct inode *object, struct inode *parent)
{
	int result;
	struct dentry goodby_dots;
	reiser4_dir_entry_desc entry;

	assert("nikita-2885", object != NULL);
	assert("nikita-2886", !reiser4_inode_get_flag(object, REISER4_NO_SD));

	memset(&entry, 0, sizeof entry);

	/* NOTE-NIKITA this only works if @parent is -the- parent of
	   @object, viz. object whose key is stored in dotdot
	   entry. Wouldn't work with hard-links on directories. */
	memset(&goodby_dots, 0, sizeof goodby_dots);
	entry.obj = goodby_dots.d_inode = parent;
	goodby_dots.d_name.name = "..";
	goodby_dots.d_name.len = 2;
	result = reiser4_rem_entry_common(object, &goodby_dots, &entry);
	reiser4_free_dentry_fsdata(&goodby_dots);
	if (result == 0) {
		/* the dot should be the only entry remaining at this time... */
		assert("nikita-3400",
		       object->i_size == 1 && object->i_nlink <= 2);
#if 0
		/* and, together with the only name directory can have, they
		 * provides for the last 2 remaining references. If we get
		 * here as part of error handling during mkdir, @object
		 * possibly has no name yet, so its nlink == 1. If we get here
		 * from rename (targeting empty directory), it has no name
		 * already, so its nlink == 1. */
		assert("nikita-3401",
		       object->i_nlink == 2 || object->i_nlink == 1);
#endif

		/* decrement nlink of directory removed ".." pointed
		   to */
		reiser4_del_nlink(parent, NULL, 0);
	}
	return result;
}

/* this is common implementation of estimate.add_entry method of
   dir plugin
   estimation of adding entry which supposes that entry is inserting a
   unit into item
*/
reiser4_block_nr estimate_add_entry_common(const struct inode *inode)
{
	return estimate_one_insert_into_item(reiser4_tree_by_inode(inode));
}

/* this is common implementation of estimate.rem_entry method of dir
   plugin
*/
reiser4_block_nr estimate_rem_entry_common(const struct inode *inode)
{
	return estimate_one_item_removal(reiser4_tree_by_inode(inode));
}

/* this is common implementation of estimate.unlink method of dir
   plugin
*/
reiser4_block_nr
dir_estimate_unlink_common(const struct inode *parent,
			   const struct inode *object)
{
	reiser4_block_nr res;

	/* hashed_rem_entry(object) */
	res = inode_dir_plugin(object)->estimate.rem_entry(object);
	/* del_nlink(parent) */
	res += 2 * inode_file_plugin(parent)->estimate.update(parent);

	return res;
}

/*
 * helper for inode_ops ->lookup() and dir plugin's ->get_parent()
 * methods: if @inode is a light-weight file, setup its credentials
 * that are not stored in the stat-data in this case
 */
void check_light_weight(struct inode *inode, struct inode *parent)
{
	if (reiser4_inode_get_flag(inode, REISER4_LIGHT_WEIGHT)) {
		inode->i_uid = parent->i_uid;
		inode->i_gid = parent->i_gid;
		/* clear light-weight flag. If inode would be read by any
		   other name, [ug]id wouldn't change. */
		reiser4_inode_clr_flag(inode, REISER4_LIGHT_WEIGHT);
	}
}

/* looks for name specified in @dentry in directory @parent and if name is
   found - key of object found entry points to is stored in @entry->key */
int reiser4_lookup_name(struct inode *parent,	/* inode of directory to lookup
					 * for name in */
		struct dentry *dentry,	/* name to look for */
		reiser4_key * key/* place to store key */)
{
	int result;
	coord_t *coord;
	lock_handle lh;
	const char *name;
	int len;
	reiser4_dir_entry_desc entry;
	struct reiser4_dentry_fsdata *fsdata;

	assert("nikita-1247", parent != NULL);
	assert("nikita-1248", dentry != NULL);
	assert("nikita-1123", dentry->d_name.name != NULL);
	assert("vs-1486",
	       dentry->d_op == &get_super_private(parent->i_sb)->ops.dentry);

	name = dentry->d_name.name;
	len = dentry->d_name.len;

	if (!inode_dir_plugin(parent)->is_name_acceptable(parent, name, len))
		/* some arbitrary error code to return */
		return RETERR(-ENAMETOOLONG);

	fsdata = reiser4_get_dentry_fsdata(dentry);
	if (IS_ERR(fsdata))
		return PTR_ERR(fsdata);

	coord = &fsdata->dec.entry_coord;
	coord_clear_iplug(coord);
	init_lh(&lh);

	/* find entry in a directory. This is plugin method. */
	result = reiser4_find_entry(parent, dentry, &lh, ZNODE_READ_LOCK,
				    &entry);
	if (result == 0) {
		/* entry was found, extract object key from it. */
		result =
		    WITH_COORD(coord,
			       item_plugin_by_coord(coord)->s.dir.
			       extract_key(coord, key));
	}
	done_lh(&lh);
	return result;

}

/* helper for reiser4_dir_init_common(): estimate number of blocks to reserve */
static reiser4_block_nr
estimate_init(struct inode *parent, struct inode *object)
{
	reiser4_block_nr res = 0;

	assert("vpf-321", parent != NULL);
	assert("vpf-322", object != NULL);

	/* hashed_add_entry(object) */
	res += inode_dir_plugin(object)->estimate.add_entry(object);
	/* reiser4_add_nlink(object) */
	res += inode_file_plugin(object)->estimate.update(object);
	/* hashed_add_entry(object) */
	res += inode_dir_plugin(object)->estimate.add_entry(object);
	/* reiser4_add_nlink(parent) */
	res += inode_file_plugin(parent)->estimate.update(parent);

	return 0;
}

/* helper function for reiser4_dir_init_common(). Create "." and ".." */
static int create_dot_dotdot(struct inode *object/* object to create dot and
						  * dotdot for */ ,
			     struct inode *parent/* parent of @object */)
{
	int result;
	struct dentry dots_entry;
	reiser4_dir_entry_desc entry;

	assert("nikita-688", object != NULL);
	assert("nikita-689", S_ISDIR(object->i_mode));
	assert("nikita-691", parent != NULL);

	/* We store dot and dotdot as normal directory entries. This is
	   not necessary, because almost all information stored in them
	   is already in the stat-data of directory, the only thing
	   being missed is objectid of grand-parent directory that can
	   easily be added there as extension.

	   But it is done the way it is done, because not storing dot
	   and dotdot will lead to the following complications:

	   . special case handling in ->lookup().
	   . addition of another extension to the sd.
	   . dependency on key allocation policy for stat data.

	 */

	memset(&entry, 0, sizeof entry);
	memset(&dots_entry, 0, sizeof dots_entry);
	entry.obj = dots_entry.d_inode = object;
	dots_entry.d_name.name = ".";
	dots_entry.d_name.len = 1;
	result = reiser4_add_entry_common(object, &dots_entry, NULL, &entry);
	reiser4_free_dentry_fsdata(&dots_entry);

	if (result == 0) {
		result = reiser4_add_nlink(object, object, 0);
		if (result == 0) {
			entry.obj = dots_entry.d_inode = parent;
			dots_entry.d_name.name = "..";
			dots_entry.d_name.len = 2;
			result = reiser4_add_entry_common(object,
						  &dots_entry, NULL, &entry);
			reiser4_free_dentry_fsdata(&dots_entry);
			/* if creation of ".." failed, iput() will delete
			   object with ".". */
			if (result == 0) {
				result = reiser4_add_nlink(parent, object, 0);
				if (result != 0)
					/*
					 * if we failed to bump i_nlink, try
					 * to remove ".."
					 */
					reiser4_detach_common(object, parent);
			}
		}
	}

	if (result != 0) {
		/*
		 * in the case of error, at least update stat-data so that,
		 * ->i_nlink updates are not lingering.
		 */
		reiser4_update_sd(object);
		reiser4_update_sd(parent);
	}

	return result;
}

/*
 * return 0 iff @coord contains a directory entry for the file with the name
 * @name.
 */
static int
check_item(const struct inode *dir, const coord_t *coord, const char *name)
{
	item_plugin *iplug;
	char buf[DE_NAME_BUF_LEN];

	iplug = item_plugin_by_coord(coord);
	if (iplug == NULL) {
		warning("nikita-1135", "Cannot get item plugin");
		print_coord("coord", coord, 1);
		return RETERR(-EIO);
	} else if (item_id_by_coord(coord) !=
		   item_id_by_plugin(inode_dir_item_plugin(dir))) {
		/* item id of current item does not match to id of items a
		   directory is built of */
		warning("nikita-1136", "Wrong item plugin");
		print_coord("coord", coord, 1);
		return RETERR(-EIO);
	}
	assert("nikita-1137", iplug->s.dir.extract_name);

	/* Compare name stored in this entry with name we are looking for.

	   NOTE-NIKITA Here should go code for support of something like
	   unicode, code tables, etc.
	 */
	return !!strcmp(name, iplug->s.dir.extract_name(coord, buf));
}

static int
check_entry(const struct inode *dir, coord_t *coord, const struct qstr *name)
{
	return WITH_COORD(coord, check_item(dir, coord, name->name));
}

/*
 * argument package used by entry_actor to scan entries with identical keys.
 */
struct entry_actor_args {
	/* name we are looking for */
	const char *name;
	/* key of directory entry. entry_actor() scans through sequence of
	 * items/units having the same key */
	reiser4_key *key;
	/* how many entries with duplicate key was scanned so far. */
	int non_uniq;
#if REISER4_USE_COLLISION_LIMIT
	/* scan limit */
	int max_non_uniq;
#endif
	/* return parameter: set to true, if ->name wasn't found */
	int not_found;
	/* what type of lock to take when moving to the next node during
	 * scan */
	znode_lock_mode mode;

	/* last coord that was visited during scan */
	coord_t last_coord;
	/* last node locked during scan */
	lock_handle last_lh;
	/* inode of directory */
	const struct inode *inode;
};

/* Function called by reiser4_find_entry() to look for given name
   in the directory. */
static int entry_actor(reiser4_tree * tree UNUSED_ARG /* tree being scanned */ ,
		       coord_t *coord /* current coord */ ,
		       lock_handle * lh /* current lock handle */ ,
		       void *entry_actor_arg/* argument to scan */)
{
	reiser4_key unit_key;
	struct entry_actor_args *args;

	assert("nikita-1131", tree != NULL);
	assert("nikita-1132", coord != NULL);
	assert("nikita-1133", entry_actor_arg != NULL);

	args = entry_actor_arg;
	++args->non_uniq;
#if REISER4_USE_COLLISION_LIMIT
	if (args->non_uniq > args->max_non_uniq) {
		args->not_found = 1;
		/* hash collision overflow. */
		return RETERR(-EBUSY);
	}
#endif

	/*
	 * did we just reach the end of the sequence of items/units with
	 * identical keys?
	 */
	if (!keyeq(args->key, unit_key_by_coord(coord, &unit_key))) {
		assert("nikita-1791",
		       keylt(args->key, unit_key_by_coord(coord, &unit_key)));
		args->not_found = 1;
		args->last_coord.between = AFTER_UNIT;
		return 0;
	}

	coord_dup(&args->last_coord, coord);
	/*
	 * did scan just moved to the next node?
	 */
	if (args->last_lh.node != lh->node) {
		int lock_result;

		/*
		 * if so, lock new node with the mode requested by the caller
		 */
		done_lh(&args->last_lh);
		assert("nikita-1896", znode_is_any_locked(lh->node));
		lock_result = longterm_lock_znode(&args->last_lh, lh->node,
						  args->mode, ZNODE_LOCK_HIPRI);
		if (lock_result != 0)
			return lock_result;
	}
	return check_item(args->inode, coord, args->name);
}

/* Look for given @name within directory @dir.

   This is called during lookup, creation and removal of directory
   entries and on reiser4_rename_common

   First calculate key that directory entry for @name would have. Search
   for this key in the tree. If such key is found, scan all items with
   the same key, checking name in each directory entry along the way.
*/
int reiser4_find_entry(struct inode *dir,	/* directory to scan */
		       struct dentry *de,	/* name to search for */
		       lock_handle * lh,	/* resulting lock handle */
		       znode_lock_mode mode,	/* required lock mode */
		       reiser4_dir_entry_desc * entry	/* parameters of found
							   directory entry */)
{
	const struct qstr *name;
	seal_t *seal;
	coord_t *coord;
	int result;
	__u32 flags;
	struct de_location *dec;
	struct reiser4_dentry_fsdata *fsdata;

	assert("nikita-1130", lh != NULL);
	assert("nikita-1128", dir != NULL);

	name = &de->d_name;
	assert("nikita-1129", name != NULL);

	/* dentry private data don't require lock, because dentry
	   manipulations are protected by i_mutex on parent.

	   This is not so for inodes, because there is no -the- parent in
	   inode case.
	 */
	fsdata = reiser4_get_dentry_fsdata(de);
	if (IS_ERR(fsdata))
		return PTR_ERR(fsdata);
	dec = &fsdata->dec;

	coord = &dec->entry_coord;
	coord_clear_iplug(coord);
	seal = &dec->entry_seal;
	/* compose key of directory entry for @name */
	inode_dir_plugin(dir)->build_entry_key(dir, name, &entry->key);

	if (reiser4_seal_is_set(seal)) {
		/* check seal */
		result = reiser4_seal_validate(seal, coord, &entry->key,
					       lh, mode, ZNODE_LOCK_LOPRI);
		if (result == 0) {
			/* key was found. Check that it is really item we are
			   looking for. */
			result = check_entry(dir, coord, name);
			if (result == 0)
				return 0;
		}
	}
	flags = (mode == ZNODE_WRITE_LOCK) ? CBK_FOR_INSERT : 0;
	/*
	 * find place in the tree where directory item should be located.
	 */
	result = reiser4_object_lookup(dir, &entry->key, coord, lh, mode,
				       FIND_EXACT, LEAF_LEVEL, LEAF_LEVEL,
				       flags, NULL/*ra_info */);
	if (result == CBK_COORD_FOUND) {
		struct entry_actor_args arg;

		/* fast path: no hash collisions */
		result = check_entry(dir, coord, name);
		if (result == 0) {
			reiser4_seal_init(seal, coord, &entry->key);
			dec->pos = 0;
		} else if (result > 0) {
			/* Iterate through all units with the same keys. */
			arg.name = name->name;
			arg.key = &entry->key;
			arg.not_found = 0;
			arg.non_uniq = 0;
#if REISER4_USE_COLLISION_LIMIT
			arg.max_non_uniq = max_hash_collisions(dir);
			assert("nikita-2851", arg.max_non_uniq > 1);
#endif
			arg.mode = mode;
			arg.inode = dir;
			coord_init_zero(&arg.last_coord);
			init_lh(&arg.last_lh);

			result = reiser4_iterate_tree
				(reiser4_tree_by_inode(dir),
				 coord, lh,
				 entry_actor, &arg, mode, 1);
			/* if end of the tree or extent was reached during
			   scanning. */
			if (arg.not_found || (result == -E_NO_NEIGHBOR)) {
				/* step back */
				done_lh(lh);

				result = zload(arg.last_coord.node);
				if (result == 0) {
					coord_clear_iplug(&arg.last_coord);
					coord_dup(coord, &arg.last_coord);
					move_lh(lh, &arg.last_lh);
					result = RETERR(-ENOENT);
					zrelse(arg.last_coord.node);
					--arg.non_uniq;
				}
			}

			done_lh(&arg.last_lh);
			if (result == 0)
				reiser4_seal_init(seal, coord, &entry->key);

			if (result == 0 || result == -ENOENT) {
				assert("nikita-2580", arg.non_uniq > 0);
				dec->pos = arg.non_uniq - 1;
			}
		}
	} else
		dec->pos = -1;
	return result;
}

/*
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   scroll-step: 1
   End:
*/
