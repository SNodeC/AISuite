add_library(ai-agent INTERFACE)
add_library(AISuite::Agent ALIAS ai-agent)
set_target_properties(ai-agent PROPERTIES EXPORT_NAME Agent)
target_compile_features(ai-agent INTERFACE cxx_std_20)
target_include_directories(
    ai-agent
    INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
              "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/aisuite>"
)
install(TARGETS ai-agent EXPORT AISuiteTargets)
install(FILES Runtime.h Agent.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aisuite/ai/agent
)
