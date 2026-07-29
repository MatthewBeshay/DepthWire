function(depthwire_add_header_library target_name export_name)
    add_library(${target_name} INTERFACE)
    add_library(DepthWire::${export_name} ALIAS ${target_name})
    set_target_properties(${target_name} PROPERTIES EXPORT_NAME ${export_name})

    string(TOLOWER "${export_name}" package_name)
    set_property(GLOBAL APPEND PROPERTY DEPTHWIRE_PACKAGE_NAMES "${package_name}")
    set_property(
        GLOBAL PROPERTY "DEPTHWIRE_PACKAGE_TARGET_${package_name}" "${target_name}"
    )
    set_property(
        GLOBAL PROPERTY "DEPTHWIRE_PACKAGE_ALIAS_${package_name}" "DepthWire::${export_name}"
    )

    target_compile_features(${target_name} INTERFACE cxx_std_26)

    target_include_directories(${target_name} INTERFACE
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )
endfunction()

depthwire_add_header_library(depthwire_contracts Contracts)
target_sources(depthwire_contracts INTERFACE
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
    FILES
        "${PROJECT_SOURCE_DIR}/include/depthwire/contracts/contracts.hpp"
)

depthwire_add_header_library(depthwire_digest Digest)
target_sources(depthwire_digest INTERFACE
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
    FILES
        "${PROJECT_SOURCE_DIR}/include/depthwire/digest/sha256.hpp"
)

depthwire_add_header_library(depthwire_market Market)
target_sources(depthwire_market INTERFACE
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
    FILES
        "${PROJECT_SOURCE_DIR}/include/depthwire/market/types.hpp"
)

depthwire_add_header_library(depthwire_book Book)
target_sources(depthwire_book INTERFACE
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
    FILES
        "${PROJECT_SOURCE_DIR}/include/depthwire/book/fixed_l2_book.hpp"
)
target_link_libraries(depthwire_book INTERFACE
    DepthWire::Contracts
    DepthWire::Market
)

depthwire_add_header_library(depthwire_strategy Strategy)
target_sources(depthwire_strategy INTERFACE
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
    FILES
        "${PROJECT_SOURCE_DIR}/include/depthwire/strategy/imbalance_strategy.hpp"
)
target_link_libraries(depthwire_strategy INTERFACE
    DepthWire::Book
    DepthWire::Market
)

depthwire_add_header_library(depthwire_order Order)
target_sources(depthwire_order INTERFACE
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
    FILES
        "${PROJECT_SOURCE_DIR}/include/depthwire/order/order_request.hpp"
        "${PROJECT_SOURCE_DIR}/include/depthwire/order/preallocated_outbox.hpp"
)
target_link_libraries(depthwire_order INTERFACE DepthWire::Market)

depthwire_add_header_library(depthwire_risk Risk)
target_sources(depthwire_risk INTERFACE
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
    FILES
        "${PROJECT_SOURCE_DIR}/include/depthwire/risk/risk_guard.hpp"
)
target_link_libraries(depthwire_risk INTERFACE
    DepthWire::Contracts
    DepthWire::Market
    DepthWire::Order
)

depthwire_add_header_library(depthwire_engine Engine)
target_sources(depthwire_engine INTERFACE
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
    FILES
        "${PROJECT_SOURCE_DIR}/include/depthwire/engine/trading_engine.hpp"
)
target_link_libraries(depthwire_engine INTERFACE
    DepthWire::Book
    DepthWire::Market
    DepthWire::Order
    DepthWire::Risk
    DepthWire::Strategy
)

depthwire_add_header_library(depthwire_wire Wire)
target_sources(depthwire_wire INTERFACE
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
    FILES
        "${PROJECT_SOURCE_DIR}/include/depthwire/wire/endian.hpp"
        "${PROJECT_SOURCE_DIR}/include/depthwire/wire/ethernet_frame.hpp"
        "${PROJECT_SOURCE_DIR}/include/depthwire/wire/market_data_packet.hpp"
        "${PROJECT_SOURCE_DIR}/include/depthwire/wire/order_packet.hpp"
        "${PROJECT_SOURCE_DIR}/include/depthwire/wire/wire_constants.hpp"
        "${PROJECT_SOURCE_DIR}/include/depthwire/wire/wire_header.hpp"
)
target_link_libraries(depthwire_wire INTERFACE DepthWire::Contracts)

depthwire_add_header_library(depthwire_trace Trace)
target_sources(depthwire_trace INTERFACE
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
    FILES
        "${PROJECT_SOURCE_DIR}/include/depthwire/trace/trace_codec.hpp"
        "${PROJECT_SOURCE_DIR}/include/depthwire/trace/trace_format.hpp"
        "${PROJECT_SOURCE_DIR}/include/depthwire/trace/trace_text.hpp"
        "${PROJECT_SOURCE_DIR}/include/depthwire/trace/trace_validator.hpp"
)
target_link_libraries(depthwire_trace INTERFACE
    DepthWire::Contracts
    DepthWire::Digest
    DepthWire::Wire
)

depthwire_add_header_library(depthwire_pipeline Pipeline)
target_sources(depthwire_pipeline INTERFACE
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
    FILES
        "${PROJECT_SOURCE_DIR}/include/depthwire/pipeline/engine_wire_adapter.hpp"
        "${PROJECT_SOURCE_DIR}/include/depthwire/pipeline/packet_processor.hpp"
)
target_link_libraries(depthwire_pipeline INTERFACE
    DepthWire::Engine
    DepthWire::Market
    DepthWire::Order
    DepthWire::Wire
)

depthwire_add_header_library(depthwire_replay Replay)
target_sources(depthwire_replay INTERFACE
    FILE_SET HEADERS
    BASE_DIRS "${PROJECT_SOURCE_DIR}/include"
    FILES
        "${PROJECT_SOURCE_DIR}/include/depthwire/replay/conformance_runner.hpp"
        "${PROJECT_SOURCE_DIR}/include/depthwire/replay/event_journal.hpp"
        "${PROJECT_SOURCE_DIR}/include/depthwire/replay/pacer.hpp"
        "${PROJECT_SOURCE_DIR}/include/depthwire/replay/replay_counters.hpp"
        "${PROJECT_SOURCE_DIR}/include/depthwire/replay/run_digest.hpp"
)
target_link_libraries(depthwire_replay INTERFACE
    DepthWire::Contracts
    DepthWire::Digest
    DepthWire::Engine
    DepthWire::Pipeline
    DepthWire::Trace
    DepthWire::Wire
)

set(DEPTHWIRE_INSTALL_TARGETS
    depthwire_contracts
    depthwire_digest
    depthwire_market
    depthwire_book
    depthwire_strategy
    depthwire_order
    depthwire_risk
    depthwire_engine
    depthwire_wire
    depthwire_trace
    depthwire_pipeline
    depthwire_replay
)
