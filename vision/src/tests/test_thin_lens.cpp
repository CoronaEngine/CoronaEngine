#include "base/import/importer.h"
#include "base/mgr/pipeline.h"
#include "base/mgr/scene.h"
#include "base/scattering/sample.h"
#include "rhi/context.h"
#include <fstream>
#include <iostream>

using namespace vision;
using namespace ocarina;

namespace {

bool nearly_equal(float lhs, float rhs, float eps = 1e-4f) {
    return std::abs(lhs - rhs) <= eps;
}

bool nearly_equal(float2 lhs, float2 rhs, float eps = 1e-4f) {
    return nearly_equal(lhs.x, rhs.x, eps) &&
           nearly_equal(lhs.y, rhs.y, eps);
}

bool nearly_equal(float3 lhs, float3 rhs, float eps = 1e-4f) {
    return nearly_equal(lhs.x, rhs.x, eps) &&
           nearly_equal(lhs.y, rhs.y, eps) &&
           nearly_equal(lhs.z, rhs.z, eps);
}

void expect(bool cond, const char *message) {
    if (!cond) {
        std::cerr << "[test-thin-lens] " << message << std::endl;
        std::exit(1);
    }
}

void expect_nearly_equal(float lhs, float rhs, const char *message, float eps = 1e-4f) {
    expect(nearly_equal(lhs, rhs, eps), message);
}

void expect_nearly_equal(float2 lhs, float2 rhs, const char *message, float eps = 1e-4f) {
    expect(nearly_equal(lhs, rhs, eps), message);
}

void expect_nearly_equal(float3 lhs, float3 rhs, const char *message, float eps = 1e-4f) {
    expect(nearly_equal(lhs, rhs, eps), message);
}

fs::path write_test_scene() {
    const fs::path dir = fs::temp_directory_path() / "vision-test-thin-lens";
    fs::create_directories(dir);
    const fs::path scene = dir / "scene.json";
    const char *json = R"JSON(
{
  "scene": {
    "camera": {
      "type": "thin_lens",
      "param": {
        "fov_y": 45.0,
        "focal_distance": 5.0,
        "lens_radius": 0.25,
        "transform": {
          "type": "matrix4x4",
          "param": {
            "matrix4x4": [
              [1.0, 0.0, 0.0, 0.0],
              [0.0, 1.0, 0.0, 0.0],
              [0.0, 0.0, 1.0, 0.0],
              [0.0, 0.0, 0.0, 1.0]
            ]
          }
        },
        "filter": {
          "type": "box"
        }
      }
    },
    "materials": [
      {
        "name": "mat_diffuse",
        "type": "diffuse",
        "param": {
          "color": [0.7, 0.7, 0.7]
        }
      }
    ],
    "mediums": {
      "process": false,
      "list": []
    },
    "shapes": [
      {
        "type": "quad",
        "name": "ground",
        "param": {
          "width": 2.0,
          "height": 2.0,
          "material": "mat_diffuse",
          "transform": {
            "type": "matrix4x4",
            "param": {
              "matrix4x4": [
                [1.0, 0.0, 0.0, 0.0],
                [0.0, 1.0, 0.0, 0.0],
                [0.0, 0.0, 1.0, 0.0],
                [0.0, -1.0, -3.0, 1.0]
              ]
            }
          }
        }
      }
    ],
    "lights": []
  },
  "render": {
    "sampler": {
      "type": "independent"
    },
    "integrator": {
      "type": "pt",
      "param": {
        "max_depth": 2,
        "min_depth": 0
      }
    },
    "light_sampler": {
      "type": "uniform",
      "param": {}
    },
    "spectrum": {
      "type": "srgb"
    },
    "render_setting": {
      "polymorphic_mode": 1,
      "min_world_radius": 1.0,
      "ray_offset_factor": 1.0
    }
  },
  "pipeline": {
    "type": "fixed",
    "param": {
      "frame_buffer": {
        "type": "normal",
        "param": {
          "resolution": [32, 32],
          "tone_mapper": {
            "type": "linear"
          },
          "upsampler": {
            "type": "bilateral"
          }
        }
      },
      "rasterizer": {
        "type": "cpu"
      }
    }
  },
  "output": {
    "fn": "test.png",
    "spp": 1,
    "save_exit": 0,
    "denoise": false
  }
}
)JSON";
    std::ofstream out(scene, std::ios::binary);
    out << json;
    out.close();
    return scene;
}

