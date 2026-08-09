if(NOT DEFINED AISUITE_BUILD_DIR OR NOT DEFINED AISUITE_SOURCE_DIR)
    message(
        FATAL_ERROR
            "UserIntegrationInstalledConsumerNotInstalled: AISUITE_BUILD_DIR and AISUITE_SOURCE_DIR are required"
    )
endif()
if(NOT DEFINED AISUITE_BUILD_APPS)
    set(AISUITE_BUILD_APPS ON)
endif()
if(NOT DEFINED AISUITE_BUILD_CODEX_FRONTEND_CLIENT)
    set(AISUITE_BUILD_CODEX_FRONTEND_CLIENT ON)
endif()
if(DEFINED AISUITE_INSTALLED_CONSUMER_TEMP_ROOT AND
   NOT "${AISUITE_INSTALLED_CONSUMER_TEMP_ROOT}" STREQUAL "")
    set(temporary_root "${AISUITE_INSTALLED_CONSUMER_TEMP_ROOT}")
elseif(DEFINED ENV{RUNNER_TEMP} AND NOT "$ENV{RUNNER_TEMP}" STREQUAL "")
    set(temporary_root "$ENV{RUNNER_TEMP}")
elseif(DEFINED ENV{TMPDIR} AND NOT "$ENV{TMPDIR}" STREQUAL "")
    set(temporary_root "$ENV{TMPDIR}")
else()
    get_filename_component(aisuite_source_parent "${AISUITE_SOURCE_DIR}" DIRECTORY)
    set(temporary_root
        "${aisuite_source_parent}/.aisuite-ic"
    )
endif()
string(SHA256 stage_identity "${AISUITE_SOURCE_DIR};${AISUITE_BUILD_DIR}")
string(SUBSTRING "${stage_identity}" 0 16 stage_identity)
set(stage
    "${temporary_root}/ic-${stage_identity}"
)
set(aisuite_install "${stage}/aisuite-install")
set(consumer_source "${stage}/consumer-source")
set(consumer_build "${stage}/consumer-build")
set(module_consumer_source "${stage}/module-consumer-source")
set(module_consumer_build "${stage}/module-consumer-build")
set(runtime_directory "${stage}/r")

