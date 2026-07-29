#pragma once

// Optional C++26 contract instrumentation (P2900). Disabled by default so the
// ordinary build carries no contract syntax at all; enable it with the CMake
// option DEPTHWIRE_ENABLE_CONTRACTS (GCC only -- Clang has no P2900 support yet,
// so it must never see the contract keywords).
//
// When enabled, DW_PRE/DW_POST attach a precondition/postcondition to a function
// declarator (placed after the noexcept-specifier, before the body) and
// DW_CONTRACT_ASSERT places an in-body assertion. When disabled they expand to
// nothing, so every consuming header stays valid on every toolchain.
//
// The gate deliberately requires BOTH the build-time opt-in macro and the
// language feature-test macro: Clang never defines __cpp_contracts, so even if
// the option is forced on there the keywords never appear.
#if defined(DEPTHWIRE_CONTRACTS_ENABLED) && defined(__cpp_contracts)
#	define DW_PRE(...) pre(__VA_ARGS__)
#	define DW_POST(...) post(__VA_ARGS__)
#	define DW_CONTRACT_ASSERT(...) contract_assert(__VA_ARGS__)
#else
#	define DW_PRE(...)
#	define DW_POST(...)
#	define DW_CONTRACT_ASSERT(...) static_cast<void>(0)
#endif
