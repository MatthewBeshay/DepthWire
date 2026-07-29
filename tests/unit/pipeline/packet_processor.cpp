#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>

#include "depthwire/pipeline/packet_processor.hpp"

namespace {

depthwire::wire::EthernetEnvelope make_input_envelope() noexcept
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

depthwire::wire::MarketDataMessage make_message(const std::uint32_t sequence, const depthwire::wire::Side side,
						const std::uint16_t level, const std::int64_t price,
						const std::uint32_t quantity) noexcept
{
	depthwire::wire::MarketDataMessage message{};
	message.sequence_number = sequence;
	message.exchange_timestamp_ns = static_cast<std::uint64_t>(sequence) * 1000ULL;
	message.instrument_id = 77;
	message.price_ticks = price;
	message.quantity = quantity;
	message.side = side;
	message.action = depthwire::wire::UpdateAction::modify;
	message.level = level;
	return message;
}

depthwire::pipeline::PacketProcessorConfig make_processor_config() noexcept
{
	depthwire::pipeline::PacketProcessorConfig config{};
	config.engine.instrument_id = 77;
	config.engine.strategy.imbalance_threshold_bps = 6000;
	config.engine.strategy.order_quantity = 10;
	config.engine.risk.max_order_quantity = 100;
	config.engine.risk.max_absolute_position = 1000;
	config.engine.risk.max_notional_ticks = 1'000'000'000ULL;
	config.engine.risk.max_outstanding_orders = 64;
	config.local_source = {
		std::byte{0x02}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x20},
	};
	config.first_output_sequence = 5001;
	return config;
}

std::array<depthwire::wire::EthernetFrame, 11> make_replay_frames()
{
	std::array<depthwire::wire::EthernetFrame, 11> frames{};
	std::uint32_t sequence = 1;
	std::size_t index = 0;

	for (std::uint16_t level = 0; level < depthwire::kVisibleDepth; ++level) {
		const auto encoded = depthwire::wire::encode_market_data_frame(
			make_input_envelope(),
			make_message(sequence++, depthwire::wire::Side::sell, level, 101 + level, 10), frames[index++]);
		assert(encoded);
	}

	for (std::uint16_t level = 0; level < depthwire::kVisibleDepth; ++level) {
		const auto encoded = depthwire::wire::encode_market_data_frame(
			make_input_envelope(),
			make_message(sequence++, depthwire::wire::Side::buy, level, 100 - level, 10), frames[index++]);
		assert(encoded);
	}

	const auto encoded_trigger = depthwire::wire::encode_market_data_frame(
		make_input_envelope(), make_message(sequence, depthwire::wire::Side::buy, 0, 100, 1000), frames[index]);
	assert(encoded_trigger);

	return frames;
}

template<std::size_t Size>
std::array<std::span<const std::byte>, Size>
	make_spans(const std::array<depthwire::wire::EthernetFrame, Size> &frames) noexcept
{
	std::array<std::span<const std::byte>, Size> spans{};

	for (std::size_t index = 0; index < Size; ++index) {
		spans[index] = std::span<const std::byte>(frames[index]);
	}

	return spans;
}

} // namespace

