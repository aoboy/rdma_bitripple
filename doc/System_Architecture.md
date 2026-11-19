# BitRipple System Architecture

## 1. Purpose and scope

BitRipple is a C99 library that transfers a single in-memory object over multiple RDMA reliable-connected (RC) queue pairs. It adds two layers above the RDMA transport:

1. **Erasure coding:** nanorq partitions the object into source blocks and produces systematic and repair symbols using RaptorQ.
2. **Multipath scheduling:** a deterministic spray sequence and weighted profile select a path for each symbol.

The receiving side feeds arriving symbols into a nanorq decoder and reconstructs the object when every source block is available or repairable. Per-path sequence numbers provide gap detection, and feedback structures provide hooks for loss-, ECN-, severity-, and RTT-driven profile changes.

This document describes the implementation currently in the repository. Where the intended control loop differs from working behavior, that distinction is called out explicitly. BitRipple is a synchronous research prototype, not a production transport protocol.

## 2. Architectural overview

There is no daemon, separate control-plane process, or library-managed thread pool. Applications instantiate client and server objects directly.

```text
                         Client process
 +----------------------------------------------------------------+
 | Client application                                              |
 |    |                                                            |
 |    v                                                            |
 | br_client_t                                                     |
 |    |-- private br_profile_t <------ br_feedback_t               |
 |    |-- br_spray_t                                               |
 |    |-- br_encoder_t ------> nanorq ------> oblas                |
 |    |-- path sequence numbers                                    |
 |    `-- br_rdma_ctx_t                                            |
 |           |-- path 0: CM ID / PD / CQ / RC QP / MRs / buffers   |
 |           |-- path 1: CM ID / PD / CQ / RC QP / MRs / buffers   |
 |           `-- up to 8 paths                                     |
 +------------------------+---------------------------------------+
                          |
                two-sided RDMA SEND/RECV
                data forward, feedback reverse
                          |
 +------------------------+---------------------------------------+
 | Server process         v                                       |
 | br_server_t                                                     |
 |    |-- br_rdma_ctx_t (one accepted RC QP per path)              |
 |    |-- br_decoder_t ------> nanorq ------> oblas                |
 |    |-- per-path receive/loss/ECN statistics                     |
 |    `-- one reconstruction buffer                                |
 |    |                                                            |
 |    v                                                            |
 | Server application                                              |
 +----------------------------------------------------------------+
