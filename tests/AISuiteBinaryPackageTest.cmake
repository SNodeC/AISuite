if(NOT DEFINED AISUITE_BUILD_DIR)
    message(FATAL_ERROR "AISUITE_BUILD_DIR is required")
endif()
if(NOT DEFINED CMAKE_CPACK_COMMAND)
    set(CMAKE_CPACK_COMMAND cpack)
endif()

file(GLOB old_archives "${AISUITE_BUILD_DIR}/AISuite-*.tar.gz")
if(old_archives)
    file(REMOVE ${old_archives})
endif()
execute_process(
    COMMAND "${CMAKE_CPACK_COMMAND}" --config "${AISUITE_BUILD_DIR}/CPackConfig.cmake" -G TGZ
    WORKING_DIRECTORY "${AISUITE_BUILD_DIR}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "AISuite binary package failed: ${result}")
endif()
file(GLOB archives "${AISUITE_BUILD_DIR}/AISuite-*.tar.gz")
list(LENGTH archives archive_count)
if(NOT archive_count EQUAL 1)
    message(FATAL_ERROR "expected one binary archive, found ${archive_count}")
endif()
list(GET archives 0 archive)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar tf "${archive}"
    OUTPUT_VARIABLE listing
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "unable to list binary package")
endif()

# Enumerate the complete installed Codex header surface from the archive
# itself. This is deliberately independent of the required-header spot checks
# below, so a duplicate, private header, or component-count drift cannot hide
# behind one known public path.
string(REPLACE "\r\n" "\n" listing "${listing}")
string(REPLACE "\n" ";" archive_entries "${listing}")
set(codex_public_headers)
set(codex_main_header_count 0)
set(codex_backend_header_count 0)
set(codex_frontend_header_count 0)
foreach(archive_entry IN LISTS archive_entries)
    string(STRIP "${archive_entry}" archive_entry)
    if(archive_entry MATCHES
       "(^|/)include/aisuite/ai/openai/codex/(.+/)?(detail|private)(/|$)")
        message(
            FATAL_ERROR
                "CodexPolicyBinaryPackageLeak: binary package contains private Codex include path ${archive_entry}"
        )
    endif()
    if(NOT archive_entry MATCHES
       "(^|/)include/aisuite/ai/openai/codex/(.+[.](h|hh|hpp|ipp))$")
        continue()
    endif()

    set(codex_header "${CMAKE_MATCH_2}")
    if(codex_header MATCHES "(^|/)(detail|private)(/|$)")
        message(
            FATAL_ERROR
                "CodexPolicyBinaryPackageLeak: binary package contains private Codex header ${codex_header}"
        )
    endif()
    list(FIND codex_public_headers "${codex_header}" existing_header_index)
    if(NOT existing_header_index EQUAL -1)
        message(
            FATAL_ERROR
                "CodexPolicyPublicHeaderInventoryMismatch: duplicate binary-package Codex header ${codex_header}"
        )
    endif()
    list(APPEND codex_public_headers "${codex_header}")

    if(codex_header MATCHES "^backend/")
        math(EXPR codex_backend_header_count
             "${codex_backend_header_count} + 1"
        )
    elseif(codex_header MATCHES "^frontend/")
        math(EXPR codex_frontend_header_count
             "${codex_frontend_header_count} + 1"
        )
    else()
        math(EXPR codex_main_header_count "${codex_main_header_count} + 1")
    endif()
endforeach()
list(LENGTH codex_public_headers codex_public_header_count)
if(NOT codex_main_header_count EQUAL 29 OR
   NOT codex_backend_header_count EQUAL 7 OR
   NOT codex_frontend_header_count EQUAL 7 OR
   NOT codex_public_header_count EQUAL 43)
    message(
        FATAL_ERROR
            "CodexPolicyPublicHeaderInventoryMismatch: binary-package Codex header inventory is main=${codex_main_header_count}, backend=${codex_backend_header_count}, frontend=${codex_frontend_header_count}, total=${codex_public_header_count}; expected 29/7/7/43"
    )
endif()

foreach(required
    "include/aisuite/ai/openai/codex/Api.h"
    "include/aisuite/ai/openai/codex/AppServerClient.h"
    "include/aisuite/ai/openai/codex/typed/Apps.h"
    "include/aisuite/ai/openai/codex/typed/ExternalAgents.h"
    "include/aisuite/ai/openai/codex/typed/Feedback.h"
    "include/aisuite/ai/openai/codex/typed/Hooks.h"
    "include/aisuite/ai/openai/codex/typed/Marketplace.h"
    "include/aisuite/ai/openai/codex/typed/Mcp.h"
    "include/aisuite/ai/openai/codex/typed/WindowsSandbox.h"
    "include/aisuite/ai/openai/codex/typed/Plugins.h"
    "include/aisuite/ai/openai/codex/typed/Skills.h"
    "lib/libaisuite-openai-codex.so.2"
    "lib/libaisuite-openai-codex-backend.so.2"
    "lib/libaisuite-openai-codex-frontend.so.2"
    "bin/codex-backend"
    "bin/codex-backend-client"
    "lib/cmake/AISuite/AISuiteConfig.cmake"
)
    string(FIND "${listing}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "binary package missing ${required}")
    endif()
endforeach()
foreach(forbidden
    "include/aisuite/ai/openai/codex/typed/Client.h"
    "lib/libaisuite-openai-codex.so.1"
    "lib/libaisuite-openai-codex-backend.so.1"
    "lib/libaisuite-openai-codex-frontend.so.1"
    "tools/codex/"
    "tools/extraction/"
    "tests/component/codex/"
    "tests/policy/"
    "tests/AISuiteSourcePackageTest.cmake"
    "tests/AISuiteBinaryPackageTest.cmake"
    "AISuiteSourcePolicyTestRoot.h"
    "CxxSourceScanner.h"
    "CodexSemanticLoggerAuthority.tsv"
    "CodexSemanticLoggerClassifications.tsv"
    "CodexPolicyMutationTest.py"
    "CodexPublicHeaderPolicyTest"
    "CodexPublicHeaderSelfContainmentTest"
    "CodexLoggingApiSurfacePolicyTest"
    "CodexSemanticLoggerPolicyTest"
    "CodexPolicyMutationTest"
    "src/ai/openai/codex/detail/"
    "ProtocolSurfaceRegistryData.inc"
)
    string(FIND "${listing}" "${forbidden}" found)
    if(NOT found EQUAL -1)
        message(
            FATAL_ERROR
                "CodexPolicyBinaryPackageLeak: binary package contains private path ${forbidden}"
        )
    endif()
endforeach()
