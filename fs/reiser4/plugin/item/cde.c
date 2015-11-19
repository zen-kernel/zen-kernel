/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

/* Directory entry implementation */

/* DESCRIPTION:

   This is "compound" directory item plugin implementation. This directory
   item type is compound (as opposed to the "simple directory item" in
   fs/reiser4/plugin/item/sde.[ch]), because it consists of several directory
   entries.

   The reason behind this decision is disk space efficiency: all directory
   entries inside the same directory have identical fragment in their
   keys. This, of course, depends on key assignment policy. In our default key
   assignment policy, all directory entries have the same locality which is
   equal to the object id of their directory.

   Composing directory item out of several directory entries for the same
   directory allows us to store said key fragment only once. That is, this is
   some ad hoc form of key compression (stem compression) that is implemented
   here, because general key compression is not supposed to be implemented in
   v4.0.

   Another decision that was made regarding all directory item plugins, is
   that they will store entry keys unaligned. This is for that sake of disk
   space efficiency again.

   In should be noted, that storing keys unaligned increases CPU consumption,
   at least on some architectures.

   Internal on-disk structure of the compound directory item is the following:

        HEADER          cde_item_format.        Here number of entries is stored.
        ENTRY_HEADER_0  cde_unit_header.        Here part of entry key and
        ENTRY_HEADER_1                          offset of entry body are stored.
        ENTRY_HEADER_2				(basically two last parts of key)
        ...
        ENTRY_HEADER_N
        ENTRY_BODY_0    directory_entry_format. Here part of stat data key and
        ENTRY_BODY_1                            NUL-terminated name are stored.
        ENTRY_BODY_2				(part of statadta key in the
  						 sence that since all SDs have
  						 zero offset, this offset is not
  						 stored on disk).
        ...
        ENTRY_BODY_N

   When it comes to the balancing, each directory entry in compound directory
   item is unit, that is, something that can be cut from one item and pasted
   into another item of the same type. Handling of unit cut and paste is major
   reason for the complexity of code below.

*/

#include "../../forward.h"
#include "../../debug.h"
#include "../../dformat.h"
#include "../../kassign.h"
#include "../../key.h"
#include "../../coord.h"
#include "sde.h"
#include "cde.h"
#include "item.h"
#include "../node/node.h"
#include "../plugin.h"
#include "../../znode.h"
#include "../../carry.h"
#include "../../tree.h"
#include "../../inode.h"

#include <linux/fs.h>		/* for struct inode */
#include <linux/dcache.h>	/* for struct dentry */

#if 0
#define CHECKME(coord)						\
({								\
	const char *message;					\
	coord_t dup;						\
								\
	coord_dup_nocheck(&dup, (coord));			\
	dup.unit_pos = 0;					\
	assert("nikita-2871", cde_check(&dup, &message) == 0);	\
})
#else
#define CHECKME(coord) noop
#endif

/* return body of compound directory item at @coord */
static inline cde_item_format *formatted_at(const coord_t * coord)
{
	assert("nikita-1282", coord != NULL);
	return item_body_by_coord(coord);
}

/* return entry header at @coord */
static inline cde_unit_header *header_at(const coord_t *
					 coord /* coord of item */ ,
					 int idx /* index of unit */ )
{
	assert("nikita-1283", coord != NULL);
	return &formatted_at(coord)->entry[idx];
}

/* return number of units in compound directory item at @coord */
static int units(const coord_t * coord /* coord of item */ )
{
	return le16_to_cpu(get_unaligned(&formatted_at(coord)->num_of_entries));
}

/* return offset of the body of @idx-th entry in @coord */
static unsigned int offset_of(const coord_t * coord /* coord of item */ ,
			      int idx /* index of unit */ )
{
	if (idx < units(coord))
		return le16_to_cpu(get_unaligned(&header_at(coord, idx)->offset));
	else if (idx == units(coord))
		return item_length_by_coord(coord);
	else
		impossible("nikita-1308", "Wrong idx");
	return 0;
}

/* set offset of the body of @idx-th entry in @coord */
static void set_offset(const coord_t * coord /* coord of item */ ,
		       int idx /* index of unit */ ,
		       unsigned int offset /* new offset */ )
{
	put_unaligned(cpu_to_le16((__u16) offset), &header_at(coord, idx)->offset);
}

