#include "PathTraceGeometrySourceRegistry.h"

#include <utility>

namespace {

constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

void HashBytes(std::uint64_t& hash, const void* data, std::size_t size)
{
    const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= kFnvPrime;
    }
}

template<typename T>
void HashValue(std::uint64_t& hash, const T& value)
{
    HashBytes(hash, &value, sizeof(value));
}

bool AddStreamBytes(
    std::uint64_t count,
    std::uint64_t stride,
    std::uint64_t& total)
{
    std::uint64_t streamBytes = 0;
    std::uint64_t nextTotal = 0;
    if (!PtCheckedMulU64(count, stride, streamBytes) ||
        !PtCheckedAddU64(total, streamBytes, nextTotal))
    {
        return false;
    }
    total = nextTotal;
    return true;
}

void CountRejected(PtGeometrySourceRegistryStats& stats)
{
    ++stats.rejected;
}

}

PtGeometrySourceObserveResult PtValidateGeometrySourcePayload(
    const PtCanonicalMeshKey& key,
    const PtGeometrySourcePayloadView& payload,
    std::uint64_t& retainedBytes)
{
    retainedBytes = 0;

    if (payload.positionCount != key.vertexCount ||
        payload.attributeCount != key.vertexCount ||
        payload.indexCount != key.indexCount)
    {
        return PtGeometrySourceObserveResult::CountMismatch;
    }
    if ((payload.indexCount % 3ull) != 0 ||
        payload.triangleCount != payload.indexCount / 3ull)
    {
        return PtGeometrySourceObserveResult::NonTriangleTopology;
    }
    if ((payload.positionCount != 0 && payload.positions == nullptr) ||
        (payload.attributeCount != 0 && payload.attributes == nullptr) ||
        (payload.indexCount != 0 && payload.indexes == nullptr) ||
        (payload.triangleCount != 0 && payload.triangles == nullptr))
    {
        return PtGeometrySourceObserveResult::MissingPayload;
    }

    for (std::uint64_t index = 0; index < payload.indexCount; ++index)
    {
        if (payload.indexes[index] >= payload.positionCount)
        {
            return PtGeometrySourceObserveResult::IndexOutOfRange;
        }
    }

    if (!AddStreamBytes(
            payload.positionCount,
            sizeof(PtGeometrySourcePosition),
            retainedBytes) ||
        !AddStreamBytes(
            payload.attributeCount,
            sizeof(PtGeometrySourceAttribute),
            retainedBytes) ||
        !AddStreamBytes(
            payload.indexCount,
            sizeof(std::uint32_t),
            retainedBytes) ||
        !AddStreamBytes(
            payload.triangleCount,
            sizeof(PtGeometrySourceTriangle),
            retainedBytes))
    {
        retainedBytes = 0;
        return PtGeometrySourceObserveResult::ArithmeticOverflow;
    }

    return PtGeometrySourceObserveResult::Added;
}

std::uint64_t PtChecksumGeometrySourcePayload(
    const PtGeometrySourcePayloadView& payload)
{
    std::uint64_t hash = kFnvOffsetBasis;
    HashValue(hash, payload.positionCount);
    HashValue(hash, payload.attributeCount);
    HashValue(hash, payload.indexCount);
    HashValue(hash, payload.triangleCount);

    for (std::uint64_t vertex = 0; vertex < payload.positionCount; ++vertex)
    {
        HashBytes(hash, payload.positions[vertex].xyz, sizeof(payload.positions[vertex].xyz));
    }
    for (std::uint64_t vertex = 0; vertex < payload.attributeCount; ++vertex)
    {
        const PtGeometrySourceAttribute& attribute = payload.attributes[vertex];
        HashBytes(hash, attribute.normal, sizeof(attribute.normal));
        HashBytes(hash, attribute.texCoord, sizeof(attribute.texCoord));
        HashBytes(hash, attribute.color, sizeof(attribute.color));
        HashBytes(hash, attribute.color2, sizeof(attribute.color2));
        HashBytes(hash, attribute.tangent, sizeof(attribute.tangent));
        HashBytes(hash, attribute.bitangent, sizeof(attribute.bitangent));
        HashValue(hash, attribute.bitangentSign);
    }
    for (std::uint64_t index = 0; index < payload.indexCount; ++index)
    {
        HashValue(hash, payload.indexes[index]);
    }
    for (std::uint64_t triangle = 0; triangle < payload.triangleCount; ++triangle)
    {
        const PtGeometrySourceTriangle& metadata = payload.triangles[triangle];
        HashValue(hash, metadata.sourceMaterialSlot);
        HashValue(hash, metadata.geometryLocalFlags);
        HashValue(hash, metadata.sourceEmissivePrimitive);
        HashValue(hash, metadata.reserved);
    }
    return hash;
}

