/*
 * main.c - standalone AF_XDP socket and UMEM initialization probe
 *
 * This milestone sets up packet storage and rings. It does not attach an XDP
 * program, receive/transmit packets, or communicate with a Caladan runtime.
 */

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <net/if.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>

#include <xdp/xsk.h>

#ifndef SOL_XDP
#define SOL_XDP 283
#endif

#define RING_SIZE	2048U
#define FRAME_SIZE	4096U
#define FRAME_COUNT	(2U * RING_SIZE)
#define FRAME_HEADROOM	128U
#define UMEM_SIZE	((size_t)FRAME_SIZE * FRAME_COUNT)

struct options {
	const char *ifname;
	uint32_t queue_id;
	bool zero_copy;
};

struct probe {
	void *buffer;
	struct xsk_umem *umem;
	struct xsk_socket *socket;
	struct xsk_ring_prod fill;
	struct xsk_ring_cons completion;
	struct xsk_ring_cons rx;
	struct xsk_ring_prod tx;
};

static void usage(FILE *out, const char *program)
{
	fprintf(out,
		"Usage: %s --interface IFACE [--queue ID] [--mode copy|zero-copy]\n"
		"\n"
		"  -i, --interface IFACE  Network interface (required)\n"
		"  -q, --queue ID         Hardware RX queue ID (default: 0)\n"
		"  -m, --mode MODE        Explicit transfer mode (default: copy)\n"
		"  -h, --help             Show this help\n"
		"\n"
		"Initializes one socket, UMEM, and four rings; waits for SIGINT/SIGTERM.\n"
		"XDP program loading and packet processing are not implemented yet.\n",
		program);
}

static int parse_queue_id(const char *value, uint32_t *queue_id)
{
	unsigned long long parsed;
	char *end;

	/* Reject signs and whitespace as well as overflow and trailing text. */
	if (!value[0] || strspn(value, "0123456789") != strlen(value))
		return -EINVAL;

	errno = 0;
	parsed = strtoull(value, &end, 10);
	if (errno || *end || parsed > UINT32_MAX)
		return -EINVAL;

	*queue_id = (uint32_t)parsed;
	return 0;
}

