# Vision is required by Optics independently of the standalone app/test options.
# Run in the parent configure, where CMake can inspect the real build graph.
block()
    foreach(required_target IN ITEMS corona_optics_system vision-all_libs
            ocarina ocarina-backend-vulkan-compute horizon-hotfix copy_slang_headers)
        if(NOT TARGET "${required_target}")
            message(FATAL_ERROR "Missing required Vision target: ${required_target}")
        endif()
    endforeach()

    get_target_property(optics_links corona_optics_system LINK_LIBRARIES)
    if(NOT "vision-all_libs" IN_LIST optics_links)
        message(FATAL_ERROR "Optics must link the required Vision renderer")
    endif()
endblock()
