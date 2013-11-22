/* Copyright 2001, 2002, 2003, 2004 by Hans Reiser, licensing governed by
 * reiser4/README */

#include "../inode.h"
#include "../safe_link.h"

static const char *possible_leak = "Possible disk space leak.";

/* re-bind existing name at @from_coord in @from_dir to point to @to_inode.

   Helper function called from hashed_rename() */
static int replace_name(struct inode *to_inode,	/* inode where @from_coord is
						 * to be re-targeted at */
			struct inode *from_dir,	/* directory where @from_coord
						 * lives */
			struct inode *from_inode, /* inode @from_coord
						   * originally point to */
			coord_t *from_coord,	/* where directory entry is in
						 * the tree */
			lock_handle * from_lh/* lock handle on @from_coord */)
{
	item_plugin *from_item;
	int result;
	znode *node;

	coord_clear_iplug(from_coord);
	node = from_coord->node;
	result = zload(node);
	if (result != 0)
		return result;
	from_item = item_plugin_by_coord(from_coord);
	if (plugin_of_group(item_plugin_by_coord(from_coord),
			    DIR_ENTRY_ITEM_TYPE)) {
		reiser4_key to_key;

		build_sd_key(to_inode, &to_key);

		/* everything is found and prepared to change directory entry
		   at @from_coord to point to @to_inode.

		   @to_inode is just about to get new name, so bump its link
		   counter.

		 */
		result = reiser4_add_nlink(to_inode, from_dir, 0);
		if (result != 0) {
			/* Don't issue warning: this may be plain -EMLINK */
			zrelse(node);
			return result;
		}

		result =
		    from_item->s.dir.update_key(from_coord, &to_key, from_lh);
		if (result != 0) {
			reiser4_del_nlink(to_inode, from_dir, 0);
			zrelse(node);
			return result;
		}

		/* @from_inode just lost its name, he-he.

		   If @from_inode was directory, it contained dotdot pointing
		   to @from_dir. @from_dir i_nlink will be decreased when
		   iput() will be called on @from_inode.

		   If file-system is not ADG (hard-links are
		   supported on directories), iput(from_inode) will not remove
		   @from_inode, and thus above is incorrect, but hard-links on
		   directories are problematic in many other respects.
		 */
		result = reiser4_del_nlink(from_inode, from_dir, 0);
		if (result != 0) {
			warning("nikita-2330",
				"Cannot remove link from source: %i. %s",
				result, possible_leak);
		}
		/* Has to return success, because entry is already
		 * modified. */
		result = 0;

		/* NOTE-NIKITA consider calling plugin method in stead of
		   accessing inode fields directly. */
		from_dir->i_mtime = CURRENT_TIME;
	} else {
		warning("nikita-2326", "Unexpected item type");
		result = RETERR(-EIO);
	}
	zrelse(node);
	return result;
}

/* add new entry pointing to @inode into @dir at @coord, locked by @lh

   Helper function used by hashed_rename(). */
static int add_name(struct inode *inode,	/* inode where @coord is to be
						 * re-targeted at */
		    struct inode *dir,	/* directory where @coord lives */
		    struct dentry *name,	/* new name */
		    coord_t *coord,	/* where directory entry is in the tree
					 */
		    lock_handle * lh,	/* lock handle on @coord */
		    int is_dir/* true, if @inode is directory */)
{
	int result;
	reiser4_dir_entry_desc entry;

	assert("nikita-2333", lh->node == coord->node);
	assert("nikita-2334", is_dir == S_ISDIR(inode->i_mode));

	memset(&entry, 0, sizeof entry);
	entry.obj = inode;
	/* build key of directory entry description */
	inode_dir_plugin(dir)->build_entry_key(dir, &name->d_name, &entry.key);

	/* ext2 does this in different order: first inserts new entry,
	   then increases directory nlink. We don't want do this,
	   because reiser4_add_nlink() calls ->add_link() plugin
	   method that can fail for whatever reason, leaving as with
	   cleanup problems.
	 */
	/* @inode is getting new name */
	reiser4_add_nlink(inode, dir, 0);
	/* create @new_name in @new_dir pointing to
	   @old_inode */
	result = WITH_COORD(coord,
			    inode_dir_item_plugin(dir)->s.dir.add_entry(dir,
									coord,
									lh,
									name,
									&entry));
	if (result != 0) {
		int result2;
		result2 = reiser4_del_nlink(inode, dir, 0);
		if (result2 != 0) {
			warning("nikita-2327",
				"Cannot drop link on %lli %i. %s",
				(unsigned long long)get_inode_oid(inode),
				result2, possible_leak);
		}
	} else
		INODE_INC_FIELD(dir, i_size);
	return result;
}

