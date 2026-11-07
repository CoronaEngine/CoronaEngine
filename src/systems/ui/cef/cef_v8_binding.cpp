#include "cef_v8_binding.h"

#include "browser_manager.h"
#include "cef_window_operations.h"
#include "cef_scene_operations.h"
#include "cef_viewport_pick.h"
#include "cef_viewport_gizmo.h"
#include "cef_editor_native_api_handlers.h"
#include "../editor_api/cef_editor_api.h"
#include "cef_script_input.h"
#include "horizon/core/logging.h"

#include <include/base/cef_callback.h>
#include <wrapper/cef_closure_task.h>
#include <wrapper/cef_helpers.h>

#include <atomic>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <initializer_list>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <variant>
#include <type_traits>
#include <vector>
#include <nlohmann/json.hpp>

namespace Corona::Systems::UI {
ViewportGizmoReply execute_viewport_gizmo_target(const ViewportGizmoTargetRequest& request);
ViewportGizmoReply execute_viewport_gizmo_pointer(const ViewportGizmoPointerRequest& request);
namespace {

enum class EditorApiMethod { Invoke, On, Off };

bool v8_to_json(CefRefPtr<CefV8Value> value, nlohmann::json& output) {
    if (!value || value->IsUndefined() || value->IsNull()) { output = nullptr; return true; }
    if (value->IsBool()) { output = value->GetBoolValue(); return true; }
    if (value->IsInt()) { output = value->GetIntValue(); return true; }
    if (value->IsUInt()) { output = value->GetUIntValue(); return true; }
    if (value->IsDouble()) { output = value->GetDoubleValue(); return std::isfinite(value->GetDoubleValue()); }
    if (value->IsString()) { output = value->GetStringValue().ToString(); return true; }
    if (value->IsArray()) {
        output = nlohmann::json::array();
        for (int index = 0; index < value->GetArrayLength(); ++index) {
            nlohmann::json item;
            if (!v8_to_json(value->GetValue(index), item)) return false;
            output.push_back(std::move(item));
        }
        return true;
    }
    if (!value->IsObject() || value->IsFunction()) return false;
    std::vector<CefString> keys;
    if (!value->GetKeys(keys)) return false;
    output = nlohmann::json::object();
    for (const auto& key : keys) {
        nlohmann::json item;
        if (!v8_to_json(value->GetValue(key), item)) return false;
        output[key.ToString()] = std::move(item);
    }
    return true;
}

CefRefPtr<CefV8Value> json_to_v8(const nlohmann::json& value) {
    if (value.is_null()) return CefV8Value::CreateNull();
    if (value.is_boolean()) return CefV8Value::CreateBool(value.get<bool>());
    if (value.is_number_integer()) return CefV8Value::CreateInt(value.get<int>());
    if (value.is_number_unsigned()) return CefV8Value::CreateUInt(value.get<unsigned int>());
    if (value.is_number_float()) return CefV8Value::CreateDouble(value.get<double>());
    if (value.is_string()) return CefV8Value::CreateString(value.get<std::string>());
    if (value.is_array()) {
        auto result = CefV8Value::CreateArray(static_cast<int>(value.size()));
        for (int index = 0; index < static_cast<int>(value.size()); ++index)
            result->SetValue(index, json_to_v8(value[index]));
        return result;
    }
    auto result = CefV8Value::CreateObject(nullptr, nullptr);
    for (auto it = value.begin(); it != value.end(); ++it)
        result->SetValue(it.key(), json_to_v8(it.value()), V8_PROPERTY_ATTRIBUTE_NONE);
    return result;
}

bool read_editor_api_call(const CefV8ValueList& arguments, std::string& api_name,
                          nlohmann::json& args) {
    if (arguments.size() != 2 || !arguments[0]->IsString() || !arguments[1]->IsArray()) return false;
    api_name = arguments[0]->GetStringValue().ToString();
    if (api_name.empty()) return false;
    return v8_to_json(arguments[1], args) && args.is_array();
}

enum class GizmoMethod { SetTarget, Pointer };

bool object_keys(CefRefPtr<CefV8Value> object, std::initializer_list<const char*> allowed);

bool read_gizmo_request(GizmoMethod method, const CefV8ValueList& arguments,
                        ViewportGizmoTargetRequest& target, ViewportGizmoPointerRequest& pointer) {
    if (arguments.size() != 3 || !arguments[0]->IsString() || !arguments[1]->IsString() ||
        arguments[0]->GetStringValue().empty() || arguments[1]->GetStringValue().empty() ||
        !object_keys(arguments[2], method == GizmoMethod::SetTarget
            ? std::initializer_list<const char*>{"archive_session", "actor_guid"}
            : std::initializer_list<const char*>{"archive_session", "request_id", "event", "x", "y", "width", "height", "button", "buttons", "modifiers"})) return false;
    auto options = arguments[2];
    auto session = options->GetValue("archive_session");
    if (!session || !session->IsString() || session->GetStringValue().empty()) return false;
    target.scene_id = arguments[0]->GetStringValue().ToString();
    target.camera_id = arguments[1]->GetStringValue().ToString();
    target.archive_session = session->GetStringValue().ToString();
    if (method == GizmoMethod::SetTarget) {
        auto actor = options->GetValue("actor_guid");
        if (actor && !actor->IsNull()) {
            if (!actor->IsString() || actor->GetStringValue().empty()) return false;
            target.actor_guid = actor->GetStringValue().ToString();
        }
        return true;
    }
    pointer.scene_id = target.scene_id; pointer.camera_id = target.camera_id;
    pointer.archive_session = target.archive_session;
    const auto text = [&](const char* name, std::string& value) { auto v = options->GetValue(name); if (!v || !v->IsString()) return false; value = v->GetStringValue().ToString(); return !value.empty(); };
    const auto number = [&](const char* name, double& value) { auto v = options->GetValue(name); if (!v || !(v->IsInt() || v->IsUInt() || v->IsDouble())) return false; value = v->GetDoubleValue(); return std::isfinite(value); };
    double button = 0, buttons = 0, modifiers = 0;
    if (!text("request_id", pointer.request_id) || !text("event", pointer.event) || !number("x", pointer.x) || !number("y", pointer.y) || !number("width", pointer.width) || !number("height", pointer.height) || !number("button", button) || !number("buttons", buttons) || !number("modifiers", modifiers) || pointer.width <= 0 || pointer.height <= 0) return false;
    pointer.button = static_cast<int>(button); pointer.buttons = static_cast<int>(buttons); pointer.modifiers = static_cast<int>(modifiers);
    return true;
}

enum class WindowEvent { Closed, CloseRequested };
struct WindowEventData {
    WindowEvent type;
    int tab_id = -1;
    std::string panel_id;
};

bool read_script_input(const CefV8ValueList& arguments, ScriptInputRequest& request) {
    if (arguments.size() != 3 || !arguments[0]->IsString() || arguments[0]->GetStringValue().empty() ||
        !arguments[1]->IsString() || arguments[1]->GetStringValue().empty()) return false;
    const auto value = arguments[2];
    if (!value || !value->IsObject() || value->IsArray() || value->IsFunction()) return false;
    const auto text = [&](const char* key, std::string& out) {
        const auto v = value->GetValue(key);
        if (!v || !v->IsString()) return false;
        out = v->GetStringValue().ToString();
        return out.find('\0') == std::string::npos;
    };
    std::string type;
    if (!text("archive_session", request.source.archive_session) || request.source.archive_session.empty() || !text("type", type)) return false;
    request.source.scene_id = arguments[0]->GetStringValue().ToString();
    request.source.camera_id = arguments[1]->GetStringValue().ToString();
    std::vector<CefString> keys;
    if (!value->GetKeys(keys)) return false;
    auto& event = request.event;
    const bool mouse = type == "mouse", key_down = type == "keyDown";
    if (!mouse && !key_down && type != "keyUp") return false;
    if (keys.size() != (mouse ? 11u : key_down ? 5u : 4u)) return false;
    for (const auto& k : keys) {
        if (k == "archive_session" || k == "type") continue;
        if (mouse) {
            if (k != "event" && k != "button" && k != "x" && k != "y" && k != "viewport_x" && k != "viewport_y" &&
                k != "viewport_width" && k != "viewport_height" && k != "picked_actor") return false;
        } else if (k != "code" && k != "key" && !(key_down && k == "modifiers")) return false;
    }
    if (!mouse) {
        event.type = key_down ? 0 : 1;
        std::string key;
        if (!text("code", event.arg0) || event.arg0.empty() || !text("key", key) || key.empty()) return false;
        if (key_down) { event.arg2 = key; return text("modifiers", event.arg1); }
        event.arg1 = key; return true;
    }
    event.type = 2;
    if (!text("event", event.arg0) || !text("button", event.arg1) || !text("picked_actor", event.picked_actor)) return false;
    if (event.arg0 != "move" && event.arg0 != "mousedown" && event.arg0 != "mouseup" && event.arg0 != "click" && event.arg0 != "wheel") return false;
    if (!event.arg1.empty() && event.arg1 != "LeftButton" && event.arg1 != "MiddleButton" && event.arg1 != "RightButton") return false;
    const auto number = [&](const char* key, double& out) {
        const auto v = value->GetValue(key);
        if (!v || !(v->IsInt() || v->IsUInt() || v->IsDouble())) return false;
        out = v->GetDoubleValue(); return std::isfinite(out);
    };
    return number("x", event.arg3) && number("y", event.arg4) && number("viewport_x", event.viewport_x) &&
        number("viewport_y", event.viewport_y) && number("viewport_width", event.viewport_width) &&
        number("viewport_height", event.viewport_height) && event.viewport_width > 0 && event.viewport_height > 0;
}

bool read_viewport_pick(const CefV8ValueList& arguments, ViewportPickRequest& request) {
    if (arguments.size() != 3 || !arguments[0]->IsString() || arguments[0]->GetStringValue().empty() ||
        !arguments[1]->IsString() || arguments[1]->GetStringValue().empty()) return false;
    const auto options = arguments[2];
    if (!options || !options->IsObject() || options->IsArray() || options->IsFunction()) return false;
    std::vector<CefString> keys;
    if (!options->GetKeys(keys) || keys.size() != 5) return false;
    for (const auto& key : keys) if (key != "archive_session" && key != "x" && key != "y" && key != "width" && key != "height") return false;
    const auto session = options->GetValue("archive_session");
    if (!session || !session->IsString() || session->GetStringValue().empty()) return false;
    request.camera.scene_id = arguments[0]->GetStringValue().ToString();
    request.camera.camera_id = arguments[1]->GetStringValue().ToString();
    request.camera.archive_session = session->GetStringValue().ToString();
    const auto number = [&](const char* name, double& out) {
        const auto value = options->GetValue(name);
        if (!value || !(value->IsInt() || value->IsUInt() || value->IsDouble())) return false;
        out = value->GetDoubleValue(); return std::isfinite(out);
    };
    return number("x", request.x) && number("y", request.y) && number("width", request.width) &&
        number("height", request.height) && request.width > 0 && request.height > 0;
}

CefRefPtr<CefV8Value> viewport_pick_value(const ViewportPickReply& reply) {
    auto value = CefV8Value::CreateObject(nullptr, nullptr);
    const auto text = [&](const char* name, const std::string& text) {
        value->SetValue(name, CefV8Value::CreateString(text), V8_PROPERTY_ATTRIBUTE_NONE);
    };
    text("status", reply.status); text("archive_session", reply.archive_session);
    text("scene", reply.scene); text("camera_id", reply.camera_id);
    text("actor_guid", reply.actor_guid); text("actor_name", reply.actor_name);
    text("actor_type", reply.actor_type); text("actor_handle", reply.actor_handle);
    value->SetValue("node_index", reply.node_index ? CefV8Value::CreateUInt(*reply.node_index) : CefV8Value::CreateNull(), V8_PROPERTY_ATTRIBUTE_NONE);
    value->SetValue("x", CefV8Value::CreateUInt(reply.x), V8_PROPERTY_ATTRIBUTE_NONE);
    value->SetValue("y", CefV8Value::CreateUInt(reply.y), V8_PROPERTY_ATTRIBUTE_NONE);
    return value;
}

bool read_float3(CefRefPtr<CefV8Value> value, std::array<float, 3>& copy) {
    if (!value || !value->IsArray() || value->GetArrayLength() != 3) return false;
    for (int index = 0; index < 3; ++index) {
        const auto item = value->GetValue(index);
        if (!item || !(item->IsDouble() || item->IsInt() || item->IsUInt())) return false;
        const auto numeric = item->GetDoubleValue();
        if (!std::isfinite(numeric) || std::abs(numeric) > std::numeric_limits<float>::max()) return false;
        copy[index] = static_cast<float>(numeric);
    }
    return true;
}

bool object_keys(CefRefPtr<CefV8Value> object, std::initializer_list<const char*> allowed) {
    if (!object || !object->IsObject() || object->IsArray() || object->IsFunction()) return false;
    std::vector<CefString> keys;
    if (!object->GetKeys(keys)) return false;
    for (const auto& key : keys) {
        if (std::none_of(allowed.begin(), allowed.end(), [&](const auto* name) { return key == name; })) return false;
    }
    return true;
}

bool read_screenshot(const CefV8ValueList& arguments, SceneRequest& request) {
    if (arguments.size() != 3) return false;
    for (int index = 0; index < 2; ++index)
        if (!arguments[index]->IsString() || arguments[index]->GetStringValue().empty()) return false;
    const auto options = arguments[2];
    if (!object_keys(options, {"archive_session", "path"})) return false;
    const auto session = options->GetValue("archive_session");
    if (!session || !session->IsString() || session->GetStringValue().empty()) return false;
    request.archive_session = session->GetStringValue().ToString();
    request.screenshot.scene = arguments[0]->GetStringValue().ToString();
    request.screenshot.camera_id = arguments[1]->GetStringValue().ToString();
    if (options->HasValue("path")) {
        const auto path = options->GetValue("path");
        if (!path || !path->IsString() || path->GetStringValue().empty()) return false;
        request.screenshot.path = path->GetStringValue().ToString();
        if (request.screenshot.path->find('\0') != std::string::npos) return false;
    }
    return true;
}

CefRefPtr<CefV8Value> screenshot_value(const ScreenshotSnapshot& snapshot) {
    auto result = CefV8Value::CreateObject(nullptr, nullptr);
    const auto text = [&](const char* name, const std::string& value) {
        result->SetValue(name, CefV8Value::CreateString(value), V8_PROPERTY_ATTRIBUTE_NONE);
    };
    text("status", snapshot.status);
    text("archive_session", snapshot.archive_session);
    text("scene", snapshot.scene);
    text("camera_id", snapshot.camera_id);
    text("path", snapshot.path);
    return result;
}

bool read_viewport_configuration(const CefV8ValueList& arguments, WindowRequest& request) {
    if (arguments.size() != 3) return false;
    for (int index = 0; index < 2; ++index)
        if (!arguments[index]->IsString() || arguments[index]->GetStringValue().empty()) return false;
    const auto object = arguments[2];
    if (!object_keys(object, {"archive_session", "mode", "calibration", "bounds", "pointer"})) return false;
    const auto session = object->GetValue("archive_session");
    if (!session || !session->IsString() || session->GetStringValue().empty()) return false;
    request.scene_id = arguments[0]->GetStringValue().ToString();
    request.camera_id = arguments[1]->GetStringValue().ToString(); request.archive_session = session->GetStringValue().ToString();
    auto& patch = request.viewport.emplace();
    if (object->HasValue("mode")) {
        const auto mode = object->GetValue("mode");
        if (!mode->IsString()) return false;
        patch.mode = mode->GetStringValue().ToString();
        if (*patch.mode != "flat2d" && *patch.mode != "stereo3d") return false;
    }
    const auto number = [](CefRefPtr<CefV8Value> object, const char* key, auto& output) {
        const auto value = object->GetValue(key);
        if (!value || !(value->IsInt() || value->IsUInt() || value->IsDouble())) return false;
        const auto numeric = value->GetDoubleValue();
        using Value = std::decay_t<decltype(output)>;
        if (!std::isfinite(numeric) || numeric < std::numeric_limits<Value>::lowest() || numeric > std::numeric_limits<Value>::max()) return false;
        if constexpr (std::is_integral_v<Value>) { if (std::trunc(numeric) != numeric) return false; }
        output = static_cast<Value>(numeric); return true;
    };
    if (object->HasValue("calibration")) {
        const auto value = object->GetValue("calibration");
        if (!object_keys(value, {"pe", "angle", "offset", "parallaxScale"})) return false;
        auto& c = patch.calibration.emplace();
        if (!number(value, "pe", c.pe) || !number(value, "angle", c.angle) || !number(value, "offset", c.offset) ||
            !number(value, "parallaxScale", c.parallax_scale)) return false;
    }
    if (object->HasValue("bounds")) {
        const auto value = object->GetValue("bounds");
        if (!object_keys(value, {"x", "y", "width", "height", "renderWidth", "renderHeight"})) return false;
        auto& bounds = patch.bounds.emplace();
        if (!number(value, "x", bounds.x) || !number(value, "y", bounds.y) || !number(value, "width", bounds.width) ||
            !number(value, "height", bounds.height) || !number(value, "renderWidth", bounds.render_width) ||
            !number(value, "renderHeight", bounds.render_height)) return false;
    }
    if (object->HasValue("pointer")) {
        const auto value = object->GetValue("pointer");
        if (!object_keys(value, {"x", "y", "cursor"})) return false;
        const auto cursor = value->GetValue("cursor");
        if (!cursor || !cursor->IsString()) return false;
        auto& pointer = patch.pointer.emplace();
        pointer.cursor = cursor->GetStringValue().ToString();
        if (pointer.cursor != "hidden" || value->HasValue("x") || value->HasValue("y")) {
            if (!number(value, "x", pointer.x) || !number(value, "y", pointer.y)) return false;
        }
    }
    return patch.mode || patch.calibration || patch.bounds || patch.pointer;
}

CefRefPtr<CefV8Value> viewport_configuration_value(const ViewportConfigurationSnapshot& snapshot) {
    auto result = CefV8Value::CreateObject(nullptr, nullptr);
    const auto text = [&](const char* key, const std::string& value) {
        result->SetValue(key, CefV8Value::CreateString(value), V8_PROPERTY_ATTRIBUTE_NONE);
    };
    const auto number = [](CefRefPtr<CefV8Value> object, const char* key, double value) {
        object->SetValue(key, CefV8Value::CreateDouble(value), V8_PROPERTY_ATTRIBUTE_NONE);
    };
    text("archive_session", snapshot.archive_session); text("scene", snapshot.scene); text("camera_id", snapshot.camera_id); text("mode", snapshot.mode);
    auto calibration = CefV8Value::CreateObject(nullptr, nullptr);
    number(calibration, "pe", snapshot.calibration.pe); number(calibration, "angle", snapshot.calibration.angle);
    number(calibration, "offset", snapshot.calibration.offset); number(calibration, "parallaxScale", snapshot.calibration.parallax_scale);
    result->SetValue("calibration", calibration, V8_PROPERTY_ATTRIBUTE_NONE);
    auto bounds = CefV8Value::CreateNull();
    if (snapshot.bounds) {
        bounds = CefV8Value::CreateObject(nullptr, nullptr);
        const auto& values = *snapshot.bounds;
        number(bounds, "x", values.x); number(bounds, "y", values.y); number(bounds, "width", values.width); number(bounds, "height", values.height);
        number(bounds, "renderWidth", values.render_width); number(bounds, "renderHeight", values.render_height);
    }
    result->SetValue("bounds", bounds, V8_PROPERTY_ATTRIBUTE_NONE);
    auto pointer = CefV8Value::CreateObject(nullptr, nullptr);
    number(pointer, "x", snapshot.pointer.x); number(pointer, "y", snapshot.pointer.y);
    pointer->SetValue("cursor", CefV8Value::CreateString(snapshot.pointer.cursor), V8_PROPERTY_ATTRIBUTE_NONE);
    result->SetValue("pointer", pointer, V8_PROPERTY_ATTRIBUTE_NONE);
    result->SetValue("systemCursorHidden", CefV8Value::CreateBool(snapshot.system_cursor_hidden), V8_PROPERTY_ATTRIBUTE_NONE);
    return result;
}

bool read_camera_properties(const CefV8ValueList& arguments, SceneRequest& request) {
    if (arguments.size() != 3) return false;
    for (int index = 0; index < 2; ++index)
        if (!arguments[index]->IsString() || arguments[index]->GetStringValue().empty()) return false;
    const auto object = arguments[2];
    if (!object_keys(object, {"archive_session", "position", "forward", "world_up", "fov", "projection_x_sign",
        "near_plane", "far_plane", "width", "height", "view_open", "view_x", "view_y", "view_width", "view_height", "move_speed"})) return false;
    const auto session = object->GetValue("archive_session");
    if (!session || !session->IsString() || session->GetStringValue().empty()) return false;
    request.archive_session = session->GetStringValue().ToString();
    auto& patch = request.camera_properties;
    patch.scene = arguments[0]->GetStringValue().ToString(); patch.camera_id = arguments[1]->GetStringValue().ToString();
    const auto vector = [&](const char* key, auto& output) {
        if (!object->HasValue(key)) return true;
        std::array<float, 3> value;
        if (!read_float3(object->GetValue(key), value)) return false;
        output = value; return true;
    };
    const auto number = [&](const char* key, auto& output) {
        if (!object->HasValue(key)) return true;
        const auto input = object->GetValue(key);
        if (!input || !(input->IsInt() || input->IsUInt() || input->IsDouble())) return false;
        const auto value = input->GetDoubleValue();
        using Value = typename std::decay_t<decltype(output)>::value_type;
        if (!std::isfinite(value) || value < std::numeric_limits<Value>::lowest() || value > std::numeric_limits<Value>::max()) return false;
        if constexpr (std::is_integral_v<Value>) { if (std::trunc(value) != value) return false; }
        output = static_cast<Value>(value); return true;
    };
    if (object->HasValue("view_open")) {
        if (!object->GetValue("view_open")->IsBool()) return false;
        patch.view_open = object->GetValue("view_open")->GetBoolValue();
    }
    return vector("position", patch.position) && vector("forward", patch.forward) && vector("world_up", patch.world_up) &&
        number("fov", patch.fov) && number("projection_x_sign", patch.projection_x_sign) &&
        number("near_plane", patch.near_plane) && number("far_plane", patch.far_plane) &&
        number("move_speed", patch.move_speed) && number("width", patch.width) && number("height", patch.height) &&
        number("view_x", patch.view_x) && number("view_y", patch.view_y) &&
        number("view_width", patch.view_width) && number("view_height", patch.view_height) && !patch.empty();
}

CefRefPtr<CefV8Value> camera_properties_value(const CameraPropertiesSnapshot& snapshot) {
    auto result = CefV8Value::CreateObject(nullptr, nullptr);
    auto camera = CefV8Value::CreateObject(nullptr, nullptr);
    const auto text = [](CefRefPtr<CefV8Value> target, const char* key, const std::string& value) {
        target->SetValue(key, CefV8Value::CreateString(value), V8_PROPERTY_ATTRIBUTE_NONE);
    };
    text(result, "status", "success"); text(result, "archive_session", snapshot.archive_session); text(result, "scene", snapshot.scene);
    text(camera, "archive_session", snapshot.archive_session); text(camera, "camera_id", snapshot.camera_id);
    text(camera, "name", snapshot.name); text(camera, "opaque_handle", std::to_string(snapshot.handle));
    const auto vector = [&](const char* key, const auto& values) {
        auto array = CefV8Value::CreateArray(3);
        for (int index = 0; index < 3; ++index) array->SetValue(index, CefV8Value::CreateDouble(values[index]));
        camera->SetValue(key, array, V8_PROPERTY_ATTRIBUTE_NONE);
    };
    const auto number = [&](const char* key, double value) {
        camera->SetValue(key, CefV8Value::CreateDouble(value), V8_PROPERTY_ATTRIBUTE_NONE);
    };
    vector("position", snapshot.position); vector("forward", snapshot.forward); vector("world_up", snapshot.world_up);
    number("fov", snapshot.fov); number("projection_x_sign", snapshot.projection_x_sign);
    number("near_plane", snapshot.near_plane); number("far_plane", snapshot.far_plane);
    number("width", snapshot.width); number("height", snapshot.height); number("move_speed", snapshot.move_speed);
    number("view_x", snapshot.view_x); number("view_y", snapshot.view_y);
    number("view_width", snapshot.view_width); number("view_height", snapshot.view_height);
    camera->SetValue("view_open", CefV8Value::CreateBool(snapshot.view_open), V8_PROPERTY_ATTRIBUTE_NONE);
    result->SetValue("camera", camera, V8_PROPERTY_ATTRIBUTE_NONE);
    return result;
}

bool read_focus(const CefV8ValueList& arguments, SceneRequest& request) {
    if (arguments.size() != 4) return false;
    for (int index = 0; index < 3; ++index)
        if (!arguments[index]->IsString() || arguments[index]->GetStringValue().empty()) return false;
    const auto scope = arguments[3];
    if (!object_keys(scope, {"archive_session"})) return false;
    const auto session = scope->GetValue("archive_session");
    if (!session || !session->IsString() || session->GetStringValue().empty()) return false;
    request.archive_session = session->GetStringValue().ToString();
    request.focus = {arguments[0]->GetStringValue().ToString(), arguments[1]->GetStringValue().ToString(),
        arguments[2]->GetStringValue().ToString()};
    return true;
}

CefRefPtr<CefV8Value> focus_value(const ActorFocusSnapshot& snapshot) {
    const auto set = [](CefRefPtr<CefV8Value> object, const char* name, CefRefPtr<CefV8Value> value) {
        object->SetValue(name, value, V8_PROPERTY_ATTRIBUTE_NONE);
    };
    const auto text = [&](CefRefPtr<CefV8Value> object, const char* name, const std::string& value) {
        set(object, name, CefV8Value::CreateString(value));
    };
    const auto vector = [](const std::array<float, 3>& values) {
        auto result = CefV8Value::CreateArray(3);
        for (int index = 0; index < 3; ++index) result->SetValue(index, CefV8Value::CreateDouble(values[index]));
        return result;
    };
    auto result = CefV8Value::CreateObject(nullptr, nullptr);
    text(result, "status", "success");
    text(result, "archive_session", snapshot.archive_session);
    text(result, "scene", snapshot.scene);
    text(result, "actor", snapshot.actor);
    text(result, "actor_guid", snapshot.actor_guid);
    set(result, "center", vector(snapshot.center));
    set(result, "distance", CefV8Value::CreateDouble(snapshot.distance));
    auto camera = CefV8Value::CreateObject(nullptr, nullptr);
    text(camera, "camera_id", snapshot.camera.camera_id);
    text(camera, "name", snapshot.camera.name);
    text(camera, "opaque_handle", std::to_string(snapshot.camera.handle));
    set(camera, "position", vector(snapshot.camera.position));
    set(camera, "forward", vector(snapshot.camera.forward));
    set(camera, "world_up", vector(snapshot.camera.world_up));
    set(camera, "fov", CefV8Value::CreateDouble(snapshot.camera.fov));
    set(result, "camera", camera);
    return result;
}

bool read_properties(const CefV8ValueList& arguments, SceneRequest& request) {
    if (arguments.size() != 3 || !arguments[0]->IsString() || !arguments[1]->IsString()) return false;
    auto& patch = request.properties;
    patch.scene = arguments[0]->GetStringValue().ToString();
    patch.actor = arguments[1]->GetStringValue().ToString();
    if (patch.scene.empty() || patch.actor.empty()) return false;
    const auto object = arguments[2];
    if (!object_keys(object, {"archive_session", "visible", "follow_camera", "mechanics", "camera_lock", "animation_sources"})) return false;
    const auto session = object->GetValue("archive_session");
    if (!session || !session->IsString() || session->GetStringValue().empty()) return false;
    request.archive_session = session->GetStringValue().ToString();
    const auto boolean = [&](const char* key, std::optional<bool>& result) {
        if (!object->HasValue(key)) return true;
        const auto value = object->GetValue(key);
        if (!value || !value->IsBool()) return false;
        result = value->GetBoolValue(); return true;
    };
    if (!boolean("visible", patch.visible) || !boolean("follow_camera", patch.follow_camera)) return false;
    if (object->HasValue("mechanics")) {
        const auto values = object->GetValue("mechanics");
        if (!object_keys(values, {"mass", "restitution", "damping", "body_type", "linear_lock", "angular_lock"})) return false;
        ActorMechanicsPatch mechanics;
        const auto scalar = [&](const char* key, std::optional<float>& result) {
            if (!values->HasValue(key)) return true;
            const auto value = values->GetValue(key);
            if (!value || !(value->IsDouble() || value->IsInt() || value->IsUInt())) return false;
            const auto numeric = value->GetDoubleValue();
            if (!std::isfinite(numeric) || std::abs(numeric) > std::numeric_limits<float>::max()) return false;
            result = static_cast<float>(numeric); return true;
        };
        const auto locks = [&](const char* key, std::optional<std::array<bool, 3>>& result) {
            if (!values->HasValue(key)) return true;
            const auto value = values->GetValue(key);
            if (!value || !value->IsArray() || value->GetArrayLength() != 3) return false;
            std::array<bool, 3> copy;
            for (int index = 0; index < 3; ++index) {
                const auto item = value->GetValue(index);
                if (!item || !item->IsBool()) return false;
                copy[index] = item->GetBoolValue();
            }
            result = copy; return true;
        };
        if (!scalar("mass", mechanics.mass) || !scalar("restitution", mechanics.restitution) ||
            !scalar("damping", mechanics.damping) || !locks("linear_lock", mechanics.linear_lock) ||
            !locks("angular_lock", mechanics.angular_lock)) return false;
        if (values->HasValue("body_type")) {
            const auto value = values->GetValue("body_type");
            if (!value || !value->IsString()) return false;
            mechanics.body_type = value->GetStringValue().ToString();
            if (*mechanics.body_type != "dynamic" && *mechanics.body_type != "kinematic" &&
                *mechanics.body_type != "static" && *mechanics.body_type != "phantom") return false;
        }
        if (mechanics.empty()) return false;
        patch.mechanics = std::move(mechanics);
    }
    if (object->HasValue("camera_lock")) {
        const auto values = object->GetValue("camera_lock");
        if (!object_keys(values, {"enabled", "position_offset", "rotation_offset"})) return false;
        const auto enabled = values->GetValue("enabled");
        if (!enabled || !enabled->IsBool()) return false;
        ActorCameraLockPatch lock; lock.enabled = enabled->GetBoolValue();
        const auto vector = [&](const char* key, std::optional<std::array<float, 3>>& result) {
            if (!values->HasValue(key)) return true;
            std::array<float, 3> copy;
            if (!read_float3(values->GetValue(key), copy)) return false;
            result = copy; return true;
        };
        if (!vector("position_offset", lock.position_offset) || !vector("rotation_offset", lock.rotation_offset)) return false;
        patch.camera_lock = lock;
    }
    if (object->HasValue("animation_sources")) {
        const auto values = object->GetValue("animation_sources");
        if (!values || !values->IsArray()) return false;
        std::vector<std::string> sources;
        for (int index = 0; index < values->GetArrayLength(); ++index) {
            const auto value = values->GetValue(index);
            if (!value || !value->IsString()) return false;
            sources.push_back(value->GetStringValue().ToString());
        }
        patch.animation_sources = std::move(sources);
    }
    return patch.visible || patch.follow_camera || patch.mechanics || patch.camera_lock || patch.animation_sources;
}

CefRefPtr<CefV8Value> properties_value(const ActorPropertiesSnapshot& snapshot) {
    const auto set = [](auto object, const char* key, auto value) { object->SetValue(key, value, V8_PROPERTY_ATTRIBUTE_NONE); };
    const auto vector = [](const auto& values) {
        auto array = CefV8Value::CreateArray(3);
        for (int index = 0; index < 3; ++index) {
            if constexpr (std::is_same_v<typename std::decay_t<decltype(values)>::value_type, bool>)
                array->SetValue(index, CefV8Value::CreateBool(values[index]));
            else array->SetValue(index, CefV8Value::CreateDouble(values[index]));
        }
        return array;
    };
    auto result = CefV8Value::CreateObject(nullptr, nullptr);
    set(result, "status", CefV8Value::CreateString("success"));
    set(result, "scene", CefV8Value::CreateString(snapshot.scene));
    auto actor = CefV8Value::CreateObject(nullptr, nullptr);
    set(actor, "name", CefV8Value::CreateString(snapshot.actor));
    set(actor, "actor_guid", CefV8Value::CreateString(snapshot.actor_guid));
    set(actor, "actor_version", CefV8Value::CreateInt(snapshot.actor_version));
    set(actor, "follow_camera", CefV8Value::CreateBool(snapshot.follow_camera));
    if (snapshot.visible) set(actor, "visible", CefV8Value::CreateBool(*snapshot.visible));
    if (snapshot.mechanics) {
        const auto& values = *snapshot.mechanics;
        auto mechanics = CefV8Value::CreateObject(nullptr, nullptr);
        set(mechanics, "mass", CefV8Value::CreateDouble(values.mass));
        set(mechanics, "restitution", CefV8Value::CreateDouble(values.restitution));
        set(mechanics, "damping", CefV8Value::CreateDouble(values.damping));
        set(mechanics, "body_type", CefV8Value::CreateString(values.body_type));
        set(mechanics, "linear_lock", vector(values.linear_lock));
        set(mechanics, "angular_lock", vector(values.angular_lock));
        set(actor, "mechanics", mechanics);
    }
    auto lock = CefV8Value::CreateObject(nullptr, nullptr);
    set(lock, "enabled", CefV8Value::CreateBool(snapshot.camera_lock.enabled));
    set(lock, "position_offset", vector(snapshot.camera_lock.position_offset));
    set(lock, "rotation_offset", vector(snapshot.camera_lock.rotation_offset));
    set(actor, "camera_lock", lock);
    if (snapshot.requested_animation_sources) {
        auto sources = CefV8Value::CreateArray(static_cast<int>(snapshot.requested_animation_sources->size()));
        for (size_t index = 0; index < snapshot.requested_animation_sources->size(); ++index)
            sources->SetValue(static_cast<int>(index), CefV8Value::CreateString((*snapshot.requested_animation_sources)[index]));
        set(actor, "requested_animation_sources", sources);
    }
    set(result, "actor", actor);
    return result;
}

bool read_environment(const CefV8ValueList& arguments, SceneRequest& request) {
    if (arguments.size() != 2 || !arguments[0]->IsString() || arguments[0]->GetStringValue().empty()) return false;
    request.environment.scene = arguments[0]->GetStringValue().ToString();
    const auto object = arguments[1];
    const bool write = request.method == SceneMethod::SetEnvironment;
    if (!(write ? object_keys(object, {"archive_session", "sun", "grid", "physics"}) :
                  object_keys(object, {"archive_session"}))) return false;
    const auto session = object->GetValue("archive_session");
    if (!session || !session->IsString() || session->GetStringValue().empty()) return false;
    request.archive_session = session->GetStringValue().ToString();
    if (!write) return true;
    auto& patch = request.environment;
    if (object->HasValue("sun")) {
        const auto value = object->GetValue("sun");
        if (!object_keys(value, {"enabled", "direction"})) return false;
        EnvironmentSunPatch sun;
        if (value->HasValue("enabled")) {
            if (!value->GetValue("enabled")->IsBool()) return false;
            sun.enabled = value->GetValue("enabled")->GetBoolValue();
        }
        if (value->HasValue("direction")) {
            std::array<float, 3> direction;
            if (!read_float3(value->GetValue("direction"), direction)) return false;
            sun.direction = direction;
        }
        if (sun.empty()) return false;
        patch.sun = sun;
    }
    if (object->HasValue("grid")) {
        const auto value = object->GetValue("grid");
        if (!object_keys(value, {"enabled"}) || !value->HasValue("enabled") || !value->GetValue("enabled")->IsBool()) return false;
        patch.grid_enabled = value->GetValue("enabled")->GetBoolValue();
    }
    if (object->HasValue("physics")) {
        const auto value = object->GetValue("physics");
        if (!object_keys(value, {"gravity", "floor_y", "floor_restitution", "fixed_dt"})) return false;
        EnvironmentPhysicsPatch physics;
        if (value->HasValue("gravity")) {
            std::array<float, 3> gravity;
            if (!read_float3(value->GetValue("gravity"), gravity)) return false;
            physics.gravity = gravity;
        }
        const auto scalar = [&](const char* key, std::optional<float>& output) {
            if (!value->HasValue(key)) return true;
            const auto input = value->GetValue(key);
            if (!input || !(input->IsInt() || input->IsUInt() || input->IsDouble())) return false;
            const auto number = input->GetDoubleValue();
            if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max()) return false;
            output = static_cast<float>(number); return true;
        };
        if (!scalar("floor_y", physics.floor_y) || !scalar("floor_restitution", physics.floor_restitution) ||
            !scalar("fixed_dt", physics.fixed_dt) || physics.empty()) return false;
        patch.physics = physics;
    }
    return patch.sun || patch.grid_enabled || patch.physics;
}

CefRefPtr<CefV8Value> environment_value(const EnvironmentSnapshot& snapshot) {
    const auto set = [](CefRefPtr<CefV8Value> target, const char* key, CefRefPtr<CefV8Value> value) {
        target->SetValue(key, value, V8_PROPERTY_ATTRIBUTE_NONE);
    };
    const auto vector = [](const std::array<float, 3>& values) {
        auto result = CefV8Value::CreateArray(3);
        for (int index = 0; index < 3; ++index) result->SetValue(index, CefV8Value::CreateDouble(values[index]));
        return result;
    };
    auto result = CefV8Value::CreateObject(nullptr, nullptr);
    set(result, "archive_session", CefV8Value::CreateString(snapshot.archive_session));
    set(result, "scene", CefV8Value::CreateString(snapshot.scene));
    auto sun = CefV8Value::CreateObject(nullptr, nullptr);
    set(sun, "enabled", CefV8Value::CreateBool(snapshot.sun_enabled));
    set(sun, "direction", vector(snapshot.sun_direction));
    set(result, "sun", sun);
    auto grid = CefV8Value::CreateObject(nullptr, nullptr);
    set(grid, "enabled", CefV8Value::CreateBool(snapshot.grid_enabled));
    set(result, "grid", grid);
    auto physics = CefV8Value::CreateObject(nullptr, nullptr);
    set(physics, "gravity", vector(snapshot.physics.gravity));
    set(physics, "floor_y", CefV8Value::CreateDouble(snapshot.physics.floor_y));
    set(physics, "floor_restitution", CefV8Value::CreateDouble(snapshot.physics.floor_restitution));
    set(physics, "fixed_dt", CefV8Value::CreateDouble(snapshot.physics.fixed_dt));
    set(result, "physics", physics);
    return result;
}

bool read_transform(const CefV8ValueList& arguments, SceneRequest& request) {
    if (arguments.size() != 3 || !arguments[0]->IsString() || !arguments[1]->IsString()) return false;
    request.transform.scene = arguments[0]->GetStringValue().ToString();
    request.transform.actor = arguments[1]->GetStringValue().ToString();
    if (request.transform.scene.empty() || request.transform.actor.empty()) return false;
    const auto patch = arguments[2];
    if (!patch->IsObject() || patch->IsArray() || patch->IsFunction()) return false;
    std::vector<CefString> keys;
    if (!patch->GetKeys(keys)) return false;
    for (const auto& key : keys) {
        if (key != "archive_session" && key != "position" && key != "rotation" && key != "scale") return false;
    }
    const auto session = patch->GetValue("archive_session");
    if (!session || !session->IsString() || session->GetStringValue().empty()) return false;
    request.archive_session = session->GetStringValue().ToString();
    const auto vector = [&](const char* key, std::optional<std::array<float, 3>>& result) {
        if (!patch->HasValue(key)) return true;
        const auto value = patch->GetValue(key);
        std::array<float, 3> copy;
        if (!read_float3(value, copy)) return false;
        result = copy;
        return true;
    };
    return vector("position", request.transform.position) && vector("rotation", request.transform.rotation) &&
        vector("scale", request.transform.scale) &&
        (request.transform.position || request.transform.rotation || request.transform.scale);
}

bool read_transforms(const CefV8ValueList& arguments, SceneRequest& request) {
    if (arguments.size() != 1) return false;
    const auto options = arguments[0];
    if (!options || !options->IsObject() || options->IsArray() || options->IsFunction()) return false;
    std::vector<CefString> keys;
    if (!options->GetKeys(keys) || keys.size() != 2) return false;
    for (const auto& key : keys) if (key != "archive_session" && key != "actors") return false;
    const auto session = options->GetValue("archive_session");
    const auto actors = options->GetValue("actors");
    if (!session || !session->IsString() || session->GetStringValue().empty() ||
        !actors || !actors->IsArray()) return false;
    request.archive_session = session->GetStringValue().ToString();
    for (int i = 0; i < actors->GetArrayLength(); ++i) {
        const auto actor = actors->GetValue(i);
        if (!actor || !actor->IsString() || actor->GetStringValue().empty()) return false;
        request.actor_guids.push_back(actor->GetStringValue().ToString());
    }
    return true;
}

CefRefPtr<CefV8Value> transform_value(const ActorTransformSnapshot& snapshot) {
    auto result = CefV8Value::CreateObject(nullptr, nullptr);
    result->SetValue("status", CefV8Value::CreateString("success"), V8_PROPERTY_ATTRIBUTE_NONE);
    result->SetValue("scene", CefV8Value::CreateString(snapshot.scene), V8_PROPERTY_ATTRIBUTE_NONE);
    auto actor = CefV8Value::CreateObject(nullptr, nullptr);
    actor->SetValue("name", CefV8Value::CreateString(snapshot.actor), V8_PROPERTY_ATTRIBUTE_NONE);
    actor->SetValue("actor_guid", CefV8Value::CreateString(snapshot.actor_guid), V8_PROPERTY_ATTRIBUTE_NONE);
    actor->SetValue("actor_version", CefV8Value::CreateInt(snapshot.actor_version), V8_PROPERTY_ATTRIBUTE_NONE);
    auto geometry = CefV8Value::CreateObject(nullptr, nullptr);
    const auto vector = [&](const char* key, const std::array<float, 3>& value) {
        auto array = CefV8Value::CreateArray(3);
        for (int index = 0; index < 3; ++index) array->SetValue(index, CefV8Value::CreateDouble(value[index]));
        geometry->SetValue(key, array, V8_PROPERTY_ATTRIBUTE_NONE);
    };
    vector("position", snapshot.position);
    vector("rotation", snapshot.rotation);
    vector("scale", snapshot.scale);
    actor->SetValue("geometry", geometry, V8_PROPERTY_ATTRIBUTE_NONE);
    result->SetValue("actor", actor, V8_PROPERTY_ATTRIBUTE_NONE);
    return result;
}

CefRefPtr<CefV8Value> transforms_value(const std::vector<ActorTransformSnapshot>& snapshots,
                                       const std::string& archive_session) {
    auto result = CefV8Value::CreateObject(nullptr, nullptr);
    result->SetValue("archive_session", CefV8Value::CreateString(archive_session), V8_PROPERTY_ATTRIBUTE_NONE);
    auto actors = CefV8Value::CreateArray(static_cast<int>(snapshots.size()));
    for (size_t i = 0; i < snapshots.size(); ++i) {
        const auto& snapshot = snapshots[i];
        auto item = CefV8Value::CreateObject(nullptr, nullptr);
        item->SetValue("scene", CefV8Value::CreateString(snapshot.scene), V8_PROPERTY_ATTRIBUTE_NONE);
        item->SetValue("actor", CefV8Value::CreateString(snapshot.actor), V8_PROPERTY_ATTRIBUTE_NONE);
        item->SetValue("actor_guid", CefV8Value::CreateString(snapshot.actor_guid), V8_PROPERTY_ATTRIBUTE_NONE);
        item->SetValue("actor_version", CefV8Value::CreateInt(snapshot.actor_version), V8_PROPERTY_ATTRIBUTE_NONE);
        const auto vector = [&](const char* key, const std::array<float, 3>& value) {
            auto array = CefV8Value::CreateArray(3);
            for (int axis = 0; axis < 3; ++axis) array->SetValue(axis, CefV8Value::CreateDouble(value[axis]));
            item->SetValue(key, array, V8_PROPERTY_ATTRIBUTE_NONE);
        };
        vector("position", snapshot.position);
        vector("rotation", snapshot.rotation);
        vector("scale", snapshot.scale);
        actors->SetValue(static_cast<int>(i), item);
    }
    result->SetValue("actors", actors, V8_PROPERTY_ATTRIBUTE_NONE);
    return result;
}

bool read_selection(const CefV8ValueList& arguments, ActorSelection& selection) {
    if ((arguments.size() != 3 && arguments.size() != 4) ||
        !arguments[0]->IsString() || !arguments[1]->IsString() || !arguments[2]->IsString()) return false;
    selection.scene = arguments[0]->GetStringValue().ToString();
    selection.actor_type = arguments[1]->GetStringValue().ToString();
    selection.actor = arguments[2]->GetStringValue().ToString();
    if (arguments.size() == 3) return true;
    const auto context = arguments[3];
    if (!context->IsObject() || context->IsArray() || context->IsFunction()) return false;
    if (context->HasValue("sourceViewport")) {
        const auto value = context->GetValue("sourceViewport");
        if (!value || !value->IsString()) return false;
        selection.source_viewport = value->GetStringValue().ToString();
    }
    if (selection.source_viewport != "main" && selection.source_viewport != "cameraView") return false;
    if (context->HasValue("sourceCameraId")) {
        const auto value = context->GetValue("sourceCameraId");
        if (!value || !value->IsString()) return false;
        selection.source_camera_id = value->GetStringValue().ToString();
        if (selection.source_camera_id.empty()) return false;
    }
    if (selection.source_viewport == "cameraView" && selection.source_camera_id.empty()) return false;
    if (selection.source_viewport == "main") selection.source_camera_id.clear();
    if (context->HasValue("node_index")) {
        const auto value = context->GetValue("node_index");
        if (!value) return false;
        if (!value->IsNull()) {
            if (!(value->IsUInt() || value->IsInt()) || value->GetDoubleValue() < 0 ||
                value->GetDoubleValue() >= std::numeric_limits<std::uint32_t>::max()) return false;
            selection.node_index = value->GetUIntValue();
        }
    }
    return true;
}

CefRefPtr<CefV8Value> selection_value(const std::optional<ActorSelection>& selection) {
    auto value = CefV8Value::CreateObject(nullptr, nullptr);
    if (!selection) return value;
    value->SetValue("scene", CefV8Value::CreateString(selection->scene), V8_PROPERTY_ATTRIBUTE_NONE);
    value->SetValue("actor_type", CefV8Value::CreateString(selection->actor_type), V8_PROPERTY_ATTRIBUTE_NONE);
    value->SetValue("actor", CefV8Value::CreateString(selection->actor), V8_PROPERTY_ATTRIBUTE_NONE);
    value->SetValue("source_viewport", CefV8Value::CreateString(selection->source_viewport), V8_PROPERTY_ATTRIBUTE_NONE);
    value->SetValue("source_camera_id", CefV8Value::CreateString(selection->source_camera_id), V8_PROPERTY_ATTRIBUTE_NONE);
    if (selection->actor_guid) value->SetValue("actor_guid", CefV8Value::CreateString(*selection->actor_guid), V8_PROPERTY_ATTRIBUTE_NONE);
    if (selection->node_index) value->SetValue("node_index", CefV8Value::CreateUInt(*selection->node_index), V8_PROPERTY_ATTRIBUTE_NONE);
    return value;
}

CefRefPtr<CefV8Value> scene_value(const ActiveSceneSnapshot& snapshot) {
    auto value = CefV8Value::CreateObject(nullptr, nullptr);
    value->SetValue("archive_session", CefV8Value::CreateString(snapshot.archive_session), V8_PROPERTY_ATTRIBUTE_NONE);
    if (snapshot.archive_session.empty()) return value;
    value->SetValue("scene", CefV8Value::CreateString(snapshot.scene), V8_PROPERTY_ATTRIBUTE_NONE);
    value->SetValue("name", CefV8Value::CreateString(snapshot.name), V8_PROPERTY_ATTRIBUTE_NONE);
    value->SetValue("project_path", CefV8Value::CreateString(snapshot.project_path), V8_PROPERTY_ATTRIBUTE_NONE);
    value->SetValue("read_only", CefV8Value::CreateBool(snapshot.read_only), V8_PROPERTY_ATTRIBUTE_NONE);
    value->SetValue("tree_revision", CefV8Value::CreateString(std::to_string(snapshot.tree_revision)), V8_PROPERTY_ATTRIBUTE_NONE);
    value->SetValue("selection", selection_value(snapshot.selection), V8_PROPERTY_ATTRIBUTE_NONE);
    return value;
}

bool number(CefRefPtr<CefV8Value> object, const char* name, float& output) {
    const auto value = object->GetValue(name);
    if (!value || !(value->IsDouble() || value->IsInt() || value->IsUInt())) return false;
    const auto numeric = value->GetDoubleValue();
    if (!std::isfinite(numeric) || std::abs(numeric) > std::numeric_limits<float>::max()) return false;
    output = static_cast<float>(numeric);
    return true;
}

std::optional<std::vector<DragRegion>> read_drag_regions(const CefV8ValueList& arguments) {
    if (arguments.size() != 1 || !arguments[0]->IsArray()) return std::nullopt;
    const auto array = arguments[0];
    const int count = array->GetArrayLength();
    if (count > 256) return std::nullopt;
    std::vector<DragRegion> result;
    result.reserve(count);
    for (int index = 0; index < count; ++index) {
        const auto value = array->GetValue(index);
        DragRegion region{};
        if (!value || !value->IsObject() ||
            !number(value, "x", region.x) || !number(value, "y", region.y) ||
            !number(value, "w", region.width) || !number(value, "h", region.height) ||
            region.width < 0 || region.height < 0) return std::nullopt;
        result.push_back(region);
    }
    return result;
}

bool read_panel_options(const CefV8ValueList& arguments, WindowRequest& request) {
    if (arguments.size() != 1 || !arguments[0]->IsObject() || arguments[0]->IsArray() || arguments[0]->IsFunction()) return false;
    const auto object = arguments[0];
    PanelOptions options;
    const bool camera = request.method == WindowMethod::CreateCameraView;
    const bool create = request.method == WindowMethod::CreatePanel || camera;
    if (create) { options.width = camera ? 960 : 400; options.height = camera ? 540 : 600; }
    const auto integer = [&](const char* name, int& output, int minimum) {
        if (!object->HasValue(name)) return true;
        const auto value = object->GetValue(name);
        if (!value || !value->IsInt() || value->GetIntValue() < minimum) return false;
        output = value->GetIntValue();
        return true;
    };
    const auto boolean = [&](const char* name, bool& output) {
        if (!object->HasValue(name)) return true;
        const auto value = object->GetValue(name);
        if (!value || !value->IsBool()) return false;
        output = value->GetBoolValue();
        return true;
    };
    const auto string = [&](const char* name, std::string& output) {
        if (!object->HasValue(name)) return true;
        const auto value = object->GetValue(name);
        if (!value || !value->IsString()) return false;
        output = value->GetStringValue().ToString();
        return true;
    };
    if (!integer("width", options.width, create ? 1 : 0) || !integer("height", options.height, create ? 1 : 0) ||
        !integer("x", options.x, std::numeric_limits<int>::min()) ||
        !integer("y", options.y, std::numeric_limits<int>::min()) ||
        !integer("zPriority", options.z_priority, std::numeric_limits<int>::min()) ||
        !integer("tabId", request.tab_id, 0) ||
        !boolean("nativeDocked", options.native_docked) || !boolean("detached", options.detached) ||
        !boolean("maximized", options.maximized) || !string("panelId", options.panel_id) ||
        !string("routePath", options.route) || !string("dockingPos", options.docking_pos)) return false;
    options.title = options.panel_id;
    if (!string("title", options.title)) return false;
    if (camera && (object->HasValue("cameraHandle") || object->HasValue("handle") ||
        !string("archiveSession", request.archive_session) || request.archive_session.empty() ||
        !string("sceneId", request.scene_id) || request.scene_id.empty() ||
        !string("cameraId", request.camera_id) || request.camera_id.empty() ||
        request.camera_id == "undefined" || request.camera_id == "null")) return false;
    if (options.docking_pos != "left" && options.docking_pos != "right" && options.docking_pos != "bottom" &&
        options.docking_pos != "right_top" && options.docking_pos != "right_bottom" &&
        options.docking_pos != "left_bottom" && options.docking_pos != "center") return false;
    request.panel = std::move(options);
    return true;
}

CefRefPtr<CefV8Value> snapshot_value(const WindowSnapshot& snapshot, int browser_id,
                                  const std::string& frame_id) {
    auto value = CefV8Value::CreateObject(nullptr, nullptr);
    value->SetValue("browserId", CefV8Value::CreateString(std::to_string(browser_id)), V8_PROPERTY_ATTRIBUTE_NONE);
    value->SetValue("frameId", CefV8Value::CreateString(frame_id), V8_PROPERTY_ATTRIBUTE_NONE);
    value->SetValue("tabId", CefV8Value::CreateInt(snapshot.tab_id), V8_PROPERTY_ATTRIBUTE_NONE);
    value->SetValue("panelId", CefV8Value::CreateString(snapshot.panel_id), V8_PROPERTY_ATTRIBUTE_NONE);
    value->SetValue("width", CefV8Value::CreateInt(snapshot.width), V8_PROPERTY_ATTRIBUTE_NONE);
    value->SetValue("height", CefV8Value::CreateInt(snapshot.height), V8_PROPERTY_ATTRIBUTE_NONE);
    value->SetValue("minimized", CefV8Value::CreateBool(snapshot.minimized), V8_PROPERTY_ATTRIBUTE_NONE);
    value->SetValue("detached", CefV8Value::CreateBool(snapshot.detached), V8_PROPERTY_ATTRIBUTE_NONE);
    value->SetValue("dockState", CefV8Value::CreateString(snapshot.dock_state), V8_PROPERTY_ATTRIBUTE_NONE);
    auto regions = CefV8Value::CreateArray(static_cast<int>(snapshot.drag_regions.size()));
    for (int index = 0; index < static_cast<int>(snapshot.drag_regions.size()); ++index) {
        const auto& region = snapshot.drag_regions[index];
        auto item = CefV8Value::CreateObject(nullptr, nullptr);
        item->SetValue("x", CefV8Value::CreateDouble(region.x), V8_PROPERTY_ATTRIBUTE_NONE);
        item->SetValue("y", CefV8Value::CreateDouble(region.y), V8_PROPERTY_ATTRIBUTE_NONE);
        item->SetValue("w", CefV8Value::CreateDouble(region.width), V8_PROPERTY_ATTRIBUTE_NONE);
        item->SetValue("h", CefV8Value::CreateDouble(region.height), V8_PROPERTY_ATTRIBUTE_NONE);
        regions->SetValue(index, item);
    }
    value->SetValue("dragRegions", regions, V8_PROPERTY_ATTRIBUTE_NONE);
    return value;
}

// CEF's RejectPromise only accepts a string. Adopt Promise.reject(Error) to
// preserve a real Error with code, without evaluating or injecting script data.
void reject(CefRefPtr<CefV8Context> context, CefRefPtr<CefV8Value> promise,
            const std::string& code, const std::string& message) {
    const auto global = context->GetGlobal();
    const auto error_constructor = global->GetValue("Error");
    const auto error = error_constructor->ExecuteFunctionWithContext(context, nullptr,
        {CefV8Value::CreateString(message)});
    if (!error) { promise->RejectPromise(code + ": " + message); return; }
    error->SetValue("code", CefV8Value::CreateString(code), V8_PROPERTY_ATTRIBUTE_NONE);
    const auto constructor = global->GetValue("Promise");
    const auto reject_function = constructor->GetValue("reject");
    const auto rejected = reject_function->ExecuteFunctionWithContext(context, constructor, {error});
    if (rejected) promise->ResolvePromise(rejected);
    else promise->RejectPromise(code + ": " + message);
}

}  // namespace

