"""Compatibility adapters for pre-manifest network and LANChat bindings.

This module also owns the normalization of manifest-backed LANChat transport and
queue objects. ``api.editor_api`` remains the public factory boundary and only
re-exports those implementation adapters for backward-compatible callers.
"""


class _LegacyNetworkAdapter:
    """Compatibility view for injected pre-manifest engine objects only."""

    def __init__(self, native_engine):
        self._native_engine = native_engine

    def broadcast_intent(self, user_id, tooltip, position, status="placing_object"):
        return self._native_engine.network_broadcast_intent(
            user_id, tooltip, position, status
        )

    def get_session_info(self):
        role_getter = getattr(self._native_engine, "network_session_role_name", None)
        return {"role": str(role_getter() or "none")} if callable(role_getter) else {}


class _LegacyLanChatAdapter:
    """Compatibility view for the old read-only LANChat bindings."""

    def __init__(self, native_engine):
        self._native_engine = native_engine

    def list_agents(self):
        getter = getattr(self._native_engine, "network_lanchat_agents_snapshot", None)
        return {"ok": True, "agents": list(getter() or [])} if callable(getter) else {}


class _LegacyLanChatQueueAdapter:
    """Compatibility adapter for old LANChat queue bindings."""

    def __init__(self, native_engine):
        self._native_engine = native_engine

    def poll_agent_trigger(self):
        getter = getattr(self._native_engine, "network_pop_lanchat_agent_trigger", None)
        return getter() if callable(getter) else None

    def poll_coordinator_sync_message(self):
        getter = getattr(
            self._native_engine, "network_pop_lanchat_coordinator_sync_message", None
        )
        return getter() if callable(getter) else None

    def poll_room_event(self):
        getter = getattr(self._native_engine, "network_pop_lanchat_room_event", None)
        return getter() if callable(getter) else None

    def poll_sync_event(self):
        getter = getattr(self._native_engine, "network_pop_lanchat_sync_event", None)
        return getter() if callable(getter) else None


class _LegacyLanChatTransportAdapter:
    """Compatibility adapter for old specialized Python bindings."""

    def __init__(self, native_engine):
        self._native_engine = native_engine

    def send_agent_reply(
        self,
        agent_id,
        agent_name,
        text,
        message_kind="agent_reply",
        target_agent_id="",
        correlation_id="",
        metadata_json="",
        source_user_id="",
    ):
        method = getattr(self._native_engine, "network_send_agent_reply_ex", None)
        if callable(method):
            return bool(
                method(
                    agent_id,
                    agent_name,
                    text,
                    message_kind,
                    target_agent_id,
                    correlation_id,
                    metadata_json,
                )
            )
        method = getattr(self._native_engine, "network_send_agent_reply", None)
        return bool(method(agent_id, agent_name, text)) if callable(method) else False

    def send_system_message(
        self,
        sender_id,
        sender_name,
        text,
        message_kind="agent_reply",
        correlation_id="",
        metadata_json="",
    ):
        method = getattr(self._native_engine, "network_send_system_message_ex", None)
        if callable(method):
            return bool(
                method(
                    sender_id,
                    sender_name,
                    text,
                    message_kind,
                    correlation_id,
                    metadata_json,
                )
            )
        method = getattr(self._native_engine, "network_send_system_message", None)
        return bool(method(sender_id, sender_name, text)) if callable(method) else False

    def send_system_message_to_host(
        self,
        sender_id,
        sender_name,
        text,
        message_kind="action_status",
        correlation_id="",
        metadata_json="",
    ):
        method = getattr(
            self._native_engine, "network_send_system_message_to_host_ex", None
        )
        return (
            bool(
                method(
                    sender_id,
                    sender_name,
                    text,
                    message_kind,
                    correlation_id,
                    metadata_json,
                )
            )
            if callable(method)
            else False
        )

    def send_system_message_to_user(
        self,
        target_user_id,
        sender_id,
        sender_name,
        text,
        message_kind="action_status",
        correlation_id="",
        metadata_json="",
    ):
        method = getattr(
            self._native_engine, "network_send_system_message_to_user_ex", None
        )
        return (
            bool(
                method(
                    target_user_id,
                    sender_id,
                    sender_name,
                    text,
                    message_kind,
                    correlation_id,
                    metadata_json,
                )
            )
            if callable(method)
            else False
        )


def _accepted_result(result):
    return bool(result.get("ok")) if isinstance(result, dict) else bool(result)


class _LanChatQueueAdapter:
    """Normalize manifest LANChat queue polling behind the public contract."""

    def __init__(self, api):
        self._api = api

    def poll_agent_trigger(self):
        return self._api.poll_agent_trigger()

    def poll_coordinator_sync_message(self):
        return self._api.poll_coordinator_sync_message()

    def poll_room_event(self):
        return self._api.poll_room_event()

    def poll_sync_event(self):
        return self._api.poll_sync_event()


class _LanChatTransportAdapter:
    """Normalize manifest LANChat transport operations behind the public contract."""

    def __init__(self, api):
        self._api = api

    def send_agent_reply(
        self, agent_id, agent_name, text, message_kind="agent_reply",
        target_agent_id="", correlation_id="", metadata_json="", source_user_id="",
    ):
        return _accepted_result(self._api.send_agent_reply({
            "agent_id": agent_id,
            "agent_name": agent_name,
            "text": text,
            "message_kind": message_kind,
            "target_agent_id": target_agent_id,
            "source_user_id": source_user_id,
            "correlation_id": correlation_id,
            "metadata_json": metadata_json,
        }))

    def send_system_message(
        self, sender_id, sender_name, text, message_kind="agent_reply",
        correlation_id="", metadata_json="",
    ):
        return _accepted_result(self._api.send_system_message({
            "sender_id": sender_id,
            "sender_name": sender_name,
            "text": text,
            "message_kind": message_kind,
            "correlation_id": correlation_id,
            "metadata_json": metadata_json,
        }))

    def send_system_message_to_host(
        self, sender_id, sender_name, text, message_kind="action_status",
        correlation_id="", metadata_json="",
    ):
        return _accepted_result(self._api.send_system_message_to_host({
            "sender_id": sender_id,
            "sender_name": sender_name,
            "text": text,
            "message_kind": message_kind,
            "correlation_id": correlation_id,
            "metadata_json": metadata_json,
        }))

    def send_system_message_to_user(
        self, target_user_id, sender_id, sender_name, text,
        message_kind="action_status", correlation_id="", metadata_json="",
    ):
        return _accepted_result(self._api.send_system_message_to_user({
            "target_user_id": target_user_id,
            "sender_id": sender_id,
            "sender_name": sender_name,
            "text": text,
            "message_kind": message_kind,
            "correlation_id": correlation_id,
            "metadata_json": metadata_json,
        }))
