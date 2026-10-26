# Two-host failover and multicast gap-fill

Status: design for review. Comments are welcome on the pull request that adds
this file.

## Problem

DepthWire is a single process on a single host. If that process or host dies,
trading stops. If one inbound frame is lost, the engine sees a sequence gap and
the application exits (`apps/depthwire-engine/main.cpp:350-357`), because the
paced replay benchmarks have to use `accept-any` for lossy runs
(`benchmarks/methodology.md:203-206`). This design adds two things:

1. **A warm standby on a second host.** It holds the primary's engine state and
   takes over within tens of milliseconds of the primary failing, without
   resending or skipping an input.
2. **A multicast feed with gap-fill.** A lost datagram is recovered by
   retransmission instead of stopping the engine.

Both rest on a property DepthWire already has: the engine is deterministic.
The same accepted input sequence, in the same order and with the same
configuration, produces the same journal and the same run digest
(`architecture/trace-format.md`, pinned by
`tests/unit/replay/conformance_fixture.cpp`). State therefore never has to be
copied. Replaying the same inputs in the same order is enough to make a
standby identical.

## Non-goals

- The DPDK hot path stays as it is. The new components use kernel UDP and TCP
  sockets and live in a separate application, `depthwire-ha`.
- No consensus protocol and no more than two engine hosts. Leader election is
  "the standby promotes when the primary goes silent", with the fencing limits
  described below.
- No exchange session layer. Orders are counted, not sent to a venue.

## Topology

Three Linux network namespaces stand in for three hosts. They are joined by a
bridge on one machine:

```text
              +-------------- br-dw (10.77.0.0/24) --------------+
              |                    |                              |
   ns feed 10.77.0.1      ns primary 10.77.0.2           ns standby 10.77.0.3
   multicast 239.77.0.1:47001 ---> receive ----------------> (joined, discarding)
   retransmit TCP :47002   <--- gap-fill requests
                                   |  replication TCP :47003 ---> apply
                                   |  heartbeat UDP   :47004 ---> failure detector
```

The feed replays a `.dwtrace` file. Using namespaces rather than two physical
machines is a limitation of the lab, and the measurements say so. The code
only sees sockets and addresses.

## Wire formats

All integers are big-endian.

| Message | Layout | Bytes |
|---|---|---|
| Feed datagram | `u32 record_index`, `u32 reserved (0)`, 48-byte DepthWire wire message | 56 |
| Gap-fill request | `u32 first_index`, `u32 count` | 8 |
| Gap-fill response | `u32 count_available`, then that many feed records | 4 + 56n |
| Replication record | `u32 record_index`, 48-byte wire message, `u8 process_status`, `u8 engine_status` | 54 |
| Heartbeat | `u32 magic 'DWHB'`, `u64 heartbeat_number`, `u32 applied_count` | 16 |

The transport index (`record_index`) is separate from the wire message's own
sequence number. Gap detection then works on what the *transport* lost, and any
trace replays byte for byte, including the conformance fixture, which contains
deliberate duplicate, gap and stale sequences. The engine's own sequence
checks are unchanged.

## Gap-fill

A `GapTracker` sits in front of `PacketProcessor::process`. It is pure logic
with no sockets or clocks, so it can be unit-tested exhaustively:

- index == expected: deliver it, advance.
- index < expected: duplicate, drop and count it.
- index > expected: hold it in a bounded reorder buffer and report the
  missing range `[expected, index)`. `GapTracker` takes the capacity as a
  template argument (default 4096). `depthwire-ha` uses 16384 records, which
  is 164 ms of feed at 100k records/s, enough for a standby's detection
  timeout plus its catch-up request.
- When the missing records arrive, deliver them and everything contiguous
  behind them in index order.
- If the reorder buffer is full, fail closed: stop processing and report.
  Never trade past a hole.

`depthwire-ha` requests each hole as soon as a later datagram exposes it.
Requests are capped at 1024 records, and the rest of a longer hole is
requested as soon as the front of the hole reaches it. The front hole is
re-requested after the retry interval (default 10 ms). If the feed goes quiet
with records still missing at the end, a tail request covers them, because
nothing later will ever expose that hole.

Recovery time for one gap is measured from detection (the first datagram past
the hole) to the moment the last missing record is delivered. It uses
`CLOCK_MONOTONIC` in the receiving process.

The feed retains the whole trace for retransmission. A real venue keeps a
bounded window and falls back to a snapshot, which is out of scope here.

## Warm standby

### Replication

The primary sends one replication record to the standby **before** it would
transmit any order for that input (write-ahead). Each record carries the
primary's outcome. The standby processes the same 48-byte message and
compares its own `process_status` and `engine_status` with the primary's.
**Any mismatch is fatal for the standby**: a standby that has diverged must
never promote.

