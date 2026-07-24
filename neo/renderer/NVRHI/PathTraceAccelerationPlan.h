#pragma once

// CPU-only acceleration planning for the PT smoke scene.
//
// These structs are intentionally plain data. They may be produced by a worker
// from an immutable snapshot, but they never contain NVRHI handles, command
// lists, binding resources, or live renderer/game pointers.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

struct RtSmokePlanVec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct RtSmokePlanGeometryRange
{
    int vertexOffset = 0;
    int vertexCount = 0;
    int indexOffset = 0;
    int indexCount = 0;
    int triangleOffset = 0;
    int triangleCount = 0;
};

struct RtSmokePlanStaticBlasSignature
{
    uint64_t hash = 0;
    int vertexCount = 0;
    int indexCount = 0;
    int triangleCount = 0;
};

struct RtSmokePlanStaticBlasSignatureDesc
{
    const void* vertices = nullptr;
    size_t vertexStride = 0;
    int totalVertexCount = 0;
    const uint32_t* indexes = nullptr;
    int totalIndexCount = 0;
    const uint32_t* triangleClasses = nullptr;
    const uint32_t* triangleMaterials = nullptr;
    int totalTriangleCount = 0;
    RtSmokePlanGeometryRange staticRange;
    RtSmokePlanVec3 sceneOrigin;
};

struct RtSmokeStaticBlasSignatureSnapshot
{
    std::vector<uint8_t> vertexBytes;
    size_t vertexStride = 0;
    int totalVertexCount = 0;
    std::vector<uint32_t> indexes;
    std::vector<uint32_t> triangleClasses;
    std::vector<uint32_t> triangleMaterials;
    RtSmokePlanGeometryRange staticRange;
    RtSmokePlanVec3 sceneOrigin;
};

struct RtSmokePlanStaticCacheInput
{
    bool hasStaticBlas = false;
    bool cacheValid = false;
    bool cacheResourcesReady = false;
    bool staticCacheChanged = false;
    uint64_t previousSignatureHash = 0;
};

struct RtSmokePlanBlasCreate
{
    bool enabled = false;
    bool cacheHit = false;
    int vertexCount = 0;
    int indexCount = 0;
    const char* debugName = nullptr;
};

struct RtSmokeAccelerationPlanInput
{
    RtSmokePlanStaticBlasSignatureDesc staticSignature;
    RtSmokePlanStaticCacheInput staticCache;
    int staticVertexCount = 0;
    int staticIndexCount = 0;
    int dynamicVertexCount = 0;
    int dynamicIndexCount = 0;
};

struct RtSmokeAccelerationPlanSnapshot
{
    RtSmokeStaticBlasSignatureSnapshot staticSignature;
    RtSmokePlanStaticCacheInput staticCache;
    int staticVertexCount = 0;
    int staticIndexCount = 0;
    int dynamicVertexCount = 0;
    int dynamicIndexCount = 0;
};

struct RtSmokeAccelerationPlan
{
    RtSmokePlanStaticBlasSignature staticSignature;
    RtSmokePlanBlasCreate staticBlas;
    RtSmokePlanBlasCreate dynamicBlas;
    bool staticSignatureReused = false;
    bool staticCacheHit = false;
    bool hasStaticBlas = false;
    bool hasDynamicBlas = false;
};

struct RtSmokeAccelerationPlanResult
{
    RtSmokeAccelerationPlan plan;
    bool valid = false;
};

struct RtSmokeAccelerationPlanTimedResult
{
    RtSmokeAccelerationPlanResult result;
    double workerExecutionMs = 0.0;
};

enum RtSmokePlanTlasInstanceKind : uint32_t
{
    RT_SMOKE_PLAN_TLAS_STATIC_BLAS = 0,
    RT_SMOKE_PLAN_TLAS_DYNAMIC_BLAS = 1,
    RT_SMOKE_PLAN_TLAS_RIGID_BLAS = 2,
    RT_SMOKE_PLAN_TLAS_STATIC_BUCKET_BLAS = 3
};

enum RtSmokeStaticActiveReasonFlags : uint32_t
{
    RT_SMOKE_STATIC_ACTIVE_VISIBLE = 1u << 0,
    RT_SMOKE_STATIC_ACTIVE_SELECTED_AREA = 1u << 1,
    RT_SMOKE_STATIC_ACTIVE_RESIDENCY = 1u << 2,
    RT_SMOKE_STATIC_ACTIVE_EMISSIVE_PLACEHOLDER = 1u << 3,
    RT_SMOKE_STATIC_ACTIVE_FORCE_INCLUDE = 1u << 4
};