static reiser4_block_nr estimate_rename(struct inode *old_dir,  /* directory
								 * where @old is
								 * located */
					struct dentry *old_name,/* old name */
					struct inode *new_dir,  /* directory
								 * where @new is
								 * located */
					struct dentry *new_name /* new name */)
{
	reiser4_block_nr res1, res2;
	dir_plugin * p_parent_old, *p_parent_new;
	file_plugin * p_child_old, *p_child_new;

	assert("vpf-311", old_dir != NULL);
	assert("vpf-312", new_dir != NULL);
	assert("vpf-313", old_name != NULL);
	assert("vpf-314", new_name != NULL);

	p_parent_old = inode_dir_plugin(old_dir);
	p_parent_new = inode_dir_plugin(new_dir);
	p_child_old = inode_file_plugin(old_name->d_inode);
	if (new_name->d_inode)
		p_child_new = inode_file_plugin(new_name->d_inode);
	else
		p_child_new = NULL;

	/* find_entry - can insert one leaf. */
	res1 = res2 = 1;

	/* replace_name */
	{
		/* reiser4_add_nlink(p_child_old) and
		 * reiser4_del_nlink(p_child_old) */
		res1 += 2 * p_child_old->estimate.update(old_name->d_inode);
		/* update key */
		res1 += 1;
		/* reiser4_del_nlink(p_child_new) */
		if (p_child_new)
			res1 += p_child_new->estimate.update(new_name->d_inode);
	}

	/* else add_name */
	{
		/* reiser4_add_nlink(p_parent_new) and
		 * reiser4_del_nlink(p_parent_new) */
		res2 +=
		    2 * inode_file_plugin(new_dir)->estimate.update(new_dir);
		/* reiser4_add_nlink(p_parent_old) */
		res2 += p_child_old->estimate.update(old_name->d_inode);
		/* add_entry(p_parent_new) */
		res2 += p_parent_new->estimate.add_entry(new_dir);
		/* reiser4_del_nlink(p_parent_old) */
		res2 += p_child_old->estimate.update(old_name->d_inode);
	}

	res1 = res1 < res2 ? res2 : res1;

	/* reiser4_write_sd(p_parent_new) */
	res1 += inode_file_plugin(new_dir)->estimate.update(new_dir);

	/* reiser4_write_sd(p_child_new) */
	if (p_child_new)
		res1 += p_child_new->estimate.update(new_name->d_inode);

	/* hashed_rem_entry(p_parent_old) */
	res1 += p_parent_old->estimate.rem_entry(old_dir);

	/* reiser4_del_nlink(p_child_old) */
	res1 += p_child_old->estimate.update(old_name->d_inode);

	/* replace_name */
	{
		/* reiser4_add_nlink(p_parent_dir_new) */
		res1 += inode_file_plugin(new_dir)->estimate.update(new_dir);
		/* update_key */
		res1 += 1;
		/* reiser4_del_nlink(p_parent_new) */
		res1 += inode_file_plugin(new_dir)->estimate.update(new_dir);
		/* reiser4_del_nlink(p_parent_old) */
		res1 += inode_file_plugin(old_dir)->estimate.update(old_dir);
	}

