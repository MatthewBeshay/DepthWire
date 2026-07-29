# Replay trace format and digest contract

A DepthWire replay trace (`.dwtrace`) is a bounded, versioned, self-describing
binary file containing market-data messages and their relative arrival times.
It is the only input the replay tooling accepts, and the whole file is
validated before any run (`depthwire::trace::validate_trace`). The binary
container is canonical; a line-oriented text form exists solely for authoring
and inspection and always round-trips through the binary encoder.

Trace format version 1 embeds wire protocol version 1 messages. All integer
fields are big-endian, matching the wire protocol. Version 1 defines no flag
bits anywhere; assigning meaning to any currently-zero bit requires a new
trace format version, never a reinterpretation of accepted bytes.

## Binary container

A trace file is a 76-byte header followed by exactly `record_count` 56-byte
records. The file size must equal `76 + 56 * record_count`; trailing bytes are
rejected.

### Header (76 bytes)

| Offset | Width | Field | Validation |
|---:|---:|---|---|
| 0 | 4 | Magic `0x44575452` (`DWTR`) | must match |
| 4 | 1 | Trace format version | must be 1 |
| 5 | 1 | Embedded wire protocol version | must equal the wire package's version (1) |
| 6 | 2 | Header length | must be 76 |
| 8 | 4 | Record length | must be 56 |
| 12 | 4 | Record count | 1 through 16,777,216 |
| 16 | 8 | Total duration (ns) | must equal the sum of all record deltas |
| 24 | 8 | Maximum arrival delta (ns) | at most 1,000,000,000; bounds every record |
| 32 | 4 | Instrument identifier | every record's instrument must match |
| 36 | 4 | Maximum quantity | bounds every record's quantity |
| 40 | 4 | Flags | must be zero in version 1 |
| 44 | 32 | SHA-256 of the record region | recomputed over bytes 76..end and compared |

The declared bounds (record count, per-record delta, total duration,
instrument, quantity) make every trace bounded by construction: replay tooling
can preallocate all storage from the header before touching a record.

### Record (56 bytes)

| Offset | Width | Field |
|---:|---:|---|
| 0 | 8 | Arrival delta (ns) relative to the previous record's due time; the first record is relative to the run epoch |
| 8 | 48 | The market-data `WireMessage` exactly as the wire codec encodes it |

Records store the 48-byte wire message, not the 62-byte Ethernet frame: the
Ethernet envelope is transport addressing owned by the replay harness
configuration, so one trace drives both the transport-neutral
conformance path and the DPDK-paced path with identical bytes.

## Validation

`validate_trace` checks, in order: container fields (magic, versions, lengths,
count, flags, exact file size), the embedded record-region SHA-256, then every
record (delta within the declared bound, message decodes through the wire
codec, instrument matches, quantity within bound) and finally that the summed
deltas equal the declared total duration. Failures return
`std::expected` errors carrying the failing record index and, for codec
rejections, the wire error.

Sequence continuity is deliberately **not** container validity. Traces
legitimately carry duplicates, gaps, stale updates, and 32-bit sequence wrap
so replay can exercise the engine's rejection and recovery semantics
(the engine's sequence semantics). A malformed *wire message* cannot exist
inside a valid trace by
construction; malformed-input conformance is covered by the validator's
rejection paths, the trace decoder fuzzer, and the wire codec tests.

## Text form

```
depthwire-trace-text v1
wire-version 1
instrument 77
max-delta-ns 1000000
max-quantity 1001
record delta=1000 seq=4294967286 ts=1000 side=sell action=modify level=0 price=100100 qty=10
```

Header lines appear in exactly this order; blank lines and `#` comment lines
are skipped. Record fields appear in exactly the order shown. Records carry no
instrument or flag fields: the instrument is pinned by the header bound and
version-1 flags must be zero. Record count, total duration, and the record
hash are computed by the binary encoder, never hand-authored. Parsing is
strict (`std::from_chars`, full-token match); binary→text→binary and
text→binary→text conversions are byte-exact and tested.

## Canonical run digest

A replay run's digest is the SHA-256 of a canonical byte stream emitted
explicitly byte by byte (never from struct memory), all integers big-endian:

1. Prefix: ASCII `DWDG`, then stream version `u8 = 1`.
2. Per processed input, in processing order: `u32` input sequence number,
   `u8` packet-processor status, `u8` engine status, then the 62 encoded
   order-frame bytes if and only if the packet status is `order_encoded`.
3. Counters block: ASCII `DWCT`, `u32` counter count (16), then the hashed
   counters as `u64` values in the declaration order of
   `depthwire::replay::ReplayCounters`.

Only counters that are pure functions of the accepted input sequence and the
configuration are hashed: offered, accepted, dropped, processed, ordered, the
per-status totals, and the final backlog. Timing-derived values (late
counts, lateness and latency distributions, queue peaks, recovery duration)
are reported but never hashed, because they vary across runs that accepted
identical input.

The numeric values of the two status enums are part of this contract: they
are pinned by `static_assert` in `include/depthwire/replay/run_digest.hpp`
and are append-only. Any change to the stream layout or the hashed-counter
list is a new stream version.

**Determinism claim:** the same accepted input sequence and the same
configuration produce the same digest across processes, compilers, and
machines. Conformance-mode replay processes every record, so repeated
conformance runs must be digest-identical; a lossless paced run at matching
configuration must reproduce the conformance digest, demonstrating that the
digest is independent of transport. Both equalities are asserted in CI.

## Checked-in fixtures

Fixtures live under `tests/fixtures/trace/`, and their construction rules are
code (`tests/support/trace_fixture_builder.hpp`); the
`depthwire_test_trace_fixtures` test proves the checked-in bytes equal the
recorded rule, so a fixture can never drift from its documented construction.

- `conformance_v1.dwtrace` (+ `.txt`, 40 records): seeds the book starting at
  sequence `0xFFFFFFF0` so order-emitting records cross the 32-bit wrap, then
  exercises duplicate, stale, gap → supplied-missing-event → replayed-later-
  event, a wire-valid but book-invalid record, and a normal alternating tail.
- `load_uniform_100k_v1.dwtrace` (100,000 records): ten seed records then
  imbalance stimuli with contiguous sequences at a uniform 1000 ns spacing
  (100 ms total), the base workload for declared-rate sweeps.
