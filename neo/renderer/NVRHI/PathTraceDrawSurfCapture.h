#pragma once

// Live drawSurf mirror for the future PT scene producer.
//
// The mirror observes the final raster-submitted surfaces as mesh identities
// plus per-frame instances. Source mode 3 can also append non-static mirror
// records into the existing shader-compatible dynamic frame bucket.

#include "PathTraceInstanceUniverse.h"
#include "PathTraceCaptureProduct.h"

#include <vector>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

class RtSmokeGeometryUniverse;
class RtPathTraceSceneUniverse;
struct PathTraceSmokeVertex;
struct RtSmokeSkinnedSurfaceRecord;
struct RtSmokeCapturedSurfaceRecord;
struct PtSkinnedHitRouteRecord;
struct viewDef_t;
struct RtSmokeRigidCaptureSkipRecord;

const int RT_PT_BOUNDS_OVERLAY_MAX_LINES = 4096;

struct RtPathTraceBoundsOverlayLine
{
    idVec4 startAndPad = idVec4(0.0f, 0.0f, 0.0f, 0.0f);
    idVec4 endAndPad = idVec4(0.0f, 0.0f, 0.0f, 0.0f);
    idVec4 color = idVec4(1.0f, 1.0f, 1.0f, 1.0f);
};

struct RtSmokeMergedWalkedRange
{
    uint64_t id = 0;
    int bucketIndex = -1;
    int vertexBegin = 0;
    int vertexCount = 0;
    int indexBegin = 0;
    int indexCount = 0;
    int triangleBegin = 0;
    int triangleCount = 0;
    uint32_t surfaceClassId = 0;
    uint32_t feedClass = 0;
    uint32_t materialId = 0;
    bool particle = false;
    bool trueDeform = false;
    uint64_t companionRigidId = 0;
    uint64_t companionSkinnedId = 0;
};

// Current-frame, owner-only harvest record.  Live pointers never enter a Lane-A
// slot; they remain valid only until the same-frame fallback append completes.
struct RtPathTraceOwnerHarvestSurface
{
    const drawSurf_t* drawSurf = nullptr;
    const srfTriangles_t* tri = nullptr;
    RtPathTraceCaptureSurfaceProduct decision;
    std::uint32_t baseMaterialId = 0;
    std::uint64_t legacyStaticKey = 0;
    std::uint64_t rigidWalkInstanceId = 0;
    std::uint64_t mergedRangeId = 0;
    std::uint64_t mergedCompanionRigidId = 0;
    std::uint64_t mergedCompanionSkinnedId = 0;
    std::uint32_t feedClass = 0;
    bool particle = false;
    bool trueDeform = false;
    bool finalized = false;
    bool appendEligible = false;
};

struct RtPathTraceOwnerHarvest
{
    std::vector<RtPathTraceOwnerHarvestSurface> surfaces;
    RtPathTraceCaptureMembershipReceipt membershipReceipt;
    std::uint64_t harvestUs = 0;
    bool complete = false;

    void Clear()
    {
        surfaces.clear();
        membershipReceipt = {};
        harvestUs = 0;
        complete = false;
    }
};

bool CopyPathTraceOwnerHarvestDecisionsToSnapshot(
    const RtPathTraceOwnerHarvest& harvest,
    RtPathTraceCaptureOwnerSnapshot& snapshot);

inline void SwapPathTraceOwnerMaterialSample(
    RtSmokeMaterialSample& lhs, RtSmokeMaterialSample& rhs) noexcept
{
    using std::swap;
    swap(lhs.id, rhs.id);
    swap(lhs.surfaces, rhs.surfaces);
    swap(lhs.triangles, rhs.triangles);
    idStr name(std::move(lhs.name));
    lhs.name = std::move(rhs.name);
    rhs.name = std::move(name);
}

