# AF_XDP backend project: persistent session context

Recorded October 19, 2026. Read this file first after context loss, then check
the current working tree and relevant source files before continuing. This is
a snapshot of project intent and source inspection, not proof of runtime behavior.

## Current instruction and work status

The user asked to ingest the project description and codebase and write this
summary for future context recovery. **Do not begin implementation or experiments
until the user requests further work.** Only this document was created during
ingestion; no source fixes, builds, dependency installation, or experiments ran.

Repository HEAD at inspection: `4a254bf` (`README update`). No AGENTS.md was
found under the parent directory. Follow CONTRIBUTING.md for coding conventions.
There was a pre-existing uncommitted change in `iokernel/commands.c`: the first
line is `i/*` instead of `/*`. It appears to be a syntax error; it was deliberately
left untouched. Do not overwrite user changes.

No AF_XDP backend or standalone AF_XDP prototype was found in this checkout.
Milestones below describe intended progress, not completed work.

## Research objective

Build/extend project (B2): add AF_XDP packet I/O to Caladan while keeping its
userspace TCP/IP stack and runtime mostly unchanged. Compare Linux TCP sockets,
Caladan + AF_XDP, and Caladan + existing DPDK to investigate how much performance
comes from packet I/O versus the rest of Caladan. AF_XDP's Linux integration
also motivates evaluating deployment practicality.

Start with one NIC queue and one Caladan runtime, establish RX and TX end to end,
then batch ring operations. The planned backend creates an AF_XDP socket,
registers UMEM, manages RX/TX/fill/completion rings, and attaches an XDP redirect
program for the selected NIC queue. Map received packets to Caladan's existing
network abstractions, transmit packets from its stack, and recycle buffers
correctly. Prefer AF_XDP zero-copy where hardware supports it; copy mode is a
valid initial deliverable and an additional comparison. No exact integration
design or AF_XDP library choice has been approved or implemented yet.

## Repository map

- `iokernel/`: dedicated dataplane core, runtime registration, packet steering,
  scheduler policies (`simple`, `ias`, `numa`), and CPU allocation.
- `runtime/`: userspace threads, scheduling, preemption, memory, I/O queues,
  softirqs, timers, synchronization, and the network stack in `runtime/net/`.
- `runtime/net/`: IPv4, ARP, ICMP, UDP, TCP, transport demultiplexing; TCP is
  divided among `tcp.c`, `tcp_in.c`, `tcp_out.c`, and supporting files.
- `runtime/net/directpath/`: optional Mellanox mlx5 runtime-to-NIC path.
- `inc/iokernel/`: shared-memory packet headers, queue commands, and control ABI.
- `base/`, `inc/base/`: low-level utilities including LRPC and memory support.
- `net/`, `inc/net/`: packet structures, mbufs, and network utilities.
- `apps/synthetic/`: Rust workload/client with Linux and runtime backends.
- `apps/bench/`, `apps/netbench/`, `tests/`: additional workloads and tests.
- `bindings/`: C++ and Rust runtime bindings. `ksched/`: Linux scheduling module.
- `scripts/`: machine setup, IRQ affinity, and statistics helpers.
- `breakwater/`: separate congestion-control-related tree; not the primary
  packet-I/O integration surface inspected here.

## Existing standard networking path

The default path is IOKernel-mediated; the TCP/IP stack lives in the runtime.
DPDK dependencies extend beyond a single NIC wrapper file.

### Initialization and polling

`iokernel/main.c` initializes base/scheduler/control state, then `dpdk`, `rx`,
`tx`, `dp_clients`, `dpdk_late`, and hardware timestamps. The dataplane loop
calls `rx_burst()`, scheduler polling, TX overflow completion draining,
`tx_burst()`, runtime command processing, and idle-time control processing.
It also directly uses DPDK core/NUMA APIs and optional DPDK statistics.

`iokernel/dpdk.c` initializes EAL and port 0, with one RX and one TX queue.
The EAL arguments include `--vdev=net_tap0`; verify the actual chosen device
on experimental hosts before calling a baseline direct physical NIC access.
The port enables RSS and checksum offloads, and promiscuous reception.

