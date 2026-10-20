"""World rules installed into a small world: validation, atomic write, and safe reads."""
import json
from pathlib import Path
import tempfile
import unittest

from game.runtime import world_rules


def sway_rule(**overrides):
    rule = {'fragmentId': 'sway', 'type': 'sway', 'axis': 'x', 'amplitude': 0.25, 'periodMs': 2400}
    rule.update(overrides)
    return rule


def spin_rule(**overrides):
    """A rotation rule: the amplitude is degrees, the axis is always z."""
    rule = {'fragmentId': 'spin', 'type': 'spin', 'axis': 'z', 'amplitude': 15.0, 'periodMs': 3600}
    rule.update(overrides)
    return rule


def map_identity():
    """地图的身份，取自权威数据文件：与 game/frontend/storyMap.mjs 的 MAP_PLACEMENT 同源。"""
    root = Path(__file__).resolve().parents[3]
    block = json.loads((root / 'game/data/prophet.json').read_text(encoding='utf-8'))['map']
    assert block['guid'] and block['modelRef'], '地图身份必须能在数据文件里读到'
    return block


def prophet_guid():
    """先知的 actor 身份，与 game/frontend/storyCharacters.mjs 里 role='prophet' 的角色同源。

    这个 guid 只写在角色表里（不在数据文件中），所以这里与运行时常量各自引用一次；
    game/tests/test_story_assets.py 会拿 node 读出角色表，证明两处没有漂开。
    """
    return 'f9f5c8b0-7324-4b6c-a011-000000000004'


class WorldRuleValidationTests(unittest.TestCase):
    def test_initial_state_is_an_empty_versioned_rule_set(self):
        self.assertEqual(world_rules.initial_state(), {'version': 1, 'rules': []})

    def test_a_good_rule_is_kept_field_for_field(self):
        self.assertEqual(world_rules.validate_rule(sway_rule()), sway_rule())
        # Both rule types are accepted, and a spin keeps its degrees untouched.
        self.assertEqual(world_rules.validate_rule(spin_rule()), spin_rule())
        self.assertEqual(world_rules.validate_rule(spin_rule(amplitude=15))['amplitude'], 15)

    def test_the_axis_must_match_the_rule_type(self):
        # Mirrors _TYPE_AXES (world_rules.py:27): sway moves along any axis, spin only z.
        for axis in ('x', 'y', 'z'):
            self.assertEqual(world_rules.validate_rule(sway_rule(axis=axis))['axis'], axis)
        self.assertEqual(world_rules.validate_rule(spin_rule(axis='z'))['axis'], 'z')
        for case in [spin_rule(axis='x'), spin_rule(axis='y'), spin_rule(axis='w')]:
            with self.subTest(case=case):
                with self.assertRaises(world_rules.WorldRuleError):
                    world_rules.validate_rule(case)

    def test_malformed_rules_are_rejected_rather_than_clamped(self):
        cases = [
            None, [], 'sway',
            sway_rule(fragmentId=''), sway_rule(fragmentId='x' * 65), sway_rule(fragmentId=7),
            sway_rule(type='orbit'), sway_rule(type=None), sway_rule(type=7),
            spin_rule(type='orbit'), spin_rule(type=None),
            sway_rule(axis='w'), sway_rule(axis='X'), spin_rule(axis='w'), spin_rule(axis='X'),
            sway_rule(amplitude=0), sway_rule(amplitude=-0.25), sway_rule(amplitude=float('nan')),
            sway_rule(amplitude=float('inf')), sway_rule(amplitude='0.25'), sway_rule(amplitude=True),
            sway_rule(amplitude=1e6),
            spin_rule(amplitude=0), spin_rule(amplitude=-15), spin_rule(amplitude='15'),
            spin_rule(amplitude=True), spin_rule(amplitude=100.1),
            sway_rule(periodMs=0), sway_rule(periodMs=-2400), sway_rule(periodMs='2400'),
            sway_rule(periodMs=10 ** 9),
            spin_rule(periodMs=0), spin_rule(periodMs=-3600), spin_rule(periodMs='3600'),
            spin_rule(periodMs=10 ** 9),
            # target 只在真的给出时才要求合法：空串、非字符串、超长都拒绝。null/缺失是
            # 「旧规则」（作用于整个小世界），因此不在这张表里。
            sway_rule(target=''), sway_rule(target='   '), sway_rule(target=7),
            sway_rule(target=['a']), sway_rule(target='x' * (world_rules.MAX_TARGET_LENGTH + 1)),
        ]
        for case in cases:
            with self.subTest(case=case):
                with self.assertRaises(world_rules.WorldRuleError):
                    world_rules.validate_rule(case)

    def test_a_rule_may_name_the_one_object_it_acts_on(self):
        """碎片改为可作用于单独物体：target 是 actor guid，合法时原样保留。"""
        target = 'story.placement.0003'
        named = sway_rule(target=target)
        self.assertEqual(world_rules.validate_rule(named), named)
        # 空白被裁掉，身份本身不变。
        self.assertEqual(world_rules.validate_rule(sway_rule(target=' story.placement.0003 ')),
                         named)
        # 没有 target 的记录回读出来仍然没有 target 键：旧规则就是旧规则。
        self.assertNotIn('target', world_rules.validate_rule(sway_rule()))
        self.assertNotIn('target', world_rules.validate_rule(sway_rule(target=None)))

    def test_a_rule_that_names_the_map_or_the_prophet_is_refused(self):
        """地图不算物体，先知是角色：任何规则都不许指向它们。

        前端在选目标时就用 isMapActor / isProphetActor 把两者挡在列表外（storyProphetActions
        .selectableObjects），运行时这一层是防御纵深：一份手改存档、一个旧版本、一次直接调用
        都进不来。非真空：把 `_target` 里的保留判断删掉，这份记录会被接受并落到盘上，
        下面每一条断言都会失败。
        """
        identity = map_identity()
        self.assertIn(identity['guid'], world_rules.RESERVED_TARGETS,
                      '运行时保留表里的地图 guid 必须就是数据文件里的那一个')
        for reserved in (identity['guid'], prophet_guid()):
            with self.subTest(reserved=reserved):
                with self.assertRaises(world_rules.WorldRuleError):
                    world_rules.validate_rule(sway_rule(target=reserved))

    def test_state_requires_the_supported_version_and_unique_fragments(self):
        self.assertEqual(world_rules.validate_state({'version': 1, 'rules': [sway_rule()]}),
                         {'version': 1, 'rules': [sway_rule()]})
        # A world may carry both rules at once, each on its own fragment.
        self.assertEqual(world_rules.validate_state({'version': 1, 'rules': [sway_rule(), spin_rule()]}),
                         {'version': 1, 'rules': [sway_rule(), spin_rule()]})
        for case in [None, [], {}, {'version': 2, 'rules': []}, {'version': 1},
                     {'version': 1, 'rules': {}}, {'version': 1, 'rules': [sway_rule()] * 2},
                     {'version': 1, 'rules': [spin_rule(), spin_rule()]},
                     {'version': 1, 'rules': [sway_rule(), spin_rule(axis='x')]},
                     {'version': 1, 'rules': [sway_rule()] * (world_rules.MAX_RULES + 1)}]:
            with self.subTest(case=case):
                with self.assertRaises(world_rules.WorldRuleError):
                    world_rules.validate_state(case)
        # The bounds are inclusive, exactly as _positive() says: the limit itself is legal
        # and the mirrored frontend bounds must reach it and not one step further.
        self.assertEqual(world_rules.validate_rule(spin_rule(amplitude=100.0))['amplitude'], 100.0)
        self.assertEqual(world_rules.validate_rule(spin_rule(periodMs=600_000))['periodMs'], 600_000)
        with self.assertRaises(world_rules.WorldRuleError):
            world_rules.validate_rule(spin_rule(amplitude=100.0 + 1e-9))
        with self.assertRaises(world_rules.WorldRuleError):
            world_rules.validate_rule(spin_rule(periodMs=600_000 + 1))


