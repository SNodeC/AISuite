add_executable(
    codex-bridge
    Configuration.cpp
    Configuration.h
    ProviderApplication.cpp
    ProviderApplication.h
    main.cpp
)
target_compile_features(codex-bridge PRIVATE cxx_std_20)
target_include_directories(codex-bridge PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(
    codex-bridge
    PRIVATE AISuite::OpenAICodex snodec::core
            snodec::net-un-stream-legacy snodec::net-in-stream-legacy
            snodec::net-in6-stream-legacy
)
if(AISUITE_ENABLE_CODEX_FRONTEND_TLS)
    target_compile_definitions(
        codex-bridge PRIVATE AISUITE_CODEX_FRONTEND_TLS=1
    )
    target_link_libraries(
        codex-bridge PRIVATE snodec::net-in-stream-tls
                             snodec::net-in6-stream-tls
    )
endif()
if(AISUITE_ENABLE_CODEX_FRONTEND_RFCOMM)
    target_compile_definitions(
        codex-bridge PRIVATE AISUITE_CODEX_FRONTEND_RFCOMM=1
    )
    target_link_libraries(
        codex-bridge PRIVATE snodec::net-rc-stream-legacy
                             snodec::net-rc-stream-tls
    )
endif()
if(AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET
   AND TARGET snodec::http-server-express-legacy-in
   AND TARGET snodec::http-server-express-legacy-in6
)
    target_sources(
        codex-bridge PRIVATE WebSocketApplication.cpp WebSocketApplication.h
    )
    target_compile_definitions(
        codex-bridge PRIVATE AISUITE_CODEX_FRONTEND_WEBSOCKET=1
    )
    target_link_libraries(
        codex-bridge
        PRIVATE snodec::http-server-express-legacy-in
                snodec::http-server-express-legacy-in6
    )
    if(AISUITE_ENABLE_CODEX_FRONTEND_TLS)
        target_link_libraries(
            codex-bridge
            PRIVATE snodec::http-server-express-tls-in
                    snodec::http-server-express-tls-in6
        )
    endif()
endif()
install(TARGETS codex-bridge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
