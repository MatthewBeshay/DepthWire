#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include "depthwire/ha/codec.hpp"

namespace {

using namespace depthwire;

wire::WireMessage patterned_message(const std::uint8_t seed)
{
	wire::WireMessage message{};

	for (std::size_t index = 0; index < message.size(); ++index) {
		message[index] = static_cast<std::byte>(seed + index);
	}

	return message;
}

void test_feed_record_round_trip_and_layout()
{
	const ha::FeedRecord record{0x01020304U, patterned_message(7)};
	std::array<std::byte, ha::kFeedRecordBytes> bytes{};
	ha::encode_feed_record(record, bytes);

	assert(bytes[0] == std::byte{0x01} && bytes[3] == std::byte{0x04});
	assert(bytes[4] == std::byte{0} && bytes[7] == std::byte{0});
	assert(bytes[8] == std::byte{7});

	const auto decoded = ha::decode_feed_record(bytes);
	assert(decoded);
	assert(decoded->index == record.index);
	assert(decoded->message == record.message);
}

void test_feed_record_rejects()
{
	std::array<std::byte, ha::kFeedRecordBytes> bytes{};
	ha::encode_feed_record({1, patterned_message(1)}, bytes);

	assert(ha::decode_feed_record(std::span<const std::byte>(bytes).first(ha::kFeedRecordBytes - 1)).error() ==
	       ha::CodecError::wrong_size);

	std::array<std::byte, ha::kFeedRecordBytes + 1> longer{};
	assert(ha::decode_feed_record(longer).error() == ha::CodecError::wrong_size);

	bytes[5] = std::byte{1};
	assert(ha::decode_feed_record(bytes).error() == ha::CodecError::nonzero_reserved);
}

void test_gap_fill_round_trip()
{
	std::array<std::byte, ha::kGapFillRequestBytes> request{};
	ha::encode_gap_fill_request({100, 25}, request);
	const auto decoded = ha::decode_gap_fill_request(request);
	assert(decoded && decoded->first_index == 100 && decoded->count == 25);
	assert(ha::decode_gap_fill_request(std::span<const std::byte>(request).first(7)).error() ==
	       ha::CodecError::wrong_size);

	std::array<std::byte, ha::kGapFillResponseHeaderBytes> header{};
	ha::encode_gap_fill_response_header(0xFFFFFFFFU, header);
	const auto count = ha::decode_gap_fill_response_header(header);
	assert(count && *count == 0xFFFFFFFFU);
	assert(ha::decode_gap_fill_response_header(std::span<const std::byte>(header).first(3)).error() ==
	       ha::CodecError::wrong_size);
}

void test_replication_record_round_trip()
{
	const ha::ReplicationRecord record{42, patterned_message(3), 1, 7};
	std::array<std::byte, ha::kReplicationRecordBytes> bytes{};
	ha::encode_replication_record(record, bytes);

	const auto decoded = ha::decode_replication_record(bytes);
	assert(decoded);
	assert(decoded->index == 42 && decoded->message == record.message);
	assert(decoded->process_status == 1 && decoded->engine_status == 7);
	assert(ha::decode_replication_record(std::span<const std::byte>(bytes).first(53)).error() ==
	       ha::CodecError::wrong_size);
}

void test_heartbeat_round_trip_and_magic()
{
	std::array<std::byte, ha::kHeartbeatBytes> bytes{};
	ha::encode_heartbeat({0x0102030405060708ULL, 99}, bytes);
	assert(bytes[0] == std::byte{'D'} && bytes[3] == std::byte{'B'});

	const auto decoded = ha::decode_heartbeat(bytes);
	assert(decoded && decoded->number == 0x0102030405060708ULL && decoded->applied_count == 99);

	bytes[0] = std::byte{'X'};
	assert(ha::decode_heartbeat(bytes).error() == ha::CodecError::invalid_magic);
	assert(ha::decode_heartbeat(std::span<const std::byte>(bytes).first(15)).error() == ha::CodecError::wrong_size);
}

} // namespace

int main()
{
	test_feed_record_round_trip_and_layout();
	test_feed_record_rejects();
	test_gap_fill_round_trip();
	test_replication_record_round_trip();
	test_heartbeat_round_trip_and_magic();
	return 0;
}
