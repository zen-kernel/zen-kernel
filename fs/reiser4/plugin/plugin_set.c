/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */
/* This file contains Reiser4 plugin set operations */

/* plugin sets
 *
 * Each file in reiser4 is controlled by a whole set of plugins (file plugin,
 * directory plugin, hash plugin, tail policy plugin, security plugin, etc.)
 * assigned (inherited, deduced from mode bits, etc.) at creation time. This
 * set of plugins (so called pset) is described by structure plugin_set (see
 * plugin/plugin_set.h), which contains pointers to all required plugins.
 *
 * Children can inherit some pset members from their parent, however sometimes
 * it is useful to specify members different from parent ones. Since object's
 * pset can not be easily changed without fatal consequences, we use for this
 * purpose another special plugin table (so called hset, or heir set) described
 * by the same structure.
 *
 * Inode only stores a pointers to pset and hset. Different inodes with the
 * same set of pset (hset) members point to the same pset (hset). This is
 * archived by storing psets and hsets in global hash table. Races are avoided
 * by simple (and efficient so far) solution of never recycling psets, even
 * when last inode pointing to it is destroyed.
 */

#include "../debug.h"
#include "../super.h"
#include "plugin_set.h"

#include <linux/slab.h>
#include <linux/stddef.h>

/* slab for plugin sets */
static struct kmem_cache *plugin_set_slab;

static spinlock_t plugin_set_lock[8] __cacheline_aligned_in_smp = {
	__SPIN_LOCK_UNLOCKED(plugin_set_lock[0]),
	__SPIN_LOCK_UNLOCKED(plugin_set_lock[1]),
	__SPIN_LOCK_UNLOCKED(plugin_set_lock[2]),
	__SPIN_LOCK_UNLOCKED(plugin_set_lock[3]),
	__SPIN_LOCK_UNLOCKED(plugin_set_lock[4]),
	__SPIN_LOCK_UNLOCKED(plugin_set_lock[5]),
	__SPIN_LOCK_UNLOCKED(plugin_set_lock[6]),
	__SPIN_LOCK_UNLOCKED(plugin_set_lock[7])
};

/* hash table support */

#define PS_TABLE_SIZE (32)

static inline plugin_set *cast_to(const unsigned long *a)
{
	return container_of(a, plugin_set, hashval);
}

static inline int pseq(const unsigned long *a1, const unsigned long *a2)
{
	plugin_set *set1;
	plugin_set *set2;

	/* make sure fields are not missed in the code below */
	cassert(sizeof *set1 ==
		sizeof set1->hashval +
		sizeof set1->link +
		sizeof set1->file +
		sizeof set1->dir +
		sizeof set1->perm +
		sizeof set1->formatting +
		sizeof set1->hash +
		sizeof set1->fibration +
		sizeof set1->sd +
		sizeof set1->dir_item +
		sizeof set1->cipher +
		sizeof set1->digest +
		sizeof set1->compression +
		sizeof set1->compression_mode +
		sizeof set1->cluster +
		sizeof set1->create);

	set1 = cast_to(a1);
	set2 = cast_to(a2);
	return
	    set1->hashval == set2->hashval &&
	    set1->file == set2->file &&
	    set1->dir == set2->dir &&
	    set1->perm == set2->perm &&
	    set1->formatting == set2->formatting &&
	    set1->hash == set2->hash &&
	    set1->fibration == set2->fibration &&
	    set1->sd == set2->sd &&
	    set1->dir_item == set2->dir_item &&
	    set1->cipher == set2->cipher &&
	    set1->digest == set2->digest &&
	    set1->compression == set2->compression &&
	    set1->compression_mode == set2->compression_mode &&
	    set1->cluster == set2->cluster &&
	    set1->create == set2->create;
}

#define HASH_FIELD(hash, set, field)		\
({						\
	(hash) += (unsigned long)(set)->field >> 2;	\
})

static inline unsigned long calculate_hash(const plugin_set * set)
{
	unsigned long result;

	result = 0;
	HASH_FIELD(result, set, file);
	HASH_FIELD(result, set, dir);
	HASH_FIELD(result, set, perm);
	HASH_FIELD(result, set, formatting);
	HASH_FIELD(result, set, hash);
	HASH_FIELD(result, set, fibration);
	HASH_FIELD(result, set, sd);
	HASH_FIELD(result, set, dir_item);
	HASH_FIELD(result, set, cipher);
	HASH_FIELD(result, set, digest);
	HASH_FIELD(result, set, compression);
	HASH_FIELD(result, set, compression_mode);
	HASH_FIELD(result, set, cluster);
	HASH_FIELD(result, set, create);
	return result & (PS_TABLE_SIZE - 1);
}

