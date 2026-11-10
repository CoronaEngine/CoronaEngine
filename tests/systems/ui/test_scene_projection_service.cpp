// scene 投影规则（WNC-013）：纯 DTO，无 CEF/引擎依赖。
#include "editor_api/editor_api_services.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

using namespace Corona::Systems::UI;
using namespace Corona::Systems::UI::SceneProjectionService;

namespace {

int checks = 0;

void check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

ActorIdentity identity(const char* guid, const char* name, const char* extension, const char* type) {
    return ActorIdentity{guid, name, extension, type};
}

void test_display_type() {
    check(display_type(identity("g", "Hero", "fbx", "model")) == "fbx",
          "route extension wins over the logical type");
    check(display_type(identity("g", "Sfx", "", "audio")) == "audio",
          "missing extension falls back to the logical type");
    check(display_type(identity("g", "Cam", "", "")) == "", "empty inputs stay empty");
}

void test_index_projection() {
    const std::vector<ActorIdentity> actors{
        identity("g1", "Hero", "fbx", "model"),
        identity("g2", "Bgm", "", "audio"),
    };
    const auto rows = project_index(actors);
    check(rows.size() == 2, "one index row per actor");
    check(rows[0].actor_guid == "g1" && rows[0].name == "Hero" && rows[0].type == "fbx",
          "head row keeps identity and projected type");
    check(rows[1].actor_guid == "g2" && rows[1].name == "Bgm" && rows[1].type == "audio",
          "tail row keeps logical type");
    check(project_index({}).empty(), "empty scene yields empty index");
}

void test_semantic_role() {
    check(semantic_role("", "camera") == "camera", "unset role falls back to the entity type");
    check(semantic_role("hero", "actor") == "hero", "explicit role wins");
    check(semantic_role("", "") == "", "both empty stays empty");
}

void test_physics_enabled() {
    check(!physics_enabled("phantom"), "phantom bodies have no physics");
    for (const char* body : {"dynamic", "kinematic", "static", ""}) {
        check(physics_enabled(body), "non-phantom bodies have physics");
    }
}

}  // namespace

int main() {
    test_display_type();
    test_index_projection();
    test_semantic_role();
    test_physics_enabled();
    std::cout << "scene_projection_service: " << checks << " checks passed\n";
    return 0;
}
