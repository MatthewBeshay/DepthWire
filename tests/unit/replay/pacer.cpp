#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "depthwire/replay/pacer.hpp"

// The pacer never reads a clock, so every behaviour is tested with synthetic
// elapsed times through the one production code path.

namespace {

using namespace depthwire::replay;
using std::chrono::nanoseconds;

void test_build_schedule_recorded_spacing()
{
	const std::array<std::uint64_t, 4> deltas{1000, 1000, 0, 500};
	const std::array<PaceSegment, 1> segments{PaceSegment{0, kRecordedSpacingPpm}};
	std::array<std::uint64_t, 4> offsets{};

	assert(build_schedule(deltas, segments, offsets));

	assert(offsets[0] == 1000);
	assert(offsets[1] == 2000);
	assert(offsets[2] == 2000);
	assert(offsets[3] == 2500);
}

void test_build_schedule_scaling_and_segments()
{
	const std::array<std::uint64_t, 6> deltas{1000, 1000, 1000, 1000, 1000, 1000};

	// Half spacing (double rate) from record 0; recorded spacing from record
	// 2; 2x spacing from record 4.
	const std::array<PaceSegment, 3> segments{
		PaceSegment{0, 500'000},
		PaceSegment{2, 1'000'000},
		PaceSegment{4, 2'000'000},
	};
	std::array<std::uint64_t, 6> offsets{};

	assert(build_schedule(deltas, segments, offsets));

	assert(offsets[0] == 500);
	assert(offsets[1] == 1000);
	assert(offsets[2] == 2000);
	assert(offsets[3] == 3000);
	assert(offsets[4] == 5000);
	assert(offsets[5] == 7000);
}

void test_build_schedule_integer_truncation()
{
	// 999 * 333333 / 1e6 = 332.999667 -> truncates to 332: the math is
	// integer-only and therefore machine-independent.
	const std::array<std::uint64_t, 1> deltas{999};
	const std::array<PaceSegment, 1> segments{PaceSegment{0, 333'333}};
	std::array<std::uint64_t, 1> offsets{};

	assert(build_schedule(deltas, segments, offsets));
	assert(offsets[0] == 332);
}

void test_build_schedule_overflow_detected()
{
	// Product overflow: delta * ppm exceeds 64 bits.
	{
		const std::array<std::uint64_t, 1> deltas{std::numeric_limits<std::uint64_t>::max() / 2};
		const std::array<PaceSegment, 1> segments{PaceSegment{0, 3}};
		std::array<std::uint64_t, 1> offsets{};
		assert(!build_schedule(deltas, segments, offsets));
	}

	// Cumulative overflow: the running due offset exceeds kMaxScheduleNs.
	{
		const std::array<std::uint64_t, 2> deltas{6'000'000'000'000'000'000ULL, 6'000'000'000'000'000'000ULL};
		const std::array<PaceSegment, 1> segments{PaceSegment{0, kRecordedSpacingPpm}};
		std::array<std::uint64_t, 2> offsets{};
		assert(!build_schedule(deltas, segments, offsets));
	}

	// A large but representable schedule is still accepted.
	{
		const std::array<std::uint64_t, 1> deltas{10'000'000'000'000ULL};
		const std::array<PaceSegment, 1> segments{PaceSegment{0, kRecordedSpacingPpm}};
		std::array<std::uint64_t, 1> offsets{};
		assert(build_schedule(deltas, segments, offsets));
		assert(offsets[0] == 10'000'000'000'000ULL);
	}
}

void test_due_now_batching_and_catch_up()
{
	const std::array<std::uint64_t, 5> offsets{1000, 2000, 3000, 4000, 5000};
	MonotonicPacer pacer{offsets, nanoseconds{100}};

	// Nothing due before the first offset.
	assert(pacer.due_now(nanoseconds{999}, 32).count == 0);
	assert(pacer.remaining() == 5);

	// Exactly at the due time the event is due.
	const auto first = pacer.due_now(nanoseconds{1000}, 32);
	assert(first.first == 0);
	assert(first.count == 1);

	// A slow consumer does not reduce offered load: after a long stall every
	// overdue event is returned, in order, capped by max_batch with the
	// remainder still due on the next call.
	const auto capped = pacer.due_now(nanoseconds{100'000}, 2);
	assert(capped.first == 1);
	assert(capped.count == 2);

	const auto remainder = pacer.due_now(nanoseconds{100'000}, 32);
	assert(remainder.first == 3);
	assert(remainder.count == 2);

	// Exhausted: further calls return nothing.
	assert(pacer.due_now(nanoseconds{200'000}, 32).count == 0);
	assert(pacer.remaining() == 0);
	assert(pacer.issued() == 5);
	assert(pacer.schedule_end_ns() == 5000);
}

void test_lateness_accounting()
{
	const std::array<std::uint64_t, 2> offsets{1000, 2000};
	MonotonicPacer pacer{offsets, nanoseconds{500}};

	// On time: issued exactly at the due time.
	assert(!pacer.is_late(0, nanoseconds{1000}));
	assert(pacer.lateness_ns(0, nanoseconds{1000}) == 0);

	// Late but within the declared bound.
	assert(!pacer.is_late(0, nanoseconds{1500}));
	assert(pacer.lateness_ns(0, nanoseconds{1500}) == 500);

	// Past the bound: still injected, but counted late.
	assert(pacer.is_late(0, nanoseconds{1501}));
	assert(pacer.lateness_ns(0, nanoseconds{1501}) == 501);

	assert(!pacer.is_late(1, nanoseconds{2500}));
	assert(pacer.is_late(1, nanoseconds{2501}));
}

void test_zero_delta_burst()
{
	// Zero deltas model a recorded burst: all due at the same instant.
	const std::array<std::uint64_t, 3> offsets{1000, 1000, 1000};
	MonotonicPacer pacer{offsets, nanoseconds{0}};

	const auto batch = pacer.due_now(nanoseconds{1000}, 32);
	assert(batch.first == 0);
	assert(batch.count == 3);
}

void test_large_schedule_progression()
{
	std::vector<std::uint64_t> deltas(10'000, 100);
	const std::array<PaceSegment, 1> segments{PaceSegment{0, kRecordedSpacingPpm}};
	std::vector<std::uint64_t> offsets(deltas.size());

	assert(build_schedule(deltas, segments, offsets));
	assert(offsets.back() == 1'000'000);

	MonotonicPacer pacer{offsets, nanoseconds{50'000}};
	std::size_t issued_total = 0;

	// Advance in 10 us steps; each step should release exactly 100 events.
	for (std::uint64_t elapsed_ns = 10'000; elapsed_ns <= 1'000'000; elapsed_ns += 10'000) {
		std::size_t step_total = 0;

		for (;;) {
			const auto batch = pacer.due_now(nanoseconds{static_cast<std::int64_t>(elapsed_ns)}, 32);

			if (batch.count == 0) {
				break;
			}

			assert(batch.first == issued_total + step_total);
			step_total += batch.count;
		}

		assert(step_total == 100);
		issued_total += step_total;
	}

	assert(issued_total == 10'000);
	assert(pacer.remaining() == 0);
}

} // namespace

int main()
{
	test_build_schedule_recorded_spacing();
	test_build_schedule_scaling_and_segments();
	test_build_schedule_integer_truncation();
	test_build_schedule_overflow_detected();
	test_due_now_batching_and_catch_up();
	test_zero_delta_burst();
	test_lateness_accounting();
	test_large_schedule_progression();
	return 0;
}
