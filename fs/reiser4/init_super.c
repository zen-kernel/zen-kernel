/* Copyright by Hans Reiser, 2003 */

#include "super.h"
#include "inode.h"
#include "plugin/plugin_set.h"

#include <linux/swap.h>

/**
 * init_fs_info - allocate reiser4 specific super block
 * @super: super block of filesystem
 *
 * Allocates and initialize reiser4_super_info_data, attaches it to
 * super->s_fs_info, initializes structures maintaining d_cursor-s.
 */
int reiser4_init_fs_info(struct super_block *super)
{
	reiser4_super_info_data *sbinfo;

	sbinfo = kzalloc(sizeof(reiser4_super_info_data),
			 reiser4_ctx_gfp_mask_get());
	if (!sbinfo)
		return RETERR(-ENOMEM);

	super->s_fs_info = sbinfo;
	super->s_op = NULL;

	ON_DEBUG(INIT_LIST_HEAD(&sbinfo->all_jnodes));
	ON_DEBUG(spin_lock_init(&sbinfo->all_guard));

	mutex_init(&sbinfo->delete_mutex);
	spin_lock_init(&(sbinfo->guard));

	/*  initialize per-super-block d_cursor resources */
	reiser4_init_super_d_info(super);

	return 0;
}

/**
 * reiser4_done_fs_info - free reiser4 specific super block
 * @super: super block of filesystem
 *
 * Performs some sanity checks, releases structures maintaining d_cursor-s,
 * frees reiser4_super_info_data.
 */
void reiser4_done_fs_info(struct super_block *super)
{
	assert("zam-990", super->s_fs_info != NULL);

	/* release per-super-block d_cursor resources */
	reiser4_done_super_d_info(super);

	/* make sure that there are not jnodes already */
	assert("", list_empty(&get_super_private(super)->all_jnodes));
	assert("", get_current_context()->trans->atom == NULL);
	reiser4_check_block_counters(super);
	kfree(super->s_fs_info);
	super->s_fs_info = NULL;
}

/* type of option parseable by parse_option() */
typedef enum {
	/* value of option is arbitrary string */
	OPT_STRING,

	/*
	 * option specifies bit in a bitmask. When option is set - bit in
	 * sbinfo->fs_flags is set. Examples are bsdgroups, 32bittimes, mtflush,
	 * dont_load_bitmap, atomic_write.
	 */
	OPT_BIT,

	/*
	 * value of option should conform to sprintf() format. Examples are
	 * tmgr.atom_max_size=N, tmgr.atom_max_age=N
	 */
	OPT_FORMAT,

	/*
	 * option can take one of predefined values. Example is onerror=panic or
	 * onerror=remount-ro
	 */
	OPT_ONEOF,
} opt_type_t;

#if 0
struct opt_bitmask_bit {
	const char *bit_name;
	int bit_nr;
};
#endif

/* description of option parseable by parse_option() */
struct opt_desc {
	/* option name.

	   parsed portion of string has a form "name=value".
	 */
	const char *name;
	/* type of option */
	opt_type_t type;
	union {
		/* where to store value of string option (type == OPT_STRING) */
		char **string;
		/* description of bits for bit option (type == OPT_BIT) */
		struct {
			int nr;
			void *addr;
		} bit;
		/* description of format and targets for format option (type
		   == OPT_FORMAT) */
		struct {
			const char *format;
			int nr_args;
			void *arg1;
			void *arg2;
			void *arg3;
			void *arg4;
		} f;
		struct {
			int *result;
			const char *list[10];
		} oneof;
		struct {
			void *addr;
			int nr_bits;
			/* struct opt_bitmask_bit *bits; */
		} bitmask;
	} u;
};

/**
 * parse_option - parse one option
 * @opt_strin: starting point of parsing
 * @opt: option description
 *
 * foo=bar,
 * ^   ^  ^
 * |   |  +-- replaced to '\0'
 * |   +-- val_start
 * +-- opt_string
 * Figures out option type and handles option correspondingly.
 */
