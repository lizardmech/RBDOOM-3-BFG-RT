#pragma once

// Persistent RT smoke geometry records.
//
// This starts as ownership for the static-world geometry cache that used to
// live directly on PathTracePrimaryPass. Keeping it behind a small module gives
// later work a stable place to add surface records, GPU buffers, and dirty
// ranges without changing shader-visible behavior in this first step.

#include "PathTraceGeometry.h"
#include "PathTraceAccelerationPlan.h"
#include "PathTraceCanonicalIdentityS1.h"
#include "PathTraceGeometryLifecycle.h"
#include "PathTraceGeometryGpuPools.h"
#include "PathTraceGeometryIdentityTransport.h"
#include "PathTraceGeometryOffsetBlasProbe.h"
#include "PathTraceGeometrySourceRegistry.h"
#include "PathTraceGeometrySourceTransport.h"
#include "PathTraceRigidPreparedPayload.h"
#include "PathTraceUniversePlanningSnapshot.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <type_traits>

const int RT_PT_RIGID_MESH_CANDIDATE_SAMPLES = 8;
const int RT_PT_RIGID_BLAS_PLAN_SAMPLES = 8;
const int RT_PT_RIGID_BLAS_INPUT_SAMPLES = 8;
const int RT_PT_RIGID_BLAS_GPU_SAMPLES = 8;
const int RT_PT_RIGID_TLAS_PLAN_SAMPLES = 8;
const int RT_PT_RIGID_RESIDENCY_SAMPLES = 8;
const int RT_PT_CANONICAL_RIGID_COMPARE_SAMPLES = 8;
const int RT_PT_CANONICAL_RIGID_IDENTITY_SAMPLES = 8;
const int RT_PT_RIGID_REGISTRY_IDENTITY_SAMPLES = 8;
const int RT_PT_RIGID_REGISTRY_IDENTITY_SURFACES = 8;

struct RtSmokeSurfaceClassStats;
struct RtSmokeMaterialStats;
struct srfTriangles_t;
struct viewDef_t;
struct RtPathTraceStaticBucketCpuSnapshot;
struct RtPathTraceCommittedBaselineEpoch;
class idRenderModel;
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
    int modelSurfaceIndex = -1;
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

enum RtPathTraceCanonicalRigidCompareFlags : uint32_t
{
    RT_PT_CANONICAL_RIGID_COMPARE_EXACT = 0,
    RT_PT_CANONICAL_RIGID_COMPARE_INVALID_LEGACY = 1u << 0,
    RT_PT_CANONICAL_RIGID_COMPARE_MISSING_SHAPE = 1u << 1,
    RT_PT_CANONICAL_RIGID_COMPARE_PAYLOAD_MISMATCH = 1u << 2,
    RT_PT_CANONICAL_RIGID_COMPARE_EQUIVALENT_CONTENT = 1u << 3,
    RT_PT_CANONICAL_RIGID_COMPARE_ENDPOINT_MISMATCH = 1u << 4,
    RT_PT_CANONICAL_RIGID_COMPARE_SOURCE_MATERIAL_MISMATCH = 1u << 5
};

struct RtPathTraceCanonicalRigidCompareSample
{
    bool valid = false;
    uint32_t flags = 0;
    uint64 legacyMeshHash = 0;
    uint64 legacyChecksum = 0;
    uint64 canonicalChecksum = 0;
    uint64 sourceAssetId = 0;
    int modelSurfaceIndex = -1;
    int shapeMatches = 0;
    int exactPayloadMatches = 0;
    int vertexCount = 0;
    int indexCount = 0;
    int triangleCount = 0;
    uint32_t sourceMaterialFirst = UINT32_MAX;
    uint32_t sourceMaterialLast = UINT32_MAX;
    uint32_t resolvedMaterialId = 0;
    uint32_t resolvedTriangleClassAndFlags = 0;
    idStr materialName;
    idStr modelName;
};

struct RtPathTraceCanonicalRigidCompareStats
{
    uint64 frameIndex = 0;
    int canonicalRigidSources = 0;
    int legacyRigidRecords = 0;
    int validLegacyPayloads = 0;
    int exactPayloadMatches = 0;
    int uniqueContentMatches = 0;
    int equivalentContentMatches = 0;
    int missingShape = 0;
    int payloadMismatch = 0;
    int endpointMismatch = 0;
    int sourceMaterialMismatch = 0;
    int resolvedMaterialBindings = 0;
    int resolvedTriangleClassBindings = 0;
    int unmatchedCanonicalSources = 0;
    RtPathTraceCanonicalRigidCompareSample
        samples[RT_PT_CANONICAL_RIGID_COMPARE_SAMPLES];
    int sampleCount = 0;
};

enum RtPathTraceCanonicalRigidIdentityFlags : uint32_t
{
    RT_PT_CANONICAL_RIGID_IDENTITY_EXACT = 0,
    RT_PT_CANONICAL_RIGID_IDENTITY_INVALID_INSTANCE_KEY = 1u << 0,
    RT_PT_CANONICAL_RIGID_IDENTITY_MISSING_BINDING = 1u << 1,
    RT_PT_CANONICAL_RIGID_IDENTITY_INVALID_BINDING = 1u << 2,
    RT_PT_CANONICAL_RIGID_IDENTITY_MISSING_SOURCE = 1u << 3,
    RT_PT_CANONICAL_RIGID_IDENTITY_MISSING_LEGACY = 1u << 4,
    RT_PT_CANONICAL_RIGID_IDENTITY_PAYLOAD_MISMATCH = 1u << 5
};

struct RtPathTraceCanonicalRigidIdentitySample
{
    bool valid = false;
    uint32_t flags = 0;
    uint64 instanceId = 0;
    uint64 legacyMeshHash = 0;
    uint64 canonicalInstanceHash = 0;
    uint64 canonicalMeshHash = 0;
    uint64 legacyChecksum = 0;
    uint64 canonicalChecksum = 0;
    uint32_t renderDefIndex = UINT32_MAX;
    uint32_t renderDefGeneration = 0;
    int modelSurfaceIndex = -1;
    idStr materialName;
    idStr modelName;
};

struct RtPathTraceCanonicalRigidIdentityStats
{
    uint64 frameIndex = 0;
    int routeInstances = 0;
    int validInstanceKeys = 0;
    int identityBindings = 0;
    int sourceBindings = 0;
    int legacyBindings = 0;
    int exactPayloadBindings = 0;
    int invalidInstanceKeys = 0;
    int missingBindings = 0;
    int invalidBindings = 0;
    int missingSources = 0;
    int missingLegacyMeshes = 0;
    int payloadMismatches = 0;
    RtPathTraceCanonicalRigidIdentitySample
        samples[RT_PT_CANONICAL_RIGID_IDENTITY_SAMPLES];
    int sampleCount = 0;
};

