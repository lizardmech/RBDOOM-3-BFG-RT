#include "PathTraceSkinnedConsumerAudit.h"

#include <cstdio>

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

PtCanonicalInstanceKey MakeInstance()
{
    PtCanonicalInstanceKey key;
    key.worldGeneration = 9;
    key.renderDefIndex = 12;
    key.renderDefGeneration = 2;
    key.subInstanceKind = PtCanonicalSubInstanceKind::SkinnedSurface;
    key.modelSurfaceIndex = 3;
    key.jointSubmeshIndex = -1;
    return key;
}

PtCanonicalMeshKey MakeMesh()
{
    PtCanonicalMeshKey key;
    key.sourceAssetId = 77;
    key.sourceAssetGeneration = 1;
    key.topologySignature = 91;
    key.sourceDomain = PtCanonicalMeshSourceDomain::SkinnedBindSource;
    key.modelSurfaceIndex = 3;
    key.vertexFormat = 1;
    key.deformationClass = PtCanonicalDeformationClass::Skinned;
    key.vertexCount = 4;
    key.indexCount = 6;
    key.jointSubmeshIndex = -1;
    return key;
}

struct Fixture
{
    std::vector<std::uint32_t> sourceIndexes = { 0, 1, 2, 0, 2, 3 };
    std::vector<std::uint32_t> legacyIndexes = { 10, 11, 12, 10, 12, 13 };
    std::vector<std::uint32_t> classes = { 4, 4 };
    std::vector<std::uint32_t> materialIds = { 700, 700 };
    std::vector<std::uint32_t> materialIndexes = { 1, 1 };
    std::vector<std::uint32_t> materialTableIds = { 10, 700 };
    PtSkinnedHitRouteBuild build;
    PtSkinnedHitRouteGpuUpload upload;
    PtSkinnedTlasRoutePlan tlas;

    Fixture()
    {
        PtSkinnedHitRouteCandidate candidate;
        candidate.instanceKey = MakeInstance();
        candidate.meshKey = MakeMesh();
        candidate.sourceChecksum = 55;
        candidate.sourceGpuIndexGeneration = 3;
        candidate.sourceIndexOffsetBytes = 32;
        candidate.sourceIndexCapacityBytes = 1024;
        candidate.sourceIndexes = sourceIndexes.data();
        candidate.sourceIndexCount = sourceIndexes.size();
        candidate.outputStorageGeneration = 4;
        candidate.outputVertexOffsetBytes = 8 * 112;
        candidate.outputVertexCount = 4;
        candidate.outputCapacityBytes = 4096;
        candidate.previousPositionOffset = 20;
        candidate.previousPositionCount = 64;
        candidate.previousValid = true;
        candidate.legacyVertexOffset = 10;
        candidate.legacyVertexCount = 4;
        candidate.legacyIndexOffset = 0;
        candidate.legacyIndexCount = legacyIndexes.size();
        candidate.legacyTriangleOffset = 0;
        candidate.legacyTriangleCount = 2;
        candidate.fallbackMaterialId = 700;
        candidate.fallbackMaterialIndex = 1;
        candidate.fallbackTriangleClassAndFlags = 4;
        candidate.dispatchReady = true;

        PtSkinnedHitRouteLegacyView legacy;
        legacy.indexes = legacyIndexes.data();
        legacy.indexCount = legacyIndexes.size();
        legacy.triangleClasses = classes.data();
        legacy.triangleClassCount = classes.size();
        legacy.triangleMaterialIds = materialIds.data();
        legacy.triangleMaterialIdCount = materialIds.size();
        legacy.triangleMaterialIndexes = materialIndexes.data();
        legacy.triangleMaterialIndexCount = materialIndexes.size();
        build = PtBuildSkinnedHitRoutes({ candidate }, legacy, 17);
        upload = PtBuildSkinnedHitRouteGpuUpload(build, 113);

        PtSkinnedTlasRouteCandidate tlasCandidate;
        tlasCandidate.cpuRoute = &build.records[0];
        tlasCandidate.gpuRoute = &upload.records[0];
        tlasCandidate.resourceFound = true;
        tlasCandidate.resourceContractExact = true;
        tlasCandidate.blasReady = true;
        PtSkinnedTlasRoutePlanInput tlasInput;
        tlasInput.gate = true;
        tlasInput.uploadedRouteCount = 1;
        tlasInput.shaderTableRecordCount = 4;
        tlasInput.candidates = { tlasCandidate };
        tlas = PtPlanSkinnedTlasRoutes(tlasInput);
    }

