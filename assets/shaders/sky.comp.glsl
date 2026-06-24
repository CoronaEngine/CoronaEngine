#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout (local_size_x = 8, local_size_y = 8) in;

layout (set = 0, binding = 0) uniform sampler2D textures[];
layout (set = 2, binding = 0, rgba16f) uniform image2D inputImageRGBA16[];

layout(push_constant) uniform PushConsts
{
    uvec2 gbufferSize;
    uint gbufferDepthImage;
    uint finalOutputImage;
    uint uniformBufferIndex;

    vec3 sun_dir;
    uint floor_grid_enabled;
    float cameraFov;
    float sky_intensity;
} pushConsts;

layout(set = 1, binding = 0) buffer UniformBufferObject
{
    // Light data
    vec3 lightPosition;
    float padding0;
    mat4 lightViewMatrix;
    mat4 lightProjMatrix;

    // Eye/Camera data
    vec3 eyePosition;
    float padding1;
    vec3 eyeDir;
    float padding2;
    mat4 eyeViewMatrix;
    mat4 eyeProjMatrix;
} uniformBufferObjects[];


/********************** atmospheric *****************************/
// Rayleigh and Mie scattering atmosphere system

#define float3 vec3

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
    float a,
    float b,
    float z2,
    float r0)
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

// http://davidson16807.github.io/tectonics.js//2019/03/24/fast-atmospheric-scattering.html
float approx_air_column_density_ratio_along_3d_ray_for_curved_world(
    float3 P,
    float3 V,
    float x,
    float r,
    float H)
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
/********************** atmospheric *****************************/


float grid_line(float coord, float scale)
{
    float v = coord * scale;
    float d = min(fract(v), 1.0f - fract(v));
    return 1.0f - smoothstep(0.0f, 0.02f, d);
}


void main()
{
    if (gl_GlobalInvocationID.x >= pushConsts.gbufferSize.x ||
        gl_GlobalInvocationID.y >= pushConsts.gbufferSize.y) {
        return;
    }

    vec2 screenUV = vec2(float(gl_GlobalInvocationID.x) / float(pushConsts.gbufferSize.x),
                         float(gl_GlobalInvocationID.y) / float(pushConsts.gbufferSize.y));
    float gbufferDepth = texture(textures[pushConsts.gbufferDepthImage], screenUV).r;

    // Only render sky for pixels without geometry
    if (gbufferDepth < (1.0 - 1e-3)) {
        return;
    }

    vec2 aspect_ratio = vec2(float(pushConsts.gbufferSize.x) / float(pushConsts.gbufferSize.y), 1);
    float fov = tan(radians(pushConsts.cameraFov * 0.5));
    vec2 point_ndc = screenUV;

    vec3 cam_local_point = vec3((2.0 * point_ndc.x - 1.0) * aspect_ratio.x * fov,
                          (1.0 - 2.0 * point_ndc.y) * aspect_ratio.y * fov,
                          1.0);

    vec3 cam_origin = vec3(0, 6371e3 + 1., 0) + uniformBufferObjects[pushConsts.uniformBufferIndex].eyePosition;

    vec3 fwd = normalize(uniformBufferObjects[pushConsts.uniformBufferIndex].eyeDir);
    vec3 up = vec3(0, 1, 0);
    vec3 right = cross(up, fwd);
    up = cross(fwd, right);

    vec3 rayOrigin = cam_origin;
    vec3 rayDir = normalize(fwd + up * cam_local_point.y + right * cam_local_point.x);

    vec3 renderResult = getAtmosphericSky(rayOrigin, rayDir, pushConsts.sun_dir, pushConsts.sky_intensity);

    // Floor grid overlay
    if (pushConsts.floor_grid_enabled != 0u)
    {
        vec3 scene_ray_origin = uniformBufferObjects[pushConsts.uniformBufferIndex].eyePosition;
        vec3 scene_ray_dir = normalize(fwd + up * cam_local_point.y + right * cam_local_point.x);

        if (abs(scene_ray_dir.y) > 1e-5f)
        {
            float t = -scene_ray_origin.y / scene_ray_dir.y;
            if (t > 0.0f)
            {
                vec3 hit = scene_ray_origin + scene_ray_dir * t;
                float minor = max(grid_line(hit.x, 1.0f), grid_line(hit.z, 1.0f));
                float major = max(grid_line(hit.x, 0.2f), grid_line(hit.z, 0.2f));
                float grid_alpha = clamp(minor * 0.35f + major * 0.65f, 0.0f, 1.0f);
                vec3 grid_color = mix(vec3(0.15f), vec3(0.30f), major);
                renderResult = mix(renderResult, grid_color, grid_alpha * 0.75f);
            }
        }
    }

    imageStore(inputImageRGBA16[pushConsts.finalOutputImage], ivec2(gl_GlobalInvocationID.xy), vec4(renderResult, 1.0));
}
