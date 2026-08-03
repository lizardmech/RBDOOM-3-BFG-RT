import upt04_binary16;

public static const uint kUpt04LightInvalid = 0u;
public static const uint kUpt04LightEmissiveTriangle = 1u;
public static const uint kUpt04LightDoomAnalytic = 2u;
public static const uint kUpt04LightCurrentSampleable = 1u << 24u;
public static const uint kUpt04InvalidLightIndex = 0xffffffffu;
public static const uint kUpt04SampleInvalid = 0u;
public static const uint kUpt04SampleValidZero = 1u;
public static const uint kUpt04SampleValidPositive = 2u;

// Exact field order of PathTraceUnifiedLightRecord.  Scalar layout gives the
// manager-owned current payload a 112-byte StructuredBuffer stride.
public struct Upt04UnifiedLightRecord
{
    public float4 positionAndRadius;
    public float4 normalAndArea;
    public float4 radianceAndLuminance;
    public float4 uvOrDoomParams;
    public uint type;
    public uint sourceIndex;
    public uint flags;
    public uint materialOrLightId;
    public uint instanceId;
    public uint primitiveIndex;
    public uint identityA;
    public uint identityB;
    public float sourcePdf;
    public float sourceWeight;
    public uint previousIndex;
    public uint padding0;
};

// UPT-only current-light sidecar. The manager-owned 112-byte record remains
// authoritative. D0 needs no previous-frame link, sourcePdf, or unused Doom
// parameter components, so its replay record fits in four uint4 words.
public struct Upt04CompactLightRecord
{
    public uint4 positionAndRadius;
    public uint4 normalAreaInfluenceRadiance;
    public uint4 radianceAndHeader;
    public uint4 identity;
};

public uint Upt04PackLightHalf2(float2 value)
{
    const float2 finiteValue = clamp(
        float2(
            isfinite(value.x) ? value.x : 0.0f,
            isfinite(value.y) ? value.y : 0.0f),
        float2(-65504.0f),
        float2(65504.0f));
    return Upt04PackBinary16Pair(finiteValue);
}

public float2 Upt04UnpackLightHalf2(uint packed)
{
    return Upt04UnpackBinary16Pair(packed);
}

