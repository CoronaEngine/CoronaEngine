#pragma once

#include <horizon.h>

#include <corona/shared_data_hub.h>  // MeshDevice

#include <vector>

namespace Corona {
namespace Resource {
class Scene;
}  // namespace Resource
}  // namespace Corona

namespace Corona::Systems {

// ============================================================================
// 几何 GPU 资源构建器（单一来源）
// ============================================================================
//
// 把"已导入的 Resource::Scene → GPU MeshDevice 数组"这件事收成唯一一份实现。
// 此前该逻辑在两处重复：
//   - Python API 层 Geometry 构造函数（初始加载）
//   - GeometrySystem::rebuild_actor_gpu_resources（距离重载 / LRU 恢复）
// 两份代码已在维护中分叉（占位纹理生命周期、压缩纹理路径），由此函数统一。
//
// 行为：为 scene 中每个 mesh 创建顶点/索引缓冲 + 对应 StorageBuffer + 纹理
// （压缩 BC1/BC3，未压缩 1/3/4 通道→RGBA8），无纹理的 mesh 使用传入的占位纹理。
// 纹理像素分批同步上传（每 32 个一批）。
//
// 无状态、不加锁、不触碰 SharedDataHub 槽位——调用方负责把返回的数组写回
// GeometryDevice::mesh_handles 并处理 LOD 缓存失效。GPU 资源创建在调用线程完成。
//
// 占位纹理（无纹理 mesh 的 1x1 白兜底）由本模块持有：进程级单例，首次构建时
// 惰性创建，由 release_geometry_placeholder_texture() 在 GPU device 仍存活时显式释放
// （GeometrySystem::shutdown() 调用）。这是占位纹理的**唯一所有者**，避免此前
// Python API 层与 GeometrySystem 各持一份、且 static 版本在 device 析构后才析构的崩溃。

/// 从已导入的 Scene 构建 GPU MeshDevice 数组。
///
/// @param scene 已导入的场景资源（含 meshes / materials）
/// @return 与 scene.data.meshes 一一对应的 MeshDevice 数组
[[nodiscard]] std::vector<MeshDevice> build_mesh_devices_from_scene(
    const Resource::Scene& scene);

/// 共享网格 GPU 数据：mesh_devices 只包含唯一 MeshData，
/// mesh_instances 保存每个节点的实例矩阵与 mesh 索引。
struct SceneGpuMeshData {
    std::vector<MeshDevice> mesh_devices;
    std::vector<MeshInstanceDevice> mesh_instances;
};

/// 构建共享 mesh GPU 数据。无 mesh_instances 的旧场景（含蒙皮）会为每个
/// mesh 自动生成单位实例，保持旧行为可用。
[[nodiscard]] SceneGpuMeshData build_gpu_mesh_data_from_scene(
    const Resource::Scene& scene);

/// 释放进程级共享占位纹理。必须在 GPU device 仍存活时调用
/// （由 GeometrySystem::shutdown() 负责）。可重复调用，已释放时为 no-op。
void release_geometry_placeholder_texture();

}  // namespace Corona::Systems
