from pathlib import Path
from unittest.mock import patch

import pytest

from runtime.archive.project import create_project


def denied(code=5):
    error = PermissionError('directory is temporarily held open')
    error.winerror = code
    return error


def test_creation_retries_transient_windows_publish_without_exposing_partial_project(tmp_path):
    target = tmp_path / 'world'
    rename = Path.rename
    attempts = []

    def publish(staged, destination):
        attempts.append(staged)
        assert not target.exists()
        assert (staged / 'scene.ini').is_file()
        assert (staged / 'assets.manifest.json').is_file()
        if len(attempts) < 3:
            raise denied()
        return rename(staged, destination)

    with patch.object(Path, 'rename', publish), patch('time.sleep'):
        result = create_project({'path': str(target), 'name': 'World', 'world': {'type': 'creative'}})
    assert result == {'path': str(target)}
    assert len(attempts) == 3
    assert len(set(attempts)) == 1
    assert sorted(path.name for path in tmp_path.iterdir()) == ['world']


def test_creation_publish_retries_are_bounded_and_staging_is_cleaned(tmp_path):
    with patch.object(Path, 'rename', side_effect=denied()) as rename, patch('time.sleep') as sleep:
        with pytest.raises(PermissionError):
            create_project({'path': str(tmp_path / 'world'), 'name': 'World'})
    assert rename.call_count == 6
    assert sleep.call_count == 5
    assert list(tmp_path.iterdir()) == []


def test_creation_does_not_retry_other_filesystem_failures(tmp_path):
    with patch.object(Path, 'rename', side_effect=denied(13)) as rename, patch('time.sleep') as sleep:
        with pytest.raises(PermissionError):
            create_project({'path': str(tmp_path / 'world'), 'name': 'World'})
    assert rename.call_count == 1
    sleep.assert_not_called()
    assert list(tmp_path.iterdir()) == []


def test_creation_does_not_overwrite_a_destination_claimed_during_publish(tmp_path):
    target = tmp_path / 'world'

    def competing_publish(staged, destination):
        target.mkdir()
        (target / 'existing.txt').write_text('preserve', encoding='utf-8')
        raise denied()

    with patch.object(Path, 'rename', competing_publish), patch('time.sleep') as sleep:
        with pytest.raises(PermissionError):
            create_project({'path': str(target), 'name': 'World'})
    sleep.assert_not_called()
    assert (target / 'existing.txt').read_text(encoding='utf-8') == 'preserve'
    assert sorted(path.name for path in tmp_path.iterdir()) == ['world']
