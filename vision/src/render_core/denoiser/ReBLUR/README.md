# REBLUR shader implementation

This plugin provides full-resolution RGB diffuse/specular REBLUR. When built
with `VISION_REBLUR_NATIVE_NRD`, selecting REBLUR uses the Ocarina C++ EDSL
translation of NRD 4.14.3 by default. The adapter prepares NRD inputs and remodulates its
outputs; it does not add receiver/material/path identity rejection to NRD.
Material classification is zero, matching RTXPT's preparation pass. Each of the
three stable-plane denoisers retains its own NRD history.

The earlier 96-bit identity and exact-token shader experiments have been
removed from the native path. Their DLL override and token shader deployment
are no longer used. Native mode allocates current guides and NRD's pools,
without allocating or compiling the legacy custom filter's histories/scratch.
`REBLUR_NATIVE_NRD=1` explicitly selects the original HLSL validation reference;
unset or `0` selects the new original-algorithm Ocarina C++ EDSL port.
It implements the quality, full-resolution RGB diffuse/specular variant, including
world-space specular Blur/PostBlur, original history repair and surface/virtual
reprojection. GPU passes use native R8/RG8 UNORM, R16/RGBA16 float, R16/R32 uint,
R32 float and RGB10A2 textures with hardware clamp sampling. NRD.dll remains the
CPU settings/constants/resource scheduler; the EDSL path does not dispatch its
HLSL shaders. Builds without native NRD retain the older custom filter. Existing
SVGF and SSAT selections are unchanged.

The same-input replay target is `corona_vision_reblur_replay`. Set
`REBLUR_REPLAY_BACKEND=native` or `edsl` and use identical
`REBLUR_REPLAY_WIDTH/HEIGHT` (defaults 1280/720). It uploads immutable inputs once,
runs three independent histories with 64 warmup and 32 measured frames, and
downloads outputs after measurement. `REBLUR_REPLAY_OUTPUT_DIR` optionally saves
the final RGBA32F buffers. `tools/reblur_compare_replay.py` validates the input,
settings and camera fingerprints before comparing per-pass GPU timings;
`tools/reblur_compare_outputs.py` compares the saved outputs. A sum of pass queries
is reported separately from a continuous GPU interval.

`corona_vision_reblur_edsl_tests` checks typed resource behavior and compares eight
frames across three layers at 1x1, 31x19 and 127x73, including a rotated camera,
fractional motion and invalid guides. Format-store, matrix and sampler probes
test the lower-level contracts separately. The numerical tolerance is fixed at
`0.001 + 0.002 * abs(HLSL)` per component. Compiler floating-point differences can
cross nearest-tap or history-footprint boundaries; the test reports those as
failures, rather than treating equal algorithm structure as numerical equivalence.

Validation on 2026-10-22 (RTX 3070 Ti, Vulkan) passes all 72 small-sequence
comparisons and the full 96-frame 720p/1080p replay at that unchanged tolerance.
Hit-distance outputs are bit identical; RGB retains small rounding differences
within tolerance. The final three-layer continuous NRD GPU intervals are
3.987/4.139 ms at 720p and 8.776/9.179 ms at 1080p (HLSL/EDSL), excluding
Pack/Unpack. Complete filter intervals including conversion are 5.287/5.461 ms
and 11.637/12.147 ms. Per-pass sums are recorded separately and must not replace
these continuous measurements. Full evidence and reproduction commands are in
`build/recordings/reblur-performance-20261021/optimization-20261021/final-report/`.

The broader `corona_vision_reblur_tests` constant/HDR regressions also pass.
Their first differing stage was HistoryFix: Slang reassociated the normalized
mean's square into the raw sum's square times the squared scale. This changed
near-constant variance and crossed FP16 output boundaries. HistoryFix now
rounds both the 72-tap anti-firefly mean and the 25-tap local mean before
squaring, matching the original shader's FP32 operation order for both lobes.
At 127x73, frame 1, layer 1, pixel (8,25), constant specular B now matches
0.198364258 instead of 0.199829102. HDR diffuse G at layer 0, pixel (1,4)
now matches 9968 instead of 9992 for input RGB (50000,10000,30000).

The complete original stress suite, 72-case oracle and fresh 96-frame
720p/1080p paired replays pass the unchanged component tolerance. Both stress
captures' first two frames have 86/86 identical stage textures/constants.
Constant-layer isolation still uses three separate original NRD instances;
invalid pixels retain exact raw-bit passthrough for both backends. Tests and
the original HLSL reference are unchanged. Reproduction, before/after snapshots,
generated instruction checks and binary fingerprints are in
`build/recordings/reblur-stress-fix-20261022/`.

