# =============================================================================
# Media Example (FFmpeg video / audio import + export)
# =============================================================================

corona_add_example(
    NAME MediaExample
    SOURCES media_example.cpp
    LIBRARIES corona::resource::manager
)

# Copy FFmpeg (and other transitive) runtime DLLs next to the binary.
if(CORONA_RESOURCE_HAVE_FFMPEG)
    corona_resource_copy_runtime_dlls(MediaExample)
endif()
