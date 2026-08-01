// Exact field order of PathTracePrimarySurfaceRecord version 2.  The source
// buffer has a 176-byte stride; UPT consumes it rather than tracing a camera ray.

public static const uint kUpt04PrimarySurfaceVersion = 2u;
public static const uint kUpt04PrimarySurfaceValid = 0x00000001u;

public struct Upt04PrimarySurfaceRecord
{
    public uint4 header;
    public float4 worldPositionAndViewDepth;
    public float4 geometricNormalAndRoughness;
    public float4 shadingNormalAndOpacity;
    public float4 viewDirectionAndReserved;
    public float4 albedoAndAlphaCutoff;
    public float4 specularF0AndReserved;
    public float4 emissiveAndHeight;
    public float4 previousPositionOrMotion;
    public uint4 materialAndSurface;
    public uint4 instancePrimitiveObject;
};

public bool Upt04PrimarySurfaceRecordValid(Upt04PrimarySurfaceRecord record)
{
    return record.header.x == kUpt04PrimarySurfaceVersion
        && (record.header.y & kUpt04PrimarySurfaceValid) != 0u
        && all(isfinite(record.worldPositionAndViewDepth.xyz))
        && all(isfinite(record.geometricNormalAndRoughness.xyz))
        && all(isfinite(record.shadingNormalAndOpacity.xyz))
        && all(isfinite(record.viewDirectionAndReserved.xyz));
}
