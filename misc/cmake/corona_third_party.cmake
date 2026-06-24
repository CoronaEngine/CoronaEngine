# ============================================================================== 
# corona_third_party.cmake
#
# Purpose:
#   Declare and fetch external dependencies using `FetchContent`.
#
# Notes:
#   - Centralizes source-level dependencies required by the engine and examples.
#   - Uses parallel capable FetchContent at configure time.
#
# Tips:
#   - Pin `GIT_TAG` values to specific commits or release versions to lock
#     dependency versions where stability is preferred.
# ============================================================================== 

include_guard(GLOBAL)

include(FetchContent)
include(corona_tbb_package)

# ------------------------------------------------------------------------------
# Helper: strip MSVC long-form UTF-8 charset flags from a target's
# INTERFACE_COMPILE_OPTIONS so they are not forwarded to consumers that already
# receive the short-form /utf-8 from corona_compile_config.cmake. MSVC treats
# /utf-8 and /source-charset:utf-8 (or /execution-charset:utf-8) as mutually
# exclusive (D8016), so the two cannot coexist on a single compile command.
#
# The target's own COMPILE_OPTIONS are intentionally left untouched: the target
# is built in its own directory scope, which is processed BEFORE
# corona_compile_config.cmake is included, so /utf-8 is not present there.
# ------------------------------------------------------------------------------
function(corona_strip_msvc_charset_interface target_name)
    if(NOT MSVC OR NOT TARGET ${target_name})
        return()
    endif()
    get_target_property(_iface_opts ${target_name} INTERFACE_COMPILE_OPTIONS)
    if(NOT _iface_opts)
        return()
    endif()
    list(REMOVE_ITEM _iface_opts
        "/source-charset:utf-8"
        "/execution-charset:utf-8"
    )
    set_target_properties(${target_name} PROPERTIES
        INTERFACE_COMPILE_OPTIONS "${_iface_opts}"
    )
endfunction()

# ------------------------------------------------------------------------------
# Core dependency declarations
# ------------------------------------------------------------------------------
FetchContent_Declare(Horizon
    GIT_REPOSITORY https://github.com/CoronaEngine/Horizon.git
    GIT_TAG f850f17a30abe4bbbc9c5a90f4e7c20167c10be5
    EXCLUDE_FROM_ALL
)

FetchContent_Declare(assimp
    GIT_REPOSITORY https://github.com/assimp/assimp.git
    GIT_TAG master
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
)

FetchContent_Declare(stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
)

FetchContent_Declare(nanobind
    GIT_REPOSITORY https://github.com/wjakob/nanobind.git
    GIT_TAG v2.9.2
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
)

FetchContent_Declare(cxxopts
    GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
    GIT_TAG v3.2.1
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL
)

FetchContent_Declare(oidn
    GIT_REPOSITORY https://github.com/OpenImageDenoise/oidn.git
    GIT_TAG master
    EXCLUDE_FROM_ALL
)


FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG master
        EXCLUDE_FROM_ALL
)

FetchContent_Declare(
        volk
        GIT_REPOSITORY https://github.com/zeux/volk.git
        GIT_TAG master
        EXCLUDE_FROM_ALL
)

FetchContent_Declare(
        Vulkan-Headers
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG main
        EXCLUDE_FROM_ALL
)

FetchContent_Declare(
        VulkanMemoryAllocator
        GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
        GIT_TAG master
        EXCLUDE_FROM_ALL
)

FetchContent_Declare(
        SDL
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.0
        GIT_SHALLOW ON
)

FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.5-docking
        EXCLUDE_FROM_ALL
)

FetchContent_Declare(
        enet
        GIT_REPOSITORY https://github.com/lsalzman/enet.git
        GIT_TAG v1.3.18
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
)

set(_CORONA_ONETBB_LOCAL_SOURCE_DIR
    "${CMAKE_SOURCE_DIR}/third_party/tbb/oneapi-tbb-2022.3.0"
)
if(EXISTS "${_CORONA_ONETBB_LOCAL_SOURCE_DIR}/CMakeLists.txt")
    set(_CORONA_ONETBB_SOURCE_LABEL "vendored source")
    FetchContent_Declare(
            oneTBB
            SOURCE_DIR "${_CORONA_ONETBB_LOCAL_SOURCE_DIR}"
            EXCLUDE_FROM_ALL
    )
else()
    set(_CORONA_ONETBB_SOURCE_LABEL "release archive")
    FetchContent_Declare(
            oneTBB
            URL https://github.com/uxlfoundation/oneTBB/archive/refs/tags/v2022.3.0.zip
            DOWNLOAD_EXTRACT_TIMESTAMP FALSE
            EXCLUDE_FROM_ALL
    )
endif()