```

The RDMA Connection Manager (RDMA CM) establishes each path using `RDMA_PS_TCP` addressing and connection events. Actual symbol payloads use `IBV_WR_SEND`, not RDMA READ or WRITE. The same RC QP can carry feedback in the opposite direction.

## 3. Source modules and responsibilities

| Module | Principal symbols | Responsibility |
| --- | --- | --- |
| `src/bitripple.c` | `br_init`, `br_cleanup` | Global lifecycle placeholders; neither currently manages global state. |
| `src/bitripple_profile.c` | `br_profile_*` | Maintains path count `n`, total balls `m`, per-path allocations, and cumulative ranges. Implements uniform, fractional, exclusion, and redistribution policies. |
| `src/bitripple_spray.c` | `br_spray_*` | Generates deterministic LRRT/RLRT sequences and maps each sequence value to a path through the profile. |
| `src/bitripple_codec.c` | `br_encoder_*`, `br_decoder_*` | Adapts nanorq to BitRipple’s memory-buffer API, OTI metadata, block-readiness checks, and reconstruction. |
| `src/bitripple_client.c` | `br_client_*` | Owns client configuration and state, establishes all paths, generates redundant symbols, schedules sends, and handles feedback. |
| `src/bitripple_server.c` | `br_server_*` | Accepts paths, receives packets, tracks sequence gaps/ECN, emits feedback, and reconstructs an object. |
| `src/bitripple_feedback.c` | `br_feedback_*` | Stores per-path metrics and implements severity classification, ECN debounce, loss “whacking,” and RTT EWMA adaptation. |
| `src/bitripple_rdma.c` | `br_rdma_*` | Implements RDMA CM setup, verbs resource allocation, memory registration, send/receive posting, CQ polling, and cleanup. |
| `src/bitripple_net.c` | `br_udp_*` | Vestigial UDP compatibility stubs. They are not part of the active transport or public API. |

Public declarations are in:

- `include/bitripple.h`: lifecycle, profile, spray, codec, feedback, client, and server APIs.
- `include/bitripple_types.h`: limits, public state structures, wire packets, statistics, flags, and error codes.
- `include/bitripple_rdma_internal.h`: per-path/per-context RDMA state and internal transport functions.

## 4. Runtime objects and ownership

### Client

`br_client_new` constructs the complete client before any object can be sent:

- deep-copies the caller’s `br_profile_t`;
- creates a spray with `l = log2(m)`, `sa = 0`, `sb = 1`, and method 1 (with `l = 1` when `m = 1`);
- copies host strings and port values;
- initializes and connects one RDMA path for every configured route; and
- creates a feedback context that borrows the client’s private profile.

Consequently, feedback-driven updates do not modify the caller’s original profile.

`br_client_send` replaces any encoder retained from a previous send. The client retains the new encoder until the next send or `br_client_free`.

### Server

`br_server_new` stores transport configuration but does not listen. `br_server_accept_paths` creates the listener and accepts RDMA CM connections. The decoder is created lazily from OTI in the first accepted data packet.

A server has only one decoder pointer and no decoder-reset operation. Its receive lifecycle therefore represents one object transfer, not a sequence of independent transfers.

### Codec objects

- `br_encoder_t` owns a nanorq encoder and a memory `ioctx` wrapper. It does **not** copy the caller’s input object.
- `br_decoder_t` owns its reconstruction buffer, memory `ioctx`, nanorq decoder, and per-block repaired-state array.
- nanorq memory wrappers own their wrapper state but not their backing memory.

## 5. End-to-end send path

The following is the implemented flow in `br_client_send`:

1. **Create the encoder.** `br_encoder_new(data, len, symbol_size)` wraps the input buffer and calls `nanorq_encoder_new(len, symbol_size, 8)`. Alignment `Al = 8` is fixed.
2. **Prepare all source blocks.** The encoder calls `nanorq_generate_symbols` for every block, precomputing intermediate symbols needed by RaptorQ.
3. **Read OTI.** Common and scheme-specific OTI values are obtained from nanorq. They are repeated in every data packet so the receiver can initialize its decoder without a separate metadata exchange.
4. **Process one source block at a time.** For a block with `K` source symbols, the client generates:

   ```text
   total_symbols = K + max(floor(K / 2), 4)
   ```

5. **Generate systematic and repair symbols.** ESI values `0 .. K-1` represent source symbols; later ESIs represent repair symbols. All symbols for the current block are allocated and generated before transmission of that block begins.
6. **Select a path.** `br_spray_next` returns the next path according to the client’s current profile and spray index.
7. **Build a packet.** A packed `br_net_header_t` is prepended to the symbol. The header carries SBN/block ID, ESI, path ID, per-path sequence number, symbol size, flags, OTI, and an RTT field.
8. **Copy and post.** `br_rdma_post_send` copies the packet into the selected path’s registered send buffer, posts a signaled `IBV_WR_SEND`, and polls for a completion before returning.
9. **Release block symbols.** Temporary symbol buffers are freed after the entire block is sent.

There is no receiver-driven early stop. Even if the receiver has enough symbols to reconstruct a block, the client continues through its fixed symbol count. Loss recovery depends on FEC rather than retransmission.

## 6. End-to-end receive path

The server flow is split between connection establishment and object reception.

### Connection establishment

`br_server_accept_paths` delegates to `br_rdma_accept_multi`, which:

1. creates a passive RDMA CM listener;
2. waits for `RDMA_CM_EVENT_CONNECT_REQUEST` events;
3. allocates a PD, CQ, RC QP, registered send buffer, and 32 registered receive buffers for the accepted path;
4. posts all receive buffers;
5. accepts the connection and waits for `RDMA_CM_EVENT_ESTABLISHED`; and
6. repeats until the requested number of paths is reached or an error breaks the loop.

Although there is one listener, there is one QP per accepted path, not one shared data QP.

### Symbol reception

`br_server_recv` repeatedly polls all active path CQs in round-robin order:

1. Ignore unsuccessful or non-receive completions.
2. Copy the received bytes into temporary storage and validate basic magic, path, and symbol-length fields.
3. Compare the packet’s per-path sequence number with the previous value. A forward gap produces a loss-feedback packet.
4. If `BR_FLAG_ECN` is present, update ECN statistics and send ECN feedback.
5. If no decoder exists, construct one from the packet’s OTI.
6. Feed `(SBN, ESI, symbol)` to nanorq and repost the receive buffer.
7. At the top of each iteration, call `br_decoder_ready`. This attempts to repair every unfinished block.
8. Once all blocks are repaired, copy the reconstructed object into the caller’s output buffer with `br_decoder_reconstruct`.

The decoder’s allocation and final output length come from OTI. The `expected_len`/`data_len` argument is checked for nonzero but does not constrain the OTI-derived allocation.

## 7. Weighted deterministic path selection

### Profile representation

A profile contains up to eight integer ball counters. The total `m` must be a power of two. A path’s ball count determines the width of its interval in `[0, m)`:

```text
cum[i] = balls[0] + balls[1] + ... + balls[i]
```

`br_spray_select` uses binary search to find the first path whose cumulative count exceeds the generated value `theta`. This makes path selection `O(log n)` after profile updates rebuild the cumulative array.

A uniform profile divides balls as evenly as possible. Fractional and redistribution APIs change interval widths without changing the spray generator itself.

### Spray sequences

For `m = 2^l`, the two methods are:

```text
LRRT (method 1): theta = bitreverse_l((sa + j * sb) mod m)
RLRT (method 2): theta = (sa + sb * bitreverse_l(j)) mod m
```

`sb` must be odd. `br_spray_next` increments `j` and wraps it after `m` positions. Over a complete cycle, the generated values cover the spray space deterministically; the profile maps that space onto paths according to their weights.

The transport always starts with LRRT, `sa = 0`, and `sb = 1`. The public API also permits standalone profile/spray use with other valid parameters.

## 8. FEC and nanorq integration

### Encoding

`nanorq_encoder_new` derives source-block partitioning and symbol parameters from transfer length, requested symbol size, and alignment. nanorq can round or grow the effective symbol size to satisfy its constraints.

For each source block, nanorq:

- loads source symbols through the memory `ioctx`;
- constructs and inverts the precode constraint system;
- generates intermediate symbols; and
- reproduces systematic symbols or generates repair symbols for requested ESIs.

The decoder identifies symbols with `nanorq_tag(sbn, esi)`. Source symbols can be written directly to their output positions; repair symbols are retained for block reconstruction.

### Decoding

`br_decoder_feed` accepts only symbols whose length exactly matches the decoder’s effective nanorq symbol size. Duplicate or ignored symbols are treated as success; a nanorq symbol error maps to `BR_ERR_CODEC`.

`br_decoder_ready` tracks repaired blocks in a BitRipple-owned array. For each unfinished block it calls `nanorq_repair_block`, which patches the constraint system with repair rows and attempts inversion and decoding. Additional overhead symbols may be necessary if a decoding matrix is singular or insufficient symbols have arrived.

`br_decoder_reconstruct` requires readiness and an output buffer at least as large as the OTI-derived object length, then copies the decoder’s buffer to the caller.

### Supporting nanorq components

| nanorq component | Role in the stack |
| --- | --- |
| `lib/params.c`, `lib/tuple.c`, `lib/rand.c` | Source parameters, encoding tuples, degree distribution, and deterministic random generation. |
| `lib/precode.c` | LDPC/HDPC constraints, preconditioning, GF(2)/GF(256) elimination, and backsolve. |
| `lib/sched.c`, `lib/spmat.c`, `lib/wrkmat.c` | Decoding schedules, sparse matrices, permutations, and working-matrix operations. |
| `lib/bitmask.c` | Received-ESI and source-gap tracking. |
| `lib/io.c` | Memory, file, stream, and mmap adapters. BitRipple uses memory contexts. |
| `deps/oblas` | Aligned matrix storage and optimized GF(2)/GF(256) arithmetic. |

## 9. RDMA transport architecture

### Per-path resources

Each `br_rdma_path_t` represents one independently connected RC path:

| Resource | Client | Server |
| --- | --- | --- |
| RDMA CM ID | One per connection | One per accepted connection |
| Protection domain | One | One |
| Completion queue | 256 entries; shared by send and receive | 256 entries; shared by send and receive |
| Queue pair | RC; up to 512 send/receive WRs requested | RC; up to 256 send/receive WRs requested |
| Registered send buffer | 131072 bytes | 131072 bytes |
| Posted receive buffers | 32 | 32 |
| Receive-buffer size | `sizeof(br_net_header_t) + configured symbol size` | Fixed 4096 bytes |

Every path owns its registered buffers and verbs objects. `br_rdma_cleanup_path` deregisters memory, frees buffers, and destroys QP/CQ/PD/CM resources.

### Data operations

- Sends copy application packets into library-owned registered memory before posting.
- Receives land in preposted library-owned registered buffers.
- A receive buffer must be reposted after its completion is consumed.
- Send and receive work completions share each path’s CQ.
- There are no completion channels or event callbacks; CQ progress uses `ibv_poll_cq` plus `usleep`.
- The implementation does not expose remote memory regions and never issues one-sided RDMA READ/WRITE operations.

Timeout arguments are implemented as loop counters rather than deadlines based on a monotonic clock. Actual elapsed time therefore varies with polling and sleep behavior. Several RDMA CM event waits can block without an overall cancellation mechanism.

### Device selection

`BR_RDMA_DEFAULT_DEV` is `"rxe0"`, and the client reads `BR_RDMA_DEV` with the same fallback. However, `br_rdma_init_path` only records the string and ignores `port_num`; it does not select a verbs device. RDMA CM address and route resolution determine the actual device.

The public device setter functions are declarations only and have no implementation in the current library.

## 10. Wire protocol

### Data packet

A data message consists of a packed 40-byte `br_net_header_t` followed by one symbol:

| Field | Purpose |
| --- | --- |
| `magic` | Identifies a BitRipple packet (`BR_MAGIC`). |
| `block_id` | Source block number/SBN. The server casts it to 8 bits when feeding nanorq. |
| `symbol_id` | Encoding symbol identifier (ESI). |
| `path_id` | Client-selected path index. |
| `path_seq` | Monotonically increasing sequence number within that path. |
| `symbol_size` | Number of symbol bytes following the header. |
| `flags` | ECN, loss, and RTT-update flag bits. Data sends currently set flags to zero. |
| `oti_common`, `oti_scheme` | Nanorq decoder construction parameters. |
| `rtt_usec` | Reserved for RTT information; current data sends set it to zero. |

### Feedback packet

A packed 28-byte `br_feedback_pkt_t` is sent as an RDMA SEND in the reverse direction on the path’s QP. It carries:

- path and sequence identifiers;
- flags and severity;
- RTT value; and
- first lost sequence and lost-count information.

The configured feedback ports and `client_host` values are stored but are not used to create a separate feedback channel.

### Protocol assumptions

Both packet types use native integer representation and compiler packing. There is no network-byte-order conversion, protocol version, transfer ID, explicit session negotiation, authentication, or cryptographic integrity check. Peers must therefore use compatible ABIs, byte order, and OTI interpretation on a trusted network.

## 11. Feedback and adaptation architecture

### Intended control loop

```text
Data on path -> server sequence/ECN observation -> feedback on same QP
             -> client feedback context -> private profile update
             -> subsequent spray selections use new path weights
