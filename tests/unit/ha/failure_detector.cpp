#include <cassert>
#include <cstdint>

#include "depthwire/ha/failure_detector.hpp"

namespace {

using namespace depthwire;

constexpr std::uint64_t kMillisecond = 1'000'000;

void test_unarmed_detector_never_fails()
{
	const ha::FailureDetector detector{5 * kMillisecond, 4};
	assert(detector.configuration_valid());
	assert(!detector.armed());
	assert(!detector.failed(1'000'000 * kMillisecond));
}

void test_timeout_is_interval_times_missed_limit()
{
	ha::FailureDetector detector{5 * kMillisecond, 4};
	detector.on_heartbeat(100 * kMillisecond);

	assert(detector.timeout_ns() == 20 * kMillisecond);
	assert(!detector.failed(100 * kMillisecond));
	assert(!detector.failed(120 * kMillisecond));
	assert(detector.failed(120 * kMillisecond + 1));
}

void test_each_heartbeat_extends_the_deadline()
{
	ha::FailureDetector detector{5 * kMillisecond, 4};

	for (std::uint64_t now = 0; now < 1000 * kMillisecond; now += 5 * kMillisecond) {
		detector.on_heartbeat(now);
		assert(!detector.failed(now + 19 * kMillisecond));
	}

	assert(detector.last_heartbeat_ns() == 995 * kMillisecond);
	assert(detector.failed(1016 * kMillisecond));
}

void test_clock_behind_last_heartbeat_is_not_a_failure()
{
	ha::FailureDetector detector{1, 1};
	detector.on_heartbeat(50);
	assert(!detector.failed(10));
}

void test_zero_interval_is_invalid()
{
	assert(!ha::FailureDetector(0, 4).configuration_valid());
	assert(!ha::FailureDetector(5, 0).configuration_valid());
}

} // namespace

int main()
{
	test_unarmed_detector_never_fails();
	test_timeout_is_interval_times_missed_limit();
	test_each_heartbeat_extends_the_deadline();
	test_clock_behind_last_heartbeat_is_not_a_failure();
	test_zero_interval_is_invalid();
	return 0;
}
