#pragma once

#include <cstdint>
#include <numeric>

#include "depthwire/book/fixed_l2_book.hpp"
#include "depthwire/market/types.hpp"

namespace depthwire {

// Basis-point scale for the order-book imbalance ratio (parts per 10,000).
inline constexpr std::int64_t kBasisPointScale = 10000;

struct StrategyConfig {
	std::int64_t imbalance_threshold_bps{6000};
	Quantity order_quantity{10};
};

[[nodiscard]] constexpr bool is_valid_strategy_config(const StrategyConfig &config) noexcept
{
	return config.imbalance_threshold_bps > 0 && config.imbalance_threshold_bps <= 10000 &&
	       config.order_quantity > 0;
}

struct StrategyDecision {
	Signal signal{Signal::no_signal};
	PriceTicks limit_price_ticks{};
	Quantity quantity{};
	std::int64_t imbalance_bps{};
};

class ImbalanceStrategy {
public:
	explicit ImbalanceStrategy(const StrategyConfig config = {}) noexcept : config_(config) {}

	[[nodiscard]] bool configuration_valid() const noexcept { return is_valid_strategy_config(config_); }

	[[nodiscard]] StrategyDecision evaluate(const FixedL2Book &book) const noexcept
	{
		StrategyDecision decision{};

		if (!configuration_valid() || !book.has_complete_visible_depth()) {
			return decision;
		}

		const std::uint64_t bid_quantity = book.total_bid_quantity();
		const std::uint64_t ask_quantity = book.total_ask_quantity();
		const std::uint64_t total_quantity = std::saturating_add(bid_quantity, ask_quantity);

		if (total_quantity == 0) {
			return decision;
		}

		const auto signed_bid_quantity = static_cast<std::int64_t>(bid_quantity);
		const auto signed_ask_quantity = static_cast<std::int64_t>(ask_quantity);
		const auto signed_total_quantity = static_cast<std::int64_t>(total_quantity);

		// Saturating arithmetic bounds (bid - ask) * scale against signed overflow
		// for extreme aggregate quantities without changing any in-range result.
		decision.imbalance_bps =
			std::saturating_mul(std::saturating_sub(signed_bid_quantity, signed_ask_quantity),
					    kBasisPointScale) /
			signed_total_quantity;

		if (decision.imbalance_bps >= config_.imbalance_threshold_bps) {
			const auto best_ask = book.best_ask();

			if (best_ask.has_value()) {
				decision.signal = Signal::buy;
				decision.limit_price_ticks = *best_ask;
				decision.quantity = config_.order_quantity;
			}

			return decision;
		}

		if (decision.imbalance_bps <= -config_.imbalance_threshold_bps) {
			const auto best_bid = book.best_bid();

			if (best_bid.has_value()) {
				decision.signal = Signal::sell;
				decision.limit_price_ticks = *best_bid;
				decision.quantity = config_.order_quantity;
			}
		}

		return decision;
	}

private:
	StrategyConfig config_{};
};

} // namespace depthwire
