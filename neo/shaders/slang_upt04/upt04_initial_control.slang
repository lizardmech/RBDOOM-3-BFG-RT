public static const uint kUpt04FamilyLocalLight = 0x00000001u;
public static const uint kUpt04TextureProbeDisabled = 0u;

// Twenty-one scalar words / 84 bytes.  The production host may add only admitted
// words while remaining below the 128-byte Vulkan push-constant target.
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
    public uint environmentEnabled;
    public uint logicalTextureCount;
    public uint primaryGeneration;
    public uint materialGeneration;
    public uint lightGeneration;
    public uint textureGeneration;
    public uint geometryGeneration;
    public uint abiGeneration;
    public uint textureBindingProbeEnabled;
    public uint materialCount;
    public uint currentLightCount;
};
