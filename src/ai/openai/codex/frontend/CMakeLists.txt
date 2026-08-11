set(AI_OPENAI_CODEX_FRONTEND_PROTOCOL_CPP
    Codec.cpp Messages.cpp detail/GeneratedSchemaValidator.cpp
)

set(AI_OPENAI_CODEX_FRONTEND_CPP
    FrontendService.cpp EventCoalescer.cpp EventJournal.cpp UpdateBatch.cpp
    detail/BackendCommandMapper.cpp detail/FrontendProjection.cpp
    detail/ProviderResultProjection.cpp
    internal/server/BackendCoreBridge.cpp
)
set(AI_OPENAI_CODEX_FRONTEND_PUBLIC_H
    FrontendService.h Codec.h EventCoalescer.h EventJournal.h GeneratedProtocol.h
    Messages.h Protocol.h Security.h UpdateBatch.h
)

add_library(
    ai-openai-codex-frontend-protocol SHARED
    ${AI_OPENAI_CODEX_FRONTEND_PROTOCOL_CPP}
)
add_library(
    AISuite::OpenAICodexFrontendProtocol ALIAS
    ai-openai-codex-frontend-protocol
)
set_target_properties(
    ai-openai-codex-frontend-protocol
    PROPERTIES
        EXPORT_NAME OpenAICodexFrontendProtocol
        VERSION ${PROJECT_VERSION}
        SOVERSION ${AISUITE_CODEX_SOVERSION}
        OUTPUT_NAME aisuite-openai-codex-frontend-protocol
)
target_include_directories(
    ai-openai-codex-frontend-protocol
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/aisuite>"
)
target_compile_features(ai-openai-codex-frontend-protocol PUBLIC cxx_std_20)
target_link_libraries(
    ai-openai-codex-frontend-protocol
    PUBLIC nlohmann_json::nlohmann_json
)

add_library(
    ai-openai-codex-frontend-model STATIC
    internal/model/Journal.cpp
    internal/model/Model.cpp
    internal/model/Occurrence.cpp
    internal/model/Projection.cpp
)
set_target_properties(
    ai-openai-codex-frontend-model
    PROPERTIES POSITION_INDEPENDENT_CODE ON
)
target_include_directories(
    ai-openai-codex-frontend-model
    PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
)
target_compile_features(ai-openai-codex-frontend-model PUBLIC cxx_std_20)
target_link_libraries(
    ai-openai-codex-frontend-model
    PUBLIC AISuite::OpenAICodexFrontendProtocol
)

add_library(
    ai-openai-codex-frontend-server-core STATIC
    internal/server/BackendProjection.cpp
    internal/server/ServerCore.cpp
)
set_target_properties(
    ai-openai-codex-frontend-server-core
    PROPERTIES POSITION_INDEPENDENT_CODE ON
)
target_include_directories(
    ai-openai-codex-frontend-server-core
    PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
)
target_compile_features(
    ai-openai-codex-frontend-server-core PUBLIC cxx_std_20
)
target_link_libraries(
    ai-openai-codex-frontend-server-core
    PUBLIC
        ai-openai-codex-frontend-model
        AISuite::OpenAICodexFrontendProtocol
        AISuite::OpenAICodexBackend
        AISuite::OpenAICodex
)

add_library(
    ai-openai-codex-frontend-client-core STATIC
    internal/client/ClientCore.cpp
)
set_target_properties(
    ai-openai-codex-frontend-client-core
    PROPERTIES POSITION_INDEPENDENT_CODE ON
)
target_include_directories(
    ai-openai-codex-frontend-client-core
    PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
)
target_compile_features(
    ai-openai-codex-frontend-client-core PUBLIC cxx_std_20
)
target_link_libraries(
    ai-openai-codex-frontend-client-core
    PUBLIC
        ai-openai-codex-frontend-model
        AISuite::OpenAICodexFrontendProtocol
        AISuite::OpenAICodex
)

add_library(ai-openai-codex-frontend SHARED ${AI_OPENAI_CODEX_FRONTEND_CPP})
add_library(AISuite::OpenAICodexFrontend ALIAS ai-openai-codex-frontend)
set_target_properties(
    ai-openai-codex-frontend PROPERTIES EXPORT_NAME OpenAICodexFrontend
)
target_include_directories(
    ai-openai-codex-frontend
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/aisuite>"
)
target_compile_features(ai-openai-codex-frontend PUBLIC cxx_std_20)
target_compile_definitions(
    ai-openai-codex-frontend
    PRIVATE
        AISUITE_CODEX_CPP_CLIENT_SDK_BUILT=$<BOOL:${AISUITE_BUILD_CODEX_FRONTEND_CLIENT}>
)
target_link_libraries(
    ai-openai-codex-frontend
    PUBLIC
        AISuite::OpenAICodexBackend
        AISuite::OpenAICodexFrontendProtocol
    PRIVATE
        ai-openai-codex-frontend-server-core
        snodec::core
)
set_target_properties(
    ai-openai-codex-frontend
    PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${AISUITE_CODEX_SOVERSION}
        OUTPUT_NAME aisuite-openai-codex-frontend
)
install(
    TARGETS ai-openai-codex-frontend-protocol ai-openai-codex-frontend
    EXPORT AISuiteTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(
    FILES ${AI_OPENAI_CODEX_FRONTEND_PUBLIC_H}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aisuite/ai/openai/codex/frontend
)

if(AISUITE_BUILD_CODEX_FRONTEND_CLIENT)
    add_subdirectory(client)
endif()
