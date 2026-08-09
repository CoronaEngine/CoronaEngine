"""Adapters that normalize the manifest-backed LANChat contract."""


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
