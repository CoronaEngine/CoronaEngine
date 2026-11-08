// 脚本/项目文件桥规则（WNC-014）：纯 DTO/文件系统路径，无 CEF 依赖。
#include "editor_api/project_script_service.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

using namespace Corona::Systems::UI::ProjectScriptService;

namespace {

int checks = 0;

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

void test_script_path() {
    check(!validate_script_path(std::filesystem::path("scripts/main.py")), "relative py accepted");
    check(!validate_script_path(std::filesystem::path("a/b/c.tool.py")), "nested py accepted");

    auto error = validate_script_path(std::filesystem::path(""));
    check(error && error.code == "INVALID_ARGUMENT" &&
          error.message == "Script path must be a project-relative .py file", "empty path rejected");

    check(static_cast<bool>(validate_script_path(std::filesystem::path("C:/abs/main.py"))),
          "rooted path rejected");

    check(static_cast<bool>(validate_script_path(std::filesystem::path("scripts/main.txt"))),
          "non-py rejected");

    check(static_cast<bool>(validate_script_path(std::filesystem::path("scripts/main"))),
          "extension-less path rejected");
}

void test_script_project() {
    check(!validate_script_project(false, true, true), "writable matching project accepted");

    auto error = validate_script_project(true, true, true);
    check(error && error.code == "PROJECT_CONTEXT_CHANGED" &&
          error.message == "Script project changed or is read-only", "read-only project rejected");

    check(static_cast<bool>(validate_script_project(false, false, true)), "missing project path rejected");
    check(static_cast<bool>(validate_script_project(false, true, false)), "different project rejected");
}

void test_target_type() {
    check(default_target_type("Hero") == "actor", "actor name selects the actor target");
    check(default_target_type("") == "project", "no actor name runs the project");
}

void test_bridge_error_code() {
    check(bridge_error_code(504) == "EDITOR_API_TIMEOUT", "timeout mapped");
    check(bridge_error_code(409) == "PROJECT_CONTEXT_CHANGED", "conflict mapped");
    check(bridge_error_code(500) == "NATIVE_FAILURE", "other codes map to native failure");
    check(bridge_error_code(0) == "NATIVE_FAILURE", "zero maps to native failure");
    check(project_files_error_code(504) == "EDITOR_API_TIMEOUT", "project files map timeouts");
    check(project_files_error_code(409) == "NATIVE_FAILURE", "project files keep conflicts as native failure");
}

void test_document_identity() {
    check(!validate_document_identity("session", "session", true), "matching document identity accepted");

    auto error = validate_document_identity("", "session", true);
    check(error && error.code == "PROJECT_CONTEXT_CHANGED", "missing scene session rejected");

    check(static_cast<bool>(validate_document_identity("session", "old", true)), "stale payload session rejected");
    check(static_cast<bool>(validate_document_identity("session", "session", false)), "other project rejected");
}

void test_writable_project() {
    auto error = require_writable_project(true);
    check(error && error.code == "READ_ONLY_PROJECT", "read-only project rejects writes");
    check(!require_writable_project(false), "writable project accepted");
}

void test_project_settings_error_code() {
    check(project_settings_error_code(409) == "PROJECT_CONTEXT_CHANGED", "conflict mapped");
    check(project_settings_error_code(403) == "READ_ONLY_PROJECT", "read-only mapped");
    check(project_settings_error_code(504) == "EDITOR_API_TIMEOUT", "timeout mapped");
    check(project_settings_error_code(500) == "NATIVE_FAILURE", "other codes map to native failure");
}

}  // namespace

int main() {
    test_script_path();
    test_script_project();
    test_target_type();
    test_bridge_error_code();
    test_document_identity();
    test_writable_project();
    test_project_settings_error_code();
    std::cout << "project_script_service: " << checks << " checks passed\n";
    return 0;
}
