#include <cassert>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "depthwire/engine/trading_engine.hpp"

namespace {

depthwire::EngineConfig make_config() noexcept
{
	depthwire::EngineConfig config{};
	config.instrument_id = 77;
	config.strategy.imbalance_threshold_bps = 6000;
	config.strategy.order_quantity = 10;
	config.risk.max_order_quantity = 100;
	config.risk.max_absolute_position = 1000;
	config.risk.max_notional_ticks = 1'000'000'000ULL;
	config.risk.max_outstanding_orders = 64;
	config.sequence_policy = depthwire::SequencePolicy::require_contiguous;
	return config;
}

depthwire::MarketUpdate make_update(const depthwire::SequenceNumber sequence,
				    const depthwire::Side side = depthwire::Side::buy, const std::uint16_t level = 0,
				    const depthwire::PriceTicks price = 100, const depthwire::Quantity quantity = 10,
				    const depthwire::UpdateAction action = depthwire::UpdateAction::modify) noexcept
{
	depthwire::MarketUpdate update{};
	update.sequence_number = sequence;
	update.instrument_id = 77;
	update.price_ticks = price;
	update.quantity = quantity;
	update.side = side;
	update.action = action;
	update.level = level;
	return update;
}

template<std::size_t Capacity>
void seed_balanced_book(depthwire::TradingEngine<Capacity> &engine, depthwire::SequenceNumber &sequence)
{
	for (std::uint16_t level = 0; level < depthwire::kVisibleDepth; ++level) {
		const auto result =
			engine.on_market_update(make_update(sequence++, depthwire::Side::sell, level, 101 + level, 10));
		assert(result.status == depthwire::EngineStatus::no_order);
	}

	for (std::uint16_t level = 0; level < depthwire::kVisibleDepth; ++level) {
		const auto result =
			engine.on_market_update(make_update(sequence++, depthwire::Side::buy, level, 100 - level, 10));
		assert(result.status == depthwire::EngineStatus::no_order);
	}
}

} // namespace

int main()
{
	static_assert(!std::is_copy_constructible_v<depthwire::TradingEngine<1>>);
	static_assert(!std::is_copy_assignable_v<depthwire::TradingEngine<1>>);
	static_assert(!std::is_move_constructible_v<depthwire::TradingEngine<1>>);

	{
		depthwire::TradingEngine<4> engine(make_config());

		assert(engine.on_market_update(make_update(100)).status == depthwire::EngineStatus::no_order);
		assert(engine.last_sequence_number() == 100);

		assert(engine.on_market_update(make_update(100)).status == depthwire::EngineStatus::duplicate_update);
		assert(engine.last_sequence_number() == 100);

		assert(engine.on_market_update(make_update(102)).status == depthwire::EngineStatus::sequence_gap);
		assert(engine.last_sequence_number() == 100);

		assert(engine.on_market_update(make_update(99)).status == depthwire::EngineStatus::stale_update);
		assert(engine.last_sequence_number() == 100);

		assert(engine.on_market_update(make_update(101)).status == depthwire::EngineStatus::no_order);
		assert(engine.last_sequence_number() == 101);
	}

	{
		depthwire::TradingEngine<4> engine(make_config());
		const auto maximum = std::numeric_limits<depthwire::SequenceNumber>::max();

		assert(engine.on_market_update(make_update(maximum)).status == depthwire::EngineStatus::no_order);
		assert(engine.on_market_update(make_update(0)).status == depthwire::EngineStatus::no_order);
		assert(engine.last_sequence_number() == 0);
	}

	{
		auto config = make_config();
		config.sequence_policy = depthwire::SequencePolicy::accept_any;
		depthwire::TradingEngine<4> engine(config);

		assert(engine.on_market_update(make_update(7)).status == depthwire::EngineStatus::no_order);
		assert(engine.on_market_update(make_update(7, depthwire::Side::buy, 0, 100, 20)).status ==
		       depthwire::EngineStatus::no_order);
		assert(engine.book().total_bid_quantity() == 20);
	}

	{
		depthwire::TradingEngine<4> engine(make_config());
		assert(engine.on_market_update(
				     make_update(1, depthwire::Side::buy, 0, 100, 10, depthwire::UpdateAction::add))
			       .status == depthwire::EngineStatus::no_order);

		const auto rejected = engine.on_market_update(
			make_update(2, depthwire::Side::buy, 0, 101, 10, depthwire::UpdateAction::add));
		assert(rejected.status == depthwire::EngineStatus::invalid_update);
		assert(rejected.book_status == depthwire::BookApplyStatus::level_already_active);
		assert(engine.last_sequence_number() == 1);
		assert(engine.book().best_bid() == 100);

		assert(engine.on_market_update(
				     make_update(2, depthwire::Side::buy, 0, 101, 10, depthwire::UpdateAction::modify))
			       .status == depthwire::EngineStatus::no_order);
		assert(engine.last_sequence_number() == 2);
		assert(engine.book().best_bid() == 101);
	}

	{
		depthwire::TradingEngine<1> engine(make_config());
		depthwire::SequenceNumber sequence = 1;
		seed_balanced_book(engine, sequence);

		const auto emitted =
			engine.on_market_update(make_update(sequence++, depthwire::Side::buy, 0, 100, 1000));
		assert(emitted.status == depthwire::EngineStatus::order_emitted);
		assert(engine.last_sequence_number() == 11);

		const auto bid_quantity = engine.book().total_bid_quantity();
		const auto ask_quantity = engine.book().total_ask_quantity();

		const auto backpressured =
			engine.on_market_update(make_update(sequence, depthwire::Side::sell, 0, 101, 1000));
		assert(backpressured.status == depthwire::EngineStatus::outbox_full);
		assert(backpressured.book_status == depthwire::BookApplyStatus::not_attempted);
		assert(engine.last_sequence_number() == 11);
		assert(engine.book().total_bid_quantity() == bid_quantity);
		assert(engine.book().total_ask_quantity() == ask_quantity);

		depthwire::OrderRequest order{};
		assert(engine.pop_order(order));

		const auto retried =
			engine.on_market_update(make_update(sequence, depthwire::Side::sell, 0, 101, 1000));
		assert(retried.status == depthwire::EngineStatus::no_order);
		assert(engine.last_sequence_number() == 12);
		assert(engine.book().total_ask_quantity() == 1040);
	}

	{
		depthwire::TradingEngine<4> engine(make_config());
		depthwire::SequenceNumber sequence = 1;
		seed_balanced_book(engine, sequence);

		engine.set_kill_switch(true);
		assert(engine.kill_switch_enabled());

		const auto rejected =
			engine.on_market_update(make_update(sequence, depthwire::Side::buy, 0, 100, 1000));
		assert(rejected.status == depthwire::EngineStatus::risk_rejected);
		assert(rejected.risk_status == depthwire::RiskCheckStatus::kill_switch_enabled);

		engine.set_kill_switch(false);
		assert(!engine.kill_switch_enabled());
	}

	{
		auto config = make_config();
		config.strategy.imbalance_threshold_bps = 0;
		depthwire::TradingEngine<4> engine(config);
		assert(!engine.configuration_valid());
		assert(engine.on_market_update(make_update(1)).status ==
		       depthwire::EngineStatus::invalid_configuration);
	}

	return 0;
}
