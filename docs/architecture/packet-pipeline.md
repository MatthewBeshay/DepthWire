# Packet pipeline

## Scope

DepthWire is a controlled, single-symbol packet-processing study. It
translates one fixed-size Ethernet market-data frame into either no output or one
fixed-size order frame. It is not an order-management system, exchange session,
or production trading service.

## Processing boundary

```text
rte_eth_rx_burst
    -> validate Ethernet and DepthWire headers
    -> require the configured local destination and a nonzero unicast source
    -> decode fixed-offset market-data fields
    -> translate wire values to MarketUpdate
    -> update the five-level L2 book
    -> evaluate imbalance
    -> validate and commit bounded risk state
    -> push/pop the fixed-capacity outbox
    -> translate OrderRequest to wire values
    -> encode a 62-byte Ethernet order frame
    -> allocate an mbuf from the preallocated DPDK pool
    -> copy the order frame
    -> rte_eth_tx_burst
```

The domain engine and codecs are public, ordinary C++26 header-library targets.
The public packet processor composes decode, adaptation, engine state, and
encode without a transport dependency. DPDK owns transport, mbufs, device
configuration, and polling only in applications, integration tests, benchmarks, 
and non-installed support code.

## Ownership and lifetime

- An RX mbuf is owned by DepthWire after `rte_eth_rx_burst()` returns it and is
  freed after decode. The bounded AF_PACKET app rejects multi-segment or
  otherwise non-contiguous RX mbufs before constructing a packet span.
- An outbound mbuf is allocated from a pre-created mempool. Ownership transfers
  to DPDK only when `rte_eth_tx_burst()` accepts it; an unaccepted mbuf is freed
  by the caller.
- The engine outbox has compile-time capacity. It stores values and performs no
  heap allocation.
- Ring PMD support closes every successfully created port before freeing its
  rings and mempool, including partial-initialisation paths.
- One engine and one symbol run on one lcore. There is no shared mutable
  cross-thread engine state.
- The AF_PACKET app reads the selected port's MAC and configures it as the
  packet processor's exact local destination and outbound source.

## Failure semantics

Malformed Ethernet or DepthWire input, a destination other than the configured
local MAC, and a zero or multicast source are rejected before they reach
strategy or risk logic. Protocol V1 also rejects every nonzero header,
market-source, or order flag because no V1 flag bits are defined. Domain results
distinguish:

- `no_order`;
- `order_emitted`;
- `invalid_update`;
- `risk_rejected`;
- `outbox_full`;
- `invalid_configuration`;
- `duplicate_update`;
- `sequence_gap`;
- `stale_update`.

Contiguous input is the default. Sequence rejection and a full outbox do not
partly mutate engine state, so the missing or backpressured event can be
replayed. The complete contract, including wrap-aware comparison and
thread-confinement, is recorded in
the engine's sequence and backpressure contract (`trading_engine.hpp`).

An application-side TX failure is a failed delivery attempt. The bounded
AF_PACKET validation processor exits non-zero rather than retrying an order whose
delivery state is uncertain. It deliberately stops after one emitted order; it
is a functional transport check, not a continuously supervised service.

The benchmarks fail non-zero on unexpected decode, strategy, order, or transport
behaviour. The Ring burst/load benchmark separately counts injection and TX
drops, and validates each accepted order against its source sequence.

## Risk semantics

The `RiskGuard` is a bounded pre-order guard, not an OMS position model.
When it accepts an order, it immediately applies the signed quantity to its
projected net position and increments the outstanding-order count. The notional
test is:

```text
abs(projected net position) * candidate limit price
```

with overflow-safe comparison against `max_notional_ticks`. It is not
mark-to-market portfolio value or the sum of open-order notionals.

Acknowledging an order only decrements the outstanding-order count. It does not
represent a fill, change projected position, or roll position back after an
exchange reject. There is no fill, cancel, reject-reconciliation, or open-order
exposure ledger, so the risk checks must not be described as OMS-grade risk or
complete trading safety.

## Measurement views

DepthWire exposes three different views and does not merge them:

1. Ring service latency: one already-received packet through TX acceptance with
   the generator and queue wait excluded.
2. Ring regime latency: NO_SIGNAL ends at the decision, while BUY and SELL end at
   TX acceptance.
3. Ring burst/load latency: a burst is queued before processing. It reports both
   per-packet RX-to-TX service latency and burst-accept-to-TX queue-plus-service
   latency, plus closed-loop throughput and drops.

AF_PACKET adds a kernel-backed veth/packet-socket transport, but its latency
timer still starts after `rte_eth_rx_burst()` has returned and ends at TX
acceptance. None of these views includes a physical NIC, DMA, PCIe, a switch,
wire transmission, exchange acknowledgement, or round-trip latency.

## Deliberate constraints

- one instrument and five visible levels;
- synthetic deterministic market updates;
- no order acknowledgements, fills, cancels, or recovery protocol;
- no physical-NIC or hardware-timestamp measurement;
- no claim of line-rate capacity or production tail latency.

Replay arrival scheduling and overload recovery are delivered by the
`depthwire-replay` command, which adds a fourth measurement view over this
same pipeline: replay-paced offered load with service, queue-plus-service,
and injection-lateness distributions plus exact loss accounting.
Multi-symbol state, order lifecycle, and supported-NIC
measurement remain optional extensions.
