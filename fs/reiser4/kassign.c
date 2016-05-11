/* Copyright 2001, 2002, 2003, 2004 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Key assignment policy implementation */

/*
 * In reiser4 every piece of file system data and meta-data has a key. Keys
 * are used to store information in and retrieve it from reiser4 internal
 * tree. In addition to this, keys define _ordering_ of all file system
 * information: things having close keys are placed into the same or
 * neighboring (in the tree order) nodes of the tree. As our block allocator
 * tries to respect tree order (see flush.c), keys also define order in which
 * things are laid out on the disk, and hence, affect performance directly.
 *
 * Obviously, assignment of keys to data and meta-data should be consistent
 * across whole file system. Algorithm that calculates a key for a given piece
 * of data or meta-data is referred to as "key assignment".
 *
 * Key assignment is too expensive to be implemented as a plugin (that is,
 * with an ability to support different key assignment schemas in the same
 * compiled kernel image). As a compromise, all key-assignment functions and
 * data-structures are collected in this single file, so that modifications to
 * key assignment algorithm can be localized. Additional changes may be
 * required in key.[ch].
 *
 * Current default reiser4 key assignment algorithm is dubbed "Plan A". As one
 * may guess, there is "Plan B" too.
 *
 */

/*
 * Additional complication with key assignment implementation is a requirement
 * to support different key length.
 */

