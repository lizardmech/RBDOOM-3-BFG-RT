#include "../idlib/precompiled.h"
#include "PathTraceCommittedBaseline.h"
#include "PathTraceCommittedCapture.h"

#include <cstdio>
#include <cstring>
#include <new>

bool AssertFailed(const char*, int, const char*)
{
    return false;
}

void* Mem_Alloc16(std::size_t size, memTag_t)
{
    return _aligned_malloc(size, 16);
}

void Mem_Free16(void* pointer)
{
    _aligned_free(pointer);
}

void* Mem_ClearedAlloc(std::size_t size, memTag_t tag)
{
    void* pointer = Mem_Alloc16(size, tag);
    if (pointer)
    {
        std::memset(pointer, 0, size);
    }
    return pointer;
}

namespace
{
int failures = 0;

void Check(bool condition, const char* name)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", name);
        ++failures;
    }
}

RtPathTraceCommittedBaselineEpoch Baseline(
    std::uint64_t token, std::uint64_t frame, std::uint64_t world,
    std::uint64_t mapLoad, std::uint64_t barrier)
{
    RtPathTraceCommittedBaselineEpoch value;
    value.committedViewToken = token;
    value.baselineFrameIndex = frame;
    value.worldLifecycleGeneration = world;
    value.mapLoadSerial = mapLoad;
    value.mapTimeStamp = 77;
    std::strcpy(value.mapName, "maps/test.proc");
    value.barrierGeneration = barrier;
    value.capturedAfterRootEndFrame = true;
    value.capturedAfterStaticPrune = true;
    return value;
}

RtPathTracePrimaryViewDtoLineage Dto(
    std::uint64_t token, std::uint64_t frame, std::uint64_t world,
    std::uint64_t mapLoad, std::uint64_t barrier)
{
    RtPathTracePrimaryViewDtoLineage value;
    value.sealedViewToken = token;
    value.predecessorCommittedViewToken = token - 1;
    value.frameIndex = frame;
    value.worldLifecycleGeneration = world;
    value.mapLoadSerial = mapLoad;
    value.mapTimeStamp = 77;
    std::strcpy(value.mapName, "maps/test.proc");
    value.barrierGeneration = barrier;
    value.configFingerprint = 0x1234;
    value.primaryView = true;
    value.complete = true;
    return value;
}

RtPathTraceCommittedPlanningBaseline CarrierSeed()
{
    RtPathTraceCommittedPlanningBaseline seed;
    seed.epoch = Baseline(70, 700, 10, 14, 1);
    seed.epoch.sealedPrimaryViewToken = 70;
    seed.epoch.ownerUniverseFrameIndex = 900;
    seed.epoch.instanceUniverseGeneration = 2;
    seed.epoch.geometryUniverseGeneration = 3;
    seed.semanticConfig.recordAllInstanceClasses = true;
    seed.semanticConfig.removeRoutedRigidDynamic = true;
    seed.semanticConfig.rigidRouteEmissiveCards = true;
    seed.semanticConfig.configFingerprint = 0x1234;
    seed.semanticConfig.configComplete = true;
    return seed;
}

void FillSyntheticCarrierShapes(
    RtPathTraceCommittedPlanningBaseline& candidate,
    const RtPathTraceCommittedBaselineCarrierCounts& counts)
{
    candidate.applyGate.omittedSkinKeys.resize(counts.applyGateKeys, 1);
    candidate.applyGateComplete = true;
    candidate.priorSkinnedRecords.resize(counts.priorSkinnedRecords);
    candidate.priorSkinnedRecordsComplete = true;
    const RtPathTracePlanningSnapshotEpoch mapped =
        RtPathTraceMapCommittedBaselineToPlanningEpoch(candidate.epoch);
    candidate.instanceUniverse.epoch = mapped;
    candidate.instanceUniverse.ownerGeneration =
        candidate.epoch.instanceUniverseGeneration;
    candidate.instanceUniverse.complete = true;
    candidate.geometryUniverse.epoch = mapped;
    candidate.geometryUniverse.ownerGeneration =
        candidate.epoch.geometryUniverseGeneration;
    candidate.geometryUniverse.complete = true;
}
}

