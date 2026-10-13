# REBLUR shader implementation

This plugin implements full-resolution RGB diffuse/specular REBLUR in the same
`Kernel`, `OC_PARAM_STRUCT`, `device.compile` and `CommandBatch` shader API as PT
and SVGF. Select it explicitly; the existing SVGF and SSAT defaults are unchanged.

In the editor, select **Vision PT** or **Vision ReSTIR**, choose **REBLUR** in
the **降噪类型** menu, and check **降噪**. The main viewport and detached camera
windows both expose these controls. The type is saved per camera; **场景设置**
inherits the scene's denoiser and parameters. Changing type rebuilds that camera's
view and clears its history. SSAT retains its own fixed denoiser.

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
| max_history | 30 |
| fast_history | 6 |
| diffuse_prepass | 30 |
| specular_prepass | 50 |
| max_radius | 30 |
| stabilization | 1 |
| anti_firefly | true |

Checkerboard is OFF. Reconstruction defaults to OFF because producers provide
lobe distances and missing supported lobes receive independent guide rays.
The internal filter also implements 3x3 and 5x5 reconstruction. SH/SG, occlusion,
spectral rendering, deforming-object motion and dynamic resolution upsampling
are outside this RGB variant. Reduced spatial radii trade noise reduction for
detail; default settings require scene-level assessment for glossy reflections.
History limits are frame counts; this variant does not adapt settings to measured
frame duration. Legacy per-layer DirectIndirect diagnostic dumps do not expose
REBLUR's separate filtered lobe buffers; the evaluation below reads final PFMs.

## Validation

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