static void adj_offset(const coord_t * coord /* coord of item */ ,
		       int idx /* index of unit */ ,
		       int delta /* offset change */ )
{
	d16 *doffset;
	__u16 offset;

	doffset = &header_at(coord, idx)->offset;
	offset = le16_to_cpu(get_unaligned(doffset));
	offset += delta;
	put_unaligned(cpu_to_le16((__u16) offset), doffset);
}

/* return pointer to @offset-th byte from the beginning of @coord */
static char *address(const coord_t * coord /* coord of item */ ,
		     int offset)
{
	return ((char *)item_body_by_coord(coord)) + offset;
}

/* return pointer to the body of @idx-th entry in @coord */
static directory_entry_format *entry_at(const coord_t * coord	/* coord of
								 * item */ ,
					int idx /* index of unit */ )
{
	return (directory_entry_format *) address(coord,
						  (int)offset_of(coord, idx));
}

/* return number of unit referenced by @coord */
static int idx_of(const coord_t * coord /* coord of item */ )
{
	assert("nikita-1285", coord != NULL);
	return coord->unit_pos;
}

/* find position where entry with @entry_key would be inserted into @coord */
static int find(const coord_t * coord /* coord of item */ ,
		const reiser4_key * entry_key /* key to look for */ ,
		cmp_t * last /* result of last comparison */ )
{
	int entries;

	int left;
	int right;

	cde_unit_header *header;

	assert("nikita-1295", coord != NULL);
	assert("nikita-1296", entry_key != NULL);
	assert("nikita-1297", last != NULL);

	entries = units(coord);
	left = 0;
	right = entries - 1;
	while (right - left >= REISER4_SEQ_SEARCH_BREAK) {
		int median;

		median = (left + right) >> 1;

		header = header_at(coord, median);
		*last = de_id_key_cmp(&header->hash, entry_key);
		switch (*last) {
		case LESS_THAN:
			left = median;
			break;
		case GREATER_THAN:
			right = median;
			break;
		case EQUAL_TO:{
				do {
					median--;
					header--;
				} while (median >= 0 &&
					 de_id_key_cmp(&header->hash,
						       entry_key) == EQUAL_TO);
				return median + 1;
			}
		}
	}
	header = header_at(coord, left);
	for (; left < entries; ++left, ++header) {
		prefetch(header + 1);
		*last = de_id_key_cmp(&header->hash, entry_key);
		if (*last != LESS_THAN)
			break;
	}
	if (left < entries)
		return left;
	else
		return RETERR(-ENOENT);

}

/* expand @coord as to accommodate for insertion of @no new entries starting
   from @pos, with total bodies size @size. */
static int expand_item(const coord_t * coord /* coord of item */ ,
		       int pos /* unit position */ , int no	/* number of new
								 * units*/ ,
		       int size /* total size of new units' data */ ,
		       unsigned int data_size	/* free space already reserved
						 * in the item for insertion */ )
{
	int entries;
	cde_unit_header *header;
	char *dent;
	int i;

	assert("nikita-1310", coord != NULL);
	assert("nikita-1311", pos >= 0);
	assert("nikita-1312", no > 0);
	assert("nikita-1313", data_size >= no * sizeof(directory_entry_format));
	assert("nikita-1343",
	       item_length_by_coord(coord) >=
	       (int)(size + data_size + no * sizeof *header));

	entries = units(coord);

	if (pos == entries)
		dent = address(coord, size);
	else
		dent = (char *)entry_at(coord, pos);
	/* place where new header will be in */
	header = header_at(coord, pos);
	/* free space for new entry headers */
	memmove(header + no, header,
		(unsigned)(address(coord, size) - (char *)header));
	/* if adding to the end initialise first new header */
	if (pos == entries) {
		set_offset(coord, pos, (unsigned)size);
	}

	/* adjust entry pointer and size */
	dent = dent + no * sizeof *header;
	size += no * sizeof *header;
	/* free space for new entries */
	memmove(dent + data_size, dent,
		(unsigned)(address(coord, size) - dent));

	/* increase counter */
	entries += no;
	put_unaligned(cpu_to_le16((__u16) entries), &formatted_at(coord)->num_of_entries);

	/* [ 0 ... pos ] entries were shifted by no * ( sizeof *header )
	   bytes.  */
	for (i = 0; i <= pos; ++i)
		adj_offset(coord, i, no * sizeof *header);
	/* [ pos + no ... +\infty ) entries were shifted by ( no *
	   sizeof *header + data_size ) bytes */
	for (i = pos + no; i < entries; ++i)
		adj_offset(coord, i, no * sizeof *header + data_size);
	return 0;
}