struct RtSmokePlanTlasInstance
{
    RtSmokePlanTlasInstanceKind kind = RT_SMOKE_PLAN_TLAS_STATIC_BLAS;
    uint32_t instanceId = 0;
    uint32_t instanceMask = 0;
    uint32_t hitGroupContribution = 0;
    uint32_t flags = 0;
    uint64_t meshHash = 0;
    uint64_t sourceInstanceId = 0;
    uint32_t routeRecordIndex = std::numeric_limits<uint32_t>::max();
    uint32_t canonicalBlasRecordIndex =
        std::numeric_limits<uint32_t>::max();
    uint64_t canonicalMeshHash = 0;
    bool sourceSeenThisFrame = true;
    bool hasPreviousTransform = false;
    bool transformContinuous = false;
    float transform[16] = {};
    float previousTransform[16] = {};
};

struct RtSmokeBaseTlasPlan
{
    RtSmokePlanTlasInstance instances[2];
    int instanceCount = 0;
};

struct RtSmokeAccelerationSubmitPlanInput
{
    bool hasStaticBlas = false;
    bool hasDynamicBlas = false;
    bool staticBlasCacheHit = false;
};

struct RtSmokeAccelerationSubmitPlan
{
    RtSmokeBaseTlasPlan baseTlasPlan;
    bool buildStaticBlas = false;
    bool buildDynamicBlas = false;
    bool submitTlas = false;
};

struct RtSmokeStaticTlasBucketObservation
{
    uint64_t bucketKey = 0;
    uint32_t activeReasonFlags = 0;
    bool resident = false;
    bool active = false;
    bool hasBlas = false;
    uint32_t routeRecordIndex = std::numeric_limits<uint32_t>::max();
    int residentVertexOffset = 0;
    int residentIndexOffset = 0;
    int residentTriangleOffset = 0;
    int residentSurfaceCount = 0;
    int residentVertexCount = 0;
    int residentIndexCount = 0;
    int residentTriangleCount = 0;
    int activeSurfaceCount = 0;
    int activeVertexCount = 0;
    int activeIndexCount = 0;
    int activeTriangleCount = 0;
};

struct RtSmokeStaticTlasBucketObservationInput
{
    uint64_t bucketKey = 0;
    uint32_t routeRecordIndex = std::numeric_limits<uint32_t>::max();
    uint32_t activeReasonFlags = 0;
    int vertexOffset = 0;
    int indexOffset = 0;
    int triangleOffset = 0;
    int vertexCount = 0;
    int indexCount = 0;
    int triangleCount = 0;
    bool valid = false;
    bool seenThisFrame = false;
    bool hasBlas = false;
};

struct RtSmokeStaticTlasActiveSetPlanDesc
{
    const RtSmokeStaticTlasBucketObservation* buckets = nullptr;
    int bucketCount = 0;
    bool monolithicStaticBlas = true;
    bool hasStaticBlas = false;
    uint32_t firstInstanceId = 0;
    uint32_t instanceMask = 0x01;
};

struct RtSmokeStaticTlasActiveSetPlan
{
    std::vector<RtSmokePlanTlasInstance> instances;
    uint64_t activeSetSignature = 0;
    uint64_t residentSetSignature = 0;
    uint64_t tlasInstanceSignature = 0;
    int residentBuckets = 0;
    int activeBuckets = 0;
    int inactiveResidentBuckets = 0;
    int emittedInstances = 0;
    int residentSurfaceCount = 0;
    int residentVertexCount = 0;
    int residentIndexCount = 0;
    int residentTriangleCount = 0;
    int activeSurfaceCount = 0;
    int activeVertexCount = 0;
    int activeIndexCount = 0;
    int activeTriangleCount = 0;
    bool monolithicStaticBlas = true;
    bool inactiveResidentGeometryIncluded = false;
    bool requiresBucketedStaticBlas = false;
};

struct RtSmokeStaticBucketBlasRecord
{
    uint64_t bucketKey = 0;
    uint32_t routeRecordIndex = std::numeric_limits<uint32_t>::max();
    uint32_t activeReasonFlags = 0;
    RtSmokePlanGeometryRange range;
    bool active = false;
};

struct RtSmokeStaticBucketBlasPlanDesc
{
    const RtSmokeStaticTlasBucketObservation* buckets = nullptr;
    int bucketCount = 0;
    bool activeOnly = true;
    int maxRecords = 0;
};