PtGeometrySourceRecord* PtGeometrySourceRegistry::FindMutable(
    const PtCanonicalMeshKey& key,
    std::uint64_t meshHash,
    bool& hashCollision)
{
    hashCollision = false;
    const std::pair<
        std::unordered_multimap<std::uint64_t, std::size_t>::iterator,
        std::unordered_multimap<std::uint64_t, std::size_t>::iterator> range =
            lookup_.equal_range(meshHash);
    for (std::unordered_multimap<std::uint64_t, std::size_t>::iterator it =
            range.first;
        it != range.second;
        ++it)
    {
        if (it->second >= records_.size())
        {
            continue;
        }
        PtGeometrySourceRecord& record = records_[it->second];
        if (record.key == key)
        {
            return &record;
        }
        hashCollision = true;
    }
    return nullptr;
}

PtGeometrySourceObserveResult PtGeometrySourceRegistry::Observe(
    const PtCanonicalMeshKey& key,
    std::uint64_t sourceContentRevision,
    const PtGeometrySourcePayloadView* payload)
{
    if (!PtCanonicalMeshKeyIsValid(key))
    {
        CountRejected(stats_);
        return PtGeometrySourceObserveResult::InvalidMeshKey;
    }
    if (key.sourceDomain != PtCanonicalMeshSourceDomain::RegisteredRenderModel)
    {
        CountRejected(stats_);
        return PtGeometrySourceObserveResult::IneligibleSourceDomain;
    }
    if (key.deformationClass != PtCanonicalDeformationClass::Rigid)
    {
        CountRejected(stats_);
        return PtGeometrySourceObserveResult::IneligibleDeformationClass;
    }
    if (sourceContentRevision == 0)
    {
        CountRejected(stats_);
        return PtGeometrySourceObserveResult::InvalidSourceRevision;
    }

    const std::uint64_t meshHash = PtHashCanonicalMeshKey(key);
    bool hashCollision = false;
    PtGeometrySourceRecord* existing =
        FindMutable(key, meshHash, hashCollision);
    if (hashCollision)
    {
        ++stats_.hashCollisions;
    }
    if (existing != nullptr &&
        sourceContentRevision == existing->sourceContentRevision)
    {
        ++stats_.reused;
        return PtGeometrySourceObserveResult::Reused;
    }
    if (existing != nullptr &&
        sourceContentRevision < existing->sourceContentRevision)
    {
        CountRejected(stats_);
        return PtGeometrySourceObserveResult::StaleSourceRevision;
    }
    if (payload == nullptr)
    {
        CountRejected(stats_);
        return PtGeometrySourceObserveResult::MissingPayload;
    }

    std::uint64_t retainedBytes = 0;
    const PtGeometrySourceObserveResult validation =
        PtValidateGeometrySourcePayload(key, *payload, retainedBytes);
    if (validation != PtGeometrySourceObserveResult::Added)
    {
        CountRejected(stats_);
        return validation;
    }

    PtGeometrySourceRecord replacement;
    replacement.key = key;
    replacement.meshHash = meshHash;
    replacement.sourceContentRevision = sourceContentRevision;
    replacement.sourceChecksum = PtChecksumGeometrySourcePayload(*payload);
    replacement.retainedBytes = retainedBytes;
    replacement.payload.positions.assign(
        payload->positions,
        payload->positions + payload->positionCount);
    replacement.payload.attributes.assign(
        payload->attributes,
        payload->attributes + payload->attributeCount);
    replacement.payload.indexes.assign(
        payload->indexes,
        payload->indexes + payload->indexCount);
    replacement.payload.triangles.assign(
        payload->triangles,
        payload->triangles + payload->triangleCount);

    ++stats_.payloadCopies;
    stats_.copiedBytes += retainedBytes;
    if (existing != nullptr)
    {
        stats_.retainedBytes -= existing->retainedBytes;
        stats_.retainedBytes += retainedBytes;
        *existing = std::move(replacement);
        ++stats_.revised;
        return PtGeometrySourceObserveResult::Revised;
    }

    records_.push_back(std::move(replacement));
    lookup_.emplace(meshHash, records_.size() - 1);
    stats_.retainedBytes += retainedBytes;
    ++stats_.added;
    return PtGeometrySourceObserveResult::Added;
}

