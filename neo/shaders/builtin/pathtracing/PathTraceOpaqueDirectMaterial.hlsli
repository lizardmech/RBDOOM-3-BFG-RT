#ifndef RB_PATH_TRACE_OPAQUE_DIRECT_MATERIAL_HLSLI
#define RB_PATH_TRACE_OPAQUE_DIRECT_MATERIAL_HLSLI

// Opaque direct-light BSDF adapter. RAB and ReSTIR code should call through
// wrapper functions instead of embedding material-specific diffuse behavior.

static const float RT_PATH_TRACE_OPAQUE_DIRECT_PI = 3.14159265358979323846;

#if defined(RB_PATH_TRACE_OPAQUE_DIRECT_ENABLE_OPENPBR)
#include "PathTraceOpenPbrEonGgxVndf.hlsli"
#ifndef RB_PATH_TRACE_OPAQUE_DIRECT_BRDF_MODE
#define RB_PATH_TRACE_OPAQUE_DIRECT_BRDF_MODE 0
#endif
#else
#define RB_PATH_TRACE_OPAQUE_DIRECT_BRDF_MODE 0
#endif

float3 MaterialOpaqueDirectCosineHemisphereDirection(float3 normal, float2 randomValues)
{
    const float phi = 2.0 * RT_PATH_TRACE_OPAQUE_DIRECT_PI * randomValues.x;
    const float radius = sqrt(saturate(randomValues.y));
    const float x = cos(phi) * radius;
    const float y = sin(phi) * radius;
    const float z = sqrt(max(0.0, 1.0 - saturate(randomValues.y)));
    const float3 tangent = RAB_BuildPerpendicular(normal);
    const float3 bitangent = RAB_SafeNormalize(cross(normal, tangent), float3(0.0, 1.0, 0.0));
    return RAB_SafeNormalize(tangent * x + bitangent * y + normal * z, normal);
}

bool SampleOpaqueDirectBrdf(RAB_Surface surface, inout RAB_RandomSamplerState rng, out float3 dir)
{
    dir = float3(0.0, 0.0, 0.0);
    if (!RAB_SurfaceSupportsOpaqueDiffuseBrdf(surface))
    {
        return false;
    }

    const float3 normal = RAB_SafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
#if RB_PATH_TRACE_OPAQUE_DIRECT_BRDF_MODE >= 3
    const float3 randomValues = float3(RAB_GetNextRandom(rng), RAB_GetNextRandom(rng), RAB_GetNextRandom(rng));
    const float3 viewDir = RAB_SafeNormalize(RAB_GetSurfaceViewDir(surface), normal);
    const float specularProbability = PathTraceOpenPbrSpecularSampleProbability(GetSpecularF0(surface.material), GetRoughness(surface.material));
    if (randomValues.x < specularProbability &&
        PathTraceOpenPbrSampleGgxVndf(GetRoughness(surface.material), normal, viewDir, randomValues.yz, dir))
    {
        return dot(normal, dir) > 0.0 && dot(RAB_GetSurfaceGeoNormal(surface), dir) > 0.0;
    }
    dir = MaterialOpaqueDirectCosineHemisphereDirection(normal, randomValues.yz);
#else
    const float2 randomValues = float2(RAB_GetNextRandom(rng), RAB_GetNextRandom(rng));
    dir = MaterialOpaqueDirectCosineHemisphereDirection(normal, randomValues);
#endif
    return dot(normal, dir) > 0.0 && dot(RAB_GetSurfaceGeoNormal(surface), dir) > 0.0;
}

float EvaluateOpaqueDirectPdf(RAB_Surface surface, float3 dir)
{
    if (!RAB_SurfaceSupportsOpaqueDiffuseBrdf(surface))
    {
        return 0.0;
    }

    const float3 normal = RAB_SafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float ndotDir = saturate(dot(normal, RAB_SafeNormalize(dir, normal)));
    if (dot(RAB_GetSurfaceGeoNormal(surface), dir) <= 0.0)
    {
        return 0.0;
    }

    const float diffusePdf = ndotDir / RT_PATH_TRACE_OPAQUE_DIRECT_PI;
#if RB_PATH_TRACE_OPAQUE_DIRECT_BRDF_MODE >= 3
    const float3 viewDir = RAB_SafeNormalize(RAB_GetSurfaceViewDir(surface), normal);
    const float specularProbability = PathTraceOpenPbrSpecularSampleProbability(GetSpecularF0(surface.material), GetRoughness(surface.material));
    const float specularPdf = PathTraceOpenPbrGgxReflectionPdf(GetRoughness(surface.material), normal, RAB_SafeNormalize(dir, normal), viewDir);
    return lerp(diffusePdf, specularPdf, specularProbability);
#else
    return diffusePdf;
#endif
}

float3 EvaluateOpaqueDirectBrdf(RAB_Surface surface, float3 wi, float3 wo)
{
    if (!RAB_SurfaceSupportsOpaqueDiffuseBrdf(surface))
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float3 normal = RAB_SafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    if (dot(normal, wi) <= 0.0 || dot(normal, wo) <= 0.0 ||
        dot(RAB_GetSurfaceGeoNormal(surface), wi) <= 0.0 ||
        dot(RAB_GetSurfaceGeoNormal(surface), wo) <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float3 albedo = GetDiffuseAlbedo(surface.material);
#if RB_PATH_TRACE_OPAQUE_DIRECT_BRDF_MODE == 1
    return PathTraceOpenPbrEvaluateEonDiffuse(albedo, GetRoughness(surface.material), normal, RAB_SafeNormalize(wi, normal), RAB_SafeNormalize(wo, normal));
#elif RB_PATH_TRACE_OPAQUE_DIRECT_BRDF_MODE >= 2
    const float3 lightDir = RAB_SafeNormalize(wi, normal);
    const float3 viewDir = RAB_SafeNormalize(wo, normal);
    const float3 f0 = GetSpecularF0(surface.material);
    const float3 fresnel = PathTraceOpenPbrFresnelSchlick(f0, saturate(dot(viewDir, RAB_SafeNormalize(lightDir + viewDir, normal))));
    const float diffuseWeight = saturate(1.0 - max(max(fresnel.r, fresnel.g), fresnel.b));
    const float3 diffuse = PathTraceOpenPbrEvaluateEonDiffuse(albedo, GetRoughness(surface.material), normal, lightDir, viewDir) * diffuseWeight;
    const float3 specular = PathTraceOpenPbrEvaluateGgxSpecular(f0, GetRoughness(surface.material), normal, lightDir, viewDir);
    return diffuse + specular;
#else
    return albedo * (1.0 / RT_PATH_TRACE_OPAQUE_DIRECT_PI);
#endif
}

float3 EvaluateOpaqueDirectBrdfOverPdf(RAB_Surface surface, float3 wi, float3 wo)
{
    const float pdf = EvaluateOpaqueDirectPdf(surface, wi);
    if (pdf <= 1.0e-6)
    {
        return float3(0.0, 0.0, 0.0);
    }

    const float3 normal = RAB_SafeNormalize(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceGeoNormal(surface));
    const float ndotWi = saturate(dot(normal, RAB_SafeNormalize(wi, normal)));
    return EvaluateOpaqueDirectBrdf(surface, wi, wo) * (ndotWi / pdf);
}

#endif
