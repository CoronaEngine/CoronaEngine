#include "material_viewer_app.h"

#include "material_viewer_gl.h"

#include "material_viewer_plot.h"
#include "material_viewer_shared.h"

#include "base/color/spectrum.h"
#include "base/import/node_desc.h"
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"
#include "base/sampler.h"
#include "base/scattering/interaction.h"
#include "base/scattering/material.h"
#include "math/warp.h"
#include "window/window.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <vector>

using namespace vision;
using namespace ocarina;

namespace {

class TestPipeline final : public Pipeline {
public:
    explicit TestPipeline(const PipelineDesc &desc)
        : Pipeline(desc) {}

    [[nodiscard]] string_view impl_type() const noexcept override { return "test_pipeline"; }
    [[nodiscard]] string_view category() const noexcept override { return "pipeline"; }
    void init_postprocessor(const DenoiserDesc &) override {}
    void render(double) noexcept override {}
};

SampledWavelengths make_test_swl() {
    SampledWavelengths swl(3u, 1u);
    swl.set_lambda(0u, 602.785f);
    swl.set_lambda(1u, 539.285f);
    swl.set_lambda(2u, 445.772f);
    swl.set_pdf(0u, 1.f / 3.f);
    swl.set_pdf(1u, 1.f / 3.f);
    swl.set_pdf(2u, 1.f / 3.f);
    return swl;
}

PartialDerivative<Float3> make_identity_frame() {
    PartialDerivative<Float3> frame;
    frame.x = make_float3(1.f, 0.f, 0.f);
    frame.y = make_float3(0.f, 1.f, 0.f);
    frame.z = make_float3(0.f, 0.f, 1.f);
    return frame;
}

SP<Material> create_material_from_preset(MaterialKind kind) {
    MaterialDesc desc;
    desc.init(ParameterSet(DataWrap::parse(kMaterialPresets[static_cast<size_t>(kind)].json)));
    auto material = Material::create_root(desc);
    material->prepare();
    return material;
}

void install_srgb_spectrum(Pipeline &pipeline) {
    SpectrumDesc desc("Spectrum");
    desc.sub_type = "srgb";
    auto spectrum = Node::create_shared<Spectrum>(desc);
    pipeline.renderer().set_spectrum(spectrum);
    pipeline.renderer().spectrum()->prepare();
}

uint current_preview_samples(const ViewerState &state) {
    uint preset = kPreviewPresetSamples[state.preview_sample_preset];
    return preset == 0u ? std::max(1u, state.preview_sample_custom) : preset;
}

uint current_integral_samples(const ViewerState &state) {
    uint preset = kIntegralPresetSamples[state.integral_sample_preset];
    return preset == 0u ? std::max(1u, state.integral_sample_custom) : preset;
}

using MaterialSet = PolymorphicGUI<SP<Material>>;

std::array<uint, static_cast<size_t>(MaterialKind::Count)> make_material_ids(const MaterialSet &material_set,
                                                                             const std::array<SP<Material>, static_cast<size_t>(MaterialKind::Count)> &materials) {
    std::array<uint, static_cast<size_t>(MaterialKind::Count)> ids{};
    for (size_t i = 0; i < materials.size(); ++i) {
        ids[i] = material_set.encode_id(0u, materials[i].get());
    }
    return ids;
}

float3 direction_from_angles(float yaw_deg, float pitch_deg) {
    float yaw = yaw_deg * kRadPerDegree;
    float pitch = pitch_deg * kRadPerDegree;
    float cp = std::cos(pitch);
    float sp = std::sin(pitch);
    float sy = std::sin(yaw);
    float cy = std::cos(yaw);
    return normalize(make_float3(cp * sy, sp, cp * cy));
}

float3 display_to_local_direction(float3 display_direction) {
    return normalize(make_float3(display_direction.x, display_direction.z, display_direction.y));
}

bool point_in_rect(float2 point, const PlotRect &rect) {
    return point.x >= static_cast<float>(rect.x) && point.x <= static_cast<float>(rect.x + rect.width) &&
           point.y >= static_cast<float>(rect.y) && point.y <= static_cast<float>(rect.y + rect.height);
}

bool point_in_3d_plot(float2 point, uint2 window_size) {
    const PlotColumns columns = compute_plot_columns(window_size);
    PlotRect left = make_full_height_plot_rect(columns.left_x, columns.column_width, window_size);
    PlotRect right = make_full_height_plot_rect(columns.right_x, columns.column_width, window_size);
    return point_in_rect(point, left) || point_in_rect(point, right);
}

void reset_viewer_state(ViewerState &state) {
    state.quantity_mode = static_cast<int>(QuantityMode::FCos);
    state.scale_mode = static_cast<int>(ScaleMode::Shared);
    state.display_mode = static_cast<int>(DisplayMode::ThreeD);
    state.render_backend = static_cast<int>(RenderBackend::GL);
    state.preview_resolution = 1;
    state.preview_sample_preset = 2;
    state.integral_sample_preset = 3;
    state.preview_sample_custom = 16u;
    state.integral_sample_custom = 4096u;
    state.yaw_deg = 0.f;
    state.pitch_deg = 35.f;
    state.lobe_view_yaw_deg = -35.f;
    state.lobe_view_pitch_deg = 25.f;
    state.lobe_view_zoom = 1.15f;
    state.lobe_height_scale = 1.f;
    state.slice_plane_height = 0.f;
    state.show_slice_plane = true;
    state.log_scale = true;
    state.preview_dirty = true;
    state.background_dirty = true;
    state.integral_dirty = true;
    state.integral_ready = false;
    state.material_data_dirty = false;
    state.material_reset_requested = false;
}

void reset_current_material(Pipeline &pipeline,
                            ViewerState &state,
                            MaterialSet &material_set,
                            std::array<SP<Material>, static_cast<size_t>(MaterialKind::Count)> &materials,
                            std::array<uint, static_cast<size_t>(MaterialKind::Count)> &material_ids) {
    size_t material_index = static_cast<size_t>(state.current_material);
    auto kind = static_cast<MaterialKind>(state.current_material);
    auto material = create_material_from_preset(kind);
    material->set_index(static_cast<uint>(material_index));
    material_set.replace(static_cast<int>(material_index), material);
    materials[material_index] = material;
    pipeline.scene().prepare_materials();
    pipeline.upload_bindless_array();
    material_ids = make_material_ids(material_set, materials);
    material_set.reset_status();
    state.preview_dirty = true;
    state.integral_dirty = true;
    state.integral_ready = false;
    state.background_dirty = true;
    state.material_data_dirty = false;
    state.material_reset_requested = false;
}

template<typename PreviewShader>
PreviewData render_preview(Device &device, PreviewShader &shader, uint material_id, const ViewerState &state) {
    PreviewData data;
    data.resolution = kPreviewResolutions[state.preview_resolution];
    const uint pixel_count = data.resolution * data.resolution;
    data.reflection.resize(pixel_count, 0.f);
    data.transmission.resize(pixel_count, 0.f);

    Stream stream = device.create_stream();
    Buffer<float> reflection_buffer = device.create_buffer<float>(pixel_count, "material_lobe_reflection");
    Buffer<float> transmission_buffer = device.create_buffer<float>(pixel_count, "material_lobe_transmission");
    const float3 display_wo = direction_from_angles(state.yaw_deg, state.pitch_deg);
    const float3 local_wo = display_to_local_direction(display_wo);
    const uint preview_samples = current_preview_samples(state);
    const uint quantity_mode = static_cast<uint>(state.quantity_mode);

    stream << shader(reflection_buffer,
                     transmission_buffer,
                     material_id,
                     local_wo,
                     data.resolution,
                     preview_samples,
                     quantity_mode).dispatch(pixel_count);
    stream << reflection_buffer.download(data.reflection.data());
    stream << transmission_buffer.download(data.transmission.data());
    stream << synchronize() << commit();

    data.reflection_max = std::max(1e-5f, *std::max_element(data.reflection.begin(), data.reflection.end()));
    data.transmission_max = std::max(1e-5f, *std::max_element(data.transmission.begin(), data.transmission.end()));
    return data;
}

template<typename IntegralShader>
IntegralData compute_integrals(Device &device, IntegralShader &shader, uint material_id, const ViewerState &state) {
    IntegralData data;
    Stream stream = device.create_stream();
    Buffer<float3> integral_buffer = device.create_buffer<float3>(2u, "material_lobe_integrals");
    const float3 display_wo = direction_from_angles(state.yaw_deg, state.pitch_deg);
    const float3 local_wo = display_to_local_direction(display_wo);
    const uint sample_count = current_integral_samples(state);
    std::array<float3, 2> host = {make_float3(0.f), make_float3(0.f)};
    stream << shader(integral_buffer,
                     material_id,
                     local_wo,
                     sample_count).dispatch(1u);
    stream << integral_buffer.download(host.data());
    stream << synchronize() << commit();

    data.reflection = host[0];
    data.transmission = host[1];
    data.ready = true;
    return data;
}

void render_control_windows(Widgets *widgets,
                            ViewerState &state,
                            MaterialSet &material_set,
                            std::array<SP<Material>, static_cast<size_t>(MaterialKind::Count)> &materials,
                            std::array<uint, static_cast<size_t>(MaterialKind::Count)> &material_ids,
                            const IntegralData &integrals) {
    bool material_switched = false;
    widgets->use_window("material lobe viewer", [&] {
        std::array<const char *, static_cast<size_t>(MaterialKind::Count)> labels{};
        for (size_t i = 0; i < kMaterialPresets.size(); ++i) {
            labels[i] = kMaterialPresets[i].label;
        }
        material_switched |= widgets->combo("material type", &state.current_material, labels);
        widgets->text("summary: %s", kMaterialPresets[static_cast<size_t>(state.current_material)].summary);
        if (widgets->radio_button("3D view", state.display_mode == static_cast<int>(DisplayMode::ThreeD)) &&
            state.display_mode != static_cast<int>(DisplayMode::ThreeD)) {
            OC_INFO("vision-material-viewer switch to 3D requested.");
            state.display_mode = static_cast<int>(DisplayMode::ThreeD);
            state.background_dirty = true;
        }
        widgets->same_line();
        if (widgets->radio_button("2D view", state.display_mode == static_cast<int>(DisplayMode::TwoD)) &&
            state.display_mode != static_cast<int>(DisplayMode::TwoD)) {
            state.display_mode = static_cast<int>(DisplayMode::TwoD);
            state.background_dirty = true;
        }
        if (state.display_mode == static_cast<int>(DisplayMode::ThreeD)) {
            bool use_gl_backend = state.render_backend == static_cast<int>(RenderBackend::GL);
            if (widgets->radio_button("GL backend", use_gl_backend) && !use_gl_backend) {
                state.render_backend = static_cast<int>(RenderBackend::GL);
                state.background_dirty = true;
            }
            widgets->same_line();
            if (widgets->radio_button("CPU backend", !use_gl_backend) && use_gl_backend) {
                state.render_backend = static_cast<int>(RenderBackend::CPU);
                state.background_dirty = true;
            }
        }
        state.preview_dirty |= widgets->combo("quantity", &state.quantity_mode, kQuantityModeNames);
        state.preview_dirty |= widgets->combo("scale", &state.scale_mode, kScaleModeNames);
        state.preview_dirty |= widgets->combo("preview res", &state.preview_resolution, kPreviewResolutionNames);
        state.preview_dirty |= widgets->combo("preview spp", &state.preview_sample_preset, kPresetSampleNames);
        if (kPreviewPresetSamples[state.preview_sample_preset] == 0u) {
            state.preview_dirty |= widgets->drag_uint("preview spp custom", &state.preview_sample_custom, 1.f, 1u, 4096u);
        }
        state.integral_dirty |= widgets->combo("integral spp", &state.integral_sample_preset, kPresetSampleNames);
        if (kIntegralPresetSamples[state.integral_sample_preset] == 0u) {
            state.integral_dirty |= widgets->drag_uint("integral spp custom", &state.integral_sample_custom, 16.f, 1u, 1u << 20u);
        }
        state.background_dirty |= widgets->check_box("log scale", &state.log_scale);
        state.preview_dirty |= widgets->drag_float("yaw", &state.yaw_deg, 0.5f, -180.f, 180.f);
        state.preview_dirty |= widgets->drag_float("pitch", &state.pitch_deg, 0.5f, -89.f, 89.f);
        state.background_dirty |= widgets->drag_float("camera yaw", &state.lobe_view_yaw_deg, 0.5f, -180.f, 180.f);
        state.background_dirty |= widgets->drag_float("camera pitch", &state.lobe_view_pitch_deg, 0.5f, -89.f, 89.f);
        state.background_dirty |= widgets->drag_float("camera zoom", &state.lobe_view_zoom, 0.01f, 0.2f, 4.f);
        state.background_dirty |= widgets->drag_float("lobe height", &state.lobe_height_scale, 0.01f, 0.2f, 4.f);
        state.background_dirty |= widgets->drag_float("slice plane y", &state.slice_plane_height, 0.01f, -0.95f, 0.95f);
        state.background_dirty |= widgets->check_box("show slice plane", &state.show_slice_plane);
        float3 display_wo = direction_from_angles(state.yaw_deg, state.pitch_deg);
        float3 local_wo = display_to_local_direction(display_wo);
        widgets->text("wo(display y-up) = [%.3f, %.3f, %.3f]", display_wo.x, display_wo.y, display_wo.z);
        widgets->text("wo(local z-up) = [%.3f, %.3f, %.3f]", local_wo.x, local_wo.y, local_wo.z);
        widgets->button_click("reset material", [&] {
            state.preview_dirty = true;
            state.integral_dirty = true;
            state.integral_ready = false;
            state.background_dirty = true;
            state.material_data_dirty = false;
            state.material_reset_requested = true;
        });
        widgets->same_line();
        widgets->button_click("reset viewer", [&] {
            reset_viewer_state(state);
        });
        widgets->same_line();
        widgets->button_click("recompute integrals", [&] {
            state.integral_dirty = true;
        });
    });

    widgets->use_window("material params", [&] {
        auto &material = materials[static_cast<size_t>(state.current_material)];
        material->render_sub_UI(widgets);
        if (material->has_changed()) {
            state.preview_dirty = true;
            state.integral_dirty = true;
            state.integral_ready = false;
            state.background_dirty = true;
            state.material_data_dirty = true;
        }
    });

    widgets->use_window("integrals", [&] {
        float3 total = integrals.reflection + integrals.transmission;
        widgets->text("status: %s", state.integral_ready ? "fresh" : "stale");
        widgets->text("reflection = [%.4f, %.4f, %.4f]", integrals.reflection.x, integrals.reflection.y, integrals.reflection.z);
        widgets->text("transmission = [%.4f, %.4f, %.4f]", integrals.transmission.x, integrals.transmission.y, integrals.transmission.z);
        widgets->text("total = [%.4f, %.4f, %.4f]", total.x, total.y, total.z);
        widgets->text("preview spp = %u", current_preview_samples(state));
        widgets->text("integral spp = %u", current_integral_samples(state));
    });

    widgets->use_window("plot layout", [&] {
        widgets->text("left column: reflection");
        widgets->text("right column: transmission");
        if (state.display_mode == static_cast<int>(DisplayMode::TwoD)) {
            widgets->text("2D mode: heatmap / XZ slice / polar contour");
        } else {
            widgets->text("3D backend: %s", state.render_backend == static_cast<int>(RenderBackend::GL) ? "OpenGL" : "CPU");
            widgets->text("3D mode: orbit camera + movable XZ slice plane");
            widgets->text("slice y = %.3f", state.slice_plane_height);
            widgets->text("LMB drag: plane  |  RMB drag: orbit");
        }
    });

    if (material_switched) {
        state.preview_dirty = true;
        state.integral_dirty = true;
        state.integral_ready = false;
        state.background_dirty = true;
    }
}

}// namespace

