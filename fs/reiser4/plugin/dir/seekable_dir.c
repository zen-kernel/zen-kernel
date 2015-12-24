/* Copyright 2005 by Hans Reiser, licensing governed by
 * reiser4/README */

#include "../../inode.h"

/* this is implementation of build_entry_key method of dir
   plugin for SEEKABLE_HASHED_DIR_PLUGIN_ID
   This is for directories where we want repeatable and restartable readdir()
   even in case 32bit user level struct dirent (readdir(3)).
*/
void
build_entry_key_seekable(const struct inode *dir, const struct qstr *name,
			 reiser4_key * result)
{
	oid_t objectid;

	assert("nikita-2283", dir != NULL);
	assert("nikita-2284", name != NULL);
	assert("nikita-2285", name->name != NULL);
	assert("nikita-2286", result != NULL);

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
	if ((name->len == 1) && (name->name[0] == '.'))
		return;

	/* objectid of key is 31 lowest bits of hash. */
	objectid =
	    inode_hash_plugin(dir)->hash(name->name,
					 (int)name->len) & 0x7fffffff;

	assert("nikita-2303", !(objectid & ~KEY_OBJECTID_MASK));
	set_key_objectid(result, objectid);

	/* offset is always 0. */
	set_key_offset(result, (__u64) 0);
	return;
}
