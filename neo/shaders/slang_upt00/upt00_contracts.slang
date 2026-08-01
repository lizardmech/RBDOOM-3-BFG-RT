// UPT-00 comparison-only ABI. This module is intentionally independent from
// every production renderer and shader include. Its import is also the Slang
// depfile rebuild probe; changing this module must rebuild all three consumers.

public struct Upt00StructuredMathInput
{
    public uint4 words;
    public float4 values;
};

public struct Upt00StructuredMathOutput
{
    public uint4 words;
    public float4 values;
};

public struct Upt00RayInput
{
    public float3 origin;
    public float tMin;
    public float3 direction;
    public float tMax;
};

public struct Upt00RayOutput
{
    public uint hit;
    public uint instanceId;
    public uint primitiveId;
    public uint geometryId;
    public float hitT;
    public float2 barycentrics;
    public uint frontFace;
};

public struct Upt00Candidate
{
    public float weight;
    public float target;
    public uint sampleId;
    public uint status;
    public float3 radiance;
    public uint padding;
};

public struct Upt00Reservoir
{
    public float weightSum;
    public float selectedTarget;
    public uint selectedId;
    public uint effectiveM;
    public float3 selectedRadiance;
    public uint flags;
};

public uint Upt00Hash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

public float Upt00UnitFloat(uint value)
{
    return float(value >> 8u) * (1.0f / 16777216.0f);
}
