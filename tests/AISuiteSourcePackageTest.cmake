if(NOT DEFINED AISUITE_BUILD_DIR OR NOT DEFINED AISUITE_SOURCE_DIR)
    message(FATAL_ERROR "AISUITE_BUILD_DIR and AISUITE_SOURCE_DIR are required")
endif()
if(NOT DEFINED CMAKE_CPACK_COMMAND)
    set(CMAKE_CPACK_COMMAND cpack)
endif()

file(GLOB old_archives "${AISUITE_BUILD_DIR}/AISuite-*.tar.gz")
if(old_archives)
    file(REMOVE ${old_archives})
endif()
execute_process(
    COMMAND "${CMAKE_CPACK_COMMAND}" --config "${AISUITE_BUILD_DIR}/CPackSourceConfig.cmake" -G TGZ
    WORKING_DIRECTORY "${AISUITE_BUILD_DIR}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "AISuite source package failed: ${result}")
endif()
file(GLOB archives "${AISUITE_BUILD_DIR}/AISuite-*.tar.gz")
list(LENGTH archives archive_count)
if(NOT archive_count EQUAL 1)
    message(FATAL_ERROR "expected one source archive, found ${archive_count}")
endif()
list(GET archives 0 archive)
set(extract "${AISUITE_BUILD_DIR}/source-package-extract")
file(REMOVE_RECURSE "${extract}")
file(MAKE_DIRECTORY "${extract}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar xzf "${archive}"
    WORKING_DIRECTORY "${extract}"
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "unable to extract source package")
endif()
file(GLOB roots "${extract}/AISuite-*")
list(GET roots 0 root)
foreach(required
    CMakeLists.txt
    src/ai/openai/codex/AppServerClient.cpp
    src/apps/codex-backend/main.cpp
    src/apps/codex-backend-client/main.cpp
    tools/codex/app_server_a1_4.py
    tools/codex/app-server-schema/0.144.6/PROVENANCE.json
    tools/codex/app-server-evidence/0.144.6/a1-4-implementation-plan.json
    tests/component/codex/CodexA14AuditToolTest.py
    docs/extraction/README.md
    docs/extraction/filter-map.json
    docs/extraction/source-manifest.json
    docs/extraction/validation.md
    docs/extraction/test-integrity.md
)
    if(NOT EXISTS "${root}/${required}")
        message(FATAL_ERROR "source package missing ${required}")
    endif()
endforeach()
execute_process(
    COMMAND python3 -B "${root}/tools/codex/app_server_a1_4.py" check --repo-root "${root}" --base-sha 21e557d9019f334ff0b1e2c29aacd3c6b0795463
    RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "A1.4 audit failed inside source package: ${result}")
endif()
