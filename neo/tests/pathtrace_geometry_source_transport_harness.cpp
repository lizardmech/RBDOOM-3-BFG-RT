#include "../renderer/NVRHI/PathTraceGeometrySourceTransport.h"

#include <cstdio>
#include <vector>

namespace {

int g_failures = 0;

void Expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

PtCanonicalMeshKey MakeKey(std::uint64_t assetId)
{
    PtCanonicalMeshKey key;
    key.sourceAssetId = assetId;
    key.sourceAssetGeneration = 1;
    key.topologySignature = assetId * 17;
    key.sourceDomain = PtCanonicalMeshSourceDomain::RegisteredRenderModel;
    key.modelSurfaceIndex = 0;
    key.vertexFormat = 1;
    key.deformationClass = PtCanonicalDeformationClass::Rigid;
    key.vertexCount = 4;
    key.indexCount = 6;
    return key;
}

struct Payload
{
    PtGeometrySourcePosition positions[4];
    PtGeometrySourceAttribute attributes[4];
    std::uint32_t indexes[6] = { 0, 1, 2, 0, 2, 3 };
    PtGeometrySourceTriangle triangles[2];

    explicit Payload(float base)
    {
        for (int vertex = 0; vertex < 4; ++vertex)
        {
            positions[vertex].xyz[0] = base + vertex;
            positions[vertex].xyz[1] = base + vertex + 1;
            positions[vertex].xyz[2] = base + vertex + 2;
            attributes[vertex].normal[2] = 1.0f;
            attributes[vertex].tangent[0] = 1.0f;
            attributes[vertex].bitangent[1] = 1.0f;
        }
    }

