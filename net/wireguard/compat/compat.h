/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2015-2019 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#ifndef _WG_COMPAT_H
#define _WG_COMPAT_H

#include <linux/kconfig.h>
#include <linux/version.h>
#include <linux/types.h>
#include <generated/utsrelease.h>

#ifdef RHEL_MAJOR
#if RHEL_MAJOR == 7
#define ISRHEL7
#endif
#endif
#ifdef UTS_UBUNTU_RELEASE_ABI
#if LINUX_VERSION_CODE == KERNEL_VERSION(3, 13, 11)
#define ISUBUNTU1404
#endif
#endif
#ifdef CONFIG_SUSE_KERNEL
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0) && LINUX_VERSION_CODE >= KERNEL_VERSION(4, 4, 0)
#define ISOPENSUSE42
#endif
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 13, 0) && LINUX_VERSION_CODE >= KERNEL_VERSION(4, 12, 0)
#define ISOPENSUSE15
#endif
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 0)
#error "WireGuard requires Linux >= 3.10"
#endif

#if defined(ISRHEL7)
#include <linux/skbuff.h>
#define headers_end headers_start
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3, 18, 0)
#define headers_start data
#define headers_end data
#endif

#include <linux/cache.h>
#ifndef __ro_after_init
#define __ro_after_init __read_mostly
#endif

#include <linux/compiler.h>
#ifndef READ_ONCE
#define READ_ONCE ACCESS_ONCE
#endif
#ifndef WRITE_ONCE
#ifdef ACCESS_ONCE_RW
#define WRITE_ONCE(p, v) (ACCESS_ONCE_RW(p) = (v))
#else
#define WRITE_ONCE(p, v) (ACCESS_ONCE(p) = (v))
#endif
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 17, 0)
#include "udp_tunnel/udp_tunnel_partial_compat.h"
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0) && !defined(DEBUG) && defined(net_dbg_ratelimited)
#undef net_dbg_ratelimited
#define net_dbg_ratelimited(fmt, ...) do { if (0) no_printk(KERN_DEBUG pr_fmt(fmt), ##__VA_ARGS__); } while (0)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
#include <linux/rcupdate.h>
#ifndef RCU_LOCKDEP_WARN
#define RCU_LOCKDEP_WARN(cond, message) rcu_lockdep_assert(!(cond), message)
#endif
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0) && !defined(ISRHEL7)
#define ipv6_dst_lookup(a, b, c, d) ipv6_dst_lookup(b, c, d)
#endif

#if (LINUX_VERSION_CODE == KERNEL_VERSION(4, 4, 0) || \
    (LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 5) && LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)) || \
    (LINUX_VERSION_CODE < KERNEL_VERSION(4, 1, 17) && LINUX_VERSION_CODE > KERNEL_VERSION(3, 19, 0)) || \
    (LINUX_VERSION_CODE < KERNEL_VERSION(3, 18, 27) && LINUX_VERSION_CODE >= KERNEL_VERSION(3, 17, 0)) || \
    (LINUX_VERSION_CODE < KERNEL_VERSION(3, 16, 8) && LINUX_VERSION_CODE >= KERNEL_VERSION(3, 15, 0)) || \
    (LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 40) && LINUX_VERSION_CODE >= KERNEL_VERSION(3, 13, 0)) || \
    (LINUX_VERSION_CODE < KERNEL_VERSION(3, 12, 54))) && !defined(ISUBUNTU1404)
#include <linux/if.h>
#include <net/ip_tunnels.h>
#define IP6_ECN_set_ce(a, b) IP6_ECN_set_ce(b)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 12, 0) && IS_ENABLED(CONFIG_IPV6) && !defined(ISRHEL7)
#include <net/ipv6.h>
struct ipv6_stub_type {
	void *udpv6_encap_enable;
	int (*ipv6_dst_lookup)(struct sock *sk, struct dst_entry **dst, struct flowi6 *fl6);
};
static const struct ipv6_stub_type ipv6_stub_impl = {
	.udpv6_encap_enable = (void *)1,
	.ipv6_dst_lookup = ip6_dst_lookup
};
static const struct ipv6_stub_type *ipv6_stub = &ipv6_stub_impl;
#endif


#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 8, 0) && IS_ENABLED(CONFIG_IPV6) && !defined(ISOPENSUSE42) && !defined(ISRHEL7)
#include <net/addrconf.h>
static inline bool ipv6_mod_enabled(void)
{
	return ipv6_stub != NULL && ipv6_stub->udpv6_encap_enable != NULL;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0) && !defined(ISRHEL7)
