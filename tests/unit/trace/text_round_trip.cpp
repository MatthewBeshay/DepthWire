#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "depthwire/trace/trace_codec.hpp"
#include "depthwire/trace/trace_format.hpp"
#include "depthwire/trace/trace_text.hpp"
#include "depthwire/wire/market_data_packet.hpp"

namespace {

using namespace depthwire::trace;

constexpr std::string_view kCanonicalText = "depthwire-trace-text v1\n"
					    "wire-version 1\n"
					    "instrument 77\n"
					    "max-delta-ns 1000000\n"
					    "max-quantity 1001\n"
					    "record delta=1000 seq=4294967286 ts=1000 side=sell "
					    "action=modify level=0 price=100100 qty=10\n"
					    "record delta=0 seq=4294967287 ts=2000 side=buy "
					    "action=add level=4 price=-5 qty=1001\n"
					    "record delta=1000000 seq=4294967288 ts=3000 side=buy "
					    "action=erase level=1 price=99999 qty=0\n";

void expect_parse_error(const std::string_view text, const TraceTextErrorCode expected, const std::size_t expected_line)
{
	const auto result = parse_trace_text(text);
	assert(!result);
	assert(result.error().code == expected);
	assert(result.error().line == expected_line);
}

void test_parse_canonical()
{
	const auto parsed = parse_trace_text(kCanonicalText);
	assert(parsed);
	assert(parsed->bounds.instrument_id == 77);
	assert(parsed->bounds.max_arrival_delta_ns == 1'000'000);
	assert(parsed->bounds.max_quantity == 1001);
	assert(parsed->records.size() == 3);

	const auto &first = parsed->records[0];
	assert(first.arrival_delta_ns == 1000);
	assert(first.message.sequence_number == 4294967286U);
	assert(first.message.exchange_timestamp_ns == 1000);
	assert(first.message.side == depthwire::wire::Side::sell);
	assert(first.message.action == depthwire::wire::UpdateAction::modify);
	assert(first.message.level == 0);
	assert(first.message.price_ticks == 100100);
	assert(first.message.quantity == 10);
	assert(first.message.instrument_id == 77);
	assert(first.message.wire_flags == 0);
	assert(first.message.source_flags == 0);

	const auto &second = parsed->records[1];
	assert(second.message.price_ticks == -5);
	assert(second.message.action == depthwire::wire::UpdateAction::add);

	const auto &third = parsed->records[2];
	assert(third.arrival_delta_ns == 1'000'000);
	assert(third.message.action == depthwire::wire::UpdateAction::erase);
}

// Text -> records -> text must reproduce the canonical bytes exactly, and the
// same records must round-trip through the binary container.
void test_round_trips()
{
	const auto parsed = parse_trace_text(kCanonicalText);
	assert(parsed);

	const auto formatted = format_trace_text(parsed->bounds, parsed->records);
	assert(formatted);
	assert(*formatted == kCanonicalText);

	// Text -> binary. The erase record carries quantity 0, which the wire
	// codec accepts for market data; the container accepts it too.
	std::vector<std::byte> file(trace_file_bytes(static_cast<std::uint32_t>(parsed->records.size())));
	const auto header = encode_trace(parsed->records, parsed->bounds, file);
	assert(header);

	// Binary -> records -> text -> binary reproduces the file bytes.
	const TraceView view{std::span<const std::byte>(file)};
	std::vector<TraceRecord> decoded{};

	for (std::uint32_t index = 0; index < view.record_count(); ++index) {
		const auto message = depthwire::wire::decode_market_data(view.message_bytes(index));
		assert(message);
		decoded.push_back({view.arrival_delta_ns(index), *message});
	}

	const auto reformatted = format_trace_text(header->bounds, decoded);
	assert(reformatted);
	assert(*reformatted == kCanonicalText);

	std::vector<std::byte> file_again(file.size());
	assert(encode_trace(decoded, header->bounds, file_again));
	assert(file == file_again);
}

void test_comments_and_blank_lines()
{
	const std::string text = std::string("# authored fixture\n\ndepthwire-trace-text v1\n") +
				 "wire-version 1\n# bounds\ninstrument 77\nmax-delta-ns 1000\n" +
				 "max-quantity 10\n\nrecord delta=1 seq=1 ts=0 side=buy " +
				 "action=modify level=0 price=1 qty=10\n";

	const auto parsed = parse_trace_text(text);
	assert(parsed);
	assert(parsed->records.size() == 1);
}

void test_parse_rejections()
{
	expect_parse_error("depthwire-trace-text v2\n", TraceTextErrorCode::invalid_banner, 1);
	expect_parse_error("record delta=1\n", TraceTextErrorCode::invalid_banner, 1);

	expect_parse_error("depthwire-trace-text v1\nwire-version 2\n", TraceTextErrorCode::invalid_header, 2);
	expect_parse_error("depthwire-trace-text v1\ninstrument 77\n", TraceTextErrorCode::invalid_header, 2);
	expect_parse_error("depthwire-trace-text v1\nwire-version 1\ninstrument seventy\n",
			   TraceTextErrorCode::invalid_header, 3);

	const std::string_view header = "depthwire-trace-text v1\nwire-version 1\ninstrument 77\n"
					"max-delta-ns 1000\nmax-quantity 10\n";

	// Missing records entirely.
	expect_parse_error(header, TraceTextErrorCode::no_records, 5);

	const auto reject_record = [&](const std::string_view record_line) {
		const std::string text = std::string(header) + std::string(record_line) + "\n";
		expect_parse_error(text, TraceTextErrorCode::invalid_record, 6);
	};

	reject_record("record delta=1 seq=1 ts=0 side=buy action=modify level=0 price=1");
	reject_record("record delta=1 seq=1 ts=0 side=hold action=modify level=0 price=1 qty=1");
	reject_record("record delta=1 seq=1 ts=0 side=buy action=cancel level=0 price=1 qty=1");
	reject_record("record seq=1 delta=1 ts=0 side=buy action=modify level=0 price=1 qty=1");
	reject_record("record delta=1 seq=1 ts=0 side=buy action=modify level=0 price=1 qty=1 x=1");
	reject_record("record delta=1 seq=4294967296 ts=0 side=buy action=modify level=0 price=1 "
		      "qty=1");
	reject_record("record delta=-1 seq=1 ts=0 side=buy action=modify level=0 price=1 qty=1");
	reject_record("nonsense");
}

void test_format_rejects_invalid_enums()
{
	TraceBounds bounds{};
	bounds.max_arrival_delta_ns = 1000;
	bounds.instrument_id = 77;
	bounds.max_quantity = 10;

	std::vector<TraceRecord> records(1);
	records[0].message.instrument_id = 77;
	records[0].message.quantity = 1;
	records[0].message.side = static_cast<depthwire::wire::Side>(9);
	records[0].message.action = depthwire::wire::UpdateAction::modify;

	const auto formatted = format_trace_text(bounds, records);
	assert(!formatted);
	assert(formatted.error().error == TraceError::record_encode_rejected);
	assert(formatted.error().record_index == 0);
}

} // namespace

int main()
{
	test_parse_canonical();
	test_round_trips();
	test_comments_and_blank_lines();
	test_parse_rejections();
	test_format_rejects_invalid_enums();
	return 0;
}
