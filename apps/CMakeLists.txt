# The AF_PACKET engine application requires DPDK; the replay command builds in
# every configuration (its paced mode is compiled in only with DPDK).
if(DEPTHWIRE_ENABLE_DPDK)
    depthwire_add_dpdk_executable(
        depthwire
        "${CMAKE_CURRENT_SOURCE_DIR}/depthwire-engine/main.cpp"
    )

    target_link_libraries(depthwire PRIVATE
        DepthWire::Engine
        DepthWire::Pipeline
        DepthWire::Wire
    )
endif()

depthwire_add_executable(
    depthwire-replay
    "${CMAKE_CURRENT_SOURCE_DIR}/depthwire-replay/main.cpp"
)
depthwire_set_benchmark_identity(depthwire-replay)
target_compile_options(depthwire-replay PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O3>
)
target_link_libraries(depthwire-replay PRIVATE
    DepthWire::Digest
    DepthWire::Engine
    DepthWire::Pipeline
    DepthWire::Replay
    DepthWire::Trace
    DepthWire::Wire
)

# The public executable installs alongside the headers.
install(
    TARGETS depthwire-replay
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

if(DEPTHWIRE_ENABLE_DPDK)
    target_sources(depthwire-replay PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/depthwire-replay/paced_run.cpp"
    )
    target_compile_definitions(depthwire-replay PRIVATE DEPTHWIRE_REPLAY_ENABLE_DPDK=1)
    # Reuses the non-installed virtual-link and latency-recorder support
    # code; DPDK stays at the executable edge.
    target_include_directories(depthwire-replay PRIVATE
        "${PROJECT_SOURCE_DIR}/benchmarks"
    )
    target_link_libraries(depthwire-replay PRIVATE depthwire_dpdk_test_support)
    depthwire_link_ring_pmd(depthwire-replay)
endif()

# Two-host failover and multicast gap-fill (docs/architecture/failover.md):
# kernel sockets only, so it builds in every configuration.
depthwire_add_executable(
    depthwire-ha
    "${CMAKE_CURRENT_SOURCE_DIR}/depthwire-ha/main.cpp"
)
depthwire_set_benchmark_identity(depthwire-ha)
target_compile_options(depthwire-ha PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O3>
)
target_link_libraries(depthwire-ha PRIVATE
    DepthWire::Digest
    DepthWire::Engine
    DepthWire::Ha
    DepthWire::Pipeline
    DepthWire::Replay
    DepthWire::Trace
    DepthWire::Wire
)
