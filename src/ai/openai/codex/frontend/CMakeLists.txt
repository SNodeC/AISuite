set(AI_OPENAI_CODEX_FRONTEND_CPP
    BackendAdapter.cpp Codec.cpp EventCoalescer.cpp EventJournal.cpp
    Messages.cpp UpdateBatch.cpp
)
set(AI_OPENAI_CODEX_FRONTEND_PUBLIC_H
    BackendAdapter.h Codec.h EventCoalescer.h EventJournal.h Messages.h
    Protocol.h UpdateBatch.h
)

add_library(ai-openai-codex-frontend SHARED ${AI_OPENAI_CODEX_FRONTEND_CPP})
add_library(AISuite::OpenAICodexFrontend ALIAS ai-openai-codex-frontend)
set_target_properties(
    ai-openai-codex-frontend PROPERTIES EXPORT_NAME OpenAICodexFrontend
)
target_include_directories(
    ai-openai-codex-frontend
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/aisuite>"
)
target_compile_features(ai-openai-codex-frontend PUBLIC cxx_std_20)
target_link_libraries(
    ai-openai-codex-frontend
    PUBLIC AISuite::OpenAICodexBackend
    PRIVATE snodec::core
)
set_target_properties(
    ai-openai-codex-frontend
    PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${AISUITE_CODEX_SOVERSION}
        OUTPUT_NAME aisuite-openai-codex-frontend
)
install(
    TARGETS ai-openai-codex-frontend
    EXPORT AISuiteTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(
    FILES ${AI_OPENAI_CODEX_FRONTEND_PUBLIC_H}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aisuite/ai/openai/codex/frontend
)
