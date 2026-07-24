#include "../renderer/NVRHI/PathTraceGeometrySourceRegistry.h"

#include <cstdio>
#include <cstring>

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

PtCanonicalMeshKey MakeRigidKey()
{
    PtCanonicalMeshKey key;
    key.sourceAssetId = 77;
    key.sourceAssetGeneration = 2;
    key.topologySignature = 0xabcddcba12344321ull;
    key.sourceDomain = PtCanonicalMeshSourceDomain::RegisteredRenderModel;
    key.modelSurfaceIndex = 3;
    key.vertexFormat = 1;
    key.deformationClass = PtCanonicalDeformationClass::Rigid;
    key.vertexCount = 4;
    key.indexCount = 6;
    return key;
}

struct TestPayload
{
    PtGeometrySourcePosition positions[4];
    PtGeometrySourceAttribute attributes[4];
    std::uint32_t indexes[6] = { 0, 1, 2, 0, 2, 3 };
    PtGeometrySourceTriangle triangles[2];

    TestPayload()
    {
        for (std::uint32_t vertex = 0; vertex < 4; ++vertex)
        {
            positions[vertex].xyz[0] = static_cast<float>(vertex);
            positions[vertex].xyz[1] = static_cast<float>(vertex + 10);
            positions[vertex].xyz[2] = static_cast<float>(vertex + 20);
            attributes[vertex].normal[2] = 1.0f;
            attributes[vertex].texCoord[0] = static_cast<float>(vertex) * 0.25f;
            attributes[vertex].texCoord[1] = static_cast<float>(vertex) * -0.5f;
            attributes[vertex].color[0] = 1.0f;
            attributes[vertex].color2[1] = 0.5f;
            attributes[vertex].tangent[0] = 1.0f;
            attributes[vertex].bitangent[1] = 1.0f;
            attributes[vertex].bitangentSign =
                vertex == 3 ? -1.0f : 1.0f;
        }
        triangles[0].sourceMaterialSlot = 5;
        triangles[0].geometryLocalFlags = 0x12;
        triangles[1].sourceMaterialSlot = 9;
        triangles[1].geometryLocalFlags = 0x34;
        triangles[1].sourceEmissivePrimitive = 1;
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

void TestFirstCopyAndValueOwnership()
{
    PtGeometrySourceRegistry registry;
    const PtCanonicalMeshKey key = MakeRigidKey();
    TestPayload input;
    const PtGeometrySourcePayloadView view = input.View();

    Expect(
        registry.Observe(key, 1, &view) ==
            PtGeometrySourceObserveResult::Added,
        "first rigid observation should add a source record");
    const PtGeometrySourceRecord* record = registry.Find(key);
    Expect(record != nullptr, "added source record should be findable");
    Expect(record != nullptr && record->payload.positions.size() == 4,
        "position stream should be copied");
    Expect(record != nullptr && record->payload.attributes.size() == 4,
        "attribute stream should be copied separately");
    Expect(record != nullptr && record->payload.indexes.size() == 6,
        "index stream should be copied");
    Expect(record != nullptr && record->payload.triangles.size() == 2,
        "triangle metadata stream should be copied");

    const float originalX = record->payload.positions[0].xyz[0];
    const std::uint32_t originalIndex = record->payload.indexes[1];
    input.positions[0].xyz[0] = 9000.0f;
    input.indexes[1] = 3;
    Expect(record->payload.positions[0].xyz[0] == originalX,
        "record must not retain the caller position pointer");
    Expect(record->payload.indexes[1] == originalIndex,
        "record must not retain the caller index pointer");

    const std::uint64_t expectedBytes =
        4ull * sizeof(PtGeometrySourcePosition) +
        4ull * sizeof(PtGeometrySourceAttribute) +
        6ull * sizeof(std::uint32_t) +
        2ull * sizeof(PtGeometrySourceTriangle);
    Expect(record->retainedBytes == expectedBytes,
        "record should account for all separated streams");
    Expect(record->sourceChecksum != 0,
        "first copy should produce a source checksum");
    Expect(registry.Stats().payloadCopies == 1 &&
        registry.Stats().copiedBytes == expectedBytes &&
        registry.Stats().retainedBytes == expectedBytes,
        "first copy diagnostics should report exact bytes");
}

void TestSteadyReuseDoesNotTouchPayload()
{
    PtGeometrySourceRegistry registry;
    const PtCanonicalMeshKey key = MakeRigidKey();
    TestPayload input;
    PtGeometrySourcePayloadView view = input.View();
    Expect(registry.Observe(key, 4, &view) ==
        PtGeometrySourceObserveResult::Added,
        "reuse test setup should add");

    Expect(registry.Observe(key, 4, nullptr) ==
        PtGeometrySourceObserveResult::Reused,
        "same revision should reuse without a payload");
    Expect(registry.Stats().reused == 1 &&
        registry.Stats().payloadCopies == 1,
        "steady reuse must not hash or copy the whole payload");
}

void TestRevisionReplacementAndStaleRejection()
{
    PtGeometrySourceRegistry registry;
    const PtCanonicalMeshKey key = MakeRigidKey();
    TestPayload first;
    PtGeometrySourcePayloadView firstView = first.View();
    Expect(registry.Observe(key, 8, &firstView) ==
        PtGeometrySourceObserveResult::Added,
        "revision test setup should add");
    const std::uint64_t firstChecksum =
        registry.Find(key)->sourceChecksum;

    TestPayload second;
    second.positions[2].xyz[1] += 0.75f;
    second.attributes[2].texCoord[0] += 4.0f;
    PtGeometrySourcePayloadView secondView = second.View();
    Expect(registry.Observe(key, 9, &secondView) ==
        PtGeometrySourceObserveResult::Revised,
        "new source revision should replace the owned payload");
    const PtGeometrySourceRecord* revised = registry.Find(key);
    Expect(revised != nullptr &&
        revised->sourceContentRevision == 9 &&
        revised->sourceChecksum != firstChecksum,
        "revision should publish the new revision and checksum");
    Expect(registry.RecordCount() == 1 &&
        registry.Stats().payloadCopies == 2 &&
        registry.Stats().revised == 1,
        "revision should replace, not duplicate, the canonical record");

    Expect(registry.Observe(key, 8, &firstView) ==
        PtGeometrySourceObserveResult::StaleSourceRevision,
        "older source revision should fail closed");
    Expect(registry.Find(key)->sourceContentRevision == 9,
        "stale rejection must not mutate the current record");
}

void TestEligibilityAndValidation()
{
    TestPayload input;
    PtGeometrySourcePayloadView view = input.View();

    PtGeometrySourceRegistry registry;
    PtCanonicalMeshKey key = MakeRigidKey();
    key.sourceDomain = PtCanonicalMeshSourceDomain::StaticWorldMap;
    Expect(registry.Observe(key, 1, &view) ==
        PtGeometrySourceObserveResult::IneligibleSourceDomain,
        "static world data is outside the rigid source slice");

    key = MakeRigidKey();
    key.sourceDomain = PtCanonicalMeshSourceDomain::SkinnedBindSource;
    key.deformationClass = PtCanonicalDeformationClass::Skinned;
    Expect(registry.Observe(key, 1, &view) ==
        PtGeometrySourceObserveResult::IneligibleSourceDomain,
        "skinned bind source should be an explicit negative control");

    key = MakeRigidKey();
    key.deformationClass = PtCanonicalDeformationClass::Skinned;
    Expect(registry.Observe(key, 1, &view) ==
        PtGeometrySourceObserveResult::IneligibleDeformationClass,
        "animated snapshot data must not enter the rigid source registry");

    key = MakeRigidKey();
    Expect(registry.Observe(key, 0, &view) ==
        PtGeometrySourceObserveResult::InvalidSourceRevision,
        "zero source revision should fail closed");
    Expect(registry.Observe(key, 1, nullptr) ==
        PtGeometrySourceObserveResult::MissingPayload,
        "first discovery requires a payload");

    PtGeometrySourcePayloadView badCounts = view;
    badCounts.attributeCount = 3;
    Expect(registry.Observe(key, 1, &badCounts) ==
        PtGeometrySourceObserveResult::CountMismatch,
        "mismatched separated stream counts should fail closed");

    PtGeometrySourcePayloadView nonTriangles = view;
    nonTriangles.indexCount = 5;
    key.indexCount = 5;
    Expect(registry.Observe(key, 1, &nonTriangles) ==
        PtGeometrySourceObserveResult::NonTriangleTopology,
        "non-triangle index topology should fail closed");

    key = MakeRigidKey();
    TestPayload badIndex;
    badIndex.indexes[5] = 4;
    PtGeometrySourcePayloadView badIndexView = badIndex.View();
    Expect(registry.Observe(key, 1, &badIndexView) ==
        PtGeometrySourceObserveResult::IndexOutOfRange,
        "out-of-range local index should fail closed");

    Expect(registry.RecordCount() == 0,
        "all rejected observations should leave the registry empty");
    Expect(registry.Stats().rejected == 8,
        "every rejected observation should have a named count");
}

void TestChecksumCoversFullFidelityStreams()
{
    TestPayload baseline;
    const PtGeometrySourcePayloadView baselineView = baseline.View();
    const std::uint64_t checksum =
        PtChecksumGeometrySourcePayload(baselineView);

    TestPayload changed = baseline;
    changed.attributes[3].bitangentSign = 1.0f;
    PtGeometrySourcePayloadView changedView = changed.View();
    Expect(PtChecksumGeometrySourcePayload(changedView) != checksum,
        "checksum should cover bitangent sign");

    changed = baseline;
    changed.triangles[1].sourceMaterialSlot++;
    changedView = changed.View();
    Expect(PtChecksumGeometrySourcePayload(changedView) != checksum,
        "checksum should cover source material slot");

    changed = baseline;
    changed.indexes[4] = 1;
    changedView = changed.View();
    Expect(PtChecksumGeometrySourcePayload(changedView) != checksum,
        "checksum should cover topology indexes");
}

}

int main()
{
    TestFirstCopyAndValueOwnership();
    TestSteadyReuseDoesNotTouchPayload();
    TestRevisionReplacementAndStaleRejection();
    TestEligibilityAndValidation();
    TestChecksumCoversFullFidelityStreams();
    if (g_failures != 0)
    {
        std::printf(
            "PathTraceGeometrySourceRegistryHarness: %d failure(s)\n",
            g_failures);
        return 1;
    }
    std::printf("PathTraceGeometrySourceRegistryHarness: PASS\n");
    return 0;
}