namespace vision {

struct MaterialViewerApp::Impl {
    Device device;
    SP<TestPipeline> pipeline;
    TSampler sampler;
    std::array<SP<Material>, static_cast<size_t>(MaterialKind::Count)> materials{};
    std::array<uint, static_cast<size_t>(MaterialKind::Count)> material_ids{};
    ViewerState state{};
    PreviewData preview{};
    IntegralData integrals{};
    std::vector<uchar4> background;
    MaterialViewerGLRenderer gl_renderer;

    Impl()
        : device(RHIContext::instance().create_device()),
          background(static_cast<size_t>(kWindowSize.x) * kWindowSize.y, make_uchar4(0u, 0u, 0u, 255u)) {
        Global::instance().set_device(&device);

        PipelineDesc desc;
        pipeline = make_shared<TestPipeline>(desc);
        SamplerDesc sampler_desc{"independent"};
        sampler = Node::create_shared<Sampler>(sampler_desc);
        Global::instance().set_pipeline(pipeline);
        install_srgb_spectrum(*pipeline);

        auto &set = material_set();
        set.clear();
        set.set_mode(PolymorphicMode::ETopology);
        for (size_t i = 0; i < materials.size(); ++i) {
            materials[i] = create_material_from_preset(static_cast<MaterialKind>(i));
            pipeline->scene().add_material(materials[i]);
        }
        pipeline->scene().prepare_materials();
        pipeline->upload_bindless_array();
        material_ids = make_material_ids(set, materials);
    }

