public static const uint kUpt04FamilyLocalLight = 0x00000001u;
public static const uint kUpt04FamilyIndirect = 0x00000002u;
public static const uint kUpt04RouteStaticBuckets = 0x00000002u;
public static const uint kUpt04EnvironmentAvailable = 0x00000004u;
public static const uint kUpt04EmissiveLookupExact = 0x00000008u;
public static const uint kUpt04MaterialUseSpecularMaps = 0x00000010u;
public static const uint kUpt04MaterialLegacySpecmapToPbr = 0x00000020u;
public static const uint kUpt04TwoSidedEmissives = 0x00000040u;
public static const uint kUpt04MaterialDecodeTextures = 0x00000080u;
public static const uint kUpt04EmissiveTrialCountShift = 8u;
public static const uint kUpt04EmissiveTrialCountMask = 0x00001f00u;
public static const uint kUpt04DirectProposalParity = 0x40000000u;
public static const uint kUpt04D0PreviousBest = 0x80000000u;
public static const uint kUpt04TextureProbeDisabled = 0u;
public static const uint kUpt04ControlMetadataValidBit = 0x80000000u;
public static const uint kUpt04ControlMetadataCountMask = 0x7fffffffu;
public static const uint kUpt04ThreeVertexProbeMask = 0x0000000fu;
public static const uint kUpt04TiledSurface = 0x00000100u;
public static const uint kUpt04ReservoirTexture = 0x00000200u;
public static const uint kUpt04PrimaryTexture = 0x00000400u;

public uint Upt04TiledSurfaceBufferIndex(
    uint surfaceIndex,
    uint width,
    uint tiled)
{
    if (tiled == 0u || width == 0u)
        return surfaceIndex;
    const uint x = surfaceIndex % width;
    const uint y = surfaceIndex / width;
    const uint tilesX = (width + 7u) / 8u;
    return (((y >> 3u) * tilesX) + (x >> 3u)) * 64u
        + ((y & 7u) * 8u) + (x & 7u);
}

public uint Upt04EnabledFamilyMask(uint dynamicMask)
{
#ifdef UPT04_STATIC_ENABLED_FAMILY_MASK
    // The production specialization removes unavailable families at compile
    // time, while the dynamic mask may further restrict admission for a
    // family-0 diagnostic. Normal production still supplies the full mask.
    return UPT04_STATIC_ENABLED_FAMILY_MASK & dynamicMask;
#else
    return dynamicMask;
#endif
}

public uint Upt04DirectProposalParityValue(uint availabilityFlags)
{
#ifdef UPT04_STATIC_DIRECT_PROPOSAL_PARITY
    return UPT04_STATIC_DIRECT_PROPOSAL_PARITY;
#else
    return (availabilityFlags & kUpt04DirectProposalParity) != 0u
        ? 1u : 0u;
#endif
}

public uint Upt04ConfiguredEmissiveTrialCount(uint availabilityFlags)
{
    return clamp(
        (availabilityFlags & kUpt04EmissiveTrialCountMask)
            >> kUpt04EmissiveTrialCountShift,
        1u,
        16u);
}

