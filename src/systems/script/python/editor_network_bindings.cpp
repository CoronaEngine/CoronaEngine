#include <corona/kernel/core/kernel_context.h>
#include <corona/systems/network/network_system.h>
#include <nanobind/nanobind.h>
#include <nanobind/stl/array.h>
#include <nanobind/stl/string.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

// Compatibility bridge for editor/AITool network adapters. The stable editor
// surface remains the manifest-backed network/lan_chat API; Script Runtime
// code must not use these functions as a second network contract.
namespace EngineScripts {

namespace {

std::shared_ptr<Corona::Systems::NetworkSystem> get_network_system() {
    auto sys_mgr = Corona::Kernel::KernelContext::instance().system_manager();
    if (!sys_mgr) {
        return nullptr;
    }
    return std::dynamic_pointer_cast<Corona::Systems::NetworkSystem>(
        sys_mgr->get_system("Network"));
}

uint64_t network_now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

nanobind::dict lanchat_message_to_dict(const Corona::Network::LanChatMessage& message) {
    nanobind::dict item;
    item["message_id"] = message.message_id;
    item["sender_id"] = message.sender_id;
    item["sender_name"] = message.sender_name;
    item["room_id"] = message.room_id;
    item["seq"] = message.seq;
    item["from"] = message.sender_name;
    item["text"] = message.text;
    item["ts"] = message.timestamp_ms / 1000;
    item["sender_type"] = message.sender_type;
    item["message_kind"] = message.message_kind;
    item["target_agent_id"] = message.target_agent_id;
    item["source_user_id"] = message.source_user_id;
    item["correlation_id"] = message.correlation_id;
    item["metadata_json"] = message.metadata_json;
    return item;
}

}  // namespace

void BindEditorNetwork(nanobind::module_& m) {
    namespace nb = nanobind;

    // ============================================================================
    // Network collaboration bridge: Python may run AI locally; C++ owns state/network.
    // ============================================================================
    m.def("network_local_peer_id", []() -> std::string {
        auto sys = get_network_system();
        return sys ? sys->local_peer_id() : std::string{};
    }, "Return the local NetworkSystem peer id.");

    m.def("network_register_agent", [](const std::string& agent_id,
                                        const std::string& name,
                                        const std::string& persona) -> bool {
        auto sys = get_network_system();
        return sys && sys->lanchat_register_agent(agent_id, name, persona).ok;
    }, nb::arg("agent_id"), nb::arg("name"), nb::arg("persona"),
       "Register an AI agent in the C++ collaboration roster.");

    m.def("network_remove_agent", [](const std::string& agent_id) -> bool {
        auto sys = get_network_system();
        return sys && sys->lanchat_remove_agent(agent_id).ok;
    }, nb::arg("agent_id"), "Remove an AI agent from the C++ collaboration roster.");

    m.def("network_send_agent_reply", [](const std::string& agent_id,
                                          const std::string& agent_name,
                                          const std::string& text) -> bool {
        auto sys = get_network_system();
        return sys && sys->lanchat_send_agent_reply(agent_id, agent_name, text).accepted;
    }, nb::arg("agent_id"), nb::arg("agent_name"), nb::arg("text"),
       "Send an AI agent reply through the C++ reliable collaboration channel.");

    m.def("network_send_agent_reply_ex", [](const std::string& agent_id,
                                             const std::string& agent_name,
                                             const std::string& text,
                                             const std::string& message_kind,
                                             const std::string& target_agent_id,
                                             const std::string& correlation_id,
                                             const std::string& metadata_json) -> bool {
        auto sys = get_network_system();
        return sys && sys->lanchat_send_agent_reply_ex(
            agent_id, agent_name, text, "agent",
            message_kind.empty() ? "agent_reply" : message_kind,
            target_agent_id, "", correlation_id, metadata_json).accepted;
    }, nb::arg("agent_id"), nb::arg("agent_name"), nb::arg("text"),
       nb::arg("message_kind") = "agent_reply", nb::arg("target_agent_id") = "",
       nb::arg("correlation_id") = "", nb::arg("metadata_json") = "",
       "Send a structured AI agent reply through the C++ LANChat channel.");

    m.def("network_send_system_message_to_host_ex",
          [](const std::string& sender_id,
             const std::string& sender_name,
             const std::string& text,
             const std::string& message_kind,
             const std::string& correlation_id,
             const std::string& metadata_json) -> bool {
              auto sys = get_network_system();
              return sys && sys->lanchat_send_system_message_to_host_ex(
                  sender_id, sender_name, text, message_kind, correlation_id,
                  metadata_json).accepted;
          },
          nb::arg("sender_id"), nb::arg("sender_name"), nb::arg("text"),
          nb::arg("message_kind") = "action_status",
          nb::arg("correlation_id") = "", nb::arg("metadata_json") = "",
          "Send a host-only LANChat system message to the local host UI.");

    m.def("network_send_system_message_to_user_ex",
          [](const std::string& target_user_id,
             const std::string& sender_id,
             const std::string& sender_name,
             const std::string& text,
             const std::string& message_kind,
             const std::string& correlation_id,
             const std::string& metadata_json) -> bool {
              auto sys = get_network_system();
              return sys && sys->lanchat_send_system_message_to_user_ex(
                  target_user_id, sender_id, sender_name, text, message_kind,
                  correlation_id, metadata_json).accepted;
          },
          nb::arg("target_user_id"), nb::arg("sender_id"), nb::arg("sender_name"),
          nb::arg("text"), nb::arg("message_kind") = "action_status",
          nb::arg("correlation_id") = "", nb::arg("metadata_json") = "",
          "Send a local-user LANChat system message without room broadcast.");

    m.def("network_pop_lanchat_agent_trigger", []() -> nb::object {
        auto sys = get_network_system();
        if (!sys) return nb::none();
        auto trigger = sys->lanchat_pop_agent_trigger();
        if (!trigger.has_value()) return nb::none();

        nb::dict result;
        result["trigger_id"] = trigger->trigger_id;
        result["message_id"] = trigger->message_id;
        result["room_id"] = trigger->room_id;
        result["sender_id"] = trigger->sender_id;
        result["sender_name"] = trigger->sender_name;
        result["sender_type"] = trigger->sender_type;
        result["message_kind"] = trigger->message_kind;
        result["target_agent_id"] = trigger->target_agent_id;
        result["source_user_id"] = trigger->source_user_id;
        result["correlation_id"] = trigger->correlation_id;
        result["metadata_json"] = trigger->metadata_json;
        result["agent_id"] = trigger->agent_id;
        result["agent_name"] = trigger->agent_name;
        result["persona"] = trigger->persona;
        result["text"] = trigger->text;

        nb::list history;
        for (const auto& message : trigger->history) {
            nb::dict item;
            item["message_id"] = message.message_id;
            item["sender_id"] = message.sender_id;
            item["room_id"] = message.room_id;
            item["seq"] = message.seq;
            item["from"] = message.sender_name;
            item["text"] = message.text;
            item["ts"] = message.timestamp_ms / 1000;
            item["sender_type"] = message.sender_type;
            item["message_kind"] = message.message_kind;
            item["target_agent_id"] = message.target_agent_id;
            item["source_user_id"] = message.source_user_id;
            item["correlation_id"] = message.correlation_id;
            item["metadata_json"] = message.metadata_json;
            history.append(item);
        }
        result["history"] = history;
        return nb::object(result);
    }, "Pop one pending LANChat AI agent trigger owned by this peer.");

    m.def("network_pop_lanchat_coordinator_sync_message", []() -> nb::object {
        auto sys = get_network_system();
        if (!sys) return nb::none();
        auto message = sys->lanchat_pop_coordinator_sync_message();
        if (!message.has_value()) return nb::none();
        return nb::object(lanchat_message_to_dict(*message));
    }, "Pop one ordinary LANChat message that should be synced into InteractionCoordinator.");

    m.def("network_session_role_name", []() -> std::string {
        auto sys = get_network_system();
        if (!sys) return "none";
        return std::string(sys->session_role_name());
    }, "Return current networking session role: none, host, or client.");

    m.def("network_pop_lanchat_room_event", []() -> nb::object {
        auto sys = get_network_system();
        if (!sys) return nb::none();
        auto event = sys->lanchat_pop_room_event();
        if (!event.has_value()) return nb::none();
        nb::dict item;
        item["channel"] = "lanchat";
        item["event"] = event->event;
        item["room_id"] = event->room_id;
        return nb::object(item);
    }, "Pop one LANChat room lifecycle event for Python scheduler/Coordinator cleanup.");

    m.def("network_pop_lanchat_sync_event", []() -> nb::object {
        auto sys = get_network_system();
        if (!sys) return nb::none();
        auto event = sys->lanchat_pop_sync_event();
        if (!event.has_value()) return nb::none();
        nb::dict item;
        item["channel"] = "lanchat_sync";
        item["event"] = event->event;
        item["room_id"] = event->room_id;
        item["payload_json"] = event->payload_json;
        return nb::object(item);
    }, "Pop one LANChat actor/asset sync fact for AgentRuntime reconciliation.");

    m.def("network_lanchat_history_snapshot", [](int limit) -> nb::list {
        nb::list history;
        auto sys = get_network_system();
        if (!sys) return history;
        const auto& messages = sys->lanchat_history();
        const size_t total = messages.size();
        const size_t keep = limit > 0 ? std::min<size_t>(static_cast<size_t>(limit), total) : total;
        const size_t start = total > keep ? total - keep : 0;
        for (size_t i = start; i < total; ++i) {
            const auto& message = messages[i];
            nb::dict item;
            item["message_id"] = message.message_id;
            item["sender_id"] = message.sender_id;
            item["room_id"] = message.room_id;
            item["seq"] = message.seq;
            item["from"] = message.sender_name;
            item["text"] = message.text;
            item["ts"] = message.timestamp_ms / 1000;
            item["sender_type"] = message.sender_type;
            item["message_kind"] = message.message_kind;
            item["target_agent_id"] = message.target_agent_id;
            item["source_user_id"] = message.source_user_id;
            item["correlation_id"] = message.correlation_id;
            item["metadata_json"] = message.metadata_json;
            history.append(item);
        }
        return history;
    }, nb::arg("limit") = 20, "Return a recent LANChat history snapshot for Python AI/GM logic.");

    m.def("network_lanchat_agents_snapshot", []() -> nb::list {
        nb::list agents;
        auto sys = get_network_system();
        if (!sys) return agents;
        for (const auto& agent : sys->lanchat_agents()) {
            nb::dict item;
            item["agent_id"] = agent.agent_id;
            item["name"] = agent.name;
            item["persona"] = agent.persona;
            item["owner"] = agent.owner_id;
            agents.append(item);
        }
        return agents;
    }, "Return the C++ LANChat agent roster for Python AI/GM logic.");

    m.def("network_send_system_message", [](const std::string& sender_id,
                                             const std::string& sender_name,
                                             const std::string& text) -> bool {
        auto sys = get_network_system();
        return sys && sys->lanchat_send_agent_reply_ex(
            sender_id, sender_name, text, "system", "agent_reply").accepted;
    }, nb::arg("sender_id"), nb::arg("sender_name"), nb::arg("text"),
       "Send a GM/system message through the C++ reliable LANChat channel.");

    m.def("network_send_system_message_ex", [](const std::string& sender_id,
                                                const std::string& sender_name,
                                                const std::string& text,
                                                const std::string& message_kind,
                                                const std::string& correlation_id,
                                                const std::string& metadata_json) -> bool {
        auto sys = get_network_system();
        return sys && sys->lanchat_send_agent_reply_ex(
            sender_id, sender_name, text, "system",
            message_kind.empty() ? "agent_reply" : message_kind,
            "", "", correlation_id, metadata_json).accepted;
    }, nb::arg("sender_id"), nb::arg("sender_name"), nb::arg("text"),
       nb::arg("message_kind") = "agent_reply", nb::arg("correlation_id") = "",
       nb::arg("metadata_json") = "",
       "Send a structured GM/system message through the C++ reliable LANChat channel.");

    m.def("network_lock_object", [](const std::string& object_id,
                                     const std::string& user_id,
                                     const std::string& operation) -> bool {
        auto sys = get_network_system();
        return sys && sys->lanchat_lock_object(
            object_id, user_id, operation, network_now_ms()).ok;
    }, nb::arg("object_id"), nb::arg("user_id"), nb::arg("operation") = "modify",
       "Acquire an object collaboration lock through C++ state.");

    m.def("network_unlock_object", [](const std::string& object_id,
                                       const std::string& user_id) -> bool {
        auto sys = get_network_system();
        return sys && sys->lanchat_unlock_object(object_id, user_id).ok;
    }, nb::arg("object_id"), nb::arg("user_id"),
       "Release an object collaboration lock through C++ state.");

    m.def("network_locked_by", [](const std::string& object_id) -> std::string {
        auto sys = get_network_system();
        return sys ? sys->lanchat_locked_by(object_id, network_now_ms()) : std::string{};
    }, nb::arg("object_id"), "Return the owner of a C++ collaboration lock.");

    m.def("network_broadcast_intent", [](const std::string& user_id,
                                          const std::string& tooltip,
                                          const std::array<float, 3>& position,
                                          const std::string& status) {
        auto sys = get_network_system();
        if (sys) {
            sys->lanchat_broadcast_intent(user_id, tooltip, position, status, network_now_ms());
        }
    }, nb::arg("user_id"), nb::arg("tooltip"), nb::arg("position"),
       nb::arg("status") = "placing_object",
       "Record a local operation preview intent in C++ collaboration state.");

    m.def("network_check_preview_collision", [](const std::string& user_id,
                                                 const std::array<float, 3>& position,
                                                 float delta) -> std::string {
        auto sys = get_network_system();
        return sys ? sys->lanchat_check_preview_collision(
                         user_id, position, delta, network_now_ms())
                   : std::string{};
    }, nb::arg("user_id"), nb::arg("position"), nb::arg("delta") = 0.5f,
       "Return the conflicting user id for a preview placement, if any.");
}

}  // namespace EngineScripts
