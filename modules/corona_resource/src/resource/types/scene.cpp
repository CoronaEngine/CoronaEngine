#include "corona/resource/types/scene.h"

#include <algorithm>
#include <array>
#include <assimp/IOStream.hpp>
#include <assimp/IOSystem.hpp>
#include <assimp/Importer.hpp>
#include <cfloat>
#include <fstream>
#include <iostream>
#include <ranges>
#include <unordered_map>
#include <vector>

#include "corona/resource/types/image.h"
#include "corona/resource/types/animation_pose.h"
#include "parse_assimp.h"

#ifdef _WIN32
#include <Windows.h>
#endif

// ============================================================================
// 自定义 IOStream 和 IOSystem 实现
// 用于支持 Unicode 路径并正确解析 MTL 等外部引用文件
// ============================================================================

namespace {

/**
 * @brief 自定义 IOStream 实现，使用 std::ifstream 支持 Unicode 路径
 */
class UnicodeIOStream : public Assimp::IOStream {
   public:
    UnicodeIOStream(const std::filesystem::path& path, const char* mode)
        : m_path(path), m_size(0) {
        std::ios_base::openmode open_mode = std::ios::binary;
        if (mode[0] == 'r') {
            open_mode |= std::ios::in;
        }
        if (mode[0] == 'w' || (mode[0] == 'r' && mode[1] == '+')) {
            open_mode |= std::ios::out;
        }

        m_stream.open(path, open_mode);
        if (m_stream.is_open()) {
            m_stream.seekg(0, std::ios::end);
            m_size = static_cast<size_t>(m_stream.tellg());
            m_stream.seekg(0, std::ios::beg);
        }
    }

    ~UnicodeIOStream() override {
        if (m_stream.is_open()) {
            m_stream.close();
        }
    }

    bool IsOpen() const { return m_stream.is_open(); }

    size_t Read(void* pvBuffer, size_t pSize, size_t pCount) override {
        m_stream.read(static_cast<char*>(pvBuffer), static_cast<std::streamsize>(pSize * pCount));
        return static_cast<size_t>(m_stream.gcount()) / pSize;
    }

    size_t Write(const void* pvBuffer, size_t pSize, size_t pCount) override {
        m_stream.write(static_cast<const char*>(pvBuffer), static_cast<std::streamsize>(pSize * pCount));
        return m_stream.good() ? pCount : 0;
    }

    aiReturn Seek(size_t pOffset, aiOrigin pOrigin) override {
        std::ios_base::seekdir dir;
        switch (pOrigin) {
            case aiOrigin_SET:
                dir = std::ios::beg;
                break;
            case aiOrigin_CUR:
                dir = std::ios::cur;
                break;
            case aiOrigin_END:
                dir = std::ios::end;
                break;
            default:
                return aiReturn_FAILURE;
        }
        m_stream.seekg(static_cast<std::streamoff>(pOffset), dir);
        return m_stream.good() ? aiReturn_SUCCESS : aiReturn_FAILURE;
    }

    size_t Tell() const override {
        return static_cast<size_t>(const_cast<std::fstream&>(m_stream).tellg());
    }

    size_t FileSize() const override {
        return m_size;
    }

    void Flush() override {
        m_stream.flush();
    }

   private:
    std::filesystem::path m_path;
    std::fstream m_stream;
    size_t m_size;
};

/**
 * @brief 自定义 IOSystem 实现，支持 Unicode 路径和相对路径解析
 *
 * 这个 IOSystem 会将相对路径解析为相对于场景文件所在目录的绝对路径，
 * 从而让 Assimp 能够正确找到 MTL 文件等外部引用。
 */
class UnicodeIOSystem : public Assimp::IOSystem {
   public:
    explicit UnicodeIOSystem(const std::filesystem::path& base_dir)
        : m_base_dir(base_dir) {}

    bool Exists(const char* pFile) const override {
        std::filesystem::path resolved = resolve_path(pFile);
        return std::filesystem::exists(resolved);
    }

    char getOsSeparator() const override {
#ifdef _WIN32
        return '\\';
#else
        return '/';
#endif
    }

    Assimp::IOStream* Open(const char* pFile, const char* pMode = "rb") override {
        std::filesystem::path resolved = resolve_path(pFile);
        auto* stream = new UnicodeIOStream(resolved, pMode);
        if (!stream->IsOpen()) {
            delete stream;
            return nullptr;
        }
        return stream;
    }

    void Close(Assimp::IOStream* pFile) override {
        delete pFile;
    }

