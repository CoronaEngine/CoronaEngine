#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#endif

#include "browser_manager.h"
#include "cef_client.h"

#include <corona/events/acoustics_system_events.h>
#include <corona/kernel/core/kernel_context.h>
#include <corona/systems/network/network_system.h>

#include <cstdint>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Corona::Systems::UI {

namespace {
std::string create_success_json(const std::string& func,
                                const nlohmann::json& data) {
    nlohmann::json r;
    r["success"] = true;
    r["data"] = data;
    r["function"] = func;
    return r.dump();
}

// Resolve the NetworkSystem from the kernel's system manager.
// Returns nullptr if unavailable.
std::shared_ptr<Corona::Systems::NetworkSystem> get_network_system() {
    auto sys_mgr = Corona::Kernel::KernelContext::instance().system_manager();
    if (!sys_mgr) return nullptr;
    return std::dynamic_pointer_cast<Corona::Systems::NetworkSystem>(
        sys_mgr->get_system("Network"));
}

std::uintptr_t json_to_uintptr(const nlohmann::json& value) {
    try {
        if (value.is_string()) {
            return static_cast<std::uintptr_t>(std::stoull(value.get<std::string>()));
        }
        if (value.is_number_unsigned()) {
            return static_cast<std::uintptr_t>(value.get<uint64_t>());
        }
        if (value.is_number_integer()) {
            auto v = value.get<int64_t>();
            return v > 0 ? static_cast<std::uintptr_t>(v) : 0;
        }
    } catch (...) {
    }
    return 0;
}

Corona::Systems::NetworkSystem::SessionRole parse_network_session_role(
    const nlohmann::json& value) {
    if (!value.is_string()) {
        return Corona::Systems::NetworkSystem::SessionRole::Host;
    }
    auto role = value.get<std::string>();
    if (role == "client") {
        return Corona::Systems::NetworkSystem::SessionRole::Client;
    }
    if (role == "none") {
        return Corona::Systems::NetworkSystem::SessionRole::None;
    }
    return Corona::Systems::NetworkSystem::SessionRole::Host;
}

std::string detect_wlan_ipv4();

nlohmann::json build_network_session_info(
    const std::shared_ptr<Corona::Systems::NetworkSystem>& sys) {
    nlohmann::json payload;
    payload["ok"] = true;
    payload["active"] =
        sys->session_state() == Corona::Systems::NetworkSystem::SessionState::Active;
    payload["role"] = std::string(sys->session_role_name());
    payload["peer_count"] = static_cast<int>(sys->peer_count());
    payload["host_address"] = sys->host_address();
    payload["host_port"] = sys->host_port();
    payload["listen_port"] = sys->session_port();
    payload["local_ip"] = detect_wlan_ipv4();
    return payload;
}

void emit_lanchat_event_json(const std::string& event_json) {
    if (event_json.empty()) {
        return;
    }
    std::string js = "if(window.__coronaEmit)window.__coronaEmit(" +
                     nlohmann::json("lanchat-event").dump() + "," +
                     event_json + ",{\"_fromCross\":1})";
    for (auto& [tab_id, tab] : BrowserManager::instance().get_tabs()) {
        if (tab && !tab->minimized && tab->client && tab->client->GetBrowser()) {
            tab->client->GetBrowser()->GetMainFrame()->ExecuteJavaScript(js, "", 0);
        }
    }
}

nlohmann::json build_lanchat_members(
    const std::vector<Corona::Network::LanChatMember>& members) {
    nlohmann::json result = nlohmann::json::array();
    for (const auto& member : members) {
        result.push_back(member.nickname);
    }
    return result;
}

nlohmann::json build_lanchat_member_details(
    const std::vector<Corona::Network::LanChatMember>& members) {
    nlohmann::json result = nlohmann::json::array();
    for (const auto& member : members) {
        result.push_back({
            {"member_id", member.member_id},
            {"nickname", member.nickname},
            {"status", member.status},
        });
    }
    return result;
}

nlohmann::json build_lanchat_history(
    const std::vector<Corona::Network::LanChatMessage>& history) {
    nlohmann::json result = nlohmann::json::array();
    for (const auto& message : history) {
        result.push_back({
            {"message_id", message.message_id},
            {"sender_id", message.sender_id},
            {"room_id", message.room_id},
            {"seq", message.seq},
            {"from", message.sender_name},
            {"text", message.text},
            {"ts", message.timestamp_ms / 1000},
            {"sender_type", message.sender_type},
            {"message_kind", message.message_kind},
            {"target_agent_id", message.target_agent_id},
            {"source_user_id", message.source_user_id},
            {"correlation_id", message.correlation_id},
            {"metadata_json", message.metadata_json},
        });
    }
    return result;
}

nlohmann::json build_lanchat_history_rooms(
    const std::vector<Corona::Network::LanChatHistoryRoomSummary>& rooms) {
    nlohmann::json result = nlohmann::json::array();
    for (const auto& room : rooms) {
        const std::string load_id = room.session_id.empty() ? room.room_id : room.session_id;
        result.push_back({
            {"room_id", load_id},
            {"session_id", load_id},
            {"display_room_id", room.room_id},
            {"message_count", room.message_count},
            {"last_timestamp_ms", room.last_timestamp_ms},
            {"last_ts", room.last_timestamp_ms / 1000},
            {"last_sender_name", room.last_sender_name},
            {"last_text", room.last_text},
        });
    }
    return result;
}

nlohmann::json build_lanchat_agents(
    const std::vector<Corona::Network::LanChatAgent>& agents) {
    nlohmann::json result = nlohmann::json::array();
    for (const auto& agent : agents) {
        result.push_back({
            {"agent_id", agent.agent_id},
            {"name", agent.name},
            {"persona", agent.persona},
            {"owner", agent.owner_id},
        });
    }
    return result;
}

std::string make_agent_id(const std::string& owner, const std::string& name) {
    static uint64_t counter = 0;
    std::ostringstream out;
    out << "agent-" << std::hash<std::string>{}(owner + ":" + name) << "-" << ++counter;
    return out.str();
}

std::string to_lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool looks_like_wlan_adapter(const std::string& adapter_name) {
    const std::string name = to_lower_ascii(adapter_name);
    return name.find("wlan") != std::string::npos ||
           name.find("wi-fi") != std::string::npos ||
           name.find("wifi") != std::string::npos ||
           name.find("wireless") != std::string::npos ||
           name.find("无线") != std::string::npos;
}

bool is_usable_ipv4(const std::string& ip) {
    return !ip.empty() && ip.rfind("127.", 0) != 0 && ip != "0.0.0.0";
}

std::string detect_hostname_ipv4() {
    char host_name[256] = {};
    if (gethostname(host_name, sizeof(host_name)) != 0) {
        return "127.0.0.1";
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host_name, nullptr, &hints, &result) != 0) {
        return "127.0.0.1";
    }

