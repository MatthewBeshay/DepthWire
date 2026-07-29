#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "depthwire/digest/sha256.hpp"
#include "depthwire/trace/trace_codec.hpp"
#include "depthwire/trace/trace_format.hpp"
#include "depthwire/trace/trace_validator.hpp"
#include "depthwire/wire/market_data_packet.hpp"
#include "depthwire/wire/wire_header.hpp"

namespace {

using namespace depthwire::trace;

depthwire::wire::MarketDataMessage test_message(const std::uint32_t sequence) noexcept
{
	depthwire::wire::MarketDataMessage message{};
	message.sequence_number = sequence;
	message.exchange_timestamp_ns = static_cast<std::uint64_t>(sequence) * 1000ULL;
	message.instrument_id = 77;
	message.price_ticks = 100000;
	message.quantity = 500;
	message.side = depthwire::wire::Side::buy;
	message.action = depthwire::wire::UpdateAction::modify;
	message.level = 0;
	return message;
}

TraceBounds test_bounds() noexcept
{
	TraceBounds bounds{};
	bounds.max_arrival_delta_ns = 10'000;
	bounds.instrument_id = 77;
	bounds.max_quantity = 1000;
	return bounds;
}

std::vector<std::byte> valid_file()
{
	const std::array<TraceRecord, 3> records{
		TraceRecord{0, test_message(1)},
		TraceRecord{1000, test_message(2)},
		TraceRecord{10'000, test_message(3)},
	};

	std::vector<std::byte> file(trace_file_bytes(records.size()));
	const auto header = encode_trace(records, test_bounds(), file);
	assert(header);
	return file;
}

// Re-embeds the record-region hash after a record byte has been tampered
// with, so validation proceeds past the digest check to the per-record rules.
void rehash(std::vector<std::byte> &file)
{
	const auto region_hash =
		depthwire::digest::sha256_of(std::span<const std::byte>(file).subspan(kTraceHeaderBytes));

	for (std::size_t index = 0; index < region_hash.size(); ++index) {
		file[44 + index] = region_hash[index];
	}
}

void expect_error(const std::span<const std::byte> file, const TraceError expected,
		  const std::uint32_t expected_index = 0)
{
	const auto result = validate_trace(file);
	assert(!result);
	assert(result.error().error == expected);
	assert(result.error().record_index == expected_index);
}

void store_be32_at(std::vector<std::byte> &file, const std::size_t offset, const std::uint32_t value)
{
	for (std::size_t index = 0; index < 4; ++index) {
		file[offset + index] = static_cast<std::byte>(value >> (24 - 8 * index));
	}
}

void store_be64_at(std::vector<std::byte> &file, const std::size_t offset, const std::uint64_t value)
{
	for (std::size_t index = 0; index < 8; ++index) {
		file[offset + index] = static_cast<std::byte>(value >> (56 - 8 * index));
	}
}

void test_valid_trace_summary()
{
	const auto file = valid_file();
	const auto summary = validate_trace(file);
	assert(summary);
	assert(summary->header.record_count == 3);
	assert(summary->header.total_duration_ns == 11'000);
	assert(summary->header.bounds.instrument_id == 77);
	assert(summary->min_arrival_delta_ns == 0);
	assert(summary->max_arrival_delta_ns == 10'000);
	assert(summary->max_quantity == 500);
}

// Every case below builds its own fresh file instead of mutating and
// restoring one shared vector, so each rejection is independently readable.
// Note: this translation unit is compiled with -fno-ipa-modref on GCC; see
// tests/CMakeLists.txt for the GCC 16.1.1 miscompilation it works around.
void test_container_errors()
{
	expect_error({}, TraceError::too_short);

	{
		const auto file = valid_file();
		expect_error(std::span<const std::byte>(file).first(kTraceHeaderBytes - 1), TraceError::too_short);
	}

	{
		const auto file = valid_file();
		expect_error(std::span<const std::byte>(file).first(file.size() - 1), TraceError::too_short);
	}

	{
		auto file = valid_file();
		file.push_back(std::byte{});
		expect_error(file, TraceError::trailing_bytes);
	}

	{
		auto file = valid_file();
		file[0] = std::byte{0x45};
		expect_error(file, TraceError::invalid_magic);
	}

	{
		auto file = valid_file();
		file[4] = std::byte{2};
		expect_error(file, TraceError::unsupported_version);
	}

	{
		auto file = valid_file();
		file[5] = std::byte{2};
		expect_error(file, TraceError::unsupported_wire_version);
	}

	{
		auto file = valid_file();
		file[7] = std::byte{0x4D};
		expect_error(file, TraceError::invalid_header_length);
	}

	{
		auto file = valid_file();
		file[11] = std::byte{0x39};
		expect_error(file, TraceError::invalid_record_length);
	}

	{
		auto file = valid_file();
		store_be32_at(file, 12, 0);
		expect_error(file, TraceError::invalid_record_count);
	}

	{
		auto file = valid_file();
		store_be32_at(file, 12, kMaxTraceRecords + 1);
		expect_error(file, TraceError::invalid_record_count);
	}

	{
		auto file = valid_file();
		store_be64_at(file, 24, kMaxArrivalDeltaNs + 1);
		expect_error(file, TraceError::delta_bound_invalid);
	}

	{
		auto file = valid_file();
		store_be32_at(file, 40, 1);
		expect_error(file, TraceError::invalid_flags);
	}

	// A record count that understates the file size is trailing bytes; one
	// that overstates it is a truncated file.
	{
		auto file = valid_file();
		store_be32_at(file, 12, 2);
		expect_error(file, TraceError::trailing_bytes);
	}

	{
		auto file = valid_file();
		store_be32_at(file, 12, 4);
		expect_error(file, TraceError::too_short);
	}
}

void test_record_errors()
{
	// Tampered record byte without rehashing: digest mismatch.
	{
		auto file = valid_file();
		file[kTraceHeaderBytes + kTraceRecordMessageOffset + 20] ^= std::byte{0xFF};
		expect_error(file, TraceError::record_digest_mismatch);
	}

	// Same tamper with the hash re-embedded: the record's wire magic breaks.
	{
		auto file = valid_file();
		file[kTraceHeaderBytes + kTraceRecordBytes + kTraceRecordMessageOffset] ^= std::byte{0xFF};
		rehash(file);
		const auto result = validate_trace(file);
		assert(!result);
		assert(result.error().error == TraceError::record_decode_rejected);
		assert(result.error().record_index == 1);
		assert(result.error().wire_error == depthwire::wire::WireError::invalid_magic);
	}

	// A record delta above the declared bound (header bound lowered under the
	// third record's 10'000 ns delta; the record region is untouched).
	{
		auto file = valid_file();
		store_be64_at(file, 24, 9'999);
		expect_error(file, TraceError::delta_bound_exceeded, 2);
	}

	// Header instrument no longer matches the records.
	{
		auto file = valid_file();
		store_be32_at(file, 32, 78);
		expect_error(file, TraceError::instrument_mismatch, 0);
	}

	// Header quantity bound lowered under the records' quantity of 500.
	{
		auto file = valid_file();
		store_be32_at(file, 36, 499);
		expect_error(file, TraceError::quantity_bound_exceeded, 0);
	}

	// Header duration that does not equal the record sum.
	{
		auto file = valid_file();
		store_be64_at(file, 16, 11'001);
		expect_error(file, TraceError::duration_mismatch);
	}
}

void test_encode_rejections()
{
	const auto bounds = test_bounds();
	std::array<TraceRecord, 1> records{TraceRecord{0, test_message(1)}};
	std::vector<std::byte> file(trace_file_bytes(1));

	records[0].arrival_delta_ns = bounds.max_arrival_delta_ns + 1;
	assert(encode_trace(records, bounds, file).error().error == TraceError::delta_bound_exceeded);
	records[0].arrival_delta_ns = 0;

	records[0].message.instrument_id = 78;
	assert(encode_trace(records, bounds, file).error().error == TraceError::instrument_mismatch);
	records[0].message.instrument_id = 77;

	records[0].message.quantity = bounds.max_quantity + 1;
	assert(encode_trace(records, bounds, file).error().error == TraceError::quantity_bound_exceeded);
	records[0].message.quantity = 500;

	records[0].message.side = static_cast<depthwire::wire::Side>(9);
	{
		const auto result = encode_trace(records, bounds, file);
		assert(!result);
		assert(result.error().error == TraceError::record_encode_rejected);
		assert(result.error().wire_error == depthwire::wire::WireError::invalid_field);
	}
	records[0].message.side = depthwire::wire::Side::buy;

	TraceBounds invalid_bounds = bounds;
	invalid_bounds.max_arrival_delta_ns = kMaxArrivalDeltaNs + 1;
	assert(encode_trace(records, invalid_bounds, file).error().error == TraceError::delta_bound_invalid);

	assert(encode_trace({}, bounds, {}).error().error == TraceError::invalid_record_count);

	std::vector<std::byte> wrong_size(trace_file_bytes(1) - 1);
	assert(encode_trace(records, bounds, wrong_size).error().error == TraceError::too_short);
}

} // namespace

int main()
{
	test_valid_trace_summary();
	test_container_errors();
	test_record_errors();
	test_encode_rejections();
	return 0;
}
