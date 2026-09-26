"""Ensure installed Python imports/frontend sources work without the source checkout."""
import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


class GameDeploymentTests(unittest.TestCase):
    def test_runtime_deploy_imports_outside_source_checkout_and_includes_hotkeys(self):
        repository = Path(__file__).resolve().parents[3]
        spec = importlib.util.spec_from_file_location('game_test_deployment', repository / 'tools/build/editor_copy_and_build.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            with contextlib.redirect_stdout(io.StringIO()):
                module.copy_game_runtime(root / 'CabbageEditor', repository)
                module.copy_game_frontend(root / 'CabbageEditor', repository)
            self.assertTrue((root / 'game/frontend/storyNavigation.mjs').is_file())
            self.assertTrue((root / 'game/runtime/story_navigation.py').is_file())
            self.assertFalse((root / 'game/tests').exists())
            code = ('import sys; sys.path.insert(0, sys.argv[1]); '
                    'from game.runtime.story_navigation import handle_story_key; '
                    'from game.runtime.subworlds import StorySubworlds; '
                    'from game import GameSession; assert GameSession(seed=1)')
            result = subprocess.run([sys.executable, '-B', '-I', '-c', code, str(root)], cwd=root,
                                    text=True, capture_output=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_source_layout_is_not_copied_onto_itself(self):
        repository = Path(__file__).resolve().parents[3]
        spec = importlib.util.spec_from_file_location('game_test_deployment', repository / 'tools/build/editor_copy_and_build.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        from unittest.mock import patch
        with patch.object(module, 'copy_tree', side_effect=AssertionError('self-copy')):
            with patch.object(module.shutil, 'copy2', side_effect=AssertionError('self-copy')):
                module.copy_game_runtime(repository / 'editor', repository)
                module.copy_game_frontend(repository / 'editor', repository)
