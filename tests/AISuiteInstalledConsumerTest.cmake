if(NOT DEFINED AISUITE_BUILD_DIR OR NOT DEFINED AISUITE_SOURCE_DIR)
    message(
        FATAL_ERROR
            "UserIntegrationInstalledConsumerNotInstalled: AISUITE_BUILD_DIR and AISUITE_SOURCE_DIR are required"
    )
endif()
if(NOT DEFINED SNODEC_SOURCE_REPOSITORY OR
   "${SNODEC_SOURCE_REPOSITORY}" STREQUAL "")
    message(
        FATAL_ERROR
            "UserIntegrationInstalledConsumerNotInstalled: set AISUITE_TEST_SNODEC_SOURCE_REPOSITORY to a local clone containing the pinned SNode.C commit"
    )
endif()

set(expected_snodec_commit
    "d18b231a1d2ec2235fd6f204786b0a761cc24ff5"
)
set(expected_snodec_tree
    "88a63edc985a851b2b76b0c56df19fae74ea8069"
)
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
set(snodec_archive "${stage}/snodec-source.tar")
set(snodec_source "${stage}/snodec-source")
set(snodec_build "${stage}/snodec-build")
set(snodec_install "${stage}/snodec-install")
set(aisuite_build "${stage}/aisuite-build")
set(aisuite_install "${stage}/aisuite-install")
set(consumer_source "${stage}/consumer-source")
set(consumer_build "${stage}/consumer-build")
set(runtime_directory "${stage}/runtime")

