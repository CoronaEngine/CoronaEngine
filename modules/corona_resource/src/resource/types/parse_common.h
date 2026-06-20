//
// Created by CoronaEngine on 2026/1/28.
// Assimp 和 USD 解析器之间共享的通用工具
//
#pragma once

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "corona/kernel/core/i_logger.h"
#include "corona/resource/types/scene.h"

namespace Corona::Resource {

// ============================================================================
// 通用类型定义
// ============================================================================

/// 纹理缓存：将纹理路径/键映射到资源 ID
using TextureCache = std::unordered_map<std::string, std::uint64_t>;

/// uint16 索引缓冲区的最大顶点数
constexpr size_t kMaxVerticesUint16 = 65530;

// ============================================================================
// 全局归一化参数
// ============================================================================

/// 全局归一化参数的基础结构
/// 用于确保所有子网格使用相同的变换
struct GlobalNormalizationParams {
    std::array<float, 3> center = {0.0f, 0.0f, 0.0f};
    float scale_factor = 1.0f;
    bool computed = false;
};

/// 从全局 AABB 计算归一化参数（共用逻辑）
/// @param params 输出参数结构体
/// @param global_min 全局 AABB 最小点
/// @param global_max 全局 AABB 最大点
/// @param source_name 数据源名称（用于日志，如 "Assimp" 或 "USD"）
/// @return 是否成功计算（如果没有有效顶点数据则返回 false）
inline bool finalize_normalization_params(
    GlobalNormalizationParams& params,
    const std::array<float, 3>& global_min,
    const std::array<float, 3>& global_max,
    const std::string& source_name) {
    // 检查是否有有效的顶点数据
    if (global_min[0] > global_max[0]) {
        params.center = {0.0f, 0.0f, 0.0f};
        params.scale_factor = 1.0f;
        params.computed = false;
        return false;
    }

    params.center = {
        (global_min[0] + global_max[0]) * 0.5f,
        (global_min[1] + global_max[1]) * 0.5f,
        (global_min[2] + global_max[2]) * 0.5f};

    float size_x = global_max[0] - global_min[0];
    float size_y = global_max[1] - global_min[1];
    float size_z = global_max[2] - global_min[2];
    float max_axis = std::max({size_x, size_y, size_z});

    params.scale_factor = (max_axis > 0.0f) ? 1.0f / max_axis : 1.0f;
    params.computed = true;

    return true;
}

// ============================================================================
// URL 解码工具
// ============================================================================

/// URL 解码助手：将 %XX 格式转换为实际字符
/// 示例: %20 -> 空格, %2F -> /
inline std::string url_decode(const std::string& str) {
    std::string result;
    result.reserve(str.size());
    for (std::size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '%' && i + 2 < str.size()) {
            // 尝试解析两个十六进制数字
            char hex[3] = {str[i + 1], str[i + 2], '\0'};
            char* end_ptr = nullptr;
            std::int32_t hex_val = static_cast<std::int32_t>(std::strtol(hex, &end_ptr, 16));
            if (end_ptr == hex + 2 && hex_val >= 0 && hex_val <= 255) {
                result += static_cast<char>(hex_val);
                i += 2;
                continue;
            }
        }
        result += str[i];
    }
    return result;
}

// ============================================================================
// AABB 计算工具
// ============================================================================

/// 计算顶点数组的 AABB（最小和最大边界）
inline void compute_aabb(const std::vector<Vertex>& vertices,
                         std::array<float, 3>& aabb_min,
                         std::array<float, 3>& aabb_max) {
    aabb_min = {FLT_MAX, FLT_MAX, FLT_MAX};
    aabb_max = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

    for (const auto& v : vertices) {
        for (int i = 0; i < 3; ++i) {
            aabb_min[i] = std::min(aabb_min[i], v.position[i]);
            aabb_max[i] = std::max(aabb_max[i], v.position[i]);
        }
    }
}

// ============================================================================
// 顶点归一化
// ============================================================================

/// 使用全局参数（中心点和缩放因子）归一化顶点
inline void normalize_vertices_with_global_params(std::vector<Vertex>& vertices,
                                                  const std::array<float, 3>& center,
                                                  float scale_factor) {
    for (auto& v : vertices) {
        v.position[0] = (v.position[0] - center[0]) * scale_factor;
        v.position[1] = (v.position[1] - center[1]) * scale_factor;
        v.position[2] = (v.position[2] - center[2]) * scale_factor;
    }
}

/// 归一化后更新 AABB
inline void normalize_aabb(std::array<float, 3>& aabb_min,
                           std::array<float, 3>& aabb_max,
                           const std::array<float, 3>& center,
                           float scale_factor) {
    for (int i = 0; i < 3; ++i) {
        aabb_min[i] = (aabb_min[i] - center[i]) * scale_factor;
        aabb_max[i] = (aabb_max[i] - center[i]) * scale_factor;
    }
}

// ============================================================================
// 数据验证与清理工具
// ============================================================================

/// 检查浮点数是否为有效值（非 NaN、非 Inf）
inline bool is_valid_float(float v) {
    return std::isfinite(v);
}

/// 检查 3D 向量是否包含有效值
inline bool is_valid_vec3(const std::array<float, 3>& v) {
    return is_valid_float(v[0]) && is_valid_float(v[1]) && is_valid_float(v[2]);
}

