import pathlib
import re
import unittest


class NativeSceneToolsRpcTests(unittest.TestCase):
    def _repo_root(self):
        return pathlib.Path(__file__).resolve().parents[4]

    def _handler_source(self):
        repo_root = self._repo_root()
        handler_path = repo_root / "src" / "systems" / "ui" / "cef" / "cef_editor_native_api_handlers.cpp"
        return handler_path.read_text(encoding="utf-8")

    def _editor_api_header(self):
        api_path = (
            self._repo_root()
            / "src"
            / "systems"
            / "ui"
            / "editor_api"
            / "cef_editor_api.h"
        )
        return api_path.read_text(encoding="utf-8")

    def _editor_api_source(self):
        api_path = (
            self._repo_root()
            / "src"
            / "systems"
            / "ui"
            / "editor_api"
            / "cef_editor_api.cpp"
        )
        return api_path.read_text(encoding="utf-8")

    def _native_api_header(self):
        return (
            self._repo_root()
            / "src"
            / "systems"
            / "ui"
            / "cef"
            / "cef_editor_native_api_registry.h"
        ).read_text(encoding="utf-8")

    def _query_bridge_source(self):
        return (
            self._repo_root()
            / "src"
            / "systems"
            / "ui"
            / "cef"
            / "cef_query_bridge.cpp"
        ).read_text(encoding="utf-8")

    def _frontend_bridge_source(self):
        frontend_root = self._repo_root() / "editor" / "Frontend" / "src"
        source_paths = [
            frontend_root / "utils" / "bridge.js",
            frontend_root / "api" / "editorApi.js",
        ]
        for directory, names in {
            "compat": {
                "appService.js",
                "fileService.js",
                "logService.js",
                "projectService.js",
                "projectSettingsService.js",
                "sceneService.js",
                "scriptingService.js",
            },
            "services": {
                "aiService.js",
                "fileService.js",
                "lanChatService.js",
                "logService.js",
                "networkService.js",
                "projectLauncherService.js",
                "projectSettingsService.js",
                "resourceService.js",
                "sceneService.js",
                "scriptingService.js",
            },
        }.items():
            source_paths.extend(frontend_root / directory / name for name in sorted(names))
        sources = [path.read_text(encoding="utf-8") for path in source_paths]
        # These tests verify delegation and ownership. Keep them stable across
        # harmless formatting changes and private unwrap-helper renames.
        variants = []
        for source in sources:
            variants.extend((source, source.replace("=> ", "=>\n    "), source.replace("unwrap", "_unwrap")))
        return "\n".join(variants)

    def _network_system_source(self):
        return (
            self._repo_root()
            / "src"
            / "systems"
            / "network"
            / "network_system.cpp"
        ).read_text(encoding="utf-8")

    def _frontend_rpc_calls(self):
        bridge_source = self._frontend_bridge_source()
        return set(
            re.findall(
                r"Bridge\.callCEF\(\s*['\"]([^'\"]+)['\"]\s*,\s*['\"]([^'\"]+)['\"]",
                bridge_source,
                re.S,
            )
        )

    def _native_rpc_methods(self):
        source = self._handler_source()
        module_map = {
            "project_launcher": "ProjectLauncher",
            "main_view": "MainView",
            "project_settings": "ProjectSettings",
            "scene_datas": "SceneDatas",
            "scene_tools": "SceneTools",
            "network": "Network",
            "lanchat": "LANChat",
        }
        methods = set()
        for register_name, module_name in module_map.items():
            match = re.search(
                rf"void register_{register_name}_api_handlers\(NativeApiRegistry& registry\) "
                rf"\{{(.*?)registry\.register_module\(\"{re.escape(module_name)}\"",
                source,
                re.S,
            )
            self.assertIsNotNone(match, module_name)
            for method in re.findall(r'\{"([A-Za-z0-9_]+)",\s*\[', match.group(1)):
                methods.add((module_name, method))
        return methods

    def _editor_api_methods(self):
        source = self._editor_api_source()
        return set(
            re.findall(
                r"EDITOR_API_METHOD(?:[A-Z0-9_]*)?\(([A-Za-z0-9_]+),\s*([A-Za-z0-9_]+)",
                source,
            )
        )

    def _editor_api_events(self):
        source = self._editor_api_source()
        return set(
            re.findall(
                r'\{"([A-Za-z0-9_]+\.[A-Za-z0-9_]+)",\s*EditorApiValueType::',
                source,
            )
        )

    def _frontend_editor_api_calls(self):
        bridge_source = self._frontend_bridge_source()
        return set(
            re.findall(
                r"editorApi\.invoke\(\s*['\"]([^'\"]+)['\"]",
                bridge_source,
                re.S,
            )
        )

    def _frontend_call_editor_api_calls(self):
        bridge_source = self._frontend_bridge_source()
        return set(
            re.findall(
                r"call_editor_api\(\s*['\"]([^'\"]+)['\"]",
                bridge_source,
                re.S,
            )
        )

    def test_rpc_contract_contains_all_frontend_calls(self):
        frontend_calls = self._frontend_rpc_calls()
        self.assertEqual(frontend_calls, set())

    def test_native_rpc_methods_are_all_in_contract(self):
        native_methods = self._native_rpc_methods()
        api_methods = self._editor_api_methods()
        missing = sorted(native_methods - api_methods)
        self.assertEqual(missing, [])

    def test_camera_lock_is_owned_by_scene_tools_aggregate(self):
        manifest = self._editor_api_source()
        handler = self._handler_source()
        frontend = self._frontend_bridge_source()

        self.assertIn(
            'set_actor_camera_lock, kSceneActorCameraLockParams, "sceneTools.setActorCameraLock"',
            manifest,
        )
        self.assertIn('{"set_actor_camera_lock",', handler)
        self.assertIn("camera_lock_enabled", handler)
        self.assertIn("CameraFollowController::instance().set_target", handler)
        self.assertIn("setActorCameraLock: (sceneName, actorName, cameraLock)", frontend)
        self.assertNotIn("setCameraLock: (sceneName, actorName, enabled)", frontend)

    def test_public_editor_api_names_do_not_use_rpc_terms(self):
        api_methods = self._editor_api_methods()
        public_rpc_names = sorted(
            f"{module}.{method}"
            for module, method in api_methods
            if "rpc" in f"{module}.{method}".lower()
        )
        self.assertEqual(public_rpc_names, [])

    def test_python_editor_api_wrapper_does_not_expose_raw_invoke(self):
        python_api_source = (
            self._repo_root() / "editor" / "api" / "editor_api.py"
        ).read_text(encoding="utf-8")
        self.assertIn("def _invoke_cpp_api(", python_api_source)
        self.assertNotIn("def invoke_cpp_api(", python_api_source)
        self.assertNotIn("invoke = staticmethod", python_api_source)

    def test_frontend_editor_api_wrapper_does_not_expose_raw_invoke(self):
        bridge_source = self._frontend_bridge_source()
        self.assertNotIn("invoke: (apiName", bridge_source)
        business_sources = [
            path
            for path in (self._repo_root() / "editor" / "Frontend" / "src").rglob("*.js")
            if path.name != "bridge.js"
        ]
        naked_invokes = []
        for path in business_sources:
            source = path.read_text(encoding="utf-8")
            if "editorApi.invoke(" in source:
                naked_invokes.append(str(path.relative_to(self._repo_root())))
        self.assertEqual(naked_invokes, [])

    def test_wrappers_validate_methods_against_cpp_manifest_before_invocation(self):
        bridge_source = self._frontend_bridge_source()
        python_api_source = (
            self._repo_root() / "editor" / "api" / "editor_api.py"
        ).read_text(encoding="utf-8")

        self.assertIn("ensureEditorApiMethod", bridge_source)
        self.assertIn("call_editor_api('EditorApi.list_methods', [])", bridge_source)
        self.assertIn("await Bridge.ensureEditorApiMethod(apiName)", bridge_source)
        self.assertIn("Editor API method is not defined by C++ manifest", bridge_source)

        self.assertIn("def _ensure_cpp_api_method(api_name):", python_api_source)
        self.assertIn('_invoke_cpp_api("EditorApi.list_methods", [], validate_method=False)', python_api_source)
        self.assertIn("_ensure_cpp_api_method(api_name)", python_api_source)
        self.assertIn("Editor API method is not defined by C++ manifest", python_api_source)
        self.assertNotIn("def _ensure_cpp_api_wrapper_method(api_name):", python_api_source)
        self.assertNotIn("Python wrapper method is not declared", python_api_source)
        self.assertIn("if validate_method:", python_api_source)
        invoke_body = python_api_source[
            python_api_source.find("def _invoke_cpp_api("):python_api_source.find("class _ProjectApi", python_api_source.find("def _invoke_cpp_api("))
        ]
        self.assertNotIn("_ensure_cpp_api_wrapper_method(api_name)", invoke_body)
        self.assertLess(invoke_body.find("spec = _validate_cpp_api_args(api_name, normalized_args)"),
                        invoke_body.find("import CoronaEngine"))

    def test_callback_wrappers_validate_events_against_cpp_manifest_before_registration(self):
        bridge_source = self._frontend_bridge_source()
        python_api_source = (
            self._repo_root() / "editor" / "api" / "editor_api.py"
        ).read_text(encoding="utf-8")

        self.assertIn("ensureEditorApiEvent", bridge_source)
        self.assertIn("call_editor_api('EditorApi.list_events', [])", bridge_source)
        self.assertIn("await Bridge.ensureEditorApiEvent(eventName)", bridge_source)
        self.assertIn("Editor API event is not defined by C++ manifest", bridge_source)

        self.assertIn("def _ensure_cpp_api_event(event_name):", python_api_source)
        self.assertIn("def _ensure_cpp_api_events():", python_api_source)
        self.assertIn('_invoke_cpp_api("EditorApi.list_events", [], validate_method=False)', python_api_source)
        self.assertIn("_ensure_cpp_api_event(event_name)", python_api_source)
        self.assertIn("for event_spec in _ensure_cpp_api_events().values():", python_api_source)
        self.assertIn("Editor API event is not defined by C++ manifest", python_api_source)

    def test_frontend_event_wrapper_paths_are_validated_against_cpp_manifest(self):
        bridge_source = self._frontend_bridge_source()
        for snippet in (
            "static validateEditorApiEventWrapperMethods()",
            "for (const spec of editorApiEventSpecs.values())",
            "const wrapperPath = spec?.js_wrapper;",
            "Bridge.resolveEditorApiWrapperPath(wrapperPath)",
            "Frontend event wrapper path is not implemented",
            "Bridge.validateEditorApiEventWrapperMethods();",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)

    def test_callback_wrappers_validate_event_payloads_against_cpp_manifest_schema(self):
        bridge_source = self._frontend_bridge_source()
        python_api_source = (
            self._repo_root() / "editor" / "api" / "editor_api.py"
        ).read_text(encoding="utf-8")

        self.assertIn("validateEditorApiEventPayload(eventName, payload, eventSpec)", bridge_source)
        self.assertIn("Bridge.validateEditorApiEventPayload(envelope?.event, envelope?.payload, entry.eventSpec)", bridge_source)
        self.assertIn("Editor API event payload schema mismatch", bridge_source)
        self.assertIn("editorApiCallbacks.set(callbackToken, { callback, eventName, eventSpec })", bridge_source)

        self.assertIn("def _validate_cpp_api_event_payload(event_name, payload, event_spec):", python_api_source)
        self.assertIn("_validate_cpp_api_event_payload(event_name, payload, event_spec)", python_api_source)
        self.assertIn("Editor API event payload schema mismatch", python_api_source)
        self.assertNotIn("payload if isinstance(payload, dict) else {}", python_api_source)

    def test_wrappers_validate_allowed_callers_against_cpp_manifest(self):
        bridge_source = self._frontend_bridge_source()
        api_header = self._editor_api_header()
        api_source = self._editor_api_source()
        python_api_source = (
            self._repo_root() / "editor" / "api" / "editor_api.py"
        ).read_text(encoding="utf-8")

        self.assertIn("const EDITOR_API_CALLER_CEF = 1", bridge_source)
        self.assertIn("validateEditorApiCaller(apiName, spec, EDITOR_API_CALLER_CEF, 'CEF')", bridge_source)
        self.assertIn("Bridge.validateEditorApiCaller(eventName, eventSpec, EDITOR_API_CALLER_CEF, 'CEF')", bridge_source)
        self.assertIn("Editor API caller is not allowed by C++ manifest", bridge_source)

        self.assertIn("PythonScript = 1u << 1u", api_header)
        self.assertNotIn("EditorApiCaller::Python)", api_source)
        self.assertNotIn("EditorApiCaller::Python,", api_source)
        self.assertIn("_CPP_EDITOR_API_CALLER_PYTHON_SCRIPT = 2", python_api_source)
        self.assertIn("def _validate_cpp_api_caller(name, spec, caller_mask, caller_name):", python_api_source)
        self.assertIn("_validate_cpp_api_caller(api_name, spec, caller_mask, caller_name)", python_api_source)
        self.assertIn("_validate_cpp_api_caller(event_name, event_spec, _CPP_EDITOR_API_CALLER_PYTHON_SCRIPT, \"PythonScript\")", python_api_source)
        self.assertIn("Editor API caller is not allowed by C++ manifest", python_api_source)

    def test_wrappers_validate_arguments_against_cpp_manifest_schema_before_invocation(self):
        bridge_source = self._frontend_bridge_source()
        python_api_source = (
            self._repo_root() / "editor" / "api" / "editor_api.py"
        ).read_text(encoding="utf-8")

        self.assertIn("validateEditorApiArgs(apiName, args)", bridge_source)
        self.assertIn("const spec = await Bridge.ensureEditorApiMethod(apiName)", bridge_source)
        self.assertIn("param.optional", bridge_source)
        self.assertIn("Editor API argument schema mismatch", bridge_source)

        self.assertIn("def _validate_cpp_api_args(\n    api_name,\n    args,", python_api_source)
        self.assertIn("spec = _ensure_cpp_api_method(api_name)", python_api_source)
        self.assertIn("param.get(\"optional\")", python_api_source)
        self.assertIn("Editor API argument schema mismatch", python_api_source)

    def test_wrappers_validate_returns_against_cpp_manifest_schema_after_invocation(self):
        bridge_source = self._frontend_bridge_source()
        python_api_source = (
            self._repo_root() / "editor" / "api" / "editor_api.py"
        ).read_text(encoding="utf-8")

        self.assertIn("validateEditorApiReturn(apiName, data, spec)", bridge_source)
        self.assertIn("Bridge.validateEditorApiReturn(apiName, jsonResponse?.data, spec)", bridge_source)
        self.assertIn("Editor API return schema mismatch", bridge_source)

        self.assertIn("def _validate_cpp_api_return(api_name, data, spec):", python_api_source)
        self.assertIn("_validate_cpp_api_return(api_name, data, spec)", python_api_source)
        self.assertIn("Editor API return schema mismatch", python_api_source)

    def test_frontend_wrapper_rejects_schema_validation_errors(self):
        bridge_source = self._frontend_bridge_source()

        self.assertIn("Bridge.validateEditorApiReturn(apiName, jsonResponse?.data, spec)", bridge_source)
        self.assertIn("reject(e);", bridge_source)
        self.assertNotIn("resolve(response);", bridge_source)

    def test_frontend_callback_unregister_keeps_local_token_until_cpp_confirms(self):
        bridge_source = self._frontend_bridge_source()

        self.assertIn("return call_editor_api('EditorApi.unregister_callback', [callbackToken])", bridge_source)
        self.assertIn(".then((response) => {", bridge_source)
        self.assertIn("editorApiCallbacks.delete(callbackToken);", bridge_source)
        self.assertNotIn(
            "const unregister_editor_api_callback = async (callbackToken) => {\n  editorApiCallbacks.delete(callbackToken);",
            bridge_source,
        )

    def test_editor_api_has_no_python_backend(self):
        api_source = self._editor_api_source()
        api_header = self._editor_api_header()

        native_rpc_source = (
            self._repo_root() / "src" / "systems" / "ui" / "cef" / "cef_editor_native_api_registry.cpp"
        ).read_text(encoding="utf-8")
        editor_source = (
            self._repo_root() / "editor" / "runtime" / "editor_host.py"
        ).read_text(encoding="utf-8")
        bind_source = (
            self._repo_root() / "src" / "systems" / "ui" / "cef" / "cef_py_bind.cpp"
        ).read_text(encoding="utf-8")
        for text in (api_source, api_header, bind_source, editor_source):
            with self.subTest(source=text[:24]):
                self.assertNotIn("EditorApiBackend::Python", text)
                self.assertNotIn("register_editor_api_python_dispatcher", text)
                self.assertNotIn("unregister_editor_api_python_dispatcher", text)
                self.assertNotIn("dispatch_editor_api_from_cpp", text)
                self.assertNotIn("Python backend", text)
        self.assertNotIn("is_python_fallback_allowed", native_rpc_source)
        self.assertNotIn("_PYTHON_ROUTE_METHOD_ALLOWLIST", editor_source)
        self.assertNotIn("deal_func_from_js", editor_source)
        self.assertIn("register_python_script_service_dispatcher", bind_source)
        self.assertNotIn("register_python_script_dispatcher", bind_source)
        self.assertIn("dispatch_script_request_from_cpp", editor_source)

    def test_unknown_rpc_is_rejected_before_python(self):
        query_source = self._query_bridge_source()
        self.assertIn("EditorApiRegistry::instance().find", query_source)
        self.assertIn("unsupported_editor_api_route_json", query_source)
        self.assertNotIn("find_editor_rpc_spec", query_source)
        self.assertNotIn("parse_native_request", query_source)
        self.assertNotIn("native_unhandled", query_source)

    def test_cef_query_bridge_has_no_python_fallback(self):
        query_source = self._query_bridge_source()
        self.assertNotIn("deal_func_from_js", query_source)
        self.assertNotIn("is_python_fallback_allowed", query_source)
        self.assertNotIn("CEF RPC python fallback", query_source)
        self.assertNotIn("CefEditorRpcInterface", query_source)
        self.assertNotIn("PythonEditorRpcInterface", query_source)
        self.assertNotIn("PyImport_Import", query_source)
        self.assertNotIn("dispatch_from_cpp", query_source)
        self.assertNotIn("initialize_python", query_source)
        self.assertNotIn("pFunc_", query_source)
        self.assertNotIn("EditorApiBackend::Python", query_source)

    def test_editor_api_core_declares_cpp_defined_endpoint_contract(self):
        header = self._editor_api_header()
        source = self._editor_api_source()
        cmake = (
            self._repo_root() / "src" / "systems" / "ui" / "CMakeLists.txt"
        ).read_text(encoding="utf-8")

        for symbol in (
            "EditorApiRegistry",
            "EditorApiMethodSpec",
            "EditorApiEndpointBase",
            "CefEditorApiEndpoint",
            "PythonScriptApiClientEndpoint",
            "register_python_script_service_dispatcher",
            "unregister_python_script_service_dispatcher",
        ):
            with self.subTest(symbol=symbol):
                self.assertIn(symbol, header)

        self.assertNotIn("CefEditorApiEndpoint(PyObject*", header)
        self.assertNotIn("PythonEditorApiEndpoint", header)
        self.assertNotIn("PythonEditorApiEndpoint", source)
        self.assertNotIn("PythonScriptEditorApiEndpoint", header)
        self.assertNotIn("PythonScriptEditorApiEndpoint", source)
        self.assertNotIn("virtual std::uint64_t register_callback", header)
        self.assertNotIn("virtual bool unregister_callback", header)
        self.assertNotIn("CefEditorApiEndpoint::register_callback", source)
        self.assertNotIn("CefEditorApiEndpoint::unregister_callback", source)
        self.assertNotIn("PythonScriptApiClientEndpoint::register_callback", source)
        self.assertNotIn("PythonScriptApiClientEndpoint::unregister_callback", source)
        self.assertIn("python_script_service_dispatcher_registered()", source)
        self.assertIn("EDITOR_API_METHOD0_WRAPPED(ProjectLauncher, get_app_version", source)
        self.assertIn("EDITOR_API_METHOD1_WRAPPED(SceneTools, list_actor_tree", source)
        self.assertIn(
            "EDITOR_API_METHOD_SCHEMA_WRAPPED(MainView, scene_save, kSceneSaveParams",
            source,
        )
        self.assertIn("editor_api/cef_editor_api.cpp", cmake)
        self.assertNotIn("cef/cef_editor_rpc_contract.cpp", cmake)

    def test_editor_api_registry_covers_all_native_handlers(self):
        api_methods = self._editor_api_methods()
        missing = []
        for module, method in sorted(self._native_rpc_methods()):
            if (module, method) not in api_methods:
                missing.append(f"{module}.{method}")
        self.assertEqual(missing, [])

    def test_frontend_native_backed_services_use_editor_api(self):
        bridge_source = self._frontend_bridge_source()
        native_methods = self._native_rpc_methods()
        legacy_native_calls = sorted(
            (module, method)
            for module, method in self._frontend_rpc_calls()
            if (module, method) in native_methods
        )
        self.assertEqual(legacy_native_calls, [])

    def test_frontend_services_no_longer_use_public_callcef(self):
        bridge_source = self._frontend_bridge_source()
        self.assertNotRegex(bridge_source, r"Bridge\.callCEF\(")

    def test_frontend_raw_editor_api_transport_is_internal_only(self):
        bridge_source = self._frontend_bridge_source()
        self.assertIn("const call_editor_api = async (apiName, args)", bridge_source)
        self.assertNotIn("static async callEditorApi", bridge_source)
        self.assertNotIn("Bridge.callEditorApi(", bridge_source)
        self.assertIn("call_editor_api('EditorApi.list_methods', [])", bridge_source)
        self.assertIn("call_editor_api('EditorApi.unregister_callback', [callbackToken])", bridge_source)

    def test_frontend_business_files_do_not_reference_cef_query_transport(self):
        frontend_root = self._repo_root() / "editor" / "Frontend" / "src"
        offenders = []
        for source_path in frontend_root.rglob("*.vue"):
            text = source_path.read_text(encoding="utf-8")
            if "window.cefQuery" in text or "cefQuery(" in text:
                offenders.append(str(source_path.relative_to(frontend_root)))
        self.assertEqual(offenders, [])

    def test_editor_api_registry_covers_all_frontend_editor_api_calls(self):
        api_methods = self._editor_api_methods()
        missing = []
        frontend_api_calls = self._frontend_editor_api_calls() | self._frontend_call_editor_api_calls()
        for api_name in sorted(frontend_api_calls):
            module, method = api_name.split(".", 1)
            if (module, method) not in api_methods:
                missing.append(api_name)
        self.assertEqual(missing, [])

    def test_editor_api_registry_covers_all_frontend_typed_wrapper_calls(self):
        bridge_source = self._frontend_bridge_source()
        api_methods = self._editor_api_methods()

        self.assertNotIn("const EDITOR_API_WRAPPER_PATHS = {", bridge_source)
        self.assertIn("static validateEditorApiWrapperMethods()", bridge_source)
        self.assertIn("Bridge.validateEditorApiWrapperMethods()", bridge_source)
        self.assertIn("for (const spec of editorApiMethodSpecs.values())", bridge_source)
        self.assertIn("Bridge.resolveEditorApiWrapperPath(wrapperPath)", bridge_source)

        wrapper_methods = self._frontend_call_editor_api_calls()
        self.assertGreater(len(wrapper_methods), 0)

        missing = []
        for api_name in sorted(wrapper_methods):
            module, method = api_name.split(".", 1)
            if (module, method) not in api_methods:
                missing.append(api_name)
        self.assertEqual(missing, [])

    def test_editor_api_registry_covers_all_python_typed_wrapper_calls(self):
        python_api_source = (
            self._repo_root() / "editor" / "api" / "editor_api.py"
        ).read_text(encoding="utf-8")
        api_methods = self._editor_api_methods()
        api_source = self._editor_api_source()

        self.assertNotIn("_CPP_EDITOR_API_WRAPPER_PATHS = {", python_api_source)
        self.assertNotIn("def _validate_cpp_api_wrapper_methods():", python_api_source)
        self.assertIn('spec.get("python_wrapper") != wrapper_path', python_api_source)

        wrapper_paths = dict(
            re.findall(
                r'_invoke_typed_cpp_api\("([A-Za-z0-9_]+\.[A-Za-z0-9_]+)",\s*"([A-Za-z0-9_]+\.[A-Za-z0-9_]+)"',
                python_api_source,
            )
        )
        self.assertGreater(len(wrapper_paths), 0)
        missing = []
        for api_name in sorted(wrapper_paths):
            module, method = api_name.split(".", 1)
            if (module, method) not in api_methods:
                missing.append(api_name)
        self.assertEqual(missing, [])

        event_python_wrappers = set(
            re.findall(
                r'\{"[A-Za-z0-9_]+\.[A-Za-z0-9_]+",\s*EditorApiValueType::[A-Za-z0-9_]+,\s*all_callers\(\),\s*"[^"]*",\s*"([^"]+)"\}',
                api_source,
            )
        )
        wrapper_events = set(
            re.findall(
                r'_register_manifest_editor_api_event_callback\("([A-Za-z0-9_]+\.[A-Za-z0-9_]+)"',
                python_api_source,
            )
        )
        missing_events = sorted(wrapper_path for wrapper_path in wrapper_events if wrapper_path not in event_python_wrappers)
        self.assertEqual(missing_events, [])

    def test_editor_api_registry_includes_script_facade_methods_as_native_api(self):
        source = self._editor_api_source()
        expected = {
            "AITool.submit_request": "kObjectPayloadParam",
            "ScratchTool.execute_python_code": "kScratchExecutePythonCodeParams",
        }
        for api_name, params_name in expected.items():
            module, method = api_name.split(".", 1)
            with self.subTest(api_name=api_name):
                plain_schema = (
                    f"EDITOR_API_METHOD_SCHEMA({module}, {method}, {params_name}, "
                    "EditorApiValueType::Any)"
                )
                wrapped_schema_prefix = (
                    f"EDITOR_API_METHOD_SCHEMA_WRAPPED({module}, {method}, {params_name}, "
                )
                self.assertTrue(
                    plain_schema in source
                    or (wrapped_schema_prefix in source and "EditorApiValueType::Any)" in source),
                    f"{api_name} must be registered as a native Editor API method",
                )
        self.assertNotIn("EditorApiBackend::Python", source)

    def test_python_script_services_do_not_keep_backend_alias(self):
        registry_source = (
            self._repo_root() / "editor" / "runtime" / "registry.py"
        ).read_text(encoding="utf-8")

        self.assertIn("PYTHON_SCRIPT_SERVICES = {", registry_source)
        self.assertIn("def register_python_script_services():", registry_source)
        self.assertNotIn("register_python_backends", registry_source)

    def test_cef_query_accepts_editor_api_requests_before_legacy_rpc(self):
        query_source = self._query_bridge_source()
        self.assertIn("CefEditorApiEndpoint", query_source)
        self.assertIn("parse_editor_api_request(request_payload", query_source)
        self.assertNotIn("find_editor_rpc_spec", query_source)
        self.assertNotIn("parse_native_request", query_source)
        self.assertNotIn('request_payload.find("module")', query_source)

    def test_old_public_rpc_contract_is_removed(self):
        repo_root = self._repo_root()
        self.assertFalse(
            (repo_root / "src" / "systems" / "ui" / "cef" / "cef_editor_rpc_contract.cpp").exists()
        )
        native_header = self._native_api_header()
        for symbol in (
            "EditorRpcId",
            "EditorRpcBackend",
            "EditorRpcSpec",
            "find_editor_rpc_spec",
            "EditorRpcInterfaceBase",
            "CefEditorRpcInterface",
            "PythonEditorRpcInterface",
        ):
            with self.subTest(symbol=symbol):
                self.assertNotIn(symbol, native_header)

    def test_frontend_sample_methods_use_editor_api_not_public_rpc(self):
        bridge_source = self._frontend_bridge_source()
        self.assertIn("export const editorApi", bridge_source)
        self.assertIn("const call_editor_api = async (apiName, args)", bridge_source)
        self.assertNotIn("static async callEditorApi", bridge_source)
        self.assertNotIn("Bridge.callEditorApi(", bridge_source)
        self.assertIn("validateEditorApiWrapperPath(apiName, spec, wrapperPath)", bridge_source)
        for snippet in (
            "getAppVersion: () => call_manifest_editor_api('project.getAppVersion', [])",
            "listActorTree: (sceneName) => call_manifest_editor_api('scene.listActorTree', [sceneName])",
            "sceneSave: (sceneName) => call_manifest_editor_api('main.sceneSave', [sceneName])",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)

    def test_python_editor_api_wrapper_consumes_cpp_defined_methods(self):
        api_source = (
            self._repo_root()
            / "editor"
            / "api"
            / "editor_api.py"
        ).read_text(encoding="utf-8")
        self.assertIn("class CoronaEditorApi", api_source)
        self.assertIn("def _invoke_cpp_api(", api_source)
        self.assertIn("def _invoke_typed_cpp_api(api_name, wrapper_path, args=None):", api_source)
        self.assertIn('if spec.get("python_wrapper") != wrapper_path:', api_source)
        self.assertIn("CoronaEngine._invoke_cpp_editor_api", api_source)
        self.assertNotIn("CoronaEngine.invoke_cpp_api", api_source)
        self.assertIn("class _DynamicApiNamespace:", api_source)
        self.assertIn("def _find_cpp_api_method_by_python_wrapper(wrapper_path):", api_source)
        self.assertIn('if spec.get("python_wrapper") == wrapper_path:', api_source)
        self.assertIn("def _invoke_manifest_cpp_api(wrapper_path, args=None):", api_source)
        self.assertIn("def _register_manifest_editor_api_event_callback(wrapper_name, callback):", api_source)
        self.assertIn("class _CoronaEditorApiMeta(type):", api_source)
        self.assertIn("def __getattr__(cls, name):", api_source)
        self.assertIn("return _DynamicApiNamespace(name)", api_source)
        self.assertIn('_invoke_typed_cpp_api("EditorApi.list_methods", "editor.list_methods", [])', api_source)
        self.assertIn('_invoke_typed_cpp_api("EditorApi.list_events", "editor.list_events", [])', api_source)
        self.assertIn('_invoke_typed_cpp_api("EditorApi.unregister_callback", "editor.unregister_callback", [callback_token])', api_source)
        self.assertIn('_invoke_manifest_cpp_api("project.get_app_version", [])', api_source)
        self.assertIn('_invoke_manifest_cpp_api("scene.list_actor_tree", [scene_name])', api_source)
        self.assertIn('_invoke_manifest_cpp_api("scene.select_actor", [scene_name, actor_type, actor_name])', api_source)
        self.assertIn('_invoke_manifest_cpp_api("main.scene_save", args)', api_source)
        self.assertIn("class _EventsApi", api_source)
        self.assertIn(
            'return _register_manifest_editor_api_event_callback("events.on_actor_changed", callback)',
            api_source,
        )
        self.assertIn(
            'return _register_manifest_editor_api_event_callback("events.on_project_opened", callback)',
            api_source,
        )
        self.assertNotIn('_register_editor_api_event_callback("SceneTools.', api_source)
        self.assertNotIn('_register_editor_api_event_callback("ProjectLauncher.', api_source)
        self.assertNotIn('_register_editor_api_event_callback("Network.', api_source)
        self.assertNotIn('_register_editor_api_event_callback("LANChat.', api_source)
        self.assertNotIn('_register_editor_api_event_callback("AI.', api_source)
        self.assertNotIn('_invoke_typed_cpp_api("SceneTools.', api_source)
        self.assertNotIn('_invoke_typed_cpp_api("ProjectLauncher.', api_source)
        self.assertNotIn('_invoke_typed_cpp_api("MainView.', api_source)
        self.assertIn('event_spec.get("python_wrapper") != wrapper_name', api_source)
        self.assertNotIn("CoronaEditorApi.editor.on(", api_source)

    def test_python_cpp_editor_api_transport_is_internal_only(self):
        bind_source = (
            self._repo_root() / "src" / "systems" / "ui" / "cef" / "cef_py_bind.cpp"
        ).read_text(encoding="utf-8")
        python_api_source = (
            self._repo_root() / "editor" / "api" / "editor_api.py"
        ).read_text(encoding="utf-8")

        self.assertIn('m.def("_invoke_cpp_editor_api"', bind_source)
        self.assertNotIn('m.def("invoke_cpp_api"', bind_source)
        self.assertIn("CoronaEngine._invoke_cpp_editor_api(api_name, payload)", python_api_source)
        self.assertNotIn("CoronaEngine.invoke_cpp_api", python_api_source)

    def test_editor_api_manifest_is_cpp_defined_and_consumed_by_wrappers(self):
        api_source = self._editor_api_source()
        handler_source = self._handler_source()
        query_source = self._query_bridge_source()
        bridge_source = self._frontend_bridge_source()
        python_api_source = (
            self._repo_root()
            / "editor"
            / "api"
            / "editor_api.py"
        ).read_text(encoding="utf-8")

        self.assertIn(
            "EDITOR_API_METHOD_SCHEMA(EditorApi, list_methods, kNoParams, EditorApiValueType::Object)",
            api_source,
        )
        self.assertIn(
            "EDITOR_API_METHOD_SCHEMA(EditorApi, list_events, kNoParams, EditorApiValueType::Object)",
            api_source,
        )
        self.assertIn(
            "EDITOR_API_METHOD_SCHEMA(EditorApi, register_callback, kEditorApiRegisterCallbackParams, EditorApiValueType::Object)",
            api_source,
        )
        self.assertIn(
            "EDITOR_API_METHOD_SCHEMA(EditorApi, unregister_callback, kCallbackTokenParam, EditorApiValueType::Object)",
            api_source,
        )
        self.assertIn('"list_methods"', handler_source)
        self.assertIn('"list_events"', handler_source)
        self.assertIn('"register_callback"', handler_source)
        self.assertIn('"unregister_callback"', handler_source)
        self.assertIn("EditorApiRegistry::instance().list_methods()", handler_source)
        self.assertIn("EditorApiRegistry::instance().list_events()", handler_source)
        self.assertNotIn('request_payload.value("api", std::string{}) == "EditorApi.register_callback"', query_source)
        self.assertNotIn('request_payload.value("api", std::string{}) == "EditorApi.unregister_callback"', query_source)
        self.assertIn("listMethods: () => call_editor_api('EditorApi.list_methods', [])", bridge_source)
        self.assertIn("listEvents: () => call_editor_api('EditorApi.list_events', [])", bridge_source)
        self.assertIn('_invoke_typed_cpp_api("EditorApi.list_methods", "editor.list_methods", [])', python_api_source)
        self.assertIn('_invoke_typed_cpp_api("EditorApi.list_events", "editor.list_events", [])', python_api_source)

    def test_editor_api_spec_uses_native_dispatch_terms_not_legacy_rpc_terms(self):
        api_header = self._editor_api_header()
        api_source = self._editor_api_source()
        handler_source = self._handler_source()
        combined = "\n".join((api_header, api_source, handler_source))

        self.assertNotIn("legacy_module", combined)
        self.assertNotIn("legacy_function", combined)
        self.assertIn("native_module", api_header)
        self.assertIn("native_function", api_header)
        self.assertIn('{"native_module", spec.native_module ? spec.native_module : ""}', handler_source)
        self.assertIn('{"native_function", spec.native_function ? spec.native_function : ""}', handler_source)
        self.assertNotIn('{"module", spec.', handler_source)
        self.assertNotIn('{"function", spec.', handler_source)

    def test_python_script_service_dispatcher_is_registered_explicitly(self):
        repo_root = self._repo_root()
        bind_source = (
            repo_root / "src" / "systems" / "ui" / "cef" / "cef_py_bind.cpp"
        ).read_text(encoding="utf-8")
        main_source = (
            repo_root / "editor" / "runtime" / "bootstrap.py"
        ).read_text(encoding="utf-8")
        editor_source = (
            repo_root / "editor" / "runtime" / "editor_host.py"
        ).read_text(encoding="utf-8")

        self.assertIn("register_python_script_service_dispatcher", bind_source)
        self.assertIn("unregister_python_script_service_dispatcher", bind_source)
        self.assertNotIn("register_python_script_dispatcher", bind_source)
        self.assertNotIn("unregister_python_script_dispatcher", bind_source)
        self.assertIn("editor.register_script_dispatcher()", main_source)
        self.assertIn("dispatch_script_request_from_cpp", editor_source)
        self.assertNotIn("register_editor_api_python_dispatcher", bind_source)
        self.assertNotIn("dispatch_editor_api_from_cpp", editor_source)
        self.assertIn("if 'api' in request", editor_source)
        self.assertIn("Editor API payload is not accepted by Python script service dispatcher", editor_source)
        self.assertIn("request.get('module'", editor_source)
        self.assertIn("request.get('function'", editor_source)

    def test_editor_api_spec_defines_real_schema_and_events(self):
        header = self._editor_api_header()
        source = self._editor_api_source()

        for symbol in (
            "enum class EditorApiValueType",
            "struct EditorApiParamSpec",
            "struct EditorApiReturnSpec",
            "struct EditorApiEventSpec",
            "js_wrapper",
            "python_wrapper",
            "class EditorApiCallbackRegistry",
        ):
            with self.subTest(symbol=symbol):
                self.assertIn(symbol, header)

        for snippet in (
            'EDITOR_API_METHOD0_WRAPPED(ProjectLauncher, get_app_version, "project.getAppVersion", "project.get_app_version", EditorApiValueType::String)',
            'EDITOR_API_METHOD1_WRAPPED(SceneTools, list_actor_tree, "scene.listActorTree", "scene.list_actor_tree", "scene_name", EditorApiValueType::String, EditorApiValueType::Array)',
            'EDITOR_API_METHOD_SCHEMA_WRAPPED(MainView, scene_save, kSceneSaveParams, "main.sceneSave", "main.scene_save", EditorApiValueType::Object)',
            '{"SceneTools.actorChanged", EditorApiValueType::Object, all_callers(), "events.onActorChanged", "events.on_actor_changed"}',
            '{"ProjectLauncher.projectOpened", EditorApiValueType::Object, all_callers(), "events.onProjectOpened", "events.on_project_opened"}',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source)

        self.assertNotIn('"json[]"', source)

    def test_editor_api_event_manifest_defines_typed_wrapper_names(self):
        handler_source = self._handler_source()

        for snippet in (
            '{"js_wrapper", spec.js_wrapper ? spec.js_wrapper : ""}',
            '{"python_wrapper", spec.python_wrapper ? spec.python_wrapper : ""}',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, handler_source)

    def test_editor_api_method_manifest_defines_typed_wrapper_names(self):
        handler_source = self._handler_source()
        start = handler_source.find("nlohmann::json editor_api_method_to_json")
        self.assertGreaterEqual(start, 0)
        end = handler_source.find("nlohmann::json editor_api_event_to_json", start)
        self.assertGreater(end, start)
        method_json_source = handler_source[start:end]

        for snippet in (
            '{"js_wrapper", editor_api_js_wrapper_path(spec)}',
            '{"python_wrapper", editor_api_python_wrapper_path(spec)}',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, method_json_source)

    def test_editor_api_method_manifest_computes_default_wrapper_names_in_cpp(self):
        handler_source = self._handler_source()

        for snippet in (
            "std::string editor_api_js_wrapper_path(const EditorApiMethodSpec& spec)",
            "std::string editor_api_python_wrapper_path(const EditorApiMethodSpec& spec)",
            "editor_api_module_js_wrapper_alias(spec.native_module)",
            "editor_api_module_python_wrapper_alias(spec.native_module)",
            "snake_to_lower_camel(spec.native_function)",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, handler_source)

    def test_frontend_wrapper_paths_are_validated_against_cpp_manifest(self):
        bridge_source = self._frontend_bridge_source()

        for snippet in (
            "static resolveEditorApiWrapperPath(wrapperPath)",
            "for (const spec of editorApiMethodSpecs.values())",
            "const wrapperPath = spec?.js_wrapper;",
            "Bridge.resolveEditorApiWrapperPath(wrapperPath)",
            "Frontend wrapper path is not implemented",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)
        self.assertNotIn("const EDITOR_API_WRAPPER_METHODS = [", bridge_source)
        self.assertNotIn("const EDITOR_API_WRAPPER_PATHS = {", bridge_source)

    def test_frontend_editor_api_object_exposes_cpp_manifest_wrapper_aliases(self):
        bridge_source = self._frontend_bridge_source()

        for snippet in (
            "editor: {",
            "listMethods: () => call_editor_api('EditorApi.list_methods', [])",
            "listEvents: () => call_editor_api('EditorApi.list_events', [])",
            "registerCallback: (eventName, callbackSpec = {}) =>",
            "unregisterCallback: (callbackToken) => unregister_callback(callbackToken)",
            "submitRequest: (payload) => call_manifest_editor_api('ai.submitRequest', [payload || {}])",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)

    def test_frontend_editor_api_uses_manifest_driven_dynamic_wrappers(self):
        bridge_source = self._frontend_bridge_source()

        for snippet in (
            "const find_editor_api_method_by_js_wrapper = async (wrapperPath) =>",
            "const find_editor_api_event_by_js_wrapper = async (wrapperPath) =>",
            "const create_dynamic_editor_api_namespace = (wrapperPath, target = {}) =>",
            "return new Proxy(target, {",
            "const editorApiStatic = {",
            "export const editorApi = create_dynamic_editor_api_namespace('', editorApiStatic);",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)

    def test_editor_api_method_manifest_explicitly_defines_acronym_wrapper_paths(self):
        source = self._editor_api_source()
        bridge_source = self._frontend_bridge_source()

        self.assertIn(
            'EDITOR_API_METHOD_SCHEMA_WRAPPED(AITool, send_message_to_ai_stream, kAnyPayloadParam, '
            '"ai.sendMessageToAIStream", "ai.send_message_to_ai_stream", EditorApiValueType::Any)',
            source,
        )
        self.assertIn("sendMessageToAIStream: (payload) =>", bridge_source)
        self.assertIn("call_manifest_editor_api('ai.sendMessageToAIStream', [payload])", bridge_source)
        self.assertNotIn("'AITool.send_message_to_ai_stream': 'ai.sendMessageToAiStream'", bridge_source)

    def test_python_wrapper_paths_are_validated_against_cpp_manifest(self):
        python_api_source = (
            self._repo_root() / "editor" / "api" / "editor_api.py"
        ).read_text(encoding="utf-8")

        for snippet in (
            'spec.get("python_wrapper") != wrapper_path',
            "Python wrapper path is not defined by C++ manifest",
            "class _EditorApi(_DynamicApiNamespace):",
            "class _EventsApi(_DynamicApiNamespace):",
            "class _ProjectApi(_DynamicApiNamespace):",
            "class _SceneApi(_DynamicApiNamespace):",
            "class _MainApi(_DynamicApiNamespace):",
            'super().__init__("editor")',
            'super().__init__("events")',
            'super().__init__("project")',
            'super().__init__("scene")',
            'super().__init__("main")',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, python_api_source)
        self.assertNotIn("_CPP_EDITOR_API_WRAPPER_PATHS", python_api_source)
        self.assertNotIn("_CPP_EDITOR_API_WRAPPER_METHODS = (", python_api_source)

    def test_editor_api_validates_arguments_and_results(self):
        source = self._editor_api_source()

        self.assertIn("validate_editor_api_args", source)
        self.assertIn("validate_editor_api_result", source)
        self.assertIn("invalid Editor API arguments", source)
        self.assertIn("invalid Editor API result", source)

        invoke_start = source.find("NativeResult EditorApiRegistry::invoke")
        self.assertGreaterEqual(invoke_start, 0)
        invoke_end = source.find("std::optional<EditorApiRequest> parse_editor_api_request", invoke_start)
        self.assertGreater(invoke_end, invoke_start)
        invoke_body = source[invoke_start:invoke_end]
        args_pos = invoke_body.find("validate_editor_api_args(")
        native_pos = invoke_body.find("invoke_native_api_method(")
        result_pos = invoke_body.find("validate_editor_api_result(")
        self.assertGreaterEqual(args_pos, 0)
        self.assertGreater(native_pos, args_pos)
        self.assertGreater(result_pos, native_pos)

    def test_editor_api_registry_has_no_unspecified_method_entries(self):
        source = self._editor_api_source()

        registry_start = source.find("kEditorApiMethods = std::to_array<EditorApiMethodSpec>")
        self.assertGreaterEqual(registry_start, 0)
        registry_end = source.find("});", registry_start)
        self.assertGreater(registry_end, registry_start)
        registry_body = source[registry_start:registry_end]
        self.assertNotIn("EDITOR_API_METHOD(", registry_body)
        self.assertRegex(
            source,
            r"EDITOR_API_METHOD_SCHEMA(?:_WRAPPED)?\(ScratchTool, execute_python_code, "
            r"kScratchExecutePythonCodeParams, .*EditorApiValueType::Any\)",
        )
        validation_start = source.find("NativeResult validate_editor_api_args")
        self.assertGreaterEqual(validation_start, 0)
        validation_end = source.find("NativeResult validate_editor_api_result", validation_start)
        self.assertGreater(validation_end, validation_start)
        validation_body = source[validation_start:validation_end]
        self.assertIn("spec.params == nullptr", validation_body)
        self.assertIn("Unspecified Editor API schema", validation_body)

    def test_scene_tools_common_editor_apis_have_explicit_schema(self):
        source = self._editor_api_source()

        expected = {
            "SceneTools.create_actor": "kSceneToolsCreateActorParams",
            "SceneTools.remove_actor": "kSceneActorParams",
            "SceneTools.rename_actor": "kSceneToolsRenameActorParams",
            "SceneTools.open_actor": "kSceneActorParams",
            "SceneTools.focus_actor": "kSceneToolsFocusActorParams",
            "SceneTools.set_render_backend": "kSceneToolsSetRenderBackendParams",
            "SceneTools.get_render_backend": "kSceneToolsCameraOptionalParams",
            "SceneTools.set_vision_render_mode": "kSceneToolsSetVisionRenderModeParams",
            "SceneTools.get_vision_render_mode": "kSceneToolsCameraOptionalParams",
        }
        for api_name, params_name in expected.items():
            module, method = api_name.split(".", 1)
            with self.subTest(api_name=api_name):
                plain_macro = f"EDITOR_API_METHOD_SCHEMA({module}, {method}, {params_name}, EditorApiValueType::Object)"
                wrapped_macro = f"EDITOR_API_METHOD_SCHEMA_WRAPPED({module}, {method}, {params_name},"
                caller_macro = f"EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS({module}, {method}, {params_name},"
                self.assertTrue(
                    plain_macro in source or wrapped_macro in source or caller_macro in source,
                    f"{api_name} should define an explicit C++ method schema",
                )

        self.assertIn("value.is_null() && param_spec.optional", source)

    def test_editor_api_callback_registry_replaces_placeholder_tokens(self):
        header = self._editor_api_header()
        source = self._editor_api_source()

        for symbol in (
            "register_cef_callback",
            "register_python_script_callback",
            "emit_editor_api_event",
            "emit_python_script_event",
        ):
            with self.subTest(symbol=symbol):
                self.assertIn(symbol, header + source)

        self.assertNotIn("return g_next_callback_token.fetch_add(1);", source)
        self.assertNotIn("return callback_token != 0;", source)
        self.assertIn("EditorApiCallbackRegistry::instance()", source)

    def test_frontend_bridge_exposes_event_subscription_helpers(self):
        bridge_source = self._frontend_bridge_source()

        for snippet in (
            "off: (callbackToken)",
            "events: {",
            "register_typed_editor_api_callback",
            "eventSpec?.js_wrapper !== wrapperName",
            "register_manifest_editor_api_callback",
            "onActorChanged: (callback) => register_manifest_editor_api_callback('events.onActorChanged', callback)",
            "onProjectOpened: (callback) => register_manifest_editor_api_callback('events.onProjectOpened', callback)",
            "__coronaEditorApiDispatch",
            "register_editor_api_callback",
            "unregister_callback",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)
        self.assertNotIn("on: (eventName, callback)", bridge_source)
        self.assertNotIn("unregister_editor_api_callback", bridge_source)

    def test_frontend_event_wrappers_are_manifest_driven(self):
        bridge_source = self._frontend_bridge_source()
        start = bridge_source.find("events: {")
        end = bridge_source.find("app: {", start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        events_section = bridge_source[start:end]

        expected_wrappers = (
            "events.onAiChunk",
            "events.onLogBatch",
            "events.onActorChanged",
            "events.onActorSelectionChanged",
            "events.onActorTransformUpdated",
            "events.onActorPickResult",
            "events.onFocusPoseResult",
            "events.onNetworkActorDeleteSyncBroadcastRequested",
            "events.onNetworkActorOwnershipClaimed",
            "events.onNetworkActorStateSyncBroadcastRequested",
            "events.onNetworkActorSyncBroadcastRequested",
            "events.onNetworkActorTransformSyncBroadcastRequested",
            "events.onNetworkAssetImportCompleted",
            "events.onNetworkFileSyncStatusChanged",
            "events.onNetworkSyncPauseRequested",
            "events.onSceneAdded",
            "events.onSceneRenamed",
            "events.onSceneTreeChanged",
            "events.onProjectOpened",
            "events.onLanChatEvent",
        )
        for wrapper in expected_wrappers:
            with self.subTest(wrapper=wrapper):
                self.assertIn(f"register_manifest_editor_api_callback('{wrapper}'", events_section)
        for event_prefix in ("AI.", "Editor.", "SceneTools.", "Network.", "ProjectLauncher.", "LANChat."):
            with self.subTest(event_prefix=event_prefix):
                self.assertNotIn(event_prefix, events_section)

    def test_scene_tree_changed_event_is_defined_and_emitted_by_cpp(self):
        source = self._editor_api_source()
        handler_source = self._handler_source()
        bridge_source = self._frontend_bridge_source()

        for snippet in (
            '{"SceneTools.sceneTreeChanged", EditorApiValueType::Object, all_callers(), '
            '"events.onSceneTreeChanged", "events.on_scene_tree_changed"}',
            "onSceneTreeChanged: (callback) => register_manifest_editor_api_callback("
            "'events.onSceneTreeChanged', callback)",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source + bridge_source)

        emit_body = re.search(
            r"void emit_scene_tree_changed\(const std::string& scene_route\) \{.*?\n\}",
            handler_source,
            re.S,
        )
        self.assertIsNotNone(emit_body)
        body = emit_body.group(0)
        self.assertIn('emit_editor_api_event("SceneTools.sceneTreeChanged"', body)
        self.assertIn('{"scene", scene_route}', body)
        self.assertNotIn("__coronaEmit", body)
        self.assertNotIn("scene-tree-changed", body)

        event_bus_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "utils"
            / "eventBus.js"
        ).read_text(encoding="utf-8")
        self.assertNotIn("event === 'scene-tree-changed'", event_bus_source)

    def test_actor_changed_event_is_emitted_by_cpp_callback_registry(self):
        handler_source = self._handler_source()

        emit_body = re.search(
            r"void emit_actor_change\(const NativeContext& context,.*?\n\}",
            handler_source,
            re.S,
        )
        self.assertIsNotNone(emit_body)
        body = emit_body.group(0)
        self.assertIn('emit_editor_api_event("SceneTools.actorChanged"', body)
        self.assertIn('{"actor_type", actor.actor_type}', body)
        self.assertIn('{"scene", scene.route}', body)
        self.assertIn('{"actor", actor.name}', body)
        self.assertIn("(void)context;", body)
        self.assertNotIn("__coronaEmit", body)
        self.assertNotIn("actor-change", body)

    def test_actor_selection_is_cpp_defined_api_and_event(self):
        source = self._editor_api_source()
        handler_source = self._handler_source()
        bridge_source = self._frontend_bridge_source()
        python_api_source = (
            self._repo_root()
            / "editor"
            / "api"
            / "editor_api.py"
        ).read_text(encoding="utf-8")
        main_page_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "views"
            / "layout"
            / "MainPage.vue"
        ).read_text(encoding="utf-8")
        scene_bar_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "views"
            / "sidebar"
            / "SceneBar.vue"
        ).read_text(encoding="utf-8")
        object_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "views"
            / "sidebar"
            / "Object.vue"
        ).read_text(encoding="utf-8")
        event_bus_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "utils"
            / "eventBus.js"
        ).read_text(encoding="utf-8")

        for snippet in (
            "constexpr std::array<EditorApiParamSpec, 4> kSceneToolsSelectActorParams",
            'EDITOR_API_METHOD_SCHEMA_WRAPPED(SceneTools, select_actor, kSceneToolsSelectActorParams, '
            '"sceneTools.selectActor", "scene.select_actor", EditorApiValueType::Object)',
            '{"SceneTools.actorSelectionChanged", EditorApiValueType::Object, all_callers(), '
            '"events.onActorSelectionChanged", "events.on_actor_selection_changed"}',
            "onActorSelectionChanged: (callback) => register_manifest_editor_api_callback("
            "'events.onActorSelectionChanged', callback)",
            'return _register_manifest_editor_api_event_callback("events.on_actor_selection_changed", callback)',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source + bridge_source + python_api_source)

        self.assertIn('{"select_actor", [](const NativeRequest& request, const NativeContext&)', handler_source)
        self.assertIn('emit_editor_api_event("SceneTools.actorSelectionChanged"', handler_source)
        self.assertIn('{"actor_type", actor_type}', handler_source)
        self.assertIn('{"scene", scene_name}', handler_source)
        self.assertIn('{"actor", actor_name}', handler_source)

        self.assertIn("editorApi.sceneTools.selectActor(sceneId, type, actorName, {", main_page_source)
        self.assertIn("editorApi.sceneTools.selectActor(currentSceneName.value, scene.type || 'actor', scene.name)", scene_bar_source)
        self.assertIn("editorApi.events.onActorSelectionChanged(handleSelection)", object_source)
        self.assertIn("editorApi.off(selectionToken)", object_source)
        for source_name, frontend_source in (
            ("MainPage", main_page_source),
            ("SceneBar", scene_bar_source),
            ("Object", object_source),
        ):
            with self.subTest(source=source_name):
                self.assertNotIn("'actor-change'", frontend_source)
        self.assertNotIn("event === 'actor-change'", event_bus_source)

    def test_actor_transform_update_is_cpp_defined_event(self):
        source = self._editor_api_source()
        bridge_source = self._frontend_bridge_source()
        python_api_source = (
            self._repo_root()
            / "editor"
            / "api"
            / "editor_api.py"
        ).read_text(encoding="utf-8")
        corona_editor_source = (
            self._repo_root()
            / "editor"
            / "runtime"
            / "editor_host.py"
        ).read_text(encoding="utf-8")
        object_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "views"
            / "sidebar"
            / "Object.vue"
        ).read_text(encoding="utf-8")
        event_bus_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "utils"
            / "eventBus.js"
        ).read_text(encoding="utf-8")

        for snippet in (
            '{"SceneTools.actorTransformUpdated", EditorApiValueType::Object, all_callers(), '
            '"events.onActorTransformUpdated", "events.on_actor_transform_updated"}',
            "onActorTransformUpdated: (callback) => register_manifest_editor_api_callback("
            "'events.onActorTransformUpdated', callback)",
            'return _register_manifest_editor_api_event_callback("events.on_actor_transform_updated", callback)',
            '"transform-update": ("events.on_actor_transform_updated", lambda values: {',
            '"position": values[2] if len(values) > 2 else {},',
            '"rotation": values[3] if len(values) > 3 else {},',
            '"scale": values[4] if len(values) > 4 else {},',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source + bridge_source + python_api_source + corona_editor_source)

        self.assertIn("editorApi.events.onActorTransformUpdated(handleTransform)", object_source)
        self.assertIn("editorApi.off(transformToken)", object_source)
        self.assertNotIn("coronaEventBus.on('transform-update'", object_source)
        self.assertNotIn("coronaEventBus.off('transform-update'", object_source)
        self.assertNotIn("event === 'transform-update'", event_bus_source)

    def test_actor_ownership_claim_is_cpp_defined_event(self):
        source = self._editor_api_source()
        bridge_source = self._frontend_bridge_source()
        python_api_source = (
            self._repo_root()
            / "editor"
            / "api"
            / "editor_api.py"
        ).read_text(encoding="utf-8")
        corona_editor_source = (
            self._repo_root()
            / "editor"
            / "runtime"
            / "editor_host.py"
        ).read_text(encoding="utf-8")
        network_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "views"
            / "sidebar"
            / "Network.vue"
        ).read_text(encoding="utf-8")
        event_bus_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "utils"
            / "eventBus.js"
        ).read_text(encoding="utf-8")

        for snippet in (
            '{"Network.actorOwnershipClaimed", EditorApiValueType::Object, all_callers(), '
            '"events.onNetworkActorOwnershipClaimed", "events.on_network_actor_ownership_claimed"}',
            "onNetworkActorOwnershipClaimed: (callback) => register_manifest_editor_api_callback("
            "'events.onNetworkActorOwnershipClaimed', callback)",
            'return _register_manifest_editor_api_event_callback("events.on_network_actor_ownership_claimed", callback)',
            '"actor-ownership-claim": ("events.on_network_actor_ownership_claimed", lambda values: {',
            '"actor_guid": (values[0] if len(values) > 0 else {}).get("actor_guid", "")',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source + bridge_source + python_api_source + corona_editor_source)

        self.assertIn(
            "editorApi.events.onNetworkActorOwnershipClaimed(onNetworkActorOwnershipClaimed)",
            network_source,
        )
        self.assertIn("editorApi.off(actorOwnershipClaimCallbackToken)", network_source)
        self.assertNotIn("coronaEventBus.on('actor-ownership-claim'", network_source)
        self.assertNotIn("coronaEventBus.off('actor-ownership-claim'", network_source)
        self.assertNotIn("event === 'actor-ownership-claim'", event_bus_source)

    def test_network_sync_pause_request_is_cpp_defined_event(self):
        source = self._editor_api_source()
        bridge_source = self._frontend_bridge_source()
        python_api_source = (
            self._repo_root()
            / "editor"
            / "api"
            / "editor_api.py"
        ).read_text(encoding="utf-8")
        corona_editor_source = (
            self._repo_root()
            / "editor"
            / "runtime"
            / "editor_host.py"
        ).read_text(encoding="utf-8")
        network_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "views"
            / "sidebar"
            / "Network.vue"
        ).read_text(encoding="utf-8")
        event_bus_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "utils"
            / "eventBus.js"
        ).read_text(encoding="utf-8")

        for snippet in (
            '{"Network.syncPauseRequested", EditorApiValueType::Object, all_callers(), '
            '"events.onNetworkSyncPauseRequested", "events.on_network_sync_pause_requested"}',
            "onNetworkSyncPauseRequested: (callback) => register_manifest_editor_api_callback("
            "'events.onNetworkSyncPauseRequested', callback)",
            'return _register_manifest_editor_api_event_callback("events.on_network_sync_pause_requested", callback)',
            '"network-sync-pause-request": ("events.on_network_sync_pause_requested", lambda values: {',
            '"paused": bool((values[0] if len(values) > 0 else {}).get("paused", False))',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source + bridge_source + python_api_source + corona_editor_source)

        self.assertIn(
            "editorApi.events.onNetworkSyncPauseRequested(onNetworkSyncPauseRequested)",
            network_source,
        )
        self.assertIn("editorApi.off(networkSyncPauseCallbackToken)", network_source)
        self.assertNotIn("coronaEventBus.on('network-sync-pause-request'", network_source)
        self.assertNotIn("coronaEventBus.off('network-sync-pause-request'", network_source)
        self.assertNotIn("event === 'network-sync-pause-request'", event_bus_source)

    def test_network_file_sync_status_is_cpp_defined_event(self):
        source = self._editor_api_source()
        bridge_source = self._frontend_bridge_source()
        python_api_source = (
            self._repo_root()
            / "editor"
            / "api"
            / "editor_api.py"
        ).read_text(encoding="utf-8")
        corona_editor_source = (
            self._repo_root()
            / "editor"
            / "runtime"
            / "editor_host.py"
        ).read_text(encoding="utf-8")
        network_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "views"
            / "sidebar"
            / "Network.vue"
        ).read_text(encoding="utf-8")
        event_bus_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "utils"
            / "eventBus.js"
        ).read_text(encoding="utf-8")

        for snippet in (
            '{"Network.fileSyncStatusChanged", EditorApiValueType::Object, all_callers(), '
            '"events.onNetworkFileSyncStatusChanged", "events.on_network_file_sync_status_changed"}',
            "onNetworkFileSyncStatusChanged: (callback) => register_manifest_editor_api_callback("
            "'events.onNetworkFileSyncStatusChanged', callback)",
            'return _register_manifest_editor_api_event_callback("events.on_network_file_sync_status_changed", callback)',
            '"file-sync-status": ("events.on_network_file_sync_status_changed", lambda values: {',
            '"status": (values[0] if len(values) > 0 else {}).get("status", ""),',
            '"model_path": (values[0] if len(values) > 0 else {}).get("model_path", ""),',
            '"progress": (values[0] if len(values) > 0 else {}).get("progress", 0),',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source + bridge_source + python_api_source + corona_editor_source)

        self.assertIn(
            "editorApi.events.onNetworkFileSyncStatusChanged(onNetworkFileSyncStatusChanged)",
            network_source,
        )
        self.assertIn("editorApi.off(networkFileSyncStatusCallbackToken)", network_source)
        self.assertNotIn("coronaEventBus.on('file-sync-status'", network_source)
        self.assertNotIn("coronaEventBus.off('file-sync-status'", network_source)
        self.assertNotIn("event === 'file-sync-status'", event_bus_source)

    def test_network_asset_import_complete_is_cpp_defined_event(self):
        source = self._editor_api_source()
        bridge_source = self._frontend_bridge_source()
        python_api_source = (
            self._repo_root()
            / "editor"
            / "api"
            / "editor_api.py"
        ).read_text(encoding="utf-8")
        corona_editor_source = (
            self._repo_root()
            / "editor"
            / "runtime"
            / "editor_host.py"
        ).read_text(encoding="utf-8")
        network_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "views"
            / "sidebar"
            / "Network.vue"
        ).read_text(encoding="utf-8")
        event_bus_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "utils"
            / "eventBus.js"
        ).read_text(encoding="utf-8")

        for snippet in (
            '{"Network.assetImportCompleted", EditorApiValueType::Object, all_callers(), '
            '"events.onNetworkAssetImportCompleted", "events.on_network_asset_import_completed"}',
            "onNetworkAssetImportCompleted: (callback) => register_manifest_editor_api_callback("
            "'events.onNetworkAssetImportCompleted', callback)",
            'return _register_manifest_editor_api_event_callback("events.on_network_asset_import_completed", callback)',
            '"import-asset-complete": ("events.on_network_asset_import_completed", lambda values: values[0] if len(values) > 0 else {}),',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source + bridge_source + python_api_source + corona_editor_source)

        self.assertIn(
            "editorApi.events.onNetworkAssetImportCompleted(onNetworkAssetImportCompleted)",
            network_source,
        )
        self.assertIn("editorApi.off(networkAssetImportCompletedCallbackToken)", network_source)
        self.assertNotIn("coronaEventBus.on('import-asset-complete'", network_source)
        self.assertNotIn("coronaEventBus.off('import-asset-complete'", network_source)
        self.assertNotIn("event === 'import-asset-complete'", event_bus_source)

    def test_network_actor_sync_broadcast_is_cpp_defined_event(self):
        source = self._editor_api_source()
        bridge_source = self._frontend_bridge_source()
        python_api_source = (
            self._repo_root()
            / "editor"
            / "api"
            / "editor_api.py"
        ).read_text(encoding="utf-8")
        corona_editor_source = (
            self._repo_root()
            / "editor"
            / "runtime"
            / "editor_host.py"
        ).read_text(encoding="utf-8")
        network_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "views"
            / "sidebar"
            / "Network.vue"
        ).read_text(encoding="utf-8")
        event_bus_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "utils"
            / "eventBus.js"
        ).read_text(encoding="utf-8")

        for snippet in (
            '{"Network.actorSyncBroadcastRequested", EditorApiValueType::Object, all_callers(), '
            '"events.onNetworkActorSyncBroadcastRequested", "events.on_network_actor_sync_broadcast_requested"}',
            "onNetworkActorSyncBroadcastRequested: (callback) => register_manifest_editor_api_callback("
            "'events.onNetworkActorSyncBroadcastRequested', callback)",
            'return _register_manifest_editor_api_event_callback("events.on_network_actor_sync_broadcast_requested", callback)',
            '"actor-sync-broadcast": ("events.on_network_actor_sync_broadcast_requested", lambda values: values[0] if len(values) > 0 else {}),',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source + bridge_source + python_api_source + corona_editor_source)

        self.assertIn(
            "editorApi.events.onNetworkActorSyncBroadcastRequested(onNetworkActorSyncBroadcastRequested)",
            network_source,
        )
        self.assertIn("editorApi.off(networkActorSyncBroadcastCallbackToken)", network_source)
        self.assertNotIn("coronaEventBus.on('actor-sync-broadcast'", network_source)
        self.assertNotIn("coronaEventBus.off('actor-sync-broadcast'", network_source)
        self.assertNotIn("event === 'actor-sync-broadcast'", event_bus_source)

    def test_network_actor_mutation_broadcasts_are_cpp_defined_events(self):
        source = self._editor_api_source()
        bridge_source = self._frontend_bridge_source()
        python_api_source = (
            self._repo_root()
            / "editor"
            / "api"
            / "editor_api.py"
        ).read_text(encoding="utf-8")
        corona_editor_source = (
            self._repo_root()
            / "editor"
            / "runtime"
            / "editor_host.py"
        ).read_text(encoding="utf-8")
        network_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "views"
            / "sidebar"
            / "Network.vue"
        ).read_text(encoding="utf-8")
        event_bus_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "utils"
            / "eventBus.js"
        ).read_text(encoding="utf-8")

        for event_name, js_wrapper, python_wrapper, legacy_name, handler_name, token_name in (
            (
                "Network.actorTransformSyncBroadcastRequested",
                "events.onNetworkActorTransformSyncBroadcastRequested",
                "events.on_network_actor_transform_sync_broadcast_requested",
                "actor-transform-sync-broadcast",
                "onNetworkActorTransformSyncBroadcastRequested",
                "networkActorTransformSyncBroadcastCallbackToken",
            ),
            (
                "Network.actorStateSyncBroadcastRequested",
                "events.onNetworkActorStateSyncBroadcastRequested",
                "events.on_network_actor_state_sync_broadcast_requested",
                "actor-state-sync-broadcast",
                "onNetworkActorStateSyncBroadcastRequested",
                "networkActorStateSyncBroadcastCallbackToken",
            ),
            (
                "Network.actorDeleteSyncBroadcastRequested",
                "events.onNetworkActorDeleteSyncBroadcastRequested",
                "events.on_network_actor_delete_sync_broadcast_requested",
                "actor-delete-sync-broadcast",
                "onNetworkActorDeleteSyncBroadcastRequested",
                "networkActorDeleteSyncBroadcastCallbackToken",
            ),
        ):
            with self.subTest(event=event_name):
                self.assertIn(
                    f'{{"{event_name}", EditorApiValueType::Object, all_callers(), "{js_wrapper}", "{python_wrapper}"}}',
                    source,
                )
                self.assertIn(
                    f"{handler_name}: (callback) => register_manifest_editor_api_callback('{js_wrapper}', callback)",
                    bridge_source,
                )
                self.assertIn(
                    f'return _register_manifest_editor_api_event_callback("{python_wrapper}", callback)',
                    python_api_source,
                )
                self.assertIn(
                    f'"{legacy_name}": ("{python_wrapper}", lambda values: values[0] if len(values) > 0 else {{}}),',
                    corona_editor_source,
                )
                self.assertIn(f"editorApi.events.{handler_name}({handler_name})", network_source)
                self.assertIn(f"editorApi.off({token_name})", network_source)
                self.assertNotIn(f"coronaEventBus.on('{legacy_name}'", network_source)
                self.assertNotIn(f"coronaEventBus.off('{legacy_name}'", network_source)
                self.assertNotIn(f"event === '{legacy_name}'", event_bus_source)

    def test_log_batch_is_cpp_defined_event(self):
        source = self._editor_api_source()
        bridge_source = self._frontend_bridge_source()
        python_api_source = (
            self._repo_root()
            / "editor"
            / "api"
            / "editor_api.py"
        ).read_text(encoding="utf-8")
        corona_editor_source = (
            self._repo_root()
            / "editor"
            / "runtime"
            / "editor_host.py"
        ).read_text(encoding="utf-8")
        log_view_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "views"
            / "sidebar"
            / "LogView.vue"
        ).read_text(encoding="utf-8")
        event_bus_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "utils"
            / "eventBus.js"
        ).read_text(encoding="utf-8")

        for snippet in (
            '{"Editor.logBatch", EditorApiValueType::Array, all_callers(), '
            '"events.onLogBatch", "events.on_log_batch"}',
            "onLogBatch: (callback) => register_manifest_editor_api_callback("
            "'events.onLogBatch', callback)",
            'return _register_manifest_editor_api_event_callback("events.on_log_batch", callback)',
            '"log-batch": ("events.on_log_batch", lambda values: values[0] if len(values) > 0 else []),',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source + bridge_source + python_api_source + corona_editor_source)

        self.assertIn("editorApi.events.onLogBatch(onLogBatch)", log_view_source)
        self.assertIn("editorApi.off(logBatchCallbackToken)", log_view_source)
        self.assertNotIn("coronaEventBus.on('log-batch'", log_view_source)
        self.assertNotIn("coronaEventBus.off('log-batch'", log_view_source)
        self.assertNotIn("event === 'log-batch'", event_bus_source)

    def test_ai_chunk_is_cpp_defined_event(self):
        source = self._editor_api_source()
        bridge_source = self._frontend_bridge_source()
        python_api_source = (
            self._repo_root()
            / "editor"
            / "api"
            / "editor_api.py"
        ).read_text(encoding="utf-8")
        corona_editor_source = (
            self._repo_root()
            / "editor"
            / "runtime"
            / "editor_host.py"
        ).read_text(encoding="utf-8")
        event_bus_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "utils"
            / "eventBus.js"
        ).read_text(encoding="utf-8")

        for snippet in (
            '{"AI.chunk", EditorApiValueType::String, all_callers(), '
            '"events.onAiChunk", "events.on_ai_chunk"}',
            "onAiChunk: (callback) => register_manifest_editor_api_callback("
            "'events.onAiChunk', callback)",
            'return _register_manifest_editor_api_event_callback("events.on_ai_chunk", callback)',
            '"ai-chunk": ("events.on_ai_chunk", lambda values: values[0] if len(values) > 0 else ""),',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source + bridge_source + python_api_source + corona_editor_source)

        self.assertNotIn("event === 'ai-chunk'", event_bus_source)

    def test_project_opened_event_is_emitted_by_cpp_callback_registry(self):
        handler_source = self._handler_source()
        start = handler_source.find('{"open_project", [](const NativeRequest& request, const NativeContext&)')
        self.assertGreaterEqual(start, 0)
        end = handler_source.find('{"open_project_file"', start)
        self.assertGreater(end, start)
        body = handler_source[start:end]

        self.assertIn('emit_editor_api_event("ProjectLauncher.projectOpened"', body)
        self.assertIn('{"path", state.project_path}', body)
        self.assertLess(
            body.find('emit_editor_api_event("ProjectLauncher.projectOpened"'),
            body.rfind('return native_success({'),
        )

    def test_lanchat_event_is_defined_and_emitted_by_cpp_callback_registry(self):
        source = self._editor_api_source()
        handler_source = self._handler_source()
        bridge_source = self._frontend_bridge_source()
        python_api_source = (
            self._repo_root()
            / "editor"
            / "api"
            / "editor_api.py"
        ).read_text(encoding="utf-8")

        for snippet in (
            '{"LANChat.event", EditorApiValueType::Object, all_callers(), '
            '"events.onLanChatEvent", "events.on_lan_chat_event"}',
            "onLanChatEvent: (callback) => register_manifest_editor_api_callback("
            "'events.onLanChatEvent', callback)",
            'return _register_manifest_editor_api_event_callback("events.on_lan_chat_event", callback)',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source + bridge_source + python_api_source)

        emit_body = re.search(
            r"void emit_lanchat_event_json\(const std::string& event_json\) \{.*?\n\}",
            handler_source,
            re.S,
        )
        self.assertIsNotNone(emit_body)
        body = emit_body.group(0)
        self.assertIn("nlohmann::json::parse(event_json)", body)
        self.assertIn('emit_editor_api_event("LANChat.event", event_payload)', body)
        self.assertNotIn("__coronaEmit", body)
        self.assertNotIn("lanchat-event", body)

        event_bus_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "utils"
            / "eventBus.js"
        ).read_text(encoding="utf-8")
        bridge_source = self._frontend_bridge_source()
        self.assertNotIn("event === 'lanchat-event'", event_bus_source)
        self.assertNotIn("__coronaEmit('lanchat-event')", bridge_source)

    def test_lanchat_frontend_consumer_uses_cpp_defined_event_wrapper(self):
        app_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "App.vue"
        ).read_text(encoding="utf-8")

        self.assertIn("editorApi.events.onLanChatEvent(onLanChatEvent)", app_source)
        self.assertIn("lanchat.handleEvent(payload)", app_source)
        self.assertIn("if (!isStandalonePanel.value)", app_source)
        self.assertIn("editorApi.off(callbackToken)", app_source)
        self.assertNotIn("coronaEventBus.on('lanchat-event'", app_source)
        self.assertNotIn("coronaEventBus.off('lanchat-event'", app_source)

    def test_realtime_focus_and_pick_events_are_defined_and_emitted_by_cpp_callback_registry(self):
        source = self._editor_api_source()
        realtime_source = (
            self._repo_root()
            / "src"
            / "systems"
            / "ui"
            / "cef"
            / "cef_realtime_bridge.cpp"
        ).read_text(encoding="utf-8")
        bridge_source = self._frontend_bridge_source()
        python_api_source = (
            self._repo_root()
            / "editor"
            / "api"
            / "editor_api.py"
        ).read_text(encoding="utf-8")

        for snippet in (
            '{"SceneTools.focusPoseResult", EditorApiValueType::Object, all_callers(), '
            '"events.onFocusPoseResult", "events.on_focus_pose_result"}',
            '{"SceneTools.actorPickResult", EditorApiValueType::Object, all_callers(), '
            '"events.onActorPickResult", "events.on_actor_pick_result"}',
            "onFocusPoseResult: (callback) => register_manifest_editor_api_callback("
            "'events.onFocusPoseResult', callback)",
            "onActorPickResult: (callback) => register_manifest_editor_api_callback("
            "'events.onActorPickResult', callback)",
            'return _register_manifest_editor_api_event_callback("events.on_focus_pose_result", callback)',
            'return _register_manifest_editor_api_event_callback("events.on_actor_pick_result", callback)',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source + bridge_source + python_api_source)

        focus_body = re.search(
            r"void send_focus_pose_result\(const CefRefPtr<CefFrame>& frame,.*?\n\}",
            realtime_source,
            re.S,
        )
        self.assertIsNotNone(focus_body)
        focus_source = focus_body.group(0)
        self.assertIn('emit_editor_api_event("SceneTools.focusPoseResult", event_payload)', focus_source)
        self.assertIn('event_payload["request_id"] = request_id;', focus_source)
        self.assertIn("(void)frame;", focus_source)
        self.assertNotIn("if (!frame)", focus_source)
        self.assertNotIn("__coronaEmit", focus_source)
        self.assertNotIn("focus-pose-result", focus_source)

        pick_body = re.search(
            r"void send_viewport_pick_result\(const CefRefPtr<CefFrame>& frame,.*?\n\}",
            realtime_source,
            re.S,
        )
        self.assertIsNotNone(pick_body)
        pick_source = pick_body.group(0)
        self.assertIn('emit_editor_api_event_to_frame("SceneTools.actorPickResult", payload, frame)', pick_source)
        self.assertNotIn("if (!frame)", pick_source)
        self.assertNotIn("__coronaEmit", pick_source)
        self.assertNotIn("actor-pick-result", pick_source)

    def test_realtime_focus_and_pick_frontend_consumers_use_cpp_defined_event_wrappers(self):
        scene_bar_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "views"
            / "sidebar"
            / "SceneBar.vue"
        ).read_text(encoding="utf-8")
        main_page_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "views"
            / "layout"
            / "MainPage.vue"
        ).read_text(encoding="utf-8")

        self.assertIn("editorApi.sceneTools.focusActor", scene_bar_source)
        self.assertNotIn("editorApi.events.onFocusPoseResult", scene_bar_source)
        self.assertNotIn("handleFocusPoseResult", scene_bar_source)
        self.assertNotIn("coronaEventBus.on('focus-pose-result'", scene_bar_source)
        self.assertNotIn("coronaEventBus.off('focus-pose-result'", scene_bar_source)

        self.assertIn("editorApi.events.onActorPickResult(handleActorPickResult)", main_page_source)
        self.assertIn("editorApi.off(actorPickResultCallbackToken)", main_page_source)
        self.assertNotIn("coronaEventBus.on('actor-pick-result'", main_page_source)
        self.assertNotIn("coronaEventBus.off('actor-pick-result'", main_page_source)

    def test_scene_tab_events_are_cpp_defined_and_python_can_emit_them(self):
        source = self._editor_api_source()
        bridge_source = self._frontend_bridge_source()
        python_api_source = (
            self._repo_root()
            / "editor"
            / "api"
            / "editor_api.py"
        ).read_text(encoding="utf-8")
        corona_editor_source = (
            self._repo_root()
            / "editor"
            / "runtime"
            / "editor_host.py"
        ).read_text(encoding="utf-8")
        py_bind_source = (
            self._repo_root()
            / "src"
            / "systems"
            / "ui"
            / "cef"
            / "cef_py_bind.cpp"
        ).read_text(encoding="utf-8")

        for snippet in (
            '{"SceneTools.sceneAdded", EditorApiValueType::Object, all_callers(), '
            '"events.onSceneAdded", "events.on_scene_added"}',
            '{"SceneTools.sceneRenamed", EditorApiValueType::Object, all_callers(), '
            '"events.onSceneRenamed", "events.on_scene_renamed"}',
            "onSceneAdded: (callback) => register_manifest_editor_api_callback("
            "'events.onSceneAdded', callback)",
            "onSceneRenamed: (callback) => register_manifest_editor_api_callback("
            "'events.onSceneRenamed', callback)",
            'return _register_manifest_editor_api_event_callback("events.on_scene_added", callback)',
            'return _register_manifest_editor_api_event_callback("events.on_scene_renamed", callback)',
            "def _emit_cpp_editor_api_event(event_name, payload):",
            "def _emit_manifest_cpp_editor_api_event(wrapper_name, payload):",
            "CoronaEngine.emit_editor_api_event(event_name, json.dumps(payload))",
            'm.def("emit_editor_api_event"',
            '"scene-add": ("events.on_scene_added"',
            '"scene-rename": ("events.on_scene_renamed"',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source + bridge_source + python_api_source + corona_editor_source + py_bind_source)

    def test_main_page_scene_tab_events_use_cpp_defined_event_wrappers(self):
        main_page_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "views"
            / "layout"
            / "MainPage.vue"
        ).read_text(encoding="utf-8")

        self.assertIn("editorApi.events.onSceneAdded(onSceneAddedEvent)", main_page_source)
        self.assertIn("editorApi.events.onSceneRenamed(onSceneRenamedEvent)", main_page_source)
        self.assertIn("editorApi.off(sceneAddedCallbackToken)", main_page_source)
        self.assertIn("editorApi.off(sceneRenamedCallbackToken)", main_page_source)
        self.assertIn("const onSceneAddedEvent = (payload) => addSceneTab(payload?.name, payload?.route);", main_page_source)
        self.assertIn(
            "const onSceneRenamedEvent = (payload) => renameSceneTab(payload?.old_path, payload?.new_path, payload?.name);",
            main_page_source,
        )
        self.assertNotIn("coronaEventBus.on('scene-add'", main_page_source)
        self.assertNotIn("coronaEventBus.on('scene-rename'", main_page_source)
        self.assertNotIn("coronaEventBus.off('scene-add'", main_page_source)
        self.assertNotIn("coronaEventBus.off('scene-rename'", main_page_source)

    def test_scene_tab_events_are_not_relayed_by_legacy_event_bus(self):
        event_bus_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "utils"
            / "eventBus.js"
        ).read_text(encoding="utf-8")

        self.assertNotIn("event === 'scene-add'", event_bus_source)
        self.assertNotIn("event === 'scene-rename'", event_bus_source)

    def test_scene_bar_uses_cpp_defined_scene_tree_changed_event_wrapper(self):
        scene_bar_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "views"
            / "sidebar"
            / "SceneBar.vue"
        ).read_text(encoding="utf-8")

        self.assertIn("editorApi.events.onSceneTreeChanged(onSceneTreeChangedEvent)", scene_bar_source)
        self.assertIn("editorApi.off(sceneTreeChangedCallbackToken)", scene_bar_source)
        self.assertIn("const sceneName = payload?.scene ?? payload;", scene_bar_source)
        self.assertNotIn("coronaEventBus.on('scene-tree-changed', onSceneTreeChangedEvent)", scene_bar_source)
        self.assertNotIn("coronaEventBus.off('scene-tree-changed', onSceneTreeChangedEvent)", scene_bar_source)

    def test_network_panel_uses_cpp_defined_scene_tree_changed_event_wrapper(self):
        network_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "views"
            / "sidebar"
            / "Network.vue"
        ).read_text(encoding="utf-8")

        self.assertIn("editorApi.events.onSceneTreeChanged(onSceneTreeChangedEvent)", network_source)
        self.assertIn("editorApi.off(sceneTreeChangedCallbackToken)", network_source)
        self.assertIn("const sceneName = payload?.scene ?? payload;", network_source)
        self.assertNotIn("coronaEventBus.on('scene-tree-changed'", network_source)
        self.assertNotIn("coronaEventBus.off('scene-tree-changed'", network_source)

    def test_scene_bar_uses_cpp_defined_actor_changed_event_wrapper(self):
        scene_bar_source = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "views"
            / "sidebar"
            / "SceneBar.vue"
        ).read_text(encoding="utf-8")

        self.assertIn("editorApi.events.onActorChanged(onActorChangeEvent)", scene_bar_source)
        self.assertIn("editorApi.off(actorChangedCallbackToken)", scene_bar_source)
        self.assertIn("const type = payload?.actor_type ?? payload?.type ?? payload;", scene_bar_source)
        self.assertIn("const sceneId = payload?.scene ?? maybeSceneId;", scene_bar_source)
        self.assertNotIn("coronaEventBus.on('actor-change', onActorChangeEvent)", scene_bar_source)
        self.assertNotIn("coronaEventBus.off('actor-change', onActorChangeEvent)", scene_bar_source)

    def test_scene_tools_schema_apis_use_frontend_typed_wrappers(self):
        bridge_source = self._frontend_bridge_source()

        for snippet in (
            "sceneTools: {",
            "createActor: (sceneName, objPath, actorType = 'model', actorData = null)",
            "removeActor: (sceneName, actorName)",
            "renameActor: (sceneName, actorName, name)",
            "focusActor: (sceneName, actorName, cameraName)",
            "setRenderBackend: (mode, sceneName = null, cameraId = null)",
            "getRenderBackend: (sceneName = null, cameraId = null)",
            "setVisionRenderMode: (sceneName, cameraId = null, mode = 'path_tracing')",
            "getVisionRenderMode: (sceneName, cameraId = null)",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)

    def test_scene_tools_scene_camera_editor_apis_have_explicit_schema(self):
        source = self._editor_api_source()

        expected = {
            "SceneTools.create_scene": ("kSceneNameParam", "EditorApiValueType::Object"),
            "SceneTools.list_scene_tree": ("kSceneNameParam", "EditorApiValueType::Object"),
            "SceneTools.reload_scene": ("kSceneToolsReloadSceneParams", "EditorApiValueType::Object"),
            "SceneTools.create_camera_view": ("kSceneToolsCreateCameraViewParams", "EditorApiValueType::Object"),
            "SceneTools.open_camera_view": ("kSceneCameraParams", "EditorApiValueType::Object"),
            "SceneTools.close_camera_view": ("kSceneCameraParams", "EditorApiValueType::Object"),
            "SceneTools.rename_camera_view": ("kSceneToolsRenameCameraViewParams", "EditorApiValueType::Object"),
            "SceneTools.list_camera_views": ("kSceneNameParam", "EditorApiValueType::Object"),
            "SceneTools.update_camera_view": ("kSceneToolsUpdateCameraViewParams", "EditorApiValueType::Object"),
            "SceneTools.delete_camera": ("kSceneCameraParams", "EditorApiValueType::Object"),
        }
        for api_name, (params_name, return_type) in expected.items():
            module, method = api_name.split(".", 1)
            with self.subTest(api_name=api_name):
                plain_macro = f"EDITOR_API_METHOD_SCHEMA({module}, {method}, {params_name}, {return_type})"
                wrapped_macro = f"EDITOR_API_METHOD_SCHEMA_WRAPPED({module}, {method}, {params_name},"
                wrapped_callers_macro = f"EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS({module}, {method}, {params_name},"
                caller_macro = f"EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS({module}, {method}, {params_name},"
                self.assertTrue(
                    plain_macro in source or wrapped_macro in source or caller_macro in source,
                    f"{api_name} should define an explicit C++ method schema",
                )

    def test_scene_tools_camera_lifecycle_apis_are_native_handlers(self):
        source = self._handler_source()
        for method in (
            "create_camera_view",
            "open_camera_view",
            "close_camera_view",
            "rename_camera_view",
            "update_camera_view",
            "delete_camera",
        ):
            with self.subTest(method=method):
                self.assertNotIn(f'{{"{method}", script_method}}', source)
                self.assertIn(f'{{"{method}", [](const NativeRequest& request', source)

    def test_scene_tools_scene_camera_apis_use_frontend_typed_wrappers(self):
        bridge_source = self._frontend_bridge_source()

        for snippet in (
            "createCameraView: (sceneName, name = null) =>\n    editorApi.sceneTools.createCameraView",
            "openCameraView: (sceneName, cameraId) =>\n    editorApi.sceneTools.openCameraView",
            "closeCameraView: (sceneName, cameraId) =>\n    editorApi.sceneTools.closeCameraView",
            "renameCameraView: (sceneName, cameraId, name) =>\n    editorApi.sceneTools.renameCameraView",
            "listCameraViews: (sceneName) =>\n    editorApi.sceneTools.listCameraViews",
            "updateCameraView: (sceneName, cameraId, state) =>\n    editorApi.sceneTools.updateCameraView",
            "deleteCamera: (sceneName, cameraId) =>\n    editorApi.sceneTools.deleteCamera",
            "reloadScene: (sceneName, projectPath = '') =>\n    editorApi.sceneTools.reloadScene",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)

        for snippet in (
            "removeActor: (sceneName, actorName) =>\n    editorApi.sceneTools.removeActor",
            "renameActor: (sceneName, actorName, name) =>\n    editorApi.sceneTools.renameActor",
            "setRenderBackend: (mode, sceneName = null, cameraId = null) =>\n    editorApi.sceneTools.setRenderBackend",
            "getRenderBackend: (sceneName = null, cameraId = null) =>\n    editorApi.sceneTools.getRenderBackend",
            "setVisionRenderMode: (sceneName, cameraId = null, mode = 'path_tracing') =>\n    editorApi.sceneTools.setVisionRenderMode",
            "getVisionRenderMode: (sceneName, cameraId = null) =>\n    editorApi.sceneTools.getVisionRenderMode",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)

    def test_scene_management_wrappers_validate_cpp_method_manifest(self):
        bridge_source = self._frontend_bridge_source()

        for snippet in (
            "listActorTree: (sceneName) => call_manifest_editor_api('scene.listActorTree', [sceneName])",
            "createScene: (sceneName) => call_manifest_editor_api('sceneTools.createScene', [sceneName])",
            "listSceneTree: (sceneName) => call_manifest_editor_api('sceneTools.listSceneTree', [sceneName])",
            "createActor: (sceneName, objPath, actorType = 'model', actorData = null) =>",
            "call_manifest_editor_api('sceneTools.createActor',",
            "removeActor: (sceneName, actorName) =>",
            "call_manifest_editor_api('sceneTools.removeActor', [sceneName, actorName])",
            "openActor: (sceneName, actorName) =>",
            "call_manifest_editor_api('sceneTools.openActor', [sceneName, actorName])",
            "createCameraView: (sceneName, name = null) =>",
            "call_manifest_editor_api('sceneTools.createCameraView', [sceneName, name])",
            "listCameraViews: (sceneName) =>",
            "call_manifest_editor_api('sceneTools.listCameraViews', [sceneName])",
            "getScene: (sceneId) => call_manifest_editor_api('sceneDatas.getScene', [sceneId])",
            "getActor: (sceneId, actorId) => call_manifest_editor_api('sceneDatas.getActor', [sceneId, actorId])",
            "actorOperation: (sceneName, actorName, operation, vector) =>",
            "call_manifest_editor_api('sceneDatas.actorOperation', [sceneName, actorName, operation, vector])",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)

    def test_scene_tools_frontend_wrappers_are_manifest_driven(self):
        bridge_source = self._frontend_bridge_source()
        scene_start = bridge_source.find("scene: {")
        scene_end = bridge_source.find("scratch: {", scene_start)
        self.assertGreaterEqual(scene_start, 0)
        self.assertGreater(scene_end, scene_start)
        scene_section = bridge_source[scene_start:scene_end]
        self.assertIn("call_manifest_editor_api('scene.listActorTree'", scene_section)
        self.assertNotIn("SceneTools.", scene_section)

        start = bridge_source.find("sceneTools: {")
        end = bridge_source.find("main: {", start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        scene_tools_section = bridge_source[start:end]
        expected_wrappers = (
            "sceneTools.createScene",
            "sceneTools.listSceneTree",
            "sceneTools.reloadScene",
            "sceneTools.createActor",
            "sceneTools.removeActor",
            "sceneTools.renameActor",
            "sceneTools.openActor",
            "sceneTools.selectActor",
            "sceneTools.focusActor",
            "sceneTools.setRenderBackend",
            "sceneTools.getRenderBackend",
            "sceneTools.setVisionRenderMode",
            "sceneTools.getVisionRenderMode",
            "sceneTools.createCameraView",
            "sceneTools.openCameraView",
            "sceneTools.closeCameraView",
            "sceneTools.renameCameraView",
            "sceneTools.listCameraViews",
            "sceneTools.updateCameraView",
            "sceneTools.deleteCamera",
            "sceneTools.sunDirection",
            "sceneTools.floorGrid",
            "sceneTools.setPhysicsParams",
            "sceneTools.getPhysicsParams",
            "sceneTools.selectScreenshotPath",
            "sceneTools.saveScreenshot",
            "sceneTools.setOutputMode",
            "sceneTools.getOutputMode",
            "sceneTools.setShadowCascadeDebug",
            "sceneTools.getShadowCascadeDebug",
            "sceneTools.setSsaoEnabled",
            "sceneTools.getSsaoEnabled",
            "sceneTools.isVisionAvailable",
            "sceneTools.loadVisionScene",
            "sceneTools.pickActor",
            "sceneTools.playAudio",
            "sceneTools.stopAudio",
            "sceneTools.actorPlayAudio",
            "sceneTools.actorStopAudio",
        )
        for wrapper in expected_wrappers:
            with self.subTest(wrapper=wrapper):
                if wrapper == "sceneTools.selectActor":
                    self.assertIn("'sceneTools.selectActor',", scene_tools_section)
                else:
                    self.assertIn(f"call_manifest_editor_api('{wrapper}'", scene_tools_section)
        self.assertNotIn("SceneTools.", scene_tools_section)

    def test_scene_tools_environment_render_audio_editor_apis_have_explicit_schema(self):
        source = self._editor_api_source()

        expected = {
            "SceneTools.sun_direction": ("kSceneToolsSunDirectionParams", "EditorApiValueType::Object"),
            "SceneTools.floor_grid": ("kSceneToolsFloorGridParams", "EditorApiValueType::Object"),
            "SceneTools.set_physics_params": ("kSceneToolsSetPhysicsParams", "EditorApiValueType::Object"),
            "SceneTools.get_physics_params": ("kSceneNameParam", "EditorApiValueType::Object"),
            "SceneTools.select_screenshot_path": ("kSceneToolsCameraOptionalParams", "EditorApiValueType::Object"),
            "SceneTools.save_screenshot": ("kSceneToolsSaveScreenshotParams", "EditorApiValueType::Object"),
            "SceneTools.set_output_mode": ("kSceneToolsSetOutputModeParams", "EditorApiValueType::Object"),
            "SceneTools.get_output_mode": ("kSceneToolsCameraOptionalParams", "EditorApiValueType::Object"),
            "SceneTools.set_shadow_cascade_debug": ("kSceneToolsSetCameraBoolParams", "EditorApiValueType::Object"),
            "SceneTools.get_shadow_cascade_debug": ("kSceneToolsCameraOptionalParams", "EditorApiValueType::Object"),
            "SceneTools.set_ssao_enabled": ("kSceneToolsSetCameraBoolParams", "EditorApiValueType::Object"),
            "SceneTools.get_ssao_enabled": ("kSceneToolsCameraOptionalParams", "EditorApiValueType::Object"),
            "SceneTools.is_vision_available": ("kNoParams", "EditorApiValueType::Object"),
            "SceneTools.load_vision_scene": ("kPathParam", "EditorApiValueType::Object"),
            "SceneTools.pick_actor_at_pixel": ("kSceneToolsPickActorParams", "EditorApiValueType::Object"),
            "SceneTools.play_audio": ("kSceneToolsPlayAudioParams", "EditorApiValueType::Object"),
            "SceneTools.stop_audio": ("kResourceIdParam", "EditorApiValueType::Object"),
            "SceneTools.actor_play_audio": ("kSceneToolsActorPlayAudioParams", "EditorApiValueType::Object"),
            "SceneTools.actor_stop_audio": ("kActorNameParam", "EditorApiValueType::Object"),
        }
        for api_name, (params_name, return_type) in expected.items():
            module, method = api_name.split(".", 1)
            with self.subTest(api_name=api_name):
                plain_macro = f"EDITOR_API_METHOD_SCHEMA({module}, {method}, {params_name}, {return_type})"
                wrapped_macro = f"EDITOR_API_METHOD_SCHEMA_WRAPPED({module}, {method}, {params_name},"
                caller_macro = f"EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS({module}, {method}, {params_name},"
                self.assertTrue(
                    plain_macro in source or wrapped_macro in source or caller_macro in source,
                    f"{api_name} should define an explicit C++ method schema",
                )

    def test_scene_tools_environment_render_audio_apis_use_frontend_typed_wrappers(self):
        bridge_source = self._frontend_bridge_source()

        for snippet in (
            "sunDirection: (sceneName, enable, direction) =>\n    editorApi.sceneTools.sunDirection",
            "floorGrid: (sceneName, enabled) =>\n    editorApi.sceneTools.floorGrid",
            "setPhysicsParams: (sceneName, params) =>\n    editorApi.sceneTools.setPhysicsParams",
            "getPhysicsParams: (sceneName) => editorApi.sceneTools.getPhysicsParams(sceneName)",
            "selectScreenshotPath: (sceneName, cameraName) =>\n    editorApi.sceneTools.selectScreenshotPath",
            "saveScreenshot: (sceneName, path, cameraName) =>\n    editorApi.sceneTools.saveScreenshot",
            "setOutputMode: (sceneName, cameraName, mode) =>\n    editorApi.sceneTools.setOutputMode",
            "getOutputMode: (sceneName, cameraName) =>\n    editorApi.sceneTools.getOutputMode",
            "setShadowCascadeDebug: (sceneName, cameraName, enabled) =>\n    editorApi.sceneTools.setShadowCascadeDebug",
            "getShadowCascadeDebug: (sceneName, cameraName) =>\n    editorApi.sceneTools.getShadowCascadeDebug",
            "setSsaoEnabled: (sceneName, cameraName, enabled) =>\n    editorApi.sceneTools.setSsaoEnabled",
            "getSsaoEnabled: (sceneName, cameraName) =>\n    editorApi.sceneTools.getSsaoEnabled",
            "isVisionAvailable: () => editorApi.sceneTools.isVisionAvailable()",
            "loadVisionScene: (path) => editorApi.sceneTools.loadVisionScene(path)",
            "pickActor: (sceneName, x, y, vpWidth, vpHeight) =>\n    editorApi.sceneTools.pickActor",
            "playAudio: (resourceId, loop) =>\n    editorApi.sceneTools.playAudio",
            "stopAudio: (resourceId) =>\n    editorApi.sceneTools.stopAudio",
            "actorPlayAudio: (actorName, loop = false) =>\n    editorApi.sceneTools.actorPlayAudio",
            "actorStopAudio: (actorName) =>\n    editorApi.sceneTools.actorStopAudio",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)

    def test_scene_datas_editor_apis_have_explicit_schema(self):
        source = self._editor_api_source()

        expected = {
            "SceneDatas.get_scene": ("kSceneNameOptionalParam", "EditorApiValueType::Object"),
            "SceneDatas.get_actor": ("kSceneActorParams", "EditorApiValueType::Object"),
            "SceneDatas.actor_operation": ("kSceneDatasActorOperationParams", "EditorApiValueType::Object"),
            "SceneDatas.save_actor": ("kSceneActorParams", "EditorApiValueType::Object"),
            "SceneDatas.select_model_file": ("kSceneDatasSelectModelFileParams", "EditorApiValueType::String"),
        }
        for api_name, (params_name, return_type) in expected.items():
            module, method = api_name.split(".", 1)
            with self.subTest(api_name=api_name):
                plain_macro = f"EDITOR_API_METHOD_SCHEMA({module}, {method}, {params_name}, {return_type})"
                wrapped_macro = f"EDITOR_API_METHOD_SCHEMA_WRAPPED({module}, {method}, {params_name},"
                caller_macro = f"EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS({module}, {method}, {params_name},"
                self.assertTrue(
                    plain_macro in source or wrapped_macro in source or caller_macro in source,
                    f"{api_name} should define an explicit C++ method schema",
                )

    def test_scene_datas_apis_use_frontend_typed_wrappers(self):
        bridge_source = self._frontend_bridge_source()

        for snippet in (
            "sceneDatas: {",
            "getScene: (sceneId) => call_manifest_editor_api('sceneDatas.getScene', [sceneId])",
            "getActor: (sceneId, actorId) => call_manifest_editor_api('sceneDatas.getActor', [sceneId, actorId])",
            "actorOperation: (sceneName, actorName, operation, vector) =>",
            "call_manifest_editor_api('sceneDatas.actorOperation', [sceneName, actorName, operation, vector])",
            "call_manifest_editor_api('sceneDatas.saveActor', [sceneName, actorName])",
            "selectModelFile: (sceneId, actorId, fileType) =>",
            "call_manifest_editor_api('sceneDatas.selectModelFile', [sceneId, actorId, fileType])",
            "actorOperation: (scene_name, actor_name, operation, vector) =>\n    editorApi.sceneDatas.actorOperation",
            "setActorCameraLock: (sceneName, actorName, cameraLock) =>\n    editorApi.sceneTools.setActorCameraLock",
            "selectModelFileDialog: (sceneName, actorName, fileType = 'model') =>\n    editorApi.sceneTools.selectModelFile",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)

        for snippet in (
            "getScene: (sceneId) => editorApi.scene.getSnapshot(sceneId)",
            "getActor: async (sceneName, actorName) =>",
            "editorApi.scene.getSnapshot(sceneName)",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)

    def test_scene_datas_frontend_wrappers_are_manifest_driven(self):
        bridge_source = self._frontend_bridge_source()
        start = bridge_source.find("sceneDatas: {")
        self.assertGreaterEqual(start, 0)
        end = bridge_source.find("\n  },\n};", start)
        self.assertGreater(end, start)
        scene_datas_section = bridge_source[start:end]

        self.assertIn("call_manifest_editor_api('sceneDatas.getScene'", scene_datas_section)
        self.assertIn("call_manifest_editor_api('sceneDatas.getActor'", scene_datas_section)
        self.assertIn("call_manifest_editor_api('sceneDatas.actorOperation'", scene_datas_section)
        self.assertIn("call_manifest_editor_api('sceneDatas.saveActor'", scene_datas_section)
        self.assertIn("call_manifest_editor_api('sceneDatas.selectModelFile'", scene_datas_section)
        self.assertNotIn("SceneDatas.", scene_datas_section)

    def test_scene_tree_and_property_handlers_respect_scene_route_args(self):
        source = self._handler_source()

        self.assertIn("NativeEditorScene* scene_for_request_route", source)
        expected = (
            '{"get_scene", [](const NativeRequest& request',
            '{"get_actor", [](const NativeRequest& request',
            '{"actor_operation", [](const NativeRequest& request',
            '{"save_actor", [](const NativeRequest& request',
            '{"list_scene_tree", [](const NativeRequest& request',
            '{"list_actor_tree", [](const NativeRequest& request',
            '{"rename_actor", [](const NativeRequest& request',
            '{"list_camera_views", [](const NativeRequest& request',
        )
        for snippet in expected:
            with self.subTest(snippet=snippet):
                start = source.find(snippet)
                self.assertGreaterEqual(start, 0)
                body = source[start:source.find("}},", start)]
                self.assertIn("scene_for_request_route(request)", body)

    def test_project_launcher_editor_apis_have_explicit_schema(self):
        source = self._editor_api_source()

        expected = {
            "ProjectLauncher.browse_folder": ("kPathOptionalParam", "EditorApiValueType::String"),
            "ProjectLauncher.create_multiplayer_project": ("kObjectPayloadParam", "EditorApiValueType::Object"),
            "ProjectLauncher.create_project": ("kObjectPayloadParam", "EditorApiValueType::String"),
            "ProjectLauncher.create_world_project": ("kObjectPayloadParam", "EditorApiValueType::Object"),
            "ProjectLauncher.get_default_project_path": ("kNoParams", "EditorApiValueType::String"),
            "ProjectLauncher.get_project_load_status": ("kNoParams", "EditorApiValueType::Object"),
            "ProjectLauncher.get_recent_projects": ("kNoParams", "EditorApiValueType::Array"),
            "ProjectLauncher.open_project": ("kOpenProjectParams", "EditorApiValueType::Object"),
            "ProjectLauncher.open_project_file": ("kNoParams", "EditorApiValueType::Object"),
            "ProjectLauncher.set_project_mode": ("kObjectPayloadParam", "EditorApiValueType::Boolean"),
        }
        for api_name, (params_name, return_type) in expected.items():
            module, method = api_name.split(".", 1)
            with self.subTest(api_name=api_name):
                plain_macro = f"EDITOR_API_METHOD_SCHEMA({module}, {method}, {params_name}, {return_type})"
                wrapped_macro = f"EDITOR_API_METHOD_SCHEMA_WRAPPED({module}, {method}, {params_name},"
                self.assertTrue(
                    plain_macro in source or wrapped_macro in source or wrapped_callers_macro in source,
                    f"{api_name} should define an explicit C++ method schema",
                )

    def test_project_launcher_apis_use_frontend_typed_wrappers(self):
        bridge_source = self._frontend_bridge_source()

        for snippet in (
            "getDefaultProjectPath: () => editorApi.project.getDefaultProjectPath()",
            "browseFolder: (default_path) =>\n    editorApi.project.browseFolder",
            "openProjectFile: () => editorApi.project.openProjectFile()",
            "createProject: (projectData) =>\n    editorApi.project.createProject",
            "createWorldProject: (worldData) =>\n    editorApi.project.createWorldProject",
            "createMultiplayerProject: (projectData) =>\n    editorApi.project.createMultiplayerProject",
            "getProjectLoadStatus: () => editorApi.project.getProjectLoadStatus()",
            "openProject: async (projectPath, options = {}) => {",
            "const result = await editorApi.project.openProject(projectPath, { load_policy: loadPolicy })",
            "setProjectMode: (mode, settings) =>\n    editorApi.project.setProjectMode",
            "getRecentProjects: () => editorApi.project.getRecentProjects()",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)

    def test_project_launcher_frontend_wrappers_are_manifest_driven(self):
        bridge_source = self._frontend_bridge_source()
        start = bridge_source.find("project: {")
        end = bridge_source.find("scratch: {", start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        project_section = bridge_source[start:end]

        self.assertIn("call_manifest_editor_api", bridge_source)
        self.assertIn("call_manifest_editor_api('project.browseFolder'", project_section)
        self.assertIn("call_manifest_editor_api('project.openProject'", project_section)
        self.assertNotIn("ProjectLauncher.", project_section)

    def test_project_launcher_python_wrappers_are_manifest_driven(self):
        python_api_source = (
            self._repo_root() / "editor" / "api" / "editor_api.py"
        ).read_text(encoding="utf-8")
        start = python_api_source.find("class _ProjectApi")
        end = python_api_source.find("class _EditorApi", start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        project_section = python_api_source[start:end]

        self.assertIn("def _invoke_manifest_cpp_api(wrapper_path, args=None):", python_api_source)
        self.assertIn('_invoke_manifest_cpp_api("project.get_app_version", [])', project_section)
        self.assertNotIn("ProjectLauncher.", project_section)

    def test_project_settings_editor_apis_have_explicit_schema(self):
        source = self._editor_api_source()

        expected = {
            "ProjectSettings.browse_scene_file": ("kNoParams", "EditorApiValueType::Object"),
            "ProjectSettings.get_active_project_info": ("kNoParams", "EditorApiValueType::Object"),
            "ProjectSettings.save_active_project_info": ("kObjectPayloadParam", "EditorApiValueType::Object"),
        }
        for api_name, (params_name, return_type) in expected.items():
            module, method = api_name.split(".", 1)
            with self.subTest(api_name=api_name):
                plain_schema = f"EDITOR_API_METHOD_SCHEMA({module}, {method}, {params_name}, {return_type})"
                wrapped_schema_prefix = (
                    f"EDITOR_API_METHOD_SCHEMA_WRAPPED({module}, {method}, {params_name}, "
                )
                self.assertTrue(
                    plain_schema in source
                    or (wrapped_schema_prefix in source and return_type in source),
                    f"{api_name} must keep an explicit C++ schema",
                )

    def test_project_settings_apis_use_frontend_typed_wrappers(self):
        bridge_source = self._frontend_bridge_source()

        for snippet in (
            "projectSettings: {",
            "getActiveProjectInfo: () => call_manifest_editor_api('projectSettings.getActiveProjectInfo', [])",
            "saveActiveProjectInfo: (settings) =>",
            "browseSceneFile: () => call_manifest_editor_api('projectSettings.browseSceneFile', [])",
            "getActiveProjectInfo: () => editorApi.projectSettings.getActiveProjectInfo()",
            "saveActiveProjectInfo: (settings) =>\n    editorApi.projectSettings.saveActiveProjectInfo",
            "browseSceneFile: () => editorApi.projectSettings.browseSceneFile()",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)

    def test_corona_editor_app_api_has_explicit_schema_and_wrapper(self):
        source = self._editor_api_source()
        bridge_source = self._frontend_bridge_source()

        self.assertIn(
            'EDITOR_API_METHOD_SCHEMA_WRAPPED(CoronaEditor, close_process, kNoParams, '
            '"app.closeProcess", "app.close_process", EditorApiValueType::Null)',
            source,
        )
        for snippet in (
            "app: {",
            "closeProcess: () => call_manifest_editor_api('app.closeProcess', [])",
            "closeProcess: () => editorApi.app.closeProcess()",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)

    def test_project_settings_frontend_wrappers_are_manifest_driven(self):
        bridge_source = self._frontend_bridge_source()
        start = bridge_source.find("projectSettings: {")
        end = bridge_source.find("resourceSearch: {", start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        project_settings_section = bridge_source[start:end]

        self.assertIn("call_manifest_editor_api('projectSettings.getActiveProjectInfo'", project_settings_section)
        self.assertIn("call_manifest_editor_api('projectSettings.browseSceneFile'", project_settings_section)
        self.assertNotIn("ProjectSettings.", project_settings_section)

    def test_file_manager_editor_apis_have_explicit_schema(self):
        source = self._editor_api_source()

        expected = {
            "FileManager.create_file": ("kFileManagerCreateFileParams", "EditorApiValueType::Boolean"),
            "FileManager.create_folder": ("kFileManagerCreateFolderParams", "EditorApiValueType::Boolean"),
            "FileManager.delete_item": ("kPathParam", "EditorApiValueType::Boolean"),
            "FileManager.get_file_tree": ("kPathOptionalParam", "EditorApiValueType::Object"),
            "FileManager.get_files": ("kPathOptionalParam", "EditorApiValueType::Array"),
            "FileManager.get_project_info": ("kNoParams", "EditorApiValueType::Object"),
            "FileManager.open_file": ("kFileManagerOpenFileParams", "EditorApiValueType::Boolean"),
            "FileManager.rename_item": ("kFileManagerRenameItemParams", "EditorApiValueType::Boolean"),
        }
        for api_name, (params_name, return_type) in expected.items():
            module, method = api_name.split(".", 1)
            with self.subTest(api_name=api_name):
                plain_schema = f"EDITOR_API_METHOD_SCHEMA({module}, {method}, {params_name}, {return_type})"
                wrapped_schema_prefix = (
                    f"EDITOR_API_METHOD_SCHEMA_WRAPPED({module}, {method}, {params_name}, "
                )
                self.assertTrue(
                    plain_schema in source
                    or (wrapped_schema_prefix in source and return_type in source),
                    f"{api_name} must keep an explicit C++ schema",
                )

    def test_file_manager_apis_use_frontend_typed_wrappers(self):
        bridge_source = self._frontend_bridge_source()

        for snippet in (
            "files: {",
            "getProjectInfo: () => call_manifest_editor_api('files.getProjectInfo', [])",
            "getFiles: (relPath = '') =>",
            "getFileTree: (relPath = '') =>",
            "createFolder: (path, folderName) =>",
            "createFile: (path, fileName, type) =>",
            "deleteItem: (path) => call_manifest_editor_api('files.deleteItem', [path])",
            "renameItem: (oldPath, newName) =>",
            "openFile: (filePath, fileType) =>",
            "getProjectInfo: () => editorApi.files.getProjectInfo()",
            "getFiles: (relPath) => editorApi.files.getFiles(relPath)",
            "getFileTree: (relPath) => editorApi.files.getFileTree(relPath)",
            "createFolder: (path, folderName) =>\n    editorApi.files.createFolder",
            "createFile: (path, fileName, type) =>\n    editorApi.files.createFile",
            "deleteItem: (path) => editorApi.files.deleteItem(path)",
            "renameItem: (oldPath, newName) =>\n    editorApi.files.renameItem",
            "openFile: (filePath, fileType) =>\n    editorApi.files.openFile",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)

    def test_file_manager_frontend_wrappers_are_manifest_driven(self):
        bridge_source = self._frontend_bridge_source()
        start = bridge_source.find("files: {")
        end = bridge_source.find("lanChat: {", start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        files_section = bridge_source[start:end]

        self.assertIn("call_manifest_editor_api('files.getProjectInfo'", files_section)
        self.assertIn("call_manifest_editor_api('files.openFile'", files_section)
        self.assertNotIn("FileManager.", files_section)

    def test_resource_search_editor_apis_have_explicit_schema(self):
        source = self._editor_api_source()

        expected = {
            "ResourceSearch.focus_actor": ("kResourceSearchFocusActorParams", "EditorApiValueType::Object"),
            "ResourceSearch.fuzzy_search": ("kResourceSearchFuzzySearchParams", "EditorApiValueType::Object"),
            "ResourceSearch.get_stats": ("kCallerParam", "EditorApiValueType::Object"),
            "ResourceSearch.image_search": ("kResourceSearchImageSearchParams", "EditorApiValueType::Object"),
            "ResourceSearch.list_types": ("kCallerParam", "EditorApiValueType::Object"),
            "ResourceSearch.mark_index_dirty": ("kResourceSearchMarkIndexDirtyParams", "EditorApiValueType::Object"),
            "ResourceSearch.prepare_index": ("kCallerParam", "EditorApiValueType::Object"),
            "ResourceSearch.rebuild_index": ("kCallerParam", "EditorApiValueType::Object"),
        }
        for api_name, (params_name, return_type) in expected.items():
            module, method = api_name.split(".", 1)
            with self.subTest(api_name=api_name):
                wrapped_schema_prefix = (
                    f"EDITOR_API_METHOD_SCHEMA_WRAPPED({module}, {method}, {params_name}, "
                )
                self.assertIn(
                    wrapped_schema_prefix,
                    source,
                    f"{api_name} must expose one manifest-backed aggregate wrapper",
                )

        self.assertIn('"resourceSearch.focusActor"', source)
        self.assertIn('"resourceSearch.fuzzySearch"', source)
        self.assertIn('"resourceSearch.getStats"', source)
        self.assertIn('"resourceSearch.imageSearch"', source)
        self.assertIn('"resourceSearch.listTypes"', source)
        self.assertIn('"resourceSearch.markIndexDirty"', source)
        self.assertIn('"resourceSearch.prepareIndex"', source)
        self.assertIn('"resourceSearch.rebuildIndex"', source)

    def test_resource_search_apis_use_frontend_typed_wrappers(self):
        bridge_source = self._frontend_bridge_source()

        for snippet in (
            "resourceSearch: {",
            "prepareIndex: () =>",
            "fuzzySearch: (query, topK = 20, typeFilter = null) =>",
            "imageSearch: (imageB64, topK = 20, threshold = 10) =>",
            "listTypes: () =>",
            "rebuildIndex: () =>",
            "getStats: () =>",
            "markIndexDirty: (reason = 'frontend') =>",
            "focusActor: (sceneName, actorName) =>",
            "? editorApi.resourceSearch.prepareIndex(CURRENT_CALLER)",
            "? editorApi.resourceSearch.fuzzySearch(query, topK, typeFilter, CURRENT_CALLER)",
            "? editorApi.resourceSearch.imageSearch(imageB64, topK, threshold, CURRENT_CALLER)",
            "? editorApi.resourceSearch.listTypes(CURRENT_CALLER)",
            "? editorApi.resourceSearch.rebuildIndex(CURRENT_CALLER)",
            "? editorApi.resourceSearch.getStats(CURRENT_CALLER)",
            "? editorApi.resourceSearch.markIndexDirty(reason, CURRENT_CALLER)",
            "? editorApi.resourceSearch.focusActor(sceneName, actorName, CURRENT_CALLER)",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)

    def test_resource_search_frontend_wrappers_are_manifest_driven(self):
        bridge_source = self._frontend_bridge_source()
        start = bridge_source.find("resourceSearch: {")
        end = bridge_source.find("sceneDatas: {", start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        resource_search_section = bridge_source[start:end]

        self.assertIn("call_manifest_editor_api('resourceSearch.prepareIndex'", resource_search_section)
        self.assertIn("call_manifest_editor_api('resourceSearch.focusActor'", resource_search_section)
        self.assertNotIn("ResourceSearch.", resource_search_section)

    def test_network_editor_apis_have_explicit_schema(self):
        source = self._editor_api_source()

        expected = {
            "Network.broadcast_actor_create": ("kNetworkBroadcastActorCreateParams", "EditorApiValueType::Object"),
            "Network.broadcast_actor_delete": ("kNetworkBroadcastActorDeleteParams", "EditorApiValueType::Object"),
            "Network.broadcast_actor_scene_snapshot": ("kNetworkBroadcastSceneSnapshotParams", "EditorApiValueType::Object"),
            "Network.broadcast_actor_state_update": ("kNetworkActorStateUpdateParams", "EditorApiValueType::Object"),
            "Network.broadcast_actor_transform": ("kNetworkActorStateUpdateParams", "EditorApiValueType::Object"),
            "Network.claim_actor_ownership": ("kActorGuidParam", "EditorApiValueType::Object"),
            "Network.connect_to_peer": ("kNetworkConnectToPeerParams", "EditorApiValueType::Object"),
            "Network.get_peer_count": ("kNoParams", "EditorApiValueType::Object"),
            "Network.get_session_info": ("kNoParams", "EditorApiValueType::Object"),
            "Network.poll_pending_actor_create": ("kNoParams", "EditorApiValueType::Object"),
            "Network.poll_pending_actor_delete": ("kNoParams", "EditorApiValueType::Object"),
            "Network.poll_pending_actor_scene_snapshot": ("kNoParams", "EditorApiValueType::Object"),
            "Network.poll_pending_actor_scene_snapshot_request": ("kNoParams", "EditorApiValueType::Object"),
            "Network.poll_pending_actor_state_update": ("kNoParams", "EditorApiValueType::Object"),
            "Network.poll_pending_actor_transform": ("kNoParams", "EditorApiValueType::Object"),
            "Network.register_actor_identity": ("kNetworkRegisterActorIdentityParams", "EditorApiValueType::Object"),
            "Network.request_actor_scene_snapshot": ("kSceneNameParam", "EditorApiValueType::Object"),
            "Network.set_project_root": ("kProjectRootParam", "EditorApiValueType::Object"),
            "Network.set_sync_paused": ("kPausedParam", "EditorApiValueType::Object"),
            "Network.start_session": ("kNetworkStartSessionParams", "EditorApiValueType::Object"),
            "Network.stop_session": ("kNoParams", "EditorApiValueType::Object"),
        }
        for api_name, (params_name, return_type) in expected.items():
            module, method = api_name.split(".", 1)
            with self.subTest(api_name=api_name):
                plain_schema = f"EDITOR_API_METHOD_SCHEMA({module}, {method}, {params_name}, {return_type})"
                wrapped_schema_prefix = (
                    f"EDITOR_API_METHOD_SCHEMA_WRAPPED({module}, {method}, {params_name}, "
                )
                self.assertTrue(
                    plain_schema in source
                    or (wrapped_schema_prefix in source and return_type in source),
                    f"{api_name} must keep an explicit C++ schema",
                )

    def test_network_apis_use_frontend_typed_wrappers(self):
        bridge_source = self._frontend_bridge_source()

        for snippet in (
            "network: {",
            "startSession: (instanceName, projectId, port = 27960, role = 'host') =>",
            "stopSession: () => call_manifest_editor_api('network.stopSession', [])",
            "getPeerCount: () => call_manifest_editor_api('network.getPeerCount', [])",
            "getSessionInfo: () => call_manifest_editor_api('network.getSessionInfo', [])",
            "connectToPeer: (ip, port, peerName) =>",
            "setProjectRoot: (projectRoot) =>",
            "broadcastActorCreate: (actorGuid, sceneName, modelPath, actorData) =>",
            "broadcastActorTransform: (actorGuid, sceneName, actorData) =>",
            "broadcastActorDelete: (actorGuid, sceneName, actorName) =>",
            "requestSceneSnapshot: (sceneName) =>",
            "broadcastSceneSnapshot: (sceneName, snapshot) =>",
            "broadcastActorStateUpdate: (actorGuid, sceneName, actorData) =>",
            "pollPendingActorCreate: () => call_manifest_editor_api('network.pollPendingActorCreate', [])",
            "pollPendingActorTransform: () => call_manifest_editor_api('network.pollPendingActorTransform', [])",
            "pollPendingActorDelete: () => call_manifest_editor_api('network.pollPendingActorDelete', [])",
            "pollPendingSceneSnapshotRequest: () =>",
            "pollPendingSceneSnapshot: () => call_manifest_editor_api('network.pollPendingSceneSnapshot', [])",
            "pollPendingActorStateUpdate: () => call_manifest_editor_api('network.pollPendingActorStateUpdate', [])",
            "setSyncPaused: (paused) => call_manifest_editor_api('network.setSyncPaused', [!!paused])",
            "registerActorIdentity: (actorGuid, actorHandle, locallyOwned = true) =>",
            "claimActorOwnership: (actorGuid) =>",
            "startSession: (instanceName, projectId, port = 27960, role = 'host') =>\n    editorApi.network.startSession",
            "stopSession: () => editorApi.network.stopSession().then(_unwrap)",
            "getPeerCount: () => editorApi.network.getPeerCount().then(_unwrap)",
            "getSessionInfo: () => editorApi.network.getSessionInfo().then(_unwrap)",
            "connectToPeer: (ip, port, peerName) =>\n    editorApi.network.connectToPeer",
            "setProjectRoot: (projectRoot) =>\n    editorApi.network.setProjectRoot",
            "broadcastActorCreate: (actorGuid, sceneName, modelPath, actorData) =>\n    editorApi.network.broadcastActorCreate",
            "registerActorIdentity: (actorGuid, actorHandle, locallyOwned = true) =>\n    editorApi.network.registerActorIdentity",
            "claimActorOwnership: (actorGuid) =>\n    editorApi.network.claimActorOwnership",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)

    def test_network_frontend_wrappers_are_manifest_driven(self):
        bridge_source = self._frontend_bridge_source()
        start = bridge_source.find("network: {")
        end = bridge_source.find("project: {", start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        network_section = bridge_source[start:end]

        expected_wrappers = (
            "network.startSession",
            "network.stopSession",
            "network.getPeerCount",
            "network.getSessionInfo",
            "network.connectToPeer",
            "network.setProjectRoot",
            "network.broadcastActorCreate",
            "network.broadcastActorTransform",
            "network.broadcastActorDelete",
            "network.requestSceneSnapshot",
            "network.broadcastSceneSnapshot",
            "network.broadcastActorStateUpdate",
            "network.pollPendingActorCreate",
            "network.pollPendingActorTransform",
            "network.pollPendingActorDelete",
            "network.pollPendingSceneSnapshotRequest",
            "network.pollPendingSceneSnapshot",
            "network.pollPendingActorStateUpdate",
            "network.setSyncPaused",
            "network.registerActorIdentity",
            "network.claimActorOwnership",
        )
        for wrapper in expected_wrappers:
            with self.subTest(wrapper=wrapper):
                self.assertIn(f"call_manifest_editor_api('{wrapper}'", network_section)
        self.assertNotIn("Network.", network_section)

    def test_lanchat_editor_apis_have_explicit_schema(self):
        source = self._editor_api_source()

        expected = {
            "LANChat.add_agent": ("kObjectPayloadParam", "EditorApiValueType::Object"),
            "LANChat.get_history": ("kNoParams", "EditorApiValueType::Object"),
            "LANChat.get_local_ip": ("kNoParams", "EditorApiValueType::Object"),
            "LANChat.join_room": ("kObjectPayloadParam", "EditorApiValueType::Object"),
            "LANChat.leave_room": ("kNoParams", "EditorApiValueType::Object"),
            "LANChat.list_agents": ("kNoParams", "EditorApiValueType::Object"),
            "LANChat.list_history_rooms": ("kNoParams", "EditorApiValueType::Object"),
            "LANChat.load_history_room": ("kObjectPayloadParam", "EditorApiValueType::Object"),
            "LANChat.remove_agent": ("kObjectPayloadParam", "EditorApiValueType::Object"),
            "LANChat.send_message": ("kObjectPayloadParam", "EditorApiValueType::Object"),
            "LANChat.start_local_room": ("kObjectPayloadParam", "EditorApiValueType::Object"),
            "LANChat.start_room": ("kObjectPayloadParam", "EditorApiValueType::Object"),
            "LANChat.stop_local_room": ("kNoParams", "EditorApiValueType::Object"),
            "LANChat.stop_room": ("kNoParams", "EditorApiValueType::Object"),
        }
        for api_name, (params_name, return_type) in expected.items():
            module, method = api_name.split(".", 1)
            with self.subTest(api_name=api_name):
                plain_schema = f"EDITOR_API_METHOD_SCHEMA({module}, {method}, {params_name}, {return_type})"
                wrapped_schema_prefix = (
                    f"EDITOR_API_METHOD_SCHEMA_WRAPPED({module}, {method}, {params_name}, "
                )
                self.assertTrue(
                    plain_schema in source
                    or (wrapped_schema_prefix in source and return_type in source),
                    f"{api_name} must keep an explicit C++ schema",
                )

    def test_lanchat_apis_use_frontend_typed_wrappers(self):
        bridge_source = self._frontend_bridge_source()

        for snippet in (
            "lanChat: {",
            "startRoom: (payload) => call_manifest_editor_api('lanChat.startRoom', [payload || {}])",
            "startLocalRoom: (payload) => call_manifest_editor_api('lanChat.startLocalRoom', [payload || {}])",
            "stopRoom: () => call_manifest_editor_api('lanChat.stopRoom', [])",
            "stopLocalRoom: () => call_manifest_editor_api('lanChat.stopLocalRoom', [])",
            "joinRoom: (payload) => call_manifest_editor_api('lanChat.joinRoom', [payload || {}])",
            "getHistory: () => call_manifest_editor_api('lanChat.getHistory', [])",
            "listHistoryRooms: () => call_manifest_editor_api('lanChat.listHistoryRooms', [])",
            "loadHistoryRoom: (room) => call_manifest_editor_api('lanChat.loadHistoryRoom', [{ room }])",
            "leaveRoom: () => call_manifest_editor_api('lanChat.leaveRoom', [])",
            "sendMessage: (text, options = {}) =>",
            "call_manifest_editor_api('lanChat.sendMessage', [{ text, ...(options || {}) }])",
            "getLocalIp: () => call_manifest_editor_api('lanChat.getLocalIp', [])",
            "addAgent: (payload) => call_manifest_editor_api('lanChat.addAgent', [payload || {}])",
            "removeAgent: (agentId) => call_manifest_editor_api('lanChat.removeAgent', [{ agent_id: agentId }])",
            "listAgents: () => call_manifest_editor_api('lanChat.listAgents', [])",
            "startRoom: (payload) => editorApi.lanChat.startRoom(payload).then(_unwrap)",
            "startLocalRoom: (payload) => editorApi.lanChat.startLocalRoom(payload).then(_unwrap)",
            "stopRoom: () => editorApi.lanChat.stopRoom().then(_unwrap)",
            "stopLocalRoom: () => editorApi.lanChat.stopLocalRoom().then(_unwrap)",
            "joinRoom: (payload) => editorApi.lanChat.joinRoom(payload).then(_unwrap)",
            "getHistory: () => editorApi.lanChat.getHistory().then(_unwrap)",
            "listHistoryRooms: () => editorApi.lanChat.listHistoryRooms().then(_unwrap)",
            "loadHistoryRoom: (room) => editorApi.lanChat.loadHistoryRoom(room).then(_unwrap)",
            "leaveRoom: () => editorApi.lanChat.leaveRoom().then(_unwrap)",
            "sendMessage: (text, options = {}) =>\n    editorApi.lanChat.sendMessage",
            "getLocalIp: () => editorApi.lanChat.getLocalIp().then(_unwrap)",
            "addAgent: (payload) => editorApi.lanChat.addAgent(payload).then(_unwrap)",
            "removeAgent: (agentId) => editorApi.lanChat.removeAgent(agentId).then(_unwrap)",
            "listAgents: () => editorApi.lanChat.listAgents().then(_unwrap)",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)

    def test_lanchat_frontend_wrappers_are_manifest_driven(self):
        bridge_source = self._frontend_bridge_source()
        start = bridge_source.find("lanChat: {")
        end = bridge_source.find("network: {", start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        lanchat_section = bridge_source[start:end]

        for wrapper_path in (
            "lanChat.startRoom",
            "lanChat.startLocalRoom",
            "lanChat.stopRoom",
            "lanChat.stopLocalRoom",
            "lanChat.joinRoom",
            "lanChat.getHistory",
            "lanChat.listHistoryRooms",
            "lanChat.loadHistoryRoom",
            "lanChat.leaveRoom",
            "lanChat.sendMessage",
            "lanChat.getLocalIp",
            "lanChat.addAgent",
            "lanChat.removeAgent",
            "lanChat.listAgents",
        ):
            with self.subTest(wrapper_path=wrapper_path):
                self.assertIn(f"call_manifest_editor_api('{wrapper_path}'", lanchat_section)
        self.assertNotIn("LANChat.", lanchat_section)

    def test_script_facade_editor_apis_have_explicit_schema(self):
        source = self._editor_api_source()

        expected = {
            "AITool.submit_request": ("kObjectPayloadParam", "EditorApiValueType::Any"),
            "AITool.generate_hint": ("kAiToolGenerateHintParams", "EditorApiValueType::Any"),
            "AITool.read_local_file_as_base64": ("kPathParam", "EditorApiValueType::Any"),
            "AITool.send_message_to_ai_stream": ("kAnyPayloadParam", "EditorApiValueType::Any"),
            "ScratchTool.execute_python_code": ("kScratchExecutePythonCodeParams", "EditorApiValueType::Any"),
            "ScratchTool.get_game_preview_status": ("kNoParams", "EditorApiValueType::Any"),
            "ScratchTool.get_script_status": ("kNoParams", "EditorApiValueType::Any"),
            "ScratchTool.key_event": ("kScratchKeyEventParams", "EditorApiValueType::Any"),
            "ScratchTool.key_release": ("kScratchKeyReleaseParams", "EditorApiValueType::Any"),
            "ScratchTool.load_blockly_target": ("kObjectPayloadParam", "EditorApiValueType::Any"),
            "ScratchTool.mouse_event": ("kScratchMouseEventParams", "EditorApiValueType::Any"),
            "ScratchTool.save_blockly_target": ("kObjectPayloadParam", "EditorApiValueType::Any"),
            "ScratchTool.start_game_preview": ("kObjectPayloadParam", "EditorApiValueType::Any"),
            "ScratchTool.stop_game_preview": ("kNoParams", "EditorApiValueType::Any"),
            "ScratchTool.stop_script_execution": ("kScratchStopScriptParams", "EditorApiValueType::Any"),
        }
        for api_name, (params_name, return_type) in expected.items():
            module, method = api_name.split(".", 1)
            with self.subTest(api_name=api_name):
                plain_schema = f"EDITOR_API_METHOD_SCHEMA({module}, {method}, {params_name}, {return_type})"
                wrapped_schema_prefix = (
                    f"EDITOR_API_METHOD_SCHEMA_WRAPPED({module}, {method}, {params_name}, "
                )
                self.assertTrue(
                    plain_schema in source
                    or (wrapped_schema_prefix in source and return_type in source),
                    f"{api_name} must keep an explicit C++ schema",
                )

    def test_script_facade_apis_use_frontend_typed_wrappers(self):
        bridge_source = self._frontend_bridge_source()

        for snippet in (
            "ai: {",
            "sendMessageToAIStream: (payload) => call_manifest_editor_api('ai.sendMessageToAIStream', [payload])",
            "readLocalFileAsBase64: (filePath) => call_manifest_editor_api('ai.readLocalFileAsBase64', [filePath])",
            "generateHint: (elementType, context = {}) =>",
            "call_manifest_editor_api('ai.generateHint', [elementType, context || {}])",
            "submitRequest: (payload) => call_manifest_editor_api('ai.submitRequest', [payload || {}])",
            "chatStream: (request) => editorApi.ai.submitRequest(request || {})",
            "cancelRequest: (requestId) =>",
            "getRequestStatus: (requestId) =>",
            "scratch: {",
            "executePythonCode: (code, mode, sceneName, actorName, targetType = 'actor') =>",
            "saveBlocklyTarget: (payload) => call_manifest_editor_api('scratch.saveBlocklyTarget', [payload || {}])",
            "loadBlocklyTarget: (payload) => call_manifest_editor_api('scratch.loadBlocklyTarget', [payload || {}])",
            "startGamePreview: (payload = { scope: 'project' }) =>",
            "stopGamePreview: () => call_manifest_editor_api('scratch.stopGamePreview', [])",
            "getGamePreviewStatus: () => call_manifest_editor_api('scratch.getGamePreviewStatus', [])",
            "stopScriptExecution: (restoreState = false) =>",
            "call_manifest_editor_api('scratch.stopScriptExecution', [Boolean(restoreState)])",
            "getScriptStatus: () => call_manifest_editor_api('scratch.getScriptStatus', [])",
            "sendKeyEvent: (key, modifiers, displayKey) =>",
            "sendKeyUpEvent: (key, displayKey) =>",
            "sendMouseEvent: (eventType, button, x, y, viewportX, viewportY, viewportWidth, viewportHeight, pickedActor = '') =>",
            "sendMessageToAIStream: (payload) => editorApi.ai.sendMessageToAIStream(payload)",
            "readLocalFileAsBase64: (filePath) => editorApi.ai.readLocalFileAsBase64(filePath)",
            "generateHint: (elementType, context = {}) => editorApi.ai.generateHint(elementType, context)",
            "chatStream: (request) => editorApi.ai.chatStream(request)",
            "executePythonCode: (code, mode, sceneName, actorName, targetType = 'actor') =>\n    editorApi.scratch.executePythonCode",
            "saveBlocklyTarget: (payload) => editorApi.scratch.saveBlocklyTarget(payload)",
            "startGamePreview: (payload = { scope: 'project' }) => editorApi.scratch.startGamePreview(payload)",
            "sendKeyEvent: (key, modifiers, displayKey) =>\n    editorApi.scratch.sendKeyEvent",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)

    def test_script_facade_scratch_frontend_wrappers_are_manifest_driven(self):
        bridge_source = self._frontend_bridge_source()
        start = bridge_source.find("scratch: {")
        end = bridge_source.find("sceneTools: {", start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        scratch_section = bridge_source[start:end]

        expected_wrappers = (
            "scratch.executePythonCode",
            "scratch.saveBlocklyTarget",
            "scratch.loadBlocklyTarget",
            "scratch.startGamePreview",
            "scratch.stopGamePreview",
            "scratch.getGamePreviewStatus",
            "scratch.stopScriptExecution",
            "scratch.getScriptStatus",
            "scratch.sendKeyEvent",
            "scratch.sendKeyUpEvent",
            "scratch.sendMouseEvent",
        )
        for wrapper in expected_wrappers:
            with self.subTest(wrapper=wrapper):
                self.assertIn(f"call_manifest_editor_api('{wrapper}'", scratch_section)
        self.assertNotIn("ScratchTool.", scratch_section)

    def test_script_facade_ai_frontend_wrappers_are_manifest_driven(self):
        bridge_source = self._frontend_bridge_source()
        start = bridge_source.find("ai: {")
        end = bridge_source.find("files: {", start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        ai_section = bridge_source[start:end]

        self.assertIn("call_manifest_editor_api('ai.sendMessageToAIStream'", ai_section)
        self.assertIn("call_manifest_editor_api('ai.readLocalFileAsBase64'", ai_section)
        self.assertIn("call_manifest_editor_api('ai.generateHint'", ai_section)
        self.assertIn("call_manifest_editor_api('ai.submitRequest'", ai_section)
        self.assertNotIn("AITool.", ai_section)

    def test_main_view_editor_apis_have_explicit_schema(self):
        source = self._editor_api_source()

        expected = {
            "MainView.get_menu_data": ("kNoParams", "EditorApiValueType::Object"),
            "MainView.import_resource_file": ("kMainViewImportResourceFileParams", "EditorApiValueType::Object"),
            "MainView.on_init": ("kPathOptionalParam", "EditorApiValueType::Object"),
            "MainView.run_project": ("kPathOptionalParam", "EditorApiValueType::Object"),
            "MainView.scene_save": ("kSceneSaveParams", "EditorApiValueType::Object"),
            "MainView.update_view_tool_state": ("kMainViewUpdateViewToolStateParams", "EditorApiValueType::Object"),
        }
        for api_name, (params_name, return_type) in expected.items():
            module, method = api_name.split(".", 1)
            with self.subTest(api_name=api_name):
                plain_macro = f"EDITOR_API_METHOD_SCHEMA({module}, {method}, {params_name}, {return_type})"
                wrapped_macro = f"EDITOR_API_METHOD_SCHEMA_WRAPPED({module}, {method}, {params_name},"
                self.assertTrue(
                    plain_macro in source or wrapped_macro in source,
                    f"{api_name} should define an explicit C++ method schema",
                )

    def test_main_view_apis_use_frontend_typed_wrappers(self):
        bridge_source = self._frontend_bridge_source()

        for snippet in (
            "main: {",
            "getMenuData: () => call_manifest_editor_api('main.getMenuData', [])",
            "importResourceFile: (sceneName, fileType) =>",
            "onInit: (projectPath = '') =>",
            "runProject: (scenePath = '') =>",
            "sceneSave: (sceneName) => call_manifest_editor_api('main.sceneSave', [sceneName])",
            "updateViewToolState: (toolId, enabled) =>",
            "OnInit: (projectPath = window.localStorage?.getItem('corona.activeProjectPath') || '') =>\n    editorApi.main.onInit",
            "importResourceFileByDialog: (sceneName, fileType) =>\n    editorApi.main.importResourceFile",
            "getMenuData: () => editorApi.main.getMenuData()",
            "updateViewToolState: (toolId, enabled) =>\n    editorApi.main.updateViewToolState",
            "runProject: (scenePath) =>\n    editorApi.main.runProject",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)

    def test_main_view_frontend_wrappers_are_manifest_driven(self):
        bridge_source = self._frontend_bridge_source()
        start = bridge_source.find("main: {")
        end = bridge_source.find("projectSettings: {", start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        main_section = bridge_source[start:end]

        self.assertIn("call_manifest_editor_api('main.getMenuData'", main_section)
        self.assertIn("call_manifest_editor_api('main.sceneSave'", main_section)
        self.assertNotIn("MainView.", main_section)

    def test_main_view_python_wrappers_are_manifest_driven(self):
        python_api_source = (
            self._repo_root() / "editor" / "api" / "editor_api.py"
        ).read_text(encoding="utf-8")
        start = python_api_source.find("class _MainApi")
        end = python_api_source.find("class CoronaEditorApi", start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        main_section = python_api_source[start:end]

        self.assertIn('_invoke_manifest_cpp_api("main.scene_save", args)', main_section)
        self.assertNotIn("MainView.", main_section)

    def test_editor_api_events_validate_payload_and_cleanup_cef_callbacks(self):
        source = self._editor_api_source()
        cef_client_source = (
            self._repo_root() / "src" / "systems" / "ui" / "cef" / "cef_client.cpp"
        ).read_text(encoding="utf-8")

        for snippet in (
            "validate_editor_api_event_payload",
            "invalid Editor API event payload",
            "event_caller_allowed",
            "emit_callbacks(event_name, payload, false)",
            "emit_callbacks(event_name, payload, true)",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source)

        self.assertIn('#include "cef_editor_api.h"', cef_client_source)
        self.assertIn(
            "EditorApiCallbackRegistry::instance().clear_cef_callbacks_for_browser",
            cef_client_source,
        )
        self.assertIn("void EditorApiCallbackRegistry::clear_cef_callbacks_for_browser(int browser_id)", source)
        self.assertIn("!it->second.python_script", source)
        self.assertIn("*record_browser_id == browser_id", source)
        self.assertIn("g_callbacks.erase(it)", source)

    def test_python_script_callbacks_are_owned_and_emitted_by_cpp_registry(self):
        source = self._editor_api_source()
        header = self._editor_api_header()
        bind_source = (
            self._repo_root() / "src" / "systems" / "ui" / "cef" / "cef_py_bind.cpp"
        ).read_text(encoding="utf-8")
        python_api_source = (
            self._repo_root()
            / "editor"
            / "api"
            / "editor_api.py"
        ).read_text(encoding="utf-8")

        for snippet in (
            "register_python_script_callback_callable",
            "clear_python_script_callbacks",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, header)
                self.assertIn(snippet, source)

        for snippet in (
            "PyCallable_Check(callback)",
            "PythonRuntimeRequestKind::Callback",
            "callback_token",
            "coordinator->submit",
            "clear_python_script_callbacks()",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source)

        callback_section = source[
            source.index("execute_editor_python_callback"):
            source.index("NativeResult invoke_python_script_service")
        ]
        self.assertNotIn("PyGILState_Ensure", callback_section)
        self.assertNotIn("PyGILState_Release", callback_section)

        for snippet in (
            'm.def("register_python_script_callback"',
            'm.def("unregister_python_script_service_dispatcher"',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bind_source)
        self.assertNotIn('m.def("unregister_editor_api_callback"', bind_source)

        for snippet in (
            "def _register_editor_api_event_callback(event_name, wrapper_name, callback):",
            'event_spec.get("python_wrapper") != wrapper_name',
            "def _dispatch(payload_json, event):",
            "return callback(payload, event)",
            "CoronaEngine.register_python_script_callback(event_name, _dispatch)",
            "def off(callback_token):",
            'return _invoke_typed_cpp_api("EditorApi.unregister_callback", "editor.unregister_callback", [callback_token])',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, python_api_source)

    def test_python_api_shutdown_does_not_release_objects_or_take_gil_off_thread(self):
        source = (
            self._repo_root() / "src" / "systems" / "script" / "python" / "python_api.cpp"
        ).read_text(encoding="utf-8")
        shutdown_start = source.index("void PythonAPI::shutdown()")
        shutdown_end = source.index("int64_t PythonAPI::nowMsec()", shutdown_start)
        shutdown_body = source[shutdown_start:shutdown_end]

        self.assertNotIn("(void)pStartFunc.release()", source)
        self.assertNotIn("(void)messageFunc.release()", source)
        self.assertNotIn("nanobind::gil_scoped_acquire", shutdown_body)
        self.assertIn("detach_python_objects_without_decref", shutdown_body)

    def test_scene_tools_registers_native_camera_handlers(self):
        source = self._handler_source()

        match = re.search(
            r"void register_scene_tools_api_handlers\(NativeApiRegistry& registry\).*?"
            r"registry\.register_module\(\"SceneTools\"",
            source,
            re.S,
        )
        self.assertIsNotNone(match)
        scene_tools_handlers = match.group(0)
        for method in (
            "save_screenshot",
            "set_render_backend",
            "get_render_backend",
            "set_vision_render_mode",
            "get_vision_render_mode",
        ):
            with self.subTest(method=method):
                self.assertIn(f'{{"{method}"', scene_tools_handlers)

    def test_native_scene_save_preserves_embedded_vision_document(self):
        source = self._handler_source()
        self.assertIn("persist_native_scene_common", source)
        self.assertIn("persist_native_scene_vision_metadata", source)
        self.assertIn("storage == \"embedded\"", source)
        self.assertIn("storage = embedded", source)
        self.assertIn("persist_native_scene_vision_document", source)
        start = source.find("void persist_native_scene_common")
        end = source.find("void apply_native_scene_environment", start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        persist_body = source[start:end]
        self.assertNotIn('remove_ini_section(scene_file, "vision_document")', persist_body)

        match = re.search(
            r'\{"scene_save", \[\]\(const NativeRequest& request, const NativeContext&\).*?'
            r"return native_success",
            source,
            re.S,
        )
        self.assertIsNotNone(match)
        self.assertIn("persist_native_scene_common(*scene)", match.group(0))

    def test_vision_project_open_embeds_document_without_scene_source_path(self):
        source = self._handler_source()

        self.assertIn("create_embedded_vision_document", source)
        self.assertIn("VISION_DOCUMENT_ENCODING", source)
        self.assertIn('"storage", "embedded"', source)
        self.assertIn('"import_mode", "external"', source)
        self.assertIn('"vision_document"', source)

        start = source.find("std::filesystem::path create_vision_project_native")
        end = source.find("std::filesystem::path open_project_native", start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        create_body = source[start:end]
        self.assertIn("create_embedded_vision_document", create_body)
        self.assertIn("persist_vision_proxy_actors_from_document", create_body)
        self.assertIn("replace_ini_section_from_map", create_body)
        self.assertIn("create_scene_folder", create_body)
        self.assertIn("scene.ini", create_body)
        self.assertNotIn("create_project_from_template_native", create_body)
        self.assertNotIn('"source_path"', create_body)

    def test_embedded_vision_source_loads_from_memory_without_runtime_json(self):
        source = self._handler_source()
        repo_root = pathlib.Path(__file__).resolve().parents[4]
        scene_tools_source = (
            repo_root / "editor" / "plugins" / "SceneTools" / "main.py"
        ).read_text(encoding="utf-8")

        self.assertNotIn("write_embedded_vision_runtime_scene", source)
        self.assertNotIn("vision_runtime", source)
        self.assertNotIn("vision_runtime", scene_tools_source)
        self.assertIn('scene.vision_storage == "embedded"', source)
        self.assertIn("decode_vision_document_data(scene.vision_document_data)", source)
        self.assertIn("Corona::API::load_vision_scene_from_json", source)
        self.assertIn('CFW_LOG_ERROR("Vision embedded scene missing', source)

    def test_project_sidecar_vision_source_remains_compatible(self):
        source = self._handler_source()

        self.assertIn("resolve_project_sidecar_vision_json", source)
        self.assertIn('scene.vision_storage == "project_sidecar"', source)
        self.assertIn('Corona::API::load_vision_scene(path_to_utf8(sidecar_json))', source)
        self.assertIn('CFW_LOG_ERROR("Vision sidecar scene missing', source)
        self.assertIn("migrate_project_sidecar_scene_to_embedded", source)
        self.assertIn("create_embedded_vision_document(scene.project_root, sidecar_json, document)", source)
        self.assertIn("scene_ini = read_ini_file(scene_file)", source)

    def test_vision_scene_load_event_supports_embedded_json_payload(self):
        repo_root = pathlib.Path(__file__).resolve().parents[4]
        event_source = (
            repo_root / "include" / "corona" / "events" / "optics_system_events.h"
        ).read_text(encoding="utf-8")
        api_header = (
            repo_root / "include" / "corona" / "engine" / "engine_runtime_api.h"
        ).read_text(encoding="utf-8")
        api_source = (
            repo_root / "src" / "engine" / "engine_runtime_api.cpp"
        ).read_text(encoding="utf-8")
        optics_header = (
            repo_root / "include" / "corona" / "systems" / "optics" / "optics_system.h"
        ).read_text(encoding="utf-8")
        optics_source = (
            repo_root / "src" / "systems" / "optics" / "optics_system.cpp"
        ).read_text(encoding="utf-8")
        scene_resource_header = (
            repo_root
            / "include"
            / "corona"
            / "systems"
            / "optics"
            / "vision_scene_resource.h"
        ).read_text(encoding="utf-8")

        for snippet in (
            "std::string scene_json",
            "std::string base_dir",
            "std::string scene_key",
            "bool external_live",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, event_source)
        self.assertIn("load_vision_scene_from_json", api_header)
        self.assertIn("load_vision_scene_from_json", api_source)
        self.assertIn("event_bus->publish<Events::VisionSceneLoadEvent>", api_source)
        self.assertIn("VisionSceneLoadRequest", optics_header)
        self.assertIn("std::optional<VisionSceneLoadRequest> pending_vision_scene_load_", optics_header)
        self.assertIn("import_vision_scene_from_data", optics_source)
        self.assertIn("DataWrap::parse(request.scene_json)", optics_source)
        self.assertIn("vision::Global::instance().set_scene_path(base_dir)", optics_source)
        self.assertIn("request.external_live", optics_source)
        self.assertIn("VisionPipelineSource::ExternalLive", optics_source)
        self.assertIn("binding->visible", optics_source)

    def test_python_corona_engine_loader_has_no_fallback_module(self):
        repo_root = pathlib.Path(__file__).resolve().parents[4]
        loader_source = (
            repo_root / "editor" / "CoronaCore" / "core" / "corona_engine.py"
        ).read_text(encoding="utf-8")
        self.assertNotIn("corona_engine_fallback", loader_source)
        self.assertFalse(
            (repo_root / "editor" / "CoronaCore" / "utils" / "corona_engine_fallback.py").exists()
        )

    def test_embedded_vision_registers_synthetic_external_live_bindings(self):
        source = self._handler_source()

        for snippet in (
            "embedded_vision_scene_key",
            "register_embedded_vision_actor_bindings",
            "clear_embedded_vision_actor_bindings",
            "set_external_vision_binding(",
            "clear_external_vision_binding()",
            "const auto shape_guid = vision_shape_guid(shape, index)",
            "scene_key,",
            "static_cast<int>(index)",
            "Corona::API::load_vision_scene_from_json(render_document.dump()",
            "true);",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source)

        load_body = re.search(
            r"void apply_native_scene_vision_source\(.*?\n\}",
            source,
            re.S,
        )
        self.assertIsNotNone(load_body)
        self.assertIn("register_embedded_vision_actor_bindings", load_body.group(0))
        self.assertIn("register_embedded_vision_actor_bindings(scene, render_document, scene_key)", load_body.group(0))

    def test_embedded_vision_visibility_uses_live_transform_without_scene_reload(self):
        source = self._handler_source()
        repo_root = pathlib.Path(__file__).resolve().parents[4]
        optics_source = (
            repo_root / "src" / "systems" / "optics" / "optics_system.cpp"
        ).read_text(encoding="utf-8")
        scene_resource_header = (
            repo_root
            / "include"
            / "corona"
            / "systems"
            / "optics"
            / "vision_scene_resource.h"
        ).read_text(encoding="utf-8")

        render_body = re.search(
            r"nlohmann::json vision_document_for_render\(.*?\n\}",
            source,
            re.S,
        )
        self.assertIsNotNone(render_body)
        self.assertNotIn("remove_if", render_body.group(0))
        self.assertNotIn("vision_shape_visible(shape)", render_body.group(0))

        sync_start = source.find(
            "bool sync_native_actor_to_embedded_vision_document(NativeEditorScene& scene,\n"
            "                                                   const NativeEditorActor& actor,\n"
            "                                                   bool create_if_missing,\n"
            "                                                   bool sync_transform) {"
        )
        sync_end = source.find("bool remove_native_actor_from_embedded_vision_document", sync_start)
        self.assertGreaterEqual(sync_start, 0)
        self.assertGreater(sync_end, sync_start)
        sync_body = source[sync_start:sync_end]
        self.assertIn("persisted && created_shape", sync_body)
        self.assertNotIn("previous_visible != next_visible", sync_body)

        remove_body = re.search(
            r"auto remove_actor_shape = .*?\n    \};",
            optics_source,
            re.S,
        )
        self.assertIsNotNone(remove_body)
        self.assertIn("external_live_shape_removal_action", remove_body.group(0))
        self.assertIn("ExternalLiveShapeRemovalAction::HideOriginal", remove_body.group(0))
        self.assertIn("embedded_runtime", remove_body.group(0))
        self.assertIn("scene_resource->erase_external_live_shape(actor_handle)", remove_body.group(0))

        transform_body = re.search(
            r"void OpticsSystem::sync_external_live_vision_transforms\(.*?\n\}",
            optics_source,
            re.S,
        )
        self.assertIsNotNone(transform_body)
        self.assertIn("hidden_bound_actors", transform_body.group(0))
        self.assertIn("hidden_external_live_o2w()", transform_body.group(0))
        self.assertIn("external_live_hidden_transform_signature", transform_body.group(0))
        self.assertIn("!binding->visible", transform_body.group(0))
        self.assertNotIn("!actor_has_visible_optics(actor_handle)", transform_body.group(0))
        self.assertIn("cache_external_live_original_instance", transform_body.group(0))
        self.assertIn("external_live_original_transform_signatures", transform_body.group(0))
        self.assertIn("restore_external_live_original_instances", transform_body.group(0))
        self.assertIn("continue;", transform_body.group(0))

        self.assertIn("external_live_original_instances", scene_resource_header)
        self.assertIn("external_live_original_transform_signatures", scene_resource_header)
        self.assertIn("cache_external_live_original_instance", scene_resource_header)
        self.assertIn("restore_external_live_original_instances", scene_resource_header)

    def test_embedded_vision_visibility_does_not_rewrite_transform_schema(self):
        source = self._handler_source()

        for snippet in (
            "write_actor_visibility_to_vision_shape",
            "write_actor_state_to_vision_shape(actor, *shape, sync_transform)",
            "operation == \"SetVisible\"",
            "operation != \"SetVisible\"",
            "cleanup_editor_trs_overrides_for_non_trs_transform",
            "cleanup_vision_document_editor_transform_overrides",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source)

    def test_embedded_vision_rewrites_and_copies_common_vision_assets(self):
        source = self._handler_source()

        for snippet in (
            "rewrite_vision_resource_paths_for_project_archive",
            "copy_vision_archive_asset",
            "copy_obj_dependencies",
            "copy_mtl_texture_dependencies",
            "copy_gltf_dependencies",
            'std::filesystem::path("Resource")',
            '"vision_imports"',
            'key == "fn"',
            'key == "texture"',
            'copy_uri_array("buffers")',
            'copy_uri_array("images")',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source)

    def test_native_actor_save_load_preserves_common_optics_fields(self):
        source = self._handler_source()
        for snippet in (
            '".optics.diffuse = "',
            '".optics.metallic = "',
            '".optics.roughness = "',
            '".optics.emission = "',
            '".material.texture = "',
            'item.optics->set_diffuse',
            'item.optics->set_metallic',
            'item.optics->set_roughness',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source)

    def test_embedded_vision_actor_operations_update_document_without_reload(self):
        source = self._handler_source()

        for snippet in (
            "sync_native_actor_to_embedded_vision_document",
            "remove_native_actor_from_embedded_vision_document",
            "persist_embedded_vision_document",
            "vision_document_for_render",
            "ensure_vision_shape_guids",
            "scene.vision_document_data = encode_vision_document_data(document)",
            "Corona::API::load_vision_scene_from_json(",
            '"visible"',
            '".optics.visible = "',
            'actor.optics->set_visible',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source)

        sync_body = re.search(
            r"bool sync_native_actor_to_embedded_vision_document\(.*?\n\}",
            source,
            re.S,
        )
        self.assertIsNotNone(sync_body)
        self.assertNotIn("load_vision_scene_from_json", sync_body.group(0))

        transform_body = re.search(
            r"NativeResult set_native_editor_actor_transform\(.*?\n\}",
            source,
            re.S,
        )
        self.assertIsNotNone(transform_body)
        self.assertIn("sync_native_actor_to_embedded_vision_document(*scene, *actor)", transform_body.group(0))

        operation_body = re.search(
            r"NativeResult apply_actor_operation\(.*?\n\}",
            source,
            re.S,
        )
        self.assertIsNotNone(operation_body)
        self.assertIn("sync_native_actor_to_embedded_vision_document(", operation_body.group(0))
        self.assertIn('operation != "SetVisible"', operation_body.group(0))

        remove_body = re.search(
            r"NativeResult remove_native_editor_actor\(.*?\n\}",
            source,
            re.S,
        )
        self.assertIsNotNone(remove_body)
        self.assertIn("remove_native_actor_from_embedded_vision_document(*scene, removed_guid)", remove_body.group(0))

        document_remove_body = re.search(
            r"bool remove_native_actor_from_embedded_vision_document\(NativeEditorScene& scene,\s+"
            r"const std::string& actor_guid\) \{.*?\n\}",
            source,
            re.S,
        )
        self.assertIsNotNone(document_remove_body)
        self.assertIn("persist_embedded_vision_document", document_remove_body.group(0))
        self.assertNotIn("refresh_embedded_vision_view", document_remove_body.group(0))
        self.assertNotIn("load_vision_scene_from_json", document_remove_body.group(0))

    def test_native_actor_guid_changes_when_same_name_and_index_are_reused(self):
        source = self._handler_source()
        guid_body = re.search(
            r"std::string make_actor_guid\(.*?\n\}",
            source,
            re.S,
        )
        self.assertIsNotNone(guid_body)
        self.assertIn("std::atomic<std::uint64_t>", guid_body.group(0))
        self.assertIn("fetch_add", guid_body.group(0))

    def test_native_actor_runtime_identity_survives_snapshot_persistence_and_transform(self):
        source = self._handler_source()

        for snippet in (
            "std::string runtime_entity_id;",
            "std::string asset_id;",
            "std::string model_ref;",
            "std::string source_plan_id;",
            "std::string source_batch_id;",
            "int actor_version{1};",
            'item["entity_id"] = actor.runtime_entity_id;',
            'item["actor_version"] = std::max(actor.actor_version, 1);',
            '".runtime.entity_id = "',
            '".runtime.actor_version = "',
            'actor_value(".runtime.entity_id")',
            'actor_value(".runtime.actor_version", "1")',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source)

        create_body = re.search(
            r"NativeResult create_native_editor_actor\(.*?\n\}",
            source,
            re.S,
        )
        self.assertIsNotNone(create_body)
        self.assertIn("apply_runtime_metadata", create_body.group(0))
        self.assertIn(
            'json_string_value(actor_data, {"entity_id", "runtime_entity_id"})',
            create_body.group(0),
        )

        transform_body = re.search(
            r"NativeResult set_native_editor_actor_transform\(.*?\n\}",
            source,
            re.S,
        )
        self.assertIsNotNone(transform_body)
        self.assertIn(
            "actor->actor_version = std::max(actor->actor_version + 1, 1);",
            transform_body.group(0),
        )

    def test_native_actor_serializer_does_not_reappend_owned_runtime_fields(self):
        source = self._handler_source()
        normalized_fields = re.search(
            r"static const std::set<std::string> normalized_fields\{(.*?)\};",
            source,
            re.S,
        )
        self.assertIsNotNone(normalized_fields)

        for field in (
            "runtime.entity_id",
            "runtime.asset_id",
            "runtime.model_ref",
            "runtime.entity_type",
            "runtime.semantic_role",
            "runtime.source_plan_id",
            "runtime.source_batch_id",
            "runtime.source_scene_version",
            "runtime.actor_version",
        ):
            with self.subTest(field=field):
                self.assertIn(f'"{field}"', normalized_fields.group(1))

    def test_actor_file_transfer_uses_runtime_asset_identity_without_wire_changes(self):
        source = self._network_system_source()

        for snippet in (
            "std::string actor_asset_id(const std::string& actor_json)",
            'document.find("asset_id")',
            "std::unordered_map<std::string, uint64_t> asset_to_transfer_group;",
            "std::unordered_map<std::string, CachedIncomingAsset> received_asset_cache;",
            "std::vector<PendingAction> actor_actions;",
            "Coalesced actor onto asset transfer",
            "Reused received asset",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source)

        self.assertIn(
            "impl_->asset_to_transfer_group[asset_id] = group_id;",
            source,
        )
        self.assertIn(
            "impl_->received_asset_cache[ft_it->second.asset_id]",
            source,
        )
        self.assertNotIn("asset_id = r.read_string", source)

    def test_embedded_vision_reload_logs_shape_guid_uniqueness(self):
        source = self._handler_source()
        refresh_body = re.search(
            r"bool refresh_embedded_vision_view\(NativeEditorScene& scene,\s+"
            r"const nlohmann::json& document\) \{.*?\n\}",
            source,
            re.S,
        )
        self.assertIsNotNone(refresh_body)
        self.assertIn("duplicate_guid_count", refresh_body.group(0))
        self.assertIn("Vision embedded reload snapshot", refresh_body.group(0))

    def test_native_camera_save_load_preserves_vision_render_settings(self):
        source = self._handler_source()
        for snippet in (
            '".vision_spp = "',
            '".vision_max_depth = "',
            '".vision_denoise = "',
            'section_value("vision_spp"',
            'section_value("vision_max_depth"',
            'section_value("vision_denoise"',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source)

    def test_opening_legacy_project_is_read_only_and_does_not_duplicate_it(self):
        source = self._handler_source()
        start = source.find("std::filesystem::path open_project_native")
        end = source.find("nlohmann::json recent_projects_native", start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        open_body = source[start:end]
        self.assertNotIn("copy_existing_project_to_data_native", open_body)
        self.assertIn("project_dir = candidate", open_body)
        self.assertIn("project_dir = raw_path.parent_path()", open_body)

    def test_project_launcher_business_logic_is_native(self):
        source = self._handler_source()
        for snippet in (
            "void register_project_launcher_api_handlers",
            '"create_project"',
            '"create_world_project"',
            '"create_multiplayer_project"',
            '"open_project"',
            "create_vision_project_native",
            "create_embedded_vision_document",
            "prepare_archive_load",
            "materialize_scene_snapshot",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source)

        rpc_source = (
            pathlib.Path(__file__).resolve().parents[4]
            / "src" / "systems" / "ui" / "cef" / "cef_editor_native_api_registry.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("register_project_launcher_api_handlers(registry)", rpc_source)
        api_source = self._editor_api_source()
        self.assertTrue(
            "EDITOR_API_METHOD_SCHEMA(ProjectLauncher, browse_folder, kPathOptionalParam, EditorApiValueType::String)" in api_source
            or "EDITOR_API_METHOD_SCHEMA_WRAPPED(ProjectLauncher, browse_folder, kPathOptionalParam, \"project.browseFolder\", \"project.browse_folder\", EditorApiValueType::String)" in api_source
        )
        self.assertTrue(
            "EDITOR_API_METHOD_SCHEMA(ProjectLauncher, open_project_file, kNoParams, EditorApiValueType::Object)" in api_source
            or "EDITOR_API_METHOD_SCHEMA_WRAPPED(ProjectLauncher, open_project_file, kNoParams, \"project.openProjectFile\", \"project.open_project_file\", EditorApiValueType::Object)" in api_source
        )
        self.assertTrue(
            "EDITOR_API_METHOD_SCHEMA(ProjectLauncher, open_project, kOpenProjectParams, EditorApiValueType::Object)" in api_source
            or "EDITOR_API_METHOD_SCHEMA_WRAPPED(ProjectLauncher, open_project, kOpenProjectParams, \"project.openProject\", \"project.open_project\", EditorApiValueType::Object)" in api_source
        )
        self.assertTrue(
            "EDITOR_API_METHOD_SCHEMA(ProjectLauncher, create_project, kObjectPayloadParam, EditorApiValueType::String)" in api_source
            or "EDITOR_API_METHOD_SCHEMA_WRAPPED(ProjectLauncher, create_project, kObjectPayloadParam, \"project.createProject\", \"project.create_project\", EditorApiValueType::String)" in api_source
        )

    def test_native_project_template_uses_project_launcher_template_owner(self):
        source = self._handler_source()
        match = re.search(
            r"std::filesystem::path project_template_path\(\) \{(?P<body>.*?)\n\}",
            source,
            re.S,
        )
        self.assertIsNotNone(match)
        body = match.group("body")
        self.assertIn('"plugins" / "ProjectLauncher" / "templates" / "project"', body)
        self.assertNotIn('"CoronaCore" / "demo" / "project"', body)

    def test_vision_project_open_updates_recent_only_after_successful_open(self):
        source = self._handler_source()

        match = re.search(
            r"std::filesystem::path create_project_from_template_native\(.*?\n\}",
            source,
            re.S,
        )
        self.assertIsNotNone(match)
        self.assertNotIn("add_recent_project_native", match.group(0))

        match = re.search(
            r"std::filesystem::path open_project_native\(.*?\n\}",
            source,
            re.S,
        )
        self.assertIsNotNone(match)
        self.assertIn("add_recent_project_native(project_dir)", match.group(0))

    def test_vision_import_uses_type_safe_json_accessors(self):
        source = self._handler_source()
        fragile_snippets = (
            'output["denoise"].get<bool>()',
            'shape.value("type"',
            'shape.value("shape_type"',
            'shape.value("name"',
            'shape.value("fn"',
            'shape.value("path"',
            'params.value("fn"',
            'params.value("path"',
            'params.value("position"',
            'params.value("direction"',
            'params.value("up"',
            'transform_params.value("t"',
            'transform_params.value("s"',
        )
        for snippet in fragile_snippets:
            with self.subTest(snippet=snippet):
                self.assertNotIn(snippet, source)
        self.assertIn('json_bool_value(output, "denoise", false)', source)
        self.assertIn("shapes.is_object()", source)

    def test_project_launcher_open_project_returns_resolved_project_path(self):
        source = self._handler_source()

        start = source.find('{"open_project", [](const NativeRequest& request, const NativeContext&)')
        end = source.find('{"set_project_mode"', start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        handler = source[start:end]
        self.assertIn("auto prepared = prepare_archive_load(path, load_policy)", handler)
        self.assertIn("materialize_scene_snapshot_into_state(", handler)
        materialize_start = source.index("NativeEditorScene& materialize_scene_snapshot_into_state")
        materialize_end = source.index("NativeResult invoke_project_archive_parser", materialize_start)
        self.assertIn(
            "state.project_path = path_to_utf8(scene.project_root)",
            source[materialize_start:materialize_end],
        )
        self.assertIn('{"path", state.project_path}', handler)
        self.assertNotIn("open_project_native", handler)

    def test_project_launcher_frontend_caches_resolved_project_path(self):
        repo_root = pathlib.Path(__file__).resolve().parents[4]
        source = (
            repo_root / "editor" / "Frontend" / "src" / "services" / "projectLauncherService.js"
        ).read_text(encoding="utf-8")
        self.assertIn("const activeProjectPath = success?.path || projectPath;", source)
        self.assertIn("setItem('corona.activeProjectPath', activeProjectPath)", source)

    def test_project_launcher_canonicalizes_settings_project_paths(self):
        source = self._handler_source()
        self.assertIn("canonical_project_dir_for_settings", source)

        for function_name in (
            "read_last_project_from_editor_ini",
            "resolve_active_project_path",
            "add_recent_project_native",
            "recent_projects_native",
            "open_project_native",
        ):
            with self.subTest(function=function_name):
                match = re.search(
                    rf"{function_name}\(.*?\n\}}",
                    source,
                    re.S,
                )
                self.assertIsNotNone(match)
                self.assertIn("canonical_project_dir_for_settings", match.group(0))

    def test_project_launcher_open_path_is_logged_at_rpc_boundaries(self):
        source = self._handler_source()
        for snippet in (
            "[ProjectLauncher] open_project request path='{}'",
            "[ProjectLauncher] open_project opened path='{}'",
            "[ProjectLauncher] open_project failed: {}",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source)

        bridge_source = self._query_bridge_source()
        for snippet in (
            "parse_editor_api_request(request_payload",
            "CefEditorApiEndpoint editor_api",
            "editor_api.invoke(editor_api_request->api_name",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, bridge_source)
        self.assertNotIn("CEF RPC request", bridge_source)
        self.assertNotIn("CEF RPC success", bridge_source)
        self.assertNotIn("CEF RPC failure", bridge_source)
        self.assertNotIn("CEF RPC python fallback", bridge_source)

    def test_native_editor_api_handlers_do_not_emit_legacy_js_event_bus(self):
        handler_source = self._handler_source()
        self.assertNotIn("__coronaEmit", handler_source)
        self.assertNotIn("emit_editor_event_to_all_tabs", handler_source)

    def test_frontend_event_bus_does_not_keep_legacy_vision_import_event(self):
        frontend_root = self._repo_root() / "editor" / "Frontend" / "src"
        offenders = []
        for path in frontend_root.rglob("*.vue"):
            source = path.read_text(encoding="utf-8")
            if "vision-scene-imported" in source:
                offenders.append(str(path.relative_to(self._repo_root())))
        for path in frontend_root.rglob("*.js"):
            source = path.read_text(encoding="utf-8")
            if "vision-scene-imported" in source:
                offenders.append(str(path.relative_to(self._repo_root())))
        self.assertEqual(offenders, [])

    def test_frontend_event_bus_is_local_only_not_cpp_relay(self):
        repo_root = self._repo_root()
        source = (
            repo_root / "editor" / "Frontend" / "src" / "utils" / "eventBus.js"
        ).read_text(encoding="utf-8")

        self.assertNotIn("import { Bridge }", source)
        self.assertNotIn("callDockCommand", source)
        self.assertNotIn("engine-started", source)

    def test_python_scene_save_preserves_embedded_vision_document(self):
        repo_root = pathlib.Path(__file__).resolve().parents[4]
        source = (repo_root / "editor" / "runtime" / "legacy" / "entities" / "scene.py").read_text(
            encoding="utf-8"
        )

        self.assertIn("self.vision_storage", source)
        self.assertIn("self.vision_document", source)
        self.assertIn("_encode_vision_document(vision_document)", source)
        self.assertIn("zlib.compressobj(level=0)", source)
        self.assertIn("'vision_document': {", source)
        self.assertIn("'data': _encode_vision_document(vision_document)", source)
        self.assertIn("'source_id': getattr(self, 'vision_source_id', '')", source)
        self.assertNotIn("self.file_data['vision']['source_path']", source)

    def test_recent_games_import_awaits_open_project_and_catches_errors(self):
        repo_root = pathlib.Path(__file__).resolve().parents[4]
        source = (
            repo_root / "editor" / "Frontend" / "src" / "views" / "layout" / "RecentGames.vue"
        ).read_text(encoding="utf-8")
        start = source.find("const handleImport = async () => {")
        end = source.find("\n};", start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        body = source[start:end]
        self.assertIn("try {", body)
        self.assertIn("await handleOpenProject(result.data.path)", body)
        self.assertIn("console.error('打开现有项目失败:'", body)

    def test_native_scene_load_enables_runtime_mesh_optimization(self):
        repo_root = pathlib.Path(__file__).resolve().parents[4]
        engine_source = (repo_root / "src" / "engine.cpp").read_text(encoding="utf-8")
        parse_common_source = (
            repo_root
            / "modules"
            / "corona_resource"
            / "src"
            / "resource"
            / "types"
            / "parse_common.h"
        ).read_text(encoding="utf-8")

        self.assertIn("scene_parser->assimp_options.simplify_mesh = true", engine_source)
        self.assertIn("scene_parser->assimp_options.lod_options.enabled = true", engine_source)

        self.assertNotIn("bool /*simplify_mesh*/", parse_common_source)
        self.assertIn("if (!simplify_mesh)", parse_common_source)

    def test_camera_view_click_uses_native_actor_pick_and_cleans_up_callback(self):
        repo_root = self._repo_root()
        source = (
            repo_root / "editor" / "Frontend" / "src" / "views" / "tools" / "CameraView.vue"
        ).read_text(encoding="utf-8")

        for snippet in (
            "createViewportPickController",
            "indexActorsByHandle",
            "editorApi.sceneTools.listSceneTree(sceneId)",
            "editorApi.events.onActorPickResult(handleCameraViewActorPickResult)",
            "cameraViewPickController.pickAt(snapshot)",
            "pickedActor",
            "editorApi.off(actorPickResultCallbackToken)",
            "cameraViewPickController.dispose()",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source)

        self.assertIn('@click="handleViewportClick"', source)
        self.assertIn("handleScratchClick(snapshot)", source)
        self.assertNotIn("@click=\"forwardScratchMouse('click', $event)\"", source)

    def test_mouse_click_block_only_dispatches_for_click_event(self):
        repo_root = self._repo_root()
        source = (
            repo_root / "editor" / "Frontend" / "src" / "blockly" / "generators" / "event.js"
        ).read_text(encoding="utf-8")
        start = source.find("pythonGenerator.forBlock['event_mouse_click']")
        end = source.find("pythonGenerator.forBlock['event_mouse_move']", start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        body = source[start:end]

        self.assertIn("strip().lower() == 'click'", body)
        self.assertNotIn("'mousedown', 'pointerdown', 'down'", body)

    def test_scratch_mouse_click_does_not_latch_held_state(self):
        repo_root = self._repo_root()
        source = (
            repo_root / "editor" / "script_runtime" / "engine" / "corona_engine.py"
        ).read_text(encoding="utf-8")
        start = source.find("def handle_mouse_event(")
        end = source.find("\ndef ", start + 1)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        body = source[start:end]

        self.assertIn('normalized_event in ("mousedown", "pointerdown", "down")', body)
        self.assertIn('normalized_event in ("mouseup", "pointerup", "up")', body)
        self.assertNotIn('normalized_event in ("click",', body)

    def test_python_settings_hydrates_native_last_project(self):
        repo_root = pathlib.Path(__file__).resolve().parents[4]
        settings_source = (
            repo_root / "editor" / "config" / "project_state.py"
        ).read_text(encoding="utf-8")

        self.assertIn("def _hydrate_active_project_from_last_project", settings_source)
        self.assertIn('self.config.get("General", "last_project"', settings_source)
        self.assertIn("self._active_project_path = project_path", settings_source)
        self.assertIn("self.active_project_config = proj_cfg", settings_source)

    def test_collision_shape_is_persisted_and_round_trips_as_three_state_value(self):
        repo_root = self._repo_root()
        hub_source = (repo_root / "include" / "corona" / "shared_data_hub.h").read_text(encoding="utf-8")
        api_source = (
            repo_root / "src" / "systems" / "ui" / "cef" / "cef_editor_native_api_handlers.cpp"
        ).read_text(encoding="utf-8")
        object_source = (
            repo_root / "editor" / "Frontend" / "src" / "views" / "sidebar" / "Object.vue"
        ).read_text(encoding="utf-8")

        self.assertIn("enum class CollisionShape", hub_source)
        self.assertIn("CollisionShape collision_shape{CollisionShape::Box}", hub_source)
        self.assertIn(".mechanics.collision_type", api_source)
        self.assertIn('? collision_shape_name(*actor.mechanics)', api_source)
        self.assertIn(': actor.persisted_collision_type', api_source)
        self.assertIn("set_collision_shape", api_source)
        self.assertIn("const normalizeCollisionType", object_source)
        self.assertIn("actor.collision = normalizeCollisionType(", object_source)
        scratch_source = (
            repo_root / "editor" / "script_runtime" / "engine" / "corona_engine.py"
        ).read_text(encoding="utf-8")
        self.assertIn("def _normalize_native_collision_type", scratch_source)
        self.assertIn("[self._collision_type]", scratch_source)
        self.assertNotIn('[bool(actor_data.get("collision", True))]', scratch_source)
        mechanics_source = (
            repo_root / "src" / "systems" / "mechanics" / "mechanics_system.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("const bool both_mesh", mechanics_source)
        self.assertIn("collision_shape == CollisionShape::Mesh", mechanics_source)

    def test_physics_loop_does_not_synchronously_invoke_python_move_callback(self):
        repo_root = self._repo_root()
        mechanics_source = (
            repo_root / "src" / "systems" / "mechanics" / "mechanics_system.cpp"
        ).read_text(encoding="utf-8")
        bindings_source = (
            repo_root / "src" / "systems" / "script" / "python" / "engine_bindings.cpp"
        ).read_text(encoding="utf-8")
        lifecycle_source = (
            repo_root / "src" / "systems" / "mechanics" / "mechanics_lifecycle.cpp"
        ).read_text(encoding="utf-8")
        actor_source = (
            repo_root / "editor" / "runtime" / "legacy" / "entities" / "actor.py"
        ).read_text(encoding="utf-8")

        self.assertNotIn("Py_AddPendingCall", bindings_source)
        self.assertNotIn("shared_ptr<nb::object>", bindings_source)
        self.assertIn("PythonRuntimeRequestKind::Callback", bindings_source)
        self.assertIn("callback_token", bindings_source)
        self.assertIn("resolve_fixed_dt", lifecycle_source)
        self.assertIn("kMaxCatchUpSteps", lifecycle_source)
        on_move_start = actor_source.index("    def on_move(self):")
        on_move_end = actor_source.index("\n    def enable_collision_callback", on_move_start)
        self.assertNotIn("self.save_data()", actor_source[on_move_start:on_move_end])

    def test_synchronous_screenshot_wait_releases_the_python_gil(self):
        repo_root = self._repo_root()
        bindings_source = (
            repo_root / "src" / "systems" / "script" / "python" / "engine_bindings.cpp"
        ).read_text(encoding="utf-8")

        binding_start = bindings_source.index('.def("save_screenshot_sync"')
        binding_end = bindings_source.index("\n        .def(", binding_start + 1)
        screenshot_binding = bindings_source[binding_start:binding_end]

        self.assertIn("nb::call_guard<nb::gil_scoped_release>()", screenshot_binding)

    def test_portable_scene_folder_is_wired_through_project_and_frontend_apis(self):
        repo_root = self._repo_root()
        handler_source = (
            repo_root / "src" / "systems" / "ui" / "cef" / "cef_editor_native_api_handlers.cpp"
        ).read_text(encoding="utf-8")
        api_source = (
            repo_root / "src" / "systems" / "ui" / "editor_api" / "cef_editor_api.cpp"
        ).read_text(encoding="utf-8")
        bridge_source = (
            repo_root / "editor" / "Frontend" / "src" / "api" / "editorApi.js"
        ).read_text(encoding="utf-8")
        self.assertIn('#include "scene_folder.h"', handler_source)
        self.assertIn('detect_scene_folder(project_dir)', handler_source)
        self.assertIn('{"migrate_legacy_scene"', handler_source)
        self.assertIn('migrate_legacy_scene({source_path, target_path, scene_name})', handler_source)
        self.assertIn('ProjectLauncher, choose_portable_scene_target', api_source)
        self.assertIn('ProjectLauncher, migrate_legacy_scene', api_source)
        self.assertIn("project.choosePortableSceneTarget", bridge_source)
        self.assertIn("project.migrateLegacyScene", bridge_source)
        self.assertIn('{"choose_portable_scene_target", []', handler_source)
        self.assertIn('L"*.ini;*.scene;*.json"', handler_source)
        open_start = handler_source.index("std::filesystem::path open_project_native")
        open_end = handler_source.index("nlohmann::json recent_projects_native", open_start)
        open_body = handler_source[open_start:open_end]
        self.assertIn('ext == ".scene"', open_body)
        self.assertIn('candidate / "project.ini"', open_body)
        self.assertNotIn("copy_existing_project_to_data_native", open_body)
        self.assertIn("project_dir = raw_path.parent_path()", open_body)
        self.assertIn("Legacy projects are read-only", handler_source)

    def test_new_project_creation_uses_portable_scene_folder_without_mode(self):
        repo_root = self._repo_root()
        handler_source = (
            repo_root / "src" / "systems" / "ui" / "cef" / "cef_editor_native_api_handlers.cpp"
        ).read_text(encoding="utf-8")
        create_start = handler_source.index('{"create_project"')
        create_end = handler_source.index('{"create_world_project"', create_start)
        create_body = handler_source[create_start:create_end]
        self.assertIn("create_scene_folder(target, name)", create_body)
        self.assertNotIn('data.value("mode"', create_body)
        self.assertNotIn("create_project_from_template_native", create_body)
        portable_info_start = handler_source.index("if (const auto portable = detect_scene_folder(project_root))")
        portable_info_end = handler_source.index("const auto project_ini", portable_info_start)

    def test_world_creation_updates_default_save_location_for_portable_migration(self):
        handler_source = self._handler_source()
        start = handler_source.index('{"create_world_project"')
        end = handler_source.index('{"create_multiplayer_project"', start)
        world_body = handler_source[start:end]
        self.assertIn(
            'update_editor_settings_section("General", {{"default_path", path_to_utf8(base_dir)}})',
            world_body,
        )

    def test_legacy_project_template_does_not_reference_missing_scene_script(self):
        repo_root = self._repo_root()
        template_source = (
            repo_root
            / "editor"
            / "plugins"
            / "ProjectLauncher"
            / "templates"
            / "project"
            / "Scene"
            / "default.scene"
        ).read_text(encoding="utf-8")
        self.assertIn("[scripts]\npath =\n", template_source)
        self.assertNotIn("Scripts/scene_script.py", template_source)

    def test_archive_open_is_python_parsed_and_cpp_materialized(self):
        source = self._handler_source()
        self.assertIn("invoke_project_archive_parser", source)
        self.assertIn('parser_request.module = "ProjectArchive"', source)
        self.assertIn('parser_request.function = "parse"', source)
        self.assertIn("validate_archive_snapshot", source)
        self.assertIn("validate_snapshot_float3", source)
        self.assertIn("Duplicate actor_guid in ArchiveSnapshot", source)
        self.assertIn("asset_path must be absolute", source)
        self.assertIn("materialize_scene_snapshot", source)
        self.assertIn("ActorLoadStatus", source)
        self.assertIn("UNSUPPORTED_RESOURCE_TYPE", source)
        self.assertIn('item.actor_type != "audio"', source)
        self.assertIn("actor.persisted_snapshot.value(", source)
        self.assertIn('"persisted_fields", nlohmann::json::object()', source)
        self.assertIn("actor.load_status != ActorLoadStatus::Loaded", source)
        self.assertIn("allow_missing", source)
        self.assertIn('{"rebind_actor_resource"', source)
        self.assertIn("load_status = ActorLoadStatus::Loaded", source)
        self.assertIn('"decision_required"', source)
        self.assertNotIn("std::unique_ptr<NativeEditorScene> load_native_scene(", source)

    def test_archive_load_materializes_only_after_target_scene_is_committed(self):
        source = self._handler_source()
        prepare_start = source.index("PreparedArchiveLoad prepare_archive_load")
        prepare_end = source.index("NativeEditorScene* ensure_native_editor_scene", prepare_start)
        prepare_body = source[prepare_start:prepare_end]
        struct_start = source.index("struct PreparedArchiveLoad")
        struct_end = source.index("};", struct_start)
        prepared_struct = source[struct_start:struct_end]

        self.assertNotIn("materialize_scene_snapshot(", prepare_body)
        self.assertNotIn("std::unique_ptr<NativeEditorScene> scene", prepared_struct)
        self.assertIn("materialize_scene_snapshot_into_state", source)
        self.assertIn("state.scene = std::make_unique<NativeEditorScene>()", source)
        self.assertNotIn("state.scene = std::move(prepared.scene)", source)

    def test_project_resource_loading_is_non_blocking_and_reported_to_frontend(self):
        source = self._handler_source()
        api_source = self._editor_api_source()
        bridge_source = self._frontend_bridge_source()
        main_page = (
            self._repo_root()
            / "editor"
            / "Frontend"
            / "src"
            / "views"
            / "layout"
            / "MainPage.vue"
        ).read_text(encoding="utf-8")

        self.assertIn('{"get_project_load_status"', source)
        self.assertTrue(
            "EDITOR_API_METHOD_SCHEMA(ProjectLauncher, get_project_load_status, "
            "kNoParams, EditorApiValueType::Object)" in api_source
            or "EDITOR_API_METHOD_SCHEMA_WRAPPED(ProjectLauncher, get_project_load_status, kNoParams, \"project.getProjectLoadStatus\", \"project.get_project_load_status\", EditorApiValueType::Object)" in api_source
        )
        self.assertIn("getProjectLoadStatus: () =>", bridge_source)
        self.assertIn("projectResourceLoadStatus", main_page)
        self.assertIn("pollProjectResourceLoadStatus", main_page)

    def test_project_resource_load_status_counts_audio_without_geometry_as_ready(self):
        source = self._handler_source()
        start = source.index("nlohmann::json project_resource_load_status()")
        end = source.index("nlohmann::json camera_to_json", start)
        handler = source[start:end]

        audio_check = handler.index('actor.actor_type == "audio"')
        geometry_failure_check = handler.index("!actor.geometry")
        self.assertLess(audio_check, geometry_failure_check)

    def test_project_launcher_dialogs_are_native_and_archive_startup_is_nonblocking(self):
        repo_root = self._repo_root()
        source = self._handler_source()
        registry_source = (
            repo_root / "editor" / "runtime" / "registry.py"
        ).read_text(encoding="utf-8")
        recent_games_source = (
            repo_root
            / "editor"
            / "Frontend"
            / "src"
            / "views"
            / "layout"
            / "RecentGames.vue"
        ).read_text(encoding="utf-8")
        new_game_source = (
            repo_root
            / "editor"
            / "Frontend"
            / "src"
            / "views"
            / "layout"
            / "NewGame.vue"
        ).read_text(encoding="utf-8")
        editor_main_source = (
            repo_root / "editor" / "runtime" / "bootstrap.py"
        ).read_text(encoding="utf-8")
        launcher_start = source.index(
            "void register_project_launcher_api_handlers"
        )
        launcher_end = source.index(
            "void register_main_view_api_handlers",
            launcher_start,
        )
        launcher_body = source[launcher_start:launcher_end]

        self.assertIn("LAZY_PYTHON_SCRIPT_SERVICES", registry_source)
        self.assertIn('"AITool"', registry_source)
        self.assertIn("LazyPythonScriptService", registry_source)
        self.assertIn("open_project_file_native()", launcher_body)
        self.assertIn('{"browse_folder"', launcher_body)
        self.assertNotIn('{"open_project_file", script_method}', launcher_body)
        self.assertNotIn('{"browse_folder", script_method}', launcher_body)
        self.assertNotIn(
            '{"choose_portable_scene_target", script_method}',
            launcher_body,
        )
        self.assertIn('"archive_service_ready"', source)
        self.assertIn('"service_initializing"', launcher_body)
        self.assertIn("存档服务正在初始化", recent_games_source)
        self.assertIn("存档服务正在初始化", new_game_source)
        self.assertNotIn("warmup_all", editor_main_source)
        self.assertNotIn("plugins.AITool", editor_main_source)
        self.assertIn(
            "register_core_python_script_services()",
            editor_main_source,
        )
        core_registration = editor_main_source.index(
            "register_core_python_script_services()"
        )
        dispatcher_registration = editor_main_source.index(
            "editor.register_script_dispatcher()"
        )
        remaining_registration = editor_main_source.index("reimport()")
        self.assertLess(core_registration, dispatcher_registration)
        self.assertLess(dispatcher_registration, remaining_registration)

    def test_main_on_init_only_reads_committed_native_scene(self):
        source = self._handler_source()
        start = source.index('static const NativeMethodTable methods = {', source.index("void register_main_view_api_handlers"))
        end = source.index('};', start)
        methods = source[start:end]
        on_init_start = methods.index('{"on_init"')
        on_init_end = methods.index('{"create_scene"', on_init_start)
        on_init = methods[on_init_start:on_init_end]
        self.assertIn("auto& state = native_editor_state()", on_init)
        self.assertIn("state.scene", on_init)
        self.assertNotIn("ensure_native_editor_scene", on_init)

    def test_frontend_handles_degraded_archive_without_duplicate_reload(self):
        repo_root = self._repo_root()
        bridge = (
            repo_root / "editor" / "Frontend" / "src" / "services" / "projectLauncherService.js"
        ).read_text(encoding="utf-8")
        recent = (
            repo_root / "editor" / "Frontend" / "src" / "views" / "layout" / "RecentGames.vue"
        ).read_text(encoding="utf-8")
        scene_bar = (
            repo_root / "editor" / "Frontend" / "src" / "views" / "sidebar" / "SceneBar.vue"
        ).read_text(encoding="utf-8")
        object_panel = (
            repo_root / "editor" / "Frontend" / "src" / "views" / "sidebar" / "Object.vue"
        ).read_text(encoding="utf-8")
        open_start = bridge.index("openProject: async")
        open_end = bridge.index("\n  },", open_start)
        open_body = bridge[open_start:open_end]
        self.assertIn("options = {}", open_body)
        self.assertIn("load_policy", open_body)
        self.assertIn("openProject: async (projectPath, options = {}) => {", bridge)
        self.assertIn(
            "editorApi.project.openProject(projectPath, { load_policy: loadPolicy })",
            open_body,
        )
        self.assertNotIn("editorApi.main.onInit", open_body)
        self.assertNotIn("reloadScene", open_body)
        self.assertIn("__coronaNodeGraphFlushSave", open_body)
        self.assertIn("corona-active-project-changed", open_body)
        self.assertIn("decision_required", recent)
        self.assertIn("loadPolicy: 'degraded'", recent)
        self.assertIn('data-testid="actor-load-warning"', scene_bar)
        self.assertIn("rebindActorResource", scene_bar)
        self.assertIn("unresolved_actor_count", scene_bar)
        self.assertIn('data-testid="actor-placeholder-warning"', object_panel)
        self.assertIn("rebindPlaceholderResource", object_panel)

    def test_archive_materialization_preserves_runtime_actor_metadata(self):
        source = self._handler_source()
        materialize_start = source.index("NativeEditorActor native_actor_from_snapshot")
        materialize_end = source.index(
            "void append_actor_materialization_diagnostic", materialize_start
        )
        materialize = source[materialize_start:materialize_end]
        actor_json_start = source.index("nlohmann::json actor_to_json")
        actor_json_end = source.index("nlohmann::json scene_to_json", actor_json_start)
        actor_json = source[actor_json_start:actor_json_end]

        for field in (
            "runtime_entity_id",
            "asset_id",
            "model_ref",
            "entity_type",
            "semantic_role",
            "source_plan_id",
            "source_batch_id",
            "source_scene_version",
            "actor_version",
        ):
            self.assertIn(field, materialize)
            self.assertIn(field, actor_json)

    def test_legacy_scene_migration_has_prompt_and_permanent_file_manager_action(self):
        repo_root = self._repo_root()
        handler_source = (
            repo_root / "src" / "systems" / "ui" / "cef" / "cef_editor_native_api_handlers.cpp"
        ).read_text(encoding="utf-8")
        recent_games_source = (
            repo_root / "editor" / "Frontend" / "src" / "views" / "layout" / "RecentGames.vue"
        ).read_text(encoding="utf-8")
        file_manager_source = (
            repo_root / "editor" / "Frontend" / "src" / "views" / "sidebar" / "FileManager.vue"
        ).read_text(encoding="utf-8")

        self.assertIn('prepared.snapshot.value("project", nlohmann::json::object())', handler_source)
        self.assertIn('.value("legacy", false)', handler_source)
        self.assertIn("corona.legacyMigrationPrompted", recent_games_source)
        self.assertIn("另存为便携场景", recent_games_source)
        self.assertIn("migrateLegacyScene", recent_games_source)
        self.assertIn("migrateLegacyScene", file_manager_source)
        self.assertIn("另存为便携场景", file_manager_source)

    def test_recent_project_card_exposes_legacy_migration_action(self):
        repo_root = self._repo_root()
        recent_games_source = (
            repo_root / "editor" / "Frontend" / "src" / "views" / "layout" / "RecentGames.vue"
        ).read_text(encoding="utf-8")
        self.assertIn("migrateLegacyProject", recent_games_source)
        self.assertIn("另存为便携场景", recent_games_source)
        self.assertIn("proj.legacy", recent_games_source)
        self.assertIn("@click.stop=\"migrateLegacyProject(proj)\"", recent_games_source)
        self.assertIn("便携场景", recent_games_source)
        self.assertIn("旧格式", recent_games_source)

    def test_project_launcher_route_and_component_are_removed_after_recent_games_unification(self):
        repo_root = self._repo_root()
        router_source = (repo_root / "editor" / "Frontend" / "src" / "router" / "index.js").read_text(
            encoding="utf-8"
        )
        self.assertFalse(
            (repo_root / "editor" / "Frontend" / "src" / "views" / "layout" / "ProjectLauncher.vue").exists()
        )
        self.assertNotIn("ProjectLauncher.vue", router_source)

    def test_frontend_merge_preserves_archive_guards_and_ui_workflows(self):
        repo_root = self._repo_root()
        frontend = repo_root / "editor" / "Frontend" / "src" / "views"
        new_game = (frontend / "layout" / "NewGame.vue").read_text(encoding="utf-8")
        recent_games = (frontend / "layout" / "RecentGames.vue").read_text(encoding="utf-8")
        object_panel = (frontend / "sidebar" / "Object.vue").read_text(encoding="utf-8")

        for source in (new_game, recent_games, object_panel):
            self.assertNotIn("<<<<<<<", source)
            self.assertNotIn("=======", source)
            self.assertNotIn(">>>>>>>", source)

        self.assertIn(':disabled="creating || !archiveReady"', new_game)
        self.assertIn("waitForPythonProjectActivation", new_game)
        self.assertIn("openResult?.status === 'service_initializing'", new_game)
        self.assertIn("lanchat.openRoom", new_game)
        self.assertIn("selectedProject && archiveReady", recent_games)
        self.assertIn("@click=\"openSelectedProject\"", recent_games)
        self.assertIn("bg-[#d8b86c]", recent_games)
        self.assertIn("actor.loadStatus === 'loaded'", object_panel)
        self.assertIn("collapsedSections.physics", object_panel)
        self.assertIn(".placeholder-warning", object_panel)

    def test_recent_project_entries_mark_legacy_projects_for_recent_games(self):
        source = self._handler_source()
        start = source.index("nlohmann::json recent_projects_native()")
        end = source.index("nlohmann::json active_project_info_json()", start)
        recent_body = source[start:end]
        self.assertIn('{"legacy", exists && !portable}', recent_body)

    def test_project_settings_does_not_write_project_section_into_portable_scene(self):
        repo_root = self._repo_root()
        settings_source = (
            repo_root / "editor" / "plugins" / "ProjectSettings" / "main.py"
        ).read_text(encoding="utf-8")
        self.assertIn("CoronaEditorApi.project_settings.get_active_project_info", settings_source)
        self.assertIn("CoronaEditorApi.project_settings.save_active_project_info", settings_source)
        self.assertNotIn("configparser", settings_source)

    def test_python_scene_save_preserves_portable_scene_metadata_without_base_section(self):
        repo_root = self._repo_root()
        scene_source = (
            repo_root / "editor" / "runtime" / "legacy" / "entities" / "scene.py"
        ).read_text(encoding="utf-8")
        self.assertIn("def _is_portable_scene_folder", scene_source)
        save_start = scene_source.index("    def save_data(self):")
        save_end = scene_source.index("    @auto_save\n    def set_script", save_start)
        save_body = scene_source[save_start:save_end]
        self.assertIn("if self._is_portable_scene_folder():", save_body)
        self.assertIn("CoronaEditorApi.main.scene_save", save_body)
        self.assertLess(save_body.index("CoronaEditorApi.main.scene_save"),
                        save_body.index("with open(data_path"))

    def test_portable_scene_has_one_native_transactional_save_path(self):
        repo_root = self._repo_root()
        handler_source = (
            repo_root / "src" / "systems" / "ui" / "cef" / "cef_editor_native_api_handlers.cpp"
        ).read_text(encoding="utf-8")
        scene_source = (
            repo_root / "editor" / "runtime" / "legacy" / "entities" / "scene.py"
        ).read_text(encoding="utf-8")
        autosave_source = (
            repo_root / "editor" / "runtime" / "scene_support.py"
        ).read_text(encoding="utf-8")
        settings_source = (
            repo_root / "editor" / "config" / "project_state.py"
        ).read_text(encoding="utf-8")

        self.assertIn("SceneDocumentStore document_store", handler_source)
        self.assertIn("document_store.replace_sections", handler_source)
        self.assertIn('sections["scene"] = {"[scene]"}', handler_source)
        self.assertIn('scene_metadata["name"] = scene.name', handler_source)
        save_start = scene_source.index("    def save_data(self):")
        save_end = scene_source.index("    @auto_save\n    def set_script", save_start)
        save_body = scene_source[save_start:save_end]
        self.assertIn("CoronaEditorApi.main.scene_save", save_body)
        self.assertIn("Portable scene saves are owned by the native scene store", save_body)
        self.assertIn("if isinstance(result, dict) and not result.get('ok', False):", save_body)
        self.assertIn("logger.exception", autosave_source)
        self.assertIn("if portable:", settings_source)
        self.assertIn(
            'self.config.set("General", "last_project", self.active_project_path)',
            settings_source,
        )

        project_settings_source = (
            repo_root / "editor" / "plugins" / "ProjectSettings" / "main.py"
        ).read_text(encoding="utf-8")
        self.assertIn("get_active_project_info", project_settings_source)
        self.assertIn("save_active_project_info", project_settings_source)
        self.assertNotIn("scene_save", project_settings_source)

    def test_portable_scene_validation_import_and_cleanup_apis_are_exposed(self):
        repo_root = self._repo_root()
        api_source = (
            repo_root / "src" / "systems" / "ui" / "editor_api" / "cef_editor_api.cpp"
        ).read_text(encoding="utf-8")
        handler_source = (
            repo_root / "src" / "systems" / "ui" / "cef" / "cef_editor_native_api_handlers.cpp"
        ).read_text(encoding="utf-8")
        bridge_source = (
            repo_root / "editor" / "Frontend" / "src" / "api" / "editorApi.js"
        ).read_text(encoding="utf-8")
        for native_name, wrapper in (
            ("validate_portable_scene", "project.validatePortableScene"),
            ("import_portable_asset", "project.importPortableAsset"),
            ("cleanup_portable_scene_assets", "project.cleanupPortableSceneAssets"),
        ):
            self.assertIn(f"ProjectLauncher, {native_name}", api_source)
            self.assertIn(f'{{"{native_name}"', handler_source)
            self.assertIn(wrapper, bridge_source)

    def test_duplicate_native_actor_check_happens_before_portable_asset_import(self):
        repo_root = self._repo_root()
        handler_source = (
            repo_root / "src" / "systems" / "ui" / "cef" / "cef_editor_native_api_handlers.cpp"
        ).read_text(encoding="utf-8")
        start = handler_source.index("NativeResult create_native_editor_actor")
        end = handler_source.index("NativeResult remove_native_editor_actor", start)
        body = handler_source[start:end]
        self.assertLess(body.index("skip_if_exists"), body.index("portable_store.emplace"))

    def test_scene_save_returns_structured_portable_validation_diagnostics(self):
        repo_root = self._repo_root()
        source = (
            repo_root / "src" / "systems" / "ui" / "cef" / "cef_editor_native_api_handlers.cpp"
        ).read_text(encoding="utf-8")
        start = source.index('{"scene_save"')
        end = source.index('{"update_view_tool_state"', start)
        body = source[start:end]
        self.assertIn("PortableSceneValidationError", body)
        self.assertIn('{"diagnostics", error.diagnostics()}', body)
        self.assertIn('{"ok", false}', body)


if __name__ == "__main__":
    unittest.main()
