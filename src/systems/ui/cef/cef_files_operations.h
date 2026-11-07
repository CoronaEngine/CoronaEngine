#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Corona::Systems::UI {

enum class FilesMethod { GetTree, CreateFolder, CreateActorTemplate, Rename, Delete, Open };
struct FilesRequest {
    FilesMethod method;
    std::string archive_session, project_path, path, name, file_type;
};
struct FileTreeEntry {
    std::string name, path;
    bool directory = false, expanded = false;
    int level = 0;
    std::optional<std::uint64_t> size;
    std::vector<FileTreeEntry> children;
};
struct FilesReply {
    std::string error_code, error_message, archive_session, project_path;
    std::optional<FileTreeEntry> tree;
    bool ok = false;
    std::string path, name;
};

FilesReply execute_files_operation(const FilesRequest& request);

} // namespace Corona::Systems::UI
