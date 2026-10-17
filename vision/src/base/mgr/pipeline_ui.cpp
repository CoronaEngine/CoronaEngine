//
// Created by Z on 2025/03/21.
//


#include "pipeline_ui.h"
#include "pipeline.h"

namespace vision {
using namespace ocarina;

namespace {

void render_perf_stats(Widgets *widgets, const Pipeline &pipeline) noexcept {
    double render_time_avg = pipeline.frame_index() == 0 ? 0.0 : pipeline.render_time() / static_cast<double>(pipeline.frame_index());
    widgets->text("pipeline type: %s", pipeline.impl_type().data());
    widgets->text("FPS: %.1f"
                  "\nrender time: %.3f ms"
                  "\ndenoise time: %.3f ms"
                  "\nrender time average: %.3f"
                  "\ntotal time: %.3f"
                  "\ntotal time average: %.3f"
                  "\nframe index: %u",
                  pipeline.frame_delta_ms() > 0.0 ? 1000.0 / pipeline.frame_delta_ms() : 0.0,
                  pipeline.cur_render_time(),
                  pipeline.renderer().integrator()->cur_denoise_time(),
                  render_time_avg,
                  pipeline.frame_delta_ms(),
                  pipeline.avg_frame_delta_ms(),
                  pipeline.frame_index());
}

}// namespace

bool PipelineUI::render_perf_panel(Widgets *widgets) noexcept {
    if (!show_fps_) {
        return true;
    }
    widgets->use_window("render stats", [&]() {
        render_perf_stats(widgets, *pipeline_);
    });
    return true;
}

bool PipelineUI::render(Widgets *widgets) noexcept {
    widgets->use_window("render stats", [&]() {
        if (show_fps_) {
            render_perf_stats(widgets, *pipeline_);
        }
        widgets->check_box("scene data", &show_scene_data_);
        widgets->check_box("detail", &show_detail_);
        widgets->check_box("framebuffer", &show_framebuffer_data_);
        widgets->check_box("stats", &show_stats_);
        widgets->check_box("hotfix", &show_hotfix_);
        widgets->check_box("output setting", &show_output_);
        widgets->button_click("clear cache", [&] {
            RHIContext::instance().clear_cache();
        });
    });
    if (show_scene_data_) {
        pipeline_->render_scene_ui(widgets);
        pipeline_->render_renderer_ui(widgets);
    }
    if (show_detail_) {
        render_detail(widgets);
    }
    if (show_framebuffer_data_) {
        widgets->use_window("frame buffer", [&] {
            pipeline_->render_framebuffer_ui(widgets);
        });
    }
    if (show_stats_) {
        render_stats(widgets);
    }
    if (show_hotfix_) {
        render_hotfix(widgets);
    }
    if (show_output_) {
        render_output(widgets);
    }
    return true;
}

struct FileNameEditor {
    static constexpr auto len = 256;
    char data[len] = {};
    int ext_index{0};
    vector<const char *> exts = {".exr", ".png", ".jpg", ".hdr"};

    explicit FileNameEditor(const fs::path &fn) noexcept {
        sync(fn);
    }

    void sync(const fs::path &fn) noexcept {
        std::fill(std::begin(data), std::end(data), '\0');
        string stem = fn.stem().string();
        size_t copy_count = std::min(stem.size(), len - 1ull);
        for (size_t i = 0; i < copy_count; ++i) {
            data[i] = stem[i];
        }
        string ext = fn.extension().string();
        for (int i = 0; i < static_cast<int>(exts.size()); ++i) {
            if (ext == exts[i]) {
                ext_index = i;
                return;
            }
        }
        ext_index = 0;
    }