/*
 *                   KEY ASSIGNMENT: PLAN A, LONG KEYS.
 *
 * DIRECTORY ITEMS
 *
 * |       60     | 4 | 7 |1|   56        |        64        |        64       |
 * +--------------+---+---+-+-------------+------------------+-----------------+
 * |    dirid     | 0 | F |H|  prefix-1   |    prefix-2      |  prefix-3/hash  |
 * +--------------+---+---+-+-------------+------------------+-----------------+
 * |                  |                   |                  |                 |
 * |    8 bytes       |      8 bytes      |     8 bytes      |     8 bytes     |
 *
 * dirid         objectid of directory this item is for
 *
 * F             fibration, see fs/reiser4/plugin/fibration.[ch]
 *
 * H             1 if last 8 bytes of the key contain hash,
 *               0 if last 8 bytes of the key contain prefix-3
 *
 * prefix-1      first 7 characters of file name.
 *               Padded by zeroes if name is not long enough.
 *
 * prefix-2      next 8 characters of the file name.
 *
 * prefix-3      next 8 characters of the file name.
 *
 * hash          hash of the rest of file name (i.e., portion of file
 *               name not included into prefix-1 and prefix-2).
 *
 * File names shorter than 23 (== 7 + 8 + 8) characters are completely encoded
 * in the key. Such file names are called "short". They are distinguished by H
 * bit set 0 in the key.
 *
 * Other file names are "long". For long name, H bit is 1, and first 15 (== 7
 * + 8) characters are encoded in prefix-1 and prefix-2 portions of the
 * key. Last 8 bytes of the key are occupied by hash of the remaining
 * characters of the name.
 *
 * This key assignment reaches following important goals:
 *
 *     (1) directory entries are sorted in approximately lexicographical
 *     order.
 *
 *     (2) collisions (when multiple directory items have the same key), while
 *     principally unavoidable in a tree with fixed length keys, are rare.
 *
 * STAT DATA
 *
 *  |       60     | 4 |       64        | 4 |     60       |        64       |
 *  +--------------+---+-----------------+---+--------------+-----------------+
 *  |  locality id | 1 |    ordering     | 0 |  objectid    |        0        |
 *  +--------------+---+-----------------+---+--------------+-----------------+
 *  |                  |                 |                  |                 |
 *  |    8 bytes       |    8 bytes      |     8 bytes      |     8 bytes     |
 *
 * locality id     object id of a directory where first name was created for
 *                 the object
 *
 * ordering        copy of second 8-byte portion of the key of directory
 *                 entry for the first name of this object. Ordering has a form
 *                         {
 *                                 fibration :7;
 *                                 h         :1;
 *                                 prefix1   :56;
 *                         }
 *                 see description of key for directory entry above.
 *
 * objectid        object id for this object
 *
 * This key assignment policy is designed to keep stat-data in the same order
 * as corresponding directory items, thus speeding up readdir/stat types of
 * workload.
 *
 * FILE BODY
 *
 *  |       60     | 4 |       64        | 4 |     60       |        64       |
 *  +--------------+---+-----------------+---+--------------+-----------------+
 *  |  locality id | 4 |    ordering     | 0 |  objectid    |      offset     |
 *  +--------------+---+-----------------+---+--------------+-----------------+
 *  |                  |                 |                  |                 |
 *  |    8 bytes       |    8 bytes      |     8 bytes      |     8 bytes     |
 *
 * locality id     object id of a directory where first name was created for
 *                 the object
 *
 * ordering        the same as in the key of stat-data for this object
 *
 * objectid        object id for this object
 *
 * offset          logical offset from the beginning of this file.
 *                 Measured in bytes.
 *
 *
 *                   KEY ASSIGNMENT: PLAN A, SHORT KEYS.
 *
 * DIRECTORY ITEMS
 *
 *  |       60     | 4 | 7 |1|   56        |        64       |
 *  +--------------+---+---+-+-------------+-----------------+
 *  |    dirid     | 0 | F |H|  prefix-1   |  prefix-2/hash  |
 *  +--------------+---+---+-+-------------+-----------------+
 *  |                  |                   |                 |
 *  |    8 bytes       |      8 bytes      |     8 bytes     |
 *
 * dirid         objectid of directory this item is for
 *
 * F             fibration, see fs/reiser4/plugin/fibration.[ch]
 *
 * H             1 if last 8 bytes of the key contain hash,
 *               0 if last 8 bytes of the key contain prefix-2
 *
 * prefix-1      first 7 characters of file name.
 *               Padded by zeroes if name is not long enough.
 *
 * prefix-2      next 8 characters of the file name.
 *
 * hash          hash of the rest of file name (i.e., portion of file
 *               name not included into prefix-1).
 *
 * File names shorter than 15 (== 7 + 8) characters are completely encoded in
 * the key. Such file names are called "short". They are distinguished by H
 * bit set in the key.
 *
 * Other file names are "long". For long name, H bit is 0, and first 7
 * characters are encoded in prefix-1 portion of the key. Last 8 bytes of the
 * key are occupied by hash of the remaining characters of the name.
 *
 * STAT DATA
 *
 *  |       60     | 4 | 4 |     60       |        64       |
 *  +--------------+---+---+--------------+-----------------+
 *  |  locality id | 1 | 0 |  objectid    |        0        |
 *  +--------------+---+---+--------------+-----------------+
 *  |                  |                  |                 |
 *  |    8 bytes       |     8 bytes      |     8 bytes     |
 *
 * locality id     object id of a directory where first name was created for
 *                 the object
 *
 * objectid        object id for this object
 *
 * FILE BODY
 *
 *  |       60     | 4 | 4 |     60       |        64       |
 *  +--------------+---+---+--------------+-----------------+
 *  |  locality id | 4 | 0 |  objectid    |      offset     |
 *  +--------------+---+---+--------------+-----------------+
 *  |                  |                  |                 |
 *  |    8 bytes       |     8 bytes      |     8 bytes     |
 *
 * locality id     object id of a directory where first name was created for
 *                 the object
 *
 * objectid        object id for this object
 *
 * offset          logical offset from the beginning of this file.
 *                 Measured in bytes.
 *
 *
 */

#include "debug.h"
#include "key.h"
#include "kassign.h"
#include "vfs_ops.h"
#include "inode.h"
#include "super.h"
#include "dscale.h"

#include <linux/types.h>	/* for __u??  */
#include <linux/fs.h>		/* for struct super_block, etc  */

/* bitmask for H bit (see comment at the beginning of this file */
static const __u64 longname_mark = 0x0100000000000000ull;
/* bitmask for F and H portions of the key. */
static const __u64 fibration_mask = 0xff00000000000000ull;

/* return true if name is not completely encoded in @key */
int is_longname_key(const reiser4_key * key)
{
	__u64 highpart;

	assert("nikita-2863", key != NULL);
	if (get_key_type(key) != KEY_FILE_NAME_MINOR)
		reiser4_print_key("oops", key);
	assert("nikita-2864", get_key_type(key) == KEY_FILE_NAME_MINOR);

	if (REISER4_LARGE_KEY)
		highpart = get_key_ordering(key);
	else
		highpart = get_key_objectid(key);

	return (highpart & longname_mark) ? 1 : 0;
}

/* return true if @name is too long to be completely encoded in the key */
int is_longname(const char *name UNUSED_ARG, int len)
{
	if (REISER4_LARGE_KEY)
		return len > 23;
	else
		return len > 15;
}

