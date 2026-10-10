#include "mechanics_internal.h"

#include <cstdio>

using namespace Corona::Systems::MechanicsInternal;

namespace {
int failed = 0;
int passed = 0;

void expect(bool condition, const char* message) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", message);
    condition ? ++passed : ++failed;
}

bool near(float a, float b) { return std::abs(a - b) < 1e-4f; }

CollisionMesh ground_mesh() {
    CollisionMesh mesh;
    mesh.vertices = {make_fvec3(-1, 0, -1), make_fvec3(-1, 0, 1),
                     make_fvec3(1, 0, 1), make_fvec3(1, 0, -1)};
    mesh.triangles = {{0, 1, 2}, {0, 2, 3}};
    mesh.total_vertex_count = 4;
    return mesh;
}

FootGroundInstance instance(const CollisionMesh& mesh, std::uintptr_t actor, const ktm::fvec3& position) {
    FootGroundInstance ground;
    ground.actor = actor;
    ground.model_id = 42; // Both instances intentionally share a resource identity.
    ground.mesh = mesh;
    for (const auto& vertex : mesh.vertices) ground.world_vertices.push_back(vec3_add(vertex, position));
    ground.octree.build(ground.world_vertices, ground.mesh, 1);
    return ground;
}

void test_instance_world_queries() {
    const auto mesh = ground_mesh();
    auto a = instance(mesh, 1, make_fvec3(100, 3, -50));
    auto b = instance(mesh, 2, make_fvec3(-200, 7, 80));
    float height = 0;
    ktm::fvec3 normal{};
    expect(a.octree.query_ground_height(a.world_vertices, a.mesh, make_fvec3(100, 3.2f, -50), 0.5f, height, &normal) &&
               near(height, 3) && near(normal.x, 0) && near(normal.y, 1) && near(normal.z, 0),
           "a translated instance can be probed on its first world-space build, with an upward unit normal");
    expect(b.octree.query_ground_height(b.world_vertices, b.mesh, make_fvec3(-200, 7.2f, 80), 0.5f, height) && near(height, 7),
           "another instance of the same mesh has its own ground height and world bounds");
    expect(!a.octree.query_ground_height(a.world_vertices, a.mesh, make_fvec3(-200, 7.2f, 80), 0.5f, height),
           "one instance cannot report the other shared-resource instance's surface");

    for (auto& vertex : a.world_vertices) vertex = vec3_add(vertex, make_fvec3(10, 1, 0));
    a.octree.refit(a.world_vertices, a.mesh);
    expect(a.octree.query_ground_height(a.world_vertices, a.mesh, make_fvec3(110, 4.2f, -50), 0.5f, height) && near(height, 4),
           "refitting a moved platform updates the queried world position and height");
    expect(!a.octree.query_ground_height(a.world_vertices, a.mesh, make_fvec3(100, 3.2f, -50), 0.5f, height),
           "a moved platform no longer returns a stale hit at its old location");
    expect(b.octree.query_ground_height(b.world_vertices, b.mesh, make_fvec3(-200, 7.2f, 80), 0.5f, height) && near(height, 7),
           "refitting one instance does not disturb another instance sharing its mesh");
}

void test_ground_shape_and_range() {
    auto mesh = ground_mesh();
    for (auto& vertex : mesh.vertices) vertex.y = vertex.x * 0.5f;
    auto ground = instance(mesh, 1, make_fvec3(10, 2, 30));
    float height = 0;
    ktm::fvec3 normal{};
    expect(ground.octree.query_ground_height(ground.world_vertices, ground.mesh, make_fvec3(10, 2.2f, 30), 0.5f, height, &normal) &&
               near(height, 2) && near(normal.x, -1.0f / std::sqrt(5.0f)) &&
               near(normal.y, 2.0f / std::sqrt(5.0f)) && near(normal.z, 0),
           "a sloped surface returns its actual plane height and normalized world-space normal");
    expect(!ground.octree.query_ground_height(ground.world_vertices, ground.mesh, make_fvec3(10, 3, 30), 0.5f, height),
           "ground below the configured probe reach is rejected");
    expect(!ground.octree.query_ground_height(ground.world_vertices, ground.mesh, make_fvec3(10, 1, 30), 0.5f, height),
           "a surface above the probe's allowed upward slop is rejected");
    expect(!ground.octree.query_ground_height(ground.world_vertices, ground.mesh, make_fvec3(12, 2.2f, 30), 0.5f, height),
           "a probe outside the triangle projection cannot hit its infinite plane");

    for (auto& triangle : mesh.triangles) std::swap(triangle[1], triangle[2]);
    ground = instance(mesh, 1, make_fvec3(10, 2, 30));
    expect(!ground.octree.query_ground_height(ground.world_vertices, ground.mesh, make_fvec3(10, 2.2f, 30), 0.5f, height),
           "downward-facing surfaces are excluded from foot support");
}
} // namespace

int main() {
    test_instance_world_queries();
    test_ground_shape_and_range();
    std::printf("IK ground queries: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