inline void SwapPathTraceOwnerDynamicMaterialSample(
    RtSmokeDynamicMaterialEvalSample& lhs,
    RtSmokeDynamicMaterialEvalSample& rhs) noexcept
{
    using std::swap;
    swap(lhs.valid, rhs.valid);
    swap(lhs.id, rhs.id);
    swap(lhs.surfaces, rhs.surfaces);
    swap(lhs.triangles, rhs.triangles);
    swap(lhs.stageIndex, rhs.stageIndex);
    swap(lhs.enabledStages, rhs.enabledStages);
    swap(lhs.disabledStages, rhs.disabledStages);
    swap(lhs.colorStages, rhs.colorStages);
    swap(lhs.alphaStages, rhs.alphaStages);
    swap(lhs.alphaTestStages, rhs.alphaTestStages);
    swap(lhs.texMatrixStages, rhs.texMatrixStages);
    swap(lhs.dynamicImageStages, rhs.dynamicImageStages);
    swap(lhs.cinematicStages, rhs.cinematicStages);
    swap(lhs.guiRenderTargetStages, rhs.guiRenderTargetStages);
    swap(lhs.programStages, rhs.programStages);
    idStr name(std::move(lhs.name));
    lhs.name = std::move(rhs.name);
    rhs.name = std::move(name);
    swap(lhs.stagePriority, rhs.stagePriority);
    swap(lhs.selectedStageEmissive, rhs.selectedStageEmissive);
    swap(lhs.condition, rhs.condition);
    swap(lhs.color, rhs.color);
    swap(lhs.alphaTest, rhs.alphaTest);
    swap(lhs.image, rhs.image);
    swap(lhs.texMatrix, rhs.texMatrix);
    swap(lhs.hasDiffuseStageColor, rhs.hasDiffuseStageColor);
    swap(lhs.diffuseStageColor, rhs.diffuseStageColor);
    swap(lhs.diffuseStageCondition, rhs.diffuseStageCondition);
    swap(lhs.orderedStages, rhs.orderedStages);
    swap(lhs.orderedStageCount, rhs.orderedStageCount);
    swap(lhs.orderedStageOverflow, rhs.orderedStageOverflow);
    swap(lhs.hasSurfaceOrigin, rhs.hasSurfaceOrigin);
    const idVec3 origin = lhs.surfaceOrigin;
    lhs.surfaceOrigin = rhs.surfaceOrigin;
    rhs.surfaceOrigin = origin;
}

inline void SwapPathTraceOwnerTranslucentDebugSample(
    RtSmokeTranslucentSubtypeDebugSample& lhs,
    RtSmokeTranslucentSubtypeDebugSample& rhs) noexcept
{
    using std::swap;
    swap(lhs.valid, rhs.valid);
    swap(lhs.subtype, rhs.subtype);
    swap(lhs.surfaceIndex, rhs.surfaceIndex);
    swap(lhs.verts, rhs.verts);
    swap(lhs.indexes, rhs.indexes);
    idStr name(std::move(lhs.materialName));
    lhs.materialName = std::move(rhs.materialName);
    rhs.materialName = std::move(name);
    swap(lhs.coverage, rhs.coverage);
    swap(lhs.sort, rhs.sort);
    swap(lhs.deform, rhs.deform);
    swap(lhs.info, rhs.info);
}

