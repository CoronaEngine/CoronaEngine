#include "sampler_viewer_app.h"

#include "base/import/node_desc.h"
#include "base/sampler.h"
#include "window/window.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

using namespace vision;
using namespace ocarina;

namespace {

constexpr uint2 kWindowSize = make_uint2(1560u, 960u);
constexpr uint kDefaultSampleCount = 4096u;
constexpr uint kMaxSampleCount = 65536u;
constexpr uint kDefaultHeatmapResolution = 32u;
constexpr uint kCanvasMinSize = 480u;
constexpr uint kOuterMargin = 24u;
constexpr uint kControlPanelWidth = 340u;
constexpr int kMouseActionPress = 1;
constexpr int kMouseButtonLeft = 0;

struct SamplerEntry {
    string subtype;
    string label;
};

struct SamplePointInfo {
    float2 uv{make_float2(0.f)};
    float2 canvas_pos{make_float2(0.f)};
    uint cell_x{0u};
    uint cell_y{0u};
    uint flat_index{0u};
    bool valid{false};
};

struct CanvasRect {
    uint x0{0u};
    uint y0{0u};
    uint x1{0u};
    uint y1{0u};
};

struct ViewerState {
    int sampler_index{0};
    uint sample_count{kDefaultSampleCount};
    uint heatmap_resolution{kDefaultHeatmapResolution};
    uint point_radius{4u};
    uint hover_radius{14u};
    uint2 seed{make_uint2(17u, 29u)};
    bool color_by_index{true};
    bool show_grid{true};
    bool show_heatmap{true};
    bool show_magnifier{true};
    bool show_axes{true};
    bool show_labels{true};
    bool fit_square_canvas{true};
    bool regenerate_requested{true};
    int locked_index{-1};
    int hovered_index{-1};
    uint duplicate_cells{0u};
    uint invalid_samples{0u};
};

[[nodiscard]] uchar4 make_color(float r, float g, float b, float a = 1.f) {
    return make_uchar4(static_cast<uint8_t>(std::clamp(r, 0.f, 1.f) * 255.f),
                       static_cast<uint8_t>(std::clamp(g, 0.f, 1.f) * 255.f),
                       static_cast<uint8_t>(std::clamp(b, 0.f, 1.f) * 255.f),
                       static_cast<uint8_t>(std::clamp(a, 0.f, 1.f) * 255.f));
}

[[nodiscard]] bool point_in_canvas(float2 point, float2 canvas_min, float2 canvas_max) {
    return point.x >= canvas_min.x && point.y >= canvas_min.y && point.x <= canvas_max.x && point.y <= canvas_max.y;
}

[[nodiscard]] uchar4 sampler_color(uint index, uint count, bool color_by_index) {
    if (!color_by_index) {
        return make_color(0.18f, 0.82f, 1.0f);
    }
    float t = count > 1u ? static_cast<float>(index) / static_cast<float>(count - 1u) : 0.f;
    float3 cold = make_float3(0.16f, 0.72f, 1.0f);
    float3 warm = make_float3(1.0f, 0.45f, 0.16f);
    float3 rgb = lerp(cold, warm, t);
    return make_color(rgb.x, rgb.y, rgb.z);
}

[[nodiscard]] vector<SamplerEntry> discover_samplers() {
    vector<SamplerEntry> entries;
    std::regex pattern(R"(^vision-sampler-(.+)\.dll$)", std::regex::icase);
    for (const auto &entry : std::filesystem::directory_iterator(std::filesystem::current_path())) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::smatch match;
        string filename = entry.path().filename().string();
        if (!std::regex_match(filename, match, pattern) || match.size() < 2u) {
            continue;
        }
        string subtype = match[1].str();
        entries.emplace_back(SamplerEntry{subtype, subtype});
    }
    std::sort(entries.begin(), entries.end(), [](const SamplerEntry &lhs, const SamplerEntry &rhs) {
        return lhs.label < rhs.label;
    });
    if (entries.empty()) {
        entries.emplace_back(SamplerEntry{"independent", "independent"});
    }
    return entries;
}

