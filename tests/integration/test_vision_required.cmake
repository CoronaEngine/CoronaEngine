# Vision is required by Optics independently of the optional test targets.
# Run in the parent configure, where CMake can inspect the real build graph.
block()
    foreach(required_target IN ITEMS corona_optics_system vision-all_libs
            ocarina ocarina-backend-vulkan-compute horizon-hotfix copy_slang_headers)
        if(NOT TARGET "${required_target}")
            message(FATAL_ERROR "Missing required Vision target: ${required_target}")
        endif()
    endforeach()

    foreach(retired_target IN ITEMS vision-importer-json vision-importer-assimp
            vision-shape-model vision-gui vision-eval
            vision-pipeline-bake vision-uvunwrapper vision-uvunwrapper-xatlas
            vision-rasterizer vision-rasterizer-cpu vision-ext-xatlas xatlas
            vision-radiancecache vision-radiancecache-sharc)
        if(TARGET "${retired_target}")
            message(FATAL_ERROR "Retired Vision target is still registered: ${retired_target}")
        endif()
    endforeach()

    get_target_property(optics_links corona_optics_system LINK_LIBRARIES)
    if(NOT "vision-all_libs" IN_LIST optics_links)
        message(FATAL_ERROR "Optics must link the required Vision renderer")
    endif()
endblock()