inline void SwapPathTraceOwnerMaterialStats(
    RtSmokeMaterialStats& lhs, RtSmokeMaterialStats& rhs) noexcept
{
    using std::swap;
    swap(lhs.totalSurfaces, rhs.totalSurfaces);
    swap(lhs.totalTriangles, rhs.totalTriangles);
    swap(lhs.uniqueMaterials, rhs.uniqueMaterials);
    swap(lhs.translucentSurfaces, rhs.translucentSurfaces);
    swap(lhs.translucentTriangles, rhs.translucentTriangles);
    swap(lhs.translucentUniqueMaterials, rhs.translucentUniqueMaterials);
    lhs.materialIds.swap(rhs.materialIds);
    lhs.translucentMaterialIds.swap(rhs.translucentMaterialIds);
    for (int i = 0; i < RT_SMOKE_MATERIAL_REASON_SAMPLES; ++i)
    {
        SwapPathTraceOwnerMaterialSample(lhs.samples[i], rhs.samples[i]);
        SwapPathTraceOwnerMaterialSample(
            lhs.translucentSamples[i], rhs.translucentSamples[i]);
    }
    swap(lhs.sampleCount, rhs.sampleCount);
    swap(lhs.translucentSampleCount, rhs.translucentSampleCount);
    swap(lhs.dynamicEvalSurfaces, rhs.dynamicEvalSurfaces);
    swap(lhs.dynamicEvalTriangles, rhs.dynamicEvalTriangles);
    swap(lhs.dynamicEvalStages, rhs.dynamicEvalStages);
    swap(lhs.dynamicEvalEnabledStages, rhs.dynamicEvalEnabledStages);
    swap(lhs.dynamicEvalDisabledStages, rhs.dynamicEvalDisabledStages);
    swap(lhs.dynamicEvalColorStages, rhs.dynamicEvalColorStages);
    swap(lhs.dynamicEvalAlphaStages, rhs.dynamicEvalAlphaStages);
    swap(lhs.dynamicEvalAlphaTestStages, rhs.dynamicEvalAlphaTestStages);
    swap(lhs.dynamicEvalTexMatrixStages, rhs.dynamicEvalTexMatrixStages);
    swap(lhs.dynamicEvalDynamicImageStages, rhs.dynamicEvalDynamicImageStages);
    swap(lhs.dynamicEvalCinematicStages, rhs.dynamicEvalCinematicStages);
    swap(lhs.dynamicEvalGuiRenderTargetStages, rhs.dynamicEvalGuiRenderTargetStages);
    swap(lhs.dynamicEvalProgramStages, rhs.dynamicEvalProgramStages);
    swap(lhs.dynamicEvalNoRegisterSurfaces, rhs.dynamicEvalNoRegisterSurfaces);
    swap(lhs.dynamicEvalNoSelectedStageSurfaces,
        rhs.dynamicEvalNoSelectedStageSurfaces);
    for (int i = 0; i < RT_SMOKE_DYNAMIC_MATERIAL_REASON_SAMPLES; ++i)
    {
        SwapPathTraceOwnerDynamicMaterialSample(
            lhs.dynamicEvalSamples[i], rhs.dynamicEvalSamples[i]);
    }
    swap(lhs.dynamicEvalSampleCount, rhs.dynamicEvalSampleCount);
    lhs.dynamicEvalMaterialSamples.swap(rhs.dynamicEvalMaterialSamples);
    swap(lhs.translucentSubtypeSurfaces, rhs.translucentSubtypeSurfaces);
    swap(lhs.translucentSubtypeTriangles, rhs.translucentSubtypeTriangles);
    for (int subtype = 0; subtype < RT_SMOKE_TRANSLUCENT_SUBTYPE_COUNT; ++subtype)
    {
        for (int sample = 0; sample < RT_SMOKE_MATERIAL_REASON_SAMPLES; ++sample)
        {
            SwapPathTraceOwnerMaterialSample(
                lhs.translucentSubtypeSamples[subtype][sample],
                rhs.translucentSubtypeSamples[subtype][sample]);
        }
    }
    swap(lhs.translucentSubtypeSampleCounts,
        rhs.translucentSubtypeSampleCounts);
    for (int i = 0; i < RT_SMOKE_TRANSLUCENT_REASON_SAMPLES; ++i)
    {
        SwapPathTraceOwnerTranslucentDebugSample(
            lhs.translucentDebugSamples[i], rhs.translucentDebugSamples[i]);
    }
    swap(lhs.translucentDebugSampleCount, rhs.translucentDebugSampleCount);
}

// Transaction candidate for every live output touched by product geometry plus
// Harvest finalization.  It starts as a full copy so legitimate prior frame
// content is preserved, and reaches live state only through one noexcept swap.
struct RtPathTraceOwnerFrameStaging
{
    std::vector<PathTraceSmokeVertex> vertices;
    std::vector<std::uint32_t> indexes;
    std::vector<std::uint32_t> triangleClasses;
    std::vector<std::uint32_t> triangleMaterials;
    std::vector<std::uint32_t> triangleInstances;
    std::vector<std::uint32_t> triangleIdentities;
    RtSmokeBucketRanges bucketRanges;
    int sourceSurfaces = 0;
    int sourceVerts = 0;
    int sourceIndexes = 0;
    RtSmokeSurfaceClassStats classStats;
    RtSmokeSurfaceSkipStats skipStats;
    RtSmokeDynamicGeometryStats dynamicStats;
    RtSmokeMaterialStats materialStats;
    RtSmokeSceneCaptureTiming captureTiming;
    std::vector<std::uint64_t> rigidCaptureWalked;
    std::vector<std::uint32_t> rigidCaptureWalkedTriangles;
    std::vector<RtSmokeMergedWalkedRange> mergedWalkedRanges;