struct RtSmokeStaticBucketBlasPlan
{
    std::vector<RtSmokeStaticBucketBlasRecord> records;
    uint64_t planSignature = 0;
    int residentBuckets = 0;
    int activeBuckets = 0;
    int emittedRecords = 0;
    int skippedInactive = 0;
    int skippedInvalid = 0;
    bool overflow = false;
};

struct RtSmokeStaticBucketTraversalCompatibilityInput
{
    const RtSmokeStaticBucketBlasRecord* records = nullptr;
    int recordCount = 0;
    int totalVertexCount = 0;
    int totalIndexCount = 0;
    int totalTriangleCount = 0;
    bool shaderSupportsStaticBucketRoutes = false;
};

struct RtSmokeStaticBucketTraversalCompatibility
{
    int recordCount = 0;
    int nonZeroOffsetRecords = 0;
    bool exactMonolithicRecord = false;
    bool currentStaticShaderCompatible = true;
    bool requiresShaderRouteMetadata = false;
};

struct RtSmokeRouteInstanceNamespacePlanInput
{
    int staticRouteRecordCount = 0;
    int rigidRouteRecordCount = 0;
    uint32_t firstRouteInstanceId = 2;
    bool enableStaticRoutes = false;
    bool shaderSupportsStaticBucketRoutes = false;
};

struct RtSmokeRouteInstanceNamespacePlan
{
    uint32_t staticFirstInstanceId = 0;
    uint32_t rigidFirstInstanceId = 2;
    int staticRouteInstanceCount = 0;
    int rigidRouteInstanceCount = 0;
    bool staticRoutesEnabled = false;
    bool staticRoutesRequireShaderSupport = false;
    bool staticRoutesBlocked = false;
    bool rigidRouteBaseShifted = false;
};

struct RtSmokeStaticRouteTableRecord
{
    uint64_t bucketKey = 0;
    uint32_t instanceId = 0;
    uint32_t routeRecordIndex = std::numeric_limits<uint32_t>::max();
    uint32_t activeReasonFlags = 0;
    RtSmokePlanGeometryRange range;
};

struct RtSmokeStaticRouteTablePlanInput
{
    const RtSmokeStaticBucketBlasRecord* records = nullptr;
    int recordCount = 0;
    int maxRecords = 0;
    RtSmokeRouteInstanceNamespacePlan routeNamespace;
};

struct RtSmokeStaticRouteTablePlan
{
    std::vector<RtSmokeStaticRouteTableRecord> records;
    uint64_t tableSignature = 0;
    int inputRecords = 0;
    int emittedRecords = 0;
    int skippedDisabled = 0;
    int skippedInvalid = 0;
    bool blocked = false;
    bool overflow = false;
};

struct RtSmokeStaticBucketBlasBuildPlanInput
{
    bool submitBuilds = false;
    bool forceRebuild = false;
    bool hasBlas = false;
    bool uploadRequired = false;
    bool blasInputsCompatible = false;
    bool signatureValid = false;
    uint64_t previousBlasInputSignature = 0;
    uint64_t currentBlasInputSignature = 0;
};

struct RtSmokeStaticBucketBlasBuildPlan
{
    bool createBlas = false;
    bool submitBuild = false;
    bool skipBuild = false;
    bool signatureChanged = false;
};

struct RtSmokeStaticBucketBlasBuildObservation
{
    uint64_t bucketKey = 0;
    bool hasBlas = false;
    bool uploadRequired = false;
    bool blasInputsCompatible = false;
    bool signatureValid = false;
    uint64_t previousBlasInputSignature = 0;
    uint64_t currentBlasInputSignature = 0;
};

struct RtSmokeStaticBucketBlasBuildBatchPlanInput
{
    const RtSmokeStaticBucketBlasBuildObservation* observations = nullptr;
    int observationCount = 0;
    bool submitBuilds = false;
    bool forceRebuild = false;
    int maxRecords = 0;
};

struct RtSmokeStaticBucketBlasBuildBatchRecord
{
    uint64_t bucketKey = 0;
    RtSmokeStaticBucketBlasBuildPlan buildPlan;
};

struct RtSmokeStaticBucketBlasBuildBatchPlan
{
    std::vector<RtSmokeStaticBucketBlasBuildBatchRecord> records;
    uint64_t planSignature = 0;
    int inputRecords = 0;
    int emittedRecords = 0;
    int createBlasRecords = 0;
    int submitBuildRecords = 0;
    int skippedBuildRecords = 0;
    int signatureChangedRecords = 0;
    int uploadRequiredRecords = 0;
    int incompatibleRecords = 0;
    int missingBlasRecords = 0;
    bool overflow = false;
};

