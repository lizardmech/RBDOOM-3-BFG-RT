#pragma once

// Persistent RT smoke geometry records.
//
// This starts as ownership for the static-world geometry cache that used to
// live directly on PathTracePrimaryPass. Keeping it behind a small module gives
// later work a stable place to add surface records, GPU buffers, and dirty
// ranges without changing shader-visible behavior in this first step.

#include "PathTraceGeometry.h"
#include "PathTraceAccelerationPlan.h"
#include "PathTraceGeometryLifecycle.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

const int RT_PT_RIGID_MESH_CANDIDATE_SAMPLES = 8;
const int RT_PT_RIGID_BLAS_PLAN_SAMPLES = 8;
const int RT_PT_RIGID_BLAS_INPUT_SAMPLES = 8;
const int RT_PT_RIGID_BLAS_GPU_SAMPLES = 8;
const int RT_PT_RIGID_TLAS_PLAN_SAMPLES = 8;
const int RT_PT_RIGID_RESIDENCY_SAMPLES = 8;

struct RtSmokeSurfaceClassStats;
struct RtSmokeMaterialStats;
struct srfTriangles_t;
struct viewDef_t;
class RtPathTraceInstanceUniverse;
class idRenderWorldLocal;

struct RtSmokeGeometryUniverseStats
{
    int staticRecords = 0;
    int staticSurfaces = 0;
    int staticVerts = 0;
    int staticIndexes = 0;
    int staticTriangles = 0;
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
    int staticDirtyVertexOffset = -1;
    int staticDirtyVertexCount = 0;
    int staticDirtyIndexOffset = -1;
    int staticDirtyIndexCount = 0;
    int staticDirtyTriangleOffset = -1;
    int staticDirtyTriangleCount = 0;
    int staticMaterialDirtyTriangleOffset = -1;
    int staticMaterialDirtyTriangleCount = 0;
    int staticBytesKB = 0;
    int previousStaticVerts = 0;
    int previousStaticIndexes = 0;
    int previousStaticTriangles = 0;
    int previousStaticBytesKB = 0;
    bool previousStaticCpuSnapshotAvailable = false;
    uint64 previousStaticSnapshotGeneration = 1;
    uint64 previousStaticSnapshotMaterialGeneration = 1;
    uint64 frameIndex = 0;
    uint64 generation = 1;
    uint64 staticGeometryGeneration = 1;
    uint64 staticMaterialGeneration = 1;
};

enum RtPathTraceRigidMeshCandidateRejectFlags : uint32_t
{
    RT_PT_RIGID_MESH_REJECT_NOT_RIGID = 1u << 0,
    RT_PT_RIGID_MESH_REJECT_INVALID_GEOMETRY = 1u << 1,
    RT_PT_RIGID_MESH_REJECT_MISSING_MATERIAL = 1u << 2,
    RT_PT_RIGID_MESH_REJECT_NO_LOCAL_SPACE = 1u << 3,
    RT_PT_RIGID_MESH_REJECT_SKINNED_OR_DEFORMING = 1u << 4,
    RT_PT_RIGID_MESH_REJECT_PARTICLE_OR_TRANSIENT = 1u << 5,
    RT_PT_RIGID_MESH_REJECT_GUI = 1u << 6,
    RT_PT_RIGID_MESH_REJECT_CALLBACK_OR_GENERATED = 1u << 7,
    RT_PT_RIGID_MESH_REJECT_STATIC_WORLD = 1u << 8,
    RT_PT_RIGID_MESH_REJECT_STATIC_CACHE_MATCH = 1u << 9
};

