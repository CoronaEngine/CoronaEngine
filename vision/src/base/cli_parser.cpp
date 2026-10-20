//
// Created by Zero on 09/09/2022.
//

#include "cli_parser.h"
#include "core/util/logging.h"

namespace vision {

using namespace ocarina;

CLIParser::CLIParser(int argc, char **argv)
    : argc_(argc), argv_(argv),
      cli_options_{ocarina::fs::path{argv[0]}.filename().string()} {
    init(argc, argv);
}

void CLIParser::init(int argc, char **argv) {
    cli_options_.add_options(
        "Renderer",
        {{"p, pipeline", "Select render pipeline:",
          cxxopts::value<std::string>()->default_value("fixed")},
         {"r, runtime-dir", "Specify runtime directory",
          cxxopts::value<fs::path>()->default_value(
              fs::canonical(argv[0]).parent_path().parent_path().string())},
         {"w, working-dir", "Specify working directory",
          cxxopts::value<fs::path>()->default_value(
              fs::canonical(fs::current_path()).string())},
         {"output-dir", "The output file path, default: the same directory as the scene description files",
          cxxopts::value<std::string>()->default_value("")},
         {"c, clear-cache", "Clear cached", cxxopts::value<bool>()},
         {"t, thread-num", "the num of threads to render", cxxopts::value<std::string>()->default_value("0")},
         {"s, scene", "The scene to render,file name end with json or scene supported by assimp", cxxopts::value<std::string>()},
         {"o, output", "The output rendering file path, output file will be saved as EXR format\n."
                       "Alternatively, you can specify the output file path in scene description \n"
                       "file(-s|--scene), this option will override the setting in scene file",
          cxxopts::value<std::string>()->default_value("")},
         {"warmup-frames", "Warmup frame count for headless evaluation", cxxopts::value<uint32_t>()->default_value("8")},
         {"profile-frames", "Profile frame count for headless evaluation", cxxopts::value<uint32_t>()->default_value("32")},
         {"save-spp", "Override scene output spp / save frame count for headless evaluation", cxxopts::value<uint32_t>()->default_value("0")},
         {"metrics-file", "Metrics output path for headless evaluation", cxxopts::value<std::string>()->default_value("")},
         {"rmse-reference", "Reference image path used to compute RMSE", cxxopts::value<std::string>()->default_value("")},
         {"disable-denoiser", "Disable runtime denoiser for headless evaluation", cxxopts::value<bool>()->default_value("false")->implicit_value("true")},
         {"stage-profile", "Synchronize and report per-stage frame times for retained profiling", cxxopts::value<bool>()->default_value("false")->implicit_value("true")},
         {"no-accumulation", "Disable frame buffer accumulation even when save-spp > 1 (for Phase 3 temporal ablation)", cxxopts::value<bool>()->default_value("false")->implicit_value("true")},
         {"camera-poses", "JSON file with camera trajectory keyframes for multi-pose evaluation", cxxopts::value<std::string>()->default_value("")},
         {"camera-override", "Override camera pose: x,y,z,pitch,yaw (for golden generation at specific poses)", cxxopts::value<std::string>()->default_value("")},
         {"positional", "Specify input file", cxxopts::value<std::string>()},
         {"n, spp", "Render N frames then save and exit",
          cxxopts::value<uint>()->default_value("0")},
         {"headless", "Run without creating a window",
          cxxopts::value<bool>()->default_value("false")},
         {"interactive-frames", "Run N interactive frames, print aggregate timing, then exit",
          cxxopts::value<uint>()->default_value("0")},
         {"denoise", "Enable realtime denoiser if the current integrator provides one"},
         {"m, maximized", "maximized window", cxxopts::value<bool>()},
         {"h, help", "Print usage"}});
}

const cxxopts::ParseResult &CLIParser::_parse_result() const noexcept {
    if (!parsed_cli_options_.has_value()) {
        parsed_cli_options_.emplace(
            cli_options_.parse(const_cast<int &>(argc_), const_cast<const char **&>(argv_)));
    }
    return *parsed_cli_options_;
}

void CLIParser::print_help() const noexcept {
    cout << cli_options_.help() << endl;
}

bool CLIParser::maximized_window() const noexcept {
    return _parse_result()["maximized"].as<bool>();
}

bool CLIParser::headless() const noexcept {
    return _parse_result()["headless"].as<bool>();
}

bool CLIParser::denoise() const noexcept {
    return _parse_result().count("denoise") > 0;
}

fs::path CLIParser::working_dir() const noexcept {
    return fs::canonical(_parse_result()["working-dir"].as<fs::path>());
}

fs::path CLIParser::runtime_dir() const noexcept {
    return fs::canonical(_parse_result()["runtime-dir"].as<fs::path>());
}

fs::path CLIParser::scene_path() const noexcept {
    return scene_file().parent_path();
}

fs::path CLIParser::output_dir() const noexcept {
    string value = _parse_result()["output-dir"].as<std::string>();
    if (value.empty()) {
        return {};
    }
    return fs::path(value);
}

fs::path CLIParser::scene_file() const noexcept {
    return fs::canonical(_parse_result()["scene"].as<std::string>());
}

bool CLIParser::clear_cache() const noexcept {
    return _parse_result()["clear-cache"].as<bool>();
}

string CLIParser::pipeline() const noexcept {
    return _parse_result()["pipeline"].as<string>();
}

string CLIParser::mode() const noexcept {
    if (_parse_result().count("mode") == 0) {
        return "";
    }
    return _parse_result()["mode"].as<string>();
}

uint CLIParser::spp() const noexcept {
    return _parse_result()["spp"].as<uint>();
}

uint CLIParser::interactive_frames() const noexcept {
    return _parse_result()["interactive-frames"].as<uint>();
}

string CLIParser::output_fn() const noexcept {
    return _parse_result()["output"].as<std::string>();
}

fs::path CLIParser::output_file() const noexcept {
    string value = _parse_result()["output"].as<std::string>();
    if (value.empty()) {
        return {};
    }
    return fs::path(value);
}

uint32_t CLIParser::warmup_frames() const noexcept {
    return _parse_result()["warmup-frames"].as<uint32_t>();
}

uint32_t CLIParser::profile_frames() const noexcept {
    return _parse_result()["profile-frames"].as<uint32_t>();
}

uint32_t CLIParser::save_spp() const noexcept {
    return _parse_result()["save-spp"].as<uint32_t>();
}

fs::path CLIParser::metrics_file() const noexcept {
    string value = _parse_result()["metrics-file"].as<std::string>();
    if (value.empty()) {
        return {};
    }
    return fs::path(value);
}

fs::path CLIParser::rmse_reference() const noexcept {
    string value = _parse_result()["rmse-reference"].as<std::string>();
    if (value.empty()) {
        return {};
    }
    return fs::path(value);
}

bool CLIParser::disable_denoiser() const noexcept {
    return _parse_result()["disable-denoiser"].as<bool>();
}

bool CLIParser::stage_profile() const noexcept {
    return _parse_result()["stage-profile"].as<bool>();
}

bool CLIParser::no_accumulation() const noexcept {
    return _parse_result()["no-accumulation"].as<bool>();
}

fs::path CLIParser::camera_poses() const noexcept {
    string value = _parse_result()["camera-poses"].as<std::string>();
    if (value.empty()) {
        return {};
    }
    return fs::path(value);
}

string CLIParser::camera_override() const noexcept {
    return _parse_result()["camera-override"].as<std::string>();
}

}// namespace vision
