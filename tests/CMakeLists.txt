find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(AddAISuiteTest)

add_library(snodec-test-support STATIC support/TestResult.cpp)
target_include_directories(
    snodec-test-support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_features(snodec-test-support PUBLIC cxx_std_20)

# Existing tests refer to support/TestResult.h through the test-root include.
include_directories("${CMAKE_CURRENT_SOURCE_DIR}")

add_subdirectory(component/codex)

add_test(
    NAME AISuiteExtractionGuardTest
    COMMAND
        ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tools/extraction/verify_extraction.py
        --repo-root ${PROJECT_SOURCE_DIR}
)
set_tests_properties(
    AISuiteExtractionGuardTest
    PROPERTIES LABELS "extraction;codex;history;package" TIMEOUT 60
)

get_filename_component(AISUITE_TEST_SNODEC_PREFIX "${snodec_DIR}/../../.." ABSOLUTE)

add_test(
    NAME AISuiteInstalledConsumerTest
    COMMAND
        ${CMAKE_COMMAND}
        -DAISUITE_BUILD_DIR=${CMAKE_BINARY_DIR}
        -DAISUITE_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DSNODEC_PREFIX=${AISUITE_TEST_SNODEC_PREFIX}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/AISuiteInstalledConsumerTest.cmake
)
set_tests_properties(
    AISuiteInstalledConsumerTest
    PROPERTIES LABELS "install;consumer;codex;extraction" TIMEOUT 300
)

add_test(
    NAME AISuiteSourcePackageTest
    COMMAND
        ${CMAKE_COMMAND}
        -DAISUITE_BUILD_DIR=${CMAKE_BINARY_DIR}
        -DAISUITE_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DCMAKE_CPACK_COMMAND=${CMAKE_CPACK_COMMAND}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/AISuiteSourcePackageTest.cmake
)
set_tests_properties(
    AISuiteSourcePackageTest
    PROPERTIES
        LABELS "package;source-package;codex;extraction"
        RUN_SERIAL TRUE
        TIMEOUT 300
)

add_test(
    NAME AISuiteBinaryPackageTest
    COMMAND
        ${CMAKE_COMMAND}
        -DAISUITE_BUILD_DIR=${CMAKE_BINARY_DIR}
        -DCMAKE_CPACK_COMMAND=${CMAKE_CPACK_COMMAND}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/AISuiteBinaryPackageTest.cmake
)
set_tests_properties(
    AISuiteBinaryPackageTest
    PROPERTIES
        LABELS "package;binary-package;codex;extraction"
        RUN_SERIAL TRUE
        TIMEOUT 300
)

set(CODEX_A14_EVIDENCE_ROOT
    ${PROJECT_SOURCE_DIR}/tools/codex/app-server-evidence/0.144.6
)
add_test(
    NAME CodexA14AuditToolTest
    COMMAND
        ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/component/codex/CodexA14AuditToolTest.py
        --tool ${PROJECT_SOURCE_DIR}/tools/codex/app_server_a1_4.py
        --repo-root ${PROJECT_SOURCE_DIR}
        --start-state ${CODEX_A14_EVIDENCE_ROOT}/a1-4-start-state.json
        --partition ${CODEX_A14_EVIDENCE_ROOT}/a1-4-total-partition.json
        --closure ${CODEX_A14_EVIDENCE_ROOT}/a1-4-type-closure.json
        --plan ${CODEX_A14_EVIDENCE_ROOT}/a1-4-implementation-plan.json
        --ledger ${CODEX_A14_EVIDENCE_ROOT}/a1-final-cross-slice-ledger.json
)
set_tests_properties(
    CodexA14AuditToolTest
    PROPERTIES
        LABELS "component;ai;openai;codex;phase-a1-4;schema;audit;generated;extraction"
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        RUN_SERIAL TRUE
        TIMEOUT 90
)

add_test(
    NAME CodexA14UserIntegrationsAuditTest
    COMMAND
        ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/component/codex/CodexA14UserIntegrationsAuditTest.py
        --tool ${PROJECT_SOURCE_DIR}/tools/codex/app_server_a1_4_user_integrations.py
        --repo-root ${PROJECT_SOURCE_DIR}
        --start-state ${CODEX_A14_EVIDENCE_ROOT}/a1-4-user-integrations-start-state.json
        --batch-plan ${CODEX_A14_EVIDENCE_ROOT}/a1-4-user-integrations-batch-plan.json
)
set_tests_properties(
    CodexA14UserIntegrationsAuditTest
    PROPERTIES
        DEPENDS CodexA14AuditToolTest
        LABELS "component;ai;openai;codex;phase-a1-4;user-integrations;schema;audit;generated;extraction"
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        RUN_SERIAL TRUE
        TIMEOUT 90
)

add_test(
    NAME CodexSyntheticSecretLeakGuardTest
    COMMAND
        ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/policy/security/CodexSyntheticSecretLeakGuardTest.py
        --repo-root ${PROJECT_SOURCE_DIR}
        --build-root ${CMAKE_BINARY_DIR}
)
set_tests_properties(
    CodexSyntheticSecretLeakGuardTest
    PROPERTIES
        LABELS "policy;security;codex;extraction;package"
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        DEPENDS
            "AISuiteInstalledConsumerTest;AISuiteSourcePackageTest;AISuiteBinaryPackageTest;CodexAppServerGeneratedArtifactsGuardTest;CodexA14AuditToolTest"
        RUN_SERIAL TRUE
        TIMEOUT 120
)