void verify_device_ray_generation(Device &device, TSensor sensor) {
    Stream stream = device.create_stream();
    Buffer<float3> ray_origins = device.create_buffer<float3>(2, "test_thin_lens_origins");
    Buffer<float3> ray_directions = device.create_buffer<float3>(2, "test_thin_lens_directions");

    Kernel kernel = [&](BufferVar<float3> origins, BufferVar<float3> directions) {
        Uint index = dispatch_id();
        SensorSample sample;
        sample.time = 0.f;
        sample.p_film = make_float2(16.5f, 16.5f);
        $if(index == 0u) {
            sample.p_lens = make_float2(0.5f, 0.5f);
        } $else {
            sample.p_lens = make_float2(1.f, 0.5f);
        };
        RayState ray_state = sensor->generate_ray(sample);
        origins.write(index, ray_state.ray->origin());
        directions.write(index, ray_state.ray->direction());
    };

    auto shader = device.compile(kernel, "test_thin_lens_gpu");
    vector<float3> host_origins(2, make_float3(0.f));
    vector<float3> host_directions(2, make_float3(0.f));

    stream << shader(ray_origins, ray_directions).dispatch(2u);
    stream << ray_origins.download(host_origins.data());
    stream << ray_directions.download(host_directions.data());
    stream << synchronize() << commit();

    expect_nearly_equal(host_origins[0], make_float3(0.f),
                        "GPU center lens sample should keep origin at camera center");
    expect_nearly_equal(host_directions[0], make_float3(0.f, 0.f, -1.f),
                        "GPU center film sample should keep direction along -Z", 1e-3f);
    expect_nearly_equal(make_float2(host_origins[1].x, host_origins[1].y), make_float2(-0.25f, 0.f),
                        "GPU lens sample should move ray origin onto lens disk", 1e-3f);
    expect(host_directions[1].z < -0.9f,
           "GPU thin lens ray should still point forward after aperture offset");
}

}// namespace

int main(int argc, char *argv[]) {
    auto device = RHIContext::instance().create_device();
    device.init_rtx();
    Global::instance().set_device(&device);

    const fs::path scene_path = write_test_scene();
    expect(fs::exists(scene_path), "temporary test scene should exist");

    Global::instance().set_scene_path(scene_path.parent_path());
    auto pipeline = Importer::import_scene(scene_path.string());
    expect(pipeline != nullptr, "import_scene should create a pipeline");

    pipeline->init();
    pipeline->prepare();

    auto sensor = pipeline->scene().sensor();
    expect(sensor.get() != nullptr, "pipeline scene should create a sensor");
    expect(sensor->impl_type() == string_view{"thin_lens"}, "sensor impl_type should be thin_lens");
    expect(sensor->category() == string_view{"sensor"}, "sensor category should be sensor");
    expect_nearly_equal(sensor->fov_y(), 45.f, "thin_lens fov_y should match scene json");
    expect_nearly_equal(sensor->position(), make_float3(0.f), "thin_lens position should stay at origin");
    expect_nearly_equal(sensor->forward(), make_float3(0.f, 0.f, -1.f), "thin_lens forward should face -Z");

    sensor->set_pitch(120.f);
    expect_nearly_equal(sensor->pitch(), Sensor::pitch_max, "pitch should clamp to Sensor::pitch_max");
    sensor->set_pitch(-120.f);
    expect_nearly_equal(sensor->pitch(), -Sensor::pitch_max, "pitch should clamp to -Sensor::pitch_max");
    sensor->set_pitch(0.f);

    sensor->set_fov_y(200.f);
    expect_nearly_equal(sensor->fov_y(), Sensor::fov_max, "fov_y should clamp to Sensor::fov_max");
    sensor->set_fov_y(1.f);
    expect_nearly_equal(sensor->fov_y(), Sensor::fov_min, "fov_y should clamp to Sensor::fov_min");
    sensor->set_fov_y(45.f);

    verify_device_ray_generation(device, sensor);
    return 0;
}
