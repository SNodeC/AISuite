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
    PROPERTIES LABELS "policy;ai;openai;codex;current-state;headers;public-api"
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
               "policy;ai;openai;codex;current-state;headers;install;consumer"
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
               "policy;ai;openai;codex;current-state;logging;api;architecture"
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
    PROPERTIES LABELS "policy;ai;openai;codex;current-state;logging;architecture"
               ENVIRONMENT "AISUITE_SOURCE_DIR=${PROJECT_SOURCE_DIR}" TIMEOUT
               30
)

add_test(
    NAME CodexPolicyMutationTest
    COMMAND
        ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/CodexPolicyMutationTest.py --repo-root
        ${PROJECT_SOURCE_DIR}
        --public-header-policy $<TARGET_FILE:CodexPublicHeaderPolicyTest>
        --logging-policy $<TARGET_FILE:CodexLoggingApiSurfacePolicyTest>
        --semantic-logger-policy $<TARGET_FILE:CodexSemanticLoggerPolicyTest>
)
set_tests_properties(
    CodexPolicyMutationTest
    PROPERTIES DEPENDS
               "CodexPublicHeaderPolicyTest;CodexPublicHeaderSelfContainmentTest;CodexLoggingApiSurfacePolicyTest;CodexSemanticLoggerPolicyTest;CodexSyntheticSecretLeakGuardTest"
               LABELS
               "policy;mutation;current-state;ai;openai;codex"
               ENVIRONMENT
               "PYTHONDONTWRITEBYTECODE=1"
               RUN_SERIAL
               TRUE
               TIMEOUT
               300
)
