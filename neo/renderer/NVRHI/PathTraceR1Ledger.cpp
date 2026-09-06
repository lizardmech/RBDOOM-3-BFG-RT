#include "PathTraceR1Ledger.h"

#include <cstdio>

namespace
{

void AddRelevantOrdinal(
    RtSmokeR1Audit& audit,
    int ordinal,
    uint32_t reasonMask,
    const RtSmokeR1CacheValidationObservation* observation,
    RtSmokeCacheHandle conditionedAmbient = 0,
    RtSmokeCacheHandle conditionedIndex = 0)
{
    ++audit.relevantOrdinalTotal;
    if (audit.relevantOrdinalCount >= RT_SMOKE_R1_ORDINAL_CAP)
    {
        ++audit.relevantOrdinalTruncated;
        return;
    }

    RtSmokeR1RelevantOrdinal& relevant =
        audit.relevantOrdinals[audit.relevantOrdinalCount++];
    relevant = {};
    relevant.ordinal = ordinal;
    relevant.reasonMask = reasonMask;
    if (!observation)
    {
        relevant.effectiveAmbient = conditionedAmbient;
        relevant.effectiveIndex = conditionedIndex;
        return;
    }

    const RtSmokeLiveCacheGateSample& live = observation->liveSample;
    const RtSmokeDiagnosticCacheGateSample& diagnostic =
        observation->diagnosticSample;
    relevant.resultMask =
        (live.ambient.result != RtSmokeCacheEvaluationResult::NotEvaluated ? 1u : 0u) |
        (live.ambient.result == RtSmokeCacheEvaluationResult::Current ? 2u : 0u) |
        (live.index.result != RtSmokeCacheEvaluationResult::NotEvaluated ? 4u : 0u) |
        (live.index.result == RtSmokeCacheEvaluationResult::Current ? 8u : 0u) |
        (diagnostic.ambientResult != RtSmokeCacheEvaluationResult::NotEvaluated ? 16u : 0u) |
        (diagnostic.ambientResult == RtSmokeCacheEvaluationResult::Current ? 32u : 0u) |
        (diagnostic.indexResult != RtSmokeCacheEvaluationResult::NotEvaluated ? 64u : 0u) |
        (diagnostic.indexResult == RtSmokeCacheEvaluationResult::Current ? 128u : 0u);
    relevant.triAmbientPresence = live.ambient.presenceHandle;
    relevant.triIndexPresence = live.index.presenceHandle;
    relevant.triAmbientEvaluated = live.ambient.evaluatedHandle;
    relevant.triIndexEvaluated = live.index.evaluatedHandle;
    relevant.drawAmbient = diagnostic.drawSurfAmbientHandle;
    relevant.drawIndex = diagnostic.drawSurfIndexHandle;
    relevant.effectiveAmbient = diagnostic.selectedAmbientHandle;
    relevant.effectiveIndex = diagnostic.selectedIndexHandle;
}

void AppendOrdinalText(
    char* text,
    size_t capacity,
    size_t& used,
    const RtSmokeR1RelevantOrdinal& relevant)
{
    if (used >= capacity)
    {
        return;
    }
    const int written = std::snprintf(
        text + used,
        capacity - used,
        "%s%d:%x:%x",
        used == 1 ? "" : ",",
        relevant.ordinal,
        relevant.reasonMask,
        relevant.resultMask);
    if (written < 0 || static_cast<size_t>(written) >= capacity - used)
    {
        used = capacity;
        return;
    }
    used += static_cast<size_t>(written);
}

} // namespace

