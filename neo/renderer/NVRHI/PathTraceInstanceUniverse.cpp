#include "precompiled.h"
#pragma hdrstop

#include "PathTraceInstanceUniverse.h"
#if !defined(RT_PT_INSTANCE_UNIVERSE_HARNESS)
#include "PathTraceCVars.h"
#include "PathTraceCommittedBaseline.h"
#include "../RenderCommon.h"
#endif

#include <algorithm>
#include <exception>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace {

uint64 PtNextObservationAuthoritySerial(uint64 value) noexcept
{
    if (value == std::numeric_limits<uint64>::max())
    {
        std::terminate();
    }
    return value + 1;
}

void PtMaybeFailObservationReplay(
    RtPathTraceInstanceUniverse::ObservationReplayAllocationTestSeam* seam,
    RtPathTraceInstanceUniverse::ObservationReplayAllocationPhase phase)
{
    if (!seam)
    {
        return;
    }
    ++seam->phaseCalls;
    if (!seam->armed || seam->failAt != phase)
    {
        return;
    }
    if (seam->throwLengthError)
    {
        throw std::length_error("instance observation replay test seam");
    }
    throw std::bad_alloc();
}

bool PtInstanceMatricesMatch(const float lhs[16], const float rhs[16])
{
    for (int element = 0; element < 16; ++element)
    {
        if (idMath::Fabs(lhs[element] - rhs[element]) > 1.0e-4f)
        {
            return false;
        }
    }
    return true;
}

float PtInstanceMaxMatrixDelta(const float lhs[16], const float rhs[16])
{
    float maxDelta = 0.0f;
    for (int element = 0; element < 16; ++element)
    {
        maxDelta = Max(maxDelta, idMath::Fabs(lhs[element] - rhs[element]));
    }
    return maxDelta;
}

idVec3 PtInstanceMatrixOrigin(const float matrix[16])
{
    return idVec3(matrix[12], matrix[13], matrix[14]);
}

float PtInstanceOriginDelta(const float lhs[16], const float rhs[16])
{
    return (PtInstanceMatrixOrigin(lhs) - PtInstanceMatrixOrigin(rhs)).Length();
}

const char* PtInstanceSourceFlagSummary(uint32_t flags)
{
    if ((flags & RT_PT_INSTANCE_SOURCE_STATIC_WORLD) != 0)
    {
        return "static";
    }
    if ((flags & RT_PT_INSTANCE_SOURCE_RIGID) != 0)
    {
        return "rigid";
    }
    if ((flags & RT_PT_INSTANCE_SOURCE_SKINNED_OR_DEFORMING) != 0)
    {
        return "skinned/deforming";
    }
    if ((flags & RT_PT_INSTANCE_SOURCE_PARTICLE_OR_TRANSIENT) != 0)
    {
        return "particle/transient";
    }
    return "unknown";
}

}

#if !defined(RT_PT_INSTANCE_UNIVERSE_HARNESS)
bool RtPathTraceInstanceUniverse::CaptureInstanceUniverseSnapshot(
    RtPathTraceInstanceUniverseSnapshot& snapshot,
    const RtPathTracePlanningSnapshotEpoch& epoch,
    size_t& captureProductSlotBytes) const
{
    return CaptureInstanceUniverseSnapshotInternal(
        snapshot, epoch, captureProductSlotBytes, false);
}

bool RtPathTraceInstanceUniverse::CaptureCommittedInstanceUniverseSnapshot(
    RtPathTraceInstanceUniverseSnapshot& snapshot,
    const RtPathTraceCommittedBaselineEpoch& epoch,
    size_t& captureProductSlotBytes) const
{
    if (!RtPathTraceCommittedBaselineEpochValid(epoch))
    {
        snapshot.ResetAndRelease();
        return false;
    }
    const RtPathTracePlanningSnapshotEpoch snapshotEpoch =
        RtPathTraceMapCommittedBaselineToPlanningEpoch(epoch);
    return CaptureInstanceUniverseSnapshotInternal(
        snapshot, snapshotEpoch, captureProductSlotBytes, true);
}

bool RtPathTraceInstanceUniverse::CaptureInstanceUniverseSnapshotInternal(
    RtPathTraceInstanceUniverseSnapshot& snapshot,
    const RtPathTracePlanningSnapshotEpoch& epoch,
    size_t& captureProductSlotBytes,
    bool committedAfterEndFrame) const
{
    if ((committedAfterEndFrame ? m_frameActive :
            (!m_frameActive || epoch.frameIndex != m_frameIndex ||
                !RtPathTracePlanningEpochValid(epoch))))
    {
        snapshot.ResetAndRelease();
        return false;
    }
    return RtPathTraceBuildPlanningSnapshotTransaction(
        snapshot, captureProductSlotBytes,
        [&](RtPathTraceInstanceUniverseSnapshot& candidate)
        {
            size_t requiredBytes = captureProductSlotBytes;
            if (!RtPathTracePlanningAccumulateBytes(
                    sizeof(candidate), requiredBytes) ||
                !RtPathTracePlanningAccumulateArrayBytes(
                    m_meshRecords.size(),
                    sizeof(RtPathTraceInstanceMeshRecordPod), requiredBytes) ||
                !RtPathTracePlanningAccumulateArrayBytes(
                    m_instanceHistories.size(),
                    sizeof(RtPathTraceInstanceHistoryPod),
                    requiredBytes))
            {
                return false;
            }

            candidate.epoch = epoch;
            candidate.ownerGeneration = m_generation;
            candidate.meshes.reserve(m_meshRecords.size());
            for (const MeshRecord& record : m_meshRecords)
            {
                RtPathTraceInstanceMeshRecordPod pod;
                pod.stableHash = record.stableHash;
                pod.vertexBufferIdentity = record.key.vertexBufferIdentity;
                pod.indexBufferIdentity = record.key.indexBufferIdentity;
                pod.numVerts = record.key.numVerts;
                pod.numIndexes = record.key.numIndexes;
                pod.vertexFormat = record.key.vertexFormat;
                pod.materialId = record.key.materialId;
                pod.sourceKind = record.key.sourceKind;
                pod.lastSeenFrame = record.lastSeenFrame > 0
                    ? static_cast<uint64>(record.lastSeenFrame) : 0;
                pod.localSpaceValid = record.localSpaceValid;
                RtPathTracePlanningCopyName(pod.materialName,
                    sizeof(pod.materialName), record.materialName.c_str());
                RtPathTracePlanningCopyName(pod.modelName,
                    sizeof(pod.modelName), record.modelName.c_str());
                candidate.meshes.push_back(pod);
            }
            std::sort(candidate.meshes.begin(), candidate.meshes.end(),
                [](const RtPathTraceInstanceMeshRecordPod& lhs,
                    const RtPathTraceInstanceMeshRecordPod& rhs)
                {
                    return lhs.stableHash < rhs.stableHash;
                });

            const uint64 historyCopyStartUs = Sys_Microseconds();
            candidate.histories.reserve(m_instanceHistories.size());
            for (const InstanceHistory& history : m_instanceHistories)
            {
                if (history.instanceId == 0)
                {
                    return false;
                }
                RtPathTraceInstanceHistoryPod pod;
                pod.instanceId = history.instanceId;
                pod.lastSeenFrame = history.lastSeenFrame;
                memcpy(pod.firstObjectToWorld, history.firstObjectToWorld,
                    sizeof(pod.firstObjectToWorld));
                memcpy(pod.lastObjectToWorld, history.lastObjectToWorld,
                    sizeof(pod.lastObjectToWorld));
                pod.maxObservedMatrixDelta = history.maxObservedMatrixDelta;
                pod.maxObservedOriginDelta = history.maxObservedOriginDelta;
                pod.sameTransformCount = history.sameTransformCount;
                pod.changedTransformCount = history.changedTransformCount;
                candidate.histories.push_back(pod);
            }
            std::sort(candidate.histories.begin(), candidate.histories.end(),
                [](const RtPathTraceInstanceHistoryPod& lhs,
                    const RtPathTraceInstanceHistoryPod& rhs)
                {
                    return lhs.instanceId < rhs.instanceId;
                });
            for (size_t index = 1; index < candidate.histories.size(); ++index)
            {
                if (candidate.histories[index - 1].instanceId ==
                    candidate.histories[index].instanceId)
                {
                    return false;
                }
            }
            candidate.historyRows = candidate.histories.size();
            candidate.historyBytes = candidate.histories.capacity() *
                sizeof(RtPathTraceInstanceHistoryPod);
            candidate.historyCopyUs = Sys_Microseconds() - historyCopyStartUs;
            candidate.complete = true;
            return true;
        });
}

