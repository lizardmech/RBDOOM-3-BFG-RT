import upt04_hit_contract;
import upt04_transport_initial_bounded;

public static const uint kUpt04InitialFamilyNone = 0u;
public static const uint kUpt04InitialFamilyDirect = 1u;
public static const uint kUpt04InitialFamilyIndirect = 2u;

// Bit zero records the D0 proposal mixture. Reuse must replay the same source
// PDF even if the live CVar changes after this reservoir was published.
public static const uint kUpt04CandidateFlagDirectProposalParity = 1u << 0u;
public static const uint kUpt04CandidateFlagGlobal = 1u << 1u;
public static const uint kUpt04CandidateFlagVisibilityKnown = 1u << 2u;
public static const uint kUpt04CandidateFlagVisibilityPassed = 1u << 3u;
public static const uint kUpt04CandidateFlagReplayable = 1u << 4u;
public static const uint kUpt04CandidateFlagNeedsShift = 1u << 5u;

// Once a reservoir update has completed, proposalPdf is no longer read by that
// update's estimator. Emissive winners persist their receiver-invariant
// four-region PDF scale as a negative finite value in word12, while pathPdf
// retains the physical source PDF needed by later shift math. All actual
// proposal PDFs are strictly positive, so the sign is a collision-free union
// discriminator. UPT04_ABI_VERSION owns cross-frame invalidation.

public float Upt04EncodeEmissiveReplayPdfScale(float scale)
{
    return isfinite(scale) && scale > 0.0f && scale <= 4.0f
        ? -scale : 0.0f;
}

public bool Upt04DecodeEmissiveReplayPdfScale(
    float payload,
    out float scale)
{
    scale = -payload;
    return isfinite(payload) && payload < 0.0f
        && scale > 0.0f && scale <= 4.0f;
}

// Diagnostic-only D0 acquisition stages. These remain in transient work and
// are never packed into the six-bit persistent candidate flag field.
public static const uint kUpt04D0DiagPreviousLookupAttempted = 1u << 0u;
public static const uint kUpt04D0DiagPreviousLookupSucceeded = 1u << 1u;
public static const uint kUpt04D0DiagPreviousRemapFailure = 1u << 2u;
public static const uint kUpt04D0DiagPreviousReplayFailure = 1u << 3u;
public static const uint kUpt04D0DiagPreviousTargetFailure = 1u << 4u;
public static const uint kUpt04D0DiagPreviousDuplicateRejected = 1u << 5u;
public static const uint kUpt04D0DiagPreviousAdmitted = 1u << 6u;
public static const uint kUpt04D0DiagPreviousSelected = 1u << 7u;
public static const uint kUpt04D0DiagFreshAnalyticSelected = 1u << 8u;
public static const uint kUpt04D0DiagFreshEmissiveSelected = 1u << 9u;
public static const uint kUpt04D0DiagWeightedCdfUnavailable = 1u << 10u;
public static const uint kUpt04D0DiagPreviousVisibilityFailure = 1u << 11u;

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
    public uint reconnectionInstanceId;
    public uint reconnectionPrimitiveId;
    public uint reconnectionFrontFace;
    public float2 reconnectionBarycentrics;
    public float2 sampleCoordinates;
    public float3 contribution;
    public float target;
    public uint directDiagnostics;
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