    void CommitTo(
        std::vector<PathTraceSmokeVertex>& liveVertices,
        std::vector<std::uint32_t>& liveIndexes,
        std::vector<std::uint32_t>& liveTriangleClasses,
        std::vector<std::uint32_t>& liveTriangleMaterials,
        std::vector<std::uint32_t>& liveTriangleInstances,
        std::vector<std::uint32_t>& liveTriangleIdentities,
        RtSmokeBucketRanges& liveBucketRanges,
        int& liveSourceSurfaces,
        int& liveSourceVerts,
        int& liveSourceIndexes,
        RtSmokeSurfaceClassStats& liveClassStats,
        RtSmokeSurfaceSkipStats& liveSkipStats,
        RtSmokeDynamicGeometryStats& liveDynamicStats,
        RtSmokeMaterialStats& liveMaterialStats,
        RtSmokeSceneCaptureTiming& liveCaptureTiming,
        std::vector<std::uint64_t>& liveRigidCaptureWalked,
        std::vector<std::uint32_t>& liveRigidCaptureWalkedTriangles,
        std::vector<RtSmokeMergedWalkedRange>& liveMergedWalkedRanges) noexcept
    {
        using std::swap;
        swap(vertices, liveVertices);
        swap(indexes, liveIndexes);
        swap(triangleClasses, liveTriangleClasses);
        swap(triangleMaterials, liveTriangleMaterials);
        swap(triangleInstances, liveTriangleInstances);
        swap(triangleIdentities, liveTriangleIdentities);
        swap(bucketRanges, liveBucketRanges);
        swap(sourceSurfaces, liveSourceSurfaces);
        swap(sourceVerts, liveSourceVerts);
        swap(sourceIndexes, liveSourceIndexes);
        swap(classStats, liveClassStats);
        swap(skipStats, liveSkipStats);
        swap(dynamicStats, liveDynamicStats);
        SwapPathTraceOwnerMaterialStats(materialStats, liveMaterialStats);
        swap(captureTiming, liveCaptureTiming);
        swap(rigidCaptureWalked, liveRigidCaptureWalked);
        swap(rigidCaptureWalkedTriangles, liveRigidCaptureWalkedTriangles);
        swap(mergedWalkedRanges, liveMergedWalkedRanges);
    }
};

static_assert(std::is_nothrow_move_constructible<idStr>::value &&
        std::is_nothrow_move_assignable<idStr>::value,
    "owner-frame string commit must not allocate or throw");

struct RtPathTraceDrawSurfMirrorSurfaceCache
{
    bool valid = false;
    const drawSurf_t* drawSurf = nullptr;
    const srfTriangles_t* tri = nullptr;
    RtSmokeSurfaceSkipStats skipStats;
    RtSmokeSurfaceClass classifiedSurfaceClass = RtSmokeSurfaceClass::Unknown;
    RtSmokeSurfaceClass surfaceClass = RtSmokeSurfaceClass::Unknown;
    RtSmokeTranslucentSubtype translucentSubtype = RtSmokeTranslucentSubtype::Unknown;
    uint64 legacyStaticKey = 0;
    uint32_t baseMaterialId = 0;
    uint32_t materialId = 0;
    uint32_t sourceKind = 0;
    uint32_t sourceFlags = 0;
    uint32_t surfaceClassId = 0;
    uint32_t surfaceClassAndFlags = 0;
    uint32_t materialClassSignature = 0;
};

void CapturePathTraceDrawSurfMirror(
    const viewDef_t* viewDef,
    const RtPathTraceSceneUniverse* sceneUniverse,
    RtSmokeGeometryUniverse* geometryUniverse,
    RtPathTraceInstanceUniverse& instanceUniverse,
    std::vector<RtPathTraceBoundsOverlayLine>* boundsOverlayLines = nullptr,
    const std::vector<RtPathTraceDrawSurfMirrorSurfaceCache>* surfaceCache = nullptr);

uint64 BuildPathTraceSkinnedCaptureViewSignature(
    const viewDef_t* viewDef);

