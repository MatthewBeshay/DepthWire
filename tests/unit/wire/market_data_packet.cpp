#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

#include "depthwire/wire/market_data_packet.hpp"

int main()
{
	using namespace depthwire::wire;

	static_assert(kWireHeaderBytes == 16);
	static_assert(kPayloadBytes == 32);
	static_assert(kWireMessageBytes == 48);
	static_assert(kEthernetFrameBytesWithoutFcs == 62);

	MarketDataMessage input{};
	input.sequence_number = 7001;
	input.wire_flags = 0;
	input.exchange_timestamp_ns = 987654321012345ULL;
	input.instrument_id = 42;
	input.price_ticks = 100250;
	input.quantity = 750;
	input.side = Side::buy;
	input.action = UpdateAction::modify;
	input.level = 3;
	input.source_flags = 0;

	WireMessage encoded{};
	assert(encode_market_data(input, encoded));

	const auto decoded = decode_market_data(std::span<const std::byte>(encoded));

	assert(decoded);
	const auto &output = *decoded;
	assert(output.sequence_number == input.sequence_number);
	assert(output.wire_flags == input.wire_flags);
	assert(output.exchange_timestamp_ns == input.exchange_timestamp_ns);
	assert(output.instrument_id == input.instrument_id);
	assert(output.price_ticks == input.price_ticks);
	assert(output.quantity == input.quantity);
	assert(output.side == input.side);
	assert(output.action == input.action);
	assert(output.level == input.level);
	assert(output.source_flags == input.source_flags);

	auto corrupted = encoded;
	corrupted[0] = std::byte{0};

	{
		const auto r = decode_market_data(std::span<const std::byte>(corrupted));
		assert(!r && r.error() == WireError::invalid_magic);
	}

	const auto short_input = std::span<const std::byte>(encoded.data(), encoded.size() - 1);

	{
		const auto r = decode_market_data(short_input);
		assert(!r && r.error() == WireError::too_short);
	}

	return 0;
}
