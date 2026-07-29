#include <cassert>
#include <cstddef>
#include <span>

#include "depthwire/wire/endian.hpp"
#include "depthwire/wire/market_data_packet.hpp"
#include "depthwire/wire/order_packet.hpp"

int main()
{
	using namespace depthwire::wire;

	MarketDataMessage market{};
	market.sequence_number = 1;
	market.exchange_timestamp_ns = 10;
	market.instrument_id = 7;
	market.price_ticks = 100;
	market.quantity = 20;
	market.side = Side::buy;
	market.action = UpdateAction::add;

	WireMessage encoded{};
	assert(encode_market_data(market, encoded));

	auto wrong_version = encoded;
	wrong_version[2] = std::byte{2};

	{
		const auto r = decode_market_data(std::span<const std::byte>(wrong_version));
		assert(!r && r.error() == WireError::unsupported_version);
	}

	auto wrong_payload_length = encoded;
	assert(try_store_be16(std::span<std::byte>(wrong_payload_length), 6, 31));

	{
		const auto r = decode_market_data(std::span<const std::byte>(wrong_payload_length));
		assert(!r && r.error() == WireError::invalid_payload_length);
	}

	auto invalid_side = encoded;
	invalid_side[kWireHeaderBytes + 24] = std::byte{9};

	{
		const auto r = decode_market_data(std::span<const std::byte>(invalid_side));
		assert(!r && r.error() == WireError::invalid_field);
	}

	{
		const auto r = decode_order(std::span<const std::byte>(encoded));
		assert(!r && r.error() == WireError::wrong_message_type);
	}

	market.wire_flags = 1;
	assert(!encode_market_data(market, encoded));
	market.wire_flags = 0;
	market.source_flags = 1;
	assert(!encode_market_data(market, encoded));
	market.source_flags = 0;
	assert(encode_market_data(market, encoded));

	auto unsupported_header_flags = encoded;
	assert(try_store_be32(std::span<std::byte>(unsupported_header_flags), 12, 1));
	{
		const auto r = decode_market_data(std::span<const std::byte>(unsupported_header_flags));
		assert(!r && r.error() == WireError::invalid_field);
	}

	auto unsupported_source_flags = encoded;
	assert(try_store_be32(std::span<std::byte>(unsupported_source_flags), kWireHeaderBytes + 28, 1));
	{
		const auto r = decode_market_data(std::span<const std::byte>(unsupported_source_flags));
		assert(!r && r.error() == WireError::invalid_field);
	}

	OutboundOrderMessage order{};
	order.client_order_id = 1;
	order.instrument_id = 7;
	order.price_ticks = 100;
	order.quantity = 1;
	order.side = Side::buy;
	order.order_type = OrderType::limit;
	order.time_in_force = TimeInForce::immediate_or_cancel;

	WireMessage encoded_order{};
	assert(encode_order(order, encoded_order));

	order.wire_flags = 1;
	assert(!encode_order(order, encoded_order));
	order.wire_flags = 0;
	order.order_flags = 1;
	assert(!encode_order(order, encoded_order));

	auto unsupported_order_flags = encoded_order;
	unsupported_order_flags[kWireHeaderBytes + 27] = std::byte{1};
	{
		const auto r = decode_order(std::span<const std::byte>(unsupported_order_flags));
		assert(!r && r.error() == WireError::invalid_field);
	}

	return 0;
}