   private:
    /**
     * @brief 从 UTF-8 字符串构造 std::filesystem::path
     * 兼容 C++17 和 C++20，包含对非 UTF-8 编码（如 GBK）的容错处理
     */
    static std::filesystem::path path_from_utf8(const char* utf8_str) {
#ifdef _WIN32
        if (utf8_str == nullptr) return {};
        // 严格按 UTF-8 解码，避免非法字节被静默替换成 U+FFFD 后变成 "����" 路径。
        int wchars_num = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_str, -1, nullptr, 0);
        if (wchars_num > 0) {
            std::vector<wchar_t> wstr(wchars_num);
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8_str, -1, wstr.data(), wchars_num);
            // 移除可能包含的 null 终止符
            if (!wstr.empty() && wstr.back() == L'\0') {
                wstr.pop_back();
            }
            return std::filesystem::path(wstr.begin(), wstr.end());
        }

        // 如果 CP_UTF8 转换失败，尝试按系统本地代码页（中文 Windows 通常为 CP936）转换。
        wchars_num = MultiByteToWideChar(CP_ACP, 0, utf8_str, -1, nullptr, 0);
        if (wchars_num > 0) {
            std::vector<wchar_t> wstr(wchars_num);
            MultiByteToWideChar(CP_ACP, 0, utf8_str, -1, wstr.data(), wchars_num);
            if (!wstr.empty() && wstr.back() == L'\0') {
                wstr.pop_back();
            }
            return std::filesystem::path(wstr.begin(), wstr.end());
        }

        CFW_LOG_WARNING("Path conversion from UTF-8 and local encoding failed (Win32)");
        return std::filesystem::path(utf8_str);
#else
        try {
#if __cplusplus >= 202002L || (defined(_MSVC_LANG) && _MSVC_LANG >= 202002L)
            // C++20: 使用 char8_t 构造函数
            return std::filesystem::path(reinterpret_cast<const char8_t*>(utf8_str));
#else
            // C++17: 使用 u8path
            return std::filesystem::u8path(utf8_str);
#endif
        } catch (const std::exception& e) {
            // 如果 UTF-8 转换失败（例如遇到了 GBK 编码的字符串），
            // 则尝试作为系统本地编码（ANSI）直接构造 path
            CFW_LOG_WARNING("Path conversion from UTF-8 failed: {}, trying local encoding", e.what());
            return std::filesystem::path(utf8_str);
        }
#endif
    }

    /**
     * @brief 解析路径，将相对路径转换为基于场景目录的绝对路径
     * @param pFile UTF-8 编码的路径字符串
     */
    std::filesystem::path resolve_path(const char* pFile) const {
        std::filesystem::path file_path = path_from_utf8(pFile);

        // 如果是绝对路径，直接返回
        if (file_path.is_absolute()) {
            return file_path;
        }

        // 相对路径：基于场景所在目录解析
        return m_base_dir / file_path;
    }

    std::filesystem::path m_base_dir;
};

}  // namespace

namespace {
/**
 * @brief 将 std::filesystem::path 转换为 UTF-8 编码的字符串
 * 用于日志输出，确保中文路径正确显示
 */
inline std::string path_to_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring& wstr = path.native();
    if (wstr.empty()) {
        return {};
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
                                   static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return path.string();
    }
    std::string utf8_str(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(),
                        static_cast<int>(wstr.size()), utf8_str.data(), size, nullptr, nullptr);
    return utf8_str;
#else
    return path.string();
#endif
}
}  // namespace

