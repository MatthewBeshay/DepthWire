#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "depthwire/wire/endian.hpp"
#include "depthwire/wire/ethernet_frame.hpp"

int main()
{
	using namespace depthwire::wire;

	MarketDataMessage market{};
	market.sequence_number = 10;
	market.exchange_timestamp_ns = 20;
	market.instrument_id = 77;
	market.price_ticks = 100;
	market.quantity = 5;
	market.side = Side::buy;
	market.action = UpdateAction::modify;

	WireMessage encoded_market{};
	assert(encode_market_data(market, encoded_market));

	for (std::size_t length = 0; length < encoded_market.size(); ++length) {
		const auto r = decode_market_data(std::span<const std::byte>(encoded_market.data(), length));
		assert(!r && r.error() == WireError::too_short);
	}

	EthernetEnvelope envelope{};
	envelope.destination = {
		std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
	};
	envelope.source = {
		std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x02},
	};

	EthernetFrame frame{};
	assert(encode_market_data_frame(envelope, market, frame));

	for (std::size_t length = 0; length < frame.size(); ++length) {
		const auto r = decode_market_data_frame(std::span<const std::byte>(frame.data(), length));
		assert(!r && r.error() == WireError::too_short);
	}

	OutboundOrderMessage order{};
	order.sequence_number = 30;
	order.client_order_id = 40;
	order.instrument_id = 77;
	order.price_ticks = 100;
	order.quantity = 5;
	order.side = Side::sell;
	order.order_type = OrderType::limit;
	order.time_in_force = TimeInForce::immediate_or_cancel;
	order.source_sequence = 10;

	WireMessage encoded_order{};
	assert(encode_order(order, encoded_order));

	for (std::size_t length = 0; length < encoded_order.size(); ++length) {
		const auto r = decode_order(std::span<const std::byte>(encoded_order.data(), length));
		assert(!r && r.error() == WireError::too_short);
	}

	EthernetFrame order_frame{};
	assert(encode_order_frame(envelope, order, order_frame));

	for (std::size_t length = 0; length < order_frame.size(); ++length) {
		const auto r = decode_order_frame(std::span<const std::byte>(order_frame.data(), length));
		assert(!r && r.error() == WireError::too_short);
	}

	{
		auto invalid_payload = frame;
		invalid_payload[kWirePayloadOffset] = std::byte{0};

		const auto r = decode_market_data_frame(std::span<const std::byte>(invalid_payload));
		assert(!r && r.error() == WireError::invalid_magic);
	}

	{
		auto invalid_payload = order_frame;
		invalid_payload[kWirePayloadOffset + kWireHeaderBytes + 24] = std::byte{9};

		const auto r = decode_order_frame(std::span<const std::byte>(invalid_payload));
		assert(!r && r.error() == WireError::invalid_field);
	}

	std::array<std::byte, 8> bytes{};
	bytes.fill(std::byte{0xCC});

	assert(try_store_be16(std::span<std::byte>(bytes), 6, 0x1122U));
	assert(bytes[6] == std::byte{0x11});
	assert(bytes[7] == std::byte{0x22});

	const auto before_failed_store = bytes;
	assert(!try_store_be16(std::span<std::byte>(bytes), 7, 0x3344U));
	assert(bytes == before_failed_store);
	assert(!try_store_be64(std::span<std::byte>(bytes), 1, 1));
	assert(!try_store_be16(std::span<std::byte>(bytes), std::numeric_limits<std::size_t>::max(), 1));

	std::uint16_t value16 = 0xFFFFU;
	assert(try_load_be16(std::span<const std::byte>(bytes), 6, value16));
	assert(value16 == 0x1122U);
	value16 = 0xFFFFU;
	assert(!try_load_be16(std::span<const std::byte>(bytes), 7, value16));
	assert(value16 == 0xFFFFU);

	MacAddress mac{};
	mac.fill(std::byte{0x11});
	const MacAddress original_mac = mac;
	assert(!try_read_mac_address(std::span<const std::byte>(bytes), 3, mac));
	assert(mac == original_mac);
	assert(!try_write_mac_address(std::span<std::byte>(bytes), 3, mac));

	assert(try_write_mac_address(std::span<std::byte>(bytes), 2, mac));
	MacAddress decoded_mac{};
	assert(try_read_mac_address(std::span<const std::byte>(bytes), 2, decoded_mac));
	assert(decoded_mac == mac);

	return 0;
}
