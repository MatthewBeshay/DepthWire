#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <stop_token>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "depthwire/digest/sha256.hpp"
#include "depthwire/engine/trading_engine.hpp"
#include "depthwire/ha/codec.hpp"
#include "depthwire/ha/failure_detector.hpp"
#include "depthwire/ha/gap_tracker.hpp"
#include "depthwire/pipeline/packet_processor.hpp"
#include "depthwire/replay/conformance_runner.hpp"
#include "depthwire/replay/event_journal.hpp"
#include "depthwire/replay/replay_counters.hpp"
#include "depthwire/replay/run_digest.hpp"
#include "depthwire/trace/trace_codec.hpp"
#include "depthwire/trace/trace_format.hpp"
#include "depthwire/trace/trace_validator.hpp"
#include "depthwire/wire/ethernet_frame.hpp"
#include "depthwire/wire/wire_constants.hpp"
#include "depthwire/wire/wire_header.hpp"

namespace {

using namespace depthwire;

// 16384 records is 164 ms of feed at 100k records/s: room for a standby's
// detection timeout plus its catch-up request before the live feed outruns it.
constexpr std::size_t kTrackerCapacity = 16384;
constexpr std::uint32_t kMaxRequestRecords = 1024;
constexpr std::uint64_t kNanosecondsPerMillisecond = 1'000'000;
constexpr int kConnectTimeoutMs = 10'000;

using Tracker = ha::GapTracker<kTrackerCapacity>;

void print_usage(const char *const executable)
{
	std::printf("Usage:\n");
	std::printf("  %s --version\n", executable);
	std::printf("  %s feed --trace=FILE --to=ADDR:PORT[,ADDR:PORT...] --retransmit-listen=ADDR:PORT\n", executable);
	std::printf("        [--rate=N] [--drop-rate=P --seed=N] [--start-delay-ms=N] [--linger-ms=N]\n");
	std::printf("        [--interface=ADDR] [--report=FILE]\n");
	std::printf("  %s primary --trace=FILE --feed=ADDR:PORT --retransmit=ADDR:PORT\n", executable);
	std::printf("        [--standby=ADDR:PORT --heartbeat-to=ADDR:PORT] [--heartbeat-ms=N]\n");
	std::printf("        [--die-after=N] [--retry-ms=N] [--interface=ADDR] [--report=FILE]\n");
	std::printf("  %s standby --trace=FILE --feed=ADDR:PORT --retransmit=ADDR:PORT\n", executable);
	std::printf("        --replication-listen=ADDR:PORT --heartbeat-listen=ADDR:PORT\n");
	std::printf("        [--heartbeat-ms=N] [--missed=N] [--retry-ms=N] [--interface=ADDR] [--report=FILE]\n");
	std::printf("\n");
	std::printf("A feed address in 224.0.0.0/4 is joined as a multicast group on --interface;\n");
	std::printf("any other address is unicast. A primary without --standby runs alone.\n");
}

[[nodiscard]] std::uint64_t monotonic_ns() noexcept
{
	timespec now{};
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (static_cast<std::uint64_t>(now.tv_sec) * 1'000'000'000ULL) + static_cast<std::uint64_t>(now.tv_nsec);
}

// Failures go to the report as well as stderr, so a failed run's report says why.
[[noreturn]] void fail(const char *const message)
{
	std::printf("failure: %s\n", message);
	std::fflush(stdout);
	std::fprintf(stderr, "DepthWire failure: %s\n", message);
	std::exit(EXIT_FAILURE);
}

[[noreturn]] void fail_errno(const char *const operation)
{
	const int error = errno;
	std::printf("failure: %s: %s\n", operation, std::strerror(error));
	std::fflush(stdout);
	std::fprintf(stderr, "DepthWire failure: %s: %s\n", operation, std::strerror(error));
	std::exit(EXIT_FAILURE);
}

class Socket {
public:
	Socket() noexcept = default;

	explicit Socket(const int descriptor) noexcept : descriptor_(descriptor) {}

	Socket(const Socket &) = delete;
	Socket &operator=(const Socket &) = delete;

	Socket(Socket &&other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}

	Socket &operator=(Socket &&other) noexcept
	{
		if (this != &other) {
			reset();
			descriptor_ = std::exchange(other.descriptor_, -1);
		}

		return *this;
	}

	~Socket() { reset(); }

	[[nodiscard]] int get() const noexcept { return descriptor_; }

	[[nodiscard]] bool valid() const noexcept { return descriptor_ >= 0; }

	void reset() noexcept
	{
		if (descriptor_ >= 0) {
			::close(descriptor_);
			descriptor_ = -1;
		}
	}

private:
	int descriptor_{-1};
};

template<typename Number> [[nodiscard]] bool parse_number(const std::string_view text, Number &output) noexcept
{
	if (text.empty()) {
		return false;
	}

	const char *const first = text.data();
	const char *const last = text.data() + text.size();
	const auto result = std::from_chars(first, last, output);
	return result.ec == std::errc{} && result.ptr == last;
}

[[nodiscard]] std::optional<in_addr> parse_address(const std::string_view text) noexcept
{
	std::array<char, INET_ADDRSTRLEN> host{};

	if (text.empty() || text.size() >= host.size()) {
		return std::nullopt;
	}

	std::ranges::copy(text, host.begin());
	in_addr address{};

	if (inet_pton(AF_INET, host.data(), &address) != 1) {
		return std::nullopt;
	}

	return address;
}

[[nodiscard]] std::optional<sockaddr_in> parse_endpoint(const std::string_view text) noexcept
{
	const std::size_t colon = text.rfind(':');
	std::uint16_t port{};

	if (colon == std::string_view::npos || !parse_number(text.substr(colon + 1), port) || port == 0) {
		return std::nullopt;
	}

	const auto address = parse_address(text.substr(0, colon));

	if (!address) {
		return std::nullopt;
	}

	sockaddr_in endpoint{};
	endpoint.sin_family = AF_INET;
	endpoint.sin_port = htons(port);
	endpoint.sin_addr = *address;
	return endpoint;
}

[[nodiscard]] bool parse_endpoint_list(std::string_view text, std::vector<sockaddr_in> &output)
{
	while (!text.empty()) {
		const std::size_t comma = text.find(',');
		const auto endpoint = parse_endpoint(text.substr(0, comma));

		if (!endpoint) {
			return false;
		}

		output.push_back(*endpoint);
		text.remove_prefix(comma == std::string_view::npos ? text.size() : comma + 1);
	}

	return !output.empty();
}

[[nodiscard]] bool is_multicast(const sockaddr_in &endpoint) noexcept
{
	return (ntohl(endpoint.sin_addr.s_addr) & 0xF0000000U) == 0xE0000000U;
}

void set_option(const int descriptor, const int level, const int name, const void *const value, const socklen_t size,
		const char *const operation)
{
	if (::setsockopt(descriptor, level, name, value, size) != 0) {
		fail_errno(operation);
	}
}

void set_int_option(const int descriptor, const int level, const int name, const int value, const char *const operation)
{
	set_option(descriptor, level, name, &value, sizeof(value), operation);
}

[[nodiscard]] Socket open_socket(const int type, const char *const operation)
{
	Socket result{::socket(AF_INET, type, 0)};

	if (!result.valid()) {
		fail_errno(operation);
	}

	return result;
}

void bind_to(const Socket &socket, const sockaddr_in &endpoint, const char *const operation)
{
	if (::bind(socket.get(), reinterpret_cast<const sockaddr *>(&endpoint), sizeof(endpoint)) != 0) {
		fail_errno(operation);
	}
}

// A multicast group is bound by group address and joined on `interface`, so
// only that group's datagrams arrive; a unicast endpoint is bound directly.
[[nodiscard]] Socket open_datagram_receiver(const sockaddr_in &endpoint, const in_addr interface)
{
	Socket result = open_socket(SOCK_DGRAM | SOCK_NONBLOCK, "socket(feed)");
	set_int_option(result.get(), SOL_SOCKET, SO_REUSEADDR, 1, "setsockopt(SO_REUSEADDR)");
	// Best effort: the kernel caps this at net.core.rmem_max, and anything the
	// buffer cannot hold is recovered by gap-fill.
	const int receive_buffer = 8 << 20;
	static_cast<void>(::setsockopt(result.get(), SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer)));
	bind_to(result, endpoint, "bind(feed)");

	if (is_multicast(endpoint)) {
		ip_mreq membership{};
		membership.imr_multiaddr = endpoint.sin_addr;
		membership.imr_interface = interface;
		set_option(result.get(), IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership),
			   "setsockopt(IP_ADD_MEMBERSHIP)");
	}

