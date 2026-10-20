//
// Created by Zero on 26/10/2022.
//

#pragma once

#include <iostream>
#include "base/cli_parser.h"
#include "base/import/project_desc.h"
#include "core/stl.h"
#include "base/mgr/pipeline.h"
#include "rhi/context.h"
#include "core/image/image.h"
#include "core/util/logging.h"
#include "base/denoiser.h"
#include "base/mgr/global.h"
#include "base/using.h"
#include "window/window.h"

namespace vision {

struct LaunchParams {
    fs::path working_dir;
    fs::path scene_file;
    fs::path scene_path;
    fs::path output_dir;
    string output_fn;
    bool clear_cache{false};
    bool maximized_window{false};
    bool headless{false};
    bool denoise{false};
    uint spp{0};
    uint interactive_frames{0};

    LaunchParams() = default;

    void init(const CLIParser *cli_parser) noexcept {
        if (cli_parser == nullptr) {
            return;
        }
        working_dir = cli_parser->working_dir();
        scene_path = cli_parser->scene_path();
        Global::instance().set_scene_path(scene_path);
        scene_file = cli_parser->scene_file();
        output_dir = cli_parser->output_dir();
        clear_cache = cli_parser->clear_cache();
        headless = cli_parser->headless();
        denoise = cli_parser->denoise();
        spp = cli_parser->spp();
        interactive_frames = cli_parser->interactive_frames();
        output_fn = cli_parser->output_fn();
        maximized_window = cli_parser->maximized_window();
    }
};
class App : public GUI {
public:
    UP<CLIParser> cli_parser{};
    Device device;
    mutable WindowPtr window{};
    SP<Pipeline> rp{};
    float2 last_cursor_pos = make_float2(0);
    bool left_key_press{false};
    bool right_key_press{false};
    bool invalidation{false};

    bool key_r_press{false};
    bool key_f_press{false};

    // debugger switching
    bool key_g_press{false};
    bool interactive_benchmark_reported{false};

    LaunchParams params;

public:
    App(int argc, char *argv[])
        : cli_parser(make_unique<CLIParser>(argc, argv)),
          device(RHIContext::instance().create_device()) {
        init(argc);
    }
    void init(int argc = 0);
    void prepare();
    void update(double dt) noexcept;
    bool render_UI(Widgets *widgets) noexcept override;
    void init_pipeline();
    [[nodiscard]] Pipeline &pipeline() const { return *rp; }
    void register_event() noexcept;
    void update_camera_view(float d_yaw, float d_pitch) const noexcept;
    void on_key_event(int key, int action) noexcept;
    void on_window_size_change(uint2 size) noexcept;
    void on_mouse_event(int button, int action, float2 pos) noexcept;
    void on_scroll_event(float2 scroll) noexcept;
    void on_cursor_move(float2 pos) noexcept;
    int run() noexcept;
};
}// namespace vision