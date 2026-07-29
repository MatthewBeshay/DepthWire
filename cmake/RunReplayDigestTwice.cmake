function(depthwire_run_replay_once output_digest_variable)
    execute_process(
        COMMAND "${DEPTHWIRE_REPLAY_EXECUTABLE}" run "${DEPTHWIRE_TRACE_FILE}"
                --mode=conformance
        RESULT_VARIABLE run_result
        OUTPUT_VARIABLE run_output
        ERROR_VARIABLE run_error
    )

    if(NOT run_result EQUAL 0)
        message(FATAL_ERROR
            "replay run failed (${run_result}):\n${run_output}\n${run_error}"
        )
    endif()

    if(NOT run_output MATCHES "Status: +OK")
        message(FATAL_ERROR "replay run did not report Status OK:\n${run_output}")
    endif()

    if(NOT run_output MATCHES "digest-sha256: ([0-9a-f]+)")
        message(FATAL_ERROR "replay run printed no digest-sha256 line:\n${run_output}")
    endif()

    string(LENGTH "${CMAKE_MATCH_1}" digest_length)

    if(NOT digest_length EQUAL 64)
        message(FATAL_ERROR "digest-sha256 line is not a SHA-256 hex digest:\n${run_output}")
    endif()

    set("${output_digest_variable}" "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

depthwire_run_replay_once(first_digest)
depthwire_run_replay_once(second_digest)

if(NOT first_digest STREQUAL second_digest)
    message(FATAL_ERROR
        "cross-process digest mismatch: ${first_digest} != ${second_digest}"
    )
endif()

message(STATUS "cross-process digest reproduced: ${first_digest}")
