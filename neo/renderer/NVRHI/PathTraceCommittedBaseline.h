#pragma once

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "PathTraceCpuProducerApplyGate.h"
#include "PathTraceSemanticConfig.h"
#include "PathTraceUniversePlanningSnapshot.h"

constexpr std::size_t RT_PT_COMMITTED_BASELINE_MAP_NAME_CAPACITY = 256u;

struct RtPathTraceCommittedBaselineEpoch
{
    std::uint64_t committedViewToken = 0;
    std::uint64_t sealedPrimaryViewToken = 0;
    std::uint64_t baselineFrameIndex = 0;
    std::uint64_t ownerUniverseFrameIndex = 0;
    std::uint64_t worldLifecycleGeneration = 0;
    std::uint64_t mapLoadSerial = 0;
    std::uint64_t mapTimeStamp = 0;
    char mapName[RT_PT_COMMITTED_BASELINE_MAP_NAME_CAPACITY] = {};
    std::uint64_t barrierGeneration = 0;
    std::uint64_t materialRegistryGeneration = 0;
    std::uint64_t residentMaterialFactsGeneration = 0;
    std::uint64_t instanceUniverseGeneration = 0;
    std::uint64_t geometryUniverseGeneration = 0;
    std::uint64_t staticMaterialGeneration = 0;
    std::uint64_t canonicalSourceIndexPoolGeneration = 0;
    std::uint64_t staticResidentPayloadGeneration = 0;
    bool capturedAfterRootEndFrame = false;
    bool capturedAfterStaticPrune = false;
};

inline RtPathTracePlanningSnapshotEpoch
RtPathTraceMapCommittedBaselineToPlanningEpoch(
    const RtPathTraceCommittedBaselineEpoch& committed)
{
    RtPathTracePlanningSnapshotEpoch planning;
    planning.generation = committed.sealedPrimaryViewToken;
    planning.frameIndex = committed.ownerUniverseFrameIndex;
    planning.mapTimeStamp = committed.mapTimeStamp;
    planning.mapLoadSerial = committed.mapLoadSerial;
    RtPathTracePlanningCopyName(planning.mapName, sizeof(planning.mapName),
        committed.mapName);
    return planning;
}

struct RtPathTracePrimaryViewDtoLineage
{
    std::uint64_t sealedViewToken = 0;
    std::uint64_t predecessorCommittedViewToken = 0;
    std::uint64_t frameIndex = 0;
    std::uint64_t worldLifecycleGeneration = 0;
    std::uint64_t mapLoadSerial = 0;
    std::uint64_t mapTimeStamp = 0;
    char mapName[RT_PT_COMMITTED_BASELINE_MAP_NAME_CAPACITY] = {};
    std::uint64_t barrierGeneration = 0;
    std::uint64_t configFingerprint = 0;
    bool primaryView = false;
    bool complete = false;
};

inline bool RtPathTraceCommittedMapIdentityMatches(
    const RtPathTraceCommittedBaselineEpoch& baseline,
    const RtPathTracePrimaryViewDtoLineage& dto)
{
    if (baseline.worldLifecycleGeneration != dto.worldLifecycleGeneration ||
        baseline.mapLoadSerial != dto.mapLoadSerial ||
        baseline.mapTimeStamp != dto.mapTimeStamp)
    {
        return false;
    }
    for (std::size_t index = 0;
        index < RT_PT_COMMITTED_BASELINE_MAP_NAME_CAPACITY; ++index)
    {
        if (baseline.mapName[index] != dto.mapName[index])
        {
            return false;
        }
    }
    return true;
}

inline bool RtPathTraceCommittedBaselineEpochValid(
    const RtPathTraceCommittedBaselineEpoch& epoch)
{
    return epoch.committedViewToken != 0 &&
        epoch.worldLifecycleGeneration != 0 &&
        epoch.barrierGeneration != 0 &&
        epoch.capturedAfterRootEndFrame &&
        epoch.capturedAfterStaticPrune;
}

inline bool RtPathTraceCommittedHistoryEpochValid(
    const RtPathTraceCommittedBaselineEpoch& epoch,
    std::uint64_t sealedPrimaryViewToken,
    std::uint64_t ownerUniverseFrameIndex)
{
    return RtPathTraceCommittedBaselineEpochValid(epoch) &&
        epoch.sealedPrimaryViewToken == sealedPrimaryViewToken &&
        epoch.ownerUniverseFrameIndex == ownerUniverseFrameIndex;
}

