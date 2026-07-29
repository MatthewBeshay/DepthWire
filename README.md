# DepthWire

DepthWire is a C++26 and DPDK packet-processing engine for studying the
market-data-to-order hot path, together with a deterministic trace-replay
study: a validated, bounded trace format; a reproducible replay digest; and
replay-paced offered load with exact loss accounting and an explicit
overload-recovery story.

The engine decodes fixed-size Ethernet frames, updates a fixed-depth L2 book,
evaluates an imbalance strategy, applies risk limits, and encodes an outbound
order:

```text
market-data frame
    -> wire decode
    -> L2 book update
    -> strategy
    -> risk
    -> order encode
    -> TX enqueue
```

DepthWire is a controlled systems benchmark, not a production trading system.

## Toolchain and platforms

DepthWire targets C++26 directly:

- **GCC 16 or newer**, or **Clang 19 or newer with a GCC 16-class libstdc++**
- CMake 3.25+ with Ninja (developed with CMake 4.3);
- DPDK 24.11+ for the DPDK applications, benchmarks, and integration tests
  (the core library, unit tests, and the replay command's conformance mode
  build without DPDK).

Development and CI run on Fedora 44 containers (GCC 16.1.1, Clang 22.1.8,
DPDK 24.11.4). The core packages are portable standard C++; the DPDK paths
are Linux-only and other platforms are untested.

## Package boundaries

The public C++ surface is split into individually exported CMake targets:

```text
Contracts -> (none)
Market    -> (none)
Digest    -> (none)
Book      -> Contracts + Market
Strategy  -> Book + Market
Order     -> Market
Risk      -> Contracts + Market + Order
Wire      -> Contracts
Engine    -> Book + Market + Order + Risk + Strategy
Pipeline  -> Engine + Market + Order + Wire
Trace     -> Contracts + Digest + Wire
Replay    -> Contracts + Digest + Engine + Pipeline + Trace + Wire
```

Consumers include headers through `depthwire/...` and link namespaced targets
such as `DepthWire::Engine`, `DepthWire::Wire`, or `DepthWire::Pipeline`.
DPDK applications and benchmarks compose those portable packages at the edge.

## Trace replay

`depthwire-replay` is the public replay command (installed with the
package). It validates a bounded,
versioned trace before any run, converts between the canonical binary and a
reviewable text form, and replays:

```bash
# Deterministic conformance replay (no DPDK required): identical accepted
# input and configuration reproduce the identical digest-sha256.
./build/core-release/apps/depthwire-replay run \
    tests/fixtures/trace/conformance_v1.dwtrace

# Replay-paced offered load over the virtual Ring PMD (DPDK build), with
# declared rate scaling and loss-accounted recovery to empty queues.
./build/dpdk-release/apps/depthwire-replay run \
    tests/fixtures/trace/load_uniform_100k_v1.dwtrace \
    --mode=paced --scale-ppm=500000 \
    -- \
    -l 0 --no-pci --no-huge --no-shconf --no-telemetry \
    --mbuf-pool-ops-name=ring_mp_mc --file-prefix=dw_replay
```

The trace format and digest contract are specified in [docs/architecture/trace-format.md](docs/architecture/trace-format.md).

## Build

### Core library and unit tests

This build does not require DPDK:

```bash
cmake --preset core-release
cmake --build --preset core-release
ctest --preset core-release
```

DPDK and benchmarks are disabled by default, so a plain CMake configure also
produces the portable core. Debug and sanitiser presets are available as
`core-debug` and `core-sanitised`.

### Install and consume the CMake package

Install the package (header-only libraries plus the `depthwire-replay`
executable) to a chosen prefix:

```bash
cmake --preset core-release
cmake --build --preset core-release
cmake --install build/core-release --prefix /path/to/depthwire-prefix
```

A consuming project can then use the exported targets:

```cmake
find_package(DepthWire 1.0 CONFIG REQUIRED)

target_link_libraries(my_application PRIVATE
    DepthWire::Engine
    DepthWire::Pipeline
    DepthWire::Wire
)
```

Pass the install prefix through `CMAKE_PREFIX_PATH` when configuring the
consumer. The core test suite installs to a temporary build-tree prefix and
builds an external consumer automatically.

### DPDK applications, integration tests, and benchmarks

Install the platform dependencies first (Fedora 44 shown; the packaged DPDK
rejects `--in-memory`, which is why the documented commands use
`--no-huge --no-shconf --no-telemetry`):

