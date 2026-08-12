import upt03_reservoir_contract;
import upt04_initial_bindings;
import upt04_initial_random;
import upt04_initial_work_contract;
import upt04_light_contract;
import upt04_random_white_hash;
import upt04_random_contract;

public void Upt04StreamInitialTraceWork(
    inout Upt03LogicalReservoir reservoir,
    Upt04InitialTraceWork work,
    uint2 pixel,
    uint candidateOrdinal)
{
    if (work.valid == 0u || work.candidateReady == 0u)
        return;
    Upt03Candidate candidate = (Upt03Candidate)0;
    candidate.status = work.candidateStatus;
    candidate.eventKind = work.eventKind;
    candidate.lobeMask = work.lobeMask;
    candidate.flags = work.candidateFlags;
    candidate.pathLength = work.pathLength;
    candidate.reconnectionVertexLength = work.reconnectionVertexLength;
    candidate.identity0 = work.identity0;
    candidate.identity1 = work.identity1;
    candidate.replayKey = work.replayKey;
    candidate.replayIndex = work.replayIndex;
    candidate.generationFingerprint = kUpt03ExpectedGeneration;
    candidate.sampleCoord0 = work.sampleCoordinates.x;
    candidate.sampleCoord1 = work.sampleCoordinates.y;
    if (candidate.status == kUpt04SampleValidPositive)
    {
        candidate.contributionR = work.contribution.x;
        candidate.contributionG = work.contribution.y;
        candidate.contributionB = work.contribution.z;
        candidate.target = work.target;
    }
    candidate.proposalPdf = work.proposalPdf;
    candidate.pathPdf = work.pathPdf;
    candidate.partialJacobian = work.partialJacobian;
    candidate.accumulatedRouletteProbability =
        work.accumulatedRouletteProbability;
    candidate.reconnectionPosition = work.reconnectionPosition;
    candidate.reconnectionNormalPacked = work.reconnectionNormalPacked;
    if (Upt03HasCachedX2Endpoint(candidate))
    {
        candidate.contributionR = work.reconnectionSuffixRadiance.x;
        candidate.contributionG = work.reconnectionSuffixRadiance.y;
        candidate.contributionB = work.reconnectionSuffixRadiance.z;
    }
    Upt03StreamCandidate(
        reservoir,
        candidate,
        Upt04WhiteHash(Upt04InitialRandomKeyAtVertex(
            pixel,
            candidateOrdinal,
            kUpt04StreamInitialReservoirSelection,
            kUpt04DimensionInitialReservoirSelection)));
}

public Upt03PackedReservoir Upt04FinalizeInitialReservoir(
    inout Upt03LogicalReservoir reservoir)
{
    if (reservoir.effectiveM != 0u)
    {
        // These are additive path-tree estimator terms, not homogeneous draws
        // from a top-level technique mixture. Primary NEE has already folded
        // its inner RIS trial count into its UCW; endpoint emission and
        // secondary NEE estimate different path-length terms. Keep their summed
        // mass and publish the complete current path tree with confidence c=1.
        // Dividing by the number of streamed event families darkens unified
        // output whenever another valid path term is admitted.
        reservoir.effectiveM = 1u;
    }
    if (reservoir.hasSelectedSample != 0u
        && reservoir.selected.eventKind == 2u)
    {
        const float replayPdfPayload = Upt04EncodeEmissiveReplayPdfScale(
            reservoir.selected.partialJacobian);
        if (Upt03EmissiveReplayPdfScalePayloadValid(
                reservoir.selected, replayPdfPayload))
            reservoir.selected.proposalPdf = replayPdfPayload;
    }
    return Upt03PackReservoir(reservoir);
}

public void Upt04WriteInitialReservoir(
    uint surfaceIndex,
    inout Upt03LogicalReservoir reservoir)
{
    gUpt04Reservoirs[surfaceIndex] =
        Upt04FinalizeInitialReservoir(reservoir);
}

// D0 split-only transport encoding. The direct pass must preserve the
// unnormalized reservoir and valid-zero multiplicity for the indirect pass.
// Bit 31 of word1 carries the already-spent direct visibility ray; the lower
// 31 bits retain effectiveM. The indirect pass removes this transport bit and
// performs the only final normalization before resolve can observe the page.
public Upt03PackedReservoir Upt04PackIntermediateInitialReservoir(
    Upt03LogicalReservoir reservoir,
    uint directVisibilityRays)
{
    Upt03PackedReservoir packed = Upt03PackReservoir(reservoir);
    packed.word1 = (reservoir.effectiveM & 0x7fffffffu)
        | ((min(directVisibilityRays, 1u) & 1u) << 31u);
    return packed;
}

