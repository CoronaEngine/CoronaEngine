# Engine tests mirror the main source modules; cross-system tests live in integration.
add_subdirectory(kernel)
add_subdirectory(systems/display)
add_subdirectory(systems/geometry)
add_subdirectory(systems/network)
add_subdirectory(systems/optics)
add_subdirectory(systems/script)
add_subdirectory(systems/ui)
add_subdirectory(integration)
