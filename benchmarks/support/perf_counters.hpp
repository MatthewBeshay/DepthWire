#pragma once

// Non-installed benchmark infrastructure: a bounded, allocation-free wrapper
// over the Linux perf_event_open interface that reads a fixed set of hardware
// and software counters around an explicitly delimited region. It mirrors the
// isolate-the-measured-region discipline of dpdk_latency_recorder.hpp: the
// counters wrap only the hot loop, never the construction or warm-up phases.
//
// It self-measures the calling process (pid = 0, cpu = -1), which the kernel
// permits without elevated privilege at perf_event_paranoid <= 2 with
// exclude_kernel set. Each hardware counter is opened independently rather than
// as one all-or-nothing group: a group larger than the available PMU counter
// slots is never scheduled at all (it reads back zero with zero running time),
// whereas independent counters are multiplexed and scaled by the kernel. Each
// value is scaled by enabled/running time, exactly as "perf stat" does, and a
// multiplexed flag is reported so a reader knows whether scaling occurred.
//
// The operating-system counters (context switches, page faults) come from
// getrusage rather than perf software events. A perf PERF_COUNT_SW_CONTEXT_SWITCHES
// event is raised only in kernel context, so with exclude_kernel set (required
// at paranoid = 2) it reads a structural zero, and opening it without
// exclude_kernel is denied; getrusage reports the real per-region delta with no
// such restriction.
//
// When a hardware counter cannot be opened (unsupported PMU, a stricter paranoid
// level, or a sandbox) the wrapper reports the reason and the benchmark falls
// back to running the loop without counters, so the run still completes and
// says why.

#include <cstddef>
#include <cstdint>

#if defined(__linux__)

#	include <cerrno>
#	include <cstring>

#	include <asm/unistd.h>
#	include <linux/perf_event.h>
#	include <sys/ioctl.h>
#	include <sys/resource.h>
#	include <unistd.h>

#endif