/* code ascii string into __u64.

   Put characters of @name into result (@str) one after another starting
   from @start_idx-th highest (arithmetically) byte. This produces
   endian-safe encoding. memcpy(2) will not do.

*/
static __u64 pack_string(const char *name /* string to encode */ ,
			 int start_idx	/* highest byte in result from
					 * which to start encoding */ )
{
	unsigned i;
	__u64 str;

	str = 0;
	for (i = 0; (i < sizeof str - start_idx) && name[i]; ++i) {
		str <<= 8;
		str |= (unsigned char)name[i];
	}
	str <<= (sizeof str - i - start_idx) << 3;
	return str;
}

/* opposite to pack_string(). Takes value produced by pack_string(), restores
 * string encoded in it and stores result in @buf */
char *reiser4_unpack_string(__u64 value, char *buf)
{
	do {
		*buf = value >> (64 - 8);
		if (*buf)
			++buf;
		value <<= 8;
	} while (value != 0);
	*buf = 0;
	return buf;
}

/* obtain name encoded in @key and store it in @buf */
char *extract_name_from_key(const reiser4_key * key, char *buf)
{
	char *c;

	assert("nikita-2868", !is_longname_key(key));

	c = buf;
	if (REISER4_LARGE_KEY) {
		c = reiser4_unpack_string(get_key_ordering(key) &
					  ~fibration_mask, c);
		c = reiser4_unpack_string(get_key_fulloid(key), c);
	} else
		c = reiser4_unpack_string(get_key_fulloid(key) &
					  ~fibration_mask, c);
	reiser4_unpack_string(get_key_offset(key), c);
	return buf;
}

/**
 * complete_entry_key - calculate entry key by name
 * @dir: directory where entry is (or will be) in
 * @name: name to calculate key of
 * @len: lenth of name
 * @result: place to store result in
 *
 * Sets fields of entry key @result which depend on file name.
 * When REISER4_LARGE_KEY is defined three fields of @result are set: ordering,
 * objectid and offset. Otherwise, objectid and offset are set.
 */
void complete_entry_key(const struct inode *dir, const char *name,
			int len, reiser4_key *result)
{
#if REISER4_LARGE_KEY
	__u64 ordering;
	__u64 objectid;
	__u64 offset;

	assert("nikita-1139", dir != NULL);
	assert("nikita-1142", result != NULL);
	assert("nikita-2867", strlen(name) == len);

	/*
	 * key allocation algorithm for directory entries in case of large
	 * keys:
	 *
	 * If name is not longer than 7 + 8 + 8 = 23 characters, put first 7
	 * characters into ordering field of key, next 8 charactes (if any)
	 * into objectid field of key and next 8 ones (of any) into offset
	 * field of key
	 *
	 * If file name is longer than 23 characters, put first 7 characters
	 * into key's ordering, next 8 to objectid and hash of remaining
	 * characters into offset field.
	 *
	 * To distinguish above cases, in latter set up unused high bit in
	 * ordering field.
	 */

	/* [0-6] characters to ordering */
	ordering = pack_string(name, 1);
	if (len > 7) {
		/* [7-14] characters to objectid */
		objectid = pack_string(name + 7, 0);
		if (len > 15) {
			if (len <= 23) {
				/* [15-23] characters to offset */
				offset = pack_string(name + 15, 0);
			} else {
				/* note in a key the fact that offset contains
				 * hash */
				ordering |= longname_mark;

				/* offset is the hash of the file name's tail */
				offset = inode_hash_plugin(dir)->hash(name + 15,
								      len - 15);
			}
		} else {
			offset = 0ull;
		}
	} else {
		objectid = 0ull;
		offset = 0ull;
	}

	assert("nikita-3480", inode_fibration_plugin(dir) != NULL);
	ordering |= inode_fibration_plugin(dir)->fibre(dir, name, len);

	set_key_ordering(result, ordering);
	set_key_fulloid(result, objectid);
	set_key_offset(result, offset);
	return;

#else
	__u64 objectid;
	__u64 offset;

	assert("nikita-1139", dir != NULL);
	assert("nikita-1142", result != NULL);
	assert("nikita-2867", strlen(name) == len);

	/*
	 * key allocation algorithm for directory entries in case of not large
	 * keys:
	 *
	 * If name is not longer than 7 + 8 = 15 characters, put first 7
	 * characters into objectid field of key, next 8 charactes (if any)
	 * into offset field of key
	 *
	 * If file name is longer than 15 characters, put first 7 characters
	 * into key's objectid, and hash of remaining characters into offset
	 * field.
	 *
	 * To distinguish above cases, in latter set up unused high bit in
	 * objectid field.
	 */

	/* [0-6] characters to objectid */
	objectid = pack_string(name, 1);
	if (len > 7) {
		if (len <= 15) {
			/* [7-14] characters to offset */
			offset = pack_string(name + 7, 0);
		} else {
			/* note in a key the fact that offset contains hash. */
			objectid |= longname_mark;

			/* offset is the hash of the file name. */
			offset = inode_hash_plugin(dir)->hash(name + 7,
							      len - 7);
		}
	} else
		offset = 0ull;

	assert("nikita-3480", inode_fibration_plugin(dir) != NULL);
	objectid |= inode_fibration_plugin(dir)->fibre(dir, name, len);

	set_key_fulloid(result, objectid);
	set_key_offset(result, offset);
	return;
#endif				/* ! REISER4_LARGE_KEY */
}

