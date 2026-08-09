"""Project settings adapter for the native ``projectSettings.*`` contract."""

from api.editor_api import CoronaEditorApi


class ProjectSettings:
    @staticmethod
    def get_active_project_info():
        return CoronaEditorApi.project_settings.get_active_project_info()

    @staticmethod
    def save_active_project_info(settings):
        return CoronaEditorApi.project_settings.save_active_project_info(settings or {})

__all__ = ["ProjectSettings"]
