#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "depthwire/contracts/contracts.hpp"
#include "depthwire/digest/sha256.hpp"
#include "depthwire/trace/trace_format.hpp"
#include "depthwire/wire/market_data_packet.hpp"
#include "depthwire/wire/wire_constants.hpp"

namespace depthwire::trace {

namespace detail {

// Local big-endian helpers so the trace codec depends only on the public wire
// surface, not on wire's codec internals.
template<typename Unsigned>
constexpr void store_be(const std::span<std::byte> output, const std::size_t offset, const Unsigned value) noexcept
{
	DW_CONTRACT_ASSERT(offset + sizeof(Unsigned) <= output.size());

	for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
		output[offset + index] = static_cast<std::byte>(value >> (8 * (sizeof(Unsigned) - 1 - index)));
	}
}

template<typename Unsigned>
[[nodiscard]] constexpr Unsigned load_be(const std::span<const std::byte> input, const std::size_t offset) noexcept
{
	DW_CONTRACT_ASSERT(offset + sizeof(Unsigned) <= input.size());

	Unsigned value{};

	for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
		value = static_cast<Unsigned>(value << 8) |
			static_cast<Unsigned>(std::to_integer<std::uint8_t>(input[offset + index]));
	}

	return value;
}

} // namespace detail

// The normative header layout lives in docs/architecture/trace-format.md;
// the golden-vector test pins these bytes exactly.
constexpr void encode_trace_header(const TraceHeader &header, const std::span<std::byte> output) noexcept
{
	DW_CONTRACT_ASSERT(output.size() >= kTraceHeaderBytes);

	detail::store_be<std::uint32_t>(output, 0, kTraceMagic);
	output[4] = static_cast<std::byte>(kTraceVersion);
	output[5] = static_cast<std::byte>(wire::kVersion);
	detail::store_be<std::uint16_t>(output, 6, static_cast<std::uint16_t>(kTraceHeaderBytes));
	detail::store_be<std::uint32_t>(output, 8, static_cast<std::uint32_t>(kTraceRecordBytes));
	detail::store_be<std::uint32_t>(output, 12, header.record_count);
	detail::store_be<std::uint64_t>(output, 16, header.total_duration_ns);
	detail::store_be<std::uint64_t>(output, 24, header.bounds.max_arrival_delta_ns);
	detail::store_be<std::uint32_t>(output, 32, header.bounds.instrument_id);
	detail::store_be<std::uint32_t>(output, 36, header.bounds.max_quantity);
	detail::store_be<std::uint32_t>(output, 40, 0);

	for (std::size_t index = 0; index < header.records_sha256.size(); ++index) {
		output[44 + index] = header.records_sha256[index];
	}
}

// Container-level header decode. Whole-file validation (record digest, per
// record bounds, duration) is validate_trace()'s job.
[[nodiscard]] constexpr std::expected<TraceHeader, TraceError>
	decode_trace_header(const std::span<const std::byte> input) noexcept
{
	if (input.size() < kTraceHeaderBytes) {
		return std::unexpected(TraceError::too_short);
	}

	if (detail::load_be<std::uint32_t>(input, 0) != kTraceMagic) {
		return std::unexpected(TraceError::invalid_magic);
	}

	if (std::to_integer<std::uint8_t>(input[4]) != kTraceVersion) {
		return std::unexpected(TraceError::unsupported_version);
	}

	if (std::to_integer<std::uint8_t>(input[5]) != wire::kVersion) {
		return std::unexpected(TraceError::unsupported_wire_version);
	}

	if (detail::load_be<std::uint16_t>(input, 6) != kTraceHeaderBytes) {
		return std::unexpected(TraceError::invalid_header_length);
	}

	if (detail::load_be<std::uint32_t>(input, 8) != kTraceRecordBytes) {
		return std::unexpected(TraceError::invalid_record_length);
	}

	TraceHeader header{};
	header.record_count = detail::load_be<std::uint32_t>(input, 12);

	if (header.record_count == 0 || header.record_count > kMaxTraceRecords) {
		return std::unexpected(TraceError::invalid_record_count);
	}

	header.total_duration_ns = detail::load_be<std::uint64_t>(input, 16);
	header.bounds.max_arrival_delta_ns = detail::load_be<std::uint64_t>(input, 24);

	if (header.bounds.max_arrival_delta_ns > kMaxArrivalDeltaNs) {
		return std::unexpected(TraceError::delta_bound_invalid);
	}

	header.bounds.instrument_id = detail::load_be<std::uint32_t>(input, 32);
	header.bounds.max_quantity = detail::load_be<std::uint32_t>(input, 36);

	if (detail::load_be<std::uint32_t>(input, 40) != 0) {
		return std::unexpected(TraceError::invalid_flags);
	}

	for (std::size_t index = 0; index < header.records_sha256.size(); ++index) {
		header.records_sha256[index] = input[44 + index];
	}

	return header;
}

