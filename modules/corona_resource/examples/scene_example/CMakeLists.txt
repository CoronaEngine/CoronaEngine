# =============================================================================
# Scene Example
# =============================================================================

corona_add_example(
    NAME SceneExample
    SOURCES scene_example.cpp
    LIBRARIES corona::resource::manager stb_headers
    NEEDS_ASSIMP ON
)

