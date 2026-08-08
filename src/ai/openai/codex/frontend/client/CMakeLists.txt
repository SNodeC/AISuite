set(
    AI_OPENAI_CODEX_FRONTEND_CLIENT_CPP
    BindingMetadata.cpp
    Client.cpp
    GeneratedFacades.cpp
    OperationCodecs.cpp
    ProjectionFingerprint.cpp
    State.cpp
)
set(
    AI_OPENAI_CODEX_FRONTEND_CLIENT_PUBLIC_H
    Accounts.h
    Apps.h
    Changes.h
    Client.h
    Commands.h
    Configuration.h
    Connection.h
    Controller.h
    Export.h
    ExternalAgents.h
    Feedback.h
    Filesystem.h
    GeneratedBindings.h
    Hooks.h
    Marketplace.h
    Mcp.h
    Models.h
    PermissionProfiles.h
    Plugins.h
    Provider.h
    ProjectionFingerprint.h
    Requests.h
    Results.h
    Reviews.h
    Skills.h
    State.h
    StateTypes.h
    Synchronization.h
    Threads.h
    Transport.h
    Turns.h
    Types.h
    WindowsSandbox.h
)

add_library(
    ai-openai-codex-frontend-client-objects OBJECT
    ${AI_OPENAI_CODEX_FRONTEND_CLIENT_CPP}
)
set_target_properties(
    ai-openai-codex-frontend-client-objects
    PROPERTIES
        POSITION_INDEPENDENT_CODE ON
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
)
target_include_directories(
    ai-openai-codex-frontend-client-objects
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_compile_features(ai-openai-codex-frontend-client-objects PRIVATE cxx_std_20)
target_compile_definitions(
    ai-openai-codex-frontend-client-objects
    PRIVATE AISUITE_OPENAI_CODEX_FRONTEND_CLIENT_BUILDING_LIBRARY=1
)
target_link_libraries(
    ai-openai-codex-frontend-client-objects
    PRIVATE AISuite::OpenAICodexFrontend
)

add_library(
    ai-openai-codex-frontend-client SHARED
    $<TARGET_OBJECTS:ai-openai-codex-frontend-client-objects>
)
add_library(
    AISuite::OpenAICodexFrontendClient ALIAS
    ai-openai-codex-frontend-client
)
set_target_properties(
    ai-openai-codex-frontend-client
    PROPERTIES
        EXPORT_NAME OpenAICodexFrontendClient
        VERSION ${PROJECT_VERSION}
        SOVERSION ${AISUITE_CODEX_SOVERSION}
        OUTPUT_NAME aisuite-openai-codex-frontend-client
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
)
target_include_directories(
    ai-openai-codex-frontend-client
    PUBLIC
        "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/aisuite>"
)
target_compile_features(ai-openai-codex-frontend-client PUBLIC cxx_std_20)
target_link_libraries(
    ai-openai-codex-frontend-client
    PUBLIC AISuite::OpenAICodexFrontend
)

if(AISUITE_BUILD_TESTS)
    add_library(
        ai-openai-codex-frontend-client-test-support STATIC
        $<TARGET_OBJECTS:ai-openai-codex-frontend-client-objects>
    )
    target_include_directories(
        ai-openai-codex-frontend-client-test-support
        PUBLIC "${PROJECT_SOURCE_DIR}/src"
    )
    target_compile_features(
        ai-openai-codex-frontend-client-test-support
        PUBLIC cxx_std_20
    )
    target_compile_definitions(
        ai-openai-codex-frontend-client-test-support
        INTERFACE AISUITE_OPENAI_CODEX_FRONTEND_CLIENT_STATIC_DEFINE=1
    )
    target_link_libraries(
        ai-openai-codex-frontend-client-test-support
        PUBLIC AISuite::OpenAICodexFrontend
    )
endif()
install(
    TARGETS ai-openai-codex-frontend-client
    EXPORT AISuiteTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(
    FILES ${AI_OPENAI_CODEX_FRONTEND_CLIENT_PUBLIC_H}
    DESTINATION
        ${CMAKE_INSTALL_INCLUDEDIR}/aisuite/ai/openai/codex/frontend/client
)
