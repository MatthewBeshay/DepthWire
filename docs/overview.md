# DepthWire overview

DepthWire is a focused C++26 packet-processing engine for studying the
latency-sensitive path between a market-data frame and an outbound order.

## Processing model

```text
Ethernet market-data frame
    -> fixed binary decoder
    -> MarketUpdate
    -> fixed-depth L2 book
    -> imbalance strategy
    -> inline risk guard
    -> preallocated order
    -> Ethernet order frame
```

The engine runs one packet to completion on one core. Its hot path avoids heap
allocation, logging, variable-length formats, floating-point prices, databases,
and cross-thread coordination.

## Package responsibilities

| Package | Responsibility |
|---|---|
| `contracts` | Opt-in contract-assertion macro shim (base package) |
| `market` | Domain types and normalised market updates |
| `book` | Fixed-depth bid and ask state |
| `strategy` | Imbalance-based signal generation |
| `order` | Order requests and preallocated output |
| `risk` | Inline order validation and limits |
| `engine` | Domain orchestration |
| `wire` | Fixed binary packet and Ethernet-frame codecs |
| `pipeline` | Translation between wire messages and engine operations |
| `digest` | Dependency-free SHA-256 for trace identity and run digests |
| `trace` | Versioned replay-trace container, validation, and text form |
| `replay` | Monotonic pacer, per-event journal, counters, and run digest |

DPDK is composed by executable, benchmark, and integration-test targets. It is
not a dependency of the public engine packages.

## DPDK modes

### Ring PMD

Two virtual Ethernet devices exchange `rte_mbuf` packets through in-memory
rings. This isolates the application packet path from kernel and physical-NIC
costs.

### AF_PACKET

A DPDK AF_PACKET virtual device communicates over a private Linux `veth` pair:

```text
raw peer on dw_peer
    -> Linux veth
    -> dw_eng
    -> DPDK AF_PACKET RX
    -> DepthWire pipeline
    -> DPDK AF_PACKET TX
    -> raw peer validation
```

This includes a kernel-backed network-interface path, but still does not include
physical NIC DMA, PCIe, a switch, or wire transmission.

## Measurement boundary

The Ring PMD latency benchmark starts after the engine's
`rte_eth_rx_burst()` call has returned a packet and ends after
`rte_eth_tx_burst()` accepts the encoded order. Generator work, RX polling wait,
and order draining are excluded.

The benchmark therefore answers:

> How quickly can DepthWire process an already-received packet and enqueue the
> resulting order through a DPDK-style interface?

It does not answer physical wire-to-wire or exchange round-trip latency.

## Correctness scenarios

The automated suite verifies:

- balanced liquidity produces no order;
- bid imbalance produces BUY;
- ask imbalance produces SELL;
- risk limits reject an otherwise valid order;
- malformed frames are rejected;
- exact local-destination and valid-unicast-source checks;
- contiguous sequence, replay, reset, and transactional-backpressure behaviour;
- a complete virtual Ring PMD RX/TX exchange preserves packet fields.

## Executable roles

| Target | Role |
|---|---|
| `depthwire-replay` | Public trace validate/convert/replay command (installed) |
| `depthwire` | AF_PACKET packet-processing application |
| `depthwire_ring_latency` | Virtual Ring PMD latency benchmark |
| `depthwire_ring_load` | Configurable virtual Ring PMD burst/load benchmark |
| `depthwire_ring_regimes` | NO_SIGNAL, BUY, and SELL regime benchmark |
| `depthwire_afpacket_latency` | Kernel-backed AF_PACKET latency benchmark |
| `depthwire_afpacket_benchmark_generator` | AF_PACKET benchmark peer |
| `depthwire_test_dpdk_scenarios` | Virtual scenario integration test |
| `depthwire_test_dpdk_ring_transport` | Complete virtual RX/TX transport test |
| `depthwire_test_replay_recovery` | Replay recovery-ownership integration test |
| `depthwire_afpacket_test_peer` | Functional AF_PACKET peer |

`depthwire` is deliberately bounded: it exits after one emitted order or a
timeout. It validates the manual AF_PACKET transport but is not a continuously
running service.

## Scope

DepthWire is not a profitable strategy, production OMS, exchange simulator, or
physical-NIC benchmark. It delivers replay-paced offered-load and recovery
evidence as the portfolio endpoint; multi-symbol, richer order lifecycle, 
AF_XDP, and physical-NIC work are optional extensions.

Risk state is intentionally narrow: acceptance immediately updates projected
net position, candidate notional is that absolute projected position times the
candidate limit price, and acknowledgement only decrements the outstanding
count. It does not model fills or exchange-reject rollback.

The Ring load target adds deterministic prequeued bursts, but does not replay
a real arrival schedule or establish line-rate capacity.
