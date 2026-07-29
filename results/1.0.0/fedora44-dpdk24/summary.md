# DepthWire 1.0.0 replay evidence summary

Source revision `2f6e29a8f513e35e037723da15a9261936ff32bb` (tag `v1.0.0`),
collected 2026-07-29T22:59:56Z by `scripts/replay/run-evidence.sh
results/1.0.0/fedora44-dpdk24`. Every exact command is in `commands.log`
and echoed as the first line of its capture; the environment is in
`environment.md`; `sha256-manifest.txt` checksums the bundle. Rules,
counter definitions, and the knee-selection procedure:
`docs/benchmarks/methodology.md`.

## Determinism

Two fresh-process conformance runs of `conformance_v1.dwtrace` produced
the identical digest, and a lossless paced run over the virtual Ring PMD
at recorded spacing (`--scale-ppm=1000000 --sequence-policy=contiguous`)
reproduced it, demonstrating transport independence:

```
digest-sha256: 8ee63c8ce91d646ca7b93d021ef452587abcd710ba82a2ee04fe01a3ad9c5952
```

Captures: `conformance-run-1.txt`, `conformance-run-2.txt`,
`paced-lossless.txt`.

## Workload

`load_uniform_100k_v1.dwtrace`: 100,000 records at uniform 1000 ns
spacing (100 ms recorded duration), instrument 77, contiguous sequences;
`trace-sha256: 066dfa25a564118a272f0aa902b203224bdc854cb28c3ff6b9ac735fa518d544`.
Offered rate = 1,000,000 events/s × (1,000,000 / scale-ppm). Paced sweep
and recovery runs use `--sequence-policy=accept-any` (printed in each
capture's provenance) because a single drop permanently gaps a
contiguous feed. Declared queue bounds: 1024-slot rings, 4095-mbuf pool.

One discarded warm-up run preceded the recorded runs at every sweep
point and for the recovery plan (retained as `*-warmup.txt`, excluded
here). Latency percentiles are nearest-rank over all retained samples;
timer overhead is reported separately and never subtracted
(representative: mean ≈ 27 ns, p50 ≈ 20 ns, p95 ≈ 70 ns per 100,000
samples; see any capture's "Timer read overhead" section).

## Rate sweep (three recorded fresh-process runs per point, all points)

| scale-ppm | offered rate | run | offered | accepted | dropped | late | queued-max | achieved events/s |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1000000 | 1.0M/s | 1 | 100000 | 100000 | 0 | 0 | 33 | 999993 |
| 1000000 | | 2 | 100000 | 100000 | 0 | 1 | 53 | 999991 |
| 1000000 | | 3 | 100000 | 100000 | 0 | 0 | 26 | 999994 |
| 500000 | 2.0M/s | 1 | 100000 | 100000 | 0 | 0 | 161 | 1999932 |
| 500000 | | 2 | 100000 | 100000 | 0 | 0 | 103 | 1999962 |
| 500000 | | 3 | 100000 | 100000 | 0 | 0 | 57 | 1999956 |
| 333333 | 3.0M/s | 1 | 100000 | 96071 | 3929 | 0 | 1023 | 2858274 |
| 333333 | | 2 | 100000 | 95081 | 4919 | 81 | 1023 | 2830488 |
| 333333 | | 3 | 100000 | 97782 | 2218 | 0 | 1023 | 2910230 |
| 250000 | 4.0M/s | 1 | 100000 | 66987 | 33013 | 0 | 1023 | 2646753 |
| 250000 | | 2 | 100000 | 64646 | 35354 | 0 | 1023 | 2553740 |
| 250000 | | 3 | 100000 | 67770 | 32230 | 0 | 1023 | 2678792 |
| 200000 | 5.0M/s | 1 | 100000 | 44959 | 55041 | 0 | 1023 | 2213496 |
| 200000 | | 2 | 100000 | 51135 | 48865 | 0 | 1023 | 2518718 |
| 200000 | | 3 | 100000 | 46943 | 53057 | 0 | 1023 | 2311767 |
| 150000 | 6.7M/s | 1 | 100000 | 32652 | 67348 | 0 | 1023 | 2134053 |
| 150000 | | 2 | 100000 | 27999 | 72001 | 0 | 1023 | 1828031 |
| 150000 | | 3 | 100000 | 26191 | 73809 | 0 | 1023 | 1701278 |
| 100000 | 10M/s | 1 | 100000 | 13887 | 86113 | 0 | 1023 | 1348193 |
| 100000 | | 2 | 100000 | 12991 | 87009 | 0 | 1023 | 1262031 |
| 100000 | | 3 | 100000 | 14463 | 85537 | 32 | 1023 | 1403603 |
| 50000 | 20M/s | 1 | 100000 | 1023 | 98977 | 0 | 1023 | 192433 |
| 50000 | | 2 | 100000 | 1023 | 98977 | 0 | 1023 | 193341 |
| 50000 | | 3 | 100000 | 1023 | 98977 | 0 | 1023 | 193196 |
| 20000 | 50M/s | 1 | 100000 | 1023 | 98977 | 0 | 1023 | 419539 |
| 20000 | | 2 | 100000 | 1023 | 98977 | 0 | 1023 | 428377 |
| 20000 | | 3 | 100000 | 1023 | 98977 | 0 | 1023 | 422886 |

Per-run service, queue-plus-service, and lateness distributions (mean,
p50, p95, p99, p99.9, maximum) are in each raw capture. Representative
per-packet service latency at 2.0M/s offered: p50 ≈ 210 ns, p99 ≈
780 ns. At the two most extreme scales the pacer makes effectively the
whole trace due before the consumer starts, so exactly the 1023-slot
ring capacity is accepted; their achieved-throughput figures divide a
fixed processed count by schedule-dominated run durations and are not
capacity claims.

## Knee selection (rule in the methodology; every attempted point above)

- **Near-saturation: 500000 ppm (2.0M events/s offered).** Zero drops
  and zero late events in all three recorded runs; achieved ≥ 99.99% of
  offered.
- **Above-saturation: 333333 ppm (3.0M events/s).** The next attempted
  step: loss in every recorded run (2218 to 4919 drops) and the backlog
  reaches the 1023 ring bound.
- **Below-saturation: 1000000 ppm (1.0M events/s).** Comfortably stable:
  zero drops, peak backlog ≤ 53, one late injection in one run.

## Overload and recovery

Segment plan `0:1000000,40001:20000,60003:1000000` on the same trace:
below-saturation, then a 50M/s overload burst, then reduced load. Three
recorded runs (`recovery-segments-run*.txt`), each ending with zero
backlog, a restored mempool, and all conservation invariants enforced by
exit status:

| run | overload seg: accepted / dropped / backlog-at-end | final seg: dropped / backlog-at-end | recovery |
|---:|---|---|---:|
| 1 | 1022 / 18980 / 1023 | 83 / 1 | 471,328 ns |
| 2 | 1022 / 18980 / 1023 | 101 / 1 | 550,561 ns |
| 3 | 1022 / 18980 / 1023 | 49 / 1 | 423,196 ns |

The overload segment fills the ring (backlog 1023) and sheds load
exactly as counted; the reduced-load segment drains it, with the small
residual drops occurring while the accumulated backlog is still
clearing. Recovery is measured from the transition into the final
segment until the total backlog is first observed at zero.

## Classification and limitations

Replay-paced offered load over the virtual Ring PMD on one lcore. This
is not line-rate capacity, physical-NIC latency, wire latency, or
exchange round-trip evidence. The host was an ordinary desktop session
(no isolation; see `environment.md`), which chiefly affects
timing-derived values (lateness counts and latency tails), not the
hashed counters or digests. Loss under overload is visible in the
counters above, never excluded.
