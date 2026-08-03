import upt03_reservoir_contract;

public struct Upt07DecodedReservoir
{
    public uint valid;
    public uint needsRescue;
    public Upt03LogicalReservoir logical;
};

public struct Upt07TemporalMergeResult
{
    public uint historyAccepted;
    public uint needsRescue;
    public Upt03LogicalReservoir logical;
};

public bool Upt07PayloadWordsZero(Upt03PackedReservoir packed)
{
    return packed.word2 == 0u && packed.word3 == 0u
        && packed.word4 == 0u && packed.word5 == 0u
        && packed.word6 == 0u && packed.word7 == 0u
        && packed.word8 == 0u && packed.word9 == 0u
        && packed.word10 == 0u && packed.word11 == 0u
        && packed.word12 == 0u && packed.word13 == 0u
        && packed.word14 == 0u && packed.word15 == 0u;
}

public float Upt07UnpackSnorm16(uint bits)
{
    int value = int(bits & 0xffffu);
    if (value >= 32768)
        value -= 65536;
    return clamp(float(value) / 32767.0f, -1.0f, 1.0f);
}

public Upt07DecodedReservoir Upt07DecodeReservoir(
    Upt03PackedReservoir packed)
{
    Upt07DecodedReservoir result = (Upt07DecodedReservoir)0;
    const uint header = packed.word0;
    const uint lowTag = header & 0x03u;
    result.needsRescue = (header & kUpt03PackedNeedsRescueBit) != 0u ? 1u : 0u;
    if ((header & 0x08u) != 0u)
        return result;
    if (lowTag == 0u)
    {
        result.valid = (header == 0u || header == kUpt03PackedNeedsRescueBit)
                && packed.word1 == 0u && Upt07PayloadWordsZero(packed)
            ? 1u : 0u;
        return result;
    }
    if (lowTag == kUpt03PackedCountOnlyTag)
    {
        const uint expectedHeader = kUpt03PackedCountOnlyTag
            | (result.needsRescue != 0u ? kUpt03PackedNeedsRescueBit : 0u);
        if (header != expectedHeader || packed.word1 == 0u
            || !Upt07PayloadWordsZero(packed))
            return result;
        result.logical.effectiveM = packed.word1;
        result.valid = 1u;
        return result;
    }
    if ((header & 0x0fu) != kUpt03PackedSelectedTag)
        return result;

    Upt03Candidate candidate = (Upt03Candidate)0;
    candidate.status = 2u;
    candidate.eventKind = (header >> 4u) & 0x7u;
    candidate.lobeMask = (header >> 7u) & 0x1fu;
    candidate.flags = (header >> 12u) & 0x3fu;
    candidate.age = (header >> 18u) & 0x3fu;
    candidate.pathLength = (header >> 24u) & 0x0fu;
    candidate.reconnectionVertexLength = (header >> 28u) & 0x0fu;
    candidate.identity0 = packed.word2;
    candidate.identity1 = packed.word3;
    candidate.replayKey = packed.word4;
    candidate.replayIndex = packed.word5;
    candidate.generationFingerprint = kUpt03ExpectedGeneration;
    if (candidate.eventKind == 3u)
    {
        candidate.sampleCoord0 = Upt07UnpackSnorm16(packed.word6);
        candidate.sampleCoord1 = Upt07UnpackSnorm16(packed.word6 >> 16u);
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
    candidate.pathPdf = asfloat(packed.word13);
    candidate.partialJacobian = asfloat(packed.word14);
    candidate.accumulatedRouletteProbability = asfloat(packed.word15);
    result.logical.hasSelectedSample = 1u;
    result.logical.effectiveM = packed.word1;
    result.logical.weightSum = asfloat(packed.word11);
    result.logical.selected = candidate;
    result.valid = result.logical.effectiveM != 0u
            && isfinite(result.logical.weightSum)
            && result.logical.weightSum > 0.0f
            && Upt03StructurallyValid(candidate)
        ? 1u : 0u;
    return result;
}

public Upt03PackedReservoir Upt07PackTemporalResult(
    Upt07TemporalMergeResult result)
{
    Upt03PackedReservoir packed = Upt03PackReservoir(result.logical);
    if (result.logical.hasSelectedSample == 0u && result.needsRescue != 0u)
        packed.word0 |= kUpt03PackedNeedsRescueBit;
    return packed;
}

public Upt07TemporalMergeResult Upt07MergeTemporal(
    Upt07DecodedReservoir current,
    Upt07DecodedReservoir history,
    Upt03Candidate shiftedHistory,
    uint shiftedValid,
    float selectionRandom,
    uint maximumHistoryM,
    uint maximumHistoryAge)
{
    Upt07TemporalMergeResult result = (Upt07TemporalMergeResult)0;
    if (current.valid != 0u)
        result.logical = current.logical;
    const bool currentSelected = current.valid != 0u
        && current.logical.hasSelectedSample != 0u;
    const bool historySelected = history.valid != 0u
        && history.logical.hasSelectedSample != 0u;
    const bool historyUsable = shiftedValid != 0u && historySelected
        && history.logical.selected.age < maximumHistoryAge
        && Upt03StructurallyValid(shiftedHistory)
        && isfinite(selectionRandom) && selectionRandom >= 0.0f
        && selectionRandom < 1.0f;
    if (!historyUsable)
    {
        result.needsRescue = currentSelected ? 0u : 1u;
        if (!currentSelected)
        {
            result.logical.hasSelectedSample = 0u;
            result.logical.weightSum = 0.0f;
        }
        return result;
    }

    const uint historyM = min(history.logical.effectiveM, maximumHistoryM);
    if (historyM == 0u || current.logical.effectiveM > 0xffffffffu - historyM)
    {
        result.needsRescue = currentSelected ? 0u : 1u;
        return result;
    }
    const float currentMass = currentSelected
        ? current.logical.weightSum * float(current.logical.effectiveM)
        : 0.0f;
    const bool shiftedPositive = shiftedHistory.status == 2u;
    const float historyMass = shiftedPositive
        ? history.logical.weightSum * float(historyM)
            * (shiftedHistory.target / history.logical.selected.target)
        : 0.0f;
    const uint outputM = current.logical.effectiveM + historyM;
    const float totalMass = currentMass + historyMass;
    if ((shiftedPositive && (!isfinite(historyMass) || historyMass <= 0.0f))
        || !isfinite(totalMass) || totalMass < 0.0f || outputM == 0u)
    {
        result.needsRescue = currentSelected ? 0u : 1u;
        return result;
    }

    const bool outputSelected = totalMass > 0.0f;
    const bool chooseHistory = shiftedPositive
        && (!currentSelected || selectionRandom * totalMass < historyMass);
    result.logical.hasSelectedSample = outputSelected ? 1u : 0u;
    result.logical.effectiveM = outputM;
    result.logical.weightSum = outputSelected ? totalMass / float(outputM) : 0.0f;
    if (chooseHistory)
    {
        result.logical.selected = shiftedHistory;
        result.logical.selected.age = min(
            history.logical.selected.age + 1u, 63u);
    }
    result.historyAccepted = 1u;
    result.needsRescue = outputSelected ? 0u : 1u;
    return result;
}