static int parse_option(char *opt_string, struct opt_desc *opt)
{
	char *val_start;
	int result;
	const char *err_msg;

	/* NOTE-NIKITA think about using lib/cmdline.c functions here. */

	val_start = strchr(opt_string, '=');
	if (val_start != NULL) {
		*val_start = '\0';
		++val_start;
	}

	err_msg = NULL;
	result = 0;
	switch (opt->type) {
	case OPT_STRING:
		if (val_start == NULL) {
			err_msg = "String arg missing";
			result = RETERR(-EINVAL);
		} else
			*opt->u.string = val_start;
		break;
	case OPT_BIT:
		if (val_start != NULL)
			err_msg = "Value ignored";
		else
			set_bit(opt->u.bit.nr, opt->u.bit.addr);
		break;
	case OPT_FORMAT:
		if (val_start == NULL) {
			err_msg = "Formatted arg missing";
			result = RETERR(-EINVAL);
			break;
		}
		if (sscanf(val_start, opt->u.f.format,
			   opt->u.f.arg1, opt->u.f.arg2, opt->u.f.arg3,
			   opt->u.f.arg4) != opt->u.f.nr_args) {
			err_msg = "Wrong conversion";
			result = RETERR(-EINVAL);
		}
		break;
	case OPT_ONEOF:
		{
			int i = 0;

			if (val_start == NULL) {
				err_msg = "Value is missing";
				result = RETERR(-EINVAL);
				break;
			}
			err_msg = "Wrong option value";
			result = RETERR(-EINVAL);
			while (opt->u.oneof.list[i]) {
				if (!strcmp(opt->u.oneof.list[i], val_start)) {
					result = 0;
					err_msg = NULL;
					*opt->u.oneof.result = i;
					break;
				}
				i++;
			}
			break;
		}
	default:
		wrong_return_value("nikita-2100", "opt -> type");
		break;
	}
	if (err_msg != NULL) {
		warning("nikita-2496", "%s when parsing option \"%s%s%s\"",
			err_msg, opt->name, val_start ? "=" : "",
			val_start ? : "");
	}
	return result;
}

/**
 * parse_options - parse reiser4 mount options
 * @opt_string: starting point
 * @opts: array of option description
 * @nr_opts: number of elements in @opts
 *
 * Parses comma separated list of reiser4 mount options.
 */
static int parse_options(char *opt_string, struct opt_desc *opts, int nr_opts)
{
	int result;

	result = 0;
	while ((result == 0) && opt_string && *opt_string) {
		int j;
		char *next;

		next = strchr(opt_string, ',');
		if (next != NULL) {
			*next = '\0';
			++next;
		}
		for (j = 0; j < nr_opts; ++j) {
			if (!strncmp(opt_string, opts[j].name,
				     strlen(opts[j].name))) {
				result = parse_option(opt_string, &opts[j]);
				break;
			}
		}
		if (j == nr_opts) {
			warning("nikita-2307", "Unrecognized option: \"%s\"",
				opt_string);
			/* traditionally, -EINVAL is returned on wrong mount
			   option */
			result = RETERR(-EINVAL);
		}
		opt_string = next;
	}
	return result;
}

#define NUM_OPT(label, fmt, addr)				\
		{						\
			.name = (label),			\
			.type = OPT_FORMAT,			\
			.u = {					\
				.f = {				\
					.format  = (fmt),	\
					.nr_args = 1,		\
					.arg1 = (addr),		\
					.arg2 = NULL,		\
					.arg3 = NULL,		\
					.arg4 = NULL		\
				}				\
			}					\
		}

#define SB_FIELD_OPT(field, fmt) NUM_OPT(#field, fmt, &sbinfo->field)

#define BIT_OPT(label, bitnr)					\
	{							\
		.name = label,					\
		.type = OPT_BIT,				\
		.u = {						\
			.bit = {				\
				.nr = bitnr,			\
				.addr = &sbinfo->fs_flags	\
			}					\
		}						\
	}

#define MAX_NR_OPTIONS (30)

#if REISER4_DEBUG
#  define OPT_ARRAY_CHECK(opt, array)					\
	if ((opt) > (array) + MAX_NR_OPTIONS) {				\
		warning("zam-1046", "opt array is overloaded"); break;	\
	}
#else
#   define OPT_ARRAY_CHECK(opt, array) noop
#endif

#define PUSH_OPT(opt, array, ...)		\
do {						\
	struct opt_desc o = __VA_ARGS__;	\
	OPT_ARRAY_CHECK(opt, array);		\
	*(opt) ++ = o;				\
} while (0)

