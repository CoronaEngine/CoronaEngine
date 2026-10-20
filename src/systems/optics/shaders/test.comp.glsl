#version 460
#extension GL_EXT_nonuniform_qualifier : enable

layout (local_size_x = 8, local_size_y = 8) in;

layout (set = 0, binding = 0) uniform sampler2D textures[];

layout(set = 2, binding = 0, r32ui) uniform uimage2D inputImageRGBA8[];
layout (set = 2, binding = 0, rgba16) uniform image2D inputImageRGBA16[];

layout(set = 2, binding = 0, rgba16) uniform readonly image2DArray inputImage2DArray[];

layout(push_constant) uniform PushConsts
{
	uvec2 gbufferSize;
	uint gbufferPostionImage;
	uint gbufferBaseColorImage;
	uint gbufferNormalImage;
    uint gbufferDepthImage;

	uvec2 shadowMapSize;
    uint shadowMapDepthImage;

    vec3 lightColor;
    vec3 sun_dir;

    // without screenspace it will boom
    float camera_near;
    float camera_far;

    uint gbufferObjectIDImage;
    uint objectIDOutputImage;

    uint finalOutputImage;

    uint uniformBufferIndex;

    uint mutiviewImageIndex;

    uint time;
    uint floor_grid_enabled;
} pushConsts;


layout(set = 1, binding = 0) buffer UniformBufferObject
{
    // Light data (for shadow mapping, etc.)
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


// ----------------------------------------------------------------------------
float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness*roughness;
    float a2 = a*a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH*NdotH;

    float nom   = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = 3.14159265359 * denom * denom;

    return nom / denom;
}
// ----------------------------------------------------------------------------
float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r*r) / 8.0;

    float nom   = NdotV;
    float denom = NdotV * (1.0 - k) + k;

    return nom / denom;
}
// ----------------------------------------------------------------------------
float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);

    return ggx1 * ggx2;
}
// ----------------------------------------------------------------------------
vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}


vec3 calculateColor(vec3 WorldPos, vec3 Normal,
vec3 lightPos,vec3 lightColor,
vec3  albedo,
float metallic,
float roughness)
{
    vec3 N = normalize(Normal);
    vec3 V = normalize(uniformBufferObjects[pushConsts.uniformBufferIndex].eyePosition - WorldPos);

    // calculate reflectance at normal incidence; if dia-electric (like plastic) use F0
    // of 0.04 and if it's a metal, use the albedo color as F0 (metallic workflow)
    vec3 F0 = vec3(0.04);
    F0 = mix(F0, albedo, metallic);

    // reflectance equation
    vec3 Lo = vec3(0.0);
    //for(int i = 0; i < 4; ++i)
    {
        // calculate per-light radiance
        vec3 L = normalize(pushConsts.sun_dir);
        vec3 H = normalize(V + L);
        //float distance = length(lightPos - WorldPos);
        float attenuation = 1.0;
        vec3 radiance = lightColor * attenuation;

        // Cook-Torrance BRDF
        float NDF = DistributionGGX(N, H, roughness);
        float G   = GeometrySmith(N, V, L, roughness);
        vec3 F    = fresnelSchlick(clamp(dot(H, V), 0.0, 1.0), F0);

        vec3 numerator    = NDF * G * F;
        float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001; // + 0.0001 to prevent divide by zero
        vec3 specular = numerator / denominator;

        // kS is equal to Fresnel
        vec3 kS = F;
        // for energy conservation, the diffuse and specular light can't
        // be above 1.0 (unless the surface emits light); to preserve this
        // relationship the diffuse component (kD) should equal 1.0 - kS.
        vec3 kD = vec3(1.0) - kS;
        // multiply kD by the inverse metalness such that only non-metals
        // have diffuse lighting, or a linear blend if partly metal (pure metals
        // have no diffuse light).
        kD *= 1.0 - metallic;

        // scale light by NdotL
        float NdotL = max(dot(N, L), 0.0);

        // add to outgoing radiance Lo
        Lo += (kD * albedo / 3.14159265359 + specular) * radiance * NdotL;  // note that we already multiplied the BRDF by the Fresnel (kS) so we won't multiply by kS again
    }

    // ambient lighting (note that the next IBL tutorial will replace
    // this ambient lighting with environment lighting).
    vec3 ambient = vec3(0.03) * albedo;

    return ambient + Lo;
}