bool RtPathTraceInstanceUniverse::CountInstanceUniverseSnapshot(
    const RtPathTracePlanningSnapshotEpoch& epoch,
    RtPathTraceInstanceUniverseSnapshotCounts& counts) const
{
    counts = {};
    if (!m_frameActive || epoch.frameIndex != m_frameIndex ||
        !RtPathTracePlanningEpochValid(epoch))
    {
        return false;
    }
    counts.meshes = m_meshRecords.size();
    counts.histories = m_instanceHistories.size();
    return true;
}

bool RtPathTraceInstanceUniverse::FillInstanceUniverseSnapshotPreReserved(
    RtPathTraceInstanceUniverseSnapshot& snapshot,
    const RtPathTracePlanningSnapshotEpoch& epoch,
    const RtPathTraceInstanceUniverseSnapshotCounts& counts) const
{
    if (!m_frameActive || epoch.frameIndex != m_frameIndex ||
        !RtPathTracePlanningEpochValid(epoch) ||
        counts.meshes != m_meshRecords.size() ||
        counts.histories != m_instanceHistories.size() ||
        snapshot.meshes.capacity() < counts.meshes ||
        snapshot.histories.capacity() < counts.histories)
    {
        return false;
    }
    snapshot.epoch = epoch;
    snapshot.ownerGeneration = m_generation;
    snapshot.meshes.resize(counts.meshes);
    for (size_t index = 0; index < counts.meshes; ++index)
    {
        const MeshRecord& record = m_meshRecords[index];
        RtPathTraceInstanceMeshRecordPod& pod = snapshot.meshes[index];
        pod.stableHash = record.stableHash;
        pod.vertexBufferIdentity = record.key.vertexBufferIdentity;
        pod.indexBufferIdentity = record.key.indexBufferIdentity;
        pod.numVerts = record.key.numVerts;
        pod.numIndexes = record.key.numIndexes;
        pod.vertexFormat = record.key.vertexFormat;
        pod.materialId = record.key.materialId;
        pod.sourceKind = record.key.sourceKind;
        pod.lastSeenFrame = record.lastSeenFrame > 0
            ? static_cast<uint64>(record.lastSeenFrame) : 0;
        pod.localSpaceValid = record.localSpaceValid;
        RtPathTracePlanningCopyName(pod.materialName, sizeof(pod.materialName),
            record.materialName.c_str());
        RtPathTracePlanningCopyName(pod.modelName, sizeof(pod.modelName),
            record.modelName.c_str());
    }
    std::sort(snapshot.meshes.begin(), snapshot.meshes.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.stableHash < rhs.stableHash; });

    const uint64 historyCopyStartUs = Sys_Microseconds();
    snapshot.histories.resize(counts.histories);
    for (size_t index = 0; index < counts.histories; ++index)
    {
        const InstanceHistory& history = m_instanceHistories[index];
        if (history.instanceId == 0)
        {
            return false;
        }
        RtPathTraceInstanceHistoryPod& pod = snapshot.histories[index];
        pod.instanceId = history.instanceId;
        pod.lastSeenFrame = history.lastSeenFrame;
        memcpy(pod.firstObjectToWorld, history.firstObjectToWorld,
            sizeof(pod.firstObjectToWorld));
        memcpy(pod.lastObjectToWorld, history.lastObjectToWorld,
            sizeof(pod.lastObjectToWorld));
        pod.maxObservedMatrixDelta = history.maxObservedMatrixDelta;
        pod.maxObservedOriginDelta = history.maxObservedOriginDelta;
        pod.sameTransformCount = history.sameTransformCount;
        pod.changedTransformCount = history.changedTransformCount;
    }
    std::sort(snapshot.histories.begin(), snapshot.histories.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.instanceId < rhs.instanceId; });
    for (size_t index = 1; index < snapshot.histories.size(); ++index)
    {
        if (snapshot.histories[index - 1].instanceId ==
            snapshot.histories[index].instanceId)
        {
            return false;
        }
    }
    snapshot.historyRows = snapshot.histories.size();
    snapshot.historyBytes = snapshot.histories.capacity() *
        sizeof(RtPathTraceInstanceHistoryPod);
    snapshot.historyCopyUs = Sys_Microseconds() - historyCopyStartUs;
    snapshot.complete = true;
    return true;
}
#endif

void RtPathTraceInstanceUniverse::Clear()
{
    m_lifecycleSerial = PtNextObservationAuthoritySerial(m_lifecycleSerial);
    m_renderWorld = nullptr;
    m_frameIndex = 0;
    m_frameActive = false;
    m_meshRecords.clear();
    m_meshLookup.clear();
    m_frameMeshHashes.clear();
    m_instanceHistories.clear();
    m_instanceHistoryLookup.clear();
    m_frameInstances.clear();
    ResetFrameStats();
    ResetFrameObservationAuthority();
    ++m_generation;
}

void RtPathTraceInstanceUniverse::BeginFrame(uint64 frameIndex, const viewDef_t* viewDef)
{
#if defined(RT_PT_INSTANCE_UNIVERSE_HARNESS)
    const void* renderWorld = viewDef;
#else
    const void* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
#endif
    if (renderWorld != m_renderWorld)
    {
        Clear();
        m_renderWorld = renderWorld;
    }

    m_frameIndex = frameIndex;
    m_frameBeginSerial = PtNextObservationAuthoritySerial(m_frameBeginSerial);
    m_frameActive = true;
    ResetFrameStats();
    m_frameStats.frameIndex = frameIndex;
    m_frameStats.generation = m_generation;
    m_frameMeshHashes.clear();
    m_frameInstances.clear();
    ResetFrameObservationAuthority();
}

void RtPathTraceInstanceUniverse::EndFrame()
{
    if (!m_frameActive)
    {
        return;
    }

    m_frameStats.uniqueMeshCount = static_cast<int>(m_frameMeshHashes.size());
    m_frameStats.instanceCount = m_frameStats.usableDrawSurfs;
    m_frameActive = false;
}

