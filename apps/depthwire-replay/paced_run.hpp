#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "depthwire/engine/trading_engine.hpp"
#include "depthwire/replay/pacer.hpp"
#include "depthwire/trace/trace_format.hpp"

namespace depthwire::replay_app {

// Options for the DPDK-paced replay run. eal_arguments carries pointers into
// the process argv (argv[0] plus everything after "--"), handed verbatim to
// rte_eal_init.
struct PacedOptions {
	const char *trace_path{};
	SequencePolicy sequence_policy{SequencePolicy::accept_any};
	std::vector<replay::PaceSegment> segments{};
	std::uint64_t lateness_bound_ns{50'000};
	std::uint16_t burst{32};
	std::vector<char *> eal_arguments{};
};

// Compiled only in DPDK-enabled builds (apps/CMakeLists.txt). The trace file
// must already have passed validate_trace.
int run_paced(const PacedOptions &options, std::span<const std::byte> trace_file, const trace::TraceSummary &summary);

} // namespace depthwire::replay_app
