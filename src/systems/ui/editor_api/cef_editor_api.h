#pragma once

#include "cef_editor_native_api_registry.h"

#include <include/cef_frame.h>

#include <Python.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Corona::Systems::UI {

enum class EditorApiCaller : std::uint32_t {
    Cef = 1u << 0u,
    PythonScript = 1u << 1u,
    ScriptRuntime = 1u << 2u,
};

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

    std::uint64_t register_cef_callback(const std::string& event_name,
                                        const nlohmann::json& callback_spec,
                                        const NativeContext& context);
    std::uint64_t register_python_script_callback(const std::string& event_name,
                                                  const nlohmann::json& callback_spec,
                                                  const NativeContext& context);
    std::uint64_t register_python_script_callback_callable(const std::string& event_name,
                                                           PyObject* callback);
    bool unregister(std::uint64_t callback_token);
    void clear_cef_callbacks_for_browser(int browser_id);
    void clear_python_script_callbacks();
    std::size_t emit_editor_api_event(std::string_view event_name,
                                      const nlohmann::json& payload);
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

class CefEditorApiEndpoint final : public EditorApiEndpointBase {
public:
    NativeResult invoke(const std::string& api_name,
                        const nlohmann::json& args,
                        const NativeContext& context) override;

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
std::size_t emit_editor_api_event_to_frame(std::string_view event_name,
                                            const nlohmann::json& payload,
                                            const CefRefPtr<CefFrame>& frame);
std::size_t emit_python_script_event(std::string_view event_name, const nlohmann::json& payload);

void register_python_script_service_dispatcher(PyObject* dispatcher);
void unregister_python_script_service_dispatcher();
bool python_script_service_dispatcher_registered();
std::uint64_t register_python_script_callback_callable(const std::string& event_name,
                                                       PyObject* callback);
void clear_python_script_callbacks();
NativeResult invoke_python_script_service(const NativeRequest& request,
                                          const char* route = "python-script");
bool enqueue_python_project_context_changed(std::string_view project_path);

}  // namespace Corona::Systems::UI
