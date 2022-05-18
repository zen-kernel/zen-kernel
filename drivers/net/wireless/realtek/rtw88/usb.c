// SPDX-License-Identifier: GPL-2.0 OR BSD-3-Clause
/* Copyright(c) 2018-2019  Realtek Corporation
 */

#include <linux/module.h>
#include <linux/usb.h>
#include <linux/mutex.h>
#include "main.h"
#include "debug.h"
#include "reg.h"
#include "tx.h"
#include "rx.h"
#include "fw.h"
#include "ps.h"
#include "usb.h"

#define RTW_USB_MAX_RXQ_LEN	128

struct rtw_usb_txcb {
	struct rtw_dev *rtwdev;
	struct sk_buff_head tx_ack_queue;
};

static void rtw_usb_fill_tx_checksum(struct rtw_usb *rtwusb,
				     struct sk_buff *skb, int agg_num)
{
	struct rtw_dev *rtwdev = rtwusb->rtwdev;
	struct rtw_tx_pkt_info pkt_info;

	SET_TX_DESC_DMA_TXAGG_NUM(skb->data, agg_num);
	pkt_info.pkt_offset = GET_TX_DESC_PKT_OFFSET(skb->data);
	rtw_tx_fill_txdesc_checksum(rtwdev, &pkt_info, skb->data);
}

static void usbctrl_async_callback(struct urb *urb)
{
	/* free dr */
	kfree(urb->setup_packet);
	/* free databuf */
	kfree(urb->transfer_buffer);
}

static int usbctrl_vendorreq_async_write(struct usb_device *udev, u8 request,
					  u16 value, u16 index, void *pdata,
					  u16 len)
{
	int rc;
	unsigned int pipe;
	u8 reqtype;
	struct usb_ctrlrequest *dr;
	struct urb *urb;
	const u16 databuf_maxlen = RTW_USB_VENQT_MAX_BUF_SIZE;
	u8 *databuf;

	if (WARN_ON_ONCE(len > databuf_maxlen))
		len = databuf_maxlen;

	pipe = usb_sndctrlpipe(udev, 0); /* write_out */
	reqtype = RTW_USB_CMD_WRITE;

	dr = kzalloc(sizeof(*dr), GFP_ATOMIC);
	if (!dr)
		return -ENOMEM;

	databuf = kmemdup(pdata, len, GFP_ATOMIC);
	if (!databuf) {
		kfree(dr);
		return -ENOMEM;
	}

	urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (!urb) {
		kfree(databuf);
		kfree(dr);
		return -ENOMEM;
	}

	dr->bRequestType = reqtype;
	dr->bRequest = request;
	dr->wValue = cpu_to_le16(value);
	dr->wIndex = cpu_to_le16(index);
	dr->wLength = cpu_to_le16(len);

	usb_fill_control_urb(urb, udev, pipe,
			     (unsigned char *)dr, databuf, len,
			     usbctrl_async_callback, NULL);
	rc = usb_submit_urb(urb, GFP_ATOMIC);
	if (rc < 0) {
		kfree(databuf);
		kfree(dr);
	}

	usb_free_urb(urb);

	return rc;
}

static u32 rtw_usb_read_sync(struct rtw_dev *rtwdev, u32 addr, u16 len)
{
	struct rtw_usb *rtwusb = rtw_get_usb_priv(rtwdev);
	struct usb_device *udev = rtwusb->udev;
	__le32 *data;
	unsigned long flags;
	int ret;
	static int count;

	spin_lock_irqsave(&rtwusb->usb_lock, flags);

	if (++rtwusb->usb_data_index >= RTW_USB_MAX_RX_COUNT)
		rtwusb->usb_data_index = 0;
	data = &rtwusb->usb_data[rtwusb->usb_data_index];

	spin_unlock_irqrestore(&rtwusb->usb_lock, flags);

	ret = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0),
				 RTW_USB_CMD_REQ, RTW_USB_CMD_READ, addr,
				 RTW_USB_VENQT_CMD_IDX, data, len, 1000);
	if (ret < 0 && ret != -ENODEV && count++ < 4)
		rtw_err(rtwdev, "reg 0x%x, usbctrl_vendorreq failed with %d\n",
			addr, ret);

	return le32_to_cpu(*data);
}

static u8 rtw_usb_read8_sync(struct rtw_dev *rtwdev, u32 addr)
{
	return (u8)rtw_usb_read_sync(rtwdev, addr, 1);
}

static u16 rtw_usb_read16_sync(struct rtw_dev *rtwdev, u32 addr)
{
	return (u16)rtw_usb_read_sync(rtwdev, addr, 2);
}

static u32 rtw_usb_read32_sync(struct rtw_dev *rtwdev, u32 addr)
{
	return (u32)rtw_usb_read_sync(rtwdev, addr, 4);
}