RtSmokeR1Terminal RtSmokeR1TerminalForValidation(
    RtSmokeR1ValidationDisposition disposition)
{
    switch (disposition)
    {
        case RtSmokeR1ValidationDisposition::NullSurface: return RtSmokeR1Terminal::NullSurface;
        case RtSmokeR1ValidationDisposition::MissingFrontEndGeo: return RtSmokeR1Terminal::MissingFrontEndGeo;
        case RtSmokeR1ValidationDisposition::NullMaterial: return RtSmokeR1Terminal::NullMaterial;
        case RtSmokeR1ValidationDisposition::ConditionedOff: return RtSmokeR1Terminal::ConditionedOff;
        case RtSmokeR1ValidationDisposition::GuiSurface: return RtSmokeR1Terminal::GuiSurface;
        case RtSmokeR1ValidationDisposition::NullSpace: return RtSmokeR1Terminal::NullSpace;
        case RtSmokeR1ValidationDisposition::NullModel: return RtSmokeR1Terminal::NullModel;
        case RtSmokeR1ValidationDisposition::CallbackEntity: return RtSmokeR1Terminal::CallbackEntity;
        case RtSmokeR1ValidationDisposition::MissingGeometryPayload: return RtSmokeR1Terminal::MissingGeometryPayload;
        case RtSmokeR1ValidationDisposition::InvalidIndexCount: return RtSmokeR1Terminal::InvalidIndexCount;
        case RtSmokeR1ValidationDisposition::NonCurrentCache: return RtSmokeR1Terminal::NonCurrentCache;
        default: return RtSmokeR1Terminal::Pending;
    }
}

uint32_t RtSmokeR1ConditionedOffMask(
    RtSmokeCacheHandle ambientHandle,
    RtSmokeCacheHandle indexHandle,
    int currentFrame)
{
    return (ClassifySmokeCacheHandle(ambientHandle, currentFrame) ==
                RtSmokeCacheHandleState::Previous ? 1u : 0u) |
        (ClassifySmokeCacheHandle(indexHandle, currentFrame) ==
                RtSmokeCacheHandleState::Previous ? 2u : 0u);
}

uint32_t RtSmokeR1RaceWitnessMask(
    bool cacheEligible,
    const RtSmokeLiveCacheVerdict& liveVerdict,
    const RtSmokeDiagnosticCacheVerdict& diagnosticVerdict)
{
    if (!cacheEligible)
    {
        return 0;
    }
    const bool liveReject =
        liveVerdict.Decision() == RtSmokeLiveCacheDecision::Reject;
    const bool diagnosticReject =
        diagnosticVerdict.Decision() == RtSmokeDiagnosticCacheDecision::Reject;
    if (liveReject == diagnosticReject)
    {
        return 0;
    }
    return liveReject ? 1u : 2u;
}

void RtSmokeR1Finalize(
    RtSmokeR1Audit& audit,
    int ordinal,
    RtSmokeR1Terminal terminal)
{
    const int terminalIndex = static_cast<int>(terminal);
    if (terminal <= RtSmokeR1Terminal::Pending ||
        terminal >= RtSmokeR1Terminal::Count)
    {
        ++audit.unknownTerminal;
        audit.available = 0;
        return;
    }
    ++audit.terminalTotals[terminalIndex];
    ++audit.terminalSum;
    if (ordinal < 0 || ordinal >= audit.surfaceCount ||
        ordinal >= RT_SMOKE_R1_LEDGER_CAP)
    {
        ++audit.overflowed;
        audit.available = 0;
        return;
    }
    uint8_t& slot = audit.terminalLedger[ordinal];
    if (slot != static_cast<uint8_t>(RtSmokeR1Terminal::Pending))
    {
        ++audit.doubleFinalized;
        audit.available = 0;
        return;
    }
    slot = static_cast<uint8_t>(terminal);
}

void RtSmokeR1ObserveConditionedOff(
    RtSmokeR1Audit& audit,
    int ordinal,
    RtSmokeCacheHandle selectedAmbient,
    RtSmokeCacheHandle selectedIndex)
{
    const uint32_t mask = RtSmokeR1ConditionedOffMask(
        selectedAmbient,
        selectedIndex,
        audit.vertexCacheFrame);
    if (mask == 3u)
    {
        ++audit.conditionedOffD;
    }
    else if (mask == 1u)
    {
        ++audit.conditionedOffVOnly;
    }
    else if (mask == 2u)
    {
        ++audit.conditionedOffIOnly;
    }
    if (mask != 0)
    {
        AddRelevantOrdinal(
            audit,
            ordinal,
            8u | mask,
            nullptr,
            selectedAmbient,
            selectedIndex);
    }
}

