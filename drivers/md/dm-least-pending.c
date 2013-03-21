/*
 * (C) Copyright 2008 Hewlett-Packard Development Company, L.P
 *
 * This file is released under the GPL.
 */

#include "dm-path-selector.h"

#include <linux/slab.h>
#include <linux/module.h>

#define DM_MSG_PREFIX "multipath least-pending"

/*-----------------------------------------------------------------
* Path-handling code, paths are held in lists
*---------------------------------------------------------------*/
struct path_info {
       struct list_head list;
       struct dm_path *path;
       unsigned repeat_count;
       atomic_t io_count;
};

static void free_paths(struct list_head *paths)
{
       struct path_info *pi, *next;

       list_for_each_entry_safe(pi, next, paths, list) {
		list_del(&pi->list);
		kfree(pi);
       }
}

/*-----------------------------------------------------------------
 * Least-pending selector
 *---------------------------------------------------------------*/

#define LPP_MIN_IO     1

struct selector {
       struct list_head valid_paths;
       struct list_head invalid_paths;
};

static struct selector *alloc_selector(void)
{
       struct selector *s = kmalloc(sizeof(*s), GFP_KERNEL);

       if (s) {
		INIT_LIST_HEAD(&s->valid_paths);
		INIT_LIST_HEAD(&s->invalid_paths);
       }

       return s;
}

static int lpp_create(struct path_selector *ps, unsigned argc, char **argv)
{
       struct selector *s;

       s = alloc_selector();
       if (!s)
		return -ENOMEM;

       ps->context = s;
       return 0;
}

static void lpp_destroy(struct path_selector *ps)
{
       struct selector *s = ps->context;

       free_paths(&s->valid_paths);
       free_paths(&s->invalid_paths);
       kfree(s);
       ps->context = NULL;
}

static int lpp_status(struct path_selector *ps, struct dm_path *path,
			status_type_t type, char *result, unsigned int maxlen)
{
       struct path_info *pi;
       int sz = 0;

       if (!path)
		switch (type) {
		case STATUSTYPE_INFO:
			DMEMIT("1 ");
		break;
		case STATUSTYPE_TABLE:
			DMEMIT("0 ");
		break;
		}
       else {
		pi = path->pscontext;
		switch (type) {
		case STATUSTYPE_INFO:
			DMEMIT("%u:%u ", pi->repeat_count,
					 atomic_read(&pi->io_count));
		break;
		case STATUSTYPE_TABLE:
		break;
		}
	}

       return sz;
}

/*
 * Called during initialisation to register each path with an
 * optional repeat_count.
 */
static int lpp_add_path(struct path_selector *ps, struct dm_path *path,
			int argc, char **argv, char **error)
{
       struct selector *s = ps->context;
       struct path_info *pi;
       unsigned repeat_count = LPP_MIN_IO;

	if (argc > 1) {
		*error = "least-pending ps: incorrect number of arguments";
		return -EINVAL;
	}

       /* First path argument is number of I/Os before switching path */
       if ((argc == 1) && (sscanf(argv[0], "%u", &repeat_count) != 1)) {
		*error = "least-pending ps: invalid repeat count";
		return -EINVAL;
       }

       /* allocate the path */
       pi = kmalloc(sizeof(*pi), GFP_KERNEL);
       if (!pi) {
		*error = "least-pending ps: Error allocating path context";
		return -ENOMEM;
       }

       pi->path = path;
       pi->repeat_count = repeat_count;
       atomic_set(&pi->io_count, 0);

       path->pscontext = pi;

       list_add(&pi->list, &s->valid_paths);

       return 0;
}

static void lpp_fail_path(struct path_selector *ps, struct dm_path *p)
{
       struct selector *s = ps->context;
       struct path_info *pi = p->pscontext;

       if (!pi)
	return;

       atomic_set(&pi->io_count, 0);

       list_move(&pi->list, &s->invalid_paths);
}

static int lpp_reinstate_path(struct path_selector *ps, struct dm_path *p)
{
       struct selector *s = ps->context;
       struct path_info *pi = p->pscontext;

       if (!pi)
	return 1;

       list_move(&pi->list, &s->valid_paths);

       return 0;
}

static struct dm_path *lpp_select_path(struct path_selector *ps,
					unsigned *repeat_count,
					size_t nr_bytes)
{
       struct selector *s = ps->context;
       struct path_info *pi, *next, *least_io_path = NULL;
       struct list_head *paths;

       if (list_empty(&s->valid_paths))
		return NULL;

       paths = &s->valid_paths;

       list_for_each_entry_safe(pi, next, paths, list) {
		if (!least_io_path || atomic_read(&least_io_path->io_count) < atomic_read(&pi->io_count))
			least_io_path = pi;
		if (!atomic_read(&least_io_path->io_count))
			break;
       }

       if (!least_io_path)
		return NULL;

       atomic_inc(&least_io_path->io_count);
       *repeat_count = least_io_path->repeat_count;

       return least_io_path->path;
}

static int lpp_end_io(struct path_selector *ps, struct dm_path *path,
		      size_t nr_bytes)
{
       struct path_info *pi = NULL;

       pi = path->pscontext;
       if (!pi)
	return 1;

       atomic_dec(&pi->io_count);

       return 0;
}

static struct path_selector_type lpp_ps = {
       .name = "least-pending",
       .module = THIS_MODULE,
       .table_args = 1,
       .info_args = 0,
       .create = lpp_create,
       .destroy = lpp_destroy,
       .status = lpp_status,
       .add_path = lpp_add_path,
       .fail_path = lpp_fail_path,
       .reinstate_path = lpp_reinstate_path,
       .select_path = lpp_select_path,
       .end_io = lpp_end_io,
};

static int __init dm_lpp_init(void)
{
       int r = dm_register_path_selector(&lpp_ps);

       if (r < 0)
		DMERR("register failed %d", r);

       DMINFO("version 1.0.0 loaded");

       return r;
}

static void __exit dm_lpp_exit(void)
{
       int r = dm_unregister_path_selector(&lpp_ps);

       if (r < 0)
		DMERR("unregister failed %d", r);
}

module_init(dm_lpp_init);
module_exit(dm_lpp_exit);

MODULE_DESCRIPTION(DM_NAME " least-pending multipath path selector");
MODULE_AUTHOR("Sakshi Chaitanya Veni <vsakshi@hp.com>");
MODULE_LICENSE("GPL");

