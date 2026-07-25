#include "PathTraceSkinnedBlasState.h"

#include <algorithm>
#include <limits>

namespace {

std::uint64_t NextNonZero(std::uint64_t& value)
{
    std::uint64_t result = value++;
    if (result == 0)
    {
        result = value++;
    }
    return result;
}

bool CheckedMultiply(
    std::uint64_t lhs,
    std::uint64_t rhs,
    std::uint64_t& result)
{
    if (lhs != 0 &&
        rhs > std::numeric_limits<std::uint64_t>::max() / lhs)
    {
        return false;
    }
    result = lhs * rhs;
    return true;
}

bool CheckedAdd(
    std::uint64_t lhs,
    std::uint64_t rhs,
    std::uint64_t& result)
{
    if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs)
    {
        return false;
    }
    result = lhs + rhs;
    return true;
}

} // namespace

PtSkinnedBlasStateTable::PtSkinnedBlasStateTable(
    const PtSkinnedBlasConfig& config)
    : m_config(config)
{
}

bool PtSkinnedBlasStateTable::Configure(
    const PtSkinnedBlasConfig& config)
{
    if (m_stats.worldGeneration != 0 ||
        !m_records.empty() ||
        !m_retired.empty() ||
        config.outputVertexStrideBytes == 0)
    {
        return false;
    }
    m_config = config;
    return true;
}

void PtSkinnedBlasStateTable::Clear()
{
    m_records.clear();
    m_lookup.clear();
    m_retired.clear();
    m_stats = PtSkinnedBlasStats();
    m_nextBlasGeneration = 1;
    m_nextRetirementToken = 1;
}

bool PtSkinnedBlasStateTable::BeginWorld(
    std::uint64_t worldGeneration,
    std::uint64_t frameIndex)
{
    if (worldGeneration == 0)
    {
        return false;
    }
    if (m_stats.worldGeneration == worldGeneration)
    {
        return true;
    }
    if (m_stats.worldGeneration != 0)
    {
        for (PtSkinnedBlasRecord& record : m_records)
        {
            if (record.state != PtSkinnedBlasState::Retiring)
            {
                RetireRecord(record, frameIndex);
            }
        }
        ++m_stats.worldResets;
    }
    m_records.clear();
    m_lookup.clear();
    m_stats.worldGeneration = worldGeneration;
    m_stats.activeRecords = 0;
    m_stats.buildPending = 0;
    m_stats.ready = 0;
    m_stats.updatePending = 0;
    m_stats.rebuildPending = 0;
    m_stats.failed = 0;
    m_stats.pendingRetirements = m_retired.size();
    return true;
}

