if(NOT DEFINED AISUITE_FRONTEND_CLIENT_LIBRARY
   OR NOT EXISTS "${AISUITE_FRONTEND_CLIENT_LIBRARY}"
)
    message(FATAL_ERROR "frontend-client shared library is unavailable")
endif()
if(NOT DEFINED AISUITE_NM OR NOT EXISTS "${AISUITE_NM}")
    message(FATAL_ERROR "CMake nm tool is unavailable")
endif()
execute_process(
    COMMAND "${AISUITE_NM}" --dynamic --defined-only --demangle
            "${AISUITE_FRONTEND_CLIENT_LIBRARY}"
    RESULT_VARIABLE nm_result
    OUTPUT_VARIABLE symbols
    ERROR_VARIABLE nm_error
)
if(NOT nm_result EQUAL 0)
    message(
        FATAL_ERROR "unable to inspect frontend-client exports: ${nm_error}"
    )
endif()

foreach(
    forbidden
    "ai::openai::codex::frontend::client::detail::"
    "ai::openai::codex::frontend::client::Client::Impl"
    "ai::openai::codex::frontend::client::Connection::Control"
    "ai::openai::codex::frontend::client::StateStorage"
    "ai::openai::codex::frontend::internal::"
    "ai::openai::codex::detail::"
)
    string(FIND "${symbols}" "${forbidden}" forbidden_index)
    if(NOT forbidden_index EQUAL -1)
        message(
            FATAL_ERROR
                "private frontend-client symbol is exported: ${forbidden}"
        )
    endif()
endforeach()

foreach(
    typed_compatibility_symbol
    "ai::openai::codex::typed::AuthMode::chatgpt()"
    "ai::openai::codex::typed::AutoCompactTokenLimitScope::total()"
    "ai::openai::codex::typed::InputModality::text()"
    "ai::openai::codex::typed::ReasoningEffort::high()"
    "ai::openai::codex::typed::TurnStatus::completed()"
)
    string(FIND "${symbols}" "${typed_compatibility_symbol}" typed_symbol_index)
    if(typed_symbol_index EQUAL -1)
        message(
            FATAL_ERROR
                "frontend-client is missing required typed source-compatibility symbol: ${typed_compatibility_symbol}"
        )
    endif()
endforeach()

if(DEFINED AISUITE_READELF AND EXISTS "${AISUITE_READELF}")
    execute_process(
        COMMAND "${AISUITE_READELF}" -d "${AISUITE_FRONTEND_CLIENT_LIBRARY}"
        RESULT_VARIABLE readelf_result
        OUTPUT_VARIABLE dynamic_section
        ERROR_VARIABLE readelf_error
    )
    if(NOT readelf_result EQUAL 0)
        message(FATAL_ERROR "unable to inspect frontend-client dependencies: ${readelf_error}")
    endif()
    foreach(
        forbidden_dependency
        "libaisuite-openai-codex.so"
        "libaisuite-openai-codex-backend.so"
        "libaisuite-openai-codex-frontend.so"
        "libsnodec-"
    )
        string(FIND "${dynamic_section}" "${forbidden_dependency}" forbidden_dependency_index)
        if(NOT forbidden_dependency_index EQUAL -1)
            message(FATAL_ERROR "frontend-client has forbidden runtime dependency: ${forbidden_dependency}")
        endif()
    endforeach()
endif()

set(public_classes
    Accounts
    Apps
    Client
    Commands
    Configuration
    Connection
    Controller
    ExternalAgents
    Feedback
    Filesystem
    Hooks
    Marketplace
    Mcp
    Models
    PermissionProfiles
    Plugins
    Provider
    Requests
    Reviews
    Skills
    State
    Synchronization
    Threads
    Turns
    WindowsSandbox
)
foreach(public_class IN LISTS public_classes)
    set(qualified "ai::openai::codex::frontend::client::${public_class}::")
    string(FIND "${symbols}" "${qualified}" public_index)
    if(public_index EQUAL -1)
        message(
            FATAL_ERROR
                "public frontend-client class has no exported symbol: ${public_class}"
        )
    endif()
endforeach()

string(FIND "${symbols}"
            "ai::openai::codex::frontend::client::projectionFingerprint"
            fingerprint_index
)
if(fingerprint_index EQUAL -1)
    message(FATAL_ERROR "projectionFingerprint is not exported")
endif()

string(FIND "${symbols}"
            "ai::openai::codex::frontend::client::userMessageSemanticView"
            user_message_view_index
)
if(user_message_view_index EQUAL -1)
    message(FATAL_ERROR "userMessageSemanticView is not exported")
endif()

string(FIND "${symbols}"
            "ai::openai::codex::frontend::client::threadIsIdle"
            thread_is_idle_index
)
if(thread_is_idle_index EQUAL -1)
    message(FATAL_ERROR "threadIsIdle is not exported")
endif()

string(REGEX MATCHALL "[^\n]*ai::openai::codex::frontend::client::[^\n]*"
             client_exports "${symbols}"
)
list(LENGTH client_exports client_export_count)
message(
    STATUS
        "frontend-client visibility verified: ${client_export_count} SDK export lines, 25 public classes, no private detail symbols"
)
