execute_process(
    COMMAND "${DEPTHWIRE_REPLAY_EXECUTABLE}" run "${DEPTHWIRE_TRACE_FILE}"
            --mode=paced
            # Boundaries deliberately not divisible by the burst size,
            # so batch/segment attribution is exercised.
            --segments=0:1000000,40001:20000,60003:1000000
            --
            -l 0
            --no-pci
            --no-huge
            --no-shconf
            --no-telemetry
            --mbuf-pool-ops-name=ring_mp_mc
            "--file-prefix=${DEPTHWIRE_FILE_PREFIX}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error
)

if(NOT run_result EQUAL 0)
    message(FATAL_ERROR
        "overload-recovery run failed (${run_result}):\n${run_output}\n${run_error}"
    )
endif()

function(depthwire_require pattern what)
    if(NOT run_output MATCHES "${pattern}")
        message(FATAL_ERROR "${what} not satisfied (pattern: ${pattern}):\n${run_output}")
    endif()
endfunction()

function(depthwire_segment_field segment field output_variable)
    if(NOT run_output MATCHES "segment ${segment}: [^\n]*${field}=([0-9]+)")
        message(FATAL_ERROR
            "segment ${segment} has no ${field} field:\n${run_output}"
        )
    endif()

    set("${output_variable}" "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

depthwire_require("Status: +OK" "clean completion")
depthwire_require("counters conserved: +yes" "counter conservation")
depthwire_require("orders reconciled: +yes" "order reconciliation")
depthwire_require("samples reconciled: +yes" "sample reconciliation")
depthwire_require("mempool restored: +yes" "mempool restoration")
depthwire_require("counter backlog-final: +0\n" "zero final backlog")

foreach(segment RANGE 2)
    depthwire_segment_field(${segment} "offered" segment_offered)
    depthwire_segment_field(${segment} "accepted" segment_accepted)
    depthwire_segment_field(${segment} "dropped" segment_dropped)
    math(EXPR segment_classified "${segment_accepted} + ${segment_dropped}")

    if(NOT segment_offered EQUAL segment_classified)
        message(FATAL_ERROR
            "segment ${segment} conservation violated: offered=${segment_offered} "
            "accepted=${segment_accepted} dropped=${segment_dropped}:\n${run_output}"
        )
    endif()
endforeach()

depthwire_require("counter dropped: +[1-9]" "visible loss under overload")
depthwire_segment_field(0 "dropped" below_dropped)
depthwire_segment_field(1 "dropped" overload_dropped)
depthwire_segment_field(1 "backlog-at-end" overload_backlog)
depthwire_segment_field(2 "backlog-at-end" final_backlog)

if(below_dropped GREATER 0)
    message(FATAL_ERROR
        "loss below saturation (segment 0 dropped=${below_dropped}):\n${run_output}"
    )
endif()

if(overload_dropped EQUAL 0)
    message(FATAL_ERROR "no loss in the overload segment:\n${run_output}")
endif()

if(overload_backlog EQUAL 0)
    message(FATAL_ERROR "no backlog built up during overload:\n${run_output}")
endif()

if(NOT final_backlog LESS overload_backlog)
    message(FATAL_ERROR
        "backlog did not decline after the overload segment "
        "(overload=${overload_backlog}, final=${final_backlog}):\n${run_output}"
    )
endif()

if(NOT run_output MATCHES "counter recovery-duration-ns: +([0-9]+)")
    message(FATAL_ERROR "no recovery-duration-ns counter:\n${run_output}")
endif()

if(CMAKE_MATCH_1 EQUAL 0)
    message(FATAL_ERROR
        "recovery duration is zero; the overload backlog never spanned the "
        "reduced-load transition:\n${run_output}"
    )
endif()

message(STATUS
    "overload recovered: overload-backlog=${overload_backlog} "
    "final-backlog=${final_backlog} recovery-duration-ns=${CMAKE_MATCH_1}"
)
