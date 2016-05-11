/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

/* Compound directory item. See cde.c for description. */

#if !defined( __FS_REISER4_PLUGIN_COMPRESSED_DE_H__ )
#define __FS_REISER4_PLUGIN_COMPRESSED_DE_H__

#include "../../forward.h"
#include "../../kassign.h"
#include "../../dformat.h"

#include <linux/fs.h>		/* for struct inode */
#include <linux/dcache.h>	/* for struct dentry, etc  */

typedef struct cde_unit_header {
	de_id hash;
	d16 offset;
} cde_unit_header;

typedef struct cde_item_format {
	d16 num_of_entries;
	cde_unit_header entry[0];
} cde_item_format;

struct cde_entry {
	const struct inode *dir;
	const struct inode *obj;
	const struct qstr *name;
};

struct cde_entry_data {
	int num_of_entries;
	struct cde_entry *entry;
};

/* plugin->item.b.* */
reiser4_key *max_key_inside_cde(const coord_t * coord, reiser4_key * result);
int can_contain_key_cde(const coord_t * coord, const reiser4_key * key,
			const reiser4_item_data *);
int mergeable_cde(const coord_t * p1, const coord_t * p2);
pos_in_node_t nr_units_cde(const coord_t * coord);
reiser4_key *unit_key_cde(const coord_t * coord, reiser4_key * key);
int estimate_cde(const coord_t * coord, const reiser4_item_data * data);
void print_cde(const char *prefix, coord_t * coord);
int init_cde(coord_t * coord, coord_t * from, reiser4_item_data * data);
lookup_result lookup_cde(const reiser4_key * key, lookup_bias bias,
			 coord_t * coord);
int paste_cde(coord_t * coord, reiser4_item_data * data,
	      carry_plugin_info * info UNUSED_ARG);
int can_shift_cde(unsigned free_space, coord_t * coord, znode * target,
		  shift_direction pend, unsigned *size, unsigned want);
void copy_units_cde(coord_t * target, coord_t * source, unsigned from,
		    unsigned count, shift_direction where_is_free_space,
		    unsigned free_space);
int cut_units_cde(coord_t * coord, pos_in_node_t from, pos_in_node_t to,
		  struct carry_cut_data *, reiser4_key * smallest_removed,
		  reiser4_key * new_first);
int kill_units_cde(coord_t * coord, pos_in_node_t from, pos_in_node_t to,
		   struct carry_kill_data *, reiser4_key * smallest_removed,
		   reiser4_key * new_first);
void print_cde(const char *prefix, coord_t * coord);
int reiser4_check_cde(const coord_t * coord, const char **error);

/* plugin->u.item.s.dir.* */
int extract_key_cde(const coord_t * coord, reiser4_key * key);
int update_key_cde(const coord_t * coord, const reiser4_key * key,
		   lock_handle * lh);
char *extract_name_cde(const coord_t * coord, char *buf);
int add_entry_cde(struct inode *dir, coord_t * coord,
		  lock_handle * lh, const struct dentry *name,
		  reiser4_dir_entry_desc * entry);
int rem_entry_cde(struct inode *dir, const struct qstr *name, coord_t * coord,
		  lock_handle * lh, reiser4_dir_entry_desc * entry);
int max_name_len_cde(const struct inode *dir);

/* __FS_REISER4_PLUGIN_COMPRESSED_DE_H__ */
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
