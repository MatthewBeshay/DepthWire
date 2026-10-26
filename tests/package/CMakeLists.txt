cmake_minimum_required(VERSION 3.25)

project(DepthWireInstalledConsumer LANGUAGES CXX)

find_package(DepthWire 1.0 CONFIG REQUIRED
    COMPONENTS
        Book
        Contracts
        Digest
        Engine
        Ha
        Market
        Order
        Pipeline
        Replay
        Risk
        Strategy
        Trace
        Wire
)

add_executable(depthwire_installed_consumer main.cpp)
target_compile_features(depthwire_installed_consumer PRIVATE cxx_std_26)
target_link_libraries(depthwire_installed_consumer PRIVATE
    DepthWire::Book
    DepthWire::Contracts
    DepthWire::Digest
    DepthWire::Engine
    DepthWire::Ha
    DepthWire::Market
    DepthWire::Order
    DepthWire::Pipeline
    DepthWire::Replay
    DepthWire::Risk
    DepthWire::Strategy
    DepthWire::Trace
    DepthWire::Wire
)

enable_testing()
add_test(NAME depthwire_installed_consumer COMMAND depthwire_installed_consumer)
