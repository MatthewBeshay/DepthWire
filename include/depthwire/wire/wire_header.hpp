#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>

#include "depthwire/wire/endian.hpp"
#include "depthwire/wire/wire_constants.hpp"

namespace depthwire::wire {

// Unified decode error for the whole wire stack (header, packet, and Ethernet
// frame). Success is carried by the value channel of std::expected, so there is
// no `none` member. `wrong_ether_type` only originates at the frame layer; the
// header/packet decoders never produce it.
enum class WireError : std::uint8_t {
	too_short = 1,
	wrong_ether_type,
	invalid_magic,
	unsupported_version,
	invalid_header_length,
	invalid_payload_length,
	wrong_message_type,
	invalid_field,
};

struct WireHeader {
	MessageType message_type{MessageType::market_data};
	std::uint32_t sequence_number{};
	std::uint32_t flags{};
};

inline void encode_header(WireMessage &output, const MessageType type, const std::uint32_t sequence_number,
			  const std::uint32_t flags) noexcept
{
	auto out = std::span<std::byte>(output);

	detail::store_be16(out, 0, kMagic);
	out[2] = static_cast<std::byte>(kVersion);
	out[3] = static_cast<std::byte>(std::to_underlying(type));
	detail::store_be16(out, 4, static_cast<std::uint16_t>(kWireHeaderBytes));
	detail::store_be16(out, 6, static_cast<std::uint16_t>(kPayloadBytes));
	detail::store_be32(out, 8, sequence_number);
	detail::store_be32(out, 12, flags);
}

[[nodiscard]] inline std::expected<WireHeader, WireError> decode_header(const std::span<const std::byte> input,
									const MessageType expected_type) noexcept
{
	if (input.size() < kWireMessageBytes) {
		return std::unexpected(WireError::too_short);
	}

	if (detail::load_be16(input, 0) != kMagic) {
		return std::unexpected(WireError::invalid_magic);
	}

	if (std::to_integer<std::uint8_t>(input[2]) != kVersion) {
		return std::unexpected(WireError::unsupported_version);
	}

	if (detail::load_be16(input, 4) != kWireHeaderBytes) {
		return std::unexpected(WireError::invalid_header_length);
	}

	if (detail::load_be16(input, 6) != kPayloadBytes) {
		return std::unexpected(WireError::invalid_payload_length);
	}

	const auto decoded_type = static_cast<MessageType>(std::to_integer<std::uint8_t>(input[3]));

	if (decoded_type != expected_type) {
		return std::unexpected(WireError::wrong_message_type);
	}

	WireHeader header{};
	header.message_type = decoded_type;
	header.sequence_number = detail::load_be32(input, 8);
	header.flags = detail::load_be32(input, 12);

	return header;
}

} // namespace depthwire::wire