class CoronaV8Endpoint final : public std::enable_shared_from_this<CoronaV8Endpoint> {
public:
    explicit CoronaV8Endpoint(CefRefPtr<CefV8Context> context)
        : context_(context), runner_(context->GetTaskRunner()),
          browser_id_(context->GetBrowser()->GetIdentifier()),
          frame_id_(context->GetFrame()->GetIdentifier().ToString()) {}

    bool active() const { return active_.load(); }
    int browser_id() const { return browser_id_; }

    void release() {
        CEF_REQUIRE_RENDERER_THREAD();
        active_.store(false);
        BrowserManager::instance().enqueue_main_thread_task([browser_id = browser_id_, context_id = context_id_] {
            release_viewport_pointer_context(browser_id, context_id);
            viewport_pick_requests().cancel_context(context_id);
        });
        pending_.clear();
        subscribers_.clear();
        context_ = nullptr;
    }

    std::uint64_t subscribe(WindowEvent event, const CefV8ValueList& arguments, CefString& exception) {
        CEF_REQUIRE_RENDERER_THREAD();
        if (!owns_current_context()) { exception = "PERMISSION_DENIED: Window binding belongs to another context"; return 0; }
        if (arguments.size() != 1 || !arguments[0] || !arguments[0]->IsFunction()) {
            exception = "INVALID_ARGUMENT: Expected exactly one event callback";
            return 0;
        }
        const auto id = ++sequence_;
        subscribers_.emplace(id, Subscriber{event, arguments[0]});
        return id;
    }

