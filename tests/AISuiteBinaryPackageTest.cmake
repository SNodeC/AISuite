if(NOT DEFINED AISUITE_BUILD_DIR)
    message(FATAL_ERROR "AISUITE_BUILD_DIR is required")
endif()
if(NOT DEFINED AISUITE_BUILD_APPS)
    set(AISUITE_BUILD_APPS ON)
endif()
if(NOT DEFINED AISUITE_BUILD_CODEX_FRONTEND_CLIENT)
    set(AISUITE_BUILD_CODEX_FRONTEND_CLIENT ON)
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
get_filename_component(archive_name "${archive}" NAME)
if(NOT archive_name MATCHES "^AISuite-0[.]2[.]0(-[^/]*)?[.]tar[.]gz$")
    message(
        FATAL_ERROR
            "unexpected AISuite 0.2.0 binary archive name ${archive_name}"
    )
endif()
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
set(codex_frontend_client_header_count 0)
foreach(archive_entry IN LISTS archive_entries)
    string(STRIP "${archive_entry}" archive_entry)
    if(archive_entry MATCHES
       "(^|/)include/aisuite/ai/openai/codex/(.+/)?(detail|internal|private)(/|$)")
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
    if(codex_header MATCHES "(^|/)(detail|internal|private)(/|$)")
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
    elseif(codex_header MATCHES "^frontend/client/")
        math(EXPR codex_frontend_client_header_count
             "${codex_frontend_client_header_count} + 1"
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
set(expected_frontend_client_header_count 0)
if(AISUITE_BUILD_CODEX_FRONTEND_CLIENT)
    set(expected_frontend_client_header_count 33)
endif()
math(EXPR expected_public_header_count
     "45 + ${expected_frontend_client_header_count}"
)
if(NOT codex_main_header_count EQUAL 29 OR
   NOT codex_backend_header_count EQUAL 7 OR
   NOT codex_frontend_header_count EQUAL 9 OR
   NOT codex_frontend_client_header_count EQUAL
       ${expected_frontend_client_header_count} OR
   NOT codex_public_header_count EQUAL ${expected_public_header_count})
    message(
        FATAL_ERROR
            "CodexPolicyPublicHeaderInventoryMismatch: binary-package Codex header inventory is main=${codex_main_header_count}, backend=${codex_backend_header_count}, frontend=${codex_frontend_header_count}, frontend-client=${codex_frontend_client_header_count}, total=${codex_public_header_count}; expected 29/7/9/${expected_frontend_client_header_count}/${expected_public_header_count}"
    )
endif()

set(required_package_entries
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
    "include/aisuite/ai/openai/codex/frontend/Codec.h"
    "include/aisuite/ai/openai/codex/frontend/GeneratedProtocol.h"
    "include/aisuite/ai/openai/codex/frontend/Messages.h"
    "include/aisuite/ai/openai/codex/frontend/Protocol.h"
    "include/aisuite/ai/openai/codex/frontend/Security.h"
    "include/aisuite/ai/openai/codex/frontend/FrontendService.h"
    "lib/libaisuite-openai-codex.so.3"
    "lib/libaisuite-openai-codex-backend.so.3"
    "lib/libaisuite-openai-codex-frontend-protocol.so.3"
    "lib/libaisuite-openai-codex-frontend.so.3"
    "lib/cmake/AISuite/AISuiteConfig.cmake"
    "lib/cmake/AISuite/AISuiteTargets.cmake"
    "lib/cmake/AISuite/AISuiteTargets-v3.cmake"
)
if(AISUITE_BUILD_CODEX_FRONTEND_CLIENT)
    list(
        APPEND required_package_entries
        "include/aisuite/ai/openai/codex/frontend/client/Client.h"
        "include/aisuite/ai/openai/codex/frontend/client/Controller.h"
        "include/aisuite/ai/openai/codex/frontend/client/Export.h"
        "include/aisuite/ai/openai/codex/frontend/client/GeneratedBindings.h"
        "include/aisuite/ai/openai/codex/frontend/client/Provider.h"
        "include/aisuite/ai/openai/codex/frontend/client/Requests.h"
        "include/aisuite/ai/openai/codex/frontend/client/State.h"
        "include/aisuite/ai/openai/codex/frontend/client/StateTypes.h"
        "include/aisuite/ai/openai/codex/frontend/client/Synchronization.h"
        "include/aisuite/ai/openai/codex/frontend/client/Threads.h"
        "include/aisuite/ai/openai/codex/frontend/client/Turns.h"
        "lib/libaisuite-openai-codex-frontend-client.so.3"
    )
endif()
if(AISUITE_BUILD_APPS)
    list(APPEND required_package_entries "bin/codex-backend")
    if(AISUITE_BUILD_CODEX_FRONTEND_CLIENT)
        list(APPEND required_package_entries "bin/codex-backend-client")
    endif()
endif()
foreach(required IN LISTS required_package_entries)
    string(FIND "${listing}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "binary package missing ${required}")
    endif()
endforeach()
set(forbidden_package_entries
    "include/aisuite/ai/openai/codex/typed/Client.h"
    "include/aisuite/ai/openai/codex/frontend/BackendAdapter.h"
    "include/aisuite/ai/openai/codex/frontend/FrontendClient.h"
    "lib/libaisuite-openai-codex.so.1"
    "lib/libaisuite-openai-codex-backend.so.1"
    "lib/libaisuite-openai-codex-frontend-protocol.so.1"
    "lib/libaisuite-openai-codex-frontend.so.1"
    "lib/libaisuite-openai-codex-frontend-client.so.1"
    "lib/libaisuite-openai-codex.so.2"
    "lib/libaisuite-openai-codex-backend.so.2"
    "lib/libaisuite-openai-codex-frontend-protocol.so.2"
    "lib/libaisuite-openai-codex-frontend.so.2"
    "lib/libaisuite-openai-codex-frontend-client.so.2"
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
    "src/ai/openai/codex/frontend/internal/"
    "ProtocolSurfaceRegistryData.inc"
)
if(NOT AISUITE_BUILD_CODEX_FRONTEND_CLIENT)
    list(
        APPEND forbidden_package_entries
        "include/aisuite/ai/openai/codex/frontend/client/"
        "lib/libaisuite-openai-codex-frontend-client.so.3"
        "bin/codex-backend-client"
    )
endif()
if(NOT AISUITE_BUILD_APPS)
    list(APPEND forbidden_package_entries "bin/codex-backend")
endif()
foreach(forbidden IN LISTS forbidden_package_entries)
    string(FIND "${listing}" "${forbidden}" found)
    if(NOT found EQUAL -1)
        message(
            FATAL_ERROR
                "CodexPolicyBinaryPackageLeak: binary package contains private path ${forbidden}"
        )
    endif()
endforeach()

# The library file alone is insufficient: prove that the binary archive ships
# the additive imported target through which an installed protocol-only
# consumer resolves it.
set(
    binary_package_export_extract
    "${AISUITE_BUILD_DIR}/binary-package-export-extract"
)
file(REMOVE_RECURSE "${binary_package_export_extract}")
file(MAKE_DIRECTORY "${binary_package_export_extract}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xzf "${archive}"
    WORKING_DIRECTORY "${binary_package_export_extract}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "unable to extract binary package target export")
endif()
file(
    GLOB binary_package_root_candidates
    LIST_DIRECTORIES TRUE
    "${binary_package_export_extract}/AISuite-*"
)
set(binary_package_roots)
foreach(candidate IN LISTS binary_package_root_candidates)
    if(IS_DIRECTORY "${candidate}")
        list(APPEND binary_package_roots "${candidate}")
    endif()
endforeach()
list(LENGTH binary_package_roots binary_package_root_count)
if(NOT binary_package_root_count EQUAL 1)
    message(
        FATAL_ERROR
            "expected one extracted binary-package root, found ${binary_package_root_count}"
    )
endif()
list(GET binary_package_roots 0 binary_package_root)
set(
    binary_package_targets
    "${binary_package_root}/lib/cmake/AISuite/AISuiteTargets.cmake"
)
if(NOT EXISTS "${binary_package_targets}")
    message(
        FATAL_ERROR
            "binary package is missing the AISuite imported-target export"
    )
endif()
file(READ "${binary_package_targets}" binary_package_targets_text)
string(
    FIND "${binary_package_targets_text}"
         "add_library(AISuite::OpenAICodexFrontendProtocol SHARED IMPORTED)"
         binary_package_protocol_target_index
)
if(binary_package_protocol_target_index EQUAL -1)
    message(
        FATAL_ERROR
            "binary package target export omits the shared AISuite::OpenAICodexFrontendProtocol imported target"
    )
endif()
