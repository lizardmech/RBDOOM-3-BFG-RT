#pragma once

// Immutable, revisioned canonical instance-to-mesh identity transport.
//
// The frontend lifecycle registry owns the journal. The backend consumes
// bounded frame packets and maintains a pointer-free lookup of the latest
// binding for each full canonical instance key.

#include "PathTraceCanonicalGeometryIdentity.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

enum class PtGeometryIdentityOperation : std::uint32_t
{
    Invalid = 0,
    Upsert,
    Remove
};

struct PtGeometryIdentityTransportRecord
{
    PtGeometryIdentityOperation operation =
        PtGeometryIdentityOperation::Invalid;
    std::uint64_t eventSequence = 0;
    PtCanonicalInstanceKey instanceKey;
    std::uint64_t instanceHash = 0;
    PtCanonicalMeshKey meshKey;
    std::uint64_t meshHash = 0;
};

struct PtGeometryIdentityTransportPlan
{
    std::size_t firstRecordIndex = 0;
    std::size_t nextRecordIndex = 0;
    bool complete = false;
    std::uint64_t packedBytes = 0;
    std::vector<PtGeometryIdentityTransportRecord> records;
};

struct PtGeometryIdentityTransportSnapshot
{
    std::uint64_t worldGeneration = 0;
    std::uint64_t publicationGeneration = 0;
    std::uint64_t publicationSequence = 0;
    std::uint64_t firstRecordIndex = 0;
    std::uint64_t nextRecordIndex = 0;
    std::uint64_t packedBytes = 0;
    std::uint64_t recordCount = 0;
    const PtGeometryIdentityTransportRecord* records = nullptr;
};

// A8-S1 always-present, frame-owned Present witness. Unlike the optional
// transport delta above, this DTO exists on every primary S1 view, including
// EmptyDelta frames. modelName is stored in frame-owned bytes by offset/length;
// no idStr or live model/shadow pointer crosses to the backend.
struct PtGeometryPresentIdentityRecord
{
    std::uint32_t valid = 0;
    PtCanonicalInstanceKey instanceKey;
    std::uint64_t instanceHash = 0;
    PtCanonicalMeshKey meshKey;
    std::uint64_t meshHash = 0;
    std::uint64_t lastUpsertSequence = 0;
    std::uint32_t modelNameOffset = 0;
    std::uint32_t modelNameLength = 0;
};

struct PtGeometryPresentIdentitySnapshot
{
    std::uint32_t available = 0;
    std::uint64_t worldGeneration = 0;
    std::uint64_t publicationGeneration = 0;
    std::uint64_t recordCount = 0;
    std::uint64_t modelNameBytes = 0;
    std::uint64_t packedBytes = 0;
    std::uint64_t captureMicroseconds = 0;
    const PtGeometryPresentIdentityRecord* records = nullptr;
    const char* modelNames = nullptr;
};

enum class PtGeometryIdentityTransportResult : std::uint32_t
{
    Success = 0,
    EmptyDelta,
    InvalidFirstRecord,
    InvalidBudget,
    RecordExceedsBudget,
    ArithmeticOverflow,
    InvalidSnapshot,
    InvalidRecord,
    HashMismatch,
    SequenceMismatch,
    StalePublication
};

PtGeometryIdentityTransportResult PtPlanGeometryIdentityTransport(
    const std::vector<PtGeometryIdentityTransportRecord>& journal,
    std::size_t firstRecordIndex,
    std::uint64_t byteBudget,
    PtGeometryIdentityTransportPlan& plan);

PtGeometryIdentityTransportResult PtValidateGeometryIdentityTransportRecord(
    const PtGeometryIdentityTransportRecord& record);

const char* PtGeometryIdentityTransportResultName(
    PtGeometryIdentityTransportResult result);

struct PtGeometryIdentityBinding
{
    bool active = false;
    PtCanonicalInstanceKey instanceKey;
    std::uint64_t instanceHash = 0;
    PtCanonicalMeshKey meshKey;
    std::uint64_t meshHash = 0;
    std::uint64_t lastEventSequence = 0;
};

struct PtGeometryIdentityRegistryStats
{
    std::uint64_t worldGeneration = 0;
    std::uint64_t publicationGeneration = 0;
    std::uint64_t publicationSequence = 0;
    std::uint64_t importCursor = 0;
    std::uint64_t activeBindings = 0;
    std::uint64_t activeStaticBindings = 0;
    std::uint64_t activeRigidBindings = 0;
    std::uint64_t activeSkinnedBindings = 0;
    std::uint64_t bindingSlots = 0;
    std::uint64_t upserts = 0;
    std::uint64_t reused = 0;
    std::uint64_t revised = 0;
    std::uint64_t removed = 0;
    std::uint64_t rejected = 0;
    std::uint64_t transportBytes = 0;
};

class PtGeometryIdentityRegistry
{
public:
    void Clear();

    PtGeometryIdentityTransportResult ApplySnapshot(
        const PtGeometryIdentityTransportSnapshot* snapshot);

    const PtGeometryIdentityBinding* Find(
        const PtCanonicalInstanceKey& key) const;

    const PtGeometryIdentityRegistryStats& Stats() const;
    void ResetIntervalStats();

private:
    PtGeometryIdentityBinding* FindMutable(
        const PtCanonicalInstanceKey& key,
        std::uint64_t hash);

    std::vector<PtGeometryIdentityBinding> m_bindings;
    std::unordered_multimap<std::uint64_t, std::size_t> m_lookup;
    PtGeometryIdentityRegistryStats m_stats;
};