void RtSmokeR1ObserveCacheSample(
    RtSmokeR1Audit& audit,
    int ordinal,
    const RtSmokeR1CacheValidationObservation& observation)
{
    if (!observation.cacheEligible)
    {
        return;
    }

    ++audit.cacheEligible;
    const bool liveReject = observation.liveVerdict.Decision() ==
        RtSmokeLiveCacheDecision::Reject;
    const bool diagnosticReject = observation.diagnosticVerdict.Decision() ==
        RtSmokeDiagnosticCacheDecision::Reject;
    audit.triCacheReject += liveReject ? 1 : 0;
    audit.effectiveCacheReject += diagnosticReject ? 1 : 0;
    audit.effectiveNonCurrent += diagnosticReject ? 1 : 0;

    const RtSmokeLiveCacheGateSample& live = observation.liveSample;
    const RtSmokeDiagnosticCacheGateSample& diagnostic =
        observation.diagnosticSample;
    const bool triAmbientWasEvaluated =
        live.ambient.result != RtSmokeCacheEvaluationResult::NotEvaluated;
    const bool triIndexWasEvaluated =
        live.index.result != RtSmokeCacheEvaluationResult::NotEvaluated;
    const bool effectiveAmbientWasEvaluated =
        diagnostic.ambientResult != RtSmokeCacheEvaluationResult::NotEvaluated;
    const bool effectiveIndexWasEvaluated =
        diagnostic.indexResult != RtSmokeCacheEvaluationResult::NotEvaluated;
    audit.triAmbientEvaluated += triAmbientWasEvaluated ? 1 : 0;
    audit.triAmbientCurrent +=
        live.ambient.result == RtSmokeCacheEvaluationResult::Current ? 1 : 0;
    audit.triIndexEvaluated += triIndexWasEvaluated ? 1 : 0;
    audit.triIndexCurrent +=
        live.index.result == RtSmokeCacheEvaluationResult::Current ? 1 : 0;
    audit.effectiveAmbientEvaluated += effectiveAmbientWasEvaluated ? 1 : 0;
    audit.effectiveAmbientCurrent +=
        diagnostic.ambientResult == RtSmokeCacheEvaluationResult::Current ? 1 : 0;
    audit.effectiveIndexEvaluated += effectiveIndexWasEvaluated ? 1 : 0;
    audit.effectiveIndexCurrent +=
        diagnostic.indexResult == RtSmokeCacheEvaluationResult::Current ? 1 : 0;

    const RtSmokeCacheHandleState states[8] = {
        ClassifySmokeCacheHandle(live.ambient.presenceHandle, audit.vertexCacheFrame),
        ClassifySmokeCacheHandle(live.index.presenceHandle, audit.vertexCacheFrame),
        triAmbientWasEvaluated
            ? ClassifySmokeCacheHandle(live.ambient.evaluatedHandle, live.ambient.evaluatedFrame)
            : RtSmokeCacheHandleState::NotEvaluated,
        triIndexWasEvaluated
            ? ClassifySmokeCacheHandle(live.index.evaluatedHandle, live.index.evaluatedFrame)
            : RtSmokeCacheHandleState::NotEvaluated,
        ClassifySmokeCacheHandle(diagnostic.drawSurfAmbientHandle, audit.vertexCacheFrame),
        ClassifySmokeCacheHandle(diagnostic.drawSurfIndexHandle, audit.vertexCacheFrame),
        ClassifySmokeCacheHandle(diagnostic.selectedAmbientHandle, audit.vertexCacheFrame),
        ClassifySmokeCacheHandle(diagnostic.selectedIndexHandle, audit.vertexCacheFrame)
    };
    for (int population = 0; population < 8; ++population)
    {
        ++audit.handleStates[population][static_cast<int>(states[population])];
    }

    uint32_t reasonMask = diagnosticReject ? 4u : 0u;
    const uint32_t witnessMask = RtSmokeR1RaceWitnessMask(
        true,
        observation.liveVerdict,
        observation.diagnosticVerdict);
    if (witnessMask == 1u)
    {
        ++audit.triRejectEffectiveAccept;
        reasonMask |= 1u;
    }
    else if (witnessMask == 2u)
    {
        ++audit.triAcceptEffectiveReject;
        reasonMask |= 2u;
    }
    if (reasonMask != 0)
    {
        AddRelevantOrdinal(audit, ordinal, reasonMask, &observation);
    }
}

