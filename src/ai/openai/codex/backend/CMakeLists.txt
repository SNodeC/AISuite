set(AI_OPENAI_CODEX_BACKEND_CPP
    BackendCommand.cpp BackendCore.cpp BackendState.cpp Reducer.cpp Snapshot.cpp
)
set(AI_OPENAI_CODEX_BACKEND_PUBLIC_H
    BackendCommand.h BackendCore.h BackendEvent.h BackendState.h
    FrontendSession.h Reducer.h Snapshot.h
)

add_library(ai-openai-codex-backend SHARED ${AI_OPENAI_CODEX_BACKEND_CPP})
add_library(AISuite::OpenAICodexBackend ALIAS ai-openai-codex-backend)
set_target_properties(
    ai-openai-codex-backend PROPERTIES EXPORT_NAME OpenAICodexBackend
)
target_include_directories(
    ai-openai-codex-backend
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/aisuite>"
)
target_compile_features(ai-openai-codex-backend PUBLIC cxx_std_20)
target_link_libraries(
    ai-openai-codex-backend PUBLIC ai-openai-codex PRIVATE core
)
set_target_properties(
    ai-openai-codex-backend
    PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${AISUITE_CODEX_SOVERSION}
        OUTPUT_NAME aisuite-openai-codex-backend
)
install(
    TARGETS ai-openai-codex-backend
    EXPORT AISuiteTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(
    FILES ${AI_OPENAI_CODEX_BACKEND_PUBLIC_H}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aisuite/ai/openai/codex/backend
)
