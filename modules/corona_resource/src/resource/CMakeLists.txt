# CoronaResource - Scene Resource Manager
# =============================================================================

set(CORONA_RESOURCE_MANAGER_SOURCE_FILES
    ### top-level source files ###
    resource_manager.cpp
    resource.cpp
    parser_registry.cpp
    resource_cache.cpp
    ### cache ###
    cache/lru_cache.cpp
    ### resource types ###
    # animation
    types/animation.cpp
    # skeletal animation runtime pose evaluation (P1)
    types/animation_pose.cpp
    # audio
    types/audio.cpp
    # miniaudio implementation TU (single MA_IMPLEMENTATION site)
    types/miniaudio_impl.cpp
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
    ${CORONA_RESOURCE_ROOT}/include/corona/resource/resource.h
    ${CORONA_RESOURCE_ROOT}/include/corona/resource/resource_manager.h
    ${CORONA_RESOURCE_ROOT}/include/corona/resource/parser_registry.h
    ${CORONA_RESOURCE_ROOT}/include/corona/resource/resource_cache.h
    ### resource types ###
    # animation
    ${CORONA_RESOURCE_ROOT}/include/corona/resource/types/animation.h
    # skeletal animation runtime pose evaluation (P1)
    ${CORONA_RESOURCE_ROOT}/include/corona/resource/types/animation_pose.h
    # audio
    ${CORONA_RESOURCE_ROOT}/include/corona/resource/types/audio.h
    # binary
    ${CORONA_RESOURCE_ROOT}/include/corona/resource/types/binary.h
    # image
    ${CORONA_RESOURCE_ROOT}/include/corona/resource/types/image.h
    # json
    ${CORONA_RESOURCE_ROOT}/include/corona/resource/types/json.h
    # scene
    ${CORONA_RESOURCE_ROOT}/include/corona/resource/types/scene.h
    # video
    ${CORONA_RESOURCE_ROOT}/include/corona/resource/types/video.h
    # text
    ${CORONA_RESOURCE_ROOT}/include/corona/resource/types/text.h
    # model
    ${CORONA_RESOURCE_ROOT}/include/corona/resource/types/model.h
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
    $<BUILD_INTERFACE:${CORONA_RESOURCE_ROOT}/include>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include/corona_resource/resource_manager>
)

# Link dependencies
target_link_libraries(CoronaResourceManager
    PRIVATE
    stb_headers
    miniaudio_headers
    assimp::assimp
    meshoptimizer
    astcenc-native-static
    tinyexr_headers
    PUBLIC
    Horizon
    TBB::tbb
    ktm::ktm
)

# Optional FFmpeg-backed video/audio import/export
if(CORONA_RESOURCE_HAVE_FFMPEG)
    target_link_libraries(CoronaResourceManager PRIVATE corona::ffmpeg)
    target_compile_definitions(CoronaResourceManager PRIVATE CORONA_RESOURCE_HAVE_FFMPEG=1)
    message(STATUS "[CoronaResource] FFmpeg video/audio support: ENABLED")
else()
    message(STATUS "[CoronaResource] FFmpeg video/audio support: DISABLED")
endif()

# miniaudio-backed audio decoding/import (header-only, always available)
target_compile_definitions(CoronaResourceManager PRIVATE CORONA_RESOURCE_HAVE_MINIAUDIO=1)
message(STATUS "[CoronaResource] miniaudio audio import: ENABLED")
