> 注：文中引用的 `render_resource/manifest.json` 等是当时的临时产物，未随仓库保存。

# Mitsuba batch screenshots

Run from the repository root with the `coronaengine-dev` Python environment:

```powershell
python -B tools/render_mitsuba_scenes.py --scene cornell-box
```

Omit `--scene` to attempt every scene in `../render_resource/manifest.json`.
Repeat `--scene` or `--mode` to select a subset. The modes are `pt2048`,
`pt_reblur`, and `restir_reblur`. `--frames 8` is a short validation override.
Use `--help` for executable, resource, output and timeout options.

All screenshots are 1920×1080. PT uses 2048 accumulated 1-spp frames. The two
ReBLUR modes use 64 frames at 1 spp without outer accumulation; their steady
timings use frames 33–64. Each capture saves the engine PNG, all per-frame
timings in `frames.csv`, and a result JSON. Import failures and unsupported
features remain recorded in each scene result. The four unsupported hair
scenes retain their original geometry and are marked unsupported.

The tool calls `SceneTools.save_screenshot(scene, path, camera, options)`:

```python
options = {
    "render_frames": 64,
    "warmup_frames": 32,
    "timings_path": "/absolute/output/frames.csv",
    "timeout_seconds": 7200,
}
```

`render_frames > 0` requires the Vision backend and final-color output. It
resets history and renders exactly that many frames continuously, then uses
the existing viewport resolve, overlay and screenshot readback. The screenshot
and viewport publish the same final image. The tool performs no color conversion
or image re-encoding. Ordinary three-argument screenshots retain their behavior.
The CSV contains every frame; `warmup_frames` specifies the window excluded by
the batch tool when calculating steady timing averages. The output directory
must be empty, and the timing CSV's parent directory must already exist.

The render loop must call `Pipeline::upload_data()` before each subframe's
`display()`. At frame end, `Sensor::after_render()` advances the previous-camera
matrices on the CPU; the next upload publishes them for GPU history reprojection.
This is required even for a stationary camera in a newly created render context.

Before starting, the tool checks the NVIDIA driver's built-in thermal protection.
It retains the GPU/driver controls and adds no FPS cap, frame sleep, cooling wait
or temperature-based pacing. Temporary files and outputs live under `.workspace/`.

The old `.workspace/tmp/mitsuba-batch/` scripts are historical experiment
evidence. Their private capture/export and pacing code must not be reused.

Camera transport settings use a public total surface depth. With ReSTIR,
`max_depth: 16` includes its primary receiver and maps to 15 suffix vertices;
reading the settings returns 16. ReSTIR accepts total depths 1–128 and rejects
0 without changing the live settings. A scene descriptor with ReSTIR depth 0
is also rejected instead of underflowing the unsigned suffix budget. PT retains
its existing 0–128 range. The batch comparison keeps `max_depth: 16`,
`min_depth: 5`, and `rr_threshold: 1` for both integrators.
