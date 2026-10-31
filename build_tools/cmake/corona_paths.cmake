# Resolve from this module, including when a helper is used by a standalone test.
get_filename_component(CORONA_BUILD_TOOLS_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
get_filename_component(CORONA_ROOT_DIR "${CORONA_BUILD_TOOLS_DIR}/.." ABSOLUTE)
set(CORONA_BUILD_SCRIPTS_DIR "${CORONA_BUILD_TOOLS_DIR}/scripts")
