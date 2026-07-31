if(NOT DEFINED AISUITE_BUILD_DIR OR NOT DEFINED AISUITE_SOURCE_DIR)
    message(
        FATAL_ERROR
            "UserIntegrationInstalledConsumerNotInstalled: AISUITE_BUILD_DIR and AISUITE_SOURCE_DIR are required"
    )
endif()
if(DEFINED AISUITE_INSTALLED_CONSUMER_TEMP_ROOT AND
   NOT "${AISUITE_INSTALLED_CONSUMER_TEMP_ROOT}" STREQUAL "")
    set(temporary_root "${AISUITE_INSTALLED_CONSUMER_TEMP_ROOT}")
elseif(DEFINED ENV{RUNNER_TEMP} AND NOT "$ENV{RUNNER_TEMP}" STREQUAL "")
    set(temporary_root "$ENV{RUNNER_TEMP}")
elseif(DEFINED ENV{TMPDIR} AND NOT "$ENV{TMPDIR}" STREQUAL "")
    set(temporary_root "$ENV{TMPDIR}")
else()
    set(temporary_root "/tmp")
endif()
string(SHA256 stage_identity "${AISUITE_SOURCE_DIR};${AISUITE_BUILD_DIR}")
string(SUBSTRING "${stage_identity}" 0 16 stage_identity)
set(stage
    "${temporary_root}/aisuite-genuine-installed-consumer-${stage_identity}"
)
set(aisuite_install "${stage}/aisuite-install")
set(consumer_source "${stage}/consumer-source")
set(consumer_build "${stage}/consumer-build")
set(runtime_directory "${stage}/runtime")

file(REMOVE_RECURSE "${stage}")
file(MAKE_DIRECTORY
     "${stage}" "${consumer_source}" "${runtime_directory}"
)

function(fail_installed message_text)
    message(
        FATAL_ERROR
            "UserIntegrationInstalledConsumerNotInstalled: ${message_text}"
    )
endfunction()

function(fail_cross_repo message_text)
    message(
        FATAL_ERROR
            "UserIntegrationCrossRepoDependencyMismatch: ${message_text}"
    )
endfunction()

function(require_success result output error description)
    if(NOT "${result}" STREQUAL "0")
        fail_installed(
            "${description} failed (${result})\nstdout:\n${output}\nstderr:\n${error}"
        )
    endif()
endfunction()

function(require_path_under actual expected_prefix description)
    file(REAL_PATH "${actual}" actual_real)
    file(REAL_PATH "${expected_prefix}" expected_real)
    string(FIND "${actual_real}/" "${expected_real}/" prefix_index)
    if(NOT prefix_index EQUAL 0)
        fail_cross_repo(
            "${description} resolved outside its install prefix: ${actual_real}; expected under ${expected_real}"
        )
    endif()
endfunction()

function(reject_forbidden_references evidence description)
    foreach(forbidden_path IN LISTS forbidden_evidence_paths)
        if("${forbidden_path}" STREQUAL "")
            continue()
        endif()
        file(TO_CMAKE_PATH "${forbidden_path}" forbidden_normalized)
        string(FIND "${evidence}" "${forbidden_normalized}" reference_index)
        if(NOT reference_index EQUAL -1)
            fail_cross_repo(
                "${description} refers to forbidden source/build/stage path: ${forbidden_normalized}"
            )
        endif()
    endforeach()
endfunction()

# Every configure, build, install, inspection, and runtime command starts
# without inherited compiler, linker, package-discovery, or loader paths.
# Explicit paths supplied below are therefore the only non-system boundary.
set(isolated_environment
    "${CMAKE_COMMAND}" -E env
    "--unset=CC"
    "--unset=CXX"
    "--unset=CPPFLAGS"
    "--unset=CFLAGS"
    "--unset=CXXFLAGS"
    "--unset=LDFLAGS"
    "--unset=LD_LIBRARY_PATH"
    "--unset=LIBRARY_PATH"
    "--unset=CPATH"
    "--unset=C_INCLUDE_PATH"
    "--unset=CPLUS_INCLUDE_PATH"
    "--unset=OBJC_INCLUDE_PATH"
    "--unset=CMAKE_PREFIX_PATH"
    "--unset=CMAKE_MODULE_PATH"
    "--unset=CMAKE_TOOLCHAIN_FILE"
    "--unset=PKG_CONFIG_PATH"
    "--unset=PKG_CONFIG_LIBDIR"
    "--unset=PKG_CONFIG_SYSROOT_DIR"
    "--unset=GIT_DIR"
    "--unset=GIT_WORK_TREE"
    "--unset=GIT_CEILING_DIRECTORIES"
)

