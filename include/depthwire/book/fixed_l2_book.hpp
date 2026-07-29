#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "depthwire/contracts/contracts.hpp"
#include "depthwire/market/types.hpp"

namespace depthwire {

inline constexpr std::size_t kVisibleDepth = 5;

struct BookLevel {
	PriceTicks price_ticks{};
	Quantity quantity{};
	bool active{};
};

enum class BookApplyStatus : std::uint8_t {
	not_attempted = 0,
	applied,
	invalid_side,
	invalid_action,
	invalid_level,
	invalid_price,
	invalid_quantity,
	level_already_active,
	level_not_active,
};

class FixedL2Book {
public:
	// Updates are level-addressed: add and erase never shift neighbouring
	// levels. Modify is deliberately an upsert so a snapshot can seed an empty
	// book. Add requires an empty level, and erase requires an active level.
	[[nodiscard]] BookApplyStatus apply_checked(const MarketUpdate &update) noexcept
	{
		if (!is_valid_side(update.side)) {
			return BookApplyStatus::invalid_side;
		}

		if (!is_valid_action(update.action)) {
			return BookApplyStatus::invalid_action;
		}

		if (update.level >= kVisibleDepth) {
			return BookApplyStatus::invalid_level;
		}

		// The bound above establishes the array-index invariant relied on below.
		DW_CONTRACT_ASSERT(update.level < kVisibleDepth);

		auto &levels = update.side == Side::buy ? bids_ : asks_;
		BookLevel &level = levels[update.level];

		if (update.action == UpdateAction::erase) {
			if (!level.active) {
				return BookApplyStatus::level_not_active;
			}

			level = {};
			return BookApplyStatus::applied;
		}

		if (update.quantity == 0) {
			return BookApplyStatus::invalid_quantity;
		}

		if (update.price_ticks <= 0) {
			return BookApplyStatus::invalid_price;
		}

		if (update.action == UpdateAction::add && level.active) {
			return BookApplyStatus::level_already_active;
		}

		level.price_ticks = update.price_ticks;
		level.quantity = update.quantity;
		level.active = true;

		return BookApplyStatus::applied;
	}

	[[nodiscard]] bool apply(const MarketUpdate &update) noexcept
	{
		return apply_checked(update) == BookApplyStatus::applied;
	}

	void clear() noexcept
	{
		bids_ = {};
		asks_ = {};
	}

	[[nodiscard]] bool has_complete_visible_depth() const noexcept
	{
		for (std::size_t index = 0; index < kVisibleDepth; ++index) {
			if (!bids_[index].active || !asks_[index].active) {
				return false;
			}
		}

		return true;
	}

	[[nodiscard]] std::uint64_t total_bid_quantity() const noexcept
	{
		std::uint64_t total{};

		for (const auto &level : bids_) {
			if (level.active) {
				total += level.quantity;
			}
		}

		return total;
	}

	[[nodiscard]] std::uint64_t total_ask_quantity() const noexcept
	{
		std::uint64_t total{};

		for (const auto &level : asks_) {
			if (level.active) {
				total += level.quantity;
			}
		}

		return total;
	}

	[[nodiscard]] std::optional<PriceTicks> best_bid() const noexcept
	{
		std::optional<PriceTicks> result{};

		for (const auto &level : bids_) {
			if (!level.active) {
				continue;
			}

			if (!result.has_value() || level.price_ticks > *result) {
				result = level.price_ticks;
			}
		}

		return result;
	}

	[[nodiscard]] std::optional<PriceTicks> best_ask() const noexcept
	{
		std::optional<PriceTicks> result{};

		for (const auto &level : asks_) {
			if (!level.active) {
				continue;
			}

			if (!result.has_value() || level.price_ticks < *result) {
				result = level.price_ticks;
			}
		}

		return result;
	}

private:
	std::array<BookLevel, kVisibleDepth> bids_{};
	std::array<BookLevel, kVisibleDepth> asks_{};
};

} // namespace depthwire
