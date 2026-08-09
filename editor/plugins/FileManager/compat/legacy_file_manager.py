"""FileManager public adapter.

Filesystem ownership lives in the native ``files.*`` aggregate.  This module
keeps the historical Python plugin entry points for older hosts without
reimplementing path validation, file operations, or legacy Scene bindings.
"""

from api.editor_api import CoronaEditorApi


class FileManager:
    @staticmethod
    def get_project_info():
        return CoronaEditorApi.files.get_project_info()

    @staticmethod
    def get_files(relative_path=""):
        return CoronaEditorApi.files.get_files(relative_path)

    @staticmethod
    def get_file_tree(relative_path=""):
        return CoronaEditorApi.files.get_file_tree(relative_path)

    @staticmethod
    def create_folder(path, folder_name):
        return CoronaEditorApi.files.create_folder(path, folder_name)

    @staticmethod
    def create_file(path, file_name, file_type):
        return CoronaEditorApi.files.create_file(path, file_name, file_type)

    @staticmethod
    def delete_item(path):
        return CoronaEditorApi.files.delete_item(path)

    @staticmethod
    def rename_item(old_path, new_name):
        return CoronaEditorApi.files.rename_item(old_path, new_name)

    @staticmethod
    def open_file(path, file_type):
        return CoronaEditorApi.files.open_file(path, file_type)
