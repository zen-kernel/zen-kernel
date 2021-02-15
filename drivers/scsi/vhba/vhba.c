/*
 * vhba.c
 *
 * Copyright (C) 2007-2012 Chia-I Wu <b90201047 AT ntu DOT edu DOT tw>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <linux/version.h>

#include <linux/init.h>
#include <linux/module.h>
#include <linux/highmem.h>
#include <linux/fs.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
#include <linux/sched/signal.h>
#else
#include <linux/sched.h>
#endif
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/scatterlist.h>
#ifdef CONFIG_COMPAT
#include <linux/compat.h>
#endif
#include <asm/uaccess.h>
#include <scsi/scsi.h>
#include <scsi/scsi_host.h>
#include <scsi/scsi_cmnd.h>
#include <scsi/scsi_device.h>


MODULE_AUTHOR("Chia-I Wu");
MODULE_VERSION(VHBA_VERSION);
MODULE_DESCRIPTION("Virtual SCSI HBA");
MODULE_LICENSE("GPL");

#ifdef DEBUG
#define DPRINTK(fmt, args...) printk(KERN_DEBUG "%s: " fmt, __FUNCTION__, ## args)
#else
#define DPRINTK(fmt, args...)
#endif

/* scmd_dbg was introduced in 3.15 */
#ifndef scmd_dbg
#define scmd_dbg(scmd, fmt, a...)       \
    dev_dbg(&(scmd)->device->sdev_gendev, fmt, ##a)
#endif

#define VHBA_MAX_SECTORS_PER_IO 256
#define VHBA_MAX_BUS 16
#define VHBA_MAX_ID 16 /* Usually 8 or 16 */
#define VHBA_MAX_DEVICES (VHBA_MAX_BUS * (VHBA_MAX_ID-1))
#define VHBA_CAN_QUEUE 32
#define VHBA_INVALID_BUS -1
#define VHBA_INVALID_ID -1
#define VHBA_KBUF_SIZE PAGE_SIZE

#define DATA_TO_DEVICE(dir) ((dir) == DMA_TO_DEVICE || (dir) == DMA_BIDIRECTIONAL)
#define DATA_FROM_DEVICE(dir) ((dir) == DMA_FROM_DEVICE || (dir) == DMA_BIDIRECTIONAL)


enum vhba_req_state {
    VHBA_REQ_FREE,
    VHBA_REQ_PENDING,
    VHBA_REQ_READING,
    VHBA_REQ_SENT,
    VHBA_REQ_WRITING,
};

struct vhba_command {
    struct scsi_cmnd *cmd;
    unsigned long serial_number;
    int status;
    struct list_head entry;
};

struct vhba_device {
    int bus; /* aka. channel */
    int id;
    int num;
    spinlock_t cmd_lock;
    struct list_head cmd_list;
    wait_queue_head_t cmd_wq;
    atomic_t refcnt;

    unsigned char *kbuf;
    size_t kbuf_size;

    unsigned long cmd_count;
};

struct vhba_host {
    struct Scsi_Host *shost;
    spinlock_t cmd_lock;
    int cmd_next;
    struct vhba_command commands[VHBA_CAN_QUEUE];
    spinlock_t dev_lock;
    struct vhba_device *devices[VHBA_MAX_DEVICES];
    int num_devices;
    DECLARE_BITMAP(chgmap, VHBA_MAX_DEVICES);
    int chgtype[VHBA_MAX_DEVICES];
    struct work_struct scan_devices;
};

#define MAX_COMMAND_SIZE 16

struct vhba_request {
    __u32 tag;
    __u32 lun;
    __u8 cdb[MAX_COMMAND_SIZE];
    __u8 cdb_len;
    __u32 data_len;
};

struct vhba_response {
    __u32 tag;
    __u32 status;
    __u32 data_len;
};

static struct vhba_command *vhba_alloc_command (void);
static void vhba_free_command (struct vhba_command *vcmd);

static struct platform_device vhba_platform_device;

static struct vhba_device *vhba_device_alloc (void)
{
    struct vhba_device *vdev;

    vdev = kzalloc(sizeof(struct vhba_device), GFP_KERNEL);
    if (!vdev) {
        return NULL;
    }

    vdev->bus = VHBA_INVALID_BUS;
    vdev->id = VHBA_INVALID_ID;
    spin_lock_init(&vdev->cmd_lock);
    INIT_LIST_HEAD(&vdev->cmd_list);
    init_waitqueue_head(&vdev->cmd_wq);
    atomic_set(&vdev->refcnt, 1);

    vdev->kbuf = NULL;
    vdev->kbuf_size = 0;

    vdev->cmd_count = 0;

    return vdev;
}

static void devnum_to_bus_and_id(int devnum, int *bus, int *id)
{
    int a = devnum / (VHBA_MAX_ID-1);
    int b = devnum % (VHBA_MAX_ID-1);

    *bus = a;
    *id  = b + 1;
}

static int bus_and_id_to_devnum(int bus, int id)
{
    return (bus * (VHBA_MAX_ID-1)) + id - 1;
}

static void vhba_device_put (struct vhba_device *vdev)
{
    if (atomic_dec_and_test(&vdev->refcnt)) {
        kfree(vdev);
    }
}

static struct vhba_device *vhba_device_get (struct vhba_device *vdev)
{
    atomic_inc(&vdev->refcnt);

    return vdev;
}

static int vhba_device_queue (struct vhba_device *vdev, struct scsi_cmnd *cmd)
{
    struct vhba_command *vcmd;
    unsigned long flags;

    vcmd = vhba_alloc_command();
    if (!vcmd) {
        return SCSI_MLQUEUE_HOST_BUSY;
    }

    vcmd->cmd = cmd;

    spin_lock_irqsave(&vdev->cmd_lock, flags);
    vcmd->serial_number = vdev->cmd_count++;
    list_add_tail(&vcmd->entry, &vdev->cmd_list);
    spin_unlock_irqrestore(&vdev->cmd_lock, flags);

    wake_up_interruptible(&vdev->cmd_wq);

    return 0;
}

static int vhba_device_dequeue (struct vhba_device *vdev, struct scsi_cmnd *cmd)
{
    struct vhba_command *vcmd;
    int retval;
    unsigned long flags;

    spin_lock_irqsave(&vdev->cmd_lock, flags);
    list_for_each_entry(vcmd, &vdev->cmd_list, entry) {
        if (vcmd->cmd == cmd) {
            list_del_init(&vcmd->entry);
            break;
        }
    }

    /* command not found */
    if (&vcmd->entry == &vdev->cmd_list) {
        spin_unlock_irqrestore(&vdev->cmd_lock, flags);
        return SUCCESS;
    }

    while (vcmd->status == VHBA_REQ_READING || vcmd->status == VHBA_REQ_WRITING) {
        spin_unlock_irqrestore(&vdev->cmd_lock, flags);
        scmd_dbg(cmd, "wait for I/O before aborting\n");
        schedule_timeout(1);
        spin_lock_irqsave(&vdev->cmd_lock, flags);
    }

    retval = (vcmd->status == VHBA_REQ_SENT) ? FAILED : SUCCESS;

    vhba_free_command(vcmd);

    spin_unlock_irqrestore(&vdev->cmd_lock, flags);

    return retval;
}

static inline void vhba_scan_devices_add (struct vhba_host *vhost, int bus, int id)
{
    struct scsi_device *sdev;

    sdev = scsi_device_lookup(vhost->shost, bus, id, 0);
    if (!sdev) {
        scsi_add_device(vhost->shost, bus, id, 0);
    } else {
        dev_warn(&vhost->shost->shost_gendev, "tried to add an already-existing device %d:%d:0!\n", bus, id);
        scsi_device_put(sdev);
    }
}

static inline void vhba_scan_devices_remove (struct vhba_host *vhost, int bus, int id)
{
    struct scsi_device *sdev;

    sdev = scsi_device_lookup(vhost->shost, bus, id, 0);
    if (sdev) {
        scsi_remove_device(sdev);
        scsi_device_put(sdev);
    } else {
        dev_warn(&vhost->shost->shost_gendev, "tried to remove non-existing device %d:%d:0!\n", bus, id);
    }
}

static void vhba_scan_devices (struct work_struct *work)
{
    struct vhba_host *vhost = container_of(work, struct vhba_host, scan_devices);
    unsigned long flags;
    int devnum, change, exists;
    int bus, id;

    while (1) {
        spin_lock_irqsave(&vhost->dev_lock, flags);

        devnum = find_first_bit(vhost->chgmap, VHBA_MAX_DEVICES);
        if (devnum >= VHBA_MAX_DEVICES) {
            spin_unlock_irqrestore(&vhost->dev_lock, flags);
            break;
        }
        change = vhost->chgtype[devnum];
        exists = vhost->devices[devnum] != NULL;

        vhost->chgtype[devnum] = 0;
        clear_bit(devnum, vhost->chgmap);

        spin_unlock_irqrestore(&vhost->dev_lock, flags);

        devnum_to_bus_and_id(devnum, &bus, &id);

        if (change < 0) {
            dev_dbg(&vhost->shost->shost_gendev, "trying to remove target %d:%d:0\n", bus, id);
            vhba_scan_devices_remove(vhost, bus, id);
        } else if (change > 0) {
            dev_dbg(&vhost->shost->shost_gendev, "trying to add target %d:%d:0\n", bus, id);
            vhba_scan_devices_add(vhost, bus, id);
        } else {
            /* quick sequence of add/remove or remove/add; we determine
               which one it was by checking if device structure exists */
            if (exists) {
                /* remove followed by add: remove and (re)add */
                dev_dbg(&vhost->shost->shost_gendev, "trying to (re)add target %d:%d:0\n", bus, id);
                vhba_scan_devices_remove(vhost, bus, id);
                vhba_scan_devices_add(vhost, bus, id);
            } else {
                /* add followed by remove: no-op */
                dev_dbg(&vhost->shost->shost_gendev, "no-op for target %d:%d:0\n", bus, id);
            }
        }
    }
}

static int vhba_add_device (struct vhba_device *vdev)
{
    struct vhba_host *vhost;
    int i;
    unsigned long flags;
    int bus, id;

    vhost = platform_get_drvdata(&vhba_platform_device);

    vhba_device_get(vdev);

    spin_lock_irqsave(&vhost->dev_lock, flags);
    if (vhost->num_devices >= VHBA_MAX_DEVICES) {
        spin_unlock_irqrestore(&vhost->dev_lock, flags);
        vhba_device_put(vdev);
        return -EBUSY;
    }

    for (i = 0; i < VHBA_MAX_DEVICES; i++) {
        devnum_to_bus_and_id(i, &bus, &id);

        if (vhost->devices[i] == NULL) {
            vdev->bus = bus;
            vdev->id  = id;
            vdev->num = i;
            vhost->devices[i] = vdev;
            vhost->num_devices++;
            set_bit(i, vhost->chgmap);
            vhost->chgtype[i]++;
            break;
        }
    }
    spin_unlock_irqrestore(&vhost->dev_lock, flags);

    schedule_work(&vhost->scan_devices);

    return 0;
}

static int vhba_remove_device (struct vhba_device *vdev)
{
    struct vhba_host *vhost;
    unsigned long flags;

    vhost = platform_get_drvdata(&vhba_platform_device);

    spin_lock_irqsave(&vhost->dev_lock, flags);
    set_bit(vdev->num, vhost->chgmap);
    vhost->chgtype[vdev->num]--;
    vhost->devices[vdev->num] = NULL;
    vhost->num_devices--;
    vdev->bus = VHBA_INVALID_BUS;
    vdev->id = VHBA_INVALID_ID;
    spin_unlock_irqrestore(&vhost->dev_lock, flags);

    vhba_device_put(vdev);

    schedule_work(&vhost->scan_devices);

    return 0;
}

static struct vhba_device *vhba_lookup_device (int devnum)
{
    struct vhba_host *vhost;
    struct vhba_device *vdev = NULL;
    unsigned long flags;

    vhost = platform_get_drvdata(&vhba_platform_device);

    if (likely(devnum < VHBA_MAX_DEVICES)) {
        spin_lock_irqsave(&vhost->dev_lock, flags);
        vdev = vhost->devices[devnum];
        if (vdev) {
            vdev = vhba_device_get(vdev);
        }

        spin_unlock_irqrestore(&vhost->dev_lock, flags);
    }

    return vdev;
}

static struct vhba_command *vhba_alloc_command (void)
{
    struct vhba_host *vhost;
    struct vhba_command *vcmd;
    unsigned long flags;
    int i;

    vhost = platform_get_drvdata(&vhba_platform_device);

    spin_lock_irqsave(&vhost->cmd_lock, flags);

    vcmd = vhost->commands + vhost->cmd_next++;
    if (vcmd->status != VHBA_REQ_FREE) {
        for (i = 0; i < vhost->shost->can_queue; i++) {
            vcmd = vhost->commands + i;

            if (vcmd->status == VHBA_REQ_FREE) {
                vhost->cmd_next = i + 1;
                break;
            }
        }

        if (i == vhost->shost->can_queue) {
            vcmd = NULL;
        }
    }

    if (vcmd) {
        vcmd->status = VHBA_REQ_PENDING;
    }

    vhost->cmd_next %= vhost->shost->can_queue;

    spin_unlock_irqrestore(&vhost->cmd_lock, flags);

    return vcmd;
}

static void vhba_free_command (struct vhba_command *vcmd)
{
    struct vhba_host *vhost;
    unsigned long flags;

    vhost = platform_get_drvdata(&vhba_platform_device);

    spin_lock_irqsave(&vhost->cmd_lock, flags);
    vcmd->status = VHBA_REQ_FREE;
    spin_unlock_irqrestore(&vhost->cmd_lock, flags);
}

static int vhba_queuecommand_lck (struct scsi_cmnd *cmd, void (*done)(struct scsi_cmnd *))
{
    struct vhba_device *vdev;
    int retval;

    scmd_dbg(cmd, "queue %p\n", cmd);

    vdev = vhba_lookup_device(bus_and_id_to_devnum(cmd->device->channel, cmd->device->id));
    if (!vdev) {
        scmd_dbg(cmd, "no such device\n");

        cmd->result = DID_NO_CONNECT << 16;
        done(cmd);

        return 0;
    }

    cmd->scsi_done = done;
    retval = vhba_device_queue(vdev, cmd);

    vhba_device_put(vdev);

    return retval;
}

#ifdef DEF_SCSI_QCMD
DEF_SCSI_QCMD(vhba_queuecommand)
#else
#define vhba_queuecommand vhba_queuecommand_lck
#endif

static int vhba_abort (struct scsi_cmnd *cmd)
{
    struct vhba_device *vdev;
    int retval = SUCCESS;

    scmd_dbg(cmd, "abort %p\n", cmd);

    vdev = vhba_lookup_device(bus_and_id_to_devnum(cmd->device->channel, cmd->device->id));
    if (vdev) {
        retval = vhba_device_dequeue(vdev, cmd);
        vhba_device_put(vdev);
    } else {
        cmd->result = DID_NO_CONNECT << 16;
    }

    return retval;
}

static struct scsi_host_template vhba_template = {
    .module = THIS_MODULE,
    .name = "vhba",
    .proc_name = "vhba",
    .queuecommand = vhba_queuecommand,
    .eh_abort_handler = vhba_abort,
    .can_queue = VHBA_CAN_QUEUE,
    .this_id = -1,
    .cmd_per_lun = 1,
    .max_sectors = VHBA_MAX_SECTORS_PER_IO,
    .sg_tablesize = 256,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
    .max_segment_size = VHBA_KBUF_SIZE,
#endif
};

static ssize_t do_request (struct vhba_device *vdev, unsigned long cmd_serial_number, struct scsi_cmnd *cmd, char __user *buf, size_t buf_len)
{
    struct vhba_request vreq;
    ssize_t ret;

    scmd_dbg(cmd, "request %lu (%p), cdb 0x%x, bufflen %d, sg count %d\n",
        cmd_serial_number, cmd, cmd->cmnd[0], scsi_bufflen(cmd), scsi_sg_count(cmd));

    ret = sizeof(vreq);
    if (DATA_TO_DEVICE(cmd->sc_data_direction)) {
        ret += scsi_bufflen(cmd);
    }

    if (ret > buf_len) {
        scmd_dbg(cmd, "buffer too small (%zd < %zd) for a request\n", buf_len, ret);
        return -EIO;
    }

    vreq.tag = cmd_serial_number;
    vreq.lun = cmd->device->lun;
    memcpy(vreq.cdb, cmd->cmnd, MAX_COMMAND_SIZE);
    vreq.cdb_len = cmd->cmd_len;
    vreq.data_len = scsi_bufflen(cmd);

    if (copy_to_user(buf, &vreq, sizeof(vreq))) {
        return -EFAULT;
    }

    if (DATA_TO_DEVICE(cmd->sc_data_direction) && vreq.data_len) {
        buf += sizeof(vreq);

        if (scsi_sg_count(cmd)) {
            unsigned char *kaddr, *uaddr;
            struct scatterlist *sglist = scsi_sglist(cmd);
            struct scatterlist *sg;
            int i;

            uaddr = (unsigned char *) buf;

            for_each_sg(sglist, sg, scsi_sg_count(cmd), i) {
                size_t len = sg->length;

                if (len > vdev->kbuf_size) {
                    scmd_dbg(cmd, "segment size (%zu) exceeds kbuf size (%zu)!", len, vdev->kbuf_size);
                    len = vdev->kbuf_size;
                }

                kaddr = kmap_atomic(sg_page(sg));
                memcpy(vdev->kbuf, kaddr + sg->offset, len);
                kunmap_atomic(kaddr);

                if (copy_to_user(uaddr, vdev->kbuf, len)) {
                    return -EFAULT;
                }
                uaddr += len;
            }
        } else {
            if (copy_to_user(buf, scsi_sglist(cmd), vreq.data_len)) {
                return -EFAULT;
            }
        }
    }

    return ret;
}

static ssize_t do_response (struct vhba_device *vdev, unsigned long cmd_serial_number, struct scsi_cmnd *cmd, const char __user *buf, size_t buf_len, struct vhba_response *res)
{
    ssize_t ret = 0;

    scmd_dbg(cmd, "response %lu (%p), status %x, data len %d, sg count %d\n",
         cmd_serial_number, cmd, res->status, res->data_len, scsi_sg_count(cmd));

    if (res->status) {
        unsigned char sense_stack[SCSI_SENSE_BUFFERSIZE];

        if (res->data_len > SCSI_SENSE_BUFFERSIZE) {
            scmd_dbg(cmd, "truncate sense (%d < %d)", SCSI_SENSE_BUFFERSIZE, res->data_len);
            res->data_len = SCSI_SENSE_BUFFERSIZE;
        }

        /* Copy via temporary buffer on stack in order to avoid problems
           with PAX on grsecurity-enabled kernels */
        if (copy_from_user(sense_stack, buf, res->data_len)) {
            return -EFAULT;
        }
        memcpy(cmd->sense_buffer, sense_stack, res->data_len);

        cmd->result = res->status;

        ret += res->data_len;
    } else if (DATA_FROM_DEVICE(cmd->sc_data_direction) && scsi_bufflen(cmd)) {
        size_t to_read;

        if (res->data_len > scsi_bufflen(cmd)) {
            scmd_dbg(cmd, "truncate data (%d < %d)\n", scsi_bufflen(cmd), res->data_len);
            res->data_len = scsi_bufflen(cmd);
        }

        to_read = res->data_len;

        if (scsi_sg_count(cmd)) {
            unsigned char *kaddr, *uaddr;
            struct scatterlist *sglist = scsi_sglist(cmd);
            struct scatterlist *sg;
            int i;

            uaddr = (unsigned char *)buf;

            for_each_sg(sglist, sg, scsi_sg_count(cmd), i) {
                size_t len = (sg->length < to_read) ? sg->length : to_read;

                if (len > vdev->kbuf_size) {
                    scmd_dbg(cmd, "segment size (%zu) exceeds kbuf size (%zu)!", len, vdev->kbuf_size);
                    len = vdev->kbuf_size;
                }

                if (copy_from_user(vdev->kbuf, uaddr, len)) {
                    return -EFAULT;
                }
                uaddr += len;

                kaddr = kmap_atomic(sg_page(sg));
                memcpy(kaddr + sg->offset, vdev->kbuf, len);
                kunmap_atomic(kaddr);

                to_read -= len;
                if (to_read == 0) {
                    break;
                }
            }
        } else {
            if (copy_from_user(scsi_sglist(cmd), buf, res->data_len)) {
                return -EFAULT;
            }

            to_read -= res->data_len;
        }

        scsi_set_resid(cmd, to_read);

        ret += res->data_len - to_read;
    }

    return ret;
}

static inline struct vhba_command *next_command (struct vhba_device *vdev)
{
    struct vhba_command *vcmd;

    list_for_each_entry(vcmd, &vdev->cmd_list, entry) {
        if (vcmd->status == VHBA_REQ_PENDING) {
            break;
        }
    }

    if (&vcmd->entry == &vdev->cmd_list) {
        vcmd = NULL;
    }

    return vcmd;
}

static inline struct vhba_command *match_command (struct vhba_device *vdev, u32 tag)
{
    struct vhba_command *vcmd;

    list_for_each_entry(vcmd, &vdev->cmd_list, entry) {
        if (vcmd->serial_number == tag) {
            break;
        }
    }

    if (&vcmd->entry == &vdev->cmd_list) {
        vcmd = NULL;
    }

    return vcmd;
}

static struct vhba_command *wait_command (struct vhba_device *vdev, unsigned long flags)
{
    struct vhba_command *vcmd;
    DEFINE_WAIT(wait);

    while (!(vcmd = next_command(vdev))) {
        if (signal_pending(current)) {
            break;
        }

        prepare_to_wait(&vdev->cmd_wq, &wait, TASK_INTERRUPTIBLE);

        spin_unlock_irqrestore(&vdev->cmd_lock, flags);

        schedule();

        spin_lock_irqsave(&vdev->cmd_lock, flags);
    }

    finish_wait(&vdev->cmd_wq, &wait);
    if (vcmd) {
        vcmd->status = VHBA_REQ_READING;
    }

    return vcmd;
}

static ssize_t vhba_ctl_read (struct file *file, char __user *buf, size_t buf_len, loff_t *offset)
{
    struct vhba_device *vdev;
    struct vhba_command *vcmd;
    ssize_t ret;
    unsigned long flags;

    vdev = file->private_data;

    /* Get next command */
    if (file->f_flags & O_NONBLOCK) {
        /* Non-blocking variant */
        spin_lock_irqsave(&vdev->cmd_lock, flags);
        vcmd = next_command(vdev);
        spin_unlock_irqrestore(&vdev->cmd_lock, flags);

        if (!vcmd) {
            return -EWOULDBLOCK;
        }
    } else {
        /* Blocking variant */
        spin_lock_irqsave(&vdev->cmd_lock, flags);
        vcmd = wait_command(vdev, flags);
        spin_unlock_irqrestore(&vdev->cmd_lock, flags);

        if (!vcmd) {
            return -ERESTARTSYS;
        }
    }

    ret = do_request(vdev, vcmd->serial_number, vcmd->cmd, buf, buf_len);

    spin_lock_irqsave(&vdev->cmd_lock, flags);
    if (ret >= 0) {
        vcmd->status = VHBA_REQ_SENT;
        *offset += ret;
    } else {
        vcmd->status = VHBA_REQ_PENDING;
    }

    spin_unlock_irqrestore(&vdev->cmd_lock, flags);

    return ret;
}

static ssize_t vhba_ctl_write (struct file *file, const char __user *buf, size_t buf_len, loff_t *offset)
{
    struct vhba_device *vdev;
    struct vhba_command *vcmd;
    struct vhba_response res;
    ssize_t ret;
    unsigned long flags;

    if (buf_len < sizeof(res)) {
        return -EIO;
    }

    if (copy_from_user(&res, buf, sizeof(res))) {
        return -EFAULT;
    }

    vdev = file->private_data;

    spin_lock_irqsave(&vdev->cmd_lock, flags);
    vcmd = match_command(vdev, res.tag);
    if (!vcmd || vcmd->status != VHBA_REQ_SENT) {
        spin_unlock_irqrestore(&vdev->cmd_lock, flags);
        DPRINTK("not expecting response\n");
        return -EIO;
    }
    vcmd->status = VHBA_REQ_WRITING;
    spin_unlock_irqrestore(&vdev->cmd_lock, flags);

    ret = do_response(vdev, vcmd->serial_number, vcmd->cmd, buf + sizeof(res), buf_len - sizeof(res), &res);

    spin_lock_irqsave(&vdev->cmd_lock, flags);
    if (ret >= 0) {
        vcmd->cmd->scsi_done(vcmd->cmd);
        ret += sizeof(res);

        /* don't compete with vhba_device_dequeue */
        if (!list_empty(&vcmd->entry)) {
            list_del_init(&vcmd->entry);
            vhba_free_command(vcmd);
        }
    } else {
        vcmd->status = VHBA_REQ_SENT;
    }

    spin_unlock_irqrestore(&vdev->cmd_lock, flags);

    return ret;
}

static long vhba_ctl_ioctl (struct file *file, unsigned int cmd, unsigned long arg)
{
    struct vhba_device *vdev = file->private_data;
    struct vhba_host *vhost;
    struct scsi_device *sdev;

    switch (cmd) {
        case 0xBEEF001: {
            vhost = platform_get_drvdata(&vhba_platform_device);
            sdev = scsi_device_lookup(vhost->shost, vdev->bus, vdev->id, 0);

            if (sdev) {
                int id[4] = {
                    sdev->host->host_no,
                    sdev->channel,
                    sdev->id,
                    sdev->lun
                };

                scsi_device_put(sdev);

                if (copy_to_user((void *)arg, id, sizeof(id))) {
                    return -EFAULT;
                }

                return 0;
            } else {
                return -ENODEV;
            }
        }
        case 0xBEEF002: {
            int device_number = vdev->num;
            if (copy_to_user((void *)arg, &device_number, sizeof(device_number))) {
                return -EFAULT;
            }
            return 0;
        }
    }

    return -ENOTTY;
}

#ifdef CONFIG_COMPAT
static long vhba_ctl_compat_ioctl (struct file *file, unsigned int cmd, unsigned long arg)
{
    unsigned long compat_arg = (unsigned long)compat_ptr(arg);
    return vhba_ctl_ioctl(file, cmd, compat_arg);
}
#endif

static unsigned int vhba_ctl_poll (struct file *file, poll_table *wait)
{
    struct vhba_device *vdev = file->private_data;
    unsigned int mask = 0;
    unsigned long flags;

    poll_wait(file, &vdev->cmd_wq, wait);

    spin_lock_irqsave(&vdev->cmd_lock, flags);
    if (next_command(vdev)) {
        mask |= POLLIN | POLLRDNORM;
    }
    spin_unlock_irqrestore(&vdev->cmd_lock, flags);

    return mask;
}

static int vhba_ctl_open (struct inode *inode, struct file *file)
{
    struct vhba_device *vdev;
    int retval;

    DPRINTK("open\n");

    /* check if vhba is probed */
    if (!platform_get_drvdata(&vhba_platform_device)) {
        return -ENODEV;
    }

    vdev = vhba_device_alloc();
    if (!vdev) {
        return -ENOMEM;
    }

    vdev->kbuf_size = VHBA_KBUF_SIZE;
    vdev->kbuf = kmalloc(vdev->kbuf_size, GFP_KERNEL);
    if (!vdev->kbuf) {
        return -ENOMEM;
    }

    if (!(retval = vhba_add_device(vdev))) {
        file->private_data = vdev;
    }

    vhba_device_put(vdev);

    return retval;
}

static int vhba_ctl_release (struct inode *inode, struct file *file)
{
    struct vhba_device *vdev;
    struct vhba_command *vcmd;
    unsigned long flags;

    DPRINTK("release\n");

    vdev = file->private_data;

    vhba_device_get(vdev);
    vhba_remove_device(vdev);

    spin_lock_irqsave(&vdev->cmd_lock, flags);
    list_for_each_entry(vcmd, &vdev->cmd_list, entry) {
        WARN_ON(vcmd->status == VHBA_REQ_READING || vcmd->status == VHBA_REQ_WRITING);

        scmd_dbg(vcmd->cmd, "device released with command %lu (%p)\n", vcmd->serial_number, vcmd->cmd);
        vcmd->cmd->result = DID_NO_CONNECT << 16;
        vcmd->cmd->scsi_done(vcmd->cmd);

        vhba_free_command(vcmd);
    }
    INIT_LIST_HEAD(&vdev->cmd_list);
    spin_unlock_irqrestore(&vdev->cmd_lock, flags);

    kfree(vdev->kbuf);
    vdev->kbuf = NULL;

    vhba_device_put(vdev);

    return 0;
}

static struct file_operations vhba_ctl_fops = {
    .owner = THIS_MODULE,
    .open = vhba_ctl_open,
    .release = vhba_ctl_release,
    .read = vhba_ctl_read,
    .write = vhba_ctl_write,
    .poll = vhba_ctl_poll,
    .unlocked_ioctl = vhba_ctl_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = vhba_ctl_compat_ioctl,
#endif
};

static struct miscdevice vhba_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "vhba_ctl",
    .fops = &vhba_ctl_fops,
};

