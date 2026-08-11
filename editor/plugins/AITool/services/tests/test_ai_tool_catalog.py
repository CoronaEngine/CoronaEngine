import unittest

from editor.plugins.AITool.services.ai_tool_catalog import (
    ENGINE_NATIVE_TOOLS,
    PUBLIC_TOOLS,
    RUNTIME_INTERNAL_PREFIXES,
    classify_tool_layer,
    classify_tool_workflows,
)


class AIToolCatalogTests(unittest.TestCase):
    def test_runtime_tools_are_internal(self):
        for name in (
            "runtime.plan.extract",
            "runtime.layout.apply_delta",
            "scene.extract_objects",
            "batch.create",
        ):
            self.assertEqual(classify_tool_layer(name), "runtime_internal")

    def test_engine_tools_are_native(self):
        for name in (
            "import_model",
            "set_actor_transform",
            "get_scene_snapshot",
            "camera_screenshot",
        ):
            self.assertEqual(classify_tool_layer(name), "engine_native")

    def test_media_and_text_tools_are_public(self):
        for name in (
            "generate_image",
            "analyze_media",
            "generate_product_text",
            "hunyuan_generate_3d",
        ):
            self.assertEqual(classify_tool_layer(name), "public")

    def test_catalog_has_no_overlap(self):
        self.assertFalse(ENGINE_NATIVE_TOOLS & PUBLIC_TOOLS)
        self.assertTrue(RUNTIME_INTERNAL_PREFIXES)

    def test_workflow_ownership_is_explicit(self):
        self.assertEqual(classify_tool_workflows("create_node"), {"node_logic"})
        self.assertIn("conversation", classify_tool_workflows("generate_product_text"))
        self.assertIn("scene_generation", classify_tool_workflows("runtime.layout.apply_delta"))


if __name__ == "__main__":
    unittest.main()
