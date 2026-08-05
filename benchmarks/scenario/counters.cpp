// Engine scenario counter benchmark.
//
// Attributes hardware and software counter cost to each distinct engine
// outcome, one scenario per process. Every scenario is a deterministic
// steady-state loop over a single prepared stimulus, so the measured region is
// exactly the repeated hot-path call and setup cost is negligible. Counters are
// descriptive: they are reported, never subtracted from one another, and never
// hashed into any digest.
//
// Seven scenarios measure the full packet path
// (pipeline::PacketProcessor::process, i.e. decode -> engine -> encode, plus an
// acknowledgement for the emitting cases). The outbox-full scenario measures
// TradingEngine::on_market_update directly, because the packet processor drains
// its outbox on every call by construction, so backpressure is only reachable
// at the engine boundary. Each scenario prints its own measured-boundary line.

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>

#include "depthwire/engine/trading_engine.hpp"
#include "depthwire/pipeline/packet_processor.hpp"
#include "depthwire/wire/ethernet_frame.hpp"
#include "depthwire/wire/market_data_packet.hpp"
#include "support/perf_counters.hpp"

#ifndef DEPTHWIRE_PROJECT_VERSION
#	define DEPTHWIRE_PROJECT_VERSION "unknown"
#endif

#ifndef DEPTHWIRE_SOURCE_REVISION
#	define DEPTHWIRE_SOURCE_REVISION "unknown"
#endif

namespace {

using depthwire::bench::PerfCounterGroup;
using depthwire::bench::PerfCounts;

inline constexpr std::size_t kDefaultIterations = 50'000'000;
inline constexpr std::size_t kDefaultWarmup = 1'000'000;
inline constexpr std::uint16_t kInstrumentId = 77;

// Compiler barrier: forces the pointed-to object to be materialised in memory
// and treats memory as clobbered, so the optimiser cannot elide a computed
// result or hoist a loop-invariant call out of the measured region.
inline void escape(const void *pointer) noexcept
{
#if defined(__GNUC__)
	asm volatile("" : : "g"(pointer) : "memory");
#else
	static_cast<void>(pointer);
#endif
}

// Generous risk limits so the emitting scenarios stay in steady state for any
// realistic iteration count: at ten shares per order the accumulated position
// and notional never approach these bounds.
depthwire::RiskLimits generous_risk_limits() noexcept
{
	depthwire::RiskLimits limits{};
	limits.max_order_quantity = 100;
	limits.max_absolute_position = 1'000'000'000'000'000;
	limits.max_notional_ticks = UINT64_MAX;
	limits.max_outstanding_orders = 64;
	return limits;
}

depthwire::pipeline::PacketProcessorConfig make_processor_config(const depthwire::SequencePolicy policy,
								 const depthwire::RiskLimits risk) noexcept
{
	depthwire::pipeline::PacketProcessorConfig config{};
	config.engine.instrument_id = kInstrumentId;
	config.engine.strategy.imbalance_threshold_bps = 6000;
	config.engine.strategy.order_quantity = 10;
	config.engine.risk = risk;
	config.engine.sequence_policy = policy;
	return config;
}

depthwire::wire::EthernetEnvelope market_envelope(const depthwire::wire::MacAddress &local_source) noexcept
{
	depthwire::wire::EthernetEnvelope envelope{};
	envelope.destination = local_source;
	envelope.source = {
		std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x10},
	};
	return envelope;
}

depthwire::wire::MarketDataMessage market_message(const std::uint32_t sequence, const depthwire::wire::Side side,
						  const std::uint16_t level, const std::int64_t price_ticks,
						  const std::uint32_t quantity) noexcept
{
	depthwire::wire::MarketDataMessage message{};
	message.sequence_number = sequence;
	message.wire_flags = 0;
	message.exchange_timestamp_ns = static_cast<std::uint64_t>(sequence) * 1000ULL;
	message.instrument_id = kInstrumentId;
	message.price_ticks = price_ticks;
	message.quantity = quantity;
	message.side = side;
	message.action = depthwire::wire::UpdateAction::modify;
	message.level = level;
	message.source_flags = 0;
	return message;
}

