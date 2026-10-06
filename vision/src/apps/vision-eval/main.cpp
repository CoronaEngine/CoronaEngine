//
// Headless evaluation entrypoint for deterministic light-field experiments.
//

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <numeric>
#include <sstream>
#ifdef _WIN32
#include <crtdbg.h>
#include <windows.h>
#endif
#include "base/cli_parser.h"
#include "base/import/importer.h"
#include "base/integral/integrator.h"
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"
#include "core/image/image.h"
#include "core/util/logging.h"
#include "rhi/context.h"
#include "ext/nlohmann/json.hpp"

using namespace ocarina;
using namespace vision;
using njson = nlohmann::json;

void save_pixel_diagnostics(Pipeline &pipeline);

namespace {

#ifdef _WIN32
void configure_headless_process() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
}
#else
void configure_headless_process() {
}
#endif

// ============================================================================
// Camera Trajectory
// ============================================================================

struct CameraKeyframe {
    uint frame{0};
    float3 delta_pos{};
    float delta_pitch{0.f};
    float delta_yaw{0.f};
};

struct CameraTrajectory {
    vector<CameraKeyframe> keyframes;
    vector<uint> save_frames;

    [[nodiscard]] bool active() const noexcept { return !keyframes.empty(); }

    [[nodiscard]] uint max_frame() const noexcept {
        uint m = 0;
        for (const auto &kf : keyframes) m = std::max(m, kf.frame);
        for (auto f : save_frames) m = std::max(m, f);
        return m;
    }
};

struct InterpolatedPose {
    float3 delta_pos{};
    float delta_pitch{0.f};
    float delta_yaw{0.f};
};

[[nodiscard]] InterpolatedPose interpolate_pose(const CameraTrajectory &traj, uint frame) {
    if (traj.keyframes.empty()) return {};
    if (frame <= traj.keyframes.front().frame) {
        const auto &kf = traj.keyframes.front();
        return {kf.delta_pos, kf.delta_pitch, kf.delta_yaw};
    }
    if (frame >= traj.keyframes.back().frame) {
        const auto &kf = traj.keyframes.back();
        return {kf.delta_pos, kf.delta_pitch, kf.delta_yaw};
    }
    for (size_t i = 0; i + 1 < traj.keyframes.size(); ++i) {
        const auto &a = traj.keyframes[i];
        const auto &b = traj.keyframes[i + 1];
        if (frame >= a.frame && frame <= b.frame) {
            float t = static_cast<float>(frame - a.frame) / static_cast<float>(b.frame - a.frame);
            InterpolatedPose p;
            p.delta_pos = lerp(t, a.delta_pos, b.delta_pos);
            p.delta_pitch = lerp(t, a.delta_pitch, b.delta_pitch);
            p.delta_yaw = lerp(t, a.delta_yaw, b.delta_yaw);
            return p;
        }
    }
    const auto &kf = traj.keyframes.back();
    return {kf.delta_pos, kf.delta_pitch, kf.delta_yaw};
}

[[nodiscard]] CameraTrajectory load_trajectory(const fs::path &path) {
    CameraTrajectory traj;
    std::ifstream f(path);
    if (!f.is_open()) {
        OC_WARNING_FORMAT("Cannot open camera trajectory file: {}", path.string());
        return traj;
    }
    njson j = njson::parse(f);
    if (j.contains("keyframes")) {
        for (const auto &kf : j["keyframes"]) {
            CameraKeyframe k;
            k.frame = kf.value("frame", 0u);
            if (kf.contains("delta_pos")) {
                auto dp = kf["delta_pos"];
                k.delta_pos = make_float3(dp[0].get<float>(), dp[1].get<float>(), dp[2].get<float>());
            }
            k.delta_pitch = kf.value("delta_pitch", 0.f);
            k.delta_yaw = kf.value("delta_yaw", 0.f);
            traj.keyframes.push_back(k);
        }
    }
    if (j.contains("save_frames")) {
        for (const auto &sf : j["save_frames"]) {
            traj.save_frames.push_back(sf.get<uint>());
        }
    }
    std::sort(traj.keyframes.begin(), traj.keyframes.end(),
              [](const CameraKeyframe &a, const CameraKeyframe &b) { return a.frame < b.frame; });
    std::sort(traj.save_frames.begin(), traj.save_frames.end());
    return traj;
}

