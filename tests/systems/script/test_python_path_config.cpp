
#include <corona/systems/script/python/python_path_config.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using Corona::Script::Python::PathCfg::PackagedRuntimeIssue;
using Corona::Script::Python::PathCfg::inspect_packaged_runtime;

namespace {

void require(bool condition, std::string_view message) {
    if (condition) {
        return;
    }
    std::cerr << "PythonPathConfigTests failed: " << message << '\n';
    std::exit(1);
}

std::string unique_root_name(std::string_view tag) {
    static int counter = 0;
    return "corona_pycfg_" + std::string(tag) + "_" + std::to_string(++counter);
}

void write_file(const fs::path& path, std::string_view content = "x") {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out << content;
}

void make_complete_runtime(const fs::path& root) {
    fs::create_directories(root / "Lib" / "encodings");
    write_file(root / "Lib" / "site.py");
    write_file(root / "Lib" / "os.py");
    write_file(root / "Lib" / "encodings" / "__init__.py");
    fs::create_directories(root / "DLLs");
}

struct TempRoot {
    fs::path path;

    explicit TempRoot(std::string_view tag)
        : path(fs::temp_directory_path() / unique_root_name(tag)) {
        fs::remove_all(path);
        fs::create_directories(path);
    }

    ~TempRoot() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempRoot(const TempRoot&) = delete;
    TempRoot& operator=(const TempRoot&) = delete;
};

void test_missing_everything_reports_missing_lib() {
    TempRoot root("empty");
    const auto result = inspect_packaged_runtime(root.path);
    require(result.issue == PackagedRuntimeIssue::MissingLib, "empty root must report missing Lib");
    require(!result.available(), "empty root must not be available");
}

void test_lib_without_dlls_reports_missing_dlls() {
    TempRoot root("nodlls");
    fs::create_directories(root.path / "Lib" / "encodings");
    write_file(root.path / "Lib" / "site.py");

    const auto result = inspect_packaged_runtime(root.path);
    require(result.issue == PackagedRuntimeIssue::MissingDlls, "Lib without DLLs must report missing DLLs");
}

void test_lib_and_dlls_without_site_module_is_rejected() {
    TempRoot root("nosite");
    fs::create_directories(root.path / "Lib" / "encodings");
    write_file(root.path / "Lib" / "os.py");
    fs::create_directories(root.path / "DLLs");

    const auto result = inspect_packaged_runtime(root.path);
    require(result.issue == PackagedRuntimeIssue::MissingSiteModule,
            "runtime without Lib/site.py must be reported as missing site module");
    require(!result.available(), "runtime without Lib/site.py must not be available");
}

void test_missing_encodings_is_reported() {
    TempRoot root("noencodings");
    fs::create_directories(root.path / "Lib");
    write_file(root.path / "Lib" / "site.py");
    fs::create_directories(root.path / "DLLs");

    const auto result = inspect_packaged_runtime(root.path);
    require(result.issue == PackagedRuntimeIssue::MissingEncodings,
            "runtime without Lib/encodings must be reported as missing encodings");
}

void test_complete_runtime_is_available() {
    TempRoot root("complete");
    make_complete_runtime(root.path);

    const auto result = inspect_packaged_runtime(root.path);
    require(result.available(), "complete runtime must be available");
    require(result.issue == PackagedRuntimeIssue::None, "complete runtime must have no issue");
    require(result.root == root.path, "inspection must report the inspected root");
}

void test_describe_is_actionable() {
    TempRoot root("describe");
    fs::create_directories(root.path / "Lib");
    write_file(root.path / "Lib" / "os.py");
    fs::create_directories(root.path / "DLLs");

    const auto unavailable = inspect_packaged_runtime(root.path);
    const std::string text = unavailable.describe();
    require(text.find("site.py") != std::string::npos,
            "describe() must name the missing piece (Lib/site.py) so the log is actionable");
    require(text.find(root.path.filename().string()) != std::string::npos,
            "describe() must include the inspected root path");

    TempRoot ok_root("describe_ok");
    make_complete_runtime(ok_root.path);
    const std::string ok_text = inspect_packaged_runtime(ok_root.path).describe();
    require(!ok_text.empty(), "describe() must also describe the usable case");
}

void test_packaged_runtime_required_reads_env() {
    using Corona::Script::Python::PathCfg::packaged_runtime_required;

    _putenv_s("CORONA_REQUIRE_PACKAGED_PYTHON", "");
    require(!packaged_runtime_required(), "unset env must not require the packaged runtime");

    _putenv_s("CORONA_REQUIRE_PACKAGED_PYTHON", "0");
    require(!packaged_runtime_required(), "env=0 must not require the packaged runtime");

    _putenv_s("CORONA_REQUIRE_PACKAGED_PYTHON", "1");
    require(packaged_runtime_required(), "env=1 must require the packaged runtime");

    _putenv_s("CORONA_REQUIRE_PACKAGED_PYTHON", "");
}

void test_decision_prefers_packaged_runtime() {
    using Corona::Script::Python::PathCfg::PackagedRuntimeDecision;
    using Corona::Script::Python::PathCfg::decide_packaged_runtime_usage;
    using Corona::Script::Python::PathCfg::PackagedRuntimeInspection;

    TempRoot root("decision_ok");
    make_complete_runtime(root.path);
    const auto packaged = inspect_packaged_runtime(root.path);

    require(decide_packaged_runtime_usage(packaged, false, true) ==
                PackagedRuntimeDecision::UsePackaged,
            "usable packaged runtime must be used even when a fallback exists");
    require(decide_packaged_runtime_usage(packaged, true, false) ==
                PackagedRuntimeDecision::UsePackaged,
            "usable packaged runtime must be used in strict mode too");
}

void test_decision_falls_back_only_when_allowed_and_possible() {
    using Corona::Script::Python::PathCfg::PackagedRuntimeDecision;
    using Corona::Script::Python::PathCfg::decide_packaged_runtime_usage;

    TempRoot root("decision_fallback");
    fs::create_directories(root.path / "Lib");
    write_file(root.path / "Lib" / "site.py");
    const auto incomplete = inspect_packaged_runtime(root.path);

    require(decide_packaged_runtime_usage(incomplete, false, true) ==
                PackagedRuntimeDecision::UseFallback,
            "incomplete runtime with an available fallback must be allowed to fall back");
    require(decide_packaged_runtime_usage(incomplete, true, true) ==
                PackagedRuntimeDecision::RejectNoPackaged,
            "strict mode must reject even when a fallback exists");
    require(decide_packaged_runtime_usage(incomplete, false, false) ==
                PackagedRuntimeDecision::RejectNoFallback,
            "missing fallback must be rejected instead of silently using a nonexistent path");
    require(decide_packaged_runtime_usage(incomplete, true, false) ==
                PackagedRuntimeDecision::RejectNoPackaged,
            "strict mode takes precedence when nothing is available");
}

}  // namespace

int main() {
    test_missing_everything_reports_missing_lib();
    test_lib_without_dlls_reports_missing_dlls();
    test_lib_and_dlls_without_site_module_is_rejected();
    test_missing_encodings_is_reported();
    test_complete_runtime_is_available();
    test_describe_is_actionable();
    test_packaged_runtime_required_reads_env();
    test_decision_prefers_packaged_runtime();
    test_decision_falls_back_only_when_allowed_and_possible();
    std::cout << "PythonPathConfigTests passed\n";
    return 0;
}
