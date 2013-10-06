/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

/* Directory entry. */

#if !defined( __FS_REISER4_PLUGIN_DIRECTORY_ENTRY_H__ )
#define __FS_REISER4_PLUGIN_DIRECTORY_ENTRY_H__

#include "../../forward.h"
#include "../../dformat.h"
#include "../../kassign.h"
#include "../../key.h"

#include <linux/fs.h>
#include <linux/dcache.h>	/* for struct dentry */

typedef struct directory_entry_format {
	/* key of object stat-data. It's not necessary to store whole
	   key here, because it's always key of stat-data, so minor
	   packing locality and offset can be omitted here. But this
	   relies on particular key allocation scheme for stat-data, so,
	   for extensibility sake, whole key can be stored here.

	   We store key as array of bytes, because we don't want 8-byte
	   alignment of dir entries.
	 */
	obj_key_id id;
	/* file name. Null terminated string. */
	d8 name[0];
} directory_entry_format;

void print_de(const char *prefix, coord_t * coord);
int extract_key_de(const coord_t * coord, reiser4_key * key);
int update_key_de(const coord_t * coord, const reiser4_key * key,
		  lock_handle * lh);
char *extract_name_de(const coord_t * coord, char *buf);
unsigned extract_file_type_de(const coord_t * coord);
int add_entry_de(struct inode *dir, coord_t * coord,
		 lock_handle * lh, const struct dentry *name,
		 reiser4_dir_entry_desc * entry);
int rem_entry_de(struct inode *dir, const struct qstr *name, coord_t * coord,
		 lock_handle * lh, reiser4_dir_entry_desc * entry);
int max_name_len_de(const struct inode *dir);

int de_rem_and_shrink(struct inode *dir, coord_t * coord, int length);

char *extract_dent_name(const coord_t * coord,
			directory_entry_format * dent, char *buf);

#if REISER4_LARGE_KEY
#define DE_NAME_BUF_LEN (24)
#else
#define DE_NAME_BUF_LEN (16)
#endif

/* __FS_REISER4_PLUGIN_DIRECTORY_ENTRY_H__ */
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