# ------------------------------------------------------------------------------
# Fetch and enable dependencies
# ------------------------------------------------------------------------------

set(BUILD_TESTING OFF CACHE BOOL "Disable building tests for 3rd party dependencies" FORCE)
set(ASSIMP_BUILD_USD_IMPORTER OFF CACHE BOOL "" FORCE)

# When Vision is enabled, assimp must build with all importers + zlib to satisfy
# Vision's mesh import paths. Only override these knobs in that case so default
# CoronaEngine builds keep assimp lightweight.
if(CORONA_BUILD_VISION)
    set(ASSIMP_BUILD_ZLIB                       ON  CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ASSIMP_TOOLS               OFF CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_TESTS                      OFF CACHE BOOL "" FORCE)
    set(ASSIMP_INSTALL                          OFF CACHE BOOL "" FORCE)
    set(ASSIMP_INJECT_DEBUG_POSTFIX             OFF CACHE BOOL "" FORCE)
    set(ASSIMP_NO_EXPORT                        ON  CACHE BOOL "" FORCE)
    set(ASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT   ON  CACHE BOOL "" FORCE)
endif()

FetchContent_MakeAvailable(assimp)
if(MSVC AND TARGET assimp)
    target_compile_options(assimp PRIVATE /EHsc)
endif()
message(STATUS "[3rdparty] assimp module enabled")

FetchContent_MakeAvailable(stb)
message(STATUS "[3rdparty] stb module enabled")

FetchContent_MakeAvailable(nanobind)
message(STATUS "[3rdparty] nanobind module enabled")

set(TBB_TEST OFF CACHE BOOL "" FORCE)
set(TBB_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TBB_STRICT OFF CACHE BOOL "" FORCE)

cmake_path(
    ABSOLUTE_PATH FETCHCONTENT_BASE_DIR
    BASE_DIRECTORY "${CMAKE_BINARY_DIR}"
    NORMALIZE
    OUTPUT_VARIABLE _CORONA_HORIZON_FETCHCONTENT_SOURCE_ROOT
)
set(HORIZON_FETCHCONTENT_SOURCE_ROOT "${_CORONA_HORIZON_FETCHCONTENT_SOURCE_ROOT}" CACHE PATH
    "Shared FetchContent source checkouts used by Horizon" FORCE)

get_filename_component(
    _corona_local_tbb_dir
    "${PROJECT_SOURCE_DIR}/third_party/tbb/oneapi-tbb-2022.3.0/lib/cmake/tbb"
    ABSOLUTE
)
set(_corona_tbb_candidate_dirs "${_corona_local_tbb_dir}")

corona_tbb_package_has_runtime("${_corona_local_tbb_dir}" _corona_local_tbb_ok)
if(EXISTS "${_corona_local_tbb_dir}/TBBConfig.cmake" AND NOT _corona_local_tbb_ok)
    message(STATUS
        "[3rdparty] Local TBB package is incomplete; "
        "falling back to Horizon package or oneTBB FetchContent")
endif()

if(DEFINED TBB_DIR AND NOT TBB_DIR STREQUAL "")
    list(PREPEND _corona_tbb_candidate_dirs "${TBB_DIR}")
endif()

if(WIN32)
    FetchContent_GetProperties(Horizon SOURCE_DIR _corona_horizon_source_dir)
    if(NOT _corona_horizon_source_dir)
        set(_corona_horizon_source_dir "${_CORONA_HORIZON_FETCHCONTENT_SOURCE_ROOT}/horizon-src")
    endif()

    get_filename_component(
        _corona_horizon_tbb_dir
        "${_corona_horizon_source_dir}/modules/corona/third_party/win/oneapi-tbb-2022.3.0/lib/cmake/tbb"
        ABSOLUTE
    )
    list(APPEND _corona_tbb_candidate_dirs "${_corona_horizon_tbb_dir}")
endif()

corona_select_valid_tbb_dir(_corona_selected_tbb_dir ${_corona_tbb_candidate_dirs})

if(_corona_selected_tbb_dir)
    set(TBB_DIR "${_corona_selected_tbb_dir}" CACHE PATH
        "Path to TBB cmake configuration directory" FORCE)
    find_package(TBB CONFIG QUIET)
endif()

if(TARGET TBB::tbb)
    message(STATUS "[3rdparty] oneTBB module provided by package: ${TBB_DIR}")
else()
    FetchContent_MakeAvailable(oneTBB)
    message(STATUS "[3rdparty] oneTBB module enabled from ${_CORONA_ONETBB_SOURCE_LABEL}")
endif()

unset(_CORONA_ONETBB_LOCAL_SOURCE_DIR)
unset(_CORONA_ONETBB_SOURCE_LABEL)
unset(_corona_local_tbb_dir)
unset(_corona_local_tbb_ok)
unset(_corona_selected_tbb_dir)
unset(_corona_tbb_candidate_dirs)
if(WIN32)
    unset(_corona_horizon_tbb_dir)
    unset(_corona_horizon_source_dir)
