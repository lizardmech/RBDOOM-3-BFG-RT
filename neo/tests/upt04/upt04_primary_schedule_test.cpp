#include "PathTraceUnifiedPtSchedule.h"
#include "PathTraceUnifiedPtRandomDimensions.h"

#include <cmath>
#include <iostream>

namespace {

using namespace rb::upt;

int failures = 0;

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

uint32_t HashWord(uint32_t value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

uint32_t MakeReplayKey(uint32_t pixelX, uint32_t pixelY, uint32_t frameSampleIndex)
{
    uint32_t value = HashWord(pixelX ^ (pixelY * 0x9e3779b9u));
    value = HashWord(value ^ frameSampleIndex);
    value = HashWord(value ^ kRandomSlotReplayPathEvent.streamNamespace
        ^ kRandomSlotReplayPathEvent.dimension);
    return (value & 0x7fffffffu) | 1u;
}

uint32_t ReplayRandomBits(
    uint32_t replayKey,
    uint32_t replayIndex,
    uint32_t pathVertex,
    uint32_t sampleOrdinal,
    uint32_t streamNamespace,
    uint32_t dimension)
{
    uint32_t value = 0x9e3779b9u;
    value = HashWord(value ^ (replayKey & 0x7fffffffu));
    value = HashWord(value ^ replayIndex);
    value = HashWord(value ^ 0u); // frameSampleIndex
    value = HashWord(value ^ 0x0001u); // kUpt04PassInitial
    value = HashWord(value ^ pathVertex);
    value = HashWord(value ^ streamNamespace);
    value = HashWord(value ^ dimension);
    value = HashWord(value ^ sampleOrdinal);
    value = HashWord(value ^ 0u); // replayEpoch
    return value >> 8u;
}

void TestPersistedContinuationReplay()
{
    constexpr uint32_t sourceX = 1919u;
    constexpr uint32_t sourceY = 1079u;
    constexpr uint32_t frameSample = 0x12345678u;
    const uint32_t replayKey = MakeReplayKey(sourceX, sourceY, frameSample);
    const uint32_t lobe = ReplayRandomBits(
        replayKey, frameSample, 0u, 0u,
        kRandomSlotMaterialLobeSelection.streamNamespace,
        kRandomSlotMaterialLobeSelection.dimension);
    const uint32_t directionU = ReplayRandomBits(
        replayKey, frameSample, 0u, 0u,
        kRandomSlotIndirectContinuationU.streamNamespace,
        kRandomSlotIndirectContinuationU.dimension);
    const uint32_t directionV = ReplayRandomBits(
        replayKey, frameSample, 0u, 0u,
        kRandomSlotIndirectContinuationV.streamNamespace,
        kRandomSlotIndirectContinuationV.dimension);
    const uint32_t secondaryIdentity = ReplayRandomBits(
        replayKey, frameSample, 1u, 3u,
        kRandomSlotInitialLocalLightIdentity.streamNamespace,
        kRandomSlotInitialLocalLightIdentity.dimension);
    const uint32_t secondarySurfaceU = ReplayRandomBits(
        replayKey, frameSample, 1u, 3u,
        kRandomSlotInitialLocalLightSurfaceU.streamNamespace,
        kRandomSlotInitialLocalLightSurfaceU.dimension);
    const uint32_t secondarySelection = ReplayRandomBits(
        replayKey, frameSample, 1u, 3u,
        kRandomSlotInitialReservoirSelection.streamNamespace,
        kRandomSlotInitialReservoirSelection.dimension);

    Check(replayKey != 0u && (replayKey & 0x80000000u) == 0u,
        "base replay identity must preserve zero-invalid and the rescue bit");
    Check(lobe == ReplayRandomBits(replayKey, frameSample, 0u, 0u,
            kRandomSlotMaterialLobeSelection.streamNamespace,
            kRandomSlotMaterialLobeSelection.dimension)
        && directionU == ReplayRandomBits(replayKey, frameSample, 0u, 0u,
            kRandomSlotIndirectContinuationU.streamNamespace,
            kRandomSlotIndirectContinuationU.dimension)
        && directionV == ReplayRandomBits(replayKey, frameSample, 0u, 0u,
            kRandomSlotIndirectContinuationV.streamNamespace,
            kRandomSlotIndirectContinuationV.dimension),
        "persisted replayKey/replayIndex must reproduce the continuation exactly");
    Check(lobe == ReplayRandomBits(replayKey | 0x80000000u, frameSample, 0u, 0u,
            kRandomSlotMaterialLobeSelection.streamNamespace,
            kRandomSlotMaterialLobeSelection.dimension),
        "UPT-09 rescue stamping must not perturb persisted continuation replay");
    Check(secondaryIdentity == ReplayRandomBits(
            replayKey, frameSample, 1u, 3u,
            kRandomSlotInitialLocalLightIdentity.streamNamespace,
            kRandomSlotInitialLocalLightIdentity.dimension)
        && secondarySurfaceU == ReplayRandomBits(
            replayKey, frameSample, 1u, 3u,
            kRandomSlotInitialLocalLightSurfaceU.streamNamespace,
            kRandomSlotInitialLocalLightSurfaceU.dimension)
        && secondarySelection == ReplayRandomBits(
            replayKey, frameSample, 1u, 3u,
            kRandomSlotInitialReservoirSelection.streamNamespace,
            kRandomSlotInitialReservoirSelection.dimension),
        "persisted replay must reproduce secondary identity, coordinates, and inner-RIS selection");
    Check(secondaryIdentity != ReplayRandomBits(
            replayKey, frameSample, 1u, 4u,
            kRandomSlotInitialLocalLightIdentity.streamNamespace,
            kRandomSlotInitialLocalLightIdentity.dimension),
        "secondary candidate ordinals must remain distinct in persisted PSS replay");
    Check(MakeReplayKey(sourceX + 1u, sourceY, frameSample) != replayKey,
        "adjacent source pixels must retain distinct replay identities");
}

void TestSecondaryNeeRawTargetRepresentation()
{
    constexpr double rawTarget = 3.25;
    constexpr double rawContribution = 2.75;
    constexpr double finalizedMass = 7.5;
    const double foldedUcw = finalizedMass / rawTarget;
    const double foldedContribution = rawContribution * foldedUcw;
    const double foldedTarget = rawTarget * foldedUcw;
    const double effectiveProposalPdf = rawTarget / finalizedMass;
    const double rawStreamMass = rawTarget / effectiveProposalPdf;
    const double foldedResolved = foldedContribution / foldedTarget;
    const double rawResolved = rawContribution / rawTarget;

    Check(std::abs(rawStreamMass - finalizedMass) < 1.0e-12,
        "raw-target secondary NEE must stream the original finalized inner-RIS mass");
    Check(std::abs(foldedResolved - rawResolved) < 1.0e-12,
        "raw-target secondary NEE must preserve the folded-UCW resolved estimator");
}

PrimaryProducerSchedule ReadySchedule(bool upt, bool clean, int view)
{
    PrimaryProducerScheduleInput input;
    input.unifiedPtRequested = upt;
    input.cleanDiRequested = clean;
    input.cleanDiView = view;
    input.pipelineReady = true;
    input.resourcesReady = true;
    return BuildPrimaryProducerSchedule(input);
}

} // namespace

int main()
{
    TestPersistedContinuationReplay();
    TestSecondaryNeeRawTargetRepresentation();
    Check(RandomSlotsAreUnique(),
        "shared C++/Slang random-slot ledger must be collision-free");
    Check(kRandomSlotInitialReservoirSelection.streamNamespace !=
            kRandomSlotInitialLocalLightIdentity.streamNamespace,
        "reservoir selection must not share the local-light stream");
    Check(kRandomSlotIndirectRouletteSurvival.streamNamespace !=
            kRandomSlotIndirectContinuationU.streamNamespace,
        "future roulette must not shift continuation dimensions");
    {
        const PrimaryProducerSchedule plan = ReadySchedule(false, false, 0);
        Check(!plan.requested && !plan.dispatch,
            "disabled routes must not produce a camera ray");
        Check(plan.blockReason == PrimaryProducerBlockReason::NotRequested,
            "disabled routes must report not requested");
    }
    {
        const PrimaryProducerSchedule plan = ReadySchedule(true, false, 0);
        Check(plan.requestedByUnifiedPt && !plan.requestedByCleanDi,
            "UPT-only must own an independent primary request");
        Check(plan.dispatch, "UPT-only must dispatch the primary producer once");
        Check(plan.returnBeforeLegacyRoutes,
            "UPT-only must return before legacy DI/GI execution");
    }
    {
        const PrimaryProducerSchedule plan = ReadySchedule(false, true, 16);
        Check(!plan.requestedByUnifiedPt && plan.requestedByCleanDi,
            "Clean-DI production view must preserve its primary request");
        Check(plan.dispatch && !plan.returnBeforeLegacyRoutes,
            "Clean-DI must continue after the shared primary producer");
    }
    {
        const PrimaryProducerSchedule plan = ReadySchedule(true, true, 16);
        Check(plan.requestedByUnifiedPt && plan.requestedByCleanDi,
            "coexisting consumers must both be represented");
        Check(plan.dispatch,
            "coexisting consumers must schedule one shared producer dispatch");
        Check(!plan.returnBeforeLegacyRoutes,
            "Clean-DI consumer must remain reachable when both routes are on");
    }
    {
        const PrimaryProducerSchedule plan = ReadySchedule(false, true, 1);
        Check(!plan.requested && !plan.dispatch,
            "Clean-DI sentinel view must not start the primary producer");
    }
    {
        PrimaryProducerScheduleInput input;
        input.unifiedPtRequested = true;
        input.pipelineReady = false;
        input.resourcesReady = true;
        const PrimaryProducerSchedule plan = BuildPrimaryProducerSchedule(input);
        Check(plan.createPipeline && !plan.dispatch,
            "a missing primary pipeline must be created without same-plan dispatch");
        Check(plan.blockReason == PrimaryProducerBlockReason::PipelineUnavailable,
            "pipeline warmup must be explicit");
    }
    {
        PrimaryProducerScheduleInput input;
        input.unifiedPtRequested = true;
        input.pipelineReady = true;
        input.resourcesReady = false;
        const PrimaryProducerSchedule plan = BuildPrimaryProducerSchedule(input);
        Check(!plan.dispatch &&
                plan.blockReason == PrimaryProducerBlockReason::ResourcesUnavailable,
            "missing primary resources must fail closed");
    }
    {
        PrimaryProducerScheduleInput input;
        input.cleanDiRequested = true;
        input.cleanDiView = 16;
        input.pipelineReady = true;
        input.resourcesReady = true;
        input.isolationActive = true;
        input.isolationAllowsDispatch = false;
        const PrimaryProducerSchedule plan = BuildPrimaryProducerSchedule(input);
        Check(!plan.dispatch &&
                plan.blockReason == PrimaryProducerBlockReason::DispatchDeferred,
            "existing staged isolation must still defer primary dispatch");
    }

    if (failures != 0)
    {
        std::cerr << "UPT-04 primary schedule tests failed: " << failures << '\n';
        return 1;
    }
    std::cout << "UPT-04 primary schedule tests passed; one shared producer, no DI/GI ownership\n";
    return 0;
}