/// 验证并修复顶点数据中的 NaN/Inf 值
/// @param vertices 顶点数组（会被修改）
/// @param mesh_name 网格名称（用于日志）
/// @return 修复的顶点数量
inline size_t validate_and_fix_vertices(std::vector<Vertex>& vertices,
                                        const std::string& mesh_name) {
    size_t fixed_count = 0;

    for (auto& v : vertices) {
        // 检查位置
        if (!is_valid_vec3(v.position)) {
            CFW_LOG_WARNING("[MeshValidate] Mesh '{}': invalid position detected, replacing with origin",
                            mesh_name);
            v.position = {0.0f, 0.0f, 0.0f};
            ++fixed_count;
        }

        // 检查法线
        if (!is_valid_vec3(v.normal)) {
            v.normal = {0.0f, 1.0f, 0.0f};  // 默认向上
            ++fixed_count;
        }

        // 检查 UV
        if (!is_valid_float(v.tex_coords[0]) || !is_valid_float(v.tex_coords[1])) {
            v.tex_coords = {0.0f, 0.0f};
            ++fixed_count;
        }
    }

    if (fixed_count > 0) {
        CFW_LOG_WARNING("[MeshValidate] Mesh '{}': fixed {} invalid vertex attributes",
                        mesh_name, fixed_count);
    }

    return fixed_count;
}

/// 计算三角形面积的平方（避免开方提高性能）
inline float triangle_area_squared(const std::array<float, 3>& v0,
                                   const std::array<float, 3>& v1,
                                   const std::array<float, 3>& v2) {
    // 边向量
    float e1x = v1[0] - v0[0], e1y = v1[1] - v0[1], e1z = v1[2] - v0[2];
    float e2x = v2[0] - v0[0], e2y = v2[1] - v0[1], e2z = v2[2] - v0[2];

    // 叉积
    float cx = e1y * e2z - e1z * e2y;
    float cy = e1z * e2x - e1x * e2z;
    float cz = e1x * e2y - e1y * e2x;

    // 面积 = |cross| / 2，面积平方 = |cross|^2 / 4
    return (cx * cx + cy * cy + cz * cz) * 0.25f;
}

/// 移除退化三角形（面积为 0 或接近 0 的三角形）
/// @param vertices 顶点数组
/// @param indices 索引数组（会被修改）
/// @param area_threshold_sq 面积阈值的平方（默认 1e-12）
/// @param mesh_name 网格名称（用于日志）
/// @return 移除的三角形数量
inline size_t remove_degenerate_triangles(const std::vector<Vertex>& vertices,
                                          std::vector<std::uint32_t>& indices,
                                          float area_threshold_sq,
                                          const std::string& mesh_name) {
    if (indices.size() % 3 != 0) {
        CFW_LOG_WARNING("[MeshValidate] Mesh '{}': index count {} is not divisible by 3",
                        mesh_name, indices.size());
        return 0;
    }

    std::vector<std::uint32_t> valid_indices;
    valid_indices.reserve(indices.size());

    size_t removed_count = 0;
    size_t triangle_count = indices.size() / 3;

    for (size_t i = 0; i < triangle_count; ++i) {
        std::uint32_t i0 = indices[i * 3 + 0];
        std::uint32_t i1 = indices[i * 3 + 1];
        std::uint32_t i2 = indices[i * 3 + 2];

        // 检查索引是否有效
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
            CFW_LOG_WARNING("[MeshValidate] Mesh '{}': invalid index in triangle {}", mesh_name, i);
            ++removed_count;
            continue;
        }

        // 检查是否为退化三角形（两个或更多索引相同）
        if (i0 == i1 || i1 == i2 || i0 == i2) {
            ++removed_count;
            continue;
        }

        // 检查面积
        float area_sq = triangle_area_squared(
            vertices[i0].position,
            vertices[i1].position,
            vertices[i2].position);

        if (area_sq < area_threshold_sq) {
            ++removed_count;
            continue;
        }

        // 保留有效三角形
        valid_indices.push_back(i0);
        valid_indices.push_back(i1);
        valid_indices.push_back(i2);
    }

    if (removed_count > 0) {
        indices = std::move(valid_indices);
    }

    return removed_count;
}

// ============================================================================
// 法线生成工具
// ============================================================================

/// 计算三角形的面法线（未归一化）
inline std::array<float, 3> compute_face_normal(const std::array<float, 3>& v0,
                                                const std::array<float, 3>& v1,
                                                const std::array<float, 3>& v2) {
    float e1x = v1[0] - v0[0], e1y = v1[1] - v0[1], e1z = v1[2] - v0[2];
    float e2x = v2[0] - v0[0], e2y = v2[1] - v0[1], e2z = v2[2] - v0[2];

    return {
        e1y * e2z - e1z * e2y,
        e1z * e2x - e1x * e2z,
        e1x * e2y - e1y * e2x};
}

/// 归一化向量（就地修改）
inline void normalize_vec3(std::array<float, 3>& v) {
    float len_sq = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (len_sq > 1e-12f) {
        float inv_len = 1.0f / std::sqrt(len_sq);
        v[0] *= inv_len;
        v[1] *= inv_len;
        v[2] *= inv_len;
    } else {
        v = {0.0f, 1.0f, 0.0f};  // 默认向上
    }
}

/// 检查顶点是否缺少有效法线
inline bool vertices_need_normals(const std::vector<Vertex>& vertices) {
    if (vertices.empty()) return false;

    size_t zero_normal_count = 0;
    for (const auto& v : vertices) {
        float len_sq = v.normal[0] * v.normal[0] +
                       v.normal[1] * v.normal[1] +
                       v.normal[2] * v.normal[2];
        if (len_sq < 1e-6f) {
            ++zero_normal_count;
        }
    }

    // 如果超过 50% 的顶点没有有效法线，认为需要生成
    return zero_normal_count > vertices.size() / 2;
}

