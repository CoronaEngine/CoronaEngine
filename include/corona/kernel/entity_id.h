#pragma once

#include <corona/kernel/entity_id.h>
#include <cstdint>

namespace Corona {

/// 实体标识。
///
/// 当前实现是 `Storage` 槽位地址（`std::uintptr_t`）。之所以引入这个别名，
/// 是因为句柄语义散落在全仓 100+ 处 `std::uintptr_t` 上，直接换容器时
/// 无法区分"实体句柄"和"普通整数"。
///
/// 迁移到 EnTT 时，这里会变成 `entt::entity`（32 位 index + version），
/// 届时所有使用 `EntityId` 的位置自动跟随，剩下的 `std::uintptr_t` 即为
/// 真正与实体无关的整数。
///
/// 约定：跨系统链接字段（`*_handle`）、命令的 `KeyOf::get()` 返回值、
/// 以实体为 key 的映射表，一律使用 `EntityId`。
using EntityId = std::uintptr_t;

/// 无效实体。与 `Storage::allocate()` 失败时的取值保持一致。
inline constexpr EntityId kInvalidEntity = 0;

}  // namespace Corona

// 让全局作用域也能直接使用 EntityId。
// 原因：不少 .cpp 在文件级的匿名命名空间里定义持句柄的 helper（那不在
// `namespace Corona` 内），逐个文件补 using 容易漏；这里统一导出一次。
using Corona::EntityId;
using Corona::kInvalidEntity;