struct RtSmokeStaticBvhBucketSignatureInput
{
    RtSmokeStaticTlasBucketObservation bucket;
    uint64_t geometryContentSignature = 0;
    uint64_t materialGeneration = 0;
};

struct RtSmokeStaticBvhBucketSignature
{
    uint64_t bucketKey = 0;
    uint64_t residentSignature = 0;
    uint64_t activeSignature = 0;
    uint64_t geometryInputSignature = 0;
    uint64_t blasInputSignature = 0;
    bool resident = false;
    bool active = false;
};

struct RtSmokeStaticBucketBlasCacheState
{
    uint64_t bucketKey = 0;
    uint64_t blasInputSignature = 0;
    bool hasBlas = false;
    bool blasInputsCompatible = false;
};

struct RtSmokeStaticBucketBlasBuildObservationPlanInput
{
    const RtSmokeStaticBvhBucketSignature* currentBuckets = nullptr;
    int currentBucketCount = 0;
    const RtSmokeStaticBucketBlasCacheState* previousBuckets = nullptr;
    int previousBucketCount = 0;
    int maxRecords = 0;
};

struct RtSmokeStaticBucketBlasBuildObservationPlan
{
    std::vector<RtSmokeStaticBucketBlasBuildObservation> observations;
    uint64_t planSignature = 0;
    int inputBuckets = 0;
    int emittedObservations = 0;
    int cacheHits = 0;
    int cacheMisses = 0;
    int signatureChanged = 0;
    int uploadRequired = 0;
    int skippedInactive = 0;
    bool overflow = false;
};

struct RtSmokeStaticBucketWorkPlanInput
{
    const RtSmokeStaticTlasBucketObservation* buckets = nullptr;
    int bucketCount = 0;
    const RtSmokeStaticBucketBlasCacheState* previousBuckets = nullptr;
    int previousBucketCount = 0;
    uint64_t geometryContentSignature = 0;
    uint64_t materialGeneration = 0;
    int totalVertexCount = 0;
    int totalIndexCount = 0;
    int totalTriangleCount = 0;
    bool monolithicStaticBlas = true;
    bool hasStaticBlas = false;
    bool submitBuilds = false;
    bool forceRebuild = false;
    bool enableStaticRoutes = false;
    bool shaderSupportsStaticBucketRoutes = false;
    uint32_t firstRouteInstanceId = 2;
    int rigidRouteRecordCount = 0;
    int maxBucketRecords = 0;
    int maxRouteRecords = 0;
    int maxBuildRecords = 0;
};

struct RtSmokeStaticBucketWorkPlanSnapshot
{
    std::vector<RtSmokeStaticTlasBucketObservation> buckets;
    std::vector<RtSmokeStaticBucketBlasCacheState> previousBuckets;
    uint64_t geometryContentSignature = 0;
    uint64_t materialGeneration = 0;
    int totalVertexCount = 0;
    int totalIndexCount = 0;
    int totalTriangleCount = 0;
    bool monolithicStaticBlas = true;
    bool hasStaticBlas = false;
    bool submitBuilds = false;
    bool forceRebuild = false;
    bool enableStaticRoutes = false;
    bool shaderSupportsStaticBucketRoutes = false;
    uint32_t firstRouteInstanceId = 2;
    int rigidRouteRecordCount = 0;
    int maxBucketRecords = 0;
    int maxRouteRecords = 0;
    int maxBuildRecords = 0;
};

struct RtSmokeStaticBucketWorkPlan
{
    std::vector<RtSmokeStaticBvhBucketSignature> bucketSignatures;
    RtSmokeStaticTlasActiveSetPlan activeSetPlan;
    RtSmokeStaticBucketBlasPlan bucketBlasPlan;
    RtSmokeStaticBucketTraversalCompatibility traversalCompatibility;
    RtSmokeRouteInstanceNamespacePlan routeNamespace;
    RtSmokeStaticRouteTablePlan routeTablePlan;
    RtSmokeStaticBucketBlasBuildObservationPlan buildObservationPlan;
    RtSmokeStaticBucketBlasBuildBatchPlan buildBatchPlan;
    uint64_t planSignature = 0;
};

struct RtSmokeStaticBucketWorkPlanTimedResult
{
    RtSmokeStaticBucketWorkPlan plan;
    uint64_t planningTimeMicros = 0;
};

