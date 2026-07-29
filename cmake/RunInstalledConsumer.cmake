foreach(required_variable IN ITEMS
        DEPTHWIRE_SOURCE_DIR
        DEPTHWIRE_BINARY_DIR
        DEPTHWIRE_GENERATOR
        DEPTHWIRE_CXX_COMPILER
        DEPTHWIRE_CTEST_COMMAND)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required.")
    endif()
endforeach()

set(test_root "${DEPTHWIRE_BINARY_DIR}/installed-consumer-test")
set(install_prefix "${test_root}/prefix")
set(consumer_build "${test_root}/build")

file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_root}")

set(install_command
    "${CMAKE_COMMAND}"
    --install "${DEPTHWIRE_BINARY_DIR}"
    --prefix "${install_prefix}"
)

if(DEFINED DEPTHWIRE_CONFIG AND NOT DEPTHWIRE_CONFIG STREQUAL "")
    list(APPEND install_command --config "${DEPTHWIRE_CONFIG}")
endif()

execute_process(
    COMMAND ${install_command}
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error
)

if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "DepthWire install failed (${install_result}).\n"
        "${install_output}\n${install_error}")
endif()

set(configure_command
    "${CMAKE_COMMAND}"
    -S "${DEPTHWIRE_SOURCE_DIR}/tests/package"
    -B "${consumer_build}"
    -G "${DEPTHWIRE_GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${DEPTHWIRE_CXX_COMPILER}"
    "-DCMAKE_PREFIX_PATH=${install_prefix}"
)

if(DEFINED DEPTHWIRE_BUILD_TYPE AND NOT DEPTHWIRE_BUILD_TYPE STREQUAL "")
    list(APPEND configure_command "-DCMAKE_BUILD_TYPE=${DEPTHWIRE_BUILD_TYPE}")
endif()

if(DEFINED DEPTHWIRE_MAKE_PROGRAM AND NOT DEPTHWIRE_MAKE_PROGRAM STREQUAL "")
    list(APPEND configure_command "-DCMAKE_MAKE_PROGRAM=${DEPTHWIRE_MAKE_PROGRAM}")
endif()

execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error
)

if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Installed-consumer configure failed (${configure_result}).\n"
        "${configure_output}\n${configure_error}")
endif()

set(build_command "${CMAKE_COMMAND}" --build "${consumer_build}")

if(DEFINED DEPTHWIRE_CONFIG AND NOT DEPTHWIRE_CONFIG STREQUAL "")
    list(APPEND build_command --config "${DEPTHWIRE_CONFIG}")
endif()

execute_process(
    COMMAND ${build_command}
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)

if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "Installed-consumer build failed (${build_result}).\n"
        "${build_output}\n${build_error}")
endif()

set(
    test_command
    "${DEPTHWIRE_CTEST_COMMAND}"
    --test-dir "${consumer_build}"
    --output-on-failure
)

if(DEFINED DEPTHWIRE_CONFIG AND NOT DEPTHWIRE_CONFIG STREQUAL "")
    list(APPEND test_command -C "${DEPTHWIRE_CONFIG}")
endif()

execute_process(
    COMMAND ${test_command}
    RESULT_VARIABLE test_result
    OUTPUT_VARIABLE test_output
    ERROR_VARIABLE test_error
)

if(NOT test_result EQUAL 0)
    message(FATAL_ERROR
        "Installed-consumer test failed (${test_result}).\n"
        "${test_output}\n${test_error}")
endif()