float linearize_depth(float d)
{
    float zNear = pushConsts.camera_near;
    float zFar  = pushConsts.camera_far;
    return zNear * zFar / (zFar + d * (zNear - zFar));
}

//https://panoskarabelas.com/posts/screen_space_shadows/
//https://github.com/PanosK92/SpartanEngine/blob/master/data/shaders/screen_space_shadows/screen_space_shadows.hlsl
bool ScreenSpaceShadows(vec3 worldSpaceOrigin, vec3 worldSpaceTarget)
{
    if (distance(worldSpaceOrigin, worldSpaceTarget) < 1e-3)
    {
        return false;
    }

    vec3 ray_origin = vec4(uniformBufferObjects[pushConsts.uniformBufferIndex].eyeViewMatrix * vec4(worldSpaceOrigin, 1.0f)).xyz;
    vec3 ray_target = vec4(uniformBufferObjects[pushConsts.uniformBufferIndex].eyeViewMatrix * vec4(worldSpaceTarget, 1.0f)).xyz;
    if(distance(ray_origin, ray_target) < 1e-3)
    {
        return false;
    }

    vec4 rayStartClipSpace = uniformBufferObjects[pushConsts.uniformBufferIndex].eyeProjMatrix * vec4(ray_origin.xyz, 1.0f);
	vec2 ray_start_uv = (rayStartClipSpace.xyz / rayStartClipSpace.w).xy * 0.5f + 0.5f;

    vec4 rayTargetClipSpace = uniformBufferObjects[pushConsts.uniformBufferIndex].eyeProjMatrix * vec4(ray_target.xyz, 1.0f);
	vec2 ray_target_uv = (rayTargetClipSpace.xyz / rayTargetClipSpace.w).xy * 0.5f + 0.5f;

    if(distance(ray_start_uv, ray_target_uv) < 1e-3)
    {
        return false;
    }

	float ScreenDiagonal = sqrt(pushConsts.gbufferSize.x * pushConsts.gbufferSize.x + pushConsts.gbufferSize.y * pushConsts.gbufferSize.y);
    vec2 ray_uv_history = ray_start_uv;
    for (int i = 0; i < ScreenDiagonal; i++)
    {
        // Step the ray
        vec3 ray_pos = ray_origin + ((ray_target - ray_origin) / distance(ray_start_uv, ray_target_uv) / ScreenDiagonal) * float(i);

        vec4 rayClipSpace = uniformBufferObjects[pushConsts.uniformBufferIndex].eyeProjMatrix * vec4(ray_pos.xyz, 1.0f);
	    vec2 ray_uv = (rayClipSpace.xyz / rayClipSpace.w).xy * 0.5f + 0.5f;

        if(distance(ray_uv_history, ray_uv) < 1e-3 || distance(ray_uv_history, ray_uv) < (1.0f / ScreenDiagonal) )
        {
            continue;
        }
        else
        {
            ray_uv_history = ray_uv;
        }

        // Ensure the UV coordinates are inside the screen
        if (ray_uv.x > 0.0 && ray_uv.y > 0.0 && ray_uv.x < 1.0 && ray_uv.y < 1.0)
        {
            // Compute the difference between the ray's and the camera's depth
            float depth_z = linearize_depth(textureLod(textures[nonuniformEXT(pushConsts.gbufferDepthImage)], ray_uv, 0.0).r);
            if (ray_pos.z > depth_z + 1e-3)
            {
                return true;
            }
        }
        else
        {
            return false;
        }
    }

    return false;
}


