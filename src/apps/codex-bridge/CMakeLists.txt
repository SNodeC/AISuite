add_executable(codex-bridge main.cpp)
target_compile_features(codex-bridge PRIVATE cxx_std_20)
target_link_libraries(codex-bridge PRIVATE AISuite::OpenAICodex2 snodec::core)
install(TARGETS codex-bridge RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