	/* reiser4_write_sd(p_parent_old) */
	res1 += inode_file_plugin(old_dir)->estimate.update(old_dir);

	/* reiser4_write_sd(p_child_old) */
	res1 += p_child_old->estimate.update(old_name->d_inode);

	return res1;
}

static int hashed_rename_estimate_and_grab(struct inode *old_dir,  /* directory
								    * where @old
								    * is located
								    */
					   struct dentry *old_name,/* old name
								    */
					   struct inode *new_dir,  /* directory
								    * where @new
								    * is located
								    */
					   struct dentry *new_name /* new name
								    */)
{
	reiser4_block_nr reserve;

	reserve = estimate_rename(old_dir, old_name, new_dir, new_name);

	if (reiser4_grab_space(reserve, BA_CAN_COMMIT))
		return RETERR(-ENOSPC);

	return 0;
}

/* check whether @old_inode and @new_inode can be moved within file system
 * tree. This singles out attempts to rename pseudo-files, for example. */
static int can_rename(struct inode *old_dir, struct inode *old_inode,
		      struct inode *new_dir, struct inode *new_inode)
{
	file_plugin *fplug;
	dir_plugin *dplug;

	assert("nikita-3370", old_inode != NULL);

	dplug = inode_dir_plugin(new_dir);
	fplug = inode_file_plugin(old_inode);

	if (dplug == NULL)
		return RETERR(-ENOTDIR);
	else if (new_dir->i_op->create == NULL)
		return RETERR(-EPERM);
	else if (!fplug->can_add_link(old_inode))
		return RETERR(-EMLINK);
	else if (new_inode != NULL) {
		fplug = inode_file_plugin(new_inode);
		if (fplug->can_rem_link != NULL &&
		    !fplug->can_rem_link(new_inode))
			return RETERR(-EBUSY);
	}
	return 0;
}

int reiser4_find_entry(struct inode *, struct dentry *, lock_handle * ,
	       znode_lock_mode, reiser4_dir_entry_desc *);
int reiser4_update_dir(struct inode *);

/* this is common implementation of vfs's rename method of struct
   inode_operations
   See comments in the body.

   It is arguable that this function can be made generic so, that it
   will be applicable to any kind of directory plugin that deals with
   directories composed out of directory entries. The only obstacle
   here is that we don't have any data-type to represent directory
   entry. This should be re-considered when more than one different
   directory plugin will be implemented.
*/
int reiser4_rename_common(struct inode *old_dir /* directory where @old
						 * is located */ ,
			  struct dentry *old_name /* old name */ ,
			  struct inode *new_dir /* directory where @new
						 * is located */ ,
			  struct dentry *new_name/* new name */)
{
	/* From `The Open Group Base Specifications Issue 6'

	   If either the old or new argument names a symbolic link, rename()
	   shall operate on the symbolic link itself, and shall not resolve
	   the last component of the argument. If the old argument and the new
	   argument resolve to the same existing file, rename() shall return
	   successfully and perform no other action.

	   [this is done by VFS: vfs_rename()]

	   If the old argument points to the pathname of a file that is not a
	   directory, the new argument shall not point to the pathname of a
	   directory.

	   [checked by VFS: vfs_rename->may_delete()]

	   If the link named by the new argument exists, it shall
	   be removed and old renamed to new. In this case, a link named new
	   shall remain visible to other processes throughout the renaming
	   operation and refer either to the file referred to by new or old
	   before the operation began.

	   [we should assure this]

	   Write access permission is required for
	   both the directory containing old and the directory containing new.

	   [checked by VFS: vfs_rename->may_delete(), may_create()]

	   If the old argument points to the pathname of a directory, the new
	   argument shall not point to the pathname of a file that is not a
	   directory.

	   [checked by VFS: vfs_rename->may_delete()]

	   If the directory named by the new argument exists, it
	   shall be removed and old renamed to new. In this case, a link named
	   new shall exist throughout the renaming operation and shall refer
	   either to the directory referred to by new or old before the
	   operation began.

	   [we should assure this]

	   If new names an existing directory, it shall be
	   required to be an empty directory.

	   [we should check this]

	   If the old argument points to a pathname of a symbolic link, the
	   symbolic link shall be renamed. If the new argument points to a
	   pathname of a symbolic link, the symbolic link shall be removed.

	   The new pathname shall not contain a path prefix that names
	   old. Write access permission is required for the directory
	   containing old and the directory containing new. If the old
	   argument points to the pathname of a directory, write access
	   permission may be required for the directory named by old, and, if
	   it exists, the directory named by new.

	   [checked by VFS: vfs_rename(), vfs_rename_dir()]

	   If the link named by the new argument exists and the file's link
	   count becomes 0 when it is removed and no process has the file
	   open, the space occupied by the file shall be freed and the file
	   shall no longer be accessible. If one or more processes have the
	   file open when the last link is removed, the link shall be removed
	   before rename() returns, but the removal of the file contents shall
	   be postponed until all references to the file are closed.

	   [iput() handles this, but we can do this manually, a la
	   reiser4_unlink()]

	   Upon successful completion, rename() shall mark for update the
	   st_ctime and st_mtime fields of the parent directory of each file.

	   [N/A]

	 */
	reiser4_context *ctx;
	int result;
	int is_dir;		/* is @old_name directory */

