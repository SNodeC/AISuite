add_executable(AnthropicProtocolTest AnthropicProtocolTest.cpp)
target_include_directories(AnthropicProtocolTest PRIVATE ../codex)
target_link_libraries(AnthropicProtocolTest PRIVATE AISuite::Anthropic)
add_test(NAME AnthropicProtocolTest COMMAND AnthropicProtocolTest)
set_tests_properties(
    AnthropicProtocolTest PROPERTIES LABELS "model;anthropic;focused" TIMEOUT
                                     10
)
add_executable(ResponsesAdapterTest ResponsesAdapterTest.cpp)
target_include_directories(ResponsesAdapterTest PRIVATE ../codex)
target_link_libraries(
    ResponsesAdapterTest PRIVATE AISuite::OpenAICodex AISuite::Anthropic
)
add_test(NAME ResponsesAdapterTest COMMAND ResponsesAdapterTest)
set_tests_properties(
    ResponsesAdapterTest PROPERTIES LABELS "model;anthropic;codex;focused"
                                    TIMEOUT 10
)
if(AISUITE_CODEX_CODEX_EXECUTABLE)
    add_executable(CodexModelCatalogTest CodexModelCatalogTest.cpp)
    target_include_directories(CodexModelCatalogTest PRIVATE ../codex)
    target_link_libraries(
        CodexModelCatalogTest PRIVATE AISuite::OpenAICodex snodec::core
    )
    add_test(NAME CodexModelCatalogTest
             COMMAND CodexModelCatalogTest ${AISUITE_CODEX_CODEX_EXECUTABLE}
    )
    set_tests_properties(
        CodexModelCatalogTest PROPERTIES LABELS "model;codex;real-app-server"
                                         TIMEOUT 30
    )
endif()
add_executable(CodexAgentTest CodexAgentTest.cpp)
target_include_directories(CodexAgentTest PRIVATE ../codex)
target_link_libraries(CodexAgentTest PRIVATE AISuite::OpenAICodex)
add_test(NAME CodexAgentTest COMMAND CodexAgentTest)
set_tests_properties(
    CodexAgentTest PROPERTIES LABELS "agent;codex;focused" TIMEOUT 10
)
if(AISUITE_BUILD_APPS AND AISUITE_CODEX_CODEX_EXECUTABLE)
    find_package(
        Python3
        COMPONENTS Interpreter
        REQUIRED
    )
    add_test(
        NAME CodexAnthropicCodingTurnMock
        COMMAND
            ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/coding_turn.py
            --bridge $<TARGET_FILE:codex-bridge> --codex
            ${AISUITE_CODEX_CODEX_EXECUTABLE}
    )
    set_tests_properties(
        CodexAnthropicCodingTurnMock
        PROPERTIES LABELS "model;anthropic;codex;end-to-end" SKIP_RETURN_CODE
                   77 TIMEOUT 120
    )
    option(AISUITE_ANTHROPIC_LIVE_TEST
           "Opt in to billable Claude coding-turn integration test" OFF
    )
    if(AISUITE_ANTHROPIC_LIVE_TEST)
        add_test(
            NAME CodexAnthropicCodingTurnLive
            COMMAND
                ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/coding_turn.py --bridge
                $<TARGET_FILE:codex-bridge> --codex
                ${AISUITE_CODEX_CODEX_EXECUTABLE} --live
        )
        set_tests_properties(
            CodexAnthropicCodingTurnLive
            PROPERTIES LABELS "model;anthropic;live;end-to-end"
                       SKIP_RETURN_CODE 77 TIMEOUT 330
        )
    endif()
endif()
add_executable(HttpStreamingTest HttpStreamingTest.cpp)
target_include_directories(HttpStreamingTest PRIVATE ../codex)
target_link_libraries(HttpStreamingTest PRIVATE AISuite::HttpStreaming snodec::http-client snodec::net-in-stream-legacy)
add_test(NAME HttpStreamingTest COMMAND HttpStreamingTest)
set_tests_properties(HttpStreamingTest PROPERTIES LABELS "model;http;focused" TIMEOUT 10)
