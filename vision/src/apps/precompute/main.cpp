//
// Created by ling.zhu on 2025/1/31.
//

#include <iostream>
#include "base/cli_parser.h"
#include "base/import/project_desc.h"
#include "core/stl.h"
#include "base/mgr/pipeline.h"
#include "rhi/context.h"
#include "core/image/image.h"
#include "core/util/logging.h"
#include "base/denoiser.h"
#include "base/mgr/global.h"
#include "math/basic_types.h"
#include "base/mgr/global.h"
#include "base/import/importer.h"
#include "rhi/stats.h"

using namespace ocarina;
using namespace vision;

struct App {
    Device device;
    SP<Pipeline> rp{};
    App() : device(RHIContext::instance().create_device()) {
        Global::instance().set_device(&device);
        fs::path file_path = parent_path(__FILE__, 1) / "precompute.json";
        rp = Importer::import_scene(file_path);
        Global::instance().set_pipeline(rp);
        rp->upload_bindless_array();
    }

    int run() {

        MaterialRegistry::instance().precompute_albedo();

        return 0;
    }
};

int main(int argc, char *argv[]) {
    App app;
    return app.run();
}