	struct inode *old_inode;
	struct inode *new_inode;
	coord_t *new_coord;

	struct reiser4_dentry_fsdata *new_fsdata;
	dir_plugin *dplug;
	file_plugin *fplug;

	reiser4_dir_entry_desc *old_entry, *new_entry, *dotdot_entry;
	lock_handle * new_lh, *dotdot_lh;
	struct dentry *dotdot_name;
	struct reiser4_dentry_fsdata *dataonstack;

	ctx = reiser4_init_context(old_dir->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	old_entry = kzalloc(3 * sizeof(*old_entry) + 2 * sizeof(*new_lh) +
			    sizeof(*dotdot_name) + sizeof(*dataonstack),
			    reiser4_ctx_gfp_mask_get());
	if (!old_entry) {
		context_set_commit_async(ctx);
		reiser4_exit_context(ctx);
		return RETERR(-ENOMEM);
	}

	new_entry = old_entry + 1;
	dotdot_entry = old_entry + 2;
	new_lh = (lock_handle *)(old_entry + 3);
	dotdot_lh = new_lh + 1;
	dotdot_name = (struct dentry *)(new_lh + 2);
	dataonstack = (struct reiser4_dentry_fsdata *)(dotdot_name + 1);

	assert("nikita-2318", old_dir != NULL);
	assert("nikita-2319", new_dir != NULL);
	assert("nikita-2320", old_name != NULL);
	assert("nikita-2321", new_name != NULL);

	old_inode = old_name->d_inode;
	new_inode = new_name->d_inode;

	dplug = inode_dir_plugin(old_dir);
	fplug = NULL;

	new_fsdata = reiser4_get_dentry_fsdata(new_name);
	if (IS_ERR(new_fsdata)) {
		kfree(old_entry);
		context_set_commit_async(ctx);
		reiser4_exit_context(ctx);
		return PTR_ERR(new_fsdata);
	}

	new_coord = &new_fsdata->dec.entry_coord;
	coord_clear_iplug(new_coord);

	is_dir = S_ISDIR(old_inode->i_mode);

	assert("nikita-3461", old_inode->i_nlink >= 1 + !!is_dir);

	/* if target is existing directory and it's not empty---return error.

	   This check is done specifically, because is_dir_empty() requires
	   tree traversal and have to be done before locks are taken.
	 */
	if (is_dir && new_inode != NULL && is_dir_empty(new_inode) != 0) {
		kfree(old_entry);
		context_set_commit_async(ctx);
		reiser4_exit_context(ctx);
		return RETERR(-ENOTEMPTY);
	}

	result = can_rename(old_dir, old_inode, new_dir, new_inode);
	if (result != 0) {
		kfree(old_entry);
		context_set_commit_async(ctx);
		reiser4_exit_context(ctx);
		return result;
	}

	result = hashed_rename_estimate_and_grab(old_dir, old_name,
						 new_dir, new_name);
	if (result != 0) {
		kfree(old_entry);
		context_set_commit_async(ctx);
		reiser4_exit_context(ctx);
		return result;
	}

	init_lh(new_lh);

	/* find entry for @new_name */
	result = reiser4_find_entry(new_dir, new_name, new_lh, ZNODE_WRITE_LOCK,
				    new_entry);

	if (IS_CBKERR(result)) {
		done_lh(new_lh);
		kfree(old_entry);
		context_set_commit_async(ctx);
		reiser4_exit_context(ctx);
		return result;
	}

	reiser4_seal_done(&new_fsdata->dec.entry_seal);

	/* add or replace name for @old_inode as @new_name */
	if (new_inode != NULL) {
		/* target (@new_name) exists. */
		/* Not clear what to do with objects that are
		   both directories and files at the same time. */
		if (result == CBK_COORD_FOUND) {
			result = replace_name(old_inode,
					      new_dir,
					      new_inode, new_coord, new_lh);
			if (result == 0)
				fplug = inode_file_plugin(new_inode);
		} else if (result == CBK_COORD_NOTFOUND) {
			/* VFS told us that @new_name is bound to existing
			   inode, but we failed to find directory entry. */
			warning("nikita-2324", "Target not found");
			result = RETERR(-ENOENT);
		}
	} else {
		/* target (@new_name) doesn't exists. */
		if (result == CBK_COORD_NOTFOUND)
			result = add_name(old_inode,
					  new_dir,
					  new_name, new_coord, new_lh, is_dir);
		else if (result == CBK_COORD_FOUND) {
			/* VFS told us that @new_name is "negative" dentry,
			   but we found directory entry. */
			warning("nikita-2331", "Target found unexpectedly");
			result = RETERR(-EIO);
		}
	}

	assert("nikita-3462", ergo(result == 0,
				   old_inode->i_nlink >= 2 + !!is_dir));

	/* We are done with all modifications to the @new_dir, release lock on
	   node. */
	done_lh(new_lh);

	if (fplug != NULL) {
		/* detach @new_inode from name-space */
		result = fplug->detach(new_inode, new_dir);
		if (result != 0)
			warning("nikita-2330", "Cannot detach %lli: %i. %s",
				(unsigned long long)get_inode_oid(new_inode),
				result, possible_leak);
	}

	if (new_inode != NULL)
		reiser4_update_sd(new_inode);

	if (result == 0) {
		old_entry->obj = old_inode;

		dplug->build_entry_key(old_dir,
				       &old_name->d_name, &old_entry->key);

		/* At this stage new name was introduced for
		   @old_inode. @old_inode, @new_dir, and @new_inode i_nlink
		   counters were updated.

		   We want to remove @old_name now. If @old_inode wasn't
		   directory this is simple.
		 */
		result = dplug->rem_entry(old_dir, old_name, old_entry);
		if (result != 0 && result != -ENOMEM) {
			warning("nikita-2335",
				"Cannot remove old name: %i", result);
		} else {
			result = reiser4_del_nlink(old_inode, old_dir, 0);
			if (result != 0 && result != -ENOMEM) {
				warning("nikita-2337",
					"Cannot drop link on old: %i", result);
			}
		}

		if (result == 0 && is_dir) {
			/* @old_inode is directory. We also have to update
			   dotdot entry. */
			coord_t *dotdot_coord;

			memset(dataonstack, 0, sizeof dataonstack);
			memset(dotdot_entry, 0, sizeof dotdot_entry);
			dotdot_entry->obj = old_dir;
			memset(dotdot_name, 0, sizeof dotdot_name);
			dotdot_name->d_name.name = "..";
			dotdot_name->d_name.len = 2;
			/*
			 * allocate ->d_fsdata on the stack to avoid using
			 * reiser4_get_dentry_fsdata(). Locking is not needed,
			 * because dentry is private to the current thread.
			 */
			dotdot_name->d_fsdata = dataonstack;
			init_lh(dotdot_lh);

			dotdot_coord = &dataonstack->dec.entry_coord;
			coord_clear_iplug(dotdot_coord);

			result = reiser4_find_entry(old_inode, dotdot_name,
						    dotdot_lh, ZNODE_WRITE_LOCK,
						    dotdot_entry);
			if (result == 0) {
				/* replace_name() decreases i_nlink on
				 * @old_dir */
				result = replace_name(new_dir,
						      old_inode,
						      old_dir,
						      dotdot_coord, dotdot_lh);
			} else
				result = RETERR(-EIO);
			done_lh(dotdot_lh);
		}
	}
	reiser4_update_dir(new_dir);
	reiser4_update_dir(old_dir);
	reiser4_update_sd(old_inode);
	if (result == 0) {
		file_plugin *fplug;

		if (new_inode != NULL) {
			/* add safe-link for target file (in case we removed
			 * last reference to the poor fellow */
			fplug = inode_file_plugin(new_inode);
			if (new_inode->i_nlink == 0)
				result = safe_link_add(new_inode, SAFE_UNLINK);
		}
	}
	kfree(old_entry);
	context_set_commit_async(ctx);
	reiser4_exit_context(ctx);
	return result;
}

#if 0
int reiser4_rename_common(struct inode *old_dir /* directory where @old
						 * is located */ ,
			  struct dentry *old_name /* old name */ ,
			  struct inode *new_dir /* directory where @new
						 * is located */ ,
			  struct dentry *new_name/* new name */)
{
	/* From `The Open Group Base Specifications Issue 6'

	   If either the old or new argument names a symbolic link, rename()
	   shall operate on the symbolic link itself, and shall not resolve
	   the last component of the argument. If the old argument and the new
	   argument resolve to the same existing file, rename() shall return
	   successfully and perform no other action.

	   [this is done by VFS: vfs_rename()]

	   If the old argument points to the pathname of a file that is not a
	   directory, the new argument shall not point to the pathname of a
	   directory.

	   [checked by VFS: vfs_rename->may_delete()]

	   If the link named by the new argument exists, it shall
	   be removed and old renamed to new. In this case, a link named new
	   shall remain visible to other processes throughout the renaming
	   operation and refer either to the file referred to by new or old
	   before the operation began.

	   [we should assure this]

	   Write access permission is required for
	   both the directory containing old and the directory containing new.

	   [checked by VFS: vfs_rename->may_delete(), may_create()]

	   If the old argument points to the pathname of a directory, the new
	   argument shall not point to the pathname of a file that is not a
	   directory.

	   [checked by VFS: vfs_rename->may_delete()]

	   If the directory named by the new argument exists, it
	   shall be removed and old renamed to new. In this case, a link named
	   new shall exist throughout the renaming operation and shall refer
	   either to the directory referred to by new or old before the
	   operation began.

	   [we should assure this]

	   If new names an existing directory, it shall be
	   required to be an empty directory.

	   [we should check this]

	   If the old argument points to a pathname of a symbolic link, the
	   symbolic link shall be renamed. If the new argument points to a
	   pathname of a symbolic link, the symbolic link shall be removed.

	   The new pathname shall not contain a path prefix that names
	   old. Write access permission is required for the directory
	   containing old and the directory containing new. If the old
	   argument points to the pathname of a directory, write access
	   permission may be required for the directory named by old, and, if
	   it exists, the directory named by new.

	   [checked by VFS: vfs_rename(), vfs_rename_dir()]

	   If the link named by the new argument exists and the file's link
	   count becomes 0 when it is removed and no process has the file
	   open, the space occupied by the file shall be freed and the file
	   shall no longer be accessible. If one or more processes have the
	   file open when the last link is removed, the link shall be removed
	   before rename() returns, but the removal of the file contents shall
	   be postponed until all references to the file are closed.

	   [iput() handles this, but we can do this manually, a la
	   reiser4_unlink()]

	   Upon successful completion, rename() shall mark for update the
	   st_ctime and st_mtime fields of the parent directory of each file.

	   [N/A]

	 */
	reiser4_context *ctx;
	int result;
	int is_dir;		/* is @old_name directory */
	struct inode *old_inode;
	struct inode *new_inode;
	reiser4_dir_entry_desc old_entry;
	reiser4_dir_entry_desc new_entry;
	coord_t *new_coord;
	struct reiser4_dentry_fsdata *new_fsdata;
	lock_handle new_lh;
	dir_plugin *dplug;
	file_plugin *fplug;

