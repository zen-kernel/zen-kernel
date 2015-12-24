/* Copyright 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

#if !defined( __REISER4_TAIL_H__ )
#define __REISER4_TAIL_H__

struct tail_coord_extension {
	int not_used;
};

struct cut_list;

/* plugin->u.item.b.* */
reiser4_key *max_key_inside_tail(const coord_t *, reiser4_key *);
int can_contain_key_tail(const coord_t * coord, const reiser4_key * key,
			 const reiser4_item_data *);
int mergeable_tail(const coord_t * p1, const coord_t * p2);
pos_in_node_t nr_units_tail(const coord_t *);
lookup_result lookup_tail(const reiser4_key *, lookup_bias, coord_t *);
int paste_tail(coord_t *, reiser4_item_data *, carry_plugin_info *);
int can_shift_tail(unsigned free_space, coord_t * source,
		   znode * target, shift_direction, unsigned *size,
		   unsigned want);
void copy_units_tail(coord_t * target, coord_t * source, unsigned from,
		     unsigned count, shift_direction, unsigned free_space);
int kill_hook_tail(const coord_t *, pos_in_node_t from, pos_in_node_t count,
		   struct carry_kill_data *);
int cut_units_tail(coord_t *, pos_in_node_t from, pos_in_node_t to,
		   struct carry_cut_data *, reiser4_key * smallest_removed,
		   reiser4_key * new_first);
int kill_units_tail(coord_t *, pos_in_node_t from, pos_in_node_t to,
		    struct carry_kill_data *, reiser4_key * smallest_removed,
		    reiser4_key * new_first);
reiser4_key *unit_key_tail(const coord_t *, reiser4_key *);

/* plugin->u.item.s.* */
ssize_t reiser4_write_tail_noreserve(struct file *file, struct inode * inode,
				     const char __user *buf, size_t count,
				     loff_t *pos);
ssize_t reiser4_write_tail(struct file *file, struct inode * inode,
			   const char __user *buf, size_t count, loff_t *pos);
int reiser4_read_tail(struct file *, flow_t *, hint_t *);
int readpage_tail(void *vp, struct page *page);
reiser4_key *append_key_tail(const coord_t *, reiser4_key *);
void init_coord_extension_tail(uf_coord_t *, loff_t offset);
int get_block_address_tail(const coord_t *, sector_t, sector_t *);

/* __REISER4_TAIL_H__ */
#endif

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   scroll-step: 1
   End:
*/
