cmake_minimum_required(VERSION 3.25)

project(DepthWire
    VERSION 1.0.0
    LANGUAGES CXX
)

option(DEPTHWIRE_ENABLE_DPDK "Build DPDK applications and integration tests" OFF)
option(
    DEPTHWIRE_BUILD_BENCHMARKS
    "Build DPDK latency benchmark executables"
    OFF
)
option(DEPTHWIRE_BUILD_FUZZERS "Build Clang libFuzzer targets" OFF)
option(DEPTHWIRE_ENABLE_SANITISERS "Enable the address and undefined-behaviour sanitisers" OFF)
option(
    DEPTHWIRE_ENABLE_PACKAGE_TESTS
    "Verify the installed CMake package with an external consumer"
    ${PROJECT_IS_TOP_LEVEL}
)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The C++26 library facilities used here need a GCC-16-class libstdc++
# (Clang consumes it too); fail early instead of emitting confusing
# "not a member of std" errors deep in the build.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
        message(FATAL_ERROR
            "DepthWire requires GCC >= 16 for its C++26 library facilities "
            "(found ${CMAKE_CXX_COMPILER_VERSION}).")
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19)
        message(FATAL_ERROR
            "DepthWire requires Clang >= 19 (with a GCC 16-class libstdc++) "
            "for its C++26 library facilities (found ${CMAKE_CXX_COMPILER_VERSION}).")
    endif()
else()
    message(WARNING
        "DepthWire is validated on GCC and Clang; "
        "compiler '${CMAKE_CXX_COMPILER_ID}' is untested for C++26.")
endif()

include(CTest)
include(GNUInstallDirs)

if(DEPTHWIRE_BUILD_FUZZERS AND NOT BUILD_TESTING)
    message(FATAL_ERROR "DEPTHWIRE_BUILD_FUZZERS requires BUILD_TESTING=ON.")
endif()

if(DEPTHWIRE_BUILD_BENCHMARKS AND NOT DEPTHWIRE_ENABLE_DPDK)
    message(FATAL_ERROR
        "DEPTHWIRE_BUILD_BENCHMARKS requires DEPTHWIRE_ENABLE_DPDK=ON.")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(Dependencies)
include(Warnings)
include(Targets)
include(PackageChecks)
include(Contracts)
include(SourceIdentity)

depthwire_verify_package_boundaries()

# Apply contract build flags (if opted in) before any compiled target is created
# so header checks, tests, apps, and benchmarks all pick them up uniformly.
depthwire_configure_contracts()

if(BUILD_TESTING)
    depthwire_add_public_header_checks()
endif()

if(DEPTHWIRE_ENABLE_DPDK)
    add_subdirectory(support)
endif()

add_subdirectory(apps)

if(DEPTHWIRE_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

include(Install)