struct RtPathTraceRigidMeshCandidateObservation
{
    const srfTriangles_t* tri = nullptr;
    uint64 meshHash = 0;
    uint64 instanceId = 0;
    uintptr_t vertexBufferIdentity = 0;
    uintptr_t indexBufferIdentity = 0;
    uint32_t sourceFlags = 0;
    uint32_t materialId = 0;
    uint32_t materialClassSignature = 0;
    uint32_t surfaceClassId = 0;
    uint32_t triangleClassAndFlags = 0;
    uint32_t vertexFormat = 0;
    int drawSurfIndex = -1;
    int entityIndex = -1;
    int renderEntityNum = -1;
    uint32_t modelEpoch = 0;
    int jointIndex = -1;
    int numVerts = 0;
    int numIndexes = 0;
    bool localSpaceValid = false;
    float normalTexMatrix[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    idStr materialName;
    idStr modelName;
};

struct RtPathTraceRigidMeshCandidateSample
{
    bool valid = false;
    bool eligible = false;
    uint64 meshHash = 0;
    uint64 instanceId = 0;
    uintptr_t triIdentity = 0;
    uintptr_t vertexBufferIdentity = 0;
    uintptr_t indexBufferIdentity = 0;
    uint32_t rejectFlags = 0;
    uint32_t materialId = 0;
    uint32_t materialClassSignature = 0;
    uint32_t vertexFormat = 0;
    int drawSurfIndex = -1;
    int entityIndex = -1;
    int renderEntityNum = -1;
    int numVerts = 0;
    int numIndexes = 0;
    int seenCount = 0;
    idStr materialName;
    idStr modelName;
};

struct RtPathTraceRigidMeshCandidateStats
{
    int observations = 0;
    int rigidObservations = 0;
    int eligibleInstances = 0;
    int rejectedInstances = 0;
    int eligibleUniqueMeshes = 0;
    int persistentEligibleMeshes = 0;
    int localMeshSourceRecords = 0;
    int localMeshSourceRecordsSeenThisFrame = 0;
    int localMeshSourceVerts = 0;
    int localMeshSourceIndexes = 0;
    int localMeshSourceTriangles = 0;
    int eligibleVertsThisFrame = 0;
    int eligibleIndexesThisFrame = 0;
    int eligibleTrianglesThisFrame = 0;
    int materialOverrideEligibleInstances = 0;
    int reusedEligibleMeshObservations = 0;
    int newlyEligibleMeshes = 0;
    int rejectNotRigid = 0;
    int rejectInvalidGeometry = 0;
    int rejectMissingMaterial = 0;
    int rejectNoLocalSpace = 0;
    int rejectSkinnedOrDeforming = 0;
    int rejectParticleOrTransient = 0;
    int rejectGui = 0;
    int rejectCallbackOrGenerated = 0;
    int rejectStaticWorld = 0;
    int rejectStaticCacheMatch = 0;
    uint64 frameIndex = 0;
    uint64 generation = 1;
    RtPathTraceRigidMeshCandidateSample eligibleSamples[RT_PT_RIGID_MESH_CANDIDATE_SAMPLES];
    int eligibleSampleCount = 0;
    RtPathTraceRigidMeshCandidateSample rejectedSamples[RT_PT_RIGID_MESH_CANDIDATE_SAMPLES];
    int rejectedSampleCount = 0;
};

struct RtPathTraceRigidMeshValidationStats
{
    int bakedRigidTriangles = 0;
    int bakedRigidMaterialIds = 0;
    int eligibleRigidTriangles = 0;
    int eligibleRigidMaterialIds = 0;
    int triangleDelta = 0;
    int missingMaterialIds = 0;
    int extraMaterialIds = 0;
    uint32_t missingMaterialSamples[8] = {};
    uint32_t extraMaterialSamples[8] = {};
    int missingMaterialSampleCount = 0;
    int extraMaterialSampleCount = 0;
};

struct RtPathTraceRigidBlasPlanSample
{
    bool valid = false;
    uint64 meshHash = 0;
    uintptr_t triIdentity = 0;
    uintptr_t vertexBufferIdentity = 0;
    uintptr_t indexBufferIdentity = 0;
    uint32_t materialId = 0;
    uint32_t vertexFormat = 0;
    int instanceCount = 0;
    int verts = 0;
    int indexes = 0;
    int triangles = 0;
    idStr materialName;
    idStr modelName;
};

struct RtPathTraceRigidBlasPlanStats
{
    int meshRecords = 0;
    int instances = 0;
    int localVerts = 0;
    int localIndexes = 0;
    int localTriangles = 0;
    int plannedRemoveRigidTriangles = 0;
    int bakedRigidSurfaces = 0;
    int bakedRigidTriangles = 0;
    int estimatedRemainingRigidTriangles = 0;
    int triangleDelta = 0;
    int persistentMeshRecords = 0;
    uint64 frameIndex = 0;
    uint64 generation = 1;
    RtPathTraceRigidBlasPlanSample samples[RT_PT_RIGID_BLAS_PLAN_SAMPLES];
    int sampleCount = 0;
};

enum RtPathTraceRigidBlasInputInvalidFlags : uint32_t
{
    RT_PT_RIGID_BLAS_INPUT_INVALID_NULL_TRI = 1u << 0,
    RT_PT_RIGID_BLAS_INPUT_INVALID_VERTEX_COUNT = 1u << 1,
    RT_PT_RIGID_BLAS_INPUT_INVALID_INDEX_COUNT = 1u << 2,
    RT_PT_RIGID_BLAS_INPUT_INVALID_TRIANGLE_COUNT = 1u << 3,
    RT_PT_RIGID_BLAS_INPUT_INVALID_VERTEX_FORMAT = 1u << 4,
    RT_PT_RIGID_BLAS_INPUT_INVALID_MISSING_SOURCE_IDENTITY = 1u << 5,
    RT_PT_RIGID_BLAS_INPUT_INVALID_MATERIAL = 1u << 6
};

struct RtPathTraceRigidBlasInputSample
{
    bool valid = false;
    uint64 meshHash = 0;
    uintptr_t triIdentity = 0;
    uintptr_t vertexBufferIdentity = 0;
    uintptr_t indexBufferIdentity = 0;
    uint32_t materialId = 0;
    uint32_t invalidFlags = 0;
    int vertexCount = 0;
    int indexCount = 0;
    int triangleCount = 0;
    int vertexStride = 0;
    int vertexOffsetBytes = 0;
    int indexOffsetBytes = 0;
    int instanceCount = 0;
    idStr materialName;
    idStr modelName;
};

struct RtPathTraceRigidBlasInputStats
{
    int descriptors = 0;
    int validDescriptors = 0;
    int invalidDescriptors = 0;
    int instances = 0;
    int geometryDescs = 0;
    int vertexCount = 0;
    int indexCount = 0;
    int triangleCount = 0;
    int vertexBytes = 0;
    int indexBytes = 0;
    int nullTri = 0;
    int invalidVertexCount = 0;
    int invalidIndexCount = 0;
    int invalidTriangleCount = 0;
    int invalidVertexFormat = 0;
    int missingSourceIdentity = 0;
    int invalidMaterial = 0;
    uint64 frameIndex = 0;
    uint64 generation = 1;
    RtPathTraceRigidBlasInputSample samples[RT_PT_RIGID_BLAS_INPUT_SAMPLES];
    int sampleCount = 0;
};

struct RtPathTraceRigidBlasGpuSample
{
    bool valid = false;
    uint64 meshHash = 0;
    uintptr_t triIdentity = 0;
    uintptr_t vertexBufferIdentity = 0;
    uintptr_t indexBufferIdentity = 0;
    uint32_t materialId = 0;
    uint32_t invalidFlags = 0;
    int vertexCount = 0;
    int indexCount = 0;
    int triangleCount = 0;
    int vertexBytes = 0;
    int indexBytes = 0;
    int instanceCount = 0;
    bool vertexBufferValid = false;
    bool indexBufferValid = false;
    bool blasValid = false;
    bool uploadedThisFrame = false;
    bool builtThisFrame = false;
    idStr materialName;
    idStr modelName;
};

struct RtPathTraceRigidBlasGpuStats
{
    int meshRecords = 0;
    int validInputs = 0;
    int invalidInputs = 0;
    int instances = 0;
    int vertexCount = 0;
    int indexCount = 0;
    int triangleCount = 0;
    int vertexBytes = 0;
    int indexBytes = 0;
    int vertexBuffersCreated = 0;
    int indexBuffersCreated = 0;
    int vertexBuffersReused = 0;
    int indexBuffersReused = 0;
    int vertexUploads = 0;
    int indexUploads = 0;
    int uploadBytes = 0;
    int blasHandlesCreated = 0;
    int blasHandlesReused = 0;
    int blasBuildsSubmitted = 0;
    int blasBuildsSkipped = 0;
    int blasBuildsSkippedUnchanged = 0;
    int blasRecreatedForInputChange = 0;
    int skippedNoDevice = 0;
    int skippedNoCommandList = 0;
    int skippedInvalid = 0;
    int buildGateOff = 0;
    uint64 frameIndex = 0;
    uint64 generation = 1;
    RtPathTraceRigidBlasGpuSample samples[RT_PT_RIGID_BLAS_GPU_SAMPLES];
    int sampleCount = 0;
};

struct RtPathTraceRigidTlasPlanSample
{
    bool valid = false;
    uint64 meshHash = 0;
    uint64 instanceId = 0;
    uintptr_t triIdentity = 0;
    uint32_t materialId = 0;
    uint32_t sourceFlags = 0;
    int drawSurfIndex = -1;
    int entityIndex = -1;
    int renderEntityNum = -1;
    int triangles = 0;
    int instanceCountForMesh = 0;
    bool hasMeshRecord = false;
    bool meshSeenThisFrame = false;
    bool hasGpuBuffers = false;
    bool hasBlas = false;
    idVec3 origin = vec3_origin;
    idStr materialName;
    idStr modelName;
};

struct RtPathTraceRigidTlasPlanStats
{
    int visibleInstances = 0;
    int rigidInstances = 0;
    int plannedInstances = 0;
    int uniqueMeshes = 0;
    int instancesWithGpuBuffers = 0;
    int instancesWithBlas = 0;
    int missingMeshRecord = 0;
    int staleMeshRecord = 0;
    int missingGpuBuffers = 0;
    int missingBlas = 0;
    int materialOverrideInstances = 0;
    int plannedRigidTriangles = 0;
    int bakedRigidSurfaces = 0;
    int bakedRigidTriangles = 0;
    int estimatedRemainingRigidTriangles = 0;
    int triangleDelta = 0;
    uint64 frameIndex = 0;
    uint64 generation = 1;
    RtPathTraceRigidTlasPlanSample samples[RT_PT_RIGID_TLAS_PLAN_SAMPLES];
    int sampleCount = 0;
};

struct RtPathTraceRigidRouteBuildStats
{
    int visibleInstances = 0;
    int emittedInstances = 0;
    int skippedNonRigid = 0;
    int skippedMissingMesh = 0;
    int skippedMissingBlas = 0;
    int vertices = 0;
    int indexes = 0;
    int triangles = 0;
    int missingMaterialTableIndex = 0;
    int emittedSeenThisFrame = 0;
    int emittedFromCache = 0;
    int emittedUniqueMeshes = 0;
    int previousTransformInstances = 0;
    int transformContinuousInstances = 0;
};

struct RtPathTraceRigidRouteBuild
{
    std::vector<PathTraceSmokeVertex> vertices;
    std::vector<uint32_t> indexes;
    std::vector<uint32_t> triangleMaterials;
    std::vector<uint32_t> triangleMaterialIndexes;
    std::vector<uint32_t> triangleClassAndFlags;
    std::vector<PathTraceRigidRouteInstance> instances;
    std::vector<std::array<float, 16>> instanceObjectToWorld;
    std::vector<uint32_t> instanceSeenThisFrame;
    RtPathTraceRigidRouteBuildStats stats;
};

struct RtPathTraceRigidRouteMeshSnapshot
{
    uint32_t routeRecordIndex = std::numeric_limits<uint32_t>::max();
    uint64 meshHash = 0;
    uint64 gpuUploadSignature = 0;
    uint32_t materialId = 0;
    uint32_t surfaceClassId = 0;
    uint32_t triangleClassAndFlags = 0;
    bool valid = false;
    bool routeReady = false;
    bool localBoundsValid = false;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    idBounds localBounds;
    std::vector<PathTraceSmokeVertex> vertices;
    std::vector<uint32_t> indexes;
};

struct RtPathTraceRigidRouteBuildSnapshot
{
    RtSmokeRigidTlasPlan plan;
    std::vector<uint32_t> materialTableIds;
    std::vector<RtPathTraceRigidRouteMeshSnapshot> meshes;
};

struct RtPathTraceRigidRouteBuildTimedResult
{
    RtPathTraceRigidRouteBuild build;
    uint64_t geometryUploadSignature = 0;
    uint64_t instanceUploadSignature = 0;
    bool geometryUploadSignatureValid = false;
    bool instanceUploadSignatureValid = false;
    uint64_t buildTimeMicros = 0;
};

struct RtPathTraceRigidResidencySample
{
    bool valid = false;
    uint64 meshHash = 0;
    uint64 instanceId = 0;
    int area = -1;
    int drawSurfIndex = -1;
    int entityIndex = -1;
    int renderEntityNum = -1;
    int lastSeenFrame = 0;
    bool seenThisFrame = false;
    bool selectedArea = false;
    bool routeReady = false;
    idVec3 origin = vec3_origin;
    idStr materialName;
    idStr modelName;
};

struct RtPathTraceRigidResidencyBoundsBox
{
    bool valid = false;
    bool seenThisFrame = false;
    bool retainedOffscreen = false;
    bool aboutToAgeOut = false;
    bool routeReady = false;
    bool missingBlas = false;
    int area = -1;
    idVec3 corners[8];
    idVec4 color = idVec4(0.0f, 1.0f, 1.0f, 1.0f);
};

struct RtPathTraceRigidResidencyStats
{
    int enabled = 0;
    int residencyV2 = 0;
    int currentArea = -1;
    int totalAreas = 0;
    int portalSteps = 1;
    int selectedAreas = 0;
    int portalEdges = 0;
    int blockedPortalEdges = 0;
    int visibleStaticWorldInstances = 0;
    int visibleDynamicFrameInstances = 0;
    int visibleTransientEffectInstances = 0;
    int visibleUnknownResidencyInstances = 0;
    int visibleRigidInstances = 0;
    int visibleRigidStaleModel = 0;
    int rejectedRigidStaticWorld = 0;
    int rejectedRigidDynamicFrame = 0;
    int rejectedRigidTransientEffect = 0;
    int rejectedRigidUnknown = 0;
    int areaWalkEntities = 0;
    int areaWalkRejectedEntities = 0;
    int areaWalkSurfaces = 0;
    int areaWalkRejectedSurfaces = 0;
    int areaWalkEligibleSurfaces = 0;
    int areaWalkDuplicateVisible = 0;
    int areaWalkDuplicateFrame = 0;
    int areaWalkRigidInstances = 0;
    int cachedRigidInstances = 0;
    int residentInstances = 0;
    int residentSeenThisFrame = 0;
    int residentFromCache = 0;
    int residentRouteReady = 0;
    int residentMissingMesh = 0;
    int residentMissingBlas = 0;
    int residentRetainedOffscreen = 0;
    int residentAgedOut = 0;
    int residentDeleted = 0;
    int meshLive = 0;
    int meshAgedOut = 0;
    int retiredBlasPending = 0;
    int residencyFramesToKeep = 0;
    int residencyMeshFramesToKeep = 0;
    int residencyEntityFeedCap = 0;
    int residencyAntiCulling = 0;
    int skippedOutsideArea = 0;
    int skippedUnknownArea = 0;
    uint64 frameIndex = 0;
    uint64 generation = 1;
    RtPathTraceRigidResidencySample samples[RT_PT_RIGID_RESIDENCY_SAMPLES];
    int sampleCount = 0;
};

struct RtPathTraceRigidRouteInstanceObservation
{
    uint64 instanceId = 0;
    uint64 meshHash = 0;
    int entityIndex = -1;
    int renderEntityNum = -1;
    int drawSurfIndex = -1;
    int modelSurfaceIndex = -1;
    int jointIndex = -1;
    int currentArea = -1;
    PtRenderDefKey renderDefKey;
    uint32_t modelEpoch = 0;
    uint32_t materialOverrideId = 0;
    uint32_t triangleClassAndFlags = 0;
    uint32_t sourceFlags = 0;
    bool seenThisFrame = true;
    bool wasMovingWhenLastSeen = false;
    bool isSkinnedOrDeforming = false;
    bool isStable = false;
    bool hasPreviousObjectToWorld = false;
    bool transformContinuous = false;
    float objectToWorld[16] = {};
    float previousObjectToWorld[16] = {};
    idStr materialName;
    idStr modelName;
};

enum class RtSmokeGeometryBufferFormat : uint32_t
{
    LegacySmokeVertex = 0
};

struct RtSmokeGeometryElementRange
{
    int offset = -1;
    int count = 0;
};

struct RtSmokeGeometryRangeRecord
{
    RtSmokeGeometryElementRange vertices;
    RtSmokeGeometryElementRange indexes;
    RtSmokeGeometryElementRange triangles;
};

struct RtSmokePersistentStaticSurfaceRecord
{
    bool valid = false;
    uint64 key = 0;
    uint32_t surfaceClassId = 0;
    uint32_t materialId = 0;
    RtSmokeGeometryRangeRecord currentRange;
    RtSmokeGeometryRangeRecord previousRange;
    uint64 lastSeenFrame = 0;
    uint64 previousSeenFrame = 0;
    bool seenThisFrame = false;
    bool newlyCreatedThisFrame = false;
    bool disappearedThisFrame = false;
    bool previousRangeValid = false;
    bool historyValid = false;
    bool dirty = true;
    uint64 materialGeneration = 0;
    RtSmokeGeometryBufferFormat geometryFormat = RtSmokeGeometryBufferFormat::LegacySmokeVertex;
};

struct RtSmokeStaticSurfaceAppend
{
    uint64 key = 0;
    uint32_t surfaceClassId = 0;
    uint32_t materialId = 0;
    int vertexOffset = 0;
    int indexOffset = 0;
    int triangleOffset = 0;
    int requestedVertexCount = 0;
    int requestedIndexCount = 0;
};

class RtSmokeGeometryUniverse
{
public:
    void Clear();
    void BeginFrame(uint64 frameIndex, const idRenderWorldLocal* renderWorld = nullptr);
    void EndFrame();
    bool PruneMissingStaticSurfaces();
    void NotifyStaticCacheChanged();
    void ReserveStaticSurfaceRecords(size_t surfaceCount);
    bool HasStaticSurface(uint64 key) const;
    RtSmokePersistentStaticSurfaceRecord* TouchStaticSurface(uint64 key);
    bool RefreshStaticSurfaceMaterial(uint64 key, uint32_t materialId);
    bool CanAppendStaticSurface(int vertexCount, int indexCount, int maxVertexCount, int maxIndexCount) const;
    RtSmokeStaticSurfaceAppend BeginStaticSurfaceAppend(uint64 key, uint32_t surfaceClassId, uint32_t materialId, int vertexCount, int indexCount) const;
    void CompleteStaticSurfaceAppend(const RtSmokeStaticSurfaceAppend& append, int emittedIndexCount);

