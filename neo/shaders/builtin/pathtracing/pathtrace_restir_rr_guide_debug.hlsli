// Purpose:
//     Supplies rbdoom data for local Ray Reconstruction guide debug views.
//
// NVIDIA reference:
//     E:\prog\references\RTXDI-main\Samples\FullSample\Shaders\LightingPasses\PT\PSRUtils.hlsli
//
// Current rbdoom supplier:
//     PathTraceRR guide textures, motion-vector masks, reset masks, and local
//     combined lighting preview.
//
// Current deviation:
//     This is a diagnostic readout only. It does not generate PSR data or alter
//     denoiser inputs.
//
// Extraction rule:
//     This file must preserve existing behavior until a later correction task.

#ifndef RB_PATH_TRACING_RESTIR_RR_GUIDE_DEBUG_HLSLI
#define RB_PATH_TRACING_RESTIR_RR_GUIDE_DEBUG_HLSLI

uint RayReconstructionGuideDebugView()
{
    return clamp((uint)max(RayReconstructionInfo.x, 0.0), 0u, 10u);
}

float4 RayReconstructionMotionMaskDebugColor(uint motionMask)
{
    const bool valid = (motionMask & PT_MOTION_VECTOR_MASK_VALID) != 0u;
    const uint sourceKind = (motionMask & PT_MOTION_VECTOR_MASK_SOURCE_MASK) >> PT_MOTION_VECTOR_MASK_SOURCE_SHIFT;
    if (!valid)
    {
        return sourceKind == 0u ? float4(0.02, 0.02, 0.02, 1.0) : float4(0.65, 0.08, 0.08, 1.0);
    }
    if (sourceKind == PT_MOTION_VECTOR_SOURCE_STATIC)
    {
        return float4(0.10, 0.75, 0.20, 1.0);
    }
    if (sourceKind == PT_MOTION_VECTOR_SOURCE_SKINNED)
    {
        return float4(0.15, 0.45, 1.00, 1.0);
    }
    if (sourceKind == PT_MOTION_VECTOR_SOURCE_RIGID)
    {
        return float4(0.10, 0.85, 0.85, 1.0);
    }
    return float4(0.85, 0.75, 0.15, 1.0);
}

float4 RayReconstructionResetMaskDebugColor(uint resetMask)
{
    if (resetMask == 0u)
    {
        return float4(0.03, 0.45, 0.12, 1.0);
    }
    if ((resetMask & RT_RR_RESET_STOCHASTIC_TRANSLUCENT) != 0u)
    {
        return float4(0.75, 0.15, 0.85, 1.0);
    }
    if ((resetMask & RT_RR_RESET_MATERIAL_MISMATCH) != 0u)
    {
        return float4(1.00, 0.60, 0.05, 1.0);
    }
    if ((resetMask & RT_RR_RESET_REJECTED_PREVIOUS) != 0u)
    {
        return float4(0.95, 0.05, 0.05, 1.0);
    }
    if ((resetMask & RT_RR_RESET_MISSING_PREVIOUS) != 0u)
    {
        return float4(0.60, 0.10, 0.10, 1.0);
    }
    if ((resetMask & RT_RR_RESET_OBJECT_MOTION_UNAVAILABLE) != 0u)
    {
        return float4(0.80, 0.80, 0.15, 1.0);
    }
    if ((resetMask & RT_RR_RESET_INVALID_SURFACE) != 0u)
    {
        return float4(0.02, 0.02, 0.02, 1.0);
    }
    return float4(0.80, 0.30, 0.10, 1.0);
}