/* insert new @entry into item */
static int expand(const coord_t * coord /* coord of item */ ,
		  struct cde_entry * entry /* entry to insert */ ,
		  int len /* length of @entry data */ ,
		  int *pos /* position to insert */ ,
		  reiser4_dir_entry_desc * dir_entry	/* parameters for new
							 * entry */ )
{
	cmp_t cmp_res;
	int datasize;

	*pos = find(coord, &dir_entry->key, &cmp_res);
	if (*pos < 0)
		*pos = units(coord);

	datasize = sizeof(directory_entry_format);
	if (is_longname(entry->name->name, entry->name->len))
		datasize += entry->name->len + 1;

	expand_item(coord, *pos, 1, item_length_by_coord(coord) - len,
		    datasize);
	return 0;
}

/* paste body of @entry into item */
static int paste_entry(const coord_t * coord /* coord of item */ ,
		       struct cde_entry * entry /* new entry */ ,
		       int pos /* position to insert */ ,
		       reiser4_dir_entry_desc * dir_entry	/* parameters for
								 * new entry */ )
{
	cde_unit_header *header;
	directory_entry_format *dent;
	const char *name;
	int len;

	header = header_at(coord, pos);
	dent = entry_at(coord, pos);

	build_de_id_by_key(&dir_entry->key, &header->hash);
	build_inode_key_id(entry->obj, &dent->id);
	/* AUDIT unsafe strcpy() operation! It should be replaced with
	   much less CPU hungry
	   memcpy( ( char * ) dent -> name, entry -> name -> name , entry -> name -> len );

	   Also a more major thing is that there should be a way to figure out
	   amount of space in dent -> name and be able to check that we are
	   not going to overwrite more than we supposed to */
	name = entry->name->name;
	len = entry->name->len;
	if (is_longname(name, len)) {
		strcpy((unsigned char *)dent->name, name);
		put_unaligned(0, &dent->name[len]);
	}
	return 0;
}

/* estimate how much space is necessary in item to insert/paste set of entries
   described in @data. */
int estimate_cde(const coord_t * coord /* coord of item */ ,
		 const reiser4_item_data * data /* parameters for new item */ )
{
	struct cde_entry_data *e;
	int result;
	int i;

	e = (struct cde_entry_data *) data->data;

	assert("nikita-1288", e != NULL);
	assert("nikita-1289", e->num_of_entries >= 0);

	if (coord == NULL)
		/* insert */
		result = sizeof(cde_item_format);
	else
		/* paste */
		result = 0;

	result += e->num_of_entries *
	    (sizeof(cde_unit_header) + sizeof(directory_entry_format));
	for (i = 0; i < e->num_of_entries; ++i) {
		const char *name;
		int len;

		name = e->entry[i].name->name;
		len = e->entry[i].name->len;
		assert("nikita-2054", strlen(name) == len);
		if (is_longname(name, len))
			result += len + 1;
	}
	((reiser4_item_data *) data)->length = result;
	return result;
}

/* ->nr_units() method for this item plugin. */
pos_in_node_t nr_units_cde(const coord_t * coord /* coord of item */ )
{
	return units(coord);
}

/* ->unit_key() method for this item plugin. */
reiser4_key *unit_key_cde(const coord_t * coord /* coord of item */ ,
			  reiser4_key * key /* resulting key */ )
{
	assert("nikita-1452", coord != NULL);
	assert("nikita-1345", idx_of(coord) < units(coord));
	assert("nikita-1346", key != NULL);

	item_key_by_coord(coord, key);
	extract_key_from_de_id(extract_dir_id_from_key(key),
			       &header_at(coord, idx_of(coord))->hash, key);
	return key;
}

/* mergeable_cde(): implementation of ->mergeable() item method.

   Two directory items are mergeable iff they are from the same
   directory. That simple.

*/
int mergeable_cde(const coord_t * p1 /* coord of first item */ ,
		  const coord_t * p2 /* coord of second item */ )
{
	reiser4_key k1;
	reiser4_key k2;

	assert("nikita-1339", p1 != NULL);
	assert("nikita-1340", p2 != NULL);

	return
	    (item_plugin_by_coord(p1) == item_plugin_by_coord(p2)) &&
	    (extract_dir_id_from_key(item_key_by_coord(p1, &k1)) ==
	     extract_dir_id_from_key(item_key_by_coord(p2, &k2)));

}

