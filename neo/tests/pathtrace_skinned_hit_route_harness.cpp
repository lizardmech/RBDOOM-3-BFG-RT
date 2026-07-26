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

void TestGpuUploadAbi()
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
    const PtSkinnedHitRouteBuild build =
        PtBuildSkinnedHitRoutes(
            { candidate },
            MakeLegacyView(
                legacyIndexes,
                one,
                one,
                one),
            113);
    const PtSkinnedHitRouteGpuUpload upload =
        PtBuildSkinnedHitRouteGpuUpload(build, 113);
    Expect(
        sizeof(PathTraceSkinnedHitRouteGpuRecord) == 80 &&
            sizeof(PathTraceSkinnedHitRouteGpuTriangle) == 36,
        "shader-facing record strides must remain exact");
    Expect(
        upload.records.size() == 1 &&
            upload.triangles.size() == 1 &&
            upload.records[0].shaderInstanceId == 113 &&
            upload.records[0].routeCount == 1 &&
            upload.records[0].triangleMetadataCount == 1 &&
            upload.records[0].triangleCount == 1 &&
            upload.records[0].sourceIndexOffset == 16 &&
            upload.records[0].outputVertexOffset == 12 &&
            upload.records[0].previousPositionOffset == 30 &&
            upload.signature != 0,
        "GPU upload must preserve the accepted route contract");
    Expect(
            upload.triangles[0].sourcePrimitiveIndex == 0 &&
            upload.triangles[0].legacyPrimitiveIndex == 0 &&
            upload.triangles[0].materialId == 4 &&
            upload.triangles[0].materialIndex == 4 &&
            upload.triangles[0].canonicalPrimitiveHashLo != 0 &&
            upload.triangles[0].emissiveIdentityHashLo != 0,
        "GPU triangle metadata must preserve source-local identity");

    const PtSkinnedHitRouteGpuUpload empty =
        PtBuildSkinnedHitRouteGpuUpload(
            PtSkinnedHitRouteBuild(),
            77);
    Expect(
        empty.records.size() == 1 &&
            empty.triangles.size() == 1 &&
            empty.records[0].shaderInstanceId == 77 &&
            empty.records[0].routeCount == 0 &&
            empty.records[0].triangleMetadataCount == 0,
        "empty GPU upload must retain a safe zero-count sentinel");

    PtSkinnedHitRouteBuild twoRouteBuild = build;
    twoRouteBuild.records.push_back(build.records.front());
    const PtSkinnedHitRouteGpuUpload rebased =
        PtBuildSkinnedHitRouteGpuUpload(twoRouteBuild, 211);
    Expect(
        rebased.records.size() == 2 &&
            rebased.records[0].shaderInstanceId == 211 &&
            rebased.records[1].shaderInstanceId == 212 &&
            rebased.records[0].routeCount == 2 &&
            rebased.records[1].routeCount == 2,
        "GPU upload must rebase a prior-frame shadow after the rigid range");

    const PtSkinnedHitRouteGpuUpload overflow =
        PtBuildSkinnedHitRouteGpuUpload(
            twoRouteBuild,
            PT_SKINNED_HIT_ROUTE_MAX_SHADER_INSTANCE_ID);
    Expect(
        overflow.records.size() == 1 &&
            overflow.records[0].routeCount == 0 &&
            overflow.triangles.size() == 1,
        "GPU upload rebasing must fail closed on 24-bit range overflow");
}

