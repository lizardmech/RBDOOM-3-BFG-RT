#pragma once

// Console diagnostics for the RT smoke/path tracing prototype.
//
// Formats scene, material, texture, timing, and one-shot CVar dumps. This module
// should not own render resources or mutate scene state except for explicit
// diagnostic CVar reset/latch behavior.

#include "PathTraceDynamicMaterialState.h"
#include "PathTraceMaterialClassifier.h"
#include "PathTraceSceneCapture.h"
#include "../Image.h"
#include "../Material.h"
#include "../Model.h"

#include <vector>

const int RT_SMOKE_DEBUG_TEXTURE_COVERAGE_CLASS_COUNT = 5;

struct viewDef_t;
struct RtPathTraceRigidRouteBuild;

struct RtPathTraceDebugModeInfo
{
    int mode = 0;
    const char* name = "unknown";
    const char* category = "unknown";
    const char* output = "unknown";
    const char* owner = "legacy-smoke-raygen";
    bool behaviorChanging = false;
    bool temporary = false;
};

struct RtPathTraceDispatchTimingLogDesc
{
    double totalSubmitMs = 0.0;
    double setupMs = 0.0;
    double constantsMs = 0.0;
    double barrierMs = 0.0;
    double primaryHistoryClearMs = 0.0;
    double targetClearMs = 0.0;
    double setStateMs = 0.0;
    double dispatchSubmitMs = 0.0;
    double historyCopyMs = 0.0;
    double readbackCopyMs = 0.0;
    int outputWidth = 0;
    int outputHeight = 0;
    int dispatchWidth = 0;
    int dispatchHeight = 0;
    int debugMode = 0;
    int samplesPerPixel = 1;
    int maxPathDepth = 1;
    int estimatedRaysPerPixel = 1;
    int selectedLights = 0;
    int analyticLights = 0;
    bool primaryHistoryClearRequested = false;
    bool readbackQueued = false;
    bool optickGpuMarkers = false;
    bool nsightGpuMarkers = false;
    const RtPathTraceDebugModeInfo* debugModeInfo = nullptr;
};

struct RtSmokeTextureCoverageClassStats
{
    int triangles = 0;
    int boundTriangles = 0;
    int fallbackTriangles = 0;
    int invalidMaterialTriangles = 0;
};

struct RtSmokeTextureCoverageStats
{
    RtSmokeTextureCoverageClassStats classes[RT_SMOKE_DEBUG_TEXTURE_COVERAGE_CLASS_COUNT];
    int materials = 0;
    int boundMaterials = 0;
    int fallbackMaterials = 0;
};

struct RtSmokeSlowSceneBuildLogDesc
{
    int sceneMs = 0;
    int captureMs = 0;
    int captureAnchorMs = 0;
    int captureValidationMs = 0;
    int captureStaticPassClassifyMs = 0;
    int captureStaticCacheLookupMs = 0;
    int captureStaticAppendMs = 0;
    int captureDynamicPassClassifyMs = 0;
    int captureDynamicAppendMs = 0;
    int captureRtCpuSkinningAppendMs = 0;
    int captureAppendMs = 0;
    int captureBucketMergeMs = 0;
    int metadataMs = 0;
    int metadataValidationMs = 0;
    int metadataRegistrationMs = 0;
    int materialMs = 0;
    int emissiveMs = 0;
    int bufferCreateMs = 0;
    int bufferUploadMs = 0;
    int bvhFramePlanMs = 0;
    int accelSubmitMs = 0;
    int blasSubmitMs = 0;
    int tlasSubmitMs = 0;
    int sourceSurfaces = 0;
    int sourceVerts = 0;
    int sourceIndexes = 0;
    int dynamicIndexCount = 0;
    int staticCachedSurfaces = 0;
    int staticNewSurfaces = 0;
    int staticSurfaceCacheSize = 0;
    int staticVertexCacheCount = 0;
    int staticIndexCacheCount = 0;
    int staticTriangleCacheCount = 0;
    int staticSeenThisFrame = 0;
    int staticNewThisFrame = 0;
    int staticDisappearedThisFrame = 0;
    int staticHistoryValid = 0;
    int staticPreviousRangeValid = 0;
    int staticDirty = 0;
    int staticValidationErrors = 0;
    int staticRangeErrors = 0;
    int staticDuplicateKeys = 0;
    int staticHistoryErrors = 0;
    int staticKeyVectorMismatches = 0;
    int staticCacheBytesKB = 0;
    bool staticBlasSignatureReused = false;
    int staticBlasSignatureMs = 0;
    int anchorSurfaceTests = 0;
    int anchorBoundsRejects = 0;
    int anchorTriangleTests = 0;
    int skinnedRtCpuSurfaces = 0;
    int skinnedRtCpuIndexes = 0;
    bool staticBlasCacheHit = false;
    bool materialTableCacheHit = false;
    const char* materialTablePath = "legacy";
    int materialTableCacheHits = 0;
    int materialTableCacheMisses = 0;
    RtSmokeMaterialTableBuildStats materialTableBuildStats;
    RtSmokeMaterialUniverseStats materialUniverseStats;
    RtSmokeMaterialTableCompareStats materialUniverseTableCompareStats;
    int materialUniverseMaterialCount = 0;
    bool materialMetadataCacheEnabled = false;
    int metadataCacheRefreshes = 0;
    int metadataFullDiscovers = 0;
    int metadataNewEntries = 0;
    int metadataRegistrations = 0;
    int metadataDuplicateSkips = 0;
    int metadataRegistrySize = 0;
    int guiTextureCandidates = 0;
    int guiTexturesAccepted = 0;
    int guiTexturesRejected = 0;
    int additiveDecals = 0;
    int lightCandidateCount = 0;
    int texturedLightCandidateCount = 0;
    int lightCandidateBytes = 0;
    int lightCount = 0;
    int debugMode = 0;
};

