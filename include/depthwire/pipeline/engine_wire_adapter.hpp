#pragma once

#include <cstdint>
#include <expected>

#include "depthwire/market/types.hpp"
#include "depthwire/order/order_request.hpp"
#include "depthwire/wire/market_data_packet.hpp"
#include "depthwire/wire/order_packet.hpp"

namespace depthwire::pipeline {

enum class EngineWireAdapterStatus : std::uint8_t {
	not_attempted = 0,
	converted,
	unsupported_market_header_flags,
	unsupported_market_source_flags,
	invalid_market_side,
	invalid_market_action,
	invalid_order_side,
	unsupported_order_kind,
	unsupported_order_flags,
};

class EngineWireAdapter {
public:
	[[nodiscard]] static std::expected<MarketUpdate, EngineWireAdapterStatus>
		to_market_update(const wire::MarketDataMessage &input) noexcept
	{
		if ((input.wire_flags & ~wire::kHeaderFlagsMaskVersion1) != 0) {
			return std::unexpected(EngineWireAdapterStatus::unsupported_market_header_flags);
		}

		if ((input.source_flags & ~wire::kMarketDataSourceFlagsMaskVersion1) != 0) {
			return std::unexpected(EngineWireAdapterStatus::unsupported_market_source_flags);
		}

		Side side{Side::buy};

		switch (input.side) {
		case wire::Side::buy:
			side = Side::buy;
			break;
		case wire::Side::sell:
			side = Side::sell;
			break;
		default:
			return std::unexpected(EngineWireAdapterStatus::invalid_market_side);
		}

		UpdateAction action{UpdateAction::modify};

		switch (input.action) {
		case wire::UpdateAction::add:
			action = UpdateAction::add;
			break;
		case wire::UpdateAction::modify:
			action = UpdateAction::modify;
			break;
		case wire::UpdateAction::erase:
			action = UpdateAction::erase;
			break;
		default:
			return std::unexpected(EngineWireAdapterStatus::invalid_market_action);
		}

		MarketUpdate output{};
		output.sequence_number = input.sequence_number;
		output.exchange_timestamp_ns = input.exchange_timestamp_ns;
		output.instrument_id = input.instrument_id;
		output.price_ticks = input.price_ticks;
		output.quantity = input.quantity;
		output.side = side;
		output.action = action;
		output.level = input.level;
		output.source_flags = input.source_flags;

		return output;
	}

	[[nodiscard]] static std::expected<wire::OutboundOrderMessage, EngineWireAdapterStatus>
		to_wire_order(const OrderRequest &input, const std::uint32_t output_sequence_number) noexcept
	{
		wire::Side side{wire::Side::buy};

		switch (input.side) {
		case Side::buy:
			side = wire::Side::buy;
			break;
		case Side::sell:
			side = wire::Side::sell;
			break;
		default:
			return std::unexpected(EngineWireAdapterStatus::invalid_order_side);
		}

		if (input.order_kind != OrderKind::limit) {
			return std::unexpected(EngineWireAdapterStatus::unsupported_order_kind);
		}

		if ((input.flags & ~wire::kOrderFlagsMaskVersion1) != 0) {
			return std::unexpected(EngineWireAdapterStatus::unsupported_order_flags);
		}

		wire::OutboundOrderMessage output{};
		output.sequence_number = output_sequence_number;
		output.wire_flags = 0;
		output.client_order_id = input.client_order_id;
		output.instrument_id = input.instrument_id;
		output.price_ticks = input.price_ticks;
		output.quantity = input.quantity;
		output.side = side;
		output.order_type = wire::OrderType::limit;
		output.time_in_force = wire::TimeInForce::immediate_or_cancel;
		output.order_flags = static_cast<std::uint8_t>(input.flags);
		output.source_sequence = input.source_sequence;

		return output;
	}
};

} // namespace depthwire::pipeline
