# Benchmark methodology

## Purpose and claim boundary

DepthWire benchmarks characterise controlled application-side packet
processing. They are not performance requirements and do not prove production
capacity. A result is publishable only with its source revision, exact command,
environment, workload, measurement boundary, distribution, failures, and known
limitations.

No pre-release result bundle is retained: measurements collected before the
release tag lack a verifiable source revision and are deliberately not
published. A published bundle lives under `results/<version>/<environment>/`
and must be produced from the tagged release revision.

## Benchmark programs

| Target | Question answered |
|---|---|
| `depthwire_ring_latency` | How long does one already-received BUY-producing packet take through virtual Ring PMD TX acceptance with no queue pressure? |
| `depthwire_ring_regimes` | How do deterministic NO_SIGNAL, BUY, and SELL paths differ under the same virtual setup? |
| `depthwire_ring_load` | Under deterministic prequeued Ring bursts, what are RX-to-TX service latency, burst queue-plus-service latency, closed-loop throughput, and observed drops? |
| `depthwire-replay` (paced) | Under replay-paced offered load whose schedule is independent of consumer progress, what are latency, achieved throughput, backlog, lateness, and loss below, near, and above saturation, and how does the system recover when load drops? |
| `depthwire_afpacket_latency` | What application-side RX-return-to-TX-accept latency is observed through the kernel-backed AF_PACKET PMD? |

Regime distributions have different endpoints and must remain separate.

## Timer and percentile definition

The benchmarks use DPDK's `rte_rdtsc_precise()` and convert cycles with
`rte_get_tsc_hz()`. Each program samples two adjacent timer reads and reports
that timer-overhead distribution separately. Timer overhead is included in
reported path intervals and is not subtracted: subtracting one percentile from
another is not a valid distribution correction.

All samples are retained in preallocated storage outside the timed operation,
sorted after measurement, and summarised using:

- arithmetic mean;
- nearest-rank p50, p95, p99, and p99.9 (`ceil(q * N)`);
- maximum.

Preserve the timer-overhead section with every raw output, especially for
sub-microsecond results. Decimal formatting does not imply accuracy beyond timer
resolution or environmental stability.

## Ring service benchmark

The timer starts after the engine's RX burst returns one packet and ends after
its order TX burst accepts one packet. It includes decode, adapter conversion,
book update, strategy, risk, outbox, order encoding, TX-mbuf pool allocation,
the 62-byte copy, and TX acceptance. Generator preparation/injection, RX polling,
order drain, and acknowledgement bookkeeping are excluded.

The workload is deterministic and stop-and-wait. It measures zero-queue service
time, not throughput or latency under offered load.

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

## Ring regime benchmark

This uses separate initialised engines and separate distributions for
NO_SIGNAL, BUY, and SELL. Runs occur in that fixed order, so cross-regime
differences are descriptive unless an external runner balances order across
independent processes.

```bash
./build/dpdk-release/benchmarks/depthwire_ring_regimes \
    -l 0 \
    --no-pci \
    --no-huge \
    --no-shconf \
    --no-telemetry \
    --mbuf-pool-ops-name=ring_mp_mc \
    --file-prefix=dw_regimes_c0
```

## Ring burst/load benchmark

`depthwire_ring_load` prepares a configurable burst, submits it to the generator
Ring port in one TX burst, then processes the accepted packets. It reports:

- service latency: each packet's engine RX return to its order TX acceptance;
- queue-plus-service latency: input-burst acceptance to each order TX acceptance;
- closed-loop harness throughput over the measured phase;
- requested, accepted, RX, TX, drop, and correctness counters.

Example smoke workload:

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

Valid burst size is 1 through 128. This is a repeatable single-lcore closed-loop
pressure test. Its throughput includes harness preparation, injection,
processing, validation, and drain. It is not replay-timed offered load,
multi-lcore generation, line-rate capacity, or overload-recovery evidence.

