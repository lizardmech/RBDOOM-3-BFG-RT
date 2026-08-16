import upt04_hit_contract;

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

#if UPT04_EMISSIVE_COMPACT_CLASSIFY || UPT04_EMISSIVE_COMPACT_CONSUME
public [[vk::binding(33, 0)]] RWStructuredBuffer<Upt56EmissiveToken> gUpt56EmissiveQueue;
public [[vk::binding(34, 0)]] RWStructuredBuffer<uint> gUpt56EmissiveMeta;
#endif
#if UPT04_EMISSIVE_COMPACT_CLASSIFY
public [[vk::binding(35, 0)]] RWStructuredBuffer<uint> gUpt56EmissiveDispatchArgs;
#endif

#if UPT04_EMISSIVE_COMPACT_CLASSIFY
static uint gUpt56EmissiveCompactKind;

public void Upt56ResetEmissiveCompactRequest()
{
    gUpt56EmissiveCompactKind = 0u;
}

public void Upt56NoteEmissiveCompactNeeded()
{
    gUpt56EmissiveCompactKind |= kUpt56KindNee2;
}

public void Upt56NoteEmissiveCompactKind(uint kind)
{
    gUpt56EmissiveCompactKind |= kind;
}

public uint Upt56EmissiveCompactRequested()
{
    return gUpt56EmissiveCompactKind;
}

public void Upt56AppendEmissiveToken(uint index, Upt04HitFacts hit)
{
    const uint kind = gUpt56EmissiveCompactKind;
    if (kind == 0u)
        return;
    uint slot;
    InterlockedAdd(gUpt56EmissiveMeta[0], 1u, slot);
    const uint capacity = gUpt56EmissiveMeta[1];
    if (slot >= capacity)
        return;
    Upt56EmissiveToken token = (Upt56EmissiveToken)0;
    token.surfaceIndex = index;
    token.kind = kind;
    token.instanceId = hit.instanceId;
    token.primitiveId = hit.primitiveId;
    token.barycentrics = hit.barycentrics;
    gUpt56EmissiveQueue[slot] = token;
    const uint requiredGroups = (slot + 64u) / 64u;
    uint ignored;
    InterlockedMax(gUpt56EmissiveDispatchArgs[0], requiredGroups, ignored);
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
    return min(gUpt56EmissiveMeta[0], gUpt56EmissiveMeta[1]);
}

public Upt56EmissiveToken Upt56CompactQueuedToken(uint slot)
{
    return gUpt56EmissiveQueue[slot];
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
