add_library(
    codex-backend-unix-adapter STATIC
    CodexFrontendSocketContext.cpp
    CodexFrontendSocketContext.h
    CodexFrontendSocketContextFactory.cpp
    CodexFrontendSocketContextFactory.h
    Configuration.cpp
    Configuration.h
    JsonLineFramer.cpp
    JsonLineFramer.h
    ReferenceAuthentication.cpp
    ReferenceAuthentication.h
    UnixPeerCredentials.cpp
    UnixPeerCredentials.h
)
target_include_directories(
    codex-backend-unix-adapter PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
)
target_compile_features(codex-backend-unix-adapter PUBLIC cxx_std_20)
target_link_libraries(
    codex-backend-unix-adapter
    PUBLIC AISuite::OpenAICodexFrontend snodec::net-un-stream-legacy
    PRIVATE snodec::core
)

add_executable(codex-backend main.cpp)
target_compile_features(codex-backend PRIVATE cxx_std_20)
target_link_libraries(
    codex-backend
    PRIVATE
        codex-backend-unix-adapter
        AISuite::OpenAICodex
        AISuite::OpenAICodexBackend
        AISuite::OpenAICodexFrontend
        snodec::net-un-stream-legacy
        snodec::core
)
install(TARGETS codex-backend RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
