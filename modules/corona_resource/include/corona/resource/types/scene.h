#pragma once
#include <array>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

#include "corona/resource/resource.h"
#include "corona/resource/types/image.h"

namespace Corona::Resource {

constexpr std::uint32_t InvalidIndex = UINT32_MAX;
constexpr std::uint64_t InvalidTextureId = UINT64_MAX;

struct Transform {
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 3> rotation{0.0f, 0.0f, 0.0f};
    std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
};

struct AABB {
    std::array<float, 3> min{};
    std::array<float, 3> max{};
};

#pragma pack(push, 1)
struct Vertex {
    std::array<float, 3> position{};
    std::array<float, 3> normal{};
    std::array<float, 2> tex_coords{};
};
#pragma pack(pop)

/// LOD 级别数据（独立顶点+索引，可直接用于渲染或物理碰撞）
struct LODLevel {
    std::vector<Vertex> vertices;
    std::vector<std::uint16_t> indices;
    float error = 0.0f;             // meshopt 计算的几何误差
    float screen_threshold = 0.0f;  // 建议屏幕占比切换阈值
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<std::uint16_t> indices;
    std::uint32_t material_index = InvalidIndex;
    std::array<float, 3> aabb_min{};
    std::array<float, 3> aabb_max{};

    // LOD 级别（LOD 1..N，LOD 0 即为 vertices/indices）
    std::vector<LODLevel> lod_levels;

    // 原始变换信息（用于恢复原始尺寸）
    std::array<float, 3> original_center{0.0f, 0.0f, 0.0f};
    float original_scale_factor{1.0f};  // 归一化时使用的缩放因子
    bool is_normalized{false};
};

/// LOD 生成配置
struct LODGenerationOptions {
    bool enabled = false;                                    // 是否生成 LOD
    std::uint32_t level_count = 3;                            // LOD 级数（不含 LOD 0）
    std::vector<float> target_ratios = {0.5f, 0.25f, 0.05f};  // 各级三角形保留比例
    std::vector<float> max_errors = {0.05f, 0.2f, 1.0f};      // 各级最大允许误差（越大简化越激进）
    std::vector<std::uint32_t> max_triangles = {0, 0, 200};   // 各级三角形数上限（0=不限制，仅用 ratio）
};

// Assimp 导入选项
struct AssimpImportOptions {
    bool simplify_mesh = true;           // 是否启用网格简化
    float simplification_error = 0.01f;  // 简化误差阈值
    LODGenerationOptions lod_options;    // LOD 生成配置
    ImageImportOptions image_options;    // 纹理导入选项
};

// 透明度混合模式
enum class AlphaMode : std::uint32_t {
    Opaque = 0,  // 完全不透明，忽略 alpha
    Mask = 1,    // Alpha 测试（cutoff）
    Blend = 2    // Alpha 混合
};

struct MaterialData {
    std::array<float, 4> base_color{1.0f, 1.0f, 1.0f, 1.0f};
    float metallic = 0.0f;
    float roughness = 0.5f;
    float ior = 1.5f;

    // 透明度相关属性
    AlphaMode alpha_mode = AlphaMode::Opaque;
    float alpha_cutoff = 0.5f;  // 仅在 AlphaMode::Mask 时使用

    std::uint64_t albedo_texture = InvalidTextureId;
    std::uint64_t normal_texture = InvalidTextureId;
    std::uint64_t metallic_texture = InvalidTextureId;
    std::uint64_t roughness_texture = InvalidTextureId;
    std::uint64_t opacity_texture = InvalidTextureId;  // 独立的透明度纹理
    std::string name;
};

struct LightData {
    enum class LightType : std::uint32_t {
        Point = 0,
        Directional = 1,
        Spot = 2,
        Area = 3
    };

    LightType type = LightType::Point;
    float intensity = 1.0f;
    float radius = 1.0f;
    float inner_angle = 30.0f;
    float outer_angle = 45.0f;
    std::array<float, 3> color{1.0f, 1.0f, 1.0f};
    std::array<float, 2> size{1.0f, 1.0f};
    float _padding[2]{};
};

struct CameraData {
    float fov = 60.0f;
    float near_clip = 0.1f;
    float far_clip = 1000.0f;
    float aspect_ratio = 1.77778f;
};

struct NodeData {
    Transform transform;

    NodeData* parent = nullptr;
    std::vector<NodeData*> children;

    std::uint32_t mesh_index = InvalidIndex;
    std::uint32_t light_index = InvalidIndex;
    std::uint32_t camera_index = InvalidIndex;

    std::string name;
};

struct SceneData {
    std::vector<MeshData> meshes;
    std::vector<MaterialData> materials;
    std::vector<LightData> lights;
    std::vector<CameraData> cameras;

    std::deque<NodeData> nodes;
};

class Scene : public IResource {
   public:
    explicit Scene(const std::filesystem::path& path);
    ~Scene() override = default;

    SceneData data;

    [[nodiscard]] std::string_view get_node_name(std::uint32_t node_idx) const;
    [[nodiscard]] std::string_view get_material_name(std::uint32_t mat_idx) const;

    std::uint32_t add_node(std::string_view name, std::uint32_t parent = InvalidIndex);

    template <typename F>
    void for_each_child(std::uint32_t node_idx, F&& func) const {
        const auto& nd = data.nodes[node_idx];
        for (auto* child_ptr : nd.children) {
            std::uint32_t idx = 0;
            for (const auto& candidate : data.nodes) {
                if (&candidate == child_ptr) break;
                ++idx;
            }
            if (idx < data.nodes.size()) {
                func(idx);
            }
        }
    }
    std::uint32_t add_mesh(MeshData&& mesh);

    // 修改：返回底层容器的常量引用（零拷贝）
    [[nodiscard]] const std::vector<Vertex>& get_mesh_vertices(std::uint32_t mesh_idx) const;
    [[nodiscard]] const std::vector<std::uint16_t>& get_mesh_indices(std::uint32_t mesh_idx) const;

    [[nodiscard]] const Vertex& get_vertex_global(std::uint32_t mesh_idx, std::uint16_t local_index) const;

    /// 获取指定 mesh 的 LOD 级数（不含 LOD 0）
    [[nodiscard]] std::uint32_t get_mesh_lod_count(std::uint32_t mesh_idx) const;
    /// 获取指定 mesh 的指定 LOD 级别数据（0=最高精度即 mesh 本身的 vertices/indices，1..N 为低精度）
    [[nodiscard]] const LODLevel& get_mesh_lod(std::uint32_t mesh_idx, std::uint32_t lod_level) const;

    /// 计算整个场景所有 mesh 合并后的 AABB 包围盒
    [[nodiscard]] AABB get_scene_aabb() const;
};

class SceneParser : public IParser {
   public:
    SceneParser();
    ~SceneParser() override = default;

    /// 设置 Assimp 导入选项（在 import 之前调用）
    AssimpImportOptions assimp_options;

   protected:
    std::shared_ptr<IResource> parse_assimp(const std::filesystem::path& path);
};

}  // namespace Corona::Resource