void RtSmokeR1Reconcile(RtSmokeR1Audit& audit)
{
    if (audit.surfaceCount < 0)
    {
        ++audit.reconciliationFailures;
    }
    const int ledgerCount = audit.surfaceCount <= 0 ? 0 :
        (audit.surfaceCount < RT_SMOKE_R1_LEDGER_CAP
            ? audit.surfaceCount : RT_SMOKE_R1_LEDGER_CAP);
    for (int ordinal = 0; ordinal < ledgerCount; ++ordinal)
    {
        if (audit.terminalLedger[ordinal] ==
            static_cast<uint8_t>(RtSmokeR1Terminal::Pending))
        {
            ++audit.unfinalized;
        }
    }
    if (audit.surfaceCount > RT_SMOKE_R1_LEDGER_CAP)
    {
        audit.available = 0;
    }

    uint64_t terminalTotalSum = 0;
    for (int terminal = 1;
         terminal < static_cast<int>(RtSmokeR1Terminal::Count);
         ++terminal)
    {
        terminalTotalSum += audit.terminalTotals[terminal];
    }
    const int expectedTerminalCount = audit.surfaceCount > 0
        ? audit.surfaceCount : 0;
    if (audit.terminalSum != static_cast<uint64_t>(expectedTerminalCount) ||
        audit.terminalSum != terminalTotalSum)
    {
        ++audit.reconciliationFailures;
    }

    if (audit.triRejectEffectiveAccept + audit.triAcceptEffectiveReject >
            audit.cacheEligible ||
        audit.triCacheReject > audit.cacheEligible ||
        audit.effectiveCacheReject > audit.cacheEligible ||
        audit.effectiveNonCurrent != audit.effectiveCacheReject ||
        audit.triRejectEffectiveAccept > audit.triCacheReject ||
        audit.triAcceptEffectiveReject > audit.effectiveCacheReject ||
        audit.terminalTotals[static_cast<int>(RtSmokeR1Terminal::NonCurrentCache)] !=
            audit.triCacheReject)
    {
        ++audit.reconciliationFailures;
    }
    if (audit.effectiveCacheReject <= audit.cacheEligible &&
        audit.triRejectEffectiveAccept >
            audit.cacheEligible - audit.effectiveCacheReject)
    {
        ++audit.reconciliationFailures;
    }
    if (audit.triCacheReject <= audit.cacheEligible &&
        audit.triAcceptEffectiveReject >
            audit.cacheEligible - audit.triCacheReject)
    {
        ++audit.reconciliationFailures;
    }

    for (int population = 0; population < 8; ++population)
    {
        uint64_t populationSum = 0;
        for (int state = 0;
             state < static_cast<int>(RtSmokeCacheHandleState::Count);
             ++state)
        {
            populationSum += audit.handleStates[population][state];
        }
        if (populationSum != audit.cacheEligible)
        {
            ++audit.reconciliationFailures;
        }
    }

    const uint64_t triAmbientAbsent =
        audit.handleStates[0][static_cast<int>(RtSmokeCacheHandleState::Absent)];
    const uint64_t triIndexAbsent =
        audit.handleStates[1][static_cast<int>(RtSmokeCacheHandleState::Absent)];
    const uint64_t effectiveAmbientAbsent =
        audit.handleStates[6][static_cast<int>(RtSmokeCacheHandleState::Absent)];
    const uint64_t effectiveIndexAbsent =
        audit.handleStates[7][static_cast<int>(RtSmokeCacheHandleState::Absent)];
    const uint64_t triAmbientPresent = triAmbientAbsent <= audit.cacheEligible
        ? audit.cacheEligible - triAmbientAbsent : 0;
    const uint64_t triIndexPresent = triIndexAbsent <= audit.cacheEligible
        ? audit.cacheEligible - triIndexAbsent : 0;
    const uint64_t effectiveAmbientPresent =
        effectiveAmbientAbsent <= audit.cacheEligible
            ? audit.cacheEligible - effectiveAmbientAbsent : 0;
    const uint64_t effectiveIndexPresent =
        effectiveIndexAbsent <= audit.cacheEligible
            ? audit.cacheEligible - effectiveIndexAbsent : 0;
    if (triAmbientAbsent > audit.cacheEligible ||
        triIndexAbsent > audit.cacheEligible ||
        effectiveAmbientAbsent > audit.cacheEligible ||
        effectiveIndexAbsent > audit.cacheEligible ||
        audit.triAmbientEvaluated != triAmbientPresent ||
        audit.triAmbientCurrent > audit.triAmbientEvaluated ||
        audit.triIndexEvaluated > triIndexPresent ||
        audit.triIndexCurrent > audit.triIndexEvaluated ||
        audit.effectiveAmbientEvaluated != effectiveAmbientPresent ||
        audit.effectiveAmbientCurrent > audit.effectiveAmbientEvaluated ||
        audit.effectiveIndexEvaluated > effectiveIndexPresent ||
        audit.effectiveIndexCurrent > audit.effectiveIndexEvaluated)
    {
        ++audit.reconciliationFailures;
    }

    const uint64_t conditionedDivergent = audit.conditionedOffD +
        audit.conditionedOffVOnly + audit.conditionedOffIOnly;
    if (conditionedDivergent >
            audit.terminalTotals[static_cast<int>(RtSmokeR1Terminal::ConditionedOff)] ||
        audit.relevantOrdinalTotal != audit.effectiveCacheReject +
            audit.triRejectEffectiveAccept + conditionedDivergent ||
        audit.relevantOrdinalTotal !=
            static_cast<uint64_t>(audit.relevantOrdinalCount) +
                audit.relevantOrdinalTruncated)
    {
        ++audit.reconciliationFailures;
    }
    if (audit.doubleFinalized != 0 || audit.unfinalized != 0 ||
        audit.overflowed != 0 || audit.unknownTerminal != 0 ||
        audit.reconciliationFailures != 0)
    {
        audit.available = 0;
    }
}