void TestSbtContributionContract()
{
    struct ExpectedSelection
    {
        PtPathTraceSbtGeometryClass geometryClass;
        std::uint32_t rayContribution;
        std::uint32_t instanceContribution;
        std::uint32_t recordIndex;
    };
    const ExpectedSelection expected[] = {
        {
            PtPathTraceSbtGeometryClass::Legacy,
            PT_PATH_TRACE_SBT_PRIMARY_RAY_CONTRIBUTION,
            PT_PATH_TRACE_SBT_LEGACY_INSTANCE_CONTRIBUTION,
            0u
        },
        {
            PtPathTraceSbtGeometryClass::Legacy,
            PT_PATH_TRACE_SBT_SHADOW_RAY_CONTRIBUTION,
            PT_PATH_TRACE_SBT_LEGACY_INSTANCE_CONTRIBUTION,
            1u
        },
        {
            PtPathTraceSbtGeometryClass::Skinned,
            PT_PATH_TRACE_SBT_PRIMARY_RAY_CONTRIBUTION,
            PT_PATH_TRACE_SBT_SKINNED_INSTANCE_CONTRIBUTION,
            2u
        },
        {
            PtPathTraceSbtGeometryClass::Skinned,
            PT_PATH_TRACE_SBT_SHADOW_RAY_CONTRIBUTION,
            PT_PATH_TRACE_SBT_SKINNED_INSTANCE_CONTRIBUTION,
            3u
        }
    };
    for (const ExpectedSelection& expectedSelection : expected)
    {
        PtPathTraceSbtSelectionInput input;
        input.geometryClass =
            expectedSelection.geometryClass;
        input.rayContribution =
            expectedSelection.rayContribution;
        input.geometryContribution = 0u;
        input.geometryMultiplier = 1u;
        input.shaderTableRecordCount = 4u;
        const PtPathTraceSbtSelection selection =
            PtPlanPathTraceSbtSelection(input);
        Expect(
            selection.result ==
                    PtPathTraceSbtSelectionResult::Accepted &&
                selection.instanceContribution ==
                    expectedSelection.instanceContribution &&
                selection.recordIndex ==
                    expectedSelection.recordIndex,
            "legacy/skinned primary/shadow SBT mapping must be exact");
    }

    PtPathTraceSbtSelectionInput invalid;
    invalid.geometryClass =
        PtPathTraceSbtGeometryClass::Skinned;
    invalid.shaderTableRecordCount = 4u;
    invalid.rayContribution = 2u;
    Expect(
        PtPlanPathTraceSbtSelection(invalid).result ==
            PtPathTraceSbtSelectionResult::
                UnsupportedRayContribution,
        "unknown ray contribution must fail closed");

    invalid.rayContribution =
        PT_PATH_TRACE_SBT_PRIMARY_RAY_CONTRIBUTION;
    invalid.geometryContribution = 1u;
    Expect(
        PtPlanPathTraceSbtSelection(invalid).result ==
            PtPathTraceSbtSelectionResult::
                UnsupportedGeometryContribution,
        "multi-geometry BLAS contribution must fail closed");

    invalid.geometryContribution = 0u;
    invalid.geometryMultiplier = 0u;
    Expect(
        PtPlanPathTraceSbtSelection(invalid).result ==
            PtPathTraceSbtSelectionResult::
                InvalidGeometryMultiplier,
        "non-unit geometry multiplier must fail closed");

    invalid.geometryMultiplier = 1u;
    invalid.shaderTableRecordCount = 2u;
    Expect(
        PtPlanPathTraceSbtSelection(invalid).result ==
            PtPathTraceSbtSelectionResult::
                MissingShaderTableRecord,
        "skinned route must reject a legacy-only shader table");

    invalid.geometryClass =
        PtPathTraceSbtGeometryClass::Legacy;
    const PtPathTraceSbtSelection legacyOnly =
        PtPlanPathTraceSbtSelection(invalid);
    Expect(
        legacyOnly.result ==
                PtPathTraceSbtSelectionResult::Accepted &&
            legacyOnly.recordIndex == 0u,
        "legacy route must remain valid with two SBT records");
}