	return result;
}

[[nodiscard]] Socket open_datagram_sender(const bool multicast, const in_addr interface)
{
	Socket result = open_socket(SOCK_DGRAM, "socket(sender)");

	if (multicast) {
		set_option(result.get(), IPPROTO_IP, IP_MULTICAST_IF, &interface, sizeof(interface),
			   "setsockopt(IP_MULTICAST_IF)");
		const unsigned char ttl = 1;
		set_option(result.get(), IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl),
			   "setsockopt(IP_MULTICAST_TTL)");
		const unsigned char loop = 1;
		set_option(result.get(), IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop),
			   "setsockopt(IP_MULTICAST_LOOP)");
	}

	return result;
}

[[nodiscard]] Socket open_listener(const sockaddr_in &endpoint)
{
	Socket result = open_socket(SOCK_STREAM, "socket(listen)");
	set_int_option(result.get(), SOL_SOCKET, SO_REUSEADDR, 1, "setsockopt(SO_REUSEADDR)");
	bind_to(result, endpoint, "bind(listen)");

	if (::listen(result.get(), 16) != 0) {
		fail_errno("listen");
	}

	return result;
}

void set_no_delay(const Socket &socket)
{
	set_int_option(socket.get(), IPPROTO_TCP, TCP_NODELAY, 1, "setsockopt(TCP_NODELAY)");
}

[[nodiscard]] Socket accept_within(const Socket &listener, const int timeout_ms)
{
	pollfd waiting{listener.get(), POLLIN, 0};
	const int ready = ::poll(&waiting, 1, timeout_ms);

	if (ready < 0) {
		fail_errno("poll(accept)");
	}

	if (ready == 0) {
		fail("no connection arrived before the timeout");
	}

	Socket result{::accept(listener.get(), nullptr, nullptr)};

	if (!result.valid()) {
		fail_errno("accept");
	}

	set_no_delay(result);
	return result;
}

// The peer may still be starting, so a refused connection is retried until
// the timeout.
[[nodiscard]] Socket connect_within(const sockaddr_in &endpoint, const int timeout_ms)
{
	const std::uint64_t deadline =
		monotonic_ns() + (static_cast<std::uint64_t>(timeout_ms) * kNanosecondsPerMillisecond);

	for (;;) {
		Socket result = open_socket(SOCK_STREAM, "socket(connect)");

		if (::connect(result.get(), reinterpret_cast<const sockaddr *>(&endpoint), sizeof(endpoint)) == 0) {
			set_no_delay(result);
			return result;
		}

		if (errno != ECONNREFUSED || monotonic_ns() > deadline) {
			fail_errno("connect");
		}

		std::this_thread::sleep_for(std::chrono::milliseconds{20});
	}
}