[[nodiscard]] bool encode_frame(const depthwire::wire::MacAddress &local_source,
				const depthwire::wire::MarketDataMessage &message,
				depthwire::wire::EthernetFrame &frame) noexcept
{
	return depthwire::wire::encode_market_data_frame(market_envelope(local_source), message, frame).has_value();
}

// Seeds a five-level-per-side balanced book through the packet processor. Every
// seed produces no order. Returns the next unused sequence number, or reports
// failure through the out parameter.
[[nodiscard]] bool seed_processor(depthwire::pipeline::PacketProcessor<64> &processor,
				  const depthwire::wire::MacAddress &local_source,
				  std::uint32_t &next_sequence) noexcept
{
	depthwire::wire::EthernetFrame frame{};
	depthwire::wire::EthernetFrame output{};

	for (std::uint16_t level = 0; level < 5; ++level) {
		const auto sell =
			market_message(next_sequence++, depthwire::wire::Side::sell, level, 100100 + level, 10);

		if (!encode_frame(local_source, sell, frame) ||
		    processor.process(frame, output).status != depthwire::pipeline::PacketProcessStatus::no_order) {
			return false;
		}
	}

	for (std::uint16_t level = 0; level < 5; ++level) {
		const auto buy = market_message(next_sequence++, depthwire::wire::Side::buy, level, 100000 - level, 10);

		if (!encode_frame(local_source, buy, frame) ||
		    processor.process(frame, output).status != depthwire::pipeline::PacketProcessStatus::no_order) {
			return false;
		}
	}

	return true;
}

struct ScenarioResult {
	bool verified{false};
	std::uint64_t checksum{};
	PerfCounts counts{};
	const char *boundary{""};
	const char *policy{""};
};

// Runs a fixed prepared frame through the processor `iterations` times, after a
// warm-up of `warmup` iterations, with counters wrapping only the measured
// region. Every processed frame must yield `expected`; emitting scenarios
// acknowledge each order so the outstanding-order slot is released.
ScenarioResult run_process_scenario(depthwire::pipeline::PacketProcessor<64> &processor,
				    const depthwire::wire::EthernetFrame &stimulus,
				    const depthwire::pipeline::PacketProcessStatus expected, const bool emits_order,
				    const std::size_t iterations, const std::size_t warmup, const char *boundary,
				    const char *policy, PerfCounterGroup &perf) noexcept
{
	ScenarioResult result{};
	result.boundary = boundary;
	result.policy = policy;
	result.verified = true;

	depthwire::wire::EthernetFrame output{};
	const std::span<const std::byte> input{stimulus};

	for (std::size_t index = 0; index < warmup; ++index) {
		const auto packet = processor.process(input, output);

		if (packet.status != expected) {
			result.verified = false;
			return result;
		}

		if (emits_order && !processor.try_acknowledge_one_order()) {
			result.verified = false;
			return result;
		}
	}

	std::uint64_t checksum = 0;
	perf.reset_and_enable();

	for (std::size_t index = 0; index < iterations; ++index) {
		const auto packet = processor.process(input, output);

		if (packet.status != expected) {
			result.verified = false;
			perf.disable();
			return result;
		}

		checksum += static_cast<std::uint64_t>(std::to_underlying(packet.status));

		if (emits_order) {
			escape(output.data());
			checksum += std::to_integer<std::uint64_t>(output[0]);

			if (!processor.try_acknowledge_one_order()) {
				result.verified = false;
				perf.disable();
				return result;
			}
		}

		escape(&checksum);
	}

	perf.disable();
	result.counts = perf.read();
	result.checksum = checksum;
	return result;
}