    const RtSmokePersistentStaticSurfaceRecord* FindStaticSurface(uint64 key) const;
    const std::vector<RtSmokePersistentStaticSurfaceRecord>& StaticSurfaceRecords() const;
    void BuildStaticTlasBucketObservations(
        std::vector<RtSmokeStaticTlasBucketObservation>& buckets,
        bool hasStaticBlas,
        uint32_t activeReasonFlags) const;

    std::vector<uint64>& StaticSurfaceKeys();
    const std::vector<uint64>& StaticSurfaceKeys() const;

    std::vector<PathTraceSmokeVertex>& StaticVertices();
    const std::vector<PathTraceSmokeVertex>& StaticVertices() const;

    std::vector<uint32_t>& StaticIndexes();
    const std::vector<uint32_t>& StaticIndexes() const;

    std::vector<uint32_t>& StaticTriangleClasses();
    const std::vector<uint32_t>& StaticTriangleClasses() const;

    std::vector<uint32_t>& StaticTriangleMaterials();
    const std::vector<uint32_t>& StaticTriangleMaterials() const;

    const std::vector<PathTraceSmokeVertex>& PreviousStaticVertices() const;
    const std::vector<uint32_t>& PreviousStaticIndexes() const;
    const std::vector<uint32_t>& PreviousStaticTriangleClasses() const;
    const std::vector<uint32_t>& PreviousStaticTriangleMaterials() const;