[[nodiscard]] bool send_all(const int descriptor, std::span<const std::byte> bytes) noexcept
{
	while (!bytes.empty()) {
		const ssize_t sent = ::send(descriptor, bytes.data(), bytes.size(), MSG_NOSIGNAL);

		if (sent < 0) {
			if (errno == EINTR) {
				continue;
			}

			return false;
		}

		bytes = bytes.subspan(static_cast<std::size_t>(sent));
	}

	return true;
}

enum class ReadStatus : std::uint8_t {
	open = 0,
	closed,
};

// Accumulates a byte stream read without blocking and hands out whole
// fixed-size messages.
class StreamBuffer {
public:
	[[nodiscard]] ReadStatus read_available(const int descriptor)
	{
		std::array<std::byte, 65536> chunk{};

		for (;;) {
			const ssize_t received = ::recv(descriptor, chunk.data(), chunk.size(), MSG_DONTWAIT);

			if (received > 0) {
				bytes_.insert(bytes_.end(), chunk.begin(), chunk.begin() + received);
				continue;
			}

			if (received == 0) {
				return ReadStatus::closed;
			}

			if (errno == EINTR) {
				continue;
			}

			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return ReadStatus::open;
			}

			if (errno == ECONNRESET) {
				return ReadStatus::closed;
			}

			fail_errno("recv(stream)");
		}
	}

	[[nodiscard]] std::optional<std::span<const std::byte>> take(const std::size_t size) noexcept
	{
		if (bytes_.size() - begin_ < size) {
			return std::nullopt;
		}

		const auto result = std::span<const std::byte>(bytes_).subspan(begin_, size);
		begin_ += size;
		return result;
	}

	// Invalidates every span returned by take().
	void compact()
	{
		bytes_.erase(bytes_.begin(), bytes_.begin() + static_cast<std::ptrdiff_t>(begin_));
		begin_ = 0;
	}

private:
	std::vector<std::byte> bytes_{};
	std::size_t begin_{};
};

struct LoadedTrace {
	std::vector<std::byte> file{};
	trace::TraceSummary summary{};
};

[[nodiscard]] LoadedTrace load_trace(const char *const path)
{
	LoadedTrace result{};
	std::FILE *const file = std::fopen(path, "rb");

	if (file == nullptr) {
		fail("cannot open the trace file");
	}

	constexpr std::size_t kMaximumBytes =
		static_cast<std::size_t>(trace::trace_file_bytes(trace::kMaxTraceRecords));
	bool success = std::fseek(file, 0, SEEK_END) == 0;
	const long size = success ? std::ftell(file) : -1;
	success = success && size >= 0 && static_cast<std::size_t>(size) <= kMaximumBytes &&
		  std::fseek(file, 0, SEEK_SET) == 0;

	if (success) {
		result.file.resize(static_cast<std::size_t>(size));
		success = std::fread(result.file.data(), 1, result.file.size(), file) == result.file.size();
	}

	if (std::fclose(file) != 0 || !success) {
		fail("cannot read the trace file");
	}

	const auto summary = trace::validate_trace(result.file);

	if (!summary) {
		fail("the trace file is not a valid DepthWire trace");
	}

	result.summary = *summary;
	return result;
}

// Processes each record exactly as replay::run_conformance does (same
// configuration, envelope, journal entry and immediate acknowledgement), so a
// run that applies every trace record once, in order, reproduces the
// conformance digest whichever host applied which records.
class EngineRun {
public:
	explicit EngineRun(const trace::TraceSummary &summary)
		: config_(replay::default_replay_processor_config(summary.header.bounds.instrument_id,
								  SequencePolicy::require_contiguous)),
		  processor_(config_),
		  journal_(summary.header.record_count),
		  record_count_(summary.header.record_count)
	{
		wire::encode_ethernet_header(replay::replay_market_envelope(), input_frame_);
	}

	EngineRun(const EngineRun &) = delete;
	EngineRun &operator=(const EngineRun &) = delete;
	EngineRun(EngineRun &&) = delete;
	EngineRun &operator=(EngineRun &&) = delete;
	~EngineRun() = default;

	[[nodiscard]] pipeline::PacketProcessResult apply(const wire::WireMessage &message)
	{
		std::ranges::copy(message,
				  input_frame_.begin() + static_cast<std::ptrdiff_t>(wire::kWirePayloadOffset));

		replay::JournalEntry entry{};
		const auto header = wire::decode_header(message, wire::MessageType::market_data);
		entry.input_sequence = header ? header->sequence_number : 0;

		wire::EthernetFrame output_frame{};
		const pipeline::PacketProcessResult result = processor_.process(input_frame_, output_frame);

		entry.process_status = result.status;
		entry.engine_status = result.engine_result.status;
		entry.has_frame = result.output_ready;

		if (result.output_ready) {
			entry.frame = output_frame;
		}

		if (!journal_.record(entry)) {
			fail("journal capacity exceeded");
		}

		replay::apply_process_result(counters_, result);

		if (result.output_ready && !processor_.try_acknowledge_one_order()) {
			fail("order acknowledgement failed");
		}

		return result;
	}

	[[nodiscard]] std::uint32_t applied() const noexcept { return static_cast<std::uint32_t>(journal_.size()); }

	[[nodiscard]] std::uint32_t record_count() const noexcept { return record_count_; }

	[[nodiscard]] bool finished() const noexcept { return applied() >= record_count_; }

	void set_kill_switch() noexcept { processor_.set_kill_switch(true); }

