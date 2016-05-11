/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

/* stat data manipulation. */

#include "../../forward.h"
#include "../../super.h"
#include "../../vfs_ops.h"
#include "../../inode.h"
#include "../../debug.h"
#include "../../dformat.h"
#include "../object.h"
#include "../plugin.h"
#include "../plugin_header.h"
#include "static_stat.h"
#include "item.h"

#include <linux/types.h>
#include <linux/fs.h>

/* see static_stat.h for explanation */

/* helper function used while we are dumping/loading inode/plugin state
    to/from the stat-data. */

static void move_on(int *length /* space remaining in stat-data */ ,
		    char **area /* current coord in stat data */ ,
		    int size_of /* how many bytes to move forward */ )
{
	assert("nikita-615", length != NULL);
	assert("nikita-616", area != NULL);

	*length -= size_of;
	*area += size_of;

	assert("nikita-617", *length >= 0);
}

/* helper function used while loading inode/plugin state from stat-data.
    Complain if there is less space in stat-data than was expected.
    Can only happen on disk corruption. */
static int not_enough_space(struct inode *inode /* object being processed */ ,
			    const char *where /* error message */ )
{
	assert("nikita-618", inode != NULL);

	warning("nikita-619", "Not enough space in %llu while loading %s",
		(unsigned long long)get_inode_oid(inode), where);

	return RETERR(-EINVAL);
}

/* helper function used while loading inode/plugin state from
    stat-data. Call it if invalid plugin id was found. */
static int unknown_plugin(reiser4_plugin_id id /* invalid id */ ,
			  struct inode *inode /* object being processed */ )
{
	warning("nikita-620", "Unknown plugin %i in %llu",
		id, (unsigned long long)get_inode_oid(inode));

	return RETERR(-EINVAL);
}

/* this is installed as ->init_inode() method of
    item_plugins[ STATIC_STAT_DATA_IT ] (fs/reiser4/plugin/item/item.c).
    Copies data from on-disk stat-data format into inode.
    Handles stat-data extensions. */
/* was sd_load */
int init_inode_static_sd(struct inode *inode /* object being processed */ ,
			 char *sd /* stat-data body */ ,
			 int len /* length of stat-data */ )
{
	int result;
	int bit;
	int chunk;
	__u16 mask;
	__u64 bigmask;
	reiser4_stat_data_base *sd_base;
	reiser4_inode *state;

	assert("nikita-625", inode != NULL);
	assert("nikita-626", sd != NULL);

	result = 0;
	sd_base = (reiser4_stat_data_base *) sd;
	state = reiser4_inode_data(inode);
	mask = le16_to_cpu(get_unaligned(&sd_base->extmask));
	bigmask = mask;
	reiser4_inode_set_flag(inode, REISER4_SDLEN_KNOWN);

	move_on(&len, &sd, sizeof *sd_base);
	for (bit = 0, chunk = 0;
	     mask != 0 || bit <= LAST_IMPORTANT_SD_EXTENSION;
	     ++bit, mask >>= 1) {
		if (((bit + 1) % 16) != 0) {
			/* handle extension */
			sd_ext_plugin *sdplug;

			if (bit >= LAST_SD_EXTENSION) {
				warning("vpf-1904",
					"No such extension %i in inode %llu",
					bit,
					(unsigned long long)
					get_inode_oid(inode));

				result = RETERR(-EINVAL);
				break;
			}

			sdplug = sd_ext_plugin_by_id(bit);
			if (sdplug == NULL) {
				warning("nikita-627",
					"No such extension %i in inode %llu",
					bit,
					(unsigned long long)
					get_inode_oid(inode));

				result = RETERR(-EINVAL);
				break;
			}
			if (mask & 1) {
				assert("nikita-628", sdplug->present);
				/* alignment is not supported in node layout
				   plugin yet.
				   result = align( inode, &len, &sd,
				   sdplug -> alignment );
				   if( result != 0 )
				   return result; */
				result = sdplug->present(inode, &sd, &len);
			} else if (sdplug->absent != NULL)
				result = sdplug->absent(inode);
			if (result)
				break;
			/* else, we are looking at the last bit in 16-bit
			   portion of bitmask */
		} else if (mask & 1) {
			/* next portion of bitmask */
			if (len < (int)sizeof(d16)) {
				warning("nikita-629",
					"No space for bitmap in inode %llu",
					(unsigned long long)
					get_inode_oid(inode));

				result = RETERR(-EINVAL);
				break;
			}
			mask = le16_to_cpu(get_unaligned((d16 *)sd));
			bigmask <<= 16;
			bigmask |= mask;
			move_on(&len, &sd, sizeof(d16));
			++chunk;
			if (chunk == 3) {
				if (!(mask & 0x8000)) {
					/* clear last bit */
					mask &= ~0x8000;
					continue;
				}
				/* too much */
				warning("nikita-630",
					"Too many extensions in %llu",
					(unsigned long long)
					get_inode_oid(inode));

				result = RETERR(-EINVAL);
				break;
			}
		} else
			/* bitmask exhausted */
			break;
	}
	state->extmask = bigmask;
	/* common initialisations */
	if (len - (bit / 16 * sizeof(d16)) > 0) {
		/* alignment in save_len_static_sd() is taken into account
		   -edward */
		warning("nikita-631", "unused space in inode %llu",
			(unsigned long long)get_inode_oid(inode));
	}

	return result;
}

