#pragma once

#include <cstdint>
#include <vector>

#include "depthwire/trace/trace_format.hpp"
#include "depthwire/wire/market_data_packet.hpp"
#include "depthwire/wire/wire_constants.hpp"

// Canonical construction rules for the checked-in trace fixtures under
// tests/fixtures/trace/. The binaries in the tree are canonical; these
// builders exist so tests can prove the checked-in bytes match their recorded
// construction rule (docs/architecture/trace-format.md), per the scope
// document's
// requirement that synthetic traces record how they were built.
namespace depthwire::test_support {

inline constexpr std::uint32_t kFixtureInstrumentId = 77;

// The conformance fixture seeds the book just below the 32-bit sequence wrap
// so its order-emitting records cross 0xFFFFFFFF -> 0x00000000, then exercises
// duplicate, stale, gap -> supply-missing -> replay-later, and a wire-valid
// but book-invalid record, before a normal alternating tail.
inline constexpr std::uint32_t kConformanceFirstSequence = 0xFFFFFFF0U;

[[nodiscard]] inline trace::TraceBounds conformance_trace_bounds() noexcept
{
	trace::TraceBounds bounds{};
	bounds.max_arrival_delta_ns = 1'000'000;
	bounds.instrument_id = kFixtureInstrumentId;
	bounds.max_quantity = 1001;
	return bounds;
}

namespace fixture_detail {

[[nodiscard]] inline wire::MarketDataMessage fixture_message(const std::uint32_t sequence, const wire::Side side,
							     const std::uint16_t level, const std::int64_t price_ticks,
							     const std::uint32_t quantity) noexcept
{
	wire::MarketDataMessage message{};
	message.sequence_number = sequence;
	message.exchange_timestamp_ns = static_cast<std::uint64_t>(sequence) * 1000ULL;
	message.instrument_id = kFixtureInstrumentId;
	message.price_ticks = price_ticks;
	message.quantity = quantity;
	message.side = side;
	message.action = wire::UpdateAction::modify;
	message.level = level;
	return message;
}

// Ten balanced records (five sell levels, then five buy levels, quantity 10)
// mirroring the burst benchmark's book seeding.
inline void append_seed_records(std::vector<trace::TraceRecord> &records, std::uint32_t &sequence)
{
	for (std::uint16_t level = 0; level < 5; ++level) {
		records.push_back({1000, fixture_message(sequence++, wire::Side::sell, level, 100100 + level, 10)});
	}

	for (std::uint16_t level = 0; level < 5; ++level) {
		records.push_back({1000, fixture_message(sequence++, wire::Side::buy, level, 100000 - level, 10)});
	}
}

// A buy-side level-0 modify whose quantity forces the imbalance strategy to
// emit a BUY order (the burst benchmark's proven stimulus).
[[nodiscard]] inline wire::MarketDataMessage imbalance_message(const std::uint32_t sequence) noexcept
{
	return fixture_message(sequence, wire::Side::buy, 0, 100000, 1000U + (sequence & 1U));
}

// A buy-side level-0 modify restoring the balanced book (no signal).
[[nodiscard]] inline wire::MarketDataMessage balance_message(const std::uint32_t sequence) noexcept
{
	return fixture_message(sequence, wire::Side::buy, 0, 100000, 10);
}

} // namespace fixture_detail

[[nodiscard]] inline std::vector<trace::TraceRecord> conformance_trace_records()
{
	using namespace fixture_detail;

	std::vector<trace::TraceRecord> records{};
	records.reserve(40);

	std::uint32_t sequence = kConformanceFirstSequence;
	append_seed_records(records, sequence); // 1-10: no_order

	// 11-16: orders up to and including sequence 0xFFFFFFFF.
	for (int count = 0; count < 6; ++count) {
		records.push_back({1000, imbalance_message(sequence++)});
	}

	// 17-22: orders across the wrap (sequences 0..5).
	for (int count = 0; count < 6; ++count) {
		records.push_back({1000, imbalance_message(sequence++)});
	}

	records.push_back({0, imbalance_message(5)});           // 23: duplicate_update
	records.push_back({0, imbalance_message(0xFFFFFFFAU)}); // 24: stale_update
	records.push_back({1'000'000, imbalance_message(7)});   // 25: sequence_gap (6 missing)
	records.push_back({1000, imbalance_message(6)});        // 26: supplied missing event
	records.push_back({1000, imbalance_message(7)});        // 27: later event replayed

	// 28: wire-valid but book-invalid (level beyond visible depth); the
	// sequence anchor must not advance, so 29 re-sends sequence 8 validly.
	records.push_back({1000, fixture_message(8, wire::Side::buy, 9, 100000, 10)});
	records.push_back({1000, balance_message(8)});                                  // 29: no_order
	records.push_back({1000, fixture_message(9, wire::Side::sell, 0, 100100, 10)}); // 30

	// 31-40: normal alternating tail (order, no_order, ...).
	for (std::uint32_t tail_sequence = 10; tail_sequence < 20; ++tail_sequence) {
		if ((tail_sequence & 1U) == 0) {
			records.push_back({1000, imbalance_message(tail_sequence)});
		} else {
			records.push_back({1000, balance_message(tail_sequence)});
		}
	}

	return records;
}

[[nodiscard]] inline trace::TraceBounds load_trace_bounds() noexcept
{
	return conformance_trace_bounds();
}

inline constexpr std::uint32_t kLoadTraceRecordCount = 100'000;
inline constexpr std::uint64_t kLoadTraceDeltaNs = 1000;

// Uniform offered-load fixture: ten seed records, then imbalance stimuli with
// contiguous sequences at a fixed 1000 ns spacing (100 ms total).
[[nodiscard]] inline std::vector<trace::TraceRecord> load_trace_records()
{
	using namespace fixture_detail;

	std::vector<trace::TraceRecord> records{};
	records.reserve(kLoadTraceRecordCount);

	std::uint32_t sequence = 1;
	append_seed_records(records, sequence);

	while (records.size() < kLoadTraceRecordCount) {
		records.push_back({kLoadTraceDeltaNs, imbalance_message(sequence++)});
	}

	return records;
}

} // namespace depthwire::test_support
