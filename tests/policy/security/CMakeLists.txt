add_test(
    NAME CodexSyntheticSecretLeakGuardTest
    COMMAND
        ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/policy/security/CodexSyntheticSecretLeakGuardTest.py
        --repo-root ${PROJECT_SOURCE_DIR} --build-root ${CMAKE_BINARY_DIR}
)
set_tests_properties(
    CodexSyntheticSecretLeakGuardTest
    PROPERTIES
        LABELS
        "policy;security;codex;extraction;package"
        ENVIRONMENT
        "PYTHONDONTWRITEBYTECODE=1"
        DEPENDS
        "AISuiteInstalledConsumerTest;AISuiteSourcePackageTest;AISuiteBinaryPackageTest;CodexAppServerGeneratedArtifactsGuardTest;CodexA14AuditToolTest"
        RUN_SERIAL
        TRUE
        TIMEOUT
        120
        WORKING_DIRECTORY
        "${CMAKE_BINARY_DIR}/tests"
)