[[nodiscard]] float2 sample_to_canvas(float2 uv, float2 canvas_min, float2 canvas_max) {
    float width = std::max(1.f, canvas_max.x - canvas_min.x);
    float height = std::max(1.f, canvas_max.y - canvas_min.y);
    return make_float2(canvas_min.x + uv.x * width,
                       canvas_max.y - uv.y * height);
}

[[nodiscard]] float2 canvas_to_uv(float2 pos, float2 canvas_min, float2 canvas_max) {
    float width = std::max(1.f, canvas_max.x - canvas_min.x);
    float height = std::max(1.f, canvas_max.y - canvas_min.y);
    return make_float2(std::clamp((pos.x - canvas_min.x) / width, 0.f, 1.f),
                       std::clamp((canvas_max.y - pos.y) / height, 0.f, 1.f));
}

void clear_image(vector<uchar4> &pixels, uchar4 color) {
    std::fill(pixels.begin(), pixels.end(), color);
}

void fill_rect(vector<uchar4> &pixels, uint2 size, uint x0, uint y0, uint x1, uint y1, uchar4 color) {
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    x1 = std::min(x1, size.x);
    y1 = std::min(y1, size.y);
    for (uint y = y0; y < y1; ++y) {
        for (uint x = x0; x < x1; ++x) {
            pixels[static_cast<size_t>(y) * size.x + x] = color;
        }
    }
}

void draw_rect_outline(vector<uchar4> &pixels, uint2 size, uint x0, uint y0, uint x1, uint y1, uchar4 color) {
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    fill_rect(pixels, size, x0, y0, x1, std::min(y0 + 1u, y1), color);
    fill_rect(pixels, size, x0, y1 > 0u ? y1 - 1u : y1, x1, y1, color);
    fill_rect(pixels, size, x0, y0, std::min(x0 + 1u, x1), y1, color);
    fill_rect(pixels, size, x1 > 0u ? x1 - 1u : x1, y0, x1, y1, color);
}

void draw_line(vector<uchar4> &pixels, uint2 size, int x0, int y0, int x1, int y1, uchar4 color) {
    int dx = std::abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        if (x0 >= 0 && y0 >= 0 && x0 < static_cast<int>(size.x) && y0 < static_cast<int>(size.y)) {
            pixels[static_cast<size_t>(y0) * size.x + static_cast<size_t>(x0)] = color;
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int twice = error * 2;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

void draw_circle(vector<uchar4> &pixels, uint2 size, int cx, int cy, int radius, uchar4 color) {
    int x = radius;
    int y = 0;
    int error = 1 - x;
    auto plot = [&](int px, int py) {
        if (px >= 0 && py >= 0 && px < static_cast<int>(size.x) && py < static_cast<int>(size.y)) {
            pixels[static_cast<size_t>(py) * size.x + static_cast<size_t>(px)] = color;
        }
    };
    while (x >= y) {
        plot(cx + x, cy + y);
        plot(cx + y, cy + x);
        plot(cx - y, cy + x);
        plot(cx - x, cy + y);
        plot(cx - x, cy - y);
        plot(cx - y, cy - x);
        plot(cx + y, cy - x);
        plot(cx + x, cy - y);
        ++y;
        if (error < 0) {
            error += 2 * y + 1;
        } else {
            --x;
            error += 2 * (y - x) + 1;
        }
    }
}

void draw_disk(vector<uchar4> &pixels, uint2 size, int cx, int cy, int radius, uchar4 color) {
    int min_x = std::max(0, cx - radius);
    int max_x = std::min(static_cast<int>(size.x) - 1, cx + radius);
    int min_y = std::max(0, cy - radius);
    int max_y = std::min(static_cast<int>(size.y) - 1, cy + radius);
    int r2 = radius * radius;
    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            int dx = x - cx;
            int dy = y - cy;
            if (dx * dx + dy * dy <= r2) {
                pixels[static_cast<size_t>(y) * size.x + static_cast<size_t>(x)] = color;
            }
        }
    }
}

}// namespace

