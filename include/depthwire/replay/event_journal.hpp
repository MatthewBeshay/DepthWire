#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "depthwire/engine/trading_engine.hpp"
#include "depthwire/pipeline/packet_processor.hpp"
#include "depthwire/wire/ethernet_frame.hpp"

namespace depthwire::replay {

struct JournalEntry {
	std::uint32_t input_sequence{};
	pipeline::PacketProcessStatus process_status{pipeline::PacketProcessStatus::no_order};
	EngineStatus engine_status{EngineStatus::no_order};
	bool has_frame{};
	wire::EthernetFrame frame{};
};

// Preallocated per-event outcome storage, filled during a run and hashed
// afterwards, so no digest work happens on the paced hot path (the
// methodology's retained-samples rule). Capacity comes from the validated
// trace header's record count, which bounds memory to
// capacity * sizeof(JournalEntry) before the run starts.
class EventJournal {
public:
	explicit EventJournal(const std::size_t capacity) : capacity_(capacity) { entries_.reserve(capacity); }

	// False when full: the run must stop and the result is not evidence.
	[[nodiscard]] bool record(const JournalEntry &entry) noexcept
	{
		if (entries_.size() >= capacity_) {
			return false;
		}

		entries_.push_back(entry);
		return true;
	}

	[[nodiscard]] std::span<const JournalEntry> entries() const noexcept { return entries_; }

	[[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

	[[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

	void clear() noexcept { entries_.clear(); }

private:
	std::vector<JournalEntry> entries_{};
	std::size_t capacity_{};
};

} // namespace depthwire::replay