### Receive and receive-buffer return

1. `iokernel/rx.c:rx_burst()` calls `rte_eth_rx_burst()` on queue 0, batching
   up to 64 packets.
2. `rx_one_pkt()` maps destination MAC to a registered runtime via a DPDK hash
   table. Broadcast packets go to all clients using mbuf reference counts;
   unregistered/unhandled traffic is freed.
3. The IOKernel prepends `struct rx_net_hdr`: completion token, packet length,
   RSS hash, checksum status, and raw Ethernet payload. Currently the completion
   token is the IOKernel's `rte_mbuf *`.
4. It sends `RX_NET_RECV` plus a shared-memory offset through a runtime thread's
   RX LRPC queue. `rx_send_to_runtime()` also interacts with core scheduling.
5. `runtime/softirq.c` dispatches received headers to
   `runtime/net/core.c:net_rx_softirq()`.
6. `net_rx_alloc_mbuf()` allocates runtime-private storage, **copies the packet**,
   preserves receive metadata, and sends `TXCMD_NET_COMPLETE` back to the
   IOKernel. Protocol processing then proceeds through Ethernet/IP/transport.
7. `iokernel/commands.c` drains these completion tokens and frees DPDK mbufs.

The ingress pool resides in shared memory (`dp.ingress_mbuf_region`), created
through control initialization and populated as a DPDK mempool by `rx_init()`.
`inc/iokernel/shm.h` defines offsets and bounds conversions; ingress region size
is `0x20000000` (512 MiB). Packet addresses cannot simply be passed as ordinary
pointers between the IOKernel and runtime address spaces.

### Transmit and transmit completion

1. Runtime networking builds Ethernet/IP/TCP or UDP in runtime mbufs.
2. `runtime/net/core.c:net_tx_iokernel()` prepends `struct tx_net_hdr`, containing
   a runtime mbuf completion token, length, offload flags, and Ethernet payload.
   It sends `TXPKT_NET_XMIT` with an offset in the runtime's shared TX region.
3. `iokernel/tx.c` drains runtime TX queues in batches of up to 64, wraps payloads
   in DPDK mbufs, resolves physical addresses, translates checksum flags, and
   calls `rte_eth_tx_burst()`. Unaccepted packets remain queued for retry.
4. `iokernel/mempool_completion.c` and `tx_send_completion()` tie mbuf recycling
   to `RX_NET_COMPLETE` messages to runtimes. Process references protect in-flight
   ownership; overflow queues handle completion-message backpressure.
5. Runtime softirq handling frees completed runtime mbufs.

`inc/iokernel/queue.h` is the key packet/message contract. Its RX and TX
completion tokens belong to different address spaces and have different meanings.

### Other integration surfaces and implications

- `iokernel/defs.h` embeds DPDK mempool/hash state. `dp_clients.c` uses DPDK
  hashes for runtime MAC registration. `control.c` maps ingress/shared runtime
  memory and resolves physical page addresses for the current TX design.
- `runtime/net/core.c` selects `net_driver_ops` for standard IOKernel TX;
  its standard `rx_batch` callback is a stub because RX uses LRPC softirqs.
  Directpath has a separate mlx5 implementation and direct RX mbuf delivery.
- The standard receive path already copies packets. AF_XDP zero-copy at the
  NIC/UMEM boundary must not be described as end-to-end zero-copy without further
  runtime changes. Keep this distinction explicit in evaluation.
- Runtime IPv4 TX requests checksum offload; TCP TX supplies a pseudoheader sum
  and requests TCP offload (`runtime/net/tcp_out.c`). A new backend must honor
  those semantics, e.g. finish checksums in software if offload is unavailable.
- A proposed IOKernel-level AF_XDP replacement could preserve the existing
  runtime/LRPC boundary, but this is an inference, not an implemented decision.
  UMEM placement, headroom, TX copying versus shared allocation, recycling,
  backpressure, teardown, and checksum handling remain design work.