struct CameraOverride {
    float3 position{};
    float pitch{0.f};
    float yaw{0.f};
    bool active{false};
};

[[nodiscard]] CameraOverride parse_camera_override(const string &s) {
    CameraOverride co;
    if (s.empty()) return co;
    std::istringstream iss(s);
    char sep;
    if (iss >> co.position.x >> sep >> co.position.y >> sep >> co.position.z >> sep
            >> co.pitch >> sep >> co.yaw) {
        co.active = true;
    } else {
        OC_WARNING_FORMAT("Invalid --camera-override format: '{}'. Expected: x,y,z,pitch,yaw", s);
    }
    return co;
}

// ============================================================================
// Eval Stats
// ============================================================================

struct PoseMetric {
    uint frame{0};
    float3 delta_pos{};
    float delta_pitch{0.f};
    float delta_yaw{0.f};
    double rmse{0.0};
    fs::path output_file;
};

struct EvalStats {
    uint warmup_frames{0u};
    uint profile_frames{0u};
    double average_frame_ms{0.0};
    double average_fps{0.0};
    double rmse{0.0};
    bool stage_profile_enabled{false};
    double average_gbuffer_ms{0.0};
    double average_sampling_mask_ms{0.0};
    double average_path_tracing_ms{0.0};
    double average_spatial_angular_ms{0.0};
    double average_temporal_ms{0.0};
    double average_combine_ms{0.0};
    double average_postprocess_ms{0.0};
    double average_render_final_ms{0.0};
    fs::path output_file;
    bool trajectory_active{false};
    vector<PoseMetric> pose_metrics;
};

[[nodiscard]] vector<float4> load_as_float4(const fs::path &path, uint2 &res) {
    Image image = Image::load(path, ColorSpace::LINEAR);
    res = image.resolution();
    vector<float4> pixels(image.pixel_num());
    if (image.pixel_storage() == PixelStorage::FLOAT4) {
        auto src = image.pixel_ptr<float4>();
        std::copy(src, src + image.pixel_num(), pixels.begin());
        return pixels;
    }
    auto [format, converted] = Image::convert_to_32bit(image.pixel_storage(), image.pixel_ptr(), image.resolution());
    OC_ASSERT(format == PixelStorage::FLOAT4);
    auto src = reinterpret_cast<const float4 *>(converted);
    std::copy(src, src + image.pixel_num(), pixels.begin());
    delete_array(converted);
    return pixels;
}

[[nodiscard]] double compute_rmse(const fs::path &reference_path, const fs::path &output_path) {
    uint2 ref_res{};
    uint2 out_res{};
    vector<float4> ref = load_as_float4(reference_path, ref_res);
    vector<float4> out = load_as_float4(output_path, out_res);
    OC_ASSERT(all(ref_res == out_res));
    double accum = 0.0;
    size_t sample_count = static_cast<size_t>(ref.size()) * 3u;
    for (size_t i = 0; i < ref.size(); ++i) {
        float3 delta = ref[i].xyz() - out[i].xyz();
        accum += static_cast<double>(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
    }
    return std::sqrt(accum / static_cast<double>(sample_count));
}

void write_metrics(const fs::path &path, const EvalStats &stats) {
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path());
    }
    std::ofstream out(path);
    out << "{\n";
    out << "  \"warmup_frames\": " << stats.warmup_frames << ",\n";
    out << "  \"profile_frames\": " << stats.profile_frames << ",\n";
    out << "  \"average_frame_ms\": " << stats.average_frame_ms << ",\n";
    out << "  \"average_fps\": " << stats.average_fps << ",\n";
    out << "  \"rmse\": " << stats.rmse << ",\n";
    out << "  \"stage_profile_enabled\": " << (stats.stage_profile_enabled ? "true" : "false") << ",\n";
    out << "  \"average_gbuffer_ms\": " << stats.average_gbuffer_ms << ",\n";
    out << "  \"average_sampling_mask_ms\": " << stats.average_sampling_mask_ms << ",\n";
    out << "  \"average_path_tracing_ms\": " << stats.average_path_tracing_ms << ",\n";
    out << "  \"average_spatial_angular_ms\": " << stats.average_spatial_angular_ms << ",\n";
    out << "  \"average_temporal_ms\": " << stats.average_temporal_ms << ",\n";
    out << "  \"average_combine_ms\": " << stats.average_combine_ms << ",\n";
    out << "  \"average_postprocess_ms\": " << stats.average_postprocess_ms << ",\n";
    out << "  \"average_render_final_ms\": " << stats.average_render_final_ms << ",\n";
    out << "  \"output_file\": \"" << stats.output_file.generic_string() << "\",\n";
    out << "  \"trajectory_active\": " << (stats.trajectory_active ? "true" : "false");
    if (!stats.pose_metrics.empty()) {
        out << ",\n  \"pose_metrics\": [\n";
        for (size_t i = 0; i < stats.pose_metrics.size(); ++i) {
            const auto &pm = stats.pose_metrics[i];
            out << "    {\"frame\": " << pm.frame
                << ", \"rmse\": " << pm.rmse
                << ", \"delta_pos\": [" << pm.delta_pos.x << ", " << pm.delta_pos.y << ", " << pm.delta_pos.z << "]"
                << ", \"delta_pitch\": " << pm.delta_pitch
                << ", \"delta_yaw\": " << pm.delta_yaw
                << ", \"output_file\": \"" << pm.output_file.generic_string() << "\""
                << "}";
            if (i + 1 < stats.pose_metrics.size()) out << ",";
            out << "\n";
        }
        out << "  ]";
    }
    out << "\n}\n";
}