int main()
{
	using namespace depthwire;
	using namespace depthwire::pipeline;

	const auto frames = make_replay_frames();
	const auto spans = make_spans(frames);

	{
		PacketProcessor<64, 16> processor(make_processor_config());
		assert(processor.configuration_valid());

		wire::EthernetFrame output{};

		for (std::size_t index = 0; index < 10; ++index) {
			const auto result = processor.process(spans[index], output);
			assert(result.status == PacketProcessStatus::no_order);
			assert(!result.output_ready);
		}

		const auto result = processor.process(spans[10], output);
		assert(result.status == PacketProcessStatus::order_encoded);
		assert(result.output_ready);
		assert(result.output_sequence_number == 5001);
		assert(result.engine_result.status == EngineStatus::order_emitted);

		const auto decoded_order_frame = wire::decode_order_frame(std::span<const std::byte>(output));
		assert(decoded_order_frame);
		const auto &output_envelope = decoded_order_frame->envelope;
		const auto &order = decoded_order_frame->message;
		assert(output_envelope.destination == make_input_envelope().source);
		assert(output_envelope.source == make_processor_config().local_source);
		assert(order.sequence_number == 5001);
		assert(order.source_sequence == 11);
		assert(order.instrument_id == 77);
		assert(order.side == wire::Side::buy);

		const auto duplicate = processor.process(spans[10], output);
		assert(duplicate.status == PacketProcessStatus::engine_rejected);
		assert(duplicate.engine_result.status == EngineStatus::duplicate_update);
		assert(!duplicate.output_ready);
	}

	{
		PacketProcessor<64, 16> processor(make_processor_config());
		wire::EthernetEnvelope invalid_envelope = make_input_envelope();
		invalid_envelope.destination[5] = std::byte{0x21};

		wire::EthernetFrame invalid_destination{};
		assert(wire::encode_market_data_frame(invalid_envelope, make_message(1, wire::Side::buy, 0, 100, 10),
						      invalid_destination));

		wire::EthernetFrame output{};
		const auto result = processor.process(std::span<const std::byte>(invalid_destination), output);
		assert(result.status == PacketProcessStatus::invalid_input_destination);
		assert(!processor.engine().last_sequence_number().has_value());
	}

	{
		PacketProcessor<64, 16> processor(make_processor_config());
		wire::EthernetEnvelope invalid_envelope = make_input_envelope();
		invalid_envelope.source.fill(std::byte{0});

		wire::EthernetFrame invalid_source{};
		assert(wire::encode_market_data_frame(invalid_envelope, make_message(1, wire::Side::buy, 0, 100, 10),
						      invalid_source));

		wire::EthernetFrame output{};
		const auto result = processor.process(std::span<const std::byte>(invalid_source), output);
		assert(result.status == PacketProcessStatus::invalid_input_source);
		assert(!processor.engine().last_sequence_number().has_value());
	}

	{
		auto config = make_processor_config();
		config.local_source[0] = std::byte{0x03};
		PacketProcessor<64, 16> processor(config);
		assert(!processor.configuration_valid());

		wire::EthernetFrame output{};
		const auto result = processor.process(spans[0], output);
		assert(result.status == PacketProcessStatus::invalid_configuration);
	}

	{
		PacketProcessor<64, 16> processor(make_processor_config());
		wire::EthernetFrame output{};
		const auto result = processor.process(std::span<const std::byte>{}, output);
		assert(result.status == PacketProcessStatus::decode_rejected);
		assert(result.decode_error == wire::WireError::too_short);
	}

	{
		PacketProcessor<64, 16> processor(make_processor_config());
		std::array<wire::EthernetFrame, 11> first_outputs{};
		std::array<wire::EthernetFrame, 11> second_outputs{};

		const auto first = processor.replay(std::span<const std::span<const std::byte>>(spans),
						    std::span<wire::EthernetFrame>(first_outputs));
		const auto second = processor.replay(std::span<const std::span<const std::byte>>(spans),
						     std::span<wire::EthernetFrame>(second_outputs));

		assert(first.status == BoundedBurstStatus::complete);
		assert(second.status == BoundedBurstStatus::complete);
		assert(first.processed == 11);
		assert(second.processed == 11);
		assert(first.orders_encoded == 1);
		assert(second.orders_encoded == 1);
		assert(first_outputs == second_outputs);

		for (std::size_t index = 0; index < spans.size(); ++index) {
			assert(first.packets[index].status == second.packets[index].status);
			assert(first.packets[index].engine_result.status == second.packets[index].engine_result.status);
			assert(first.packets[index].output_ready == second.packets[index].output_ready);
			assert(first.packets[index].output_sequence_number ==
			       second.packets[index].output_sequence_number);
		}
	}

	{
		PacketProcessor<64, 16> processor(make_processor_config());
		std::array<wire::EthernetFrame, 10> too_few_outputs{};
		const auto result = processor.process_burst(std::span<const std::span<const std::byte>>(spans),
							    std::span<wire::EthernetFrame>(too_few_outputs));
		assert(result.status == BoundedBurstStatus::insufficient_output_slots);
		assert(result.processed == 0);
		assert(!processor.engine().last_sequence_number().has_value());
	}

	{
		PacketProcessor<64, 4> processor(make_processor_config());
		std::array<wire::EthernetFrame, 4> outputs{};
		const auto result = processor.process_burst(std::span<const std::span<const std::byte>>(spans),
							    std::span<wire::EthernetFrame>(outputs));
		assert(result.status == BoundedBurstStatus::input_truncated);
		assert(result.processed == 4);
		assert(result.orders_encoded == 0);
		assert(processor.engine().last_sequence_number() == 4);
	}

	return 0;
}