struct RtPathTraceCanonicalRigidBlasStats
{
    uint64 frameIndex = 0;
    int enabled = 0;
    int requestedInstances = 0;
    int requestedUniqueMeshes = 0;
    int resolvedIdentity = 0;
    int resolvedSources = 0;
    int resolvedPoolRecords = 0;
    int readyRequestedMeshes = 0;
    int activeBlas = 0;
    int deferredBuilds = 0;
    int deferredOperationBudget = 0;
    int deferredResultByteBudget = 0;
    int deferredUnknownResultBytes = 0;
    int deferredAllocationFailure = 0;
    int resultRequirementQueries = 0;
    int resultRequirementFailures = 0;
    int oversizedResultAdmissions = 0;
    int missingIdentity = 0;
    int missingSource = 0;
    int missingPoolRecord = 0;
    uint64 admittedResultBytes = 0;
    uint64 oversizedResultBytes = 0;
    uint64 maxDeferredAge = 0;
    uint64 blasCreated = 0;
    uint64 blasBuilt = 0;
    uint64 blasReused = 0;
    uint64 blasRetired = 0;
    uint64 buildSubmitMicroseconds = 0;
};

struct RtPathTraceCanonicalRigidTlasStats
{
    uint64 frameIndex = 0;
    int enabled = 0;
    int plannedInstances = 0;
    int legacyDescriptors = 0;
    int canonicalDescriptors = 0;
    int exactRecordMappings = 0;
    int missingRecordIndex = 0;
    int meshHashMismatch = 0;
    int missingBlas = 0;
    int traversalRequested = 0;
    int exactParity = 0;
    int selectedForSubmit = 0;
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
    int deferredOperationBudget = 0;
    int deferredResultByteBudget = 0;
    int deferredUnknownResultBytes = 0;
    int deferredAllocationFailure = 0;
    int resultRequirementQueries = 0;
    int resultRequirementFailures = 0;
    int oversizedResultAdmissions = 0;
    uint64 admittedResultBytes = 0;
    uint64 oversizedResultBytes = 0;
    uint64 maxDeferredAge = 0;
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

struct RtPathTraceRigidRouteGeometryRange
{
    uint64 meshHash = 0;
    uint64 gpuUploadSignature = 0;
    uint32_t vertexOffset = 0;
    uint32_t indexOffset = 0;
    uint32_t triangleOffset = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t triangleCount = 0;
    uint32_t materialId = 0;
    uint32_t materialIndex = 0;
    uint32_t triangleClassAndFlags = 0;
};

struct RtPathTraceRigidRouteBuild
{
    std::vector<PathTraceSmokeVertex> vertices;
    std::vector<uint32_t> indexes;
    std::vector<uint32_t> triangleMaterials;
    std::vector<uint32_t> triangleMaterialIndexes;
    std::vector<uint32_t> triangleClassAndFlags;
    std::vector<RtPathTraceRigidRouteGeometryRange> geometryRanges;
    std::vector<PathTraceRigidRouteInstance> instances;
    std::vector<std::array<float, 16>> instanceObjectToWorld;
    std::vector<uint32_t> instanceSeenThisFrame;
    RtPathTraceRigidRouteBuildStats stats;
};

struct RtPathTraceRigidRouteMeshSnapshot
{
    uint32_t routeRecordIndex = std::numeric_limits<uint32_t>::max();
    uint64 meshHash = 0;
    uint64 cpuMeshContentSignature = 0;
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

struct RtPathTraceRigidRouteInstanceEligibility
{
    bool transformUsable = false;
    bool worldBoundsValid = false;
};

struct RtPathTraceRigidRouteBuildSnapshot
{
    RtSmokeRigidTlasPlan plan;
    std::vector<uint32_t> materialTableIds;
    std::vector<RtPathTraceRigidRouteMeshSnapshot> meshes;
    std::vector<RtPathTraceRigidRouteInstanceEligibility> instanceEligibility;
};

struct RtPathTraceRigidRouteBuildSnapshotCounts
{
    size_t planInstances = 0;
    size_t planTruncatedInstanceIds = 0;
    size_t materialTableIds = 0;
    size_t meshes = 0;
    size_t meshVertices = 0;
    size_t meshIndexes = 0;
    size_t instanceEligibility = 0;
};

// Heavy resident storage owns only immutable mesh metadata and CPU geometry.
// The per-frame ticket is the sole authority for plans, material ordering,
// transforms, and instance eligibility.
struct RtPathTraceRigidRouteResidentMeshPayloadCounts
{
    size_t meshes = 0;
    size_t meshVertices = 0;
    size_t meshIndexes = 0;
};

struct RtPathTraceStaticBucketCpuSnapshotCounts
{
    size_t surfaces = 0;
    size_t vertices = 0;
    size_t indexes = 0;
    size_t triangleClasses = 0;
    size_t triangleMaterials = 0;
};

inline void AdvancePathTraceResidentPayloadGeneration(uint64& generation)
{
    ++generation;
    if (generation == 0)
        ++generation;
}

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

struct RtSmokePersistentStaticSurfaceRecord
{
    bool valid = false;
    uint64 key = 0;
    uint64 bucketSurfaceKey = 0;
    uint32_t surfaceClassId = 0;
    uint32_t materialId = 0;
    int portalArea = RT_SMOKE_STATIC_BUCKET_FALLBACK_AREA;
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
    uint64 bucketSurfaceKey = 0;
    uint32_t surfaceClassId = 0;
    uint32_t materialId = 0;
    int portalArea = RT_SMOKE_STATIC_BUCKET_FALLBACK_AREA;
    int vertexOffset = 0;
    int indexOffset = 0;
    int triangleOffset = 0;
    int requestedVertexCount = 0;
    int requestedIndexCount = 0;
};

struct RtPathTraceStaticBucketBlasGpuStats
{
    uint64 frameIndex = 0;
    uint64 contentSignature = 0;
    uint64 uploadSignature = 0;
    int enabled = 0;
    int submitBuilds = 0;
    int residentBuckets = 0;
    int activeBuckets = 0;
    int readyBuckets = 0;
    int deferredBuckets = 0;
    int invalidBuckets = 0;
    int vertexCount = 0;
    int indexCount = 0;
    int triangleCount = 0;
    int surfaceRecordCount = 0;
    int geometryDescCount = 0;
    int opaqueGeometryDescCount = 0;
    int programmableGeometryDescCount = 0;
    int multiGeometryBuckets = 0;
    int invalidSurfaceRecords = 0;
    int staticClassMetadataWordCount = 0;
    int surfaceRecordWordOffset = 0;
    uint64 vertexBytes = 0;
    uint64 indexBytes = 0;
    uint64 metadataBytes = 0;
    uint64 surfaceRecordBytes = 0;
    uint64 uploadBytes = 0;
    int buffersCreated = 0;
    int bufferUploads = 0;
    int blasCreated = 0;
    int blasBuilt = 0;
    int blasReused = 0;
    int blasRetired = 0;
    int retainedReplacementBuckets = 0;
    int blasResultQueries = 0;
    int blasResultQueryFailures = 0;
    int compactedBlases = 0;
    uint64 blasResultBytes = 0;
    uint64 blasResultMaxBytes = 0;
    uint64 blasResultMaxAlignment = 0;
    int deferredOperationBudget = 0;
    int deferredResultByteBudget = 0;
    int deferredUnknownResultBytes = 0;
    int deferredAllocationFailure = 0;
    int resultRequirementQueries = 0;
    int resultRequirementFailures = 0;
    int oversizedResultAdmissions = 0;
    uint64 admittedResultBytes = 0;
    uint64 oversizedResultBytes = 0;
    uint64 maxDeferredAge = 0;
    int skippedNoDevice = 0;
    int skippedNoCommandList = 0;
    int skippedInexactPack = 0;
    uint64 buildSubmitMicroseconds = 0;
};

struct RtSmokeRetiredStaticBucketGpuResources
{
    std::vector<nvrhi::BufferHandle> buffers;
    std::vector<nvrhi::rt::AccelStructHandle> blases;