    void unsubscribe(std::uint64_t id, CefString& exception) {
        CEF_REQUIRE_RENDERER_THREAD();
        if (!active()) return;
        if (!owns_current_context()) { exception = "PERMISSION_DENIED: Window binding belongs to another context"; return; }
        subscribers_.erase(id);
    }

    void post_event(WindowEventData data) {
        runner_->PostTask(CefCreateClosureTask(base::BindOnce([](std::weak_ptr<CoronaV8Endpoint> weak,
                                                               WindowEventData data) {
            if (const auto endpoint = weak.lock()) endpoint->deliver(data);
        }, weak_from_this(), std::move(data))));
    }

    CefRefPtr<CefV8Value> invoke(ScriptInputMethod method, const CefV8ValueList& arguments) {
        CEF_REQUIRE_RENDERER_THREAD();
        auto promise = CefV8Value::CreatePromise();
        if (!owns_current_context()) {
            if (active() && context_ && context_->IsValid()) reject(context_, promise, "PERMISSION_DENIED", "Input belongs to another context");
            else promise->RejectPromise("CONTEXT_CHANGED");
            return promise;
        }
        ScriptInputRequest request;
        if (!read_script_input(arguments, request)) {
            reject(context_, promise, "INVALID_ARGUMENT", "Expected scoped typed keyboard or mouse input"); return promise;
        }
        request.source.browser_id = browser_id_; request.source.context_id = context_id_;
        const auto id = ++sequence_;
        pending_.emplace(id, Pending{method, promise});
        const auto weak = weak_from_this();
        const bool posted = CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(
            [](std::weak_ptr<CoronaV8Endpoint> weak, std::uint64_t id, ScriptInputRequest request) {
                const auto endpoint = weak.lock();
                if (!endpoint || !endpoint->active()) return;
                WindowReply prepared;
                try { prepared = prepare_camera_window_request(request.source); }
                catch (...) { prepared.error_code = "NATIVE_FAILURE"; prepared.error_message = "Input camera lookup failed"; }
                if (!prepared.error_code.empty()) { endpoint->complete(id, ScriptInputReply{prepared.error_code, prepared.error_message}); return; }
                BrowserManager::instance().enqueue_main_thread_task([weak, id, request = std::move(request)]() mutable {
                    const auto current = [weak] { const auto e = weak.lock(); return e && e->active(); };
                    if (!current()) return;
                    ScriptInputReply reply;
                    try { reply = dispatch_script_input(std::move(request), current); }
                    catch (...) { reply = {"NATIVE_FAILURE", "Input dispatch failed"}; }
                    if (const auto e = weak.lock()) e->complete(id, std::move(reply));
                });
            }, weak, id, std::move(request))));
        if (!posted) { pending_.erase(id); reject(context_, promise, "NATIVE_UNAVAILABLE", "Scene owner unavailable"); }
        return promise;
    }

    CefRefPtr<CefV8Value> invoke(ViewportMethod method, const CefV8ValueList& arguments) {
        CEF_REQUIRE_RENDERER_THREAD();
        auto promise = CefV8Value::CreatePromise();
        if (!owns_current_context()) {
            if (active() && context_ && context_->IsValid()) reject(context_, promise, "PERMISSION_DENIED", "Pick binding belongs to another context");
            else promise->RejectPromise("CONTEXT_CHANGED");
            return promise;
        }
        ViewportPickRequest request;
        if (!read_viewport_pick(arguments, request)) {
            reject(context_, promise, "INVALID_ARGUMENT", "Expected scene, camera ID and scoped viewport coordinates"); return promise;
        }
        request.camera.browser_id = browser_id_; request.camera.context_id = context_id_;
        const auto id = ++sequence_;
        pending_.emplace(id, Pending{method, promise});
        const auto weak = weak_from_this();
        const bool posted = CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(
            [](std::weak_ptr<CoronaV8Endpoint> weak, std::uint64_t id, ViewportPickRequest request) {
                const auto endpoint = weak.lock();
                if (!endpoint || !endpoint->active()) return;
                WindowReply prepared;
                try { prepared = prepare_camera_window_request(request.camera); }
                catch (...) { prepared.error_code = "NATIVE_FAILURE"; prepared.error_message = "Pick camera lookup failed"; }
                if (!prepared.error_code.empty()) {
                    ViewportPickReply reply; reply.error_code = prepared.error_code; reply.error_message = prepared.error_message;
                    endpoint->complete(id, std::move(reply)); return;
                }
                BrowserManager::instance().enqueue_main_thread_task([weak, id, request = std::move(request)]() mutable {
                    const auto endpoint = weak.lock();
                    if (!endpoint || !endpoint->active()) return;
                    begin_viewport_pick(std::move(request), [weak, id](const ViewportPickRequest& request, const Corona::ActorPickCompletion& result) {
                        const bool posted = CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(
                            [](std::weak_ptr<CoronaV8Endpoint> weak, std::uint64_t id, ViewportPickRequest request, Corona::ActorPickCompletion result) {
                                const auto endpoint = weak.lock();
                                if (!endpoint || !endpoint->active()) return;
                                ViewportPickReply reply;
                                try { reply = finish_viewport_pick(request, result); }
                                catch (...) { reply.error_code = "NATIVE_FAILURE"; reply.error_message = "Pick result lookup failed"; }
                                endpoint->complete(id, std::move(reply));
                            }, weak, id, request, result)));
                        if (!posted) if (const auto endpoint = weak.lock()) {
                            ViewportPickReply reply; reply.error_code = "NATIVE_UNAVAILABLE"; reply.error_message = "Scene owner unavailable";
                            endpoint->complete(id, std::move(reply));
                        }
                    });
                });
            }, weak, id, std::move(request))));
        if (!posted) {
            pending_.erase(id); reject(context_, promise, "NATIVE_UNAVAILABLE", "Scene owner unavailable");
        }
        return promise;
    }

    CefRefPtr<CefV8Value> invoke(GizmoMethod method, const CefV8ValueList& arguments) {
        CEF_REQUIRE_RENDERER_THREAD();
        auto promise = CefV8Value::CreatePromise();
        if (!owns_current_context()) { promise->RejectPromise("CONTEXT_CHANGED"); return promise; }
        ViewportGizmoTargetRequest target;
        ViewportGizmoPointerRequest pointer;
        if (!read_gizmo_request(method, arguments, target, pointer)) {
            reject(context_, promise, "INVALID_ARGUMENT", "Expected scoped typed viewport gizmo data"); return promise;
        }
        const auto id = ++sequence_;
        pending_.emplace(id, Pending{method, promise});
        const auto weak = weak_from_this();
        CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(
            [](std::weak_ptr<CoronaV8Endpoint> weak, std::uint64_t id, GizmoMethod method,
               ViewportGizmoTargetRequest target, ViewportGizmoPointerRequest pointer) {
                const auto endpoint = weak.lock(); if (!endpoint || !endpoint->active()) return;
                WindowRequest camera{WindowMethod::ConfigureViewport, endpoint->browser_id_};
                camera.scene_id = method == GizmoMethod::SetTarget ? target.scene_id : pointer.scene_id;
                camera.camera_id = method == GizmoMethod::SetTarget ? target.camera_id : pointer.camera_id;
                camera.archive_session = method == GizmoMethod::SetTarget ? target.archive_session : pointer.archive_session;
                const auto checked = prepare_camera_window_request(camera);
                if (!checked.error_code.empty()) {
                    ViewportGizmoReply reply; reply.error_code = checked.error_code; reply.error_message = checked.error_message; endpoint->complete(id, std::move(reply)); return;
                }
                if (method == GizmoMethod::SetTarget) {
                    target.camera_handle = camera.camera_handle;
                    target.camera_authority = camera.camera_authority;
                    if (target.actor_guid) {
                        target.actor_handle = resolve_native_actor_handle_by_guid(target.scene_id, target.archive_session, *target.actor_guid, target.error_code, target.error_message);
                        if (!target.actor_handle) { ViewportGizmoReply reply; reply.error_code = target.error_code; reply.error_message = target.error_message; endpoint->complete(id, std::move(reply)); return; }
                    }
                    BrowserManager::instance().enqueue_main_thread_task([weak, id, target = std::move(target)]() mutable { if (const auto e = weak.lock(); e && e->active()) { ViewportGizmoReply reply; if (!target.camera_authority) { reply.error_code = "PROJECT_CONTEXT_CHANGED"; reply.error_message = "Camera was retired"; } else { std::lock_guard lock(target.camera_authority->mutex); if (!target.camera_authority->active) { reply.error_code = "PROJECT_CONTEXT_CHANGED"; reply.error_message = "Camera was retired"; } else reply = execute_viewport_gizmo_target(target); } e->complete(id, std::move(reply)); } });
                } else {
                    pointer.camera_handle = camera.camera_handle;
                    pointer.camera_authority = camera.camera_authority;
                    BrowserManager::instance().enqueue_main_thread_task([weak, id, pointer = std::move(pointer)]() mutable { if (const auto e = weak.lock(); e && e->active()) { ViewportGizmoReply reply; if (!pointer.camera_authority) { reply.error_code = "PROJECT_CONTEXT_CHANGED"; reply.error_message = "Camera was retired"; } else { std::lock_guard lock(pointer.camera_authority->mutex); if (!pointer.camera_authority->active) { reply.error_code = "PROJECT_CONTEXT_CHANGED"; reply.error_message = "Camera was retired"; } else reply = execute_viewport_gizmo_pointer(pointer); } e->complete(id, std::move(reply)); } });
                }
            }, weak, id, method, std::move(target), std::move(pointer))));
        return promise;
    }

    CefRefPtr<CefV8Value> invoke(SceneMethod method, const CefV8ValueList& arguments) {
        CEF_REQUIRE_RENDERER_THREAD();
        auto promise = CefV8Value::CreatePromise();
        if (!owns_current_context()) {
            if (active() && context_ && context_->IsValid())
                reject(context_, promise, "PERMISSION_DENIED", "Scene binding belongs to another context");
            else promise->RejectPromise("CONTEXT_CHANGED");
            return promise;
        }
        SceneRequest request{method};
        if ((method == SceneMethod::ReadActive && !arguments.empty()) ||
            (method == SceneMethod::ReadTransforms && !read_transforms(arguments, request)) ||
            (method == SceneMethod::SelectActor && !read_selection(arguments, request.selection)) ||
            (method == SceneMethod::SetActorTransform && !read_transform(arguments, request)) ||
            (method == SceneMethod::SetActorProperties && !read_properties(arguments, request)) ||
            (method == SceneMethod::FocusActor && !read_focus(arguments, request)) ||
            (method == SceneMethod::CaptureScreenshot && !read_screenshot(arguments, request)) ||
            (method == SceneMethod::SetCameraProperties && !read_camera_properties(arguments, request)) ||
            ((method == SceneMethod::GetEnvironment || method == SceneMethod::SetEnvironment) && !read_environment(arguments, request))) {
            reject(context_, promise, "INVALID_ARGUMENT", "Invalid native scene operation arguments");
            return promise;
        }
        // Mutations use the archive this context last observed, captured before
        // crossing threads. A queued old selection cannot enter a new archive.
        if (method == SceneMethod::SelectActor) request.archive_session = scene_archive_session_;
        const auto id = ++sequence_;
        pending_.emplace(id, Pending{method, promise});
        const bool posted = CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(
            [](std::weak_ptr<CoronaV8Endpoint> weak, std::uint64_t id, SceneRequest request) {
                const auto endpoint = weak.lock();
                if (!endpoint || !endpoint->active()) return;
                SceneReply reply;
                try { reply = execute_scene_operation(request); }
                catch (const std::exception& error) { reply.error_code = "NATIVE_FAILURE"; reply.error_message = error.what(); }
                catch (...) { reply.error_code = "NATIVE_FAILURE"; reply.error_message = "Native scene operation failed"; }
                endpoint->complete(id, std::move(reply));
            }, weak_from_this(), id, std::move(request))));
        if (!posted) {
            pending_.erase(id);
            reject(context_, promise, "NATIVE_UNAVAILABLE", "Native scene owner is unavailable");
        }
        return promise;
    }

    CefRefPtr<CefV8Value> invoke(WindowMethod method, const CefV8ValueList& arguments) {
        CEF_REQUIRE_RENDERER_THREAD();
        auto promise = CefV8Value::CreatePromise();
        if (!active() || !context_ || !context_->IsValid()) {
            promise->RejectPromise("CONTEXT_CHANGED");
            return promise;
        }
        const auto current = CefV8Context::GetCurrentContext();
        if (!current || !current->IsSame(context_)) {
            reject(context_, promise, "PERMISSION_DENIED", "Window binding belongs to another context");
            return promise;
        }
        WindowRequest request{method, browser_id_};
        request.context_id = context_id_;
        const bool panel_operation = method == WindowMethod::CreatePanel || method == WindowMethod::CreateCameraView ||
            method == WindowMethod::DetachPanel || method == WindowMethod::RedockPanel;
        const bool targeted = method == WindowMethod::CloseTab || method == WindowMethod::FocusTab;
        const bool resize = method == WindowMethod::ResizeCameraView;
        const bool camera_control = resize || method == WindowMethod::ToggleCameraMaximized ||
            method == WindowMethod::CycleCameraWindowMode || method == WindowMethod::ToggleCameraBorderless;
        const bool camera_operation = method == WindowMethod::SuspendCameraViews || method == WindowMethod::CloseCameraView || camera_control;
        const bool pair = method == WindowMethod::CloseCameraView || camera_control;
        const std::size_t offset = resize ? 2u : 0u;
        if (method == WindowMethod::SetDragRegions) request.regions = read_drag_regions(arguments);
        if ((camera_operation && (arguments.size() != offset + (pair ? 2u : 1u) ||
              !arguments[offset]->IsString() || (pair && !arguments[offset + 1]->IsString()))) ||
            (resize && (!arguments[0]->IsInt() || arguments[0]->GetIntValue() <= 0 ||
                        !arguments[1]->IsInt() || arguments[1]->GetIntValue() <= 0)) ||
            (panel_operation && !read_panel_options(arguments, request)) ||
            (method == WindowMethod::ConfigureViewport && !read_viewport_configuration(arguments, request)) ||
            (method == WindowMethod::SetDragRegions && !request.regions) ||
            ((method == WindowMethod::GetCurrent || method == WindowMethod::Close || method == WindowMethod::ToggleMainBorderless) && !arguments.empty()) ||
            (targeted && (arguments.size() != 1 || !arguments[0]->IsInt() || arguments[0]->GetIntValue() < 0))) {
            reject(context_, promise, "INVALID_ARGUMENT", "Invalid native window operation arguments");
            return promise;
        }
        if (targeted) request.tab_id = arguments[0]->GetIntValue();
        if (camera_operation) request.scene_id = arguments[offset]->GetStringValue().ToString();
        if (pair) request.camera_id = arguments[offset + 1]->GetStringValue().ToString();
        if (camera_control && request.scene_id.empty() != request.camera_id.empty()) {
            reject(context_, promise, "INVALID_ARGUMENT", "Scene and camera identity must be supplied together");
            return promise;
        }
        if (resize) { request.width = arguments[0]->GetIntValue(); request.height = arguments[1]->GetIntValue(); }
        if (method == WindowMethod::CreatePanel || method == WindowMethod::CreateCameraView) {
            request.base_url = context_->GetFrame()->GetURL().ToString();
            if (const auto hash = request.base_url.find('#'); hash != std::string::npos) request.base_url.erase(hash);
        }
        const auto id = ++sequence_;
        pending_.emplace(id, Pending{method, promise});
        if (method == WindowMethod::CreateCameraView || method == WindowMethod::ConfigureViewport) {
            const bool posted = CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(
                [](std::weak_ptr<CoronaV8Endpoint> weak, std::uint64_t id, WindowRequest request) {
                    const auto endpoint = weak.lock();
                    if (!endpoint || !endpoint->active()) return;
                    WindowReply reply;
                    try { reply = prepare_camera_window_request(request); }
                    catch (const std::exception& error) { reply.error_code = "NATIVE_FAILURE"; reply.error_message = error.what(); }
                    catch (...) { reply.error_code = "NATIVE_FAILURE"; reply.error_message = "Camera identity lookup failed"; }
                    if (!reply.error_code.empty()) endpoint->complete(id, std::move(reply));
                    else endpoint->queue_operation(id, std::move(request));
                }, weak_from_this(), id, std::move(request))));
            if (!posted) {
                pending_.erase(id);
                reject(context_, promise, "NATIVE_UNAVAILABLE", "Native scene owner is unavailable");
            }
        } else queue_operation(id, std::move(request));
        return promise;
    }

    CefRefPtr<CefV8Value> invoke(EditorApiMethod method, const CefV8ValueList& arguments) {
        CEF_REQUIRE_RENDERER_THREAD();
        auto promise = CefV8Value::CreatePromise();
        if (!owns_current_context()) {
            promise->RejectPromise("CONTEXT_CHANGED");
            return promise;
        }
        if (method == EditorApiMethod::On || method == EditorApiMethod::Off) {
            if (method == EditorApiMethod::On) {
                if (arguments.size() != 2 || !arguments[0]->IsString() || !arguments[1]->IsFunction()) {
                    reject(context_, promise, "INVALID_ARGUMENT", "Expected an event name and callback");
                    return promise;
                }
                const auto token = EditorApiCallbackRegistry::instance().register_cef_v8_callback(
                    arguments[0]->GetStringValue().ToString(), context_, arguments[1]);
                if (!token) reject(context_, promise, "INVALID_ARGUMENT", "Editor API event is unavailable");
                else promise->ResolvePromise(CefV8Value::CreateDouble(static_cast<double>(token)));
            } else {
                if (arguments.size() != 1 || !(arguments[0]->IsUInt() || arguments[0]->IsInt() || arguments[0]->IsDouble())) {
                    reject(context_, promise, "INVALID_ARGUMENT", "Expected a callback token");
                    return promise;
                }
                const auto token = arguments[0]->GetDoubleValue();
                if (!std::isfinite(token) || token < 0 || std::trunc(token) != token ||
                    token > static_cast<double>(std::numeric_limits<std::uint64_t>::max())) {
                    reject(context_, promise, "INVALID_ARGUMENT", "Expected a callback token");
                    return promise;
                }
                const auto removed = EditorApiCallbackRegistry::instance().unregister(
                    static_cast<std::uint64_t>(token));
                promise->ResolvePromise(CefV8Value::CreateBool(removed));
            }
            return promise;
        }
        std::string api_name;
        nlohmann::json args;
        if (!read_editor_api_call(arguments, api_name, args)) {
            reject(context_, promise, "INVALID_ARGUMENT", "Expected an API name and argument array");
            return promise;
        }
        const auto id = ++sequence_;
        pending_.emplace(id, Pending{EditorApiMethod::Invoke, promise});
        const auto weak = weak_from_this();
        const bool posted = CefPostTask(TID_UI, CefCreateClosureTask(base::BindOnce(
            [](std::weak_ptr<CoronaV8Endpoint> weak, std::uint64_t id,
               std::string api_name, nlohmann::json args, NativeContext context) {
                const auto endpoint = weak.lock();
                if (!endpoint || !endpoint->active()) return;
                NativeResult result;
                try {
                    register_builtin_native_api_handlers();
                    result = EditorApiRegistry::instance().invoke(
                        {std::move(api_name), std::move(args), EditorApiCaller::Cef}, context);
                } catch (const std::exception& error) {
                    result = native_failure(error.what(), 500, "editor-api");
                } catch (...) {
                    result = native_failure("Native editor API invocation failed", 500, "editor-api");
                }
                endpoint->complete(id, std::move(result));
            }, weak, id, std::move(api_name), std::move(args),
            NativeContext{context_->GetBrowser(), context_->GetFrame(), 0, false})));
        if (!posted) {
            pending_.erase(id);
            reject(context_, promise, "NATIVE_UNAVAILABLE", "Native editor API is unavailable");
        }
        return promise;
    }

