#include <array>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "depthwire/digest/sha256.hpp"

namespace {

using depthwire::digest::Sha256;
using depthwire::digest::sha256_of;
using depthwire::digest::to_hex;

std::span<const std::byte> as_bytes(const std::string_view text) noexcept
{
	return {reinterpret_cast<const std::byte *>(text.data()), text.size()};
}

bool hex_equals(const depthwire::digest::Sha256Digest &digest, const std::string_view expected)
{
	const auto hex = to_hex(digest);
	return expected.size() == hex.size() && std::memcmp(hex.data(), expected.data(), hex.size()) == 0;
}

// FIPS 180-2 appendix B vectors plus the canonical empty-message digest.
void test_nist_vectors()
{
	assert(hex_equals(sha256_of({}), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));

	assert(hex_equals(sha256_of(as_bytes("abc")),
			  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));

	assert(hex_equals(sha256_of(as_bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
			  "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));

	assert(hex_equals(sha256_of(as_bytes("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
					     "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")),
			  "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
}

void test_million_a()
{
	const std::vector<std::byte> message(1'000'000, std::byte{'a'});

	assert(hex_equals(sha256_of(std::span<const std::byte>(message)),
			  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

// Splitting the input at any point, including across block boundaries, must
// produce the one-shot digest.
void test_incremental_split_equivalence()
{
	const std::string_view text = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
	const auto message = as_bytes(text);
	const auto expected = sha256_of(message);

	for (std::size_t split = 0; split <= message.size(); ++split) {
		Sha256 hasher{};
		hasher.update(message.first(split));
		hasher.update(message.subspan(split));
		assert(hasher.finalise() == expected);
	}

	const std::vector<std::byte> long_message(1'000, std::byte{'a'});
	const auto long_expected = sha256_of(std::span<const std::byte>(long_message));

	for (std::size_t chunk = 1; chunk <= 130; ++chunk) {
		Sha256 hasher{};
		std::span<const std::byte> remaining(long_message);

		while (!remaining.empty()) {
			const std::size_t take = remaining.size() < chunk ? remaining.size() : chunk;
			hasher.update(remaining.first(take));
			remaining = remaining.subspan(take);
		}

		assert(hasher.finalise() == long_expected);
	}
}

void test_reset_reuses_instance()
{
	Sha256 hasher{};
	hasher.update(as_bytes("garbage state"));
	(void)hasher.finalise();

	hasher.reset();
	hasher.update(as_bytes("abc"));
	assert(hex_equals(hasher.finalise(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

} // namespace

int main()
{
	test_nist_vectors();
	test_million_a();
	test_incremental_split_equivalence();
	test_reset_reuses_instance();
	return 0;
}