namespace vision {

struct SamplerViewerApp::Impl {
    Device device{RHIContext::instance().create_device()};
    vector<SamplerEntry> samplers{discover_samplers()};
    ViewerState state{};
    vector<float2> samples{};
    vector<uint> heatmap_counts{};
    vector<const char *> sampler_labels{};
    vector<uchar4> background{};
    float2 cursor_pos{make_float2(-1.f)};
    bool left_click_pending{false};

    Impl() {
        sampler_labels.reserve(samplers.size());
        for (const SamplerEntry &entry : samplers) {
            sampler_labels.emplace_back(entry.label.c_str());
        }
    }

    void regenerate_samples() {
        const string &subtype = samplers[static_cast<size_t>(state.sampler_index)].subtype;
        SamplerDesc desc("sampler_viewer");
        desc.sub_type = subtype;
        TSampler sampler{desc};
        Kernel kernel = [sampler](BufferVar<float2> output, Uint seed_x, Uint seed_y) {
            Uint index = dispatch_id();
            sampler->load_data();
            sampler->set_seed(make_uint2(seed_x, seed_y), index, 0u);
            output.write(index, sampler->next_2d());
        };
        auto sample_shader = device.compile(kernel, format("sampler_viewer_{}", subtype));

        samples.assign(state.sample_count, make_float2(0.f));
        heatmap_counts.assign(static_cast<size_t>(state.heatmap_resolution) * state.heatmap_resolution, 0u);
        state.invalid_samples = 0u;
        state.duplicate_cells = 0u;
        state.hovered_index = -1;
        if (state.locked_index >= static_cast<int>(state.sample_count)) {
            state.locked_index = -1;
        }

        Stream stream = device.create_stream();
        Buffer<float2> sample_buffer = device.create_buffer<float2>(state.sample_count, "sampler_viewer_samples");
    stream << sample_shader(sample_buffer, state.seed.x, state.seed.y).dispatch(state.sample_count);
        stream << sample_buffer.download(samples.data());
        stream << synchronize() << commit();

        std::unordered_set<uint> occupied_cells;
        occupied_cells.reserve(samples.size());
        for (const float2 sample : samples) {
            if (!(sample.x >= 0.f && sample.x < 1.f && sample.y >= 0.f && sample.y < 1.f)) {
                ++state.invalid_samples;
                continue;
            }
            uint cell_x = std::min(static_cast<uint>(sample.x * static_cast<float>(state.heatmap_resolution)), state.heatmap_resolution - 1u);
            uint cell_y = std::min(static_cast<uint>(sample.y * static_cast<float>(state.heatmap_resolution)), state.heatmap_resolution - 1u);
            uint flat = cell_y * state.heatmap_resolution + cell_x;
            if (!occupied_cells.insert(flat).second) {
                ++state.duplicate_cells;
            }
            ++heatmap_counts[flat];
        }
        state.regenerate_requested = false;
    }

    void ensure_background(uint2 size) {
        size_t pixel_count = static_cast<size_t>(size.x) * size.y;
        if (background.size() != pixel_count) {
            background.assign(pixel_count, make_uchar4(9u, 11u, 15u, 255u));
        }
    }

    [[nodiscard]] CanvasRect compute_canvas_rect(uint2 window_size) const {
        CanvasRect rect;
        if (window_size.x <= kOuterMargin * 2u || window_size.y <= kOuterMargin * 2u) {
            return rect;
        }
        uint left = std::min(window_size.x - kOuterMargin, kControlPanelWidth + kOuterMargin * 2u);
        uint right_margin = kOuterMargin;
        uint top_margin = kOuterMargin;
        uint bottom_margin = kOuterMargin;
        uint avail_w = window_size.x > left + right_margin ? window_size.x - left - right_margin : 0u;
        uint avail_h = window_size.y > top_margin + bottom_margin ? window_size.y - top_margin - bottom_margin : 0u;
        if (avail_w < 32u || avail_h < 32u) {
            return rect;
        }
        uint canvas_w = avail_w;
        uint canvas_h = avail_h;
        if (state.fit_square_canvas) {
            uint side = std::min({avail_w, avail_h, std::max(kCanvasMinSize, std::min(avail_w, avail_h))});
            canvas_w = side;
            canvas_h = side;
        }
        rect.x0 = left + (avail_w - canvas_w) / 2u;
        rect.y0 = top_margin + (avail_h - canvas_h) / 2u;
        rect.x1 = rect.x0 + canvas_w;
        rect.y1 = rect.y0 + canvas_h;
        return rect;
    }