// ============================================================================
// EvalApp
// ============================================================================

class EvalApp {
private:
    CLIParser cli_parser_;
    Device device_;
    SP<Pipeline> pipeline_{};
    fs::path scene_file_;
    fs::path output_root_;
    fs::path output_file_;
    fs::path metrics_file_;
    fs::path rmse_reference_;
    uint warmup_frames_{0u};
    uint profile_frames_{0u};
    uint save_spp_{0u};
    bool stage_profile_{false};
    CameraTrajectory trajectory_;
    CameraOverride camera_override_;

public:
    EvalApp(int argc, char **argv)
        : cli_parser_(argc, argv),
          device_(RHIContext::instance().create_device(cli_parser_.backend())) {
        init();
    }

    void init() {
        core::log_level_info();
        if (cli_parser_.has_help_cmd()) {
            cli_parser_.print_help();
            exit(0);
        }
        scene_file_ = cli_parser_.scene_file();
        output_root_ = cli_parser_.output_dir();
        output_file_ = cli_parser_.output_file();
        metrics_file_ = cli_parser_.metrics_file();
        rmse_reference_ = cli_parser_.rmse_reference();
        warmup_frames_ = cli_parser_.warmup_frames();
        profile_frames_ = cli_parser_.profile_frames();
        save_spp_ = cli_parser_.save_spp();
        stage_profile_ = cli_parser_.stage_profile();

        // Load trajectory if specified
        fs::path traj_path = cli_parser_.camera_poses();
        if (!traj_path.empty() && fs::exists(traj_path)) {
            trajectory_ = load_trajectory(traj_path);
            if (trajectory_.active()) {
                OC_INFO_FORMAT("Loaded camera trajectory with {} keyframes, {} save frames",
                               trajectory_.keyframes.size(), trajectory_.save_frames.size());
            }
        }

        // Parse camera override
        camera_override_ = parse_camera_override(cli_parser_.camera_override());

        Global::instance().set_device(&device_);
        device_.init_rtx();
        Global::instance().set_scene_path(scene_file_.parent_path());
        pipeline_ = Importer::import_scene(scene_file_);
        if (cli_parser_.disable_denoiser()) {
            _putenv_s("VISION_DISABLE_DENOISER", "1");
            auto *integrator = pipeline_->renderer().integrator().get();
            if (auto *illum = dynamic_cast<IlluminationIntegrator *>(integrator)) {
                if (auto *denoiser = illum->denoiser()) {
                    denoiser->set_enabled(false);
                }
            }
        } else {
            _putenv_s("VISION_DISABLE_DENOISER", "");
        }
        _putenv_s("VISION_STAGE_PROFILE", stage_profile_ ? "1" : "");

        if (cli_parser_.clear_cache()) {
            RHIContext::instance().clear_cache();
        }

        if (!output_root_.empty()) {
            fs::create_directories(output_root_);
            Global::instance().set_scene_path(output_root_);
        }

        if (!output_file_.empty()) {
            fs::path resolved = output_file_;
            if (resolved.has_parent_path()) {
                fs::create_directories(resolved.parent_path());
                Global::instance().set_scene_path(resolved.parent_path());
                pipeline_->output_desc().fn = resolved.filename().string();
            } else {
                pipeline_->output_desc().fn = resolved.string();
            }
        }

        if (save_spp_ > 0u) {
            pipeline_->output_desc().spp = save_spp_;
            pipeline_->output_desc().save_exit = false;
        } else if (pipeline_->output_desc().spp == 0u) {
            pipeline_->output_desc().spp = warmup_frames_ + profile_frames_;
        }

        if (save_spp_ > 1u && !cli_parser_.no_accumulation()) {
            pipeline_->frame_buffer()->set_enable_accumulation(true);
        }

        pipeline_->prepare();
        pipeline_->frame_buffer()->prepare_view_texture();

        // Apply camera override after prepare (sensor is initialized)
        if (camera_override_.active) {
            auto *sensor = pipeline_->scene().sensor().get();
            sensor->set_position(camera_override_.position);
            sensor->set_pitch(camera_override_.pitch);
            sensor->set_yaw(camera_override_.yaw);
            sensor->update_device_data();
            sensor->store_prev_data();
            OC_INFO_FORMAT("Camera override applied: pos=({}, {}, {}), pitch={}, yaw={}",
                           camera_override_.position.x, camera_override_.position.y,
                           camera_override_.position.z, camera_override_.pitch, camera_override_.yaw);
        }
    }

