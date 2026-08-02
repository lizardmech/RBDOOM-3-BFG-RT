import upt04_primary_surface_live_contract;

public static const uint kUpt04PackedPrimaryVersion = 1u;
public static const uint kUpt04PackedPrimaryValid = 0x00010000u;

// Must remain byte-identical to PathTraceUnifiedPtPrimaryReceiver (48 bytes).
public struct Upt04PackedPrimarySurfaceRecord
{
    public uint4 positionAndHeader;
    public uint4 directionsAndRoughness;
    public uint4 material;
};

public float2 Upt04UnpackHalf2(uint packed)
{
    return float2(
        f16tof32(packed & 0xffffu),
        f16tof32((packed >> 16u) & 0xffffu));
}
public float3 Upt04DecodeOctahedral(uint packed)
{
    const float2 encoded = float2(
        float(packed & 0xffffu),
        float((packed >> 16u) & 0xffffu)) / 65535.0f;
    const float2 f = encoded * 2.0f - 1.0f;
    float3 value = float3(f, 1.0f - abs(f.x) - abs(f.y));
    if (value.z < 0.0f)
    {
        const float2 signValue = float2(
            value.x >= 0.0f ? 1.0f : -1.0f,
            value.y >= 0.0f ? 1.0f : -1.0f);
        value.xy = (1.0f - abs(value.yx)) * signValue;
    }
    const float lengthSquared = dot(value, value);
    return isfinite(lengthSquared) && lengthSquared > 1.0e-12f
        ? value * rsqrt(lengthSquared)
        : float3(0.0f, 0.0f, 1.0f);
}

public Upt04PrimarySurfaceRecord Upt04UnpackPrimarySurface(
    Upt04PackedPrimarySurfaceRecord packed)
{
    Upt04PrimarySurfaceRecord surface = (Upt04PrimarySurfaceRecord)0;
    const uint packedHeader = packed.positionAndHeader.w;
    if ((packedHeader & 0xffffu) != kUpt04PackedPrimaryVersion ||
        (packedHeader & kUpt04PackedPrimaryValid) == 0u)
        return surface;

    const float3 baseColor = float3(
        Upt04UnpackHalf2(packed.material.x),
        Upt04UnpackHalf2(packed.material.y).x);
    const float3 specularF0 = float3(
        Upt04UnpackHalf2(packed.material.y).y,
        Upt04UnpackHalf2(packed.material.z));
    surface.header = uint4(
        kUpt04PrimarySurfaceVersion,
        kUpt04PrimarySurfaceValid,
        0u,
        0u);
    surface.worldPositionAndViewDepth = float4(
        asfloat(packed.positionAndHeader.xyz),
        0.0f);
    surface.geometricNormalAndRoughness = float4(
        Upt04DecodeOctahedral(packed.directionsAndRoughness.x),
        Upt04UnpackHalf2(packed.directionsAndRoughness.w).x);
    surface.shadingNormalAndOpacity = float4(
        Upt04DecodeOctahedral(packed.directionsAndRoughness.y),
        1.0f);
    surface.viewDirectionAndReserved = float4(
        Upt04DecodeOctahedral(packed.directionsAndRoughness.z),
        0.0f);
    surface.albedoAndAlphaCutoff = float4(baseColor, 0.0f);
    surface.specularF0AndReserved = float4(specularF0, 0.0f);
    return surface;
}
