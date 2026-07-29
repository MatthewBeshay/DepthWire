# Experimental C++26 contract checking (P2900), off by default.

option(DEPTHWIRE_ENABLE_CONTRACTS
    "Prototype: enable C++26 contract checking (GCC only, off by default)" OFF)

set(DEPTHWIRE_CONTRACT_SEMANTIC "enforce" CACHE STRING
    "GCC contract evaluation semantic used when DEPTHWIRE_ENABLE_CONTRACTS is ON")
set_property(CACHE DEPTHWIRE_CONTRACT_SEMANTIC PROPERTY STRINGS
    ignore observe enforce quick_enforce)

function(depthwire_configure_contracts)
    if(NOT DEPTHWIRE_ENABLE_CONTRACTS)
        return()
    endif()

    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR
            "DEPTHWIRE_ENABLE_CONTRACTS requires GCC; ${CMAKE_CXX_COMPILER_ID} has "
            "no C++26 P2900 contract support.")
    endif()

    add_compile_definitions(DEPTHWIRE_CONTRACTS_ENABLED=1)
    add_compile_options(-fcontract-evaluation-semantic=${DEPTHWIRE_CONTRACT_SEMANTIC})
    link_libraries(stdc++exp)

    message(STATUS
        "DepthWire: C++26 contracts ENABLED "
        "(GCC, semantic=${DEPTHWIRE_CONTRACT_SEMANTIC}).")
endfunction()
