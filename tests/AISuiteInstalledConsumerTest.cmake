if(NOT DEFINED AISUITE_BUILD_DIR OR NOT DEFINED AISUITE_SOURCE_DIR)
    message(FATAL_ERROR "AISUITE_BUILD_DIR and AISUITE_SOURCE_DIR are required")
endif()
if(NOT DEFINED SNODEC_PREFIX)
    message(FATAL_ERROR "SNODEC_PREFIX is required")
endif()

set(stage "${AISUITE_BUILD_DIR}/installed-consumer-stage")
set(consumer_build "${AISUITE_BUILD_DIR}/installed-consumer-build")
file(REMOVE_RECURSE "${stage}" "${consumer_build}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${AISUITE_BUILD_DIR}" --prefix "${stage}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "AISuite staged install failed: ${result}")
endif()

execute_process(
    COMMAND
        "${CMAKE_COMMAND}"
        -S "${AISUITE_SOURCE_DIR}/tests/installed/codex"
        -B "${consumer_build}"
        -DCMAKE_BUILD_TYPE=Debug
        "-DCMAKE_PREFIX_PATH=${stage};${SNODEC_PREFIX}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "installed consumer configure failed: ${result}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}" --parallel 2
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "installed consumer build failed: ${result}")
endif()
