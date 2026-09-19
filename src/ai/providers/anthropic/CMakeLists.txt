add_library(ai-anthropic STATIC AnthropicProvider.cpp)
add_library(AISuite::Anthropic ALIAS ai-anthropic)
set_target_properties(ai-anthropic PROPERTIES EXPORT_NAME Anthropic)
target_link_libraries(ai-anthropic PUBLIC AISuite::Model AISuite::HttpStreaming)
install(
    TARGETS ai-anthropic
    EXPORT AISuiteTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES AnthropicProvider.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aisuite/ai/providers/anthropic
)
