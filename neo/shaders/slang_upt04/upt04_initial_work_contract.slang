import upt04_hit_contract;
import upt04_transport_initial_bounded;

public static const uint kUpt04InitialFamilyNone = 0u;
public static const uint kUpt04InitialFamilyDirect = 1u;
public static const uint kUpt04InitialFamilyIndirect = 2u;

public static const uint kUpt04CandidateFlagGlobal = 1u << 1u;
public static const uint kUpt04CandidateFlagVisibilityKnown = 1u << 2u;
public static const uint kUpt04CandidateFlagVisibilityPassed = 1u << 3u;
public static const uint kUpt04CandidateFlagReplayable = 1u << 4u;
public static const uint kUpt04CandidateFlagNeedsShift = 1u << 5u;

public struct Upt04InitialTraceWork
{
    public uint valid;
    public uint candidateReady;
    public uint candidateStatus;
    public uint traceRequired;
    public uint surfaceIndex;
    public uint familyKind;
    public uint eventKind;
    public uint lobeMask;
    public uint candidateFlags;
    public uint identity0;
    public uint identity1;
    public uint replayKey;
    public uint replayIndex;
    public uint pathLength;
    public uint reconnectionVertexLength;
    public uint lightIndex;
    public uint lightType;
    public float proposalPdf;
    public float pathPdf;
    public float partialJacobian;
    public float accumulatedRouletteProbability;
    public float2 sampleCoordinates;
    public float3 contribution;
    public float target;
    public float3 pathThroughput;
    public Upt04TraceRequest ray;
    public Upt04BoundedTransportState transport;
};

public Upt04InitialTraceWork Upt04InvalidateInitialWork(
    Upt04InitialTraceWork work)
{
    work.valid = 0u;
    work.candidateReady = 0u;
    work.traceRequired = 0u;
    return work;
}
