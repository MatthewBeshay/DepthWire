#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>

#include "depthwire/digest/sha256.hpp"
#include "depthwire/engine/trading_engine.hpp"
#include "depthwire/pipeline/packet_processor.hpp"
#include "depthwire/replay/event_journal.hpp"
#include "depthwire/replay/replay_counters.hpp"
#include "depthwire/replay/run_digest.hpp"
#include "depthwire/trace/trace_codec.hpp"
#include "depthwire/wire/ethernet_frame.hpp"
#include "depthwire/wire/wire_constants.hpp"
#include "depthwire/wire/wire_header.hpp"

namespace depthwire::replay {

// Replay transport addressing is configuration, not trace content: the same
// addresses serve the transport-neutral conformance path and the DPDK edge.
inline constexpr wire::MacAddress kReplayEngineMac{
	std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x20},
};

inline constexpr wire::MacAddress kReplayGeneratorMac{
	std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x10},
};

[[nodiscard]] constexpr wire::EthernetEnvelope replay_market_envelope() noexcept
{
	return {kReplayEngineMac, kReplayGeneratorMac};
}

// The documented replay engine configuration (part of the digest claim's
// "same configuration"): the burst benchmark's proven strategy/risk settings,
// with the instrument taken from the validated trace header.
[[nodiscard]] constexpr pipeline::PacketProcessorConfig
	default_replay_processor_config(const std::uint32_t instrument_id,
					const SequencePolicy sequence_policy) noexcept
{
	pipeline::PacketProcessorConfig config{};
	config.engine.instrument_id = instrument_id;
	config.engine.strategy.imbalance_threshold_bps = 6000;
	config.engine.strategy.order_quantity = 10;
	config.engine.risk.max_order_quantity = 100;
	config.engine.risk.max_absolute_position = std::numeric_limits<std::int64_t>::max() / 4;
	config.engine.risk.max_notional_ticks = std::numeric_limits<std::uint64_t>::max();
	config.engine.risk.max_outstanding_orders = 128;
	config.engine.sequence_policy = sequence_policy;
	config.local_source = kReplayEngineMac;
	config.first_output_sequence = 1;
	return config;
}

enum class ConformanceRunError : std::uint8_t {
	journal_capacity_exceeded = 1,
	acknowledge_failed,
};

struct ConformanceResult {
	ReplayCounters counters{};
	digest::Sha256Digest digest{};
};

// Transport-neutral conformance replay: every validated record is framed with
// the configured envelope, processed to completion in trace order, and its
// outcome journaled; each encoded order is acknowledged immediately. No
// pacing and no transport are involved, so repeated runs over the same trace
// and configuration must produce the identical digest
// (docs/architecture/trace-format.md). The paced
// DPDK harness shares this processing and digest path; only its transport and
// timing accounting differ.
template<std::size_t OutboxCapacity = 64>
[[nodiscard]] inline std::expected<ConformanceResult, ConformanceRunError>
	run_conformance(const trace::TraceView &trace_view, const pipeline::PacketProcessorConfig &config,
			const wire::EthernetEnvelope &envelope, EventJournal &journal) noexcept
{
	pipeline::PacketProcessor<OutboxCapacity> processor{config};
	ConformanceResult result{};

	wire::EthernetFrame input_frame{};
	wire::encode_ethernet_header(envelope, input_frame);

	for (std::uint32_t index = 0; index < trace_view.record_count(); ++index) {
		const std::span<const std::byte> message_bytes = trace_view.message_bytes(index);

		// The trace stores the encoded wire message verbatim; the frame is
		// envelope + those bytes, so replay is byte-preserving by
		// construction.
		for (std::size_t byte_index = 0; byte_index < wire::kWireMessageBytes; ++byte_index) {
			input_frame[wire::kWirePayloadOffset + byte_index] = message_bytes[byte_index];
		}

		JournalEntry entry{};
		const auto header = wire::decode_header(message_bytes, wire::MessageType::market_data);
		entry.input_sequence = header ? header->sequence_number : 0;

		wire::EthernetFrame output_frame{};
		const pipeline::PacketProcessResult process_result = processor.process(input_frame, output_frame);

		entry.process_status = process_result.status;
		entry.engine_status = process_result.engine_result.status;
		entry.has_frame = process_result.output_ready;

		if (process_result.output_ready) {
			entry.frame = output_frame;
		}

		if (!journal.record(entry)) {
			return std::unexpected(ConformanceRunError::journal_capacity_exceeded);
		}

		apply_process_result(result.counters, process_result);

		if (process_result.output_ready && !processor.try_acknowledge_one_order()) {
			return std::unexpected(ConformanceRunError::acknowledge_failed);
		}
	}

	result.counters.offered = trace_view.record_count();
	result.counters.accepted = trace_view.record_count();
	result.digest = compute_run_digest(journal, result.counters);
	return result;
}

} // namespace depthwire::replay