/* estimates size of stat-data required to store inode.
    Installed as ->save_len() method of
    item_plugins[ STATIC_STAT_DATA_IT ] (fs/reiser4/plugin/item/item.c). */
/* was sd_len */
int save_len_static_sd(struct inode *inode /* object being processed */ )
{
	unsigned int result;
	__u64 mask;
	int bit;

	assert("nikita-632", inode != NULL);

	result = sizeof(reiser4_stat_data_base);
	mask = reiser4_inode_data(inode)->extmask;
	for (bit = 0; mask != 0; ++bit, mask >>= 1) {
		if (mask & 1) {
			sd_ext_plugin *sdplug;

			sdplug = sd_ext_plugin_by_id(bit);
			assert("nikita-633", sdplug != NULL);
			/*
			  no aligment support
			  result +=
			  reiser4_round_up(result, sdplug -> alignment) -
			  result;
			*/
			result += sdplug->save_len(inode);
		}
	}
	result += bit / 16 * sizeof(d16);
	return result;
}

/* saves inode into stat-data.
    Installed as ->save() method of
    item_plugins[ STATIC_STAT_DATA_IT ] (fs/reiser4/plugin/item/item.c). */
/* was sd_save */
int save_static_sd(struct inode *inode /* object being processed */ ,
		   char **area /* where to save stat-data */ )
{
	int result;
	__u64 emask;
	int bit;
	unsigned int len;
	reiser4_stat_data_base *sd_base;

	assert("nikita-634", inode != NULL);
	assert("nikita-635", area != NULL);

	result = 0;
	emask = reiser4_inode_data(inode)->extmask;
	sd_base = (reiser4_stat_data_base *) * area;
	put_unaligned(cpu_to_le16((__u16)(emask & 0xffff)), &sd_base->extmask);
	/*cputod16((unsigned)(emask & 0xffff), &sd_base->extmask);*/

	*area += sizeof *sd_base;
	len = 0xffffffffu;
	for (bit = 0; emask != 0; ++bit, emask >>= 1) {
		if (emask & 1) {
			if ((bit + 1) % 16 != 0) {
				sd_ext_plugin *sdplug;
				sdplug = sd_ext_plugin_by_id(bit);
				assert("nikita-636", sdplug != NULL);
				/* no alignment support yet
				   align( inode, &len, area,
				   sdplug -> alignment ); */
				result = sdplug->save(inode, area);
				if (result)
					break;
			} else {
				put_unaligned(cpu_to_le16((__u16)(emask & 0xffff)),
					      (d16 *)(*area));
				/*cputod16((unsigned)(emask & 0xffff),
				  (d16 *) * area);*/
				*area += sizeof(d16);
			}
		}
	}
	return result;
}

/* stat-data extension handling functions. */

static int present_lw_sd(struct inode *inode /* object being processed */ ,
			 char **area /* position in stat-data */ ,
			 int *len /* remaining length */ )
{
	if (*len >= (int)sizeof(reiser4_light_weight_stat)) {
		reiser4_light_weight_stat *sd_lw;

		sd_lw = (reiser4_light_weight_stat *) * area;

		inode->i_mode = le16_to_cpu(get_unaligned(&sd_lw->mode));
		set_nlink(inode, le32_to_cpu(get_unaligned(&sd_lw->nlink)));
		inode->i_size = le64_to_cpu(get_unaligned(&sd_lw->size));
		if ((inode->i_mode & S_IFMT) == (S_IFREG | S_IFIFO)) {
			inode->i_mode &= ~S_IFIFO;
			warning("", "partially converted file is encountered");
			reiser4_inode_set_flag(inode, REISER4_PART_MIXED);
		}
		move_on(len, area, sizeof *sd_lw);
		return 0;
	} else
		return not_enough_space(inode, "lw sd");
}

static int save_len_lw_sd(struct inode *inode UNUSED_ARG	/* object being
								 * processed */ )
{
	return sizeof(reiser4_light_weight_stat);
}

