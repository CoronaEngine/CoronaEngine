// EnTT 世界骨架的契约测试：创建/附加/查询/视图/销毁，以及句柄宽度假设。
#include <corona/ecs/world.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failed = 0;

void expect(bool condition, const char* message) {
    if (condition) {
        return;
    }
    std::cerr << "FAIL: " << message << '\n';
    ++g_failed;
}

struct Position {
    float x{0.0f};
    float y{0.0f};
};

struct Velocity {
    float dx{0.0f};
    float dy{0.0f};
};

struct Name {
    std::string value;
};

}  // namespace

int main() {
    using Corona::Ecs::Entity;
    using Corona::Ecs::World;

    // 实体/句柄宽度：ENTT_ID_TYPE=uint64_t 已由 corona_compile_config.cmake 设定，
    // 与迁移前 uintptr_t 句柄同宽，替换时不会改变结构体布局。
    static_assert(sizeof(Entity) == sizeof(std::uint64_t),
                  "ENTT_ID_TYPE 必须保持 64 位，否则 EntityId 迁移会改变布局");

    auto& world = World::instance();
    expect(world.size() == 0, "新世界应为空");

    // 带组件创建
    const Entity a = world.create<Position, Velocity>();
    world.get<Velocity>(a).dx = 3.0f;
    const Entity b = world.create<Position>();
    world.get<Position>(b).x = 7.0f;

    expect(world.size() == 2, "应有两个实体");
    expect(world.alive(a) && world.alive(b), "两个实体都应存活");
    expect(world.try_get<Velocity>(b) == nullptr, "b 没有 Velocity");
    expect(world.try_get<Position>(a) != nullptr, "a 应有 Position");

    // 视图只遍历同时具备两个组件的实体
    std::size_t both = 0;
    for (const auto entity : world.view<Position, Velocity>()) {
        ++both;
        expect(entity == a, "同时具备 Position/Velocity 的应只有 a");
    }
    expect(both == 1, "视图应只命中 1 个实体");

    // 运行期附加组件
    world.emplace<Name>(b, Name{"bee"});
    expect(world.try_get<Name>(b) != nullptr, "运行期附加的组件可查");
    expect(world.get<Name>(b).value == "bee", "附加的组件值正确");

    // 销毁
    world.destroy(b);
    expect(!world.alive(b), "销毁后实体失效");
    expect(world.alive(a), "销毁 b 不影响 a");
    expect(world.size() == 1, "销毁后只剩 1 个实体");

    // 重复销毁是安全的（幂等）
    world.destroy(b);
    expect(world.size() == 1, "重复销毁不应改变实体数");

    // 版本号：同一索引被复用后，旧句柄必须失效
    const Entity c = world.create<Position>();
    expect(c != b, "复用槽位的新实体句柄必须与旧句柄不同");

    world.destroy(c);
    world.destroy(a);

    if (g_failed == 0) {
        std::cout << "EntityWorldTests: all checks passed\n";
        return 0;
    }
    std::cerr << "EntityWorldTests: " << g_failed << " failure(s)\n";
    return 1;
}
