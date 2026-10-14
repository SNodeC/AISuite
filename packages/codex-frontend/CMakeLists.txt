# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

cmake_minimum_required(VERSION 3.18)

project(AISuiteFrontend LANGUAGES NONE)
include(CMakePackageConfigHelpers)
set(CMAKE_INSTALL_DATADIR
    "share"
    CACHE PATH "Read-only architecture-independent data directory"
)
find_program(AISUITE_FRONTEND_NODE node REQUIRED)
find_program(AISUITE_FRONTEND_NPM NAMES npm npm.cmd REQUIRED)
execute_process(
    COMMAND "${AISUITE_FRONTEND_NODE}" -p "require('./package.json').version"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE AISuiteFrontend_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE sdk_version_result
)
if(NOT sdk_version_result EQUAL 0)
    message(FATAL_ERROR "Cannot read the frontend SDK package version")
endif()

set(sdk_stage "${CMAKE_CURRENT_BINARY_DIR}/package")
set(sdk_archive "${CMAKE_CURRENT_BINARY_DIR}/aisuite-frontend.tgz")
file(GLOB_RECURSE sdk_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.ts"
)
set(sdk_inputs
    ${sdk_sources}
    "${CMAKE_CURRENT_SOURCE_DIR}/package.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/package-lock.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/tsconfig.json"
    "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSE-MIT"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSE-LGPL-3.0-or-later"
)
set_property(
    DIRECTORY
    APPEND
    PROPERTY CMAKE_CONFIGURE_DEPENDS ${sdk_inputs}
)
file(REMOVE_RECURSE "${sdk_stage}")
file(MAKE_DIRECTORY "${sdk_stage}")
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/src" DESTINATION "${sdk_stage}")
file(
    COPY "${CMAKE_CURRENT_SOURCE_DIR}/package.json"
         "${CMAKE_CURRENT_SOURCE_DIR}/package-lock.json"
         "${CMAKE_CURRENT_SOURCE_DIR}/tsconfig.json"
         "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
         "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSE-MIT"
         "${CMAKE_CURRENT_SOURCE_DIR}/../../LICENSE-LGPL-3.0-or-later"
    DESTINATION "${sdk_stage}"
)
add_custom_command(
    OUTPUT "${sdk_archive}"
    COMMAND "${AISUITE_FRONTEND_NPM}" ci --no-audit --no-fund
    COMMAND "${AISUITE_FRONTEND_NPM}" pack --pack-destination
            "${CMAKE_CURRENT_BINARY_DIR}"
    COMMAND
        "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_BINARY_DIR}/snodec-codex-frontend-${AISuiteFrontend_VERSION}.tgz"
        "${sdk_archive}"
    DEPENDS ${sdk_inputs}
    WORKING_DIRECTORY "${sdk_stage}"
    VERBATIM
)
add_custom_target(aisuite-frontend-package ALL DEPENDS "${sdk_archive}")
set(AISuiteFrontend_INSTALL_DIR "${CMAKE_INSTALL_DATADIR}/aisuite/npm")
configure_package_config_file(
    AISuiteFrontendConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/AISuiteFrontendConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_DATADIR}/cmake/AISuiteFrontend"
    PATH_VARS AISuiteFrontend_INSTALL_DIR
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/AISuiteFrontendConfigVersion.cmake"
    VERSION "${AISuiteFrontend_VERSION}"
    COMPATIBILITY SameMajorVersion
)
install(
    FILES "${sdk_archive}"
    DESTINATION "${AISuiteFrontend_INSTALL_DIR}"
    COMPONENT AISuiteFrontend
)
install(
    FILES "${CMAKE_CURRENT_BINARY_DIR}/AISuiteFrontendConfig.cmake"
          "${CMAKE_CURRENT_BINARY_DIR}/AISuiteFrontendConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/cmake/AISuiteFrontend"
    COMPONENT AISuiteFrontend
)