struct RtSmokeSceneBuildSummaryLogDesc
{
    int sourceSurfaces = 0;
    int sourceVerts = 0;
    int sourceIndexes = 0;
    int anchorTriangle = -1;
    int staticIndexCount = 0;
    int staticVertexCount = 0;
    int dynamicIndexCount = 0;
    int dynamicVertexCount = 0;
    int instanceCount = 0;
    int rigidTlasInstanceCount = 0;
    int rigidRouteInstanceCount = 0;
    int rigidRouteUniqueMeshes = 0;
    int rigidRouteVertexCount = 0;
    int rigidRouteIndexCount = 0;
    int rigidRouteTriangleCount = 0;
    int staticBvhResidentBuckets = 0;
    int staticBvhActiveBuckets = 0;
    int staticBvhInactiveResidentBuckets = 0;
    int staticBvhEmittedInstances = 0;
    int staticBucketBlasRecords = 0;
    int staticBucketBlasSkippedInactive = 0;
    int staticBucketBlasSkippedInvalid = 0;
    bool staticBucketBlasOverflow = false;
    bool staticBucketTraversalRouteRequired = false;
    bool staticBucketTraversalCurrentShaderCompatible = true;
    bool staticBucketTraversalExactMonolithic = false;
    int staticBucketTraversalNonZeroOffsetRecords = 0;
    bool staticRouteNamespaceBlocked = false;
    uint32 staticRouteNamespaceFirst = 0;
    uint32 rigidRouteNamespaceFirst = 2;
    int staticRouteNamespaceCount = 0;
    bool rigidRouteNamespaceShifted = false;
    bool staticMonolithicInactiveIncluded = false;
    bool staticRequiresBucketedBlas = false;
    bool bvhDirtyPreviousValid = false;
    bool bvhGeometryContentChanged = false;
    bool bvhActiveGeometryContentChanged = false;
    bool bvhResidentSetChanged = false;
    bool bvhMaterialChanged = false;
    bool bvhActiveMembershipChanged = false;
    bool bvhTlasInstanceChanged = false;
    bool bvhBlasInputDirty = false;
    bool bvhTlasDirty = false;
    uint64 bvhActiveSetSignature = 0;
    uint64 bvhResidentSetSignature = 0;
    uint64 bvhGeometryContentSignature = 0;
    uint64 bvhActiveBlasInputSignature = 0;
    uint64 bvhTlasInstanceSignature = 0;
    uint64 staticUploadBytes = 0;
    uint64 previousStaticUploadBytes = 0;
    uint64 previousStaticUploadSkippedBytes = 0;
    uint64 dynamicUploadBytes = 0;
    uint64 rigidRouteUploadBytes = 0;
    uint64 rigidRouteGeometryBytes = 0;
    uint64 rigidRouteInstanceBytes = 0;
    int rigidRouteBuildMs = 0;
    int bvhFramePlanMs = 0;
    int bufferUploadMs = 0;
    int accelSubmitMs = 0;
    int blasSubmitMs = 0;
    int tlasSubmitMs = 0;
    bool staticBlasBuildSubmitted = false;
    bool staticBlasBuildSkipped = false;
    bool dynamicBlasBuildSubmitted = false;
    bool dynamicBlasBuildSkipped = false;
    RtSmokeSurfaceClassStats classStats;
    RtSmokeSurfaceSkipStats skipStats;
    RtSmokeDynamicGeometryStats dynamicStats;
    bool allowGuiSurfaces = false;
    bool skipCallbackEntities = false;
    bool staticBlasCacheHit = false;
    uint64 staticBlasSignature = 0;
    int staticSurfaceCacheSize = 0;
    int staticVertexCacheCount = 0;
    int staticIndexCacheCount = 0;
    int staticTriangleCacheCount = 0;
    int staticSeenThisFrame = 0;
    int staticNewThisFrame = 0;
    int staticDisappearedThisFrame = 0;
    int staticHistoryValid = 0;
    int staticPreviousRangeValid = 0;
    int staticDirty = 0;
    int staticValidationErrors = 0;
    int staticRangeErrors = 0;
    int staticDuplicateKeys = 0;
    int staticHistoryErrors = 0;
    int staticKeyVectorMismatches = 0;
    int staticCacheBytesKB = 0;
    bool staticBlasSignatureReused = false;
    int staticBlasSignatureMs = 0;
    int staticBlasCacheHitCount = 0;
    int staticBlasCacheMissCount = 0;
    bool materialTableCacheHit = false;
    const char* materialTablePath = "legacy";
    uint64 materialTableSignature = 0;
    RtSmokeMaterialTableCacheStats materialTableCacheStats;
    RtSmokeMaterialTableBuildStats materialTableBuildStats;
    RtMaterialClassifierStats materialClassifierStats;
    RtSmokeMaterialUniverseStats materialUniverseStats;
    RtSmokeMaterialTableCompareStats materialUniverseTableCompareStats;
    const RtSmokeMaterialStats* materialStats = nullptr;
    const RtSmokeMaterialTableBuild* materialTable = nullptr;
    const RtSmokeEmissiveInventoryStats* emissiveInventoryStats = nullptr;
    int lightCandidateBytes = 0;
    bool enableTextureProbe = false;
    const RtSmokeTextureCoverageStats* textureCoverageStats = nullptr;
    const RtSmokeAttributeStats* attributeStats = nullptr;
    const RtSmokeBucketRanges* bucketRanges = nullptr;
};