void RtPathTraceInstanceUniverse::SetObservedDrawSurfCount(int drawSurfCount)
{
    ++m_frameObservedDrawSurfCountCalls;
    m_frameStats.drawSurfCount = drawSurfCount;
}

void RtPathTraceInstanceUniverse::RecordSkippedDrawSurf(const RtSmokeSurfaceSkipStats& skipStats)
{
    ++m_frameSkippedDrawSurfCalls;
    ++m_frameStats.skippedDrawSurfs;
    m_frameStats.nullSurfaceSkips += skipStats.nullSurface;
    m_frameStats.missingGeometrySkips += skipStats.missingGeometry;
    m_frameStats.nullMaterialSkips += skipStats.nullMaterial;
    m_frameStats.nullSpaceSkips += skipStats.nullSpace;
    m_frameStats.nullModelSkips += skipStats.nullModel;
    m_frameStats.invalidIndexSkips += skipStats.invalidIndexCount;
    m_frameStats.nonCurrentCacheSkips += skipStats.nonCurrentCache;
    m_frameStats.guiSurfaceSkips += skipStats.guiSurface;
    m_frameStats.callbackEntitySkips += skipStats.callbackEntity;
}

void RtPathTraceInstanceUniverse::RecordObservation(
    const RtPathTraceMeshObservation& meshObservation,
    const RtPathTraceInstanceObservation& instanceObservation,
    RtSmokeSurfaceClass surfaceClass,
    int numVerts,
    int numIndexes)
{
    RecordObservationImpl(
        m_meshRecords, m_meshLookup, m_instanceHistories,
        m_instanceHistoryLookup, m_generation, meshObservation,
        instanceObservation, surfaceClass, numVerts, numIndexes, nullptr);
}

bool RtPathTraceInstanceUniverse::BeginObservationTxn(
    ObservationTxn& txn,
    ObservationTxnAllocationTestSeam* allocationTestSeam) const
{
    txn.complete = false;
    if (!m_frameActive || !ObservationApplyMayBeFirstTouch())
    {
        return false;
    }
    txn.owner = this;
    txn.lifecycleSerial = m_lifecycleSerial;
    txn.frameBeginSerial = m_frameBeginSerial;
    txn.frameIndex = m_frameIndex;
    txn.baseGeneration = m_generation;
    txn.stagedObservationCount = 0;
    const auto beforeClone = [&]()
    {
        if (!allocationTestSeam)
        {
            return;
        }
        const size_t call = allocationTestSeam->cloneCalls++;
        if (call != allocationTestSeam->failAtClone)
        {
            return;
        }
        if (allocationTestSeam->throwLengthError)
        {
            throw std::length_error("instance observation clone test seam");
        }
        throw std::bad_alloc();
    };

    try
    {
        beforeClone();
        txn.meshRecords = m_meshRecords;
        beforeClone();
        txn.meshLookup = m_meshLookup;
        beforeClone();
        txn.instanceHistories = m_instanceHistories;
        beforeClone();
        txn.instanceHistoryLookup = m_instanceHistoryLookup;
        txn.generation = m_generation;
        txn.complete = true;
        return true;
    }
    catch (const std::bad_alloc&)
    {
        txn.complete = false;
        return false;
    }
    catch (const std::length_error&)
    {
        txn.complete = false;
        return false;
    }
}

bool RtPathTraceInstanceUniverse::RecordObservationStaged(
    ObservationTxn& txn,
    const RtPathTraceMeshObservation& meshObservation,
    const RtPathTraceInstanceObservation& instanceObservation,
    RtSmokeSurfaceClass surfaceClass,
    int numVerts,
    int numIndexes,
    ObservationReplayAllocationTestSeam* allocationTestSeam)
{
    if (!ObservationTxnMatchesLive(txn))
    {
        InvalidateObservationTxn(txn);
        return false;
    }
    try
    {
        RecordObservationImpl(
            txn.meshRecords, txn.meshLookup, txn.instanceHistories,
            txn.instanceHistoryLookup, txn.generation, meshObservation,
            instanceObservation, surfaceClass, numVerts, numIndexes,
            allocationTestSeam);
        ++txn.stagedObservationCount;
        return true;
    }
    catch (const std::bad_alloc&)
    {
        InvalidateObservationTxn(txn);
        return false;
    }
    catch (const std::length_error&)
    {
        InvalidateObservationTxn(txn);
        return false;
    }
    catch (...)
    {
        InvalidateObservationTxn(txn);
        throw;
    }
}

bool RtPathTraceInstanceUniverse::CommitObservationTxn(ObservationTxn& txn) noexcept
{
    using MeshRecords = decltype(m_meshRecords);
    using MeshLookup = decltype(m_meshLookup);
    using InstanceHistories = decltype(m_instanceHistories);
    using InstanceHistoryLookup = decltype(m_instanceHistoryLookup);
    static_assert(noexcept(std::declval<MeshRecords&>().swap(
        std::declval<MeshRecords&>())), "mesh record swap must be noexcept");
    static_assert(noexcept(std::declval<MeshLookup&>().swap(
        std::declval<MeshLookup&>())), "mesh lookup swap must be noexcept");
    static_assert(noexcept(std::declval<InstanceHistories&>().swap(
        std::declval<InstanceHistories&>())), "instance history swap must be noexcept");
    static_assert(noexcept(std::declval<InstanceHistoryLookup&>().swap(
        std::declval<InstanceHistoryLookup&>())), "instance lookup swap must be noexcept");
    static_assert(std::is_nothrow_assignable<uint64&, uint64>::value,
        "generation assignment must be noexcept");

    if (!ObservationTxnMatchesLive(txn))
    {
        InvalidateObservationTxn(txn);
        return false;
    }
    m_meshRecords.swap(txn.meshRecords);
    m_meshLookup.swap(txn.meshLookup);
    m_instanceHistories.swap(txn.instanceHistories);
    m_instanceHistoryLookup.swap(txn.instanceHistoryLookup);
    m_generation = txn.generation;
    InvalidateObservationTxn(txn);
    return true;
}

void RtPathTraceInstanceUniverse::AbortFrameObservations() noexcept
{
    m_frameMeshHashes.clear();
    m_frameInstances.clear();
    ResetFrameStats();
    m_frameStats.frameIndex = m_frameIndex;
    m_frameStats.generation = m_generation;
    ResetFrameObservationAuthority();
}

bool RtPathTraceInstanceUniverse::ObservationApplyMayBeFirstTouch() const noexcept
{
    return m_frameActive && m_frameObservedDrawSurfCountCalls == 0 &&
        m_frameSkippedDrawSurfCalls == 0 && m_frameObservationCalls == 0 &&
        m_frameMeshHashes.empty() && m_frameInstances.empty();
}

bool RtPathTraceInstanceUniverse::ObservationTxnMatchesLive(
    const ObservationTxn& txn) const noexcept
{
    return txn.complete && txn.owner == this && m_frameActive &&
        txn.lifecycleSerial != 0 && txn.lifecycleSerial == m_lifecycleSerial &&
        txn.frameBeginSerial != 0 && txn.frameBeginSerial == m_frameBeginSerial &&
        txn.frameIndex == m_frameIndex && txn.baseGeneration != 0 &&
        txn.baseGeneration == m_generation &&
        m_frameObservedDrawSurfCountCalls == 0 &&
        m_frameSkippedDrawSurfCalls == 0 &&
        m_frameObservationCalls == txn.stagedObservationCount;
}

