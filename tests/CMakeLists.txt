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

set(AISUITE_TEST_INSTALLED_CONSUMER_TEMP_ROOT
    ""
    CACHE
        PATH
        "Optional spacious root for the disjoint genuine installed-consumer staging tree"
)

add_test(
    NAME AISuiteInstalledConsumerTest
    COMMAND
        ${CMAKE_COMMAND}
        -DAISUITE_BUILD_DIR=${CMAKE_BINARY_DIR}
        -DAISUITE_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -DAISUITE_INSTALLED_CONSUMER_TEMP_ROOT=${AISUITE_TEST_INSTALLED_CONSUMER_TEMP_ROOT}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/AISuiteInstalledConsumerTest.cmake
)
set_tests_properties(
    AISuiteInstalledConsumerTest
    PROPERTIES
        LABELS "install;consumer;codex;current-state;user-integrations"
        RUN_SERIAL TRUE
        TIMEOUT 300
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
        LABELS "package;source-package;codex;current-state"
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
        LABELS "package;binary-package;codex;current-state"
        RUN_SERIAL TRUE
        TIMEOUT 300
)

add_subdirectory(policy)
