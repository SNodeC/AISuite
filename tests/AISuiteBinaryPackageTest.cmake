if(NOT DEFINED AISUITE_BUILD_DIR)
    message(FATAL_ERROR "AISUITE_BUILD_DIR is required")
endif()
if(NOT DEFINED CMAKE_CPACK_COMMAND)
    set(CMAKE_CPACK_COMMAND cpack)
endif()

file(GLOB old_archives "${AISUITE_BUILD_DIR}/AISuite-*.tar.gz")
if(old_archives)
    file(REMOVE ${old_archives})
endif()
execute_process(
    COMMAND "${CMAKE_CPACK_COMMAND}" --config "${AISUITE_BUILD_DIR}/CPackConfig.cmake" -G TGZ
    WORKING_DIRECTORY "${AISUITE_BUILD_DIR}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "AISuite binary package failed: ${result}")
endif()
file(GLOB archives "${AISUITE_BUILD_DIR}/AISuite-*.tar.gz")
list(LENGTH archives archive_count)
if(NOT archive_count EQUAL 1)
    message(FATAL_ERROR "expected one binary archive, found ${archive_count}")
endif()
list(GET archives 0 archive)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar tf "${archive}"
    OUTPUT_VARIABLE listing
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "unable to list binary package")
endif()
foreach(required
    "include/aisuite/ai/openai/codex/AppServerClient.h"
    "include/aisuite/ai/openai/codex/typed/Apps.h"
    "include/aisuite/ai/openai/codex/typed/ExternalAgents.h"
    "include/aisuite/ai/openai/codex/typed/Feedback.h"
    "include/aisuite/ai/openai/codex/typed/Hooks.h"
    "include/aisuite/ai/openai/codex/typed/Marketplace.h"
    "include/aisuite/ai/openai/codex/typed/Plugins.h"
    "include/aisuite/ai/openai/codex/typed/Skills.h"
    "lib/libaisuite-openai-codex.so.1"
    "lib/libaisuite-openai-codex-backend.so.1"
    "lib/libaisuite-openai-codex-frontend.so.1"
    "bin/codex-backend"
    "bin/codex-backend-client"
    "lib/cmake/AISuite/AISuiteConfig.cmake"
)
    string(FIND "${listing}" "${required}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "binary package missing ${required}")
    endif()
endforeach()
foreach(forbidden
    "tools/codex/"
    "tests/component/codex/"
    "src/ai/openai/codex/detail/"
    "ProtocolSurfaceRegistryData.inc"
)
    string(FIND "${listing}" "${forbidden}" found)
    if(NOT found EQUAL -1)
        message(FATAL_ERROR "binary package contains private path ${forbidden}")
    endif()
endforeach()