template<typename Snapshot>
inline bool RtPathTraceCommittedSnapshotEpochMatches(
    const Snapshot& snapshot,
    const RtPathTraceCommittedBaselineEpoch& committedEpoch,
    std::uint64_t expectedOwnerGeneration)
{
    const RtPathTracePlanningSnapshotEpoch expected =
        RtPathTraceMapCommittedBaselineToPlanningEpoch(committedEpoch);
    return snapshot.complete && expectedOwnerGeneration != 0 &&
        snapshot.ownerGeneration == expectedOwnerGeneration &&
        snapshot.epoch.generation == expected.generation &&
        snapshot.epoch.frameIndex == expected.frameIndex &&
        snapshot.epoch.mapTimeStamp == expected.mapTimeStamp &&
        snapshot.epoch.mapLoadSerial == expected.mapLoadSerial &&
        std::memcmp(snapshot.epoch.mapName, expected.mapName,
            sizeof(expected.mapName)) == 0 &&
        !snapshot.epoch.capturedAfterBeginFrame &&
        !snapshot.epoch.capturedAfterStaticPreload &&
        !snapshot.epoch.capturedBeforeSerialMutate;
}

inline RtPathTraceInstanceHistoryApplication ApplyInstanceHistoryFromPod(
    const RtPathTraceInstanceUniverseSnapshot& snapshot,
    const RtPathTraceCommittedBaselineEpoch& committedEpoch,
    std::uint64_t instanceId,
    const float currentObjectToWorld[16],
    std::uint64_t frameIndex)
{
    return ApplyInstanceHistoryRowsValidatedFromPod(
        RtPathTraceCommittedSnapshotEpochMatches(snapshot, committedEpoch,
            committedEpoch.instanceUniverseGeneration) &&
            RtPathTraceCommittedHistoryEpochValid(
            committedEpoch, snapshot.epoch.generation, frameIndex),
        snapshot.histories.data(), snapshot.histories.size(), instanceId,
        currentObjectToWorld, frameIndex);
}

inline bool RtPathTraceCommittedStaticQueryEpochValid(
    const RtPathTraceCommittedBaselineEpoch& epoch,
    std::uint64_t ownerUniverseFrameIndex)
{
    return RtPathTraceCommittedBaselineEpochValid(epoch) &&
        epoch.ownerUniverseFrameIndex == ownerUniverseFrameIndex &&
        epoch.geometryUniverseGeneration != 0;
}

inline bool HasStaticSurfaceFromCommittedBaselinePod(
    const RtSmokeGeometryUniverseSnapshot& snapshot,
    const RtPathTraceCommittedBaselineEpoch& committedEpoch,
    std::uint64_t key)
{
    if (!RtPathTraceCommittedSnapshotEpochMatches(snapshot, committedEpoch,
            committedEpoch.geometryUniverseGeneration) ||
        !RtPathTraceCommittedStaticQueryEpochValid(
            committedEpoch, committedEpoch.ownerUniverseFrameIndex))
    {
        return false;
    }
    const auto first = std::lower_bound(snapshot.staticSurfaces.begin(),
        snapshot.staticSurfaces.end(), key,
        [](const RtSmokeStaticSurfacePod& row, std::uint64_t value)
        {
            return row.key < value;
        });
    if (first == snapshot.staticSurfaces.end() || first->key != key ||
        !first->valid)
    {
        return false;
    }
    return std::next(first) == snapshot.staticSurfaces.end() ||
        std::next(first)->key != key;
}

inline bool RtPathTraceCommittedRigidQueryEpochValid(
    const RtPathTraceCommittedBaselineEpoch& epoch,
    std::uint64_t ownerUniverseFrameIndex)
{
    return RtPathTraceCommittedStaticQueryEpochValid(
        epoch, ownerUniverseFrameIndex) &&
        epoch.staticMaterialGeneration != 0;
}

struct RtPathTraceCommittedSkinnedRecordPod
{
    std::uint64_t worldGeneration = 0;
    std::uint32_t renderDefIndex = UINT32_MAX;
    std::uint32_t renderDefGeneration = 0;
    std::uint32_t subInstanceKind = 0;
    std::uint32_t modelSurfaceIndex = UINT32_MAX;
    std::int32_t jointSubmeshIndex = -1;
    std::uint64_t jointCacheHandle = 0;
    std::uintptr_t jointSource = 0;
    std::int32_t jointCount = 0;
    std::int32_t vertexCount = 0;
    std::int32_t indexCount = 0;
    std::int32_t triangleCount = 0;
    bool rtCpuSkinned = false;
};
static_assert(std::is_trivially_copyable<
    RtPathTraceCommittedSkinnedRecordPod>::value,
    "committed skinned baseline rows must remain pointer-free POD");

