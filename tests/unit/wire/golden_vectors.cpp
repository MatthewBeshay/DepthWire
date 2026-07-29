#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

#include "depthwire/wire/market_data_packet.hpp"
#include "depthwire/wire/order_packet.hpp"

namespace {

constexpr std::byte byte(const unsigned int value) noexcept
{
	return static_cast<std::byte>(value);
}

} // namespace

int main()
{
	using namespace depthwire::wire;

	static_assert(kProtocolVersion1 == 1);
	static_assert(kVersion == kProtocolVersion1);

	{
		MarketDataMessage message{};
		message.sequence_number = 0x01020304U;
		message.wire_flags = 0;
		message.exchange_timestamp_ns = 0x0102030405060708ULL;
		message.instrument_id = 0x11223344U;
		message.price_ticks = -2;
		message.quantity = 0x01020304U;
		message.side = Side::buy;
		message.action = UpdateAction::modify;
		message.level = 3;
		message.source_flags = 0;

		const WireMessage expected{
			byte(0x50), byte(0x42), byte(0x01), byte(0x01), byte(0x00), byte(0x10), byte(0x00), byte(0x20),
			byte(0x01), byte(0x02), byte(0x03), byte(0x04), byte(0x00), byte(0x00), byte(0x00), byte(0x00),
			byte(0x01), byte(0x02), byte(0x03), byte(0x04), byte(0x05), byte(0x06), byte(0x07), byte(0x08),
			byte(0x11), byte(0x22), byte(0x33), byte(0x44), byte(0xFF), byte(0xFF), byte(0xFF), byte(0xFF),
			byte(0xFF), byte(0xFF), byte(0xFF), byte(0xFE), byte(0x01), byte(0x02), byte(0x03), byte(0x04),
			byte(0x01), byte(0x02), byte(0x00), byte(0x03), byte(0x00), byte(0x00), byte(0x00), byte(0x00),
		};

		WireMessage encoded{};
		assert(encode_market_data(message, encoded));
		assert(encoded == expected);

		const auto decoded_result = decode_market_data(std::span<const std::byte>(expected));
		assert(decoded_result);
		const auto &decoded = *decoded_result;
		assert(decoded.sequence_number == message.sequence_number);
		assert(decoded.wire_flags == message.wire_flags);
		assert(decoded.exchange_timestamp_ns == message.exchange_timestamp_ns);
		assert(decoded.instrument_id == message.instrument_id);
		assert(decoded.price_ticks == message.price_ticks);
		assert(decoded.quantity == message.quantity);
		assert(decoded.side == message.side);
		assert(decoded.action == message.action);
		assert(decoded.level == message.level);
		assert(decoded.source_flags == message.source_flags);
	}

	{
		OutboundOrderMessage order{};
		order.sequence_number = 0x0A0B0C0DU;
		order.wire_flags = 0;
		order.client_order_id = 0x1122334455667788ULL;
		order.instrument_id = 0x99AABBCCU;
		order.price_ticks = 0x0102030405060708LL;
		order.quantity = 25;
		order.side = Side::sell;
		order.order_type = OrderType::limit;
		order.time_in_force = TimeInForce::immediate_or_cancel;
		order.order_flags = 0;
		order.source_sequence = 0xDEADBEEFU;

		const WireMessage expected{
			byte(0x50), byte(0x42), byte(0x01), byte(0x02), byte(0x00), byte(0x10), byte(0x00), byte(0x20),
			byte(0x0A), byte(0x0B), byte(0x0C), byte(0x0D), byte(0x00), byte(0x00), byte(0x00), byte(0x00),
			byte(0x11), byte(0x22), byte(0x33), byte(0x44), byte(0x55), byte(0x66), byte(0x77), byte(0x88),
			byte(0x99), byte(0xAA), byte(0xBB), byte(0xCC), byte(0x01), byte(0x02), byte(0x03), byte(0x04),
			byte(0x05), byte(0x06), byte(0x07), byte(0x08), byte(0x00), byte(0x00), byte(0x00), byte(0x19),
			byte(0x02), byte(0x01), byte(0x01), byte(0x00), byte(0xDE), byte(0xAD), byte(0xBE), byte(0xEF),
		};

		WireMessage encoded{};
		assert(encode_order(order, encoded));
		assert(encoded == expected);

		const auto decoded_result = decode_order(std::span<const std::byte>(expected));
		assert(decoded_result);
		const auto &decoded = *decoded_result;
		assert(decoded.sequence_number == order.sequence_number);
		assert(decoded.wire_flags == order.wire_flags);
		assert(decoded.client_order_id == order.client_order_id);
		assert(decoded.instrument_id == order.instrument_id);
		assert(decoded.price_ticks == order.price_ticks);
		assert(decoded.quantity == order.quantity);
		assert(decoded.side == order.side);
		assert(decoded.order_type == order.order_type);
		assert(decoded.time_in_force == order.time_in_force);
		assert(decoded.order_flags == order.order_flags);
		assert(decoded.source_sequence == order.source_sequence);
	}

	return 0;
}
