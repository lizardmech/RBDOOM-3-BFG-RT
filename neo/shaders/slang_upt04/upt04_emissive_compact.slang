#ifndef UPT04_EMISSIVE_COMPACT_CLASSIFY
#define UPT04_EMISSIVE_COMPACT_CLASSIFY 0
#endif
#ifndef UPT04_EMISSIVE_COMPACT_CONSUME
#define UPT04_EMISSIVE_COMPACT_CONSUME 0
#endif

#if UPT04_EMISSIVE_COMPACT_CLASSIFY || UPT04_EMISSIVE_COMPACT_CONSUME
public [[vk::binding(33, 0)]] RWStructuredBuffer<uint> gUpt56EmissiveQueue;
public [[vk::binding(34, 0)]] RWStructuredBuffer<uint> gUpt56EmissiveMeta;
#endif
#if UPT04_EMISSIVE_COMPACT_CLASSIFY
public [[vk::binding(35, 0)]] RWStructuredBuffer<uint> gUpt56EmissiveDispatchArgs;
#endif

#if UPT04_EMISSIVE_COMPACT_CLASSIFY
static uint gUpt56EmissiveCompactRequested;

public void Upt56ResetEmissiveCompactRequest()
{
    gUpt56EmissiveCompactRequested = 0u;
}

public void Upt56NoteEmissiveCompactNeeded()
{
    gUpt56EmissiveCompactRequested = 1u;
}

public uint Upt56EmissiveCompactRequested()
{
    return gUpt56EmissiveCompactRequested;
}

public void Upt56AppendEmissivePixel(uint index)
{
    uint slot;
    InterlockedAdd(gUpt56EmissiveMeta[0], 1u, slot);
    const uint capacity = gUpt56EmissiveMeta[1];
    if (slot >= capacity)
        return;
    gUpt56EmissiveQueue[slot] = index;
    const uint requiredGroups = (slot + 64u) / 64u;
    uint ignored;
    InterlockedMax(gUpt56EmissiveDispatchArgs[0], requiredGroups, ignored);
}
#else
public void Upt56ResetEmissiveCompactRequest()
{
}

public void Upt56NoteEmissiveCompactNeeded()
{
}

public uint Upt56EmissiveCompactRequested()
{
    return 0u;
}

public void Upt56AppendEmissivePixel(uint index)
{
}

#if UPT04_EMISSIVE_COMPACT_CONSUME
public uint Upt56CompactQueuedCount()
{
    return min(gUpt56EmissiveMeta[0], gUpt56EmissiveMeta[1]);
}

public uint Upt56CompactQueuedIndex(uint slot)
{
    return gUpt56EmissiveQueue[slot];
}
#endif
#endif
