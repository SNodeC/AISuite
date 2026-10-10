file(MAKE_DIRECTORY "${TEST_DIRECTORY}/private" "${TEST_DIRECTORY}/public")
execute_process(
    COMMAND chmod 700 "${TEST_DIRECTORY}/private"
    RESULT_VARIABLE permission_result
)
if(NOT permission_result EQUAL 0)
    message(FATAL_ERROR "Cannot prepare private runtime directory")
endif()
execute_process(
    COMMAND chmod 755 "${TEST_DIRECTORY}/public"
    RESULT_VARIABLE permission_result
)
if(NOT permission_result EQUAL 0)
    message(FATAL_ERROR "Cannot prepare public runtime directory")
endif()
execute_process(
    COMMAND id -u
    OUTPUT_VARIABLE uid
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE uid_result
)
if(NOT uid_result EQUAL 0)
    message(FATAL_ERROR "Cannot determine effective user ID")
endif()
set(fallback "/tmp/codex-bridge-${uid}/codex-bridge.sock")

function(check_path environment expected)
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env "${environment}"
            "XDG_CONFIG_HOME=${TEST_DIRECTORY}/config"
            "XDG_STATE_HOME=${TEST_DIRECTORY}/state" "${CLIENT}" --config-file
            /dev/null --log-file /dev/null --command-line=complete ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
        TIMEOUT 5
    )
    # Configuration inspection intentionally exits before starting transports.
    if(NOT result EQUAL 2)
        message(
            FATAL_ERROR
                "Configuration inspection failed: ${result}\n${output}\n${error}"
        )
    endif()
    string(FIND "${output}${error}" "remote --sun-path ${expected} local"
                position
    )
    if(position EQUAL -1)
        message(
            FATAL_ERROR
                "Expected Unix frontend path ${expected}\n${output}\n${error}"
        )
    endif()
endfunction()

check_path(
    "XDG_RUNTIME_DIR=${TEST_DIRECTORY}/private"
    "${TEST_DIRECTORY}/private/codex-bridge.sock"
)
check_path("--unset=XDG_RUNTIME_DIR" "${fallback}")
check_path("XDG_RUNTIME_DIR=relative-runtime" "${fallback}")
check_path("XDG_RUNTIME_DIR=${TEST_DIRECTORY}/missing" "${fallback}")
check_path("XDG_RUNTIME_DIR=${TEST_DIRECTORY}/public" "${fallback}")
check_path(
    "XDG_RUNTIME_DIR=${TEST_DIRECTORY}/private"
    "${TEST_DIRECTORY}/explicit.sock" codex-bridge-client-unix remote
    --sun-path "${TEST_DIRECTORY}/explicit.sock"
)
