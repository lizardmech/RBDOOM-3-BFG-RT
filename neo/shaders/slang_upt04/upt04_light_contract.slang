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

public bool Upt04FinitePositive(float value)
{
    return isfinite(value) && value > 0.0f;
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
