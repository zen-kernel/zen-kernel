/* Copyright 2001, 2002, 2003, 2004 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Directory plugin using hashes (see fs/reiser4/plugin/hash.c) to map file
   names to the files. */

/*
 * Hashed directory logically consists of persistent directory
 * entries. Directory entry is a pair of a file name and a key of stat-data of
 * a file that has this name in the given directory.
 *
 * Directory entries are stored in the tree in the form of directory
 * items. Directory item should implement dir_entry_ops portion of item plugin
 * interface (see plugin/item/item.h). Hashed directory interacts with
 * directory item plugin exclusively through dir_entry_ops operations.
 *
 * Currently there are two implementations of directory items: "simple
 * directory item" (plugin/item/sde.[ch]), and "compound directory item"
 * (plugin/item/cde.[ch]) with the latter being the default.
 *
 * There is, however some delicate way through which directory code interferes
 * with item plugin: key assignment policy. A key for a directory item is
 * chosen by directory code, and as described in kassign.c, this key contains
 * a portion of file name. Directory item uses this knowledge to avoid storing
 * this portion of file name twice: in the key and in the directory item body.
 *
 */

#include "../../inode.h"

void complete_entry_key(const struct inode *, const char *name,
			int len, reiser4_key * result);

/* this is implementation of build_entry_key method of dir
   plugin for HASHED_DIR_PLUGIN_ID
 */
void build_entry_key_hashed(const struct inode *dir,	/* directory where entry is
							 * (or will be) in.*/
			    const struct qstr *qname,	/* name of file referenced
							 * by this entry */
			    reiser4_key * result	/* resulting key of directory
							 * entry */ )
{
	const char *name;
	int len;

	assert("nikita-1139", dir != NULL);
	assert("nikita-1140", qname != NULL);
	assert("nikita-1141", qname->name != NULL);
	assert("nikita-1142", result != NULL);

	name = qname->name;
	len = qname->len;

	assert("nikita-2867", strlen(name) == len);

	reiser4_key_init(result);
	/* locality of directory entry's key is objectid of parent
	   directory */
	set_key_locality(result, get_inode_oid(dir));
	/* minor packing locality is constant */
	set_key_type(result, KEY_FILE_NAME_MINOR);
	/* dot is special case---we always want it to be first entry in
	   a directory. Actually, we just want to have smallest
	   directory entry.
	 */
	if (len == 1 && name[0] == '.')
		return;

	/* initialize part of entry key which depends on file name */
	complete_entry_key(dir, name, len, result);
}

/* Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
