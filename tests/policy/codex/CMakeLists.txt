add_executable(CodexPublicHeaderPolicyTest CodexPublicHeaderPolicyTest.cpp)
add_executable(
    CodexLoggingApiSurfacePolicyTest CodexLoggingApiSurfacePolicyTest.cpp
)
add_executable(CodexSemanticLoggerPolicyTest CodexSemanticLoggerPolicyTest.cpp)

foreach(policy_test IN
        ITEMS CodexPublicHeaderPolicyTest CodexLoggingApiSurfacePolicyTest
              CodexSemanticLoggerPolicyTest
)
    target_compile_features(${policy_test} PRIVATE cxx_std_20)
    target_include_directories(${policy_test} PRIVATE "${PROJECT_SOURCE_DIR}")
endforeach()

add_dependencies(
    CodexPublicHeaderPolicyTest ai-openai-codex ai-openai-codex-backend
    ai-openai-codex-frontend
)

add_test(NAME CodexPublicHeaderPolicyTest
         COMMAND CodexPublicHeaderPolicyTest --repo-root ${PROJECT_SOURCE_DIR}
)
set_tests_properties(
    CodexPublicHeaderPolicyTest
    PROPERTIES LABELS "policy;ai;openai;codex;extraction;headers;public-api"
               ENVIRONMENT "AISUITE_SOURCE_DIR=${PROJECT_SOURCE_DIR}" TIMEOUT
               30
)

add_test(
    NAME CodexPublicHeaderSelfContainmentTest
    COMMAND
        ${CMAKE_COMMAND}
        -DAISUITE_PUBLIC_HEADER_POLICY_EXECUTABLE=$<TARGET_FILE:CodexPublicHeaderPolicyTest>
        -DAISUITE_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DAISUITE_BUILD_DIR=${CMAKE_BINARY_DIR}
        -DAISUITE_CMAKE_COMMAND=${CMAKE_COMMAND}
        -DAISUITE_CMAKE_GENERATOR=${CMAKE_GENERATOR}
        -DAISUITE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -DAISUITE_SNODEC_DIR=${snodec_DIR}
        -DAISUITE_NLOHMANN_JSON_DIR=${nlohmann_json_DIR} -P
        ${CMAKE_CURRENT_SOURCE_DIR}/CodexPublicHeaderSelfContainmentTest.cmake
)
set_tests_properties(
    CodexPublicHeaderSelfContainmentTest
    PROPERTIES DEPENDS
               CodexPublicHeaderPolicyTest
               LABELS
               "policy;ai;openai;codex;extraction;headers;install;consumer"
               RUN_SERIAL
               TRUE
               TIMEOUT
               300
)

add_test(NAME CodexLoggingApiSurfacePolicyTest
         COMMAND CodexLoggingApiSurfacePolicyTest --repo-root
                 ${PROJECT_SOURCE_DIR}
)
set_tests_properties(
    CodexLoggingApiSurfacePolicyTest
    PROPERTIES LABELS
               "policy;ai;openai;codex;extraction;logging;api;architecture"
               ENVIRONMENT
               "AISUITE_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
               TIMEOUT
               30
)

add_test(NAME CodexSemanticLoggerPolicyTest
         COMMAND CodexSemanticLoggerPolicyTest --repo-root
                 ${PROJECT_SOURCE_DIR}
)
set_tests_properties(
    CodexSemanticLoggerPolicyTest
    PROPERTIES LABELS "policy;ai;openai;codex;extraction;logging;architecture"
               ENVIRONMENT "AISUITE_SOURCE_DIR=${PROJECT_SOURCE_DIR}" TIMEOUT
               30
)

set(CODEX_POLICY_OWNERSHIP_TOOL
    "${PROJECT_SOURCE_DIR}/tools/extraction/verify_codex_policy_ownership.py"
)
set(CODEX_POLICY_SNODEC_ARGUMENTS)
if(NOT "${AISUITE_TEST_SNODEC_SOURCE_REPOSITORY}" STREQUAL "")
    list(APPEND CODEX_POLICY_SNODEC_ARGUMENTS --snodec-root
         "${AISUITE_TEST_SNODEC_SOURCE_REPOSITORY}"
    )
endif()

add_test(
    NAME CodexPolicyOwnershipTest
    COMMAND
        ${Python3_EXECUTABLE} -B ${CODEX_POLICY_OWNERSHIP_TOOL} check
        --repo-root ${PROJECT_SOURCE_DIR} --build-dir ${CMAKE_BINARY_DIR}
        ${CODEX_POLICY_SNODEC_ARGUMENTS}
)
set_tests_properties(
    CodexPolicyOwnershipTest
    PROPERTIES
        DEPENDS
        "CodexPublicHeaderPolicyTest;CodexPublicHeaderSelfContainmentTest;CodexLoggingApiSurfacePolicyTest;CodexSemanticLoggerPolicyTest;CodexSyntheticSecretLeakGuardTest"
        LABELS
        "policy;verification;ownership;cutover-readiness;ai;openai;codex;extraction"
        ENVIRONMENT
        "PYTHONDONTWRITEBYTECODE=1"
        RUN_SERIAL
        TRUE
        TIMEOUT
        120
)

add_test(
    NAME CodexPolicyMutationTest
    COMMAND
        ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/CodexPolicyMutationTest.py --repo-root
        ${PROJECT_SOURCE_DIR} --build-dir ${CMAKE_BINARY_DIR}
        --public-header-policy $<TARGET_FILE:CodexPublicHeaderPolicyTest>
        --logging-policy $<TARGET_FILE:CodexLoggingApiSurfacePolicyTest>
        --semantic-logger-policy $<TARGET_FILE:CodexSemanticLoggerPolicyTest>
        --ownership-tool ${CODEX_POLICY_OWNERSHIP_TOOL} --baseline-model
        ${PROJECT_SOURCE_DIR}/docs/extraction/codex-policy-baseline-ctest.json
        --final-model
        ${PROJECT_SOURCE_DIR}/docs/extraction/codex-policy-final-ctest.json
        --ownership
        ${PROJECT_SOURCE_DIR}/docs/extraction/codex-policy-ownership.json
        --manifest ${PROJECT_SOURCE_DIR}/docs/extraction/source-manifest.json
        --workflow ${PROJECT_SOURCE_DIR}/.github/workflows/ci.yml
        --source-package-test
        ${PROJECT_SOURCE_DIR}/tests/AISuiteSourcePackageTest.cmake
        --binary-package-test
        ${PROJECT_SOURCE_DIR}/tests/AISuiteBinaryPackageTest.cmake
)
set_tests_properties(
    CodexPolicyMutationTest
    PROPERTIES DEPENDS
               CodexPolicyOwnershipTest
               LABELS
               "policy;verification;mutation;ai;openai;codex;extraction"
               ENVIRONMENT
               "PYTHONDONTWRITEBYTECODE=1"
               RUN_SERIAL
               TRUE
               TIMEOUT
               300
)
