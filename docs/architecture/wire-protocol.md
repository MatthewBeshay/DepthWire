# DepthWire wire protocol

## Purpose

DepthWire uses fixed-size binary messages for the DPDK packet hot path.

## Ethernet Frame Layout

| Region | Bytes |
|---|---:|
| Ethernet header | 14 |
| DepthWire header | 16 |
| DepthWire payload | 32 |
| Total excluding Ethernet FCS | 62 |

## DepthWire Header

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 2 | Magic: 0x5042 |
| 2 | 1 | Protocol version |
| 3 | 1 | Message type |
| 4 | 2 | Header length |
| 6 | 2 | Payload length |
| 8 | 4 | Sequence number |
| 12 | 4 | Flags |

Protocol V1 defines no header flag bits. The field must be zero; encoders and
decoders reject any nonzero value rather than ignoring unknown semantics.

## Message Types

| Value | Meaning |
|---:|---|
| 1 | Market data |
| 2 | New order |

## Market Data Payload

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 8 | Exchange timestamp in nanoseconds |
| 8 | 4 | Instrument ID |
| 12 | 8 | Price ticks |
| 20 | 4 | Quantity |
| 24 | 1 | Side |
| 25 | 1 | Update action |
| 26 | 2 | Level |
| 28 | 4 | Source flags |

Protocol V1 defines no market-data source flag bits. `source_flags` must be
zero.

## Outbound Order Payload

| Offset | Bytes | Field |
|---:|---:|---|
| 0 | 8 | Client order ID |
| 8 | 4 | Instrument ID |
| 12 | 8 | Price ticks |
| 20 | 4 | Quantity |
| 24 | 1 | Side |
| 25 | 1 | Order type |
| 26 | 1 | Time in force |
| 27 | 1 | Order flags |
| 28 | 4 | Source market-data sequence number |

Protocol V1 defines no order flag bits. `order_flags` must be zero.

## Hot-Path Rules

- No heap allocation.
- No string formatting.
- No logging.
- No variable-length packet parsing.
- No exceptions.
- All integer fields are encoded in big-endian order.

The outbound `source_sequence` correlates an order with the market-data update
that produced it. It is validated by the burst/load benchmark so duplicated or
reordered responses cannot satisfy a different input silently.

The transport-neutral packet processor accepts a frame only when its Ethernet
destination exactly matches the configured local MAC and its source is nonzero
and unicast. An outbound frame uses that configured local MAC as its source and
the validated input source as its destination. This prevents a received
destination address from being copied blindly into the outbound source field.