float performPCF(sampler2D shadowMap, vec2 projCoords, float currentDepth, float bias) {
    float shadow = 0.0;
    vec2 texelSize = 1.0 / textureSize(shadowMap, 0); // 获取纹理单元大小

    float pcfDepth = textureLod(shadowMap, projCoords, 0.0).r;
    shadow = ((currentDepth - bias > pcfDepth) ? 1.0 : 0.0);

    return 1.0f - shadow;

    float GaussKernel[3][3] = {
        {1.0/16.0,2.0/16.0,1.0/16.0},
        {2.0/16.0,4.0/16.0,2.0/16.0},
        {1.0/16.0,2.0/16.0,1.0/16.0},
    };
    // 3x3 PCF 采样
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            // 获取偏移后的深度值
            float pcfDepth = textureLod(shadowMap, projCoords + vec2(x, y) * texelSize, 0.0).r;
            // 比较深度，累加阴影贡献
            shadow += ((currentDepth - bias > pcfDepth) ? 1.0 : 0.0) * GaussKernel[x+1][y+1];
        }
    }

    // 归一化阴影值
    //shadow /= 9.0;
    return 1.0f - shadow;
}



vec3 acesFilmicToneMapCurve(vec3 x)
{
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;

    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 acesFilmicToneMapInverse(vec3 x)
{
    vec3 a = -0.59 * x + 0.03;
    vec3 b = sqrt(-1.0127 * x * x + 1.3702 * x + 0.0009);
    vec3 c = 2 * (2.43 * x - 2.51);
    return ((a - b) / c);
}




/********************** atmospheric *****************************/
// ----------------------------------------------------------------------------
// Rayleigh and Mie scattering atmosphere system
// implementation of the techniques described here:
// ----------------------------------------------------------------------------

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
    // GUIDE TO VARIABLE NAMES:
    //  "x*" distance along the ray from closest approach
    //  "z*" distance from the center of the world at closest approach
    //  "r*" distance ("radius") from the center of the world
    //  "*0" variable at reference point
    //  "*2" the square of a variable
    //  "ch" a nudge we give to prevent division by zero, analogous to the Chapman function
    const float SQRT_HALF_PI = sqrt(3.1415926f / 2.0f);
    const float k = 0.6; // "k" is an empirically derived constant
    float x0 = sqrt(max(r0 * r0 - z2, 1e-20));
    // if obstructed by the world, approximate answer by using a ludicrously large number
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
    float3 P,  // position of viewer
    float3 V,  // direction of viewer (unit vector)
    float x, // distance from the viewer at which we stop the "raymarch"
    float r, // radius of the planet
    float H  // scale height of the planet's atmosphere
)
{
    float xz = dot(-P, V);          // distance ("radius") from the ray to the center of the world at closest approach, squared
    float z2 = dot(P, P) - xz * xz; // distance from the origin at which closest approach occurs
    return approx_air_column_density_ratio_through_atmosphere(0.0f - xz, x - xz, z2, r / H);
}