int main()
{
    Check(!RtPathTraceCommittedPrimaryDtoMayPublish(
            true, true, false, true, 101, 101),
        "queued but not joined cannot publish complete DTO");
    Check(!RtPathTraceCommittedPrimaryDtoMayPublish(
            true, false, false, false, 0, 101),
        "preflight fallback cannot publish complete DTO");
    Check(!RtPathTraceCommittedPrimaryDtoMayPublish(
            true, false, false, false, 0, 101),
        "budget fallback cannot publish complete DTO");
    Check(!RtPathTraceCommittedPrimaryDtoMayPublish(
            true, true, true, false, 101, 101),
        "incomplete product cannot publish complete DTO");
    Check(!RtPathTraceCommittedPrimaryDtoMayPublish(
            true, true, true, true, 102, 101),
        "token mismatch cannot publish complete DTO");
    Check(RtPathTraceCommittedPrimaryDtoMayPublish(
            true, true, true, true, 101, 101),
        "joined exact-token product publishes complete DTO");

    RtPathTracePrimarySemanticDto semanticDto;
    semanticDto.complete = true;
    semanticDto.sealedPrimaryViewToken = 101;
    semanticDto.sourceDrawSurfCount = 1;
    semanticDto.surfaceCount = 1;
    semanticDto.surfaces = reinterpret_cast<RtPathTraceCaptureRawSurface*>(1);
    Check(RtPathTracePrimarySemanticDtoCarrierComplete(semanticDto, 101),
        "source DTO complete with semantic facts absent");
    semanticDto.factsDerivedSurfaceCount = 1;
    Check(!RtPathTracePrimarySemanticDtoCarrierComplete(semanticDto, 101),
        "frontend source DTO rejects derived readiness facts");
    semanticDto.factsDerivedSurfaceCount = 0;
    semanticDto.classifierStageCount = 1;
    Check(!RtPathTracePrimarySemanticDtoCarrierComplete(semanticDto, 101),
        "missing classifier stage carrier rejected");
    semanticDto.classifierStageCount = 0;
    semanticDto.surfaces = nullptr;
    Check(!RtPathTracePrimarySemanticDtoCarrierComplete(semanticDto, 101),
        "missing raw surface carrier rejected");

    RtPathTraceCommittedBaselineRendezvous rendezvous;
    const std::uint64_t barrier = rendezvous.BarrierGeneration();
    RtPathTraceCommittedBaselineEpoch baseline = Baseline(10, 100, 4, 8, barrier);
    RtPathTracePrimaryViewDtoLineage dto = Dto(11, 101, 4, 8, barrier);
    rendezvous.PublishPrimaryDto(dto);
    rendezvous.PublishBaseline(baseline);
    const std::uint64_t firstBaselinePublicationSerial =
        rendezvous.BaselinePublicationSerial();
    const std::uint64_t firstDtoPublicationSerial =
        rendezvous.DtoPublicationSerial();
    rendezvous.PublishBaseline(baseline);
    rendezvous.PublishPrimaryDto(dto);
    Check(rendezvous.BaselinePublicationSerial() ==
            firstBaselinePublicationSerial,
        "equal baseline duplicate does not publish");
    Check(rendezvous.DtoPublicationSerial() == firstDtoPublicationSerial,
        "equal DTO duplicate does not publish");
    RtPathTraceCommittedBaselineEpoch pairedBaseline;
    RtPathTracePrimaryViewDtoLineage pairedDto;
    Check(rendezvous.TryAcquirePair(pairedBaseline, pairedDto), "exact pair");
    const std::uint64_t firstPairSerial = rendezvous.PairSerial();
    Check(!rendezvous.TryAcquirePair(pairedBaseline, pairedDto), "pair consumed once");
    rendezvous.PublishBaseline(baseline);
    rendezvous.PublishPrimaryDto(dto);
    Check(rendezvous.BaselinePublicationSerial() ==
            firstBaselinePublicationSerial,
        "consumed baseline replay does not publish");
    Check(rendezvous.DtoPublicationSerial() == firstDtoPublicationSerial,
        "consumed DTO replay does not publish");
    Check(!rendezvous.TryAcquirePair(pairedBaseline, pairedDto),
        "exact consumed pair cannot replay");
    Check(rendezvous.PairSerial() == firstPairSerial,
        "replay does not advance pair serial");

    rendezvous.PublishBaseline(Baseline(11, 101, 4, 8, barrier));
    RtPathTracePrimaryViewDtoLineage wrongToken = Dto(12, 102, 4, 8, barrier);
    wrongToken.predecessorCommittedViewToken = 10;
    rendezvous.PublishPrimaryDto(wrongToken);
    Check(!rendezvous.TryAcquirePair(pairedBaseline, pairedDto),
        "reject wrong predecessor token");

    rendezvous.PublishBaseline(Baseline(12, 102, 4, 8, barrier));
    rendezvous.PublishPrimaryDto(Dto(14, 104, 4, 8, barrier));
    Check(!rendezvous.TryAcquirePair(pairedBaseline, pairedDto), "reject two-frame lineage");
    const std::uint64_t beforeStrictNewer =
        rendezvous.BaselinePublicationSerial();
    rendezvous.PublishBaseline(Baseline(13, 103, 4, 8, barrier));
    Check(rendezvous.BaselinePublicationSerial() == beforeStrictNewer + 1,
        "strict-newer baseline supersedes once");
    Check(rendezvous.TryAcquirePair(pairedBaseline, pairedDto), "newer baseline supersedes");

    rendezvous.PublishBaseline(Baseline(20, 200, 4, 8, barrier));
    rendezvous.PublishPrimaryDto(Dto(21, 201, 5, 8, barrier));
    Check(!rendezvous.TryAcquirePair(pairedBaseline, pairedDto), "reject world mismatch");
    rendezvous.Drain(RtPathTraceCommittedBaselineDrainReason::WorldReplacement);
    Check(rendezvous.LastDrainReason() == RtPathTraceCommittedBaselineDrainReason::WorldReplacement,
        "world drain reason");
    Check(rendezvous.BarrierGeneration() != barrier, "world drain advances barrier");
    Check(!rendezvous.TryAcquirePair(pairedBaseline, pairedDto), "world drain clears pending");

    const std::uint64_t mapBarrier = rendezvous.BarrierGeneration();
    rendezvous.PublishBaseline(Baseline(30, 300, 6, 9, mapBarrier));
    rendezvous.PublishPrimaryDto(Dto(31, 301, 6, 10, mapBarrier));
    Check(!rendezvous.TryAcquirePair(pairedBaseline, pairedDto), "reject map mismatch");
    rendezvous.Drain(RtPathTraceCommittedBaselineDrainReason::MapReset);

    const std::uint64_t vidBarrier = rendezvous.BarrierGeneration();
    rendezvous.PublishBaseline(Baseline(40, 400, 7, 11, vidBarrier));
    rendezvous.PublishPrimaryDto(Dto(41, 401, 7, 11, vidBarrier));
    rendezvous.Drain(RtPathTraceCommittedBaselineDrainReason::VidRestart);
    Check(!rendezvous.TryAcquirePair(pairedBaseline, pairedDto),
        "vid_restart drain clears pending");

    const std::uint64_t shutdownBarrier = rendezvous.BarrierGeneration();
    rendezvous.PublishBaseline(Baseline(50, 500, 8, 12, shutdownBarrier));
    rendezvous.PublishPrimaryDto(Dto(51, 501, 8, 12, shutdownBarrier));
    rendezvous.Drain(RtPathTraceCommittedBaselineDrainReason::Shutdown);
    Check(!rendezvous.TryAcquirePair(pairedBaseline, pairedDto),
        "shutdown drain clears pending");
    Check(rendezvous.LastDrainReason() == RtPathTraceCommittedBaselineDrainReason::Shutdown,
        "shutdown drain");

    RtPathTraceCommittedBaselineRendezvous subviewRendezvous;
    RtPathTraceCommittedBaselineEpoch subviewBaseline = Baseline(
        60, 600, 9, 13, subviewRendezvous.BarrierGeneration());
    RtPathTracePrimaryViewDtoLineage subviewDto = Dto(
        61, 601, 9, 13, subviewRendezvous.BarrierGeneration());
    subviewDto.primaryView = false;
    subviewRendezvous.PublishBaseline(subviewBaseline);
    subviewRendezvous.PublishPrimaryDto(subviewDto);
    Check(!subviewRendezvous.TryAcquirePair(pairedBaseline, pairedDto),
        "subview remains serial");

    RtPathTraceCommittedBaselineDispatchGuard guard;
    Check(RtPathTraceCommittedBaselineMayDispatch(guard), "default dispatch guard");
    guard.baselineAdmissionRoutesPresent = true;
    Check(!RtPathTraceCommittedBaselineMayDispatch(guard), "admission hint guard");
    guard.baselineAdmissionRoutesPresent = false;
    guard.captureAllocationFailureArmed = true;
    Check(!RtPathTraceCommittedBaselineMayDispatch(guard), "allocation seam guard");
    guard = RtPathTraceCommittedBaselineDispatchGuard{};
    const void* admissionRoutes = nullptr;
    Check(RtPathTraceCommittedBaselineMayDispatch(guard) &&
        admissionRoutes == nullptr && !guard.baselineSkinnedSplitGate,
        "MayDispatch-eligible semantic fixture has null routes and split gate false");

    const std::uint64_t viewTokenA = NextPathTraceSealedPrimaryViewToken();
    const std::uint64_t viewTokenB = NextPathTraceSealedPrimaryViewToken();
    const std::uint64_t worldA = NextPathTraceRenderWorldLifecycleGeneration();
    const std::uint64_t worldB = NextPathTraceRenderWorldLifecycleGeneration();
    Check(viewTokenA != 0 && viewTokenB > viewTokenA, "monotonic view token");
    Check(worldA != 0 && worldB > worldA, "monotonic world generation");

    RtPathTraceCommittedBaselineEpoch domains = Baseline(70, 700, 10, 14, 1);
    domains.sealedPrimaryViewToken = 70;
    domains.ownerUniverseFrameIndex = 900;
    domains.instanceUniverseGeneration = 2;
    domains.geometryUniverseGeneration = 3;
    domains.staticMaterialGeneration = 4;
    Check(RtPathTraceCommittedHistoryEpochValid(domains, 70, 900),
        "committed history uses explicit sealed and owner frame domains");
    Check(!RtPathTraceCommittedHistoryEpochValid(domains, 70, 700),
        "sealed frame cannot substitute for owner-universe frame");
    const RtPathTracePlanningSnapshotEpoch mapped =
        RtPathTraceMapCommittedBaselineToPlanningEpoch(domains);
    Check(mapped.generation == 70,
        "committed mapper generation is sealed primary token");
    Check(mapped.frameIndex == 900 && mapped.frameIndex != 70 &&
            mapped.frameIndex != 700,
        "committed mapper frame is owner-universe frame");
    Check(mapped.mapTimeStamp == domains.mapTimeStamp &&
            mapped.mapLoadSerial == domains.mapLoadSerial &&
            std::equal(std::begin(mapped.mapName), std::end(mapped.mapName),
                std::begin(domains.mapName)),
        "committed mapper copies map identity");
    Check(!mapped.capturedAfterBeginFrame &&
            !mapped.capturedAfterStaticPreload &&
            !mapped.capturedBeforeSerialMutate &&
            !RtPathTracePlanningEpochValid(mapped),
        "committed mapper leaves same-frame flags false and invalid");
    Check(RtPathTraceCommittedStaticQueryEpochValid(domains, 900),
        "committed static query predicate");
    Check(RtPathTraceCommittedRigidQueryEpochValid(domains, 900),
        "committed rigid query predicate");

    RtPathTraceCommittedPlanningBaseline carriers;
    carriers.epoch = domains;
    carriers.instanceUniverse.complete = true;
    carriers.instanceUniverse.ownerGeneration =
        domains.instanceUniverseGeneration;
    carriers.geometryUniverse.complete = true;
    carriers.geometryUniverse.ownerGeneration =
        domains.geometryUniverseGeneration;
    carriers.semanticConfig = CarrierSeed().semanticConfig;
    carriers.applyGateComplete = true;
    carriers.priorSkinnedRecordsComplete = true;
    carriers.complete = true;
    Check(carriers.Valid(), "apply-gate and prior-skinned baseline carriers complete");
    const RtPathTracePlanningSnapshotEpoch carrierEpoch =
        RtPathTraceMapCommittedBaselineToPlanningEpoch(carriers.epoch);
    carriers.instanceUniverse.epoch = carrierEpoch;
    carriers.geometryUniverse.epoch = carrierEpoch;
    RtPathTraceInstanceHistoryPod history;
    history.instanceId = 41;
    history.lastSeenFrame = 899;
    history.lastObjectToWorld[0] = 1.0f;
    history.lastObjectToWorld[5] = 1.0f;
    history.lastObjectToWorld[10] = 1.0f;
    history.lastObjectToWorld[15] = 1.0f;
    carriers.instanceUniverse.histories.push_back(history);
    RtSmokeStaticSurfacePod staticSurface;
    staticSurface.valid = true;
    staticSurface.key = 51;
    carriers.geometryUniverse.staticSurfaces.push_back(staticSurface);
    float currentTransform[16] = {};
    currentTransform[0] = currentTransform[5] =
        currentTransform[10] = currentTransform[15] = 1.0f;
    Check(ApplyInstanceHistoryFromPod(carriers.instanceUniverse,
            carriers.epoch, 41, currentTransform, 900).found &&
        HasStaticSurfaceFromCommittedBaselinePod(carriers.geometryUniverse,
            carriers.epoch, 51),
        "committed history/static queries accept exact owner generations");
    const RtPathTraceCommittedBaselineCarrierCounts emptyCarrierCounts;
    Check(RtPathTraceCommittedBaselineCarrierShapeValid(
            carriers, emptyCarrierCounts),
        "committed carrier shape accepts coherent owner generations");
    RtPathTraceCommittedPlanningBaseline generationMismatch = carriers;
    ++generationMismatch.instanceUniverse.ownerGeneration;
    Check(!generationMismatch.Valid() &&
        !RtPathTraceCommittedBaselineCarrierShapeValid(
            generationMismatch, emptyCarrierCounts),
        "instance owner-generation mismatch rejects baseline carrier");
    Check(!ApplyInstanceHistoryFromPod(generationMismatch.instanceUniverse,
            generationMismatch.epoch, 41, currentTransform, 900).found,
        "committed history query rejects instance owner-generation mismatch");
    generationMismatch = carriers;
    ++generationMismatch.geometryUniverse.ownerGeneration;
    Check(!generationMismatch.Valid() &&
        !RtPathTraceCommittedBaselineCarrierShapeValid(
            generationMismatch, emptyCarrierCounts),
        "geometry owner-generation mismatch rejects baseline carrier");
    Check(!HasStaticSurfaceFromCommittedBaselinePod(
            generationMismatch.geometryUniverse, generationMismatch.epoch, 51),
        "committed static query rejects geometry owner-generation mismatch");
    generationMismatch = carriers;
    generationMismatch.instanceUniverse.ownerGeneration = 0;
    Check(!generationMismatch.Valid(),
        "zero instance owner generation rejects baseline");
    generationMismatch = carriers;
    generationMismatch.geometryUniverse.ownerGeneration = 0;
    Check(!generationMismatch.Valid(),
        "zero geometry owner generation rejects baseline");
    generationMismatch = carriers;
    generationMismatch.epoch.instanceUniverseGeneration = 0;
    Check(!generationMismatch.Valid(),
        "zero instance generation witness rejects baseline");
    generationMismatch = carriers;
    generationMismatch.epoch.geometryUniverseGeneration = 0;
    Check(!generationMismatch.Valid(),
        "zero geometry generation witness rejects baseline");
    carriers.priorSkinnedRecordsComplete = false;
    Check(!carriers.Valid(), "missing prior-skinned carrier rejects baseline");

    RtPathTraceCommittedBaselineCarrierCounts carrierCounts;
    carrierCounts.applyGateKeys = 2;
    carrierCounts.priorSkinnedRecords = 2;
    RtPathTraceCommittedPlanningBaseline measured = CarrierSeed();
    measured.applyGate.omittedSkinKeys.reserve(carrierCounts.applyGateKeys);
    measured.priorSkinnedRecords.reserve(carrierCounts.priorSkinnedRecords);
    FillSyntheticCarrierShapes(measured, carrierCounts);
    const std::size_t exactCarrierBytes =
        RtPathTraceCommittedBaselineCarrierBytes(measured);
    RtPathTraceCommittedPlanningBaseline transactionOutput;
    Check(RtPathTraceBuildCommittedBaselineCarrierTransaction(
            CarrierSeed(), carrierCounts, exactCarrierBytes,
            transactionOutput,
            [&](RtPathTraceCommittedPlanningBaseline& candidate,
                std::size_t&)
            {
                FillSyntheticCarrierShapes(candidate, carrierCounts);
                return true;
            }),
        "committed carrier transaction accepts exactly-at-cap");
    Check(RtPathTraceCommittedBaselineCarrierBytes(transactionOutput) ==
            exactCarrierBytes,
        "committed carrier transaction charges actual capacities");

    RtPathTraceCommittedPlanningBaseline preserved = CarrierSeed();
    preserved.epoch.committedViewToken = 999;
    Check(!RtPathTraceBuildCommittedBaselineCarrierTransaction(
            CarrierSeed(), carrierCounts, exactCarrierBytes - 1,
            preserved,
            [&](RtPathTraceCommittedPlanningBaseline& candidate,
                std::size_t&)
            {
                FillSyntheticCarrierShapes(candidate, carrierCounts);
                return true;
            }) && preserved.epoch.committedViewToken == 999,
        "over-cap rejects atomically and preserves prior output");

    RtPathTraceCommittedBaselineCarrierCounts inflatedCounts;
    inflatedCounts.applyGateKeys = 1;
    inflatedCounts.priorSkinnedRecords = 1;
    RtPathTraceCommittedPlanningBaseline nominal = CarrierSeed();
    nominal.applyGate.omittedSkinKeys.reserve(1);
    nominal.priorSkinnedRecords.reserve(1);
    FillSyntheticCarrierShapes(nominal, inflatedCounts);
    const std::size_t nominalBytes =
        RtPathTraceCommittedBaselineCarrierBytes(nominal);
    Check(!RtPathTraceBuildCommittedBaselineCarrierTransaction(
            CarrierSeed(), inflatedCounts, nominalBytes,
            preserved,
            [&](RtPathTraceCommittedPlanningBaseline& candidate,
                std::size_t&)
            {
                candidate.applyGate.omittedSkinKeys.reserve(32);
                candidate.priorSkinnedRecords.reserve(32);
                FillSyntheticCarrierShapes(candidate, inflatedCounts);
                return true;
            }) && preserved.epoch.committedViewToken == 999,
        "reserve inflation is charged and rejected atomically");

    Check(!RtPathTraceBuildCommittedBaselineCarrierTransaction(
            CarrierSeed(), inflatedCounts, nominalBytes,
            preserved,
            [&](RtPathTraceCommittedPlanningBaseline&, std::size_t&) -> bool
            {
                throw std::bad_alloc();
            }) && preserved.epoch.committedViewToken == 999,
        "allocation failure is contained and preserves prior output");

    RtPathTraceCommittedSemanticConfig copiedConfig;
    RtPathTracePrimaryViewDtoLineage sourceLineage =
        Dto(71, 701, 10, 14,
            PathTraceCommittedBaselineBarrierGeneration());
    Check(PublishPathTraceCommittedPlanningBaselinePhase1(
            std::move(transactionOutput)) &&
        CopyPathTraceCommittedSemanticConfigForSourcePhase1(
            sourceLineage, copiedConfig) &&
        copiedConfig.recordAllInstanceClasses &&
        copiedConfig.removeRoutedRigidDynamic &&
        copiedConfig.rigidRouteEmissiveCards &&
        copiedConfig.configFingerprint == 0x1234,
        "exact predecessor lineage transports committed semantic config");
    RtPathTraceCommittedSemanticConfig rejectedConfig;
    sourceLineage.frameIndex = 702;
    Check(!CopyPathTraceCommittedSemanticConfigForSourcePhase1(
            sourceLineage, rejectedConfig),
        "non-successor source lineage cannot read committed semantic config");
    DrainPathTraceCommittedBaselinePhase1(
        RtPathTraceCommittedBaselineDrainReason::Shutdown);

    if (failures != 0)
    {
        std::fprintf(stderr, "PathTraceCommittedBaselineHarness: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("PathTraceCommittedBaselineHarness: PASS\n");
    return 0;
}
