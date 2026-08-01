#ifndef RB_UPT00_CONTRACTS_HLSLI
#define RB_UPT00_CONTRACTS_HLSLI

// DXC mirror of the comparison-only Slang ABI. UPT-00 readback must prove
// these declarations agree; neither file is a production renderer contract.
// This include is the DXC depfile rebuild probe for all three consumers.

struct Upt00StructuredMathInput
{
    uint4 words;
    float4 values;
};

struct Upt00StructuredMathOutput
{
    uint4 words;
    float4 values;
};

struct Upt00RayInput
{
    float3 origin;
    float tMin;
    float3 direction;
    float tMax;
};

struct Upt00RayOutput
{
    uint hit;
    uint instanceId;
    uint primitiveId;
    uint geometryId;
    float hitT;
    float2 barycentrics;
    uint frontFace;
};

struct Upt00Candidate
{
    float weight;
    float target;
    uint sampleId;
    uint status;
    float3 radiance;
    uint padding;
};

struct Upt00Reservoir
{
    float weightSum;
    float selectedTarget;
    uint selectedId;
    uint effectiveM;
    float3 selectedRadiance;
    uint flags;
};

uint Upt00Hash(uint value)
{
    uint state = value * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float Upt00UnitFloat(uint value)
{
    return float(value >> 8u) * (1.0f / 16777216.0f);
}

#endif
