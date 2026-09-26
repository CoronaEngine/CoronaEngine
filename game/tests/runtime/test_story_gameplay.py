"""Durability, idempotency, context and shared-inventory regression tests."""
from pathlib import Path
from types import SimpleNamespace
import json
import tempfile
import unittest
from unittest.mock import Mock, patch
import uuid

from game.runtime import story_gameplay as game
from game.runtime.subworlds import StorySubworlds
from game.tests.runtime.test_subworlds import SCENE


class GameplayTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / '主世界'
        self.root.mkdir()
        (self.root / 'scene.ini').write_text(SCENE, encoding='utf-8')
        self.info = {'project_path': str(self.root), 'mode': 'story'}
        self.api = SimpleNamespace(project_settings=SimpleNamespace(
            get_active_project_info=Mock(side_effect=lambda: {'data': self.info})))
        self.revision = 0

    def request(self, action='load', **kwargs):
        data = {'projectPath': self.info['project_path'], 'action': action}
        if action != 'load':
            data.update(operationId=str(uuid.uuid4()), expectedRevision=self.revision)
            data.update(bossPosition=[0, 0, 12] if action == 'hitBoss' else None)
            if action == 'pickupDrop':
                data['dropId'] = game.DROP_ID
        data.update(kwargs)
        return data

    def call(self, data=None, **kwargs):
        result = game.handle_gameplay_request(json.dumps(data or self.request(**kwargs)), api=self.api)
        if result['status'] == 'ok':
            self.revision = result['state']['revision']
        return result

    def hit(self):
        result = self.call(action='hitBoss')
        self.assertEqual(result['status'], 'ok', result)
        return result

    def kill(self):
        for _ in range(10):
            result = self.hit()
        return result

    def child(self):
        return Path(StorySubworlds(save=lambda _: None, validate=lambda _: None)
                    .prepare(self.root, 'O')['navigation']['target'])

    def test_ten_hits_death_pickup_and_reopen(self):
        self.assertEqual(self.call()['state']['boss']['hp'], 200)
        for _ in range(5):
            self.hit()
        self.assertEqual(self.call()['state']['boss']['hp'], 100)
        for _ in range(5):
            result = self.hit()
        self.assertEqual(result['state']['boss']['hp'], 0)
        self.assertFalse(self.call()['state']['drop']['collected'])
        self.assertEqual(result['state']['drop']['position'], [0, 0, 12])
        self.assertEqual(self.call(action='hitBoss')['status'], 'error')
        self.assertEqual(self.call(action='pickupDrop')['state']['inventory']['worldFragment'], 1)
        self.assertTrue(self.call()['state']['drop']['collected'])
        self.assertEqual(self.call(action='pickupDrop')['status'], 'error')
        self.assertEqual(len(self.call()['state']['operations']), 11)

    def test_repeated_ids_and_lost_reply_do_not_apply_twice(self):
        request = self.request('hitBoss')
        first = self.call(request)
        second = self.call(request)
        self.assertEqual(first['state'], second['state'])
        self.assertEqual(second['state']['boss']['hp'], 180)
        for _ in range(9):
            self.hit()
        pickup = self.request('pickupDrop')
        self.call(pickup)
        self.assertEqual(self.call(pickup)['state']['inventory']['worldFragment'], 1)
        self.assertEqual(self.call({**request, 'bossPosition': [10, 0, 12]})['status'], 'error')

    def test_revision_conflict_returns_current_state_without_writing(self):
        self.hit()
        original = (self.root / game.SAVE_PATH).read_bytes()
        result = self.call(action='hitBoss', expectedRevision=0)
        self.assertEqual(result['code'], 'REVISION_CONFLICT')
        self.assertEqual(result['state']['boss']['hp'], 180)
        self.assertEqual((self.root / game.SAVE_PATH).read_bytes(), original)

    def test_child_shares_inventory_but_cannot_fight_or_pick_up(self):
        child = self.child()
        self.kill()
        self.call(action='pickupDrop')
        self.info['project_path'] = str(child)
        result = self.call()
        self.assertEqual(result['role'], 'child')
        self.assertEqual(result['state']['inventory']['worldFragment'], 1)
        self.assertEqual(self.call(action='hitBoss')['status'], 'error')
        self.assertEqual(self.call(action='pickupDrop')['status'], 'error')
        self.assertFalse((child / game.SAVE_PATH).exists())
        self.info['project_path'] = str(self.root)
        self.assertEqual(self.call()['state']['inventory']['worldFragment'], 1)

    def test_bad_save_is_not_overwritten(self):
        self.hit()
        path = self.root / game.SAVE_PATH
        path.write_text('{broken', encoding='utf-8')
        self.assertIn('存档损坏', self.call()['message'])
        self.assertEqual(self.call(action='hitBoss')['status'], 'error')
        self.assertEqual(path.read_text(), '{broken')

    def test_atomic_replace_failure_keeps_old_state_and_operation_retry(self):
        self.hit()
        path = self.root / game.SAVE_PATH
        previous = path.read_bytes()
        request = self.request('hitBoss')
        with patch.object(game.os, 'replace', side_effect=OSError('disk full')):
            self.assertEqual(self.call(request)['status'], 'error')
        self.assertEqual(path.read_bytes(), previous)
        self.assertEqual(list(path.parent.glob('.story-gameplay-*')), [])
        self.assertEqual(self.call(request)['state']['boss']['hp'], 160)

    def test_pickup_failure_is_atomic_with_inventory(self):
        self.kill()
        request = self.request('pickupDrop')
        with patch.object(game.os, 'replace', side_effect=PermissionError('read only')):
            self.assertEqual(self.call(request)['status'], 'error')
        state = self.call()['state']
        self.assertFalse(state['drop']['collected'])
        self.assertEqual(state['inventory']['worldFragment'], 0)
        self.assertEqual(self.call(request)['state']['inventory']['worldFragment'], 1)

    def test_stale_or_creative_project_cannot_write(self):
        request = self.request('hitBoss')
        self.info['project_path'] = str(self.root.parent)
        self.assertEqual(self.call(request)['status'], 'error')
        self.info.update(project_path=str(self.root), mode='creative')
        self.assertEqual(self.call(request)['status'], 'error')
        self.assertFalse((self.root / game.SAVE_PATH).exists())

    def test_source_changed_before_commit_preserves_disk(self):
        self.hit()
        path = self.root / game.SAVE_PATH
        previous = path.read_bytes()
        original = game.os.fsync
        def change_source(fd):
            original(fd)
            self.info['project_path'] = str(self.root.parent)
        with patch.object(game.os, 'fsync', side_effect=change_source):
            self.assertEqual(self.call(action='hitBoss')['status'], 'error')
        self.assertEqual(path.read_bytes(), previous)

    def test_unknown_actions_bad_numbers_and_operation_ids_are_rejected(self):
        for data in [self.request('writeFile'), self.request('hitBoss', bossPosition=[float('nan'), 0, 0]),
                     self.request('hitBoss', operationId='../elsewhere'),
                     self.request('hitBoss', expectedRevision=True), self.request(projectPath='')]:
            self.assertEqual(self.call(data)['status'], 'error', data)
        self.assertEqual(game.handle_gameplay_request('[]', api=self.api)['status'], 'error')
        self.assertFalse((self.root / game.SAVE_PATH).exists())

    def test_broken_child_link_is_not_treated_as_fresh_main(self):
        child = self.child()
        (child / '.game/story-link.json').unlink()
        self.info['project_path'] = str(child)
        self.assertEqual(self.call()['status'], 'error')
        self.assertFalse((child / game.SAVE_PATH).exists())

    def test_navigation_lock_prevents_concurrent_save(self):
        (self.root / '.game').mkdir()
        lock = self.root / '.game/story-navigation.lock'
        lock.write_text('busy')
        self.assertEqual(self.call(action='hitBoss')['status'], 'error')
        self.assertTrue(lock.exists())
        self.assertFalse((self.root / game.SAVE_PATH).exists())


if __name__ == '__main__':
    unittest.main()
