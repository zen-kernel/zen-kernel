#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <assert.h>
#include <poll.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "kdbus-util.h"
#include "kdbus-enum.h"

#define SERVICE_NAME "foo.bar.echo"

static char stress_payload[8192];

struct stats {
	uint64_t count;
	uint64_t latency_acc;
	uint64_t latency_low;
	uint64_t latency_high;
};

static struct stats stats;

static uint64_t
timeval_diff(const struct timeval *hi, const struct timeval *lo)
{
	struct timeval r;

	timersub(hi, lo, &r);

	return (uint64_t) (r.tv_sec * 1000000ULL) +
		(uint64_t) r.tv_usec;
}

static void reset_stats(void)
{
	stats.count = 0;
	stats.latency_acc = 0;
	stats.latency_low = UINT64_MAX;
	stats.latency_high = 0;
}

static void dump_stats(void)
{
	if (stats.count > 0) {
		printf("stats: %llu packets processed, latency (usecs) min/max/avg %llu/%llu/%llu\n",
			(unsigned long long) stats.count,
			(unsigned long long) stats.latency_low,
			(unsigned long long) stats.latency_high,
			(unsigned long long) (stats.latency_acc / stats.count));
	} else {
		printf("*** no packets received. bus stuck?\n");
	}
}

static void add_stats(const struct timeval *tv)
{
	struct timeval now;
	uint64_t diff;

	gettimeofday(&now, NULL);
	diff = timeval_diff(&now, tv);

	stats.count++;
	stats.latency_acc += diff;
	if (stats.latency_low > diff)
		stats.latency_low = diff;

	if (stats.latency_high < diff)
		stats.latency_high = diff;
}

static int
send_echo_request(struct conn *conn, uint64_t dst_id)
{
	struct kdbus_msg *msg;
	struct kdbus_item *item;
	uint64_t size;
	int memfd = -1;
	int ret;
	struct timeval now;

	gettimeofday(&now, NULL);

	size = sizeof(struct kdbus_msg);
	size += KDBUS_ITEM_SIZE(sizeof(struct kdbus_vec));

	ret = ioctl(conn->fd, KDBUS_CMD_MEMFD_NEW, &memfd);
	if (ret < 0) {
		fprintf(stderr, "KDBUS_CMD_MEMFD_NEW failed: %m\n");
		return EXIT_FAILURE;
	}

	if (write(memfd, &now, sizeof(now)) != sizeof(now)) {
		fprintf(stderr, "writing to memfd failed: %m\n");
		return EXIT_FAILURE;
	}

	ret = ioctl(memfd, KDBUS_CMD_MEMFD_SEAL_SET, true);
	if (ret < 0) {
		fprintf(stderr, "memfd sealing failed: %m\n");
		return EXIT_FAILURE;
	}

	size += KDBUS_ITEM_SIZE(sizeof(struct kdbus_memfd));

	msg = malloc(size);
	if (!msg) {
		fprintf(stderr, "unable to malloc()!?\n");
		return EXIT_FAILURE;
	}

	memset(msg, 0, size);
	msg->size = size;
	msg->src_id = conn->id;
	msg->dst_id = dst_id;
	msg->payload_type = KDBUS_PAYLOAD_DBUS;

	item = msg->items;

	item->type = KDBUS_ITEM_PAYLOAD_VEC;
	item->size = KDBUS_ITEM_HEADER_SIZE + sizeof(struct kdbus_vec);
	item->vec.address = (uintptr_t) stress_payload;
	item->vec.size = sizeof(stress_payload);
	item = KDBUS_ITEM_NEXT(item);

	item->type = KDBUS_ITEM_PAYLOAD_MEMFD;
	item->size = KDBUS_ITEM_HEADER_SIZE + sizeof(struct kdbus_memfd);
	item->memfd.size = sizeof(struct timeval);
	item->memfd.fd = memfd;
	item = KDBUS_ITEM_NEXT(item);

	ret = ioctl(conn->fd, KDBUS_CMD_MSG_SEND, msg);
	if (ret) {
		fprintf(stderr, "error sending message: %d err %d (%m)\n", ret, errno);
		return EXIT_FAILURE;
	}

	if (memfd >= 0)
		close(memfd);
	free(msg);

	return 0;
}