    [[nodiscard]] std::optional<SamplePointInfo> inspect_sample(int index, const CanvasRect &rect) const {
        return inspect_sample(index,
                              make_float2(static_cast<float>(rect.x0), static_cast<float>(rect.y0)),
                              make_float2(static_cast<float>(rect.x1), static_cast<float>(rect.y1)));
    }

    void update_hover_state(const CanvasRect &rect) {
        state.hovered_index = -1;
        if (rect.x1 <= rect.x0 || rect.y1 <= rect.y0) {
            left_click_pending = false;
            return;
        }
        float2 canvas_min = make_float2(static_cast<float>(rect.x0), static_cast<float>(rect.y0));
        float2 canvas_max = make_float2(static_cast<float>(rect.x1), static_cast<float>(rect.y1));
        if (!point_in_canvas(cursor_pos, canvas_min, canvas_max)) {
            left_click_pending = false;
            return;
        }
        float best_dist2 = std::numeric_limits<float>::max();
        float hover_radius2 = static_cast<float>(state.hover_radius * state.hover_radius);
        for (uint i = 0; i < samples.size(); ++i) {
            float2 uv = samples[i];
            if (!(uv.x >= 0.f && uv.x < 1.f && uv.y >= 0.f && uv.y < 1.f)) {
                continue;
            }
            float2 pos = sample_to_canvas(uv, canvas_min, canvas_max);
            float dx = cursor_pos.x - pos.x;
            float dy = cursor_pos.y - pos.y;
            float dist2 = dx * dx + dy * dy;
            if (dist2 <= hover_radius2 && dist2 < best_dist2) {
                best_dist2 = dist2;
                state.hovered_index = static_cast<int>(i);
            }
        }
        if (left_click_pending) {
            state.locked_index = state.hovered_index;
            left_click_pending = false;
        }
    }

    [[nodiscard]] std::optional<SamplePointInfo> inspect_sample(int index, float2 canvas_min, float2 canvas_max) const {
        if (index < 0 || index >= static_cast<int>(samples.size())) {
            return std::nullopt;
        }
        float2 uv = samples[static_cast<size_t>(index)];
        if (!(uv.x >= 0.f && uv.x < 1.f && uv.y >= 0.f && uv.y < 1.f)) {
            return std::nullopt;
        }
        uint cell_x = std::min(static_cast<uint>(uv.x * static_cast<float>(state.heatmap_resolution)), state.heatmap_resolution - 1u);
        uint cell_y = std::min(static_cast<uint>(uv.y * static_cast<float>(state.heatmap_resolution)), state.heatmap_resolution - 1u);
        return SamplePointInfo{uv, sample_to_canvas(uv, canvas_min, canvas_max), cell_x, cell_y, static_cast<uint>(index), true};
    }