    uint64 Generation() const;
    uint64 StaticMaterialGeneration() const;
    RtSmokeGeometryUniverseStats GetStats(bool validateRecords) const;
    void LogStaticValidationFailures(int maxRecords) const;
    void LogStaticRangeHistory(int maxRecords) const;
    void RecordRigidMeshCandidate(const RtPathTraceRigidMeshCandidateObservation& observation);
    const RtPathTraceRigidMeshCandidateStats& GetRigidMeshCandidateStats() const;
    void RunRigidMeshCandidateDiagnostics(bool dumpRequested, int sceneSource, const RtSmokeSurfaceClassStats* sourceClassStats = nullptr);
    RtPathTraceRigidMeshValidationStats ValidateRigidMeshCandidatesAgainstDynamicPayload(
        const std::vector<uint32_t>& dynamicTriangleClassData,
        const std::vector<uint32_t>& dynamicTriangleMaterialData,
        uint32_t triangleClassMask,
        uint32_t rigidClassId) const;
    void DumpRigidMeshValidationStats(const RtPathTraceRigidMeshValidationStats& stats, int sceneSource) const;
    RtPathTraceRigidBlasPlanStats BuildRigidBlasPlanStats(const RtSmokeSurfaceClassStats* sourceClassStats = nullptr) const;
    void DumpRigidBlasPlanStats(const RtPathTraceRigidBlasPlanStats& stats, int sceneSource) const;
    RtPathTraceRigidBlasInputStats BuildRigidBlasInputStats() const;
    void DumpRigidBlasInputStats(const RtPathTraceRigidBlasInputStats& stats, int sceneSource) const;
    RtPathTraceRigidBlasGpuStats UpdateRigidBlasGpuScaffold(nvrhi::IDevice* device, nvrhi::ICommandList* commandList, bool submitBuilds);
    void ReleaseRigidBlasGpuScaffold();
    void ClearRetiredRigidBlas();
    void DumpRigidBlasGpuStats(const RtPathTraceRigidBlasGpuStats& stats, int sceneSource, bool scaffoldEnabled, bool submitBuilds) const;
    RtPathTraceRigidResidencyStats UpdateRigidResidency(
        const viewDef_t* viewDef,
        const RtPathTraceInstanceUniverse& instanceUniverse,
        bool enabled,
        int portalSteps);
    void RefreshRigidResidencyAreaWalk(const viewDef_t* viewDef, const RtPathTraceInstanceUniverse& instanceUniverse, int portalSteps, bool recordResidents = true, RtSmokeMaterialStats* materialStats = nullptr);
    const RtPathTraceRigidResidencyStats& GetRigidResidencyStats() const;
    void DumpRigidResidencyStats(const RtPathTraceRigidResidencyStats& stats, int sceneSource, bool includeSamples = true) const;
    void CollectRigidResidencyBoundsBoxes(std::vector<RtPathTraceRigidResidencyBoundsBox>& boxes, int maxBoxes) const;
    void CollectStaticSurfaceBoundsBoxes(std::vector<RtPathTraceRigidResidencyBoundsBox>& boxes, int maxBoxes, bool cacheOnlyFirst) const;
    RtPathTraceRigidTlasPlanStats BuildRigidTlasPlanStats(const RtPathTraceInstanceUniverse& instanceUniverse, const RtSmokeSurfaceClassStats* sourceClassStats = nullptr) const;
    void DumpRigidTlasPlanStats(const RtPathTraceRigidTlasPlanStats& stats, int sceneSource) const;
    bool IsRigidRouteReady(uint64 meshHash) const;
    bool IsRigidRouteResidentReadyForEntityMaterial(int entityIndex, int renderEntityNum, uint32_t materialId) const;
    std::vector<uint32_t> CollectRigidRouteMaterialIds(const RtSmokeRigidTlasPlan& plan) const;
    std::vector<uint32_t> CollectRigidRouteMaterialIds(const RtPathTraceInstanceUniverse& instanceUniverse, int maxInstances) const;
    RtSmokeRigidTlasPlanSnapshot CaptureRigidTlasInstancePlanSnapshot(
        const RtPathTraceInstanceUniverse& instanceUniverse,
        uint32_t firstInstanceId,
        uint32_t instanceMask,
        int maxInstances) const;
    RtSmokeRigidTlasPlan BuildRigidTlasInstancePlan(
        const RtPathTraceInstanceUniverse& instanceUniverse,
        uint32_t firstInstanceId,
        uint32_t instanceMask,
        int maxInstances) const;
    int BuildRigidTlasInstanceDescs(
        const RtSmokeRigidTlasPlan& plan,
        std::vector<nvrhi::rt::InstanceDesc>& instanceDescs) const;
    int BuildRigidTlasInstanceDescs(
        const RtPathTraceInstanceUniverse& instanceUniverse,
        std::vector<nvrhi::rt::InstanceDesc>& instanceDescs,
        uint32_t firstInstanceId,
        uint32_t instanceMask,
        int maxInstances) const;
    RtPathTraceRigidRouteBuild BuildRigidRouteBuffers(
        const RtSmokeRigidTlasPlan& plan,
        const std::vector<uint32_t>& materialTableIds) const;
    RtPathTraceRigidRouteBuild BuildRigidRouteBuffers(
        const RtPathTraceInstanceUniverse& instanceUniverse,
        const std::vector<uint32_t>& materialTableIds,
        int maxInstances) const;
    RtPathTraceRigidRouteBuildSnapshot CaptureRigidRouteBuildSnapshot(
        const RtSmokeRigidTlasPlan& plan,
        const std::vector<uint32_t>& materialTableIds,
        bool captureGeometryPayload = true) const;

public:
    struct RigidMeshCandidateRecord
    {
        bool valid = false;
        const srfTriangles_t* tri = nullptr;
        uint64 meshHash = 0;
        uintptr_t vertexBufferIdentity = 0;
        uintptr_t indexBufferIdentity = 0;
        uint32_t materialId = 0;
        uint32_t materialClassSignature = 0;
        uint32_t surfaceClassId = 0;
        uint32_t triangleClassAndFlags = 0;
        uint32_t sourceFlags = 0;
        uint32_t vertexFormat = 0;
        uint32_t modelEpoch = 0;
        int jointIndex = -1;
        RtSmokeGeometryRangeRecord sourceRange;
        int firstSeenFrame = 0;
        int lastSeenFrame = 0;
        int seenCount = 0;
        int instanceCountThisFrame = 0;
        bool seenThisFrame = false;
        bool newlyCreatedThisFrame = false;
        float normalTexMatrix[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
        idBounds localBounds;
        bool localBoundsValid = false;
        std::vector<PathTraceSmokeVertex> cachedLocalVertices;
        std::vector<uint32_t> cachedLocalIndexes;
        nvrhi::BufferHandle rigidVertexBuffer;
        nvrhi::BufferHandle rigidIndexBuffer;
        nvrhi::rt::AccelStructDesc rigidBlasDesc;
        nvrhi::rt::AccelStructHandle rigidBlas;
        uint64 gpuUploadSignature = 0;
        int gpuBlasVertexCount = 0;
        int gpuBlasIndexCount = 0;
        bool gpuBuffersUploaded = false;
        bool gpuBlasCreated = false;
        bool gpuBlasBuildSubmitted = false;
        idStr materialName;
        idStr modelName;
    };

private:
    struct RetiredRigidBlasRecord
    {
        nvrhi::rt::AccelStructHandle rigidBlas;
        uint64 retireFrame = 0;
        uint64 retireGeneration = 0;
    };

