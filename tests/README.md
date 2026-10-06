# Engine tests

This directory owns the tests for the engine code in `src/`:

- `kernel/`: kernel lifecycle and system management.
- `systems/<module>/`: tests owned by one engine system, mirroring `src/systems/<module>/`.
- `integration/`: collaboration, UI/display lifecycle, and embedded Vision workflows spanning systems.

Keep a test's private helper headers beside that test. Introduce `support/` when
multiple test suites need the same helper; do not move production headers into it.
Editor, Vision, and `modules/corona_resource` keep their own test directories.

Each test directory defines its targets in a local `CMakeLists.txt`. The root
build adds `tests/` after the production modules when `BUILD_CORONA_TESTING` or
`BUILD_TESTING` is enabled. Vision-dependent engine tests also require
`CORONA_BUILD_VISION`.

Configure and build with the existing `tests-<configuration>` or
`vision-tests-<configuration>` presets, then run CTest directly. CMake adds the
Slang, TBB, Python, CEF, and renderer runtime directories to each test's `PATH`
on Windows. CLion's **All CTest** uses the same settings; no manual Conan runtime
activation is needed. Reload CMake after changing this configuration.

```powershell
ctest --test-dir build/conan/tests/relwithdebinfo -C RelWithDebInfo --output-on-failure
ctest --test-dir build/conan/vision-tests/relwithdebinfo -C RelWithDebInfo --output-on-failure
```

Set `CORONA_RUN_GPU_SMOKE=1` to opt into the UI GPU smoke test on a machine with a
usable GPU and desktop session.

The shader source checks remain Python tests and can be run in an environment
with pytest installed:

```powershell
python -m pytest tests/systems/optics -q
```

Tests that need private engine headers add the owning production directory as a
`PRIVATE` include directory. Resource-dependent tests specify their working
directory explicitly; preserve those settings when moving a test.

## PT → ReSTIR switch profiling

Build `corona_vision_embedded_mode_switch_tests`, then run the bounded Windows
launcher from the engine root (64-bit Python, standard library only):

```powershell
python tools/profile_render_switch.py --scene ../CoronaExample/test_vision/render_scene/cbox/vision_scene.json --build-dir cmake-build-relwithdebinfo
```

The tool writes `report.html`, `report.json`, per-run stdout/stderr, launch/exit
metadata, and a Chrome/Perfetto `trace.json` under `build/switch-profile/<time>`.
Open the HTML for the distribution and expandable stage tree. Each process loads
PT, switches to ReSTIR, returns to PT, and switches to ReSTIR again. Preparation
and the first GPU-completed frame are timed separately. Nested stage durations
are not added twice. Normal rendering does not emit scopes unless
`CORONA_SWITCH_PROFILE=1`.

By default it runs two processes, preserving all caches: the first uses the
current disk cache, the second reuses what the first wrote. This is not a forced
cold-cache test. Same-process repeat timings measure live runtime/view reuse.
Use `--runs 1`, `--denoise`, `--timeout 300`, or `--output-dir <empty-directory>`
as needed. The total execution bound is the number of runs times the per-run
timeout plus up to 20 seconds cleanup per run. A failed process stops the batch
and still produces a partial report, with unfinished scopes explicitly marked.

`--runtime-dir <directory>` selects the DLL / `cuda` / `.cache` directory, default
`<build-dir>/bin`. Runtime dependency paths are read from the generated CTest
configuration. Use the same runtime directory when comparing runs: PTX cache
keys currently depend on absolute CUDA include paths. Profiling does not edit
the scene, delete caches, or infer OptiX driver-cache hits. Engine scopes cover
scene initialization, geometry, images, buffers, lights, view preparation and
DI/GI compilation. NVRTC per-shader times and PTX hits/misses come from existing
compiler logs. With the instrumented Ocarina backend built, the function table
also records cache I/O, `nvrtcCompileProgram`, `cuModuleLoadData`,
`optixModuleCreate`, `optixProgramGroupCreate`, `optixPipelineCreate`, and SBT
preparation. It separates inclusive and self time; do not sum inclusive parent
and child rows. Backend instrumentation lives in the Horizon workspace alongside
the existing renderer timing, so rebuild `ocarina-backend-cuda` as well.

`VisionSceneAssetReuseTests` checks first-time PT → ReSTIR creation with the
scene/model/texture files removed after the initial PT import. It covers both
embedded and file sources, textured geometry and area lights, independent
runtime state, failed and successful forced reloads, and recreation after all
GPU runtimes are retired. Use a bounded launcher with a 330-second outer limit
and CTest `--interactive-debug-mode 0 --timeout 300 --output-on-failure`.
Source revisions retain CPU geometry and decoded image data; switching modes
restores those assets without file I/O. Each runtime still owns its material
objects, GPU resources and histories. The cache is replaced on source reload
and released when the scene resource is discarded.
