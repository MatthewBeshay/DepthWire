#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

#include "depthwire/wire/ethernet_frame.hpp"
#include "depthwire/wire/market_data_packet.hpp"
#include "depthwire/wire/order_packet.hpp"

namespace {

void check_market_data_round_trip(const std::span<const std::byte> input) noexcept
{
	const auto decoded_result = depthwire::wire::decode_market_data(input);
	if (!decoded_result) {
		return;
	}
	const auto &decoded = *decoded_result;

	depthwire::wire::WireMessage encoded{};
	assert(depthwire::wire::encode_market_data(decoded, encoded));
	assert(input.size() >= encoded.size());

	for (std::size_t index = 0; index < encoded.size(); ++index) {
		assert(encoded[index] == input[index]);
	}
}

void check_order_round_trip(const std::span<const std::byte> input) noexcept
{
	const auto decoded_result = depthwire::wire::decode_order(input);
	if (!decoded_result) {
		return;
	}
	const auto &decoded = *decoded_result;

	depthwire::wire::WireMessage encoded{};
	assert(depthwire::wire::encode_order(decoded, encoded));
	assert(input.size() >= encoded.size());

	for (std::size_t index = 0; index < encoded.size(); ++index) {
		assert(encoded[index] == input[index]);
	}
}

void check_market_frame_round_trip(const std::span<const std::byte> input) noexcept
{
	const auto result = depthwire::wire::decode_market_data_frame(input);
	if (!result) {
		return;
	}
	const auto &envelope = result->envelope;
	const auto &decoded = result->message;

	depthwire::wire::EthernetFrame encoded{};
	assert(depthwire::wire::encode_market_data_frame(envelope, decoded, encoded));
	assert(input.size() >= encoded.size());

	for (std::size_t index = 0; index < encoded.size(); ++index) {
		assert(encoded[index] == input[index]);
	}
}

void check_order_frame_round_trip(const std::span<const std::byte> input) noexcept
{
	const auto result = depthwire::wire::decode_order_frame(input);
	if (!result) {
		return;
	}
	const auto &envelope = result->envelope;
	const auto &decoded = result->message;

	depthwire::wire::EthernetFrame encoded{};
	assert(depthwire::wire::encode_order_frame(envelope, decoded, encoded));
	assert(input.size() >= encoded.size());

	for (std::size_t index = 0; index < encoded.size(); ++index) {
		assert(encoded[index] == input[index]);
	}
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, const std::size_t size)
{
	const auto input = std::as_bytes(std::span<const std::uint8_t>(data, size));

	check_market_data_round_trip(input);
	check_order_round_trip(input);
	check_market_frame_round_trip(input);
	check_order_frame_round_trip(input);

	return 0;
}