    struct RigidResidentInstanceRecord
    {
        RtPathTraceRigidRouteInstanceObservation observation;
        uint64 lastSeenFrame = 0;
        int seenCount = 0;
        bool seenThisFrame = false;
    };

    RtSmokePersistentStaticSurfaceRecord* FindStaticSurfaceMutable(uint64 key);
    RigidMeshCandidateRecord* FindOrCreateRigidMeshCandidate(const RtPathTraceRigidMeshCandidateObservation& observation, bool& cacheHit);
    void RetireRigidBlas(RigidMeshCandidateRecord& record);
    void ReleaseExpiredRetiredRigidBlas();
    void ClearRigidResidencyCaches();
    void ResetRigidMeshCandidateFrameStats();
    void AddRigidMeshCandidateSample(const RtPathTraceRigidMeshCandidateObservation& observation, bool eligible, uint32_t rejectFlags, int seenCount);
    void BuildRigidRouteInstanceList(const RtPathTraceInstanceUniverse& instanceUniverse, std::vector<RtPathTraceRigidRouteInstanceObservation>& instances) const;
    void AddRigidResidencySample(const RigidResidentInstanceRecord& record, bool selectedArea, bool routeReady);
    bool RigidResidentObservationMatchesCurrentModel(const RtPathTraceRigidRouteInstanceObservation& instance) const;
    void RecordRigidResidentObservation(const RtPathTraceRigidRouteInstanceObservation& instance);
    void PruneRigidCachesToCurrentFrame(
        const idRenderWorldLocal* renderWorld,
        const idRenderMatrix* viewMvp,
        const idVec3* viewOrigin,
        const std::vector<bool>& selectedAreas);