static void rtw_usb_write_async(struct rtw_dev *rtwdev, u32 addr, u32 val,
				u16 len)
{
	struct rtw_usb *rtwusb = rtw_get_usb_priv(rtwdev);
	struct usb_device *udev = rtwusb->udev;
	u8 request;
	u16 wvalue;
	u16 index;
	__le32 data;

	request = RTW_USB_CMD_REQ;
	index = RTW_USB_VENQT_CMD_IDX; /* n/a */
	wvalue = (u16)(addr & 0x0000ffff);
	data = cpu_to_le32(val);
	usbctrl_vendorreq_async_write(udev, request, wvalue, index, &data, len);
}

static void rtw_usb_write8_async(struct rtw_dev *rtwdev, u32 addr, u8 val)
{
	rtw_usb_write_async(rtwdev, addr, val, 1);
}

static void rtw_usb_write16_async(struct rtw_dev *rtwdev, u32 addr, u16 val)
{
	rtw_usb_write_async(rtwdev, addr, val, 2);
}

static void rtw_usb_write32_async(struct rtw_dev *rtwdev, u32 addr, u32 val)
{
	rtw_usb_write_async(rtwdev, addr, val, 4);
}

static int rtw_usb_parse(struct rtw_dev *rtwdev,
			 struct usb_interface *interface)
{
	struct rtw_usb *rtwusb = rtw_get_usb_priv(rtwdev);
	struct usb_host_interface *host_interface = &interface->altsetting[0];
	struct usb_interface_descriptor *interface_desc = &host_interface->desc;
	struct usb_endpoint_descriptor *endpoint;
	struct usb_device *usbd = interface_to_usbdev(interface);
	int num_out_pipes = 0;
	int i;
	u8 num;

	for (i = 0; i < interface_desc->bNumEndpoints; i++) {
		endpoint = &host_interface->endpoint[i].desc;
		num = usb_endpoint_num(endpoint);

		if (usb_endpoint_dir_in(endpoint) &&
		    usb_endpoint_xfer_bulk(endpoint)) {
			if (rtwusb->pipe_in) {
				rtw_err(rtwdev, "IN pipes overflow\n");
				return -EINVAL;
			}

			rtwusb->pipe_in = num;
		}

		if (usb_endpoint_dir_in(endpoint) &&
		    usb_endpoint_xfer_int(endpoint)) {
			if (rtwusb->pipe_interrupt) {
				rtw_err(rtwdev, "INT pipes overflow\n");
				return -EINVAL;
			}

			rtwusb->pipe_interrupt = num;
		}

		if (usb_endpoint_dir_out(endpoint) &&
		    usb_endpoint_xfer_bulk(endpoint)) {
			if (num_out_pipes >= ARRAY_SIZE(rtwusb->out_ep)) {
				rtw_err(rtwdev, "OUT pipes overflow\n");
				return -EINVAL;
			}

			rtwusb->out_ep[num_out_pipes++] = num;
		}
	}

	switch (usbd->speed) {
	case USB_SPEED_LOW:
	case USB_SPEED_FULL:
		rtwusb->bulkout_size = RTW_USB_FULL_SPEED_BULK_SIZE;
		break;
	case USB_SPEED_HIGH:
		rtwusb->bulkout_size = RTW_USB_HIGH_SPEED_BULK_SIZE;
		break;
	case USB_SPEED_SUPER:
		rtwusb->bulkout_size = RTW_USB_SUPER_SPEED_BULK_SIZE;
		break;
	default:
		rtw_err(rtwdev, "failed to detect usb speed\n");
		return -EINVAL;
	}

	rtwdev->hci.bulkout_num = num_out_pipes;

	switch (num_out_pipes) {
	case 4:
	case 3:
		rtwusb->qsel_to_ep[TX_DESC_QSEL_TID0] = 2;
		rtwusb->qsel_to_ep[TX_DESC_QSEL_TID1] = 2;
		rtwusb->qsel_to_ep[TX_DESC_QSEL_TID2] = 2;
		rtwusb->qsel_to_ep[TX_DESC_QSEL_TID3] = 2;
		rtwusb->qsel_to_ep[TX_DESC_QSEL_TID4] = 1;
		rtwusb->qsel_to_ep[TX_DESC_QSEL_TID5] = 1;
		rtwusb->qsel_to_ep[TX_DESC_QSEL_TID6] = 1;
		rtwusb->qsel_to_ep[TX_DESC_QSEL_TID7] = 1;
		break;
	case 2:
		rtwusb->qsel_to_ep[TX_DESC_QSEL_TID0] = 1;
		rtwusb->qsel_to_ep[TX_DESC_QSEL_TID1] = 1;
		rtwusb->qsel_to_ep[TX_DESC_QSEL_TID2] = 1;
		rtwusb->qsel_to_ep[TX_DESC_QSEL_TID3] = 1;
		break;
	case 1:
		break;
	default:
		rtw_err(rtwdev, "failed to get out_pipes(%d)\n", num_out_pipes);
		return -EINVAL;
	}