void RtPathTraceInstanceUniverse::InvalidateObservationTxn(
    ObservationTxn& txn) const noexcept
{
    txn.complete = false;
}

void RtPathTraceInstanceUniverse::RecordObservationImpl(
    std::vector<MeshRecord>& meshRecords,
    std::unordered_map<uint64, size_t>& meshLookup,
    std::vector<InstanceHistory>& instanceHistories,
    std::unordered_map<uint64, size_t>& instanceHistoryLookup,
    uint64& generation,
    const RtPathTraceMeshObservation& meshObservation,
    const RtPathTraceInstanceObservation& instanceObservation,
    RtSmokeSurfaceClass surfaceClass,
    int numVerts,
    int numIndexes,
    ObservationReplayAllocationTestSeam* allocationTestSeam)
{
    PtMaybeFailObservationReplay(allocationTestSeam,
        ObservationReplayAllocationPhase::BeforePersistentMutation);
    ++m_frameObservationCalls;
    bool meshCacheHit = false;
    MeshRecord* meshRecord = FindOrCreateMeshRecord(
        meshRecords, meshLookup, generation, meshObservation, meshCacheHit);
    if (meshCacheHit)
    {
        ++m_frameStats.meshCacheHits;
    }
    else
    {
        ++m_frameStats.meshCacheMisses;
    }
    if (meshRecord)
    {
        meshRecord->lastSeenFrame = static_cast<int>(m_frameIndex);
        ++meshRecord->seenCount;
    }
    PtMaybeFailObservationReplay(allocationTestSeam,
        ObservationReplayAllocationPhase::AfterPersistentMutation);
    m_frameMeshHashes.insert(meshObservation.stableHash);
    PtMaybeFailObservationReplay(allocationTestSeam,
        ObservationReplayAllocationPhase::AfterLiveFrameMeshMutation);
    RtPathTraceInstanceObservation frameInstance = instanceObservation;
    if (frameInstance.surfaceClassId == 0u)
    {
        frameInstance.surfaceClassId = meshObservation.surfaceClassId;
    }

    ++m_frameStats.usableDrawSurfs;
    m_frameStats.instanceCount = m_frameStats.usableDrawSurfs;
    switch (surfaceClass)
    {
        case RtSmokeSurfaceClass::StaticWorld:
            ++m_frameStats.staticWorldSurfaces;
            break;
        case RtSmokeSurfaceClass::RigidEntity:
            ++m_frameStats.rigidSurfaces;
            break;
        case RtSmokeSurfaceClass::SkinnedDeformed:
            ++m_frameStats.skinnedOrDeformingSurfaces;
            break;
        case RtSmokeSurfaceClass::ParticleAlpha:
            ++m_frameStats.particleOrTransientSurfaces;
            break;
        default:
            ++m_frameStats.unknownSurfaces;
            break;
    }

    if ((frameInstance.sourceFlags & RT_PT_INSTANCE_SOURCE_STATIC_UNIVERSE_MATCH) != 0)
    {
        ++m_frameStats.staticUniverseMatches;
    }
    if ((frameInstance.sourceFlags & RT_PT_INSTANCE_SOURCE_STATIC_CACHE_MATCH) != 0)
    {
        ++m_frameStats.staticGeometryCacheMatches;
    }
    if ((frameInstance.sourceFlags & RT_PT_INSTANCE_SOURCE_MATERIAL_OVERRIDE) != 0)
    {
        ++m_frameStats.materialOverrideObservations;
    }
    if ((frameInstance.sourceFlags & RT_PT_INSTANCE_SOURCE_SKINNED_OR_DEFORMING) != 0)
    {
        ++m_frameStats.dynamicSkinnedDeformingCandidates;
    }
    if ((frameInstance.sourceFlags & RT_PT_INSTANCE_SOURCE_CALLBACK_OR_GENERATED) != 0)
    {
        ++m_frameStats.callbackOrGeneratedCandidates;
    }
    if ((frameInstance.sourceFlags & RT_PT_INSTANCE_SOURCE_GUI) != 0)
    {
        ++m_frameStats.guiCandidates;
    }
    if ((frameInstance.sourceFlags & RT_PT_INSTANCE_SOURCE_PARTICLE_OR_TRANSIENT) != 0)
    {
        ++m_frameStats.particlesOrTransientCandidates;
    }
    if (frameInstance.materialName.IsEmpty() || frameInstance.materialName.Icmp("<none>") == 0)
    {
        ++m_frameStats.missingMaterialOrSkinOverrideMetadata;
    }
    const RtPathTraceResidencyClass residencyClass = RtPathTraceResidencyClassForSourceFlags(frameInstance.sourceFlags);
    switch (residencyClass)
    {
        case RtPathTraceResidencyClass::StaticWorld:
            ++m_frameStats.residencyStaticWorldInstances;
            break;
        case RtPathTraceResidencyClass::DurableRigid:
            ++m_frameStats.residencyDurableRigidInstances;
            break;
        case RtPathTraceResidencyClass::DynamicFrame:
            ++m_frameStats.residencyDynamicFrameInstances;
            break;
        case RtPathTraceResidencyClass::TransientEffect:
            ++m_frameStats.residencyTransientEffectInstances;
            break;
        default:
            ++m_frameStats.residencyUnknownInstances;
            break;
    }

    InstanceHistory* history = FindOrCreateInstanceHistory(
        instanceHistories, instanceHistoryLookup, frameInstance.instanceId);
    if (history)
    {
        const bool hasAnyPrevious = history->lastSeenFrame > 0;
        const bool hasConsecutivePrevious = hasAnyPrevious && history->lastSeenFrame + 1 == m_frameIndex;
        if (hasConsecutivePrevious)
        {
            frameInstance.hasPreviousObjectToWorld = true;
            frameInstance.transformContinuous = true;
            memcpy(frameInstance.previousObjectToWorld, history->lastObjectToWorld, sizeof(frameInstance.previousObjectToWorld));
            if (residencyClass == RtPathTraceResidencyClass::DynamicFrame)
            {
                ++m_frameStats.dynamicFramePreviousMatches;
            }
            else if (residencyClass == RtPathTraceResidencyClass::TransientEffect)
            {
                ++m_frameStats.transientEffectPreviousMatches;
            }
        }
        if (!hasAnyPrevious)
        {
            memcpy(history->firstObjectToWorld, frameInstance.objectToWorld, sizeof(history->firstObjectToWorld));
        }
        else
        {
            history->maxObservedMatrixDelta = Max(history->maxObservedMatrixDelta, PtInstanceMaxMatrixDelta(history->firstObjectToWorld, frameInstance.objectToWorld));
            history->maxObservedOriginDelta = Max(history->maxObservedOriginDelta, PtInstanceOriginDelta(history->firstObjectToWorld, frameInstance.objectToWorld));
            if (PtInstanceMatricesMatch(history->lastObjectToWorld, frameInstance.objectToWorld))
            {
                if (hasConsecutivePrevious)
                {
                    ++history->sameTransformCount;
                    ++m_frameStats.sameTransformObservations;
                }
            }
            else
            {
                ++history->changedTransformCount;
                if (hasConsecutivePrevious)
                {
                    ++m_frameStats.changedTransformObservations;
                    if (surfaceClass == RtSmokeSurfaceClass::RigidEntity)
                    {
                        ++m_frameStats.changingTransformRigidObservations;
                    }
                }
            }
        }
        if (history->changedTransformCount > 0)
        {
            ++m_frameStats.everChangedTransformObservations;
            if (surfaceClass == RtSmokeSurfaceClass::RigidEntity)
            {
                ++m_frameStats.everChangedRigidTransformObservations;
                AddMovedRigidSample(meshObservation, frameInstance, *history);
            }
        }
        memcpy(history->lastObjectToWorld, frameInstance.objectToWorld, sizeof(history->lastObjectToWorld));
        history->lastSeenFrame = m_frameIndex;
    }

    m_frameInstances.push_back(frameInstance);
    PtMaybeFailObservationReplay(allocationTestSeam,
        ObservationReplayAllocationPhase::AfterLiveFrameInstanceMutation);
    AddSample(meshObservation, frameInstance, surfaceClass, numVerts, numIndexes);
}

