from pathlib import Path


def test_script_runtime_bundled_audio_uses_repository_assets_root():
    from script_runtime.engine.corona_engine import _bundled_audio_root

    repo_root = Path(__file__).resolve().parents[3]

    assert _bundled_audio_root() == repo_root / "assets" / "audio"
