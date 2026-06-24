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
        static constexpr float kAtrousDepthScaleDefault = 1.0f;
    };

    struct Modulator {
        static constexpr float kSoftEpsilon = 0.1f;
        // Demodulation strategy is now chosen at runtime by RealTimeDenoiseInput::channel_kind
        // (the producer declares whether the buffers are diffuse/specular or direct/indirect),
        // NOT by kDualSignal. In the diffuse/specular case the diffuse channel is demodulated by
        // DIFFUSE albedo and the specular channel is left in radiance space by default
        // (kDemodulateSpecular=false): a highlight is a directional spike not proportional to
        // specular reflectance, and dividing by tiny F0 would re-create fireflies. In the
        // direct/indirect case both channels are demodulated by the full surface albedo.
        static constexpr bool kDemodulateSpecular = false;
    };

    struct Temporal {
        // P1 (anti-ghosting): the previous values leaned far too hard on temporal
        // accumulation (48-frame history, alpha capped at 0.15 under motion) to hide
        // noise the weak spatial filter could not remove. After the P0 a-trous rewrite
        // (dense 4-iteration B-spline) the spatial filter carries its weight, so history
        // is shortened and motion response opened up to kill smearing/trailing.
        static constexpr float kDepthThreshold = 0.03f;    // tighter disocclusion reject (was 0.05)
        static constexpr float kAlbedoThreshold = 0.15f;
        static constexpr float kNormalExp = 128.f;
        static constexpr float kNormalThreshold = 0.5f;
        static constexpr float kMaxHistoryStatic = 32.f;   // long clean history; ghosting handled by HistoryClamp (was 16/48)
        static constexpr float kMaxHistoryFast = 4.f;      // fast-motion alpha_min 1/4 (was 8)
        static constexpr float kMotionScaleDivisor = 16.f;
        static constexpr float kMotionAlphaScale = 0.5f;   // motion can reach alpha 0.5 (was 0.15)
        static constexpr float kMotionAlphaDivisor = 8.f;
    };

    // NRD/ReLAX-style temporal history color clamping (anti-ghosting).
    // Reprojected history is clamped to the current frame's local luminance box
    // [mean - kSigmaScale*sigma, mean + kSigmaScale*sigma] computed over a small
    // neighborhood. This decouples anti-ghosting from history length, so a long,
    // clean history can be used for noise reduction without trailing/smearing.
    struct HistoryClamp {
        static constexpr float kSigmaScale = 1.5f;   // tightened from 2.0 (ReLAX default): with race-free
                                                     // history (double-buffered) trailing on high-contrast
                                                     // edges is now the dominant artifact; lower = stronger
                                                     // anti-ghost, more flicker risk.
        static constexpr int kRadius = 1;            // 3x3 neighborhood
    };

    // Input anti-firefly clamp (NRD-style). Specular highlights leave isolated
    // single-pixel luminance spikes that SVGF cannot remove. Clamp the current pixel's
    // luminance to max(neighbourMax, mean + kSigmaScale*sigma) over the neighbours
    // (centre excluded) BEFORE it pollutes moments/history. The neighbourMax floor
    // protects real multi-pixel highlights; only lone outliers above the local max are
    // reined in. kSigmaScale is generous on purpose to avoid dimming highlights.
    struct InputFirefly {
        static constexpr float kSigmaScale = 4.0f;
        static constexpr int kRadius = 1;            // 3x3 neighborhood
    };

    struct Ghosting {
        static constexpr float kColorDiffThreshold = 0.7f;
        static constexpr float kBrightHistoryRatio = 50.f;
        static constexpr float kBrightHistoryMinLum = 5.0f;
        static constexpr float kFastMotionThreshold = 12.f;
        static constexpr float kMotionLumRatio = 35.f;
        static constexpr float kMotionLumMinPrev = 2.0f;
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
