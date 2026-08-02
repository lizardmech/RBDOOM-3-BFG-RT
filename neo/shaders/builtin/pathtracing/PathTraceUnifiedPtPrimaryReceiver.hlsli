#ifndef RB_PATH_TRACE_UNIFIED_PT_PRIMARY_RECEIVER_HLSLI
#define RB_PATH_TRACE_UNIFIED_PT_PRIMARY_RECEIVER_HLSLI

static const uint RT_UPT_PRIMARY_RECEIVER_VERSION = 1u;
static const uint RT_UPT_PRIMARY_RECEIVER_VALID = 0x00010000u;

// Three uint4 words, exactly 48 bytes.  This is a current-frame shading
// receiver, not a temporal history record.
struct PathTraceUnifiedPtPrimaryReceiver
{
    uint4 positionAndHeader;       // xyz = asuint(world position), w = version/valid
    uint4 directionsAndRoughness;  // oct geom, oct shading, oct view, fp16 roughness
    uint4 material;                // fp16 baseColor.rgb + specularF0.rgb
};

// Two uint4 words, exactly 32 bytes.  World position is reconstructed in D0
// from the primary camera origin, full-precision hit distance, and the packed
// surface-to-camera direction.  This is an experimental traffic A/B; the
// proven 48-byte receiver remains available independently.
struct PathTraceUnifiedPtPrimaryReceiver32
{
    uint4 geometry; // hit distance, oct geom, oct shading, oct view
    uint4 material; // fp16 baseColor.rgb, roughness, specularF0.rgb, header
};

float2 PathTraceUptEncodeOctahedral(float3 value)
{
    value = SafeNormalize(value, float3(0.0, 0.0, 1.0));
    value /= max(abs(value.x) + abs(value.y) + abs(value.z), 1.0e-6);
    float2 encoded = value.xy;
    if (value.z < 0.0)
    {
        const float2 signValue = float2(
            encoded.x >= 0.0 ? 1.0 : -1.0,
            encoded.y >= 0.0 ? 1.0 : -1.0);
        encoded = (1.0 - abs(encoded.yx)) * signValue;
    }
    return encoded * 0.5 + 0.5;
}

uint PathTraceUptPackOctahedral(float3 value)
{
    const uint2 packed = uint2(round(saturate(
        PathTraceUptEncodeOctahedral(value)) * 65535.0));
    return (packed.x & 0xffffu) | (packed.y << 16u);
}

uint PathTraceUptPackHalf2(float2 value)
{
    const float2 finiteValue = all(isfinite(value)) ? value : float2(0.0, 0.0);
    return (f32tof16(finiteValue.x) & 0xffffu) |
        (f32tof16(finiteValue.y) << 16u);
}

PathTraceUnifiedPtPrimaryReceiver PackPathTraceUnifiedPtPrimaryReceiver(
    RAB_Surface surface)
{
    PathTraceUnifiedPtPrimaryReceiver receiver =
        (PathTraceUnifiedPtPrimaryReceiver)0;
    receiver.positionAndHeader.w = RT_UPT_PRIMARY_RECEIVER_VERSION;
    if (!RAB_IsSurfaceValid(surface))
    {
        return receiver;
    }

    receiver.positionAndHeader = uint4(
        asuint(surface.worldPos),
        RT_UPT_PRIMARY_RECEIVER_VERSION | RT_UPT_PRIMARY_RECEIVER_VALID);
    receiver.directionsAndRoughness = uint4(
        PathTraceUptPackOctahedral(surface.geometryNormal),
        PathTraceUptPackOctahedral(surface.shadingNormal),
        PathTraceUptPackOctahedral(surface.viewDir),
        PathTraceUptPackHalf2(float2(saturate(surface.material.roughness), 0.0)));
    const float3 baseColor = clamp(
        surface.material.diffuseAlbedo,
        float3(0.0, 0.0, 0.0),
        float3(65504.0, 65504.0, 65504.0));
    const float3 specularF0 = clamp(
        surface.material.specularF0,
        float3(0.0, 0.0, 0.0),
        float3(65504.0, 65504.0, 65504.0));
    receiver.material = uint4(
        PathTraceUptPackHalf2(baseColor.xy),
        PathTraceUptPackHalf2(float2(baseColor.z, specularF0.x)),
        PathTraceUptPackHalf2(specularF0.yz),
        0u);
    return receiver;
}

PathTraceUnifiedPtPrimaryReceiver32 PackPathTraceUnifiedPtPrimaryReceiver32(
    RAB_Surface surface)
{
    PathTraceUnifiedPtPrimaryReceiver32 receiver =
        (PathTraceUnifiedPtPrimaryReceiver32)0;
    receiver.material.w = RT_UPT_PRIMARY_RECEIVER_VERSION << 16u;
    if (!RAB_IsSurfaceValid(surface))
    {
        return receiver;
    }

    const float3 toCamera = CameraOriginAndTMax.xyz - surface.worldPos;
    const float hitDistance = length(toCamera);
    const float3 viewDirection = hitDistance > 1.0e-6
        ? toCamera / hitDistance
        : surface.viewDir;
    const float3 baseColor = clamp(
        surface.material.diffuseAlbedo,
        float3(0.0, 0.0, 0.0),
        float3(65504.0, 65504.0, 65504.0));
    const float3 specularF0 = clamp(
        surface.material.specularF0,
        float3(0.0, 0.0, 0.0),
        float3(65504.0, 65504.0, 65504.0));
    receiver.geometry = uint4(
        asuint(hitDistance),
        PathTraceUptPackOctahedral(surface.geometryNormal),
        PathTraceUptPackOctahedral(surface.shadingNormal),
        PathTraceUptPackOctahedral(viewDirection));
    receiver.material = uint4(
        PathTraceUptPackHalf2(baseColor.xy),
        PathTraceUptPackHalf2(float2(
            baseColor.z,
            saturate(surface.material.roughness))),
        PathTraceUptPackHalf2(specularF0.xy),
        (f32tof16(specularF0.z) & 0xffffu) |
            ((RT_UPT_PRIMARY_RECEIVER_VERSION | 0x8000u) << 16u));
    return receiver;
}

#endif
