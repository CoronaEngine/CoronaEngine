# GI-only native StandardBSDF

This module embeds the model from the source snapshot recorded by
`.workspace/experiments/rtxpt-nee-off-quality-20261108/build-record.json`.
`source_manifest.json` records the original paths and SHA-256 values. All 14
vendored shader/configuration files match that proven native build record.
The original NVIDIA notices and `LICENSE.txt` are retained.

`original/` is the unchanged source reference. `generate_literal.py` needs only
these checked-in files, never another RTXPT checkout or an absolute source path.
Run it to regenerate `standard_bsdf.slangh` and its C++ embedding `literal.h`.
The HLSL model's function bodies are retained. Whole-file inclusion removes
include directives; three utility functions and the DeltaLobe type are extracted
without translating their code. Original CRLF is normalized in generated text;
a Windows-1252 apostrophe in a comment is converted to UTF-8.

The only method adaptation is a Slang-only `[mutating]` signature annotation on
native setters and `FalcorBSDF::__init`. HLSL permits mutable `this` implicitly.
The annotations leave the native mathematical bodies unchanged. The generated
types/functions/constants use namespace `crtxpt_gi_native`; bridge functions use
the `crtxpt_gi_bsdf_` prefix. Native macros are undefined after the include.

The fixed settings match the measured native executable: FP32 `lpfloat`,
Frostbite diffuse, correlated Smith masking, GGX BVNDF, recycled three-component
samples, enabled delta lobes, uncompressed StandardBSDFData and separate
diffuse/specular evaluation. The actual model is `FalcorBSDF`, used by the native
`StandardBSDF` wrapper. Its ShadingData constructor route is preserved.

`../rtxpt_gi_standard_bsdf.h` implements the model with typed DSL expressions.
The embedded source in this directory is retained as an offline reference;
the current typed GI path does not register it as a runtime shader header.
The reference embedding uses small raw-string chunks to respect MSVC's
individual string-token limit.

Both directions point away from the surface; localView is native `wi`, and
localScatter is native `wo`. F includes the scattering cosine. Sampling retains
native component weight, mixture PDF and lobe probability. A valid delta sample
has a zero continuous PDF, so path transport must preserve it while continuous
GI proposals reject its delta lobe. `standard_lobes` reproduces the native
data-only `getLobes`; active header flags separately control initialization of
sampling probabilities.

`probe.hlsl` has no renderer dependency. It compiles with both DXC and the
project's Slang 2026.10 (SPIR-V 1.5, row-major) and also supports a Slang C++ CPU
target. Its outputs include a bitwise mismatch mask against direct original
native calls. This probe is never part of a rendering kernel unless explicitly
used by a validation harness.

The private stable-plane build uses the typed StandardMaterial helpers. Current
GI candidate tracing instead runs the complete ordinary camera PT and captures
sampled Principled parameters in a quantized96B receiver record. Temporal,
spatial and final GI evaluation decode that record and use the complete
Principled model. Ordinary PT and Falcor DI do not install the GI parameter
capture/decode scope. The native reference files remain useful as offline
oracles for the typed helpers.