    std::string fallback = "127.0.0.1";
    for (addrinfo* it = result; it; it = it->ai_next) {
        auto* addr = reinterpret_cast<sockaddr_in*>(it->ai_addr);
        char ip[INET_ADDRSTRLEN] = {};
        if (!inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip))) {
            continue;
        }
        std::string candidate(ip);
        if (is_usable_ipv4(candidate)) {
            fallback = candidate;
            break;
        }
    }
    freeaddrinfo(result);
    return fallback;
}

#ifdef _WIN32
std::string wide_to_utf8(const wchar_t* value) {
    if (!value || !*value) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string result(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::string ipv4_from_sockaddr(const sockaddr* address) {
    if (!address || address->sa_family != AF_INET) {
        return {};
    }
    const auto* addr = reinterpret_cast<const sockaddr_in*>(address);
    char ip[INET_ADDRSTRLEN] = {};
    if (!inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip))) {
        return {};
    }
    return std::string(ip);
}

bool adapter_has_ipv4_gateway(const IP_ADAPTER_ADDRESSES* adapter) {
    for (auto* gateway = adapter ? adapter->FirstGatewayAddress : nullptr; gateway; gateway = gateway->Next) {
        if (gateway->Address.lpSockaddr &&
            gateway->Address.lpSockaddr->sa_family == AF_INET) {
            return true;
        }
    }
    return false;
}
#endif

std::string detect_wlan_ipv4() {
#ifdef _WIN32
    WSADATA wsa{};
    const bool started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    std::string wlan_with_gateway;
    std::string wlan_fallback;
    std::string gateway_fallback;
    std::string any_fallback;

    ULONG buffer_size = 15000;
    std::vector<unsigned char> buffer(buffer_size);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST |
                  GAA_FLAG_SKIP_MULTICAST |
                  GAA_FLAG_SKIP_DNS_SERVER |
                  GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG result = GetAdaptersAddresses(
        AF_INET, flags, nullptr,
        reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
        &buffer_size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buffer_size);
        result = GetAdaptersAddresses(
            AF_INET, flags, nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()),
            &buffer_size);
    }

    if (result == NO_ERROR) {
        auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
            if (adapter->OperStatus != IfOperStatusUp ||
                adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
                continue;
            }

            std::string adapter_name = wide_to_utf8(adapter->FriendlyName);
            if (adapter->AdapterName) {
                adapter_name += " ";
                adapter_name += adapter->AdapterName;
            }
            const bool is_wlan = looks_like_wlan_adapter(adapter_name);
            const bool has_gateway = adapter_has_ipv4_gateway(adapter);

            for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
                const std::string ip = ipv4_from_sockaddr(unicast->Address.lpSockaddr);
                if (!is_usable_ipv4(ip)) {
                    continue;
                }
                if (is_wlan && has_gateway && wlan_with_gateway.empty()) {
                    wlan_with_gateway = ip;
                }
                if (is_wlan && wlan_fallback.empty()) {
                    wlan_fallback = ip;
                }
                if (has_gateway && gateway_fallback.empty()) {
                    gateway_fallback = ip;
                }
                if (any_fallback.empty()) {
                    any_fallback = ip;
                }
            }
        }
    }

    std::string selected = !wlan_with_gateway.empty() ? wlan_with_gateway :
                           !wlan_fallback.empty() ? wlan_fallback :
                           !gateway_fallback.empty() ? gateway_fallback :
                           !any_fallback.empty() ? any_fallback :
                           detect_hostname_ipv4();
    if (started) WSACleanup();
#endif
#ifdef _WIN32
    return selected;
#else
    return detect_hostname_ipv4();
#endif
}
}  // namespace

