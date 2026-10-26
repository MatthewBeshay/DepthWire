#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "depthwire/contracts/contracts.hpp"
#include "depthwire/ha/codec.hpp"

namespace depthwire::ha {

enum class OfferStatus : std::uint8_t {
	accepted = 0,
	duplicate,
	overflow,
};

struct GapRange {
	std::uint32_t first_index{};
	std::uint32_t count{};
};

// Restores transport order in front of the engine. Records are offered in
// arrival order (live datagrams and gap-fill responses alike) and popped
// strictly in index order, so the engine sees exactly the sequence a lossless
// transport would have delivered. Indices are trace record indices, bounded by
// trace::kMaxTraceRecords, so they never wrap. A record further ahead than the
// buffer can hold fails the tracker closed: the caller must stop rather than
// trade past a hole.
template<std::size_t Capacity = 4096> class GapTracker {
public:
	static_assert(Capacity > 0);

	explicit GapTracker(const std::uint32_t first_expected = 0) noexcept : expected_(first_expected) {}

	[[nodiscard]] OfferStatus offer(const FeedRecord &record) noexcept
	{
		if (failed_) {
			return OfferStatus::overflow;
		}

		if (record.index < expected_) {
			++duplicates_;
			return OfferStatus::duplicate;
		}

		const std::uint64_t distance = std::uint64_t{record.index} - expected_;

		if (distance >= Capacity) {
			failed_ = true;
			return OfferStatus::overflow;
		}

		Slot &slot = slots_[slot_index(distance)];

		if (slot.occupied) {
			++duplicates_;
			return OfferStatus::duplicate;
		}

		slot.occupied = true;
		slot.message = record.message;
		++buffered_;

		if (!has_highest_ || record.index > highest_) {
			highest_ = record.index;
			has_highest_ = true;
		}

		return OfferStatus::accepted;
	}

	[[nodiscard]] bool pop(FeedRecord &output) noexcept
	{
		Slot &slot = slots_[head_];

		if (!slot.occupied) {
			return false;
		}

		output.index = expected_;
		output.message = slot.message;
		slot.occupied = false;
		head_ = (head_ + 1) % Capacity;
		++expected_;
		--buffered_;
		DW_CONTRACT_ASSERT(buffered_ <= Capacity);
		return true;
	}

	// The hole between the next expected index and the first buffered record
	// behind it, if records are waiting on one.
	[[nodiscard]] std::optional<GapRange> front_gap() const noexcept
	{
		if (buffered_ == 0 || slots_[head_].occupied) {
			return std::nullopt;
		}

		std::uint32_t count = 1;

		while (count < Capacity && !slots_[slot_index(count)].occupied) {
			++count;
		}

		return GapRange{expected_, count};
	}

	[[nodiscard]] std::uint32_t expected() const noexcept { return expected_; }

	[[nodiscard]] std::size_t buffered() const noexcept { return buffered_; }

	[[nodiscard]] std::uint64_t duplicates() const noexcept { return duplicates_; }

	[[nodiscard]] bool failed() const noexcept { return failed_; }

	// The highest index ever accepted, if any.
	[[nodiscard]] std::optional<std::uint32_t> highest() const noexcept
	{
		if (!has_highest_) {
			return std::nullopt;
		}

		return highest_;
	}

private:
	struct Slot {
		bool occupied{};
		wire::WireMessage message{};
	};

	[[nodiscard]] std::size_t slot_index(const std::uint64_t distance) const noexcept
	{
		return static_cast<std::size_t>((head_ + distance) % Capacity);
	}

	std::array<Slot, Capacity> slots_{};
	std::size_t head_{};
	std::size_t buffered_{};
	std::uint32_t expected_{};
	std::uint32_t highest_{};
	bool has_highest_{};
	bool failed_{};
	std::uint64_t duplicates_{};
};

} // namespace depthwire::ha
