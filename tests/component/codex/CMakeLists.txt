# SNode.C - A Slim Toolkit for Network Communication
# Copyright (C) Volker Christian <me@vchrist.at>
#               2020, 2021, 2022, 2023, 2024, 2025, 2026

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

if(AISUITE_BUILD_CODEX_FRONTEND_CLIENT)
    add_test(
        NAME CodexFrontendClientBindingsCurrentnessTest
        COMMAND
            ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tools/frontend/generate_cpp_frontend_client.py
            --check
    )
    add_test(
        NAME CodexFrontendClientGeneratorTest
        COMMAND
            ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/CodexFrontendClientGeneratorTest.py
    )
    set_tests_properties(
        CodexFrontendClientBindingsCurrentnessTest
        CodexFrontendClientGeneratorTest
        PROPERTIES
            LABELS "component;ai;openai;codex;frontend;client;generated;a1-7c-1"
            TIMEOUT 10
    )

    snodec_add_test(
        CodexFrontendClientBindingTest
        CodexFrontendClientBindingTest.cpp
    )
    target_link_libraries(
        CodexFrontendClientBindingTest
        PRIVATE
            snodec-test-support
            ai-openai-codex-frontend-client-test-support
    )
    target_compile_features(CodexFrontendClientBindingTest PRIVATE cxx_std_20)
    set_tests_properties(
        CodexFrontendClientBindingTest
        PROPERTIES
            LABELS "component;ai;openai;codex;frontend;client;api;generated;a1-7c-1"
            TIMEOUT 5
    )

    snodec_add_test(
        CodexFrontendClientLifecycleTest
        CodexFrontendClientLifecycleTest.cpp
    )
    target_link_libraries(
        CodexFrontendClientLifecycleTest
        PRIVATE
            snodec-test-support
            ai-openai-codex-frontend-client-test-support
    )
    target_compile_features(CodexFrontendClientLifecycleTest PRIVATE cxx_std_20)
    set_tests_properties(
        CodexFrontendClientLifecycleTest
        PROPERTIES
            LABELS "component;ai;openai;codex;frontend;client;lifecycle;correlation;a1-7c-1"
            TIMEOUT 5
    )

    snodec_add_test(
        CodexFrontendServiceClientIntegrationTest
        CodexFrontendServiceClientIntegrationTest.cpp
    )
    target_link_libraries(
        CodexFrontendServiceClientIntegrationTest
        PRIVATE
            snodec-test-support
            ai-openai-codex-frontend-client-test-support
            AISuite::OpenAICodexFrontend
            snodec::core
    )
    target_compile_features(CodexFrontendServiceClientIntegrationTest PRIVATE cxx_std_20)
    set_tests_properties(
        CodexFrontendServiceClientIntegrationTest
        PROPERTIES
            LABELS "component;ai;openai;codex;frontend;client;service;integration;a1-7c-1"
            TIMEOUT 40
    )

    snodec_add_test(
        CodexFrontendPublicClientAdapterTest
        CodexFrontendPublicClientAdapterTest.cpp
    )
    target_link_libraries(
        CodexFrontendPublicClientAdapterTest
        PRIVATE
            snodec-test-support
            ai-openai-codex-frontend-client-test-support
            ai-openai-codex-frontend-client-core
            ai-openai-codex-frontend-model
    )
    target_compile_features(
        CodexFrontendPublicClientAdapterTest PRIVATE cxx_std_20
    )
    set_tests_properties(
        CodexFrontendPublicClientAdapterTest
        PROPERTIES
            LABELS "component;ai;openai;codex;frontend;client;adapter;state-builder;transaction;p3"
            TIMEOUT 10
    )

    add_test(
        NAME CodexFrontendClientSymbolVisibilityTest
        COMMAND
            ${CMAKE_COMMAND}
            -DAISUITE_FRONTEND_CLIENT_LIBRARY=$<TARGET_FILE:ai-openai-codex-frontend-client>
            -DAISUITE_NM=${CMAKE_NM}
            -DAISUITE_READELF=${CMAKE_READELF}
            -P
            ${CMAKE_CURRENT_SOURCE_DIR}/CodexFrontendClientSymbolVisibilityTest.cmake
    )
    set_tests_properties(
        CodexFrontendClientSymbolVisibilityTest
        PROPERTIES
            LABELS "component;ai;openai;codex;frontend;client;abi;symbols;a1-7c-1"
            TIMEOUT 5
    )

    add_executable(
        CodexFrontendClientSourceCompatibilityTest
        CodexFrontendClientSourceCompatibilityTest.cpp
    )
    target_compile_features(
        CodexFrontendClientSourceCompatibilityTest PRIVATE cxx_std_20
    )
    target_link_libraries(
        CodexFrontendClientSourceCompatibilityTest
        PRIVATE AISuite::OpenAICodexFrontendClient
    )
    add_test(
        NAME CodexFrontendClientSourceCompatibilityTest
        COMMAND CodexFrontendClientSourceCompatibilityTest
    )
    set_tests_properties(
        CodexFrontendClientSourceCompatibilityTest
        PROPERTIES
            LABELS "component;ai;openai;codex;frontend;client;source-compatibility;p3"
            TIMEOUT 5
    )
endif()

set(CODEX_PHASE_A0_SCHEMA_ROOT
    ${PROJECT_SOURCE_DIR}/tools/codex/app-server-schema/0.144.6
)
set(CODEX_PHASE_A0_PROVENANCE
    ${CODEX_PHASE_A0_SCHEMA_ROOT}/PROVENANCE.json
)
set(CODEX_PHASE_A0_SURFACE_MANIFEST
    ${PROJECT_SOURCE_DIR}/tools/codex/app-server-surface/0.144.6.json
)
set(CODEX_PHASE_A0_SURFACE_TOOL
    ${PROJECT_SOURCE_DIR}/tools/codex/app_server_surface.py
)
set(CODEX_PHASE_A0_TRANSCRIPT_FIXTURES
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/app-server-0.144.6
)
set(CODEX_PHASE_A0_REGISTRY
    ${PROJECT_SOURCE_DIR}/src/ai/openai/codex/detail/ProtocolSurfaceRegistryData.inc
)
set(CODEX_PHASE_A0_COVERAGE_DOC
    ${PROJECT_SOURCE_DIR}/docs/ai/openai/codex/app-server-api-coverage.md
)
set(CODEX_PHASE_A0_SECURITY_DOC
    ${PROJECT_SOURCE_DIR}/docs/ai/openai/codex/app-server-security-decisions.md
)
set(
    CODEX_PHASE_A0_PYTHON_ARGUMENTS
    --tool
    ${CODEX_PHASE_A0_SURFACE_TOOL}
    --schema-root
    ${CODEX_PHASE_A0_SCHEMA_ROOT}
    --provenance
    ${CODEX_PHASE_A0_PROVENANCE}
    --manifest
    ${CODEX_PHASE_A0_SURFACE_MANIFEST}
    --fixtures
    ${CODEX_PHASE_A0_TRANSCRIPT_FIXTURES}
    --registry
    ${CODEX_PHASE_A0_REGISTRY}
    --coverage-doc
    ${CODEX_PHASE_A0_COVERAGE_DOC}
    --security-doc
    ${CODEX_PHASE_A0_SECURITY_DOC}
)

add_test(
    NAME CodexVendoredAppServerSchemaTest
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/CodexAppServerSurfaceToolTest.py vendored
        ${CODEX_PHASE_A0_PYTHON_ARGUMENTS}
)
add_test(
    NAME CodexAppServerSurfaceExtractionTest
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/CodexAppServerSurfaceToolTest.py extraction
        ${CODEX_PHASE_A0_PYTHON_ARGUMENTS}
)
add_test(
    NAME CodexAppServerGeneratedArtifactsGuardTest
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/CodexAppServerSurfaceToolTest.py artifacts
        ${CODEX_PHASE_A0_PYTHON_ARGUMENTS}
)
add_test(
    NAME CodexFrontendProtocolGeneratedArtifactsGuardTest
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/CodexFrontendProtocolGeneratorTest.py
        --app-tool
        ${CODEX_PHASE_A0_SURFACE_TOOL}
        --app-manifest
        ${CODEX_PHASE_A0_SURFACE_MANIFEST}
        --registry
        ${CODEX_PHASE_A0_REGISTRY}
        --source
        ${PROJECT_SOURCE_DIR}/tools/frontend/frontend-registry-source.json
        --generator
        ${PROJECT_SOURCE_DIR}/tools/frontend/generate_frontend_protocol.py
        --manifest
        ${PROJECT_SOURCE_DIR}/docs/ai/openai/codex/frontend-protocol-v1.manifest.json
        --header
        ${PROJECT_SOURCE_DIR}/src/ai/openai/codex/frontend/GeneratedProtocol.h
        --schema-template
        ${PROJECT_SOURCE_DIR}/tools/frontend/frontend-protocol-v1.schema.template.json
        --schema
        ${PROJECT_SOURCE_DIR}/docs/ai/openai/codex/frontend-protocol-v1.schema.json
        --schema-data
        ${PROJECT_SOURCE_DIR}/src/ai/openai/codex/frontend/GeneratedProtocolSchema.inc
        --fixtures
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/frontend-protocol-v1.generated.json
        --review-identities
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/frontend-contract-review-identities.json
)
add_test(
    NAME CodexSchemaTranscriptFixtureValidationTest
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/CodexAppServerSurfaceToolTest.py fixtures
        ${CODEX_PHASE_A0_PYTHON_ARGUMENTS}
)
set_tests_properties(
    CodexVendoredAppServerSchemaTest
    CodexAppServerSurfaceExtractionTest
    CodexAppServerGeneratedArtifactsGuardTest
    CodexFrontendProtocolGeneratedArtifactsGuardTest
    CodexSchemaTranscriptFixtureValidationTest
    PROPERTIES LABELS "component;ai;openai;codex;phase-a0;schema" TIMEOUT 30
)

add_test(
    NAME CodexAppServerOperationContractsTest
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/CodexAppServerContractsToolTest.py
        --tool
        ${PROJECT_SOURCE_DIR}/tools/codex/app_server_contracts.py
        --source-root
        ${PROJECT_SOURCE_DIR}/tools/codex/app-server-protocol-source/0.144.6
        --schema-root
        ${CODEX_PHASE_A0_SCHEMA_ROOT}
        --manifest
        ${CODEX_PHASE_A0_SURFACE_MANIFEST}
        --schema-provenance
        ${CODEX_PHASE_A0_PROVENANCE}
        --evidence-root
        ${PROJECT_SOURCE_DIR}/tools/codex/app-server-evidence/0.144.6
)
set_tests_properties(
    CodexAppServerOperationContractsTest
    PROPERTIES
        LABELS "component;ai;openai;codex;phase-a1-0;schema;operation-contract"
        TIMEOUT 30
)

set(CODEX_PHASE_A1_1_EVIDENCE_ROOT
    ${PROJECT_SOURCE_DIR}/tools/codex/app-server-evidence/0.144.6
)
add_test(
    NAME CodexA11A13CurrentStateTest
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/CodexA11A13CurrentStateTest.py
        --repo-root
        ${PROJECT_SOURCE_DIR}
)
set_tests_properties(
    CodexA11A13CurrentStateTest
    PROPERTIES
        LABELS
            "component;ai;openai;codex;phase-a1-1;phase-a1-2;phase-a1-3;schema;registry;codec;current-state"
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        TIMEOUT 20
)

add_test(
    NAME CodexA11OperationProductionCoverageGuardTest
    COMMAND
        ${Python3_EXECUTABLE}
        ${CODEX_PHASE_A0_SURFACE_TOOL}
        operation-production-coverage
        --manifest
        ${CODEX_PHASE_A0_SURFACE_MANIFEST}
        --evidence-root
        ${CODEX_PHASE_A1_1_EVIDENCE_ROOT}
        --fixture-index
        ${PROJECT_SOURCE_DIR}/tools/codex/app-server-fixtures/0.144.6/index.json
        --repo-root
        ${PROJECT_SOURCE_DIR}
        --output
        ${CODEX_PHASE_A1_1_EVIDENCE_ROOT}/a1-1-operation-production-coverage.json
        --check
)
set_tests_properties(
    CodexA11OperationProductionCoverageGuardTest
    PROPERTIES
        LABELS "component;ai;openai;codex;phase-a1-1;operation;codec;corpus;generated"
        TIMEOUT 30
)

add_test(
    NAME CodexA11NotificationProductionCoverageGuardTest
    COMMAND
        ${Python3_EXECUTABLE}
        ${CODEX_PHASE_A0_SURFACE_TOOL}
        notification-production-coverage
        --manifest
        ${CODEX_PHASE_A0_SURFACE_MANIFEST}
        --evidence-root
        ${CODEX_PHASE_A1_1_EVIDENCE_ROOT}
        --fixture-index
        ${PROJECT_SOURCE_DIR}/tools/codex/app-server-fixtures/0.144.6/index.json
        --repo-root
        ${PROJECT_SOURCE_DIR}
        --output
        ${CODEX_PHASE_A1_1_EVIDENCE_ROOT}/a1-1-notification-production-coverage.json
        --check
)
set_tests_properties(
    CodexA11NotificationProductionCoverageGuardTest
    PROPERTIES
        LABELS "component;ai;openai;codex;phase-a1-1;notification;codec;corpus;generated"
        TIMEOUT 30
)