	return 0;
}

static void rtw_usb_txcb_enqueue(struct rtw_usb_txcb *txcb, struct sk_buff *skb)
{
	skb_queue_tail(&txcb->tx_ack_queue, skb);
}

static void rtw_usb_tx_agg_skb(struct rtw_usb *rtwusb, struct sk_buff_head *list,
			       struct sk_buff *skb_head, struct sk_buff *skb,
			       struct rtw_usb_txcb *txcb)
{
	struct sk_buff *skb_iter;
	unsigned long flags;
	u8 *data_ptr;
	int agg_num = 0, len, max_len;

	data_ptr = skb_head->data;
	skb_iter = skb;

	while (skb_iter) {
		memcpy(data_ptr, skb_iter->data, skb_iter->len);
		len = ALIGN(skb_iter->len, 8);
		skb_put(skb_head, len);
		data_ptr += len;
		agg_num++;

		rtw_usb_txcb_enqueue(txcb, skb_iter);

		spin_lock_irqsave(&list->lock, flags);

		skb_iter = skb_peek(list);
		max_len = RTW_USB_MAX_XMITBUF_SZ - skb_head->len;

		if (skb_iter && skb_iter->len < max_len)
			__skb_unlink(skb_iter, list);
		else
			skb_iter = NULL;
		spin_unlock_irqrestore(&list->lock, flags);
	}

	if (agg_num > 1)
		rtw_usb_fill_tx_checksum(rtwusb, skb_head, agg_num);
}

static void rtw_usb_indicate_tx_status(struct rtw_dev *rtwdev,
				       struct sk_buff *skb)
{
	struct ieee80211_hw *hw = rtwdev->hw;
	struct ieee80211_tx_info *info = IEEE80211_SKB_CB(skb);
	struct rtw_usb_tx_data *tx_data = rtw_usb_get_tx_data(skb);

	/* enqueue to wait for tx report */
	if (info->flags & IEEE80211_TX_CTL_REQ_TX_STATUS) {
		rtw_tx_report_enqueue(rtwdev, skb, tx_data->sn);
		return;
	}

	/* always ACK for others, then they won't be marked as drop */
	ieee80211_tx_info_clear_status(info);
	if (info->flags & IEEE80211_TX_CTL_NO_ACK)
		info->flags |= IEEE80211_TX_STAT_NOACK_TRANSMITTED;
	else
		info->flags |= IEEE80211_TX_STAT_ACK;

	ieee80211_tx_status_irqsafe(hw, skb);
}

static void rtw_usb_write_port_tx_complete(struct urb *urb)
{
	struct rtw_usb_txcb *txcb = urb->context;
	struct rtw_dev *rtwdev = txcb->rtwdev;

	while (true) {
		struct sk_buff *skb = skb_dequeue(&txcb->tx_ack_queue);
		if (!skb)
			break;

		if (GET_TX_DESC_QSEL(skb->data) <= TX_DESC_QSEL_TID7)
			rtw_usb_indicate_tx_status(rtwdev, skb);
		else
			dev_kfree_skb_any(skb);
	}

	kfree(txcb);
}

static int rtw_usb_write_port(struct rtw_dev *rtwdev, u8 qsel, struct sk_buff *skb,
			      usb_complete_t cb, void *context)
{
	struct rtw_usb *rtwusb = rtw_get_usb_priv(rtwdev);
	struct usb_device *usbd = rtwusb->udev;
	struct urb *urb;
	unsigned int pipe;
	int ret;
	int ep = rtwusb->qsel_to_ep[qsel];

	pipe = usb_sndbulkpipe(usbd, rtwusb->out_ep[ep]);
	urb = usb_alloc_urb(0, GFP_ATOMIC);
	if (!urb)
		return -ENOMEM;

	usb_fill_bulk_urb(urb, usbd, pipe, skb->data, skb->len, cb, context);
	ret = usb_submit_urb(urb, GFP_ATOMIC);

	usb_free_urb(urb);

	return ret;
}

static struct sk_buff *rtw_usb_tx_agg_check(struct rtw_usb *rtwusb,
					    struct sk_buff *skb,
					    int index,
					    struct rtw_usb_txcb *txcb)
{
	struct sk_buff_head *list;
	struct sk_buff *skb_head;

	list = &rtwusb->tx_queue[index];
	if (skb_queue_empty(list))
		return NULL;

