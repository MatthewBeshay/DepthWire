#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>

#include "depthwire/wire/wire_header.hpp"

namespace depthwire::wire {

struct MarketDataMessage {
	std::uint32_t sequence_number{};
	std::uint32_t wire_flags{};
	std::uint64_t exchange_timestamp_ns{};
	std::uint32_t instrument_id{};
	std::int64_t price_ticks{};
	std::uint32_t quantity{};
	Side side{Side::buy};
	UpdateAction action{UpdateAction::modify};
	std::uint16_t level{};
	std::uint32_t source_flags{};
};

[[nodiscard]] inline std::expected<void, WireError> encode_market_data(const MarketDataMessage &message,
								       WireMessage &output) noexcept
{
	if (!is_valid_side(message.side) || !is_valid_update_action(message.action) ||
	    (message.wire_flags & ~kHeaderFlagsMaskVersion1) != 0 ||
	    (message.source_flags & ~kMarketDataSourceFlagsMaskVersion1) != 0) {
		return std::unexpected(WireError::invalid_field);
	}

	output.fill(std::byte{0});

	encode_header(output, MessageType::market_data, message.sequence_number, message.wire_flags);

	auto out = std::span<std::byte>(output);
	constexpr std::size_t base = kWireHeaderBytes;

	detail::store_be64(out, base + 0, message.exchange_timestamp_ns);
	detail::store_be32(out, base + 8, message.instrument_id);
	detail::store_i64_be(out, base + 12, message.price_ticks);
	detail::store_be32(out, base + 20, message.quantity);
	out[base + 24] = static_cast<std::byte>(std::to_underlying(message.side));
	out[base + 25] = static_cast<std::byte>(std::to_underlying(message.action));
	detail::store_be16(out, base + 26, message.level);
	detail::store_be32(out, base + 28, message.source_flags);

	return {};
}

[[nodiscard]] inline std::expected<MarketDataMessage, WireError>
	decode_market_data(const std::span<const std::byte> input) noexcept
{
	const auto header = decode_header(input, MessageType::market_data);

	if (!header) {
		return std::unexpected(header.error());
	}

	constexpr std::size_t base = kWireHeaderBytes;

	const auto side = static_cast<Side>(std::to_integer<std::uint8_t>(input[base + 24]));

	const auto action = static_cast<UpdateAction>(std::to_integer<std::uint8_t>(input[base + 25]));

	const std::uint32_t source_flags = detail::load_be32(input, base + 28);

	if (!is_valid_side(side) || !is_valid_update_action(action) ||
	    (header->flags & ~kHeaderFlagsMaskVersion1) != 0 ||
	    (source_flags & ~kMarketDataSourceFlagsMaskVersion1) != 0) {
		return std::unexpected(WireError::invalid_field);
	}

	MarketDataMessage output{};
	output.sequence_number = header->sequence_number;
	output.wire_flags = header->flags;
	output.exchange_timestamp_ns = detail::load_be64(input, base + 0);
	output.instrument_id = detail::load_be32(input, base + 8);
	output.price_ticks = detail::load_i64_be(input, base + 12);
	output.quantity = detail::load_be32(input, base + 20);
	output.side = side;
	output.action = action;
	output.level = detail::load_be16(input, base + 26);
	output.source_flags = source_flags;

	return output;
}

} // namespace depthwire::wire
