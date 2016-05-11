/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

/* Directory entry implementation */
#include "../../forward.h"
#include "../../debug.h"
#include "../../dformat.h"
#include "../../kassign.h"
#include "../../coord.h"
#include "sde.h"
#include "item.h"
#include "../plugin.h"
#include "../../znode.h"
#include "../../carry.h"
#include "../../tree.h"
#include "../../inode.h"

#include <linux/fs.h>		/* for struct inode */
#include <linux/dcache.h>	/* for struct dentry */

/* ->extract_key() method of simple directory item plugin. */
int extract_key_de(const coord_t * coord /* coord of item */ ,
		   reiser4_key * key /* resulting key */ )
{
	directory_entry_format *dent;

	assert("nikita-1458", coord != NULL);
	assert("nikita-1459", key != NULL);

	dent = (directory_entry_format *) item_body_by_coord(coord);
	assert("nikita-1158", item_length_by_coord(coord) >= (int)sizeof *dent);
	return extract_key_from_id(&dent->id, key);
}

int
update_key_de(const coord_t * coord, const reiser4_key * key,
	      lock_handle * lh UNUSED_ARG)
{
	directory_entry_format *dent;
	obj_key_id obj_id;
	int result;

	assert("nikita-2342", coord != NULL);
	assert("nikita-2343", key != NULL);

	dent = (directory_entry_format *) item_body_by_coord(coord);
	result = build_obj_key_id(key, &obj_id);
	if (result == 0) {
		dent->id = obj_id;
		znode_make_dirty(coord->node);
	}
	return 0;
}

char *extract_dent_name(const coord_t * coord, directory_entry_format * dent,
			char *buf)
{
	reiser4_key key;

	unit_key_by_coord(coord, &key);
	if (get_key_type(&key) != KEY_FILE_NAME_MINOR)
		reiser4_print_address("oops", znode_get_block(coord->node));
	if (!is_longname_key(&key)) {
		if (is_dot_key(&key))
			return (char *)".";
		else
			return extract_name_from_key(&key, buf);
	} else
		return (char *)dent->name;
}

/* ->extract_name() method of simple directory item plugin. */
char *extract_name_de(const coord_t * coord /* coord of item */ , char *buf)
{
	directory_entry_format *dent;

	assert("nikita-1460", coord != NULL);

	dent = (directory_entry_format *) item_body_by_coord(coord);
	return extract_dent_name(coord, dent, buf);
}

/* ->extract_file_type() method of simple directory item plugin. */
unsigned extract_file_type_de(const coord_t * coord UNUSED_ARG	/* coord of
								 * item */ )
{
	assert("nikita-1764", coord != NULL);
	/* we don't store file type in the directory entry yet.

	   But see comments at kassign.h:obj_key_id
	 */
	return DT_UNKNOWN;
}

int add_entry_de(struct inode *dir /* directory of item */ ,
		 coord_t * coord /* coord of item */ ,
		 lock_handle * lh /* insertion lock handle */ ,
		 const struct dentry *de /* name to add */ ,
		 reiser4_dir_entry_desc * entry	/* parameters of new directory
						 * entry */ )
{
	reiser4_item_data data;
	directory_entry_format *dent;
	int result;
	const char *name;
	int len;
	int longname;

	name = de->d_name.name;
	len = de->d_name.len;
	assert("nikita-1163", strlen(name) == len);

	longname = is_longname(name, len);

	data.length = sizeof *dent;
	if (longname)
		data.length += len + 1;
	data.data = NULL;
	data.user = 0;
	data.iplug = item_plugin_by_id(SIMPLE_DIR_ENTRY_ID);

	inode_add_bytes(dir, data.length);

	result = insert_by_coord(coord, &data, &entry->key, lh, 0 /*flags */ );
	if (result != 0)
		return result;

	dent = (directory_entry_format *) item_body_by_coord(coord);
	build_inode_key_id(entry->obj, &dent->id);
	if (longname) {
		memcpy(dent->name, name, len);
		put_unaligned(0, &dent->name[len]);
	}
	return 0;
}

int rem_entry_de(struct inode *dir /* directory of item */ ,
		 const struct qstr *name UNUSED_ARG,
		 coord_t * coord /* coord of item */ ,
		 lock_handle * lh UNUSED_ARG	/* lock handle for
						 * removal */ ,
		 reiser4_dir_entry_desc * entry UNUSED_ARG	/* parameters of
								 * directory entry
								 * being removed */ )
{
	coord_t shadow;
	int result;
	int length;

	length = item_length_by_coord(coord);
	if (inode_get_bytes(dir) < length) {
		warning("nikita-2627", "Dir is broke: %llu: %llu",
			(unsigned long long)get_inode_oid(dir),
			inode_get_bytes(dir));

		return RETERR(-EIO);
	}

	/* cut_node() is supposed to take pointers to _different_
	   coords, because it will modify them without respect to
	   possible aliasing. To work around this, create temporary copy
	   of @coord.
	 */
	coord_dup(&shadow, coord);
	result =
	    kill_node_content(coord, &shadow, NULL, NULL, NULL, NULL, NULL, 0);
	if (result == 0) {
		inode_sub_bytes(dir, length);
	}
	return result;
}

int max_name_len_de(const struct inode *dir)
{
	return reiser4_tree_by_inode(dir)->nplug->max_item_size() -
		sizeof(directory_entry_format) - 2;
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
