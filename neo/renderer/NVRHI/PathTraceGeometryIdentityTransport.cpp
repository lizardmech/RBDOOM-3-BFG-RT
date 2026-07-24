#include "PathTraceGeometryIdentityTransport.h"

namespace {

constexpr std::uint64_t kTransportAllocationAlignment = 128;

bool CheckedAlign(
    std::uint64_t value,
    std::uint64_t alignment,
    std::uint64_t& aligned)
{
    const std::uint64_t remainder = value % alignment;
    const std::uint64_t padding =
        remainder == 0 ? 0 : alignment - remainder;
    return PtCheckedAddU64(value, padding, aligned);
}

bool CalculatePackedBytes(
    std::uint64_t recordCount,
    std::uint64_t& packedBytes)
{
    std::uint64_t snapshotBytes = 0;
    std::uint64_t recordBytes = 0;
    std::uint64_t alignedRecordBytes = 0;
    return CheckedAlign(
            sizeof(PtGeometryIdentityTransportSnapshot),
            kTransportAllocationAlignment,
            snapshotBytes) &&
        PtCheckedMulU64(
            recordCount,
            sizeof(PtGeometryIdentityTransportRecord),
            recordBytes) &&
        CheckedAlign(
            recordBytes,
            kTransportAllocationAlignment,
            alignedRecordBytes) &&
        PtCheckedAddU64(
            snapshotBytes,
            alignedRecordBytes,
            packedBytes);
}

void AdjustActiveKind(
    PtGeometryIdentityRegistryStats& stats,
    PtCanonicalSubInstanceKind kind,
    bool added)
{
    std::uint64_t* count = nullptr;
    switch (kind)
    {
        case PtCanonicalSubInstanceKind::StaticSurface:
            count = &stats.activeStaticBindings;
            break;
        case PtCanonicalSubInstanceKind::RigidSurface:
            count = &stats.activeRigidBindings;
            break;
        case PtCanonicalSubInstanceKind::SkinnedSurface:
        case PtCanonicalSubInstanceKind::JointSubmesh:
            count = &stats.activeSkinnedBindings;
            break;
        default:
            break;
    }
    if (count != nullptr)
    {
        if (added)
        {
            ++(*count);
        }
        else if (*count > 0)
        {
            --(*count);
        }
    }
}

}

PtGeometryIdentityTransportResult PtPlanGeometryIdentityTransport(
    const std::vector<PtGeometryIdentityTransportRecord>& journal,
    std::size_t firstRecordIndex,
    std::uint64_t byteBudget,
    PtGeometryIdentityTransportPlan& plan)
{
    plan = PtGeometryIdentityTransportPlan();
    plan.firstRecordIndex = firstRecordIndex;
    plan.nextRecordIndex = firstRecordIndex;
    if (firstRecordIndex > journal.size())
    {
        return PtGeometryIdentityTransportResult::InvalidFirstRecord;
    }
    if (byteBudget == 0)
    {
        return PtGeometryIdentityTransportResult::InvalidBudget;
    }
    if (firstRecordIndex == journal.size())
    {
        plan.complete = true;
        return PtGeometryIdentityTransportResult::EmptyDelta;
    }

    for (std::size_t recordIndex = firstRecordIndex;
        recordIndex < journal.size();
        ++recordIndex)
    {
        if (PtValidateGeometryIdentityTransportRecord(journal[recordIndex]) !=
            PtGeometryIdentityTransportResult::Success)
        {
            plan = PtGeometryIdentityTransportPlan();
            return PtGeometryIdentityTransportResult::InvalidRecord;
        }
        std::uint64_t candidateBytes = 0;
        if (!CalculatePackedBytes(
                static_cast<std::uint64_t>(plan.records.size()) + 1,
                candidateBytes))
        {
            plan = PtGeometryIdentityTransportPlan();
            return PtGeometryIdentityTransportResult::ArithmeticOverflow;
        }
        if (candidateBytes > byteBudget)
        {
            if (plan.records.empty())
            {
                plan.firstRecordIndex = firstRecordIndex;
                plan.nextRecordIndex = firstRecordIndex;
                return PtGeometryIdentityTransportResult::RecordExceedsBudget;
            }
            break;
        }
        plan.records.push_back(journal[recordIndex]);
        plan.packedBytes = candidateBytes;
        plan.nextRecordIndex = recordIndex + 1;
    }
    plan.complete = plan.nextRecordIndex == journal.size();
    return PtGeometryIdentityTransportResult::Success;
}

