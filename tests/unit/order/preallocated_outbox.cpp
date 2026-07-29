#include <cassert>
#include <cstdint>

#include "depthwire/order/preallocated_outbox.hpp"

namespace {

depthwire::OrderRequest make_order(const std::uint64_t client_order_id) noexcept
{
	depthwire::OrderRequest order{};
	order.client_order_id = client_order_id;
	order.price_ticks = 100;
	order.quantity = 1;
	order.instrument_id = 77;
	order.source_sequence = static_cast<depthwire::SequenceNumber>(client_order_id);
	order.side = depthwire::Side::buy;
	order.order_kind = depthwire::OrderKind::limit;
	return order;
}

} // namespace

int main()
{
	depthwire::PreallocatedOutbox<3> outbox{};
	static_assert(outbox.capacity() == 3);

	assert(outbox.empty());
	assert(!outbox.full());
	assert(outbox.size() == 0);

	assert(outbox.push(make_order(1)));
	assert(outbox.push(make_order(2)));
	assert(outbox.push(make_order(3)));
	assert(outbox.full());
	assert(!outbox.push(make_order(4)));

	depthwire::OrderRequest output{};
	assert(outbox.pop(output));
	assert(output.client_order_id == 1);
	assert(outbox.push(make_order(4)));

	assert(outbox.pop(output));
	assert(output.client_order_id == 2);
	assert(outbox.pop(output));
	assert(output.client_order_id == 3);
	assert(outbox.pop(output));
	assert(output.client_order_id == 4);
	assert(outbox.empty());

	output.client_order_id = 99;
	assert(!outbox.pop(output));
	assert(output.client_order_id == 99);

	assert(outbox.push(make_order(5)));
	assert(outbox.push(make_order(6)));
	outbox.clear();
	assert(outbox.empty());
	assert(outbox.size() == 0);
	assert(outbox.push(make_order(7)));
	assert(outbox.pop(output));
	assert(output.client_order_id == 7);

	return 0;
}
