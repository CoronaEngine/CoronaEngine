"""Durability, idempotency and boundary tests for the small-world roster."""
from pathlib import Path
import json
import tempfile
import unittest
from unittest.mock import patch

from game.runtime import subworld_roster as roster


class RosterTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / '主世界'
        self.root.mkdir()
        self.path = self.root / roster.SAVE_PATH

    def read(self):
        return self.path.read_text(encoding='utf-8')

    def test_missing_file_is_an_empty_roster(self):
        self.assertEqual(roster.load(self.root), {'version': 1, 'subworlds': []})

    def test_add_creates_stable_sequential_ids(self):
        first = roster.add(self.root)
        second = roster.add(self.root)
        self.assertEqual(first['subworlds'][0]['id'], 'subworld-1')
        self.assertEqual(second['subworlds'][1]['id'], 'subworld-2')
        self.assertEqual(roster.load(self.root), second)

    def test_rename_updates_name_and_persists(self):
        roster.add(self.root)
        state = roster.rename(self.root, 'subworld-1', '  埃及世界  ')
        self.assertEqual(state['subworlds'][0]['name'], '埃及世界')

    def test_rename_rejects_blank_overlong_and_duplicate(self):
        roster.add(self.root)
        roster.add(self.root)
        for name in ('  ', '', 'x' * 33):
            with self.assertRaises(roster.RosterError):
                roster.rename(self.root, 'subworld-1', name)
        with self.assertRaises(roster.RosterError):
            roster.rename(self.root, 'subworld-1', '未命名小世界')

    def test_rename_unknown_id_rejected(self):
        with self.assertRaises(roster.RosterError):
            roster.rename(self.root, 'subworld-99', '埃及世界')

    def test_cap_rejects_overflow(self):
        subworlds = [{'id': 'subworld-%d' % i, 'name': '世界 %d' % i}
                     for i in range(roster.MAX_SUBWORLDS)]
        roster.save(self.root, subworlds)
        with self.assertRaises(roster.RosterError):
            roster.add(self.root)

    def test_corrupt_file_is_not_overwritten(self):
        roster.add(self.root)
        self.path.write_text('{broken', encoding='utf-8')
        with self.assertRaises(roster.RosterError):
            roster.load(self.root)
        with self.assertRaises(roster.RosterError):
            roster.rename(self.root, 'subworld-1', '埃及世界')
        self.assertEqual(self.read(), '{broken')

    def test_oversize_file_is_rejected(self):
        (self.root / '.game').mkdir()
        self.path.write_text(' ' * (roster.MAX_BYTES + 1), encoding='utf-8')
        with self.assertRaises(roster.RosterError):
            roster.load(self.root)

    def test_atomic_replace_failure_keeps_previous_roster(self):
        roster.add(self.root)
        previous = self.path.read_bytes()
        # `save` lets the I/O error propagate like the exhibition save does; the
        # gameplay handler turns it into a rejected request instead of a partial write.
        with patch.object(roster.os, 'replace', side_effect=OSError('disk full')):
            with self.assertRaises(OSError):
                roster.add(self.root)
        self.assertEqual(self.path.read_bytes(), previous)
        self.assertEqual(list(self.path.parent.glob('.story-roster-*')), [])

    def test_invalid_entries_and_duplicate_ids_rejected(self):
        for state in ({'version': 1, 'subworlds': [{'id': 'x', 'name': 'x' * 0}]},
                      {'version': 2, 'subworlds': []},
                      {'version': 1, 'subworlds': [{'id': 'bad id', 'name': 'x'}]},
                      {'version': 1, 'subworlds': [{'id': 'a', 'name': 'x'}, {'id': 'a', 'name': 'y'}]}):
            with self.subTest(state=state):
                with self.assertRaises(roster.RosterError):
                    roster.validate_state(state)


if __name__ == '__main__':
    unittest.main()
