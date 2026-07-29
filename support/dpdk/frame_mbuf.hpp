#pragma once

#include <cstdint>
#include <cstring>

#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include "depthwire/wire/ethernet_frame.hpp"

namespace depthwire::dpdk {

// Copies an encoded Ethernet frame into a freshly allocated mbuf; returns
// nullptr when the pool is exhausted or the mbuf cannot hold the frame.
[[nodiscard]] inline rte_mbuf *frame_to_mbuf(rte_mempool *const pool, const wire::EthernetFrame &frame) noexcept
{
	rte_mbuf *const packet = rte_pktmbuf_alloc(pool);

	if (packet == nullptr) {
		return nullptr;
	}

	void *const destination = rte_pktmbuf_append(packet, static_cast<std::uint16_t>(frame.size()));

	if (destination == nullptr) {
		rte_pktmbuf_free(packet);
		return nullptr;
	}

	std::memcpy(destination, frame.data(), frame.size());
	return packet;
}

// The paced harness's outbound-order transport branch: allocate, copy, and
// transmit the encoded order frame, freeing the mbuf on allocation failure or
// TX rejection. Returns whether the transport accepted the order. Shared with
// the recovery integration test so failure-injection tests exercise exactly
// this production path.
[[nodiscard]] inline bool transmit_order_frame(rte_mempool *const pool, const std::uint16_t port,
					       const wire::EthernetFrame &frame) noexcept
{
	rte_mbuf *packet = frame_to_mbuf(pool, frame);

	if (packet == nullptr) {
		return false;
	}

	if (rte_eth_tx_burst(port, 0, &packet, 1) != 1) {
		rte_pktmbuf_free(packet);
		return false;
	}

	return true;
}

} // namespace depthwire::dpdk