static noinline void push_sb_field_opts(struct opt_desc **p,
					struct opt_desc *opts,
					reiser4_super_info_data *sbinfo)
{
#define PUSH_SB_FIELD_OPT(field, format)		\
	PUSH_OPT(*p, opts, SB_FIELD_OPT(field, format))
	/*
	 * tmgr.atom_max_size=N
	 * Atoms containing more than N blocks will be forced to commit. N is
	 * decimal.
	 */
	PUSH_SB_FIELD_OPT(tmgr.atom_max_size, "%u");
	/*
	 * tmgr.atom_max_age=N
	 * Atoms older than N seconds will be forced to commit. N is decimal.
	 */
	PUSH_SB_FIELD_OPT(tmgr.atom_max_age, "%u");
	/*
	 * tmgr.atom_min_size=N
	 * In committing an atom to free dirty pages, force the atom less than
	 * N in size to fuse with another one.
	 */
	PUSH_SB_FIELD_OPT(tmgr.atom_min_size, "%u");
	/*
	 * tmgr.atom_max_flushers=N
	 * limit of concurrent flushers for one atom. 0 means no limit.
	 */
	PUSH_SB_FIELD_OPT(tmgr.atom_max_flushers, "%u");
	/*
	 * tree.cbk_cache_slots=N
	 * Number of slots in the cbk cache.
	 */
	PUSH_SB_FIELD_OPT(tree.cbk_cache.nr_slots, "%u");
	/*
	 * If flush finds more than FLUSH_RELOCATE_THRESHOLD adjacent dirty
	 * leaf-level blocks it will force them to be relocated.
	 */
	PUSH_SB_FIELD_OPT(flush.relocate_threshold, "%u");
	/*
	 * If flush finds can find a block allocation closer than at most
	 * FLUSH_RELOCATE_DISTANCE from the preceder it will relocate to that
	 * position.
	 */
	PUSH_SB_FIELD_OPT(flush.relocate_distance, "%u");
	/*
	 * If we have written this much or more blocks before encountering busy
	 * jnode in flush list - abort flushing hoping that next time we get
	 * called this jnode will be clean already, and we will save some
	 * seeks.
	 */
	PUSH_SB_FIELD_OPT(flush.written_threshold, "%u");
	/* The maximum number of nodes to scan left on a level during flush. */
	PUSH_SB_FIELD_OPT(flush.scan_maxnodes, "%u");
	/* preferred IO size */
	PUSH_SB_FIELD_OPT(optimal_io_size, "%u");
	/* carry flags used for insertion of new nodes */
	PUSH_SB_FIELD_OPT(tree.carry.new_node_flags, "%u");
	/* carry flags used for insertion of new extents */
	PUSH_SB_FIELD_OPT(tree.carry.new_extent_flags, "%u");
	/* carry flags used for paste operations */
	PUSH_SB_FIELD_OPT(tree.carry.paste_flags, "%u");
	/* carry flags used for insert operations */
	PUSH_SB_FIELD_OPT(tree.carry.insert_flags, "%u");

#ifdef CONFIG_REISER4_BADBLOCKS
	/*
	 * Alternative master superblock location in case if it's original
	 * location is not writeable/accessable. This is offset in BYTES.
	 */
	PUSH_SB_FIELD_OPT(altsuper, "%lu");
#endif
}

/**
 * reiser4_init_super_data - initialize reiser4 private super block
 * @super: super block to initialize
 * @opt_string: list of reiser4 mount options
 *
 * Sets various reiser4 parameters to default values. Parses mount options and
 * overwrites default settings.
 */
