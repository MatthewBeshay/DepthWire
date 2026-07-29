#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <vector>

#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include "depthwire/engine/trading_engine.hpp"
#include "depthwire/pipeline/packet_processor.hpp"
#include "depthwire/replay/conformance_runner.hpp"
#include "depthwire/wire/ethernet_frame.hpp"
#include "depthwire/wire/market_data_packet.hpp"
#include "dpdk/config.hpp"
#include "dpdk/frame_mbuf.hpp"
#include "dpdk/latency_virtual_link.hpp"

// Recovery-ownership scenarios for the replay

namespace {

using namespace depthwire;

bool check(const bool condition, const char *const what)
{
	if (!condition) {
		std::fprintf(stderr, "FAILED: %s\n", what);
	}

	return condition;
}

wire::MarketDataMessage make_message(const std::uint32_t sequence, const wire::Side side, const std::uint16_t level,
				     const std::int64_t price_ticks, const std::uint32_t quantity) noexcept
{
	wire::MarketDataMessage message{};
	message.sequence_number = sequence;
	message.exchange_timestamp_ns = static_cast<std::uint64_t>(sequence) * 1000ULL;
	message.instrument_id = 77;
	message.price_ticks = price_ticks;
	message.quantity = quantity;
	message.side = side;
	message.action = wire::UpdateAction::modify;
	message.level = level;
	return message;
}

wire::MarketDataMessage imbalance_message(const std::uint32_t sequence) noexcept
{
	return make_message(sequence, wire::Side::buy, 0, 100000, 1000);
}

bool process_over_link(dpdk::LatencyVirtualLink &link, pipeline::PacketProcessor<64> &processor,
		       const wire::MarketDataMessage &message, pipeline::PacketProcessResult &result)
{
	wire::EthernetFrame frame{};

	if (!wire::encode_market_data_frame(replay::replay_market_envelope(), message, frame) ||
	    !link.inject_market_frame(frame)) {
		return false;
	}

	rte_mbuf *packet{};

	if (!link.receive_engine_packet(packet)) {
		return false;
	}

	if (packet->nb_segs != 1 || rte_pktmbuf_pkt_len(packet) != rte_pktmbuf_data_len(packet)) {
		rte_pktmbuf_free(packet);
		return false;
	}

	const auto *const data = rte_pktmbuf_mtod(packet, const std::byte *);
	const std::span<const std::byte> bytes(data, static_cast<std::size_t>(rte_pktmbuf_data_len(packet)));

	wire::EthernetFrame output{};
	result = processor.process(bytes, output);
	rte_pktmbuf_free(packet);

	if (result.output_ready && !processor.try_acknowledge_one_order()) {
		return false;
	}

	return true;
}

bool seed_book(dpdk::LatencyVirtualLink &link, pipeline::PacketProcessor<64> &processor, std::uint32_t &sequence)
{
	for (std::uint16_t level = 0; level < 5; ++level) {
		pipeline::PacketProcessResult result{};

		if (!process_over_link(link, processor,
				       make_message(sequence++, wire::Side::sell, level, 100100 + level, 10), result) ||
		    result.status != pipeline::PacketProcessStatus::no_order) {
			return false;
		}
	}

	for (std::uint16_t level = 0; level < 5; ++level) {
		pipeline::PacketProcessResult result{};

		if (!process_over_link(link, processor,
				       make_message(sequence++, wire::Side::buy, level, 100000 - level, 10), result) ||
		    result.status != pipeline::PacketProcessStatus::no_order) {
			return false;
		}
	}

	return true;
}

// A gap is rejected without state advancement; supplying the missing event
// and replaying the later one recovers the feed (the engine's sequence
// contract, exercised over the ring transport).
bool scenario_missing_sequence_recovery(dpdk::LatencyVirtualLink &link)
{
	const auto config = replay::default_replay_processor_config(77, SequencePolicy::require_contiguous);
	pipeline::PacketProcessor<64> processor{config};

	std::uint32_t sequence = 1;
	bool ok = check(seed_book(link, processor, sequence), "missing-sequence: book seeding");

	pipeline::PacketProcessResult result{};
	ok = ok && check(process_over_link(link, processor, imbalance_message(11), result) &&
				 result.status == pipeline::PacketProcessStatus::order_encoded,
			 "missing-sequence: sequence 11 emits an order");

	// Skip 12; 13 must be rejected as a gap without advancing the anchor.
	ok = ok && check(process_over_link(link, processor, imbalance_message(13), result) &&
				 result.status == pipeline::PacketProcessStatus::engine_rejected &&
				 result.engine_result.status == EngineStatus::sequence_gap,
			 "missing-sequence: gap rejected");
	ok = ok && check(processor.engine().last_sequence_number().value_or(0) == 11,
			 "missing-sequence: anchor unchanged after gap");

	// Supply the missing event, then replay the later one, then resume.
	ok = ok && check(process_over_link(link, processor, imbalance_message(12), result) &&
				 result.status == pipeline::PacketProcessStatus::order_encoded,
			 "missing-sequence: supplied event applies");
	ok = ok && check(process_over_link(link, processor, imbalance_message(13), result) &&
				 result.status == pipeline::PacketProcessStatus::order_encoded,
			 "missing-sequence: later event replays");
	ok = ok && check(process_over_link(link, processor, imbalance_message(14), result) &&
				 result.status == pipeline::PacketProcessStatus::order_encoded,
			 "missing-sequence: feed resumes");

	return ok;
}

// A full outbox backpressures transactionally: the same update retries
// successfully after one order is drained, without a duplicate rejection.
bool scenario_outbox_drain_and_retry()
{
	EngineConfig config{};
	config.instrument_id = 77;
	config.strategy.imbalance_threshold_bps = 6000;
	config.strategy.order_quantity = 10;
	config.risk.max_order_quantity = 100;
	config.risk.max_absolute_position = 1'000'000;
	config.risk.max_notional_ticks = 1'000'000'000'000ULL;
	config.risk.max_outstanding_orders = 64;

	TradingEngine<2> engine{config};
	std::uint32_t sequence = 1;
	bool ok = true;

	for (std::uint16_t level = 0; level < 5; ++level) {
		ok = ok && engine.on_market_update({sequence++, 0, 77, 100100 + level, 10, Side::sell,
						    UpdateAction::modify, level, 0})
					   .status == EngineStatus::no_order;
		ok = ok && engine.on_market_update({sequence++, 0, 77, 100000 - level, 10, Side::buy,
						    UpdateAction::modify, level, 0})
					   .status == EngineStatus::no_order;
	}

	ok = check(ok, "outbox: book seeding");

	// Two orders fill the two-slot outbox.
	const MarketUpdate stimulus{sequence, 0, 77, 100000, 1000, Side::buy, UpdateAction::modify, 0, 0};
	MarketUpdate update = stimulus;
	ok = ok && check(engine.on_market_update(update).status == EngineStatus::order_emitted, "outbox: first order");
	update.sequence_number = ++sequence;
	ok = ok && check(engine.on_market_update(update).status == EngineStatus::order_emitted, "outbox: second order");

	// The third update is rejected before any mutation...
	update.sequence_number = ++sequence;
	ok = ok && check(engine.on_market_update(update).status == EngineStatus::outbox_full,
			 "outbox: full outbox backpressures");

	// ...so draining one order and retrying the identical update succeeds.
	OrderRequest drained{};
	ok = ok && check(engine.pop_order(drained), "outbox: drain one order");
	ok = ok && check(engine.on_market_update(update).status == EngineStatus::order_emitted,
			 "outbox: same update retries after drain");

	return ok;
}

// Risk outstanding-order capacity is restored by acknowledgements; unlike
// outbox backpressure, a risk rejection consumes the input sequence, so
// recovery continues with the next event (a deliberate consequence of the
// engine's transactional-backpressure design).
bool scenario_risk_outstanding_release()
{
	auto config = replay::default_replay_processor_config(77, SequencePolicy::require_contiguous);
	config.engine.risk.max_outstanding_orders = 2;

	pipeline::PacketProcessor<64> processor{config};
	std::uint32_t sequence = 1;
	bool ok = true;

	for (std::uint16_t level = 0; level < 5; ++level) {
		wire::EthernetFrame frame{};
		ok = ok && wire::encode_market_data_frame(
				   replay::replay_market_envelope(),
				   make_message(sequence++, wire::Side::sell, level, 100100 + level, 10), frame);
		wire::EthernetFrame output{};
		ok = ok && processor.process(frame, output).status == pipeline::PacketProcessStatus::no_order;
	}

	for (std::uint16_t level = 0; level < 5; ++level) {
		wire::EthernetFrame frame{};
		ok = ok && wire::encode_market_data_frame(
				   replay::replay_market_envelope(),
				   make_message(sequence++, wire::Side::buy, level, 100000 - level, 10), frame);
		wire::EthernetFrame output{};
		ok = ok && processor.process(frame, output).status == pipeline::PacketProcessStatus::no_order;
	}

	ok = check(ok, "risk: book seeding");

	const auto process_imbalance = [&](const std::uint32_t event_sequence, pipeline::PacketProcessResult &result) {
		wire::EthernetFrame frame{};

		if (!wire::encode_market_data_frame(replay::replay_market_envelope(), imbalance_message(event_sequence),
						    frame)) {
			return false;
		}

		wire::EthernetFrame output{};
		result = processor.process(frame, output);
		return true;
	};

	// Two unacknowledged orders exhaust the outstanding-order budget.
	pipeline::PacketProcessResult result{};
	ok = ok && check(process_imbalance(11, result) && result.status == pipeline::PacketProcessStatus::order_encoded,
			 "risk: first order");
	ok = ok && check(process_imbalance(12, result) && result.status == pipeline::PacketProcessStatus::order_encoded,
			 "risk: second order");
	ok = ok &&
	     check(process_imbalance(13, result) && result.status == pipeline::PacketProcessStatus::engine_rejected &&
			   result.engine_result.status == EngineStatus::risk_rejected,
		   "risk: third order rejected at capacity");

	// Acknowledging one order restores capacity for the next event.
	ok = ok && check(processor.try_acknowledge_one_order(), "risk: acknowledge one");
	ok = ok && check(process_imbalance(14, result) && result.status == pipeline::PacketProcessStatus::order_encoded,
			 "risk: capacity restored");

	return ok;
}

// Outbound order-TX rejection: when the order ring is full, the engine-side
// transmit is rejected. This exercises the paced harness's actual outbound
// branch (dpdk::transmit_order_frame, shared production code): the rejected
// mbuf must be freed, the risk slot must still be released exactly once per
// encoded order, and draining the ring must restore the mempool exactly.
bool scenario_order_tx_rejection(dpdk::LatencyVirtualLink &link)
{
	const unsigned int initial_available = rte_mempool_avail_count(link.mempool());

	const auto config = replay::default_replay_processor_config(77, SequencePolicy::accept_any);
	pipeline::PacketProcessor<64> processor{config};

	// Seed the book directly (transport-neutral), like the paced harness's
	// engine state before stimulus.
	bool ok = true;
	std::uint32_t sequence = 1;

	for (std::uint16_t level = 0; level < 5; ++level) {
		wire::EthernetFrame frame{};
		ok = ok && wire::encode_market_data_frame(
				   replay::replay_market_envelope(),
				   make_message(sequence++, wire::Side::sell, level, 100100 + level, 10), frame)
				   .has_value();
		wire::EthernetFrame output{};
		ok = ok && processor.process(frame, output).status == pipeline::PacketProcessStatus::no_order;
	}

	for (std::uint16_t level = 0; level < 5; ++level) {
		wire::EthernetFrame frame{};
		ok = ok && wire::encode_market_data_frame(
				   replay::replay_market_envelope(),
				   make_message(sequence++, wire::Side::buy, level, 100000 - level, 10), frame)
				   .has_value();
		wire::EthernetFrame output{};
		ok = ok && processor.process(frame, output).status == pipeline::PacketProcessStatus::no_order;
	}

	ok = check(ok, "order-tx: book seeding");

	// Emit orders into the order ring without draining it, acknowledging
	// exactly once per encoded order whether or not the transport accepted
	// it, exactly as the paced harness does.
	std::uint64_t tx_accepted = 0;
	std::uint64_t tx_rejected = 0;

	while (ok && tx_rejected == 0) {
		wire::EthernetFrame frame{};

		if (!wire::encode_market_data_frame(replay::replay_market_envelope(), imbalance_message(sequence++),
						    frame)) {
			ok = check(false, "order-tx: stimulus encode");
			break;
		}

		wire::EthernetFrame output{};
		const auto result = processor.process(frame, output);

		if (result.status != pipeline::PacketProcessStatus::order_encoded) {
			ok = check(false, "order-tx: stimulus emits an order");
			break;
		}

		if (dpdk::transmit_order_frame(link.mempool(), link.engine_port(), output)) {
			++tx_accepted;
		} else {
			++tx_rejected;
		}

		if (!check(processor.try_acknowledge_one_order(),
			   "order-tx: risk slot released exactly once per order")) {
			ok = false;
			break;
		}

		if (!check(tx_accepted <= dpdk::kRingSize, "order-tx: ring bounded")) {
			ok = false;
			break;
		}
	}

	ok = ok && check(tx_rejected == 1, "order-tx: rejection observed at ring capacity");
	ok = ok && check(tx_accepted == dpdk::kRingSize - 1, "order-tx: ring accepted capacity - 1 orders");
	ok = ok && check(rte_mempool_avail_count(link.mempool()) == initial_available - tx_accepted,
			 "order-tx: rejected mbuf freed, in-flight orders accounted");

	// Risk capacity is intact after the rejection (slots were released
	// exactly once), so the next stimulus still emits an order.
	{
		wire::EthernetFrame frame{};
		wire::EthernetFrame output{};
		ok = ok &&
		     wire::encode_market_data_frame(replay::replay_market_envelope(), imbalance_message(sequence++),
						    frame)
			     .has_value() &&
		     check(processor.process(frame, output).status == pipeline::PacketProcessStatus::order_encoded,
			   "order-tx: capacity intact after rejection");
		ok = ok && check(processor.try_acknowledge_one_order(), "order-tx: final acknowledge");
	}

	// Drain every queued order back off the generator side and restore the
	// pool exactly.
	std::vector<rte_mbuf *> packets(32);
	std::uint64_t drained = 0;

	for (;;) {
		const std::uint16_t received = rte_eth_rx_burst(link.generator_port(), 0, packets.data(),
								static_cast<std::uint16_t>(packets.size()));

		if (received == 0) {
			break;
		}

		for (std::uint16_t index = 0; index < received; ++index) {
			rte_pktmbuf_free(packets[index]);
		}

		drained += received;
	}

	ok = ok && check(drained == tx_accepted, "order-tx: all queued orders drained");
	ok = ok &&
	     check(rte_mempool_avail_count(link.mempool()) == initial_available, "order-tx: mempool fully restored");

	return ok;
}

// Ring TX rejection frees the rejected mbuf immediately, and draining the
// ring restores the mempool exactly.
bool scenario_tx_rejection_and_pool_accounting(dpdk::LatencyVirtualLink &link)
{
	const unsigned int initial_available = rte_mempool_avail_count(link.mempool());

	wire::EthernetFrame frame{};
	bool ok = check(wire::encode_market_data_frame(replay::replay_market_envelope(), imbalance_message(1), frame)
				.has_value(),
			"tx-rejection: frame encode");

	// Fill the market ring to capacity; every accepted injection holds one
	// mbuf.
	std::uint32_t accepted = 0;

	while (link.inject_market_frame(frame)) {
		++accepted;

		if (!check(accepted <= dpdk::kRingSize, "tx-rejection: ring bounded")) {
			return false;
		}
	}

	ok = ok && check(accepted == dpdk::kRingSize - 1, "tx-rejection: ring accepts capacity - 1 packets");
	ok = ok && check(rte_mempool_avail_count(link.mempool()) == initial_available - accepted,
			 "tx-rejection: in-flight mbufs accounted");

	// The rejected injection freed its mbuf (inject_market_frame frees on
	// rejection), so draining everything restores the pool exactly.
	std::vector<rte_mbuf *> packets(32);
	std::uint32_t drained = 0;

	for (;;) {
		const std::uint16_t received = rte_eth_rx_burst(link.engine_port(), 0, packets.data(),
								static_cast<std::uint16_t>(packets.size()));

		if (received == 0) {
			break;
		}

		for (std::uint16_t index = 0; index < received; ++index) {
			rte_pktmbuf_free(packets[index]);
		}

		drained += received;
	}

	ok = ok && check(drained == accepted, "tx-rejection: all queued packets drained");
	ok = ok && check(rte_mempool_avail_count(link.mempool()) == initial_available,
			 "tx-rejection: mempool fully restored");

	return ok;
}

// Shutting down with queued mbufs must not leak: the link drains its rings
// before freeing the pool. Ring-PMD ethdev names are not released within a
// process, so cleanup is proven by re-creating a mempool under the same name
// (which fails while the old pool exists) rather than a whole second link.
// This scenario must run last: it destroys the shared link.
bool scenario_shutdown_with_queued_mbufs(dpdk::LatencyVirtualLink &link)
{
	const unsigned int initial_available = rte_mempool_avail_count(link.mempool());

	wire::EthernetFrame frame{};

	if (!check(wire::encode_market_data_frame(replay::replay_market_envelope(), imbalance_message(1), frame)
			   .has_value(),
		   "shutdown: frame encode")) {
		return false;
	}

	for (std::uint32_t count = 0; count < 100; ++count) {
		if (!check(link.inject_market_frame(frame), "shutdown: injection")) {
			return false;
		}
	}

	if (!check(rte_mempool_avail_count(link.mempool()) == initial_available - 100, "shutdown: queued mbufs held")) {
		return false;
	}

	link.shutdown();

	rte_mempool *const fresh_pool = rte_pktmbuf_pool_create(dpdk::kLatencyMempoolName, dpdk::kMempoolSize,
								dpdk::kMempoolCacheSize, 0, dpdk::kDataRoomSize,
								static_cast<int>(rte_socket_id()));

	const bool ok =
		check(fresh_pool != nullptr, "shutdown: mempool name released after shutdown") &&
		check(rte_mempool_avail_count(fresh_pool) == initial_available, "shutdown: fresh pool fully available");

	if (fresh_pool != nullptr) {
		rte_mempool_free(fresh_pool);
	}

	return ok;
}

} // namespace

int main(int argc, char **argv)
{
	if (rte_eal_init(argc, argv) < 0) {
		std::fprintf(stderr, "DepthWire failure: EAL initialisation failed\n");
		return EXIT_FAILURE;
	}

	bool ok = false;

	{
		dpdk::LatencyVirtualLink link{};

		if (check(link.initialise(), "shared link initialisation")) {
			ok = scenario_missing_sequence_recovery(link);
			ok = scenario_outbox_drain_and_retry() && ok;
			ok = scenario_risk_outstanding_release() && ok;
			ok = scenario_order_tx_rejection(link) && ok;
			ok = scenario_tx_rejection_and_pool_accounting(link) && ok;
			ok = scenario_shutdown_with_queued_mbufs(link) && ok;
		}
	}

	const int cleanup_result = rte_eal_cleanup();

	if (!ok || cleanup_result != 0) {
		return EXIT_FAILURE;
	}

	std::printf("replay recovery scenarios: OK\n");
	return EXIT_SUCCESS;
}
