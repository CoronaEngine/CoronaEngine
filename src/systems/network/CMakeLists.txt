# ==============================================================================
# Network System
# ==============================================================================

corona_add_system(network
    SOURCES
        network_system.cpp
        peer_manager.cpp
        sync_engine.cpp
        discovery.cpp
        lanchat_history_store.cpp
        lanchat_state.cpp
    DEPENDENCIES
        Horizon
        enet
        nlohmann_json
)

if(WIN32)
    target_link_libraries(corona_network_system
        PUBLIC
            ws2_32
            winmm
    )
endif()

if(BUILD_CORONA_TESTING OR BUILD_TESTING)
    add_executable(corona_actor_editor_sync_tests
        tests/test_actor_editor_sync.cpp
    )

    target_include_directories(corona_actor_editor_sync_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    target_link_libraries(corona_actor_editor_sync_tests
        PRIVATE
            nlohmann_json::nlohmann_json
    )

    add_test(NAME ActorEditorSyncTests COMMAND corona_actor_editor_sync_tests)

    add_executable(corona_peer_liveness_tests
        tests/test_peer_liveness.cpp
    )

    target_include_directories(corona_peer_liveness_tests
        PRIVATE
            ${PROJECT_SOURCE_DIR}/include
    )

    add_test(NAME PeerLivenessTests COMMAND corona_peer_liveness_tests)

    add_executable(corona_network_protocol_tests
        tests/test_network_protocol.cpp
        ${PROJECT_SOURCE_DIR}/src/shared_data_hub.cpp
    )

    target_link_libraries(corona_network_protocol_tests
        PRIVATE
            corona::system::network
    )

    add_test(NAME NetworkProtocolTests COMMAND corona_network_protocol_tests)

    add_executable(corona_discovery_tests
        tests/test_discovery.cpp
    )
    target_link_libraries(corona_discovery_tests
        PRIVATE corona::system::network
    )
    add_test(NAME DiscoveryTests COMMAND corona_discovery_tests)
endif()
