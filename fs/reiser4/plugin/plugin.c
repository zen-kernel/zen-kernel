/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Basic plugin infrastructure, lookup etc. */

/* PLUGINS:

   Plugins are internal Reiser4 "modules" or "objects" used to increase
   extensibility and allow external users to easily adapt reiser4 to
   their needs.

   Plugins are classified into several disjoint "types". Plugins
   belonging to the particular plugin type are termed "instances" of
   this type. Existing types are listed by enum reiser4_plugin_type
   (see plugin/plugin_header.h)

NIKITA-FIXME-HANS: update this list, and review this entire comment for currency

   Object (file) plugin determines how given file-system object serves
   standard VFS requests for read, write, seek, mmap etc. Instances of
   file plugins are: regular file, directory, symlink. Another example
   of file plugin is audit plugin, that optionally records accesses to
   underlying object and forwards requests to it.

   Hash plugins compute hashes used by reiser4 to store and locate
   files within directories. Instances of hash plugin type are: r5,
   tea, rupasov.

   Tail plugins (or, more precisely, tail policy plugins) determine
   when last part of the file should be stored in a formatted item.

   Scope and lookup:

   label such that pair ( type_label, plugin_label ) is unique.  This
   pair is a globally persistent and user-visible plugin
   identifier. Internally kernel maintains plugins and plugin types in
   arrays using an index into those arrays as plugin and plugin type
   identifiers. File-system in turn, also maintains persistent
   "dictionary" which is mapping from plugin label to numerical
   identifier which is stored in file-system objects.  That is, we
   store the offset into the plugin array for that plugin type as the
   plugin id in the stat data of the filesystem object.

   Internal kernel plugin type identifier (index in plugins[] array) is
   of type reiser4_plugin_type. Set of available plugin types is
   currently static, but dynamic loading doesn't seem to pose
   insurmountable problems.

   Within each type plugins are addressed by the identifiers of type
   reiser4_plugin_id (indices in reiser4_plugin_type_data.builtin[]).
   Such identifiers are only required to be unique within one type,
   not globally.

   Thus, plugin in memory is uniquely identified by the pair (type_id,
   id).

   Usage:

   There exists only one instance of each plugin instance, but this
   single instance can be associated with many entities (file-system
   objects, items, nodes, transactions, file-descriptors etc.). Entity
   to which plugin of given type is termed (due to the lack of
   imagination) "subject" of this plugin type and, by abuse of
   terminology, subject of particular instance of this type to which
   it's attached currently. For example, inode is subject of object
   plugin type. Inode representing directory is subject of directory
   plugin, hash plugin type and some particular instance of hash plugin
   type. Inode, representing regular file is subject of "regular file"
   plugin, tail-policy plugin type etc.

   With each subject the plugin possibly stores some state. For example,
   the state of a directory plugin (instance of object plugin type) is pointer
   to hash plugin (if directories always use hashing that is).

   Interface:

   In addition to a scalar identifier, each plugin type and plugin
   proper has a "label": short string and a "description"---longer
   descriptive string. Labels and descriptions of plugin types are
   hard-coded into plugins[] array, declared and defined in
   plugin.c. Label and description of plugin are stored in .label and
   .desc fields of reiser4_plugin_header respectively. It's possible to
   locate plugin by the pair of labels.

   Features (not implemented):

    . user-level plugin manipulations:
      + reiser4("filename/..file_plugin<='audit'");
      + write(open("filename/..file_plugin"), "audit", 8);

    . user level utilities lsplug and chplug to manipulate plugins.
      Utilities are not of primary priority. Possibly they will be not
      working on v4.0

   NIKITA-FIXME-HANS: this should be a mkreiserfs option not a mount
   option, do you agree?  I don't think that specifying it at mount time,
   and then changing it with each mount, is a good model for usage.

    . mount option "plug" to set-up plugins of root-directory.
      "plug=foo:bar" will set "bar" as default plugin of type "foo".

   Limitations:

    . each plugin type has to provide at least one builtin
      plugin. This is technical limitation and it can be lifted in the
      future.

   TODO:

   New plugin types/plugings:
   Things we should be able to separately choose to inherit:

   security plugins

   stat data

   file bodies

   file plugins

   dir plugins

    . perm:acl

    . audi---audit plugin intercepting and possibly logging all
      accesses to object. Requires to put stub functions in file_operations
      in stead of generic_file_*.

NIKITA-FIXME-HANS: why make overflows a plugin?
    . over---handle hash overflows

    . sqnt---handle different access patterns and instruments read-ahead

NIKITA-FIXME-HANS: describe the line below in more detail.

    . hier---handle inheritance of plugins along file-system hierarchy

   Different kinds of inheritance: on creation vs. on access.
   Compatible/incompatible plugins.
   Inheritance for multi-linked files.
   Layered plugins.
   Notion of plugin context is abandoned.

Each file is associated
   with one plugin and dependant plugins (hash, etc.) are stored as
   main plugin state. Now, if we have plugins used for regular files
   but not for directories, how such plugins would be inherited?
    . always store them with directories also

NIKTIA-FIXME-HANS: Do the line above.  It is not exclusive of doing
the line below which is also useful.

    . use inheritance hierarchy, independent of file-system namespace
*/

