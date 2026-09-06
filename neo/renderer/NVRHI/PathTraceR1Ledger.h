#pragma once

#include "PathTraceCacheGate.h"

#include <cstddef>
#include <cstdint>

enum class RtSmokeR1ValidationDisposition : uint8_t
{
    None,
    NullSurface,
    MissingFrontEndGeo,
    NullMaterial,
    ConditionedOff,
    GuiSurface,
    NullSpace,
    NullModel,
    CallbackEntity,
    MissingGeometryPayload,
    InvalidIndexCount,
    NonCurrentCache,
    Accepted
};

struct RtSmokeR1CacheValidationObservation
{
    RtSmokeR1ValidationDisposition disposition = RtSmokeR1ValidationDisposition::None;
    bool cacheEligible = false;
    RtSmokeLiveCacheVerdict liveVerdict{};
    RtSmokeDiagnosticCacheVerdict diagnosticVerdict{};
    RtSmokeLiveCacheGateSample liveSample;
    RtSmokeDiagnosticCacheGateSample diagnosticSample;
};

enum class RtSmokeR1Terminal : uint8_t
{
    Pending,
    ApplyGateSkip,
    NullSurface,
    MissingFrontEndGeo,
    NullMaterial,
    ConditionedOff,
    GuiSurface,
    NullSpace,
    NullModel,
    CallbackEntity,
    MissingGeometryPayload,
    InvalidIndexCount,
    NonCurrentCache,
    CompositeOnly,
    AlphaClipDiagnostic,
    StaticWorld,
    StaticMatch,
    RigidRouteReadyRemoved,
    SkinnedCaptureOmitted,
    AdmissionRejectedPreAppend,
    AppendEmpty,
    AdmissionRollbackPostAppend,
    Accepted,
    Count
};

constexpr int RT_SMOKE_R1_LEDGER_CAP = 4096;
constexpr int RT_SMOKE_R1_ORDINAL_CAP = 32;
constexpr int RT_SMOKE_R1_ORDINAL_TEXT_CAP = 512;
constexpr int RT_SMOKE_R1_OUTPUT_CAP = 4096;

struct RtSmokeR1RelevantOrdinal
{
    int ordinal = -1;
    uint32_t reasonMask = 0;
    uint32_t resultMask = 0;
    RtSmokeCacheHandle triAmbientPresence = 0;
    RtSmokeCacheHandle triIndexPresence = 0;
    RtSmokeCacheHandle triAmbientEvaluated = 0;
    RtSmokeCacheHandle triIndexEvaluated = 0;
    RtSmokeCacheHandle drawAmbient = 0;
    RtSmokeCacheHandle drawIndex = 0;
    RtSmokeCacheHandle effectiveAmbient = 0;
    RtSmokeCacheHandle effectiveIndex = 0;
};

struct RtSmokeR1Audit
{
    int available = 0;
    int frameCount = 0;
    int vertexCacheFrame = 0;
    int surfaceCount = 0;
    uint64_t startUs = 0;
    RtSmokeR1CacheValidationObservation rowObservation;
    uint64_t terminalTotals[static_cast<int>(RtSmokeR1Terminal::Count)] = {};
    uint8_t terminalLedger[RT_SMOKE_R1_LEDGER_CAP] = {};
    uint64_t terminalSum = 0;
    uint64_t cacheEligible = 0;
    uint64_t triCacheReject = 0;
    uint64_t effectiveCacheReject = 0;
    uint64_t triRejectEffectiveAccept = 0;
    uint64_t triAcceptEffectiveReject = 0;
    uint64_t effectiveNonCurrent = 0;
    uint64_t conditionedOffD = 0;
    uint64_t conditionedOffVOnly = 0;
    uint64_t conditionedOffIOnly = 0;
    uint64_t triAmbientEvaluated = 0;
    uint64_t triAmbientCurrent = 0;
    uint64_t triIndexEvaluated = 0;
    uint64_t triIndexCurrent = 0;
    uint64_t effectiveAmbientEvaluated = 0;
    uint64_t effectiveAmbientCurrent = 0;
    uint64_t effectiveIndexEvaluated = 0;
    uint64_t effectiveIndexCurrent = 0;
    uint64_t handleStates[8][static_cast<int>(RtSmokeCacheHandleState::Count)] = {};
    uint64_t doubleFinalized = 0;
    uint64_t unfinalized = 0;
    uint64_t overflowed = 0;
    uint64_t unknownTerminal = 0;
    uint64_t reconciliationFailures = 0;
    uint64_t relevantOrdinalTotal = 0;
    uint64_t relevantOrdinalTruncated = 0;
    int relevantOrdinalCount = 0;
    RtSmokeR1RelevantOrdinal relevantOrdinals[RT_SMOKE_R1_ORDINAL_CAP] = {};
};

RtSmokeR1Terminal RtSmokeR1TerminalForValidation(
    RtSmokeR1ValidationDisposition disposition);
uint32_t RtSmokeR1ConditionedOffMask(
    RtSmokeCacheHandle ambientHandle,
    RtSmokeCacheHandle indexHandle,
    int currentFrame);
uint32_t RtSmokeR1RaceWitnessMask(
    bool cacheEligible,
    const RtSmokeLiveCacheVerdict& liveVerdict,
    const RtSmokeDiagnosticCacheVerdict& diagnosticVerdict);
void RtSmokeR1Finalize(
    RtSmokeR1Audit& audit,
    int ordinal,
    RtSmokeR1Terminal terminal);
void RtSmokeR1ObserveConditionedOff(
    RtSmokeR1Audit& audit,
    int ordinal,
    RtSmokeCacheHandle selectedAmbient,
    RtSmokeCacheHandle selectedIndex);
void RtSmokeR1ObserveCacheSample(
    RtSmokeR1Audit& audit,
    int ordinal,
    const RtSmokeR1CacheValidationObservation& observation);
void RtSmokeR1Reconcile(RtSmokeR1Audit& audit);
bool RtSmokeR1FormatLine(
    RtSmokeR1Audit& audit,
    uint64_t auditPreLogUs,
    char* output,
    size_t outputCapacity);
