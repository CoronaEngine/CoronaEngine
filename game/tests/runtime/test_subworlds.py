from pathlib import Path
import json
import os
import shutil
import stat
import tempfile
import unittest
from unittest.mock import patch

from game.runtime import subworlds
from game.runtime.subworlds import CHILD, METADATA, StorySubworlds, SubworldError

SCENE = '[format]\ntype = corona_scene_folder\nversion = 1\n[world]\ntype = story\n[camera:main]\nposition = 1, 2, 3\n'


class SubworldTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / '主世界'
        self.root.mkdir()
        (self.root / 'scene.ini').write_text(SCENE, encoding='utf-8')
        (self.root / 'Assets').mkdir()
        (self.root / 'Assets/model.glb').write_bytes(b'model bytes')
        (self.root / 'assets.manifest.json').write_text('{"assets": []}')
        self.saved = []
        self.validated = []
        self.manager = StorySubworlds(save=self.save, validate=self.validate)

    def save(self, root):
        self.saved.append(root)

    def validate(self, root):
        self.validated.append(root)
        self.assertTrue((root / 'scene.ini').is_file())
        self.assertEqual((root / 'Assets/model.glb').read_bytes(), b'model bytes')

    def enter(self):
        return self.manager.prepare(self.root, 'O')

    def assert_clean_failure(self):
        self.assertFalse((self.root / CHILD).exists())
        self.assertFalse((self.root / METADATA).exists())
        self.assertFalse((self.root / '.game/story-navigation.lock').exists())
        self.assertEqual(list((self.root / '.game').glob('.subworld-staging-*')), [])
        self.assertEqual((self.root / 'scene.ini').read_text(encoding='utf-8'), SCENE)
        self.assertEqual((self.root / 'Assets/model.glb').read_bytes(), b'model bytes')

    def test_first_entry_copies_complete_saved_scene_and_resources(self):
        (self.root / '.game').mkdir()
        (self.root / '.game/private.txt').write_text('not scene content')
        (self.root / 'Scripts').mkdir()
        (self.root / 'Scripts/main.py').write_text('print(1)')
        result = self.enter()['navigation']
        child = Path(result['target'])
        self.assertTrue(result['created'])
        self.assertEqual(child, self.root / CHILD)
        self.assertEqual(self.saved, [self.root])
        self.assertEqual((child / 'scene.ini').read_bytes(), (self.root / 'scene.ini').read_bytes())
        self.assertEqual((child / 'assets.manifest.json').read_bytes(), (self.root / 'assets.manifest.json').read_bytes())
        self.assertEqual((child / 'Scripts/main.py').read_text(), 'print(1)')
        self.assertFalse((child / '.game/private.txt').exists())
        self.assertFalse((child / '.game/subworld').exists())
        self.assertFalse((child / 'Assets/model.glb').samefile(self.root / 'Assets/model.glb'))
        self.assertEqual(result['mode'], 'story')
        parent_link = json.loads((self.root / METADATA).read_text())
        child_link = json.loads((child / METADATA).read_text())
        self.assertEqual(parent_link['id'], child_link['id'])
        self.assertEqual(parent_link['target'], '.game/subworld')
        self.assertEqual(child_link['target'], '../..')

    def test_independent_scenes_and_resources_and_repeated_entry_reuses_copy(self):
        child = Path(self.enter()['navigation']['target'])
        (child / 'scene.ini').write_text(SCENE.replace('1, 2, 3', '9, 8, 7'))
        (child / 'Scripts').mkdir()
        (child / 'Scripts/child.py').write_text('unique')
        (child / 'Assets/model.glb').write_bytes(b'independent child')
        manager = StorySubworlds(save=self.save, validate=lambda _: None,
                                copy_scene=lambda *_: self.fail('must not recopy'))
        self.assertEqual(manager.prepare(child, 'P')['navigation']['target'], str(self.root))
        result = manager.prepare(self.root, 'O')['navigation']
        self.assertFalse(result['created'])
        self.assertEqual((child / 'Assets/model.glb').read_bytes(), b'independent child')
        self.assertEqual((self.root / 'Assets/model.glb').read_bytes(), b'model bytes')
        self.assertIn('9, 8, 7', (child / 'scene.ini').read_text())
        self.assertIn('1, 2, 3', (self.root / 'scene.ini').read_text())
        self.assertEqual(self.saved, [self.root, child, self.root])

    def test_noop_keys_do_not_save_or_create_nested_worlds(self):
        self.assertEqual(self.manager.prepare(self.root, 'P'), {'status': 'noop'})
        self.assertEqual(self.saved, [])
        child = Path(self.enter()['navigation']['target'])
        self.assertEqual(self.manager.prepare(child, 'O'), {'status': 'noop'})
        self.assertFalse((child / CHILD).exists())
        self.assertEqual(len(self.saved), 1)

    def test_relationship_reloads_after_moving_entire_main_world(self):
        self.enter()
        moved = self.root.with_name('moved')
        self.root.rename(moved)
        manager = StorySubworlds(save=self.save, validate=self.validate)
        self.assertEqual(manager.prepare(moved / CHILD, 'P')['navigation']['target'], str(moved))
        self.assertFalse(manager.prepare(moved, 'O')['navigation']['created'])

    def test_save_failure_never_creates_a_copy(self):
        self.manager.save = lambda _: (_ for _ in ()).throw(OSError('disk full'))
        with self.assertRaisesRegex(OSError, 'disk full'):
            self.enter()
        self.assert_clean_failure()

    def test_partial_copy_failure_cleans_only_staging_and_preserves_source(self):
        def fail_copy(source, target):
            target.mkdir()
            (target / 'scene.ini').write_text('partial')
            raise OSError('copy failed')
        self.manager.copy_scene = fail_copy
        with self.assertRaisesRegex(OSError, 'copy failed'):
            self.enter()
        self.assert_clean_failure()

    def test_failed_validation_does_not_publish_copy(self):
        def validate(path):
            if path != self.root:
                raise SubworldError('asset hash mismatch')
        self.manager.validate = validate
        with self.assertRaisesRegex(SubworldError, 'hash'):
            self.enter()
        self.assert_clean_failure()

    def test_publish_failure_rolls_back_parent_link(self):
        with patch.object(Path, 'rename', side_effect=OSError('rename failed')):
            with self.assertRaisesRegex(OSError, 'rename failed'):
                self.enter()
        self.assert_clean_failure()

    def test_parent_metadata_write_failure_rolls_back_partial_link(self):
        original = json.dump
        def write_link(link, stream, **kwargs):
            if link['role'] == 'main':
                stream.write('{partial')
                raise OSError('metadata full')
            original(link, stream, **kwargs)
        with patch.object(json, 'dump', side_effect=write_link):
            with self.assertRaisesRegex(OSError, 'metadata full'):
                self.enter()
        self.assert_clean_failure()

    def test_missing_target_does_not_recreate_it(self):
        child = Path(self.enter()['navigation']['target'])
        displaced = self.root / '.game/missing-child'
        child.rename(displaced)
        with self.assertRaisesRegex(SubworldError, '缺失'):
            self.enter()
        self.assertTrue(displaced.is_dir())
        self.assertFalse(child.exists())
        self.assertEqual(len(self.saved), 1)

    def test_corrupt_links_and_missing_reverse_are_errors_even_on_noop(self):
        child = Path(self.enter()['navigation']['target'])
        for content in ('{', '{}', '[]', '{"version": 9}',
                        json.dumps({'version': 1, 'role': 'child', 'id': 'invalid', 'target': '../..'})):
            with self.subTest(content=content):
                (child / METADATA).write_text(content)
                with self.assertRaises(SubworldError):
                    self.enter()
                with self.assertRaises(SubworldError):
                    self.manager.prepare(child, 'O')
        (child / METADATA).unlink()
        with self.assertRaisesRegex(SubworldError, '嵌套'):
            self.manager.prepare(child, 'O')
        with self.assertRaises(SubworldError):
            self.enter()

    def test_absolute_or_wrong_relative_target_is_not_followed(self):
        self.enter()
        path = self.root / METADATA
        data = json.loads(path.read_text())
        for target in (str(self.root), '../other', ''):
            data['target'] = target
            path.write_text(json.dumps(data))
            with self.assertRaises(SubworldError):
                self.enter()

    def test_unlinked_directory_collision_is_not_overwritten(self):
        target = self.root / CHILD
        target.mkdir(parents=True)
        marker = target / 'keep.txt'
        marker.write_text('keep')
        with self.assertRaisesRegex(SubworldError, '拒绝覆盖'):
            self.enter()
        self.assertEqual(marker.read_text(), 'keep')
        self.assertEqual(self.saved, [])

    def test_collision_appearing_during_copy_is_preserved(self):
        def copy(source, staged):
            subworlds._copy_scene(source, staged)
            (self.root / CHILD).mkdir()
            (self.root / CHILD / 'keep').write_text('external')
        self.manager.copy_scene = copy
        with self.assertRaisesRegex(SubworldError, '冲突'):
            self.enter()
        self.assertEqual((self.root / CHILD / 'keep').read_text(), 'external')
        self.assertFalse((self.root / METADATA).exists())
        self.assertEqual(list((self.root / '.game').glob('.subworld-staging-*')), [])

    def test_lock_conflict_does_not_remove_someone_elses_lock(self):
        (self.root / '.game').mkdir()
        lock = self.root / '.game/story-navigation.lock'
        lock.write_text('other writer')
        with self.assertRaisesRegex(SubworldError, '锁文件'):
            self.enter()
        self.assertEqual(lock.read_text(), 'other writer')
        self.assertEqual(self.saved, [])

    def test_non_story_or_legacy_scene_is_not_copied(self):
        for scene in (SCENE.replace('story', 'creative'), SCENE.replace('[world]\ntype = story\n', ''), '[Project]\nname = legacy'):
            (self.root / 'scene.ini').write_text(scene)
            with self.assertRaises(SubworldError):
                self.enter()
        self.assertEqual(self.saved, [])

    def test_child_mode_changed_is_an_error_not_an_editor_navigation(self):
        child = Path(self.enter()['navigation']['target'])
        (child / 'scene.ini').write_text(SCENE.replace('story', 'creative'))
        with self.assertRaisesRegex(SubworldError, '剧情世界'):
            self.enter()

    def test_symlink_assets_fail_closed(self):
        link = self.root / 'Assets/linked.glb'
        try:
            link.symlink_to(self.root / 'Assets/model.glb')
        except OSError:
            self.skipTest('Creating symlinks requires Windows developer mode/privileges')
        with self.assertRaises(SubworldError):
            self.enter()
        self.assert_clean_failure()

    def test_failed_exclusive_metadata_open_does_not_delete_external_file(self):
        original = subworlds._new_link
        def external_conflict(root, role, link_id):
            if role == 'main':
                (root / METADATA).write_text('external metadata')
                raise PermissionError('exclusive open denied')
            original(root, role, link_id)
        with patch.object(subworlds, '_new_link', side_effect=external_conflict):
            with self.assertRaises(PermissionError):
                self.enter()
        self.assertEqual((self.root / METADATA).read_text(), 'external metadata')
        self.assertFalse((self.root / CHILD).exists())
        self.assertEqual(list((self.root / '.game').glob('.subworld-staging-*')), [])

    @unittest.skipUnless(os.name == 'nt', 'Case-insensitive Windows filesystem')
    def test_game_directory_case_variant_is_excluded_from_copy(self):
        (self.root / '.GAME').mkdir()
        (self.root / '.GAME/private.txt').write_text('must not copy')
        child = Path(self.enter()['navigation']['target'])
        self.assertFalse((child / '.game/private.txt').exists())
        self.assertFalse((child / '.game/subworld').exists())
        self.assertEqual(self.manager.prepare(child, 'P')['navigation']['target'], str(self.root))

    @unittest.skipUnless(os.name == 'nt', 'Case-insensitive Windows filesystem')
    def test_case_variant_unlinked_child_cannot_create_nested_world(self):
        child = self.root / '.GAME/SUBWORLD'
        child.mkdir(parents=True)
        (child / 'scene.ini').write_text(SCENE, encoding='utf-8')
        with self.assertRaisesRegex(SubworldError, '嵌套'):
            self.manager.prepare(child, 'O')
        self.assertEqual(self.saved, [])
        self.assertFalse((child / CHILD).exists())

    def test_failed_copy_cleans_readonly_staged_assets_without_touching_source(self):
        asset = self.root / 'Assets/model.glb'
        asset.chmod(stat.S_IREAD)
        self.addCleanup(asset.chmod, stat.S_IREAD | stat.S_IWRITE)
        def failing_copy(source, target):
            subworlds._copy_scene(source, target)
            raise OSError('copy interrupted after readonly asset')
        self.manager.copy_scene = failing_copy
        with self.assertRaisesRegex(OSError, 'copy interrupted'):
            self.enter()
        self.assert_clean_failure()
        self.assertFalse(asset.stat().st_mode & stat.S_IWRITE)
