add_library(depthwire_dpdk_test_support INTERFACE)

target_include_directories(depthwire_dpdk_test_support INTERFACE
    "${PROJECT_SOURCE_DIR}/support"
)

target_link_libraries(depthwire_dpdk_test_support INTERFACE
    DepthWire::Dpdk
    DepthWire::Wire
)
