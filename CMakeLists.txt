cmake_minimum_required(VERSION 3.18)

project(
    AISuite
    VERSION 0.1.0
    DESCRIPTION "Reusable C++ AI integrations based on SNode.C"
    LANGUAGES CXX
)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(AISUITE_BUILD_APPS "Build AISuite applications" ON)
option(AISUITE_BUILD_TESTS "Build AISuite tests" OFF)
option(
    AISUITE_BUILD_CODEX_FRONTEND_CLIENT
    "Build the transport-neutral Codex C++ Frontend SDK"
    ON
)
option(AISUITE_ENABLE_ASAN "Build AISuite with AddressSanitizer" OFF)
option(AISUITE_DISABLE_LOGLEVEL_LOGGING "Disable SNode.C LOG/PLOG logging" OFF)
option(AISUITE_DISABLE_VERBOSE_LOGGING "Disable SNode.C VLOG logging" OFF)

set(AISUITE_CODEX_SOVERSION 2)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_BUILD_RPATH_USE_ORIGIN TRUE)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH TRUE)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -Werror
        -Wall
        -Wextra
        -Wconversion
        -Wuninitialized
        -Wunreachable-code
        -Wfloat-equal
        -fexec-charset=UTF-8
        -ftemplate-backtrace-limit=0
        -Wno-shadow
        -Wno-psabi
        $<$<CXX_COMPILER_ID:Clang>:-Wno-used-but-marked-unused>
        $<$<CXX_COMPILER_ID:Clang>:-Weverything>
        $<$<CXX_COMPILER_ID:Clang>:-Wno-c++98-compat-pedantic>
        $<$<CXX_COMPILER_ID:Clang>:-Wno-exit-time-destructors>
        $<$<CXX_COMPILER_ID:Clang>:-Wno-global-constructors>
        $<$<CXX_COMPILER_ID:Clang>:-Wno-shadow-field>
        $<$<CXX_COMPILER_ID:Clang>:-Wno-padded>
        $<$<CXX_COMPILER_ID:Clang>:-Wno-documentation-unknown-command>
        $<$<AND:$<CXX_COMPILER_ID:Clang>,$<VERSION_GREATER_EQUAL:$<CXX_COMPILER_VERSION>,16.0>>:-Wno-unsafe-buffer-usage>
        $<$<AND:$<CXX_COMPILER_ID:Clang>,$<VERSION_GREATER_EQUAL:$<CXX_COMPILER_VERSION>,18.0>>:-Wno-switch-default>
    )
endif()

if(AISUITE_ENABLE_ASAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "AISUITE_ENABLE_ASAN requires GCC or Clang")
    endif()
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer -g)
    add_link_options(LINKER:--as-needed -fsanitize=address)
else()
    add_link_options(LINKER:--as-needed LINKER:--no-undefined)
endif()

if(AISUITE_DISABLE_LOGLEVEL_LOGGING)
    add_compile_definitions(SNODEC_DISABLE_LOGLEVEL_LOGGING)
endif()
if(AISUITE_DISABLE_VERBOSE_LOGGING)
    add_compile_definitions(SNODEC_DISABLE_VERBOSE_LOGGING)
endif()

find_package(nlohmann_json 3.11 CONFIG QUIET)
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_path(AISUITE_NLOHMANN_JSON_INCLUDE_DIR nlohmann/json.hpp REQUIRED)
    add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
    set_target_properties(
        nlohmann_json::nlohmann_json
        PROPERTIES INTERFACE_INCLUDE_DIRECTORIES
                   "${AISUITE_NLOHMANN_JSON_INCLUDE_DIR}"
    )
endif()
set(AISUITE_SNODEC_COMPONENTS core)
if(AISUITE_BUILD_APPS OR AISUITE_BUILD_TESTS)
    list(APPEND AISUITE_SNODEC_COMPONENTS net-un-stream-legacy
         net-in-stream-legacy net-in6-stream-legacy
    )
endif()
find_package(snodec 2.0 CONFIG REQUIRED COMPONENTS ${AISUITE_SNODEC_COMPONENTS})

set(AISUITE_CODEX_FRONTEND_TLS_DEFAULT OFF)
if(EXISTS "${snodec_DIR}/snodec_net-in-stream-tls_Targets.cmake"
   AND EXISTS "${snodec_DIR}/snodec_net-in6-stream-tls_Targets.cmake"
)
    set(AISUITE_CODEX_FRONTEND_TLS_DEFAULT ON)