inline bool RtPathTraceCommittedBaselinePairs(
    const RtPathTraceCommittedBaselineEpoch& baseline,
    const RtPathTracePrimaryViewDtoLineage& dto)
{
    return RtPathTraceCommittedBaselineEpochValid(baseline) &&
        dto.primaryView && dto.complete && dto.sealedViewToken != 0 &&
        dto.configFingerprint != 0 &&
        dto.predecessorCommittedViewToken == baseline.committedViewToken &&
        dto.frameIndex == baseline.baselineFrameIndex + 1 &&
        dto.barrierGeneration == baseline.barrierGeneration &&
        RtPathTraceCommittedMapIdentityMatches(baseline, dto);
}

enum class RtPathTraceCommittedBaselineDrainReason : std::uint32_t
{
    None = 0,
    MapReset,
    WorldReplacement,
    VidRestart,
    Shutdown,
    TopologyBarrier
};

struct RtPathTraceCommittedBaselineDispatchGuard
{
    bool baselineSkinnedSplitGate = false;
    bool baselineAdmissionRoutesPresent = false;
    bool captureAllocationFailureArmed = false;
    bool producerAllocationFailureArmed = false;
};

struct RtPathTraceCommittedPlanningBaseline
{
    RtPathTraceCommittedBaselineEpoch epoch;
    RtPathTraceCommittedBaselineDispatchGuard dispatchGuard;
    RtPathTraceCommittedSemanticConfig semanticConfig;
    RtPathTraceInstanceUniverseSnapshot instanceUniverse;
    RtSmokeGeometryUniverseSnapshot geometryUniverse;
    RtPtCpuProducerApplyGateSnapshot applyGate;
    std::vector<RtPathTraceCommittedSkinnedRecordPod> priorSkinnedRecords;
    bool applyGateComplete = false;
    bool priorSkinnedRecordsComplete = false;
    bool complete = false;

    bool Valid() const
    {
        return complete && RtPathTraceCommittedBaselineEpochValid(epoch) &&
            instanceUniverse.complete && geometryUniverse.complete &&
            epoch.instanceUniverseGeneration != 0 &&
            instanceUniverse.ownerGeneration ==
                epoch.instanceUniverseGeneration &&
            epoch.geometryUniverseGeneration != 0 &&
            geometryUniverse.ownerGeneration ==
                epoch.geometryUniverseGeneration &&
            semanticConfig.configComplete &&
            semanticConfig.configFingerprint != 0 &&
            applyGateComplete && priorSkinnedRecordsComplete;
    }
};

constexpr std::size_t RT_PT_COMMITTED_BASELINE_MAX_BYTES =
    RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES;

struct RtPathTraceCommittedBaselineCarrierCounts
{
    std::size_t applyGateKeys = 0;
    std::size_t priorSkinnedRecords = 0;
};

inline std::size_t RtPathTraceCommittedBaselineCarrierBytes(
    const RtPathTraceCommittedPlanningBaseline& baseline)
{
    return sizeof(baseline.semanticConfig) + baseline.applyGate.OwnedBytes() +
        sizeof(baseline.priorSkinnedRecords) +
        baseline.priorSkinnedRecords.capacity() *
            sizeof(RtPathTraceCommittedSkinnedRecordPod) +
        baseline.instanceUniverse.OwnedBytes() +
        baseline.geometryUniverse.OwnedBytes();
}

inline bool RtPathTraceCommittedBaselineCarrierShapeValid(
    const RtPathTraceCommittedPlanningBaseline& baseline,
    const RtPathTraceCommittedBaselineCarrierCounts& counts)
{
    const RtPathTracePlanningSnapshotEpoch expected =
        RtPathTraceMapCommittedBaselineToPlanningEpoch(baseline.epoch);
    const auto epochMatches = [&expected](
        const RtPathTracePlanningSnapshotEpoch& actual)
    {
        return actual.generation == expected.generation &&
            actual.frameIndex == expected.frameIndex &&
            actual.mapTimeStamp == expected.mapTimeStamp &&
            actual.mapLoadSerial == expected.mapLoadSerial &&
            std::memcmp(actual.mapName, expected.mapName,
                sizeof(actual.mapName)) == 0 &&
            !actual.capturedAfterBeginFrame &&
            !actual.capturedAfterStaticPreload &&
            !actual.capturedBeforeSerialMutate;
    };
    return baseline.semanticConfig.configComplete &&
        baseline.semanticConfig.configFingerprint != 0 &&
        baseline.applyGateComplete &&
        baseline.priorSkinnedRecordsComplete &&
        baseline.applyGate.omittedSkinKeys.size() == counts.applyGateKeys &&
        baseline.priorSkinnedRecords.size() == counts.priorSkinnedRecords &&
        baseline.instanceUniverse.complete && baseline.geometryUniverse.complete &&
        baseline.epoch.instanceUniverseGeneration != 0 &&
        baseline.instanceUniverse.ownerGeneration ==
            baseline.epoch.instanceUniverseGeneration &&
        baseline.epoch.geometryUniverseGeneration != 0 &&
        baseline.geometryUniverse.ownerGeneration ==
            baseline.epoch.geometryUniverseGeneration &&
        epochMatches(baseline.instanceUniverse.epoch) &&
        epochMatches(baseline.geometryUniverse.epoch);
}