BrowserSideJSHandler::~BrowserSideJSHandler() {
    PyGILState_STATE state = PyGILState_Ensure();
    Py_XDECREF(pFunc_);
    PyGILState_Release(state);
}

void BrowserSideJSHandler::initialize_python() {
    if (!Py_IsInitialized()) {
        Py_Initialize();
        PyEval_SaveThread();
    }

    PyGILState_STATE state = PyGILState_Ensure();
    PyObject* pModule = nullptr;

    try {
        PyRun_SimpleString("import sys");
        PyRun_SimpleString("import os");
        PyRun_SimpleString("sys.path.insert(0, os.path.join(os.getcwd(), 'CabbageEditor'))");

        PyObject* pName = PyUnicode_FromString("main");
        if (!pName) {
            throw std::runtime_error("Failed to create module name");
        }

        pModule = PyImport_Import(pName);
        Py_DECREF(pName);

        if (!pModule) {
            PyErr_Print();
            PyGILState_Release(state);
            throw std::runtime_error("Failed to import Python module 'main'");
        }

        PyObject* pClass = PyObject_GetAttrString(pModule, "editor");
        if (!pClass) {
            Py_DECREF(pModule);
            PyErr_Print();
            PyGILState_Release(state);
            throw std::runtime_error("Failed to get 'editor' attribute from module");
        }

        if (PyCallable_Check(pClass)) {
            pFunc_ = PyObject_GetAttrString(pClass, "deal_func_from_js");
        }

        Py_DECREF(pClass);
        Py_DECREF(pModule);

    } catch (const std::exception&) {
        if (pModule) {
            Py_DECREF(pModule);
        }
        PyErr_Print();
        PyGILState_Release(state);
        throw;
    }

    PyGILState_Release(state);
}

