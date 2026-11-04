# ReBLUR CPU scheduler

This directory contains the CPU portion needed by CoronaEngine's EDSL
`REBLUR_DIFFUSE_SPECULAR` implementation. It is adapted from NVIDIA NRD 4.14.3
(20 March 2025), with the necessary MathLib 1.3 CPU headers. These source files
are part of this repository. Building this static target does not download or
link an external NRD SDK, NRD DLL, NVRHI, graphics headers, shader compiler, or
precompiled GPU shader.

`vision-reblur-scheduler` exposes the existing `nrd` interfaces through
`include/NRD.h`. It registers only `REBLUR_DIFFUSE_SPECULAR`; other denoiser
requests return `UNSUPPORTED`. Independent identifiers retain separate histories
and support Corona's three stable planes. Original resource pools, ping-pong
updates, camera/common constants, reconstruction OFF/3x3/5x5, prepass, history
fix, anti-firefly, and temporal stabilization scheduling are retained.

GPU filtering remains in Corona's `edsl_*.cpp` kernels. Pipeline descriptions
retain the original names, resource ranges, and constant layouts, with empty
DXBC/DXIL/SPIR-V records. `src/CpuResourceLayouts.h` and `CpuReblurConfig.h`
contain only CPU constant records and group dimensions extracted from the
matching NRD resource declarations, not shader implementations. The MathLib
`ml.hlsli` file is used through its C++ branch by the original CPU calculations.

The current target matches the existing x86-64 build. It uses C++17 and MSVC's
precise floating-point mode without an AVX/FMA override, matching the supplied
reference CPU build settings. The independent CPU test compares dispatch
selection, pools, resource bindings, and all 832 constant bytes against the
checked-in golden fixtures. It does not load a reference DLL. GPU and whole
render validation remain required after integration.

`ORIGIN.json` records hashes of the supplied source snapshot and imported files.
The retained code changes remove other denoisers and all embedded shader data,
replace shader include paths with the CPU declarations here, and limit the
supported-method list. The common settings, resource allocation, and ReBLUR
constant calculations otherwise retain their upstream expressions and order.

NRD's license is in `LICENSE-NRD.txt`; the MathLib license is in
`math/LICENSE.txt`. File-level notice comments are omitted from the local ReBLUR implementation
sources. Original MathLib source headers remain intact. Complete license texts
and upstream provenance are retained; this cleanup does not change the upstream
license obligations.
