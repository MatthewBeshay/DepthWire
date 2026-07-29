# Proves the shipped binary itself rejects a trailing-byte trace

configure_file("${DEPTHWIRE_TRACE_FILE}" "${DEPTHWIRE_WORK_FILE}" COPYONLY)
file(APPEND "${DEPTHWIRE_WORK_FILE}" "x")

execute_process(
    COMMAND "${DEPTHWIRE_REPLAY_EXECUTABLE}" validate "${DEPTHWIRE_WORK_FILE}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
)

if(run_result EQUAL 0)
    message(FATAL_ERROR
        "validate accepted a trace with trailing bytes:\n${run_output}"
    )
endif()

if(NOT run_error MATCHES "trailing_bytes")
    message(FATAL_ERROR
        "validate rejected the trace but not as trailing_bytes:\n"
        "${run_output}\n${run_error}"
    )
endif()

message(STATUS "validate correctly rejected trailing bytes")
