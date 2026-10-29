#pragma once

// Test fixtures construct renderer objects in memory. Production Vision has no
// scene-file importer; mesh fixtures use Corona's ordinary resource importer.
#include <corona/resource/resource_manager.h>
#include <corona/resource/types/scene.h>
#include "vision_test_config.h"
#include "base/mgr/global.h"
#include "base/mgr/pipeline.h"
#include "base/mgr/scene.h"
#include "base/shape.h"
#include <stdexcept>

namespace Corona::Tests {

inline auto make_vision_test_pipeline(const vision::DataWrap& configuration,
                                      const std::filesystem::path& asset_root = std::filesystem::current_path()) {
    using namespace vision;
    ProjectDesc desc;
    configure_vision_test_project(desc, configuration);
    desc.scene_path = asset_root;
    Global::instance().set_scene_path(asset_root);
    auto shapes = std::move(desc.scene_desc.shape_descs);
    desc.scene_desc.shape_descs.clear();
    auto pipeline = Node::create_shared<Pipeline>(desc.pipeline_desc);
    pipeline->init_project(desc);
    auto& scene = pipeline->scene();
    for (const auto& shape : shapes) {
        if (shape.sub_type != "model") {
            scene.add_shape(Node::create_shared<ShapeGroup>(shape), shape);
            continue;
        }
        Resource::ModelImportOptions options;
        options.scene_geometry = true;
        options.flip_uvs = shape["flip_uv"].as_bool(true);
        auto& manager = Resource::ResourceManager::get_instance();
        const auto id = manager.import_sync(asset_root / shape["fn"].as_string(), options);
        auto resource = manager.acquire_read<Resource::Scene>(id);
        if (!resource) throw std::runtime_error("Test mesh failed to load through Corona");
        auto group = std::make_shared<ShapeGroup>(shape);
        for (uint32_t m = 0; m < resource->data.meshes.size(); ++m) {
            vector<Vertex> vertices;
            for (const auto& source : resource->get_mesh_vertices(m)) {
                Vertex vertex;
                vertex.pos = {source.position[0], source.position[1], source.position[2]};
                vertex.n = {source.normal[0], source.normal[1], source.normal[2]};
                vertex.uv = {source.tex_coords[0], source.tex_coords[1]};
                vertex.uv2 = {0.f, 0.f};
                vertices.push_back(vertex);
            }
            vector<Triangle> triangles;
            const auto& indices = resource->get_mesh_indices(m);
            if (indices.size() % 3) throw std::runtime_error("Non-triangle test mesh");
            for (size_t i = 0; i < indices.size(); i += 3)
                triangles.emplace_back(indices[i], indices[i + 1], indices[i + 2]);
            group->add_instance(ShapeInstance(std::make_shared<Mesh>(std::move(vertices), std::move(triangles))));
        }
        group->post_init(shape);
        scene.add_shape(group, shape);
    }
    pipeline->init_postprocessor(desc.renderer_desc.denoiser_desc);
    return pipeline;
}

} // namespace Corona::Tests