    bool Empty() const
    {
        return buffers.empty() && blases.empty();
    }
};

struct RtSmokeRetiredRigidGpuResources
{
    std::vector<nvrhi::BufferHandle> buffers;
    std::vector<nvrhi::rt::AccelStructHandle> blases;
    int legacyBufferCount = 0;
    int legacyBlasCount = 0;
    int canonicalBlasCount = 0;
    int canonicalPoolBufferCount = 0;
    int canonicalProbeBufferCount = 0;
    int canonicalProbeBlasCount = 0;

    bool Empty() const
    {
        return buffers.empty() && blases.empty();
    }
};

struct RtPathTraceStaticBucketRouteRecord
{
    uint32_t instanceId = 0;
    uint32_t vertexOffset = 0;
    uint32_t indexOffset = 0;
    uint32_t triangleOffset = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t triangleCount = 0;
    uint32_t surfaceCount = 0;
    uint32_t generationLo = 0;
    uint32_t generationHi = 0;
    uint32_t bucketKeyLo = 0;
    uint32_t bucketKeyHi = 0;
};
static_assert(
    sizeof(RtPathTraceStaticBucketRouteRecord) == 48,
    "GEO-10 static bucket route ABI must remain 12 uint32 words");

static constexpr uint32_t
    RT_PATH_TRACE_STATIC_BUCKET_INSTANCE_ID_BASE =
        RT_SMOKE_STATIC_BUCKET_INSTANCE_ID_NAMESPACE;

struct RtPathTraceStaticBucketActivePublication
{
    uint64 sourceGeneration = 0;
    uint64 storageGeneration = 0;
    uint64 materialGeneration = 0;
    uint64 contentSignature = 0;
    uint64 activeSetSignature = 0;
    uint64 publicationGeneration = 0;
    uint64 tlasGeneration = 0;
    uint64 routeGeneration = 0;
    int residentBuckets = 0;
    int activeBuckets = 0;
    int readyActiveBuckets = 0;
    int missingBlas = 0;
    int missingMaterialIndexes = 0;
    int invalidRanges = 0;
    int instanceIdOverflow = 0;
    bool activeSetExact = false;
    bool valid = false;
    bool mixedEpochRejected = false;
    std::vector<uint8_t> activeBucketMask;
    std::vector<nvrhi::rt::InstanceDesc> tlasInstances;
    std::vector<RtPathTraceStaticBucketRouteRecord> routeRecords;
};

class RtSmokeGeometryUniverse
{
public:
    void Clear();
    void BeginFrame(
        uint64 frameIndex,
        const idRenderWorldLocal* renderWorld = nullptr,
        bool capturePreviousStaticSnapshot = true);
    void EndFrame();
    void ImportCanonicalSourceSnapshot(const PtGeometrySourceTransportSnapshot* snapshot);
    void ImportCanonicalIdentitySnapshot(
        const PtGeometryIdentityTransportSnapshot* snapshot);
    const PtGeometryIdentityBinding* FindCanonicalIdentityBinding(
        const PtCanonicalInstanceKey& key) const;
    const PtGeometryIdentityRegistryStats& CanonicalIdentityRegistryStats() const;
    void RecordA8S1RouteObservation(
        const PtRenderDefKey& renderDefKey,
        int modelSurfaceIndex,
        bool modelSurfaceIndexValid,
        uint64 legacyMeshHash,
        const char* modelName,
        PtA8S1RouteProducer producer);
    const std::vector<PtA8S1RouteObservation>& A8S1RouteObservations() const;
    bool A8S1RouteObservationsAvailable() const;
    const std::array<uint64, static_cast<size_t>(PtA8S1RouteProducer::Count)>&
        A8S1ProducerOpportunities() const;
    const std::array<uint64, static_cast<size_t>(PtA8S1RouteProducer::Count)>&
        A8S1ProducerObserved() const;
    const std::array<uint64, static_cast<size_t>(PtA8S1RouteProducer::Count)>&
        A8S1ProducerSuppressedDiagnosticUnavailable() const;
    PtA8S1AliasSidecar& A8S1AliasSidecar();
    PtA8S1LegacyCandidateProbe ProbeA8S1LegacyCandidates(
        const PtA8S1NormalizedRouteKey& route) const;
    bool CaptureA8S1LegacyCandidateProduct(
        std::vector<PtA8S1LegacyCandidateProductRecord>& out) const;
    const PtGeometrySourceRecord* FindCanonicalSourceRecord(
        const PtCanonicalMeshKey& key) const;
    const PtGeometryGpuPoolRecord* FindCanonicalSourceGpuRecord(
        const PtCanonicalMeshKey& key) const;
    uint64 CanonicalSourceIndexPoolGeneration() const;
    uint64 CanonicalSourceIndexPoolCapacityBytes() const;
    nvrhi::BufferHandle CanonicalSourceIndexBuffer() const;
    void UpdateCanonicalSourceGpuPools(
        nvrhi::IDevice* device,
        nvrhi::ICommandList* commandList);
    void DumpCanonicalSourceImportStats();
    void DumpCanonicalIdentityImportStats();
    void DumpCanonicalSourceGpuPoolStats();
    void DumpCanonicalOffsetBlasProbeStats();
    RtPathTraceCanonicalRigidCompareStats
        BuildCanonicalRigidSourceCompareStats() const;
    void DumpCanonicalRigidSourceCompareStats(
        const RtPathTraceCanonicalRigidCompareStats& stats) const;
    RtPathTraceCanonicalRigidIdentityStats
        BuildCanonicalRigidIdentityStats(
            const RtPathTraceInstanceUniverse& instanceUniverse) const;
    void DumpCanonicalRigidIdentityStats(
        const RtPathTraceCanonicalRigidIdentityStats& stats) const;
    void UpdateCanonicalRigidBlasScaffold(
        nvrhi::IDevice* device,
        nvrhi::ICommandList* commandList,
        const RtPathTraceInstanceUniverse& instanceUniverse,
        bool enabled);
    void DumpCanonicalRigidBlasStats();
    RtPathTraceCanonicalRigidTlasStats
        BuildCanonicalRigidTlasInstanceDescs(
            const RtSmokeRigidTlasPlan& plan,
            int legacyDescriptorCount,
            std::vector<nvrhi::rt::InstanceDesc>& instanceDescs) const;
    void DumpCanonicalRigidTlasStats(
        const RtPathTraceCanonicalRigidTlasStats& stats) const;
    bool PruneMissingStaticSurfaces();
    void NotifyStaticCacheChanged();
    uint64 StaticResidentPayloadGeneration() const;
    void ReserveStaticSurfaceRecords(size_t surfaceCount);
    bool HasStaticSurface(uint64 key) const;
    bool CaptureGeometryUniversePlanningSnapshot(
        RtSmokeGeometryUniverseSnapshot& snapshot,
        const RtPathTracePlanningSnapshotEpoch& epoch,
        size_t& captureProductSlotBytes) const;
    bool CaptureCommittedGeometryUniverseSnapshot(
        RtSmokeGeometryUniverseSnapshot& snapshot,
        const RtPathTraceCommittedBaselineEpoch& epoch,
        size_t& captureProductSlotBytes) const;
    bool CountGeometryUniversePlanningSnapshot(
        const RtPathTracePlanningSnapshotEpoch& epoch,
        RtSmokeGeometryUniverseSnapshotCounts& counts) const;
    bool FillGeometryUniversePlanningSnapshotPreReserved(
        RtSmokeGeometryUniverseSnapshot& snapshot,
        const RtPathTracePlanningSnapshotEpoch& epoch,
        const RtSmokeGeometryUniverseSnapshotCounts& counts) const;
    RtSmokePersistentStaticSurfaceRecord* TouchStaticSurface(uint64 key);
    bool RefreshStaticSurfaceMaterial(uint64 key, uint32_t materialId);
    bool RefreshStaticSurfacePortalArea(uint64 key, int portalArea);
    bool RefreshStaticSurfaceBucketKey(
        uint64 key,
        uint64 bucketSurfaceKey);
    RtSmokeStaticSurfaceAppend BeginStaticSurfaceAppend(
        uint64 key,
        uint32_t surfaceClassId,
        uint32_t materialId,
        int vertexCount,
        int indexCount,
        int portalArea = RT_SMOKE_STATIC_BUCKET_FALLBACK_AREA,
        uint64 bucketSurfaceKey = 0) const;
    void CompleteStaticSurfaceAppend(const RtSmokeStaticSurfaceAppend& append, int emittedIndexCount);

