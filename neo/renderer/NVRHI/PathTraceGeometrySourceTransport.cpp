#include "PathTraceGeometrySourceTransport.h"

namespace {

constexpr std::uint64_t kTransportAllocationAlignment = 16;

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

bool CheckedAddCount(
    std::uint64_t lhs,
    std::size_t rhs,
    std::uint64_t& result)
{
    return PtCheckedAddU64(
        lhs,
        static_cast<std::uint64_t>(rhs),
        result);
}

bool CheckedStreamEnd(
    std::uint64_t offset,
    std::uint64_t count,
    std::uint64_t limit)
{
    std::uint64_t end = 0;
    return PtCheckedAddU64(offset, count, end) && end <= limit;
}

bool CalculatePackedBytes(
    std::uint64_t recordCount,
    std::uint64_t positionCount,
    std::uint64_t attributeCount,
    std::uint64_t indexCount,
    std::uint64_t triangleCount,
    std::uint64_t& packedBytes)
{
    const std::uint64_t counts[] = {
        recordCount,
        positionCount,
        attributeCount,
        indexCount,
        triangleCount
    };
    const std::uint64_t strides[] = {
        sizeof(PtGeometrySourceTransportRecord),
        sizeof(PtGeometrySourcePosition),
        sizeof(PtGeometrySourceAttribute),
        sizeof(std::uint32_t),
        sizeof(PtGeometrySourceTriangle)
    };

    if (!CheckedAlign(
            sizeof(PtGeometrySourceTransportSnapshot),
            kTransportAllocationAlignment,
            packedBytes))
    {
        packedBytes = 0;
        return false;
    }
    for (std::size_t stream = 0; stream < 5; ++stream)
    {
        std::uint64_t streamBytes = 0;
        std::uint64_t alignedBytes = 0;
        if (!PtCheckedMulU64(counts[stream], strides[stream], streamBytes) ||
            !CheckedAlign(
                streamBytes,
                kTransportAllocationAlignment,
                alignedBytes) ||
            !PtCheckedAddU64(packedBytes, alignedBytes, packedBytes))
        {
            packedBytes = 0;
            return false;
        }
    }
    return true;
}

}

PtGeometrySourceTransportResult PtPlanGeometrySourceTransport(
    const PtGeometrySourceRegistry& registry,
    std::size_t firstRecordIndex,
    std::uint64_t byteBudget,
    PtGeometrySourceTransportPlan& plan)
{
    plan = PtGeometrySourceTransportPlan();
    plan.firstRecordIndex = firstRecordIndex;
    plan.nextRecordIndex = firstRecordIndex;

    if (firstRecordIndex > registry.RecordCount())
    {
        return PtGeometrySourceTransportResult::InvalidFirstRecord;
    }
    if (byteBudget == 0)
    {
        return PtGeometrySourceTransportResult::InvalidBudget;
    }
    if (firstRecordIndex == registry.RecordCount())
    {
        plan.complete = true;
        return PtGeometrySourceTransportResult::EmptyDelta;
    }

    for (std::size_t recordIndex = firstRecordIndex;
        recordIndex < registry.RecordCount();
        ++recordIndex)
    {
        const PtGeometrySourceRecord* source = registry.RecordAt(recordIndex);
        if (source == nullptr ||
            !PtCanonicalMeshKeyIsValid(source->key) ||
            source->sourceContentRevision == 0 ||
            source->sourceChecksum == 0 ||
            source->payload.positions.size() != source->key.vertexCount ||
            source->payload.attributes.size() != source->key.vertexCount ||
            source->payload.indexes.size() != source->key.indexCount ||
            source->payload.triangles.size() != source->key.indexCount / 3u)
        {
            plan = PtGeometrySourceTransportPlan();
            return PtGeometrySourceTransportResult::InvalidSourceRecord;
        }

        std::uint64_t nextPositionCount = 0;
        std::uint64_t nextAttributeCount = 0;
        std::uint64_t nextIndexCount = 0;
        std::uint64_t nextTriangleCount = 0;
        if (!CheckedAddCount(
                plan.positionCount,
                source->payload.positions.size(),
                nextPositionCount) ||
            !CheckedAddCount(
                plan.attributeCount,
                source->payload.attributes.size(),
                nextAttributeCount) ||
            !CheckedAddCount(
                plan.indexCount,
                source->payload.indexes.size(),
                nextIndexCount) ||
            !CheckedAddCount(
                plan.triangleCount,
                source->payload.triangles.size(),
                nextTriangleCount))
        {
            plan = PtGeometrySourceTransportPlan();
            return PtGeometrySourceTransportResult::ArithmeticOverflow;
        }

        std::uint64_t candidateBytes = 0;
        if (!CalculatePackedBytes(
                static_cast<std::uint64_t>(plan.records.size()) + 1,
                nextPositionCount,
                nextAttributeCount,
                nextIndexCount,
                nextTriangleCount,
                candidateBytes))
        {
            plan = PtGeometrySourceTransportPlan();
            return PtGeometrySourceTransportResult::ArithmeticOverflow;
        }
        if (candidateBytes > byteBudget)
        {
            if (plan.records.empty())
            {
                plan = PtGeometrySourceTransportPlan();
                plan.firstRecordIndex = firstRecordIndex;
                plan.nextRecordIndex = firstRecordIndex;
                return PtGeometrySourceTransportResult::RecordExceedsBudget;
            }
            break;
        }

        PtGeometrySourceTransportRecord transport;
        transport.key = source->key;
        transport.sourceContentRevision = source->sourceContentRevision;
        transport.sourceChecksum = source->sourceChecksum;
        transport.retainedBytes = source->retainedBytes;
        transport.positionOffset = plan.positionCount;
        transport.attributeOffset = plan.attributeCount;
        transport.indexOffset = plan.indexCount;
        transport.triangleOffset = plan.triangleCount;
        plan.records.push_back(transport);
        plan.positionCount = nextPositionCount;
        plan.attributeCount = nextAttributeCount;
        plan.indexCount = nextIndexCount;
        plan.triangleCount = nextTriangleCount;
        plan.packedBytes = candidateBytes;
        plan.nextRecordIndex = recordIndex + 1;
    }

    plan.complete = plan.nextRecordIndex == registry.RecordCount();
    return PtGeometrySourceTransportResult::Success;
}

