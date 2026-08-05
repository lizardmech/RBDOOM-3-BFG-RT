import upt04_primary_surface_live_contract;

public static const uint kUpt04PrimaryHistorySidecarVersion = 1u;

// Must remain byte-identical to PathTraceUnifiedPtPrimaryHistorySidecar.
public struct Upt04PrimaryHistorySidecar
{
    public uint4 metadata;
    public float4 previousPositionAndAlphaCutoff;
};

public Upt04PrimarySurfaceRecord Upt04ApplyPrimaryHistorySidecar(
    Upt04PrimarySurfaceRecord surface,
    Upt04PrimaryHistorySidecar sidecar)
{
    if ((sidecar.metadata.x & 0xffffu) !=
        kUpt04PrimaryHistorySidecarVersion)
        return (Upt04PrimarySurfaceRecord)0;

    surface.header.y = sidecar.metadata.y;
    surface.materialAndSurface = uint4(
        sidecar.metadata.z,
        0u,
        sidecar.metadata.w,
        sidecar.metadata.x >> 16u);
    surface.previousPositionOrMotion = float4(
        sidecar.previousPositionAndAlphaCutoff.xyz,
        (sidecar.metadata.y & kUpt04PrimarySurfaceHasPreviousPosition) != 0u
            ? 1.0f : 0.0f);
    surface.albedoAndAlphaCutoff.w =
        sidecar.previousPositionAndAlphaCutoff.w;
    return surface;
}