    [[nodiscard]] string render_UI(Widgets *widgets) noexcept {
        widgets->input_text("fn", data, len);
        widgets->combo("ext", &ext_index, exts);
        string ext = exts[ext_index];
        string stem = data;
        return stem + ext;
    }
};

void PipelineUI::render_output(Widgets *widgets) noexcept {
    widgets->use_window("output setting", [&] {
        const OutputDesc &output_desc = pipeline_->output_desc();
        uint spp = output_desc.spp;
        bool save_exit = output_desc.save_exit;
        bool denoise = output_desc.denoise;
        widgets->drag_uint("frame num", &spp, 10, 1, InvalidUI32);
        widgets->check_box("exit after save", &save_exit);
        widgets->check_box("use denoise", &denoise);
        static FileNameEditor fnp{output_desc.fn};
        fnp.sync(output_desc.fn);
        string fn = fnp.render_UI(widgets);
        widgets->button_click("save", [&] {
            pipeline_->request_save();
        });
        widgets->button_click("offline rendering", [&] {
            pipeline_->offline_rendering();
        });
        pipeline_->set_output_spp(spp);
        pipeline_->set_output_save_exit(save_exit);
        pipeline_->set_output_denoise(denoise);
        pipeline_->set_output_fn(std::move(fn));
    });
}

void PipelineUI::render_detail(Widgets *widgets) noexcept {
    if (cur_node_ == nullptr) {
        return;
    }
    widgets->use_window("detail", [&] {
        cur_node_->render_UI(widgets);
    });
}

namespace {
void hotfix_window(Widgets *widgets) noexcept {
    HotfixSystem &hotfix = HotfixSystem::instance();
    if (hotfix.is_working()) {
        return;
    }
    widgets->button_click("reload", [&] {
        bool update = hotfix.check_and_build();
        if (!update) {
            OC_INFO("no modification");
        }
    });

    widgets->set_enabled(hotfix.has_previous(), [&] {
        widgets->button_click("previous", [&] {
            hotfix.previous_version();
        });
    });

    widgets->set_enabled(hotfix.has_next(), [&] {
        widgets->button_click("next", [&] {
            hotfix.next_version();
        });
    });
}
}// namespace

void PipelineUI::render_hotfix(Widgets *widgets) noexcept {
    widgets->use_window("hotfix system", [&] {
        hotfix_window(widgets);
    });
}

void PipelineUI::render_stats(Widgets *widgets) noexcept {
    auto tex_size = MemoryStats::instance().tex_size();
    auto buffer_size = MemoryStats::instance().buffer_size();
    BindlessStats bindless_stats = pipeline_->bindless_stats();
    size_t tex_slot_mem_size = bindless_stats.texture_slot_mem_size;
    size_t buffer_slot_mem_size = bindless_stats.buffer_slot_mem_size;
    auto label = ocarina::format("memory stats total is {}", bytes_string(tex_size + buffer_size +
                                                                          tex_slot_mem_size + buffer_slot_mem_size));
    widgets->use_window(label, [&] {
        widgets->use_folding_header("texture stats", [&] {
            widgets->text("total texture size is %s", bytes_string(tex_size).c_str());
            MemoryStats::instance().foreach_tex_info([&](auto data) {
                double percent = double(data.size()) / tex_size;
                widgets->text(ocarina::format("size {}, percent {:.2f} %%, tex name {}\n",
                                              bytes_string(data.size()), percent * 100, data.name));
            });
        });

        widgets->use_folding_header("buffer stats", [&] {
            widgets->text("total buffer size is %s", bytes_string(buffer_size).c_str());
            MemoryStats::instance().foreach_buffer_info([&](auto data) {
                double percent = double(data.size) / buffer_size;
                widgets->text(ocarina::format("size {}, percent {:.2f} %%, block {}\n",
                                              bytes_string(data.size), percent * 100,
                                              data.name));
            });
        });

        widgets->use_folding_header("mesh stats", [&] {
            GeometryStats geometry_stats = pipeline_->geometry_stats();
            auto string = ocarina::format("vertex num is {}\ntriangle num is {}\nmesh num is {}",
                                          geometry_stats.vertex_num,
                                          geometry_stats.triangle_num,
                                          geometry_stats.mesh_num);
            widgets->text(string);
        });

        widgets->use_folding_header("bindless_array stats", [&] {
            size_t max_slot_num = BindlessArray::max_slot_num();

            widgets->text(ocarina::format("bindless buffer num is {}", bindless_stats.buffer_num));
            widgets->text(ocarina::format("bindless tex num is {}", bindless_stats.texture_num));
            widgets->text(ocarina::format("max slot num is {}", max_slot_num));
            widgets->text(ocarina::format("bindless slot men size is {}",
                                          bytes_string(tex_slot_mem_size + buffer_slot_mem_size)));
        });
    });
}

}// namespace vision