```

### Implemented policies

- **Loss:** the server infers loss only when a later sequence number reveals a gap. The client maps lost counts to a profile “whack” of 50%, 75%, or 90%.
- **ECN:** the server reacts to an incoming ECN flag. The client debounces ECN events and applies a 25% whack after three events.
- **RTT:** the standalone feedback handler maintains an EWMA with smoothing factor 0.3 and applies 10% or 30% whacks above baseline multipliers.
- **Severity:** a standalone classifier combines loss, ECN, and RTT into five levels. Another API attempts to distribute balls inversely to severity weights.

A whack removes a fraction of one path’s balls and redistributes them. Since the client’s spray continues to use the same profile object, a successful update affects subsequent path selections without reconstructing the client.

### Current disconnections

The complete control loop is not operational:

- `br_client_poll_feedback` requests `br_rdma_poll_cq(..., 0, ...)`, whose loop only runs for positive timeouts; it returns without polling the CQ.
- The client never sets ECN on outgoing data and does not measure RTT.
- `br_feedback_apply_rtt` and severity-based adaptation are not called by transport code.
- Loss feedback changes path weights only; it does not schedule retransmissions.
- `br_feedback_apply_severity` has a normalization/removal defect that can prevent termination for some mixed-severity inputs.

The feedback APIs can be exercised independently, but the high-level transport should not be described as a functioning closed-loop congestion controller.

## 12. Concurrency and synchronization

BitRipple creates no threads. All CM setup, encoding, sending, receiving, decoding, and feedback calls run in the caller’s context.

The library is not internally thread-safe:

- client, server, profile, spray, feedback, and codec objects contain mutable unsynchronized state;
- a path has one registered send buffer reused by sends;
- CQ polling consumes shared completion state; and
- decoder reconstruction state is mutated during readiness checks.

`br_rdma_ctx_t` contains a mutex and condition variable initialized during multi-accept, but transport operations do not lock, signal, wait on, or destroy them. Applications must externally serialize access to an object and its associated paths. The Python integration test’s server thread is application-managed concurrency, not a library worker.

## 13. Memory and buffer lifetime rules

| Object/resource | Owner | Lifetime requirement |
| --- | --- | --- |
| Caller’s original profile | Caller | Must remain valid only for the call that consumes it; client construction deep-copies it. |
| Client’s private profile | Client | Borrowed by its feedback context and freed after that context during client destruction. |
| Encoder input object | Caller | Not copied by `br_encoder_new`; it must remain valid for encoder operations that access the memory `ioctx`. |
| Encoder symbol output | Caller | Must have capacity for nanorq’s effective symbol size; the API has no capacity argument. |
| Decoder reconstruction storage | Decoder | Allocated from OTI-derived transfer length and freed by `br_decoder_free`. |
| Reconstruction destination | Caller | Must be at least the OTI-derived object length. |
| RDMA send/receive buffers | RDMA path | Registered with the path PD and released by path cleanup. |
| Temporary block symbol buffers | `br_client_send` | Allocated per block and freed after generation, failure, or transmission. |

Requested and effective symbol sizes can differ because nanorq aligns or grows symbols. Some client allocations use the originally requested size, so unaligned or forced-growth configurations can overflow buffers. The 64-byte size used by the transport tests avoids the common small-alignment case but does not remove the server’s fixed receive-buffer ceiling.

## 14. Build and dependency structure

```text
Application
    |
    v