    PtSkinnedConsumerAuditInput Input()
    {
        PtSkinnedConsumerAuditInput input;
        input.cpuBuild = &build;
        input.gpuUpload = &upload;
        input.tlasPlan = &tlas;
        input.legacyTriangleClasses = &classes;
        input.legacyTriangleMaterialIds = &materialIds;
        input.legacyTriangleMaterialIndexes = &materialIndexes;
        input.materialTableIds = &materialTableIds;
        input.currentOutputVertexCount = 64;
        input.previousPositionCount = 64;
        return input;
    }
};

void TestExactContract()
{
    Fixture fixture;
    const PtSkinnedConsumerAuditStats stats =
        PtAuditSkinnedConsumerContract(fixture.Input());
    Expect(stats.Accepted(), "exact consumer tuple should pass");
    Expect(
        stats.routes == 1 &&
            stats.triangles == 2 &&
            stats.mappedLegacyTriangles == 2 &&
            stats.sourceOnlyTriangles == 0 &&
            stats.motionReadyRoutes == 1,
        "accepted audit should account for every route and triangle");
}

void TestMaterialMismatchFails()
{
    Fixture fixture;
    fixture.materialTableIds[1] = 701;
    const PtSkinnedConsumerAuditStats stats =
        PtAuditSkinnedConsumerContract(fixture.Input());
    Expect(
        !stats.Accepted() && stats.materialTableMismatch == 2,
        "material-table mismatch must fail every affected triangle");
}

void TestLegacyAndMotionMismatchFail()
{
    Fixture fixture;
    fixture.classes[1] = 2;
    PtSkinnedConsumerAuditInput input = fixture.Input();
    input.previousPositionCount = 22;
    const PtSkinnedConsumerAuditStats stats =
        PtAuditSkinnedConsumerContract(input);
    Expect(
        !stats.Accepted() &&
            stats.legacyMetadataMismatch == 1 &&
            stats.previousRangeMismatch == 1,
        "legacy metadata and previous-range mismatch must be named");
}

void TestUploadAndTlasMismatchFail()
{
    Fixture fixture;
    ++fixture.upload.records[0].outputVertexOffset;
    fixture.tlas.records[0].shaderInstanceId = 999;
    const PtSkinnedConsumerAuditStats stats =
        PtAuditSkinnedConsumerContract(fixture.Input());
    Expect(
        !stats.Accepted() &&
            stats.routeUploadMismatch == 1 &&
            stats.tlasRouteMismatch == 1,
        "upload and TLAS mismatches must fail independently");
}

void TestShadowContractDoesNotRequireTlas()
{
    Fixture fixture;
    PtSkinnedConsumerAuditInput input = fixture.Input();
    input.tlasPlan = nullptr;
    input.requireTlasPlan = false;
    const PtSkinnedConsumerAuditStats stats =
        PtAuditSkinnedConsumerContract(input);
    Expect(
        stats.Accepted() &&
            stats.mappedLegacyTriangles == 2,
        "same-frame legacy shadow should validate without claiming TLAS admission");
}

} // namespace

int main()
{
    TestExactContract();
    TestMaterialMismatchFails();
    TestLegacyAndMotionMismatchFail();
    TestUploadAndTlasMismatchFail();
    TestShadowContractDoesNotRequireTlas();

    if (g_failures != 0)
    {
        std::printf(
            "PathTraceSkinnedConsumerAuditHarness: %d failure(s)\n",
            g_failures);
        return 1;
    }
    std::printf("PathTraceSkinnedConsumerAuditHarness: PASS\n");
    return 0;
}
