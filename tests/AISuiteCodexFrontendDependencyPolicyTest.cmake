if(NOT DEFINED AISUITE_SOURCE_DIR)
    message(FATAL_ERROR "AISUITE_SOURCE_DIR is required")
endif()

include("${AISUITE_SOURCE_DIR}/cmake/AISuiteCodexFrontendDependencies.cmake")

function(expect_websocket_client apps tests sdk expected)
    aisuite_codex_frontend_websocket_client_required(
        actual "${apps}" "${tests}" "${sdk}"
    )
    if(NOT actual STREQUAL expected)
        message(
            FATAL_ERROR
                "WebSocket client dependency policy for apps=${apps}, tests=${tests}, sdk=${sdk} is ${actual}; expected ${expected}"
        )
    endif()
endfunction()

expect_websocket_client(ON OFF ON ON)
expect_websocket_client(ON OFF OFF OFF)
expect_websocket_client(OFF OFF ON OFF)
expect_websocket_client(OFF OFF OFF OFF)
expect_websocket_client(ON ON ON ON)
expect_websocket_client(ON ON OFF ON)
expect_websocket_client(OFF ON ON ON)
expect_websocket_client(OFF ON OFF ON)

aisuite_codex_frontend_websocket_client_required(
    expected_current "${AISUITE_BUILD_APPS}" "${AISUITE_BUILD_TESTS}"
    "${AISUITE_BUILD_CODEX_FRONTEND_CLIENT}"
)
if(NOT expected_current STREQUAL AISUITE_ACTUAL_WEBSOCKET_CLIENT_REQUIRED)
    message(
        FATAL_ERROR
            "configured WebSocket client dependency truth is ${AISUITE_ACTUAL_WEBSOCKET_CLIENT_REQUIRED}; expected ${expected_current}"
    )
endif()
