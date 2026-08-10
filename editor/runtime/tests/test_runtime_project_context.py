import sys
import types

from runtime import project_context


def test_native_project_context_is_authoritative_over_legacy_settings(monkeypatch):
    native_engine = types.SimpleNamespace(active_project_path="C:/native-project")
    legacy_settings = types.SimpleNamespace(active_project_path="C:/legacy-project")
    legacy_module = types.ModuleType("config.project_state")
    legacy_module.settings_manager = legacy_settings

    monkeypatch.setitem(sys.modules, "config.project_state", legacy_module)
    monkeypatch.setattr(
        project_context,
        "_resolve_native_engine",
        lambda native_engine_arg=None: native_engine,
    )

    assert project_context.get_active_project_path() == "C:/native-project"
