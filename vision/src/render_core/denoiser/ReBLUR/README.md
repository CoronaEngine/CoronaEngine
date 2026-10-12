# REBLUR shader implementation

This plugin implements full-resolution RGB diffuse/specular REBLUR in the same
`Kernel`, `OC_PARAM_STRUCT`, `device.compile` and `CommandBatch` shader API as PT
and SVGF. Select it explicitly; the existing SVGF and SSAT defaults are unchanged.

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
layer; current guides and temporal metadata are shared. Storage and arithmetic
remain FP32. Persistent updates happen after all reprojection neighborhood reads.

Combined Substrate and Plastic materials expose both components to REBLUR.
Contribution routing uses the evaluated RGB BSDF ratio at the actual sampled
direction; guide sampling uses component support. Demodulation uses directional
Fresnel and diffuse attenuation in the appropriate lobe frame, including stable
prefix throughput. Delta-only hit distances continue through up to three
scattering segments, following the pinned RTXPT convention.

## Stages and reference

The stage order is Pack, ClassifyTiles, optional HitDistReconstruction, PrePass,
TemporalAccumulation, HistoryFix, Blur, PostBlur, TemporalStabilization, Unpack.
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