float3 getAtmosphericSky(float3 rayOrigin, float3 rayDir, float3 sun_dir, float sun_power) {

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



vec3 FilmicToneMappingExpr(vec3 x)
{
    float A = 0.22; // default: 0.22
    float B = 0.30;   // default: 0.30
    float C = 0.10;      // default: 0.10
    float D = 0.20;      // default: 0.20
    float E = 0.01;     // default: 0.01
    float F = 0.30;   // default: 0.30
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

// Hash-based ID to RGB color mapping
vec3 objectIDToColor(float id)
{
    uint uid = uint(id);
    uint r = (uid * 1664525u + 1013904223u) & 0xFFu;
    uint g = ((uid >> 8u) * 1664525u + 1013904223u) & 0xFFu;
    uint b = ((uid >> 16u) * 1664525u + 1013904223u) & 0xFFu;
    return vec3(float(r), float(g), float(b)) / 255.0;
}

float grid_line(float coord, float scale)
{
    float v = coord * scale;
    float d = min(fract(v), 1.0f - fract(v));
    return 1.0f - smoothstep(0.0f, 0.02f, d);
}


void main()
{
	vec2 screenUV = vec2(float(gl_GlobalInvocationID.x)/float(pushConsts.gbufferSize.x), float(gl_GlobalInvocationID.y)/float(pushConsts.gbufferSize.y));
	float gbufferDepth = textureLod(textures[nonuniformEXT(pushConsts.gbufferDepthImage)], screenUV, 0.0).r;

	vec3 renderResult = vec3(0.0f, 0.0f, 0.0f);

	if (gbufferDepth < (1.0 - 1e-3)) // There must be something on the GBuffer.
	{
		vec4 gbufferPostion = imageLoad(inputImageRGBA16[pushConsts.gbufferPostionImage], ivec2(gl_GlobalInvocationID.xy));
		vec4 gbufferBaseColor = imageLoad(inputImageRGBA16[pushConsts.gbufferBaseColorImage], ivec2(gl_GlobalInvocationID.xy));
		vec4 gbufferNormal = imageLoad(inputImageRGBA16[pushConsts.gbufferNormalImage], ivec2(gl_GlobalInvocationID.xy));

		renderResult = calculateColor(gbufferPostion.xyz, gbufferNormal.xyz,
            uniformBufferObjects[pushConsts.uniformBufferIndex].lightPosition,
            pushConsts.lightColor,
            gbufferBaseColor.xyz, 0.5, 0.5);
        renderResult = max(renderResult,vec3(0.01,0.01,0.01));
	}
    else
    {
        vec2 aspect_ratio = vec2(float(pushConsts.gbufferSize.x) / float(pushConsts.gbufferSize.y), 1);
        float fov = tan(radians(45.0));
        vec2 point_ndc = vec2(float(gl_GlobalInvocationID.x) / float(pushConsts.gbufferSize.x), float(gl_GlobalInvocationID.y) / float(pushConsts.gbufferSize.y));


        vec3 cam_local_point = vec3((2.0 * point_ndc.x - 1.0) * aspect_ratio.x * fov,
                              (1.0 - 2.0 * point_ndc.y) * aspect_ratio.y * fov,
                              1.0);

        vec3 cam_origin = vec3(0, 6371e3 + 1., 0) + uniformBufferObjects[pushConsts.uniformBufferIndex].eyePosition;
        vec3 cam_look_at = vec3(0, 6371e3 + 1., 0) + uniformBufferObjects[pushConsts.uniformBufferIndex].eyeDir;

        vec3 fwd = normalize(cam_look_at - cam_origin);
        vec3 up = vec3(0, 1, 0);
        vec3 right = cross(up, fwd);
        up = cross(fwd, right);

        vec3 rayOrigin = cam_origin;
        vec3 rayDir = normalize(fwd + up * cam_local_point.y + right * cam_local_point.x);

        renderResult = getAtmosphericSky(rayOrigin, rayDir,pushConsts.sun_dir,20.0f);

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
    }

	imageStore(inputImageRGBA16[pushConsts.finalOutputImage], ivec2(gl_GlobalInvocationID.xy), vec4(renderResult, 1.0f));

	// Write Object ID visualization
	vec4 rawObjectID = imageLoad(inputImageRGBA16[pushConsts.gbufferObjectIDImage], ivec2(gl_GlobalInvocationID.xy));
	vec3 idColor = (gbufferDepth < (1.0 - 1e-3)) ? objectIDToColor(rawObjectID.r) : vec3(0.0);
	imageStore(inputImageRGBA16[pushConsts.objectIDOutputImage], ivec2(gl_GlobalInvocationID.xy), vec4(idColor, 1.0f));
}