- Preserve ARP/broadcast delivery and ensure packets actually reach the selected
  queue and runtime MAC. Single queue does not automatically imply single thread.
- Distinguish standard Caladan/DPDK from mlx5 directpath when selecting a fair
  baseline; changing both packet I/O and processing placement adds variables.

## Build and environment snapshot

Top-level `Makefile` builds `libbase.a`, `libnet.a`, `libruntime.a`, `iokerneld`,
and C tests. It uses wildcard source discovery, always includes DPDK headers for
IOKernel objects, and links DPDK plus PCM/NUMA/pthread/dl. Backend selection is
not currently abstracted in the build. `build/shared.mk` defines GCC/G++ flags.

All current `build/config` options are `n`: MLX5, MLX4, SPDK, DEBUG, OPTIMIZE,
DIRECTPATH. DPDK, SPDK, rdma-core, PCM, and snappy submodules were uninitialized
at inspection (`git submodule status` entries began with `-`). No build was tried.
The local workspace is on macOS; actual Caladan/AF_XDP operation requires a
Linux experimental environment. README platform/version claims are historical
repository documentation, not verified compatibility for the proposed hosts.

## Evaluation plan

Compare the same network workload across:

1. Linux application using normal Linux TCP sockets.
2. Caladan TCP/IP/runtime using AF_XDP.
3. Caladan TCP/IP/runtime using existing DPDK.

The central controlled comparison is 2 versus 3. Hold application, packet sizes,
connection counts, and CPU allocation constant. Generate increasing load from
a separate client machine. Measure throughput, median, p99, p99.9 latency,
CPU utilization, and drops. Produce latency-throughput curves, with separate
median/tail figures as useful. Repeat measurements for stability. Supplement
with CPU profiles, system CPU time, softirq activity, context switches, and
syscall counts where useful. Treat copying, wakeups, batching, and processing
placement as possible confounders when interpreting results.

`apps/synthetic/src/main.rs` already exposes Linux/runtime client and server
modes, TCP/UDP selection, offered-load options, and percentile reporting through
p99.99. Both Linux and runtime TCP modes call a common server routine via backend
abstractions (`backend.rs`); this is a useful candidate workload, not a validated
experiment setup. Also inspect `apps/bench/netbench_linux.cc` and related workloads
before choosing final benchmark commands. No performance results exist from this
session.

Resources: two CloudLab nodes (server and load generator) connected through the
experiment network. Prefer a modern Intel E810 or another NIC supporting native
XDP and AF_XDP zero-copy; hardware/driver support must be verified on allocation.
Use copy mode on other available nodes rather than blocking the core project.
Development reservations should last a few hours; later use longer repeated
measurement sessions.

Interpretation goals: AF_XDP near DPDK would suggest DPDK-specific packet access
is not dominant for the measured workloads; a larger gap suggests packet I/O
matters. Copy versus zero-copy adds evidence about copying costs. These are
conditional interpretations, not conclusions already established.

## Scope and milestones supplied by the user

- **75%:** working single-queue RX/TX backend, one end-to-end application, and
  Linux/AF_XDP/DPDK latency-throughput comparison. Copy mode and simple buffer
  management are acceptable.
- **100%:** stable zero-copy where supported, batched RX/TX, correct recycling,
  repeated load sweeps, throughput/tail latency/CPU measurements and profiles.
- **125%:** compare copy and zero-copy, vary polling/wakeups and batch sizes,
  possibly add multiple queues to explain remaining performance differences.
- **October 5:** understand existing packet-I/O boundary; CloudLab setup;
  standalone AF_XDP initialization and RX prototype.
- **October 12:** standalone TX/RX verification; begin mapping Caladan buffers
  and network abstractions onto AF_XDP/UMEM.
- **October 19 checkpoint:** backend structure and partial integration, ideally
  packets entering or leaving Caladan, design presentation, early standalone
  microbenchmarks, and evaluation plan.

These dates and objectives were provided as a plan. Their completion outside
this checkout has not been established. Await the user's next instruction.