    const RtSmokePersistentStaticSurfaceRecord* FindStaticSurface(uint64 key) const;
    const std::vector<RtSmokePersistentStaticSurfaceRecord>& StaticSurfaceRecords() const;
    bool CaptureStaticBucketCpuSnapshot(
        RtPathTraceStaticBucketCpuSnapshot& snapshot,
        uint64 worldGeneration,
        uint64 sourceGeneration,
        int portalAreaCount,
        int maxVerticesPerBucket,
        int maxIndexesPerBucket,
        int maxTrianglesPerBucket,
        const std::vector<bool>* activePortalAreas = nullptr) const;
    bool CountStaticBucketCpuSnapshot(
        RtPathTraceStaticBucketCpuSnapshotCounts& counts) const;
    bool FillStaticBucketCpuSnapshotPreReserved(
        RtPathTraceStaticBucketCpuSnapshot& snapshot,
        const RtPathTraceStaticBucketCpuSnapshotCounts& counts,
        uint64 worldGeneration,
        uint64 sourceGeneration,
        int portalAreaCount,
        int maxVerticesPerBucket,
        int maxIndexesPerBucket,
        int maxTrianglesPerBucket,
        const std::vector<bool>* activePortalAreas = nullptr) const;
    void BuildStaticTlasBucketObservations(
        std::vector<RtSmokeStaticTlasBucketObservation>& buckets,
        bool hasStaticBlas,
        uint32_t activeReasonFlags) const;
    RtSmokeStaticBucketAssignmentPlan BuildStaticBucketAssignmentPlan(
        uint64 worldGeneration,
        uint64 sourceGeneration,
        int portalAreaCount,
        int maxVerticesPerBucket,
        int maxIndexesPerBucket,
        int maxTrianglesPerBucket,
        const std::vector<bool>* activePortalAreas = nullptr,
        bool* cacheHit = nullptr);
    RtSmokeStaticBucketGeometryPack BuildStaticBucketGeometryPack(
        const RtSmokeStaticBucketAssignmentPlan& assignmentPlan) const;
    const RtSmokeStaticBucketGeometryPack&
        GetOrBuildStaticBucketResidentGeometryPack(
            const RtSmokeStaticBucketAssignmentPlan& assignmentPlan,
            bool& cacheHit);
    bool TryGetStaticBucketResidentCpuState(
        RtSmokeStaticBucketAssignmentPlan& assignmentPlan,
        const RtSmokeStaticBucketGeometryPack*& geometryPack) const;
    void InstallStaticBucketResidentCpuState(
        RtSmokeStaticBucketAssignmentPlan&& assignmentPlan,
        RtSmokeStaticBucketGeometryPack&& geometryPack);
    bool GetOrBuildStaticBucketMaterialIndexes(
        const RtSmokeStaticBucketGeometryPack& geometryPack,
        const std::vector<uint32_t>& materialTableIds,
        bool& cacheHit,
        uint64& materialBindingSignature,
        const std::vector<uint32_t>*& triangleMaterialIndexes,
        const std::vector<int>*& missingMaterialIndexesByBucket);
    RtPathTraceStaticBucketBlasGpuStats
        UpdateStaticBucketBlasGpuScaffold(
            nvrhi::IDevice* device,
            nvrhi::ICommandList* commandList,
            const RtSmokeStaticBucketGeometryPack& geometryPack,
            bool enabled,
            bool submitBuilds,
            int maxBuildsPerFrame,
            uint64 maxResultBytesPerFrame,
            bool forceRebuild,
            bool collectResultMemory);
    void BuildStaticBucketTlasObservations(
        const RtSmokeStaticBucketGeometryPack& geometryPack,
        std::vector<RtSmokeStaticTlasBucketObservation>& buckets) const;
    void ReleaseStaticBucketBlasGpuScaffold();
    bool HasStaticBucketGpuResources() const;
    bool TakeRetiredStaticBucketGpuResources(
        RtSmokeRetiredStaticBucketGpuResources& resources);
    void DumpStaticBucketBlasGpuStats(
        const RtPathTraceStaticBucketBlasGpuStats& stats) const;
    RtPathTraceStaticBucketActivePublication
        BuildStaticBucketActivePublication(
            const RtSmokeStaticBucketGeometryPack& geometryPack,
            uint64 sourceGeneration,
            uint64 storageGeneration,
            uint64 materialGeneration,
            int missingActiveMaterialIndexes,
            uint32_t instanceMask,
            const std::vector<bool>* activePortalAreas = nullptr) const;
    bool UpdateStaticBucketMaterialIndexGpuScaffold(
        nvrhi::IDevice* device,
        nvrhi::ICommandList* commandList,
        const RtSmokeStaticBucketGeometryPack& geometryPack,
        const std::vector<uint32_t>& triangleMaterialIndexes,
        uint64 materialBindingSignature);
    nvrhi::BufferHandle StaticBucketVertexBuffer() const
    {
        return m_staticBucketVertexBuffer;
    }
    nvrhi::BufferHandle StaticBucketIndexBuffer() const
    {
        return m_staticBucketIndexBuffer;
    }
    nvrhi::BufferHandle StaticBucketTriangleClassBuffer() const
    {
        return m_staticBucketTriangleClassBuffer;
    }
    nvrhi::BufferHandle StaticBucketTriangleMaterialBuffer() const
    {
        return m_staticBucketTriangleMaterialBuffer;
    }
    nvrhi::BufferHandle StaticBucketTriangleMaterialIndexBuffer() const
    {
        return m_staticBucketTriangleMaterialIndexBuffer;
    }
    void DumpStaticBucketActivePublication(
        const RtPathTraceStaticBucketActivePublication&
            publication) const;

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
    void BindAsRigidMeshPersistTarget();
    void UnbindAsRigidMeshPersistTarget();
    static RtSmokeGeometryUniverse* ActiveRigidMeshPersistTarget();
	static void NoteRigidMeshPersistFirstEntityAdd(bool targetBound);
	static void NoteRigidMeshPersistCall();
	static void NoteRigidMeshPersistedSurfaces(uint32 persistedSurfaces);
    // A2: persist Mesh records from a loaded/presented MODEL. No drawSurf.
    int PersistRigidMeshFromPresent(const idRenderModel* model, uint32_t modelEpoch);
    // A3: complete ordered per-surface A2 hashes. No table insert.
    static int ComputeRigidMeshHashesFromPresent(
        const idRenderModel* model,
        uint32_t modelEpoch,
        std::vector<uint64>& outHashes,
		int maxSurfaceCount = -1);
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
    RtPathTraceRigidBlasGpuStats UpdateRigidBlasGpuScaffold(
        nvrhi::IDevice* device,
        nvrhi::ICommandList* commandList,
        bool submitBuilds,
        int maxBuildsPerFrame,
        uint64 maxResultBytesPerFrame,
        bool collectAdmissionInterval);
    void ReleaseRigidBlasGpuScaffold();
    void ClearRetiredRigidBlas();
    bool HasRetiredRigidGpuResources() const;
    bool TakeRetiredRigidGpuResources(
        RtSmokeRetiredRigidGpuResources& resources);
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
    uint64 RigidMeshCandidateBlasToken(uint64 meshHash) const;
    nvrhi::rt::AccelStructHandle RigidMeshCandidateBlas(uint64 meshHash) const;
    bool RigidMeshCandidateBlasWasBuilt(uint64 meshHash) const;
    struct RigidMeshRegistryLookupSnapshotEntry
    {
        bool ready = false;
        bool pending = false;
        bool pendingExpired = false;
        bool built = false;
        uint64 blasToken = 0;
        nvrhi::rt::AccelStructHandle builtBlas;
		uint64 refreshAttempts = 0;
		uint64 refreshSuccesses = 0;
		uint64 refreshFailures = 0;
		uint8 diagnosticReason = 0;
    };
	struct RigidMeshRegistryIdentityDiagnosticInput
	{
		uint64 instanceId = 0;
		const idRenderModel* model = nullptr;
		uint32 modelEpoch = 0;
		std::array<uint64, RT_PT_RIGID_REGISTRY_IDENTITY_SURFACES> storedMeshIds = {};
		uint32 storedMeshIdCount = 0;
	};
	struct RigidMeshRegistryIdentityDiagnosticSample
	{
		uint64 instanceId = 0;
		uint32 modelEpoch = 0;
		bool recomputeAvailable = false;
		std::array<uint64, RT_PT_RIGID_REGISTRY_IDENTITY_SURFACES> storedMeshIds = {};
		std::array<uint8, RT_PT_RIGID_REGISTRY_IDENTITY_SURFACES> storedLookupMembership = {};
		uint32 storedMeshIdCount = 0;
		std::array<uint64, RT_PT_RIGID_REGISTRY_IDENTITY_SURFACES> recomputedMeshIds = {};
		std::array<uint8, RT_PT_RIGID_REGISTRY_IDENTITY_SURFACES> recomputedLookupMembership = {};
		uint32 recomputedMeshIdCount = 0;
	};
	struct RigidMeshRegistryLookupDiagnostics
	{
		bool earlyReturn = false;
		bool available = false;
		bool populationSnapshotAvailable = false;
		uint32 candidateRecordCount = 0;
		uint32 lookupTableSize = 0;
		uint64 candidateRecordInsertTotal = 0;
		uint64 candidateRecordInsertsThisFrame = 0;
		int32 persistTargetBoundAtFirstEntityAdd = -1;
		uint64 persistCallsTotal = 0;
		uint64 persistedSurfacesTotal = 0;
		uint32 uniqueMeshRequests = 0;
		uint32 zeroHashSkipped = 0;
		uint32 lookupHits = 0;
		uint32 lookupMisses = 0;
		uint32 ready = 0;
		uint32 built = 0;
		uint32 pending = 0;
		uint32 blasTokens = 0;
		std::array<uint64, 8> residentKeySample = {};
		uint32 residentKeySampleCount = 0;
		std::array<uint64, 8> presentRequestKeySample = {};
		uint32 presentRequestKeySampleCount = 0;
		bool identitySamplesAvailable = false;
		std::array<RigidMeshRegistryIdentityDiagnosticSample,
			RT_PT_RIGID_REGISTRY_IDENTITY_SAMPLES> identitySamples = {};
		uint32 identitySampleCount = 0;
	};
    void SnapshotRigidMeshRegistryLookup(
        const std::vector<uint64>& meshHashes,
		std::unordered_map<uint64, RigidMeshRegistryLookupSnapshotEntry>& out,
		RigidMeshRegistryLookupDiagnostics* diagnostics = nullptr,
		const std::vector<RigidMeshRegistryIdentityDiagnosticInput>*
			identityDiagnosticInputs = nullptr) const;
    ~RtSmokeGeometryUniverse();
    nvrhi::rt::AccelStructHandle RigidMeshCandidateBuiltBlas(uint64 meshHash) const;
    void ClassifyRigidMeshForRegistry(uint64 meshHash, bool& ready, bool& pending) const;
    bool FindRigidResidentSource(uint64 instanceId, PtRenderDefKey& key, uint64& meshHash) const;
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
        const RtSmokeRigidTlasPlan& plan,
        std::vector<nvrhi::rt::InstanceDesc>& instanceDescs,
        std::vector<RtSmokeRigidBuilderInstanceResult>* builderResults,
        std::vector<cpu_producer_publish::RigidSubmitBoundaryRecord>* submitMetadata = nullptr) const;
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
    bool CountRigidRouteBuildSnapshot(
        const RtSmokeRigidTlasPlan& plan,
        const std::vector<uint32_t>& materialTableIds,
        bool captureGeometryPayload,
        RtPathTraceRigidRouteBuildSnapshotCounts& counts) const;
    bool FillRigidRouteBuildSnapshotPreReserved(
        RtPathTraceRigidRouteBuildSnapshot& snapshot,
        const RtSmokeRigidTlasPlan& plan,
        const std::vector<uint32_t>& materialTableIds,
        bool captureGeometryPayload,
        const RtPathTraceRigidRouteBuildSnapshotCounts& counts) const;
    bool CountRigidRouteResidentMeshPayload(
        const RtSmokeRigidTlasPlan& plan,
        bool captureGeometryPayload,
        RtPathTraceRigidRouteResidentMeshPayloadCounts& counts) const;
    bool FillRigidRouteResidentMeshPayloadPreReserved(
        RtPathTraceRigidRouteBuildSnapshot& snapshot,
        const RtSmokeRigidTlasPlan& plan,
        bool captureGeometryPayload,
        const RtPathTraceRigidRouteResidentMeshPayloadCounts& counts) const;

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
        int modelSurfaceIndex = -1;
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
        uint64 cpuMeshContentSignature = 0;
        bool cachedRouteDataValid = false;
		uint64 cpuCacheRefreshAttempts = 0;
		uint64 cpuCacheRefreshSuccesses = 0;
		uint64 cpuCacheRefreshFailures = 0;
		uint8 lastCpuCacheDiagnosticReason = 0;
        uint64 gpuUploadSignature = 0;
        int gpuBlasVertexCount = 0;
        int gpuBlasIndexCount = 0;
        bool gpuBuffersUploaded = false;
        bool gpuBlasCreated = false;
        bool gpuBlasBuildSubmitted = false;
        uint64 deferredSinceFrame = 0;
        idStr materialName;
        idStr modelName;
    };

