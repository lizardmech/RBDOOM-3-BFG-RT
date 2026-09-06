#include "../renderer/VertexCacheHandle.h"

#include "../renderer/NVRHI/PathTraceCacheGate.h"
#include "../renderer/NVRHI/PathTraceR1Ledger.h"

#include <cstring>
#include <iostream>
#include <limits>
#include <type_traits>

namespace
{

int failures = 0;

void Check(bool condition, const char* name)
{
    if (condition)
    {
        std::cout << "[PASS] " << name << "\n";
    }
    else
    {
        std::cout << "[FAIL] " << name << "\n";
        ++failures;
    }
}

RtSmokeCacheHandle FrameHandle(int frame)
{
    return static_cast<RtSmokeCacheHandle>(frame & VERTCACHE_FRAME_MASK)
        << VERTCACHE_FRAME_SHIFT;
}

static_assert(std::is_invocable<decltype(&SmokeLiveCacheGateRejects),
        const RtSmokeLiveCacheVerdict&>::value,
    "the actual live verdict must be accepted by the live consumer shape");
static_assert(!std::is_invocable<decltype(&SmokeLiveCacheGateRejects),
        const RtSmokeDiagnosticCacheVerdict&>::value,
    "the live consumer must reject a diagnostic verdict");

void TestCurrencyAuthority()
{
    const int frame = 17;
    const RtSmokeCacheHandle current = FrameHandle(frame);
    const RtSmokeCacheHandle previous = FrameHandle(frame - 1);
    const RtSmokeCacheHandle stale = FrameHandle(frame - 2);
    Check(!VertCacheHandleIsCurrent(0, frame), "currency zero is noncurrent outside frame zero");
    Check(VertCacheHandleIsCurrent(0, 0), "currency zero preserves exact frame-zero behavior");
    Check(VertCacheHandleIsCurrent(VERTCACHE_STATIC, frame), "currency static");
    Check(VertCacheHandleIsCurrent(current, frame), "currency current");
    Check(!VertCacheHandleIsCurrent(previous, frame), "currency previous");
    Check(!VertCacheHandleIsCurrent(stale, frame), "currency stale");
    Check(ClassifySmokeCacheHandle(0, frame) == RtSmokeCacheHandleState::Absent,
        "decoder checks zero before currency bits");
    Check(ClassifySmokeCacheHandle(VERTCACHE_STATIC, frame) == RtSmokeCacheHandleState::Static,
        "decoder static");
    Check(ClassifySmokeCacheHandle(current, frame) == RtSmokeCacheHandleState::Current,
        "decoder current");
    Check(ClassifySmokeCacheHandle(previous, frame) == RtSmokeCacheHandleState::Previous,
        "decoder previous");
    Check(ClassifySmokeCacheHandle(stale, frame) == RtSmokeCacheHandleState::Stale,
        "decoder stale");
    // Independently encoded against the live #if 1 layout so that mutating
    // VERTCACHE_FRAME_SHIFT changes only the decoder, not the fixture.
    constexpr RtSmokeCacheHandle kPinnedPreviousFrameAtWrap =
        static_cast<RtSmokeCacheHandle>(0x1fff) << 51;
    Check(ClassifySmokeCacheHandle(kPinnedPreviousFrameAtWrap, 0) ==
            RtSmokeCacheHandleState::Previous,
        "decoder frame wrap pins active vertex-cache layout");

    static_assert(std::is_same<decltype(VertCacheHandleIsCurrent(
            RtSmokeCacheHandle{}, int{})), bool>::value,
        "shared currency authority must remain bool(handle, frame)");
}

struct BetweenReadMutationPolicy
{
    RtSmokeCacheHandle* ambient = nullptr;
    RtSmokeCacheHandle* index = nullptr;
    RtSmokeCacheHandle ambientReplacement = 0;
    RtSmokeCacheHandle indexReplacement = 0;
    int presenceReads = 0;
    int evaluatedReads = 0;

    RtSmokeCacheHandle ReadPresence(const RtSmokeCacheHandle& slot)
    {
        ++presenceReads;
        return slot;
    }