/* ->max_key_inside() method for this item plugin. */
reiser4_key *max_key_inside_cde(const coord_t * coord /* coord of item */ ,
				reiser4_key * result /* resulting key */ )
{
	assert("nikita-1342", coord != NULL);

	item_key_by_coord(coord, result);
	set_key_ordering(result, get_key_ordering(reiser4_max_key()));
	set_key_fulloid(result, get_key_fulloid(reiser4_max_key()));
	set_key_offset(result, get_key_offset(reiser4_max_key()));
	return result;
}

/* @data contains data which are to be put into tree */
int can_contain_key_cde(const coord_t * coord /* coord of item */ ,
			const reiser4_key * key /* key to check */ ,
			const reiser4_item_data * data	/* parameters of new
							 * item/unit being
							 * created */ )
{
	reiser4_key item_key;

	/* FIXME-VS: do not rely on anything but iplug field of @data. Only
	   data->iplug is initialized */
	assert("vs-457", data && data->iplug);
/*	assert( "vs-553", data -> user == 0 );*/
	item_key_by_coord(coord, &item_key);

	return (item_plugin_by_coord(coord) == data->iplug) &&
	    (extract_dir_id_from_key(&item_key) ==
	     extract_dir_id_from_key(key));
}

#if REISER4_DEBUG
/* cde_check ->check() method for compressed directory items

   used for debugging, every item should have here the most complete
   possible check of the consistency of the item that the inventor can
   construct
*/
int reiser4_check_cde(const coord_t * coord /* coord of item to check */,
		      const char **error /* where to store error message */)
{
	int i;
	int result;
	char *item_start;
	char *item_end;
	reiser4_key key;

	coord_t c;

	assert("nikita-1357", coord != NULL);
	assert("nikita-1358", error != NULL);

	if (!ergo(coord->item_pos != 0,
		  is_dot_key(item_key_by_coord(coord, &key)))) {
		*error = "CDE doesn't start with dot";
		return -1;
	}
	item_start = item_body_by_coord(coord);
	item_end = item_start + item_length_by_coord(coord);

	coord_dup(&c, coord);
	result = 0;
	for (i = 0; i < units(coord); ++i) {
		directory_entry_format *entry;

		if ((char *)(header_at(coord, i) + 1) >
		    item_end - units(coord) * sizeof *entry) {
			*error = "CDE header is out of bounds";
			result = -1;
			break;
		}
		entry = entry_at(coord, i);
		if ((char *)entry < item_start + sizeof(cde_item_format)) {
			*error = "CDE header is too low";
			result = -1;
			break;
		}
		if ((char *)(entry + 1) > item_end) {
			*error = "CDE header is too high";
			result = -1;
			break;
		}
	}

	return result;
}
#endif

/* ->init() method for this item plugin. */
int init_cde(coord_t * coord /* coord of item */ ,
	     coord_t * from UNUSED_ARG, reiser4_item_data * data	/* structure used for insertion */
	     UNUSED_ARG)
{
	put_unaligned(cpu_to_le16(0), &formatted_at(coord)->num_of_entries);
	return 0;
}

/* ->lookup() method for this item plugin. */
lookup_result lookup_cde(const reiser4_key * key /* key to search for */ ,
			 lookup_bias bias /* search bias */ ,
			 coord_t * coord /* coord of item to lookup in */ )
{
	cmp_t last_comp;
	int pos;

	reiser4_key utmost_key;

	assert("nikita-1293", coord != NULL);
	assert("nikita-1294", key != NULL);

	CHECKME(coord);

	if (keygt(item_key_by_coord(coord, &utmost_key), key)) {
		coord->unit_pos = 0;
		coord->between = BEFORE_UNIT;
		return CBK_COORD_NOTFOUND;
	}
	pos = find(coord, key, &last_comp);
	if (pos >= 0) {
		coord->unit_pos = (int)pos;
		switch (last_comp) {
		case EQUAL_TO:
			coord->between = AT_UNIT;
			return CBK_COORD_FOUND;
		case GREATER_THAN:
			coord->between = BEFORE_UNIT;
			return RETERR(-ENOENT);
		case LESS_THAN:
		default:
			impossible("nikita-1298", "Broken find");
			return RETERR(-EIO);
		}
	} else {
		coord->unit_pos = units(coord) - 1;
		coord->between = AFTER_UNIT;
		return (bias ==
			FIND_MAX_NOT_MORE_THAN) ? CBK_COORD_FOUND :
		    CBK_COORD_NOTFOUND;
	}
}