    MaterialSet &material_set() {
        return pipeline->scene().materials();
    }

    auto compile_preview_shader() {
        auto &set = material_set();
        TSampler preview_sampler = sampler;
        Kernel preview_kernel = [&](BufferVar<float> reflection_out,
                                    BufferVar<float> transmission_out,
                                    Uint material_id,
                                    Float3 wo,
                                    Uint resolution,
                                    Uint sample_num,
                                    Uint quantity) {
            Uint index = dispatch_id();
            SampledWavelengths swl = make_test_swl();
            auto frame = make_identity_frame();

            Interaction it(false);
            it.pos = make_float3(0.f);
            it.wo = wo;
            it.ng = frame.normal();
            it.ng_local = frame.normal();
            it.shading = frame;
            preview_sampler->load_data();

            set.dispatch(material_id, [&](const Material *material) {
                MaterialEvaluator evaluator = material->create_evaluator(it, swl);

                auto accumulate_side = [&](Float sign, Uint flag) {
                    Float total = 0.f;
                    Uint valid = 0u;
                    $for(sample_index, sample_num) {
                        Uint px = index % resolution;
                        Uint py = index / resolution;
                        preview_sampler->set_seed(make_uint2(px, py), sample_index, flag);
                        Float2 jitter = preview_sampler->next_2d();
                        Float2 uv = (make_float2(cast<float>(px), cast<float>(py)) + jitter) / cast<float>(resolution);
                        Float2 disk = square_to_disk(uv);
                        Float z = sqrt(max(0.f, 1.f - dot(disk, disk)));
                        Float3 wi = make_float3(disk.x, disk.y, sign * z);
                        Float cos_theta = abs_cos_theta(wi);
                        ScatterEval eval = evaluator.evaluate(wo, wi, MaterialEvalMode::F, flag, TransportMode::Radiance);
                        Float value = max(0.f, eval.f.average());
                        $if(quantity == 0u) {
                            value = select(cos_theta > 1e-6f, value / cos_theta, 0.f);
                        };
                        total += value;
                        valid += 1u;
                    };
                    return select(valid > 0u, total / cast<float>(valid), 0.f);
                };

                reflection_out.write(index, accumulate_side(1.f, BxDFFlag::Reflection));
                transmission_out.write(index, accumulate_side(-1.f, BxDFFlag::Transmission));
            });
        };
        return device.compile(preview_kernel, "material_viewer_preview");
    }

