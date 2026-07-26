#pragma once

// Doom render-surface capture for the RT smoke/path tracing scene.
//
// Converts the current view's draw surfaces into compact static and dynamic
// triangle buckets. This is the boundary where Doom material/surface
// classification, CPU skinning, GUI admission, and scene-origin anchoring meet.

#include "PathTraceDoomMaterialClassifier.h"
#include "PathTraceGeometry.h"
#include "PathTraceGeometryUniverse.h"
#include "PathTraceSkinnedHistoryPolicy.h"
#include "PathTraceSurfaceClassification.h"

#include <cstdint>
#include <vector>

struct drawSurf_t;
struct srfTriangles_t;
struct viewDef_t;
class idRenderEntityLocal;
class idJointMat;
class idImage;
class idMaterial;

const int RT_SMOKE_MAX_SURFACES = 128;
const int RT_SMOKE_MAX_VERTS = 65536;
const int RT_SMOKE_MAX_INDEXES = 196608;
const int RT_SMOKE_MATERIAL_REASON_SAMPLES = 12;
const int RT_SMOKE_TRANSLUCENT_REASON_SAMPLES = 24;
const int RT_SMOKE_DYNAMIC_MATERIAL_REASON_SAMPLES = 12;
const uint32_t RT_SMOKE_TRIANGLE_CLASS_MASK = 0x0000ffffu;
const uint32_t RT_SMOKE_TRIANGLE_FORCE_GEOMETRIC_NORMAL = 0x00010000u;
const uint32_t RT_SMOKE_EMISSIVE_TRIANGLE_HISTORY_DYNAMIC = 0x00020000u;
const uint32_t RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF = 0x00040000u;

struct RtSmokeSurfaceClassStats
{
    int staticWorldSurfaces = 0;
    int rigidEntitySurfaces = 0;
    int skinnedDeformedSurfaces = 0;
    int particleAlphaSurfaces = 0;
    int unknownSurfaces = 0;
    int staticWorldVerts = 0;
    int rigidEntityVerts = 0;
    int skinnedDeformedVerts = 0;
    int particleAlphaVerts = 0;
    int unknownVerts = 0;
    int staticWorldIndexes = 0;
    int rigidEntityIndexes = 0;
    int skinnedDeformedIndexes = 0;
    int particleAlphaIndexes = 0;
    int unknownIndexes = 0;
    int staticWorldTriangles = 0;
    int rigidEntityTriangles = 0;
    int skinnedDeformedTriangles = 0;
    int particleAlphaTriangles = 0;
    int unknownTriangles = 0;
};

struct RtSmokeDynamicGeometryStats
{
    int rigidSurfaces = 0;
    int skinnedCpuCurrentSurfaces = 0;
    int skinnedLikelyBasePoseSurfaces = 0;
    int skinnedRtCpuSkinnedSurfaces = 0;
    int particleAlphaSurfaces = 0;
    int unknownSurfaces = 0;
    int retainedOccluderSurfaces = 0;
    int rigidIndexes = 0;
    int skinnedCpuCurrentIndexes = 0;
    int skinnedLikelyBasePoseIndexes = 0;
    int skinnedRtCpuSkinnedIndexes = 0;
    int particleAlphaIndexes = 0;
    int unknownIndexes = 0;
    int retainedOccluderIndexes = 0;
};

struct RtSmokeMaterialSample
{
    uint32_t id = 0;
    int surfaces = 0;
    int triangles = 0;
    idStr name;
};

static const int RT_SMOKE_DYNAMIC_ORDERED_STAGE_CAPACITY = 8;