/* ->paste() method for this item plugin. */
int paste_cde(coord_t * coord /* coord of item */ ,
	      reiser4_item_data * data	/* parameters of new unit being
					 * inserted */ ,
	      carry_plugin_info * info UNUSED_ARG /* todo carry queue */ )
{
	struct cde_entry_data *e;
	int result;
	int i;

	CHECKME(coord);
	e = (struct cde_entry_data *) data->data;

	result = 0;
	for (i = 0; i < e->num_of_entries; ++i) {
		int pos;
		int phantom_size;

		phantom_size = data->length;
		if (units(coord) == 0)
			phantom_size -= sizeof(cde_item_format);

		result =
		    expand(coord, e->entry + i, phantom_size, &pos, data->arg);
		if (result != 0)
			break;
		result = paste_entry(coord, e->entry + i, pos, data->arg);
		if (result != 0)
			break;
	}
	CHECKME(coord);
	return result;
}

/* amount of space occupied by all entries starting from @idx both headers and
   bodies. */
static unsigned int part_size(const coord_t * coord /* coord of item */ ,
			      int idx /* index of unit */ )
{
	assert("nikita-1299", coord != NULL);
	assert("nikita-1300", idx < (int)units(coord));

	return sizeof(cde_item_format) +
	    (idx + 1) * sizeof(cde_unit_header) + offset_of(coord,
							    idx + 1) -
	    offset_of(coord, 0);
}

/* how many but not more than @want units of @source can be merged with
   item in @target node. If pend == append - we try to append last item
   of @target by first units of @source. If pend == prepend - we try to
   "prepend" first item in @target by last units of @source. @target
   node has @free_space bytes of free space. Total size of those units
   are returned via @size */
int can_shift_cde(unsigned free_space /* free space in item */ ,
		  coord_t * coord /* coord of source item */ ,
		  znode * target /* target node */ ,
		  shift_direction pend /* shift direction */ ,
		  unsigned *size /* resulting number of shifted bytes */ ,
		  unsigned want /* maximal number of bytes to shift */ )
{
	int shift;

	CHECKME(coord);
	if (want == 0) {
		*size = 0;
		return 0;
	}

	/* pend == SHIFT_LEFT <==> shifting to the left */
	if (pend == SHIFT_LEFT) {
		for (shift = min((int)want - 1, units(coord)); shift >= 0;
		     --shift) {
			*size = part_size(coord, shift);
			if (target != NULL)
				*size -= sizeof(cde_item_format);
			if (*size <= free_space)
				break;
		}
		shift = shift + 1;
	} else {
		int total_size;

		assert("nikita-1301", pend == SHIFT_RIGHT);

		total_size = item_length_by_coord(coord);
		for (shift = units(coord) - want - 1; shift < units(coord) - 1;
		     ++shift) {
			*size = total_size - part_size(coord, shift);
			if (target == NULL)
				*size += sizeof(cde_item_format);
			if (*size <= free_space)
				break;
		}
		shift = units(coord) - shift - 1;
	}
	if (shift == 0)
		*size = 0;
	CHECKME(coord);
	return shift;
}

