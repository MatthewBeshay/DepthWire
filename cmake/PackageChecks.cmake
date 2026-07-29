function(depthwire_verify_package_boundaries)
    get_property(package_names GLOBAL PROPERTY DEPTHWIRE_PACKAGE_NAMES)
    set(registered_headers)

    foreach(package_name IN LISTS package_names)
        get_property(
            target_name GLOBAL PROPERTY "DEPTHWIRE_PACKAGE_TARGET_${package_name}"
        )
        get_target_property(header_files ${target_name} HEADER_SET)

        if(NOT header_files OR header_files MATCHES "-NOTFOUND$")
            message(FATAL_ERROR
                "Public package ${package_name} has no HEADERS file set.")
        endif()

        get_target_property(
            declared_dependencies ${target_name} INTERFACE_LINK_LIBRARIES
        )

        if(declared_dependencies MATCHES "-NOTFOUND$")
            set(declared_dependencies)
        endif()

        set(observed_dependencies)

        foreach(header_file IN LISTS header_files)
            list(FIND registered_headers "${header_file}" duplicate_index)

            if(NOT duplicate_index EQUAL -1)
                message(FATAL_ERROR
                    "Public header belongs to more than one package: ${header_file}")
            endif()

            list(APPEND registered_headers "${header_file}")
            file(
                RELATIVE_PATH include_path
                "${PROJECT_SOURCE_DIR}/include"
                "${header_file}"
            )

            string(
                REGEX MATCH
                [=[^depthwire/([^/]+)/]=]
                package_path_match
                "${include_path}"
            )

            if(NOT package_path_match OR
               NOT CMAKE_MATCH_1 STREQUAL package_name)
                message(FATAL_ERROR
                    "${header_file} is owned by ${package_name}, but its path "
                    "does not live below include/depthwire/${package_name}/.")
            endif()

            file(
                STRINGS "${header_file}" project_include_lines
                REGEX [=[^[ \t]*#[ \t]*include[ \t]*[<"]depthwire/]=]
            )

            foreach(include_line IN LISTS project_include_lines)
                string(
                    REGEX MATCH
                    [=[depthwire/([^/]+)/]=]
                    dependency_path_match
                    "${include_line}"
                )

                if(NOT dependency_path_match)
                    continue()
                endif()

                set(dependency_package "${CMAKE_MATCH_1}")

                if(dependency_package STREQUAL package_name)
                    continue()
                endif()

                get_property(
                    dependency_is_known GLOBAL
                    PROPERTY "DEPTHWIRE_PACKAGE_ALIAS_${dependency_package}"
                    SET
                )

                if(NOT dependency_is_known)
                    message(FATAL_ERROR
                        "${header_file} includes the unknown public package "
                        "${dependency_package}.")
                endif()

                get_property(
                    dependency_alias GLOBAL
                    PROPERTY "DEPTHWIRE_PACKAGE_ALIAS_${dependency_package}"
                )
                list(FIND declared_dependencies "${dependency_alias}" dependency_index)

                if(dependency_index EQUAL -1)
                    message(FATAL_ERROR
                        "${header_file} includes ${dependency_alias}, but "
                        "${target_name} does not declare it as a direct dependency.")
                endif()

                list(APPEND observed_dependencies "${dependency_alias}")
            endforeach()
        endforeach()

        list(REMOVE_DUPLICATES observed_dependencies)

        foreach(declared_dependency IN LISTS declared_dependencies)
            if(NOT declared_dependency MATCHES "^DepthWire::")
                continue()
            endif()

            list(FIND
                observed_dependencies "${declared_dependency}" dependency_index
            )

            if(dependency_index EQUAL -1)
                message(FATAL_ERROR
                    "${target_name} declares unused public package dependency "
                    "${declared_dependency}.")
            endif()
        endforeach()
    endforeach()

    file(
        GLOB_RECURSE on_disk_headers
        LIST_DIRECTORIES FALSE
        CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/include/depthwire/*.h"
        "${PROJECT_SOURCE_DIR}/include/depthwire/*.hpp"
    )

    foreach(header_file IN LISTS on_disk_headers)
        list(FIND registered_headers "${header_file}" registered_index)

        if(registered_index EQUAL -1)
            message(FATAL_ERROR
                "Public header is not owned by an exported target: ${header_file}")
        endif()
    endforeach()
endfunction()

function(depthwire_add_public_header_checks)
    get_property(package_names GLOBAL PROPERTY DEPTHWIRE_PACKAGE_NAMES)
    set(generated_directory "${PROJECT_BINARY_DIR}/generated/header-checks")
    file(MAKE_DIRECTORY "${generated_directory}")

    foreach(package_name IN LISTS package_names)
        get_property(
            target_name GLOBAL PROPERTY "DEPTHWIRE_PACKAGE_TARGET_${package_name}"
        )
        get_target_property(header_files ${target_name} HEADER_SET)

        foreach(header_file IN LISTS header_files)
            file(
                RELATIVE_PATH include_path
                "${PROJECT_SOURCE_DIR}/include"
                "${header_file}"
            )
            string(MAKE_C_IDENTIFIER "${include_path}" check_identifier)
            set(check_target "depthwire_header_check_${check_identifier}")
            set(check_source "${generated_directory}/${check_identifier}.cpp")

            file(
                GENERATE
                OUTPUT "${check_source}"
                CONTENT "#include <${include_path}>\n"
            )

            add_library(${check_target} OBJECT "${check_source}")
            target_compile_features(${check_target} PRIVATE cxx_std_26)
            target_link_libraries(${check_target} PRIVATE ${target_name})
            depthwire_enable_warnings(${check_target})
        endforeach()
    endforeach()
endfunction()