static int save_lw_sd(struct inode *inode /* object being processed */ ,
		      char **area /* position in stat-data */ )
{
	reiser4_light_weight_stat *sd;
	mode_t delta;

	assert("nikita-2705", inode != NULL);
	assert("nikita-2706", area != NULL);
	assert("nikita-2707", *area != NULL);

	sd = (reiser4_light_weight_stat *) * area;

	delta = (reiser4_inode_get_flag(inode,
					REISER4_PART_MIXED) ? S_IFIFO : 0);
	put_unaligned(cpu_to_le16(inode->i_mode | delta), &sd->mode);
	put_unaligned(cpu_to_le32(inode->i_nlink), &sd->nlink);
	put_unaligned(cpu_to_le64((__u64) inode->i_size), &sd->size);
	*area += sizeof *sd;
	return 0;
}

static int present_unix_sd(struct inode *inode /* object being processed */ ,
			   char **area /* position in stat-data */ ,
			   int *len /* remaining length */ )
{
	assert("nikita-637", inode != NULL);
	assert("nikita-638", area != NULL);
	assert("nikita-639", *area != NULL);
	assert("nikita-640", len != NULL);
	assert("nikita-641", *len > 0);

	if (*len >= (int)sizeof(reiser4_unix_stat)) {
		reiser4_unix_stat *sd;

		sd = (reiser4_unix_stat *) * area;

		i_uid_write(inode, le32_to_cpu(get_unaligned(&sd->uid)));
		i_gid_write(inode, le32_to_cpu(get_unaligned(&sd->gid)));
		inode->i_atime.tv_sec = le32_to_cpu(get_unaligned(&sd->atime));
		inode->i_mtime.tv_sec = le32_to_cpu(get_unaligned(&sd->mtime));
		inode->i_ctime.tv_sec = le32_to_cpu(get_unaligned(&sd->ctime));
		if (S_ISBLK(inode->i_mode) || S_ISCHR(inode->i_mode))
			inode->i_rdev = le64_to_cpu(get_unaligned(&sd->u.rdev));
		else
			inode_set_bytes(inode, (loff_t) le64_to_cpu(get_unaligned(&sd->u.bytes)));
		move_on(len, area, sizeof *sd);
		return 0;
	} else
		return not_enough_space(inode, "unix sd");
}

static int absent_unix_sd(struct inode *inode /* object being processed */ )
{
	i_uid_write(inode, get_super_private(inode->i_sb)->default_uid);
	i_gid_write(inode, get_super_private(inode->i_sb)->default_gid);
	inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME;
	inode_set_bytes(inode, inode->i_size);
	/* mark inode as lightweight, so that caller (lookup_common) will
	   complete initialisation by copying [ug]id from a parent. */
	reiser4_inode_set_flag(inode, REISER4_LIGHT_WEIGHT);
	return 0;
}

/* Audited by: green(2002.06.14) */
static int save_len_unix_sd(struct inode *inode UNUSED_ARG	/* object being
								 * processed */ )
{
	return sizeof(reiser4_unix_stat);
}

static int save_unix_sd(struct inode *inode /* object being processed */ ,
			char **area /* position in stat-data */ )
{
	reiser4_unix_stat *sd;

	assert("nikita-642", inode != NULL);
	assert("nikita-643", area != NULL);
	assert("nikita-644", *area != NULL);

	sd = (reiser4_unix_stat *) * area;
	put_unaligned(cpu_to_le32(i_uid_read(inode)), &sd->uid);
	put_unaligned(cpu_to_le32(i_gid_read(inode)), &sd->gid);
	put_unaligned(cpu_to_le32((__u32) inode->i_atime.tv_sec), &sd->atime);
	put_unaligned(cpu_to_le32((__u32) inode->i_ctime.tv_sec), &sd->ctime);
	put_unaligned(cpu_to_le32((__u32) inode->i_mtime.tv_sec), &sd->mtime);
	if (S_ISBLK(inode->i_mode) || S_ISCHR(inode->i_mode))
		put_unaligned(cpu_to_le64(inode->i_rdev), &sd->u.rdev);
	else
		put_unaligned(cpu_to_le64((__u64) inode_get_bytes(inode)), &sd->u.bytes);
	*area += sizeof *sd;
	return 0;
}

static int
present_large_times_sd(struct inode *inode /* object being processed */ ,
		       char **area /* position in stat-data */ ,
		       int *len /* remaining length */ )
{
	if (*len >= (int)sizeof(reiser4_large_times_stat)) {
		reiser4_large_times_stat *sd_lt;

		sd_lt = (reiser4_large_times_stat *) * area;

		inode->i_atime.tv_nsec = le32_to_cpu(get_unaligned(&sd_lt->atime));
		inode->i_mtime.tv_nsec = le32_to_cpu(get_unaligned(&sd_lt->mtime));
		inode->i_ctime.tv_nsec = le32_to_cpu(get_unaligned(&sd_lt->ctime));

		move_on(len, area, sizeof *sd_lt);
		return 0;
	} else
		return not_enough_space(inode, "large times sd");
}

