/*
 * Copyright 2005 by Hans Reiser, licensing governed by reiser4/README
 */

/*
 * this file contains typical implementations for most of methods of struct
 * inode_operations
 */

#include "../inode.h"
#include "../safe_link.h"

#include <linux/quotaops.h>
#include <linux/namei.h>

static int create_vfs_object(struct inode *parent, struct dentry *dentry,
		      reiser4_object_create_data *data);

/**
 * reiser4_create_common - create of inode operations
 * @parent: inode of parent directory
 * @dentry: dentry of new object to create
 * @mode: the permissions to use
 * @exclusive:
 *
 * This is common implementation of vfs's create method of struct
 * inode_operations.
 * Creates regular file using file plugin from parent directory plugin set.
 */
int reiser4_create_common(struct inode *parent, struct dentry *dentry,
			  umode_t mode, bool exclusive)
{
	reiser4_object_create_data data;
	file_plugin *fplug;

	memset(&data, 0, sizeof data);
	data.mode = S_IFREG | mode;
	fplug = child_create_plugin(parent) ? : inode_create_plugin(parent);
	if (!plugin_of_group(fplug, REISER4_REGULAR_FILE)) {
		warning("vpf-1900", "'%s' is not a regular file plugin.",
			fplug->h.label);
		return RETERR(-EIO);
	}
	data.id = fplug->h.id;
	return create_vfs_object(parent, dentry, &data);
}

int reiser4_lookup_name(struct inode *dir, struct dentry *, reiser4_key *);
void check_light_weight(struct inode *inode, struct inode *parent);

/**
 * reiser4_lookup_common - lookup of inode operations
 * @parent: inode of directory to lookup into
 * @dentry: name to look for
 * @flags:
 *
 * This is common implementation of vfs's lookup method of struct
 * inode_operations.
 */
struct dentry *reiser4_lookup_common(struct inode *parent,
				     struct dentry *dentry,
				     unsigned int flags)
{
	reiser4_context *ctx;
	int result;
	struct dentry *new;
	struct inode *inode;
	reiser4_dir_entry_desc entry;

	ctx = reiser4_init_context(parent->i_sb);
	if (IS_ERR(ctx))
		return (struct dentry *)ctx;

	/* set up operations on dentry. */
	dentry->d_op = &get_super_private(parent->i_sb)->ops.dentry;

	result = reiser4_lookup_name(parent, dentry, &entry.key);
	if (result) {
		context_set_commit_async(ctx);
		reiser4_exit_context(ctx);
		if (result == -ENOENT) {
			/* object not found */
			if (!IS_DEADDIR(parent))
				d_add(dentry, NULL);
			return NULL;
		}
		return ERR_PTR(result);
	}

	inode = reiser4_iget(parent->i_sb, &entry.key, 0);
	if (IS_ERR(inode)) {
		context_set_commit_async(ctx);
		reiser4_exit_context(ctx);
		return ERR_PTR(PTR_ERR(inode));
	}

	/* success */
	check_light_weight(inode, parent);
	new = d_splice_alias(inode, dentry);
	reiser4_iget_complete(inode);

	/* prevent balance_dirty_pages() from being called: we don't want to
	 * do this under directory i_mutex. */
	context_set_commit_async(ctx);
	reiser4_exit_context(ctx);
	return new;
}

static reiser4_block_nr common_estimate_link(struct inode *parent,
					     struct inode *object);
int reiser4_update_dir(struct inode *);

static inline void reiser4_check_immutable(struct inode *inode)
{
        do {
	        if (!reiser4_inode_get_flag(inode, REISER4_IMMUTABLE))
		        break;
		yield();
	} while (1);
}

/**
 * reiser4_link_common - link of inode operations
 * @existing: dentry of object which is to get new name
 * @parent: directory where new name is to be created
 * @newname: new name
 *
 * This is common implementation of vfs's link method of struct
 * inode_operations.
 */
int reiser4_link_common(struct dentry *existing, struct inode *parent,
			struct dentry *newname)
{
	reiser4_context *ctx;
	int result;
	struct inode *object;
	dir_plugin *parent_dplug;
	reiser4_dir_entry_desc entry;
	reiser4_object_create_data data;
	reiser4_block_nr reserve;

