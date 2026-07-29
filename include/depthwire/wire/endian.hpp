#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

#include "depthwire/contracts/contracts.hpp"

namespace depthwire::wire {

[[nodiscard]] constexpr bool contains_bytes(const std::size_t size, const std::size_t offset,
					    const std::size_t width) noexcept
{
	return offset <= size && width <= size - offset;
}

namespace detail {

// Fixed-layout codec internals. Their precondition is
// contains_bytes(span.size(), offset, field_width).
//
// Each codec stages the field in a fixed-size std::array<std::byte, N> and uses
// std::bit_cast + std::byteswap to convert between the host integer and the
// big-endian wire order. The span is only ever indexed byte-by-byte, so the
// access stays defined regardless of the offset's alignment.
template<typename Unsigned> [[nodiscard]] constexpr Unsigned host_to_big(const Unsigned value) noexcept
{
	if constexpr (std::endian::native == std::endian::little) {
		return std::byteswap(value);
	} else {
		return value;
	}
}

template<typename Unsigned>
constexpr void store_be(std::span<std::byte> out, const std::size_t offset, const Unsigned value) noexcept
{
	DW_CONTRACT_ASSERT(contains_bytes(out.size(), offset, sizeof(Unsigned)));
	const auto bytes = std::bit_cast<std::array<std::byte, sizeof(Unsigned)>>(host_to_big(value));
	for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
		out[offset + index] = bytes[index];
	}
}

template<typename Unsigned>
[[nodiscard]] constexpr Unsigned load_be(const std::span<const std::byte> in, const std::size_t offset) noexcept
{
	DW_CONTRACT_ASSERT(contains_bytes(in.size(), offset, sizeof(Unsigned)));
	std::array<std::byte, sizeof(Unsigned)> bytes{};
	for (std::size_t index = 0; index < sizeof(Unsigned); ++index) {
		bytes[index] = in[offset + index];
	}
	return host_to_big(std::bit_cast<Unsigned>(bytes));
}

constexpr void store_be16(std::span<std::byte> out, const std::size_t offset, const std::uint16_t value) noexcept
{
	store_be<std::uint16_t>(out, offset, value);
}

constexpr void store_be32(std::span<std::byte> out, const std::size_t offset, const std::uint32_t value) noexcept
{
	store_be<std::uint32_t>(out, offset, value);
}

constexpr void store_be64(std::span<std::byte> out, const std::size_t offset, const std::uint64_t value) noexcept
{
	store_be<std::uint64_t>(out, offset, value);
}

[[nodiscard]] constexpr std::uint16_t load_be16(const std::span<const std::byte> in, const std::size_t offset) noexcept
{
	return load_be<std::uint16_t>(in, offset);
}

[[nodiscard]] constexpr std::uint32_t load_be32(const std::span<const std::byte> in, const std::size_t offset) noexcept
{
	return load_be<std::uint32_t>(in, offset);
}

[[nodiscard]] constexpr std::uint64_t load_be64(const std::span<const std::byte> in, const std::size_t offset) noexcept
{
	return load_be<std::uint64_t>(in, offset);
}

constexpr void store_i64_be(const std::span<std::byte> out, const std::size_t offset, const std::int64_t value) noexcept
	DW_PRE(contains_bytes(out.size(), offset, 8))
{
	store_be64(out, offset, std::bit_cast<std::uint64_t>(value));
}

[[nodiscard]] constexpr std::int64_t load_i64_be(const std::span<const std::byte> in, const std::size_t offset) noexcept
	DW_PRE(contains_bytes(in.size(), offset, 8))
{
	return std::bit_cast<std::int64_t>(load_be64(in, offset));
}

} // namespace detail

[[nodiscard]] inline bool try_store_be16(const std::span<std::byte> output, const std::size_t offset,
					 const std::uint16_t value) noexcept
{
	if (!contains_bytes(output.size(), offset, 2)) {
		return false;
	}

	detail::store_be16(output, offset, value);
	return true;
}

[[nodiscard]] inline bool try_store_be32(const std::span<std::byte> output, const std::size_t offset,
					 const std::uint32_t value) noexcept
{
	if (!contains_bytes(output.size(), offset, 4)) {
		return false;
	}

	detail::store_be32(output, offset, value);
	return true;
}

[[nodiscard]] inline bool try_store_be64(const std::span<std::byte> output, const std::size_t offset,
					 const std::uint64_t value) noexcept
{
	if (!contains_bytes(output.size(), offset, 8)) {
		return false;
	}

	detail::store_be64(output, offset, value);
	return true;
}

[[nodiscard]] inline bool try_store_i64_be(const std::span<std::byte> output, const std::size_t offset,
					   const std::int64_t value) noexcept
{
	if (!contains_bytes(output.size(), offset, 8)) {
		return false;
	}

	detail::store_i64_be(output, offset, value);
	return true;
}

[[nodiscard]] inline bool try_load_be16(const std::span<const std::byte> input, const std::size_t offset,
					std::uint16_t &output) noexcept
{
	if (!contains_bytes(input.size(), offset, 2)) {
		return false;
	}

	output = detail::load_be16(input, offset);
	return true;
}

[[nodiscard]] inline bool try_load_be32(const std::span<const std::byte> input, const std::size_t offset,
					std::uint32_t &output) noexcept
{
	if (!contains_bytes(input.size(), offset, 4)) {
		return false;
	}

	output = detail::load_be32(input, offset);
	return true;
}

[[nodiscard]] inline bool try_load_be64(const std::span<const std::byte> input, const std::size_t offset,
					std::uint64_t &output) noexcept
{
	if (!contains_bytes(input.size(), offset, 8)) {
		return false;
	}

	output = detail::load_be64(input, offset);
	return true;
}

[[nodiscard]] inline bool try_load_i64_be(const std::span<const std::byte> input, const std::size_t offset,
					  std::int64_t &output) noexcept
{
	if (!contains_bytes(input.size(), offset, 8)) {
		return false;
	}

	output = detail::load_i64_be(input, offset);
	return true;
}

} // namespace depthwire::wire
