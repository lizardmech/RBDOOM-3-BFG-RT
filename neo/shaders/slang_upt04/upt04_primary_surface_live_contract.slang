// Exact field order of PathTracePrimarySurfaceRecord version 2.  The source
// buffer has a 176-byte stride; UPT consumes it rather than tracing a camera ray.

public static const uint kUpt04PrimarySurfaceVersion = 2u;
public static const uint kUpt04PrimarySurfaceValid = 0x00000001u;
public static const uint kUpt04PrimarySurfaceHasObjectMotion = 0x00000004u;
public static const uint kUpt04PrimarySurfaceHasPreviousPosition = 0x00000008u;
public static const uint kUpt04SurfaceClassRigidEntity = 1u;
public static const uint kUpt04SurfaceClassSkinnedDeformed = 2u;
public static const uint kUpt04MaterialAlphaTest = 0x00000001u;

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

public bool Upt04PrimarySurfaceRequiresObjectMotion(
    Upt04PrimarySurfaceRecord record)
{
    return record.materialAndSurface.w == kUpt04SurfaceClassRigidEntity
        || record.materialAndSurface.w == kUpt04SurfaceClassSkinnedDeformed;
}

public bool Upt04PrimarySurfaceHasValidObjectMotion(
    Upt04PrimarySurfaceRecord record)
{
    const uint requiredFlags = kUpt04PrimarySurfaceHasObjectMotion
        | kUpt04PrimarySurfaceHasPreviousPosition;
    return (record.header.y & requiredFlags) == requiredFlags
        && record.previousPositionOrMotion.w >= 0.5f
        && all(isfinite(record.previousPositionOrMotion.xyz));
}

public bool Upt04PrimarySurfaceUsesAlphaCoverage(
    Upt04PrimarySurfaceRecord record)
{
    return (record.materialAndSurface.z & kUpt04MaterialAlphaTest) != 0u;
}

public bool Upt04PrimarySurfaceAlphaReuseCompatible(
    Upt04PrimarySurfaceRecord receiver,
    Upt04PrimarySurfaceRecord donor)
{
    const bool receiverAlpha = Upt04PrimarySurfaceUsesAlphaCoverage(receiver);
    const bool donorAlpha = Upt04PrimarySurfaceUsesAlphaCoverage(donor);
    if (!receiverAlpha && !donorAlpha)
        return true;

    // An accepted texel on an alpha-clipped card and the opaque surface seen
    // through a neighboring rejected texel can be coplanar, share normals and
    // pass the ordinary depth gate. Reusing between those two domains lets a
    // high-weight sample cross the cutout silhouette; repeated temporal and
    // spatial reuse then propagates and amplifies it across the frame. Keep
    // the broad geometric gate for opaque materials, but make alpha coverage
    // an explicit boundary keyed by the stable material identity. Do not use
    // the dense material-table index here: it is a frame-local address and may
    // be remapped while the stable ID remains valid.
    return receiverAlpha && donorAlpha
        && receiver.materialAndSurface.x == donor.materialAndSurface.x
        && isfinite(receiver.albedoAndAlphaCutoff.w)
        && isfinite(donor.albedoAndAlphaCutoff.w)
        && abs(receiver.albedoAndAlphaCutoff.w
            - donor.albedoAndAlphaCutoff.w) <= 1.0e-4f;
}

public float Upt04PrimarySurfaceMaterialLuminance(float3 value)
{
    return dot(value, float3(0.2126f, 0.7152f, 0.0722f));
}

public bool Upt04PrimarySurfaceMaterialsSimilar(
    Upt04PrimarySurfaceRecord receiver,
    Upt04PrimarySurfaceRecord donor)
{
    const float receiverRoughness =
        saturate(receiver.geometricNormalAndRoughness.w);
    const float donorRoughness =
        saturate(donor.geometricNormalAndRoughness.w);
    const float roughnessScale = max(
        max(abs(receiverRoughness), abs(donorRoughness)), 1.0e-4f);
    const float roughnessRelativeDifference =
        abs(receiverRoughness - donorRoughness) / roughnessScale;
    const float receiverReflectivity = Upt04PrimarySurfaceMaterialLuminance(
        saturate(receiver.specularF0AndReserved.xyz));
    const float donorReflectivity = Upt04PrimarySurfaceMaterialLuminance(
        saturate(donor.specularF0AndReserved.xyz));
    const float receiverAlbedo = Upt04PrimarySurfaceMaterialLuminance(
        saturate(receiver.albedoAndAlphaCutoff.xyz));
    const float donorAlbedo = Upt04PrimarySurfaceMaterialLuminance(
        saturate(donor.albedoAndAlphaCutoff.xyz));

    // Match the material edge-stop used by the RTXDI FullSample bridge and
    // required unconditionally by its PT spatial pass. The earlier UPT09 gate
    // checked only roughness, so texture/decal-composited albedo discontinuities
    // on one geometric receiver were invisible to spatial reuse. This operates
    // on the evaluated primary record, not the material identity, and therefore
    // also covers filter/alpha composites that deliberately retain the base
    // receiver's stable material ID.
    return isfinite(roughnessRelativeDifference)
        && isfinite(receiverReflectivity) && isfinite(donorReflectivity)
        && isfinite(receiverAlbedo) && isfinite(donorAlbedo)
        && roughnessRelativeDifference <= 0.5f
        && abs(receiverReflectivity - donorReflectivity) <= 0.25f
        && abs(receiverAlbedo - donorAlbedo) <= 0.25f;
}