static int
save_len_large_times_sd(struct inode *inode UNUSED_ARG
			/* object being processed */ )
{
	return sizeof(reiser4_large_times_stat);
}

static int
save_large_times_sd(struct inode *inode /* object being processed */ ,
		    char **area /* position in stat-data */ )
{
	reiser4_large_times_stat *sd;

	assert("nikita-2817", inode != NULL);
	assert("nikita-2818", area != NULL);
	assert("nikita-2819", *area != NULL);

	sd = (reiser4_large_times_stat *) * area;

	put_unaligned(cpu_to_le32((__u32) inode->i_atime.tv_nsec), &sd->atime);
	put_unaligned(cpu_to_le32((__u32) inode->i_ctime.tv_nsec), &sd->ctime);
	put_unaligned(cpu_to_le32((__u32) inode->i_mtime.tv_nsec), &sd->mtime);

	*area += sizeof *sd;
	return 0;
}

/* symlink stat data extension */

/* allocate memory for symlink target and attach it to inode->i_private */
static int
symlink_target_to_inode(struct inode *inode, const char *target, int len)
{
	assert("vs-845", inode->i_private == NULL);
	assert("vs-846", !reiser4_inode_get_flag(inode,
						 REISER4_GENERIC_PTR_USED));
	/* FIXME-VS: this is prone to deadlock. Not more than other similar
	   places, though */
	inode->i_private = kmalloc((size_t) len + 1,
				   reiser4_ctx_gfp_mask_get());
	if (!inode->i_private)
		return RETERR(-ENOMEM);

	memcpy((char *)(inode->i_private), target, (size_t) len);
	((char *)(inode->i_private))[len] = 0;
	reiser4_inode_set_flag(inode, REISER4_GENERIC_PTR_USED);
	return 0;
}

/* this is called on read_inode. There is nothing to do actually, but some
   sanity checks */
static int present_symlink_sd(struct inode *inode, char **area, int *len)
{
	int result;
	int length;
	reiser4_symlink_stat *sd;

	length = (int)inode->i_size;
	/*
	 * *len is number of bytes in stat data item from *area to the end of
	 * item. It must be not less than size of symlink + 1 for ending 0
	 */
	if (length > *len)
		return not_enough_space(inode, "symlink");

	if (*(*area + length) != 0) {
		warning("vs-840", "Symlink is not zero terminated");
		return RETERR(-EIO);
	}

	sd = (reiser4_symlink_stat *) * area;
	result = symlink_target_to_inode(inode, sd->body, length);

	move_on(len, area, length + 1);
	return result;
}

static int save_len_symlink_sd(struct inode *inode)
{
	return inode->i_size + 1;
}

/* this is called on create and update stat data. Do nothing on update but
   update @area */
static int save_symlink_sd(struct inode *inode, char **area)
{
	int result;
	int length;
	reiser4_symlink_stat *sd;

	length = (int)inode->i_size;
	/* inode->i_size must be set already */
	assert("vs-841", length);

	result = 0;
	sd = (reiser4_symlink_stat *) * area;
	if (!reiser4_inode_get_flag(inode, REISER4_GENERIC_PTR_USED)) {
		const char *target;

		target = (const char *)(inode->i_private);
		inode->i_private = NULL;

		result = symlink_target_to_inode(inode, target, length);

		/* copy symlink to stat data */
		memcpy(sd->body, target, (size_t) length);
		(*area)[length] = 0;
	} else {
		/* there is nothing to do in update but move area */
		assert("vs-844",
		       !memcmp(inode->i_private, sd->body,
			       (size_t) length + 1));
	}

	*area += (length + 1);
	return result;
}

static int present_flags_sd(struct inode *inode /* object being processed */ ,
			    char **area /* position in stat-data */ ,
			    int *len /* remaining length */ )
{
	assert("nikita-645", inode != NULL);
	assert("nikita-646", area != NULL);
	assert("nikita-647", *area != NULL);
	assert("nikita-648", len != NULL);
	assert("nikita-649", *len > 0);

	if (*len >= (int)sizeof(reiser4_flags_stat)) {
		reiser4_flags_stat *sd;

		sd = (reiser4_flags_stat *) * area;
		inode->i_flags = le32_to_cpu(get_unaligned(&sd->flags));
		move_on(len, area, sizeof *sd);
		return 0;
	} else
		return not_enough_space(inode, "generation and attrs");
}

/* Audited by: green(2002.06.14) */
static int save_len_flags_sd(struct inode *inode UNUSED_ARG	/* object being
								 * processed */ )
{
	return sizeof(reiser4_flags_stat);
}