const RtPathTraceInstanceUniverseStats& RtPathTraceInstanceUniverse::GetFrameStats() const
{
    return m_frameStats;
}

const std::vector<RtPathTraceInstanceObservation>& RtPathTraceInstanceUniverse::FrameInstances() const
{
    return m_frameInstances;
}

bool RtPathTraceInstanceUniverse::HasFrameInstance(uint64 instanceId) const
{
    if (instanceId == 0)
    {
        return false;
    }

    for (const RtPathTraceInstanceObservation& instance : m_frameInstances)
    {
        if (instance.instanceId == instanceId)
        {
            return true;
        }
    }

    return false;
}

#if !defined(RT_PT_INSTANCE_UNIVERSE_HARNESS)
void RtPathTraceInstanceUniverse::RunDiagnostics(const RtPathTraceInstanceUniverseDiagnosticDesc& desc)
{
    if (r_pathTracingSmokeLog.GetInteger() != 0 && (m_frameIndex % 120ull) == 1ull)
    {
        common->Printf("PathTracePrimaryPass: PT drawSurf mirror source=%d drawSurfs=%d usable=%d skipped=%d meshes=%d instances=%d oldSmokeSurfaces=%d static/rigid/skinned/particle/unknown=%d/%d/%d/%d/%d residencyClass(static/durable/dynamic/transient/unknown)=%d/%d/%d/%d/%d dynamicPrevMatch(dynamic/transient)=%d/%d matches(scene/staticCache)=%d/%d transforms(same/changed/rigidChanged)=%d/%d/%d lifetimeChanged(all/rigid)=%d/%d overrides=%d missingMaterial=%d candidates(skinned/callback/gui/transient)=%d/%d/%d/%d\n",
            desc.sceneSource,
            m_frameStats.drawSurfCount,
            m_frameStats.usableDrawSurfs,
            m_frameStats.skippedDrawSurfs,
            m_frameStats.uniqueMeshCount,
            m_frameStats.instanceCount,
            desc.legacySourceSurfaces,
            m_frameStats.staticWorldSurfaces,
            m_frameStats.rigidSurfaces,
            m_frameStats.skinnedOrDeformingSurfaces,
            m_frameStats.particleOrTransientSurfaces,
            m_frameStats.unknownSurfaces,
            m_frameStats.residencyStaticWorldInstances,
            m_frameStats.residencyDurableRigidInstances,
            m_frameStats.residencyDynamicFrameInstances,
            m_frameStats.residencyTransientEffectInstances,
            m_frameStats.residencyUnknownInstances,
            m_frameStats.dynamicFramePreviousMatches,
            m_frameStats.transientEffectPreviousMatches,
            m_frameStats.staticUniverseMatches,
            m_frameStats.staticGeometryCacheMatches,
            m_frameStats.sameTransformObservations,
            m_frameStats.changedTransformObservations,
            m_frameStats.changingTransformRigidObservations,
            m_frameStats.everChangedTransformObservations,
            m_frameStats.everChangedRigidTransformObservations,
            m_frameStats.materialOverrideObservations,
            m_frameStats.missingMaterialOrSkinOverrideMetadata,
            m_frameStats.dynamicSkinnedDeformingCandidates,
            m_frameStats.callbackOrGeneratedCandidates,
            m_frameStats.guiCandidates,
            m_frameStats.particlesOrTransientCandidates);
    }

    if (!desc.dumpRequested)
    {
        return;
    }

    const RtSmokeSurfaceClassStats* legacyClassStats = desc.legacyClassStats;
    const RtSmokeSurfaceSkipStats* legacySkipStats = desc.legacySkipStats;
    common->Printf("PathTracePrimaryPass: PT instance universe dump source=%d frame=%llu generation=%llu drawSurfs=%d usable=%d skipped=%d uniqueMeshes=%d instances=%d meshCache(hit/miss)=%d/%d\n",
        desc.sceneSource,
        static_cast<unsigned long long>(m_frameStats.frameIndex),
        static_cast<unsigned long long>(m_frameStats.generation),
        m_frameStats.drawSurfCount,
        m_frameStats.usableDrawSurfs,
        m_frameStats.skippedDrawSurfs,
        m_frameStats.uniqueMeshCount,
        m_frameStats.instanceCount,
        m_frameStats.meshCacheHits,
        m_frameStats.meshCacheMisses);
    common->Printf("PathTracePrimaryPass: PT instance universe classes mirror static/rigid/skinned/particle/unknown=%d/%d/%d/%d/%d oldSmoke=%d/%d/%d/%d/%d sourceSurfaces=%d\n",
        m_frameStats.staticWorldSurfaces,
        m_frameStats.rigidSurfaces,
        m_frameStats.skinnedOrDeformingSurfaces,
        m_frameStats.particleOrTransientSurfaces,
        m_frameStats.unknownSurfaces,
        legacyClassStats ? legacyClassStats->staticWorldSurfaces : 0,
        legacyClassStats ? legacyClassStats->rigidEntitySurfaces : 0,
        legacyClassStats ? legacyClassStats->skinnedDeformedSurfaces : 0,
        legacyClassStats ? legacyClassStats->particleAlphaSurfaces : 0,
        legacyClassStats ? legacyClassStats->unknownSurfaces : 0,
        desc.legacySourceSurfaces);
    common->Printf("PathTracePrimaryPass: PT instance universe identity residencyClass(static/durable/dynamic/transient/unknown)=%d/%d/%d/%d/%d dynamicPrevMatch(dynamic/transient)=%d/%d matches(scene/staticCache)=%d/%d transforms(same/changed/rigidChanged)=%d/%d/%d lifetimeChanged(all/rigid)=%d/%d overrides=%d missingMaterialOrSkin=%d dynamicCandidates(skinned/callback/gui/transient)=%d/%d/%d/%d\n",
        m_frameStats.residencyStaticWorldInstances,
        m_frameStats.residencyDurableRigidInstances,
        m_frameStats.residencyDynamicFrameInstances,
        m_frameStats.residencyTransientEffectInstances,
        m_frameStats.residencyUnknownInstances,
        m_frameStats.dynamicFramePreviousMatches,
        m_frameStats.transientEffectPreviousMatches,
        m_frameStats.staticUniverseMatches,
        m_frameStats.staticGeometryCacheMatches,
        m_frameStats.sameTransformObservations,
        m_frameStats.changedTransformObservations,
        m_frameStats.changingTransformRigidObservations,
        m_frameStats.everChangedTransformObservations,
        m_frameStats.everChangedRigidTransformObservations,
        m_frameStats.materialOverrideObservations,
        m_frameStats.missingMaterialOrSkinOverrideMetadata,
        m_frameStats.dynamicSkinnedDeformingCandidates,
        m_frameStats.callbackOrGeneratedCandidates,
        m_frameStats.guiCandidates,
        m_frameStats.particlesOrTransientCandidates);
    common->Printf("PathTracePrimaryPass: PT instance universe skips mirror null/missingGeo/nullMat/nullSpace/nullModel/invalid/nonCurrent/gui/callback=%d/%d/%d/%d/%d/%d/%d/%d/%d oldSmoke=%d/%d/%d/%d/%d/%d/%d/%d/%d\n",
        m_frameStats.nullSurfaceSkips,
        m_frameStats.missingGeometrySkips,
        m_frameStats.nullMaterialSkips,
        m_frameStats.nullSpaceSkips,
        m_frameStats.nullModelSkips,
        m_frameStats.invalidIndexSkips,
        m_frameStats.nonCurrentCacheSkips,
        m_frameStats.guiSurfaceSkips,
        m_frameStats.callbackEntitySkips,
        legacySkipStats ? legacySkipStats->nullSurface : 0,
        legacySkipStats ? legacySkipStats->missingGeometry : 0,
        legacySkipStats ? legacySkipStats->nullMaterial : 0,
        legacySkipStats ? legacySkipStats->nullSpace : 0,
        legacySkipStats ? legacySkipStats->nullModel : 0,
        legacySkipStats ? legacySkipStats->invalidIndexCount : 0,
        legacySkipStats ? legacySkipStats->nonCurrentCache : 0,
        legacySkipStats ? legacySkipStats->guiSurface : 0,
        legacySkipStats ? legacySkipStats->callbackEntity : 0);

    for (int sampleIndex = 0; sampleIndex < m_frameStats.sampleCount; ++sampleIndex)
    {
        const RtPathTraceInstanceUniverseSample& sample = m_frameStats.samples[sampleIndex];
        if (!sample.valid)
        {
            continue;
        }

        common->Printf("PathTracePrimaryPass: PT instance sample %d surf=%d entity=%d renderEntity=%d class=%s flags=%s mesh=%llu instance=%llu origin=(%.2f %.2f %.2f) verts=%d indexes=%d material='%s' model='%s'\n",
            sampleIndex,
            sample.drawSurfIndex,
            sample.entityIndex,
            sample.renderEntityNum,
            SmokeSurfaceClassName(sample.surfaceClass),
            PtInstanceSourceFlagSummary(sample.sourceFlags),
            static_cast<unsigned long long>(sample.meshHash),
            static_cast<unsigned long long>(sample.instanceId),
            sample.origin.x,
            sample.origin.y,
            sample.origin.z,
            sample.verts,
            sample.indexes,
            sample.materialName.c_str(),
            sample.modelName.c_str());
    }

    for (int sampleIndex = 0; sampleIndex < m_frameStats.movedRigidSampleCount; ++sampleIndex)
    {
        const RtPathTraceMovedRigidInstanceSample& sample = m_frameStats.movedRigidSamples[sampleIndex];
        if (!sample.valid)
        {
            continue;
        }

        common->Printf("PathTracePrimaryPass: PT moved rigid sample %d surf=%d entity=%d renderEntity=%d mesh=%llu instance=%llu changes=%d firstOrigin=(%.2f %.2f %.2f) currentOrigin=(%.2f %.2f %.2f) currentMatrixDelta=%.6f maxObservedMatrixDelta=%.6f maxObservedOriginDelta=%.3f material='%s' model='%s'\n",
            sampleIndex,
            sample.drawSurfIndex,
            sample.entityIndex,
            sample.renderEntityNum,
            static_cast<unsigned long long>(sample.meshHash),
            static_cast<unsigned long long>(sample.instanceId),
            sample.transformChangeCount,
            sample.firstOrigin.x,
            sample.firstOrigin.y,
            sample.firstOrigin.z,
            sample.currentOrigin.x,
            sample.currentOrigin.y,
            sample.currentOrigin.z,
            sample.maxFirstToCurrentMatrixDelta,
            sample.maxObservedMatrixDelta,
            sample.maxObservedOriginDelta,
            sample.materialName.c_str(),
            sample.modelName.c_str());
    }

    r_pathTracingInstanceUniverseDump.SetInteger(0);
}
#endif

