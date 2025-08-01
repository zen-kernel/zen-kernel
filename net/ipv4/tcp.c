// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * INET		An implementation of the TCP/IP protocol suite for the LINUX
 *		operating system.  INET is implemented using the  BSD Socket
 *		interface as the means of communication with the user level.
 *
 *		Implementation of the Transmission Control Protocol(TCP).
 *
 * Authors:	Ross Biro
 *		Fred N. van Kempen, <waltje@uWalt.NL.Mugnet.ORG>
 *		Mark Evans, <evansmp@uhura.aston.ac.uk>
 *		Corey Minyard <wf-rch!minyard@relay.EU.net>
 *		Florian La Roche, <flla@stud.uni-sb.de>
 *		Charles Hedrick, <hedrick@klinzhai.rutgers.edu>
 *		Linus Torvalds, <torvalds@cs.helsinki.fi>
 *		Alan Cox, <gw4pts@gw4pts.ampr.org>
 *		Matthew Dillon, <dillon@apollo.west.oic.com>
 *		Arnt Gulbrandsen, <agulbra@nvg.unit.no>
 *		Jorge Cwik, <jorge@laser.satlink.net>
 *
 * Fixes:
 *		Alan Cox	:	Numerous verify_area() calls
 *		Alan Cox	:	Set the ACK bit on a reset
 *		Alan Cox	:	Stopped it crashing if it closed while
 *					sk->inuse=1 and was trying to connect
 *					(tcp_err()).
 *		Alan Cox	:	All icmp error handling was broken
 *					pointers passed where wrong and the
 *					socket was looked up backwards. Nobody
 *					tested any icmp error code obviously.
 *		Alan Cox	:	tcp_err() now handled properly. It
 *					wakes people on errors. poll
 *					behaves and the icmp error race
 *					has gone by moving it into sock.c
 *		Alan Cox	:	tcp_send_reset() fixed to work for
 *					everything not just packets for
 *					unknown sockets.
 *		Alan Cox	:	tcp option processing.
 *		Alan Cox	:	Reset tweaked (still not 100%) [Had
 *					syn rule wrong]
 *		Herp Rosmanith  :	More reset fixes
 *		Alan Cox	:	No longer acks invalid rst frames.
 *					Acking any kind of RST is right out.
 *		Alan Cox	:	Sets an ignore me flag on an rst
 *					receive otherwise odd bits of prattle
 *					escape still
 *		Alan Cox	:	Fixed another acking RST frame bug.
 *					Should stop LAN workplace lockups.
 *		Alan Cox	: 	Some tidyups using the new skb list
 *					facilities
 *		Alan Cox	:	sk->keepopen now seems to work
 *		Alan Cox	:	Pulls options out correctly on accepts
 *		Alan Cox	:	Fixed assorted sk->rqueue->next errors
 *		Alan Cox	:	PSH doesn't end a TCP read. Switched a
 *					bit to skb ops.
 *		Alan Cox	:	Tidied tcp_data to avoid a potential
 *					nasty.
 *		Alan Cox	:	Added some better commenting, as the
 *					tcp is hard to follow
 *		Alan Cox	:	Removed incorrect check for 20 * psh
 *	Michael O'Reilly	:	ack < copied bug fix.
 *	Johannes Stille		:	Misc tcp fixes (not all in yet).
 *		Alan Cox	:	FIN with no memory -> CRASH
 *		Alan Cox	:	Added socket option proto entries.
 *					Also added awareness of them to accept.
 *		Alan Cox	:	Added TCP options (SOL_TCP)
 *		Alan Cox	:	Switched wakeup calls to callbacks,
 *					so the kernel can layer network
 *					sockets.
 *		Alan Cox	:	Use ip_tos/ip_ttl settings.
 *		Alan Cox	:	Handle FIN (more) properly (we hope).
 *		Alan Cox	:	RST frames sent on unsynchronised
 *					state ack error.
 *		Alan Cox	:	Put in missing check for SYN bit.
 *		Alan Cox	:	Added tcp_select_window() aka NET2E
 *					window non shrink trick.
 *		Alan Cox	:	Added a couple of small NET2E timer
 *					fixes
 *		Charles Hedrick :	TCP fixes
 *		Toomas Tamm	:	TCP window fixes
 *		Alan Cox	:	Small URG fix to rlogin ^C ack fight
 *		Charles Hedrick	:	Rewrote most of it to actually work
 *		Linus		:	Rewrote tcp_read() and URG handling
 *					completely
 *		Gerhard Koerting:	Fixed some missing timer handling
 *		Matthew Dillon  :	Reworked TCP machine states as per RFC
 *		Gerhard Koerting:	PC/TCP workarounds
 *		Adam Caldwell	:	Assorted timer/timing errors
 *		Matthew Dillon	:	Fixed another RST bug
 *		Alan Cox	:	Move to kernel side addressing changes.
 *		Alan Cox	:	Beginning work on TCP fastpathing
 *					(not yet usable)
 *		Arnt Gulbrandsen:	Turbocharged tcp_check() routine.
 *		Alan Cox	:	TCP fast path debugging
 *		Alan Cox	:	Window clamping
 *		Michael Riepe	:	Bug in tcp_check()
 *		Matt Dillon	:	More TCP improvements and RST bug fixes
 *		Matt Dillon	:	Yet more small nasties remove from the
 *					TCP code (Be very nice to this man if
 *					tcp finally works 100%) 8)
 *		Alan Cox	:	BSD accept semantics.
 *		Alan Cox	:	Reset on closedown bug.
 *	Peter De Schrijver	:	ENOTCONN check missing in tcp_sendto().
 *		Michael Pall	:	Handle poll() after URG properly in
 *					all cases.
 *		Michael Pall	:	Undo the last fix in tcp_read_urg()
 *					(multi URG PUSH broke rlogin).
 *		Michael Pall	:	Fix the multi URG PUSH problem in
 *					tcp_readable(), poll() after URG
 *					works now.
 *		Michael Pall	:	recv(...,MSG_OOB) never blocks in the
 *					BSD api.
 *		Alan Cox	:	Changed the semantics of sk->socket to
 *					fix a race and a signal problem with
 *					accept() and async I/O.
 *		Alan Cox	:	Relaxed the rules on tcp_sendto().
 *		Yury Shevchuk	:	Really fixed accept() blocking problem.
 *		Craig I. Hagan  :	Allow for BSD compatible TIME_WAIT for
 *					clients/servers which listen in on
 *					fixed ports.
 *		Alan Cox	:	Cleaned the above up and shrank it to
 *					a sensible code size.
 *		Alan Cox	:	Self connect lockup fix.
 *		Alan Cox	:	No connect to multicast.
 *		Ross Biro	:	Close unaccepted children on master
 *					socket close.
 *		Alan Cox	:	Reset tracing code.
 *		Alan Cox	:	Spurious resets on shutdown.
 *		Alan Cox	:	Giant 15 minute/60 second timer error
 *		Alan Cox	:	Small whoops in polling before an
 *					accept.
 *		Alan Cox	:	Kept the state trace facility since
 *					it's handy for debugging.
 *		Alan Cox	:	More reset handler fixes.
 *		Alan Cox	:	Started rewriting the code based on
 *					the RFC's for other useful protocol
 *					references see: Comer, KA9Q NOS, and
 *					for a reference on the difference
 *					between specifications and how BSD
 *					works see the 4.4lite source.
 *		A.N.Kuznetsov	:	Don't time wait on completion of tidy
 *					close.
 *		Linus Torvalds	:	Fin/Shutdown & copied_seq changes.
 *		Linus Torvalds	:	Fixed BSD port reuse to work first syn
 *		Alan Cox	:	Reimplemented timers as per the RFC
 *					and using multiple timers for sanity.
 *		Alan Cox	:	Small bug fixes, and a lot of new
 *					comments.
 *		Alan Cox	:	Fixed dual reader crash by locking
 *					the buffers (much like datagram.c)
 *		Alan Cox	:	Fixed stuck sockets in probe. A probe
 *					now gets fed up of retrying without
 *					(even a no space) answer.
 *		Alan Cox	:	Extracted closing code better
 *		Alan Cox	:	Fixed the closing state machine to
 *					resemble the RFC.
 *		Alan Cox	:	More 'per spec' fixes.
 *		Jorge Cwik	:	Even faster checksumming.
 *		Alan Cox	:	tcp_data() doesn't ack illegal PSH
 *					only frames. At least one pc tcp stack
 *					generates them.
 *		Alan Cox	:	Cache last socket.
 *		Alan Cox	:	Per route irtt.
 *		Matt Day	:	poll()->select() match BSD precisely on error
 *		Alan Cox	:	New buffers
 *		Marc Tamsky	:	Various sk->prot->retransmits and
 *					sk->retransmits misupdating fixed.
 *					Fixed tcp_write_timeout: stuck close,
 *					and TCP syn retries gets used now.
 *		Mark Yarvis	:	In tcp_read_wakeup(), don't send an
 *					ack if state is TCP_CLOSED.
 *		Alan Cox	:	Look up device on a retransmit - routes may
 *					change. Doesn't yet cope with MSS shrink right
 *					but it's a start!
 *		Marc Tamsky	:	Closing in closing fixes.
 *		Mike Shaver	:	RFC1122 verifications.
 *		Alan Cox	:	rcv_saddr errors.
 *		Alan Cox	:	Block double connect().
 *		Alan Cox	:	Small hooks for enSKIP.
 *		Alexey Kuznetsov:	Path MTU discovery.
 *		Alan Cox	:	Support soft errors.
 *		Alan Cox	:	Fix MTU discovery pathological case
 *					when the remote claims no mtu!
 *		Marc Tamsky	:	TCP_CLOSE fix.
 *		Colin (G3TNE)	:	Send a reset on syn ack replies in
 *					window but wrong (fixes NT lpd problems)
 *		Pedro Roque	:	Better TCP window handling, delayed ack.
 *		Joerg Reuter	:	No modification of locked buffers in
 *					tcp_do_retransmit()
 *		Eric Schenk	:	Changed receiver side silly window
 *					avoidance algorithm to BSD style
 *					algorithm. This doubles throughput
 *					against machines running Solaris,
 *					and seems to result in general
 *					improvement.
 *	Stefan Magdalinski	:	adjusted tcp_readable() to fix FIONREAD
 *	Willy Konynenberg	:	Transparent proxying support.
 *	Mike McLagan		:	Routing by source
 *		Keith Owens	:	Do proper merging with partial SKB's in
 *					tcp_do_sendmsg to avoid burstiness.
 *		Eric Schenk	:	Fix fast close down bug with
 *					shutdown() followed by close().
 *		Andi Kleen 	:	Make poll agree with SIGIO
 *	Salvatore Sanfilippo	:	Support SO_LINGER with linger == 1 and
 *					lingertime == 0 (RFC 793 ABORT Call)
 *	Hirokazu Takahashi	:	Use copy_from_user() instead of
 *					csum_and_copy_from_user() if possible.
 *
 * Description of States:
 *
 *	TCP_SYN_SENT		sent a connection request, waiting for ack
 *
 *	TCP_SYN_RECV		received a connection request, sent ack,
 *				waiting for final ack in three-way handshake.
 *
 *	TCP_ESTABLISHED		connection established
 *
 *	TCP_FIN_WAIT1		our side has shutdown, waiting to complete
 *				transmission of remaining buffered data
 *
 *	TCP_FIN_WAIT2		all buffered data sent, waiting for remote
 *				to shutdown
 *
 *	TCP_CLOSING		both sides have shutdown but we still have
 *				data we have to finish sending
 *
 *	TCP_TIME_WAIT		timeout to catch resent junk before entering
 *				closed, can only be entered from FIN_WAIT2
 *				or CLOSING.  Required because the other end
 *				may not have gotten our last ACK causing it
 *				to retransmit the data packet (which we ignore)
 *
 *	TCP_CLOSE_WAIT		remote side has shutdown and is waiting for
 *				us to finish writing our data and to shutdown
 *				(we have to close() to move on to LAST_ACK)
 *
 *	TCP_LAST_ACK		out side has shutdown after remote has
 *				shutdown.  There may still be data in our
 *				buffer that we have to finish sending
 *
 *	TCP_CLOSE		socket is finished
 */

#define pr_fmt(fmt) "TCP: " fmt

#include <crypto/hash.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/fcntl.h>
#include <linux/poll.h>
#include <linux/inet_diag.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/skbuff.h>
#include <linux/scatterlist.h>
#include <linux/splice.h>
#include <linux/net.h>
#include <linux/socket.h>
#include <linux/random.h>
#include <linux/memblock.h>
#include <linux/highmem.h>
#include <linux/cache.h>
#include <linux/err.h>
#include <linux/time.h>
#include <linux/slab.h>
#include <linux/errqueue.h>
#include <linux/static_key.h>
#include <linux/btf.h>

#include <net/icmp.h>
#include <net/inet_common.h>
#include <net/tcp.h>
#include <net/mptcp.h>
#include <net/proto_memory.h>
#include <net/xfrm.h>
#include <net/ip.h>
#include <net/sock.h>
#include <net/rstreason.h>

#include <linux/uaccess.h>
#include <asm/ioctls.h>
#include <net/busy_poll.h>
#include <net/hotdata.h>
#include <trace/events/tcp.h>
#include <net/rps.h>

#include "../core/devmem.h"

/* Track pending CMSGs. */
enum {
	TCP_CMSG_INQ = 1,
	TCP_CMSG_TS = 2
};

DEFINE_PER_CPU(unsigned int, tcp_orphan_count);
EXPORT_PER_CPU_SYMBOL_GPL(tcp_orphan_count);

DEFINE_PER_CPU(u32, tcp_tw_isn);
EXPORT_PER_CPU_SYMBOL_GPL(tcp_tw_isn);

long sysctl_tcp_mem[3] __read_mostly;
EXPORT_IPV6_MOD(sysctl_tcp_mem);

atomic_long_t tcp_memory_allocated ____cacheline_aligned_in_smp;	/* Current allocated memory. */
EXPORT_IPV6_MOD(tcp_memory_allocated);
DEFINE_PER_CPU(int, tcp_memory_per_cpu_fw_alloc);
EXPORT_PER_CPU_SYMBOL_GPL(tcp_memory_per_cpu_fw_alloc);

#if IS_ENABLED(CONFIG_SMC)
DEFINE_STATIC_KEY_FALSE(tcp_have_smc);
EXPORT_SYMBOL(tcp_have_smc);
#endif

/*
 * Current number of TCP sockets.
 */
struct percpu_counter tcp_sockets_allocated ____cacheline_aligned_in_smp;
EXPORT_IPV6_MOD(tcp_sockets_allocated);

/*
 * TCP splice context
 */
struct tcp_splice_state {
	struct pipe_inode_info *pipe;
	size_t len;
	unsigned int flags;
};

/*
 * Pressure flag: try to collapse.
 * Technical note: it is used by multiple contexts non atomically.
 * All the __sk_mem_schedule() is of this nature: accounting
 * is strict, actions are advisory and have some latency.
 */
unsigned long tcp_memory_pressure __read_mostly;
EXPORT_SYMBOL_GPL(tcp_memory_pressure);

void tcp_enter_memory_pressure(struct sock *sk)
{
	unsigned long val;

	if (READ_ONCE(tcp_memory_pressure))
		return;
	val = jiffies;

	if (!val)
		val--;
	if (!cmpxchg(&tcp_memory_pressure, 0, val))
		NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPMEMORYPRESSURES);
}
EXPORT_IPV6_MOD_GPL(tcp_enter_memory_pressure);

void tcp_leave_memory_pressure(struct sock *sk)
{
	unsigned long val;

	if (!READ_ONCE(tcp_memory_pressure))
		return;
	val = xchg(&tcp_memory_pressure, 0);
	if (val)
		NET_ADD_STATS(sock_net(sk), LINUX_MIB_TCPMEMORYPRESSURESCHRONO,
			      jiffies_to_msecs(jiffies - val));
}
EXPORT_IPV6_MOD_GPL(tcp_leave_memory_pressure);

/* Convert seconds to retransmits based on initial and max timeout */
static u8 secs_to_retrans(int seconds, int timeout, int rto_max)
{
	u8 res = 0;

	if (seconds > 0) {
		int period = timeout;

		res = 1;
		while (seconds > period && res < 255) {
			res++;
			timeout <<= 1;
			if (timeout > rto_max)
				timeout = rto_max;
			period += timeout;
		}
	}
	return res;
}

/* Convert retransmits to seconds based on initial and max timeout */
static int retrans_to_secs(u8 retrans, int timeout, int rto_max)
{
	int period = 0;

	if (retrans > 0) {
		period = timeout;
		while (--retrans) {
			timeout <<= 1;
			if (timeout > rto_max)
				timeout = rto_max;
			period += timeout;
		}
	}
	return period;
}

static u64 tcp_compute_delivery_rate(const struct tcp_sock *tp)
{
	u32 rate = READ_ONCE(tp->rate_delivered);
	u32 intv = READ_ONCE(tp->rate_interval_us);
	u64 rate64 = 0;

	if (rate && intv) {
		rate64 = (u64)rate * tp->mss_cache * USEC_PER_SEC;
		do_div(rate64, intv);
	}
	return rate64;
}

/* Address-family independent initialization for a tcp_sock.
 *
 * NOTE: A lot of things set to zero explicitly by call to
 *       sk_alloc() so need not be done here.
 */
void tcp_init_sock(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	int rto_min_us, rto_max_ms;

	tp->out_of_order_queue = RB_ROOT;
	sk->tcp_rtx_queue = RB_ROOT;
	tcp_init_xmit_timers(sk);
	INIT_LIST_HEAD(&tp->tsq_node);
	INIT_LIST_HEAD(&tp->tsorted_sent_queue);

	icsk->icsk_rto = TCP_TIMEOUT_INIT;

	rto_max_ms = READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_rto_max_ms);
	icsk->icsk_rto_max = msecs_to_jiffies(rto_max_ms);

	rto_min_us = READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_rto_min_us);
	icsk->icsk_rto_min = usecs_to_jiffies(rto_min_us);
	icsk->icsk_delack_max = TCP_DELACK_MAX;
	tp->mdev_us = jiffies_to_usecs(TCP_TIMEOUT_INIT);
	minmax_reset(&tp->rtt_min, tcp_jiffies32, ~0U);

	/* So many TCP implementations out there (incorrectly) count the
	 * initial SYN frame in their delayed-ACK and congestion control
	 * algorithms that we must have the following bandaid to talk
	 * efficiently to them.  -DaveM
	 */
	tcp_snd_cwnd_set(tp, TCP_INIT_CWND);

	/* There's a bubble in the pipe until at least the first ACK. */
	tp->app_limited = ~0U;
	tp->rate_app_limited = 1;

	/* See draft-stevens-tcpca-spec-01 for discussion of the
	 * initialization of these values.
	 */
	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	tp->snd_cwnd_clamp = ~0;
	tp->mss_cache = TCP_MSS_DEFAULT;

	tp->reordering = READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_reordering);
	tcp_assign_congestion_control(sk);

	tp->tsoffset = 0;
	tp->rack.reo_wnd_steps = 1;

	sk->sk_write_space = sk_stream_write_space;
	sock_set_flag(sk, SOCK_USE_WRITE_QUEUE);

	icsk->icsk_sync_mss = tcp_sync_mss;

	WRITE_ONCE(sk->sk_sndbuf, READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_wmem[1]));
	WRITE_ONCE(sk->sk_rcvbuf, READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_rmem[1]));
	tcp_scaling_ratio_init(sk);

	set_bit(SOCK_SUPPORT_ZC, &sk->sk_socket->flags);
	sk_sockets_allocated_inc(sk);
	xa_init_flags(&sk->sk_user_frags, XA_FLAGS_ALLOC1);
}
EXPORT_IPV6_MOD(tcp_init_sock);

static void tcp_tx_timestamp(struct sock *sk, struct sockcm_cookie *sockc)
{
	struct sk_buff *skb = tcp_write_queue_tail(sk);
	u32 tsflags = sockc->tsflags;

	if (tsflags && skb) {
		struct skb_shared_info *shinfo = skb_shinfo(skb);
		struct tcp_skb_cb *tcb = TCP_SKB_CB(skb);

		sock_tx_timestamp(sk, sockc, &shinfo->tx_flags);
		if (tsflags & SOF_TIMESTAMPING_TX_ACK)
			tcb->txstamp_ack |= TSTAMP_ACK_SK;
		if (tsflags & SOF_TIMESTAMPING_TX_RECORD_MASK)
			shinfo->tskey = TCP_SKB_CB(skb)->seq + skb->len - 1;
	}

	if (cgroup_bpf_enabled(CGROUP_SOCK_OPS) &&
	    SK_BPF_CB_FLAG_TEST(sk, SK_BPF_CB_TX_TIMESTAMPING) && skb)
		bpf_skops_tx_timestamping(sk, skb, BPF_SOCK_OPS_TSTAMP_SENDMSG_CB);
}

static bool tcp_stream_is_readable(struct sock *sk, int target)
{
	if (tcp_epollin_ready(sk, target))
		return true;
	return sk_is_readable(sk);
}

/*
 *	Wait for a TCP event.
 *
 *	Note that we don't need to lock the socket, as the upper poll layers
 *	take care of normal races (between the test and the event) and we don't
 *	go look at any of the socket buffers directly.
 */
__poll_t tcp_poll(struct file *file, struct socket *sock, poll_table *wait)
{
	__poll_t mask;
	struct sock *sk = sock->sk;
	const struct tcp_sock *tp = tcp_sk(sk);
	u8 shutdown;
	int state;

	sock_poll_wait(file, sock, wait);

	state = inet_sk_state_load(sk);
	if (state == TCP_LISTEN)
		return inet_csk_listen_poll(sk);

	/* Socket is not locked. We are protected from async events
	 * by poll logic and correct handling of state changes
	 * made by other threads is impossible in any case.
	 */

	mask = 0;

	/*
	 * EPOLLHUP is certainly not done right. But poll() doesn't
	 * have a notion of HUP in just one direction, and for a
	 * socket the read side is more interesting.
	 *
	 * Some poll() documentation says that EPOLLHUP is incompatible
	 * with the EPOLLOUT/POLLWR flags, so somebody should check this
	 * all. But careful, it tends to be safer to return too many
	 * bits than too few, and you can easily break real applications
	 * if you don't tell them that something has hung up!
	 *
	 * Check-me.
	 *
	 * Check number 1. EPOLLHUP is _UNMASKABLE_ event (see UNIX98 and
	 * our fs/select.c). It means that after we received EOF,
	 * poll always returns immediately, making impossible poll() on write()
	 * in state CLOSE_WAIT. One solution is evident --- to set EPOLLHUP
	 * if and only if shutdown has been made in both directions.
	 * Actually, it is interesting to look how Solaris and DUX
	 * solve this dilemma. I would prefer, if EPOLLHUP were maskable,
	 * then we could set it on SND_SHUTDOWN. BTW examples given
	 * in Stevens' books assume exactly this behaviour, it explains
	 * why EPOLLHUP is incompatible with EPOLLOUT.	--ANK
	 *
	 * NOTE. Check for TCP_CLOSE is added. The goal is to prevent
	 * blocking on fresh not-connected or disconnected socket. --ANK
	 */
	shutdown = READ_ONCE(sk->sk_shutdown);
	if (shutdown == SHUTDOWN_MASK || state == TCP_CLOSE)
		mask |= EPOLLHUP;
	if (shutdown & RCV_SHUTDOWN)
		mask |= EPOLLIN | EPOLLRDNORM | EPOLLRDHUP;

	/* Connected or passive Fast Open socket? */
	if (state != TCP_SYN_SENT &&
	    (state != TCP_SYN_RECV || rcu_access_pointer(tp->fastopen_rsk))) {
		int target = sock_rcvlowat(sk, 0, INT_MAX);
		u16 urg_data = READ_ONCE(tp->urg_data);

		if (unlikely(urg_data) &&
		    READ_ONCE(tp->urg_seq) == READ_ONCE(tp->copied_seq) &&
		    !sock_flag(sk, SOCK_URGINLINE))
			target++;

		if (tcp_stream_is_readable(sk, target))
			mask |= EPOLLIN | EPOLLRDNORM;

		if (!(shutdown & SEND_SHUTDOWN)) {
			if (__sk_stream_is_writeable(sk, 1)) {
				mask |= EPOLLOUT | EPOLLWRNORM;
			} else {  /* send SIGIO later */
				sk_set_bit(SOCKWQ_ASYNC_NOSPACE, sk);
				set_bit(SOCK_NOSPACE, &sk->sk_socket->flags);

				/* Race breaker. If space is freed after
				 * wspace test but before the flags are set,
				 * IO signal will be lost. Memory barrier
				 * pairs with the input side.
				 */
				smp_mb__after_atomic();
				if (__sk_stream_is_writeable(sk, 1))
					mask |= EPOLLOUT | EPOLLWRNORM;
			}
		} else
			mask |= EPOLLOUT | EPOLLWRNORM;

		if (urg_data & TCP_URG_VALID)
			mask |= EPOLLPRI;
	} else if (state == TCP_SYN_SENT &&
		   inet_test_bit(DEFER_CONNECT, sk)) {
		/* Active TCP fastopen socket with defer_connect
		 * Return EPOLLOUT so application can call write()
		 * in order for kernel to generate SYN+data
		 */
		mask |= EPOLLOUT | EPOLLWRNORM;
	}
	/* This barrier is coupled with smp_wmb() in tcp_done_with_error() */
	smp_rmb();
	if (READ_ONCE(sk->sk_err) ||
	    !skb_queue_empty_lockless(&sk->sk_error_queue))
		mask |= EPOLLERR;

	return mask;
}
EXPORT_SYMBOL(tcp_poll);