static int save_flags_sd(struct inode *inode /* object being processed */ ,
			 char **area /* position in stat-data */ )
{
	reiser4_flags_stat *sd;

	assert("nikita-650", inode != NULL);
	assert("nikita-651", area != NULL);
	assert("nikita-652", *area != NULL);

	sd = (reiser4_flags_stat *) * area;
	put_unaligned(cpu_to_le32(inode->i_flags), &sd->flags);
	*area += sizeof *sd;
	return 0;
}

static int absent_plugin_sd(struct inode *inode);
static int present_plugin_sd(struct inode *inode /* object being processed */ ,
			     char **area /* position in stat-data */ ,
			     int *len /* remaining length */,
			     int is_pset /* 1 if plugin set, 0 if heir set. */)
{
	reiser4_plugin_stat *sd;
	reiser4_plugin *plugin;
	reiser4_inode *info;
	int i;
	__u16 mask;
	int result;
	int num_of_plugins;

	assert("nikita-653", inode != NULL);
	assert("nikita-654", area != NULL);
	assert("nikita-655", *area != NULL);
	assert("nikita-656", len != NULL);
	assert("nikita-657", *len > 0);

	if (*len < (int)sizeof(reiser4_plugin_stat))
		return not_enough_space(inode, "plugin");

	sd = (reiser4_plugin_stat *) * area;
	info = reiser4_inode_data(inode);

	mask = 0;
	num_of_plugins = le16_to_cpu(get_unaligned(&sd->plugins_no));
	move_on(len, area, sizeof *sd);
	result = 0;
	for (i = 0; i < num_of_plugins; ++i) {
		reiser4_plugin_slot *slot;
		reiser4_plugin_type type;
		pset_member memb;

		slot = (reiser4_plugin_slot *) * area;
		if (*len < (int)sizeof *slot)
			return not_enough_space(inode, "additional plugin");

		memb = le16_to_cpu(get_unaligned(&slot->pset_memb));
		type = aset_member_to_type_unsafe(memb);

		if (type == REISER4_PLUGIN_TYPES) {
			warning("nikita-3502",
				"wrong %s member (%i) for %llu", is_pset ?
				"pset" : "hset", memb,
				(unsigned long long)get_inode_oid(inode));
			return RETERR(-EINVAL);
		}
		plugin = plugin_by_disk_id(reiser4_tree_by_inode(inode),
					   type, &slot->id);
		if (plugin == NULL)
			return unknown_plugin(le16_to_cpu(get_unaligned(&slot->id)), inode);

		/* plugin is loaded into inode, mark this into inode's
		   bitmask of loaded non-standard plugins */
		if (!(mask & (1 << memb))) {
			mask |= (1 << memb);
		} else {
			warning("nikita-658", "duplicate plugin for %llu",
				(unsigned long long)get_inode_oid(inode));
			return RETERR(-EINVAL);
		}
		move_on(len, area, sizeof *slot);
		/* load plugin data, if any */
		if (plugin->h.pops != NULL && plugin->h.pops->load)
			result = plugin->h.pops->load(inode, plugin, area, len);
		else
			result = aset_set_unsafe(is_pset ? &info->pset :
						 &info->hset, memb, plugin);
		if (result)
			return result;
	}
	if (is_pset) {
		/* if object plugin wasn't loaded from stat-data, guess it by
		   mode bits */
		plugin = file_plugin_to_plugin(inode_file_plugin(inode));
		if (plugin == NULL)
			result = absent_plugin_sd(inode);
		info->plugin_mask = mask;
	} else
		info->heir_mask = mask;

	return result;
}

static int present_pset_sd(struct inode *inode, char **area, int *len) {
	return present_plugin_sd(inode, area, len, 1 /* pset */);
}

/* Determine object plugin for @inode based on i_mode.

   Many objects in reiser4 file system are controlled by standard object
   plugins that emulate traditional unix objects: unix file, directory, symlink, fifo, and so on.

   For such files we don't explicitly store plugin id in object stat
   data. Rather required plugin is guessed from mode bits, where file "type"
   is encoded (see stat(2)).
*/
static int
guess_plugin_by_mode(struct inode *inode /* object to guess plugins for */ )
{
	int fplug_id;
	int dplug_id;
	reiser4_inode *info;

	assert("nikita-736", inode != NULL);

	dplug_id = fplug_id = -1;

	switch (inode->i_mode & S_IFMT) {
	case S_IFSOCK:
	case S_IFBLK:
	case S_IFCHR:
	case S_IFIFO:
		fplug_id = SPECIAL_FILE_PLUGIN_ID;
		break;
	case S_IFLNK:
		fplug_id = SYMLINK_FILE_PLUGIN_ID;
		break;
	case S_IFDIR:
		fplug_id = DIRECTORY_FILE_PLUGIN_ID;
		dplug_id = HASHED_DIR_PLUGIN_ID;
		break;
	default:
		warning("nikita-737", "wrong file mode: %o", inode->i_mode);
		return RETERR(-EIO);
	case S_IFREG:
		fplug_id = UNIX_FILE_PLUGIN_ID;
		break;
	}
	info = reiser4_inode_data(inode);
	set_plugin(&info->pset, PSET_FILE, (fplug_id >= 0) ?
		   plugin_by_id(REISER4_FILE_PLUGIN_TYPE, fplug_id) : NULL);
	set_plugin(&info->pset, PSET_DIR, (dplug_id >= 0) ?
		   plugin_by_id(REISER4_DIR_PLUGIN_TYPE, dplug_id) : NULL);
	return 0;
}