	void print_result()
	{
		counters_.offered = record_count_;
		counters_.accepted = record_count_;

		if (!replay::counters_conserved(counters_)) {
			fail("counter conservation violated");
		}

		std::printf("records-applied: %u\n", applied());
		std::printf("orders-encoded: %llu\n", static_cast<unsigned long long>(counters_.ordered));
		const auto hex = digest::to_hex(replay::compute_run_digest(journal_, counters_));
		std::printf("digest-sha256: %.*s\n", static_cast<int>(hex.size()), hex.data());
	}

private:
	pipeline::PacketProcessorConfig config_{};
	pipeline::PacketProcessor<> processor_;
	replay::EventJournal journal_;
	replay::ReplayCounters counters_{};
	wire::EthernetFrame input_frame_{};
	std::uint32_t record_count_{};
};

[[nodiscard]] std::uint64_t percentile(std::vector<std::uint64_t> values, const std::uint32_t per_mille)
{
	if (values.empty()) {
		return 0;
	}

	// Nearest rank, ceil(q * N), as in docs/benchmarks/methodology.md.
	std::ranges::sort(values);
	const std::size_t rank = std::max<std::size_t>(1, ((values.size() * per_mille) + 999) / 1000);
	return values[rank - 1];
}

struct ReceiverOptions {
	std::uint32_t first_expected{};
	std::uint64_t retry_ns{10 * kNanosecondsPerMillisecond};
	// A promoted standby knows records are missing even if the live feed has
	// already ended, so it may request the tail before any datagram arrives.
	bool request_tail_before_live{};
};

// Live feed plus gap-fill in front of the engine: every hole is requested as
// soon as a later datagram exposes it, re-requested while it stays open, and
// timed from detection until the engine has applied past it.
class LiveReceiver {
public:
	LiveReceiver(const int feed, Socket retransmit, const std::uint32_t record_count, const ReceiverOptions options)
		: feed_(feed),
		  retransmit_(std::move(retransmit)),
		  tracker_(std::make_unique<Tracker>(options.first_expected)),
		  record_count_(record_count),
		  frontier_(options.first_expected),
		  options_(options),
		  last_live_ns_(monotonic_ns())
	{
	}

