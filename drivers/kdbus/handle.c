/*
 * Copyright (C) 2013 Kay Sievers
 * Copyright (C) 2013 Greg Kroah-Hartman <gregkh@linuxfoundation.org>
 * Copyright (C) 2013 Daniel Mack <daniel@zonque.org>
 * Copyright (C) 2013 Linux Foundation
 *
 * kdbus is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#include <linux/device.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/idr.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "bus.h"
#include "connection.h"
#include "endpoint.h"
#include "handle.h"
#include "match.h"
#include "memfd.h"
#include "message.h"
#include "metadata.h"
#include "names.h"
#include "namespace.h"
#include "notify.h"
#include "policy.h"

/**
 * enum kdbus_handle_type - type a handle can be of
 * @_KDBUS_HANDLE_NULL:			Uninitialized/invalid
 * @KDBUS_HANDLE_CONTROL:		New file descriptor of a control node
 * @KDBUS_HANDLE_CONTROL_NS_OWNER:	File descriptor to hold a namespace
 * @KDBUS_HANDLE_CONTROL_BUS_OWNER:	File descriptor to hold a bus
 * @KDBUS_HANDLE_EP:			New file descriptor of a bus node
 * @KDBUS_HANDLE_EP_CONNECTED:		A bus connection after HELLO
 * @KDBUS_HANDLE_EP_OWNER:		File descriptor to hold an endpoint
 * @KDBUS_HANDLE_DISCONNECTED:		Handle is disconnected
 */
enum kdbus_handle_type {
	_KDBUS_HANDLE_NULL,
	KDBUS_HANDLE_CONTROL,
	KDBUS_HANDLE_CONTROL_NS_OWNER,
	KDBUS_HANDLE_CONTROL_BUS_OWNER,
	KDBUS_HANDLE_EP,
	KDBUS_HANDLE_EP_CONNECTED,
	KDBUS_HANDLE_EP_OWNER,
	KDBUS_HANDLE_DISCONNECTED,
};

/**
 * struct kdbus_handle - a handle to the kdbus system
 * @type:	Type of this handle (KDBUS_HANDLE_*)
 * @ns:		Namespace for this handle
 * @meta:	Cached connection creator's metadata/credentials
 * @ep:		The endpoint this handle owns, in case @type
 *		is KDBUS_HANDLE_EP
 * @ns_owner:	The namespace this handle owns, in case @type
 *		is KDBUS_HANDLE_CONTROL_NS_OWNER
 * @bus_owner:	The bus this handle owns, in case @type
 *		is KDBUS_HANDLE_CONTROL_BUS_OWNER
 * @ep_owner	The endpoint this handle owns, in case @type
 *		is KDBUS_HANDLE_EP_OWNER
 * @conn	The connection this handle owns, in case @type
 *		is KDBUS_HANDLE_EP, after HELLO it is
 *		KDBUS_HANDLE_EP_CONNECTED
 */
struct kdbus_handle {
	enum kdbus_handle_type type;
	struct kdbus_ns *ns;
	struct kdbus_meta *meta;
	struct kdbus_ep *ep;
	union {
		struct kdbus_ns *ns_owner;
		struct kdbus_bus *bus_owner;
		struct kdbus_ep *ep_owner;
		struct kdbus_conn *conn;
	};
};

