#include <cassert>
#include <cstdint>

#include "depthwire/risk/risk_guard.hpp"

namespace {

depthwire::OrderRequest make_order(const depthwire::Side side = depthwire::Side::buy,
				   const depthwire::Quantity quantity = 5,
				   const depthwire::PriceTicks price = 10) noexcept
{
	depthwire::OrderRequest order{};
	order.client_order_id = 1;
	order.price_ticks = price;
	order.quantity = quantity;
	order.instrument_id = 77;
	order.source_sequence = 1;
	order.side = side;
	order.order_kind = depthwire::OrderKind::limit;
	return order;
}

depthwire::RiskLimits make_limits() noexcept
{
	depthwire::RiskLimits limits{};
	limits.max_order_quantity = 10;
	limits.max_absolute_position = 10;
	limits.max_notional_ticks = 1'000;
	limits.max_outstanding_orders = 2;
	return limits;
}

} // namespace

int main()
{
	using depthwire::RiskCheckStatus;

	{
		auto invalid_limits = make_limits();
		invalid_limits.max_absolute_position = -1;
		depthwire::RiskGuard guard(invalid_limits);

		assert(!guard.configuration_valid());
		assert(guard.validate_and_commit_checked(make_order()) == RiskCheckStatus::invalid_configuration);
		assert(guard.position() == 0);
		assert(guard.outstanding_orders() == 0);
	}

	{
		auto invalid_limits = make_limits();
		invalid_limits.max_order_quantity = 0;
		assert(!depthwire::is_valid_risk_limits(invalid_limits));

		invalid_limits = make_limits();
		invalid_limits.max_notional_ticks = 0;
		assert(!depthwire::is_valid_risk_limits(invalid_limits));

		invalid_limits = make_limits();
		invalid_limits.max_outstanding_orders = 0;
		assert(!depthwire::is_valid_risk_limits(invalid_limits));
	}

	{
		depthwire::RiskGuard guard(make_limits());
		auto order = make_order();

		order.side = static_cast<depthwire::Side>(99);
		assert(guard.validate_and_commit_checked(order) == RiskCheckStatus::invalid_order_side);

		order = make_order();
		order.order_kind = depthwire::OrderKind::invalid;
		assert(guard.validate_and_commit_checked(order) == RiskCheckStatus::unsupported_order_kind);

		order = make_order(depthwire::Side::buy, 0);
		assert(guard.validate_and_commit_checked(order) == RiskCheckStatus::invalid_quantity);

		order = make_order(depthwire::Side::buy, 11);
		assert(guard.validate_and_commit_checked(order) == RiskCheckStatus::invalid_quantity);

		order = make_order(depthwire::Side::buy, 5, 0);
		assert(guard.validate_and_commit_checked(order) == RiskCheckStatus::invalid_price);

		assert(guard.position() == 0);
		assert(guard.outstanding_orders() == 0);
	}

	{
		depthwire::RiskGuard guard(make_limits());
		guard.set_kill_switch(true);
		assert(guard.kill_switch_enabled());
		assert(guard.validate_and_commit_checked(make_order()) == RiskCheckStatus::kill_switch_enabled);

		guard.set_kill_switch(false);
		assert(!guard.kill_switch_enabled());
		assert(guard.validate_and_commit_checked(make_order()) == RiskCheckStatus::accepted);
		assert(guard.position() == 5);
		assert(guard.outstanding_orders() == 1);
	}

	{
		depthwire::RiskGuard guard(make_limits());
		assert(!guard.try_acknowledge_one());
		assert(guard.validate_and_commit(make_order()));
		assert(guard.validate_and_commit(make_order()));
		assert(guard.validate_and_commit_checked(make_order()) == RiskCheckStatus::outstanding_limit);

		assert(guard.try_acknowledge_one());
		assert(guard.outstanding_orders() == 1);
		guard.acknowledge_one();
		assert(guard.outstanding_orders() == 0);
		assert(!guard.try_acknowledge_one());
	}

	{
		depthwire::RiskGuard guard(make_limits());
		assert(guard.validate_and_commit_checked(make_order(depthwire::Side::buy, 10)) ==
		       RiskCheckStatus::accepted);
		assert(guard.validate_and_commit_checked(make_order(depthwire::Side::buy, 1)) ==
		       RiskCheckStatus::position_limit);
		assert(guard.position() == 10);

		assert(guard.validate_and_commit_checked(make_order(depthwire::Side::sell, 10)) ==
		       RiskCheckStatus::accepted);
		assert(guard.position() == 0);
	}

	{
		auto limits = make_limits();
		limits.max_absolute_position = 100;
		limits.max_notional_ticks = 50;
		depthwire::RiskGuard guard(limits);

		assert(guard.validate_and_commit_checked(make_order(depthwire::Side::buy, 6, 10)) ==
		       RiskCheckStatus::notional_limit);
		assert(guard.position() == 0);
		assert(guard.outstanding_orders() == 0);
	}

	return 0;
}
