/* Copyright 2000, 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Declarations of key-related data-structures and operations on keys. */

#if !defined(__REISER4_KEY_H__)
#define __REISER4_KEY_H__

#include "dformat.h"
#include "forward.h"
#include "debug.h"

#include <linux/types.h>	/* for __u??  */

/* Operations on keys in reiser4 tree */

/* No access to any of these fields shall be done except via a
   wrapping macro/function, and that wrapping macro/function shall
   convert to little endian order. Compare keys will consider cpu byte order. */

/* A storage layer implementation difference between a regular unix file body
   and its attributes is in the typedef below which causes all of the attributes
   of a file to be near in key to all of the other attributes for all of the
   files within that directory, and not near to the file itself. It is
   interesting to consider whether this is the wrong approach, and whether there
   should be no difference at all. For current usage patterns this choice is
   probably the right one.  */

/* possible values for minor packing locality (4 bits required) */
typedef enum {
	/* file name */
	KEY_FILE_NAME_MINOR = 0,
	/* stat-data */
	KEY_SD_MINOR = 1,
	/* file attribute name */
	KEY_ATTR_NAME_MINOR = 2,
	/* file attribute value */
	KEY_ATTR_BODY_MINOR = 3,
	/* file body (tail or extent) */
	KEY_BODY_MINOR = 4,
} key_minor_locality;

/* Everything stored in the tree has a unique key, which means that the tree is
   (logically) fully ordered by key. Physical order is determined by dynamic
   heuristics that attempt to reflect key order when allocating available space,
   and by the repacker. It is stylistically better to put aggregation
   information into the key. Thus, if you want to segregate extents from tails,
   it is better to give them distinct minor packing localities rather than
   changing block_alloc.c to check the node type when deciding where to allocate
   the node.

   The need to randomly displace new directories and large files disturbs this
   symmetry unfortunately. However, it should be noted that this is a need that
   is not clearly established given the existence of a repacker. Also, in our
   current implementation tails have a different minor packing locality from
   extents, and no files have both extents and tails, so maybe symmetry can be
   had without performance cost after all. Symmetry is what we ship for now....
*/

/* Arbitrary major packing localities can be assigned to objects using
   the reiser4(filenameA/..packing<=some_number) system call.

   In reiser4, the creat() syscall creates a directory

   whose default flow (that which is referred to if the directory is
   read as a file) is the traditional unix file body.

   whose directory plugin is the 'filedir'

   whose major packing locality is that of the parent of the object created.

   The static_stat item is a particular commonly used directory
   compression (the one for normal unix files).

   The filedir plugin checks to see if the static_stat item exists.
   There is a unique key for static_stat.  If yes, then it uses the
   static_stat item for all of the values that it contains.  The
   static_stat item contains a flag for each stat it contains which
   indicates whether one should look outside the static_stat item for its
   contents.
*/

/* offset of fields in reiser4_key. Value of each element of this enum
    is index within key (thought as array of __u64's) where this field
    is. */
typedef enum {
	/* major "locale", aka dirid. Sits in 1st element */
	KEY_LOCALITY_INDEX = 0,
	/* minor "locale", aka item type. Sits in 1st element */
	KEY_TYPE_INDEX = 0,
	ON_LARGE_KEY(KEY_ORDERING_INDEX,)
	    /* "object band". Sits in 2nd element */
	    KEY_BAND_INDEX,
	/* objectid. Sits in 2nd element */
	KEY_OBJECTID_INDEX = KEY_BAND_INDEX,
	/* full objectid. Sits in 2nd element */
	KEY_FULLOID_INDEX = KEY_BAND_INDEX,
	/* Offset. Sits in 3rd element */
	KEY_OFFSET_INDEX,
	/* Name hash. Sits in 3rd element */
	KEY_HASH_INDEX = KEY_OFFSET_INDEX,
	KEY_CACHELINE_END = KEY_OFFSET_INDEX,
	KEY_LAST_INDEX
} reiser4_key_field_index;

/* key in reiser4 internal "balanced" tree. It is just array of three
    64bit integers in disk byte order (little-endian by default). This
    array is actually indexed by reiser4_key_field.  Each __u64 within
    this array is called "element". Logical key component encoded within
    elements are called "fields".

    We declare this as union with second component dummy to suppress
    inconvenient array<->pointer casts implied in C. */
union reiser4_key {
	__le64 el[KEY_LAST_INDEX];
	int pad;
};

/* bitmasks showing where within reiser4_key particular key is stored. */
/* major locality occupies higher 60 bits of the first element */
#define KEY_LOCALITY_MASK 0xfffffffffffffff0ull

/* minor locality occupies lower 4 bits of the first element */
#define KEY_TYPE_MASK 0xfull

/* controversial band occupies higher 4 bits of the 2nd element */
#define KEY_BAND_MASK 0xf000000000000000ull

/* objectid occupies lower 60 bits of the 2nd element */
#define KEY_OBJECTID_MASK 0x0fffffffffffffffull

