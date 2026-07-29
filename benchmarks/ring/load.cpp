#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <system_error>
#include <vector>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_pause.h>
#include <rte_version.h>

#include "depthwire/engine/trading_engine.hpp"
#include "depthwire/pipeline/engine_wire_adapter.hpp"
#include "depthwire/wire/ethernet_frame.hpp"
#include "dpdk/config.hpp"
#include "dpdk/latency_virtual_link.hpp"
#include "support/dpdk_latency_recorder.hpp"

namespace {

inline constexpr std::size_t kDefaultWarmupEvents = 100'000;
inline constexpr std::size_t kDefaultMeasuredEvents = 1'000'000;
inline constexpr std::uint16_t kDefaultBurstSize = 32;
inline constexpr std::uint16_t kMaximumBurstSize = 128;
inline constexpr std::size_t kTimerOverheadSamples = 100'000;

#ifndef DEPTHWIRE_PROJECT_VERSION
#	define DEPTHWIRE_PROJECT_VERSION "unknown"
#endif

#ifndef DEPTHWIRE_SOURCE_REVISION
#	define DEPTHWIRE_SOURCE_REVISION "unknown"
#endif

struct LoadConfiguration {
	std::size_t warmup_events{kDefaultWarmupEvents};
	std::size_t measured_events{kDefaultMeasuredEvents};
	std::uint16_t burst_size{kDefaultBurstSize};
};

struct LoadCounters {
	std::uint64_t requested{};
	std::uint64_t injected{};
	std::uint64_t injection_dropped{};
	std::uint64_t received{};
	std::uint64_t orders_emitted{};
	std::uint64_t tx_dropped{};
	std::uint64_t invalid_or_unexpected{};
};

struct PhaseResult {
	LoadCounters counters{};
	std::uint64_t elapsed_cycles{};
};

enum class ParseResult : std::uint8_t {
	run,
	help,
	error,
};

void print_usage(const char *const executable)
{
	std::printf("Usage: %s [EAL options] -- "
		    "[--events=N] [--warmup=N] [--burst=N]\n",
		    executable);
	std::printf("  --events=N  measured events (default %zu)\n", kDefaultMeasuredEvents);
	std::printf("  --warmup=N  warmup events (default %zu)\n", kDefaultWarmupEvents);
	std::printf("  --burst=N   packets queued per burst, 1..%u (default %u)\n", kMaximumBurstSize,
		    kDefaultBurstSize);
}

bool parse_unsigned(const std::string_view text, std::uint64_t &output) noexcept
{
	if (text.empty()) {
		return false;
	}

	const char *const first = text.data();
	const char *const last = text.data() + text.size();
	const auto result = std::from_chars(first, last, output);
	return result.ec == std::errc{} && result.ptr == last;
}

ParseResult parse_configuration(const int argc, char **const argv, const int first_application_argument,
				LoadConfiguration &output)
{
	for (int index = first_application_argument; index < argc; ++index) {
		const std::string_view argument{argv[index]};

		if (argument.empty() || argument == "--") {
			continue;
		}

		if (argument == "--help") {
			return ParseResult::help;
		}

		constexpr std::string_view events_prefix{"--events="};
		constexpr std::string_view warmup_prefix{"--warmup="};
		constexpr std::string_view burst_prefix{"--burst="};

		std::uint64_t parsed{};

		if (argument.starts_with(events_prefix)) {
			if (!parse_unsigned(argument.substr(events_prefix.size()), parsed) || parsed == 0 ||
			    parsed > std::numeric_limits<std::size_t>::max()) {
				std::fprintf(stderr, "Invalid --events value\n");
				return ParseResult::error;
			}

			output.measured_events = static_cast<std::size_t>(parsed);
			continue;
		}

		if (argument.starts_with(warmup_prefix)) {
			if (!parse_unsigned(argument.substr(warmup_prefix.size()), parsed) ||
			    parsed > std::numeric_limits<std::size_t>::max()) {
				std::fprintf(stderr, "Invalid --warmup value\n");
				return ParseResult::error;
			}

			output.warmup_events = static_cast<std::size_t>(parsed);
			continue;
		}

		if (argument.starts_with(burst_prefix)) {
			if (!parse_unsigned(argument.substr(burst_prefix.size()), parsed) || parsed == 0 ||
			    parsed > kMaximumBurstSize) {
				std::fprintf(stderr, "Invalid --burst value; expected 1..%u\n", kMaximumBurstSize);
				return ParseResult::error;
			}

			output.burst_size = static_cast<std::uint16_t>(parsed);
			continue;
		}

		std::fprintf(stderr, "Unknown application argument: %s\n", argument.data());
		return ParseResult::error;
	}

	return ParseResult::run;
}

depthwire::EngineConfig make_engine_config() noexcept
{
	depthwire::EngineConfig configuration{};
	configuration.instrument_id = 77;
	configuration.strategy.imbalance_threshold_bps = 6000;
	configuration.strategy.order_quantity = 10;
	configuration.risk.max_order_quantity = 100;
	configuration.risk.max_absolute_position = std::numeric_limits<std::int64_t>::max() / 4;
	configuration.risk.max_notional_ticks = std::numeric_limits<std::uint64_t>::max();
	configuration.risk.max_outstanding_orders = kMaximumBurstSize;
	return configuration;
}

depthwire::wire::EthernetEnvelope market_envelope() noexcept
{
	depthwire::wire::EthernetEnvelope envelope{};
	envelope.destination = {
		std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x20},
	};
	envelope.source = {
		std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x10},
	};
	return envelope;
}

