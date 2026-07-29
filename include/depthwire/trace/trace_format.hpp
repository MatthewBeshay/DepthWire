#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "depthwire/digest/sha256.hpp"
#include "depthwire/wire/market_data_packet.hpp"
#include "depthwire/wire/wire_constants.hpp"
#include "depthwire/wire/wire_header.hpp"

namespace depthwire::trace {

// Versioned replay-trace container (see docs/architecture/trace-format.md).
// A trace is a bounded, self-describing file: a fixed 76-byte header followed
// by record_count fixed 56-byte records. All integer fields are big-endian,
// matching the wire packages. Version 1 defines no flag bits; a new meaning
// requires a new trace version, never a reinterpretation of accepted bytes.

inline constexpr std::uint32_t kTraceMagic = 0x44575452U; // "DWTR"
inline constexpr std::uint8_t kTraceVersion1 = 1;
inline constexpr std::uint8_t kTraceVersion = kTraceVersion1;
inline constexpr std::size_t kTraceHeaderBytes = 76;
inline constexpr std::size_t kTraceRecordBytes = 56;
inline constexpr std::size_t kTraceRecordMessageOffset = 8;
inline constexpr std::uint32_t kMaxTraceRecords = 16'777'216;
inline constexpr std::uint64_t kMaxArrivalDeltaNs = 1'000'000'000;

[[nodiscard]] constexpr std::uint64_t trace_file_bytes(const std::uint32_t record_count) noexcept
{
	return kTraceHeaderBytes +
	       static_cast<std::uint64_t>(kTraceRecordBytes) * static_cast<std::uint64_t>(record_count);
}

// Declared bounds a trace commits to; every record is validated against them.
struct TraceBounds {
	std::uint64_t max_arrival_delta_ns{};
	std::uint32_t instrument_id{};
	std::uint32_t max_quantity{};
};

struct TraceHeader {
	std::uint32_t record_count{};
	std::uint64_t total_duration_ns{};
	TraceBounds bounds{};
	digest::Sha256Digest records_sha256{};
};

struct TraceRecord {
	std::uint64_t arrival_delta_ns{};
	wire::MarketDataMessage message{};
};

enum class TraceError : std::uint8_t {
	too_short = 1,
	trailing_bytes,
	invalid_magic,
	unsupported_version,
	unsupported_wire_version,
	invalid_header_length,
	invalid_record_length,
	invalid_record_count,
	invalid_flags,
	delta_bound_invalid,
	delta_bound_exceeded,
	duration_mismatch,
	record_digest_mismatch,
	record_decode_rejected,
	record_encode_rejected,
	instrument_mismatch,
	quantity_bound_exceeded,
};

// record_index identifies the failing record for per-record errors and is zero
// for container-level errors. wire_error is populated only for
// record_decode_rejected / record_encode_rejected.
struct TraceValidationError {
	TraceError error{TraceError::too_short};
	std::uint32_t record_index{};
	std::optional<wire::WireError> wire_error{};
};

struct TraceSummary {
	TraceHeader header{};
	std::uint64_t min_arrival_delta_ns{};
	std::uint64_t max_arrival_delta_ns{};
	std::uint32_t max_quantity{};
};

} // namespace depthwire::trace