file(REAL_PATH "${AISUITE_SOURCE_DIR}" aisuite_source_real)
file(REAL_PATH "${AISUITE_BUILD_DIR}" aisuite_outer_build_real)
file(
    STRINGS "${AISUITE_BUILD_DIR}/CMakeCache.txt" snodec_package_dir_line
    REGEX "^snodec_DIR:"
)
if(NOT snodec_package_dir_line)
    fail_cross_repo(
        "the configured AISuite build does not record its installed SNode.C package"
    )
endif()
string(
    REGEX REPLACE "^[^=]*=" "" SNODEC_PACKAGE_DIR
    "${snodec_package_dir_line}"
)
file(REAL_PATH "${SNODEC_PACKAGE_DIR}" snodec_package_dir_real)
get_filename_component(snodec_install "${snodec_package_dir_real}" DIRECTORY)
get_filename_component(snodec_install "${snodec_install}" DIRECTORY)
get_filename_component(snodec_install "${snodec_install}" DIRECTORY)
if(NOT EXISTS "${snodec_package_dir_real}/snodecConfig.cmake" OR
   NOT EXISTS "${snodec_install}/include/snode.c" OR
   NOT EXISTS "${snodec_install}/lib")
    fail_cross_repo(
        "configured SNode.C package is not a complete installed prefix: ${snodec_package_dir_real}"
    )
endif()
set(forbidden_evidence_paths
    "${aisuite_source_real}"
    "${aisuite_outer_build_real}"
)

# Remember non-system absolute paths inherited through variables that are
# scrubbed above. The configured installed SNode.C prefix is the one intentional
# exception because it remains the sole dependency installation under test.
foreach(
    environment_name
    IN ITEMS
       LDFLAGS
       LD_LIBRARY_PATH
       LIBRARY_PATH
       CPATH
       C_INCLUDE_PATH
       CPLUS_INCLUDE_PATH
       CMAKE_PREFIX_PATH
       CMAKE_MODULE_PATH
       PKG_CONFIG_PATH
)
    if(DEFINED ENV{${environment_name}} AND
       NOT "$ENV{${environment_name}}" STREQUAL "")
        string(
            REGEX MATCHALL "/[^,;: \t]+"
            inherited_absolute_paths
            "$ENV{${environment_name}}"
        )
        foreach(inherited_path IN LISTS inherited_absolute_paths)
            file(TO_CMAKE_PATH "${inherited_path}" inherited_normalized)
            file(TO_CMAKE_PATH "${snodec_install}" snodec_install_normalized)
            string(
                FIND "${inherited_normalized}/"
                "${snodec_install_normalized}/" intended_snodec_index
            )
            if(NOT inherited_path MATCHES "^/(usr|lib|lib64)(/|$)" AND
               NOT intended_snodec_index EQUAL 0)
                list(APPEND forbidden_evidence_paths "${inherited_path}")
            endif()
        endforeach()
    endif()
endforeach()
list(REMOVE_DUPLICATES forbidden_evidence_paths)

find_program(readelf_executable readelf REQUIRED)
find_program(ldd_executable ldd REQUIRED)

file(
    GLOB_RECURSE snodec_installed_codex_artifacts
    LIST_DIRECTORIES FALSE
    "${snodec_install}/include/*/ai/openai/codex/*"
    "${snodec_install}/lib/*ai-openai-codex*"
)
if(snodec_installed_codex_artifacts)
    fail_cross_repo(
        "installed SNode.C package contains duplicate Codex headers or libraries: ${snodec_installed_codex_artifacts}"
    )
endif()