struct RtSmokeMaterialDiagnosticTriggerDesc
{
    const viewDef_t* viewDef = nullptr;
    const RtSmokeMaterialTableBuild* materialTable = nullptr;
    const std::vector<PathTraceDynamicMaterialRecord>* dynamicMaterialRecords = nullptr;
    const std::vector<uint32_t>* dynamicTriangleMaterialIds = nullptr;
    const std::vector<uint32_t>* dynamicTriangleMaterialIndexes = nullptr;
    const std::vector<uint32_t>* staticTriangleMaterialIds = nullptr;
    const std::vector<uint32_t>* staticTriangleMaterialIndexes = nullptr;
    const RtPathTraceRigidRouteBuild* rigidRouteBuild = nullptr;
    bool enableTextureProbe = false;
};

struct RtSmokeEmissiveInventoryDiagnosticTriggerDesc
{
    const RtSmokeMaterialTableBuild* materialTable = nullptr;
    const std::vector<PathTraceSmokeEmissiveTriangle>* emissiveTriangles = nullptr;
    const RtSmokeEmissiveInventoryStats* emissiveInventoryStats = nullptr;
};

struct RtSmokeSceneBuildDiagnosticLogDesc
{
    int sceneMs = 0;
    int captureMs = 0;
    int metadataMs = 0;
    int metadataValidationMs = 0;
    int metadataRegistrationMs = 0;
    int materialMs = 0;
    int emissiveMs = 0;
    int bufferCreateMs = 0;
    int bufferUploadMs = 0;
    int accelSubmitMs = 0;
    int blasSubmitMs = 0;
    int tlasSubmitMs = 0;
    int sourceSurfaces = 0;
    int sourceVerts = 0;
    int sourceIndexes = 0;
    int anchorTriangle = -1;
    int staticIndexCount = 0;
    int staticVertexCount = 0;
    int dynamicIndexCount = 0;
    int dynamicVertexCount = 0;
    int instanceCount = 0;
    int rigidTlasInstanceCount = 0;
    int rigidRouteInstanceCount = 0;
    int rigidRouteUniqueMeshes = 0;
    int rigidRouteVertexCount = 0;
    int rigidRouteIndexCount = 0;
    int rigidRouteTriangleCount = 0;
    int staticBvhResidentBuckets = 0;
    int staticBvhActiveBuckets = 0;
    int staticBvhInactiveResidentBuckets = 0;
    int staticBvhEmittedInstances = 0;
    int staticBucketBlasRecords = 0;
    int staticBucketBlasSkippedInactive = 0;
    int staticBucketBlasSkippedInvalid = 0;
    bool staticBucketBlasOverflow = false;
    bool staticBucketTraversalRouteRequired = false;
    bool staticBucketTraversalCurrentShaderCompatible = true;
    bool staticBucketTraversalExactMonolithic = false;
    int staticBucketTraversalNonZeroOffsetRecords = 0;
    bool staticRouteNamespaceBlocked = false;
    uint32 staticRouteNamespaceFirst = 0;
    uint32 rigidRouteNamespaceFirst = 2;
    int staticRouteNamespaceCount = 0;
    bool rigidRouteNamespaceShifted = false;
    bool staticMonolithicInactiveIncluded = false;
    bool staticRequiresBucketedBlas = false;
    bool bvhDirtyPreviousValid = false;
    bool bvhGeometryContentChanged = false;
    bool bvhActiveGeometryContentChanged = false;
    bool bvhResidentSetChanged = false;
    bool bvhMaterialChanged = false;
    bool bvhActiveMembershipChanged = false;
    bool bvhTlasInstanceChanged = false;
    bool bvhBlasInputDirty = false;
    bool bvhTlasDirty = false;
    uint64 bvhActiveSetSignature = 0;
    uint64 bvhResidentSetSignature = 0;
    uint64 bvhGeometryContentSignature = 0;
    uint64 bvhActiveBlasInputSignature = 0;
    uint64 bvhTlasInstanceSignature = 0;
    int requestedDebugMode = 0;
    uint64 staticUploadBytes = 0;
    uint64 previousStaticUploadBytes = 0;
    uint64 previousStaticUploadSkippedBytes = 0;
    uint64 dynamicUploadBytes = 0;
    uint64 rigidRouteUploadBytes = 0;
    uint64 rigidRouteGeometryBytes = 0;
    uint64 rigidRouteInstanceBytes = 0;
    int rigidRouteBuildMs = 0;
    int bvhFramePlanMs = 0;
    bool staticBlasBuildSubmitted = false;
    bool staticBlasBuildSkipped = false;
    bool dynamicBlasBuildSubmitted = false;
    bool dynamicBlasBuildSkipped = false;
    int staticSurfaceCacheSize = 0;
    int staticVertexCacheCount = 0;
    int staticIndexCacheCount = 0;
    int staticTriangleCacheCount = 0;
    int staticSeenThisFrame = 0;
    int staticNewThisFrame = 0;
    int staticDisappearedThisFrame = 0;
    int staticHistoryValid = 0;
    int staticPreviousRangeValid = 0;
    int staticDirty = 0;
    int staticValidationErrors = 0;
    int staticRangeErrors = 0;
    int staticDuplicateKeys = 0;
    int staticHistoryErrors = 0;
    int staticKeyVectorMismatches = 0;
    int staticCacheBytesKB = 0;
    bool staticBlasSignatureReused = false;
    int staticBlasSignatureMs = 0;
    int staticBlasCacheHitCount = 0;
    int staticBlasCacheMissCount = 0;
    int sceneCaptureLogIntervalFrames = 0;
    bool staticBlasCacheHit = false;
    bool materialTableCacheHit = false;
    const char* materialTablePath = "legacy";
    bool enableTextureProbe = false;
    bool dumpClassReasons = false;
    uint64 staticBlasSignature = 0;
    uint64 materialTableSignature = 0;
    RtSmokeSceneCaptureTiming captureTiming;
    const RtSmokeSurfaceClassStats* classStats = nullptr;
    const RtSmokeSurfaceSkipStats* skipStats = nullptr;
    const RtSmokeDynamicGeometryStats* dynamicStats = nullptr;
    const RtSmokeAttributeStats* attributeStats = nullptr;
    const RtSmokeMaterialStats* materialStats = nullptr;
    const RtSmokeBucketRanges* bucketRanges = nullptr;
    const RtSmokeMaterialTableBuild* materialTable = nullptr;
    const RtSmokeEmissiveInventoryStats* emissiveInventoryStats = nullptr;
    int lightCandidateBytes = 0;
    const RtSmokeMaterialTableCacheStats* materialTableCacheStats = nullptr;
    const RtSmokeMaterialTableBuildStats* materialTableBuildStats = nullptr;
    const RtMaterialClassifierStats* materialClassifierStats = nullptr;
    const RtSmokeMaterialUniverseStats* materialUniverseStats = nullptr;
    const RtSmokeMaterialTableCompareStats* materialUniverseTableCompareStats = nullptr;
    const RtSmokeTextureCoverageStats* textureCoverageStats = nullptr;
    const RtSmokeSurfaceClassReasonSamples* reasonSamples = nullptr;
    int* lastSceneTimingLogMs = nullptr;
    bool* sceneRebuildLogged = nullptr;
    int* sceneLogCooldownFrames = nullptr;
};