struct RtSmokeBvhDirtyTokenState
{
    uint64_t geometryContentSignature = 0;
    uint64_t activeBlasInputSignature = 0;
    uint64_t residentSetSignature = 0;
    uint64_t materialGeneration = 0;
    uint64_t activeSetSignature = 0;
    uint64_t tlasInstanceSignature = 0;
};

struct RtSmokeStaticBucketWorkDirtyTokenInput
{
    const RtSmokeStaticBucketWorkPlan* plan = nullptr;
    uint64_t materialGeneration = 0;
};

struct RtSmokeBvhDirtyPlanInput
{
    bool previousValid = false;
    RtSmokeBvhDirtyTokenState previous;
    RtSmokeBvhDirtyTokenState current;
};

struct RtSmokeBvhDirtyPlan
{
    bool geometryContentChanged = false;
    bool activeGeometryContentChanged = false;
    bool residentSetChanged = false;
    bool materialChanged = false;
    bool activeMembershipChanged = false;
    bool tlasInstanceChanged = false;
    bool blasInputDirty = false;
    bool tlasDirty = false;
};

struct RtSmokeBvhFrameTokenInput
{
    uint64_t staticBlasSignature = 0;
    uint64_t geometryGeneration = 0;
    uint64_t materialGeneration = 0;
    uint64_t staticActiveSetSignature = 0;
    uint64_t staticResidentSetSignature = 0;
    uint64_t staticTlasInstanceSignature = 0;
    int dynamicVertexCount = 0;
    int dynamicIndexCount = 0;
    int rigidRouteVertexCount = 0;
    int rigidRouteIndexCount = 0;
    int rigidRouteTriangleCount = 0;
    int rigidRouteInstanceCount = 0;
    int rigidRouteSeenThisFrameCount = 0;
    int rigidRouteCachedInstanceCount = 0;
    uint64_t rigidTlasInstanceSignature = 0;
    int baseTlasInstanceCount = 0;
    int rigidTlasInstanceCount = 0;
    bool hasStaticBlas = false;
    bool hasDynamicBlas = false;
};

struct RtSmokeBvhFrameToken
{
    RtSmokeBvhDirtyTokenState dirtyToken;
    uint64_t residentSetSignature = 0;
};

struct RtSmokeBvhFramePlanningInput
{
    RtSmokeStaticBucketWorkPlanInput staticBucketWorkInput;
    RtSmokeBvhFrameTokenInput frameTokenInput;
    RtSmokeBvhDirtyTokenState previousDirtyToken;
    bool previousDirtyTokenValid = false;
};

struct RtSmokeBvhFramePlanningSnapshot
{
    RtSmokeStaticBucketWorkPlanSnapshot staticBucketWorkSnapshot;
    RtSmokeBvhFrameTokenInput frameTokenInput;
    RtSmokeBvhDirtyTokenState previousDirtyToken;
    bool previousDirtyTokenValid = false;
};

struct RtSmokeBvhFramePlanningResult
{
    RtSmokeStaticBucketWorkPlan staticBucketWorkPlan;
    RtSmokeBvhFrameToken frameToken;
    RtSmokeBvhDirtyPlan dirtyPlan;
};

struct RtSmokeBvhFramePlanningTimedResult
{
    RtSmokeBvhFramePlanningResult result;
    uint64_t planningTimeMicros = 0;
};

struct RtSmokeRigidTlasObservation
{
    uint64_t meshHash = 0;
    uint64_t instanceId = 0;
    uint32_t sourceFlags = 0;
    bool hasMeshRecord = false;
    bool meshSeenThisFrame = false;
    bool residencyEnabled = false;
    bool hasBlas = false;
    uint32_t routeRecordIndex = std::numeric_limits<uint32_t>::max();
    uint32_t canonicalBlasRecordIndex =
        std::numeric_limits<uint32_t>::max();
    uint64_t canonicalMeshHash = 0;
    bool seenThisFrame = true;
    bool hasPreviousObjectToWorld = false;
    bool transformContinuous = false;
    float objectToWorld[16] = {};
    float previousObjectToWorld[16] = {};
};

struct RtSmokeRigidTlasPlanDesc
{
    const RtSmokeRigidTlasObservation* observations = nullptr;
    int observationCount = 0;
    uint32_t rigidSourceMask = 0;
    uint32_t firstInstanceId = 0;
    uint32_t instanceMask = 0;
    int maxInstances = 0;
};

