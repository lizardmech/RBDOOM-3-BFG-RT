import upt03_reservoir_contract;

public static const uint kUpt07RejectNoCompatibleHistory = 1u;
public static const uint kUpt07RejectHistoryNoSelection = 2u;
public static const uint kUpt07RejectUnsupportedHistoryEvent = 3u;
public static const uint kUpt07RejectShiftInvalid = 4u;
public static const uint kUpt07RejectShiftedValidZero = 5u;
public static const uint kUpt07RejectMergeMath = 6u;
public static const uint kUpt07RejectMissingObjectMotion = 7u;
public static const uint kUpt07RejectHistoryOutlier = 8u;
public static const uint kUpt07RejectIndirectIdentity = 9u;
public static const uint kUpt07RejectIndirectContinuation = 10u;
public static const uint kUpt07RejectIndirectGeometry = 11u;
public static const uint kUpt07RejectIndirectEndpoint = 12u;
public static const uint kUpt07RejectIndirectInnerRis = 13u;
public static const uint kUpt07RejectIndirectVisibility = 14u;
public static const uint kUpt07RejectIndirectStructure = 15u;
// Internal S43.4 scheduling result. It is never persisted in a reservoir:
// the classifier returns it immediately before exact continuation replay so
// the owning pixel can be appended to the bounded compact dispatch.
public static const uint kUpt07NeedsCompactReplay = 16u;

public struct Upt07DecodedReservoir
{
    public uint valid;
    public uint needsRescue;
    public Upt03LogicalReservoir logical;
};

public struct Upt07TemporalMergeResult
{
    public uint historyAccepted;
    public uint selectedHistory;
    public uint needsRescue;
    public uint rejectionReason;
    public Upt03LogicalReservoir logical;
};