int tcp_ioctl(struct sock *sk, int cmd, int *karg)
{
	struct tcp_sock *tp = tcp_sk(sk);
	int answ;
	bool slow;

	switch (cmd) {
	case SIOCINQ:
		if (sk->sk_state == TCP_LISTEN)
			return -EINVAL;

		slow = lock_sock_fast(sk);
		answ = tcp_inq(sk);
		unlock_sock_fast(sk, slow);
		break;
	case SIOCATMARK:
		answ = READ_ONCE(tp->urg_data) &&
		       READ_ONCE(tp->urg_seq) == READ_ONCE(tp->copied_seq);
		break;
	case SIOCOUTQ:
		if (sk->sk_state == TCP_LISTEN)
			return -EINVAL;

		if ((1 << sk->sk_state) & (TCPF_SYN_SENT | TCPF_SYN_RECV))
			answ = 0;
		else
			answ = READ_ONCE(tp->write_seq) - tp->snd_una;
		break;
	case SIOCOUTQNSD:
		if (sk->sk_state == TCP_LISTEN)
			return -EINVAL;

		if ((1 << sk->sk_state) & (TCPF_SYN_SENT | TCPF_SYN_RECV))
			answ = 0;
		else
			answ = READ_ONCE(tp->write_seq) -
			       READ_ONCE(tp->snd_nxt);
		break;
	default:
		return -ENOIOCTLCMD;
	}

	*karg = answ;
	return 0;
}
EXPORT_IPV6_MOD(tcp_ioctl);

void tcp_mark_push(struct tcp_sock *tp, struct sk_buff *skb)
{
	TCP_SKB_CB(skb)->tcp_flags |= TCPHDR_PSH;
	tp->pushed_seq = tp->write_seq;
}

static inline bool forced_push(const struct tcp_sock *tp)
{
	return after(tp->write_seq, tp->pushed_seq + (tp->max_window >> 1));
}

void tcp_skb_entail(struct sock *sk, struct sk_buff *skb)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct tcp_skb_cb *tcb = TCP_SKB_CB(skb);

	tcb->seq     = tcb->end_seq = tp->write_seq;
	tcb->tcp_flags = TCPHDR_ACK;
	__skb_header_release(skb);
	tcp_add_write_queue_tail(sk, skb);
	sk_wmem_queued_add(sk, skb->truesize);
	sk_mem_charge(sk, skb->truesize);
	if (tp->nonagle & TCP_NAGLE_PUSH)
		tp->nonagle &= ~TCP_NAGLE_PUSH;

	tcp_slow_start_after_idle_check(sk);
}

static inline void tcp_mark_urg(struct tcp_sock *tp, int flags)
{
	if (flags & MSG_OOB)
		tp->snd_up = tp->write_seq;
}

/* If a not yet filled skb is pushed, do not send it if
 * we have data packets in Qdisc or NIC queues :
 * Because TX completion will happen shortly, it gives a chance
 * to coalesce future sendmsg() payload into this skb, without
 * need for a timer, and with no latency trade off.
 * As packets containing data payload have a bigger truesize
 * than pure acks (dataless) packets, the last checks prevent
 * autocorking if we only have an ACK in Qdisc/NIC queues,
 * or if TX completion was delayed after we processed ACK packet.
 */
static bool tcp_should_autocork(struct sock *sk, struct sk_buff *skb,
				int size_goal)
{
	return skb->len < size_goal &&
	       READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_autocorking) &&
	       !tcp_rtx_queue_empty(sk) &&
	       refcount_read(&sk->sk_wmem_alloc) > skb->truesize &&
	       tcp_skb_can_collapse_to(skb);
}

void tcp_push(struct sock *sk, int flags, int mss_now,
	      int nonagle, int size_goal)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct sk_buff *skb;

	skb = tcp_write_queue_tail(sk);
	if (!skb)
		return;
	if (!(flags & MSG_MORE) || forced_push(tp))
		tcp_mark_push(tp, skb);

	tcp_mark_urg(tp, flags);

	if (tcp_should_autocork(sk, skb, size_goal)) {

		/* avoid atomic op if TSQ_THROTTLED bit is already set */
		if (!test_bit(TSQ_THROTTLED, &sk->sk_tsq_flags)) {
			NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPAUTOCORKING);
			set_bit(TSQ_THROTTLED, &sk->sk_tsq_flags);
			smp_mb__after_atomic();
		}
		/* It is possible TX completion already happened
		 * before we set TSQ_THROTTLED.
		 */
		if (refcount_read(&sk->sk_wmem_alloc) > skb->truesize)
			return;
	}

	if (flags & MSG_MORE)
		nonagle = TCP_NAGLE_CORK;

	__tcp_push_pending_frames(sk, mss_now, nonagle);
}

static int tcp_splice_data_recv(read_descriptor_t *rd_desc, struct sk_buff *skb,
				unsigned int offset, size_t len)
{
	struct tcp_splice_state *tss = rd_desc->arg.data;
	int ret;

	ret = skb_splice_bits(skb, skb->sk, offset, tss->pipe,
			      min(rd_desc->count, len), tss->flags);
	if (ret > 0)
		rd_desc->count -= ret;
	return ret;
}

static int __tcp_splice_read(struct sock *sk, struct tcp_splice_state *tss)
{
	/* Store TCP splice context information in read_descriptor_t. */
	read_descriptor_t rd_desc = {
		.arg.data = tss,
		.count	  = tss->len,
	};

	return tcp_read_sock(sk, &rd_desc, tcp_splice_data_recv);
}

/**
 *  tcp_splice_read - splice data from TCP socket to a pipe
 * @sock:	socket to splice from
 * @ppos:	position (not valid)
 * @pipe:	pipe to splice to
 * @len:	number of bytes to splice
 * @flags:	splice modifier flags
 *
 * Description:
 *    Will read pages from given socket and fill them into a pipe.
 *
 **/
ssize_t tcp_splice_read(struct socket *sock, loff_t *ppos,
			struct pipe_inode_info *pipe, size_t len,
			unsigned int flags)
{
	struct sock *sk = sock->sk;
	struct tcp_splice_state tss = {
		.pipe = pipe,
		.len = len,
		.flags = flags,
	};
	long timeo;
	ssize_t spliced;
	int ret;

	sock_rps_record_flow(sk);
	/*
	 * We can't seek on a socket input
	 */
	if (unlikely(*ppos))
		return -ESPIPE;

	ret = spliced = 0;

	lock_sock(sk);

	timeo = sock_rcvtimeo(sk, sock->file->f_flags & O_NONBLOCK);
	while (tss.len) {
		ret = __tcp_splice_read(sk, &tss);
		if (ret < 0)
			break;
		else if (!ret) {
			if (spliced)
				break;
			if (sock_flag(sk, SOCK_DONE))
				break;
			if (sk->sk_err) {
				ret = sock_error(sk);
				break;
			}
			if (sk->sk_shutdown & RCV_SHUTDOWN)
				break;
			if (sk->sk_state == TCP_CLOSE) {
				/*
				 * This occurs when user tries to read
				 * from never connected socket.
				 */
				ret = -ENOTCONN;
				break;
			}
			if (!timeo) {
				ret = -EAGAIN;
				break;
			}
			/* if __tcp_splice_read() got nothing while we have
			 * an skb in receive queue, we do not want to loop.
			 * This might happen with URG data.
			 */
			if (!skb_queue_empty(&sk->sk_receive_queue))
				break;
			ret = sk_wait_data(sk, &timeo, NULL);
			if (ret < 0)
				break;
			if (signal_pending(current)) {
				ret = sock_intr_errno(timeo);
				break;
			}
			continue;
		}
		tss.len -= ret;
		spliced += ret;

		if (!tss.len || !timeo)
			break;
		release_sock(sk);
		lock_sock(sk);

		if (sk->sk_err || sk->sk_state == TCP_CLOSE ||
		    (sk->sk_shutdown & RCV_SHUTDOWN) ||
		    signal_pending(current))
			break;
	}

	release_sock(sk);

	if (spliced)
		return spliced;

	return ret;
}
EXPORT_IPV6_MOD(tcp_splice_read);

struct sk_buff *tcp_stream_alloc_skb(struct sock *sk, gfp_t gfp,
				     bool force_schedule)
{
	struct sk_buff *skb;

	skb = alloc_skb_fclone(MAX_TCP_HEADER, gfp);
	if (likely(skb)) {
		bool mem_scheduled;

		skb->truesize = SKB_TRUESIZE(skb_end_offset(skb));
		if (force_schedule) {
			mem_scheduled = true;
			sk_forced_mem_schedule(sk, skb->truesize);
		} else {
			mem_scheduled = sk_wmem_schedule(sk, skb->truesize);
		}
		if (likely(mem_scheduled)) {
			skb_reserve(skb, MAX_TCP_HEADER);
			skb->ip_summed = CHECKSUM_PARTIAL;
			INIT_LIST_HEAD(&skb->tcp_tsorted_anchor);
			return skb;
		}
		__kfree_skb(skb);
	} else {
		sk->sk_prot->enter_memory_pressure(sk);
		sk_stream_moderate_sndbuf(sk);
	}
	return NULL;
}

static unsigned int tcp_xmit_size_goal(struct sock *sk, u32 mss_now,
				       int large_allowed)
{
	struct tcp_sock *tp = tcp_sk(sk);
	u32 new_size_goal, size_goal;

	if (!large_allowed)
		return mss_now;

	/* Note : tcp_tso_autosize() will eventually split this later */
	new_size_goal = tcp_bound_to_half_wnd(tp, sk->sk_gso_max_size);

	/* We try hard to avoid divides here */
	size_goal = tp->gso_segs * mss_now;
	if (unlikely(new_size_goal < size_goal ||
		     new_size_goal >= size_goal + mss_now)) {
		tp->gso_segs = min_t(u16, new_size_goal / mss_now,
				     sk->sk_gso_max_segs);
		size_goal = tp->gso_segs * mss_now;
	}

	return max(size_goal, mss_now);
}

int tcp_send_mss(struct sock *sk, int *size_goal, int flags)
{
	int mss_now;

	mss_now = tcp_current_mss(sk);
	*size_goal = tcp_xmit_size_goal(sk, mss_now, !(flags & MSG_OOB));

	return mss_now;
}

/* In some cases, sendmsg() could have added an skb to the write queue,
 * but failed adding payload on it. We need to remove it to consume less
 * memory, but more importantly be able to generate EPOLLOUT for Edge Trigger
 * epoll() users. Another reason is that tcp_write_xmit() does not like
 * finding an empty skb in the write queue.
 */
void tcp_remove_empty_skb(struct sock *sk)
{
	struct sk_buff *skb = tcp_write_queue_tail(sk);

	if (skb && TCP_SKB_CB(skb)->seq == TCP_SKB_CB(skb)->end_seq) {
		tcp_unlink_write_queue(skb, sk);
		if (tcp_write_queue_empty(sk))
			tcp_chrono_stop(sk, TCP_CHRONO_BUSY);
		tcp_wmem_free_skb(sk, skb);
	}
}

/* skb changing from pure zc to mixed, must charge zc */
static int tcp_downgrade_zcopy_pure(struct sock *sk, struct sk_buff *skb)
{
	if (unlikely(skb_zcopy_pure(skb))) {
		u32 extra = skb->truesize -
			    SKB_TRUESIZE(skb_end_offset(skb));

		if (!sk_wmem_schedule(sk, extra))
			return -ENOMEM;

		sk_mem_charge(sk, extra);
		skb_shinfo(skb)->flags &= ~SKBFL_PURE_ZEROCOPY;
	}
	return 0;
}


int tcp_wmem_schedule(struct sock *sk, int copy)
{
	int left;

	if (likely(sk_wmem_schedule(sk, copy)))
		return copy;

	/* We could be in trouble if we have nothing queued.
	 * Use whatever is left in sk->sk_forward_alloc and tcp_wmem[0]
	 * to guarantee some progress.
	 */
	left = READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_wmem[0]) - sk->sk_wmem_queued;
	if (left > 0)
		sk_forced_mem_schedule(sk, min(left, copy));
	return min(copy, sk->sk_forward_alloc);
}

void tcp_free_fastopen_req(struct tcp_sock *tp)
{
	if (tp->fastopen_req) {
		kfree(tp->fastopen_req);
		tp->fastopen_req = NULL;
	}
}

int tcp_sendmsg_fastopen(struct sock *sk, struct msghdr *msg, int *copied,
			 size_t size, struct ubuf_info *uarg)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct inet_sock *inet = inet_sk(sk);
	struct sockaddr *uaddr = msg->msg_name;
	int err, flags;

	if (!(READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_fastopen) &
	      TFO_CLIENT_ENABLE) ||
	    (uaddr && msg->msg_namelen >= sizeof(uaddr->sa_family) &&
	     uaddr->sa_family == AF_UNSPEC))
		return -EOPNOTSUPP;
	if (tp->fastopen_req)
		return -EALREADY; /* Another Fast Open is in progress */

	tp->fastopen_req = kzalloc(sizeof(struct tcp_fastopen_request),
				   sk->sk_allocation);
	if (unlikely(!tp->fastopen_req))
		return -ENOBUFS;
	tp->fastopen_req->data = msg;
	tp->fastopen_req->size = size;
	tp->fastopen_req->uarg = uarg;

	if (inet_test_bit(DEFER_CONNECT, sk)) {
		err = tcp_connect(sk);
		/* Same failure procedure as in tcp_v4/6_connect */
		if (err) {
			tcp_set_state(sk, TCP_CLOSE);
			inet->inet_dport = 0;
			sk->sk_route_caps = 0;
		}
	}
	flags = (msg->msg_flags & MSG_DONTWAIT) ? O_NONBLOCK : 0;
	err = __inet_stream_connect(sk->sk_socket, uaddr,
				    msg->msg_namelen, flags, 1);
	/* fastopen_req could already be freed in __inet_stream_connect
	 * if the connection times out or gets rst
	 */
	if (tp->fastopen_req) {
		*copied = tp->fastopen_req->copied;
		tcp_free_fastopen_req(tp);
		inet_clear_bit(DEFER_CONNECT, sk);
	}
	return err;
}

int tcp_sendmsg_locked(struct sock *sk, struct msghdr *msg, size_t size)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct ubuf_info *uarg = NULL;
	struct sk_buff *skb;
	struct sockcm_cookie sockc;
	int flags, err, copied = 0;
	int mss_now = 0, size_goal, copied_syn = 0;
	int process_backlog = 0;
	int zc = 0;
	long timeo;

	flags = msg->msg_flags;

	if ((flags & MSG_ZEROCOPY) && size) {
		if (msg->msg_ubuf) {
			uarg = msg->msg_ubuf;
			if (sk->sk_route_caps & NETIF_F_SG)
				zc = MSG_ZEROCOPY;
		} else if (sock_flag(sk, SOCK_ZEROCOPY)) {
			skb = tcp_write_queue_tail(sk);
			uarg = msg_zerocopy_realloc(sk, size, skb_zcopy(skb));
			if (!uarg) {
				err = -ENOBUFS;
				goto out_err;
			}
			if (sk->sk_route_caps & NETIF_F_SG)
				zc = MSG_ZEROCOPY;
			else
				uarg_to_msgzc(uarg)->zerocopy = 0;
		}
	} else if (unlikely(msg->msg_flags & MSG_SPLICE_PAGES) && size) {
		if (sk->sk_route_caps & NETIF_F_SG)
			zc = MSG_SPLICE_PAGES;
	}

	if (unlikely(flags & MSG_FASTOPEN ||
		     inet_test_bit(DEFER_CONNECT, sk)) &&
	    !tp->repair) {
		err = tcp_sendmsg_fastopen(sk, msg, &copied_syn, size, uarg);
		if (err == -EINPROGRESS && copied_syn > 0)
			goto out;
		else if (err)
			goto out_err;
	}

	timeo = sock_sndtimeo(sk, flags & MSG_DONTWAIT);

	tcp_rate_check_app_limited(sk);  /* is sending application-limited? */

	/* Wait for a connection to finish. One exception is TCP Fast Open
	 * (passive side) where data is allowed to be sent before a connection
	 * is fully established.
	 */
	if (((1 << sk->sk_state) & ~(TCPF_ESTABLISHED | TCPF_CLOSE_WAIT)) &&
	    !tcp_passive_fastopen(sk)) {
		err = sk_stream_wait_connect(sk, &timeo);
		if (err != 0)
			goto do_error;
	}

	if (unlikely(tp->repair)) {
		if (tp->repair_queue == TCP_RECV_QUEUE) {
			copied = tcp_send_rcvq(sk, msg, size);
			goto out_nopush;
		}

		err = -EINVAL;
		if (tp->repair_queue == TCP_NO_QUEUE)
			goto out_err;

		/* 'common' sending to sendq */
	}

	sockc = (struct sockcm_cookie) { .tsflags = READ_ONCE(sk->sk_tsflags)};
	if (msg->msg_controllen) {
		err = sock_cmsg_send(sk, msg, &sockc);
		if (unlikely(err)) {
			err = -EINVAL;
			goto out_err;
		}
	}

	/* This should be in poll */
	sk_clear_bit(SOCKWQ_ASYNC_NOSPACE, sk);

	/* Ok commence sending. */
	copied = 0;

restart:
	mss_now = tcp_send_mss(sk, &size_goal, flags);

	err = -EPIPE;
	if (sk->sk_err || (sk->sk_shutdown & SEND_SHUTDOWN))
		goto do_error;

	while (msg_data_left(msg)) {
		int copy = 0;

		skb = tcp_write_queue_tail(sk);
		if (skb)
			copy = size_goal - skb->len;

		if (copy <= 0 || !tcp_skb_can_collapse_to(skb)) {
			bool first_skb;

new_segment:
			if (!sk_stream_memory_free(sk))
				goto wait_for_space;

			if (unlikely(process_backlog >= 16)) {
				process_backlog = 0;
				if (sk_flush_backlog(sk))
					goto restart;
			}
			first_skb = tcp_rtx_and_write_queues_empty(sk);
			skb = tcp_stream_alloc_skb(sk, sk->sk_allocation,
						   first_skb);
			if (!skb)
				goto wait_for_space;

			process_backlog++;

#ifdef CONFIG_SKB_DECRYPTED
			skb->decrypted = !!(flags & MSG_SENDPAGE_DECRYPTED);
#endif
			tcp_skb_entail(sk, skb);
			copy = size_goal;

			/* All packets are restored as if they have
			 * already been sent. skb_mstamp_ns isn't set to
			 * avoid wrong rtt estimation.
			 */
			if (tp->repair)
				TCP_SKB_CB(skb)->sacked |= TCPCB_REPAIRED;
		}

		/* Try to append data to the end of skb. */
		if (copy > msg_data_left(msg))
			copy = msg_data_left(msg);

		if (zc == 0) {
			bool merge = true;
			int i = skb_shinfo(skb)->nr_frags;
			struct page_frag *pfrag = sk_page_frag(sk);

			if (!sk_page_frag_refill(sk, pfrag))
				goto wait_for_space;

			if (!skb_can_coalesce(skb, i, pfrag->page,
					      pfrag->offset)) {
				if (i >= READ_ONCE(net_hotdata.sysctl_max_skb_frags)) {
					tcp_mark_push(tp, skb);
					goto new_segment;
				}
				merge = false;
			}

			copy = min_t(int, copy, pfrag->size - pfrag->offset);

			if (unlikely(skb_zcopy_pure(skb) || skb_zcopy_managed(skb))) {
				if (tcp_downgrade_zcopy_pure(sk, skb))
					goto wait_for_space;
				skb_zcopy_downgrade_managed(skb);
			}

			copy = tcp_wmem_schedule(sk, copy);
			if (!copy)
				goto wait_for_space;

			err = skb_copy_to_page_nocache(sk, &msg->msg_iter, skb,
						       pfrag->page,
						       pfrag->offset,
						       copy);
			if (err)
				goto do_error;

			/* Update the skb. */
			if (merge) {
				skb_frag_size_add(&skb_shinfo(skb)->frags[i - 1], copy);
			} else {
				skb_fill_page_desc(skb, i, pfrag->page,
						   pfrag->offset, copy);
				page_ref_inc(pfrag->page);
			}
			pfrag->offset += copy;
		} else if (zc == MSG_ZEROCOPY)  {
			/* First append to a fragless skb builds initial
			 * pure zerocopy skb
			 */
			if (!skb->len)
				skb_shinfo(skb)->flags |= SKBFL_PURE_ZEROCOPY;

			if (!skb_zcopy_pure(skb)) {
				copy = tcp_wmem_schedule(sk, copy);
				if (!copy)
					goto wait_for_space;
			}

			err = skb_zerocopy_iter_stream(sk, skb, msg, copy, uarg);
			if (err == -EMSGSIZE || err == -EEXIST) {
				tcp_mark_push(tp, skb);
				goto new_segment;
			}
			if (err < 0)
				goto do_error;
			copy = err;
		} else if (zc == MSG_SPLICE_PAGES) {
			/* Splice in data if we can; copy if we can't. */
			if (tcp_downgrade_zcopy_pure(sk, skb))
				goto wait_for_space;
			copy = tcp_wmem_schedule(sk, copy);
			if (!copy)
				goto wait_for_space;

			err = skb_splice_from_iter(skb, &msg->msg_iter, copy,
						   sk->sk_allocation);
			if (err < 0) {
				if (err == -EMSGSIZE) {
					tcp_mark_push(tp, skb);
					goto new_segment;
				}
				goto do_error;
			}
			copy = err;

			if (!(flags & MSG_NO_SHARED_FRAGS))
				skb_shinfo(skb)->flags |= SKBFL_SHARED_FRAG;

			sk_wmem_queued_add(sk, copy);
			sk_mem_charge(sk, copy);
		}

		if (!copied)
			TCP_SKB_CB(skb)->tcp_flags &= ~TCPHDR_PSH;

		WRITE_ONCE(tp->write_seq, tp->write_seq + copy);
		TCP_SKB_CB(skb)->end_seq += copy;
		tcp_skb_pcount_set(skb, 0);

		copied += copy;
		if (!msg_data_left(msg)) {
			if (unlikely(flags & MSG_EOR))
				TCP_SKB_CB(skb)->eor = 1;
			goto out;
		}

		if (skb->len < size_goal || (flags & MSG_OOB) || unlikely(tp->repair))
			continue;

		if (forced_push(tp)) {
			tcp_mark_push(tp, skb);
			__tcp_push_pending_frames(sk, mss_now, TCP_NAGLE_PUSH);
		} else if (skb == tcp_send_head(sk))
			tcp_push_one(sk, mss_now);
		continue;

wait_for_space:
		set_bit(SOCK_NOSPACE, &sk->sk_socket->flags);
		tcp_remove_empty_skb(sk);
		if (copied)
			tcp_push(sk, flags & ~MSG_MORE, mss_now,
				 TCP_NAGLE_PUSH, size_goal);

		err = sk_stream_wait_memory(sk, &timeo);
		if (err != 0)
			goto do_error;

		mss_now = tcp_send_mss(sk, &size_goal, flags);
	}