void RtPathTraceInstanceUniverse::ResetFrameStats()
{
    m_frameStats = RtPathTraceInstanceUniverseStats();
}

void RtPathTraceInstanceUniverse::ResetFrameObservationAuthority() noexcept
{
    m_frameObservedDrawSurfCountCalls = 0;
    m_frameSkippedDrawSurfCalls = 0;
    m_frameObservationCalls = 0;
}

RtPathTraceInstanceUniverse::MeshRecord* RtPathTraceInstanceUniverse::FindOrCreateMeshRecord(
    std::vector<MeshRecord>& meshRecords,
    std::unordered_map<uint64, size_t>& meshLookup,
    uint64& generation,
    const RtPathTraceMeshObservation& observation,
    bool& cacheHit)
{
    cacheHit = false;
    const std::unordered_map<uint64, size_t>::iterator it = meshLookup.find(observation.stableHash);
    if (it != meshLookup.end() && it->second < meshRecords.size())
    {
        MeshRecord& record = meshRecords[it->second];
        cacheHit = true;
        return &record;
    }

    MeshRecord record;
    record.key = observation.key;
    record.stableHash = observation.stableHash;
    record.baseMaterial = observation.baseMaterial;
    record.materialName = observation.materialName;
    record.modelName = observation.modelName;
    record.firstSeenFrame = static_cast<int>(m_frameIndex);
    record.lastSeenFrame = static_cast<int>(m_frameIndex);
    record.seenCount = 0;
    record.localSpaceValid = observation.localSpaceValid;
    const size_t recordIndex = meshRecords.size();
    meshRecords.push_back(record);
    meshLookup[observation.stableHash] = recordIndex;
    ++generation;
    return &meshRecords.back();
}

RtPathTraceInstanceUniverse::InstanceHistory* RtPathTraceInstanceUniverse::FindOrCreateInstanceHistory(
    std::vector<InstanceHistory>& instanceHistories,
    std::unordered_map<uint64, size_t>& instanceHistoryLookup,
    uint64 instanceId)
{
    const std::unordered_map<uint64, size_t>::iterator it = instanceHistoryLookup.find(instanceId);
    if (it != instanceHistoryLookup.end() && it->second < instanceHistories.size())
    {
        return &instanceHistories[it->second];
    }

    InstanceHistory history;
    history.instanceId = instanceId;
    const size_t historyIndex = instanceHistories.size();
    instanceHistories.push_back(history);
    instanceHistoryLookup[instanceId] = historyIndex;
    return &instanceHistories.back();
}