	skb_head = dev_alloc_skb(RTW_USB_MAX_XMITBUF_SZ);
	if (!skb_head)
		return NULL;

	rtw_usb_tx_agg_skb(rtwusb, list, skb_head, skb, txcb);

	return skb_head;
}

static void rtw_usb_tx_agg(struct rtw_usb *rtwusb, struct sk_buff *skb, int index)
{
	struct rtw_dev *rtwdev = rtwusb->rtwdev;
	struct sk_buff *skb_head;
	struct rtw_usb_txcb *txcb;
	u8 qsel;

	txcb = kmalloc(sizeof(*txcb), GFP_ATOMIC);
	if (!txcb)
		return;

	txcb->rtwdev = rtwdev;
	skb_queue_head_init(&txcb->tx_ack_queue);

	skb_head = rtw_usb_tx_agg_check(rtwusb, skb, index, txcb);
	if (!skb_head) {
		skb_head = skb;
		rtw_usb_txcb_enqueue(txcb, skb);
	}

	qsel = GET_TX_DESC_QSEL(skb->data);

	rtw_usb_write_port(rtwdev, qsel, skb_head,
			   rtw_usb_write_port_tx_complete, txcb);

	if (skb_head != skb)
		dev_kfree_skb(skb_head);
}

static void rtw_usb_tx_handler(struct work_struct *work)
{
	struct rtw_usb *rtwusb = container_of(work, struct rtw_usb, tx_work);
	struct sk_buff *skb;
	int index, limit;

	for (index = ARRAY_SIZE(rtwusb->tx_queue) - 1; index >= 0; index--) {
		for (limit = 0; limit < 200; limit++) {
			skb = skb_dequeue(&rtwusb->tx_queue[index]);
			if (skb)
				rtw_usb_tx_agg(rtwusb, skb, index);
			else
				break;
		}
	}
}

static void rtw_usb_tx_queue_purge(struct rtw_usb *rtwusb)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(rtwusb->tx_queue); i++)
		skb_queue_purge(&rtwusb->tx_queue[i]);
}

static void rtw_usb_write_port_complete(struct urb *urb)
{
	struct sk_buff *skb = urb->context;

	dev_kfree_skb_any(skb);
}

static int rtw_usb_write_data(struct rtw_dev *rtwdev,
			      struct rtw_tx_pkt_info *pkt_info,
			      u8 *buf)
{
	struct rtw_chip_info *chip = rtwdev->chip;
	struct sk_buff *skb;
	unsigned int desclen, headsize, size;
	u8 qsel;
	int ret = 0;

	size = pkt_info->tx_pkt_size;
	qsel = pkt_info->qsel;
	desclen = chip->tx_pkt_desc_sz;
	headsize = pkt_info->offset ? pkt_info->offset : desclen;

	skb = dev_alloc_skb(headsize + size);
	if (unlikely(!skb))
		return -ENOMEM;

	skb_reserve(skb, headsize);
	skb_put_data(skb, buf, size);
	skb_push(skb, headsize);
	memset(skb->data, 0, headsize);
	rtw_tx_fill_tx_desc(pkt_info, skb);
	rtw_tx_fill_txdesc_checksum(rtwdev, pkt_info, skb->data);

	ret = rtw_usb_write_port(rtwdev, qsel, skb,
				 rtw_usb_write_port_complete, skb);
	if (unlikely(ret))
		rtw_err(rtwdev, "failed to do USB write, ret=%d\n", ret);

	return ret;
}

static int rtw_usb_write_data_rsvd_page(struct rtw_dev *rtwdev, u8 *buf,
					u32 size)
{
	struct rtw_chip_info *chip = rtwdev->chip;
	struct rtw_usb *rtwusb;
	struct rtw_tx_pkt_info pkt_info = {0};
	u32 len, desclen;

	if (unlikely(!rtwdev))
		return -EINVAL;

	rtwusb = rtw_get_usb_priv(rtwdev);
	if (unlikely(!rtwusb))
		return -EINVAL;

	pkt_info.tx_pkt_size = size;
	pkt_info.qsel = TX_DESC_QSEL_BEACON;

	desclen = chip->tx_pkt_desc_sz;
	len = desclen + size;
	if (len % rtwusb->bulkout_size == 0) {
		len += RTW_USB_PACKET_OFFSET_SZ;
		pkt_info.offset = desclen + RTW_USB_PACKET_OFFSET_SZ;
		pkt_info.pkt_offset = 1;
	} else {
		pkt_info.offset = desclen;
	}

	return rtw_usb_write_data(rtwdev, &pkt_info, buf);
}

static int rtw_usb_write_data_h2c(struct rtw_dev *rtwdev, u8 *buf, u32 size)
{
	struct rtw_tx_pkt_info pkt_info = {0};

	pkt_info.tx_pkt_size = size;
	pkt_info.qsel = TX_DESC_QSEL_H2C;

	return rtw_usb_write_data(rtwdev, &pkt_info, buf);
}