#include "../debug.h"
#include "../dformat.h"
#include "plugin_header.h"
#include "item/static_stat.h"
#include "node/node.h"
#include "security/perm.h"
#include "space/space_allocator.h"
#include "disk_format/disk_format.h"
#include "plugin.h"
#include "../reiser4.h"
#include "../jnode.h"
#include "../inode.h"

#include <linux/fs.h>		/* for struct super_block  */

/*
 * init_plugins - initialize plugin sub-system.
 * Just call this once on reiser4 startup.
 *
 * Initializes plugin sub-system. It is part of reiser4 module
 * initialization. For each plugin of each type init method is called and each
 * plugin is put into list of plugins.
 */
int init_plugins(void)
{
	reiser4_plugin_type type_id;

	for (type_id = 0; type_id < REISER4_PLUGIN_TYPES; ++type_id) {
		struct reiser4_plugin_type_data *ptype;
		int i;

		ptype = &plugins[type_id];
		assert("nikita-3508", ptype->label != NULL);
		assert("nikita-3509", ptype->type_id == type_id);

		INIT_LIST_HEAD(&ptype->plugins_list);
/* NIKITA-FIXME-HANS: change builtin_num to some other name lacking the term
 * builtin. */
		for (i = 0; i < ptype->builtin_num; ++i) {
			reiser4_plugin *plugin;

			plugin = plugin_at(ptype, i);

			if (plugin->h.label == NULL)
				/* uninitialized slot encountered */
				continue;
			assert("nikita-3445", plugin->h.type_id == type_id);
			plugin->h.id = i;
			if (plugin->h.pops != NULL &&
			    plugin->h.pops->init != NULL) {
				int result;

				result = plugin->h.pops->init(plugin);
				if (result != 0)
					return result;
			}
			INIT_LIST_HEAD(&plugin->h.linkage);
			list_add_tail(&plugin->h.linkage, &ptype->plugins_list);
		}
	}
	return 0;
}

/* true if plugin type id is valid */
int is_plugin_type_valid(reiser4_plugin_type type)
{
	/* "type" is unsigned, so no comparison with 0 is
	   necessary */
	return (type < REISER4_PLUGIN_TYPES);
}

/* true if plugin id is valid */
int is_plugin_id_valid(reiser4_plugin_type type, reiser4_plugin_id id)
{
	assert("nikita-1653", is_plugin_type_valid(type));
	return id < plugins[type].builtin_num;
}

