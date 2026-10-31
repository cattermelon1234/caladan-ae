This standalone probe implements the first AF_XDP initialization milestone in
[the backend design](../../AF_XDP_BACKEND_DESIGN.md). It allocates/registers
UMEM, configures four rings, binds one socket to an interface/queue, publishes
empty receive frames, verifies the transfer mode, and waits for SIGINT or
SIGTERM before releasing resources.

Build on Linux with a C compiler, `make`, `pkg-config`, and development packages
for libxdp and its dependencies (including libbpf headers). For Debian/Ubuntu
releases providing them, the development packages are `libxdp-dev` and
`libbpf-dev`. This tool builds independently of Caladan and DPDK:

```sh
make -C tools/af_xdp_probe
tools/af_xdp_probe/af_xdp_probe --help
sudo tools/af_xdp_probe/af_xdp_probe --interface eth0 --queue 0 --mode copy
```

Replace `eth0` with the Linux experiment interface and use a valid queue ID.
Queue 0 and explicit copy mode are the defaults. To require zero-copy support:

```sh
sudo tools/af_xdp_probe/af_xdp_probe --interface eth0 --queue 0 --mode zero-copy
```

The program requests the chosen mode without fallback and checks `XDP_OPTIONS`
after binding. Unsupported modes or invalid queues fail with a stage-specific
error. UMEM registration may also fail if locked-memory limits or privileges
are insufficient; the probe attempts to raise its own limit.

The fixed initial configuration is 4,096 frames of 4,096 bytes (16 MiB), with
128 bytes of configured application headroom. Each ring has 2,048 descriptors.
The Fill ring initially receives the first 2,048 distinct frame addresses; the
remaining half of UMEM is left available for future transmit work.

**This version only tests initialization.** It explicitly inhibits libxdp's
automatic XDP program loading, does not attach/detach a redirect program, and
does not modify an XSKMAP. It has no packet receive, recycle, or transmit loop
and does not communicate with Caladan. Successfully binding a socket does not
establish packet reception. Existing interface XDP attachments are left alone.

On a supported Linux host, the initialization check is:

1. Build and run `--help`; malformed options should exit unsuccessfully.
2. Start on the selected interface/queue and check the reported configuration.
3. Press Ctrl+C or send SIGTERM; expect exit status zero after cleanup.
4. Repeat initialization on the same queue to check that resources were released.
5. Try an invalid interface/queue and confirm a useful error and unsuccessful exit.

Linux compilation, socket binding, and kernel resource cleanup have not been
verified on a Linux host in this session. The development workspace is macOS;
the Makefile reports that AF_XDP requires Linux there.

Host-side checks compiled the C source with warnings treated as errors against
a temporary AF_XDP API shim. They exercised help/invalid options, injected UMEM,
socket, mode-query, and Fill-reservation failures, and checked cleanup order on
SIGINT/SIGTERM for both requested modes. This checks application control flow;
it does not substitute for compiling against Linux/libxdp or testing the kernel.

The next milestone is an XDP redirect program plus XSKMAP setup, followed by an
RX loop that inspects packets, recycles frames, and handles required wakeups.

The implementation uses the public
[libxdp AF_XDP API](https://github.com/xdp-project/xdp-tools/blob/main/headers/xdp/xsk.h).
Ring ownership, queue binding, and mode behavior are described in the
[Linux AF_XDP documentation](https://docs.kernel.org/networking/af_xdp.html).