depthwire::wire::EthernetEnvelope order_envelope() noexcept
{
	const auto request = market_envelope();
	return {
		request.source,
		request.destination,
	};
}

depthwire::wire::MarketDataMessage make_message(const std::uint32_t sequence, const depthwire::wire::Side side,
						const std::uint16_t level, const std::int64_t price_ticks,
						const std::uint32_t quantity) noexcept
{
	depthwire::wire::MarketDataMessage message{};
	message.sequence_number = sequence;
	message.exchange_timestamp_ns = static_cast<std::uint64_t>(sequence) * 1000ULL;
	message.instrument_id = 77;
	message.price_ticks = price_ticks;
	message.quantity = quantity;
	message.side = side;
	message.action = depthwire::wire::UpdateAction::modify;
	message.level = level;
	return message;
}

bool apply_received_market_packet(rte_mbuf *const packet, depthwire::TradingEngine<64> &engine,
				  depthwire::EngineResult &output_result) noexcept
{
	if (packet->nb_segs != 1 || rte_pktmbuf_pkt_len(packet) != rte_pktmbuf_data_len(packet)) {
		rte_pktmbuf_free(packet);
		return false;
	}

	const auto *const data = rte_pktmbuf_mtod(packet, const std::byte *);
	const auto bytes = std::span<const std::byte>(data, static_cast<std::size_t>(rte_pktmbuf_data_len(packet)));

	const auto decode_result = depthwire::wire::decode_market_data_frame(bytes);

	rte_pktmbuf_free(packet);

	if (!decode_result) {
		return false;
	}

	const auto &decoded_message = decode_result->message;

	const auto update_result = depthwire::pipeline::EngineWireAdapter::to_market_update(decoded_message);

	if (!update_result) {
		return false;
	}

	const auto &update = *update_result;

	output_result = engine.on_market_update(update);
	return true;
}

bool seed_book(depthwire::dpdk::LatencyVirtualLink &link, depthwire::TradingEngine<64> &engine,
	       std::uint32_t &sequence) noexcept
{
	const auto envelope = market_envelope();

	for (std::uint16_t side_index = 0; side_index < 2; ++side_index) {
		const auto side = side_index == 0 ? depthwire::wire::Side::sell : depthwire::wire::Side::buy;

		for (std::uint16_t level = 0; level < depthwire::kVisibleDepth; ++level) {
			const std::int64_t price = side == depthwire::wire::Side::sell ? 100100 + level
										       : 100000 - level;

			depthwire::wire::EthernetFrame frame{};

			if (!depthwire::wire::encode_market_data_frame(
				    envelope, make_message(sequence++, side, level, price, 10), frame) ||
			    !link.inject_market_frame(frame)) {
				return false;
			}

			rte_mbuf *packet{};

			if (!link.receive_engine_packet(packet)) {
				return false;
			}

			depthwire::EngineResult result{};

			if (!apply_received_market_packet(packet, engine, result) ||
			    result.status != depthwire::EngineStatus::no_order) {
				return false;
			}
		}
	}

	return true;
}