/* return plugin by its @type and @id.

   Both arguments are checked for validness: this is supposed to be called
   from user-level.

NIKITA-FIXME-HANS: Do you instead mean that this checks ids created in
user space, and passed to the filesystem by use of method files? Your
comment really confused me on the first reading....

*/
reiser4_plugin *plugin_by_unsafe_id(reiser4_plugin_type type /* plugin type
								 * unchecked */,
				    reiser4_plugin_id id	/* plugin id,
								 * unchecked */)
{
	if (is_plugin_type_valid(type)) {
		if (is_plugin_id_valid(type, id))
			return plugin_at(&plugins[type], id);
		else
			/* id out of bounds */
			warning("nikita-2913",
				"Invalid plugin id: [%i:%i]", type, id);
	} else
		/* type_id out of bounds */
		warning("nikita-2914", "Invalid type_id: %i", type);
	return NULL;
}

/**
 * save_plugin_id - store plugin id in disk format
 * @plugin: plugin to convert
 * @area: where to store result
 *
 * Puts id of @plugin in little endian format to address @area.
 */
int save_plugin_id(reiser4_plugin *plugin /* plugin to convert */ ,
		   d16 * area/* where to store result */)
{
	assert("nikita-1261", plugin != NULL);
	assert("nikita-1262", area != NULL);

	put_unaligned(cpu_to_le16(plugin->h.id), area);
	return 0;
}

/* list of all plugins of given type */
struct list_head *get_plugin_list(reiser4_plugin_type type)
{
	assert("nikita-1056", is_plugin_type_valid(type));
	return &plugins[type].plugins_list;
}

static void update_pset_mask(reiser4_inode * info, pset_member memb)
{
	struct dentry *rootdir;
	reiser4_inode *root;

	assert("edward-1443", memb != PSET_FILE);

	rootdir = inode_by_reiser4_inode(info)->i_sb->s_root;
	if (rootdir != NULL) {
		root = reiser4_inode_data(rootdir->d_inode);
		/*
		 * if inode is different from the default one, or we are
		 * changing plugin of root directory, update plugin_mask
		 */
		if (aset_get(info->pset, memb) !=
		    aset_get(root->pset, memb) ||
		    info == root)
			info->plugin_mask |= (1 << memb);
		else
			info->plugin_mask &= ~(1 << memb);
	}
}

/* Get specified plugin set member from parent,
   or from fs-defaults (if no parent is given) and
   install the result to pset of @self */
int grab_plugin_pset(struct inode *self,
		     struct inode *ancestor,
		     pset_member memb)
{
	reiser4_plugin *plug;
	reiser4_inode *info;
	int result = 0;

	/* Do not grab if initialised already. */
	info = reiser4_inode_data(self);
	if (aset_get(info->pset, memb) != NULL)
		return 0;
	if (ancestor) {
		reiser4_inode *parent;

		parent = reiser4_inode_data(ancestor);
		plug = aset_get(parent->hset, memb) ? :
			aset_get(parent->pset, memb);
	} else
		plug = get_default_plugin(memb);

	result = set_plugin(&info->pset, memb, plug);
	if (result == 0) {
		if (!ancestor || self->i_sb->s_root->d_inode != self)
			update_pset_mask(info, memb);
	}
	return result;
}

/* Take missing pset members from root inode */
int finish_pset(struct inode *inode)
{
	reiser4_plugin *plug;
	reiser4_inode *root;
	reiser4_inode *info;
	pset_member memb;
	int result = 0;

	root = reiser4_inode_data(inode->i_sb->s_root->d_inode);
	info = reiser4_inode_data(inode);

	assert("edward-1455", root != NULL);
	assert("edward-1456", info != NULL);

	/* file and directory plugins are already initialized. */
	for (memb = PSET_DIR + 1; memb < PSET_LAST; ++memb) {

		/* Do not grab if initialised already. */
		if (aset_get(info->pset, memb) != NULL)
			continue;

		plug = aset_get(root->pset, memb);
		result = set_plugin(&info->pset, memb, plug);
		if (result != 0)
			break;
	}
	if (result != 0) {
		warning("nikita-3447",
			"Cannot set up plugins for %lli",
			(unsigned long long)
			get_inode_oid(inode));
	}
	return result;
}

