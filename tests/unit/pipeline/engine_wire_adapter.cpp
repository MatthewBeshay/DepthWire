#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

#include "depthwire/engine/trading_engine.hpp"
#include "depthwire/pipeline/engine_wire_adapter.hpp"
#include "depthwire/wire/order_packet.hpp"

namespace {

depthwire::wire::MarketDataMessage make_market_message(const std::uint32_t sequence, const depthwire::wire::Side side,
						       const std::uint16_t level, const std::int64_t price_ticks,
						       const std::uint32_t quantity)
{
	depthwire::wire::MarketDataMessage message{};
	message.sequence_number = sequence;
	message.exchange_timestamp_ns = sequence * 1000ULL;
	message.instrument_id = 77;
	message.price_ticks = price_ticks;
	message.quantity = quantity;
	message.side = side;
	message.action = depthwire::wire::UpdateAction::modify;
	message.level = level;
	return message;
}

depthwire::EngineResult send_to_engine(depthwire::TradingEngine<16> &engine,
				       const depthwire::wire::MarketDataMessage &wire_message)
{
	const auto update_result = depthwire::pipeline::EngineWireAdapter::to_market_update(wire_message);

	assert(update_result);

	const auto &update = *update_result;

	return engine.on_market_update(update);
}

} // namespace

int main()
{
	depthwire::EngineConfig config{};
	config.instrument_id = 77;
	config.strategy.imbalance_threshold_bps = 6000;
	config.strategy.order_quantity = 10;
	config.risk.max_order_quantity = 100;
	config.risk.max_absolute_position = 1000;
	config.risk.max_notional_ticks = 1'000'000'000ULL;

	depthwire::TradingEngine<16> engine(config);

	std::uint32_t sequence = 1;

	for (std::uint16_t level = 0; level < 5; ++level) {
		const auto result = send_to_engine(engine, make_market_message(sequence++, depthwire::wire::Side::sell,
									       level, 100100 + level, 10));

		assert(result.status == depthwire::EngineStatus::no_order);
	}

	for (std::uint16_t level = 0; level < 5; ++level) {
		const auto result = send_to_engine(engine, make_market_message(sequence++, depthwire::wire::Side::buy,
									       level, 100000 - level, 10));

		assert(result.status == depthwire::EngineStatus::no_order);
	}

	const auto trigger_result =
		send_to_engine(engine, make_market_message(sequence++, depthwire::wire::Side::buy, 0, 100000, 1000));

	assert(trigger_result.status == depthwire::EngineStatus::order_emitted);
	assert(trigger_result.signal == depthwire::Signal::buy);
	assert(trigger_result.imbalance_bps >= 6000);

	depthwire::OrderRequest emitted_order{};
	assert(engine.pop_order(emitted_order));

	static_assert(sizeof(depthwire::OrderRequest) == 32);

	assert(emitted_order.client_order_id == 1);
	assert(emitted_order.instrument_id == 77);
	assert(emitted_order.price_ticks == 100100);
	assert(emitted_order.quantity == 10);
	assert(emitted_order.side == depthwire::Side::buy);
	assert(emitted_order.order_kind == depthwire::OrderKind::limit);

	const auto wire_order_result = depthwire::pipeline::EngineWireAdapter::to_wire_order(emitted_order, 5001);

	assert(wire_order_result);

	const auto &wire_order = *wire_order_result;

	depthwire::wire::WireMessage encoded{};
	assert(depthwire::wire::encode_order(wire_order, encoded));

	const auto decoded_result = depthwire::wire::decode_order(std::span<const std::byte>(encoded));

	assert(decoded_result);

	const auto &decoded = *decoded_result;

	assert(decoded.sequence_number == 5001);
	assert(decoded.client_order_id == emitted_order.client_order_id);
	assert(decoded.instrument_id == emitted_order.instrument_id);
	assert(decoded.price_ticks == emitted_order.price_ticks);
	assert(decoded.quantity == emitted_order.quantity);
	assert(decoded.side == depthwire::wire::Side::buy);
	assert(decoded.source_sequence == emitted_order.source_sequence);

	{
		auto invalid_market = make_market_message(1, depthwire::wire::Side::buy, 0, 100, 10);

		invalid_market.wire_flags = 1;
		{
			const auto r = depthwire::pipeline::EngineWireAdapter::to_market_update(invalid_market);
			assert(!r &&
			       r.error() ==
				       depthwire::pipeline::EngineWireAdapterStatus::unsupported_market_header_flags);
		}

		invalid_market.wire_flags = 0;
		invalid_market.source_flags = 1;
		{
			const auto r = depthwire::pipeline::EngineWireAdapter::to_market_update(invalid_market);
			assert(!r &&
			       r.error() ==
				       depthwire::pipeline::EngineWireAdapterStatus::unsupported_market_source_flags);
		}

		invalid_market.source_flags = 0;
		invalid_market.side = depthwire::wire::Side::invalid;
		{
			const auto r = depthwire::pipeline::EngineWireAdapter::to_market_update(invalid_market);
			assert(!r && r.error() == depthwire::pipeline::EngineWireAdapterStatus::invalid_market_side);
		}

		invalid_market.side = depthwire::wire::Side::buy;
		invalid_market.action = depthwire::wire::UpdateAction::invalid;
		{
			const auto r = depthwire::pipeline::EngineWireAdapter::to_market_update(invalid_market);
			assert(!r && r.error() == depthwire::pipeline::EngineWireAdapterStatus::invalid_market_action);
		}
	}

	{
		auto invalid_order = emitted_order;
		invalid_order.flags = 1;

		const auto r = depthwire::pipeline::EngineWireAdapter::to_wire_order(invalid_order, 5002);
		assert(!r && r.error() == depthwire::pipeline::EngineWireAdapterStatus::unsupported_order_flags);
	}

	return 0;
}
