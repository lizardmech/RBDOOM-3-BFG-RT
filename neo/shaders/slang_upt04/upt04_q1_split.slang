import upt04_geometry_contract;
import upt04_hit_contract;
import upt04_initial_work_contract;
import upt04_transport_initial_bounded;
import upt04_light_contract;

#ifndef UPT04_Q1_SIBLING
#define UPT04_Q1_SIBLING 0
#endif
#ifndef UPT04_Q1_SIBLING_CONSUME
#define UPT04_Q1_SIBLING_CONSUME 0
#endif

// 112-byte token. No float3. Bounce-only produce; pick lives in sibling.
public struct Upt04Q1Token
{
    public uint4 a;
    public uint4 b;
    public uint4 c;
    public uint4 d;
    public uint4 e;
    public uint4 f;
    public uint4 g;
};

#if UPT04_Q1_SIBLING || UPT04_Q1_SIBLING_CONSUME
public [[vk::binding(46, 0)]] RWStructuredBuffer<Upt04Q1Token> gUpt04Q1Queue;
public [[vk::binding(47, 0)]] RWStructuredBuffer<uint> gUpt04Q1Meta;
public [[vk::binding(48, 0)]] RWStructuredBuffer<uint> gUpt04Q1DispatchArgs;
#endif

#if UPT04_Q1_SIBLING
public void Upt04AppendQ1Token(
    Upt04InitialTraceWork work,
    Upt04HitFacts hit,
    Upt04LocalLightSelection selection)
{
    if (work.valid == 0u)
        return;
    uint slot = 0u;
    InterlockedAdd(gUpt04Q1Meta[0], 1u, slot);
    const uint capacity = gUpt04Q1Meta[1];
    if (slot >= capacity)
        return;
    Upt04Q1Token token;
    token.a = uint4(
        work.surfaceIndex,
        hit.geometryId,
        selection.type
            | (selection.requiresTriangleReplay << 8u)
            | (selection.valid << 9u)
            | (work.eventKind << 16u),
        work.replayKey);
    token.b = uint4(
        work.replayIndex,
        hit.instanceId,
        hit.primitiveId,
        hit.status
            | ((hit.frontFace & 1u) << 8u)
            | ((work.transport.budget.visibilityRays & 3u) << 9u));
    token.c = uint4(
        asuint(hit.barycentrics.x),
        asuint(hit.barycentrics.y),
        asuint(hit.hitT),
        asuint(work.proposalPdf));
    token.d = uint4(
        asuint(work.pathPdf),
        asuint(work.pathThroughput.x),
        asuint(work.pathThroughput.y),
        asuint(work.pathThroughput.z));
    token.e = uint4(
        asuint(work.ray.origin.x),
        asuint(work.ray.origin.y),
        asuint(work.ray.origin.z),
        asuint(work.ray.tMin));
    token.f = uint4(
        asuint(work.ray.direction.x),
        asuint(work.ray.direction.y),
        asuint(work.ray.direction.z),
        asuint(work.ray.tMax));
    token.g = uint4(
        work.lobeMask,
        work.candidateFlags,
        asuint(work.accumulatedRouletteProbability),
        asuint(work.partialJacobian));
    gUpt04Q1Queue[slot] = token;
    const uint requiredGroups = (slot + 64u) / 64u;
    uint ignored;
    InterlockedMax(gUpt04Q1DispatchArgs[0], requiredGroups, ignored);
}
#endif

#if UPT04_Q1_SIBLING_CONSUME
public uint Upt04Q1QueuedCount()
{
    return min(gUpt04Q1Meta[0], gUpt04Q1Meta[1]);
}

public Upt04Q1Token Upt04Q1QueuedToken(uint slot)
{
    return gUpt04Q1Queue[slot];
}

public void Upt04Q1Unpack(
    Upt04Q1Token token,
    out Upt04InitialTraceWork work,
    out Upt04HitFacts hit,
    out Upt04LocalLightSelection selection)
{
    work = (Upt04InitialTraceWork)0;
    hit = (Upt04HitFacts)0;
    selection = (Upt04LocalLightSelection)0;
    work.valid = 1u;
    work.traceRequired = 1u;
    work.familyKind = kUpt04InitialFamilyIndirect;
    work.surfaceIndex = token.a.x;
    work.replayKey = token.a.w;
    work.replayIndex = token.b.x;
    work.pathPdf = asfloat(token.d.x);
    work.pathThroughput = float3(
        asfloat(token.d.y), asfloat(token.d.z), asfloat(token.d.w));
    work.ray.origin = float3(
        asfloat(token.e.x), asfloat(token.e.y), asfloat(token.e.z));
    work.ray.tMin = asfloat(token.e.w);
    work.ray.direction = float3(
        asfloat(token.f.x), asfloat(token.f.y), asfloat(token.f.z));
    work.ray.tMax = asfloat(token.f.w);
    work.eventKind = token.a.z >> 16u;
    if (work.eventKind == 0u)
        work.eventKind = 4u;
    work.proposalPdf = asfloat(token.c.w);
    work.lobeMask = token.g.x;
    work.candidateFlags = token.g.y;
    work.accumulatedRouletteProbability = asfloat(token.g.z);
    work.partialJacobian = asfloat(token.g.w);
    work.transport.valid = 1u;
    work.transport.stage = kUpt04TransportIndirectContinuation;
    // First continuation (x2). AdmitSecondIndirectContinuation requires
    // continuationRays==1; 2 is the post-admit x3-token value and drops x3.
    work.transport.budget.continuationRays = 1u;
    work.transport.budget.visibilityRays = min((token.b.w >> 9u) & 3u, 1u);
    hit.status = token.b.w & 0xffu;
    hit.instanceId = token.b.y;
    hit.primitiveId = token.b.z;
    hit.geometryId = token.a.y;
    hit.barycentrics = float2(asfloat(token.c.x), asfloat(token.c.y));
    hit.hitT = asfloat(token.c.z);
    hit.frontFace = (token.b.w >> 8u) & 1u;
    selection.valid = (token.a.z >> 9u) & 1u;
    selection.denseIndex = 0u;
    selection.type = token.a.z & 0xffu;
    selection.requiresTriangleReplay = (token.a.z >> 8u) & 1u;
}
#endif