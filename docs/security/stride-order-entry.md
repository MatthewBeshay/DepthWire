# STRIDE threat model: order-entry path

## Scope

This models the path from an inbound market-data frame to an outbound order
frame in the `depthwire` AF_PACKET application, plus the trace files that the
replay tool reads and that the published results depend on. It is written as if
the engine were attached to a real venue feed and order gateway. Today it runs
only against veth pairs and ring PMDs with synthetic traffic (see
`architecture/packet-pipeline.md`), so the likelihoods below describe that
intended deployment, not the lab.

Line references are to commit `a593d1d` (v1.1.0).

## Data flow and trust boundaries

```text
            TB1: network (any L2 peer on the segment)
 feed publisher --62-byte frame, EtherType 0x88B5--> rte_eth_rx_burst          apps/depthwire-engine/main.cpp:282
                                                       |
                                                       v
                                   decode_market_data_frame             wire/ethernet_frame.hpp:110
                                   destination == local MAC,
                                   source is nonzero unicast            pipeline/packet_processor.hpp:117-125
                                                       |
                                                       v
                                   TradingEngine::on_market_update      engine/trading_engine.hpp:73
                                     sequence check                     engine/trading_engine.hpp:219-240
                                     book -> strategy -> RiskGuard      risk/risk_guard.hpp:45-112
                                                       |
                                                       v
                                   encode_order_frame, destination =
                                   the input frame's source MAC         pipeline/packet_processor.hpp:166-168
                                                       |
 order gateway <------62-byte order frame------- rte_eth_tx_burst       apps/depthwire-engine/main.cpp:180

            TB2: host and operator     EAL arguments -> rte_eal_init     apps/depthwire-engine/main.cpp:192
                                       process runs as root              README.md:268-271
            TB3: filesystem            trace file -> read_file           apps/depthwire-replay/main.cpp:68-93
                                       -> validate_trace (SHA-256)       trace/trace_validator.hpp:42
```

Paths under `include/depthwire/` are abbreviated.

## Threats

Likelihood and impact are rated low, medium or high for the intended
deployment.

| ID | STRIDE | Threat | Where | L | I | Existing control | Mitigation |
|---|---|---|---|---|---|---|---|
| S1 | Spoofing | Any host on the segment can send market data the engine accepts. The only identity checks are on MAC addresses, which are trivially forged. | `packet_processor.hpp:117-125` | M | H | Exact destination MAC; source must be nonzero unicast | Allow-list the publisher's source MAC as a first step; authenticate frames (keyed MAC per frame, rotated keys) if the feed ever crosses a shared segment |
| S2 | Spoofing | Orders go to whichever MAC sent the triggering market data, so a spoofed source redirects live orders to the attacker. | `packet_processor.hpp:166-168` | M | H | None | Send orders only to a configured gateway MAC; never derive the destination from input |
| T1 | Tampering | Market data can be altered in flight. Nothing beyond the Ethernet FCS protects a frame's integrity. | `wire/wire_header.hpp:49-84` | L | H | Strict field validation, zero-flag rule | Same as S1; integrity and authenticity come together |
| T2 | Tampering | The first frame is accepted unconditionally and sets the sequence anchor. A forged first frame 2^31 ahead makes every genuine update "stale". A forged next-sequence frame that arrives first turns the genuine update into a "duplicate". | `trading_engine.hpp:221-239` | M | H | Wrap-aware gap, duplicate and stale classification | Take the expected start sequence from session setup; authenticate frames (S1) |
| T3 | Tampering | The trace record digest is unkeyed SHA-256, so anyone who can edit a trace can recompute it. The published evidence bundles rely on these digests. | `trace_validator.hpp:42` | L | M | Size-bounded read, structural validation, digest | Sign `results/*/sha256-manifest.txt` with a published key; or switch to HMAC-SHA-256 with a key held outside the repository |
| R1 | Repudiation | The live application keeps counters only. No record ties an order to the input that caused it, so a disputed order cannot be explained after the fact. | `main.cpp:38-46` | M | M | `source_sequence` is carried on every order (`trading_engine.hpp:142`) | Append-only journal of input sequence, client order ID, outbound sequence and local time, written off the hot path |
| I1 | Information disclosure | Orders reveal price, size and side in clear text to any observer on the segment, and S2 can redirect them. | `main.cpp:160-186` | L | M | None | Dedicated segment or VLAN for order traffic; fix S2 |
| D1 | Denial of service | One frame with a duplicate, gap or stale sequence makes the application exit. A single forged or lost frame halts trading. | `main.cpp:350-357` | H | M | Fails closed rather than trading on a gapped book | Recover gaps instead of exiting (multicast gap-fill, planned in `architecture/failover.md`); drop duplicates and stale frames with a counter |
| D2 | Denial of service | Order flood: nothing limits order *rate*. The outstanding-order cap only works today because nothing ever acknowledges an order. Once acknowledgements exist it stops limiting anything. | `risk_guard.hpp:12-17,71-73,123-131` | M | H | Per-order quantity, position, notional and outstanding limits | Orders-per-second throttle at the transmit boundary, on local monotonic time. Exchange timestamps cannot be used because a spoofer controls them, so the throttle stays outside the deterministic engine. |
| E1 | Elevation of privilege | The engine runs as root, and EAL arguments pass straight to `rte_eal_init`. A memory-safety bug in decode would become root compromise. | `README.md:268-271`; `main.cpp:192` | L | H | Bounds-checked fixed-size decode; libFuzzer targets; ASan/UBSan preset | Run unprivileged with only `CAP_NET_RAW` and `CAP_IPC_LOCK`; drop capabilities after EAL initialisation |
| E2 | Elevation of privilege (missing control) | The kill switch exists but no application reaches it. Tests are the only callers, so an operator's only way to stop trading is to kill the process. | `risk_guard.hpp:119`; `packet_processor.hpp:225` | M | H | Kill switch in `RiskGuard` | Wire `SIGUSR1` (or a control socket) to `set_kill_switch(true)`, and test that orders stop |

## Priorities

1. **S2, then S1/T1/T2.** A spoofed source MAC redirecting live orders is the
   most damaging finding, and the cheapest to fix.
2. **E2 and D2.** An operator must be able to halt trading, and order rate must
   be bounded independently of acknowledgement handling.
3. **D1.** It is addressed by the gap-fill work planned in `architecture/failover.md`.
4. **E1, R1, T3, I1.** Deployment hardening, needed before any non-lab use.

Each priority item is tracked as a GitHub issue.

## Assumptions and limits

- The host and its kernel are trusted, and so are the veth or NIC driver and
  the DPDK libraries.
- The model does not cover the build supply chain: compilers, DPDK packages
  and GitHub Actions.
- The model does not cover side channels.
- The engine has no session layer (logon, heartbeats, order acknowledgements),
  so threats against an exchange session protocol are out of scope until one
  exists.
