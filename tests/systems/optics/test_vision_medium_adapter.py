"""Execute the adapter's production conversion and binding with CPU backend types."""

from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "src/systems/optics/vision/vision_medium_adapter.cpp"
TEMP = ROOT / ".workspace/tmp/volumetric-caustic-media-20261118/import-adapter-cpu"


def function(source, signature):
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for index in range(opening, len(source)):
        depth += (source[index] == "{") - (source[index] == "}")
        if depth == 0:
            return source[start:index + 1]
    raise AssertionError("Unbalanced production adapter")


def json_include():
    for data in (ROOT / "build/conan/examples/relwithdebinfo/generators").glob("nlohmann_json*-data.cmake"):
        match = re.search(r'set\(nlohmann_json_PACKAGE_FOLDER_\w+ "([^"]+)"\)', data.read_text(encoding="utf-8"))
        if match and (Path(match[1]) / "include/nlohmann/json.hpp").is_file():
            return Path(match[1]) / "include"
    raise AssertionError("Configured nlohmann headers are required")


class VisionMediumAdapterTests(unittest.TestCase):
    def test_production_adapter_preserves_density_transform_and_explicit_interfaces(self):
        self.assertTrue(SOURCE.is_file(), "Vision medium adapter is missing")
        source = SOURCE.read_text(encoding="utf-8-sig")
        helpers = "\n".join(function(source, signature) for signature in (
            "nlohmann::json vision_medium_descriptor_json(",
            "::vision::float4x4 vision_medium_boundary_transform(",
            "std::shared_ptr<::vision::Medium> resolve_vision_medium(",
            "unsigned vision_medium_id(",
            "VisionMediaTelemetry configure_vision_media(",
            "void attach_vision_medium_interface(",
        ))
        program = r'''
#include <nlohmann/json.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
namespace vision {
constexpr unsigned InvalidUI32 = UINT32_MAX;
using DataWrap = nlohmann::json;
struct ParameterSet { nlohmann::json value; };
using float4x4 = std::array<std::array<float, 4>, 4>;
float4x4 make_float4x4(float diagonal) {
    float4x4 matrix{}; for (int i = 0; i < 4; ++i) matrix[i][i] = diagonal; return matrix;
}
struct MediumDesc {
    std::string name;
    nlohmann::json value;
    void init(ParameterSet parameter) { value = parameter.value; }
};
struct Medium {
    MediumDesc description;
    unsigned id;
    const auto& name() const { return description.name; }
    unsigned index() const { return id; }
};
struct MediumsDesc { std::vector<MediumDesc> mediums; bool process{}; std::string global; };
struct Mediums : std::vector<std::shared_ptr<Medium>> {
    unsigned encode_id(unsigned index, const Medium*) const { return 100 + index; }
};
struct GlobalMedium { std::string name; void reset() {} };
struct MediumRegistry {
    Mediums values;
    bool process{};
    GlobalMedium global;
    auto& elements() { return values; }
    auto& global_medium() { return global; }
    void set_process_mediums(bool enabled) { process = enabled; }
    bool process_mediums() const { return process && !values.empty(); }
};
struct MediumBoundary { float4x4 object_to_world; unsigned inside, outside; };
template<class T> using vector = std::vector<T>;
struct Geometry {
    std::vector<MediumBoundary> boundaries;
    bool process{};
    bool has_gpu_resource() const { return true; }
    void set_process_mediums(bool enabled) { process = enabled; }
    void set_medium_boundaries(const std::vector<MediumBoundary>& source) { boundaries = source; }
    std::size_t boundary_count() const { return boundaries.size(); }
};
struct Scene {
    MediumRegistry registry;
    Geometry geo;
    auto& medium_registry() { return registry; }
    auto& mediums() { return registry.values; }
    auto& geometry() { return geo; }
    bool process_mediums() const { return registry.process_mediums(); }
    void load_mediums(const MediumsDesc& description) {
        for (const auto& medium : description.mediums)
            registry.values.push_back(std::make_shared<Medium>(Medium{medium, unsigned(registry.values.size())}));
    }
};
struct ShapeInstance {
    std::shared_ptr<Medium> inside, outside;
    std::string inside_name, outside_name;
    unsigned inside_id{InvalidUI32}, outside_id{InvalidUI32};
    bool medium_transition{};
    void set_inside(std::shared_ptr<Medium> value) { inside = std::move(value); }
    void set_outside(std::shared_ptr<Medium> value) { outside = std::move(value); }
    void set_inside_name(const std::string& value) { inside_name = value; }
    void set_outside_name(const std::string& value) { outside_name = value; }
    void update_inside_medium_id(unsigned id) { inside_id = id; medium_transition = true; }
    void update_outside_medium_id(unsigned id) { outside_id = id; medium_transition = true; }
};
}
struct OpticsDevice { std::shared_ptr<const nlohmann::json> medium_interface; };
struct VisionMediaTelemetry { std::size_t medium_count{}, boundary_count{}; bool enabled{}; };
__HELPERS__
using nlohmann::json;
void require(bool good, const char* message) { if (!good) throw std::runtime_error(message); }
int main() {
    json media = {{"schema", 1}, {"mediums", {
        {{"id", "fog"}, {"type", "homogeneous"}, {"sigma_a", {0., .25, .5}},
         {"sigma_s", {1., 2., 3.}}, {"scale", .5}, {"g", -.2}},
        {{"id", "mist"}, {"type", "homogeneous"}, {"sigma_a", {0., 0., 0.}},
         {"sigma_s", {1., 1., 1.}}, {"scale", 2.}, {"g", 0.}}}},
        {"boundaries", {{{"id", "bounds"}, {"shape", "cube"}, {"inside", "fog"}, {"outside", "mist"},
          {"object_to_world", {2., 0., 0., 5., 0., 3., 0., -2., 0., 0., 4., -7., 0., 0., 0., 1.}}}}}};
    const auto original = media;
    vision::Scene pt;
    auto counts = configure_vision_media(pt, &media, true);
    require(counts.medium_count == 2 && counts.boundary_count == 1 && counts.enabled,
            "media-enabled scene did not load exactly two media and one boundary");
    require(pt.registry.process && pt.geo.process, "registry/transport policy disagrees");
    auto description = pt.mediums()[0]->description;
    require(description.name == "fog" && description.value["type"] == "homogeneous", "medium identity lost");
    require(description.value["param"]["sigma_a"] == json({0., .25, .5}) &&
            description.value["param"]["sigma_s"] == json({1., 2., 3.}) &&
            description.value["param"]["scale"] == .5 && description.value["param"]["g"] == -.2,
            "coefficients pre-scaled, density scale omitted, or phase changed");
    const auto& boundary = pt.geo.boundaries[0];
    require(boundary.inside == 100 && boundary.outside == 101, "boundary did not use encoded medium IDs");
    require(boundary.object_to_world[3][0] == 5 && boundary.object_to_world[3][1] == -2 &&
            boundary.object_to_world[3][2] == -7 && boundary.object_to_world[0][0] == 2 &&
            boundary.object_to_world[1][1] == 3 && boundary.object_to_world[2][2] == 4,
            "row-major source matrix transposed or Z-flipped incorrectly");
    require(media == original, "raw source media mutated");
    OpticsDevice source;
    vision::ShapeInstance inherited;
    attach_vision_medium_interface(pt, inherited, source);
    require(!inherited.medium_transition && !inherited.inside && !inherited.outside,
            "undeclared glass interface changed the incident medium");
    source.medium_interface = std::make_shared<json>(json{{"inside", "mist"}, {"outside", "fog"}});
    vision::ShapeInstance glass;
    attach_vision_medium_interface(pt, glass, source);
    require(glass.medium_transition && glass.inside_id == 101 && glass.outside_id == 100 &&
            glass.inside == pt.mediums()[1] && glass.outside == pt.mediums()[0], "explicit glass interface lost");
    source.medium_interface = std::make_shared<json>(json{{"inside", nullptr}, {"outside", nullptr}});
    vision::ShapeInstance vacuum;
    attach_vision_medium_interface(pt, vacuum, source);
    require(vacuum.medium_transition && vacuum.inside_id == vision::InvalidUI32 &&
            vacuum.outside_id == vision::InvalidUI32, "explicit vacuum confused with inherited medium");
    auto rt = vision::Scene{};
    auto disabled = configure_vision_media(rt, &media, false);
    require(!disabled.enabled && disabled.medium_count == 0 && disabled.boundary_count == 0 &&
            rt.mediums().empty() && rt.geo.boundaries.empty() && !rt.geo.process,
            "real-time scene acquired participating media");
    vision::ShapeInstance realtime_glass;
    attach_vision_medium_interface(rt, realtime_glass, source);
    require(!realtime_glass.medium_transition, "disabled scene bound an actor medium interface");
    require(pt.mediums().size() == 2 && pt.geo.boundaries.size() == 1,
            "independent real-time configuration contaminated PT scene");
    bool rejected = false;
    try { resolve_vision_medium(pt, "missing"); } catch (const std::exception&) { rejected = true; }
    require(rejected, "unknown medium reference silently became vacuum");
    std::cout << "PASS medium descriptors, single density scale, transformed boundary, encoded IDs, interfaces and isolation\n";
}
'''.replace("__HELPERS__", helpers)
        compiler = shutil.which("g++")
        self.assertIsNotNone(compiler, "A C++ compiler is required for adapter CPU tests")
        TEMP.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="import-adapter-", dir=TEMP) as directory:
            cpp, executable = Path(directory) / "adapter.cpp", Path(directory) / "adapter.exe"
            cpp.write_text(program, encoding="utf-8")
            result = subprocess.run([compiler, "-std=c++20", "-O1", "-I", str(json_include()),
                                     str(cpp), "-o", str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS medium descriptors", result.stdout)


if __name__ == "__main__":
    unittest.main()
