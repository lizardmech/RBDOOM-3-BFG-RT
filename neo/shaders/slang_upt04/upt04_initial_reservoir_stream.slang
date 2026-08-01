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
    Upt03StreamCandidate(
        reservoir,
        candidate,
        Upt04WhiteHash(Upt04InitialRandomKeyAtVertex(
            pixel,
            candidateOrdinal,
            kUpt04StreamInitialReservoirSelection,
            kUpt04DimensionInitialReservoirSelection)));
}

public void Upt04WriteInitialReservoir(
    uint surfaceIndex,
    inout Upt03LogicalReservoir reservoir)
{
    if (reservoir.hasSelectedSample != 0u && reservoir.effectiveM != 0u)
    {
        reservoir.weightSum /= float(reservoir.effectiveM);
        reservoir.effectiveM = 1u;
    }
    gUpt04Reservoirs[surfaceIndex] = Upt03PackReservoir(reservoir);
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