struct RtSmokeRigidTlasPlanSnapshot
{
    std::vector<RtSmokeRigidTlasObservation> observations;
    uint32_t rigidSourceMask = 0;
    uint32_t firstInstanceId = 0;
    uint32_t instanceMask = 0;
    int maxInstances = 0;
};

struct RtSmokeRigidTlasPlan
{
    std::vector<RtSmokePlanTlasInstance> instances;
    uint64_t tlasInstanceSignature = 0;
    int visibleInstances = 0;
    int rigidInstances = 0;
    int emittedInstances = 0;
    int rejectedNonRigid = 0;
    int rejectedMissingMesh = 0;
    int rejectedStaleMesh = 0;
    int rejectedMissingBlas = 0;
};

struct RtSmokeCanonicalRigidTlasSelectionInput
{
    bool providerEnabled = false;
    bool traversalRequested = false;
    int legacyDescriptors = 0;
    int canonicalDescriptors = 0;
    int exactRecordMappings = 0;
    int missingRecordIndex = 0;
    int meshHashMismatch = 0;
    int missingBlas = 0;
};

struct RtSmokeCanonicalRigidTlasSelection
{
    bool exactParity = false;
    bool selectCanonical = false;
};

struct RtSmokeRigidTlasPlanTimedResult
{
    RtSmokeRigidTlasPlan plan;
    uint64_t planningTimeMicros = 0;
};

struct RtSmokeRigidBlasBuildPlanInput
{
    bool submitBuilds = false;
    bool forceRebuild = false;
    bool hasBlas = false;
    bool uploadRequired = false;
    bool blasInputsCompatible = false;
};

struct RtSmokeRigidBlasBuildPlan
{
    bool createBlas = false;
    bool submitBuild = false;
    bool skipBuild = false;
};

struct RtSmokeRigidBvhObjectSignatureInput
{
    uint64_t meshHash = 0;
    uint64_t instanceId = 0;
    uint64_t geometryContentSignature = 0;
    uint64_t materialGeneration = 0;
    uint32_t sourceFlags = 0;
    uint32_t rigidSourceMask = 0;
    uint32_t routeRecordIndex = std::numeric_limits<uint32_t>::max();
    int vertexCount = 0;
    int indexCount = 0;
    bool hasMeshRecord = false;
    bool meshSeenThisFrame = false;
    bool residencyEnabled = false;
};

struct RtSmokeRigidBvhObjectSignature
{
    uint64_t objectKey = 0;
    uint64_t geometryInputSignature = 0;
    uint64_t blasInputSignature = 0;
    uint64_t tlasMembershipSignature = 0;
    bool resident = false;
    bool activeCandidate = false;
};

struct RtSmokeUploadPlanMetadata
{
    bool skip = false;
    size_t byteSize = 0;
    size_t sourceOffsetBytes = 0;
    uint64_t destOffsetBytes = 0;
};

struct RtSmokeStaticDirtyUploadPlanInput
{
    bool staticBlasCacheHit = false;
    bool staticCacheChanged = false;
    bool staticGeometryBuffersReused = false;
    int staticDirtyCount = 0;
    int dirtyVertexOffset = -1;
    int dirtyVertexCount = 0;
    size_t totalVertexCount = 0;
    int dirtyIndexOffset = -1;
    int dirtyIndexCount = 0;
    size_t totalIndexCount = 0;
    int dirtyTriangleOffset = -1;
    int dirtyTriangleCount = 0;
    size_t totalTriangleClassCount = 0;
    size_t totalTriangleMaterialCount = 0;
};

struct RtSmokeStaticDirtyUploadPlan
{
    bool dirtyRangesValid = false;
    bool useDirtyRangeUploads = false;
};

struct RtSmokeStaticVertexUploadPlanInput
{
    bool forceRebuildWithoutUpload = false;
    bool staticBlasCacheHit = false;
    bool useDirtyRangeUploads = false;
    bool fullUploadOnCacheMissWithTexMatrices = false;
    int dirtyVertexOffset = -1;
    int dirtyVertexCount = 0;
    int texMatrixVertexCount = 0;
    int texMatrixFirstVertex = -1;
    int texMatrixLastVertex = -1;
    size_t totalVertexCount = 0;
};

struct RtSmokeStaticVertexUploadPlan
{
    bool skipUpload = false;
    bool fullUpload = false;
    bool texMatrixRangeUpload = false;
    bool dirtyRangeUpload = false;
    int elementOffset = -1;
    int elementCount = 0;
};