    RtSmokeCacheHandle ReadEvaluated(const RtSmokeCacheHandle& slot)
    {
        ++evaluatedReads;
        if (&slot == ambient && ambient)
        {
            *ambient = ambientReplacement;
        }
        else if (&slot == index && index)
        {
            *index = indexReplacement;
        }
        return slot;
    }

    int ReadCurrentFrame(const int& currentFrame)
    {
        return currentFrame;
    }
};

void TestLiveGate()
{
    const int frame = 23;
    const RtSmokeCacheHandle current = FrameHandle(frame);
    const RtSmokeCacheHandle stale = FrameHandle(frame - 2);
    RtSmokeLiveCacheGateSample sample;

    Check(EvaluateSmokeLiveCacheGate(0, 0, frame, &sample).Decision() ==
            RtSmokeLiveCacheDecision::Accept &&
            sample.ambient.result == RtSmokeCacheEvaluationResult::NotEvaluated &&
            sample.index.result == RtSmokeCacheEvaluationResult::NotEvaluated,
        "live gate absent slots");
    Check(EvaluateSmokeLiveCacheGate(VERTCACHE_STATIC, current, frame, &sample).Decision() ==
            RtSmokeLiveCacheDecision::Accept &&
            sample.ambient.result == RtSmokeCacheEvaluationResult::Current &&
            sample.index.result == RtSmokeCacheEvaluationResult::Current,
        "live gate static/current slots");
    Check(EvaluateSmokeLiveCacheGate(stale, current, frame, &sample).Decision() ==
            RtSmokeLiveCacheDecision::Reject &&
            sample.ambient.presenceHandle == stale &&
            sample.ambient.evaluatedHandle == stale &&
            sample.ambient.result == RtSmokeCacheEvaluationResult::NonCurrent &&
            sample.index.presenceHandle == current &&
            sample.index.result == RtSmokeCacheEvaluationResult::NotEvaluated,
        "live gate ambient rejection preserves exact sample and index short circuit");
    Check(EvaluateSmokeLiveCacheGate(current, stale, frame, &sample).Decision() ==
            RtSmokeLiveCacheDecision::Reject &&
            sample.index.evaluatedHandle == stale &&
            sample.index.result == RtSmokeCacheEvaluationResult::NonCurrent,
        "live gate index rejection");

    RtSmokeCacheHandle ambient = current;
    RtSmokeCacheHandle index = current;
    BetweenReadMutationPolicy mutation;
    mutation.ambient = &ambient;
    mutation.index = &index;
    mutation.ambientReplacement = stale;
    mutation.indexReplacement = current;
    const RtSmokeLiveCacheVerdict mutated = EvaluateSmokeLiveCacheGateKernel(
        ambient, index, frame, &sample, mutation);
    Check(mutated.Decision() == RtSmokeLiveCacheDecision::Reject &&
            sample.ambient.presenceHandle == current &&
            sample.ambient.evaluatedHandle == stale &&
            sample.index.result == RtSmokeCacheEvaluationResult::NotEvaluated &&
            mutation.presenceReads == 2 && mutation.evaluatedReads == 1,
        "production-shared seam observes deterministic between-read mutation");
}

void TestDiagnosticGate()
{
    const int frame = 31;
    const RtSmokeCacheHandle current = FrameHandle(frame);
    const RtSmokeCacheHandle stale = FrameHandle(frame - 2);
    RtSmokeDiagnosticCacheGateSample sample;
    Check(EvaluateSmokeDiagnosticCacheGate(
                current, 0, stale, current, frame, &sample).Decision() ==
            RtSmokeDiagnosticCacheDecision::Accept &&
            sample.selectedAmbientHandle == current &&
            sample.selectedIndexHandle == current,
        "diagnostic drawSurf override and zero-first tri fallback");
    Check(EvaluateSmokeDiagnosticCacheGate(
                0, 0, stale, current, frame, &sample).Decision() ==
            RtSmokeDiagnosticCacheDecision::Reject &&
            sample.selectedAmbientHandle == stale &&
            sample.ambientResult == RtSmokeCacheEvaluationResult::NonCurrent &&
            sample.indexResult == RtSmokeCacheEvaluationResult::NotEvaluated,
        "diagnostic retained selection and ambient short circuit");
    Check(SelectSmokeDiagnosticCacheHandle(7, 9) == 7 &&
            SelectSmokeDiagnosticCacheHandle(0, 9) == 9,
        "diagnostic selector exact retained handle");
}

RtSmokeR1CacheValidationObservation RejectingObservation(int frame)
{
    const RtSmokeCacheHandle stale = FrameHandle(frame - 2);
    RtSmokeR1CacheValidationObservation observation;
    observation.cacheEligible = true;
    observation.disposition = RtSmokeR1ValidationDisposition::NonCurrentCache;
    observation.liveVerdict = RtSmokeLiveCacheVerdict(
        RtSmokeLiveCacheDecision::Reject);
    observation.diagnosticVerdict = RtSmokeDiagnosticCacheVerdict(
        RtSmokeDiagnosticCacheDecision::Reject);
    observation.liveSample.ambient.presenceHandle = stale;
    observation.liveSample.ambient.evaluatedHandle = stale;
    observation.liveSample.ambient.evaluatedFrame = frame;
    observation.liveSample.ambient.result = RtSmokeCacheEvaluationResult::NonCurrent;
    observation.diagnosticSample.triAmbientHandle = stale;
    observation.diagnosticSample.selectedAmbientHandle = stale;
    observation.diagnosticSample.ambientEvaluatedFrame = frame;
    observation.diagnosticSample.ambientResult = RtSmokeCacheEvaluationResult::NonCurrent;
    return observation;
}

RtSmokeR1Audit CompleteLedgerFixture()
{
    RtSmokeR1Audit audit;
    audit.available = 1;
    audit.frameCount = 77;
    audit.vertexCacheFrame = 41;
    audit.surfaceCount = static_cast<int>(RtSmokeR1Terminal::Count) - 1;
    for (int terminal = 1;
         terminal < static_cast<int>(RtSmokeR1Terminal::Count);
         ++terminal)
    {
        RtSmokeR1Finalize(
            audit,
            terminal - 1,
            static_cast<RtSmokeR1Terminal>(terminal));
    }
    RtSmokeR1ObserveCacheSample(audit, 0, RejectingObservation(audit.vertexCacheFrame));
    return audit;
}

void TestLedger()
{
    RtSmokeR1Audit complete = CompleteLedgerFixture();
    RtSmokeR1Reconcile(complete);
    Check(complete.available != 0 &&
            complete.terminalSum == static_cast<uint64_t>(complete.surfaceCount) &&
            complete.reconciliationFailures == 0,
        "ledger every terminal and all populations reconcile");

    RtSmokeR1Audit omitted;
    omitted.available = 1;
    omitted.surfaceCount = 1;
    RtSmokeR1Reconcile(omitted);
    Check(omitted.available == 0 && omitted.unfinalized == 1,
        "ledger omitted/pending fails closed");

    RtSmokeR1Audit duplicate;
    duplicate.available = 1;
    duplicate.surfaceCount = 1;
    RtSmokeR1Finalize(duplicate, 0, RtSmokeR1Terminal::Accepted);
    RtSmokeR1Finalize(duplicate, 0, RtSmokeR1Terminal::Accepted);
    Check(duplicate.available == 0 && duplicate.doubleFinalized == 1,
        "ledger double finalize fails closed");

    RtSmokeR1Audit pending;
    pending.available = 1;
    pending.surfaceCount = 1;
    RtSmokeR1Finalize(pending, 0, RtSmokeR1Terminal::Pending);
    Check(pending.available == 0 && pending.unknownTerminal == 1,
        "ledger explicit pending terminal fails closed");

    RtSmokeR1Audit unknown;
    unknown.available = 1;
    unknown.surfaceCount = 1;
    RtSmokeR1Finalize(
        unknown,
        0,
        static_cast<RtSmokeR1Terminal>(255));
    Check(unknown.available == 0 && unknown.unknownTerminal == 1,
        "ledger out-of-range terminal fails closed");

    RtSmokeR1Audit overflow;
    overflow.available = 1;
    overflow.surfaceCount = RT_SMOKE_R1_LEDGER_CAP + 1;
    RtSmokeR1Finalize(
        overflow,
        RT_SMOKE_R1_LEDGER_CAP,
        RtSmokeR1Terminal::Accepted);
    Check(overflow.available == 0 && overflow.overflowed == 1,
        "ledger overflow fails closed");

    const RtSmokeLiveCacheVerdict liveReject(RtSmokeLiveCacheDecision::Reject);
    const RtSmokeLiveCacheVerdict liveAccept(RtSmokeLiveCacheDecision::Accept);
    const RtSmokeDiagnosticCacheVerdict diagnosticReject(
        RtSmokeDiagnosticCacheDecision::Reject);
    const RtSmokeDiagnosticCacheVerdict diagnosticAccept(
        RtSmokeDiagnosticCacheDecision::Accept);
    Check(RtSmokeR1RaceWitnessMask(true, liveReject, diagnosticAccept) == 1 &&
            RtSmokeR1RaceWitnessMask(true, liveAccept, diagnosticReject) == 2 &&
            RtSmokeR1RaceWitnessMask(true, liveAccept, diagnosticAccept) == 0 &&
            RtSmokeR1RaceWitnessMask(true, liveReject, diagnosticReject) == 0 &&
            RtSmokeR1RaceWitnessMask(false, liveReject, diagnosticAccept) == 0,
        "ledger both witness directions and agreement/ineligible cases");

    RtSmokeR1Audit g3;
    g3.vertexCacheFrame = 9;
    RtSmokeR1ObserveConditionedOff(
        g3, 0, FrameHandle(8), FrameHandle(8));
    RtSmokeR1ObserveConditionedOff(
        g3, 1, FrameHandle(8), 0);
    RtSmokeR1ObserveConditionedOff(
        g3, 2, 0, FrameHandle(8));
    Check(g3.conditionedOffD == 1 && g3.conditionedOffVOnly == 1 &&
            g3.conditionedOffIOnly == 1 &&
            g3.effectiveNonCurrent + g3.conditionedOffD == 1,
        "ledger G3 partition is nonoverlapping");

    RtSmokeR1Audit ordinals;
    ordinals.vertexCacheFrame = 15;
    const RtSmokeR1CacheValidationObservation observation =
        RejectingObservation(ordinals.vertexCacheFrame);
    for (int ordinal = 0; ordinal < RT_SMOKE_R1_ORDINAL_CAP + 1; ++ordinal)
    {
        RtSmokeR1ObserveCacheSample(ordinals, ordinal, observation);
    }
    bool stableOrder = ordinals.relevantOrdinalCount == RT_SMOKE_R1_ORDINAL_CAP &&
        ordinals.relevantOrdinalTruncated == 1 &&
        ordinals.relevantOrdinalTotal == RT_SMOKE_R1_ORDINAL_CAP + 1;
    for (int ordinal = 0; ordinal < ordinals.relevantOrdinalCount; ++ordinal)
    {
        stableOrder = stableOrder &&
            ordinals.relevantOrdinals[ordinal].ordinal == ordinal;
    }
    Check(stableOrder, "ledger ordinal stable order and exact truncation");

    RtSmokeR1Audit populationMutation = CompleteLedgerFixture();
    ++populationMutation.handleStates[0][0];
    RtSmokeR1Reconcile(populationMutation);
    Check(populationMutation.available == 0 &&
            populationMutation.reconciliationFailures != 0,
        "ledger population mutation fails reconciliation");

    RtSmokeR1Audit formatted = CompleteLedgerFixture();
    formatted.conditionedOffD = std::numeric_limits<uint64_t>::max();
    char line[RT_SMOKE_R1_OUTPUT_CAP];
    const bool formatOk = RtSmokeR1FormatLine(
        formatted,
        std::numeric_limits<uint64_t>::max(),
        line,
        sizeof(line));
    const char* prefix = "PathTracePrimaryPass: R1 fixNObserve";
    Check(formatOk &&
            std::memcmp(line, prefix, std::strlen(prefix)) == 0 &&
            std::strlen(line) < sizeof(line),
        "ledger bounded stable-prefix 64-bit-safe formatting");
}

} // namespace

int main()
{
    TestCurrencyAuthority();
    TestLiveGate();
    TestDiagnosticGate();
    TestLedger();
    if (failures != 0)
    {
        std::cerr << failures << " R1 harness checks failed\n";
        return 1;
    }
    std::cout << "PathTraceR1Harness: PASS\n";
    return 0;
}