PtGeometryIdentityTransportResult PtValidateGeometryIdentityTransportRecord(
    const PtGeometryIdentityTransportRecord& record)
{
    if ((record.operation != PtGeometryIdentityOperation::Upsert &&
         record.operation != PtGeometryIdentityOperation::Remove) ||
        record.eventSequence == 0 ||
        !PtCanonicalInstanceKeyIsValid(record.instanceKey) ||
        !PtCanonicalMeshKeyIsValid(record.meshKey))
    {
        return PtGeometryIdentityTransportResult::InvalidRecord;
    }
    if (record.instanceHash !=
            PtHashCanonicalInstanceKey(record.instanceKey) ||
        record.meshHash != PtHashCanonicalMeshKey(record.meshKey))
    {
        return PtGeometryIdentityTransportResult::HashMismatch;
    }
    return PtGeometryIdentityTransportResult::Success;
}

const char* PtGeometryIdentityTransportResultName(
    PtGeometryIdentityTransportResult result)
{
    switch (result)
    {
        case PtGeometryIdentityTransportResult::Success: return "success";
        case PtGeometryIdentityTransportResult::EmptyDelta: return "empty_delta";
        case PtGeometryIdentityTransportResult::InvalidFirstRecord: return "invalid_first_record";
        case PtGeometryIdentityTransportResult::InvalidBudget: return "invalid_budget";
        case PtGeometryIdentityTransportResult::RecordExceedsBudget: return "record_exceeds_budget";
        case PtGeometryIdentityTransportResult::ArithmeticOverflow: return "arithmetic_overflow";
        case PtGeometryIdentityTransportResult::InvalidSnapshot: return "invalid_snapshot";
        case PtGeometryIdentityTransportResult::InvalidRecord: return "invalid_record";
        case PtGeometryIdentityTransportResult::HashMismatch: return "hash_mismatch";
        case PtGeometryIdentityTransportResult::SequenceMismatch: return "sequence_mismatch";
        case PtGeometryIdentityTransportResult::StalePublication: return "stale_publication";
    }
    return "unknown";
}

void PtGeometryIdentityRegistry::Clear()
{
    m_bindings.clear();
    m_lookup.clear();
    m_stats = PtGeometryIdentityRegistryStats();
}