The retained optimizations use hardware typed textures, dispatch-uniform resource
indices, lazy read-only parameter loads and an NRD-only dynamic constant UBO,
plus matched shared-memory layouts and selective HistoryFix unrolling. They
preserve the resource ABI and filtering settings. Three explicit scalar rounding
points retain Native's normalized-mean-before-square behavior in TA and TS;
Slang's reassociation otherwise changed variance, history counters and subsequent
frames. The native reference also serializes its reset clear dispatches to prevent
late clears from overwriting the first frame. Diagnostic chronology and moment
probes used to establish these causes have been removed.

In the editor, select **Vision PT** or **Vision ReSTIR**, choose **REBLUR** in
the **降噪类型** menu, and check **降噪**. The main viewport and detached camera
windows both expose these controls. The type is saved per camera; **场景设置**
inherits the scene's denoiser and parameters. Changing type rebuilds that camera's
view and clears its history. SSAT retains its own fixed denoiser.

When REBLUR is selected, click **REBLUR 参数** beside the denoiser selector in
either viewport. The panel exposes all 17 filter settings: temporal history and
repair, diffuse/specular prepass and blur radii, geometry/roughness tolerances,
stabilization, anti-lag, firefly suppression and hit-distance reconstruction.
**应用** updates that camera on its next render and resets its temporal history
without rebuilding the view. **恢复默认值** only resets the panel draft; click
**应用** to use it. Enabling denoising remains a separate checkbox.

Settings are saved per camera in `vision_reblur_settings` and restored with the
scene. Unspecified fields inherit the source scene's REBLUR descriptor. The panel
loads effective renderer values plus pending camera overrides; changes in one
camera do not affect other cameras. Invalid ranges and inconsistent history or
radius bounds are rejected before a change is queued.

Parameter UI validation (2026-10-13): the canonical CLion engine/frontend build,
four related CTest entries (including live PT/ReSTIR parameter updates), 74
focused frontend tests and 15 Vision preference/archive tests passed. Browser
interaction with the actual component and a CEF-shaped mock verified loading,
editing and applying all 17 values. The full frontend suite has one pre-existing
`dockCommand` timeout, reproduced on pristine HEAD. Evidence is stored under
`cmake-build-relwithdebinfo/reblur-evidence/reblur-ui-*`.

```json
{
  "render": {
    "integrator": {
      "type": "pt",
      "param": { "denoiser": { "type": "ReBLUR" } }
    }
  },
  "output": { "denoise": true }
}
```

Use `"type": "rt"` for ReSTIR. Its DI/GI producers split contributions into
diffuse/specular RGB before denoising. Emission and primary sky bypass illumination
filtering. DI/GI switches are applied before filtering and composition applies
them once. Three stable-plane layers have independent geometry and illumination
histories and share scratch buffers. Final coverage uses SVGF's coverage-only
mode, which does not allocate or execute its illumination filters.
Only previous-frame geometry, illumination and consumed metadata persist per
layer; current guides and temporal metadata are shared. Normalized YCoCg signals,
fast/stabilized luminance histories and frame counts use `half4`/`half2` storage.
Shaders promote them to Float before filtering, reprojection or moment arithmetic;
writes saturate to the signed FP16 range so negative chroma is preserved.
Geometry, motion, virtual coordinates, curvature and world-space hit distances
remain FP32, as do the raw producer and final output buffers. Persistent updates
happen after all reprojection neighborhood reads.

Combined Substrate and Plastic materials expose both components to REBLUR.
Contribution routing uses the evaluated RGB BSDF ratio at the actual sampled
direction; guide sampling uses component support. Demodulation uses directional
Fresnel and diffuse attenuation in the appropriate lobe frame, including stable
prefix throughput. Delta-only hit distances continue through up to three
scattering segments, following the pinned RTXPT convention.

## Stages and reference

The native backend dispatches the original NRD stage descriptions without
changing their shader arithmetic, sampling budgets or filtering rules. The
translation details and historical evaluations below describe the legacy
custom implementation unless explicitly labeled native NRD.

The stage order is ClassifyAndPack, optional HitDistReconstruction, PrePass,
TemporalAccumulation, HistoryFix, Blur, PostBlur and TemporalStabilization.
The final stage also stores the post-blur history, updates geometry and
remodulates output, preserving the FP16 roundtrip of the former Unpack stage.
Surface and virtual specular reprojection have separate confidence and histories.
Slow and fast luminance histories drive history repair, antilag and stabilization.
Resize, frame gaps, branch/receiver changes and enable switches discard invalid
history. The screen-space spatial sampling variant is used.

