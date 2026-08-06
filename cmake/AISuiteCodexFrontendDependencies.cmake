function(aisuite_codex_frontend_websocket_client_required output build_apps
         build_tests build_cpp_frontend_client
)
    if(build_tests OR (build_apps AND build_cpp_frontend_client))
        set(required ON)
    else()
        set(required OFF)
    endif()
    set(${output}
        "${required}"
        PARENT_SCOPE
    )
endfunction()