rte_mbuf *frame_to_mbuf(rte_mempool *const pool, const depthwire::wire::EthernetFrame &frame) noexcept
{
	rte_mbuf *const packet = rte_pktmbuf_alloc(pool);

	if (packet == nullptr) {
		return nullptr;
	}

	void *const destination = rte_pktmbuf_append(packet, static_cast<std::uint16_t>(frame.size()));

	if (destination == nullptr) {
		rte_pktmbuf_free(packet);
		return nullptr;
	}

	std::memcpy(destination, frame.data(), frame.size());
	return packet;
}

bool transmit_order(depthwire::dpdk::LatencyVirtualLink &link, const depthwire::OrderRequest &order,
		    std::uint32_t &outbound_sequence) noexcept
{
	const auto wire_order_result = depthwire::pipeline::EngineWireAdapter::to_wire_order(order, outbound_sequence);

	if (!wire_order_result) {
		return false;
	}

	const auto &wire_order = *wire_order_result;

	depthwire::wire::EthernetFrame frame{};

	if (!depthwire::wire::encode_order_frame(order_envelope(), wire_order, frame)) {
		return false;
	}

	rte_mbuf *const packet = frame_to_mbuf(link.mempool(), frame);

	if (packet == nullptr || !link.transmit_engine_packet(packet)) {
		return false;
	}

	++outbound_sequence;
	return true;
}

bool drain_and_validate_orders(depthwire::dpdk::LatencyVirtualLink &link, depthwire::TradingEngine<64> &engine,
			       const std::span<const std::uint32_t> source_sequences) noexcept
{
	for (const std::uint32_t source_sequence : source_sequences) {
		depthwire::wire::EthernetFrame frame{};

		if (!link.drain_generated_order(frame)) {
			return false;
		}

		const auto decode_result = depthwire::wire::decode_order_frame(std::span<const std::byte>(frame));

		if (!decode_result) {
			return false;
		}

		const auto &order = decode_result->message;

		if (order.instrument_id != 77 || order.price_ticks != 100100 || order.quantity != 10 ||
		    order.side != depthwire::wire::Side::buy || order.source_sequence != source_sequence) {
			return false;
		}

		engine.acknowledge_one_order();
	}

	return true;
}

void free_received_tail(const std::vector<rte_mbuf *> &packets, const std::uint16_t first,
			const std::uint16_t count) noexcept
{
	for (std::uint16_t index = first; index < count; ++index) {
		rte_pktmbuf_free(packets[index]);
	}
}

