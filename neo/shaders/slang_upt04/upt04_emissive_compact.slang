import upt04_hit_contract;
import upt04_binary16;

#ifndef UPT04_EMISSIVE_COMPACT_CLASSIFY
#define UPT04_EMISSIVE_COMPACT_CLASSIFY 0
#endif
#ifndef UPT04_EMISSIVE_COMPACT_CONSUME
#define UPT04_EMISSIVE_COMPACT_CONSUME 0
#endif

public static const uint kUpt56KindHit2 = 1u;
public static const uint kUpt56KindNee2 = 2u;

public struct Upt56EmissiveToken
{
    public uint surfaceIndex;
    public uint kind;
    public uint instanceId;
    public uint primitiveId;
    public float2 barycentrics;
    public uint reserved0;
    public uint reserved1;
};

#if 0
public [[vk::binding(33, 0)]] RWStructuredBuffer<Upt56EmissiveToken> gUpt56EmissiveQueue;
public [[vk::binding(34, 0)]] RWStructuredBuffer<uint> gUpt56EmissiveMeta;
public [[vk::binding(35, 0)]] RWStructuredBuffer<uint> gUpt56EmissiveDispatchArgs;
#endif

#if UPT04_EMISSIVE_COMPACT_CLASSIFY
static uint gUpt56EmissiveCompactKind;
static uint gUpt56NeeLightIndex;
static float2 gUpt56NeeSample;
static float gUpt56NeeMass;
static uint gUpt56NeeCandidateCount;

public void Upt56ResetEmissiveCompactRequest()
{
    gUpt56EmissiveCompactKind = 0u;
    gUpt56NeeLightIndex = 0u;
    gUpt56NeeSample = float2(0.0f);
    gUpt56NeeMass = 0.0f;
    gUpt56NeeCandidateCount = 0u;
}

public void Upt56NoteEmissiveCompactNeeded()
{
    gUpt56EmissiveCompactKind |= kUpt56KindNee2;
}

public void Upt56NoteEmissiveCompactKind(uint kind)
{
    gUpt56EmissiveCompactKind |= kind;
}

public void Upt56SetDeferredNee(
    uint lightIndex,
    float2 sampleCoordinates,
    float finalizedMass,
    uint candidateCount)
{
    gUpt56NeeLightIndex = lightIndex;
    gUpt56NeeSample = sampleCoordinates;
    gUpt56NeeMass = finalizedMass;
    gUpt56NeeCandidateCount = candidateCount;
    gUpt56EmissiveCompactKind |= kUpt56KindNee2;
}

public uint Upt56EmissiveCompactRequested()
{
    return gUpt56EmissiveCompactKind;
}

public void Upt56AppendEmissiveToken(uint index, Upt04HitFacts hit)
{
}

public void Upt56AppendEmissivePixel(uint index)
{
    Upt56AppendEmissiveToken(index, (Upt04HitFacts)0);
}
#elif UPT04_EMISSIVE_COMPACT_CONSUME
public void Upt56ResetEmissiveCompactRequest()
{
}

public void Upt56NoteEmissiveCompactNeeded()
{
}

public void Upt56NoteEmissiveCompactKind(uint kind)
{
}

public void Upt56SetDeferredNee(
    uint lightIndex,
    float2 sampleCoordinates,
    float finalizedMass,
    uint candidateCount)
{
}

public uint Upt56EmissiveCompactRequested()
{
    return 0u;
}

public void Upt56AppendEmissivePixel(uint index)
{
}

public void Upt56AppendEmissiveToken(uint index, Upt04HitFacts hit)
{
}

public uint Upt56CompactQueuedCount()
{
    return 0u;
}

public Upt56EmissiveToken Upt56CompactQueuedToken(uint slot)
{
    return (Upt56EmissiveToken)0;
}

public uint Upt56TokenKindFlags(Upt56EmissiveToken token)
{
    return token.kind & 0xffu;
}

public uint Upt56TokenNeeLightIndex(Upt56EmissiveToken token)
{
    return token.reserved0;
}

public float2 Upt56TokenNeeSample(Upt56EmissiveToken token)
{
    return Upt04UnpackBinary16Pair(token.reserved1);
}

public float Upt56TokenNeeMass(Upt56EmissiveToken token)
{
    return Upt04UnpackBinary16Pair(token.kind >> 16u).x;
}

public uint Upt56TokenNeeCandidateCount(Upt56EmissiveToken token)
{
    return (token.kind >> 8u) & 0xffu;
}
#else
public void Upt56ResetEmissiveCompactRequest()
{
}

public void Upt56NoteEmissiveCompactNeeded()
{
}

public void Upt56NoteEmissiveCompactKind(uint kind)
{
}

public void Upt56SetDeferredNee(
    uint lightIndex,
    float2 sampleCoordinates,
    float finalizedMass,
    uint candidateCount)
{
}

public uint Upt56EmissiveCompactRequested()
{
    return 0u;
}

public void Upt56AppendEmissivePixel(uint index)
{
}

public void Upt56AppendEmissiveToken(uint index, Upt04HitFacts hit)
{
}
#endif