/* ->copy_units() method for this item plugin. */
void copy_units_cde(coord_t * target /* coord of target item */ ,
		    coord_t * source /* coord of source item */ ,
		    unsigned from /* starting unit */ ,
		    unsigned count /* how many units to copy */ ,
		    shift_direction where_is_free_space /* shift direction */ ,
		    unsigned free_space /* free space in item */ )
{
	char *header_from;
	char *header_to;

	char *entry_from;
	char *entry_to;

	int pos_in_target;
	int data_size;
	int data_delta;
	int i;

	assert("nikita-1303", target != NULL);
	assert("nikita-1304", source != NULL);
	assert("nikita-1305", (int)from < units(source));
	assert("nikita-1307", (int)(from + count) <= units(source));

	if (where_is_free_space == SHIFT_LEFT) {
		assert("nikita-1453", from == 0);
		pos_in_target = units(target);
	} else {
		assert("nikita-1309", (int)(from + count) == units(source));
		pos_in_target = 0;
		memmove(item_body_by_coord(target),
			(char *)item_body_by_coord(target) + free_space,
			item_length_by_coord(target) - free_space);
	}

	CHECKME(target);
	CHECKME(source);

	/* expand @target */
	data_size =
	    offset_of(source, (int)(from + count)) - offset_of(source,
							       (int)from);

	if (units(target) == 0)
		free_space -= sizeof(cde_item_format);

	expand_item(target, pos_in_target, (int)count,
		    (int)(item_length_by_coord(target) - free_space),
		    (unsigned)data_size);

	/* copy first @count units of @source into @target */
	data_delta =
	    offset_of(target, pos_in_target) - offset_of(source, (int)from);

	/* copy entries */
	entry_from = (char *)entry_at(source, (int)from);
	entry_to = (char *)entry_at(source, (int)(from + count));
	memmove(entry_at(target, pos_in_target), entry_from,
		(unsigned)(entry_to - entry_from));

	/* copy headers */
	header_from = (char *)header_at(source, (int)from);
	header_to = (char *)header_at(source, (int)(from + count));
	memmove(header_at(target, pos_in_target), header_from,
		(unsigned)(header_to - header_from));

	/* update offsets */
	for (i = pos_in_target; i < (int)(pos_in_target + count); ++i)
		adj_offset(target, i, data_delta);
	CHECKME(target);
	CHECKME(source);
}

/* ->cut_units() method for this item plugin. */
int cut_units_cde(coord_t * coord /* coord of item */ ,
		  pos_in_node_t from /* start unit pos */ ,
		  pos_in_node_t to /* stop unit pos */ ,
		  struct carry_cut_data *cdata UNUSED_ARG,
		  reiser4_key * smallest_removed, reiser4_key * new_first)
{
	char *header_from;
	char *header_to;

	char *entry_from;
	char *entry_to;

	int size;
	int entry_delta;
	int header_delta;
	int i;

	unsigned count;

	CHECKME(coord);

	count = to - from + 1;

	assert("nikita-1454", coord != NULL);
	assert("nikita-1455", (int)(from + count) <= units(coord));

	if (smallest_removed)
		unit_key_by_coord(coord, smallest_removed);

	if (new_first) {
		coord_t next;

		/* not everything is cut from item head */
		assert("vs-1527", from == 0);
		assert("vs-1528", to < units(coord) - 1);

		coord_dup(&next, coord);
		next.unit_pos++;
		unit_key_by_coord(&next, new_first);
	}

	size = item_length_by_coord(coord);
	if (count == (unsigned)units(coord)) {
		return size;
	}

	header_from = (char *)header_at(coord, (int)from);
	header_to = (char *)header_at(coord, (int)(from + count));

	entry_from = (char *)entry_at(coord, (int)from);
	entry_to = (char *)entry_at(coord, (int)(from + count));

	/* move headers */
	memmove(header_from, header_to,
		(unsigned)(address(coord, size) - header_to));

	header_delta = header_to - header_from;

	entry_from -= header_delta;
	entry_to -= header_delta;
	size -= header_delta;

	/* copy entries */
	memmove(entry_from, entry_to,
		(unsigned)(address(coord, size) - entry_to));

	entry_delta = entry_to - entry_from;
	size -= entry_delta;

	/* update offsets */

	for (i = 0; i < (int)from; ++i)
		adj_offset(coord, i, -header_delta);

	for (i = from; i < units(coord) - (int)count; ++i)
		adj_offset(coord, i, -header_delta - entry_delta);

	put_unaligned(cpu_to_le16((__u16) units(coord) - count),
		      &formatted_at(coord)->num_of_entries);

	if (from == 0) {
		/* entries from head was removed - move remaining to right */
		memmove((char *)item_body_by_coord(coord) +
			header_delta + entry_delta, item_body_by_coord(coord),
			(unsigned)size);
		if (REISER4_DEBUG)
			memset(item_body_by_coord(coord), 0,
			       (unsigned)header_delta + entry_delta);
	} else {
		/* freed space is already at the end of item */
		if (REISER4_DEBUG)
			memset((char *)item_body_by_coord(coord) + size, 0,
			       (unsigned)header_delta + entry_delta);
	}

	return header_delta + entry_delta;
}

int kill_units_cde(coord_t * coord /* coord of item */ ,
		   pos_in_node_t from /* start unit pos */ ,
		   pos_in_node_t to /* stop unit pos */ ,
		   struct carry_kill_data *kdata UNUSED_ARG,
		   reiser4_key * smallest_removed, reiser4_key * new_first)
{
	return cut_units_cde(coord, from, to, NULL, smallest_removed, new_first);
}

