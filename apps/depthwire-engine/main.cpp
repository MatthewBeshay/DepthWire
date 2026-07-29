#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <print>
#include <span>

#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_pause.h>
#include <rte_timer.h>
#include <rte_version.h>

#include "depthwire/pipeline/packet_processor.hpp"
#include "depthwire/wire/ethernet_frame.hpp"

namespace {

inline constexpr std::uint16_t kRxQueues = 1;
inline constexpr std::uint16_t kTxQueues = 1;
inline constexpr std::uint16_t kRxDescriptors = 512;
inline constexpr std::uint16_t kTxDescriptors = 512;
inline constexpr std::uint32_t kMempoolSize = 4095;
inline constexpr std::uint16_t kDataRoomSize = 4096;
inline constexpr std::uint16_t kBurstSize = 8;
inline constexpr std::uint64_t kTimeoutSeconds = 30;
inline constexpr std::uint64_t kTargetOrders = 1;
inline constexpr const char *kMempoolName = "dw_amp";
inline constexpr const char *kAfPacketDriverName = "net_af_packet";

struct Counters {
	std::uint64_t received_packets{};
	std::uint64_t valid_market_packets{};
	std::uint64_t invalid_packets{};
	std::uint64_t no_signal_events{};
	std::uint64_t orders_emitted{};
	std::uint64_t risk_rejected{};
	std::uint64_t outbox_full{};
};

struct AfPacketPort {
	std::uint16_t id{};
	std::array<char, RTE_ETH_NAME_MAX_LEN> name{};
};

void print_port_error(const char *operation, const int result) noexcept
{
	std::println(stderr, "{} failed: {}", operation, rte_strerror(result < 0 ? -result : result));
}

bool select_af_packet_port(AfPacketPort &output) noexcept
{
	bool found = false;
	std::uint16_t current_port{};

	RTE_ETH_FOREACH_DEV(current_port)
	{
		rte_eth_dev_info information{};
		const int information_result = rte_eth_dev_info_get(current_port, &information);

		if (information_result < 0) {
			print_port_error("rte_eth_dev_info_get", information_result);
			return false;
		}

		if (information.driver_name == nullptr ||
		    std::strcmp(information.driver_name, kAfPacketDriverName) != 0) {
			continue;
		}

		if (found) {
			std::println(stderr, "Multiple AF_PACKET DPDK ports are available; expose exactly "
					     "one port with --no-pci and one --vdev argument");
			return false;
		}

		output.id = current_port;

		const int name_result = rte_eth_dev_get_name_by_port(current_port, output.name.data());

		if (name_result < 0) {
			print_port_error("rte_eth_dev_get_name_by_port", name_result);
			return false;
		}

		found = true;
	}

	if (!found) {
		std::println(stderr, "No net_af_packet DPDK port found. Use --no-pci and "
				     "--vdev=eth_af_packet0,iface=dw_eng,qpairs=1");
	}

	return found;
}

depthwire::pipeline::PacketProcessorConfig make_processor_config(const rte_ether_addr &local_mac) noexcept
{
	depthwire::pipeline::PacketProcessorConfig config{};

	config.engine.instrument_id = 77;
	config.engine.strategy.imbalance_threshold_bps = 6000;
	config.engine.strategy.order_quantity = 10;

	config.engine.risk.max_order_quantity = 100;
	config.engine.risk.max_absolute_position = 1000;
	config.engine.risk.max_notional_ticks = 1'000'000'000ULL;
	config.engine.risk.max_outstanding_orders = 64;
	config.first_output_sequence = 5001;

	for (std::size_t index = 0; index < config.local_source.size(); ++index) {
		config.local_source[index] = std::byte{local_mac.addr_bytes[index]};
	}

	return config;
}

bool configure_port(const std::uint16_t port_id, rte_mempool *const mempool) noexcept
{
	rte_eth_conf configuration{};

	int result = rte_eth_dev_configure(port_id, kRxQueues, kTxQueues, &configuration);

	if (result < 0) {
		print_port_error("rte_eth_dev_configure", result);
		return false;
	}

	result = rte_eth_rx_queue_setup(port_id, 0, kRxDescriptors, rte_socket_id(), nullptr, mempool);

	if (result < 0) {
		print_port_error("rte_eth_rx_queue_setup", result);
		return false;
	}

	result = rte_eth_tx_queue_setup(port_id, 0, kTxDescriptors, rte_socket_id(), nullptr);

	if (result < 0) {
		print_port_error("rte_eth_tx_queue_setup", result);
		return false;
	}

	result = rte_eth_dev_start(port_id);

	if (result < 0) {
		print_port_error("rte_eth_dev_start", result);
		return false;
	}

	return true;
}

bool transmit_frame(const std::uint16_t port_id, rte_mempool *const mempool,
		    const depthwire::wire::EthernetFrame &frame) noexcept
{
	rte_mbuf *const packet = rte_pktmbuf_alloc(mempool);

	if (packet == nullptr) {
		return false;
	}

	void *const payload = rte_pktmbuf_append(packet, static_cast<std::uint16_t>(frame.size()));

	if (payload == nullptr) {
		rte_pktmbuf_free(packet);
		return false;
	}

	std::memcpy(payload, frame.data(), frame.size());

	rte_mbuf *packets[] = {packet};

	if (rte_eth_tx_burst(port_id, 0, packets, 1) != 1) {
		rte_pktmbuf_free(packet);
		return false;
	}

	return true;
}

} // namespace