// Encodes records into a complete trace file, computing count, duration, and
// the record-region hash, and validating every record against the declared
// bounds. The output span must be exactly trace_file_bytes(records.size()).
// This is fixture/converter tooling, not the replay hot path.
[[nodiscard]] inline std::expected<TraceHeader, TraceValidationError>
	encode_trace(const std::span<const TraceRecord> records, const TraceBounds &bounds,
		     const std::span<std::byte> output) noexcept
{
	if (records.empty() || records.size() > kMaxTraceRecords) {
		return std::unexpected(TraceValidationError{TraceError::invalid_record_count});
	}

	if (bounds.max_arrival_delta_ns > kMaxArrivalDeltaNs) {
		return std::unexpected(TraceValidationError{TraceError::delta_bound_invalid});
	}

	if (output.size() != trace_file_bytes(static_cast<std::uint32_t>(records.size()))) {
		return std::unexpected(TraceValidationError{TraceError::too_short});
	}

	TraceHeader header{};
	header.record_count = static_cast<std::uint32_t>(records.size());
	header.bounds = bounds;

	for (std::size_t index = 0; index < records.size(); ++index) {
		const TraceRecord &record = records[index];
		const auto record_index = static_cast<std::uint32_t>(index);

		if (record.arrival_delta_ns > bounds.max_arrival_delta_ns) {
			return std::unexpected(TraceValidationError{TraceError::delta_bound_exceeded, record_index});
		}

		if (record.message.instrument_id != bounds.instrument_id) {
			return std::unexpected(TraceValidationError{TraceError::instrument_mismatch, record_index});
		}

		if (record.message.quantity > bounds.max_quantity) {
			return std::unexpected(TraceValidationError{TraceError::quantity_bound_exceeded, record_index});
		}

		const std::span<std::byte> record_bytes =
			output.subspan(kTraceHeaderBytes + index * kTraceRecordBytes, kTraceRecordBytes);

		detail::store_be<std::uint64_t>(record_bytes, 0, record.arrival_delta_ns);

		wire::WireMessage message_bytes{};
		const auto encoded = wire::encode_market_data(record.message, message_bytes);

		if (!encoded) {
			return std::unexpected(TraceValidationError{TraceError::record_encode_rejected, record_index,
								    encoded.error()});
		}

		for (std::size_t byte_index = 0; byte_index < message_bytes.size(); ++byte_index) {
			record_bytes[kTraceRecordMessageOffset + byte_index] = message_bytes[byte_index];
		}

		header.total_duration_ns += record.arrival_delta_ns;
	}

	header.records_sha256 = digest::sha256_of(output.subspan(kTraceHeaderBytes));
	encode_trace_header(header, output);
	return header;
}

// Cheap, allocation-free record accessor over an already-validated trace file.
// Precondition: bytes passed validate_trace(); nothing is re-checked here.
class TraceView {
public:
	explicit constexpr TraceView(const std::span<const std::byte> validated_file) noexcept
		: file_(validated_file), record_count_(detail::load_be<std::uint32_t>(validated_file, 12))
	{
		DW_CONTRACT_ASSERT(validated_file.size() ==
				   trace_file_bytes(detail::load_be<std::uint32_t>(validated_file, 12)));
	}

	[[nodiscard]] constexpr std::uint32_t record_count() const noexcept { return record_count_; }

	[[nodiscard]] constexpr std::uint64_t arrival_delta_ns(const std::uint32_t index) const noexcept
	{
		DW_CONTRACT_ASSERT(index < record_count_);
		return detail::load_be<std::uint64_t>(record_bytes(index), 0);
	}

	[[nodiscard]] constexpr std::span<const std::byte> message_bytes(const std::uint32_t index) const noexcept
	{
		DW_CONTRACT_ASSERT(index < record_count_);
		return record_bytes(index).subspan(kTraceRecordMessageOffset, wire::kWireMessageBytes);
	}

private:
	[[nodiscard]] constexpr std::span<const std::byte> record_bytes(const std::uint32_t index) const noexcept
	{
		return file_.subspan(kTraceHeaderBytes + static_cast<std::size_t>(index) * kTraceRecordBytes,
				     kTraceRecordBytes);
	}

	std::span<const std::byte> file_{};
	std::uint32_t record_count_{};
};

} // namespace depthwire::trace
