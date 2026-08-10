include_guard(GLOBAL)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# UTF-8 source/execution charset.
# When built inside CoronaEngine, the parent project enforces /utf-8 globally
# from cmake/corona_compile_config.cmake. For standalone builds (no
# parent CoronaEngine target), apply it here so source files with non-ASCII
# literals still compile correctly.
if(MSVC AND NOT TARGET CoronaEngine)
    add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>)
endif()

add_compile_definitions(
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
)