int main(int argc, char **argv)
{
	if (rte_eal_init(argc, argv) < 0) {
		std::println(stderr, "EAL initialisation failed");
		return EXIT_FAILURE;
	}

	const std::uint16_t available_ports = rte_eth_dev_count_avail();

	if (available_ports == 0) {
		std::println(stderr, "No DPDK port found. Start this executable with "
				     "--vdev=eth_af_packet0,iface=dw_eng,qpairs=1");
		rte_eal_cleanup();
		return EXIT_FAILURE;
	}

	AfPacketPort selected_port{};

	if (!select_af_packet_port(selected_port)) {
		rte_eal_cleanup();
		return EXIT_FAILURE;
	}

	const std::uint16_t port_id = selected_port.id;

	rte_mempool *const mempool =
		rte_pktmbuf_pool_create(kMempoolName, kMempoolSize, 0, 0, kDataRoomSize, rte_socket_id());

	if (mempool == nullptr) {
		std::println(stderr, "Mempool creation failed: {}", rte_strerror(rte_errno));
		rte_eal_cleanup();
		return EXIT_FAILURE;
	}

	if (!configure_port(port_id, mempool)) {
		rte_eth_dev_close(port_id);
		rte_mempool_free(mempool);
		rte_eal_cleanup();
		return EXIT_FAILURE;
	}

	rte_ether_addr local_mac{};
	const int mac_result = rte_eth_macaddr_get(port_id, &local_mac);

	if (mac_result < 0) {
		print_port_error("rte_eth_macaddr_get", mac_result);
		rte_eth_dev_stop(port_id);
		rte_eth_dev_close(port_id);
		rte_mempool_free(mempool);
		rte_eal_cleanup();
		return EXIT_FAILURE;
	}

	depthwire::pipeline::PacketProcessor<64, kBurstSize> processor(make_processor_config(local_mac));

	if (!processor.configuration_valid()) {
		std::println(stderr, "Packet processor configuration is invalid; "
				     "the selected port must expose a nonzero unicast MAC");
		rte_eth_dev_stop(port_id);
		rte_eth_dev_close(port_id);
		rte_mempool_free(mempool);
		rte_eal_cleanup();
		return EXIT_FAILURE;
	}

	Counters counters{};
	bool success = true;

	std::println("============================================================");
	std::println("DepthWire - Bounded DPDK AF_PACKET Validation Processor");
	std::println("============================================================");
	std::println("DPDK version:              {}", rte_version());
	std::println("Main lcore:                {}", rte_lcore_id());
	std::println("Available DPDK ports:      {}", available_ports);
	std::println("Selected port:             {}", port_id);
	std::println("Selected device:           {}", selected_port.name.data());
	std::println("Selected driver:           {}", kAfPacketDriverName);
	std::println(
		"Local source MAC:          {:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
		static_cast<unsigned int>(local_mac.addr_bytes[0]), static_cast<unsigned int>(local_mac.addr_bytes[1]),
		static_cast<unsigned int>(local_mac.addr_bytes[2]), static_cast<unsigned int>(local_mac.addr_bytes[3]),
		static_cast<unsigned int>(local_mac.addr_bytes[4]), static_cast<unsigned int>(local_mac.addr_bytes[5]));
	std::println("Target orders:             {}", kTargetOrders);
	std::println("Classification:            Kernel-backed DPDK AF_PACKET");
	std::println("Waiting for packets...");
	std::fflush(stdout);

	const std::uint64_t deadline = rte_get_timer_cycles() + rte_get_timer_hz() * kTimeoutSeconds;

	while (success && counters.orders_emitted < kTargetOrders && rte_get_timer_cycles() < deadline) {
		rte_mbuf *packets[kBurstSize]{};

		const std::uint16_t received = rte_eth_rx_burst(port_id, 0, packets, kBurstSize);

		if (received == 0) {
			rte_pause();
			continue;
		}

		for (std::uint16_t index = 0; index < received; ++index) {
			rte_mbuf *const packet = packets[index];
			++counters.received_packets;

			if (packet->nb_segs != 1 || rte_pktmbuf_pkt_len(packet) != rte_pktmbuf_data_len(packet)) {
				rte_pktmbuf_free(packet);
				++counters.invalid_packets;
				continue;
			}

			const auto *const data = rte_pktmbuf_mtod(packet, const std::byte *);

			const auto bytes = std::span<const std::byte>(
				data, static_cast<std::size_t>(rte_pktmbuf_data_len(packet)));

			depthwire::wire::EthernetFrame output_frame{};
			const auto result = processor.process(bytes, output_frame);

			rte_pktmbuf_free(packet);

			if (!result.decode_error.has_value() &&
			    result.input_adapter_status == depthwire::pipeline::EngineWireAdapterStatus::converted) {
				++counters.valid_market_packets;
			}

			switch (result.status) {
			case depthwire::pipeline::PacketProcessStatus::no_order:
				++counters.no_signal_events;
				break;

			case depthwire::pipeline::PacketProcessStatus::order_encoded:
				if (!result.output_ready || !transmit_frame(port_id, mempool, output_frame)) {
					success = false;
					break;
				}

				++counters.orders_emitted;
				break;

			case depthwire::pipeline::PacketProcessStatus::decode_rejected:
			case depthwire::pipeline::PacketProcessStatus::invalid_input_destination:
			case depthwire::pipeline::PacketProcessStatus::invalid_input_source:
			case depthwire::pipeline::PacketProcessStatus::input_adapter_rejected:
				++counters.invalid_packets;
				break;

			case depthwire::pipeline::PacketProcessStatus::engine_rejected:
				switch (result.engine_result.status) {
				case depthwire::EngineStatus::risk_rejected:
					++counters.risk_rejected;
					break;

				case depthwire::EngineStatus::outbox_full:
					++counters.outbox_full;
					success = false;
					break;

				case depthwire::EngineStatus::invalid_update:
					++counters.invalid_packets;
					break;

				case depthwire::EngineStatus::invalid_configuration:
				case depthwire::EngineStatus::duplicate_update:
				case depthwire::EngineStatus::sequence_gap:
				case depthwire::EngineStatus::stale_update:
				case depthwire::EngineStatus::no_order:
				case depthwire::EngineStatus::order_emitted:
					++counters.invalid_packets;
					success = false;
					break;
				}
				break;

			case depthwire::pipeline::PacketProcessStatus::invalid_configuration:
			case depthwire::pipeline::PacketProcessStatus::output_queue_invariant:
			case depthwire::pipeline::PacketProcessStatus::output_adapter_rejected:
			case depthwire::pipeline::PacketProcessStatus::output_encode_rejected:
				++counters.invalid_packets;
				success = false;
				break;
			}

			if (!success || counters.orders_emitted >= kTargetOrders) {
				for (std::uint16_t remaining = index + 1; remaining < received; ++remaining) {
					rte_pktmbuf_free(packets[remaining]);
				}

				break;
			}
		}
	}

	if (counters.orders_emitted != kTargetOrders) {
		std::println(stderr, "Expected {} outbound order(s), observed {}", kTargetOrders,
			     counters.orders_emitted);
		success = false;
	}

	rte_eth_stats stats{};
	rte_eth_stats_get(port_id, &stats);

	if (success) {
		std::println("\nAF_PACKET engine result:");
		std::println("  Received packets:         {}", counters.received_packets);
		std::println("  Valid market packets:     {}", counters.valid_market_packets);
		std::println("  No-signal events:         {}", counters.no_signal_events);
		std::println("  Orders emitted:           {}", counters.orders_emitted);
		std::println("  Invalid packets:          {}", counters.invalid_packets);
		std::println("  Risk rejected:            {}", counters.risk_rejected);
		std::println("  PMD RX packets:           {}", stats.ipackets);
		std::println("  PMD TX packets:           {}", stats.opackets);
		std::println("");
		std::println("This is a bounded kernel-backed AF_PACKET functional path.");
		std::println("It exits after the configured target order count.");
		std::println("It is not VFIO physical-NIC bypass latency.");
		std::println("\nStatus:                    OK");
	}

	const int stop_result = rte_eth_dev_stop(port_id);
	if (stop_result < 0) {
		print_port_error("rte_eth_dev_stop", stop_result);
		success = false;
	}

	const int close_result = rte_eth_dev_close(port_id);
	if (close_result < 0) {
		print_port_error("rte_eth_dev_close", close_result);
		success = false;
	}

	rte_mempool_free(mempool);

	const int cleanup_result = rte_eal_cleanup();

	if (!success || cleanup_result != 0) {
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