/* true, if @key is the key of "." */
int is_dot_key(const reiser4_key * key/* key to check */)
{
	assert("nikita-1717", key != NULL);
	assert("nikita-1718", get_key_type(key) == KEY_FILE_NAME_MINOR);
	return
	    (get_key_ordering(key) == 0ull) &&
	    (get_key_objectid(key) == 0ull) && (get_key_offset(key) == 0ull);
}

/* build key for stat-data.

   return key of stat-data of this object. This should became sd plugin
   method in the future. For now, let it be here.

*/
reiser4_key *build_sd_key(const struct inode *target /* inode of an object */ ,
			  reiser4_key * result	/* resulting key of @target
						   stat-data */ )
{
	assert("nikita-261", result != NULL);

	reiser4_key_init(result);
	set_key_locality(result, reiser4_inode_data(target)->locality_id);
	set_key_ordering(result, get_inode_ordering(target));
	set_key_objectid(result, get_inode_oid(target));
	set_key_type(result, KEY_SD_MINOR);
	set_key_offset(result, (__u64) 0);
	return result;
}

/* encode part of key into &obj_key_id

   This encodes into @id part of @key sufficient to restore @key later,
   given that latter is key of object (key of stat-data).

   See &obj_key_id
*/
int build_obj_key_id(const reiser4_key * key /* key to encode */ ,
		     obj_key_id * id/* id where key is encoded in */)
{
	assert("nikita-1151", key != NULL);
	assert("nikita-1152", id != NULL);

	memcpy(id, key, sizeof *id);
	return 0;
}

/* encode reference to @obj in @id.

   This is like build_obj_key_id() above, but takes inode as parameter. */
int build_inode_key_id(const struct inode *obj /* object to build key of */ ,
		       obj_key_id * id/* result */)
{
	reiser4_key sdkey;

	assert("nikita-1166", obj != NULL);
	assert("nikita-1167", id != NULL);

	build_sd_key(obj, &sdkey);
	build_obj_key_id(&sdkey, id);
	return 0;
}

/* decode @id back into @key

   Restore key of object stat-data from @id. This is dual to
   build_obj_key_id() above.
*/
int extract_key_from_id(const obj_key_id * id	/* object key id to extract key
						 * from */ ,
			reiser4_key * key/* result */)
{
	assert("nikita-1153", id != NULL);
	assert("nikita-1154", key != NULL);

	reiser4_key_init(key);
	memcpy(key, id, sizeof *id);
	return 0;
}

/* extract objectid of directory from key of directory entry within said
   directory.
   */
oid_t extract_dir_id_from_key(const reiser4_key * de_key	/* key of
								 * directory
								 * entry */ )
{
	assert("nikita-1314", de_key != NULL);
	return get_key_locality(de_key);
}

/* encode into @id key of directory entry.

   Encode into @id information sufficient to later distinguish directory
   entries within the same directory. This is not whole key, because all
   directory entries within directory item share locality which is equal
   to objectid of their directory.

*/
int build_de_id(const struct inode *dir /* inode of directory */ ,
		const struct qstr *name	/* name to be given to @obj by
					 * directory entry being
					 * constructed */ ,
		de_id * id/* short key of directory entry */)
{
	reiser4_key key;

	assert("nikita-1290", dir != NULL);
	assert("nikita-1292", id != NULL);

	/* NOTE-NIKITA this is suboptimal. */
	inode_dir_plugin(dir)->build_entry_key(dir, name, &key);
	return build_de_id_by_key(&key, id);
}

