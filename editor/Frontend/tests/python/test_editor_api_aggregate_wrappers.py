import unittest
from pathlib import Path


class EditorApiAggregateWrapperTests(unittest.TestCase):
    def test_manifest_contract_owner_lives_in_frontend_api_directory(self):
        source_root = Path(__file__).resolve().parents[2] / "src"
        api_source_path = source_root / "api" / "editorApi.js"
        bridge_source = (source_root / "utils" / "bridge.js").read_text(encoding="utf-8")

        self.assertTrue(api_source_path.is_file())
        api_source = api_source_path.read_text(encoding="utf-8")
        self.assertIn("export class Bridge", api_source)
        self.assertIn("window.cefQuery", api_source)
        self.assertIn("export * from '../api/editorApi.js'", bridge_source)
        self.assertNotIn("export class Bridge", bridge_source)

    def test_scene_service_has_a_dedicated_service_owner(self):
        source_root = Path(__file__).resolve().parents[2] / "src"
        service_path = source_root / "services" / "sceneService.js"
        compatibility_path = source_root / "compat" / "sceneService.js"
        api_source = (source_root / "api" / "editorApi.js").read_text(encoding="utf-8")
        bridge_source = (source_root / "utils" / "bridge.js").read_text(encoding="utf-8")

        self.assertTrue(service_path.is_file())
        self.assertTrue(compatibility_path.is_file())
        self.assertIn("../compat/sceneService.js", service_path.read_text(encoding="utf-8"))
        self.assertIn("import { editorApi } from '../api/editorApi.js'", compatibility_path.read_text(encoding="utf-8"))
        self.assertNotIn("export const sceneService", api_source)
        self.assertIn("sceneService", bridge_source)

    def test_project_service_has_a_dedicated_service_owner(self):
        source_root = Path(__file__).resolve().parents[2] / "src"
        service_path = source_root / "services" / "projectService.js"
        compatibility_path = source_root / "compat" / "projectService.js"
        api_source = (source_root / "api" / "editorApi.js").read_text(encoding="utf-8")
        bridge_source = (source_root / "utils" / "bridge.js").read_text(encoding="utf-8")

        self.assertTrue(service_path.is_file())
        self.assertTrue(compatibility_path.is_file())
        service_source = service_path.read_text(encoding="utf-8")
        compatibility_source = compatibility_path.read_text(encoding="utf-8")
        self.assertIn("../compat/projectService.js", service_source)
        self.assertIn("import { Bridge, editorApi } from '../api/editorApi.js'", compatibility_source)
        self.assertNotIn("export const projectService", api_source)
        self.assertIn("projectService", bridge_source)

    def test_app_service_has_a_dedicated_service_owner(self):
        source_root = Path(__file__).resolve().parents[2] / "src"
        service_path = source_root / "services" / "appService.js"
        compatibility_path = source_root / "compat" / "appService.js"
        api_source = (source_root / "api" / "editorApi.js").read_text(encoding="utf-8")
        bridge_source = (source_root / "utils" / "bridge.js").read_text(encoding="utf-8")

        self.assertTrue(service_path.is_file())
        self.assertTrue(compatibility_path.is_file())
        self.assertIn("../compat/appService.js", service_path.read_text(encoding="utf-8"))
        self.assertIn("import { Bridge, editorApi } from '../api/editorApi.js'", compatibility_path.read_text(encoding="utf-8"))
        self.assertNotIn("export const appService", api_source)
        self.assertIn("export { appService } from '../compat/appService.js';", bridge_source)

    def test_lan_chat_service_has_a_dedicated_service_owner(self):
        source_root = Path(__file__).resolve().parents[2] / "src"
        service_path = source_root / "services" / "lanChatService.js"
        api_source = (source_root / "api" / "editorApi.js").read_text(encoding="utf-8")
        bridge_source = (source_root / "utils" / "bridge.js").read_text(encoding="utf-8")

        self.assertTrue(service_path.is_file())
        self.assertIn("import { editorApi } from '../api/editorApi.js'", service_path.read_text(encoding="utf-8"))
        self.assertNotIn("export const lanChatService", api_source)
        self.assertIn("lanChatService", bridge_source)

    def test_network_service_has_a_dedicated_service_owner(self):
        source_root = Path(__file__).resolve().parents[2] / "src"
        service_path = source_root / "services" / "networkService.js"
        api_source = (source_root / "api" / "editorApi.js").read_text(encoding="utf-8")
        bridge_source = (source_root / "utils" / "bridge.js").read_text(encoding="utf-8")

        self.assertTrue(service_path.is_file())
        self.assertIn("import { editorApi } from '../api/editorApi.js'", service_path.read_text(encoding="utf-8"))
        self.assertNotIn("export const networkService", api_source)
        self.assertIn("networkService", bridge_source)

    def test_scripting_service_has_a_dedicated_service_owner(self):
        source_root = Path(__file__).resolve().parents[2] / "src"
        service_path = source_root / "services" / "scriptingService.js"
        compatibility_path = source_root / "compat" / "scriptingService.js"
        api_source = (source_root / "api" / "editorApi.js").read_text(encoding="utf-8")
        bridge_source = (source_root / "utils" / "bridge.js").read_text(encoding="utf-8")

        self.assertTrue(service_path.is_file())
        self.assertTrue(compatibility_path.is_file())
        self.assertIn("../compat/scriptingService.js", service_path.read_text(encoding="utf-8"))
        self.assertIn("import { editorApi } from '../api/editorApi.js'", compatibility_path.read_text(encoding="utf-8"))
        self.assertNotIn("export const scriptingService", api_source)
        self.assertIn("scriptingService", bridge_source)

    def test_ai_service_has_a_dedicated_service_owner(self):
        source_root = Path(__file__).resolve().parents[2] / "src"
        service_path = source_root / "services" / "aiService.js"
        api_source = (source_root / "api" / "editorApi.js").read_text(encoding="utf-8")
        bridge_source = (source_root / "utils" / "bridge.js").read_text(encoding="utf-8")

        self.assertTrue(service_path.is_file())
        self.assertIn("import { editorApi } from '../api/editorApi.js'", service_path.read_text(encoding="utf-8"))
        self.assertNotIn("export const aiService", api_source)
        self.assertNotIn("export const aiClient", api_source)
        self.assertIn("aiService", bridge_source)

    def test_project_launcher_service_has_a_dedicated_service_owner(self):
        source_root = Path(__file__).resolve().parents[2] / "src"
        service_path = source_root / "services" / "projectLauncherService.js"
        api_source = (source_root / "api" / "editorApi.js").read_text(encoding="utf-8")
        bridge_source = (source_root / "utils" / "bridge.js").read_text(encoding="utf-8")

        self.assertTrue(service_path.is_file())
        self.assertIn("import { editorApi } from '../api/editorApi.js'", service_path.read_text(encoding="utf-8"))
        self.assertNotIn("export const projectLauncherService", api_source)
        self.assertIn("projectLauncherService", bridge_source)

    def test_file_service_has_a_dedicated_service_owner(self):
        source_root = Path(__file__).resolve().parents[2] / "src"
        service_path = source_root / "services" / "fileService.js"
        compatibility_path = source_root / "compat" / "fileService.js"
        api_source = (source_root / "api" / "editorApi.js").read_text(encoding="utf-8")
        bridge_source = (source_root / "utils" / "bridge.js").read_text(encoding="utf-8")

        self.assertTrue(service_path.is_file())
        self.assertTrue(compatibility_path.is_file())
        service_source = service_path.read_text(encoding="utf-8")
        compatibility_source = compatibility_path.read_text(encoding="utf-8")
        self.assertIn("../compat/fileService.js", service_source)
        self.assertIn("import { editorApi } from '../api/editorApi.js'", compatibility_source)
        self.assertNotIn("export const fileService", api_source)
        self.assertIn("export { fileService } from '../compat/fileService.js';", bridge_source)

    def test_project_settings_service_has_a_dedicated_service_owner(self):
        source_root = Path(__file__).resolve().parents[2] / "src"
        service_path = source_root / "services" / "projectSettingsService.js"
        compatibility_path = source_root / "compat" / "projectSettingsService.js"
        api_source = (source_root / "api" / "editorApi.js").read_text(encoding="utf-8")
        bridge_source = (source_root / "utils" / "bridge.js").read_text(encoding="utf-8")

        self.assertTrue(service_path.is_file())
        self.assertTrue(compatibility_path.is_file())
        service_source = service_path.read_text(encoding="utf-8")
        compatibility_source = compatibility_path.read_text(encoding="utf-8")
        self.assertIn("../compat/projectSettingsService.js", service_source)
        self.assertIn("import { editorApi } from '../api/editorApi.js'", compatibility_source)
        self.assertNotIn("export const projectSettingsService", api_source)
        self.assertIn(
            "export { projectSettingsService } from '../compat/projectSettingsService.js';",
            bridge_source,
        )

    def test_resource_service_has_a_dedicated_service_owner(self):
        source_root = Path(__file__).resolve().parents[2] / "src"
        service_path = source_root / "services" / "resourceService.js"
        api_source = (source_root / "api" / "editorApi.js").read_text(encoding="utf-8")
        bridge_source = (source_root / "utils" / "bridge.js").read_text(encoding="utf-8")

        self.assertTrue(service_path.is_file())
        service_source = service_path.read_text(encoding="utf-8")
        self.assertIn("import { editorApi } from '../api/editorApi.js'", service_source)
        self.assertNotIn("export const resourceService", api_source)
        self.assertIn("const RESOURCE_SEARCH_ENABLED = false", service_source)
        self.assertIn("resource_search_disabled", service_source)
        self.assertIn("export { resourceService } from '../services/resourceService.js';", bridge_source)

    def test_log_service_has_a_dedicated_service_owner(self):
        source_root = Path(__file__).resolve().parents[2] / "src"
        service_path = source_root / "services" / "logService.js"
        compatibility_path = source_root / "compat" / "logService.js"
        api_source = (source_root / "api" / "editorApi.js").read_text(encoding="utf-8")
        bridge_source = (source_root / "utils" / "bridge.js").read_text(encoding="utf-8")

        self.assertTrue(service_path.is_file())
        self.assertTrue(compatibility_path.is_file())
        service_source = service_path.read_text(encoding="utf-8")
        compatibility_source = compatibility_path.read_text(encoding="utf-8")
        self.assertNotIn("export const logService", api_source)
        self.assertNotIn("export const logService", service_source)
        self.assertIn("disabled: true", compatibility_source)
        self.assertIn("../compat/logService.js", service_source)
        self.assertIn("export { logService } from '../compat/logService.js';", bridge_source)

    def test_log_view_does_not_call_the_legacy_log_facade(self):
        source_root = Path(__file__).resolve().parents[2] / "src"
        log_view_source = (source_root / "views" / "sidebar" / "LogView.vue").read_text(
            encoding="utf-8"
        )

        self.assertNotIn("services/logService", log_view_source)
        self.assertNotIn("logService.setLogReady", log_view_source)

    def test_object_panel_does_not_call_legacy_renderer_bridge_for_actor_edits(self):
        source = (
            Path(__file__).resolve().parents[2]
            / "src"
            / "views"
            / "sidebar"
            / "Object.vue"
        ).read_text(encoding="utf-8")

        self.assertNotIn("bridge.actorTransform", source)
        self.assertNotIn("bridge.setProperty", source)
        self.assertIn("editorApi.scene.setActorTransform", source)
        self.assertIn("editorApi.sceneTools.setActorPhysics", source)
        self.assertIn("await editorApi.sceneTools.setActorPhysics", source)
        self.assertIn("await editorApi.sceneTools.setActorState", source)
        self.assertIn("await editorApi.sceneTools.setActorCameraLock", source)
        self.assertIn("cameraLock.enabled ?? cameraLock.lock_to_camera", source)
        self.assertNotIn("sceneService.setCameraLock", source)
        self.assertNotIn("sceneService.setCameraLockOffset", source)

    def test_scene_bar_uses_focus_actor_aggregate(self):
        source = (
            Path(__file__).resolve().parents[2]
            / "src"
            / "views"
            / "sidebar"
            / "SceneBar.vue"
        ).read_text(encoding="utf-8")

        self.assertNotIn("bridge.computeActorFocusPose", source)
        self.assertNotIn("bridge.cameraMove", source)
        self.assertIn("editorApi.sceneTools.focusActor", source)
        self.assertNotIn("sceneService.focusActor", source)
        self.assertIn("editorApi.sceneTools.setActorState", source)

    def test_scene_tree_consumers_use_the_scene_tools_aggregate_directly(self):
        source_root = Path(__file__).resolve().parents[2] / "src"
        consumers = (
            source_root / "views" / "sidebar" / "SceneBar.vue",
            source_root / "views" / "layout" / "MainPage.vue",
            source_root / "views" / "tools" / "CameraView.vue",
        )
        for path in consumers:
            source = path.read_text(encoding="utf-8")
            self.assertNotIn("sceneService.listSceneTree", source, str(path))
            self.assertIn("editorApi.sceneTools.listSceneTree", source, str(path))

    def test_actor_save_consumers_use_the_scene_tools_aggregate_directly(self):
        source_root = Path(__file__).resolve().parents[2] / "src"
        consumers = (
            source_root / "views" / "sidebar" / "Object.vue",
            source_root / "views" / "layout" / "MainPage.vue",
            source_root / "views" / "tools" / "CameraView.vue",
        )
        for path in consumers:
            source = path.read_text(encoding="utf-8")
            self.assertNotIn("sceneService.saveActor", source, str(path))
            self.assertIn("editorApi.sceneTools.saveActor", source, str(path))

    def test_node_graph_uses_the_scene_actor_tree_contract_directly(self):
        path = (
            Path(__file__).resolve().parents[2]
            / "src"
            / "blockly"
            / "components"
            / "NodeGraphWorkspace.vue"
        )
        source = path.read_text(encoding="utf-8")
        self.assertNotIn("sceneService.listActorTree", source)
        self.assertIn("editorApi.scene.listActorTree", source)

    def test_scene_bar_creates_actors_through_the_scene_tools_contract(self):
        path = (
            Path(__file__).resolve().parents[2]
            / "src"
            / "views"
            / "sidebar"
            / "SceneBar.vue"
        )
        source = path.read_text(encoding="utf-8")
        self.assertNotIn("sceneService.createActor", source)
        self.assertIn("editorApi.sceneTools.createActor", source)

    def test_editor_views_use_scene_tools_for_camera_and_actor_commands(self):
        source_root = Path(__file__).resolve().parents[2] / "src"
        views = (
            source_root / "views" / "tools" / "CameraView.vue",
            source_root / "views" / "sidebar" / "Object.vue",
            source_root / "views" / "sidebar" / "SceneBar.vue",
            source_root / "views" / "layout" / "MainPage.vue",
        )
        legacy_commands = (
            "listCameraViews",
            "renameCameraView",
            "setRenderBackend",
            "setOutputMode",
            "setVisionRenderMode",
            "setShadowCascadeDebug",
            "setSsaoEnabled",
            "updateCameraView",
            "selectScreenshotPath",
            "saveScreenshot",
            "closeCameraView",
            "getActor",
            "renameActor",
            "selectModelFileDialog",
            "rebindActorResource",
            "openSceneActor",
            "deleteCamera",
            "createCameraView",
            "removeActor",
            "isVisionAvailable",
        )
        for path in views:
            source = path.read_text(encoding="utf-8")
            for command in legacy_commands:
                self.assertNotIn(f"sceneService.{command}", source, f"{path}: {command}")

    def test_frontend_exposes_scene_and_viewport_aggregate_wrappers(self):
        source = (
            Path(__file__).resolve().parents[2] / "src" / "api" / "editorApi.js"
        ).read_text(encoding="utf-8")

        self.assertIn(
            "getSnapshot: (sceneName = '') => call_manifest_editor_api('scene.getSnapshot', [sceneName])",
            source,
        )
        self.assertIn(
            "setActorTransform: (sceneName, actorName, transform) =>",
            source,
        )
        self.assertIn(
            "setActorPhysics: (sceneName, actorName, physics) =>",
            source,
        )
        self.assertIn(
            "call_manifest_editor_api('sceneTools.setActorPhysics', [sceneName, actorName, physics])",
            source,
        )
        self.assertIn(
            "setActorState: (sceneName, actorName, state) =>",
            source,
        )
        self.assertIn(
            "call_manifest_editor_api('sceneTools.setActorState', [sceneName, actorName, state])",
            source,
        )
        self.assertIn(
            "saveActor: (sceneName, actorName) =>",
            source,
        )
        self.assertIn(
            "call_manifest_editor_api('sceneTools.saveActor', [sceneName, actorName])",
            source,
        )
        self.assertIn(
            "selectModelFile: (sceneName, actorName, fileType = 'model') =>",
            source,
        )
        self.assertIn(
            "call_manifest_editor_api('sceneTools.selectModelFile', [sceneName, actorName, fileType])",
            source,
        )
        self.assertIn(
            "setActorCameraLock: (sceneName, actorName, cameraLock) =>",
            source,
        )
        self.assertIn(
            "call_manifest_editor_api('sceneTools.setActorCameraLock', [sceneName, actorName, cameraLock])",
            source,
        )
        self.assertIn(
            "getActor: async (sceneName, actorName) =>",
            source,
        )
        self.assertIn(
            "editorApi.scene.getSnapshot(sceneName)",
            source,
        )
        self.assertIn(
            "getActor: async (sceneName, actorName) =>",
            source,
        )
        self.assertIn(
            "capture: (sceneName, cameraName, camera, outputPath) =>",
            source,
        )
        self.assertIn(
            "setCameraPose: (sceneName, cameraName, camera) =>",
            source,
        )

    def test_scene_service_remains_a_thin_adapter(self):
        source = (
            Path(__file__).resolve().parents[2] / "src" / "compat" / "sceneService.js"
        ).read_text(encoding="utf-8")
        scene_service = source

        self.assertNotIn("call_manifest_editor_api(", scene_service)
        self.assertNotIn("cefQuery(", scene_service)
        self.assertNotIn("throw new Error", scene_service)
        self.assertNotIn("createActor:", scene_service)
        self.assertIn("editorApi.sceneTools", scene_service)
        self.assertIn("editorApi.scene", scene_service)

    def test_manifest_transport_is_centralized_in_api_owner(self):
        source_root = Path(__file__).resolve().parents[2] / "src"
        allowed_transport_files = {
            source_root / "api" / "editorApi.js",
            source_root / "compat" / "legacyEditorAdapter.js",
        }
        for path in source_root.rglob("*.js"):
            if path in allowed_transport_files:
                continue
            source = path.read_text(encoding="utf-8")
            self.assertNotIn("call_manifest_editor_api(", source, str(path))
            self.assertNotIn("window.cefQuery", source, str(path))
        for path in source_root.rglob("*.vue"):
            source = path.read_text(encoding="utf-8")
            self.assertNotIn("call_manifest_editor_api(", source, str(path))
            self.assertNotIn("window.cefQuery", source, str(path))

    def test_frontend_exposes_network_aggregate_wrappers(self):
        source = (
            Path(__file__).resolve().parents[2] / "src" / "api" / "editorApi.js"
        ).read_text(encoding="utf-8")

        self.assertIn(
            "lockObject: (objectId, userId, operation = 'modify') =>",
            source,
        )
        self.assertIn(
            "broadcastIntent: (userId, tooltip, position, status = 'placing_object') =>",
            source,
        )
        self.assertIn(
            "checkPreviewCollision: (userId, position, delta = 0.5) =>",
            source,
        )

    def test_frontend_exposes_lan_chat_transport_wrappers(self):
        source = (
            Path(__file__).resolve().parents[2] / "src" / "api" / "editorApi.js"
        ).read_text(encoding="utf-8")

        self.assertIn(
            "sendAgentReply: (payload) =>",
            source,
        )
        self.assertIn(
            "sendSystemMessageToHost: (payload) =>",
            source,
        )
        self.assertIn(
            "sendSystemMessageToUser: (payload) =>",
            source,
        )

    def test_frontend_exposes_lan_chat_queue_wrappers(self):
        source = (
            Path(__file__).resolve().parents[2] / "src" / "api" / "editorApi.js"
        ).read_text(encoding="utf-8")

        self.assertIn(
            "pollAgentTrigger: () => call_manifest_editor_api('lanChat.pollAgentTrigger', [])",
            source,
        )
        self.assertIn(
            "pollSyncEvent: () => call_manifest_editor_api('lanChat.pollSyncEvent', [])",
            source,
        )


if __name__ == "__main__":
    unittest.main()