// no-signal / buy-signal / sell-signal / risk-reject: accept-any sequence policy
// lets a single prepared frame replay every iteration with no per-iteration
// staging, so the counters cover exactly the processing path.
ScenarioResult run_signal_scenario(const depthwire::wire::Side stimulus_side, const std::uint32_t stimulus_quantity,
				   const depthwire::pipeline::PacketProcessStatus expected, const bool emits_order,
				   const depthwire::RiskLimits risk, const char *const boundary,
				   const std::size_t iterations, const std::size_t warmup,
				   PerfCounterGroup &perf) noexcept
{
	const auto config = make_processor_config(depthwire::SequencePolicy::accept_any, risk);
	depthwire::pipeline::PacketProcessor<64> processor{config};

	std::uint32_t next_sequence = 1;

	if (!seed_processor(processor, config.local_source, next_sequence)) {
		return ScenarioResult{};
	}

	const std::int64_t price = stimulus_side == depthwire::wire::Side::buy ? 100000 : 100100;
	const auto message = market_message(next_sequence, stimulus_side, 0, price, stimulus_quantity);

	depthwire::wire::EthernetFrame stimulus{};

	if (!encode_frame(config.local_source, message, stimulus)) {
		return ScenarioResult{};
	}

	return run_process_scenario(processor, stimulus, expected, emits_order, iterations, warmup, boundary,
				    "accept-any", perf);
}

// duplicate / sequence-gap: contiguous policy is the only one that produces
// these rejections. Seeding leaves the anchor at sequence 10; a fixed frame at
// sequence 10 duplicates, and one at sequence 12 gaps, every iteration.
ScenarioResult run_sequence_scenario(const std::uint32_t stimulus_sequence,
				     const depthwire::pipeline::PacketProcessStatus expected,
				     const std::size_t iterations, const std::size_t warmup,
				     PerfCounterGroup &perf) noexcept
{
	const auto config =
		make_processor_config(depthwire::SequencePolicy::require_contiguous, generous_risk_limits());
	depthwire::pipeline::PacketProcessor<64> processor{config};

	std::uint32_t next_sequence = 1;

	if (!seed_processor(processor, config.local_source, next_sequence)) {
		return ScenarioResult{};
	}

	// The last applied seed committed the anchor at (next_sequence - 1) == 10.
	const auto message = market_message(stimulus_sequence, depthwire::wire::Side::buy, 0, 100000, 1000);

	depthwire::wire::EthernetFrame stimulus{};

	if (!encode_frame(config.local_source, message, stimulus)) {
		return ScenarioResult{};
	}

	return run_process_scenario(processor, stimulus, expected, false, iterations, warmup,
				    "pipeline::PacketProcessor::process (decode -> engine reject)",
				    "require-contiguous", perf);
}

// invalid-frame: a corrupted wire magic fails the decoder before the engine.
ScenarioResult run_invalid_frame_scenario(const std::size_t iterations, const std::size_t warmup,
					  PerfCounterGroup &perf) noexcept
{
	const auto config = make_processor_config(depthwire::SequencePolicy::accept_any, generous_risk_limits());
	depthwire::pipeline::PacketProcessor<64> processor{config};

	const auto message = market_message(1, depthwire::wire::Side::buy, 0, 100000, 10);
	depthwire::wire::EthernetFrame stimulus{};

	if (!encode_frame(config.local_source, message, stimulus)) {
		return ScenarioResult{};
	}

	// Corrupt the first byte of the wire magic in the payload region.
	stimulus[depthwire::wire::kWirePayloadOffset] = std::byte{0xFF};

	return run_process_scenario(processor, stimulus, depthwire::pipeline::PacketProcessStatus::decode_rejected,
				    false, iterations, warmup, "pipeline::PacketProcessor::process (decode reject)",
				    "accept-any", perf);
}