bool run_batch(depthwire::dpdk::LatencyVirtualLink &link, depthwire::TradingEngine<64> &engine,
	       const std::size_t requested, std::uint32_t &incoming_sequence, std::uint32_t &outbound_sequence,
	       std::vector<depthwire::wire::EthernetFrame> &frames, std::vector<rte_mbuf *> &injected_packets,
	       std::vector<rte_mbuf *> &received_packets, std::vector<std::uint32_t> &injected_sequences,
	       std::vector<std::uint32_t> &transmitted_sequences,
	       depthwire::dpdk::DpdkLatencyRecorder *const service_recorder,
	       depthwire::dpdk::DpdkLatencyRecorder *const queue_recorder, LoadCounters &counters) noexcept
{
	injected_packets.clear();
	injected_sequences.clear();
	transmitted_sequences.clear();
	counters.requested += requested;

	for (std::size_t index = 0; index < requested; ++index) {
		const std::uint32_t sequence = incoming_sequence;
		const std::uint32_t quantity = 1000U + static_cast<std::uint32_t>(sequence & 1U);

		if (!depthwire::wire::encode_market_data_frame(
			    market_envelope(), make_message(sequence, depthwire::wire::Side::buy, 0, 100000, quantity),
			    frames[index])) {
			++counters.invalid_or_unexpected;
			return false;
		}

		rte_mbuf *const packet = frame_to_mbuf(link.mempool(), frames[index]);

		if (packet == nullptr) {
			++counters.injection_dropped;
			continue;
		}

		injected_packets.push_back(packet);
		injected_sequences.push_back(sequence);
		++incoming_sequence;
	}

	if (injected_packets.empty()) {
		return true;
	}

	const std::uint16_t prepared = static_cast<std::uint16_t>(injected_packets.size());
	const std::uint16_t accepted = rte_eth_tx_burst(link.generator_port(), 0, injected_packets.data(), prepared);

	for (std::uint16_t index = accepted; index < prepared; ++index) {
		rte_pktmbuf_free(injected_packets[index]);
	}

	counters.injected += accepted;
	counters.injection_dropped += static_cast<std::uint64_t>(prepared - accepted);
	incoming_sequence -= static_cast<std::uint32_t>(prepared - accepted);

	if (accepted == 0) {
		return true;
	}

	injected_sequences.resize(accepted);
	const std::uint64_t burst_accepted_cycles = rte_rdtsc_precise();
	std::size_t total_received = 0;
	std::uint32_t empty_polls = 0;

	while (total_received < accepted) {
		const std::uint16_t remaining = static_cast<std::uint16_t>(accepted - total_received);
		const std::uint16_t received =
			rte_eth_rx_burst(link.engine_port(), 0, received_packets.data(), remaining);

		if (received == 0) {
			if (++empty_polls >= depthwire::dpdk::kReceivePollAttempts) {
				++counters.invalid_or_unexpected;
				return false;
			}

			rte_pause();
			continue;
		}

		empty_polls = 0;

		for (std::uint16_t index = 0; index < received; ++index) {
			const std::uint64_t service_start_cycles = rte_rdtsc_precise();
			depthwire::EngineResult engine_result{};

			if (!apply_received_market_packet(received_packets[index], engine, engine_result) ||
			    engine_result.status != depthwire::EngineStatus::order_emitted ||
			    engine_result.signal != depthwire::Signal::buy) {
				++counters.invalid_or_unexpected;
				free_received_tail(received_packets, static_cast<std::uint16_t>(index + 1), received);
				return false;
			}

			++counters.received;

			depthwire::OrderRequest order{};

			if (!engine.pop_order(order)) {
				++counters.invalid_or_unexpected;
				free_received_tail(received_packets, static_cast<std::uint16_t>(index + 1), received);
				return false;
			}

			const std::uint32_t expected_source_sequence = injected_sequences[total_received + index];

			if (order.source_sequence != expected_source_sequence) {
				++counters.invalid_or_unexpected;
				free_received_tail(received_packets, static_cast<std::uint16_t>(index + 1), received);
				return false;
			}

			if (!transmit_order(link, order, outbound_sequence)) {
				++counters.tx_dropped;
				engine.acknowledge_one_order();
				continue;
			}

			const std::uint64_t service_end_cycles = rte_rdtsc_precise();
			++counters.orders_emitted;
			transmitted_sequences.push_back(expected_source_sequence);

			if (service_recorder != nullptr &&
			    (!service_recorder->record_cycles(service_end_cycles - service_start_cycles) ||
			     !queue_recorder->record_cycles(service_end_cycles - burst_accepted_cycles))) {
				++counters.invalid_or_unexpected;
				free_received_tail(received_packets, static_cast<std::uint16_t>(index + 1), received);
				return false;
			}
		}

		total_received += received;
	}

	return drain_and_validate_orders(link, engine, std::span<const std::uint32_t>(transmitted_sequences));
}

bool run_phase(depthwire::dpdk::LatencyVirtualLink &link, depthwire::TradingEngine<64> &engine,
	       const std::size_t events, const std::uint16_t burst_size, std::uint32_t &incoming_sequence,
	       std::uint32_t &outbound_sequence, std::vector<depthwire::wire::EthernetFrame> &frames,
	       std::vector<rte_mbuf *> &injected_packets, std::vector<rte_mbuf *> &received_packets,
	       std::vector<std::uint32_t> &injected_sequences, std::vector<std::uint32_t> &transmitted_sequences,
	       depthwire::dpdk::DpdkLatencyRecorder *const service_recorder,
	       depthwire::dpdk::DpdkLatencyRecorder *const queue_recorder, PhaseResult &output) noexcept
{
	const std::uint64_t phase_start_cycles = rte_rdtsc_precise();
	std::size_t attempted = 0;

	while (attempted < events) {
		const std::size_t requested = std::min<std::size_t>(burst_size, events - attempted);

		if (!run_batch(link, engine, requested, incoming_sequence, outbound_sequence, frames, injected_packets,
			       received_packets, injected_sequences, transmitted_sequences, service_recorder,
			       queue_recorder, output.counters)) {
			return false;
		}

		attempted += requested;
	}

	output.elapsed_cycles = rte_rdtsc_precise() - phase_start_cycles;
	return true;
}

