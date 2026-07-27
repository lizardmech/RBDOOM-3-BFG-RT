// Purpose:
//     Supplies rbdoom shadow visibility callbacks for RTXDI/RAB visibility
//     contracts.
//
// NVIDIA reference:
//     E:\prog\references\RTXDI-main\Samples\FullSample\Shaders\LightingPasses\RtxdiApplicationBridge\RAB_VisibilityTest.hlsli
//
// Current rbdoom supplier:
//     pathtrace_smoke.rt.hlsl shadow payload and smoke shadow ray dispatch.
//
// Current deviation:
//     rbdoom visibility uses the smoke ray payload, ignore-instance/material
//     policy, and existing shadow any-hit shaders instead of the reference
//     sample visibility payload.
//
// Extraction rule:
//     This file must preserve existing behavior until a later correction task.

#ifndef PATHTRACE_SMOKE_RAB_VISIBILITY_SUPPLIER_HLSLI
#define PATHTRACE_SMOKE_RAB_VISIBILITY_SUPPLIER_HLSLI

PathTraceSmokeShadowPayload InitSmokeShadowPayload(uint ignoreInstanceId, uint ignorePrimitiveIndex, uint ignoreMaterialId)
{
    PathTraceSmokeShadowPayload payload;
    payload.hit = 0u;
    payload.rayMode = ignoreInstanceId != 0xffffffffu ? 2u : 1u;
    payload.ignoreInstanceId = ignoreInstanceId;
    payload.ignorePrimitiveIndex = ignorePrimitiveIndex;
    payload.ignoreMaterialId = ignoreMaterialId;
    return payload;
}

float TraceSmokeShadowVisibility(float3 origin, float3 direction, float tMax, uint ignoreInstanceId, uint ignorePrimitiveIndex, uint ignoreMaterialId)
{
    PathTraceSmokeShadowPayload shadowPayload = InitSmokeShadowPayload(ignoreInstanceId, ignorePrimitiveIndex, ignoreMaterialId);

    RayDesc shadowRay;
    shadowRay.Origin = origin;
    shadowRay.Direction = direction;
    shadowRay.TMin = 0.01;
    shadowRay.TMax = tMax;

    const uint rayFlags = ignoreInstanceId != 0xffffffffu
        ? (RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER | RAY_FLAG_FORCE_NON_OPAQUE)
        : (RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_CLOSEST_HIT_SHADER);

    TraceRay(
        SmokeScene,
        rayFlags,
        0xff,
        1,
        0,
        1,
        shadowRay,
        shadowPayload);

    return shadowPayload.hit == 0u ? 1.0 : 0.0;
}

#endif
