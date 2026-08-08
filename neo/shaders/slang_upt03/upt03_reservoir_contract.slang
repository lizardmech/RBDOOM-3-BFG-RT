// rbdoom-owned UPT-03 GPU form of the UPT-02 CPU oracle.  This module has no
// material, random-provider, renderer, ray-tracing, or NVRHI dependency.

public static const uint kUpt03ExpectedGeneration = 0x10203040u;
// replayKey is also the persistent random-sample identity used by UPT-08's
// duplication map.  The high bit is reserved as a one-frame spatial-rescue
// flag; the remaining 31 bits remain stable while a sample moves through
// temporal/spatial reuse.  Do not replace this with an exact magic value: that
// destroys the identity needed to detect correlated copies.
public static const uint kUpt03ReplayKeyRescueBit = 0x80000000u;
public static const uint kUpt03PackedSelectedTag = 1u;
public static const uint kUpt03PackedCountOnlyTag = 2u;
public static const uint kUpt03PackedNeedsRescueBit = 4u;

public struct Upt03Candidate
{
    public uint status;
    public uint eventKind;
    public uint lobeMask;
    public uint flags;
    public uint age;
    public uint pathLength;
    public uint reconnectionVertexLength;
    public uint identity0;
    public uint identity1;
    public uint replayKey;
    public uint replayIndex;
    public uint generationFingerprint;
    public float sampleCoord0;
    public float sampleCoord1;
    public float contributionR;
    public float contributionG;
    public float contributionB;
    public float target;
    public float proposalPdf;
    public float pathPdf;
    public float partialJacobian;
    public float accumulatedRouletteProbability;
    public float selectionRandom;
    public uint padding;
};

public bool Upt03IsDirectSpatialRescue(Upt03Candidate candidate)
{
    return (candidate.eventKind == 1u || candidate.eventKind == 2u)
        && (candidate.replayKey & kUpt03ReplayKeyRescueBit) != 0u;
}

public uint Upt03DuplicationSampleId(Upt03Candidate candidate)
{
    return Upt03IsDirectSpatialRescue(candidate)
        ? 0u : (candidate.replayKey & ~kUpt03ReplayKeyRescueBit);
}

public struct Upt03PackedReservoir
{
    public uint word0;
    public uint word1;
    public uint word2;
    public uint word3;
    public uint word4;
    public uint word5;
    public uint word6;
    public uint word7;
    public uint word8;
    public uint word9;
    public uint word10;
    public uint word11;
    public uint word12;
    public uint word13;
    public uint word14;
    public uint word15;
};

public struct Upt03LogicalReservoir
{
    public uint hasSelectedSample;
    public uint effectiveM;
    // UPT persists the target-scaled UCW, pHat(selected) * W, which is also
    // the mean RIS weight after finalization.  This is deliberately not the
    // RTXDI convention of persisting W itself.  Same-domain formulas often
    // hide the distinction because pHat cancels; every cross-domain merge
    // must convert through the selected sample's freshly evaluated source
    // target before applying an RTXDI/GRIS UCW formula.
    public float weightSum;
    public Upt03Candidate selected;
};

public float Upt03ScaledWeightToUcw(float scaledWeight, float sourceTarget)
{
    return isfinite(scaledWeight) && scaledWeight > 0.0f
            && isfinite(sourceTarget) && sourceTarget > 0.0f
        ? scaledWeight / sourceTarget : 0.0f;
}

public float Upt03UcwToScaledWeight(float ucw, float receiverTarget)
{
    return isfinite(ucw) && ucw > 0.0f
            && isfinite(receiverTarget) && receiverTarget > 0.0f
        ? ucw * receiverTarget : 0.0f;
}

public bool Upt03FinitePositive(float value)
{
    return isfinite(value) && value > 0.0f;
}

public bool Upt03FiniteNonNegative(float value)
{
    return isfinite(value) && value >= 0.0f;
}

public bool Upt03SampleCoordinatesValid(Upt03Candidate candidate)
{
    if (!isfinite(candidate.sampleCoord0) || !isfinite(candidate.sampleCoord1))
        return false;
    if (candidate.eventKind == 3u)
        return candidate.sampleCoord0 >= -1.0f && candidate.sampleCoord0 <= 1.0f
            && candidate.sampleCoord1 >= -1.0f && candidate.sampleCoord1 <= 1.0f;
    const bool unitSquare = candidate.sampleCoord0 >= 0.0f && candidate.sampleCoord0 <= 1.0f
        && candidate.sampleCoord1 >= 0.0f && candidate.sampleCoord1 <= 1.0f;
    if (!unitSquare)
        return false;
    if ((candidate.eventKind == 2u || candidate.eventKind == 4u)
        && candidate.sampleCoord0 + candidate.sampleCoord1 > 1.0f)
        return false;
    return true;
}

