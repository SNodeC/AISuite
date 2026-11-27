set(CPACK_DEBIAN_AISUITE-DEV_PACKAGE_DEPENDS
    "aisuite (= ${CPACK_PACKAGE_VERSION}), snodec, libssl-dev, nlohmann-json3-dev"
)
set(CPACK_RPM_AISUITE-DEV_PACKAGE_REQUIRES
    "aisuite = ${CPACK_PACKAGE_VERSION}-${CPACK_RPM_PACKAGE_RELEASE}, snodec, openssl-devel, json-devel"
)
# Exact native dependencies record the wave's frozen build inputs.
if(DEFINED ENV{DEPENDENCIES_JSON})
    set(dependencies "$ENV{DEPENDENCIES_JSON}")
    string(JSON count LENGTH "${dependencies}")
    if(NOT count EQUAL 1)
        message(FATAL_ERROR "Expected the resolved snodec dependency")
    endif()
    string(JSON package GET "${dependencies}" 0 package)
    string(JSON version GET "${dependencies}" 0 version)
    string(JSON revision GET "${dependencies}" 0 revision)
    if(NOT package STREQUAL "snodec")
        message(FATAL_ERROR "Unexpected resolved dependency: ${package}")
    endif()
    if(CPACK_GENERATOR STREQUAL "DEB")
        string(JSON codename GET "$ENV{TARGET_JSON}" codename)
        set(bound "snodec (= ${version}-${revision}~${codename})")
        set(runtime CPACK_DEBIAN_AISUITE_PACKAGE_DEPENDS)
        set(development CPACK_DEBIAN_AISUITE-DEV_PACKAGE_DEPENDS)
    else()
        string(JSON disttag GET "$ENV{TARGET_JSON}" disttag)
        set(bound "snodec = ${version}-${revision}.${disttag}")
        set(runtime CPACK_RPM_AISUITE_PACKAGE_REQUIRES)
        set(development CPACK_RPM_AISUITE-DEV_PACKAGE_REQUIRES)
    endif()
    if(NOT "${${runtime}}" STREQUAL "")
        string(APPEND ${runtime} ", ")
    endif()
    string(APPEND ${runtime} "${bound}")
    string(REPLACE "snodec" "${bound}" ${development} "${${development}}")
endif()
file(MAKE_DIRECTORY "${CPACK_OUTPUT_FILE_PREFIX}")
file(
    WRITE "${CPACK_OUTPUT_FILE_PREFIX}/aisuite.packages"
    "${CPACK_DEBIAN_AISUITE_PACKAGE_NAME}\n${CPACK_DEBIAN_AISUITE-DEV_PACKAGE_NAME}\n"
)
