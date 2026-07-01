#ifndef RB_PATH_TRACE_OPAQUE_DIRECT_MATERIAL_HLSLI
#define RB_PATH_TRACE_OPAQUE_DIRECT_MATERIAL_HLSLI

// Opaque direct-light BSDF adapter. RAB and ReSTIR code should call through
// wrapper functions instead of embedding material-specific diffuse behavior.

static const float RT_PATH_TRACE_OPAQUE_DIRECT_PI = 3.14159265358979323846;

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
    const float2 randomValues = float2(RAB_GetNextRandom(rng), RAB_GetNextRandom(rng));
    dir = MaterialOpaqueDirectCosineHemisphereDirection(normal, randomValues);
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
    return dot(RAB_GetSurfaceGeoNormal(surface), dir) > 0.0 ? ndotDir / RT_PATH_TRACE_OPAQUE_DIRECT_PI : 0.0;
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

    return GetDiffuseAlbedo(surface.material) * (1.0 / RT_PATH_TRACE_OPAQUE_DIRECT_PI);
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