static u8 rtw_usb_tx_queue_mapping_to_qsel(struct sk_buff *skb)
{
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	__le16 fc = hdr->frame_control;
	u8 qsel;

	if (unlikely(ieee80211_is_mgmt(fc) || ieee80211_is_ctl(fc)))
		qsel = TX_DESC_QSEL_MGMT;
	else if (skb_get_queue_mapping(skb) <= IEEE80211_AC_BK)
		qsel = skb->priority;
	else
		qsel = TX_DESC_QSEL_BEACON;

	return qsel;
}

static int rtw_usb_tx_write(struct rtw_dev *rtwdev,
			    struct rtw_tx_pkt_info *pkt_info,
			    struct sk_buff *skb)
{
	struct rtw_usb *rtwusb = rtw_get_usb_priv(rtwdev);
	struct rtw_chip_info *chip = rtwdev->chip;
	struct rtw_usb_tx_data *tx_data;
	u8 *pkt_desc;
	int ep;

	pkt_desc = skb_push(skb, chip->tx_pkt_desc_sz);
	memset(pkt_desc, 0, chip->tx_pkt_desc_sz);
	pkt_info->qsel = rtw_usb_tx_queue_mapping_to_qsel(skb);
	ep = rtwusb->qsel_to_ep[pkt_info->qsel];
	rtw_tx_fill_tx_desc(pkt_info, skb);
	rtw_tx_fill_txdesc_checksum(rtwdev, pkt_info, skb->data);
	tx_data = rtw_usb_get_tx_data(skb);
	tx_data->sn = pkt_info->sn;

	skb_queue_tail(&rtwusb->tx_queue[ep], skb);

	return 0;
}

static void rtw_usb_tx_kick_off(struct rtw_dev *rtwdev)
{
	struct rtw_usb *rtwusb = rtw_get_usb_priv(rtwdev);

	queue_work(rtwusb->txwq, &rtwusb->tx_work);
}

static void rtw_usb_rx_handler(struct work_struct *work)
{
	struct rtw_usb *rtwusb = container_of(work, struct rtw_usb, rx_work);
	struct rtw_dev *rtwdev = rtwusb->rtwdev;
	struct rtw_chip_info *chip = rtwdev->chip;
	struct rtw_rx_pkt_stat pkt_stat;
	struct ieee80211_rx_status rx_status;
	struct sk_buff *skb;
	u32 pkt_desc_sz = chip->rx_pkt_desc_sz;
	u32 pkt_offset;
	u8 *rx_desc;
	int limit;

	for (limit = 0; limit < 200; limit++) {
		skb = skb_dequeue(&rtwusb->rx_queue);
		if (!skb)
			break;

		rx_desc = skb->data;
		chip->ops->query_rx_desc(rtwdev, rx_desc, &pkt_stat,
					 &rx_status);
		pkt_offset = pkt_desc_sz + pkt_stat.drv_info_sz +
			     pkt_stat.shift;

		if (pkt_stat.is_c2h) {
			skb_put(skb, pkt_stat.pkt_len + pkt_offset);
			rtw_fw_c2h_cmd_rx_irqsafe(rtwdev, pkt_offset,
						  skb);
			continue;
		}

		if (skb_queue_len(&rtwusb->rx_queue) >= RTW_USB_MAX_RXQ_LEN) {
			rtw_err(rtwdev, "failed to get rx_queue, overflow\n");
			dev_kfree_skb_any(skb);
			continue;
		}

		skb_put(skb, pkt_stat.pkt_len);
		skb_reserve(skb, pkt_offset);
		memcpy(skb->cb, &rx_status, sizeof(rx_status));
		ieee80211_rx_irqsafe(rtwdev->hw, skb);
	}
}

static void rtw_usb_rx_data_put(struct rtw_usb *rtwusb,
				struct rx_usb_ctrl_block *rxcb)
{
	unsigned long flags;

	spin_lock_irqsave(&rtwusb->rx_data_list_lock, flags);
	list_move(&rxcb->list, &rtwusb->rx_data_free);
	spin_unlock_irqrestore(&rtwusb->rx_data_list_lock, flags);
}

