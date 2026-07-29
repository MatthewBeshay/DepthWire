#pragma once

#include <cstdint>

#include "depthwire/engine/trading_engine.hpp"
#include "depthwire/pipeline/packet_processor.hpp"

namespace depthwire::replay {

// Exact replay accounting (docs/architecture/trace-format.md and the replay
// methodology). The first group is hashed into the
// run digest: every field is a pure function of the accepted input sequence
// and the configuration. The second group is reported but never hashed: those
// values are timing-derived and legitimately vary across runs that accepted
// identical input.
struct ReplayCounters {
	// Hashed into the run digest, in this order.
	std::uint64_t offered{};
	std::uint64_t accepted{};
	std::uint64_t dropped{};
	std::uint64_t processed{};
	std::uint64_t ordered{};
	std::uint64_t status_no_order{};
	std::uint64_t status_order_encoded{};
	std::uint64_t status_decode_rejected{};
	std::uint64_t status_adapter_rejected{};
	std::uint64_t status_other{};
	std::uint64_t engine_duplicate{};
	std::uint64_t engine_gap{};
	std::uint64_t engine_stale{};
	std::uint64_t engine_invalid{};
	std::uint64_t engine_other{};
	std::uint64_t backlog_final{};

	// Reported, never hashed.
	std::uint64_t late{};
	std::uint64_t queued_max{};
	std::uint64_t orders_tx_dropped{};
	std::uint64_t recovery_duration_ns{};
};

inline constexpr std::uint32_t kHashedCounterCount = 16;

// Classifies one packet-processor outcome. `ordered` counts orders encoded by
// the engine (a deterministic outcome); transport TX acceptance is accounted
// separately by the paced harness via orders_tx_dropped.
constexpr void apply_process_result(ReplayCounters &counters, const pipeline::PacketProcessResult &result) noexcept
{
	++counters.processed;

	switch (result.status) {
	case pipeline::PacketProcessStatus::no_order:
		++counters.status_no_order;
		break;
	case pipeline::PacketProcessStatus::order_encoded:
		++counters.status_order_encoded;
		++counters.ordered;
		break;
	case pipeline::PacketProcessStatus::decode_rejected:
		++counters.status_decode_rejected;
		break;
	case pipeline::PacketProcessStatus::input_adapter_rejected:
	case pipeline::PacketProcessStatus::output_adapter_rejected:
		++counters.status_adapter_rejected;
		break;
	case pipeline::PacketProcessStatus::engine_rejected:
		switch (result.engine_result.status) {
		case EngineStatus::duplicate_update:
			++counters.engine_duplicate;
			break;
		case EngineStatus::sequence_gap:
			++counters.engine_gap;
			break;
		case EngineStatus::stale_update:
			++counters.engine_stale;
			break;
		case EngineStatus::invalid_update:
			++counters.engine_invalid;
			break;
		default:
			++counters.engine_other;
			break;
		}

		break;
	default:
		++counters.status_other;
		break;
	}
}

// Conservation invariants every completed run must satisfy; a violation is a
// harness defect, not valid evidence.
[[nodiscard]] constexpr bool counters_conserved(const ReplayCounters &counters) noexcept
{
	const std::uint64_t classified = counters.status_no_order + counters.status_order_encoded +
					 counters.status_decode_rejected + counters.status_adapter_rejected +
					 counters.status_other + counters.engine_duplicate + counters.engine_gap +
					 counters.engine_stale + counters.engine_invalid + counters.engine_other;

	return counters.processed == classified && counters.ordered == counters.status_order_encoded &&
	       counters.offered == counters.accepted + counters.dropped && counters.accepted == counters.processed;
}

} // namespace depthwire::replay
