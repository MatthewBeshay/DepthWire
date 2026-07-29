#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "depthwire/engine/trading_engine.hpp"
#include "depthwire/pipeline/engine_wire_adapter.hpp"
#include "depthwire/wire/ethernet_frame.hpp"

namespace depthwire::pipeline {

enum class PacketProcessStatus : std::uint8_t {
	no_order = 0,
	order_encoded,
	invalid_configuration,
	decode_rejected,
	invalid_input_destination,
	invalid_input_source,
	input_adapter_rejected,
	engine_rejected,
	output_queue_invariant,
	output_adapter_rejected,
	output_encode_rejected,
};

struct PacketProcessResult {
	PacketProcessStatus status{PacketProcessStatus::invalid_configuration};
	std::optional<wire::WireError> decode_error{};
	EngineWireAdapterStatus input_adapter_status{EngineWireAdapterStatus::not_attempted};
	EngineResult engine_result{};
	EngineWireAdapterStatus output_adapter_status{EngineWireAdapterStatus::not_attempted};
	std::uint32_t output_sequence_number{};
	bool output_ready{};
};

enum class BoundedBurstStatus : std::uint8_t {
	complete = 0,
	input_truncated,
	insufficient_output_slots,
};

template<std::size_t MaxBurst> struct BoundedBurstResult {
	static_assert(MaxBurst > 0);

	BoundedBurstStatus status{BoundedBurstStatus::complete};
	std::array<PacketProcessResult, MaxBurst> packets{};
	std::size_t processed{};
	std::size_t orders_encoded{};
};

struct PacketProcessorConfig {
	EngineConfig engine{};
	wire::MacAddress local_source{
		std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
	};
	std::uint32_t first_output_sequence{1};
};

[[nodiscard]] constexpr bool is_valid_unicast_source(const wire::MacAddress &address) noexcept
{
	bool has_nonzero_octet = false;

	for (const std::byte octet : address) {
		has_nonzero_octet = has_nonzero_octet || std::to_integer<std::uint8_t>(octet) != 0;
	}

	return has_nonzero_octet && (std::to_integer<std::uint8_t>(address[0]) & 0x01U) == 0;
}

// Allocation-free, thread-confined wire-to-engine endpoint. The output
// destination is the validated input source; the configured local address is
// always used as the output source.
template<std::size_t OutboxCapacity = 64, std::size_t MaxBurst = 32> class PacketProcessor {
public:
	static_assert(OutboxCapacity > 0);
	static_assert(MaxBurst > 0);

	explicit PacketProcessor(const PacketProcessorConfig config = {}) noexcept
		: config_(config), engine_(config.engine), outbound_sequence_(config.first_output_sequence)
	{
	}

	PacketProcessor(const PacketProcessor &) = delete;
	PacketProcessor &operator=(const PacketProcessor &) = delete;
	PacketProcessor(PacketProcessor &&) = delete;
	PacketProcessor &operator=(PacketProcessor &&) = delete;

	[[nodiscard]] bool configuration_valid() const noexcept
	{
		return engine_.configuration_valid() && is_valid_unicast_source(config_.local_source);
	}

	[[nodiscard]] PacketProcessResult process(const std::span<const std::byte> input,
						  wire::EthernetFrame &output) noexcept
	{
		PacketProcessResult result{};

		if (!configuration_valid()) {
			result.status = PacketProcessStatus::invalid_configuration;
			result.engine_result.status = EngineStatus::invalid_configuration;
			return result;
		}

		const auto decoded = wire::decode_market_data_frame(input);

		if (!decoded) {
			result.decode_error = decoded.error();
			result.status = PacketProcessStatus::decode_rejected;
			return result;
		}

		const wire::EthernetEnvelope &input_envelope = decoded->envelope;

		if (input_envelope.destination != config_.local_source) {
			result.status = PacketProcessStatus::invalid_input_destination;
			return result;
		}

		if (!is_valid_unicast_source(input_envelope.source)) {
			result.status = PacketProcessStatus::invalid_input_source;
			return result;
		}

		const auto update = EngineWireAdapter::to_market_update(decoded->message);

		if (!update) {
			result.input_adapter_status = update.error();
			result.status = PacketProcessStatus::input_adapter_rejected;
			return result;
		}

		result.input_adapter_status = EngineWireAdapterStatus::converted;
		result.engine_result = engine_.on_market_update(*update);

		if (result.engine_result.status == EngineStatus::no_order) {
			result.status = PacketProcessStatus::no_order;
			return result;
		}

		if (result.engine_result.status != EngineStatus::order_emitted) {
			result.status = PacketProcessStatus::engine_rejected;
			return result;
		}

		OrderRequest order{};

		if (!engine_.pop_order(order)) {
			result.status = PacketProcessStatus::output_queue_invariant;
			return result;
		}

		result.output_sequence_number = outbound_sequence_;
		const auto wire_order = EngineWireAdapter::to_wire_order(order, outbound_sequence_);

		if (!wire_order) {
			result.output_adapter_status = wire_order.error();
			result.status = PacketProcessStatus::output_adapter_rejected;
			return result;
		}

		result.output_adapter_status = EngineWireAdapterStatus::converted;

		wire::EthernetEnvelope output_envelope{};
		output_envelope.destination = input_envelope.source;
		output_envelope.source = config_.local_source;

		if (!wire::encode_order_frame(output_envelope, *wire_order, output)) {
			result.status = PacketProcessStatus::output_encode_rejected;
			return result;
		}

		++outbound_sequence_;
		result.output_ready = true;
		result.status = PacketProcessStatus::order_encoded;
		return result;
	}

	[[nodiscard]] BoundedBurstResult<MaxBurst>
		process_burst(const std::span<const std::span<const std::byte>> inputs,
			      const std::span<wire::EthernetFrame> outputs) noexcept
	{
		BoundedBurstResult<MaxBurst> result{};
		const std::size_t count = inputs.size() < MaxBurst ? inputs.size() : MaxBurst;

		if (outputs.size() < count) {
			result.status = BoundedBurstStatus::insufficient_output_slots;
			return result;
		}

		if (inputs.size() > MaxBurst) {
			result.status = BoundedBurstStatus::input_truncated;
		}

		for (std::size_t index = 0; index < count; ++index) {
			result.packets[index] = process(inputs[index], outputs[index]);
			++result.processed;

			if (result.packets[index].output_ready) {
				++result.orders_encoded;
			}
		}

		return result;
	}

	// A replay always starts from the post-construction engine and output
	// sequence state, so identical inputs produce identical result records and
	// output bytes.
	[[nodiscard]] BoundedBurstResult<MaxBurst> replay(const std::span<const std::span<const std::byte>> inputs,
							  const std::span<wire::EthernetFrame> outputs) noexcept
	{
		reset();
		return process_burst(inputs, outputs);
	}

	void reset() noexcept
	{
		engine_.reset();
		outbound_sequence_ = config_.first_output_sequence;
	}

	void set_kill_switch(const bool enabled) noexcept { engine_.set_kill_switch(enabled); }

	[[nodiscard]] bool try_acknowledge_one_order() noexcept { return engine_.try_acknowledge_one_order(); }

	[[nodiscard]] const TradingEngine<OutboxCapacity> &engine() const noexcept { return engine_; }

private:
	PacketProcessorConfig config_{};
	TradingEngine<OutboxCapacity> engine_;
	std::uint32_t outbound_sequence_{};
};

} // namespace depthwire::pipeline
