"""验证公共导出的身份一致性及所有子模块的独立导入。"""

import importlib
from pathlib import Path
import subprocess
import sys
import unittest

import game


class ImportTests(unittest.TestCase):
    def test_public_exports_keep_their_canonical_identity(self) -> None:
        exports = {
            'game.core.config': ('GameConfig',),
            'game.core.session': ('GameSession',),
            'game.core.types': (
                'WorldId', 'OrbId', 'DropId', 'Position', 'Result', 'Error',
                'DayPhase', 'BossPhase', 'MerchantPhase', 'ItemKind', 'WorldKind',
            ),
            'game.core.state': (
                'WorldOrb', 'ClockState', 'ClockTransition', 'BossState', 'MerchantState',
                'DropState', 'InventoryState', 'CreativeWorldState', 'WorldNavigation',
            ),
            'game.core.events': (
                'GameEvent', 'EventPayload', 'DayNightChanged', 'BossSpawned',
                'BossChaseStarted', 'BossDefeated', 'DropSpawned', 'DropCollected',
                'MerchantSpawned', 'MerchantDeparted', 'ItemGranted',
                'CreativeWorldCreated', 'WorldChanged',
            ),
        }
        names = [name for group in exports.values() for name in group]
        self.assertCountEqual(game.__all__, names)
        for module_name, group in exports.items():
            module = importlib.import_module(module_name)
            for name in group:
                with self.subTest(module=module_name, name=name):
                    self.assertIs(getattr(game, name), getattr(module, name))

    def test_submodules_import_in_a_fresh_process_without_side_effects(self) -> None:
        package_dir = Path(game.__file__).resolve().parent
        modules = sorted(
            'game.' + '.'.join(path.relative_to(package_dir).with_suffix('').parts)
            for folder in ('core', 'systems', 'world')
            for path in (package_dir / folder).glob('*.py')
            if path.name != '__init__.py'
        )
        code = """
import importlib
import random
import sys
import threading
from unittest.mock import patch

threads = threading.enumerate()
state = random.getstate()
with patch('random.Random', side_effect=AssertionError('import instantiated Random')):
    with patch('threading.Thread.start', side_effect=AssertionError('import started thread')):
        for name in sys.argv[1:]:
            importlib.import_module(name)
import game
assert threading.enumerate() == threads
assert random.getstate() == state
assert not any(isinstance(value, game.GameSession) for value in vars(game).values())
"""
        for order in (modules, list(reversed(modules))):
            with self.subTest(first_module=order[0]):
                result = subprocess.run(
                    [sys.executable, '-c', code, *order], cwd=package_dir.parent,
                    capture_output=True, text=True, encoding='utf-8', timeout=15,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout, '')
