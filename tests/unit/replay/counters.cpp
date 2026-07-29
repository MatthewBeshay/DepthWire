#include <cassert>

#include "depthwire/engine/trading_engine.hpp"
#include "depthwire/pipeline/packet_processor.hpp"
#include "depthwire/replay/replay_counters.hpp"

namespace {

using namespace depthwire;

pipeline::PacketProcessResult make_result(const pipeline::PacketProcessStatus status,
					  const EngineStatus engine_status) noexcept
{
	pipeline::PacketProcessResult result{};
	result.status = status;
	result.engine_result.status = engine_status;
	return result;
}

// Every PacketProcessStatus / EngineStatus combination the pipeline can
// produce lands in exactly one counter, and conservation holds afterwards.
void test_classification()
{
	replay::ReplayCounters counters{};

	apply_process_result(counters, make_result(pipeline::PacketProcessStatus::no_order, EngineStatus::no_order));
	assert(counters.status_no_order == 1);

	apply_process_result(counters,
			     make_result(pipeline::PacketProcessStatus::order_encoded, EngineStatus::order_emitted));
	assert(counters.status_order_encoded == 1);
	assert(counters.ordered == 1);

	apply_process_result(counters,
			     make_result(pipeline::PacketProcessStatus::decode_rejected, EngineStatus::no_order));
	assert(counters.status_decode_rejected == 1);

	apply_process_result(counters, make_result(pipeline::PacketProcessStatus::input_adapter_rejected,
						   EngineStatus::no_order));
	apply_process_result(counters, make_result(pipeline::PacketProcessStatus::output_adapter_rejected,
						   EngineStatus::order_emitted));
	assert(counters.status_adapter_rejected == 2);

	apply_process_result(counters, make_result(pipeline::PacketProcessStatus::engine_rejected,
						   EngineStatus::duplicate_update));
	assert(counters.engine_duplicate == 1);

	apply_process_result(counters,
			     make_result(pipeline::PacketProcessStatus::engine_rejected, EngineStatus::sequence_gap));
	assert(counters.engine_gap == 1);

	apply_process_result(counters,
			     make_result(pipeline::PacketProcessStatus::engine_rejected, EngineStatus::stale_update));
	assert(counters.engine_stale == 1);

	apply_process_result(counters,
			     make_result(pipeline::PacketProcessStatus::engine_rejected, EngineStatus::invalid_update));
	assert(counters.engine_invalid == 1);

	apply_process_result(counters,
			     make_result(pipeline::PacketProcessStatus::engine_rejected, EngineStatus::risk_rejected));
	apply_process_result(counters,
			     make_result(pipeline::PacketProcessStatus::engine_rejected, EngineStatus::outbox_full));
	assert(counters.engine_other == 2);

	apply_process_result(counters, make_result(pipeline::PacketProcessStatus::invalid_input_destination,
						   EngineStatus::no_order));
	apply_process_result(counters, make_result(pipeline::PacketProcessStatus::output_encode_rejected,
						   EngineStatus::order_emitted));
	assert(counters.status_other == 2);

	assert(counters.processed == 13);

	counters.offered = 13;
	counters.accepted = 13;
	assert(replay::counters_conserved(counters));
}

void test_conservation_violations_detected()
{
	replay::ReplayCounters counters{};
	counters.offered = 2;
	counters.accepted = 1;
	counters.dropped = 1;
	counters.processed = 1;
	counters.status_no_order = 1;
	assert(replay::counters_conserved(counters));

	// A processed event that was never classified.
	counters.processed = 2;
	counters.accepted = 2;
	counters.dropped = 0;
	assert(!replay::counters_conserved(counters));
	counters.status_no_order = 2;
	assert(replay::counters_conserved(counters));

	// ordered must equal status_order_encoded.
	counters.ordered = 1;
	assert(!replay::counters_conserved(counters));
	counters.status_order_encoded = 1;
	counters.status_no_order = 1;
	assert(replay::counters_conserved(counters));

	// offered must equal accepted + dropped.
	counters.dropped = 1;
	assert(!replay::counters_conserved(counters));
}

} // namespace

int main()
{
	test_classification();
	test_conservation_violations_detected();
	return 0;
}