bool BrowserSideJSHandler::OnQuery(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   int64_t query_id,
                                   const CefString& request,
                                   bool persistent,
                                   CefRefPtr<Callback> callback) {
    CEF_REQUIRE_UI_THREAD();
    std::string req = request.ToString();

    // ── SceneTools.play_audio / stop_audio：C++ 快速通道，不走 Python ──
    // 前端 Bridge.callCEF(\"SceneTools\", \"play_audio\", [rid, loop]) 直接在此处理，
    // 避免持有 GIL 阻塞 Python 线程。
    if (req.find("\"SceneTools\"") != std::string::npos) {
        try {
            auto j = nlohmann::json::parse(req);
            if (j.value("module", "") == "SceneTools") {
                std::string func = j.value("function", "");
                auto args = j.value("args", nlohmann::json::array());

                if (func == "play_audio" || func == "stop_audio") {
                    auto* event_bus = Corona::Kernel::KernelContext::instance().event_bus();
                    if (!event_bus) {
                        callback->Failure(2, "event_bus unavailable");
                        return true;
                    }

                    // resource_id 以字符串传递（JS number 无法精确表示 64 位整数）。
                    // 兼容字符串和数字两种 JSON 形态。
                    uint64_t rid = 0;
                    if (args.size() > 0) {
                        if (args[0].is_string()) {
                            try {
                                rid = std::stoull(args[0].get<std::string>());
                            } catch (...) {
                                rid = 0;
                            }
                        } else if (args[0].is_number_unsigned()) {
                            rid = args[0].get<uint64_t>();
                        }
                    }
                    if (rid == 0) {
                        callback->Failure(2, "invalid resource_id");
                        return true;
                    }

                    if (func == "play_audio") {
                        bool loop = args.size() > 1 ? args[1].get<bool>() : false;
                        event_bus->publish<::Corona::Events::PlayAudioEvent>({rid, loop});
                    } else {
                        event_bus->publish<::Corona::Events::StopAudioEvent>({rid});
                    }

                    nlohmann::json payload;
                    payload["ok"] = true;
                    callback->Success(create_success_json(func, payload));
                    return true;
                }
            }
        } catch (...) {
            callback->Failure(2, "SceneTools fast path error");
            return true;
        }
    }

    // ── Network 模块：C++ 直接处理，不走 Python ──
    // LAN 协同编辑的 start/stop/peer_count 全部由 C++ 直接响应，避免高频
    // get_peer_count 轮询在持有 GIL 时阻塞 SystemManager 锁导致的关闭死锁。
    if (req.find("\"Network\"") != std::string::npos) {
        try {
            auto j = nlohmann::json::parse(req);
            if (j.value("module", "") == "Network") {
                std::string func = j.value("function", "");
                auto args = j.value("args", nlohmann::json::array());

                auto sys = get_network_system();
                if (!sys) {
                    callback->Failure(2, "NetworkSystem unavailable");
                    return true;
                }

                if (func == "start_session") {
                    // args: [instance_name, project_id, port, role]
                    std::string name = args.size() > 0 ? args[0].get<std::string>() : "";
                    uint64_t project_id = args.size() > 1 ? args[1].get<uint64_t>() : 0;
                    uint16_t port = args.size() > 2 ? args[2].get<uint16_t>() : 27960;
                    auto role = args.size() > 3
                        ? parse_network_session_role(args[3])
                        : Corona::Systems::NetworkSystem::SessionRole::Host;

                    bool ok = sys->start_session(name, project_id, port, role);
                    nlohmann::json payload = build_network_session_info(sys);
                    payload["ok"] = ok;
                    callback->Success(create_success_json("start_session", payload));
                    return true;
                }

                if (func == "stop_session") {
                    sys->stop_session();
                    nlohmann::json payload;
                    payload["ok"] = true;
                    callback->Success(create_success_json("stop_session", payload));
                    return true;
                }

                if (func == "get_peer_count") {
                    nlohmann::json payload = build_network_session_info(sys);
                    callback->Success(create_success_json("get_peer_count", payload));
                    return true;
                }

                if (func == "get_session_info") {
                    callback->Success(create_success_json(
                        "get_session_info", build_network_session_info(sys)));
                    return true;
                }

                if (func == "connect_to_peer") {
                    // args: [ip, port, peer_name]
                    std::string ip = args.size() > 0 ? args[0].get<std::string>() : "";
                    uint16_t port = args.size() > 1 ? args[1].get<uint16_t>() : 27960;
                    std::string peer_name = args.size() > 2 ? args[2].get<std::string>() : "";

                    bool ok = sys->connect_to_peer(ip, port, peer_name);
                    nlohmann::json payload = build_network_session_info(sys);
                    payload["ok"] = ok;
                    callback->Success(create_success_json("connect_to_peer", payload));
                    return true;
                }

                if (func == "set_project_root") {
                    // args: [project_root_path]
                    std::string root = args.size() > 0 ? args[0].get<std::string>() : "";
                    if (!root.empty()) {
                        sys->set_project_root(root);
                    }
                    nlohmann::json payload;
                    payload["ok"] = true;
                    callback->Success(create_success_json("set_project_root", payload));
                    return true;
                }

                if (func == "poll_pending_actor_create") {
                    // Called by frontend every ~500ms when session is active.
                    // Returns the next pending actor create entry, so Python
                    // can call SceneTools.create_actor_internal.
                    nlohmann::json payload;
                    std::string actor_guid, scene_name, model_path;
                    std::string actor_json;
                    Network::ActorCreatePacked packed;
                    if (sys->pop_pending_actor_create(actor_guid, scene_name, model_path,
                                                       &packed, sizeof(packed), &actor_json)) {
                        payload["has_pending"] = true;
                        payload["actor_guid"] = actor_guid;
                        payload["scene_name"] = scene_name;
                        payload["model_path"] = model_path;
                        // Convert transform (9 floats) and optics for Python
                        nlohmann::json actor_data = nlohmann::json::object();
                        if (!actor_json.empty()) {
                            try {
                                actor_data = nlohmann::json::parse(actor_json);
                                if (!actor_data.is_object()) {
                                    actor_data = nlohmann::json::object();
                                }
                            } catch (const nlohmann::json::parse_error&) {
                                actor_data = nlohmann::json::object();
                            }
                        }
                        actor_data["geometry"]["position"] = {
                            packed.transform[0], packed.transform[1], packed.transform[2]
                        };
                        actor_data["geometry"]["rotation"] = {
                            packed.transform[3], packed.transform[4], packed.transform[5]
                        };
                        actor_data["geometry"]["scale"] = {
                            packed.transform[6], packed.transform[7], packed.transform[8]
                        };
                        payload["actor_data"] = actor_data;
                    } else {
                        payload["has_pending"] = false;
                    }
                    payload["ok"] = true;
                    callback->Success(create_success_json("poll_pending_actor_create", payload));
                    return true;
                }

                if (func == "poll_pending_actor_transform") {
                    nlohmann::json payload;
                    std::string actor_guid, scene_name, source_user_id, correlation_id;
                    float transform[9] = {0,0,0, 0,0,0, 1,1,1};
                    if (sys->pop_pending_actor_transform_update(
                            actor_guid, scene_name, transform, 9,
                            source_user_id, correlation_id)) {
                        payload["has_pending"] = true;
                        payload["actor_guid"] = actor_guid;
                        payload["scene_name"] = scene_name;
                        payload["source_user_id"] = source_user_id;
                        payload["correlation_id"] = correlation_id;
                        payload["geometry"]["position"] = {
                            transform[0], transform[1], transform[2]
                        };
                        payload["geometry"]["rotation"] = {
                            transform[3], transform[4], transform[5]
                        };
                        payload["geometry"]["scale"] = {
                            transform[6], transform[7], transform[8]
                        };
                    } else {
                        payload["has_pending"] = false;
                    }
                    payload["ok"] = true;
                    callback->Success(create_success_json("poll_pending_actor_transform", payload));
                    return true;
                }

                if (func == "poll_pending_actor_delete") {
                    nlohmann::json payload;
                    std::string actor_guid, scene_name, actor_name;
                    if (sys->pop_pending_actor_delete(actor_guid, scene_name, actor_name)) {
                        payload["has_pending"] = true;
                        payload["actor_guid"] = actor_guid;
                        payload["scene_name"] = scene_name;
                        payload["actor_name"] = actor_name;
                    } else {
                        payload["has_pending"] = false;
                    }
                    payload["ok"] = true;
                    callback->Success(create_success_json("poll_pending_actor_delete", payload));
                    return true;
                }

                if (func == "poll_pending_actor_scene_snapshot_request") {
                    nlohmann::json payload;
                    std::string scene_name;
                    if (sys->pop_pending_actor_scene_snapshot_request(scene_name)) {
                        payload["has_pending"] = true;
                        payload["scene_name"] = scene_name;
                    } else {
                        payload["has_pending"] = false;
                    }
                    payload["ok"] = true;
                    callback->Success(create_success_json(
                        "poll_pending_actor_scene_snapshot_request", payload));
                    return true;
                }

                if (func == "poll_pending_actor_scene_snapshot") {
                    nlohmann::json payload;
                    std::string scene_name, snapshot_json;
                    if (sys->pop_pending_actor_scene_snapshot(scene_name, snapshot_json)) {
                        payload["has_pending"] = true;
                        payload["scene_name"] = scene_name;
                        payload["snapshot_json"] = snapshot_json;
                    } else {
                        payload["has_pending"] = false;
                    }
                    payload["ok"] = true;
                    callback->Success(create_success_json(
                        "poll_pending_actor_scene_snapshot", payload));
                    return true;
                }

                if (func == "poll_pending_actor_state_update") {
                    nlohmann::json payload;
                    std::string actor_guid, scene_name, actor_json;
                    if (sys->pop_pending_actor_state_update(actor_guid, scene_name, actor_json)) {
                        payload["has_pending"] = true;
                        payload["actor_guid"] = actor_guid;
                        payload["scene_name"] = scene_name;
                        payload["actor_json"] = actor_json;
                    } else {
                        payload["has_pending"] = false;
                    }
                    payload["ok"] = true;
                    callback->Success(create_success_json(
                        "poll_pending_actor_state_update", payload));
                    return true;
                }

                if (func == "set_sync_paused") {
                    bool paused = args.size() > 0 ? args[0].get<bool>() : false;
                    sys->set_sync_paused(paused);
                    nlohmann::json payload;
                    payload["ok"] = true;
                    callback->Success(create_success_json("set_sync_paused", payload));
                    return true;
                }

                if (func == "register_actor_identity") {
                    // args: [actor_guid, actor_handle, locally_owned]
                    std::string actor_guid = args.size() > 0 ? args[0].get<std::string>() : "";
                    std::uintptr_t actor_handle = args.size() > 1 ? json_to_uintptr(args[1]) : 0;
                    bool locally_owned = args.size() > 2 ? args[2].get<bool>() : true;
                    bool ok = sys->register_actor_identity(
                        actor_guid, actor_handle, locally_owned);
                    nlohmann::json payload;
                    payload["ok"] = ok;
                    callback->Success(create_success_json("register_actor_identity", payload));
                    return true;
                }

                if (func == "claim_actor_ownership") {
                    // args: [actor_guid]
                    std::string actor_guid = args.size() > 0 ? args[0].get<std::string>() : "";
                    bool ok = sys->claim_actor_ownership(actor_guid);
                    nlohmann::json payload;
                    payload["ok"] = ok;
                    callback->Success(create_success_json("claim_actor_ownership", payload));
                    return true;
                }

                if (func == "broadcast_actor_transform") {
                    std::string actor_guid = args.size() > 0 ? args[0].get<std::string>() : "";
                    std::string scene_name = args.size() > 1 ? args[1].get<std::string>() : "";
                    float transform[9] = {0,0,0, 0,0,0, 1,1,1};
                    std::string source_user_id;
                    std::string correlation_id;
                    if (args.size() > 2 && args[2].is_object()) {
                        auto& ad = args[2];
                        if (ad.contains("geometry")) {
                            auto& geo = ad["geometry"];
                            if (geo.contains("position") && geo["position"].is_array() && geo["position"].size() >= 3) {
                                transform[0] = geo["position"][0].get<float>();
                                transform[1] = geo["position"][1].get<float>();
                                transform[2] = geo["position"][2].get<float>();
                            }
                            if (geo.contains("rotation") && geo["rotation"].is_array() && geo["rotation"].size() >= 3) {
                                transform[3] = geo["rotation"][0].get<float>();
                                transform[4] = geo["rotation"][1].get<float>();
                                transform[5] = geo["rotation"][2].get<float>();
                            }
                            if (geo.contains("scale") && geo["scale"].is_array() && geo["scale"].size() >= 3) {
                                transform[6] = geo["scale"][0].get<float>();
                                transform[7] = geo["scale"][1].get<float>();
                                transform[8] = geo["scale"][2].get<float>();
                            }
                        }
                        source_user_id = ad.value("source_user_id", "");
                        correlation_id = ad.value("correlation_id", "");
                    }
                    sys->broadcast_actor_transform_update(
                        actor_guid, scene_name, transform, source_user_id, correlation_id);
                    nlohmann::json payload;
                    payload["ok"] = true;
                    callback->Success(create_success_json("broadcast_actor_transform", payload));
                    return true;
                }

                if (func == "broadcast_actor_delete") {
                    std::string actor_guid = args.size() > 0 ? args[0].get<std::string>() : "";
                    std::string scene_name = args.size() > 1 ? args[1].get<std::string>() : "";
                    std::string actor_name = args.size() > 2 ? args[2].get<std::string>() : "";
                    sys->broadcast_actor_delete(actor_guid, scene_name, actor_name);
                    nlohmann::json payload;
                    payload["ok"] = true;
                    callback->Success(create_success_json("broadcast_actor_delete", payload));
                    return true;
                }

                if (func == "request_actor_scene_snapshot") {
                    std::string scene_name = args.size() > 0 ? args[0].get<std::string>() : "";
                    sys->request_actor_scene_snapshot(scene_name);
                    nlohmann::json payload;
                    payload["ok"] = true;
                    callback->Success(create_success_json(
                        "request_actor_scene_snapshot", payload));
                    return true;
                }

                if (func == "broadcast_actor_scene_snapshot") {
                    std::string scene_name = args.size() > 0 ? args[0].get<std::string>() : "";
                    std::string snapshot_json;
                    if (args.size() > 1) {
                        if (args[1].is_string()) {
                            snapshot_json = args[1].get<std::string>();
                        } else {
                            snapshot_json = args[1].dump();
                        }
                    }
                    sys->broadcast_actor_scene_snapshot(scene_name, snapshot_json);
                    nlohmann::json payload;
                    payload["ok"] = true;
                    callback->Success(create_success_json(
                        "broadcast_actor_scene_snapshot", payload));
                    return true;
                }

                if (func == "broadcast_actor_state_update") {
                    std::string actor_guid = args.size() > 0 ? args[0].get<std::string>() : "";
                    std::string scene_name = args.size() > 1 ? args[1].get<std::string>() : "";
                    std::string actor_json;
                    if (args.size() > 2) {
                        if (args[2].is_string()) {
                            actor_json = args[2].get<std::string>();
                        } else {
                            actor_json = args[2].dump();
                        }
                    }
                    sys->broadcast_actor_state_update(actor_guid, scene_name, actor_json);
                    nlohmann::json payload;
                    payload["ok"] = true;
                    callback->Success(create_success_json(
                        "broadcast_actor_state_update", payload));
                    return true;
                }

                if (func == "broadcast_actor_create") {
                    // args: [actor_guid, scene_name, model_path, actor_data_dict]
                    std::string actor_guid = args.size() > 0 ? args[0].get<std::string>() : "";
                    std::string scene_name = args.size() > 1 ? args[1].get<std::string>() : "";
                    std::string model_path = args.size() > 2 ? args[2].get<std::string>() : "";
                    // actor_data is a dict with geometry.position/rotation/scale
                    // Extract transform (9 floats) — default to identity
                    float transform[9] = {0,0,0, 0,0,0, 1,1,1};
                    std::vector<std::string> dependency_paths;
                    std::string actor_json;
                    if (args.size() > 3 && args[3].is_object()) {
                        auto& ad = args[3];
                        actor_json = ad.dump();
                        if (actor_guid.empty() && ad.contains("actor_guid") && ad["actor_guid"].is_string()) {
                            actor_guid = ad["actor_guid"].get<std::string>();
                        }
                        if (ad.contains("model_dependencies") && ad["model_dependencies"].is_array()) {
                            for (const auto& dep : ad["model_dependencies"]) {
                                if (dep.is_string()) {
                                    dependency_paths.push_back(dep.get<std::string>());
                                }
                            }
                        }
                        if (ad.contains("geometry")) {
                            auto& geo = ad["geometry"];
                            if (geo.contains("position") && geo["position"].is_array() && geo["position"].size() >= 3) {
                                transform[0] = geo["position"][0].get<float>();
                                transform[1] = geo["position"][1].get<float>();
                                transform[2] = geo["position"][2].get<float>();
                            }
                            if (geo.contains("rotation") && geo["rotation"].is_array() && geo["rotation"].size() >= 3) {
                                transform[3] = geo["rotation"][0].get<float>();
                                transform[4] = geo["rotation"][1].get<float>();
                                transform[5] = geo["rotation"][2].get<float>();
                            }
                            if (geo.contains("scale") && geo["scale"].is_array() && geo["scale"].size() >= 3) {
                                transform[6] = geo["scale"][0].get<float>();
                                transform[7] = geo["scale"][1].get<float>();
                                transform[8] = geo["scale"][2].get<float>();
                            }
                        }
                    }
                    if (actor_guid.empty()) {
                        actor_guid = scene_name + ":" + model_path;
                    }
                    // Build default optics (all defaults)
                    Network::ActorCreatePacked opt;
                    std::memset(&opt, 0, sizeof(opt));
                    opt.visible = true;
                    opt.bEnableLighting = true;
                    opt.metallic = 0.0f;
                    opt.roughness = 0.5f;
                    opt.specular = 0.5f;
                    opt.specularTint = 0.0f;
                    opt.sheen = 0.0f;
                    opt.sheenTint = 0.5f;
                    opt.clearcoat = 0.0f;
                    opt.clearcoatGloss = 1.0f;
                    opt.ambient[0] = 0.2f; opt.ambient[1] = 0.2f; opt.ambient[2] = 0.2f;
                    opt.diffuse[0] = 0.8f; opt.diffuse[1] = 0.8f; opt.diffuse[2] = 0.8f;
                    opt.specular_color[0] = 1.0f; opt.specular_color[1] = 1.0f; opt.specular_color[2] = 1.0f;
                    opt.shininess = 32.0f;

                    sys->broadcast_actor_create(actor_guid, scene_name, model_path,
                                                dependency_paths, transform,
                                                &opt, sizeof(opt), actor_json);
                    nlohmann::json payload;
                    payload["ok"] = true;
                    callback->Success(create_success_json("broadcast_actor_create", payload));
                    return true;
                }

                callback->Failure(1, "Unknown Network function: " + func);
                return true;
            }
        } catch (const nlohmann::json::parse_error&) {
            // 非合法 JSON，继续走后续路径
        }
    }

    // ── LANChat 模块：C++ NetworkSystem 接管，不再进入 Python LANChat 插件 ──
    if (req.find("\"LANChat\"") != std::string::npos) {
        try {
            auto j = nlohmann::json::parse(req);
            if (j.value("module", "") == "LANChat") {
                std::string func = j.value("function", "");
                auto args = j.value("args", nlohmann::json::array());
                const nlohmann::json payload_arg =
                    (args.size() > 0 && args[0].is_object()) ? args[0] : nlohmann::json::object();

                auto sys = get_network_system();
                if (!sys) {
                    callback->Failure(2, "NetworkSystem unavailable");
                    return true;
                }
                sys->set_lanchat_event_callback(emit_lanchat_event_json);

                if (func == "start_room") {
                    const std::string room = payload_arg.value("room", "");
                    const uint16_t port = payload_arg.value("port", 27960);
                    const std::string nickname = payload_arg.value("nickname", "房主");
                    const bool restore_history = payload_arg.value("restore_history", false);
                    const std::string history_room = payload_arg.value("history_room", room);
                    const std::string host_nickname = nickname.empty() ? "房主" : nickname;
                    bool ok = sys->lanchat_start_room(room, host_nickname, port);
                    bool restored_history = false;
                    if (ok && restore_history) {
                        restored_history = sys->lanchat_restore_history_room(history_room);
                    }
                    const uint16_t actual_port = sys->session_port() != 0 ? sys->session_port() : port;
                    nlohmann::json data;
                    data["ok"] = ok;
                    data["you"] = host_nickname;
                    data["ip"] = detect_wlan_ipv4();
                    data["port"] = actual_port;
                    data["room"] = room;
                    data["peer_id"] = sys->local_peer_id();
                    data["members"] = build_lanchat_members(sys->lanchat_members());
                    data["member_details"] = build_lanchat_member_details(sys->lanchat_members());
                    data["history"] = build_lanchat_history(sys->lanchat_history());
                    data["agents"] = build_lanchat_agents(sys->lanchat_agents());
                    data["restored_history"] = restored_history;
                    callback->Success(create_success_json(func, data));
                    return true;
                }

                if (func == "start_local_room") {
                    const std::string room = payload_arg.value("room", "");
                    const std::string nickname = payload_arg.value("nickname", "房主");
                    const bool restore_history = payload_arg.value("restore_history", false);
                    const std::string history_room = payload_arg.value("history_room", room);
                    const std::string host_nickname = nickname.empty() ? "房主" : nickname;
                    bool ok = sys->lanchat_start_local_room(room, host_nickname);
                    bool restored_history = false;
                    if (ok && restore_history) {
                        restored_history = sys->lanchat_restore_history_room(history_room);
                    }
                    nlohmann::json data;
                    data["ok"] = ok;
                    data["you"] = host_nickname;
                    data["ip"] = "";
                    data["port"] = 0;
                    data["room"] = room;
                    data["mode"] = "single";
                    data["peer_id"] = "local-single-player";
                    data["members"] = build_lanchat_members(sys->lanchat_members());
                    data["member_details"] = build_lanchat_member_details(sys->lanchat_members());
                    data["history"] = build_lanchat_history(sys->lanchat_history());
                    data["agents"] = build_lanchat_agents(sys->lanchat_agents());
                    data["restored_history"] = restored_history;
                    callback->Success(create_success_json(func, data));
                    return true;
                }

                if (func == "stop_room") {
                    sys->lanchat_leave_room();
                    sys->stop_session();
                    nlohmann::json data;
                    data["ok"] = true;
                    callback->Success(create_success_json(func, data));
                    return true;
                }

                if (func == "stop_local_room") {
                    sys->lanchat_stop_local_room();
                    nlohmann::json data;
                    data["ok"] = true;
                    callback->Success(create_success_json(func, data));
                    return true;
                }

                if (func == "get_history") {
                    nlohmann::json data;
                    data["ok"] = true;
                    data["history"] = build_lanchat_history(sys->lanchat_history());
                    callback->Success(create_success_json(func, data));
                    return true;
                }

                if (func == "list_history_rooms") {
                    nlohmann::json data;
                    data["ok"] = true;
                    data["rooms"] = build_lanchat_history_rooms(sys->lanchat_history_rooms());
                    callback->Success(create_success_json(func, data));
                    return true;
                }

                if (func == "load_history_room") {
                    const std::string room = payload_arg.value("room", "");
                    nlohmann::json data;
                    data["ok"] = !room.empty();
                    data["room"] = room;
                    data["history"] = room.empty()
                        ? nlohmann::json::array()
                        : build_lanchat_history(sys->lanchat_load_history_room(room));
                    data["agents"] = room.empty()
                        ? nlohmann::json::array()
                        : build_lanchat_agents(sys->lanchat_load_history_agents(room));
                    if (room.empty()) data["error"] = "ROOM_REQUIRED";
                    callback->Success(create_success_json(func, data));
                    return true;
                }

                if (func == "join_room") {
                    const std::string ip = payload_arg.value("ip", "");
                    const uint16_t port = payload_arg.value("port", 27960);
                    const std::string room = payload_arg.value("room", "");
                    const std::string nickname = payload_arg.value("nickname", "Guest");
                    bool ok = sys->lanchat_join_room(ip, port, room, nickname);
                    nlohmann::json data;
                    data["ok"] = ok;
                    data["you"] = nickname;
                    data["peer_id"] = sys->local_peer_id();
                    data["port"] = sys->host_port() != 0 ? sys->host_port() : port;
                    data["members"] = build_lanchat_members(sys->lanchat_members());
                    data["member_details"] = build_lanchat_member_details(sys->lanchat_members());
                    data["history"] = build_lanchat_history(sys->lanchat_history());
                    data["agents"] = build_lanchat_agents(sys->lanchat_agents());
                    if (!ok) data["error"] = "JOIN_FAILED";
                    callback->Success(create_success_json(func, data));
                    return true;
                }

                if (func == "leave_room") {
                    sys->lanchat_leave_room();
                    nlohmann::json data;
                    data["ok"] = true;
                    callback->Success(create_success_json(func, data));
                    return true;
                }

                if (func == "send_message") {
                    const std::string text = payload_arg.value("text", "");
                    const std::string message_kind = payload_arg.value("message_kind", "chat");
                    const std::string target_agent_id = payload_arg.value("target_agent_id", "");
                    const std::string source_user_id = payload_arg.value("source_user_id", "");
                    const std::string correlation_id = payload_arg.value("correlation_id", "");
                    std::string metadata_json;
                    if (payload_arg.contains("metadata_json")) {
                        metadata_json = payload_arg.value("metadata_json", "");
                    } else if (payload_arg.contains("metadata")) {
                        metadata_json = payload_arg["metadata"].dump();
                    }
                    auto result = sys->lanchat_send_message_ex(
                        text, message_kind, target_agent_id, source_user_id,
                        correlation_id, metadata_json);
                    nlohmann::json data;
                    data["ok"] = result.accepted;
                    if (result.accepted) {
                        data["message_id"] = result.message.message_id;
                        data["seq"] = result.message.seq;
                    } else {
                        data["error"] = result.error.empty() ? "SEND_FAILED" : result.error;
                    }
                    callback->Success(create_success_json(func, data));
                    return true;
                }

                if (func == "add_agent") {
                    const std::string name = payload_arg.value("name", "Agent");
                    const std::string persona = payload_arg.value("persona", "");
                    const std::string peer_id = sys->local_peer_id().empty()
                        ? "local-single-player"
                        : sys->local_peer_id();
                    const std::string agent_id = make_agent_id(peer_id, name);
                    auto result = sys->lanchat_register_agent(agent_id, name, persona);
                    nlohmann::json data;
                    data["ok"] = result.ok;
                    data["agent_id"] = agent_id;
                    data["name"] = name;
                    if (!result.ok) data["error"] = result.error;
                    callback->Success(create_success_json(func, data));
                    return true;
                }

                if (func == "remove_agent") {
                    const std::string agent_id = payload_arg.value("agent_id", "");
                    auto result = sys->lanchat_remove_agent(agent_id);
                    nlohmann::json data;
                    data["ok"] = result.ok;
                    if (!result.ok) data["error"] = result.error;
                    callback->Success(create_success_json(func, data));
                    return true;
                }

                if (func == "list_agents") {
                    nlohmann::json data;
                    data["ok"] = true;
                    data["agents"] = build_lanchat_agents(sys->lanchat_agents());
                    callback->Success(create_success_json(func, data));
                    return true;
                }

                if (func == "get_local_ip") {
                    nlohmann::json data;
                    data["ok"] = true;
                    data["ip"] = detect_wlan_ipv4();
                    data["port"] = sys->session_port() != 0 ? sys->session_port() : 27960;
                    callback->Success(create_success_json(func, data));
                    return true;
                }

                callback->Failure(1, "Unknown LANChat function: " + func);
                return true;
            }
        } catch (const nlohmann::json::parse_error&) {
            // 非合法 JSON，继续走后续路径
        }
    }

    if (!Py_IsInitialized()) {
        Py_Initialize();
        PyEval_SaveThread();
    }

    PyGILState_STATE gstate = PyGILState_Ensure();

    try {
        if (!pFunc_) {
            initialize_python();
        }

        PyObject* args = PyTuple_Pack(1, PyUnicode_FromString(req.c_str()));
        PyObject* object = PyObject_CallObject(pFunc_, args);
        Py_DECREF(args);

        if (!object) {
            PyErr_Print();
            VUE_LOG_ERROR("Python function call failed for request");
            callback->Failure(0, "Python function call failed");
        } else {
            if (PyUnicode_Check(object)) {
                const char* result = PyUnicode_AsUTF8(object);
                callback->Success(result);
            } else {
                if (PyObject* str_obj = PyObject_Str(object)) {
                    const char* result = PyUnicode_AsUTF8(str_obj);
                    callback->Success(result);
                    Py_DECREF(str_obj);
                }
            }
            Py_DECREF(object);
        }

    } catch (const std::exception& e) {
        std::cerr << "Exception in OnQuery: " << e.what() << std::endl;
        callback->Failure(0, e.what());
        PyGILState_Release(gstate);
        return false;
    }

    PyGILState_Release(gstate);
    return true;
}

}  // namespace Corona::Systems::UI