/// 为顶点生成平滑法线（基于面法线的角度加权平均）
/// @param vertices 顶点数组（法线会被修改）
/// @param indices 索引数组
/// @param mesh_name 网格名称（用于日志）
inline void generate_smooth_normals(std::vector<Vertex>& vertices,
                                    const std::vector<std::uint32_t>& indices,
                                    const std::string& mesh_name) {
    if (vertices.empty() || indices.empty() || indices.size() % 3 != 0) {
        return;
    }

    // 清零所有法线
    for (auto& v : vertices) {
        v.normal = {0.0f, 0.0f, 0.0f};
    }

    size_t triangle_count = indices.size() / 3;

    // 累加每个三角形对顶点法线的贡献（角度加权）
    for (size_t i = 0; i < triangle_count; ++i) {
        std::uint32_t i0 = indices[i * 3 + 0];
        std::uint32_t i1 = indices[i * 3 + 1];
        std::uint32_t i2 = indices[i * 3 + 2];

        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
            continue;
        }

        const auto& p0 = vertices[i0].position;
        const auto& p1 = vertices[i1].position;
        const auto& p2 = vertices[i2].position;

        // 计算面法线
        auto face_normal = compute_face_normal(p0, p1, p2);

        // 计算每个顶点的角度权重
        // 边向量
        std::array<float, 3> e01 = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        std::array<float, 3> e02 = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        std::array<float, 3> e12 = {p2[0] - p1[0], p2[1] - p1[1], p2[2] - p1[2]};

        auto dot = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        };
        auto length = [&dot](const std::array<float, 3>& v) {
            return std::sqrt(dot(v, v));
        };

        float len01 = length(e01);
        float len02 = length(e02);
        float len12 = length(e12);

        // 避免除以零
        if (len01 < 1e-6f || len02 < 1e-6f || len12 < 1e-6f) {
            continue;
        }

        // 计算每个顶点处的角度
        float cos_angle0 = dot(e01, e02) / (len01 * len02);
        cos_angle0 = std::clamp(cos_angle0, -1.0f, 1.0f);
        float angle0 = std::acos(cos_angle0);

        std::array<float, 3> e10 = {-e01[0], -e01[1], -e01[2]};
        float cos_angle1 = dot(e10, e12) / (len01 * len12);
        cos_angle1 = std::clamp(cos_angle1, -1.0f, 1.0f);
        float angle1 = std::acos(cos_angle1);

        float angle2 = 3.14159265f - angle0 - angle1;

        // 累加角度加权的面法线
        vertices[i0].normal[0] += face_normal[0] * angle0;
        vertices[i0].normal[1] += face_normal[1] * angle0;
        vertices[i0].normal[2] += face_normal[2] * angle0;

        vertices[i1].normal[0] += face_normal[0] * angle1;
        vertices[i1].normal[1] += face_normal[1] * angle1;
        vertices[i1].normal[2] += face_normal[2] * angle1;

        vertices[i2].normal[0] += face_normal[0] * angle2;
        vertices[i2].normal[1] += face_normal[1] * angle2;
        vertices[i2].normal[2] += face_normal[2] * angle2;
    }

    // 归一化所有法线
    for (auto& v : vertices) {
        normalize_vec3(v.normal);
    }
}

/// 验证并修复法线（确保所有法线都是单位向量）
inline void validate_and_fix_normals(std::vector<Vertex>& vertices,
                                     const std::string& mesh_name) {
    size_t fixed_count = 0;

    for (auto& v : vertices) {
        float len_sq = v.normal[0] * v.normal[0] +
                       v.normal[1] * v.normal[1] +
                       v.normal[2] * v.normal[2];

        if (len_sq < 1e-6f) {
            // 零长度法线，设为默认值
            v.normal = {0.0f, 1.0f, 0.0f};
            ++fixed_count;
        } else if (std::abs(len_sq - 1.0f) > 0.01f) {
            // 非单位长度，归一化
            normalize_vec3(v.normal);
            ++fixed_count;
        }
    }

    if (fixed_count > 0) {
        CFW_LOG_DEBUG("[MeshValidate] Mesh '{}': normalized {} vertex normals",
                      mesh_name, fixed_count);
    }
}

// ============================================================================
// Phase 2: UV 处理工具
// ============================================================================

/// 翻转 UV 的 V 坐标 (v = 1 - v)
/// 某些 DCC 工具导出的 UV 坐标原点在左上角，需要翻转到左下角
/// @param vertices 顶点数组（UV 会被修改）
/// @param mesh_name 网格名称（用于日志）
inline void flip_uv_v_coordinate(std::vector<Vertex>& vertices,
                                 const std::string& mesh_name) {
    for (auto& v : vertices) {
        v.tex_coords[1] = 1.0f - v.tex_coords[1];
    }
    CFW_LOG_DEBUG("[MeshTransform] Mesh '{}': flipped UV V coordinates for {} vertices",
                  mesh_name, vertices.size());
}

// ============================================================================
// Phase 2: 坐标系转换工具
// ============================================================================

/// 坐标系转换配置
struct CoordinateSystemConfig {
    bool is_z_up = false;      // 源坐标系是否为 Z-up
    bool flip_winding = true;  // 是否翻转三角形绕序
};

