#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <utility>

constexpr std::uint32_t RT_PT_PRODUCER_LANE_COUNT = 3;
constexpr std::uint32_t RT_PT_PRODUCER_LANE_A_SLOT_COUNT = 3;

enum class RtPathTraceOwnerFrameFailurePoint : std::uint8_t
{
    None,
    AfterGeometry,
    AfterFinalize
};

// Shared transaction control used by the production owner-frame staging path
// and its dependency-light executable tests.  The callbacks may mutate only
// the owned candidate; live state is reached later through one noexcept commit.
template <typename Candidate, typename GeometryFn, typename FinalizeFn>
bool BuildPathTraceOwnerFrameStagedTransaction(
    Candidate& candidate,
    GeometryFn&& applyGeometry,
    FinalizeFn&& finalizeOwner,
    RtPathTraceOwnerFrameFailurePoint failurePoint =
        RtPathTraceOwnerFrameFailurePoint::None)
{
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try
    {
#endif
        if (!applyGeometry(candidate))
        {
            return false;
        }
        if (failurePoint == RtPathTraceOwnerFrameFailurePoint::AfterGeometry)
        {
            return false;
        }
        if (!finalizeOwner(candidate))
        {
            return false;
        }
        return failurePoint != RtPathTraceOwnerFrameFailurePoint::AfterFinalize;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
    catch (const std::length_error&)
    {
        return false;
    }
#endif
}

enum class RtPathTraceCaptureFirstFailure : std::uint8_t
{
    None,
    Incomplete,
    Epoch,
    ViewIdentity,
    Vertices,
    Indexes,
    TriangleClasses,
    TriangleMaterials,
    TriangleInstances,
    TriangleIdentities,
    MaterialInfoIntents,
    MaterialVariants,
    InstanceObservations,
    RigidCandidates,
    StaticMembership,
    RoutedReadySkip,
    SurfaceCount,
    SurfaceDecision,
    Count
};

constexpr std::size_t RT_PT_CAPTURE_FIRST_FAILURE_COUNT =
    static_cast<std::size_t>(RtPathTraceCaptureFirstFailure::Count);

struct RtPathTraceCaptureLiveCardinalityAttribution
{
    std::uint64_t skinnedOmittedSurfaces = 0;
    std::uint64_t skinnedOmittedVertices = 0;
    std::uint64_t skinnedOmittedIndexes = 0;
    std::uint64_t rigidRemovedSurfaces = 0;
    std::uint64_t rigidRemovedVertices = 0;
    std::uint64_t rigidRemovedIndexes = 0;
};

struct RtPathTraceCaptureCardinalityAttribution
{
    RtPathTraceCaptureLiveCardinalityAttribution live;
    std::uint64_t productAcceptedSurfaces = 0;
    std::uint64_t productAcceptedVertices = 0;
    std::uint64_t oracleAcceptedSurfaces = 0;
    std::uint64_t oracleAcceptedVertices = 0;
    std::int64_t afterSkinnedVertexDelta = 0;
    std::int64_t unattributedVertexDelta = 0;
};

struct RtPathTraceCaptureAcceptedSurfaceScalar
{
    bool present = false;
    std::uint32_t terminal = 0;
    std::uint32_t surfaceClass = 0;
    std::uint32_t sourceFlags = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
    bool rigidReadyByMesh = false;
    bool rigidReadyByResident = false;
    bool skinnedCaptureOmitted = false;
    std::uint32_t skinnedAdmission = 0;
};
constexpr std::uint32_t RT_PT_CAPTURE_TERMINAL_ACCEPTED_SCALAR = 7u;

struct RtPathTraceCaptureAcceptedSetDiff
{
    bool available = false;
    bool found = false;
    std::uint32_t ordinal = UINT32_MAX;
    RtPathTraceCaptureAcceptedSurfaceScalar product;
    RtPathTraceCaptureAcceptedSurfaceScalar oracle;
};

struct RtPathTraceCaptureInstanceObservationDiff
{
    bool found = false;
    std::uint32_t ordinal = UINT32_MAX;
    bool productHasPrevious = false;
    bool oracleHasPrevious = false;
    bool productTransformContinuous = false;
    bool oracleTransformContinuous = false;
};

inline std::int64_t RtPathTraceCaptureSignedVertexDelta(
    std::uint64_t productVertices,
    std::uint64_t oracleVertices,
    std::uint64_t omittedVertices)
{
    const std::uint64_t accounted = oracleVertices > UINT64_MAX - omittedVertices
        ? UINT64_MAX : oracleVertices + omittedVertices;
    if (productVertices >= accounted)
    {
        const std::uint64_t delta = productVertices - accounted;
        return delta > static_cast<std::uint64_t>(INT64_MAX)
            ? INT64_MAX : static_cast<std::int64_t>(delta);
    }
    const std::uint64_t delta = accounted - productVertices;
    return delta > static_cast<std::uint64_t>(INT64_MAX)
        ? INT64_MIN : -static_cast<std::int64_t>(delta);
}

inline RtPathTraceCaptureCardinalityAttribution
RtPathTraceBuildCaptureCardinalityAttribution(
    std::uint64_t productAcceptedSurfaces,
    std::uint64_t productAcceptedVertices,
    std::uint64_t oracleAcceptedSurfaces,
    std::uint64_t oracleAcceptedVertices,
    const RtPathTraceCaptureLiveCardinalityAttribution& live)
{
    RtPathTraceCaptureCardinalityAttribution result;
    result.live = live;
    result.productAcceptedSurfaces = productAcceptedSurfaces;
    result.productAcceptedVertices = productAcceptedVertices;
    result.oracleAcceptedSurfaces = oracleAcceptedSurfaces;
    result.oracleAcceptedVertices = oracleAcceptedVertices;
    result.afterSkinnedVertexDelta = RtPathTraceCaptureSignedVertexDelta(
        productAcceptedVertices, oracleAcceptedVertices,
        live.skinnedOmittedVertices);
    const std::uint64_t omittedAndRigid =
        live.skinnedOmittedVertices > UINT64_MAX - live.rigidRemovedVertices
            ? UINT64_MAX
            : live.skinnedOmittedVertices + live.rigidRemovedVertices;
    result.unattributedVertexDelta = RtPathTraceCaptureSignedVertexDelta(
        productAcceptedVertices, oracleAcceptedVertices, omittedAndRigid);
    return result;
}

enum class RtPathTraceProducerSlotState : std::uint8_t
{
    Free,
    Queued,
    Running,
    Ready,
    Consuming
};

struct RtPathTraceProducerSlotContract
{
    std::uint64_t generation = 0;
    RtPathTraceProducerSlotState state = RtPathTraceProducerSlotState::Free;
    bool oracleReady = false;
};

struct RtPathTraceProducerLaneBOwnershipDecision
{
    bool active = false;
    bool entering = false;
    bool leaving = false;
    bool legacyWorkersMayStart = true;
};

inline RtPathTraceProducerLaneBOwnershipDecision
RtPathTraceResolveProducerLaneBOwnership(
    bool previouslyActive, bool effectiveLaneBActive)
{
    RtPathTraceProducerLaneBOwnershipDecision result;
    result.active = effectiveLaneBActive;
    result.entering = !previouslyActive && effectiveLaneBActive;
    result.leaving = previouslyActive && !effectiveLaneBActive;
    result.legacyWorkersMayStart = !effectiveLaneBActive;
    return result;
}

inline bool RtPathTraceProducerModeImplemented(int mode)
{
    return mode >= 0 && mode <= 2;
}

inline bool RtPathTraceProducerConfigurationChanged(
    int configuredMode,
    int configuredMask,
    int requestedMode,
    int requestedMask)
{
    return configuredMode != requestedMode || configuredMask != requestedMask;
}

inline bool RtPathTraceProducerLaneBShouldArmBootstrap(
    bool laneBActive,
    int effectiveMode,
    bool loadBearingHit,
    bool completeProduct)
{
    return laneBActive && effectiveMode == 2 &&
        loadBearingHit && completeProduct;
}

inline bool RtPathTraceProducerMayDispatch(
    const RtPathTraceProducerSlotContract& slot,
    std::uint64_t generation)
{
    return generation != 0 && slot.state == RtPathTraceProducerSlotState::Free &&
        slot.generation == 0 && !slot.oracleReady;
}

inline bool RtPathTraceProducerGenerationMatches(
    const RtPathTraceProducerSlotContract& slot,
    std::uint64_t generation)
{
    return generation != 0 && slot.generation == generation;
}

inline bool RtPathTraceProducerReadyToCompare(
    const RtPathTraceProducerSlotContract& slot)
{
    return slot.state == RtPathTraceProducerSlotState::Ready && slot.oracleReady;
}

inline bool RtPathTraceProducerSlotBytesFit(
    std::size_t snapshotBytes,
    std::size_t productBytes,
    std::size_t cap)
{
    return snapshotBytes <= cap && productBytes <= cap - snapshotBytes;
}

inline bool RtPathTraceProducerCheckedAddBytes(
    std::size_t& total, std::size_t bytes, std::size_t cap)
{
    if (total > cap || bytes > cap - total)
    {
        return false;
    }
    total += bytes;
    return true;
}

inline bool RtPathTraceProducerCheckedArrayBytes(
    std::size_t count,
    std::size_t elementBytes,
    std::size_t& total,
    std::size_t cap)
{
    if (elementBytes != 0 && count > cap / elementBytes)
    {
        return false;
    }
    return RtPathTraceProducerCheckedAddBytes(
        total, count * elementBytes, cap);
}

inline bool RtPathTraceReconcileReservedCapacity(
    std::size_t fixedAndOtherBytes,
    std::size_t actualReservedBytes,
    std::size_t remainingPlannedBytes,
    std::size_t cap)
{
    std::size_t total = fixedAndOtherBytes;
    return RtPathTraceProducerCheckedAddBytes(total, actualReservedBytes, cap) &&
        RtPathTraceProducerCheckedAddBytes(total, remainingPlannedBytes, cap);
}

inline bool RtPathTraceProducerPartitionSelects(
    int requestedBucket, int surfaceBucket, bool accepted)
{
    return accepted && requestedBucket == surfaceBucket;
}

struct RtPathTraceOwnerDecisionHandoffState
{
    bool harvested = false;
    bool decisionsAttached = false;
};

inline bool RtPathTraceOwnerDecisionHandoffCanDispatch(
    const RtPathTraceOwnerDecisionHandoffState& state)
{
    return state.harvested && state.decisionsAttached;
}

inline bool RtPathTraceOwnerDecisionHandoffUsesHarvestFallback(
    const RtPathTraceOwnerDecisionHandoffState& state)
{
    return state.harvested;
}

inline bool RtPathTraceOwnerDecisionHandoffUsesFullCapture(
    const RtPathTraceOwnerDecisionHandoffState& state)
{
    return !state.harvested;
}

inline bool RtPathTraceProducerRebaseIndex(
    std::uint32_t baseVertex,
    std::uint32_t sourceIndex,
    std::uint32_t sourceVertexOffset,
    std::uint32_t& destinationIndex)
{
    if (sourceIndex < sourceVertexOffset)
    {
        return false;
    }
    const std::uint32_t localIndex = sourceIndex - sourceVertexOffset;
    if (localIndex > UINT32_MAX - baseVertex)
    {
        return false;
    }
    destinationIndex = baseVertex + localIndex;
    return true;
}

struct RtPathTraceCompleteSlotCardinality
{
    std::size_t surfaces = 0;
    std::size_t stages = 0;
    std::size_t runtimeStages = 0;
    std::size_t registers = 0;
    std::size_t vertices = 0;
    std::size_t indexes = 0;
    std::size_t joints = 0;
    std::size_t variantBases = 0;
    std::size_t registryMaterials = 0;
    std::size_t modelTables = 0;
    std::size_t modelSurfaceTokens = 0;
    std::size_t applyGateKeys = 0;
    std::size_t instanceMeshes = 0;
    std::size_t instanceHistories = 0;
    std::size_t geometryStatic = 0;
    std::size_t geometryRoutes = 0;
    std::size_t geometryResidents = 0;
    std::size_t receiptSurfaces = 0;
    std::size_t materialInfoIntents = 0;
    std::size_t materialVariants = 0;
    std::size_t instanceObservations = 0;
    std::size_t rigidCandidates = 0;
    std::size_t preparedRigidPayloads = 0;
    std::size_t preparedRigidVertices = 0;
    std::size_t preparedRigidIndexes = 0;
    std::size_t frameMaterialIds = 0;
};

struct RtPathTraceCompleteSlotLayout
{
    std::size_t snapshotFixed = 0;
    std::size_t rawSurface = 0;
    std::size_t ownerDecision = 0;
    std::size_t stage = 0;
    std::size_t runtimeStage = 0;
    std::size_t registerValue = 0;
    std::size_t rawVertex = 0;
    std::size_t rawIndex = 0;
    std::size_t joint = 0;
    std::size_t variantBase = 0;
    std::size_t registryMaterial = 0;
    std::size_t modelTable = 0;
    std::size_t modelSurfaceToken = 0;
    std::size_t applyGateKey = 0;
    std::size_t instanceMesh = 0;
    std::size_t instanceHistory = 0;
    std::size_t geometryStatic = 0;
    std::size_t geometryRoute = 0;
    std::size_t geometryResident = 0;
    std::size_t productFixed = 0;
    std::size_t productSurface = 0;
    std::size_t productVertex = 0;
    std::size_t productIndex = 0;
    std::size_t triangleClass = 0;
    std::size_t triangleMaterial = 0;
    std::size_t triangleInstance = 0;
    std::size_t triangleIdentity = 0;
    std::size_t materialIntent = 0;
    std::size_t materialVariant = 0;
    std::size_t instanceObservation = 0;
    std::size_t rigidCandidate = 0;
    std::size_t preparedRigidPayload = 0;
    std::size_t preparedRigidVertex = 0;
    std::size_t preparedRigidIndex = 0;
    std::size_t frameMaterialId = 0;
    std::size_t membershipOrdinal = 0;
    std::size_t receiptSurface = 0;
    std::size_t oracleFixed = 0;
    std::size_t oracleSurface = 0;
};

struct RtPathTraceCompleteSlotPlan
{
    std::size_t snapshotBytes = 0;
    std::size_t candidateBytes = 0;
    std::size_t finalProductBytes = 0;
    std::size_t oracleBytes = 0;
    std::size_t peakBytes = 0;
};

inline bool RtPathTracePlanCompleteSlot(
    const RtPathTraceCompleteSlotCardinality& count,
    const RtPathTraceCompleteSlotLayout& size,
    std::size_t cap,
    RtPathTraceCompleteSlotPlan& plan)
{
    plan = {};
    std::size_t total = 0;
    auto array = [&](std::size_t n, std::size_t element)
    {
        return RtPathTraceProducerCheckedArrayBytes(n, element, total, cap);
    };
    if (!RtPathTraceProducerCheckedAddBytes(total, size.snapshotFixed, cap) ||
        !array(count.surfaces, size.rawSurface) ||
        !array(count.surfaces, size.ownerDecision) ||
        !array(count.stages, size.stage) ||
        !array(count.runtimeStages, size.runtimeStage) ||
        !array(count.registers, size.registerValue) ||
        !array(count.vertices, size.rawVertex) ||
        !array(count.indexes, size.rawIndex) ||
        !array(count.joints, size.joint) ||
        !array(count.variantBases, size.variantBase) ||
        !array(count.registryMaterials, size.registryMaterial) ||
        !array(count.modelTables, size.modelTable) ||
        !array(count.modelSurfaceTokens, size.modelSurfaceToken) ||
        !array(count.applyGateKeys, size.applyGateKey) ||
        !array(count.instanceMeshes, size.instanceMesh) ||
        !array(count.instanceHistories, size.instanceHistory) ||
        !array(count.geometryStatic, size.geometryStatic) ||
        !array(count.geometryRoutes, size.geometryRoute) ||
        !array(count.geometryResidents, size.geometryResident))
    {
        return false;
    }
    plan.snapshotBytes = total;
    const std::size_t triangles = count.indexes / 3;
    auto product = [&]()
    {
        return RtPathTraceProducerCheckedAddBytes(total, size.productFixed, cap) &&
            array(count.surfaces, size.productSurface) &&
            array(count.vertices, size.productVertex) &&
            array(count.indexes, size.productIndex) &&
            array(triangles, size.triangleClass) &&
            array(triangles, size.triangleMaterial) &&
            array(triangles, size.triangleInstance) &&
            array(triangles, size.triangleIdentity) &&
            array(count.materialInfoIntents, size.materialIntent) &&
            array(count.materialVariants, size.materialVariant) &&
            array(count.instanceObservations, size.instanceObservation) &&
            array(count.rigidCandidates, size.rigidCandidate) &&
            array(count.preparedRigidPayloads, size.preparedRigidPayload) &&
            array(count.preparedRigidVertices, size.preparedRigidVertex) &&
            array(count.preparedRigidIndexes, size.preparedRigidIndex) &&
            array(count.frameMaterialIds, size.frameMaterialId) &&
            array(count.receiptSurfaces, size.receiptSurface) &&
            array(count.surfaces, size.membershipOrdinal) &&
            array(count.surfaces, size.membershipOrdinal);
    };
    const std::size_t candidateStart = total;
    if (!product()) return false;
    plan.candidateBytes = total - candidateStart;
    // The local candidate is the final product publication.  S2 deliberately
    // removes the former second full partitioned product from the slot peak.
    plan.finalProductBytes = 0;
    const std::size_t oracleStart = total;
    if (!RtPathTraceProducerCheckedAddBytes(total, size.oracleFixed, cap) ||
        !array(count.vertices, size.productVertex) ||
        !array(count.indexes, size.productIndex) ||
        !array(triangles, size.triangleClass) ||
        !array(triangles, size.triangleMaterial) ||
        !array(triangles, size.triangleInstance) ||
        !array(triangles, size.triangleIdentity) ||
        !array(count.surfaces, size.materialIntent) ||
        !array(count.surfaces, size.materialVariant) ||
        !array(count.surfaces, size.instanceObservation) ||
        !array(count.surfaces, size.rigidCandidate) ||
        !array(count.surfaces, size.membershipOrdinal) ||
        !array(count.surfaces, size.membershipOrdinal) ||
        !array(count.surfaces, size.oracleSurface))
    {
        return false;
    }
    plan.oracleBytes = total - oracleStart;
    plan.peakBytes = total;
    return true;
}