    void render_controls(Widgets *widgets) {
        widgets->use_window("vision-sampler-viewer", [&] {
            state.regenerate_requested |= widgets->combo("sampler", &state.sampler_index, sampler_labels.data(), static_cast<int>(sampler_labels.size()));
            state.regenerate_requested |= widgets->input_uint_limit("sample count", &state.sample_count, 1u, kMaxSampleCount, 64u, 1024u);
            state.regenerate_requested |= widgets->input_uint2("seed", &state.seed);
            state.regenerate_requested |= widgets->slider_uint("heatmap cells", &state.heatmap_resolution, 4u, 128u);
            widgets->slider_uint("point radius", &state.point_radius, 1u, 8u);
            widgets->slider_uint("hover radius", &state.hover_radius, 4u, 36u);
            widgets->check_box("color by index", &state.color_by_index);
            widgets->check_box("show heatmap", &state.show_heatmap);
            widgets->check_box("show grid", &state.show_grid);
            widgets->check_box("show axes", &state.show_axes);
            widgets->check_box("show labels", &state.show_labels);
            widgets->check_box("show magnifier", &state.show_magnifier);
            widgets->check_box("square canvas", &state.fit_square_canvas);
            widgets->button_click("generate", [&] {
                state.regenerate_requested = true;
            });
            widgets->same_line();
            widgets->button_click("clear selection", [&] {
                state.locked_index = -1;
            });
            widgets->text("valid samples: %u", state.sample_count - state.invalid_samples);
            widgets->text("invalid samples: %u", state.invalid_samples);
            widgets->text("shared cells: %u", state.duplicate_cells);
            float2 mouse_uv = make_float2(-1.f);
            CanvasRect rect = compute_canvas_rect(widgets->window()->size());
            if (rect.x1 > rect.x0 && rect.y1 > rect.y0 &&
                point_in_canvas(cursor_pos,
                                make_float2(static_cast<float>(rect.x0), static_cast<float>(rect.y0)),
                                make_float2(static_cast<float>(rect.x1), static_cast<float>(rect.y1)))) {
                mouse_uv = canvas_to_uv(cursor_pos,
                                        make_float2(static_cast<float>(rect.x0), static_cast<float>(rect.y0)),
                                        make_float2(static_cast<float>(rect.x1), static_cast<float>(rect.y1)));
            }
            widgets->text("mouse uv: (%.5f, %.5f)", mouse_uv.x, mouse_uv.y);
            auto hovered = inspect_sample(state.hovered_index, rect);
            auto locked = inspect_sample(state.locked_index, rect);
            if (hovered) {
                widgets->text("hover: #%u uv=(%.6f, %.6f) cell=(%u, %u)", hovered->flat_index, hovered->uv.x, hovered->uv.y, hovered->cell_x, hovered->cell_y);
            } else {
                widgets->text("hover: none");
            }
            if (locked) {
                widgets->text("locked: #%u uv=(%.6f, %.6f) cell=(%u, %u)", locked->flat_index, locked->uv.x, locked->uv.y, locked->cell_x, locked->cell_y);
            } else {
                widgets->text("locked: none");
            }
            widgets->text("interaction: move mouse to inspect | left click lock point");
            widgets->text("canvas UV: origin bottom-left, Y up");
        });
    }

    void draw_heatmap(const CanvasRect &rect, uint2 size) {
        if (!state.show_heatmap || heatmap_counts.empty()) {
            return;
        }
        uint max_count = 0u;
        for (uint count : heatmap_counts) {
            max_count = std::max(max_count, count);
        }
        if (max_count == 0u) {
            return;
        }
        float cell_w = static_cast<float>(rect.x1 - rect.x0) / static_cast<float>(state.heatmap_resolution);
        float cell_h = static_cast<float>(rect.y1 - rect.y0) / static_cast<float>(state.heatmap_resolution);
        for (uint y = 0; y < state.heatmap_resolution; ++y) {
            for (uint x = 0; x < state.heatmap_resolution; ++x) {
                uint count = heatmap_counts[static_cast<size_t>(y) * state.heatmap_resolution + x];
                if (count == 0u) {
                    continue;
                }
                float t = std::clamp(static_cast<float>(count) / static_cast<float>(max_count), 0.f, 1.f);
                uchar4 color = make_color(0.09f + 0.60f * t, 0.12f + 0.26f * t, 0.18f + 0.05f * (1.f - t), 0.18f + 0.35f * t);
                uint x0 = rect.x0 + static_cast<uint>(static_cast<float>(x) * cell_w);
                uint y1 = rect.y1 - static_cast<uint>(static_cast<float>(y) * cell_h);
                uint x1 = rect.x0 + static_cast<uint>(static_cast<float>(x + 1u) * cell_w);
                uint y0 = rect.y1 - static_cast<uint>(static_cast<float>(y + 1u) * cell_h);
                fill_rect(background, size, x0, y0, x1, y1, color);
            }
        }
    }

