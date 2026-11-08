#pragma once

#include "cef_editor_native_api_registry.h"
#include "editor_api_caller.h"

#include <Python.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Corona::Systems::UI {

enum class EditorApiValueType : std::uint32_t {
    Any,
    Null,
    Boolean,
    Integer,
    Number,
    String,
    Object,
    Array,
};

struct EditorApiParamSpec {
    const char* name;
    EditorApiValueType type;
    bool optional;
};

struct EditorApiReturnSpec {
    EditorApiValueType type;
};

struct EditorApiMethodSpec {
    const char* api_name;
    const char* native_module;
    const char* native_function;
    const EditorApiParamSpec* params;
    std::size_t param_count;
    EditorApiReturnSpec return_spec;
    const char* js_wrapper;
    const char* python_wrapper;
    bool async;
    std::uint32_t allowed_callers;
};

struct EditorApiEventSpec {
    const char* event_name;
    EditorApiValueType payload_type;
    std::uint32_t allowed_callers;
    const char* js_wrapper;
    const char* python_wrapper;
};

struct EditorApiRequest {
    std::string api_name;
    nlohmann::json args = nlohmann::json::array();
    // The retired Web caller bit keeps an omitted caller fail-closed.
    EditorApiCaller caller = EditorApiCaller::Cef;
};

class EditorApiRegistry {
public:
    static EditorApiRegistry& instance();

    const EditorApiMethodSpec* find(std::string_view api_name) const;
    std::vector<EditorApiMethodSpec> list_methods() const;
    std::vector<EditorApiEventSpec> list_events() const;
    NativeResult invoke(const EditorApiRequest& request, const NativeContext& context) const;

private:
    EditorApiRegistry() = default;
};

class EditorApiCallbackRegistry {
public:
    static EditorApiCallbackRegistry& instance();

    std::uint64_t register_python_script_callback(const std::string& event_name,
                                                  const nlohmann::json& callback_spec,
                                                  const NativeContext& context);
    std::uint64_t register_python_script_callback_callable(const std::string& event_name,
                                                           PyObject* callback);
    bool unregister(std::uint64_t callback_token);
    void clear_python_script_callbacks();
    std::size_t emit_python_script_event(std::string_view event_name,
                                         const nlohmann::json& payload);

private:
    EditorApiCallbackRegistry() = default;
};

class EditorApiEndpointBase {
public:
    virtual ~EditorApiEndpointBase() = default;

    virtual NativeResult invoke(const std::string& api_name,
                                const nlohmann::json& args,
                                const NativeContext& context) = 0;
};

class PythonScriptApiClientEndpoint final : public EditorApiEndpointBase {
public:
    NativeResult invoke(const std::string& api_name,
                        const nlohmann::json& args,
                        const NativeContext& context) override;
};

class ScriptRuntimeApiClientEndpoint final : public EditorApiEndpointBase {
public:
    NativeResult invoke(const std::string& api_name,
                        const nlohmann::json& args,
                        const NativeContext& context) override;
};

std::optional<EditorApiRequest> parse_editor_api_request(const nlohmann::json& payload,
                                                         EditorApiCaller caller);
std::optional<EditorApiEventSpec> find_editor_api_event(std::string_view event_name);
std::size_t emit_editor_api_event(std::string_view event_name, const nlohmann::json& payload);
std::size_t emit_python_script_event(std::string_view event_name, const nlohmann::json& payload);

void register_python_script_service_dispatcher(PyObject* dispatcher);
void unregister_python_script_service_dispatcher();
bool python_script_service_dispatcher_registered();
std::uint64_t register_python_script_callback_callable(const std::string& event_name,
                                                       PyObject* callback);
void clear_python_script_callbacks();
NativeResult invoke_python_script_service(const NativeRequest& request,
                                          const char* route = "python-script",
                                          std::chrono::milliseconds timeout = std::chrono::seconds(2));

}  // namespace Corona::Systems::UI