add_test(
    NAME CodexA14RuntimePlatformCurrentStateTest
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/CodexA14RuntimePlatformCurrentStateTest.py
        --repo-root
        ${PROJECT_SOURCE_DIR}
)
set_tests_properties(
    CodexA14RuntimePlatformCurrentStateTest
    PROPERTIES
        LABELS "component;ai;openai;codex;phase-a1-4;schema;registry;current-state"
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        TIMEOUT 10
)

add_test(
    NAME CodexDraft07ValidatorTest
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/CodexDraft07ValidatorTest.py
)
set_tests_properties(
    CodexDraft07ValidatorTest
    PROPERTIES LABELS "component;ai;openai;codex;phase-a1-0;schema;validator"
               TIMEOUT 10
)

add_test(
    NAME CodexAppServerFixtureInfrastructureTest
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/CodexAppServerFixtureToolTest.py
)
set_tests_properties(
    CodexAppServerFixtureInfrastructureTest
    PROPERTIES LABELS
               "component;ai;openai;codex;phase-a1-0;schema;fixtures"
               ENVIRONMENT "PYTHONHASHSEED=1"
               TIMEOUT 420
)

add_test(
    NAME CodexFrontendProtocolByteIdentityTest
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/CodexFrontendProtocolByteIdentityTest.py
        --schema
        ${PROJECT_SOURCE_DIR}/docs/ai/openai/codex/frontend-protocol-v1.schema.json
)
set_tests_properties(
    CodexFrontendProtocolByteIdentityTest
    PROPERTIES LABELS "component;ai;openai;codex;phase-a1-0;frontend;guard"
               TIMEOUT 5
)

snodec_add_test(
    CodexProtocolSurfaceRegistryTest CodexProtocolSurfaceRegistryTest.cpp
)
target_link_libraries(
    CodexProtocolSurfaceRegistryTest PRIVATE snodec-test-support
                                             AISuite::OpenAICodex
)
target_compile_features(CodexProtocolSurfaceRegistryTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexProtocolSurfaceRegistryTest
    PROPERTIES LABELS "component;ai;openai;codex;phase-a0;registry" TIMEOUT 5
)

snodec_add_test(
    CodexProtocolSurfaceCoverageGuardTest
    CodexProtocolSurfaceCoverageGuardTest.cpp
)
target_link_libraries(
    CodexProtocolSurfaceCoverageGuardTest
    PRIVATE snodec-test-support AISuite::OpenAICodex
)
target_compile_features(
    CodexProtocolSurfaceCoverageGuardTest PRIVATE cxx_std_20
)
target_compile_definitions(
    CodexProtocolSurfaceCoverageGuardTest
    PRIVATE CODEX_PHASE_A0_SURFACE_MANIFEST="${CODEX_PHASE_A0_SURFACE_MANIFEST}"
)
set_tests_properties(
    CodexProtocolSurfaceCoverageGuardTest
    PROPERTIES LABELS "component;ai;openai;codex;phase-a0;registry;guard"
               TIMEOUT 5
)

snodec_add_test(CodexSchemaTranscriptUnixTest CodexSchemaTranscriptUnixTest.cpp)
target_link_libraries(
    CodexSchemaTranscriptUnixTest
    PRIVATE snodec-test-support AISuite::OpenAICodexBackend
)
target_compile_features(CodexSchemaTranscriptUnixTest PRIVATE cxx_std_20)
target_compile_definitions(
    CodexSchemaTranscriptUnixTest
    PRIVATE
        CODEX_PHASE_A0_TRANSCRIPT_FIXTURES="${CODEX_PHASE_A0_TRANSCRIPT_FIXTURES}"
)
set_tests_properties(
    CodexSchemaTranscriptUnixTest
    PROPERTIES LABELS "component;ai;openai;codex;phase-a0;schema;unix"
               TIMEOUT 5
)

add_executable(CodexFakeAppServer CodexFakeAppServer.cpp)
target_compile_features(CodexFakeAppServer PRIVATE cxx_std_20)

snodec_add_test(CodexStdioLifecycleTest CodexStdioLifecycleTest.cpp)
target_link_libraries(
    CodexStdioLifecycleTest PRIVATE snodec-test-support AISuite::OpenAICodex
                                    snodec::core
)
target_compile_features(CodexStdioLifecycleTest PRIVATE cxx_std_20)
target_compile_definitions(
    CodexStdioLifecycleTest
    PRIVATE CODEX_FAKE_APP_SERVER="$<TARGET_FILE:CodexFakeAppServer>"
)
add_dependencies(CodexStdioLifecycleTest CodexFakeAppServer)

