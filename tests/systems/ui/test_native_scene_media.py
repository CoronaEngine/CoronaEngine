"""Exercise production JSON helpers without building CEF or starting a GPU."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'src/systems/ui/cef/cef_editor_native_api_handlers.cpp'
TEMP = ROOT / '.workspace/tmp/volumetric-caustic-media-20261118/native-contract'


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 0
    for index in range(opening, len(source)):
        depth += (source[index] == '{') - (source[index] == '}')
        if depth == 0:
            return source[start:index + 1]
    raise AssertionError('Unbalanced production source')


def json_include():
    conan = ROOT / 'build/conan/examples/relwithdebinfo/generators'
    for data in conan.glob('nlohmann_json*-data.cmake'):
        match = re.search(r'set\(nlohmann_json_PACKAGE_FOLDER_\w+ "([^"]+)"\)',
                          data.read_text(encoding='utf-8'))
        if match and (Path(match[1]) / 'include/nlohmann/json.hpp').is_file():
            return Path(match[1]) / 'include'
    raise AssertionError('Native JSON test requires configured nlohmann headers')


class NativeSceneMediaTests(unittest.TestCase):
    def test_actual_native_validator_and_effective_pt_response(self):
        source = SOURCE.read_text(encoding='utf-8-sig')
        helpers = '\n'.join(function(source, signature) for signature in (
            'void validate_authored_medium_interface(',
            'std::unordered_set<std::string> validate_authored_media(',
            'std::string validate_native_pt_settings(',
            'nlohmann::json native_pt_settings_payload(',
        ))
        snapshots = '\n'.join(function(source, signature) for signature in (
            'nlohmann::json scene_to_json(', 'std::string get_editor_scene_snapshot_from_python('))
        runtime = (ROOT / 'src/engine/engine_runtime_api.cpp').read_text(encoding='utf-8-sig')
        media_setter = function(runtime, 'void Corona::API::Scene::set_authored_media(')
        program = r'''
#include <nlohmann/json.hpp>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <iostream>
#include <memory>
#include <optional>
#include <array>
#include <vector>
#include <algorithm>
#define CFW_LOG_ERROR(...)
namespace Corona {
struct SceneDevice { std::shared_ptr<const nlohmann::json> authored_media; std::size_t authored_media_revision{}; };
struct SceneStorage { SceneDevice value; SceneDevice* acquire_write(std::uintptr_t h) { return h ? &value : nullptr; } };
struct SharedDataHub {
    SceneStorage storage; static SharedDataHub& instance() { static SharedDataHub hub; return hub; }
    SceneStorage& scene_storage() { return storage; }
};
namespace API { struct Scene {
    std::uintptr_t handle_{1};
    void set_authored_media(std::shared_ptr<const nlohmann::json>);
}; }
}
__MEDIA_SETTER__
__HELPERS__
using nlohmann::json;
struct EngineCamera { bool get_requested_vision_stable_planes() { return false; } };
struct NativeEditorCamera { std::shared_ptr<EngineCamera> engine_camera; };
struct NativeEditorScene {
    std::vector<int> actors; std::vector<NativeEditorCamera> cameras; std::size_t active_camera_index{};
    std::string route="scene", name="source", archive_session="session", project_root="project";
    bool sun_enabled=false, floor_grid_enabled=false; std::array<float,3> sun_direction{};
    std::string terrain_path, terrain_type, script_path;
    std::shared_ptr<const json> authored_media;
} scene;
NativeEditorScene* ensure_native_editor_scene() { return &scene; }
std::string normalize_route(const std::string& s) { return s; }
NativeEditorScene* resolve_native_editor_scene_request(NativeEditorScene* p, const std::string&) { return p; }
json actor_to_json(const NativeEditorScene&, int) { return json::object(); }
json camera_to_json(const NativeEditorCamera&) { return json::object(); }
std::optional<std::array<float,6>> native_scene_world_aabb(const NativeEditorScene&) { return {}; }
json aabb_to_json(const std::array<float,6>& a) { return a; }
std::string path_to_utf8(const std::string& s) { return s; }
__SNAPSHOTS__
void require(bool good, const char* message) { if (!good) throw std::runtime_error(message); }
template<class F> void rejects(F f) {
    bool rejected = false; try { f(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "malformed media accepted");
}
int main() {
    json media = {
        {"schema", 1}, {"mediums", {{{"id", "fog"}, {"type", "homogeneous"},
            {"sigma_a", {0., 0., 0.}}, {"sigma_s", {1., 1., 1.}}, {"scale", .5}, {"g", 0.}}}},
        {"boundaries", {{{"id", "region"}, {"shape", "cube"}, {"inside", "fog"}, {"outside", nullptr},
            {"object_to_world", {2., 0., 0., 5., 0., 3., 0., -2., 0., 0., 4., 1., 0., 0., 0., 1.}}}}}
    };
    const auto original = media;
    auto ids = validate_authored_media(media);
    require(ids == std::unordered_set<std::string>{"fog"}, "medium identity lost");
    require(media == original, "authored values rescaled or changed");
    Corona::API::Scene api_scene;
    auto authored = std::make_shared<const json>(media);
    api_scene.set_authored_media(authored);
    auto& stored = Corona::SharedDataHub::instance().storage.value;
    require(stored.authored_media == authored && stored.authored_media_revision == 1, "Scene API did not store media");
    api_scene.set_authored_media(authored);
    require(stored.authored_media_revision == 1, "Unchanged media advanced revision");
    scene.authored_media = std::make_shared<const json>(media);
    require(scene_to_json(scene)["media"] == media, "live save dropped authored media");
    auto live = json::parse(get_editor_scene_snapshot_from_python("scene"));
    require(live["media"] == media && live["actor_count"] == 0, "snapshot lost media or exposed null actors");
    validate_authored_medium_interface({{"inside", "fog"}, {"outside", nullptr}}, ids);
    validate_authored_medium_interface({{"inside", nullptr}, {"outside", nullptr}}, {});
    rejects([&] { validate_authored_medium_interface({}, ids); });
    rejects([&] { validate_authored_medium_interface({{"inside", "missing"}, {"outside", nullptr}}, ids); });
    rejects([&] { validate_authored_medium_interface({{"inside", 3}, {"outside", nullptr}}, ids); });
    auto invalid = [&](auto change) { auto candidate = media; change(candidate);
        rejects([&] { validate_authored_media(candidate); }); };
    invalid([](auto& j) { j["schema"] = 2; });
    invalid([](auto& j) { j["mediums"][0]["type"] = "heterogeneous"; });
    invalid([](auto& j) { j["mediums"].push_back(j["mediums"][0]); });
    invalid([](auto& j) { j["mediums"][0]["sigma_a"] = {-1., 0., 0.}; });
    invalid([](auto& j) { j["mediums"][0]["sigma_s"] = {0., 0.}; });
    invalid([](auto& j) { j["mediums"][0]["sigma_s"][0] = std::numeric_limits<double>::infinity(); });
    invalid([](auto& j) { j["mediums"][0]["sigma_a"][0] = std::numeric_limits<double>::quiet_NaN(); });
    invalid([](auto& j) { j["mediums"][0]["scale"] = -1; });
    invalid([](auto& j) { j["mediums"][0]["scale"] = 1e300; });
    invalid([](auto& j) { j["mediums"][0]["g"] = 1.; });
    invalid([](auto& j) { j["mediums"][0]["g"] = -1.; });
    invalid([](auto& j) { j["mediums"][0]["g"] = 0.999999999; });
    invalid([](auto& j) { j["mediums"][0]["g"] = -0.999999999; });
    invalid([](auto& j) { j["mediums"][0]["sigma_a"][0] = 2e38;
                          j["mediums"][0]["sigma_s"][0] = 2e38; j["mediums"][0]["scale"] = 0.; });
    invalid([](auto& j) { j["mediums"][0]["sigma_a"][0] = 2e38;
                          j["mediums"][0]["sigma_s"][0] = 1e38; j["mediums"][0]["scale"] = 1.2; });
    invalid([](auto& j) { j["boundaries"][0]["inside"] = "missing"; });
    invalid([](auto& j) { j["boundaries"][0].erase("outside"); });
    invalid([](auto& j) { j["boundaries"][0]["shape"] = "sphere"; });
    invalid([](auto& j) { j["boundaries"].push_back(j["boundaries"][0]); });
    invalid([](auto& j) { j["boundaries"][0]["object_to_world"] = {1, 2}; });
    invalid([](auto& j) { j["boundaries"][0]["object_to_world"][12] = 1.; });
    invalid([](auto& j) { j["boundaries"][0]["object_to_world"][0] = 0.; });
    auto zero = media; zero["mediums"][0]["scale"] = 0.; validate_authored_media(zero);
    require(validate_native_pt_settings({{"participating_media", true}}).empty(), "PT media bool rejected");
    require(validate_native_pt_settings({{"medium_diagnostics", true}}).empty(), "PT diagnostic bool rejected");
    require(!validate_native_pt_settings({{"medium_diagnostics", 1}}).empty(), "PT diagnostic integer accepted");
    require(!validate_native_pt_settings({{"participating_media", 1}}).empty(), "PT media integer accepted");
    require(!validate_native_pt_settings({{"participating_media", "true"}}).empty(), "PT media text accepted");
    require(!validate_native_pt_settings({{"bad", false}}).empty(), "unknown PT option accepted");
    require(!validate_native_pt_settings({{"max_depth", 1.5}}).empty(), "fractional depth accepted");
    json effective = {{"max_depth", 6}, {"min_depth", 2}, {"rr_threshold", .1},
        {"participating_media", false}, {"imported_media_count", 1}, {"medium_boundary_count", 1},
        {"adaptive", {{"enabled", false}}}};
    json requested = {{"max_depth", 8}, {"participating_media", true}};
    auto payload = native_pt_settings_payload(effective, requested);
    require(payload["ready"] == true && payload["effective"] == effective, "effective feedback changed");
    require(payload["settings"]["participating_media"] == false, "request overwrote effective denial");
    require(payload["settings"]["imported_media_count"] == 1 &&
            payload["settings"]["medium_boundary_count"] == 1, "imported counts lost");
    require(payload["requested"] == requested, "requested status lost");
    auto pending = native_pt_settings_payload(json::object(), requested);
    require(pending["ready"] == false && !pending["effective"].contains("participating_media"),
            "pending response fabricated effective status");
    std::cout << "PASS native media schema, references, row-major transforms, numeric safety and PT feedback\n";
}
'''.replace('__HELPERS__', helpers).replace('__SNAPSHOTS__', snapshots).replace('__MEDIA_SETTER__', media_setter)
        compiler = shutil.which('g++') or shutil.which('clang++')
        self.assertIsNotNone(compiler, 'Native CPU test requires a C++ compiler')
        TEMP.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='json-', dir=TEMP) as directory:
            cpp, executable = Path(directory) / 'test.cpp', Path(directory) / 'test.exe'
            cpp.write_text(program, encoding='utf-8')
            built = subprocess.run([compiler, '-std=c++20', '-O0', '-I' + str(json_include()),
                                    str(cpp), '-o', str(executable)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            run = subprocess.run([str(executable)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertTrue(run.stdout.startswith('PASS '), run.stdout)
            print(run.stdout.strip())

    def test_storage_and_both_snapshot_paths_keep_authored_media(self):
        source = SOURCE.read_text(encoding='utf-8-sig')
        shared = (ROOT / 'include/corona/shared_data_hub.h').read_text(encoding='utf-8-sig')
        for declaration in ('std::shared_ptr<const nlohmann::json> authored_media;',
                            'std::size_t authored_media_revision{};',
                            'std::shared_ptr<const nlohmann::json> medium_interface;'):
            self.assertIn(declaration, shared)
        validator = function(source, 'void validate_archive_snapshot(')
        self.assertIn('validate_authored_media(', validator)
        self.assertIn('validate_authored_medium_interface(', validator)
        materializer = function(source, 'NativeEditorScene& materialize_scene_snapshot_into_state(')
        self.assertIn('scene.engine_scene->set_authored_media(scene.authored_media);', materializer)
        self.assertNotIn('scene.engine_scene->get_handle()', materializer)
        api = (ROOT / 'include/corona/engine/engine_runtime_api.h').read_text(encoding='utf-8-sig')
        self.assertIn('void set_authored_media(std::shared_ptr<const nlohmann::json> media);', api)
        apply_optics = function(source, 'void apply_native_actor_optics_state(')
        self.assertIn('write->medium_interface = item.medium_interface;', apply_optics)
        actor_snapshot = function(source, 'nlohmann::json actor_to_json(')
        self.assertIn('item["medium_interface"] = *actor.medium_interface;', actor_snapshot)
        for signature in ('nlohmann::json scene_to_json(',
                          'std::string get_editor_scene_snapshot_from_python('):
            snapshot = function(source, signature)
            self.assertIn('["media"]', snapshot)
            self.assertIn('authored_media', snapshot)
            self.assertIn('nlohmann::json snapshot =', snapshot)


if __name__ == '__main__':
    unittest.main()