	ctx = reiser4_init_context(old_dir->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	assert("nikita-2318", old_dir != NULL);
	assert("nikita-2319", new_dir != NULL);
	assert("nikita-2320", old_name != NULL);
	assert("nikita-2321", new_name != NULL);

	old_inode = old_name->d_inode;
	new_inode = new_name->d_inode;

	dplug = inode_dir_plugin(old_dir);
	fplug = NULL;

	new_fsdata = reiser4_get_dentry_fsdata(new_name);
	if (IS_ERR(new_fsdata)) {
		result = PTR_ERR(new_fsdata);
		goto exit;
	}

	new_coord = &new_fsdata->dec.entry_coord;
	coord_clear_iplug(new_coord);

	is_dir = S_ISDIR(old_inode->i_mode);

	assert("nikita-3461", old_inode->i_nlink >= 1 + !!is_dir);

	/* if target is existing directory and it's not empty---return error.

	   This check is done specifically, because is_dir_empty() requires
	   tree traversal and have to be done before locks are taken.
	 */
	if (is_dir && new_inode != NULL && is_dir_empty(new_inode) != 0)
		return RETERR(-ENOTEMPTY);

	result = can_rename(old_dir, old_inode, new_dir, new_inode);
	if (result != 0)
		goto exit;

	result = hashed_rename_estimate_and_grab(old_dir, old_name,
						 new_dir, new_name);
	if (result != 0)
		goto exit;

	init_lh(&new_lh);

	/* find entry for @new_name */
	result = reiser4_find_entry(new_dir, new_name, &new_lh,
				    ZNODE_WRITE_LOCK, &new_entry);

	if (IS_CBKERR(result)) {
		done_lh(&new_lh);
		goto exit;
	}

	reiser4_seal_done(&new_fsdata->dec.entry_seal);

	/* add or replace name for @old_inode as @new_name */
	if (new_inode != NULL) {
		/* target (@new_name) exists. */
		/* Not clear what to do with objects that are
		   both directories and files at the same time. */
		if (result == CBK_COORD_FOUND) {
			result = replace_name(old_inode,
					      new_dir,
					      new_inode, new_coord, &new_lh);
			if (result == 0)
				fplug = inode_file_plugin(new_inode);
		} else if (result == CBK_COORD_NOTFOUND) {
			/* VFS told us that @new_name is bound to existing
			   inode, but we failed to find directory entry. */
			warning("nikita-2324", "Target not found");
			result = RETERR(-ENOENT);
		}
	} else {
		/* target (@new_name) doesn't exists. */
		if (result == CBK_COORD_NOTFOUND)
			result = add_name(old_inode,
					  new_dir,
					  new_name, new_coord, &new_lh, is_dir);
		else if (result == CBK_COORD_FOUND) {
			/* VFS told us that @new_name is "negative" dentry,
			   but we found directory entry. */
			warning("nikita-2331", "Target found unexpectedly");
			result = RETERR(-EIO);
		}
	}

	assert("nikita-3462", ergo(result == 0,
				   old_inode->i_nlink >= 2 + !!is_dir));

	/* We are done with all modifications to the @new_dir, release lock on
	   node. */
	done_lh(&new_lh);

	if (fplug != NULL) {
		/* detach @new_inode from name-space */
		result = fplug->detach(new_inode, new_dir);
		if (result != 0)
			warning("nikita-2330", "Cannot detach %lli: %i. %s",
				(unsigned long long)get_inode_oid(new_inode),
				result, possible_leak);
	}

	if (new_inode != NULL)
		reiser4_update_sd(new_inode);

	if (result == 0) {
		memset(&old_entry, 0, sizeof old_entry);
		old_entry.obj = old_inode;

		dplug->build_entry_key(old_dir,
				       &old_name->d_name, &old_entry.key);

		/* At this stage new name was introduced for
		   @old_inode. @old_inode, @new_dir, and @new_inode i_nlink
		   counters were updated.

		   We want to remove @old_name now. If @old_inode wasn't
		   directory this is simple.
		 */
		result = dplug->rem_entry(old_dir, old_name, &old_entry);
		/*result = rem_entry_hashed(old_dir, old_name, &old_entry); */
		if (result != 0 && result != -ENOMEM) {
			warning("nikita-2335",
				"Cannot remove old name: %i", result);
		} else {
			result = reiser4_del_nlink(old_inode, old_dir, 0);
			if (result != 0 && result != -ENOMEM) {
				warning("nikita-2337",
					"Cannot drop link on old: %i", result);
			}
		}

		if (result == 0 && is_dir) {
			/* @old_inode is directory. We also have to update
			   dotdot entry. */
			coord_t *dotdot_coord;
			lock_handle dotdot_lh;
			struct dentry dotdot_name;
			reiser4_dir_entry_desc dotdot_entry;
			struct reiser4_dentry_fsdata dataonstack;
			struct reiser4_dentry_fsdata *fsdata;

			memset(&dataonstack, 0, sizeof dataonstack);
			memset(&dotdot_entry, 0, sizeof dotdot_entry);
			dotdot_entry.obj = old_dir;
			memset(&dotdot_name, 0, sizeof dotdot_name);
			dotdot_name.d_name.name = "..";
			dotdot_name.d_name.len = 2;
			/*
			 * allocate ->d_fsdata on the stack to avoid using
			 * reiser4_get_dentry_fsdata(). Locking is not needed,
			 * because dentry is private to the current thread.
			 */
			dotdot_name.d_fsdata = &dataonstack;
			init_lh(&dotdot_lh);

			fsdata = &dataonstack;
			dotdot_coord = &fsdata->dec.entry_coord;
			coord_clear_iplug(dotdot_coord);

			result = reiser4_find_entry(old_inode,
						    &dotdot_name,
						    &dotdot_lh,
						    ZNODE_WRITE_LOCK,
						    &dotdot_entry);
			if (result == 0) {
				/* replace_name() decreases i_nlink on
				 * @old_dir */
				result = replace_name(new_dir,
						      old_inode,
						      old_dir,
						      dotdot_coord, &dotdot_lh);
			} else
				result = RETERR(-EIO);
			done_lh(&dotdot_lh);
		}
	}
	reiser4_update_dir(new_dir);
	reiser4_update_dir(old_dir);
	reiser4_update_sd(old_inode);
	if (result == 0) {
		file_plugin *fplug;

		if (new_inode != NULL) {
			/* add safe-link for target file (in case we removed
			 * last reference to the poor fellow */
			fplug = inode_file_plugin(new_inode);
			if (new_inode->i_nlink == 0)
				result = safe_link_add(new_inode, SAFE_UNLINK);
		}
	}
exit:
	context_set_commit_async(ctx);
	reiser4_exit_context(ctx);
	return result;
}
#endif
