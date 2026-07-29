#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "depthwire/digest/sha256.hpp"
#include "depthwire/trace/trace_codec.hpp"
#include "depthwire/trace/trace_format.hpp"
#include "depthwire/wire/market_data_packet.hpp"

namespace {

constexpr std::byte byte(const unsigned int value) noexcept
{
	return static_cast<std::byte>(value);
}

depthwire::wire::MarketDataMessage golden_message(const std::uint32_t sequence, const std::uint32_t quantity) noexcept
{
	depthwire::wire::MarketDataMessage message{};
	message.sequence_number = sequence;
	message.exchange_timestamp_ns = 0x0102030405060708ULL;
	message.instrument_id = 77;
	message.price_ticks = 100000;
	message.quantity = quantity;
	message.side = depthwire::wire::Side::buy;
	message.action = depthwire::wire::UpdateAction::modify;
	message.level = 0;
	return message;
}

} // namespace

int main()
{
	using namespace depthwire::trace;

	static_assert(kTraceVersion1 == 1);
	static_assert(kTraceHeaderBytes == 76);
	static_assert(kTraceRecordBytes == 56);
	static_assert(trace_file_bytes(2) == 188);

	std::array<TraceRecord, 2> records{
		TraceRecord{1000, golden_message(0x01020304U, 1000)},
		TraceRecord{0x0000000000000400ULL, golden_message(0x01020305U, 1001)},
	};

	TraceBounds bounds{};
	bounds.max_arrival_delta_ns = 0x1000;
	bounds.instrument_id = 77;
	bounds.max_quantity = 1001;

	std::vector<std::byte> file(trace_file_bytes(2));
	const auto header = encode_trace(records, bounds, file);
	assert(header);
	assert(header->record_count == 2);
	assert(header->total_duration_ns == 1000 + 0x400);

	// Header prefix, byte-exact (offsets 0..43; the trailing 32 bytes are the
	// record-region SHA-256, asserted separately below).
	const std::array<std::byte, 44> expected_prefix{
		byte(0x44), byte(0x57), byte(0x54), byte(0x52), // magic "DWTR"
		byte(0x01),                                     // trace version
		byte(0x01),                                     // wire version
		byte(0x00), byte(0x4C),                         // header length 76
		byte(0x00), byte(0x00), byte(0x00), byte(0x38), // record length 56
		byte(0x00), byte(0x00), byte(0x00), byte(0x02), // record count
		byte(0x00), byte(0x00), byte(0x00), byte(0x00), // total duration ns
		byte(0x00), byte(0x00), byte(0x07), byte(0xE8),
		byte(0x00), byte(0x00), byte(0x00), byte(0x00), // max arrival delta ns
		byte(0x00), byte(0x00), byte(0x10), byte(0x00),
		byte(0x00), byte(0x00), byte(0x00), byte(0x4D), // instrument id 77
		byte(0x00), byte(0x00), byte(0x03), byte(0xE9), // max quantity 1001
		byte(0x00), byte(0x00), byte(0x00), byte(0x00), // flags
	};

	for (std::size_t index = 0; index < expected_prefix.size(); ++index) {
		assert(file[index] == expected_prefix[index]);
	}

	// Record 0: big-endian arrival delta, then the wire-encoded message bytes.
	const std::array<std::byte, 8> expected_delta{
		byte(0x00), byte(0x00), byte(0x00), byte(0x00), byte(0x00), byte(0x00), byte(0x03), byte(0xE8),
	};

	for (std::size_t index = 0; index < expected_delta.size(); ++index) {
		assert(file[kTraceHeaderBytes + index] == expected_delta[index]);
	}

	depthwire::wire::WireMessage expected_message{};
	assert(depthwire::wire::encode_market_data(records[0].message, expected_message));

	for (std::size_t index = 0; index < expected_message.size(); ++index) {
		assert(file[kTraceHeaderBytes + kTraceRecordMessageOffset + index] == expected_message[index]);
	}

	// The embedded hash is the SHA-256 of the record region.
	const auto region_hash =
		depthwire::digest::sha256_of(std::span<const std::byte>(file).subspan(kTraceHeaderBytes));
	assert(header->records_sha256 == region_hash);

	for (std::size_t index = 0; index < region_hash.size(); ++index) {
		assert(file[44 + index] == region_hash[index]);
	}

	const auto decoded = decode_trace_header(file);
	assert(decoded);
	assert(decoded->record_count == header->record_count);
	assert(decoded->total_duration_ns == header->total_duration_ns);
	assert(decoded->bounds.max_arrival_delta_ns == bounds.max_arrival_delta_ns);
	assert(decoded->bounds.instrument_id == bounds.instrument_id);
	assert(decoded->bounds.max_quantity == bounds.max_quantity);
	assert(decoded->records_sha256 == header->records_sha256);

	const TraceView view{std::span<const std::byte>(file)};
	assert(view.record_count() == 2);
	assert(view.arrival_delta_ns(0) == 1000);
	assert(view.arrival_delta_ns(1) == 0x400);

	const auto message_bytes = view.message_bytes(1);
	const auto second = depthwire::wire::decode_market_data(message_bytes);
	assert(second);
	assert(second->sequence_number == 0x01020305U);
	assert(second->quantity == 1001);

	return 0;
}