libbitripple.(a|so)
    |-- libibverbs
    |-- librdmacm
    |-- libm / platform thread support
    `-- libnanorq.(a|so)
            `-- oblas objects
```

The root Makefile compiles BitRipple with C99, optimization, debug information, warnings, and PIC. `libnanorq.a` bundles nanorq and oblas objects. `libbitripple.a` contains only BitRipple objects, so static consumers must explicitly resolve the remaining dependencies.

`libbitripple.so` has no embedded runpath. Repository-local execution generally uses `LD_LIBRARY_PATH=.`; the standalone C test target adds a repository-local rpath.

The root build selects AVX and 32-byte matrix alignment for oblas. Together with oblas host-specific compiler flags, this makes binaries dependent on the build CPU/toolchain configuration. There is no install target or package metadata.

## 15. Verification architecture

| Test | Coverage | Important boundary |
| --- | --- | --- |
| `tests/test_library.py` | Uniform profile sums, deterministic two-path spray distribution, full-symbol encoder/decoder round trip | Does not require RDMA; does not test minimal-overhead recovery, loss patterns, feedback, or malformed inputs. |
| `tests/test_library_net.py` | Two-path client/server transfer and payload comparison through `ctypes` | Hard-coded address; device/connection failures are reported as skips. |
| `tests/test_rdma_transport.c` | Standalone client/server operation, configurable payload/path settings, timing, feedback-poll call | Processes generate independent payloads and do not compare them; failure paths can still exit with status zero. |

The Python tests declare 32-element profile arrays while the C ABI uses `BR_MAX_PATHS = 8`. Their limited accesses happen to work for the tested prefix, but those declarations are not an exact ABI model.

## 16. Current implementation boundaries and hazards

The following constraints are important when reasoning about correctness or extending the architecture:

1. **Completion demultiplexing:** send WRs use `wr_id = 0`, while receive buffer zero also uses `wr_id = 0`. Shared-CQ polling can consume and discard the wrong completion type or confuse feedback with data.
2. **Send completion reporting:** exhausting the send polling loop still returns success if the original `ibv_post_send` succeeded.
3. **Receive capacity:** fixed 4096-byte server buffers do not support the public maximum symbol size. Packet-length validation also needs to occur before header-size subtraction or fixed-size copies.
4. **Packet validation:** the server ignores decoder-feed errors, does not enforce consistent OTI across packets, and has no transfer identifier to isolate sessions.
5. **Connection cleanup:** partial client construction can leak previously initialized paths because the context path count is set only after all connections succeed. Server synchronization/listener cleanup and disconnect handling are incomplete.
6. **Path bounds:** accepting more than eight paths is not checked against the fixed path array before acceptance.
7. **Single-transfer state:** the server decoder is never reset, and there is no streaming or message-boundary abstraction.
8. **Feedback robustness:** the high-level polling defect, unwired RTT/ECN generation, and severity redistribution defect prevent reliable adaptive behavior.
9. **Host-specific wire format and CPU build:** neither interoperates automatically with arbitrary architectures or heterogeneous peers.
10. **API completeness:** the declared RDMA device setters are absent, and some test configuration constants disagree with the public ABI.

These are implementation issues, not inherent properties of fountain coding or multipath RDMA. They should be addressed before treating the library as a reliable or secure transport.

## 17. Extension points

The existing separation provides clear boundaries for future work without changing the basic object model:

- replace synchronous polling with completion-channel or event-driven progress;
- introduce distinct send/receive completion namespaces and robust buffer reposting;
- negotiate protocol version, transfer IDs, effective symbol size, and OTI before data transfer;
- make receive capacity follow negotiated symbol sizes;
- add decoder reset/session management and receiver-driven send termination;
- complete or remove the feedback control loop and retransmission policy; and
- preserve the profile/spray/codec APIs while substituting alternative transport or FEC backends.

Any such work should first add tests for loss/reordering, malformed packets, feedback adaptation, partial connection failures, buffer boundaries, and repeated transfers.
