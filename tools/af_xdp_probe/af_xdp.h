/* AF_XDP resources and their initialization/cleanup interface. */

#pragma once

#include <xdp/xsk.h>

struct probe_options;

struct af_xdp {
	void *buffer;
	struct xsk_umem *umem;
	struct xsk_socket *socket;
	struct xsk_ring_prod fill;
	struct xsk_ring_cons completion;
	struct xsk_ring_cons rx;
	struct xsk_ring_prod tx;
};

/* Pass zero-initialized state. Cleanup is required even if init fails. */
int af_xdp_init(struct af_xdp *io, const struct probe_options *opts);
int af_xdp_cleanup(struct af_xdp *io);
void af_xdp_print_config(const struct probe_options *opts);