```bash
sudo dnf install gcc-c++ cmake ninja-build pkgconf-pkg-config \
    dpdk dpdk-devel dpdk-tools
```

Then build and test:

```bash
cmake --preset dpdk-release
cmake --build --preset dpdk-release
ctest --preset dpdk-release
```

The DPDK suite includes both the virtual Ring PMD scenario test and a complete
virtual RX/TX transport test.

### Wire-codec fuzz smoke test

The libFuzzer target is opt-in and requires Clang:

```bash
CC=clang CXX=clang++ cmake --preset core-fuzz
cmake --build --preset core-fuzz
ctest --preset core-fuzz
```

## Run the Ring PMD benchmark

```bash
./build/dpdk-release/benchmarks/depthwire_ring_latency \
    -l 0 \
    --no-pci \
    --no-huge \
    --no-shconf \
    --no-telemetry \
    --mbuf-pool-ops-name=ring_mp_mc \
    --file-prefix=dw_ring_c0
```

The measured interval begins after `rte_eth_rx_burst()` returns a market frame
and ends after `rte_eth_tx_burst()` accepts the generated order. Generator work
and polling wait time are outside the interval.

For controlled queue pressure, `depthwire_ring_load` accepts application options
after `--`:

```bash
./build/dpdk-release/benchmarks/depthwire_ring_load \
    -l 0 \
    --no-pci \
    --no-huge \
    --no-shconf \
    --no-telemetry \
    --mbuf-pool-ops-name=ring_mp_mc \
    --file-prefix=dw_load_c0 \
    -- \
    --warmup=100000 \
    --events=1000000 \
    --burst=32
```

It reports service latency, burst queue-plus-service latency, closed-loop
throughput, and drops. It is not replay-timed or line-rate evidence (use
`depthwire-replay --mode=paced` for replay-paced offered load); no load
result is published yet.

## Performance results

No performance numbers are published yet, deliberately: a result is
publishable only with full provenance (exact clean revision, environment,
warm-up policy, timer overhead, every run, raw-output checksums; see the
[methodology](docs/benchmarks/methodology.md)). The evidence bundle is
collected from the tagged release revision with
`scripts/replay/run-evidence.sh` and published under `results/`.

## AF_PACKET path

Create a private `veth` pair:

```bash
./scripts/dpdk/setup-veth.sh
```

Find the packaged AF_PACKET PMD:

```bash
AF_PACKET_LIB="$(find "$(pkg-config --variable=libdir libdpdk)" \
    -name 'librte_net_af_packet.so' -print -quit)"
```

Run the engine as root:

```bash
sudo ./build/dpdk-release/apps/depthwire \
    -l 0 \
    --no-pci \
    --no-huge \
    --no-shconf \
    --no-telemetry \
    --mbuf-pool-ops-name=ring_mp_mc \
    -d "${AF_PACKET_LIB}" \
    --vdev='eth_af_packet0,iface=dw_eng,qpairs=1,qdisc_bypass=1' \
    --file-prefix=dw_afpacket
```

Run the functional peer in a second terminal:

```bash
sudo ./build/dpdk-release/tests/depthwire_afpacket_test_peer dw_peer
```

Remove the pair afterward:

```bash
./scripts/dpdk/remove-veth.sh
```

## Repository map

| Path | Purpose |
|---|---|
| `include/depthwire/` | Installed public packages (engine, wire, pipeline, digest, trace, replay) |
| `apps/` | The replay command and the AF_PACKET application |
| `benchmarks/` | Ring and AF_PACKET measurement programs |
| `support/` | Non-installed DPDK test and benchmark infrastructure |
| `tests/unit/` | Package-focused correctness tests |
| `tests/integration/` | DPDK transport, scenario, and recovery validation |
| `tests/fixtures/trace/` | Checked-in replay traces with recorded construction rules |
| `scripts/dpdk/` | Repeatable host and virtual-link setup |
| `scripts/replay/` | Release evidence collection |
| `docs/` | Architecture specs, methodology, and scope |
| `results/` | Benchmark evidence produced from the release tag |

## Documentation

- [Project overview](docs/overview.md)
- [Packet pipeline](docs/architecture/packet-pipeline.md)
- [Wire protocol](docs/architecture/wire-protocol.md)
- [Trace format and digest contract](docs/architecture/trace-format.md)
- [Benchmark methodology](docs/benchmarks/methodology.md)
