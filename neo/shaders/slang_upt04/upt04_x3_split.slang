import upt04_geometry_contract;
import upt04_hit_contract;
import upt04_initial_work_contract;
import upt04_transport_initial_bounded;

#ifndef UPT04_THREE_VERTEX_SPLIT_PRODUCE
#define UPT04_THREE_VERTEX_SPLIT_PRODUCE 0
#endif
#ifndef UPT04_THREE_VERTEX_SPLIT_CONSUME
#define UPT04_THREE_VERTEX_SPLIT_CONSUME 0
#endif

#if UPT04_THREE_VERTEX_SPLIT_PRODUCE || UPT04_THREE_VERTEX_SPLIT_CONSUME
import upt04_initial_multibounce;
#endif

// Explicit 128-byte / 32-word token. Do not put float3 in this struct:
// std430 aligns float3 to 16 bytes and the host stride then desynchronizes.
// Produce runs PrepareSecondContinuation on the live x2 surface and writes
// the already-admitted x3 ray. Consume traces that ray; it does not Prepare.
public struct Upt04X3Token
{
    public uint4 a;
    public uint4 b;
    public uint4 c;
    public uint4 d;
    public uint4 e;
    public uint4 f;
    public uint4 g;
    public uint4 h;
};

#if UPT04_THREE_VERTEX_SPLIT_PRODUCE || UPT04_THREE_VERTEX_SPLIT_CONSUME
public [[vk::binding(37, 0)]] RWStructuredBuffer<Upt04X3Token> gUpt04X3Queue;
public [[vk::binding(38, 0)]] RWStructuredBuffer<uint> gUpt04X3Meta;
public [[vk::binding(39, 0)]] RWStructuredBuffer<uint> gUpt04X3DispatchArgs;
#endif

#if UPT04_THREE_VERTEX_SPLIT_PRODUCE
public Upt04X3Token Upt04X3PackPrepared(
    uint surfaceIndex,
    Upt04SecondContinuationResult prepared,
    float3 x2Position,
    uint visibilityRays)
{
    Upt04X3Token token;
    token.a = uint4(
        surfaceIndex,
        visibilityRays & 0xffu,
        prepared.work.replayKey,
        prepared.work.replayIndex);
    token.b = uint4(
        prepared.work.lobeMask,
        asuint(prepared.localDirectionalPdf),
        asuint(prepared.work.pathPdf),
        asuint(prepared.work.accumulatedRouletteProbability));
    token.c = uint4(
        asuint(prepared.work.pathThroughput.x),
        asuint(prepared.work.pathThroughput.y),
        asuint(prepared.work.pathThroughput.z),
        asuint(prepared.work.partialJacobian));
    token.d = uint4(
        asuint(prepared.work.ray.origin.x),
        asuint(prepared.work.ray.origin.y),
        asuint(prepared.work.ray.origin.z),
        asuint(prepared.work.ray.tMin));
    token.e = uint4(
        asuint(prepared.work.ray.direction.x),
        asuint(prepared.work.ray.direction.y),
        asuint(prepared.work.ray.direction.z),
        asuint(prepared.work.ray.tMax));
    token.f = uint4(
        asuint(x2Position.x),
        asuint(x2Position.y),
        asuint(x2Position.z),
        prepared.work.candidateFlags);
    token.g = uint4(0u, 0u, 0u, 0u);
    token.h = uint4(0u, 0u, 0u, 0u);
    return token;
}

public void Upt04AppendX3Token(
    uint surfaceIndex,
    Upt04SecondContinuationResult prepared,
    float3 x2Position,
    uint visibilityRays)
{
    if (prepared.work.valid == 0u || prepared.work.traceRequired == 0u)
        return;
    uint slot = 0u;
    InterlockedAdd(gUpt04X3Meta[0], 1u, slot);
    const uint capacity = gUpt04X3Meta[1];
    if (slot >= capacity)
        return;
    gUpt04X3Queue[slot] = Upt04X3PackPrepared(
        surfaceIndex, prepared, x2Position, visibilityRays);
    const uint requiredGroups = (slot + 64u) / 64u;
    uint ignored;
    InterlockedMax(gUpt04X3DispatchArgs[0], requiredGroups, ignored);
}
#endif

#if UPT04_THREE_VERTEX_SPLIT_CONSUME
public uint Upt04X3QueuedCount()
{
    return min(gUpt04X3Meta[0], gUpt04X3Meta[1]);
}

public Upt04X3Token Upt04X3QueuedToken(uint slot)
{
    return gUpt04X3Queue[slot];
}

public Upt04SecondContinuationResult Upt04X3UnpackPrepared(Upt04X3Token token)
{
    Upt04SecondContinuationResult result = (Upt04SecondContinuationResult)0;
    Upt04InitialTraceWork work = (Upt04InitialTraceWork)0;
    work.valid = 1u;
    work.traceRequired = 1u;
    work.familyKind = kUpt04InitialFamilyIndirect;
    work.surfaceIndex = token.a.x;
    work.replayKey = token.a.z;
    work.replayIndex = token.a.w;
    work.lobeMask = token.b.x;
    work.pathPdf = asfloat(token.b.z);
    work.accumulatedRouletteProbability = asfloat(token.b.w);
    work.pathThroughput = float3(
        asfloat(token.c.x), asfloat(token.c.y), asfloat(token.c.z));
    work.partialJacobian = asfloat(token.c.w);
    work.candidateFlags = token.f.w;
    work.ray.origin = float3(
        asfloat(token.d.x), asfloat(token.d.y), asfloat(token.d.z));
    work.ray.tMin = asfloat(token.d.w);
    work.ray.direction = float3(
        asfloat(token.e.x), asfloat(token.e.y), asfloat(token.e.z));
    work.ray.tMax = asfloat(token.e.w);
    work.transport.valid = 1u;
    work.transport.stage = kUpt04TransportIndirectContinuation;
    work.transport.budget.continuationRays = 2u;
    work.transport.budget.visibilityRays = min(token.a.y & 0xffu, 2u);
    result.work = work;
    result.localDirectionalPdf = asfloat(token.b.y);
    return result;
}

public Upt04GeometryHitSurface Upt04X3UnpackX2Surface(Upt04X3Token token)
{
    Upt04GeometryHitSurface surface = (Upt04GeometryHitSurface)0;
    surface.valid = 1u;
    surface.position = float3(
        asfloat(token.f.x), asfloat(token.f.y), asfloat(token.f.z));
    return surface;
}
#endif