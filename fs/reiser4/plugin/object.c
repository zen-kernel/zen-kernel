/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/*
 * Examples of object plugins: file, directory, symlink, special file.
 *
 * Plugins associated with inode:
 *
 * Plugin of inode is plugin referenced by plugin-id field of on-disk
 * stat-data. How we store this plugin in in-core inode is not
 * important. Currently pointers are used, another variant is to store offsets
 * and do array lookup on each access.
 *
 * Now, each inode has one selected plugin: object plugin that
 * determines what type of file this object is: directory, regular etc.
 *
 * This main plugin can use other plugins that are thus subordinated to
 * it. Directory instance of object plugin uses hash; regular file
 * instance uses tail policy plugin.
 *
 * Object plugin is either taken from id in stat-data or guessed from
 * i_mode bits. Once it is established we ask it to install its
 * subordinate plugins, by looking again in stat-data or inheriting them
 * from parent.
 *
 * How new inode is initialized during ->read_inode():
 * 1 read stat-data and initialize inode fields: i_size, i_mode,
 *   i_generation, capabilities etc.
 * 2 read plugin id from stat data or try to guess plugin id
 *   from inode->i_mode bits if plugin id is missing.
 * 3 Call ->init_inode() method of stat-data plugin to initialise inode fields.
 *
 * NIKITA-FIXME-HANS: can you say a little about 1 being done before 3?  What
 * if stat data does contain i_size, etc., due to it being an unusual plugin?
 *
 * 4 Call ->activate() method of object's plugin. Plugin is either read from
 *    from stat-data or guessed from mode bits
 * 5 Call ->inherit() method of object plugin to inherit as yet un initialized
 *    plugins from parent.
 *
 * Easy induction proves that on last step all plugins of inode would be
 * initialized.
 *
 * When creating new object:
 * 1 obtain object plugin id (see next period)
 * NIKITA-FIXME-HANS: period?
 * 2 ->install() this plugin
 * 3 ->inherit() the rest from the parent
 *
 * We need some examples of creating an object with default and non-default
 * plugin ids.  Nikita, please create them.
 */

#include "../inode.h"

int _bugop(void)
{
	BUG_ON(1);
	return 0;
}

#define bugop ((void *)_bugop)

static int flow_by_inode_bugop(struct inode *inode, const char __user *buf,
			       int user, loff_t size,
			       loff_t off, rw_op op, flow_t *f)
{
	BUG_ON(1);
	return 0;
}

static int key_by_inode_bugop(struct inode *inode, loff_t off, reiser4_key *key)
{
	BUG_ON(1);
	return 0;
}

static int _dummyop(void)
{
	return 0;
}

#define dummyop ((void *)_dummyop)

static int change_file(struct inode *inode,
		       reiser4_plugin * plugin,
		       pset_member memb)
{
	/* cannot change object plugin of already existing object */
	if (memb == PSET_FILE)
		return RETERR(-EINVAL);

	/* Change PSET_CREATE */
	return aset_set_unsafe(&reiser4_inode_data(inode)->pset, memb, plugin);
}

static reiser4_plugin_ops file_plugin_ops = {
	.change = change_file
};

static struct inode_operations         null_i_ops = {.create = NULL};
static struct file_operations          null_f_ops = {.owner = NULL};
static struct address_space_operations null_a_ops = {.writepage = NULL};

/*
 * Reiser4 provides for VFS either dispatcher, or common (fop,
 * iop, aop) method.
 *
 * Dispatchers (suffixed with "dispatch") pass management to
 * proper plugin in accordance with plugin table (pset) located
 * in the private part of inode.
 *
 * Common methods are NOT prefixed with "dispatch". They are
 * the same for all plugins of FILE interface, and, hence, no
 * dispatching is needed.
 */

/*
 * VFS methods for regular files
 */
