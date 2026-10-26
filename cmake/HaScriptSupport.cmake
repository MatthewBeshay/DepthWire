# Shared by the depthwire-ha script tests. Every process writes its report to a
# file, so the roles can run concurrently in one execute_process pipeline.

function(depthwire_conformance_digest output_variable)
    execute_process(
        COMMAND "${DEPTHWIRE_REPLAY_EXECUTABLE}" run "${DEPTHWIRE_TRACE_FILE}" --mode=conformance
        RESULT_VARIABLE run_result
        OUTPUT_VARIABLE run_output
        ERROR_VARIABLE run_error
    )

    if(NOT run_result EQUAL 0 OR NOT run_output MATCHES "digest-sha256: ([0-9a-f]+)")
        message(FATAL_ERROR "conformance replay failed (${run_result}):\n${run_output}\n${run_error}")
    endif()

    set("${output_variable}" "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

function(depthwire_report_value report_file key output_variable)
    file(STRINGS "${report_file}" lines REGEX "^${key}: ")

    if(NOT lines)
        file(READ "${report_file}" report)
        message(FATAL_ERROR "${report_file} has no '${key}' line:\n${report}")
    endif()

    list(GET lines 0 line)
    string(REGEX REPLACE "^${key}: (.*)$" "\\1" value "${line}")
    set("${output_variable}" "${value}" PARENT_SCOPE)
endfunction()

function(depthwire_require_success results)
    foreach(result IN LISTS results)
        if(NOT result EQUAL 0)
            foreach(report_file IN LISTS ARGN)
                if(EXISTS "${report_file}")
                    file(READ "${report_file}" report)
                    message("---- ${report_file}\n${report}")
                endif()
            endforeach()

            message(FATAL_ERROR "a depthwire-ha process failed: exit codes ${results}")
        endif()
    endforeach()
endfunction()
