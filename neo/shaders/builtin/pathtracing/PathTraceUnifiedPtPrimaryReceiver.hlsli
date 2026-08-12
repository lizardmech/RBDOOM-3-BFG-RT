#ifndef RB_PATH_TRACE_UNIFIED_PT_PRIMARY_RECEIVER_HLSLI
#define RB_PATH_TRACE_UNIFIED_PT_PRIMARY_RECEIVER_HLSLI

static const uint RT_UPT_PRIMARY_RECEIVER_VERSION = 2u;
static const uint RT_UPT_PRIMARY_RECEIVER_VALID = 0x00010000u;

// Three uint4 words, exactly 48 bytes.  This is a current-frame shading
// receiver, not a temporal history record.
struct PathTraceUnifiedPtPrimaryReceiver
{
    uint4 positionAndHeader;       // xyz = asuint(world position), w = version/valid
    uint4 directionsAndRoughness;  // oct geom, oct shading, oct view, fp16 roughness
    uint4 material;                // fp16 baseColor/specularF0 + RGB9E5 emission
};

// Two uint4 words, exactly 32 bytes.  World position is reconstructed in D0
// from the primary camera origin, full-precision hit distance, and the packed
// surface-to-camera direction.  This is an experimental traffic A/B; the
// proven 48-byte receiver remains available independently.
struct PathTraceUnifiedPtPrimaryReceiver32
{
    uint4 geometry; // hit distance, oct geom, oct shading, oct view
    uint4 material; // RGB10 base/specular, RGB9E5 emission, fp16 roughness + header
};

// Motion and stable material identity are cold for D0 but required by T0/S0.
// Keeping them in this separate 32-byte stream lets D0 retain its 32-byte hot
// receiver without falling back to the shared 176-byte history record.
static const uint RT_UPT_PRIMARY_HISTORY_SIDECAR_VERSION = 1u;
struct PathTraceUnifiedPtPrimaryHistorySidecar
{
    uint4 metadata; // version|surfaceClass, valid flags, material id, material flags
    float4 previousPositionAndAlphaCutoff;
};

float2 PathTraceUptEncodeOctahedral(float3 value)
{
    const float lengthSquared = dot(value, value);
    value = lengthSquared > 1.0e-12 && isfinite(lengthSquared)
        ? value * rsqrt(lengthSquared)
        : float3(0.0, 0.0, 1.0);
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

uint PathTraceUptPackRgb10(float3 value)
{
    const uint3 packed = uint3(round(saturate(
        all(isfinite(value)) ? value : float3(0.0, 0.0, 0.0)) * 1023.0));
    return (packed.x & 0x3ffu) | ((packed.y & 0x3ffu) << 10u) |
        ((packed.z & 0x3ffu) << 20u);
}

// Unsigned RGB9E5-style shared-exponent storage. Primary self-emission is
// resolved locally and must survive the compact P0->D0 ABI; it must not be
// injected into the reusable reservoir where it could migrate to a neighbor.
uint PathTraceUptPackSharedExponentEmission(float3 value)
{
    const float3 finiteValue = all(isfinite(value))
        ? min(max(value, float3(0.0, 0.0, 0.0)), float3(65408.0, 65408.0, 65408.0))
        : float3(0.0, 0.0, 0.0);
    const float maximum = max(finiteValue.x, max(finiteValue.y, finiteValue.z));
    if (maximum <= 0.0)
    {
        return 0u;
    }

    uint exponent = (uint)clamp((int)floor(log2(maximum)) + 16, 0, 31);
    float scale = exp2(24.0 - float(exponent));
    uint3 mantissa = uint3(round(finiteValue * scale));
    if (max(mantissa.x, max(mantissa.y, mantissa.z)) > 511u && exponent < 31u)
    {
        ++exponent;
        scale *= 0.5;
        mantissa = uint3(round(finiteValue * scale));
    }
    mantissa = min(mantissa, uint3(511u, 511u, 511u));
    return mantissa.x | (mantissa.y << 9u) | (mantissa.z << 18u) |
        (exponent << 27u);
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
        PathTraceUptPackSharedExponentEmission(
            surface.material.emissiveRadiance));
    return receiver;
}

PathTraceUnifiedPtPrimaryReceiver32 PackPathTraceUnifiedPtPrimaryReceiver32(
    RAB_Surface surface,
    float3 cameraWorldPosition)
{
    PathTraceUnifiedPtPrimaryReceiver32 receiver =
        (PathTraceUnifiedPtPrimaryReceiver32)0;
    receiver.material.w = RT_UPT_PRIMARY_RECEIVER_VERSION << 16u;
    if (!RAB_IsSurfaceValid(surface))
    {
        return receiver;
    }

    const float3 toCamera = cameraWorldPosition - surface.worldPos;
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
        PathTraceUptPackRgb10(baseColor),
        PathTraceUptPackRgb10(specularF0),
        PathTraceUptPackSharedExponentEmission(
            surface.material.emissiveRadiance),
        (f32tof16(saturate(surface.material.roughness)) & 0xffffu) |
            ((RT_UPT_PRIMARY_RECEIVER_VERSION | 0x8000u) << 16u));
    return receiver;
}

PathTraceUnifiedPtPrimaryHistorySidecar
PackPathTraceUnifiedPtPrimaryHistorySidecar(
    PathTracePrimarySurfaceRecord record)
{
    PathTraceUnifiedPtPrimaryHistorySidecar sidecar =
        (PathTraceUnifiedPtPrimaryHistorySidecar)0;
    sidecar.metadata = uint4(
        RT_UPT_PRIMARY_HISTORY_SIDECAR_VERSION |
            ((record.materialAndSurface.w & 0xffffu) << 16u),
        record.header.y,
        record.materialAndSurface.x,
        record.materialAndSurface.z);
    sidecar.previousPositionAndAlphaCutoff = float4(
        record.previousPositionOrMotion.xyz,
        record.albedoAndAlphaCutoff.w);
    return sidecar;
}

#endif