add_executable(
    CodexStdioEnvironmentTest
    CodexStdioEnvironmentTest.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/Configuration.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/FrontendWebSecurity.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/ReferenceAuthentication.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/ReferenceInvocationPolicy.cpp
)
target_include_directories(
    CodexStdioEnvironmentTest PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(
    CodexStdioEnvironmentTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodex
        AISuite::OpenAICodexBackend
        AISuite::OpenAICodexFrontend
        snodec::core
)
target_compile_features(CodexStdioEnvironmentTest PRIVATE cxx_std_20)
target_compile_definitions(
    CodexStdioEnvironmentTest
    PRIVATE CODEX_FAKE_APP_SERVER="$<TARGET_FILE:CodexFakeAppServer>"
)
add_dependencies(CodexStdioEnvironmentTest CodexFakeAppServer)
foreach(scenario IN ITEMS configured inherited)
    add_test(
        NAME CodexStdioEnvironment_${scenario}
        COMMAND CodexStdioEnvironmentTest ${scenario}
    )
    set_tests_properties(
        CodexStdioEnvironment_${scenario}
        PROPERTIES
            ENVIRONMENT "CODEX_HOME=/tmp/aisuite-parent-codex-home"
            LABELS "component;ai;openai;codex;stdio;environment;configuration"
            SKIP_RETURN_CODE 77
            TIMEOUT 8
    )
endforeach()

add_executable(CodexStdioProtocolFlowTest CodexStdioProtocolFlowTest.cpp)
target_link_libraries(
    CodexStdioProtocolFlowTest PRIVATE snodec-test-support
                                       AISuite::OpenAICodex snodec::core
)
target_compile_features(CodexStdioProtocolFlowTest PRIVATE cxx_std_20)
target_compile_definitions(
    CodexStdioProtocolFlowTest
    PRIVATE CODEX_FAKE_APP_SERVER="$<TARGET_FILE:CodexFakeAppServer>"
)
add_dependencies(CodexStdioProtocolFlowTest CodexFakeAppServer)

foreach(scenario IN ITEMS flow typed-flow exit-restart)
    string(REPLACE "-" "_" scenario_name ${scenario})
    set(protocol_flow_labels "component;ai;openai;codex;stdio;protocol")
    if(scenario STREQUAL "typed-flow")
        list(APPEND protocol_flow_labels typed)
    endif()
    add_test(NAME CodexStdioProtocolFlow_${scenario_name}
             COMMAND CodexStdioProtocolFlowTest ${scenario}
    )
    set_tests_properties(
        CodexStdioProtocolFlow_${scenario_name}
        PROPERTIES LABELS "${protocol_flow_labels}" SKIP_RETURN_CODE 77 TIMEOUT
                   10
    )
endforeach()

snodec_add_test(CodexProtocolCodecTest CodexProtocolCodecTest.cpp)
target_link_libraries(
    CodexProtocolCodecTest PRIVATE snodec-test-support AISuite::OpenAICodex
)
target_compile_features(CodexProtocolCodecTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexProtocolCodecTest
    PROPERTIES LABELS "component;ai;openai;codex;protocol" SKIP_RETURN_CODE 77
               TIMEOUT 5
)

snodec_add_test(CodexProtocolEngineTest CodexProtocolEngineTest.cpp)
target_link_libraries(
    CodexProtocolEngineTest PRIVATE snodec-test-support AISuite::OpenAICodex
                                    snodec::core
)
target_compile_features(CodexProtocolEngineTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexProtocolEngineTest
    PROPERTIES LABELS "component;ai;openai;codex;protocol;engine"
               SKIP_RETURN_CODE 77 TIMEOUT 30
)

snodec_add_test(CodexA11OperationWireTest CodexA11OperationWireTest.cpp)
target_link_libraries(
    CodexA11OperationWireTest
    PRIVATE snodec-test-support AISuite::OpenAICodex snodec::core
)
target_compile_features(CodexA11OperationWireTest PRIVATE cxx_std_20)
target_compile_definitions(
    CodexA11OperationWireTest
    PRIVATE CODEX_A1_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tools/codex/app-server-fixtures/0.144.6"
)
target_compile_options(
    CodexA11OperationWireTest PRIVATE -Wno-deprecated-declarations
)
set_tests_properties(
    CodexA11OperationWireTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;operation;wire;a1-1"
        SKIP_RETURN_CODE 77
        TIMEOUT 30
)

snodec_add_test(CodexA12AccountCodecTest CodexA12AccountCodecTest.cpp)
target_link_libraries(
    CodexA12AccountCodecTest
    PRIVATE snodec-test-support AISuite::OpenAICodex
)
target_compile_features(CodexA12AccountCodecTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexA12AccountCodecTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;account;codec;a1-2;b2"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(CodexA12AccountWireTest CodexA12AccountWireTest.cpp)
target_link_libraries(
    CodexA12AccountWireTest
    PRIVATE snodec-test-support AISuite::OpenAICodex snodec::core
)
target_compile_features(CodexA12AccountWireTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexA12AccountWireTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;account;operation;wire;a1-2;b2"
        SKIP_RETURN_CODE 77
        TIMEOUT 20
)

snodec_add_test(CodexA12ModelCodecTest CodexA12ModelCodecTest.cpp)
target_link_libraries(
    CodexA12ModelCodecTest
    PRIVATE snodec-test-support AISuite::OpenAICodex
)
target_compile_features(CodexA12ModelCodecTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexA12ModelCodecTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;model;codec;a1-2;b3"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(
    CodexA14AppsExternalAgentFeedbackCodecTest
    CodexA14AppsExternalAgentFeedbackCodecTest.cpp
)
target_link_libraries(
    CodexA14AppsExternalAgentFeedbackCodecTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodexBackend
)
target_compile_features(
    CodexA14AppsExternalAgentFeedbackCodecTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA14AppsExternalAgentFeedbackCodecTest
    PROPERTIES
        LABELS
            "component;ai;openai;codex;typed;apps;external-agent;feedback;backend;codec;a1-4;user-integrations"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(
    CodexA14AppsExternalAgentFeedbackWireTest
    CodexA14AppsExternalAgentFeedbackWireTest.cpp
)
target_link_libraries(
    CodexA14AppsExternalAgentFeedbackWireTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodex
        snodec::core
)
target_compile_features(
    CodexA14AppsExternalAgentFeedbackWireTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA14AppsExternalAgentFeedbackWireTest
    PROPERTIES
        LABELS
            "component;ai;openai;codex;typed;apps;external-agent;feedback;operation;wire;lifecycle;a1-4;user-integrations"
        SKIP_RETURN_CODE 77
        TIMEOUT 20
)

snodec_add_test(
    CodexA14HooksMarketplaceSkillsCodecTest
    CodexA14HooksMarketplaceSkillsCodecTest.cpp
)
target_link_libraries(
    CodexA14HooksMarketplaceSkillsCodecTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodexBackend
)
target_compile_features(
    CodexA14HooksMarketplaceSkillsCodecTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA14HooksMarketplaceSkillsCodecTest
    PROPERTIES
        LABELS
            "component;ai;openai;codex;typed;hooks;marketplace;skills;backend;codec;a1-4;user-integrations"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(
    CodexA14HooksMarketplaceSkillsWireTest
    CodexA14HooksMarketplaceSkillsWireTest.cpp
)
target_link_libraries(
    CodexA14HooksMarketplaceSkillsWireTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodex
        snodec::core
)
target_compile_features(
    CodexA14HooksMarketplaceSkillsWireTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA14HooksMarketplaceSkillsWireTest
    PROPERTIES
        LABELS
            "component;ai;openai;codex;typed;hooks;marketplace;skills;operation;wire;lifecycle;a1-4;user-integrations"
        SKIP_RETURN_CODE 77
        TIMEOUT 20
)

snodec_add_test(
    CodexA14PluginOperationsCodecTest
    CodexA14PluginOperationsCodecTest.cpp
)
target_link_libraries(
    CodexA14PluginOperationsCodecTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodex
)
target_compile_features(
    CodexA14PluginOperationsCodecTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA14PluginOperationsCodecTest
    PROPERTIES
        LABELS
            "component;ai;openai;codex;typed;plugins;codec;a1-4;user-integrations"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(
    CodexA14PluginOperationsWireTest
    CodexA14PluginOperationsWireTest.cpp
)
target_link_libraries(
    CodexA14PluginOperationsWireTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodex
        snodec::core
)
target_compile_features(
    CodexA14PluginOperationsWireTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA14PluginOperationsWireTest
    PROPERTIES
        LABELS
            "component;ai;openai;codex;typed;plugins;operation;wire;lifecycle;a1-4;user-integrations"
        SKIP_RETURN_CODE 77
        TIMEOUT 20
)

snodec_add_test(
    CodexA14PluginCatalogCodecTest
    CodexA14PluginCatalogCodecTest.cpp
)
target_link_libraries(
    CodexA14PluginCatalogCodecTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodex
)
target_compile_features(
    CodexA14PluginCatalogCodecTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA14PluginCatalogCodecTest
    PROPERTIES
        LABELS
            "component;ai;openai;codex;typed;plugins;catalog;union;codec;a1-4;user-integrations"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(
    CodexA14PluginCatalogWireTest
    CodexA14PluginCatalogWireTest.cpp
)
target_link_libraries(
    CodexA14PluginCatalogWireTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodex
        snodec::core
)
target_compile_features(
    CodexA14PluginCatalogWireTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA14PluginCatalogWireTest
    PROPERTIES
        LABELS
            "component;ai;openai;codex;typed;plugins;catalog;union;operation;wire;lifecycle;a1-4;user-integrations"
        SKIP_RETURN_CODE 77
        TIMEOUT 20
)

snodec_add_test(
    CodexA14McpClientTest
    CodexA14McpClientTest.cpp
)
target_link_libraries(
    CodexA14McpClientTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodex
)
target_compile_features(CodexA14McpClientTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexA14McpClientTest
    PROPERTIES
        LABELS
            "component;ai;openai;codex;typed;mcp;codec;facade;a1-4;mcp-reverse"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(
    CodexA14WindowsSandboxClientTest
    CodexA14WindowsSandboxClientTest.cpp
)
target_link_libraries(
    CodexA14WindowsSandboxClientTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodex
)
target_compile_features(
    CodexA14WindowsSandboxClientTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA14WindowsSandboxClientTest
    PROPERTIES
        LABELS
            "component;ai;openai;codex;typed;windows-sandbox;codec;facade;a1-4;runtime-platform"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(
    CodexA14WindowsSandboxWireTest
    CodexA14WindowsSandboxWireTest.cpp
)
target_link_libraries(
    CodexA14WindowsSandboxWireTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodex
        snodec::core
)
target_compile_features(
    CodexA14WindowsSandboxWireTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA14WindowsSandboxWireTest
    PROPERTIES
        LABELS
            "component;ai;openai;codex;typed;windows-sandbox;operation;wire;lifecycle;a1-4;runtime-platform"
        SKIP_RETURN_CODE 77
        TIMEOUT 15
)

snodec_add_test(
    CodexA14McpNotificationEventTest
    CodexA14McpNotificationEventTest.cpp
)
target_link_libraries(
    CodexA14McpNotificationEventTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodex
)
target_compile_features(
    CodexA14McpNotificationEventTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA14McpNotificationEventTest
    PROPERTIES
        LABELS
            "component;ai;openai;codex;typed;mcp;notifications;codec;a1-4;mcp-reverse"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(
    CodexA14AttestationDynamicToolCodecTest
    CodexA14AttestationDynamicToolCodecTest.cpp
)
target_link_libraries(
    CodexA14AttestationDynamicToolCodecTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodex
)
target_compile_features(
    CodexA14AttestationDynamicToolCodecTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA14AttestationDynamicToolCodecTest
    PROPERTIES
        LABELS
            "component;ai;openai;codex;typed;server-request;codec;a1-4;mcp-reverse"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(
    CodexA14AttestationDynamicToolWireTest
    CodexA14AttestationDynamicToolWireTest.cpp
)
target_link_libraries(
    CodexA14AttestationDynamicToolWireTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodex
        snodec::core
)
target_compile_features(
    CodexA14AttestationDynamicToolWireTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA14AttestationDynamicToolWireTest
    PROPERTIES
        LABELS
            "component;ai;openai;codex;typed;server-request;wire;lifecycle;a1-4;mcp-reverse"
        SKIP_RETURN_CODE 77
        TIMEOUT 15
)

snodec_add_test(
    CodexA14McpClientWireTest
    CodexA14McpClientWireTest.cpp
)
target_link_libraries(
    CodexA14McpClientWireTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodex
        snodec::core
)
target_compile_features(CodexA14McpClientWireTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexA14McpClientWireTest
    PROPERTIES
        LABELS
            "component;ai;openai;codex;typed;mcp;operation;wire;lifecycle;a1-4;mcp-reverse"
        SKIP_RETURN_CODE 77
        TIMEOUT 15
)

snodec_add_test(
    CodexA14NineRequestStdioTest
    CodexA14NineRequestStdioTest.cpp
)
target_link_libraries(
    CodexA14NineRequestStdioTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodex
        snodec::core
)
target_compile_features(CodexA14NineRequestStdioTest PRIVATE cxx_std_20)
target_compile_definitions(
    CodexA14NineRequestStdioTest
    PRIVATE CODEX_FAKE_APP_SERVER="$<TARGET_FILE:CodexFakeAppServer>"
)
add_dependencies(CodexA14NineRequestStdioTest CodexFakeAppServer)
set_tests_properties(
    CodexA14NineRequestStdioTest
    PROPERTIES
        LABELS
            "component;ai;openai;codex;typed;server-request;lifecycle;wire;stdio;a1-4;mcp-reverse"
        SKIP_RETURN_CODE 77
        TIMEOUT 20
)

snodec_add_test(
    CodexA14ServerRequestResolvedLifecycleTest
    CodexA14ServerRequestResolvedLifecycleTest.cpp
)
target_link_libraries(
    CodexA14ServerRequestResolvedLifecycleTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodex
        snodec::core
)
target_compile_features(
    CodexA14ServerRequestResolvedLifecycleTest PRIVATE cxx_std_20
)
target_compile_definitions(
    CodexA14ServerRequestResolvedLifecycleTest
    PRIVATE CODEX_FAKE_APP_SERVER="$<TARGET_FILE:CodexFakeAppServer>"
)
add_dependencies(CodexA14ServerRequestResolvedLifecycleTest CodexFakeAppServer)
set_tests_properties(
    CodexA14ServerRequestResolvedLifecycleTest
    PROPERTIES
        LABELS
            "component;ai;openai;codex;typed;server-request;notification;lifecycle;wire;stdio;a1-4;runtime-platform"
        SKIP_RETURN_CODE 77
        TIMEOUT 20
)

snodec_add_test(
    CodexA14UserInputElicitationCodecTest
    CodexA14UserInputElicitationCodecTest.cpp
)
target_link_libraries(
    CodexA14UserInputElicitationCodecTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodex
)
target_compile_features(
    CodexA14UserInputElicitationCodecTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA14UserInputElicitationCodecTest
    PROPERTIES
        LABELS
            "component;ai;openai;codex;typed;server-request;codec;a1-4;mcp-reverse"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(CodexA12ModelWireTest CodexA12ModelWireTest.cpp)
target_link_libraries(
    CodexA12ModelWireTest
    PRIVATE snodec-test-support AISuite::OpenAICodex snodec::core
)
target_compile_features(CodexA12ModelWireTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexA12ModelWireTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;model;operation;wire;a1-2;b3"
        SKIP_RETURN_CODE 77
        TIMEOUT 20
)

snodec_add_test(
    CodexA12ConfigurationReadCodecTest
    CodexA12ConfigurationReadCodecTest.cpp
)
target_link_libraries(
    CodexA12ConfigurationReadCodecTest
    PRIVATE snodec-test-support AISuite::OpenAICodex
)
target_compile_features(
    CodexA12ConfigurationReadCodecTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA12ConfigurationReadCodecTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;configuration;codec;a1-2;b4"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(
    CodexA12ConfigurationReadWireTest
    CodexA12ConfigurationReadWireTest.cpp
)
target_link_libraries(
    CodexA12ConfigurationReadWireTest
    PRIVATE snodec-test-support AISuite::OpenAICodex snodec::core
)
target_compile_features(
    CodexA12ConfigurationReadWireTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA12ConfigurationReadWireTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;configuration;operation;wire;a1-2;b4"
        SKIP_RETURN_CODE 77
        TIMEOUT 20
)

snodec_add_test(
    CodexA12ConfigurationMutationCodecTest
    CodexA12ConfigurationMutationCodecTest.cpp
)
target_link_libraries(
    CodexA12ConfigurationMutationCodecTest
    PRIVATE snodec-test-support AISuite::OpenAICodex
)
target_compile_features(
    CodexA12ConfigurationMutationCodecTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA12ConfigurationMutationCodecTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;configuration;feature;codec;a1-2;b5"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(
    CodexA12ConfigurationMutationWireTest
    CodexA12ConfigurationMutationWireTest.cpp
)
target_link_libraries(
    CodexA12ConfigurationMutationWireTest
    PRIVATE snodec-test-support AISuite::OpenAICodex snodec::core
)
target_compile_features(
    CodexA12ConfigurationMutationWireTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA12ConfigurationMutationWireTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;configuration;feature;operation;wire;a1-2;b5"
        SKIP_RETURN_CODE 77
        TIMEOUT 20
)

snodec_add_test(
    CodexA13CommandCodecTest
    CodexA13CommandCodecTest.cpp
)
target_link_libraries(
    CodexA13CommandCodecTest
    PRIVATE snodec-test-support AISuite::OpenAICodex
)
target_compile_features(CodexA13CommandCodecTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexA13CommandCodecTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;command;codec;a1-3;b2"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(
    CodexA13CommandWireTest
    CodexA13CommandWireTest.cpp
)
target_link_libraries(
    CodexA13CommandWireTest
    PRIVATE snodec-test-support AISuite::OpenAICodex snodec::core
)
target_compile_features(CodexA13CommandWireTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexA13CommandWireTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;command;operation;wire;a1-3;b2"
        SKIP_RETURN_CODE 77
        TIMEOUT 20
)

snodec_add_test(
    CodexA13CommandBackendCompatibilityTest
    CodexA13CommandBackendCompatibilityTest.cpp
)
target_link_libraries(
    CodexA13CommandBackendCompatibilityTest
    PRIVATE snodec-test-support AISuite::OpenAICodexBackend
)
target_compile_features(
    CodexA13CommandBackendCompatibilityTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA13CommandBackendCompatibilityTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;command;backend;compatibility;a1-3;b2"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(
    CodexA13FilesystemCodecTest
    CodexA13FilesystemCodecTest.cpp
)
target_link_libraries(
    CodexA13FilesystemCodecTest
    PRIVATE snodec-test-support AISuite::OpenAICodex
)
target_compile_features(CodexA13FilesystemCodecTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexA13FilesystemCodecTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;filesystem;fuzzy;codec;a1-3;b3"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(
    CodexA13FilesystemWireTest
    CodexA13FilesystemWireTest.cpp
)
target_link_libraries(
    CodexA13FilesystemWireTest
    PRIVATE snodec-test-support AISuite::OpenAICodex snodec::core
)
target_compile_features(CodexA13FilesystemWireTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexA13FilesystemWireTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;filesystem;fuzzy;operation;wire;a1-3;b3"
        SKIP_RETURN_CODE 77
        TIMEOUT 20
)

snodec_add_test(
    CodexA13FilesystemBackendCompatibilityTest
    CodexA13FilesystemBackendCompatibilityTest.cpp
)
target_link_libraries(
    CodexA13FilesystemBackendCompatibilityTest
    PRIVATE snodec-test-support AISuite::OpenAICodexBackend
)
target_compile_features(
    CodexA13FilesystemBackendCompatibilityTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA13FilesystemBackendCompatibilityTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;filesystem;fuzzy;backend;compatibility;a1-3;b3"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(
    CodexA13ApprovalCodecTest
    CodexA13ApprovalCodecTest.cpp
)
target_link_libraries(
    CodexA13ApprovalCodecTest
    PRIVATE snodec-test-support AISuite::OpenAICodex
)
target_compile_features(CodexA13ApprovalCodecTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexA13ApprovalCodecTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;approval;permission;codec;a1-3;b4"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(
    CodexA13ApprovalWireTest
    CodexA13ApprovalWireTest.cpp
)
target_link_libraries(
    CodexA13ApprovalWireTest
    PRIVATE snodec-test-support AISuite::OpenAICodex snodec::core
)
target_compile_features(CodexA13ApprovalWireTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexA13ApprovalWireTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;approval;permission;server-request;wire;a1-3;b4"
        SKIP_RETURN_CODE 77
        TIMEOUT 30
)

snodec_add_test(
    CodexA13ApprovalBackendCompatibilityTest
    CodexA13ApprovalBackendCompatibilityTest.cpp
)
target_link_libraries(
    CodexA13ApprovalBackendCompatibilityTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodex
        AISuite::OpenAICodexFrontend
)
target_compile_features(
    CodexA13ApprovalBackendCompatibilityTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA13ApprovalBackendCompatibilityTest
    PROPERTIES
        LABELS
            "component;ai;openai;codex;typed;approval;permission;backend;frontend;compatibility;a1-3;b4"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(
    CodexA13ReviewCodecTest
    CodexA13ReviewCodecTest.cpp
)
target_link_libraries(
    CodexA13ReviewCodecTest
    PRIVATE snodec-test-support AISuite::OpenAICodex
)
target_compile_features(CodexA13ReviewCodecTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexA13ReviewCodecTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;review;guardian;codec;a1-3;b5"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(
    CodexA13ReviewWireTest
    CodexA13ReviewWireTest.cpp
)
target_link_libraries(
    CodexA13ReviewWireTest
    PRIVATE snodec-test-support AISuite::OpenAICodex snodec::core
)
target_compile_features(CodexA13ReviewWireTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexA13ReviewWireTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;review;guardian;operation;event;wire;a1-3;b5"
        SKIP_RETURN_CODE 77
        TIMEOUT 20
)

snodec_add_test(
    CodexA13ReviewBackendCompatibilityTest
    CodexA13ReviewBackendCompatibilityTest.cpp
)
target_link_libraries(
    CodexA13ReviewBackendCompatibilityTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodex
        AISuite::OpenAICodexFrontend
)
target_compile_features(
    CodexA13ReviewBackendCompatibilityTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA13ReviewBackendCompatibilityTest
    PROPERTIES
        LABELS
            "component;ai;openai;codex;typed;review;guardian;backend;frontend;compatibility;a1-3;b5"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(
    CodexA12AuthRefreshWireTest
    CodexA12AuthRefreshWireTest.cpp
)
target_link_libraries(
    CodexA12AuthRefreshWireTest
    PRIVATE snodec-test-support AISuite::OpenAICodex snodec::core
)
target_compile_features(CodexA12AuthRefreshWireTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexA12AuthRefreshWireTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;account;server-request;wire;a1-2;b2"
        SKIP_RETURN_CODE 77
        TIMEOUT 20
)

snodec_add_test(
    CodexA11OperationResultCorpusTest
    CodexA11OperationResultCorpusTest.cpp
)
target_link_libraries(
    CodexA11OperationResultCorpusTest
    PRIVATE snodec-test-support AISuite::OpenAICodex
)
target_compile_features(
    CodexA11OperationResultCorpusTest PRIVATE cxx_std_20
)
target_compile_definitions(
    CodexA11OperationResultCorpusTest
    PRIVATE
        CODEX_A1_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tools/codex/app-server-fixtures/0.144.6"
        CODEX_A1_OPERATION_PRODUCTION_COVERAGE="${CODEX_PHASE_A1_1_EVIDENCE_ROOT}/a1-1-operation-production-coverage.json"
)
set_tests_properties(
    CodexA11OperationResultCorpusTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;operation;codec;corpus;a1-1"
        SKIP_RETURN_CODE 77
        TIMEOUT 30
)

snodec_add_test(
    CodexA11OperationAggregateValueCorpusTest
    CodexA11OperationAggregateValueCorpusTest.cpp
)
target_link_libraries(
    CodexA11OperationAggregateValueCorpusTest
    PRIVATE snodec-test-support AISuite::OpenAICodex
)
target_compile_features(
    CodexA11OperationAggregateValueCorpusTest PRIVATE cxx_std_20
)
target_compile_definitions(
    CodexA11OperationAggregateValueCorpusTest
    PRIVATE
        CODEX_A1_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tools/codex/app-server-fixtures/0.144.6"
        CODEX_A1_OPERATION_PRODUCTION_COVERAGE="${CODEX_PHASE_A1_1_EVIDENCE_ROOT}/a1-1-operation-production-coverage.json"
)
set_tests_properties(
    CodexA11OperationAggregateValueCorpusTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;operation;aggregate;value;corpus;a1-1"
        SKIP_RETURN_CODE 77
        TIMEOUT 30
)

snodec_add_test(
    CodexA11NotificationCodecTest
    CodexA11NotificationCodecTest.cpp
)
target_link_libraries(
    CodexA11NotificationCodecTest
    PRIVATE snodec-test-support AISuite::OpenAICodex
)
target_compile_features(
    CodexA11NotificationCodecTest PRIVATE cxx_std_20
)
target_compile_definitions(
    CodexA11NotificationCodecTest
    PRIVATE
        CODEX_A1_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tools/codex/app-server-fixtures/0.144.6"
        CODEX_A1_NOTIFICATION_PRODUCTION_COVERAGE="${CODEX_PHASE_A1_1_EVIDENCE_ROOT}/a1-1-notification-production-coverage.json"
)
set_tests_properties(
    CodexA11NotificationCodecTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;notification;codec;corpus;a1-1"
        SKIP_RETURN_CODE 77
        TIMEOUT 30
)

foreach(
    typed_test IN
    ITEMS CodexTypedThreadCodecTest CodexTypedTurnCodecTest
          CodexTypedItemDecoderTest CodexTypedEventDecoderTest
          CodexTypedServerRequestDecoderTest CodexErrorInfoCodecTest
          CodexConversationCodecTest CodexConversationB3NestedCodecTest
          CodexConversationB4NestedCodecTest CodexA11ItemCodecTest
)
    snodec_add_test(${typed_test} ${typed_test}.cpp)
    target_link_libraries(
        ${typed_test} PRIVATE snodec-test-support AISuite::OpenAICodex
    )
    target_compile_features(${typed_test} PRIVATE cxx_std_20)
    set_tests_properties(
        ${typed_test} PROPERTIES LABELS "component;ai;openai;codex;typed;codec"
                                 SKIP_RETURN_CODE 77 TIMEOUT 5
    )
endforeach()

target_compile_definitions(
    CodexErrorInfoCodecTest
    PRIVATE CODEX_A1_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tools/codex/app-server-fixtures/0.144.6"
)
target_compile_definitions(
    CodexConversationCodecTest
    PRIVATE CODEX_A1_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tools/codex/app-server-fixtures/0.144.6"
)
set_tests_properties(
    CodexConversationCodecTest
    PROPERTIES TIMEOUT 30
)
target_compile_definitions(
    CodexConversationB3NestedCodecTest
    PRIVATE CODEX_A1_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tools/codex/app-server-fixtures/0.144.6"
)
target_compile_definitions(
    CodexConversationB4NestedCodecTest
    PRIVATE
        CODEX_A1_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tools/codex/app-server-fixtures/0.144.6"
        CODEX_A1_OPERATION_PRODUCTION_COVERAGE="${CODEX_PHASE_A1_1_EVIDENCE_ROOT}/a1-1-operation-production-coverage.json"
)
set_tests_properties(
    CodexConversationB4NestedCodecTest
    PROPERTIES TIMEOUT 30
)
target_compile_definitions(
    CodexA11ItemCodecTest
    PRIVATE CODEX_A1_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tools/codex/app-server-fixtures/0.144.6"
)
set_tests_properties(
    CodexA11ItemCodecTest
    PROPERTIES TIMEOUT 30
)

snodec_add_test(CodexBackendReducerTest CodexBackendReducerTest.cpp)
target_link_libraries(
    CodexBackendReducerTest PRIVATE snodec-test-support
                                    AISuite::OpenAICodexBackend
)
target_compile_features(CodexBackendReducerTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexBackendReducerTest
    PROPERTIES LABELS "component;ai;openai;codex;backend;reducer"
               SKIP_RETURN_CODE 77 TIMEOUT 10
)

snodec_add_test(
    CodexThreadItemBackendParityTest
    CodexThreadItemBackendParityTest.cpp
)
target_link_libraries(
    CodexThreadItemBackendParityTest
    PRIVATE snodec-test-support AISuite::OpenAICodexBackend
)
target_compile_features(
    CodexThreadItemBackendParityTest PRIVATE cxx_std_20
)
target_compile_definitions(
    CodexThreadItemBackendParityTest
    PRIVATE CODEX_A1_FIXTURE_ROOT="${PROJECT_SOURCE_DIR}/tools/codex/app-server-fixtures/0.144.6"
)
set_tests_properties(
    CodexThreadItemBackendParityTest
    PROPERTIES LABELS "component;ai;openai;codex;typed;backend;reducer;parity"
               SKIP_RETURN_CODE 77 TIMEOUT 10
)

snodec_add_test(
    CodexA11NotificationBackendPreservationTest
    CodexA11NotificationBackendPreservationTest.cpp
)
target_link_libraries(
    CodexA11NotificationBackendPreservationTest
    PRIVATE snodec-test-support AISuite::OpenAICodexBackend
)
target_compile_features(
    CodexA11NotificationBackendPreservationTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA11NotificationBackendPreservationTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;backend;reducer;preservation;a1-1"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(
    CodexA12AccountBackendPreservationTest
    CodexA12AccountBackendPreservationTest.cpp
)
target_link_libraries(
    CodexA12AccountBackendPreservationTest
    PRIVATE snodec-test-support AISuite::OpenAICodexFrontend snodec::core
)
target_compile_features(
    CodexA12AccountBackendPreservationTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA12AccountBackendPreservationTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;account;backend;reducer;preservation;a1-2;b2"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(
    CodexA12ModelBackendCompatibilityTest
    CodexA12ModelBackendCompatibilityTest.cpp
)
target_link_libraries(
    CodexA12ModelBackendCompatibilityTest
    PRIVATE snodec-test-support AISuite::OpenAICodexFrontend snodec::core
)
target_compile_features(
    CodexA12ModelBackendCompatibilityTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA12ModelBackendCompatibilityTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;model;backend;reducer;compatibility;preservation;a1-2;b3"
        SKIP_RETURN_CODE 77
        TIMEOUT 15
)

snodec_add_test(
    CodexA12ConfigurationReadBackendPreservationTest
    CodexA12ConfigurationReadBackendPreservationTest.cpp
)
target_link_libraries(
    CodexA12ConfigurationReadBackendPreservationTest
    PRIVATE snodec-test-support AISuite::OpenAICodexBackend
)
target_compile_features(
    CodexA12ConfigurationReadBackendPreservationTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexA12ConfigurationReadBackendPreservationTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;configuration;backend;reducer;preservation;a1-2;b4"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(CodexFrontendProtocolV1Test CodexFrontendProtocolV1Test.cpp)
target_link_libraries(
    CodexFrontendProtocolV1Test PRIVATE snodec-test-support
                                        AISuite::OpenAICodexFrontend
)
target_compile_features(CodexFrontendProtocolV1Test PRIVATE cxx_std_20)
set_tests_properties(
    CodexFrontendProtocolV1Test
    PROPERTIES LABELS
               "component;ai;openai;codex;frontend;protocol;journal;batch"
               SKIP_RETURN_CODE 77 TIMEOUT 30
)

snodec_add_test(
    CodexFrontendProtocolTargetIsolationTest
    CodexFrontendProtocolTargetIsolationTest.cpp
)
target_link_libraries(
    CodexFrontendProtocolTargetIsolationTest
    PRIVATE AISuite::OpenAICodexFrontendProtocol
)
target_compile_features(
    CodexFrontendProtocolTargetIsolationTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexFrontendProtocolTargetIsolationTest
    PROPERTIES
        LABELS "component;ai;openai;codex;frontend;protocol;target;isolation;p2"
        TIMEOUT 10
)

add_test(
    NAME CodexFrontendProtocolMinimalConfigurationTest
    COMMAND
        ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/CodexFrontendProtocolMinimalConfigurationTest.py
        --source-dir ${PROJECT_SOURCE_DIR}
        --build-dir ${PROJECT_BINARY_DIR}
        --snodec-dir ${snodec_DIR}
)
set_tests_properties(
    CodexFrontendProtocolMinimalConfigurationTest
    PROPERTIES
        LABELS "component;ai;openai;codex;frontend;protocol;minimal;p2"
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        RUN_SERIAL TRUE
        TIMEOUT 900
)

snodec_add_test(
    CodexFrontendContractMetadataTest
    ${PROJECT_SOURCE_DIR}/tests/ai/openai/codex/CodexFrontendContractMetadataTest.cpp
)
target_link_libraries(
    CodexFrontendContractMetadataTest PRIVATE snodec-test-support
                                              AISuite::OpenAICodexFrontend
)
target_compile_features(CodexFrontendContractMetadataTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexFrontendContractMetadataTest
    PROPERTIES LABELS
               "component;ai;openai;codex;frontend;protocol;security;a1-7a;a1-7b"
               TIMEOUT 10
)

snodec_add_test(
    CodexFrontendBackendMappingTest
    ${PROJECT_SOURCE_DIR}/tests/ai/openai/codex/CodexFrontendBackendMappingTest.cpp
)
target_link_libraries(
    CodexFrontendBackendMappingTest
    PRIVATE snodec-test-support AISuite::OpenAICodexBackend
)
target_compile_features(CodexFrontendBackendMappingTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexFrontendBackendMappingTest
    PROPERTIES LABELS
               "component;ai;openai;codex;frontend;protocol;backend-mapping;a1-7a"
               TIMEOUT 10
)

snodec_add_test(
    CodexFrontendRuntimeBackendMappingTest
    CodexFrontendRuntimeBackendMappingTest.cpp
)
target_link_libraries(
    CodexFrontendRuntimeBackendMappingTest
    PRIVATE snodec-test-support AISuite::OpenAICodexFrontend snodec::core
)
target_compile_features(
    CodexFrontendRuntimeBackendMappingTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexFrontendRuntimeBackendMappingTest
    PROPERTIES LABELS
               "component;ai;openai;codex;frontend;runtime;backend-mapping;a1-7b"
               TIMEOUT 10
)

snodec_add_test(
    CodexFrontendCompleteCodecTest CodexFrontendCompleteCodecTest.cpp
)
target_link_libraries(
    CodexFrontendCompleteCodecTest PRIVATE snodec-test-support
                                           AISuite::OpenAICodexFrontend
)
target_compile_features(CodexFrontendCompleteCodecTest PRIVATE cxx_std_20)
target_compile_definitions(
    CodexFrontendCompleteCodecTest
    PRIVATE
        CODEX_FRONTEND_GOLDEN_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/frontend-protocol-v1.generated.json"
)
set_tests_properties(
    CodexFrontendCompleteCodecTest
    PROPERTIES LABELS "component;ai;openai;codex;frontend;protocol;codec;a1-7a"
               TIMEOUT 10
)

snodec_add_test(
    CodexFrontendProviderResultProjectionTest
    CodexFrontendProviderResultProjectionTest.cpp
)
target_link_libraries(
    CodexFrontendProviderResultProjectionTest
    PRIVATE snodec-test-support AISuite::OpenAICodexFrontend
)
target_compile_features(
    CodexFrontendProviderResultProjectionTest PRIVATE cxx_std_20
)
target_compile_definitions(
    CodexFrontendProviderResultProjectionTest
    PRIVATE
        CODEX_FRONTEND_GOLDEN_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/frontend-protocol-v1.generated.json"
)
set_tests_properties(
    CodexFrontendProviderResultProjectionTest
    PROPERTIES LABELS
               "component;ai;openai;codex;frontend;service;provider-results;a1-7b"
               TIMEOUT 15
)

snodec_add_test(
    CodexFrontendRuntimeActivationTest CodexFrontendRuntimeActivationTest.cpp
)
target_link_libraries(
    CodexFrontendRuntimeActivationTest
    PRIVATE snodec-test-support AISuite::OpenAICodexFrontend snodec::core
)
target_compile_features(CodexFrontendRuntimeActivationTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexFrontendRuntimeActivationTest
    PROPERTIES LABELS
               "component;ai;openai;codex;frontend;service;runtime;authorization;a1-7b"
               TIMEOUT 15
)

snodec_add_test(
    CodexFrontendRuntimeSecurityTest CodexFrontendRuntimeSecurityTest.cpp
)
target_link_libraries(
    CodexFrontendRuntimeSecurityTest
    PRIVATE snodec-test-support AISuite::OpenAICodexFrontend snodec::core
)
target_compile_features(CodexFrontendRuntimeSecurityTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexFrontendRuntimeSecurityTest
    PROPERTIES LABELS
               "component;ai;openai;codex;frontend;service;runtime;security;a1-7b"
               TIMEOUT 15
)

snodec_add_test(
    CodexFrontendSchemaValidatorTest CodexFrontendSchemaValidatorTest.cpp
)
target_link_libraries(
    CodexFrontendSchemaValidatorTest PRIVATE snodec-test-support
                                             AISuite::OpenAICodexFrontend
)
target_compile_features(CodexFrontendSchemaValidatorTest PRIVATE cxx_std_20)
target_compile_definitions(
    CodexFrontendSchemaValidatorTest
    PRIVATE
        CODEX_FRONTEND_GOLDEN_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/frontend-protocol-v1.generated.json"
)
set_tests_properties(
    CodexFrontendSchemaValidatorTest
    PROPERTIES LABELS
               "component;ai;openai;codex;frontend;protocol;codec;schema;validator;a1-7a"
               TIMEOUT 20
)

snodec_add_test(CodexFrontendServiceTest CodexFrontendServiceTest.cpp)
target_link_libraries(
    CodexFrontendServiceTest
    PRIVATE snodec-test-support AISuite::OpenAICodexFrontend
)
target_compile_features(CodexFrontendServiceTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexFrontendServiceTest
    PROPERTIES
        LABELS "component;ai;openai;codex;frontend;compatibility;utilities;p3"
        TIMEOUT 5
)

snodec_add_test(
    CodexFrontendPublicServiceAdapterTest
    CodexFrontendPublicServiceAdapterTest.cpp
)
target_link_libraries(
    CodexFrontendPublicServiceAdapterTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodexFrontend
        snodec::core
)
target_compile_features(CodexFrontendPublicServiceAdapterTest PRIVATE cxx_std_20)
target_compile_definitions(
    CodexFrontendPublicServiceAdapterTest
    PRIVATE
        AISUITE_CODEX_CPP_CLIENT_SDK_BUILT=$<BOOL:${AISUITE_BUILD_CODEX_FRONTEND_CLIENT}>
)
set_tests_properties(
    CodexFrontendPublicServiceAdapterTest
    PROPERTIES
        LABELS "component;ai;openai;codex;frontend;server;adapter;p3"
        TIMEOUT 20
)

foreach(
    typed_frontend_test
    IN ITEMS
       CodexFrontendTypedModelTest
       CodexFrontendTypedOccurrenceTest
       CodexFrontendTypedSnapshotTest
       CodexFrontendTypedJournalTest
       CodexFrontendProjectionSecurityTest
)
    snodec_add_test(${typed_frontend_test} ${typed_frontend_test}.cpp)
    target_link_libraries(
        ${typed_frontend_test}
        PRIVATE snodec-test-support ai-openai-codex-frontend-model
    )
    target_compile_features(${typed_frontend_test} PRIVATE cxx_std_20)
    set_tests_properties(
        ${typed_frontend_test}
        PROPERTIES
            LABELS "component;ai;openai;codex;frontend;typed;model;p2"
            TIMEOUT 30
    )
endforeach()

foreach(
    frontend_server_core_test
    IN ITEMS
       CodexFrontendServerCoreLifecycleTest
       CodexFrontendServerCoreSynchronizationTest
       CodexFrontendServerCoreCommandTest
       CodexFrontendServerCoreSecurityTest
       CodexFrontendServerCoreBackpressureTest
)
    snodec_add_test(
        ${frontend_server_core_test} ${frontend_server_core_test}.cpp
    )
    target_link_libraries(
        ${frontend_server_core_test}
        PRIVATE
            snodec-test-support
            ai-openai-codex-frontend-server-core
    )
    target_compile_features(${frontend_server_core_test} PRIVATE cxx_std_20)
    set_tests_properties(
        ${frontend_server_core_test}
        PROPERTIES
            LABELS "component;ai;openai;codex;frontend;server;core;p2"
            TIMEOUT 30
    )
endforeach()

foreach(
    frontend_client_core_test
    IN ITEMS
       CodexFrontendClientCoreLifecycleTest
       CodexFrontendClientCoreSynchronizationTest
       CodexFrontendClientCoreOperationTest
       CodexFrontendClientCoreStateTest
       CodexFrontendClientCoreCapacityTest
)
    snodec_add_test(
        ${frontend_client_core_test} ${frontend_client_core_test}.cpp
    )
    target_link_libraries(
        ${frontend_client_core_test}
        PRIVATE
            snodec-test-support
            ai-openai-codex-frontend-client-core
    )
    target_compile_features(${frontend_client_core_test} PRIVATE cxx_std_20)
    target_compile_definitions(
        ${frontend_client_core_test}
        PRIVATE
            CODEX_FRONTEND_GOLDEN_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/frontend-protocol-v1.generated.json"
    )
    set_tests_properties(
        ${frontend_client_core_test}
        PROPERTIES
            LABELS "component;ai;openai;codex;frontend;client;core;p2"
            TIMEOUT 30
    )
endforeach()

snodec_add_test(
    CodexFrontendCompatibilityAdaptersTest
    CodexFrontendCompatibilityAdaptersTest.cpp
)
target_link_libraries(
    CodexFrontendCompatibilityAdaptersTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodexFrontend
        ai-openai-codex-frontend-client-test-support
        ai-openai-codex-frontend-model
        ai-openai-codex-frontend-server-core
        ai-openai-codex-frontend-client-core
)
target_compile_features(CodexFrontendCompatibilityAdaptersTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexFrontendCompatibilityAdaptersTest
    PROPERTIES
        LABELS "component;ai;openai;codex;frontend;compatibility;adapters;p2"
        TIMEOUT 30
)

add_executable(
    CodexFrontendDifferentialMutationProbe
    CodexFrontendDifferentialMutationProbe.cpp
)
target_link_libraries(
    CodexFrontendDifferentialMutationProbe
    PRIVATE AISuite::OpenAICodexFrontendProtocol
)
target_compile_features(
    CodexFrontendDifferentialMutationProbe PRIVATE cxx_std_20
)

add_test(
    NAME CodexFrontendDifferentialCoverageGuardTest
    COMMAND
        ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/CodexFrontendDifferentialCoverageGuardTest.py
        --coverage ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/p2-frontend-differential-coverage.json
)
set_tests_properties(
    CodexFrontendDifferentialCoverageGuardTest
    PROPERTIES
        LABELS "component;ai;openai;codex;frontend;differential;coverage;p2"
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        TIMEOUT 30
)

add_test(
    NAME CodexFrontendDifferentialMutationTest
    COMMAND
        ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/CodexFrontendDifferentialMutationTest.py
        --probe $<TARGET_FILE:CodexFrontendDifferentialMutationProbe>
)
set_tests_properties(
    CodexFrontendDifferentialMutationTest
    PROPERTIES
        LABELS "component;ai;openai;codex;frontend;differential;mutation;p2"
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        TIMEOUT 30
)
snodec_add_test(
    CodexFrontendJsonLineFramerTest CodexFrontendJsonLineFramerTest.cpp
)
target_link_libraries(
    CodexFrontendJsonLineFramerTest
    PRIVATE snodec-test-support ai-openai-codex-frontend-jsonl-framer
)
target_include_directories(
    CodexFrontendJsonLineFramerTest PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_compile_features(CodexFrontendJsonLineFramerTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexFrontendJsonLineFramerTest
    PROPERTIES LABELS "component;ai;openai;codex;frontend;unix;framing"
               SKIP_RETURN_CODE 77 TIMEOUT 5
)

snodec_add_test(
    CodexFrontendDeliveryRetryBackoffTest CodexFrontendDeliveryRetryBackoffTest.cpp
)
target_include_directories(
    CodexFrontendDeliveryRetryBackoffTest PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(
    CodexFrontendDeliveryRetryBackoffTest PRIVATE snodec-test-support
)
target_compile_features(CodexFrontendDeliveryRetryBackoffTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexFrontendDeliveryRetryBackoffTest
    PROPERTIES LABELS "component;ai;openai;codex;frontend;transport;backpressure" TIMEOUT 5
)

snodec_add_test(
    CodexFrontendReferenceAuthenticationTest
    CodexFrontendReferenceAuthenticationTest.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/ReferenceAuthentication.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/UnixPeerCredentials.cpp
)

snodec_add_test(
    CodexFrontendNativeTransportTest
    CodexFrontendNativeTransportTest.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/FrontendStreamSocketContext.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/FrontendStreamSocketContextFactory.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/UnixPeerCredentials.cpp
)
target_include_directories(
    CodexFrontendNativeTransportTest PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(
    CodexFrontendNativeTransportTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodexBackend
        AISuite::OpenAICodexFrontend
        ai-openai-codex-frontend-jsonl-framer
        snodec::core
        snodec::net-un-stream-legacy
        snodec::net-in-stream-legacy
        snodec::net-in6-stream-legacy
)
if(AISUITE_ENABLE_CODEX_FRONTEND_TLS)
    target_compile_definitions(
        CodexFrontendNativeTransportTest
        PRIVATE
            AISUITE_CODEX_FRONTEND_TLS=1
            AISUITE_CODEX_TEST_TLS_CERT="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/a1-7b-loopback-cert.pem"
            AISUITE_CODEX_TEST_TLS_KEY="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/a1-7b-loopback-key.pem"
    )
    target_link_libraries(
        CodexFrontendNativeTransportTest
        PRIVATE snodec::net-in-stream-tls snodec::net-in6-stream-tls
    )
endif()
if(AISUITE_ENABLE_CODEX_FRONTEND_RFCOMM)
    target_compile_definitions(
        CodexFrontendNativeTransportTest PRIVATE AISUITE_TEST_CODEX_FRONTEND_RFCOMM=1
    )
    target_link_libraries(
        CodexFrontendNativeTransportTest
        PRIVATE snodec::net-rc-stream-legacy snodec::net-rc-stream-tls
    )
endif()
target_compile_features(CodexFrontendNativeTransportTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexFrontendNativeTransportTest
    PROPERTIES
        LABELS "component;ai;openai;codex;frontend;transport;stream;a1-7b"
        TIMEOUT 20
)

snodec_add_test(
    CodexFrontendNativeConfigurationTest
    CodexFrontendNativeConfigurationTest.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/Configuration.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/FrontendWebSecurity.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/ReferenceAuthentication.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/ReferenceInvocationPolicy.cpp
)
target_include_directories(
    CodexFrontendNativeConfigurationTest PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(
    CodexFrontendNativeConfigurationTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodexBackend
        AISuite::OpenAICodexFrontend
        snodec::core
)
target_compile_definitions(
    CodexFrontendNativeConfigurationTest
    PRIVATE
        CODEX_BACKEND_CONFIGURATION_SOURCE="${PROJECT_SOURCE_DIR}/src/apps/codex-backend/Configuration.cpp"
        CODEX_BACKEND_MAIN_SOURCE="${PROJECT_SOURCE_DIR}/src/apps/codex-backend/main.cpp"
        CODEX_BACKEND_STREAM_CONTEXT_SOURCE="${PROJECT_SOURCE_DIR}/src/apps/codex-backend/FrontendStreamSocketContext.cpp"
        CODEX_BACKEND_UNIX_CREDENTIAL_SOURCE="${PROJECT_SOURCE_DIR}/src/apps/codex-backend/UnixPeerCredentials.cpp"
        AISUITE_CODEX_TEST_TLS_CERT="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/a1-7b-loopback-cert.pem"
        AISUITE_CODEX_TEST_TLS_KEY="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/a1-7b-loopback-key.pem"
)
if(AISUITE_ENABLE_CODEX_FRONTEND_TLS)
    target_compile_definitions(
        CodexFrontendNativeConfigurationTest
        PRIVATE AISUITE_CODEX_FRONTEND_TLS=1
    )
endif()
if(AISUITE_ENABLE_CODEX_FRONTEND_RFCOMM)
    target_compile_definitions(
        CodexFrontendNativeConfigurationTest
        PRIVATE AISUITE_CODEX_FRONTEND_RFCOMM=1
    )
endif()
target_compile_features(
    CodexFrontendNativeConfigurationTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexFrontendNativeConfigurationTest
    PROPERTIES
        LABELS "component;ai;openai;codex;frontend;transport;configuration;security;a1-7b"
        TIMEOUT 5
)

snodec_add_test(
    CodexFrontendNativeConfigurationCliTest
    CodexFrontendNativeConfigurationCliTest.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/Configuration.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/FrontendWebSecurity.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/ReferenceAuthentication.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/ReferenceInvocationPolicy.cpp
)
target_include_directories(
    CodexFrontendNativeConfigurationCliTest PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(
    CodexFrontendNativeConfigurationCliTest
    PRIVATE
        snodec-test-support
        AISuite::OpenAICodexBackend
        AISuite::OpenAICodexFrontend
        snodec::core
)
target_compile_definitions(
    CodexFrontendNativeConfigurationCliTest
    PRIVATE
        AISUITE_CODEX_TEST_TLS_CERT="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/a1-7b-loopback-cert.pem"
        AISUITE_CODEX_TEST_TLS_KEY="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/a1-7b-loopback-key.pem"
)
if(AISUITE_ENABLE_CODEX_FRONTEND_TLS)
    target_compile_definitions(
        CodexFrontendNativeConfigurationCliTest
        PRIVATE AISUITE_CODEX_FRONTEND_TLS=1
    )
endif()
if(AISUITE_ENABLE_CODEX_FRONTEND_RFCOMM)
    target_compile_definitions(
        CodexFrontendNativeConfigurationCliTest
        PRIVATE AISUITE_CODEX_FRONTEND_RFCOMM=1
    )
endif()
target_compile_features(
    CodexFrontendNativeConfigurationCliTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexFrontendNativeConfigurationCliTest
    PROPERTIES
        LABELS "component;ai;openai;codex;frontend;transport;configuration;cli;security;a1-7b"
        TIMEOUT 5
)

snodec_add_test(
    CodexBackendReferenceInvocationPolicyTest
    CodexBackendReferenceInvocationPolicyTest.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/ReferenceInvocationPolicy.cpp
)
target_include_directories(
    CodexBackendReferenceInvocationPolicyTest PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(
    CodexBackendReferenceInvocationPolicyTest
    PRIVATE snodec-test-support AISuite::OpenAICodexFrontend
)
target_compile_features(
    CodexBackendReferenceInvocationPolicyTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexBackendReferenceInvocationPolicyTest
    PROPERTIES
        LABELS "component;ai;openai;codex;frontend;configuration;security;policy;a1-7b"
        TIMEOUT 5
)

snodec_add_test(
    CodexBackendUnixCliCompatibilityTest
    CodexBackendUnixCliCompatibilityTest.cpp
)
target_link_libraries(
    CodexBackendUnixCliCompatibilityTest
    PRIVATE
        snodec-test-support
        snodec::core
        snodec::net-un-stream-legacy
        snodec::net-in-stream-legacy
        snodec::net-in6-stream-legacy
)
if(AISUITE_ENABLE_CODEX_FRONTEND_TLS)
    target_compile_definitions(
        CodexBackendUnixCliCompatibilityTest
        PRIVATE AISUITE_CODEX_FRONTEND_TLS=1
    )
    target_link_libraries(
        CodexBackendUnixCliCompatibilityTest
        PRIVATE snodec::net-in-stream-tls snodec::net-in6-stream-tls
    )
endif()
if(AISUITE_ENABLE_CODEX_FRONTEND_RFCOMM)
    target_compile_definitions(
        CodexBackendUnixCliCompatibilityTest
        PRIVATE AISUITE_CODEX_FRONTEND_RFCOMM=1
    )
    target_link_libraries(
        CodexBackendUnixCliCompatibilityTest
        PRIVATE snodec::net-rc-stream-legacy snodec::net-rc-stream-tls
    )
endif()
target_compile_features(
    CodexBackendUnixCliCompatibilityTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexBackendUnixCliCompatibilityTest
    PROPERTIES
        LABELS "component;ai;openai;codex;frontend;unix;configuration;compatibility;a1-7b"
        TIMEOUT 5
)
target_link_libraries(
    CodexFrontendReferenceAuthenticationTest
    PRIVATE snodec-test-support AISuite::OpenAICodexFrontend
            snodec::net-un-stream-legacy
)
target_include_directories(
    CodexFrontendReferenceAuthenticationTest PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_compile_features(
    CodexFrontendReferenceAuthenticationTest PRIVATE cxx_std_20
)
set_tests_properties(
    CodexFrontendReferenceAuthenticationTest
    PROPERTIES LABELS
               "component;ai;openai;codex;frontend;authentication;unix;security;a1-7b"
               TIMEOUT 5
)

if(TARGET codex-backend-client-support)
    snodec_add_test(
        CodexBackendClientTransportSelectionTest
        CodexBackendClientTransportSelectionTest.cpp
    )
    target_link_libraries(
        CodexBackendClientTransportSelectionTest
        PRIVATE snodec-test-support codex-backend-client-support
    )
    target_compile_features(
        CodexBackendClientTransportSelectionTest PRIVATE cxx_std_20
    )
    set_tests_properties(
        CodexBackendClientTransportSelectionTest
        PROPERTIES
            LABELS
            "component;ai;openai;codex;frontend;client;transport;configuration;a1-7c-1"
            TIMEOUT 5
    )

    snodec_add_test(
        CodexBackendClientAuthenticationTest
        CodexBackendClientAuthenticationTest.cpp
    )
    target_link_libraries(
        CodexBackendClientAuthenticationTest PRIVATE snodec-test-support
                                                     codex-backend-client-support
    )
    target_compile_features(
        CodexBackendClientAuthenticationTest PRIVATE cxx_std_20
    )
    target_compile_definitions(
        CodexBackendClientAuthenticationTest
        PRIVATE
            CODEX_BACKEND_CLIENT_MAIN_SOURCE="${PROJECT_SOURCE_DIR}/src/apps/codex-backend-client/main.cpp"
            CODEX_BACKEND_CLIENT_CONNECTION_SOURCE="${PROJECT_SOURCE_DIR}/src/apps/codex-backend-client/ClientConnection.cpp"
            CODEX_BACKEND_CLIENT_WEBSOCKET_SOURCE="${PROJECT_SOURCE_DIR}/src/apps/codex-backend-client/FrontendWebSocketClient.cpp"
    )
    set_tests_properties(
        CodexBackendClientAuthenticationTest
        PROPERTIES
            LABELS
            "component;ai;openai;codex;frontend;client;transport;authentication;security;a1-7c-1"
            TIMEOUT 5
    )

    if(AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET)
        snodec_add_test(
            CodexBackendClientTransportCompositionTest
            CodexBackendClientTransportCompositionTest.cpp
        )
        target_link_libraries(
            CodexBackendClientTransportCompositionTest
            PRIVATE snodec-test-support codex-backend-client-support
                    snodec::websocket-client
        )
        target_compile_features(
            CodexBackendClientTransportCompositionTest PRIVATE cxx_std_20
        )
        target_compile_definitions(
            CodexBackendClientTransportCompositionTest
            PRIVATE
                CODEX_BACKEND_CLIENT_MAIN_SOURCE="${PROJECT_SOURCE_DIR}/src/apps/codex-backend-client/main.cpp"
                CODEX_BACKEND_CLIENT_WEBSOCKET_SOURCE="${PROJECT_SOURCE_DIR}/src/apps/codex-backend-client/FrontendWebSocketClient.cpp"
        )
        set_tests_properties(
            CodexBackendClientTransportCompositionTest
            PROPERTIES
                LABELS
                "component;ai;openai;codex;frontend;client;transport;websocket;configuration;a1-7c-1"
                TIMEOUT 5
        )
    endif()

    snodec_add_test(
        CodexBackendClientCommandTest CodexBackendClientCommandTest.cpp
    )
    target_link_libraries(
        CodexBackendClientCommandTest PRIVATE snodec-test-support
                                              codex-backend-client-support
    )
    target_compile_features(CodexBackendClientCommandTest PRIVATE cxx_std_20)
    set_tests_properties(
        CodexBackendClientCommandTest
        PROPERTIES LABELS
                   "component;ai;openai;codex;frontend;client;command;codec"
                   SKIP_RETURN_CODE 77 TIMEOUT 5
    )

    snodec_add_test(
        CodexBackendClientCommandDrainTest
        CodexBackendClientCommandDrainTest.cpp
    )
    target_link_libraries(
        CodexBackendClientCommandDrainTest PRIVATE snodec-test-support
                                                   codex-backend-client-support
    )
    target_compile_features(
        CodexBackendClientCommandDrainTest PRIVATE cxx_std_20
    )
    set_tests_properties(
        CodexBackendClientCommandDrainTest
        PROPERTIES LABELS
                   "component;ai;openai;codex;frontend;client;command;lifecycle"
                   SKIP_RETURN_CODE 77 TIMEOUT 5
    )

    snodec_add_test(
        CodexBackendClientPresenterTest CodexBackendClientPresenterTest.cpp
    )
    target_link_libraries(
        CodexBackendClientPresenterTest PRIVATE snodec-test-support
                                                codex-backend-client-support
    )
    target_compile_features(CodexBackendClientPresenterTest PRIVATE cxx_std_20)
    set_tests_properties(
        CodexBackendClientPresenterTest
        PROPERTIES LABELS
                   "component;ai;openai;codex;frontend;client;presentation"
                   SKIP_RETURN_CODE 77 TIMEOUT 5
    )

    snodec_add_test(
        CodexBackendClientJsonLineFramerTest
        CodexBackendClientJsonLineFramerTest.cpp
    )
    target_link_libraries(
        CodexBackendClientJsonLineFramerTest
        PRIVATE snodec-test-support codex-backend-client-support
                ai-openai-codex-frontend-jsonl-framer
    )
    target_compile_features(
        CodexBackendClientJsonLineFramerTest PRIVATE cxx_std_20
    )
    set_tests_properties(
        CodexBackendClientJsonLineFramerTest
        PROPERTIES LABELS
                   "component;ai;openai;codex;frontend;client;unix;framing"
                   SKIP_RETURN_CODE 77 TIMEOUT 5
    )

    snodec_add_test(
        CodexBackendClientStdinReaderTest CodexBackendClientStdinReaderTest.cpp
    )
    target_link_libraries(
        CodexBackendClientStdinReaderTest
        PRIVATE snodec-test-support codex-backend-client-support snodec::core
    )
    target_compile_features(
        CodexBackendClientStdinReaderTest PRIVATE cxx_std_20
    )
    set_tests_properties(
        CodexBackendClientStdinReaderTest
        PROPERTIES LABELS
                   "component;ai;openai;codex;frontend;client;stdin;lifecycle"
                   SKIP_RETURN_CODE 77 TIMEOUT 5
    )

    snodec_add_test(
        CodexBackendClientUnixAcceptanceTest
        CodexBackendClientUnixAcceptanceTest.cpp
    )
    target_link_libraries(
        CodexBackendClientUnixAcceptanceTest
        PRIVATE snodec-test-support codex-backend-client-support
                snodec::net-un-stream-legacy snodec::core
    )
    target_compile_features(
        CodexBackendClientUnixAcceptanceTest PRIVATE cxx_std_20
    )
    set_tests_properties(
        CodexBackendClientUnixAcceptanceTest
        PROPERTIES LABELS
                   "component;ai;openai;codex;frontend;client;unix;acceptance"
                   SKIP_RETURN_CODE 77 TIMEOUT 10
    )

    add_executable(
        CodexBackendClientStreamTransportAcceptanceTest
        CodexBackendClientStreamTransportAcceptanceTest.cpp
    )
    target_link_libraries(
        CodexBackendClientStreamTransportAcceptanceTest
        PRIVATE snodec-test-support codex-backend-client-support snodec::core
    )
    target_compile_features(
        CodexBackendClientStreamTransportAcceptanceTest PRIVATE cxx_std_20
    )
    if(AISUITE_ENABLE_CODEX_FRONTEND_TLS)
        target_compile_definitions(
            CodexBackendClientStreamTransportAcceptanceTest
            PRIVATE
                AISUITE_CODEX_TEST_TLS_CERT="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/a1-7b-loopback-cert.pem"
                AISUITE_CODEX_TEST_TLS_KEY="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/a1-7b-loopback-key.pem"
        )
    endif()
    add_test(
        NAME CodexBackendClientIpv4AcceptanceTest
        COMMAND CodexBackendClientStreamTransportAcceptanceTest ipv4
    )
    add_test(
        NAME CodexBackendClientIpv6AcceptanceTest
        COMMAND CodexBackendClientStreamTransportAcceptanceTest ipv6
    )
    set_tests_properties(
        CodexBackendClientIpv4AcceptanceTest
        PROPERTIES
            LABELS
            "component;integration;ai;openai;codex;frontend;client;transport;ipv4;jsonl;acceptance;a1-7c-1"
            SKIP_RETURN_CODE
            77
            TIMEOUT
            10
    )
    set_tests_properties(
        CodexBackendClientIpv6AcceptanceTest
        PROPERTIES
            LABELS
            "component;integration;ai;openai;codex;frontend;client;transport;ipv6;jsonl;acceptance;a1-7c-1"
            SKIP_RETURN_CODE
            77
            TIMEOUT
            10
    )
    if(AISUITE_ENABLE_CODEX_FRONTEND_TLS)
        add_test(
            NAME CodexBackendClientTlsIpv4AcceptanceTest
            COMMAND CodexBackendClientStreamTransportAcceptanceTest tls-ipv4
        )
        add_test(
            NAME CodexBackendClientTlsIpv6AcceptanceTest
            COMMAND CodexBackendClientStreamTransportAcceptanceTest tls-ipv6
        )
        set_tests_properties(
            CodexBackendClientTlsIpv4AcceptanceTest
            PROPERTIES
                LABELS
                "component;integration;ai;openai;codex;frontend;client;transport;ipv4;tls;jsonl;acceptance;a1-7c-1"
                SKIP_RETURN_CODE
                77
                TIMEOUT
                10
        )
        set_tests_properties(
            CodexBackendClientTlsIpv6AcceptanceTest
            PROPERTIES
                LABELS
                "component;integration;ai;openai;codex;frontend;client;transport;ipv6;tls;jsonl;acceptance;a1-7c-1"
                SKIP_RETURN_CODE
                77
                TIMEOUT
                10
        )
    endif()

    if(AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET
       AND TARGET codex-backend-web-adapter
       AND TARGET snodec::http-server-express-legacy-in
       AND TARGET snodec::http-server-express-legacy-in6
       AND TARGET snodec::websocket-server
       AND TARGET snodec::websocket-client
    )
        add_executable(
            CodexBackendClientWebSocketAcceptanceTest
            CodexBackendClientWebSocketAcceptanceTest.cpp
        )
        target_link_libraries(
            CodexBackendClientWebSocketAcceptanceTest
            PRIVATE
                snodec-test-support
                codex-backend-client-support
                codex-backend-web-adapter
                AISuite::OpenAICodexBackend
                AISuite::OpenAICodexFrontend
                snodec::http-server-express-legacy-in
                snodec::http-server-express-legacy-in6
                snodec::websocket-server
                snodec::websocket-client
                snodec::core
        )
        target_compile_features(
            CodexBackendClientWebSocketAcceptanceTest PRIVATE cxx_std_20
        )
        if(AISUITE_ENABLE_CODEX_FRONTEND_TLS
           AND TARGET snodec::http-server-express-tls-in
           AND TARGET snodec::http-server-express-tls-in6
        )
            target_link_libraries(
                CodexBackendClientWebSocketAcceptanceTest
                PRIVATE snodec::http-server-express-tls-in
                        snodec::http-server-express-tls-in6
            )
            target_compile_definitions(
                CodexBackendClientWebSocketAcceptanceTest
                PRIVATE
                    AISUITE_CODEX_WEBSOCKET_ACCEPTANCE_TLS=1
                    AISUITE_CODEX_TEST_TLS_CERT="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/a1-7b-loopback-cert.pem"
                    AISUITE_CODEX_TEST_TLS_KEY="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/a1-7b-loopback-key.pem"
            )
        endif()
        add_test(
            NAME CodexBackendClientWebSocketIpv4AcceptanceTest
            COMMAND CodexBackendClientWebSocketAcceptanceTest websocket-ipv4
        )
        add_test(
            NAME CodexBackendClientWebSocketIpv6AcceptanceTest
            COMMAND CodexBackendClientWebSocketAcceptanceTest websocket-ipv6
        )
        set_tests_properties(
            CodexBackendClientWebSocketIpv4AcceptanceTest
            PROPERTIES
                LABELS
                "component;integration;ai;openai;codex;frontend;client;transport;ipv4;websocket;acceptance;a1-7c-1"
                SKIP_RETURN_CODE
                77
                TIMEOUT
                15
        )
        set_tests_properties(
            CodexBackendClientWebSocketIpv6AcceptanceTest
            PROPERTIES
                LABELS
                "component;integration;ai;openai;codex;frontend;client;transport;ipv6;websocket;acceptance;a1-7c-1"
                SKIP_RETURN_CODE
                77
                TIMEOUT
                15
        )
        if(AISUITE_ENABLE_CODEX_FRONTEND_TLS
           AND TARGET snodec::http-server-express-tls-in
           AND TARGET snodec::http-server-express-tls-in6
        )
            add_test(
                NAME CodexBackendClientWssIpv4AcceptanceTest
                COMMAND CodexBackendClientWebSocketAcceptanceTest wss-ipv4
            )
            add_test(
                NAME CodexBackendClientWssIpv6AcceptanceTest
                COMMAND CodexBackendClientWebSocketAcceptanceTest wss-ipv6
            )
            set_tests_properties(
                CodexBackendClientWssIpv4AcceptanceTest
                PROPERTIES
                    LABELS
                    "component;integration;ai;openai;codex;frontend;client;transport;ipv4;websocket;wss;tls;acceptance;a1-7c-1"
                    SKIP_RETURN_CODE
                    77
                    TIMEOUT
                    15
            )
            set_tests_properties(
                CodexBackendClientWssIpv6AcceptanceTest
                PROPERTIES
                    LABELS
                    "component;integration;ai;openai;codex;frontend;client;transport;ipv6;websocket;wss;tls;acceptance;a1-7c-1"
                    SKIP_RETURN_CODE
                    77
                    TIMEOUT
                    15
            )
        endif()
    endif()

    if(TARGET codex-backend-stream-adapter)
        snodec_add_test(
            CodexBackendClientRealBackendAcceptanceTest
            CodexBackendClientRealBackendAcceptanceTest.cpp
        )
        target_link_libraries(
            CodexBackendClientRealBackendAcceptanceTest
            PRIVATE snodec-test-support codex-backend-client-support
                    codex-backend-stream-adapter AISuite::OpenAICodexBackend
                    snodec::net-un-stream-legacy snodec::core
        )
        target_compile_features(
            CodexBackendClientRealBackendAcceptanceTest PRIVATE cxx_std_20
        )
        set_tests_properties(
            CodexBackendClientRealBackendAcceptanceTest
            PROPERTIES
                LABELS
                "component;ai;openai;codex;frontend;backend;client;unix;acceptance;interactive"
                ENVIRONMENT
                "SNODEC_CODEX_CLIENT_ACCEPTANCE_SCENARIO=interactive"
                SKIP_RETURN_CODE
                77
                TIMEOUT
                20
        )
        add_test(
            NAME CodexBackendClientThreadWorkflowAcceptanceTest
            COMMAND CodexBackendClientRealBackendAcceptanceTest
        )
        set_tests_properties(
            CodexBackendClientThreadWorkflowAcceptanceTest
            PROPERTIES
                LABELS
                "component;ai;openai;codex;frontend;backend;client;unix;acceptance;thread-workflow"
                ENVIRONMENT
                "SNODEC_CODEX_CLIENT_ACCEPTANCE_SCENARIO=thread-workflow"
                SKIP_RETURN_CODE
                77
                TIMEOUT
                20
        )
    endif()
endif()

snodec_add_test(CodexBackendSessionTest CodexBackendSessionTest.cpp)
target_link_libraries(
    CodexBackendSessionTest PRIVATE snodec-test-support
                                    AISuite::OpenAICodexBackend
)
target_compile_features(CodexBackendSessionTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexBackendSessionTest
    PROPERTIES LABELS "component;ai;openai;codex;backend;session"
               SKIP_RETURN_CODE 77 TIMEOUT 10
)

snodec_add_test(CodexBackendCoreTest CodexBackendCoreTest.cpp)
target_link_libraries(
    CodexBackendCoreTest PRIVATE snodec-test-support
                                 AISuite::OpenAICodexBackend snodec::core
)

snodec_add_test(CodexBackendFoundationTest CodexBackendFoundationTest.cpp)
target_link_libraries(
    CodexBackendFoundationTest PRIVATE snodec-test-support
                                       AISuite::OpenAICodexBackend snodec::core
)
target_compile_features(CodexBackendFoundationTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexBackendFoundationTest
    PROPERTIES LABELS "component;ai;openai;codex;backend;foundation"
               SKIP_RETURN_CODE 77 TIMEOUT 12
)
target_compile_features(CodexBackendCoreTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexBackendCoreTest
    PROPERTIES LABELS "component;ai;openai;codex;backend;core" SKIP_RETURN_CODE
               77 TIMEOUT 15
)

snodec_add_test(
    CodexBackendUnixAcceptanceTest
    CodexBackendUnixAcceptanceTest.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/FrontendStreamSocketContext.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/FrontendStreamSocketContextFactory.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/UnixPeerCredentials.cpp
)
target_include_directories(
    CodexBackendUnixAcceptanceTest PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(
    CodexBackendUnixAcceptanceTest
    PRIVATE snodec-test-support AISuite::OpenAICodexFrontend
            AISuite::OpenAICodexBackend ai-openai-codex-frontend-jsonl-framer
            snodec::net-un-stream-legacy
            snodec::core
)
target_compile_features(CodexBackendUnixAcceptanceTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexBackendUnixAcceptanceTest
    PROPERTIES LABELS
               "component;ai;openai;codex;backend;frontend;unix;acceptance"
               SKIP_RETURN_CODE 77 TIMEOUT 20
)

snodec_add_test(CodexTypedProtocolEngineTest CodexTypedProtocolEngineTest.cpp)
target_link_libraries(
    CodexTypedProtocolEngineTest PRIVATE snodec-test-support
                                         AISuite::OpenAICodex snodec::core
)
target_compile_features(CodexTypedProtocolEngineTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexTypedProtocolEngineTest
    PROPERTIES LABELS "component;ai;openai;codex;typed;protocol;engine"
               SKIP_RETURN_CODE 77 TIMEOUT 30
)

snodec_add_test(CodexPublicApiFacadeTest CodexPublicApiFacadeTest.cpp)
target_link_libraries(
    CodexPublicApiFacadeTest PRIVATE snodec-test-support AISuite::OpenAICodex
)
target_compile_features(CodexPublicApiFacadeTest PRIVATE cxx_std_20)
target_compile_definitions(
    CodexPublicApiFacadeTest
    PRIVATE AISUITE_TEST_PROJECT_VERSION="${PROJECT_VERSION}"
)
set_tests_properties(
    CodexPublicApiFacadeTest
    PROPERTIES LABELS "component;ai;openai;codex;api;facade" TIMEOUT 5
)

add_test(
    NAME CodexPublicApiUsageGuardTest
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/CodexPublicApiUsageGuardTest.py
        --source-root ${PROJECT_SOURCE_DIR}/src
)
set_tests_properties(
    CodexPublicApiUsageGuardTest
    PROPERTIES LABELS "component;ai;openai;codex;api;facade;policy" TIMEOUT 5
)

add_test(
    NAME CodexFrontendClientContractGuardTest
    COMMAND
        ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/CodexFrontendClientContractGuardTest.py
        --controller ${PROJECT_SOURCE_DIR}/src/apps/codex-backend-client/CommandDrainController.cpp
        --integration ${CMAKE_CURRENT_SOURCE_DIR}/CodexFrontendServiceClientIntegrationTest.cpp
)
set_tests_properties(
    CodexFrontendClientContractGuardTest
    PROPERTIES LABELS "component;ai;openai;codex;frontend;client;lifecycle;policy;a1-7c-1" TIMEOUT 5
)

add_executable(CodexStdioScenarioTest CodexStdioScenarioTest.cpp)
target_link_libraries(
    CodexStdioScenarioTest PRIVATE snodec-test-support AISuite::OpenAICodex
                                   snodec::core
)
target_compile_features(CodexStdioScenarioTest PRIVATE cxx_std_20)
target_compile_definitions(
    CodexStdioScenarioTest
    PRIVATE CODEX_FAKE_APP_SERVER="$<TARGET_FILE:CodexFakeAppServer>"
)
add_dependencies(CodexStdioScenarioTest CodexFakeAppServer)

add_executable(CodexGlobalShutdownTest CodexGlobalShutdownTest.cpp)
target_link_libraries(
    CodexGlobalShutdownTest PRIVATE snodec-test-support AISuite::OpenAICodex
                                    snodec::core
)
target_compile_features(CodexGlobalShutdownTest PRIVATE cxx_std_20)
target_compile_definitions(
    CodexGlobalShutdownTest
    PRIVATE CODEX_FAKE_APP_SERVER="$<TARGET_FILE:CodexFakeAppServer>"
)
add_dependencies(CodexGlobalShutdownTest CodexFakeAppServer)

foreach(scenario IN ITEMS forced-polling-normal forced-polling-ignore-sigterm
                          ignore-sigterm destroy-client
)
    string(REPLACE "-" "_" scenario_name ${scenario})
    add_test(NAME CodexGlobalShutdown_${scenario_name}
             COMMAND CodexGlobalShutdownTest ${scenario}
    )
    set_tests_properties(
        CodexGlobalShutdown_${scenario_name}
        PROPERTIES LABELS "component;ai;openai;codex;stdio;shutdown"
                   SKIP_RETURN_CODE 77 TIMEOUT 5
    )
endforeach()

snodec_add_test(CodexShutdownAdmissionTest CodexShutdownAdmissionTest.cpp)
target_link_libraries(
    CodexShutdownAdmissionTest PRIVATE snodec-test-support
                                       AISuite::OpenAICodex snodec::core
)
target_compile_features(CodexShutdownAdmissionTest PRIVATE cxx_std_20)
target_compile_definitions(
    CodexShutdownAdmissionTest
    PRIVATE CODEX_FAKE_APP_SERVER="$<TARGET_FILE:CodexFakeAppServer>"
)
add_dependencies(CodexShutdownAdmissionTest CodexFakeAppServer)
set_tests_properties(
    CodexShutdownAdmissionTest
    PROPERTIES LABELS "component;ai;openai;codex;stdio;shutdown"
               SKIP_RETURN_CODE 77 TIMEOUT 5
)

add_executable(CodexDescriptorCollisionTest CodexDescriptorCollisionTest.cpp)
target_link_libraries(
    CodexDescriptorCollisionTest PRIVATE snodec-test-support
                                         AISuite::OpenAICodex snodec::core
)
target_compile_features(CodexDescriptorCollisionTest PRIVATE cxx_std_20)
target_compile_definitions(
    CodexDescriptorCollisionTest
    PRIVATE CODEX_FAKE_APP_SERVER="$<TARGET_FILE:CodexFakeAppServer>"
)
add_dependencies(CodexDescriptorCollisionTest CodexFakeAppServer)

foreach(scenario IN ITEMS stdin stdout stderr all)
    add_test(NAME CodexDescriptorCollision_${scenario}
             COMMAND CodexDescriptorCollisionTest ${scenario}
    )
    set_tests_properties(
        CodexDescriptorCollision_${scenario}
        PROPERTIES LABELS "component;ai;openai;codex;stdio;descriptor"
                   SKIP_RETURN_CODE 77 TIMEOUT 5
    )
endforeach()

foreach(
    scenario IN
    ITEMS exit-before-initialize
          close-stdout
          initialization-error
          malformed-json
          launch-failure
          post-spawn-setup-failure
          registration-lifecycle-failure
          registration-pidfd-fallback
          registration-stdin-failure
          registration-stdout-failure
          registration-stderr-failure
          bounded-overflow
          slow-stdin
          ignore-shutdown
          blocked-shutdown
          blocked-ignore-shutdown
          stop-starting
          stop-initializing
          stdio-framing
          close-stderr
          recover-after-failure
          forced-fallback-normal
          forced-fallback-ignore-shutdown
          forced-fallback-close-stdout
          forced-fallback-exit-before-initialize
)
    string(REPLACE "-" "_" scenario_name ${scenario})
    add_test(NAME CodexStdioScenario_${scenario_name}
             COMMAND CodexStdioScenarioTest ${scenario}
    )
    set_tests_properties(
        CodexStdioScenario_${scenario_name}
        PROPERTIES LABELS "component;ai;openai;codex;stdio" SKIP_RETURN_CODE 77
                   TIMEOUT 8
    )
endforeach()

snodec_add_test(CodexAppServerIntegrationTest CodexAppServerIntegrationTest.cpp)
target_link_libraries(
    CodexAppServerIntegrationTest PRIVATE snodec-test-support
                                          AISuite::OpenAICodex snodec::core
)

snodec_add_test(
    CodexTypedAppServerIntegrationTest CodexTypedAppServerIntegrationTest.cpp
)
target_link_libraries(
    CodexTypedAppServerIntegrationTest
    PRIVATE snodec-test-support AISuite::OpenAICodex snodec::core
)
target_compile_features(CodexTypedAppServerIntegrationTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexTypedAppServerIntegrationTest
    PROPERTIES LABELS "integration;ai;openai;codex;typed;optional"
               SKIP_RETURN_CODE 77 TIMEOUT 60
)
target_compile_features(CodexAppServerIntegrationTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexAppServerIntegrationTest
    PROPERTIES LABELS "integration;ai;openai;codex;optional" SKIP_RETURN_CODE
               77 TIMEOUT 15
)

set_tests_properties(
    CodexStdioLifecycleTest
    PROPERTIES LABELS "component;ai;openai;codex;stdio;lifecycle"
               SKIP_RETURN_CODE 77 TIMEOUT 8
)

snodec_add_test(CodexBackendLifecycleTest CodexBackendLifecycleTest.cpp)
target_include_directories(
    CodexBackendLifecycleTest PRIVATE ${PROJECT_SOURCE_DIR}
)
target_compile_features(CodexBackendLifecycleTest PRIVATE cxx_std_20)
target_link_libraries(
    CodexBackendLifecycleTest
    PRIVATE snodec-test-support AISuite::OpenAICodexBackend snodec::logger
)
set_tests_properties(
    CodexBackendLifecycleTest
    PROPERTIES LABELS "component;codex;backend;thread;turn;lifecycle;log"
               SKIP_RETURN_CODE 77 TIMEOUT 10
)

snodec_add_test(
    AppServerClientRequestLifecycleTest
    AppServerClientRequestLifecycleTest.cpp
)
target_include_directories(
    AppServerClientRequestLifecycleTest PRIVATE ${PROJECT_SOURCE_DIR}
)
target_compile_features(
    AppServerClientRequestLifecycleTest PRIVATE cxx_std_20
)
target_link_libraries(
    AppServerClientRequestLifecycleTest
    PRIVATE snodec-test-support AISuite::OpenAICodex snodec::core
            snodec::logger
)
set_tests_properties(
    AppServerClientRequestLifecycleTest
    PROPERTIES LABELS "component;codex;protocol;request;lifecycle;log"
               SKIP_RETURN_CODE 77 TIMEOUT 10
)

snodec_add_test(
    CodexA14RuntimePlatformNotificationTest
    CodexA14RuntimePlatformNotificationTest.cpp
)
target_link_libraries(
    CodexA14RuntimePlatformNotificationTest
    PRIVATE snodec-test-support AISuite::OpenAICodex
)
target_compile_features(CodexA14RuntimePlatformNotificationTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexA14RuntimePlatformNotificationTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;runtime;platform;notifications;a1-4"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)

snodec_add_test(
    CodexA14RuntimePlatformNotificationWireTest
    CodexA14RuntimePlatformNotificationWireTest.cpp
)
target_link_libraries(
    CodexA14RuntimePlatformNotificationWireTest
    PRIVATE snodec-test-support AISuite::OpenAICodex snodec::core
)
target_compile_features(CodexA14RuntimePlatformNotificationWireTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexA14RuntimePlatformNotificationWireTest
    PROPERTIES
        LABELS "component;ai;openai;codex;typed;runtime;platform;notifications;wire;a1-4"
        SKIP_RETURN_CODE 77
        TIMEOUT 10
)
snodec_add_test(
    CodexFrontendWebConfigurationTest
    CodexFrontendWebConfigurationTest.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/Configuration.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/FrontendWebSecurity.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/ReferenceAuthentication.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/ReferenceInvocationPolicy.cpp
)
target_include_directories(
    CodexFrontendWebConfigurationTest PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(
    CodexFrontendWebConfigurationTest
    PRIVATE snodec-test-support AISuite::OpenAICodexBackend
            AISuite::OpenAICodexFrontend snodec::core
)
target_compile_definitions(
    CodexFrontendWebConfigurationTest
    PRIVATE
        AISUITE_CODEX_TEST_TLS_CERT="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/a1-7b-loopback-cert.pem"
        AISUITE_CODEX_TEST_TLS_KEY="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/a1-7b-loopback-key.pem"
)
if(AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET)
    target_compile_definitions(
        CodexFrontendWebConfigurationTest
        PRIVATE AISUITE_CODEX_FRONTEND_WEBSOCKET=1
    )
endif()
if(AISUITE_ENABLE_CODEX_FRONTEND_TLS)
    target_compile_definitions(
        CodexFrontendWebConfigurationTest PRIVATE AISUITE_CODEX_FRONTEND_TLS=1
    )
endif()
target_compile_features(CodexFrontendWebConfigurationTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexFrontendWebConfigurationTest
    PROPERTIES
        LABELS
        "component;ai;openai;codex;frontend;web;configuration;security;a1-7b"
        TIMEOUT 5
)

snodec_add_test(
    CodexFrontendWebSecurityTest CodexFrontendWebSecurityTest.cpp
    ${PROJECT_SOURCE_DIR}/src/apps/codex-backend/FrontendWebSecurity.cpp
)
target_include_directories(
    CodexFrontendWebSecurityTest PRIVATE ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(CodexFrontendWebSecurityTest PRIVATE snodec-test-support)
target_compile_features(CodexFrontendWebSecurityTest PRIVATE cxx_std_20)
set_tests_properties(
    CodexFrontendWebSecurityTest
    PROPERTIES LABELS "component;ai;openai;codex;frontend;web;security;a1-7b"
               TIMEOUT 5
)

if(AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET
   AND TARGET snodec::http-server-express-legacy-in
   AND TARGET snodec::http-client
   AND TARGET codex-backend-web-adapter
)
    snodec_add_test(
        CodexFrontendWebHttpIntegrationTest
        CodexFrontendWebHttpIntegrationTest.cpp
    )
    target_include_directories(
        CodexFrontendWebHttpIntegrationTest PRIVATE ${PROJECT_SOURCE_DIR}/src
    )
    target_link_libraries(
        CodexFrontendWebHttpIntegrationTest
        PRIVATE snodec-test-support
                codex-backend-web-adapter
                AISuite::OpenAICodexBackend
                AISuite::OpenAICodexFrontend
                snodec::http-server-express-legacy-in
                snodec::http-client
                snodec::core
    )
    target_compile_features(
        CodexFrontendWebHttpIntegrationTest PRIVATE cxx_std_20
    )
    set_tests_properties(
        CodexFrontendWebHttpIntegrationTest
        PROPERTIES
            LABELS
            "component;integration;ai;openai;codex;frontend;http;express;static;origin;security"
            SKIP_RETURN_CODE
            77
            TIMEOUT
            15
    )
endif()

if(AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET
   AND TARGET snodec::http-server-express-legacy-in
   AND TARGET snodec::websocket-server
   AND TARGET snodec::websocket-client
   AND TARGET codex-backend-stream-adapter
   AND TARGET codex-backend-web-adapter
)
    snodec_add_test(
        CodexFrontendWebSocketIntegrationTest
        CodexFrontendWebSocketIntegrationTest.cpp
    )
    target_include_directories(
        CodexFrontendWebSocketIntegrationTest PRIVATE ${PROJECT_SOURCE_DIR}/src
    )
    target_link_libraries(
        CodexFrontendWebSocketIntegrationTest
        PRIVATE snodec-test-support
                codex-backend-stream-adapter
                codex-backend-web-adapter
                AISuite::OpenAICodexBackend
                AISuite::OpenAICodexFrontend
                snodec::http-server-express-legacy-in
                snodec::websocket-server
                snodec::websocket-client
                snodec::core
    )
    target_compile_features(
        CodexFrontendWebSocketIntegrationTest PRIVATE cxx_std_20
    )
    set_tests_properties(
        CodexFrontendWebSocketIntegrationTest
        PROPERTIES
            LABELS
            "component;integration;ai;openai;codex;frontend;websocket;express;security"
            SKIP_RETURN_CODE
            77
            TIMEOUT
            15
    )
endif()

if(AISUITE_ENABLE_CODEX_FRONTEND_WEBSOCKET
   AND AISUITE_ENABLE_CODEX_FRONTEND_TLS
   AND TARGET snodec::http-server-express-tls-in
   AND TARGET snodec::net-in-stream-tls
   AND TARGET snodec::websocket-server
   AND TARGET snodec::websocket-client
   AND TARGET codex-backend-web-adapter
)
    snodec_add_test(
        CodexFrontendWebSocketTlsIntegrationTest
        CodexFrontendWebSocketTlsIntegrationTest.cpp
    )
    target_include_directories(
        CodexFrontendWebSocketTlsIntegrationTest
        PRIVATE ${PROJECT_SOURCE_DIR}/src
    )
    target_compile_definitions(
        CodexFrontendWebSocketTlsIntegrationTest
        PRIVATE
            AISUITE_CODEX_TEST_TLS_CERT="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/a1-7b-loopback-cert.pem"
            AISUITE_CODEX_TEST_TLS_KEY="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/a1-7b-loopback-key.pem"
    )
    target_link_libraries(
        CodexFrontendWebSocketTlsIntegrationTest
        PRIVATE snodec-test-support
                codex-backend-web-adapter
                AISuite::OpenAICodexBackend
                AISuite::OpenAICodexFrontend
                snodec::http-server-express-tls-in
                snodec::net-in-stream-tls
                snodec::websocket-server
                snodec::websocket-client
                snodec::core
    )
    target_compile_features(
        CodexFrontendWebSocketTlsIntegrationTest PRIVATE cxx_std_20
    )
    set_tests_properties(
        CodexFrontendWebSocketTlsIntegrationTest
        PROPERTIES
            LABELS
            "component;integration;ai;openai;codex;frontend;websocket;wss;tls;express;security"
            SKIP_RETURN_CODE
            77
            TIMEOUT
            15
    )
endif()
