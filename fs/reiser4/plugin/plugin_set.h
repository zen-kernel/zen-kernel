/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Reiser4 plugin set definition.
   See fs/reiser4/plugin/plugin_set.c for details */

#if !defined(__PLUGIN_SET_H__)
#define __PLUGIN_SET_H__

#include "../type_safe_hash.h"
#include "plugin.h"

#include <linux/rcupdate.h>

struct plugin_set;
typedef struct plugin_set plugin_set;

TYPE_SAFE_HASH_DECLARE(ps, plugin_set);

struct plugin_set {
	unsigned long hashval;
	/* plugin of file */
	file_plugin *file;
	/* plugin of dir */
	dir_plugin *dir;
	/* perm plugin for this file */
	perm_plugin *perm;
	/* tail policy plugin. Only meaningful for regular files */
	formatting_plugin *formatting;
	/* hash plugin. Only meaningful for directories. */
	hash_plugin *hash;
	/* fibration plugin. Only meaningful for directories. */
	fibration_plugin *fibration;
	/* plugin of stat-data */
	item_plugin *sd;
	/* plugin of items a directory is built of */
	item_plugin *dir_item;
	/* cipher plugin */
	cipher_plugin *cipher;
	/* digest plugin */
	digest_plugin *digest;
	/* compression plugin */
	compression_plugin *compression;
	/* compression mode plugin */
	compression_mode_plugin *compression_mode;
	/* cluster plugin */
	cluster_plugin *cluster;
	/* this specifies file plugin of regular children.
	   only meaningful for directories */
	file_plugin *create;
	ps_hash_link link;
};

extern plugin_set *plugin_set_get_empty(void);
extern void plugin_set_put(plugin_set * set);

extern int init_plugin_set(void);
extern void done_plugin_set(void);

extern reiser4_plugin *aset_get(plugin_set * set, pset_member memb);
extern int set_plugin(plugin_set ** set, pset_member memb,
		      reiser4_plugin * plugin);
extern int aset_set_unsafe(plugin_set ** set, pset_member memb,
			   reiser4_plugin * plugin);
extern reiser4_plugin_type aset_member_to_type_unsafe(pset_member memb);

/* __PLUGIN_SET_H__ */
#endif

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