static int vhba_probe (struct platform_device *pdev)
{
    struct Scsi_Host *shost;
    struct vhba_host *vhost;
    int i;

    shost = scsi_host_alloc(&vhba_template, sizeof(struct vhba_host));
    if (!shost) {
        return -ENOMEM;
    }

    shost->max_channel = VHBA_MAX_BUS-1;
    shost->max_id = VHBA_MAX_ID;
    /* we don't support lun > 0 */
    shost->max_lun = 1;
    shost->max_cmd_len = MAX_COMMAND_SIZE;

    vhost = (struct vhba_host *)shost->hostdata;
    memset(vhost, 0, sizeof(*vhost));

    vhost->shost = shost;
    vhost->num_devices = 0;
    spin_lock_init(&vhost->dev_lock);
    spin_lock_init(&vhost->cmd_lock);
    INIT_WORK(&vhost->scan_devices, vhba_scan_devices);
    vhost->cmd_next = 0;
    for (i = 0; i < vhost->shost->can_queue; i++) {
        vhost->commands[i].status = VHBA_REQ_FREE;
    }

    platform_set_drvdata(pdev, vhost);

    if (scsi_add_host(shost, &pdev->dev)) {
        scsi_host_put(shost);
        return -ENOMEM;
    }

    return 0;
}