namespace depthwire::bench {

struct PerfCounts {
	std::uint64_t cycles{};
	std::uint64_t instructions{};
	std::uint64_t branches{};
	std::uint64_t branch_misses{};
	std::uint64_t cache_references{};
	std::uint64_t cache_misses{};
	std::uint64_t context_switches{};
	std::uint64_t page_faults{};
	// True when at least one counter was multiplexed onto the PMU and its value
	// is a scaled estimate rather than an exact count.
	bool multiplexed{false};
	bool valid{false};
};

#if defined(__linux__)

// Six hardware events, opened independently. The operating-system counters are
// taken from getrusage, not perf, so they are not in this count.
inline constexpr std::size_t kPerfEventCount = 6;

class PerfCounterGroup {
public:
	PerfCounterGroup() noexcept
	{
		struct EventSpec {
			std::uint32_t type;
			std::uint64_t config;
		};

		// Order matches PerfCounts assembly in read().
		constexpr EventSpec specs[kPerfEventCount] = {
			{PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
			{PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
			{PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_INSTRUCTIONS},
			{PERF_TYPE_HARDWARE, PERF_COUNT_HW_BRANCH_MISSES},
			{PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_REFERENCES},
			{PERF_TYPE_HARDWARE, PERF_COUNT_HW_CACHE_MISSES},
		};

		for (const EventSpec &spec : specs) {
			if (!open_event(spec.type, spec.config)) {
				close_all();
				return;
			}
		}

		available_ = true;
	}

	PerfCounterGroup(const PerfCounterGroup &) = delete;
	PerfCounterGroup &operator=(const PerfCounterGroup &) = delete;
	PerfCounterGroup(PerfCounterGroup &&) = delete;
	PerfCounterGroup &operator=(PerfCounterGroup &&) = delete;

	~PerfCounterGroup() { close_all(); }

	[[nodiscard]] bool available() const noexcept { return available_; }

	[[nodiscard]] const char *unavailable_reason() const noexcept { return reason_; }

	void reset_and_enable() noexcept
	{
		if (!available_) {
			return;
		}

		for (const int fd : fds_) {
			::ioctl(fd, PERF_EVENT_IOC_RESET, 0);
			::ioctl(fd, PERF_EVENT_IOC_ENABLE, 0);
		}

		::getrusage(RUSAGE_SELF, &rusage_start_);
	}

	void disable() noexcept
	{
		if (!available_) {
			return;
		}

		::getrusage(RUSAGE_SELF, &rusage_end_);

		for (const int fd : fds_) {
			::ioctl(fd, PERF_EVENT_IOC_DISABLE, 0);
		}
	}

	[[nodiscard]] PerfCounts read() const noexcept
	{
		PerfCounts counts{};

		if (!available_) {
			return counts;
		}

		std::uint64_t values[kPerfEventCount] = {};
		bool multiplexed = false;

		for (std::size_t index = 0; index < kPerfEventCount; ++index) {
			if (!read_scaled(fds_[index], values[index], multiplexed)) {
				return counts;
			}
		}

		counts.cycles = values[0];
		counts.instructions = values[1];
		counts.branches = values[2];
		counts.branch_misses = values[3];
		counts.cache_references = values[4];
		counts.cache_misses = values[5];

		// Operating-system counters from the getrusage delta over the measured
		// region. ru_nvcsw/ru_nivcsw are voluntary/involuntary context switches;
		// ru_minflt/ru_majflt are minor/major page faults.
		counts.context_switches = rusage_delta(rusage_start_.ru_nvcsw, rusage_end_.ru_nvcsw) +
					  rusage_delta(rusage_start_.ru_nivcsw, rusage_end_.ru_nivcsw);
		counts.page_faults = rusage_delta(rusage_start_.ru_minflt, rusage_end_.ru_minflt) +
				     rusage_delta(rusage_start_.ru_majflt, rusage_end_.ru_majflt);
		counts.multiplexed = multiplexed;
		counts.valid = true;
		return counts;
	}

private:
	[[nodiscard]] static long perf_event_open(perf_event_attr *attr, pid_t pid, int cpu, int group_fd,
						  unsigned long flags) noexcept
	{
		return ::syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
	}

	// Reads one independently-opened counter and scales it by enabled/running,
	// which is how the kernel reports a multiplexed counter (perf stat uses the
	// same correction). read_format layout: { value, time_enabled, time_running }.
	[[nodiscard]] static bool read_scaled(int fd, std::uint64_t &out, bool &multiplexed) noexcept
	{
		std::uint64_t sample[3] = {};

		if (::read(fd, sample, sizeof(sample)) != static_cast<ssize_t>(sizeof(sample))) {
			return false;
		}

		const std::uint64_t value = sample[0];
		const std::uint64_t enabled = sample[1];
		const std::uint64_t running = sample[2];

		if (running == 0) {
			// The counter was never scheduled onto the PMU; treat it as a scaled
			// (and here unmeasured) value rather than a real zero.
			out = 0;
			multiplexed = multiplexed || enabled > 0;
			return true;
		}

		if (running < enabled) {
			// Scale in double to avoid overflow in value * enabled; the relative
			// error is far below the counter's own sampling noise. perf stat
			// applies the same enabled/running correction.
			const double scaled = (static_cast<double>(value) * static_cast<double>(enabled)) /
					      static_cast<double>(running);
			out = static_cast<std::uint64_t>(scaled);
			multiplexed = true;
			return true;
		}

		out = value;
		return true;
	}

	[[nodiscard]] bool open_event(std::uint32_t type, std::uint64_t config) noexcept
	{
		perf_event_attr attr{};
		attr.type = type;
		attr.size = sizeof(perf_event_attr);
		attr.config = config;
		attr.disabled = 1U;
		// exclude_kernel/exclude_hv keep the measurement to user space, which is
		// what perf_event_paranoid = 2 permits without privilege.
		attr.exclude_kernel = 1U;
		attr.exclude_hv = 1U;
		attr.read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;

		const long fd = perf_event_open(&attr, 0, -1, -1, 0);

		if (fd < 0) {
			reason_ = std::strerror(errno);
			return false;
		}

		fds_[count_] = static_cast<int>(fd);
		++count_;
		return true;
	}

	void close_all() noexcept
	{
		for (std::size_t index = count_; index > 0; --index) {
			if (fds_[index - 1] >= 0) {
				::close(fds_[index - 1]);
				fds_[index - 1] = -1;
			}
		}

		count_ = 0;
		available_ = false;
	}

	// getrusage fields are non-decreasing over a process's life; guard against a
	// spurious inversion so an underflow can never appear as a huge count.
	[[nodiscard]] static std::uint64_t rusage_delta(long start, long end) noexcept
	{
		return end > start ? static_cast<std::uint64_t>(end - start) : 0;
	}

	int fds_[kPerfEventCount] = {-1, -1, -1, -1, -1, -1};
	std::size_t count_{0};
	bool available_{false};
	const char *reason_{"perf counters not initialised"};
	rusage rusage_start_{};
	rusage rusage_end_{};
};

#else // !defined(__linux__)

// Portable stub: no counters off Linux; the benchmark still runs its loop.
class PerfCounterGroup {
public:
	[[nodiscard]] bool available() const noexcept { return false; }
	[[nodiscard]] const char *unavailable_reason() const noexcept { return "perf counters require Linux"; }
	void reset_and_enable() noexcept {}
	void disable() noexcept {}
	[[nodiscard]] PerfCounts read() const noexcept { return PerfCounts{}; }
};

#endif // defined(__linux__)

} // namespace depthwire::bench