    auto compile_integral_shader() {
        auto &set = material_set();
        TSampler integral_sampler = sampler;
        Kernel integral_kernel = [&](BufferVar<float3> output,
                                     Uint material_id,
                                     Float3 wo,
                                     Uint sample_num) {
            SampledWavelengths swl = make_test_swl();
            auto frame = make_identity_frame();

            Interaction it(false);
            it.pos = make_float3(0.f);
            it.wo = wo;
            it.ng = frame.normal();
            it.ng_local = frame.normal();
            it.shading = frame;

            integral_sampler->load_data();

            set.dispatch(material_id, [&](const Material *material) {
                MaterialEvaluator evaluator = material->create_evaluator(it, swl);
                SampledSpectrum reflection = SampledSpectrum::zero(swl);
                SampledSpectrum transmission = SampledSpectrum::zero(swl);

                auto sample_cosine = [&](Float z_sign) {
                    Float3 wi = square_to_cosine_hemisphere(integral_sampler->next_2d());
                    return make_float3(wi.x, wi.y, z_sign * wi.z);
                };

                $for(i, sample_num) {
                    integral_sampler->set_seed(make_uint2(17u, 29u), i, 0u);
                    Float3 wi_reflection = sample_cosine(1.f);
                    Float pdf_reflection = abs_cos_theta(wi_reflection) * InvPi;
                    ScatterEval reflection_eval = evaluator.evaluate(wo, wi_reflection, MaterialEvalMode::F, BxDFFlag::Reflection, TransportMode::Radiance);
                    $if(pdf_reflection > 0.f) {
                        SampledSpectrum contribution = reflection_eval.f / pdf_reflection;
                        reflection += contribution;
                    };

                    integral_sampler->set_seed(make_uint2(17u, 29u), i, 1u);
                    Float3 wi_transmission = sample_cosine(-1.f);
                    Float pdf_transmission = abs_cos_theta(wi_transmission) * InvPi;
                    ScatterEval transmission_eval = evaluator.evaluate(wo, wi_transmission, MaterialEvalMode::F, BxDFFlag::Transmission, TransportMode::Radiance);
                    $if(pdf_transmission > 0.f) {
                        SampledSpectrum contribution = transmission_eval.f / pdf_transmission;;
                        transmission += contribution;
                    };
                };

                output.write(0u, (reflection / sample_num).vec3());
                output.write(1u, (transmission / sample_num).vec3());
            });
        };
        return device.compile(integral_kernel, "material_viewer_integral");
    }

