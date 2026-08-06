# AISuite - Reusable AI integrations based on SNode.C
# Copyright (C) Volker Christian <me@vchrist.at>
# SPDX-License-Identifier: LGPL-3.0-or-later OR MIT

cmake_minimum_required(VERSION 3.18)

function(fail_self_containment detail)
    message(
        FATAL_ERROR
            "CodexPolicyPublicHeaderSelfContainmentMismatch: ${detail}"
    )
endfunction()

set(required_variables
    AISUITE_PUBLIC_HEADER_POLICY_EXECUTABLE
    AISUITE_SOURCE_DIR
    AISUITE_BUILD_DIR
    AISUITE_CMAKE_COMMAND
    AISUITE_CMAKE_GENERATOR
    AISUITE_CXX_COMPILER
    AISUITE_SNODEC_DIR
)
foreach(variable IN LISTS required_variables)
    if(NOT DEFINED ${variable} OR "${${variable}}" STREQUAL "")
        fail_self_containment("required variable ${variable} is missing")
    endif()
endforeach()
if(NOT EXISTS "${AISUITE_PUBLIC_HEADER_POLICY_EXECUTABLE}")
    fail_self_containment("public-header policy executable is missing")
endif()
if(NOT IS_DIRECTORY "${AISUITE_SOURCE_DIR}" OR
   NOT IS_DIRECTORY "${AISUITE_BUILD_DIR}"
)
    fail_self_containment("AISuite source/build authority is unavailable")
endif()
if(NOT IS_DIRECTORY "${AISUITE_SNODEC_DIR}")
    fail_self_containment("installed SNode.C package directory is unavailable")
endif()

set(inventory_output "")
if(DEFINED AISUITE_INVENTORY_OUTPUT)
    set(inventory_output "${AISUITE_INVENTORY_OUTPUT}")
endif()
math(EXPR last_argument "${CMAKE_ARGC} - 1")
if(last_argument GREATER_EQUAL 0)
    foreach(index RANGE 0 ${last_argument})
        set(argument "${CMAKE_ARGV${index}}")
        if(argument STREQUAL "--inventory-output")
            math(EXPR value_index "${index} + 1")
            if(value_index GREATER last_argument)
                fail_self_containment("--inventory-output requires a path")
            endif()
            set(inventory_output "${CMAKE_ARGV${value_index}}")
        endif()
    endforeach()
endif()

execute_process(
    COMMAND
        "${AISUITE_PUBLIC_HEADER_POLICY_EXECUTABLE}"
        --repo-root
        "${AISUITE_SOURCE_DIR}"
    RESULT_VARIABLE policy_result
    OUTPUT_VARIABLE policy_output
    ERROR_VARIABLE policy_error
)
if(NOT policy_result EQUAL 0)
    fail_self_containment(
        "source policy failed before staging:\n${policy_output}${policy_error}"
    )
endif()

function(
    read_authority
    component
    cmake_relative
    variable
    prefix
    expected_count
    output_headers
    output_lines
)
    file(READ "${AISUITE_SOURCE_DIR}/${cmake_relative}" cmake_source)
    string(REGEX REPLACE "#[^\n]*" "" cmake_source "${cmake_source}")
    string(
        REGEX MATCH
              "set\\([ \t\r\n]*${variable}([ \t\r\n]+[^\\)]*)\\)"
              authority_match
              "${cmake_source}"
    )
    if(NOT authority_match)
        fail_self_containment(
            "cannot derive ${variable} from ${cmake_relative}"
        )
    endif()
    set(authority_body "${CMAKE_MATCH_1}")
    string(REGEX REPLACE "#[^\n]*" "" authority_body "${authority_body}")
    separate_arguments(authority_entries UNIX_COMMAND "${authority_body}")
    set(headers)
    set(lines)
    foreach(entry IN LISTS authority_entries)
        if(NOT entry MATCHES "^[A-Za-z0-9_./+-]+[.]h(h|pp|xx)?$")
            fail_self_containment(
                "${variable} contains unexpected entry '${entry}'"
            )
        endif()
        if(prefix STREQUAL "")
            set(relative "${entry}")
        else()
            set(relative "${prefix}/${entry}")
        endif()
        if(relative MATCHES "(^|/)(detail|private)(/|$)" OR
           relative MATCHES "(^|/)\\.\\.(/|$)"
        )
            fail_self_containment(
                "${variable} exposes private header ${relative}"
            )
        endif()
        if(NOT EXISTS
           "${AISUITE_SOURCE_DIR}/src/ai/openai/codex/${relative}"
        )
            fail_self_containment(
                "${variable} names missing header ${relative}"
            )
        endif()
        list(APPEND headers "${relative}")
        list(APPEND lines "${component}\t${relative}")
    endforeach()
    list(LENGTH headers actual_count)
    if(NOT actual_count EQUAL expected_count)
        fail_self_containment(
            "${component} inventory has ${actual_count} headers; expected ${expected_count}"
        )
    endif()
    set(${output_headers} "${headers}" PARENT_SCOPE)
    set(${output_lines} "${lines}" PARENT_SCOPE)
