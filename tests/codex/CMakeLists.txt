add_executable(CodexJsonLineFramerTest JsonLineFramerTest.cpp)
add_executable(CodexFrontendSdkTest FrontendSdkTest.cpp)
add_executable(CodexBridgeRoutingTest BridgeRoutingTest.cpp)
add_executable(CodexFakeStdioAppServer FakeStdioAppServer.cpp)
add_executable(CodexStdioProviderAcceptanceTest
               StdioProviderAcceptanceTest.cpp)

foreach(target IN ITEMS CodexJsonLineFramerTest CodexFrontendSdkTest
                        CodexBridgeRoutingTest)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${target} PRIVATE AISuite::OpenAICodex)
endforeach()

target_compile_features(CodexFakeStdioAppServer PRIVATE cxx_std_20)
target_link_libraries(CodexFakeStdioAppServer PRIVATE nlohmann_json::nlohmann_json)
target_compile_features(CodexStdioProviderAcceptanceTest PRIVATE cxx_std_20)
target_include_directories(CodexStdioProviderAcceptanceTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(CodexStdioProviderAcceptanceTest
                      PRIVATE AISuite::OpenAICodex snodec::core)

add_test(NAME CodexJsonLineFramerTest COMMAND CodexJsonLineFramerTest)
add_test(NAME CodexFrontendSdkTest COMMAND CodexFrontendSdkTest)
add_test(NAME CodexBridgeRoutingTest COMMAND CodexBridgeRoutingTest)
add_test(
    NAME CodexProvider_stdio
    COMMAND CodexStdioProviderAcceptanceTest
            $<TARGET_FILE:CodexFakeStdioAppServer>
)

set_tests_properties(
    CodexJsonLineFramerTest CodexFrontendSdkTest CodexBridgeRoutingTest
    PROPERTIES LABELS "codex;focused" TIMEOUT 5
)
set_tests_properties(
    CodexProvider_stdio
    PROPERTIES LABELS "codex;provider;transport;stdio;jsonl" TIMEOUT 10
)

add_executable(CodexStreamTransportAcceptanceTest
               StreamTransportAcceptanceTest.cpp)
target_compile_features(CodexStreamTransportAcceptanceTest PRIVATE cxx_std_20)
target_include_directories(CodexStreamTransportAcceptanceTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
    CodexStreamTransportAcceptanceTest
    PRIVATE AISuite::OpenAICodex snodec::core snodec::net-un-stream-legacy
            snodec::net-in-stream-legacy snodec::net-in6-stream-legacy
)

set(codex_tls_directory "${CMAKE_CURRENT_BINARY_DIR}/tls")
set(codex_tls_certificate "${codex_tls_directory}/loopback-cert.pem")
set(codex_tls_key "${codex_tls_directory}/loopback-key.pem")
find_program(AISUITE_CODEX_OPENSSL_EXECUTABLE openssl)
if(AISUITE_ENABLE_CODEX_FRONTEND_TLS
   AND AISUITE_CODEX_OPENSSL_EXECUTABLE
   AND TARGET snodec::net-in-stream-tls
   AND TARGET snodec::net-in6-stream-tls
)
    target_compile_definitions(CodexStreamTransportAcceptanceTest
                               PRIVATE AISUITE_CODEX_TEST_TLS=1)
    target_link_libraries(CodexStreamTransportAcceptanceTest
                          PRIVATE snodec::net-in-stream-tls
                                  snodec::net-in6-stream-tls)
    add_test(
        NAME CodexGenerateTlsCertificates
        COMMAND
            ${CMAKE_COMMAND}
            -DOPENSSL_EXECUTABLE=${AISUITE_CODEX_OPENSSL_EXECUTABLE}
            -DOUTPUT_DIRECTORY=${codex_tls_directory} -P
            ${CMAKE_CURRENT_SOURCE_DIR}/GenerateTlsCertificates.cmake
    )
    set_tests_properties(
        CodexGenerateTlsCertificates
        PROPERTIES FIXTURES_SETUP codex_tls LABELS "codex;fixture;tls"
                   TIMEOUT 10
    )
endif()

set(codex_stream_socket "${CMAKE_CURRENT_BINARY_DIR}/codex-transport.sock")
find_program(AISUITE_CODEX_CODEX_EXECUTABLE codex)
foreach(transport IN ITEMS unix ipv4 ipv6)
    string(REPLACE "-" "_" transport_id "${transport}")
    add_test(
        NAME CodexStream_${transport_id}
        COMMAND CodexStreamTransportAcceptanceTest ${transport}
                ${codex_stream_socket}
    )
    set_tests_properties(
        CodexStream_${transport_id}
        PROPERTIES LABELS "codex;transport;jsonl;${transport}"
                   SKIP_RETURN_CODE 77 TIMEOUT 10
    )
    if(AISUITE_CODEX_CODEX_EXECUTABLE)
        add_test(
            NAME CodexRealAppServer_Stream_${transport_id}
            COMMAND CodexStreamTransportAcceptanceTest ${transport}
                    "${CMAKE_CURRENT_BINARY_DIR}/codex-real-${transport}.sock"
                    --real-app-server ${AISUITE_CODEX_CODEX_EXECUTABLE}
        )
        set_tests_properties(
            CodexRealAppServer_Stream_${transport_id}
            PROPERTIES LABELS
                       "codex;real-app-server;end-to-end;transport;jsonl;${transport}"
                       SKIP_RETURN_CODE 77 TIMEOUT 30
        )
    endif()
endforeach()

if(TEST CodexGenerateTlsCertificates)
    foreach(transport IN ITEMS tls-ipv4 tls-ipv6)
        string(REPLACE "-" "_" transport_id "${transport}")
        add_test(
            NAME CodexStream_${transport_id}
            COMMAND CodexStreamTransportAcceptanceTest ${transport}
                    ${codex_stream_socket} ${codex_tls_certificate}
                    ${codex_tls_key}
        )
        set_tests_properties(
            CodexStream_${transport_id}
            PROPERTIES FIXTURES_REQUIRED codex_tls
                       LABELS "codex;transport;jsonl;tls;${transport}"
                       SKIP_RETURN_CODE 77 TIMEOUT 10
        )
        if(AISUITE_CODEX_CODEX_EXECUTABLE)
            add_test(
                NAME CodexRealAppServer_Stream_${transport_id}
                COMMAND CodexStreamTransportAcceptanceTest ${transport}
                        "${CMAKE_CURRENT_BINARY_DIR}/codex-real-${transport}.sock"
                        ${codex_tls_certificate} ${codex_tls_key}
                        --real-app-server ${AISUITE_CODEX_CODEX_EXECUTABLE}
            )
            set_tests_properties(
                CodexRealAppServer_Stream_${transport_id}
                PROPERTIES FIXTURES_REQUIRED codex_tls
                           LABELS
                           "codex;real-app-server;end-to-end;transport;jsonl;tls;${transport}"
                           SKIP_RETURN_CODE 77 TIMEOUT 30
            )
        endif()
    endforeach()
endif()

if(AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET
   AND TARGET snodec::http-server-express-legacy-un
   AND TARGET snodec::http-server-express-legacy-in
   AND TARGET snodec::http-server-express-legacy-in6
   AND TARGET snodec::websocket-server
   AND TARGET snodec::http-client
   AND TARGET snodec::websocket-client
)
    add_executable(CodexWebSocketProviderAcceptanceTest
                   WebSocketProviderAcceptanceTest.cpp)
    target_compile_features(CodexWebSocketProviderAcceptanceTest
                            PRIVATE cxx_std_20)
    target_include_directories(CodexWebSocketProviderAcceptanceTest
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(
        CodexWebSocketProviderAcceptanceTest
        PRIVATE AISuite::OpenAICodex snodec::core
                snodec::http-server-express-legacy-un
                snodec::http-server-express-legacy-in
                snodec::http-server-express-legacy-in6
                snodec::websocket-server snodec::http-client
                snodec::websocket-client
    )
    set(codex_provider_socket
        "${CMAKE_CURRENT_BINARY_DIR}/codex-provider.sock")
    foreach(transport IN ITEMS unix ipv4 ipv6)
        add_test(
            NAME CodexProvider_websocket_${transport}
            COMMAND CodexWebSocketProviderAcceptanceTest ${transport}
                    ${codex_provider_socket}
        )
        set_tests_properties(
            CodexProvider_websocket_${transport}
            PROPERTIES LABELS
                       "codex;provider;transport;websocket;${transport}"
                       SKIP_RETURN_CODE 77 TIMEOUT 10
        )
    endforeach()

endif()

if(AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET
   AND TARGET snodec::http-server-express-legacy-in
   AND TARGET snodec::http-server-express-legacy-in6
   AND TARGET snodec::websocket-server
   AND TARGET snodec::http-client
   AND TARGET snodec::websocket-client
)
    add_executable(
        CodexWebSocketTransportAcceptanceTest
        WebSocketTransportAcceptanceTest.cpp
        ${PROJECT_SOURCE_DIR}/src/apps/codex-bridge/WebSocketApplication.cpp
    )
    add_executable(
        CodexWebUiPathTest
        WebUiPathTest.cpp
        ${PROJECT_SOURCE_DIR}/src/apps/codex-bridge/WebSocketApplication.cpp
    )
    target_compile_features(CodexWebSocketTransportAcceptanceTest
                            PRIVATE cxx_std_20)
    target_compile_features(CodexWebUiPathTest PRIVATE cxx_std_20)
    target_include_directories(
        CodexWebSocketTransportAcceptanceTest
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src
    )
    target_include_directories(
        CodexWebUiPathTest
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src
    )
    target_link_libraries(
        CodexWebSocketTransportAcceptanceTest
        PRIVATE AISuite::OpenAICodex snodec::core
                snodec::http-server-express-legacy-in
                snodec::http-server-express-legacy-in6
                snodec::websocket-server snodec::http-client
                snodec::websocket-client
    )
    target_link_libraries(
        CodexWebUiPathTest
        PRIVATE AISuite::OpenAICodex snodec::core
                snodec::http-server-express-legacy-in
                snodec::http-server-express-legacy-in6
                snodec::websocket-server snodec::http-client
                snodec::websocket-client
    )
    add_test(NAME CodexWebUiPathTest COMMAND CodexWebUiPathTest)
    set_tests_properties(CodexWebUiPathTest
                         PROPERTIES LABELS "codex;webui;focused" TIMEOUT 5)

    foreach(transport IN ITEMS websocket-ipv4 websocket-ipv6)
        string(REPLACE "-" "_" transport_id "${transport}")
        add_test(
            NAME CodexWebSocket_${transport_id}
            COMMAND CodexWebSocketTransportAcceptanceTest ${transport}
        )
        set_tests_properties(
            CodexWebSocket_${transport_id}
            PROPERTIES LABELS "codex;transport;websocket;${transport}"
                       SKIP_RETURN_CODE 77 TIMEOUT 10
        )
        if(AISUITE_CODEX_CODEX_EXECUTABLE)
            add_test(
                NAME CodexRealAppServer_WebSocket_${transport_id}
                COMMAND CodexWebSocketTransportAcceptanceTest ${transport}
                        --real-app-server ${AISUITE_CODEX_CODEX_EXECUTABLE}
            )
            set_tests_properties(
                CodexRealAppServer_WebSocket_${transport_id}
                PROPERTIES LABELS
                           "codex;real-app-server;end-to-end;transport;websocket;${transport}"
                           SKIP_RETURN_CODE 77 TIMEOUT 30
            )
        endif()
    endforeach()

    if(TEST CodexGenerateTlsCertificates
       AND TARGET snodec::http-server-express-tls-in
       AND TARGET snodec::http-server-express-tls-in6
    )
        target_compile_definitions(
            CodexWebSocketTransportAcceptanceTest
            PRIVATE AISUITE_CODEX_TEST_WEBSOCKET_TLS=1
        )
        target_link_libraries(
            CodexWebSocketTransportAcceptanceTest
            PRIVATE snodec::http-server-express-tls-in
                    snodec::http-server-express-tls-in6
                    snodec::net-in-stream-tls snodec::net-in6-stream-tls
        )
        foreach(transport IN ITEMS wss-ipv4 wss-ipv6)
            string(REPLACE "-" "_" transport_id "${transport}")
            add_test(
                NAME CodexWebSocket_${transport_id}
                COMMAND CodexWebSocketTransportAcceptanceTest ${transport}
                        ${codex_tls_certificate} ${codex_tls_key}
            )
            set_tests_properties(
                CodexWebSocket_${transport_id}
                PROPERTIES FIXTURES_REQUIRED codex_tls
                           LABELS "codex;transport;websocket;wss;tls;${transport}"
                           SKIP_RETURN_CODE 77 TIMEOUT 10
            )
            if(AISUITE_CODEX_CODEX_EXECUTABLE)
                add_test(
                    NAME CodexRealAppServer_WebSocket_${transport_id}
                    COMMAND CodexWebSocketTransportAcceptanceTest ${transport}
                            ${codex_tls_certificate} ${codex_tls_key}
                            --real-app-server ${AISUITE_CODEX_CODEX_EXECUTABLE}
                )
                set_tests_properties(
                    CodexRealAppServer_WebSocket_${transport_id}
                    PROPERTIES FIXTURES_REQUIRED codex_tls
                               LABELS
                               "codex;real-app-server;end-to-end;transport;websocket;wss;tls;${transport}"
                               SKIP_RETURN_CODE 77 TIMEOUT 30
                )
            endif()
        endforeach()
    endif()
endif()
