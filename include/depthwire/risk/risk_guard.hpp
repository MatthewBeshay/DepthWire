#pragma once

#include <cstdint>
#include <numeric>

#include "depthwire/contracts/contracts.hpp"
#include "depthwire/market/types.hpp"
#include "depthwire/order/order_request.hpp"

namespace depthwire {

struct RiskLimits {
	Quantity max_order_quantity{100};
	std::int64_t max_absolute_position{10000};
	std::uint64_t max_notional_ticks{5'000'000'000ULL};
	std::uint32_t max_outstanding_orders{1024};
};

[[nodiscard]] constexpr bool is_valid_risk_limits(const RiskLimits &limits) noexcept
{
	return limits.max_order_quantity > 0 && limits.max_absolute_position > 0 && limits.max_notional_ticks > 0 &&
	       limits.max_outstanding_orders > 0;
}

enum class RiskCheckStatus : std::uint8_t {
	not_checked = 0,
	accepted,
	invalid_configuration,
	kill_switch_enabled,
	invalid_order_side,
	unsupported_order_kind,
	invalid_quantity,
	invalid_price,
	outstanding_limit,
	position_limit,
	notional_limit,
};

class RiskGuard {
public:
	explicit RiskGuard(const RiskLimits limits = {}) noexcept : limits_(limits) {}

	[[nodiscard]] bool configuration_valid() const noexcept { return is_valid_risk_limits(limits_); }

	[[nodiscard]] RiskCheckStatus validate_and_commit_checked(const OrderRequest &order) noexcept
	{
		if (!configuration_valid()) {
			return RiskCheckStatus::invalid_configuration;
		}

		if (kill_switch_) {
			return RiskCheckStatus::kill_switch_enabled;
		}

		if (!is_valid_side(order.side)) {
			return RiskCheckStatus::invalid_order_side;
		}

		if (order.order_kind != OrderKind::limit) {
			return RiskCheckStatus::unsupported_order_kind;
		}

		if (order.quantity == 0 || order.quantity > limits_.max_order_quantity) {
			return RiskCheckStatus::invalid_quantity;
		}

		if (order.price_ticks <= 0) {
			return RiskCheckStatus::invalid_price;
		}

		if (outstanding_orders_ >= limits_.max_outstanding_orders) {
			return RiskCheckStatus::outstanding_limit;
		}

		const std::int64_t signed_quantity = order.side == Side::buy
							     ? static_cast<std::int64_t>(order.quantity)
							     : -static_cast<std::int64_t>(order.quantity);

		if ((signed_quantity > 0 &&
		     position_ > static_cast<std::int64_t>(limits_.max_absolute_position - signed_quantity)) ||
		    (signed_quantity < 0 &&
		     position_ < static_cast<std::int64_t>(-limits_.max_absolute_position - signed_quantity))) {
			return RiskCheckStatus::position_limit;
		}

		const std::int64_t proposed_position = position_ + signed_quantity;

		const std::uint64_t absolute_position = absolute_value(proposed_position);

		if (absolute_position > static_cast<std::uint64_t>(limits_.max_absolute_position)) {
			return RiskCheckStatus::position_limit;
		}

		const std::uint64_t absolute_price = absolute_value(order.price_ticks);

		// notional = |position| * |price|; the saturating multiply trips the limit
		// on overflow instead of wrapping (a * p > M iff a > M / p for a, p >= 0).
		if (std::saturating_mul(absolute_position, absolute_price) > limits_.max_notional_ticks) {
			return RiskCheckStatus::notional_limit;
		}

		position_ = proposed_position;
		++outstanding_orders_;

		// Post-commit invariants: a committed order never breaches the outstanding
		// count or the absolute position limit.
		DW_CONTRACT_ASSERT(outstanding_orders_ <= limits_.max_outstanding_orders);
		DW_CONTRACT_ASSERT(absolute_value(position_) <=
				   static_cast<std::uint64_t>(limits_.max_absolute_position));

		return RiskCheckStatus::accepted;
	}

	[[nodiscard]] bool validate_and_commit(const OrderRequest &order) noexcept
	{
		return validate_and_commit_checked(order) == RiskCheckStatus::accepted;
	}

	void set_kill_switch(const bool enabled) noexcept { kill_switch_ = enabled; }

	[[nodiscard]] bool kill_switch_enabled() const noexcept { return kill_switch_; }

	[[nodiscard]] bool try_acknowledge_one() noexcept
	{
		if (outstanding_orders_ == 0) {
			return false;
		}

		--outstanding_orders_;
		return true;
	}

	void acknowledge_one() noexcept { static_cast<void>(try_acknowledge_one()); }

	[[nodiscard]] std::int64_t position() const noexcept { return position_; }

	[[nodiscard]] std::uint32_t outstanding_orders() const noexcept { return outstanding_orders_; }

private:
	[[nodiscard]] static std::uint64_t absolute_value(const std::int64_t value) noexcept
	{
		if (value >= 0) {
			return static_cast<std::uint64_t>(value);
		}

		return static_cast<std::uint64_t>(-(value + 1)) + 1U;
	}

	RiskLimits limits_{};
	std::int64_t position_{};
	std::uint32_t outstanding_orders_{};
	bool kill_switch_{};
};

} // namespace depthwire
