cmake_minimum_required(VERSION 3.18)

project(AISuiteInstalledCodexConsumer LANGUAGES CXX)

find_package(AISuite CONFIG REQUIRED)

if(NOT TARGET AISuite::OpenAICodexFrontendProtocol)
    message(FATAL_ERROR "OpenAICodexFrontendProtocol is not exported")
endif()
get_target_property(
    frontend_protocol_links
    AISuite::OpenAICodexFrontendProtocol
    INTERFACE_LINK_LIBRARIES
)
if(NOT "${frontend_protocol_links}" STREQUAL
   "nlohmann_json::nlohmann_json")
    message(
        FATAL_ERROR
            "OpenAICodexFrontendProtocol must export exactly nlohmann_json::nlohmann_json: ${frontend_protocol_links}"
    )
endif()

if(TARGET AISuite::OpenAICodexFrontendClient)
    get_target_property(
        frontend_client_links
        AISuite::OpenAICodexFrontendClient
        INTERFACE_LINK_LIBRARIES
    )
    if(NOT frontend_client_links)
        message(
            FATAL_ERROR
                "OpenAICodexFrontendClient has no exported public link interface"
        )
    endif()
    list(
        FIND frontend_client_links AISuite::OpenAICodexFrontendProtocol
        frontend_protocol_index
    )
    if(frontend_protocol_index EQUAL -1)
        message(
            FATAL_ERROR
                "OpenAICodexFrontendClient does not export its Frontend Protocol dependency: ${frontend_client_links}"
        )
    endif()
    if(NOT frontend_client_links STREQUAL "AISuite::OpenAICodexFrontendProtocol")
        message(
            FATAL_ERROR
                "OpenAICodexFrontendClient must export exactly its Frontend Protocol dependency: ${frontend_client_links}"
        )
    endif()
    foreach(
        forbidden_sdk_target
        IN ITEMS
           AISuite::OpenAICodex
           AISuite::OpenAICodexFrontend
           AISuite::OpenAICodexBackend
    )
        list(FIND frontend_client_links "${forbidden_sdk_target}" forbidden_sdk_index)
        if(NOT forbidden_sdk_index EQUAL -1)
            message(
                FATAL_ERROR
                    "OpenAICodexFrontendClient leaks forbidden SDK dependency ${forbidden_sdk_target}: ${frontend_client_links}"
            )
        endif()
    endforeach()
    foreach(
        forbidden_transport
        IN ITEMS
           net-un
           net-in
           net-in6
           net-rc
           http
           websocket
           OpenSSL
           Bluetooth
           Qt
           Curses
           CURSES
    )
        string(FIND "${frontend_client_links}" "${forbidden_transport}" forbidden_index)
        if(NOT forbidden_index EQUAL -1)
            message(
                FATAL_ERROR
                    "OpenAICodexFrontendClient leaks transport dependency ${forbidden_transport}: ${frontend_client_links}"
            )
        endif()
    endforeach()
endif()

add_executable(
    AISuiteInstalledCodexFrontendProtocolConsumer
    CodexFrontendProtocolConsumer.cpp
)
target_compile_features(
    AISuiteInstalledCodexFrontendProtocolConsumer PRIVATE cxx_std_20
)
target_link_libraries(
    AISuiteInstalledCodexFrontendProtocolConsumer
    PRIVATE AISuite::OpenAICodexFrontendProtocol
)

add_executable(
    AISuiteInstalledSNodeCoreConsumer
    SNodeInstalledCoreConsumer.cpp
)
target_compile_features(
    AISuiteInstalledSNodeCoreConsumer PRIVATE cxx_std_20
)
target_link_libraries(
    AISuiteInstalledSNodeCoreConsumer PRIVATE snodec::core
)

add_executable(AISuiteInstalledCodexConsumer CodexRawConsumer.cpp)
target_compile_features(AISuiteInstalledCodexConsumer PRIVATE cxx_std_20)
target_link_libraries(
    AISuiteInstalledCodexConsumer PRIVATE AISuite::OpenAICodex
)

add_executable(AISuiteInstalledCodexApiConsumer CodexApiConsumer.cpp)
target_compile_features(AISuiteInstalledCodexApiConsumer PRIVATE cxx_std_20)
target_link_libraries(
    AISuiteInstalledCodexApiConsumer PRIVATE AISuite::OpenAICodex
)

add_executable(AISuiteInstalledCodexApiExample CodexApiExample.cpp)
target_compile_features(AISuiteInstalledCodexApiExample PRIVATE cxx_std_20)
target_link_libraries(
    AISuiteInstalledCodexApiExample
    PRIVATE AISuite::OpenAICodex snodec::core
)

foreach(
    domain
    IN ITEMS Accounts Models Configuration Commands Filesystem
             PermissionProfiles Reviews
)
    add_executable(
        AISuiteInstalledCodex${domain}HeaderConsumer
        Codex${domain}HeaderConsumer.cpp
    )
    target_compile_features(
        AISuiteInstalledCodex${domain}HeaderConsumer PRIVATE cxx_std_20
    )
    target_link_libraries(
        AISuiteInstalledCodex${domain}HeaderConsumer
        PRIVATE AISuite::OpenAICodex
    )
endforeach()

add_executable(
    AISuiteInstalledCodexApplicationProjectionConsumer
    CodexApplicationProjectionConsumer.cpp
)
target_compile_features(
    AISuiteInstalledCodexApplicationProjectionConsumer PRIVATE cxx_std_20
)
target_link_libraries(
    AISuiteInstalledCodexApplicationProjectionConsumer
    PRIVATE AISuite::OpenAICodex
)

add_executable(
    AISuiteInstalledCodexBackendFrontendConsumer
    CodexBackendFrontendConsumer.cpp
)
target_compile_features(
    AISuiteInstalledCodexBackendFrontendConsumer PRIVATE cxx_std_20
)
target_link_libraries(
    AISuiteInstalledCodexBackendFrontendConsumer
    PRIVATE AISuite::OpenAICodexBackend AISuite::OpenAICodexFrontend
)

if(TARGET AISuite::OpenAICodexFrontendClient)
    add_executable(
        AISuiteInstalledCodexFrontendClientConsumer
        CodexFrontendClientConsumer.cpp
    )
    target_compile_features(
        AISuiteInstalledCodexFrontendClientConsumer PRIVATE cxx_std_20
    )
    target_link_libraries(
        AISuiteInstalledCodexFrontendClientConsumer
        PRIVATE AISuite::OpenAICodexFrontendClient
    )
endif()