#include <linux/skbuff.h>
static inline void skb_reset_tc(struct sk_buff *skb)
{
#ifdef CONFIG_NET_CLS_ACT
	skb->tc_verd = 0;
#endif
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
#include <linux/random.h>
#include <linux/siphash.h>
static inline u32 __wgcompat_get_random_u32(void)
{
	static siphash_key_t key;
	static u32 counter = 0;
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)
	static bool has_seeded = false;
	if (unlikely(!has_seeded)) {
		get_random_bytes(&key, sizeof(key));
		has_seeded = true;
	}
#else
	get_random_once(&key, sizeof(key));
#endif
	return siphash_2u32(counter++, get_random_int(), &key);
}
#define get_random_u32 __wgcompat_get_random_u32
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 18, 0) && !defined(ISRHEL7)
static inline void netif_keep_dst(struct net_device *dev)
{
	dev->priv_flags &= ~IFF_XMIT_DST_RELEASE;
}
#define COMPAT_CANNOT_USE_CSUM_LEVEL
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0) && !defined(ISRHEL7)
#include <linux/netdevice.h>
#ifndef netdev_alloc_pcpu_stats
#define pcpu_sw_netstats pcpu_tstats
#endif
#ifndef netdev_alloc_pcpu_stats
#define netdev_alloc_pcpu_stats alloc_percpu
#endif
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3, 15, 0) && !defined(ISRHEL7)
#include <linux/netdevice.h>
#ifndef netdev_alloc_pcpu_stats
#define netdev_alloc_pcpu_stats(type)					\
({									\
	typeof(type) __percpu *pcpu_stats = alloc_percpu(type);		\
	if (pcpu_stats)	{						\
		int __cpu;						\
		for_each_possible_cpu(__cpu) {				\
			typeof(type) *stat;				\
			stat = per_cpu_ptr(pcpu_stats, __cpu);		\
			u64_stats_init(&stat->syncp);			\
		}							\
	}								\
	pcpu_stats;							\
})
#endif
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0) && !defined(ISRHEL7)
#include "checksum/checksum_partial_compat.h"
static inline void *our_pskb_put(struct sk_buff *skb, struct sk_buff *tail, int len)
{
	if (tail != skb) {
		skb->data_len += len;
		skb->len += len;
	}
	return skb_put(tail, len);
}
#define pskb_put our_pskb_put
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 11, 0) && !defined(ISRHEL7)
#include <net/xfrm.h>
static inline void skb_scrub_packet(struct sk_buff *skb, bool xnet)
{
#ifdef CONFIG_CAVIUM_OCTEON_IPFWD_OFFLOAD
	memset(&skb->cvm_info, 0, sizeof(skb->cvm_info));
	skb->cvm_reserved = 0;
#endif
	skb->tstamp.tv64 = 0;
	skb->pkt_type = PACKET_HOST;
	skb->skb_iif = 0;
	skb_dst_drop(skb);
	secpath_reset(skb);
	nf_reset(skb);
	nf_reset_trace(skb);
	if (!xnet)
		return;
	skb_orphan(skb);
	skb->mark = 0;
}
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(3, 12, 0) || defined(ISUBUNTU1404)) && !defined(ISRHEL7)
#include <linux/random.h>
static inline u32 prandom_u32_max(u32 ep_ro)
{
	return (u32)(((u64) prandom_u32() * ep_ro) >> 32);
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 75) && !defined(ISRHEL7)
#ifndef U8_MAX
#define U8_MAX ((u8)~0U)
#endif
#ifndef S8_MAX
#define S8_MAX ((s8)(U8_MAX >> 1))
#endif
#ifndef S8_MIN
#define S8_MIN ((s8)(-S8_MAX - 1))
#endif
#ifndef U16_MAX
#define U16_MAX ((u16)~0U)
#endif
#ifndef S16_MAX
#define S16_MAX ((s16)(U16_MAX >> 1))
#endif
#ifndef S16_MIN
#define S16_MIN ((s16)(-S16_MAX - 1))
#endif
#ifndef U32_MAX
#define U32_MAX ((u32)~0U)
#endif
#ifndef S32_MAX
#define S32_MAX ((s32)(U32_MAX >> 1))
#endif
#ifndef S32_MIN
#define S32_MIN ((s32)(-S32_MAX - 1))
#endif
#ifndef U64_MAX
#define U64_MAX ((u64)~0ULL)
#endif
#ifndef S64_MAX
#define S64_MAX ((s64)(U64_MAX >> 1))
#endif
#ifndef S64_MIN
#define S64_MIN ((s64)(-S64_MAX - 1))
#endif
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 10, 60) && !defined(ISRHEL7)
/* Making this static may very well invalidate its usefulness,
 * but so it goes with compat code. */
