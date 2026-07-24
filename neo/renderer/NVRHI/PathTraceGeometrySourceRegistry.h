#pragma once

// Pure, value-owned CPU contract for immutable path-tracing mesh sources.
//
// The registry owns no renderer pointers or GPU objects. Positions remain a
// separate float3 stream from the full-fidelity decoded attributes. Resolved
// material-table bindings are deliberately not source content: the triangle
// stream retains only the source material slot and geometry-local metadata.

#include "PathTraceCanonicalGeometryIdentity.h"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

struct PtGeometrySourcePosition
{
    float xyz[3] = {};
};

struct PtGeometrySourceAttribute
{
    float normal[3] = {};
    float texCoord[2] = {};
    float color[4] = {};
    float color2[4] = {};
    float tangent[3] = {};
    float bitangent[3] = {};
    float bitangentSign = 1.0f;
};

struct PtGeometrySourceTriangle
{
    std::uint32_t sourceMaterialSlot = 0;
    std::uint32_t geometryLocalFlags = 0;
    std::uint32_t sourceEmissivePrimitive = UINT32_MAX;
    std::uint32_t reserved = 0;
};

struct PtGeometrySourcePayloadView
{
    const PtGeometrySourcePosition* positions = nullptr;
    std::uint64_t positionCount = 0;
    const PtGeometrySourceAttribute* attributes = nullptr;
    std::uint64_t attributeCount = 0;
    const std::uint32_t* indexes = nullptr;
    std::uint64_t indexCount = 0;
    const PtGeometrySourceTriangle* triangles = nullptr;
    std::uint64_t triangleCount = 0;
};

struct PtGeometrySourcePayload
{
    std::vector<PtGeometrySourcePosition> positions;
    std::vector<PtGeometrySourceAttribute> attributes;
    std::vector<std::uint32_t> indexes;
    std::vector<PtGeometrySourceTriangle> triangles;
};

struct PtGeometrySourceRecord
{
    PtCanonicalMeshKey key;
    std::uint64_t meshHash = 0;
    std::uint64_t sourceContentRevision = 0;
    std::uint64_t sourceChecksum = 0;
    std::uint64_t retainedBytes = 0;
    PtGeometrySourcePayload payload;
};

enum class PtGeometrySourceObserveResult : std::uint32_t
{
    Added = 0,
    Reused,
    Revised,
    InvalidMeshKey,
    IneligibleSourceDomain,
    IneligibleDeformationClass,
    InvalidSourceRevision,
    StaleSourceRevision,
    MissingPayload,
    CountMismatch,
    NonTriangleTopology,
    IndexOutOfRange,
    ArithmeticOverflow
};

struct PtGeometrySourceRegistryStats
{
    std::uint64_t added = 0;
    std::uint64_t reused = 0;
    std::uint64_t revised = 0;
    std::uint64_t rejected = 0;
    std::uint64_t hashCollisions = 0;
    std::uint64_t payloadCopies = 0;
    std::uint64_t copiedBytes = 0;
    std::uint64_t retainedBytes = 0;
};

class PtGeometrySourceRegistry
{
public:
    PtGeometrySourceObserveResult Observe(
        const PtCanonicalMeshKey& key,
        std::uint64_t sourceContentRevision,
        const PtGeometrySourcePayloadView* payload);

    const PtGeometrySourceRecord* Find(const PtCanonicalMeshKey& key) const;
    const PtGeometrySourceRecord* RecordAt(std::size_t index) const;
    std::size_t RecordCount() const;
    const PtGeometrySourceRegistryStats& Stats() const;
    void Clear();

private:
    PtGeometrySourceRecord* FindMutable(
        const PtCanonicalMeshKey& key,
        std::uint64_t meshHash,
        bool& hashCollision);

    std::vector<PtGeometrySourceRecord> records_;
    std::unordered_multimap<std::uint64_t, std::size_t> lookup_;
    PtGeometrySourceRegistryStats stats_;
};

PtGeometrySourceObserveResult PtValidateGeometrySourcePayload(
    const PtCanonicalMeshKey& key,
    const PtGeometrySourcePayloadView& payload,
    std::uint64_t& retainedBytes);

std::uint64_t PtChecksumGeometrySourcePayload(
    const PtGeometrySourcePayloadView& payload);

const char* PtGeometrySourceObserveResultName(
    PtGeometrySourceObserveResult result);