const PtGeometrySourceRecord* PtGeometrySourceRegistry::Find(
    const PtCanonicalMeshKey& key) const
{
    const std::uint64_t meshHash = PtHashCanonicalMeshKey(key);
    const std::pair<
        std::unordered_multimap<std::uint64_t, std::size_t>::const_iterator,
        std::unordered_multimap<std::uint64_t, std::size_t>::const_iterator> range =
            lookup_.equal_range(meshHash);
    for (std::unordered_multimap<std::uint64_t, std::size_t>::const_iterator it =
            range.first;
        it != range.second;
        ++it)
    {
        if (it->second < records_.size() && records_[it->second].key == key)
        {
            return &records_[it->second];
        }
    }
    return nullptr;
}

const PtGeometrySourceRecord* PtGeometrySourceRegistry::RecordAt(
    std::size_t index) const
{
    return index < records_.size() ? &records_[index] : nullptr;
}

std::size_t PtGeometrySourceRegistry::RecordCount() const
{
    return records_.size();
}

const PtGeometrySourceRegistryStats& PtGeometrySourceRegistry::Stats() const
{
    return stats_;
}

void PtGeometrySourceRegistry::Clear()
{
    records_.clear();
    lookup_.clear();
    stats_ = PtGeometrySourceRegistryStats();
}

const char* PtGeometrySourceObserveResultName(
    PtGeometrySourceObserveResult result)
{
    switch (result)
    {
        case PtGeometrySourceObserveResult::Added: return "added";
        case PtGeometrySourceObserveResult::Reused: return "reused";
        case PtGeometrySourceObserveResult::Revised: return "revised";
        case PtGeometrySourceObserveResult::InvalidMeshKey: return "invalid_mesh_key";
        case PtGeometrySourceObserveResult::IneligibleSourceDomain: return "ineligible_source_domain";
        case PtGeometrySourceObserveResult::IneligibleDeformationClass: return "ineligible_deformation_class";
        case PtGeometrySourceObserveResult::InvalidSourceRevision: return "invalid_source_revision";
        case PtGeometrySourceObserveResult::StaleSourceRevision: return "stale_source_revision";
        case PtGeometrySourceObserveResult::MissingPayload: return "missing_payload";
        case PtGeometrySourceObserveResult::CountMismatch: return "count_mismatch";
        case PtGeometrySourceObserveResult::NonTriangleTopology: return "non_triangle_topology";
        case PtGeometrySourceObserveResult::IndexOutOfRange: return "index_out_of_range";
        case PtGeometrySourceObserveResult::ArithmeticOverflow: return "arithmetic_overflow";
    }
    return "unknown";
}