static inline void memzero_explicit(void *s, size_t count)
{
	memset(s, 0, count);
	barrier();
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 12, 0) && !defined(ISRHEL7)
static const struct in6_addr our_in6addr_any = IN6ADDR_ANY_INIT;
#define in6addr_any our_in6addr_any
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 13, 0) && LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0) && !defined(ISOPENSUSE15)
#include <linux/completion.h>
#include <linux/random.h>
#include <linux/errno.h>
struct rng_initializer {
	struct completion done;
	struct random_ready_callback cb;
};
static inline void rng_initialized_callback(struct random_ready_callback *cb)
{
	complete(&container_of(cb, struct rng_initializer, cb)->done);
}
static inline int wait_for_random_bytes(void)
{
	static bool rng_is_initialized = false;
	int ret;
	if (unlikely(!rng_is_initialized)) {
		struct rng_initializer rng = {
			.done = COMPLETION_INITIALIZER(rng.done),
			.cb = { .owner = THIS_MODULE, .func = rng_initialized_callback }
		};
		ret = add_random_ready_callback(&rng.cb);
		if (!ret) {
			ret = wait_for_completion_interruptible(&rng.done);
			if (ret) {
				del_random_ready_callback(&rng.cb);
				return ret;
			}
		} else if (ret != -EALREADY)
			return ret;
		rng_is_initialized = true;
	}
	return 0;
}
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
/* This is a disaster. Without this API, we really have no way of
 * knowing if it's initialized. We just return that it has and hope
 * for the best... */
static inline int wait_for_random_bytes(void)
{
	return 0;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 0) && LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
#include <linux/random.h>
#include <linux/slab.h>
struct rng_is_initialized_callback {
	struct random_ready_callback cb;
	atomic_t *rng_state;
};
static inline void rng_is_initialized_callback(struct random_ready_callback *cb)
{
	struct rng_is_initialized_callback *rdy = container_of(cb, struct rng_is_initialized_callback, cb);
	atomic_set(rdy->rng_state, 2);
	kfree(rdy);
}
static inline bool rng_is_initialized(void)
{
	static atomic_t rng_state = ATOMIC_INIT(0);

	if (atomic_read(&rng_state) == 2)
		return true;

	if (atomic_cmpxchg(&rng_state, 0, 1) == 0) {
		int ret;
		struct rng_is_initialized_callback *rdy = kmalloc(sizeof(*rdy), GFP_ATOMIC);
		if (!rdy) {
			atomic_set(&rng_state, 0);
			return false;
		}
		rdy->cb.owner = THIS_MODULE;
		rdy->cb.func = rng_is_initialized_callback;
		rdy->rng_state = &rng_state;
		ret = add_random_ready_callback(&rdy->cb);
		if (ret)
			kfree(rdy);
		if (ret == -EALREADY) {
			atomic_set(&rng_state, 2);
			return true;
		} else if (ret)
			atomic_set(&rng_state, 0);
		return false;
	}
	return false;
}
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
/* This is a disaster. Without this API, we really have no way of
 * knowing if it's initialized. We just return that it has and hope
 * for the best... */
static inline bool rng_is_initialized(void)
{
	return true;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 13, 0) && !defined(ISOPENSUSE15)
static inline int get_random_bytes_wait(void *buf, int nbytes)
{
	int ret = wait_for_random_bytes();
	if (unlikely(ret))
		return ret;
	get_random_bytes(buf, nbytes);
	return 0;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 11, 0) && !defined(ISRHEL7)
