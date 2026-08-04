add_library(
    codex-backend-stream-adapter STATIC
    Configuration.cpp
    Configuration.h
    FrontendStreamSocketContext.cpp
    FrontendStreamSocketContext.h
    FrontendStreamSocketContextFactory.cpp
    FrontendStreamSocketContextFactory.h
    JsonLineFramer.cpp
    JsonLineFramer.h
    ReferenceAuthentication.cpp
    ReferenceAuthentication.h
    ReferenceInvocationPolicy.cpp
    ReferenceInvocationPolicy.h
    UnixPeerCredentials.cpp
    UnixPeerCredentials.h
)
target_include_directories(
    codex-backend-stream-adapter
    PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
)
target_compile_features(codex-backend-stream-adapter PUBLIC cxx_std_20)
target_link_libraries(
    codex-backend-stream-adapter
    PUBLIC AISuite::OpenAICodexFrontend snodec::net-un-stream-legacy
           snodec::net-in-stream-legacy snodec::net-in6-stream-legacy
    PRIVATE snodec::core
)
if(AISUITE_ENABLE_CODEX_FRONTEND_TLS)
    target_compile_definitions(
        codex-backend-stream-adapter PUBLIC AISUITE_CODEX_FRONTEND_TLS=1
    )
    target_link_libraries(
        codex-backend-stream-adapter PUBLIC snodec::net-in-stream-tls
                                            snodec::net-in6-stream-tls
    )
endif()
if(AISUITE_ENABLE_CODEX_FRONTEND_RFCOMM)
    target_compile_definitions(
        codex-backend-stream-adapter PUBLIC AISUITE_CODEX_FRONTEND_RFCOMM=1
    )
    target_link_libraries(
        codex-backend-stream-adapter PUBLIC snodec::net-rc-stream-legacy
                                            snodec::net-rc-stream-tls
    )
endif()

add_executable(codex-backend main.cpp)
target_compile_features(codex-backend PRIVATE cxx_std_20)
target_link_libraries(
    codex-backend
    PRIVATE codex-backend-stream-adapter AISuite::OpenAICodex
            AISuite::OpenAICodexBackend AISuite::OpenAICodexFrontend
            snodec::net-un-stream-legacy snodec::core
)
install(TARGETS codex-backend RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
