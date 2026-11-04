#include "cef/project_document_store.h"
#include <iostream>
#include <thread>
#include <vector>
#include <atomic>

using Json = nlohmann::json;
namespace fs = std::filesystem;
using Corona::Systems::UI::project_document_store;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
int main(int argc, char** argv) {
    try {
        require(argc == 2, "expected isolated fixture directory");
        const auto root = fs::u8path(argv[1]) / fs::u8path("中文-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(root);
        Json request = {{"document", "CabbageAssistant/context.json"}};
        auto read = project_document_store(root, request, false);
        require(read["success"] && read["content"].is_null(), "missing document");
        request["version"] = read["version"];
        request["content"] = {{"messages", {"中文 🌱"}}, {"revision", 1}};
        require(project_document_store(root, request, true)["success"], "first write");
        require(project_document_store(root, request, true)["error"] == "DOCUMENT_CONFLICT", "stale version");
        read = project_document_store(root, request, false);
        require(read["content"] == request["content"], "Unicode roundtrip");
        request["version"] = read["version"];
        std::atomic_int winners = 0;
        std::vector<std::thread> writers;
        for (int i = 0; i < 6; ++i) {
            writers.emplace_back([&, i] {
                auto next = request;
                next["content"]["revision"] = i + 2;
                if (project_document_store(root, next, true)["success"]) ++winners;
            });
        }
        for (auto& writer : writers) writer.join();
        require(winners == 1, "CAS lost update");
        auto invalid = request;
        invalid["document"] = "../outside.json";
        require(project_document_store(root, invalid, false)["error"] == "INVALID_PROJECT_DOCUMENT", "path traversal");
        { std::ofstream corrupt(root / "corrupt.json"); corrupt << "{bad"; }
        request = {{"document", "corrupt.json"}};
        read = project_document_store(root, request, false);
        require(read["corrupt"], "corrupt document");
        request["version"] = read["version"]; request["content"] = {{"recovered", true}};
        require(project_document_store(root, request, true)["success"], "recover corrupt document");
        bool backup = false;
        for (auto& entry : fs::directory_iterator(root))
            if (entry.path().filename().string().find(".corrupt.json") != std::string::npos) backup = true;
        require(backup, "corrupt backup");
        std::cout << "project document store: passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