// Fifty-six scalar words / 224 bytes. The first 128 bytes retain the
// accepted current-frame D0 ABI. The final four words groups add the exact
// previous-camera/remap metadata needed by the D0 previous-best lookup and the
// current valid CDF count. Both count words reserve their high bit for the
// matching availability/validity bit. The final float2 is an ABI suffix.
public struct Upt04InitialControl
{
    public uint renderWidth;
    public uint renderHeight;
    public uint surfaceCount;
    public uint frameSampleIndex;
    public uint enabledFamilyMask;
    public float primaryCameraOriginX;
    public uint emissiveRangeStart;
    public uint emissiveRangeCount;
    public uint analyticRangeStart;
    public uint analyticRangeCount;
    public uint availabilityFlags;
    public uint logicalTextureCount;
    public float primaryCameraOriginY;
    public float primaryCameraOriginZ;
    public uint shaderProofMode;
    public uint materialCount;
    public uint currentLightCount;
    public uint staticVertexCount;
    public uint staticIndexCount;
    public uint staticTriangleCount;
    public uint dynamicVertexCount;
    public uint dynamicIndexCount;
    public uint dynamicTriangleCount;
    public uint rigidVertexCount;
    public uint rigidIndexCount;
    public uint rigidTriangleCount;
    public uint rigidInstanceCount;
    public uint skinnedRouteRecordCount;
    public uint skinnedRouteTriangleCount;
    public uint skinnedSourceIndexCount;
    public uint skinnedCurrentVertexCount;
    // The replay buffer publishes its own count through GetDimensions. Reuse
    // this final ABI word for the live emissive scale so D0/T0/S0 evaluate the
    // same emitter energy as the accepted legacy path.
    public float emissiveScale;
    public float previousCameraOriginX;
    public float previousCameraOriginY;
    public float previousCameraOriginZ;
    public uint previousToCurrentLightCountAndHistory;
    public float previousCameraForwardX;
    public float previousCameraForwardY;
    public float previousCameraForwardZ;
    public float previousCameraTanX;
    public float previousCameraLeftX;
    public float previousCameraLeftY;
    public float previousCameraLeftZ;
    public float previousCameraTanY;
    public float previousCameraUpX;
    public float previousCameraUpY;
    public float previousCameraUpZ;
    public uint emissiveDistributionCountAndValid;
    public uint emissiveLookupCapacityAndValid;
    public uint indirectPolicyFlags;
    public uint reservedControl0;
    public uint reservedControl1;
    // Primary rays are stored at pixel centers shifted by the RR projection
    // jitter. Reprojection adds the previous frame's offset to recover the
    // actual history-buffer pixel without changing the geometric projection.
    public float2 previousCameraJitterPixels;
    // Directional sky terminal emission must be established in D0 so the
    // ordinary temporal/spatial reservoir chain owns it. Keep this as a
    // suffix to preserve every accepted field offset above.
    public float skyBrightness;
    // Texture-space floor shared by D0/T0/S0. Near-black compression residue
    // must not become a valid terminal emitter and enter replay.
    public float emissiveTexelBlackFloor;
};

public bool Upt04D0PreviousBestHistoryAvailable(
    uint availabilityFlags,
    uint previousToCurrentLightCountAndHistory)
{
    return (availabilityFlags & kUpt04D0PreviousBest) != 0u
        && (previousToCurrentLightCountAndHistory
            & kUpt04ControlMetadataValidBit) != 0u;
}

public uint Upt04D0PreviousToCurrentLightCount(
    uint previousToCurrentLightCountAndHistory)
{
    return previousToCurrentLightCountAndHistory
        & kUpt04ControlMetadataCountMask;
}

public bool Upt04EmissiveDistributionValid(
    uint emissiveDistributionCountAndValid)
{
    return (emissiveDistributionCountAndValid
        & kUpt04ControlMetadataValidBit) != 0u;
}

public uint Upt04EmissiveDistributionCount(
    uint emissiveDistributionCountAndValid)
{
    return emissiveDistributionCountAndValid
        & kUpt04ControlMetadataCountMask;
}

public bool Upt04EmissiveLookupValid(
    uint availabilityFlags,
    uint capacityAndValid)
{
    return (availabilityFlags & kUpt04EmissiveLookupExact) != 0u
        && (capacityAndValid & kUpt04ControlMetadataValidBit) != 0u;
}

public uint Upt04EmissiveLookupCapacity(uint capacityAndValid)
{
    return capacityAndValid & kUpt04ControlMetadataCountMask;
}

public uint Upt04ThreeVertexBottleneckProbe(uint indirectPolicyFlags)
{
    return indirectPolicyFlags & kUpt04ThreeVertexProbeMask;
}