namespace Corona::Resource {

Scene::Scene(const std::filesystem::path& path) : IResource(path) {
}

std::string_view Scene::get_node_name(std::uint32_t node_idx) const {
    return data.nodes[node_idx].name;
}

std::string_view Scene::get_material_name(std::uint32_t mat_idx) const {
    return data.materials[mat_idx].name;
}

std::uint32_t Scene::add_node(std::string_view name, std::uint32_t parent) {
    auto new_index = static_cast<std::uint32_t>(data.nodes.size());
    data.nodes.emplace_back();
    NodeData& node = data.nodes.back();
    node.name = std::string{name};
    if (parent != InvalidIndex) {
        NodeData& p = data.nodes[parent];
        node.parent = &p;
        p.children.push_back(&node);
    }
    return new_index;
}

std::uint32_t Scene::add_mesh(MeshData&& mesh) {
    auto mesh_index = static_cast<std::uint32_t>(data.meshes.size());
    data.meshes.push_back(std::move(mesh));
    return mesh_index;
}

std::uint32_t Scene::add_mesh_instance(MeshInstanceData instance) {
    auto instance_index = static_cast<std::uint32_t>(data.mesh_instances.size());
    data.mesh_instances.push_back(std::move(instance));
    return instance_index;
}

SceneParser::SceneParser() {
    register_extension(".usd", [this](const auto& path, ResourceCache& cache) { return parse_assimp(path); });
    register_extension(".usda", [this](const auto& path, ResourceCache& cache) { return parse_assimp(path); });
    register_extension(".usdc", [this](const auto& path, ResourceCache& cache) { return parse_assimp(path); });
    register_extension(".usdz", [this](const auto& path, ResourceCache& cache) { return parse_assimp(path); });

    register_extension(".fbx", [this](const auto& path, ResourceCache& cache) { return parse_assimp(path); });
    register_extension(".obj", [this](const auto& path, ResourceCache& cache) { return parse_assimp(path); });
    register_extension(".gltf", [this](const auto& path, ResourceCache& cache) { return parse_assimp(path); });
    register_extension(".glb", [this](const auto& path, ResourceCache& cache) { return parse_assimp(path); });
    register_extension(".dae", [this](const auto& path, ResourceCache& cache) { return parse_assimp(path); });
    // register_extension(".blend", [this](const auto& path, ResourceCache& cache) { return parse_assimp(path); });
    register_extension(".3ds", [this](const auto& path, ResourceCache& cache) { return parse_assimp(path); });
    // register_extension(".ply", [this](const auto& path, ResourceCache& cache) { return parse_assimp(path); });
    register_extension(".stl", [this](const auto& path, ResourceCache& cache) { return parse_assimp(path); });
}

std::shared_ptr<IResource> SceneParser::parse_assimp(const std::filesystem::path& path) {
    // 获取场景文件所在目录，用于解析相对路径引用（如 MTL 文件）
    std::filesystem::path scene_dir = path.parent_path();

    // 获取文件扩展名作为格式提示
    std::string extension = path.extension().string();

    Assimp::Importer importer;

    // 设置自定义 IOSystem 以支持 Unicode 路径并正确解析相对路径引用
    // 注意：Importer 会获取 IOSystem 的所有权，无需手动删除
    importer.SetIOHandler(new UnicodeIOSystem(scene_dir));

    // 注意：不再移除原始法线 (aiComponent_NORMALS)
    // 保留模型原始法线以保持艺术家设计的硬边/软边效果
    // aiProcess_GenSmoothNormals 只在模型没有法线时才会生成

    // 使用自定义 IOSystem 后，可以直接使用 ReadFile
    // 将路径转换为 UTF-8 字符串供 Assimp 使用
    // 注意：u8string() 返回的类型在 C++17 是 std::string，在 C++20 是 std::u8string
    auto path_u8 = path.u8string();
    std::string path_str(reinterpret_cast<const char*>(path_u8.data()), path_u8.size());
    // Assimp 5.4.3 的 ValidateDataStructure 会误判合法的 KHR_materials_specular：
    // 只有 specularColorTexture（slot 1）而没有 specularTexture（slot 0）时会拒绝模型。
    // 该槽位不参与本引擎的实际材质解析，因此不要启用结构校验。
    const aiScene* ai_scene = importer.ReadFile(
        path_str,
        aiProcess_Triangulate |
            aiProcess_FindDegenerates |
            aiProcess_FindInvalidData |
            aiProcess_RemoveComponent |
            aiProcess_GenSmoothNormals |
            aiProcess_JoinIdenticalVertices |
            aiProcess_SortByPType |
            aiProcess_FlipUVs |
            aiProcess_GenBoundingBoxes |
            aiProcess_LimitBoneWeights |  // 限制每顶点最多 4 骨骼权重并归一化（蒙皮）
            aiProcess_MakeLeftHanded |    // 转换为左手坐标系
            aiProcess_FlipWindingOrder);  // 翻转三角形绕序以匹配左手坐标系

    if (!ai_scene || ai_scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !ai_scene->mRootNode) {
        CFW_LOG_ERROR("[Assimp] Failed to load scene: {}", path_to_utf8(path));
        CFW_LOG_ERROR("[Assimp] Error: {}", importer.GetErrorString());
        return nullptr;
    }

    auto scene = std::make_shared<Scene>(path);
    std::vector<std::uint32_t> material_map;

    process_assimp_materials(ai_scene, *scene, material_map, scene_dir, path, assimp_options.image_options);

    // 创建初始变换矩阵，用于处理不同格式的坐标系差异
    // STL 格式通常使用 Z-up 坐标系，需要旋转到 Y-up
    aiMatrix4x4 initial_transform;
    if (extension == ".stl") {
        // 绕 X 轴旋转 -90 度（将 Z-up 转换为 Y-up）
        // cos(-90°) = 0, sin(-90°) = -1
        // 旋转矩阵：
        // [1,  0,    0,   0]
        // [0,  0,    1,   0]  (原 Z 变成新 Y)
        // [0, -1,    0,   0]  (原 Y 变成新 -Z)
        // [0,  0,    0,   1]
        initial_transform = aiMatrix4x4(
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, -1.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f);
    }

    // 计算全局归一化参数，确保所有子网格使用相同的变换
    GlobalNormalizationParams global_params = compute_global_normalization_params(ai_scene, initial_transform);

    // ---- 蒙皮检测 ----
    // 模型带动画或任一 mesh 带骨骼即视为蒙皮场景。蒙皮分支在 process_assimp_mesh
    // 内保留绑定姿态空间（不烘焙节点变换、不单位化），并抽取每顶点骨骼权重。
    bool has_skinning = ai_scene->mNumAnimations > 0;
    for (unsigned int m = 0; !has_skinning && m < ai_scene->mNumMeshes; ++m) {
        if (ai_scene->mMeshes[m]->HasBones()) has_skinning = true;
    }

    // 蒙皮模型：先构建骨架层级 + 全局逆变换，bone_map/bone_counter 在所有 mesh 间共享。
    std::unordered_map<std::string, BoneInfo> bone_map;
    int bone_counter = 0;
    if (has_skinning) {
        SkeletonData skeleton;
        skeleton.root = read_skeleton_hierarchy(ai_scene->mRootNode, skeleton);
        // global_inverse = inverse(root 节点变换)，用于把世界空间结果拉回模型空间
        aiMatrix4x4 root_inv = ai_scene->mRootNode->mTransformation;
        root_inv.Inverse();
        skeleton.global_inverse = ai_to_mat4_colmajor(root_inv);
        // skeleton 暂存入 scene；bone_map / bone_count 在节点遍历后回填
        scene->data.skeleton = std::move(skeleton);
    }

    std::unordered_map<const aiMesh*, MeshOptimizeResult> mesh_optimization_cache;
    SharedStaticMeshAssetCache shared_static_asset_cache;
    const bool share_static_instances =
        !has_skinning && assimp_options.share_static_mesh_instances;
    process_assimp_node(ai_scene->mRootNode, ai_scene, *scene, InvalidIndex, material_map,
                        global_params, initial_transform, assimp_options,
                        has_skinning ? &bone_map : nullptr,
                        has_skinning ? &bone_counter : nullptr,
                        &mesh_optimization_cache,
                        share_static_instances,
                        &shared_static_asset_cache);

    // 回填 bone_map / bone_count（节点遍历期间由 extract_bone_weights 填充）
    if (has_skinning && scene->data.skeleton.has_value()) {
        scene->data.skeleton->bone_map = std::move(bone_map);
        scene->data.skeleton->bone_count = bone_counter;
        read_animations(ai_scene, scene->data.animations);

        CFW_LOG_NOTICE("[Assimp] Skinned model '{}': {} bones, {} animation(s)",
                       path_to_utf8(path), bone_counter, scene->data.animations.size());
        for (const auto& clip : scene->data.animations) {
            CFW_LOG_NOTICE("[Assimp]   clip '{}': duration={} ticks, tps={}, {} channels",
                           clip.name, clip.duration, clip.ticks_per_second, clip.channels.size());
        }

        // 权重健全性抽检：统计每个蒙皮 mesh 的权重和分布，确认 ≈ 1.0
        // （aiProcess_LimitBoneWeights 已归一化；偏离 1 说明抽取/remap 出错）。
        for (std::uint32_t mi = 0; mi < scene->data.meshes.size(); ++mi) {
            const auto& mesh = scene->data.meshes[mi];
            if (mesh.bone_weights.empty()) continue;

            std::size_t bad = 0;        // 权重和偏离 1.0 超过 1e-3 的顶点数
            std::size_t unweighted = 0;  // 完全无骨骼影响的顶点数
            float min_sum = 2.0f, max_sum = 0.0f;
            for (const auto& bw : mesh.bone_weights) {
                float sum = bw.weights[0] + bw.weights[1] + bw.weights[2] + bw.weights[3];
                if (sum <= 0.0f) { ++unweighted; continue; }
                min_sum = std::min(min_sum, sum);
                max_sum = std::max(max_sum, sum);
                if (std::abs(sum - 1.0f) > 1e-3f) ++bad;
            }
            CFW_LOG_NOTICE("[Assimp]   mesh[{}] skin check: {} verts, weight_sum=[{:.4f},{:.4f}], "
                           "{} off-by->1e-3, {} unweighted",
                           mi, mesh.bone_weights.size(), min_sum, max_sum, bad, unweighted);
        }

        // ====================================================================
        // 首帧归一化：把蒙皮模型规整到 1×1×1 单位立方体
        // --------------------------------------------------------------------
        // 静态模型在导入期把顶点烘进单位立方体；蒙皮模型不能这么做（会破坏 offset
        // 矩阵）。改为：用首帧（绑定姿态 t=0）蒙皮顶点求 AABB → 得 center/scale →
        // 构造归一化矩阵 N(p)=scale·(p-center)，左乘进 global_inverse。
        // 因 final = global_inverse·global·offset 是蒙皮的最左因子，左乘 N 后
        // 每帧蒙皮输出都自动落在单位立方体；绑定顶点与 offset 矩阵完全不动，
        // 蒙皮数学不变。固定一次（基于首帧），不随动画忽大忽小。
        // ====================================================================
        {
            const auto& skel = *scene->data.skeleton;
            // 1. 首帧骨骼矩阵（无动画则 animations 为空，用单位 clip 退化为绑定姿态）
            std::vector<std::array<float, 16>> finals;
            if (!scene->data.animations.empty()) {
                compute_pose(skel, scene->data.animations[0], 0.0f, finals);
            } else {
                // 无动画但有骨骼：用空 clip 取绑定姿态（compute_pose 对无通道节点用 local）
                AnimationClip empty_clip;
                compute_pose(skel, empty_clip, 0.0f, finals);
            }

            // 2. 遍历所有蒙皮 mesh 的绑定顶点，做首帧蒙皮，累积 AABB
            auto skin_pos = [&finals](const Vertex& v, const BoneWeights& bw,
                                      float& ox, float& oy, float& oz) -> bool {
                float px = 0, py = 0, pz = 0, tw = 0;
                for (int i = 0; i < MAX_BONE_INFLUENCE; ++i) {
                    std::int32_t id = bw.ids[i];
                    float w = bw.weights[i];
                    if (id < 0 || w <= 0.0f) continue;
                    if (id >= static_cast<std::int32_t>(finals.size())) continue;
                    const auto& m = finals[static_cast<std::size_t>(id)];
                    float bx = v.position[0], by = v.position[1], bz = v.position[2];
                    px += w * (m[0] * bx + m[4] * by + m[8] * bz + m[12]);
                    py += w * (m[1] * bx + m[5] * by + m[9] * bz + m[13]);
                    pz += w * (m[2] * bx + m[6] * by + m[10] * bz + m[14]);
                    tw += w;
                }
                if (tw <= 0.0f) { ox = v.position[0]; oy = v.position[1]; oz = v.position[2]; return false; }
                ox = px; oy = py; oz = pz; return true;
            };

            float min_x = FLT_MAX, min_y = FLT_MAX, min_z = FLT_MAX;
            float max_x = -FLT_MAX, max_y = -FLT_MAX, max_z = -FLT_MAX;
            bool any = false;
            for (const auto& mesh : scene->data.meshes) {
                if (mesh.bone_weights.size() != mesh.vertices.size()) continue;
                for (std::size_t v = 0; v < mesh.vertices.size(); ++v) {
                    float x, y, z;
                    skin_pos(mesh.vertices[v], mesh.bone_weights[v], x, y, z);
                    min_x = std::min(min_x, x); min_y = std::min(min_y, y); min_z = std::min(min_z, z);
                    max_x = std::max(max_x, x); max_y = std::max(max_y, y); max_z = std::max(max_z, z);
                    any = true;
                }
            }

            if (any && max_x >= min_x) {
                float cx = (min_x + max_x) * 0.5f;
                float cy = (min_y + max_y) * 0.5f;
                float cz = (min_z + max_z) * 0.5f;
                float ext = std::max({max_x - min_x, max_y - min_y, max_z - min_z});
                float s = (ext > 1e-6f) ? (1.0f / ext) : 1.0f;

                // N = scale(s) · translate(-c)，列主序：N·p = s·(p - c)
                // N[col*4+row]：对角 s，第 3 列平移 -s·c
                std::array<float, 16> N{
                    s, 0, 0, 0,
                    0, s, 0, 0,
                    0, 0, s, 0,
                    -s * cx, -s * cy, -s * cz, 1};

                // 左乘进 global_inverse：global_inverse' = N · global_inverse
                scene->data.skeleton->global_inverse =
                    mat4_mul(N, skel.global_inverse);

                // 同步更新每个蒙皮 mesh 的静态 AABB 到归一化后空间（首帧近似，用于剔除）。
                // 注意（细节1）：动画运行中 AABB 会变，这里只存首帧值作为保守初值。
                for (auto& mesh : scene->data.meshes) {
                    if (mesh.bone_weights.empty()) continue;
                    mesh.aabb_min = {(min_x - cx) * s, (min_y - cy) * s, (min_z - cz) * s};
                    mesh.aabb_max = {(max_x - cx) * s, (max_y - cy) * s, (max_z - cz) * s};
                }

                CFW_LOG_NOTICE("[Assimp] Skinned model '{}': normalized to unit cube "
                               "(center=[{:.3f},{:.3f},{:.3f}], extent={:.3f}, scale={:.4f})",
                               path_to_utf8(path), cx, cy, cz, ext, s);
            }
        }
    }

    std::unordered_map<std::string, std::uint32_t> node_name_map;

    build_node_name_map(*scene, node_name_map);
    process_assimp_lights(ai_scene, *scene, node_name_map);
    process_assimp_cameras(ai_scene, *scene, node_name_map);

    return scene;
}

const std::vector<Vertex>& Scene::get_mesh_vertices(std::uint32_t mesh_idx) const {
    return data.meshes[mesh_idx].vertices;
}

const std::vector<std::uint16_t>& Scene::get_mesh_indices(std::uint32_t mesh_idx) const {
    return data.meshes[mesh_idx].indices;
}

const Vertex& Scene::get_vertex_global(std::uint32_t mesh_idx, std::uint16_t local_index) const {
    return data.meshes[mesh_idx].vertices[local_index];
}

std::uint32_t Scene::get_mesh_lod_count(std::uint32_t mesh_idx) const {
    return static_cast<std::uint32_t>(data.meshes[mesh_idx].lod_levels.size());
}

const LODLevel& Scene::get_mesh_lod(std::uint32_t mesh_idx, std::uint32_t lod_level) const {
    return data.meshes[mesh_idx].lod_levels[lod_level];
}

AABB Scene::get_scene_aabb() const {
    if (data.meshes.empty()) {
        return AABB{};  // 返回全零 AABB
    }

    std::array<float, 3> scene_min = {FLT_MAX, FLT_MAX, FLT_MAX};
    std::array<float, 3> scene_max = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

    auto include_transformed_mesh = [&](const MeshData& mesh,
                                        const std::array<float, 16>& matrix) {
        const float xs[2] = {mesh.aabb_min[0], mesh.aabb_max[0]};
        const float ys[2] = {mesh.aabb_min[1], mesh.aabb_max[1]};
        const float zs[2] = {mesh.aabb_min[2], mesh.aabb_max[2]};
        for (float x : xs) {
            for (float y : ys) {
                for (float z : zs) {
                    const float tx = matrix[0] * x + matrix[4] * y + matrix[8] * z + matrix[12];
                    const float ty = matrix[1] * x + matrix[5] * y + matrix[9] * z + matrix[13];
                    const float tz = matrix[2] * x + matrix[6] * y + matrix[10] * z + matrix[14];
                    scene_min[0] = std::min(scene_min[0], tx);
                    scene_min[1] = std::min(scene_min[1], ty);
                    scene_min[2] = std::min(scene_min[2], tz);
                    scene_max[0] = std::max(scene_max[0], tx);
                    scene_max[1] = std::max(scene_max[1], ty);
                    scene_max[2] = std::max(scene_max[2], tz);
                }
            }
        }
    };

    if (!data.mesh_instances.empty()) {
        for (const auto& instance : data.mesh_instances) {
            if (instance.mesh_index >= data.meshes.size()) continue;
            include_transformed_mesh(data.meshes[instance.mesh_index], instance.transform);
        }
    } else {
        const std::array<float, 16> identity{
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f};
        for (const auto& mesh : data.meshes) {
            include_transformed_mesh(mesh, identity);
        }
    }

    return AABB{scene_min, scene_max};
}

}  // namespace Corona::Resource
