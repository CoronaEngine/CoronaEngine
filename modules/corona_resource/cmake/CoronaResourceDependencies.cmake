# =============================================================================
# CoronaResource dependency checks
#
# Dependencies are resolved by Conan in cmake/corona_third_party.cmake.
# This module keeps resource-local assumptions explicit.
# =============================================================================

include_guard(GLOBAL)

foreach(_target
        ktm::ktm
        assimp::assimp
        stb_headers
        miniaudio_headers
        nlohmann_json::nlohmann_json
        tinyexr_headers
        meshoptimizer
        astcenc-native-static
        TBB::tbb)
    if(NOT TARGET ${_target})
        message(FATAL_ERROR "[CoronaResource] Expected dependency target '${_target}'")
    endif()
endforeach()

unset(_target)