int reiser4_init_super_data(struct super_block *super, char *opt_string)
{
	int result;
	struct opt_desc *opts, *p;
	reiser4_super_info_data *sbinfo = get_super_private(super);

	/* initialize super, export, dentry operations */
	sbinfo->ops.super = reiser4_super_operations;
	sbinfo->ops.export = reiser4_export_operations;
	sbinfo->ops.dentry = reiser4_dentry_operations;
	super->s_op = &sbinfo->ops.super;
	super->s_export_op = &sbinfo->ops.export;

	/* initialize transaction manager parameters to default values */
	sbinfo->tmgr.atom_max_size = totalram_pages / 4;
	sbinfo->tmgr.atom_max_age = REISER4_ATOM_MAX_AGE / HZ;
	sbinfo->tmgr.atom_min_size = 256;
	sbinfo->tmgr.atom_max_flushers = ATOM_MAX_FLUSHERS;

	/* initialize cbk cache parameter */
	sbinfo->tree.cbk_cache.nr_slots = CBK_CACHE_SLOTS;

	/* initialize flush parameters */
	sbinfo->flush.relocate_threshold = FLUSH_RELOCATE_THRESHOLD;
	sbinfo->flush.relocate_distance = FLUSH_RELOCATE_DISTANCE;
	sbinfo->flush.written_threshold = FLUSH_WRITTEN_THRESHOLD;
	sbinfo->flush.scan_maxnodes = FLUSH_SCAN_MAXNODES;

	sbinfo->optimal_io_size = REISER4_OPTIMAL_IO_SIZE;

	/* preliminary tree initializations */
	sbinfo->tree.super = super;
	sbinfo->tree.carry.new_node_flags = REISER4_NEW_NODE_FLAGS;
	sbinfo->tree.carry.new_extent_flags = REISER4_NEW_EXTENT_FLAGS;
	sbinfo->tree.carry.paste_flags = REISER4_PASTE_FLAGS;
	sbinfo->tree.carry.insert_flags = REISER4_INSERT_FLAGS;
	rwlock_init(&(sbinfo->tree.tree_lock));
	spin_lock_init(&(sbinfo->tree.epoch_lock));

	/* initialize default readahead params */
	sbinfo->ra_params.max = totalram_pages / 4;
	sbinfo->ra_params.flags = 0;

	/* allocate memory for structure describing reiser4 mount options */
	opts = kmalloc(sizeof(struct opt_desc) * MAX_NR_OPTIONS,
		       reiser4_ctx_gfp_mask_get());
	if (opts == NULL)
		return RETERR(-ENOMEM);

	/* initialize structure describing reiser4 mount options */
	p = opts;

	push_sb_field_opts(&p, opts, sbinfo);
	/* turn on BSD-style gid assignment */

#define PUSH_BIT_OPT(name, bit)			\
	PUSH_OPT(p, opts, BIT_OPT(name, bit))

	PUSH_BIT_OPT("bsdgroups", REISER4_BSD_GID);
	/* turn on 32 bit times */
	PUSH_BIT_OPT("32bittimes", REISER4_32_BIT_TIMES);
	/*
	 * Don't load all bitmap blocks at mount time, it is useful for
	 * machines with tiny RAM and large disks.
	 */
	PUSH_BIT_OPT("dont_load_bitmap", REISER4_DONT_LOAD_BITMAP);
	/* disable transaction commits during write() */
	PUSH_BIT_OPT("atomic_write", REISER4_ATOMIC_WRITE);
	/* disable use of write barriers in the reiser4 log writer. */
	PUSH_BIT_OPT("no_write_barrier", REISER4_NO_WRITE_BARRIER);

	PUSH_OPT(p, opts,
	{
		/*
		 * tree traversal readahead parameters:
		 * -o readahead:MAXNUM:FLAGS
		 * MAXNUM - max number fo nodes to request readahead for: -1UL
		 * will set it to max_sane_readahead()
		 * FLAGS - combination of bits: RA_ADJCENT_ONLY, RA_ALL_LEVELS,
		 * CONTINUE_ON_PRESENT
		 */
		.name = "readahead",
		.type = OPT_FORMAT,
		.u = {
			.f = {
				.format = "%u:%u",
				.nr_args = 2,
				.arg1 = &sbinfo->ra_params.max,
				.arg2 = &sbinfo->ra_params.flags,
				.arg3 = NULL,
				.arg4 = NULL
			}
		}
	}
	);

	/* What to do in case of fs error */
	PUSH_OPT(p, opts,
	{
		.name = "onerror",
		.type = OPT_ONEOF,
		.u = {
			.oneof = {
				.result = &sbinfo->onerror,
				.list = {
					"panic", "remount-ro", NULL
				},
			}
		}
	}
	);

	/* modify default settings to values set by mount options */
	result = parse_options(opt_string, opts, p - opts);
	kfree(opts);
	if (result != 0)
		return result;

	/* correct settings to sanity values */
	sbinfo->tmgr.atom_max_age *= HZ;
	if (sbinfo->tmgr.atom_max_age <= 0)
		/* overflow */
		sbinfo->tmgr.atom_max_age = REISER4_ATOM_MAX_AGE;

	/* round optimal io size up to 512 bytes */
	sbinfo->optimal_io_size >>= VFS_BLKSIZE_BITS;
	sbinfo->optimal_io_size <<= VFS_BLKSIZE_BITS;
	if (sbinfo->optimal_io_size == 0) {
		warning("nikita-2497", "optimal_io_size is too small");
		return RETERR(-EINVAL);
	}
	return result;
}