bool RtSmokeR1FormatLine(
    RtSmokeR1Audit& audit,
    uint64_t auditPreLogUs,
    char* output,
    size_t outputCapacity)
{
    if (!output || outputCapacity == 0)
    {
        return false;
    }
    RtSmokeR1Reconcile(audit);

    char ordinalText[RT_SMOKE_R1_ORDINAL_TEXT_CAP];
    ordinalText[0] = '[';
    size_t used = 1;
    for (int i = 0; i < audit.relevantOrdinalCount; ++i)
    {
        AppendOrdinalText(
            ordinalText,
            sizeof(ordinalText) - 2,
            used,
            audit.relevantOrdinals[i]);
    }
    if (used > sizeof(ordinalText) - 2)
    {
        used = sizeof(ordinalText) - 2;
        audit.available = 0;
        ++audit.reconciliationFailures;
    }
    ordinalText[used++] = ']';
    ordinalText[used] = '\0';

    const uint64_t* t = audit.terminalTotals;
    RtSmokeR1RelevantOrdinal first = {};
    first.ordinal = -1;
    if (audit.relevantOrdinalCount > 0)
    {
        first = audit.relevantOrdinals[0];
    }
    const int written = std::snprintf(
        output,
        outputCapacity,
        "PathTracePrimaryPass: R1 fixNObserve frame=%d cacheFrame=%d drawSurfs=%d available=%d terminalSum=%llu terminal(apply/null/frontEnd/material/condition/gui/space/model/callback/payload/index/cache/composite/alpha/staticWorld/staticMatch/rigidRemoved/skinnedOmitted/admissionPre/appendEmpty/rollback/accepted)=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu cacheEligible=%llu triCacheReject=%llu effectiveCacheReject=%llu raceWitness=%llu triRejectEffectiveAccept=%llu triAcceptEffectiveReject=%llu effectiveNonCurrent=%llu conditionedOffD=%llu conditionedOffVOnly=%llu conditionedOffIOnly=%llu g3Partition=%llu triEvalA(e/current)=%llu/%llu triEvalI(e/current)=%llu/%llu effectiveEvalA(e/current)=%llu/%llu effectiveEvalI(e/current)=%llu/%llu reconcile(double/unfinalized/overflow/unknown/fail)=%llu/%llu/%llu/%llu/%llu relevant(total/stored/truncated)=%llu/%d/%llu handleTriPresenceA(z/s/c/p/x/n)=%llu/%llu/%llu/%llu/%llu/%llu handleTriPresenceI(z/s/c/p/x/n)=%llu/%llu/%llu/%llu/%llu/%llu handleTriEvalA(z/s/c/p/x/n)=%llu/%llu/%llu/%llu/%llu/%llu handleTriEvalI(z/s/c/p/x/n)=%llu/%llu/%llu/%llu/%llu/%llu handleDrawA(z/s/c/p/x/n)=%llu/%llu/%llu/%llu/%llu/%llu handleDrawI(z/s/c/p/x/n)=%llu/%llu/%llu/%llu/%llu/%llu handleEffectiveA(z/s/c/p/x/n)=%llu/%llu/%llu/%llu/%llu/%llu handleEffectiveI(z/s/c/p/x/n)=%llu/%llu/%llu/%llu/%llu/%llu first(ord/reason/results/triAP/triIP/triAE/triIE/drawA/drawI/effA/effI)=%d/%x/%x/%016llx/%016llx/%016llx/%016llx/%016llx/%016llx/%016llx/%016llx auditPreLogUs=%llu ord=%s\n",
        audit.frameCount, audit.vertexCacheFrame, audit.surfaceCount,
        audit.available, static_cast<unsigned long long>(audit.terminalSum),
        static_cast<unsigned long long>(t[1]), static_cast<unsigned long long>(t[2]),
        static_cast<unsigned long long>(t[3]), static_cast<unsigned long long>(t[4]),
        static_cast<unsigned long long>(t[5]), static_cast<unsigned long long>(t[6]),
        static_cast<unsigned long long>(t[7]), static_cast<unsigned long long>(t[8]),
        static_cast<unsigned long long>(t[9]), static_cast<unsigned long long>(t[10]),
        static_cast<unsigned long long>(t[11]), static_cast<unsigned long long>(t[12]),
        static_cast<unsigned long long>(t[13]), static_cast<unsigned long long>(t[14]),
        static_cast<unsigned long long>(t[15]), static_cast<unsigned long long>(t[16]),
        static_cast<unsigned long long>(t[17]), static_cast<unsigned long long>(t[18]),
        static_cast<unsigned long long>(t[19]), static_cast<unsigned long long>(t[20]),
        static_cast<unsigned long long>(t[21]), static_cast<unsigned long long>(t[22]),
        static_cast<unsigned long long>(audit.cacheEligible),
        static_cast<unsigned long long>(audit.triCacheReject),
        static_cast<unsigned long long>(audit.effectiveCacheReject),
        static_cast<unsigned long long>(audit.triRejectEffectiveAccept + audit.triAcceptEffectiveReject),
        static_cast<unsigned long long>(audit.triRejectEffectiveAccept),
        static_cast<unsigned long long>(audit.triAcceptEffectiveReject),
        static_cast<unsigned long long>(audit.effectiveNonCurrent),
        static_cast<unsigned long long>(audit.conditionedOffD),
        static_cast<unsigned long long>(audit.conditionedOffVOnly),
        static_cast<unsigned long long>(audit.conditionedOffIOnly),
        static_cast<unsigned long long>(audit.effectiveNonCurrent + audit.conditionedOffD),
        static_cast<unsigned long long>(audit.triAmbientEvaluated),
        static_cast<unsigned long long>(audit.triAmbientCurrent),
        static_cast<unsigned long long>(audit.triIndexEvaluated),
        static_cast<unsigned long long>(audit.triIndexCurrent),
        static_cast<unsigned long long>(audit.effectiveAmbientEvaluated),
        static_cast<unsigned long long>(audit.effectiveAmbientCurrent),
        static_cast<unsigned long long>(audit.effectiveIndexEvaluated),
        static_cast<unsigned long long>(audit.effectiveIndexCurrent),
        static_cast<unsigned long long>(audit.doubleFinalized),
        static_cast<unsigned long long>(audit.unfinalized),
        static_cast<unsigned long long>(audit.overflowed),
        static_cast<unsigned long long>(audit.unknownTerminal),
        static_cast<unsigned long long>(audit.reconciliationFailures),
        static_cast<unsigned long long>(audit.relevantOrdinalTotal),
        audit.relevantOrdinalCount,
        static_cast<unsigned long long>(audit.relevantOrdinalTruncated),
        static_cast<unsigned long long>(audit.handleStates[0][0]), static_cast<unsigned long long>(audit.handleStates[0][1]), static_cast<unsigned long long>(audit.handleStates[0][2]), static_cast<unsigned long long>(audit.handleStates[0][3]), static_cast<unsigned long long>(audit.handleStates[0][4]), static_cast<unsigned long long>(audit.handleStates[0][5]),
        static_cast<unsigned long long>(audit.handleStates[1][0]), static_cast<unsigned long long>(audit.handleStates[1][1]), static_cast<unsigned long long>(audit.handleStates[1][2]), static_cast<unsigned long long>(audit.handleStates[1][3]), static_cast<unsigned long long>(audit.handleStates[1][4]), static_cast<unsigned long long>(audit.handleStates[1][5]),
        static_cast<unsigned long long>(audit.handleStates[2][0]), static_cast<unsigned long long>(audit.handleStates[2][1]), static_cast<unsigned long long>(audit.handleStates[2][2]), static_cast<unsigned long long>(audit.handleStates[2][3]), static_cast<unsigned long long>(audit.handleStates[2][4]), static_cast<unsigned long long>(audit.handleStates[2][5]),
        static_cast<unsigned long long>(audit.handleStates[3][0]), static_cast<unsigned long long>(audit.handleStates[3][1]), static_cast<unsigned long long>(audit.handleStates[3][2]), static_cast<unsigned long long>(audit.handleStates[3][3]), static_cast<unsigned long long>(audit.handleStates[3][4]), static_cast<unsigned long long>(audit.handleStates[3][5]),
        static_cast<unsigned long long>(audit.handleStates[4][0]), static_cast<unsigned long long>(audit.handleStates[4][1]), static_cast<unsigned long long>(audit.handleStates[4][2]), static_cast<unsigned long long>(audit.handleStates[4][3]), static_cast<unsigned long long>(audit.handleStates[4][4]), static_cast<unsigned long long>(audit.handleStates[4][5]),
        static_cast<unsigned long long>(audit.handleStates[5][0]), static_cast<unsigned long long>(audit.handleStates[5][1]), static_cast<unsigned long long>(audit.handleStates[5][2]), static_cast<unsigned long long>(audit.handleStates[5][3]), static_cast<unsigned long long>(audit.handleStates[5][4]), static_cast<unsigned long long>(audit.handleStates[5][5]),
        static_cast<unsigned long long>(audit.handleStates[6][0]), static_cast<unsigned long long>(audit.handleStates[6][1]), static_cast<unsigned long long>(audit.handleStates[6][2]), static_cast<unsigned long long>(audit.handleStates[6][3]), static_cast<unsigned long long>(audit.handleStates[6][4]), static_cast<unsigned long long>(audit.handleStates[6][5]),
        static_cast<unsigned long long>(audit.handleStates[7][0]), static_cast<unsigned long long>(audit.handleStates[7][1]), static_cast<unsigned long long>(audit.handleStates[7][2]), static_cast<unsigned long long>(audit.handleStates[7][3]), static_cast<unsigned long long>(audit.handleStates[7][4]), static_cast<unsigned long long>(audit.handleStates[7][5]),
        first.ordinal, first.reasonMask, first.resultMask,
        static_cast<unsigned long long>(first.triAmbientPresence),
        static_cast<unsigned long long>(first.triIndexPresence),
        static_cast<unsigned long long>(first.triAmbientEvaluated),
        static_cast<unsigned long long>(first.triIndexEvaluated),
        static_cast<unsigned long long>(first.drawAmbient),
        static_cast<unsigned long long>(first.drawIndex),
        static_cast<unsigned long long>(first.effectiveAmbient),
        static_cast<unsigned long long>(first.effectiveIndex),
        static_cast<unsigned long long>(auditPreLogUs),
        ordinalText);
    if (written < 0 || static_cast<size_t>(written) >= outputCapacity)
    {
        audit.available = 0;
        ++audit.reconciliationFailures;
        const int fallback = std::snprintf(
            output,
            outputCapacity,
            "PathTracePrimaryPass: R1 fixNObserve frame=%d available=0 formatError=1 reconcileFailures=%llu\n",
            audit.frameCount,
            static_cast<unsigned long long>(audit.reconciliationFailures));
        return fallback >= 0 && static_cast<size_t>(fallback) < outputCapacity;
    }
    return true;
}