// outbox-full: measured at the engine boundary. The outbox is filled to capacity
// without draining, then every further update is rejected before any mutation.
ScenarioResult run_outbox_full_scenario(const std::size_t iterations, const std::size_t warmup,
					PerfCounterGroup &perf) noexcept
{
	constexpr std::size_t kCapacity = 4;

	depthwire::EngineConfig config{};
	config.instrument_id = kInstrumentId;
	config.strategy.imbalance_threshold_bps = 6000;
	config.strategy.order_quantity = 10;
	config.risk = generous_risk_limits();
	config.sequence_policy = depthwire::SequencePolicy::accept_any;

	depthwire::TradingEngine<kCapacity> engine{config};

	std::uint32_t sequence = 1;

	for (std::uint16_t level = 0; level < 5; ++level) {
		const depthwire::MarketUpdate sell{sequence++,
						   0,
						   kInstrumentId,
						   100100 + level,
						   10,
						   depthwire::Side::sell,
						   depthwire::UpdateAction::modify,
						   level,
						   0};

		if (engine.on_market_update(sell).status != depthwire::EngineStatus::no_order) {
			return ScenarioResult{};
		}
	}

	for (std::uint16_t level = 0; level < 5; ++level) {
		const depthwire::MarketUpdate buy{sequence++,
						  0,
						  kInstrumentId,
						  100000 - level,
						  10,
						  depthwire::Side::buy,
						  depthwire::UpdateAction::modify,
						  level,
						  0};

		if (engine.on_market_update(buy).status != depthwire::EngineStatus::no_order) {
			return ScenarioResult{};
		}
	}

	const depthwire::MarketUpdate stimulus{
		sequence, 0, kInstrumentId, 100000, 1000, depthwire::Side::buy, depthwire::UpdateAction::modify, 0, 0};

	// Fill the outbox to capacity without draining.
	for (std::size_t index = 0; index < kCapacity; ++index) {
		if (engine.on_market_update(stimulus).status != depthwire::EngineStatus::order_emitted) {
			return ScenarioResult{};
		}
	}

	ScenarioResult result{};
	result.boundary = "TradingEngine::on_market_update (outbox saturated)";
	result.policy = "accept-any";
	result.verified = true;

	for (std::size_t index = 0; index < warmup; ++index) {
		if (engine.on_market_update(stimulus).status != depthwire::EngineStatus::outbox_full) {
			result.verified = false;
			return result;
		}
	}

	std::uint64_t checksum = 0;
	depthwire::MarketUpdate probe = stimulus;
	perf.reset_and_enable();

	for (std::size_t index = 0; index < iterations; ++index) {
		// Vary an ignored field (accept-any never reads the sequence) so the
		// optimiser cannot prove the rejecting call is loop-invariant and hoist
		// it out of the measured region.
		probe.sequence_number = static_cast<std::uint32_t>(index);
		const auto status = engine.on_market_update(probe).status;

		if (status != depthwire::EngineStatus::outbox_full) {
			result.verified = false;
			perf.disable();
			return result;
		}

		checksum += static_cast<std::uint64_t>(std::to_underlying(status));
		escape(&checksum);
	}

	perf.disable();
	result.counts = perf.read();
	result.checksum = checksum;
	return result;
}

struct Scenario {
	std::string_view name;
	ScenarioResult (*run)(std::size_t iterations, std::size_t warmup, PerfCounterGroup &perf);
};

constexpr const char *kProcessNoOrderBoundary = "pipeline::PacketProcessor::process (decode -> engine, no order)";
constexpr const char *kProcessEncodeBoundary = "pipeline::PacketProcessor::process (decode -> engine -> encode) + ack";
constexpr const char *kProcessRejectBoundary = "pipeline::PacketProcessor::process (decode -> engine reject)";

ScenarioResult run_no_signal(std::size_t iterations, std::size_t warmup, PerfCounterGroup &perf) noexcept
{
	return run_signal_scenario(depthwire::wire::Side::buy, 10, depthwire::pipeline::PacketProcessStatus::no_order,
				   false, generous_risk_limits(), kProcessNoOrderBoundary, iterations, warmup, perf);
}