/**
 * reiser4_init_read_super - read reiser4 master super block
 * @super: super block to fill
 * @silent: if 0 - print warnings
 *
 * Reads reiser4 master super block either from predefined location or from
 * location specified by altsuper mount option, initializes disk format plugin.
 */
int reiser4_init_read_super(struct super_block *super, int silent)
{
	struct buffer_head *super_bh;
	struct reiser4_master_sb *master_sb;
	reiser4_super_info_data *sbinfo = get_super_private(super);
	unsigned long blocksize;

 read_super_block:
#ifdef CONFIG_REISER4_BADBLOCKS
	if (sbinfo->altsuper)
		/*
		 * read reiser4 master super block at position specified by
		 * mount option
		 */
		super_bh = sb_bread(super,
				    (sector_t)(sbinfo->altsuper / super->s_blocksize));
	else
#endif
		/* read reiser4 master super block at 16-th 4096 block */
		super_bh = sb_bread(super,
				    (sector_t)(REISER4_MAGIC_OFFSET / super->s_blocksize));
	if (!super_bh)
		return RETERR(-EIO);

	master_sb = (struct reiser4_master_sb *)super_bh->b_data;
	/* check reiser4 magic string */
	if (!strncmp(master_sb->magic, REISER4_SUPER_MAGIC_STRING,
		     sizeof(REISER4_SUPER_MAGIC_STRING))) {
		/* reiser4 master super block contains filesystem blocksize */
		blocksize = le16_to_cpu(get_unaligned(&master_sb->blocksize));

		if (blocksize != PAGE_CACHE_SIZE) {
			/*
			 * currenly reiser4's blocksize must be equal to
			 * pagesize
			 */
			if (!silent)
				warning("nikita-2609",
					"%s: wrong block size %ld\n", super->s_id,
					blocksize);
			brelse(super_bh);
			return RETERR(-EINVAL);
		}
		if (blocksize != super->s_blocksize) {
			/*
			 * filesystem uses different blocksize. Reread master
			 * super block with correct blocksize
			 */
			brelse(super_bh);
			if (!sb_set_blocksize(super, (int)blocksize))
				return RETERR(-EINVAL);
			goto read_super_block;
		}

		sbinfo->df_plug =
			disk_format_plugin_by_id(
				le16_to_cpu(get_unaligned(&master_sb->disk_plugin_id)));
		if (sbinfo->df_plug == NULL) {
			if (!silent)
				warning("nikita-26091",
					"%s: unknown disk format plugin %d\n",
					super->s_id,
					le16_to_cpu(get_unaligned(&master_sb->disk_plugin_id)));
			brelse(super_bh);
			return RETERR(-EINVAL);
		}
		sbinfo->diskmap_block = le64_to_cpu(get_unaligned(&master_sb->diskmap));
		brelse(super_bh);
		return 0;
	}

	/* there is no reiser4 on the device */
	if (!silent)
		warning("nikita-2608",
			"%s: wrong master super block magic", super->s_id);
	brelse(super_bh);
	return RETERR(-EINVAL);
}

