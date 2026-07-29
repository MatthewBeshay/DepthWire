#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace depthwire::wire {

inline constexpr std::uint16_t kEtherType = 0x88B5;
inline constexpr std::uint16_t kMagic = 0x5042;
inline constexpr std::uint8_t kProtocolVersion1 = 1;
inline constexpr std::uint8_t kVersion = kProtocolVersion1;
// Version 1 defines no flag bits. A later meaning requires a new protocol
// version rather than silently changing the interpretation of accepted bytes.
inline constexpr std::uint32_t kHeaderFlagsMaskVersion1 = 0;
inline constexpr std::uint32_t kMarketDataSourceFlagsMaskVersion1 = 0;
inline constexpr std::uint16_t kOrderFlagsMaskVersion1 = 0;

inline constexpr std::size_t kWireHeaderBytes = 16;
inline constexpr std::size_t kPayloadBytes = 32;
inline constexpr std::size_t kWireMessageBytes = kWireHeaderBytes + kPayloadBytes;
inline constexpr std::size_t kEthernetHeaderBytes = 14;
inline constexpr std::size_t kEthernetFrameBytesWithoutFcs = kEthernetHeaderBytes + kWireMessageBytes;

enum class MessageType : std::uint8_t {
	invalid = 0,
	market_data = 1,
	new_order = 2,
};

enum class Side : std::uint8_t {
	invalid = 0,
	buy = 1,
	sell = 2,
};

enum class UpdateAction : std::uint8_t {
	invalid = 0,
	add = 1,
	modify = 2,
	erase = 3,
};

enum class OrderType : std::uint8_t {
	invalid = 0,
	limit = 1,
};

enum class TimeInForce : std::uint8_t {
	invalid = 0,
	immediate_or_cancel = 1,
};

using WireMessage = std::array<std::byte, kWireMessageBytes>;

constexpr bool is_valid_side(const Side value) noexcept
{
	return value == Side::buy || value == Side::sell;
}

constexpr bool is_valid_update_action(const UpdateAction value) noexcept
{
	return value == UpdateAction::add || value == UpdateAction::modify || value == UpdateAction::erase;
}

constexpr bool is_valid_order_type(const OrderType value) noexcept
{
	return value == OrderType::limit;
}

constexpr bool is_valid_time_in_force(const TimeInForce value) noexcept
{
	return value == TimeInForce::immediate_or_cancel;
}

} // namespace depthwire::wire