file(REMOVE_RECURSE "${stage}")
file(MAKE_DIRECTORY
     "${stage}" "${consumer_source}" "${module_consumer_source}"
     "${runtime_directory}"
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

set(installed_aisuite_config
    "${aisuite_install}/lib/cmake/AISuite/AISuiteConfig.cmake"
)
if(NOT EXISTS "${installed_aisuite_config}")
    fail_installed(
        "installed AISuite package config is missing: ${installed_aisuite_config}"
    )
endif()
file(READ "${installed_aisuite_config}" installed_aisuite_config_text)
string(FIND "${installed_aisuite_config_text}" "COMPONENTS core"
       required_core_component_index
)
if(required_core_component_index EQUAL -1)
    fail_cross_repo(
        "installed AISuite library package must require SNode.C core"
    )
endif()
foreach(
    application_transport_component
    IN ITEMS
       net-un-stream-legacy
       net-in-stream-legacy
       net-in6-stream-legacy
       net-in-stream-tls
       net-in6-stream-tls
       net-rc-stream-legacy
       net-rc-stream-tls
       http-server
       http-server-express
       http-server-express-legacy-in
       http-server-express-legacy-in6
       http-server-express-tls-in
       http-server-express-tls-in6
       websocket-server
       websocket-client
)
    string(FIND "${installed_aisuite_config_text}"
           "${application_transport_component}" transport_component_index
    )
    if(NOT transport_component_index EQUAL -1)
        fail_cross_repo(
            "installed AISuite library package leaked application transport dependency ${application_transport_component}"
        )
    endif()
endforeach()

set(installed_aisuite_targets
    "${aisuite_install}/lib/cmake/AISuite/AISuiteTargets.cmake"
)
if(NOT EXISTS "${installed_aisuite_targets}")
    fail_installed(
        "installed AISuite target export is missing: ${installed_aisuite_targets}"
    )
endif()
file(READ "${installed_aisuite_targets}" installed_aisuite_targets_text)
string(FIND "${installed_aisuite_targets_text}"
       "add_library(AISuite::OpenAICodexFrontend SHARED IMPORTED)"
       frontend_target_index
)
string(FIND "${installed_aisuite_targets_text}"
       "add_library(AISuite::OpenAICodexFrontendProtocol SHARED IMPORTED)"
       frontend_protocol_target_index
)
string(FIND "${installed_aisuite_targets_text}"
       "add_library(AISuite::OpenAICodexFrontendClient SHARED IMPORTED)"
       frontend_client_target_index
)
if(frontend_target_index EQUAL -1)
    fail_installed(
        "installed target boundary must expose OpenAICodexFrontend"
    )
endif()
if(frontend_protocol_target_index EQUAL -1)
    fail_installed(
        "installed target boundary must expose OpenAICodexFrontendProtocol"
    )
endif()
if(AISUITE_BUILD_CODEX_FRONTEND_CLIENT AND
   frontend_client_target_index EQUAL -1)
    fail_installed(
        "SDK-enabled install must expose OpenAICodexFrontendClient"
    )
elseif(NOT AISUITE_BUILD_CODEX_FRONTEND_CLIENT AND
       NOT frontend_client_target_index EQUAL -1)
    fail_installed(
        "SDK-disabled install unexpectedly exposes OpenAICodexFrontendClient"
    )
endif()

set(codex_libraries
    aisuite-openai-codex
    aisuite-openai-codex-backend
    aisuite-openai-codex-frontend-protocol
    aisuite-openai-codex-frontend
)
if(AISUITE_BUILD_CODEX_FRONTEND_CLIENT)
    list(APPEND codex_libraries aisuite-openai-codex-frontend-client)
endif()
foreach(codex_library IN LISTS codex_libraries)
    set(codex_soversion_library
        "${aisuite_install}/lib/lib${codex_library}.so.2"
    )
    set(codex_legacy_soversion_library
        "${aisuite_install}/lib/lib${codex_library}.so.1"
    )
    if(NOT EXISTS "${codex_soversion_library}")
        fail_installed(
            "installed SOVERSION-2 library is missing: ${codex_soversion_library}"
        )
    endif()
    if(EXISTS "${codex_legacy_soversion_library}" OR
       IS_SYMLINK "${codex_legacy_soversion_library}")
        fail_installed(
            "obsolete SOVERSION-1 compatibility library is installed: ${codex_legacy_soversion_library}"
        )
    endif()
    execute_process(
        COMMAND
            ${isolated_environment}
            "${readelf_executable}" -d "${codex_soversion_library}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE codex_dynamic_metadata
        ERROR_VARIABLE error
    )
    require_success(
        "${result}" "${codex_dynamic_metadata}" "${error}"
        "ELF SONAME inspection for ${codex_library}"
    )
    string(
        FIND "${codex_dynamic_metadata}"
        "Library soname: [lib${codex_library}.so.2]"
        codex_soname_index
    )
    if(codex_soname_index EQUAL -1)
        fail_installed(
            "${codex_library} does not declare the required .so.2 SONAME"
        )
    endif()
    if(codex_library STREQUAL "aisuite-openai-codex-frontend")
        set(frontend_protocol_needed FALSE)
        string(REPLACE "\n" ";" codex_dynamic_lines
                       "${codex_dynamic_metadata}"
        )
        foreach(codex_dynamic_line IN LISTS codex_dynamic_lines)
            if(codex_dynamic_line MATCHES
               "\\(NEEDED\\).*Shared library: \\[libaisuite-openai-codex-frontend-protocol\\.so\\.2\\][ \t]*$"
            )
                set(frontend_protocol_needed TRUE)
            endif()
        endforeach()
        if(NOT frontend_protocol_needed)
            fail_installed(
                "installed frontend DSO does not directly need libaisuite-openai-codex-frontend-protocol.so.2"
            )
        endif()
    endif()
endforeach()

if(AISUITE_BUILD_APPS AND AISUITE_BUILD_CODEX_FRONTEND_CLIENT)
    set(installed_frontend_client
        "${aisuite_install}/bin/codex-backend-client"
    )
    if(NOT EXISTS "${installed_frontend_client}")
        fail_installed(
            "SDK-enabled application install is missing ${installed_frontend_client}"
        )
    endif()
    execute_process(
        COMMAND
            ${isolated_environment}
            "${readelf_executable}" -d "${installed_frontend_client}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE frontend_client_dynamic_metadata
        ERROR_VARIABLE error
    )
    require_success(
        "${result}" "${frontend_client_dynamic_metadata}" "${error}"
        "codex-backend-client ELF dependency inspection"
    )
    string(
        FIND "${frontend_client_dynamic_metadata}"
        "Shared library: [libaisuite-openai-codex-frontend-client.so.2]"
        frontend_client_sdk_needed
    )
    if(frontend_client_sdk_needed EQUAL -1)
        fail_installed(
            "installed codex-backend-client does not record the canonical Frontend SDK as a direct runtime dependency"
        )
    endif()
endif()

file(
    COPY "${AISUITE_SOURCE_DIR}/tests/installed/codex/"
    DESTINATION "${consumer_source}"
)
file(
    COPY "${AISUITE_SOURCE_DIR}/tests/installed/codex-module-server/"
    DESTINATION "${module_consumer_source}"
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

execute_process(
    COMMAND
        ${isolated_environment}
        "${CMAKE_COMMAND}"
        -G Ninja
        -S "${module_consumer_source}"
        -B "${module_consumer_build}"
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
    "isolated installed AISuite/SNode.C module-server configure"
)
execute_process(
    COMMAND
        ${isolated_environment}
        "${CMAKE_COMMAND}" --build "${module_consumer_build}" --parallel 4
        --verbose
    RESULT_VARIABLE result
    OUTPUT_VARIABLE module_consumer_build_output
    ERROR_VARIABLE module_consumer_build_error
)
require_success(
    "${result}" "${module_consumer_build_output}"
    "${module_consumer_build_error}"
    "isolated installed AISuite/SNode.C module-server build"
)
file(READ "${module_consumer_build}/compile_commands.json"
     module_compile_commands
)
reject_forbidden_references(
    "${module_compile_commands}" "module-server compile commands"
)
reject_forbidden_references(
    "${module_consumer_build_output}\n${module_consumer_build_error}"
    "module-server verbose build/link evidence"
)
foreach(required_prefix IN ITEMS "${aisuite_install}" "${snodec_install}")
    string(FIND "${module_compile_commands}\n${module_consumer_build_output}"
           "${required_prefix}" required_prefix_index
    )
    if(required_prefix_index EQUAL -1)
        fail_cross_repo(
            "module-server build does not use staged prefix: ${required_prefix}"
        )
    endif()
endforeach()

set(installed_consumers
    AISuiteInstalledSNodeCoreConsumer
    AISuiteInstalledCodexConsumer
    AISuiteInstalledCodexApiConsumer
    AISuiteInstalledCodexFrontendProtocolConsumer
    AISuiteInstalledCodexAccountsHeaderConsumer
    AISuiteInstalledCodexModelsHeaderConsumer
    AISuiteInstalledCodexConfigurationHeaderConsumer
    AISuiteInstalledCodexCommandsHeaderConsumer
    AISuiteInstalledCodexFilesystemHeaderConsumer
    AISuiteInstalledCodexPermissionProfilesHeaderConsumer
    AISuiteInstalledCodexReviewsHeaderConsumer
    AISuiteInstalledCodexApplicationProjectionConsumer
    AISuiteInstalledCodexBackendFrontendConsumer
)
if(AISUITE_BUILD_CODEX_FRONTEND_CLIENT)
    list(APPEND installed_consumers AISuiteInstalledCodexFrontendClientConsumer)
endif()
set(saw_aisuite_main_runtime_library FALSE)
set(saw_aisuite_backend_runtime_library FALSE)
set(saw_aisuite_frontend_runtime_library FALSE)
set(saw_aisuite_frontend_client_runtime_library FALSE)
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
        string(FIND "${linked_libraries}" "${aisuite_install}/lib"
               linked_aisuite_index
        )
        if(linked_aisuite_index EQUAL -1)
            fail_cross_repo(
                "${consumer} did not resolve AISuite Codex libraries from ${aisuite_install}"
            )
        endif()
        string(FIND "${linked_libraries}" "libaisuite-openai-codex.so.1"
               linked_legacy_main
        )
        string(FIND "${linked_libraries}"
               "libaisuite-openai-codex-backend.so.1"
               linked_legacy_backend
        )
        string(FIND "${linked_libraries}"
               "libaisuite-openai-codex-frontend.so.1"
               linked_legacy_frontend
        )
        string(FIND "${linked_libraries}"
               "libaisuite-openai-codex-frontend-client.so.1"
               linked_legacy_frontend_client
        )
        if(NOT linked_legacy_main EQUAL -1 OR
           NOT linked_legacy_backend EQUAL -1 OR
           NOT linked_legacy_frontend EQUAL -1 OR
           NOT linked_legacy_frontend_client EQUAL -1)
            fail_installed(
                "${consumer} resolved an obsolete AISuite Codex .so.1 runtime"
            )
        endif()
        string(FIND "${linked_libraries}" "libaisuite-openai-codex.so.2"
               linked_main
        )
        string(FIND "${linked_libraries}"
               "libaisuite-openai-codex-backend.so.2"
               linked_backend
        )
        string(FIND "${linked_libraries}"
               "libaisuite-openai-codex-frontend.so.2"
               linked_frontend
        )
        string(FIND "${linked_libraries}"
               "libaisuite-openai-codex-frontend-client.so.2"
               linked_frontend_client
        )
        if(NOT linked_main EQUAL -1)
            set(saw_aisuite_main_runtime_library TRUE)
        endif()
        if(NOT linked_backend EQUAL -1)
            set(saw_aisuite_backend_runtime_library TRUE)
        endif()
        if(NOT linked_frontend EQUAL -1)
            set(saw_aisuite_frontend_runtime_library TRUE)
        endif()
        if(NOT linked_frontend_client EQUAL -1)
            set(saw_aisuite_frontend_client_runtime_library TRUE)
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
    if(consumer STREQUAL "AISuiteInstalledCodexFrontendClientConsumer")
        foreach(
            forbidden_sdk_runtime
            IN ITEMS
               libsnodec-net-
               libsnodec-http
               libsnodec-websocket
               libssl
               libcrypto
               libbluetooth
        )
            string(FIND "${linked_libraries}" "${forbidden_sdk_runtime}"
                   forbidden_sdk_runtime_index
            )
            if(NOT forbidden_sdk_runtime_index EQUAL -1)
                fail_cross_repo(
                    "transport-neutral SDK consumer resolves forbidden runtime ${forbidden_sdk_runtime}: ${linked_libraries}"
                )
            endif()
        endforeach()
    endif()
    if(consumer STREQUAL "AISuiteInstalledCodexFrontendProtocolConsumer")
        foreach(
            forbidden_protocol_runtime
            IN ITEMS
               libaisuite-openai-codex.so.2
               libaisuite-openai-codex-backend.so.2
               libaisuite-openai-codex-frontend.so.2
               libaisuite-openai-codex-frontend-client.so.2
               libsnodec-
               libssl
               libcrypto
               libbluetooth
        )
            string(FIND "${linked_libraries}" "${forbidden_protocol_runtime}"
                   forbidden_protocol_runtime_index
            )
            if(NOT forbidden_protocol_runtime_index EQUAL -1)
                fail_cross_repo(
                    "protocol-only consumer resolves forbidden runtime ${forbidden_protocol_runtime}: ${linked_libraries}"
                )
            endif()
        endforeach()
        string(FIND "${linked_libraries}"
               "libaisuite-openai-codex-frontend-protocol.so.2"
               protocol_runtime_index
        )
        if(protocol_runtime_index EQUAL -1)
            fail_installed(
                "protocol-only consumer does not resolve the protocol DSO"
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

set(module_consumer_executable
    "${module_consumer_build}/AISuiteInstalledCodexModuleServer"
)
if(NOT EXISTS "${module_consumer_executable}")
    fail_installed(
        "module-server consumer executable is missing: ${module_consumer_executable}"
    )
endif()
execute_process(
    COMMAND
        ${isolated_environment}
        "LD_LIBRARY_PATH=${aisuite_install}/lib:${snodec_install}/lib"
        "${ldd_executable}" "${module_consumer_executable}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE module_linked_libraries
    ERROR_VARIABLE error
)
require_success(
    "${result}" "${module_linked_libraries}" "${error}"
    "linked-library inspection for installed module-server consumer"
)
reject_forbidden_references(
    "${module_linked_libraries}"
    "installed module-server linked-library evidence"
)
foreach(
    required_library
    IN ITEMS
       libaisuite-openai-codex.so.2
       libaisuite-openai-codex-backend.so.2
       libaisuite-openai-codex-frontend.so.2
       libsnodec-core
       libsnodec-net-un-stream-legacy
)
    string(FIND "${module_linked_libraries}" "${required_library}"
           required_library_index
    )
    if(required_library_index EQUAL -1)
        fail_cross_repo(
            "module-server runtime does not resolve required installed library: ${required_library}"
        )
    endif()
endforeach()
execute_process(
    COMMAND
        ${isolated_environment}
        "LD_LIBRARY_PATH=${aisuite_install}/lib:${snodec_install}/lib"
        "${module_consumer_executable}"
    WORKING_DIRECTORY "${runtime_directory}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
require_success(
    "${result}" "${output}" "${error}"
    "sanitized installed AISuite/SNode.C module-server runtime"
)
if(NOT saw_aisuite_main_runtime_library OR
   NOT saw_aisuite_backend_runtime_library OR
   NOT saw_aisuite_frontend_runtime_library OR
   (AISUITE_BUILD_CODEX_FRONTEND_CLIENT AND
    NOT saw_aisuite_frontend_client_runtime_library) OR
   NOT saw_snodec_runtime_library)
    fail_cross_repo(
        "consumer runtime proof did not observe every configured AISuite Codex .so.2 library and SNode.C from their isolated install prefixes"
    )
endif()

message(
    STATUS
        "Installed boundary passed: snodec_install=${snodec_install}; aisuite_install=${aisuite_install}; AISuite_DIR=${aisuite_dir}; snodec_DIR=${snodec_dir}; consumer_build=${consumer_build}; module_consumer_build=${module_consumer_build}"
)