endfunction()

read_authority(
    main
    "src/ai/openai/codex/CMakeLists.txt"
    AI_OPENAI_CODEX_PUBLIC_H
    ""
    29
    main_headers
    main_lines
)
read_authority(
    backend
    "src/ai/openai/codex/backend/CMakeLists.txt"
    AI_OPENAI_CODEX_BACKEND_PUBLIC_H
    backend
    7
    backend_headers
    backend_lines
)
read_authority(
    frontend
    "src/ai/openai/codex/frontend/CMakeLists.txt"
    AI_OPENAI_CODEX_FRONTEND_PUBLIC_H
    frontend
    9
    frontend_headers
    frontend_lines
)
set(frontend_client_headers)
set(frontend_client_lines)
set(frontend_client_expected_count 0)
if(AISUITE_BUILD_CODEX_FRONTEND_CLIENT)
    read_authority(
        frontend-client
        "src/ai/openai/codex/frontend/client/CMakeLists.txt"
        AI_OPENAI_CODEX_FRONTEND_CLIENT_PUBLIC_H
        frontend/client
        33
        frontend_client_headers
        frontend_client_lines
    )
    set(frontend_client_expected_count 33)
endif()
set(expected_headers
    ${main_headers} ${backend_headers} ${frontend_headers}
    ${frontend_client_headers}
)
set(inventory_lines
    ${main_lines} ${backend_lines} ${frontend_lines}
    ${frontend_client_lines}
)
list(LENGTH expected_headers expected_count)
math(EXPR expected_total "45 + ${frontend_client_expected_count}")
if(NOT expected_count EQUAL expected_total)
    fail_self_containment(
        "derived total is ${expected_count}; expected ${expected_total}"
    )
endif()
list(SORT expected_headers)
set(previous "")
foreach(header IN LISTS expected_headers)
    if(header STREQUAL previous)
        fail_self_containment("derived inventory contains duplicate ${header}")
    endif()
    set(previous "${header}")
endforeach()

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run_id)
set(
    test_root
    "${AISUITE_BUILD_DIR}/Testing/Temporary/CodexPublicHeaderSelfContainment-${run_id}"
)
set(install_prefix "${test_root}/aisuite-install")
set(consumer_source "${test_root}/consumer-source")
set(consumer_build "${test_root}/consumer-build")
file(MAKE_DIRECTORY "${test_root}" "${consumer_source}")

execute_process(
    COMMAND
        "${AISUITE_CMAKE_COMMAND}" -E env --unset=DESTDIR --
        "${AISUITE_CMAKE_COMMAND}" --install "${AISUITE_BUILD_DIR}" --prefix
        "${install_prefix}"
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_output
    ERROR_VARIABLE install_error
)
if(NOT install_result EQUAL 0)
    fail_self_containment(
        "isolated current-build install failed:\n${install_output}${install_error}"
    )
endif()