template<typename Builder>
inline bool RtPathTraceBuildCommittedBaselineCarrierTransaction(
    RtPathTraceCommittedPlanningBaseline seed,
    const RtPathTraceCommittedBaselineCarrierCounts& counts,
    std::size_t maxBytes,
    RtPathTraceCommittedPlanningBaseline& output,
    Builder&& builder)
{
    bool built = false;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try
    {
#endif
        seed.applyGate.omittedSkinKeys.reserve(counts.applyGateKeys);
        seed.priorSkinnedRecords.reserve(counts.priorSkinnedRecords);
        std::size_t snapshotBytes = sizeof(seed.semanticConfig) +
            seed.applyGate.OwnedBytes() +
            sizeof(seed.priorSkinnedRecords) +
            seed.priorSkinnedRecords.capacity() *
                sizeof(RtPathTraceCommittedSkinnedRecordPod);
        built = snapshotBytes <= maxBytes && builder(seed, snapshotBytes);
        const std::size_t actualBytes =
            RtPathTraceCommittedBaselineCarrierBytes(seed);
        built = built &&
            RtPathTraceCommittedBaselineCarrierShapeValid(seed, counts) &&
            actualBytes <= maxBytes;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    }
    catch (const std::bad_alloc&)
    {
        built = false;
    }
    catch (const std::length_error&)
    {
        built = false;
    }
#endif
    if (!built)
    {
        return false;
    }
    seed.complete = true;
    output = std::move(seed);
    return true;
}

inline bool RtPathTraceCommittedBaselineMayDispatch(
    const RtPathTraceCommittedBaselineDispatchGuard& guard)
{
    return !guard.baselineSkinnedSplitGate &&
        !guard.baselineAdmissionRoutesPresent &&
        !guard.captureAllocationFailureArmed &&
        !guard.producerAllocationFailureArmed;
}

class RtPathTraceCommittedBaselineRendezvous
{
public:
    void PublishBaseline(const RtPathTraceCommittedBaselineEpoch& baseline)
    {
        if (!RtPathTraceCommittedBaselineEpochValid(baseline) ||
            baseline.barrierGeneration != barrierGeneration)
        {
            return;
        }
        if (baseline.baselineFrameIndex <= publishedBaselineFrameHighWater ||
            baseline.committedViewToken <= publishedBaselineTokenHighWater ||
            baseline.baselineFrameIndex <= consumedBaselineFrameHighWater ||
            baseline.committedViewToken <= consumedBaselineTokenHighWater)
        {
            return;
        }
        if (!baselineReady ||
            baseline.baselineFrameIndex > pendingBaseline.baselineFrameIndex)
        {
            pendingBaseline = baseline;
            baselineReady = true;
            publishedBaselineFrameHighWater = baseline.baselineFrameIndex;
            publishedBaselineTokenHighWater = baseline.committedViewToken;
            ++baselinePublicationSerial;
        }
    }

    void PublishPrimaryDto(const RtPathTracePrimaryViewDtoLineage& dto)
    {
        if (!dto.primaryView || !dto.complete || dto.sealedViewToken == 0 ||
            dto.configFingerprint == 0 ||
            dto.barrierGeneration != barrierGeneration)
        {
            return;
        }
        if (dto.frameIndex <= publishedDtoFrameHighWater ||
            dto.sealedViewToken <= publishedDtoTokenHighWater ||
            dto.frameIndex <= consumedDtoFrameHighWater ||
            dto.sealedViewToken <= consumedDtoTokenHighWater)
        {
            return;
        }
        if (!dtoReady || dto.frameIndex > pendingDto.frameIndex)
        {
            pendingDto = dto;
            dtoReady = true;
            publishedDtoFrameHighWater = dto.frameIndex;
            publishedDtoTokenHighWater = dto.sealedViewToken;
            ++dtoPublicationSerial;
        }
    }