void print_latency_summary(const depthwire::dpdk::LatencySummary &summary)
{
	std::printf("  Samples:              %zu\n", summary.samples);
	std::printf("  Mean:                 %.3f ns\n", summary.mean_ns);
	std::printf("  p50:                  %.3f ns\n", summary.p50_ns);
	std::printf("  p95:                  %.3f ns\n", summary.p95_ns);
	std::printf("  p99:                  %.3f ns\n", summary.p99_ns);
	std::printf("  p99.9:                %.3f ns\n", summary.p999_ns);
	std::printf("  Maximum:              %.3f ns\n", summary.maximum_ns);
}

double events_per_second(const std::uint64_t events, const std::uint64_t cycles, const std::uint64_t tsc_hz) noexcept
{
	if (cycles == 0 || tsc_hz == 0) {
		return 0.0;
	}

	return static_cast<double>((static_cast<long double>(events) * static_cast<long double>(tsc_hz)) /
				   static_cast<long double>(cycles));
}

void print_counters(const char *const label, const LoadCounters &counters)
{
	std::printf("%s:\n", label);
	std::printf("  Requested packets:       %llu\n", static_cast<unsigned long long>(counters.requested));
	std::printf("  Injected packets:        %llu\n", static_cast<unsigned long long>(counters.injected));
	std::printf("  Injection drops:         %llu\n", static_cast<unsigned long long>(counters.injection_dropped));
	std::printf("  Engine RX packets:       %llu\n", static_cast<unsigned long long>(counters.received));
	std::printf("  Orders TX accepted:      %llu\n", static_cast<unsigned long long>(counters.orders_emitted));
	std::printf("  Order TX drops:          %llu\n", static_cast<unsigned long long>(counters.tx_dropped));
	std::printf("  Invalid/unexpected:      %llu\n",
		    static_cast<unsigned long long>(counters.invalid_or_unexpected));
}

} // namespace

