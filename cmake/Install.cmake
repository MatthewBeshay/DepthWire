include(CMakePackageConfigHelpers)

install(
    TARGETS ${DEPTHWIRE_INSTALL_TARGETS}
    EXPORT DepthWireTargets
    FILE_SET HEADERS DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

install(
    EXPORT DepthWireTargets
    FILE DepthWireTargets.cmake
    NAMESPACE DepthWire::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/DepthWire"
)

configure_package_config_file(
    "${PROJECT_SOURCE_DIR}/cmake/DepthWireConfig.cmake.in"
    "${PROJECT_BINARY_DIR}/DepthWireConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/DepthWire"
)

write_basic_package_version_file(
    "${PROJECT_BINARY_DIR}/DepthWireConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT
)

install(
    FILES
        "${PROJECT_BINARY_DIR}/DepthWireConfig.cmake"
        "${PROJECT_BINARY_DIR}/DepthWireConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/DepthWire"
)

install(
    FILES "${PROJECT_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DOCDIR}"
)