public float Upt04UnpackIntermediateSnorm16(uint bits)
{
    int value = int(bits & 0xffffu);
    if (value >= 32768)
        value -= 65536;
    return clamp(float(value) / 32767.0f, -1.0f, 1.0f);
}

public Upt03LogicalReservoir Upt04UnpackIntermediateInitialReservoir(
    Upt03PackedReservoir packed,
    out uint directVisibilityRays)
{
    Upt03LogicalReservoir reservoir = (Upt03LogicalReservoir)0;
    directVisibilityRays = packed.word1 >> 31u;
    reservoir.effectiveM = packed.word1 & 0x7fffffffu;
    if ((packed.word0 & 1u) == 0u)
        return reservoir;

    Upt03Candidate candidate = (Upt03Candidate)0;
    candidate.status = kUpt04SampleValidPositive;
    candidate.eventKind = (packed.word0 >> 4u) & 0x7u;
    candidate.lobeMask = (packed.word0 >> 7u) & 0x1fu;
    candidate.flags = (packed.word0 >> 12u) & 0x3fu;
    candidate.age = (packed.word0 >> 18u) & 0x3fu;
    candidate.pathLength = (packed.word0 >> 24u) & 0x0fu;
    candidate.reconnectionVertexLength = (packed.word0 >> 28u) & 0x0fu;
    candidate.identity0 = packed.word2;
    candidate.identity1 = packed.word3;
    candidate.replayKey = packed.word4;
    candidate.replayIndex = packed.word5;
    candidate.generationFingerprint = kUpt03ExpectedGeneration;
    if (candidate.eventKind == 3u)
    {
        candidate.sampleCoord0 = Upt04UnpackIntermediateSnorm16(packed.word6);
        candidate.sampleCoord1 = Upt04UnpackIntermediateSnorm16(packed.word6 >> 16u);
    }
    else
    {
        candidate.sampleCoord0 = float(packed.word6 & 0xffffu) / 65535.0f;
        candidate.sampleCoord1 = float(packed.word6 >> 16u) / 65535.0f;
    }
    candidate.contributionR = asfloat(packed.word7);
    candidate.contributionG = asfloat(packed.word8);
    candidate.contributionB = asfloat(packed.word9);
    candidate.target = asfloat(packed.word10);
    candidate.proposalPdf = asfloat(packed.word12);
    if (Upt03HasCachedX2Endpoint(candidate))
    {
        candidate.reconnectionNormalPacked = packed.word3;
        candidate.reconnectionPosition = asfloat(uint3(
            packed.word13, packed.word14, packed.word15));
        candidate.pathPdf = 1.0f;
        candidate.partialJacobian = 1.0f;
        candidate.accumulatedRouletteProbability = 1.0f;
    }
    else
    {
        candidate.pathPdf = asfloat(packed.word13);
        candidate.partialJacobian = asfloat(packed.word14);
        candidate.accumulatedRouletteProbability = asfloat(packed.word15);
    }
    reservoir.hasSelectedSample = 1u;
    reservoir.weightSum = asfloat(packed.word11);
    reservoir.selected = candidate;
    return reservoir;
}

public void Upt04WriteIntermediateInitialReservoir(
    uint surfaceIndex,
    Upt03LogicalReservoir reservoir,
    uint directVisibilityRays)
{
    gUpt04Reservoirs[surfaceIndex] = Upt04PackIntermediateInitialReservoir(
        reservoir,
        directVisibilityRays);
}

public Upt03LogicalReservoir Upt04LoadIntermediateInitialReservoir(
    uint surfaceIndex,
    out uint directVisibilityRays)
{
    return Upt04UnpackIntermediateInitialReservoir(
        gUpt04Reservoirs[surfaceIndex],
        directVisibilityRays);
}

public void Upt04FinishInitialTraceWork(Upt04InitialTraceWork work)
{
    Upt03LogicalReservoir reservoir = (Upt03LogicalReservoir)0;
    const uint2 pixel = uint2(
        work.surfaceIndex % gUpt04Control.renderWidth,
        work.surfaceIndex / gUpt04Control.renderWidth);
    Upt04StreamInitialTraceWork(reservoir, work, pixel, 0u);
    Upt04WriteInitialReservoir(work.surfaceIndex, reservoir);
}

public void Upt04FinishIntermediateDirectTraceWork(Upt04InitialTraceWork work)
{
    Upt03LogicalReservoir reservoir = (Upt03LogicalReservoir)0;
    const uint2 pixel = uint2(
        work.surfaceIndex % gUpt04Control.renderWidth,
        work.surfaceIndex / gUpt04Control.renderWidth);
    Upt04StreamInitialTraceWork(reservoir, work, pixel, 0u);
    Upt04WriteIntermediateInitialReservoir(
        work.surfaceIndex,
        reservoir,
        work.transport.budget.visibilityRays);
}
