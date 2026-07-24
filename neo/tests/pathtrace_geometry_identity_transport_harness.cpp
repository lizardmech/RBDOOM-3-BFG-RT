#include "../renderer/NVRHI/PathTraceGeometryIdentityTransport.h"

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

PtCanonicalInstanceKey MakeInstance(std::uint32_t renderDefIndex)
{
    PtCanonicalInstanceKey key;
    key.worldGeneration = 7;
    key.renderDefIndex = renderDefIndex;
    key.renderDefGeneration = 2;
    key.subInstanceKind = PtCanonicalSubInstanceKind::RigidSurface;
    key.modelSurfaceIndex = 3;
    return key;
}

PtCanonicalMeshKey MakeMesh(std::uint64_t assetId)
{
    PtCanonicalMeshKey key;
    key.sourceAssetId = assetId;
    key.sourceAssetGeneration = 1;
    key.topologySignature = assetId * 19;
    key.sourceDomain = PtCanonicalMeshSourceDomain::RegisteredRenderModel;
    key.modelSurfaceIndex = 3;
    key.vertexFormat = 1;
    key.deformationClass = PtCanonicalDeformationClass::Rigid;
    key.vertexCount = 4;
    key.indexCount = 6;
    return key;
}

PtGeometryIdentityTransportRecord MakeRecord(
    PtGeometryIdentityOperation operation,
    std::uint64_t sequence,
    const PtCanonicalInstanceKey& instance,
    const PtCanonicalMeshKey& mesh)
{
    PtGeometryIdentityTransportRecord record;
    record.operation = operation;
    record.eventSequence = sequence;
    record.instanceKey = instance;
    record.instanceHash = PtHashCanonicalInstanceKey(instance);
    record.meshKey = mesh;
    record.meshHash = PtHashCanonicalMeshKey(mesh);
    return record;
}

PtGeometryIdentityTransportSnapshot MakeSnapshot(
    const PtGeometryIdentityTransportPlan& plan,
    std::uint64_t publicationSequence)
{
    PtGeometryIdentityTransportSnapshot snapshot;
    snapshot.worldGeneration = 7;
    snapshot.publicationGeneration = 5;
    snapshot.publicationSequence = publicationSequence;
    snapshot.firstRecordIndex = plan.firstRecordIndex;
    snapshot.nextRecordIndex = plan.nextRecordIndex;
    snapshot.packedBytes = plan.packedBytes;
    snapshot.recordCount = plan.records.size();
    snapshot.records = plan.records.data();
    return snapshot;
}

void TestPlanningAndRegistry()
{
    const PtCanonicalInstanceKey first = MakeInstance(10);
    const PtCanonicalInstanceKey second = MakeInstance(11);
    const PtCanonicalMeshKey meshA = MakeMesh(100);
    const PtCanonicalMeshKey meshB = MakeMesh(200);
    std::vector<PtGeometryIdentityTransportRecord> journal;
    journal.push_back(MakeRecord(
        PtGeometryIdentityOperation::Upsert, 1, first, meshA));
    journal.push_back(MakeRecord(
        PtGeometryIdentityOperation::Upsert, 2, second, meshA));
    journal.push_back(MakeRecord(
        PtGeometryIdentityOperation::Upsert, 3, first, meshB));
    journal.push_back(MakeRecord(
        PtGeometryIdentityOperation::Remove, 4, second, meshA));

    PtGeometryIdentityTransportPlan full;
    Expect(
        PtPlanGeometryIdentityTransport(
            journal, 0, 1024 * 1024, full) ==
            PtGeometryIdentityTransportResult::Success,
        "large budget should plan the complete identity journal");
    Expect(
        full.records.size() == 4 &&
            full.nextRecordIndex == 4 &&
            full.complete,
        "complete identity plan should expose its exact cursor");

    PtGeometryIdentityTransportPlan firstPage;
    Expect(
        PtPlanGeometryIdentityTransport(
            journal, 0, full.packedBytes / 2, firstPage) ==
            PtGeometryIdentityTransportResult::Success,
        "bounded identity budget should produce a page");
    Expect(
        !firstPage.records.empty() &&
            firstPage.records.size() < journal.size() &&
            !firstPage.complete,
        "bounded identity page should leave a continuation");

    PtGeometryIdentityRegistry registry;
    PtGeometryIdentityTransportSnapshot firstSnapshot =
        MakeSnapshot(firstPage, 1);
    Expect(
        registry.ApplySnapshot(&firstSnapshot) ==
            PtGeometryIdentityTransportResult::Success,
        "first identity page should import");

    PtGeometryIdentityTransportPlan continuation;
    Expect(
        PtPlanGeometryIdentityTransport(
            journal,
            firstPage.nextRecordIndex,
            1024 * 1024,
            continuation) ==
            PtGeometryIdentityTransportResult::Success,
        "identity continuation should plan");
    PtGeometryIdentityTransportSnapshot continuationSnapshot =
        MakeSnapshot(continuation, 2);
    Expect(
        registry.ApplySnapshot(&continuationSnapshot) ==
            PtGeometryIdentityTransportResult::Success,
        "identity continuation should import");

    const PtGeometryIdentityBinding* firstBinding = registry.Find(first);
    Expect(
        firstBinding != nullptr && firstBinding->meshKey == meshB,
        "later upsert should revise the exact instance binding");
    Expect(
        registry.Find(second) == nullptr,
        "remove event should retire the exact instance binding");
    Expect(
        registry.Stats().activeBindings == 1 &&
            registry.Stats().importCursor == 4 &&
            registry.Stats().upserts == 2 &&
            registry.Stats().revised == 1 &&
            registry.Stats().removed == 1,
        "identity registry stats should reflect add revise remove");
}

void TestFailClosedOrdering()
{
    const PtCanonicalInstanceKey instance = MakeInstance(21);
    const PtCanonicalMeshKey mesh = MakeMesh(300);
    std::vector<PtGeometryIdentityTransportRecord> journal;
    journal.push_back(MakeRecord(
        PtGeometryIdentityOperation::Upsert, 1, instance, mesh));

    PtGeometryIdentityTransportPlan plan;
    Expect(
        PtPlanGeometryIdentityTransport(journal, 0, 4096, plan) ==
            PtGeometryIdentityTransportResult::Success,
        "single identity event should plan");

    PtGeometryIdentityRegistry registry;
    PtGeometryIdentityTransportPlan corruptPlan = plan;
    corruptPlan.records[0].instanceHash++;
    PtGeometryIdentityTransportSnapshot snapshot =
        MakeSnapshot(corruptPlan, 1);
    Expect(
        registry.ApplySnapshot(&snapshot) ==
            PtGeometryIdentityTransportResult::HashMismatch,
        "corrupt identity hash should fail closed");
    Expect(
        registry.Stats().activeBindings == 0,
        "rejected first publication must not mutate bindings");
    snapshot = MakeSnapshot(plan, 1);
    snapshot.firstRecordIndex = 1;
    snapshot.nextRecordIndex = 2;
    Expect(
        registry.ApplySnapshot(&snapshot) ==
            PtGeometryIdentityTransportResult::SequenceMismatch,
        "new publication must begin at cursor zero");
}

}

int main()
{
    TestPlanningAndRegistry();
    TestFailClosedOrdering();
    if (g_failures != 0)
    {
        std::printf(
            "PathTraceGeometryIdentityTransportHarness: %d failure(s)\n",
            g_failures);
        return 1;
    }
    std::printf("PathTraceGeometryIdentityTransportHarness: PASS\n");
    return 0;
}
