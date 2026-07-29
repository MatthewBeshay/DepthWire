function(depthwire_enable_warnings target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /permissive-
            /W4
            /WX
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
        )
    else()
        message(WARNING
            "DepthWire has no strict warning profile for "
            "${CMAKE_CXX_COMPILER_ID}; continuing without warnings-as-errors.")
    endif()
endfunction()

function(depthwire_enable_sanitisers target_name)
    if(NOT DEPTHWIRE_ENABLE_SANITISERS)
        return()
    endif()

    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR
            "DEPTHWIRE_ENABLE_SANITISERS requires GCC, Clang, or AppleClang.")
    endif()

    target_compile_options(${target_name} PRIVATE
        -fno-omit-frame-pointer
        -fsanitize=address,undefined
    )
    target_link_options(${target_name} PRIVATE
        -fno-omit-frame-pointer
        -fsanitize=address,undefined
    )
endfunction()

function(depthwire_add_executable target_name source_file)
    add_executable(${target_name} ${source_file})
    target_compile_features(${target_name} PRIVATE cxx_std_26)
    depthwire_enable_warnings(${target_name})
    depthwire_enable_sanitisers(${target_name})
endfunction()

function(depthwire_add_dpdk_executable target_name source_file)
    depthwire_add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE DepthWire::Dpdk)
    target_compile_options(${target_name} PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O3>
    )
endfunction()