	void poll_once(const int timeout_ms, const std::function<void(const ha::FeedRecord &)> &deliver)
	{
		std::array<pollfd, 2> waiting{{{feed_, POLLIN, 0}, {retransmit_.get(), POLLIN, 0}}};

		if (::poll(waiting.data(), waiting.size(), timeout_ms) < 0 && errno != EINTR) {
			fail_errno("poll(receiver)");
		}

		const std::uint64_t now = monotonic_ns();

		if ((waiting[0].revents & POLLIN) != 0) {
			drain_feed(now, deliver);
		}

		if ((waiting[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
			drain_retransmit(deliver);
		}

		request_gaps(now);
	}

	void print_summary() const
	{
		std::vector<std::uint64_t> recovery{};
		recovery.reserve(gaps_.size());

		for (const ClosedGap &gap : gaps_) {
			std::printf("gap first=%u count=%u recovery-ns=%llu\n", gap.first, gap.count,
				    static_cast<unsigned long long>(gap.recovery_ns));
			recovery.push_back(gap.recovery_ns);
		}

		std::printf("live-received: %llu\n", static_cast<unsigned long long>(live_received_));
		std::printf("filled-records: %llu\n", static_cast<unsigned long long>(filled_records_));
		std::printf("duplicates: %llu\n", static_cast<unsigned long long>(tracker_->duplicates()));
		std::printf("rejected-datagrams: %llu\n", static_cast<unsigned long long>(rejected_));
		std::printf("gap-requests: %llu\n", static_cast<unsigned long long>(requests_));
		std::printf("gaps-seen: %llu\n", static_cast<unsigned long long>(gaps_seen_));
		std::printf("gaps-recovered: %zu\n", gaps_.size());
		std::printf("gap-recovery-ns-p50: %llu\n", static_cast<unsigned long long>(percentile(recovery, 500)));
		std::printf("gap-recovery-ns-p99: %llu\n", static_cast<unsigned long long>(percentile(recovery, 990)));
		std::printf("gap-recovery-ns-max: %llu\n", static_cast<unsigned long long>(percentile(recovery, 1000)));
	}

private:
	struct OpenGap {
		std::uint32_t first{};
		std::uint32_t end{};
		std::uint64_t detected_ns{};
	};

	struct ClosedGap {
		std::uint32_t first{};
		std::uint32_t count{};
		std::uint64_t recovery_ns{};
	};

	void drain_feed(const std::uint64_t now, const std::function<void(const ha::FeedRecord &)> &deliver)
	{
		std::array<std::byte, ha::kFeedRecordBytes + 1> datagram{};

		for (;;) {
			const ssize_t received = ::recv(feed_, datagram.data(), datagram.size(), MSG_DONTWAIT);

			if (received < 0) {
				if (errno == EINTR) {
					continue;
				}

				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					return;
				}

				fail_errno("recv(feed)");
			}

			const auto record = ha::decode_feed_record(
				std::span<const std::byte>(datagram).first(static_cast<std::size_t>(received)));

			if (!record || record->index >= record_count_) {
				++rejected_;
				continue;
			}

			++live_received_;
			live_seen_ = true;
			last_live_ns_ = now;

			if (record->index > frontier_) {
				open_gap(frontier_, record->index, now);
				send_request(frontier_, record->index - frontier_, now);
			}

			offer(*record, false, deliver);
		}
	}

	void drain_retransmit(const std::function<void(const ha::FeedRecord &)> &deliver)
	{
		if (buffer_.read_available(retransmit_.get()) == ReadStatus::closed && !tracker_done()) {
			fail("the retransmit service closed the connection");
		}

		for (;;) {
			if (pending_records_ == 0) {
				const auto header = buffer_.take(ha::kGapFillResponseHeaderBytes);

				if (!header) {
					break;
				}

				pending_records_ = ha::decode_gap_fill_response_header(*header).value_or(0);
				continue;
			}

			const auto bytes = buffer_.take(ha::kFeedRecordBytes);

			if (!bytes) {
				break;
			}

			--pending_records_;
			const auto record = ha::decode_feed_record(*bytes);

			if (!record || record->index >= record_count_) {
				fail("malformed gap-fill response");
			}

			offer(*record, true, deliver);
		}

		buffer_.compact();
	}

	void offer(const ha::FeedRecord &record, const bool filled,
		   const std::function<void(const ha::FeedRecord &)> &deliver)
	{
		switch (tracker_->offer(record)) {
		case ha::OfferStatus::duplicate:
			return;
		case ha::OfferStatus::overflow:
			fail("reorder buffer overflow; stopping rather than trading past a hole");
		case ha::OfferStatus::accepted:
			break;
		}

		filled_records_ += filled ? 1U : 0U;
		frontier_ = std::max(frontier_, record.index + 1);

		ha::FeedRecord ready{};

		while (tracker_->pop(ready)) {
			deliver(ready);
		}

		while (!open_gaps_.empty() && tracker_->expected() >= open_gaps_.front().end) {
			const OpenGap &gap = open_gaps_.front();
			gaps_.push_back({gap.first, gap.end - gap.first, monotonic_ns() - gap.detected_ns});
			open_gaps_.pop_front();
		}
	}

	void open_gap(const std::uint32_t first, const std::uint32_t end, const std::uint64_t now)
	{
		open_gaps_.push_back({first, end, now});
		++gaps_seen_;
	}

	void send_request(const std::uint32_t first, const std::uint32_t count, const std::uint64_t now)
	{
		std::array<std::byte, ha::kGapFillRequestBytes> request{};
		ha::encode_gap_fill_request({first, std::min(count, kMaxRequestRecords)}, request);

		if (!send_all(retransmit_.get(), request)) {
			fail_errno("send(gap-fill request)");
		}

		++requests_;
		last_request_ns_ = now;
		requested_end_ = std::max(requested_end_, first + std::min(count, kMaxRequestRecords));
	}

	void request_gaps(const std::uint64_t now)
	{
		// A capped request leaves the rest of a long hole unrequested; ask for it
		// as soon as the hole's front reaches it rather than after a retry.
		if (const auto gap = tracker_->front_gap()) {
			if (gap->first_index >= requested_end_ || now - last_request_ns_ >= options_.retry_ns) {
				send_request(gap->first_index, gap->count, now);
			}

			return;
		}

		const std::uint32_t expected = tracker_->expected();

		if (expected >= record_count_ || tracker_->buffered() != 0 ||
		    (!live_seen_ && !options_.request_tail_before_live) || now - last_live_ns_ < options_.retry_ns ||
		    (expected < tail_end_ && now - last_request_ns_ < options_.retry_ns)) {
			return;
		}

		// The live feed has gone quiet with records still missing at the end.
		if (open_gaps_.empty() || open_gaps_.back().end <= expected) {
			open_gap(expected, record_count_, now);
		}

		tail_end_ = expected + std::min(record_count_ - expected, kMaxRequestRecords);
		send_request(expected, tail_end_ - expected, now);
	}

	[[nodiscard]] bool tracker_done() const noexcept { return tracker_->expected() >= record_count_; }

	int feed_{-1};
	Socket retransmit_;
	StreamBuffer buffer_{};
	std::unique_ptr<Tracker> tracker_;
	std::uint32_t record_count_{};
	std::uint32_t frontier_{};
	std::uint32_t pending_records_{};
	std::uint32_t tail_end_{};
	std::uint32_t requested_end_{};
	ReceiverOptions options_{};
	std::uint64_t last_live_ns_{};
	std::uint64_t last_request_ns_{};
	bool live_seen_{};
	std::deque<OpenGap> open_gaps_{};
	std::vector<ClosedGap> gaps_{};
	std::uint64_t live_received_{};
	std::uint64_t filled_records_{};
	std::uint64_t rejected_{};
	std::uint64_t requests_{};
	std::uint64_t gaps_seen_{};
};

struct Options {
	const char *trace_path{};
	const char *report_path{};
	std::vector<sockaddr_in> destinations{};
	std::optional<sockaddr_in> feed{};
	std::optional<sockaddr_in> retransmit{};
	std::optional<sockaddr_in> retransmit_listen{};
	std::optional<sockaddr_in> standby{};
	std::optional<sockaddr_in> heartbeat_to{};
	std::optional<sockaddr_in> replication_listen{};
	std::optional<sockaddr_in> heartbeat_listen{};
	in_addr interface{};
	std::uint64_t rate{100'000};
	double drop_rate{};
	std::uint64_t seed{1};
	std::uint64_t start_delay_ms{500};
	std::uint64_t linger_ms{3000};
	std::uint64_t heartbeat_ms{5};
	std::uint32_t missed{4};
	std::uint32_t die_after{};
	std::uint64_t retry_ms{10};
};

[[nodiscard]] bool parse_option(const std::string_view argument, Options &options)
{
	const std::size_t equals = argument.find('=');

	if (equals == std::string_view::npos) {
		return false;
	}

	const std::string_view key = argument.substr(0, equals);
	const std::string_view value = argument.substr(equals + 1);

	const auto endpoint = [&value](std::optional<sockaddr_in> &output) {
		output = parse_endpoint(value);
		return output.has_value();
	};

	if (key == "--trace") {
		options.trace_path = value.data();
		return !value.empty();
	}

	if (key == "--report") {
		options.report_path = value.data();
		return !value.empty();
	}

	if (key == "--to") {
		return parse_endpoint_list(value, options.destinations);
	}

	if (key == "--interface") {
		const auto address = parse_address(value);
		options.interface = address.value_or(in_addr{});
		return address.has_value();
	}

	if (key == "--feed") {
		return endpoint(options.feed);
	}

	if (key == "--retransmit") {
		return endpoint(options.retransmit);
	}

	if (key == "--retransmit-listen") {
		return endpoint(options.retransmit_listen);
	}

	if (key == "--standby") {
		return endpoint(options.standby);
	}

	if (key == "--heartbeat-to") {
		return endpoint(options.heartbeat_to);
	}

	if (key == "--replication-listen") {
		return endpoint(options.replication_listen);
	}

	if (key == "--heartbeat-listen") {
		return endpoint(options.heartbeat_listen);
	}

	if (key == "--rate") {
		return parse_number(value, options.rate) && options.rate > 0;
	}

	if (key == "--drop-rate") {
		return parse_number(value, options.drop_rate) && options.drop_rate >= 0.0 && options.drop_rate < 1.0;
	}

	if (key == "--seed") {
		return parse_number(value, options.seed);
	}

	if (key == "--start-delay-ms") {
		return parse_number(value, options.start_delay_ms);
	}

	if (key == "--linger-ms") {
		return parse_number(value, options.linger_ms);
	}

	if (key == "--heartbeat-ms") {
		return parse_number(value, options.heartbeat_ms) && options.heartbeat_ms > 0;
	}

	if (key == "--missed") {
		return parse_number(value, options.missed) && options.missed > 0;
	}

	if (key == "--die-after") {
		return parse_number(value, options.die_after) && options.die_after > 0;
	}

	if (key == "--retry-ms") {
		return parse_number(value, options.retry_ms) && options.retry_ms > 0;
	}

	return false;
}

void print_role(const char *const role, const Options &options, const trace::TraceSummary &summary)
{
	std::printf("DepthWire version:         %s\n", DEPTHWIRE_PROJECT_VERSION);
	std::printf("Source revision:           %s\n", DEPTHWIRE_SOURCE_REVISION);
	std::printf("Role:                      %s\n", role);
	std::printf("Trace file:                %s\n", options.trace_path);
	std::printf("Trace records:             %u\n", summary.header.record_count);
}

void serve_gap_fill_client(const std::stop_token &stop, const Socket &client, const trace::TraceView &view)
{
	StreamBuffer buffer{};
	std::vector<std::byte> response{};

	while (!stop.stop_requested()) {
		pollfd waiting{client.get(), POLLIN, 0};

		if (::poll(&waiting, 1, 50) <= 0) {
			continue;
		}

		const ReadStatus status = buffer.read_available(client.get());

		while (const auto bytes = buffer.take(ha::kGapFillRequestBytes)) {
			const auto request = ha::decode_gap_fill_request(*bytes).value_or(ha::GapFillRequest{});
			const std::uint32_t available =
				request.first_index < view.record_count()
					? std::min(request.count, view.record_count() - request.first_index)
					: 0;

			response.assign(ha::kGapFillResponseHeaderBytes +
						(std::size_t{available} * ha::kFeedRecordBytes),
					std::byte{});
			ha::encode_gap_fill_response_header(
				available, std::span<std::byte>(response).first<ha::kGapFillResponseHeaderBytes>());

			for (std::uint32_t offset = 0; offset < available; ++offset) {
				ha::FeedRecord record{};
				record.index = request.first_index + offset;
				std::ranges::copy(view.message_bytes(record.index), record.message.begin());
				const std::size_t position =
					ha::kGapFillResponseHeaderBytes + (std::size_t{offset} * ha::kFeedRecordBytes);
				ha::encode_feed_record(
					record,
					std::span<std::byte>(response).subspan(position).first<ha::kFeedRecordBytes>());
			}

			if (!send_all(client.get(), response)) {
				return;
			}
		}

		buffer.compact();

		if (status == ReadStatus::closed) {
			return;
		}
	}
}

int run_feed(const Options &options)
{
	if (options.destinations.empty() || !options.retransmit_listen) {
		return EXIT_FAILURE;
	}

	const LoadedTrace loaded = load_trace(options.trace_path);
	const trace::TraceView view{std::span<const std::byte>(loaded.file)};
	print_role("feed", options, loaded.summary);
	std::printf("Rate:                      %llu records/s\n", static_cast<unsigned long long>(options.rate));
	std::printf("Drop rate:                 %.6f (seed %llu)\n", options.drop_rate,
		    static_cast<unsigned long long>(options.seed));
	std::fflush(stdout);

	const bool multicast = std::ranges::any_of(options.destinations, is_multicast);
	const Socket sender = open_datagram_sender(multicast, options.interface);
	const Socket listener = open_listener(*options.retransmit_listen);

	std::jthread service([&listener, &view](const std::stop_token &stop) {
		std::vector<std::jthread> clients{};

		while (!stop.stop_requested()) {
			pollfd waiting{listener.get(), POLLIN, 0};

			if (::poll(&waiting, 1, 50) <= 0) {
				continue;
			}

			Socket client{::accept(listener.get(), nullptr, nullptr)};

			if (!client.valid()) {
				continue;
			}

			set_no_delay(client);
			clients.emplace_back(
				[&view, connection = std::move(client)](const std::stop_token &client_stop) {
					serve_gap_fill_client(client_stop, connection, view);
				});
		}
	});

	std::this_thread::sleep_for(std::chrono::milliseconds{options.start_delay_ms});

	std::mt19937_64 random{options.seed};
	std::bernoulli_distribution drop{options.drop_rate};
	std::uint64_t sent{};
	std::uint64_t dropped{};
	std::uint64_t send_errors{};
	std::array<std::byte, ha::kFeedRecordBytes> datagram{};
	const std::uint64_t start = monotonic_ns();

	for (std::uint32_t index = 0; index < view.record_count(); ++index) {
		const std::uint64_t due = start + ((std::uint64_t{index} * 1'000'000'000ULL) / options.rate);

		for (std::uint64_t now = monotonic_ns(); now < due; now = monotonic_ns()) {
			if (due - now > 200'000) {
				std::this_thread::sleep_for(std::chrono::nanoseconds{due - now - 100'000});
			}
		}

		if (options.drop_rate > 0.0 && drop(random)) {
			++dropped;
			continue;
		}

		ha::FeedRecord record{};
		record.index = index;
		std::ranges::copy(view.message_bytes(index), record.message.begin());
		ha::encode_feed_record(record, datagram);

		for (const sockaddr_in &destination : options.destinations) {
			if (::sendto(sender.get(), datagram.data(), datagram.size(), 0,
				     reinterpret_cast<const sockaddr *>(&destination), sizeof(destination)) < 0) {
				++send_errors;
			}
		}

		++sent;
	}

	std::printf("feed-sent: %llu\n", static_cast<unsigned long long>(sent));
	std::printf("feed-dropped: %llu\n", static_cast<unsigned long long>(dropped));
	std::printf("feed-send-errors: %llu\n", static_cast<unsigned long long>(send_errors));
	std::printf("feed-duration-ns: %llu\n", static_cast<unsigned long long>(monotonic_ns() - start));
	std::fflush(stdout);

	std::this_thread::sleep_for(std::chrono::milliseconds{options.linger_ms});
	service.request_stop();
	return EXIT_SUCCESS;
}

int run_primary(const Options &options)
{
	if (!options.feed || !options.retransmit || options.standby.has_value() != options.heartbeat_to.has_value()) {
		return EXIT_FAILURE;
	}

	const LoadedTrace loaded = load_trace(options.trace_path);
	print_role("primary", options, loaded.summary);
	std::fflush(stdout);

	EngineRun engine{loaded.summary};
	const Socket feed = open_datagram_receiver(*options.feed, options.interface);
	Socket replication{};
	Socket heartbeat{};

	if (options.standby) {
		replication = connect_within(*options.standby, kConnectTimeoutMs);
		heartbeat = open_datagram_sender(false, options.interface);
	}

	LiveReceiver receiver{feed.get(), connect_within(*options.retransmit, kConnectTimeoutMs), engine.record_count(),
			      ReceiverOptions{0, options.retry_ms * kNanosecondsPerMillisecond, false}};

	std::array<std::byte, ha::kReplicationRecordBytes> replication_bytes{};
	const std::function<void(const ha::FeedRecord &)> deliver = [&](const ha::FeedRecord &record) {
		const pipeline::PacketProcessResult result = engine.apply(record.message);

		// Write-ahead: the standby holds the record before any order for it
		// would leave this host.
		if (replication.valid()) {
			ha::encode_replication_record({record.index, record.message, std::to_underlying(result.status),
						       std::to_underlying(result.engine_result.status)},
						      replication_bytes);

			if (!send_all(replication.get(), replication_bytes)) {
				engine.set_kill_switch();
				std::printf("self-fence: replication write failed after %u records\n",
					    engine.applied());
				std::fflush(stdout);
				std::exit(3);
			}
		}

		if (options.die_after != 0 && engine.applied() == options.die_after) {
			std::printf("primary-applied-at-death: %u\n", engine.applied());
			std::printf("primary-death-monotonic-ns: %llu\n",
				    static_cast<unsigned long long>(monotonic_ns()));
			std::fflush(stdout);
			std::_Exit(EXIT_SUCCESS);
		}
	};

	const sockaddr_in heartbeat_target = options.heartbeat_to.value_or(sockaddr_in{});
	const std::uint64_t heartbeat_interval = options.heartbeat_ms * kNanosecondsPerMillisecond;
	std::uint64_t next_heartbeat = monotonic_ns();
	std::uint64_t heartbeats{};
	std::array<std::byte, ha::kHeartbeatBytes> heartbeat_bytes{};

	while (!engine.finished()) {
		const std::uint64_t now = monotonic_ns();

		if (heartbeat.valid() && now >= next_heartbeat) {
			ha::encode_heartbeat({++heartbeats, engine.applied()}, heartbeat_bytes);
			static_cast<void>(::sendto(heartbeat.get(), heartbeat_bytes.data(), heartbeat_bytes.size(), 0,
						   reinterpret_cast<const sockaddr *>(&heartbeat_target),
						   sizeof(heartbeat_target)));
			next_heartbeat = now + heartbeat_interval;
		}

		receiver.poll_once(1, deliver);
	}

	receiver.print_summary();
	engine.print_result();
	return EXIT_SUCCESS;
}

int run_standby(const Options &options)
{
	if (!options.feed || !options.retransmit || !options.replication_listen || !options.heartbeat_listen) {
		return EXIT_FAILURE;
	}

	const LoadedTrace loaded = load_trace(options.trace_path);
	print_role("standby", options, loaded.summary);
	std::fflush(stdout);

	EngineRun engine{loaded.summary};
	// Joined from the start so promotion does not wait for a multicast join;
	// datagrams are discarded until then.
	const Socket feed = open_datagram_receiver(*options.feed, options.interface);
	const Socket heartbeat = open_datagram_receiver(*options.heartbeat_listen, options.interface);
	const Socket listener = open_listener(*options.replication_listen);
	Socket replication = accept_within(listener, kConnectTimeoutMs);

	ha::FailureDetector detector{options.heartbeat_ms * kNanosecondsPerMillisecond, options.missed};
	StreamBuffer replicated{};
	bool replication_open = true;
	std::uint64_t heartbeats{};
	std::uint64_t discarded{};

	const auto apply_replicated = [&engine, &replicated] {
		while (const auto bytes = replicated.take(ha::kReplicationRecordBytes)) {
			const auto record = ha::decode_replication_record(*bytes);

			if (!record || record->index != engine.applied()) {
				fail("replication stream out of order");
			}

			const pipeline::PacketProcessResult result = engine.apply(record->message);

			if (std::to_underlying(result.status) != record->process_status ||
			    std::to_underlying(result.engine_result.status) != record->engine_status) {
				std::printf("replication-divergence: record %u\n", record->index);
				std::fflush(stdout);
				std::exit(4);
			}
		}

		replicated.compact();
	};

	std::array<std::byte, ha::kFeedRecordBytes + 1> datagram{};
	std::uint64_t failure_detected_ns{};

	while (!engine.finished()) {
		std::array<pollfd, 3> waiting{{{replication_open ? replication.get() : -1, POLLIN, 0},
					       {heartbeat.get(), POLLIN, 0},
					       {feed.get(), POLLIN, 0}}};

		if (::poll(waiting.data(), waiting.size(), 1) < 0 && errno != EINTR) {
			fail_errno("poll(standby)");
		}

		const std::uint64_t now = monotonic_ns();

		if ((waiting[1].revents & POLLIN) != 0) {
			for (;;) {
				const ssize_t received =
					::recv(heartbeat.get(), datagram.data(), datagram.size(), MSG_DONTWAIT);

				if (received < 0) {
					break;
				}

				if (ha::decode_heartbeat(std::span<const std::byte>(datagram).first(
					    static_cast<std::size_t>(received)))) {
					detector.on_heartbeat(now);
					++heartbeats;
				}
			}
		}

		if ((waiting[2].revents & POLLIN) != 0) {
			while (::recv(feed.get(), datagram.data(), datagram.size(), MSG_DONTWAIT) >= 0) {
				++discarded;
			}
		}

		if (replication_open && (waiting[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
			replication_open = replicated.read_available(replication.get()) == ReadStatus::open;
			apply_replicated();
		}

		if (detector.failed(now)) {
			failure_detected_ns = now;

			// Apply what the primary had already written, then take over.
			if (replication_open) {
				static_cast<void>(replicated.read_available(replication.get()));
				apply_replicated();
			}

			break;
		}
	}

	replication.reset();

	if (engine.finished()) {
		std::printf("promoted: no\n");
		std::printf("heartbeats-received: %llu\n", static_cast<unsigned long long>(heartbeats));
		engine.print_result();
		return EXIT_SUCCESS;
	}

	std::printf("promoted: yes\n");
	std::printf("failure-detected-monotonic-ns: %llu\n", static_cast<unsigned long long>(failure_detected_ns));
	std::printf("last-heartbeat-monotonic-ns: %llu\n",
		    static_cast<unsigned long long>(detector.last_heartbeat_ns()));
	std::printf("detector-timeout-ns: %llu\n", static_cast<unsigned long long>(detector.timeout_ns()));
	std::printf("heartbeats-received: %llu\n", static_cast<unsigned long long>(heartbeats));
	std::printf("feed-discarded-while-standby: %llu\n", static_cast<unsigned long long>(discarded));
	std::printf("applied-at-promotion: %u\n", engine.applied());
	std::fflush(stdout);

	LiveReceiver receiver{feed.get(), connect_within(*options.retransmit, kConnectTimeoutMs), engine.record_count(),
			      ReceiverOptions{engine.applied(), options.retry_ms * kNanosecondsPerMillisecond, true}};
	std::uint64_t first_apply_ns{};
	const std::function<void(const ha::FeedRecord &)> deliver = [&engine,
								     &first_apply_ns](const ha::FeedRecord &record) {
		static_cast<void>(engine.apply(record.message));

		if (first_apply_ns == 0) {
			first_apply_ns = monotonic_ns();
		}
	};

	while (!engine.finished()) {
		receiver.poll_once(1, deliver);
	}

	std::printf("promoted-first-apply-monotonic-ns: %llu\n", static_cast<unsigned long long>(first_apply_ns));
	std::printf("promoted-finished-monotonic-ns: %llu\n", static_cast<unsigned long long>(monotonic_ns()));
	receiver.print_summary();
	engine.print_result();
	return EXIT_SUCCESS;
}

} // namespace

int main(int argc, char **argv)
{
	const std::span<char *> arguments(argv, static_cast<std::size_t>(argc));

	if (arguments.size() == 2 && std::string_view{arguments[1]} == "--version") {
		std::printf("depthwire-ha %s (revision %s)\n", DEPTHWIRE_PROJECT_VERSION, DEPTHWIRE_SOURCE_REVISION);
		return EXIT_SUCCESS;
	}

	if (arguments.size() < 2) {
		print_usage(arguments[0]);
		return EXIT_FAILURE;
	}

	Options options{};

	for (const char *const argument : arguments.subspan(2)) {
		if (!parse_option(argument, options)) {
			std::fprintf(stderr, "DepthWire failure: invalid option %s\n", argument);
			print_usage(arguments[0]);
			return EXIT_FAILURE;
		}
	}

	if (options.trace_path == nullptr) {
		print_usage(arguments[0]);
		return EXIT_FAILURE;
	}

	if (options.report_path != nullptr && std::freopen(options.report_path, "w", stdout) == nullptr) {
		fail("cannot open the report file");
	}

	const std::string_view role = arguments[1];
	int result = EXIT_FAILURE;

	if (role == "feed") {
		result = run_feed(options);
	} else if (role == "primary") {
		result = run_primary(options);
	} else if (role == "standby") {
		result = run_standby(options);
	}

	if (result == EXIT_FAILURE) {
		print_usage(arguments[0]);
	}

	std::fflush(stdout);
	return result;
}
