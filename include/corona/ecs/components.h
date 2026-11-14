#pragma once

/// 13 个 `Storage` 对应的组件类型清单。
///
/// 这张表是 S3（逐组件切换）的依据：每切换一个存储，就把对应的类型从这里
/// 的「待迁移」移到一个真正的 entt 组件（必要时拆分，见
/// docs/entt-full-replacement-analysis.md §4）。

#include <corona/shared_data_hub.h>

#include <cstddef>
#include <tuple>
#include <type_traits>

namespace Corona::Ecs {

/// 与 `SharedDataHub` 中 13 个存储（现已由 Kernel::Utils::EntityStorage 承载）一一对应的类型。
///
/// 注意：这里只是**类型清单**。底层存储已切换到 entt（EntityStorage），
/// 这张表用于后续把调用点改写成原生 view/get 时对照。
using StorageComponentTypes = std::tuple<
    ModelResource,
    ModelTransform,
    GeometryDevice,
    MechanicsDevice,
    AcousticsDevice,
    OpticsDevice,
    ProfileDevice,
    ActorDevice,
    CameraDevice,
    EnvironmentDevice,
    SceneDevice,
    ImageDevice>;

inline constexpr std::size_t kStorageComponentCount = std::tuple_size_v<StorageComponentTypes>;

static_assert(kStorageComponentCount == 12,
              "SharedDataHub 的 Storage 数量变了，请同步更新组件清单与迁移文档");

}  // namespace Corona::Ecs