#define system_power_efficient_wq system_unbound_wq
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 17, 0) && !defined(ISRHEL7)
#include <linux/hrtimer.h>
static inline u64 ktime_get_boot_ns(void)
{
	return ktime_to_ns(ktime_get_boottime());
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0)
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 17, 0)
#include <linux/hrtimer.h>
#else
#include <linux/timekeeping.h>
#endif
static inline u64 __wgcompat_ktime_get_boot_fast_ns(void)
{
	return ktime_get_boot_ns();
}
#define ktime_get_boot_fast_ns __wgcompat_ktime_get_boot_fast_ns
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
#include <linux/inetdevice.h>
static inline __be32 our_confirm_addr_indev(struct in_device *in_dev, __be32 dst,  __be32 local, int scope)
{
	int same = 0;
	__be32 addr = 0;
	for_ifa(in_dev) {
		if (!addr && (local == ifa->ifa_local || !local) && ifa->ifa_scope <= scope) {
			addr = ifa->ifa_local;
			if (same)
				break;
		}
		if (!same) {
			same = (!local || inet_ifa_match(local, ifa)) && (!dst || inet_ifa_match(dst, ifa));
			if (same && addr) {
				if (local || !dst)
					break;
				if (inet_ifa_match(addr, ifa))
					break;
				if (ifa->ifa_scope <= scope) {
					addr = ifa->ifa_local;
					break;
				}
				same = 0;
			}
		}
	} endfor_ifa(in_dev);
	return same ? addr : 0;
}
static inline __be32 our_inet_confirm_addr(struct net *net, struct in_device *in_dev, __be32 dst, __be32 local, int scope)
{
	__be32 addr = 0;
	struct net_device *dev;
	if (in_dev)
		return our_confirm_addr_indev(in_dev, dst, local, scope);
	rcu_read_lock();
	for_each_netdev_rcu(net, dev) {
		in_dev = __in_dev_get_rcu(dev);
		if (in_dev) {
			addr = our_confirm_addr_indev(in_dev, dst, local, scope);
			if (addr)
				break;
		}
	}
	rcu_read_unlock();
	return addr;
}
#define inet_confirm_addr our_inet_confirm_addr
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/slab.h>
static inline void *kvmalloc_ours(size_t size, gfp_t flags)
{
	gfp_t kmalloc_flags = flags;
	void *ret;
	if (size > PAGE_SIZE) {
		kmalloc_flags |= __GFP_NOWARN;
		if (!(kmalloc_flags & __GFP_REPEAT) || (size <= PAGE_SIZE << PAGE_ALLOC_COSTLY_ORDER))
			kmalloc_flags |= __GFP_NORETRY;
	}
	ret = kmalloc(size, kmalloc_flags);
	if (ret || size <= PAGE_SIZE)
		return ret;
	return __vmalloc(size, flags, PAGE_KERNEL);
}
static inline void *kvzalloc_ours(size_t size, gfp_t flags)
{
	return kvmalloc_ours(size, flags | __GFP_ZERO);
}
#define kvmalloc kvmalloc_ours
#define kvzalloc kvzalloc_ours
#endif

#if ((LINUX_VERSION_CODE < KERNEL_VERSION(3, 15, 0) && LINUX_VERSION_CODE >= KERNEL_VERSION(3, 13, 0)) || LINUX_VERSION_CODE < KERNEL_VERSION(3, 12, 41)) && !defined(ISUBUNTU1404)
#include <linux/vmalloc.h>
#include <linux/mm.h>
static inline void kvfree_ours(const void *addr)
{
	if (is_vmalloc_addr(addr))
		vfree(addr);
	else
		kfree(addr);
}
#define kvfree kvfree_ours
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 9)
#include <linux/netdevice.h>
#define priv_destructor destructor
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 13, 0) && !defined(ISOPENSUSE15)
#define wg_newlink(a,b,c,d,e) wg_newlink(a,b,c,d)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
#include <net/netlink.h>
#include <net/genetlink.h>
#define nlmsg_parse(a, b, c, d, e, f) nlmsg_parse(a, b, c, d, e)
#define nla_parse_nested(a, b, c, d, e) nla_parse_nested(a, b, c, d)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0) && !defined(ISRHEL7)
static inline struct nlattr **genl_family_attrbuf(const struct genl_family *family)
{
	return family->attrbuf;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 12, 0)
#define PTR_ERR_OR_ZERO(p) PTR_RET(p)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 7, 0)
#include <net/netlink.h>
#define nla_put_u64_64bit(a, b, c, d) nla_put_u64(a, b, c)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 6, 0)
#include <net/genetlink.h>
#ifndef GENL_UNS_ADMIN_PERM
#define GENL_UNS_ADMIN_PERM GENL_ADMIN_PERM
#endif
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 10, 0) && !defined(ISRHEL7)
#include <net/genetlink.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 13, 0)
#define genl_register_family(a) genl_register_family_with_ops(a, genl_ops, ARRAY_SIZE(genl_ops))
#define COMPAT_CANNOT_USE_CONST_GENL_OPS
#else
#define genl_register_family(a) genl_register_family_with_ops(a, genl_ops)
#endif
#define COMPAT_CANNOT_USE_GENL_NOPS
#endif

