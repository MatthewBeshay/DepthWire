#include <array>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "depthwire/digest/sha256.hpp"
#include "depthwire/engine/trading_engine.hpp"
#include "depthwire/pipeline/packet_processor.hpp"
#include "depthwire/replay/conformance_runner.hpp"
#include "depthwire/replay/event_journal.hpp"
#include "depthwire/trace/trace_codec.hpp"
#include "depthwire/trace/trace_validator.hpp"

// Replays the checked-in conformance fixture and pins both the exact
// per-record status sequence and the digest hex. The digest constant is the
// cross-compiler, cross-machine determinism contract: GCC and Clang CI jobs
// must reproduce it bit-for-bit (docs/architecture/trace-format.md).

namespace {

using namespace depthwire;

// The fixture's expected outcome per record (see
// tests/support/trace_fixture_builder.hpp for the construction rule).
struct ExpectedOutcome {
	pipeline::PacketProcessStatus process_status;
	EngineStatus engine_status;
};

constexpr ExpectedOutcome kSeed{pipeline::PacketProcessStatus::no_order, EngineStatus::no_order};
constexpr ExpectedOutcome kOrder{pipeline::PacketProcessStatus::order_encoded, EngineStatus::order_emitted};
constexpr ExpectedOutcome kBalance{pipeline::PacketProcessStatus::no_order, EngineStatus::no_order};

constexpr std::array<ExpectedOutcome, 40> kExpected{
	kSeed,
	kSeed,
	kSeed,
	kSeed,
	kSeed,
	kSeed,
	kSeed,
	kSeed,
	kSeed,
	kSeed, // 1-10
	kOrder,
	kOrder,
	kOrder,
	kOrder,
	kOrder,
	kOrder, // 11-16
	kOrder,
	kOrder,
	kOrder,
	kOrder,
	kOrder,
	kOrder,                                                                                          // 17-22 (wrap)
	ExpectedOutcome{pipeline::PacketProcessStatus::engine_rejected, EngineStatus::duplicate_update}, // 23
	ExpectedOutcome{pipeline::PacketProcessStatus::engine_rejected, EngineStatus::stale_update},     // 24
	ExpectedOutcome{pipeline::PacketProcessStatus::engine_rejected, EngineStatus::sequence_gap},     // 25
	kOrder,
	kOrder,                                                                                        // 26-27
	ExpectedOutcome{pipeline::PacketProcessStatus::engine_rejected, EngineStatus::invalid_update}, // 28
	kBalance,
	kBalance, // 29-30
	kOrder,
	kBalance,
	kOrder,
	kBalance,
	kOrder,
	kBalance,
	kOrder,
	kBalance,
	kOrder,
	kBalance, // 31-40
};

// Pinned digest of the fixture replayed with the default configuration and
// require_contiguous. Reproduced by GCC and Clang, in fresh processes, on any
// machine. Changing the fixture, the engine's decision path, the digest
// stream, or the default replay configuration legitimately changes this
// value; anything else reproducing a mismatch is a determinism defect.
constexpr std::string_view kExpectedDigestHex = "8ee63c8ce91d646ca7b93d021ef452587abcd710ba82a2ee04fe01a3ad9c5952";

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

} // namespace

int main(const int argc, char **const argv)
{
	assert(argc == 2);

	const auto file = read_file(std::string(argv[1]) + "/conformance_v1.dwtrace");
	const auto summary = trace::validate_trace(std::span<const std::byte>(file));
	assert(summary);
	assert(summary->header.record_count == kExpected.size());

	const auto config = replay::default_replay_processor_config(summary->header.bounds.instrument_id,
								    SequencePolicy::require_contiguous);
	const trace::TraceView view{std::span<const std::byte>(file)};

	replay::EventJournal journal{summary->header.record_count};
	const auto result = replay::run_conformance(view, config, replay::replay_market_envelope(), journal);
	assert(result);

	// Exact per-record status sequence.
	const auto entries = journal.entries();
	assert(entries.size() == kExpected.size());

	for (std::size_t index = 0; index < kExpected.size(); ++index) {
		assert(entries[index].process_status == kExpected[index].process_status);
		assert(entries[index].engine_status == kExpected[index].engine_status);
		assert(entries[index].has_frame ==
		       (kExpected[index].process_status == pipeline::PacketProcessStatus::order_encoded));
	}

	// Counter totals implied by the expected sequence.
	const auto &counters = result->counters;
	assert(counters.offered == 40);
	assert(counters.accepted == 40);
	assert(counters.processed == 40);
	assert(counters.ordered == 19);
	assert(counters.status_no_order == 17);
	assert(counters.engine_duplicate == 1);
	assert(counters.engine_gap == 1);
	assert(counters.engine_stale == 1);
	assert(counters.engine_invalid == 1);
	assert(counters.dropped == 0);
	assert(replay::counters_conserved(counters));

	// Determinism: the pinned digest, and a second fresh run reproducing it.
	const auto hex = digest::to_hex(result->digest);

	if (kExpectedDigestHex != std::string_view{hex.data(), hex.size()}) {
		std::fprintf(stderr, "digest mismatch:\n  expected %.*s\n  actual   %.*s\n",
			     static_cast<int>(kExpectedDigestHex.size()), kExpectedDigestHex.data(),
			     static_cast<int>(hex.size()), hex.data());
		return 1;
	}

	replay::EventJournal second_journal{summary->header.record_count};
	const auto second_result =
		replay::run_conformance(view, config, replay::replay_market_envelope(), second_journal);
	assert(second_result);
	assert(second_result->digest == result->digest);

	return 0;
}