#if defined(RT_PT_INSTANCE_UNIVERSE_HARNESS)
bool RtPathTraceInstanceUniverse::DebugPersistentStateEquals(
    const RtPathTraceInstanceUniverse& rhs) const
{
    if (m_generation != rhs.m_generation ||
        m_meshLookup != rhs.m_meshLookup ||
        m_instanceHistoryLookup != rhs.m_instanceHistoryLookup ||
        m_meshRecords.size() != rhs.m_meshRecords.size() ||
        m_instanceHistories.size() != rhs.m_instanceHistories.size())
    {
        return false;
    }
    for (size_t index = 0; index < m_meshRecords.size(); ++index)
    {
        const MeshRecord& lhsRecord = m_meshRecords[index];
        const MeshRecord& rhsRecord = rhs.m_meshRecords[index];
        const RtPathTraceMeshKey& lhsKey = lhsRecord.key;
        const RtPathTraceMeshKey& rhsKey = rhsRecord.key;
        if (lhsKey.tri != rhsKey.tri ||
            lhsKey.vertexBufferIdentity != rhsKey.vertexBufferIdentity ||
            lhsKey.indexBufferIdentity != rhsKey.indexBufferIdentity ||
            lhsKey.numVerts != rhsKey.numVerts ||
            lhsKey.numIndexes != rhsKey.numIndexes ||
            lhsKey.vertexFormat != rhsKey.vertexFormat ||
            lhsKey.materialId != rhsKey.materialId ||
            lhsKey.materialClassSignature != rhsKey.materialClassSignature ||
            lhsKey.sourceKind != rhsKey.sourceKind ||
            lhsRecord.stableHash != rhsRecord.stableHash ||
            lhsRecord.baseMaterial != rhsRecord.baseMaterial ||
            lhsRecord.materialName.Cmp(rhsRecord.materialName.c_str()) != 0 ||
            lhsRecord.modelName.Cmp(rhsRecord.modelName.c_str()) != 0 ||
            lhsRecord.firstSeenFrame != rhsRecord.firstSeenFrame ||
            lhsRecord.lastSeenFrame != rhsRecord.lastSeenFrame ||
            lhsRecord.seenCount != rhsRecord.seenCount ||
            lhsRecord.localSpaceValid != rhsRecord.localSpaceValid)
        {
            return false;
        }
    }
    for (size_t index = 0; index < m_instanceHistories.size(); ++index)
    {
        const InstanceHistory& lhsHistory = m_instanceHistories[index];
        const InstanceHistory& rhsHistory = rhs.m_instanceHistories[index];
        if (lhsHistory.instanceId != rhsHistory.instanceId ||
            lhsHistory.lastSeenFrame != rhsHistory.lastSeenFrame ||
            memcmp(lhsHistory.firstObjectToWorld,
                rhsHistory.firstObjectToWorld,
                sizeof(lhsHistory.firstObjectToWorld)) != 0 ||
            memcmp(lhsHistory.lastObjectToWorld,
                rhsHistory.lastObjectToWorld,
                sizeof(lhsHistory.lastObjectToWorld)) != 0 ||
            lhsHistory.maxObservedMatrixDelta != rhsHistory.maxObservedMatrixDelta ||
            lhsHistory.maxObservedOriginDelta != rhsHistory.maxObservedOriginDelta ||
            lhsHistory.sameTransformCount != rhsHistory.sameTransformCount ||
            lhsHistory.changedTransformCount != rhsHistory.changedTransformCount)
        {
            return false;
        }
    }
    return true;
}

