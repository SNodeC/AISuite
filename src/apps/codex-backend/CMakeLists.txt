add_library(
    codex-reference-authentication STATIC
    ReferenceAuthentication.cpp
    ReferenceAuthentication.h
)
target_include_directories(
    codex-reference-authentication
    PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
)
target_compile_features(codex-reference-authentication PUBLIC cxx_std_20)
target_link_libraries(
    codex-reference-authentication PUBLIC AISuite::OpenAICodexFrontend
)

add_library(
    codex-backend-stream-adapter STATIC
    Configuration.cpp
    Configuration.h
    FrontendCloseReason.h
    FrontendStreamSocketContext.cpp
    FrontendStreamSocketContext.h
    FrontendStreamSocketContextFactory.cpp
    FrontendStreamSocketContextFactory.h
    FrontendWebSecurity.cpp
    FrontendWebSecurity.h
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
    PUBLIC AISuite::OpenAICodexFrontend codex-reference-authentication snodec::net-un-stream-legacy
           snodec::net-in-stream-legacy snodec::net-in6-stream-legacy
    PRIVATE ai-openai-codex-frontend-jsonl-framer snodec::core
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

if(AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET)
    add_library(
        codex-backend-runtime-bridge
        SHARED
        FrontendCloseReason.h
        FrontendRuntimeBridge.cpp
        FrontendRuntimeBridge.h
        FrontendWebSocketSubProtocol.cpp
        FrontendWebSocketSubProtocol.h
    )
    target_include_directories(
        codex-backend-runtime-bridge
        PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
    )
    target_compile_features(codex-backend-runtime-bridge PUBLIC cxx_std_20)
    target_link_libraries(
        codex-backend-runtime-bridge
        PUBLIC AISuite::OpenAICodexFrontend
        PRIVATE snodec::core snodec::websocket-server
    )
    set_target_properties(
        codex-backend-runtime-bridge
        PROPERTIES OUTPUT_NAME aisuite-codex-backend-runtime
                   VERSION ${PROJECT_VERSION}
                   SOVERSION ${AISUITE_CODEX_SOVERSION}
    )

    add_library(
        codex-backend-web-adapter STATIC
        FrontendWebApplication.cpp FrontendWebApplication.h
    )
    target_include_directories(
        codex-backend-web-adapter
        PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
    )
    target_compile_features(codex-backend-web-adapter PUBLIC cxx_std_20)
    target_compile_definitions(
        codex-backend-web-adapter PUBLIC AISUITE_CODEX_FRONTEND_WEBSOCKET=1
    )
    target_link_libraries(
        codex-backend-web-adapter
        PUBLIC AISuite::OpenAICodexFrontend
        PRIVATE codex-backend-runtime-bridge codex-backend-stream-adapter
                snodec::core snodec::http-server snodec::http-server-express
                snodec::websocket-server
    )

    add_library(
        codex-backend-websocket-subprotocol SHARED
        FrontendWebSocketSubProtocolFactory.cpp
        FrontendWebSocketSubProtocolFactory.h
    )
    target_include_directories(
        codex-backend-websocket-subprotocol
        PRIVATE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
    )
    target_compile_features(
        codex-backend-websocket-subprotocol PRIVATE cxx_std_20
    )
    target_compile_options(
        codex-backend-websocket-subprotocol PRIVATE -fno-gnu-unique
    )
    target_link_libraries(
        codex-backend-websocket-subprotocol
        PRIVATE codex-backend-runtime-bridge AISuite::OpenAICodexFrontend
                snodec::websocket-server
    )
    set_target_properties(
        codex-backend-websocket-subprotocol
        PROPERTIES OUTPUT_NAME snodec-websocket-codex-server
                   VERSION ${PROJECT_VERSION}
                   SOVERSION ${AISUITE_CODEX_SOVERSION}
                   LIBRARY_OUTPUT_DIRECTORY
                   "${CMAKE_CURRENT_BINARY_DIR}/websocket"
    )
    # Configuration.cpp is part of the native stream adapter so all listener
    # options share one CLI authority. Compile its web validation path only when
    # the bound web runtime is present. FrontendWebSecurity.cpp lives there as
    # the one app-private policy implementation in feature-on and feature-off
    # builds; this adapter borrows it instead of compiling a second copy.
    target_compile_definitions(
        codex-backend-stream-adapter PRIVATE AISUITE_CODEX_FRONTEND_WEBSOCKET=1
    )
    if(AISUITE_ENABLE_CODEX_FRONTEND_TLS)
        target_compile_definitions(
            codex-backend-web-adapter PUBLIC AISUITE_CODEX_FRONTEND_TLS=1
        )
    endif()
endif()

add_executable(codex-backend main.cpp)
target_compile_features(codex-backend PRIVATE cxx_std_20)
target_link_libraries(
    codex-backend
    PRIVATE codex-backend-stream-adapter AISuite::OpenAICodex
            AISuite::OpenAICodexBackend AISuite::OpenAICodexFrontend
            snodec::net-un-stream-legacy snodec::core
)
if(AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET)
    target_compile_definitions(
        codex-backend PRIVATE AISUITE_CODEX_FRONTEND_WEBSOCKET=1
    )
    target_link_libraries(
        codex-backend
        PRIVATE codex-backend-runtime-bridge codex-backend-web-adapter
                snodec::http-server-express-legacy-in
                snodec::http-server-express-legacy-in6
    )
    if(AISUITE_ENABLE_CODEX_FRONTEND_TLS)
        target_link_libraries(
            codex-backend PRIVATE snodec::http-server-express-tls-in
                                  snodec::http-server-express-tls-in6
        )
    endif()
endif()
install(TARGETS codex-backend RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET)
    install(
        TARGETS codex-backend-runtime-bridge
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
    install(
        TARGETS codex-backend-websocket-subprotocol
        LIBRARY DESTINATION
                ${WEBSOCKET_SUBPROTOCOL_INSTALL_LIBDIR}/codex-backend
    )
endif()