#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 2) && LINUX_VERSION_CODE >= KERNEL_VERSION(4, 14, 0)) || (LINUX_VERSION_CODE < KERNEL_VERSION(4, 13, 16) && LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)) || (LINUX_VERSION_CODE < KERNEL_VERSION(4, 9, 65) && LINUX_VERSION_CODE >= KERNEL_VERSION(4, 5, 0)) || (LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 101) && LINUX_VERSION_CODE >= KERNEL_VERSION(3, 19, 0)) || LINUX_VERSION_CODE < KERNEL_VERSION(3, 18, 84)
#define ___COMPAT_NETLINK_DUMP_BLOCK { \
	int ret; \
	skb->end -= nlmsg_total_size(sizeof(int)); \
	ret = wg_get_device_dump_real(skb, cb); \
	skb->end += nlmsg_total_size(sizeof(int)); \
	return ret; \
}
#define ___COMPAT_NETLINK_DUMP_OVERRIDE
#else
#define ___COMPAT_NETLINK_DUMP_BLOCK return wg_get_device_dump_real(skb, cb);
#endif
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 8) && LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0)) || (LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 25) && LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)) || LINUX_VERSION_CODE < KERNEL_VERSION(4, 9, 87)
#define wg_get_device_dump(a, b) wg_get_device_dump_real(a, b); \
static int wg_get_device_dump(a, b) { \
	struct wg_device *wg = (struct wg_device *)cb->args[0]; \
	if (!wg) { \
		int ret = wg_get_device_start(cb); \
		if (ret) \
			return ret; \
	} \
	___COMPAT_NETLINK_DUMP_BLOCK \
} \
static int wg_get_device_dump_real(a, b)
#define COMPAT_CANNOT_USE_NETLINK_START
#elif defined(___COMPAT_NETLINK_DUMP_OVERRIDE)
#define wg_get_device_dump(a, b) wg_get_device_dump_real(a, b); \
static int wg_get_device_dump(a, b) { \
	___COMPAT_NETLINK_DUMP_BLOCK \
} \
static int wg_get_device_dump_real(a, b)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 17, 0)
#define COMPAT_CANNOT_USE_IN6_DEV_GET
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
#define COMPAT_CANNOT_USE_DEV_CNF
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
#define COMPAT_CANNOT_USE_IFF_NO_QUEUE
#endif

#if defined(CONFIG_X86_64) && LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0)
#include <asm/user.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 2, 0)
#include <asm/xsave.h>
#include <asm/xcr.h>
static inline int cpu_has_xfeatures(u64 xfeatures_needed, const char **feature_name)
{
	return xgetbv(XCR_XFEATURE_ENABLED_MASK) & xfeatures_needed;
}
#endif
#ifndef XFEATURE_MASK_YMM
#define XFEATURE_MASK_YMM XSTATE_YMM
#endif
#ifndef XFEATURE_MASK_SSE
#define XFEATURE_MASK_SSE XSTATE_SSE
#endif
#ifndef XSTATE_AVX512
#define XSTATE_AVX512 (XSTATE_OPMASK | XSTATE_ZMM_Hi256 | XSTATE_Hi16_ZMM)
#endif
#ifndef XFEATURE_MASK_AVX512
#define XFEATURE_MASK_AVX512 XSTATE_AVX512
#endif
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 6, 0) && defined(CONFIG_X86_64)
/* This is incredibly dumb and reckless, but as it turns out, there's
 * not really hardware Linux runs properly on that supports F but not BW
 * and VL, so in practice this isn't so bad. Plus, this is compat layer,
 * so the bar remains fairly low.
 */
