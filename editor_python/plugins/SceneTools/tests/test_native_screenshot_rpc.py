import pathlib
import re
import unittest


class NativeSceneToolsRpcTests(unittest.TestCase):
    def _repo_root(self):
        return pathlib.Path(__file__).resolve().parents[4] / "CoronaEditor"

    def _engine_root(self):
        return pathlib.Path(__file__).resolve().parents[4]

    def _handler_source(self):
        repo_root = self._engine_root()
        handler_path = self._engine_root() / "src" / "systems" / "ui" / "cef" / "cef_editor_native_api_handlers.cpp"
        return handler_path.read_text(encoding="utf-8")

    def test_network_actor_create_rejects_missing_stable_guid(self):
        source = self._handler_source()
        start = source.find('{"broadcast_actor_create"')
        end = source.find('        }},', start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        handler = source[start:end]

        self.assertIn('return native_failure("Actor GUID is required", 2);', handler)
        self.assertNotIn('actor_guid = scene_name + ":" + model_path;', handler)
        self.assertIn("initialize_actor_create_packed(opt);", handler)

        self.assertIn("initialize_actor_create_packed(optics);", source)

    def _editor_api_header(self):
        api_path = (
            self._engine_root()
            / "src"
            / "systems"
            / "ui"
            / "editor_api"
            / "cef_editor_api.h"
        )
        return api_path.read_text(encoding="utf-8")

    def _editor_api_source(self):
        api_path = (
            self._engine_root()
            / "src"
            / "systems"
            / "ui"
            / "editor_api"
            / "cef_editor_api.cpp"
        )
        return api_path.read_text(encoding="utf-8")

    def _native_api_header(self):
        return (
            self._engine_root()
            / "src"
            / "systems"
            / "ui"
            / "cef"
            / "cef_editor_native_api_registry.h"
        ).read_text(encoding="utf-8")

    def _assert_retired_query_transport(self):
        root = self._engine_root() / "src/systems/ui"
        for retired in ("cef_query_bridge.cpp", "cef_query_bridge.h", "cef_realtime_bridge.cpp"):
            self.assertFalse((root / "cef" / retired).exists(), retired)
        cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertNotIn("cef_query_bridge", cmake)
        binding = (root / "cef/cef_v8_binding.cpp").read_text(encoding="utf-8")
        for retired in ("CefEditorApiEndpoint", "cefQuery", "PyImport_Import", "dispatch_from_cpp"):
            self.assertNotIn(retired, binding)
        self._assert_named_v8_frontend()

    def _frontend_bridge_source(self):
        frontend_root = self._repo_root() / "editor" / "Frontend" / "src"
        source_paths = [
            frontend_root / "api" / "editorApi.js",
        ]
        for directory, names in {
            "services": {
                "appService.js",
                "aiService.js",
                "projectLauncherService.js",
            },
            "stores": set(),
            "views/layout": {"JoinGame.vue"},
        }.items():
            source_paths.extend(frontend_root / directory / name for name in sorted(names))
        sources = [path.read_text(encoding="utf-8") for path in source_paths]
        # These tests verify delegation and ownership. Keep them stable across
        # harmless formatting changes and private unwrap-helper renames.
        variants = []
        for source in sources:
            variants.extend((source, source.replace("=> ", "=>\n    "), source.replace("unwrap", "_unwrap")))
        return "\n".join(variants)

    def _assert_named_v8_frontend(self):
        frontend = self._frontend_bridge_source()
        self.assertIn("window.corona", frontend)
        for retired in ("Bridge.", "call_editor_api(", "editorApi.invoke(",
                        "ensureEditorApiMethod", "ensureEditorApiEvent"):
            self.assertNotIn(retired, frontend)
        binding = (self._engine_root() / "src/systems/ui/cef/cef_v8_binding.cpp").read_text(encoding="utf-8")
        self.assertIn('reject(context_, promise, "INVALID_ARGUMENT"', binding)
        self.assertIn('"PERMISSION_DENIED"', binding)
        self.assertNotIn('root->SetValue("editor"', binding)
        self.assertIn("Generic API methods must not be exposed to Web callers", self._editor_api_source())

    def _network_system_source(self):
        return (
            self._engine_root()
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
            "scene_tools": "SceneTools",
            "network": "Network",
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

    def test_camera_lock_shares_the_native_actor_property_authority(self):
        manifest = self._editor_api_source()
        handler = self._handler_source()
        frontend = self._frontend_bridge_source()

        self.assertIn(
            'set_actor_camera_lock, kSceneActorCameraLockParams, ""',
            manifest,
        )
        self.assertIn('{"set_actor_camera_lock",', handler)
        self.assertIn("camera_lock_enabled", handler)
        self.assertIn("controller.set_target", handler)
        self.assertIn("setActorProperties: (sceneName, actorName, patch)", frontend)
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
        self._assert_named_v8_frontend()
        python_api_source = (
            self._repo_root() / "editor" / "api" / "editor_api.py"
        ).read_text(encoding="utf-8")


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
        self._assert_named_v8_frontend()
        python_api_source = (
            self._repo_root() / "editor" / "api" / "editor_api.py"
        ).read_text(encoding="utf-8")


        self.assertIn("def _ensure_cpp_api_event(event_name):", python_api_source)
        self.assertIn("def _ensure_cpp_api_events():", python_api_source)
        self.assertIn('_invoke_cpp_api("EditorApi.list_events", [], validate_method=False)', python_api_source)
        self.assertIn("_ensure_cpp_api_event(event_name)", python_api_source)
        self.assertIn("for event_spec in _ensure_cpp_api_events().values():", python_api_source)
        self.assertIn("Editor API event is not defined by C++ manifest", python_api_source)

    def test_frontend_event_wrapper_paths_are_validated_against_cpp_manifest(self):
        self._assert_named_v8_frontend()
        binding = (self._engine_root() / "src/systems/ui/cef/cef_v8_binding.cpp").read_text(encoding="utf-8")
        for snippet in (
            'window->SetValue("onClosed"',
            '"INVALID_ARGUMENT: Expected exactly one event callback"',
            '"PERMISSION_DENIED: Event binding belongs to another context"',
            "arguments[0]->IsFunction()",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, binding)

    def test_callback_wrappers_validate_event_payloads_against_cpp_manifest_schema(self):
        self._assert_named_v8_frontend()
        python_api_source = (
            self._repo_root() / "editor" / "api" / "editor_api.py"
        ).read_text(encoding="utf-8")


        self.assertIn("def _validate_cpp_api_event_payload(event_name, payload, event_spec):", python_api_source)
        self.assertIn("_validate_cpp_api_event_payload(event_name, payload, event_spec)", python_api_source)
        self.assertIn("Editor API event payload schema mismatch", python_api_source)
        self.assertNotIn("payload if isinstance(payload, dict) else {}", python_api_source)

    def test_wrappers_validate_allowed_callers_against_cpp_manifest(self):
        self._assert_named_v8_frontend()
        api_header = self._editor_api_header()
        api_source = self._editor_api_source()
        python_api_source = (
            self._repo_root() / "editor" / "api" / "editor_api.py"
        ).read_text(encoding="utf-8")


        self.assertIn("PythonScript = 1u << 1u", api_header)
        self.assertNotIn("EditorApiCaller::Python)", api_source)
        self.assertNotIn("EditorApiCaller::Python,", api_source)
        self.assertIn("_CPP_EDITOR_API_CALLER_PYTHON_SCRIPT = 2", python_api_source)
        self.assertIn("def _validate_cpp_api_caller(name, spec, caller_mask, caller_name):", python_api_source)
        self.assertIn("_validate_cpp_api_caller(api_name, spec, caller_mask, caller_name)", python_api_source)
        self.assertIn("_validate_cpp_api_caller(event_name, event_spec, _CPP_EDITOR_API_CALLER_PYTHON_SCRIPT, \"PythonScript\")", python_api_source)
        self.assertIn("Editor API caller is not allowed by C++ manifest", python_api_source)

    def test_wrappers_validate_arguments_against_cpp_manifest_schema_before_invocation(self):
        self._assert_named_v8_frontend()
        python_api_source = (
            self._repo_root() / "editor" / "api" / "editor_api.py"
        ).read_text(encoding="utf-8")


        self.assertIn("def _validate_cpp_api_args(\n    api_name,\n    args,", python_api_source)
        self.assertIn("spec = _ensure_cpp_api_method(api_name)", python_api_source)
        self.assertIn("param.get(\"optional\")", python_api_source)
        self.assertIn("Editor API argument schema mismatch", python_api_source)

    def test_wrappers_validate_returns_against_cpp_manifest_schema_after_invocation(self):
        self._assert_named_v8_frontend()
        python_api_source = (
            self._repo_root() / "editor" / "api" / "editor_api.py"
        ).read_text(encoding="utf-8")


        self.assertIn("def _validate_cpp_api_return(api_name, data, spec):", python_api_source)
        self.assertIn("_validate_cpp_api_return(api_name, data, spec)", python_api_source)
        self.assertIn("Editor API return schema mismatch", python_api_source)

    def test_frontend_wrapper_rejects_schema_validation_errors(self):
        self._assert_named_v8_frontend()
        binding = (self._engine_root() / "src/systems/ui/cef/cef_v8_binding.cpp").read_text(encoding="utf-8")
        self.assertIn("RejectPromise", binding)
        self.assertIn("INVALID_ARGUMENT", binding)

    def test_frontend_callback_unregister_releases_local_token_even_on_failure(self):
        binding = (self._engine_root() / "src/systems/ui/cef/cef_v8_binding.cpp").read_text(encoding="utf-8")
        lifetime = (self._repo_root() / "editor/Frontend/src/utils/viewLifetime.js").read_text(encoding="utf-8")
        self.assertIn('CefV8Value::CreateFunction("unsubscribe", new NativeEventHandler(endpoint, event_, token))', binding)
        self.assertIn("endpoint->unsubscribe(token_, exception)", binding)
        self.assertIn("subscribers_.erase(id)", binding)
        self.assertIn("subscriptions.clear()", lifetime)
        self.assertIn("release().catch(error =>", lifetime)
        self.assertIn("await unregister(token)", lifetime)
        self.assertIn("await release();\n      return null;", lifetime)

    def test_editor_api_has_no_python_backend(self):
        api_source = self._editor_api_source()
        api_header = self._editor_api_header()

        native_rpc_source = (
            self._engine_root() / "src" / "systems" / "ui" / "cef" / "cef_editor_native_api_registry.cpp"
        ).read_text(encoding="utf-8")
        editor_source = (
            self._repo_root() / "editor" / "runtime" / "editor_host.py"
        ).read_text(encoding="utf-8")
        bind_source = (
            self._engine_root() / "src" / "systems" / "ui" / "cef" / "cef_py_bind.cpp"
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
        self._assert_retired_query_transport()
        self.assertIn("Generic API methods must not be exposed to Web callers", self._editor_api_source())

    def test_cef_query_bridge_has_no_python_fallback(self):
        self._assert_retired_query_transport()

    def test_editor_api_core_declares_cpp_defined_endpoint_contract(self):
        header = self._editor_api_header()
        source = self._editor_api_source()
        cmake = (
            self._engine_root() / "src" / "systems" / "ui" / "CMakeLists.txt"
        ).read_text(encoding="utf-8")

        for symbol in (
            "EditorApiRegistry",
            "EditorApiMethodSpec",
            "EditorApiEndpointBase",
            "PythonScriptApiClientEndpoint",
            "register_python_script_service_dispatcher",
            "unregister_python_script_service_dispatcher",
        ):
            with self.subTest(symbol=symbol):
                self.assertIn(symbol, header)

        self.assertNotIn("CefEditorApiEndpoint", header)
        self.assertNotIn("CefEditorApiEndpoint", source)
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
        self.assertIn("EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(ProjectLauncher, get_app_version", source)
        self.assertIn("EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, list_actor_tree", source)
        self.assertIn(
            "EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(MainView, scene_save, kSceneSaveParams",
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
        self._assert_named_v8_frontend()
        frontend = self._frontend_bridge_source()
        self.assertNotIn("callEditorApi", frontend)
        self.assertNotIn("EditorApi.unregister_callback", frontend)

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


    def test_frontend_native_calls_resolve_to_named_v8_bindings(self):
        frontend = (self._repo_root() / "editor/Frontend/src/api/editorApi.js").read_text(encoding="utf-8")
        binding = (self._engine_root() / "src/systems/ui/cef/cef_v8_binding.cpp").read_text(encoding="utf-8")
        roots = dict(re.findall(r'root->SetValue\("(\w+)",\s*(\w+),', binding))
        installed = set()
        for owner, member, function in re.findall(
            r'(\w+)->SetValue\("(\w+)",\s*CefV8Value::CreateFunction\("(\w+)"', binding
        ):
            self.assertEqual(member, function)
            for namespace, variable in roots.items():
                if variable == owner:
                    installed.add((namespace, member))
        calls = set(re.findall(r"window\.corona\.(\w+)\.(\w+)\(", frontend))
        self.assertGreater(len(calls), 60)
        self.assertEqual(sorted(calls - installed), [])

    def test_production_consumers_only_use_declared_static_editor_methods(self):
        root = self._repo_root() / "editor/Frontend/src"
        facade = (root / "api/editorApi.js").read_text(encoding="utf-8")
        declarations = set()
        sections = re.split(r"(?m)^  (\w+): \{", facade)
        for index in range(1, len(sections), 2):
            namespace, body = sections[index:index + 2]
            for method in re.findall(r"(?m)^    (\w+):", body):
                declarations.add((namespace, method))
        consumers = set()
        missing = []
        for path in root.rglob("*"):
            if path.suffix not in (".js", ".vue"):
                continue
            source = path.read_text(encoding="utf-8")
            for namespace, method in re.findall(r"\beditorApi\.(\w+)\.(\w+)", source):
                consumers.add((namespace, method))
                if (namespace, method) not in declarations:
                    missing.append(f"{path.relative_to(root)}: {namespace}.{method}")
        self.assertGreater(len(consumers), 40)
        self.assertEqual(sorted(set(missing)), [])

    def test_named_facade_preserves_network_audio_defaults_and_explicit_scope(self):
        source = (self._repo_root() / "editor/Frontend/src/api/editorApi.js").read_text(encoding="utf-8")
        self.assertIn("startSession: (instanceName, projectId, port = 27960, role = 'host')", source)
        self.assertNotIn("lanChat:", source)
        self.assertIn("playResource: (resourceId, loop = false)", source)
        self.assertIn("window.corona.scene.renameActor(sceneName, actor, name, scope)", source)
        self.assertIn("window.corona.scene.deleteActor(sceneName, actor, scope)", source)
        self.assertIn("window.corona.scene.save(sceneName, scope)", source)
        self.assertNotIn("localStorage", source)

    def test_frontend_facade_has_no_dynamic_manifest_or_retired_namespaces(self):
        self._assert_named_v8_frontend()
        frontend = (self._repo_root() / "editor/Frontend/src/api/editorApi.js").read_text(encoding="utf-8")
        self.assertIn("export const editorApi = editorApiStatic;", frontend)
        for retired in ("new Proxy", "call_manifest_editor_api", "create_dynamic_editor_api_namespace"):
            self.assertNotIn(retired, frontend)
        for retired in ("editor", "events", "sceneTools", "main", "ai", "resourceSearch"):
            self.assertNotRegex(frontend, rf"(?m)^\s+{retired}:\s*\{{")

    def test_frontend_script_input_uses_named_v8_dispatch(self):
        frontend = self._frontend_bridge_source()
        self.assertIn("window.corona.input.dispatch(sceneName, cameraId, event)", frontend)
        for retired in ("scripts.sendKeyEvent", "scripts.sendKeyUpEvent", "scripts.sendMouseEvent"):
            self.assertNotIn(retired, frontend)

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
                r'\{"[A-Za-z0-9_]+\.[A-Za-z0-9_]+",\s*EditorApiValueType::[A-Za-z0-9_]+,\s*all_callers\(\)\s*&\s*~caller_mask\(EditorApiCaller::Cef\),\s*"",\s*"([^"]+)"\}',
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

    def test_retired_ai_request_has_no_native_or_web_entrypoint(self):
        self.assertNotIn(("AITool", "submit_request"), self._editor_api_methods())
        frontend = self._frontend_bridge_source()
        self.assertNotIn("ai.submitRequest", frontend)
        self.assertNotIn("EditorApiBackend::Python", self._editor_api_source())
        for retired in ("sendMessageToAIStream", "readLocalFileAsBase64", "generateHint"):
            self.assertNotIn(retired, self._editor_api_source() + frontend)

    def test_python_script_services_do_not_keep_backend_alias(self):
        registry_source = (
            self._repo_root() / "editor" / "runtime" / "registry.py"
        ).read_text(encoding="utf-8")

        self.assertIn("PYTHON_SCRIPT_SERVICES = {", registry_source)
        self.assertIn("def register_python_script_services():", registry_source)
        self.assertNotIn("register_python_backends", registry_source)

    def test_cef_query_accepts_editor_api_requests_before_legacy_rpc(self):
        self._assert_retired_query_transport()
        binding = (self._engine_root() / "src/systems/ui/cef/cef_v8_binding.cpp").read_text(encoding="utf-8")
        self.assertIn("owns_current_context()", binding)
        self.assertIn("RejectPromise", binding)

    def test_old_public_rpc_contract_is_removed(self):
        repo_root = self._repo_root()
        self.assertFalse(
            (self._engine_root() / "src" / "systems" / "ui" / "cef" / "cef_editor_rpc_contract.cpp").exists()
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
        self.assertIn('return CoronaEngine.unregister_python_script_callback(callback_token)', api_source)
        self.assertIn('_invoke_manifest_cpp_api("project.get_app_version", [])', api_source)
        self.assertIn('_invoke_manifest_cpp_api("scene.list_actor_tree", [scene_name])', api_source)
        self.assertIn('_invoke_manifest_cpp_api("scene.select_actor", [scene_name, actor_type, actor_name])', api_source)
        self.assertIn('_invoke_manifest_cpp_api("main.scene_save", args)', api_source)
        self.assertIn("class _EventsApi", api_source)
        self.assertNotIn("def on_lan_chat_event", api_source)
        for retired in ("on_actor_changed", "on_actor_selection_changed",
                        "on_actor_transform_updated", "on_project_opened"):
            self.assertNotIn(f"def {retired}(", api_source)
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
            self._engine_root() / "src" / "systems" / "ui" / "cef" / "cef_py_bind.cpp"
        ).read_text(encoding="utf-8")
        python_api_source = (
            self._repo_root() / "editor" / "api" / "editor_api.py"
        ).read_text(encoding="utf-8")

        self.assertIn('m.def("_invoke_cpp_editor_api"', bind_source)
        self.assertNotIn('m.def("invoke_cpp_api"', bind_source)
        self.assertIn("CoronaEngine._invoke_cpp_editor_api(api_name, payload)", python_api_source)
        self.assertNotIn("CoronaEngine.invoke_cpp_api", python_api_source)

    def test_editor_api_manifest_is_cpp_defined_and_consumed_by_wrappers(self):
        self._assert_retired_query_transport()
        source = self._editor_api_source()
        handlers = self._handler_source()
        python = (self._repo_root() / "editor/api/editor_api.py").read_text(encoding="utf-8")
        for method in ("list_methods", "list_events"):
            self.assertIn(f"EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(EditorApi, {method}, kNoParams", source)
            self.assertIn(f'{{"{method}",', handlers)
            self.assertIn(f"EditorApiRegistry::instance().{method}()", handlers)
            self.assertIn(f'_invoke_typed_cpp_api("EditorApi.{method}", "editor.{method}", [])', python)
        for retired in ("register_callback", "unregister_callback"):
            self.assertNotRegex(source, rf"EDITOR_API_METHOD[^\n]*\(EditorApi, {retired},")
            self.assertNotIn(f'{{"{retired}",', handlers)
        self.assertIn("CoronaEngine.register_python_script_callback(event_name, _dispatch)", python)
        self.assertIn("CoronaEngine.unregister_python_script_callback(callback_token)", python)

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
            self._engine_root() / "src" / "systems" / "ui" / "cef" / "cef_py_bind.cpp"
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

        for method, params, alias, value_type in (
            ("ProjectLauncher, get_app_version", "kNoParams", "project.get_app_version", "String"),
            ("SceneTools, list_actor_tree", "kReadTreeParams", "scene.list_actor_tree", "Array"),
            ("MainView, scene_save", "kSceneSaveParams", "main.scene_save", "Object"),
        ):
            with self.subTest(method=method):
                declaration = next(line for line in source.splitlines()
                                   if "EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(" + method + "," in line)
                self.assertIn(params, declaration)
                self.assertIn('"' + alias + '"', declaration)
                self.assertIn("EditorApiValueType::" + value_type, declaration)
                self.assertRegex(declaration, r"non_cef_|~caller_mask\(EditorApiCaller::Cef\)")
        self.assertNotIn("SceneTools.actorChanged", source)
        self.assertNotIn("ProjectLauncher.projectOpened", source)
        events = source.split("constexpr auto kEditorApiEvents =", 1)[1].split("\n});", 1)[0]
        declarations = [line for line in events.splitlines() if 'EditorApiValueType::' in line]
        self.assertEqual(len(declarations), 8)
        for declaration in declarations:
            self.assertIn("~caller_mask(EditorApiCaller::Cef)", declaration)
            self.assertIn(', "", "events.', declaration)

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
            r'EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS\(Scripts, run_file, kObjectPayloadParam, ',
        )
        validation_start = source.find("NativeResult validate_editor_api_args")
        self.assertGreaterEqual(validation_start, 0)
        validation_end = source.find("NativeResult validate_editor_api_result", validation_start)
        self.assertGreater(validation_end, validation_start)
        validation_body = source[validation_start:validation_end]
        self.assertIn("spec.params == nullptr", validation_body)
        self.assertIn("Unspecified Editor API schema", validation_body)

    def test_retired_scene_methods_have_no_native_or_web_declaration(self):
        methods = self._editor_api_methods()
        handlers = self._native_rpc_methods()
        for method in ("open_actor", "set_shadow_cascade_debug", "get_shadow_cascade_debug",
                       "set_ssao_enabled", "get_ssao_enabled"):
            self.assertNotIn(("SceneTools", method), methods)
            self.assertNotIn(("SceneTools", method), handlers)

    def test_scene_tools_common_editor_apis_have_explicit_schema(self):
        source = self._editor_api_source()

        expected = {
            "SceneTools.create_actor": "kSceneToolsCreateActorParams",
            "SceneTools.remove_actor": "kSceneActorParams",
            "SceneTools.rename_actor": "kSceneToolsRenameActorParams",
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
            "register_python_script_callback",
            "emit_editor_api_event",
            "emit_python_script_event",
        ):
            with self.subTest(symbol=symbol):
                self.assertIn(symbol, header + source)

        self.assertNotIn("return g_next_callback_token.fetch_add(1);", source)
        self.assertNotIn("return callback_token != 0;", source)
        self.assertIn("EditorApiCallbackRegistry::instance()", source)

    def test_frontend_events_use_named_native_and_local_scene_subscriptions(self):
        source = (self._repo_root() / "editor/Frontend/src/api/editorApi.js").read_text(encoding="utf-8")
        self.assertIn("observe: async (kind, callback) => sceneReader.observe(kind, callback)", source)
        self.assertIn("off: callbackToken => sceneReader.off(callbackToken)", source)
        for retired in ("register_manifest_editor_api_callback", "window.corona.editor.on",
                        "unregister_callback", "events: {"):
            self.assertNotIn(retired, source)

    def test_tree_reads_are_scoped_and_revision_checked(self):
        source = self._handler_source()
        self.assertIn('query.at("known_revision") == scene->tree_revision', source)
        self.assertIn('data["archive_session"] = scene->archive_session', source)
        self.assertNotIn('emit_editor_api_event("SceneTools.sceneTreeChanged"', source)

    def test_scene_mutations_do_not_dispatch_actor_state_to_either_client(self):
        self.assertNotIn('emit_editor_api_event("SceneTools.actorChanged"', self._handler_source())
        source = self._editor_api_source()
        emit = re.search(r"std::size_t emit_editor_api_event\(.*?\n\}", source, re.S)
        self.assertIsNotNone(emit)
        self.assertIn("return 0;", emit.group(0))
        self.assertNotIn("SceneTools.actorChanged", source)
        self.assertNotIn("SceneTools.actorSelectionChanged", source)
        self.assertNotIn("SceneTools.actorTransformUpdated", source)

    def test_actor_selection_uses_native_authority_and_local_observation(self):
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

        declaration = next(line for line in source.splitlines()
                           if "EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, select_actor," in line)
        self.assertIn("kSceneToolsSelectActorParams", declaration)
        self.assertIn('"scene.select_actor"', declaration)
        self.assertIn("EditorApiValueType::Object", declaration)
        self.assertIn("constexpr std::array<EditorApiParamSpec, 4> kSceneToolsSelectActorParams", source)
        for retired in ("SceneTools.actorSelectionChanged", "onActorSelectionChanged", "on_actor_selection_changed"):
            self.assertNotIn(retired, source + bridge_source + python_api_source)

        self.assertIn('{"select_actor", [](const NativeRequest& request, const NativeContext&)', handler_source)
        self.assertIn("scene->selection = reply.selection;", handler_source)
        self.assertIn("execute_scene_operation({SceneMethod::SelectActor, std::move(selection)})", handler_source)
        self.assertIn('{"actor_type", actor_type}', handler_source)
        self.assertIn('{"scene", scene_name}', handler_source)
        self.assertIn('{"actor", actor_name}', handler_source)

        self.assertIn("editorApi.scene.selectActor(sceneId, type, actorName, {", main_page_source)
        self.assertIn(".selectActor(currentSceneName.value, scene.type || 'actor', scene.name)", scene_bar_source)
        self.assertIn("viewLifetime.subscribe(callback => editorApi.scene.observe('selection', callback), handleSelection, editorApi.off)", object_source)
        self.assertIn("viewLifetime.dispose()", object_source)
        for source_name, frontend_source in (
            ("MainPage", main_page_source),
            ("SceneBar", scene_bar_source),
            ("Object", object_source),
        ):
            with self.subTest(source=source_name):
                self.assertNotIn("'actor-change'", frontend_source)
        self.assertNotIn("event === 'actor-change'", event_bus_source)

    def test_actor_transform_observation_is_local_and_lifetime_owned(self):
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

        for retired in ("SceneTools.actorTransformUpdated", "onActorTransformUpdated", "on_actor_transform_updated"):
            self.assertNotIn(retired, source + bridge_source + python_api_source + corona_editor_source)
        self.assertIn("viewLifetime.subscribe(callback => editorApi.scene.observe('transform', callback), handleTransform, editorApi.off)", object_source)
        self.assertIn("viewLifetime.dispose()", object_source)
        self.assertNotIn("coronaEventBus.on('transform-update'", object_source)
        self.assertNotIn("coronaEventBus.off('transform-update'", object_source)
        self.assertNotIn("event === 'transform-update'", event_bus_source)







    def test_log_batch_is_absent_from_native_event_manifest(self):
        self.assertNotIn('"Editor.logBatch"', self._editor_api_source())

    def test_camera_frame_timing_is_not_an_editor_api(self):
        self.assertNotIn("get_frame_timing", self._editor_api_source())
        self.assertNotIn('"get_frame_timing"', self._handler_source())

    def test_ai_uses_web_session_instead_of_retired_native_chunk_events(self):
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

        for retired in ("AI.chunk", "onAiChunk", "on_ai_chunk"):
            self.assertNotIn(retired, source + bridge_source + python_api_source + corona_editor_source)
        model = (self._repo_root() / "editor/Frontend/src/services/aiModelService.js").read_text(encoding="utf-8")
        for method in ("start", "status", "cancel"):
            self.assertIn("initializeAiSession().invoke('model." + method + "'", model)
        self.assertNotIn("event === 'ai-chunk'", event_bus_source)

    def test_project_open_is_visible_through_active_scene_query(self):
        source = self._handler_source()
        self.assertIn('{"read_active_scene",', source)
        self.assertIn('{"project_path", state.project_path}', source)
        self.assertNotIn('emit_editor_api_event("ProjectLauncher.projectOpened"', source)

    def test_network_events_keep_python_wrappers_without_web_subscriptions(self):
        source = self._editor_api_source()
        python = (self._repo_root() / "editor/api/editor_api.py").read_text(encoding="utf-8")
        frontend = self._frontend_bridge_source()
        bus = (self._repo_root() / "editor/Frontend/src/utils/eventBus.js").read_text(encoding="utf-8")
        host = (self._repo_root() / "editor/runtime/editor_host.py").read_text(encoding="utf-8")
        for event, wrapper, legacy in (
            ("actorOwnershipClaimed", "on_network_actor_ownership_claimed", "actor-ownership-claim"),
            ("syncPauseRequested", "on_network_sync_pause_requested", "network-sync-pause-request"),
            ("fileSyncStatusChanged", "on_network_file_sync_status_changed", "file-sync-status"),
            ("assetImportCompleted", "on_network_asset_import_completed", "import-asset-complete"),
            ("actorSyncBroadcastRequested", "on_network_actor_sync_broadcast_requested", "actor-sync-broadcast"),
            ("actorTransformSyncBroadcastRequested", "on_network_actor_transform_sync_broadcast_requested", "actor-transform-sync-broadcast"),
            ("actorStateSyncBroadcastRequested", "on_network_actor_state_sync_broadcast_requested", "actor-state-sync-broadcast"),
            ("actorDeleteSyncBroadcastRequested", "on_network_actor_delete_sync_broadcast_requested", "actor-delete-sync-broadcast"),
        ):
            with self.subTest(event=event):
                self.assertIn(f'{{"Network.{event}", EditorApiValueType::Object, all_callers() & ~caller_mask(EditorApiCaller::Cef), "", "events.{wrapper}"}}', source)
                self.assertIn(f'return _register_manifest_editor_api_event_callback("events.{wrapper}", callback)', python)
                self.assertNotIn("onNetwork" + event[0].upper() + event[1:], frontend)
                self.assertNotIn(f"event === '{legacy}'", bus)
                self.assertNotIn(f'"{legacy}": ("events.{wrapper}"', host)




    def test_web_pick_uses_owned_v8_results_without_legacy_events_or_json_broker(self):
        root = self._engine_root()
        self._assert_retired_query_transport()
        v8 = (root / "src/systems/ui/cef/cef_v8_binding.cpp").read_text(encoding="utf-8")
        frontend = self._frontend_bridge_source()
        for retired in ("actorPickResult", "onActorPickResult", "focusPoseResult"):
            self.assertNotIn(retired, self._editor_api_source() + v8 + frontend)
        self.assertFalse((root / "src/systems/ui/cef/request_response_broker.cpp").exists())
        self.assertIn('viewport_pick_value', v8)
        self.assertIn('begin_viewport_pick', v8)
        self.assertIn('window.corona.viewport.pick', frontend)

    def test_focus_and_pick_frontend_consumers_use_direct_native_results(self):
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

        self.assertIn("editorApi.scene.focusActor", scene_bar_source)
        self.assertIn("camera-pose-applied", scene_bar_source)
        self.assertNotIn("camera-pose-request", scene_bar_source + main_page_source)
        self.assertNotIn("editorApi.events.onFocusPoseResult", scene_bar_source)
        self.assertNotIn("handleFocusPoseResult", scene_bar_source)
        self.assertNotIn("coronaEventBus.on('focus-pose-result'", scene_bar_source)
        self.assertNotIn("coronaEventBus.off('focus-pose-result'", scene_bar_source)

        self.assertIn("onResult: (payload, result) => handleActorPickResult(payload, result)", main_page_source)
        self.assertNotIn("actorPickResultCallbackToken", main_page_source)
        self.assertNotIn("coronaEventBus.on('actor-pick-result'", main_page_source)
        self.assertNotIn("coronaEventBus.off('actor-pick-result'", main_page_source)

    def test_retired_multiscene_events_have_no_wrappers_or_aliases(self):
        from api.editor_api import _EventsApi
        host = (self._repo_root() / "editor/runtime/editor_host.py").read_text(encoding="utf-8")
        for name in ("on_scene_added", "on_scene_renamed"):
            self.assertFalse(hasattr(_EventsApi, name))
            self.assertNotIn(name, host)
        for name in ("SceneTools.sceneAdded", "SceneTools.sceneRenamed"):
            self.assertNotIn(name, self._editor_api_source())


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

        self.assertIn("viewLifetime.subscribe(callback => editorApi.scene.observe('tree', callback), onSceneTreeChangedEvent, editorApi.off)", scene_bar_source)
        self.assertIn("viewLifetime.dispose()", scene_bar_source)
        self.assertIn("const sceneName = payload?.scene ?? payload;", scene_bar_source)
        self.assertNotIn("coronaEventBus.on('scene-tree-changed', onSceneTreeChangedEvent)", scene_bar_source)
        self.assertNotIn("coronaEventBus.off('scene-tree-changed', onSceneTreeChangedEvent)", scene_bar_source)

    def test_network_host_session_seeds_native_editor_actors(self):
        source = self._handler_source()
        start = source.find('{"start_session"')
        end = source.find('        }},', start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        handler = source[start:end]

        self.assertIn("execute_legacy_network_operation(typed)", handler)
        owner = source.split("NetworkReply execute_network_operation(", 1)[1].split("case NetworkMethod::StopSession:", 1)[0]
        self.assertIn("seed_network_editor_state(*sys, role)", owner)
        self.assertIn("reply.ok && !was_active", owner)
        self.assertIn("reply.seeded_actor_count", owner)
        self.assertIn("is_network_editor_seed_actor(actor)", source)

    def test_network_pending_actor_operations_apply_before_acknowledgement(self):
        source = self._handler_source()
        for handler_name, apply_call, ack_call in (
            ("poll_pending_actor_create", "apply_pending_network_actor_create", "ack_pending_actor_create"),
            ("poll_pending_actor_state_update", "apply_pending_network_actor_state", "ack_pending_actor_state_update"),
            ("poll_pending_actor_transform", "apply_pending_network_actor_transform", "ack_pending_actor_transform_update"),
            ("poll_pending_actor_delete", "apply_pending_network_actor_delete", "ack_pending_actor_delete"),
        ):
            with self.subTest(handler=handler_name):
                start = source.find(f'{{"{handler_name}",')
                end = source.find('        }},', start)
                self.assertGreaterEqual(start, 0)
                self.assertGreater(end, start)
                handler = source[start:end]
                self.assertIn(apply_call, handler)
                self.assertIn(ack_call, handler)
                self.assertLess(handler.find(apply_call), handler.find(ack_call))

        self.assertIn("peek_pending_actor_create", source)
        self.assertIn("peek_pending_actor_state_update", source)
        self.assertIn("peek_pending_actor_transform_update", source)
        self.assertIn("peek_pending_actor_delete", source)

    def test_network_pending_actor_apply_covers_editor_fields_and_retry_policy(self):
        source = self._handler_source()
        state_start = source.rfind("PendingNetworkActorApplyOutcome apply_pending_network_actor_state(")
        state_end = source.find("apply_pending_network_actor_delete(", state_start)
        self.assertGreaterEqual(state_start, 0)
        self.assertGreater(state_end, state_start)
        state = source[state_start:state_end]
        for token in (
            "merge_patch",
            "set_position",
            "set_rotation",
            "set_scale",
            "set_visible",
            "set_follow_camera",
            "apply_native_actor_optics_state",
            "set_mass",
            "set_restitution",
            "set_damping",
            "set_body_type",
            "set_linear_lock",
            "set_angular_lock",
            "camera_lock_position_offset",
        ):
            self.assertIn(token, state)
        self.assertNotIn("persist_native_scene_actors", state)

        create_start = source.find("apply_pending_network_actor_create(")
        create_end = source.find("apply_pending_network_actor_transform(", create_start)
        create = source[create_start:create_end]
        self.assertIn('actor_data["skip_if_exists"] = true', create)
        self.assertIn("register_actor_identity", create)
        self.assertIn("apply_pending_network_actor_state", create)
        self.assertIn("PendingNetworkActorApplyResult::Retry", create)

        delete_start = state_end
        delete_end = source.find("void register_", delete_start)
        delete = source[delete_start:delete_end]
        self.assertIn("if (!find_native_actor", delete)
        self.assertIn("PendingNetworkActorApplyResult::Applied", delete)

    def test_native_network_polling_retains_retrying_updates(self):
        source = self._handler_source()
        for method in ("poll_pending_actor_create", "poll_pending_actor_transform",
                       "poll_pending_actor_delete", "poll_pending_actor_state_update"):
            with self.subTest(method=method):
                start = source.index('{"' + method + '",')
                end = source.index('        }},', start)
                handler = source[start:end]
                self.assertIn("peek_pending_actor_", handler)
                self.assertIn('payload["retrying"]', handler)
                self.assertRegex(handler, r'if \(outcome.result != PendingNetworkActorApplyResult::Retry\)\s*\{\s*sys->ack_pending_actor_')

    def test_network_actor_delete_clears_pending_state_updates(self):
        source = (
            self._engine_root()
            / "src"
            / "systems"
            / "network"
            / "network_system.cpp"
        ).read_text(encoding="utf-8")
        callback_start = source.find("set_on_editor_operation_applied(")
        callback_end = source.find("// Wire up PeerManager", callback_start)
        self.assertGreaterEqual(callback_start, 0)
        self.assertGreater(callback_end, callback_start)
        callback = source[callback_start:callback_end]
        self.assertIn("pending_actor_state_updates", callback)
        self.assertIn("PendingActorStateUpdate", callback)

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

        self.assertIn("viewLifetime.subscribe(callback => editorApi.scene.observe('selection', callback), onActorChangeEvent, editorApi.off)", scene_bar_source)
        self.assertIn("viewLifetime.dispose()", scene_bar_source)
        self.assertIn("const type = payload?.actor_type ?? payload?.type ?? payload;", scene_bar_source)
        self.assertIn("const sceneId = payload?.scene ?? maybeSceneId;", scene_bar_source)
        self.assertNotIn("coronaEventBus.on('actor-change', onActorChangeEvent)", scene_bar_source)
        self.assertNotIn("coronaEventBus.off('actor-change', onActorChangeEvent)", scene_bar_source)


    def test_scene_tools_scene_camera_editor_apis_have_explicit_schema(self):
        source = self._editor_api_source()

        expected = {
            "SceneTools.list_scene_tree": ("kReadTreeParams", "EditorApiValueType::Object"),
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


    def test_scene_management_calls_named_bindings_with_explicit_scope(self):
        source = (self._repo_root() / "editor/Frontend/src/api/editorApi.js").read_text(encoding="utf-8")
        for call in (
            "window.corona.scene.readActorTree(sceneName, scope)",
            "window.corona.scene.readTree(sceneName, scope)",
            "window.corona.scene.createActor(sceneName, path, options)",
            "window.corona.scene.deleteActor(sceneName, actor, scope)",
            "window.corona.scene.createCamera(sceneName, options)",
            "window.corona.scene.readCameras(sceneName, scope)",
        ):
            with self.subTest(call=call):
                self.assertIn(call, source)
        self.assertNotIn("call_manifest_editor_api", source)
        self.assertNotIn("callProjectAssetOperation", source)


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
            "SceneTools.is_vision_available": ("kNoParams", "EditorApiValueType::Object"),
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


    def test_scene_datas_legacy_apis_are_removed_from_cpp_contract(self):
        source = self._editor_api_source()
        self.assertNotIn("SceneDatas", source)
        self.assertNotIn("scene_datas.", source)
        self.assertIn(
            "EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, save_actor",
            source,
        )
        self.assertNotIn("EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS(SceneTools, select_model_file", source)
        binding = (self._engine_root() / "src/systems/ui/cef/cef_v8_binding.cpp").read_text(encoding="utf-8")
        self.assertIn('"selectModelFile"', binding)

    def test_scene_datas_legacy_apis_are_not_exposed_to_frontend(self):
        bridge_source = self._frontend_bridge_source()
        self.assertNotIn("\n  sceneDatas: {", bridge_source)
        self.assertNotIn("sceneDatas.", bridge_source)
        self.assertIn("window.corona.scene.readActorTree(sceneName, scope)", bridge_source)
        self.assertIn("window.corona.scene.readActorProperties(sceneName, actor, scope)", bridge_source)
        self.assertNotIn("editorApi.scene.getSnapshot", bridge_source)
        self.assertNotIn("editorApi.scene.readActor(", bridge_source)

    def test_scene_datas_legacy_manifest_is_removed(self):
        bridge_source = self._frontend_bridge_source()
        self.assertNotIn("sceneDatas", bridge_source)
        cpp_source = self._editor_api_source()
        self.assertNotIn("SceneDatas", cpp_source)

    def test_scene_tree_and_property_handlers_respect_scene_route_args(self):
        source = self._handler_source()

        self.assertIn("NativeEditorScene* scene_for_request_route", source)
        expected = (
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
        }
        for api_name, (params_name, return_type) in expected.items():
            module, method = api_name.split(".", 1)
            with self.subTest(api_name=api_name):
                plain_macro = f"EDITOR_API_METHOD_SCHEMA({module}, {method}, {params_name}, {return_type})"
                wrapped_macro = f"EDITOR_API_METHOD_SCHEMA_WRAPPED({module}, {method}, {params_name},"
                self.assertTrue(
                    plain_macro in source or wrapped_macro in source
                    or f"EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS({module}, {method}, {params_name}," in source,
                    f"{api_name} should define an explicit C++ method schema",
                )

    def test_project_launcher_service_keeps_only_project_open_orchestration(self):
        source = (self._repo_root() / "editor/Frontend/src/services/projectLauncherService.js").read_text(encoding="utf-8")
        self.assertIn("openProject: (projectPath, options = {}) => {", source)
        self.assertIn("const result = await editorApi.project.openProject(projectPath, { load_policy: loadPolicy })", source)
        for method in ("getDefaultProjectPath", "browseFolder", "openProjectFile", "createProject",
                       "createWorldProject", "createMultiplayerProject", "getProjectLoadStatus", "getRecentProjects"):
            self.assertNotIn(f"  {method}:", source)


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
                    or (any(prefix in source for prefix in (wrapped_schema_prefix, wrapped_schema_prefix.replace("WRAPPED(", "WRAPPED_CALLERS("))) and return_type in source),
                    f"{api_name} must keep an explicit C++ schema",
                )


    def test_corona_editor_app_api_has_explicit_schema_and_wrapper(self):
        source = self._editor_api_source()
        declaration = next(line for line in source.splitlines() if "CoronaEditor, close_process," in line)
        self.assertIn('kNoParams, "", "app.close_process", EditorApiValueType::Null', declaration)
        self.assertIn("all_callers() & ~caller_mask(EditorApiCaller::Cef)", declaration)
        frontend = self._frontend_bridge_source()
        self.assertIn("closeProcess: () => window.corona.app.closeProcess()", frontend)
        self.assertIn("closeProcess: () => editorApi.app.closeProcess()", frontend)

    def test_project_settings_frontend_wrappers_use_named_v8(self):
        source = self._frontend_bridge_source()
        section = source.split('  projectSettings: {', 1)[1].split('\n};', 1)[0]
        for method in ('getActiveProjectInfo', 'saveActiveProjectInfo', 'readProjectDocument', 'commitProjectDocument'):
            self.assertIn(f'window.corona.projectSettings.{method}(', section)
        self.assertNotIn('call_manifest_editor_api(', section)

    def test_file_manager_editor_apis_have_explicit_schema(self):
        source = self._editor_api_source()

        expected = {
            "FileManager.create_file": ("kFileManagerCreateFileParams", "EditorApiValueType::Object"),
            "FileManager.create_folder": ("kFileManagerCreateFolderParams", "EditorApiValueType::Boolean"),
            "FileManager.delete_item": ("kFileManagerDeleteParams", "EditorApiValueType::Boolean"),
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
                    or (any(prefix in source for prefix in (wrapped_schema_prefix, wrapped_schema_prefix.replace("WRAPPED(", "WRAPPED_CALLERS("))) and return_type in source),
                    f"{api_name} must keep an explicit C++ schema",
                )

    def test_file_manager_apis_use_named_v8_with_explicit_scope(self):
        source = self._frontend_bridge_source()
        section = source.split('  files: {', 1)[1].split('  network:', 1)[0]
        for method in ('getFileTree', 'createFolder', 'createActorTemplate', 'deleteItem', 'renameItem', 'openFile'):
            self.assertIn(f'{method}: request => window.corona.files.{method}(request)', section)
        self.assertNotIn('call_manifest_editor_api(', section)
        self.assertNotIn('getProjectInfo:', section)
        self.assertNotIn('getFiles:', section)
        self.assertNotIn('createFile:', section)

    def test_file_manager_legacy_schema_retains_python_without_cef(self):
        source = self._editor_api_source()
        entries = [line for line in source.splitlines() if 'WRAPPED_CALLERS(FileManager,' in line]
        self.assertEqual(len(entries), 8)
        for entry in entries:
            self.assertIn('all_callers() & ~caller_mask(EditorApiCaller::Cef)', entry)
            self.assertIn(', "", "files.', entry)

    def test_resource_search_retains_explicit_python_schema_without_web_permission(self):
        source = self._editor_api_source()
        expected = {
            'fuzzy_search': 'kResourceSearchFuzzySearchParams', 'get_stats': 'kCallerParam',
            'image_search': 'kResourceSearchImageSearchParams', 'list_types': 'kCallerParam',
            'mark_index_dirty': 'kResourceSearchMarkIndexDirtyParams', 'prepare_index': 'kCallerParam',
            'rebuild_index': 'kCallerParam',
        }
        for method, params in expected.items():
            entry = next(line for line in source.splitlines() if f'ResourceSearch, {method},' in line)
            self.assertIn(f'WRAPPED_CALLERS(ResourceSearch, {method}, {params}, "", "resource_search.{method}"', entry)
            self.assertIn('all_callers() & ~caller_mask(EditorApiCaller::Cef)', entry)
            self.assertIn('EditorApiValueType::Object', entry)

    def test_unused_resource_search_frontend_namespace_is_retired(self):
        source = self._frontend_bridge_source()
        self.assertNotIn('resourceSearch:', source)
        self.assertNotIn("call_manifest_editor_api('resourceSearch.", source)

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
            "Network.get_discovered_peers": ("kNoParams", "EditorApiValueType::Object"),
            "Network.clear_discovered_peers": ("kNoParams", "EditorApiValueType::Object"),
            "Network.search_lan": ("kNoParams", "EditorApiValueType::Object"),
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
                    or (any(prefix in source for prefix in (wrapped_schema_prefix, wrapped_schema_prefix.replace("WRAPPED(", "WRAPPED_CALLERS("))) and return_type in source),
                    f"{api_name} must keep an explicit C++ schema",
                )






    def test_project_files_use_structured_python_service_and_scripts_keep_schema(self):
        source = self._editor_api_source()
        self.assertRegex(source, r"EDITOR_API_METHOD_SCHEMA_WRAPPED_CALLERS\(Scripts, get_status, kNoParams,")
        self.assertNotIn(("ProjectFiles", "save"), self._editor_api_methods())
        self.assertNotIn(("ProjectFiles", "load"), self._editor_api_methods())
        registry = (self._repo_root() / "editor/runtime/registry.py").read_text(encoding="utf-8")
        self.assertIn('"ProjectFiles": ("runtime.archive.script_files", "ProjectFiles")', registry)
        service = (self._repo_root() / "editor/runtime/archive/script_files.py").read_text(encoding="utf-8")
        self.assertIn("def save(cls, request):", service)
        self.assertIn("def load(cls, request):", service)
        owner = re.search(r"ProjectFilesReply execute_project_files_operation\(.*?\n\}", self._handler_source(), re.S)
        self.assertIsNotNone(owner)
        self.assertIn('invoke_python_script_service({"ProjectFiles", save ? "save" : "load", nlohmann::json::array({std::move(payload)})})', owner.group(0))
        self.assertNotIn(".dump()", owner.group(0))




    def test_main_view_editor_apis_have_explicit_schema(self):
        source = self._editor_api_source()

        expected = {
            "MainView.on_init": ("kPathOptionalParam", "EditorApiValueType::Object"),
            "MainView.scene_save": ("kSceneSaveParams", "EditorApiValueType::Object"),
        }
        for api_name, (params_name, return_type) in expected.items():
            module, method = api_name.split(".", 1)
            with self.subTest(api_name=api_name):
                plain_macro = f"EDITOR_API_METHOD_SCHEMA({module}, {method}, {params_name}, {return_type})"
                wrapped_macro = f"EDITOR_API_METHOD_SCHEMA_WRAPPED({module}, {method}, {params_name},"
                self.assertTrue(
                    plain_macro in source or wrapped_macro in source or wrapped_macro.replace("WRAPPED(", "WRAPPED_CALLERS(") in source,
                    f"{api_name} should define an explicit C++ method schema",
                )



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

    def test_editor_api_events_validate_python_payload_and_release_v8_subscriptions(self):
        source = self._editor_api_source()
        for snippet in ("validate_editor_api_event_payload", "invalid Editor API event payload",
                        "event_caller_allowed", "emit_callbacks(event_name, payload)",
                        "clear_python_script_callbacks", "g_callbacks.erase(it)"):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source)
        for retired in ("register_cef_callback", "clear_cef_callbacks_for_browser",
                        "emit_callbacks(event_name, payload, false)", "!it->second.python_script"):
            self.assertNotIn(retired, source)
        binding = (self._engine_root() / "src/systems/ui/cef/cef_v8_binding.cpp").read_text(encoding="utf-8")
        self.assertIn("release_corona_v8_binding", binding)
        self.assertIn("pending_.clear();", binding)
        self.assertIn("subscribers_.clear();", binding)

    def test_python_script_callbacks_are_owned_and_emitted_by_cpp_registry(self):
        source = self._editor_api_source()
        header = self._editor_api_header()
        bind_source = (
            self._engine_root() / "src" / "systems" / "ui" / "cef" / "cef_py_bind.cpp"
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
            'return CoronaEngine.unregister_python_script_callback(callback_token)',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, python_api_source)

    def test_python_api_shutdown_does_not_release_objects_or_take_gil_off_thread(self):
        source = (
            self._engine_root() / "src" / "systems" / "script" / "python" / "python_api.cpp"
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







    def test_python_corona_engine_loader_has_no_fallback_module(self):
        self.assertFalse((pathlib.Path(__file__).resolve().parents[4] / "editor" / "CoronaCore" / "core" / "corona_engine.py").is_file())
        return





    def test_native_actor_save_load_preserves_common_optics_fields(self):
        source = self._handler_source()
        for snippet in (
            'optics["diffuse"]',
            'optics["metallic"]',
            'optics["roughness"]',
            'optics["emission"]',
            'item["material"]["texture"]',
            'item.optics->set_diffuse',
            'item.optics->set_metallic',
            'item.optics->set_roughness',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source)


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
            r"SceneReply apply_native_actor_transform\(.*?\n\}",
            source,
            re.S,
        )
        self.assertIsNotNone(transform_body)
        self.assertIn(
            "actor->actor_version = std::max(actor->actor_version + 1, 1);",
            transform_body.group(0),
        )

    def test_native_actor_save_delivers_typed_values_to_the_python_writer(self):
        source = self._handler_source()
        save = source.split("NativeResult save_editor_scene", 1)[1].split("\n}", 1)[0]
        self.assertIn('archive_scene_values(scene)', save)
        self.assertIn('project_archive_call("save"', save)
        self.assertNotIn('SceneDocumentStore', save)

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


    def test_native_camera_save_load_preserves_vision_render_settings(self):
        source = self._handler_source()
        for snippet in (
            'item["vision_spp"]',
            'item["vision_max_depth"]',
            'item["vision_denoise"]',
            'section_value("vision_spp"',
            'section_value("vision_max_depth"',
            'section_value("vision_denoise"',
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source)


    def test_project_launcher_business_logic_is_native(self):
        source = self._handler_source()
        for snippet in (
            "void register_project_launcher_api_handlers",
            '"create_project"',
            '"create_world_project"',
            '"create_multiplayer_project"',
            '"open_project"',
            "prepare_archive_load",
            "materialize_scene_snapshot",
        ):
            with self.subTest(snippet=snippet):
                self.assertIn(snippet, source)

        rpc_source = (
            self._engine_root()
            / "src" / "systems" / "ui" / "cef" / "cef_editor_native_api_registry.cpp"
        ).read_text(encoding="utf-8")
        self.assertIn("register_project_launcher_api_handlers(registry)", rpc_source)
        api_source = self._editor_api_source()
        declaration = next(line for line in api_source.splitlines() if 'ProjectLauncher, browse_folder,' in line)
        self.assertIn('WRAPPED_CALLERS', declaration)
        self.assertIn('all_callers() & ~caller_mask(EditorApiCaller::Cef)', declaration)
        self.assertIn(', "", "project.browse_folder"', declaration)
        for method, params in (("open_project_file", "kNoParams"), ("open_project", "kOpenProjectParams")):
            declaration = next(line for line in api_source.splitlines() if f"ProjectLauncher, {method}," in line)
            self.assertIn("WRAPPED_CALLERS", declaration)
            self.assertIn(params, declaration)
            self.assertIn('all_callers() & ~caller_mask(EditorApiCaller::Cef)', declaration)
            self.assertIn(f', "", "project.{method}"', declaration)
        declaration = next(line for line in api_source.splitlines() if 'ProjectLauncher, create_project,' in line)
        self.assertIn('WRAPPED_CALLERS', declaration)
        self.assertIn('all_callers() & ~caller_mask(EditorApiCaller::Cef)', declaration)
        self.assertIn(', "", "project.create_project"', declaration)




    def test_project_launcher_open_project_returns_resolved_project_path(self):
        source = self._handler_source()

        start = source.find('{"open_project", [](const NativeRequest& request, const NativeContext&)')
        end = source.find('{"open_project_file"', start)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        handler = source[start:end]
        self.assertIn("return open_launcher_project(arg_string(request.args, 0)", handler)
        owner_start = source.index("NativeResult open_launcher_project(")
        owner_end = source.index("ProjectLauncherReply execute_project_launcher_operation", owner_start)
        handler = source[owner_start:owner_end]
        self.assertIn("auto prepared = prepare_archive_load(path, load_policy)", handler)
        self.assertIn("materialize_scene_snapshot_into_state(", handler)
        materialize_start = source.index("NativeEditorScene& materialize_scene_snapshot_into_state")
        materialize_end = source.index("NativeResult invoke_project_archive_parser", materialize_start)
        self.assertIn(
            'state.project_path = scene.read_only ? snapshot.at("source_path").get<std::string>()',
            source[materialize_start:materialize_end],
        )
        self.assertIn('{"path", state.project_path}', handler)
        self.assertNotIn("open_project_native", handler)

    def test_project_launcher_resolves_and_publishes_native_project_path(self):
        repo_root = pathlib.Path(__file__).resolve().parents[4]
        source = (
            repo_root / "editor" / "Frontend" / "src" / "services" / "projectLauncherService.js"
        ).read_text(encoding="utf-8")
        self.assertIn("const activeProjectPath = opened?.path || projectPath;", source)
        self.assertIn("worldModeService.resolve(activeProjectPath, { force: true })", source)
        self.assertIn("publishProjectChanged(activeProjectPath)", source)
        self.assertNotIn("localStorage", source)
        messages = (repo_root / "editor/Frontend/src/services/projectMessages.js").read_text(encoding="utf-8")
        self.assertIn("messages.emit(PROJECT_CHANGED_TOPIC", messages)
        self.assertIn("worldModeService.commitActiveProject(projectPath)", messages)

    def test_project_launcher_canonicalizes_settings_project_paths(self):
        source = self._handler_source()
        self.assertIn("canonical_project_dir_for_settings", source)

        for function_name in (
            "read_last_project_from_editor_ini",
            "resolve_active_project_path",
            "add_recent_project_native",
            "recent_projects_native",
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

        self._assert_retired_query_transport()
        binding = (self._engine_root() / "src/systems/ui/cef/cef_v8_binding.cpp").read_text(encoding="utf-8")
        self.assertIn("ProjectLauncherMethod::Open", binding)
        self.assertIn('"openProject"', binding)

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
        self.assertIn("await handleOpenProject(path)", body)
        self.assertIn("console.error('打开现有项目失败:'", body)

    def test_native_scene_load_enables_runtime_mesh_optimization(self):
        repo_root = pathlib.Path(__file__).resolve().parents[4]
        engine_source = (self._engine_root() / "src" / "engine.cpp").read_text(encoding="utf-8")
        parse_common_source = (
            self._engine_root()
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

    def test_camera_view_click_uses_native_pick_promise_and_disposes_controller(self):
        repo_root = self._repo_root()
        source = (
            repo_root / "editor" / "Frontend" / "src" / "views" / "tools" / "CameraView.vue"
        ).read_text(encoding="utf-8")

        for snippet in (
            "createViewportPickController",
            "indexActorsByHandle",
            "editorApi.scene.readActorIndex(sceneId, { archive_session: archive })",
            "onResult: (payload, result) => handleCameraViewActorPickResult(payload, result)",
            "cameraViewPickController.pickAt(snapshot)",
            "pickedActor",
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
            repo_root / "editor" / "Frontend" / "src" / "blockly" / "generators" / "runtime" / "block_program.py"
        ).read_text(encoding="utf-8")
        start = source.find("def handle_mouse_event(")
        end = source.find("\ndef ", start + 1)
        self.assertGreaterEqual(start, 0)
        self.assertGreater(end, start)
        body = source[start:end]

        self.assertIn('normalized_event in ("mousedown", "pointerdown", "down")', body)
        self.assertIn('normalized_event in ("mouseup", "pointerup", "up")', body)
        self.assertNotIn('normalized_event in ("click",', body)

    def test_launcher_history_is_not_used_as_a_live_scene(self):
        settings_source = (self._repo_root() / "editor/config/project_state.py").read_text(encoding="utf-8")
        self.assertNotIn("_hydrate_active_project_from_last_project", settings_source)
        self.assertNotIn("set_active_project_path", settings_source)

    def test_actor_body_type_is_persisted_through_native_and_editor(self):
        hub = (self._engine_root() / "include/corona/shared_data_hub.h").read_text(encoding="utf-8")
        panel = (self._repo_root() / "editor/Frontend/src/views/sidebar/Object.vue").read_text(encoding="utf-8")
        self.assertIn("enum class BodyType", hub)
        for value in ("Dynamic", "Kinematic", "Static", "Phantom"):
            self.assertIn("BodyType::" + value, hub)
        self.assertIn('item["body_type"]', self._handler_source())
        self.assertIn("set_body_type", self._handler_source())
        self.assertIn("normalizeBodyType", panel)
        self.assertIn("{ body_type: next }", panel)

    def test_physics_callbacks_remain_queued_and_catch_up_is_bounded(self):
        bindings = (self._engine_root() / 'src/systems/script/python/engine_bindings.cpp').read_text(encoding='utf-8')
        lifecycle = (self._engine_root() / 'src/systems/mechanics/mechanics_lifecycle.cpp').read_text(encoding='utf-8')
        self.assertNotIn('Py_AddPendingCall', bindings)
        self.assertNotIn('shared_ptr<nb::object>', bindings)
        self.assertIn('PythonRuntimeRequestKind::Callback', bindings)
        self.assertIn('callback_token', bindings)
        self.assertIn('resolve_fixed_dt', lifecycle)
        self.assertIn('kMaxCatchUpSteps', lifecycle)

    def test_synchronous_screenshot_wait_releases_the_python_gil(self):
        repo_root = self._repo_root()
        bindings_source = (
            self._engine_root() / "src" / "systems" / "script" / "python" / "engine_bindings.cpp"
        ).read_text(encoding="utf-8")

        binding_start = bindings_source.index('.def("save_screenshot_sync"')
        binding_end = bindings_source.index("\n        .def(", binding_start + 1)
        screenshot_binding = bindings_source[binding_start:binding_end]

        self.assertIn("nb::call_guard<nb::gil_scoped_release>()", screenshot_binding)

    def test_portable_scene_folder_is_wired_through_project_and_frontend_apis(self):
        editor_root = self._repo_root()
        engine_root = editor_root.parent
        handler_source = self._handler_source()
        api_source = (
            engine_root / "src" / "systems" / "ui" / "editor_api" / "cef_editor_api.cpp"
        ).read_text(encoding="utf-8")
        bridge_source = (
            editor_root / "editor" / "Frontend" / "src" / "api" / "editorApi.js"
        ).read_text(encoding="utf-8")
        self.assertIn('#include "scene_folder.h"', handler_source)
        self.assertIn('project_archive_call("project_info"', handler_source)
        self.assertIn('L"scene.ini;*.xml;*.pbrt"', handler_source)
        self.assertNotIn('L"*.json"', handler_source)
        self.assertIn('ProjectLauncher, open_project', api_source)
        self.assertIn("project.openProject", bridge_source)
        for retired in ("migrate_legacy_scene", "load_vision_scene", "import_vision_scene"):
            self.assertNotIn(retired, handler_source)
            self.assertNotIn(retired, api_source)
            self.assertNotIn(retired, bridge_source)

    def test_new_project_creation_uses_portable_scene_folder_without_mode(self):
        repo_root = self._repo_root()
        handler_source = (
            self._engine_root() / "src" / "systems" / "ui" / "cef" / "cef_editor_native_api_handlers.cpp"
        ).read_text(encoding="utf-8")
        create_start = handler_source.index('{"create_project"')
        create_end = handler_source.index('{"create_world_project"', create_start)
        create_body = handler_source[create_start:create_end]
        self.assertIn("project_archive_call(\"create_project\"", create_body)
        self.assertNotIn('data.value("mode"', create_body)
        self.assertNotIn("create_project_from_template_native", create_body)

    def test_world_creation_always_writes_the_single_editable_world_type(self):
        handler_source = self._handler_source()
        start = handler_source.index('NativeResult create_launcher_project(')
        end = handler_source.index('ProjectLauncherReply execute_project_launcher_operation(', start)
        world_body = handler_source[start:end]
        self.assertNotIn('data.value("mode"', world_body)
        self.assertNotIn('world_type == "story"', world_body)
        self.assertIn('{"type", "creative"}', world_body)
        self.assertIn('return create_launcher_project(arg_object(request.args, 0).value("prompt", std::string{}), "")', handler_source)
        self.assertIn(
            'update_editor_settings_section("General", {{"default_path", path_to_utf8(base_dir)}})',
            world_body,
        )


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
        self.assertIn("actor.load_status != ActorLoadStatus::Loaded", source)
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
        declaration = next(line for line in api_source.splitlines() if 'ProjectLauncher, get_project_load_status,' in line)
        self.assertIn('all_callers() & ~caller_mask(EditorApiCaller::Cef)', declaration)
        self.assertIn(', "", "project.get_project_load_status"', declaration)
        self.assertIn("getProjectLoadStatus: () => window.corona.project.getProjectLoadStatus()", bridge_source)
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
        self.assertIn('LAZY_PYTHON_SCRIPT_SERVICES = {"ProjectArchive"}', registry_source)
        self.assertNotIn('"AITool"', registry_source)
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
        self.assertIn('"service_initializing"', source)
        self.assertIn("return open_launcher_project(", launcher_body)
        self.assertIn("存档服务正在初始化", recent_games_source)
        self.assertIn("存档服务正在初始化", new_game_source)
        self.assertNotIn("warmup_all", editor_main_source)
        self.assertNotIn("plugins.AITool", editor_main_source)
        loader = (repo_root / "editor/runtime/plugin_loader.py").read_text(encoding="utf-8")
        self.assertIn("registry.register_active_python_script_services()", loader)
        self.assertNotIn("register_core_python_script_services", editor_main_source)
        dispatcher_registration = editor_main_source.index("editor.register_script_dispatcher()")
        remaining_registration = editor_main_source.index("reimport()")
        self.assertLess(dispatcher_registration, remaining_registration)

    def test_main_on_init_reuses_the_committed_scene_and_checks_requested_path(self):
        source = self._handler_source()
        start = source.index('{"on_init"', source.index("void register_main_view_api_handlers"))
        end = source.index('{"scene_save"', start)
        handler = source[start:end]
        self.assertIn("auto& state = native_editor_state()", handler)
        self.assertIn("absolute_normalized_path(state.scene->project_root)", handler)
        self.assertIn('else if (!state.scene)', handler)
        self.assertIn("No committed native editor scene", handler)
        self.assertIn("make_native_main_init_payload(state.scene.get())", handler)

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
        open_start = bridge.index("openProject:")
        open_end = bridge.index("\n  },", open_start)
        open_body = bridge[open_start:open_end]
        self.assertIn("options = {}", open_body)
        self.assertIn("load_policy", open_body)
        self.assertIn("openProject: (projectPath, options = {}) => {", bridge)
        self.assertIn(
            "editorApi.project.openProject(projectPath, { load_policy: loadPolicy })",
            open_body,
        )
        self.assertNotIn("editorApi.main.onInit", open_body)
        self.assertNotIn("reloadScene", open_body)
        self.assertIn("await appService.setEditorUiEnabled(false)", open_body)
        self.assertIn("await drainWorldSession()", open_body)
        self.assertLess(open_body.index("await drainWorldSession()"), open_body.index("editorApi.project.openProject"))
        self.assertIn("await publishProjectChanged(activeProjectPath)", open_body)
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
        self.assertNotIn("lanchat", new_game)
        self.assertIn("selectedProject && archiveReady", recent_games)
        self.assertIn("@click=\"openSelectedProject\"", recent_games)
        self.assertIn("bg-[#d8b86c]", recent_games)
        self.assertIn("actor.loadStatus === 'loaded'", object_panel)
        self.assertIn("collapsedSections.physics", object_panel)
        self.assertIn(".placeholder-warning", object_panel)


    def test_retired_python_scene_writer_is_removed(self):
        self.assertFalse((self._repo_root() / 'editor/runtime/legacy/entities/scene.py').exists())

    def test_actor_edits_have_no_implicit_native_save_path(self):
        source = self._handler_source()
        self.assertNotIn("persist_native_scene_", source)
        self.assertIn("save_editor_scene(*scene, false, metadata)", source)
        self.assertIn("save_editor_scene(*scene, true)", source)

    def test_portable_scene_validation_import_and_cleanup_keep_non_web_contracts(self):
        repo_root = self._repo_root()
        api_source = (
            self._engine_root() / "src" / "systems" / "ui" / "editor_api" / "cef_editor_api.cpp"
        ).read_text(encoding="utf-8")
        handler_source = (
            self._engine_root() / "src" / "systems" / "ui" / "cef" / "cef_editor_native_api_handlers.cpp"
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
            declaration = next(line for line in api_source.splitlines() if f'ProjectLauncher, {native_name},' in line)
            self.assertIn('all_callers() & ~caller_mask(EditorApiCaller::Cef)', declaration)
            self.assertIn(f', "", "project.{native_name}"', declaration)
            self.assertNotIn(wrapper, bridge_source)

    def test_duplicate_native_actor_check_happens_before_portable_asset_import(self):
        repo_root = self._repo_root()
        handler_source = (
            self._engine_root() / "src" / "systems" / "ui" / "cef" / "cef_editor_native_api_handlers.cpp"
        ).read_text(encoding="utf-8")
        start = handler_source.index("NativeResult create_native_editor_actor")
        end = handler_source.index("NativeResult remove_native_editor_actor", start)
        body = handler_source[start:end]
        self.assertLess(body.index("skip_if_exists"), body.index("SceneAssetStore store"))

    def test_scene_save_returns_structured_portable_validation_diagnostics(self):
        repo_root = self._repo_root()
        source = (
            self._engine_root() / "src" / "systems" / "ui" / "cef" / "cef_editor_native_api_handlers.cpp"
        ).read_text(encoding="utf-8")
        start = source.index('{"scene_save"')
        end = source.index('registry.register_module("MainView"', start)
        body = source[start:end]
        self.assertIn('if (!saved.success || !saved.data.value("ok", false)) return saved', body)
        self.assertIn('{"ok", false}', body)


if __name__ == "__main__":
    unittest.main()
