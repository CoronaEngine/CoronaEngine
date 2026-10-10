#include "cef/ik_chain_config.h"

#include <cstdlib>
#include <iostream>
#include <limits>

using namespace Corona;
using namespace Corona::Systems::UI;
using nlohmann::json;

namespace {
int checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

template <typename F>
void rejects(F&& operation, const char* message) {
    try { operation(); } catch (const std::invalid_argument&) { check(true, message); return; }
    check(false, message);
}

Resource::SkeletonData skeleton() {
    Resource::SkeletonData result;
    result.nodes.resize(4);
    result.nodes[0].name = "Root";
    result.nodes[0].parent = -1;
    result.nodes[1].name = "Knee";
    result.nodes[1].parent = 0;
    result.nodes[2].name = "Foot_L";
    result.nodes[2].parent = 1;
    result.nodes[3].name = "Foot_R";
    result.nodes[3].parent = 1;
    return result;
}
}  // namespace

int main() {
    const auto skel = skeleton();
    const json left = {{"id", "left"}, {"bone_name", "Foot_L"}, {"mode", "foot_plant"},
                       {"enabled", true}, {"weight", 0}, {"damping", 0}};
    auto parsed = IkChainConfig::parse(json::array({left}), &skel);
    check(parsed[0].end_node == 2 && parsed[0].end_bone_name == "Foot_L", "resource skeleton resolves before runtime setup");
    check(parsed[0].weight == 0 && parsed[0].damping == 0, "zero user values remain zero");
    check(parsed[0].enabled && !parsed[0].runtime.has_target && parsed[0].runtime.weight == 0, "enabled foot starts with no active target");
    check(!parsed[0].contact_driven && parsed[0].contact_weight_rise == 8, "Foot uses ground driver and defaults");
    auto contact = left;
    contact["mode"] = "contact";
    check(IkChainConfig::parse(json::array({contact}), &skel)[0].contact_driven, "Contact requests collision feedback");

    for (const json bad : {json{{"bone_name", "missing"}}, json{{"bone_name", "Root"}},
                           json{{"bone_name", "Foot_L"}, {"mode", "unknown"}},
                           json{{"bone_name", "Foot_L"}, {"enabled", 1}},
                           json{{"bone_name", "Foot_L"}, {"weight", -0.1}},
                           json{{"bone_name", "Foot_L"}, {"damping", 1.1}},
                           json{{"bone_name", "Foot_L"}, {"chain_length", 2.5}},
                           json{{"bone_name", "Foot_L"}, {"target", {0, 1}}},
                           json{{"bone_name", "Foot_L"}, {"target", {0, "x", 0}}},
                           json{{"bone_name", "Foot_L"}, {"target", {0, 1e100, 0}}}}) {
        rejects([&] { IkChainConfig::parse(json::array({bad}), &skel); }, "invalid entry rejects configuration");
    }
    rejects([&] { IkChainConfig::parse(json::array({left, left}), &skel); }, "duplicate bone/mode rejected");
    auto right = left;
    right["bone_name"] = "Foot_R";
    rejects([&] { IkChainConfig::parse(json::array({left, right}), &skel); }, "duplicate id rejected");
    right["id"] = "right";
    auto current = IkChainConfig::parse(json::array({left, right}), &skel);
    current[0].runtime.has_target = true;
    current[0].runtime.weight = 0.75f;
    current[0].runtime.target = {1, 2, 3};
    auto reordered = IkChainConfig::parse(json::array({right, left}), &skel);
    IkChainConfig::preserve_runtime(reordered, current);
    check(reordered[1].runtime.has_target && reordered[1].runtime.weight == 0.75f &&
          !reordered[0].runtime.has_target, "runtime follows identity across reorder");
    auto disabled = left;
    disabled["enabled"] = false;
    auto reset = IkChainConfig::parse(json::array({disabled}), &skel);
    IkChainConfig::preserve_runtime(reset, current);
    check(!reset[0].runtime.has_target && reset[0].runtime.weight == 0, "disable clears runtime");
    auto retarget = left;
    retarget["target"] = {3, 2, 1};
    reset = IkChainConfig::parse(json::array({retarget}), &skel);
    IkChainConfig::preserve_runtime(reset, current);
    check(!reset[0].runtime.has_target, "retarget does not retain stale target");

    const auto saved = IkChainConfig::serialize(current, &skel);
    check(!saved[0].contains("runtime"), "persistence excludes transient runtime");
    auto pending = IkChainConfig::parse(saved);
    check(pending[0].end_node == -1 && pending[0].end_bone_name == "Foot_L", "saved names await async import");
    check(IkChainConfig::serialize(pending) == saved, "configuration roundtrips without a loaded model");
    auto unresolved_model_change = current;
    unresolved_model_change[0].end_node = 0;
    check(IkChainConfig::serialize(unresolved_model_change, &skel)[0]["bone_name"] == "Foot_L",
          "persisted bone name survives a stale node index during model replacement");
    const auto status = IkChainConfig::serialize(current, &skel, true);
    check(status[0]["weight"] == 0 && status[0]["runtime"]["weight"] == 0.75f, "readback separates config and runtime weights");
    const json escaped = json::array({{{"bone_name", "Foot 100%"}}});
    const auto ini = IkChainConfig::dump_ini(escaped);
    check(ini.find('%') == std::string::npos && json::parse(ini) == escaped, "INI interpolation cannot corrupt names");
    std::uint64_t revision = 7;
    check(!IkChainConfig::apply_snapshot(current, revision, IkChainConfig::parse(saved, &skel), &skel) &&
          revision == 7 && current[0].runtime.has_target, "identical remote snapshot retains revision and support");
    auto changed_snapshot = saved;
    changed_snapshot[0]["damping"] = 0.4;
    check(IkChainConfig::apply_snapshot(current, revision, IkChainConfig::parse(changed_snapshot, &skel), &skel) &&
          revision == 8 && current[0].runtime.has_target, "remote config change increments revision and preserves compatible runtime");
    check(IkChainConfig::apply_snapshot(current, revision, {}, &skel) && revision == 9 && current.empty(),
          "remote empty list clears all chains and invalidates in-flight runtime");
    std::cout << checks << " IK configuration checks passed\n";
}