#include <asm/cpufeature.h>
#ifndef X86_FEATURE_AVX512BW
#define X86_FEATURE_AVX512BW X86_FEATURE_AVX512F
#endif
#ifndef X86_FEATURE_AVX512VL
#define X86_FEATURE_AVX512VL X86_FEATURE_AVX512F
#endif
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 11, 0)
struct _____dummy_container { char dev; };
#define netdev_notifier_info net_device *)data); __attribute((unused)) char _____dummy = ((struct _____dummy_container
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
#define timer_setup(a, b, c) setup_timer(a, ((void (*)(unsigned long))b), ((unsigned long)a))
#define from_timer(var, callback_timer, timer_fieldname) container_of(callback_timer, typeof(*var), timer_fieldname)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 3)
#define COMPAT_CANNOT_USE_AVX512
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 17, 0)
#define timespec64 timespec
#define ktime_get_real_ts64 ktime_get_real_ts
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)
#include <net/genetlink.h>
#define genl_dump_check_consistent(a, b) genl_dump_check_consistent(a, b, &genl_family)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 13, 0) && !defined(ISRHEL7) && !defined(ISOPENSUSE15)
static inline void *skb_put_data(struct sk_buff *skb, const void *data, unsigned int len)
{
	void *tmp = skb_put(skb, len);
	memcpy(tmp, data, len);
	return tmp;
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 19, 0) && !defined(ISRHEL7)
#define napi_complete_done(n, work_done) napi_complete(n)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 5, 0)
#include <linux/netdevice.h>
/* NAPI_STATE_SCHED gets set by netif_napi_add anyway, so this is safe.
 * Also, kernels without NAPI_STATE_NO_BUSY_POLL don't have a call to
 * napi_hash_add inside of netif_napi_add.
 */
#define NAPI_STATE_NO_BUSY_POLL NAPI_STATE_SCHED
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 14, 0)
#include <linux/atomic.h>
#ifndef atomic_read_acquire
#define atomic_read_acquire(v) ({ int ___p1 = atomic_read(v); smp_rmb(); ___p1; })
#endif
#ifndef atomic_set_release
#define atomic_set_release(v, i) ({ smp_wmb(); atomic_set(v, i); })
#endif
#elif LINUX_VERSION_CODE < KERNEL_VERSION(4, 3, 0)
#include <linux/atomic.h>
#ifndef atomic_read_acquire
#define atomic_read_acquire(v) smp_load_acquire(&(v)->counter)
#endif
#ifndef atomic_set_release
#define atomic_set_release(v, i) smp_store_release(&(v)->counter, (i))
#endif
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 17, 0)
static inline void le32_to_cpu_array(u32 *buf, unsigned int words)
{
	while (words--) {
		__le32_to_cpus(buf);
		buf++;
	}
}
static inline void cpu_to_le32_array(u32 *buf, unsigned int words)
{
	while (words--) {
		__cpu_to_le32s(buf);
		buf++;
	}
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
#include <crypto/algapi.h>
static inline void crypto_xor_cpy(u8 *dst, const u8 *src1, const u8 *src2,
				  unsigned int size)
{
	if (IS_ENABLED(CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS) &&
	    __builtin_constant_p(size) &&
	    (size % sizeof(unsigned long)) == 0) {
		unsigned long *d = (unsigned long *)dst;
		unsigned long *s1 = (unsigned long *)src1;
		unsigned long *s2 = (unsigned long *)src2;

		while (size > 0) {
			*d++ = *s1++ ^ *s2++;
			size -= sizeof(unsigned long);
		}
	} else {
		if (unlikely(dst != src1))
			memmove(dst, src1, size);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0)
		crypto_xor(dst, src2, size);
#else
		__crypto_xor(dst, src2, size);
#endif
	}
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 17, 0)
#define read_cpuid_part() read_cpuid_part_number()
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 17, 0)
#define hlist_add_behind(a, b) hlist_add_after(b, a)
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 0, 0)
#define totalram_pages() totalram_pages
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 18, 0)
struct __kernel_timespec {
	int64_t tv_sec, tv_nsec;
};
#elif LINUX_VERSION_CODE < KERNEL_VERSION(5, 1, 0)
#include <linux/time64.h>
#ifdef __kernel_timespec
#undef __kernel_timespec
struct __kernel_timespec {
	int64_t tv_sec, tv_nsec;
};
#endif
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 12, 0)
#include <linux/kernel.h>
#ifndef ALIGN_DOWN
#define ALIGN_DOWN(x, a) __ALIGN_KERNEL((x) - ((a) - 1), (a))
#endif
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 1, 0)
#include <linux/skbuff.h>
#define skb_probe_transport_header(a) skb_probe_transport_header(a, 0)
#endif


