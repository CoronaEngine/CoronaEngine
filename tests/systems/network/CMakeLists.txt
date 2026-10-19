add_executable(corona_actor_editor_sync_tests
    test_actor_editor_sync.cpp
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
    test_peer_liveness.cpp
)

target_include_directories(corona_peer_liveness_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include
)

add_test(NAME PeerLivenessTests COMMAND corona_peer_liveness_tests)

add_executable(corona_network_protocol_tests
    test_network_protocol.cpp
    ${PROJECT_SOURCE_DIR}/src/shared_data_hub.cpp
)

target_link_libraries(corona_network_protocol_tests
    PRIVATE
        corona::system::network
        corona::resource::manager
)

add_test(NAME NetworkProtocolTests COMMAND corona_network_protocol_tests)

add_executable(corona_discovery_tests
    test_discovery.cpp
)
target_link_libraries(corona_discovery_tests
    PRIVATE corona::system::network
)
add_test(NAME DiscoveryTests COMMAND corona_discovery_tests)

# The identity registry is written by the editor/collaboration threads while the
# network thread scans it every tick, so it must synchronize its own map.
add_executable(corona_network_identity_registry_tests
    test_network_identity_registry.cpp
    ${PROJECT_SOURCE_DIR}/src/shared_data_hub.cpp
)
target_link_libraries(corona_network_identity_registry_tests
    PRIVATE
        corona::system::network
        corona::resource::manager
)
add_test(NAME NetworkIdentityRegistryTests
         COMMAND corona_network_identity_registry_tests)