    enum class RigidMeshCandidatePrepareFailurePoint : uint8_t
    {
        None,
        AfterOffsideState,
        AfterRecordReserve,
        AfterLookupReserve,
        AfterFrameHashReserve,
        AfterAllReserves
    };

    struct RigidMeshCandidatePrepareTestSeam
    {
        RigidMeshCandidatePrepareFailurePoint failAt =
            RigidMeshCandidatePrepareFailurePoint::None;
        size_t retainedGrowthBytes = 0;
        size_t predictedFinalOwnedBytes = 0;
        size_t predictedRecordPeakBytes = 0;
        size_t predictedLookupPeakBytes = 0;
        size_t predictedFrameHashPeakBytes = 0;
        size_t predictedLookupBucketTransientBytes = 0;
        size_t predictedFrameHashBucketTransientBytes = 0;
        size_t oldLookupBucketCount = 0;
        size_t replacementLookupBucketCount = 0;
        size_t oldFrameHashBucketCount = 0;
        size_t replacementFrameHashBucketCount = 0;
        int liveReserveCalls = 0;
        size_t failAfterObservation = static_cast<size_t>(-1);
        size_t stagedObservationCount = 0;
    };

    // MainThread-local only. The owner pointer and serials are provenance, not
    // worker payload; this object must never cross the producer boundary.
    struct RigidMeshCandidatePreparedDelta
    {
        RigidMeshCandidatePreparedDelta() = default;
        RigidMeshCandidatePreparedDelta(RigidMeshCandidatePreparedDelta&&) noexcept = default;
        RigidMeshCandidatePreparedDelta& operator=(RigidMeshCandidatePreparedDelta&&) noexcept = default;
        RigidMeshCandidatePreparedDelta(const RigidMeshCandidatePreparedDelta&) = delete;
        RigidMeshCandidatePreparedDelta& operator=(const RigidMeshCandidatePreparedDelta&) = delete;

