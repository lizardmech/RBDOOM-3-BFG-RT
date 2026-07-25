#include "PathTraceSkinnedHitRoute.h"

#include <cstdint>
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

PtCanonicalInstanceKey MakeInstance(
    std::uint64_t world,
    std::uint32_t entity,
    std::uint32_t surface)
{
    PtCanonicalInstanceKey key;
    key.worldGeneration = world;
    key.renderDefIndex = entity;
    key.renderDefGeneration = 1;
    key.subInstanceKind =
        PtCanonicalSubInstanceKind::SkinnedSurface;
    key.modelSurfaceIndex = surface;
    key.jointSubmeshIndex = -1;
    return key;
}

PtCanonicalMeshKey MakeMesh(
    std::uint64_t asset,
    std::uint32_t surface,
    std::uint32_t vertexCount,
    std::uint32_t indexCount)
{
    PtCanonicalMeshKey key;
    key.sourceAssetId = asset;
    key.sourceAssetGeneration = 1;
    key.topologySignature = asset * 17 + surface + 1;
    key.sourceDomain =
        PtCanonicalMeshSourceDomain::SkinnedBindSource;
    key.modelSurfaceIndex = surface;
    key.vertexFormat = 1;
    key.deformationClass =
        PtCanonicalDeformationClass::Skinned;
    key.vertexCount = vertexCount;
    key.indexCount = indexCount;
    key.jointSubmeshIndex = -1;
    return key;
}

PtSkinnedHitRouteCandidate MakeCandidate(
    const std::vector<std::uint32_t>& sourceIndexes)
{
    PtSkinnedHitRouteCandidate candidate;
    candidate.instanceKey = MakeInstance(4, 20, 0);
    candidate.meshKey = MakeMesh(
        80,
        0,
        6,
        static_cast<std::uint32_t>(
            sourceIndexes.size()));
    candidate.sourceChecksum = 91;
    candidate.sourceGpuIndexGeneration = 3;
    candidate.sourceIndexOffsetBytes = 64;
    candidate.sourceIndexCapacityBytes = 4096;
    candidate.sourceIndexes = sourceIndexes.data();
    candidate.sourceIndexCount = sourceIndexes.size();
    candidate.outputStorageGeneration = 5;
    candidate.outputVertexOffsetBytes = 12 * 112;
    candidate.outputVertexCount = 6;
    candidate.outputCapacityBytes = 8192;
    candidate.previousPositionOffset = 30;
    candidate.previousPositionCount = 64;
    candidate.previousValid = true;
    candidate.legacyVertexOffset = 10;
    candidate.legacyVertexCount = 6;
    candidate.legacyIndexOffset = 3;
    candidate.legacyIndexCount = 9;
    candidate.legacyTriangleOffset = 1;
    candidate.legacyTriangleCount = 3;
    candidate.fallbackMaterialId = 700;
    candidate.fallbackMaterialIndex = 9;
    candidate.fallbackTriangleClassAndFlags = 4;
    candidate.dispatchReady = true;
    return candidate;
}

PtSkinnedHitRouteLegacyView MakeLegacyView(
    const std::vector<std::uint32_t>& indexes,
    const std::vector<std::uint32_t>& classes,
    const std::vector<std::uint32_t>& materialIds,
    const std::vector<std::uint32_t>& materialIndexes)
{
    PtSkinnedHitRouteLegacyView view;
    view.indexes = indexes.data();
    view.indexCount = indexes.size();
    view.triangleClasses = classes.data();
    view.triangleClassCount = classes.size();
    view.triangleMaterialIds = materialIds.data();
    view.triangleMaterialIdCount = materialIds.size();
    view.triangleMaterialIndexes =
        materialIndexes.data();
    view.triangleMaterialIndexCount =
        materialIndexes.size();
    return view;
}

