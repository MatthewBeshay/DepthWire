#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "depthwire/trace/trace_codec.hpp"
#include "depthwire/trace/trace_format.hpp"
#include "depthwire/trace/trace_validator.hpp"
#include "depthwire/wire/market_data_packet.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, const std::size_t size)
{
	const auto input = std::as_bytes(std::span<const std::uint8_t>(data, size));

	const auto summary = depthwire::trace::validate_trace(input);

	if (!summary) {
		return 0;
	}

	// A validated trace must decode record-by-record and re-encode to the
	// identical bytes (the embedded hash included).
	const depthwire::trace::TraceView view{input};
	std::vector<depthwire::trace::TraceRecord> records{};
	records.reserve(view.record_count());

	for (std::uint32_t index = 0; index < view.record_count(); ++index) {
		const auto message = depthwire::wire::decode_market_data(view.message_bytes(index));
		assert(message);
		records.push_back({view.arrival_delta_ns(index), *message});
	}

	std::vector<std::byte> encoded(input.size());
	const auto header = depthwire::trace::encode_trace(records, summary->header.bounds, encoded);
	assert(header);

	for (std::size_t index = 0; index < encoded.size(); ++index) {
		assert(encoded[index] == input[index]);
	}

	return 0;
}