    uint64 m_currentFrameIndex = 0;
    bool m_frameActive = false;
    uint64 m_generation = 1;
    uint64 m_staticGeometryGeneration = 1;
    uint64 m_staticMaterialGeneration = 1;
    uint64 m_previousStaticSnapshotGeneration = 1;
    uint64 m_previousStaticSnapshotMaterialGeneration = 1;
    int m_staticMaterialDirtyTriangleOffset = -1;
    int m_staticMaterialDirtyTriangleCount = 0;
    std::vector<RtSmokePersistentStaticSurfaceRecord> m_staticSurfaceRecords;
    std::unordered_map<uint64, size_t> m_staticSurfaceLookup;
    std::vector<uint64> m_staticSurfaceKeys;
    std::vector<PathTraceSmokeVertex> m_staticVertexCache;
    std::vector<uint32_t> m_staticIndexCache;
    std::vector<uint32_t> m_staticTriangleClassCache;
    std::vector<uint32_t> m_staticTriangleMaterialCache;
    std::vector<PathTraceSmokeVertex> m_previousStaticVertexCache;
    std::vector<uint32_t> m_previousStaticIndexCache;
    std::vector<uint32_t> m_previousStaticTriangleClassCache;
    std::vector<uint32_t> m_previousStaticTriangleMaterialCache;
    std::vector<RigidMeshCandidateRecord> m_rigidMeshCandidateRecords;
    std::vector<RetiredRigidBlasRecord> m_retiredRigidBlasRecords;
    std::unordered_map<uint64, size_t> m_rigidMeshCandidateLookup;
    std::unordered_set<uint64> m_frameRigidMeshCandidateHashes;
    std::vector<RigidResidentInstanceRecord> m_rigidResidentRecords;
    std::unordered_map<uint64, size_t> m_rigidResidentLookup;
    std::vector<RtPathTraceRigidRouteInstanceObservation> m_rigidResidentFrameInstances;
    RtPathTraceRigidMeshCandidateStats m_rigidMeshCandidateFrameStats;
    RtPathTraceRigidResidencyStats m_rigidResidencyStats;
    int m_rigidResidencyAreaWalkEntitiesThisFrame = 0;
    int m_rigidResidencyAreaWalkRejectedEntitiesThisFrame = 0;
    int m_rigidResidencyAreaWalkSurfacesThisFrame = 0;
    int m_rigidResidencyAreaWalkRejectedSurfacesThisFrame = 0;
    int m_rigidResidencyAreaWalkEligibleSurfacesThisFrame = 0;
    int m_rigidResidencyAreaWalkDuplicateVisibleThisFrame = 0;
    int m_rigidResidencyAreaWalkDuplicateFrameThisFrame = 0;
    int m_rigidResidencyAreaWalkInstancesThisFrame = 0;
    bool m_rigidResidencyEnabled = false;
    const idRenderWorldLocal* m_rigidResidencyWorld = nullptr;
};

RtPathTraceRigidRouteBuild BuildRigidRouteBuffersFromSnapshot(
    const RtPathTraceRigidRouteBuildSnapshot& snapshot);

uint64_t BuildRigidRouteGeometryUploadSignature(
    const RtPathTraceRigidRouteBuild& build);

uint64_t BuildRigidRouteInstanceUploadSignature(
    const RtPathTraceRigidRouteBuild& build);

RtPathTraceRigidRouteBuildTimedResult BuildRigidRouteBuffersTimedResult(
    const RtPathTraceRigidRouteBuildSnapshot& snapshot);
