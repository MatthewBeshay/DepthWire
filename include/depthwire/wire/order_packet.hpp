#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>

#include "depthwire/wire/wire_header.hpp"

namespace depthwire::wire {

struct OutboundOrderMessage {
	std::uint32_t sequence_number{};
	std::uint32_t wire_flags{};
	std::uint64_t client_order_id{};
	std::uint32_t instrument_id{};
	std::int64_t price_ticks{};
	std::uint32_t quantity{};
	Side side{Side::buy};
	OrderType order_type{OrderType::limit};
	TimeInForce time_in_force{TimeInForce::immediate_or_cancel};
	std::uint8_t order_flags{};
	// Protocol version 1: sequence number of the market update that triggered
	// this order.
	std::uint32_t source_sequence{};
};

[[nodiscard]] inline std::expected<void, WireError> encode_order(const OutboundOrderMessage &order,
								 WireMessage &output) noexcept
{
	if (order.quantity == 0 || !is_valid_side(order.side) || !is_valid_order_type(order.order_type) ||
	    !is_valid_time_in_force(order.time_in_force) || (order.wire_flags & ~kHeaderFlagsMaskVersion1) != 0 ||
	    (static_cast<std::uint16_t>(order.order_flags) & ~kOrderFlagsMaskVersion1) != 0) {
		return std::unexpected(WireError::invalid_field);
	}

	output.fill(std::byte{0});

	encode_header(output, MessageType::new_order, order.sequence_number, order.wire_flags);

	auto out = std::span<std::byte>(output);
	constexpr std::size_t base = kWireHeaderBytes;

	detail::store_be64(out, base + 0, order.client_order_id);
	detail::store_be32(out, base + 8, order.instrument_id);
	detail::store_i64_be(out, base + 12, order.price_ticks);
	detail::store_be32(out, base + 20, order.quantity);
	out[base + 24] = static_cast<std::byte>(std::to_underlying(order.side));
	out[base + 25] = static_cast<std::byte>(std::to_underlying(order.order_type));
	out[base + 26] = static_cast<std::byte>(std::to_underlying(order.time_in_force));
	out[base + 27] = static_cast<std::byte>(order.order_flags);
	detail::store_be32(out, base + 28, order.source_sequence);

	return {};
}

[[nodiscard]] inline std::expected<OutboundOrderMessage, WireError>
	decode_order(const std::span<const std::byte> input) noexcept
{
	const auto header = decode_header(input, MessageType::new_order);

	if (!header) {
		return std::unexpected(header.error());
	}

	constexpr std::size_t base = kWireHeaderBytes;

	const auto side = static_cast<Side>(std::to_integer<std::uint8_t>(input[base + 24]));

	const auto order_type = static_cast<OrderType>(std::to_integer<std::uint8_t>(input[base + 25]));

	const auto time_in_force = static_cast<TimeInForce>(std::to_integer<std::uint8_t>(input[base + 26]));

	const std::uint32_t quantity = detail::load_be32(input, base + 20);
	const std::uint8_t order_flags = std::to_integer<std::uint8_t>(input[base + 27]);

	if (quantity == 0 || !is_valid_side(side) || !is_valid_order_type(order_type) ||
	    !is_valid_time_in_force(time_in_force) || (header->flags & ~kHeaderFlagsMaskVersion1) != 0 ||
	    (static_cast<std::uint16_t>(order_flags) & ~kOrderFlagsMaskVersion1) != 0) {
		return std::unexpected(WireError::invalid_field);
	}

	OutboundOrderMessage output{};
	output.sequence_number = header->sequence_number;
	output.wire_flags = header->flags;
	output.client_order_id = detail::load_be64(input, base + 0);
	output.instrument_id = detail::load_be32(input, base + 8);
	output.price_ticks = detail::load_i64_be(input, base + 12);
	output.quantity = quantity;
	output.side = side;
	output.order_type = order_type;
	output.time_in_force = time_in_force;
	output.order_flags = order_flags;
	output.source_sequence = detail::load_be32(input, base + 28);

	return output;
}

} // namespace depthwire::wire
