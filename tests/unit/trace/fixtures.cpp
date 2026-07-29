#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "depthwire/trace/trace_codec.hpp"
#include "depthwire/trace/trace_format.hpp"
#include "depthwire/trace/trace_text.hpp"
#include "depthwire/trace/trace_validator.hpp"
#include "support/trace_fixture_builder.hpp"

// Proves the checked-in fixtures under tests/fixtures/trace/ are exactly the
// bytes produced by their recorded construction rules
// (tests/support/trace_fixture_builder.hpp), that both validate, and that the
// conformance text form round-trips to the same records.

namespace {

std::vector<std::byte> read_file(const std::string &path)
{
	std::FILE *const file = std::fopen(path.c_str(), "rb");
	assert(file != nullptr);

	assert(std::fseek(file, 0, SEEK_END) == 0);
	const long size = std::ftell(file);
	assert(size > 0);
	assert(std::fseek(file, 0, SEEK_SET) == 0);

	std::vector<std::byte> contents(static_cast<std::size_t>(size));
	assert(std::fread(contents.data(), 1, contents.size(), file) == contents.size());
	assert(std::fclose(file) == 0);
	return contents;
}

std::vector<std::byte> encode(const std::vector<depthwire::trace::TraceRecord> &records,
			      const depthwire::trace::TraceBounds &bounds)
{
	std::vector<std::byte> file(depthwire::trace::trace_file_bytes(static_cast<std::uint32_t>(records.size())));
	const auto header = depthwire::trace::encode_trace(records, bounds, file);
	assert(header);
	return file;
}

} // namespace

int main(const int argc, char **const argv)
{
	assert(argc == 2);
	const std::string fixture_directory = argv[1];

	using namespace depthwire;

	// Conformance fixture: checked-in binary == recorded construction rule.
	const auto conformance_records = test_support::conformance_trace_records();
	const auto conformance_bounds = test_support::conformance_trace_bounds();
	const auto conformance_expected = encode(conformance_records, conformance_bounds);
	const auto conformance_file = read_file(fixture_directory + "/conformance_v1.dwtrace");
	assert(conformance_file == conformance_expected);

	const auto conformance_summary = trace::validate_trace(std::span<const std::byte>(conformance_file));
	assert(conformance_summary);
	assert(conformance_summary->header.record_count == 40);
	assert(conformance_summary->header.bounds.instrument_id == test_support::kFixtureInstrumentId);

	// Conformance text: formats from the rule byte-for-byte, and parses back
	// to records that encode to the identical binary.
	const auto conformance_text = read_file(fixture_directory + "/conformance_v1.dwtrace.txt");
	const std::string text_contents(reinterpret_cast<const char *>(conformance_text.data()),
					conformance_text.size());

	const auto formatted = trace::format_trace_text(conformance_bounds, conformance_records);
	assert(formatted);
	assert(*formatted == text_contents);

	const auto parsed = trace::parse_trace_text(text_contents);
	assert(parsed);
	assert(parsed->bounds.instrument_id == conformance_bounds.instrument_id);
	assert(parsed->bounds.max_arrival_delta_ns == conformance_bounds.max_arrival_delta_ns);
	assert(parsed->bounds.max_quantity == conformance_bounds.max_quantity);
	assert(encode(parsed->records, parsed->bounds) == conformance_expected);

	// Load fixture: checked-in binary == recorded construction rule.
	const auto load_expected = encode(test_support::load_trace_records(), test_support::load_trace_bounds());
	const auto load_file = read_file(fixture_directory + "/load_uniform_100k_v1.dwtrace");
	assert(load_file == load_expected);

	const auto load_summary = trace::validate_trace(std::span<const std::byte>(load_file));
	assert(load_summary);
	assert(load_summary->header.record_count == test_support::kLoadTraceRecordCount);
	assert(load_summary->header.total_duration_ns ==
	       test_support::kLoadTraceRecordCount * test_support::kLoadTraceDeltaNs);

	return 0;
}
