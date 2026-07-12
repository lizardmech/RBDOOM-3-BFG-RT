// Purpose:
//     Supplies rbdoom smoke payload surface data for RTXDI/RAB surface
//     contracts.
//
// NVIDIA reference:
//     E:\prog\references\RTXDI-main\Samples\FullSample\Shaders\LightingPasses\RtxdiApplicationBridge\RAB_Surface.hlsli
//     E:\prog\references\RTXDI-main\Samples\FullSample\Shaders\LightingPasses\RtxdiApplicationBridge\PathTracer\RAB_PathTracer.hlsli
//
// Current rbdoom supplier:
//     pathtrace_smoke.rt.hlsl ray payload fields and smoke material supplier.
//
// Current deviation:
//     rbdoom builds a RAB surface from the path tracing ray payload instead of
//     reconstructing it from reference G-buffer inputs.
//
// Extraction rule:
//     This file must preserve existing behavior until a later correction task.

#ifndef PATHTRACE_SMOKE_RAB_SURFACE_SUPPLIER_HLSLI
#define PATHTRACE_SMOKE_RAB_SURFACE_SUPPLIER_HLSLI

#ifndef RB_PT_ENABLE_RESTIR
#include "RtxdiBridge/RAB_SurfaceCore.hlsli"
#endif

RAB_Surface RAB_BuildSurfaceFromSmokePayload(PathTraceSmokePayload payload, float3 rayOrigin, float3 rayDirection, bool useNormalMap)
{
    RAB_Surface surface = RAB_EmptySurface();
    if (payload.value == 0u)
    {
        return surface;
    }

    const PathTraceSmokeMaterial smokeMaterial = LoadSmokeMaterial(payload.materialIndex);
    const float3 baseNormal = SafeNormalize(payload.normal, payload.geometricNormal);
    float3 shadingNormal = useNormalMap
        ? DecodeSmokeNormalTexture(smokeMaterial, payload.normalTexCoord, baseNormal, payload.tangent, payload.bitangent)
        : baseNormal;
    const float3 hitPosition = rayOrigin + rayDirection * payload.hitT;
    const float3 viewDir = SafeNormalize(rayOrigin - hitPosition, -rayDirection);
    float3 geometryNormal = SafeNormalize(payload.geometricNormal, baseNormal);
    if (dot(geometryNormal, viewDir) < 0.0)
    {
        geometryNormal = -geometryNormal;
    }
    if (dot(shadingNormal, viewDir) < 0.0)
    {
        shadingNormal = -shadingNormal;
    }
    shadingNormal = ConstrainSmokeShadingNormal(shadingNormal, geometryNormal);

    surface.valid = 1u;
    surface.worldPos = hitPosition;
    surface.linearDepth = payload.hitT;
    surface.geometryNormal = geometryNormal;
    surface.materialId = payload.materialId;
    surface.shadingNormal = shadingNormal;
    surface.materialIndex = payload.materialIndex;
    surface.viewDir = viewDir;
    surface.instanceId = payload.instanceId;
    surface.primitiveIndex = payload.primitiveIndex;
    surface.surfaceClass = payload.surfaceClass;
    surface.flags = payload.triangleClassAndFlags;
    surface.material = RAB_BuildMaterialFromSmokePayload(payload);
    return surface;
}

#endif
