if(NOT DEFINED STARTUP_CONFIG_SOURCE OR NOT DEFINED STARTUP_CONFIG_DESTINATION)
    message(FATAL_ERROR "Startup configuration source and destination are required")
endif()
# Never replace a user's local settings during an incremental build.
if(NOT EXISTS "${STARTUP_CONFIG_DESTINATION}")
    configure_file("${STARTUP_CONFIG_SOURCE}" "${STARTUP_CONFIG_DESTINATION}" COPYONLY)
endif()
