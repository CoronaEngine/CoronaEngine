"""Static guards for LANChat native/Python bridge contracts.

These checks intentionally do not compile C++ or start CEF. They catch bridge
name drift before the F5/native validation phase.
"""

from __future__ import annotations

from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[4]


def _read(path: str) -> str:
    return (REPO_ROOT / path).read_text(encoding="utf-8", errors="ignore")


def _assert_contains(path: str, *needles: str) -> None:
    source = _read(path)
    missing = [needle for needle in needles if needle not in source]
    assert not missing, f"{path} missing bridge contract(s): {missing}"


def test_lanchat_room_event_bridge_is_pollable_from_python_worker() -> None:
    _assert_contains(
        "include/corona/systems/network/network_system.h",
        "struct LanChatRoomEvent",
        "lanchat_pop_room_event",
    )
    _assert_contains(
        "src/systems/network/network_system.cpp",
        "pending_lanchat_room_events",
        "notify_lanchat_room_closed",
        "\"room_closed\"",
        "lanchat_pop_room_event",
    )
    _assert_contains(
        "src/systems/script/python/engine_bindings.cpp",
        "network_pop_lanchat_room_event",
        "lanchat_pop_room_event",
        "room_id",
    )
    _assert_contains(
        "editor/plugins/AITool/services/lanchat_agent_worker.py",
        "network_pop_lanchat_room_event",
        "handle_lanchat_room_event",
    )


def test_lanchat_plain_chat_coordinator_bridge_contract_is_intact() -> None:
    _assert_contains(
        "include/corona/systems/network/lanchat_state.h",
        "pop_coordinator_sync_message",
        "enqueue_coordinator_sync_message",
    )
    _assert_contains(
        "src/systems/network/lanchat_state.cpp",
        "enqueue_coordinator_sync_message(message)",
        "pop_coordinator_sync_message",
        "message_kind",
        "metadata_json",
    )
    _assert_contains(
        "include/corona/systems/network/network_system.h",
        "lanchat_pop_coordinator_sync_message",
    )
    _assert_contains(
        "src/systems/script/python/engine_bindings.cpp",
        "network_pop_lanchat_coordinator_sync_message",
        "lanchat_message_to_dict",
    )
    _assert_contains(
        "editor/plugins/AITool/services/lanchat_agent_worker.py",
        "network_pop_lanchat_coordinator_sync_message",
        "sync_chat_message_to_coordinator",
        "lanchat_native_queue",
    )


def test_lanchat_cef_event_callback_remains_registered() -> None:
    _assert_contains(
        "src/systems/ui/cef/cef_query_bridge.cpp",
        "set_lanchat_event_callback",
        "emit_lanchat_event_json",
        "lanchat-event",
    )


def test_lanchat_host_only_disclosure_has_native_targeted_api() -> None:
    _assert_contains(
        "include/corona/systems/network/network_system.h",
        "lanchat_send_system_message_to_host_ex",
        "lanchat_send_system_message_to_user_ex",
    )
    _assert_contains(
        "src/systems/network/network_system.cpp",
        "lanchat_send_system_message_to_host_ex",
        "lanchat_send_system_message_to_user_ex",
        "TARGET_NOT_LOCAL",
        "message_event_json(result.message)",
    )
    _assert_contains(
        "src/systems/script/python/engine_bindings.cpp",
        "network_send_system_message_to_host_ex",
        "network_send_system_message_to_user_ex",
        "lanchat_send_system_message_to_host_ex",
        "lanchat_send_system_message_to_user_ex",
    )
    _assert_contains(
        "editor/plugins/AITool/services/lanchat_agent_worker.py",
        "network_send_system_message_to_host_ex",
        "network_send_system_message_to_user_ex",
        "_host_disclosure_broadcast_payload",
    )


def test_lanchat_worker_is_started_with_scene_composer_factory() -> None:
    _assert_contains(
        "editor/plugins/AITool/main.py",
        "def _create_lanchat_scene_composer",
        "SceneComposer",
        "composer_factory=_create_lanchat_scene_composer",
    )