PtSkinnedBlasObserveResult PtSkinnedBlasStateTable::Observe(
    const PtSkinnedBlasCandidate& candidate)
{
    PtSkinnedBlasObserveResult failure =
        PtSkinnedBlasObserveResult::InvalidWorld;
    std::uint64_t sourceIndexBytes = 0;
    std::uint64_t outputVertexBytes = 0;
    if (!CandidateValid(
            candidate,
            failure,
            sourceIndexBytes,
            outputVertexBytes))
    {
        ++m_stats.rejected;
        return failure;
    }

    const std::uint64_t instanceHash =
        PtHashCanonicalInstanceKey(candidate.instanceKey);
    PtSkinnedBlasRecord* record =
        FindMutable(candidate.instanceKey, instanceHash);
    if (record != nullptr &&
        record->state == PtSkinnedBlasState::Retiring)
    {
        ++m_stats.rejected;
        return PtSkinnedBlasObserveResult::RetiredInstanceKey;
    }
    if (record == nullptr)
    {
        PtSkinnedBlasRecord added;
        added.instanceKey = candidate.instanceKey;
        added.instanceHash = instanceHash;
        added.firstSeenFrame = candidate.frameIndex;
        added.stateRevision = 1;
        CopyContract(
            added,
            candidate,
            sourceIndexBytes,
            outputVertexBytes);
        const std::size_t recordIndex = m_records.size();
        m_records.push_back(added);
        m_lookup.emplace(instanceHash, recordIndex);
        ++m_stats.activeRecords;
        SetState(
            m_records.back(),
            PtSkinnedBlasState::BuildPending);
        ++m_stats.buildRequested;
        return PtSkinnedBlasObserveResult::BuildRequired;
    }

    record->lastSeenFrame = candidate.frameIndex;
    const bool contractChanged =
        ContractChanged(*record, candidate);
    if (record->state == PtSkinnedBlasState::BuildPending ||
        record->state == PtSkinnedBlasState::UpdatePending ||
        record->state == PtSkinnedBlasState::RebuildPending)
    {
        if (contractChanged)
        {
            CopyContract(
                *record,
                candidate,
                sourceIndexBytes,
                outputVertexBytes);
            if (record->state ==
                PtSkinnedBlasState::BuildPending)
            {
                ++record->stateRevision;
                ++m_stats.buildRequested;
                return
                    PtSkinnedBlasObserveResult::BuildRequired;
            }
            SetState(
                *record,
                PtSkinnedBlasState::RebuildPending);
            ++m_stats.rebuildRequested;
            return PtSkinnedBlasObserveResult::RebuildRequired;
        }
        ++m_stats.alreadyPending;
        return PtSkinnedBlasObserveResult::AlreadyPending;
    }

    CopyContract(
        *record,
        candidate,
        sourceIndexBytes,
        outputVertexBytes);
    const bool cadenceExpired =
        m_config.rebuildCadenceFrames != 0 &&
        record->lastFullBuildFrame != 0 &&
        candidate.frameIndex >= record->lastFullBuildFrame &&
        candidate.frameIndex - record->lastFullBuildFrame >=
            m_config.rebuildCadenceFrames;
    if (contractChanged ||
        candidate.forceRebuild ||
        cadenceExpired ||
        record->state == PtSkinnedBlasState::Failed)
    {
        SetState(*record, PtSkinnedBlasState::RebuildPending);
        ++m_stats.rebuildRequested;
        return PtSkinnedBlasObserveResult::RebuildRequired;
    }

    SetState(*record, PtSkinnedBlasState::UpdatePending);
    ++m_stats.updateRequested;
    return PtSkinnedBlasObserveResult::UpdateRequired;
}

PtSkinnedBlasSubmitResult
PtSkinnedBlasStateTable::MarkSubmitted(
    const PtCanonicalInstanceKey& instanceKey,
    PtSkinnedBlasAction action,
    bool succeeded,
    std::uint64_t frameIndex)
{
    PtSkinnedBlasRecord* record =
        FindMutable(
            instanceKey,
            PtHashCanonicalInstanceKey(instanceKey));
    if (record == nullptr ||
        record->state == PtSkinnedBlasState::Retiring)
    {
        return PtSkinnedBlasSubmitResult::NotFound;
    }
    const bool actionMatches =
        (action == PtSkinnedBlasAction::Build &&
            record->state == PtSkinnedBlasState::BuildPending) ||
        (action == PtSkinnedBlasAction::Update &&
            record->state == PtSkinnedBlasState::UpdatePending) ||
        (action == PtSkinnedBlasAction::Rebuild &&
            record->state == PtSkinnedBlasState::RebuildPending);
    if (!actionMatches)
    {
        return PtSkinnedBlasSubmitResult::ActionMismatch;
    }

    record->lastSeenFrame = frameIndex;
    if (!succeeded)
    {
        SetState(*record, PtSkinnedBlasState::Failed);
        ++m_stats.submissionsFailed;
        return PtSkinnedBlasSubmitResult::Failed;
    }

    if (action == PtSkinnedBlasAction::Build ||
        action == PtSkinnedBlasAction::Rebuild)
    {
        record->blasGeneration =
            NextNonZero(m_nextBlasGeneration);
        record->lastFullBuildFrame = frameIndex;
    }
    else
    {
        record->lastUpdateFrame = frameIndex;
    }
    SetState(*record, PtSkinnedBlasState::Ready);
    ++m_stats.submissionsSucceeded;
    return PtSkinnedBlasSubmitResult::Succeeded;
}