int main(int argc, char **argv)
{
	const int eal_arguments = rte_eal_init(argc, argv);

	if (eal_arguments < 0) {
		std::fprintf(stderr, "DepthWire failure: EAL initialisation failed\n");
		return EXIT_FAILURE;
	}

	LoadConfiguration configuration{};
	const ParseResult parse_result = parse_configuration(argc, argv, eal_arguments + 1, configuration);

	if (parse_result != ParseResult::run) {
		if (parse_result == ParseResult::help) {
			print_usage(argv[0]);
		}

		const int cleanup_result = rte_eal_cleanup();
		return parse_result == ParseResult::help && cleanup_result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	const std::uint64_t tsc_hz = rte_get_tsc_hz();

	if (tsc_hz == 0) {
		std::fprintf(stderr, "DepthWire failure: invalid TSC frequency\n");
		rte_eal_cleanup();
		return EXIT_FAILURE;
	}

	depthwire::dpdk::DpdkLatencyRecorder timer_overhead(kTimerOverheadSamples, tsc_hz);

	bool success = true;

	for (std::size_t sample = 0; sample < kTimerOverheadSamples; ++sample) {
		const std::uint64_t start = rte_rdtsc_precise();
		const std::uint64_t end = rte_rdtsc_precise();
		success = timer_overhead.record_cycles(end - start);

		if (!success) {
			break;
		}
	}

	depthwire::dpdk::DpdkLatencyRecorder service_latency(configuration.measured_events, tsc_hz);
	depthwire::dpdk::DpdkLatencyRecorder queue_latency(configuration.measured_events, tsc_hz);

	std::vector<depthwire::wire::EthernetFrame> frames(configuration.burst_size);
	std::vector<rte_mbuf *> injected_packets{};
	std::vector<rte_mbuf *> received_packets(configuration.burst_size);
	std::vector<std::uint32_t> injected_sequences{};
	std::vector<std::uint32_t> transmitted_sequences{};

	injected_packets.reserve(configuration.burst_size);
	injected_sequences.reserve(configuration.burst_size);
	transmitted_sequences.reserve(configuration.burst_size);

	PhaseResult warmup_result{};
	PhaseResult measured_result{};
	depthwire::dpdk::LatencyLinkStatistics port_statistics{};

	{
		depthwire::dpdk::LatencyVirtualLink link{};

		if (!link.initialise()) {
			std::fprintf(stderr, "DepthWire failure: virtual DPDK link initialisation failed: %s\n",
				     rte_strerror(rte_errno));
			success = false;
		}

		depthwire::TradingEngine<64> engine(make_engine_config());
		std::uint32_t incoming_sequence = 1;
		std::uint32_t outbound_sequence = 1;

		if (success && !seed_book(link, engine, incoming_sequence)) {
			std::fprintf(stderr, "DepthWire failure: book seeding failed\n");
			success = false;
		}

		if (success &&
		    !run_phase(link, engine, configuration.warmup_events, configuration.burst_size, incoming_sequence,
			       outbound_sequence, frames, injected_packets, received_packets, injected_sequences,
			       transmitted_sequences, nullptr, nullptr, warmup_result)) {
			std::fprintf(stderr, "DepthWire failure: warmup burst phase failed\n");
			success = false;
		}

		if (success &&
		    !run_phase(link, engine, configuration.measured_events, configuration.burst_size, incoming_sequence,
			       outbound_sequence, frames, injected_packets, received_packets, injected_sequences,
			       transmitted_sequences, &service_latency, &queue_latency, measured_result)) {
			std::fprintf(stderr, "DepthWire failure: measured burst phase failed\n");
			success = false;
		}

		if (success) {
			port_statistics = link.statistics();
		}
	}

	if (success) {
		const auto overhead_summary = timer_overhead.summarise();
		const auto service_summary = service_latency.summarise();
		const auto queue_summary = queue_latency.summarise();
		const double harness_throughput = events_per_second(measured_result.counters.orders_emitted,
								    measured_result.elapsed_cycles, tsc_hz);

		std::printf("============================================================\n");
		std::printf("DepthWire - Virtual Ring PMD Burst/Load Benchmark\n");
		std::printf("============================================================\n");
		std::printf("DepthWire version:         %s\n", DEPTHWIRE_PROJECT_VERSION);
		std::printf("Source revision:           %s\n", DEPTHWIRE_SOURCE_REVISION);
		std::printf("DPDK version:              %s\n", rte_version());
		std::printf("Main lcore:                %u\n", rte_lcore_id());
		std::printf("TSC frequency:             %llu Hz\n", static_cast<unsigned long long>(tsc_hz));
		std::printf("Configured burst size:     %u\n", configuration.burst_size);
		std::printf("Warmup events:             %zu\n", configuration.warmup_events);
		std::printf("Measured events:           %zu\n", configuration.measured_events);
		std::printf("\n");

		std::printf("Timing definitions:\n");
		std::printf("  Service: each packet's RX return -> its order TX accepted.\n");
		std::printf("  Queue:   input burst accepted -> each order TX accepted.\n");
		std::printf("  Throughput includes this single-lcore closed-loop harness,\n");
		std::printf("  including batch preparation, injection, processing, and drain.\n");
		std::printf("  Timer overhead is reported separately and not subtracted.\n");
		std::printf("\n");

		std::printf("Timer read overhead:\n");
		print_latency_summary(overhead_summary);
		std::printf("\nPer-packet service latency:\n");
		print_latency_summary(service_summary);
		std::printf("\nBurst queue-plus-service latency:\n");
		print_latency_summary(queue_summary);
		std::printf("\n");

		std::printf("Measured closed-loop harness throughput: %.3f events/s\n", harness_throughput);
		std::printf("\n");
		print_counters("Warmup counters", warmup_result.counters);
		std::printf("\n");
		print_counters("Measured counters", measured_result.counters);
		std::printf("\n");

		std::printf("Virtual port counters including seed and warmup:\n");
		std::printf("  Generator TX:            %llu\n",
			    static_cast<unsigned long long>(port_statistics.generator_tx));
		std::printf("  Generator RX:            %llu\n",
			    static_cast<unsigned long long>(port_statistics.generator_rx));
		std::printf("  Engine RX:               %llu\n",
			    static_cast<unsigned long long>(port_statistics.engine_rx));
		std::printf("  Engine TX:               %llu\n",
			    static_cast<unsigned long long>(port_statistics.engine_tx));
		std::printf("\n");

		std::printf("Classification:\n");
		std::printf("  Deterministic virtual Ring PMD burst pressure on one lcore.\n");
		std::printf("  This is not replay-driven arrival timing, line-rate capacity,\n");
		std::printf("  physical-NIC latency, wire latency, or exchange RTT.\n");
		std::printf("\nStatus:                    OK\n");
	}

	const int cleanup_result = rte_eal_cleanup();

	if (!success || cleanup_result != 0) {
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
