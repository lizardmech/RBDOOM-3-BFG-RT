#pragma once

// Pure GEO-08 lifecycle contract for one updateable BLAS per canonical
// skinned-surface instance. This file owns no renderer or GPU objects.

#include "PathTraceCanonicalGeometryIdentity.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

enum class PtSkinnedBlasState : std::uint32_t
{
    Absent = 0,
    BuildPending,
    Ready,
    UpdatePending,
    RebuildPending,
    Failed,
    Retiring
};

enum class PtSkinnedBlasAction : std::uint32_t
{
    None = 0,
    Build,
    Update,
    Rebuild
};

enum class PtSkinnedBlasObserveResult : std::uint32_t
{
    BuildRequired = 0,
    UpdateRequired,
    RebuildRequired,
    AlreadyPending,
    InvalidWorld,
    InvalidInstanceKey,
    InvalidMeshKey,
    InvalidSourceChecksum,
    InvalidSourceGpuGeneration,
    InvalidSourceIndexRange,
    InvalidOutputStorageGeneration,
    InvalidOutputRange,
    DispatchNotReady,
    RetiredInstanceKey
};

enum class PtSkinnedBlasSubmitResult : std::uint32_t
{
    Succeeded = 0,
    Failed,
    NotFound,
    ActionMismatch
};

enum class PtSkinnedBlasRetireResult : std::uint32_t
{
    Retired = 0,
    NotFound,
    AlreadyRetired,
    InvalidInstanceKey
};

struct PtSkinnedBlasConfig
{
    std::uint64_t outputVertexStrideBytes = 112;
    std::uint64_t rebuildCadenceFrames = 60;
};

struct PtSkinnedBlasCandidate
{
    PtCanonicalInstanceKey instanceKey;
    PtCanonicalMeshKey meshKey;
    std::uint64_t sourceChecksum = 0;
    std::uint64_t sourceGpuIndexGeneration = 0;
    std::uint64_t sourceIndexOffsetBytes = 0;
    std::uint64_t sourceIndexCapacityBytes = 0;
    std::uint64_t outputStorageGeneration = 0;
    std::uint64_t outputVertexOffsetBytes = 0;
    std::uint64_t outputVertexCount = 0;
    std::uint64_t outputCapacityBytes = 0;
    std::uint64_t frameIndex = 0;
    bool dispatchReady = false;
    bool forceRebuild = false;
};

struct PtSkinnedBlasRecord
{
    PtCanonicalInstanceKey instanceKey;
    std::uint64_t instanceHash = 0;
    PtCanonicalMeshKey meshKey;
    std::uint64_t sourceChecksum = 0;
    std::uint64_t sourceGpuIndexGeneration = 0;
    std::uint64_t sourceIndexOffsetBytes = 0;
    std::uint64_t sourceIndexBytes = 0;
    std::uint64_t outputStorageGeneration = 0;
    std::uint64_t outputVertexOffsetBytes = 0;
    std::uint64_t outputVertexCount = 0;
    std::uint64_t outputVertexBytes = 0;
    std::uint64_t firstSeenFrame = 0;
    std::uint64_t lastSeenFrame = 0;
    std::uint64_t lastFullBuildFrame = 0;
    std::uint64_t lastUpdateFrame = 0;
    std::uint64_t blasGeneration = 0;
    std::uint64_t stateRevision = 0;
    PtSkinnedBlasState state = PtSkinnedBlasState::Absent;
};

struct PtSkinnedBlasRetiredRecord
{
    PtCanonicalInstanceKey instanceKey;
    std::uint64_t blasGeneration = 0;
    std::uint64_t outputStorageGeneration = 0;
    std::uint64_t retirementToken = 0;
    std::uint64_t retiredFrame = 0;
};

struct PtSkinnedBlasStats
{
    std::uint64_t worldGeneration = 0;
    std::uint64_t activeRecords = 0;
    std::uint64_t buildPending = 0;
    std::uint64_t ready = 0;
    std::uint64_t updatePending = 0;
    std::uint64_t rebuildPending = 0;
    std::uint64_t failed = 0;
    std::uint64_t pendingRetirements = 0;

    std::uint64_t buildRequested = 0;
    std::uint64_t updateRequested = 0;
    std::uint64_t rebuildRequested = 0;
    std::uint64_t alreadyPending = 0;
    std::uint64_t submissionsSucceeded = 0;
    std::uint64_t submissionsFailed = 0;
    std::uint64_t retired = 0;
    std::uint64_t retirementsReleased = 0;
    std::uint64_t rejected = 0;
    std::uint64_t worldResets = 0;
};

class PtSkinnedBlasStateTable
{
public:
    explicit PtSkinnedBlasStateTable(
        const PtSkinnedBlasConfig& config =
            PtSkinnedBlasConfig());

    bool Configure(const PtSkinnedBlasConfig& config);
    void Clear();
    bool BeginWorld(
        std::uint64_t worldGeneration,
        std::uint64_t frameIndex);

    PtSkinnedBlasObserveResult Observe(
        const PtSkinnedBlasCandidate& candidate);
    PtSkinnedBlasSubmitResult MarkSubmitted(
        const PtCanonicalInstanceKey& instanceKey,
        PtSkinnedBlasAction action,
        bool succeeded,
        std::uint64_t frameIndex);
    PtSkinnedBlasRetireResult Retire(
        const PtCanonicalInstanceKey& instanceKey,
        std::uint64_t frameIndex);

    const PtSkinnedBlasRecord* Find(
        const PtCanonicalInstanceKey& instanceKey) const;
    std::size_t ReleaseRetirementsThrough(
        std::uint64_t completedRetirementToken);

    const std::vector<PtSkinnedBlasRetiredRecord>&
        PendingRetirements() const;
    const PtSkinnedBlasStats& Stats() const;
    void ResetIntervalStats();

private:
    PtSkinnedBlasRecord* FindMutable(
        const PtCanonicalInstanceKey& instanceKey,
        std::uint64_t instanceHash);
    bool CandidateValid(
        const PtSkinnedBlasCandidate& candidate,
        PtSkinnedBlasObserveResult& failure,
        std::uint64_t& sourceIndexBytes,
        std::uint64_t& outputVertexBytes) const;
    bool ContractChanged(
        const PtSkinnedBlasRecord& record,
        const PtSkinnedBlasCandidate& candidate) const;
    void CopyContract(
        PtSkinnedBlasRecord& record,
        const PtSkinnedBlasCandidate& candidate,
        std::uint64_t sourceIndexBytes,
        std::uint64_t outputVertexBytes);
    void SetState(
        PtSkinnedBlasRecord& record,
        PtSkinnedBlasState state);
    void RetireRecord(
        PtSkinnedBlasRecord& record,
        std::uint64_t frameIndex);

    PtSkinnedBlasConfig m_config;
    std::vector<PtSkinnedBlasRecord> m_records;
    std::unordered_multimap<std::uint64_t, std::size_t> m_lookup;
    std::vector<PtSkinnedBlasRetiredRecord> m_retired;
    PtSkinnedBlasStats m_stats;
    std::uint64_t m_nextBlasGeneration = 1;
    std::uint64_t m_nextRetirementToken = 1;
};

const char* PtSkinnedBlasStateName(PtSkinnedBlasState state);
const char* PtSkinnedBlasObserveResultName(
    PtSkinnedBlasObserveResult result);