endif()

if(CORONA_BUILD_VISION)
    set(HORIZON_BUILD_OCARINA ON CACHE BOOL "" FORCE)
    set(HORIZON_BUILD_VISION_HOTFIX ON CACHE BOOL "" FORCE)
endif()
set(HORIZON_BUILD_TOOLS ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(Horizon)
# Horizon's Helicon / corona kernel targets publish /source-charset:utf-8 and
# /execution-charset:utf-8 via PUBLIC compile options. Strip the INTERFACE
# side so downstream consumers (which already get /utf-8 globally) do not
# trigger MSVC D8016 from mixing the long and short UTF-8 charset flags.
corona_strip_msvc_charset_interface(Helicon)
corona_strip_msvc_charset_interface(corona_kernel)
corona_strip_msvc_charset_interface(corona_pal)

if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    if(TARGET ShaderCompileScripts)
        target_compile_options(ShaderCompileScripts PRIVATE
            $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>)
    endif()

    if(TARGET Horizon)
        target_compile_options(Horizon PRIVATE
            $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>)
    endif()
endif()

message(STATUS "[3rdparty] Horizon module enabled")
unset(_CORONA_HORIZON_FETCHCONTENT_SOURCE_ROOT)

FetchContent_MakeAvailable(glfw)
message(STATUS "[3rdparty] glfw module enabled")

FetchContent_MakeAvailable(volk)
message(STATUS "[3rdparty] volk module enabled")

FetchContent_MakeAvailable(Vulkan-Headers)
message(STATUS "[3rdparty] Vulkan-Headers module enabled")

FetchContent_MakeAvailable(VulkanMemoryAllocator)
message(STATUS "[3rdparty] VulkanMemoryAllocator module enabled")

FetchContent_MakeAvailable(SDL)
message(STATUS "[3rdparty] SDL module enabled")

FetchContent_MakeAvailable(imgui)
message(STATUS "[3rdparty] imgui module enabled")

# Manually define imgui target since it has no CMakeLists.txt
if(NOT TARGET imgui)
    add_library(imgui STATIC
            "${imgui_SOURCE_DIR}/imgui.cpp"
            "${imgui_SOURCE_DIR}/imgui_demo.cpp"
            "${imgui_SOURCE_DIR}/imgui_draw.cpp"
            "${imgui_SOURCE_DIR}/imgui_tables.cpp"
            "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    )
    target_include_directories(imgui PUBLIC "${imgui_SOURCE_DIR}")

    # imgui is created in the root directory scope BEFORE corona_compile_config
    # is included, so it does NOT inherit the directory-level /utf-8 set by
    # add_compile_options() there (directory COMPILE_OPTIONS are snapshotted
    # into a target at the moment add_library() runs).
    #
    # Apply /utf-8 explicitly so imgui's own translation units are compiled
    # under the same UTF-8 policy as the rest of CoronaEngine. PRIVATE is
    # sufficient: imgui headers are pure ASCII; only its .cpp files need it.
    if(MSVC OR CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        target_compile_options(imgui PRIVATE
            $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>)
    endif()
endif()

# Manually define enet target — ENet v1.3.18's CMakeLists.txt only supports
# Unix Makefiles, so we fetch the source and build the library ourselves.
FetchContent_GetProperties(enet)
if(NOT enet_POPULATED)
    FetchContent_Populate(enet)
endif()
if(NOT TARGET enet)
    add_library(enet STATIC
        "${enet_SOURCE_DIR}/callbacks.c"
        "${enet_SOURCE_DIR}/compress.c"
        "${enet_SOURCE_DIR}/host.c"
        "${enet_SOURCE_DIR}/list.c"
        "${enet_SOURCE_DIR}/packet.c"
        "${enet_SOURCE_DIR}/peer.c"
        "${enet_SOURCE_DIR}/protocol.c"
        "${enet_SOURCE_DIR}/win32.c"
    )
    target_include_directories(enet PUBLIC "${enet_SOURCE_DIR}/include")
    if(WIN32)
        target_link_libraries(enet PUBLIC ws2_32 winmm)
    endif()
    if(MSVC)
        target_compile_definitions(enet PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(enet PRIVATE
            $<$<COMPILE_LANGUAGE:C>:/utf-8>)
    endif()
    message(STATUS "[3rdparty] enet module enabled")
endif()

if(CORONA_BUILD_VISION)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(VISION_BUILD_VULKAN OFF CACHE BOOL "" FORCE)

    # cxxopts: required by Vision (replaces submodule src/ext/cxxopts)
    set(CXXOPTS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CXXOPTS_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
    set(CXXOPTS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(cxxopts)
    message(STATUS "[3rdparty] cxxopts module enabled")

    # OIDN: optional, on demand (replaces submodule src/ext/oidn)
    if(VISION_BUILD_OIDN)
        set(OIDN_DEVICE_CUDA ON CACHE BOOL "" FORCE)
        FetchContent_MakeAvailable(oidn)
        message(STATUS "[3rdparty] oidn module enabled")
    endif()
endif()

# ==============================================================================
# UTF-8 flag normalization for Horizon targets
#
# Problem:
#   Horizon's root CMakeLists adds:
#       target_compile_options(Helicon PUBLIC
#           /source-charset:utf-8 /execution-charset:utf-8)
#   The PUBLIC scope propagates those flags via INTERFACE_COMPILE_OPTIONS to
#   every downstream consumer (Horizon -> Helicon, then our systems link to
#   Horizon).
#
#   OpenUSD (pulled in by modules/corona_resource via FetchContent) injects
#   /utf-8 PUBLIC on every pxr target. Any CoronaEngine target that
#   transitively links BOTH chains ends up with:
#       /source-charset:utf-8 /execution-charset:utf-8 /utf-8
#   which MSVC rejects with `command-line error D8016`.
#
# Fix:
#   Strip ONLY INTERFACE_COMPILE_OPTIONS on Horizon-provided targets that
#   publish the long-form charset flags. This leaves their own compilation
#   untouched while preventing the conflicting pair from leaking into our
#   targets. Our own targets get /utf-8 from corona_compile_config.cmake via
#   add_compile_options().
#
# Failure handling:
#   If a future Horizon upgrade wraps these flags in generator expressions or
#   moves them to PRIVATE/CACHE, the literal REMOVE_ITEM call may silently
#   become a no-op. The post-check below FATAL_ERRORs in that case so we
#   notice immediately during configure rather than debugging D8016 again.
# ==============================================================================
if(MSVC)
    # Match the long-form charset flag, whether it appears as a bare token
    # ("/source-charset:utf-8") or wrapped inside a generator expression
    # (e.g. "$<$<COMPILE_LANGUAGE:CXX>:/source-charset:utf-8>") that a future
    # Horizon upgrade might introduce. The list(REMOVE_ITEM) approach used
    # previously only handled the bare-token form.
    set(_CORONA_LEGACY_CHARSET_REGEX "/(source|execution)-charset:utf-8")

    foreach(_corona_charset_target Helicon Horizon corona_kernel corona_pal)
        if(TARGET ${_corona_charset_target})
            get_target_property(_corona_iopts
                ${_corona_charset_target} INTERFACE_COMPILE_OPTIONS)
            if(_corona_iopts)
                set(_corona_iopts_kept "")
                set(_corona_stripped FALSE)
                foreach(_opt IN LISTS _corona_iopts)
                    if(_opt MATCHES "${_CORONA_LEGACY_CHARSET_REGEX}")
                        set(_corona_stripped TRUE)
                    else()
                        list(APPEND _corona_iopts_kept "${_opt}")
                    endif()
                endforeach()
                if(_corona_stripped)
                    set_target_properties(${_corona_charset_target} PROPERTIES
                        INTERFACE_COMPILE_OPTIONS "${_corona_iopts_kept}")
                    message(STATUS
                        "[3rdparty] Stripped legacy /source-charset & /execution-charset"
                        " from INTERFACE_COMPILE_OPTIONS of ${_corona_charset_target}")
                endif()
            endif()
        endif()
    endforeach()

    # Post-check: ensure no Horizon-provided consumer can hit D8016. Walk each option
    # individually with the same regex used above, so this guard and the
    # stripping logic stay in lock-step (avoids the previous failure mode
    # where REMOVE_ITEM silently no-op'd but string(FIND) still tripped).
    foreach(_corona_charset_target Helicon Horizon corona_kernel corona_pal)
        if(TARGET ${_corona_charset_target})
            get_target_property(_corona_iopts
                ${_corona_charset_target} INTERFACE_COMPILE_OPTIONS)
            if(_corona_iopts)
                foreach(_opt IN LISTS _corona_iopts)
                    if(_opt MATCHES "${_CORONA_LEGACY_CHARSET_REGEX}")
                        message(FATAL_ERROR
                            "[3rdparty] ${_corona_charset_target} still exposes long-form charset flag "
                            "via INTERFACE_COMPILE_OPTIONS after stripping: '${_opt}'. "
                            "The regex in misc/cmake/corona_third_party.cmake must be "
                            "updated to match the new upstream flag form.")
                    endif()
                endforeach()
            endif()
        endif()
    endforeach()

    unset(_CORONA_LEGACY_CHARSET_REGEX)
endif()
