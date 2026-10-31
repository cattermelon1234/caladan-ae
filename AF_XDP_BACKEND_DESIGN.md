Caladan AF_XDP backend design and first implementation step
========================================================

Recorded October 31, 2026. This is a proposed implementation plan, not an
implemented backend or a report of successful experiments. It captures the
packet-path discussion and complements [PROJECT_OUTLINE.md](PROJECT_OUTLINE.md).

An initialization-only probe now exists in
[tools/af_xdp_probe](tools/af_xdp_probe/README.md). It implements UMEM/ring/socket
setup and signal-driven cleanup. XDP redirection, packet processing, and Caladan
integration remain planned; Linux execution has not yet been verified.

**The first implementation step is a standalone, single-queue AF_XDP receive
and buffer-recycling prototype on Linux.** Its first success should be receiving
repeated traffic from another host without exhausting its receive frames.
After that, add standalone transmit/completion handling, then connect the
working packet I/O to Caladan's shared-memory interface.

The current workspace runs on macOS. Code and documentation can be developed
here; AF_XDP execution and Caladan integration testing need a Linux host.

1. **Establish the first prototype's environment and scope.**

   Use one experiment interface, one hardware receive queue, one AF_XDP socket,
   and one userspace polling thread. Record the Linux kernel, interface, NIC,
   driver, queue ID, and selected operating mode. Confirm that generated test
   traffic reaches the selected queue. A socket on queue 0 will not automatically
   receive packets assigned to other queues.

   Use explicit copy mode for the first working version if needed. Establish
   zero-copy support separately and record the actual mode for later comparisons.
   Keep Caladan's optional mlx5 directpath disabled during this integration.

   Build the prototype as a small independent tool with an XDP program and a
   userspace executable. File responsibilities and current status:

   | File | Responsibility |
   | --- | --- |
   | `tools/af_xdp_probe/xdp_redirect.bpf.c` | Planned: redirect selected experiment traffic to the socket |
   | `tools/af_xdp_probe/main.c` | Setup and cleanup implemented; polling, recycling, and traffic counters planned |
   | `tools/af_xdp_probe/Makefile` | Builds userspace executable on Linux; BPF build planned |
   | `tools/af_xdp_probe/README.md` | Dependencies, commands, scope, and validation limits |

   A useful reference implementation is the XDP project's
   [AF_XDP example](https://github.com/xdp-project/bpf-examples/tree/main/AF_XDP-example).
   It includes receive/drop, transmit, and Ethernet-forwarding examples; it is
   a capabilities sample rather than a minimal implementation. Choose compatible
   library versions after inspecting the Linux host.

2. **Implement and verify AF_XDP ownership independently.**

   UMEM holds packet bytes. Rings hold descriptors or frame addresses. Create
   the socket, register UMEM, configure the rings, bind the socket to the chosen
   interface/queue, and install its XSKMAP entry. An attached XDP program uses
   that map to redirect ingress traffic. The socket must match the receiving
   interface and queue.

   | Ring | Direction | Purpose |
   | --- | --- | --- |
   | Fill | Userspace to kernel | Supply empty RX frames |
   | RX | Kernel to userspace | Deliver packet locations and lengths |
   | TX | Userspace to kernel | Submit outgoing packet locations and lengths |
   | Completion | Kernel to userspace | Return ownership of submitted TX frames |

   Rings use single-producer/single-consumer access. The initial polling thread
   should own all userspace ring operations. Handle required wakeups. Completion
   returns ownership; it does not prove successful transmission. See the
   [Linux AF_XDP documentation](https://docs.kernel.org/networking/af_xdp.html).

   For the first RX milestone, inspect frame length and Ethernet addresses, count
   packets, and recycle each consumed frame. Keep an ownership ledger so that
   duplicate returns, lost frames, and reuse while in flight are detectable.

   **RX acceptance:** receive substantially more packets than the number of UMEM
   frames, stop and restart traffic without starvation, and reconcile ownership
   counts after stopping and draining. Record errors and drops. Clean shutdown
   should remove the attachment/resources created by this run.

   **Next standalone milestone:** transmit known valid Ethernet frames, exercise
   completion reclamation, and verify reception on the peer. Exercise saturation
   and required wakeups before incorporating Caladan.

3. **Preserve the existing placement of Caladan's networking stack.**

   Normally one IOKernel process serves multiple application processes. Each
   application links its own runtime: user-thread scheduler, TCP/IP stack,
   memory management, timers, and IOKernel communication.

   The IOKernel allocates CPUs to Linux worker threads. Each runtime schedules
   its lightweight application and internal networking tasks on those workers.
   This backend changes the IOKernel's packet I/O while retaining that structure.

   ```text
   Current receive:
   device -> DPDK -> IOKernel -> LRPC -> runtime TCP/IP -> application

   Proposed receive:
   device -> XDP/AF_XDP -> IOKernel -> LRPC -> runtime TCP/IP -> application

   Proposed transmit:
   application -> runtime TCP/IP -> LRPC -> IOKernel -> AF_XDP -> device
   ```

   An AF_XDP socket belongs to the IOKernel in this design. Runtimes need access
   to shared packet storage, not their own AF_XDP sockets or descriptor rings.

4. **Introduce packet-I/O selection throughout the IOKernel.**

   Selecting AF_XDP initialization in `main.c` is only one integration change.
   RX polling, TX submission, receive-buffer release, transmit reclamation,
   statistics, and cleanup must all use the selected implementation.

   | Existing file | Integration responsibility |
   | --- | --- |
   | [iokernel/main.c](iokernel/main.c) | Initialization selection and polling-loop dispatch |
   | [iokernel/dpdk.c](iokernel/dpdk.c) | Existing device initialization; retain as the DPDK implementation |
   | [iokernel/rx.c](iokernel/rx.c) | Separate DPDK packet access from runtime selection and delivery |
   | [iokernel/tx.c](iokernel/tx.c) | Adapt runtime TX draining, submission, and completion handling |
   | [iokernel/commands.c](iokernel/commands.c) | Return ingress tokens to the selected allocator |
   | [iokernel/control.c](iokernel/control.c) | Shared ingress mapping and backend-specific memory assumptions |
   | [iokernel/dp_clients.c](iokernel/dp_clients.c) | Preserve runtime registration; isolate NIC memory-registration assumptions |
   | [iokernel/defs.h](iokernel/defs.h) | Backend state and existing DPDK types |
   | [Makefile](Makefile), [build/config](build/config), [build/shared.mk](build/shared.mk) | Backend options, compilation, and dependencies |

   Audit DPDK calls in scheduler/NUMA setup, timestamps, and statistics as well.
   Some DPDK utility dependencies, such as the runtime MAC hash table, may remain
   initially. Replacing NIC access and eliminating every DPDK dependency are
   separate tasks.

   Keep scheduler polling and control-plane processing in the loop. Bound packet
   work so a burst does not indefinitely postpone scheduling. Preserve the DPDK
   option for baseline comparisons. This checkout also requests a TAP virtual
   device during DPDK setup, so verify which device port 0 actually denotes.

5. **Keep the three receive-routing decisions distinct.**

   ```text
   XDP redirect -> IOKernel's AF_XDP socket
       -> destination MAC lookup selects a runtime process
       -> flow hash and Caladan flow table select a worker queue
       -> runtime transport lookup selects a TCP/UDP endpoint
   ```

   Hardware RSS selects a NIC queue. Caladan's software flow table selects a
   runtime worker queue; they are different decisions. One hardware queue still
   permits software distribution across multiple runtime workers.

   Do not assume ordinary AF_XDP descriptors provide DPDK's `rss_hash` metadata.
   For the initial backend, compute a deterministic flow hash from parsed
   IPv4/TCP/UDP fields and supply it in Caladan's receive header. Define a stable
   fallback for ARP and other supported traffic. Keep packets in one flow on a
   consistent bucket while worker assignments remain unchanged.

   Reuse the scheduling-aware behavior of `rx_send_to_runtime()`, including its
   attempt to wake an idle runtime. Preserve broadcast delivery for ARP.

6. **Adapt receive memory to Caladan's existing LRPC contract.**

   The packet/message ABI is defined in
   [inc/iokernel/queue.h](inc/iokernel/queue.h). Shared address conversions are in
   [inc/iokernel/shm.h](inc/iokernel/shm.h). Runtime mappings and queues are set up
   in [runtime/ioqueues.c](runtime/ioqueues.c).

   The existing runtime expects this contiguous layout:

   ```text
   shared ingress storage:
       [ rx_net_hdr ][ Ethernet | IPv4 | TCP/UDP | payload ]

   LRPC message:
       command = RX_NET_RECV
       payload = offset of rx_net_hdr within the shared ingress region
   ```

   `rx_net_hdr` carries a completion token, Ethernet frame length, flow hash,
   checksum status, and the frame bytes immediately after the header.

   Two implementation choices can preserve this contract:

   | Choice | Design consequence |
   | --- | --- |
   | Shared ingress-backed UMEM | Map packet storage into both processes and reserve metadata headroom; avoid an extra IOKernel RX copy |
   | Separate UMEM plus shared staging buffers | Copy each received frame into compatible shared ingress storage; easier isolation of the initial adapter, with an extra copy |

   These choices are still proposed. Choose explicitly before integration.
   For shared UMEM, the existing fixed ingress mapping/length must remain
   compatible, or both sides' mapping setup must be adapted together. The UMEM
   registration must cover valid packet storage without colliding with the
   DPDK allocator when that backend is selected.

   Derive the packet pointer from the returned descriptor, then place the header
   immediately before the Ethernet frame within reserved application headroom.
   Distinguish a frame's allocation token from its packet-data offset. Validate
   lengths, boundaries, and metadata space before publishing the LRPC message.
   Send a region-relative offset, not an IOKernel virtual pointer or ring index.

   Mark checksum status as `CHECKSUM_TYPE_NEEDED` unless verification is actually
   known. Initially retain the supported ordinary Ethernet/IPv4 packet sizes;
   larger or unsupported layouts require explicit handling.

7. **Return RX storage only after runtime consumption of that storage.**

   [runtime/softirq.c](runtime/softirq.c) consumes `RX_NET_RECV` and calls the
   networking code in [runtime/net/core.c](runtime/net/core.c).
   `net_rx_alloc_mbuf()` already copies the frame into runtime-private memory
   and sends `TXCMD_NET_COMPLETE` for the original receive storage.

   The proposed shared-UMEM lifetime is:

   ```text
   available RX frame
       -> offered for receive
       -> received by IOKernel
       -> held while runtime reads shared bytes
       -> runtime copies frame and sends TXCMD_NET_COMPLETE
       -> IOKernel releases the token for reuse
   ```

   Change `commands.c` to dispatch tokens to the selected backend. Its current
   DPDK pointer cast and `rte_pktmbuf_free()` must not be applied to UMEM tokens.

   If delivery fails or no runtime matches, release the frame without waiting
   for a runtime completion. Broadcast storage needs one outstanding reference
   for each successful recipient and may be recycled only after all return it.
   The staging design instead completes staging-buffer ownership through LRPC;
   its UMEM frame can be recycled after the staging copy finishes.

   Once the runtime has its private copy, the normal stack continues:

   ```text
   net_rx_one(): Ethernet and IPv4
       -> transport.c: protocol/IP/port lookup
       -> tcp_in.c: TCP state and ordered receive queue
       -> wake waiting application user thread
       -> tcp_read(): copy bytes into application buffer
   ```

   UDP uses its existing endpoint queue or spawner handler. No new TCP connection
   lookup is needed in the IOKernel. AF_XDP zero-copy at the device boundary
   would still leave the existing runtime RX copy in this design.

8. **Adapt runtime transmit messages and checksum requests.**

   The runtime already constructs TCP/UDP, IPv4, and Ethernet headers and sends
   `TXPKT_NET_XMIT` with a shared offset to `tx_net_hdr`. Its completion token is
   an opaque runtime mbuf pointer that the IOKernel returns unchanged.

   The proposed initial TX adapter is:

   ```text
   TXPKT_NET_XMIT
       -> translate and validate tx_net_hdr
       -> reserve a free TX UMEM frame
       -> copy only the Ethernet frame, excluding tx_net_hdr
       -> complete requested IPv4/TCP checksums in the copied bytes
       -> submit for transmission
       -> reclaim TX UMEM storage when ownership returns
   ```

   Reserve RX and TX capacity so responses cannot consume all receive storage.
   If capacity is unavailable, retain pending work and retry without losing
   packets or completion tokens.

   Caladan currently requests IPv4 checksum generation and places a partial
   pseudoheader checksum in standard-path TCP packets. The baseline AF_XDP
   adapter should calculate complete checksums in software. For TCP, explicitly
   account for the existing partial seed, for example by zeroing the copied TCP
   checksum field and recomputing from the pseudoheader and complete segment.
   Do this on the copied frame so retransmission storage stays unchanged.

   Return `RX_NET_COMPLETE` for the original runtime buffer after its bytes are
   copied and the adapter has retained any state needed to finish or retry the
   send. It need not wait for the independent UMEM frame to be reclaimed.
   A later design transmitting runtime-backed memory directly would require a
   different allocation/registration arrangement and longer buffer retention.

9. **Keep the completion mechanisms separate.**

   | Event | Storage/reference it releases |
   | --- | --- |
   | `TXCMD_NET_COMPLETE` | Shared ingress storage read by a runtime |
   | `RX_NET_COMPLETE` | Runtime's outgoing packet-I/O reference |
   | AF_XDP TX reclamation | Backend's outgoing UMEM storage |
   | TCP acknowledgment from peer | TCP's retransmission reference |

   An outgoing TCP mbuf can remain allocated after `RX_NET_COMPLETE` because TCP
   still needs its payload for retransmission. Preserve runtime process lifetime
   handling and retry completion notifications when LRPC queues are full.
   Also account for runtime exit with shared receive frames outstanding.

10. **Integrate in small, observable milestones.**

    | Order | Deliverable | Evidence required |
    | --- | --- | --- |
    | 1 | Standalone RX and recycling | Repeated peer traffic; stable frame accounting |
    | 2 | Standalone TX and reclamation | Peer observes valid packets; no in-flight reuse |
    | 3 | Selectable IOKernel packet I/O | DPDK remains usable; AF_XDP initializes and polling preserves scheduler work |
    | 4 | One runtime receives through LRPC | Valid metadata, flow selection, and returned ingress tokens |
    | 5 | ARP and application response | Valid checksums and complete request/response exchange |
    | 6 | Sustained TCP/UDP operation | Handshake, reads/writes, closure, drops, pressure, and teardown accounted for |
    | 7 | Optimization and comparison | Explicit modes, batching, repeatable load sweeps, copy accounting |

    First make ownership and end-to-end behavior correct. Then evaluate batching,
    zero-copy, and multiple queues while holding application behavior and CPU
    allocation comparable to the standard Caladan path.