struct RtSmokePlanDataSpan
{
    const void* data = nullptr;
    size_t elementSize = 0;
    size_t elementCount = 0;
};

struct RtSmokePreviousStaticSnapshotUploadPlanInput
{
    bool dataAvailable = false;
    bool buffersReused = false;
    uint64_t previousUploadSignature = 0;
    uint64_t currentUploadSignature = 0;
};

struct RtSmokePreviousStaticSnapshotUploadPlan
{
    bool skipUpload = false;
};

uint64_t HashSmokePlanBytes(uint64_t hash, const void* data, size_t size);

RtSmokeStaticBlasSignatureSnapshot CaptureSmokeStaticBlasSignatureSnapshot(
    const RtSmokePlanStaticBlasSignatureDesc& desc);

RtSmokeAccelerationPlanSnapshot CaptureSmokeAccelerationPlanSnapshot(
    const RtSmokeAccelerationPlanInput& input);

uint64_t BuildSmokeAccelerationPlanInputToken(
    const RtSmokeAccelerationPlanInput& input);

RtSmokePlanStaticBlasSignature ComputeSmokeStaticBlasSignaturePlan(
    const RtSmokePlanStaticBlasSignatureDesc& desc);

RtSmokeAccelerationPlan BuildSmokeAccelerationPlan(
    const RtSmokeAccelerationPlanInput& input);

RtSmokeAccelerationPlanResult BuildSmokeAccelerationPlanResult(
    const RtSmokeAccelerationPlanSnapshot& snapshot);

RtSmokeAccelerationPlanTimedResult BuildSmokeAccelerationPlanTimedResult(
    const RtSmokeAccelerationPlanSnapshot& snapshot);

RtSmokeBaseTlasPlan BuildSmokeBaseTlasPlan(bool hasStaticBlas, bool hasDynamicBlas);

RtSmokeAccelerationSubmitPlan BuildSmokeAccelerationSubmitPlan(
    const RtSmokeAccelerationSubmitPlanInput& input);

RtSmokeStaticTlasActiveSetPlan BuildSmokeStaticTlasActiveSetPlan(
    const RtSmokeStaticTlasActiveSetPlanDesc& desc);

bool BuildSmokeStaticTlasBucketObservation(
    const RtSmokeStaticTlasBucketObservationInput& input,
    RtSmokeStaticTlasBucketObservation& observation);

RtSmokeStaticBucketBlasPlan BuildSmokeStaticBucketBlasPlan(
    const RtSmokeStaticBucketBlasPlanDesc& desc);

RtSmokeStaticBucketTraversalCompatibility BuildSmokeStaticBucketTraversalCompatibility(
    const RtSmokeStaticBucketTraversalCompatibilityInput& input);

RtSmokeRouteInstanceNamespacePlan BuildSmokeRouteInstanceNamespacePlan(
    const RtSmokeRouteInstanceNamespacePlanInput& input);

RtSmokeStaticRouteTablePlan BuildSmokeStaticRouteTablePlan(
    const RtSmokeStaticRouteTablePlanInput& input);

RtSmokeStaticBucketBlasBuildPlan BuildSmokeStaticBucketBlasBuildPlan(
    const RtSmokeStaticBucketBlasBuildPlanInput& input);

RtSmokeStaticBucketBlasBuildBatchPlan BuildSmokeStaticBucketBlasBuildBatchPlan(
    const RtSmokeStaticBucketBlasBuildBatchPlanInput& input);

RtSmokeStaticBvhBucketSignature BuildSmokeStaticBvhBucketSignature(
    const RtSmokeStaticBvhBucketSignatureInput& input);

RtSmokeStaticBucketBlasBuildObservationPlan BuildSmokeStaticBucketBlasBuildObservationPlan(
    const RtSmokeStaticBucketBlasBuildObservationPlanInput& input);

RtSmokeStaticBucketWorkPlan BuildSmokeStaticBucketWorkPlan(
    const RtSmokeStaticBucketWorkPlanInput& input);

RtSmokeStaticBucketWorkPlanSnapshot CaptureSmokeStaticBucketWorkPlanSnapshot(
    const RtSmokeStaticBucketWorkPlanInput& input);

uint64_t BuildSmokeStaticBucketWorkPlanInputToken(
    const RtSmokeStaticBucketWorkPlanInput& input);

uint64_t BuildSmokeStaticBucketWorkPlanInputToken(
    const RtSmokeStaticBucketWorkPlanSnapshot& snapshot);

