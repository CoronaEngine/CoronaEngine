# RELAX in Ocarina

Implementation in progress on `feat/relax`, based directly on `svgf_magic` at
`f8a5ceb33f201e748ad69bcdfc7ab3afebc24371`.

The algorithm reference is NVIDIA NRD 4.14.3 at
`16a64cd4daebfb153590623f0348b36007284ba3`, as included by RTXPT at
`21ef566cb37ef832438375fb3b5bd0b170e52f7c`.
Source notices and [LICENSE-NRD.txt](LICENSE-NRD.txt) accompany the translation.
The target is full-resolution RGB diffuse/specular RELAX using the engine's
`Kernel`, `OC_PARAM_STRUCT`, `device.compile` and `CommandBatch` APIs.

The front end retains linear RGB and **world-space hit distances**. It does not
normalize distances. Internal temporal moments and spatial variance are separate
from input distances. The 12-tap Catmull-Rom sampler follows NRD's omitted-corner
footprint and normalization. Ocarina's interpolation order is `lerp(t,a,b)`.

Current validated scope: front-end sanitization, demodulation, motion conversion,
and point/bilinear/12-tap bicubic buffer sampling on the CUDA backend. The plugin
and complete filter are not yet available; validation results will be updated as
the remaining passes and producers are integrated.