The algorithm reference is NVIDIA NRD 4.14.3 at
`16a64cd4daebfb153590623f0348b36007284ba3`, as included by RTXPT at
`21ef566cb37ef832438375fb3b5bd0b170e52f7c`. The upstream license in
[LICENSE-NRD.txt](LICENSE-NRD.txt) accompanies the translation. This is an engine
shader implementation, not binary or bitwise compatibility with the NRD SDK.

## Settings

Set these under `denoiser.param`. Distances supplied by producers are in world
units; only the internal filter uses NRD normalized hit distances and YCoCg.

| Setting | Default |
| --- | ---: |
| max_history | 50 |
| fast_history | 6 |
| history_fix_frames / history_fix_stride | 3 / 14 |
| diffuse_prepass | 15 |
| specular_prepass | 40 |
| min_radius | 1 |
| max_radius | 30 |
| lobe_fraction / roughness_fraction | 0.15 / 0.15 |
| plane_sensitivity / min_hit_weight | 0.02 / 0.1 |
| stabilization | 63/64 |
| antilag_sigma / antilag_sensitivity | 4 / 3 |
| anti_firefly | true |
| reconstruction | AREA_5X5 |

These defaults match RTXPT's `NrdConfig::getDefaultREBLURSettings()` preset.
The public lobe fraction is squared at dispatch, matching NRD's constant setup.
Checkerboard is OFF. Producers provide lobe distances and independent guide rays
for missing supported lobes; 5x5 reconstruction is enabled by the matched preset.
The internal filter also implements OFF and 3x3 reconstruction. SH/SG, occlusion,
spectral rendering, deforming-object motion and dynamic resolution upsampling
are outside this RGB variant. Reduced spatial radii trade noise reduction for
detail; default settings require scene-level assessment for glossy reflections.
History limits are frame counts. Antilag sensitivity and stabilization clipping
use NRD's `max(33.333 / frame_delta_ms, 1)` frame-rate scale. Legacy per-layer
DirectIndirect diagnostic dumps do not expose
REBLUR's separate filtered lobe buffers; the evaluation below reads final PFMs.

## Validation

The native-only test mode is `REBLUR_TEST_BACKEND=native`. It checks guide-only
allocation, switching storage modes, constants, independent layers, resize,
invalid-pixel passthrough, HDR and finite outputs. It does not impose the removed
full 96-bit identity contract on NRD. Numerical tolerances are unchanged; the
previously observed constant-signal precision failure is reported separately
from performance results, and later cases still run after an assertion fails.

`VisionReblurTests` executes the actual filter shaders and imported PT/ReSTIR
scenes. It covers color and distance packing, colored lobe ownership, constant
signals over 64 frames, depth boundaries, virtual motion, stochastic pixel
jitter, layer isolation, empty layers, resets, 1x1 and 127x73 sizes, DI/GI switches,
glass branches, and enabling the plugin after raw rendering.
Smooth compound materials and per-lobe shading frames are tested explicitly.
An allocation budget guards the three-layer filter's storage size. GPU cases
cover FP16 overflow saturation, signed chroma, HDR through 65504, and world-space
tracking distances above the FP16 range.
Sparse partial tiles exercise both lobes and all reconstruction modes, including
empty-to-visible transitions and a lone valid pixel at the last tile lane.
Signed packed motion is checked against the actual temporal tracking position.

Run related regressions in the configured CLion build:

```powershell
ctest --test-dir cmake-build-relwithdebinfo -R 'VisionReblurTests|VisionEmbeddedModeSwitchTests|VisionRenderModeConfigTests|VisionReSTIR|VisionShaderBindingTests' --output-on-failure
```

For fixed scene comparisons, install NumPy in the evaluation Python environment
and point `CORONA_EXAMPLE` at the existing CoronaExample asset checkout:

```powershell
$env:CORONA_EXAMPLE = 'D:/work/corona/CoronaExample'
python vision/tools/reblur_eval.py --build-dir cmake-build-relwithdebinfo --manifest tests/fixtures/reblur/scenarios.json --output-dir cmake-build-relwithdebinfo/reblur-evidence
```

