# Caladan AF_XDP project outline

Updated October 20, 2026. Design and task outline only; backend implementation
has not started in this checkout. For session recovery, also read
[PROJECT_CONTEXT_README.md](PROJECT_CONTEXT_README.md).

## 1. Understand the repository

| Directory / file | Purpose | Relevance |
| --- | --- | --- |
| `iokernel/` | Separate service for packet I/O, steering, and CPU allocation | Main integration area |
| `runtime/` | Application-linked userspace threading and scheduling library | Preserve its behavior |
| `runtime/net/` | Userspace IPv4, TCP, UDP, ARP, ICMP | Preserve protocol implementation |
| `inc/iokernel/queue.h` | Packet metadata and runtime/IOKernel messages | Preserve the packet contract |
| `inc/iokernel/shm.h` | Shared-memory regions and offset conversion | Make packet memory accessible across processes |
| `base/`, `inc/base/` | Memory, atomics, queues, and low-level utilities | Reuse existing mechanisms |
| `net/`, `inc/net/` | Packet structures, mbufs, checksum helpers | Buffer and checksum integration |
| `ksched/` | Linux module supporting CPU scheduling | Required platform setup |
| `bindings/` | Rust and C++ runtime interfaces | Existing benchmark support |
| `apps/synthetic/` | Workload with Linux/runtime networking backends | Candidate common benchmark |
| `apps/bench/`, `apps/netbench/`, `tests/` | Workloads and tests | Correctness and performance checks |
| `build/config`, `build/shared.mk`, `Makefile` | Features, compiler settings, linking | Add backend build selection |
| `scripts/` | Host setup and measurement utilities | CloudLab setup |
| `dpdk/`, `rdma-core/`, `spdk/`, `deps/pcm/` | External dependencies/submodules | Current build dependencies |

Suggested reading order: root README and CONTRIBUTING; `iokernel/main.c`;
`inc/iokernel/queue.h`; `iokernel/rx.c`, `tx.c`, `commands.c`;
`runtime/softirq.c`; `runtime/net/core.c`; `iokernel/control.c`, `dp_clients.c`,
`defs.h`; build files; benchmark backend and main.

## 2. Understand Caladan

Caladan combines a userspace networking stack with an application runtime and
a service that allocates cores across applications. Its purpose includes low
tail latency under changing load while using CPU resources efficiently.

The IOKernel is a userspace process despite its name. The separate `ksched`
component is a Linux kernel module. Applications link against the runtime and
use its threads and networking APIs. TCP processing happens in the application
runtime; the standard IOKernel handles raw packet movement and steering.

Standard receive path:

```text
NIC -> DPDK RX -> IOKernel destination-MAC lookup
    -> shared ingress packet + RX_NET_RECV message
    -> runtime copies packet into its own mbuf
    -> Ethernet/IP/TCP processing -> application
```

Standard transmit path:

```text
application -> runtime TCP/IP -> shared TX packet + TXPKT_NET_XMIT
            -> IOKernel DPDK TX -> NIC
```

Receive recycling: runtime sends `TXCMD_NET_COMPLETE`; IOKernel releases the
ingress buffer. Transmit recycling: IOKernel sends `RX_NET_COMPLETE` when the
runtime's outgoing buffer can be released. These are different ownership events.

The optional mlx5 directpath moves NIC access into runtime cores. Keep that
disabled for the initial standard-path comparison so placement stays comparable.

## 3. Define the project boundary

Research question: how much does the packet-I/O mechanism affect Caladan's
performance when its TCP stack, application, and runtime stay mostly fixed?

Compare Linux TCP, standard Caladan/DPDK, and Caladan/AF_XDP. The most controlled
comparison is the two Caladan variants. Linux versus Caladan changes more than
one component, so it cannot independently quantify TCP versus scheduling gains.

Proposed initial integration: put AF_XDP in the IOKernel, preserving existing
shared-memory queues and runtime protocol processing. This is a proposed design,
not an existing generic backend interface. DPDK is currently used in RX/TX,
initialization, runtime registration hashes, statistics, and other support code.
Retaining some DPDK utility dependencies initially is possible; removing all
DPDK linkage is a separate task from replacing DPDK NIC access.

AF_XDP uses registered packet memory (UMEM), an interface/queue-bound socket,
and an XDP program that redirects selected ingress traffic through an XSK map.
Four rings transfer descriptors/ownership:

| Ring | Producer -> consumer | Role |
| --- | --- | --- |
| Fill | Userspace -> kernel | Supply empty receive frames |
| RX | Kernel -> userspace | Deliver received packet descriptors |
| TX | Userspace -> kernel | Submit outgoing packet descriptors |
| Completion | Kernel -> userspace | Return ownership of submitted TX frames |