static struct inode_operations regular_file_i_ops = {
	.permission = reiser4_permission_common,
	.setattr = reiser4_setattr_dispatch,
	.getattr = reiser4_getattr_common
};
static struct file_operations regular_file_f_ops = {
	.llseek = generic_file_llseek,
	.read = reiser4_read_dispatch,
	.write = reiser4_write_dispatch,
	.aio_read = generic_file_aio_read,
	.unlocked_ioctl = reiser4_ioctl_dispatch,
#ifdef CONFIG_COMPAT
	.compat_ioctl = reiser4_ioctl_dispatch,
#endif
	.mmap = reiser4_mmap_dispatch,
	.open = reiser4_open_dispatch,
	.release = reiser4_release_dispatch,
	.fsync = reiser4_sync_file_common,
	.splice_read = generic_file_splice_read,
	.splice_write = generic_file_splice_write
};
static struct address_space_operations regular_file_a_ops = {
	.writepage = reiser4_writepage,
	.readpage = reiser4_readpage_dispatch,
	//.sync_page = block_sync_page,
	.writepages = reiser4_writepages_dispatch,
	.set_page_dirty = reiser4_set_page_dirty,
	.readpages = reiser4_readpages_dispatch,
	.write_begin = reiser4_write_begin_dispatch,
	.write_end = reiser4_write_end_dispatch,
	.bmap = reiser4_bmap_dispatch,
	.invalidatepage = reiser4_invalidatepage,
	.releasepage = reiser4_releasepage,
	.migratepage = reiser4_migratepage
};

/* VFS methods for symlink files */
static struct inode_operations symlink_file_i_ops = {
	.readlink = generic_readlink,
	.follow_link = reiser4_follow_link_common,
	.permission = reiser4_permission_common,
	.setattr = reiser4_setattr_common,
	.getattr = reiser4_getattr_common
};

/* VFS methods for special files */
static struct inode_operations special_file_i_ops = {
	.permission = reiser4_permission_common,
	.setattr = reiser4_setattr_common,
	.getattr = reiser4_getattr_common
};

/* VFS methods for directories */
static struct inode_operations directory_i_ops = {
	.create = reiser4_create_common,
	.lookup = reiser4_lookup_common,
	.link = reiser4_link_common,
	.unlink = reiser4_unlink_common,
	.symlink = reiser4_symlink_common,
	.mkdir = reiser4_mkdir_common,
	.rmdir = reiser4_unlink_common,
	.mknod = reiser4_mknod_common,
	.rename = reiser4_rename_common,
	.permission = reiser4_permission_common,
	.setattr = reiser4_setattr_common,
	.getattr = reiser4_getattr_common
};
static struct file_operations directory_f_ops = {
	.llseek = reiser4_llseek_dir_common,
	.read = generic_read_dir,
	.iterate = reiser4_iterate_common,
	.release = reiser4_release_dir_common,
	.fsync = reiser4_sync_common
};
static struct address_space_operations directory_a_ops = {
	.writepages = dummyop,
};

/*
 * Definitions of object plugins.
 */

