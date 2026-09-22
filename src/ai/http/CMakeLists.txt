add_library(ai-http-streaming STATIC StreamingClient.cpp SseDecoder.cpp)
add_library(AISuite::HttpStreaming ALIAS ai-http-streaming)
set_target_properties(ai-http-streaming PROPERTIES EXPORT_NAME HttpStreaming)
target_link_libraries(
    ai-http-streaming
    PUBLIC AISuite::Model
    PRIVATE snodec::http-client snodec::net-in-stream-tls
            snodec::net-in-stream-legacy OpenSSL::SSL OpenSSL::Crypto
)
install(
    TARGETS ai-http-streaming
    EXPORT AISuiteTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES StreamingClient.h SseDecoder.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aisuite/ai/http
)