static void rtw_usb_read_port_complete(struct urb *urb)
{
	struct rx_usb_ctrl_block *rxcb = urb->context;
	struct rtw_dev *rtwdev = rxcb->rtwdev;
	struct rtw_usb *rtwusb = rtw_get_usb_priv(rtwdev);
	struct sk_buff *skb = rxcb->rx_skb;

	if (urb->status == 0) {
		if (urb->actual_length >= RTW_USB_MAX_RECVBUF_SZ ||
		    urb->actual_length < 24) {
			rtw_err(rtwdev, "failed to get urb length:%d\n",
				urb->actual_length);
			if (skb)
				dev_kfree_skb_any(skb);
		} else {
			skb_queue_tail(&rtwusb->rx_queue, skb);
			queue_work(rtwusb->rxwq, &rtwusb->rx_work);
		}

		rtw_usb_rx_data_put(rtwusb, rxcb);
		queue_work(rtwusb->rxwq, &rtwusb->rx_refill_work);
	} else {
		switch (urb->status) {
		case -EINVAL:
		case -EPIPE:
		case -ENODEV:
		case -ESHUTDOWN:
		case -ENOENT:
		case -EPROTO:
		case -EILSEQ:
		case -ETIME:
		case -ECOMM:
		case -EOVERFLOW:
		case -EINPROGRESS:
			break;
		default:
			rtw_err(rtwdev, "status unknown=%d\n", urb->status);
			break;
		}
		if (skb)
			dev_kfree_skb_any(skb);
	}
}

static void rtw_usb_rx_refill_work(struct work_struct *work)
{
	struct rtw_usb *rtwusb = container_of(work, struct rtw_usb, rx_refill_work);
	struct rtw_dev *rtwdev = rtwusb->rtwdev;
	struct rx_usb_ctrl_block *rxcb;
	unsigned long flags;
	int error;

	do {
		spin_lock_irqsave(&rtwusb->rx_data_list_lock, flags);

		rxcb = list_first_entry_or_null(&rtwusb->rx_data_free,
						struct rx_usb_ctrl_block, list);

		spin_unlock_irqrestore(&rtwusb->rx_data_list_lock, flags);
		if (!rxcb)
			return;

		rxcb->rx_skb = alloc_skb(RTW_USB_MAX_RECVBUF_SZ, GFP_KERNEL);
		if (!rxcb->rx_skb) {
			rtw_err(rtwdev, "could not allocate rx skbuff\n");
			return;
		}

		usb_fill_bulk_urb(rxcb->rx_urb, rtwusb->udev,
				  usb_rcvbulkpipe(rtwusb->udev, rtwusb->pipe_in),
				  rxcb->rx_skb->data, RTW_USB_MAX_RECVBUF_SZ,
				  rtw_usb_read_port_complete, rxcb);

		spin_lock_irqsave(&rtwusb->rx_data_list_lock, flags);
		list_move(&rxcb->list, &rtwusb->rx_data_used);
		spin_unlock_irqrestore(&rtwusb->rx_data_list_lock, flags);

		error = usb_submit_urb(rxcb->rx_urb, GFP_KERNEL);
		if (error) {
			kfree_skb(rxcb->rx_skb);
			if (error != -ENODEV)
				rtw_err(rtwdev, "Err sending rx data urb %d\n",
					   error);
			rtw_usb_rx_data_put(rtwusb, rxcb);

			return;
		}
	} while (true);
}

static void rtw_usb_cancel_rx_bufs(struct rtw_usb *rtwusb)
{
	struct rx_usb_ctrl_block *rxcb;
	unsigned long flags;

	spin_lock_irqsave(&rtwusb->rx_data_list_lock, flags);

	while (true) {
		rxcb = list_first_entry_or_null(&rtwusb->rx_data_used,
						struct rx_usb_ctrl_block, list);

		spin_unlock_irqrestore(&rtwusb->rx_data_list_lock, flags);

		if (!rxcb)
			break;

		usb_kill_urb(rxcb->rx_urb);

		spin_lock_irqsave(&rtwusb->rx_data_list_lock, flags);
		list_move(&rxcb->list, &rtwusb->rx_data_free);
	}
}

static void rtw_usb_free_rx_bufs(struct rtw_usb *rtwusb)
{
	struct rx_usb_ctrl_block *rxcb;
	unsigned long flags;

	rtw_usb_cancel_rx_bufs(rtwusb);

	spin_lock_irqsave(&rtwusb->rx_data_list_lock, flags);

	while (true) {
		rxcb = list_first_entry_or_null(&rtwusb->rx_data_free, struct rx_usb_ctrl_block, list);

		spin_unlock_irqrestore(&rtwusb->rx_data_list_lock, flags);

		if (!rxcb)
			break;

		usb_free_urb(rxcb->rx_urb);

		spin_lock_irqsave(&rtwusb->rx_data_list_lock, flags);
		list_del(&rxcb->list);
	}
}

static int rtw_usb_alloc_rx_bufs(struct rtw_usb *rtwusb)
{
	int i;

	for (i = 0; i < RTW_USB_RXCB_NUM; i++) {
		struct rx_usb_ctrl_block *rxcb = &rtwusb->rx_cb[i];

		rxcb->rtwdev = rtwusb->rtwdev;
		rxcb->rx_urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!rxcb->rx_urb)
			goto err;
		list_add_tail(&rxcb->list, &rtwusb->rx_data_free);
	}

	return 0;
