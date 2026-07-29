#include "paced_run.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_pause.h>
#include <rte_version.h>

#include "depthwire/pipeline/packet_processor.hpp"
#include "depthwire/replay/conformance_runner.hpp"
#include "depthwire/replay/event_journal.hpp"
#include "depthwire/replay/pacer.hpp"
#include "depthwire/replay/replay_counters.hpp"
#include "depthwire/replay/run_digest.hpp"
#include "depthwire/trace/trace_codec.hpp"
#include "depthwire/wire/ethernet_frame.hpp"
#include "depthwire/wire/order_packet.hpp"
#include "depthwire/wire/wire_constants.hpp"
#include "depthwire/wire/wire_header.hpp"
#include "dpdk/config.hpp"
#include "dpdk/frame_mbuf.hpp"
#include "dpdk/latency_virtual_link.hpp"
#include "report.hpp"
#include "support/dpdk_latency_recorder.hpp"

// The paced replay loop. The pacing, loss-taxonomy, and recovery semantics
// it implements are specified in docs/benchmarks/methodology.md (replay
// section); the constraints that matter locally are stated at their sites,
// and the termination invariants are enforced through the exit status.

namespace depthwire::replay_app {

namespace {

inline constexpr std::size_t kTimerOverheadSamples = 100'000;

// The lateness recorder stores nanosecond values directly: constructing the
// cycle recorder with a 1 GHz "frequency" makes its cycle->ns conversion the
// identity.
inline constexpr std::uint64_t kNanosecondIdentityHz = 1'000'000'000;

void print_latency_summary(const char *const label, const dpdk::LatencySummary &summary)
{
	std::printf("%s:\n", label);
	std::printf("  Samples:              %zu\n", summary.samples);
	std::printf("  Mean:                 %.3f ns\n", summary.mean_ns);
	std::printf("  p50:                  %.3f ns\n", summary.p50_ns);
	std::printf("  p95:                  %.3f ns\n", summary.p95_ns);
	std::printf("  p99:                  %.3f ns\n", summary.p99_ns);
	std::printf("  p99.9:                %.3f ns\n", summary.p999_ns);
	std::printf("  Maximum:              %.3f ns\n", summary.maximum_ns);
}

struct PacedRunState {
	replay::ReplayCounters counters{};
	std::uint64_t orders_tx_accepted{};
	std::uint64_t orders_drained{};
	std::uint64_t final_elapsed_ns{};
	bool drain_stalled{};
	bool transport_invariant_violated{};
};

// Per-declared-segment accounting, reported (never hashed) so evidence shows
// backlog rising under overload and recovering afterwards.
struct SegmentMetrics {
	std::uint64_t offered{};
	std::uint64_t accepted{};
	std::uint64_t dropped{};
	std::uint64_t late{};
	std::uint64_t queued_max{};
	std::uint64_t backlog_at_end{};
};

} // namespace

int run_paced(const PacedOptions &options, const std::span<const std::byte> trace_file,
	      const trace::TraceSummary &summary)
{
	std::vector<char *> eal_arguments = options.eal_arguments;
	const int eal_result = rte_eal_init(static_cast<int>(eal_arguments.size()), eal_arguments.data());

	if (eal_result < 0) {
		std::fprintf(stderr, "DepthWire failure: EAL initialisation failed\n");
		return EXIT_FAILURE;
	}

	const std::uint64_t tsc_hz = rte_get_tsc_hz();

	if (tsc_hz == 0) {
		std::fprintf(stderr, "DepthWire failure: invalid TSC frequency\n");
		rte_eal_cleanup();
		return EXIT_FAILURE;
	}

	dpdk::DpdkLatencyRecorder timer_overhead(kTimerOverheadSamples, tsc_hz);

	for (std::size_t sample = 0; sample < kTimerOverheadSamples; ++sample) {
		const std::uint64_t start = rte_rdtsc_precise();
		const std::uint64_t end = rte_rdtsc_precise();

		if (!timer_overhead.record_cycles(end - start)) {
			break;
		}
	}

	bool success = true;
	PacedRunState state{};
	digest::Sha256Digest run_digest{};
	bool pool_restored = false;
	std::uint64_t schedule_end_ns = 0;
	dpdk::LatencyLinkStatistics port_statistics{};

	const trace::TraceView view{trace_file};
	const std::uint32_t record_count = view.record_count();

	dpdk::DpdkLatencyRecorder service_latency(record_count, tsc_hz);
	dpdk::DpdkLatencyRecorder queue_latency(record_count, tsc_hz);
	dpdk::DpdkLatencyRecorder lateness(record_count, kNanosecondIdentityHz);

	std::vector<SegmentMetrics> segment_metrics(options.segments.size());
	std::uint64_t recovery_reference_ns = 0;

	{
		dpdk::LatencyVirtualLink link{};

		if (!link.initialise()) {
			std::fprintf(stderr, "DepthWire failure: virtual DPDK link initialisation failed: %s\n",
				     rte_strerror(rte_errno));
			rte_eal_cleanup();
			return EXIT_FAILURE;
		}

		// Everything is preallocated from the validated header before the
		// run epoch: the schedule, the journal, and all bookkeeping.
		std::vector<std::uint64_t> deltas(record_count);

		for (std::uint32_t index = 0; index < record_count; ++index) {
			deltas[index] = view.arrival_delta_ns(index);
		}

		std::vector<std::uint64_t> due_offsets(record_count);

		if (!replay::build_schedule(deltas, options.segments, due_offsets)) {
			std::fprintf(stderr, "DepthWire failure: schedule arithmetic overflow; reduce the "
					     "scale factors or the trace duration\n");
			rte_eal_cleanup();
			return EXIT_FAILURE;
		}

		replay::MonotonicPacer pacer{
			due_offsets, std::chrono::nanoseconds(static_cast<std::int64_t>(options.lateness_bound_ns))};
		schedule_end_ns = pacer.schedule_end_ns();

		replay::EventJournal journal{record_count};
		std::vector<std::uint64_t> injected_tsc(record_count, 0);
		std::vector<std::uint32_t> injected_indices{};
		injected_indices.reserve(record_count);
		std::size_t rx_cursor = 0;

		std::vector<rte_mbuf *> prepared_packets(options.burst);
		std::vector<std::uint32_t> prepared_indices(options.burst);
		std::vector<rte_mbuf *> received_packets(options.burst);

		const auto config = replay::default_replay_processor_config(summary.header.bounds.instrument_id,
									    options.sequence_policy);
		pipeline::PacketProcessor<64> processor{config};

		wire::EthernetFrame staging_frame{};
		wire::encode_ethernet_header(replay::replay_market_envelope(), staging_frame);

		const unsigned int initial_mempool_available = rte_mempool_avail_count(link.mempool());

		// Recovery is measured from the load transition into the final
		// declared segment (the reduced-load segment in an overload plan)
		// until the total backlog first returns to zero; a single-segment run
		// recovers from the end of its schedule.
		recovery_reference_ns = options.segments.size() > 1 ? due_offsets[options.segments.back().first_record]
								    : schedule_end_ns;

		std::size_t active_segment = 0;
		bool final_segment_closed = false;

		std::uint64_t queued_max = 0;
		std::uint64_t recovery_end_elapsed_ns = 0;
		bool recovered = false;
		bool drained_clean = false;
		std::uint32_t idle_drain_polls = 0;

		const auto total_backlog = [&state]() noexcept {
			return (state.counters.accepted - state.counters.processed) +
			       (state.orders_tx_accepted - state.orders_drained);
		};

		const auto epoch = std::chrono::steady_clock::now();

		while (success) {
			bool progressed = false;

			// Inject every event that is due, independent of consumer
			// progress: the producer keeps pace with the schedule, so a slow
			// consumer accumulates ring backlog and eventually visible drops,
			// never a reduced offered rate. Batches never cross a declared
			// segment boundary, so every batch's accounting belongs to
			// exactly one segment; per-batch staging work stays bounded by
			// the burst size.
			for (;;) {
				// Reaching a boundary closes the finished segment with the
				// backlog at that instant, before any next-segment event is
				// issued.
				while (active_segment + 1 < options.segments.size() &&
				       pacer.issued() >= options.segments[active_segment + 1].first_record) {
					segment_metrics[active_segment].backlog_at_end = total_backlog();
					++active_segment;
				}

				std::size_t batch_limit = options.burst;

				if (active_segment + 1 < options.segments.size()) {
					batch_limit = std::min<std::size_t>(
						batch_limit,
						options.segments[active_segment + 1].first_record - pacer.issued());
				}

				const std::chrono::nanoseconds batch_elapsed =
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - epoch);
				const auto due = pacer.due_now(batch_elapsed, batch_limit);

				if (due.count == 0) {
					break;
				}

				progressed = true;
				std::uint16_t prepared = 0;

				for (std::size_t offset = 0; offset < due.count; ++offset) {
					const auto trace_index = static_cast<std::uint32_t>(due.first + offset);
					++state.counters.offered;
					++segment_metrics[active_segment].offered;

					const std::span<const std::byte> message_bytes =
						view.message_bytes(trace_index);
					std::memcpy(staging_frame.data() + wire::kWirePayloadOffset,
						    message_bytes.data(), wire::kWireMessageBytes);

					rte_mbuf *const packet = dpdk::frame_to_mbuf(link.mempool(), staging_frame);

					if (packet == nullptr) {
						++state.counters.dropped;
						++segment_metrics[active_segment].dropped;
						continue;
					}

					prepared_packets[prepared] = packet;
					prepared_indices[prepared] = trace_index;
					++prepared;
				}

				const std::uint16_t accepted =
					rte_eth_tx_burst(link.generator_port(), 0, prepared_packets.data(), prepared);
				const std::uint64_t injection_tsc = rte_rdtsc_precise();

				// Lateness is measured against the burst's actual issue time,
				// sampled after the transmit call returned.
				const std::chrono::nanoseconds issue_elapsed =
					std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - epoch);

				for (std::uint16_t index = accepted; index < prepared; ++index) {
					rte_pktmbuf_free(prepared_packets[index]);
					++state.counters.dropped;
					++segment_metrics[active_segment].dropped;
				}

				for (std::uint16_t index = 0; index < accepted; ++index) {
					const std::uint32_t trace_index = prepared_indices[index];
					++state.counters.accepted;
					++segment_metrics[active_segment].accepted;
					injected_tsc[trace_index] = injection_tsc;
					injected_indices.push_back(trace_index);

					if (pacer.is_late(trace_index, issue_elapsed)) {
						++state.counters.late;
						++segment_metrics[active_segment].late;
					}

					(void)lateness.record_cycles(pacer.lateness_ns(trace_index, issue_elapsed));
				}

				// Peak backlog (market plus orders in flight) is sampled
				// right after each injected burst, before the consumer gets
				// its turn.
				const std::uint64_t backlog_now = total_backlog();
				queued_max = std::max(queued_max, backlog_now);
				segment_metrics[active_segment].queued_max =
					std::max(segment_metrics[active_segment].queued_max, backlog_now);
			}

			if (pacer.remaining() == 0 && !final_segment_closed) {
				segment_metrics[active_segment].backlog_at_end = total_backlog();
				final_segment_closed = true;
			}

			const std::uint16_t received =
				rte_eth_rx_burst(link.engine_port(), 0, received_packets.data(), options.burst);

			for (std::uint16_t index = 0; index < received; ++index) {
				progressed = true;
				rte_mbuf *const packet = received_packets[index];
				const std::uint64_t service_start = rte_rdtsc_precise();

				if (packet->nb_segs != 1 ||
				    rte_pktmbuf_pkt_len(packet) != rte_pktmbuf_data_len(packet)) {
					rte_pktmbuf_free(packet);
					state.transport_invariant_violated = true;
					success = false;
					break;
				}

				const auto *const data = rte_pktmbuf_mtod(packet, const std::byte *);
				const std::span<const std::byte> bytes(
					data, static_cast<std::size_t>(rte_pktmbuf_data_len(packet)));

				wire::EthernetFrame output_frame{};
				const pipeline::PacketProcessResult process_result =
					processor.process(bytes, output_frame);
				rte_pktmbuf_free(packet);

				const std::uint32_t trace_index = injected_indices[rx_cursor];
				++rx_cursor;

				replay::JournalEntry entry{};
				const auto header = wire::decode_header(view.message_bytes(trace_index),
									wire::MessageType::market_data);
				entry.input_sequence = header ? header->sequence_number : 0;
				entry.process_status = process_result.status;
				entry.engine_status = process_result.engine_result.status;
				entry.has_frame = process_result.output_ready;

				if (process_result.output_ready) {
					entry.frame = output_frame;
				}

				if (!journal.record(entry)) {
					success = false;
					break;
				}

				replay::apply_process_result(state.counters, process_result);

				if (process_result.output_ready) {
					const bool transmitted = dpdk::transmit_order_frame(
						link.mempool(), link.engine_port(), output_frame);

					if (transmitted) {
						++state.orders_tx_accepted;
						const std::uint64_t service_end = rte_rdtsc_precise();
						(void)service_latency.record_cycles(service_end - service_start);
						(void)queue_latency.record_cycles(service_end -
										  injected_tsc[trace_index]);
					} else {
						++state.counters.orders_tx_dropped;
					}

					// The order left the outbox either way; release the risk
					// slot exactly once.
					if (!processor.try_acknowledge_one_order()) {
						success = false;
						break;
					}
				}
			}

			// Drain orders arriving back at the generator side.
			const std::uint16_t drained =
				rte_eth_rx_burst(link.generator_port(), 0, received_packets.data(), options.burst);

			for (std::uint16_t index = 0; index < drained; ++index) {
				progressed = true;
				rte_mbuf *const packet = received_packets[index];
				const auto *const data = rte_pktmbuf_mtod(packet, const std::byte *);
				const std::span<const std::byte> bytes(
					data, static_cast<std::size_t>(rte_pktmbuf_data_len(packet)));

				if (!wire::decode_order_frame(bytes)) {
					state.transport_invariant_violated = true;
					success = false;
				}

				rte_pktmbuf_free(packet);
				++state.orders_drained;
			}

			if (!success) {
				break;
			}

			const std::uint64_t backlog = total_backlog();
			const std::uint64_t sample_elapsed_ns =
				static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
								   std::chrono::steady_clock::now() - epoch)
								   .count());

			// First observation of an empty system at or after the recovery
			// reference point marks the end of recovery.
			if (!recovered && backlog == 0 && sample_elapsed_ns >= recovery_reference_ns) {
				recovery_end_elapsed_ns = sample_elapsed_ns;
				recovered = true;
			}

			if (pacer.remaining() == 0) {
				if (backlog == 0) {
					drained_clean = true;
					break;
				}

				if (!progressed) {
					if (++idle_drain_polls >= dpdk::kReceivePollAttempts) {
						state.drain_stalled = true;
						success = false;
						break;
					}

					rte_pause();
				} else {
					idle_drain_polls = 0;
				}
			} else if (!progressed) {
				rte_pause();
			}
		}

		state.final_elapsed_ns = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - epoch)
				.count());

		state.counters.queued_max = queued_max;
		state.counters.backlog_final = total_backlog();
		state.counters.recovery_duration_ns = drained_clean && recovered &&
								      recovery_end_elapsed_ns > recovery_reference_ns
							      ? recovery_end_elapsed_ns - recovery_reference_ns
							      : 0;

		pool_restored = rte_mempool_avail_count(link.mempool()) == initial_mempool_available;
		port_statistics = link.statistics();
		run_digest = replay::compute_run_digest(journal, state.counters);
	}

	// Termination invariants: violating any of them makes the run a harness
	// defect, not evidence.
	const bool conserved = replay::counters_conserved(state.counters);
	const bool orders_reconciled = state.counters.ordered ==
					       state.orders_tx_accepted + state.counters.orders_tx_dropped &&
				       state.orders_drained == state.orders_tx_accepted;
	const bool samples_reconciled = service_latency.size() == state.orders_tx_accepted &&
					queue_latency.size() == state.orders_tx_accepted;
	const bool clean = success && state.counters.backlog_final == 0 && pool_restored && conserved &&
			   orders_reconciled && samples_reconciled;

	std::printf("============================================================\n");
	std::printf("DepthWire - Trace Replay (paced)\n");
	std::printf("============================================================\n");
	print_identity("paced");
	std::printf("DPDK version:              %s\n", rte_version());
	std::printf("Main lcore:                %u\n", rte_lcore_id());
	std::printf("TSC frequency:             %llu Hz\n", static_cast<unsigned long long>(tsc_hz));
	std::printf("Pacing clock:              std::chrono::steady_clock\n");
	std::printf("Burst size:                %u\n", options.burst);
	std::printf("Lateness bound:            %llu ns\n", static_cast<unsigned long long>(options.lateness_bound_ns));
	std::printf("Sequence policy:           %s\n", options.sequence_policy == SequencePolicy::require_contiguous
							       ? "require_contiguous"
							       : "accept_any");

	for (std::size_t index = 0; index < options.segments.size(); ++index) {
		std::printf("segment %zu: first=%u scale-ppm=%u offered=%llu accepted=%llu "
			    "dropped=%llu late=%llu queued-max=%llu backlog-at-end=%llu\n",
			    index, options.segments[index].first_record, options.segments[index].scale_ppm,
			    static_cast<unsigned long long>(segment_metrics[index].offered),
			    static_cast<unsigned long long>(segment_metrics[index].accepted),
			    static_cast<unsigned long long>(segment_metrics[index].dropped),
			    static_cast<unsigned long long>(segment_metrics[index].late),
			    static_cast<unsigned long long>(segment_metrics[index].queued_max),
			    static_cast<unsigned long long>(segment_metrics[index].backlog_at_end));
	}

	print_trace_summary(options.trace_path, trace_file, summary);
	std::printf("Schedule end:              %llu ns\n", static_cast<unsigned long long>(schedule_end_ns));
	std::printf("Recovery reference:        %llu ns (%s)\n", static_cast<unsigned long long>(recovery_reference_ns),
		    options.segments.size() > 1 ? "start of final declared segment" : "end of schedule");
	std::printf("Run duration:              %llu ns\n", static_cast<unsigned long long>(state.final_elapsed_ns));
	std::printf("\n");

	std::printf("Timing definitions:\n");
	std::printf("  Pacing: events become due on the steady_clock schedule regardless\n");
	std::printf("  of consumer progress; the clock is re-read for every injected\n");
	std::printf("  burst of at most the configured burst size.\n");
	std::printf("  Service: per-packet processing start (after the receive burst\n");
	std::printf("  returns) -> that order's TX accepted (TSC). Waiting inside a\n");
	std::printf("  received burst is included in queue-plus-service, not service.\n");
	std::printf("  Queue+service: injection TX accepted -> order TX accepted (TSC).\n");
	std::printf("  Lateness: burst issue time minus the event's scheduled due time.\n");
	std::printf("  Recovery: recovery reference -> first observation of zero total\n");
	std::printf("  backlog (market plus orders in flight).\n");
	std::printf("  Timer overhead is reported separately and not subtracted.\n");
	std::printf("\n");

	print_latency_summary("Timer read overhead", timer_overhead.summarise());
	print_latency_summary("Per-packet service latency", service_latency.summarise());
	print_latency_summary("Queue-plus-service latency", queue_latency.summarise());
	print_latency_summary("Injection lateness", lateness.summarise());
	std::printf("\n");

	const double throughput = state.final_elapsed_ns == 0 ? 0.0
							      : static_cast<double>(state.counters.processed) * 1e9 /
									static_cast<double>(state.final_elapsed_ns);
	std::printf("Achieved throughput:       %.3f events/s (processed over run duration)\n", throughput);
	std::printf("Orders TX accepted:        %llu\n", static_cast<unsigned long long>(state.orders_tx_accepted));
	std::printf("Orders drained:            %llu\n", static_cast<unsigned long long>(state.orders_drained));
	std::printf("\n");
	print_counters(state.counters);
	std::printf("\n");
	print_hex_line("digest-sha256", run_digest);
	std::printf("\n");

	std::printf("Virtual port counters:\n");
	std::printf("  Generator TX:            %llu\n", static_cast<unsigned long long>(port_statistics.generator_tx));
	std::printf("  Generator RX:            %llu\n", static_cast<unsigned long long>(port_statistics.generator_rx));
	std::printf("  Engine RX:               %llu\n", static_cast<unsigned long long>(port_statistics.engine_rx));
	std::printf("  Engine TX:               %llu\n", static_cast<unsigned long long>(port_statistics.engine_tx));
	std::printf("\n");

	std::printf("Invariants:\n");
	std::printf("  counters conserved:      %s\n", conserved ? "yes" : "NO");
	std::printf("  orders reconciled:       %s\n", orders_reconciled ? "yes" : "NO");
	std::printf("  samples reconciled:      %s\n", samples_reconciled ? "yes" : "NO");
	std::printf("  zero final backlog:      %s\n", state.counters.backlog_final == 0 ? "yes" : "NO");
	std::printf("  mempool restored:        %s\n", pool_restored ? "yes" : "NO");

	if (state.drain_stalled) {
		std::printf("  drain stalled before reaching empty queues\n");
	}

	if (state.transport_invariant_violated) {
		std::printf("  transport invariant violated\n");
	}

	std::printf("\n");
	std::printf("Classification:\n");
	std::printf("  Replay-paced offered load over the virtual Ring PMD on one lcore.\n");
	std::printf("  Loss under overload is visible in the counters, not excluded.\n");
	std::printf("  This is not line-rate capacity, physical-NIC latency, wire\n");
	std::printf("  latency, or exchange round-trip evidence.\n");
	std::printf("\nStatus:                    %s\n", clean ? "OK" : "FAILED");

	const int cleanup_result = rte_eal_cleanup();
	return clean && cleanup_result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace depthwire::replay_app