// This policy bit shares the existing geometry-availability word so the
// frozen 128-byte push-constant ABI does not grow. Geometry replay only tests
// its own route bits.
public static const uint kUpt07PreviousBestSeedFlag = 0x80000000u;
public static const uint kUpt07PairwiseMisFlag = 0x08000000u;

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
    const uint nonSelectedBaseHeader = header & ~(0x0fu << 8u);
    result.needsRescue = (header & kUpt03PackedNeedsRescueBit) != 0u ? 1u : 0u;
    if ((header & 0x08u) != 0u)
        return result;
    if (lowTag == 0u)
    {
        result.valid = (nonSelectedBaseHeader == 0u
                || nonSelectedBaseHeader == kUpt03PackedNeedsRescueBit)
                && packed.word1 == 0u && Upt07PayloadWordsZero(packed)
            ? 1u : 0u;
        return result;
    }
    if (lowTag == kUpt03PackedCountOnlyTag)
    {
        const uint expectedHeader = kUpt03PackedCountOnlyTag
            | (result.needsRescue != 0u ? kUpt03PackedNeedsRescueBit : 0u);
        if (nonSelectedBaseHeader != expectedHeader || packed.word1 == 0u
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

// Reintroduce a compatible previous winner as one current-domain proposal.
// This is deliberately distinct from the later history-confidence merge.
// RTXDI's previous-best seed is streamed with invSourcePdf=1, finalized with
// M=1, and therefore has contribution weight W=1 (not W=target). When a fresh
// D0 winner also exists, combine the two finalized one-sample proposals,
// finalize over M=2, then collapse the result back to one age-zero current
// proposal. No visibility ray or additional reservoir read is introduced.
public Upt07DecodedReservoir Upt07InjectPreviousBestSeed(
    Upt07DecodedReservoir current,
    Upt03Candidate shiftedHistory,
    uint seedValid,
    float selectionRandom,
    out uint seedAdmitted)
{
    seedAdmitted = 0u;
    if (seedValid == 0u
        || shiftedHistory.status != 2u
        || !Upt03StructurallyValid(shiftedHistory)
        || !isfinite(shiftedHistory.target)
        || shiftedHistory.target <= 0.0f
        || !isfinite(selectionRandom)
        || selectionRandom < 0.0f
        || selectionRandom >= 1.0f)
        return current;

    const bool currentSelected = current.valid != 0u
        && current.logical.hasSelectedSample != 0u
        && current.logical.effectiveM != 0u
        && isfinite(current.logical.weightSum)
        && current.logical.weightSum > 0.0f
        && Upt03StructurallyValid(current.logical.selected);
    const float currentMass = currentSelected
        ? current.logical.weightSum * float(current.logical.effectiveM)
        : 0.0f;
    const float seedMass = shiftedHistory.target;
    const float totalMass = currentMass + seedMass;
    const uint sourceM = currentSelected ? 2u : 1u;
    if (!isfinite(currentMass) || currentMass < 0.0f
        || !isfinite(totalMass) || totalMass <= 0.0f)
        return current;

    const bool chooseSeed = !currentSelected
        || selectionRandom * totalMass < seedMass;
    // Both candidates are already in the current receiver's domain.  Their
    // stream weights are target-scaled UCWs, so finalization stores the mean
    // stream weight directly.  Dividing by the selected target here would
    // silently switch this one path to persisted-UCW convention.
    const float finalizedWeight = totalMass / float(sourceM);
    if (!isfinite(finalizedWeight) || finalizedWeight <= 0.0f)
        return current;

    Upt07DecodedReservoir seeded = current;
    seeded.valid = 1u;
    seeded.needsRescue = 0u;
    seeded.logical.hasSelectedSample = 1u;
    seeded.logical.effectiveM = 1u;
    seeded.logical.weightSum = finalizedWeight;
    if (chooseSeed)
        seeded.logical.selected = shiftedHistory;
    seeded.logical.selected.age = 0u;
    seedAdmitted = 1u;
    return seeded;
}

public Upt03PackedReservoir Upt07PackTemporalResult(
    Upt07TemporalMergeResult result)
{
    Upt03PackedReservoir packed = Upt03PackReservoir(result.logical);
    if (result.logical.hasSelectedSample == 0u && result.needsRescue != 0u)
        packed.word0 |= kUpt03PackedNeedsRescueBit;
    if (result.logical.hasSelectedSample == 0u)
        packed.word0 |= (result.rejectionReason & 0x0fu) << 8u;
    return packed;
}

// BASIC correction chooses between the current and shifted-history masses
// before it needs the selected sample's target in the opposite domain.  Let
// the caller use the same decision to defer an expensive current-at-history
// path replay when history will be selected.  If any later merge guard rejects
// history, the cross target is irrelevant because the current reservoir is
// returned unchanged.
public bool Upt07BasicTemporalWouldSelectHistory(
    Upt07DecodedReservoir current,
    Upt07DecodedReservoir history,
    Upt03Candidate shiftedHistory,
    uint shiftedValid,
    float selectionRandom,
    uint maximumHistoryM,
    float shiftJacobian)
{
    const bool currentSelected = current.valid != 0u
        && current.logical.hasSelectedSample != 0u;
    const bool historySelected = history.valid != 0u
        && history.logical.hasSelectedSample != 0u;
    if (shiftedValid == 0u || !historySelected
        || !isfinite(selectionRandom) || selectionRandom < 0.0f
        || selectionRandom >= 1.0f)
        return false;

    const uint historyM = min(history.logical.effectiveM, maximumHistoryM);
    if (historyM == 0u)
        return false;
    const float currentMass = currentSelected
        ? current.logical.weightSum * float(current.logical.effectiveM)
        : 0.0f;
    const bool shiftedPositive = shiftedHistory.status == 2u;
    const float historyMass = shiftedPositive
        ? history.logical.weightSum * float(historyM)
            * (shiftedHistory.target / history.logical.selected.target)
            * shiftJacobian
        : 0.0f;
    const float totalMass = currentMass + historyMass;
    return shiftedPositive && isfinite(historyMass) && historyMass > 0.0f
        && isfinite(totalMass) && totalMass > 0.0f
        && (!currentSelected || selectionRandom * totalMass < historyMass);
}

public Upt07TemporalMergeResult Upt07MergeTemporal(
    Upt07DecodedReservoir current,
    Upt07DecodedReservoir history,
    Upt03Candidate shiftedHistory,
    uint shiftedValid,
    float currentTargetAtCurrent,
    float currentTargetAtHistory,
    float historyTargetAtHistory,
    uint historyFailureReason,
    float selectionRandom,
    uint maximumHistoryM,
    uint maximumHistoryAge,
    float maximumHistoryContributionRatio,
    uint pairwiseMis,
    float shiftJacobian)
{
    Upt07TemporalMergeResult result = (Upt07TemporalMergeResult)0;
    if (current.valid != 0u)
        result.logical = current.logical;
    const bool currentSelected = current.valid != 0u
        && current.logical.hasSelectedSample != 0u;
    const bool historySelected = history.valid != 0u
        && history.logical.hasSelectedSample != 0u;
    // Age 63 is the saturated six-bit representation, not an expiry marker.
    // Rejecting age == 63 forced coherent history to reset after roughly 63
    // frames, producing a visible blue reset wave across stationary views.
    const bool historyAgeUsable = maximumHistoryAge >= 63u
        || history.logical.selected.age < maximumHistoryAge;
    const bool historyUsable = shiftedValid != 0u && historySelected
        && historyAgeUsable
        && Upt03StructurallyValid(shiftedHistory)
        && isfinite(shiftJacobian) && shiftJacobian > 0.0f
        && isfinite(selectionRandom) && selectionRandom >= 0.0f
        && selectionRandom < 1.0f;
    if (!historyUsable)
    {
        result.rejectionReason = historyFailureReason != 0u
            ? historyFailureReason : kUpt07RejectMergeMath;
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
        result.rejectionReason = kUpt07RejectMergeMath;
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
            * (pairwiseMis == 0u ? shiftJacobian : 1.0f)
        : 0.0f;
    // Match the accepted legacy DI history gate.  A mature history sample
    // whose current-receiver contribution scale is an extreme outlier must
    // not be repacked and recursively amplified.  This is deliberately a
    // history rejection, not a resolve clamp; fresh current samples remain
    // untouched.
    const float currentContributionScale = currentSelected
        ? current.logical.selected.target * current.logical.weightSum : 0.0f;
    const float historyContributionScale = shiftedPositive
        ? shiftedHistory.target * history.logical.weightSum
            * (pairwiseMis == 0u ? shiftJacobian : 1.0f)
        : 0.0f;
    if (maximumHistoryContributionRatio > 0.0f
        && history.logical.selected.age != 0u
        && isfinite(currentContributionScale)
        && currentContributionScale > 0.0f
        && isfinite(historyContributionScale)
        && historyContributionScale > currentContributionScale
            * max(maximumHistoryContributionRatio, 1.0f))
    {
        result.rejectionReason = kUpt07RejectHistoryOutlier;
        result.needsRescue = currentSelected ? 0u : 1u;
        return result;
    }
    const uint outputM = current.logical.effectiveM + historyM;

    if (pairwiseMis != 0u)
    {
        // Temporal reuse has exactly two proposal domains: the current frame
        // and the compatible history receiver.  Give each selected sample a
        // pair-local balance weight, then finalize in the current domain.
        // Unlike the legacy global denominator below, neither sample can make
        // an unrelated cross-domain target collapse a shared divisor and
        // recursively amplify the published reservoir.
        const float currentTerm = currentSelected
            && isfinite(currentTargetAtCurrent)
            && currentTargetAtCurrent > 0.0f
            ? float(current.logical.effectiveM) * currentTargetAtCurrent
            : 0.0f;
        const float currentCrossTerm = currentSelected
            && isfinite(currentTargetAtHistory)
            && currentTargetAtHistory > 0.0f
            ? float(historyM) * currentTargetAtHistory
            : 0.0f;
        const float currentDenominator = currentTerm + currentCrossTerm;
        const float currentMis = currentTerm > 0.0f
            && isfinite(currentDenominator) && currentDenominator > 0.0f
            ? currentTerm / currentDenominator : 0.0f;
        const float currentUcw = currentSelected
            ? Upt03ScaledWeightToUcw(
                current.logical.weightSum, currentTargetAtCurrent)
            : 0.0f;
        const float currentPairMass = currentMis > 0.0f
            ? currentMis * currentTargetAtCurrent * currentUcw
            : 0.0f;

        const float historyTerm = shiftedPositive
            && isfinite(historyTargetAtHistory)
            && historyTargetAtHistory > 0.0f
            ? float(historyM) * historyTargetAtHistory : 0.0f;
        const float historyCrossTerm = shiftedPositive
            && isfinite(shiftedHistory.target)
            && shiftedHistory.target > 0.0f
            ? float(current.logical.effectiveM) * shiftedHistory.target
            : 0.0f;
        const float historyDenominator = historyTerm + historyCrossTerm;
        const float historyMis = historyTerm > 0.0f
            && isfinite(historyDenominator) && historyDenominator > 0.0f
            ? historyTerm / historyDenominator : 0.0f;
        const float historyUcw = shiftedPositive
            ? Upt03ScaledWeightToUcw(
                history.logical.weightSum, historyTargetAtHistory)
            : 0.0f;
        const float historyPairMass = historyMis > 0.0f
            ? historyMis * shiftedHistory.target * historyUcw
            : 0.0f;
        const float pairMass = currentPairMass + historyPairMass;
        if (!isfinite(currentPairMass) || currentPairMass < 0.0f
            || !isfinite(historyPairMass) || historyPairMass < 0.0f
            || !isfinite(pairMass) || pairMass < 0.0f
            || outputM == 0u)
        {
            result.rejectionReason = kUpt07RejectMergeMath;
            result.needsRescue = currentSelected ? 0u : 1u;
            return result;
        }

        const bool chooseHistory = historyPairMass > 0.0f
            && (currentPairMass <= 0.0f
                || selectionRandom * pairMass < historyPairMass);
        // pairMass is already pHat_current(selected) * W_out: persist it
        // directly.  Dividing by the selected target would store UCW and make
        // resolve divide by pHat a second time.
        const float finalizedWeight = pairMass;
        result.logical.hasSelectedSample = pairMass > 0.0f ? 1u : 0u;
        result.logical.effectiveM = outputM;
        result.logical.weightSum = finalizedWeight;
        if (result.logical.hasSelectedSample != 0u
            && (!isfinite(finalizedWeight) || finalizedWeight <= 0.0f))
        {
            result.rejectionReason = kUpt07RejectMergeMath;
            result.logical.hasSelectedSample = 0u;
            result.needsRescue = 1u;
            return result;
        }
        if (chooseHistory)
        {
            result.logical.selected = shiftedHistory;
            result.logical.selected.age = min(
                history.logical.selected.age + 1u, 63u);
            result.selectedHistory = 1u;
        }
        else if (result.logical.hasSelectedSample != 0u)
        {
            result.logical.selected.target = currentTargetAtCurrent;
        }
        result.historyAccepted = 1u;
        if (result.logical.hasSelectedSample == 0u)
            result.rejectionReason = kUpt07RejectShiftedValidZero;
        result.needsRescue = result.logical.hasSelectedSample != 0u ? 0u : 1u;
        return result;
    }

    const float totalMass = currentMass + historyMass;
    if ((shiftedPositive && (!isfinite(historyMass) || historyMass <= 0.0f))
        || !isfinite(totalMass) || totalMass < 0.0f || outputM == 0u)
    {
        result.rejectionReason = kUpt07RejectMergeMath;
        result.needsRescue = currentSelected ? 0u : 1u;
        return result;
    }

    const bool outputSelected = totalMass > 0.0f;
    const bool chooseHistory = shiftedPositive
        && (!currentSelected || selectionRandom * totalMass < historyMass);
    const float selectedTargetAtCurrent = chooseHistory
        ? shiftedHistory.target
        : (currentSelected ? current.logical.selected.target : 0.0f);
    const float selectedTargetAtHistory = chooseHistory
        ? history.logical.selected.target
        : max(currentTargetAtHistory, 0.0f);
    const float targetWeightedDenominator =
        selectedTargetAtCurrent * float(current.logical.effectiveM)
        + selectedTargetAtHistory * float(historyM);
    // RTXDI BASIC correction uses the selected sample's target in the domain
    // that supplied it as the normalization numerator.  UPT persists
    // pHat_current(selected) * W rather than W, but pHat_current cancels the
    // final RTXDI denominator; it does not replace this source-domain pi.
    const float finalizedMeanWeight = outputSelected
            && isfinite(selectedTargetAtCurrent)
            && selectedTargetAtCurrent > 0.0f
            && isfinite(targetWeightedDenominator)
            && targetWeightedDenominator > 0.0f
        ? totalMass * selectedTargetAtHistory / targetWeightedDenominator
        : 0.0f;
    result.logical.hasSelectedSample = outputSelected ? 1u : 0u;
    result.logical.effectiveM = outputM;
    result.logical.weightSum = finalizedMeanWeight;
    if (outputSelected && (!isfinite(finalizedMeanWeight)
        || finalizedMeanWeight <= 0.0f))
    {
        result.rejectionReason = kUpt07RejectMergeMath;
        result.logical.hasSelectedSample = 0u;
        result.needsRescue = 1u;
        return result;
    }
    if (chooseHistory)
    {
        result.logical.selected = shiftedHistory;
        result.logical.selected.age = min(
            history.logical.selected.age + 1u, 63u);
        result.selectedHistory = 1u;
    }
    result.historyAccepted = 1u;
    if (!outputSelected)
        result.rejectionReason = kUpt07RejectShiftedValidZero;
    result.needsRescue = outputSelected ? 0u : 1u;
    return result;
}
