#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

#include "depthwire/wire/ethernet_frame.hpp"

int main()
{
	using namespace depthwire::wire;

	static_assert(kEthernetHeaderBytes == 14);
	static_assert(kWireMessageBytes == 48);
	static_assert(kEthernetFrameBytesWithoutFcs == 62);

	EthernetEnvelope input_envelope{};
	input_envelope.destination = {
		std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x10},
	};
	input_envelope.source = {
		std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x20},
	};

	MarketDataMessage input_message{};
	input_message.sequence_number = 101;
	input_message.wire_flags = 0;
	input_message.exchange_timestamp_ns = 5500000001ULL;
	input_message.instrument_id = 7;
	input_message.price_ticks = 100250;
	input_message.quantity = 500;
	input_message.side = Side::buy;
	input_message.action = UpdateAction::modify;
	input_message.level = 2;
	input_message.source_flags = 0;

	EthernetFrame frame{};
	assert(encode_market_data_frame(input_envelope, input_message, frame));

	assert(std::to_integer<std::uint8_t>(frame[12]) == 0x88U);
	assert(std::to_integer<std::uint8_t>(frame[13]) == 0xB5U);

	const auto result = decode_market_data_frame(std::span<const std::byte>(frame));

	assert(result);
	const auto &output_envelope = result->envelope;
	const auto &output_message = result->message;

	assert(output_envelope.destination == input_envelope.destination);
	assert(output_envelope.source == input_envelope.source);

	assert(output_message.sequence_number == input_message.sequence_number);
	assert(output_message.wire_flags == input_message.wire_flags);
	assert(output_message.exchange_timestamp_ns == input_message.exchange_timestamp_ns);
	assert(output_message.instrument_id == input_message.instrument_id);
	assert(output_message.price_ticks == input_message.price_ticks);
	assert(output_message.quantity == input_message.quantity);
	assert(output_message.side == input_message.side);
	assert(output_message.action == input_message.action);
	assert(output_message.level == input_message.level);
	assert(output_message.source_flags == input_message.source_flags);

	EthernetFrame wrong_ether_type = frame;
	assert(try_store_be16(std::span<std::byte>(wrong_ether_type), kEtherTypeOffset, 0x0800U));

	{
		const auto r = decode_market_data_frame(std::span<const std::byte>(wrong_ether_type));
		assert(!r && r.error() == WireError::wrong_ether_type);
	}

	const auto short_frame = std::span<const std::byte>(frame.data(), frame.size() - 1);

	{
		const auto r = decode_market_data_frame(short_frame);
		assert(!r && r.error() == WireError::too_short);
	}

	return 0;
}