out:
	if (copied) {
		tcp_tx_timestamp(sk, &sockc);
		tcp_push(sk, flags, mss_now, tp->nonagle, size_goal);
	}
out_nopush:
	/* msg->msg_ubuf is pinned by the caller so we don't take extra refs */
	if (uarg && !msg->msg_ubuf)
		net_zcopy_put(uarg);
	return copied + copied_syn;

do_error:
	tcp_remove_empty_skb(sk);

	if (copied + copied_syn)
		goto out;
out_err:
	/* msg->msg_ubuf is pinned by the caller so we don't take extra refs */
	if (uarg && !msg->msg_ubuf)
		net_zcopy_put_abort(uarg, true);
	err = sk_stream_error(sk, flags, err);
	/* make sure we wake any epoll edge trigger waiter */
	if (unlikely(tcp_rtx_and_write_queues_empty(sk) && err == -EAGAIN)) {
		sk->sk_write_space(sk);
		tcp_chrono_stop(sk, TCP_CHRONO_SNDBUF_LIMITED);
	}
	return err;
}
EXPORT_SYMBOL_GPL(tcp_sendmsg_locked);

int tcp_sendmsg(struct sock *sk, struct msghdr *msg, size_t size)
{
	int ret;

	lock_sock(sk);
	ret = tcp_sendmsg_locked(sk, msg, size);
	release_sock(sk);

	return ret;
}
EXPORT_SYMBOL(tcp_sendmsg);

void tcp_splice_eof(struct socket *sock)
{
	struct sock *sk = sock->sk;
	struct tcp_sock *tp = tcp_sk(sk);
	int mss_now, size_goal;

	if (!tcp_write_queue_tail(sk))
		return;

	lock_sock(sk);
	mss_now = tcp_send_mss(sk, &size_goal, 0);
	tcp_push(sk, 0, mss_now, tp->nonagle, size_goal);
	release_sock(sk);
}
EXPORT_IPV6_MOD_GPL(tcp_splice_eof);

/*
 *	Handle reading urgent data. BSD has very simple semantics for
 *	this, no blocking and very strange errors 8)
 */

static int tcp_recv_urg(struct sock *sk, struct msghdr *msg, int len, int flags)
{
	struct tcp_sock *tp = tcp_sk(sk);

	/* No URG data to read. */
	if (sock_flag(sk, SOCK_URGINLINE) || !tp->urg_data ||
	    tp->urg_data == TCP_URG_READ)
		return -EINVAL;	/* Yes this is right ! */

	if (sk->sk_state == TCP_CLOSE && !sock_flag(sk, SOCK_DONE))
		return -ENOTCONN;

	if (tp->urg_data & TCP_URG_VALID) {
		int err = 0;
		char c = tp->urg_data;

		if (!(flags & MSG_PEEK))
			WRITE_ONCE(tp->urg_data, TCP_URG_READ);

		/* Read urgent data. */
		msg->msg_flags |= MSG_OOB;

		if (len > 0) {
			if (!(flags & MSG_TRUNC))
				err = memcpy_to_msg(msg, &c, 1);
			len = 1;
		} else
			msg->msg_flags |= MSG_TRUNC;

		return err ? -EFAULT : len;
	}

	if (sk->sk_state == TCP_CLOSE || (sk->sk_shutdown & RCV_SHUTDOWN))
		return 0;

	/* Fixed the recv(..., MSG_OOB) behaviour.  BSD docs and
	 * the available implementations agree in this case:
	 * this call should never block, independent of the
	 * blocking state of the socket.
	 * Mike <pall@rz.uni-karlsruhe.de>
	 */
	return -EAGAIN;
}

static int tcp_peek_sndq(struct sock *sk, struct msghdr *msg, int len)
{
	struct sk_buff *skb;
	int copied = 0, err = 0;

	skb_rbtree_walk(skb, &sk->tcp_rtx_queue) {
		err = skb_copy_datagram_msg(skb, 0, msg, skb->len);
		if (err)
			return err;
		copied += skb->len;
	}

	skb_queue_walk(&sk->sk_write_queue, skb) {
		err = skb_copy_datagram_msg(skb, 0, msg, skb->len);
		if (err)
			break;

		copied += skb->len;
	}

	return err ?: copied;
}

/* Clean up the receive buffer for full frames taken by the user,
 * then send an ACK if necessary.  COPIED is the number of bytes
 * tcp_recvmsg has given to the user so far, it speeds up the
 * calculation of whether or not we must ACK for the sake of
 * a window update.
 */
void __tcp_cleanup_rbuf(struct sock *sk, int copied)
{
	struct tcp_sock *tp = tcp_sk(sk);
	bool time_to_ack = false;

	if (inet_csk_ack_scheduled(sk)) {
		const struct inet_connection_sock *icsk = inet_csk(sk);

		if (/* Once-per-two-segments ACK was not sent by tcp_input.c */
		    tp->rcv_nxt - tp->rcv_wup > icsk->icsk_ack.rcv_mss ||
		    /*
		     * If this read emptied read buffer, we send ACK, if
		     * connection is not bidirectional, user drained
		     * receive buffer and there was a small segment
		     * in queue.
		     */
		    (copied > 0 &&
		     ((icsk->icsk_ack.pending & ICSK_ACK_PUSHED2) ||
		      ((icsk->icsk_ack.pending & ICSK_ACK_PUSHED) &&
		       !inet_csk_in_pingpong_mode(sk))) &&
		      !atomic_read(&sk->sk_rmem_alloc)))
			time_to_ack = true;
	}

	/* We send an ACK if we can now advertise a non-zero window
	 * which has been raised "significantly".
	 *
	 * Even if window raised up to infinity, do not send window open ACK
	 * in states, where we will not receive more. It is useless.
	 */
	if (copied > 0 && !time_to_ack && !(sk->sk_shutdown & RCV_SHUTDOWN)) {
		__u32 rcv_window_now = tcp_receive_window(tp);

		/* Optimize, __tcp_select_window() is not cheap. */
		if (2*rcv_window_now <= tp->window_clamp) {
			__u32 new_window = __tcp_select_window(sk);

			/* Send ACK now, if this read freed lots of space
			 * in our buffer. Certainly, new_window is new window.
			 * We can advertise it now, if it is not less than current one.
			 * "Lots" means "at least twice" here.
			 */
			if (new_window && new_window >= 2 * rcv_window_now)
				time_to_ack = true;
		}
	}
	if (time_to_ack)
		tcp_send_ack(sk);
}

void tcp_cleanup_rbuf(struct sock *sk, int copied)
{
	struct sk_buff *skb = skb_peek(&sk->sk_receive_queue);
	struct tcp_sock *tp = tcp_sk(sk);

	WARN(skb && !before(tp->copied_seq, TCP_SKB_CB(skb)->end_seq),
	     "cleanup rbuf bug: copied %X seq %X rcvnxt %X\n",
	     tp->copied_seq, TCP_SKB_CB(skb)->end_seq, tp->rcv_nxt);
	__tcp_cleanup_rbuf(sk, copied);
}

static void tcp_eat_recv_skb(struct sock *sk, struct sk_buff *skb)
{
	__skb_unlink(skb, &sk->sk_receive_queue);
	if (likely(skb->destructor == sock_rfree)) {
		sock_rfree(skb);
		skb->destructor = NULL;
		skb->sk = NULL;
		return skb_attempt_defer_free(skb);
	}
	__kfree_skb(skb);
}

struct sk_buff *tcp_recv_skb(struct sock *sk, u32 seq, u32 *off)
{
	struct sk_buff *skb;
	u32 offset;

	while ((skb = skb_peek(&sk->sk_receive_queue)) != NULL) {
		offset = seq - TCP_SKB_CB(skb)->seq;
		if (unlikely(TCP_SKB_CB(skb)->tcp_flags & TCPHDR_SYN)) {
			pr_err_once("%s: found a SYN, please report !\n", __func__);
			offset--;
		}
		if (offset < skb->len || (TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN)) {
			*off = offset;
			return skb;
		}
		/* This looks weird, but this can happen if TCP collapsing
		 * splitted a fat GRO packet, while we released socket lock
		 * in skb_splice_bits()
		 */
		tcp_eat_recv_skb(sk, skb);
	}
	return NULL;
}
EXPORT_SYMBOL(tcp_recv_skb);

/*
 * This routine provides an alternative to tcp_recvmsg() for routines
 * that would like to handle copying from skbuffs directly in 'sendfile'
 * fashion.
 * Note:
 *	- It is assumed that the socket was locked by the caller.
 *	- The routine does not block.
 *	- At present, there is no support for reading OOB data
 *	  or for 'peeking' the socket using this routine
 *	  (although both would be easy to implement).
 */
static int __tcp_read_sock(struct sock *sk, read_descriptor_t *desc,
			   sk_read_actor_t recv_actor, bool noack,
			   u32 *copied_seq)
{
	struct sk_buff *skb;
	struct tcp_sock *tp = tcp_sk(sk);
	u32 seq = *copied_seq;
	u32 offset;
	int copied = 0;

	if (sk->sk_state == TCP_LISTEN)
		return -ENOTCONN;
	while ((skb = tcp_recv_skb(sk, seq, &offset)) != NULL) {
		if (offset < skb->len) {
			int used;
			size_t len;

			len = skb->len - offset;
			/* Stop reading if we hit a patch of urgent data */
			if (unlikely(tp->urg_data)) {
				u32 urg_offset = tp->urg_seq - seq;
				if (urg_offset < len)
					len = urg_offset;
				if (!len)
					break;
			}
			used = recv_actor(desc, skb, offset, len);
			if (used <= 0) {
				if (!copied)
					copied = used;
				break;
			}
			if (WARN_ON_ONCE(used > len))
				used = len;
			seq += used;
			copied += used;
			offset += used;

			/* If recv_actor drops the lock (e.g. TCP splice
			 * receive) the skb pointer might be invalid when
			 * getting here: tcp_collapse might have deleted it
			 * while aggregating skbs from the socket queue.
			 */
			skb = tcp_recv_skb(sk, seq - 1, &offset);
			if (!skb)
				break;
			/* TCP coalescing might have appended data to the skb.
			 * Try to splice more frags
			 */
			if (offset + 1 != skb->len)
				continue;
		}
		if (TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN) {
			tcp_eat_recv_skb(sk, skb);
			++seq;
			break;
		}
		tcp_eat_recv_skb(sk, skb);
		if (!desc->count)
			break;
		WRITE_ONCE(*copied_seq, seq);
	}
	WRITE_ONCE(*copied_seq, seq);

	if (noack)
		goto out;

	tcp_rcv_space_adjust(sk);

	/* Clean up data we have read: This will do ACK frames. */
	if (copied > 0) {
		tcp_recv_skb(sk, seq, &offset);
		tcp_cleanup_rbuf(sk, copied);
	}
out:
	return copied;
}

int tcp_read_sock(struct sock *sk, read_descriptor_t *desc,
		  sk_read_actor_t recv_actor)
{
	return __tcp_read_sock(sk, desc, recv_actor, false,
			       &tcp_sk(sk)->copied_seq);
}
EXPORT_SYMBOL(tcp_read_sock);

int tcp_read_sock_noack(struct sock *sk, read_descriptor_t *desc,
			sk_read_actor_t recv_actor, bool noack,
			u32 *copied_seq)
{
	return __tcp_read_sock(sk, desc, recv_actor, noack, copied_seq);
}

int tcp_read_skb(struct sock *sk, skb_read_actor_t recv_actor)
{
	struct sk_buff *skb;
	int copied = 0;

	if (sk->sk_state == TCP_LISTEN)
		return -ENOTCONN;

	while ((skb = skb_peek(&sk->sk_receive_queue)) != NULL) {
		u8 tcp_flags;
		int used;

		__skb_unlink(skb, &sk->sk_receive_queue);
		WARN_ON_ONCE(!skb_set_owner_sk_safe(skb, sk));
		tcp_flags = TCP_SKB_CB(skb)->tcp_flags;
		used = recv_actor(sk, skb);
		if (used < 0) {
			if (!copied)
				copied = used;
			break;
		}
		copied += used;

		if (tcp_flags & TCPHDR_FIN)
			break;
	}
	return copied;
}
EXPORT_IPV6_MOD(tcp_read_skb);

void tcp_read_done(struct sock *sk, size_t len)
{
	struct tcp_sock *tp = tcp_sk(sk);
	u32 seq = tp->copied_seq;
	struct sk_buff *skb;
	size_t left;
	u32 offset;

	if (sk->sk_state == TCP_LISTEN)
		return;

	left = len;
	while (left && (skb = tcp_recv_skb(sk, seq, &offset)) != NULL) {
		int used;

		used = min_t(size_t, skb->len - offset, left);
		seq += used;
		left -= used;

		if (skb->len > offset + used)
			break;

		if (TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN) {
			tcp_eat_recv_skb(sk, skb);
			++seq;
			break;
		}
		tcp_eat_recv_skb(sk, skb);
	}
	WRITE_ONCE(tp->copied_seq, seq);

	tcp_rcv_space_adjust(sk);

	/* Clean up data we have read: This will do ACK frames. */
	if (left != len)
		tcp_cleanup_rbuf(sk, len - left);
}
EXPORT_SYMBOL(tcp_read_done);

int tcp_peek_len(struct socket *sock)
{
	return tcp_inq(sock->sk);
}
EXPORT_IPV6_MOD(tcp_peek_len);

/* Make sure sk_rcvbuf is big enough to satisfy SO_RCVLOWAT hint */
int tcp_set_rcvlowat(struct sock *sk, int val)
{
	int space, cap;

	if (sk->sk_userlocks & SOCK_RCVBUF_LOCK)
		cap = sk->sk_rcvbuf >> 1;
	else
		cap = READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_rmem[2]) >> 1;
	val = min(val, cap);
	WRITE_ONCE(sk->sk_rcvlowat, val ? : 1);

	/* Check if we need to signal EPOLLIN right now */
	tcp_data_ready(sk);

	if (sk->sk_userlocks & SOCK_RCVBUF_LOCK)
		return 0;

	space = tcp_space_from_win(sk, val);
	if (space > sk->sk_rcvbuf) {
		WRITE_ONCE(sk->sk_rcvbuf, space);
		WRITE_ONCE(tcp_sk(sk)->window_clamp, val);
	}
	return 0;
}
EXPORT_IPV6_MOD(tcp_set_rcvlowat);

void tcp_update_recv_tstamps(struct sk_buff *skb,
			     struct scm_timestamping_internal *tss)
{
	if (skb->tstamp)
		tss->ts[0] = ktime_to_timespec64(skb->tstamp);
	else
		tss->ts[0] = (struct timespec64) {0};

	if (skb_hwtstamps(skb)->hwtstamp)
		tss->ts[2] = ktime_to_timespec64(skb_hwtstamps(skb)->hwtstamp);
	else
		tss->ts[2] = (struct timespec64) {0};
}

#ifdef CONFIG_MMU
static const struct vm_operations_struct tcp_vm_ops = {
};

int tcp_mmap(struct file *file, struct socket *sock,
	     struct vm_area_struct *vma)
{
	if (vma->vm_flags & (VM_WRITE | VM_EXEC))
		return -EPERM;
	vm_flags_clear(vma, VM_MAYWRITE | VM_MAYEXEC);

	/* Instruct vm_insert_page() to not mmap_read_lock(mm) */
	vm_flags_set(vma, VM_MIXEDMAP);

	vma->vm_ops = &tcp_vm_ops;
	return 0;
}
EXPORT_IPV6_MOD(tcp_mmap);

static skb_frag_t *skb_advance_to_frag(struct sk_buff *skb, u32 offset_skb,
				       u32 *offset_frag)
{
	skb_frag_t *frag;

	if (unlikely(offset_skb >= skb->len))
		return NULL;

	offset_skb -= skb_headlen(skb);
	if ((int)offset_skb < 0 || skb_has_frag_list(skb))
		return NULL;

	frag = skb_shinfo(skb)->frags;
	while (offset_skb) {
		if (skb_frag_size(frag) > offset_skb) {
			*offset_frag = offset_skb;
			return frag;
		}
		offset_skb -= skb_frag_size(frag);
		++frag;
	}
	*offset_frag = 0;
	return frag;
}

static bool can_map_frag(const skb_frag_t *frag)
{
	struct page *page;

	if (skb_frag_size(frag) != PAGE_SIZE || skb_frag_off(frag))
		return false;

	page = skb_frag_page(frag);

	if (PageCompound(page) || page->mapping)
		return false;

	return true;
}

static int find_next_mappable_frag(const skb_frag_t *frag,
				   int remaining_in_skb)
{
	int offset = 0;

	if (likely(can_map_frag(frag)))
		return 0;

	while (offset < remaining_in_skb && !can_map_frag(frag)) {
		offset += skb_frag_size(frag);
		++frag;
	}
	return offset;
}

static void tcp_zerocopy_set_hint_for_skb(struct sock *sk,
					  struct tcp_zerocopy_receive *zc,
					  struct sk_buff *skb, u32 offset)
{
	u32 frag_offset, partial_frag_remainder = 0;
	int mappable_offset;
	skb_frag_t *frag;

	/* worst case: skip to next skb. try to improve on this case below */
	zc->recv_skip_hint = skb->len - offset;

	/* Find the frag containing this offset (and how far into that frag) */
	frag = skb_advance_to_frag(skb, offset, &frag_offset);
	if (!frag)
		return;

	if (frag_offset) {
		struct skb_shared_info *info = skb_shinfo(skb);

		/* We read part of the last frag, must recvmsg() rest of skb. */
		if (frag == &info->frags[info->nr_frags - 1])
			return;

		/* Else, we must at least read the remainder in this frag. */
		partial_frag_remainder = skb_frag_size(frag) - frag_offset;
		zc->recv_skip_hint -= partial_frag_remainder;
		++frag;
	}

	/* partial_frag_remainder: If part way through a frag, must read rest.
	 * mappable_offset: Bytes till next mappable frag, *not* counting bytes
	 * in partial_frag_remainder.
	 */
	mappable_offset = find_next_mappable_frag(frag, zc->recv_skip_hint);
	zc->recv_skip_hint = mappable_offset + partial_frag_remainder;
}

static int tcp_recvmsg_locked(struct sock *sk, struct msghdr *msg, size_t len,
			      int flags, struct scm_timestamping_internal *tss,
			      int *cmsg_flags);
static int receive_fallback_to_copy(struct sock *sk,
				    struct tcp_zerocopy_receive *zc, int inq,
				    struct scm_timestamping_internal *tss)
{
	unsigned long copy_address = (unsigned long)zc->copybuf_address;
	struct msghdr msg = {};
	int err;

	zc->length = 0;
	zc->recv_skip_hint = 0;

	if (copy_address != zc->copybuf_address)
		return -EINVAL;

	err = import_ubuf(ITER_DEST, (void __user *)copy_address, inq,
			  &msg.msg_iter);
	if (err)
		return err;

	err = tcp_recvmsg_locked(sk, &msg, inq, MSG_DONTWAIT,
				 tss, &zc->msg_flags);
	if (err < 0)
		return err;

	zc->copybuf_len = err;
	if (likely(zc->copybuf_len)) {
		struct sk_buff *skb;
		u32 offset;

		skb = tcp_recv_skb(sk, tcp_sk(sk)->copied_seq, &offset);
		if (skb)
			tcp_zerocopy_set_hint_for_skb(sk, zc, skb, offset);
	}
	return 0;
}

static int tcp_copy_straggler_data(struct tcp_zerocopy_receive *zc,
				   struct sk_buff *skb, u32 copylen,
				   u32 *offset, u32 *seq)
{
	unsigned long copy_address = (unsigned long)zc->copybuf_address;
	struct msghdr msg = {};
	int err;

	if (copy_address != zc->copybuf_address)
		return -EINVAL;

	err = import_ubuf(ITER_DEST, (void __user *)copy_address, copylen,
			  &msg.msg_iter);
	if (err)
		return err;
	err = skb_copy_datagram_msg(skb, *offset, &msg, copylen);
	if (err)
		return err;
	zc->recv_skip_hint -= copylen;
	*offset += copylen;
	*seq += copylen;
	return (__s32)copylen;
}

static int tcp_zc_handle_leftover(struct tcp_zerocopy_receive *zc,
				  struct sock *sk,
				  struct sk_buff *skb,
				  u32 *seq,
				  s32 copybuf_len,
				  struct scm_timestamping_internal *tss)
{
	u32 offset, copylen = min_t(u32, copybuf_len, zc->recv_skip_hint);

	if (!copylen)
		return 0;
	/* skb is null if inq < PAGE_SIZE. */
	if (skb) {
		offset = *seq - TCP_SKB_CB(skb)->seq;
	} else {
		skb = tcp_recv_skb(sk, *seq, &offset);
		if (TCP_SKB_CB(skb)->has_rxtstamp) {
			tcp_update_recv_tstamps(skb, tss);
			zc->msg_flags |= TCP_CMSG_TS;
		}
	}

	zc->copybuf_len = tcp_copy_straggler_data(zc, skb, copylen, &offset,
						  seq);
	return zc->copybuf_len < 0 ? 0 : copylen;
}

static int tcp_zerocopy_vm_insert_batch_error(struct vm_area_struct *vma,
					      struct page **pending_pages,
					      unsigned long pages_remaining,
					      unsigned long *address,
					      u32 *length,
					      u32 *seq,
					      struct tcp_zerocopy_receive *zc,
					      u32 total_bytes_to_map,
					      int err)
{
	/* At least one page did not map. Try zapping if we skipped earlier. */
	if (err == -EBUSY &&
	    zc->flags & TCP_RECEIVE_ZEROCOPY_FLAG_TLB_CLEAN_HINT) {
		u32 maybe_zap_len;

		maybe_zap_len = total_bytes_to_map -  /* All bytes to map */
				*length + /* Mapped or pending */
				(pages_remaining * PAGE_SIZE); /* Failed map. */
		zap_page_range_single(vma, *address, maybe_zap_len, NULL);
		err = 0;
	}

	if (!err) {
		unsigned long leftover_pages = pages_remaining;
		int bytes_mapped;

		/* We called zap_page_range_single, try to reinsert. */
		err = vm_insert_pages(vma, *address,
				      pending_pages,
				      &pages_remaining);
		bytes_mapped = PAGE_SIZE * (leftover_pages - pages_remaining);
		*seq += bytes_mapped;
		*address += bytes_mapped;
	}
	if (err) {
		/* Either we were unable to zap, OR we zapped, retried an
		 * insert, and still had an issue. Either ways, pages_remaining
		 * is the number of pages we were unable to map, and we unroll
		 * some state we speculatively touched before.
		 */
		const int bytes_not_mapped = PAGE_SIZE * pages_remaining;

		*length -= bytes_not_mapped;
		zc->recv_skip_hint += bytes_not_mapped;
	}
	return err;
}