    void save_frame_image(uint frame, const fs::path &base_path) {
        fs::path dir = base_path.parent_path();
        string stem = base_path.stem().string();
        string ext = base_path.extension().string();
        if (ext.empty()) ext = ".png";

        char suffix[32];
        snprintf(suffix, sizeof(suffix), "_frame_%04u", frame);
        fs::path out_path = dir / (stem + suffix + ext);

        vector<float4> pixels(pipeline_->pixel_num());
        OutputDesc desc = pipeline_->output_desc();
        desc.fn = out_path.filename().string();
        pipeline_->final_picture(desc, pixels.data());
        Image::save_image(out_path, PixelStorage::FLOAT4, pipeline_->resolution(), pixels.data());
        save_linear_image(out_path);
        save_pixel_diagnostics(*pipeline_);
    }

    // Opt-in evaluation data. final_picture is already exposed/tone-mapped;
    // comparisons of transport energy need the actual pre-tonemap float buffer.
    void save_linear_image(const fs::path &image_path) {
        const char *enabled = std::getenv("VISION_EVAL_LINEAR_OUTPUT");
        if (!enabled || enabled[0] != '1') return;
        const auto resolution = pipeline_->resolution();
        vector<float4> pixels(pipeline_->pixel_num());
        pipeline_->stream() << pipeline_->frame_buffer()->display_source_buffer().download(pixels.data())
                            << synchronize() << commit();
        auto path = image_path;
        path.replace_extension(".pfm");
        std::ofstream out(path, std::ios::binary);
        out << "PF\n" << resolution.x << ' ' << resolution.y << "\n-1.0\n";
        for (uint y = resolution.y; y > 0; --y) {
            for (uint x = 0; x < resolution.x; ++x) {
                const auto &p = pixels[(y - 1) * resolution.x + x];
                const float rgb[] = {p.x, p.y, p.z};
                out.write(reinterpret_cast<const char *>(rgb), sizeof(rgb));
            }
        }
        if (!out) throw std::runtime_error("Failed to write linear evaluation image: " + path.string());
        const auto *sensor = pipeline_->scene().sensor().get();
        const auto position = sensor->position();
        njson pose = {{"position", {position.x, position.y, position.z}},
                      {"pitch", sensor->pitch()}, {"yaw", sensor->yaw()},
                      {"frame_index", pipeline_->frame_index()},
                      {"denoise", pipeline_->output_desc().denoise},
                      {"accumulation", pipeline_->frame_buffer()->enable_accumulation()}};
        path.replace_extension(".pose.json");
        std::ofstream metadata(path);
        metadata << pose.dump(2);
          if (!metadata) throw std::runtime_error("Failed to write evaluation pose: " + path.string());
    }

