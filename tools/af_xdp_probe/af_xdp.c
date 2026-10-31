/*
 * af_xdp.c - UMEM allocation, socket/ring setup, and resource cleanup
 */

#include <errno.h>
#include <inttypes.h>
#include <net/if.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>

#include "af_xdp.h"
#include "options.h"

#ifndef SOL_XDP
#define SOL_XDP 283
#endif

#define RING_SIZE	2048U
#define FRAME_SIZE	4096U
#define FRAME_COUNT	(2U * RING_SIZE)
#define FRAME_HEADROOM	128U
#define UMEM_SIZE	((size_t)FRAME_SIZE * FRAME_COUNT)

static void raise_memlock_limit(void)
{
	const struct rlimit limit = {RLIM_INFINITY, RLIM_INFINITY};

	/* Older kernels charge UMEM against this limit; newer ones may not. */
	if (setrlimit(RLIMIT_MEMLOCK, &limit))
		fprintf(stderr, "Could not raise locked-memory limit: %s; "
			"continuing with the current limit\n", strerror(errno));
}

static int allocate_umem(struct af_xdp *p)
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

static int create_socket(struct af_xdp *p, const struct probe_options *opts)
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

static int verify_mode(struct af_xdp *p, const struct probe_options *opts)
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

static int supply_receive_frames(struct af_xdp *p)
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

int af_xdp_cleanup(struct af_xdp *p)
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

int af_xdp_init(struct af_xdp *p, const struct probe_options *opts)
{
	const char *stage;
	int ret;

	if (!if_nametoindex(opts->ifname)) {
		ret = -errno;
		fprintf(stderr, "Cannot find interface %s: %s\n", opts->ifname,
			strerror(-ret));
		return ret;
	}

	raise_memlock_limit();
	stage = "Allocate/register UMEM and configure Fill/Completion rings";
	ret = allocate_umem(p);
	if (ret)
		goto fail;

	stage = "Create/bind socket and configure RX/TX rings";
	ret = create_socket(p, opts);
	if (ret)
		goto fail;

	stage = "Verify selected AF_XDP mode";
	ret = verify_mode(p, opts);
	if (ret)
		goto fail;

	stage = "Supply receive frames";
	ret = supply_receive_frames(p);
	if (ret)
		goto fail;

	return 0;

fail:
	fprintf(stderr, "%s: %s\n", stage, strerror(-ret));
	return ret;
}

void af_xdp_print_config(const struct probe_options *opts)
{
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
	       opts->ifname, opts->queue_id, opts->zero_copy ? "zero-copy" : "copy",
	       UMEM_SIZE, FRAME_COUNT, FRAME_SIZE, FRAME_HEADROOM, RING_SIZE,
	       RING_SIZE, FRAME_COUNT - RING_SIZE);
	fflush(stdout);
}
