# BitRipple

BitRipple is an experimental C library for multipath data transfer over RDMA/RoCE. It combines RaptorQ forward error correction (FEC), deterministic weighted path selection, and one reliable-connected (RC) queue pair per path. The sender transmits systematic source symbols and repair symbols; the receiver reconstructs the original object once each source block can be decoded.

The FEC implementation is provided by [nanorq](https://github.com/sleepybishop/nanorq), with finite-field operations supplied by its `oblas` dependency.

> **Project status:** This is a research prototype, not a production-ready transport. The current implementation uses synchronous, two-sided RDMA `SEND`/`RECV` operations, sends a fixed amount of redundancy, and has incomplete feedback handling. See [Current limitations](#current-limitations) and [doc/System_Architecture.md](doc/System_Architecture.md) before using it.

## Highlights

- Transfers one object across up to eight RDMA paths.
- Uses nanorq/RaptorQ systematic encoding and repair symbols to tolerate missing symbols.
- Distributes symbols according to a configurable integer “ball” profile.
- Provides two deterministic bit-reversed spray sequences (LRRT and RLRT).
- Carries object transfer information (OTI), block/symbol identifiers, and per-path sequence numbers in each data packet.
- Includes loss, ECN, severity, and RTT adaptation primitives.
- Builds static and shared C libraries, with C++-compatible public headers.

## Architecture at a glance

```text
Client application                         Server application
       |                                          |
       v                                          v
 br_client_send                            br_server_recv
       |                                          |
  RaptorQ encoder -- source + repair symbols --> RaptorQ decoder
       |                                          ^
       v                                          |
 Weighted spray -- symbol --> path selection       |
       |                                          |
       +--> RC QP 0 ---- RDMA SEND/RECV --------> QP 0
       +--> RC QP 1 ---- RDMA SEND/RECV --------> QP 1
       +--> ...                                      ...
       +--> RC QP N ---- RDMA SEND/RECV --------> QP N
```

The client and server are synchronous library objects; BitRipple does not create worker threads. Feedback is intended to travel in the reverse direction on the same queue pairs and adjust the client’s private path profile.

For component details, packet flow, ownership, concurrency, and protocol constraints, read the [system architecture document](doc/System_Architecture.md).

## Requirements

- Linux with RDMA support.
- GCC and GNU Make.
- `libibverbs` and `librdmacm`, including development headers.
- An RDMA-capable device and configured network, or SoftRoCE (`rxe`) for development.
- Python 3 for the Python tests.
- A compiler/CPU configuration supported by the included `oblas` build. The root build selects AVX and `oblas` uses host-specific optimization flags.

On Debian/Ubuntu-based systems, the principal RDMA packages are typically `libibverbs-dev`, `librdmacm-dev`, and `ibverbs-utils`. Package names and SoftRoCE setup vary by distribution.

## Clone and build

If the repository was cloned without submodules, initialize nanorq and oblas first:

```sh
git submodule update --init --recursive
```

Build all libraries:

```sh
make all
```

This produces:

| Output | Contents |
| --- | --- |
| `libbitripple.a` | BitRipple objects only |
| `libbitripple.so` | BitRipple shared library |
| `libnanorq.a` | nanorq and oblas objects |
| `libnanorq.so` | nanorq shared library |

If `nanorq` is absent, the Makefile’s `clone-nanorq` target can retrieve it recursively with Git.

The shared libraries do not contain an install-time runpath. When running directly from the repository, set:

```sh
export LD_LIBRARY_PATH="$PWD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
```

### Link an application

A typical shared-library link from the repository root is:

```sh
gcc app.c -o app \
  -Iinclude -Inanorq/include -Inanorq/deps/oblas \
  -L. -lbitripple -lnanorq -libverbs -lrdmacm -lm -lpthread
```

Static consumers of `libbitripple.a` must also link nanorq/oblas and the RDMA/math/thread dependencies. There is currently no `make install` target.

## Run the transport test program

Build the standalone client/server test:

```sh
make test_rdma_transport
```

Use the same address, port, path count, and payload size on both sides. Replace `192.0.2.10` with an address on the server’s RDMA-capable network.

On the server:

```sh
LD_LIBRARY_PATH=. ./test_rdma_transport \
  -m server -a 192.0.2.10 -p 13337 -n 2 -s large
```

On the client:

```sh
LD_LIBRARY_PATH=. ./test_rdma_transport \
  -m client -a 192.0.2.10 -p 13337 -n 2 -s large
```

`-s small` selects a 25-byte payload, `-s large` selects 2250 bytes, and a numeric value selects a custom size. The test uses 64-byte symbols and two paths by default.

This executable is a benchmark-style smoke test: each process generates its own random payload and does not compare the received bytes. Use the Python integration test for an end-to-end data comparison after configuring its address for your environment.

## Tests

Run the non-network profile, spray, and codec tests:

```sh
make all
LD_LIBRARY_PATH=. python3 tests/test_library.py
```

Run all Python tests:

```sh
make test
```

`make test` also invokes `tests/test_library_net.py`. That integration test:

- requires two RDMA connections to the hard-coded address `172.28.49.50`;
- must be edited to match the local RDMA network before use;
- reports a skip when no usable device or client connection is available; and
- can therefore print its final success message even when the network portion was skipped.

Additional Makefile targets run the standalone transport test in combined, large-payload, and custom-size configurations. Override `TEST_IP` and `TEST_PORT` when appropriate, for example:

```sh
make test_rdma_transport_combined TEST_IP=192.0.2.10 TEST_PORT=13337
```

These convenience targets terminate processes using the configured test ports and should only be run on a dedicated test setup.

## Public API overview

Include `bitripple.h` and, when direct access to public structures is needed, `bitripple_types.h`.

| Area | Main API |
| --- | --- |
| Library lifecycle | `br_init`, `br_cleanup` |
| Path profiles | `br_profile_new`, `br_profile_set_uniform`, `br_profile_set_fractions`, redistribution functions, `br_profile_free` |
| Deterministic spray | `br_spray_new`, `br_spray_select`, `br_spray_next`, `br_spray_free` |
| Encoding | `br_encoder_new`, `br_encoder_symbol`, `br_encoder_oti`, block metadata functions, `br_encoder_free` |
| Decoding | `br_decoder_new`, `br_decoder_feed`, `br_decoder_ready`, `br_decoder_reconstruct`, `br_decoder_free` |
| Client transport | `br_client_new`, `br_client_send`, `br_client_poll_feedback`, `br_client_free` |
| Server transport | `br_server_new`, `br_server_accept_paths`, `br_server_recv`, `br_server_free` |
| Adaptation | `br_feedback_new`, classification and loss/ECN/RTT handlers, `br_feedback_free` |

A normal client transfer constructs a uniform profile, supplies one host/port entry per path to `br_client_new`, and calls `br_client_send`. A server calls `br_server_new`, waits for exactly the expected number of connections with `br_server_accept_paths`, and then calls `br_server_recv` with an output buffer large enough for the object. See `tests/test_rdma_transport.c` for a complete C example.

### Configuration rules

- Path counts must be between 1 and `BR_MAX_PATHS` (8).
- The profile’s ball count `m` must be a power of two.
- The client requires its path count to match `prof->n`.
- A spray’s `sb` parameter must be odd, and `l` must be between 1 and 31.
- The tested transport configuration uses 64-byte symbols. Symbol-size and receive-buffer restrictions in the current implementation make arbitrary values up to `BR_MAX_SYMBOL_SIZE` unsafe.
- Client and server objects should be externally serialized; the public API is not thread-safe.
- Every successful `br_*_new` call has a corresponding `br_*_free` call.

## Repository layout

```text
include/                    Public API and internal RDMA declarations
src/                        BitRipple profile, spray, codec, feedback, and transport code
nanorq/                     RaptorQ implementation (Git submodule)
  deps/oblas/               Finite-field/matrix operations (nested submodule)
tests/test_library.py       Non-network unit tests
tests/test_library_net.py   RDMA client/server integration test
tests/test_rdma_transport.c Standalone transport test program
doc/System_Architecture.md  Detailed design and implementation notes
Makefile                    Library and test build
```

## Current limitations

The following are properties of the code as it exists today, rather than limitations of RaptorQ or RDMA in general:

- The transport uses two-sided `IBV_WR_SEND`; it does not use one-sided RDMA reads/writes or remote-memory zero-copy transfers.
- Each source block sends `K + max(floor(K / 2), 4)` symbols. The receiver cannot stop the sender early after decoding.
- A server keeps one decoder and is effectively limited to one object transfer.
- Loss is detected from per-path sequence gaps, but lost symbols are not retransmitted. FEC provides the recovery mechanism.
- Client feedback polling is currently ineffective because a zero-timeout CQ poll performs no poll. RTT measurement is not connected to the transport, and ECN is not generated by the client.
- Send and receive completions share a CQ and use overlapping completion identifiers, so completion handling is not robust.
- RDMA connection establishment and data transfer are synchronous and polling-based. Several CM waits have no overall cancellation or timeout mechanism.
- Server receive buffers are fixed at 4096 bytes. The public 65535-byte symbol constant is not a supported end-to-end transport size.
- The packed wire structures use native byte order and have no protocol version, transfer identifier, authentication, or integrity protection.
- `br_client_set_rdma_device` and `br_server_set_rdma_device` are declared but not implemented. Device selection currently follows RDMA CM address/route resolution.
- Error cleanup, malformed-packet validation, and completion handling need hardening before deployment on untrusted or unreliable networks.

## Third-party code

nanorq and oblas are included as Git submodules and are MIT-licensed; their license files are retained under `nanorq/`. The root project does not currently contain a separate license file for BitRipple itself.