public bool Upt03StructurallyValid(Upt03Candidate candidate)
{
    if (candidate.status == 0u || candidate.eventKind < 1u || candidate.eventKind > 4u
        || candidate.generationFingerprint != kUpt03ExpectedGeneration
        || (candidate.lobeMask & 0x1fu) == 0u || (candidate.lobeMask & ~0x1fu) != 0u
        || (candidate.flags & ~0x3fu) != 0u
        || ((candidate.flags & 8u) != 0u && (candidate.flags & 4u) == 0u)
        || candidate.identity0 == 0u || (candidate.eventKind == 4u && candidate.replayKey == 0u)
        || !Upt03SampleCoordinatesValid(candidate)
        || candidate.age > 63u || candidate.pathLength > 15u
        || candidate.reconnectionVertexLength > candidate.pathLength
        || !Upt03FinitePositive(candidate.proposalPdf)
        || !Upt03FinitePositive(candidate.pathPdf)
        || !Upt03FinitePositive(candidate.partialJacobian)
        || !Upt03FinitePositive(candidate.accumulatedRouletteProbability)
        || candidate.accumulatedRouletteProbability > 1.0f
        || !Upt03FiniteNonNegative(candidate.contributionR)
        || !Upt03FiniteNonNegative(candidate.contributionG)
        || !Upt03FiniteNonNegative(candidate.contributionB))
        return false;

    if (candidate.status == 1u)
        return candidate.target == 0.0f && candidate.contributionR == 0.0f
            && candidate.contributionG == 0.0f && candidate.contributionB == 0.0f;
    return candidate.status == 2u && Upt03FinitePositive(candidate.target)
        && (candidate.contributionR > 0.0f || candidate.contributionG > 0.0f
            || candidate.contributionB > 0.0f);
}

public void Upt03StreamCandidate(
    inout Upt03LogicalReservoir reservoir,
    Upt03Candidate candidate,
    float selectionRandom)
{
    if (!Upt03StructurallyValid(candidate) || reservoir.effectiveM == 0xffffffffu)
        return;
    if (candidate.status == 1u)
    {
        reservoir.effectiveM += 1u;
        return;
    }
    if (!isfinite(selectionRandom) || selectionRandom < 0.0f || selectionRandom >= 1.0f)
        return;

    const float candidateWeight = candidate.target / candidate.proposalPdf;
    const float newWeightSum = reservoir.weightSum + candidateWeight;
    if (!Upt03FinitePositive(candidateWeight) || !Upt03FinitePositive(newWeightSum))
        return;

    const bool select = reservoir.hasSelectedSample == 0u
        || selectionRandom * newWeightSum < candidateWeight;
    reservoir.effectiveM += 1u;
    reservoir.weightSum = newWeightSum;
    if (select)
    {
        reservoir.hasSelectedSample = 1u;
        reservoir.selected = candidate;
    }
}

public uint Upt03PackCoordinatePair(Upt03Candidate candidate)
{
    if (candidate.eventKind == 3u)
    {
        const int low = int(round(candidate.sampleCoord0 * 32767.0f));
        const int high = int(round(candidate.sampleCoord1 * 32767.0f));
        return (uint(low) & 0xffffu) | ((uint(high) & 0xffffu) << 16u);
    }
    const uint low = uint(round(candidate.sampleCoord0 * 65535.0f));
    const uint high = uint(round(candidate.sampleCoord1 * 65535.0f));
    return low | (high << 16u);
}

public uint Upt03PackHeader(Upt03Candidate candidate)
{
    return 1u
        | (candidate.eventKind << 4u)
        | ((candidate.lobeMask & 0x1fu) << 7u)
        | (((candidate.flags | 1u) & 0x3fu) << 12u)
        | ((candidate.age & 0x3fu) << 18u)
        | ((candidate.pathLength & 0x0fu) << 24u)
        | ((candidate.reconnectionVertexLength & 0x0fu) << 28u);
}

public Upt03PackedReservoir Upt03PackReservoir(Upt03LogicalReservoir reservoir)
{
    Upt03PackedReservoir packed = (Upt03PackedReservoir)0;
    if (reservoir.hasSelectedSample == 0u)
    {
        if (reservoir.effectiveM != 0u && reservoir.weightSum == 0.0f)
        {
            packed.word0 = kUpt03PackedCountOnlyTag;
            packed.word1 = reservoir.effectiveM;
        }
        return packed;
    }

    const Upt03Candidate candidate = reservoir.selected;
    packed.word0 = Upt03PackHeader(candidate);
    packed.word1 = reservoir.effectiveM;
    packed.word2 = candidate.identity0;
    packed.word3 = candidate.identity1;
    packed.word4 = candidate.replayKey;
    packed.word5 = candidate.replayIndex;
    packed.word6 = Upt03PackCoordinatePair(candidate);
    packed.word7 = asuint(candidate.contributionR);
    packed.word8 = asuint(candidate.contributionG);
    packed.word9 = asuint(candidate.contributionB);
    packed.word10 = asuint(candidate.target);
    packed.word11 = asuint(reservoir.weightSum);
    packed.word12 = asuint(candidate.proposalPdf);
    packed.word13 = asuint(candidate.pathPdf);
    packed.word14 = asuint(candidate.partialJacobian);
    packed.word15 = asuint(candidate.accumulatedRouletteProbability);
    return packed;
}
