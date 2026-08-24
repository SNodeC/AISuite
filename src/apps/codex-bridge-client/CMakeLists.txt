add_executable(
    codex-bridge-client
    ClientSession.cpp
    ClientSession.h
    CommandParser.cpp
    CommandParser.h
    Configuration.cpp
    Configuration.h
    Presenter.cpp
    Presenter.h
    StdinReader.cpp
    StdinReader.h
    main.cpp
)
target_compile_features(codex-bridge-client PRIVATE cxx_std_20)
target_include_directories(codex-bridge-client PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(
    codex-bridge-client
    PRIVATE AISuite::OpenAICodex snodec::core
            snodec::net-un-stream-legacy snodec::net-in-stream-legacy
            snodec::net-in6-stream-legacy
)
if(AISUITE_ENABLE_CODEX_FRONTEND_TLS)
    target_compile_definitions(
        codex-bridge-client PRIVATE AISUITE_CODEX_FRONTEND_TLS=1
    )
    target_link_libraries(
        codex-bridge-client PRIVATE snodec::net-in-stream-tls
                                    snodec::net-in6-stream-tls
    )
endif()
if(AISUITE_ENABLE_CODEX_FRONTEND_RFCOMM)
    target_compile_definitions(
        codex-bridge-client PRIVATE AISUITE_CODEX_FRONTEND_RFCOMM=1
    )
    target_link_libraries(
        codex-bridge-client PRIVATE snodec::net-rc-stream-legacy
                                    snodec::net-rc-stream-tls
    )
endif()
if(AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET
   AND TARGET snodec::websocket-client
)
    target_compile_definitions(
        codex-bridge-client PRIVATE AISUITE_CODEX_FRONTEND_WEBSOCKET=1
    )
    target_link_libraries(
        codex-bridge-client PRIVATE snodec::http-client
                                    snodec::websocket-client
    )
endif()
install(TARGETS codex-bridge-client RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
