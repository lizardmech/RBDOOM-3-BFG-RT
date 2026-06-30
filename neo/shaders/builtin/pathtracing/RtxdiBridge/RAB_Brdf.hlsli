#ifndef RB_PATH_TRACING_RAB_BRDF_HLSLI
#define RB_PATH_TRACING_RAB_BRDF_HLSLI

#include "RAB_SurfaceCore.hlsli"
#include "RAB_RandomSamplerState.hlsli"
#include "../PathTraceOpaqueDirectMaterial.hlsli"

float3 RAB_CosineHemisphereDirection(float3 normal, float2 randomValues)
{
    return MaterialOpaqueDirectCosineHemisphereDirection(normal, randomValues);
}

bool RAB_GetSurfaceBrdfSample(RAB_Surface surface, inout RAB_RandomSamplerState rng, out float3 dir)
{
    return SampleOpaqueDirectBrdf(surface, rng, dir);
}

float RAB_GetSurfaceBrdfPdf(RAB_Surface surface, float3 dir)
{
    return EvaluateOpaqueDirectPdf(surface, dir);
}

float3 RAB_EvaluateSurfaceBrdf(RAB_Surface surface, float3 wi, float3 wo)
{
    return EvaluateOpaqueDirectBrdf(surface, wi, wo);
}

float3 RAB_EvaluateSurfaceBrdfOverPdf(RAB_Surface surface, float3 wi, float3 wo)
{
    return EvaluateOpaqueDirectBrdfOverPdf(surface, wi, wo);
}

bool RAB_SurfaceImportanceSampleBrdf(RAB_Surface surface, inout RAB_RandomSamplerState rng, out float3 dir)
{
    return RAB_GetSurfaceBrdfSample(surface, rng, dir);
}

bool RAB_SurfaceImportanceSampleBrdf(RAB_Surface surface, inout RTXDI_RandomSamplerState rng, out float3 dir)
{
    RAB_RandomSamplerState rabRng = RAB_CreateRandomSamplerFromDirectSeed(rng.seed, rng.index);
    const bool valid = RAB_GetSurfaceBrdfSample(surface, rabRng, dir);
    rng.seed = rabRng.seed;
    rng.index = rabRng.index;
    return valid;
}

float RAB_SurfaceEvaluateBrdfPdf(RAB_Surface surface, float3 dir)
{
    return RAB_GetSurfaceBrdfPdf(surface, dir);
}

#endif
