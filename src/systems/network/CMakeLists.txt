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
        corona::resource::manager
)

if(WIN32)
    target_link_libraries(corona_network_system
        PUBLIC
            ws2_32
            winmm
    )
endif()
