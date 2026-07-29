#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "depthwire/book/fixed_l2_book.hpp"
#include "depthwire/market/types.hpp"
#include "depthwire/order/order_request.hpp"
#include "depthwire/order/preallocated_outbox.hpp"
#include "depthwire/risk/risk_guard.hpp"
#include "depthwire/strategy/imbalance_strategy.hpp"

namespace depthwire {

enum class EngineStatus : std::uint8_t {
	no_order = 0,
	order_emitted,
	invalid_update,
	risk_rejected,
	outbox_full,
	invalid_configuration,
	duplicate_update,
	sequence_gap,
	stale_update,
};

enum class SequencePolicy : std::uint8_t {
	require_contiguous = 0,
	accept_any,
};

[[nodiscard]] constexpr bool is_valid_sequence_policy(const SequencePolicy policy) noexcept
{
	return policy == SequencePolicy::require_contiguous || policy == SequencePolicy::accept_any;
}

struct EngineConfig {
	InstrumentId instrument_id{77};
	StrategyConfig strategy{};
	RiskLimits risk{};
	SequencePolicy sequence_policy{SequencePolicy::require_contiguous};
};

struct EngineResult {
	EngineStatus status{EngineStatus::no_order};
	Signal signal{Signal::no_signal};
	std::int64_t imbalance_bps{};
	BookApplyStatus book_status{BookApplyStatus::not_attempted};
	RiskCheckStatus risk_status{RiskCheckStatus::not_checked};
};

// Thread-confined state machine. Every operation, including destruction, must
// run on one caller-owned thread or executor.
template<std::size_t OutboxCapacity = 64> class TradingEngine {
public:
	explicit TradingEngine(const EngineConfig config = {}) noexcept
		: config_(config), strategy_(config.strategy), risk_(config.risk)
	{
	}

	TradingEngine(const TradingEngine &) = delete;
	TradingEngine &operator=(const TradingEngine &) = delete;
	TradingEngine(TradingEngine &&) = delete;
	TradingEngine &operator=(TradingEngine &&) = delete;

	[[nodiscard]] bool configuration_valid() const noexcept
	{
		return is_valid_strategy_config(config_.strategy) && is_valid_risk_limits(config_.risk) &&
		       is_valid_sequence_policy(config_.sequence_policy);
	}

	[[nodiscard]] EngineResult on_market_update(const MarketUpdate &update) noexcept
	{
		if (!configuration_valid()) {
			return {
				EngineStatus::invalid_configuration,
				Signal::no_signal,
				0,
			};
		}

		if (update.instrument_id != config_.instrument_id) {
			return {
				EngineStatus::invalid_update,
				Signal::no_signal,
				0,
			};
		}

		// A full outbox backpressures the entire state machine. Reject before
		// sequence or book mutation so the caller can drain and retry exactly
		// the same update without losing its signal.
		if (outbox_.full()) {
			return {
				EngineStatus::outbox_full,
				Signal::no_signal,
				0,
			};
		}

		const EngineStatus sequence_status = validate_sequence(update.sequence_number);

		if (sequence_status != EngineStatus::no_order) {
			return {
				sequence_status,
				Signal::no_signal,
				0,
			};
		}

		const BookApplyStatus book_status = book_.apply_checked(update);

		if (book_status != BookApplyStatus::applied) {
			return {
				EngineStatus::invalid_update,
				Signal::no_signal,
				0,
				book_status,
			};
		}

		last_sequence_number_ = update.sequence_number;
		has_last_sequence_number_ = true;

		const StrategyDecision decision = strategy_.evaluate(book_);

		if (decision.signal == Signal::no_signal) {
			return {
				EngineStatus::no_order,
				Signal::no_signal,
				decision.imbalance_bps,
				book_status,
			};
		}

		OrderRequest order{};
		order.client_order_id = next_client_order_id_;
		order.price_ticks = decision.limit_price_ticks;
		order.quantity = decision.quantity;
		order.instrument_id = update.instrument_id;
		order.source_sequence = update.sequence_number;
		order.side = decision.signal == Signal::buy ? Side::buy : Side::sell;
		order.order_kind = OrderKind::limit;
		order.flags = 0;

		const RiskCheckStatus risk_status = risk_.validate_and_commit_checked(order);

		if (risk_status != RiskCheckStatus::accepted) {
			return {
				EngineStatus::risk_rejected,
				decision.signal,
				decision.imbalance_bps,
				book_status,
				risk_status,
			};
		}

		if (!outbox_.push(order)) {
			return {
				EngineStatus::outbox_full,
				decision.signal,
				decision.imbalance_bps,
				book_status,
				risk_status,
			};
		}

		++next_client_order_id_;

		return {
			EngineStatus::order_emitted, decision.signal, decision.imbalance_bps, book_status, risk_status,
		};
	}

	[[nodiscard]] bool pop_order(OrderRequest &order) noexcept { return outbox_.pop(order); }

	[[nodiscard]] const FixedL2Book &book() const noexcept { return book_; }

	void acknowledge_one_order() noexcept { risk_.acknowledge_one(); }

	[[nodiscard]] bool try_acknowledge_one_order() noexcept { return risk_.try_acknowledge_one(); }

	void set_kill_switch(const bool enabled) noexcept { risk_.set_kill_switch(enabled); }

	[[nodiscard]] bool kill_switch_enabled() const noexcept { return risk_.kill_switch_enabled(); }

	[[nodiscard]] const RiskGuard &risk() const noexcept { return risk_; }

	[[nodiscard]] std::optional<SequenceNumber> last_sequence_number() const noexcept
	{
		if (!has_last_sequence_number_) {
			return std::nullopt;
		}

		return last_sequence_number_;
	}

	void reset_market_state() noexcept
	{
		book_.clear();
		has_last_sequence_number_ = false;
		last_sequence_number_ = 0;
	}

	// Deterministic replay reset: restores the exact post-construction state.
	void reset() noexcept
	{
		book_.clear();
		strategy_ = ImbalanceStrategy(config_.strategy);
		risk_ = RiskGuard(config_.risk);
		outbox_.clear();
		next_client_order_id_ = 1;
		last_sequence_number_ = 0;
		has_last_sequence_number_ = false;
	}

private:
	[[nodiscard]] EngineStatus validate_sequence(const SequenceNumber candidate) const noexcept
	{
		if (config_.sequence_policy == SequencePolicy::accept_any || !has_last_sequence_number_) {
			return EngineStatus::no_order;
		}

		const std::uint32_t forward_distance = candidate - last_sequence_number_;

		if (forward_distance == 0) {
			return EngineStatus::duplicate_update;
		}

		if (forward_distance == 1) {
			return EngineStatus::no_order;
		}

		if (forward_distance < (std::uint32_t{1} << 31U)) {
			return EngineStatus::sequence_gap;
		}

		return EngineStatus::stale_update;
	}

	EngineConfig config_{};
	FixedL2Book book_{};
	ImbalanceStrategy strategy_{};
	RiskGuard risk_{};
	PreallocatedOutbox<OutboxCapacity> outbox_{};
	std::uint64_t next_client_order_id_{1};
	SequenceNumber last_sequence_number_{};
	bool has_last_sequence_number_{};
};

} // namespace depthwire