static int tcp_zerocopy_vm_insert_batch(struct vm_area_struct *vma,
					struct page **pages,
					unsigned int pages_to_map,
					unsigned long *address,
					u32 *length,
					u32 *seq,
					struct tcp_zerocopy_receive *zc,
					u32 total_bytes_to_map)
{
	unsigned long pages_remaining = pages_to_map;
	unsigned int pages_mapped;
	unsigned int bytes_mapped;
	int err;

	err = vm_insert_pages(vma, *address, pages, &pages_remaining);
	pages_mapped = pages_to_map - (unsigned int)pages_remaining;
	bytes_mapped = PAGE_SIZE * pages_mapped;
	/* Even if vm_insert_pages fails, it may have partially succeeded in
	 * mapping (some but not all of the pages).
	 */
	*seq += bytes_mapped;
	*address += bytes_mapped;

	if (likely(!err))
		return 0;

	/* Error: maybe zap and retry + rollback state for failed inserts. */
	return tcp_zerocopy_vm_insert_batch_error(vma, pages + pages_mapped,
		pages_remaining, address, length, seq, zc, total_bytes_to_map,
		err);
}

#define TCP_VALID_ZC_MSG_FLAGS   (TCP_CMSG_TS)
static void tcp_zc_finalize_rx_tstamp(struct sock *sk,
				      struct tcp_zerocopy_receive *zc,
				      struct scm_timestamping_internal *tss)
{
	unsigned long msg_control_addr;
	struct msghdr cmsg_dummy;

	msg_control_addr = (unsigned long)zc->msg_control;
	cmsg_dummy.msg_control_user = (void __user *)msg_control_addr;
	cmsg_dummy.msg_controllen =
		(__kernel_size_t)zc->msg_controllen;
	cmsg_dummy.msg_flags = in_compat_syscall()
		? MSG_CMSG_COMPAT : 0;
	cmsg_dummy.msg_control_is_user = true;
	zc->msg_flags = 0;
	if (zc->msg_control == msg_control_addr &&
	    zc->msg_controllen == cmsg_dummy.msg_controllen) {
		tcp_recv_timestamp(&cmsg_dummy, sk, tss);
		zc->msg_control = (__u64)
			((uintptr_t)cmsg_dummy.msg_control_user);
		zc->msg_controllen =
			(__u64)cmsg_dummy.msg_controllen;
		zc->msg_flags = (__u32)cmsg_dummy.msg_flags;
	}
}

static struct vm_area_struct *find_tcp_vma(struct mm_struct *mm,
					   unsigned long address,
					   bool *mmap_locked)
{
	struct vm_area_struct *vma = lock_vma_under_rcu(mm, address);

	if (vma) {
		if (vma->vm_ops != &tcp_vm_ops) {
			vma_end_read(vma);
			return NULL;
		}
		*mmap_locked = false;
		return vma;
	}

	mmap_read_lock(mm);
	vma = vma_lookup(mm, address);
	if (!vma || vma->vm_ops != &tcp_vm_ops) {
		mmap_read_unlock(mm);
		return NULL;
	}
	*mmap_locked = true;
	return vma;
}

#define TCP_ZEROCOPY_PAGE_BATCH_SIZE 32
static int tcp_zerocopy_receive(struct sock *sk,
				struct tcp_zerocopy_receive *zc,
				struct scm_timestamping_internal *tss)
{
	u32 length = 0, offset, vma_len, avail_len, copylen = 0;
	unsigned long address = (unsigned long)zc->address;
	struct page *pages[TCP_ZEROCOPY_PAGE_BATCH_SIZE];
	s32 copybuf_len = zc->copybuf_len;
	struct tcp_sock *tp = tcp_sk(sk);
	const skb_frag_t *frags = NULL;
	unsigned int pages_to_map = 0;
	struct vm_area_struct *vma;
	struct sk_buff *skb = NULL;
	u32 seq = tp->copied_seq;
	u32 total_bytes_to_map;
	int inq = tcp_inq(sk);
	bool mmap_locked;
	int ret;

	zc->copybuf_len = 0;
	zc->msg_flags = 0;

	if (address & (PAGE_SIZE - 1) || address != zc->address)
		return -EINVAL;

	if (sk->sk_state == TCP_LISTEN)
		return -ENOTCONN;

	sock_rps_record_flow(sk);

	if (inq && inq <= copybuf_len)
		return receive_fallback_to_copy(sk, zc, inq, tss);

	if (inq < PAGE_SIZE) {
		zc->length = 0;
		zc->recv_skip_hint = inq;
		if (!inq && sock_flag(sk, SOCK_DONE))
			return -EIO;
		return 0;
	}

	vma = find_tcp_vma(current->mm, address, &mmap_locked);
	if (!vma)
		return -EINVAL;

	vma_len = min_t(unsigned long, zc->length, vma->vm_end - address);
	avail_len = min_t(u32, vma_len, inq);
	total_bytes_to_map = avail_len & ~(PAGE_SIZE - 1);
	if (total_bytes_to_map) {
		if (!(zc->flags & TCP_RECEIVE_ZEROCOPY_FLAG_TLB_CLEAN_HINT))
			zap_page_range_single(vma, address, total_bytes_to_map,
					      NULL);
		zc->length = total_bytes_to_map;
		zc->recv_skip_hint = 0;
	} else {
		zc->length = avail_len;
		zc->recv_skip_hint = avail_len;
	}
	ret = 0;
	while (length + PAGE_SIZE <= zc->length) {
		int mappable_offset;
		struct page *page;

		if (zc->recv_skip_hint < PAGE_SIZE) {
			u32 offset_frag;

			if (skb) {
				if (zc->recv_skip_hint > 0)
					break;
				skb = skb->next;
				offset = seq - TCP_SKB_CB(skb)->seq;
			} else {
				skb = tcp_recv_skb(sk, seq, &offset);
			}

			if (!skb_frags_readable(skb))
				break;

			if (TCP_SKB_CB(skb)->has_rxtstamp) {
				tcp_update_recv_tstamps(skb, tss);
				zc->msg_flags |= TCP_CMSG_TS;
			}
			zc->recv_skip_hint = skb->len - offset;
			frags = skb_advance_to_frag(skb, offset, &offset_frag);
			if (!frags || offset_frag)
				break;
		}

		mappable_offset = find_next_mappable_frag(frags,
							  zc->recv_skip_hint);
		if (mappable_offset) {
			zc->recv_skip_hint = mappable_offset;
			break;
		}
		page = skb_frag_page(frags);
		if (WARN_ON_ONCE(!page))
			break;

		prefetchw(page);
		pages[pages_to_map++] = page;
		length += PAGE_SIZE;
		zc->recv_skip_hint -= PAGE_SIZE;
		frags++;
		if (pages_to_map == TCP_ZEROCOPY_PAGE_BATCH_SIZE ||
		    zc->recv_skip_hint < PAGE_SIZE) {
			/* Either full batch, or we're about to go to next skb
			 * (and we cannot unroll failed ops across skbs).
			 */
			ret = tcp_zerocopy_vm_insert_batch(vma, pages,
							   pages_to_map,
							   &address, &length,
							   &seq, zc,
							   total_bytes_to_map);
			if (ret)
				goto out;
			pages_to_map = 0;
		}
	}
	if (pages_to_map) {
		ret = tcp_zerocopy_vm_insert_batch(vma, pages, pages_to_map,
						   &address, &length, &seq,
						   zc, total_bytes_to_map);
	}
out:
	if (mmap_locked)
		mmap_read_unlock(current->mm);
	else
		vma_end_read(vma);
	/* Try to copy straggler data. */
	if (!ret)
		copylen = tcp_zc_handle_leftover(zc, sk, skb, &seq, copybuf_len, tss);

	if (length + copylen) {
		WRITE_ONCE(tp->copied_seq, seq);
		tcp_rcv_space_adjust(sk);

		/* Clean up data we have read: This will do ACK frames. */
		tcp_recv_skb(sk, seq, &offset);
		tcp_cleanup_rbuf(sk, length + copylen);
		ret = 0;
		if (length == zc->length)
			zc->recv_skip_hint = 0;
	} else {
		if (!zc->recv_skip_hint && sock_flag(sk, SOCK_DONE))
			ret = -EIO;
	}
	zc->length = length;
	return ret;
}
#endif

/* Similar to __sock_recv_timestamp, but does not require an skb */
void tcp_recv_timestamp(struct msghdr *msg, const struct sock *sk,
			struct scm_timestamping_internal *tss)
{
	int new_tstamp = sock_flag(sk, SOCK_TSTAMP_NEW);
	u32 tsflags = READ_ONCE(sk->sk_tsflags);
	bool has_timestamping = false;

	if (tss->ts[0].tv_sec || tss->ts[0].tv_nsec) {
		if (sock_flag(sk, SOCK_RCVTSTAMP)) {
			if (sock_flag(sk, SOCK_RCVTSTAMPNS)) {
				if (new_tstamp) {
					struct __kernel_timespec kts = {
						.tv_sec = tss->ts[0].tv_sec,
						.tv_nsec = tss->ts[0].tv_nsec,
					};
					put_cmsg(msg, SOL_SOCKET, SO_TIMESTAMPNS_NEW,
						 sizeof(kts), &kts);
				} else {
					struct __kernel_old_timespec ts_old = {
						.tv_sec = tss->ts[0].tv_sec,
						.tv_nsec = tss->ts[0].tv_nsec,
					};
					put_cmsg(msg, SOL_SOCKET, SO_TIMESTAMPNS_OLD,
						 sizeof(ts_old), &ts_old);
				}
			} else {
				if (new_tstamp) {
					struct __kernel_sock_timeval stv = {
						.tv_sec = tss->ts[0].tv_sec,
						.tv_usec = tss->ts[0].tv_nsec / 1000,
					};
					put_cmsg(msg, SOL_SOCKET, SO_TIMESTAMP_NEW,
						 sizeof(stv), &stv);
				} else {
					struct __kernel_old_timeval tv = {
						.tv_sec = tss->ts[0].tv_sec,
						.tv_usec = tss->ts[0].tv_nsec / 1000,
					};
					put_cmsg(msg, SOL_SOCKET, SO_TIMESTAMP_OLD,
						 sizeof(tv), &tv);
				}
			}
		}

		if (tsflags & SOF_TIMESTAMPING_SOFTWARE &&
		    (tsflags & SOF_TIMESTAMPING_RX_SOFTWARE ||
		     !(tsflags & SOF_TIMESTAMPING_OPT_RX_FILTER)))
			has_timestamping = true;
		else
			tss->ts[0] = (struct timespec64) {0};
	}

	if (tss->ts[2].tv_sec || tss->ts[2].tv_nsec) {
		if (tsflags & SOF_TIMESTAMPING_RAW_HARDWARE &&
		    (tsflags & SOF_TIMESTAMPING_RX_HARDWARE ||
		     !(tsflags & SOF_TIMESTAMPING_OPT_RX_FILTER)))
			has_timestamping = true;
		else
			tss->ts[2] = (struct timespec64) {0};
	}

	if (has_timestamping) {
		tss->ts[1] = (struct timespec64) {0};
		if (sock_flag(sk, SOCK_TSTAMP_NEW))
			put_cmsg_scm_timestamping64(msg, tss);
		else
			put_cmsg_scm_timestamping(msg, tss);
	}
}

static int tcp_inq_hint(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	u32 copied_seq = READ_ONCE(tp->copied_seq);
	u32 rcv_nxt = READ_ONCE(tp->rcv_nxt);
	int inq;

	inq = rcv_nxt - copied_seq;
	if (unlikely(inq < 0 || copied_seq != READ_ONCE(tp->copied_seq))) {
		lock_sock(sk);
		inq = tp->rcv_nxt - tp->copied_seq;
		release_sock(sk);
	}
	/* After receiving a FIN, tell the user-space to continue reading
	 * by returning a non-zero inq.
	 */
	if (inq == 0 && sock_flag(sk, SOCK_DONE))
		inq = 1;
	return inq;
}

/* batch __xa_alloc() calls and reduce xa_lock()/xa_unlock() overhead. */
struct tcp_xa_pool {
	u8		max; /* max <= MAX_SKB_FRAGS */
	u8		idx; /* idx <= max */
	__u32		tokens[MAX_SKB_FRAGS];
	netmem_ref	netmems[MAX_SKB_FRAGS];
};

static void tcp_xa_pool_commit_locked(struct sock *sk, struct tcp_xa_pool *p)
{
	int i;

	/* Commit part that has been copied to user space. */
	for (i = 0; i < p->idx; i++)
		__xa_cmpxchg(&sk->sk_user_frags, p->tokens[i], XA_ZERO_ENTRY,
			     (__force void *)p->netmems[i], GFP_KERNEL);
	/* Rollback what has been pre-allocated and is no longer needed. */
	for (; i < p->max; i++)
		__xa_erase(&sk->sk_user_frags, p->tokens[i]);

	p->max = 0;
	p->idx = 0;
}

static void tcp_xa_pool_commit(struct sock *sk, struct tcp_xa_pool *p)
{
	if (!p->max)
		return;

	xa_lock_bh(&sk->sk_user_frags);

	tcp_xa_pool_commit_locked(sk, p);

	xa_unlock_bh(&sk->sk_user_frags);
}

static int tcp_xa_pool_refill(struct sock *sk, struct tcp_xa_pool *p,
			      unsigned int max_frags)
{
	int err, k;

	if (p->idx < p->max)
		return 0;

	xa_lock_bh(&sk->sk_user_frags);

	tcp_xa_pool_commit_locked(sk, p);

	for (k = 0; k < max_frags; k++) {
		err = __xa_alloc(&sk->sk_user_frags, &p->tokens[k],
				 XA_ZERO_ENTRY, xa_limit_31b, GFP_KERNEL);
		if (err)
			break;
	}

	xa_unlock_bh(&sk->sk_user_frags);

	p->max = k;
	p->idx = 0;
	return k ? 0 : err;
}

/* On error, returns the -errno. On success, returns number of bytes sent to the
 * user. May not consume all of @remaining_len.
 */
static int tcp_recvmsg_dmabuf(struct sock *sk, const struct sk_buff *skb,
			      unsigned int offset, struct msghdr *msg,
			      int remaining_len)
{
	struct dmabuf_cmsg dmabuf_cmsg = { 0 };
	struct tcp_xa_pool tcp_xa_pool;
	unsigned int start;
	int i, copy, n;
	int sent = 0;
	int err = 0;

	tcp_xa_pool.max = 0;
	tcp_xa_pool.idx = 0;
	do {
		start = skb_headlen(skb);

		if (skb_frags_readable(skb)) {
			err = -ENODEV;
			goto out;
		}

		/* Copy header. */
		copy = start - offset;
		if (copy > 0) {
			copy = min(copy, remaining_len);

			n = copy_to_iter(skb->data + offset, copy,
					 &msg->msg_iter);
			if (n != copy) {
				err = -EFAULT;
				goto out;
			}

			offset += copy;
			remaining_len -= copy;

			/* First a dmabuf_cmsg for # bytes copied to user
			 * buffer.
			 */
			memset(&dmabuf_cmsg, 0, sizeof(dmabuf_cmsg));
			dmabuf_cmsg.frag_size = copy;
			err = put_cmsg_notrunc(msg, SOL_SOCKET,
					       SO_DEVMEM_LINEAR,
					       sizeof(dmabuf_cmsg),
					       &dmabuf_cmsg);
			if (err)
				goto out;

			sent += copy;

			if (remaining_len == 0)
				goto out;
		}

		/* after that, send information of dmabuf pages through a
		 * sequence of cmsg
		 */
		for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
			skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
			struct net_iov *niov;
			u64 frag_offset;
			int end;

			/* !skb_frags_readable() should indicate that ALL the
			 * frags in this skb are dmabuf net_iovs. We're checking
			 * for that flag above, but also check individual frags
			 * here. If the tcp stack is not setting
			 * skb_frags_readable() correctly, we still don't want
			 * to crash here.
			 */
			if (!skb_frag_net_iov(frag)) {
				net_err_ratelimited("Found non-dmabuf skb with net_iov");
				err = -ENODEV;
				goto out;
			}

			niov = skb_frag_net_iov(frag);
			if (!net_is_devmem_iov(niov)) {
				err = -ENODEV;
				goto out;
			}

			end = start + skb_frag_size(frag);
			copy = end - offset;

			if (copy > 0) {
				copy = min(copy, remaining_len);

				frag_offset = net_iov_virtual_addr(niov) +
					      skb_frag_off(frag) + offset -
					      start;
				dmabuf_cmsg.frag_offset = frag_offset;
				dmabuf_cmsg.frag_size = copy;
				err = tcp_xa_pool_refill(sk, &tcp_xa_pool,
							 skb_shinfo(skb)->nr_frags - i);
				if (err)
					goto out;

				/* Will perform the exchange later */
				dmabuf_cmsg.frag_token = tcp_xa_pool.tokens[tcp_xa_pool.idx];
				dmabuf_cmsg.dmabuf_id = net_devmem_iov_binding_id(niov);

				offset += copy;
				remaining_len -= copy;

				err = put_cmsg_notrunc(msg, SOL_SOCKET,
						       SO_DEVMEM_DMABUF,
						       sizeof(dmabuf_cmsg),
						       &dmabuf_cmsg);
				if (err)
					goto out;

				atomic_long_inc(&niov->pp_ref_count);
				tcp_xa_pool.netmems[tcp_xa_pool.idx++] = skb_frag_netmem(frag);

				sent += copy;

				if (remaining_len == 0)
					goto out;
			}
			start = end;
		}

		tcp_xa_pool_commit(sk, &tcp_xa_pool);
		if (!remaining_len)
			goto out;

		/* if remaining_len is not satisfied yet, we need to go to the
		 * next frag in the frag_list to satisfy remaining_len.
		 */
		skb = skb_shinfo(skb)->frag_list ?: skb->next;

		offset = offset - start;
	} while (skb);

	if (remaining_len) {
		err = -EFAULT;
		goto out;
	}

out:
	tcp_xa_pool_commit(sk, &tcp_xa_pool);
	if (!sent)
		sent = err;

	return sent;
}

/*
 *	This routine copies from a sock struct into the user buffer.
 *
 *	Technical note: in 2.3 we work on _locked_ socket, so that
 *	tricks with *seq access order and skb->users are not required.
 *	Probably, code can be easily improved even more.
 */

static int tcp_recvmsg_locked(struct sock *sk, struct msghdr *msg, size_t len,
			      int flags, struct scm_timestamping_internal *tss,
			      int *cmsg_flags)
{
	struct tcp_sock *tp = tcp_sk(sk);
	int last_copied_dmabuf = -1; /* uninitialized */
	int copied = 0;
	u32 peek_seq;
	u32 *seq;
	unsigned long used;
	int err;
	int target;		/* Read at least this many bytes */
	long timeo;
	struct sk_buff *skb, *last;
	u32 peek_offset = 0;
	u32 urg_hole = 0;

	err = -ENOTCONN;
	if (sk->sk_state == TCP_LISTEN)
		goto out;

	if (tp->recvmsg_inq) {
		*cmsg_flags = TCP_CMSG_INQ;
		msg->msg_get_inq = 1;
	}
	timeo = sock_rcvtimeo(sk, flags & MSG_DONTWAIT);

	/* Urgent data needs to be handled specially. */
	if (flags & MSG_OOB)
		goto recv_urg;

	if (unlikely(tp->repair)) {
		err = -EPERM;
		if (!(flags & MSG_PEEK))
			goto out;

		if (tp->repair_queue == TCP_SEND_QUEUE)
			goto recv_sndq;

		err = -EINVAL;
		if (tp->repair_queue == TCP_NO_QUEUE)
			goto out;

		/* 'common' recv queue MSG_PEEK-ing */
	}

	seq = &tp->copied_seq;
	if (flags & MSG_PEEK) {
		peek_offset = max(sk_peek_offset(sk, flags), 0);
		peek_seq = tp->copied_seq + peek_offset;
		seq = &peek_seq;
	}

	target = sock_rcvlowat(sk, flags & MSG_WAITALL, len);

	do {
		u32 offset;

		/* Are we at urgent data? Stop if we have read anything or have SIGURG pending. */
		if (unlikely(tp->urg_data) && tp->urg_seq == *seq) {
			if (copied)
				break;
			if (signal_pending(current)) {
				copied = timeo ? sock_intr_errno(timeo) : -EAGAIN;
				break;
			}
		}

		/* Next get a buffer. */

		last = skb_peek_tail(&sk->sk_receive_queue);
		skb_queue_walk(&sk->sk_receive_queue, skb) {
			last = skb;
			/* Now that we have two receive queues this
			 * shouldn't happen.
			 */
			if (WARN(before(*seq, TCP_SKB_CB(skb)->seq),
				 "TCP recvmsg seq # bug: copied %X, seq %X, rcvnxt %X, fl %X\n",
				 *seq, TCP_SKB_CB(skb)->seq, tp->rcv_nxt,
				 flags))
				break;

			offset = *seq - TCP_SKB_CB(skb)->seq;
			if (unlikely(TCP_SKB_CB(skb)->tcp_flags & TCPHDR_SYN)) {
				pr_err_once("%s: found a SYN, please report !\n", __func__);
				offset--;
			}
			if (offset < skb->len)
				goto found_ok_skb;
			if (TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN)
				goto found_fin_ok;
			WARN(!(flags & MSG_PEEK),
			     "TCP recvmsg seq # bug 2: copied %X, seq %X, rcvnxt %X, fl %X\n",
			     *seq, TCP_SKB_CB(skb)->seq, tp->rcv_nxt, flags);
		}

		/* Well, if we have backlog, try to process it now yet. */

		if (copied >= target && !READ_ONCE(sk->sk_backlog.tail))
			break;

		if (copied) {
			if (!timeo ||
			    sk->sk_err ||
			    sk->sk_state == TCP_CLOSE ||
			    (sk->sk_shutdown & RCV_SHUTDOWN) ||
			    signal_pending(current))
				break;
		} else {
			if (sock_flag(sk, SOCK_DONE))
				break;

			if (sk->sk_err) {
				copied = sock_error(sk);
				break;
			}

			if (sk->sk_shutdown & RCV_SHUTDOWN)
				break;

			if (sk->sk_state == TCP_CLOSE) {
				/* This occurs when user tries to read
				 * from never connected socket.
				 */
				copied = -ENOTCONN;
				break;
			}

			if (!timeo) {
				copied = -EAGAIN;
				break;
			}

			if (signal_pending(current)) {
				copied = sock_intr_errno(timeo);
				break;
			}
		}

		if (copied >= target) {
			/* Do not sleep, just process backlog. */
			__sk_flush_backlog(sk);
		} else {
			tcp_cleanup_rbuf(sk, copied);
			err = sk_wait_data(sk, &timeo, last);
			if (err < 0) {
				err = copied ? : err;
				goto out;
			}
		}

		if ((flags & MSG_PEEK) &&
		    (peek_seq - peek_offset - copied - urg_hole != tp->copied_seq)) {
			net_dbg_ratelimited("TCP(%s:%d): Application bug, race in MSG_PEEK\n",
					    current->comm,
					    task_pid_nr(current));
			peek_seq = tp->copied_seq + peek_offset;
		}
		continue;

found_ok_skb:
		/* Ok so how much can we use? */
		used = skb->len - offset;
		if (len < used)
			used = len;

		/* Do we have urgent data here? */
		if (unlikely(tp->urg_data)) {
			u32 urg_offset = tp->urg_seq - *seq;
			if (urg_offset < used) {
				if (!urg_offset) {
					if (!sock_flag(sk, SOCK_URGINLINE)) {
						WRITE_ONCE(*seq, *seq + 1);
						urg_hole++;
						offset++;
						used--;
						if (!used)
							goto skip_copy;
					}
				} else
					used = urg_offset;
			}
		}

		if (!(flags & MSG_TRUNC)) {
			if (last_copied_dmabuf != -1 &&
			    last_copied_dmabuf != !skb_frags_readable(skb))
				break;

			if (skb_frags_readable(skb)) {
				err = skb_copy_datagram_msg(skb, offset, msg,
							    used);
				if (err) {
					/* Exception. Bailout! */
					if (!copied)
						copied = -EFAULT;
					break;
				}
			} else {
				if (!(flags & MSG_SOCK_DEVMEM)) {
					/* dmabuf skbs can only be received
					 * with the MSG_SOCK_DEVMEM flag.
					 */
					if (!copied)
						copied = -EFAULT;

					break;
				}

				err = tcp_recvmsg_dmabuf(sk, skb, offset, msg,
							 used);
				if (err <= 0) {
					if (!copied)
						copied = -EFAULT;

					break;
				}
				used = err;
			}
		}

		last_copied_dmabuf = !skb_frags_readable(skb);

		WRITE_ONCE(*seq, *seq + used);
		copied += used;
		len -= used;
		if (flags & MSG_PEEK)
			sk_peek_offset_fwd(sk, used);
		else
			sk_peek_offset_bwd(sk, used);
		tcp_rcv_space_adjust(sk);

skip_copy:
		if (unlikely(tp->urg_data) && after(tp->copied_seq, tp->urg_seq)) {
			WRITE_ONCE(tp->urg_data, 0);
			tcp_fast_path_check(sk);
		}

		if (TCP_SKB_CB(skb)->has_rxtstamp) {
			tcp_update_recv_tstamps(skb, tss);
			*cmsg_flags |= TCP_CMSG_TS;
		}

		if (used + offset < skb->len)
			continue;

		if (TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN)
			goto found_fin_ok;
		if (!(flags & MSG_PEEK))
			tcp_eat_recv_skb(sk, skb);
		continue;

found_fin_ok:
		/* Process the FIN. */
		WRITE_ONCE(*seq, *seq + 1);
		if (!(flags & MSG_PEEK))
			tcp_eat_recv_skb(sk, skb);
		break;
	} while (len > 0);

	/* According to UNIX98, msg_name/msg_namelen are ignored
	 * on connected socket. I was just happy when found this 8) --ANK
	 */

	/* Clean up data we have read: This will do ACK frames. */
	tcp_cleanup_rbuf(sk, copied);
	return copied;

out:
	return err;

recv_urg:
	err = tcp_recv_urg(sk, msg, len, flags);
	goto out;

recv_sndq:
	err = tcp_peek_sndq(sk, msg, len);
	goto out;
}

