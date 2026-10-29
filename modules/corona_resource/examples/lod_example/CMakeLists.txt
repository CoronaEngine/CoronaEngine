# =============================================================================
# LOD Example
# =============================================================================

corona_add_example(
    NAME LODExample
    SOURCES lod_example.cpp
    LIBRARIES corona::resource::manager
    NEEDS_ASSIMP ON
)