/* full 64bit objectid*/
#define KEY_FULLOID_MASK 0xffffffffffffffffull

/* offset is just 3rd L.M.Nt itself */
#define KEY_OFFSET_MASK 0xffffffffffffffffull

/* ordering is whole second element */
#define KEY_ORDERING_MASK 0xffffffffffffffffull

/* how many bits key element should be shifted to left to get particular field
 */
typedef enum {
	KEY_LOCALITY_SHIFT = 4,
	KEY_TYPE_SHIFT = 0,
	KEY_BAND_SHIFT = 60,
	KEY_OBJECTID_SHIFT = 0,
	KEY_FULLOID_SHIFT = 0,
	KEY_OFFSET_SHIFT = 0,
	KEY_ORDERING_SHIFT = 0,
} reiser4_key_field_shift;

static inline __u64
get_key_el(const reiser4_key * key, reiser4_key_field_index off)
{
	assert("nikita-753", key != NULL);
	assert("nikita-754", off < KEY_LAST_INDEX);
	return le64_to_cpu(get_unaligned(&key->el[off]));
}

static inline void
set_key_el(reiser4_key * key, reiser4_key_field_index off, __u64 value)
{
	assert("nikita-755", key != NULL);
	assert("nikita-756", off < KEY_LAST_INDEX);
	put_unaligned(cpu_to_le64(value), &key->el[off]);
}