/* Audited by: green(2002.06.14) */
static int absent_plugin_sd(struct inode *inode /* object being processed */ )
{
	int result;

	assert("nikita-659", inode != NULL);

	result = guess_plugin_by_mode(inode);
	/* if mode was wrong, guess_plugin_by_mode() returns "regular file",
	   but setup_inode_ops() will call make_bad_inode().
	   Another, more logical but bit more complex solution is to add
	   "bad-file plugin". */
	/* FIXME-VS: activate was called here */
	return result;
}

/* helper function for plugin_sd_save_len(): calculate how much space
    required to save state of given plugin */
/* Audited by: green(2002.06.14) */
static int len_for(reiser4_plugin * plugin /* plugin to save */ ,
		   struct inode *inode /* object being processed */ ,
		   pset_member memb,
		   int len, int is_pset)
{
	reiser4_inode *info;
	assert("nikita-661", inode != NULL);

	if (plugin == NULL)
		return len;

	info = reiser4_inode_data(inode);
	if (is_pset ?
	    info->plugin_mask & (1 << memb) :
	    info->heir_mask & (1 << memb)) {
		len += sizeof(reiser4_plugin_slot);
		if (plugin->h.pops && plugin->h.pops->save_len != NULL) {
			/*
			 * non-standard plugin, call method
			 * commented as it is incompatible with alignment
			 * policy in save_plug() -edward
			 *
			 * len = reiser4_round_up(len,
			 * plugin->h.pops->alignment);
			 */
			len += plugin->h.pops->save_len(inode, plugin);
		}
	}
	return len;
}

/* calculate how much space is required to save state of all plugins,
    associated with inode */
static int save_len_plugin_sd(struct inode *inode /* object being processed */,
			      int is_pset)
{
	int len;
	int last;
	reiser4_inode *state;
	pset_member memb;

	assert("nikita-663", inode != NULL);

	state = reiser4_inode_data(inode);

	/* common case: no non-standard plugins */
	if (is_pset ? state->plugin_mask == 0 : state->heir_mask == 0)
		return 0;
	len = sizeof(reiser4_plugin_stat);
	last = PSET_LAST;

	for (memb = 0; memb < last; ++memb) {
	      len = len_for(aset_get(is_pset ? state->pset : state->hset, memb),
			    inode, memb, len, is_pset);
	}
	assert("nikita-664", len > (int)sizeof(reiser4_plugin_stat));
	return len;
}

static int save_len_pset_sd(struct inode *inode) {
	return save_len_plugin_sd(inode, 1 /* pset */);
}

/* helper function for plugin_sd_save(): save plugin, associated with
    inode. */
static int save_plug(reiser4_plugin * plugin /* plugin to save */ ,
		     struct inode *inode /* object being processed */ ,
		     int memb /* what element of pset is saved */ ,
		     char **area /* position in stat-data */ ,
		     int *count	/* incremented if plugin were actually saved. */,
		     int is_pset /* 1 for plugin set, 0 for heir set */)
{
	reiser4_plugin_slot *slot;
	int fake_len;
	int result;

	assert("nikita-665", inode != NULL);
	assert("nikita-666", area != NULL);
	assert("nikita-667", *area != NULL);

	if (plugin == NULL)
		return 0;

	if (is_pset ?
	    !(reiser4_inode_data(inode)->plugin_mask & (1 << memb)) :
	    !(reiser4_inode_data(inode)->heir_mask & (1 << memb)))
		return 0;
	slot = (reiser4_plugin_slot *) * area;
	put_unaligned(cpu_to_le16(memb), &slot->pset_memb);
	put_unaligned(cpu_to_le16(plugin->h.id), &slot->id);
	fake_len = (int)0xffff;
	move_on(&fake_len, area, sizeof *slot);
	++*count;
	result = 0;
	if (plugin->h.pops != NULL) {
		if (plugin->h.pops->save != NULL)
			result = plugin->h.pops->save(inode, plugin, area);
	}
	return result;
}

