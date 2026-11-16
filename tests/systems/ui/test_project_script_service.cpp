// 脚本/项目文件桥规则（WNC-014）：纯 DTO/文件系统路径，无 CEF 依赖。
#include "editor_api/editor_api_services.h"

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
    test_bridge_error_code();
    test_document_identity();
    test_writable_project();
    test_project_settings_error_code();
    std::cout << "project_script_service: " << checks << " checks passed\n";
    return 0;
}
