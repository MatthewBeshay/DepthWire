#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "depthwire/ha/gap_tracker.hpp"

namespace {

using namespace depthwire;

ha::FeedRecord record_at(const std::uint32_t index)
{
	ha::FeedRecord record{};
	record.index = index;

	for (std::size_t byte_index = 0; byte_index < record.message.size(); ++byte_index) {
		record.message[byte_index] = static_cast<std::byte>(index + byte_index);
	}

	return record;
}

template<std::size_t Capacity> std::vector<std::uint32_t> drain(ha::GapTracker<Capacity> &tracker)
{
	std::vector<std::uint32_t> indices{};
	ha::FeedRecord output{};

	while (tracker.pop(output)) {
		assert(output.message == record_at(output.index).message);
		indices.push_back(output.index);
	}

	return indices;
}

void test_in_order_delivery()
{
	ha::GapTracker<8> tracker{};

	for (std::uint32_t index = 0; index < 20; ++index) {
		assert(tracker.offer(record_at(index)) == ha::OfferStatus::accepted);
		assert(!tracker.front_gap());
		assert(drain(tracker) == std::vector<std::uint32_t>{index});
	}

	assert(tracker.expected() == 20 && tracker.buffered() == 0 && tracker.duplicates() == 0);
}

void test_gap_is_reported_and_filled()
{
	ha::GapTracker<8> tracker{};
	assert(tracker.offer(record_at(0)) == ha::OfferStatus::accepted);
	assert(drain(tracker).size() == 1);

	assert(tracker.offer(record_at(3)) == ha::OfferStatus::accepted);
	assert(tracker.offer(record_at(5)) == ha::OfferStatus::accepted);
	assert(drain(tracker).empty());

	const auto gap = tracker.front_gap();
	assert(gap && gap->first_index == 1 && gap->count == 2);

	assert(tracker.offer(record_at(2)) == ha::OfferStatus::accepted);
	assert(tracker.front_gap()->count == 1);
	assert(tracker.offer(record_at(1)) == ha::OfferStatus::accepted);
	assert((drain(tracker) == std::vector<std::uint32_t>{1, 2, 3}));

	const auto second_gap = tracker.front_gap();
	assert(second_gap && second_gap->first_index == 4 && second_gap->count == 1);
	assert(tracker.offer(record_at(4)) == ha::OfferStatus::accepted);
	assert((drain(tracker) == std::vector<std::uint32_t>{4, 5}));
	assert(!tracker.front_gap() && tracker.buffered() == 0);
	assert(tracker.highest() == 5U);
}

void test_duplicates_are_counted_not_delivered()
{
	ha::GapTracker<8> tracker{};
	assert(tracker.offer(record_at(0)) == ha::OfferStatus::accepted);
	assert(drain(tracker).size() == 1);

	// Behind the window, and twice inside it.
	assert(tracker.offer(record_at(0)) == ha::OfferStatus::duplicate);
	assert(tracker.offer(record_at(2)) == ha::OfferStatus::accepted);
	assert(tracker.offer(record_at(2)) == ha::OfferStatus::duplicate);
	assert(tracker.duplicates() == 2);

	assert(tracker.offer(record_at(1)) == ha::OfferStatus::accepted);
	assert((drain(tracker) == std::vector<std::uint32_t>{1, 2}));
}

void test_overflow_fails_closed()
{
	ha::GapTracker<4> tracker{};
	assert(tracker.offer(record_at(3)) == ha::OfferStatus::accepted);
	assert(tracker.offer(record_at(4)) == ha::OfferStatus::overflow);
	assert(tracker.failed());

	// Once failed, nothing more is accepted, even a record that would fit.
	assert(tracker.offer(record_at(0)) == ha::OfferStatus::overflow);
}

void test_window_wraps_around_the_ring()
{
	ha::GapTracker<4> tracker{};

	for (std::uint32_t base = 0; base < 40; base += 4) {
		const std::array<std::uint32_t, 4> order{base + 3, base + 1, base + 2, base};

		for (const std::uint32_t index : order) {
			assert(tracker.offer(record_at(index)) == ha::OfferStatus::accepted);
		}

		assert((drain(tracker) == std::vector<std::uint32_t>{base, base + 1, base + 2, base + 3}));
	}
}

void test_every_permutation_of_a_window_restores_order()
{
	std::array<std::uint32_t, 6> order{0, 1, 2, 3, 4, 5};

	do {
		auto tracker = std::make_unique<ha::GapTracker<8>>(100);
		std::vector<std::uint32_t> delivered{};

		for (const std::uint32_t offset : order) {
			assert(tracker->offer(record_at(100 + offset)) == ha::OfferStatus::accepted);
			const auto popped = drain(*tracker);
			delivered.insert(delivered.end(), popped.begin(), popped.end());
		}

		assert((delivered == std::vector<std::uint32_t>{100, 101, 102, 103, 104, 105}));
		assert(tracker->expected() == 106 && !tracker->front_gap());
	} while (std::ranges::next_permutation(order).found);
}

} // namespace

int main()
{
	test_in_order_delivery();
	test_gap_is_reported_and_filled();
	test_duplicates_are_counted_not_delivered();
	test_overflow_fails_closed();
	test_window_wraps_around_the_ring();
	test_every_permutation_of_a_window_restores_order();
	return 0;
}