PtSkinnedBlasRetireResult PtSkinnedBlasStateTable::Retire(
    const PtCanonicalInstanceKey& instanceKey,
    std::uint64_t frameIndex)
{
    if (!PtCanonicalInstanceKeyIsValid(instanceKey))
    {
        return PtSkinnedBlasRetireResult::InvalidInstanceKey;
    }
    PtSkinnedBlasRecord* record =
        FindMutable(
            instanceKey,
            PtHashCanonicalInstanceKey(instanceKey));
    if (record == nullptr)
    {
        return PtSkinnedBlasRetireResult::NotFound;
    }
    if (record->state == PtSkinnedBlasState::Retiring)
    {
        return PtSkinnedBlasRetireResult::AlreadyRetired;
    }
    RetireRecord(*record, frameIndex);
    return PtSkinnedBlasRetireResult::Retired;
}

const PtSkinnedBlasRecord* PtSkinnedBlasStateTable::Find(
    const PtCanonicalInstanceKey& instanceKey) const
{
    const std::uint64_t hash =
        PtHashCanonicalInstanceKey(instanceKey);
    const auto range = m_lookup.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it)
    {
        if (it->second < m_records.size())
        {
            const PtSkinnedBlasRecord& record =
                m_records[it->second];
            if (record.instanceKey == instanceKey &&
                record.state != PtSkinnedBlasState::Retiring)
            {
                return &record;
            }
        }
    }
    return nullptr;
}

std::size_t PtSkinnedBlasStateTable::ReleaseRetirementsThrough(
    std::uint64_t completedRetirementToken)
{
    const std::size_t before = m_retired.size();
    m_retired.erase(
        std::remove_if(
            m_retired.begin(),
            m_retired.end(),
            [completedRetirementToken](
                const PtSkinnedBlasRetiredRecord& retired)
            {
                return retired.retirementToken <=
                    completedRetirementToken;
            }),
        m_retired.end());
    const std::size_t released =
        before - m_retired.size();
    m_stats.retirementsReleased += released;
    m_stats.pendingRetirements = m_retired.size();
    return released;
}

const std::vector<PtSkinnedBlasRetiredRecord>&
PtSkinnedBlasStateTable::PendingRetirements() const
{
    return m_retired;
}

const PtSkinnedBlasStats& PtSkinnedBlasStateTable::Stats() const
{
    return m_stats;
}

void PtSkinnedBlasStateTable::ResetIntervalStats()
{
    m_stats.buildRequested = 0;
    m_stats.updateRequested = 0;
    m_stats.rebuildRequested = 0;
    m_stats.alreadyPending = 0;
    m_stats.submissionsSucceeded = 0;
    m_stats.submissionsFailed = 0;
    m_stats.retired = 0;
    m_stats.retirementsReleased = 0;
    m_stats.rejected = 0;
    m_stats.worldResets = 0;
}

PtSkinnedBlasRecord* PtSkinnedBlasStateTable::FindMutable(
    const PtCanonicalInstanceKey& instanceKey,
    std::uint64_t instanceHash)
{
    const auto range = m_lookup.equal_range(instanceHash);
    for (auto it = range.first; it != range.second; ++it)
    {
        if (it->second < m_records.size() &&
            m_records[it->second].instanceKey == instanceKey)
        {
            return &m_records[it->second];
        }
    }
    return nullptr;
}