    PtGeometrySourcePayloadView View() const
    {
        PtGeometrySourcePayloadView view;
        view.positions = positions;
        view.positionCount = 4;
        view.attributes = attributes;
        view.attributeCount = 4;
        view.indexes = indexes;
        view.indexCount = 6;
        view.triangles = triangles;
        view.triangleCount = 2;
        return view;
    }
};

PtGeometrySourceRegistry MakeRegistry()
{
    PtGeometrySourceRegistry registry;
    for (std::uint64_t record = 0; record < 3; ++record)
    {
        Payload payload(static_cast<float>(record * 10));
        const PtGeometrySourcePayloadView view = payload.View();
        Expect(
            registry.Observe(MakeKey(record + 1), 1, &view) ==
                PtGeometrySourceObserveResult::Added,
            "transport setup should add source record");
    }
    return registry;
}

void TestCompleteAndPaginatedPlans()
{
    const PtGeometrySourceRegistry registry = MakeRegistry();
    PtGeometrySourceTransportPlan full;
    Expect(
        PtPlanGeometrySourceTransport(
            registry,
            0,
            1024 * 1024,
            full) == PtGeometrySourceTransportResult::Success,
        "large budget should produce a transport plan");
    Expect(
        full.records.size() == 3 &&
            full.nextRecordIndex == 3 &&
            full.complete,
        "large budget should carry the complete delta");
    Expect(
        full.records[0].positionOffset == 0 &&
            full.records[1].positionOffset == 4 &&
            full.records[2].indexOffset == 12,
        "record stream offsets should be element-relative and append-only");

    PtGeometrySourceTransportPlan firstOnly;
    Expect(
        PtPlanGeometrySourceTransport(
            registry,
            0,
            full.packedBytes / 3 + 64,
            firstOnly) == PtGeometrySourceTransportResult::Success,
        "bounded budget should produce a partial page");
    Expect(
        !firstOnly.records.empty() &&
            firstOnly.records.size() < full.records.size() &&
            !firstOnly.complete &&
            firstOnly.nextRecordIndex == firstOnly.records.size(),
        "partial page should expose an exact continuation cursor");

    PtGeometrySourceTransportPlan continuation;
    Expect(
        PtPlanGeometrySourceTransport(
            registry,
            firstOnly.nextRecordIndex,
            1024 * 1024,
            continuation) == PtGeometrySourceTransportResult::Success,
        "continuation cursor should plan remaining records");
    Expect(
        continuation.firstRecordIndex == firstOnly.nextRecordIndex &&
            continuation.nextRecordIndex == 3 &&
            continuation.complete,
        "continuation should finish without replaying earlier records");
}

void TestBudgetAndCursorFailures()
{
    const PtGeometrySourceRegistry registry = MakeRegistry();
    PtGeometrySourceTransportPlan plan;
    Expect(
        PtPlanGeometrySourceTransport(registry, 4, 4096, plan) ==
            PtGeometrySourceTransportResult::InvalidFirstRecord,
        "cursor beyond record count should fail");
    Expect(
        PtPlanGeometrySourceTransport(registry, 0, 0, plan) ==
            PtGeometrySourceTransportResult::InvalidBudget,
        "zero byte budget should fail");
    Expect(
        PtPlanGeometrySourceTransport(registry, 0, 1, plan) ==
            PtGeometrySourceTransportResult::RecordExceedsBudget,
        "a record larger than the budget should fail without progress");
    Expect(
        plan.records.empty() && plan.nextRecordIndex == 0,
        "oversize failure must preserve the continuation cursor");
    Expect(
        PtPlanGeometrySourceTransport(
            registry,
            registry.RecordCount(),
            4096,
            plan) == PtGeometrySourceTransportResult::EmptyDelta &&
            plan.complete,
        "cursor at record count should report an empty complete delta");
}

void TestImportValidation()
{
    const PtGeometrySourceRegistry registry = MakeRegistry();
    const PtGeometrySourceRecord* source = registry.RecordAt(0);
    Expect(source != nullptr, "validation setup source should exist");
    if (source == nullptr)
    {
        return;
    }

    PtGeometrySourceTransportRecord record;
    record.key = source->key;
    record.sourceContentRevision = source->sourceContentRevision;
    record.sourceChecksum = source->sourceChecksum;
    record.retainedBytes = source->retainedBytes;

    PtGeometrySourceTransportStreams streams;
    streams.positions = source->payload.positions.data();
    streams.positionCount = source->payload.positions.size();
    streams.attributes = source->payload.attributes.data();
    streams.attributeCount = source->payload.attributes.size();
    streams.indexes = source->payload.indexes.data();
    streams.indexCount = source->payload.indexes.size();
    streams.triangles = source->payload.triangles.data();
    streams.triangleCount = source->payload.triangles.size();

    Expect(
        PtValidateGeometrySourceTransportRecord(record, streams) ==
            PtGeometrySourceTransportResult::Success,
        "intact transport record should validate");

    PtGeometrySourceTransportRecord corrupt = record;
    ++corrupt.sourceChecksum;
    Expect(
        PtValidateGeometrySourceTransportRecord(corrupt, streams) ==
            PtGeometrySourceTransportResult::ChecksumMismatch,
        "checksum mismatch should fail closed");

    corrupt = record;
    ++corrupt.retainedBytes;
    Expect(
        PtValidateGeometrySourceTransportRecord(corrupt, streams) ==
            PtGeometrySourceTransportResult::RetainedBytesMismatch,
        "retained byte mismatch should fail closed");

    corrupt = record;
    corrupt.positionOffset = 1;
    Expect(
        PtValidateGeometrySourceTransportRecord(corrupt, streams) ==
            PtGeometrySourceTransportResult::RangeOutOfBounds,
        "stream range beyond the packet should fail closed");

    PtGeometrySourceTransportStreams missing = streams;
    missing.attributes = nullptr;
    Expect(
        PtValidateGeometrySourceTransportRecord(record, missing) ==
            PtGeometrySourceTransportResult::MissingStream,
        "missing packet stream should fail closed");
}

}

int main()
{
    TestCompleteAndPaginatedPlans();
    TestBudgetAndCursorFailures();
    TestImportValidation();
    if (g_failures != 0)
    {
        std::printf(
            "PathTraceGeometrySourceTransportHarness: %d failure(s)\n",
            g_failures);
        return 1;
    }
    std::printf("PathTraceGeometrySourceTransportHarness: PASS\n");
    return 0;
}
