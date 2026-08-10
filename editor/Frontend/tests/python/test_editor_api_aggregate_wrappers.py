import unittest
from pathlib import Path


class EditorApiAggregateWrapperTests(unittest.TestCase):
    def setUp(self):
        self.source_root = Path(__file__).resolve().parents[2] / "src"
        self.api_source = (self.source_root / "api" / "editorApi.js").read_text(
            encoding="utf-8"
        )

    def test_manifest_contract_has_a_single_frontend_owner(self):
        self.assertIn("export class Bridge", self.api_source)
        self.assertIn("window.cefQuery", self.api_source)
        self.assertFalse((self.source_root / "utils" / "bridge.js").exists())
        self.assertFalse((self.source_root / "compat").exists())

    def test_services_are_thin_canonical_facades(self):
        for name in (
            "sceneService",
            "projectService",
            "appService",
            "lanChatService",
            "networkService",
            "scriptingService",
            "aiService",
            "projectLauncherService",
            "fileService",
            "projectSettingsService",
            "resourceService",
            "logService",
        ):
            path = self.source_root / "services" / f"{name}.js"
            self.assertTrue(path.is_file(), name)
            source = path.read_text(encoding="utf-8")
            self.assertNotIn("call_manifest_editor_api(", source, name)
            self.assertNotIn("window.cefQuery", source, name)

    def test_frontend_exposes_scene_and_viewport_aggregate_wrappers(self):
        for marker in (
            "getSnapshot: (sceneName = '')",
            "setActorTransform: (sceneName, actorName, transform)",
            "setActorPhysics: (sceneName, actorName, physics)",
            "setActorState: (sceneName, actorName, state)",
            "saveActor: (sceneName, actorName)",
            "selectModelFile: (sceneName, actorName, fileType = 'model')",
            "setActorCameraLock: (sceneName, actorName, cameraLock)",
            "capture: (sceneName, cameraName, camera, outputPath)",
            "setCameraPose: (sceneName, cameraName, camera)",
        ):
            self.assertIn(marker, self.api_source)

    def test_frontend_exposes_project_network_and_lan_chat_aggregates(self):
        for marker in (
            "copyExistingToData: (payload)",
            "lockObject: (objectId, userId, operation = 'modify')",
            "broadcastIntent: (userId, tooltip, position, status = 'placing_object')",
            "sendAgentReply: (payload)",
            "sendSystemMessageToHost: (payload)",
            "pollAgentTrigger: ()",
            "pollSyncEvent: ()",
        ):
            self.assertIn(marker, self.api_source)

    def test_active_consumers_use_aggregate_contracts(self):
        views = (
            self.source_root / "views" / "tools" / "CameraView.vue",
            self.source_root / "views" / "sidebar" / "Object.vue",
            self.source_root / "views" / "sidebar" / "SceneBar.vue",
            self.source_root / "views" / "layout" / "MainPage.vue",
        )
        for path in views:
            source = path.read_text(encoding="utf-8")
            self.assertNotIn("sceneService.listSceneTree", source, str(path))
            self.assertNotIn("sceneService.saveActor", source, str(path))


if __name__ == "__main__":
    unittest.main()