int tcp_recvmsg(struct sock *sk, struct msghdr *msg, size_t len, int flags,
		int *addr_len)
{
	int cmsg_flags = 0, ret;
	struct scm_timestamping_internal tss;

	if (unlikely(flags & MSG_ERRQUEUE))
		return inet_recv_error(sk, msg, len, addr_len);

	if (sk_can_busy_loop(sk) &&
	    skb_queue_empty_lockless(&sk->sk_receive_queue) &&
	    sk->sk_state == TCP_ESTABLISHED)
		sk_busy_loop(sk, flags & MSG_DONTWAIT);

	lock_sock(sk);
	ret = tcp_recvmsg_locked(sk, msg, len, flags, &tss, &cmsg_flags);
	release_sock(sk);

	if ((cmsg_flags || msg->msg_get_inq) && ret >= 0) {
		if (cmsg_flags & TCP_CMSG_TS)
			tcp_recv_timestamp(msg, sk, &tss);
		if (msg->msg_get_inq) {
			msg->msg_inq = tcp_inq_hint(sk);
			if (cmsg_flags & TCP_CMSG_INQ)
				put_cmsg(msg, SOL_TCP, TCP_CM_INQ,
					 sizeof(msg->msg_inq), &msg->msg_inq);
		}
	}
	return ret;
}
EXPORT_IPV6_MOD(tcp_recvmsg);

void tcp_set_state(struct sock *sk, int state)
{
	int oldstate = sk->sk_state;

	/* We defined a new enum for TCP states that are exported in BPF
	 * so as not force the internal TCP states to be frozen. The
	 * following checks will detect if an internal state value ever
	 * differs from the BPF value. If this ever happens, then we will
	 * need to remap the internal value to the BPF value before calling
	 * tcp_call_bpf_2arg.
	 */
	BUILD_BUG_ON((int)BPF_TCP_ESTABLISHED != (int)TCP_ESTABLISHED);
	BUILD_BUG_ON((int)BPF_TCP_SYN_SENT != (int)TCP_SYN_SENT);
	BUILD_BUG_ON((int)BPF_TCP_SYN_RECV != (int)TCP_SYN_RECV);
	BUILD_BUG_ON((int)BPF_TCP_FIN_WAIT1 != (int)TCP_FIN_WAIT1);
	BUILD_BUG_ON((int)BPF_TCP_FIN_WAIT2 != (int)TCP_FIN_WAIT2);
	BUILD_BUG_ON((int)BPF_TCP_TIME_WAIT != (int)TCP_TIME_WAIT);
	BUILD_BUG_ON((int)BPF_TCP_CLOSE != (int)TCP_CLOSE);
	BUILD_BUG_ON((int)BPF_TCP_CLOSE_WAIT != (int)TCP_CLOSE_WAIT);
	BUILD_BUG_ON((int)BPF_TCP_LAST_ACK != (int)TCP_LAST_ACK);
	BUILD_BUG_ON((int)BPF_TCP_LISTEN != (int)TCP_LISTEN);
	BUILD_BUG_ON((int)BPF_TCP_CLOSING != (int)TCP_CLOSING);
	BUILD_BUG_ON((int)BPF_TCP_NEW_SYN_RECV != (int)TCP_NEW_SYN_RECV);
	BUILD_BUG_ON((int)BPF_TCP_BOUND_INACTIVE != (int)TCP_BOUND_INACTIVE);
	BUILD_BUG_ON((int)BPF_TCP_MAX_STATES != (int)TCP_MAX_STATES);

	/* bpf uapi header bpf.h defines an anonymous enum with values
	 * BPF_TCP_* used by bpf programs. Currently gcc built vmlinux
	 * is able to emit this enum in DWARF due to the above BUILD_BUG_ON.
	 * But clang built vmlinux does not have this enum in DWARF
	 * since clang removes the above code before generating IR/debuginfo.
	 * Let us explicitly emit the type debuginfo to ensure the
	 * above-mentioned anonymous enum in the vmlinux DWARF and hence BTF
	 * regardless of which compiler is used.
	 */
	BTF_TYPE_EMIT_ENUM(BPF_TCP_ESTABLISHED);

	if (BPF_SOCK_OPS_TEST_FLAG(tcp_sk(sk), BPF_SOCK_OPS_STATE_CB_FLAG))
		tcp_call_bpf_2arg(sk, BPF_SOCK_OPS_STATE_CB, oldstate, state);

	switch (state) {
	case TCP_ESTABLISHED:
		if (oldstate != TCP_ESTABLISHED)
			TCP_INC_STATS(sock_net(sk), TCP_MIB_CURRESTAB);
		break;
	case TCP_CLOSE_WAIT:
		if (oldstate == TCP_SYN_RECV)
			TCP_INC_STATS(sock_net(sk), TCP_MIB_CURRESTAB);
		break;

	case TCP_CLOSE:
		if (oldstate == TCP_CLOSE_WAIT || oldstate == TCP_ESTABLISHED)
			TCP_INC_STATS(sock_net(sk), TCP_MIB_ESTABRESETS);

		sk->sk_prot->unhash(sk);
		if (inet_csk(sk)->icsk_bind_hash &&
		    !(sk->sk_userlocks & SOCK_BINDPORT_LOCK))
			inet_put_port(sk);
		fallthrough;
	default:
		if (oldstate == TCP_ESTABLISHED || oldstate == TCP_CLOSE_WAIT)
			TCP_DEC_STATS(sock_net(sk), TCP_MIB_CURRESTAB);
	}

	/* Change state AFTER socket is unhashed to avoid closed
	 * socket sitting in hash tables.
	 */
	inet_sk_state_store(sk, state);
}
EXPORT_SYMBOL_GPL(tcp_set_state);

/*
 *	State processing on a close. This implements the state shift for
 *	sending our FIN frame. Note that we only send a FIN for some
 *	states. A shutdown() may have already sent the FIN, or we may be
 *	closed.
 */

static const unsigned char new_state[16] = {
  /* current state:        new state:      action:	*/
  [0 /* (Invalid) */]	= TCP_CLOSE,
  [TCP_ESTABLISHED]	= TCP_FIN_WAIT1 | TCP_ACTION_FIN,
  [TCP_SYN_SENT]	= TCP_CLOSE,
  [TCP_SYN_RECV]	= TCP_FIN_WAIT1 | TCP_ACTION_FIN,
  [TCP_FIN_WAIT1]	= TCP_FIN_WAIT1,
  [TCP_FIN_WAIT2]	= TCP_FIN_WAIT2,
  [TCP_TIME_WAIT]	= TCP_CLOSE,
  [TCP_CLOSE]		= TCP_CLOSE,
  [TCP_CLOSE_WAIT]	= TCP_LAST_ACK  | TCP_ACTION_FIN,
  [TCP_LAST_ACK]	= TCP_LAST_ACK,
  [TCP_LISTEN]		= TCP_CLOSE,
  [TCP_CLOSING]		= TCP_CLOSING,
  [TCP_NEW_SYN_RECV]	= TCP_CLOSE,	/* should not happen ! */
};

static int tcp_close_state(struct sock *sk)
{
	int next = (int)new_state[sk->sk_state];
	int ns = next & TCP_STATE_MASK;

	tcp_set_state(sk, ns);

	return next & TCP_ACTION_FIN;
}

/*
 *	Shutdown the sending side of a connection. Much like close except
 *	that we don't receive shut down or sock_set_flag(sk, SOCK_DEAD).
 */

void tcp_shutdown(struct sock *sk, int how)
{
	/*	We need to grab some memory, and put together a FIN,
	 *	and then put it into the queue to be sent.
	 *		Tim MacKenzie(tym@dibbler.cs.monash.edu.au) 4 Dec '92.
	 */
	if (!(how & SEND_SHUTDOWN))
		return;

	/* If we've already sent a FIN, or it's a closed state, skip this. */
	if ((1 << sk->sk_state) &
	    (TCPF_ESTABLISHED | TCPF_SYN_SENT |
	     TCPF_CLOSE_WAIT)) {
		/* Clear out any half completed packets.  FIN if needed. */
		if (tcp_close_state(sk))
			tcp_send_fin(sk);
	}
}
EXPORT_IPV6_MOD(tcp_shutdown);

int tcp_orphan_count_sum(void)
{
	int i, total = 0;

	for_each_possible_cpu(i)
		total += per_cpu(tcp_orphan_count, i);

	return max(total, 0);
}

static int tcp_orphan_cache;
static struct timer_list tcp_orphan_timer;
#define TCP_ORPHAN_TIMER_PERIOD msecs_to_jiffies(100)

static void tcp_orphan_update(struct timer_list *unused)
{
	WRITE_ONCE(tcp_orphan_cache, tcp_orphan_count_sum());
	mod_timer(&tcp_orphan_timer, jiffies + TCP_ORPHAN_TIMER_PERIOD);
}

static bool tcp_too_many_orphans(int shift)
{
	return READ_ONCE(tcp_orphan_cache) << shift >
		READ_ONCE(sysctl_tcp_max_orphans);
}

static bool tcp_out_of_memory(const struct sock *sk)
{
	if (sk->sk_wmem_queued > SOCK_MIN_SNDBUF &&
	    sk_memory_allocated(sk) > sk_prot_mem_limits(sk, 2))
		return true;
	return false;
}

bool tcp_check_oom(const struct sock *sk, int shift)
{
	bool too_many_orphans, out_of_socket_memory;

	too_many_orphans = tcp_too_many_orphans(shift);
	out_of_socket_memory = tcp_out_of_memory(sk);

	if (too_many_orphans)
		net_info_ratelimited("too many orphaned sockets\n");
	if (out_of_socket_memory)
		net_info_ratelimited("out of memory -- consider tuning tcp_mem\n");
	return too_many_orphans || out_of_socket_memory;
}

void __tcp_close(struct sock *sk, long timeout)
{
	struct sk_buff *skb;
	int data_was_unread = 0;
	int state;

	WRITE_ONCE(sk->sk_shutdown, SHUTDOWN_MASK);

	if (sk->sk_state == TCP_LISTEN) {
		tcp_set_state(sk, TCP_CLOSE);

		/* Special case. */
		inet_csk_listen_stop(sk);

		goto adjudge_to_death;
	}

	/*  We need to flush the recv. buffs.  We do this only on the
	 *  descriptor close, not protocol-sourced closes, because the
	 *  reader process may not have drained the data yet!
	 */
	while ((skb = __skb_dequeue(&sk->sk_receive_queue)) != NULL) {
		u32 len = TCP_SKB_CB(skb)->end_seq - TCP_SKB_CB(skb)->seq;

		if (TCP_SKB_CB(skb)->tcp_flags & TCPHDR_FIN)
			len--;
		data_was_unread += len;
		__kfree_skb(skb);
	}

	/* If socket has been already reset (e.g. in tcp_reset()) - kill it. */
	if (sk->sk_state == TCP_CLOSE)
		goto adjudge_to_death;

	/* As outlined in RFC 2525, section 2.17, we send a RST here because
	 * data was lost. To witness the awful effects of the old behavior of
	 * always doing a FIN, run an older 2.1.x kernel or 2.0.x, start a bulk
	 * GET in an FTP client, suspend the process, wait for the client to
	 * advertise a zero window, then kill -9 the FTP client, wheee...
	 * Note: timeout is always zero in such a case.
	 */
	if (unlikely(tcp_sk(sk)->repair)) {
		sk->sk_prot->disconnect(sk, 0);
	} else if (data_was_unread) {
		/* Unread data was tossed, zap the connection. */
		NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPABORTONCLOSE);
		tcp_set_state(sk, TCP_CLOSE);
		tcp_send_active_reset(sk, sk->sk_allocation,
				      SK_RST_REASON_TCP_ABORT_ON_CLOSE);
	} else if (sock_flag(sk, SOCK_LINGER) && !sk->sk_lingertime) {
		/* Check zero linger _after_ checking for unread data. */
		sk->sk_prot->disconnect(sk, 0);
		NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPABORTONDATA);
	} else if (tcp_close_state(sk)) {
		/* We FIN if the application ate all the data before
		 * zapping the connection.
		 */

		/* RED-PEN. Formally speaking, we have broken TCP state
		 * machine. State transitions:
		 *
		 * TCP_ESTABLISHED -> TCP_FIN_WAIT1
		 * TCP_SYN_RECV	-> TCP_FIN_WAIT1 (it is difficult)
		 * TCP_CLOSE_WAIT -> TCP_LAST_ACK
		 *
		 * are legal only when FIN has been sent (i.e. in window),
		 * rather than queued out of window. Purists blame.
		 *
		 * F.e. "RFC state" is ESTABLISHED,
		 * if Linux state is FIN-WAIT-1, but FIN is still not sent.
		 *
		 * The visible declinations are that sometimes
		 * we enter time-wait state, when it is not required really
		 * (harmless), do not send active resets, when they are
		 * required by specs (TCP_ESTABLISHED, TCP_CLOSE_WAIT, when
		 * they look as CLOSING or LAST_ACK for Linux)
		 * Probably, I missed some more holelets.
		 * 						--ANK
		 * XXX (TFO) - To start off we don't support SYN+ACK+FIN
		 * in a single packet! (May consider it later but will
		 * probably need API support or TCP_CORK SYN-ACK until
		 * data is written and socket is closed.)
		 */
		tcp_send_fin(sk);
	}

	sk_stream_wait_close(sk, timeout);

adjudge_to_death:
	state = sk->sk_state;
	sock_hold(sk);
	sock_orphan(sk);

	local_bh_disable();
	bh_lock_sock(sk);
	/* remove backlog if any, without releasing ownership. */
	__release_sock(sk);

	this_cpu_inc(tcp_orphan_count);

	/* Have we already been destroyed by a softirq or backlog? */
	if (state != TCP_CLOSE && sk->sk_state == TCP_CLOSE)
		goto out;

	/*	This is a (useful) BSD violating of the RFC. There is a
	 *	problem with TCP as specified in that the other end could
	 *	keep a socket open forever with no application left this end.
	 *	We use a 1 minute timeout (about the same as BSD) then kill
	 *	our end. If they send after that then tough - BUT: long enough
	 *	that we won't make the old 4*rto = almost no time - whoops
	 *	reset mistake.
	 *
	 *	Nope, it was not mistake. It is really desired behaviour
	 *	f.e. on http servers, when such sockets are useless, but
	 *	consume significant resources. Let's do it with special
	 *	linger2	option.					--ANK
	 */

	if (sk->sk_state == TCP_FIN_WAIT2) {
		struct tcp_sock *tp = tcp_sk(sk);
		if (READ_ONCE(tp->linger2) < 0) {
			tcp_set_state(sk, TCP_CLOSE);
			tcp_send_active_reset(sk, GFP_ATOMIC,
					      SK_RST_REASON_TCP_ABORT_ON_LINGER);
			__NET_INC_STATS(sock_net(sk),
					LINUX_MIB_TCPABORTONLINGER);
		} else {
			const int tmo = tcp_fin_time(sk);

			if (tmo > TCP_TIMEWAIT_LEN) {
				tcp_reset_keepalive_timer(sk,
						tmo - TCP_TIMEWAIT_LEN);
			} else {
				tcp_time_wait(sk, TCP_FIN_WAIT2, tmo);
				goto out;
			}
		}
	}
	if (sk->sk_state != TCP_CLOSE) {
		if (tcp_check_oom(sk, 0)) {
			tcp_set_state(sk, TCP_CLOSE);
			tcp_send_active_reset(sk, GFP_ATOMIC,
					      SK_RST_REASON_TCP_ABORT_ON_MEMORY);
			__NET_INC_STATS(sock_net(sk),
					LINUX_MIB_TCPABORTONMEMORY);
		} else if (!check_net(sock_net(sk))) {
			/* Not possible to send reset; just close */
			tcp_set_state(sk, TCP_CLOSE);
		}
	}

	if (sk->sk_state == TCP_CLOSE) {
		struct request_sock *req;

		req = rcu_dereference_protected(tcp_sk(sk)->fastopen_rsk,
						lockdep_sock_is_held(sk));
		/* We could get here with a non-NULL req if the socket is
		 * aborted (e.g., closed with unread data) before 3WHS
		 * finishes.
		 */
		if (req)
			reqsk_fastopen_remove(sk, req, false);
		inet_csk_destroy_sock(sk);
	}
	/* Otherwise, socket is reprieved until protocol close. */

out:
	bh_unlock_sock(sk);
	local_bh_enable();
}

void tcp_close(struct sock *sk, long timeout)
{
	lock_sock(sk);
	__tcp_close(sk, timeout);
	release_sock(sk);
	if (!sk->sk_net_refcnt)
		inet_csk_clear_xmit_timers_sync(sk);
	sock_put(sk);
}
EXPORT_SYMBOL(tcp_close);

/* These states need RST on ABORT according to RFC793 */

static inline bool tcp_need_reset(int state)
{
	return (1 << state) &
	       (TCPF_ESTABLISHED | TCPF_CLOSE_WAIT | TCPF_FIN_WAIT1 |
		TCPF_FIN_WAIT2 | TCPF_SYN_RECV);
}

static void tcp_rtx_queue_purge(struct sock *sk)
{
	struct rb_node *p = rb_first(&sk->tcp_rtx_queue);

	tcp_sk(sk)->highest_sack = NULL;
	while (p) {
		struct sk_buff *skb = rb_to_skb(p);

		p = rb_next(p);
		/* Since we are deleting whole queue, no need to
		 * list_del(&skb->tcp_tsorted_anchor)
		 */
		tcp_rtx_queue_unlink(skb, sk);
		tcp_wmem_free_skb(sk, skb);
	}
}

void tcp_write_queue_purge(struct sock *sk)
{
	struct sk_buff *skb;

	tcp_chrono_stop(sk, TCP_CHRONO_BUSY);
	while ((skb = __skb_dequeue(&sk->sk_write_queue)) != NULL) {
		tcp_skb_tsorted_anchor_cleanup(skb);
		tcp_wmem_free_skb(sk, skb);
	}
	tcp_rtx_queue_purge(sk);
	INIT_LIST_HEAD(&tcp_sk(sk)->tsorted_sent_queue);
	tcp_clear_all_retrans_hints(tcp_sk(sk));
	tcp_sk(sk)->packets_out = 0;
	inet_csk(sk)->icsk_backoff = 0;
}