bool PtSkinnedBlasStateTable::CandidateValid(
    const PtSkinnedBlasCandidate& candidate,
    PtSkinnedBlasObserveResult& failure,
    std::uint64_t& sourceIndexBytes,
    std::uint64_t& outputVertexBytes) const
{
    if (m_stats.worldGeneration == 0 ||
        candidate.instanceKey.worldGeneration !=
            m_stats.worldGeneration)
    {
        failure = PtSkinnedBlasObserveResult::InvalidWorld;
        return false;
    }
    if (!PtCanonicalInstanceKeyIsValid(candidate.instanceKey) ||
        candidate.instanceKey.subInstanceKind !=
            PtCanonicalSubInstanceKind::SkinnedSurface)
    {
        failure =
            PtSkinnedBlasObserveResult::InvalidInstanceKey;
        return false;
    }
    if (!PtCanonicalMeshKeyIsValid(candidate.meshKey) ||
        candidate.meshKey.sourceDomain !=
            PtCanonicalMeshSourceDomain::SkinnedBindSource ||
        candidate.meshKey.deformationClass !=
            PtCanonicalDeformationClass::Skinned ||
        candidate.meshKey.modelSurfaceIndex !=
            candidate.instanceKey.modelSurfaceIndex ||
        candidate.meshKey.vertexCount !=
            candidate.outputVertexCount)
    {
        failure = PtSkinnedBlasObserveResult::InvalidMeshKey;
        return false;
    }
    if (candidate.sourceChecksum == 0)
    {
        failure =
            PtSkinnedBlasObserveResult::InvalidSourceChecksum;
        return false;
    }
    if (candidate.sourceGpuIndexGeneration == 0)
    {
        failure =
            PtSkinnedBlasObserveResult::
                InvalidSourceGpuGeneration;
        return false;
    }
    std::uint64_t sourceIndexEnd = 0;
    if (candidate.meshKey.indexCount == 0 ||
        candidate.sourceIndexOffsetBytes % sizeof(std::uint32_t) != 0 ||
        !CheckedMultiply(
            candidate.meshKey.indexCount,
            sizeof(std::uint32_t),
            sourceIndexBytes) ||
        !CheckedAdd(
            candidate.sourceIndexOffsetBytes,
            sourceIndexBytes,
            sourceIndexEnd) ||
        sourceIndexEnd > candidate.sourceIndexCapacityBytes)
    {
        failure =
            PtSkinnedBlasObserveResult::InvalidSourceIndexRange;
        return false;
    }
    if (candidate.outputStorageGeneration == 0)
    {
        failure =
            PtSkinnedBlasObserveResult::
                InvalidOutputStorageGeneration;
        return false;
    }
    std::uint64_t outputEnd = 0;
    if (m_config.outputVertexStrideBytes == 0 ||
        candidate.outputVertexCount == 0 ||
        candidate.outputVertexCount > UINT32_MAX ||
        candidate.outputVertexOffsetBytes %
                m_config.outputVertexStrideBytes !=
            0 ||
        !CheckedMultiply(
            candidate.outputVertexCount,
            m_config.outputVertexStrideBytes,
            outputVertexBytes) ||
        !CheckedAdd(
            candidate.outputVertexOffsetBytes,
            outputVertexBytes,
            outputEnd) ||
        outputEnd > candidate.outputCapacityBytes)
    {
        failure = PtSkinnedBlasObserveResult::InvalidOutputRange;
        return false;
    }
    if (!candidate.dispatchReady)
    {
        failure = PtSkinnedBlasObserveResult::DispatchNotReady;
        return false;
    }
    return true;
}

bool PtSkinnedBlasStateTable::ContractChanged(
    const PtSkinnedBlasRecord& record,
    const PtSkinnedBlasCandidate& candidate) const
{
    return
        record.meshKey != candidate.meshKey ||
        record.sourceChecksum != candidate.sourceChecksum ||
        record.sourceGpuIndexGeneration !=
            candidate.sourceGpuIndexGeneration ||
        record.sourceIndexOffsetBytes !=
            candidate.sourceIndexOffsetBytes ||
        record.outputStorageGeneration !=
            candidate.outputStorageGeneration ||
        record.outputVertexOffsetBytes !=
            candidate.outputVertexOffsetBytes ||
        record.outputVertexCount !=
            candidate.outputVertexCount;
}

void PtSkinnedBlasStateTable::CopyContract(
    PtSkinnedBlasRecord& record,
    const PtSkinnedBlasCandidate& candidate,
    std::uint64_t sourceIndexBytes,
    std::uint64_t outputVertexBytes)
{
    record.meshKey = candidate.meshKey;
    record.sourceChecksum = candidate.sourceChecksum;
    record.sourceGpuIndexGeneration =
        candidate.sourceGpuIndexGeneration;
    record.sourceIndexOffsetBytes =
        candidate.sourceIndexOffsetBytes;
    record.sourceIndexBytes = sourceIndexBytes;
    record.outputStorageGeneration =
        candidate.outputStorageGeneration;
    record.outputVertexOffsetBytes =
        candidate.outputVertexOffsetBytes;
    record.outputVertexCount = candidate.outputVertexCount;
    record.outputVertexBytes = outputVertexBytes;
    record.lastSeenFrame = candidate.frameIndex;
}

