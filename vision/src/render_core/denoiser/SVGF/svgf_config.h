#pragma once

namespace vision::svgf {

struct SVGFConfig {

// Half precision (FP16) safety limits
struct HalfSafety {
    static constexpr float kMaxLuminance = 200.f;      // Prevent M2 overflow (200^2 * 9 < 65504)
    static constexpr float kMaxRadiance = 500.f;       // Prevent accumulation overflow
    static constexpr float kMinPositive = 6.2e-5f;     // Half min normal number
};

struct Epsilon {
    static constexpr float kGeometry = 1e-3f;          // Half-safe (was 1e-4f)
    static constexpr float kLuminance = 0.002f;        // Half-safe (was 0.001f)
    static constexpr float kWeight = 1e-3f;            // Half-safe (was 1e-4f)
    static constexpr float kVariance = 1e-4f;          // Half-safe (was 1e-6f)
};

    struct GeometryWeight {
        static constexpr float kEpsilon = Epsilon::kGeometry;
        static constexpr float kPrefilterNormalPower = 32.f;
        static constexpr float kPrefilterDepthScale = 0.1f;
        static constexpr float kAtrousNormalPowerDefault = 128.f;
        static constexpr float kAtrousDepthScaleDefault = 0.1f;
    };

    struct Modulator {
        static constexpr float kSoftEpsilon = 0.02f;
        // Composite glossy lobes (e.g. substrate) also carry textured diffuse
        // reflectance. Preserve that albedo in both channels rather than filtering
        // it as illumination. The soft floor keeps near-black guides invertible.
        static constexpr bool kDemodulateSpecular = true;
    };

    struct Temporal {
        // Bounded history for stationary 1-spp rendering; motion still raises alpha
        // and disocclusion rejects history rather than accumulating indefinitely.
        static constexpr float kDepthThreshold = 0.03f;    // tighter disocclusion reject (was 0.05)
        static constexpr float kAlbedoThreshold = 0.15f;
        static constexpr float kNormalExp = 128.f;
        static constexpr float kReSTIRNormalExp = 8.f;
        static constexpr float kNormalThreshold = 0.5f;
        static constexpr float kMaxHistoryStatic = 128.f;
        static constexpr float kMaxHistoryFast = 4.f;      // fast-motion alpha_min 1/4 (was 8)
        static constexpr float kMotionScaleDivisor = 16.f;
        static constexpr float kMotionAlphaScale = 0.5f;   // motion can reach alpha 0.5 (was 0.15)
        static constexpr float kMotionAlphaDivisor = 8.f;
        static constexpr float kFallbackMotionThreshold = 32.f;
        static constexpr float kFallbackPlaneThreshold = 0.005f;
        static constexpr float kFallbackMaxHistory = 16.f;
    };

    struct Resolve {
        // A stationary image must keep converging. A short, fixed EMA leaves a
        // permanent noise floor and makes jittered silhouettes slowly breathe.
        // This is only a numerical guard: FP32 represents every integer up to 2^24.
        static constexpr uint kHistoryPrecisionLimit = 1u << 24u;
        // Surface interiors retain responsive illumination history. Only pixels
        // near geometric coverage boundaries use the progressively longer mean.
        static constexpr uint kInteriorHistory = 32u;
        static constexpr float kNormalThreshold = 0.99f;
        static constexpr float kPlaneThreshold = 0.005f;
        static constexpr float kMovingAlpha = 0.25f;
        static constexpr float kMotionRejectPixels = 32.f;
        static constexpr float kMinReprojectionSupport = 0.1f;
    };

    struct Ghosting {
        static constexpr float kColorDiffThreshold = 0.7f;
    };

    struct Firefly {
        static constexpr float kSigmaMultiplierMin = 10.f;
        static constexpr float kSigmaMultiplierMax = 20.f;
        static constexpr float kMinSigma = 0.5f;

        static constexpr float kSpatialIsolationThreshold = 3.0f;
        static constexpr float kSpatialWeightIsolated = 0.7f;
        static constexpr float kSpatialWeightNormal = 0.3f;

        static constexpr float kSoftnessDefault = 2.0f;
        static constexpr float kSoftnessIndirect = 1.6f;
        static constexpr float kRetainRatio = 0.85f;

        static constexpr float kExtremeValueThreshold = 100.f;
        static constexpr float kExtremeValueScale = 0.1f;
    };

