import configparser
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

EDITOR_ROOT = Path(__file__).resolve().parents[3]
if str(EDITOR_ROOT) not in sys.path:
    sys.path.insert(0, str(EDITOR_ROOT))

_import_cwd = Path.cwd()
with tempfile.TemporaryDirectory() as _import_temp_dir:
    os.chdir(_import_temp_dir)
    try:
        from config.settings import CoronaSettings
    finally:
        os.chdir(_import_cwd)


class RecentProjectSettingsTests(unittest.TestCase):
    @staticmethod
    def _write_settings(path: Path, projects):
        config = configparser.ConfigParser()
        config['General'] = {'version': '1.2.0'}
        config['History'] = {'recent_projects': json.dumps(projects)}
        with path.open('w', encoding='utf-8') as handle:
            config.write(handle)

    def test_get_recent_projects_deduplicates_slash_variants_and_cleans_config(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            project = root / 'creative_world_1'
            project.mkdir()
            (project / 'project.ini').write_text(
                '[Project]\nname = ????_1\nlast_opened = 2026-07-23 10:00:00\n',
                encoding='utf-8',
            )
            config_path = root / 'CoronaEditor.ini'
            slash_path = str(project).replace('\\', '/')
            backslash_path = str(project).replace('/', '\\')
            self._write_settings(config_path, [slash_path, backslash_path])

            settings = CoronaSettings(str(config_path))
            recent = settings.get_recent_projects()

            self.assertEqual(len(recent), 1)
            self.assertEqual(recent[0]['name'], '????_1')
            cleaned = json.loads(settings.config.get('History', 'recent_projects'))
            self.assertEqual(cleaned, [str(project.resolve())])

    def test_add_recent_project_moves_existing_project_to_front_without_duplicates(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            first = root / 'creative_world_1'
            second = root / 'creative_world_2'
            first.mkdir()
            second.mkdir()
            config_path = root / 'CoronaEditor.ini'
            self._write_settings(
                config_path,
                [str(first).replace('\\', '/'), str(second), str(first)],
            )

            settings = CoronaSettings(str(config_path))
            settings.add_recent_project(str(second).replace('\\', '/'))

            cleaned = json.loads(settings.config.get('History', 'recent_projects'))
            self.assertEqual(cleaned, [str(second.resolve()), str(first.resolve())])


if __name__ == '__main__':
    unittest.main()