void TestSkinnedTlasRoutePlanner()
{
    const std::vector<std::uint32_t> sourceIndexes = {
        0, 1, 2
    };
    const std::vector<std::uint32_t> legacyIndexes = {
        10, 11, 12
    };
    const std::vector<std::uint32_t> one = { 4 };
    PtSkinnedHitRouteCandidate routeCandidate =
        MakeCandidate(sourceIndexes);
    routeCandidate.meshKey.indexCount = 3;
    routeCandidate.legacyIndexOffset = 0;
    routeCandidate.legacyIndexCount = 3;
    routeCandidate.legacyTriangleOffset = 0;
    routeCandidate.legacyTriangleCount = 1;
    const PtSkinnedHitRouteBuild build =
        PtBuildSkinnedHitRoutes(
            { routeCandidate },
            MakeLegacyView(
                legacyIndexes,
                one,
                one,
                one),
            31);
    const PtSkinnedHitRouteGpuUpload upload =
        PtBuildSkinnedHitRouteGpuUpload(build, 31);

    PtSkinnedTlasRouteCandidate candidate;
    candidate.cpuRoute = &build.records[0];
    candidate.gpuRoute = &upload.records[0];
    candidate.resourceFound = true;
    candidate.resourceContractExact = true;
    candidate.blasReady = true;

    PtSkinnedTlasRoutePlanInput input;
    input.gate = true;
    input.baseInstanceCount = 2;
    input.existingExtraInstanceCount = 7;
    input.uploadedRouteCount = 1;
    input.candidates = { candidate };
    const PtSkinnedTlasRoutePlan accepted =
        PtPlanSkinnedTlasRoutes(input);
    Expect(
        accepted.result ==
                PtSkinnedTlasRouteResult::Accepted &&
            accepted.records.size() == 1 &&
            accepted.records[0].shaderInstanceId == 31 &&
            accepted.records[0].instanceMask == 0x02u &&
            accepted.records[0].hitGroupContribution ==
                PT_PATH_TRACE_SBT_SKINNED_INSTANCE_CONTRIBUTION &&
            accepted.stats.accepted == 1 &&
            accepted.stats.rejected == 0,
        "exact skinned route/resource/SBT tuple should emit one TLAS record");

    PtSkinnedTlasRoutePlanInput disabledInput = input;
    disabledInput.gate = false;
    const PtSkinnedTlasRoutePlan disabled =
        PtPlanSkinnedTlasRoutes(disabledInput);
    Expect(
        disabled.result ==
                PtSkinnedTlasRouteResult::GateDisabled &&
            disabled.records.empty() &&
            disabled.stats.rejected == 0,
        "disabled comparison gate must remain behavior-neutral");

    PtSkinnedTlasRoutePlanInput countMismatch = input;
    countMismatch.uploadedRouteCount = 0;
    const PtSkinnedTlasRoutePlan countMismatchPlan =
        PtPlanSkinnedTlasRoutes(countMismatch);
    Expect(
        countMismatchPlan.result ==
                PtSkinnedTlasRouteResult::
                    UploadRouteCountMismatch &&
            countMismatchPlan.records.empty() &&
            countMismatchPlan.stats.
                uploadRouteCountMismatch == 1,
        "CPU/upload route-count mismatch must fail closed");

    PtSkinnedTlasRoutePlanInput resourceMismatch = input;
    resourceMismatch.candidates[0].
        resourceContractExact = false;
    const PtSkinnedTlasRoutePlan resourceMismatchPlan =
        PtPlanSkinnedTlasRoutes(resourceMismatch);
    Expect(
        resourceMismatchPlan.result ==
                PtSkinnedTlasRouteResult::
                    ResourceContractMismatch &&
            resourceMismatchPlan.records.empty() &&
            resourceMismatchPlan.stats.
                resourceContractMismatch == 1,
        "resource-contract mismatch must fail closed");

    PtSkinnedTlasRoutePlanInput missingBlas = input;
    missingBlas.candidates[0].blasReady = false;
    const PtSkinnedTlasRoutePlan missingBlasPlan =
        PtPlanSkinnedTlasRoutes(missingBlas);
    Expect(
        missingBlasPlan.result ==
                PtSkinnedTlasRouteResult::MissingBlas &&
            missingBlasPlan.records.empty() &&
            missingBlasPlan.stats.missingBlas == 1,
        "missing BLAS must fail closed");

    PtSkinnedTlasRoutePlanInput legacyOnlySbt = input;
    legacyOnlySbt.shaderTableRecordCount = 2;
    const PtSkinnedTlasRoutePlan legacyOnlySbtPlan =
        PtPlanSkinnedTlasRoutes(legacyOnlySbt);
    Expect(
        legacyOnlySbtPlan.result ==
                PtSkinnedTlasRouteResult::
                    InvalidSbtSelection &&
            legacyOnlySbtPlan.records.empty() &&
            legacyOnlySbtPlan.stats.
                invalidSbtSelection == 1,
        "legacy-only shader table must reject skinned TLAS records");

    PtSkinnedTlasRoutePlanInput capacity = input;
    capacity.existingExtraInstanceCount = 510;
    const PtSkinnedTlasRoutePlan capacityPlan =
        PtPlanSkinnedTlasRoutes(capacity);
    Expect(
        capacityPlan.result ==
                PtSkinnedTlasRouteResult::
                    TlasCapacityExceeded &&
            capacityPlan.records.empty() &&
            capacityPlan.stats.
                tlasCapacityExceeded == 1,
        "TLAS capacity overflow must fail closed");
}