/* encode into @id key of directory entry.

   Encode into @id information sufficient to later distinguish directory
   entries within the same directory. This is not whole key, because all
   directory entries within directory item share locality which is equal
   to objectid of their directory.

*/
int build_de_id_by_key(const reiser4_key * entry_key	/* full key of directory
							 * entry */ ,
		       de_id * id/* short key of directory entry */)
{
	memcpy(id, ((__u64 *) entry_key) + 1, sizeof *id);
	return 0;
}

/* restore from @id key of directory entry.

   Function dual to build_de_id(): given @id and locality, build full
   key of directory entry within directory item.

*/
int extract_key_from_de_id(const oid_t locality	/* locality of directory
						 * entry */ ,
			   const de_id * id /* directory entry id */ ,
			   reiser4_key * key/* result */)
{
	/* no need to initialise key here: all fields are overwritten */
	memcpy(((__u64 *) key) + 1, id, sizeof *id);
	set_key_locality(key, locality);
	set_key_type(key, KEY_FILE_NAME_MINOR);
	return 0;
}

/* compare two &de_id's */
cmp_t de_id_cmp(const de_id * id1 /* first &de_id to compare */ ,
		const de_id * id2/* second &de_id to compare */)
{
	/* NOTE-NIKITA ugly implementation */
	reiser4_key k1;
	reiser4_key k2;

	extract_key_from_de_id((oid_t) 0, id1, &k1);
	extract_key_from_de_id((oid_t) 0, id2, &k2);
	return keycmp(&k1, &k2);
}

/* compare &de_id with key */
cmp_t de_id_key_cmp(const de_id * id /* directory entry id to compare */ ,
		    const reiser4_key * key/* key to compare */)
{
	cmp_t result;
	reiser4_key *k1;

	k1 = (reiser4_key *) (((unsigned long)id) - sizeof key->el[0]);
	result = KEY_DIFF_EL(k1, key, 1);
	if (result == EQUAL_TO) {
		result = KEY_DIFF_EL(k1, key, 2);
		if (REISER4_LARGE_KEY && result == EQUAL_TO)
			result = KEY_DIFF_EL(k1, key, 3);
	}
	return result;
}

/*
 * return number of bytes necessary to encode @inode identity.
 */
int inode_onwire_size(const struct inode *inode)
{
	int result;

	result = dscale_bytes_to_write(get_inode_oid(inode));
	result += dscale_bytes_to_write(get_inode_locality(inode));

	/*
	 * ordering is large (it usually has highest bits set), so it makes
	 * little sense to dscale it.
	 */
	if (REISER4_LARGE_KEY)
		result += sizeof(get_inode_ordering(inode));
	return result;
}

/*
 * encode @inode identity at @start
 */
char *build_inode_onwire(const struct inode *inode, char *start)
{
	start += dscale_write(start, get_inode_locality(inode));
	start += dscale_write(start, get_inode_oid(inode));

	if (REISER4_LARGE_KEY) {
		put_unaligned(cpu_to_le64(get_inode_ordering(inode)), (__le64 *)start);
		start += sizeof(get_inode_ordering(inode));
	}
	return start;
}

/*
 * extract key that was previously encoded by build_inode_onwire() at @addr
 */
char *extract_obj_key_id_from_onwire(char *addr, obj_key_id * key_id)
{
	__u64 val;

	addr += dscale_read(addr, &val);
	val = (val << KEY_LOCALITY_SHIFT) | KEY_SD_MINOR;
	put_unaligned(cpu_to_le64(val), (__le64 *)key_id->locality);
	addr += dscale_read(addr, &val);
	put_unaligned(cpu_to_le64(val), (__le64 *)key_id->objectid);
#if REISER4_LARGE_KEY
	memcpy(&key_id->ordering, addr, sizeof key_id->ordering);
	addr += sizeof key_id->ordering;
#endif
	return addr;
}

/*
 * skip a key that was previously encoded by build_inode_onwire() at @addr
 * FIXME: handle IO errors.
 */
char * locate_obj_key_id_onwire(char * addr)
{
	/* locality */
	addr += dscale_bytes_to_read(addr);
	/* objectid */
	addr += dscale_bytes_to_read(addr);
#if REISER4_LARGE_KEY
	addr += sizeof ((obj_key_id *)0)->ordering;
#endif
	return addr;
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