        size_t RetainedGrowthBytes() const noexcept { return retainedGrowthBytes; }
        bool Complete() const noexcept { return complete; }

    private:
        friend class RtSmokeGeometryUniverse;
        RtSmokeGeometryUniverse* owner = nullptr;
        uint64 baseGeneration = 0;
        uint64 baseRigidRevision = 0;
        uint64 baseFrameBeginSerial = 0;
        uint64 baseFrameIndex = 0;
        uint64 baseInsertTotal = 0;
        size_t baseRecordCount = 0;
        size_t requiredRecordCapacity = 0;
        size_t retainedGrowthBytes = 0;
        bool complete = false;
        bool hasObservations = false;
        uint64 workingGeneration = 0;
        uint64 workingInsertTotal = 0;
        uint32 workingLookupHealth = 0;
        uint32 workingLookupReason = 0;
        uint32 workingLookupCanary = 0;
        std::vector<RigidMeshCandidateRecord> workingRecords;
        RtPathTraceRigidMeshCandidateStats workingStats;
        std::unordered_map<uint64, size_t> workingLookup;
        std::unordered_set<uint64> workingFrameHashes;
    };

    struct RigidPreparedApplyTouchedRecord
    {
        size_t liveIndex = static_cast<size_t>(-1);
        bool newRecord = false;
        bool replaceCpuCache = false;
        uint32_t occurrenceCount = 0;
        RigidMeshCandidateRecord staged;
        std::vector<PathTraceSmokeVertex> retiredVertices;
        std::vector<uint32_t> retiredIndexes;
        idStr retiredMaterialName;
        idStr retiredModelName;
    };

    // MainThread apply transaction for worker-owned prepared rigid payloads.
    // Preparation may allocate and retain live topology growth; Commit performs
    // only O(touched records) nothrow publication.
    struct RigidPreparedApplyDelta
    {
        RigidPreparedApplyDelta() = default;
        RigidPreparedApplyDelta(RigidPreparedApplyDelta&&) noexcept = default;
        RigidPreparedApplyDelta& operator=(RigidPreparedApplyDelta&&) noexcept = default;
        RigidPreparedApplyDelta(const RigidPreparedApplyDelta&) = delete;
        RigidPreparedApplyDelta& operator=(const RigidPreparedApplyDelta&) = delete;

        bool Complete() const noexcept { return complete; }

    private:
        friend class RtSmokeGeometryUniverse;
        RtSmokeGeometryUniverse* owner = nullptr;
        uint64 baseGeneration = 0;
        uint64 baseRigidRevision = 0;
        uint64 baseFrameBeginSerial = 0;
        uint64 baseFrameIndex = 0;
        uint64 baseInsertTotal = 0;
        size_t baseRecordCount = 0;
        size_t baseRecordCapacity = 0;
        size_t baseLookupSize = 0;
        size_t baseLookupBucketCount = 0;
        size_t baseFrameHashSize = 0;
        size_t baseFrameHashBucketCount = 0;
        float baseLookupMaxLoadFactor = 1.0f;
        float baseFrameHashMaxLoadFactor = 1.0f;
        size_t targetLookupBucketCount = 0;
        size_t targetFrameHashBucketCount = 0;
        uint64 finalGeneration = 0;
        uint64 finalInsertTotal = 0;
        std::vector<RigidPreparedApplyTouchedRecord> touched;
        std::unordered_map<uint64, size_t> stagedLookupNodes;
        std::unordered_set<uint64> stagedFrameHashNodes;
        RtPathTraceRigidMeshCandidateStats statsDelta;
        bool complete = false;
    };

    bool PrepareRigidPreparedPayloadApply(
        std::vector<RtPathTraceRigidPreparedPayload>& payloads,
        const std::vector<RtPathTraceRigidMeshCandidateObservation>& observations,
        size_t alreadyOwnedBytes,
        size_t maxOwnedBytes,
        RigidPreparedApplyDelta& delta,
        RigidMeshCandidatePrepareTestSeam* seam = nullptr) noexcept;
    bool CommitRigidPreparedPayloadApply(
        RigidPreparedApplyDelta& delta) noexcept;
    void AbortRigidPreparedPayloadApply(
        RigidPreparedApplyDelta& delta) noexcept;

    bool PrepareRigidMeshCandidateBatch(
        const RtPathTraceRigidMeshCandidateObservation* observations,
        size_t observationCount,
        size_t alreadyOwnedBytes,
        size_t maxOwnedBytes,
        RigidMeshCandidatePreparedDelta& delta,
        RigidMeshCandidatePrepareTestSeam* seam = nullptr) noexcept;