void TestCaptureSplitAdmission()
{
    const std::vector<std::uint32_t> sourceIndexes = {
        0, 1, 2
    };
    PtSkinnedHitRouteCandidate routeCandidate =
        MakeCandidate(sourceIndexes);
    routeCandidate.meshKey.indexCount = 3;
    routeCandidate.legacyCapturePresent = false;
    routeCandidate.legacyVertexOffset = 0;
    routeCandidate.legacyVertexCount = 0;
    routeCandidate.legacyIndexOffset = 0;
    routeCandidate.legacyIndexCount = 0;
    routeCandidate.legacyTriangleOffset = 0;
    routeCandidate.legacyTriangleCount = 0;
    routeCandidate.fallbackMaterialId = 9;
    routeCandidate.fallbackMaterialIndex = 4;
    routeCandidate.fallbackTriangleClassAndFlags = 2;
    const PtSkinnedHitRouteBuild sourceOnlyBuild =
        PtBuildSkinnedHitRoutes(
            { routeCandidate },
            PtSkinnedHitRouteLegacyView(),
            31);
    Expect(
        sourceOnlyBuild.stats.accepted == 1 &&
            sourceOnlyBuild.stats.sourceTriangles == 1 &&
            sourceOnlyBuild.stats.legacyTriangles == 0 &&
            sourceOnlyBuild.stats.sourceOnlyTriangles == 1 &&
            sourceOnlyBuild.triangles.size() == 1 &&
            sourceOnlyBuild.triangles[0].
                    legacyPrimitiveIndex ==
                PT_SKINNED_HIT_ROUTE_INVALID_INDEX &&
            sourceOnlyBuild.triangles[0].materialId == 9 &&
            sourceOnlyBuild.triangles[0].materialIndex == 4,
        "CPU-omitted route must preserve source topology and explicit fallback metadata");

    const PtSkinnedHitRouteRecord& prior =
        sourceOnlyBuild.records[0];
    PtSkinnedCaptureAdmissionInput input;
    input.gate = true;
    input.currentInstance = prior.instanceKey;
    input.currentMesh = prior.meshKey;
    input.currentSourceChecksum = prior.sourceChecksum;
    input.currentVertexCount = prior.vertexCount;
    input.currentIndexCount = prior.indexCount;
    input.jointDataReady = true;
    input.priorRouteLive = true;
    input.priorRoute = &prior;
    Expect(
        PtPlanSkinnedCaptureAdmission(input) ==
            PtSkinnedCaptureAdmissionResult::
                OmitCpuCapture,
        "exact prior route plus current source and joints should omit CPU capture");

    PtSkinnedCaptureAdmissionInput disabled = input;
    disabled.gate = false;
    Expect(
        PtPlanSkinnedCaptureAdmission(disabled) ==
            PtSkinnedCaptureAdmissionResult::GateDisabled,
        "capture split gate off must retain CPU capture");

    PtSkinnedCaptureAdmissionInput missing = input;
    missing.priorRoute = nullptr;
    Expect(
        PtPlanSkinnedCaptureAdmission(missing) ==
            PtSkinnedCaptureAdmissionResult::
                MissingPriorRoute,
        "new surfaces without a prior accepted route must retain CPU capture");

    PtSkinnedCaptureAdmissionInput retired = input;
    retired.priorRouteLive = false;
    Expect(
        PtPlanSkinnedCaptureAdmission(retired) ==
            PtSkinnedCaptureAdmissionResult::
                PriorRouteNotLive,
        "retired prior route resources must retain same-frame CPU capture");

    PtSkinnedCaptureAdmissionInput changedSource = input;
    ++changedSource.currentSourceChecksum;
    Expect(
        PtPlanSkinnedCaptureAdmission(changedSource) ==
            PtSkinnedCaptureAdmissionResult::
                CurrentSourceMismatch,
        "changed source topology must retain same-frame CPU capture");

    PtSkinnedCaptureAdmissionInput missingJoints = input;
    missingJoints.jointDataReady = false;
    Expect(
        PtPlanSkinnedCaptureAdmission(missingJoints) ==
            PtSkinnedCaptureAdmissionResult::
                JointDataNotReady,
        "missing current joints must retain same-frame CPU capture");
}

}

int main()
{
    TestSourcePrimitiveMappingAndMotion();
    TestTopologyMismatchFailsClosed();
    TestDuplicateAndInstanceIdOverflow();
    TestGpuUploadAbi();
    TestSbtContributionContract();
    TestSkinnedTlasRoutePlanner();
    TestCaptureSplitAdmission();

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