bool CapturePathTraceDynamicFrameFromDrawSurfMirror(
    const viewDef_t* viewDef,
    const RtPathTraceSceneUniverse* sceneUniverse,
    RtSmokeGeometryUniverse* geometryUniverse,
    std::vector<PathTraceSmokeVertex>& vertexData,
    std::vector<uint32_t>& indexData,
    std::vector<uint32_t>& triangleClassData,
    std::vector<uint32_t>& triangleMaterialData,
    std::vector<uint32_t>* triangleInstanceData,
    std::vector<uint32_t>* triangleIdentityData,
    int& sourceSurfaces,
    int& sourceVerts,
    int& sourceIndexes,
    RtSmokeSurfaceClassStats& classStats,
    RtSmokeSurfaceSkipStats& skipStats,
    RtSmokeDynamicGeometryStats& dynamicStats,
    RtSmokeAttributeStats& attributeStats,
    RtSmokeMaterialStats& materialStats,
    RtSmokeBucketRanges& bucketRanges,
    RtSmokeSceneCaptureTiming& captureTiming,
    std::vector<RtSmokeSkinnedSurfaceRecord>* skinnedSurfaceRecords = nullptr,
    std::vector<RtSmokeCapturedSurfaceRecord>* capturedSurfaceRecords = nullptr,
    std::vector<RtPathTraceDrawSurfMirrorSurfaceCache>* surfaceCache = nullptr,
    RtPathTraceInstanceUniverse* instanceUniverse = nullptr,
    std::vector<RtPathTraceBoundsOverlayLine>* boundsOverlayLines = nullptr,
    bool recordAllInstanceClasses = false,
    const std::vector<PtSkinnedHitRouteRecord>*
        skinnedCaptureAdmissionRoutes = nullptr,
    std::vector<RtSmokeRigidCaptureSkipRecord>* rigidCaptureSkips = nullptr,
    std::vector<uint64_t>* rigidCaptureWalked = nullptr,
    std::vector<uint32_t>* rigidCaptureWalkedTriangles = nullptr,
    std::vector<RtSmokeMergedWalkedRange>* mergedWalkedRanges = nullptr,
    RtPathTraceOwnerHarvest* ownerHarvest = nullptr,
    std::vector<RtPathTraceRigidMeshCandidateObservation>*
        deferredRigidCandidates = nullptr);

bool AppendPathTraceOwnerHarvestGeometry(
    const RtPathTraceOwnerHarvest& harvest,
    std::vector<PathTraceSmokeVertex>& vertexData,
    std::vector<uint32_t>& indexData,
    std::vector<uint32_t>& triangleClassData,
    std::vector<uint32_t>& triangleMaterialData,
    std::vector<uint32_t>* triangleInstanceData,
    std::vector<uint32_t>* triangleIdentityData,
    int& sourceSurfaces,
    int& sourceVerts,
    int& sourceIndexes,
    RtSmokeSurfaceClassStats& classStats,
    RtSmokeSurfaceSkipStats& skipStats,
    RtSmokeDynamicGeometryStats& dynamicStats,
    RtSmokeAttributeStats& attributeStats,
    RtSmokeMaterialStats& materialStats,
    RtSmokeBucketRanges& bucketRanges,
    RtSmokeSceneCaptureTiming& captureTiming,
    std::vector<RtSmokeSkinnedSurfaceRecord>* skinnedSurfaceRecords,
    std::vector<RtSmokeCapturedSurfaceRecord>* capturedSurfaceRecords,
    std::vector<uint64_t>* rigidCaptureWalked,
    std::vector<uint32_t>* rigidCaptureWalkedTriangles,
    std::vector<RtSmokeMergedWalkedRange>* mergedWalkedRanges);

bool FinalizePathTraceOwnerHarvestWithProduct(
    const RtPathTraceOwnerHarvest& harvest,
    const RtPathTraceCaptureProduct& product,
    int& sourceSurfaces,
    int& sourceVerts,
    int& sourceIndexes,
    RtSmokeSurfaceClassStats& classStats,
    RtSmokeSurfaceSkipStats& skipStats,
    RtSmokeDynamicGeometryStats& dynamicStats,
    RtSmokeMaterialStats& materialStats,
    RtSmokeBucketRanges& bucketRanges,
    RtSmokeSceneCaptureTiming& captureTiming,
    std::vector<uint64_t>* rigidCaptureWalked,
    std::vector<uint32_t>* rigidCaptureWalkedTriangles,
    std::vector<RtSmokeMergedWalkedRange>* mergedWalkedRanges);
bool PathTraceOwnerHarvestProductEligible(
    const RtPathTraceOwnerHarvest& harvest,
    const RtPathTraceCaptureProduct& product);

// Owner-only evaluation; pointers never enter producer jobs. Runtime stage selection
// remains authoritative, with authored diffuse UVs when it supplies no matrix.
void BuildSmokeSurfaceTextureMatrices(const drawSurf_t* surface, int selectedStage,
    float primary[6], float normal[6]);