    bool PrepareRigidMeshCandidateDelta(
        const RtPathTraceRigidMeshCandidateObservation& observation,
        size_t alreadyOwnedBytes,
        size_t maxOwnedBytes,
        RigidMeshCandidatePreparedDelta& delta,
        RigidMeshCandidatePrepareTestSeam* seam = nullptr) noexcept;
    bool CommitRigidMeshCandidateDelta(
        RigidMeshCandidatePreparedDelta& delta) noexcept;
    void AbortRigidMeshCandidateDelta(
        RigidMeshCandidatePreparedDelta& delta) noexcept;
    size_t RigidMeshCandidateRetainedStorageBytes() const noexcept;
#if defined(RT_PT_RIGID_PREPARED_DELTA_HARNESS)
    uint64 RigidMeshCandidateSemanticFingerprintForTest() const noexcept;
    size_t RigidMeshCandidateRecordCountForTest() const noexcept;
    size_t RigidMeshCandidateLookupCountForTest() const noexcept;
    size_t RigidMeshCandidateFrameHashCountForTest() const noexcept;
    bool RigidMeshCandidateHasRecordForTest(uint64 meshHash) const noexcept;
    void RigidMeshCandidatePoisonLookupForTest(uint64 meshHash, size_t index);
    void RigidMeshCandidateAdvanceGenerationForTest() noexcept;
    void RigidMeshCandidateBeginFrameForTest(uint64 frameIndex) noexcept;
    void RigidMeshCandidateEndFrameForTest() noexcept;
    void RigidMeshCandidateResetForTest() noexcept;
    void RigidMeshCandidateAdvanceSemanticRevisionForTest() noexcept;
    void RigidMeshCandidateSetGpuSignatureForTest(
        uint64 meshHash, uint64 signature) noexcept;
    uint64 RigidMeshCandidateGpuSignatureForTest(uint64 meshHash) const noexcept;
    void RigidMeshCandidateSetFrameBeginSerialForTest(uint64 serial) noexcept;
    uint64 RigidMeshCandidateFrameBeginSerialForTest() const noexcept;
    uint64 RigidMeshCandidateGenerationForTest() const noexcept { return m_generation; }
#endif

private:
    // Caller must already own m_rigidMeshCandidateLookupMutex.
    size_t RigidMeshCandidateRetainedStorageBytesUnlocked() const noexcept;
    void AdvanceRigidMeshCandidateSemanticRevisionUnlocked() const noexcept;
    void AdvanceRigidMeshCandidateFrameBeginSerialUnlocked() noexcept;
    void InvalidateRigidMeshCandidateLifecycleUnlocked() noexcept;
    bool CaptureGeometryUniversePlanningSnapshotInternal(
        RtSmokeGeometryUniverseSnapshot& snapshot,
        const RtPathTracePlanningSnapshotEpoch& epoch,
        size_t& captureProductSlotBytes,
        bool committedAfterEndFrame) const;
    struct RigidResidentInstanceRecord
    {
        RtPathTraceRigidRouteInstanceObservation observation;
        uint64 lastSeenFrame = 0;
        int seenCount = 0;
        bool seenThisFrame = false;
    };

    struct CanonicalRigidBlasRecord
    {
        PtCanonicalMeshKey key;
        uint64 meshHash = 0;
        uint64 inputSignature = 0;
        nvrhi::rt::AccelStructDesc blasDesc;
        nvrhi::rt::AccelStructHandle blas;
        bool buildSubmitted = false;
        uint64 deferredSinceFrame = 0;
    };

    struct StaticBucketBlasRecord
    {
        uint64 bucketKey = 0;
        uint64 inputSignature = 0;
        RtSmokePlanGeometryRange range;
        uint32_t firstSurfaceRecord = 0;
        uint32_t surfaceRecordCount = 0;
        uint32_t geometryDescCount = 0;
        nvrhi::rt::AccelStructDesc blasDesc;
        nvrhi::rt::AccelStructHandle blas;
        bool buildSubmitted = false;
        bool seenThisUpdate = false;
        uint64 deferredSinceFrame = 0;
    };

    struct StaticBucketAdmissionIntervalStats
    {
        int deferredOperationBudget = 0;
        int deferredResultByteBudget = 0;
        int deferredUnknownResultBytes = 0;
        int deferredAllocationFailure = 0;
        int resultRequirementQueries = 0;
        int resultRequirementFailures = 0;
        int oversizedResultAdmissions = 0;
        uint64 admittedResultBytes = 0;
        uint64 oversizedResultBytes = 0;
        uint64 maxDeferredAge = 0;
    };

    struct LegacyRigidAdmissionIntervalStats
    {
        int deferredOperationBudget = 0;
        int deferredResultByteBudget = 0;
        int deferredUnknownResultBytes = 0;
        int deferredAllocationFailure = 0;
        int resultRequirementQueries = 0;
        int resultRequirementFailures = 0;
        int oversizedResultAdmissions = 0;
        uint64 admittedResultBytes = 0;
        uint64 oversizedResultBytes = 0;
        uint64 maxDeferredAge = 0;
    };