static inline unsigned long
pshash(ps_hash_table * table, const unsigned long *a)
{
	return *a;
}

/* The hash table definition */
#define KMALLOC(size) kmalloc((size), reiser4_ctx_gfp_mask_get())
#define KFREE(ptr, size) kfree(ptr)
TYPE_SAFE_HASH_DEFINE(ps, plugin_set, unsigned long, hashval, link, pshash,
		      pseq);
#undef KFREE
#undef KMALLOC

static ps_hash_table ps_table;
static plugin_set empty_set = {
	.hashval = 0,
	.file = NULL,
	.dir = NULL,
	.perm = NULL,
	.formatting = NULL,
	.hash = NULL,
	.fibration = NULL,
	.sd = NULL,
	.dir_item = NULL,
	.cipher = NULL,
	.digest = NULL,
	.compression = NULL,
	.compression_mode = NULL,
	.cluster = NULL,
	.create = NULL,
	.link = {NULL}
};

plugin_set *plugin_set_get_empty(void)
{
	return &empty_set;
}

void plugin_set_put(plugin_set * set)
{
}

static inline unsigned long *pset_field(plugin_set * set, int offset)
{
	return (unsigned long *)(((char *)set) + offset);
}

static int plugin_set_field(plugin_set ** set, const unsigned long val,
			    const int offset)
{
	unsigned long *spot;
	spinlock_t *lock;
	plugin_set replica;
	plugin_set *twin;
	plugin_set *psal;
	plugin_set *orig;

	assert("nikita-2902", set != NULL);
	assert("nikita-2904", *set != NULL);

	spot = pset_field(*set, offset);
	if (unlikely(*spot == val))
		return 0;

	replica = *(orig = *set);
	*pset_field(&replica, offset) = val;
	replica.hashval = calculate_hash(&replica);
	rcu_read_lock();
	twin = ps_hash_find(&ps_table, &replica.hashval);
	if (unlikely(twin == NULL)) {
		rcu_read_unlock();
		psal = kmem_cache_alloc(plugin_set_slab,
					reiser4_ctx_gfp_mask_get());
		if (psal == NULL)
			return RETERR(-ENOMEM);
		*psal = replica;
		lock = &plugin_set_lock[replica.hashval & 7];
		spin_lock(lock);
		twin = ps_hash_find(&ps_table, &replica.hashval);
		if (likely(twin == NULL)) {
			*set = psal;
			ps_hash_insert_rcu(&ps_table, psal);
		} else {
			*set = twin;
			kmem_cache_free(plugin_set_slab, psal);
		}
		spin_unlock(lock);
	} else {
		rcu_read_unlock();
		*set = twin;
	}
	return 0;
}

static struct {
	int offset;
	reiser4_plugin_groups groups;
	reiser4_plugin_type type;
} pset_descr[PSET_LAST] = {
	[PSET_FILE] = {
		.offset = offsetof(plugin_set, file),
		.type = REISER4_FILE_PLUGIN_TYPE,
		.groups = 0
	},
	[PSET_DIR] = {
		.offset = offsetof(plugin_set, dir),
		.type = REISER4_DIR_PLUGIN_TYPE,
		.groups = 0
	},
	[PSET_PERM] = {
		.offset = offsetof(plugin_set, perm),
		.type = REISER4_PERM_PLUGIN_TYPE,
		.groups = 0
	},
	[PSET_FORMATTING] = {
		.offset = offsetof(plugin_set, formatting),
		.type = REISER4_FORMATTING_PLUGIN_TYPE,
		.groups = 0
	},
	[PSET_HASH] = {
		.offset = offsetof(plugin_set, hash),
		.type = REISER4_HASH_PLUGIN_TYPE,
		.groups = 0
	},
	[PSET_FIBRATION] = {
		.offset = offsetof(plugin_set, fibration),
		.type = REISER4_FIBRATION_PLUGIN_TYPE,
		.groups = 0
	},
	[PSET_SD] = {
		.offset = offsetof(plugin_set, sd),
		.type = REISER4_ITEM_PLUGIN_TYPE,
		.groups = (1 << STAT_DATA_ITEM_TYPE)
	},
	[PSET_DIR_ITEM] = {
		.offset = offsetof(plugin_set, dir_item),
		.type = REISER4_ITEM_PLUGIN_TYPE,
		.groups = (1 << DIR_ENTRY_ITEM_TYPE)
	},
	[PSET_CIPHER] = {
		.offset = offsetof(plugin_set, cipher),
		.type = REISER4_CIPHER_PLUGIN_TYPE,
		.groups = 0
	},
	[PSET_DIGEST] = {
		.offset = offsetof(plugin_set, digest),
		.type = REISER4_DIGEST_PLUGIN_TYPE,
		.groups = 0
	},
	[PSET_COMPRESSION] = {
		.offset = offsetof(plugin_set, compression),
		.type = REISER4_COMPRESSION_PLUGIN_TYPE,
		.groups = 0
	},
	[PSET_COMPRESSION_MODE] = {
		.offset = offsetof(plugin_set, compression_mode),
		.type = REISER4_COMPRESSION_MODE_PLUGIN_TYPE,
		.groups = 0
	},
	[PSET_CLUSTER] = {
		.offset = offsetof(plugin_set, cluster),
		.type = REISER4_CLUSTER_PLUGIN_TYPE,
		.groups = 0
	},
	[PSET_CREATE] = {
		.offset = offsetof(plugin_set, create),
		.type = REISER4_FILE_PLUGIN_TYPE,
		.groups = (1 << REISER4_REGULAR_FILE)
	}
};