    bool TryAcquirePair(RtPathTraceCommittedBaselineEpoch& baseline,
        RtPathTracePrimaryViewDtoLineage& dto)
    {
        if (!baselineReady || !dtoReady ||
            !RtPathTraceCommittedBaselinePairs(pendingBaseline, pendingDto))
        {
            return false;
        }
        baseline = pendingBaseline;
        dto = pendingDto;
        consumedBaselineFrameHighWater = pendingBaseline.baselineFrameIndex;
        consumedBaselineTokenHighWater = pendingBaseline.committedViewToken;
        consumedDtoFrameHighWater = pendingDto.frameIndex;
        consumedDtoTokenHighWater = pendingDto.sealedViewToken;
        baselineReady = false;
        dtoReady = false;
        ++pairSerial;
        return true;
    }

    void Drain(RtPathTraceCommittedBaselineDrainReason reason) noexcept
    {
        pendingBaseline = RtPathTraceCommittedBaselineEpoch{};
        pendingDto = RtPathTracePrimaryViewDtoLineage{};
        baselineReady = false;
        dtoReady = false;
        lastDrainReason = reason;
        publishedBaselineFrameHighWater = 0;
        publishedBaselineTokenHighWater = 0;
        publishedDtoFrameHighWater = 0;
        publishedDtoTokenHighWater = 0;
        consumedBaselineFrameHighWater = 0;
        consumedBaselineTokenHighWater = 0;
        consumedDtoFrameHighWater = 0;
        consumedDtoTokenHighWater = 0;
        ++barrierGeneration;
        if (barrierGeneration == 0)
        {
            barrierGeneration = 1;
        }
    }

    std::uint64_t BarrierGeneration() const noexcept { return barrierGeneration; }
    std::uint64_t PairSerial() const noexcept { return pairSerial; }
    std::uint64_t BaselinePublicationSerial() const noexcept { return baselinePublicationSerial; }
    std::uint64_t DtoPublicationSerial() const noexcept { return dtoPublicationSerial; }
    RtPathTraceCommittedBaselineDrainReason LastDrainReason() const noexcept { return lastDrainReason; }

private:
    RtPathTraceCommittedBaselineEpoch pendingBaseline;
    RtPathTracePrimaryViewDtoLineage pendingDto;
    std::uint64_t barrierGeneration = 1;
    std::uint64_t baselinePublicationSerial = 0;
    std::uint64_t pairSerial = 0;
    std::uint64_t dtoPublicationSerial = 0;
    std::uint64_t publishedBaselineFrameHighWater = 0;
    std::uint64_t publishedBaselineTokenHighWater = 0;
    std::uint64_t publishedDtoFrameHighWater = 0;
    std::uint64_t publishedDtoTokenHighWater = 0;
    std::uint64_t consumedBaselineFrameHighWater = 0;
    std::uint64_t consumedBaselineTokenHighWater = 0;
    std::uint64_t consumedDtoFrameHighWater = 0;
    std::uint64_t consumedDtoTokenHighWater = 0;
    RtPathTraceCommittedBaselineDrainReason lastDrainReason =
        RtPathTraceCommittedBaselineDrainReason::None;
    bool baselineReady = false;
    bool dtoReady = false;
};

std::uint64_t NextPathTraceSealedPrimaryViewToken(
    std::uint64_t* predecessorToken = nullptr) noexcept;
std::uint64_t NextPathTraceRenderWorldLifecycleGeneration() noexcept;

// Phase 1 publishes authority and transport only.  There is deliberately no
// shipping acquire/consume entry point until the phase-3 acceptance path exists.
std::uint64_t PathTraceCommittedBaselineBarrierGeneration() noexcept;
void PublishPathTracePrimaryViewDtoLineagePhase1(
    const RtPathTracePrimaryViewDtoLineage& dto) noexcept;
bool PublishPathTraceCommittedPlanningBaselinePhase1(
    RtPathTraceCommittedPlanningBaseline&& baseline) noexcept;
void DrainPathTraceCommittedBaselinePhase1(
    RtPathTraceCommittedBaselineDrainReason reason) noexcept;
bool CopyPathTraceCommittedSemanticConfigForSourcePhase1(
    const RtPathTracePrimaryViewDtoLineage& sourceLineage,
    RtPathTraceCommittedSemanticConfig& output) noexcept;