static int vhba_remove (struct platform_device *pdev)
{
    struct vhba_host *vhost;
    struct Scsi_Host *shost;

    vhost = platform_get_drvdata(pdev);
    shost = vhost->shost;

    scsi_remove_host(shost);
    scsi_host_put(shost);

    return 0;
}

static void vhba_release (struct device * dev)
{
    return;
}

static struct platform_device vhba_platform_device = {
    .name = "vhba",
    .id = -1,
    .dev = {
        .release = vhba_release,
    },
};

static struct platform_driver vhba_platform_driver = {
    .driver = {
        .owner = THIS_MODULE,
        .name = "vhba",
    },
    .probe = vhba_probe,
    .remove = vhba_remove,
};

static int __init vhba_init (void)
{
    int ret;

    ret = platform_device_register(&vhba_platform_device);
    if (ret < 0) {
        return ret;
    }

    ret = platform_driver_register(&vhba_platform_driver);
    if (ret < 0) {
        platform_device_unregister(&vhba_platform_device);
        return ret;
    }

    ret = misc_register(&vhba_miscdev);
    if (ret < 0) {
        platform_driver_unregister(&vhba_platform_driver);
        platform_device_unregister(&vhba_platform_device);
        return ret;
    }

    return 0;
}

static void __exit vhba_exit(void)
{
    misc_deregister(&vhba_miscdev);
    platform_driver_unregister(&vhba_platform_driver);
    platform_device_unregister(&vhba_platform_device);
}

module_init(vhba_init);
module_exit(vhba_exit);