	ctx = reiser4_init_context(parent->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	assert("nikita-1431", existing != NULL);
	assert("nikita-1432", parent != NULL);
	assert("nikita-1433", newname != NULL);

	object = existing->d_inode;
	assert("nikita-1434", object != NULL);

	/* check for race with create_object() */
	reiser4_check_immutable(object);

	parent_dplug = inode_dir_plugin(parent);

	memset(&entry, 0, sizeof entry);
	entry.obj = object;

	data.mode = object->i_mode;
	data.id = inode_file_plugin(object)->h.id;

	reserve = common_estimate_link(parent, existing->d_inode);
	if ((__s64) reserve < 0) {
		context_set_commit_async(ctx);
		reiser4_exit_context(ctx);
		return reserve;
	}

	if (reiser4_grab_space(reserve, BA_CAN_COMMIT)) {
		context_set_commit_async(ctx);
		reiser4_exit_context(ctx);
		return RETERR(-ENOSPC);
	}

	/*
	 * Subtle race handling: sys_link() doesn't take i_mutex on @parent. It
	 * means that link(2) can race against unlink(2) or rename(2), and
	 * inode is dead (->i_nlink == 0) when reiser4_link() is entered.
	 *
	 * For such inode we have to undo special processing done in
	 * reiser4_unlink() viz. creation of safe-link.
	 */
	if (unlikely(object->i_nlink == 0)) {
		result = safe_link_del(reiser4_tree_by_inode(object),
				       get_inode_oid(object), SAFE_UNLINK);
		if (result != 0) {
			context_set_commit_async(ctx);
			reiser4_exit_context(ctx);
			return result;
		}
	}

	/* increment nlink of @existing and update its stat data */
	result = reiser4_add_nlink(object, parent, 1);
	if (result == 0) {
		/* add entry to the parent */
		result =
		    parent_dplug->add_entry(parent, newname, &data, &entry);
		if (result != 0) {
			/* failed to add entry to the parent, decrement nlink
			   of @existing */
			reiser4_del_nlink(object, parent, 1);
			/*
			 * now, if that failed, we have a file with too big
			 * nlink---space leak, much better than directory
			 * entry pointing to nowhere
			 */
		}
	}
	if (result == 0) {
		atomic_inc(&object->i_count);
		/*
		 * Upon successful completion, link() shall mark for update
		 * the st_ctime field of the file. Also, the st_ctime and
		 * st_mtime fields of the directory that contains the new
		 * entry shall be marked for update. --SUS
		 */
		result = reiser4_update_dir(parent);
	}
	if (result == 0)
		d_instantiate(newname, existing->d_inode);

	context_set_commit_async(ctx);
	reiser4_exit_context(ctx);
	return result;
}

static int unlink_check_and_grab(struct inode *parent, struct dentry *victim);

/**
 * reiser4_unlink_common - unlink of inode operations
 * @parent: inode of directory to remove name from
 * @victim: name to be removed
 *
 * This is common implementation of vfs's unlink method of struct
 * inode_operations.
 */
int reiser4_unlink_common(struct inode *parent, struct dentry *victim)
{
	reiser4_context *ctx;
	int result;
	struct inode *object;
	file_plugin *fplug;

	ctx = reiser4_init_context(parent->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	object = victim->d_inode;
	fplug = inode_file_plugin(object);
	assert("nikita-2882", fplug->detach != NULL);

	result = unlink_check_and_grab(parent, victim);
	if (result != 0) {
		context_set_commit_async(ctx);
		reiser4_exit_context(ctx);
		return result;
	}

	result = fplug->detach(object, parent);
	if (result == 0) {
		dir_plugin *parent_dplug;
		reiser4_dir_entry_desc entry;

		parent_dplug = inode_dir_plugin(parent);
		memset(&entry, 0, sizeof entry);

		/* first, delete directory entry */
		result = parent_dplug->rem_entry(parent, victim, &entry);
		if (result == 0) {
			/*
			 * if name was removed successfully, we _have_ to
			 * return 0 from this function, because upper level
			 * caller (vfs_{rmdir,unlink}) expect this.
			 *
			 * now that directory entry is removed, update
			 * stat-data
			 */
			reiser4_del_nlink(object, parent, 1);
			/*
			 * Upon successful completion, unlink() shall mark for
			 * update the st_ctime and st_mtime fields of the
			 * parent directory. Also, if the file's link count is
			 * not 0, the st_ctime field of the file shall be
			 * marked for update. --SUS
			 */
			reiser4_update_dir(parent);
			/* add safe-link for this file */
			if (object->i_nlink == 0)
				safe_link_add(object, SAFE_UNLINK);
		}
	}

	if (unlikely(result != 0)) {
		if (result != -ENOMEM)
			warning("nikita-3398", "Cannot unlink %llu (%i)",
				(unsigned long long)get_inode_oid(object),
				result);
		/* if operation failed commit pending inode modifications to
		 * the stat-data */
		reiser4_update_sd(object);
		reiser4_update_sd(parent);
	}

	reiser4_release_reserved(object->i_sb);

	/* @object's i_ctime was updated by ->rem_link() method(). */

	/* @victim can be already removed from the disk by this time. Inode is
	   then marked so that iput() wouldn't try to remove stat data. But
	   inode itself is still there.
	 */

	/*
	 * we cannot release directory semaphore here, because name has
	 * already been deleted, but dentry (@victim) still exists.  Prevent
	 * balance_dirty_pages() from being called on exiting this context: we
	 * don't want to do this under directory i_mutex.
	 */
	context_set_commit_async(ctx);
	reiser4_exit_context(ctx);
	return result;
}

/**
 * reiser4_symlink_common - symlink of inode operations
 * @parent: inode of parent directory
 * @dentry: dentry of object to be created
 * @linkname: string symlink is to contain
 *
 * This is common implementation of vfs's symlink method of struct
 * inode_operations.
 * Creates object using file plugin SYMLINK_FILE_PLUGIN_ID.
 */
int reiser4_symlink_common(struct inode *parent, struct dentry *dentry,
			   const char *linkname)
{
	reiser4_object_create_data data;

	memset(&data, 0, sizeof data);
	data.name = linkname;
	data.id = SYMLINK_FILE_PLUGIN_ID;
	data.mode = S_IFLNK | S_IRWXUGO;
	return create_vfs_object(parent, dentry, &data);
}

/**
 * reiser4_mkdir_common - mkdir of inode operations
 * @parent: inode of parent directory
 * @dentry: dentry of object to be created
 * @mode: the permissions to use
 *
 * This is common implementation of vfs's mkdir method of struct
 * inode_operations.
 * Creates object using file plugin DIRECTORY_FILE_PLUGIN_ID.
 */
int reiser4_mkdir_common(struct inode *parent, struct dentry *dentry, umode_t mode)
{
	reiser4_object_create_data data;

	memset(&data, 0, sizeof data);
	data.mode = S_IFDIR | mode;
	data.id = DIRECTORY_FILE_PLUGIN_ID;
	return create_vfs_object(parent, dentry, &data);
}

/**
 * reiser4_mknod_common - mknod of inode operations
 * @parent: inode of parent directory
 * @dentry: dentry of object to be created
 * @mode: the permissions to use and file type
 * @rdev: minor and major of new device file
 *
 * This is common implementation of vfs's mknod method of struct
 * inode_operations.
 * Creates object using file plugin SPECIAL_FILE_PLUGIN_ID.
 */
int reiser4_mknod_common(struct inode *parent, struct dentry *dentry,
			 umode_t mode, dev_t rdev)
{
	reiser4_object_create_data data;

	memset(&data, 0, sizeof data);
	data.mode = mode;
	data.rdev = rdev;
	data.id = SPECIAL_FILE_PLUGIN_ID;
	return create_vfs_object(parent, dentry, &data);
}

/*
 * implementation of vfs's rename method of struct inode_operations for typical
 * directory is in inode_ops_rename.c
 */

/**
 * reiser4_follow_link_common - follow_link of inode operations
 * @dentry: dentry of symlink
 * @data:
 *
 * This is common implementation of vfs's followlink method of struct
 * inode_operations.
 * Assumes that inode's i_private points to the content of symbolic link.
 */
void *reiser4_follow_link_common(struct dentry *dentry, struct nameidata *nd)
{
	assert("vs-851", S_ISLNK(dentry->d_inode->i_mode));

	if (!dentry->d_inode->i_private
	    || !reiser4_inode_get_flag(dentry->d_inode,
				       REISER4_GENERIC_PTR_USED))
		return ERR_PTR(RETERR(-EINVAL));
	nd_set_link(nd, dentry->d_inode->i_private);
	return NULL;
}

/**
 * reiser4_permission_common - permission of inode operations
 * @inode: inode to check permissions for
 * @mask: mode bits to check permissions for
 * @flags:
 *
 * Uses generic function to check for rwx permissions.
 */
int reiser4_permission_common(struct inode *inode, int mask)
{
	// generic_permission() says that it's rcu-aware...
#if 0
	if (mask & MAY_NOT_BLOCK)
		return -ECHILD;
#endif
	return generic_permission(inode, mask);
}

static int setattr_reserve(reiser4_tree *);

/* this is common implementation of vfs's setattr method of struct
   inode_operations
*/
int reiser4_setattr_common(struct dentry *dentry, struct iattr *attr)
{
	reiser4_context *ctx;
	struct inode *inode;
	int result;

	inode = dentry->d_inode;
	result = inode_change_ok(inode, attr);
	if (result)
		return result;

	ctx = reiser4_init_context(inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	assert("nikita-3119", !(attr->ia_valid & ATTR_SIZE));

	/*
	 * grab disk space and call standard
	 * setattr_copy();
	 * mark_inode_dirty().
	 */
	result = setattr_reserve(reiser4_tree_by_inode(inode));
	if (!result) {
		if ((attr->ia_valid & ATTR_UID && attr->ia_uid != inode->i_uid)
		    || (attr->ia_valid & ATTR_GID
			&& attr->ia_gid != inode->i_gid)) {
			result = dquot_transfer(inode, attr) ? -EDQUOT : 0;
			if (result) {
				context_set_commit_async(ctx);
				reiser4_exit_context(ctx);
				return result;
			}
		}
		setattr_copy(inode, attr);
		mark_inode_dirty(inode);
		result = reiser4_update_sd(inode);
	}
	context_set_commit_async(ctx);
	reiser4_exit_context(ctx);
	return result;
}

/* this is common implementation of vfs's getattr method of struct
   inode_operations
*/
int reiser4_getattr_common(struct vfsmount *mnt UNUSED_ARG,
			   struct dentry *dentry, struct kstat *stat)
{
	struct inode *obj;

	assert("nikita-2298", dentry != NULL);
	assert("nikita-2299", stat != NULL);
	assert("nikita-2300", dentry->d_inode != NULL);

	obj = dentry->d_inode;

	stat->dev = obj->i_sb->s_dev;
	stat->ino = oid_to_uino(get_inode_oid(obj));
	stat->mode = obj->i_mode;
	/* don't confuse userland with huge nlink. This is not entirely
	 * correct, because nlink_t is not necessary 16 bit signed. */
	stat->nlink = min(obj->i_nlink, (typeof(obj->i_nlink)) 0x7fff);
	stat->uid = obj->i_uid;
	stat->gid = obj->i_gid;
	stat->rdev = obj->i_rdev;
	stat->atime = obj->i_atime;
	stat->mtime = obj->i_mtime;
	stat->ctime = obj->i_ctime;
	stat->size = obj->i_size;
	stat->blocks =
	    (inode_get_bytes(obj) + VFS_BLKSIZE - 1) >> VFS_BLKSIZE_BITS;
	/* "preferred" blocksize for efficient file system I/O */
	stat->blksize = get_super_private(obj->i_sb)->optimal_io_size;

	return 0;
}

/* Estimate the maximum amount of nodes which might be allocated or changed on
   typical new object creation. Typical creation consists of calling create
   method of file plugin, adding directory entry to parent and update parent
   directory's stat data.
*/
static reiser4_block_nr estimate_create_vfs_object(struct inode *parent,
						   /* parent object */
						   struct inode *object
						   /* object */)
{
	assert("vpf-309", parent != NULL);
	assert("vpf-307", object != NULL);

	return
	    /* object creation estimation */
	    inode_file_plugin(object)->estimate.create(object) +
	    /* stat data of parent directory estimation */
	    inode_file_plugin(parent)->estimate.update(parent) +
	    /* adding entry estimation */
	    inode_dir_plugin(parent)->estimate.add_entry(parent) +
	    /* to undo in the case of failure */
	    inode_dir_plugin(parent)->estimate.rem_entry(parent);
}

/* Create child in directory.

   . get object's plugin
   . get fresh inode
   . initialize inode
   . add object's stat-data
   . initialize object's directory
   . add entry to the parent
   . instantiate dentry

*/
static int do_create_vfs_child(reiser4_object_create_data * data,/* parameters
								    of new
								    object */
			       struct inode **retobj)
{
	int result;

	struct dentry *dentry;	/* parent object */
	struct inode *parent;	/* new name */

	dir_plugin *par_dir;	/* directory plugin on the parent */
	dir_plugin *obj_dir;	/* directory plugin on the new object */
	file_plugin *obj_plug;	/* object plugin on the new object */
	struct inode *object;	/* new object */
	reiser4_block_nr reserve;

	reiser4_dir_entry_desc entry;	/* new directory entry */

	assert("nikita-1420", data != NULL);
	parent = data->parent;
	dentry = data->dentry;

	assert("nikita-1418", parent != NULL);
	assert("nikita-1419", dentry != NULL);

	/* check, that name is acceptable for parent */
	par_dir = inode_dir_plugin(parent);
	if (par_dir->is_name_acceptable &&
	    !par_dir->is_name_acceptable(parent,
					 dentry->d_name.name,
					 (int)dentry->d_name.len))
		return RETERR(-ENAMETOOLONG);

	result = 0;
	obj_plug = file_plugin_by_id((int)data->id);
	if (obj_plug == NULL) {
		warning("nikita-430", "Cannot find plugin %i", data->id);
		return RETERR(-ENOENT);
	}
	object = new_inode(parent->i_sb);
	if (object == NULL)
		return RETERR(-ENOMEM);
	/* new_inode() initializes i_ino to "arbitrary" value. Reset it to 0,
	 * to simplify error handling: if some error occurs before i_ino is
	 * initialized with oid, i_ino should already be set to some
	 * distinguished value. */
	object->i_ino = 0;

	/* So that on error iput will be called. */
	*retobj = object;

	if (dquot_alloc_inode(object)) {
		dquot_drop(object);
		object->i_flags |= S_NOQUOTA;
		return RETERR(-EDQUOT);
	}

	memset(&entry, 0, sizeof entry);
	entry.obj = object;

	set_plugin(&reiser4_inode_data(object)->pset, PSET_FILE,
		   file_plugin_to_plugin(obj_plug));
	result = obj_plug->set_plug_in_inode(object, parent, data);
	if (result) {
		warning("nikita-431", "Cannot install plugin %i on %llx",
			data->id, (unsigned long long)get_inode_oid(object));
		dquot_free_inode(object);
		object->i_flags |= S_NOQUOTA;
		return result;
	}

	/* reget plugin after installation */
	obj_plug = inode_file_plugin(object);

	if (obj_plug->create_object == NULL) {
		dquot_free_inode(object);
		object->i_flags |= S_NOQUOTA;
		return RETERR(-EPERM);
	}

	/* if any of hash, tail, sd or permission plugins for newly created
	   object are not set yet set them here inheriting them from parent
	   directory
	 */
	assert("nikita-2070", obj_plug->adjust_to_parent != NULL);
	result = obj_plug->adjust_to_parent(object,
					    parent,
					    object->i_sb->s_root->d_inode);
	if (result == 0)
		result = finish_pset(object);
	if (result != 0) {
		warning("nikita-432", "Cannot inherit from %llx to %llx",
			(unsigned long long)get_inode_oid(parent),
			(unsigned long long)get_inode_oid(object));
		dquot_free_inode(object);
		object->i_flags |= S_NOQUOTA;
		return result;
	}

	/* setup inode and file-operations for this inode */
	setup_inode_ops(object, data);

	/* call file plugin's method to initialize plugin specific part of
	 * inode */
	if (obj_plug->init_inode_data)
		obj_plug->init_inode_data(object, data, 1/*create */);

	/* obtain directory plugin (if any) for new object. */
	obj_dir = inode_dir_plugin(object);
	if (obj_dir != NULL && obj_dir->init == NULL) {
		dquot_free_inode(object);
		object->i_flags |= S_NOQUOTA;
		return RETERR(-EPERM);
	}

	reiser4_inode_data(object)->locality_id = get_inode_oid(parent);

	reserve = estimate_create_vfs_object(parent, object);
	if (reiser4_grab_space(reserve, BA_CAN_COMMIT)) {
		dquot_free_inode(object);
		object->i_flags |= S_NOQUOTA;
		return RETERR(-ENOSPC);
	}

	/* mark inode `immutable'. We disable changes to the file being
	   created until valid directory entry for it is inserted. Otherwise,
	   if file were expanded and insertion of directory entry fails, we
	   have to remove file, but we only alloted enough space in
	   transaction to remove _empty_ file. 3.x code used to remove stat
	   data in different transaction thus possibly leaking disk space on
	   crash. This all only matters if it's possible to access file
	   without name, for example, by inode number
	 */
	reiser4_inode_set_flag(object, REISER4_IMMUTABLE);

	/* create empty object, this includes allocation of new objectid. For
	   directories this implies creation of dot and dotdot  */
	assert("nikita-2265", reiser4_inode_get_flag(object, REISER4_NO_SD));

	/* mark inode as `loaded'. From this point onward
	   reiser4_delete_inode() will try to remove its stat-data. */
	reiser4_inode_set_flag(object, REISER4_LOADED);

	result = obj_plug->create_object(object, parent, data);
	if (result != 0) {
		reiser4_inode_clr_flag(object, REISER4_IMMUTABLE);
		if (result != -ENAMETOOLONG && result != -ENOMEM)
			warning("nikita-2219",
				"Failed to create sd for %llu",
				(unsigned long long)get_inode_oid(object));
		dquot_free_inode(object);
		object->i_flags |= S_NOQUOTA;
		return result;
	}

	if (obj_dir != NULL)
		result = obj_dir->init(object, parent, data);
	if (result == 0) {
		assert("nikita-434", !reiser4_inode_get_flag(object,
							     REISER4_NO_SD));
		/* insert inode into VFS hash table */
		insert_inode_hash(object);
		/* create entry */
		result = par_dir->add_entry(parent, dentry, data, &entry);
		if (result == 0) {
			/* If O_CREAT is set and the file did not previously
			   exist, upon successful completion, open() shall
			   mark for update the st_atime, st_ctime, and
			   st_mtime fields of the file and the st_ctime and
			   st_mtime fields of the parent directory. --SUS
			 */
			object->i_ctime = CURRENT_TIME;
			reiser4_update_dir(parent);
		}
		if (result != 0)
			/* cleanup failure to add entry */
			obj_plug->detach(object, parent);
	} else if (result != -ENOMEM)
		warning("nikita-2219", "Failed to initialize dir for %llu: %i",
			(unsigned long long)get_inode_oid(object), result);

	/*
	 * update stat-data, committing all pending modifications to the inode
	 * fields.
	 */
	reiser4_update_sd(object);
	if (result != 0) {
		dquot_free_inode(object);
		object->i_flags |= S_NOQUOTA;
		/* if everything was ok (result == 0), parent stat-data is
		 * already updated above (update_parent_dir()) */
		reiser4_update_sd(parent);
		/* failure to create entry, remove object */
		obj_plug->delete_object(object);
	}

	/* file has name now, clear immutable flag */
	reiser4_inode_clr_flag(object, REISER4_IMMUTABLE);

	/* on error, iput() will call ->delete_inode(). We should keep track
	   of the existence of stat-data for this inode and avoid attempt to
	   remove it in reiser4_delete_inode(). This is accomplished through
	   REISER4_NO_SD bit in inode.u.reiser4_i.plugin.flags
	 */
	return result;
}

/* this is helper for common implementations of reiser4_mkdir, reiser4_create,
   reiser4_mknod and reiser4_symlink
*/
static int
create_vfs_object(struct inode *parent,
		  struct dentry *dentry, reiser4_object_create_data * data)
{
	reiser4_context *ctx;
	int result;
	struct inode *child;

	ctx = reiser4_init_context(parent->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	context_set_commit_async(ctx);

	data->parent = parent;
	data->dentry = dentry;
	child = NULL;
	result = do_create_vfs_child(data, &child);
	if (unlikely(result != 0)) {
		if (child != NULL) {
			/* for unlinked inode accounting in iput() */
			clear_nlink(child);
			reiser4_make_bad_inode(child);
			iput(child);
		}
	} else
		d_instantiate(dentry, child);

	reiser4_exit_context(ctx);
	return result;
}

/**
 * helper for link_common. Estimate disk space necessary to add a link
 * from @parent to @object
 */
static reiser4_block_nr common_estimate_link(struct inode *parent /* parent
								   * directory
								   */,
					     struct inode *object /* object to
								   * which new
								   * link is
								   * being
								   * created */)
{
	reiser4_block_nr res = 0;
	file_plugin *fplug;
	dir_plugin *dplug;

	assert("vpf-317", object != NULL);
	assert("vpf-318", parent != NULL);

	fplug = inode_file_plugin(object);
	dplug = inode_dir_plugin(parent);
	/* VS-FIXME-HANS: why do we do fplug->estimate.update(object) twice
	 * instead of multiplying by 2? */
	/* reiser4_add_nlink(object) */
	res += fplug->estimate.update(object);
	/* add_entry(parent) */
	res += dplug->estimate.add_entry(parent);
	/* reiser4_del_nlink(object) */
	res += fplug->estimate.update(object);
	/* update_dir(parent) */
	res += inode_file_plugin(parent)->estimate.update(parent);
	/* safe-link */
	res += estimate_one_item_removal(reiser4_tree_by_inode(object));

	return res;
}

/* Estimate disk space necessary to remove a link between @parent and
   @object.
*/
static reiser4_block_nr estimate_unlink(struct inode *parent /* parent
							      * directory */,
					struct inode *object /* object to which
							      * new link is
							      * being created
							      */)
{
	reiser4_block_nr res = 0;
	file_plugin *fplug;
	dir_plugin *dplug;

	assert("vpf-317", object != NULL);
	assert("vpf-318", parent != NULL);

	fplug = inode_file_plugin(object);
	dplug = inode_dir_plugin(parent);

	/* rem_entry(parent) */
	res += dplug->estimate.rem_entry(parent);
	/* reiser4_del_nlink(object) */
	res += fplug->estimate.update(object);
	/* update_dir(parent) */
	res += inode_file_plugin(parent)->estimate.update(parent);
	/* fplug->unlink */
	res += fplug->estimate.unlink(object, parent);
	/* safe-link */
	res += estimate_one_insert_item(reiser4_tree_by_inode(object));

	return res;
}

/* helper for reiser4_unlink_common. Estimate and grab space for unlink. */
static int unlink_check_and_grab(struct inode *parent, struct dentry *victim)
{
	file_plugin *fplug;
	struct inode *child;
	int result;

	result = 0;
	child = victim->d_inode;
	fplug = inode_file_plugin(child);

	/* check for race with create_object() */
	reiser4_check_immutable(child);

	/* object being deleted should have stat data */
	assert("vs-949", !reiser4_inode_get_flag(child, REISER4_NO_SD));

	/* ask object plugin */
	if (fplug->can_rem_link != NULL && !fplug->can_rem_link(child))
		return RETERR(-ENOTEMPTY);

	result = (int)estimate_unlink(parent, child);
	if (result < 0)
		return result;

	return reiser4_grab_reserved(child->i_sb, result, BA_CAN_COMMIT);
}

/* helper for reiser4_setattr_common */
static int setattr_reserve(reiser4_tree * tree)
{
	assert("vs-1096", is_grab_enabled(get_current_context()));
	return reiser4_grab_space(estimate_one_insert_into_item(tree),
				  BA_CAN_COMMIT);
}

/* helper function. Standards require that for many file-system operations
   on success ctime and mtime of parent directory is to be updated. */
int reiser4_update_dir(struct inode *dir)
{
	assert("nikita-2525", dir != NULL);

	dir->i_ctime = dir->i_mtime = CURRENT_TIME;
	return reiser4_update_sd(dir);
}
