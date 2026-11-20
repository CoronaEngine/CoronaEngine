# Vision is required by Optics independently of the optional test targets.
# Run in the parent configure, where CMake can inspect the real build graph.
block()
    foreach(required_target IN ITEMS corona_optics_system vision-all_libs
            vision-base corona-hotfix-runtime
            ocarina ocarina-backend-vulkan-compute copy_slang_headers)
        if(NOT TARGET "${required_target}")
            message(FATAL_ERROR "Missing required Vision target: ${required_target}")
        endif()
    endforeach()

    get_target_property(runtime_type corona-hotfix-runtime TYPE)
    if(NOT runtime_type STREQUAL "SHARED_LIBRARY")
        message(FATAL_ERROR "Vision hotfix runtime must have one shared owner")
    endif()
    foreach(helper horizon-hotfix-build-cmake horizon-hotfix-process-windows
            horizon-hotfix-process-posix horizon-logging)
        if(TARGET "${helper}")
            get_target_property(helper_type "${helper}" TYPE)
            get_target_property(helper_pic "${helper}" POSITION_INDEPENDENT_CODE)
            if(helper_type STREQUAL "STATIC_LIBRARY" AND NOT helper_pic)
                message(FATAL_ERROR "Shared hotfix runtime requires PIC helper: ${helper}")
            endif()
        endif()
    endforeach()
    get_target_property(base_links vision-base LINK_LIBRARIES)
    if(NOT "corona-hotfix-runtime" IN_LIST base_links)
        message(FATAL_ERROR "Vision base must link the shared hotfix runtime")
    endif()
    # Upstream may define its static runtime, but production Vision must not
    # acquire a second singleton/registry copy through any linked dependency.
    set(pending_targets vision-base)
    set(checked_targets)
    while(pending_targets)
        list(POP_FRONT pending_targets linked_target)
        if(linked_target IN_LIST checked_targets)
            continue()
        endif()
        list(APPEND checked_targets "${linked_target}")
        foreach(property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
            get_target_property(links "${linked_target}" "${property}")
            foreach(link IN LISTS links)
                if(link MATCHES "(^|[^A-Za-z0-9_-])horizon-hotfix(-all)?([^A-Za-z0-9_-]|$)")
                    message(FATAL_ERROR "Vision must not link the upstream static hotfix runtime: ${linked_target} -> ${link}")
                endif()
                if(TARGET "${link}")
                    list(APPEND pending_targets "${link}")
                endif()
            endforeach()
        endforeach()
    endwhile()

    foreach(retired_target IN ITEMS vision-importer-json vision-importer-assimp
            vision-shape-model vision-gui vision-eval
            vision-pipeline-bake vision-uvunwrapper vision-uvunwrapper-xatlas
            vision-rasterizer vision-rasterizer-cpu vision-ext-xatlas xatlas
            vision-radiancecache vision-radiancecache-sharc
            vision-upsampler vision-upsampler-bilateral vision-lightsampler-bvh
            vision-pipeline-customized vision-render_graph vision-pass
            vision-pass-integrate vision-pass-accumulate vision-pass-tonemapping
            vision-pass-gamma test-render_graph)
        if(TARGET "${retired_target}")
            message(FATAL_ERROR "Retired Vision target is still registered: ${retired_target}")
        endif()
    endforeach()

    get_target_property(optics_links corona_optics_system LINK_LIBRARIES)
    if(NOT "vision-all_libs" IN_LIST optics_links)
        message(FATAL_ERROR "Optics must link the required Vision renderer")
    endif()
endblock()