/* Returns 1 for help, 0 for valid options, or a negative error. */
static int parse_options(int argc, char **argv, struct options *opts)
{
	static const struct option long_options[] = {
		{"interface", required_argument, NULL, 'i'},
		{"queue", required_argument, NULL, 'q'},
		{"mode", required_argument, NULL, 'm'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};
	int opt;

	opterr = 0;
	while ((opt = getopt_long(argc, argv, "i:q:m:h", long_options,
				  NULL)) != -1) {
		switch (opt) {
		case 'i':
			opts->ifname = optarg;
			break;
		case 'q':
			if (parse_queue_id(optarg, &opts->queue_id)) {
				fprintf(stderr, "Invalid queue ID: %s\n", optarg);
				return -EINVAL;
			}
			break;
		case 'm':
			if (!strcmp(optarg, "copy")) {
				opts->zero_copy = false;
			} else if (!strcmp(optarg, "zero-copy")) {
				opts->zero_copy = true;
			} else {
				fprintf(stderr, "Invalid mode: %s\n", optarg);
				return -EINVAL;
			}
			break;
		case 'h':
			return 1;
		default:
			fprintf(stderr, "Unknown option or missing option argument\n");
			return -EINVAL;
		}
	}

	if (!opts->ifname || !opts->ifname[0] ||
	    strlen(opts->ifname) >= IFNAMSIZ || optind != argc) {
		fprintf(stderr, "Provide an interface name and no positional arguments\n");
		return -EINVAL;
	}

	return 0;
}

static void raise_memlock_limit(void)
{
	const struct rlimit limit = {RLIM_INFINITY, RLIM_INFINITY};

	/* Older kernels charge UMEM against this limit; newer ones may not. */
	if (setrlimit(RLIMIT_MEMLOCK, &limit))
		fprintf(stderr, "Could not raise locked-memory limit: %s; "
			"continuing with the current limit\n", strerror(errno));
}

static int allocate_umem(struct probe *p)
{
	const struct xsk_umem_config config = {
		.fill_size = RING_SIZE,
		.comp_size = RING_SIZE,
		.frame_size = FRAME_SIZE,
		.frame_headroom = FRAME_HEADROOM,
		.flags = 0,
	};

	p->buffer = mmap(NULL, UMEM_SIZE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
	if (p->buffer == MAP_FAILED) {
		p->buffer = NULL;
		return -errno;
	}

	return xsk_umem__create(&p->umem, p->buffer, UMEM_SIZE, &p->fill,
				&p->completion, &config);
}

static int create_socket(struct probe *p, const struct options *opts)
{
	const struct xsk_socket_config config = {
		.rx_size = RING_SIZE,
		.tx_size = RING_SIZE,
		/* Attaching a redirect program belongs to the next milestone. */
		.libxdp_flags = XSK_LIBXDP_FLAGS__INHIBIT_PROG_LOAD,
		.xdp_flags = 0,
		.bind_flags = XDP_USE_NEED_WAKEUP |
			(opts->zero_copy ? XDP_ZEROCOPY : XDP_COPY),
	};

	return xsk_socket__create(&p->socket, opts->ifname, opts->queue_id,
				  p->umem, &p->rx, &p->tx, &config);
}

static int verify_mode(struct probe *p, const struct options *opts)
{
	struct xdp_options actual = {0};
	socklen_t len = sizeof(actual);
	bool zero_copy;

	if (getsockopt(xsk_socket__fd(p->socket), SOL_XDP, XDP_OPTIONS,
		       &actual, &len))
		return -errno;
	if (len != sizeof(actual))
		return -EIO;

	zero_copy = (actual.flags & XDP_OPTIONS_ZEROCOPY) != 0;
	return zero_copy == opts->zero_copy ? 0 : -EIO;
}

static int supply_receive_frames(struct probe *p)
{
	uint32_t index, i;

	if (xsk_ring_prod__reserve(&p->fill, RING_SIZE, &index) != RING_SIZE)
		return -ENOSPC;

	/* Publish each frame once. Leave the other half available for future TX. */
	for (i = 0; i < RING_SIZE; i++)
		*xsk_ring_prod__fill_addr(&p->fill, index + i) =
			(uint64_t)i * FRAME_SIZE;
	xsk_ring_prod__submit(&p->fill, RING_SIZE);
	return 0;
}

static int cleanup(struct probe *p)
{
	int ret;

	/* The socket must release its UMEM reference before deleting the UMEM. */
	if (p->socket) {
		xsk_socket__delete(p->socket);
		p->socket = NULL;
	}
	if (p->umem) {
		ret = xsk_umem__delete(p->umem);
		if (ret)
			return ret;
		p->umem = NULL;
	}
	if (p->buffer) {
		if (munmap(p->buffer, UMEM_SIZE))
			return -errno;
		p->buffer = NULL;
	}
	return 0;
}

int main(int argc, char **argv)
{
	struct options opts = {0};
	struct probe p = {0};
	const char *stage;
	sigset_t signals;
	int ret, cleanup_ret, signal_number;

	ret = parse_options(argc, argv, &opts);
	if (ret) {
		usage(ret > 0 ? stdout : stderr, argv[0]);
		return ret > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	if (!if_nametoindex(opts.ifname)) {
		fprintf(stderr, "Cannot find interface %s: %s\n", opts.ifname,
			strerror(errno));
		return EXIT_FAILURE;
	}

	/* Block before setup so SIGINT/SIGTERM also lead to cleanup during startup. */
	sigemptyset(&signals);
	sigaddset(&signals, SIGINT);
	sigaddset(&signals, SIGTERM);
	if (sigprocmask(SIG_BLOCK, &signals, NULL)) {
		perror("Block termination signals");
		return EXIT_FAILURE;
	}

	raise_memlock_limit();
	stage = "Allocate/register UMEM and configure Fill/Completion rings";
	ret = allocate_umem(&p);
	if (ret)
		goto out;

	stage = "Create/bind socket and configure RX/TX rings";
	ret = create_socket(&p, &opts);
	if (ret)
		goto out;

	stage = "Verify selected AF_XDP mode";
	ret = verify_mode(&p, &opts);
	if (ret)
		goto out;

	stage = "Supply receive frames";
	ret = supply_receive_frames(&p);
	if (ret)
		goto out;

	printf("AF_XDP initialization succeeded\n"
	       "  interface: %s\n"
	       "  queue: %" PRIu32 "\n"
	       "  verified mode: %s\n"
	       "  UMEM: %zu bytes, %u frames of %u bytes\n"
	       "  frame headroom: %u bytes\n"
	       "  rings: Fill/RX/TX/Completion = %u descriptors each\n"
	       "  published RX frames: %u; remaining frames: %u\n"
	       "  XDP program loading: disabled\n"
	       "Packet processing is not implemented. Press Ctrl+C to clean up.\n",
	       opts.ifname, opts.queue_id, opts.zero_copy ? "zero-copy" : "copy",
	       UMEM_SIZE, FRAME_COUNT, FRAME_SIZE, FRAME_HEADROOM, RING_SIZE,
	       RING_SIZE, FRAME_COUNT - RING_SIZE);
	fflush(stdout);

	stage = "Wait for termination signal";
	ret = sigwait(&signals, &signal_number);
	if (ret)
		ret = -ret;
	else
		printf("Received signal %d; releasing AF_XDP resources\n",
		       signal_number);

out:
	if (ret)
		fprintf(stderr, "%s: %s\n", stage, strerror(-ret));
	cleanup_ret = cleanup(&p);
	if (cleanup_ret)
		fprintf(stderr, "Release AF_XDP resources: %s\n",
			strerror(-cleanup_ret));
	return ret || cleanup_ret ? EXIT_FAILURE : EXIT_SUCCESS;
}
