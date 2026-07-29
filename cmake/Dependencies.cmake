if(NOT DEPTHWIRE_ENABLE_DPDK)
    return()
endif()

find_package(PkgConfig REQUIRED)
set(DEPTHWIRE_MINIMUM_DPDK_VERSION "24.11")
pkg_check_modules(
    DPDK REQUIRED IMPORTED_TARGET
    "libdpdk>=${DEPTHWIRE_MINIMUM_DPDK_VERSION}"
)
pkg_get_variable(DPDK_LIBRARY_DIRECTORY libdpdk libdir)

find_library(DEPTHWIRE_DPDK_NET_RING_LIBRARY
    NAMES rte_net_ring
    HINTS "${DPDK_LIBRARY_DIRECTORY}"
    PATH_SUFFIXES dpdk-pmds
)

if(NOT DEPTHWIRE_DPDK_NET_RING_LIBRARY)
    message(FATAL_ERROR
        "Could not find librte_net_ring, required by the virtual Ring PMD targets.")
endif()

get_filename_component(DEPTHWIRE_DPDK_NET_RING_DIRECTORY
    "${DEPTHWIRE_DPDK_NET_RING_LIBRARY}"
    DIRECTORY
)

message(STATUS "DPDK Ring PMD library: ${DEPTHWIRE_DPDK_NET_RING_LIBRARY}")

add_library(depthwire_dpdk_dependency INTERFACE)
add_library(DepthWire::Dpdk ALIAS depthwire_dpdk_dependency)

target_link_libraries(depthwire_dpdk_dependency INTERFACE
    PkgConfig::DPDK
)

function(depthwire_link_ring_pmd target_name)
    target_link_libraries(${target_name} PRIVATE
        "${DEPTHWIRE_DPDK_NET_RING_LIBRARY}"
    )

    set_property(
        TARGET ${target_name}
        APPEND PROPERTY BUILD_RPATH "${DEPTHWIRE_DPDK_NET_RING_DIRECTORY}"
    )
endfunction()