class WorldRuleStorageTests(unittest.TestCase):
    def setUp(self):
        self._temporary = tempfile.TemporaryDirectory(prefix='world rules ')
        self.root = Path(self._temporary.name)

    def tearDown(self):
        self._temporary.cleanup()

    def save_path(self):
        return self.root / world_rules.SAVE_PATH

    def test_a_world_without_the_file_has_no_rules(self):
        self.assertEqual(world_rules.load(self.root), world_rules.initial_state())
        self.assertFalse(self.save_path().exists())

    def test_save_then_load_round_trips_and_creates_the_game_folder(self):
        world_rules.save(self.root, [sway_rule()])
        self.assertTrue(self.save_path().is_file())
        self.assertTrue((self.root / '.game').is_dir())
        self.assertEqual(world_rules.load(self.root), {'version': 1, 'rules': [sway_rule()]})
        written = json.loads(self.save_path().read_text(encoding='utf-8'))
        self.assertEqual(written, {'version': 1, 'rules': [sway_rule()]})

    def test_saved_rules_keep_chinese_readable(self):
        world_rules.save(self.root, [sway_rule(fragmentId='浮动')])
        self.assertIn('浮动', self.save_path().read_text(encoding='utf-8'))
        self.assertEqual(world_rules.load(self.root)['rules'][0]['fragmentId'], '浮动')

    def test_a_saved_rule_that_names_the_map_is_refused_rather_than_stored(self):
        """指向地图/先知的规则在运行时就被拒绝，而不是被悄悄改成一条整世界规则。

        前端在选目标时就把两者挡在列表外（storyProphetActions.selectableObjects +
        storyWorldRules.RESERVED_TARGET_GUIDS）；这里是防御纵深：一份手改存档、一个旧版本、
        一次直接调用都写不进去。拒绝而不是丢字段，是因为「丢掉 target」会把一条本来只该作用
        于一个物体的规则变成作用于整个小世界——那正是这次要改掉的行为。

        非真空：把 `_target` 里的保留判断删掉，`save` 会把这条记录写到盘上，下面每条断言都会失败。
        """
        identity = map_identity()
        for reserved in (identity['guid'], prophet_guid()):
            with self.subTest(reserved=reserved):
                with self.assertRaises(world_rules.WorldRuleError):
                    world_rules.save(self.root, [sway_rule(target=reserved)])
        self.assertFalse(self.save_path().exists(), '被拒绝的保存一个字节都不写')
        # 身份字段只认 target：写进 guid/model_ref 这些本不属于规则的字段，谁也混不进来。
        named = sway_rule(guid=identity['guid'], modelRef=identity['modelRef'],
                          model_ref=identity['modelRef'], entity_id='story.world.map',
                          entity_type='story_scenery', targets=[identity['guid']],
                          actor_guid=identity['guid'])
        self.assertEqual(world_rules.validate_rule(named), sway_rule(), '地图身份在校验时就被丢掉')
        world_rules.save(self.root, [named])
        self.assertEqual(world_rules.load(self.root), {'version': 1, 'rules': [sway_rule()]})
        self.assertEqual(json.loads(self.save_path().read_text(encoding='utf-8')),
                         {'version': 1, 'rules': [sway_rule()]}, '盘上也不留任何地图身份')

    def test_a_rule_with_a_target_round_trips_and_old_targetless_rules_stay_targetless(self):
        """碎片指向一个物体时原样落盘；改动前存的「没有 target」的记录一字不改地回读。

        这就是旧世界的兼容承诺：target 是可选的，缺了它规则仍然是**旧的整世界语义**，
        既不需要迁移，也不会因为一次读写往返变成「只作用于某个物体」。
        """
        targeted = sway_rule(target='story.placement.0003')
        legacy = spin_rule()
        world_rules.save(self.root, [targeted, legacy])
        self.assertEqual(world_rules.load(self.root),
                         {'version': 1, 'rules': [targeted, legacy]})
        written = json.loads(self.save_path().read_text(encoding='utf-8'))
        self.assertEqual(written['rules'][0]['target'], 'story.placement.0003')
        self.assertNotIn('target', written['rules'][1], '旧规则不会凭空长出一个 target')
        self.assertEqual(written['version'], 1, '版本号不跳：旧构建仍能读，新构建向后兼容')

    def test_a_world_may_keep_a_sway_and_a_spin_at_the_same_time(self):
        world_rules.save(self.root, [sway_rule(), spin_rule()])
        self.assertEqual(world_rules.load(self.root),
                         {'version': 1, 'rules': [sway_rule(), spin_rule()]})
        written = json.loads(self.save_path().read_text(encoding='utf-8'))
        self.assertEqual([rule['type'] for rule in written['rules']], ['sway', 'spin'])
        self.assertEqual([rule['axis'] for rule in written['rules']], ['x', 'z'])
        # A spin with a non-z axis is refused by the same path that refuses a bad sway.
        for bad in [[spin_rule(axis='x')], [sway_rule(), spin_rule(axis='y')],
                    [spin_rule(amplitude=0)]]:
            with self.subTest(bad=bad):
                with self.assertRaises(world_rules.WorldRuleError):
                    world_rules.save(self.root, bad)
        self.assertEqual(world_rules.load(self.root)['rules'], [sway_rule(), spin_rule()],
                         '被拒绝的保存不会覆盖盘上已有的好规则')

    def test_save_refuses_invalid_input_so_garbage_never_reaches_the_disk(self):
        for bad in [[sway_rule(axis='w')], [sway_rule()] * 2, 'nope', [sway_rule(amplitude=0)]]:
            with self.subTest(bad=bad):
                with self.assertRaises(world_rules.WorldRuleError):
                    world_rules.save(self.root, bad)
        self.assertFalse(self.save_path().exists())

    def test_clear_leaves_an_empty_rule_set(self):
        world_rules.save(self.root, [sway_rule()])
        self.assertEqual(world_rules.clear(self.root), world_rules.initial_state())
        self.assertEqual(world_rules.load(self.root), world_rules.initial_state())

    def test_a_corrupt_file_is_reported_and_never_overwritten(self):
        self.save_path().parent.mkdir(parents=True, exist_ok=True)
        self.save_path().write_text('{ not json', encoding='utf-8')
        with self.assertRaises(world_rules.WorldRuleError) as raised:
            world_rules.load(self.root)
        self.assertIn('未覆盖原文件', str(raised.exception))
        self.assertEqual(self.save_path().read_text(encoding='utf-8'), '{ not json')

    def test_an_oversized_file_is_refused(self):
        self.save_path().parent.mkdir(parents=True, exist_ok=True)
        self.save_path().write_text('x' * (world_rules.MAX_BYTES + 1), encoding='utf-8')
        with self.assertRaises(world_rules.WorldRuleError):
            world_rules.load(self.root)

    def test_a_failed_write_leaves_no_temporary_file_behind(self):
        world_rules.save(self.root, [sway_rule()])
        leftovers = [path.name for path in (self.root / '.game').iterdir()
                     if path.name != 'story-world-rules.json']
        self.assertEqual(leftovers, [], leftovers)


if __name__ == '__main__':
    unittest.main()
