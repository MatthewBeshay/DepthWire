# Git-derived source identity for the benchmark and replay executables.
# Exported trees pass -DDEPTHWIRE_SOURCE_REVISION or output prints "unknown",
# which is not publishable (docs/benchmarks/methodology.md).
set(DEPTHWIRE_SOURCE_REVISION "" CACHE STRING
    "Source revision embedded in benchmark output (auto-detected when empty)"
)

set(_depthwire_source_revision "${DEPTHWIRE_SOURCE_REVISION}")

if(_depthwire_source_revision STREQUAL "")
    set(_depthwire_source_revision "unknown")
    find_package(Git QUIET)

    if(Git_FOUND)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" rev-parse --verify HEAD
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            RESULT_VARIABLE _depthwire_git_revision_result
            OUTPUT_VARIABLE _depthwire_git_revision
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )

        if(_depthwire_git_revision_result EQUAL 0)
            set(_depthwire_source_revision "${_depthwire_git_revision}")

            execute_process(
                COMMAND "${GIT_EXECUTABLE}" status
                        --porcelain
                        --untracked-files=normal
                WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
                RESULT_VARIABLE _depthwire_git_status_result
                OUTPUT_VARIABLE _depthwire_git_status
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET
            )

            if(_depthwire_git_status_result EQUAL 0
               AND NOT _depthwire_git_status STREQUAL "")
                string(APPEND _depthwire_source_revision "-dirty")
            endif()
        endif()
    endif()
endif()

function(depthwire_set_benchmark_identity target_name)
    target_compile_definitions(${target_name} PRIVATE
        DEPTHWIRE_PROJECT_VERSION="${PROJECT_VERSION}"
        DEPTHWIRE_SOURCE_REVISION="${_depthwire_source_revision}"
    )
endfunction()
