#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "depthwire/trace/trace_format.hpp"
#include "depthwire/wire/wire_constants.hpp"

namespace depthwire::trace {

// Line-oriented authoring/inspection form of a trace. The binary container is
// canonical; this text form exists so fixtures can be hand-written and
// reviewed. Record count, total duration, and the record-region hash are
// always computed by the binary encoder, never hand-authored. This is offline
// converter tooling, not the replay hot path, so it may allocate.
//
// Layout (fixed order; blank lines and '#' comment lines are skipped):
//   depthwire-trace-text v1
//   wire-version 1
//   instrument <u32>
//   max-delta-ns <u64>
//   max-quantity <u32>
//   record delta=<u64> seq=<u32> ts=<u64> side=<buy|sell>
//       action=<add|modify|erase> level=<u16> price=<i64> qty=<u32>
//
// Records carry no instrument or flag fields: the instrument is pinned by the
// header bound and version-1 wire flags must be zero.

enum class TraceTextErrorCode : std::uint8_t {
	invalid_banner = 1,
	invalid_header,
	invalid_record,
	no_records,
	too_many_records,
};

struct TraceTextError {
	TraceTextErrorCode code{TraceTextErrorCode::invalid_banner};
	std::size_t line{};
};

struct ParsedTraceText {
	TraceBounds bounds{};
	std::vector<TraceRecord> records{};
};

namespace text_detail {

template<typename Unsigned>
[[nodiscard]] inline bool parse_unsigned(const std::string_view text, Unsigned &output) noexcept
{
	if (text.empty()) {
		return false;
	}

	const char *const first = text.data();
	const char *const last = text.data() + text.size();
	const auto result = std::from_chars(first, last, output);
	return result.ec == std::errc{} && result.ptr == last;
}

[[nodiscard]] inline bool parse_signed(const std::string_view text, std::int64_t &output) noexcept
{
	if (text.empty()) {
		return false;
	}

	const char *const first = text.data();
	const char *const last = text.data() + text.size();
	const auto result = std::from_chars(first, last, output);
	return result.ec == std::errc{} && result.ptr == last;
}

// Consumes the next whitespace-separated token of the form "<key>=<value>"
// and returns the value, or an empty view on any mismatch.
[[nodiscard]] inline std::string_view take_field(std::string_view &rest, const std::string_view key) noexcept
{
	while (rest.starts_with(' ')) {
		rest.remove_prefix(1);
	}

	if (!rest.starts_with(key) || rest.size() <= key.size() || rest[key.size()] != '=') {
		return {};
	}

	rest.remove_prefix(key.size() + 1);
	const std::size_t end = rest.find(' ');
	const std::string_view value = rest.substr(0, end);
	rest.remove_prefix(end == std::string_view::npos ? rest.size() : end);
	return value;
}

[[nodiscard]] inline bool parse_side(const std::string_view text, wire::Side &output) noexcept
{
	if (text == "buy") {
		output = wire::Side::buy;
		return true;
	}

	if (text == "sell") {
		output = wire::Side::sell;
		return true;
	}

	return false;
}

[[nodiscard]] inline bool parse_action(const std::string_view text, wire::UpdateAction &output) noexcept
{
	if (text == "add") {
		output = wire::UpdateAction::add;
		return true;
	}

	if (text == "modify") {
		output = wire::UpdateAction::modify;
		return true;
	}

	if (text == "erase") {
		output = wire::UpdateAction::erase;
		return true;
	}

	return false;
}

[[nodiscard]] inline std::string_view side_name(const wire::Side side) noexcept
{
	return side == wire::Side::buy ? "buy" : "sell";
}

[[nodiscard]] inline std::string_view action_name(const wire::UpdateAction action) noexcept
{
	if (action == wire::UpdateAction::add) {
		return "add";
	}

	return action == wire::UpdateAction::modify ? "modify" : "erase";
}

inline void append_unsigned(std::string &output, const std::uint64_t value)
{
	char buffer[20];
	const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
	output.append(buffer, result.ptr);
}

inline void append_signed(std::string &output, const std::int64_t value)
{
	char buffer[21];
	const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
	output.append(buffer, result.ptr);
}

} // namespace text_detail

[[nodiscard]] inline std::expected<ParsedTraceText, TraceTextError> parse_trace_text(const std::string_view text)
{
	ParsedTraceText parsed{};
	std::size_t header_lines_seen = 0;
	std::size_t line_number = 0;
	std::string_view remaining = text;

	while (!remaining.empty()) {
		++line_number;
		const std::size_t newline = remaining.find('\n');
		const std::string_view line = remaining.substr(0, newline);
		remaining.remove_prefix(newline == std::string_view::npos ? remaining.size() : newline + 1);

		if (line.empty() || line.starts_with('#')) {
			continue;
		}

		if (header_lines_seen == 0) {
			if (line != "depthwire-trace-text v1") {
				return std::unexpected(TraceTextError{TraceTextErrorCode::invalid_banner, line_number});
			}

			++header_lines_seen;
			continue;
		}

		if (header_lines_seen < 5) {
			const TraceTextError error{TraceTextErrorCode::invalid_header, line_number};

			switch (header_lines_seen) {
			case 1: {
				std::uint8_t version{};

				if (!line.starts_with("wire-version ") ||
				    !text_detail::parse_unsigned(line.substr(13), version) ||
				    version != wire::kVersion) {
					return std::unexpected(error);
				}

				break;
			}
			case 2:
				if (!line.starts_with("instrument ") ||
				    !text_detail::parse_unsigned(line.substr(11), parsed.bounds.instrument_id)) {
					return std::unexpected(error);
				}

				break;
			case 3:
				if (!line.starts_with("max-delta-ns ") ||
				    !text_detail::parse_unsigned(line.substr(13), parsed.bounds.max_arrival_delta_ns)) {
					return std::unexpected(error);
				}

				break;
			default:
				if (!line.starts_with("max-quantity ") ||
				    !text_detail::parse_unsigned(line.substr(13), parsed.bounds.max_quantity)) {
					return std::unexpected(error);
				}

				break;
			}

			++header_lines_seen;
			continue;
		}

		const TraceTextError error{TraceTextErrorCode::invalid_record, line_number};

		if (!line.starts_with("record ")) {
			return std::unexpected(error);
		}

		if (parsed.records.size() >= kMaxTraceRecords) {
			return std::unexpected(TraceTextError{TraceTextErrorCode::too_many_records, line_number});
		}

		std::string_view rest = line.substr(7);
		TraceRecord record{};
		record.message.instrument_id = parsed.bounds.instrument_id;

		if (!text_detail::parse_unsigned(text_detail::take_field(rest, "delta"), record.arrival_delta_ns) ||
		    !text_detail::parse_unsigned(text_detail::take_field(rest, "seq"),
						 record.message.sequence_number) ||
		    !text_detail::parse_unsigned(text_detail::take_field(rest, "ts"),
						 record.message.exchange_timestamp_ns) ||
		    !text_detail::parse_side(text_detail::take_field(rest, "side"), record.message.side) ||
		    !text_detail::parse_action(text_detail::take_field(rest, "action"), record.message.action) ||
		    !text_detail::parse_unsigned(text_detail::take_field(rest, "level"), record.message.level) ||
		    !text_detail::parse_signed(text_detail::take_field(rest, "price"), record.message.price_ticks) ||
		    !text_detail::parse_unsigned(text_detail::take_field(rest, "qty"), record.message.quantity) ||
		    !rest.empty()) {
			return std::unexpected(error);
		}

		parsed.records.push_back(record);
	}

	if (header_lines_seen < 5) {
		return std::unexpected(TraceTextError{TraceTextErrorCode::invalid_header, line_number});
	}

	if (parsed.records.empty()) {
		return std::unexpected(TraceTextError{TraceTextErrorCode::no_records, line_number});
	}

	return parsed;
}

[[nodiscard]] inline std::expected<std::string, TraceValidationError>
	format_trace_text(const TraceBounds &bounds, const std::span<const TraceRecord> records)
{
	std::string output = "depthwire-trace-text v1\nwire-version ";
	text_detail::append_unsigned(output, wire::kVersion);
	output += "\ninstrument ";
	text_detail::append_unsigned(output, bounds.instrument_id);
	output += "\nmax-delta-ns ";
	text_detail::append_unsigned(output, bounds.max_arrival_delta_ns);
	output += "\nmax-quantity ";
	text_detail::append_unsigned(output, bounds.max_quantity);
	output += '\n';

	for (std::size_t index = 0; index < records.size(); ++index) {
		const TraceRecord &record = records[index];
		const wire::MarketDataMessage &message = record.message;

		if (!wire::is_valid_side(message.side) || !wire::is_valid_update_action(message.action)) {
			return std::unexpected(TraceValidationError{TraceError::record_encode_rejected,
								    static_cast<std::uint32_t>(index)});
		}

		output += "record delta=";
		text_detail::append_unsigned(output, record.arrival_delta_ns);
		output += " seq=";
		text_detail::append_unsigned(output, message.sequence_number);
		output += " ts=";
		text_detail::append_unsigned(output, message.exchange_timestamp_ns);
		output += " side=";
		output += text_detail::side_name(message.side);
		output += " action=";
		output += text_detail::action_name(message.action);
		output += " level=";
		text_detail::append_unsigned(output, message.level);
		output += " price=";
		text_detail::append_signed(output, message.price_ticks);
		output += " qty=";
		text_detail::append_unsigned(output, message.quantity);
		output += '\n';
	}

	return output;
}

} // namespace depthwire::trace
