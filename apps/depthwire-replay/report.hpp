#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <utility>

#include "depthwire/digest/sha256.hpp"
#include "depthwire/replay/replay_counters.hpp"
#include "depthwire/trace/trace_format.hpp"

#ifndef DEPTHWIRE_PROJECT_VERSION
#	define DEPTHWIRE_PROJECT_VERSION "unknown"
#endif

#ifndef DEPTHWIRE_SOURCE_REVISION
#	define DEPTHWIRE_SOURCE_REVISION "unknown"
#endif

// Report formatting shared by the conformance and paced run paths. The
// machine-greppable lines (trace-sha256:, digest-sha256:, counter <name>:)
// are stable interfaces consumed by CI scripts and the evidence tooling.
namespace depthwire::replay_app {

[[nodiscard]] inline const char *trace_error_name(const trace::TraceError error) noexcept
{
	using trace::TraceError;

	switch (error) {
	case TraceError::too_short:
		return "too_short";
	case TraceError::trailing_bytes:
		return "trailing_bytes";
	case TraceError::invalid_magic:
		return "invalid_magic";
	case TraceError::unsupported_version:
		return "unsupported_version";
	case TraceError::unsupported_wire_version:
		return "unsupported_wire_version";
	case TraceError::invalid_header_length:
		return "invalid_header_length";
	case TraceError::invalid_record_length:
		return "invalid_record_length";
	case TraceError::invalid_record_count:
		return "invalid_record_count";
	case TraceError::invalid_flags:
		return "invalid_flags";
	case TraceError::delta_bound_invalid:
		return "delta_bound_invalid";
	case TraceError::delta_bound_exceeded:
		return "delta_bound_exceeded";
	case TraceError::duration_mismatch:
		return "duration_mismatch";
	case TraceError::record_digest_mismatch:
		return "record_digest_mismatch";
	case TraceError::record_decode_rejected:
		return "record_decode_rejected";
	case TraceError::record_encode_rejected:
		return "record_encode_rejected";
	case TraceError::instrument_mismatch:
		return "instrument_mismatch";
	case TraceError::quantity_bound_exceeded:
		return "quantity_bound_exceeded";
	}

	return "unknown";
}

inline void print_trace_validation_error(const trace::TraceValidationError &error)
{
	std::fprintf(stderr, "DepthWire failure: trace validation failed: %s (record %u", trace_error_name(error.error),
		     error.record_index);

	if (error.wire_error.has_value()) {
		std::fprintf(stderr, ", wire error %u", static_cast<unsigned>(std::to_underlying(*error.wire_error)));
	}

	std::fprintf(stderr, ")\n");
}

inline void print_hex_line(const char *const label, const digest::Sha256Digest &digest_value)
{
	const auto hex = digest::to_hex(digest_value);
	std::printf("%s: %.*s\n", label, static_cast<int>(hex.size()), hex.data());
}

inline void print_trace_summary(const char *const path, const std::span<const std::byte> file,
				const trace::TraceSummary &summary)
{
	std::printf("Trace file:                %s\n", path);
	std::printf("Trace records:             %u\n", summary.header.record_count);
	std::printf("Trace duration:            %llu ns\n",
		    static_cast<unsigned long long>(summary.header.total_duration_ns));
	std::printf("Instrument:                %u\n", summary.header.bounds.instrument_id);
	std::printf("Declared max delta:        %llu ns\n",
		    static_cast<unsigned long long>(summary.header.bounds.max_arrival_delta_ns));
	std::printf("Declared max quantity:     %u\n", summary.header.bounds.max_quantity);
	std::printf("Observed delta range:      %llu..%llu ns\n",
		    static_cast<unsigned long long>(summary.min_arrival_delta_ns),
		    static_cast<unsigned long long>(summary.max_arrival_delta_ns));
	print_hex_line("trace-records-sha256", summary.header.records_sha256);
	print_hex_line("trace-sha256", digest::sha256_of(file));
}

inline void print_identity(const char *const mode)
{
	std::printf("DepthWire version:         %s\n", DEPTHWIRE_PROJECT_VERSION);
	std::printf("Source revision:           %s\n", DEPTHWIRE_SOURCE_REVISION);
	std::printf("Mode:                      %s\n", mode);

	if (std::strcmp(DEPTHWIRE_SOURCE_REVISION, "unknown") == 0) {
		std::printf("WARNING: source revision unknown; this run is not publishable "
			    "evidence; see docs/benchmarks/methodology.md.\n");
	}
}

inline void print_counter(const char *const name, const std::uint64_t value)
{
	std::printf("counter %-22s %llu\n", name, static_cast<unsigned long long>(value));
}

inline void print_counters(const replay::ReplayCounters &counters)
{
	std::printf("Counters (hashed into digest):\n");
	print_counter("offered:", counters.offered);
	print_counter("accepted:", counters.accepted);
	print_counter("dropped:", counters.dropped);
	print_counter("processed:", counters.processed);
	print_counter("ordered:", counters.ordered);
	print_counter("no-order:", counters.status_no_order);
	print_counter("order-encoded:", counters.status_order_encoded);
	print_counter("decode-rejected:", counters.status_decode_rejected);
	print_counter("adapter-rejected:", counters.status_adapter_rejected);
	print_counter("status-other:", counters.status_other);
	print_counter("engine-duplicate:", counters.engine_duplicate);
	print_counter("engine-gap:", counters.engine_gap);
	print_counter("engine-stale:", counters.engine_stale);
	print_counter("engine-invalid:", counters.engine_invalid);
	print_counter("engine-other:", counters.engine_other);
	print_counter("backlog-final:", counters.backlog_final);
	std::printf("Counters (reported, not hashed):\n");
	print_counter("late:", counters.late);
	print_counter("queued-max:", counters.queued_max);
	print_counter("orders-tx-dropped:", counters.orders_tx_dropped);
	print_counter("recovery-duration-ns:", counters.recovery_duration_ns);
}

} // namespace depthwire::replay_app