    void draw_grid(const CanvasRect &rect, uint2 size) {
        if (!state.show_grid) {
            return;
        }
        constexpr int divisions = 10;
        uchar4 minor = make_uchar4(62u, 70u, 82u, 255u);
        uchar4 axis = make_uchar4(126u, 136u, 150u, 255u);
        for (int i = 0; i <= divisions; ++i) {
            float t = static_cast<float>(i) / static_cast<float>(divisions);
            int x = static_cast<int>(std::lerp(static_cast<float>(rect.x0), static_cast<float>(rect.x1), t));
            int y = static_cast<int>(std::lerp(static_cast<float>(rect.y0), static_cast<float>(rect.y1), t));
            draw_line(background, size, x, static_cast<int>(rect.y0), x, static_cast<int>(rect.y1), minor);
            draw_line(background, size, static_cast<int>(rect.x0), y, static_cast<int>(rect.x1), y, minor);
        }
        if (state.show_axes) {
            draw_rect_outline(background, size, rect.x0, rect.y0, rect.x1, rect.y1, axis);
        }
    }

    void draw_points(const CanvasRect &rect, uint2 size) {
        float2 canvas_min = make_float2(static_cast<float>(rect.x0), static_cast<float>(rect.y0));
        float2 canvas_max = make_float2(static_cast<float>(rect.x1), static_cast<float>(rect.y1));
        for (uint i = 0; i < samples.size(); ++i) {
            float2 uv = samples[i];
            if (!(uv.x >= 0.f && uv.x < 1.f && uv.y >= 0.f && uv.y < 1.f)) {
                continue;
            }
            float2 pos = sample_to_canvas(uv, canvas_min, canvas_max);
            uchar4 color = sampler_color(i, static_cast<uint>(samples.size()), state.color_by_index);
            draw_disk(background, size, static_cast<int>(pos.x), static_cast<int>(pos.y), static_cast<int>(state.point_radius), color);
        }

        int highlight_index = state.hovered_index >= 0 ? state.hovered_index : state.locked_index;
        auto selected = inspect_sample(highlight_index, rect);
        if (selected) {
            float2 pos = selected->canvas_pos;
            draw_circle(background, size, static_cast<int>(pos.x), static_cast<int>(pos.y), static_cast<int>(state.point_radius) + 4, make_uchar4(255u, 255u, 255u, 255u));
            draw_line(background, size, static_cast<int>(pos.x), static_cast<int>(rect.y0), static_cast<int>(pos.x), static_cast<int>(rect.y1), make_uchar4(255u, 255u, 255u, 255u));
            draw_line(background, size, static_cast<int>(rect.x0), static_cast<int>(pos.y), static_cast<int>(rect.x1), static_cast<int>(pos.y), make_uchar4(255u, 255u, 255u, 255u));
        }
    }