static struct {
	reiser4_plugin_type type;
	reiser4_plugin_id id;
} default_plugins[PSET_LAST] = {
	[PSET_FILE] = {
		.type = REISER4_FILE_PLUGIN_TYPE,
		.id = UNIX_FILE_PLUGIN_ID
	},
	[PSET_DIR] = {
		.type = REISER4_DIR_PLUGIN_TYPE,
		.id = HASHED_DIR_PLUGIN_ID
	},
	[PSET_HASH] = {
		.type = REISER4_HASH_PLUGIN_TYPE,
		.id = R5_HASH_ID
	},
	[PSET_FIBRATION] = {
		.type = REISER4_FIBRATION_PLUGIN_TYPE,
		.id = FIBRATION_DOT_O
	},
	[PSET_PERM] = {
		.type = REISER4_PERM_PLUGIN_TYPE,
		.id = NULL_PERM_ID
	},
	[PSET_FORMATTING] = {
		.type = REISER4_FORMATTING_PLUGIN_TYPE,
		.id = SMALL_FILE_FORMATTING_ID
	},
	[PSET_SD] = {
		.type = REISER4_ITEM_PLUGIN_TYPE,
		.id = STATIC_STAT_DATA_ID
	},
	[PSET_DIR_ITEM] = {
		.type = REISER4_ITEM_PLUGIN_TYPE,
		.id = COMPOUND_DIR_ID
	},
	[PSET_CIPHER] = {
		.type = REISER4_CIPHER_PLUGIN_TYPE,
		.id = NONE_CIPHER_ID
	},
	[PSET_DIGEST] = {
		.type = REISER4_DIGEST_PLUGIN_TYPE,
		.id = SHA256_32_DIGEST_ID
	},
	[PSET_COMPRESSION] = {
		.type = REISER4_COMPRESSION_PLUGIN_TYPE,
		.id = LZO1_COMPRESSION_ID
	},
	[PSET_COMPRESSION_MODE] = {
		.type = REISER4_COMPRESSION_MODE_PLUGIN_TYPE,
		.id = CONVX_COMPRESSION_MODE_ID
	},
	[PSET_CLUSTER] = {
		.type = REISER4_CLUSTER_PLUGIN_TYPE,
		.id = CLUSTER_64K_ID
	},
	[PSET_CREATE] = {
		.type = REISER4_FILE_PLUGIN_TYPE,
		.id = UNIX_FILE_PLUGIN_ID
	}
};

/* access to default plugin table */
reiser4_plugin *get_default_plugin(pset_member memb)
{
	return plugin_by_id(default_plugins[memb].type,
			    default_plugins[memb].id);
}

/**
 * reiser4_init_root_inode - obtain inode of root directory
 * @super: super block of filesystem
 *
 * Obtains inode of root directory (reading it from disk), initializes plugin
 * set it was not initialized.
 */
int reiser4_init_root_inode(struct super_block *super)
{
	reiser4_super_info_data *sbinfo = get_super_private(super);
	struct inode *inode;
	int result = 0;

	inode = reiser4_iget(super, sbinfo->df_plug->root_dir_key(super), 0);
	if (IS_ERR(inode))
		return RETERR(PTR_ERR(inode));

	super->s_root = d_make_root(inode);
	if (!super->s_root) {
		return RETERR(-ENOMEM);
	}

	super->s_root->d_op = &sbinfo->ops.dentry;

	if (!is_inode_loaded(inode)) {
		pset_member memb;
		plugin_set *pset;

		pset = reiser4_inode_data(inode)->pset;
		for (memb = 0; memb < PSET_LAST; ++memb) {

			if (aset_get(pset, memb) != NULL)
				continue;

			result = grab_plugin_pset(inode, NULL, memb);
			if (result != 0)
				break;

			reiser4_inode_clr_flag(inode, REISER4_SDLEN_KNOWN);
		}

		if (result == 0) {
			if (REISER4_DEBUG) {
				for (memb = 0; memb < PSET_LAST; ++memb)
					assert("nikita-3500",
					       aset_get(pset, memb) != NULL);
			}
		} else
			warning("nikita-3448", "Cannot set plugins of root: %i",
				result);
		reiser4_iget_complete(inode);

		/* As the default pset kept in the root dir may has been changed
		   (length is unknown), call update_sd. */
		if (!reiser4_inode_get_flag(inode, REISER4_SDLEN_KNOWN)) {
			result = reiser4_grab_space(
				inode_file_plugin(inode)->estimate.update(inode),
				BA_CAN_COMMIT);

			if (result == 0)
				result = reiser4_update_sd(inode);

			all_grabbed2free();
		}
	}

	super->s_maxbytes = MAX_LFS_FILESIZE;
	return result;
}

/*
 * Local variables:
 * c-indentation-style: "K&R"
 * mode-name: "LC"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 79
 * End:
 */