int tcp_disconnect(struct sock *sk, int flags)
{
	struct inet_sock *inet = inet_sk(sk);
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	int old_state = sk->sk_state;
	u32 seq;

	if (old_state != TCP_CLOSE)
		tcp_set_state(sk, TCP_CLOSE);

	/* ABORT function of RFC793 */
	if (old_state == TCP_LISTEN) {
		inet_csk_listen_stop(sk);
	} else if (unlikely(tp->repair)) {
		WRITE_ONCE(sk->sk_err, ECONNABORTED);
	} else if (tcp_need_reset(old_state)) {
		tcp_send_active_reset(sk, gfp_any(), SK_RST_REASON_TCP_STATE);
		WRITE_ONCE(sk->sk_err, ECONNRESET);
	} else if (tp->snd_nxt != tp->write_seq &&
		   (1 << old_state) & (TCPF_CLOSING | TCPF_LAST_ACK)) {
		/* The last check adjusts for discrepancy of Linux wrt. RFC
		 * states
		 */
		tcp_send_active_reset(sk, gfp_any(),
				      SK_RST_REASON_TCP_DISCONNECT_WITH_DATA);
		WRITE_ONCE(sk->sk_err, ECONNRESET);
	} else if (old_state == TCP_SYN_SENT)
		WRITE_ONCE(sk->sk_err, ECONNRESET);

	tcp_clear_xmit_timers(sk);
	__skb_queue_purge(&sk->sk_receive_queue);
	WRITE_ONCE(tp->copied_seq, tp->rcv_nxt);
	WRITE_ONCE(tp->urg_data, 0);
	sk_set_peek_off(sk, -1);
	tcp_write_queue_purge(sk);
	tcp_fastopen_active_disable_ofo_check(sk);
	skb_rbtree_purge(&tp->out_of_order_queue);

	inet->inet_dport = 0;

	inet_bhash2_reset_saddr(sk);

	WRITE_ONCE(sk->sk_shutdown, 0);
	sock_reset_flag(sk, SOCK_DONE);
	tp->srtt_us = 0;
	tp->mdev_us = jiffies_to_usecs(TCP_TIMEOUT_INIT);
	tp->rcv_rtt_last_tsecr = 0;

	seq = tp->write_seq + tp->max_window + 2;
	if (!seq)
		seq = 1;
	WRITE_ONCE(tp->write_seq, seq);

	icsk->icsk_backoff = 0;
	icsk->icsk_probes_out = 0;
	icsk->icsk_probes_tstamp = 0;
	icsk->icsk_rto = TCP_TIMEOUT_INIT;
	WRITE_ONCE(icsk->icsk_rto_min, TCP_RTO_MIN);
	WRITE_ONCE(icsk->icsk_delack_max, TCP_DELACK_MAX);
	tp->snd_ssthresh = TCP_INFINITE_SSTHRESH;
	tcp_snd_cwnd_set(tp, TCP_INIT_CWND);
	tp->snd_cwnd_cnt = 0;
	tp->is_cwnd_limited = 0;
	tp->max_packets_out = 0;
	tp->window_clamp = 0;
	tp->delivered = 0;
	tp->delivered_ce = 0;
	if (icsk->icsk_ca_initialized && icsk->icsk_ca_ops->release)
		icsk->icsk_ca_ops->release(sk);
	memset(icsk->icsk_ca_priv, 0, sizeof(icsk->icsk_ca_priv));
	icsk->icsk_ca_initialized = 0;
	tcp_set_ca_state(sk, TCP_CA_Open);
	tp->is_sack_reneg = 0;
	tcp_clear_retrans(tp);
	tp->total_retrans = 0;
	inet_csk_delack_init(sk);
	/* Initialize rcv_mss to TCP_MIN_MSS to avoid division by 0
	 * issue in __tcp_select_window()
	 */
	icsk->icsk_ack.rcv_mss = TCP_MIN_MSS;
	memset(&tp->rx_opt, 0, sizeof(tp->rx_opt));
	__sk_dst_reset(sk);
	dst_release(unrcu_pointer(xchg(&sk->sk_rx_dst, NULL)));
	tcp_saved_syn_free(tp);
	tp->compressed_ack = 0;
	tp->segs_in = 0;
	tp->segs_out = 0;
	tp->bytes_sent = 0;
	tp->bytes_acked = 0;
	tp->bytes_received = 0;
	tp->bytes_retrans = 0;
	tp->data_segs_in = 0;
	tp->data_segs_out = 0;
	tp->duplicate_sack[0].start_seq = 0;
	tp->duplicate_sack[0].end_seq = 0;
	tp->dsack_dups = 0;
	tp->reord_seen = 0;
	tp->retrans_out = 0;
	tp->sacked_out = 0;
	tp->tlp_high_seq = 0;
	tp->last_oow_ack_time = 0;
	tp->plb_rehash = 0;
	/* There's a bubble in the pipe until at least the first ACK. */
	tp->app_limited = ~0U;
	tp->rate_app_limited = 1;
	tp->rack.mstamp = 0;
	tp->rack.advanced = 0;
	tp->rack.reo_wnd_steps = 1;
	tp->rack.last_delivered = 0;
	tp->rack.reo_wnd_persist = 0;
	tp->rack.dsack_seen = 0;
	tp->syn_data_acked = 0;
	tp->rx_opt.saw_tstamp = 0;
	tp->rx_opt.dsack = 0;
	tp->rx_opt.num_sacks = 0;
	tp->rcv_ooopack = 0;
	tp->fast_ack_mode = 0;


	/* Clean up fastopen related fields */
	tcp_free_fastopen_req(tp);
	inet_clear_bit(DEFER_CONNECT, sk);
	tp->fastopen_client_fail = 0;

	WARN_ON(inet->inet_num && !icsk->icsk_bind_hash);

	if (sk->sk_frag.page) {
		put_page(sk->sk_frag.page);
		sk->sk_frag.page = NULL;
		sk->sk_frag.offset = 0;
	}
	sk_error_report(sk);
	return 0;
}
EXPORT_SYMBOL(tcp_disconnect);

static inline bool tcp_can_repair_sock(const struct sock *sk)
{
	return sockopt_ns_capable(sock_net(sk)->user_ns, CAP_NET_ADMIN) &&
		(sk->sk_state != TCP_LISTEN);
}

static int tcp_repair_set_window(struct tcp_sock *tp, sockptr_t optbuf, int len)
{
	struct tcp_repair_window opt;

	if (!tp->repair)
		return -EPERM;

	if (len != sizeof(opt))
		return -EINVAL;

	if (copy_from_sockptr(&opt, optbuf, sizeof(opt)))
		return -EFAULT;

	if (opt.max_window < opt.snd_wnd)
		return -EINVAL;

	if (after(opt.snd_wl1, tp->rcv_nxt + opt.rcv_wnd))
		return -EINVAL;

	if (after(opt.rcv_wup, tp->rcv_nxt))
		return -EINVAL;

	tp->snd_wl1	= opt.snd_wl1;
	tp->snd_wnd	= opt.snd_wnd;
	tp->max_window	= opt.max_window;

	tp->rcv_wnd	= opt.rcv_wnd;
	tp->rcv_wup	= opt.rcv_wup;

	return 0;
}

static int tcp_repair_options_est(struct sock *sk, sockptr_t optbuf,
		unsigned int len)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct tcp_repair_opt opt;
	size_t offset = 0;

	while (len >= sizeof(opt)) {
		if (copy_from_sockptr_offset(&opt, optbuf, offset, sizeof(opt)))
			return -EFAULT;

		offset += sizeof(opt);
		len -= sizeof(opt);

		switch (opt.opt_code) {
		case TCPOPT_MSS:
			tp->rx_opt.mss_clamp = opt.opt_val;
			tcp_mtup_init(sk);
			break;
		case TCPOPT_WINDOW:
			{
				u16 snd_wscale = opt.opt_val & 0xFFFF;
				u16 rcv_wscale = opt.opt_val >> 16;

				if (snd_wscale > TCP_MAX_WSCALE || rcv_wscale > TCP_MAX_WSCALE)
					return -EFBIG;

				tp->rx_opt.snd_wscale = snd_wscale;
				tp->rx_opt.rcv_wscale = rcv_wscale;
				tp->rx_opt.wscale_ok = 1;
			}
			break;
		case TCPOPT_SACK_PERM:
			if (opt.opt_val != 0)
				return -EINVAL;

			tp->rx_opt.sack_ok |= TCP_SACK_SEEN;
			break;
		case TCPOPT_TIMESTAMP:
			if (opt.opt_val != 0)
				return -EINVAL;

			tp->rx_opt.tstamp_ok = 1;
			break;
		}
	}

	return 0;
}

DEFINE_STATIC_KEY_FALSE(tcp_tx_delay_enabled);
EXPORT_IPV6_MOD(tcp_tx_delay_enabled);

static void tcp_enable_tx_delay(void)
{
	if (!static_branch_unlikely(&tcp_tx_delay_enabled)) {
		static int __tcp_tx_delay_enabled = 0;

		if (cmpxchg(&__tcp_tx_delay_enabled, 0, 1) == 0) {
			static_branch_enable(&tcp_tx_delay_enabled);
			pr_info("TCP_TX_DELAY enabled\n");
		}
	}
}

/* When set indicates to always queue non-full frames.  Later the user clears
 * this option and we transmit any pending partial frames in the queue.  This is
 * meant to be used alongside sendfile() to get properly filled frames when the
 * user (for example) must write out headers with a write() call first and then
 * use sendfile to send out the data parts.
 *
 * TCP_CORK can be set together with TCP_NODELAY and it is stronger than
 * TCP_NODELAY.
 */
void __tcp_sock_set_cork(struct sock *sk, bool on)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (on) {
		tp->nonagle |= TCP_NAGLE_CORK;
	} else {
		tp->nonagle &= ~TCP_NAGLE_CORK;
		if (tp->nonagle & TCP_NAGLE_OFF)
			tp->nonagle |= TCP_NAGLE_PUSH;
		tcp_push_pending_frames(sk);
	}
}

void tcp_sock_set_cork(struct sock *sk, bool on)
{
	lock_sock(sk);
	__tcp_sock_set_cork(sk, on);
	release_sock(sk);
}
EXPORT_SYMBOL(tcp_sock_set_cork);

/* TCP_NODELAY is weaker than TCP_CORK, so that this option on corked socket is
 * remembered, but it is not activated until cork is cleared.
 *
 * However, when TCP_NODELAY is set we make an explicit push, which overrides
 * even TCP_CORK for currently queued segments.
 */
void __tcp_sock_set_nodelay(struct sock *sk, bool on)
{
	if (on) {
		tcp_sk(sk)->nonagle |= TCP_NAGLE_OFF|TCP_NAGLE_PUSH;
		tcp_push_pending_frames(sk);
	} else {
		tcp_sk(sk)->nonagle &= ~TCP_NAGLE_OFF;
	}
}

void tcp_sock_set_nodelay(struct sock *sk)
{
	lock_sock(sk);
	__tcp_sock_set_nodelay(sk, true);
	release_sock(sk);
}
EXPORT_SYMBOL(tcp_sock_set_nodelay);

static void __tcp_sock_set_quickack(struct sock *sk, int val)
{
	if (!val) {
		inet_csk_enter_pingpong_mode(sk);
		return;
	}

	inet_csk_exit_pingpong_mode(sk);
	if ((1 << sk->sk_state) & (TCPF_ESTABLISHED | TCPF_CLOSE_WAIT) &&
	    inet_csk_ack_scheduled(sk)) {
		inet_csk(sk)->icsk_ack.pending |= ICSK_ACK_PUSHED;
		tcp_cleanup_rbuf(sk, 1);
		if (!(val & 1))
			inet_csk_enter_pingpong_mode(sk);
	}
}

void tcp_sock_set_quickack(struct sock *sk, int val)
{
	lock_sock(sk);
	__tcp_sock_set_quickack(sk, val);
	release_sock(sk);
}
EXPORT_SYMBOL(tcp_sock_set_quickack);

int tcp_sock_set_syncnt(struct sock *sk, int val)
{
	if (val < 1 || val > MAX_TCP_SYNCNT)
		return -EINVAL;

	WRITE_ONCE(inet_csk(sk)->icsk_syn_retries, val);
	return 0;
}
EXPORT_SYMBOL(tcp_sock_set_syncnt);

int tcp_sock_set_user_timeout(struct sock *sk, int val)
{
	/* Cap the max time in ms TCP will retry or probe the window
	 * before giving up and aborting (ETIMEDOUT) a connection.
	 */
	if (val < 0)
		return -EINVAL;

	WRITE_ONCE(inet_csk(sk)->icsk_user_timeout, val);
	return 0;
}
EXPORT_SYMBOL(tcp_sock_set_user_timeout);

int tcp_sock_set_keepidle_locked(struct sock *sk, int val)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (val < 1 || val > MAX_TCP_KEEPIDLE)
		return -EINVAL;

	/* Paired with WRITE_ONCE() in keepalive_time_when() */
	WRITE_ONCE(tp->keepalive_time, val * HZ);
	if (sock_flag(sk, SOCK_KEEPOPEN) &&
	    !((1 << sk->sk_state) & (TCPF_CLOSE | TCPF_LISTEN))) {
		u32 elapsed = keepalive_time_elapsed(tp);

		if (tp->keepalive_time > elapsed)
			elapsed = tp->keepalive_time - elapsed;
		else
			elapsed = 0;
		tcp_reset_keepalive_timer(sk, elapsed);
	}

	return 0;
}

int tcp_sock_set_keepidle(struct sock *sk, int val)
{
	int err;

	lock_sock(sk);
	err = tcp_sock_set_keepidle_locked(sk, val);
	release_sock(sk);
	return err;
}
EXPORT_SYMBOL(tcp_sock_set_keepidle);

int tcp_sock_set_keepintvl(struct sock *sk, int val)
{
	if (val < 1 || val > MAX_TCP_KEEPINTVL)
		return -EINVAL;

	WRITE_ONCE(tcp_sk(sk)->keepalive_intvl, val * HZ);
	return 0;
}
EXPORT_SYMBOL(tcp_sock_set_keepintvl);

int tcp_sock_set_keepcnt(struct sock *sk, int val)
{
	if (val < 1 || val > MAX_TCP_KEEPCNT)
		return -EINVAL;

	/* Paired with READ_ONCE() in keepalive_probes() */
	WRITE_ONCE(tcp_sk(sk)->keepalive_probes, val);
	return 0;
}
EXPORT_SYMBOL(tcp_sock_set_keepcnt);

int tcp_set_window_clamp(struct sock *sk, int val)
{
	u32 old_window_clamp, new_window_clamp, new_rcv_ssthresh;
	struct tcp_sock *tp = tcp_sk(sk);

	if (!val) {
		if (sk->sk_state != TCP_CLOSE)
			return -EINVAL;
		WRITE_ONCE(tp->window_clamp, 0);
		return 0;
	}

	old_window_clamp = tp->window_clamp;
	new_window_clamp = max_t(int, SOCK_MIN_RCVBUF / 2, val);

	if (new_window_clamp == old_window_clamp)
		return 0;

	WRITE_ONCE(tp->window_clamp, new_window_clamp);

	/* Need to apply the reserved mem provisioning only
	 * when shrinking the window clamp.
	 */
	if (new_window_clamp < old_window_clamp) {
		__tcp_adjust_rcv_ssthresh(sk, new_window_clamp);
	} else {
		new_rcv_ssthresh = min(tp->rcv_wnd, new_window_clamp);
		tp->rcv_ssthresh = max(new_rcv_ssthresh, tp->rcv_ssthresh);
	}
	return 0;
}

/*
 *	Socket option code for TCP.
 */
int do_tcp_setsockopt(struct sock *sk, int level, int optname,
		      sockptr_t optval, unsigned int optlen)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct net *net = sock_net(sk);
	int val;
	int err = 0;

	/* These are data/string values, all the others are ints */
	switch (optname) {
	case TCP_CONGESTION: {
		char name[TCP_CA_NAME_MAX];

		if (optlen < 1)
			return -EINVAL;

		val = strncpy_from_sockptr(name, optval,
					min_t(long, TCP_CA_NAME_MAX-1, optlen));
		if (val < 0)
			return -EFAULT;
		name[val] = 0;

		sockopt_lock_sock(sk);
		err = tcp_set_congestion_control(sk, name, !has_current_bpf_ctx(),
						 sockopt_ns_capable(sock_net(sk)->user_ns,
								    CAP_NET_ADMIN));
		sockopt_release_sock(sk);
		return err;
	}
	case TCP_ULP: {
		char name[TCP_ULP_NAME_MAX];

		if (optlen < 1)
			return -EINVAL;

		val = strncpy_from_sockptr(name, optval,
					min_t(long, TCP_ULP_NAME_MAX - 1,
					      optlen));
		if (val < 0)
			return -EFAULT;
		name[val] = 0;

		sockopt_lock_sock(sk);
		err = tcp_set_ulp(sk, name);
		sockopt_release_sock(sk);
		return err;
	}
	case TCP_FASTOPEN_KEY: {
		__u8 key[TCP_FASTOPEN_KEY_BUF_LENGTH];
		__u8 *backup_key = NULL;

		/* Allow a backup key as well to facilitate key rotation
		 * First key is the active one.
		 */
		if (optlen != TCP_FASTOPEN_KEY_LENGTH &&
		    optlen != TCP_FASTOPEN_KEY_BUF_LENGTH)
			return -EINVAL;

		if (copy_from_sockptr(key, optval, optlen))
			return -EFAULT;

		if (optlen == TCP_FASTOPEN_KEY_BUF_LENGTH)
			backup_key = key + TCP_FASTOPEN_KEY_LENGTH;

		return tcp_fastopen_reset_cipher(net, sk, key, backup_key);
	}
	default:
		/* fallthru */
		break;
	}

	if (optlen < sizeof(int))
		return -EINVAL;

	if (copy_from_sockptr(&val, optval, sizeof(val)))
		return -EFAULT;

	/* Handle options that can be set without locking the socket. */
	switch (optname) {
	case TCP_SYNCNT:
		return tcp_sock_set_syncnt(sk, val);
	case TCP_USER_TIMEOUT:
		return tcp_sock_set_user_timeout(sk, val);
	case TCP_KEEPINTVL:
		return tcp_sock_set_keepintvl(sk, val);
	case TCP_KEEPCNT:
		return tcp_sock_set_keepcnt(sk, val);
	case TCP_LINGER2:
		if (val < 0)
			WRITE_ONCE(tp->linger2, -1);
		else if (val > TCP_FIN_TIMEOUT_MAX / HZ)
			WRITE_ONCE(tp->linger2, TCP_FIN_TIMEOUT_MAX);
		else
			WRITE_ONCE(tp->linger2, val * HZ);
		return 0;
	case TCP_DEFER_ACCEPT:
		/* Translate value in seconds to number of retransmits */
		WRITE_ONCE(icsk->icsk_accept_queue.rskq_defer_accept,
			   secs_to_retrans(val, TCP_TIMEOUT_INIT / HZ,
					   TCP_RTO_MAX / HZ));
		return 0;
	case TCP_RTO_MAX_MS:
		if (val < MSEC_PER_SEC || val > TCP_RTO_MAX_SEC * MSEC_PER_SEC)
			return -EINVAL;
		WRITE_ONCE(inet_csk(sk)->icsk_rto_max, msecs_to_jiffies(val));
		return 0;
	case TCP_RTO_MIN_US: {
		int rto_min = usecs_to_jiffies(val);

		if (rto_min > TCP_RTO_MIN || rto_min < TCP_TIMEOUT_MIN)
			return -EINVAL;
		WRITE_ONCE(inet_csk(sk)->icsk_rto_min, rto_min);
		return 0;
	}
	case TCP_DELACK_MAX_US: {
		int delack_max = usecs_to_jiffies(val);

		if (delack_max > TCP_DELACK_MAX || delack_max < TCP_TIMEOUT_MIN)
			return -EINVAL;
		WRITE_ONCE(inet_csk(sk)->icsk_delack_max, delack_max);
		return 0;
	}
	}

	sockopt_lock_sock(sk);

	switch (optname) {
	case TCP_MAXSEG:
		/* Values greater than interface MTU won't take effect. However
		 * at the point when this call is done we typically don't yet
		 * know which interface is going to be used
		 */
		if (val && (val < TCP_MIN_MSS || val > MAX_TCP_WINDOW)) {
			err = -EINVAL;
			break;
		}
		tp->rx_opt.user_mss = val;
		break;

	case TCP_NODELAY:
		__tcp_sock_set_nodelay(sk, val);
		break;

	case TCP_THIN_LINEAR_TIMEOUTS:
		if (val < 0 || val > 1)
			err = -EINVAL;
		else
			tp->thin_lto = val;
		break;

	case TCP_THIN_DUPACK:
		if (val < 0 || val > 1)
			err = -EINVAL;
		break;

	case TCP_REPAIR:
		if (!tcp_can_repair_sock(sk))
			err = -EPERM;
		else if (val == TCP_REPAIR_ON) {
			tp->repair = 1;
			sk->sk_reuse = SK_FORCE_REUSE;
			tp->repair_queue = TCP_NO_QUEUE;
		} else if (val == TCP_REPAIR_OFF) {
			tp->repair = 0;
			sk->sk_reuse = SK_NO_REUSE;
			tcp_send_window_probe(sk);
		} else if (val == TCP_REPAIR_OFF_NO_WP) {
			tp->repair = 0;
			sk->sk_reuse = SK_NO_REUSE;
		} else
			err = -EINVAL;

		break;

	case TCP_REPAIR_QUEUE:
		if (!tp->repair)
			err = -EPERM;
		else if ((unsigned int)val < TCP_QUEUES_NR)
			tp->repair_queue = val;
		else
			err = -EINVAL;
		break;

	case TCP_QUEUE_SEQ:
		if (sk->sk_state != TCP_CLOSE) {
			err = -EPERM;
		} else if (tp->repair_queue == TCP_SEND_QUEUE) {
			if (!tcp_rtx_queue_empty(sk))
				err = -EPERM;
			else
				WRITE_ONCE(tp->write_seq, val);
		} else if (tp->repair_queue == TCP_RECV_QUEUE) {
			if (tp->rcv_nxt != tp->copied_seq) {
				err = -EPERM;
			} else {
				WRITE_ONCE(tp->rcv_nxt, val);
				WRITE_ONCE(tp->copied_seq, val);
			}
		} else {
			err = -EINVAL;
		}
		break;

	case TCP_REPAIR_OPTIONS:
		if (!tp->repair)
			err = -EINVAL;
		else if (sk->sk_state == TCP_ESTABLISHED && !tp->bytes_sent)
			err = tcp_repair_options_est(sk, optval, optlen);
		else
			err = -EPERM;
		break;

	case TCP_CORK:
		__tcp_sock_set_cork(sk, val);
		break;

	case TCP_KEEPIDLE:
		err = tcp_sock_set_keepidle_locked(sk, val);
		break;
	case TCP_SAVE_SYN:
		/* 0: disable, 1: enable, 2: start from ether_header */
		if (val < 0 || val > 2)
			err = -EINVAL;
		else
			tp->save_syn = val;
		break;

	case TCP_WINDOW_CLAMP:
		err = tcp_set_window_clamp(sk, val);
		break;

	case TCP_QUICKACK:
		__tcp_sock_set_quickack(sk, val);
		break;

	case TCP_AO_REPAIR:
		if (!tcp_can_repair_sock(sk)) {
			err = -EPERM;
			break;
		}
		err = tcp_ao_set_repair(sk, optval, optlen);
		break;
#ifdef CONFIG_TCP_AO
	case TCP_AO_ADD_KEY:
	case TCP_AO_DEL_KEY:
	case TCP_AO_INFO: {
		/* If this is the first TCP-AO setsockopt() on the socket,
		 * sk_state has to be LISTEN or CLOSE. Allow TCP_REPAIR
		 * in any state.
		 */
		if ((1 << sk->sk_state) & (TCPF_LISTEN | TCPF_CLOSE))
			goto ao_parse;
		if (rcu_dereference_protected(tcp_sk(sk)->ao_info,
					      lockdep_sock_is_held(sk)))
			goto ao_parse;
		if (tp->repair)
			goto ao_parse;
		err = -EISCONN;
		break;
ao_parse:
		err = tp->af_specific->ao_parse(sk, optname, optval, optlen);
		break;
	}
#endif
#ifdef CONFIG_TCP_MD5SIG
	case TCP_MD5SIG:
	case TCP_MD5SIG_EXT:
		err = tp->af_specific->md5_parse(sk, optname, optval, optlen);
		break;
#endif
	case TCP_FASTOPEN:
		if (val >= 0 && ((1 << sk->sk_state) & (TCPF_CLOSE |
		    TCPF_LISTEN))) {
			tcp_fastopen_init_key_once(net);

			fastopen_queue_tune(sk, val);
		} else {
			err = -EINVAL;
		}
		break;
	case TCP_FASTOPEN_CONNECT:
		if (val > 1 || val < 0) {
			err = -EINVAL;
		} else if (READ_ONCE(net->ipv4.sysctl_tcp_fastopen) &
			   TFO_CLIENT_ENABLE) {
			if (sk->sk_state == TCP_CLOSE)
				tp->fastopen_connect = val;
			else
				err = -EINVAL;
		} else {
			err = -EOPNOTSUPP;
		}
		break;
	case TCP_FASTOPEN_NO_COOKIE:
		if (val > 1 || val < 0)
			err = -EINVAL;
		else if (!((1 << sk->sk_state) & (TCPF_CLOSE | TCPF_LISTEN)))
			err = -EINVAL;
		else
			tp->fastopen_no_cookie = val;
		break;
	case TCP_TIMESTAMP:
		if (!tp->repair) {
			err = -EPERM;
			break;
		}
		/* val is an opaque field,
		 * and low order bit contains usec_ts enable bit.
		 * Its a best effort, and we do not care if user makes an error.
		 */
		tp->tcp_usec_ts = val & 1;
		WRITE_ONCE(tp->tsoffset, val - tcp_clock_ts(tp->tcp_usec_ts));
		break;
	case TCP_REPAIR_WINDOW:
		err = tcp_repair_set_window(tp, optval, optlen);
		break;
	case TCP_NOTSENT_LOWAT:
		WRITE_ONCE(tp->notsent_lowat, val);
		sk->sk_write_space(sk);
		break;
	case TCP_INQ:
		if (val > 1 || val < 0)
			err = -EINVAL;
		else
			tp->recvmsg_inq = val;
		break;
	case TCP_TX_DELAY:
		if (val)
			tcp_enable_tx_delay();
		WRITE_ONCE(tp->tcp_tx_delay, val);
		break;
	default:
		err = -ENOPROTOOPT;
		break;
	}

	sockopt_release_sock(sk);
	return err;
}