err:
	rtw_usb_free_rx_bufs(rtwusb);
	return -ENOMEM;
}

static int rtw_usb_setup(struct rtw_dev *rtwdev)
{
	/* empty function for rtw_hci_ops */
	return 0;
}

static int rtw_usb_start(struct rtw_dev *rtwdev)
{
	struct rtw_usb *rtwusb = rtw_get_usb_priv(rtwdev);

	queue_work(rtwusb->rxwq, &rtwusb->rx_refill_work);

	return 0;
}

static void rtw_usb_stop(struct rtw_dev *rtwdev)
{
	struct rtw_usb *rtwusb = rtw_get_usb_priv(rtwdev);

	cancel_work_sync(&rtwusb->rx_refill_work);
	rtw_usb_cancel_rx_bufs(rtwusb);
}

static void rtw_usb_deep_ps(struct rtw_dev *rtwdev, bool enter)
{
	/* empty function for rtw_hci_ops */
}

static void rtw_usb_link_ps(struct rtw_dev *rtwdev, bool enter)
{
	/* empty function for rtw_hci_ops */
}

static void rtw_usb_interface_cfg(struct rtw_dev *rtwdev)
{
	/* empty function for rtw_hci_ops */
}

static struct rtw_hci_ops rtw_usb_ops = {
	.tx_write = rtw_usb_tx_write,
	.tx_kick_off = rtw_usb_tx_kick_off,
	.setup = rtw_usb_setup,
	.start = rtw_usb_start,
	.stop = rtw_usb_stop,
	.deep_ps = rtw_usb_deep_ps,
	.link_ps = rtw_usb_link_ps,
	.interface_cfg = rtw_usb_interface_cfg,

	.write8  = rtw_usb_write8_async,
	.write16 = rtw_usb_write16_async,
	.write32 = rtw_usb_write32_async,
	.read8	= rtw_usb_read8_sync,
	.read16 = rtw_usb_read16_sync,
	.read32 = rtw_usb_read32_sync,

	.write_data_rsvd_page = rtw_usb_write_data_rsvd_page,
	.write_data_h2c = rtw_usb_write_data_h2c,
};

static int rtw_usb_init_rx(struct rtw_dev *rtwdev)
{
	struct rtw_usb *rtwusb = rtw_get_usb_priv(rtwdev);

	rtwusb->rxwq = create_singlethread_workqueue("rtw88_usb: rx wq");
	if (!rtwusb->rxwq) {
		rtw_err(rtwdev, "failed to create RX work queue\n");
		return -ENOMEM;
	}

	skb_queue_head_init(&rtwusb->rx_queue);

	INIT_WORK(&rtwusb->rx_work, rtw_usb_rx_handler);

	return 0;
}

static void rtw_usb_deinit_rx(struct rtw_dev *rtwdev)
{
	struct rtw_usb *rtwusb = rtw_get_usb_priv(rtwdev);

	skb_queue_purge(&rtwusb->rx_queue);

	flush_workqueue(rtwusb->rxwq);
	destroy_workqueue(rtwusb->rxwq);
}

static int rtw_usb_init_tx(struct rtw_dev *rtwdev)
{
	struct rtw_usb *rtwusb = rtw_get_usb_priv(rtwdev);
	int i;

	rtwusb->txwq = create_singlethread_workqueue("rtw88_usb: tx wq");
	if (!rtwusb->txwq) {
		rtw_err(rtwdev, "failed to create TX work queue\n");
		return -ENOMEM;
	}

	for (i = 0; i < ARRAY_SIZE(rtwusb->tx_queue); i++)
		skb_queue_head_init(&rtwusb->tx_queue[i]);

	INIT_WORK(&rtwusb->tx_work, rtw_usb_tx_handler);

	return 0;
}

static void rtw_usb_deinit_tx(struct rtw_dev *rtwdev)
{
	struct rtw_usb *rtwusb = rtw_get_usb_priv(rtwdev);

	rtw_usb_tx_queue_purge(rtwusb);
	flush_workqueue(rtwusb->txwq);
	destroy_workqueue(rtwusb->txwq);
}

static int rtw_usb_intf_init(struct rtw_dev *rtwdev,
			     struct usb_interface *intf)
{
	struct rtw_usb *rtwusb = rtw_get_usb_priv(rtwdev);
	struct usb_device *udev = usb_get_dev(interface_to_usbdev(intf));
	int ret;

	rtwusb->udev = udev;
	ret = rtw_usb_parse(rtwdev, intf);
	if (ret)
		return ret;

