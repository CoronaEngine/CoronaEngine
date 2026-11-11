// Full editor test host. The test controller sends file commands to this C++ thread,
// without a renderer frame, and observes Actor geometry independently through CEF.
#include <corona/engine.h>
#include <corona/systems/ui/cef_runtime.h>
#include <corona/utils/path_utils.h>
#include <corona/systems/ui/file_script_api.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>

namespace UI = Corona::Systems::UI;
namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

// The request/response protocol with the test controller stays name-based; the mapping
// to the editor's named entry points lives here, in the probe, not in production code.
nlohmann::json dispatch(const std::string& api, const nlohmann::json& args) {
    if (api == "Scripts.run_file") return UI::run_project_script_file(args);
    if (api == "Scripts.run_path") return UI::run_project_script_path(args);
    if (api == "Scripts.get_status") return UI::project_script_status(args);
    if (api == "Scripts.get_run") return UI::project_script_run(args);
    if (api == "Scripts.stop") return UI::stop_project_script(args);
    return {{"success", false}, {"code", 404}, {"error", api + " is not a file script route"}};
}

}  // namespace

int main(int argc, char** argv) {
    if (auto exit = UI::execute_cef_subprocess_if_needed(argc, argv)) return *exit;
    const char* control = std::getenv("CORONA_NATIVE_FILE_PROBE");
    if (!control) return 2;
    const auto root = Corona::Utils::utf8_to_path(std::string(control));
    Corona::Engine engine;
    if (!engine.initialize()) return 1;
    std::jthread caller([root](std::stop_token stopping) {
        for (unsigned seq = 0; !stopping.stop_requested(); ++seq) {
            const auto path = root / (std::to_string(seq) + ".request");
            std::ifstream input;
            while (!input.is_open()) {
                if (stopping.stop_requested()) return;
                input.open(path, std::ios::binary);
                if (!input.is_open()) std::this_thread::sleep_for(10ms);
            }
            nlohmann::json reply;
            try {
                const auto message = nlohmann::json::parse(input);
                reply = dispatch(message.at("api").get<std::string>(), message.at("args"));
            } catch (const std::exception& error) {
                reply = {{"success", false}, {"error", error.what()}};
            }
            const auto response = root / (std::to_string(seq) + ".reply");
            const auto tmp = root / "reply.tmp";
            { std::ofstream out(tmp); out << reply.dump(); }
            fs::rename(tmp, response);
        }
    });
    engine.run();
    caller.request_stop();
    caller.join();
    engine.shutdown();
    return 0;
}
