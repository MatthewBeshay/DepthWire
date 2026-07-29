#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace depthwire::digest {

inline constexpr std::size_t kSha256DigestBytes = 32;
inline constexpr std::size_t kSha256BlockBytes = 64;

using Sha256Digest = std::array<std::byte, kSha256DigestBytes>;
using Sha256HexText = std::array<char, 2 * kSha256DigestBytes>;

// Incremental SHA-256 (FIPS 180-2). Allocation-free and self-contained so the
// package carries no dependencies; canonical run digests and trace hashes are
// evidence artefacts, so the implementation is verified against the NIST test
// vectors rather than trusted by construction.
//
// finalise() consumes the accumulated state; call reset() (or use a fresh
// instance) before hashing another message.
class Sha256 {
public:
	constexpr Sha256() noexcept = default;

	constexpr void reset() noexcept { *this = Sha256{}; }

	constexpr void update(std::span<const std::byte> input) noexcept
	{
		total_bytes_ += input.size();

		while (!input.empty()) {
			const std::size_t take = input.size() < kSha256BlockBytes - buffered_
							 ? input.size()
							 : kSha256BlockBytes - buffered_;

			for (std::size_t index = 0; index < take; ++index) {
				block_[buffered_ + index] = input[index];
			}

			buffered_ += take;
			input = input.subspan(take);

			if (buffered_ == kSha256BlockBytes) {
				compress();
				buffered_ = 0;
			}
		}
	}

	[[nodiscard]] constexpr Sha256Digest finalise() noexcept
	{
		const std::uint64_t message_bits = total_bytes_ * 8U;

		block_[buffered_++] = std::byte{0x80};

		if (buffered_ > kSha256BlockBytes - 8) {
			while (buffered_ < kSha256BlockBytes) {
				block_[buffered_++] = std::byte{};
			}

			compress();
			buffered_ = 0;
		}

		while (buffered_ < kSha256BlockBytes - 8) {
			block_[buffered_++] = std::byte{};
		}

		for (std::size_t index = 0; index < 8; ++index) {
			block_[kSha256BlockBytes - 8 + index] =
				static_cast<std::byte>(message_bits >> (56 - 8 * index));
		}

		compress();

		Sha256Digest output{};

		for (std::size_t word = 0; word < state_.size(); ++word) {
			for (std::size_t index = 0; index < 4; ++index) {
				output[4 * word + index] = static_cast<std::byte>(state_[word] >> (24 - 8 * index));
			}
		}

		return output;
	}

private:
	constexpr void compress() noexcept
	{
		constexpr std::array<std::uint32_t, 64> k{
			0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
			0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
			0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
			0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
			0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
			0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
			0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
			0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
			0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
			0xc67178f2U,
		};

		std::array<std::uint32_t, 64> schedule{};

		for (std::size_t word = 0; word < 16; ++word) {
			schedule[word] = (std::to_integer<std::uint32_t>(block_[4 * word]) << 24) |
					 (std::to_integer<std::uint32_t>(block_[4 * word + 1]) << 16) |
					 (std::to_integer<std::uint32_t>(block_[4 * word + 2]) << 8) |
					 std::to_integer<std::uint32_t>(block_[4 * word + 3]);
		}

		for (std::size_t word = 16; word < 64; ++word) {
			const std::uint32_t sigma0 = std::rotr(schedule[word - 15], 7) ^
						     std::rotr(schedule[word - 15], 18) ^ (schedule[word - 15] >> 3);
			const std::uint32_t sigma1 = std::rotr(schedule[word - 2], 17) ^
						     std::rotr(schedule[word - 2], 19) ^ (schedule[word - 2] >> 10);
			schedule[word] = schedule[word - 16] + sigma0 + schedule[word - 7] + sigma1;
		}

		std::uint32_t a = state_[0];
		std::uint32_t b = state_[1];
		std::uint32_t c = state_[2];
		std::uint32_t d = state_[3];
		std::uint32_t e = state_[4];
		std::uint32_t f = state_[5];
		std::uint32_t g = state_[6];
		std::uint32_t h = state_[7];

		for (std::size_t round = 0; round < 64; ++round) {
			const std::uint32_t big_sigma1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
			const std::uint32_t choose = (e & f) ^ (~e & g);
			const std::uint32_t temp1 = h + big_sigma1 + choose + k[round] + schedule[round];
			const std::uint32_t big_sigma0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
			const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
			const std::uint32_t temp2 = big_sigma0 + majority;

			h = g;
			g = f;
			f = e;
			e = d + temp1;
			d = c;
			c = b;
			b = a;
			a = temp1 + temp2;
		}

		state_[0] += a;
		state_[1] += b;
		state_[2] += c;
		state_[3] += d;
		state_[4] += e;
		state_[5] += f;
		state_[6] += g;
		state_[7] += h;
	}

	std::array<std::uint32_t, 8> state_{
		0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU, 0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U,
	};
	std::array<std::byte, kSha256BlockBytes> block_{};
	std::uint64_t total_bytes_{};
	std::size_t buffered_{};
};

[[nodiscard]] constexpr Sha256Digest sha256_of(const std::span<const std::byte> input) noexcept
{
	Sha256 hasher{};
	hasher.update(input);
	return hasher.finalise();
}

[[nodiscard]] constexpr Sha256HexText to_hex(const Sha256Digest &digest) noexcept
{
	constexpr std::array<char, 16> alphabet{'0', '1', '2', '3', '4', '5', '6', '7',
						'8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};

	Sha256HexText output{};

	for (std::size_t index = 0; index < digest.size(); ++index) {
		const auto value = std::to_integer<std::uint8_t>(digest[index]);
		output[2 * index] = alphabet[value >> 4];
		output[2 * index + 1] = alphabet[value & 0x0FU];
	}

	return output;
}

} // namespace depthwire::digest