private:
    void queue_operation(std::uint64_t id, WindowRequest request) {
        // Only the immutable request DTO and weak lifetime token cross threads.
        BrowserManager::instance().enqueue_main_thread_task(
            [weak = weak_from_this(), id, request = std::move(request)] {
                const auto endpoint = weak.lock();
                if (!endpoint || !endpoint->active()) return;
                WindowReply reply;
                try { reply = execute_window_operation(request); }
                catch (const std::exception& error) { reply.error_code = "NATIVE_FAILURE"; reply.error_message = error.what(); }
                catch (...) { reply.error_code = "NATIVE_FAILURE"; reply.error_message = "Native window operation failed"; }
                if (request.method == WindowMethod::SuspendCameraViews && reply.error_code.empty()) {
                    // Preserve the lifecycle fence: retirement executes in this turn;
                    // acknowledgment is a separate queued main-thread turn.
                    BrowserManager::instance().enqueue_main_thread_task([weak, id, reply = std::move(reply)] {
                        if (const auto endpoint = weak.lock(); endpoint && endpoint->active()) endpoint->complete(id, reply);
                    });
                } else endpoint->complete(id, std::move(reply));
            });
    }

    bool owns_current_context() const {
        const auto current = CefV8Context::GetCurrentContext();
        return active() && context_ && context_->IsValid() && current && current->IsSame(context_);
    }

    void deliver(const WindowEventData& data) {
        CEF_REQUIRE_RENDERER_THREAD();
        if (!active() || !context_ || !context_->IsValid()) return;
        const auto context = context_;
        if (!context->Enter()) return;
        std::vector<std::uint64_t> ids;
        for (const auto& [id, subscriber] : subscribers_) if (subscriber.type == data.type) ids.push_back(id);
        for (const auto id : ids) {
            const auto entry = subscribers_.find(id);
            if (!active() || entry == subscribers_.end()) continue;
            const auto callback = entry->second.callback;
            // A fresh V8 object for each subscriber prevents mutation of another
            // subscriber's event. No V8 handles or JSON cross the thread boundary.
            auto value = CefV8Value::CreateObject(nullptr, nullptr);
            value->SetValue("tabId", CefV8Value::CreateInt(data.tab_id), V8_PROPERTY_ATTRIBUTE_NONE);
            value->SetValue("panelId", CefV8Value::CreateString(data.panel_id), V8_PROPERTY_ATTRIBUTE_NONE);
            const bool rethrow = callback->WillRethrowExceptions();
            callback->SetRethrowExceptions(false);
            callback->ExecuteFunctionWithContext(context, nullptr, {value});
            if (callback->HasException()) {
                CFW_LOG_WARNING("Native window event callback failed: {}", callback->GetException()->GetMessage().ToString());
                callback->ClearException();
            }
            callback->SetRethrowExceptions(rethrow);
        }
        context->Exit();
    }

    struct Subscriber {
        WindowEvent type;
        CefRefPtr<CefV8Value> callback;
    };
    template <typename Reply>
    void complete(std::uint64_t id, Reply reply) {
        runner_->PostTask(CefCreateClosureTask(base::BindOnce([](std::weak_ptr<CoronaV8Endpoint> weak,
                                                               std::uint64_t id, Reply reply) {
            const auto endpoint = weak.lock();
            if (endpoint) endpoint->resolve(id, reply);
        }, weak_from_this(), id, std::move(reply))));
    }

    void resolve(std::uint64_t id, const ScriptInputReply& reply) {
        CEF_REQUIRE_RENDERER_THREAD();
        const auto entry = pending_.find(id);
        if (entry == pending_.end()) return;
        const auto promise = entry->second.promise;
        pending_.erase(entry);
        if (!active() || !context_ || !context_->IsValid() || !context_->Enter()) return;
        if (!reply.error_code.empty()) reject(context_, promise, reply.error_code, reply.error_message);
        else {
            auto value = CefV8Value::CreateObject(nullptr, nullptr);
            value->SetValue("status", CefV8Value::CreateString("queued"), V8_PROPERTY_ATTRIBUTE_NONE);
            promise->ResolvePromise(value);
        }
        context_->Exit();
    }

    void resolve(std::uint64_t id, const ViewportPickReply& reply) {
        CEF_REQUIRE_RENDERER_THREAD();
        const auto entry = pending_.find(id);
        if (entry == pending_.end()) return;
        const auto promise = entry->second.promise;
        pending_.erase(entry);
        if (!active() || !context_ || !context_->IsValid() || !context_->Enter()) return;
        if (!reply.error_code.empty()) reject(context_, promise, reply.error_code, reply.error_message);
        else promise->ResolvePromise(viewport_pick_value(reply));
        context_->Exit();
    }

    void resolve(std::uint64_t id, const ViewportGizmoReply& reply) {
        CEF_REQUIRE_RENDERER_THREAD();
        const auto entry = pending_.find(id); if (entry == pending_.end()) return;
        const auto promise = entry->second.promise; pending_.erase(entry);
        if (!active() || !context_ || !context_->IsValid() || !context_->Enter()) return;
        if (!reply.error_code.empty()) reject(context_, promise, reply.error_code, reply.error_message);
        else {
            auto value = CefV8Value::CreateObject(nullptr, nullptr);
            value->SetValue("axis", CefV8Value::CreateString(reply.axis), V8_PROPERTY_ATTRIBUTE_NONE);
            value->SetValue("consumed", CefV8Value::CreateBool(reply.consumed), V8_PROPERTY_ATTRIBUTE_NONE);
            value->SetValue("dragging", CefV8Value::CreateBool(reply.dragging), V8_PROPERTY_ATTRIBUTE_NONE);
            value->SetValue("ended", CefV8Value::CreateBool(reply.ended), V8_PROPERTY_ATTRIBUTE_NONE);
            value->SetValue("cancelled", CefV8Value::CreateBool(reply.cancelled), V8_PROPERTY_ATTRIBUTE_NONE);
            auto position = CefV8Value::CreateArray(3);
            for (int i = 0; i < 3; ++i) position->SetValue(i, CefV8Value::CreateDouble(reply.position[i]));
            value->SetValue("position", position, V8_PROPERTY_ATTRIBUTE_NONE);
            promise->ResolvePromise(value);
        }
        context_->Exit();
    }

    void resolve(std::uint64_t id, const SceneReply& reply) {
        CEF_REQUIRE_RENDERER_THREAD();
        const auto entry = pending_.find(id);
        if (entry == pending_.end()) return;
        const auto pending = entry->second;
        pending_.erase(entry);
        if (!active() || !context_ || !context_->IsValid() || !context_->Enter()) return;
        if (!reply.error_code.empty()) reject(context_, pending.promise, reply.error_code, reply.error_message);
        else if (std::get<SceneMethod>(pending.method) == SceneMethod::ReadActive) {
            scene_archive_session_ = reply.active.archive_session;
            pending.promise->ResolvePromise(scene_value(reply.active));
        } else if (std::get<SceneMethod>(pending.method) == SceneMethod::ReadTransforms) {
            pending.promise->ResolvePromise(transforms_value(reply.transforms, reply.active.archive_session));
        } else if (std::get<SceneMethod>(pending.method) == SceneMethod::SetActorTransform) {
            pending.promise->ResolvePromise(transform_value(reply.transform));
        } else if (std::get<SceneMethod>(pending.method) == SceneMethod::SetActorProperties) {
            pending.promise->ResolvePromise(properties_value(reply.properties));
        } else if (std::get<SceneMethod>(pending.method) == SceneMethod::FocusActor) {
            pending.promise->ResolvePromise(focus_value(reply.focus));
        } else if (std::get<SceneMethod>(pending.method) == SceneMethod::CaptureScreenshot) {
            pending.promise->ResolvePromise(screenshot_value(reply.screenshot));
        } else if (std::get<SceneMethod>(pending.method) == SceneMethod::SetCameraProperties) {
            pending.promise->ResolvePromise(camera_properties_value(reply.camera));
        } else if (std::get<SceneMethod>(pending.method) == SceneMethod::GetEnvironment ||
                   std::get<SceneMethod>(pending.method) == SceneMethod::SetEnvironment) {
            pending.promise->ResolvePromise(environment_value(reply.environment));
        } else {
            auto value = CefV8Value::CreateObject(nullptr, nullptr);
            value->SetValue("status", CefV8Value::CreateString("success"), V8_PROPERTY_ATTRIBUTE_NONE);
            value->SetValue("scene", CefV8Value::CreateString(reply.selection.scene), V8_PROPERTY_ATTRIBUTE_NONE);
            value->SetValue("actor_type", CefV8Value::CreateString(reply.selection.actor_type), V8_PROPERTY_ATTRIBUTE_NONE);
            value->SetValue("actor", CefV8Value::CreateString(reply.selection.actor), V8_PROPERTY_ATTRIBUTE_NONE);
            pending.promise->ResolvePromise(value);
        }
        context_->Exit();
    }

    void resolve(std::uint64_t id, const NativeResult& reply) {
        CEF_REQUIRE_RENDERER_THREAD();
        const auto entry = pending_.find(id);
        if (entry == pending_.end()) return;
        const auto promise = entry->second.promise;
        pending_.erase(entry);
        if (!active() || !context_ || !context_->IsValid() || !context_->Enter()) return;
        if (!reply.success) reject(context_, promise, std::to_string(reply.error_code), reply.error);
        else {
            auto response = CefV8Value::CreateObject(nullptr, nullptr);
            response->SetValue("status", CefV8Value::CreateString("success"), V8_PROPERTY_ATTRIBUTE_NONE);
            response->SetValue("data", json_to_v8(reply.data), V8_PROPERTY_ATTRIBUTE_NONE);
            promise->ResolvePromise(response);
        }
        context_->Exit();
    }

    void resolve(std::uint64_t id, const WindowReply& reply) {
        CEF_REQUIRE_RENDERER_THREAD();
        const auto entry = pending_.find(id);
        if (entry == pending_.end()) return;
        const auto pending = entry->second;
        const auto method = std::get<WindowMethod>(pending.method);
        const auto promise = pending.promise;
        pending_.erase(entry);
        if (!active() || !context_ || !context_->IsValid() || !context_->Enter()) return;
        if (!reply.error_code.empty()) reject(context_, promise, reply.error_code, reply.error_message);
        else if (method == WindowMethod::ConfigureViewport && reply.viewport)
            promise->ResolvePromise(viewport_configuration_value(*reply.viewport));
        else if (method == WindowMethod::GetCurrent || method == WindowMethod::SetDragRegions)
            promise->ResolvePromise(snapshot_value(reply.snapshot, browser_id_, frame_id_));
        else if (method == WindowMethod::CreatePanel || method == WindowMethod::CreateCameraView) {
            auto created = CefV8Value::CreateObject(nullptr, nullptr);
            created->SetValue("tab_id", CefV8Value::CreateInt(reply.snapshot.tab_id), V8_PROPERTY_ATTRIBUTE_NONE);
            if (method == WindowMethod::CreatePanel)
                created->SetValue("panel_id", CefV8Value::CreateString(reply.snapshot.panel_id), V8_PROPERTY_ATTRIBUTE_NONE);
            else created->SetValue("existing", CefV8Value::CreateBool(reply.existing), V8_PROPERTY_ATTRIBUTE_NONE);
            promise->ResolvePromise(created);
        } else if (method == WindowMethod::SuspendCameraViews || method == WindowMethod::CloseCameraView) {
            auto value = CefV8Value::CreateObject(nullptr, nullptr);
            value->SetValue("closed", method == WindowMethod::SuspendCameraViews ?
                CefV8Value::CreateInt(reply.closed_count) : CefV8Value::CreateBool(reply.accepted), V8_PROPERTY_ATTRIBUTE_NONE);
            promise->ResolvePromise(value);
        } else promise->ResolvePromise(CefV8Value::CreateBool(reply.accepted));
        context_->Exit();
        if (reply.close_self_tab_id >= 0) {
            BrowserManager::instance().enqueue_main_thread_task([tab_id = reply.close_self_tab_id] {
                if (auto* tab = BrowserManager::instance().get_tab(tab_id)) tab->open = false;
            });
        }
    }

    std::atomic_bool active_{true};
    CefRefPtr<CefV8Context> context_;
    const CefRefPtr<CefTaskRunner> runner_;
    const int browser_id_;
    const std::string frame_id_;
    inline static std::atomic<std::uint64_t> context_sequence_{0};
    const std::uint64_t context_id_ = ++context_sequence_;
    std::uint64_t sequence_ = 0;
    std::string scene_archive_session_;
    struct Pending { std::variant<WindowMethod, SceneMethod, ViewportMethod, ScriptInputMethod, GizmoMethod, EditorApiMethod> method; CefRefPtr<CefV8Value> promise; };
    std::unordered_map<std::uint64_t, Pending> pending_;
    std::map<std::uint64_t, Subscriber> subscribers_;
};

