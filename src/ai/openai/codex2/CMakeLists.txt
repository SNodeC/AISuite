add_library(ai-openai-codex2 INTERFACE)
add_library(AISuite::OpenAICodex2 ALIAS ai-openai-codex2)
target_include_directories(
    ai-openai-codex2
    INTERFACE
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/aisuite>"
)
target_compile_features(ai-openai-codex2 INTERFACE cxx_std_20)
target_link_libraries(
    ai-openai-codex2 INTERFACE nlohmann_json::nlohmann_json snodec::core
)
install(
    TARGETS ai-openai-codex2
    EXPORT AISuiteTargets
)
