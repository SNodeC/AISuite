set(AI_OPENAI_CODEX_CPP
    AppServerClient.cpp
    detail/AccountCodec.cpp
    detail/ApprovalCodec.cpp
    detail/CodexErrorInfoCodec.cpp
    detail/ClientOperationCodec.cpp
    detail/CommandCodec.cpp
    detail/ConfigurationCodec.cpp
    detail/ConversationCodec.cpp
    detail/EventDecoder.cpp
    detail/FilesystemCodec.cpp
    detail/ItemDecoder.cpp
    detail/ModelCodec.cpp
    detail/ProtocolCodec.cpp
    detail/ProtocolSurfaceRegistry.cpp
    detail/ReviewCodec.cpp
    detail/ServerRequestDecoder.cpp
    detail/ThreadCodec.cpp
    detail/Transport.cpp
    detail/TurnCodec.cpp
    stdio/Client.cpp
    stdio/StdioTransport.cpp
    typed/Accounts.cpp
    typed/Client.cpp
    typed/Configuration.cpp
    typed/Models.cpp
    typed/Types.cpp
)

set(AI_OPENAI_CODEX_PUBLIC_H
    AppServerClient.h
    Protocol.h
    stdio/Client.h
    typed/Accounts.h
    typed/Client.h
    typed/CodexErrorInfo.h
    typed/Commands.h
    typed/Configuration.h
    typed/Conversation.h
    typed/Events.h
    typed/Filesystem.h
    typed/Items.h
    typed/Models.h
    typed/PermissionProfiles.h
    typed/Reviews.h
    typed/Results.h
    typed/ServerRequests.h
    typed/Threads.h
    typed/Turns.h
    typed/Types.h
)

add_library(ai-openai-codex SHARED ${AI_OPENAI_CODEX_CPP})
add_library(AISuite::OpenAICodex ALIAS ai-openai-codex)
add_library(snodec::ai-openai-codex ALIAS ai-openai-codex)
set_target_properties(ai-openai-codex PROPERTIES EXPORT_NAME OpenAICodex)

target_include_directories(
    ai-openai-codex
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/aisuite>"
)
target_compile_features(ai-openai-codex PUBLIC cxx_std_20)
target_link_libraries(
    ai-openai-codex
    PUBLIC nlohmann_json::nlohmann_json
    PRIVATE core
)
set_target_properties(
    ai-openai-codex
    PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${AISUITE_CODEX_SOVERSION}
        OUTPUT_NAME aisuite-openai-codex
)
install(
    TARGETS ai-openai-codex
    EXPORT AISuiteTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(
    FILES AppServerClient.h Protocol.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aisuite/ai/openai/codex
)
install(
    FILES stdio/Client.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aisuite/ai/openai/codex/stdio
)
install(
    FILES
        typed/Accounts.h
        typed/Client.h
        typed/CodexErrorInfo.h
        typed/Commands.h
        typed/Configuration.h
        typed/Conversation.h
        typed/Events.h
        typed/Filesystem.h
        typed/Items.h
        typed/Models.h
        typed/PermissionProfiles.h
        typed/Reviews.h
        typed/Results.h
        typed/ServerRequests.h
        typed/Threads.h
        typed/Turns.h
        typed/Types.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aisuite/ai/openai/codex/typed
)

add_subdirectory(backend)
add_subdirectory(frontend)
