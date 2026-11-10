#pragma once

#include <string>

namespace Corona::Systems::UI {

// Opening a project file may reload the native scene, so this single route
// stays in C++. Every other project file operation lives behind the Web ->
// Python bridge.
enum class FilesMethod { Open };
struct FilesRequest {
    FilesMethod method;
    std::string archive_session, project_path, path, file_type;
};
struct FilesReply {
    std::string error_code, error_message, archive_session, project_path;
    bool ok = false;
};

FilesReply execute_files_operation(const FilesRequest& request);

} // namespace Corona::Systems::UI
