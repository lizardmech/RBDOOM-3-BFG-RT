import upt04_binary16;

public static const uint kUpt04LightInvalid = 0u;
public static const uint kUpt04LightEmissiveTriangle = 1u;
public static const uint kUpt04LightDoomAnalytic = 2u;
public static const uint kUpt04LightCurrentSampleable = 1u << 24u;
public static const uint kUpt04LightStableCacheable = 1u << 25u;
public static const uint kUpt04LightUnstableDynamic = 1u << 26u;
public static const uint kUpt04EmissiveHistoryDynamic = 0x00020000u;
public static const uint kUpt04InvalidLightIndex = 0xffffffffu;
public static const uint kUpt04SampleInvalid = 0u;
public static const uint kUpt04SampleValidZero = 1u;
public static const uint kUpt04SampleValidPositive = 2u;
public static const uint kUpt04AnalyticPortalDomain = 0x10000000u;

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
        signedArea > 0.0f
            ? Upt04DecodeLightOctahedral(packed.normalAreaInfluenceRadiance.x)
            : float3(0.0f, 0.0f, 1.0f),
        abs(signedArea));
    light.radianceAndLuminance = float4(
        Upt04UnpackLightHalf2(packed.normalAreaInfluenceRadiance.w),
        Upt04UnpackLightHalf2(packed.radianceAndHeader.x));
    light.uvOrDoomParams.x = asfloat(packed.normalAreaInfluenceRadiance.z);
    // Analytic spheres do not consume a surface normal. Preserve the current
    // portal-selection depth in that otherwise-dead compact word so the 64 B
    // route and the authoritative 112 B route make identical domain choices.
    if (signedArea < 0.0f)
        light.uvOrDoomParams.z = asfloat(
            packed.normalAreaInfluenceRadiance.x);
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
    public float conditionalIdentityPdf;
};

// Exact scalar layout of PathTraceEmissiveDistributionEntry.  This is the
// manager-owned power/area CDF already consumed by the legacy DI lane; D0 uses
// it only to choose an emissive identity and retains exact material replay for
// the selected triangle's radiance.
public struct Upt04EmissiveDistributionEntry
{
    public uint emissiveTriangleIndex;
    public float cumulativePdf;
    public float weight;
    // Filled by the scene publisher after the current light-manager payload
    // is known.  CDF entry order is not a dense-light indexing contract.
    public uint denseLightIndex;
};

public static const uint kUpt04EmissiveLookupMaxProbes = 16u;

public bool Upt04FinitePositive(float value)
{
    // IEEE-754 binary32 ordering makes every strictly-positive finite value
    // occupy bit patterns [1, 0x7f7fffff].  Subtracting one folds that interval
    // into one unsigned comparison while rejecting +0, both signs of infinity,
    // NaNs, negative values, and -0 exactly as isfinite(value) && value > 0.
    // This predicate is hot inside every RIS candidate evaluation; keeping it
    // off the FP32 comparison/classification pipe materially reduces repeated
    // guard work without weakening invalid-sample rejection.
    return asuint(value) - 1u < 0x7f7fffffu;
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
    if (record.sourceIndex == kUpt04InvalidLightIndex
        || !all(isfinite(record.positionAndRadius.xyz))
        || !all(isfinite(record.radianceAndLuminance.xyz))
        || !Upt04FinitePositive(record.radianceAndLuminance.w))
        return false;

    // CURRENT_SAMPLEABLE is assigned by the manager's Doom-analytic
    // stability classification. Emissive range membership and triangle
    // replay are their admission contract; requiring the analytic-only bit
    // here rejects every otherwise valid emissive before replay.
    if (record.type == kUpt04LightEmissiveTriangle)
        return Upt04FinitePositive(record.normalAndArea.w)
            && dot(record.normalAndArea.xyz, record.normalAndArea.xyz) > 1.0e-8f
            && Upt04FinitePositive(record.sourceWeight);

    return record.type == kUpt04LightDoomAnalytic
        && (record.flags & kUpt04LightCurrentSampleable) != 0u
        && Upt04FinitePositive(record.positionAndRadius.w)
        && Upt04FinitePositive(record.uvOrDoomParams.x);
}

public bool Upt04AnalyticPortalDomainEnabled(uint policyFlags)
{
    return (policyFlags & kUpt04AnalyticPortalDomain) != 0u;
}

public bool Upt04AnalyticLightCurrentPortalEligible(
    Upt04UnifiedLightRecord light,
    uint policyFlags)
{
    // BuildDoomLightPortalSelection writes -1 for lights outside the current
    // selected portal region and a nonnegative portal depth for admitted
    // lights. Keep the global stable universe for remapping, but do not let an
    // unrelated area supply a current-frame direct-light candidate.
    return !Upt04AnalyticPortalDomainEnabled(policyFlags)
        || (isfinite(light.uvOrDoomParams.z)
            && light.uvOrDoomParams.z >= 0.0f);
}

// The light manager remaps the exact tuple
// (type, identityA, identityB, materialOrLightId). Reservoir word3 has room for
// one guard word, so retain a nonzero fingerprint of that tuple. sourceIndex is
// only a frame-local payload address and must never be used as history identity.
public uint Upt04StableLightIdentityFingerprint(Upt04UnifiedLightRecord light)
{
    uint hash = 2166136261u;
    hash = (hash ^ light.type) * 16777619u;
    hash = (hash ^ light.identityA) * 16777619u;
    hash = (hash ^ light.identityB) * 16777619u;
    hash = (hash ^ light.materialOrLightId) * 16777619u;
    hash ^= hash >> 16u;
    hash *= 0x7feb352du;
    hash ^= hash >> 15u;
    hash *= 0x846ca68bu;
    hash ^= hash >> 16u;
    return hash != 0u ? hash : 1u;
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
        light.type == kUpt04LightDoomAnalytic
            ? asuint(light.uvOrDoomParams.z)
            : Upt04EncodeLightOctahedral(light.normalAndArea.xyz),
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