file_plugin file_plugins[LAST_FILE_PLUGIN_ID] = {
	[UNIX_FILE_PLUGIN_ID] = {
		.h = {
			.type_id = REISER4_FILE_PLUGIN_TYPE,
			.id = UNIX_FILE_PLUGIN_ID,
			.groups = (1 << REISER4_REGULAR_FILE),
			.pops = &file_plugin_ops,
			.label = "reg",
			.desc = "regular file",
			.linkage = {NULL, NULL},
		},
		/*
		 * invariant vfs ops
		 */
		.inode_ops = &regular_file_i_ops,
		.file_ops = &regular_file_f_ops,
		.as_ops = &regular_file_a_ops,
		/*
		 * private i_ops
		 */
		.setattr = setattr_unix_file,
		.open = open_unix_file,
		.read = read_unix_file,
		.write = write_unix_file,
		.ioctl = ioctl_unix_file,
		.mmap = mmap_unix_file,
		.release = release_unix_file,
		/*
		 * private f_ops
		 */
		.readpage = readpage_unix_file,
		.readpages = readpages_unix_file,
		.writepages = writepages_unix_file,
		.write_begin = write_begin_unix_file,
		.write_end = write_end_unix_file,
		/*
		 * private a_ops
		 */
		.bmap = bmap_unix_file,
		/*
		 * other private methods
		 */
		.write_sd_by_inode = write_sd_by_inode_common,
		.flow_by_inode = flow_by_inode_unix_file,
		.key_by_inode = key_by_inode_and_offset_common,
		.set_plug_in_inode = set_plug_in_inode_common,
		.adjust_to_parent = adjust_to_parent_common,
		.create_object = reiser4_create_object_common,
		.delete_object = delete_object_unix_file,
		.add_link = reiser4_add_link_common,
		.rem_link = reiser4_rem_link_common,
		.owns_item = owns_item_unix_file,
		.can_add_link = can_add_link_common,
		.detach = dummyop,
		.bind = dummyop,
		.safelink = safelink_common,
		.estimate = {
			.create = estimate_create_common,
			.update = estimate_update_common,
			.unlink = estimate_unlink_common
		},
		.init_inode_data = init_inode_data_unix_file,
		.cut_tree_worker = cut_tree_worker_common,
		.wire = {
			.write = wire_write_common,
			.read = wire_read_common,
			.get = wire_get_common,
			.size = wire_size_common,
			.done = wire_done_common
		}
	},
	[DIRECTORY_FILE_PLUGIN_ID] = {
		.h = {
			.type_id = REISER4_FILE_PLUGIN_TYPE,
			.id = DIRECTORY_FILE_PLUGIN_ID,
			.groups = (1 << REISER4_DIRECTORY_FILE),
			.pops = &file_plugin_ops,
			.label = "dir",
			.desc = "directory",
			.linkage = {NULL, NULL}
		},
		.inode_ops = &null_i_ops,
		.file_ops = &null_f_ops,
		.as_ops = &null_a_ops,

		.write_sd_by_inode = write_sd_by_inode_common,
		.flow_by_inode = flow_by_inode_bugop,
		.key_by_inode = key_by_inode_bugop,
		.set_plug_in_inode = set_plug_in_inode_common,
		.adjust_to_parent = adjust_to_parent_common_dir,
		.create_object = reiser4_create_object_common,
		.delete_object = reiser4_delete_dir_common,
		.add_link = reiser4_add_link_common,
		.rem_link = rem_link_common_dir,
		.owns_item = owns_item_common_dir,
		.can_add_link = can_add_link_common,
		.can_rem_link = can_rem_link_common_dir,
		.detach = reiser4_detach_common_dir,
		.bind = reiser4_bind_common_dir,
		.safelink = safelink_common,
		.estimate = {
			.create = estimate_create_common_dir,
			.update = estimate_update_common,
			.unlink = estimate_unlink_common_dir
		},
		.wire = {
			.write = wire_write_common,
			.read = wire_read_common,
			.get = wire_get_common,
			.size = wire_size_common,
			.done = wire_done_common
		},
		.init_inode_data = init_inode_ordering,
		.cut_tree_worker = cut_tree_worker_common,
	},
	[SYMLINK_FILE_PLUGIN_ID] = {
		.h = {
			.type_id = REISER4_FILE_PLUGIN_TYPE,
			.id = SYMLINK_FILE_PLUGIN_ID,
			.groups = (1 << REISER4_SYMLINK_FILE),
			.pops = &file_plugin_ops,
			.label = "symlink",
			.desc = "symbolic link",
			.linkage = {NULL,NULL}
		},
		.inode_ops = &symlink_file_i_ops,
		/* inode->i_fop of symlink is initialized
		   by NULL in setup_inode_ops */
		.file_ops = &null_f_ops,
		.as_ops = &null_a_ops,

		.write_sd_by_inode = write_sd_by_inode_common,
		.set_plug_in_inode = set_plug_in_inode_common,
		.adjust_to_parent = adjust_to_parent_common,
		.create_object = reiser4_create_symlink,
		.delete_object = reiser4_delete_object_common,
		.add_link = reiser4_add_link_common,
		.rem_link = reiser4_rem_link_common,
		.can_add_link = can_add_link_common,
		.detach = dummyop,
		.bind = dummyop,
		.safelink = safelink_common,
		.estimate = {
			.create = estimate_create_common,
			.update = estimate_update_common,
			.unlink = estimate_unlink_common
		},
		.init_inode_data = init_inode_ordering,
		.cut_tree_worker = cut_tree_worker_common,
		.destroy_inode = destroy_inode_symlink,
		.wire = {
			.write = wire_write_common,
			.read = wire_read_common,
			.get = wire_get_common,
			.size = wire_size_common,
			.done = wire_done_common
		}
	},
	[SPECIAL_FILE_PLUGIN_ID] = {
		.h = {
			.type_id = REISER4_FILE_PLUGIN_TYPE,
			.id = SPECIAL_FILE_PLUGIN_ID,
			.groups = (1 << REISER4_SPECIAL_FILE),
			.pops = &file_plugin_ops,
			.label = "special",
			.desc =
			"special: fifo, device or socket",
			.linkage = {NULL, NULL}
		},
		.inode_ops = &special_file_i_ops,
		/* file_ops of special files (sockets, block, char, fifo) are
		   initialized by init_special_inode. */
		.file_ops = &null_f_ops,
		.as_ops = &null_a_ops,

		.write_sd_by_inode = write_sd_by_inode_common,
		.set_plug_in_inode = set_plug_in_inode_common,
		.adjust_to_parent = adjust_to_parent_common,
		.create_object = reiser4_create_object_common,
		.delete_object = reiser4_delete_object_common,
		.add_link = reiser4_add_link_common,
		.rem_link = reiser4_rem_link_common,
		.owns_item = owns_item_common,
		.can_add_link = can_add_link_common,
		.detach = dummyop,
		.bind = dummyop,
		.safelink = safelink_common,
		.estimate = {
			.create = estimate_create_common,
			.update = estimate_update_common,
			.unlink = estimate_unlink_common
		},
		.init_inode_data = init_inode_ordering,
		.cut_tree_worker = cut_tree_worker_common,
		.wire = {
			.write = wire_write_common,
			.read = wire_read_common,
			.get = wire_get_common,
			.size = wire_size_common,
			.done = wire_done_common
		}
	},
	[CRYPTCOMPRESS_FILE_PLUGIN_ID] = {
		.h = {
			.type_id = REISER4_FILE_PLUGIN_TYPE,
			.id = CRYPTCOMPRESS_FILE_PLUGIN_ID,
			.groups = (1 << REISER4_REGULAR_FILE),
			.pops = &file_plugin_ops,
			.label = "cryptcompress",
			.desc = "cryptcompress file",
			.linkage = {NULL, NULL}
		},
		.inode_ops = &regular_file_i_ops,
		.file_ops = &regular_file_f_ops,
		.as_ops = &regular_file_a_ops,

		.setattr = setattr_cryptcompress,
		.open = open_cryptcompress,
		.read = read_cryptcompress,
		.write = write_cryptcompress,
		.ioctl = ioctl_cryptcompress,
		.mmap = mmap_cryptcompress,
		.release = release_cryptcompress,

		.readpage = readpage_cryptcompress,
		.readpages = readpages_cryptcompress,
		.writepages = writepages_cryptcompress,
		.write_begin = write_begin_cryptcompress,
		.write_end = write_end_cryptcompress,

		.bmap = bmap_cryptcompress,

		.write_sd_by_inode = write_sd_by_inode_common,
		.flow_by_inode = flow_by_inode_cryptcompress,
		.key_by_inode = key_by_inode_cryptcompress,
		.set_plug_in_inode = set_plug_in_inode_common,
		.adjust_to_parent = adjust_to_parent_cryptcompress,
		.create_object = create_object_cryptcompress,
		.delete_object = delete_object_cryptcompress,
		.add_link = reiser4_add_link_common,
		.rem_link = reiser4_rem_link_common,
		.owns_item = owns_item_common,
		.can_add_link = can_add_link_common,
		.detach = dummyop,
		.bind = dummyop,
		.safelink = safelink_common,
		.estimate = {
			.create = estimate_create_common,
			.update = estimate_update_common,
			.unlink = estimate_unlink_common
		},
		.init_inode_data = init_inode_data_cryptcompress,
		.cut_tree_worker = cut_tree_worker_cryptcompress,
		.destroy_inode = destroy_inode_cryptcompress,
		.wire = {
			.write = wire_write_common,
			.read = wire_read_common,
			.get = wire_get_common,
			.size = wire_size_common,
			.done = wire_done_common
		}
	}
};