The outstanding-order count is the only engine state that depends on something
other than market data. It depends on acknowledgements. `depthwire-ha` uses
the conformance policy: every encoded order is acknowledged immediately
(`replay/conformance_runner.hpp:118`). The primary and the standby therefore
apply identical acknowledgement events without shipping them. A system with
real acknowledgements would put them into the same replication stream, in
order, as events of their own.

### Failure detection

The primary sends a heartbeat every H ms (default 5). The standby declares
the primary failed when no heartbeat has arrived for K x H (default K = 4,
so 20 ms). The `FailureDetector` takes time as an argument and is unit-tested
without sleeping.

The primary checks whether a heartbeat is due after every record it applies, as
well as on every poll, so draining a backlog never starves the standby. An
earlier revision checked only on every poll, and it produced false failovers
(`results/failover/superseded-5a2d096/`).

| Lower H x K | Higher H x K |
|---|---|
| Faster takeover | Fewer false failovers when a heartbeat is delayed by scheduling, GC-like pauses elsewhere on the host, or a congested link |

The measurements report detection latency at the default and at one tighter
setting.

### Promotion

On declaring failure, the standby:

1. applies the replication records it has already received, then closes
   that connection;
2. reads `applied_count`, the next record index it needs;
3. starts consuming the multicast feed it joined at startup, where it was
   discarding datagrams, and requests gap-fill for
   `[applied_count, first live index)`;
4. continues as primary. Its run digest over the whole trace must equal the
   conformance digest of the same trace in one process. **That equality is
   the acceptance test.**

### Fencing, split brain and duplicate orders

**Split brain.** If the heartbeat path fails while the primary is still alive,
both hosts would trade. Two measures bound this:

- **Self-fencing.** The primary sets its kill switch and stops when a
  replication write fails. Replication and heartbeats share a network path,
  so the partition that makes the standby promote also stops the primary.
  A promoting standby closes the replication connection, so a primary that
  was only slow, not dead, also fences on its next write. The records it
  applied in between are the split-brain overlap, and the measurements report
  it. A lease timer that fences a primary whose writes merely stall is not
  implemented. A stalled write stops that primary's heartbeats, which the
  standby treats as a failure.
- **The venue is the final fence.** A real exchange session permits one logon
  per session ID. A second engine logging on is rejected or disconnects the
  first.

**Duplicate orders.** The standby regenerates exactly the orders the primary
would have sent, with the same `client_order_id`, because the engine is
deterministic and order IDs come from engine state
(`engine/trading_engine.hpp:138`). Suppose the primary transmitted an order
whose replication record never reached the standby. The standby resends that
order with the same client order ID, and a venue that rejects duplicate
client order IDs drops it. Write-ahead replication makes that window at most
the records still in flight when the primary died.

`--die-after N` models a process crash. The kernel still delivers what the
process had written, so in these tests the standby always holds every record
the primary applied. A host crash can lose records still in flight, and the
duplicate-order argument above covers that case.

## Testing and evidence

| Test | Kind | Pass condition |
|---|---|---|
| `GapTracker`, `FailureDetector`, codecs | unit, no sockets | all orderings, duplicates, overflow, timeouts |
| failover script test (`cmake/RunHaFailover.cmake`) | `ctest`, loopback unicast fan-out, no root | primary dies after record N, standby promotes, final digest == conformance digest |
| gap-fill script test (`cmake/RunHaGapFill.cmake`) | `ctest`, loopback, seeded application-level drop | final digest == lossless digest; every gap recovered |
| failover measurement | namespaces + bridge + multicast, root | detection latency, outage and catch-up, 20 runs at each heartbeat setting, p50/p99/max; false failovers and their split-brain overlap |
| gap-fill measurement | namespaces + `tc netem loss` at 0.1%, 1%, 5%, on feed datagrams only and on all traffic | recovery time p50/p99/max, gaps recovered / gaps seen |

The CI script tests use unicast fan-out on loopback because a CI container
cannot configure multicast routing. The measurements use real multicast on the
bridge. `scripts/ha/setup-netns.sh` builds the topology and
`scripts/ha/run-evidence.sh` collects the runs. Measurements go to
`results/failover/` with the commands used and the environment, following
`benchmarks/methodology.md`. They are taken from a branch revision, not a
tagged release, and say so.

## Open questions for reviewers

1. Is per-record outcome comparison worth its 6 extra bytes over a periodic
   rolling digest in the heartbeat? Per-record comparison catches divergence
   at the first wrong record rather than at the next heartbeat.
2. Should the standby subscribe to the feed from startup (faster promotion,
   more work while idle) or only on promotion (cheaper, but it adds a
   multicast join to the outage)?
3. Self-fencing on a failed replication write makes the primary depend on the
   standby's health. Is "trade only while replicated" the right default, or
   should the primary be allowed to trade alone after an alarm?