void TestSourcePrimitiveMappingAndMotion()
{
    const std::vector<std::uint32_t> sourceIndexes = {
        0, 1, 2,
        2, 2, 3,
        2, 3, 4,
        0, 4, 5
    };
    const std::vector<std::uint32_t> legacyIndexes = {
        99, 99, 99,
        10, 11, 12,
        12, 13, 14,
        10, 14, 15
    };
    const std::vector<std::uint32_t> classes = {
        0, 4, 4, 4
    };
    const std::vector<std::uint32_t> materialIds = {
        0, 700, 700, 700
    };
    const std::vector<std::uint32_t> materialIndexes = {
        0, 9, 9, 9
    };
    PtSkinnedHitRouteCandidate candidate =
        MakeCandidate(sourceIndexes);
    const PtSkinnedHitRouteBuild build =
        PtBuildSkinnedHitRoutes(
            { candidate },
            MakeLegacyView(
                legacyIndexes,
                classes,
                materialIds,
                materialIndexes),
            7);

    Expect(
        build.stats.accepted == 1 &&
            build.stats.rejected == 0,
        "exact source/legacy subsequence should be accepted");
    Expect(
        build.records.size() == 1 &&
            build.records[0].shaderInstanceId == 7 &&
            build.records[0].sourceIndexOffset == 16 &&
            build.records[0].outputVertexOffset == 12,
        "route record should preserve dense slot and GPU offsets");
    Expect(
        build.records[0].previousPositionOffset == 30 &&
            (build.records[0].flags &
                PT_SKINNED_HIT_ROUTE_HAS_PREVIOUS) != 0,
        "valid previous range should become the motion contract");
    Expect(
        build.triangles.size() == 4 &&
            build.stats.mappedLegacyTriangles == 3 &&
            build.stats.sourceOnlyTriangles == 1,
        "source topology should retain one filtered primitive explicitly");
    Expect(
        build.triangles[0].legacyPrimitiveIndex == 1 &&
            build.triangles[1].legacyPrimitiveIndex ==
                PT_SKINNED_HIT_ROUTE_INVALID_INDEX &&
            build.triangles[2].legacyPrimitiveIndex == 2 &&
            build.triangles[3].legacyPrimitiveIndex == 3,
        "source primitive indexes must map to compact legacy primitives");
    Expect(
        build.triangles[1].materialId == 700 &&
            build.triangles[1].materialIndex == 9 &&
            build.triangles[1].triangleClassAndFlags == 4,
        "source-only primitive should retain explicit surface metadata");
    Expect(
        build.triangles[0].canonicalPrimitiveHash != 0 &&
            build.triangles[0].emissiveIdentityHash != 0 &&
            build.stats.emissiveIdentityCollisions == 0,
        "canonical primitive and emissive identities must be stable and unique");
}

void TestTopologyMismatchFailsClosed()
{
    const std::vector<std::uint32_t> sourceIndexes = {
        0, 1, 2,
        2, 3, 4
    };
    const std::vector<std::uint32_t> legacyIndexes = {
        10, 14, 15
    };
    const std::vector<std::uint32_t> one = { 4 };
    PtSkinnedHitRouteCandidate candidate =
        MakeCandidate(sourceIndexes);
    candidate.meshKey.indexCount = 6;
    candidate.legacyIndexOffset = 0;
    candidate.legacyIndexCount = 3;
    candidate.legacyTriangleOffset = 0;
    candidate.legacyTriangleCount = 1;
    const PtSkinnedHitRouteBuild build =
        PtBuildSkinnedHitRoutes(
            { candidate },
            MakeLegacyView(
                legacyIndexes,
                one,
                one,
                one),
            2);
    Expect(
        build.stats.accepted == 0 &&
            build.results[0] ==
                PtSkinnedHitRouteResult::
                    LegacyTopologyMismatch,
        "legacy triangle not present in source order must fail closed");
}

void TestDuplicateAndInstanceIdOverflow()
{
    const std::vector<std::uint32_t> sourceIndexes = {
        0, 1, 2
    };
    const std::vector<std::uint32_t> legacyIndexes = {
        10, 11, 12
    };
    const std::vector<std::uint32_t> one = { 4 };
    PtSkinnedHitRouteCandidate candidate =
        MakeCandidate(sourceIndexes);
    candidate.meshKey.indexCount = 3;
    candidate.legacyIndexOffset = 0;
    candidate.legacyIndexCount = 3;
    candidate.legacyTriangleOffset = 0;
    candidate.legacyTriangleCount = 1;
    const PtSkinnedHitRouteLegacyView legacy =
        MakeLegacyView(
            legacyIndexes,
            one,
            one,
            one);

    const PtSkinnedHitRouteBuild duplicate =
        PtBuildSkinnedHitRoutes(
            { candidate, candidate },
            legacy,
            2);
    Expect(
        duplicate.stats.accepted == 1 &&
            duplicate.stats.duplicateInstances == 1 &&
            duplicate.results[1] ==
                PtSkinnedHitRouteResult::
                    DuplicateInstanceKey,
        "duplicate full InstanceKey must not receive a second route slot");

    const PtSkinnedHitRouteBuild overflow =
        PtBuildSkinnedHitRoutes(
            { candidate },
            legacy,
            PT_SKINNED_HIT_ROUTE_MAX_SHADER_INSTANCE_ID +
                1u);
    Expect(
        overflow.stats.accepted == 0 &&
            overflow.results[0] ==
                PtSkinnedHitRouteResult::
                    ShaderInstanceIdOverflow,
        "24-bit shader instance ID overflow must fail closed");
}

}

int main()
{
    TestSourcePrimitiveMappingAndMotion();
    TestTopologyMismatchFailsClosed();
    TestDuplicateAndInstanceIdOverflow();

    if (g_failures != 0)
    {
        std::printf(
            "PathTraceSkinnedHitRouteHarness: %d failure(s)\n",
            g_failures);
        return 1;
    }
    std::printf(
        "PathTraceSkinnedHitRouteHarness: PASS\n");
    return 0;
}
