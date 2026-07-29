#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "depthwire/digest/sha256.hpp"
#include "depthwire/engine/trading_engine.hpp"
#include "depthwire/replay/conformance_runner.hpp"
#include "depthwire/replay/event_journal.hpp"
#include "depthwire/replay/pacer.hpp"
#include "depthwire/replay/replay_counters.hpp"
#include "depthwire/trace/trace_codec.hpp"
#include "depthwire/trace/trace_format.hpp"
#include "depthwire/trace/trace_text.hpp"
#include "depthwire/trace/trace_validator.hpp"
#include "depthwire/wire/market_data_packet.hpp"
#include "report.hpp"

#ifdef DEPTHWIRE_REPLAY_ENABLE_DPDK
#	include "paced_run.hpp"
#endif

namespace {

using namespace depthwire;
using replay_app::print_counters;
using replay_app::print_hex_line;
using replay_app::print_identity;
using replay_app::print_trace_summary;
using replay_app::print_trace_validation_error;

void print_usage(const char *const executable)
{
	std::printf("Usage:\n");
	std::printf("  %s --version\n", executable);
	std::printf("  %s validate <trace.dwtrace>\n", executable);
	std::printf("  %s convert --to-binary <in.txt> <out.dwtrace>\n", executable);
	std::printf("  %s convert --to-text <in.dwtrace> <out.txt>\n", executable);
	std::printf("  %s run <trace.dwtrace> [options] [-- <EAL arguments>]\n", executable);
	std::printf("\n");
	std::printf("run options:\n");
	std::printf("  --mode=conformance|paced      (default conformance)\n");
	std::printf("  --sequence-policy=contiguous|accept-any\n");
	std::printf("                                (default: contiguous for conformance,\n");
	std::printf("                                 accept-any for paced)\n");
	std::printf("  --scale-ppm=N                 paced arrival-spacing scale;\n");
	std::printf("                                1000000 replays recorded spacing\n");
	std::printf("  --segments=IDX:PPM[,IDX:PPM...]\n");
	std::printf("                                paced per-segment scales; first IDX must\n");
	std::printf("                                be 0 and indices must ascend\n");
	std::printf("  --lateness-bound-ns=N         paced lateness bound (default 50000)\n");
	std::printf("  --burst=N                     paced loop burst, 1..128 (default 32)\n");
	std::printf("\n");
	std::printf("  Paced mode requires a DPDK-enabled build; EAL arguments follow --.\n");
}

// Bounded whole-file read: a trace can never exceed the container's declared
// maximum, and the text form is bounded by the same record limit.
constexpr std::size_t kMaximumInputBytes = static_cast<std::size_t>(trace::trace_file_bytes(trace::kMaxTraceRecords));

[[nodiscard]] bool read_file(const char *const path, std::vector<std::byte> &output)
{
	std::FILE *const file = std::fopen(path, "rb");

	if (file == nullptr) {
		std::fprintf(stderr, "DepthWire failure: cannot open %s\n", path);
		return false;
	}

	bool success = std::fseek(file, 0, SEEK_END) == 0;
	const long size = success ? std::ftell(file) : -1;
	success = success && size >= 0 && static_cast<std::size_t>(size) <= kMaximumInputBytes &&
		  std::fseek(file, 0, SEEK_SET) == 0;

	if (success) {
		output.resize(static_cast<std::size_t>(size));
		success = std::fread(output.data(), 1, output.size(), file) == output.size();
	}

	if (std::fclose(file) != 0 || !success) {
		std::fprintf(stderr, "DepthWire failure: cannot read %s\n", path);
		return false;
	}

	return true;
}

[[nodiscard]] bool write_file(const char *const path, const std::span<const std::byte> bytes)
{
	std::FILE *const file = std::fopen(path, "wb");

	if (file == nullptr) {
		std::fprintf(stderr, "DepthWire failure: cannot create %s\n", path);
		return false;
	}

	const bool success = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();

	if (std::fclose(file) != 0 || !success) {
		std::fprintf(stderr, "DepthWire failure: cannot write %s\n", path);
		return false;
	}

	return true;
}

template<typename Unsigned> [[nodiscard]] bool parse_unsigned(const std::string_view text, Unsigned &output) noexcept
{
	if (text.empty()) {
		return false;
	}

	const char *const first = text.data();
	const char *const last = text.data() + text.size();
	const auto result = std::from_chars(first, last, output);
	return result.ec == std::errc{} && result.ptr == last;
}

int run_validate(const char *const path)
{
	std::vector<std::byte> file{};

	if (!read_file(path, file)) {
		return EXIT_FAILURE;
	}

	const auto summary = trace::validate_trace(file);

	if (!summary) {
		print_trace_validation_error(summary.error());
		return EXIT_FAILURE;
	}

	std::printf("============================================================\n");
	std::printf("DepthWire - Trace Validation\n");
	std::printf("============================================================\n");
	print_identity("validate");
	print_trace_summary(path, file, *summary);
	std::printf("\nStatus:                    OK\n");
	return EXIT_SUCCESS;
}

int run_convert_to_binary(const char *const input_path, const char *const output_path)
{
	std::vector<std::byte> text_bytes{};

	if (!read_file(input_path, text_bytes)) {
		return EXIT_FAILURE;
	}

	const std::string_view text{reinterpret_cast<const char *>(text_bytes.data()), text_bytes.size()};
	const auto parsed = trace::parse_trace_text(text);

	if (!parsed) {
		std::fprintf(stderr, "DepthWire failure: text parse failed at line %zu (code %u)\n",
			     parsed.error().line, static_cast<unsigned>(std::to_underlying(parsed.error().code)));
		return EXIT_FAILURE;
	}

	std::vector<std::byte> file(trace::trace_file_bytes(static_cast<std::uint32_t>(parsed->records.size())));
	const auto header = trace::encode_trace(parsed->records, parsed->bounds, file);

	if (!header) {
		print_trace_validation_error(header.error());
		return EXIT_FAILURE;
	}

	if (!write_file(output_path, file)) {
		return EXIT_FAILURE;
	}

	std::printf("Converted %s -> %s (%u records)\n", input_path, output_path, header->record_count);
	print_hex_line("trace-sha256", digest::sha256_of(file));
	std::printf("Status:                    OK\n");
	return EXIT_SUCCESS;
}

int run_convert_to_text(const char *const input_path, const char *const output_path)
{
	std::vector<std::byte> file{};

	if (!read_file(input_path, file)) {
		return EXIT_FAILURE;
	}

	const auto summary = trace::validate_trace(file);

	if (!summary) {
		print_trace_validation_error(summary.error());
		return EXIT_FAILURE;
	}

	const trace::TraceView view{std::span<const std::byte>(file)};
	std::vector<trace::TraceRecord> records{};
	records.reserve(view.record_count());

	for (std::uint32_t index = 0; index < view.record_count(); ++index) {
		const auto message = wire::decode_market_data(view.message_bytes(index));

		if (!message) {
			std::fprintf(stderr, "DepthWire failure: record %u decode failed\n", index);
			return EXIT_FAILURE;
		}

		records.push_back({view.arrival_delta_ns(index), *message});
	}

	const auto text = trace::format_trace_text(summary->header.bounds, records);

	if (!text) {
		print_trace_validation_error(text.error());
		return EXIT_FAILURE;
	}

	if (!write_file(output_path, std::as_bytes(std::span<const char>(text->data(), text->size())))) {
		return EXIT_FAILURE;
	}

	std::printf("Converted %s -> %s (%u records)\n", input_path, output_path, summary->header.record_count);
	std::printf("Status:                    OK\n");
	return EXIT_SUCCESS;
}

struct RunOptions {
	const char *trace_path{};
	bool paced{};
	SequencePolicy sequence_policy{SequencePolicy::require_contiguous};
	std::vector<replay::PaceSegment> segments{};
	std::uint64_t lateness_bound_ns{50'000};
	std::uint16_t burst{32};
	int eal_arguments_start{-1};
};

// The lateness bound participates in signed nanosecond arithmetic; 1000
// seconds is far beyond any meaningful pacing bound.
constexpr std::uint64_t kMaxLatenessBoundNs = 1'000'000'000'000ULL;

[[nodiscard]] bool parse_segments(const std::string_view specification, std::vector<replay::PaceSegment> &segments)
{
	std::string_view remaining = specification;

	while (!remaining.empty()) {
		const std::size_t comma = remaining.find(',');
		const std::string_view entry = remaining.substr(0, comma);
		remaining.remove_prefix(comma == std::string_view::npos ? remaining.size() : comma + 1);

		const std::size_t colon = entry.find(':');

		if (colon == std::string_view::npos) {
			return false;
		}

		replay::PaceSegment segment{};

		if (!parse_unsigned(entry.substr(0, colon), segment.first_record) ||
		    !parse_unsigned(entry.substr(colon + 1), segment.scale_ppm) || segment.scale_ppm == 0) {
			return false;
		}

		if (segments.empty()) {
			if (segment.first_record != 0) {
				return false;
			}
		} else if (segment.first_record <= segments.back().first_record) {
			return false;
		}

		segments.push_back(segment);
	}

	return !segments.empty();
}

[[nodiscard]] bool parse_run_options(const int argc, char **const argv, RunOptions &options)
{
	if (argc < 3) {
		return false;
	}

	options.trace_path = argv[2];
	bool policy_set = false;
	std::uint32_t scale_ppm = replay::kRecordedSpacingPpm;
	bool scale_set = false;
	bool segments_set = false;
	bool lateness_set = false;
	bool burst_set = false;

	for (int index = 3; index < argc; ++index) {
		const std::string_view argument{argv[index]};

		if (argument == "--") {
			options.eal_arguments_start = index + 1;
			break;
		}

		constexpr std::string_view scale_prefix{"--scale-ppm="};
		constexpr std::string_view segments_prefix{"--segments="};
		constexpr std::string_view lateness_prefix{"--lateness-bound-ns="};
		constexpr std::string_view burst_prefix{"--burst="};

		if (argument == "--mode=conformance") {
			options.paced = false;
		} else if (argument == "--mode=paced") {
			options.paced = true;
		} else if (argument == "--sequence-policy=contiguous") {
			options.sequence_policy = SequencePolicy::require_contiguous;
			policy_set = true;
		} else if (argument == "--sequence-policy=accept-any") {
			options.sequence_policy = SequencePolicy::accept_any;
			policy_set = true;
		} else if (argument.starts_with(scale_prefix)) {
			if (!parse_unsigned(argument.substr(scale_prefix.size()), scale_ppm) || scale_ppm == 0) {
				std::fprintf(stderr, "Invalid --scale-ppm value\n");
				return false;
			}

			scale_set = true;
		} else if (argument.starts_with(segments_prefix)) {
			if (!parse_segments(argument.substr(segments_prefix.size()), options.segments)) {
				std::fprintf(stderr, "Invalid --segments value\n");
				return false;
			}

			segments_set = true;
		} else if (argument.starts_with(lateness_prefix)) {
			if (!parse_unsigned(argument.substr(lateness_prefix.size()), options.lateness_bound_ns) ||
			    options.lateness_bound_ns > kMaxLatenessBoundNs) {
				std::fprintf(stderr, "Invalid --lateness-bound-ns value; expected 0..%llu\n",
					     static_cast<unsigned long long>(kMaxLatenessBoundNs));
				return false;
			}

			lateness_set = true;
		} else if (argument.starts_with(burst_prefix)) {
			std::uint16_t burst{};

			if (!parse_unsigned(argument.substr(burst_prefix.size()), burst) || burst == 0 || burst > 128) {
				std::fprintf(stderr, "Invalid --burst value; expected 1..128\n");
				return false;
			}

			options.burst = burst;
			burst_set = true;
		} else {
			std::fprintf(stderr, "Unknown run argument: %s\n", argv[index]);
			return false;
		}
	}

	if (segments_set && scale_set) {
		std::fprintf(stderr, "--scale-ppm and --segments are mutually exclusive\n");
		return false;
	}

	if (!segments_set) {
		options.segments.push_back(replay::PaceSegment{0, scale_ppm});
	}

	// Paced-only options are configuration errors in conformance mode:
	// accepting and ignoring them would misrepresent what the run measured.
	if (!options.paced &&
	    (scale_set || segments_set || lateness_set || burst_set || options.eal_arguments_start >= 0)) {
		std::fprintf(stderr, "conformance mode accepts no pacing options or EAL arguments "
				     "(--scale-ppm, --segments, --lateness-bound-ns, --burst, --)\n");
		return false;
	}

	// Under require_contiguous a single paced drop gaps the feed permanently
	// (the engine's contiguous-sequence contract), so lossy paced runs
	// default to the explicit opt-out.
	if (options.paced && !policy_set) {
		options.sequence_policy = SequencePolicy::accept_any;
	}

	return true;
}

void print_run_configuration(const pipeline::PacketProcessorConfig &config)
{
	std::printf("Sequence policy:           %s\n",
		    config.engine.sequence_policy == SequencePolicy::require_contiguous ? "require_contiguous"
											: "accept_any");
	std::printf("Imbalance threshold:       %lld bps\n",
		    static_cast<long long>(config.engine.strategy.imbalance_threshold_bps));
	std::printf("Order quantity:            %u\n", config.engine.strategy.order_quantity);
	std::printf("Max outstanding orders:    %u\n", config.engine.risk.max_outstanding_orders);
	std::printf("First output sequence:     %u\n", config.first_output_sequence);
}

int run_conformance_mode(const RunOptions &options, const std::vector<std::byte> &file,
			 const trace::TraceSummary &summary)
{
	const auto config =
		replay::default_replay_processor_config(summary.header.bounds.instrument_id, options.sequence_policy);

	replay::EventJournal journal{summary.header.record_count};
	const trace::TraceView view{std::span<const std::byte>(file)};

	const auto result = replay::run_conformance(view, config, replay::replay_market_envelope(), journal);

	if (!result) {
		std::fprintf(stderr, "DepthWire failure: conformance run failed (code %u)\n",
			     static_cast<unsigned>(std::to_underlying(result.error())));
		return EXIT_FAILURE;
	}

	if (!replay::counters_conserved(result->counters)) {
		std::fprintf(stderr, "DepthWire failure: counter conservation violated\n");
		return EXIT_FAILURE;
	}

	std::printf("============================================================\n");
	std::printf("DepthWire - Trace Replay\n");
	std::printf("============================================================\n");
	print_identity("conformance");
	print_trace_summary(options.trace_path, file, summary);
	print_run_configuration(config);
	std::printf("\n");
	std::printf("Determinism claim:\n");
	std::printf("  Same accepted input sequence and configuration produce the same\n");
	std::printf("  digest across independent runs. Conformance replay\n");
	std::printf("  processes every record without pacing or transport.\n");
	std::printf("\n");
	print_counters(result->counters);
	std::printf("\n");
	print_hex_line("digest-sha256", result->digest);
	std::printf("\n");
	std::printf("Classification:\n");
	std::printf("  Deterministic transport-neutral conformance replay. This is not\n");
	std::printf("  offered-load, latency, throughput, or overload-recovery evidence.\n");
	std::printf("\nStatus:                    OK\n");
	return EXIT_SUCCESS;
}

int run_replay(const int argc, char **const argv)
{
	RunOptions options{};

	if (!parse_run_options(argc, argv, options)) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	std::vector<std::byte> file{};

	if (!read_file(options.trace_path, file)) {
		return EXIT_FAILURE;
	}

	const auto summary = trace::validate_trace(file);

	if (!summary) {
		print_trace_validation_error(summary.error());
		return EXIT_FAILURE;
	}

	// Segment indices are trace positions; declaring one beyond the trace is
	// a configuration error (indices ascend, so checking the last suffices).
	if (options.segments.back().first_record >= summary->header.record_count) {
		std::fprintf(stderr,
			     "Invalid --segments value: record index %u is beyond the trace's %u "
			     "records\n",
			     options.segments.back().first_record, summary->header.record_count);
		return EXIT_FAILURE;
	}

	if (!options.paced) {
		return run_conformance_mode(options, file, *summary);
	}

#ifdef DEPTHWIRE_REPLAY_ENABLE_DPDK
	replay_app::PacedOptions paced{};
	paced.trace_path = options.trace_path;
	paced.sequence_policy = options.sequence_policy;
	paced.segments = options.segments;
	paced.lateness_bound_ns = options.lateness_bound_ns;
	paced.burst = options.burst;
	paced.eal_arguments.push_back(argv[0]);

	if (options.eal_arguments_start > 0) {
		for (int index = options.eal_arguments_start; index < argc; ++index) {
			paced.eal_arguments.push_back(argv[index]);
		}
	}

	return replay_app::run_paced(paced, file, *summary);
#else
	std::fprintf(stderr, "DepthWire failure: paced mode requires a DPDK-enabled build; "
			     "this executable was built without DEPTHWIRE_ENABLE_DPDK.\n");
	return EXIT_FAILURE;
#endif
}

} // namespace

int main(int argc, char **argv)
{
	if (argc < 2) {
		print_usage(argv[0]);
		return EXIT_FAILURE;
	}

	const std::string_view subcommand{argv[1]};

	if (subcommand == "--help" || subcommand == "help") {
		print_usage(argv[0]);
		return EXIT_SUCCESS;
	}

	if (subcommand == "--version" || subcommand == "version") {
		std::printf("depthwire-replay %s (revision %s)\n", DEPTHWIRE_PROJECT_VERSION,
			    DEPTHWIRE_SOURCE_REVISION);
		return EXIT_SUCCESS;
	}

	if (subcommand == "validate" && argc == 3) {
		return run_validate(argv[2]);
	}

	if (subcommand == "convert" && argc == 5) {
		const std::string_view direction{argv[2]};

		if (direction == "--to-binary") {
			return run_convert_to_binary(argv[3], argv[4]);
		}

		if (direction == "--to-text") {
			return run_convert_to_text(argv[3], argv[4]);
		}
	}

	if (subcommand == "run") {
		return run_replay(argc, argv);
	}

	print_usage(argv[0]);
	return EXIT_FAILURE;
}
