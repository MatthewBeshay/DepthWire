# depthwire_set_benchmark_identity() comes from cmake/SourceIdentity.cmake.

depthwire_add_dpdk_executable(
    depthwire_ring_latency
    "${CMAKE_CURRENT_SOURCE_DIR}/ring/latency.cpp"
)
depthwire_set_benchmark_identity(depthwire_ring_latency)
target_include_directories(depthwire_ring_latency PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(depthwire_ring_latency PRIVATE
    DepthWire::Engine
    DepthWire::Pipeline
    DepthWire::Wire
    depthwire_dpdk_test_support
)
depthwire_link_ring_pmd(depthwire_ring_latency)

depthwire_add_dpdk_executable(
    depthwire_ring_load
    "${CMAKE_CURRENT_SOURCE_DIR}/ring/load.cpp"
)
depthwire_set_benchmark_identity(depthwire_ring_load)
target_include_directories(depthwire_ring_load PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(depthwire_ring_load PRIVATE
    DepthWire::Engine
    DepthWire::Pipeline
    DepthWire::Wire
    depthwire_dpdk_test_support
)
depthwire_link_ring_pmd(depthwire_ring_load)

if(BUILD_TESTING)
    add_test(
        NAME depthwire_test_ring_load_smoke
        COMMAND depthwire_ring_load
            -l 0
            --no-pci
            --no-huge
            --no-shconf
            --no-telemetry
            --mbuf-pool-ops-name=ring_mp_mc
            --file-prefix=dw_ctest_load
            --
            --warmup=64
            --events=256
            --burst=32
    )
    set_tests_properties(depthwire_test_ring_load_smoke PROPERTIES
        LABELS "benchmark;dpdk"
        TIMEOUT 60
    )
endif()

depthwire_add_dpdk_executable(
    depthwire_ring_regimes
    "${CMAKE_CURRENT_SOURCE_DIR}/ring/regimes.cpp"
)
depthwire_set_benchmark_identity(depthwire_ring_regimes)
target_include_directories(depthwire_ring_regimes PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(depthwire_ring_regimes PRIVATE
    DepthWire::Engine
    DepthWire::Pipeline
    DepthWire::Wire
    depthwire_dpdk_test_support
)
depthwire_link_ring_pmd(depthwire_ring_regimes)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    depthwire_add_executable(
        depthwire_afpacket_benchmark_generator
        "${CMAKE_CURRENT_SOURCE_DIR}/af_packet/generator.cpp"
    )
    depthwire_set_benchmark_identity(depthwire_afpacket_benchmark_generator)
    target_link_libraries(depthwire_afpacket_benchmark_generator PRIVATE
        DepthWire::Wire
    )
    target_compile_options(depthwire_afpacket_benchmark_generator PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O3>
    )

    depthwire_add_dpdk_executable(
        depthwire_afpacket_latency
        "${CMAKE_CURRENT_SOURCE_DIR}/af_packet/latency.cpp"
    )
    depthwire_set_benchmark_identity(depthwire_afpacket_latency)
    target_include_directories(depthwire_afpacket_latency PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_link_libraries(depthwire_afpacket_latency PRIVATE
        DepthWire::Engine
        DepthWire::Pipeline
        DepthWire::Wire
    )
else()
    message(STATUS "Skipping AF_PACKET benchmarks: supported only on Linux.")
endif()