/// 应用坐标系转换（Z-up 到 Y-up）
/// 变换: (x, y, z) -> (x, z, y)
/// 法线同样变换: (nx, ny, nz) -> (nx, nz, ny)
/// @param vertices 顶点数组（位置和法线会被修改）
/// @param center 归一化中心点
/// @param scale_factor 归一化缩放因子
/// @param config 坐标系配置
/// @param mesh_name 网格名称（用于日志）
inline void apply_coordinate_system_transform(
    std::vector<Vertex>& vertices,
    const std::array<float, 3>& center,
    float scale_factor,
    const CoordinateSystemConfig& config,
    const std::string& mesh_name) {
    for (auto& v : vertices) {
        // 0. 验证法线 (防止 Zero-Length Normal 导致 shader NaN)
        float n_len_sq = v.normal[0] * v.normal[0] +
                         v.normal[1] * v.normal[1] +
                         v.normal[2] * v.normal[2];
        if (n_len_sq < 1e-6f) {
            v.normal = {0.0f, 1.0f, 0.0f};
        }

        // 1. 先进行归一化计算
        float x = (v.position[0] - center[0]) * scale_factor;
        float y = (v.position[1] - center[1]) * scale_factor;
        float z = (v.position[2] - center[2]) * scale_factor;

        // 2. 应用坐标系修正
        if (config.is_z_up) {
            // Z-up 到 Y-up 的转换: (x, y, z) -> (x, z, y)
            v.position[0] = x;
            v.position[1] = z;
            v.position[2] = y;

            // 法线变换
            float nx = v.normal[0];
            float ny = v.normal[1];
            float nz = v.normal[2];

            v.normal[0] = nx;
            v.normal[1] = nz;
            v.normal[2] = ny;
        } else {
            // 无需坐标系转换，仅应用归一化
            v.position[0] = x;
            v.position[1] = y;
            v.position[2] = z;
        }
    }

    if (config.is_z_up) {
        CFW_LOG_DEBUG("[MeshTransform] Mesh '{}': applied Z-up to Y-up coordinate transform",
                      mesh_name);
    }
}

/// 翻转三角形绕序（用于坐标系转换后修正面朝向）
/// @param indices 索引数组（会被修改）
/// @param mesh_name 网格名称（用于日志）
inline void flip_triangle_winding_order(std::vector<std::uint16_t>& indices,
                                        const std::string& mesh_name) {
    if (indices.size() % 3 != 0) {
        CFW_LOG_WARNING("[MeshTransform] Mesh '{}': cannot flip winding order, index count {} is not divisible by 3",
                        mesh_name, indices.size());
        return;
    }

    for (size_t i = 0; i < indices.size(); i += 3) {
        std::swap(indices[i], indices[i + 1]);
    }

    CFW_LOG_DEBUG("[MeshTransform] Mesh '{}': flipped winding order for {} triangles",
                  mesh_name, indices.size() / 3);
}

/// 翻转三角形绕序 (uint32 版本)
inline void flip_triangle_winding_order(std::vector<std::uint32_t>& indices,
                                        const std::string& mesh_name) {
    if (indices.size() % 3 != 0) {
        CFW_LOG_WARNING("[MeshTransform] Mesh '{}': cannot flip winding order, index count {} is not divisible by 3",
                        mesh_name, indices.size());
        return;
    }

    for (size_t i = 0; i < indices.size(); i += 3) {
        std::swap(indices[i], indices[i + 1]);
    }

    CFW_LOG_DEBUG("[MeshTransform] Mesh '{}': flipped winding order for {} triangles",
                  mesh_name, indices.size() / 3);
}

// ============================================================================
// 网格优化流水线 (使用 meshoptimizer)
// ============================================================================

/// 单个网格优化结果
struct SingleMeshResult {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};

/// 网格优化结果结构体（支持拆分为多个子网格）
struct MeshOptimizeResult {
    std::vector<Vertex> vertices;              // 主网格顶点（向后兼容）
    std::vector<std::uint32_t> indices;        // 主网格索引（向后兼容）
    std::vector<SingleMeshResult> sub_meshes;  // 拆分产生的子网格
    bool success = false;
    bool was_split = false;  // 是否进行了拆分
};

/// Phase 1 简化结果
struct SimplifyPhase1Result {
    std::vector<std::uint32_t> indices;
    size_t unique_vertex_count = 0;
    bool success = false;  // true 表示顶点数在限制以下
};

/// Phase 1: 迭代简化尝试将顶点数降至 uint16 限制以下
/// - target_index_count 始终为 0
/// - error 从 0.001 开始，步进 0.001，直到 0.01
/// @return 简化结果，如果 success=false 则需要进入 Phase 2 拆分
inline SimplifyPhase1Result simplify_phase1(
    const std::vector<Vertex>& vertices,
    const std::vector<std::uint32_t>& original_indices,
    const std::string& mesh_name) {
    SimplifyPhase1Result result;
    result.indices = original_indices;

    std::vector<unsigned int> remap(vertices.size());

    // 先检查原始网格是否已经满足条件
    result.unique_vertex_count = meshopt_generateVertexRemap(
        remap.data(),
        original_indices.data(),
        original_indices.size(),
        vertices.data(),
        vertices.size(),
        sizeof(Vertex));

    CFW_LOG_TRACE("[MeshOpt] Mesh '{}': starting phase1 simplification, {} unique vertices",
                  mesh_name, result.unique_vertex_count);

    // 如果原始网格的唯一顶点数已经在 uint16 限制以内，跳过简化
    if (result.unique_vertex_count <= kMaxVerticesUint16) {
        CFW_LOG_TRACE("[MeshOpt] Mesh '{}': already within uint16 vertex limit ({} <= {}), skipping simplification",
                      mesh_name, result.unique_vertex_count, kMaxVerticesUint16);
        result.success = true;
        return result;
    }

    // Phase 1 参数
    constexpr size_t target_index_count = 0;  // 始终为 0
    constexpr float kPhase1MaxError = 0.01f;  // 最大误差 0.01
    constexpr float kPhase1Step = 0.001f;     // 步进 0.001

    float current_error = 0.001f;

    while (current_error <= kPhase1MaxError + 0.0001f) {
        float result_error = 0.0f;
        std::vector<std::uint32_t> simplified_indices(original_indices.size());

        size_t simplified_count = meshopt_simplify(
            simplified_indices.data(),
            original_indices.data(),
            original_indices.size(),
            &vertices[0].position[0],
            vertices.size(),
            sizeof(Vertex),
            target_index_count,
            current_error,
            meshopt_SimplifyLockBorder,
            &result_error);

        if (simplified_count > 0) {
            simplified_indices.resize(simplified_count);
            result.indices = std::move(simplified_indices);

            // 重新计算唯一顶点数
            result.unique_vertex_count = meshopt_generateVertexRemap(
                remap.data(),
                result.indices.data(),
                result.indices.size(),
                vertices.data(),
                vertices.size(),
                sizeof(Vertex));

            CFW_LOG_TRACE("[MeshOpt] Mesh '{}' phase1: error {:.4f}, unique vertices {}",
                          mesh_name, current_error, result.unique_vertex_count);

            if (result.unique_vertex_count <= kMaxVerticesUint16) {
                CFW_LOG_TRACE("[MeshOpt] Mesh '{}': target reached with error {:.4f}, unique vertices {}",
                              mesh_name, current_error, result.unique_vertex_count);
                result.success = true;
                return result;
            }
        }

        current_error += kPhase1Step;
    }

    CFW_LOG_TRACE("[MeshOpt] Mesh '{}': phase1 failed to reduce below limit, {} unique vertices",
                  mesh_name, result.unique_vertex_count);
    result.success = false;
    return result;
}

