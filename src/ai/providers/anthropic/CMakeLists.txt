add_library(ai-anthropic SHARED AnthropicProvider.cpp)
add_library(AISuite::Anthropic ALIAS ai-anthropic)
set_target_properties(
    ai-anthropic
    PROPERTIES EXPORT_NAME Anthropic
               VERSION ${PROJECT_VERSION}
               SOVERSION ${PROJECT_VERSION_MAJOR}
)
target_link_libraries(ai-anthropic PUBLIC AISuite::Model AISuite::HttpStreaming)
install(
    TARGETS ai-anthropic
    EXPORT AISuiteTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            COMPONENT aisuite
            NAMELINK_COMPONENT aisuite-dev
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT aisuite-dev
)
install(FILES AnthropicProvider.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aisuite/ai/providers/anthropic
)
