"""ProjectSettings public adapter.

Project metadata ownership lives in the native ``projectSettings.*``
aggregate.  This module keeps the historical Python plugin entry points for
older hosts without duplicating configuration parsing or persistence.
"""

from api.editor_api import CoronaEditorApi


class ProjectSettings:
    @staticmethod
    def get_active_project_info():
        return CoronaEditorApi.project_settings.get_active_project_info()

    @staticmethod
    def save_active_project_info(settings):
        return CoronaEditorApi.project_settings.save_active_project_info(settings or {})