struct RtSmokeDynamicStageEval
{
    int stageIndex = -1;
    bool enabled = false;
    bool emissive = false;
    bool hasAlphaTest = false;
    bool hasTexMatrix = false;
    float condition = 1.0f;
    float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float alphaTest = 0.0f;
    float texMatrix[2][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
};

struct RtSmokeDynamicMaterialEvalSample
{
    bool valid = false;
    uint32_t id = 0;
    int surfaces = 0;
    int triangles = 0;
    int stageIndex = -1;
    int enabledStages = 0;
    int disabledStages = 0;
    int colorStages = 0;
    int alphaStages = 0;
    int alphaTestStages = 0;
    int texMatrixStages = 0;
    int dynamicImageStages = 0;
    int cinematicStages = 0;
    int guiRenderTargetStages = 0;
    int programStages = 0;
    idStr name;
    int stagePriority = -1;
    bool selectedStageEmissive = false;
    float condition = 1.0f;
    float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float alphaTest = 0.0f;
    idImage* image = nullptr;
    float texMatrix[2][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    // Detail-decal channels: the DIFFUSE stage's evaluated color (generic stage
    // selection can pick a white bump stage over the authored tint), and a
    // representative world position for spectrum light association.
    bool hasDiffuseStageColor = false;
    float diffuseStageColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float diffuseStageCondition = 1.0f;
    RtSmokeDynamicStageEval orderedStages[RT_SMOKE_DYNAMIC_ORDERED_STAGE_CAPACITY];
    int orderedStageCount = 0;
    bool orderedStageOverflow = false;
    bool hasSurfaceOrigin = false;
    idVec3 surfaceOrigin = idVec3(0.0f, 0.0f, 0.0f);
};

struct RtSmokeTranslucentSubtypeDebugSample
{
    bool valid = false;
    RtSmokeTranslucentSubtype subtype = RtSmokeTranslucentSubtype::Unknown;
    int surfaceIndex = -1;
    int verts = 0;
    int indexes = 0;
    idStr materialName;
    materialCoverage_t coverage = MC_BAD;
    float sort = SS_BAD;
    deform_t deform = DFRM_NONE;
    RtSmokeTranslucentClassifierInfo info;
};

struct RtSmokeMaterialStats
{
    int totalSurfaces = 0;
    int totalTriangles = 0;
    int uniqueMaterials = 0;
    int translucentSurfaces = 0;
    int translucentTriangles = 0;
    int translucentUniqueMaterials = 0;
    std::vector<uint32_t> materialIds;
    std::vector<uint32_t> translucentMaterialIds;
    RtSmokeMaterialSample samples[RT_SMOKE_MATERIAL_REASON_SAMPLES];
    RtSmokeMaterialSample translucentSamples[RT_SMOKE_MATERIAL_REASON_SAMPLES];
    int sampleCount = 0;
    int translucentSampleCount = 0;
    int dynamicEvalSurfaces = 0;
    int dynamicEvalTriangles = 0;
    int dynamicEvalStages = 0;
    int dynamicEvalEnabledStages = 0;
    int dynamicEvalDisabledStages = 0;
    int dynamicEvalColorStages = 0;
    int dynamicEvalAlphaStages = 0;
    int dynamicEvalAlphaTestStages = 0;
    int dynamicEvalTexMatrixStages = 0;
    int dynamicEvalDynamicImageStages = 0;
    int dynamicEvalCinematicStages = 0;
    int dynamicEvalGuiRenderTargetStages = 0;
    int dynamicEvalProgramStages = 0;
    int dynamicEvalNoRegisterSurfaces = 0;
    int dynamicEvalNoSelectedStageSurfaces = 0;
    RtSmokeDynamicMaterialEvalSample dynamicEvalSamples[RT_SMOKE_DYNAMIC_MATERIAL_REASON_SAMPLES];
    int dynamicEvalSampleCount = 0;
    std::vector<RtSmokeDynamicMaterialEvalSample> dynamicEvalMaterialSamples;
    int translucentSubtypeSurfaces[RT_SMOKE_TRANSLUCENT_SUBTYPE_COUNT] = {};
    int translucentSubtypeTriangles[RT_SMOKE_TRANSLUCENT_SUBTYPE_COUNT] = {};
    RtSmokeMaterialSample translucentSubtypeSamples[RT_SMOKE_TRANSLUCENT_SUBTYPE_COUNT][RT_SMOKE_MATERIAL_REASON_SAMPLES];
    int translucentSubtypeSampleCounts[RT_SMOKE_TRANSLUCENT_SUBTYPE_COUNT] = {};
    RtSmokeTranslucentSubtypeDebugSample translucentDebugSamples[RT_SMOKE_TRANSLUCENT_REASON_SAMPLES];
    int translucentDebugSampleCount = 0;
};

struct RtSmokeBucketRange
{
    int vertexOffset = 0;
    int vertexCount = 0;
    int indexOffset = 0;
    int indexCount = 0;
    int triangleOffset = 0;
    int triangleCount = 0;
    int surfaceCount = 0;
};

struct RtSmokeBucketRanges
{
    RtSmokeBucketRange buckets[RT_SMOKE_CLASS_COUNT];
};

struct RtSmokeSkinnedSurfaceKey
{
    // Bridge-grade identity for the CPU-skinned previous-frame path. The scene
    // reset/invalidation policy keeps these pointer fields conservative, but
    // future PT-owned temporal consumers need explicit stable instance and
    // geometry IDs instead of treating this as the final identity contract.
    int entityIndex = -1;
    uintptr_t entityDef = 0;
    uintptr_t model = 0;
    uintptr_t tri = 0;
    uint32_t materialId = 0;
    uint32_t surfaceClassId = 0;
};

struct RtSmokeSkinnedSurfaceRecord
{
    RtSmokeSkinnedSurfaceKey key;
    PtCanonicalHistoryOwnerKey historyOwner;
    PtCanonicalInstanceKey canonicalInstance;
    uint64_t jointCacheHandle = 0;
    uintptr_t jointCacheCpuSnapshot = 0;
    int jointCacheCpuSnapshotCount = 0;
    bool jointCacheCpuSourceComparable = false;
    bool jointCacheCpuSourceChanged = false;
    int currentVertexOffset = 0;
    int currentIndexOffset = 0;
    int currentTriangleOffset = 0;
    int vertexCount = 0;
    int indexCount = 0;
    int triangleCount = 0;
    int previousVertexOffset = -1;
    int previousIndexOffset = -1;
    int previousTriangleOffset = -1;
    bool previousValid = false;
    bool rtCpuSkinned = false;
    bool cpuCaptureOmitted = false;
    bool basePoseLikely = false;
    int entityIndex = -1;
    int drawSurfIndex = -1;
    int modelSurfaceIndex = -1;
    idStr modelName;
    uint32_t materialId = 0;
    uint32_t triangleClassAndFlags = 0;
    uint32_t invalidReasonFlags = RT_SMOKE_SKINNED_INVALID_NONE;
    uint32_t temporalStateFlags = 0;
    int jointCount = 0;
    uintptr_t jointSource = 0;
    int retainedVertexOffset = -1;
    int retainedJointOffset = -1;
    int gpuSourceVertexOffset = -1;
    int64 gpuOutputVertexOffset = -1;
    int gpuPreviousPositionOffset = -1;
    int bucketIndex = 0;
    bool hasEntityOrigin = false;
    idVec3 entityOrigin = vec3_origin;
    float objectToWorld[12] = {};
};

struct RtSmokeSceneCaptureTiming
{
    int anchorMs = 0;
    int anchorSurfaceTests = 0;
    int anchorBoundsRejects = 0;
    int anchorTriangleTests = 0;
    int validationMs = 0;
    int staticPassClassifyMs = 0;
    int staticCacheLookupMs = 0;
    int staticAppendMs = 0;
    int dynamicPassClassifyMs = 0;
    int dynamicAppendMs = 0;
    int rtCpuSkinningAppendMs = 0;
    uint64 rtCpuSkinningAppendUs = 0;
    int skinnedCaptureAdmissionRoutes = 0;
    int skinnedCaptureOmittedSurfaces = 0;
    int skinnedCaptureOmittedVerts = 0;
    int skinnedCaptureOmittedIndexes = 0;
    int skinnedCaptureFallbackGate = 0;
    int skinnedCaptureFallbackPriorRoute = 0;
    int skinnedCaptureFallbackCurrentContract = 0;
    int skinnedCaptureFallbackJointData = 0;
    int appendMs = 0;
    int bucketMergeMs = 0;
    int staticCachedSurfaces = 0;
    int staticNewSurfaces = 0;
};

struct RtSmokeSurfaceSkipStats
{
    int nullSurface = 0;
    int missingGeometry = 0;
    int nullMaterial = 0;
    int nullSpace = 0;
    int nullModel = 0;
    int invalidIndexCount = 0;
    int conditionedOff = 0;
    int nonCurrentCache = 0;
    int limitExceeded = 0;
    int zeroAreaOnly = 0;
    int emptyClassBuffer = 0;
    int guiSurface = 0;
    int callbackEntity = 0;
};

struct RtSmokeAttributeClassStats
{
    int invalidNormalVerts = 0;
    int invalidNormalTriangles = 0;
    int invalidUvVerts = 0;
    int invalidUvTriangles = 0;
    int forcedGeometricNormalTriangles = 0;
};

struct RtSmokeAttributeStats
{
    RtSmokeAttributeClassStats classes[5];
};

struct RtSmokeTranslucentClassifierInfo;

void TransformSurfacePointToWorld(const drawSurf_t* drawSurf, const idVec3& localPoint, idVec3& worldPoint);
void TransformSurfaceVectorToWorld(const drawSurf_t* drawSurf, const idVec3& localVector, idVec3& worldVector);
void ApplySmokeDetailDecalNormalOffset(
    const idMaterial* material,
    uint64 surfaceOffsetKey,
    bool liquidOnlyStaticRouteEligible,
    std::vector<PathTraceSmokeVertex>& vertices,
    const std::vector<uint32_t>& indexes,
    size_t vertexStart,
    size_t indexStart);
bool ValidateSmokeDrawSurface(const viewDef_t* viewDef, const drawSurf_t* drawSurf, const srfTriangles_t*& tri, RtSmokeSurfaceSkipStats* skipStats);
uint64 BuildSmokeStaticSurfaceKeyForDiagnostics(const drawSurf_t* drawSurf, const srfTriangles_t* tri);
void AddSmokeDynamicMaterialEvalStats(RtSmokeMaterialStats& stats, const drawSurf_t* drawSurf, int indexes);
void AddSmokeDynamicMaterialEvalStatsForMaterialId(RtSmokeMaterialStats& stats, const drawSurf_t* drawSurf, int indexes, uint32_t materialId);
bool BuildSmokeDynamicMaterialEvalSampleForDrawSurf(const drawSurf_t* drawSurf, uint32_t materialId, RtSmokeDynamicMaterialEvalSample& sample);
uint32_t SmokeRuntimeMaterialVariantIdForDrawSurf(const drawSurf_t* drawSurf, uint32_t baseMaterialId);
uint32_t SmokeRuntimeMaterialTableIdForDrawSurf(const drawSurf_t* drawSurf, uint32_t baseMaterialId);
uint32_t SmokeRuntimeMaterialTableIdForEntitySurface(const idRenderEntityLocal* entity, int modelSurfaceIndex, const idMaterial* material, uint32_t baseMaterialId);
bool SmokeDrawSurfaceHasActiveEmissiveStage(const drawSurf_t* drawSurf);
bool SmokeEntitySurfaceHasActiveEmissiveStage(const viewDef_t* viewDef, const idRenderEntityLocal* entity, const idMaterial* material);
bool SmokeEntitySurfaceHasActiveEmissiveStage(const viewDef_t* viewDef, const idRenderEntityLocal* entity, const idMaterial* material, const RtSmokeTranslucentClassifierInfo& classifier);
bool FindCenterCameraRayAnchor(const viewDef_t* viewDef, idVec3& anchorPoint, int& anchorSurface, int& anchorTriangle, RtSmokeSceneCaptureTiming* captureTiming = nullptr);
PathTraceSmokeVertex BuildSmokeSurfaceVertex(const drawSurf_t* drawSurf, const srfTriangles_t* tri, int vertexIndex, const idJointMat* rtCpuSkinningJoints);
void TransformSmokeSurfaceVertexToWorld(const drawSurf_t* drawSurf, const srfTriangles_t* tri, int vertexIndex, const idJointMat* rtCpuSkinningJoints, idVec3& worldPosition);
int AppendSmokeSurfaceGeometry(
    const drawSurf_t* drawSurf,
    const srfTriangles_t* tri,
    uint32_t surfaceClassId,
    uint32_t materialId,
    int classCount,
    uint32_t triangleClassMask,
    uint32_t particleAlphaClassId,
    uint32_t forceGeometricNormalFlag,
    std::vector<PathTraceSmokeVertex>& vertices,
    std::vector<uint32_t>& indexes,
    std::vector<uint32_t>& triangleClasses,
    std::vector<uint32_t>& triangleMaterials,
    RtSmokeSurfaceSkipStats& skipStats,
    RtSmokeAttributeStats& attributeStats);
void AddSmokeSkinnedSurfaceRecord(
    std::vector<RtSmokeSkinnedSurfaceRecord>* records,
    const drawSurf_t* drawSurf,
    const srfTriangles_t* tri,
    uint32_t surfaceClassId,
    uint32_t materialId,
    int drawSurfIndex,
    int bucketIndex,
    int currentVertexOffset,
    int currentIndexOffset,
    int currentTriangleOffset,
    int vertexCount,
    int indexCount,
    int triangleCount);
void FinalizeSmokeSkinnedSurfaceRecordOffsets(
    std::vector<RtSmokeSkinnedSurfaceRecord>* records,
    int bucketIndex,
    const RtSmokeBucketRange& range);

bool CaptureDoomSurfacesForSmokeTest(
    const viewDef_t* viewDef,
    std::vector<PathTraceSmokeVertex>& vertexData,
    std::vector<uint32_t>& indexData,
    std::vector<uint32_t>& triangleClassData,
    std::vector<uint32_t>& triangleMaterialData,
    std::vector<uint32_t>* triangleInstanceData,
    std::vector<uint32_t>* triangleIdentityData,
    RtSmokeGeometryUniverse& geometryUniverse,
    bool& staticCacheChanged,
    idVec3& sceneOrigin,
    int& sourceSurfaces,
    int& sourceVerts,
    int& sourceIndexes,
    int& anchorTriangle,
    RtSmokeSurfaceClassStats& classStats,
    RtSmokeSurfaceSkipStats& skipStats,
    RtSmokeDynamicGeometryStats& dynamicStats,
    RtSmokeAttributeStats& attributeStats,
    RtSmokeMaterialStats& materialStats,
    RtSmokeBucketRanges& bucketRanges,
    RtSmokeSceneCaptureTiming& captureTiming,
    std::vector<RtSmokeSkinnedSurfaceRecord>* skinnedSurfaceRecords = nullptr,
    bool skipStaticWorldCapture = false,
    bool skipPromotedStaticSurfaceCapture = false,
    bool skipDynamicCapture = false);
