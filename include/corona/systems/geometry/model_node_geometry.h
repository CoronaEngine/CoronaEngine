#pragma once
#include <corona/shared_data_hub.h>
#include <algorithm>

namespace Corona::ModelNodes {
inline ktm::fmat4x4 ktm_matrix(const Matrix& source) { ktm::fmat4x4 m=ktm::fmat4x4::from_eye(); for(int c=0;c<4;++c)for(int r=0;r<4;++r)m[c][r]=source[c*4+r]; return m; }
inline std::optional<Matrix> inverse_matrix(const Matrix& source) {
    if (!finite(source)) return std::nullopt;
    double rows[4][8]{};
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) rows[r][c] = source[c * 4 + r];
        rows[r][r + 4] = 1;
    }
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::abs(rows[r][c]) > std::abs(rows[pivot][c])) pivot = r;
        if (std::abs(rows[pivot][c]) < 1e-20) return std::nullopt;
        for (int k = 0; k < 8; ++k) std::swap(rows[c][k], rows[pivot][k]);
        const double divisor = rows[c][c];
        for (double& value : rows[c]) value /= divisor;
        for (int r = 0; r < 4; ++r) {
            if (r == c) continue;
            const double factor = rows[r][c];
            for (int k = 0; k < 8; ++k) rows[r][k] -= factor * rows[c][k];
        }
    }
    Matrix result{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) result[c * 4 + r] = static_cast<float>(rows[r][c + 4]);
    return finite(result) ? std::optional<Matrix>(result) : std::nullopt;
}

inline bool update_instances(GeometryDevice& geometry) {
    // Imported instances already encode normalization, or are identity because
    // the vertices were baked/skinned. A node's raw world matrix is neither.
    // Stage updates so a singular/invalid edit cannot partly change the model.
    struct Update { std::size_t index; ktm::fmat4x4 transform; bool visible; };
    std::vector<Update> updates;
    updates.reserve(geometry.mesh_instances.size());
    for (std::size_t i = 0; i < geometry.mesh_instances.size(); ++i) {
        const auto& instance = geometry.mesh_instances[i];
        if (instance.node_index >= geometry.model_nodes.size()) continue;
        const auto& node = geometry.model_nodes[instance.node_index];
        if (!finite(node.world)) return false;
        auto transform = instance.original_transform;
        if (node.world != node.world_base) {
            Matrix changed;
            if (geometry.has_node_instance_prefix) {
                changed = multiply(geometry.node_instance_prefix, node.world);
            } else {
                // Baked/skinned vertices already contain the base pose. Apply
                // only the edit delta, expressed in their normalized space.
                const auto inverse_base = inverse_matrix(node.world_base);
                const auto inverse_prefix = inverse_matrix(instance.node_delta_prefix);
                if (!inverse_base || !inverse_prefix) return false;
                changed = multiply(multiply(multiply(instance.node_delta_prefix,
                    node.world), *inverse_base), *inverse_prefix);
                Matrix original{};
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r) original[c * 4 + r] = instance.original_transform[c][r];
                changed = multiply(changed, original);
            }
            if (!finite(changed)) return false;
            transform = ktm_matrix(changed);
        }
        updates.push_back({i, transform, node.state.effective_visible});
    }
    for (const auto& update : updates) {
        auto& instance = geometry.mesh_instances[update.index];
        instance.transform = update.transform;
        instance.visible = update.visible;
    }
    geometry.model_node_bounds.reset();
    ++geometry.model_node_revision;
    return true;
}
inline bool apply_overrides(GeometryDevice& g) {
    for(const auto& o:g.model_node_overrides) { if(o.node_index>=g.model_nodes.size() || g.model_nodes[o.node_index].state.node_path!=o.node_path) return false; auto state=g.model_nodes[o.node_index].state; if(o.position)state.position=*o.position; if(o.rotation)state.rotation=*o.rotation; if(o.scale)state.scale=*o.scale; if(o.visible)state.visible=*o.visible; if(!set(g.model_nodes,o.node_index,state))return false; }
    return update_instances(g);
}
inline std::optional<std::array<float,6>> bounds(const GeometryDevice&, std::uint32_t) { return std::nullopt; }
}
