/**
 * @brief CEF 子进程可执行文件
 *
 * 这是一个轻量级的可执行文件，专门用于 CEF 子进程（Browser, Renderer, GPU 等）。
 * 使用单独的可执行文件可以避免主引擎的静态初始化代码在子进程中执行。
 */

#include <cef_app.h>
#include <include/cef_v8.h>
#include <wrapper/cef_helpers.h>
#include <wrapper/cef_message_router.h>

#include <iostream>

#ifdef _WIN32
#include <windows.h>
#endif

// 与主程序使用相同的消息路由配置
static CefMessageRouterConfig GetMessageRouterConfig() {
    CefMessageRouterConfig config;
    config.js_query_function = "cefQuery";
    config.js_cancel_function = "cefQueryCancel";
    return config;
}

// 渲染进程处理器 - 负责在渲染进程中注入 cefQuery 函数
class SubprocessRenderHandler : public CefRenderProcessHandler {
   public:
    SubprocessRenderHandler() : renderer_side_router_(nullptr) {}

    class FastCameraMoveHandler : public CefV8Handler {
       public:
        bool Execute(const CefString& name,
                     CefRefPtr<CefV8Value> object,
                     const CefV8ValueList& arguments,
                     CefRefPtr<CefV8Value>& retval,
                     CefString& exception) override {
            if (name == "actorTransform") {
                if (arguments.size() < 3) {
                    exception = "actorTransform(handle, operation, vector) requires 3 arguments";
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }

                const auto handle_value = arguments[0];
                if (!handle_value || (!handle_value->IsInt() && !handle_value->IsDouble())) {
                    exception = "actorTransform: handle must be a number";
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }

                const auto operation_value = arguments[1];
                if (!operation_value || !operation_value->IsInt()) {
                    exception = "actorTransform: operation must be an integer";
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }

                const auto vector_value = arguments[2];
                if (!vector_value || !vector_value->IsArray() || vector_value->GetArrayLength() != 3) {
                    exception = "actorTransform: vector must be number[3]";
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }

                auto context = CefV8Context::GetCurrentContext();
                if (!context || !context->GetBrowser()) {
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }

                CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create("ActorTransformFast");
                CefRefPtr<CefListValue> args = message->GetArgumentList();
                args->SetDouble(0, handle_value->GetDoubleValue());
                args->SetInt(1, operation_value->GetIntValue());

                auto vector_list = CefListValue::Create();
                for (int i = 0; i < 3; ++i) {
                    auto elem = vector_value->GetValue(i);
                    if (!elem || (!elem->IsInt() && !elem->IsDouble())) {
                        exception = "actorTransform: vector must be number[3]";
                        retval = CefV8Value::CreateBool(false);
                        return true;
                    }
                    if (elem->IsInt()) {
                        vector_list->SetInt(i, elem->GetIntValue());
                    } else {
                        vector_list->SetDouble(i, elem->GetDoubleValue());
                    }
                }

                args->SetList(2, vector_list);
                context->GetFrame()->SendProcessMessage(PID_BROWSER, message);
                retval = CefV8Value::CreateBool(true);
                return true;
            }

            if (name == "setViewportUiMode") {
                if (arguments.size() < 2 ||
                    !arguments[0] || (!arguments[0]->IsInt() && !arguments[0]->IsDouble()) ||
                    !arguments[1] || !arguments[1]->IsString()) {
                    exception = "setViewportUiMode(cameraHandle, mode) requires (number, string)";
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }
                auto ctx = CefV8Context::GetCurrentContext();
                if (!ctx || !ctx->GetBrowser()) {
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }
                CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("ViewportUiMode");
                CefRefPtr<CefListValue> args = msg->GetArgumentList();
                args->SetDouble(0, arguments[0]->GetDoubleValue());
                args->SetString(1, arguments[1]->GetStringValue());
                ctx->GetFrame()->SendProcessMessage(PID_BROWSER, msg);
                retval = CefV8Value::CreateBool(true);
                return true;
            }

            if (name == "setCameraViewport") {
                if (arguments.size() < 7) {
                    exception =
                        "setCameraViewport(cameraHandle, x, y, width, height, renderWidth, renderHeight) requires 7 numbers";
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }
                for (size_t i = 0; i < 7; ++i) {
                    if (!arguments[i] || (!arguments[i]->IsInt() && !arguments[i]->IsDouble())) {
                        exception = "setCameraViewport arguments must be numbers";
                        retval = CefV8Value::CreateBool(false);
                        return true;
                    }
                }
                auto ctx = CefV8Context::GetCurrentContext();
                if (!ctx || !ctx->GetBrowser()) {
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }
                CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("CameraViewportFast");
                CefRefPtr<CefListValue> args = msg->GetArgumentList();
                for (size_t i = 0; i < 7; ++i) {
                    args->SetDouble(static_cast<int>(i), arguments[i]->GetDoubleValue());
                }
                ctx->GetFrame()->SendProcessMessage(PID_BROWSER, msg);
                retval = CefV8Value::CreateBool(true);
                return true;
            }

            if (name == "setViewportGizmoMode") {
                if (arguments.size() < 1 || !arguments[0] || !arguments[0]->IsString()) {
                    exception = "setViewportGizmoMode(mode) requires a string mode";
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }
                auto ctx = CefV8Context::GetCurrentContext();
                if (!ctx || !ctx->GetBrowser()) {
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }
                CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("ViewportGizmoMode");
                msg->GetArgumentList()->SetString(0, arguments[0]->GetStringValue());
                ctx->GetFrame()->SendProcessMessage(PID_BROWSER, msg);
                retval = CefV8Value::CreateBool(true);
                return true;
            }

            if (name == "clearViewportGizmoSelection") {
                auto ctx = CefV8Context::GetCurrentContext();
                if (!ctx || !ctx->GetBrowser()) {
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }
                CefRefPtr<CefProcessMessage> msg =
                    CefProcessMessage::Create("ViewportGizmoClearSelection");
                ctx->GetFrame()->SendProcessMessage(PID_BROWSER, msg);
                retval = CefV8Value::CreateBool(true);
                return true;
            }

            if (name == "setViewportGizmoSelection") {
                if (arguments.size() < 3 ||
                    !arguments[0] || !arguments[0]->IsString() ||
                    !arguments[1] || (!arguments[1]->IsInt() && !arguments[1]->IsDouble()) ||
                    !arguments[2] || (!arguments[2]->IsInt() && !arguments[2]->IsDouble())) {
                    exception = "setViewportGizmoSelection(sceneId, cameraHandle, actorHandle) requires (string, number, number)";
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }
                auto ctx = CefV8Context::GetCurrentContext();
                if (!ctx || !ctx->GetBrowser()) {
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }
                CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("ViewportGizmoSelection");
                CefRefPtr<CefListValue> args = msg->GetArgumentList();
                args->SetString(0, arguments[0]->GetStringValue());
                args->SetDouble(1, arguments[1]->GetDoubleValue());
                args->SetDouble(2, arguments[2]->GetDoubleValue());
                ctx->GetFrame()->SendProcessMessage(PID_BROWSER, msg);
                retval = CefV8Value::CreateBool(true);
                return true;
            }

            if (name == "setViewportUiCalibration") {
                bool args_ok = arguments.size() >= 5;
                for (size_t i = 0; args_ok && i < 5; ++i) {
                    if (!arguments[i] || (!arguments[i]->IsInt() && !arguments[i]->IsDouble())) {
                        args_ok = false;
                    }
                }
                if (!args_ok) {
                    exception =
                        "setViewportUiCalibration(cameraHandle, pe, angle, offset, parallaxScale) requires 5 numbers";
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }
                auto ctx = CefV8Context::GetCurrentContext();
                if (!ctx || !ctx->GetBrowser()) {
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }
                CefRefPtr<CefProcessMessage> msg =
                    CefProcessMessage::Create("ViewportUiCalibration");
                CefRefPtr<CefListValue> args = msg->GetArgumentList();
                for (size_t i = 0; i < 5; ++i) {
                    args->SetDouble(static_cast<int>(i), arguments[i]->GetDoubleValue());
                }
                ctx->GetFrame()->SendProcessMessage(PID_BROWSER, msg);
                retval = CefV8Value::CreateBool(true);
                return true;
            }

            if (name == "setViewportSystemCursorHidden") {
                if (arguments.empty() || !arguments[0] || !arguments[0]->IsBool()) {
                    exception = "setViewportSystemCursorHidden(hidden[, custom]) requires (boolean[, boolean])";
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }
                auto ctx = CefV8Context::GetCurrentContext();
                if (!ctx || !ctx->GetBrowser()) {
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }
                const bool hidden = arguments[0]->GetBoolValue();
                bool custom = hidden;
                if (arguments.size() > 1 && arguments[1] && arguments[1]->IsBool()) {
                    custom = arguments[1]->GetBoolValue();
                }
                CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("ViewportSystemCursor");
                CefRefPtr<CefListValue> args = msg->GetArgumentList();
                args->SetBool(0, hidden);
                args->SetBool(1, custom);
                ctx->GetFrame()->SendProcessMessage(PID_BROWSER, msg);
                retval = CefV8Value::CreateBool(true);
                return true;
            }

            if (name == "viewportUiPointer") {
                if (arguments.size() < 4 ||
                    !arguments[0] || (!arguments[0]->IsInt() && !arguments[0]->IsDouble()) ||
                    !arguments[1] || !arguments[1]->IsString() ||
                    !arguments[2] || (!arguments[2]->IsInt() && !arguments[2]->IsDouble()) ||
                    !arguments[3] || (!arguments[3]->IsInt() && !arguments[3]->IsDouble())) {
                    exception = "viewportUiPointer(cameraHandle, type, x, y, buttons?, modifiers?, cursor?) requires (number, string, number, number, ...)";
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }
                auto ctx = CefV8Context::GetCurrentContext();
                if (!ctx || !ctx->GetBrowser()) {
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }
                CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("ViewportUiPointer");
                CefRefPtr<CefListValue> args = msg->GetArgumentList();
                args->SetDouble(0, arguments[0]->GetDoubleValue());
                args->SetString(1, arguments[1]->GetStringValue());
                args->SetDouble(2, arguments[2]->GetDoubleValue());
                args->SetDouble(3, arguments[3]->GetDoubleValue());
                for (size_t i = 4; i < arguments.size() && i < 7; ++i) {
                    const auto& arg = arguments[i];
                    if (!arg) continue;
                    if (arg->IsInt()) args->SetInt(static_cast<int>(i), arg->GetIntValue());
                    else if (arg->IsDouble()) args->SetDouble(static_cast<int>(i), arg->GetDoubleValue());
                    else if (arg->IsString()) args->SetString(static_cast<int>(i), arg->GetStringValue());
                }
                ctx->GetFrame()->SendProcessMessage(PID_BROWSER, msg);
                retval = CefV8Value::CreateBool(true);
                return true;
            }

            // ── setProperty: 属性编辑快速通道 ──
            if (name == "setProperty") {
                // arguments: (actorHandle: number, propertyType: int, value: number)
                // propertyType: 0=Mass, 1=Restitution, 2=Damping, 3=Visible, 4=CollisionEnabled, 5=PhysicsEnabled, 6=LinearLockMask, 7=AngularLockMask
                if (arguments.size() < 3) {
                    exception = "setProperty(handle, propertyType, value) requires 3 arguments";
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }

                const auto prop_handle = arguments[0];
                if (!prop_handle || (!prop_handle->IsInt() && !prop_handle->IsDouble())) {
                    exception = "setProperty: handle must be a number";
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }

                const auto prop_type = arguments[1];
                if (!prop_type || !prop_type->IsInt()) {
                    exception = "setProperty: propertyType must be an integer";
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }

                const auto prop_value = arguments[2];
                if (!prop_value || (!prop_value->IsInt() && !prop_value->IsDouble() && !prop_value->IsBool())) {
                    exception = "setProperty: value must be a number or bool";
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }

                auto prop_ctx = CefV8Context::GetCurrentContext();
                if (!prop_ctx || !prop_ctx->GetBrowser()) {
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }

                CefRefPtr<CefProcessMessage> prop_msg = CefProcessMessage::Create("PropertyFast");
                CefRefPtr<CefListValue> prop_args = prop_msg->GetArgumentList();
                prop_args->SetDouble(0, prop_handle->GetDoubleValue());
                prop_args->SetInt(1, prop_type->GetIntValue());
                if (prop_value->IsBool()) {
                    prop_args->SetDouble(2, prop_value->GetBoolValue() ? 1.0 : 0.0);
                } else if (prop_value->IsInt()) {
                    prop_args->SetDouble(2, static_cast<double>(prop_value->GetIntValue()));
                } else {
                    prop_args->SetDouble(2, prop_value->GetDoubleValue());
                }
                prop_ctx->GetFrame()->SendProcessMessage(PID_BROWSER, prop_msg);
                retval = CefV8Value::CreateBool(true);
                return true;
            }

            // ── injectInput: 积木脚本键盘/鼠标注入快速通道 ──
            if (name == "injectInput") {
                // arguments: (type: int, arg1, arg2, arg3, arg4)
                // type: 0=keyDown(code, modifiers?, displayKey?), 1=keyUp(code, displayKey?),
                //       2=mouseEvent(eventType, button?, x?, y?)
                if (arguments.size() < 1) {
                    exception = "injectInput requires at least 1 argument (type)";
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }
                auto inj_ctx = CefV8Context::GetCurrentContext();
                if (!inj_ctx || !inj_ctx->GetBrowser()) {
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }
                CefRefPtr<CefProcessMessage> inj_msg = CefProcessMessage::Create("InputInject");
                CefRefPtr<CefListValue> inj_args = inj_msg->GetArgumentList();
                for (size_t i = 0; i < arguments.size() && i < 6; ++i) {
                    const auto& arg = arguments[i];
                    if (!arg) continue;
                    if (arg->IsInt()) inj_args->SetInt(i, arg->GetIntValue());
                    else if (arg->IsDouble()) inj_args->SetDouble(i, arg->GetDoubleValue());
                    else if (arg->IsString()) inj_args->SetString(i, arg->GetStringValue());
                    else if (arg->IsBool()) inj_args->SetBool(i, arg->GetBoolValue());
                }
                inj_ctx->GetFrame()->SendProcessMessage(PID_BROWSER, inj_msg);
                retval = CefV8Value::CreateBool(true);
                return true;
            }

            // ── pickActor: 视口拾取快速通道 ──
            if (name == "pickActor") {
                // arguments: (cameraHandle: number, sceneId: string, requestId: string,
                //             x: number, y: number, vpWidth: number, vpHeight: number)
                if (arguments.size() < 7) {
                    exception = "pickActor(cameraHandle, sceneId, requestId, x, y, vpW, vpH) requires 7 arguments";
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }
                auto pick_ctx = CefV8Context::GetCurrentContext();
                if (!pick_ctx || !pick_ctx->GetBrowser()) {
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }
                CefRefPtr<CefProcessMessage> pick_msg = CefProcessMessage::Create("ViewportPick");
                CefRefPtr<CefListValue> pick_args = pick_msg->GetArgumentList();

                const auto set_numeric_arg = [&](int index) -> bool {
                    const auto& arg = arguments[index];
                    if (!arg) return false;
                    if (arg->IsInt()) {
                        pick_args->SetInt(index, arg->GetIntValue());
                        return true;
                    }
                    if (arg->IsDouble()) {
                        pick_args->SetDouble(index, arg->GetDoubleValue());
                        return true;
                    }
                    return false;
                };

                if (!set_numeric_arg(0) ||
                    !arguments[1] || !arguments[1]->IsString() ||
                    !arguments[2] || !arguments[2]->IsString() ||
                    !set_numeric_arg(3) ||
                    !set_numeric_arg(4) ||
                    !set_numeric_arg(5) ||
                    !set_numeric_arg(6)) {
                    exception = "pickActor: expected (number, string, string, number, number, number, number)";
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }
                pick_args->SetString(1, arguments[1]->GetStringValue());
                pick_args->SetString(2, arguments[2]->GetStringValue());
                pick_ctx->GetFrame()->SendProcessMessage(PID_BROWSER, pick_msg);
                retval = CefV8Value::CreateBool(true);
                return true;
            }

            if (name == "dockCommand") {
                if (arguments.size() < 1 || !arguments[0] || !arguments[0]->IsString()) {
                    exception = "dockCommand(jsonString) requires a string argument";
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }

                auto dock_ctx = CefV8Context::GetCurrentContext();
                if (!dock_ctx || !dock_ctx->GetBrowser()) {
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }

                CefRefPtr<CefProcessMessage> dock_msg = CefProcessMessage::Create("DockCommand");
                dock_msg->GetArgumentList()->SetString(0, arguments[0]->GetStringValue());
                dock_ctx->GetFrame()->SendProcessMessage(PID_BROWSER, dock_msg);
                retval = CefV8Value::CreateBool(true);
                return true;
            }

            if (name == "computeActorFocusPose") {
                if (arguments.size() < 2) {
                    exception = "computeActorFocusPose(actorHandle, requestId) requires 2 arguments";
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }

                const auto actor_handle = arguments[0];
                if (!actor_handle || (!actor_handle->IsInt() && !actor_handle->IsDouble())) {
                    exception = "computeActorFocusPose: actorHandle must be a number";
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }

                if (!arguments[1] || !arguments[1]->IsString()) {
                    exception = "computeActorFocusPose: requestId must be a string";
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }

                auto focus_ctx = CefV8Context::GetCurrentContext();
                if (!focus_ctx || !focus_ctx->GetBrowser()) {
                    retval = CefV8Value::CreateBool(false);
                    return true;
                }

                CefRefPtr<CefProcessMessage> focus_msg =
                    CefProcessMessage::Create("ComputeActorFocusPoseFast");
                CefRefPtr<CefListValue> focus_args = focus_msg->GetArgumentList();
                focus_args->SetDouble(0, actor_handle->GetDoubleValue());
                focus_args->SetString(1, arguments[1]->GetStringValue());
                focus_ctx->GetFrame()->SendProcessMessage(PID_BROWSER, focus_msg);
                retval = CefV8Value::CreateBool(true);
                return true;
            }

            if (name != "cameraMove") {
                return false;
            }

            if (arguments.size() < 5) {
                exception = "cameraMove(handle, position, forward, up, fov) requires 5 arguments";
                retval = CefV8Value::CreateBool(false);
                return true;
            }

            const auto handle_value = arguments[0];
            if (!handle_value || (!handle_value->IsInt() && !handle_value->IsDouble())) {
                exception = "cameraMove: handle must be a number";
                retval = CefV8Value::CreateBool(false);
                return true;
            }

            auto read_vec3 = [&](const CefRefPtr<CefV8Value>& value, CefRefPtr<CefListValue> out) -> bool {
                if (!value || !value->IsArray() || value->GetArrayLength() != 3) {
                    return false;
                }
                for (int i = 0; i < 3; ++i) {
                    auto elem = value->GetValue(i);
                    if (!elem || (!elem->IsInt() && !elem->IsDouble())) {
                        return false;
                    }
                    if (elem->IsInt()) {
                        out->SetInt(i, elem->GetIntValue());
                    } else {
                        out->SetDouble(i, elem->GetDoubleValue());
                    }
                }
                return true;
            };

            auto context = CefV8Context::GetCurrentContext();
            if (!context) {
                retval = CefV8Value::CreateBool(false);
                return true;
            }

            auto browser = context->GetBrowser();
            if (!browser) {
                retval = CefV8Value::CreateBool(false);
                return true;
            }

            CefRefPtr<CefProcessMessage> message = CefProcessMessage::Create("CameraMoveFast");
            CefRefPtr<CefListValue> args = message->GetArgumentList();
            args->SetDouble(0, handle_value->GetDoubleValue());

            auto pos_list = CefListValue::Create();
            auto fwd_list = CefListValue::Create();
            auto up_list = CefListValue::Create();

            if (!read_vec3(arguments[1], pos_list) ||
                !read_vec3(arguments[2], fwd_list) ||
                !read_vec3(arguments[3], up_list)) {
                exception = "cameraMove: position/forward/up must be number[3]";
                retval = CefV8Value::CreateBool(false);
                return true;
            }

            args->SetList(1, pos_list);
            args->SetList(2, fwd_list);
            args->SetList(3, up_list);

            if (!arguments[4] || (!arguments[4]->IsInt() && !arguments[4]->IsDouble())) {
                exception = "cameraMove: fov must be a number";
                retval = CefV8Value::CreateBool(false);
                return true;
            }

            if (arguments[4]->IsInt()) {
                args->SetInt(4, arguments[4]->GetIntValue());
            } else {
                args->SetDouble(4, arguments[4]->GetDoubleValue());
            }
            context->GetFrame()->SendProcessMessage(PID_BROWSER, message);
            retval = CefV8Value::CreateBool(true);
            return true;
        }