execute_process(
    COMMAND
        ${isolated_environment}
        "${CMAKE_COMMAND}" --install "${AISUITE_BUILD_DIR}" --prefix
        "${aisuite_install}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
require_success(
    "${result}" "${output}" "${error}"
    "install from the one configured AISuite build"
)

file(
    COPY "${AISUITE_SOURCE_DIR}/tests/installed/codex/"
    DESTINATION "${consumer_source}"
)
execute_process(
    COMMAND
        ${isolated_environment}
        "${CMAKE_COMMAND}"
        -G Ninja
        -S "${consumer_source}"
        -B "${consumer_build}"
        -DCMAKE_BUILD_TYPE=Debug
        "-DCMAKE_PREFIX_PATH=${aisuite_install};${snodec_install}"
        "-DCMAKE_EXE_LINKER_FLAGS=-Wl,-rpath-link,${snodec_install}/lib"
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
        -DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE
        -DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=TRUE
        -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
require_success(
    "${result}" "${output}" "${error}"
    "isolated installed consumer configure"
)

file(STRINGS "${consumer_build}/CMakeCache.txt" aisuite_dir_line
     REGEX "^AISuite_DIR:"
)
file(STRINGS "${consumer_build}/CMakeCache.txt" snodec_dir_line
     REGEX "^snodec_DIR:"
)
if(NOT aisuite_dir_line OR NOT snodec_dir_line)
    fail_cross_repo(
        "consumer cache does not contain both AISuite_DIR and snodec_DIR"
    )
endif()
string(REGEX REPLACE "^[^=]*=" "" aisuite_dir "${aisuite_dir_line}")
string(REGEX REPLACE "^[^=]*=" "" snodec_dir "${snodec_dir_line}")
require_path_under("${aisuite_dir}" "${aisuite_install}" "AISuite_DIR")
require_path_under("${snodec_dir}" "${snodec_install}" "snodec_DIR")

execute_process(
    COMMAND
        ${isolated_environment}
        "${CMAKE_COMMAND}" --build "${consumer_build}" --parallel 4 --verbose
    RESULT_VARIABLE result
    OUTPUT_VARIABLE consumer_build_output
    ERROR_VARIABLE consumer_build_error
)
require_success(
    "${result}" "${consumer_build_output}" "${consumer_build_error}"
    "isolated installed consumer build"
)

file(READ "${consumer_build}/compile_commands.json" compile_commands)
reject_forbidden_references(
    "${compile_commands}" "consumer compile commands"
)
string(FIND "${compile_commands}" "${aisuite_install}/include"
       aisuite_include_index
)
if(aisuite_include_index EQUAL -1)
    fail_cross_repo(
        "consumer compile commands do not use the installed AISuite include prefix"
    )
endif()
string(
    FIND "${compile_commands}" "${snodec_install}/include/snode.c"
    snodec_include_index
)
if(snodec_include_index EQUAL -1)
    fail_cross_repo(
        "consumer compile commands do not use the installed SNode.C include prefix"
    )
endif()
reject_forbidden_references(
    "${consumer_build_output}\n${consumer_build_error}"
    "verbose consumer build/link evidence"
)
string(FIND "${consumer_build_output}" "${aisuite_install}/lib"
       aisuite_link_index
)
if(aisuite_link_index EQUAL -1)
    fail_cross_repo(
        "verbose consumer link commands do not use the installed AISuite library prefix"
    )
endif()
string(FIND "${consumer_build_output}" "${snodec_install}/lib"
       snodec_link_index
)
if(snodec_link_index EQUAL -1)
    fail_cross_repo(
        "verbose consumer link commands do not use the installed SNode.C library prefix"
    )
endif()
set(expected_snodec_rpath_link
    "-Wl,-rpath-link,${snodec_install}/lib"
)
string(
    FIND "${consumer_build_output}" "${expected_snodec_rpath_link}"
    snodec_rpath_link_index
)
if(snodec_rpath_link_index EQUAL -1)
    fail_cross_repo(
        "consumer linker did not resolve indirect dependencies from the installed SNode.C prefix"
    )
endif()

set(installed_consumers
    AISuiteInstalledSNodeCoreConsumer
    AISuiteInstalledCodexConsumer
    AISuiteInstalledCodexTypedConsumer
    AISuiteInstalledCodexAccountsHeaderConsumer
    AISuiteInstalledCodexModelsHeaderConsumer
    AISuiteInstalledCodexConfigurationHeaderConsumer
    AISuiteInstalledCodexCommandsHeaderConsumer
    AISuiteInstalledCodexFilesystemHeaderConsumer
    AISuiteInstalledCodexPermissionProfilesHeaderConsumer
    AISuiteInstalledCodexReviewsHeaderConsumer
    AISuiteInstalledCodexDeprecatedTypedConsumer
    AISuiteInstalledCodexBackendFrontendConsumer
)
set(saw_aisuite_runtime_library FALSE)
set(saw_snodec_runtime_library FALSE)
foreach(consumer IN LISTS installed_consumers)
    set(executable "${consumer_build}/${consumer}")
    if(NOT EXISTS "${executable}")
        fail_installed("consumer executable is missing: ${executable}")
    endif()

    execute_process(
        COMMAND
            ${isolated_environment}
            "${readelf_executable}" -d "${executable}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE dynamic_metadata
        ERROR_VARIABLE error
    )
    require_success(
        "${result}" "${dynamic_metadata}" "${error}"
        "ELF dynamic metadata inspection for ${consumer}"
    )
    reject_forbidden_references(
        "${dynamic_metadata}" "${consumer} ELF dynamic metadata"
    )

    execute_process(
        COMMAND
            ${isolated_environment}
            "LD_LIBRARY_PATH=${aisuite_install}/lib:${snodec_install}/lib"
            "${ldd_executable}" "${executable}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE linked_libraries
        ERROR_VARIABLE error
    )
    require_success(
        "${result}" "${linked_libraries}" "${error}"
        "linked-library inspection for ${consumer}"
    )
    reject_forbidden_references(
        "${linked_libraries}" "${consumer} linked-library evidence"
    )
    string(FIND "${linked_libraries}" "libaisuite-openai-codex"
           uses_aisuite
    )
    if(NOT uses_aisuite EQUAL -1)
        set(saw_aisuite_runtime_library TRUE)
        string(FIND "${linked_libraries}" "${aisuite_install}/lib"
               linked_aisuite_index
        )
        if(linked_aisuite_index EQUAL -1)
            fail_cross_repo(
                "${consumer} did not resolve AISuite Codex libraries from ${aisuite_install}"
            )
        endif()
    endif()
    string(FIND "${linked_libraries}" "libsnodec-" uses_snodec)
    if(NOT uses_snodec EQUAL -1)
        set(saw_snodec_runtime_library TRUE)
        string(FIND "${linked_libraries}" "${snodec_install}/lib"
               linked_snodec_index
        )
        if(linked_snodec_index EQUAL -1)
            fail_cross_repo(
                "${consumer} did not resolve SNode.C libraries from ${snodec_install}"
            )
        endif()
    endif()
    if(consumer STREQUAL "AISuiteInstalledSNodeCoreConsumer")
        string(FIND "${linked_libraries}" "libsnodec-core" direct_snodec_core)
        string(
            FIND "${linked_libraries}" "${snodec_install}/lib"
            direct_snodec_prefix
        )
        if(direct_snodec_core EQUAL -1 OR direct_snodec_prefix EQUAL -1)
            fail_cross_repo(
                "direct installed SNode.C consumer did not resolve snodec::core from ${snodec_install}"
            )
        endif()
    endif()

    execute_process(
        COMMAND
            ${isolated_environment}
            "LD_LIBRARY_PATH=${aisuite_install}/lib:${snodec_install}/lib"
            "${executable}"
        WORKING_DIRECTORY "${runtime_directory}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    require_success(
        "${result}" "${output}" "${error}"
        "sanitized installed runtime for ${consumer}"
    )
endforeach()
if(NOT saw_aisuite_runtime_library OR NOT saw_snodec_runtime_library)
    fail_cross_repo(
        "consumer runtime proof did not observe libraries from both isolated install prefixes"
    )
endif()

message(
    STATUS
        "Installed boundary passed: snodec_install=${snodec_install}; aisuite_install=${aisuite_install}; AISuite_DIR=${aisuite_dir}; snodec_DIR=${snodec_dir}; consumer_build=${consumer_build}"
)
