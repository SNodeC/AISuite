if(NOT DEFINED AISUITE_BUILD_DIR OR NOT DEFINED AISUITE_SOURCE_DIR)
    message(FATAL_ERROR "AISUITE_BUILD_DIR and AISUITE_SOURCE_DIR are required")
endif()
if(NOT DEFINED CMAKE_CPACK_COMMAND)
    set(CMAKE_CPACK_COMMAND cpack)
endif()
set(
    source_package_ignore_files
    "/[.]git/;/[.]qtcreator/;/__pycache__/;[.]py[co]$;/build[^/]*/;/stage[^/]*/;/package[^/]*/;(^|/)CMakeFiles/;(^|/)CMakeCache[.]txt$;~$"
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
    cmake/AISuiteCodexFrontendDependencies.cmake
    src/ai/openai/codex/AppServerClient.cpp
    src/ai/openai/codex/Api.h
    src/ai/openai/codex/typed/Apps.h
    src/ai/openai/codex/typed/ExternalAgents.h
    src/ai/openai/codex/typed/Feedback.h
    src/ai/openai/codex/typed/Hooks.h
    src/ai/openai/codex/typed/Marketplace.h
    src/ai/openai/codex/typed/Plugins.h
    src/ai/openai/codex/typed/Skills.h
    src/ai/openai/codex/typed/WindowsSandbox.h
    src/ai/openai/codex/frontend/FrontendService.cpp
    src/ai/openai/codex/frontend/FrontendService.h
    src/ai/openai/codex/frontend/CMakeLists.txt
    src/ai/openai/codex/frontend/Codec.cpp
    src/ai/openai/codex/frontend/Codec.h
    src/ai/openai/codex/frontend/GeneratedProtocol.h
    src/ai/openai/codex/frontend/GeneratedProtocolSchema.inc
    src/ai/openai/codex/frontend/Messages.cpp
    src/ai/openai/codex/frontend/Messages.h
    src/ai/openai/codex/frontend/Protocol.h
    src/ai/openai/codex/frontend/Security.h
    src/ai/openai/codex/frontend/detail/EventRepresentation.h
    src/ai/openai/codex/frontend/detail/GeneratedSchemaValidator.cpp
    src/ai/openai/codex/frontend/detail/GeneratedSchemaValidator.h
    src/ai/openai/codex/frontend/internal/model/Journal.cpp
    src/ai/openai/codex/frontend/internal/model/Journal.h
    src/ai/openai/codex/frontend/internal/model/Model.cpp
    src/ai/openai/codex/frontend/internal/model/Model.h
    src/ai/openai/codex/frontend/internal/model/Occurrence.cpp
    src/ai/openai/codex/frontend/internal/model/Occurrence.h
    src/ai/openai/codex/frontend/internal/model/Projection.cpp
    src/ai/openai/codex/frontend/internal/model/Projection.h
    src/ai/openai/codex/frontend/internal/server/BackendProjection.cpp
    src/ai/openai/codex/frontend/internal/server/BackendProjection.h
    src/ai/openai/codex/frontend/internal/server/ServerCore.cpp
    src/ai/openai/codex/frontend/internal/server/ServerCore.h
    src/ai/openai/codex/frontend/internal/client/ClientCore.cpp
    src/ai/openai/codex/frontend/internal/client/ClientCore.h
    src/ai/openai/codex/frontend/client/Client.cpp
    src/ai/openai/codex/frontend/client/Client.h
    src/ai/openai/codex/frontend/client/CMakeLists.txt
    src/ai/openai/codex/frontend/client/Export.h
    src/ai/openai/codex/frontend/client/GeneratedBindings.h
    src/ai/openai/codex/frontend/client/GeneratedFacades.cpp
    src/ai/openai/codex/frontend/client/OperationCodecs.cpp
    src/ai/openai/codex/frontend/client/Requests.h
    src/ai/openai/codex/frontend/client/State.cpp
    src/ai/openai/codex/frontend/client/State.h
    src/ai/openai/codex/frontend/client/StateTypes.h
    src/ai/openai/codex/frontend/client/Threads.h
    src/ai/openai/codex/frontend/client/Turns.h
    src/ai/openai/codex/frontend/client/detail/BoundOperation.h
    src/ai/openai/codex/frontend/client/detail/ClientTestAccess.h
    src/ai/openai/codex/frontend/client/detail/OperationCodecs.h
    src/ai/openai/codex/frontend/client/detail/StateReducer.h
    src/apps/codex-backend/Configuration.cpp
    src/apps/codex-backend/FrontendRuntimeBridge.cpp
    src/apps/codex-backend/FrontendRuntimeBridge.h
    src/apps/codex-backend/FrontendStreamSocketContext.cpp
    src/apps/codex-backend/FrontendWebApplication.h
    src/apps/codex-backend/FrontendWebApplication.cpp
    src/apps/codex-backend/FrontendWebSecurity.cpp
    src/apps/codex-backend/FrontendWebSecurity.h
    src/apps/codex-backend/FrontendWebSocketSubProtocol.cpp
    src/apps/codex-backend/FrontendWebSocketSubProtocol.h
    src/apps/codex-backend/FrontendWebSocketSubProtocolFactory.cpp
    src/apps/codex-backend/FrontendWebSocketSubProtocolFactory.h
    src/apps/codex-backend/ReferenceAuthentication.cpp
    src/apps/codex-backend/UnixPeerCredentials.cpp
    src/apps/codex-backend/main.cpp
    src/apps/codex-backend-client/main.cpp
    src/apps/codex-backend-client/ClientAuthentication.cpp
    src/apps/codex-backend-client/ClientAuthentication.h
    src/apps/codex-backend-client/FrontendWebSocketClient.cpp
    tools/frontend/cpp-client-bindings.json
    tools/frontend/generate_cpp_frontend_client.py
    tools/codex/app_server_surface.py
    tools/codex/app-server-schema/0.144.6/PROVENANCE.json
    tests/CMakeLists.txt
    tests/AISuiteCodexFrontendDependencyPolicyTest.cmake
    tests/AISuiteBinaryPackageTest.cmake
    tests/AISuiteInstalledConsumerTest.cmake
    tests/AISuiteSourcePackageTest.cmake
    tests/policy/CMakeLists.txt
    tests/policy/support/AISuiteSourcePolicyTestRoot.h
    tests/policy/support/CxxSourceScanner.h
    tests/policy/codex/CMakeLists.txt
    tests/policy/codex/CodexPublicHeaderPolicyTest.cpp
    tests/policy/codex/CodexPublicHeaderSelfContainmentTest.cmake
    tests/policy/codex/CodexFrontendCoreDependencyPolicyTest.py
    tests/policy/codex/CodexLoggingApiSurfacePolicyTest.cpp
    tests/policy/codex/CodexPolicyMutationTest.py
    tests/policy/codex/CodexSemanticLoggerPolicyTest.cpp
    tests/policy/codex/CodexSemanticLoggerAuthority.tsv
    tests/policy/codex/CodexSemanticLoggerClassifications.tsv
    tests/policy/security/CMakeLists.txt
    tests/policy/security/CodexSyntheticSecretLeakGuardTest.py
    tests/component/codex/CodexA14RuntimePlatformCurrentStateTest.py
    tests/component/codex/CMakeLists.txt
    tests/component/codex/CodexBackendClientAuthenticationTest.cpp
    tests/component/codex/CodexFrontendClientBindingTest.cpp
    tests/component/codex/CodexFrontendClientGeneratorTest.py
    tests/component/codex/CodexFrontendClientLifecycleTest.cpp
    tests/component/codex/CodexFrontendClientSymbolVisibilityTest.cmake
    tests/component/codex/CodexFrontendClientSynchronizationTest.cpp
    tests/component/codex/CodexFrontendServiceClientIntegrationTest.cpp
    tests/component/codex/CodexFrontendProtocolTargetIsolationTest.cpp
    tests/component/codex/CodexFrontendProtocolMinimalConfigurationTest.py
    tests/component/codex/CodexFrontendProtocolLegacyBinaryCompatibilityTest.py
    tests/component/codex/CodexFrontendLegacyBinaryConsumer.cpp
    tests/component/codex/CodexFrontendTypedModelTest.cpp
    tests/component/codex/CodexFrontendTypedOccurrenceTest.cpp
    tests/component/codex/CodexFrontendTypedSnapshotTest.cpp
    tests/component/codex/CodexFrontendTypedJournalTest.cpp
    tests/component/codex/CodexFrontendProjectionSecurityTest.cpp
    tests/component/codex/CodexFrontendServerCoreLifecycleTest.cpp
    tests/component/codex/CodexFrontendServerCoreSynchronizationTest.cpp
    tests/component/codex/CodexFrontendServerCoreCommandTest.cpp
    tests/component/codex/CodexFrontendServerCoreSecurityTest.cpp
    tests/component/codex/CodexFrontendServerCoreBackpressureTest.cpp
    tests/component/codex/CodexFrontendClientCoreLifecycleTest.cpp
    tests/component/codex/CodexFrontendClientCoreSynchronizationTest.cpp
    tests/component/codex/CodexFrontendClientCoreOperationTest.cpp
    tests/component/codex/CodexFrontendClientCoreStateTest.cpp
    tests/component/codex/CodexFrontendClientCoreCapacityTest.cpp
    tests/component/codex/CodexFrontendServerDifferentialTest.cpp
    tests/component/codex/CodexFrontendClientDifferentialTest.cpp
    tests/component/codex/CodexFrontendDifferentialAuthority.h
    tests/component/codex/CodexFrontendDifferentialComparison.h
    tests/component/codex/CodexFrontendDifferentialCoverageGuardTest.py
    tests/component/codex/CodexFrontendDifferentialExecutionLedger.h
    tests/component/codex/CodexFrontendDifferentialMutationProbe.cpp
    tests/component/codex/CodexFrontendDifferentialMutationTest.py
    tests/component/codex/CodexFrontendCompatibilityAdapters.h
    tests/component/codex/CodexFrontendCompatibilityAdaptersTest.cpp
    tests/component/codex/fixtures/p2-frontend-differential-coverage.json
    tests/component/codex/fixtures/frontend-protocol-v1.generated.json
    tests/component/codex/fixtures/frontend-client-reducer/conformance.json
    tests/installed/codex/CMakeLists.txt
    tests/installed/codex/CodexFrontendProtocolConsumer.cpp
    tests/installed/codex/CodexApiConsumer.cpp
    tests/installed/codex/CodexApiExample.cpp
    tests/installed/codex/CodexApplicationProjectionConsumer.cpp
    tests/installed/codex/CodexBackendFrontendConsumer.cpp
    tests/installed/codex/CodexFrontendClientConsumer.cpp
    tests/installed/codex/SNodeInstalledCoreConsumer.cpp
    tests/installed/codex-module-server/CMakeLists.txt
    tests/installed/codex-module-server/CodexModuleServer.cpp
    docs/ai/openai/codex/a1-4-user-facing-integrations.md
    docs/ai/openai/codex/a1-4-runtime-and-platform-long-tail.md
    docs/ai/openai/codex/a1-6a-backend-foundation.md
    docs/ai/openai/codex/a1-7b-frontend-service.md
    docs/ai/openai/codex/a1-7c-1-cpp-frontend-sdk-architecture.md
    docs/ai/openai/codex/a1-7c-1-cpp-frontend-sdk.md
    docs/ai/openai/codex/a1-final-abi-transition.md
    docs/ai/openai/codex/architecture-reduction/p2-greenfield-frontend.md
    src/apps/codex-backend/FrontendCloseReason.h
    src/apps/codex-backend/README.md
)
    if(NOT EXISTS "${root}/${required}")
        message(
            FATAL_ERROR
                "CodexPolicySourcePackageMismatch: source package missing ${required}"
        )
    endif()
