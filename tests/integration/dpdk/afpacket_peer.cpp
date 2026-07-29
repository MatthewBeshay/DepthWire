#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>

#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <net/if.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "depthwire/wire/ethernet_frame.hpp"

namespace {

inline constexpr const char *kDefaultInterface = "dw_peer";
inline constexpr int kReceiveTimeoutMs = 5000;

depthwire::wire::EthernetEnvelope market_envelope() noexcept
{
	depthwire::wire::EthernetEnvelope envelope{};

	envelope.destination = {
		std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x20},
	};

	envelope.source = {
		std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x10},
	};

	return envelope;
}

depthwire::wire::MarketDataMessage make_message(const std::uint32_t sequence_number, const depthwire::wire::Side side,
						const std::uint16_t level, const std::int64_t price_ticks,
						const std::uint32_t quantity) noexcept
{
	depthwire::wire::MarketDataMessage message{};

	message.sequence_number = sequence_number;
	message.exchange_timestamp_ns = static_cast<std::uint64_t>(sequence_number) * 1000ULL;
	message.instrument_id = 77;
	message.price_ticks = price_ticks;
	message.quantity = quantity;
	message.side = side;
	message.action = depthwire::wire::UpdateAction::modify;
	message.level = level;
	message.source_flags = 0;

	return message;
}

bool transmit_message(const int socket_fd, const sockaddr_ll &destination,
		      const depthwire::wire::MarketDataMessage &message) noexcept
{
	depthwire::wire::EthernetFrame frame{};

	if (!depthwire::wire::encode_market_data_frame(market_envelope(), message, frame)) {
		return false;
	}

	const ssize_t sent = sendto(socket_fd, frame.data(), frame.size(), 0,
				    reinterpret_cast<const sockaddr *>(&destination), sizeof(destination));

	return sent == static_cast<ssize_t>(frame.size());
}

bool wait_for_order(const int socket_fd, depthwire::wire::EthernetEnvelope &output_envelope,
		    depthwire::wire::OutboundOrderMessage &output) noexcept
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kReceiveTimeoutMs);

	while (std::chrono::steady_clock::now() < deadline) {
		pollfd descriptor{};
		descriptor.fd = socket_fd;
		descriptor.events = POLLIN;

		const int poll_result = poll(&descriptor, 1, 250);

		if (poll_result < 0) {
			return false;
		}

		if (poll_result == 0) {
			continue;
		}

		std::array<std::byte, 2048> buffer{};

		const ssize_t received = recvfrom(socket_fd, buffer.data(), buffer.size(), 0, nullptr, nullptr);

		if (received <= 0) {
			return false;
		}

		const auto decode_result = depthwire::wire::decode_order_frame(
			std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(received)));

		if (decode_result) {
			output_envelope = decode_result->envelope;
			output = decode_result->message;
			return true;
		}
	}

	return false;
}

} // namespace

int main(int argc, char **argv)
{
	const char *const interface_name = argc > 1 ? argv[1] : kDefaultInterface;

	const unsigned int interface_index = if_nametoindex(interface_name);

	if (interface_index == 0) {
		std::fprintf(stderr, "Generator failure: interface %s was not found\n", interface_name);
		return EXIT_FAILURE;
	}

	const int socket_fd = socket(AF_PACKET, SOCK_RAW, htons(depthwire::wire::kEtherType));

	if (socket_fd < 0) {
		std::fprintf(stderr, "Generator failure: raw socket creation failed: %s\n", std::strerror(errno));
		return EXIT_FAILURE;
	}

	sockaddr_ll bind_address{};
	bind_address.sll_family = AF_PACKET;
	bind_address.sll_protocol = htons(depthwire::wire::kEtherType);
	bind_address.sll_ifindex = static_cast<int>(interface_index);

	if (bind(socket_fd, reinterpret_cast<const sockaddr *>(&bind_address), sizeof(bind_address)) < 0) {
		std::fprintf(stderr, "Generator failure: bind failed: %s\n", std::strerror(errno));
		close(socket_fd);
		return EXIT_FAILURE;
	}

	sockaddr_ll destination{};
	destination.sll_family = AF_PACKET;
	destination.sll_protocol = htons(depthwire::wire::kEtherType);
	destination.sll_ifindex = static_cast<int>(interface_index);
	destination.sll_halen = 6;

	const auto envelope = market_envelope();

	for (std::size_t index = 0; index < envelope.destination.size(); ++index) {
		destination.sll_addr[index] = std::to_integer<std::uint8_t>(envelope.destination[index]);
	}

	std::uint32_t sequence_number = 1;

	for (std::uint16_t level = 0; level < 5; ++level) {
		if (!transmit_message(socket_fd, destination,
				      make_message(sequence_number++, depthwire::wire::Side::sell, level,
						   100100 + level, 10))) {
			std::fprintf(stderr, "Generator failure: ASK TX failed\n");
			close(socket_fd);
			return EXIT_FAILURE;
		}
	}

	for (std::uint16_t level = 0; level < 5; ++level) {
		if (!transmit_message(socket_fd, destination,
				      make_message(sequence_number++, depthwire::wire::Side::buy, level, 100000 - level,
						   10))) {
			std::fprintf(stderr, "Generator failure: BID TX failed\n");
			close(socket_fd);
			return EXIT_FAILURE;
		}
	}

	if (!transmit_message(socket_fd, destination,
			      make_message(sequence_number, depthwire::wire::Side::buy, 0, 100000, 1000))) {
		std::fprintf(stderr, "Generator failure: trigger TX failed\n");
		close(socket_fd);
		return EXIT_FAILURE;
	}

	depthwire::wire::EthernetEnvelope response_envelope{};
	depthwire::wire::OutboundOrderMessage order{};

	if (!wait_for_order(socket_fd, response_envelope, order)) {
		std::fprintf(stderr, "Generator failure: valid outbound order not received\n");
		close(socket_fd);
		return EXIT_FAILURE;
	}

	const bool valid = response_envelope.destination == envelope.source &&
			   response_envelope.source == envelope.destination && order.sequence_number == 5001 &&
			   order.client_order_id == 1 && order.instrument_id == 77 && order.price_ticks == 100100 &&
			   order.quantity == 10 && order.side == depthwire::wire::Side::buy &&
			   order.order_type == depthwire::wire::OrderType::limit &&
			   order.time_in_force == depthwire::wire::TimeInForce::immediate_or_cancel &&
			   order.order_flags == 0 && order.source_sequence == sequence_number;

	if (!valid) {
		std::fprintf(stderr, "Generator failure: outbound order fields are incorrect\n");
		close(socket_fd);
		return EXIT_FAILURE;
	}

	std::printf("============================================================\n");
	std::printf("DepthWire - Raw Packet Generator\n");
	std::printf("============================================================\n");
	std::printf("Interface:                 %s\n", interface_name);
	std::printf("Market packets sent:       11\n");
	std::printf("Outbound orders received:  1\n");
	std::printf("Generated order side:      BUY\n");
	std::printf("Generated order price:     %lld\n", static_cast<long long>(order.price_ticks));
	std::printf("Generated order quantity:  %u\n", order.quantity);
	std::printf("Source market sequence:    %u\n", order.source_sequence);
	std::printf("Status:                    OK\n");

	close(socket_fd);
	return EXIT_SUCCESS;
}