ScenarioResult run_buy_signal(std::size_t iterations, std::size_t warmup, PerfCounterGroup &perf) noexcept
{
	return run_signal_scenario(depthwire::wire::Side::buy, 1000,
				   depthwire::pipeline::PacketProcessStatus::order_encoded, true,
				   generous_risk_limits(), kProcessEncodeBoundary, iterations, warmup, perf);
}

ScenarioResult run_sell_signal(std::size_t iterations, std::size_t warmup, PerfCounterGroup &perf) noexcept
{
	return run_signal_scenario(depthwire::wire::Side::sell, 1000,
				   depthwire::pipeline::PacketProcessStatus::order_encoded, true,
				   generous_risk_limits(), kProcessEncodeBoundary, iterations, warmup, perf);
}

ScenarioResult run_risk_reject(std::size_t iterations, std::size_t warmup, PerfCounterGroup &perf) noexcept
{
	// A tight absolute-position limit rejects the ten-share order before commit,
	// so the position never advances and every iteration is identical.
	depthwire::RiskLimits risk = generous_risk_limits();
	risk.max_absolute_position = 5;

	return run_signal_scenario(depthwire::wire::Side::buy, 1000,
				   depthwire::pipeline::PacketProcessStatus::engine_rejected, false, risk,
				   kProcessRejectBoundary, iterations, warmup, perf);
}

ScenarioResult run_duplicate(std::size_t iterations, std::size_t warmup, PerfCounterGroup &perf) noexcept
{
	return run_sequence_scenario(10, depthwire::pipeline::PacketProcessStatus::engine_rejected, iterations, warmup,
				     perf);
}

ScenarioResult run_sequence_gap(std::size_t iterations, std::size_t warmup, PerfCounterGroup &perf) noexcept
{
	return run_sequence_scenario(12, depthwire::pipeline::PacketProcessStatus::engine_rejected, iterations, warmup,
				     perf);
}

ScenarioResult run_invalid_frame(std::size_t iterations, std::size_t warmup, PerfCounterGroup &perf) noexcept
{
	return run_invalid_frame_scenario(iterations, warmup, perf);
}

ScenarioResult run_outbox_full(std::size_t iterations, std::size_t warmup, PerfCounterGroup &perf) noexcept
{
	return run_outbox_full_scenario(iterations, warmup, perf);
}

constexpr Scenario kScenarios[] = {
	{"no-signal", run_no_signal},         {"buy-signal", run_buy_signal},   {"sell-signal", run_sell_signal},
	{"invalid-frame", run_invalid_frame}, {"duplicate", run_duplicate},     {"sequence-gap", run_sequence_gap},
	{"risk-reject", run_risk_reject},     {"outbox-full", run_outbox_full},
};

[[nodiscard]] bool parse_unsigned(const std::string_view text, std::size_t &value) noexcept
{
	std::size_t parsed = 0;
	const char *const first = text.data();
	const char *const last = first + text.size();
	const auto [ptr, ec] = std::from_chars(first, last, parsed);

	if (ec != std::errc{} || ptr != last) {
		return false;
	}

	value = parsed;
	return true;
}

void print_counter(const char *const name, const std::uint64_t value) noexcept
{
	std::printf("counter %-22s %llu\n", name, static_cast<unsigned long long>(value));
}

void print_usage() noexcept
{
	std::printf("usage: depthwire_scenario_counters --scenario=NAME [--iterations=N] [--warmup=M]\n");
	std::printf("scenarios:");

	for (const Scenario &scenario : kScenarios) {
		std::printf(" %.*s", static_cast<int>(scenario.name.size()), scenario.name.data());
	}

	std::printf("\n");
}

} // namespace