    struct Variance {
        static constexpr float kMinVarianceConsistent = 0.001f;   // Half-safe (was 0.0001f)
        static constexpr float kMinVarianceDisocclusion = 0.5f;
        static constexpr float kHistoryThreshold = 4.f;
        static constexpr float kDisocclusionBoost = 8.f;
    };

    struct Disocclusion {
        static constexpr float kDecayRate = 0.5f;
        static constexpr float kMaxVarianceMultiplier = 10.f;
        static constexpr float kMinVarianceFloor = 0.3f;
        static constexpr float kMaxBlendWeight = 0.95f;
    };

    struct Atrous {
        static constexpr float kBSpline1D[3] = {0.375f, 0.25f, 0.0625f};
        static constexpr float kMinPhi = 0.05f;
        static constexpr float kMinVariance = 0.001f;   // Half-safe (was 0.00005f)

        static constexpr uint kIterationCount = 4;
        static constexpr uint kStepSizes[4] = {1, 2, 4, 8};

        static constexpr uint kLargeStepThreshold = 4;
        // Correct temporal variance is much wider than the former accidental
        // floor. Keep mixed ReSTIR lighting responsive to real reflections.
        static constexpr float kReSTIRLPhiMultiplier = 0.25f;
        static constexpr float kLargeStepLPhiMultiplier = 1.4f;
        static constexpr float kLargeStepNPhiMultiplier = 0.85f;

        // Colour-history feedback (Schied 2017). kFeedbackEnabled is the master switch.
        // Disabled: it was added to mask noise in the OLD racy/single-buffered history by
        // compounding the spatial filter into history every frame. With the double-buffered
        // race-free history (P0-A) that compounding now just over-blurs and trails (each
        // frame re-filters an already-filtered history). The per-frame 4-iteration a-trous
        // still denoises the display output; we simply no longer feed it back. kFeedbackSpecular
        // controls whether the SPECULAR channel is fed back too (kept false; view-dependent).
        static constexpr bool kFeedbackEnabled = false;
        static constexpr bool kFeedbackSpecular = false;
    };

    struct PoissonDisk {
        static constexpr uint kSampleCount = 12;
        static constexpr float kGoldenAngle = 2.39996323f;
        static constexpr float kSamplesX[12] = {
            0.0f,
            -0.5f, 0.5f, -0.5f, 0.5f,
            -0.85f, 0.0f, 0.85f, 0.0f,
            -0.65f, 0.65f, -0.65f};
        static constexpr float kSamplesY[12] = {
            0.0f,
            -0.5f, -0.5f, 0.5f, 0.5f,
            0.0f, -0.85f, 0.0f, 0.85f,
            -0.65f, -0.65f, 0.65f};
        static constexpr float kWeights[12] = {
            1.0f,
            0.7f, 0.7f, 0.7f, 0.7f,
            0.4f, 0.4f, 0.4f, 0.4f,
            0.25f, 0.25f, 0.25f};
    };

    struct Prefilter {
        static constexpr float kHistoryBlendThreshold = 6.f;
        static constexpr float kMaxRadianceBlend = 0.8f;
        static constexpr float kLuminanceSigma = 3.5f;
        static constexpr float kFireflyHistoryThreshold = 2.f;
        static constexpr float kDisocclusionBlendBoost = 0.9f;
    };

    struct VarianceBlend {
        static constexpr float kHistoryThreshold = 6.f;
        static constexpr float kLumFloorScale = 0.08f;
        static constexpr float kMinSpatialWeight = 0.3f;
        static constexpr float kMaxSpatialWeight = 0.9f;
        static constexpr float kSoftTransitionStart = 1.f;
        static constexpr float kSoftTransitionEnd = 8.f;
    };

    struct AdaptiveRadius {
        static constexpr float kMinScale = 0.5f;
        static constexpr float kMaxScale = 1.5f;
        static constexpr float kVarianceScale = 5.f;
        static constexpr float kDirectAdaptiveStrength = 0.3f;
        static constexpr float kIndirectAdaptiveStrength = 0.7f;
    };

    struct Anisotropic {
        static constexpr bool kEnabled = true;
        static constexpr float kMaxAnisotropy = 3.0f;
        static constexpr float kGrazingAngleThreshold = 0.3f;
        static constexpr float kAnisotropyStrength = 0.8f;
        static constexpr bool kEdgeAwareEnabled = true;
        static constexpr float kEdgeAnisotropyBlend = 0.6f;
        static constexpr float kMinEdgeWeight = 0.1f;
    };
};

}// namespace vision::svgf
