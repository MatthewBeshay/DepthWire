#include <depthwire/book/fixed_l2_book.hpp>
#include <depthwire/contracts/contracts.hpp>
#include <depthwire/digest/sha256.hpp>
#include <depthwire/engine/trading_engine.hpp>
#include <depthwire/market/types.hpp>
#include <depthwire/order/order_request.hpp>
#include <depthwire/order/preallocated_outbox.hpp>
#include <depthwire/pipeline/engine_wire_adapter.hpp>
#include <depthwire/pipeline/packet_processor.hpp>
#include <depthwire/replay/conformance_runner.hpp>
#include <depthwire/replay/event_journal.hpp>
#include <depthwire/replay/pacer.hpp>
#include <depthwire/replay/replay_counters.hpp>
#include <depthwire/replay/run_digest.hpp>
#include <depthwire/risk/risk_guard.hpp>
#include <depthwire/strategy/imbalance_strategy.hpp>
#include <depthwire/trace/trace_codec.hpp>
#include <depthwire/trace/trace_format.hpp>
#include <depthwire/trace/trace_text.hpp>
#include <depthwire/trace/trace_validator.hpp>
#include <depthwire/wire/endian.hpp>
#include <depthwire/wire/ethernet_frame.hpp>
#include <depthwire/wire/market_data_packet.hpp>
#include <depthwire/wire/order_packet.hpp>
#include <depthwire/wire/wire_constants.hpp>
#include <depthwire/wire/wire_header.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <type_traits>

int main()
{
	static_assert(std::is_default_constructible_v<depthwire::EngineConfig>);
	static_assert(sizeof(depthwire::OrderRequest) == 32);
	static_assert(depthwire::wire::kWireMessageBytes == 48);
	static_assert(depthwire::wire::kEthernetFrameBytesWithoutFcs == 62);

	// Exercise the exported DepthWire::Digest package: the empty-message
	// SHA-256 digest is a compile-time constant.
	static_assert(depthwire::digest::to_hex(depthwire::digest::sha256_of({}))[0] == 'e');

	// Exercise the exported DepthWire::Trace package.
	static_assert(depthwire::trace::trace_file_bytes(1) == 132);
	static_assert(!depthwire::trace::decode_trace_header({}));

	// Exercise the exported DepthWire::Replay package: schedule building,
	// synthetic-time pacing, journaling, and the run digest.
	const std::array<std::uint64_t, 2> deltas{1000, 1000};
	const std::array<depthwire::replay::PaceSegment, 1> segments{
		depthwire::replay::PaceSegment{0, depthwire::replay::kRecordedSpacingPpm}};
	std::array<std::uint64_t, 2> offsets{};

	if (!depthwire::replay::build_schedule(deltas, segments, offsets) || offsets[1] != 2000) {
		return 1;
	}

	depthwire::replay::MonotonicPacer pacer{offsets, std::chrono::nanoseconds{100}};

	if (pacer.due_now(std::chrono::nanoseconds{2000}, 32).count != 2) {
		return 1;
	}

	depthwire::replay::EventJournal journal{1};
	depthwire::replay::ReplayCounters counters{};

	if (!journal.record({}) ||
	    depthwire::replay::compute_run_digest(journal, counters) == depthwire::digest::Sha256Digest{}) {
		return 1;
	}

	depthwire::TradingEngine<> engine{};

	// Exercise the exported DepthWire::Contracts package. The macro is inert
	// unless the installing project opted into contracts, so this compiles on
	// every consumer while still proving the component is discoverable.
	DW_CONTRACT_ASSERT(!engine.book().has_complete_visible_depth());

	return engine.book().has_complete_visible_depth() ? 1 : 0;
}
