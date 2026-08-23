find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(AddAISuiteTest)

message(STATUS "AISuite tests are intentionally disabled for codex2 bootstrap")