    RtSmokePersistentStaticSurfaceRecord* FindStaticSurfaceMutable(uint64 key);
    bool EnsureRigidMeshCandidateLookupLive(uint64 meshHash) const;
    bool RigidMeshCandidateLookupFind(uint64 meshHash, size_t& recordIndex) const;
    bool RigidMeshCandidateLookupBind(uint64 meshHash, size_t recordIndex);
    bool RigidMeshCandidateLookupFindUnlocked(uint64 meshHash, size_t& recordIndex) const;
    bool RigidMeshCandidateLookupBindUnlocked(uint64 meshHash, size_t recordIndex);
    void QuarantineRigidMeshCandidateLookup(
        const char* reason,
        uint64 meshHash,
        uint64 mapSize,
        uint64 mapBuckets) const;
    bool RebuildRigidMeshCandidateLookupFromRecords();
    bool RebuildRigidMeshCandidateLookupFromRecordsUnlocked();
    void RetireRigidBlas(RigidMeshCandidateRecord& record);
    void RetireRigidBuffer(nvrhi::BufferHandle& buffer);
    void RetireRigidMeshGpuResources(RigidMeshCandidateRecord& record);
    void ClearRigidResidencyCaches();
    void ResetRigidMeshCandidateFrameStats();
    CanonicalRigidBlasRecord* FindCanonicalRigidBlasRecord(
        const PtCanonicalMeshKey& key,
        uint64 meshHash);
    uint32_t FindCanonicalRigidBlasRecordIndex(
        const PtCanonicalMeshKey& key,
        uint64 meshHash) const;
    void RetireCanonicalRigidBlas(
        CanonicalRigidBlasRecord& record);
    void ReleaseCanonicalRigidBlasScaffold();
    void RetireStaticBucketBuffer(
        nvrhi::BufferHandle& buffer);
    void RetireStaticBucketBlas(
        StaticBucketBlasRecord& record);
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
    uint64 m_staticResidentPayloadGeneration = 1;
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
    LegacyRigidAdmissionIntervalStats
        m_legacyRigidAdmissionIntervalStats;
    RtSmokeRetiredRigidGpuResources m_retiredRigidGpuResources;
    mutable std::mutex m_rigidMeshCandidateLookupMutex;
    std::unordered_map<uint64, size_t> m_rigidMeshCandidateLookup;
	uint64 m_rigidMeshCandidateRecordInsertTotal = 0;
	uint64 m_rigidMeshCandidateRecordInsertFrameStart = 0;
	mutable uint64 m_rigidMeshCandidateSemanticRevision = 1;
	uint64 m_rigidMeshCandidateFrameBeginSerial = 1;
    mutable uint32_t m_rigidMeshCandidateLookupHealth = 0;
    mutable uint32_t m_rigidMeshCandidateLookupReason = 0;
    mutable uint32_t m_rigidMeshCandidateLookupCanary = 0xA41104CAu;
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
    PtGeometrySourceRegistry m_canonicalSourceRegistry;
    PtGeometryIdentityRegistry m_canonicalIdentityRegistry;
    std::vector<PtA8S1RouteObservation> m_a8S1RouteObservations;
    PtA8S1AliasSidecar m_a8S1AliasSidecar;
    bool m_a8S1RouteObservationsAvailable = true;
    std::array<uint64, static_cast<size_t>(PtA8S1RouteProducer::Count)>
        m_a8S1ProducerOpportunities = {};
    std::array<uint64, static_cast<size_t>(PtA8S1RouteProducer::Count)>
        m_a8S1ProducerObserved = {};
    std::array<uint64, static_cast<size_t>(PtA8S1RouteProducer::Count)>
        m_a8S1ProducerSuppressedDiagnosticUnavailable = {};
    PtGeometryGpuPoolSet m_canonicalSourceGpuPools;
    PtGeometryGpuPoolStats m_canonicalSourceGpuPoolStats;
    PtGeometryOffsetBlasProbe m_canonicalOffsetBlasProbe;
    std::vector<CanonicalRigidBlasRecord> m_canonicalRigidBlasRecords;
    std::unordered_multimap<uint64, size_t> m_canonicalRigidBlasLookup;
    RtPathTraceCanonicalRigidBlasStats m_canonicalRigidBlasStats;
    uint64 m_canonicalSourceWorldGeneration = 0;
    uint64 m_canonicalSourcePublicationGeneration = 0;
    uint64 m_canonicalSourcePublicationSequence = 0;
    uint64 m_canonicalSourceImportCursor = 0;
    uint64 m_canonicalSourceImported = 0;
    uint64 m_canonicalSourceReused = 0;
    uint64 m_canonicalSourceRevised = 0;
    uint64 m_canonicalSourceRejected = 0;
    uint64 m_canonicalSourceTransportBytes = 0;
    int m_rigidResidencyAreaWalkDuplicateVisibleThisFrame = 0;
    int m_rigidResidencyAreaWalkDuplicateFrameThisFrame = 0;
    int m_rigidResidencyAreaWalkInstancesThisFrame = 0;
    bool m_rigidResidencyEnabled = false;
    const idRenderWorldLocal* m_rigidResidencyWorld = nullptr;
    nvrhi::BufferHandle m_staticBucketVertexBuffer;
    nvrhi::BufferHandle m_staticBucketIndexBuffer;
    nvrhi::BufferHandle m_staticBucketTriangleClassBuffer;
    nvrhi::BufferHandle m_staticBucketTriangleMaterialBuffer;
    nvrhi::BufferHandle m_staticBucketTriangleMaterialIndexBuffer;
    std::vector<StaticBucketBlasRecord> m_staticBucketBlasRecords;
    StaticBucketAdmissionIntervalStats
        m_staticBucketAdmissionIntervalStats;
    RtSmokeRetiredStaticBucketGpuResources
        m_retiredStaticBucketGpuResources;
    uint64 m_staticBucketUploadSignature = 0;
    uint64 m_staticBucketMaterialIndexUploadSignature = 0;
    RtSmokeStaticBucketGeometryPack
        m_staticBucketResidentGeometryPack;
    RtSmokeStaticBucketAssignmentPlan
        m_staticBucketResidentAssignmentPlan;
    RtSmokeStaticBucketAssignmentPlan
        m_staticBucketAssignmentPlanCache;
    uint64 m_staticBucketAssignmentWorldGeneration = 0;
    uint64 m_staticBucketAssignmentSourceGeneration = 0;
    uint64 m_staticBucketAssignmentStorageGeneration = 0;
    int m_staticBucketAssignmentPortalAreaCount = 0;
    int m_staticBucketAssignmentMaxVertices = 0;
    int m_staticBucketAssignmentMaxIndexes = 0;
    int m_staticBucketAssignmentMaxTriangles = 0;
    bool m_staticBucketAssignmentPlanCacheValid = false;
    uint64 m_staticBucketResidentAssignmentPlanSignature = 0;
    uint64 m_staticBucketResidentGeometryGeneration = 0;
    uint64 m_staticBucketResidentMaterialGeneration = 0;
    bool m_staticBucketResidentGeometryPackValid = false;
    std::vector<uint32_t> m_staticBucketMaterialIndexes;
    std::vector<int> m_staticBucketMissingMaterialIndexesByBucket;
    std::vector<uint32_t> m_staticBucketReferencedMaterialIds;
    uint64 m_staticBucketReferencedMaterialIdsResidentPackSignature = 0;
    uint64 m_staticBucketMaterialIndexResidentPackSignature = 0;
    uint64 m_staticBucketMaterialIndexBindingSignature = 0;
    bool m_staticBucketMaterialIndexCacheValid = false;
};

bool RtPathTraceRigidPreparedPayloadsCompatible(
    const std::vector<RtPathTraceRigidPreparedPayload>& payloads,
    const std::vector<RtPathTraceRigidMeshCandidateObservation>& observations) noexcept;

size_t ReplayPathTraceDeferredRigidCandidates(
    RtSmokeGeometryUniverse& geometryUniverse,
    bool deferralActive,
    bool compactApplied,
    const std::vector<RtPathTraceRigidMeshCandidateObservation>& observations,
    size_t observationBegin = 0,
    size_t alreadyReplayedCount = 0);

RtPathTraceRigidRouteBuild BuildRigidRouteBuffersFromSnapshot(
    const RtPathTraceRigidRouteBuildSnapshot& snapshot);
bool BuildRigidRouteBuffersFromSnapshotPreReserved(
    const RtPathTraceRigidRouteBuildSnapshot& snapshot,
    RtPathTraceRigidRouteBuild& build,
    std::vector<uint64_t>& emittedMeshScratch);
bool BuildRigidRouteBuffersFromResidentPayloadPreReserved(
    const std::vector<RtPathTraceRigidRouteMeshSnapshot>& meshes,
    const RtSmokeRigidTlasPlan& plan,
    const std::vector<uint32_t>& materialTableIds,
    const std::vector<RtPathTraceRigidRouteInstanceEligibility>& instanceEligibility,
    RtPathTraceRigidRouteBuild& build,
    std::vector<uint64_t>& emittedMeshScratch);
RtPathTraceRigidRouteInstanceEligibility
BuildRigidRouteInstanceEligibilityFromPod(
    const RtSmokePlanTlasInstance& plannedInstance,
    const RtPathTraceRigidRouteMeshSnapshot& mesh);

bool UpdateRigidRouteGeometryFromSnapshot(
    RtPathTraceRigidRouteBuild& build,
    const RtPathTraceRigidRouteBuildSnapshot& snapshot);

bool RigidRouteGeometrySnapshotCovered(
    const RtPathTraceRigidRouteBuild& build,
    const RtPathTraceRigidRouteBuildSnapshot& snapshot);

bool RemapRigidRouteMaterialIndexes(
    RtPathTraceRigidRouteBuild& build,
    const std::vector<uint32_t>& materialTableIds,
    bool* geometryChangedOut = nullptr,
    bool* instanceChangedOut = nullptr);

void RebuildRigidRouteInstancesFromSnapshot(
    RtPathTraceRigidRouteBuild& build,
    const RtPathTraceRigidRouteBuildSnapshot& snapshot);

uint64_t BuildRigidRouteGeometryUploadSignature(
    const RtPathTraceRigidRouteBuild& build);

uint64_t BuildRigidRouteInstanceUploadSignature(
    const RtPathTraceRigidRouteBuild& build);

RtPathTraceRigidRouteBuildTimedResult BuildRigidRouteBuffersTimedResult(
    const RtPathTraceRigidRouteBuildSnapshot& snapshot);
