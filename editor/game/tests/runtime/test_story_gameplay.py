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
        data.update(kwargs)
        return data

    def call(self, data=None, **kwargs):
        result = game.handle_gameplay_request(json.dumps(data or self.request(**kwargs)), api=self.api)
        if result['status'] == 'ok':
            self.revision = result['state'].get('revision', self.revision)
        return result

    def document(self, action, **kwargs):
        """A request against one of the small world's own documents (no combat revision)."""
        return self.call({'projectPath': self.info['project_path'], 'action': action, **kwargs})

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

    def test_ten_hits_unlock_the_small_world_without_a_pickup(self):
        self.assertEqual(self.call()['state']['boss']['hp'], 200)
        self.assertEqual(self.call()['state']['inventory']['block'], 0, '开局的背包里没有方块')
        self.assertEqual(self.call()['state']['inventory']['ball'], 0, '开局的背包里没有球')
        for _ in range(5):
            self.hit()
        self.assertEqual(self.call()['state']['boss']['hp'], 100)
        for _ in range(5):
            result = self.hit()
        self.assertEqual(result['state']['boss']['hp'], 0)
        # The reward is handed over by the killing blow itself: no red ball, no F.
        self.assertIsNone(result['state']['drop'])
        self.assertEqual(result['state']['inventory']['worldFragment'], 1)
        self.assertEqual(result['state']['inventory']['block'], game.BLOCK_REWARD)
        self.assertEqual(result['state']['inventory']['ball'], game.BALL_REWARD)
        self.assertEqual(self.call(action='hitBoss')['status'], 'error')
        self.assertEqual(self.call(action='pickupDrop')['status'], 'error')
        self.assertEqual(len(self.call()['state']['operations']), 10)
        # Reopening the world reads the same unlocked inventory back.
        reopened = self.call()
        self.assertEqual(reopened['state']['inventory']['worldFragment'], 1)
        self.assertIsNone(reopened['state']['drop'])

    def test_each_monster_has_its_own_health_pool_and_can_be_hit(self):
        self.assertEqual(game.CONFIG['monsterDropMin'], 5)
        self.assertEqual(game.CONFIG['monsterDropMax'], 15)
        self.assertEqual(game.CONFIG['monsterEquipmentChance'], 0.5)
        state = self.call()['state']
        self.assertEqual(set(state['monsters']), set(game.CONFIG['monsters']))
        for role, stats in game.CONFIG['monsters'].items():
            self.assertEqual(state['monsters'][role]['hp'], stats['hp'])
        result = self.call(self.request('hitMonster', monsterId='monster-mantis'))
        self.assertEqual(result['status'], 'ok', result)
        self.assertEqual(result['state']['monsters']['monster-mantis']['hp'], 60)
        self.assertEqual(result['state']['monsters']['monster-larva']['hp'], 90)
        self.assertEqual(result['state']['boss']['hp'], 200, '打怪不会顺带伤到 Boss')
        self.assertEqual(result['state']['rage'], 10)
        self.assertEqual(self.call(action='hitMonster', monsterId='no-such-monster')['status'], 'error')

    def test_a_monster_kill_rolls_both_drops_and_replays_the_same_outcome(self):
        last = None
        # Every kill drops 5–15 of one model; the equipment roll hits at 0.1 < 0.5.
        with patch.object(game.random, 'random', return_value=0.1), \
                patch.object(game.random, 'randint', return_value=7), \
                patch.object(game.random, 'choice', side_effect=['ball', 'hunter-blade']):
            for _ in range(4):
                last = self.request('hitMonster', monsterId='monster-mantis')
                result = self.call(last)
        self.assertEqual(result['state']['monsters']['monster-mantis']['hp'], 0)
        self.assertEqual(result['state']['inventory']['ball'], 7, '击杀掉落 7 个球')
        self.assertEqual(result['state']['inventory']['hunter-blade'], 2,
                         '50% 命中：装备也直接进背包（开局自带 1 件）')
        # A retry of the same operation must not roll or grant again.
        self.assertEqual(self.call(last)['state']['inventory']['ball'], 7)
        self.assertEqual(self.call()['state']['inventory']['hunter-blade'], 2)
        last_operation = self.call()['state']['operations'][-1]
        self.assertEqual(last_operation['loot'], 'ball')
        self.assertEqual(last_operation['lootCount'], 7)
        self.assertEqual(last_operation['gear'], 'hunter-blade')
        # High equipment roll: no gear, but the model stack still always drops.
        with patch.object(game.random, 'random', return_value=0.9), \
                patch.object(game.random, 'randint', return_value=5), \
                patch.object(game.random, 'choice', return_value='block'):
            for _ in range(5):
                last = self.request('hitMonster', monsterId='monster-larva')
                result = self.call(last)
        self.assertEqual(result['state']['monsters']['monster-larva']['hp'], 0)
        self.assertEqual(result['state']['inventory']['ball'], 7)
        self.assertEqual(result['state']['inventory']['block'], 5)
        last_operation = self.call()['state']['operations'][-1]
        self.assertEqual(last_operation['loot'], 'block')
        self.assertEqual(last_operation['lootCount'], 5)
        self.assertIsNone(last_operation['gear'])
        self.assertEqual(self.call(action='hitMonster', monsterId='monster-mantis')['status'], 'error')

    def test_a_client_cannot_forge_a_drop_by_sending_loot_or_gear(self):
        with patch.object(game.random, 'random', return_value=0.9), \
                patch.object(game.random, 'randint', return_value=9), \
                patch.object(game.random, 'choice', return_value='block'):
            for _ in range(4):
                last = self.request('hitMonster', monsterId='monster-mantis',
                                    loot='ball', gear='hunter-blade', lootCount=99)
                result = self.call(last)
        self.assertEqual(result['state']['monsters']['monster-mantis']['hp'], 0)
        self.assertEqual(result['state']['inventory']['block'], 9, '掉落只能由后端掷点决定')
        self.assertEqual(result['state']['inventory']['ball'], 0)
        self.assertEqual(result['state']['inventory']['hunter-blade'], 1,
                         '只能拿到开局自带的那一件，客户端塞不进额外的')
        last_operation = self.call()['state']['operations'][-1]
        self.assertEqual(last_operation['loot'], 'block')
        self.assertEqual(last_operation['lootCount'], 9)
        self.assertIsNone(last_operation['gear'])

    def test_skills_can_finish_a_monster_without_touching_the_boss(self):
        for _ in range(3):
            self.call(action='hitMonster', monsterId='monster-mantis')
        self.assertEqual(self.call()['state']['monsters']['monster-mantis']['hp'], 20)
        self.assertEqual(self.call()['state']['rage'], 30)
        with patch.object(game.random, 'random', return_value=0.9), \
                patch.object(game.random, 'randint', return_value=5), \
                patch.object(game.random, 'choice', return_value='block'):
            result = self.call(action='castSkill', skillId='heavy', hit=True,
                               monsterId='monster-mantis')
        self.assertEqual(result['status'], 'ok', result)
        self.assertEqual(result['state']['monsters']['monster-mantis']['hp'], 0)
        self.assertEqual(result['state']['boss']['hp'], 200)
        self.assertEqual(result['state']['rage'], 0)

    def test_repeated_ids_and_lost_reply_do_not_apply_twice(self):
        request = self.request('hitBoss')
        first = self.call(request)
        second = self.call(request)
        self.assertEqual(first['state'], second['state'])
        self.assertEqual(second['state']['boss']['hp'], 180)
        for _ in range(9):
            result = self.hit()
        # The killing blow is the one that hands over the fragment; replaying any earlier
        # request must neither resurrect the boss nor grant a second fragment.
        self.assertEqual(result['state']['inventory']['worldFragment'], 1)
        self.assertIsNone(result['state']['drop'])
        self.assertEqual(self.call(request)['state']['inventory']['worldFragment'], 1)
        self.assertEqual(self.call({**request, 'bossPosition': [10, 0, 12]})
                         ['status'], 'error')

    def test_revision_conflict_returns_current_state_without_writing(self):
        self.hit()
        original = (self.root / game.SAVE_PATH).read_bytes()
        result = self.call(action='hitBoss', expectedRevision=0)
        self.assertEqual(result['code'], 'REVISION_CONFLICT')
        self.assertEqual(result['state']['boss']['hp'], 180)
        self.assertEqual((self.root / game.SAVE_PATH).read_bytes(), original)

    def test_child_shares_inventory_but_cannot_fight_or_take_the_reward_again(self):
        child = self.child()
        self.kill()
        self.info['project_path'] = str(child)
        result = self.call()
        self.assertEqual(result['role'], 'child')
        self.assertEqual(result['state']['inventory']['worldFragment'], 1)
        self.assertEqual(result['state']['inventory']['block'], game.BLOCK_REWARD)
        self.assertEqual(result['state']['inventory']['ball'], game.BALL_REWARD)
        self.assertEqual(self.call(action='hitBoss')['status'], 'error')
        self.assertEqual(self.call(action='pickupDrop')['status'], 'error')
        self.assertFalse((child / game.SAVE_PATH).exists())
        self.info['project_path'] = str(self.root)
        self.assertEqual(self.call()['state']['inventory']['worldFragment'], 1)
        self.assertIsNone(self.call()['state']['drop'])

    def test_rage_skills_charge_once_on_hit_or_miss_and_survive_reload(self):
        for hit in (True, False):
            with self.subTest(hit=hit):
                for _ in range(3):
                    self.hit()
                before = self.call()['state']
                self.assertEqual(before['rage'], 30)
                request = self.request('castSkill', skillId='heavy', hit=hit, bossPosition=[0, 0, 12])
                result = self.call(request)
                self.assertEqual(result['status'], 'ok', result)
                self.assertEqual(result['state']['rage'], 0)
                self.assertEqual(result['state']['boss']['hp'], before['boss']['hp'] - (50 if hit else 0))
                self.assertEqual(self.call(request)['state'], result['state'])
                self.assertEqual(self.call()['state'], result['state'])
                self.assertEqual(self.call(action='castSkill', skillId='heavy', hit=False)['status'], 'error')

    def test_v1_migration_preserves_progress_without_granting_retroactive_rage(self):
        for _ in range(3):
            old = self.hit()['state']
        old.update(version=1)
        del old['rage'], old['legacyRevision']
        path = self.root / game.SAVE_PATH
        path.write_text(json.dumps(old), encoding='utf-8')
        migrated = self.call()['state']
        self.assertEqual(migrated['version'], 4)
        self.assertEqual(migrated['legacyRevision'], 3)
        self.assertEqual(migrated['rage'], 0)
        self.assertEqual(migrated['boss'], old['boss'])
        self.assertEqual(migrated['operations'], old['operations'])
        state = self.hit()['state']
        self.assertEqual(state['rage'], 10)
        self.assertEqual(self.call()['state'], state)
        state['rage'] = 100
        path.write_text(json.dumps(state), encoding='utf-8')
        previous = path.read_bytes()
        self.assertEqual(self.call()['status'], 'error')
        self.assertEqual(path.read_bytes(), previous)

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

    def test_the_killing_blow_grants_the_reward_atomically(self):
        for _ in range(9):
            self.hit()
        request = self.request('hitBoss')
        with patch.object(game.os, 'replace', side_effect=PermissionError('read only')):
            self.assertEqual(self.call(request)['status'], 'error')
        state = self.call()['state']
        self.assertEqual(state['boss']['hp'], 20)
        self.assertEqual(state['inventory']['worldFragment'], 0)
        self.assertEqual(state['inventory']['block'], 0)
        self.assertEqual(state['inventory']['ball'], 0)
        self.assertIsNone(state['drop'])
        # Same operation ID, retried after the disk is writable: commits once.
        committed = self.call(request)['state']['inventory']
        self.assertEqual(committed['worldFragment'], 1)
        self.assertEqual(committed['block'], game.BLOCK_REWARD)
        self.assertEqual(committed['ball'], game.BALL_REWARD)
        self.assertEqual(self.call(request)['state']['inventory']['block'], game.BLOCK_REWARD)

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
        for data in [self.request('writeFile'), self.request('pickupDrop'),
                     self.request('hitBoss', bossPosition=[float('nan'), 0, 0]),
                     self.request('hitBoss', operationId='../elsewhere'),
                     self.request('hitBoss', expectedRevision=True), self.request(projectPath='')]:
            self.assertEqual(self.call(data)['status'], 'error', data)
        self.assertEqual(game.handle_gameplay_request('[]', api=self.api)['status'], 'error')
        self.assertFalse((self.root / game.SAVE_PATH).exists())

    def test_a_legacy_red_ball_save_opens_with_the_small_world_unlocked(self):
        """Worlds saved by the previous flow keep their reward.

        The previous boss death wrote a red ball and a `pickupDrop` operation. Loading such
        a save must hide the ball and hand the fragment over instead of leaving the player
        locked out of a small world they already earned.
        """
        for collected, pickup_operation in ((False, True), (True, True), (False, False)):
            with self.subTest(collected=collected, pickup_operation=pickup_operation):
                operations = [{'operationId': str(uuid.uuid4()), 'action': 'hitBoss',
                               'expectedRevision': index, 'bossPosition': [0, 0, 12]}
                              for index in range(10)]
                if pickup_operation:
                    operations.append({'operationId': str(uuid.uuid4()), 'action': 'pickupDrop',
                                       'expectedRevision': 10, 'dropId': game.DROP_ID})
                state = {'version': 2, 'revision': len(operations), 'legacyRevision': 0, 'rage': 100,
                         'boss': {'hp': 0},
                         'drop': {'id': game.DROP_ID, 'position': [0, 0, 12],
                                  'collected': collected},
                         'inventory': {'worldFragment': 1 if collected else 0,
                                       'block': game.BLOCK_SEED},
                         'operations': operations}
                folder = self.root / '.game'
                folder.mkdir(exist_ok=True)
                path = self.root / game.SAVE_PATH
                path.write_text(json.dumps(state), encoding='utf-8')
                loaded = self.call()
                self.assertEqual(loaded['status'], 'ok', loaded)
                self.assertEqual(loaded['state']['inventory']['worldFragment'], 1)
                self.assertEqual(loaded['state']['inventory']['block'], game.BLOCK_REWARD)
                self.assertEqual(loaded['state']['inventory']['ball'], game.BALL_REWARD)
                self.assertIsNone(loaded['state']['drop'])
                self.assertEqual(set(loaded['state']['monsters']), set(game.CONFIG['monsters']),
                                 '旧存档读取时补齐每只怪物的满血状态')
                self.assertEqual(self.call(action='pickupDrop')['status'], 'error')

    def test_a_retired_pillar_save_becomes_a_ball(self):
        """The pillar reused the block's cube model; both its count and its old log entry
        are folded into the ball, so an existing small world does not lose the object."""
        monsters = game.initial_monsters()
        monsters['monster-mantis']['hp'] = 0
        operations = [{'operationId': str(uuid.uuid4()), 'action': 'hitMonster',
                       'expectedRevision': index, 'monsterId': 'monster-mantis'}
                      for index in range(4)]
        operations[-1]['loot'] = 'pillar'
        inventory = game.initial_inventory()
        inventory['pillar'] = 1
        state = {'version': 2, 'revision': 4, 'legacyRevision': 0, 'rage': 40,
                 'boss': {'hp': 200}, 'drop': None, 'monsters': monsters,
                 'inventory': inventory, 'operations': operations}
        folder = self.root / '.game'
        folder.mkdir(exist_ok=True)
        (self.root / game.SAVE_PATH).write_text(json.dumps(state), encoding='utf-8')
        loaded = self.call()
        self.assertEqual(loaded['status'], 'ok', loaded)
        self.assertEqual(loaded['state']['inventory']['ball'], 1)
        self.assertNotIn('pillar', loaded['state']['inventory'])
        self.assertEqual(loaded['state']['monsters']['monster-mantis']['hp'], 0)
        self.assertTrue(all('gear' not in operation for operation in loaded['state']['operations']),
                        '旧日志没有装备字段，重放时不会凭空补发装备')

    def test_a_pre_kill_old_save_does_not_keep_the_free_block_stack(self):
        """The 64 blocks used to be a starting item; now only the killing blow grants them."""
        for _ in range(3):
            self.hit()
        state = self.call()['state']
        state['inventory']['block'] = game.BLOCK_SEED
        path = self.root / game.SAVE_PATH
        path.write_text(json.dumps(state), encoding='utf-8')
        loaded = self.call()
        self.assertEqual(loaded['status'], 'ok', loaded)
        self.assertEqual(loaded['state']['inventory']['block'], 0)
        # The next committed hit persists the corrected inventory, not the old free stack.
        self.assertEqual(self.hit()['state']['inventory']['block'], 0)
        self.assertEqual(self.call()['state']['inventory']['block'], 0)

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

    def test_child_placements_live_beside_the_link_owner_and_survive_reentry(self):
        """The layout is stored in the link owner's `.game`, not in the small world copy.

        The path is what decides whether a re-entry sees the blocks the player put down:
        `<main>/.game/story-placements.json` is owned by the world that keeps the link and
        is never recreated, while the small world's own `.game` holds only its link.
        """
        child = self.child()
        entry = {'propId': 'block', 'index': 0, 'siteId': None,
                 'position': [1, 0, 2], 'rotation': [0, 0, 0], 'scale': 1}
        self.assertEqual(self.document('loadPlacements')['state']['placements'], [])
        self.assertEqual(self.document('savePlacements', placements=[entry])['status'], 'error')
        self.info['project_path'] = str(child)
        saved = self.document('savePlacements', placements=[entry])
        self.assertEqual(saved['status'], 'ok', saved)
        self.assertEqual(saved['role'], 'child')
        self.assertTrue((self.root / '.game/story-placements.json').is_file())
        self.assertFalse((child / '.game/story-placements.json').exists())
        self.assertFalse((child / game.SAVE_PATH).exists())
        # Re-entering the same small world reads the same layout back, block for block.
        reloaded = self.document('loadPlacements')
        self.assertEqual(reloaded['state']['placements'], [entry])
        self.assertEqual(self.document('load')['state']['inventory']['block'], 0,
                         '未击杀 Boss 时行囊里没有方块')

    def test_a_worn_weapon_changes_damage_and_boots_shorten_the_cooldown(self):
        """披挂改的是同一记普通攻击的扣血量，冷却也被疾行词条压缩。"""
        for _ in range(10):
            result = self.hit()
        self.assertEqual(result['state']['inventory']['boss-main'], 1, 'Boss 的兵甲随击杀进包')
        self.assertEqual(self.call()['config']['skills']['heavy']['cooldownMs'], 1200)
        # 空手 20 点伤害：怪物 80 血打一下还剩 60。
        hit = self.call(action='hitMonster', monsterId='monster-mantis')
        self.assertEqual(hit['state']['monsters']['monster-mantis']['hp'], 60)
        self.assertEqual(self.call(action='equipItem', itemId='boss-main')['status'], 'ok')
        # 攻击 20 + 148：同一条命令，伤害由后端按属性算。
        self.assertEqual(self.call(action='hitMonster', monsterId='monster-mantis')
                         ['state']['monsters']['monster-mantis']['hp'], 0)
        equipped = self.call()['state']
        self.assertEqual(equipped['equipped']['weapon'], 'boss-main')
        self.assertEqual(equipped['inventory']['boss-main'], 0, '披挂后不在囊中重复计数')
        self.assertEqual(self.call()['config']['attributes']['atk'], 168)
        # 换上疾行战靴：重斩的冷却从 1200ms 压到 1080ms。
        self.assertEqual(self.call(action='equipItem', itemId='boss-boots')['status'], 'ok')
        self.assertEqual(self.call()['config']['skills']['heavy']['cooldownMs'], 1080)
        self.assertEqual(self.call(action='unequipItem', slot='weapon')['status'], 'ok')
        self.assertEqual(self.call()['config']['attributes']['atk'], 20, '卸下后完全还原')
        self.assertEqual(self.call()['state']['inventory']['boss-main'], 1)

    def test_using_and_discarding_items_survive_a_reload(self):
        """服用扣一枚、回气；弃置也落进存档，重进世界不会复活。"""
        self.assertEqual(self.call()['state']['inventory']['rage-herb'], 3)
        used = self.call(action='useItem', itemId='rage-herb')
        self.assertEqual(used['state']['inventory']['rage-herb'], 2)
        self.assertEqual(used['state']['rage'], 30)
        dropped = self.call(action='discardItem', itemId='rage-herb')
        self.assertEqual(dropped['state']['inventory']['rage-herb'], 1)
        reloaded = self.call()
        self.assertEqual(reloaded['state']['inventory']['rage-herb'], 1)
        self.assertEqual(reloaded['state']['rage'], 30)
        self.assertEqual(self.call(action='discardItem', itemId='not-an-item')['status'], 'error')
        self.assertEqual(self.call(action='equipItem', itemId='boss-main')['status'], 'error',
                         '囊中没有的装备不能凭空披挂')



if __name__ == '__main__':
    unittest.main()