endif()
option(AISUITE_ENABLE_CODEX_FRONTEND_TLS
       "Build Codex TLS frontend stream transports"
       ${AISUITE_CODEX_FRONTEND_TLS_DEFAULT}
)

set(AISUITE_CODEX_FRONTEND_WEBSOCKET_DEFAULT OFF)
if(EXISTS "${snodec_DIR}/snodec_http-server_Targets.cmake"
   AND EXISTS "${snodec_DIR}/snodec_http-server-express_Targets.cmake"
   AND EXISTS "${snodec_DIR}/snodec_http-server-express-legacy-in_Targets.cmake"
   AND EXISTS
       "${snodec_DIR}/snodec_http-server-express-legacy-in6_Targets.cmake"
   AND EXISTS "${snodec_DIR}/snodec_websocket-server_Targets.cmake"
)
    set(AISUITE_CODEX_FRONTEND_WEBSOCKET_DEFAULT ON)
endif()
option(AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET
       "Build Codex HTTP and WebSocket frontend transports"
       ${AISUITE_CODEX_FRONTEND_WEBSOCKET_DEFAULT}
)

set(AISUITE_CODEX_FRONTEND_RFCOMM_DEFAULT OFF)
if(EXISTS "${snodec_DIR}/snodec_net-rc-stream-legacy_Targets.cmake"
   AND EXISTS "${snodec_DIR}/snodec_net-rc-stream-tls_Targets.cmake"
)
    set(AISUITE_CODEX_FRONTEND_RFCOMM_DEFAULT ON)
endif()
option(AISUITE_ENABLE_CODEX_FRONTEND_RFCOMM
       "Build Codex RFCOMM frontend stream transports"
       ${AISUITE_CODEX_FRONTEND_RFCOMM_DEFAULT}
)

set(AISUITE_SNODEC_OPTIONAL_COMPONENTS)
if(AISUITE_BUILD_APPS OR AISUITE_BUILD_TESTS)
    if(AISUITE_ENABLE_CODEX_FRONTEND_TLS)
        list(APPEND AISUITE_SNODEC_OPTIONAL_COMPONENTS net-in-stream-tls
             net-in6-stream-tls
        )
    endif()
    if(AISUITE_ENABLE_CODEX_FRONTEND_RFCOMM)
        list(APPEND AISUITE_SNODEC_OPTIONAL_COMPONENTS net-rc-stream-legacy
             net-rc-stream-tls
        )
    endif()
    if(AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET)
        list(
            APPEND
            AISUITE_SNODEC_OPTIONAL_COMPONENTS
            http-server
            http-server-express
            http-server-express-legacy-in
            http-server-express-legacy-in6
            websocket-server
        )
        if(AISUITE_ENABLE_CODEX_FRONTEND_TLS)
            list(APPEND AISUITE_SNODEC_OPTIONAL_COMPONENTS
                 http-server-express-tls-in http-server-express-tls-in6
            )
        endif()
        list(APPEND AISUITE_SNODEC_OPTIONAL_COMPONENTS websocket-client)
    endif()
endif()
if(AISUITE_SNODEC_OPTIONAL_COMPONENTS)
    find_package(
        snodec CONFIG REQUIRED COMPONENTS ${AISUITE_SNODEC_OPTIONAL_COMPONENTS}
    )
endif()

add_subdirectory(src/ai/openai/codex)

if(AISUITE_BUILD_APPS)
    add_subdirectory(src/apps/codex-backend)
    add_subdirectory(src/apps/codex-backend-client)
endif()

if(AISUITE_BUILD_TESTS)
    include(CTest)
    enable_testing()
    add_subdirectory(tests)
endif()

configure_package_config_file(
    cmake/AISuiteConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/AISuiteConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/AISuite"
    NO_SET_AND_CHECK_MACRO
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/AISuiteConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMinorVersion
)

install(
    EXPORT AISuiteTargets
    FILE AISuiteTargets.cmake
    NAMESPACE AISuite::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/AISuite"
)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/AISuiteConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/AISuiteConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/AISuite"
)

set(CPACK_PACKAGE_NAME "AISuite")
set(CPACK_PACKAGE_VENDOR "SNodeC")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_GENERATOR "TGZ")
set(CPACK_SOURCE_GENERATOR "TGZ")
set(CPACK_SOURCE_IGNORE_FILES
    "/[.]git/"
    "/build[^/]*/"
    "/stage[^/]*/"
    "/package[^/]*/"
    "~$"
)
include(CPack)