file(REMOVE_RECURSE "${stage}")
file(MAKE_DIRECTORY
     "${stage}" "${snodec_source}" "${consumer_source}" "${runtime_directory}"
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
file(REAL_PATH "${SNODEC_SOURCE_REPOSITORY}" snodec_outer_source_real)
set(forbidden_evidence_paths
    "${aisuite_source_real}"
    "${aisuite_outer_build_real}"
    "${snodec_outer_source_real}"
    "${snodec_source}"
    "${snodec_build}"
    "${aisuite_build}"
)

# Capture any outer SNode.C package/staging prefix selected by the parent
# AISuite configure so that the fresh consumer cannot silently reuse it.
if(EXISTS "${AISUITE_BUILD_DIR}/CMakeCache.txt")
    file(
        STRINGS "${AISUITE_BUILD_DIR}/CMakeCache.txt"
        outer_snodec_dir_line
        REGEX "^snodec_DIR:"
    )
    if(outer_snodec_dir_line)
        string(
            REGEX REPLACE "^[^=]*=" "" outer_snodec_dir
            "${outer_snodec_dir_line}"
        )
        if(IS_ABSOLUTE "${outer_snodec_dir}")
            list(APPEND forbidden_evidence_paths "${outer_snodec_dir}")
            get_filename_component(
                outer_snodec_prefix "${outer_snodec_dir}" DIRECTORY
            )
            get_filename_component(
                outer_snodec_prefix "${outer_snodec_prefix}" DIRECTORY
            )
            get_filename_component(
                outer_snodec_prefix "${outer_snodec_prefix}" DIRECTORY
            )
            list(APPEND forbidden_evidence_paths "${outer_snodec_prefix}")
        endif()
    endif()
    file(
        STRINGS "${AISUITE_BUILD_DIR}/CMakeCache.txt"
        outer_prefix_path_line
        REGEX "^CMAKE_PREFIX_PATH:"
    )
    if(outer_prefix_path_line)
        string(
            REGEX REPLACE "^[^=]*=" "" outer_prefix_paths
            "${outer_prefix_path_line}"
        )
        foreach(outer_prefix IN LISTS outer_prefix_paths)
            if(IS_ABSOLUTE "${outer_prefix}")
                list(APPEND forbidden_evidence_paths "${outer_prefix}")
            endif()
        endforeach()
    endif()
endif()

# Remember non-system absolute paths inherited through variables that are
# scrubbed above. Evidence must not contain any of these former search roots.
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
            if(NOT inherited_path MATCHES "^/(usr|lib|lib64)(/|$)")
                list(APPEND forbidden_evidence_paths "${inherited_path}")
            endif()
        endforeach()
    endif()
endforeach()
list(REMOVE_DUPLICATES forbidden_evidence_paths)

find_program(git_executable git REQUIRED)
find_program(readelf_executable readelf REQUIRED)
find_program(ldd_executable ldd REQUIRED)

execute_process(
    COMMAND
        ${isolated_environment}
        "${git_executable}" -C "${SNODEC_SOURCE_REPOSITORY}" status
        "--porcelain=v1" "--untracked-files=all"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE snodec_status_before
    ERROR_VARIABLE error
)
require_success(
    "${result}" "${snodec_status_before}" "${error}"
    "SNode.C worktree status capture"
)
execute_process(
    COMMAND
        ${isolated_environment}
        "${git_executable}" -C "${SNODEC_SOURCE_REPOSITORY}" rev-parse
        "--verify" "${expected_snodec_commit}^{commit}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE verified_snodec_commit
    ERROR_VARIABLE error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
require_success(
    "${result}" "${verified_snodec_commit}" "${error}"
    "pinned SNode.C commit verification"
)
if(NOT verified_snodec_commit STREQUAL expected_snodec_commit)
    fail_cross_repo(
        "the supplied SNode.C repository does not resolve the pinned commit exactly"
    )
endif()
execute_process(
    COMMAND
        ${isolated_environment}
        "${git_executable}" -C "${SNODEC_SOURCE_REPOSITORY}" rev-parse
        "${expected_snodec_commit}^{tree}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE verified_snodec_tree
    ERROR_VARIABLE error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
require_success(
    "${result}" "${verified_snodec_tree}" "${error}"
    "pinned SNode.C tree verification"
)
if(NOT verified_snodec_tree STREQUAL expected_snodec_tree)
    fail_cross_repo(
        "the supplied SNode.C commit has an unexpected source tree"
    )
endif()

# Exporting the pinned tree leaves the supplied SNode.C clone byte-for-byte
# untouched and gives the consumer gate a source directory disjoint from every
# pre-existing checkout/build/install directory.
execute_process(
    COMMAND
        ${isolated_environment}
        "${git_executable}" -C "${SNODEC_SOURCE_REPOSITORY}" archive
        "--format=tar" "--output=${snodec_archive}" "${expected_snodec_commit}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
require_success(
    "${result}" "${output}" "${error}" "pinned SNode.C source export"
)
execute_process(
    COMMAND
        ${isolated_environment}
        "${CMAKE_COMMAND}" -E tar xf "${snodec_archive}"
    WORKING_DIRECTORY "${snodec_source}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
require_success(
    "${result}" "${output}" "${error}" "pinned SNode.C source extraction"
)

execute_process(
    COMMAND
        ${isolated_environment}
        "${CMAKE_COMMAND}"
        -G Ninja
        -S "${snodec_source}"
        -B "${snodec_build}"
        -DCMAKE_BUILD_TYPE=Debug
        "-DCMAKE_INSTALL_PREFIX=${snodec_install}"
        -DSNODEC_BUILD_TESTS=OFF
        -DSNODEC_BUILD_APPS=OFF
        -DCHECK_INCLUDES=OFF
        -DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE
        -DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=TRUE
        -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
require_success(
    "${result}" "${output}" "${error}" "fresh pinned SNode.C configure"
)
execute_process(
    COMMAND
        ${isolated_environment}
        "${CMAKE_COMMAND}" --build "${snodec_build}" --target
        net-un-stream-legacy --parallel 4
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
require_success(
    "${result}" "${output}" "${error}"
    "fresh pinned SNode.C dependency-target build"
)
set(snodec_install_components
    logger
    utils
    mux-epoll
    core
    core-socket
    core-socket-stream
    core-socket-stream-legacy
    net
    net-un
    net-un-phy
    net-un-phy-stream
    # The pinned SNode.C tree assigns only this target export to the
    # historical misspelled component; installing it repairs the staged
    # package without modifying the pinned dependency.
    net-un-sphy-tream
    net-un-stream
    net-un-stream-legacy
)
foreach(component IN LISTS snodec_install_components)
    execute_process(
        COMMAND
            ${isolated_environment}
            "${CMAKE_COMMAND}" --install "${snodec_build}" --component
            "${component}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error
    )
    require_success(
        "${result}" "${output}" "${error}"
        "fresh pinned SNode.C ${component} component install"
    )
endforeach()

file(
    GLOB_RECURSE snodec_installed_codex_artifacts
    LIST_DIRECTORIES FALSE
    "${snodec_install}/include/*/ai/openai/codex/*"
    "${snodec_install}/lib/*ai-openai-codex*"
)
if(snodec_installed_codex_artifacts)
    fail_cross_repo(
        "pinned SNode.C installed duplicate Codex headers or libraries: ${snodec_installed_codex_artifacts}"
    )
endif()

execute_process(
    COMMAND
        ${isolated_environment}
        "${CMAKE_COMMAND}"
        -G Ninja
        -S "${AISUITE_SOURCE_DIR}"
        -B "${aisuite_build}"
        -DCMAKE_BUILD_TYPE=Debug
        "-DCMAKE_PREFIX_PATH=${snodec_install}"
        "-DCMAKE_INSTALL_PREFIX=${aisuite_install}"
        -DAISUITE_BUILD_TESTS=OFF
        -DAISUITE_BUILD_APPS=OFF
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
    "fresh AISuite configure against only installed SNode.C"
)
execute_process(
    COMMAND
        ${isolated_environment}
        "${CMAKE_COMMAND}" --build "${aisuite_build}" --target all --parallel 4
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
require_success("${result}" "${output}" "${error}" "fresh AISuite build")
execute_process(
    COMMAND
        ${isolated_environment}
        "${CMAKE_COMMAND}" --install "${aisuite_build}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
require_success("${result}" "${output}" "${error}" "fresh AISuite install")

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

execute_process(
    COMMAND
        ${isolated_environment}
        "${git_executable}" -C "${SNODEC_SOURCE_REPOSITORY}" status
        "--porcelain=v1" "--untracked-files=all"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE snodec_status_after
    ERROR_VARIABLE error
)
require_success(
    "${result}" "${snodec_status_after}" "${error}"
    "SNode.C worktree status recheck"
)
if(NOT snodec_status_after STREQUAL snodec_status_before)
    fail_cross_repo("the installed-consumer gate modified SNode.C")
endif()

message(
    STATUS
        "UserIntegration installed boundary passed: pinned_snodec=${expected_snodec_commit}; pinned_tree=${expected_snodec_tree}; snodec_install=${snodec_install}; aisuite_install=${aisuite_install}; AISuite_DIR=${aisuite_dir}; snodec_DIR=${snodec_dir}; consumer_build=${consumer_build}"
)
