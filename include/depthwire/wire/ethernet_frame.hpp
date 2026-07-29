#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>

#include "depthwire/wire/endian.hpp"
#include "depthwire/wire/market_data_packet.hpp"
#include "depthwire/wire/order_packet.hpp"
#include "depthwire/wire/wire_constants.hpp"

namespace depthwire::wire {

inline constexpr std::size_t kMacAddressBytes = 6;
inline constexpr std::size_t kDestinationMacOffset = 0;
inline constexpr std::size_t kSourceMacOffset = 6;
inline constexpr std::size_t kEtherTypeOffset = 12;
inline constexpr std::size_t kWirePayloadOffset = kEthernetHeaderBytes;

using MacAddress = std::array<std::byte, kMacAddressBytes>;
using EthernetFrame = std::array<std::byte, kEthernetFrameBytesWithoutFcs>;

struct EthernetEnvelope {
	MacAddress destination{};
	MacAddress source{};
};

// Value payloads returned by the frame decoders: the parsed envelope bundled
// with the decoded wire message. Both are trivially copyable so that the
// enclosing std::expected stays trivially copyable and allocation-free.
struct DecodedMarketFrame {
	EthernetEnvelope envelope{};
	MarketDataMessage message{};
};

struct DecodedOrderFrame {
	EthernetEnvelope envelope{};
	OutboundOrderMessage message{};
};

static_assert(std::is_trivially_copyable_v<DecodedMarketFrame>);
static_assert(std::is_trivially_copyable_v<DecodedOrderFrame>);

namespace detail {

inline void write_mac_address(const std::span<std::byte> output, const std::size_t offset,
			      const MacAddress &address) noexcept
{
	for (std::size_t index = 0; index < kMacAddressBytes; ++index) {
		output[offset + index] = address[index];
	}
}

[[nodiscard]] inline MacAddress read_mac_address(const std::span<const std::byte> input,
						 const std::size_t offset) noexcept
{
	MacAddress address{};

	for (std::size_t index = 0; index < kMacAddressBytes; ++index) {
		address[index] = input[offset + index];
	}

	return address;
}

} // namespace detail

[[nodiscard]] inline bool try_write_mac_address(const std::span<std::byte> output, const std::size_t offset,
						const MacAddress &address) noexcept
{
	if (!contains_bytes(output.size(), offset, kMacAddressBytes)) {
		return false;
	}

	detail::write_mac_address(output, offset, address);
	return true;
}

[[nodiscard]] inline bool try_read_mac_address(const std::span<const std::byte> input, const std::size_t offset,
					       MacAddress &output) noexcept
{
	if (!contains_bytes(input.size(), offset, kMacAddressBytes)) {
		return false;
	}

	output = detail::read_mac_address(input, offset);
	return true;
}

inline void encode_ethernet_header(const EthernetEnvelope &envelope, EthernetFrame &output) noexcept
{
	auto frame = std::span<std::byte>(output);

	detail::write_mac_address(frame, kDestinationMacOffset, envelope.destination);
	detail::write_mac_address(frame, kSourceMacOffset, envelope.source);
	detail::store_be16(frame, kEtherTypeOffset, kEtherType);
}

[[nodiscard]] inline std::expected<EthernetEnvelope, WireError>
	decode_ethernet_header(const std::span<const std::byte> input) noexcept
{
	if (input.size() < kEthernetFrameBytesWithoutFcs) {
		return std::unexpected(WireError::too_short);
	}

	if (detail::load_be16(input, kEtherTypeOffset) != kEtherType) {
		return std::unexpected(WireError::wrong_ether_type);
	}

	EthernetEnvelope output{};
	output.destination = detail::read_mac_address(input, kDestinationMacOffset);
	output.source = detail::read_mac_address(input, kSourceMacOffset);

	return output;
}

[[nodiscard]] inline std::expected<void, WireError> encode_market_data_frame(const EthernetEnvelope &envelope,
									     const MarketDataMessage &message,
									     EthernetFrame &output) noexcept
{
	WireMessage wire_message{};

	const auto encoded = encode_market_data(message, wire_message);

	if (!encoded) {
		return std::unexpected(encoded.error());
	}

	output.fill(std::byte{0});
	encode_ethernet_header(envelope, output);

	std::copy(wire_message.begin(), wire_message.end(), output.begin() + kWirePayloadOffset);

	return {};
}

// Composes envelope and payload decoding through the shared WireError channel,
// so no error translation between layers is needed.
[[nodiscard]] inline std::expected<DecodedMarketFrame, WireError>
	decode_market_data_frame(const std::span<const std::byte> input) noexcept
{
	return decode_ethernet_header(input).and_then(
		[input](const EthernetEnvelope envelope) -> std::expected<DecodedMarketFrame, WireError> {
			return decode_market_data(input.subspan(kWirePayloadOffset, kWireMessageBytes))
				.transform([envelope](const MarketDataMessage message) {
					return DecodedMarketFrame{envelope, message};
				});
		});
}

[[nodiscard]] inline std::expected<void, WireError> encode_order_frame(const EthernetEnvelope &envelope,
								       const OutboundOrderMessage &order,
								       EthernetFrame &output) noexcept
{
	WireMessage wire_message{};

	const auto encoded = encode_order(order, wire_message);

	if (!encoded) {
		return std::unexpected(encoded.error());
	}

	output.fill(std::byte{0});
	encode_ethernet_header(envelope, output);

	std::copy(wire_message.begin(), wire_message.end(), output.begin() + kWirePayloadOffset);

	return {};
}

[[nodiscard]] inline std::expected<DecodedOrderFrame, WireError>
	decode_order_frame(const std::span<const std::byte> input) noexcept
{
	return decode_ethernet_header(input).and_then(
		[input](const EthernetEnvelope envelope) -> std::expected<DecodedOrderFrame, WireError> {
			return decode_order(input.subspan(kWirePayloadOffset, kWireMessageBytes))
				.transform([envelope](const OutboundOrderMessage message) {
					return DecodedOrderFrame{envelope, message};
				});
		});
}

} // namespace depthwire::wire