/* ->s.dir.extract_key() method for this item plugin. */
int extract_key_cde(const coord_t * coord /* coord of item */ ,
		    reiser4_key * key /* resulting key */ )
{
	directory_entry_format *dent;

	assert("nikita-1155", coord != NULL);
	assert("nikita-1156", key != NULL);

	dent = entry_at(coord, idx_of(coord));
	return extract_key_from_id(&dent->id, key);
}

int
update_key_cde(const coord_t * coord, const reiser4_key * key,
	       lock_handle * lh UNUSED_ARG)
{
	directory_entry_format *dent;
	obj_key_id obj_id;
	int result;

	assert("nikita-2344", coord != NULL);
	assert("nikita-2345", key != NULL);

	dent = entry_at(coord, idx_of(coord));
	result = build_obj_key_id(key, &obj_id);
	if (result == 0) {
		dent->id = obj_id;
		znode_make_dirty(coord->node);
	}
	return 0;
}

/* ->s.dir.extract_name() method for this item plugin. */
char *extract_name_cde(const coord_t * coord /* coord of item */ , char *buf)
{
	directory_entry_format *dent;

	assert("nikita-1157", coord != NULL);

	dent = entry_at(coord, idx_of(coord));
	return extract_dent_name(coord, dent, buf);
}

static int cde_bytes(int pasting, const reiser4_item_data * data)
{
	int result;

	result = data->length;
	if (!pasting)
		result -= sizeof(cde_item_format);
	return result;
}

/* ->s.dir.add_entry() method for this item plugin */
int add_entry_cde(struct inode *dir /* directory object */ ,
		  coord_t * coord /* coord of item */ ,
		  lock_handle * lh /* lock handle for insertion */ ,
		  const struct dentry *name /* name to insert */ ,
		  reiser4_dir_entry_desc * dir_entry	/* parameters of new
							 * directory entry */ )
{
	reiser4_item_data data;
	struct cde_entry entry;
	struct cde_entry_data edata;
	int result;

	assert("nikita-1656", coord->node == lh->node);
	assert("nikita-1657", znode_is_write_locked(coord->node));

	edata.num_of_entries = 1;
	edata.entry = &entry;

	entry.dir = dir;
	entry.obj = dir_entry->obj;
	entry.name = &name->d_name;

	data.data = (char *)&edata;
	data.user = 0;		/* &edata is not user space */
	data.iplug = item_plugin_by_id(COMPOUND_DIR_ID);
	data.arg = dir_entry;
	assert("nikita-1302", data.iplug != NULL);

	result = is_dot_key(&dir_entry->key);
	data.length = estimate_cde(result ? coord : NULL, &data);

	inode_add_bytes(dir, cde_bytes(result, &data));

	if (result)
		result = insert_by_coord(coord, &data, &dir_entry->key, lh, 0);
	else
		result = reiser4_resize_item(coord, &data, &dir_entry->key,
					     lh, 0);
	return result;
}

/* ->s.dir.rem_entry() */
int rem_entry_cde(struct inode *dir /* directory of item */ ,
		  const struct qstr *name, coord_t * coord /* coord of item */ ,
		  lock_handle * lh UNUSED_ARG	/* lock handle for
						 * removal */ ,
		  reiser4_dir_entry_desc * entry UNUSED_ARG	/* parameters of
								 * directory entry
								 * being removed */ )
{
	coord_t shadow;
	int result;
	int length;
	ON_DEBUG(char buf[DE_NAME_BUF_LEN]);

	assert("nikita-2870", strlen(name->name) == name->len);
	assert("nikita-2869",
	       !strcmp(name->name, extract_name_cde(coord, buf)));

	length = sizeof(directory_entry_format) + sizeof(cde_unit_header);
	if (is_longname(name->name, name->len))
		length += name->len + 1;

	if (inode_get_bytes(dir) < length) {
		warning("nikita-2628", "Dir is broke: %llu: %llu",
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

/* ->s.dir.max_name_len() method for this item plugin */
int max_name_len_cde(const struct inode *dir /* directory */ )
{
	return
		reiser4_tree_by_inode(dir)->nplug->max_item_size() -
		sizeof(directory_entry_format) - sizeof(cde_item_format) -
		sizeof(cde_unit_header) - 2;
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
