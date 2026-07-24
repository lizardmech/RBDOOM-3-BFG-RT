#pragma once

// Pure planning/validation contract for carrying newly published immutable
// geometry sources across the frontend/backend frame-command boundary.

#include "PathTraceGeometrySourceRegistry.h"

#include <cstddef>
#include <cstdint>
#include <vector>

struct PtGeometrySourceTransportRecord
{
    PtCanonicalMeshKey key;
    std::uint64_t sourceContentRevision = 0;
    std::uint64_t sourceChecksum = 0;
    std::uint64_t retainedBytes = 0;
    std::uint64_t positionOffset = 0;
    std::uint64_t attributeOffset = 0;
    std::uint64_t indexOffset = 0;
    std::uint64_t triangleOffset = 0;
};

struct PtGeometrySourceTransportPlan
{
    std::size_t firstRecordIndex = 0;
    std::size_t nextRecordIndex = 0;
    bool complete = false;
    std::uint64_t packedBytes = 0;
    std::uint64_t positionCount = 0;
    std::uint64_t attributeCount = 0;
    std::uint64_t indexCount = 0;
    std::uint64_t triangleCount = 0;
    std::vector<PtGeometrySourceTransportRecord> records;
};

struct PtGeometrySourceTransportStreams
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

struct PtGeometrySourceTransportSnapshot
{
    std::uint64_t worldGeneration = 0;
    std::uint64_t publicationGeneration = 0;
    std::uint64_t publicationSequence = 0;
    std::uint64_t firstRecordIndex = 0;
    std::uint64_t nextRecordIndex = 0;
    std::uint64_t packedBytes = 0;
    std::uint64_t recordCount = 0;
    PtGeometrySourceTransportStreams streams;
    const PtGeometrySourceTransportRecord* records = nullptr;
};

enum class PtGeometrySourceTransportResult : std::uint32_t
{
    Success = 0,
    EmptyDelta,
    InvalidFirstRecord,
    InvalidBudget,
    InvalidSourceRecord,
    RecordExceedsBudget,
    ArithmeticOverflow,
    MissingStream,
    RangeOutOfBounds,
    CountMismatch,
    RetainedBytesMismatch,
    ChecksumMismatch
};

PtGeometrySourceTransportResult PtPlanGeometrySourceTransport(
    const PtGeometrySourceRegistry& registry,
    std::size_t firstRecordIndex,
    std::uint64_t byteBudget,
    PtGeometrySourceTransportPlan& plan);

PtGeometrySourceTransportResult PtValidateGeometrySourceTransportRecord(
    const PtGeometrySourceTransportRecord& record,
    const PtGeometrySourceTransportStreams& streams);

const char* PtGeometrySourceTransportResultName(
    PtGeometrySourceTransportResult result);
