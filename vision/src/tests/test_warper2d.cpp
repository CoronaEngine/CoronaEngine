#include "base/import/node_desc.h"
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"
#include "base/warper.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <vector>

using namespace vision;
using namespace ocarina;

namespace {

constexpr uint kSampleCount = 1u << 17u;
constexpr float kFreqTolerance = 1.5e-2f;
constexpr float kPdfTolerance = 2e-4f;
constexpr float kValueTolerance = 1e-5f;

int g_pass = 0;
int g_fail = 0;

class TestPipeline final : public Pipeline {
public:
    explicit TestPipeline(const PipelineDesc &desc)
        : Pipeline(desc) {}

    [[nodiscard]] string_view impl_type() const noexcept override { return "test_pipeline"; }
    [[nodiscard]] string_view category() const noexcept override { return "pipeline"; }

    void init_postprocessor(const DenoiserDesc &desc) override {}
    void render(double dt) noexcept override {}
};

struct SampleRecord {
    float2 p{};
    float pdf{};
    uint2 coord{};
};

void expect(bool cond, const char *message) {
    if (!cond) {
        std::cerr << "[test-warper2d] FAIL: " << message << std::endl;
        ++g_fail;
    } else {
        ++g_pass;
    }
}

void expect_nearly_equal(float lhs, float rhs, float eps, const char *message) {
    expect(std::abs(lhs - rhs) <= eps, message);
}

[[nodiscard]] size_t linear_index(uint2 res, uint2 coord) {
    return static_cast<size_t>(coord.y) * res.x + coord.x;
}

[[nodiscard]] float total_weight(const std::vector<float> &weights) {
    return std::accumulate(weights.begin(), weights.end(), 0.f);
}

[[nodiscard]] float expected_cell_probability(const std::vector<float> &weights,
                                              uint2 res,
                                              uint2 coord) {
    return weights[linear_index(res, coord)] / total_weight(weights);
}

[[nodiscard]] float expected_cell_pdf(const std::vector<float> &weights,
                                      uint2 res,
                                      uint2 coord) {
    return expected_cell_probability(weights, res, coord) * static_cast<float>(res.x * res.y);
}

[[nodiscard]] float2 cell_center(uint2 res, uint2 coord) {
    return make_float2((static_cast<float>(coord.x) + 0.5f) / static_cast<float>(res.x),
                       (static_cast<float>(coord.y) + 0.5f) / static_cast<float>(res.y));
}

[[nodiscard]] SP<Warper2D> create_alias_warper2d(Pipeline &pipeline,
                                                  const std::vector<float> &weights,
                                                  uint2 res) {
    WarperDesc desc("Warper");
    desc.sub_type = "alias2d";
    auto warper = Node::create_shared<Warper2D>(desc);
    warper->allocate(res);
    warper->build(weights, res);
    warper->upload_immediately();
    pipeline.upload_bindless_array();
    return warper;
}

[[nodiscard]] std::vector<float2> make_uniform_inputs(uint count) {
    std::mt19937 rng(42u);
    std::uniform_real_distribution<float> dist(0.f, std::nextafter(1.f, 0.f));
    std::vector<float2> ret(count);
    for (auto &value : ret) {
        value = make_float2(dist(rng), dist(rng));
    }
    return ret;
}

[[nodiscard]] std::vector<SampleRecord> sample_on_gpu(Device &device,
                                                      const SP<Warper2D> &warper,
                                                      const std::vector<float2> &inputs) {
    Stream stream = device.create_stream();
    Buffer<float2> input_buffer = device.create_buffer<float2>(static_cast<uint>(inputs.size()), "test_warper2d_inputs");
    Buffer<float2> point_buffer = device.create_buffer<float2>(static_cast<uint>(inputs.size()), "test_warper2d_points");
    Buffer<float> pdf_buffer = device.create_buffer<float>(static_cast<uint>(inputs.size()), "test_warper2d_pdfs");
    Buffer<uint2> coord_buffer = device.create_buffer<uint2>(static_cast<uint>(inputs.size()), "test_warper2d_coords");

    Kernel kernel = [&](BufferVar<float2> in,
                        BufferVar<float2> out_points,
                        BufferVar<float> out_pdfs,
                        BufferVar<uint2> out_coords) {
        Uint index = dispatch_id();
        Float pdf = 0.f;
        Uint2 coord = make_uint2(0u);
        Float2 p = warper->sample_continuous(in.read(index), std::addressof(pdf), std::addressof(coord));
        out_points.write(index, p);
        out_pdfs.write(index, pdf);
        out_coords.write(index, coord);
    };

    auto shader = device.compile(kernel, "test_warper2d_sampling");
    std::vector<float2> host_points(inputs.size(), make_float2(0.f));
    std::vector<float> host_pdfs(inputs.size(), 0.f);
    std::vector<uint2> host_coords(inputs.size(), make_uint2(0u));

    stream << input_buffer.upload(inputs.data());
    stream << shader(input_buffer, point_buffer, pdf_buffer, coord_buffer).dispatch(static_cast<uint>(inputs.size()));
    stream << point_buffer.download(host_points.data());
    stream << pdf_buffer.download(host_pdfs.data());
    stream << coord_buffer.download(host_coords.data());
    stream << synchronize() << commit();

    std::vector<SampleRecord> ret(inputs.size());
    for (size_t i = 0; i < ret.size(); ++i) {
        ret[i] = SampleRecord{host_points[i], host_pdfs[i], host_coords[i]};
    }
    return ret;
}

struct InspectRecord {
    float func_value{};
    float pdf_value{};
};

[[nodiscard]] std::vector<InspectRecord> inspect_on_gpu(Device &device,
                                                        const SP<Warper2D> &warper,
                                                        const std::vector<uint2> &coords,
                                                        const std::vector<float2> &points) {
    Stream stream = device.create_stream();
    const uint count = static_cast<uint>(coords.size());
    Buffer<uint2> coord_buffer = device.create_buffer<uint2>(count, "test_warper2d_inspect_coords");
    Buffer<float2> point_buffer = device.create_buffer<float2>(count, "test_warper2d_inspect_points");
    Buffer<float> func_buffer = device.create_buffer<float>(count, "test_warper2d_inspect_func");
    Buffer<float> pdf_buffer = device.create_buffer<float>(count, "test_warper2d_inspect_pdf");

    Kernel kernel = [&](BufferVar<uint2> in_coords,
                        BufferVar<float2> in_points,
                        BufferVar<float> out_func,
                        BufferVar<float> out_pdf) {
        Uint index = dispatch_id();
        Uint2 coord = in_coords.read(index);
        Float2 p = in_points.read(index);
        out_func.write(index, warper->func_at(coord));
        out_pdf.write(index, warper->PDF(p));
    };

    auto shader = device.compile(kernel, "test_warper2d_inspect");
    std::vector<float> host_func(count, 0.f);
    std::vector<float> host_pdf(count, 0.f);
    stream << coord_buffer.upload(coords.data());
    stream << point_buffer.upload(points.data());
    stream << shader(coord_buffer, point_buffer, func_buffer, pdf_buffer).dispatch(count);
    stream << func_buffer.download(host_func.data());
    stream << pdf_buffer.download(host_pdf.data());
    stream << synchronize() << commit();

    std::vector<InspectRecord> ret(count);
    for (uint i = 0; i < count; ++i) {
        ret[i] = InspectRecord{host_func[i], host_pdf[i]};
    }
    return ret;
}

void test_factory_creation(Pipeline &pipeline) {
    std::cout << "[test-warper2d] Running test_factory_creation ..." << std::endl;
    uint2 res = make_uint2(2u, 2u);
    std::vector<float> weights = {1.f, 2.f, 3.f, 4.f};
    auto warper = create_alias_warper2d(pipeline, weights, res);

    expect(warper != nullptr, "factory should create a Warper2D instance");
    expect(warper->category() == "warper", "created object should report warper category");
    expect(warper->impl_type() == "alias2d", "created object should resolve to alias2d subclass");
}

void test_cpu_contract(Device &device, Pipeline &pipeline) {
    std::cout << "[test-warper2d] Running test_cpu_contract ..." << std::endl;
    uint2 res = make_uint2(2u, 2u);
    std::vector<float> weights = {1.f, 3.f, 2.f, 6.f};
    auto warper = create_alias_warper2d(pipeline, weights, res);
    std::vector<uint2> coords;
    std::vector<float2> points;
    coords.reserve(res.x * res.y + 1u);
    points.reserve(res.x * res.y + 1u);

    expect_nearly_equal(warper->integral().hv(),
                        total_weight(weights) / static_cast<float>(res.x * res.y),
                        kValueTolerance,
                        "integral should match the average weight over the full 2D domain");

    for (uint y = 0; y < res.y; ++y) {
        for (uint x = 0; x < res.x; ++x) {
            uint2 coord = make_uint2(x, y);
            coords.emplace_back(coord);
            points.emplace_back(cell_center(res, coord));
        }
    }
    coords.emplace_back(make_uint2(res.x - 1u, res.y - 1u));
    points.emplace_back(make_float2(0.99999f, 0.99999f));

    auto inspected = inspect_on_gpu(device, warper, coords, points);

    for (uint y = 0; y < res.y; ++y) {
        for (uint x = 0; x < res.x; ++x) {
            size_t idx = linear_index(res, make_uint2(x, y));
            uint2 coord = make_uint2(x, y);
            expect_nearly_equal(inspected[idx].func_value,
                                weights[idx],
                                kValueTolerance,
                                "func_at should return the original cell weight");
            expect_nearly_equal(inspected[idx].pdf_value,
                                expected_cell_pdf(weights, res, coord),
                                kValueTolerance,
                                "PDF at cell center should equal the expected cell density");
        }
    }

    expect_nearly_equal(inspected.back().pdf_value,
                        expected_cell_pdf(weights, res, make_uint2(res.x - 1u, res.y - 1u)),
                        kValueTolerance,
                        "PDF should clamp points near 1 to the last cell");
}

void test_sampling_distribution(Device &device, Pipeline &pipeline) {
    std::cout << "[test-warper2d] Running test_sampling_distribution ..." << std::endl;
    uint2 res = make_uint2(3u, 2u);
    std::vector<float> weights = {
        0.f, 1.f, 3.f,
        2.f, 4.f, 0.f
    };
    auto warper = create_alias_warper2d(pipeline, weights, res);
    auto inputs = make_uniform_inputs(kSampleCount);
    auto samples = sample_on_gpu(device, warper, inputs);
    std::vector<uint> counts(weights.size(), 0u);

    bool coords_valid = true;
    bool points_valid = true;
    bool pdf_valid = true;

    for (const auto &sample : samples) {
        if (sample.coord.x >= res.x || sample.coord.y >= res.y) {
            coords_valid = false;
            continue;
        }
        size_t idx = linear_index(res, sample.coord);
        ++counts[idx];

        float cell_min_x = static_cast<float>(sample.coord.x) / static_cast<float>(res.x);
        float cell_max_x = static_cast<float>(sample.coord.x + 1u) / static_cast<float>(res.x);
        float cell_min_y = static_cast<float>(sample.coord.y) / static_cast<float>(res.y);
        float cell_max_y = static_cast<float>(sample.coord.y + 1u) / static_cast<float>(res.y);
        if (!(sample.p.x >= cell_min_x && sample.p.x < cell_max_x &&
              sample.p.y >= cell_min_y && sample.p.y < cell_max_y)) {
            points_valid = false;
        }

        float expected_pdf = expected_cell_pdf(weights, res, sample.coord);
        if (std::abs(sample.pdf - expected_pdf) > kPdfTolerance) {
            pdf_valid = false;
        }
    }

    expect(coords_valid, "sampled coordinates should stay within the grid");
    expect(points_valid, "sample_continuous should return a point inside the sampled cell");
    expect(pdf_valid, "sample_continuous should report the cell density as pdf");

    for (uint y = 0; y < res.y; ++y) {
        for (uint x = 0; x < res.x; ++x) {
            uint2 coord = make_uint2(x, y);
            size_t idx = linear_index(res, coord);
            float empirical = static_cast<float>(counts[idx]) / static_cast<float>(samples.size());
            float expected = expected_cell_probability(weights, res, coord);
            std::string message = "sample frequency should match the target cell probability at (" +
                                  std::to_string(x) + ", " + std::to_string(y) + ")";
            expect(std::abs(empirical - expected) <= kFreqTolerance, message.c_str());
        }
    }
}

}// namespace

int main(int argc, char *argv[]) {
    auto device = RHIContext::instance().create_device();
    Global::instance().set_device(&device);

    PipelineDesc desc;
    auto pipeline = make_shared<TestPipeline>(desc);
    Global::instance().set_pipeline(pipeline);

    test_factory_creation(*pipeline);
    test_cpu_contract(device, *pipeline);
    test_sampling_distribution(device, *pipeline);

    std::cout << "[test-warper2d] Passed " << g_pass << " checks, failed " << g_fail << std::endl;
    return g_fail == 0 ? 0 : 1;
}