	rtwusb->usb_data = kcalloc(RTW_USB_MAX_RX_COUNT, sizeof(u32),
				   GFP_KERNEL);
	if (!rtwusb->usb_data)
		return -ENOMEM;

	usb_set_intfdata(intf, rtwdev->hw);

	SET_IEEE80211_DEV(rtwdev->hw, &intf->dev);
	spin_lock_init(&rtwusb->usb_lock);

	return 0;
}

static void rtw_usb_intf_deinit(struct rtw_dev *rtwdev,
				struct usb_interface *intf)
{
	struct rtw_usb *rtwusb = rtw_get_usb_priv(rtwdev);

	usb_put_dev(rtwusb->udev);
	usb_set_intfdata(intf, NULL);
}

int rtw_usb_probe(struct usb_interface *intf, const struct usb_device_id *id)
{
	struct rtw_dev *rtwdev;
	struct ieee80211_hw *hw;
	struct rtw_usb *rtwusb;
	int drv_data_size;
	int ret;

	drv_data_size = sizeof(struct rtw_dev) + sizeof(struct rtw_usb);
	hw = ieee80211_alloc_hw(drv_data_size, &rtw_ops);
	if (!hw)
		return -ENOMEM;

	rtwdev = hw->priv;
	rtwdev->hw = hw;
	rtwdev->dev = &intf->dev;
	rtwdev->chip = (struct rtw_chip_info *)id->driver_info;
	rtwdev->hci.ops = &rtw_usb_ops;
	rtwdev->hci.type = RTW_HCI_TYPE_USB;

	rtwusb = rtw_get_usb_priv(rtwdev);
	rtwusb->rtwdev = rtwdev;

	INIT_WORK(&rtwusb->rx_refill_work, rtw_usb_rx_refill_work);
	INIT_LIST_HEAD(&rtwusb->rx_data_free);
	INIT_LIST_HEAD(&rtwusb->rx_data_used);
	spin_lock_init(&rtwusb->rx_data_list_lock);

	ret = rtw_usb_alloc_rx_bufs(rtwusb);
	if (ret)
		return ret;

	ret = rtw_core_init(rtwdev);
	if (ret)
		goto err_release_hw;

	ret = rtw_usb_intf_init(rtwdev, intf);
	if (ret) {
		rtw_err(rtwdev, "failed to init USB interface\n");
		goto err_deinit_core;
	}

	ret = rtw_usb_init_tx(rtwdev);
	if (ret) {
		rtw_err(rtwdev, "failed to init USB TX\n");
		goto err_destroy_usb;
	}

	ret = rtw_usb_init_rx(rtwdev);
	if (ret) {
		rtw_err(rtwdev, "failed to init USB RX\n");
		goto err_destroy_txwq;
	}

	ret = rtw_chip_info_setup(rtwdev);
	if (ret) {
		rtw_err(rtwdev, "failed to setup chip information\n");
		goto err_destroy_rxwq;
	}

	ret = rtw_register_hw(rtwdev, rtwdev->hw);
	if (ret) {
		rtw_err(rtwdev, "failed to register hw\n");
		goto err_destroy_rxwq;
	}

	return 0;

err_destroy_rxwq:
	rtw_usb_deinit_rx(rtwdev);

err_destroy_txwq:
	rtw_usb_deinit_tx(rtwdev);

err_destroy_usb:
	rtw_usb_intf_deinit(rtwdev, intf);

err_deinit_core:
	rtw_core_deinit(rtwdev);

err_release_hw:
	ieee80211_free_hw(hw);

	return ret;
}
EXPORT_SYMBOL(rtw_usb_probe);

void rtw_usb_disconnect(struct usb_interface *intf)
{
	struct ieee80211_hw *hw = usb_get_intfdata(intf);
	struct rtw_dev *rtwdev;
	struct rtw_usb *rtwusb;

	if (!hw)
		return;

	rtwdev = hw->priv;
	rtwusb = rtw_get_usb_priv(rtwdev);

	rtw_unregister_hw(rtwdev, hw);
	rtw_usb_deinit_tx(rtwdev);
	rtw_usb_deinit_rx(rtwdev);

	if (rtwusb->udev->state != USB_STATE_NOTATTACHED)
		usb_reset_device(rtwusb->udev);

	rtw_usb_free_rx_bufs(rtwusb);

	rtw_usb_intf_deinit(rtwdev, intf);
	rtw_core_deinit(rtwdev);
	ieee80211_free_hw(hw);
}
EXPORT_SYMBOL(rtw_usb_disconnect);

MODULE_AUTHOR("Realtek Corporation");
MODULE_DESCRIPTION("Realtek 802.11ac wireless USB driver");
MODULE_LICENSE("Dual BSD/GPL");
