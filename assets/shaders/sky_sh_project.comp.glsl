#version 460
#extension GL_EXT_nonuniform_qualifier : enable

// ============================================================================
// Sky → SH9 projection (sky-driven ambient).
// One workgroup of 64 threads. Each thread samples one Fibonacci-sphere
// direction, evaluates the analytic atmospheric sky radiance for that
// direction, projects it onto the 9 real spherical-harmonic basis functions,
// and the group reduces into 9 vec3 coefficients written to an SSBO. The
// lighting pass later evaluates these coefficients per-pixel as irradiance.
//
// Resolution-independent: dispatched (1,1,1), once per environment change
// (signature-cached on the C++ side). The atmospheric functions are copied
// verbatim from sky.comp.glsl so Native ambient matches the visible sky.
// ============================================================================

layout(local_size_x = 64) in;

// Writable SSBO pool (same set/binding as actor_pick.comp.glsl). The lighting
// pass reads the same buffer through the readonly SSBOPool view.
layout(set = 1, binding = 0) buffer SSBOPool { uint data[]; } ssbos[];

layout(push_constant) uniform PushConsts
{
    uint  outputBufferIndex;  // SH coefficient SSBO (writes 9 vec3 = 27 floats)
    uint  sampleCount;        // sphere sample count (== local_size_x, e.g. 64)
    uint  pad0;
    uint  pad1;
    vec3  sun_dir;
    float sky_intensity;
} pushConsts;

#define PI 3.14159265359
#define float3 vec3

// ---- atmospheric model (copied from sky.comp.glsl) -------------------------

bool intersectWithEarth(float3 rayOrigin, float3 rayDir, inout float t0, inout float t1)
{
    float3 rc = -rayOrigin;
    float radius2 = 6471e3 * 6471e3;
    float tca = dot(rc, rayDir);
    float d2 = dot(rc, rc) - tca * tca;
    if (d2 > radius2)
        return false;
    float thc = sqrt(radius2 - d2);
    t0 = tca - thc;
    t1 = tca + thc;
    return true;
}

float rayleighPhase(float mu)
{
    return 3.0f * (1.0f + mu * mu) / (16.0f * 3.1415926);
}

float HenyeyGreensteinPhase(float mu)
{
    const float g = 0.76f;
    return (1.0f - g * g) / ((4.0f + 3.1415926f) * pow(1.0f + g * g - 2.0f * g * mu, 1.5f));
}

float approx_air_column_density_ratio_through_atmosphere(
    float a, float b, float z2, float r0)
{
    const float SQRT_HALF_PI = sqrt(3.1415926f / 2.0f);
    const float k = 0.6;
    float x0 = sqrt(max(r0 * r0 - z2, 1e-20));
    if (a < x0 && -x0 < b && z2 < r0 * r0)
    {
        return 1e20;
    }
    float abs_a = abs(a);
    float abs_b = abs(b);
    float z = sqrt(z2);
    float sqrt_z = sqrt(z);
    float ra = sqrt(a * a + z2);
    float rb = sqrt(b * b + z2);
    float ch0 = (1.0f - 1.0f / (2.0f * r0)) * SQRT_HALF_PI * sqrt_z + k * x0;
    float cha = (1.0f - 1.0f / (2.0f * ra)) * SQRT_HALF_PI * sqrt_z + k * abs_a;
    float chb = (1.0f - 1.0f / (2.0f * rb)) * SQRT_HALF_PI * sqrt_z + k * abs_b;
    float s0 = min(exp(r0 - z), 1.0f) / (x0 / r0 + 1.0f / ch0);
    float sa = exp(r0 - ra) / max(abs_a / ra + 1.0f / cha, 0.01f);
    float sb = exp(r0 - rb) / max(abs_b / rb + 1.0f / chb, 0.01f);
    return max(sign(b) * (s0 - sb) - sign(a) * (s0 - sa), 0.0f);
}

float approx_air_column_density_ratio_along_3d_ray_for_curved_world(
    float3 P, float3 V, float x, float r, float H)
{
    float xz = dot(-P, V);
    float z2 = dot(P, P) - xz * xz;
    return approx_air_column_density_ratio_through_atmosphere(0.0f - xz, x - xz, z2, r / H);
}

