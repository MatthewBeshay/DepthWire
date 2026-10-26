#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "depthwire/wire/endian.hpp"
#include "depthwire/wire/wire_constants.hpp"

namespace depthwire::ha {

// Fixed-size big-endian formats for the failover and gap-fill transport
// (docs/architecture/failover.md). The transport index is the trace record
// index, kept separate from the wire message's own sequence number so that
// transport loss is detected independently of the engine's sequence checks.
inline constexpr std::size_t kFeedRecordBytes = 8 + wire::kWireMessageBytes;
inline constexpr std::size_t kGapFillRequestBytes = 8;
inline constexpr std::size_t kGapFillResponseHeaderBytes = 4;
inline constexpr std::size_t kReplicationRecordBytes = 4 + wire::kWireMessageBytes + 2;
inline constexpr std::size_t kHeartbeatBytes = 16;
inline constexpr std::uint32_t kHeartbeatMagic = 0x44574842; // "DWHB"

enum class CodecError : std::uint8_t {
	wrong_size = 1,
	nonzero_reserved,
	invalid_magic,
};

struct FeedRecord {
	std::uint32_t index{};
	wire::WireMessage message{};
};

struct GapFillRequest {
	std::uint32_t first_index{};
	std::uint32_t count{};
};

struct ReplicationRecord {
	std::uint32_t index{};
	wire::WireMessage message{};
	std::uint8_t process_status{};
	std::uint8_t engine_status{};
};

struct Heartbeat {
	std::uint64_t number{};
	std::uint32_t applied_count{};
};

namespace detail {

inline void copy_message(const std::span<const std::byte> input, const std::size_t offset,
			 wire::WireMessage &output) noexcept
{
	for (std::size_t index = 0; index < output.size(); ++index) {
		output[index] = input[offset + index];
	}
}

inline void store_message(const std::span<std::byte> output, const std::size_t offset,
			  const wire::WireMessage &message) noexcept
{
	for (std::size_t index = 0; index < message.size(); ++index) {
		output[offset + index] = message[index];
	}
}

} // namespace detail

inline void encode_feed_record(const FeedRecord &record, const std::span<std::byte, kFeedRecordBytes> output) noexcept
{
	wire::detail::store_be32(output, 0, record.index);
	wire::detail::store_be32(output, 4, 0);
	detail::store_message(output, 8, record.message);
}

[[nodiscard]] inline std::expected<FeedRecord, CodecError>
	decode_feed_record(const std::span<const std::byte> input) noexcept
{
	if (input.size() != kFeedRecordBytes) {
		return std::unexpected(CodecError::wrong_size);
	}

	if (wire::detail::load_be32(input, 4) != 0) {
		return std::unexpected(CodecError::nonzero_reserved);
	}

	FeedRecord record{};
	record.index = wire::detail::load_be32(input, 0);
	detail::copy_message(input, 8, record.message);
	return record;
}

inline void encode_gap_fill_request(const GapFillRequest &request,
				    const std::span<std::byte, kGapFillRequestBytes> output) noexcept
{
	wire::detail::store_be32(output, 0, request.first_index);
	wire::detail::store_be32(output, 4, request.count);
}

[[nodiscard]] inline std::expected<GapFillRequest, CodecError>
	decode_gap_fill_request(const std::span<const std::byte> input) noexcept
{
	if (input.size() != kGapFillRequestBytes) {
		return std::unexpected(CodecError::wrong_size);
	}

	return GapFillRequest{wire::detail::load_be32(input, 0), wire::detail::load_be32(input, 4)};
}

inline void encode_gap_fill_response_header(const std::uint32_t count_available,
					    const std::span<std::byte, kGapFillResponseHeaderBytes> output) noexcept
{
	wire::detail::store_be32(output, 0, count_available);
}

[[nodiscard]] inline std::expected<std::uint32_t, CodecError>
	decode_gap_fill_response_header(const std::span<const std::byte> input) noexcept
{
	if (input.size() != kGapFillResponseHeaderBytes) {
		return std::unexpected(CodecError::wrong_size);
	}

	return wire::detail::load_be32(input, 0);
}

inline void encode_replication_record(const ReplicationRecord &record,
				      const std::span<std::byte, kReplicationRecordBytes> output) noexcept
{
	wire::detail::store_be32(output, 0, record.index);
	detail::store_message(output, 4, record.message);
	output[4 + wire::kWireMessageBytes] = static_cast<std::byte>(record.process_status);
	output[5 + wire::kWireMessageBytes] = static_cast<std::byte>(record.engine_status);
}

[[nodiscard]] inline std::expected<ReplicationRecord, CodecError>
	decode_replication_record(const std::span<const std::byte> input) noexcept
{
	if (input.size() != kReplicationRecordBytes) {
		return std::unexpected(CodecError::wrong_size);
	}

	ReplicationRecord record{};
	record.index = wire::detail::load_be32(input, 0);
	detail::copy_message(input, 4, record.message);
	record.process_status = std::to_integer<std::uint8_t>(input[4 + wire::kWireMessageBytes]);
	record.engine_status = std::to_integer<std::uint8_t>(input[5 + wire::kWireMessageBytes]);
	return record;
}

inline void encode_heartbeat(const Heartbeat &heartbeat, const std::span<std::byte, kHeartbeatBytes> output) noexcept
{
	wire::detail::store_be32(output, 0, kHeartbeatMagic);
	wire::detail::store_be64(output, 4, heartbeat.number);
	wire::detail::store_be32(output, 12, heartbeat.applied_count);
}

[[nodiscard]] inline std::expected<Heartbeat, CodecError>
	decode_heartbeat(const std::span<const std::byte> input) noexcept
{
	if (input.size() != kHeartbeatBytes) {
		return std::unexpected(CodecError::wrong_size);
	}

	if (wire::detail::load_be32(input, 0) != kHeartbeatMagic) {
		return std::unexpected(CodecError::invalid_magic);
	}

	return Heartbeat{wire::detail::load_be64(input, 4), wire::detail::load_be32(input, 12)};
}

} // namespace depthwire::ha
