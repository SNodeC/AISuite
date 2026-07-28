cmake_minimum_required(VERSION 3.18)

project(AISuiteInstalledCodexConsumer LANGUAGES CXX)

find_package(AISuite CONFIG REQUIRED)

add_executable(
    AISuiteInstalledSNodeCoreConsumer
    SNodeInstalledCoreConsumer.cpp
)
target_compile_features(
    AISuiteInstalledSNodeCoreConsumer PRIVATE cxx_std_20
)
target_link_libraries(
    AISuiteInstalledSNodeCoreConsumer PRIVATE snodec::core
)

add_executable(AISuiteInstalledCodexConsumer CodexRawConsumer.cpp)
target_compile_features(AISuiteInstalledCodexConsumer PRIVATE cxx_std_20)
target_link_libraries(
    AISuiteInstalledCodexConsumer PRIVATE AISuite::OpenAICodex
)

add_executable(AISuiteInstalledCodexTypedConsumer CodexTypedConsumer.cpp)
target_compile_features(AISuiteInstalledCodexTypedConsumer PRIVATE cxx_std_20)
target_link_libraries(
    AISuiteInstalledCodexTypedConsumer PRIVATE AISuite::OpenAICodex
)

foreach(
    domain
    IN ITEMS Accounts Models Configuration Commands Filesystem
             PermissionProfiles Reviews
)
    add_executable(
        AISuiteInstalledCodex${domain}HeaderConsumer
        Codex${domain}HeaderConsumer.cpp
    )
    target_compile_features(
        AISuiteInstalledCodex${domain}HeaderConsumer PRIVATE cxx_std_20
    )
    target_link_libraries(
        AISuiteInstalledCodex${domain}HeaderConsumer
        PRIVATE AISuite::OpenAICodex
    )
endforeach()

add_executable(
    AISuiteInstalledCodexDeprecatedTypedConsumer
    CodexDeprecatedTypedConsumer.cpp
)
target_compile_features(
    AISuiteInstalledCodexDeprecatedTypedConsumer PRIVATE cxx_std_20
)
target_compile_options(
    AISuiteInstalledCodexDeprecatedTypedConsumer
    PRIVATE -Werror -Wno-deprecated-declarations
)
target_link_libraries(
    AISuiteInstalledCodexDeprecatedTypedConsumer PRIVATE AISuite::OpenAICodex
)

add_executable(
    AISuiteInstalledCodexBackendFrontendConsumer
    CodexBackendFrontendConsumer.cpp
)
target_compile_features(
    AISuiteInstalledCodexBackendFrontendConsumer PRIVATE cxx_std_20
)
target_link_libraries(
    AISuiteInstalledCodexBackendFrontendConsumer
    PRIVATE AISuite::OpenAICodexBackend AISuite::OpenAICodexFrontend
)
