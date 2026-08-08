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
)
    string(FIND "${symbols}" "${forbidden}" forbidden_index)
    if(NOT forbidden_index EQUAL -1)
        message(
            FATAL_ERROR
                "private frontend-client symbol is exported: ${forbidden}"
        )
    endif()
endforeach()

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

string(REGEX MATCHALL "[^\n]*ai::openai::codex::frontend::client::[^\n]*"
             client_exports "${symbols}"
)
list(LENGTH client_exports client_export_count)
message(
    STATUS
        "frontend-client visibility verified: ${client_export_count} SDK export lines, 25 public classes, no private detail symbols"
)