#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 1, 0)
/* Note that all intentional uses of the non-_bh variety need to explicitly
 * undef these, conditionalized on COMPAT_CANNOT_DEPRECIATE_BH_RCU.
 */
#include <linux/rcupdate.h>
static __always_inline void old_synchronize_rcu(void)
{
	synchronize_rcu();
}
static __always_inline void old_call_rcu(void *a, void *b)
{
	call_rcu(a, b);
}
static __always_inline void old_rcu_barrier(void)
{
	rcu_barrier();
}
#ifdef synchronize_rcu
#undef synchronize_rcu
#endif
#ifdef call_rcu
#undef call_rcu
#endif
#ifdef rcu_barrier
#undef rcu_barrier
#endif
#define synchronize_rcu synchronize_rcu_bh
#define call_rcu call_rcu_bh
#define rcu_barrier rcu_barrier_bh
#define COMPAT_CANNOT_DEPRECIATE_BH_RCU
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 19, 10)
static inline void skb_mark_not_on_list(struct sk_buff *skb)
{
	skb->next = NULL;
}
#endif

/* https://github.com/ClangBuiltLinux/linux/issues/7 */
#if defined( __clang__) && (!defined(CONFIG_CLANG_VERSION) || CONFIG_CLANG_VERSION < 80000)
#include <linux/bug.h>
#undef BUILD_BUG_ON
#define BUILD_BUG_ON(x)
#endif

/* https://lkml.kernel.org/r/20170624021727.17835-1-Jason@zx2c4.com */
#if IS_ENABLED(CONFIG_NF_CONNTRACK)
#include <linux/ip.h>
#include <linux/icmpv6.h>
#include <net/ipv6.h>
#include <net/icmp.h>
#include <net/netfilter/nf_conntrack.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 1, 0)
#include <net/netfilter/nf_nat_core.h>
#endif
static inline void new_icmp_send(struct sk_buff *skb_in, int type, int code, __be32 info)
{
	enum ip_conntrack_info ctinfo;
	struct nf_conn *ct = nf_ct_get(skb_in, &ctinfo);
	if (skb_network_header(skb_in) < skb_in->head || (skb_network_header(skb_in) + sizeof(struct iphdr)) > skb_tail_pointer(skb_in))
		return;
	if (ct)
		ip_hdr(skb_in)->saddr = ct->tuplehash[0].tuple.src.u3.ip;
	icmp_send(skb_in, type, code, info);
}
static inline void new_icmpv6_send(struct sk_buff *skb, u8 type, u8 code, __u32 info)
{
	enum ip_conntrack_info ctinfo;
	struct nf_conn *ct = nf_ct_get(skb, &ctinfo);
	if (skb_network_header(skb) < skb->head || (skb_network_header(skb) + sizeof(struct ipv6hdr)) > skb_tail_pointer(skb))
		return;
	if (ct)
		ipv6_hdr(skb)->saddr = ct->tuplehash[0].tuple.src.u3.in6;
	icmpv6_send(skb, type, code, info);
}
#define icmp_send(a,b,c,d) new_icmp_send(a,b,c,d)
#define icmpv6_send(a,b,c,d) new_icmpv6_send(a,b,c,d)
#endif

/* PaX compatibility */
#ifdef CONSTIFY_PLUGIN
#include <linux/cache.h>
#undef __read_mostly
#define __read_mostly
#endif
#if (defined(RAP_PLUGIN) || defined(CONFIG_CFI_CLANG)) && LINUX_VERSION_CODE < KERNEL_VERSION(4, 15, 0)
#include <linux/timer.h>
#define wg_expired_retransmit_handshake(a) wg_expired_retransmit_handshake(unsigned long timer)
#define wg_expired_send_keepalive(a) wg_expired_send_keepalive(unsigned long timer)
#define wg_expired_new_handshake(a) wg_expired_new_handshake(unsigned long timer)
#define wg_expired_zero_key_material(a) wg_expired_zero_key_material(unsigned long timer)
#define wg_expired_send_persistent_keepalive(a) wg_expired_send_persistent_keepalive(unsigned long timer)
#undef timer_setup
#define timer_setup(a, b, c) setup_timer(a, ((void (*)(unsigned long))b), ((unsigned long)a))
#undef from_timer
#define from_timer(var, callback_timer, timer_fieldname) container_of((struct timer_list *)callback_timer, typeof(*var), timer_fieldname)
#endif

#endif /* _WG_COMPAT_H */
