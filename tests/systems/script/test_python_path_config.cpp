
#include <corona/systems/script/python/python_path_config.h>
#include <corona/utils/runtime_paths.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;
using Corona::Script::Python::PathCfg::inspect_packaged_runtime;
using Corona::Script::Python::PathCfg::PackagedRuntimeIssue;
using Corona::Script::Python::PathCfg::platform_layout;
using Corona::Script::Python::PathCfg::posix_layout;
using Corona::Script::Python::PathCfg::PythonRuntimeLayout;
using Corona::Script::Python::PathCfg::windows_layout;

namespace {

void require(bool condition, std::string_view message) {
    if (condition) {
        return;
    }
    std::cerr << "PythonPathConfigTests failed: " << message << '\n';
    std::exit(1);
}

void set_env(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    if (value != nullptr && *value != '\0') {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
#endif
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

fs::path stdlib_path(const fs::path& root, const PythonRuntimeLayout& layout) {
    return root / layout.stdlib_rel;
}

fs::path ext_modules_path(const fs::path& root, const PythonRuntimeLayout& layout) {
    return root / layout.ext_modules_rel;
}

// A runtime tree shaped like the given layout: stdlib with site.py, encodings
// and one module, plus the extension-module directory.
void make_complete_runtime(const fs::path& root, const PythonRuntimeLayout& layout) {
    const fs::path stdlib = stdlib_path(root, layout);
    fs::create_directories(stdlib / "encodings");
    write_file(stdlib / "site.py");
    write_file(stdlib / "os.py");
    write_file(stdlib / "encodings" / "__init__.py");
    fs::create_directories(ext_modules_path(root, layout));
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

// ---------------------------------------------------------------------------
// The same five issues must be reported for every layout: the checks are about
// the layout data, not about the host this test binary was built on.
// ---------------------------------------------------------------------------

void check_missing_everything_reports_missing_stdlib(const PythonRuntimeLayout& layout,
                                                     std::string_view label) {
    TempRoot root(std::string(label) + "_empty");
    const auto result = inspect_packaged_runtime(root.path, layout);
    require(result.issue == PackagedRuntimeIssue::MissingStdlib,
            std::string(label) + ": empty root must report a missing stdlib directory");
    require(!result.available(), std::string(label) + ": empty root must not be available");
}

void check_stdlib_without_ext_modules_reports_missing_ext_modules(const PythonRuntimeLayout& layout,
                                                                  std::string_view label) {
    TempRoot root(std::string(label) + "_noext");
    const fs::path stdlib = stdlib_path(root.path, layout);
    fs::create_directories(stdlib / "encodings");
    write_file(stdlib / "site.py");

    const auto result = inspect_packaged_runtime(root.path, layout);
    require(result.issue == PackagedRuntimeIssue::MissingExtensionModules,
            std::string(label) + ": stdlib without the extension-module directory must report it");
}

void check_stdlib_and_ext_modules_without_site_module_is_rejected(const PythonRuntimeLayout& layout,
                                                                  std::string_view label) {
    TempRoot root(std::string(label) + "_nosite");
    const fs::path stdlib = stdlib_path(root.path, layout);
    fs::create_directories(stdlib / "encodings");
    write_file(stdlib / "os.py");
    fs::create_directories(ext_modules_path(root.path, layout));

    const auto result = inspect_packaged_runtime(root.path, layout);
    require(result.issue == PackagedRuntimeIssue::MissingSiteModule,
            std::string(label) + ": runtime without site.py must be reported as missing site module");
    require(!result.available(), std::string(label) + ": runtime without site.py must not be available");
}

void check_missing_encodings_is_reported(const PythonRuntimeLayout& layout, std::string_view label) {
    TempRoot root(std::string(label) + "_noencodings");
    const fs::path stdlib = stdlib_path(root.path, layout);
    fs::create_directories(stdlib);
    write_file(stdlib / "site.py");
    fs::create_directories(ext_modules_path(root.path, layout));

    const auto result = inspect_packaged_runtime(root.path, layout);
    require(result.issue == PackagedRuntimeIssue::MissingEncodings,
            std::string(label) + ": runtime without encodings must be reported as missing encodings");
}

void check_complete_runtime_is_available(const PythonRuntimeLayout& layout, std::string_view label) {
    TempRoot root(std::string(label) + "_complete");
    make_complete_runtime(root.path, layout);

    const auto result = inspect_packaged_runtime(root.path, layout);
    require(result.available(), std::string(label) + ": complete runtime must be available");
    require(result.issue == PackagedRuntimeIssue::None,
            std::string(label) + ": complete runtime must have no issue");
    require(result.root == root.path, std::string(label) + ": inspection must report the inspected root");
}

void check_describe_is_actionable(const PythonRuntimeLayout& layout, std::string_view label) {
    TempRoot root(std::string(label) + "_describe");
    const fs::path stdlib = stdlib_path(root.path, layout);
    fs::create_directories(stdlib);
    write_file(stdlib / "os.py");
    fs::create_directories(ext_modules_path(root.path, layout));

    const std::string text = inspect_packaged_runtime(root.path, layout).describe();
    require(text.find("site.py") != std::string::npos,
            std::string(label) + ": describe() must name the missing piece (site.py) so the log is actionable");
    require(text.find(root.path.filename().string()) != std::string::npos,
            std::string(label) + ": describe() must include the inspected root path");
    require(text.find(stdlib.filename().string()) != std::string::npos,
            std::string(label) + ": describe() must name the layout's stdlib directory");
}

void check_describe_names_extension_modules_dir(const PythonRuntimeLayout& layout,
                                                std::string_view label) {
    TempRoot root(std::string(label) + "_describeext");
    make_complete_runtime(root.path, layout);
    fs::remove_all(ext_modules_path(root.path, layout));

    const std::string text = inspect_packaged_runtime(root.path, layout).describe();
    require(text.find(layout.ext_modules_rel.generic_string()) != std::string::npos,
            std::string(label) + ": describe() must name the layout's extension-module directory");
}

void run_layout_suite(const PythonRuntimeLayout& layout, std::string_view label) {
    check_missing_everything_reports_missing_stdlib(layout, label);
    check_stdlib_without_ext_modules_reports_missing_ext_modules(layout, label);
    check_stdlib_and_ext_modules_without_site_module_is_rejected(layout, label);
    check_missing_encodings_is_reported(layout, label);
    check_complete_runtime_is_available(layout, label);
    check_describe_is_actionable(layout, label);
    check_describe_names_extension_modules_dir(layout, label);
}

// ---------------------------------------------------------------------------
// Layout-independent behaviour
// ---------------------------------------------------------------------------

void test_packaged_runtime_required_reads_env() {
    using Corona::Script::Python::PathCfg::packaged_runtime_required;

    set_env("CORONA_REQUIRE_PACKAGED_PYTHON", "");
    require(!packaged_runtime_required(), "unset env must not require the packaged runtime");

    set_env("CORONA_REQUIRE_PACKAGED_PYTHON", "0");
    require(!packaged_runtime_required(), "env=0 must not require the packaged runtime");

    set_env("CORONA_REQUIRE_PACKAGED_PYTHON", "1");
    require(packaged_runtime_required(), "env=1 must require the packaged runtime");

    set_env("CORONA_REQUIRE_PACKAGED_PYTHON", "");
}

void test_decision_prefers_packaged_runtime() {
    using Corona::Script::Python::PathCfg::decide_packaged_runtime_usage;
    using Corona::Script::Python::PathCfg::PackagedRuntimeDecision;

    TempRoot root("decision_ok");
    make_complete_runtime(root.path, platform_layout());
    const auto packaged = inspect_packaged_runtime(root.path);

    require(decide_packaged_runtime_usage(packaged, false, true) ==
                PackagedRuntimeDecision::UsePackaged,
            "usable packaged runtime must be used even when a fallback exists");
    require(decide_packaged_runtime_usage(packaged, true, false) ==
                PackagedRuntimeDecision::UsePackaged,
            "usable packaged runtime must be used in strict mode too");
}

void test_decision_falls_back_only_when_allowed_and_possible() {
    using Corona::Script::Python::PathCfg::decide_packaged_runtime_usage;
    using Corona::Script::Python::PathCfg::PackagedRuntimeDecision;

    TempRoot root("decision_fallback");
    fs::create_directories(stdlib_path(root.path, platform_layout()));
    write_file(stdlib_path(root.path, platform_layout()) / "site.py");
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
    // platform_layout() must be the host's layout, not an arbitrary default.
#ifdef _WIN32
    require(&platform_layout() == &windows_layout(),
            "platform_layout() must be the Windows layout on Windows");
#else
    require(&platform_layout() == &posix_layout(),
            "platform_layout() must be the POSIX layout on POSIX");
#endif

    {
        TempRoot unrelated("unrelated_cwd");
        const auto previous = fs::current_path();
        fs::current_path(unrelated.path);
        const auto native_root = Corona::Utils::executable_path().parent_path();
        using namespace Corona::Script::Python::PathCfg;
        require(engine_root_path() == native_root, "runtime root must not depend on cwd");
        require(fs::u8path(engine_root()) == native_root, "Python root must be UTF-8");
        require(fs::u8path(editor_backend_abs()) == native_root / "CabbageEditor",
                "editor imports must resolve beside the executable, including Unicode paths");
#ifdef _WIN32
        if (const auto expected = _wgetenv(L"CORONA_TEST_EXPECTED_RUNTIME_ROOT")) {
            require(native_root == fs::path(expected), "long executable path must not be truncated");
        }
#endif
        fs::current_path(previous);
    }

    run_layout_suite(windows_layout(), "windows");
    run_layout_suite(posix_layout(), "posix");

    test_packaged_runtime_required_reads_env();
    test_decision_prefers_packaged_runtime();
    test_decision_falls_back_only_when_allowed_and_possible();
    std::cout << "PythonPathConfigTests passed\n";
    return 0;
}