namespace {

std::mutex endpoints_mutex;
std::vector<std::weak_ptr<CoronaV8Endpoint>> endpoints;

void publish_window_event(const WindowEventData& data, std::optional<int> browser_id = std::nullopt) {
    std::vector<std::shared_ptr<CoronaV8Endpoint>> recipients;
    {
        std::lock_guard lock(endpoints_mutex);
        std::erase_if(endpoints, [](const auto& weak) { return weak.expired(); });
        for (const auto& weak : endpoints) {
            if (auto endpoint = weak.lock(); endpoint && endpoint->active() &&
                (!browser_id || endpoint->browser_id() == *browser_id)) recipients.push_back(std::move(endpoint));
        }
    }
    for (const auto& endpoint : recipients) endpoint->post_event(data);
}

class WindowEventHandler final : public CefV8Handler {
public:
    WindowEventHandler(std::weak_ptr<CoronaV8Endpoint> endpoint, WindowEvent event, std::uint64_t token = 0)
        : endpoint_(std::move(endpoint)), event_(event), token_(token) {}

    bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& arguments,
                 CefRefPtr<CefV8Value>& result, CefString& exception) override {
        result = CefV8Value::CreateUndefined();
        const auto endpoint = endpoint_.lock();
        if (token_) {
            if (endpoint) endpoint->unsubscribe(token_, exception);
        } else if (!endpoint) {
            exception = "CONTEXT_CHANGED";
        } else if (const auto token = endpoint->subscribe(event_, arguments, exception)) {
            result = CefV8Value::CreateFunction("unsubscribe", new WindowEventHandler(endpoint, event_, token));
        }
        return true;
    }

private:
    std::weak_ptr<CoronaV8Endpoint> endpoint_;
    WindowEvent event_;
    std::uint64_t token_;
    IMPLEMENT_REFCOUNTING(WindowEventHandler);
};

