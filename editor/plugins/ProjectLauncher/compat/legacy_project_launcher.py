from runtime.plugin_base import PluginBase
from api.editor_api import CoronaEditorApi


@PluginBase.register_web("ProjectLauncher")
class ProjectLauncher(PluginBase):

    @staticmethod
    def get_default_project_path() -> str:
        return CoronaEditorApi.project.get_default_project_path()

    @staticmethod
    def get_app_version() -> str:
        return CoronaEditorApi.project.get_app_version()

    @staticmethod
    def get_recent_projects() -> list:
        return CoronaEditorApi.project.get_recent_projects()

    @staticmethod
    def create_project(project_data: dict) -> str:
        return CoronaEditorApi.project.create_project(project_data)

    @staticmethod
    def create_world_project(world_data: dict) -> dict:
        return CoronaEditorApi.project.create_world_project(world_data)

    @staticmethod
    def create_multiplayer_project(project_data: dict) -> dict:
        return CoronaEditorApi.project.create_multiplayer_project(project_data)

    @staticmethod
    def open_project(project_path: str) -> dict:
        return CoronaEditorApi.project.open_project(project_path, {})

    @staticmethod
    def set_project_mode(mode_data: dict) -> bool:
        return CoronaEditorApi.project.set_project_mode(mode_data)
