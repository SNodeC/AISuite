add_test(
    NAME CodexFrontendP3OracleIsolationTest
    COMMAND
        ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/CodexFrontendP3OracleIsolationTest.py
        --repo-root ${PROJECT_SOURCE_DIR}
        --source-authority
        ${PROJECT_SOURCE_DIR}/tests/component/codex/oracle/source-closure.txt
)
set_tests_properties(
    CodexFrontendP3OracleIsolationTest
    PROPERTIES
        LABELS "policy;codex;frontend;p3;oracle;isolation"
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        TIMEOUT 10
)

add_test(
    NAME CodexFrontendP3OracleDependencyResolutionTest
    COMMAND
        ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/CodexFrontendP3OracleDependencyResolutionTest.py
        --repo-root ${PROJECT_SOURCE_DIR}
        --build-dir ${CMAKE_BINARY_DIR}
        --generator "${CMAKE_GENERATOR}"
        --make-program "${CMAKE_MAKE_PROGRAM}"
        --source-authority
        ${PROJECT_SOURCE_DIR}/tests/component/codex/oracle/source-closure.txt
)
set_tests_properties(
    CodexFrontendP3OracleDependencyResolutionTest
    PROPERTIES
        LABELS "policy;codex;frontend;p3;oracle;dependencies"
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        TIMEOUT 60
)

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
if(TARGET ai-openai-codex-frontend-client)
    add_dependencies(
        CodexPublicHeaderPolicyTest ai-openai-codex-frontend-client
    )
endif()

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
        -DAISUITE_BUILD_CODEX_FRONTEND_CLIENT=${AISUITE_BUILD_CODEX_FRONTEND_CLIENT}
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
               600
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

add_test(
    NAME CodexArchitectureReductionBaselineTest
    COMMAND
        ${Python3_EXECUTABLE} -B
        ${CMAKE_CURRENT_SOURCE_DIR}/CodexArchitectureReductionBaselineTest.py
        --source-dir ${PROJECT_SOURCE_DIR}
        --tool
        ${PROJECT_SOURCE_DIR}/tools/codex/capture_architecture_baseline.py
        --baseline
        ${PROJECT_SOURCE_DIR}/docs/ai/openai/codex/architecture-reduction/p0-baseline.json
        --roadmap
        ${PROJECT_SOURCE_DIR}/docs/ai/openai/codex/architecture-reduction/README.md
)
set_tests_properties(
    CodexArchitectureReductionBaselineTest
    PROPERTIES
        LABELS "codex;architecture;baseline;policy;p0;reduction"
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        TIMEOUT 60
)

if(TARGET ai-openai-codex-frontend-server-core AND
   TARGET ai-openai-codex-frontend-client-core AND
   TARGET codex-backend AND
   TARGET codex-backend-client
)
    set(
        frontend_core_dependency_manifest
        "${CMAKE_CURRENT_BINARY_DIR}/CodexFrontendCoreDependencyPolicy-$<CONFIG>.txt"
    )
    string(
        CONCAT frontend_core_dependency_manifest_content
        "ai-openai-codex-frontend-protocol|LINK_LIBRARIES|$<TARGET_PROPERTY:ai-openai-codex-frontend-protocol,LINK_LIBRARIES>\n"
        "ai-openai-codex-frontend-protocol|INTERFACE_LINK_LIBRARIES|$<TARGET_PROPERTY:ai-openai-codex-frontend-protocol,INTERFACE_LINK_LIBRARIES>\n"
        "ai-openai-codex-frontend-model|LINK_LIBRARIES|$<TARGET_PROPERTY:ai-openai-codex-frontend-model,LINK_LIBRARIES>\n"
        "ai-openai-codex-frontend-model|INTERFACE_LINK_LIBRARIES|$<TARGET_PROPERTY:ai-openai-codex-frontend-model,INTERFACE_LINK_LIBRARIES>\n"
        "ai-openai-codex-frontend-server-core|LINK_LIBRARIES|$<TARGET_PROPERTY:ai-openai-codex-frontend-server-core,LINK_LIBRARIES>\n"
        "ai-openai-codex-frontend-server-core|INTERFACE_LINK_LIBRARIES|$<TARGET_PROPERTY:ai-openai-codex-frontend-server-core,INTERFACE_LINK_LIBRARIES>\n"
        "ai-openai-codex-frontend-client-core|LINK_LIBRARIES|$<TARGET_PROPERTY:ai-openai-codex-frontend-client-core,LINK_LIBRARIES>\n"
        "ai-openai-codex-frontend-client-core|INTERFACE_LINK_LIBRARIES|$<TARGET_PROPERTY:ai-openai-codex-frontend-client-core,INTERFACE_LINK_LIBRARIES>\n"
    )
    file(
        GENERATE
        OUTPUT "${frontend_core_dependency_manifest}"
        CONTENT "${frontend_core_dependency_manifest_content}"
    )
    add_test(
        NAME CodexFrontendCoreDependencyPolicyTest
        COMMAND
            ${Python3_EXECUTABLE} -B
            ${CMAKE_CURRENT_SOURCE_DIR}/CodexFrontendCoreDependencyPolicyTest.py
            --source-dir ${PROJECT_SOURCE_DIR}
            --build-dir ${CMAKE_BINARY_DIR}
            --link-manifest ${frontend_core_dependency_manifest}
    )
    set_tests_properties(
        CodexFrontendCoreDependencyPolicyTest
        PROPERTIES
            LABELS "policy;ai;openai;codex;frontend;dependency;file-api;p2"
            ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
            TIMEOUT 60
    )
endif()