Compare several burst sizes in independently started processes. Do not publish a
single favourable burst size without the selection rule and all attempted values.

## Replay benchmark (paced offered load)

`depthwire-replay run --mode=paced` replays a validated trace
(`docs/architecture/trace-format.md`) over the virtual Ring PMD with
independent monotonic pacing: events become due on a
`std::chrono::steady_clock` schedule built from the trace's recorded arrival
deltas scaled by declared parts-per-million factors, and every due event is
injected regardless of consumer progress. TSC remains the measurement clock
for latency intervals; the timer-overhead distribution is reported separately
and never subtracted. Pacing is batch-granular within a loop iteration, so
offered load is honoured in aggregate and the injection lateness distribution
is always reported.

Counter definitions (exact; conservation is enforced by the executable's exit
status):

- `offered`: events whose due time elapsed; `accepted`: injections the ring
  accepted; `dropped`: due events refused (mempool exhausted or ring TX
  rejected), freed and never retried.
- `late`: accepted events issued more than the declared lateness bound after
  their due time (still injected); lateness uses a clock reading taken for
  each injected burst.
- `queued-max`: peak total backlog (market events accepted minus processed,
  plus orders in flight), sampled after each injected burst; the declared
  bounds are the ring (1024) and mempool (4095).
- `processed`: packets run through the packet processor; `ordered`: orders
  the engine encoded; `orders-tx-dropped`: encoded orders the transport
  rejected; per-status rejection counters follow the engine's
  sequence-rejection semantics.
- `recovery-duration-ns`: from the load transition into the final declared
  segment (the reduced-load segment of an overload plan; the end of the
  schedule for a single-segment run) until the total backlog is first
  observed at zero. The run must still end with empty rings, an empty
  outbox, and a fully restored mempool.
- per-segment lines report offered, accepted, dropped, late, peak backlog,
  and backlog at each declared segment's end, so a published overload run
  shows the backlog rising and then recovering.

Per published sweep point, report: offered, accepted, processed, ordered,
late, and dropped events; achieved throughput; peak backlog; the service and
queue-plus-service distributions (mean, p50, p95, p99, p99.9, maximum); the
lateness distribution; and the run digest. A run with loss is valid evidence
when the loss is visible in the counters, never silently excluded. Paced
lossy runs declare `--sequence-policy=accept-any` (printed in provenance);
a single drop permanently gaps a contiguous feed, so lossy paced runs use
the explicit opt-out.

**Warm-up policy.** One discarded warm-up run precedes the recorded runs at
every sweep point and for the recovery plan; its raw output is retained and
labelled, but it never contributes to a published summary. Each run also
samples the timer-overhead distribution before its measurement epoch.
`scripts/replay/run-evidence.sh` implements this policy.

**Rate sweep and knee selection.** Sweep points are independent fresh-process
runs of the same trace at declared descending `--scale-ppm` values.
Near-saturation is the smallest attempted scale with zero drops, zero late
events, and achieved throughput at least 99% of offered; above-saturation is
the next attempted step past it (loss must be visible); below-saturation is a
comfortably stable point. Publish every attempted point with the selection
rule, never a single favourable one.

**Overload and recovery.** The recovery demonstration is one run with a
declared segment plan (below-saturation, an intentional overload interval,
then reduced load) whose boundaries and scales come from the sweep:

```bash
./build/dpdk-release/apps/depthwire-replay run \
    tests/fixtures/trace/load_uniform_100k_v1.dwtrace \
    --mode=paced \
    --segments=0:SCALE_BELOW,40000:SCALE_OVERLOAD,60000:SCALE_BELOW \
    -- \
    -l 0 --no-pci --no-huge --no-shconf --no-telemetry \
    --mbuf-pool-ops-name=ring_mp_mc --file-prefix=dw_replay_recovery
```

The run must end with zero backlog, a restored mempool, and all conservation
invariants holding, or it exits nonzero and is not evidence.