set(
    installed_root
    "${install_prefix}/include/aisuite/ai/openai/codex"
)
file(
    GLOB_RECURSE installed_headers
    LIST_DIRECTORIES FALSE
    RELATIVE "${installed_root}"
    "${installed_root}/*.h"
    "${installed_root}/*.hh"
    "${installed_root}/*.hpp"
    "${installed_root}/*.hxx"
)
list(SORT installed_headers)
list(LENGTH installed_headers installed_count)
if(NOT installed_count EQUAL expected_total)
    fail_self_containment(
        "installed Codex inventory has ${installed_count} headers; expected ${expected_total}"
    )
endif()
set(previous "")
foreach(header IN LISTS installed_headers)
    if(header STREQUAL previous)
        fail_self_containment(
            "installed inventory contains duplicate ${header}"
        )
    endif()
    if(header MATCHES "(^|/)(detail|private)(/|$)")
        fail_self_containment(
            "installed prefix leaks private header ${header}"
        )
    endif()
    set(previous "${header}")
endforeach()
if(NOT "${installed_headers}" STREQUAL "${expected_headers}")
    set(missing ${expected_headers})
    set(extra ${installed_headers})
    foreach(header IN LISTS installed_headers)
        list(REMOVE_ITEM missing "${header}")
    endforeach()
    foreach(header IN LISTS expected_headers)
        list(REMOVE_ITEM extra "${header}")
    endforeach()
    fail_self_containment(
        "installed inventory differs from CMake authority; missing=[${missing}] extra=[${extra}]"
    )
endif()

file(
    GLOB_RECURSE aisuite_configs
    LIST_DIRECTORIES FALSE
    "${install_prefix}/*/cmake/AISuite/AISuiteConfig.cmake"
)
list(LENGTH aisuite_configs config_count)
if(NOT config_count EQUAL 1)
    fail_self_containment(
        "staged prefix does not contain exactly one AISuiteConfig.cmake"
    )
endif()
list(GET aisuite_configs 0 aisuite_config)
get_filename_component(aisuite_config_dir "${aisuite_config}" DIRECTORY)

file(
    WRITE "${consumer_source}/CMakeLists.txt"
    [=[
cmake_minimum_required(VERSION 3.18)
project(AISuiteCodexPublicHeaderSelfContainment LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(AISuite CONFIG REQUIRED)
]=]
)
set(index 0)
set(translation_units)
set(consumer_link_targets
    "AISuite::OpenAICodex AISuite::OpenAICodexBackend AISuite::OpenAICodexFrontend"
)
if(AISUITE_BUILD_CODEX_FRONTEND_CLIENT)
    string(APPEND consumer_link_targets " AISuite::OpenAICodexFrontendClient")
endif()
foreach(header IN LISTS expected_headers)
    math(EXPR index "${index} + 1")
    set(target "codex_public_header_${index}")
    set(translation_unit "${consumer_source}/${target}.cpp")
    file(
        WRITE "${translation_unit}"
        "#include <ai/openai/codex/${header}>\n"
    )
    file(
        APPEND "${consumer_source}/CMakeLists.txt"
        "add_library(${target} OBJECT \"${translation_unit}\")\n"
        "target_link_libraries(${target} PRIVATE ${consumer_link_targets})\n"
    )
    list(APPEND translation_units "${translation_unit}")
endforeach()

set(
    configure_command
    "${AISUITE_CMAKE_COMMAND}"
    -E
    env
    --unset=CMAKE_PREFIX_PATH
    --unset=CPATH
    --unset=CPLUS_INCLUDE_PATH
    --unset=DESTDIR
    --
    "${AISUITE_CMAKE_COMMAND}"
    -S
    "${consumer_source}"
    -B
    "${consumer_build}"
    -G
    "${AISUITE_CMAKE_GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${AISUITE_CXX_COMPILER}"
    "-DAISuite_DIR=${aisuite_config_dir}"
    "-Dsnodec_DIR=${AISUITE_SNODEC_DIR}"
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
    -DCMAKE_FIND_USE_PACKAGE_REGISTRY=FALSE
    -DCMAKE_FIND_PACKAGE_NO_PACKAGE_REGISTRY=TRUE
    -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=FALSE
)
if(DEFINED AISUITE_NLOHMANN_JSON_DIR AND
   NOT "${AISUITE_NLOHMANN_JSON_DIR}" STREQUAL ""
)
    list(
        APPEND configure_command
        "-Dnlohmann_json_DIR=${AISUITE_NLOHMANN_JSON_DIR}"
    )
endif()
execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error
)
if(NOT configure_result EQUAL 0)
    fail_self_containment(
        "isolated installed-prefix consumer configure failed:\n${configure_output}${configure_error}"
    )
