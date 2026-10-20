//
// Created by Zero on 09/09/2022.
//

#pragma once

#include "cxxopts.hpp"
#include "core/stl.h"
#include "base/using.h"

namespace vision {


class CLIParser {
private:
    int argc_;
    char **argv_;
    mutable cxxopts::Options cli_options_;
    mutable std::optional<cxxopts::ParseResult> parsed_cli_options_;

private:
    const cxxopts::ParseResult &_parse_result() const noexcept;

public:
    CLIParser(int argc, char **argv);
    void init(int argc, char **argv);
    void print_help() const noexcept;
    [[nodiscard]] bool has_help_cmd() const noexcept { return _parse_result().count("help") > 0; }
    void try_print_help_and_exit() const noexcept {
        if (has_help_cmd()) {
            print_help();
            exit(0);
        }
    }
    [[nodiscard]] bool clear_cache() const noexcept;
    [[nodiscard]] string pipeline() const noexcept;
    [[nodiscard]] string mode() const noexcept;
    [[nodiscard]] fs::path scene_file() const noexcept;
    [[nodiscard]] fs::path working_dir() const noexcept;
    [[nodiscard]] fs::path runtime_dir() const noexcept;
    [[nodiscard]] fs::path scene_path() const noexcept;
    [[nodiscard]] fs::path output_dir() const noexcept;
    [[nodiscard]] fs::path output_file() const noexcept;
    [[nodiscard]] uint32_t warmup_frames() const noexcept;
    [[nodiscard]] uint32_t profile_frames() const noexcept;
    [[nodiscard]] uint32_t save_spp() const noexcept;
    [[nodiscard]] fs::path metrics_file() const noexcept;
    [[nodiscard]] fs::path rmse_reference() const noexcept;
    [[nodiscard]] bool disable_denoiser() const noexcept;
    [[nodiscard]] bool stage_profile() const noexcept;
    [[nodiscard]] bool no_accumulation() const noexcept;
    [[nodiscard]] fs::path camera_poses() const noexcept;
    [[nodiscard]] string camera_override() const noexcept;
    [[nodiscard]] bool maximized_window() const noexcept;
    [[nodiscard]] bool headless() const noexcept;
    [[nodiscard]] bool denoise() const noexcept;
    [[nodiscard]] string output_fn() const noexcept;
    [[nodiscard]] uint spp() const noexcept;
    [[nodiscard]] uint interactive_frames() const noexcept;
};

}// namespace vision