int tcp_setsockopt(struct sock *sk, int level, int optname, sockptr_t optval,
		   unsigned int optlen)
{
	const struct inet_connection_sock *icsk = inet_csk(sk);

	if (level != SOL_TCP)
		/* Paired with WRITE_ONCE() in do_ipv6_setsockopt() and tcp_v6_connect() */
		return READ_ONCE(icsk->icsk_af_ops)->setsockopt(sk, level, optname,
								optval, optlen);
	return do_tcp_setsockopt(sk, level, optname, optval, optlen);
}
EXPORT_IPV6_MOD(tcp_setsockopt);

static void tcp_get_info_chrono_stats(const struct tcp_sock *tp,
				      struct tcp_info *info)
{
	u64 stats[__TCP_CHRONO_MAX], total = 0;
	enum tcp_chrono i;

	for (i = TCP_CHRONO_BUSY; i < __TCP_CHRONO_MAX; ++i) {
		stats[i] = tp->chrono_stat[i - 1];
		if (i == tp->chrono_type)
			stats[i] += tcp_jiffies32 - tp->chrono_start;
		stats[i] *= USEC_PER_SEC / HZ;
		total += stats[i];
	}

	info->tcpi_busy_time = total;
	info->tcpi_rwnd_limited = stats[TCP_CHRONO_RWND_LIMITED];
	info->tcpi_sndbuf_limited = stats[TCP_CHRONO_SNDBUF_LIMITED];
}

/* Return information about state of tcp endpoint in API format. */
void tcp_get_info(struct sock *sk, struct tcp_info *info)
{
	const struct tcp_sock *tp = tcp_sk(sk); /* iff sk_type == SOCK_STREAM */
	const struct inet_connection_sock *icsk = inet_csk(sk);
	unsigned long rate;
	u32 now;
	u64 rate64;
	bool slow;

	memset(info, 0, sizeof(*info));
	if (sk->sk_type != SOCK_STREAM)
		return;

	info->tcpi_state = inet_sk_state_load(sk);

	/* Report meaningful fields for all TCP states, including listeners */
	rate = READ_ONCE(sk->sk_pacing_rate);
	rate64 = (rate != ~0UL) ? rate : ~0ULL;
	info->tcpi_pacing_rate = rate64;

	rate = READ_ONCE(sk->sk_max_pacing_rate);
	rate64 = (rate != ~0UL) ? rate : ~0ULL;
	info->tcpi_max_pacing_rate = rate64;

	info->tcpi_reordering = tp->reordering;
	info->tcpi_snd_cwnd = tcp_snd_cwnd(tp);

	if (info->tcpi_state == TCP_LISTEN) {
		/* listeners aliased fields :
		 * tcpi_unacked -> Number of children ready for accept()
		 * tcpi_sacked  -> max backlog
		 */
		info->tcpi_unacked = READ_ONCE(sk->sk_ack_backlog);
		info->tcpi_sacked = READ_ONCE(sk->sk_max_ack_backlog);
		return;
	}

	slow = lock_sock_fast(sk);

	info->tcpi_ca_state = icsk->icsk_ca_state;
	info->tcpi_retransmits = icsk->icsk_retransmits;
	info->tcpi_probes = icsk->icsk_probes_out;
	info->tcpi_backoff = icsk->icsk_backoff;

	if (tp->rx_opt.tstamp_ok)
		info->tcpi_options |= TCPI_OPT_TIMESTAMPS;
	if (tcp_is_sack(tp))
		info->tcpi_options |= TCPI_OPT_SACK;
	if (tp->rx_opt.wscale_ok) {
		info->tcpi_options |= TCPI_OPT_WSCALE;
		info->tcpi_snd_wscale = tp->rx_opt.snd_wscale;
		info->tcpi_rcv_wscale = tp->rx_opt.rcv_wscale;
	}

	if (tcp_ecn_mode_any(tp))
		info->tcpi_options |= TCPI_OPT_ECN;
	if (tp->ecn_flags & TCP_ECN_SEEN)
		info->tcpi_options |= TCPI_OPT_ECN_SEEN;
	if (tp->ecn_flags & TCP_ECN_LOW)
		info->tcpi_options |= TCPI_OPT_ECN_LOW;
	if (tp->syn_data_acked)
		info->tcpi_options |= TCPI_OPT_SYN_DATA;
	if (tp->tcp_usec_ts)
		info->tcpi_options |= TCPI_OPT_USEC_TS;

	info->tcpi_rto = jiffies_to_usecs(icsk->icsk_rto);
	info->tcpi_ato = jiffies_to_usecs(min_t(u32, icsk->icsk_ack.ato,
						tcp_delack_max(sk)));
	info->tcpi_snd_mss = tp->mss_cache;
	info->tcpi_rcv_mss = icsk->icsk_ack.rcv_mss;

	info->tcpi_unacked = tp->packets_out;
	info->tcpi_sacked = tp->sacked_out;

	info->tcpi_lost = tp->lost_out;
	info->tcpi_retrans = tp->retrans_out;

	now = tcp_jiffies32;
	info->tcpi_last_data_sent = jiffies_to_msecs(now - tp->lsndtime);
	info->tcpi_last_data_recv = jiffies_to_msecs(now - icsk->icsk_ack.lrcvtime);
	info->tcpi_last_ack_recv = jiffies_to_msecs(now - tp->rcv_tstamp);

	info->tcpi_pmtu = icsk->icsk_pmtu_cookie;
	info->tcpi_rcv_ssthresh = tp->rcv_ssthresh;
	info->tcpi_rtt = tp->srtt_us >> 3;
	info->tcpi_rttvar = tp->mdev_us >> 2;
	info->tcpi_snd_ssthresh = tp->snd_ssthresh;
	info->tcpi_advmss = tp->advmss;

	info->tcpi_rcv_rtt = tp->rcv_rtt_est.rtt_us >> 3;
	info->tcpi_rcv_space = tp->rcvq_space.space;

	info->tcpi_total_retrans = tp->total_retrans;

	info->tcpi_bytes_acked = tp->bytes_acked;
	info->tcpi_bytes_received = tp->bytes_received;
	info->tcpi_notsent_bytes = max_t(int, 0, tp->write_seq - tp->snd_nxt);
	tcp_get_info_chrono_stats(tp, info);

	info->tcpi_segs_out = tp->segs_out;

	/* segs_in and data_segs_in can be updated from tcp_segs_in() from BH */
	info->tcpi_segs_in = READ_ONCE(tp->segs_in);
	info->tcpi_data_segs_in = READ_ONCE(tp->data_segs_in);

	info->tcpi_min_rtt = tcp_min_rtt(tp);
	info->tcpi_data_segs_out = tp->data_segs_out;

	info->tcpi_delivery_rate_app_limited = tp->rate_app_limited ? 1 : 0;
	rate64 = tcp_compute_delivery_rate(tp);
	if (rate64)
		info->tcpi_delivery_rate = rate64;
	info->tcpi_delivered = tp->delivered;
	info->tcpi_delivered_ce = tp->delivered_ce;
	info->tcpi_bytes_sent = tp->bytes_sent;
	info->tcpi_bytes_retrans = tp->bytes_retrans;
	info->tcpi_dsack_dups = tp->dsack_dups;
	info->tcpi_reord_seen = tp->reord_seen;
	info->tcpi_rcv_ooopack = tp->rcv_ooopack;
	info->tcpi_snd_wnd = tp->snd_wnd;
	info->tcpi_rcv_wnd = tp->rcv_wnd;
	info->tcpi_rehash = tp->plb_rehash + tp->timeout_rehash;
	info->tcpi_fastopen_client_fail = tp->fastopen_client_fail;

	info->tcpi_total_rto = tp->total_rto;
	info->tcpi_total_rto_recoveries = tp->total_rto_recoveries;
	info->tcpi_total_rto_time = tp->total_rto_time;
	if (tp->rto_stamp)
		info->tcpi_total_rto_time += tcp_clock_ms() - tp->rto_stamp;

	unlock_sock_fast(sk, slow);
}
EXPORT_SYMBOL_GPL(tcp_get_info);

static size_t tcp_opt_stats_get_size(void)
{
	return
		nla_total_size_64bit(sizeof(u64)) + /* TCP_NLA_BUSY */
		nla_total_size_64bit(sizeof(u64)) + /* TCP_NLA_RWND_LIMITED */
		nla_total_size_64bit(sizeof(u64)) + /* TCP_NLA_SNDBUF_LIMITED */
		nla_total_size_64bit(sizeof(u64)) + /* TCP_NLA_DATA_SEGS_OUT */
		nla_total_size_64bit(sizeof(u64)) + /* TCP_NLA_TOTAL_RETRANS */
		nla_total_size_64bit(sizeof(u64)) + /* TCP_NLA_PACING_RATE */
		nla_total_size_64bit(sizeof(u64)) + /* TCP_NLA_DELIVERY_RATE */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_SND_CWND */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_REORDERING */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_MIN_RTT */
		nla_total_size(sizeof(u8)) + /* TCP_NLA_RECUR_RETRANS */
		nla_total_size(sizeof(u8)) + /* TCP_NLA_DELIVERY_RATE_APP_LMT */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_SNDQ_SIZE */
		nla_total_size(sizeof(u8)) + /* TCP_NLA_CA_STATE */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_SND_SSTHRESH */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_DELIVERED */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_DELIVERED_CE */
		nla_total_size_64bit(sizeof(u64)) + /* TCP_NLA_BYTES_SENT */
		nla_total_size_64bit(sizeof(u64)) + /* TCP_NLA_BYTES_RETRANS */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_DSACK_DUPS */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_REORD_SEEN */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_SRTT */
		nla_total_size(sizeof(u16)) + /* TCP_NLA_TIMEOUT_REHASH */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_BYTES_NOTSENT */
		nla_total_size_64bit(sizeof(u64)) + /* TCP_NLA_EDT */
		nla_total_size(sizeof(u8)) + /* TCP_NLA_TTL */
		nla_total_size(sizeof(u32)) + /* TCP_NLA_REHASH */
		0;
}

/* Returns TTL or hop limit of an incoming packet from skb. */
static u8 tcp_skb_ttl_or_hop_limit(const struct sk_buff *skb)
{
	if (skb->protocol == htons(ETH_P_IP))
		return ip_hdr(skb)->ttl;
	else if (skb->protocol == htons(ETH_P_IPV6))
		return ipv6_hdr(skb)->hop_limit;
	else
		return 0;
}

struct sk_buff *tcp_get_timestamping_opt_stats(const struct sock *sk,
					       const struct sk_buff *orig_skb,
					       const struct sk_buff *ack_skb)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct sk_buff *stats;
	struct tcp_info info;
	unsigned long rate;
	u64 rate64;

	stats = alloc_skb(tcp_opt_stats_get_size(), GFP_ATOMIC);
	if (!stats)
		return NULL;

	tcp_get_info_chrono_stats(tp, &info);
	nla_put_u64_64bit(stats, TCP_NLA_BUSY,
			  info.tcpi_busy_time, TCP_NLA_PAD);
	nla_put_u64_64bit(stats, TCP_NLA_RWND_LIMITED,
			  info.tcpi_rwnd_limited, TCP_NLA_PAD);
	nla_put_u64_64bit(stats, TCP_NLA_SNDBUF_LIMITED,
			  info.tcpi_sndbuf_limited, TCP_NLA_PAD);
	nla_put_u64_64bit(stats, TCP_NLA_DATA_SEGS_OUT,
			  tp->data_segs_out, TCP_NLA_PAD);
	nla_put_u64_64bit(stats, TCP_NLA_TOTAL_RETRANS,
			  tp->total_retrans, TCP_NLA_PAD);

	rate = READ_ONCE(sk->sk_pacing_rate);
	rate64 = (rate != ~0UL) ? rate : ~0ULL;
	nla_put_u64_64bit(stats, TCP_NLA_PACING_RATE, rate64, TCP_NLA_PAD);

	rate64 = tcp_compute_delivery_rate(tp);
	nla_put_u64_64bit(stats, TCP_NLA_DELIVERY_RATE, rate64, TCP_NLA_PAD);

	nla_put_u32(stats, TCP_NLA_SND_CWND, tcp_snd_cwnd(tp));
	nla_put_u32(stats, TCP_NLA_REORDERING, tp->reordering);
	nla_put_u32(stats, TCP_NLA_MIN_RTT, tcp_min_rtt(tp));

	nla_put_u8(stats, TCP_NLA_RECUR_RETRANS, inet_csk(sk)->icsk_retransmits);
	nla_put_u8(stats, TCP_NLA_DELIVERY_RATE_APP_LMT, !!tp->rate_app_limited);
	nla_put_u32(stats, TCP_NLA_SND_SSTHRESH, tp->snd_ssthresh);
	nla_put_u32(stats, TCP_NLA_DELIVERED, tp->delivered);
	nla_put_u32(stats, TCP_NLA_DELIVERED_CE, tp->delivered_ce);

	nla_put_u32(stats, TCP_NLA_SNDQ_SIZE, tp->write_seq - tp->snd_una);
	nla_put_u8(stats, TCP_NLA_CA_STATE, inet_csk(sk)->icsk_ca_state);

	nla_put_u64_64bit(stats, TCP_NLA_BYTES_SENT, tp->bytes_sent,
			  TCP_NLA_PAD);
	nla_put_u64_64bit(stats, TCP_NLA_BYTES_RETRANS, tp->bytes_retrans,
			  TCP_NLA_PAD);
	nla_put_u32(stats, TCP_NLA_DSACK_DUPS, tp->dsack_dups);
	nla_put_u32(stats, TCP_NLA_REORD_SEEN, tp->reord_seen);
	nla_put_u32(stats, TCP_NLA_SRTT, tp->srtt_us >> 3);
	nla_put_u16(stats, TCP_NLA_TIMEOUT_REHASH, tp->timeout_rehash);
	nla_put_u32(stats, TCP_NLA_BYTES_NOTSENT,
		    max_t(int, 0, tp->write_seq - tp->snd_nxt));
	nla_put_u64_64bit(stats, TCP_NLA_EDT, orig_skb->skb_mstamp_ns,
			  TCP_NLA_PAD);
	if (ack_skb)
		nla_put_u8(stats, TCP_NLA_TTL,
			   tcp_skb_ttl_or_hop_limit(ack_skb));

	nla_put_u32(stats, TCP_NLA_REHASH, tp->plb_rehash + tp->timeout_rehash);
	return stats;
}

int do_tcp_getsockopt(struct sock *sk, int level,
		      int optname, sockptr_t optval, sockptr_t optlen)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct tcp_sock *tp = tcp_sk(sk);
	struct net *net = sock_net(sk);
	int val, len;

	if (copy_from_sockptr(&len, optlen, sizeof(int)))
		return -EFAULT;

	if (len < 0)
		return -EINVAL;

	len = min_t(unsigned int, len, sizeof(int));

	switch (optname) {
	case TCP_MAXSEG:
		val = tp->mss_cache;
		if (tp->rx_opt.user_mss &&
		    ((1 << sk->sk_state) & (TCPF_CLOSE | TCPF_LISTEN)))
			val = tp->rx_opt.user_mss;
		if (tp->repair)
			val = tp->rx_opt.mss_clamp;
		break;
	case TCP_NODELAY:
		val = !!(tp->nonagle&TCP_NAGLE_OFF);
		break;
	case TCP_CORK:
		val = !!(tp->nonagle&TCP_NAGLE_CORK);
		break;
	case TCP_KEEPIDLE:
		val = keepalive_time_when(tp) / HZ;
		break;
	case TCP_KEEPINTVL:
		val = keepalive_intvl_when(tp) / HZ;
		break;
	case TCP_KEEPCNT:
		val = keepalive_probes(tp);
		break;
	case TCP_SYNCNT:
		val = READ_ONCE(icsk->icsk_syn_retries) ? :
			READ_ONCE(net->ipv4.sysctl_tcp_syn_retries);
		break;
	case TCP_LINGER2:
		val = READ_ONCE(tp->linger2);
		if (val >= 0)
			val = (val ? : READ_ONCE(net->ipv4.sysctl_tcp_fin_timeout)) / HZ;
		break;
	case TCP_DEFER_ACCEPT:
		val = READ_ONCE(icsk->icsk_accept_queue.rskq_defer_accept);
		val = retrans_to_secs(val, TCP_TIMEOUT_INIT / HZ,
				      TCP_RTO_MAX / HZ);
		break;
	case TCP_WINDOW_CLAMP:
		val = READ_ONCE(tp->window_clamp);
		break;
	case TCP_INFO: {
		struct tcp_info info;

		if (copy_from_sockptr(&len, optlen, sizeof(int)))
			return -EFAULT;

		tcp_get_info(sk, &info);

		len = min_t(unsigned int, len, sizeof(info));
		if (copy_to_sockptr(optlen, &len, sizeof(int)))
			return -EFAULT;
		if (copy_to_sockptr(optval, &info, len))
			return -EFAULT;
		return 0;
	}
	case TCP_CC_INFO: {
		const struct tcp_congestion_ops *ca_ops;
		union tcp_cc_info info;
		size_t sz = 0;
		int attr;

		if (copy_from_sockptr(&len, optlen, sizeof(int)))
			return -EFAULT;

		ca_ops = icsk->icsk_ca_ops;
		if (ca_ops && ca_ops->get_info)
			sz = ca_ops->get_info(sk, ~0U, &attr, &info);

		len = min_t(unsigned int, len, sz);
		if (copy_to_sockptr(optlen, &len, sizeof(int)))
			return -EFAULT;
		if (copy_to_sockptr(optval, &info, len))
			return -EFAULT;
		return 0;
	}
	case TCP_QUICKACK:
		val = !inet_csk_in_pingpong_mode(sk);
		break;

	case TCP_CONGESTION:
		if (copy_from_sockptr(&len, optlen, sizeof(int)))
			return -EFAULT;
		len = min_t(unsigned int, len, TCP_CA_NAME_MAX);
		if (copy_to_sockptr(optlen, &len, sizeof(int)))
			return -EFAULT;
		if (copy_to_sockptr(optval, icsk->icsk_ca_ops->name, len))
			return -EFAULT;
		return 0;

	case TCP_ULP:
		if (copy_from_sockptr(&len, optlen, sizeof(int)))
			return -EFAULT;
		len = min_t(unsigned int, len, TCP_ULP_NAME_MAX);
		if (!icsk->icsk_ulp_ops) {
			len = 0;
			if (copy_to_sockptr(optlen, &len, sizeof(int)))
				return -EFAULT;
			return 0;
		}
		if (copy_to_sockptr(optlen, &len, sizeof(int)))
			return -EFAULT;
		if (copy_to_sockptr(optval, icsk->icsk_ulp_ops->name, len))
			return -EFAULT;
		return 0;

	case TCP_FASTOPEN_KEY: {
		u64 key[TCP_FASTOPEN_KEY_BUF_LENGTH / sizeof(u64)];
		unsigned int key_len;

		if (copy_from_sockptr(&len, optlen, sizeof(int)))
			return -EFAULT;

		key_len = tcp_fastopen_get_cipher(net, icsk, key) *
				TCP_FASTOPEN_KEY_LENGTH;
		len = min_t(unsigned int, len, key_len);
		if (copy_to_sockptr(optlen, &len, sizeof(int)))
			return -EFAULT;
		if (copy_to_sockptr(optval, key, len))
			return -EFAULT;
		return 0;
	}
	case TCP_THIN_LINEAR_TIMEOUTS:
		val = tp->thin_lto;
		break;

	case TCP_THIN_DUPACK:
		val = 0;
		break;

	case TCP_REPAIR:
		val = tp->repair;
		break;

	case TCP_REPAIR_QUEUE:
		if (tp->repair)
			val = tp->repair_queue;
		else
			return -EINVAL;
		break;

	case TCP_REPAIR_WINDOW: {
		struct tcp_repair_window opt;

		if (copy_from_sockptr(&len, optlen, sizeof(int)))
			return -EFAULT;

		if (len != sizeof(opt))
			return -EINVAL;

		if (!tp->repair)
			return -EPERM;

		opt.snd_wl1	= tp->snd_wl1;
		opt.snd_wnd	= tp->snd_wnd;
		opt.max_window	= tp->max_window;
		opt.rcv_wnd	= tp->rcv_wnd;
		opt.rcv_wup	= tp->rcv_wup;

		if (copy_to_sockptr(optval, &opt, len))
			return -EFAULT;
		return 0;
	}
	case TCP_QUEUE_SEQ:
		if (tp->repair_queue == TCP_SEND_QUEUE)
			val = tp->write_seq;
		else if (tp->repair_queue == TCP_RECV_QUEUE)
			val = tp->rcv_nxt;
		else
			return -EINVAL;
		break;

	case TCP_USER_TIMEOUT:
		val = READ_ONCE(icsk->icsk_user_timeout);
		break;

	case TCP_FASTOPEN:
		val = READ_ONCE(icsk->icsk_accept_queue.fastopenq.max_qlen);
		break;

	case TCP_FASTOPEN_CONNECT:
		val = tp->fastopen_connect;
		break;

	case TCP_FASTOPEN_NO_COOKIE:
		val = tp->fastopen_no_cookie;
		break;

	case TCP_TX_DELAY:
		val = READ_ONCE(tp->tcp_tx_delay);
		break;

	case TCP_TIMESTAMP:
		val = tcp_clock_ts(tp->tcp_usec_ts) + READ_ONCE(tp->tsoffset);
		if (tp->tcp_usec_ts)
			val |= 1;
		else
			val &= ~1;
		break;
	case TCP_NOTSENT_LOWAT:
		val = READ_ONCE(tp->notsent_lowat);
		break;
	case TCP_INQ:
		val = tp->recvmsg_inq;
		break;
	case TCP_SAVE_SYN:
		val = tp->save_syn;
		break;
	case TCP_SAVED_SYN: {
		if (copy_from_sockptr(&len, optlen, sizeof(int)))
			return -EFAULT;

		sockopt_lock_sock(sk);
		if (tp->saved_syn) {
			if (len < tcp_saved_syn_len(tp->saved_syn)) {
				len = tcp_saved_syn_len(tp->saved_syn);
				if (copy_to_sockptr(optlen, &len, sizeof(int))) {
					sockopt_release_sock(sk);
					return -EFAULT;
				}
				sockopt_release_sock(sk);
				return -EINVAL;
			}
			len = tcp_saved_syn_len(tp->saved_syn);
			if (copy_to_sockptr(optlen, &len, sizeof(int))) {
				sockopt_release_sock(sk);
				return -EFAULT;
			}
			if (copy_to_sockptr(optval, tp->saved_syn->data, len)) {
				sockopt_release_sock(sk);
				return -EFAULT;
			}
			tcp_saved_syn_free(tp);
			sockopt_release_sock(sk);
		} else {
			sockopt_release_sock(sk);
			len = 0;
			if (copy_to_sockptr(optlen, &len, sizeof(int)))
				return -EFAULT;
		}
		return 0;
	}
#ifdef CONFIG_MMU
	case TCP_ZEROCOPY_RECEIVE: {
		struct scm_timestamping_internal tss;
		struct tcp_zerocopy_receive zc = {};
		int err;

		if (copy_from_sockptr(&len, optlen, sizeof(int)))
			return -EFAULT;
		if (len < 0 ||
		    len < offsetofend(struct tcp_zerocopy_receive, length))
			return -EINVAL;
		if (unlikely(len > sizeof(zc))) {
			err = check_zeroed_sockptr(optval, sizeof(zc),
						   len - sizeof(zc));
			if (err < 1)
				return err == 0 ? -EINVAL : err;
			len = sizeof(zc);
			if (copy_to_sockptr(optlen, &len, sizeof(int)))
				return -EFAULT;
		}
		if (copy_from_sockptr(&zc, optval, len))
			return -EFAULT;
		if (zc.reserved)
			return -EINVAL;
		if (zc.msg_flags &  ~(TCP_VALID_ZC_MSG_FLAGS))
			return -EINVAL;
		sockopt_lock_sock(sk);
		err = tcp_zerocopy_receive(sk, &zc, &tss);
		err = BPF_CGROUP_RUN_PROG_GETSOCKOPT_KERN(sk, level, optname,
							  &zc, &len, err);
		sockopt_release_sock(sk);
		if (len >= offsetofend(struct tcp_zerocopy_receive, msg_flags))
			goto zerocopy_rcv_cmsg;
		switch (len) {
		case offsetofend(struct tcp_zerocopy_receive, msg_flags):
			goto zerocopy_rcv_cmsg;
		case offsetofend(struct tcp_zerocopy_receive, msg_controllen):
		case offsetofend(struct tcp_zerocopy_receive, msg_control):
		case offsetofend(struct tcp_zerocopy_receive, flags):
		case offsetofend(struct tcp_zerocopy_receive, copybuf_len):
		case offsetofend(struct tcp_zerocopy_receive, copybuf_address):
		case offsetofend(struct tcp_zerocopy_receive, err):
			goto zerocopy_rcv_sk_err;
		case offsetofend(struct tcp_zerocopy_receive, inq):
			goto zerocopy_rcv_inq;
		case offsetofend(struct tcp_zerocopy_receive, length):
		default:
			goto zerocopy_rcv_out;
		}
zerocopy_rcv_cmsg:
		if (zc.msg_flags & TCP_CMSG_TS)
			tcp_zc_finalize_rx_tstamp(sk, &zc, &tss);
		else
			zc.msg_flags = 0;
zerocopy_rcv_sk_err:
		if (!err)
			zc.err = sock_error(sk);
zerocopy_rcv_inq:
		zc.inq = tcp_inq_hint(sk);
zerocopy_rcv_out:
		if (!err && copy_to_sockptr(optval, &zc, len))
			err = -EFAULT;
		return err;
	}
#endif
	case TCP_AO_REPAIR:
		if (!tcp_can_repair_sock(sk))
			return -EPERM;
		return tcp_ao_get_repair(sk, optval, optlen);
	case TCP_AO_GET_KEYS:
	case TCP_AO_INFO: {
		int err;

		sockopt_lock_sock(sk);
		if (optname == TCP_AO_GET_KEYS)
			err = tcp_ao_get_mkts(sk, optval, optlen);
		else
			err = tcp_ao_get_sock_info(sk, optval, optlen);
		sockopt_release_sock(sk);

		return err;
	}
	case TCP_IS_MPTCP:
		val = 0;
		break;
	case TCP_RTO_MAX_MS:
		val = jiffies_to_msecs(tcp_rto_max(sk));
		break;
	case TCP_RTO_MIN_US:
		val = jiffies_to_usecs(READ_ONCE(inet_csk(sk)->icsk_rto_min));
		break;
	case TCP_DELACK_MAX_US:
		val = jiffies_to_usecs(READ_ONCE(inet_csk(sk)->icsk_delack_max));
		break;
	default:
		return -ENOPROTOOPT;
	}

	if (copy_to_sockptr(optlen, &len, sizeof(int)))
		return -EFAULT;
	if (copy_to_sockptr(optval, &val, len))
		return -EFAULT;
	return 0;
}

