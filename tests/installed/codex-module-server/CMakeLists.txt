cmake_minimum_required(VERSION 3.18)

project(AISuiteInstalledCodexModuleServer LANGUAGES CXX)

find_package(snodec CONFIG REQUIRED COMPONENTS core net-un-stream-legacy)
find_package(AISuite CONFIG REQUIRED)

add_executable(AISuiteInstalledCodexModuleServer CodexModuleServer.cpp)
target_compile_features(AISuiteInstalledCodexModuleServer PRIVATE cxx_std_20)
target_link_libraries(
    AISuiteInstalledCodexModuleServer
    PRIVATE AISuite::OpenAICodex AISuite::OpenAICodexBackend
            AISuite::OpenAICodexFrontend snodec::core
            snodec::net-un-stream-legacy
)