endforeach()

foreach(
    forbidden
    src/ai/openai/codex/frontend/BackendAdapter.cpp
    src/ai/openai/codex/frontend/BackendAdapter.h
    src/ai/openai/codex/frontend/FrontendClient.cpp
    src/ai/openai/codex/frontend/FrontendClient.h
)
    if(EXISTS "${root}/${forbidden}")
        message(
            FATAL_ERROR
                "CodexPolicySourcePackageMismatch: source package contains obsolete or future-phase frontend API ${forbidden}"
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
       packaged_entry MATCHES "(^|/)[.]git(/|$)" OR
       packaged_entry MATCHES "(^|/)CMakeFiles(/|$)" OR
       packaged_entry MATCHES "(^|/)CMakeCache[.]txt$")
        message(
            FATAL_ERROR
                "source package contains forbidden repository/build metadata: ${packaged_entry}"
        )
    endif()
endforeach()

find_program(python_executable python3 REQUIRED)
get_filename_component(package_parent "${root}" DIRECTORY)
set(package_check_environment
    "${CMAKE_COMMAND}" -E env
    "--unset=GIT_DIR"
    "--unset=GIT_WORK_TREE"
    "GIT_CEILING_DIRECTORIES=${package_parent}"
)

execute_process(
    COMMAND
        ${package_check_environment}
        "${python_executable}" -B
        "${root}/tools/frontend/generate_cpp_frontend_client.py"
        --check
    WORKING_DIRECTORY "${root}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(
        FATAL_ERROR
            "C++ frontend client binding authority is stale inside source package: ${result}"
    )
endif()

execute_process(
    COMMAND
        ${package_check_environment}
        "${python_executable}" -B
        "${root}/tests/component/codex/CodexA14RuntimePlatformCurrentStateTest.py"
        --repo-root "${root}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(
        FATAL_ERROR
            "current A1.4 registry/schema/API validation failed inside source package: ${result}"
    )
endif()