static int
handle_echo_reply(struct conn *conn)
{
	int ret;
	uint64_t off;
	struct kdbus_msg *msg;
	const struct kdbus_item *item;

	ret = ioctl(conn->fd, KDBUS_CMD_MSG_RECV, &off);
	if (ret < 0) {
		fprintf(stderr, "error receiving message: %d (%m)\n", ret);
		return EXIT_FAILURE;
	}

	msg = (struct kdbus_msg *)(conn->buf + off);
	item = msg->items;

	KDBUS_ITEM_FOREACH(item, msg, items) {
		switch (item->type) {
		case KDBUS_ITEM_PAYLOAD_MEMFD: {
			char *buf;

			buf = mmap(NULL, item->memfd.size, PROT_READ, MAP_SHARED, item->memfd.fd, 0);
			if (buf == MAP_FAILED) {
				printf("mmap() fd=%i failed: %m", item->memfd.fd);
				break;
			}

			add_stats((struct timeval *) buf);
			munmap(buf, item->memfd.size);
			close(item->memfd.fd);
			break;
		}

		case KDBUS_ITEM_PAYLOAD_OFF: {
			/* ignore */
			break;
		}
		}
	}

	ret = ioctl(conn->fd, KDBUS_CMD_FREE, &off);
	if (ret < 0) {
		fprintf(stderr, "error free message: %d (%m)\n", ret);
		return EXIT_FAILURE;
	}

	return 0;
}

int main(int argc, char *argv[])
{
	struct {
		struct kdbus_cmd_make head;

		/* bloom size item */
		struct {
			uint64_t size;
			uint64_t type;
			uint64_t bloom_size;
		} bs;

		/* name item */
		uint64_t n_size;
		uint64_t n_type;
		char name[64];
	} bus_make;
	int fdc, ret;
	char *bus;
	struct conn *conn_a;
	struct conn *conn_b;
	struct pollfd fds[2];
	struct timeval start;
	unsigned int i;

	for (i = 0; i < sizeof(stress_payload); i++)
		stress_payload[i] = i;

	printf("-- opening /dev/" KBUILD_MODNAME "/control\n");
	fdc = open("/dev/" KBUILD_MODNAME "/control", O_RDWR|O_CLOEXEC);
	if (fdc < 0) {
		fprintf(stderr, "--- error %d (%m)\n", fdc);
		return EXIT_FAILURE;
	}

	memset(&bus_make, 0, sizeof(bus_make));
	bus_make.bs.size = sizeof(bus_make.bs);
	bus_make.bs.type = KDBUS_ITEM_BLOOM_SIZE;
	bus_make.bs.bloom_size = 64;

	snprintf(bus_make.name, sizeof(bus_make.name), "%u-testbus", getuid());
	bus_make.n_type = KDBUS_ITEM_MAKE_NAME;
	bus_make.n_size = KDBUS_ITEM_HEADER_SIZE + strlen(bus_make.name) + 1;

	bus_make.head.size = sizeof(struct kdbus_cmd_make) +
			     sizeof(bus_make.bs) +
			     bus_make.n_size;

	printf("-- creating bus '%s'\n", bus_make.name);
	ret = ioctl(fdc, KDBUS_CMD_BUS_MAKE, &bus_make);
	if (ret) {
		fprintf(stderr, "--- error %d (%m)\n", ret);
		return EXIT_FAILURE;
	}

	if (asprintf(&bus, "/dev/" KBUILD_MODNAME "/%s/bus", bus_make.name) < 0)
		return EXIT_FAILURE;

	conn_a = connect_to_bus(bus, 0);
	if (!conn_a)
		return EXIT_FAILURE;

	conn_b = connect_to_bus(bus, 0);
	if (!conn_b)
		return EXIT_FAILURE;

	upload_policy(conn_a->fd, SERVICE_NAME);

	add_match_empty(conn_a->fd);
	add_match_empty(conn_b->fd);

	fds[0].fd = conn_a->fd;
	fds[1].fd = conn_b->fd;

	name_acquire(conn_a, SERVICE_NAME, 0);

	gettimeofday(&start, NULL);
	reset_stats();

	ret = send_echo_request(conn_b, conn_a->id);
	if (ret)
		return EXIT_FAILURE;

	printf("-- entering poll loop ...\n");

	while (1) {
		struct timeval now;
		unsigned int nfds = sizeof(fds) / sizeof(fds[0]);
		unsigned int i;

		for (i = 0; i < nfds; i++) {
			fds[i].events = POLLIN | POLLPRI | POLLHUP;
			fds[i].revents = 0;
		}

		ret = poll(fds, nfds, 10);
		if (ret < 0)
			break;

		if (fds[0].revents & POLLIN) {
			ret = handle_echo_reply(conn_a);
			if (ret)
				break;

			ret = send_echo_request(conn_b, conn_a->id);
			if (ret)
				break;
		}

		gettimeofday(&now, NULL);
		if (timeval_diff(&now, &start) / 1000ULL > 1000ULL) {
			start.tv_sec = now.tv_sec;
			start.tv_usec = now.tv_usec;
			dump_stats();
			reset_stats();
		}
	}

	printf("-- closing bus connections\n");

	close(conn_a->fd);
	close(conn_b->fd);

	free(conn_a);
	free(conn_b);

	printf("-- closing bus master\n");
	close(fdc);
	free(bus);

	return EXIT_SUCCESS;
}
