set(
    CODEX_BACKEND_CLIENT_SUPPORT_SOURCES
    ClientConnection.cpp
    ClientConnection.h
    ClientAuthentication.cpp
    ClientAuthentication.h
    CommandDrainController.cpp
    CommandDrainController.h
    CodexBackendClientSocketContext.cpp
    CodexBackendClientSocketContext.h
    CodexBackendClientSocketContextFactory.cpp
    CodexBackendClientSocketContextFactory.h
    FrontendWebSocketClient.h
    CommandParser.cpp
    CommandParser.h
    Configuration.cpp
    Configuration.h
    JsonLineFramer.cpp
    JsonLineFramer.h
    Presenter.cpp
    Presenter.h
    StdinReader.cpp
    StdinReader.h
)
if(AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET)
    list(APPEND CODEX_BACKEND_CLIENT_SUPPORT_SOURCES FrontendWebSocketClient.cpp)
endif()
add_library(
    codex-backend-client-support STATIC
    ${CODEX_BACKEND_CLIENT_SUPPORT_SOURCES}
)
target_include_directories(
    codex-backend-client-support
    PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
)
target_compile_features(codex-backend-client-support PUBLIC cxx_std_20)
target_link_libraries(
    codex-backend-client-support
    PUBLIC
        AISuite::OpenAICodexFrontendClient
        codex-reference-authentication
        snodec::net-un-stream-legacy
        snodec::net-in-stream-legacy
        snodec::net-in6-stream-legacy
        snodec::core
)
if(AISUITE_ENABLE_CODEX_FRONTEND_TLS)
    target_compile_definitions(
        codex-backend-client-support PUBLIC AISUITE_CODEX_FRONTEND_TLS=1
    )
    target_link_libraries(
        codex-backend-client-support
        PUBLIC snodec::net-in-stream-tls snodec::net-in6-stream-tls
    )
endif()
if(AISUITE_ENABLE_CODEX_FRONTEND_RFCOMM)
    target_compile_definitions(
        codex-backend-client-support PUBLIC AISUITE_CODEX_FRONTEND_RFCOMM=1
    )
    target_link_libraries(
        codex-backend-client-support
        PUBLIC snodec::net-rc-stream-legacy snodec::net-rc-stream-tls
    )
endif()
if(AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET)
    target_compile_definitions(
        codex-backend-client-support PUBLIC AISUITE_CODEX_FRONTEND_WEBSOCKET=1
    )
    target_link_libraries(
        codex-backend-client-support PUBLIC snodec::websocket-client
    )
endif()

add_executable(codex-backend-client main.cpp)
target_compile_features(codex-backend-client PRIVATE cxx_std_20)
target_link_libraries(codex-backend-client PRIVATE codex-backend-client-support)
install(
    TARGETS codex-backend-client RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
