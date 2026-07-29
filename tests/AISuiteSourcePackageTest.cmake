if(NOT DEFINED AISUITE_BUILD_DIR OR NOT DEFINED AISUITE_SOURCE_DIR)
    message(FATAL_ERROR "AISUITE_BUILD_DIR and AISUITE_SOURCE_DIR are required")
endif()
if(NOT DEFINED CMAKE_CPACK_COMMAND)
    set(CMAKE_CPACK_COMMAND cpack)
endif()
set(
    source_package_ignore_files
    "/[.]git/;/[.]qtcreator/;/__pycache__/;[.]py[co]$;/build[^/]*/;/stage[^/]*/;/package[^/]*/;~$"
)
set(
    source_package_guard_config
    "${AISUITE_BUILD_DIR}/CPackSourcePackageGuardConfig.cmake"
)
file(
    READ "${AISUITE_BUILD_DIR}/CPackSourceConfig.cmake"
    source_package_config
)
file(
    WRITE "${source_package_guard_config}"
    "${source_package_config}\nset(CPACK_SOURCE_IGNORE_FILES \"${source_package_ignore_files}\")\nset(CPACK_IGNORE_FILES \"${source_package_ignore_files}\")\n"
)

file(GLOB old_archives "${AISUITE_BUILD_DIR}/AISuite-*.tar.gz")
if(old_archives)
    file(REMOVE ${old_archives})
