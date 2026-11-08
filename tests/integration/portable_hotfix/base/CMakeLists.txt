add_library(fixture-base SHARED "${CMAKE_BINARY_DIR}/base/base.cpp")
set_target_properties(fixture-base PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
