// ActorWorld（ECS E1）的行为契约测试。
//
// 覆盖点：
//  1. ensure_actor 幂等，且补齐身份/变换/可见性/包围盒/驻留组件
//  2. scene 实体 + SceneMember 关系，actors_in_scene 按 order 排序
//  3. 换场景时旧关系被移除
//  4. profile 子实体：ProfileOf / Parent / 父实体 Children
//  5. destroy_actor 级联销毁 profile、清场景成员、解绑句柄
//  6. 句柄 ↔ 实体 双向映射

#include "corona/ecs/actor_world.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* expression, int line, const std::string& detail = {}) {
    if (condition) return;
    ++g_failures;
    std::cerr << "FAIL(line " << line << "): " << expression;
    if (!detail.empty()) std::cerr << " [" << detail << ']';
    std::cerr << '\n';
}

#define CHECK(expr) check((expr), #expr, __LINE__)
#define CHECK_MSG(expr, detail) check((expr), #expr, __LINE__, (detail))

using Corona::Ecs::ActorWorld;
using Corona::Ecs::ActorTag;
using Corona::Ecs::Bounds;
using Corona::Ecs::Children;
using Corona::Ecs::Entity;
using Corona::Ecs::kNullEntity;
using Corona::Ecs::Parent;
using Corona::Ecs::ProfileOf;
using Corona::Ecs::ResidencyState;
using Corona::Ecs::SceneMember;
using Corona::Ecs::SceneTag;
using Corona::Ecs::Transform;
using Corona::Ecs::Visibility;
using Corona::Ecs::World;
using Corona::EntityId;

void test_ensure_actor_is_idempotent() {
    auto& actors = ActorWorld::instance();
    auto& world = World::instance();

    const Entity first = actors.ensure_actor(EntityId{9001});
    const Entity second = actors.ensure_actor(EntityId{9001});
    CHECK(first != kNullEntity);
    CHECK(first == second);

    CHECK(world.try_get<ActorTag>(first) != nullptr);
    CHECK(world.try_get<Transform>(first) != nullptr);
    CHECK(world.try_get<Visibility>(first) != nullptr);
    CHECK(world.try_get<Bounds>(first) != nullptr);
    CHECK(world.try_get<ResidencyState>(first) != nullptr);
    CHECK(world.get<ActorTag>(first).actor_id == 9001);
    CHECK(world.get<Transform>(first).scale.x == 1.0f);

    CHECK(actors.find(EntityId{9001}) == first);
    CHECK(actors.handle_of(first) == EntityId{9001});

    actors.destroy_actor(first);
    CHECK(actors.find(EntityId{9001}) == kNullEntity);
    CHECK(!world.alive(first));
}

void test_scene_membership_and_order() {
    auto& actors = ActorWorld::instance();
    auto& world = World::instance();

    const Entity scene = actors.ensure_scene(EntityId{8001});
    CHECK(scene != kNullEntity);
    CHECK(world.try_get<SceneTag>(scene) != nullptr);

    const Entity a = actors.ensure_actor(EntityId{9101});
    const Entity b = actors.ensure_actor(EntityId{9102});
    const Entity c = actors.ensure_actor(EntityId{9103});

    actors.attach_to_scene(c, scene, 3);
    actors.attach_to_scene(a, scene, 1);
    actors.attach_to_scene(b, scene, 2);

    const auto* member = world.try_get<SceneMember>(a);
    CHECK(member != nullptr);
    CHECK(member->scene == scene);
    CHECK(member->order == 1);

    const auto ordered = actors.actors_in_scene(scene);
    CHECK(ordered.size() == 3);
    CHECK(ordered.size() == 3 && ordered[0] == a && ordered[1] == b && ordered[2] == c);

    // 换场景：旧关系移除，新场景生效
    const Entity other = actors.ensure_scene(EntityId{8002});
    actors.attach_to_scene(a, other, 7);
    CHECK(world.get<SceneMember>(a).scene == other);
    const auto old_members = actors.actors_in_scene(scene);
    CHECK(old_members.size() == 2);
    CHECK(actors.actors_in_scene(other).size() == 1);

    actors.destroy_actor(a);
    actors.destroy_actor(b);
    actors.destroy_actor(c);
    world.destroy(scene);
    world.destroy(other);
}

void test_profile_children() {
    auto& actors = ActorWorld::instance();
    auto& world = World::instance();

    const Entity actor = actors.ensure_actor(EntityId{9201});
    const Entity profile = actors.create_profile(actor, EntityId{9202});
    CHECK(profile != kNullEntity);
    CHECK(world.get<ProfileOf>(profile).actor == actor);
    CHECK(world.get<Parent>(profile).value == actor);

    const auto* children = world.try_get<Children>(actor);
    CHECK(children != nullptr);
    CHECK(children != nullptr && children->value.size() == 1);

    // 级联销毁：profile 随 actor 一起消失
    actors.destroy_actor(actor);
    CHECK(!world.alive(profile));
    CHECK(!world.alive(actor));
    CHECK(actors.find(EntityId{9202}) == kNullEntity);
}

void test_owner_thread_binding() {
    auto& actors = ActorWorld::instance();
    actors.bind_owner_thread();
    CHECK(actors.is_owner_thread());
}

}  // namespace

int main() {
    test_ensure_actor_is_idempotent();
    test_scene_membership_and_order();
    test_profile_children();
    test_owner_thread_binding();

    if (g_failures != 0) {
        std::cerr << g_failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "ActorWorldTests: all checks passed\n";
    return 0;
}