PtGeometryIdentityTransportResult PtGeometryIdentityRegistry::ApplySnapshot(
    const PtGeometryIdentityTransportSnapshot* snapshot)
{
    if (snapshot == nullptr ||
        snapshot->worldGeneration == 0 ||
        snapshot->publicationGeneration == 0 ||
        snapshot->publicationSequence == 0 ||
        snapshot->records == nullptr ||
        snapshot->recordCount == 0 ||
        snapshot->nextRecordIndex < snapshot->firstRecordIndex ||
        snapshot->nextRecordIndex - snapshot->firstRecordIndex !=
            snapshot->recordCount)
    {
        ++m_stats.rejected;
        return PtGeometryIdentityTransportResult::InvalidSnapshot;
    }

    for (std::uint64_t index = 0; index < snapshot->recordCount; ++index)
    {
        const PtGeometryIdentityTransportRecord& record =
            snapshot->records[index];
        const PtGeometryIdentityTransportResult validation =
            PtValidateGeometryIdentityTransportRecord(record);
        if (validation != PtGeometryIdentityTransportResult::Success)
        {
            ++m_stats.rejected;
            return validation;
        }
        if (record.eventSequence !=
            snapshot->firstRecordIndex + index + 1)
        {
            ++m_stats.rejected;
            return PtGeometryIdentityTransportResult::SequenceMismatch;
        }
    }

    const bool newPublication =
        m_stats.worldGeneration != snapshot->worldGeneration ||
        m_stats.publicationGeneration != snapshot->publicationGeneration;
    if ((m_stats.worldGeneration != 0 &&
         snapshot->worldGeneration < m_stats.worldGeneration) ||
        (m_stats.worldGeneration == snapshot->worldGeneration &&
         m_stats.publicationGeneration != 0 &&
         snapshot->publicationGeneration <
            m_stats.publicationGeneration))
    {
        ++m_stats.rejected;
        return PtGeometryIdentityTransportResult::StalePublication;
    }
    if ((newPublication && snapshot->firstRecordIndex != 0) ||
        (!newPublication &&
         (snapshot->publicationSequence <= m_stats.publicationSequence ||
          snapshot->firstRecordIndex != m_stats.importCursor)))
    {
        ++m_stats.rejected;
        return newPublication
            ? PtGeometryIdentityTransportResult::SequenceMismatch
            : PtGeometryIdentityTransportResult::StalePublication;
    }

    if (newPublication)
    {
        m_bindings.clear();
        m_lookup.clear();
        m_stats.worldGeneration = snapshot->worldGeneration;
        m_stats.publicationGeneration = snapshot->publicationGeneration;
        m_stats.publicationSequence = 0;
        m_stats.importCursor = 0;
        m_stats.activeBindings = 0;
        m_stats.activeStaticBindings = 0;
        m_stats.activeRigidBindings = 0;
        m_stats.activeSkinnedBindings = 0;
        m_stats.bindingSlots = 0;
    }

    for (std::uint64_t index = 0; index < snapshot->recordCount; ++index)
    {
        const PtGeometryIdentityTransportRecord& transport =
            snapshot->records[index];
        PtGeometryIdentityBinding* binding =
            FindMutable(transport.instanceKey, transport.instanceHash);
        if (transport.operation == PtGeometryIdentityOperation::Upsert)
        {
            if (binding == nullptr)
            {
                PtGeometryIdentityBinding added;
                added.active = true;
                added.instanceKey = transport.instanceKey;
                added.instanceHash = transport.instanceHash;
                added.meshKey = transport.meshKey;
                added.meshHash = transport.meshHash;
                added.lastEventSequence = transport.eventSequence;
                const std::size_t bindingIndex = m_bindings.size();
                m_bindings.push_back(added);
                m_lookup.emplace(transport.instanceHash, bindingIndex);
                ++m_stats.activeBindings;
                AdjustActiveKind(
                    m_stats,
                    transport.instanceKey.subInstanceKind,
                    true);
                ++m_stats.upserts;
            }
            else
            {
                if (!binding->active)
                {
                    binding->active = true;
                    ++m_stats.activeBindings;
                    AdjustActiveKind(
                        m_stats,
                        transport.instanceKey.subInstanceKind,
                        true);
                    ++m_stats.upserts;
                }
                else if (binding->meshKey == transport.meshKey)
                {
                    ++m_stats.reused;
                }
                else
                {
                    ++m_stats.revised;
                }
                binding->meshKey = transport.meshKey;
                binding->meshHash = transport.meshHash;
                binding->lastEventSequence = transport.eventSequence;
            }
        }
        else if (binding != nullptr && binding->active)
        {
            binding->active = false;
            binding->lastEventSequence = transport.eventSequence;
            --m_stats.activeBindings;
            AdjustActiveKind(
                m_stats,
                transport.instanceKey.subInstanceKind,
                false);
            ++m_stats.removed;
        }
    }

    m_stats.publicationSequence = snapshot->publicationSequence;
    m_stats.importCursor = snapshot->nextRecordIndex;
    m_stats.bindingSlots = m_bindings.size();
    m_stats.transportBytes += snapshot->packedBytes;
    return PtGeometryIdentityTransportResult::Success;
}

const PtGeometryIdentityBinding* PtGeometryIdentityRegistry::Find(
    const PtCanonicalInstanceKey& key) const
{
    const std::uint64_t hash = PtHashCanonicalInstanceKey(key);
    const auto range = m_lookup.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it)
    {
        if (it->second < m_bindings.size() &&
            m_bindings[it->second].active &&
            m_bindings[it->second].instanceKey == key)
        {
            return &m_bindings[it->second];
        }
    }
    return nullptr;
}

const PtGeometryIdentityRegistryStats&
PtGeometryIdentityRegistry::Stats() const
{
    return m_stats;
}

void PtGeometryIdentityRegistry::ResetIntervalStats()
{
    m_stats.upserts = 0;
    m_stats.reused = 0;
    m_stats.revised = 0;
    m_stats.removed = 0;
    m_stats.rejected = 0;
    m_stats.transportBytes = 0;
}

PtGeometryIdentityBinding* PtGeometryIdentityRegistry::FindMutable(
    const PtCanonicalInstanceKey& key,
    std::uint64_t hash)
{
    const auto range = m_lookup.equal_range(hash);
    for (auto it = range.first; it != range.second; ++it)
    {
        if (it->second < m_bindings.size() &&
            m_bindings[it->second].instanceKey == key)
        {
            return &m_bindings[it->second];
        }
    }
    return nullptr;
}
