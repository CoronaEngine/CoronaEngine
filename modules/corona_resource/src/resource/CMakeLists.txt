# CoronaResource - Scene Resource Manager
# =============================================================================

set(CORONA_RESOURCE_MANAGER_SOURCE_FILES
    ### top-level source files ###
    resource_manager.cpp
    resource.cpp
    parser_registry.cpp
    resource_cache.cpp
    ### resource types ###
    # animation
    types/animation.cpp
    # audio
    types/audio.cpp
    # image
    types/image.cpp
    # binary
    types/binary.cpp
    # json
    types/json.cpp
    # scene
    types/parse_assimp.h
    types/scene.cpp
    # video
    types/video.cpp
    # audio / video shared ffmpeg helpers
    types/ffmpeg_common.h
    # model
    types/model.cpp
    # text
    types/text.cpp
)

set(CORONA_RESOURCE_MANAGER_HEADER_FILES
    ### top-level header files ###
    ${PROJECT_SOURCE_DIR}/include/corona/resource/resource.h
    ${PROJECT_SOURCE_DIR}/include/corona/resource/resource_manager.h
    ${PROJECT_SOURCE_DIR}/include/corona/resource/parser_registry.h
    ${PROJECT_SOURCE_DIR}/include/corona/resource/resource_cache.h
    ### resource types ###
    # animation
    ${PROJECT_SOURCE_DIR}/include/corona/resource/types/animation.h
    # audio
    ${PROJECT_SOURCE_DIR}/include/corona/resource/types/audio.h
    # binary
    ${PROJECT_SOURCE_DIR}/include/corona/resource/types/binary.h
    # image
    ${PROJECT_SOURCE_DIR}/include/corona/resource/types/image.h
    # json
    ${PROJECT_SOURCE_DIR}/include/corona/resource/types/json.h
    # scene
    ${PROJECT_SOURCE_DIR}/include/corona/resource/types/scene.h
    # video
    ${PROJECT_SOURCE_DIR}/include/corona/resource/types/video.h
    # text
    ${PROJECT_SOURCE_DIR}/include/corona/resource/types/text.h
    # model
    ${PROJECT_SOURCE_DIR}/include/corona/resource/types/model.h
)

# Create static library
add_library(CoronaResourceManager STATIC
    ${CORONA_RESOURCE_MANAGER_SOURCE_FILES}
    ${CORONA_RESOURCE_MANAGER_HEADER_FILES}
)
add_library(corona::resource::manager ALIAS CoronaResourceManager)

# Set C++20 standard
target_compile_features(CoronaResourceManager PUBLIC cxx_std_20)

# Configure include directories
target_include_directories(CoronaResourceManager
    PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/corona_resource/resource_manager>

    ${ktm_SOURCE_DIR}
    PRIVATE
    ${tinyexr_SOURCE_DIR}
    ${astc-encoder_SOURCE_DIR}/Source
)

# Link dependencies
target_link_libraries(CoronaResourceManager
    PRIVATE
    stb_headers
    assimp::assimp
    meshoptimizer
    astcenc-native-static
    PUBLIC
    corona::kernel
    TBB::tbb
)

# Optional FFmpeg-backed video/audio import/export
if(CORONA_RESOURCE_HAVE_FFMPEG)
    target_link_libraries(CoronaResourceManager PRIVATE corona::ffmpeg)
    target_compile_definitions(CoronaResourceManager PRIVATE CORONA_RESOURCE_HAVE_FFMPEG=1)
    message(STATUS "[CoronaResource] FFmpeg video/audio support: ENABLED")
else()
    message(STATUS "[CoronaResource] FFmpeg video/audio support: DISABLED")
endif()