**Determinism claim.** Conformance mode
(`depthwire-replay run <trace> --mode=conformance`) is transport-neutral and
needs no DPDK or EAL arguments: identical accepted input and configuration
must reproduce the identical `digest-sha256` across independent runs,
processes, compilers, and machines (the digest contract in
`docs/architecture/trace-format.md`). A lossless paced run at
matching configuration reproduces the conformance digest, demonstrating the
digest is independent of transport. Timing values are reported but never
hashed.

The classification boundary: replay-paced offered load over the virtual Ring
PMD on one lcore. It is not line-rate capacity, physical-NIC latency, wire
latency, or exchange round-trip evidence, and `depthwire_ring_load` remains a
closed-loop comparison that must not be relabeled as replay.

## AF_PACKET benchmark

Create the marked private veth pair with `scripts/dpdk/setup-veth.sh`, locate the
AF_PACKET PMD, and run exactly one AF_PACKET vdev:

```bash
AF_PACKET_LIB="$(find "$(pkg-config --variable=libdir libdpdk)" \
    -name 'librte_net_af_packet.so' -print -quit)"

sudo ./build/dpdk-release/benchmarks/depthwire_afpacket_latency \
    -l 0 \
    --no-pci \
    --no-huge \
    --no-shconf \
    --no-telemetry \
    --mbuf-pool-ops-name=ring_mp_mc \
    -d "${AF_PACKET_LIB}" \
    --vdev='eth_af_packet0,iface=dw_eng,qpairs=1,qdisc_bypass=1' \
    --file-prefix=dw_afpacket_latency
```

In a second terminal:

```bash
sudo ./build/dpdk-release/benchmarks/depthwire_afpacket_benchmark_generator \
    dw_peer
```

The engine verifies that exactly one `net_af_packet` port is exposed. The
generator is stop-and-wait and validates every returned order. This suppresses
queue pressure deliberately; use the Ring load benchmark for controlled burst
experiments and the `depthwire-replay` paced mode for replay-timed
arrivals.

## Environment control

For a retained result:

1. use a Release preset and record the effective compile/link commands;
2. record DepthWire version and source revision printed by the executable;
3. record compiler, standard library, CMake, Ninja, DPDK, OS, kernel, CPU,
   microcode when available, SMT/NUMA topology, selected lcore, TSC frequency,
   governor, platform profile, and isolation/IRQ policy;
4. keep input counts and application arguments fixed;
5. record temperature/background-load controls or explicitly state they were
   uncontrolled;
6. warm up before measurement;
7. use multiple fresh process runs and report every run, not only the best;
8. balance or randomise before/after run order for comparisons;
9. state a practical regression/equivalence threshold before drawing a
   no-regression conclusion.

CPU affinity from `-l` does not isolate a core from scheduler work or interrupts.
Benchmark builds auto-detect Git revision and dirty state when repository
metadata is available. If output says `Source revision: unknown`, the run is not
publishable: reconfigure from a revision-bearing checkout or pass the verified
identifier explicitly with
`-DDEPTHWIRE_SOURCE_REVISION=<commit-or-tag>`. Never infer that value after the
run.

## Evidence retention

Each result bundle under `results/<version>/<environment>/` should contain:

- `environment.md`;
- `summary.md` with the exact command and every independent run;
- the unedited stdout/stderr text for representative runs when small;
- source revision and dirty state;
- effective flags or a retained `compile_commands.json` reference;
- timer-overhead results;
- workload and percentile definition;
- failures, drops, sample count, and measurement boundary;
- links and checksums for large approved external artefacts;
- limitations and allowed comparisons.

Diagnostic and profiler output may contain hostnames, paths, addresses, payloads,
and proprietary symbols. Store it with appropriate permissions and retention;
redact before publishing. Raw per-event latency samples can be large and should
normally live in release/CI artefact storage, with a checksum and link from the
small Git summary.

Never manufacture missing provenance for an older result. Label it historical
and unverified instead.
