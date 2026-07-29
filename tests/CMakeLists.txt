function(depthwire_add_test target_name source_file)
    depthwire_add_executable(${target_name} "${source_file}")

    if(MSVC)
        target_compile_options(${target_name} PRIVATE /UNDEBUG)
    else()
        target_compile_options(${target_name} PRIVATE -UNDEBUG)
    endif()

    add_test(
        NAME ${target_name}
        COMMAND ${target_name}
    )
endfunction()

depthwire_add_test(
    depthwire_test_wire_market_data
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/wire/market_data_packet.cpp"
)
target_link_libraries(depthwire_test_wire_market_data PRIVATE DepthWire::Wire)

depthwire_add_test(
    depthwire_test_wire_order
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/wire/order_packet.cpp"
)
target_link_libraries(depthwire_test_wire_order PRIVATE DepthWire::Wire)

depthwire_add_test(
    depthwire_test_wire_invalid_packet
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/wire/invalid_packet.cpp"
)
target_link_libraries(depthwire_test_wire_invalid_packet PRIVATE DepthWire::Wire)

depthwire_add_test(
    depthwire_test_ethernet_market_frame
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/wire/ethernet_market_frame.cpp"
)
target_link_libraries(depthwire_test_ethernet_market_frame PRIVATE DepthWire::Wire)

depthwire_add_test(
    depthwire_test_ethernet_order_frame
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/wire/ethernet_order_frame.cpp"
)
target_link_libraries(depthwire_test_ethernet_order_frame PRIVATE DepthWire::Wire)

depthwire_add_test(
    depthwire_test_engine_wire_adapter
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/pipeline/engine_wire_adapter.cpp"
)
target_link_libraries(depthwire_test_engine_wire_adapter PRIVATE
    DepthWire::Engine
    DepthWire::Pipeline
    DepthWire::Wire
)

depthwire_add_test(
    depthwire_test_engine_scenarios
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/engine/scenarios.cpp"
)
target_link_libraries(depthwire_test_engine_scenarios PRIVATE
    DepthWire::Engine
    DepthWire::Pipeline
    DepthWire::Wire
)

depthwire_add_test(
    depthwire_test_risk_guard
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/risk/risk_guard.cpp"
)
target_link_libraries(depthwire_test_risk_guard PRIVATE DepthWire::Risk)

depthwire_add_test(
    depthwire_test_fixed_l2_book
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/book/fixed_l2_book.cpp"
)
target_link_libraries(depthwire_test_fixed_l2_book PRIVATE DepthWire::Book)

depthwire_add_test(
    depthwire_test_preallocated_outbox
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/order/preallocated_outbox.cpp"
)
target_link_libraries(depthwire_test_preallocated_outbox PRIVATE DepthWire::Order)

depthwire_add_test(
    depthwire_test_engine_sequence_policy
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/engine/sequence_policy.cpp"
)
target_link_libraries(depthwire_test_engine_sequence_policy PRIVATE DepthWire::Engine)

depthwire_add_test(
    depthwire_test_wire_golden_vectors
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/wire/golden_vectors.cpp"
)
target_link_libraries(depthwire_test_wire_golden_vectors PRIVATE DepthWire::Wire)

depthwire_add_test(
    depthwire_test_wire_truncation
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/wire/truncation.cpp"
)
target_link_libraries(depthwire_test_wire_truncation PRIVATE DepthWire::Wire)

depthwire_add_test(
    depthwire_test_packet_processor
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/pipeline/packet_processor.cpp"
)
target_link_libraries(depthwire_test_packet_processor PRIVATE DepthWire::Pipeline)

depthwire_add_test(
    depthwire_test_sha256
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/digest/sha256.cpp"
)
target_link_libraries(depthwire_test_sha256 PRIVATE DepthWire::Digest)

depthwire_add_test(
    depthwire_test_trace_golden_vectors
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/trace/golden_vectors.cpp"
)
target_link_libraries(depthwire_test_trace_golden_vectors PRIVATE
    DepthWire::Digest
    DepthWire::Trace
    DepthWire::Wire
)

depthwire_add_test(
    depthwire_test_trace_validator
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/trace/validator.cpp"
)
target_link_libraries(depthwire_test_trace_validator PRIVATE
    DepthWire::Digest
    DepthWire::Trace
    DepthWire::Wire
)