/// Phase 2: 将网格按空间位置拆分为多个子网格
/// 先计算所有三角形的重心沿最长轴的位置，排序后再均分，
/// 保证切口沿空间平面 → 最小化拆分边界处的缺口。
/// 每个子网格的顶点数都在 uint16 限制以下。
/// @param vertices 原始顶点数组
/// @param indices 原始索引数组（三角形列表）
/// @param unique_vertex_count 实际唯一顶点数（用于计算拆分数量）
/// @param mesh_name 网格名称（用于日志）
/// @return 拆分后的子网格列表
inline std::vector<SingleMeshResult> split_mesh_uniformly(
    const std::vector<Vertex>& vertices,
    const std::vector<std::uint32_t>& indices,
    size_t unique_vertex_count,
    const std::string& mesh_name) {
    std::vector<SingleMeshResult> results;

    if (indices.size() % 3 != 0) {
        CFW_LOG_WARNING("[MeshOpt] Mesh '{}': cannot split, index count {} is not divisible by 3",
                        mesh_name, indices.size());
        return results;
    }

    size_t triangle_count = indices.size() / 3;

    // 基于实际唯一顶点数计算需要拆分成多少份
    constexpr size_t kSafeVertexLimit = kMaxVerticesUint16 - 100;  // 留一些余量

    // 使用实际唯一顶点数计算拆分数量
    size_t num_splits = (unique_vertex_count + kSafeVertexLimit - 1) / kSafeVertexLimit;
    num_splits = std::max(num_splits, static_cast<size_t>(2));  // 至少拆分成 2 份

    // ============================================================
    // 按空间位置排序三角形，再均分
    // 之前直接按三角形列表顺序切分，切口处三角形失去了相邻面 → 大面积缺口
    // 现在先计算每个三角形的重心在最长沙轴上的投影，排序后切分，
    // 切口沿空间平面 → 边界最小化
    // ============================================================

    // 1. 计算 AABB，找最长轴
    std::array<float, 3> aabb_min = {FLT_MAX, FLT_MAX, FLT_MAX};
    std::array<float, 3> aabb_max = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (size_t t = 0; t < triangle_count; ++t) {
        for (int j = 0; j < 3; ++j) {
            uint32_t vi = indices[t * 3 + j];
            if (vi < vertices.size()) {
                for (int k = 0; k < 3; ++k) {
                    float p = vertices[vi].position[k];
                    if (p < aabb_min[k]) aabb_min[k] = p;
                    if (p > aabb_max[k]) aabb_max[k] = p;
                }
            }
        }
    }
    float axis_x = aabb_max[0] - aabb_min[0];
    float axis_y = aabb_max[1] - aabb_min[1];
    float axis_z = aabb_max[2] - aabb_min[2];
    int longest_axis = 0;
    if (axis_y > axis_x) longest_axis = 1;
    if (axis_z > axis_x && axis_z > axis_y) longest_axis = 2;

    // 2. 计算每个三角形重心在最长沙轴上的投影，存入 (projection, original_index) 对
    struct TriProjection {
        float proj;
        size_t tri_idx;
    };
    std::vector<TriProjection> tri_projections(triangle_count);
    for (size_t t = 0; t < triangle_count; ++t) {
        uint32_t i0 = indices[t * 3 + 0];
        uint32_t i1 = indices[t * 3 + 1];
        uint32_t i2 = indices[t * 3 + 2];
        float centroid = 0.0f;
        if (i0 < vertices.size()) centroid += vertices[i0].position[longest_axis];
        if (i1 < vertices.size()) centroid += vertices[i1].position[longest_axis];
        if (i2 < vertices.size()) centroid += vertices[i2].position[longest_axis];
        centroid /= 3.0f;
        tri_projections[t] = {centroid, t};
    }

    // 3. 沿最长轴排序
    std::sort(tri_projections.begin(), tri_projections.end(),
              [](const TriProjection& a, const TriProjection& b) { return a.proj < b.proj; });

    // 4. 按排序后的顺序均分三角形
    size_t triangles_per_split = (triangle_count + num_splits - 1) / num_splits;

    // 执行拆分
    for (size_t split_idx = 0; split_idx < num_splits; ++split_idx) {
        size_t start_tri = split_idx * triangles_per_split;
        size_t end_tri = std::min(start_tri + triangles_per_split, triangle_count);

        if (start_tri >= triangle_count) break;

        // 收集这个分片的索引（按空间排序后的顺序）
        std::vector<std::uint32_t> split_indices;
        split_indices.reserve((end_tri - start_tri) * 3);

        for (size_t s = start_tri; s < end_tri; ++s) {
            size_t tri = tri_projections[s].tri_idx;
            split_indices.push_back(indices[tri * 3 + 0]);
            split_indices.push_back(indices[tri * 3 + 1]);
            split_indices.push_back(indices[tri * 3 + 2]);
        }

        // 生成顶点重映射，压缩顶点缓冲区
        std::vector<unsigned int> remap(vertices.size());
        size_t unique_count = meshopt_generateVertexRemap(
            remap.data(),
            split_indices.data(),
            split_indices.size(),
            vertices.data(),
            vertices.size(),
            sizeof(Vertex));

        // 如果这个分片的顶点数仍然超过限制，需要进一步拆分
        if (unique_count > kMaxVerticesUint16) {
            CFW_LOG_WARNING("[MeshOpt] Mesh '{}' split {}: {} unique vertices still exceeds limit, further splitting",
                            mesh_name, split_idx, unique_count);

            // 收集这个分片的索引
            auto sub_splits = split_mesh_uniformly(vertices, split_indices, unique_count,
                                                   mesh_name + "_sub" + std::to_string(split_idx));
            results.insert(results.end(), sub_splits.begin(), sub_splits.end());
            continue;
        }

        // 重映射索引和顶点
        SingleMeshResult split_result;
        split_result.indices.resize(split_indices.size());
        meshopt_remapIndexBuffer(split_result.indices.data(), split_indices.data(),
                                 split_indices.size(), remap.data());

        split_result.vertices.resize(unique_count);
        meshopt_remapVertexBuffer(
            split_result.vertices.data(),
            vertices.data(),
            vertices.size(),
            sizeof(Vertex),
            remap.data());

        CFW_LOG_TRACE("[MeshOpt] Mesh '{}' split {}: {} vertices, {} indices (spatial axis={})",
                      mesh_name, split_idx, split_result.vertices.size(), split_result.indices.size(),
                      longest_axis);

        results.push_back(std::move(split_result));
    }

    return results;
}

