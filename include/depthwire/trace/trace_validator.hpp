#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

#include "depthwire/digest/sha256.hpp"
#include "depthwire/trace/trace_codec.hpp"
#include "depthwire/trace/trace_format.hpp"
#include "depthwire/wire/market_data_packet.hpp"

namespace depthwire::trace {

// Whole-file boundary validation of an untrusted trace: runs before any
// replay and is never skipped or weakened for speed.
//
// Sequence continuity is deliberately not container validity: traces
// legitimately carry duplicates, gaps, stale updates, and 32-bit wrap to
// exercise the engine's sequence-rejection semantics (trading_engine.hpp).
[[nodiscard]] inline std::expected<TraceSummary, TraceValidationError>
	validate_trace(const std::span<const std::byte> file) noexcept
{
	const auto header = decode_trace_header(file);

	if (!header) {
		return std::unexpected(TraceValidationError{header.error()});
	}

	const std::uint64_t expected_bytes = trace_file_bytes(header->record_count);

	if (file.size() < expected_bytes) {
		return std::unexpected(TraceValidationError{TraceError::too_short});
	}

	if (file.size() > expected_bytes) {
		return std::unexpected(TraceValidationError{TraceError::trailing_bytes});
	}

	const std::span<const std::byte> record_region = file.subspan(kTraceHeaderBytes);

	if (digest::sha256_of(record_region) != header->records_sha256) {
		return std::unexpected(TraceValidationError{TraceError::record_digest_mismatch});
	}

	TraceSummary summary{};
	summary.header = *header;
	summary.min_arrival_delta_ns = kMaxArrivalDeltaNs + 1;

	std::uint64_t duration_ns = 0;

	for (std::uint32_t index = 0; index < header->record_count; ++index) {
		const std::span<const std::byte> record_bytes =
			record_region.subspan(static_cast<std::size_t>(index) * kTraceRecordBytes, kTraceRecordBytes);

		const std::uint64_t delta_ns = detail::load_be<std::uint64_t>(record_bytes, 0);

		if (delta_ns > header->bounds.max_arrival_delta_ns) {
			return std::unexpected(TraceValidationError{TraceError::delta_bound_exceeded, index});
		}

		const auto message = wire::decode_market_data(record_bytes.subspan(kTraceRecordMessageOffset));

		if (!message) {
			return std::unexpected(
				TraceValidationError{TraceError::record_decode_rejected, index, message.error()});
		}

		if (message->instrument_id != header->bounds.instrument_id) {
			return std::unexpected(TraceValidationError{TraceError::instrument_mismatch, index});
		}

		if (message->quantity > header->bounds.max_quantity) {
			return std::unexpected(TraceValidationError{TraceError::quantity_bound_exceeded, index});
		}

		duration_ns += delta_ns;

		if (delta_ns < summary.min_arrival_delta_ns) {
			summary.min_arrival_delta_ns = delta_ns;
		}

		if (delta_ns > summary.max_arrival_delta_ns) {
			summary.max_arrival_delta_ns = delta_ns;
		}

		if (message->quantity > summary.max_quantity) {
			summary.max_quantity = message->quantity;
		}
	}

	if (duration_ns != header->total_duration_ns) {
		return std::unexpected(TraceValidationError{TraceError::duration_mismatch});
	}

	return summary;
}

} // namespace depthwire::trace