target_compile_options(depthwire_test_trace_validator PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-fno-ipa-modref>
)

depthwire_add_test(
    depthwire_test_trace_text_round_trip
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/trace/text_round_trip.cpp"
)
target_link_libraries(depthwire_test_trace_text_round_trip PRIVATE
    DepthWire::Trace
    DepthWire::Wire
)

# The fixture test takes the fixture directory as its argument, so it is
# registered directly instead of through depthwire_add_test.
depthwire_add_executable(
    depthwire_test_trace_fixtures
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/trace/fixtures.cpp"
)
target_compile_options(depthwire_test_trace_fixtures PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
)
target_include_directories(depthwire_test_trace_fixtures PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(depthwire_test_trace_fixtures PRIVATE
    DepthWire::Trace
    DepthWire::Wire
)
add_test(
    NAME depthwire_test_trace_fixtures
    COMMAND depthwire_test_trace_fixtures "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/trace"
)

depthwire_add_test(
    depthwire_test_run_digest
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/replay/run_digest.cpp"
)
target_link_libraries(depthwire_test_run_digest PRIVATE
    DepthWire::Digest
    DepthWire::Engine
    DepthWire::Pipeline
    DepthWire::Replay
)

depthwire_add_test(
    depthwire_test_pacer
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/replay/pacer.cpp"
)
target_link_libraries(depthwire_test_pacer PRIVATE DepthWire::Replay)

depthwire_add_test(
    depthwire_test_replay_counters
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/replay/counters.cpp"
)
target_link_libraries(depthwire_test_replay_counters PRIVATE
    DepthWire::Engine
    DepthWire::Pipeline
    DepthWire::Replay
)

# The conformance fixture test takes the fixture directory as its argument.
depthwire_add_executable(
    depthwire_test_conformance_fixture
    "${CMAKE_CURRENT_SOURCE_DIR}/unit/replay/conformance_fixture.cpp"
)
target_compile_options(depthwire_test_conformance_fixture PRIVATE
    $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>
    $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
)
target_link_libraries(depthwire_test_conformance_fixture PRIVATE
    DepthWire::Digest
    DepthWire::Engine
    DepthWire::Pipeline
    DepthWire::Replay
    DepthWire::Trace
    DepthWire::Wire
)
add_test(
    NAME depthwire_test_conformance_fixture
    COMMAND depthwire_test_conformance_fixture "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/trace"
)

# The public CLI, run twice in fresh processes, must reproduce the
# identical digest (docs/architecture/trace-format.md).
add_test(
    NAME depthwire_test_replay_digest_twice
    COMMAND "${CMAKE_COMMAND}"
        "-DDEPTHWIRE_REPLAY_EXECUTABLE=$<TARGET_FILE:depthwire-replay>"
        "-DDEPTHWIRE_TRACE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/trace/conformance_v1.dwtrace"
        -P "${PROJECT_SOURCE_DIR}/cmake/RunReplayDigestTwice.cmake"
)

# The shipped binary's own boundary validation (see the script for why this
# exists in addition to the unit tests).
add_test(
    NAME depthwire_test_replay_validate_rejects
    COMMAND "${CMAKE_COMMAND}"
        "-DDEPTHWIRE_REPLAY_EXECUTABLE=$<TARGET_FILE:depthwire-replay>"
        "-DDEPTHWIRE_TRACE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/trace/conformance_v1.dwtrace"
        "-DDEPTHWIRE_WORK_FILE=${CMAKE_CURRENT_BINARY_DIR}/trailing_bytes.dwtrace"
        -P "${PROJECT_SOURCE_DIR}/cmake/RunReplayValidateRejects.cmake"
)

if(DEPTHWIRE_ENABLE_DPDK)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        depthwire_add_executable(
            depthwire_afpacket_test_peer
            "${CMAKE_CURRENT_SOURCE_DIR}/integration/dpdk/afpacket_peer.cpp"
        )
        target_link_libraries(
            depthwire_afpacket_test_peer PRIVATE DepthWire::Wire
        )
        target_compile_options(depthwire_afpacket_test_peer PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O3>
        )
    else()
        message(STATUS "Skipping AF_PACKET test peer: supported only on Linux.")
    endif()

    depthwire_add_dpdk_executable(
        depthwire_test_dpdk_scenarios
        "${CMAKE_CURRENT_SOURCE_DIR}/integration/dpdk/ring_scenarios.cpp"
    )
    target_link_libraries(depthwire_test_dpdk_scenarios PRIVATE
        DepthWire::Engine
        DepthWire::Pipeline
        DepthWire::Wire
        depthwire_dpdk_test_support
    )
    depthwire_link_ring_pmd(depthwire_test_dpdk_scenarios)

    add_test(
        NAME depthwire_test_dpdk_scenarios_virtual
        COMMAND depthwire_test_dpdk_scenarios
            -l 0
            --no-pci
            --no-huge
            --no-shconf
            --no-telemetry
            --mbuf-pool-ops-name=ring_mp_mc
            --file-prefix=dw_ctest_scenarios
    )

    depthwire_add_dpdk_executable(
        depthwire_test_dpdk_ring_transport
        "${CMAKE_CURRENT_SOURCE_DIR}/integration/dpdk/ring_transport.cpp"
    )
    target_link_libraries(depthwire_test_dpdk_ring_transport PRIVATE
        DepthWire::Wire
        depthwire_dpdk_test_support
    )
    depthwire_link_ring_pmd(depthwire_test_dpdk_ring_transport)

    add_test(
        NAME depthwire_test_dpdk_ring_transport_virtual
        COMMAND depthwire_test_dpdk_ring_transport
            -l 0
            --no-pci
            --no-huge
            --no-shconf
            --no-telemetry
            --mbuf-pool-ops-name=ring_mp_mc
            --file-prefix=dw_ctest_transport
    )

    depthwire_add_dpdk_executable(
        depthwire_test_replay_recovery
        "${CMAKE_CURRENT_SOURCE_DIR}/integration/dpdk/replay_recovery.cpp"
    )
    target_link_libraries(depthwire_test_replay_recovery PRIVATE
        DepthWire::Engine
        DepthWire::Pipeline
        DepthWire::Replay
        DepthWire::Trace
        DepthWire::Wire
        depthwire_dpdk_test_support
    )
    depthwire_link_ring_pmd(depthwire_test_replay_recovery)

    add_test(
        NAME depthwire_test_replay_recovery_virtual
        COMMAND depthwire_test_replay_recovery
            -l 0
            --no-pci
            --no-huge
            --no-shconf
            --no-telemetry
            --mbuf-pool-ops-name=ring_mp_mc
            --file-prefix=dw_ctest_recovery
    )

    # Paced replay smoke: exits nonzero unless every termination invariant
    # (zero backlog, mempool restored, counters conserved) holds.
    add_test(
        NAME depthwire_test_replay_paced_smoke
        COMMAND depthwire-replay run
            "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/trace/conformance_v1.dwtrace"
            --mode=paced
            --sequence-policy=contiguous
            --
            -l 0
            --no-pci
            --no-huge
            --no-shconf
            --no-telemetry
            --mbuf-pool-ops-name=ring_mp_mc
            --file-prefix=dw_ctest_paced_smoke
    )
    set_tests_properties(depthwire_test_replay_paced_smoke PROPERTIES
        LABELS dpdk
        TIMEOUT 60
    )

    # The script asserts loss and backlog actually occurred under overload
    # and that the backlog then drained to zero with exact accounting.
    add_test(
        NAME depthwire_test_replay_overload_recovery
        COMMAND "${CMAKE_COMMAND}"
            "-DDEPTHWIRE_REPLAY_EXECUTABLE=$<TARGET_FILE:depthwire-replay>"
            "-DDEPTHWIRE_TRACE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/trace/load_uniform_100k_v1.dwtrace"
            "-DDEPTHWIRE_FILE_PREFIX=dw_ctest_overload_rec"
            -P "${PROJECT_SOURCE_DIR}/cmake/RunReplayOverloadRecovery.cmake"
    )
    set_tests_properties(depthwire_test_replay_overload_recovery PROPERTIES
        LABELS dpdk
        TIMEOUT 120
    )

    # Digest transport-independence: paced-at-recorded-spacing equals the
    # conformance digest (docs/architecture/trace-format.md).
    add_test(
        NAME depthwire_test_replay_paced_digest
        COMMAND "${CMAKE_COMMAND}"
            "-DDEPTHWIRE_REPLAY_EXECUTABLE=$<TARGET_FILE:depthwire-replay>"
            "-DDEPTHWIRE_TRACE_FILE=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/trace/conformance_v1.dwtrace"
            "-DDEPTHWIRE_FILE_PREFIX=dw_ctest_paced_digest"
            -P "${PROJECT_SOURCE_DIR}/cmake/RunReplayPacedDigest.cmake"
    )
    set_tests_properties(depthwire_test_replay_paced_digest PROPERTIES
        LABELS dpdk
        TIMEOUT 120
    )
endif()

if(DEPTHWIRE_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "DEPTHWIRE_BUILD_FUZZERS requires Clang.")
    endif()

    add_executable(
        depthwire_fuzz_wire_codecs
        "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/wire_codecs.cpp"
    )
    target_compile_features(depthwire_fuzz_wire_codecs PRIVATE cxx_std_26)
    target_link_libraries(depthwire_fuzz_wire_codecs PRIVATE DepthWire::Wire)
    target_compile_options(depthwire_fuzz_wire_codecs PRIVATE
        -UNDEBUG
        -fno-omit-frame-pointer
        -fsanitize=fuzzer,address,undefined
    )
    target_link_options(depthwire_fuzz_wire_codecs PRIVATE
        -fno-omit-frame-pointer
        -fsanitize=fuzzer,address,undefined
    )
    depthwire_enable_warnings(depthwire_fuzz_wire_codecs)

    add_test(
        NAME depthwire_fuzz_wire_codecs_smoke
        COMMAND depthwire_fuzz_wire_codecs
            -dict=${CMAKE_CURRENT_SOURCE_DIR}/fuzz/wire_codecs.dict
            -runs=10000
    )
    set_tests_properties(depthwire_fuzz_wire_codecs_smoke PROPERTIES
        LABELS fuzz
        TIMEOUT 60
    )

    add_executable(
        depthwire_fuzz_trace_decoder
        "${CMAKE_CURRENT_SOURCE_DIR}/fuzz/trace_decoder.cpp"
    )
    target_compile_features(depthwire_fuzz_trace_decoder PRIVATE cxx_std_26)
    target_link_libraries(depthwire_fuzz_trace_decoder PRIVATE
        DepthWire::Trace
        DepthWire::Wire
    )
    target_compile_options(depthwire_fuzz_trace_decoder PRIVATE
        -UNDEBUG
        -fno-omit-frame-pointer
        -fsanitize=fuzzer,address,undefined
    )
    target_link_options(depthwire_fuzz_trace_decoder PRIVATE
        -fno-omit-frame-pointer
        -fsanitize=fuzzer,address,undefined
    )
    depthwire_enable_warnings(depthwire_fuzz_trace_decoder)

    add_test(
        NAME depthwire_fuzz_trace_decoder_smoke
        COMMAND depthwire_fuzz_trace_decoder
            -dict=${CMAKE_CURRENT_SOURCE_DIR}/fuzz/trace_decoder.dict
            -runs=10000
    )
    set_tests_properties(depthwire_fuzz_trace_decoder_smoke PROPERTIES
        LABELS fuzz
        TIMEOUT 60
    )
endif()

if(DEPTHWIRE_ENABLE_PACKAGE_TESTS)
    add_test(
        NAME depthwire_test_installed_package
        COMMAND "${CMAKE_COMMAND}"
            "-DDEPTHWIRE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
            "-DDEPTHWIRE_BINARY_DIR=${PROJECT_BINARY_DIR}"
            "-DDEPTHWIRE_GENERATOR=${CMAKE_GENERATOR}"
            "-DDEPTHWIRE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
            "-DDEPTHWIRE_CONFIG=$<CONFIG>"
            "-DDEPTHWIRE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DDEPTHWIRE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}"
            "-DDEPTHWIRE_CTEST_COMMAND=${CMAKE_CTEST_COMMAND}"
            -P "${PROJECT_SOURCE_DIR}/cmake/RunInstalledConsumer.cmake"
    )
    set_tests_properties(depthwire_test_installed_package PROPERTIES
        LABELS package
        RUN_SERIAL TRUE
    )
endif()