static int change_dir(struct inode *inode,
		      reiser4_plugin * plugin,
		      pset_member memb)
{
	/* cannot change dir plugin of already existing object */
	return RETERR(-EINVAL);
}

static reiser4_plugin_ops dir_plugin_ops = {
	.change = change_dir
};

/*
 * definition of directory plugins
 */

dir_plugin dir_plugins[LAST_DIR_ID] = {
	/* standard hashed directory plugin */
	[HASHED_DIR_PLUGIN_ID] = {
		.h = {
			.type_id = REISER4_DIR_PLUGIN_TYPE,
			.id = HASHED_DIR_PLUGIN_ID,
			.pops = &dir_plugin_ops,
			.label = "dir",
			.desc = "hashed directory",
			.linkage = {NULL, NULL}
		},
		.inode_ops = &directory_i_ops,
		.file_ops = &directory_f_ops,
		.as_ops = &directory_a_ops,

		.get_parent = get_parent_common,
		.is_name_acceptable = is_name_acceptable_common,
		.build_entry_key = build_entry_key_hashed,
		.build_readdir_key = build_readdir_key_common,
		.add_entry = reiser4_add_entry_common,
		.rem_entry = reiser4_rem_entry_common,
		.init = reiser4_dir_init_common,
		.done = reiser4_dir_done_common,
		.attach = reiser4_attach_common,
		.detach = reiser4_detach_common,
		.estimate = {
			.add_entry = estimate_add_entry_common,
			.rem_entry = estimate_rem_entry_common,
			.unlink = dir_estimate_unlink_common
		}
	},
	/* hashed directory for which seekdir/telldir are guaranteed to
	 * work. Brain-damage. */
	[SEEKABLE_HASHED_DIR_PLUGIN_ID] = {
		.h = {
			.type_id = REISER4_DIR_PLUGIN_TYPE,
			.id = SEEKABLE_HASHED_DIR_PLUGIN_ID,
			.pops = &dir_plugin_ops,
			.label = "dir32",
			.desc = "directory hashed with 31 bit hash",
			.linkage = {NULL, NULL}
		},
		.inode_ops = &directory_i_ops,
		.file_ops = &directory_f_ops,
		.as_ops = &directory_a_ops,

		.get_parent = get_parent_common,
		.is_name_acceptable = is_name_acceptable_common,
		.build_entry_key = build_entry_key_seekable,
		.build_readdir_key = build_readdir_key_common,
		.add_entry = reiser4_add_entry_common,
		.rem_entry = reiser4_rem_entry_common,
		.init = reiser4_dir_init_common,
		.done = reiser4_dir_done_common,
		.attach = reiser4_attach_common,
		.detach = reiser4_detach_common,
		.estimate = {
			.add_entry = estimate_add_entry_common,
			.rem_entry = estimate_rem_entry_common,
			.unlink = dir_estimate_unlink_common
		}
	}
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
