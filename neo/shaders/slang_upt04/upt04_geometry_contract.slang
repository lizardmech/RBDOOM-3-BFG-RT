import upt04_hit_contract;

public static const uint kUpt04RigidSurfaceClass = 1u;
public static const uint kUpt04StaticBucketInstanceBit = 0x00800000u;
public static const uint kUpt04StaticBucketTriangleMask = 0x007fffffu;
public static const uint kUpt04ShaderInstanceMask = 0x00ffffffu;

public struct Upt04SmokeVertex
{
    public float4 position;
    public float4 normal;
    public float4 texCoord;
    public float4 color;
    public float4 color2;
    public float4 tangent;
    public float4 bitangent;
};

public struct Upt04RigidRouteInstance
{
    public uint vertexOffset;
    public uint indexOffset;
    public uint triangleOffset;
    public uint materialId;
    public uint materialIndex;
    public uint vertexCount;
    public uint indexCount;
    public uint triangleCount;
    public uint flags;
    public uint instanceIdLo;
    public uint instanceIdHi;
    public uint padding0;
    public float4 currentObjectToWorld0;
    public float4 currentObjectToWorld1;
    public float4 currentObjectToWorld2;
    public float4 previousObjectToWorld0;
    public float4 previousObjectToWorld1;
    public float4 previousObjectToWorld2;
};

public struct Upt04SkinnedRouteRecord
{
    public uint shaderInstanceId;
    public uint sourceIndexOffset;
    public uint outputVertexOffset;
    public uint previousPositionOffset;
    public uint triangleMetadataOffset;
    public uint vertexCount;
    public uint indexCount;
    public uint triangleCount;
    public uint flags;
    public uint instanceHashLo;
    public uint instanceHashHi;
    public uint sourceChecksumLo;
    public uint sourceChecksumHi;
    public uint sourceGpuIndexGenerationLo;
    public uint sourceGpuIndexGenerationHi;
    public uint outputStorageGenerationLo;
    public uint outputStorageGenerationHi;
    public uint routeCount;
    public uint triangleMetadataCount;
    public uint previousPositionCount;
};

public struct Upt04SkinnedRouteTriangle
{
    public uint sourcePrimitiveIndex;
    public uint legacyPrimitiveIndex;
    public uint materialId;
    public uint materialIndex;
    public uint triangleClassAndFlags;
    public uint canonicalPrimitiveHashLo;
    public uint canonicalPrimitiveHashHi;
    public uint emissiveIdentityHashLo;
    public uint emissiveIdentityHashHi;
};

public struct Upt04EmissiveReplayRecord
{
    public float4 centerAndArea;
    public float4 normalAndLuminance;
    public float4 uvBounds;
    public float4 centroidUvAndWeight;
    public float4 estimatedRadianceAndLuminance;
    public float4 sampleWeightAndPdf;
    public uint materialIndex;
    public uint instanceId;
    public uint primitiveIndex;
    public uint flags;
    public uint emissiveTextureIndex;
    public uint emissiveTextureWidth;
    public uint emissiveTextureHeight;
    public uint materialId;
    public uint universeMaterialIndex;
    public uint identityHashLo;
    public uint identityHashHi;
    public uint padding0;
};

public struct Upt04ResolvedTriangle
{
    public uint valid;
    public uint materialIndex;
    public uint triangleClassAndFlags;
    public uint instanceId;
    public uint primitiveIndex;
    public Upt04SmokeVertex vertex0;
    public Upt04SmokeVertex vertex1;
    public Upt04SmokeVertex vertex2;
};

public struct Upt04GeometryHitSurface
{
    public uint valid;
    public uint materialIndex;
    public uint triangleClassAndFlags;
    public uint frontFace;
    public float3 position;
    public float3 geometricNormal;
    public float3 shadingNormal;
    public float3 tangent;
    public float3 bitangent;
    public float2 texCoord;
    public float2 normalTexCoord;
    public float4 vertexColor;
    public float4 vertexColorAdd;
};

public struct Upt04EmissiveGeometrySample
{
    public uint status;
    public uint materialIndex;
    public uint triangleClassAndFlags;
    public uint replayIndex;
    public float3 position;
    public float3 normal;
    public float3 direction;
    public float distance;
    public float solidAnglePdf;
    public float2 texCoord;
};