    [[nodiscard]] EvalStats run() {
        constexpr double dt = 1.0 / 60.0;
        vector<double> profiled_ms;
        vector<IntegratorStageProfile> stage_profiles;
        profiled_ms.reserve(profile_frames_);
        stage_profiles.reserve(profile_frames_);

        // Determine total frames
        uint base_total = std::max(pipeline_->output_desc().spp, warmup_frames_ + profile_frames_);
        uint total_frames = base_total;
        if (trajectory_.active()) {
            uint traj_end = trajectory_.max_frame() + 16u;
            total_frames = std::max(total_frames, traj_end);
        }

        // Record initial camera state for trajectory interpolation
        auto *sensor = pipeline_->scene().sensor().get();
        float3 initial_pos = sensor->position();
        float initial_pitch = sensor->pitch();
        float initial_yaw = sensor->yaw();

        // Build save_frames set for fast lookup
        std::set<uint> save_frame_set(trajectory_.save_frames.begin(), trajectory_.save_frames.end());

        // Output path for per-frame saves
        fs::path frame_output_base = Global::instance().scene_path() / pipeline_->output_desc().fn;

        for (uint frame = 0u; frame < total_frames; ++frame) {
            // Camera trajectory update
            if (trajectory_.active()) {
                auto pose = interpolate_pose(trajectory_, frame);
                sensor->set_position(initial_pos + pose.delta_pos);
                sensor->set_pitch(initial_pitch + pose.delta_pitch);
                sensor->set_yaw(initial_yaw + pose.delta_yaw);
                sensor->update_device_data();
            }

            pipeline_->upload_data();
            pipeline_->display(dt);
            pipeline_->check_and_save();
            pipeline_->reset_status();

            // Per-frame save at trajectory save points
            if (trajectory_.active() && save_frame_set.count(frame)) {
                save_frame_image(frame, frame_output_base);
            }

            if (frame >= warmup_frames_ && profiled_ms.size() < profile_frames_) {
                profiled_ms.push_back(pipeline_->cur_render_time());
                stage_profiles.push_back(pipeline_->renderer().integrator()->cur_stage_profile());
            }
        }

        // Headless evaluation should measure and save the same final realtime state.
        // This matters for temporal denoisers such as SVGF: save_spp=1 still means
        // one new sample per frame, but the PNG should not be the first frame before
        // warmup/profile history has converged.
        pipeline_->save_result();
        save_linear_image(Global::instance().scene_path() / pipeline_->output_desc().fn);

        EvalStats stats;
        stats.warmup_frames = warmup_frames_;
        stats.profile_frames = static_cast<uint>(profiled_ms.size());
        if (!profiled_ms.empty()) {
            double sum = std::accumulate(profiled_ms.begin(), profiled_ms.end(), 0.0);
            stats.average_frame_ms = sum / static_cast<double>(profiled_ms.size());
            stats.average_fps = stats.average_frame_ms > 0.0 ? 1000.0 / stats.average_frame_ms : 0.0;
        }
        if (!stage_profiles.empty() && stage_profiles.front().enabled) {
            auto average_stage = [&](auto accessor) {
                double sum = 0.0;
                for (const auto &profile : stage_profiles) {
                    sum += accessor(profile);
                }
                return sum / static_cast<double>(stage_profiles.size());
            };
            stats.stage_profile_enabled = true;
            stats.average_gbuffer_ms = average_stage([](const IntegratorStageProfile &profile) { return profile.gbuffer_ms; });
            stats.average_sampling_mask_ms = average_stage([](const IntegratorStageProfile &profile) { return profile.sampling_mask_ms; });
            stats.average_path_tracing_ms = average_stage([](const IntegratorStageProfile &profile) { return profile.path_tracing_ms; });
            stats.average_spatial_angular_ms = average_stage([](const IntegratorStageProfile &profile) { return profile.spatial_angular_ms; });
            stats.average_temporal_ms = average_stage([](const IntegratorStageProfile &profile) { return profile.temporal_ms; });
            stats.average_combine_ms = average_stage([](const IntegratorStageProfile &profile) { return profile.combine_ms; });
            stats.average_postprocess_ms = average_stage([](const IntegratorStageProfile &profile) { return profile.postprocess_ms; });
            stats.average_render_final_ms = average_stage([](const IntegratorStageProfile &profile) { return profile.render_final_ms; });
        }

        fs::path final_output = Global::instance().scene_path() / pipeline_->output_desc().fn;
        stats.output_file = final_output;

        // Static RMSE (standard eval) — only if reference is a file, not a directory
        if (!rmse_reference_.empty() && fs::exists(rmse_reference_) &&
            fs::is_regular_file(rmse_reference_) && fs::exists(final_output)) {
            stats.rmse = compute_rmse(rmse_reference_, final_output);
        }

        // Per-pose RMSE for trajectory mode
        stats.trajectory_active = trajectory_.active();
        if (trajectory_.active()) {
            fs::path golden_dir = rmse_reference_.empty() ? fs::path{} : rmse_reference_.parent_path();
            for (uint sf : trajectory_.save_frames) {
                PoseMetric pm;
                pm.frame = sf;
                auto pose = interpolate_pose(trajectory_, sf);
                pm.delta_pos = pose.delta_pos;
                pm.delta_pitch = pose.delta_pitch;
                pm.delta_yaw = pose.delta_yaw;

                fs::path dir = frame_output_base.parent_path();
                string stem = frame_output_base.stem().string();
                string ext = frame_output_base.extension().string();
                if (ext.empty()) ext = ".png";
                char suffix[32];
                snprintf(suffix, sizeof(suffix), "_frame_%04u", sf);
                pm.output_file = dir / (stem + suffix + ext);

                if (!golden_dir.empty()) {
                    char golden_suffix[32];
                    snprintf(golden_suffix, sizeof(golden_suffix), "_frame_%04u.png", sf);
                    fs::path golden_path = golden_dir / (string("golden") + golden_suffix);
                    if (fs::exists(golden_path) && fs::exists(pm.output_file)) {
                        pm.rmse = compute_rmse(golden_path, pm.output_file);
                    }
                }
                stats.pose_metrics.push_back(pm);
            }
        }

        if (!metrics_file_.empty()) {
            write_metrics(metrics_file_, stats);
        }
        return stats;
    }
};

}// namespace