int force_plugin_pset(struct inode *self, pset_member memb,
		      reiser4_plugin * plug)
{
	reiser4_inode *info;
	int result = 0;

	if (!self->i_sb->s_root || self->i_sb->s_root->d_inode == self) {
		/* Changing pset in the root object. */
		return RETERR(-EINVAL);
	}

	info = reiser4_inode_data(self);
	if (plug->h.pops != NULL && plug->h.pops->change != NULL)
		result = plug->h.pops->change(self, plug, memb);
	else
		result = aset_set_unsafe(&info->pset, memb, plug);
	if (result == 0) {
		__u16 oldmask = info->plugin_mask;

		update_pset_mask(info, memb);
		if (oldmask != info->plugin_mask)
			reiser4_inode_clr_flag(self, REISER4_SDLEN_KNOWN);
	}
	return result;
}

struct reiser4_plugin_type_data plugins[REISER4_PLUGIN_TYPES] = {
	/* C90 initializers */
	[REISER4_FILE_PLUGIN_TYPE] = {
		.type_id = REISER4_FILE_PLUGIN_TYPE,
		.label = "file",
		.desc = "Object plugins",
		.builtin_num = sizeof_array(file_plugins),
		.builtin = file_plugins,
		.plugins_list = {NULL, NULL},
		.size = sizeof(file_plugin)
	},
	[REISER4_DIR_PLUGIN_TYPE] = {
		.type_id = REISER4_DIR_PLUGIN_TYPE,
		.label = "dir",
		.desc = "Directory plugins",
		.builtin_num = sizeof_array(dir_plugins),
		.builtin = dir_plugins,
		.plugins_list = {NULL, NULL},
		.size = sizeof(dir_plugin)
	},
	[REISER4_HASH_PLUGIN_TYPE] = {
		.type_id = REISER4_HASH_PLUGIN_TYPE,
		.label = "hash",
		.desc = "Directory hashes",
		.builtin_num = sizeof_array(hash_plugins),
		.builtin = hash_plugins,
		.plugins_list = {NULL, NULL},
		.size = sizeof(hash_plugin)
	},
	[REISER4_FIBRATION_PLUGIN_TYPE] = {
		.type_id =
		REISER4_FIBRATION_PLUGIN_TYPE,
		.label = "fibration",
		.desc = "Directory fibrations",
		.builtin_num = sizeof_array(fibration_plugins),
		.builtin = fibration_plugins,
		.plugins_list =	{NULL, NULL},
		.size = sizeof(fibration_plugin)
	},
	[REISER4_CIPHER_PLUGIN_TYPE] = {
		.type_id = REISER4_CIPHER_PLUGIN_TYPE,
		.label = "cipher",
		.desc = "Cipher plugins",
		.builtin_num = sizeof_array(cipher_plugins),
		.builtin = cipher_plugins,
		.plugins_list =	{NULL, NULL},
		.size = sizeof(cipher_plugin)
	},
	[REISER4_DIGEST_PLUGIN_TYPE] = {
		.type_id = REISER4_DIGEST_PLUGIN_TYPE,
		.label = "digest",
		.desc = "Digest plugins",
		.builtin_num = sizeof_array(digest_plugins),
		.builtin = digest_plugins,
		.plugins_list =	{NULL, NULL},
		.size = sizeof(digest_plugin)
	},
	[REISER4_COMPRESSION_PLUGIN_TYPE] = {
		.type_id = REISER4_COMPRESSION_PLUGIN_TYPE,
		.label = "compression",
		.desc = "Compression plugins",
		.builtin_num = sizeof_array(compression_plugins),
		.builtin = compression_plugins,
		.plugins_list = {NULL, NULL},
		.size = sizeof(compression_plugin)
	},
	[REISER4_FORMATTING_PLUGIN_TYPE] = {
		.type_id = REISER4_FORMATTING_PLUGIN_TYPE,
		.label = "formatting",
		.desc = "Tail inlining policies",
		.builtin_num = sizeof_array(formatting_plugins),
		.builtin = formatting_plugins,
		.plugins_list = {NULL, NULL},
		.size = sizeof(formatting_plugin)
	},
	[REISER4_PERM_PLUGIN_TYPE] = {
		.type_id = REISER4_PERM_PLUGIN_TYPE,
		.label = "perm",
		.desc = "Permission checks",
		.builtin_num = sizeof_array(perm_plugins),
		.builtin = perm_plugins,
		.plugins_list = {NULL, NULL},
		.size = sizeof(perm_plugin)
	},
	[REISER4_ITEM_PLUGIN_TYPE] = {
		.type_id = REISER4_ITEM_PLUGIN_TYPE,
		.label = "item",
		.desc = "Item handlers",
		.builtin_num = sizeof_array(item_plugins),
		.builtin = item_plugins,
		.plugins_list = {NULL, NULL},
		.size = sizeof(item_plugin)
	},
	[REISER4_NODE_PLUGIN_TYPE] = {
		.type_id = REISER4_NODE_PLUGIN_TYPE,
		.label = "node",
		.desc = "node layout handlers",
		.builtin_num = sizeof_array(node_plugins),
		.builtin = node_plugins,
		.plugins_list = {NULL, NULL},
		.size = sizeof(node_plugin)
	},
	[REISER4_SD_EXT_PLUGIN_TYPE] = {
		.type_id = REISER4_SD_EXT_PLUGIN_TYPE,
		.label = "sd_ext",
		.desc = "Parts of stat-data",
		.builtin_num = sizeof_array(sd_ext_plugins),
		.builtin = sd_ext_plugins,
		.plugins_list = {NULL, NULL},
		.size = sizeof(sd_ext_plugin)
	},
	[REISER4_FORMAT_PLUGIN_TYPE] = {
		.type_id = REISER4_FORMAT_PLUGIN_TYPE,
		.label = "disk_layout",
		.desc = "defines filesystem on disk layout",
		.builtin_num = sizeof_array(format_plugins),
		.builtin = format_plugins,
		.plugins_list = {NULL, NULL},
		.size = sizeof(disk_format_plugin)
	},
	[REISER4_JNODE_PLUGIN_TYPE] = {
		.type_id = REISER4_JNODE_PLUGIN_TYPE,
		.label = "jnode",
		.desc = "defines kind of jnode",
		.builtin_num = sizeof_array(jnode_plugins),
		.builtin = jnode_plugins,
		.plugins_list = {NULL, NULL},
		.size = sizeof(jnode_plugin)
	},
	[REISER4_COMPRESSION_MODE_PLUGIN_TYPE] = {
		.type_id = REISER4_COMPRESSION_MODE_PLUGIN_TYPE,
		.label = "compression_mode",
		.desc = "Defines compression mode",
		.builtin_num = sizeof_array(compression_mode_plugins),
		.builtin = compression_mode_plugins,
		.plugins_list = {NULL, NULL},
		.size = sizeof(compression_mode_plugin)
	},
	[REISER4_CLUSTER_PLUGIN_TYPE] = {
		.type_id = REISER4_CLUSTER_PLUGIN_TYPE,
		.label = "cluster",
		.desc = "Defines cluster size",
		.builtin_num = sizeof_array(cluster_plugins),
		.builtin = cluster_plugins,
		.plugins_list = {NULL, NULL},
		.size = sizeof(cluster_plugin)
	},
	[REISER4_TXMOD_PLUGIN_TYPE] = {
		.type_id = REISER4_TXMOD_PLUGIN_TYPE,
		.label = "txmod",
		.desc = "Defines transaction model",
		.builtin_num = sizeof_array(txmod_plugins),
		.builtin = txmod_plugins,
		.plugins_list = {NULL, NULL},
		.size = sizeof(txmod_plugin)
	}
};

/*
 * Local variables:
 * c-indentation-style: "K&R"
 * mode-name: "LC"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 120
 * End:
 */
