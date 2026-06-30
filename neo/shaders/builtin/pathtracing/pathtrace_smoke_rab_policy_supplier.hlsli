// Purpose:
//     Supplies rbdoom path-tracer policy data for RTXDI/RAB path tracing.
//
// NVIDIA reference:
//     E:\prog\references\RTXDI-main\Samples\FullSample\Shaders\LightingPasses\RtxdiApplicationBridge\PathTracer\RAB_PathTracer.hlsli
//
// Current rbdoom supplier:
//     pathtrace_smoke.rt.hlsl constant buffers and safety policy helpers.
//
// Current deviation:
//     rbdoom policy comes from smoke-path constants and debug safety bits rather
//     than the reference sample's path tracing constants.
//
// Extraction rule:
//     This file must preserve existing behavior until a later correction task.

#ifndef PATHTRACE_SMOKE_RAB_POLICY_SUPPLIER_HLSLI
#define PATHTRACE_SMOKE_RAB_POLICY_SUPPLIER_HLSLI

bool DoomAnalyticLightsEnabled()
{
    return (((uint)max(DoomAnalyticLightInfo.w, 0.0)) & 1u) != 0u && !PathTraceSafetyDisabled(RT_PT_SAFETY_DISABLE_ANALYTIC_LIGHT_LOOP);
}

bool DoomAnalyticLightsReplaceSelected()
{
    return (((uint)max(DoomAnalyticLightInfo.w, 0.0)) & 2u) != 0u;
}

bool SmokeToyFakePBRSpecularEnabled()
{
    return ((((uint)TextureInfo.w) & RT_SMOKE_TEXTURE_FLAG_TOY_FAKE_PBR_SPECULAR) != 0u);
}

uint PathTraceIntegratorSamplesPerPixel()
{
    return clamp((uint)max(IntegratorInfo.x, 1.0), 1u, 4u);
}

uint PathTraceIntegratorMaxPathDepth()
{
    return clamp((uint)max(IntegratorInfo.y, 1.0), 1u, 4u);
}

uint PathTraceIntegratorDiffuseBounceLimit()
{
    if (PathTraceSafetyDisabled(RT_PT_SAFETY_DISABLE_DIFFUSE_SECONDARY_RAY))
    {
        return 0u;
    }
    return clamp((uint)max(IntegratorInfo.z, 0.0), 0u, 3u);
}

uint PathTraceIntegratorSpecularBounceLimit()
{
    if (PathTraceSafetyDisabled(RT_PT_SAFETY_DISABLE_REFLECTION_RAY))
    {
        return 0u;
    }
    return clamp((uint)max(IntegratorInfo.w, 0.0), 0u, 2u);
}

uint PathTraceIntegratorTransmissionBounceLimit()
{
    return clamp((uint)max(IntegratorInfo2.x, 0.0), 0u, 1u);
}

uint PathTraceIntegratorReflectionMode()
{
    if (PathTraceSafetyDisabled(RT_PT_SAFETY_DISABLE_REFLECTION_RAY))
    {
        return 0u;
    }
    return clamp((uint)max(IntegratorInfo2.y, 0.0), 0u, 2u);
}

uint PathTraceIntegratorRussianRouletteDepth()
{
    return clamp((uint)max(IntegratorInfo2.z, 0.0), 0u, 8u);
}

bool PathTraceIntegratorNextEventEstimationEnabled()
{
    return IntegratorInfo2.w >= 0.5;
}

uint PathTraceIntegratorSecondaryNeeMode()
{
    return clamp((uint)max(NeeInfo.x, 0.0), 0u, 2u);
}

uint PathTraceIntegratorSecondaryAnalyticNeeMode()
{
    return clamp((uint)max(NeeInfo.z, 0.0), 0u, 2u);
}

uint PathTraceIntegratorSecondaryAnalyticNeeSamples()
{
    return clamp((uint)max(NeeInfo.w, 0.0), 0u, 8u);
}

bool PathTraceIntegratorSecondaryNeeVisibilityEnabled()
{
    return NeeInfo.y >= 0.5;
}

#endif
