# Failover and gap-fill results

Revision `e15cd45` (branch `ha/failover`). Design:
`docs/architecture/failover.md`. Environment and controls: `environment.md`.
Per-run tables: `summary-raw.txt`. Raw reports: one directory per scenario.

**The "hosts" are three network namespaces on one machine.**

**Percentiles.** Nearest rank, `ceil(q * N)`. With N = 20, p99 is the maximum
and is reported as such.

## Correctness

| Scenario | Runs | Completed | Final digest equals the conformance digest |
|---|---|---|---|
| Failover, all three heartbeat settings | 60 | 60 | 60 of 60 |
| Gap-fill, loss on feed datagrams only | 9 | 9 | 9 of 9 |
| Gap-fill, loss on all traffic | 9 | 7 | 7 of 7 completed; 2 failed closed (below) |

**Failover.** In every run the primary died after applying record 40,000.
The standby had applied exactly 40,000 records at promotion, so write-ahead
replication lost no record. It then recovered the records it had discarded
while it was standby by gap-fill and finished the trace.

There were no false failovers at any setting: no run had a standby promote
while its primary was alive.

## Failover timing

| Setting | Heartbeat x missed | Timeout | Detection p50 / max | Outage p50 / max | Catch-up p50 / max | Primary's largest heartbeat gap |
|---|---|---|---|---|---|---|
| default | 5 ms x 4 | 20 ms | 17.66 / 19.96 ms | 18.44 / 20.74 ms | 0.72 / 1.46 ms | 5.94 ms |
| moderate | 2 ms x 4 | 8 ms | 6.91 / 7.98 ms | 7.82 / 9.27 ms | 0.36 / 1.53 ms | 3.82 ms |
| tight | 1 ms x 3 | 3 ms | 2.37 / 2.90 ms | 3.33 / 3.69 ms | 0.30 / 1.75 ms | 1.72 ms |

Each column is measured as follows:
- **Detection:** from the primary's death to the standby declaring failure.
- **Outage:** from the primary's death to the first record the promoted
  standby applies.
- **Catch-up:** from detecting the hole `[40000, first live)` to having
  applied past it.

Detection is bounded by the timeout. It falls below it when the primary's last
heartbeat came shortly before death, so the range is roughly (timeout - H,
timeout + 1 ms poll]. The outage is detection plus about 0.8 ms for the standby
to connect to the retransmit service and apply its first record.

The tight setting kept a 1.7x margin: the largest heartbeat gap was 1.72 ms
against a 3 ms timeout. That margin is specific to an idle desktop. Under
load, the superseded run (`../superseded-5a2d096/`) shows what happens when
heartbeats stall.

## Gap-fill recovery

Each gap is timed from detection, when the first datagram arrives past the
hole, until the engine has applied past it. The table pools all gaps from 3
runs at each level.

| Loss on | netem loss | Gaps | p50 | p99 | max |
|---|---|---|---|---|---|
| feed datagrams only | 0.1% | 339 | 0.12 ms | 0.27 ms | 0.49 ms |
| feed datagrams only | 1% | 3,321 | 0.11 ms | 0.39 ms | 0.97 ms |
| feed datagrams only | 5% | 14,807 | 0.11 ms | 0.43 ms | 1.34 ms |
| all traffic | 0.1% | 338 | 0.13 ms | 0.36 ms | 7.10 ms |
| all traffic | 1% | 3,178 | 0.11 ms | 169.35 ms | 212.22 ms |
| all traffic | 5% (1 of 3 runs completed) | 4,851 | 51.11 ms | 208.30 ms | 215.95 ms |

**Loss on feed datagrams only.** Recovery stays sub-millisecond up to 5% loss,
because each hole is requested the moment it is exposed.

**Loss on all traffic.** This also drops gap-fill responses on the single
TCP connection. TCP then waits for its retransmission timeout, at least
200 ms on Linux. Every request queued behind the lost segment waits too: that
is head-of-line blocking.

At 5% loss this happened often enough that, in two of three runs, the live
feed ran more than 16,384 records ahead of an unfilled hole. The receiver then
**failed closed** (`failure: reorder buffer overflow; stopping rather than
trading past a hole`) instead of trading past the hole. This is the designed
behaviour, and those runs are recorded as failures in `failures.txt`, not
dropped.

## What this does and does not show

**It shows that:**
- the standby's state is exactly the primary's: write-ahead replication with
  per-record outcome comparison never diverged;
- failover completes within the configured timeout plus about 1 ms;
- gap-fill restores the lossless result whenever the recovery channel itself
  delivers.

**It does not show:**
- behaviour across physical hosts, NICs or switches;
- clock skew between machines;
- a host crash rather than a process crash;
- load from other processes.

## Implications for the design

A recovery channel that shares a lossy path with the feed is the weak point.
The next steps are:
- lower the TCP minimum retransmission timeout on that route (`ip route ...
  rto_min`);
- use several gap-fill connections, or a UDP request/response with its own
  retry;
- or arbitrate redundant A/B feeds, as venues provide, so that most gaps
  never need a request.
