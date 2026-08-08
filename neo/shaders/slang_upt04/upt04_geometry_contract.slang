import upt04_hit_contract;
import upt04_binary16;

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

// UPT-only replay sidecar. The scene/TLAS vertex ABI remains Upt04SmokeVertex;
// a small compute pass repacks the four live vertex pools immediately before
// D0. Position stays fp32, basis vectors use oct16, and texture/color channels
// use fp16. Three uint4 words are exactly 48 bytes.
public static const uint kUpt04CompactVertexVersion = 1u;
public static const uint kUpt04CompactVertexNegativeHandedness = 0x00010000u;

public struct Upt04CompactVertex
{
    public uint4 positionAndHeader;
    public uint4 basisAndTexCoord;
    public uint4 color;
};

public float3 Upt04GeometrySafeNormalizeForPacking(float3 value)
{
    const float lengthSquared = dot(value, value);
    return isfinite(lengthSquared) && lengthSquared > 1.0e-12f
        ? value * rsqrt(lengthSquared)
        : float3(0.0f, 0.0f, 1.0f);
}

public uint Upt04PackHalf2(float2 value)
{
    const float2 finiteValue = float2(
        isfinite(value.x) ? value.x : 0.0f,
        isfinite(value.y) ? value.y : 0.0f);
    return Upt04PackBinary16Pair(finiteValue);
}

public float2 Upt04UnpackVertexHalf2(uint packed)
{
    return Upt04UnpackBinary16Pair(packed);
}

public uint Upt04EncodeVertexOctahedral(float3 value)
{
    value = Upt04GeometrySafeNormalizeForPacking(value);
    value /= max(abs(value.x) + abs(value.y) + abs(value.z), 1.0e-12f);
    if (value.z < 0.0f)
    {
        const float2 signValue = float2(
            value.x >= 0.0f ? 1.0f : -1.0f,
            value.y >= 0.0f ? 1.0f : -1.0f);
        value.xy = (1.0f - abs(value.yx)) * signValue;
    }
    const uint2 encoded = uint2(round(saturate(value.xy * 0.5f + 0.5f) * 65535.0f));
    return (encoded.x & 0xffffu) | ((encoded.y & 0xffffu) << 16u);
}

public float3 Upt04DecodeVertexOctahedral(uint packed)
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

public Upt04CompactVertex Upt04PackCompactVertex(Upt04SmokeVertex source)
{
    Upt04CompactVertex packed = (Upt04CompactVertex)0;
    const float3 normal = Upt04GeometrySafeNormalizeForPacking(source.normal.xyz);
    const float3 tangent = Upt04GeometrySafeNormalizeForPacking(source.tangent.xyz);
    const float3 bitangent = Upt04GeometrySafeNormalizeForPacking(source.bitangent.xyz);
    const bool negativeHandedness = dot(cross(normal, tangent), bitangent) < 0.0f;
    packed.positionAndHeader = uint4(
        asuint(source.position.x),
        asuint(source.position.y),
        asuint(source.position.z),
        kUpt04CompactVertexVersion |
            (negativeHandedness ? kUpt04CompactVertexNegativeHandedness : 0u));
    packed.basisAndTexCoord = uint4(
        Upt04EncodeVertexOctahedral(normal),
        Upt04EncodeVertexOctahedral(tangent),
        Upt04PackHalf2(source.texCoord.xy),
        Upt04PackHalf2(source.texCoord.zw));
    packed.color = uint4(
        Upt04PackHalf2(source.color.xy),
        Upt04PackHalf2(source.color.zw),
        Upt04PackHalf2(source.color2.xy),
        Upt04PackHalf2(source.color2.zw));
    return packed;
}

public Upt04SmokeVertex Upt04UnpackCompactVertex(Upt04CompactVertex packed)
{
    Upt04SmokeVertex vertex = (Upt04SmokeVertex)0;
    const float3 normal = Upt04DecodeVertexOctahedral(packed.basisAndTexCoord.x);
    float3 tangent = Upt04DecodeVertexOctahedral(packed.basisAndTexCoord.y);
    tangent = tangent - normal * dot(normal, tangent);
    tangent = Upt04GeometrySafeNormalizeForPacking(tangent);
    const float handedness =
        (packed.positionAndHeader.w & kUpt04CompactVertexNegativeHandedness) != 0u
            ? -1.0f
            : 1.0f;
    vertex.position = float4(asfloat(packed.positionAndHeader.xyz), 1.0f);
    vertex.normal = float4(normal, 0.0f);
    vertex.tangent = float4(tangent, 0.0f);
    vertex.bitangent = float4(
        Upt04GeometrySafeNormalizeForPacking(cross(normal, tangent)) * handedness,
        0.0f);
    vertex.texCoord = float4(
        Upt04UnpackVertexHalf2(packed.basisAndTexCoord.z),
        Upt04UnpackVertexHalf2(packed.basisAndTexCoord.w));
    vertex.color = float4(
        Upt04UnpackVertexHalf2(packed.color.x),
        Upt04UnpackVertexHalf2(packed.color.y));
    vertex.color2 = float4(
        Upt04UnpackVertexHalf2(packed.color.z),
        Upt04UnpackVertexHalf2(packed.color.w));
    return vertex;
}

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
    // RTXDI-style triangle-footprint emissive evaluation.  Geometry remains
    // sampled at texCoord/barycentrics, but the emitted radiance is constant
    // over the triangle and filtered over its UV footprint.  Keeping these
    // values in-register avoids widening the persistent light/reservoir ABI.
    public float2 emissionTexCoord;
    public float2 emissionUvGradient0;
    public float2 emissionUvGradient1;
    public float2 barycentrics;
};
