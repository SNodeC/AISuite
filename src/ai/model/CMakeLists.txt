add_library(ai-model INTERFACE)
add_library(AISuite::Model ALIAS ai-model)
set_target_properties(ai-model PROPERTIES EXPORT_NAME Model)
target_compile_features(ai-model INTERFACE cxx_std_20)
target_include_directories(
    ai-model
    INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>"
              "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/aisuite>"
)
target_link_libraries(ai-model INTERFACE nlohmann_json::nlohmann_json)
install(TARGETS ai-model EXPORT AISuiteTargets)
install(FILES Provider.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/aisuite/ai/model
)
