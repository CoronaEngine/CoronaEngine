#pragma once

#include <cstdint>
#include <filesystem>
#include <set>
#include <optional>
#include <string>
#include <vector>

namespace Corona::Systems::UI::SceneFolders {

struct Diagnostic {
    std::string code;
    std::string message;
    std::filesystem::path path;
    std::string actor;
    std::string field;
};

struct ImportedFile {
    std::filesystem::path source;
    std::string route;
    std::string sha256;
    std::uint64_t size{};
};

struct ImportResult {
    std::string type{"model"};
    std::string main_route;
    std::string bundle_sha256;
    std::vector<ImportedFile> files;
    std::vector<std::string> dependencies;
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool ok() const noexcept {
        return diagnostics.empty() && !main_route.empty();
    }
};

[[nodiscard]] bool is_valid_asset_route(std::string_view route);
[[nodiscard]] std::string sha256_file(const std::filesystem::path& path);

class SceneAssetStore {
   public:
    explicit SceneAssetStore(std::filesystem::path scene_root,
                             bool allow_missing_manifest = false);

    [[nodiscard]] ImportResult import_model(const std::filesystem::path& source);
    [[nodiscard]] ImportResult import_actor(const std::filesystem::path& actor_source,
                                            const std::filesystem::path& model_source);
    [[nodiscard]] ImportResult import_file(const std::filesystem::path& source,
                                           std::string_view category);
    [[nodiscard]] bool write_manifest() const;
    [[nodiscard]] std::vector<Diagnostic> validate_manifest(bool verify_hashes = true) const;
    [[nodiscard]] bool contains_route(std::string_view route) const;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

   private:
    std::filesystem::path root_;
    std::vector<ImportResult> bundles_;
    std::optional<Diagnostic> manifest_error_;
};

struct CleanupResult {
    std::uint64_t removed_bundles{};
    std::uint64_t removed_files{};
    std::uint64_t reclaimed_bytes{};
    std::vector<Diagnostic> diagnostics;

    [[nodiscard]] bool ok() const noexcept { return diagnostics.empty(); }
};

[[nodiscard]] CleanupResult cleanup_unreferenced_assets(
    const std::filesystem::path& scene_root,
    const std::set<std::string>& referenced_routes, bool dry_run = true);

}  // namespace Corona::Systems::UI::SceneFolders