/* save state of all non-standard plugins associated with inode */
static int save_plugin_sd(struct inode *inode /* object being processed */ ,
			  char **area /* position in stat-data */,
			  int is_pset /* 1 for pset, 0 for hset */)
{
	int fake_len;
	int result = 0;
	int num_of_plugins;
	reiser4_plugin_stat *sd;
	reiser4_inode *state;
	pset_member memb;

	assert("nikita-669", inode != NULL);
	assert("nikita-670", area != NULL);
	assert("nikita-671", *area != NULL);

	state = reiser4_inode_data(inode);
	if (is_pset ? state->plugin_mask == 0 : state->heir_mask == 0)
		return 0;
	sd = (reiser4_plugin_stat *) * area;
	fake_len = (int)0xffff;
	move_on(&fake_len, area, sizeof *sd);

	num_of_plugins = 0;
	for (memb = 0; memb < PSET_LAST; ++memb) {
		result = save_plug(aset_get(is_pset ? state->pset : state->hset,
					    memb),
				   inode, memb, area, &num_of_plugins, is_pset);
		if (result != 0)
			break;
	}

	put_unaligned(cpu_to_le16((__u16)num_of_plugins), &sd->plugins_no);
	return result;
}

static int save_pset_sd(struct inode *inode, char **area) {
	return save_plugin_sd(inode, area, 1 /* pset */);
}

static int present_hset_sd(struct inode *inode, char **area, int *len) {
	return present_plugin_sd(inode, area, len, 0 /* hset */);
}

static int save_len_hset_sd(struct inode *inode) {
	return save_len_plugin_sd(inode, 0 /* pset */);
}

static int save_hset_sd(struct inode *inode, char **area) {
	return save_plugin_sd(inode, area, 0 /* hset */);
}

/* helper function for crypto_sd_present(), crypto_sd_save.
   Extract crypto info from stat-data and attach it to inode */
static int extract_crypto_info (struct inode * inode,
				reiser4_crypto_stat * sd)
{
	struct reiser4_crypto_info * info;
	assert("edward-11", !inode_crypto_info(inode));
	assert("edward-1413",
	       !reiser4_inode_get_flag(inode, REISER4_CRYPTO_STAT_LOADED));
	/* create and attach a crypto-stat without secret key loaded */
	info = reiser4_alloc_crypto_info(inode);
	if (IS_ERR(info))
		return PTR_ERR(info);
	info->keysize = le16_to_cpu(get_unaligned(&sd->keysize));
	memcpy(info->keyid, sd->keyid, inode_digest_plugin(inode)->fipsize);
	reiser4_attach_crypto_info(inode, info);
	reiser4_inode_set_flag(inode, REISER4_CRYPTO_STAT_LOADED);
	return 0;
}

/* crypto stat-data extension */

static int present_crypto_sd(struct inode *inode, char **area, int *len)
{
	int result;
	reiser4_crypto_stat *sd;
	digest_plugin *dplug = inode_digest_plugin(inode);

	assert("edward-06", dplug != NULL);
	assert("edward-684", dplug->fipsize);
	assert("edward-07", area != NULL);
	assert("edward-08", *area != NULL);
	assert("edward-09", len != NULL);
	assert("edward-10", *len > 0);

	if (*len < (int)sizeof(reiser4_crypto_stat)) {
		return not_enough_space(inode, "crypto-sd");
	}
	/* *len is number of bytes in stat data item from *area to the end of
	   item. It must be not less than size of this extension */
	assert("edward-75", sizeof(*sd) + dplug->fipsize <= *len);

	sd = (reiser4_crypto_stat *) * area;
	result = extract_crypto_info(inode, sd);
	move_on(len, area, sizeof(*sd) + dplug->fipsize);

	return result;
}

static int save_len_crypto_sd(struct inode *inode)
{
	return sizeof(reiser4_crypto_stat) +
		inode_digest_plugin(inode)->fipsize;
}

static int save_crypto_sd(struct inode *inode, char **area)
{
	int result = 0;
	reiser4_crypto_stat *sd;
	struct reiser4_crypto_info * info = inode_crypto_info(inode);
	digest_plugin *dplug = inode_digest_plugin(inode);

	assert("edward-12", dplug != NULL);
	assert("edward-13", area != NULL);
	assert("edward-14", *area != NULL);
	assert("edward-15", info != NULL);
	assert("edward-1414", info->keyid != NULL);
	assert("edward-1415", info->keysize != 0);
	assert("edward-76", reiser4_inode_data(inode) != NULL);

	if (!reiser4_inode_get_flag(inode, REISER4_CRYPTO_STAT_LOADED)) {
		/* file is just created */
		sd = (reiser4_crypto_stat *) *area;
		/* copy everything but private key to the disk stat-data */
		put_unaligned(cpu_to_le16(info->keysize), &sd->keysize);
		memcpy(sd->keyid, info->keyid, (size_t) dplug->fipsize);
		reiser4_inode_set_flag(inode, REISER4_CRYPTO_STAT_LOADED);
	}
	*area += (sizeof(*sd) + dplug->fipsize);
	return result;
}