endif()

file(
    STRINGS "${consumer_build}/CMakeCache.txt"
    resolved_aisuite
    REGEX "^AISuite_DIR:"
)
file(
    STRINGS "${consumer_build}/CMakeCache.txt"
    resolved_snodec
    REGEX "^snodec_DIR:"
)
if(NOT "${resolved_aisuite}" MATCHES "=${aisuite_config_dir}$" OR
   NOT "${resolved_snodec}" MATCHES "=${AISUITE_SNODEC_DIR}$"
)
    fail_self_containment(
        "consumer did not resolve the staged AISuite and installed SNode.C packages"
    )
endif()

execute_process(
    COMMAND
        # This test deliberately compiles every installed public header as a
        # separate translation unit. Keep its nested build aligned with the
        # conservative ordinary-suite concurrency so hosted runners do not
        # launch the entire inventory at once.
        "${AISUITE_CMAKE_COMMAND}" --build "${consumer_build}" --parallel 2
        --verbose
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error
)
if(NOT build_result EQUAL 0)
    fail_self_containment(
        "one or more isolated installed public-header translations failed:\n${build_output}${build_error}"
    )
endif()

file(READ "${consumer_build}/compile_commands.json" compile_commands)
string(
    REGEX MATCHALL
          "\"file\"[ \t]*:[ \t]*\"[^\"]+\""
          compile_entries
          "${compile_commands}"
)
list(LENGTH compile_entries compile_count)
if(NOT compile_count EQUAL expected_total)
    fail_self_containment(
        "compile_commands contains ${compile_count} translation units; expected ${expected_total}"
    )
endif()
foreach(translation_unit IN LISTS translation_units)
    string(FIND "${compile_commands}" "${translation_unit}" translation_index)
    if(translation_index EQUAL -1)
        fail_self_containment(
            "compile_commands omits isolated translation ${translation_unit}"
        )
    endif()
endforeach()
string(
    FIND "${compile_commands}"
         "${install_prefix}/include/aisuite"
         installed_include_index
)
if(installed_include_index EQUAL -1)
    fail_self_containment(
        "compile commands do not use the staged AISuite include prefix"
    )
endif()
foreach(
    forbidden
    IN ITEMS "${AISUITE_SOURCE_DIR}/src"
             "${AISUITE_SOURCE_DIR}/tests"
             "${AISUITE_BUILD_DIR}/src"
             "${AISUITE_BUILD_DIR}/include"
)
    foreach(
        marker
        IN ITEMS "-I${forbidden}" "-I ${forbidden}"
                 "-isystem ${forbidden}"
    )
        string(
            FIND "${compile_commands}\n${build_output}\n${build_error}"
                 "${marker}"
                 forbidden_index
        )
        if(NOT forbidden_index EQUAL -1)
            fail_self_containment(
                "compile evidence uses forbidden source/build-tree include path ${forbidden}"
            )
        endif()
    endforeach()
endforeach()

if(NOT inventory_output STREQUAL "")
    get_filename_component(
        inventory_directory "${inventory_output}" DIRECTORY
    )
    if(NOT inventory_directory STREQUAL "")
        file(MAKE_DIRECTORY "${inventory_directory}")
    endif()
    list(SORT inventory_lines)
    file(WRITE "${inventory_output}" "")
    foreach(line IN LISTS inventory_lines)
        file(APPEND "${inventory_output}" "${line}\n")
    endforeach()
endif()
file(REMOVE_RECURSE "${test_root}")
message(
    STATUS
        "Codex public-header self-containment verified: 29 main + 7 backend + 9 frontend + ${frontend_client_expected_count} frontend-client = ${expected_total} isolated installed includes"
)