float3 getAtmosphericSky(float3 rayOrigin, float3 rayDir, float3 sun_dir, float sun_power)
{
    rayDir = normalize(rayDir);
    sun_dir = normalize(sun_dir);

    int samplesCount = 16;
    float3 betaR = float3(5.5e-6, 13.0e-6, 22.4e-6);
    float3 betaM = float3(21e-6);
    const float earthRadius = 6371e3;

    float t0, t1;
    if (!intersectWithEarth(rayOrigin, rayDir, t0, t1)) {
        return float3(0);
    }
    if (t1 <= 0.0f) return float3(0);

    float march_step = t1 / float(samplesCount);
    float mu = clamp(dot(rayDir, sun_dir), -1.0, 1.0);

    float phaseR = rayleighPhase(mu);
    float phaseM = HenyeyGreensteinPhase(mu);

    float optical_depthR = 0.0f;
    float optical_depthM = 0.0f;

    float3 sumR = float3(0);
    float3 sumM = float3(0);
    float march_pos = 0.0f;

    for (int i = 0; i < samplesCount; i++) {
        const float hR = 7994.0f;
        const float hM = 1200.0f;

        float3 s = rayOrigin + rayDir * (march_pos + 0.5f * march_step);
        float height = max(length(s) - earthRadius, 0.0f);

        float hr = exp(-height / hR) * march_step;
        float hm = exp(-height / hM) * march_step;
        optical_depthR += hr;
        optical_depthM += hm;

        float t0_light = 0.0f, t1_light = 0.0f;
        intersectWithEarth(s, sun_dir, t0_light, t1_light);

        float optical_depth_lightR = approx_air_column_density_ratio_along_3d_ray_for_curved_world(s, sun_dir, t1_light, earthRadius, hR);
        float optical_depth_lightM = approx_air_column_density_ratio_along_3d_ray_for_curved_world(s, sun_dir, t1_light, earthRadius, hM);

        float3 tau = betaR * (optical_depthR + optical_depth_lightR) + betaM * 1.1f * (optical_depthM + optical_depth_lightM);
        float3 attenuation = exp(-max(tau, 0.0f));

        sumR += hr * attenuation;
        sumM += hm * attenuation;

        march_pos += march_step;
    }

    return sun_power * (sumR * phaseR * betaR + sumM * phaseM * betaM);
}

// ---- SH projection ---------------------------------------------------------

// ~uniform sphere distribution via the golden-angle spiral.
vec3 fibonacciSphere(uint i, uint n)
{
    float golden = PI * (3.0 - sqrt(5.0));
    float y = 1.0 - (float(i) + 0.5) / float(n) * 2.0;  // 1 .. -1
    float radius = sqrt(max(0.0, 1.0 - y * y));
    float theta = golden * float(i);
    return vec3(cos(theta) * radius, y, sin(theta) * radius);
}

void writeFloat(uint buf, uint off, float v)
{
    ssbos[nonuniformEXT(buf)].data[off] = floatBitsToUint(v);
}

// Per-thread partial coefficients; thread 0 reduces. 64*9 vec3 well under the
// shared-memory budget and avoids any atomic-on-vec3 gymnastics.
shared vec3 g_sh[64 * 9];

void main()
{
    uint tid = gl_LocalInvocationID.x;
    uint N = pushConsts.sampleCount;

    vec3 dir = fibonacciSphere(tid, N);
    // Origin barely matters at planetary scale; fixed ground-level point.
    vec3 skyOrigin = vec3(0.0, 6371e3 + 1.0, 0.0);
    vec3 radiance = getAtmosphericSky(skyOrigin, dir, pushConsts.sun_dir,
                                      pushConsts.sky_intensity);

    // Monte-Carlo solid-angle weight for a uniform sphere distribution.
    float w = 4.0 * PI / float(N);

    float x = dir.x, y = dir.y, z = dir.z;
    float Y[9];
    Y[0] = 0.282095;                       // Y(0, 0)
    Y[1] = 0.488603 * y;                   // Y(1,-1)
    Y[2] = 0.488603 * z;                   // Y(1, 0)
    Y[3] = 0.488603 * x;                   // Y(1, 1)
    Y[4] = 1.092548 * x * y;               // Y(2,-2)
    Y[5] = 1.092548 * y * z;               // Y(2,-1)
    Y[6] = 0.315392 * (3.0 * z * z - 1.0); // Y(2, 0)
    Y[7] = 1.092548 * x * z;               // Y(2, 1)
    Y[8] = 0.546274 * (x * x - y * y);     // Y(2, 2)

    for (int k = 0; k < 9; k++) {
        g_sh[tid * 9u + uint(k)] = radiance * Y[k] * w;
    }

    barrier();

    if (tid == 0u) {
        for (int k = 0; k < 9; k++) {
            vec3 sum = vec3(0.0);
            for (uint i = 0u; i < N; i++) {
                sum += g_sh[i * 9u + uint(k)];
            }
            uint off = uint(k) * 3u;
            writeFloat(pushConsts.outputBufferIndex, off + 0u, sum.x);
            writeFloat(pushConsts.outputBufferIndex, off + 1u, sum.y);
            writeFloat(pushConsts.outputBufferIndex, off + 2u, sum.z);
        }
    }
}
