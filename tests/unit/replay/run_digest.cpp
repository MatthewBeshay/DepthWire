#include <cassert>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <vector>

#include "depthwire/digest/sha256.hpp"
#include "depthwire/replay/event_journal.hpp"
#include "depthwire/replay/replay_counters.hpp"
#include "depthwire/replay/run_digest.hpp"

namespace {

using namespace depthwire;

bool hex_equals(const digest::Sha256Digest &digest_value, const std::string_view expected)
{
	const auto hex = digest::to_hex(digest_value);
	return expected.size() == hex.size() && std::memcmp(hex.data(), expected.data(), hex.size()) == 0;
}

// Rebuilds the documented canonical stream by hand and checks that
// compute_run_digest hashes exactly those bytes. This pins the stream layout:
// any accidental change to field order, widths, or the hashed-counter list
// breaks this test.
void test_stream_matches_manual_construction()
{
	replay::EventJournal journal{2};

	replay::JournalEntry first{};
	first.input_sequence = 0x01020304U;
	first.process_status = pipeline::PacketProcessStatus::order_encoded;
	first.engine_status = EngineStatus::order_emitted;
	first.has_frame = true;

	for (std::size_t index = 0; index < first.frame.size(); ++index) {
		first.frame[index] = static_cast<std::byte>(index);
	}

	replay::JournalEntry second{};
	second.input_sequence = 0x0A0B0C0DU;
	second.process_status = pipeline::PacketProcessStatus::engine_rejected;
	second.engine_status = EngineStatus::sequence_gap;

	assert(journal.record(first));
	assert(journal.record(second));

	replay::ReplayCounters counters{};
	counters.offered = 2;
	counters.accepted = 2;
	counters.processed = 2;
	counters.ordered = 1;
	counters.status_order_encoded = 1;
	counters.engine_gap = 1;
	// Reported-only values must not influence the digest.
	counters.late = 77;
	counters.queued_max = 99;
	counters.recovery_duration_ns = 123456;

	std::vector<std::byte> stream{};
	const auto push_u8 = [&stream](const std::uint8_t value) {
		stream.push_back(static_cast<std::byte>(value));
	};
	const auto push_be32 = [&push_u8](const std::uint32_t value) {
		push_u8(static_cast<std::uint8_t>(value >> 24));
		push_u8(static_cast<std::uint8_t>(value >> 16));
		push_u8(static_cast<std::uint8_t>(value >> 8));
		push_u8(static_cast<std::uint8_t>(value));
	};
	const auto push_be64 = [&push_be32](const std::uint64_t value) {
		push_be32(static_cast<std::uint32_t>(value >> 32));
		push_be32(static_cast<std::uint32_t>(value));
	};

	push_u8('D');
	push_u8('W');
	push_u8('D');
	push_u8('G');
	push_u8(1);

	push_be32(first.input_sequence);
	push_u8(1); // order_encoded
	push_u8(1); // order_emitted

	for (const std::byte value : first.frame) {
		stream.push_back(value);
	}

	push_be32(second.input_sequence);
	push_u8(7); // engine_rejected
	push_u8(7); // sequence_gap

	push_u8('D');
	push_u8('W');
	push_u8('C');
	push_u8('T');
	push_be32(replay::kHashedCounterCount);
	push_be64(counters.offered);
	push_be64(counters.accepted);
	push_be64(counters.dropped);
	push_be64(counters.processed);
	push_be64(counters.ordered);
	push_be64(counters.status_no_order);
	push_be64(counters.status_order_encoded);
	push_be64(counters.status_decode_rejected);
	push_be64(counters.status_adapter_rejected);
	push_be64(counters.status_other);
	push_be64(counters.engine_duplicate);
	push_be64(counters.engine_gap);
	push_be64(counters.engine_stale);
	push_be64(counters.engine_invalid);
	push_be64(counters.engine_other);
	push_be64(counters.backlog_final);

	assert(replay::compute_run_digest(journal, counters) == digest::sha256_of(stream));
}

// Reported-only counters must never change the digest.
void test_unhashed_counters_do_not_affect_digest()
{
	replay::EventJournal journal{1};
	replay::JournalEntry entry{};
	entry.input_sequence = 42;
	assert(journal.record(entry));

	replay::ReplayCounters counters{};
	counters.offered = 1;
	counters.accepted = 1;
	counters.processed = 1;
	counters.status_no_order = 1;

	const auto baseline = replay::compute_run_digest(journal, counters);

	counters.late = 1000;
	counters.queued_max = 555;
	counters.orders_tx_dropped = 3;
	counters.recovery_duration_ns = 9999999;
	assert(replay::compute_run_digest(journal, counters) == baseline);

	// Hashed counters must change it.
	counters.dropped = 1;
	assert(replay::compute_run_digest(journal, counters) != baseline);
}

// Empty-journal digest with zero counters is a stable stream vector.
void test_empty_run_digest_vector()
{
	const replay::EventJournal journal{0};
	const replay::ReplayCounters counters{};

	// Stream: "DWDG" 0x01 "DWCT" u32(16) + 16 zero u64s.
	std::vector<std::byte> stream{};

	for (const char character : std::string_view{"DWDG"}) {
		stream.push_back(static_cast<std::byte>(character));
	}

	stream.push_back(std::byte{1});

	for (const char character : std::string_view{"DWCT"}) {
		stream.push_back(static_cast<std::byte>(character));
	}

	stream.push_back(std::byte{0});
	stream.push_back(std::byte{0});
	stream.push_back(std::byte{0});
	stream.push_back(std::byte{16});

	for (std::size_t index = 0; index < std::size_t{16} * 8; ++index) {
		stream.push_back(std::byte{0});
	}

	const auto expected = digest::sha256_of(stream);
	assert(replay::compute_run_digest(journal, counters) == expected);
	assert(!hex_equals(expected, "0000000000000000000000000000000000000000000000000000000000000000"));
}

} // namespace

int main()
{
	test_stream_matches_manual_construction();
	test_unhashed_counters_do_not_affect_digest();
	test_empty_run_digest_vector();
	return 0;
}
