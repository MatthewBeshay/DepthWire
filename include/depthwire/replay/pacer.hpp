#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "depthwire/contracts/contracts.hpp"

namespace depthwire::replay {

// Independent monotonic-time pacing over a precomputed schedule (see the
// replay section of docs/benchmarks/methodology.md).
//
// The pacer never reads a clock: the harness samples steady_clock and
// passes elapsed time since its run epoch. That keeps the offered load
// independent of consumer progress (due_now() returns every not-yet-issued
// event whose due time has passed, whether or not the consumer kept up) and
// makes the pacer fully testable with synthetic time. One code path serves
// production and tests.
//
// due_offsets_ns is the cumulative due time of each event relative to the run
// epoch, built once at load time (see build_schedule) and borrowed for the
// pacer's lifetime. All hot-path operations are allocation-free.
class MonotonicPacer {
public:
	struct DueBatch {
		std::size_t first{};
		std::size_t count{};
	};

	MonotonicPacer(const std::span<const std::uint64_t> due_offsets_ns,
		       const std::chrono::nanoseconds lateness_bound) noexcept
		: due_offsets_ns_(due_offsets_ns),
		  lateness_bound_ns_(static_cast<std::uint64_t>(lateness_bound.count()))
	{
		DW_CONTRACT_ASSERT(lateness_bound.count() >= 0);
	}

	// Every event with due_offset <= elapsed that has not been issued yet, in
	// schedule order, capped at max_batch per call (the remainder stays due
	// and is returned by the next call). elapsed must be non-decreasing
	// across calls.
	[[nodiscard]] DueBatch due_now(const std::chrono::nanoseconds elapsed, const std::size_t max_batch) noexcept
	{
		DW_CONTRACT_ASSERT(elapsed.count() >= 0);

		const auto elapsed_ns = static_cast<std::uint64_t>(elapsed.count());
		DueBatch batch{next_index_, 0};

		while (batch.count < max_batch && next_index_ < due_offsets_ns_.size() &&
		       due_offsets_ns_[next_index_] <= elapsed_ns) {
			++next_index_;
			++batch.count;
		}

		return batch;
	}

	// True when the event was issued later than its due time by more than the
	// declared lateness bound. Late events are still injected; the harness
	// records the lateness distribution.
	[[nodiscard]] bool is_late(const std::size_t index, const std::chrono::nanoseconds elapsed) const noexcept
	{
		DW_CONTRACT_ASSERT(index < due_offsets_ns_.size());
		DW_CONTRACT_ASSERT(elapsed.count() >= 0);

		const auto elapsed_ns = static_cast<std::uint64_t>(elapsed.count());
		return elapsed_ns > due_offsets_ns_[index] && elapsed_ns - due_offsets_ns_[index] > lateness_bound_ns_;
	}

	// Nanoseconds past the event's due time at issue (zero when on time).
	[[nodiscard]] std::uint64_t lateness_ns(const std::size_t index,
						const std::chrono::nanoseconds elapsed) const noexcept
	{
		DW_CONTRACT_ASSERT(index < due_offsets_ns_.size());

		const auto elapsed_ns = static_cast<std::uint64_t>(elapsed.count());
		const std::uint64_t due_ns = due_offsets_ns_[index];
		return elapsed_ns > due_ns ? elapsed_ns - due_ns : 0;
	}

	[[nodiscard]] std::size_t issued() const noexcept { return next_index_; }

	[[nodiscard]] std::size_t remaining() const noexcept { return due_offsets_ns_.size() - next_index_; }

	[[nodiscard]] std::uint64_t schedule_end_ns() const noexcept
	{
		return due_offsets_ns_.empty() ? 0 : due_offsets_ns_[due_offsets_ns_.size() - 1];
	}

private:
	std::span<const std::uint64_t> due_offsets_ns_{};
	std::uint64_t lateness_bound_ns_{};
	std::size_t next_index_{};
};

// One declared pacing segment: from record index `first_record` onward, each
// recorded arrival delta is scaled by scale_ppm / 1'000'000. 1'000'000 ppm
// replays the recorded spacing; smaller values compress it (higher offered
// rate), larger values stretch it.
struct PaceSegment {
	std::uint32_t first_record{};
	std::uint32_t scale_ppm{};
};

inline constexpr std::uint32_t kRecordedSpacingPpm = 1'000'000;

// Schedules are compared against a signed-nanosecond monotonic clock, so no
// due offset may exceed what int64 nanoseconds can represent (~292 years).
inline constexpr std::uint64_t kMaxScheduleNs = 9'223'372'036'854'775'807ULL;

// Builds cumulative due offsets from recorded deltas and declared segments.
// Integer-only math (delta * ppm / 1e6 truncated), so a schedule is
// bit-identical on every machine. Returns false when extreme delta or scale
// values would overflow the arithmetic or push an offset past kMaxScheduleNs;
// a false schedule must not be run.
//
// Preconditions: output.size() == deltas.size(); segments non-empty, sorted
// by ascending first_record, segments[0].first_record == 0, every scale_ppm
// nonzero.
[[nodiscard]] inline bool build_schedule(const std::span<const std::uint64_t> deltas,
					 const std::span<const PaceSegment> segments,
					 const std::span<std::uint64_t> output) noexcept
{
	DW_CONTRACT_ASSERT(output.size() == deltas.size());
	DW_CONTRACT_ASSERT(!segments.empty());
	DW_CONTRACT_ASSERT(segments[0].first_record == 0);

	std::size_t segment_index = 0;
	std::uint64_t due_ns = 0;

	for (std::size_t index = 0; index < deltas.size(); ++index) {
		while (segment_index + 1 < segments.size() && index >= segments[segment_index + 1].first_record) {
			++segment_index;
		}

		const std::uint64_t scale_ppm = segments[segment_index].scale_ppm;
		DW_CONTRACT_ASSERT(scale_ppm != 0);

		const std::uint64_t delta = deltas[index];

		if (delta != 0 && scale_ppm > std::numeric_limits<std::uint64_t>::max() / delta) {
			return false;
		}

		const std::uint64_t scaled = delta * scale_ppm / 1'000'000U;

		if (scaled > kMaxScheduleNs - due_ns) {
			return false;
		}

		due_ns += scaled;
		output[index] = due_ns;
	}

	return true;
}

} // namespace depthwire::replay
