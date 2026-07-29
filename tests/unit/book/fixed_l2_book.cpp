#include <cassert>
#include <cstdint>

#include "depthwire/book/fixed_l2_book.hpp"

namespace {

depthwire::MarketUpdate make_update(const depthwire::Side side, const depthwire::UpdateAction action,
				    const std::uint16_t level, const depthwire::PriceTicks price,
				    const depthwire::Quantity quantity) noexcept
{
	depthwire::MarketUpdate update{};
	update.sequence_number = 1;
	update.instrument_id = 77;
	update.price_ticks = price;
	update.quantity = quantity;
	update.side = side;
	update.action = action;
	update.level = level;
	return update;
}

} // namespace

int main()
{
	using depthwire::BookApplyStatus;

	{
		depthwire::FixedL2Book book{};

		assert(book.apply_checked(make_update(depthwire::Side::buy, depthwire::UpdateAction::modify, 0, 100,
						      10)) == BookApplyStatus::applied);
		assert(book.best_bid() == 100);

		assert(book.apply_checked(make_update(depthwire::Side::buy, depthwire::UpdateAction::add, 0, 101,
						      11)) == BookApplyStatus::level_already_active);
		assert(book.best_bid() == 100);

		assert(book.apply_checked(make_update(depthwire::Side::buy, depthwire::UpdateAction::erase, 0, 0, 0)) ==
		       BookApplyStatus::applied);
		assert(!book.best_bid().has_value());

		assert(book.apply_checked(make_update(depthwire::Side::buy, depthwire::UpdateAction::erase, 0, 0, 0)) ==
		       BookApplyStatus::level_not_active);

		assert(book.apply_checked(make_update(depthwire::Side::buy, depthwire::UpdateAction::add, 0, 99, 12)) ==
		       BookApplyStatus::applied);
		assert(book.best_bid() == 99);
	}

	{
		depthwire::FixedL2Book book{};

		assert(book.apply_checked(make_update(depthwire::Side::buy, depthwire::UpdateAction::add, 0, 100, 0)) ==
		       BookApplyStatus::invalid_quantity);
		assert(book.apply_checked(make_update(depthwire::Side::buy, depthwire::UpdateAction::modify, 0, 0,
						      10)) == BookApplyStatus::invalid_price);
		assert(book.apply_checked(make_update(depthwire::Side::buy, depthwire::UpdateAction::modify, 5, 100,
						      10)) == BookApplyStatus::invalid_level);

		auto invalid_side = make_update(depthwire::Side::buy, depthwire::UpdateAction::modify, 0, 100, 10);
		invalid_side.side = depthwire::Side::invalid;
		assert(book.apply_checked(invalid_side) == BookApplyStatus::invalid_side);

		auto invalid_action = invalid_side;
		invalid_action.side = depthwire::Side::buy;
		invalid_action.action = depthwire::UpdateAction::invalid;
		assert(book.apply_checked(invalid_action) == BookApplyStatus::invalid_action);
	}

	{
		depthwire::FixedL2Book book{};

		for (std::uint16_t level = 0; level < depthwire::kVisibleDepth; ++level) {
			assert(book.apply(make_update(depthwire::Side::buy, depthwire::UpdateAction::modify, level,
						      100 - level, 10 + level)));
			assert(book.apply(make_update(depthwire::Side::sell, depthwire::UpdateAction::modify, level,
						      101 + level, 20 + level)));
		}

		assert(book.has_complete_visible_depth());
		assert(book.best_bid() == 100);
		assert(book.best_ask() == 101);
		assert(book.total_bid_quantity() == 60);
		assert(book.total_ask_quantity() == 110);

		book.clear();
		assert(!book.has_complete_visible_depth());
		assert(book.total_bid_quantity() == 0);
		assert(book.total_ask_quantity() == 0);
		assert(!book.best_bid().has_value());
		assert(!book.best_ask().has_value());
	}

	return 0;
}
