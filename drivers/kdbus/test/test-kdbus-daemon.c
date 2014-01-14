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

		uint64_t n_size;
		uint64_t n_type;
		char name[64];
	} bus_make;
	int fd_owner;
	char *bus;
	struct conn *conn;
	struct pollfd fds[2];
	int count;
	int ret;

	printf("Starting Test Bus Daemon (press ENTER to exit)\n");

	fd_owner = open("/dev/" KBUILD_MODNAME "/control", O_RDWR|O_CLOEXEC);
	if (fd_owner < 0) {
		fprintf(stderr, "/dev/" KBUILD_MODNAME "/control: %m\n");
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

	ret = ioctl(fd_owner, KDBUS_CMD_BUS_MAKE, &bus_make);
	if (ret) {
		fprintf(stderr, "KDBUS_CMD_BUS_MAKE: %m\n");
		return EXIT_FAILURE;
	}
	printf("  Created bus '%s'\n", bus_make.name);

	if (asprintf(&bus, "/dev/" KBUILD_MODNAME "/%s/bus", bus_make.name) < 0)
		return EXIT_FAILURE;

	conn = connect_to_bus(bus, 0);
	if (!conn)
		return EXIT_FAILURE;
	printf("  Created connection %llu on bus '%s'\n", (unsigned long long)conn->id, bus_make.name);

	upload_policy(conn->fd, "com.example.kdbus-test");
	name_acquire(conn, "com.example.kdbus-test", 0);
	printf("  Aquired name: com.example.kdbus-test\n");

	fds[0].fd = conn->fd;
	fds[1].fd = STDIN_FILENO;

	printf("  Monitoring connections:\n");

	for (count = 0;; count++) {
		int i, nfds = sizeof(fds) / sizeof(fds[0]);

		for (i = 0; i < nfds; i++) {
			fds[i].events = POLLIN | POLLPRI | POLLHUP;
			fds[i].revents = 0;
		}

		ret = poll(fds, nfds, -1);
		if (ret <= 0)
			break;

		if (fds[0].revents & POLLIN)
			msg_recv(conn);

		/* stdin */
		if (fds[1].revents & POLLIN)
			break;
	}

	printf("  Closing bus connection\n");
	close(conn->fd);
	free(conn);

	printf("  Closing bus\n");
	close(fd_owner);
	free(bus);

	return EXIT_SUCCESS;
}