/// 对所有网格执行统一的迭代简化
/// 使用两阶段策略：
/// - Phase 1：error 从 0.001 开始，步进 0.001，直到 0.01
/// - Phase 2：如果 Phase 1 失败，对网格进行均匀拆分
/// @return 简化后的索引（仅当不需要拆分时有效）
/// @note 如果需要拆分，应使用 optimize_mesh_pipeline 并检查 was_split 标志
inline std::vector<std::uint32_t> iterative_simplify_for_uint16(
    const std::vector<Vertex>& vertices,
    const std::vector<std::uint32_t>& original_indices,
    const std::string& mesh_name) {
    auto phase1_result = simplify_phase1(vertices, original_indices, mesh_name);
    return phase1_result.indices;
}

/// 执行顶点缓存、过度绘制和获取优化
inline void optimize_mesh_for_gpu(std::vector<Vertex>& vertices,
                                  std::vector<std::uint32_t>& indices,
                                  const std::string& mesh_name) {
    if (vertices.empty() || indices.empty()) return;

    // 顶点缓存优化
    meshopt_optimizeVertexCache(
        indices.data(),
        indices.data(),
        indices.size(),
        vertices.size());

    // 过度绘制优化（仅针对三角形网格）
    if (indices.size() % 3 == 0) {
        meshopt_optimizeOverdraw(
            indices.data(),
            indices.data(),
            indices.size(),
            &vertices[0].position[0],
            vertices.size(),
            sizeof(Vertex),
            1.05f);
    }

    // 顶点获取优化
    std::vector<Vertex> optimized_vertices(vertices.size());
    size_t final_count = meshopt_optimizeVertexFetch(
        optimized_vertices.data(),
        indices.data(),
        indices.size(),
        vertices.data(),
        vertices.size(),
        sizeof(Vertex));

    optimized_vertices.resize(final_count);
    vertices = std::move(optimized_vertices);
}