public uint Upt04EncodeLightOctahedral(float3 value)
{
    const float lengthSquared = dot(value, value);
    value = isfinite(lengthSquared) && lengthSquared > 1.0e-12f
        ? value * rsqrt(lengthSquared)
        : float3(0.0f, 0.0f, 1.0f);
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

public float3 Upt04DecodeLightOctahedral(uint packed)
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

public Upt04UnifiedLightRecord Upt04UnpackCompactLight(
    Upt04CompactLightRecord packed)
{
    Upt04UnifiedLightRecord light = (Upt04UnifiedLightRecord)0;
    const float signedArea = asfloat(packed.normalAreaInfluenceRadiance.y);
    light.positionAndRadius = asfloat(packed.positionAndRadius);
    light.normalAndArea = float4(
        Upt04DecodeLightOctahedral(packed.normalAreaInfluenceRadiance.x),
        abs(signedArea));
    light.radianceAndLuminance = float4(
        Upt04UnpackLightHalf2(packed.normalAreaInfluenceRadiance.w),
        Upt04UnpackLightHalf2(packed.radianceAndHeader.x));
    light.uvOrDoomParams.x = asfloat(packed.normalAreaInfluenceRadiance.z);
    light.type = signedArea > 0.0f
        ? kUpt04LightEmissiveTriangle
        : (signedArea < 0.0f ? kUpt04LightDoomAnalytic : kUpt04LightInvalid);
    light.sourceIndex = packed.radianceAndHeader.y;
    light.flags = packed.radianceAndHeader.z;
    light.materialOrLightId = packed.radianceAndHeader.w;
    light.instanceId = packed.identity.x;
    light.primitiveIndex = packed.identity.y;
    light.identityA = packed.identity.z;
    light.identityB = packed.identity.w;
    // D0 uses sourceWeight only as a positive structural-admission bit. The
    // packer emits positive signed area only when that source field is valid.
    light.sourceWeight = light.type == kUpt04LightEmissiveTriangle ? 1.0f : 0.0f;
    return light;
}

public struct Upt04LocalLightRanges
{
    public uint emissiveStart;
    public uint emissiveCount;
    public uint analyticStart;
    public uint analyticCount;
};

public struct Upt04LocalLightSelection
{
    public uint valid;
    public uint denseIndex;
    public uint type;
    public uint requiresTriangleReplay;
    public float selectionPdf;
    public float2 sampleCoordinates;
    public uint reserved;
};

public struct Upt04LocalLightSample
{
    public uint status;
    public float3 position;
    public float3 normal;
    public float distance;
    public float3 direction;
    public float solidAnglePdf;
    public float3 radiance;
    public uint reserved;
};

public struct Upt04EvaluatedLocalLight
{
    public uint valid;
    public Upt04LocalLightSample sample;
    public float2 storedSampleCoordinates;
};

public struct Upt04EmissiveLookupEntry
{
    public uint instanceId;
    public uint primitiveIndex;
    public uint denseLightIndex;
    public uint occupied;
};

public static const uint kUpt04EmissiveLookupMaxProbes = 16u;

public bool Upt04FinitePositive(float value)
{
    return isfinite(value) && value > 0.0f;
}

public float Upt04BalanceHeuristic(float selectedPdf, float alternatePdf)
{
    if (!Upt04FinitePositive(selectedPdf))
        return 0.0f;
    if (!Upt04FinitePositive(alternatePdf))
        return 1.0f;
    const float sum = selectedPdf + alternatePdf;
    return Upt04FinitePositive(sum) ? selectedPdf / sum : 0.0f;
}

public bool Upt04UnifiedLightRecordStructurallyValid(Upt04UnifiedLightRecord record)
{
    if ((record.flags & kUpt04LightCurrentSampleable) == 0u
        || record.sourceIndex == kUpt04InvalidLightIndex
        || !all(isfinite(record.positionAndRadius.xyz))
        || !all(isfinite(record.radianceAndLuminance.xyz))
        || !Upt04FinitePositive(record.radianceAndLuminance.w))
        return false;

    if (record.type == kUpt04LightEmissiveTriangle)
        return Upt04FinitePositive(record.normalAndArea.w)
            && dot(record.normalAndArea.xyz, record.normalAndArea.xyz) > 1.0e-8f
            && Upt04FinitePositive(record.sourceWeight);

    return record.type == kUpt04LightDoomAnalytic
        && Upt04FinitePositive(record.positionAndRadius.w)
        && Upt04FinitePositive(record.uvOrDoomParams.x);
}

public Upt04CompactLightRecord Upt04PackCompactLight(
    Upt04UnifiedLightRecord light)
{
    Upt04CompactLightRecord packed = (Upt04CompactLightRecord)0;
    float signedArea = 0.0f;
    if (Upt04UnifiedLightRecordStructurallyValid(light))
    {
        signedArea = light.type == kUpt04LightEmissiveTriangle
            ? light.normalAndArea.w
            : -1.0f;
    }
    packed.positionAndRadius = asuint(light.positionAndRadius);
    packed.normalAreaInfluenceRadiance = uint4(
        Upt04EncodeLightOctahedral(light.normalAndArea.xyz),
        asuint(signedArea),
        asuint(light.uvOrDoomParams.x),
        Upt04PackLightHalf2(light.radianceAndLuminance.xy));
    packed.radianceAndHeader = uint4(
        Upt04PackLightHalf2(light.radianceAndLuminance.zw),
        light.sourceIndex,
        light.flags,
        light.materialOrLightId);
    packed.identity = uint4(
        light.instanceId,
        light.primitiveIndex,
        light.identityA,
        light.identityB);
    return packed;
}