    int run() {
        auto preview_shader = compile_preview_shader();
        auto integral_shader = compile_integral_shader();
        auto window = create_window("vision-material-viewer", kWindowSize, "imGui", true);
        window->set_clear_color(make_float4(10.f / 255.f, 12.f / 255.f, 16.f / 255.f, 1.f));
        window->set_window_size_callback([&](uint2) {
            state.background_dirty = true;
        });
        window->set_mouse_callback([&](int button, int action, float2 pos) {
            if (state.display_mode != static_cast<int>(DisplayMode::ThreeD)) {
                return;
            }
            bool in_plot = point_in_3d_plot(pos, window->size());
            if (action == 1 && in_plot) {
                state.last_cursor_x = pos.x;
                state.last_cursor_y = pos.y;
                if (button == 0) {
                    state.slice_plane_dragging = true;
                } else if (button == 1) {
                    state.camera_dragging = true;
                }
            } else if (action == 0) {
                state.slice_plane_dragging = false;
                state.camera_dragging = false;
            }
        });
        window->set_cursor_position_callback([&](float2 pos) {
            if (state.display_mode != static_cast<int>(DisplayMode::ThreeD)) {
                return;
            }
            float delta_x = pos.x - state.last_cursor_x;
            float delta_y = pos.y - state.last_cursor_y;
            state.last_cursor_x = pos.x;
            state.last_cursor_y = pos.y;
            if (state.slice_plane_dragging) {
                float panel_height = static_cast<float>(std::max(1u, window->size().y - 2u * kOuterMargin));
                state.slice_plane_height = std::clamp(state.slice_plane_height - delta_y * 2.f / panel_height, -0.95f, 0.95f);
                state.background_dirty = true;
            }
            if (state.camera_dragging) {
                state.lobe_view_yaw_deg = std::clamp(state.lobe_view_yaw_deg + delta_x * 0.35f, -180.f, 180.f);
                state.lobe_view_pitch_deg = std::clamp(state.lobe_view_pitch_deg - delta_y * 0.35f, -89.f, 89.f);
                state.background_dirty = true;
            }
        });
        window->set_scroll_callback([&](float2 delta) {
            if (state.display_mode != static_cast<int>(DisplayMode::ThreeD)) {
                return;
            }
            float2 cursor = make_float2(state.last_cursor_x, state.last_cursor_y);
            if (!point_in_3d_plot(cursor, window->size()) || delta.y == 0.f) {
                return;
            }
            float zoom_scale = std::pow(1.12f, delta.y);
            state.lobe_view_zoom = std::clamp(state.lobe_view_zoom * zoom_scale, 0.2f, 4.f);
            state.background_dirty = true;
        });
        window->set_render_callback([&] {
            if (state.display_mode == static_cast<int>(DisplayMode::ThreeD) &&
                state.render_backend == static_cast<int>(RenderBackend::GL)) {
                window->make_current();
                gl_renderer.initialize();
                gl_renderer.render();
            }
        });

        window->run([&](double) {
            render_control_windows(window->widgets(), state, material_set(), materials, material_ids, integrals);

            if (state.material_reset_requested) {
                reset_current_material(*pipeline, state, material_set(), materials, material_ids);
            }

            if (state.material_data_dirty) {
                material_set().upload_immediately();
                material_set().reset_status();
                state.material_data_dirty = false;
            }

            if (state.preview_dirty) {
                preview = render_preview(device, preview_shader, material_ids[static_cast<size_t>(state.current_material)], state);
                state.preview_dirty = false;
                state.background_dirty = true;
            }
            if (state.background_dirty) {
                draw_preview_background(background, window->size(), preview, state);
                if (state.display_mode == static_cast<int>(DisplayMode::ThreeD) &&
                    state.render_backend == static_cast<int>(RenderBackend::GL)) {
                    gl_renderer.update_meshes(preview, state, window->size());
                }
                state.background_dirty = false;
            }
            if (state.integral_dirty) {
                integrals = compute_integrals(device, integral_shader, material_ids[static_cast<size_t>(state.current_material)], state);
                state.integral_dirty = false;
                state.integral_ready = integrals.ready;
            }

            bool use_gl_3d = state.display_mode == static_cast<int>(DisplayMode::ThreeD) &&
                             state.render_backend == static_cast<int>(RenderBackend::GL) &&
                             gl_renderer.is_available();
            window->set_background_visible(!use_gl_3d);
            window->set_background(background.data(), window->size());
        });
        gl_renderer.shutdown();
        return 0;
    }
};

MaterialViewerApp::MaterialViewerApp()
    : impl_(std::make_unique<Impl>()) {}

MaterialViewerApp::~MaterialViewerApp() = default;

int MaterialViewerApp::run() {
    return impl_->run();
}

}// namespace vision