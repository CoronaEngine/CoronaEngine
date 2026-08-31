//
// Created by Administrator on 2025/12/2.
//
#pragma once

#include <assimp/GltfMaterial.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include "corona/resource/resource_manager.h"
#include "corona/resource/types/image.h"
#include "parse_common.h"

namespace Corona::Resource {

// 构建 mesh 索引到累积变换的映射
inline void build_mesh_transform_map(aiNode* node, const aiMatrix4x4& parent_transform,
                                     std::unordered_map<unsigned int, aiMatrix4x4>& mesh_transforms) {
    aiMatrix4x4 current_transform = parent_transform * node->mTransformation;

    for (unsigned int i = 0; i < node->mNumMeshes; ++i) {
        unsigned int mesh_index = node->mMeshes[i];
        mesh_transforms[mesh_index] = current_transform;
    }

    for (unsigned int i = 0; i < node->mNumChildren; ++i) {
        build_mesh_transform_map(node->mChildren[i], current_transform, mesh_transforms);
    }
}

// 计算场景中所有网格的全局 AABB（在世界空间，应用节点变换）
// 这样可以正确处理节点层级的相对变换关系
// initial_transform: 可选的初始变换矩阵，用于坐标系转换（如 STL 的 Z-up 转 Y-up）
inline GlobalNormalizationParams compute_global_normalization_params(const aiScene* ai_scene,
                                                                     const aiMatrix4x4& initial_transform = aiMatrix4x4()) {
    GlobalNormalizationParams params;
    std::array<float, 3> global_min = {FLT_MAX, FLT_MAX, FLT_MAX};
    std::array<float, 3> global_max = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

    // 首先构建 mesh 到累积变换的映射（应用初始变换）
    std::unordered_map<unsigned int, aiMatrix4x4> mesh_transforms;
    build_mesh_transform_map(ai_scene->mRootNode, initial_transform, mesh_transforms);

    // 遍历所有网格，计算联合 AABB（应用节点变换到世界空间）
    for (unsigned int m = 0; m < ai_scene->mNumMeshes; ++m) {
        aiMesh* ai_mesh = ai_scene->mMeshes[m];
        aiMatrix4x4 transform = mesh_transforms.count(m) ? mesh_transforms[m] : aiMatrix4x4();

        for (unsigned int v = 0; v < ai_mesh->mNumVertices; ++v) {
            // 将顶点变换到世界空间
            aiVector3D world_pos = transform * ai_mesh->mVertices[v];
            global_min[0] = std::min(global_min[0], world_pos.x);
            global_min[1] = std::min(global_min[1], world_pos.y);
            global_min[2] = std::min(global_min[2], world_pos.z);
            global_max[0] = std::max(global_max[0], world_pos.x);
            global_max[1] = std::max(global_max[1], world_pos.y);
            global_max[2] = std::max(global_max[2], world_pos.z);
        }
    }

    // 使用公共函数计算归一化参数
    finalize_normalization_params(params, global_min, global_max, "Assimp");

    return params;
}

// 从嵌入式纹理数据加载纹理 (GLB/glTF 等格式)
// scene_path: 模型文件路径，用于区分不同模型的嵌入纹理，避免虚拟路径冲突
inline std::uint64_t load_embedded_texture(const aiTexture* embedded_tex,
                                           const std::string& cache_key,
                                           TextureCache& texture_cache,
                                           const std::filesystem::path& scene_path,
                                           const ImageImportOptions& options = {}) {
    if (!embedded_tex) {
        return InvalidTextureId;
    }

    // 检查缓存
    auto cache_it = texture_cache.find(cache_key);
    if (cache_it != texture_cache.end()) {
        return cache_it->second;
    }

    std::shared_ptr<IResource> image_resource;

    if (embedded_tex->mHeight == 0) {
        // 压缩格式 (PNG, JPG 等) - mWidth 是数据大小（字节数）
        std::span<const std::byte> data_span(
            reinterpret_cast<const std::byte*>(embedded_tex->pcData),
            embedded_tex->mWidth);

        // 生成一个虚拟路径用于资源标识（包含模型路径以区分不同模型的嵌入纹理）
        std::string format_hint = embedded_tex->achFormatHint;
        std::string scene_id = scene_path.string();
        std::filesystem::path virtual_path = std::string("__embedded_texture_") + scene_id + "_" + cache_key +
                                             (format_hint.empty() ? ".png" : ("." + format_hint));

        ImageParser parser;
        image_resource = parser.parse_from_memory(data_span, virtual_path, options);

        if (image_resource) {
        }
    } else {
        // 未压缩的 ARGB8888 格式
        auto image = std::make_shared<Image>(
            std::filesystem::path(std::string("__embedded_texture_") + scene_path.string() + "_" + cache_key + ".raw"));

        // Assimp 的未压缩纹理是 ARGB8888 格式，需要转换为 RGBA
        std::vector<unsigned char> rgba_data(embedded_tex->mWidth * embedded_tex->mHeight * 4);
        for (unsigned int i = 0; i < embedded_tex->mWidth * embedded_tex->mHeight; ++i) {
            aiTexel& texel = embedded_tex->pcData[i];
            rgba_data[i * 4 + 0] = texel.r;
            rgba_data[i * 4 + 1] = texel.g;
            rgba_data[i * 4 + 2] = texel.b;
            rgba_data[i * 4 + 3] = texel.a;
        }

        image->set_data(rgba_data.data(),
                        static_cast<int>(embedded_tex->mWidth),
                        static_cast<int>(embedded_tex->mHeight),
                        4);
        if (options.compress) {
            image->compress(options.format);
        }
        image_resource = image;
    }

    if (!image_resource) {
        CFW_LOG_WARNING("[Assimp] Failed to load embedded texture '{}'", cache_key);
        return InvalidTextureId;
    }

    // 注册资源并缓存
    std::uint64_t tex_id = image_resource->get_uid();
    if (!ResourceManager::get_instance().add_resource(tex_id, image_resource)) {
        CFW_LOG_WARNING("[Assimp] Embedded texture '{}' (scene '{}') uid={} already exists in ResourceManager, "
                        "using existing resource",
                        cache_key, scene_path.string(), tex_id);
    }
    texture_cache[cache_key] = tex_id;

    return tex_id;
}

inline std::uint64_t load_assimp_texture(aiMaterial* ai_mat, aiTextureType type,
                                         const std::filesystem::path& scene_dir,
                                         TextureCache& texture_cache,
                                         const std::filesystem::path& scene_path,
                                         const aiScene* ai_scene = nullptr,
                                         const ImageImportOptions& options = {}) {
    if (ai_mat->GetTextureCount(type) == 0) {
        return InvalidTextureId;
    }
    aiString tex_path;
    if (ai_mat->GetTexture(type, 0, &tex_path) != AI_SUCCESS) {
        return InvalidTextureId;
    }

    std::string tex_path_str = tex_path.C_Str();

    // URL 解码：GLTF 等格式的纹理路径可能包含 URL 编码字符（如 %20 表示空格）
    std::string decoded_path = url_decode(tex_path_str);

    // 检查是否是嵌入式纹理
    if (ai_scene != nullptr) {
        // 方式1: 以 '*' 开头的索引格式 (如 "*0", "*1" 等，常见于 glTF/GLB)
        if (!decoded_path.empty() && decoded_path[0] == '*') {
            int texture_index = std::atoi(decoded_path.c_str() + 1);

            if (texture_index >= 0 &&
                static_cast<unsigned int>(texture_index) < ai_scene->mNumTextures) {
                aiTexture* embedded_tex = ai_scene->mTextures[texture_index];
                return load_embedded_texture(embedded_tex, tex_path_str, texture_cache, scene_path, options);
            } else {
                CFW_LOG_WARNING("[Assimp] Embedded texture index {} out of range (scene has {} textures)",
                                texture_index, ai_scene->mNumTextures);
                return InvalidTextureId;
            }
        }

        // 方式2: 使用 GetEmbeddedTexture 检查 (FBX 等格式的内嵌纹理)
        // FBX 内嵌纹理的路径可能是文件名格式，需要通过 GetEmbeddedTexture 来获取
        const aiTexture* embedded_tex = ai_scene->GetEmbeddedTexture(tex_path.C_Str());
        if (embedded_tex != nullptr) {
            return load_embedded_texture(embedded_tex, tex_path_str, texture_cache, scene_path, options);
        }
    }

    std::filesystem::path original_path(decoded_path);
    std::filesystem::path full_path;

    // 尝试多种路径策略来定位纹理文件
    // 1. 首先尝试直接使用相对路径（使用 URL 解码后的路径）
    full_path = scene_dir / decoded_path;
    if (!std::filesystem::exists(full_path)) {
        // 2. 如果是绝对路径或包含目录，尝试只使用文件名
        std::filesystem::path filename = original_path.filename();
        full_path = scene_dir / filename;

        if (!std::filesystem::exists(full_path)) {
            // 3. 尝试在 textures 子目录中查找
            full_path = scene_dir / "textures" / filename;

            if (!std::filesystem::exists(full_path)) {
                // 4. 尝试在 Textures 子目录中查找 (大小写敏感的文件系统)
                full_path = scene_dir / "Textures" / filename;

                if (!std::filesystem::exists(full_path)) {
                    CFW_LOG_WARNING("[Assimp] Texture not found: '{}', tried scene dir and textures subdir",
                                    tex_path.C_Str());
                    return InvalidTextureId;
                }
            }
        }
    }
    std::string path_str = full_path.string();

    auto cache_it = texture_cache.find(path_str);
    if (cache_it != texture_cache.end()) {
        return cache_it->second;
    }

    auto& mgr = ResourceManager::get_instance();
    TResourceID tex_id = mgr.import_sync(full_path);
    if (tex_id == IResource::INVALID_UID) {
        return InvalidTextureId;
    }

    texture_cache[path_str] = tex_id;
    return tex_id;
}

inline void process_assimp_materials(const aiScene* ai_scene, Scene& scene,
                                     std::vector<std::uint32_t>& material_map,
                                     const std::filesystem::path& scene_dir,
                                     const std::filesystem::path& scene_path,
                                     const ImageImportOptions& options = {}) {
    TextureCache texture_cache;
    material_map.resize(ai_scene->mNumMaterials);
    for (unsigned int i = 0; i < ai_scene->mNumMaterials; ++i) {
        aiMaterial* ai_mat = ai_scene->mMaterials[i];
        MaterialData mat_data;
        aiString name;
        if (ai_mat->Get(AI_MATKEY_NAME, name) == AI_SUCCESS) {
            mat_data.name = name.C_Str();
        }
        const bool is_assimp_default_material =
            mat_data.name.empty() || mat_data.name == AI_DEFAULT_MATERIAL_NAME;

        // 尝试读取颜色：优先使用 PBR 的 BASE_COLOR，然后回退到传统的 DIFFUSE
        aiColor4D color;
        bool color_found = false;

        // 首先尝试 PBR 基础颜色 (glTF, FBX 等格式)
        if (ai_mat->Get(AI_MATKEY_BASE_COLOR, color) == AI_SUCCESS) {
            mat_data.base_color = {color.r, color.g, color.b, color.a};
            color_found = true;
        }
        // 然后尝试传统漫反射颜色 (OBJ, 3DS 等格式)
        else if (ai_mat->Get(AI_MATKEY_COLOR_DIFFUSE, color) == AI_SUCCESS) {
            mat_data.base_color = {color.r, color.g, color.b, color.a};
            color_found = true;
        }

        // OBJ files without an MTL still get an Assimp-generated material
        // whose diffuse color is the library default, not authored content.
        // Keep Corona's default white material for those proxy/simple meshes.
        if (color_found && is_assimp_default_material) {
            constexpr float kAssimpDefaultDiffuse = 0.6f;
            constexpr float kEpsilon = 1e-4f;
            const bool default_diffuse =
                std::abs(mat_data.base_color[0] - kAssimpDefaultDiffuse) < kEpsilon &&
                std::abs(mat_data.base_color[1] - kAssimpDefaultDiffuse) < kEpsilon &&
                std::abs(mat_data.base_color[2] - kAssimpDefaultDiffuse) < kEpsilon;
            if (default_diffuse) {
                mat_data.base_color = MaterialData{}.base_color;
                color_found = false;
            }
        }

        // 如果没有找到任何颜色，记录警告
        if (!color_found) {
            CFW_LOG_WARNING("[Assimp] Material '{}': no color found, using default white",
                            mat_data.name);
        }

        // 确保 alpha 值有效（某些格式可能不设置 alpha）
        if (mat_data.base_color[3] <= 0.0f) {
            // 尝试从 opacity 属性读取
            float opacity = 1.0f;
            if (ai_mat->Get(AI_MATKEY_OPACITY, opacity) == AI_SUCCESS) {
                mat_data.base_color[3] = opacity;
            } else {
                mat_data.base_color[3] = 1.0f;
            }
        }

        float metallic = 0.0f;
        if (ai_mat->Get(AI_MATKEY_METALLIC_FACTOR, metallic) == AI_SUCCESS) {
            mat_data.metallic = metallic;
        }
        float roughness = 0.5f;
        if (ai_mat->Get(AI_MATKEY_ROUGHNESS_FACTOR, roughness) == AI_SUCCESS) {
            mat_data.roughness = roughness;
        } else {
            // 对于传统材质，尝试从 shininess 转换为 roughness
            float shininess = 0.0f;
            if (ai_mat->Get(AI_MATKEY_SHININESS, shininess) == AI_SUCCESS && shininess > 0.0f) {
                // 将 shininess (通常 0-1000) 转换为 roughness (0-1)
                // 使用简单的经验公式：roughness = 1 - sqrt(shininess / 1000)
                mat_data.roughness = 1.0f - std::sqrt(std::min(shininess, 1000.0f) / 1000.0f);
            }
        }

        // === 诊断日志：打印材质中所有纹理类型的数量 ===
        static const std::pair<aiTextureType, const char*> all_texture_types[] = {
            {aiTextureType_DIFFUSE, "DIFFUSE"},
            {aiTextureType_SPECULAR, "SPECULAR"},
            {aiTextureType_AMBIENT, "AMBIENT"},
            {aiTextureType_EMISSIVE, "EMISSIVE"},
            {aiTextureType_HEIGHT, "HEIGHT"},
            {aiTextureType_NORMALS, "NORMALS"},
            {aiTextureType_SHININESS, "SHININESS"},
            {aiTextureType_OPACITY, "OPACITY"},
            {aiTextureType_DISPLACEMENT, "DISPLACEMENT"},
            {aiTextureType_LIGHTMAP, "LIGHTMAP"},
            {aiTextureType_REFLECTION, "REFLECTION"},
            {aiTextureType_BASE_COLOR, "BASE_COLOR"},
            {aiTextureType_NORMAL_CAMERA, "NORMAL_CAMERA"},
            {aiTextureType_EMISSION_COLOR, "EMISSION_COLOR"},
            {aiTextureType_METALNESS, "METALNESS"},
            {aiTextureType_DIFFUSE_ROUGHNESS, "DIFFUSE_ROUGHNESS"},
            {aiTextureType_AMBIENT_OCCLUSION, "AMBIENT_OCCLUSION"},
            {aiTextureType_UNKNOWN, "UNKNOWN"},
        };
        for (const auto& [type, type_name] : all_texture_types) {
            unsigned int count = ai_mat->GetTextureCount(type);
            if (count > 0) {
                aiString path;
                ai_mat->GetTexture(type, 0, &path);
            }
        }

        // 加载纹理：优先使用 PBR 纹理类型，然后回退到传统类型
        // glTF/PBR 使用 aiTextureType_BASE_COLOR，传统格式使用 aiTextureType_DIFFUSE
        mat_data.albedo_texture = load_assimp_texture(ai_mat, aiTextureType_BASE_COLOR, scene_dir, texture_cache, scene_path, ai_scene, options);
        if (mat_data.albedo_texture == InvalidTextureId) {
            mat_data.albedo_texture = load_assimp_texture(ai_mat, aiTextureType_DIFFUSE, scene_dir, texture_cache, scene_path, ai_scene, options);
        }
        // 扩展回退：尝试 AMBIENT 类型（某些 FBX 导出器可能将 diffuse 纹理存储在这里）
        if (mat_data.albedo_texture == InvalidTextureId) {
            mat_data.albedo_texture = load_assimp_texture(ai_mat, aiTextureType_AMBIENT, scene_dir, texture_cache, scene_path, ai_scene, options);
        }
        // 扩展回退：尝试 EMISSIVE 类型（某些工具可能错误地将纹理存储在这里）
        if (mat_data.albedo_texture == InvalidTextureId) {
            mat_data.albedo_texture = load_assimp_texture(ai_mat, aiTextureType_EMISSIVE, scene_dir, texture_cache, scene_path, ai_scene, options);
        }
        // 扩展回退：尝试 UNKNOWN 类型（通用/未分类的纹理）
        if (mat_data.albedo_texture == InvalidTextureId) {
            mat_data.albedo_texture = load_assimp_texture(ai_mat, aiTextureType_UNKNOWN, scene_dir, texture_cache, scene_path, ai_scene, options);
        }
        // 最后手段：如果所有类型都失败，但场景有嵌入纹理，直接使用第一个嵌入纹理
        if (mat_data.albedo_texture == InvalidTextureId && ai_scene->mNumTextures > 0) {
            CFW_LOG_WARNING("[Assimp] Material '{}': all texture types failed, using first embedded texture as fallback",
                            mat_data.name);
            mat_data.albedo_texture = load_embedded_texture(ai_scene->mTextures[0], "__fallback_embedded_0", texture_cache, scene_path, options);
        }

        mat_data.normal_texture = load_assimp_texture(ai_mat, aiTextureType_NORMALS, scene_dir, texture_cache, scene_path, ai_scene, options);
        if (mat_data.normal_texture == InvalidTextureId) {
            mat_data.normal_texture = load_assimp_texture(ai_mat, aiTextureType_NORMAL_CAMERA, scene_dir, texture_cache, scene_path, ai_scene, options);
        }
        if (mat_data.normal_texture == InvalidTextureId) {
            mat_data.normal_texture = load_assimp_texture(ai_mat, aiTextureType_HEIGHT, scene_dir, texture_cache, scene_path, ai_scene, options);
        }

        mat_data.metallic_texture = load_assimp_texture(ai_mat, aiTextureType_METALNESS, scene_dir, texture_cache, scene_path, ai_scene, options);

        // glTF 通常将 metallic 和 roughness 存储在同一张纹理中 (unknown texture type)
        mat_data.roughness_texture = load_assimp_texture(ai_mat, aiTextureType_DIFFUSE_ROUGHNESS, scene_dir, texture_cache, scene_path, ai_scene, options);
        if (mat_data.roughness_texture == InvalidTextureId) {
            mat_data.roughness_texture = load_assimp_texture(ai_mat, aiTextureType_SHININESS, scene_dir, texture_cache, scene_path, ai_scene, options);
        }
        // 如果有 metallic 纹理但没有 roughness 纹理，尝试使用 unknown 类型 (glTF metallicRoughness)
        if (mat_data.roughness_texture == InvalidTextureId && mat_data.metallic_texture == InvalidTextureId) {
            std::uint64_t mr_texture = load_assimp_texture(ai_mat, aiTextureType_UNKNOWN, scene_dir, texture_cache, scene_path, ai_scene, options);
            if (mr_texture != InvalidTextureId) {
                // glTF 的 metallicRoughness 纹理同时包含两个通道
                mat_data.metallic_texture = mr_texture;
                mat_data.roughness_texture = mr_texture;
            }
        }

        // ========== 透明度处理 ==========
        // 加载 opacity 纹理
        mat_data.opacity_texture = load_assimp_texture(ai_mat, aiTextureType_OPACITY, scene_dir, texture_cache, scene_path, ai_scene, options);

        // 读取 glTF alphaMode (通过 Assimp 的字符串属性)
        aiString alpha_mode_str;
        if (ai_mat->Get(AI_MATKEY_GLTF_ALPHAMODE, alpha_mode_str) == AI_SUCCESS) {
            std::string mode = alpha_mode_str.C_Str();
            if (mode == "MASK") {
                mat_data.alpha_mode = AlphaMode::Mask;
            } else if (mode == "BLEND") {
                mat_data.alpha_mode = AlphaMode::Blend;
            }
            // "OPAQUE" 是默认值，无需处理
        }

        // 读取 glTF alphaCutoff
        float alpha_cutoff = 0.5f;
        if (ai_mat->Get(AI_MATKEY_GLTF_ALPHACUTOFF, alpha_cutoff) == AI_SUCCESS) {
            mat_data.alpha_cutoff = alpha_cutoff;
            // 如果设置了 cutoff 但没有明确的 alphaMode，推断为 Mask 模式
            if (mat_data.alpha_mode == AlphaMode::Opaque && alpha_cutoff > 0.0f) {
                mat_data.alpha_mode = AlphaMode::Mask;
            }
            (void)mat_data.name;
        }

        // 如果没有明确的 alphaMode，根据其他属性推断
        if (mat_data.alpha_mode == AlphaMode::Opaque) {
            // 如果有 opacity 纹理，使用 Blend 模式
            if (mat_data.opacity_texture != InvalidTextureId) {
                mat_data.alpha_mode = AlphaMode::Blend;
            }
            // 如果 base_color alpha < 1，使用 Blend 模式
            else if (mat_data.base_color[3] < 1.0f) {
                mat_data.alpha_mode = AlphaMode::Blend;
            }
        }

        // 修复：如果有纹理但颜色为黑色/接近黑色，将base_color设为白色
        fix_black_material_with_texture(mat_data, "Assimp");

        if (mat_data.albedo_texture == InvalidTextureId) {
            mat_data.albedo_texture = get_or_create_default_white_texture();
        }

        material_map[i] = static_cast<std::uint32_t>(scene.data.materials.size());
        scene.data.materials.emplace_back(mat_data);
    }
}

// ============================================================================
// 骨骼动画抽取（绑定期）
// ============================================================================

/// 将 assimp aiMatrix4x4（行主序）转为列主序 std::array<float,16>（ktm/glsl 约定，
/// 下标 = col*4 + row）。即对 assimp 矩阵做转置存储。
inline std::array<float, 16> ai_to_mat4_colmajor(const aiMatrix4x4& m) {
    return {
        m.a1, m.b1, m.c1, m.d1,   // 列 0
        m.a2, m.b2, m.c2, m.d2,   // 列 1
        m.a3, m.b3, m.c3, m.d3,   // 列 2
        m.a4, m.b4, m.c4, m.d4};  // 列 3
}

/// 抽取一个 aiMesh 的骨骼权重，填入与该 mesh 顶点等长的 out_weights。
/// bone_map / bone_counter 在多个 mesh 间共享（同名骨骼复用同一 id，
/// 处理「骨骼影响超出自身 mesh 范围」的情形——见 LearnOpenGL）。
/// @param vertex_count 该 mesh 的顶点数（out_weights 会被 resize 到此长度）
inline void extract_bone_weights(const aiMesh* ai_mesh,
                                 std::unordered_map<std::string, BoneInfo>& bone_map,
                                 int& bone_counter,
                                 std::vector<BoneWeights>& out_weights,
                                 unsigned int vertex_count) {
    out_weights.assign(vertex_count, BoneWeights{});

    for (unsigned int b = 0; b < ai_mesh->mNumBones; ++b) {
        const aiBone* ai_bone = ai_mesh->mBones[b];
        std::string bone_name = ai_bone->mName.C_Str();

        std::int32_t bone_id;
        auto it = bone_map.find(bone_name);
        if (it == bone_map.end()) {
            BoneInfo info;
            info.id = bone_counter;
            info.offset = ai_to_mat4_colmajor(ai_bone->mOffsetMatrix);
            bone_map[bone_name] = info;
            bone_id = bone_counter;
            ++bone_counter;
        } else {
            bone_id = it->second.id;
        }

        for (unsigned int w = 0; w < ai_bone->mNumWeights; ++w) {
            const aiVertexWeight& vw = ai_bone->mWeights[w];
            if (vw.mVertexId < vertex_count) {
                out_weights[vw.mVertexId].add_influence(bone_id, vw.mWeight);
            }
        }
    }
}

/// 递归复刻 aiNode 树到扁平 BoneNode 数组，返回新建节点的下标。
/// parent_index 为父节点在 skeleton.nodes 中的下标（根节点传 -1），
/// 供 IK 沿链从末端上溯到根使用。
inline int read_skeleton_hierarchy(const aiNode* ai_node, SkeletonData& skeleton,
                                   int parent_index = -1) {
    int index = static_cast<int>(skeleton.nodes.size());
    skeleton.nodes.emplace_back();
    // 注意：先 emplace 再递归子节点，递归会让 vector 重新分配，
    // 故子节点下标先收集到局部变量，最后统一赋值（避免悬挂引用）。
    {
        BoneNode& node = skeleton.nodes[index];
        node.name = ai_node->mName.C_Str();
        node.local = ai_to_mat4_colmajor(ai_node->mTransformation);
        node.parent = parent_index;
    }

    std::vector<int> child_indices;
    child_indices.reserve(ai_node->mNumChildren);
    for (unsigned int i = 0; i < ai_node->mNumChildren; ++i) {
        child_indices.push_back(read_skeleton_hierarchy(ai_node->mChildren[i], skeleton, index));
    }
    skeleton.nodes[index].children = std::move(child_indices);
    return index;
}

/// 抽取所有动画片段（aiAnimation → AnimationClip）。
inline void read_animations(const aiScene* ai_scene, std::vector<AnimationClip>& clips) {
    for (unsigned int a = 0; a < ai_scene->mNumAnimations; ++a) {
        const aiAnimation* ai_anim = ai_scene->mAnimations[a];
        AnimationClip clip;
        clip.name = ai_anim->mName.C_Str();
        clip.duration = static_cast<float>(ai_anim->mDuration);
        clip.ticks_per_second = ai_anim->mTicksPerSecond != 0.0
                                    ? static_cast<float>(ai_anim->mTicksPerSecond)
                                    : 25.0f;

        for (unsigned int c = 0; c < ai_anim->mNumChannels; ++c) {
            const aiNodeAnim* ch = ai_anim->mChannels[c];
            AnimChannel channel;
            channel.bone_name = ch->mNodeName.C_Str();

            channel.positions.reserve(ch->mNumPositionKeys);
            for (unsigned int k = 0; k < ch->mNumPositionKeys; ++k) {
                const auto& key = ch->mPositionKeys[k];
                channel.positions.emplace_back(
                    static_cast<float>(key.mTime),
                    std::array<float, 3>{key.mValue.x, key.mValue.y, key.mValue.z});
            }
            channel.rotations.reserve(ch->mNumRotationKeys);
            for (unsigned int k = 0; k < ch->mNumRotationKeys; ++k) {
                const auto& key = ch->mRotationKeys[k];
                // assimp 四元数为 (w, x, y, z)；存为 (x, y, z, w)
                channel.rotations.emplace_back(
                    static_cast<float>(key.mTime),
                    std::array<float, 4>{key.mValue.x, key.mValue.y, key.mValue.z, key.mValue.w});
            }
            channel.scales.reserve(ch->mNumScalingKeys);
            for (unsigned int k = 0; k < ch->mNumScalingKeys; ++k) {
                const auto& key = ch->mScalingKeys[k];
                channel.scales.emplace_back(
                    static_cast<float>(key.mTime),
                    std::array<float, 3>{key.mValue.x, key.mValue.y, key.mValue.z});
            }
            clip.channels.push_back(std::move(channel));
        }
        clips.push_back(std::move(clip));
    }
}

inline void process_assimp_mesh(aiMesh* ai_mesh, Scene& scene, std::uint32_t node_index,
                                const std::vector<std::uint32_t>& material_map,
                                const GlobalNormalizationParams& global_params,
                                const aiMatrix4x4& accumulated_transform,
                                const AssimpImportOptions& options = AssimpImportOptions{},
                                std::unordered_map<std::string, BoneInfo>* bone_map = nullptr,
                                int* bone_counter = nullptr) {
    if (ai_mesh->mNumVertices == 0 || ai_mesh->mNumFaces == 0) {
        CFW_LOG_WARNING("[Assimp] Mesh '{}' is empty, skipping", ai_mesh->mName.C_Str());
        return;
    }

    std::string mesh_name = ai_mesh->mName.C_Str();

    // 蒙皮网格判定：模型带骨架（bone_map 非空指针）且该 mesh 带骨骼。
    // 蒙皮分支保留「绑定姿态空间」——不烘焙节点累积变换、不做单位化归一化，
    // 因为 offset 矩阵假设顶点在 mesh 原始绑定空间；烘焙/归一化会让 offset 失配。
    const bool is_skinned_mesh =
        bone_map != nullptr && bone_counter != nullptr && ai_mesh->HasBones();

    // 提取法线变换矩阵（累积变换的逆转置的 3x3 部分）。
    // 蒙皮网格用单位变换（保持绑定空间），故法线矩阵也是单位。
    aiMatrix4x4 vertex_transform = is_skinned_mesh ? aiMatrix4x4() : accumulated_transform;
    aiMatrix3x3 normal_matrix(vertex_transform);
    normal_matrix.Inverse().Transpose();

    // 提取顶点数据
    std::vector<Vertex> unindexed_vertices(ai_mesh->mNumVertices);
    for (unsigned int i = 0; i < ai_mesh->mNumVertices; ++i) {
        // 静态网格变换到世界空间；蒙皮网格保持绑定空间（vertex_transform=单位）
        aiVector3D world_pos = vertex_transform * ai_mesh->mVertices[i];
        unindexed_vertices[i].position = {world_pos.x, world_pos.y, world_pos.z};

        if (ai_mesh->HasNormals()) {
            aiVector3D world_normal = normal_matrix * ai_mesh->mNormals[i];
            world_normal.Normalize();
            unindexed_vertices[i].normal = {world_normal.x, world_normal.y, world_normal.z};
        }
        if (ai_mesh->mTextureCoords[0]) {
            unindexed_vertices[i].tex_coords = {ai_mesh->mTextureCoords[0][i].x, ai_mesh->mTextureCoords[0][i].y};
        }
    }

    // 抽取骨骼权重（与 unindexed_vertices 等长并行）。仅蒙皮网格非空。
    std::vector<BoneWeights> unindexed_weights;
    if (is_skinned_mesh) {
        extract_bone_weights(ai_mesh, *bone_map, *bone_counter, unindexed_weights,
                             ai_mesh->mNumVertices);
    }
    const bool has_bones = !unindexed_weights.empty();

    // 提取索引数据
    std::vector<std::uint32_t> indices;
    indices.reserve(ai_mesh->mNumFaces * 3);
    for (unsigned int i = 0; i < ai_mesh->mNumFaces; ++i) {
        aiFace& face = ai_mesh->mFaces[i];
        for (unsigned int j = 0; j < face.mNumIndices; ++j) {
            indices.push_back(face.mIndices[j]);
        }
    }

    MeshOptimizeResult opt_result = optimize_mesh_pipeline(
        unindexed_vertices,
        indices,
        options.simplify_mesh,
        options.simplification_error,
        mesh_name,
        has_bones ? &unindexed_weights : nullptr);

    if (!opt_result.success) {
        CFW_LOG_WARNING("[Assimp] Mesh '{}' optimization failed, skipping", mesh_name);
        return;
    }

    // 确定材质索引
    std::uint32_t material_index = InvalidIndex;
    if (ai_mesh->mMaterialIndex < material_map.size()) {
        material_index = material_map[ai_mesh->mMaterialIndex];
    }

    // 使用全局参数进行归一化
    const std::array<float, 3>& center = global_params.center;
    float scale_factor = global_params.scale_factor;

    // 处理拆分或非拆分的情况。
    // sub_weights：与 vertices 等长并行的骨骼权重（蒙皮网格非空，已随 opt 同步 remap）。
    auto process_single_mesh = [&](std::vector<Vertex>& vertices,
                                   std::vector<std::uint32_t>& mesh_indices,
                                   const std::string& sub_mesh_name,
                                   std::vector<BoneWeights>& sub_weights) {
        if (vertices.empty()) return;
        const bool mesh_has_bones = !sub_weights.empty();

        // 计算 AABB
        std::array<float, 3> aabb_min, aabb_max;
        compute_aabb(vertices, aabb_min, aabb_max);

        // 归一化：仅静态网格。蒙皮网格保留绑定空间（offset 矩阵的前提），
        // 原始尺寸/位置改由 model transform 还原，故跳过单位化。
        if (!mesh_has_bones) {
            normalize_vertices_with_global_params(vertices, center, scale_factor);
            normalize_aabb(aabb_min, aabb_max, center, scale_factor);
        }

        // 转换索引为 uint16
        std::vector<std::uint16_t> final_indices =
            convert_indices_to_uint16(mesh_indices, vertices.size(), sub_mesh_name);

        (void)sub_mesh_name;

        // 构建 MeshData（蒙皮网格 original_scale_factor=1，center=0）
        MeshData mesh_data = build_mesh_data(
            std::move(vertices),
            std::move(final_indices),
            material_index,
            aabb_min,
            aabb_max,
            mesh_has_bones ? std::array<float, 3>{0.0f, 0.0f, 0.0f} : center,
            mesh_has_bones ? 1.0f : scale_factor);

        // 写入骨骼权重（蒙皮网格）
        if (mesh_has_bones) {
            mesh_data.bone_weights = std::move(sub_weights);
        }

        // 生成 LOD（蒙皮网格同步携带 bone_weights）
        if (options.lod_options.enabled) {
            mesh_data.lod_levels = generate_lod_levels(
                mesh_data.vertices,
                mesh_data.indices,
                options.lod_options,
                sub_mesh_name,
                mesh_data.aabb_min,
                mesh_data.aabb_max,
                mesh_has_bones ? &mesh_data.bone_weights : nullptr);
        }

        std::uint32_t mesh_idx = scene.add_mesh(std::move(mesh_data));

        // 只有第一个子网格绑定到当前节点
        if (scene.data.nodes[node_index].mesh_index == InvalidIndex) {
            scene.data.nodes[node_index].mesh_index = mesh_idx;
        } else {
            // 为后续子网格创建新节点（作为当前节点的子节点）
            std::uint32_t sub_node_index = scene.add_node(sub_mesh_name, node_index);
            scene.data.nodes[sub_node_index].mesh_index = mesh_idx;
            // 子网格使用单位变换（因为它们已经在父节点的坐标系中）
            scene.data.nodes[sub_node_index].transform = Transform{};
        }
    };

    if (opt_result.was_split) {
        // 处理拆分的子网格
        for (size_t i = 0; i < opt_result.sub_meshes.size(); ++i) {
            auto& sub_mesh = opt_result.sub_meshes[i];
            std::string sub_mesh_name = mesh_name + "_split" + std::to_string(i);
            process_single_mesh(sub_mesh.vertices, sub_mesh.indices, sub_mesh_name,
                                sub_mesh.bone_weights);
        }
    } else {
        // 处理单个网格（检查顶点数量）
        if (opt_result.vertices.size() > 65535) {
            CFW_LOG_ERROR("[Assimp] Mesh '{}' has {} vertices, which exceeds the uint16 limit. Skipping to prevent device loss.",
                          mesh_name, opt_result.vertices.size());
            return;
        }
        process_single_mesh(opt_result.vertices, opt_result.indices, mesh_name,
                            opt_result.bone_weights);
    }
}

inline Transform extract_assimp_transform(const aiMatrix4x4& m) {
    Transform transform;
    aiVector3D scaling, position;
    aiQuaternion rotation;
    m.Decompose(scaling, rotation, position);
    transform.position = {position.x, position.y, position.z};
    transform.scale = {scaling.x, scaling.y, scaling.z};
    aiMatrix3x3 rot_mat = rotation.GetMatrix();
    float sy = std::sqrt(rot_mat.a1 * rot_mat.a1 + rot_mat.b1 * rot_mat.b1);
    bool singular = sy < 1e-6f;
    float x, y, z;
    if (!singular) {
        x = std::atan2(rot_mat.c2, rot_mat.c3);
        y = std::atan2(-rot_mat.c1, sy);
        z = std::atan2(rot_mat.b1, rot_mat.a1);
    } else {
        x = std::atan2(-rot_mat.b3, rot_mat.b2);
        y = std::atan2(-rot_mat.c1, sy);
        z = 0;
    }
    static constexpr float rad_to_deg = 180.0f / 3.14159265f;
    transform.rotation = {x * rad_to_deg, y * rad_to_deg, z * rad_to_deg};
    return transform;
}

inline void process_assimp_node(aiNode* ai_node, const aiScene* ai_scene, Scene& scene,
                                std::uint32_t parent_index,
                                const std::vector<std::uint32_t>& material_map,
                                const GlobalNormalizationParams& global_params,
                                const aiMatrix4x4& parent_accumulated_transform = aiMatrix4x4(),
                                const AssimpImportOptions& options = AssimpImportOptions{},
                                std::unordered_map<std::string, BoneInfo>* bone_map = nullptr,
                                int* bone_counter = nullptr) {
    std::uint32_t node_index = scene.add_node(ai_node->mName.C_Str(), parent_index);
    scene.data.nodes[node_index].transform = extract_assimp_transform(ai_node->mTransformation);

    // 计算此节点的累积变换（父累积变换 * 当前节点局部变换）
    aiMatrix4x4 current_accumulated = parent_accumulated_transform * ai_node->mTransformation;

    for (unsigned int i = 0; i < ai_node->mNumMeshes; ++i) {
        unsigned int mesh_index = ai_node->mMeshes[i];
        aiMesh* ai_mesh = ai_scene->mMeshes[mesh_index];

        if (i == 0) {
            process_assimp_mesh(ai_mesh, scene, node_index, material_map, global_params, current_accumulated, options, bone_map, bone_counter);
        } else {
            std::string child_name = std::string(ai_node->mName.C_Str()) + "_mesh_" + std::to_string(i);
            std::uint32_t child_index = scene.add_node(child_name, node_index);
            scene.data.nodes[child_index].transform = scene.data.nodes[node_index].transform;
            process_assimp_mesh(ai_mesh, scene, child_index, material_map, global_params, current_accumulated, options, bone_map, bone_counter);
        }
    }

    for (unsigned int i = 0; i < ai_node->mNumChildren; ++i) {
        process_assimp_node(ai_node->mChildren[i], ai_scene, scene, node_index, material_map, global_params, current_accumulated, options, bone_map, bone_counter);
    }
}

inline void process_assimp_lights(const aiScene* ai_scene, Scene& scene,
                                  const std::unordered_map<std::string, std::uint32_t>& node_name_map) {
    for (unsigned int i = 0; i < ai_scene->mNumLights; ++i) {
        aiLight* ai_light = ai_scene->mLights[i];
        LightData light;
        switch (ai_light->mType) {
            case aiLightSource_POINT:
                light.type = LightData::LightType::Point;
                break;
            case aiLightSource_DIRECTIONAL:
                light.type = LightData::LightType::Directional;
                break;
            case aiLightSource_SPOT:
                light.type = LightData::LightType::Spot;
                light.inner_angle = ai_light->mAngleInnerCone * 180.0f / 3.14159265f;
                light.outer_angle = ai_light->mAngleOuterCone * 180.0f / 3.14159265f;
                break;
            case aiLightSource_AREA:
                light.type = LightData::LightType::Area;
                light.size = {ai_light->mSize.x, ai_light->mSize.y};
                break;
            default:
                continue;
        }
        light.color = {ai_light->mColorDiffuse.r, ai_light->mColorDiffuse.g, ai_light->mColorDiffuse.b};
        float intensity = (light.color[0] + light.color[1] + light.color[2]) / 3.0f;
        if (intensity > 0) {
            light.color[0] /= intensity;
            light.color[1] /= intensity;
            light.color[2] /= intensity;
            light.intensity = intensity;
        }
        auto light_index = static_cast<std::uint32_t>(scene.data.lights.size());
        scene.data.lights.emplace_back(light);
        auto it = node_name_map.find(ai_light->mName.C_Str());
        if (it != node_name_map.end()) {
            scene.data.nodes[it->second].light_index = light_index;
        }
    }
}

inline void process_assimp_cameras(const aiScene* ai_scene, Scene& scene,
                                   const std::unordered_map<std::string, std::uint32_t>& node_name_map) {
    for (unsigned int i = 0; i < ai_scene->mNumCameras; ++i) {
        aiCamera* ai_camera = ai_scene->mCameras[i];
        CameraData camera;
        camera.fov = ai_camera->mHorizontalFOV * 2.0f * 180.0f / 3.14159265f;
        camera.near_clip = ai_camera->mClipPlaneNear;
        camera.far_clip = ai_camera->mClipPlaneFar;
        camera.aspect_ratio = ai_camera->mAspect;
        auto camera_index = static_cast<std::uint32_t>(scene.data.cameras.size());
        scene.data.cameras.emplace_back(camera);
        auto it = node_name_map.find(ai_camera->mName.C_Str());
        if (it != node_name_map.end()) {
            scene.data.nodes[it->second].camera_index = camera_index;
        }
    }
}

}  // namespace Corona::Resource
