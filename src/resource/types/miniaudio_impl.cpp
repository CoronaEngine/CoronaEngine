// =============================================================================
// miniaudio_impl.cpp
//
// The single translation unit that compiles the miniaudio implementation.
// MA_IMPLEMENTATION must be defined in exactly ONE .cpp across the whole link
// unit; every other site includes <miniaudio.h> for declarations only.
//
// Provides BOTH audio decoding (resource import, Step 1) and device playback
// via ma_engine (acoustics system, Step 2). Only encoding stays disabled — the
// engine never encodes audio through miniaudio (FFmpeg handles export).
// =============================================================================

#ifdef CORONA_RESOURCE_HAVE_MINIAUDIO

// We decode and play, but never encode through miniaudio.
#define MA_NO_ENCODING

#define MA_IMPLEMENTATION
#include <miniaudio.h>

#endif  // CORONA_RESOURCE_HAVE_MINIAUDIO
