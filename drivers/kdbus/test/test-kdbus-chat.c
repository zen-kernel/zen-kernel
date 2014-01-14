#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <assert.h>
#include <poll.h>
#include <sys/ioctl.h>

#include "kdbus-util.h"
#include "kdbus-enum.h"

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
	int fdc, ret, cookie;
	char *bus;
	struct conn *conn_a, *conn_b;
	struct pollfd fds[2];
	int count;
	int r;

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
	conn_b = connect_to_bus(bus, 0);
	if (!conn_a || !conn_b)
		return EXIT_FAILURE;

	r = upload_policy(conn_a->fd, "foo.bar.test");
	if (r < 0)
		return EXIT_FAILURE;
	r = upload_policy(conn_a->fd, "foo.bar.baz");
	if (r < 0)
		return EXIT_FAILURE;
	r = upload_policy(conn_a->fd, "foo.bar.double");
	if (r < 0)
		return EXIT_FAILURE;

	r = name_acquire(conn_a, "foo.bar.test", KDBUS_NAME_ALLOW_REPLACEMENT);
	if (r < 0)
		return EXIT_FAILURE;
	r = name_acquire(conn_a, "foo.bar.baz", 0);
	if (r < 0)
		return EXIT_FAILURE;
	r = name_acquire(conn_b, "foo.bar.baz", KDBUS_NAME_QUEUE);
	if (r < 0)
		return EXIT_FAILURE;

	r = name_acquire(conn_a, "foo.bar.double", 0);
	if (r < 0)
		return EXIT_FAILURE;
	r = name_acquire(conn_a, "foo.bar.double", 0);
	if (r != -EALREADY)
		return EXIT_FAILURE;

	r = name_release(conn_a, "foo.bar.double");
	if (r < 0)
		return EXIT_FAILURE;
	r = name_release(conn_a, "foo.bar.double");
	if (r != -ESRCH)
		return EXIT_FAILURE;

	name_list(conn_b, KDBUS_NAME_LIST_UNIQUE|
			  KDBUS_NAME_LIST_NAMES|
			  KDBUS_NAME_LIST_QUEUED|
			  KDBUS_NAME_LIST_ACTIVATORS);

	add_match_empty(conn_a->fd);
	add_match_empty(conn_b->fd);

	cookie = 0;
	msg_send(conn_b, NULL, 0xc0000000 | cookie, KDBUS_DST_ID_BROADCAST);

	fds[0].fd = conn_a->fd;
	fds[1].fd = conn_b->fd;

	printf("-- entering poll loop ...\n");

	for (count = 0;; count++) {
		int i, nfds = sizeof(fds) / sizeof(fds[0]);

		for (i = 0; i < nfds; i++) {
			fds[i].events = POLLIN | POLLPRI | POLLHUP;
			fds[i].revents = 0;
		}

		ret = poll(fds, nfds, 3000);
		if (ret <= 0)
			break;

		if (fds[0].revents & POLLIN) {
			if (count > 2)
				name_release(conn_a, "foo.bar.baz");

			msg_recv(conn_a);
			msg_send(conn_a, NULL, 0xc0000000 | cookie++, conn_b->id);
		}

		if (fds[1].revents & POLLIN) {
			msg_recv(conn_b);
			msg_send(conn_b, NULL, 0xc0000000 | cookie++, conn_a->id);
		}

		name_list(conn_b, KDBUS_NAME_LIST_UNIQUE|
				  KDBUS_NAME_LIST_NAMES|
				  KDBUS_NAME_LIST_QUEUED|
				  KDBUS_NAME_LIST_ACTIVATORS);

		if (count > 10)
			break;
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