float4 EvaluateRayReconstructionGuideDebug(uint2 pixel, uint view)
{
    const RAB_Surface surface = LoadPathTracePrimarySurfaceRecord(int2(pixel), false);
    if (view == 1u)
    {
        const float3 guideAlbedo = PathTraceRRGuideAlbedo[pixel].rgb;
        const float3 historyAlbedo = RAB_IsSurfaceValid(surface) ? surface.material.diffuseAlbedo : float3(0.0, 0.0, 0.0);
        const float3 albedo = RAB_Luminance(guideAlbedo) > 0.0 ? guideAlbedo : historyAlbedo;
        return float4(saturate(albedo), 1.0);
    }
    if (view == 2u)
    {
        const float3 guideNormal = PathTraceRRGuideNormalRoughness[pixel].rgb;
        const float3 historyNormal = RAB_IsSurfaceValid(surface) ? surface.shadingNormal * 0.5 + 0.5 : float3(0.5, 0.5, 1.0);
        const float normalHasGuide = any(abs(guideNormal - float3(0.0, 0.0, 1.0)) > float3(0.001, 0.001, 0.001)) ? 1.0 : 0.0;
        return float4(saturate(lerp(historyNormal, guideNormal * 0.5 + 0.5, normalHasGuide)), 1.0);
    }
    if (view == 8u)
    {
        const float3 guideSpecular = PathTraceRRGuideSpecularAlbedo[pixel].rgb;
        const float3 historySpecular = RAB_IsSurfaceValid(surface) ? surface.material.specularF0 : float3(0.0, 0.0, 0.0);
        const float3 specularAlbedo = max(max(guideSpecular.r, guideSpecular.g), guideSpecular.b) > 0.0 ? guideSpecular : historySpecular;
        // Dielectric F0 normally occupies roughly 0.04-0.12. Displaying that
        // range raw made valid coat changes look uniformly black, so view 8 is
        // an amplified diagnostic only; the stored RR guide remains untouched.
        return float4(saturate(specularAlbedo * 8.0), 1.0);
    }
    if (view == 3u)
    {
        float roughness = saturate(PathTraceRRGuideNormalRoughness[pixel].a);
        if (roughness >= 0.999 && RAB_IsSurfaceValid(surface))
        {
            roughness = saturate(surface.material.roughness);
        }
        return float4(roughness, roughness, roughness, 1.0);
    }
    if (view == 4u)
    {
        float depth = PathTraceRRGuideDepth[pixel];
        if (depth <= 0.0 && RAB_IsSurfaceValid(surface))
        {
            depth = surface.linearDepth;
        }
        const float normalizedDepth = saturate(depth / 4096.0);
        return float4(normalizedDepth, normalizedDepth, normalizedDepth, 1.0);
    }
    if (view == 5u)
    {
        const float hitDistance = PathTraceRRGuideHitDistance[pixel];
        if (!(hitDistance == hitDistance))
        {
            return float4(1.0, 0.0, 1.0, 1.0);
        }
        if (hitDistance <= 0.0)
        {
            return float4(0.0, 0.0, 0.0, 1.0);
        }
        const float normalizedHitDistance = saturate(hitDistance / 1000.0);
        return float4(normalizedHitDistance, normalizedHitDistance, normalizedHitDistance, 1.0);
    }

    if (view == 6u)
    {
        return RayReconstructionMotionMaskDebugColor(PathTraceMotionVectorMask[pixel]);
    }

    if (view == 7u)
    {
        return RayReconstructionResetMaskDebugColor(PathTraceRRGuideResetMask[pixel]);
    }

    if (view == 9u)
    {
        const RestirPTCombinedLighting lighting = RestirPTEvaluateCombinedLightingNoReflection(surface, pixel);
        return float4(RestirPTToneMapPreview(lighting.hdrRadiance), 1.0);
    }

    if (view == 10u)
    {
        const float2 motionPixels = PathTraceRRMotionVectors[pixel];
        const float magnitude = saturate(length(motionPixels) / 64.0);
        const float2 direction = saturate(motionPixels / 128.0 + 0.5);
        return float4(direction.x, direction.y, magnitude, 1.0);
    }

    return float4(0.0, 0.0, 0.0, 1.0);
}

#endif