/// 完整的网格优化流水线
/// 使用两阶段策略：
/// - Phase 1：error 从 0.001 开始，步进 0.001，直到 0.01
/// - Phase 2：如果 Phase 1 失败，对网格进行均匀拆分
/// @note 如果发生拆分，was_split=true 且 sub_meshes 包含拆分后的网格
inline MeshOptimizeResult optimize_mesh_pipeline(
    std::vector<Vertex>& unindexed_vertices,
    std::vector<std::uint32_t>& indices,
    bool /*simplify_mesh*/,          // 忽略：始终简化
    float /*simplification_error*/,  // 忽略：使用渐进式误差
    const std::string& mesh_name) {
    MeshOptimizeResult result;

    // 只有非空且索引数为3的倍数的三角形网格才能被简化
    bool can_simplify = (indices.size() % 3 == 0) && !indices.empty();

    if (!can_simplify) {
        CFW_LOG_WARNING("[MeshOpt] Mesh '{}' cannot be simplified (not a triangle mesh or empty), skipping simplification",
                        mesh_name);
        return result;
    }

    // Phase 1: 尝试迭代简化
    auto phase1_result = simplify_phase1(unindexed_vertices, indices, mesh_name);

    if (phase1_result.success) {
        // Phase 1 成功，生成顶点重映射
        std::vector<unsigned int> remap(unindexed_vertices.size());
        size_t unique_vertex_count = meshopt_generateVertexRemap(
            remap.data(),
            phase1_result.indices.data(),
            phase1_result.indices.size(),
            unindexed_vertices.data(),
            unindexed_vertices.size(),
            sizeof(Vertex));

        // 重映射索引缓冲区
        result.indices.resize(phase1_result.indices.size());
        meshopt_remapIndexBuffer(result.indices.data(), phase1_result.indices.data(),
                                 phase1_result.indices.size(), remap.data());

        // 重映射顶点缓冲区
        result.vertices.resize(unique_vertex_count);
        meshopt_remapVertexBuffer(
            result.vertices.data(),
            unindexed_vertices.data(),
            unindexed_vertices.size(),
            sizeof(Vertex),
            remap.data());

        CFW_LOG_TRACE("[MeshOpt] Mesh '{}' indexed: {} -> {} unique vertices",
                      mesh_name, unindexed_vertices.size(), unique_vertex_count);

        // GPU 优化
        if (!result.vertices.empty()) {
            optimize_mesh_for_gpu(result.vertices, result.indices, mesh_name);
        }

        result.was_split = false;
        result.success = true;
    } else {
        // Phase 2: 对网格进行均匀拆分

        auto split_results = split_mesh_uniformly(unindexed_vertices, indices,
                                                  phase1_result.unique_vertex_count, mesh_name);

        if (split_results.empty()) {
            CFW_LOG_ERROR("[MeshOpt] Mesh '{}': failed to split mesh", mesh_name);
            return result;
        }

        // 对每个子网格进行 GPU 优化
        for (auto& sub_mesh : split_results) {
            if (!sub_mesh.vertices.empty()) {
                optimize_mesh_for_gpu(sub_mesh.vertices, sub_mesh.indices, mesh_name);
            }
        }

        result.sub_meshes = std::move(split_results);
        result.was_split = true;
        result.success = true;

    }

    (void)mesh_name;

    result.success = true;
    return result;
}

/// 将 uint32 索引转换为 uint16
inline std::vector<std::uint16_t> convert_indices_to_uint16(const std::vector<std::uint32_t>& indices) {
    std::vector<std::uint16_t> result;
    result.reserve(indices.size());
    for (auto idx : indices) {
        result.push_back(static_cast<std::uint16_t>(idx));
    }
    return result;
}

// ============================================================================
// MeshData 构建器
// ============================================================================

/// 从优化的顶点和索引构建 MeshData
inline MeshData build_mesh_data(
    std::vector<Vertex>&& vertices,
    std::vector<std::uint16_t>&& indices,
    std::uint32_t material_index,
    const std::array<float, 3>& aabb_min,
    const std::array<float, 3>& aabb_max,
    const std::array<float, 3>& original_center,
    float original_scale_factor) {
    MeshData mesh_data;
    mesh_data.vertices = std::move(vertices);
    mesh_data.indices = std::move(indices);
    mesh_data.material_index = material_index;
    mesh_data.aabb_min = aabb_min;
    mesh_data.aabb_max = aabb_max;
    mesh_data.original_center = original_center;
    mesh_data.original_scale_factor = original_scale_factor;
    mesh_data.is_normalized = true;

    return mesh_data;
}

// ============================================================================
// LOD 生成
// ============================================================================