static int eio(struct inode *inode, char **area, int *len)
{
	return RETERR(-EIO);
}

sd_ext_plugin sd_ext_plugins[LAST_SD_EXTENSION] = {
	[LIGHT_WEIGHT_STAT] = {
		.h = {
			.type_id = REISER4_SD_EXT_PLUGIN_TYPE,
			.id = LIGHT_WEIGHT_STAT,
			.pops = NULL,
			.label = "light-weight sd",
			.desc = "sd for light-weight files",
			.linkage = {NULL,NULL}
		},
		.present = present_lw_sd,
		.absent = NULL,
		.save_len = save_len_lw_sd,
		.save = save_lw_sd,
		.alignment = 8
	},
	[UNIX_STAT] = {
		.h = {
			.type_id = REISER4_SD_EXT_PLUGIN_TYPE,
			.id = UNIX_STAT,
			.pops = NULL,
			.label = "unix-sd",
			.desc = "unix stat-data fields",
			.linkage = {NULL,NULL}
		},
		.present = present_unix_sd,
		.absent = absent_unix_sd,
		.save_len = save_len_unix_sd,
		.save = save_unix_sd,
		.alignment = 8
	},
	[LARGE_TIMES_STAT] = {
		.h = {
			.type_id = REISER4_SD_EXT_PLUGIN_TYPE,
			.id = LARGE_TIMES_STAT,
			.pops = NULL,
			.label = "64time-sd",
			.desc = "nanosecond resolution for times",
			.linkage = {NULL,NULL}
		},
		.present = present_large_times_sd,
		.absent = NULL,
		.save_len = save_len_large_times_sd,
		.save = save_large_times_sd,
		.alignment = 8
	},
	[SYMLINK_STAT] = {
		/* stat data of symlink has this extension */
		.h = {
			.type_id = REISER4_SD_EXT_PLUGIN_TYPE,
			.id = SYMLINK_STAT,
			.pops = NULL,
			.label = "symlink-sd",
			.desc =
			"stat data is appended with symlink name",
			.linkage = {NULL,NULL}
		},
		.present = present_symlink_sd,
		.absent = NULL,
		.save_len = save_len_symlink_sd,
		.save = save_symlink_sd,
		.alignment = 8
	},
	[PLUGIN_STAT] = {
		.h = {
			.type_id = REISER4_SD_EXT_PLUGIN_TYPE,
			.id = PLUGIN_STAT,
			.pops = NULL,
			.label = "plugin-sd",
			.desc = "plugin stat-data fields",
			.linkage = {NULL,NULL}
		},
		.present = present_pset_sd,
		.absent = absent_plugin_sd,
		.save_len = save_len_pset_sd,
		.save = save_pset_sd,
		.alignment = 8
	},
	[HEIR_STAT] = {
		.h = {
			.type_id = REISER4_SD_EXT_PLUGIN_TYPE,
			.id = HEIR_STAT,
			.pops = NULL,
			.label = "heir-plugin-sd",
			.desc = "heir plugin stat-data fields",
			.linkage = {NULL,NULL}
		},
		.present = present_hset_sd,
		.absent = NULL,
		.save_len = save_len_hset_sd,
		.save = save_hset_sd,
		.alignment = 8
	},
	[FLAGS_STAT] = {
		.h = {
			.type_id = REISER4_SD_EXT_PLUGIN_TYPE,
			.id = FLAGS_STAT,
			.pops = NULL,
			.label = "flags-sd",
			.desc = "inode bit flags",
			.linkage = {NULL, NULL}
		},
		.present = present_flags_sd,
		.absent = NULL,
		.save_len = save_len_flags_sd,
		.save = save_flags_sd,
		.alignment = 8
	},
	[CAPABILITIES_STAT] = {
		.h = {
			.type_id = REISER4_SD_EXT_PLUGIN_TYPE,
			.id = CAPABILITIES_STAT,
			.pops = NULL,
			.label = "capabilities-sd",
			.desc = "capabilities",
			.linkage = {NULL, NULL}
		},
		.present = eio,
		.absent = NULL,
		.save_len = save_len_flags_sd,
		.save = save_flags_sd,
		.alignment = 8
	},
	[CRYPTO_STAT] = {
		.h = {
			.type_id = REISER4_SD_EXT_PLUGIN_TYPE,
			.id = CRYPTO_STAT,
			.pops = NULL,
			.label = "crypto-sd",
			.desc = "secret key size and id",
			.linkage = {NULL, NULL}
		},
		.present = present_crypto_sd,
		.absent = NULL,
		.save_len = save_len_crypto_sd,
		.save = save_crypto_sd,
		.alignment = 8
	}
};

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