endif()
execute_process(
    COMMAND
        "${CMAKE_CPACK_COMMAND}"
        --config "${source_package_guard_config}"
        -G TGZ
    WORKING_DIRECTORY "${AISUITE_BUILD_DIR}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "AISuite source package failed: ${result}")
endif()
file(GLOB archives "${AISUITE_BUILD_DIR}/AISuite-*.tar.gz")
list(LENGTH archives archive_count)
if(NOT archive_count EQUAL 1)
    message(FATAL_ERROR "expected one source archive, found ${archive_count}")
endif()
list(GET archives 0 archive)
set(extract "${AISUITE_BUILD_DIR}/source-package-extract")
file(REMOVE_RECURSE "${extract}")
file(MAKE_DIRECTORY "${extract}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xzf "${archive}"
    WORKING_DIRECTORY "${extract}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "unable to extract source package")
endif()
file(GLOB roots "${extract}/AISuite-*")
list(GET roots 0 root)
foreach(required
    CMakeLists.txt
    src/ai/openai/codex/AppServerClient.cpp
    src/ai/openai/codex/typed/Apps.h
    src/ai/openai/codex/typed/ExternalAgents.h
    src/ai/openai/codex/typed/Feedback.h
    src/ai/openai/codex/typed/Hooks.h
    src/ai/openai/codex/typed/Marketplace.h
    src/ai/openai/codex/typed/Plugins.h
    src/ai/openai/codex/typed/Skills.h
    src/apps/codex-backend/main.cpp
    src/apps/codex-backend-client/main.cpp
    tools/codex/app_server_a1_4.py
    tools/codex/app_server_a1_4_user_integrations.py
    tools/codex/app_server_a1_4_user_integrations_abi.py
    tools/codex/app_server_a1_4_user_integrations_closure.py
    tools/codex/app-server-schema/0.144.6/PROVENANCE.json
    tools/codex/app-server-evidence/0.144.6/a1-4-implementation-plan.json
    tools/codex/app-server-evidence/0.144.6/a1-4-user-integrations-start-state.json
    tools/codex/app-server-evidence/0.144.6/a1-4-user-integrations-batch-plan.json
    tools/codex/app-server-evidence/0.144.6/a1-4-user-integrations-api-abi-evidence.json
    tools/codex/app-server-evidence/0.144.6/a1-4-user-integrations-closure-report.json
    tools/codex/app-server-evidence/0.144.6/a1-4-user-integrations-generation-pre.json
    tools/codex/app-server-evidence/0.144.6/a1-4-user-integrations-generation-pass-1.json
    tools/codex/app-server-evidence/0.144.6/a1-4-user-integrations-generation-pass-2.json
    tools/codex/app-server-evidence/0.144.6/a1-4-user-integrations-symbols.txt
    tools/extraction/verify_codex_policy_ownership.py
    tools/extraction/verify_extraction.py
    tests/CMakeLists.txt
    tests/AISuiteBinaryPackageTest.cmake
    tests/AISuiteSourcePackageTest.cmake
    tests/policy/CMakeLists.txt
    tests/policy/support/AISuiteSourcePolicyTestRoot.h
    tests/policy/support/CxxSourceScanner.h
    tests/policy/codex/CMakeLists.txt
    tests/policy/codex/CodexPublicHeaderPolicyTest.cpp
    tests/policy/codex/CodexPublicHeaderSelfContainmentTest.cmake
    tests/policy/codex/CodexLoggingApiSurfacePolicyTest.cpp
    tests/policy/codex/CodexPolicyMutationTest.py
    tests/policy/codex/CodexSemanticLoggerPolicyTest.cpp
    tests/policy/codex/CodexSemanticLoggerAuthority.tsv
    tests/policy/codex/CodexSemanticLoggerClassifications.tsv
    tests/policy/security/CMakeLists.txt
    tests/policy/security/CodexSyntheticSecretLeakGuardTest.py
    tests/component/codex/CodexA14AuditToolTest.py
    tests/component/codex/CodexA14UserIntegrationsAuditTest.py
    tests/component/codex/CodexA14UserIntegrationsClosureTest.py
    tests/installed/codex/CodexA14UserIntegrationsAbiLayoutProbe.cpp
    tests/installed/codex/CodexTypedConsumer.cpp
    tests/installed/codex/SNodeInstalledCoreConsumer.cpp
    docs/ai/openai/codex/a1-4-user-facing-integrations.md
    docs/extraction/README.md
    docs/extraction/codex-policy-baseline-ctest.json
    docs/extraction/codex-policy-final-ctest.json
    docs/extraction/codex-policy-ownership.json
    docs/extraction/codex-policy-ownership.md
    docs/extraction/filter-map.json
    docs/extraction/source-manifest.json
    docs/extraction/validation.md
    docs/extraction/test-integrity.md
)
    if(NOT EXISTS "${root}/${required}")
        message(
            FATAL_ERROR
                "CodexPolicySourcePackageMismatch: source package missing ${required}"
        )
    endif()
endforeach()

file(
    GLOB_RECURSE packaged_entries
    LIST_DIRECTORIES TRUE
    RELATIVE "${root}"
    "${root}/*"
)
foreach(packaged_entry IN LISTS packaged_entries)
    string(REPLACE "\\" "/" packaged_entry "${packaged_entry}")
    if(packaged_entry STREQUAL ".git" OR
       packaged_entry MATCHES "(^|/)[.]git(/|$)")
        message(
            FATAL_ERROR
                "source package contains forbidden Git metadata: ${packaged_entry}"
        )
    endif()
endforeach()

find_program(git_executable git REQUIRED)
find_program(python_executable python3 REQUIRED)
get_filename_component(package_parent "${root}" DIRECTORY)
set(package_check_environment
    "${CMAKE_COMMAND}" -E env
    "--unset=GIT_DIR"
    "--unset=GIT_WORK_TREE"
    "GIT_CEILING_DIRECTORIES=${package_parent}"
)

# The extracted package is intentionally not a repository. The ceiling makes
# this assertion independent of where the CPack test itself was launched.
execute_process(
    COMMAND
        ${package_check_environment}
        "${git_executable}" -C "${root}" rev-parse --show-toplevel
    RESULT_VARIABLE result
    OUTPUT_VARIABLE package_git_output
    ERROR_VARIABLE package_git_error
)
if(result EQUAL 0)
    message(
        FATAL_ERROR
            "source package unexpectedly resolved Git history under GIT_CEILING_DIRECTORIES=${package_parent}: ${package_git_output}"
    )
endif()

execute_process(
    COMMAND
        ${package_check_environment}
        "${python_executable}" -B
        "${root}/tools/extraction/verify_extraction.py"
        check-package
        --repo-root "${root}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(
        FATAL_ERROR
            "extraction package-boundary check failed inside source package: ${result}"
    )
endif()

execute_process(
    COMMAND
        ${package_check_environment}
        "${python_executable}" -B
        "${root}/tools/extraction/verify_codex_policy_ownership.py"
        check-package
        --repo-root "${root}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(
        FATAL_ERROR
            "CodexPolicySourcePackageMismatch: Codex policy ownership check failed inside source package: ${result}"
    )
endif()

execute_process(
    COMMAND
        ${package_check_environment}
        "${python_executable}" -B
        "${root}/tools/codex/app_server_a1_4_user_integrations_abi.py"
        check
        --repo-root "${root}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(
        FATAL_ERROR
            "A1.4 user-integrations API/ABI evidence failed inside source package: ${result}"
    )
endif()
execute_process(
    COMMAND
        ${package_check_environment}
        "${python_executable}" -B
        "${root}/tools/codex/app_server_a1_4_user_integrations_closure.py"
        check-package
        --repo-root "${root}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(
        FATAL_ERROR
            "A1.4 user-integrations closure failed inside source package: ${result}"
    )
endif()