void PtSkinnedBlasStateTable::SetState(
    PtSkinnedBlasRecord& record,
    PtSkinnedBlasState state)
{
    auto subtractState = [this](PtSkinnedBlasState oldState)
    {
        switch (oldState)
        {
            case PtSkinnedBlasState::BuildPending:
                --m_stats.buildPending;
                break;
            case PtSkinnedBlasState::Ready:
                --m_stats.ready;
                break;
            case PtSkinnedBlasState::UpdatePending:
                --m_stats.updatePending;
                break;
            case PtSkinnedBlasState::RebuildPending:
                --m_stats.rebuildPending;
                break;
            case PtSkinnedBlasState::Failed:
                --m_stats.failed;
                break;
            default:
                break;
        }
    };
    auto addState = [this](PtSkinnedBlasState newState)
    {
        switch (newState)
        {
            case PtSkinnedBlasState::BuildPending:
                ++m_stats.buildPending;
                break;
            case PtSkinnedBlasState::Ready:
                ++m_stats.ready;
                break;
            case PtSkinnedBlasState::UpdatePending:
                ++m_stats.updatePending;
                break;
            case PtSkinnedBlasState::RebuildPending:
                ++m_stats.rebuildPending;
                break;
            case PtSkinnedBlasState::Failed:
                ++m_stats.failed;
                break;
            default:
                break;
        }
    };

    if (record.state == state)
    {
        return;
    }
    subtractState(record.state);
    record.state = state;
    addState(state);
    ++record.stateRevision;
}

void PtSkinnedBlasStateTable::RetireRecord(
    PtSkinnedBlasRecord& record,
    std::uint64_t frameIndex)
{
    PtSkinnedBlasRetiredRecord retired;
    retired.instanceKey = record.instanceKey;
    retired.blasGeneration = record.blasGeneration;
    retired.outputStorageGeneration =
        record.outputStorageGeneration;
    retired.retirementToken =
        NextNonZero(m_nextRetirementToken);
    retired.retiredFrame = frameIndex;
    m_retired.push_back(retired);
    SetState(record, PtSkinnedBlasState::Retiring);
    --m_stats.activeRecords;
    ++m_stats.retired;
    m_stats.pendingRetirements = m_retired.size();
}

const char* PtSkinnedBlasStateName(PtSkinnedBlasState state)
{
    switch (state)
    {
        case PtSkinnedBlasState::Absent: return "absent";
        case PtSkinnedBlasState::BuildPending:
            return "build-pending";
        case PtSkinnedBlasState::Ready: return "ready";
        case PtSkinnedBlasState::UpdatePending:
            return "update-pending";
        case PtSkinnedBlasState::RebuildPending:
            return "rebuild-pending";
        case PtSkinnedBlasState::Failed: return "failed";
        case PtSkinnedBlasState::Retiring: return "retiring";
        default: return "unknown";
    }
}

const char* PtSkinnedBlasObserveResultName(
    PtSkinnedBlasObserveResult result)
{
    switch (result)
    {
        case PtSkinnedBlasObserveResult::BuildRequired:
            return "build-required";
        case PtSkinnedBlasObserveResult::UpdateRequired:
            return "update-required";
        case PtSkinnedBlasObserveResult::RebuildRequired:
            return "rebuild-required";
        case PtSkinnedBlasObserveResult::AlreadyPending:
            return "already-pending";
        case PtSkinnedBlasObserveResult::InvalidWorld:
            return "invalid-world";
        case PtSkinnedBlasObserveResult::InvalidInstanceKey:
            return "invalid-instance";
        case PtSkinnedBlasObserveResult::InvalidMeshKey:
            return "invalid-mesh";
        case PtSkinnedBlasObserveResult::InvalidSourceChecksum:
            return "invalid-source-checksum";
        case PtSkinnedBlasObserveResult::
                InvalidSourceGpuGeneration:
            return "invalid-source-gpu-generation";
        case PtSkinnedBlasObserveResult::InvalidSourceIndexRange:
            return "invalid-source-index-range";
        case PtSkinnedBlasObserveResult::
                InvalidOutputStorageGeneration:
            return "invalid-output-storage-generation";
        case PtSkinnedBlasObserveResult::InvalidOutputRange:
            return "invalid-output-range";
        case PtSkinnedBlasObserveResult::DispatchNotReady:
            return "dispatch-not-ready";
        case PtSkinnedBlasObserveResult::RetiredInstanceKey:
            return "retired-instance";
        default:
            return "unknown";
    }
}
