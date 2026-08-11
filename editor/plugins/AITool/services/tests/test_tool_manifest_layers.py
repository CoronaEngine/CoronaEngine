import unittest

from editor.plugins.AITool.services.agent_runtime.core import (
    ToolCategory,
    ToolDefinition,
    ToolResult,
)


class ToolManifestLayerTests(unittest.TestCase):
    def test_manifest_declares_runtime_internal_layer(self):
        definition = ToolDefinition(
            name="runtime.layout.apply_delta",
            handler=lambda call: ToolResult(True, "ok"),
            category=ToolCategory.GEOMETRY,
        )

        self.assertEqual(definition.as_manifest()["layer"], "runtime_internal")

    def test_manifest_declares_unknown_layer_explicitly(self):
        definition = ToolDefinition(
            name="unclassified.tool",
            handler=lambda call: ToolResult(True, "ok"),
        )

        self.assertEqual(definition.as_manifest()["layer"], "unclassified")


if __name__ == "__main__":
    unittest.main()
