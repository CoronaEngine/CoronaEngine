#pragma once

#include "vision_geometry_snapshot.h"
#include "base/mgr/image_pool.h"

namespace Corona::Systems::Vision {

// One successful source revision. Never retains a Pipeline, Mesh, material
// plugin or GPU texture; those remain owned by each algorithm runtime.
struct VisionSceneImportCache {
    ::vision::DataWrap project_data;
    std::shared_ptr<const VisionGeometrySnapshot> geometry;
    std::shared_ptr<::vision::ImagePool::SourceCache> images =
        std::make_shared<::vision::ImagePool::SourceCache>();
};

}
