public static const uint kUpt04FamilyLocalLight = 0x00000001u;
public static const uint kUpt04FamilyIndirect = 0x00000002u;
public static const uint kUpt04RouteStaticBuckets = 0x00000002u;
public static const uint kUpt04EnvironmentAvailable = 0x00000004u;
public static const uint kUpt04TextureProbeDisabled = 0u;

// Thirty-two scalar words / 128 bytes.  The host combines all resource epochs
// into one 64-bit page generation, leaving exact O(1) geometry bounds in the
// Vulkan push-constant budget without admitting a second constant buffer.
public struct Upt04InitialControl
{
    public uint renderWidth;
    public uint renderHeight;
    public uint surfaceCount;
    public uint frameSampleIndex;
    public uint enabledFamilyMask;
    public uint specializationIdentity;
    public uint emissiveRangeStart;
    public uint emissiveRangeCount;
    public uint analyticRangeStart;
    public uint analyticRangeCount;
    public uint availabilityFlags;
    public uint logicalTextureCount;
    public uint pageGenerationLo;
    public uint pageGenerationHi;
    public uint textureBindingProbeEnabled;
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
    public uint emissiveReplayCount;
};