    void draw_magnifier(const CanvasRect &rect, uint2 size, const SamplePointInfo &focus) {
        if (!state.show_magnifier) {
            return;
        }
        uint panel_size = 200u;
        uint margin = 20u;
        if (size.x <= panel_size + margin * 2u || size.y <= panel_size + margin * 2u) {
            return;
        }
        uint box_x1 = size.x - margin;
        uint box_y1 = size.y - margin;
        uint box_x0 = box_x1 - panel_size;
        uint box_y0 = box_y1 - panel_size;
        fill_rect(background, size, box_x0, box_y0, box_x1, box_y1, make_uchar4(8u, 10u, 14u, 255u));
        draw_rect_outline(background, size, box_x0, box_y0, box_x1, box_y1, make_uchar4(104u, 112u, 124u, 255u));
        uint inset = 20u;
        uint view_x0 = box_x0 + inset;
        uint view_y0 = box_y0 + inset;
        uint view_x1 = box_x1 - inset;
        uint view_y1 = box_y1 - inset;
        fill_rect(background, size, view_x0, view_y0, view_x1, view_y1, make_uchar4(14u, 18u, 22u, 255u));
        draw_rect_outline(background, size, view_x0, view_y0, view_x1, view_y1, make_uchar4(86u, 94u, 106u, 255u));
        float2 canvas_min = make_float2(static_cast<float>(rect.x0), static_cast<float>(rect.y0));
        float2 canvas_max = make_float2(static_cast<float>(rect.x1), static_cast<float>(rect.y1));
        float scale = 8.f;
        float2 focus_pos = focus.canvas_pos;
        float2 view_center = make_float2(static_cast<float>(view_x0 + view_x1) * 0.5f, static_cast<float>(view_y0 + view_y1) * 0.5f);
        for (uint i = 0; i < samples.size(); ++i) {
            float2 uv = samples[i];
            if (!(uv.x >= 0.f && uv.x < 1.f && uv.y >= 0.f && uv.y < 1.f)) {
                continue;
            }
            float2 pos = sample_to_canvas(uv, canvas_min, canvas_max);
            float2 delta = pos - focus_pos;
            if (std::abs(delta.x) > 10.f || std::abs(delta.y) > 10.f) {
                continue;
            }
            int px = static_cast<int>(view_center.x + delta.x * scale);
            int py = static_cast<int>(view_center.y + delta.y * scale);
            draw_disk(background, size, px, py, static_cast<int>(state.point_radius) + 1, sampler_color(i, static_cast<uint>(samples.size()), state.color_by_index));
        }
        int cx = static_cast<int>((view_x0 + view_x1) / 2u);
        int cy = static_cast<int>((view_y0 + view_y1) / 2u);
        draw_line(background, size, cx, static_cast<int>(view_y0), cx, static_cast<int>(view_y1), make_uchar4(255u, 255u, 255u, 255u));
        draw_line(background, size, static_cast<int>(view_x0), cy, static_cast<int>(view_x1), cy, make_uchar4(255u, 255u, 255u, 255u));
    }

    void render_background(Window *window) {
        uint2 size = window->size();
        if (size.x == 0u || size.y == 0u) {
            return;
        }
        ensure_background(size);
        clear_image(background, make_uchar4(9u, 11u, 15u, 255u));
        fill_rect(background, size, 0u, 0u, std::min(size.x, kControlPanelWidth + kOuterMargin), size.y, make_uchar4(12u, 14u, 18u, 255u));

        CanvasRect rect = compute_canvas_rect(size);
        if (rect.x1 > rect.x0 && rect.y1 > rect.y0) {
            fill_rect(background, size, rect.x0, rect.y0, rect.x1, rect.y1, make_uchar4(16u, 20u, 25u, 255u));
            draw_rect_outline(background, size, rect.x0, rect.y0, rect.x1, rect.y1, make_uchar4(90u, 100u, 110u, 255u));
            update_hover_state(rect);
            draw_heatmap(rect, size);
            draw_grid(rect, size);
            draw_points(rect, size);
            auto focus = inspect_sample(state.hovered_index >= 0 ? state.hovered_index : state.locked_index, rect);
            if (focus) {
                draw_magnifier(rect, size, *focus);
            }
        } else {
            state.hovered_index = -1;
            left_click_pending = false;
        }

        window->set_background_visible(true);
        window->set_background(background.data(), size);
    }

    int run() {
        auto window = create_window("vision-sampler-viewer", kWindowSize, "imGui", true);
        window->init_widgets();
        window->set_clear_color(make_float4(9.f / 255.f, 11.f / 255.f, 15.f / 255.f, 1.f));
        window->set_window_size_callback([&](uint2) {
            left_click_pending = false;
        });
        window->set_cursor_position_callback([&](float2 pos) {
            cursor_pos = pos;
        });
        window->set_mouse_callback([&](int button, int action, float2 pos) {
            cursor_pos = pos;
            if (button == kMouseButtonLeft && action == kMouseActionPress) {
                left_click_pending = true;
            }
        });

        window->run([&](double) {
            if (state.regenerate_requested) {
                regenerate_samples();
            }
            render_background(window.get());
            render_controls(window->widgets());
        });
        return 0;
    }
};

SamplerViewerApp::SamplerViewerApp()
    : impl_(std::make_unique<Impl>()) {}

SamplerViewerApp::~SamplerViewerApp() = default;

int SamplerViewerApp::run() {
    return impl_->run();
}

}// namespace vision