template <typename Method>
class NativeHandler final : public CefV8Handler {
public:
    NativeHandler(std::weak_ptr<CoronaV8Endpoint> endpoint, Method method)
        : endpoint_(std::move(endpoint)), method_(method) {}

    bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& arguments,
                 CefRefPtr<CefV8Value>& result, CefString& exception) override {
        const auto endpoint = endpoint_.lock();
        if (!endpoint) { exception = "CONTEXT_CHANGED"; return true; }
        result = endpoint->invoke(method_, arguments);
        return true;
    }

private:
    const std::weak_ptr<CoronaV8Endpoint> endpoint_;
    const Method method_;
    IMPLEMENT_REFCOUNTING(NativeHandler);
};

}  // namespace

std::shared_ptr<CoronaV8Endpoint> attach_corona_v8_binding(CefRefPtr<CefV8Context> context) {
    CEF_REQUIRE_RENDERER_THREAD();
    // Only trusted local top-level editor documents get native window authority.
    if (!context->GetFrame()->IsMain() || !context->GetFrame()->GetURL().ToString().starts_with("file://")) return {};
    auto endpoint = std::make_shared<CoronaV8Endpoint>(context);
    {
        std::lock_guard lock(endpoints_mutex);
        std::erase_if(endpoints, [](const auto& weak) { return weak.expired(); });
        endpoints.push_back(endpoint);
    }
    auto root = CefV8Value::CreateObject(nullptr, nullptr);
    auto scene = CefV8Value::CreateObject(nullptr, nullptr);
    scene->SetValue("readActive", CefV8Value::CreateFunction("readActive",
        new NativeHandler(endpoint, SceneMethod::ReadActive)), V8_PROPERTY_ATTRIBUTE_READONLY);
    scene->SetValue("readTransforms", CefV8Value::CreateFunction("readTransforms",
        new NativeHandler(endpoint, SceneMethod::ReadTransforms)), V8_PROPERTY_ATTRIBUTE_READONLY);
    scene->SetValue("selectActor", CefV8Value::CreateFunction("selectActor",
        new NativeHandler(endpoint, SceneMethod::SelectActor)), V8_PROPERTY_ATTRIBUTE_READONLY);
    scene->SetValue("setActorTransform", CefV8Value::CreateFunction("setActorTransform",
        new NativeHandler(endpoint, SceneMethod::SetActorTransform)), V8_PROPERTY_ATTRIBUTE_READONLY);
    scene->SetValue("setActorProperties", CefV8Value::CreateFunction("setActorProperties",
        new NativeHandler(endpoint, SceneMethod::SetActorProperties)), V8_PROPERTY_ATTRIBUTE_READONLY);
    scene->SetValue("getEnvironment", CefV8Value::CreateFunction("getEnvironment",
        new NativeHandler(endpoint, SceneMethod::GetEnvironment)), V8_PROPERTY_ATTRIBUTE_READONLY);
    scene->SetValue("focusActor", CefV8Value::CreateFunction("focusActor",
        new NativeHandler(endpoint, SceneMethod::FocusActor)), V8_PROPERTY_ATTRIBUTE_READONLY);
    scene->SetValue("setCameraProperties", CefV8Value::CreateFunction("setCameraProperties",
        new NativeHandler(endpoint, SceneMethod::SetCameraProperties)), V8_PROPERTY_ATTRIBUTE_READONLY);
    scene->SetValue("captureScreenshot", CefV8Value::CreateFunction("captureScreenshot",
        new NativeHandler(endpoint, SceneMethod::CaptureScreenshot)), V8_PROPERTY_ATTRIBUTE_READONLY);
    scene->SetValue("setEnvironment", CefV8Value::CreateFunction("setEnvironment",
        new NativeHandler(endpoint, SceneMethod::SetEnvironment)), V8_PROPERTY_ATTRIBUTE_READONLY);
    root->SetValue("scene", scene, V8_PROPERTY_ATTRIBUTE_READONLY);
    auto window = CefV8Value::CreateObject(nullptr, nullptr);
    window->SetValue("getCurrent", CefV8Value::CreateFunction("getCurrent",
        new NativeHandler(endpoint, WindowMethod::GetCurrent)), V8_PROPERTY_ATTRIBUTE_READONLY);
    window->SetValue("setDragRegions", CefV8Value::CreateFunction("setDragRegions",
        new NativeHandler(endpoint, WindowMethod::SetDragRegions)), V8_PROPERTY_ATTRIBUTE_READONLY);
    window->SetValue("close", CefV8Value::CreateFunction("close",
        new NativeHandler(endpoint, WindowMethod::Close)), V8_PROPERTY_ATTRIBUTE_READONLY);
    window->SetValue("closeTab", CefV8Value::CreateFunction("closeTab",
        new NativeHandler(endpoint, WindowMethod::CloseTab)), V8_PROPERTY_ATTRIBUTE_READONLY);
    window->SetValue("focusTab", CefV8Value::CreateFunction("focusTab",
        new NativeHandler(endpoint, WindowMethod::FocusTab)), V8_PROPERTY_ATTRIBUTE_READONLY);
    window->SetValue("createPanel", CefV8Value::CreateFunction("createPanel",
        new NativeHandler(endpoint, WindowMethod::CreatePanel)), V8_PROPERTY_ATTRIBUTE_READONLY);
    window->SetValue("createCameraView", CefV8Value::CreateFunction("createCameraView",
        new NativeHandler(endpoint, WindowMethod::CreateCameraView)), V8_PROPERTY_ATTRIBUTE_READONLY);
    window->SetValue("detachPanel", CefV8Value::CreateFunction("detachPanel",
        new NativeHandler(endpoint, WindowMethod::DetachPanel)), V8_PROPERTY_ATTRIBUTE_READONLY);
    window->SetValue("redockPanel", CefV8Value::CreateFunction("redockPanel",
        new NativeHandler(endpoint, WindowMethod::RedockPanel)), V8_PROPERTY_ATTRIBUTE_READONLY);
    window->SetValue("suspendCameraViews", CefV8Value::CreateFunction("suspendCameraViews",
        new NativeHandler(endpoint, WindowMethod::SuspendCameraViews)), V8_PROPERTY_ATTRIBUTE_READONLY);
    window->SetValue("closeCameraView", CefV8Value::CreateFunction("closeCameraView",
        new NativeHandler(endpoint, WindowMethod::CloseCameraView)), V8_PROPERTY_ATTRIBUTE_READONLY);
    window->SetValue("toggleCameraMaximized", CefV8Value::CreateFunction("toggleCameraMaximized",
        new NativeHandler(endpoint, WindowMethod::ToggleCameraMaximized)), V8_PROPERTY_ATTRIBUTE_READONLY);
    window->SetValue("cycleCameraWindowMode", CefV8Value::CreateFunction("cycleCameraWindowMode",
        new NativeHandler(endpoint, WindowMethod::CycleCameraWindowMode)), V8_PROPERTY_ATTRIBUTE_READONLY);
    window->SetValue("toggleCameraBorderless", CefV8Value::CreateFunction("toggleCameraBorderless",
        new NativeHandler(endpoint, WindowMethod::ToggleCameraBorderless)), V8_PROPERTY_ATTRIBUTE_READONLY);
    window->SetValue("resizeCameraView", CefV8Value::CreateFunction("resizeCameraView",
        new NativeHandler(endpoint, WindowMethod::ResizeCameraView)), V8_PROPERTY_ATTRIBUTE_READONLY);
    window->SetValue("toggleMainBorderless", CefV8Value::CreateFunction("toggleMainBorderless",
        new NativeHandler(endpoint, WindowMethod::ToggleMainBorderless)), V8_PROPERTY_ATTRIBUTE_READONLY);
    window->SetValue("onClosed", CefV8Value::CreateFunction("onClosed",
        new WindowEventHandler(endpoint, WindowEvent::Closed)), V8_PROPERTY_ATTRIBUTE_READONLY);
    window->SetValue("onCloseRequested", CefV8Value::CreateFunction("onCloseRequested",
        new WindowEventHandler(endpoint, WindowEvent::CloseRequested)), V8_PROPERTY_ATTRIBUTE_READONLY);
    auto viewport = CefV8Value::CreateObject(nullptr, nullptr);
    auto gizmo = CefV8Value::CreateObject(nullptr, nullptr);
    gizmo->SetValue("setTarget", CefV8Value::CreateFunction("setTarget", new NativeHandler(endpoint, GizmoMethod::SetTarget)), V8_PROPERTY_ATTRIBUTE_READONLY);
    gizmo->SetValue("pointer", CefV8Value::CreateFunction("pointer", new NativeHandler(endpoint, GizmoMethod::Pointer)), V8_PROPERTY_ATTRIBUTE_READONLY);
    auto input = CefV8Value::CreateObject(nullptr, nullptr);
    input->SetValue("dispatch", CefV8Value::CreateFunction("dispatch",
        new NativeHandler(endpoint, ScriptInputMethod::Dispatch)), V8_PROPERTY_ATTRIBUTE_READONLY);
    root->SetValue("input", input, V8_PROPERTY_ATTRIBUTE_READONLY);
    auto editor = CefV8Value::CreateObject(nullptr, nullptr);
    editor->SetValue("invoke", CefV8Value::CreateFunction("invoke",
        new NativeHandler(endpoint, EditorApiMethod::Invoke)), V8_PROPERTY_ATTRIBUTE_READONLY);
    editor->SetValue("on", CefV8Value::CreateFunction("on",
        new NativeHandler(endpoint, EditorApiMethod::On)), V8_PROPERTY_ATTRIBUTE_READONLY);
    editor->SetValue("off", CefV8Value::CreateFunction("off",
        new NativeHandler(endpoint, EditorApiMethod::Off)), V8_PROPERTY_ATTRIBUTE_READONLY);
    root->SetValue("editor", editor, V8_PROPERTY_ATTRIBUTE_READONLY);
    viewport->SetValue("pick", CefV8Value::CreateFunction("pick",
        new NativeHandler(endpoint, ViewportMethod::Pick)), V8_PROPERTY_ATTRIBUTE_READONLY);
    viewport->SetValue("configure", CefV8Value::CreateFunction("configure",
        new NativeHandler(endpoint, WindowMethod::ConfigureViewport)), V8_PROPERTY_ATTRIBUTE_READONLY);
    root->SetValue("viewport", viewport, V8_PROPERTY_ATTRIBUTE_READONLY);
    root->SetValue("gizmo", gizmo, V8_PROPERTY_ATTRIBUTE_READONLY);
    root->SetValue("window", window, V8_PROPERTY_ATTRIBUTE_READONLY);
    context->GetGlobal()->SetValue("corona", root, V8_PROPERTY_ATTRIBUTE_READONLY);
    return endpoint;
}

void release_corona_v8_binding(const std::shared_ptr<CoronaV8Endpoint>& endpoint) {
    if (endpoint) endpoint->release();
}

void notify_corona_window_closed(int tab_id, const std::string& panel_id) {
    publish_window_event({WindowEvent::Closed, tab_id, panel_id});
}

void request_corona_window_close(int browser_id) {
    publish_window_event({WindowEvent::CloseRequested}, browser_id);
}

}  // namespace Corona::Systems::UI