        IMPLEMENT_REFCOUNTING(FastCameraMoveHandler);
    };

    void OnContextCreated(CefRefPtr<CefBrowser> browser,
                          CefRefPtr<CefFrame> frame,
                          CefRefPtr<CefV8Context> context) override {
        if (!renderer_side_router_) {
            renderer_side_router_ = CefMessageRouterRendererSide::Create(GetMessageRouterConfig());
        }

        // 将 cefQuery 和 cefQueryCancel 函数注入到 window 对象中
        renderer_side_router_->OnContextCreated(browser, frame, context);

        CefRefPtr<CefV8Value> global = context->GetGlobal();
        CefRefPtr<CefV8Value> bridge = CefV8Value::CreateObject(nullptr, nullptr);
        CefRefPtr<CefV8Handler> handler(new FastCameraMoveHandler());
        CefRefPtr<CefV8Value> camera_move = CefV8Value::CreateFunction("cameraMove", handler);
        CefRefPtr<CefV8Value> actor_transform = CefV8Value::CreateFunction("actorTransform", handler);
        CefRefPtr<CefV8Value> set_property = CefV8Value::CreateFunction("setProperty", handler);
        CefRefPtr<CefV8Value> pick_actor = CefV8Value::CreateFunction("pickActor", handler);
        CefRefPtr<CefV8Value> set_viewport_ui_mode =
            CefV8Value::CreateFunction("setViewportUiMode", handler);
        CefRefPtr<CefV8Value> set_camera_viewport =
            CefV8Value::CreateFunction("setCameraViewport", handler);
        CefRefPtr<CefV8Value> set_viewport_gizmo_mode =
            CefV8Value::CreateFunction("setViewportGizmoMode", handler);
        CefRefPtr<CefV8Value> clear_viewport_gizmo_selection =
            CefV8Value::CreateFunction("clearViewportGizmoSelection", handler);
        CefRefPtr<CefV8Value> set_viewport_gizmo_selection =
            CefV8Value::CreateFunction("setViewportGizmoSelection", handler);
        CefRefPtr<CefV8Value> set_viewport_ui_calibration =
            CefV8Value::CreateFunction("setViewportUiCalibration", handler);
        CefRefPtr<CefV8Value> viewport_system_cursor =
            CefV8Value::CreateFunction("setViewportSystemCursorHidden", handler);
        CefRefPtr<CefV8Value> viewport_ui_pointer =
            CefV8Value::CreateFunction("viewportUiPointer", handler);
        CefRefPtr<CefV8Value> inject_input = CefV8Value::CreateFunction("injectInput", handler);
        CefRefPtr<CefV8Value> dock_command = CefV8Value::CreateFunction("dockCommand", handler);
        CefRefPtr<CefV8Value> compute_actor_focus_pose =
            CefV8Value::CreateFunction("computeActorFocusPose", handler);
        bridge->SetValue("cameraMove", camera_move, V8_PROPERTY_ATTRIBUTE_NONE);
        bridge->SetValue("actorTransform", actor_transform, V8_PROPERTY_ATTRIBUTE_NONE);
        bridge->SetValue("setProperty", set_property, V8_PROPERTY_ATTRIBUTE_NONE);
        bridge->SetValue("pickActor", pick_actor, V8_PROPERTY_ATTRIBUTE_NONE);
        bridge->SetValue("setViewportUiMode", set_viewport_ui_mode, V8_PROPERTY_ATTRIBUTE_NONE);
        bridge->SetValue("setCameraViewport", set_camera_viewport, V8_PROPERTY_ATTRIBUTE_NONE);
        bridge->SetValue("setViewportGizmoMode", set_viewport_gizmo_mode, V8_PROPERTY_ATTRIBUTE_NONE);
        bridge->SetValue("clearViewportGizmoSelection",
                         clear_viewport_gizmo_selection,
                         V8_PROPERTY_ATTRIBUTE_NONE);
        bridge->SetValue("setViewportGizmoSelection",
                         set_viewport_gizmo_selection,
                         V8_PROPERTY_ATTRIBUTE_NONE);
        bridge->SetValue("setViewportUiCalibration", set_viewport_ui_calibration, V8_PROPERTY_ATTRIBUTE_NONE);
        bridge->SetValue("setViewportSystemCursorHidden", viewport_system_cursor, V8_PROPERTY_ATTRIBUTE_NONE);
        bridge->SetValue("viewportUiPointer", viewport_ui_pointer, V8_PROPERTY_ATTRIBUTE_NONE);
        bridge->SetValue("injectInput", inject_input, V8_PROPERTY_ATTRIBUTE_NONE);
        bridge->SetValue("dockCommand", dock_command, V8_PROPERTY_ATTRIBUTE_NONE);
        bridge->SetValue("computeActorFocusPose", compute_actor_focus_pose, V8_PROPERTY_ATTRIBUTE_NONE);
        global->SetValue("coronaBridge", bridge, V8_PROPERTY_ATTRIBUTE_NONE);

        std::cout << "[Renderer] V8 context created, cefQuery injected" << std::endl;
    }