/// 为已优化的网格生成多级 LOD（独立顶点缓冲区）
/// @param vertices LOD 0 的顶点数据
/// @param indices LOD 0 的索引数据（uint16）
/// @param options LOD 生成配置
/// @param mesh_name 网格名称（用于日志）
/// @return LOD 级别列表（LOD 1..N）
inline std::vector<LODLevel> generate_lod_levels(
    const std::vector<Vertex>& vertices,
    const std::vector<std::uint16_t>& indices,
    const LODGenerationOptions& options,
    const std::string& mesh_name) {
    std::vector<LODLevel> levels;

    if (!options.enabled || options.level_count == 0 || vertices.empty() || indices.empty()) {
        return levels;
    }

    // 索引数必须是三角形（3 的倍数）
    if (indices.size() % 3 != 0) {
        CFW_LOG_WARNING("[LOD] Mesh '{}': index count {} not divisible by 3, skipping LOD generation",
                        mesh_name, indices.size());
        return levels;
    }

    // 将 uint16 索引升级为 uint32 供 meshopt 使用
    std::vector<std::uint32_t> indices_u32(indices.begin(), indices.end());

    std::uint32_t actual_levels = std::min({options.level_count,
                                            static_cast<std::uint32_t>(options.target_ratios.size()),
                                            static_cast<std::uint32_t>(options.max_errors.size()),
                                            options.max_triangles.empty() ? options.level_count : static_cast<std::uint32_t>(options.max_triangles.size())});

    for (std::uint32_t i = 0; i < actual_levels; ++i) {
        float ratio = options.target_ratios[i];
        float level_max_error = options.max_errors[i];
        size_t target_index_count = static_cast<size_t>(
            static_cast<float>(indices_u32.size()) * ratio);

        // 应用绝对三角形上限（0=不限制）
        if (i < options.max_triangles.size() && options.max_triangles[i] > 0) {
            size_t cap = static_cast<size_t>(options.max_triangles[i]) * 3;
            if (target_index_count > cap) {
                target_index_count = cap;
            }
        }

        // 至少保留一个三角形
        target_index_count = std::max(target_index_count, static_cast<size_t>(3));
        // 对齐到 3 的倍数
        target_index_count = (target_index_count / 3) * 3;

        std::vector<std::uint32_t> simplified_indices(indices_u32.size());
        float result_error = 0.0f;

        size_t simplified_count = meshopt_simplify(
            simplified_indices.data(),
            indices_u32.data(),
            indices_u32.size(),
            &vertices[0].position[0],
            vertices.size(),
            sizeof(Vertex),
            target_index_count,
            level_max_error,
            0,  // 不锁定边界，LOD 允许更自由地简化
            &result_error);

        // 如果 meshopt_simplify 未能达到目标（实际 > 目标的 2 倍），
        // 之前使用 meshopt_simplifySloppy 强制达到目标面数，
        // 但 sloppy 不保拓扑，会产生裂缝/破洞/翻转三角形，导致渲染缺口。
        // 现在改为：保持 simplify 的结果，宁可面数多一点也不牺牲拓扑正确性。
        if (simplified_count > target_index_count * 2) {
            CFW_LOG_WARNING("[LOD] Mesh '{}' LOD {}: simplify could not reach target (got {} indices, target was {}). "
                            "Sloppy fallback disabled to preserve topology. Keeping simplify result.",
                            mesh_name, i + 1, simplified_count, target_index_count);
        }

        if (simplified_count == 0) {
            CFW_LOG_WARNING("[LOD] Mesh '{}': LOD {} simplification produced 0 indices, stopping",
                            mesh_name, i + 1);
            break;
        }

        simplified_indices.resize(simplified_count);

        // 生成紧凑的独立顶点缓冲区
        std::vector<unsigned int> remap(vertices.size());
        size_t unique_vertex_count = meshopt_generateVertexRemap(
            remap.data(),
            simplified_indices.data(),
            simplified_indices.size(),
            vertices.data(),
            vertices.size(),
            sizeof(Vertex));

        std::vector<std::uint32_t> remapped_indices(simplified_indices.size());
        meshopt_remapIndexBuffer(remapped_indices.data(), simplified_indices.data(),
                                 simplified_indices.size(), remap.data());

        std::vector<Vertex> compact_vertices(unique_vertex_count);
        meshopt_remapVertexBuffer(
            compact_vertices.data(),
            vertices.data(),
            vertices.size(),
            sizeof(Vertex),
            remap.data());

        // 移除退化三角形（meshopt_simplifySloppy 可能产生零面积/退化三角形，导致渲染缺口）
        remove_degenerate_triangles(compact_vertices, remapped_indices,
                                    1e-12f, mesh_name + "_lod" + std::to_string(i + 1));

        // 重新计算法线：仅当几何真的被简化了才需要。
        // 对于 split 子网格（有大面积切口边界），SimplifyLockBorder 可能导致
        // 简化完全无法进行 → simplified_count == original_count → 几何未变。
        // 此时若重算法线，边界处缺少相邻 split 的三角形贡献 → 法线偏斜 → 黑色缺口。
        // 几何未变时保留 LOD0 原法线即可。
        if (simplified_count < indices_u32.size()) {
            generate_smooth_normals(compact_vertices, remapped_indices,
                                    mesh_name + "_lod" + std::to_string(i + 1));
        }

        // GPU 优化
        optimize_mesh_for_gpu(compact_vertices, remapped_indices,
                              mesh_name + "_lod" + std::to_string(i + 1));

        // 转换索引为 uint16
        std::vector<std::uint16_t> final_indices;
        final_indices.reserve(remapped_indices.size());
        for (auto idx : remapped_indices) {
            final_indices.push_back(static_cast<std::uint16_t>(idx));
        }

        LODLevel level;
        level.vertices = std::move(compact_vertices);
        level.indices = std::move(final_indices);
        level.error = result_error;
        // 屏幕阈值：误差越大，阈值越小（越远才使用）
        // 使用反比公式：error 越大 → threshold 越小 → 只在屏幕占比更小时选中此 LOD
        level.screen_threshold = (result_error > 0.0f) ? std::max(0.01f, 1.0f / (1.0f + result_error * 80.0f)) : 0.0f;

        (void)mesh_name;
        (void)ratio;
        (void)result_error;

        levels.push_back(std::move(level));
    }

    return levels;
}

// ============================================================================
// 节点名称映射构建器
// ============================================================================

/// 构建节点名称到节点索引的映射
inline void build_node_name_map(const Scene& scene,
                                std::unordered_map<std::string, std::uint32_t>& node_name_map) {
    for (std::uint32_t i = 0; i < scene.data.nodes.size(); ++i) {
        node_name_map[std::string(scene.get_node_name(i))] = i;
    }
}

// ============================================================================
// 材质工具
// ============================================================================

/// 当存在纹理时修复黑色材质颜色
/// 某些导出器（如 Maya）导出 Kd=0，但依赖 map_Kd 纹理
inline void fix_black_material_with_texture(MaterialData& mat_data, const std::string& source_name) {
    if (mat_data.albedo_texture != InvalidTextureId) {
        constexpr float black_threshold = 0.01f;
        bool is_nearly_black = (mat_data.base_color[0] < black_threshold &&
                                mat_data.base_color[1] < black_threshold &&
                                mat_data.base_color[2] < black_threshold);
        if (is_nearly_black) {
            CFW_LOG_DEBUG("[{}] Material '{}': black/dark color ({}, {}, {}) with texture, overriding to white",
                          source_name, mat_data.name,
                          mat_data.base_color[0], mat_data.base_color[1], mat_data.base_color[2]);
            mat_data.base_color[0] = 1.0f;
            mat_data.base_color[1] = 1.0f;
            mat_data.base_color[2] = 1.0f;
            // 保留原始 Alpha 值
        }
    }
}

}  // namespace Corona::Resource
