add_library(
    ai-openai-codex2
    STATIC
    bridge/CodexBridge.cpp
    bridge/CodexBridge.h
    bridge/Endpoint.h
    frontend/CodexBridge.cpp
    frontend/CodexBridge.h
    frontend/client/ClientConnection.cpp
    frontend/client/ClientConnection.h
    frontend/client/StreamSocketContext.cpp
    frontend/client/StreamSocketContext.h
    frontend/client/StreamSocketContextFactory.cpp
    frontend/client/StreamSocketContextFactory.h
    frontend/StreamSocketContext.cpp
    frontend/StreamSocketContext.h
    frontend/StreamSocketContextFactory.cpp
    frontend/StreamSocketContextFactory.h
    protocol/Envelope.cpp
    protocol/Envelope.h
    protocol/generated/ProtocolTypes.h
    protocol/JsonLineFramer.cpp
    protocol/JsonLineFramer.h
    provider/ChildExitObserver.cpp
    provider/ChildExitObserver.h
    provider/StdioAppServer.cpp
    provider/StdioAppServer.h
)
add_library(AISuite::OpenAICodex2 ALIAS ai-openai-codex2)
set_target_properties(ai-openai-codex2 PROPERTIES EXPORT_NAME OpenAICodex2)
target_include_directories(
    ai-openai-codex2
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/aisuite>"
)
target_compile_features(ai-openai-codex2 PUBLIC cxx_std_20)
target_link_libraries(
    ai-openai-codex2
    PUBLIC nlohmann_json::nlohmann_json snodec::core
    PRIVATE snodec::net-un-stream-legacy snodec::net-in-stream-legacy
            snodec::net-in6-stream-legacy
)
if(AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET AND TARGET snodec::websocket-server)
    target_sources(
        ai-openai-codex2
        PRIVATE frontend/WebSocketSubProtocol.cpp
                frontend/WebSocketSubProtocol.h
                frontend/WebSocketUpgrade.cpp
                frontend/WebSocketUpgrade.h
                frontend/client/WebSocketClient.cpp
                frontend/client/WebSocketClient.h
                provider/WebSocketAppServer.cpp
                provider/WebSocketAppServer.h
    )
    target_compile_definitions(
        ai-openai-codex2 PUBLIC AISUITE_CODEX2_FRONTEND_WEBSOCKET=1
    )
    target_link_libraries(
        ai-openai-codex2
        PRIVATE snodec::websocket-server snodec::http-client
                snodec::websocket-client
    )
endif()
install(
    TARGETS ai-openai-codex2
    EXPORT AISuiteTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(
    DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aisuite/ai/openai/codex2
    FILES_MATCHING PATTERN "*.h"
)