Use the [Linux AF_XDP documentation](https://docs.kernel.org/networking/af_xdp.html)
to check details against the target kernel. Native XDP attachment and AF_XDP
zero-copy are separate capabilities. Explicitly select and record copy versus
zero-copy; avoid silent fallback in measured configurations.

## 4. Work packages and completion criteria

### A. Establish the Linux environment and baseline

- Reserve two CloudLab nodes and record NIC, driver, firmware, kernel, topology,
  NUMA placement, link speed, and CPU allocation.
- Establish which native XDP and zero-copy modes actually work on the host.
- Initialize required submodules and build Caladan, ksched, bindings, and workload.
- Run the unmodified standard DPDK path and Linux TCP workload end to end.
- Verify the actual DPDK port: this checkout requests a TAP virtual device in
  EAL initialization, so do not assume port 0 is the experiment's physical NIC.
- Preserve management access while configuring the experiment interface.

Done when both baseline applications exchange requests between the two nodes
and configurations and device selection are recorded reproducibly.

### B. Validate AF_XDP independently

- Choose an AF_XDP userspace library/API compatible with the target environment.
- Create UMEM, frame allocation bookkeeping, four rings, and one socket.
- Load/attach the redirect program and populate its socket map.
- Select the interface/queue; arrange NIC steering so test packets arrive there.
- Verify receive, transmit, recycling, and polling/wakeup behavior.
- Count packets, drops, allocation failures, and ring pressure; clean up resources.
- Verify copy mode first if needed, then explicitly request zero-copy where supported.

Done when a standalone test reliably receives and transmits valid packets and
continues operating without frame exhaustion.

### C. Introduce a selectable IOKernel packet-I/O path

- Define backend operations for initialization, RX polling, TX submission,
  receive-buffer release, TX reclamation, statistics, and shutdown.
- Adapt `iokernel/main.c`, build options, and configuration to select a backend.
- Keep shared scheduling/control behavior and existing DPDK operation intact.
- Audit direct DPDK assumptions in `defs.h`, `dp_clients.c`, `control.c`, NUMA,
  timestamps, and statistics; gate or adapt those used by the AF_XDP path.
- Expose interface, queue, mode, ring/frame counts, and batch size explicitly.

Done when backend selection is explicit and DPDK still builds and functions.

### D. Connect AF_XDP RX to the runtime

- Choose memory layout: shared ingress-backed UMEM with sufficient metadata
  headroom, or an initial copy from UMEM into existing shared ingress storage.
- Validate packet lengths and descriptor offsets before exposing packet memory.
- Construct `rx_net_hdr` and deliver its shared offset through `RX_NET_RECV`.
- Preserve runtime MAC selection, ARP/broadcast handling, checksum metadata, and
  flow assignment. Define a deterministic hash policy when no NIC RSS hash is available.
- Adapt `commands.c` so returned tokens release AF_XDP frames or staging buffers.
- Refill only after all consumers release a frame; recover immediately on failed delivery.

Done when runtime packet processing sees valid traffic and receive buffers are
returned exactly once, including drops and full-queue cases.

### E. Connect runtime TX to AF_XDP

- Drain existing `TXPKT_NET_XMIT` messages and read `tx_net_hdr` payloads.
- Initially copy packets into free TX UMEM frames unless a shared allocation
  design has been explicitly implemented. Reserve frames so TX cannot starve RX.
- Complete IPv4/TCP checksums in software where required by offload flags;
  runtime TCP packets currently contain a partial checksum intended for NIC completion.
- Publish TX descriptors, issue wakeups when required, and handle ring saturation.
- Drain completion descriptors and reclaim the corresponding UMEM frames.
- Return runtime `RX_NET_COMPLETE` tokens only when original runtime buffers
  are safe to reuse. With a completed copy, that can precede UMEM TX reclamation;
  if transmitting runtime-backed memory, retain it until ownership returns.
- Preserve completion overflow behavior and runtime/process lifetime protection.

Done when an application receives a request and sends a valid response over
AF_XDP, repeatedly, with no premature reuse or lost completions.

### F. Prove correctness and stability

- Establish ARP resolution, TCP handshake, request/response, and clean close.
- Exercise sustained traffic, low load, bursts, full queues, frame shortages,
  failed startup, and runtime exit with outstanding packets.
- Check checksums, packet captures, drop counters, frame accounting, and leaks.
- Verify shutdown removes only program/map/socket resources owned by this run.

Done when the single-queue backend runs the selected application reliably and
buffer counts remain consistent under stress and teardown.

### G. Add and evaluate batching and zero-copy

- Batch RX consumption, fill replenishment, TX reservation/publication, and
  completion reclamation; bound work to preserve scheduler responsiveness.
- Confirm the actual selected mode; compare copy and zero-copy on the same host.
- Record every packet copy, including runtime RX and any staging/TX copy.
- Vary batch sizes and polling/wakeup settings after a stable default exists.

Done when batched operation is stable and zero-copy runs where supported.
AF_XDP zero-copy refers to NIC/UMEM transfer; the current runtime RX copy remains.

### H. Run controlled experiments and produce the report

- Reuse the same workload, payload sizes, connections, work per request, core
  budget (including IOKernel and supporting processing), placement, and link.
- Fix client behavior across server configurations; confirm the client is not
  the bottleneck and report achieved throughput as well as offered load.
- Sweep load from light traffic through saturation; warm up, repeat trials,
  and collect enough requests to estimate p99.9 meaningfully.
- Capture throughput, median/p99/p99.9, CPU use, and losses/failures, including
  requests never sent or unfinished rather than only completed-request latency.
- Collect profiles, softirq/system CPU, context switches, syscall counts, NIC
  and AF_XDP counters where useful to explain differences.
- Plot median and tail latency against achieved throughput; show variability
  across repetitions and describe saturation/drop behavior.
- Explain copying, checksum work, placement, batching, and wakeups as possible
  contributors instead of attributing every difference to DPDK alone.

Done when results and scripts/configuration can reproduce the three-way comparison.

## 5. Deliverable tiers

- **75%:** single-queue end-to-end backend, copy mode acceptable, one application,
  and Linux/AF_XDP/DPDK latency-throughput comparison.
- **100%:** stable zero-copy where supported, efficient batching/recycling,
  repeated experiments and CPU/profile analysis.
- **125%:** copy-versus-zero-copy, batch/polling/wakeup sweeps, possibly multiple queues.

The original October 5/12/19 milestones are planning dates, not verified achievements.
No implementation or experiments were performed while creating this outline.