The manifest controls asset paths, camera, resolution and trajectory. Static
runs save raw/SVGF/REBLUR and a 2048-sample PT reference; moving runs save selected
frames for visual inspection. PNGs, linear PFM files, camera/accumulation
metadata, commands, logs, ROI errors and synchronized integrator stage timings
are retained. Realtime runs explicitly disable framebuffer accumulation and
check the saved metadata. A finite-sample reference is not a proof of convergence;
moving runs do not report RMSE against a different camera pose. Timing reports
group the denoiser and coverage together; they do not isolate individual REBLUR
passes or guide-ray cost.

### Recorded run: 2026-10-12

CLion RelWithDebInfo, GeForce RTX 3070 Ti (8 GiB), driver 610.88. The engine
build and all eight related CTest entries passed. The Kitchen manifest was
rendered at 320x240, one sample per frame, 64 realtime frames, against its
2048-sample PT reference. Full-image linear RGB RMSE (lower is better):

| Producer | Raw | SVGF | REBLUR |
| --- | ---: | ---: | ---: |
| PT | 1.28762 | 0.71490 | 0.68731 |
| ReSTIR, three stable layers | 2.48090 | 0.35981 | 0.34086 |

These are scene-specific results, not a general quality advantage. PT's glossy
kettle ROI is worse with REBLUR (0.10875 versus SVGF's 0.07058); ReSTIR's tile/edge
ROI is also worse (0.08651 versus 0.07972). Metal surfaces retain grain, some
reflections are softened, and the kettle is darker than the reference. The
96-frame camera sequence shows softer detail during movement and recovery
after stopping; its nine saved frames are finite. There is no moving-pose
reference or quantitative ghosting claim.

At 320x240, average frame times for SVGF/REBLUR are 1.536/1.816 ms with PT and
4.686/5.817 ms with ReSTIR. The denoiser-plus-coverage stage accounts for
0.686/0.841 ms and 1.100/2.013 ms respectively. Timing uses eight warmup frames
and sixteen profiled frames with synchronized stages; it is not an isolated
GPU timestamp benchmark. Individual pass timings were not measured.

At 1280x720, three-layer ReSTIR takes 35.476 ms/frame with SVGF and
44.177 ms/frame with REBLUR (8.961/15.610 ms denoising plus coverage).
The REBLUR run peaks at 4322 MiB device-wide in the sampled memory report.
Single-layer PT at 1920x1080 takes 26.189/32.433 ms/frame for SVGF/REBLUR;
REBLUR denoising plus coverage takes 16.680 ms.

At 1920x1080, three-layer ReSTIR remains limited by memory on this 8 GiB GPU:
REBLUR averages 582.966 ms/frame, including 35.192 ms denoising plus coverage,
versus 77.135 ms/frame for SVGF. The allocation audit reports 7.08 GiB of buffers
and 57.71 MiB of textures for the complete REBLUR scene, before driver/desktop
overhead. A 200 ms `nvidia-smi` poll reaches 7904 MiB device-wide, including the
desktop. Other rendering stages also slow sharply, consistent with paging.
The compact history layout saves about 500 MiB relative to the preceding
full-history layout, but does not make this scene realtime at 1080p on 8 GiB.
No precision reduction or unrelated stable-plane storage rewrite was introduced.

The retained local evidence is under
`cmake-build-relwithdebinfo/reblur-evidence/compact-validation`: report.json,
each method's scene/pose/metrics/log files, PNGs and linear PFMs. All 38 PFMs,
including raw/SVGF and the moving sequence, are byte-identical to the earlier
accumulation-checked `validated` baseline; `storage-comparison.json` records this
check. Performance runs are in `performance-720p`, `performance-compact` and
`performance-1080p`. Build/test logs and the local reproduction helper are in
`validation`. All performance PFMs were checked for finite values.

### FP16 storage validation: 2026-10-13

The existing Ocarina `half`/`Half` types now store REBLUR's normalized radiance,
fast/stabilized histories and frame counts. Filtering and moment calculations
remain Float. Geometry, motion, curvature, world-space tracking and the producer
interfaces remain FP32; the global `VS_HALF_RADIANCE` setting is not changed.

The three-layer filter allocates 408 bytes/pixel plus tile flags, down from 556.
The allocation regression measures 94,006,880 versus 128,106,080 bytes at 640x360.
That is a 26.6% reduction and saves 292.68 MiB at 1920x1080. This measures the
filter's allocations, not all ReSTIR resources or device-wide memory use.

The canonical CLion engine build and all eight related CTest entries passed.
The GPU tests cover signed saturation at +/-65504, bright constant signals,
negative YCoCg chroma, 100,000-unit tracking distances, long histories, layers,
reprojection, resize and actual PT/ReSTIR producers. At 320x240, static output
differs from the preceding FP32 baseline by 0.420% relative L2 for PT and 0.209%
for ReSTIR. Full-image RMSE against the same 2048-sample reference changes from
0.687307 to 0.687336 for PT and 0.340864 to 0.340873 for ReSTIR. The ReSTIR
tile/edge ROI changes from 0.086514 to 0.087109. The nine moving-frame snapshots
remain finite, with at most 0.752% relative L2 difference from FP32. Quantization
is not bitwise equivalence; these results cover the Kitchen fixture only.

The same 8-warmup/16-profile-frame, non-accumulating performance cases were rerun:

| ReSTIR mode | FP32 frame ms | FP16 frame ms | FP32 denoise/coverage ms | FP16 denoise/coverage ms |
| --- | ---: | ---: | ---: | ---: |
| 720p, three layers | 45.440 | 47.669 | 16.111 | 14.928 |
| 1080p, three layers | 655.285 | 1896.030 | 48.601 | 47.066 |
| 1080p, stable planes off | 58.861 | 61.097 | 16.931 | 15.148 |
| 720p, stable planes off | 26.984 | 27.983 | 7.392 | 6.516 |

These synchronized stage timings include guide preparation and coverage, and
the sequential runs had different desktop GPU memory use. They do not establish
an overall frame-time improvement. Both 1080p three-layer runs reached about
7950 MiB device-wide, and the later run was slower in stable-plane construction
and sampling despite the smaller filter. The complete pipeline's memory pressure
remains unresolved; converting REBLUR storage alone does not make it realtime.

Evidence is retained under `cmake-build-relwithdebinfo/reblur-evidence/` in
`2026-10-13-half-quality`, `2026-10-13-half-performance` and
`2026-10-13-half-validation.json`. The earlier comparison timings are in
`2026-10-13-restir-recheck`. All twelve quality PFMs and four performance PFMs
were checked for finite values; static ReSTIR and a moving frame were also
visually compared with their FP32 images.

### Filter optimization: 2026-10-13

The filter now uses native SOA guides/history, packs motion into unused factor
components, shares diffuse/specular neighborhood loads, skips empty tiles, and
fuses classification/packing and stabilization/output/history stores. Guide
preparation exits before material evaluation for invalid secondary planes.
Sampling, filter settings, arithmetic precision and FP16 storage semantics are
unchanged. The three-layer allocation is now 392 bytes/pixel plus tile flags
(90,320,480 bytes at 640x360; about 775.23 MiB at 1920x1080).

On the same RTX 3070 Ti, Kitchen at 1280x720, three layers, one sample/frame and
16 warmup plus 32 measured frames, CUDA events report 8.634 ms before versus
3.734 ms after for the filter passes summed across all layers (-56.7%). Guide
preparation is separate (2.237 versus 1.485 ms). The temporary profiler records
events on the dispatch stream and resolves them at existing synchronization
points; it has been removed from the runtime. Fresh RTXPT/NRD runs report
1.721 ms for its denoiser, so parity has not been achieved.

Without that profiler, two 720p runs average 37.463 ms/frame for Corona versus
43.944 ms for RTXPT, with process dedicated peaks of 3601 and 2163 MiB.
At 1080p Corona remains unstable (580 and 1536 ms/frame; 6976 MiB peak), while
RTXPT averages 95.678 ms/frame and 3411 MiB. These are process memory peaks,
not allocated resource totals. Geometry/camera, resolution and path-depth caps
are aligned; the renderers' materials, estimators and NRD settings differ.
The 1080p results do not establish an overall speedup or prove paging.

The canonical engine build and eight related CTest entries passed. Sparse-tile
tests cover full/sparse/full transitions, both lobes, partial tiles and every
hit-distance reconstruction mode; another test checks packed nonzero motion.
All 12 quality PFMs (PT, ReSTIR and camera motion) and all four performance PFMs
are byte-identical to the pre-optimization FP16 baseline. Evidence, profiler
source/binary, commands and raw measurements are retained in
`cmake-build-relwithdebinfo/reblur-evidence/2026-10-13-reblur-optimization/`;
`summary.json` is the aggregate report.

### Pipeline memory follow-up: 2026-10-13

An allocation audit of the optimized filter at 1920x1080 found 6.763 GiB of
buffers and 57.71 MiB of textures. The largest allocations were outside the
filter (MiB below measures allocated bytes, not WDDM residency):

| Resources | Before | After |
| --- | ---: | ---: |
| Three current stable planes and their geometry history | 2752.73 | 2467.97 |
| Two dominant surface/extension records | 1075.78 | 1012.50 |
| ReSTIR reservoirs, samples and radiance | 783.11 | 783.11 |
| REBLUR filter | 775.23 | 775.23 |
| Unused SHARC placeholder | 176.00 | 0.00 |

Keeping aligned vectors together removes padding: `SurfaceExtend` changes
80 -> 64 bytes, `StablePlaneData` 336 -> 304, and `StablePlaneHistory` 128 -> 112.
All fields, defaults and precision are preserved. The dominant double buffers
plus three current/history planes drop from 1936 to 1760 bytes/pixel, saving
348.047 MiB at 1080p. ReSTIR never invokes the cache's transport operations,
so it no longer prepares the unused four SHARC allocations (176 MiB).
Total buffer allocation is now 6.251 GiB, an exact reduction of 524.047 MiB.

The reference RTXPT stores 96-byte stable-plane records and 64-byte packed
surface records, versus our larger full-precision records and separate plane
history/lobe buffers. Its packed normals, material properties and radiance have
different contracts; these sizes explain part of the remaining memory gap and
are not interchangeable structures. Kitchen geometry/acceleration accounts for
only 83.39 MiB in our allocation audit.

The immediate before/after run changes from 579.482 to 78.134 ms/frame, with
byte-identical linear output. Two subsequent independent runs, alternating
Corona/RTXPT order and using the same comparison setup, give:

| Renderer | 720p frame ms | 1080p frame ms | 720p process dedicated peak MiB | 1080p process dedicated peak MiB |
| --- | ---: | ---: | ---: | ---: |
| Corona | 36.329 | 78.275 | 3267.26 | 6793.27 |
| RTXPT | 44.194 | 96.006 | 2163.92 | 3410.54 |

Corona's two 1080p repeats are 78.097/78.453 ms. They no longer show the severe
slowdown observed above, although memory consumption remains substantially
higher than RTXPT. WDDM shared usage also decreases (426 to 106 MiB in the
immediate comparison); a direct page-fault trace was not captured. Allocated,
committed and dedicated-resident memory are distinct measurements: the exact
524 MiB allocation reduction need not equal the sampled dedicated-peak change.

The full canonical engine build and eight relevant CTest entries pass. The
storage-budget test was first observed failing at 1936 bytes/pixel and now
passes at 1760; the real ReSTIR fixture also rejects idle SHARC allocations.
All 12 quality PFMs and four performance PFMs remain byte-identical to their
baselines. Temporary audit code has been removed. This changes shared structure
layouts, so validation uses rebuilt modules/shaders in fresh processes; mixing
old and new modules through a live hot reload is not covered.

The same evidence directory contains `memory-summary.json`, detailed allocation
lists and review/build/test logs in `memory-audit/`, `memory-quality/`, and the
`memory-corona-*` / `memory-rtxpt-*` measurement runs. These results retain the
renderer/settings comparison limitations described above.

### REBLUR-only comparison: 2026-10-13

Fresh measurements after the pipeline memory fix isolate the three filter
layers. Same Kitchen/camera, RTX 3070 Ti, native 1 spp, 16 warmup + 32 measured
frames, two serial runs per renderer/resolution with alternating order:

| Resolution | Corona filter GPU ms | RTXPT/NRD GPU ms | Corona history/scratch MiB | NRD permanent/transient MiB |
| --- | ---: | ---: | ---: | ---: |
| 1280x720 | 3.736 | 1.713 | 274.23 | 184.58 |
| 1920x1080 | 8.446 | 3.709 | 617.02 | 415.31 |

These memory columns use the same boundary: history and temporary work storage,
excluding current input guides, radiance input/output, coverage, driver heap
alignment, shader code and small constant/descriptor allocations. NRD numbers
come from querying the exact RTXPT runtime `NRD.dll` (4.14.3) with one
`REBLUR_DIFFUSE_SPECULAR` instance and multiplying its pools by the three
independently initialized integrations. Texture extents follow RTXPT's actual
`NrdIntegration::Initialize` calculation. Corona uses the audited buffer sizes.

The previously reported 775.23 MiB includes the 158.20 MiB current `Guide`;
removing that input gives 617.02 MiB. At 1080p the breakdown is:

| Storage | Corona MiB | RTXPT/NRD MiB |
| --- | ---: | ---: |
| Persistent history, three layers | 474.61 | 249.17 |
| Temporary work storage | 142.41 | 166.14 |
| Current input geometry guides (separate from pools) | 158.20 | 31.64 |
| Diffuse/specular radiance input/output | 189.84 | 126.56 |

NRD additionally allocates a 1.98 MiB disocclusion-mix input at 1080p. The
radiance row counts Corona's three in-place float4 lobe pairs, and RTXPT's one
reused half4 input pair plus three output pairs. Producer bypass radiance and
optional debug/validation outputs are excluded. These integration resources
have different contracts; their totals should not be confused with process
dedicated memory or the whole ReSTIR pipeline.

Per layer, Corona retains 48 bytes/pixel of geometry and 32 bytes of signal
history, versus NRD's 10 + 32 bytes. Thus the main internal memory difference is
geometry, while Corona saves some scratch by sharing it across layers. The
internal memory ratio is 1.486x; time ratios are 2.181x at 720p and 2.277x at 1080p.

Timing excludes guide preparation, ReSTIR-lobe merge/composition and coverage.
Corona sums events around its seven filter dispatches per layer; RTXPT times
`RunDenoiserPasses`. Corona's fused pack/remodulation work is inside that filter
boundary, while RTXPT performs corresponding conversion in prepare/merge
passes. Guide preparation alone is 1.302/2.547 ms (720p/1080p) for Corona and
0.261/0.584 ms for RTXPT. Filter settings and material implementations still
differ, so this is a current-implementation comparison, not equal-quality data.

Evidence is in `cmake-build-relwithdebinfo/reblur-evidence/2026-10-13-reblur-only/`:
`comparison.json`, the runtime NRD pool query/script, eight benchmark runs with
raw timestamps and commands, and cleanup/build logs. All four profiled Corona
PFMs are byte-identical to the preceding memory-optimized runs. No filter or
quality settings were changed for this comparison.

### Glossy history confidence: 2026-10-14

The spatial normal angle and temporal view angle now use the unit-direction
half-angle expression `2 * atan2(length(a-b), length(a+b))`. Unlike
`acos(dot(a,b))`, it returns exactly zero for identical FP32 directions. A CUDA
probe found false static angles in 1656/4096 normalized directions; the narrow
lobe's surface confidence fell as low as 0.233. The actual filter helper test
failed on identical glossy normals before this change and now passes, including
real 0.0001-radian motion and opposite directions.

Virtual reflection-history confidence now includes the pixel footprint:
`footprint = sqrt(lobe_pixels*lobe_pixels + 0.25)` and
`confidence = footprint / (footprint + displacement_pixels)`. This soft weight
avoids over-rejecting stochastic subpixel hit-distance changes. It does not
replace geometry/identity rejection or guarantee zero history at a reflected
disocclusion. No storage, scene parameters, history limits or sampling budgets
changed.

Frozen-parameter evaluation covered Kitchen, Staircase, Staircase2, Coffee,
Spaceship and Bathroom2 at 960x540, 1 spp/frame without accumulation. Each scene
has a 128-frame static run and a 244-frame run containing 64 preroll frames and
180 frames from its existing smooth camera path. PT references use 2048 spp at
the matched static/mid-motion and end-motion poses. Average display-RMSE changes
over the two moving glass views are -1.60% (Staircase2), -3.74% (Coffee) and
-7.41% (Spaceship); their linear-RMSE changes are -0.55%, -1.28% and -3.02%.
The other three scenes' selected glossy regions are essentially unchanged.
All six static full-frame display-RMSE changes are within 0.08%. Residual glass
noise remains visible; these results do not establish PT-equivalent quality.

A fast/slow-history-driven radius experiment was rejected: it reduced Coffee
highlight fluctuations but increased Staircase2's static glossy-wood linear
RMSE by 6.62%. The retained change only modifies the two confidence formulas.
The canonical CLion engine build and rebuilt `VisionReblurTests` pass. Evidence,
commands, frozen scenes, raw renders, PT references and ablations are in
`build/recordings/glossy-denoiser-20261014/`; `before`, `after-angle`, `after` and
`after-adaptive` denote the original, angle-only, retained and rejected variants.

Native 1920x1080 static checks reuse the previously captured, parameter-matched
Coffee/Spaceship baselines and PT references (eight frames, 120–127). Full-frame
display/linear RMSE changes are +0.24/+0.36% for Coffee and +0.03/+0.05% for
Spaceship. Coffee glass changes are +0.61/+0.72%, while its highlight region
improves -0.36/-0.19%. Thus the benefit is predominantly in moving reflections,
with a small static Coffee-glass regression. The final rebuilt Coffee motion
output is byte-identical to the retained candidate; the tested and deployed
REBLUR DLL hashes also match. `report.html` and `validation.json` collect the
results and source/binary hashes.

### Residual glossy pixel footprint: 2026-10-14

Layer readback of Coffee and Spaceship found mature specular histories but
almost no spatial support on curved glass with roughness 0.01. In the sampled
Spaceship glass layers, more than 99% of immediate normal neighbours exceeded
the narrow-lobe angular cutoff. The post-blur correction combines the optical
lobe with a pixel footprint instead of increasing global radius or history.

For roughness r, support is `1-smooth01(linear_step(0.02,0.1,r))`. The radius
floor is `min(min_radius,1)*support` pixels. The angular footprint is twice the
length of the minimum opposing one-pixel normal slopes, capped at 0.2 radians
and scaled by support. Identity and plane-distance rejection still apply.
Secondary hit-distance rejection is relaxed only for offsets within one pixel;
wider taps retain the original hit test.

Added weights are protected separately from the original filter. Let A/B be
fast demodulated luminance and R_A/R_B their raw samples. The edge factor is
`E = 1/(1+3*d^2+9*d^4)`, where
`d = (A-B)/(0.2*(A+B)+abs(R_A-A)+abs(R_B-B)+1e-6)`.
The raw-to-fast differences are a noise heuristic, not a calibrated variance.
Final weight is `base + max(expanded-base,0)*E`; base is zero if the radius
floor moved the integer tap. This prevents the roughness taper from leaking
unguarded colour across a narrow highlight. No new buffers, settings, samples,
history frames, or temporal reprojection formulas were added. Saved scene
parameter files remain unchanged.

GPU regression reproduces missing support (first-frame MSE 0.0625) and reduces
it to 0.0424627. After 64 frames, curved one-pixel highlight peaks are
3.99219/3.98828/3.98633 for roughness 0.01/0.03/0.04 (target 4); the planar
case is 3.73242. Neighbours stay within 0.00977 of background 1. The translated
planar reflection has peak 3.73828 and old-location luminance 1.00879 with the
same surface identity. Branch disocclusion discards the mature bright history.
Existing storage, HDR, invalid-tile, zero-radius and layer-isolation checks pass.

All six frozen scenes use 960x540, 1 spp/frame, no framebuffer accumulation,
128 static frames and the same 244-frame smooth paths as the previous section.
Coffee glass display/linear RMSE changes are -12.25/-1.43% static,
-5.80/-4.89% at the middle moving view, and -10.05/-7.14% at the end.
Spaceship changes are -5.50/-1.99%, -4.20/-0.61%, and -7.61/-2.38% respectively.
Static temporal standard deviation falls 25.09%/12.82%. Selected highlight
ROIs are essentially unchanged. Staircase2 static glass improves 3.59% in
display RMSE; Kitchen, Staircase and Bathroom2 are essentially unchanged.
Coffee/Spaceship 540p motion frame times are 17.19/22.05 ms versus 16.85/21.80 ms
in the frozen baselines; these are individual runs, not isolated benchmarks.

Native 1920x1080 static validation uses parameter- and pose-matched baselines
and 2048-spp PT references. Coffee glass display/linear RMSE decreases
16.13/11.31%, with temporal standard deviation down 30.56%. Spaceship decreases
6.82/3.54%, with temporal standard deviation down 17.21%. Highlight-region errors
are essentially unchanged. Coffee's native glass edge-contrast projection
decreases 0.069; fine-edge softness remains despite improved overall error.

Stopping and cutting the camera improves glass RMSE at all seven sampled
frames, but Coffee glass edge contrast drops roughly 0.02-0.05 in the gradient
projection metric. Old-view correlation also increases by up to 0.024; that
metric includes spatial/brightness bias and is not a pure ghosting measure.
Residual noise remains: this is not PT-equivalent quality or a guarantee of
zero ghosting for arbitrary motion.

Canonical CLion engine build and GPU tests are recorded in
`build/recordings/glass-residual-20261014/`. The retained variant is `guarded`;
`pixel-local` was rejected despite lower noise because Coffee static linear
error increased 16%. A read-only review caught and verified the fix for
taper-related highlight leakage. `report.html`, `validation.json`,
`scores-guarded.json`, `native-scores.json`, and `temporal-scores-guarded.json`
contain native-resolution comparisons, exact jobs, limitations and hashes.
Temporary layer readback has been removed from production code.