bool tcp_bpf_bypass_getsockopt(int level, int optname)
{
	/* TCP do_tcp_getsockopt has optimized getsockopt implementation
	 * to avoid extra socket lock for TCP_ZEROCOPY_RECEIVE.
	 */
	if (level == SOL_TCP && optname == TCP_ZEROCOPY_RECEIVE)
		return true;

	return false;
}
EXPORT_IPV6_MOD(tcp_bpf_bypass_getsockopt);

int tcp_getsockopt(struct sock *sk, int level, int optname, char __user *optval,
		   int __user *optlen)
{
	struct inet_connection_sock *icsk = inet_csk(sk);

	if (level != SOL_TCP)
		/* Paired with WRITE_ONCE() in do_ipv6_setsockopt() and tcp_v6_connect() */
		return READ_ONCE(icsk->icsk_af_ops)->getsockopt(sk, level, optname,
								optval, optlen);
	return do_tcp_getsockopt(sk, level, optname, USER_SOCKPTR(optval),
				 USER_SOCKPTR(optlen));
}
EXPORT_IPV6_MOD(tcp_getsockopt);

#ifdef CONFIG_TCP_MD5SIG
int tcp_md5_sigpool_id = -1;
EXPORT_IPV6_MOD_GPL(tcp_md5_sigpool_id);

int tcp_md5_alloc_sigpool(void)
{
	size_t scratch_size;
	int ret;

	scratch_size = sizeof(union tcp_md5sum_block) + sizeof(struct tcphdr);
	ret = tcp_sigpool_alloc_ahash("md5", scratch_size);
	if (ret >= 0) {
		/* As long as any md5 sigpool was allocated, the return
		 * id would stay the same. Re-write the id only for the case
		 * when previously all MD5 keys were deleted and this call
		 * allocates the first MD5 key, which may return a different
		 * sigpool id than was used previously.
		 */
		WRITE_ONCE(tcp_md5_sigpool_id, ret); /* Avoids the compiler potentially being smart here */
		return 0;
	}
	return ret;
}

void tcp_md5_release_sigpool(void)
{
	tcp_sigpool_release(READ_ONCE(tcp_md5_sigpool_id));
}

void tcp_md5_add_sigpool(void)
{
	tcp_sigpool_get(READ_ONCE(tcp_md5_sigpool_id));
}

int tcp_md5_hash_key(struct tcp_sigpool *hp,
		     const struct tcp_md5sig_key *key)
{
	u8 keylen = READ_ONCE(key->keylen); /* paired with WRITE_ONCE() in tcp_md5_do_add */
	struct scatterlist sg;

	sg_init_one(&sg, key->key, keylen);
	ahash_request_set_crypt(hp->req, &sg, NULL, keylen);

	/* We use data_race() because tcp_md5_do_add() might change
	 * key->key under us
	 */
	return data_race(crypto_ahash_update(hp->req));
}
EXPORT_IPV6_MOD(tcp_md5_hash_key);

/* Called with rcu_read_lock() */
static enum skb_drop_reason
tcp_inbound_md5_hash(const struct sock *sk, const struct sk_buff *skb,
		     const void *saddr, const void *daddr,
		     int family, int l3index, const __u8 *hash_location)
{
	/* This gets called for each TCP segment that has TCP-MD5 option.
	 * We have 3 drop cases:
	 * o No MD5 hash and one expected.
	 * o MD5 hash and we're not expecting one.
	 * o MD5 hash and its wrong.
	 */
	const struct tcp_sock *tp = tcp_sk(sk);
	struct tcp_md5sig_key *key;
	u8 newhash[16];
	int genhash;

	key = tcp_md5_do_lookup(sk, l3index, saddr, family);

	if (!key && hash_location) {
		NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPMD5UNEXPECTED);
		trace_tcp_hash_md5_unexpected(sk, skb);
		return SKB_DROP_REASON_TCP_MD5UNEXPECTED;
	}

	/* Check the signature.
	 * To support dual stack listeners, we need to handle
	 * IPv4-mapped case.
	 */
	if (family == AF_INET)
		genhash = tcp_v4_md5_hash_skb(newhash, key, NULL, skb);
	else
		genhash = tp->af_specific->calc_md5_hash(newhash, key,
							 NULL, skb);
	if (genhash || memcmp(hash_location, newhash, 16) != 0) {
		NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPMD5FAILURE);
		trace_tcp_hash_md5_mismatch(sk, skb);
		return SKB_DROP_REASON_TCP_MD5FAILURE;
	}
	return SKB_NOT_DROPPED_YET;
}
#else
static inline enum skb_drop_reason
tcp_inbound_md5_hash(const struct sock *sk, const struct sk_buff *skb,
		     const void *saddr, const void *daddr,
		     int family, int l3index, const __u8 *hash_location)
{
	return SKB_NOT_DROPPED_YET;
}

#endif

/* Called with rcu_read_lock() */
enum skb_drop_reason
tcp_inbound_hash(struct sock *sk, const struct request_sock *req,
		 const struct sk_buff *skb,
		 const void *saddr, const void *daddr,
		 int family, int dif, int sdif)
{
	const struct tcphdr *th = tcp_hdr(skb);
	const struct tcp_ao_hdr *aoh;
	const __u8 *md5_location;
	int l3index;

	/* Invalid option or two times meet any of auth options */
	if (tcp_parse_auth_options(th, &md5_location, &aoh)) {
		trace_tcp_hash_bad_header(sk, skb);
		return SKB_DROP_REASON_TCP_AUTH_HDR;
	}

	if (req) {
		if (tcp_rsk_used_ao(req) != !!aoh) {
			u8 keyid, rnext, maclen;

			if (aoh) {
				keyid = aoh->keyid;
				rnext = aoh->rnext_keyid;
				maclen = tcp_ao_hdr_maclen(aoh);
			} else {
				keyid = rnext = maclen = 0;
			}

			NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPAOBAD);
			trace_tcp_ao_handshake_failure(sk, skb, keyid, rnext, maclen);
			return SKB_DROP_REASON_TCP_AOFAILURE;
		}
	}

	/* sdif set, means packet ingressed via a device
	 * in an L3 domain and dif is set to the l3mdev
	 */
	l3index = sdif ? dif : 0;

	/* Fast path: unsigned segments */
	if (likely(!md5_location && !aoh)) {
		/* Drop if there's TCP-MD5 or TCP-AO key with any rcvid/sndid
		 * for the remote peer. On TCP-AO established connection
		 * the last key is impossible to remove, so there's
		 * always at least one current_key.
		 */
		if (tcp_ao_required(sk, saddr, family, l3index, true)) {
			trace_tcp_hash_ao_required(sk, skb);
			return SKB_DROP_REASON_TCP_AONOTFOUND;
		}
		if (unlikely(tcp_md5_do_lookup(sk, l3index, saddr, family))) {
			NET_INC_STATS(sock_net(sk), LINUX_MIB_TCPMD5NOTFOUND);
			trace_tcp_hash_md5_required(sk, skb);
			return SKB_DROP_REASON_TCP_MD5NOTFOUND;
		}
		return SKB_NOT_DROPPED_YET;
	}

	if (aoh)
		return tcp_inbound_ao_hash(sk, skb, family, req, l3index, aoh);

	return tcp_inbound_md5_hash(sk, skb, saddr, daddr, family,
				    l3index, md5_location);
}
EXPORT_IPV6_MOD_GPL(tcp_inbound_hash);

void tcp_done(struct sock *sk)
{
	struct request_sock *req;

	/* We might be called with a new socket, after
	 * inet_csk_prepare_forced_close() has been called
	 * so we can not use lockdep_sock_is_held(sk)
	 */
	req = rcu_dereference_protected(tcp_sk(sk)->fastopen_rsk, 1);

	if (sk->sk_state == TCP_SYN_SENT || sk->sk_state == TCP_SYN_RECV)
		TCP_INC_STATS(sock_net(sk), TCP_MIB_ATTEMPTFAILS);

	tcp_set_state(sk, TCP_CLOSE);
	tcp_clear_xmit_timers(sk);
	if (req)
		reqsk_fastopen_remove(sk, req, false);

	WRITE_ONCE(sk->sk_shutdown, SHUTDOWN_MASK);

	if (!sock_flag(sk, SOCK_DEAD))
		sk->sk_state_change(sk);
	else
		inet_csk_destroy_sock(sk);
}
EXPORT_SYMBOL_GPL(tcp_done);

int tcp_abort(struct sock *sk, int err)
{
	int state = inet_sk_state_load(sk);

	if (state == TCP_NEW_SYN_RECV) {
		struct request_sock *req = inet_reqsk(sk);

		local_bh_disable();
		inet_csk_reqsk_queue_drop(req->rsk_listener, req);
		local_bh_enable();
		return 0;
	}
	if (state == TCP_TIME_WAIT) {
		struct inet_timewait_sock *tw = inet_twsk(sk);

		refcount_inc(&tw->tw_refcnt);
		local_bh_disable();
		inet_twsk_deschedule_put(tw);
		local_bh_enable();
		return 0;
	}

	/* BPF context ensures sock locking. */
	if (!has_current_bpf_ctx())
		/* Don't race with userspace socket closes such as tcp_close. */
		lock_sock(sk);

	/* Avoid closing the same socket twice. */
	if (sk->sk_state == TCP_CLOSE) {
		if (!has_current_bpf_ctx())
			release_sock(sk);
		return -ENOENT;
	}

	if (sk->sk_state == TCP_LISTEN) {
		tcp_set_state(sk, TCP_CLOSE);
		inet_csk_listen_stop(sk);
	}

	/* Don't race with BH socket closes such as inet_csk_listen_stop. */
	local_bh_disable();
	bh_lock_sock(sk);

	if (tcp_need_reset(sk->sk_state))
		tcp_send_active_reset(sk, GFP_ATOMIC,
				      SK_RST_REASON_TCP_STATE);
	tcp_done_with_error(sk, err);

	bh_unlock_sock(sk);
	local_bh_enable();
	if (!has_current_bpf_ctx())
		release_sock(sk);
	return 0;
}
EXPORT_SYMBOL_GPL(tcp_abort);

extern struct tcp_congestion_ops tcp_reno;

static __initdata unsigned long thash_entries;
static int __init set_thash_entries(char *str)
{
	ssize_t ret;

	if (!str)
		return 0;

	ret = kstrtoul(str, 0, &thash_entries);
	if (ret)
		return 0;

	return 1;
}
__setup("thash_entries=", set_thash_entries);

static void __init tcp_init_mem(void)
{
	unsigned long limit = nr_free_buffer_pages() / 16;

	limit = max(limit, 128UL);
	sysctl_tcp_mem[0] = limit / 4 * 3;		/* 4.68 % */
	sysctl_tcp_mem[1] = limit;			/* 6.25 % */
	sysctl_tcp_mem[2] = sysctl_tcp_mem[0] * 2;	/* 9.37 % */
}

static void __init tcp_struct_check(void)
{
	/* TX read-mostly hotpath cache lines */
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_tx, max_window);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_tx, rcv_ssthresh);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_tx, reordering);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_tx, notsent_lowat);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_tx, gso_segs);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_tx, lost_skb_hint);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_tx, retransmit_skb_hint);
	CACHELINE_ASSERT_GROUP_SIZE(struct tcp_sock, tcp_sock_read_tx, 40);

	/* TXRX read-mostly hotpath cache lines */
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_txrx, tsoffset);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_txrx, snd_wnd);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_txrx, mss_cache);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_txrx, snd_cwnd);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_txrx, prr_out);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_txrx, lost_out);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_txrx, sacked_out);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_txrx, scaling_ratio);
	CACHELINE_ASSERT_GROUP_SIZE(struct tcp_sock, tcp_sock_read_txrx, 32);

	/* RX read-mostly hotpath cache lines */
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, copied_seq);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, rcv_tstamp);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, snd_wl1);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, tlp_high_seq);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, rttvar_us);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, retrans_out);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, advmss);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, urg_data);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, lost);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, rtt_min);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, out_of_order_queue);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, snd_ssthresh);
#if IS_ENABLED(CONFIG_TLS_DEVICE)
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_read_rx, tcp_clean_acked);
	CACHELINE_ASSERT_GROUP_SIZE(struct tcp_sock, tcp_sock_read_rx, 77);
#else
	CACHELINE_ASSERT_GROUP_SIZE(struct tcp_sock, tcp_sock_read_rx, 69);
#endif

	/* TX read-write hotpath cache lines */
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, segs_out);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, data_segs_out);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, bytes_sent);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, snd_sml);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, chrono_start);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, chrono_stat);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, write_seq);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, pushed_seq);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, lsndtime);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, mdev_us);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, tcp_wstamp_ns);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, rtt_seq);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, tsorted_sent_queue);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, highest_sack);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_tx, ecn_flags);
	CACHELINE_ASSERT_GROUP_SIZE(struct tcp_sock, tcp_sock_write_tx, 89);

	/* TXRX read-write hotpath cache lines */
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, pred_flags);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, tcp_clock_cache);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, tcp_mstamp);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, rcv_nxt);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, snd_nxt);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, snd_una);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, window_clamp);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, srtt_us);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, packets_out);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, snd_up);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, delivered);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, delivered_ce);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, app_limited);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, rcv_wnd);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_txrx, rx_opt);

	/* 32bit arches with 8byte alignment on u64 fields might need padding
	 * before tcp_clock_cache.
	 */
	CACHELINE_ASSERT_GROUP_SIZE(struct tcp_sock, tcp_sock_write_txrx, 92 + 4);

	/* RX read-write hotpath cache lines */
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_rx, bytes_received);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_rx, segs_in);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_rx, data_segs_in);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_rx, rcv_wup);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_rx, max_packets_out);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_rx, cwnd_usage_seq);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_rx, rate_delivered);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_rx, rate_interval_us);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_rx, rcv_rtt_last_tsecr);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_rx, first_tx_mstamp);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_rx, delivered_mstamp);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_rx, bytes_acked);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_rx, rcv_rtt_est);
	CACHELINE_ASSERT_GROUP_MEMBER(struct tcp_sock, tcp_sock_write_rx, rcvq_space);
	CACHELINE_ASSERT_GROUP_SIZE(struct tcp_sock, tcp_sock_write_rx, 99);
}

void __init tcp_init(void)
{
	int max_rshare, max_wshare, cnt;
	unsigned long limit;
	unsigned int i;

	BUILD_BUG_ON(TCP_MIN_SND_MSS <= MAX_TCP_OPTION_SPACE);
	BUILD_BUG_ON(sizeof(struct tcp_skb_cb) >
		     sizeof_field(struct sk_buff, cb));

	tcp_struct_check();

	percpu_counter_init(&tcp_sockets_allocated, 0, GFP_KERNEL);

	timer_setup(&tcp_orphan_timer, tcp_orphan_update, TIMER_DEFERRABLE);
	mod_timer(&tcp_orphan_timer, jiffies + TCP_ORPHAN_TIMER_PERIOD);

	inet_hashinfo2_init(&tcp_hashinfo, "tcp_listen_portaddr_hash",
			    thash_entries, 21,  /* one slot per 2 MB*/
			    0, 64 * 1024);
	tcp_hashinfo.bind_bucket_cachep =
		kmem_cache_create("tcp_bind_bucket",
				  sizeof(struct inet_bind_bucket), 0,
				  SLAB_HWCACHE_ALIGN | SLAB_PANIC |
				  SLAB_ACCOUNT,
				  NULL);
	tcp_hashinfo.bind2_bucket_cachep =
		kmem_cache_create("tcp_bind2_bucket",
				  sizeof(struct inet_bind2_bucket), 0,
				  SLAB_HWCACHE_ALIGN | SLAB_PANIC |
				  SLAB_ACCOUNT,
				  NULL);

	/* Size and allocate the main established and bind bucket
	 * hash tables.
	 *
	 * The methodology is similar to that of the buffer cache.
	 */
	tcp_hashinfo.ehash =
		alloc_large_system_hash("TCP established",
					sizeof(struct inet_ehash_bucket),
					thash_entries,
					17, /* one slot per 128 KB of memory */
					0,
					NULL,
					&tcp_hashinfo.ehash_mask,
					0,
					thash_entries ? 0 : 512 * 1024);
	for (i = 0; i <= tcp_hashinfo.ehash_mask; i++)
		INIT_HLIST_NULLS_HEAD(&tcp_hashinfo.ehash[i].chain, i);

	if (inet_ehash_locks_alloc(&tcp_hashinfo))
		panic("TCP: failed to alloc ehash_locks");
	tcp_hashinfo.bhash =
		alloc_large_system_hash("TCP bind",
					2 * sizeof(struct inet_bind_hashbucket),
					tcp_hashinfo.ehash_mask + 1,
					17, /* one slot per 128 KB of memory */
					0,
					&tcp_hashinfo.bhash_size,
					NULL,
					0,
					64 * 1024);
	tcp_hashinfo.bhash_size = 1U << tcp_hashinfo.bhash_size;
	tcp_hashinfo.bhash2 = tcp_hashinfo.bhash + tcp_hashinfo.bhash_size;
	for (i = 0; i < tcp_hashinfo.bhash_size; i++) {
		spin_lock_init(&tcp_hashinfo.bhash[i].lock);
		INIT_HLIST_HEAD(&tcp_hashinfo.bhash[i].chain);
		spin_lock_init(&tcp_hashinfo.bhash2[i].lock);
		INIT_HLIST_HEAD(&tcp_hashinfo.bhash2[i].chain);
	}

	tcp_hashinfo.pernet = false;

	cnt = tcp_hashinfo.ehash_mask + 1;
	sysctl_tcp_max_orphans = cnt / 2;

	tcp_init_mem();
	/* Set per-socket limits to no more than 1/128 the pressure threshold */
	limit = nr_free_buffer_pages() << (PAGE_SHIFT - 7);
	max_wshare = min(4UL*1024*1024, limit);
	max_rshare = min(6UL*1024*1024, limit);

	init_net.ipv4.sysctl_tcp_wmem[0] = PAGE_SIZE;
	init_net.ipv4.sysctl_tcp_wmem[1] = 16*1024;
	init_net.ipv4.sysctl_tcp_wmem[2] = max(64*1024, max_wshare);

	init_net.ipv4.sysctl_tcp_rmem[0] = PAGE_SIZE;
	init_net.ipv4.sysctl_tcp_rmem[1] = 131072;
	init_net.ipv4.sysctl_tcp_rmem[2] = max(131072, max_rshare);

	pr_info("Hash tables configured (established %u bind %u)\n",
		tcp_hashinfo.ehash_mask + 1, tcp_hashinfo.bhash_size);

	tcp_v4_init();
	tcp_metrics_init();
	BUG_ON(tcp_register_congestion_control(&tcp_reno) != 0);
	tcp_tasklet_init();
	mptcp_init();
}