PtGeometrySourceTransportResult PtValidateGeometrySourceTransportRecord(
    const PtGeometrySourceTransportRecord& record,
    const PtGeometrySourceTransportStreams& streams)
{
    if (!PtCanonicalMeshKeyIsValid(record.key) ||
        record.sourceContentRevision == 0 ||
        record.sourceChecksum == 0)
    {
        return PtGeometrySourceTransportResult::InvalidSourceRecord;
    }
    if (streams.positions == nullptr ||
        streams.attributes == nullptr ||
        streams.indexes == nullptr ||
        streams.triangles == nullptr)
    {
        return PtGeometrySourceTransportResult::MissingStream;
    }

    const std::uint64_t vertexCount = record.key.vertexCount;
    const std::uint64_t indexCount = record.key.indexCount;
    const std::uint64_t triangleCount = indexCount / 3ull;
    if (!CheckedStreamEnd(
            record.positionOffset,
            vertexCount,
            streams.positionCount) ||
        !CheckedStreamEnd(
            record.attributeOffset,
            vertexCount,
            streams.attributeCount) ||
        !CheckedStreamEnd(
            record.indexOffset,
            indexCount,
            streams.indexCount) ||
        !CheckedStreamEnd(
            record.triangleOffset,
            triangleCount,
            streams.triangleCount))
    {
        return PtGeometrySourceTransportResult::RangeOutOfBounds;
    }

    PtGeometrySourcePayloadView payload;
    payload.positions = streams.positions + record.positionOffset;
    payload.positionCount = vertexCount;
    payload.attributes = streams.attributes + record.attributeOffset;
    payload.attributeCount = vertexCount;
    payload.indexes = streams.indexes + record.indexOffset;
    payload.indexCount = indexCount;
    payload.triangles = streams.triangles + record.triangleOffset;
    payload.triangleCount = triangleCount;

    std::uint64_t retainedBytes = 0;
    const PtGeometrySourceObserveResult validation =
        PtValidateGeometrySourcePayload(record.key, payload, retainedBytes);
    if (validation == PtGeometrySourceObserveResult::CountMismatch ||
        validation == PtGeometrySourceObserveResult::NonTriangleTopology)
    {
        return PtGeometrySourceTransportResult::CountMismatch;
    }
    if (validation != PtGeometrySourceObserveResult::Added)
    {
        return PtGeometrySourceTransportResult::InvalidSourceRecord;
    }
    if (retainedBytes != record.retainedBytes)
    {
        return PtGeometrySourceTransportResult::RetainedBytesMismatch;
    }
    if (PtChecksumGeometrySourcePayload(payload) != record.sourceChecksum)
    {
        return PtGeometrySourceTransportResult::ChecksumMismatch;
    }
    return PtGeometrySourceTransportResult::Success;
}

const char* PtGeometrySourceTransportResultName(
    PtGeometrySourceTransportResult result)
{
    switch (result)
    {
        case PtGeometrySourceTransportResult::Success: return "success";
        case PtGeometrySourceTransportResult::EmptyDelta: return "empty_delta";
        case PtGeometrySourceTransportResult::InvalidFirstRecord: return "invalid_first_record";
        case PtGeometrySourceTransportResult::InvalidBudget: return "invalid_budget";
        case PtGeometrySourceTransportResult::InvalidSourceRecord: return "invalid_source_record";
        case PtGeometrySourceTransportResult::RecordExceedsBudget: return "record_exceeds_budget";
        case PtGeometrySourceTransportResult::ArithmeticOverflow: return "arithmetic_overflow";
        case PtGeometrySourceTransportResult::MissingStream: return "missing_stream";
        case PtGeometrySourceTransportResult::RangeOutOfBounds: return "range_out_of_bounds";
        case PtGeometrySourceTransportResult::CountMismatch: return "count_mismatch";
        case PtGeometrySourceTransportResult::RetainedBytesMismatch: return "retained_bytes_mismatch";
        case PtGeometrySourceTransportResult::ChecksumMismatch: return "checksum_mismatch";
    }
    return "unknown";
}
