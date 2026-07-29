#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

#include "depthwire/digest/sha256.hpp"
#include "depthwire/engine/trading_engine.hpp"
#include "depthwire/pipeline/packet_processor.hpp"
#include "depthwire/replay/event_journal.hpp"
#include "depthwire/replay/replay_counters.hpp"

namespace depthwire::replay {

// Canonical run-digest stream, version 1; the normative layout and the
// determinism claim (same accepted input sequence + same configuration =>
// same digest) live in docs/architecture/trace-format.md. The stream is
// emitted explicitly byte-by-byte in big-endian order, never from struct
// memory, so the digest is identical across compilers and machines.

inline constexpr std::uint8_t kDigestStreamVersion = 1;

// The numeric values of both status enums are part of the digest contract.
// Both enums are append-only; reordering is a compile error.
static_assert(std::to_underlying(pipeline::PacketProcessStatus::no_order) == 0);
static_assert(std::to_underlying(pipeline::PacketProcessStatus::order_encoded) == 1);
static_assert(std::to_underlying(pipeline::PacketProcessStatus::invalid_configuration) == 2);
static_assert(std::to_underlying(pipeline::PacketProcessStatus::decode_rejected) == 3);
static_assert(std::to_underlying(pipeline::PacketProcessStatus::invalid_input_destination) == 4);
static_assert(std::to_underlying(pipeline::PacketProcessStatus::invalid_input_source) == 5);
static_assert(std::to_underlying(pipeline::PacketProcessStatus::input_adapter_rejected) == 6);
static_assert(std::to_underlying(pipeline::PacketProcessStatus::engine_rejected) == 7);
static_assert(std::to_underlying(pipeline::PacketProcessStatus::output_queue_invariant) == 8);
static_assert(std::to_underlying(pipeline::PacketProcessStatus::output_adapter_rejected) == 9);
static_assert(std::to_underlying(pipeline::PacketProcessStatus::output_encode_rejected) == 10);

static_assert(std::to_underlying(EngineStatus::no_order) == 0);
static_assert(std::to_underlying(EngineStatus::order_emitted) == 1);
static_assert(std::to_underlying(EngineStatus::invalid_update) == 2);
static_assert(std::to_underlying(EngineStatus::risk_rejected) == 3);
static_assert(std::to_underlying(EngineStatus::outbox_full) == 4);
static_assert(std::to_underlying(EngineStatus::invalid_configuration) == 5);
static_assert(std::to_underlying(EngineStatus::duplicate_update) == 6);
static_assert(std::to_underlying(EngineStatus::sequence_gap) == 7);
static_assert(std::to_underlying(EngineStatus::stale_update) == 8);

namespace digest_detail {

inline void append_u8(digest::Sha256 &hasher, const std::uint8_t value) noexcept
{
	const std::array<std::byte, 1> bytes{static_cast<std::byte>(value)};
	hasher.update(bytes);
}

inline void append_be32(digest::Sha256 &hasher, const std::uint32_t value) noexcept
{
	const std::array<std::byte, 4> bytes{
		static_cast<std::byte>(value >> 24),
		static_cast<std::byte>(value >> 16),
		static_cast<std::byte>(value >> 8),
		static_cast<std::byte>(value),
	};
	hasher.update(bytes);
}

inline void append_be64(digest::Sha256 &hasher, const std::uint64_t value) noexcept
{
	append_be32(hasher, static_cast<std::uint32_t>(value >> 32));
	append_be32(hasher, static_cast<std::uint32_t>(value));
}

} // namespace digest_detail

[[nodiscard]] inline digest::Sha256Digest compute_run_digest(const EventJournal &journal,
							     const ReplayCounters &counters) noexcept
{
	digest::Sha256 hasher{};

	const std::array<std::byte, 4> stream_magic{std::byte{'D'}, std::byte{'W'}, std::byte{'D'}, std::byte{'G'}};
	hasher.update(stream_magic);
	digest_detail::append_u8(hasher, kDigestStreamVersion);

	for (const JournalEntry &entry : journal.entries()) {
		digest_detail::append_be32(hasher, entry.input_sequence);
		digest_detail::append_u8(hasher, std::to_underlying(entry.process_status));
		digest_detail::append_u8(hasher, std::to_underlying(entry.engine_status));

		if (entry.process_status == pipeline::PacketProcessStatus::order_encoded) {
			hasher.update(entry.frame);
		}
	}

	const std::array<std::byte, 4> counters_magic{std::byte{'D'}, std::byte{'W'}, std::byte{'C'}, std::byte{'T'}};
	hasher.update(counters_magic);
	digest_detail::append_be32(hasher, kHashedCounterCount);
	digest_detail::append_be64(hasher, counters.offered);
	digest_detail::append_be64(hasher, counters.accepted);
	digest_detail::append_be64(hasher, counters.dropped);
	digest_detail::append_be64(hasher, counters.processed);
	digest_detail::append_be64(hasher, counters.ordered);
	digest_detail::append_be64(hasher, counters.status_no_order);
	digest_detail::append_be64(hasher, counters.status_order_encoded);
	digest_detail::append_be64(hasher, counters.status_decode_rejected);
	digest_detail::append_be64(hasher, counters.status_adapter_rejected);
	digest_detail::append_be64(hasher, counters.status_other);
	digest_detail::append_be64(hasher, counters.engine_duplicate);
	digest_detail::append_be64(hasher, counters.engine_gap);
	digest_detail::append_be64(hasher, counters.engine_stale);
	digest_detail::append_be64(hasher, counters.engine_invalid);
	digest_detail::append_be64(hasher, counters.engine_other);
	digest_detail::append_be64(hasher, counters.backlog_final);

	return hasher.finalise();
}

} // namespace depthwire::replay