int main(int argc, char **argv) {
    configure_headless_process();
    CLIParser bootstrap_parser(argc, argv);
    fs::path runtime_dir = bootstrap_parser.runtime_dir();
    fs::current_path(runtime_dir);
    RHIContext::instance().init(runtime_dir);
    EvalApp app(argc, argv);
    EvalStats stats = app.run();
    std::cout << "average_frame_ms=" << stats.average_frame_ms << "\n";
    std::cout << "average_fps=" << stats.average_fps << "\n";
    if (stats.rmse > 0.0) {
        std::cout << "rmse=" << stats.rmse << "\n";
    }
    if (stats.stage_profile_enabled) {
        std::cout << "average_gbuffer_ms=" << stats.average_gbuffer_ms << "\n";
        std::cout << "average_sampling_mask_ms=" << stats.average_sampling_mask_ms << "\n";
        std::cout << "average_path_tracing_ms=" << stats.average_path_tracing_ms << "\n";
        std::cout << "average_spatial_angular_ms=" << stats.average_spatial_angular_ms << "\n";
        std::cout << "average_temporal_ms=" << stats.average_temporal_ms << "\n";
        std::cout << "average_combine_ms=" << stats.average_combine_ms << "\n";
        std::cout << "average_postprocess_ms=" << stats.average_postprocess_ms << "\n";
        std::cout << "average_render_final_ms=" << stats.average_render_final_ms << "\n";
    }
    std::cout << "output_file=" << stats.output_file.string() << "\n";
    if (stats.trajectory_active && !stats.pose_metrics.empty()) {
        std::cout << "trajectory_poses=" << stats.pose_metrics.size() << "\n";
        for (const auto &pm : stats.pose_metrics) {
            std::cout << "pose_frame_" << pm.frame << "_rmse=" << pm.rmse << "\n";
        }
    }
    return 0;
}
