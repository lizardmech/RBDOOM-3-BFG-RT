import upt04_binary16;
import upt04_packed_primary_surface;
import upt04_packed_primary_surface32;
import upt04_primary_history_sidecar;

public static const uint kUpt45SurfaceClassMask = 0xffu;
public static const uint kUpt45SurfaceClassTranslucent = 3u;
public static const uint kUpt45MaterialAlphaTest = 0x00000001u;
public static const uint kUpt45MaterialDiffuseYCoCg = 0x00000002u;
public static const uint kUpt45MaterialEmissive = 0x00000008u;
public static const uint kUpt45MaterialForceDebugAlbedo = 0x00000080u;
public static const uint kUpt45MaterialPortalWindow = 0x00000200u;
public static const uint kUpt45MaterialObjectGlass = 0x00000400u;
public static const uint kUpt45TriangleEmissiveStageOff = 0x00040000u;
public static const uint kUpt45Invalid = 0xffffffffu;
public static const uint kUpt45StaticBucketInstanceBit = 0x00800000u;
public static const uint kUpt45StaticBucketTriangleMask = 0x007fffffu;
public static const uint kUpt45ShaderInstanceMask = 0x00ffffffu;
public static const uint kUpt45GeometryChunkTriangles = 256u;
public static const uint kUpt45PrimaryValid = 0x00000001u;
public static const uint kUpt45PrimaryObjectMotion = 0x00000004u;
public static const uint kUpt45PrimaryPreviousPosition = 0x00000008u;
public static const uint kUpt45PrimaryGlassTransmissionEligible = 0x00010000u;
public static const uint kUpt45ControlWriteRrGuides = 0x00000001u;
public static const uint kUpt45MaterialKindTranslucentGlass = 5u;
public static const uint kUpt45MaterialCapPathTransmission = 0x00000010u;
public static const uint kUpt45MaterialCapDebugFailClosed = 0x80000000u;
public static const uint kUpt45MaterialLobeSpecularTransmission = 0x00000008u;
public static const uint kUpt45MaterialPassTransmissionProducer = 0x00000020u;
public static const uint kUpt45MaterialFeatureAbiVersion = 4u;
public static const uint kUpt45MotionValid = 0x00000001u;
public static const uint kUpt45MotionSourceShift = 1u;
public static const uint kUpt45MotionInvalidReasonShift = 5u;
public static const uint kUpt45MotionMissingPreviousCamera = 4u;
public static const uint kUpt45MotionRejectedPrevious = 5u;
public static const uint kUpt45MotionPreviousBehindCamera = 6u;
public static const uint kUpt45RrResetStochasticTranslucent = 0x00000020u;

public struct Upt45Control
{
    public uint width;
    public uint height;
    public uint surfaceCount;
    public uint materialCount;
    public uint logicalTextureCount;
    public uint staticVertexCount;
    public uint staticIndexCount;
    public uint staticTriangleCount;
    public uint dynamicVertexCount;
    public uint dynamicIndexCount;
    public uint dynamicTriangleCount;
    public uint rigidVertexCount;
    public uint rigidIndexCount;
    public uint rigidInstanceCount;
    public uint skinnedVertexCount;
    public uint skinnedIndexCount;
    public uint skinnedRouteCount;
    public uint skinnedTriangleCount;
    public uint skinnedPreviousCount;
    public uint flags;
    public float3 cameraOrigin;
    public float emissiveScale;
    public float forwardOffset;
    public uint materialFeatureCount;
    public uint reserved0;
    public float rrNear;
    public float4 cameraForwardAndTanX;
    public float4 cameraLeftAndTanY;
    public float4 cameraUpAndPreviousValid;
    public float4 previousCameraOrigin;
    public float4 previousCameraForwardAndTanX;
    public float4 previousCameraLeftAndTanY;
    public float4 previousCameraUp;
};

public struct Upt45NarrowHit
{
    public uint valid;
    public uint instanceId;
    public uint primitiveId;
    public uint frontFace;
    public float2 barycentrics;
    public float hitT;
    public uint reserved0;
};

public struct Upt45NarrowSurface
{
    public uint valid;
    public uint materialIndex;
    public uint materialId;
    public uint triangleClassAndFlags;
    public float3 position;
    public float3 previousPosition;
    public float3 geometricNormal;
    public float3 shadingNormal;
    public float2 texCoord;
    public uint hasPreviousPosition;
};

public float3 Upt45SafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    if (all(isfinite(value)) && isfinite(lengthSquared) && lengthSquared > 1.0e-12f)
        return value * rsqrt(lengthSquared);
    const float fallbackLengthSquared = dot(fallback, fallback);
    return all(isfinite(fallback)) && isfinite(fallbackLengthSquared) &&
            fallbackLengthSquared > 1.0e-12f
        ? fallback * rsqrt(fallbackLengthSquared)
        : float3(0.0f, 0.0f, 1.0f);
}

public uint Upt45PackRgb10(float3 value)
{
    const uint3 packed = uint3(round(saturate(
        all(isfinite(value)) ? value : float3(0.0f)) * 1023.0f));
    return (packed.x & 0x3ffu) | ((packed.y & 0x3ffu) << 10u) |
        ((packed.z & 0x3ffu) << 20u);
}

public uint Upt45PackOctahedral(float3 value)
{
    value = Upt45SafeNormalize(value, float3(0.0f, 0.0f, 1.0f));
    value /= max(abs(value.x) + abs(value.y) + abs(value.z), 1.0e-12f);
    if (value.z < 0.0f)
    {
        const float2 signs = float2(
            value.x >= 0.0f ? 1.0f : -1.0f,
            value.y >= 0.0f ? 1.0f : -1.0f);
        value.xy = (1.0f - abs(value.yx)) * signs;
    }
    const uint2 encoded = uint2(round(saturate(value.xy * 0.5f + 0.5f) * 65535.0f));
    return (encoded.x & 0xffffu) | ((encoded.y & 0xffffu) << 16u);
}

public uint Upt45PackSharedExponent(float3 value)
{
    const float3 finiteValue = all(isfinite(value))
        ? min(max(value, float3(0.0f)), float3(65408.0f))
        : float3(0.0f);
    const float maximum = max(finiteValue.x, max(finiteValue.y, finiteValue.z));
    if (maximum <= 0.0f)
        return 0u;
    uint exponent = (uint)clamp((int)floor(log2(maximum)) + 16, 0, 31);
    float scale = exp2(24.0f - float(exponent));
    uint3 mantissa = uint3(round(finiteValue * scale));
    if (max(mantissa.x, max(mantissa.y, mantissa.z)) > 511u && exponent < 31u)
    {
        ++exponent;
        scale *= 0.5f;
        mantissa = uint3(round(finiteValue * scale));
    }
    mantissa = min(mantissa, uint3(511u));
    return mantissa.x | (mantissa.y << 9u) | (mantissa.z << 18u) |
        (exponent << 27u);
}

public bool Upt45FrontIsEligible(
    Upt04PackedPrimarySurfaceRecord32 packed,
    Upt04PrimaryHistorySidecar sidecar)
{
    const uint header = packed.material.w >> 16u;
    return (header & 0x7fffu) == kUpt04PackedPrimary32Version &&
        (header & kUpt04PackedPrimary32Valid) != 0u &&
        (sidecar.metadata.x & 0xffffu) == kUpt04PrimaryHistorySidecarVersion &&
        (sidecar.metadata.y & kUpt45PrimaryGlassTransmissionEligible) != 0u;
}