static int kdbus_handle_open(struct inode *inode, struct file *file)
{
	struct kdbus_handle *handle;
	struct kdbus_ns *ns;
	struct kdbus_ep *ep;
	int ret;

	handle = kzalloc(sizeof(struct kdbus_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	/* find and reference namespace */
	ns = kdbus_ns_find_by_major(MAJOR(inode->i_rdev));
	if (!ns || ns->disconnected) {
		kfree(handle);
		return -ESHUTDOWN;
	}
	handle->ns = ns;
	file->private_data = handle;

	/* control device node */
	if (MINOR(inode->i_rdev) == 0) {
		handle->type = KDBUS_HANDLE_CONTROL;
		return 0;
	}

	/* find endpoint for device node */
	mutex_lock(&handle->ns->lock);
	ep = idr_find(&handle->ns->idr, MINOR(inode->i_rdev));
	if (!ep || ep->disconnected) {
		ret = -ESHUTDOWN;
		goto exit_unlock;
	}

	/* create endpoint connection */
	handle->type = KDBUS_HANDLE_EP;
	handle->ep = kdbus_ep_ref(ep);

	/* cache the metadata/credentials of the creator of the connection */
	ret = kdbus_meta_new(&handle->meta);
	if (ret < 0)
		goto exit_unlock;

	ret = kdbus_meta_append(handle->meta, NULL,
				KDBUS_ATTACH_CREDS |
				KDBUS_ATTACH_COMM |
				KDBUS_ATTACH_EXE |
				KDBUS_ATTACH_CMDLINE |
				KDBUS_ATTACH_CGROUP |
				KDBUS_ATTACH_CAPS |
				KDBUS_ATTACH_SECLABEL |
				KDBUS_ATTACH_AUDIT);
	if (ret < 0)
		goto exit_unlock;

	mutex_unlock(&handle->ns->lock);
	return 0;

exit_unlock:
	mutex_unlock(&handle->ns->lock);
	kdbus_ns_unref(handle->ns);
	kfree(handle);
	return ret;
}

static int kdbus_handle_release(struct inode *inode, struct file *file)
{
	struct kdbus_handle *handle = file->private_data;

	switch (handle->type) {
	case KDBUS_HANDLE_CONTROL_NS_OWNER:
		kdbus_ns_disconnect(handle->ns_owner);
		kdbus_ns_unref(handle->ns_owner);
		break;

	case KDBUS_HANDLE_CONTROL_BUS_OWNER:
		kdbus_bus_disconnect(handle->bus_owner);
		kdbus_bus_unref(handle->bus_owner);
		break;

	case KDBUS_HANDLE_EP_OWNER:
		kdbus_ep_disconnect(handle->ep_owner);
		kdbus_ep_unref(handle->ep_owner);
		break;

	case KDBUS_HANDLE_EP:
		kdbus_ep_unref(handle->ep);
		break;

	case KDBUS_HANDLE_EP_CONNECTED:
		kdbus_conn_disconnect(handle->conn, false);
		kdbus_conn_unref(handle->conn);
		break;

	default:
		break;
	}

	kdbus_meta_free(handle->meta);
	kdbus_ns_unref(handle->ns);
	kfree(handle);

	return 0;
}

static bool kdbus_check_flags(u64 kernel_flags)
{
	/*
	 * The higher 32bit are considered 'incompatible
	 * flags'. Refuse them all for now.
	 */
	return kernel_flags <= 0xFFFFFFFFULL;
}

/* kdbus control device commands */
static long kdbus_handle_ioctl_control(struct file *file, unsigned int cmd,
				       void __user *buf)
{
	struct kdbus_handle *handle = file->private_data;
	struct kdbus_cmd_make *make = NULL;
	struct kdbus_bus *bus = NULL;
	struct kdbus_ns *ns = NULL;
	umode_t mode = 0600;
	int ret;

	switch (cmd) {
	case KDBUS_CMD_BUS_MAKE: {
		kgid_t gid = KGIDT_INIT(0);
		size_t bloom_size;
		char *name;

		if (!KDBUS_IS_ALIGNED8((uintptr_t)buf)) {
			ret = -EFAULT;
			break;
		}

		ret = kdbus_bus_make_user(buf, &make, &name, &bloom_size);
		if (ret < 0)
			break;

		if (!kdbus_check_flags(make->flags)) {
			ret = -ENOTSUPP;
			break;
		}

		if (make->flags & KDBUS_MAKE_ACCESS_WORLD) {
			mode = 0666;
		} else if (make->flags & KDBUS_MAKE_ACCESS_GROUP) {
			mode = 0660;
			gid = current_fsgid();
		}

		ret = kdbus_bus_new(handle->ns, make, name, bloom_size,
				    mode, current_fsuid(), gid, &bus);
		if (ret < 0)
			break;

		/* turn the control fd into a new bus owner device */
		handle->type = KDBUS_HANDLE_CONTROL_BUS_OWNER;
		handle->bus_owner = bus;
		break;
	}

	case KDBUS_CMD_NS_MAKE: {
		char *name;

		if (!capable(CAP_IPC_OWNER)) {
			ret = -EPERM;
			break;
		}

		if (!KDBUS_IS_ALIGNED8((uintptr_t)buf)) {
			ret = -EFAULT;
			break;
		}

		ret = kdbus_ns_make_user(buf, &make, &name);
		if (ret < 0)
			break;

		if (!kdbus_check_flags(make->flags)) {
			ret = -ENOTSUPP;
			break;
		}

		if (make->flags & KDBUS_MAKE_ACCESS_WORLD)
			mode = 0666;

		ret = kdbus_ns_new(kdbus_ns_init, name, mode, &ns);
		if (ret < 0)
			break;

		/* turn the control fd into a new ns owner device */
		handle->type = KDBUS_HANDLE_CONTROL_NS_OWNER;
		handle->ns_owner = ns;
		break;
	}

	case KDBUS_CMD_MEMFD_NEW: {
		int fd;
		int __user *addr = buf;

		ret = kdbus_memfd_new(&fd);
		if (ret < 0)
			break;

		if (put_user(fd, addr))
			ret = -EFAULT;
		break;
	}

	default:
		ret = -ENOTTY;
		break;
	}

	kfree(make);
	return ret;
}

/* kdbus endpoint make commands */
static long kdbus_handle_ioctl_ep(struct file *file, unsigned int cmd,
				  void __user *buf)
{
	struct kdbus_handle *handle = file->private_data;
	struct kdbus_cmd_make *make = NULL;
	struct kdbus_cmd_hello *hello = NULL;
	long ret = 0;

	switch (cmd) {
	case KDBUS_CMD_EP_MAKE: {
		umode_t mode = 0;
		kgid_t gid = KGIDT_INIT(0);
		char *n;

		if (!KDBUS_IS_ALIGNED8((uintptr_t)buf)) {
			ret = -EFAULT;
			break;
		}

		ret = kdbus_ep_make_user(buf, &make, &n);
		if (ret < 0)
			break;

		if (!kdbus_check_flags(make->flags)) {
			ret = -ENOTSUPP;
			break;
		}

		if (make->flags & KDBUS_MAKE_ACCESS_WORLD) {
			mode = 0666;
		} else if (make->flags & KDBUS_MAKE_ACCESS_GROUP) {
			mode = 0660;
			gid = current_fsgid();
		}

		ret = kdbus_ep_new(handle->ep->bus, handle->ep->bus->ns, n,
				   mode, current_fsuid(), gid,
				   make->flags & KDBUS_MAKE_POLICY_OPEN);

		handle->type = KDBUS_HANDLE_EP_OWNER;
		break;
	}

	case KDBUS_CMD_HELLO: {
		/* turn this fd into a connection. */
		size_t size;
		void *v;

		if (!KDBUS_IS_ALIGNED8((uintptr_t)buf)) {
			ret = -EFAULT;
			break;
		}

		if (kdbus_size_get_user(&size, buf, struct kdbus_cmd_hello)) {
			ret = -EFAULT;
			break;
		}

		if (size < sizeof(struct kdbus_cmd_hello) ||
		    size > KDBUS_HELLO_MAX_SIZE) {
			ret = -EMSGSIZE;
			break;
		}

		v = memdup_user(buf, size);
		if (IS_ERR(v)) {
			ret = PTR_ERR(v);
			break;
		}
		hello = v;

		if (!kdbus_check_flags(hello->conn_flags)) {
			ret = -ENOTSUPP;
			break;
		}

		if (hello->pool_size == 0 ||
		    !IS_ALIGNED(hello->pool_size, PAGE_SIZE)) {
			ret = -EFAULT;
			break;
		}

		ret = kdbus_conn_new(handle->ep, hello, handle->meta,
				     &handle->conn);
		if (ret < 0)
			break;

		handle->type = KDBUS_HANDLE_EP_CONNECTED;

		if (copy_to_user(buf, hello, sizeof(struct kdbus_cmd_hello))) {
			kdbus_conn_unref(handle->conn);
			ret = -EFAULT;
		}

		break;
	}

	default:
		ret = -ENOTTY;
		break;
	}

	kfree(make);
	kfree(hello);

	return ret;
}

/* kdbus endpoint commands for connected peers */
static long kdbus_handle_ioctl_ep_connected(struct file *file, unsigned int cmd,
					    void __user *buf)
{
	struct kdbus_handle *handle = file->private_data;
	struct kdbus_conn *conn = handle->conn;
	struct kdbus_bus *bus = conn->ep->bus;
	long ret = 0;

	switch (cmd) {
	case KDBUS_CMD_BYEBYE:
		ret = kdbus_conn_disconnect(conn, true);
		if (ret == 0) {
			kdbus_conn_unref(conn);
			handle->type = KDBUS_HANDLE_DISCONNECTED;
		}
		break;

	case KDBUS_CMD_EP_POLICY_SET:
		/* upload a policy for this endpoint */
		if (!KDBUS_IS_ALIGNED8((uintptr_t)buf)) {
			ret = -EFAULT;
			break;
		}

		if (!conn->ep->policy_db) {
			ret = kdbus_policy_db_new(&conn->ep->policy_db);
			if (ret < 0)
				break;
		}

		ret = kdbus_cmd_policy_set_from_user(conn->ep->policy_db, buf);
		break;

	case KDBUS_CMD_NAME_ACQUIRE:
		/* acquire a well-known name */
		if (!KDBUS_IS_ALIGNED8((uintptr_t)buf)) {
			ret = -EFAULT;
			break;
		}

		ret = kdbus_cmd_name_acquire(bus->name_registry, conn, buf);
		break;

	case KDBUS_CMD_NAME_RELEASE:
		/* release a well-known name */
		if (!KDBUS_IS_ALIGNED8((uintptr_t)buf)) {
			ret = -EFAULT;
			break;
		}

		ret = kdbus_cmd_name_release(bus->name_registry, conn, buf);
		break;

	case KDBUS_CMD_NAME_LIST:
		/* query current IDs and names */
		if (!KDBUS_IS_ALIGNED8((uintptr_t)buf)) {
			ret = -EFAULT;
			break;
		}

		ret = kdbus_cmd_name_list(bus->name_registry, conn, buf);
		break;

	case KDBUS_CMD_CONN_INFO:
		/* return the properties of a connection */
		if (!KDBUS_IS_ALIGNED8((uintptr_t)buf)) {
			ret = -EFAULT;
			break;
		}

		ret = kdbus_cmd_conn_info(conn, buf);
		break;

	case KDBUS_CMD_MATCH_ADD:
		/* subscribe to/filter for broadcast messages */
		if (!KDBUS_IS_ALIGNED8((uintptr_t)buf)) {
			ret = -EFAULT;
			break;
		}

		ret = kdbus_match_db_add(conn, buf);
		break;

	case KDBUS_CMD_MATCH_REMOVE:
		/* unsubscribe from broadcast messages */
		if (!KDBUS_IS_ALIGNED8((uintptr_t)buf)) {
			ret = -EFAULT;
			break;
		}

		ret = kdbus_match_db_remove(conn, buf);
		break;

	case KDBUS_CMD_MSG_SEND: {
		/* submit a message which will be queued in the receiver */
		struct kdbus_kmsg *kmsg = NULL;

		if (!KDBUS_IS_ALIGNED8((uintptr_t)buf)) {
			ret = -EFAULT;
			break;
		}

		ret = kdbus_kmsg_new_from_user(conn, buf, &kmsg);
		if (ret < 0)
			break;

		ret = kdbus_conn_kmsg_send(conn->ep, conn, kmsg);
		kdbus_kmsg_free(kmsg);
		break;
	}

	case KDBUS_CMD_MSG_RECV:
		/* receive a pointer to a queued message */
		if (!KDBUS_IS_ALIGNED8((uintptr_t)buf)) {
			ret = -EFAULT;
			break;
		}

		ret = kdbus_conn_recv_msg(conn, buf);
		break;

	case KDBUS_CMD_FREE: {
		u64 off;

		/* free the memory used in the receiver's pool */
		if (!KDBUS_IS_ALIGNED8((uintptr_t)buf)) {
			ret = -EFAULT;
			break;
		}

		if (copy_from_user(&off, buf, sizeof(__u64))) {
			ret = -EFAULT;
			break;
		}

		mutex_lock(&conn->lock);
		ret = kdbus_pool_free_range(conn->pool, off);
		mutex_unlock(&conn->lock);
		break;
	}

	case KDBUS_CMD_MSG_DROP:
		ret = kdbus_conn_drop_msg(conn);
		break;

	case KDBUS_CMD_MSG_SRC:
		ret = kdbus_conn_src_msg(conn, buf);
		break;

	case KDBUS_CMD_MEMFD_NEW: {
		int fd;
		int __user *addr = buf;

		ret = kdbus_memfd_new(&fd);
		if (ret < 0)
			break;

		if (put_user(fd, addr))
			ret = -EFAULT;
		break;
	}

	default:
		ret = -ENOTTY;
		break;
	}

	return ret;
}

static long kdbus_handle_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	struct kdbus_handle *handle = file->private_data;
	void __user *argp = (void __user *)arg;

	switch (handle->type) {
	case KDBUS_HANDLE_CONTROL:
		return kdbus_handle_ioctl_control(file, cmd, argp);

	case KDBUS_HANDLE_EP:
		return kdbus_handle_ioctl_ep(file, cmd, argp);

	case KDBUS_HANDLE_EP_CONNECTED:
		return kdbus_handle_ioctl_ep_connected(file, cmd, argp);

	default:
		return -EBADFD;
	}
}

static unsigned int kdbus_handle_poll(struct file *file,
				      struct poll_table_struct *wait)
{
	struct kdbus_handle *handle = file->private_data;
	struct kdbus_conn *conn;
	unsigned int mask = 0;
	bool disconnected;

	/* Only a connected endpoint can read/write data */
	if (handle->type != KDBUS_HANDLE_EP_CONNECTED)
		return POLLERR | POLLHUP;

	conn = handle->conn;

	poll_wait(file, &conn->ep->wait, wait);

	mutex_lock(&conn->lock);

	mutex_lock(&conn->ep->lock);
	disconnected = conn->ep->disconnected;
	mutex_unlock(&conn->ep->lock);

	if (unlikely(disconnected))
		mask |= POLLERR | POLLHUP;
	else if (!list_empty(&conn->msg_list))
		mask |= POLLIN | POLLRDNORM;

	mutex_unlock(&conn->lock);

	return mask;
}

static int kdbus_handle_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct kdbus_handle *handle = file->private_data;

	if (handle->type != KDBUS_HANDLE_EP_CONNECTED)
		return -EPERM;

	if (handle->conn->flags & KDBUS_HELLO_ACTIVATOR)
		return -EPERM;

	return kdbus_pool_mmap(handle->conn->pool, vma);
}

const struct file_operations kdbus_device_ops = {
	.owner =		THIS_MODULE,
	.open =			kdbus_handle_open,
	.release =		kdbus_handle_release,
	.poll =			kdbus_handle_poll,
	.llseek =		noop_llseek,
	.unlocked_ioctl =	kdbus_handle_ioctl,
	.mmap =			kdbus_handle_mmap,
#ifdef CONFIG_COMPAT
	.compat_ioctl =		kdbus_handle_ioctl,
#endif
};