const char* SmokeCoverageName(materialCoverage_t coverage);
const char* SmokeStageLightingName(stageLighting_t lighting);
const char* SmokeTexgenName(texgen_t texgen);
const char* SmokeTextureUsageName(textureUsage_t usage);
const char* SmokeTextureColorFormatName(textureColor_t colorFormat);
const char* SmokeDeformName(deform_t deform);
const char* SmokeDynamicModelName(dynamicModel_t dynamicModel);

RtPathTraceDebugModeInfo GetPathTraceDebugModeInfo(int debugMode);
void LogPathTraceDebugModeInfo(const RtPathTraceDebugModeInfo& info);
bool ShouldLogSmokeTiming(int elapsedMs, int nowMs, int& lastLogMs);
void LogPathTraceDispatchTiming(const RtPathTraceDispatchTimingLogDesc& desc);
void LogSmokeSlowSceneBuild(const RtSmokeSlowSceneBuildLogDesc& desc);
void LogSmokeSceneRebuildSummary(const RtSmokeSceneBuildSummaryLogDesc& desc);
void LogSmokeSceneCaptureSummary(const RtSmokeSceneBuildSummaryLogDesc& desc);
void LogSmokeSurfaceClassReasonSamples(const RtSmokeSurfaceClassReasonSamples& samples);
void LogSmokeBucketRanges(const RtSmokeBucketRanges& ranges);
void LogSmokeAttributeStats(const RtSmokeAttributeStats& stats);
void LogSmokeMaterialStats(const RtSmokeMaterialStats& stats);
RtSmokeTextureCoverageStats BuildSmokeTextureCoverageStats(
    const RtSmokeMaterialTableBuild& table,
    const std::vector<uint32_t>& staticTriangleClassData,
    const std::vector<uint32_t>& staticTriangleMaterialIndexes,
    const std::vector<uint32_t>& dynamicTriangleClassData,
    const std::vector<uint32_t>& dynamicTriangleMaterialIndexes);
void LogSmokeMaterialTable(const RtSmokeMaterialTableBuild& table);
void LogSmokeTextureProbe(const RtSmokeMaterialTableBuild& table);
void LogSmokeTextureProbeSwitch(const RtSmokeMaterialTableBuild& table);
void LogSmokeTextureActiveWindow(const RtSmokeMaterialTableBuild& table);
void LogSmokeTextureCoverage(const RtSmokeTextureCoverageStats& stats);
void LogSmokeMaterialTextureDiscovery(const RtSmokeMaterialTableBuild& table);
void RunSmokeMaterialDiagnosticTriggers(const RtSmokeMaterialDiagnosticTriggerDesc& desc);
void RunSmokeEmissiveInventoryDiagnosticTriggers(const RtSmokeEmissiveInventoryDiagnosticTriggerDesc& desc);
void RunSmokeSceneBuildDiagnosticLogs(const RtSmokeSceneBuildDiagnosticLogDesc& desc);
