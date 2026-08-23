add_executable(Codex2JsonLineFramerTest JsonLineFramerTest.cpp)
add_executable(Codex2FrontendSdkTest FrontendSdkTest.cpp)
add_executable(Codex2BridgeRoutingTest BridgeRoutingTest.cpp)
add_executable(Codex2FakeStdioAppServer FakeStdioAppServer.cpp)
add_executable(Codex2StdioProviderAcceptanceTest
               StdioProviderAcceptanceTest.cpp)

foreach(target IN ITEMS Codex2JsonLineFramerTest Codex2FrontendSdkTest
                        Codex2BridgeRoutingTest)
    target_compile_features(${target} PRIVATE cxx_std_20)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${target} PRIVATE AISuite::OpenAICodex2)
endforeach()

target_compile_features(Codex2FakeStdioAppServer PRIVATE cxx_std_20)
target_link_libraries(Codex2FakeStdioAppServer PRIVATE nlohmann_json::nlohmann_json)
target_compile_features(Codex2StdioProviderAcceptanceTest PRIVATE cxx_std_20)
target_include_directories(Codex2StdioProviderAcceptanceTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(Codex2StdioProviderAcceptanceTest
                      PRIVATE AISuite::OpenAICodex2 snodec::core)

add_test(NAME Codex2JsonLineFramerTest COMMAND Codex2JsonLineFramerTest)
add_test(NAME Codex2FrontendSdkTest COMMAND Codex2FrontendSdkTest)
add_test(NAME Codex2BridgeRoutingTest COMMAND Codex2BridgeRoutingTest)
add_test(
    NAME Codex2Provider_stdio
    COMMAND Codex2StdioProviderAcceptanceTest
            $<TARGET_FILE:Codex2FakeStdioAppServer>
)

set_tests_properties(
    Codex2JsonLineFramerTest Codex2FrontendSdkTest Codex2BridgeRoutingTest
    PROPERTIES LABELS "codex2;focused" TIMEOUT 5
)
set_tests_properties(
    Codex2Provider_stdio
    PROPERTIES LABELS "codex2;provider;transport;stdio;jsonl" TIMEOUT 10
)

add_executable(Codex2StreamTransportAcceptanceTest
               StreamTransportAcceptanceTest.cpp)
target_compile_features(Codex2StreamTransportAcceptanceTest PRIVATE cxx_std_20)
target_include_directories(Codex2StreamTransportAcceptanceTest
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(
    Codex2StreamTransportAcceptanceTest
    PRIVATE AISuite::OpenAICodex2 snodec::core snodec::net-un-stream-legacy
            snodec::net-in-stream-legacy snodec::net-in6-stream-legacy
)

set(codex2_tls_directory "${CMAKE_CURRENT_BINARY_DIR}/tls")
set(codex2_tls_certificate "${codex2_tls_directory}/loopback-cert.pem")
set(codex2_tls_key "${codex2_tls_directory}/loopback-key.pem")
find_program(AISUITE_CODEX2_OPENSSL_EXECUTABLE openssl)
if(AISUITE_ENABLE_CODEX_FRONTEND_TLS
   AND AISUITE_CODEX2_OPENSSL_EXECUTABLE
   AND TARGET snodec::net-in-stream-tls
   AND TARGET snodec::net-in6-stream-tls
)
    target_compile_definitions(Codex2StreamTransportAcceptanceTest
                               PRIVATE AISUITE_CODEX2_TEST_TLS=1)
    target_link_libraries(Codex2StreamTransportAcceptanceTest
                          PRIVATE snodec::net-in-stream-tls
                                  snodec::net-in6-stream-tls)
    add_test(
        NAME Codex2GenerateTlsCertificates
        COMMAND
            ${CMAKE_COMMAND}
            -DOPENSSL_EXECUTABLE=${AISUITE_CODEX2_OPENSSL_EXECUTABLE}
            -DOUTPUT_DIRECTORY=${codex2_tls_directory} -P
            ${CMAKE_CURRENT_SOURCE_DIR}/GenerateTlsCertificates.cmake
    )
    set_tests_properties(
        Codex2GenerateTlsCertificates
        PROPERTIES FIXTURES_SETUP codex2_tls LABELS "codex2;fixture;tls"
                   TIMEOUT 10
    )
endif()

set(codex2_stream_socket "${CMAKE_CURRENT_BINARY_DIR}/codex2-transport.sock")
find_program(AISUITE_CODEX2_CODEX_EXECUTABLE codex)
foreach(transport IN ITEMS unix ipv4 ipv6)
    string(REPLACE "-" "_" transport_id "${transport}")
    add_test(
        NAME Codex2Stream_${transport_id}
        COMMAND Codex2StreamTransportAcceptanceTest ${transport}
                ${codex2_stream_socket}
    )
    set_tests_properties(
        Codex2Stream_${transport_id}
        PROPERTIES LABELS "codex2;transport;jsonl;${transport}"
                   SKIP_RETURN_CODE 77 TIMEOUT 10
    )
    if(AISUITE_CODEX2_CODEX_EXECUTABLE)
        add_test(
            NAME Codex2RealAppServer_Stream_${transport_id}
            COMMAND Codex2StreamTransportAcceptanceTest ${transport}
                    "${CMAKE_CURRENT_BINARY_DIR}/codex2-real-${transport}.sock"
                    --real-app-server ${AISUITE_CODEX2_CODEX_EXECUTABLE}
        )
        set_tests_properties(
            Codex2RealAppServer_Stream_${transport_id}
            PROPERTIES LABELS
                       "codex2;real-app-server;end-to-end;transport;jsonl;${transport}"
                       SKIP_RETURN_CODE 77 TIMEOUT 30
        )
    endif()
endforeach()

if(TEST Codex2GenerateTlsCertificates)
    foreach(transport IN ITEMS tls-ipv4 tls-ipv6)
        string(REPLACE "-" "_" transport_id "${transport}")
        add_test(
            NAME Codex2Stream_${transport_id}
            COMMAND Codex2StreamTransportAcceptanceTest ${transport}
                    ${codex2_stream_socket} ${codex2_tls_certificate}
                    ${codex2_tls_key}
        )
        set_tests_properties(
            Codex2Stream_${transport_id}
            PROPERTIES FIXTURES_REQUIRED codex2_tls
                       LABELS "codex2;transport;jsonl;tls;${transport}"
                       SKIP_RETURN_CODE 77 TIMEOUT 10
        )
        if(AISUITE_CODEX2_CODEX_EXECUTABLE)
            add_test(
                NAME Codex2RealAppServer_Stream_${transport_id}
                COMMAND Codex2StreamTransportAcceptanceTest ${transport}
                        "${CMAKE_CURRENT_BINARY_DIR}/codex2-real-${transport}.sock"
                        ${codex2_tls_certificate} ${codex2_tls_key}
                        --real-app-server ${AISUITE_CODEX2_CODEX_EXECUTABLE}
            )
            set_tests_properties(
                Codex2RealAppServer_Stream_${transport_id}
                PROPERTIES FIXTURES_REQUIRED codex2_tls
                           LABELS
                           "codex2;real-app-server;end-to-end;transport;jsonl;tls;${transport}"
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
    add_executable(Codex2WebSocketProviderAcceptanceTest
                   WebSocketProviderAcceptanceTest.cpp)
    target_compile_features(Codex2WebSocketProviderAcceptanceTest
                            PRIVATE cxx_std_20)
    target_include_directories(Codex2WebSocketProviderAcceptanceTest
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(
        Codex2WebSocketProviderAcceptanceTest
        PRIVATE AISuite::OpenAICodex2 snodec::core
                snodec::http-server-express-legacy-un
                snodec::http-server-express-legacy-in
                snodec::http-server-express-legacy-in6
                snodec::websocket-server snodec::http-client
                snodec::websocket-client
    )
    set(codex2_provider_socket
        "${CMAKE_CURRENT_BINARY_DIR}/codex2-provider.sock")
    foreach(transport IN ITEMS unix ipv4 ipv6)
        add_test(
            NAME Codex2Provider_websocket_${transport}
            COMMAND Codex2WebSocketProviderAcceptanceTest ${transport}
                    ${codex2_provider_socket}
        )
        set_tests_properties(
            Codex2Provider_websocket_${transport}
            PROPERTIES LABELS
                       "codex2;provider;transport;websocket;${transport}"
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
        Codex2WebSocketTransportAcceptanceTest
        WebSocketTransportAcceptanceTest.cpp
        ${PROJECT_SOURCE_DIR}/src/apps/codex-bridge/WebSocketApplication.cpp
    )
    target_compile_features(Codex2WebSocketTransportAcceptanceTest
                            PRIVATE cxx_std_20)
    target_include_directories(
        Codex2WebSocketTransportAcceptanceTest
        PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/src
    )
    target_link_libraries(
        Codex2WebSocketTransportAcceptanceTest
        PRIVATE AISuite::OpenAICodex2 snodec::core
                snodec::http-server-express-legacy-in
                snodec::http-server-express-legacy-in6
                snodec::websocket-server snodec::http-client
                snodec::websocket-client
    )

    foreach(transport IN ITEMS websocket-ipv4 websocket-ipv6)
        string(REPLACE "-" "_" transport_id "${transport}")
        add_test(
            NAME Codex2WebSocket_${transport_id}
            COMMAND Codex2WebSocketTransportAcceptanceTest ${transport}
        )
        set_tests_properties(
            Codex2WebSocket_${transport_id}
            PROPERTIES LABELS "codex2;transport;websocket;${transport}"
                       SKIP_RETURN_CODE 77 TIMEOUT 10
        )
        if(AISUITE_CODEX2_CODEX_EXECUTABLE)
            add_test(
                NAME Codex2RealAppServer_WebSocket_${transport_id}
                COMMAND Codex2WebSocketTransportAcceptanceTest ${transport}
                        --real-app-server ${AISUITE_CODEX2_CODEX_EXECUTABLE}
            )
            set_tests_properties(
                Codex2RealAppServer_WebSocket_${transport_id}
                PROPERTIES LABELS
                           "codex2;real-app-server;end-to-end;transport;websocket;${transport}"
                           SKIP_RETURN_CODE 77 TIMEOUT 30
            )
        endif()
    endforeach()

    if(TEST Codex2GenerateTlsCertificates
       AND TARGET snodec::http-server-express-tls-in
       AND TARGET snodec::http-server-express-tls-in6
    )
        target_compile_definitions(
            Codex2WebSocketTransportAcceptanceTest
            PRIVATE AISUITE_CODEX2_TEST_WEBSOCKET_TLS=1
        )
        target_link_libraries(
            Codex2WebSocketTransportAcceptanceTest
            PRIVATE snodec::http-server-express-tls-in
                    snodec::http-server-express-tls-in6
                    snodec::net-in-stream-tls snodec::net-in6-stream-tls
        )
        foreach(transport IN ITEMS wss-ipv4 wss-ipv6)
            string(REPLACE "-" "_" transport_id "${transport}")
            add_test(
                NAME Codex2WebSocket_${transport_id}
                COMMAND Codex2WebSocketTransportAcceptanceTest ${transport}
                        ${codex2_tls_certificate} ${codex2_tls_key}
            )
            set_tests_properties(
                Codex2WebSocket_${transport_id}
                PROPERTIES FIXTURES_REQUIRED codex2_tls
                           LABELS "codex2;transport;websocket;wss;tls;${transport}"
                           SKIP_RETURN_CODE 77 TIMEOUT 10
            )
            if(AISUITE_CODEX2_CODEX_EXECUTABLE)
                add_test(
                    NAME Codex2RealAppServer_WebSocket_${transport_id}
                    COMMAND Codex2WebSocketTransportAcceptanceTest ${transport}
                            ${codex2_tls_certificate} ${codex2_tls_key}
                            --real-app-server ${AISUITE_CODEX2_CODEX_EXECUTABLE}
                )
                set_tests_properties(
                    Codex2RealAppServer_WebSocket_${transport_id}
                    PROPERTIES FIXTURES_REQUIRED codex2_tls
                               LABELS
                               "codex2;real-app-server;end-to-end;transport;websocket;wss;tls;${transport}"
                               SKIP_RETURN_CODE 77 TIMEOUT 30
                )
            endif()
        endforeach()
    endif()
endif()
