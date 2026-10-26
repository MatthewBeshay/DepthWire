#pragma once

#include <cstdint>

namespace depthwire::ha {

// Heartbeat-timeout failure detection with time passed in, so it is tested
// without sleeping. It arms on the first heartbeat: a standby that has never
// heard from a primary has nothing to take over from.
class FailureDetector {
public:
	FailureDetector(const std::uint64_t interval_ns, const std::uint32_t missed_limit) noexcept
		: timeout_ns_(interval_ns * missed_limit)
	{
	}

	[[nodiscard]] bool configuration_valid() const noexcept { return timeout_ns_ > 0; }

	void on_heartbeat(const std::uint64_t now_ns) noexcept
	{
		last_heartbeat_ns_ = now_ns;
		armed_ = true;
	}

	[[nodiscard]] bool failed(const std::uint64_t now_ns) const noexcept
	{
		return armed_ && now_ns > last_heartbeat_ns_ && now_ns - last_heartbeat_ns_ > timeout_ns_;
	}

	[[nodiscard]] bool armed() const noexcept { return armed_; }

	[[nodiscard]] std::uint64_t last_heartbeat_ns() const noexcept { return last_heartbeat_ns_; }

	[[nodiscard]] std::uint64_t timeout_ns() const noexcept { return timeout_ns_; }

private:
	std::uint64_t timeout_ns_{};
	std::uint64_t last_heartbeat_ns_{};
	bool armed_{};
};

} // namespace depthwire::ha