bool RtPathTraceInstanceUniverse::DebugFrameStateEquals(
    const RtPathTraceInstanceUniverse& rhs) const
{
    if (m_frameIndex != rhs.m_frameIndex || m_frameActive != rhs.m_frameActive ||
        m_frameMeshHashes != rhs.m_frameMeshHashes ||
        m_frameInstances.size() != rhs.m_frameInstances.size() ||
        m_frameObservedDrawSurfCountCalls != rhs.m_frameObservedDrawSurfCountCalls ||
        m_frameSkippedDrawSurfCalls != rhs.m_frameSkippedDrawSurfCalls ||
        m_frameObservationCalls != rhs.m_frameObservationCalls)
    {
        return false;
    }
    for (size_t index = 0; index < m_frameInstances.size(); ++index)
    {
        const RtPathTraceInstanceObservation& lhs = m_frameInstances[index];
        const RtPathTraceInstanceObservation& right = rhs.m_frameInstances[index];
        if (lhs.instanceId != right.instanceId || lhs.meshHash != right.meshHash ||
            lhs.entity != right.entity || lhs.entityIndex != right.entityIndex ||
            lhs.renderEntityNum != right.renderEntityNum ||
            lhs.drawSurfIndex != right.drawSurfIndex ||
            lhs.modelSurfaceIndex != right.modelSurfaceIndex ||
            lhs.jointIndex != right.jointIndex || lhs.currentArea != right.currentArea ||
            memcmp(&lhs.renderDefKey, &right.renderDefKey, sizeof(lhs.renderDefKey)) != 0 ||
            lhs.modelEpoch != right.modelEpoch ||
            lhs.materialOverrideId != right.materialOverrideId ||
            lhs.surfaceClassId != right.surfaceClassId ||
            lhs.triangleClassAndFlags != right.triangleClassAndFlags ||
            lhs.sourceFlags != right.sourceFlags || lhs.trustFlags != right.trustFlags ||
            memcmp(lhs.objectToWorld, right.objectToWorld, sizeof(lhs.objectToWorld)) != 0 ||
            lhs.hasPreviousObjectToWorld != right.hasPreviousObjectToWorld ||
            lhs.transformContinuous != right.transformContinuous ||
            memcmp(lhs.previousObjectToWorld, right.previousObjectToWorld,
                sizeof(lhs.previousObjectToWorld)) != 0 ||
            lhs.materialName.Cmp(right.materialName.c_str()) != 0 ||
            lhs.modelName.Cmp(right.modelName.c_str()) != 0)
        {
            return false;
        }
    }

#define RT_PT_COMPARE_STAT(field) if (m_frameStats.field != rhs.m_frameStats.field) return false
    RT_PT_COMPARE_STAT(drawSurfCount);
    RT_PT_COMPARE_STAT(usableDrawSurfs);
    RT_PT_COMPARE_STAT(skippedDrawSurfs);
    RT_PT_COMPARE_STAT(uniqueMeshCount);
    RT_PT_COMPARE_STAT(instanceCount);
    RT_PT_COMPARE_STAT(meshCacheHits);
    RT_PT_COMPARE_STAT(meshCacheMisses);
    RT_PT_COMPARE_STAT(staticWorldSurfaces);
    RT_PT_COMPARE_STAT(rigidSurfaces);
    RT_PT_COMPARE_STAT(skinnedOrDeformingSurfaces);
    RT_PT_COMPARE_STAT(particleOrTransientSurfaces);
    RT_PT_COMPARE_STAT(unknownSurfaces);
    RT_PT_COMPARE_STAT(staticUniverseMatches);
    RT_PT_COMPARE_STAT(staticGeometryCacheMatches);
    RT_PT_COMPARE_STAT(sameTransformObservations);
    RT_PT_COMPARE_STAT(changedTransformObservations);
    RT_PT_COMPARE_STAT(changingTransformRigidObservations);
    RT_PT_COMPARE_STAT(everChangedTransformObservations);
    RT_PT_COMPARE_STAT(everChangedRigidTransformObservations);
    RT_PT_COMPARE_STAT(materialOverrideObservations);
    RT_PT_COMPARE_STAT(missingMaterialOrSkinOverrideMetadata);
    RT_PT_COMPARE_STAT(residencyStaticWorldInstances);
    RT_PT_COMPARE_STAT(residencyDurableRigidInstances);
    RT_PT_COMPARE_STAT(residencyDynamicFrameInstances);
    RT_PT_COMPARE_STAT(residencyTransientEffectInstances);
    RT_PT_COMPARE_STAT(residencyUnknownInstances);
    RT_PT_COMPARE_STAT(dynamicSkinnedDeformingCandidates);
    RT_PT_COMPARE_STAT(callbackOrGeneratedCandidates);
    RT_PT_COMPARE_STAT(guiCandidates);
    RT_PT_COMPARE_STAT(particlesOrTransientCandidates);
    RT_PT_COMPARE_STAT(dynamicFramePreviousMatches);
    RT_PT_COMPARE_STAT(transientEffectPreviousMatches);
    RT_PT_COMPARE_STAT(nullSurfaceSkips);
    RT_PT_COMPARE_STAT(missingGeometrySkips);
    RT_PT_COMPARE_STAT(nullMaterialSkips);
    RT_PT_COMPARE_STAT(nullSpaceSkips);
    RT_PT_COMPARE_STAT(nullModelSkips);
    RT_PT_COMPARE_STAT(invalidIndexSkips);
    RT_PT_COMPARE_STAT(nonCurrentCacheSkips);
    RT_PT_COMPARE_STAT(guiSurfaceSkips);
    RT_PT_COMPARE_STAT(callbackEntitySkips);
    RT_PT_COMPARE_STAT(frameIndex);
    RT_PT_COMPARE_STAT(generation);
    RT_PT_COMPARE_STAT(sampleCount);
    RT_PT_COMPARE_STAT(movedRigidSampleCount);
#undef RT_PT_COMPARE_STAT

    for (int index = 0; index < m_frameStats.sampleCount; ++index)
    {
        const RtPathTraceInstanceUniverseSample& lhs = m_frameStats.samples[index];
        const RtPathTraceInstanceUniverseSample& right = rhs.m_frameStats.samples[index];
        if (lhs.valid != right.valid || lhs.drawSurfIndex != right.drawSurfIndex ||
            lhs.entityIndex != right.entityIndex || lhs.renderEntityNum != right.renderEntityNum ||
            lhs.verts != right.verts || lhs.indexes != right.indexes ||
            lhs.meshHash != right.meshHash || lhs.instanceId != right.instanceId ||
            lhs.surfaceClass != right.surfaceClass || lhs.sourceFlags != right.sourceFlags ||
            lhs.origin != right.origin ||
            lhs.materialName.Cmp(right.materialName.c_str()) != 0 ||
            lhs.modelName.Cmp(right.modelName.c_str()) != 0)
        {
            return false;
        }
    }
    for (int index = 0; index < m_frameStats.movedRigidSampleCount; ++index)
    {
        const RtPathTraceMovedRigidInstanceSample& lhs = m_frameStats.movedRigidSamples[index];
        const RtPathTraceMovedRigidInstanceSample& right = rhs.m_frameStats.movedRigidSamples[index];
        if (lhs.valid != right.valid || lhs.drawSurfIndex != right.drawSurfIndex ||
            lhs.entityIndex != right.entityIndex || lhs.renderEntityNum != right.renderEntityNum ||
            lhs.meshHash != right.meshHash || lhs.instanceId != right.instanceId ||
            lhs.transformChangeCount != right.transformChangeCount ||
            lhs.maxFirstToCurrentMatrixDelta != right.maxFirstToCurrentMatrixDelta ||
            lhs.maxObservedMatrixDelta != right.maxObservedMatrixDelta ||
            lhs.maxObservedOriginDelta != right.maxObservedOriginDelta ||
            lhs.firstOrigin != right.firstOrigin || lhs.currentOrigin != right.currentOrigin ||
            lhs.materialName.Cmp(right.materialName.c_str()) != 0 ||
            lhs.modelName.Cmp(right.modelName.c_str()) != 0)
        {
            return false;
        }
    }
    return true;
}
#endif

void RtPathTraceInstanceUniverse::AddSample(
    const RtPathTraceMeshObservation& meshObservation,
    const RtPathTraceInstanceObservation& instanceObservation,
    RtSmokeSurfaceClass surfaceClass,
    int numVerts,
    int numIndexes)
{
    if (m_frameStats.sampleCount >= RT_PT_INSTANCE_UNIVERSE_SAMPLES)
    {
        return;
    }

    RtPathTraceInstanceUniverseSample& sample = m_frameStats.samples[m_frameStats.sampleCount++];
    sample.valid = true;
    sample.drawSurfIndex = instanceObservation.drawSurfIndex;
    sample.entityIndex = instanceObservation.entityIndex;
    sample.renderEntityNum = instanceObservation.renderEntityNum;
    sample.verts = numVerts;
    sample.indexes = numIndexes;
    sample.meshHash = meshObservation.stableHash;
    sample.instanceId = instanceObservation.instanceId;
    sample.surfaceClass = surfaceClass;
    sample.sourceFlags = instanceObservation.sourceFlags;
    sample.origin.Set(
        instanceObservation.objectToWorld[12],
        instanceObservation.objectToWorld[13],
        instanceObservation.objectToWorld[14]);
    sample.materialName = instanceObservation.materialName;
    sample.modelName = instanceObservation.modelName;
}

void RtPathTraceInstanceUniverse::AddMovedRigidSample(
    const RtPathTraceMeshObservation& meshObservation,
    const RtPathTraceInstanceObservation& instanceObservation,
    const InstanceHistory& history)
{
    if (m_frameStats.movedRigidSampleCount >= RT_PT_INSTANCE_UNIVERSE_MOVED_RIGID_SAMPLES)
    {
        return;
    }

    RtPathTraceMovedRigidInstanceSample& sample = m_frameStats.movedRigidSamples[m_frameStats.movedRigidSampleCount++];
    sample.valid = true;
    sample.drawSurfIndex = instanceObservation.drawSurfIndex;
    sample.entityIndex = instanceObservation.entityIndex;
    sample.renderEntityNum = instanceObservation.renderEntityNum;
    sample.meshHash = meshObservation.stableHash;
    sample.instanceId = instanceObservation.instanceId;
    sample.transformChangeCount = history.changedTransformCount;
    sample.maxFirstToCurrentMatrixDelta = PtInstanceMaxMatrixDelta(history.firstObjectToWorld, instanceObservation.objectToWorld);
    sample.maxObservedMatrixDelta = history.maxObservedMatrixDelta;
    sample.maxObservedOriginDelta = history.maxObservedOriginDelta;
    sample.firstOrigin = PtInstanceMatrixOrigin(history.firstObjectToWorld);
    sample.currentOrigin = PtInstanceMatrixOrigin(instanceObservation.objectToWorld);
    sample.materialName = instanceObservation.materialName;
    sample.modelName = instanceObservation.modelName;
}
