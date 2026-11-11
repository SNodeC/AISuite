add_library(ai-http-streaming SHARED StreamingClient.cpp SseDecoder.cpp)
add_library(AISuite::HttpStreaming ALIAS ai-http-streaming)
set_target_properties(
    ai-http-streaming
    PROPERTIES EXPORT_NAME HttpStreaming
               VERSION ${PROJECT_VERSION}
               SOVERSION ${PROJECT_VERSION_MAJOR}
)
target_link_libraries(
    ai-http-streaming
    PUBLIC AISuite::Model
    PRIVATE snodec::http-client snodec::net-in-stream-tls
            snodec::net-in-stream-legacy OpenSSL::SSL OpenSSL::Crypto
)
install(
    TARGETS ai-http-streaming
    EXPORT AISuiteTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            COMPONENT aisuite
            NAMELINK_COMPONENT aisuite-dev
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT aisuite-dev
)
install(FILES StreamingClient.h SseDecoder.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aisuite/ai/http
)