RtSmokeStaticBucketWorkPlan BuildSmokeStaticBucketWorkPlan(
    const RtSmokeStaticBucketWorkPlanSnapshot& snapshot);

RtSmokeStaticBucketWorkPlanTimedResult BuildSmokeStaticBucketWorkPlanTimedResult(
    const RtSmokeStaticBucketWorkPlanSnapshot& snapshot);

RtSmokeBvhDirtyTokenState BuildSmokeStaticBucketWorkDirtyToken(
    const RtSmokeStaticBucketWorkDirtyTokenInput& input);

RtSmokeBvhDirtyPlan BuildSmokeBvhDirtyPlan(
    const RtSmokeBvhDirtyPlanInput& input);

RtSmokeBvhFrameToken BuildSmokeBvhFrameToken(
    const RtSmokeBvhFrameTokenInput& input);

RtSmokeBvhFramePlanningSnapshot CaptureSmokeBvhFramePlanningSnapshot(
    const RtSmokeBvhFramePlanningInput& input);

uint64_t BuildSmokeBvhFramePlanningInputToken(
    const RtSmokeBvhFramePlanningInput& input);

uint64_t BuildSmokeBvhFramePlanningInputToken(
    const RtSmokeBvhFramePlanningSnapshot& snapshot);

RtSmokeBvhFramePlanningResult BuildSmokeBvhFramePlanningResult(
    const RtSmokeBvhFramePlanningInput& input);

RtSmokeBvhFramePlanningResult BuildSmokeBvhFramePlanningResult(
    const RtSmokeBvhFramePlanningSnapshot& snapshot);

RtSmokeBvhFramePlanningTimedResult BuildSmokeBvhFramePlanningTimedResult(
    const RtSmokeBvhFramePlanningSnapshot& snapshot);

bool AppendSmokeRigidTlasPlanObservation(
    RtSmokeRigidTlasPlan& plan,
    const RtSmokeRigidTlasPlanDesc& desc,
    const RtSmokeRigidTlasObservation& observation);

RtSmokeRigidTlasPlanSnapshot CaptureSmokeRigidTlasPlanSnapshot(
    const RtSmokeRigidTlasPlanDesc& desc);

uint64_t BuildSmokeRigidTlasPlanInputToken(
    const RtSmokeRigidTlasPlanDesc& desc);

uint64_t BuildSmokeRigidTlasPlanInputToken(
    const RtSmokeRigidTlasPlanSnapshot& snapshot);

void UpdateSmokeRigidTlasPlanInstanceSignature(
    RtSmokeRigidTlasPlan& plan,
    const RtSmokeRigidTlasPlanSnapshot& snapshot);

RtSmokeRigidTlasPlan BuildSmokeRigidTlasPlan(const RtSmokeRigidTlasPlanDesc& desc);

RtSmokeRigidTlasPlan BuildSmokeRigidTlasPlan(
    const RtSmokeRigidTlasPlanSnapshot& snapshot);

RtSmokeCanonicalRigidTlasSelection
BuildSmokeCanonicalRigidTlasSelection(
    const RtSmokeCanonicalRigidTlasSelectionInput& input);

RtSmokeRigidTlasPlanTimedResult BuildSmokeRigidTlasPlanTimedResult(
    const RtSmokeRigidTlasPlanSnapshot& snapshot);

RtSmokeRigidBlasBuildPlan BuildSmokeRigidBlasBuildPlan(
    const RtSmokeRigidBlasBuildPlanInput& input);

RtSmokeRigidBvhObjectSignature BuildSmokeRigidBvhObjectSignature(
    const RtSmokeRigidBvhObjectSignatureInput& input);

RtSmokeUploadPlanMetadata BuildSmokeVectorUploadPlanMetadata(
    size_t elementCount,
    size_t elementSize,
    bool skip,
    int dirtyElementOffset,
    int dirtyElementCount);

RtSmokeStaticDirtyUploadPlan BuildSmokeStaticDirtyUploadPlan(
    const RtSmokeStaticDirtyUploadPlanInput& input);

RtSmokeStaticVertexUploadPlan BuildSmokeStaticVertexUploadPlan(
    const RtSmokeStaticVertexUploadPlanInput& input);

uint64_t BuildSmokePlanDataSpanSignature(
    const RtSmokePlanDataSpan* spans,
    int spanCount);

RtSmokePreviousStaticSnapshotUploadPlan BuildSmokePreviousStaticSnapshotUploadPlan(
    const RtSmokePreviousStaticSnapshotUploadPlanInput& input);
