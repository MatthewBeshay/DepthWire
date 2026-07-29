#include <cassert>
#include <cstdint>

#include "depthwire/engine/trading_engine.hpp"
#include "depthwire/pipeline/engine_wire_adapter.hpp"
#include "depthwire/wire/market_data_packet.hpp"

namespace {

depthwire::EngineConfig make_config(const depthwire::Quantity max_order_quantity = 100) noexcept
{
	depthwire::EngineConfig config{};
	config.instrument_id = 77;
	config.strategy.imbalance_threshold_bps = 6000;
	config.strategy.order_quantity = 10;
	config.risk.max_order_quantity = max_order_quantity;
	config.risk.max_absolute_position = 1000;
	config.risk.max_notional_ticks = 1'000'000'000ULL;
	config.risk.max_outstanding_orders = 64;
	return config;
}

depthwire::wire::MarketDataMessage make_message(const std::uint32_t sequence_number, const depthwire::wire::Side side,
						const std::uint16_t level, const std::int64_t price_ticks,
						const std::uint32_t quantity,
						const std::uint32_t instrument_id = 77) noexcept
{
	depthwire::wire::MarketDataMessage message{};
	message.sequence_number = sequence_number;
	message.exchange_timestamp_ns = static_cast<std::uint64_t>(sequence_number) * 1000ULL;
	message.instrument_id = instrument_id;
	message.price_ticks = price_ticks;
	message.quantity = quantity;
	message.side = side;
	message.action = depthwire::wire::UpdateAction::modify;
	message.level = level;
	return message;
}

depthwire::EngineResult apply_message(depthwire::TradingEngine<64> &engine,
				      const depthwire::wire::MarketDataMessage &message) noexcept
{
	const auto update_result = depthwire::pipeline::EngineWireAdapter::to_market_update(message);
	assert(update_result);
	const auto &update = *update_result;

	return engine.on_market_update(update);
}

void seed_balanced_book(depthwire::TradingEngine<64> &engine, std::uint32_t &sequence_number) noexcept
{
	for (std::uint16_t level = 0; level < 5; ++level) {
		const auto result = apply_message(engine, make_message(sequence_number++, depthwire::wire::Side::sell,
								       level, 100100 + level, 10));

		assert(result.status == depthwire::EngineStatus::no_order);
	}

	for (std::uint16_t level = 0; level < 5; ++level) {
		const auto result = apply_message(engine, make_message(sequence_number++, depthwire::wire::Side::buy,
								       level, 100000 - level, 10));

		assert(result.status == depthwire::EngineStatus::no_order);
	}
}

} // namespace

int main()
{
	static_assert(sizeof(depthwire::OrderRequest) == 32);

	{
		depthwire::TradingEngine<64> engine(make_config());
		std::uint32_t sequence_number = 1;

		seed_balanced_book(engine, sequence_number);

		depthwire::OrderRequest order{};
		assert(!engine.pop_order(order));
	}

	{
		depthwire::TradingEngine<64> engine(make_config());
		std::uint32_t sequence_number = 100;

		seed_balanced_book(engine, sequence_number);

		const auto result = apply_message(engine, make_message(sequence_number++, depthwire::wire::Side::buy, 0,
								       100000, 1000));

		assert(result.status == depthwire::EngineStatus::order_emitted);
		assert(result.signal == depthwire::Signal::buy);

		depthwire::OrderRequest order{};
		assert(engine.pop_order(order));
		assert(order.side == depthwire::Side::buy);
		assert(order.price_ticks == 100100);
		assert(order.quantity == 10);
	}

	{
		depthwire::TradingEngine<64> engine(make_config());
		std::uint32_t sequence_number = 200;

		seed_balanced_book(engine, sequence_number);

		const auto result = apply_message(engine, make_message(sequence_number++, depthwire::wire::Side::sell,
								       0, 100100, 1000));

		assert(result.status == depthwire::EngineStatus::order_emitted);
		assert(result.signal == depthwire::Signal::sell);

		depthwire::OrderRequest order{};
		assert(engine.pop_order(order));
		assert(order.side == depthwire::Side::sell);
		assert(order.price_ticks == 100000);
		assert(order.quantity == 10);
	}

	{
		depthwire::TradingEngine<64> engine(make_config(5));
		std::uint32_t sequence_number = 300;

		seed_balanced_book(engine, sequence_number);

		const auto result = apply_message(engine, make_message(sequence_number++, depthwire::wire::Side::buy, 0,
								       100000, 1000));

		assert(result.status == depthwire::EngineStatus::risk_rejected);

		depthwire::OrderRequest order{};
		assert(!engine.pop_order(order));
	}

	{
		depthwire::TradingEngine<64> engine(make_config());

		const auto result =
			apply_message(engine, make_message(400, depthwire::wire::Side::buy, 0, 100000, 10, 99));

		assert(result.status == depthwire::EngineStatus::invalid_update);
	}

	return 0;
}
