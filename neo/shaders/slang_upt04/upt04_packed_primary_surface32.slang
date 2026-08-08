import upt04_primary_surface_live_contract;
import upt04_packed_primary_surface;

public static const uint kUpt04PackedPrimary32Version = 2u;
public static const uint kUpt04PackedPrimary32Valid = 0x8000u;

// Must remain byte-identical to PathTraceUnifiedPtPrimaryReceiver32.
public struct Upt04PackedPrimarySurfaceRecord32
{
    public uint4 geometry;
    public uint4 material;
};

public Upt04PrimarySurfaceRecord Upt04UnpackPrimarySurface32(
    Upt04PackedPrimarySurfaceRecord32 packed,
    float3 cameraOrigin)
{
    Upt04PrimarySurfaceRecord surface = (Upt04PrimarySurfaceRecord)0;
    const uint header = packed.material.w >> 16u;
    if ((header & 0x7fffu) != kUpt04PackedPrimary32Version ||
        (header & kUpt04PackedPrimary32Valid) == 0u)
        return surface;

    const float hitDistance = asfloat(packed.geometry.x);
    const float3 viewDirection = Upt04DecodeOctahedral(packed.geometry.w);
    if (!isfinite(hitDistance) || hitDistance < 0.0f ||
        !all(isfinite(cameraOrigin)))
        return surface;

    const float3 baseColor = float3(
        float(packed.material.x & 0x3ffu),
        float((packed.material.x >> 10u) & 0x3ffu),
        float((packed.material.x >> 20u) & 0x3ffu)) / 1023.0f;
    const float3 specularF0 = float3(
        float(packed.material.y & 0x3ffu),
        float((packed.material.y >> 10u) & 0x3ffu),
        float((packed.material.y >> 20u) & 0x3ffu)) / 1023.0f;
    const float roughness = Upt04UnpackHalf2(packed.material.w).x;
    surface.header = uint4(
        kUpt04PrimarySurfaceVersion,
        kUpt04PrimarySurfaceValid,
        0u,
        0u);
    surface.worldPositionAndViewDepth = float4(
        cameraOrigin - viewDirection * hitDistance,
        hitDistance);
    surface.geometricNormalAndRoughness = float4(
        Upt04DecodeOctahedral(packed.geometry.y),
        roughness);
    surface.shadingNormalAndOpacity = float4(
        Upt04DecodeOctahedral(packed.geometry.z),
        1.0f);
    surface.viewDirectionAndReserved = float4(viewDirection, 0.0f);
    surface.albedoAndAlphaCutoff = float4(
        baseColor,
        0.0f);
    surface.specularF0AndReserved = float4(specularF0, 0.0f);
    surface.emissiveAndHeight = float4(
        Upt04UnpackSharedExponentEmission(packed.material.z), 0.0f);
    return surface;
}