int main(int argc, char **argv)
{
	std::string_view scenario_name{};
	std::size_t iterations = kDefaultIterations;
	std::size_t warmup = kDefaultWarmup;

	for (int index = 1; index < argc; ++index) {
		const std::string_view argument{argv[index]};

		if (argument == "--help") {
			print_usage();
			return EXIT_SUCCESS;
		}

		if (argument.starts_with("--scenario=")) {
			scenario_name = argument.substr(std::string_view{"--scenario="}.size());
		} else if (argument.starts_with("--iterations=")) {
			if (!parse_unsigned(argument.substr(std::string_view{"--iterations="}.size()), iterations) ||
			    iterations == 0) {
				std::fprintf(stderr, "DepthWire failure: invalid --iterations value\n");
				return EXIT_FAILURE;
			}
		} else if (argument.starts_with("--warmup=")) {
			if (!parse_unsigned(argument.substr(std::string_view{"--warmup="}.size()), warmup)) {
				std::fprintf(stderr, "DepthWire failure: invalid --warmup value\n");
				return EXIT_FAILURE;
			}
		} else {
			std::fprintf(stderr, "DepthWire failure: unknown argument: %s\n", argv[index]);
			print_usage();
			return EXIT_FAILURE;
		}
	}

	const Scenario *selected = nullptr;

	for (const Scenario &scenario : kScenarios) {
		if (scenario.name == scenario_name) {
			selected = &scenario;
			break;
		}
	}

	if (selected == nullptr) {
		std::fprintf(stderr, "DepthWire failure: unknown or missing --scenario\n");
		print_usage();
		return EXIT_FAILURE;
	}

	PerfCounterGroup perf{};

	const ScenarioResult result = selected->run(iterations, warmup, perf);

	if (!result.verified) {
		std::fprintf(stderr, "DepthWire failure: scenario %.*s did not reach its intended outcome\n",
			     static_cast<int>(scenario_name.size()), scenario_name.data());
		return EXIT_FAILURE;
	}

	std::printf("============================================================\n");
	std::printf("DepthWire - Engine Scenario Counter Benchmark\n");
	std::printf("============================================================\n");
	std::printf("DepthWire version:         %s\n", DEPTHWIRE_PROJECT_VERSION);
	std::printf("Source revision:           %s\n", DEPTHWIRE_SOURCE_REVISION);
	std::printf("Scenario:                  %.*s\n", static_cast<int>(scenario_name.size()), scenario_name.data());
	std::printf("Iterations:                %zu\n", iterations);
	std::printf("Warmup iterations:         %zu\n", warmup);
	std::printf("Sequence policy:           %s\n", result.policy);
	std::printf("Measured boundary:         %s\n", result.boundary);
	std::printf("Counters are descriptive, reported per fresh process, never subtracted or hashed.\n");

	if (result.counts.valid) {
		const std::uint64_t cycles = result.counts.cycles;
		const std::uint64_t instructions = result.counts.instructions;
		const std::uint64_t ipc_milli = cycles == 0 ? 0 : (instructions * 1000ULL) / cycles;
		const std::uint64_t cycles_per_iter_milli =
			iterations == 0 ? 0 : (cycles * 1000ULL) / static_cast<std::uint64_t>(iterations);

		print_counter("cycles:", cycles);
		print_counter("instructions:", instructions);
		print_counter("ipc-milli:", ipc_milli);
		print_counter("cycles-per-iter-milli:", cycles_per_iter_milli);
		print_counter("branches:", result.counts.branches);
		print_counter("branch-misses:", result.counts.branch_misses);
		print_counter("cache-references:", result.counts.cache_references);
		print_counter("cache-misses:", result.counts.cache_misses);
		print_counter("context-switches:", result.counts.context_switches);
		print_counter("page-faults:", result.counts.page_faults);
		print_counter("multiplexed:", result.counts.multiplexed ? 1U : 0U);
	} else {
		std::printf("perf counters unavailable: %s\n", perf.unavailable_reason());
	}

	std::printf("checksum:                  %llu\n", static_cast<unsigned long long>(result.checksum));
	std::printf("Status:                    OK\n");
	return EXIT_SUCCESS;
}