#define DEFINE_PSET_OPS(PREFIX)						       \
	reiser4_plugin_type PREFIX##_member_to_type_unsafe(pset_member memb)   \
{								               \
	if (memb > PSET_LAST)						       \
		return REISER4_PLUGIN_TYPES;				       \
	return pset_descr[memb].type;					       \
}									       \
									       \
int PREFIX##_set_unsafe(plugin_set ** set, pset_member memb,		       \
		     reiser4_plugin * plugin)				       \
{									       \
	assert("nikita-3492", set != NULL);				       \
	assert("nikita-3493", *set != NULL);				       \
	assert("nikita-3494", plugin != NULL);				       \
	assert("nikita-3495", 0 <= memb && memb < PSET_LAST);		       \
	assert("nikita-3496", plugin->h.type_id == pset_descr[memb].type);     \
									       \
	if (pset_descr[memb].groups)					       \
		if (!(pset_descr[memb].groups & plugin->h.groups))	       \
			return -EINVAL;					       \
									       \
	return plugin_set_field(set,					       \
			(unsigned long)plugin, pset_descr[memb].offset);       \
}									       \
									       \
reiser4_plugin *PREFIX##_get(plugin_set * set, pset_member memb)	       \
{									       \
	assert("nikita-3497", set != NULL);				       \
	assert("nikita-3498", 0 <= memb && memb < PSET_LAST);		       \
									       \
	return *(reiser4_plugin **) (((char *)set) + pset_descr[memb].offset); \
}

DEFINE_PSET_OPS(aset);

int set_plugin(plugin_set ** set, pset_member memb, reiser4_plugin * plugin)
{
	return plugin_set_field(set,
		(unsigned long)plugin, pset_descr[memb].offset);
}

/**
 * init_plugin_set - create plugin set cache and hash table
 *
 * Initializes slab cache of plugin_set-s and their hash table. It is part of
 * reiser4 module initialization.
 */
int init_plugin_set(void)
{
	int result;

	result = ps_hash_init(&ps_table, PS_TABLE_SIZE);
	if (result == 0) {
		plugin_set_slab = kmem_cache_create("plugin_set",
						    sizeof(plugin_set), 0,
						    SLAB_HWCACHE_ALIGN,
						    NULL);
		if (plugin_set_slab == NULL)
			result = RETERR(-ENOMEM);
	}
	return result;
}

/**
 * done_plugin_set - delete plugin_set cache and plugin_set hash table
 *
 * This is called on reiser4 module unloading or system shutdown.
 */
void done_plugin_set(void)
{
	plugin_set *cur, *next;

	for_all_in_htable(&ps_table, ps, cur, next) {
		ps_hash_remove(&ps_table, cur);
		kmem_cache_free(plugin_set_slab, cur);
	}
	destroy_reiser4_cache(&plugin_set_slab);
	ps_hash_done(&ps_table);
}

/*
 * Local variables:
 * c-indentation-style: "K&R"
 * mode-name: "LC"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 120
 * End:
 */