/* macro to define getter and setter functions for field F with type T */
#define DEFINE_KEY_FIELD(L, U, T)					\
static inline T get_key_ ## L(const reiser4_key *key)		        \
{									\
	assert("nikita-750", key != NULL);				\
	return (T) (get_key_el(key, KEY_ ## U ## _INDEX) &		\
		 KEY_ ## U ## _MASK) >> KEY_ ## U ## _SHIFT;		\
}									\
									\
static inline void set_key_ ## L(reiser4_key * key, T loc)		\
{									\
	__u64 el;							\
									\
	assert("nikita-752", key != NULL);				\
									\
	el = get_key_el(key, KEY_ ## U ## _INDEX);			\
	/* clear field bits in the key */				\
	el &= ~KEY_ ## U ## _MASK;					\
	/* actually it should be					\
									\
	   el |= ( loc << KEY_ ## U ## _SHIFT ) & KEY_ ## U ## _MASK;	\
									\
	   but we trust user to never pass values that wouldn't fit	\
	   into field. Clearing extra bits is one operation, but this	\
	   function is time-critical.					\
	   But check this in assertion. */				\
	assert("nikita-759", ((loc << KEY_ ## U ## _SHIFT) &		\
		~KEY_ ## U ## _MASK) == 0);				\
	el |= (loc << KEY_ ## U ## _SHIFT);				\
	set_key_el(key, KEY_ ## U ## _INDEX, el);			\
}

typedef __u64 oid_t;

/* define get_key_locality(), set_key_locality() */
DEFINE_KEY_FIELD(locality, LOCALITY, oid_t);
/* define get_key_type(), set_key_type() */
DEFINE_KEY_FIELD(type, TYPE, key_minor_locality);
/* define get_key_band(), set_key_band() */
DEFINE_KEY_FIELD(band, BAND, __u64);
/* define get_key_objectid(), set_key_objectid() */
DEFINE_KEY_FIELD(objectid, OBJECTID, oid_t);
/* define get_key_fulloid(), set_key_fulloid() */
DEFINE_KEY_FIELD(fulloid, FULLOID, oid_t);
/* define get_key_offset(), set_key_offset() */
DEFINE_KEY_FIELD(offset, OFFSET, __u64);
#if (REISER4_LARGE_KEY)
/* define get_key_ordering(), set_key_ordering() */
DEFINE_KEY_FIELD(ordering, ORDERING, __u64);
#else
static inline __u64 get_key_ordering(const reiser4_key * key)
{
	return 0;
}

static inline void set_key_ordering(reiser4_key * key, __u64 val)
{
}
#endif

/* key comparison result */
typedef enum { LESS_THAN = -1,	/* if first key is less than second */
	EQUAL_TO = 0,		/* if keys are equal */
	GREATER_THAN = +1	/* if first key is greater than second */
} cmp_t;

void reiser4_key_init(reiser4_key * key);

/* minimal possible key in the tree. Return pointer to the static storage. */
extern const reiser4_key *reiser4_min_key(void);
extern const reiser4_key *reiser4_max_key(void);

/* helper macro for keycmp() */
#define KEY_DIFF(k1, k2, field)						\
({									\
	typeof(get_key_ ## field(k1)) f1;				\
	typeof(get_key_ ## field(k2)) f2;				\
									\
	f1 = get_key_ ## field(k1);					\
	f2 = get_key_ ## field(k2);					\
									\
	(f1 < f2) ? LESS_THAN : ((f1 == f2) ? EQUAL_TO : GREATER_THAN);	\
})

/* helper macro for keycmp() */
#define KEY_DIFF_EL(k1, k2, off)					\
({									\
	__u64 e1;							\
	__u64 e2;							\
									\
	e1 = get_key_el(k1, off);					\
	e2 = get_key_el(k2, off);					\
									\
	(e1 < e2) ? LESS_THAN : ((e1 == e2) ? EQUAL_TO : GREATER_THAN);	\
})

/* compare `k1' and `k2'.  This function is a heart of "key allocation
    policy". All you need to implement new policy is to add yet another
    clause here. */
static inline cmp_t keycmp(const reiser4_key * k1 /* first key to compare */ ,
			   const reiser4_key * k2/* second key to compare */)
{
	cmp_t result;

	/*
	 * This function is the heart of reiser4 tree-routines. Key comparison
	 * is among most heavily used operations in the file system.
	 */

	assert("nikita-439", k1 != NULL);
	assert("nikita-440", k2 != NULL);

	/* there is no actual branch here: condition is compile time constant
	 * and constant folding and propagation ensures that only one branch
	 * is actually compiled in. */

	if (REISER4_PLANA_KEY_ALLOCATION) {
		/* if physical order of fields in a key is identical
		   with logical order, we can implement key comparison
		   as three 64bit comparisons. */
		/* logical order of fields in plan-a:
		   locality->type->objectid->offset. */
		/* compare locality and type at once */
		result = KEY_DIFF_EL(k1, k2, 0);
		if (result == EQUAL_TO) {
			/* compare objectid (and band if it's there) */
			result = KEY_DIFF_EL(k1, k2, 1);
			/* compare offset */
			if (result == EQUAL_TO) {
				result = KEY_DIFF_EL(k1, k2, 2);
				if (REISER4_LARGE_KEY && result == EQUAL_TO)
					result = KEY_DIFF_EL(k1, k2, 3);
			}
		}
	} else if (REISER4_3_5_KEY_ALLOCATION) {
		result = KEY_DIFF(k1, k2, locality);
		if (result == EQUAL_TO) {
			result = KEY_DIFF(k1, k2, objectid);
			if (result == EQUAL_TO) {
				result = KEY_DIFF(k1, k2, type);
				if (result == EQUAL_TO)
					result = KEY_DIFF(k1, k2, offset);
			}
		}
	} else
		impossible("nikita-441", "Unknown key allocation scheme!");
	return result;
}

/* true if @k1 equals @k2 */
static inline int keyeq(const reiser4_key * k1 /* first key to compare */ ,
			const reiser4_key * k2/* second key to compare */)
{
	assert("nikita-1879", k1 != NULL);
	assert("nikita-1880", k2 != NULL);
	return !memcmp(k1, k2, sizeof *k1);
}

/* true if @k1 is less than @k2 */
static inline int keylt(const reiser4_key * k1 /* first key to compare */ ,
			const reiser4_key * k2/* second key to compare */)
{
	assert("nikita-1952", k1 != NULL);
	assert("nikita-1953", k2 != NULL);
	return keycmp(k1, k2) == LESS_THAN;
}

/* true if @k1 is less than or equal to @k2 */
static inline int keyle(const reiser4_key * k1 /* first key to compare */ ,
			const reiser4_key * k2/* second key to compare */)
{
	assert("nikita-1954", k1 != NULL);
	assert("nikita-1955", k2 != NULL);
	return keycmp(k1, k2) != GREATER_THAN;
}

/* true if @k1 is greater than @k2 */
static inline int keygt(const reiser4_key * k1 /* first key to compare */ ,
			const reiser4_key * k2/* second key to compare */)
{
	assert("nikita-1959", k1 != NULL);
	assert("nikita-1960", k2 != NULL);
	return keycmp(k1, k2) == GREATER_THAN;
}

/* true if @k1 is greater than or equal to @k2 */
static inline int keyge(const reiser4_key * k1 /* first key to compare */ ,
			const reiser4_key * k2/* second key to compare */)
{
	assert("nikita-1956", k1 != NULL);
	assert("nikita-1957", k2 != NULL);	/* October  4: sputnik launched
						 * November 3: Laika */
	return keycmp(k1, k2) != LESS_THAN;
}

static inline void prefetchkey(reiser4_key * key)
{
	prefetch(key);
	prefetch(&key->el[KEY_CACHELINE_END]);
}

/* (%Lx:%x:%Lx:%Lx:%Lx:%Lx) =
	1 + 16 + 1 + 1 + 1 + 1 + 1 + 16 + 1 + 16 + 1 + 16 + 1 */
/* size of a buffer suitable to hold human readable key representation */
#define KEY_BUF_LEN (80)

#if REISER4_DEBUG
extern void reiser4_print_key(const char *prefix, const reiser4_key * key);
#else
#define reiser4_print_key(p, k) noop
#endif

/* __FS_REISERFS_KEY_H__ */
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