    void OnContextReleased(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefV8Context> context) override {
        if (renderer_side_router_) {
            renderer_side_router_->OnContextReleased(browser, frame, context);
        }
    }

    bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                  CefRefPtr<CefFrame> frame,
                                  CefProcessId source_process,
                                  CefRefPtr<CefProcessMessage> message) override {
        if (renderer_side_router_) {
            return renderer_side_router_->OnProcessMessageReceived(
                browser, frame, source_process, message);
        }
        return false;
    }

   private:
    CefRefPtr<CefMessageRouterRendererSide> renderer_side_router_;
    IMPLEMENT_REFCOUNTING(SubprocessRenderHandler);
};

// CefApp 实现，用于子进程
class SubprocessApp : public CefApp {
   public:
    SubprocessApp() : render_handler_(new SubprocessRenderHandler()) {}

    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
        return render_handler_;
    }

   private:
    CefRefPtr<SubprocessRenderHandler> render_handler_;
    IMPLEMENT_REFCOUNTING(SubprocessApp);
};

#ifdef _WIN32
int APIENTRY wWinMain(HINSTANCE hInstance,
                      HINSTANCE hPrevInstance,
                      LPWSTR lpCmdLine,
                      int nCmdShow) {
    CefMainArgs main_args(hInstance);
#else
int main(int argc, char* argv[]) {
    CefMainArgs main_args(argc, argv);
#endif

    CefRefPtr<SubprocessApp> app(new SubprocessApp());

    // 执行 CEF 子进程逻辑
    // 这将阻塞直到子进程结束
    return CefExecuteProcess(main_args, app.get(), nullptr);
}
