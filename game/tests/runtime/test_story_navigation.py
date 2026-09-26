from pathlib import Path
from types import SimpleNamespace
import sys
import unittest
from unittest.mock import Mock, patch

from game.runtime import story_navigation
from game.tests.runtime import test_subworlds as fixtures


class StoryNavigationTests(unittest.TestCase):
    save = fixtures.SubworldTests.save
    validate = fixtures.SubworldTests.validate
    assert_clean_failure = fixtures.SubworldTests.assert_clean_failure
    def setUp(self):
        fixtures.SubworldTests.setUp(self)
        self.info = {'mode': 'story', 'project_path': str(self.root), 'entrance_scene': 'scene.ini'}
        self.api = SimpleNamespace(
            project_settings=SimpleNamespace(get_active_project_info=Mock(side_effect=lambda: {'data': self.info})),
            main=SimpleNamespace(on_init=Mock(return_value={'data': {
                'scenes': [{'path': 'scene.ini'}], 'active_index': 0, 'path': 'scene.ini'}}),
                scene_save=Mock(return_value={'data': {'ok': True, 'status': 'success'}})),
            project=SimpleNamespace(validate_portable_scene=Mock(return_value={'data': {'ok': True, 'status': 'portable_v1'}})),
        )

    def test_adapter_saves_current_route_and_returns_navigation_without_opening(self):
        result = story_navigation.handle_story_key('KeyO', api=self.api)
        self.assertEqual(result['status'], 'ok', result)
        self.api.main.scene_save.assert_called_once_with('scene.ini')
        for call in self.api.project.validate_portable_scene.call_args_list:
            self.assertIs(call.args[0]['verifyHashes'], True)
        self.assertTrue(result['navigation']['created'])
        self.info['project_path'] = result['navigation']['target']
        result = story_navigation.handle_story_key('KeyP', api=self.api)
        self.assertEqual(result['navigation']['target'], str(self.root))

    def test_other_keys_and_modifiers_leave_scratch_behavior_untouched(self):
        for key, mods in [('KeyW', []), ('KeyO', ['Ctrl']), ('p', ['Alt']), ('KeyP', ['Meta'])]:
            self.assertIsNone(story_navigation.handle_story_key(key, mods, api=self.api))
        self.api.project_settings.get_active_project_info.assert_not_called()
        self.info['mode'] = 'creative'
        self.assertIsNone(story_navigation.handle_story_key('KeyO', api=self.api))
        self.api.main.scene_save.assert_not_called()

    def test_unmarked_portable_world_is_not_story_even_if_native_defaults_to_story(self):
        path = self.root / 'scene.ini'
        path.write_text(path.read_text().replace('[world]\ntype = story\n', ''))
        self.assertIsNone(story_navigation.handle_story_key('KeyO', api=self.api))
        self.api.main.scene_save.assert_not_called()

    def test_saving_failure_is_structured_and_does_not_copy(self):
        self.api.main.scene_save.return_value = {'ok': False, 'message': 'disk full'}
        result = story_navigation.handle_story_key('KeyO', api=self.api)
        self.assertEqual(result['status'], 'error')
        self.assertIn('disk full', result['message'])
        self.assert_clean_failure()

    def test_validation_diagnostic_is_reported_and_no_target_is_opened(self):
        self.api.project.validate_portable_scene.return_value = {'ok': False, 'diagnostics': [{'message': 'missing texture'}]}
        result = story_navigation.handle_story_key('KeyO', api=self.api)
        self.assertEqual(result['status'], 'error')
        self.assertIn('missing texture', result['message'])
        self.assert_clean_failure()

    def test_wrong_scene_route_does_not_save_or_reload_another_scene(self):
        self.api.main.on_init.return_value = {'scenes': [{'path': '../other/scene.ini'}], 'active_index': 0}
        result = story_navigation.handle_story_key('KeyO', api=self.api)
        self.assertEqual(result['status'], 'error')
        self.api.main.scene_save.assert_not_called()

    def test_changed_authoritative_world_does_not_save_the_old_route(self):
        self.api.main.on_init.side_effect = lambda: (self.info.update(project_path=str(self.root / 'other'))
                                                   or {'path': 'scene.ini'})
        result = story_navigation.handle_story_key('KeyO', api=self.api)
        self.assertEqual(result['status'], 'error')
        self.api.main.scene_save.assert_not_called()

    def test_concurrent_prepare_is_rejected(self):
        with story_navigation._preparing:
            result = story_navigation.handle_story_key('KeyO', api=self.api)
        self.assertEqual(result['status'], 'error')
        self.api.main.scene_save.assert_not_called()


# Keep runtime tests discoverable without importing the engine or editor at module load.
class ScratchNavigationBridgeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.editor_path = str(Path(__file__).resolve().parents[3] / 'editor')
        sys.path.insert(0, cls.editor_path)
        from script_runtime.blockly.main import ScratchTool
        from script_runtime.engine import corona_engine
        cls.tool, cls.engine = ScratchTool, corona_engine

    @classmethod
    def tearDownClass(cls):
        sys.path.remove(cls.editor_path)

    def test_o_p_delegate_to_game_only_once(self):
        response = {'status': 'noop'}
        with patch.object(story_navigation, 'handle_story_key', return_value=response) as handler:
            with patch.object(self.engine, 'handle_key_event') as scratch:
                self.assertIs(self.tool.key_event('KeyO'), response)
                handler.assert_called_once_with('KeyO', [])
                scratch.assert_not_called()

    def test_unhandled_o_p_and_other_keys_retain_original_scratch_contract(self):
        with patch.object(story_navigation, 'handle_story_key', return_value=None) as handler:
            with patch.object(self.engine, 'handle_key_event') as scratch:
                self.assertEqual(self.tool.key_event('KeyP', 'Ctrl,Shift', 'P'), {'status': 'ok'})
                handler.assert_called_once_with('KeyP', ['Ctrl', 'Shift'])
                scratch.assert_called_once_with('KeyP', ['Ctrl', 'Shift'], 'P')
                handler.reset_mock()
                self.assertEqual(self.tool.key_event('KeyW', '', 'w'), {'status': 'ok'})
                handler.assert_not_called()
                scratch.assert_called_with('KeyW', [], 'w')

    def test_release_behavior_is_unchanged(self):
        with patch.object(self.engine, 'handle_key_release') as release:
            self.assertEqual(self.tool.key_release('KeyP', 'P'), {'status': 'ok'})
            release.assert_called_once_with('KeyP', 'P')

    def test_gameplay_rpc_is_not_forwarded_to_scratch_or_navigation(self):
        from game.runtime import story_gameplay
        response = {'status': 'ok', 'state': {}}
        with patch.object(story_gameplay, 'handle_gameplay_request', return_value=response) as handler:
            with patch.object(self.engine, 'handle_key_event') as scratch:
                self.assertIs(self.tool.key_event(story_gameplay.REQUEST_KEY, '', '{"action":"load"}'), response)
                handler.assert_called_once_with('{"action":"load"}')
                scratch.assert_not_called()