def test_network_host_periodic_snapshot_does_not_rebroadcast_actor_creates() -> None:
    source = _read("editor/Frontend/src/views/sidebar/Network.vue")
    expected = (
        "if (count > 0 && sessionRole.value === 'host') {\n"
        "        await broadcastCurrentSceneSnapshot(currentSceneName.value, false, false);"
    )
    assert expected in source, "host polling must not rebroadcast actor create every 2 seconds"
    assert "await broadcastCurrentSceneSnapshot(sceneName, true, true);" in source, (
        "host must still send actor creates when a client explicitly requests a full snapshot"
    )
    assert "rememberActorCreateBroadcast(targetScene, actorGuid, modelPath)" in source, (
        "snapshot fallback actor creates must be deduped across explicit snapshot requests"
    )
    assert "rememberActorCreateBroadcast(sceneName, actorGuid, modelPath)" in source, (
        "realtime actor create broadcasts must share snapshot fallback dedupe state"
    )
    assert "forgetActorCreateBroadcast(sceneName, actorGuid)" in source, (
        "actor deletes must clear snapshot fallback actor-create dedupe state"
    )


def test_lanchat_room_panel_exposes_validation_agent_bundle() -> None:
    _assert_contains(
        "editor/Frontend/src/views/sidebar/lanchat/RoomPanel.vue",
        "roleTemplateBundles",
        "night_market_validation",
        "夜市验证组",
        "addRoleTemplateBundle",
    )


def test_lanchat_room_panel_exposes_host_vlm_toggle() -> None:
    _assert_contains(
        "editor/Frontend/src/views/sidebar/lanchat/RoomPanel.vue",
        "VLM 外观检查",
        "onVlmToggle",
        "generationOptionsMetadata",
        "vlmMaxTargets",
    )
    _assert_contains(
        "editor/Frontend/src/stores/lanchat.js",
        "generationOptions",
        "setGenerationOptions",
        "generationOptionsMetadata",
        "vlm_max_targets",
    )


def test_lanchat_history_rooms_are_session_loadable() -> None:
    _assert_contains(
        "include/corona/systems/network/lanchat_history_store.h",
        "std::string session_id",
    )
    _assert_contains(
        "src/systems/ui/cef/cef_query_bridge.cpp",
        "display_room_id",
        "const std::string load_id = room.session_id.empty() ? room.room_id : room.session_id",
        "{\"room_id\", load_id}",
    )
    _assert_contains(
        "editor/Frontend/src/views/sidebar/lanchat/RoomPanel.vue",
        "room.display_room_id || room.room_id",
    )


def test_lanchat_displayed_ip_prefers_wlan_adapter() -> None:
    _assert_contains(
        "src/systems/ui/cef/cef_query_bridge.cpp",
        "detect_wlan_ipv4",
        "GetAdaptersAddresses",
        "looks_like_wlan_adapter",
        'payload["local_ip"] = detect_wlan_ipv4()',
        'data["ip"] = detect_wlan_ipv4()',
    )


if __name__ == "__main__":
    test_lanchat_room_event_bridge_is_pollable_from_python_worker()
    test_lanchat_plain_chat_coordinator_bridge_contract_is_intact()
    test_lanchat_cef_event_callback_remains_registered()
    test_lanchat_host_only_disclosure_has_native_targeted_api()
    test_lanchat_worker_is_started_with_scene_composer_factory()
    test_network_host_periodic_snapshot_does_not_rebroadcast_actor_creates()
    test_lanchat_room_panel_exposes_validation_agent_bundle()
    test_lanchat_room_panel_exposes_host_vlm_toggle()
    test_lanchat_history_rooms_are_session_loadable()
    test_lanchat_displayed_ip_prefers_wlan_adapter()
    print("[OK] LANChat native bridge static contracts")
