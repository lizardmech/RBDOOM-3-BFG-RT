#ifndef RB_PATH_TRACING_RAB_SURFACE_CORE_HLSLI
#define RB_PATH_TRACING_RAB_SURFACE_CORE_HLSLI

#include "RAB_Material.hlsli"

struct RAB_Surface
{
    uint valid;
    float3 worldPos;
    float linearDepth;
    float3 geometryNormal;
    uint materialId;
    float3 shadingNormal;
    uint materialIndex;
    float3 viewDir;
    uint instanceId;
    uint primitiveIndex;
    uint surfaceClass;
    uint flags;
    RAB_Material material;
};

RAB_Surface RAB_EmptySurface()
{
    RAB_Surface surface = (RAB_Surface)0;
    surface.material = RAB_EmptyMaterial();
    return surface;
}

bool RAB_IsSurfaceValid(RAB_Surface surface)
{
    return surface.valid != 0;
}

float3 RAB_GetSurfaceWorldPos(RAB_Surface surface)
{
    return surface.worldPos;
}

void RAB_SetSurfaceWorldPos(inout RAB_Surface surface, float3 worldPos)
{
    surface.worldPos = worldPos;
}

float3 RAB_GetSurfaceNormal(RAB_Surface surface)
{
    return surface.shadingNormal;
}

float3 RAB_GetSurfaceGeoNormal(RAB_Surface surface)
{
    return surface.geometryNormal;
}

void RAB_SetSurfaceNormal(inout RAB_Surface surface, float3 normal)
{
    surface.shadingNormal = normal;
}

float3 RAB_GetSurfaceViewDir(RAB_Surface surface)
{
    return surface.viewDir;
}

float RAB_GetSurfaceLinearDepth(RAB_Surface surface)
{
    return surface.linearDepth;
}

RAB_Material RAB_GetMaterial(RAB_Surface surface)
{
    return surface.material;
}

float RAB_GetSurfaceRoughness(RAB_Surface surface)
{
    return GetRoughness(surface.material);
}

float3 RAB_SafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-8 ? value * rsqrt(lengthSquared) : fallback;
}

float3 RAB_BuildPerpendicular(float3 normal)
{
    const float3 axis = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    return RAB_SafeNormalize(cross(axis, normal), float3(1.0, 0.0, 0.0));
}

#include "../PathTraceMaterialFeature.hlsli"

bool RAB_SurfaceSupportsOpaqueDiffuseBrdf(RAB_Surface surface)
{
    return MaterialSupportsOpaqueDirect(surface);
}

#endif
