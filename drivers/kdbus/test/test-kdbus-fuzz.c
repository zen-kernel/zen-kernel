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

static unsigned int ioctl_cmds[] = {
	KDBUS_CMD_BUS_MAKE,
	KDBUS_CMD_NS_MAKE,
	KDBUS_CMD_EP_MAKE,
	KDBUS_CMD_HELLO,
	KDBUS_CMD_MSG_SEND,
	KDBUS_CMD_MSG_RECV,
	KDBUS_CMD_NAME_ACQUIRE,
	KDBUS_CMD_NAME_RELEASE,
	KDBUS_CMD_NAME_LIST,
	KDBUS_CMD_CONN_INFO,
	KDBUS_CMD_MATCH_ADD,
	KDBUS_CMD_MATCH_REMOVE,
	KDBUS_CMD_EP_POLICY_SET,
};

static const char *ioctl_name(unsigned int ioctl)
{
	switch(ioctl) {
	case KDBUS_CMD_BUS_MAKE:
		return "BUS_MAKE";
	case KDBUS_CMD_NS_MAKE:
		return "NS_MAKE";
	case KDBUS_CMD_EP_MAKE:
		return "EP_MAKE";
	case KDBUS_CMD_HELLO:
		return "HELLO";
	case KDBUS_CMD_MSG_SEND:
		return "MSG_SEND";
	case KDBUS_CMD_MSG_RECV:
		return "MSG_RECV";
	case KDBUS_CMD_NAME_ACQUIRE:
		return "NAME_ACQUIRE";
	case KDBUS_CMD_NAME_RELEASE:
		return "NAME_RELEASE";
	case KDBUS_CMD_NAME_LIST:
		return "NAME_LIST";
	case KDBUS_CMD_CONN_INFO:
		return "NAME_INFO";
	case KDBUS_CMD_MATCH_ADD:
		return "MATCH_ADD";
	case KDBUS_CMD_MATCH_REMOVE:
		return "MATCH_REMOVE";
	case KDBUS_CMD_EP_POLICY_SET:
		return "EP_POLICY_SET";
	default:
		return "unknown";
	}
}

static int fd_table[100] = { -1 };

static void add_fd(int fd)
{
	unsigned int i;

	for (i = 0; i < ELEMENTSOF(fd_table); i++)
		if (fd_table[i] == -1)  {
			fd_table[i] = fd;
			return;
		}
}

static int make_bus(void)
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
	char name[10];
	char *bus;
	unsigned int i;
	int ret, fdc;

	printf("-- opening /dev/" KBUILD_MODNAME "/control\n");
	fdc = open("/dev/" KBUILD_MODNAME "/control", O_RDWR|O_CLOEXEC);
	if (fdc < 0) {
		fprintf(stderr, "--- error %d (%m)\n", fdc);
		return EXIT_FAILURE;
	}

	add_fd(fdc);

	memset(name, 0, sizeof(name));

	for(i = 0; i < sizeof(name) - 1; i++)
		name[i] =( random() % ('z' - 'a')) + 'a';

	memset(&bus_make, 0, sizeof(bus_make));
	snprintf(bus_make.name, sizeof(bus_make.name), "%u-%s", getuid(), name);
	bus_make.head.flags = KDBUS_MAKE_ACCESS_WORLD;
	bus_make.head.size = sizeof(struct kdbus_cmd_make) + strlen(bus_make.name) + 1;

	bus_make.bs.size = sizeof(bus_make.bs);
	bus_make.bs.type = KDBUS_ITEM_BLOOM_SIZE;
	bus_make.bs.bloom_size = 64;

	printf("-- creating bus '%s'\n", bus_make.name);
	ret = ioctl(fdc, KDBUS_CMD_BUS_MAKE, &bus_make);
	if (ret) {
		fprintf(stderr, "--- error %d (%m)\n", ret);
		return EXIT_FAILURE;
	}

	if (asprintf(&bus, "/dev/" KBUILD_MODNAME "/%s/bus", bus_make.name) < 0)
		return EXIT_FAILURE;

	for (ret = 0; ret < random() % 20; ret++) {
		struct conn *conn = connect_to_bus(bus, 0);
		if (conn)
			add_fd(conn->fd);
	}

	return 0;
}

static int get_random_fd(void)
{
	unsigned int i, count = 0;

	for (i = 0; i < ELEMENTSOF(fd_table); i++)
		if (fd_table[i] != -1)
			count++;

	if (count > 0)
		count = random() % count;

	for (i = 0; i < ELEMENTSOF(fd_table); i++)
		if (fd_table[i] != -1)
			if (count-- == 0)
				return fd_table[i];

	return -1;
}

static void close_random_fd(void)
{
	unsigned int i, count = 0;

	for (i = 0; i < ELEMENTSOF(fd_table); i++)
		if (fd_table[i] != -1)
			count++;

	if (count > 0)
		count = random() % count;

	for (i = 0; i < ELEMENTSOF(fd_table); i++)
		if (fd_table[i] != -1)
			if (count-- == 0) {
				close(fd_table[i]);
				fd_table[i] = -1;
				return;
			}
}

int main(int argc, char *argv[])
{
	unsigned int i;

	srandom(time(NULL));

	for (i = 0; i < ELEMENTSOF(fd_table); i++)
		fd_table[i] = -1;

	make_bus();
	make_bus();

	while(1) {
		char buf[0xffff];
		int fd = get_random_fd();
		int cmd = ioctl_cmds[random() % ELEMENTSOF(ioctl_cmds)];
		int ret;

		if (random() % 1000 == 0)
			make_bus();

		if (random() % 1000 == 0)
			close_random_fd();

		for (i = 0; i < sizeof(buf); i++)
			buf[i] = random();

		errno = 0;
		ret = ioctl(fd, cmd, buf);
		printf(" ioctl(%13s) on fd %d returned\t%d\t(%m)\n",
			ioctl_name(cmd), fd, ret);
	}

	return EXIT_SUCCESS;
}
