# DepthWire 1.1.0 ring and scenario evidence summary

Source revision `9997170584b2a13c36a2c4a3851692931be3488d` (tag `v1.1.0`),
collected 2026-08-05T07:27:09Z by `scripts/ring/run-evidence.sh
results/1.1.0/fedora44-dpdk24-ring`. Every exact command is in `commands.log`
and echoed as the first line of its capture; the environment is in
`environment.md`; `sha256-manifest.txt` checksums the bundle. Rules, counter
definitions, and the warm-up and selection policies are in
`docs/benchmarks/methodology.md`.

All points discard one warm-up process and record three fresh processes; every
recorded run is retained. Latency percentiles are nearest-rank over preallocated
samples; the timer-read-overhead distribution is reported and never subtracted
(representative: mean about 25 ns, p50 about 30 ns). TSC frequency
1,996,200,000 Hz; all times in nanoseconds.

## Ring service latency

`depthwire_ring_latency`, 1,000,000 measured events per run. The interval starts
after the engine's `rte_eth_rx_burst()` returns a BUY-producing packet and ends
after `rte_eth_tx_burst()` accepts the encoded order; generator work and polling
wait are excluded.

| run | mean | p50 | p95 | p99 | p99.9 | max |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 135.8 | 80.2 | 270.5 | 430.8 | 611.2 | 26430.2 |
| 2 | 136.4 | 80.2 | 270.5 | 430.8 | 661.3 | 29335.7 |
| 3 | 136.6 | 80.2 | 270.5 | 430.8 | 631.2 | 25217.9 |

The maxima are single-sample tail outliers on an unisolated desktop; the p50
through p99.9 body is stable across the three runs.

## Ring regime latency

`depthwire_ring_regimes`, 1,000,000 measured events per regime per run, three
independent distributions with different endpoints (NO_SIGNAL ends at the
decision; BUY and SELL end at order TX acceptance), run in that fixed order.

| run | NO_SIGNAL p50 / p99 | BUY p50 / p99 | SELL p50 / p99 |
|---|---:|---:|---:|
| 1 | 50.1 / 80.2 | 180.3 / 450.9 | 170.3 / 440.8 |
| 2 | 50.1 / 100.2 | 180.3 / 440.8 | 180.3 / 450.9 |
| 3 | 50.1 / 60.1 | 170.3 / 420.8 | 180.3 / 440.8 |

The order-emitting regimes cost about 120 ns more at the median than the
decision-only path, consistent with the added strategy signal, risk commit,
order encode, and TX acceptance.

## Ring burst/load sweep

`depthwire_ring_load`, 1,000,000 measured events per run at each burst size,
every attempted burst published. This is a single-lcore closed-loop pressure
test: its throughput includes harness preparation, injection, processing,
validation, and drain, and it is not replay-timed offered load, line-rate
capacity, or overload-recovery evidence. Every recorded run was lossless in its
measured phase (zero injection drops, order-TX drops, and invalid orders). Per-run
service, queue-plus-service, and throughput figures are in the raw captures;
representative values (run 1):

| burst | service p50 | service p99 | queue+service p99 | throughput (events/s) |
|---:|---:|---:|---:|---:|
| 1 | 80.2 | 430.8 | 460.9 | 3,237,040 |
| 2 | 140.3 | 440.8 | 801.5 | 3,450,390 |
| 4 | 140.3 | 430.8 | 1,262.4 | 3,750,988 |
| 8 | 150.3 | 420.8 | 2,364.5 | 4,112,836 |
| 16 | 90.2 | 420.8 | 4,568.7 | 4,270,626 |
| 32 | 90.2 | 430.8 | 9,197.5 | 4,297,907 |
| 64 | 80.2 | 420.8 | 15,399.3 | 4,483,049 |
| 128 | 180.3 | 450.9 | 38,543.2 | 3,844,001 |

Per-packet service latency stays roughly flat across burst sizes, while
queue-plus-service latency grows with the burst (each packet waits behind more
of its batch). Closed-loop throughput rises with burst size to a peak near
4.5M events/s around burst 64 (three-run range 3.87M to 4.51M) and falls back at
burst 128 as queue latency dominates. No single burst size is published as a
headline without the full sweep, per the selection rule.

## Engine scenario counters

`depthwire_scenario_counters`, 50,000,000 iterations per run. Each scenario is a
deterministic steady-state loop that reaches its intended engine outcome on every
iteration. Seven scenarios measure the full packet path
(`pipeline::PacketProcessor::process`, decode to engine to encode, plus an
acknowledgement for the emitting cases); outbox-full measures
`TradingEngine::on_market_update` at the engine boundary, where backpressure is
reachable. Counters are descriptive, reported per fresh process, never subtracted
or hashed. All hardware counters were multiplexed on this six-PMC host and are
enabled/running scaled; context switches and page faults come from `getrusage`.
Cycles per iteration are shown for all three runs to convey stability; the
remaining columns are from run 1.

| scenario | cycles/iter (r1, r2, r3) | IPC | instr/iter | branch-miss/iter | cache-miss/iter |
|---|---|---:|---:|---:|---:|
| outbox-full | 4.0, 4.1, 4.1 | 6.94 | 28 | ~0 | ~0 |
| invalid-frame | 97.0, 96.8, 96.5 | 1.17 | 114 | ~0 | ~0 |
| duplicate | 129.3, 128.9, 129.6 | 1.71 | 221 | ~0 | ~0 |
| sequence-gap | 129.5, 129.5, 128.8 | 1.74 | 225 | ~0 | ~0 |
| no-signal | 144.5, 144.4, 144.4 | 2.66 | 384 | ~0 | ~0 |
| risk-reject | 192.6, 193.2, 192.8 | 2.56 | 494 | ~0 | ~0 |
| buy-signal | 246.8, 247.0, 247.5 | 2.60 | 642 | ~0 | ~0 |
| sell-signal | 267.7, 241.6, 241.3 | 2.42 | 647 | ~0 | ~0 |

The cost ordering matches the work each outcome performs: outbox-full rejects
before any book or strategy work (4 cycles, 28 instructions); invalid-frame
stops at the decoder; duplicate and sequence-gap add the wire decode and adapter
before the engine's sequence rejection; no-signal runs the full decode, book,
and strategy without emitting; risk-reject adds the strategy signal and risk
check; buy-signal and sell-signal add the order build, encode, and
acknowledgement. Branch misses and cache misses are near zero because each loop
is a tight, predictable, hot-working-set repetition. The sell-signal run 1 is a
mild single-run outlier (one extra preemption); its runs 2 and 3 agree with
buy-signal.

## Classification and limitations

Virtual Ring PMD on one lcore, and a transport-neutral engine loop for the
scenario counters. These are controlled application-side measurements: not
line-rate capacity, physical-NIC latency, wire latency, or exchange round-trip
evidence, and `depthwire_ring_load` is a closed-loop comparison that must not be
relabelled as replay. The host was an ordinary desktop session with no core
isolation (see `environment.md`), which chiefly affects the latency tails and the
scenario counter noise, not the median body or the relative outcome ordering. The
scenario hardware counters were multiplexed and are scaled estimates; they are
descriptive and support no equality or capacity claim.
