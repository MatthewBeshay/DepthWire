function(depthwire_extract_digest run_output output_variable)
    if(NOT run_output MATCHES "digest-sha256: ([0-9a-f]+)")
        message(FATAL_ERROR "replay run printed no digest-sha256 line:\n${run_output}")
    endif()

    set("${output_variable}" "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

execute_process(
    COMMAND "${DEPTHWIRE_REPLAY_EXECUTABLE}" run "${DEPTHWIRE_TRACE_FILE}"
            --mode=conformance
    RESULT_VARIABLE conformance_result
    OUTPUT_VARIABLE conformance_output
    ERROR_VARIABLE conformance_error
)

if(NOT conformance_result EQUAL 0)
    message(FATAL_ERROR
        "conformance run failed (${conformance_result}):\n"
        "${conformance_output}\n${conformance_error}"
    )
endif()

depthwire_extract_digest("${conformance_output}" conformance_digest)

# Recorded spacing cannot overload the ring for the small conformance
# fixture, so the paced run accepts every record; late events do not affect
# the digest. The sequence policy must match the conformance default.
execute_process(
    COMMAND "${DEPTHWIRE_REPLAY_EXECUTABLE}" run "${DEPTHWIRE_TRACE_FILE}"
            --mode=paced
            --sequence-policy=contiguous
            --scale-ppm=1000000
            --
            -l 0
            --no-pci
            --no-huge
            --no-shconf
            --no-telemetry
            --mbuf-pool-ops-name=ring_mp_mc
            "--file-prefix=${DEPTHWIRE_FILE_PREFIX}"
    RESULT_VARIABLE paced_result
    OUTPUT_VARIABLE paced_output
    ERROR_VARIABLE paced_error
)

if(NOT paced_result EQUAL 0)
    message(FATAL_ERROR
        "paced run failed (${paced_result}):\n${paced_output}\n${paced_error}"
    )
endif()

depthwire_extract_digest("${paced_output}" paced_digest)

if(NOT paced_output MATCHES "counter dropped: +0\n")
    message(FATAL_ERROR
        "paced run dropped events at recorded spacing; digest comparison "
        "is only meaningful for a lossless run:\n${paced_output}"
    )
endif()

if(NOT conformance_digest STREQUAL paced_digest)
    message(FATAL_ERROR
        "paced digest differs from conformance digest: "
        "${paced_digest} != ${conformance_digest}"
    )
endif()

message(STATUS "paced digest matches conformance digest: ${paced_digest}")
