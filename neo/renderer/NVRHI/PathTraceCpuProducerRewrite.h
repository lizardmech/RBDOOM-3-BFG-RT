#pragma once

#include <cstdint>
#include <memory>
struct PathTraceDoomAnalyticLightSnapshotData;
#if defined(RT_PT_CPU_PRODUCER_REWRITE_HARNESS)
typedef std::uint32_t uint32_t;
typedef std::uint64_t uint64_t;
#endif

#include "PathTraceCanonicalGeometryIdentity.h"
#include "PathTraceGeometryLifecycle.h"
#include "PathTraceSkinnedHitRoute.h"
#if defined(RT_PT_CPU_PRODUCER_REWRITE_HARNESS)
struct PathTraceSmokeVertex
{
	float position[4];
	float normal[4];
	float texCoord[4];
	float color[4];
	float color2[4];
	float tangent[4];
	float bitangent[4];
};
static_assert(sizeof(PathTraceSmokeVertex) == 112, "PathTraceSmokeVertex HLSL ABI mismatch");
struct PathTraceRigidRouteInstance
{
	std::uint32_t vertexOffset = 0;
	std::uint32_t indexOffset = 0;
	std::uint32_t triangleOffset = 0;
	std::uint32_t materialId = 0;
	std::uint32_t materialIndex = 0;
	std::uint32_t vertexCount = 0;
	std::uint32_t indexCount = 0;
	std::uint32_t triangleCount = 0;
	std::uint32_t flags = 0;
	std::uint32_t instanceIdLo = 0;
	std::uint32_t instanceIdHi = 0;
	std::uint32_t padding0 = 0;
	float currentObjectToWorld[12];
	float previousObjectToWorld[12];
};
static_assert(sizeof(PathTraceRigidRouteInstance) == 144, "PathTraceRigidRouteInstance HLSL ABI mismatch");
enum PathTraceRigidRouteInstanceFlags : std::uint32_t
{
	PT_RIGID_ROUTE_HAS_PREVIOUS_TRANSFORM = 1u << 0,
	PT_RIGID_ROUTE_TRANSFORM_CONTINUOUS = 1u << 1,
	PT_RIGID_ROUTE_CACHED_SOURCE = 1u << 2
};
struct PathTraceSkinnedJointMatrix
{
	float rows[12];
};
struct PathTraceSkinnedSourceVertex
{
	float localPosition[4];
	float localNormal[4];
	float localTangent[4];
	float texCoord[4];
	float color[4];
	std::uint32_t jointIndices[4];
	float jointWeights[4];
};
struct PathTraceSkinnedPreviousPosition
{
    float previousPosition[4];
};

enum PathTraceSkinnedSurfaceDispatchFlags : uint32_t
{
    PT_SKINNED_DISPATCH_HAS_VALID_PREVIOUS = 1u << 0,
    PT_SKINNED_DISPATCH_RT_CPU_SKINNED = 1u << 1,
    PT_SKINNED_DISPATCH_SOURCE_READY = 1u << 2,
    PT_SKINNED_DISPATCH_HAS_CURRENT_JOINTS = 1u << 3,
    PT_SKINNED_DISPATCH_HAS_PREVIOUS_JOINTS = 1u << 4,
    PT_SKINNED_DISPATCH_HAS_TEX_MATRIX = 1u << 5
};

struct PathTraceSkinnedSurfaceDispatchRecord
{
    uint32_t sourceVertexOffset = 0;
    uint32_t outputVertexOffset = 0;
    uint32_t previousPositionOffset = 0;
    uint32_t vertexCount = 0;
    uint32_t currentJointOffset = 0;
    uint32_t previousJointOffset = 0;
    uint32_t surfaceRecordIndex = 0;
    uint32_t flags = 0;
    uint32_t dynamicVertexOffset = 0;
    uint32_t dynamicIndexOffset = 0;
    uint32_t dynamicTriangleOffset = 0;
    uint32_t triangleCount = 0;
    float texMatrix0[4];
    float texMatrix1[4];
    float currentObjectToWorld[12];
    float previousObjectToWorld[12];
};
static_assert(
    sizeof(PathTraceSkinnedSurfaceDispatchRecord) == 176,
    "PathTraceSkinnedSurfaceDispatchRecord HLSL ABI mismatch");

#else
#include "PathTraceGeometry.h"
#endif

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <functional>
#include <vector>
#include "PathTraceMaterialFrameKernel.h"

struct RtCpuRewriteMaterialBinding
{
	std::uint32_t materialId = 0;
	std::uint32_t materialIndex = 0;
};
bool RtCpuRewriteBuildMaterialBindings(const std::vector<std::uint32_t>& ids,
	std::vector<RtCpuRewriteMaterialBinding>& bindings);
bool RtCpuRewriteResolveMaterial(const std::vector<RtCpuRewriteMaterialBinding>& bindings,
	std::uint32_t id, std::uint32_t& index);

struct viewDef_t;
struct viewEntity_t;

#define RT_CPU_REWRITE_RETAINED_SKINNED 1
struct RtCpuRewriteSkinnedLayoutRow
{
	// Explicit canonical fields/counts/offsets: no key-struct padding is compared.
	std::uint64_t words[32] = {};
};
struct RtCpuRewriteSkinnedLayout
{
	std::vector<RtCpuRewriteSkinnedLayoutRow> rows;
	std::vector<PathTraceSkinnedSourceVertex> vertices;
	std::vector<std::uint32_t> indexes;
	std::vector<std::uint32_t> blasIndexes;
	std::vector<std::uint64_t> capacities;
};
bool RtCpuRewritePlanSkinnedSlot(std::uint64_t commitSerial, std::uint64_t slotSerial,
	int currentSlot, int selectedSlot);
bool RtCpuRewriteValidateSkinnedCapacity(std::uint64_t resident, std::uint64_t candidate,
	std::uint64_t retired, std::uint64_t limit, std::uint64_t* peak);
bool RtCpuRewriteSkinnedLayoutEqual(const RtCpuRewriteSkinnedLayout& a,
	const RtCpuRewriteSkinnedLayout& b);
bool RtCpuRewriteSkinnedHistoryIdentityEqual(const RtCpuRewriteSkinnedLayoutRow& a,
    const RtCpuRewriteSkinnedLayoutRow& b);
// Allocation reuse is weaker than content equality: changed offsets/materials
// still require complete uploads and BUILD, never a topology-changing UPDATE.
bool RtCpuRewritePlanSkinnedAllocationReuse(const RtCpuRewriteSkinnedLayout& previous,
    const RtCpuRewriteSkinnedLayout& current, const std::vector<std::uint64_t>& vertexBounds,
    std::uint64_t currentVertexCount, std::vector<std::uint32_t>& previousMeshes);
std::uint64_t RtCpuRewriteSkinnedAllocationVertexCapacity(std::uint64_t vertexCount);
bool RtCpuRewriteRetirementReleaseBudgetAllows(std::uint64_t released,
    std::uint64_t elapsedUs, bool forceSchedule);
enum class RtCpuRewriteSkinnedLayoutMismatch : std::uint32_t
{
	Equal, RowCount, Capacity, RowField, SourceVertexCount, SourceVertex,
	SourceIndexCount, SourceIndex, BlasIndexCount, BlasIndex
};
struct RtCpuRewriteSkinnedLayoutComparison
{
	RtCpuRewriteSkinnedLayoutMismatch reason = RtCpuRewriteSkinnedLayoutMismatch::Equal;
	std::uint32_t row = UINT32_MAX;
	std::uint32_t word = UINT32_MAX;
};
RtCpuRewriteSkinnedLayoutComparison RtCpuRewriteCompareSkinnedLayout(
	const RtCpuRewriteSkinnedLayout& a, const RtCpuRewriteSkinnedLayout& b);
bool RtCpuRewriteSkinnedPreviousPoseValid(std::uint64_t previousFrame, std::uint64_t frame,
	std::uint64_t previousEpoch, std::uint64_t epoch, bool identityMatches);
bool RtCpuRewriteCommittedPoseValid(std::uint64_t previousFrame, std::uint64_t frame,
    std::uint64_t previousEpoch, std::uint64_t epoch, bool identityMatches, std::uint64_t committedRoot);
std::uint64_t RtCpuRewriteArenaGrowth(std::uint64_t current, std::uint64_t required, std::uint64_t limit);
bool RtCpuRewriteSceneInstanceCountFits(std::uint64_t rigid, std::uint64_t skinned);
bool RtCpuRewritePackedCountsFit(std::uint64_t vertices, std::uint64_t indexes,
    std::uint64_t triangles, std::uint64_t instances);

enum class RtCpuProducerRewriteRoute : std::uint32_t
{
	LegacyOnly = 0,
	RewriteWarmup = 1,
	RewriteOnly = 2,
	DrainingToLegacy = 3
};

enum class RtCpuRewriteInputSlotState : std::uint32_t
{
	Free = 0,
	Capturing,
	Sealed,
	WorkerReading
};

enum class RtCpuRewriteProductSlotState : std::uint32_t
{
	Free = 0,
	Building,
	Ready,
	Consuming
};

enum class RtCpuRewriteInvalidReason : std::uint32_t
{
	None = 0,
	LifecycleReset,
	MapWorldChange,
	VidRestart,
	BeginLevelLoad,
	Shutdown,
	ParallelAddModelsDisabled,
	Malformed,
	Capacity,
	Cancel
};

struct RtCpuRewriteOwnedVertex
{
	float xyz[3];
	float normal[3];
	float tangent[3];
	float bitangent[3];
	float bitangentSign;
	float st[2];
	float color[4];
	float color2[4];
};

struct RtCpuRewriteRetainedGeometry
{
    std::vector<RtCpuRewriteOwnedVertex> vertices;
    std::vector<uint32_t> indexes;
    uint64_t topology = 0, content = 0, charged = 0;
    std::shared_ptr<std::atomic<uint64_t>> budget;
    ~RtCpuRewriteRetainedGeometry() { if (budget) budget->fetch_sub(charged); }
};

struct RtCpuRewriteSurfaceWrite
{
    std::shared_ptr<const RtCpuRewriteRetainedGeometry> geometry;
	PtCanonicalMeshKey meshKey;
	PtCanonicalInstanceKey instanceKey;
	std::uint32_t sourceClass = 0;
	float modelMatrix[16];
	float bounds[6];
	std::uint32_t materialLogicalId = 0;
	std::uint32_t activeEmissiveStage = 0;
	std::uint32_t surfaceOrdinal = 0;
	const RtCpuRewriteOwnedVertex* vertices = nullptr;
	std::uint32_t vertexCount = 0;
	const std::uint32_t* indexes = nullptr;
	std::uint32_t indexCount = 0;
	const void* nativeDrawVerts = nullptr;
	const void* nativeIndexes = nullptr;
	std::uint32_t nativeIndexStride = 0;
	const PathTraceSkinnedJointMatrix* joints = nullptr;
	std::uint32_t jointCount = 0;
};

enum class RtCpuRewriteCaptureAdmissionReason : std::uint32_t
{
	Admitted = 0,
	Callback,
	ForceUpdate,
	WeaponDepthHack,
	ModelDepthHack,
	UnsupportedSource,
	GpuSkinningDisabled,
	CpuPosedSurface,
	MissingJointSnapshot,
	UnresolvedCurrentSurface,
	Count
};

static constexpr std::uint32_t kRtCpuRewriteCaptureSourceDomainCount =
	static_cast<std::uint32_t>(PtCanonicalMeshSourceDomain::UnsupportedTransient) + 1u;
static constexpr std::uint32_t kRtCpuRewriteCaptureAdmissionReasonCount =
	static_cast<std::uint32_t>(RtCpuRewriteCaptureAdmissionReason::Count);
static_assert(kRtCpuRewriteCaptureAdmissionReasonCount != 0 &&
	kRtCpuRewriteCaptureSourceDomainCount <= UINT32_MAX / kRtCpuRewriteCaptureAdmissionReasonCount,
	"capture telemetry matrix dimensions must have a checked fixed-size product");
static constexpr std::uint32_t kRtCpuRewriteCaptureEntityRejectCellCount =
	kRtCpuRewriteCaptureSourceDomainCount * kRtCpuRewriteCaptureAdmissionReasonCount;

struct RtCpuRewriteCaptureAdmissionFacts
{
	PtCanonicalMeshSourceDomain sourceDomain = PtCanonicalMeshSourceDomain::Invalid;
	bool callback = false;
	bool forceUpdate = false;
	bool weaponDepthHack = false;
	bool modelDepthHack = false;
	bool rootView = false;
	int allowSurfaceInViewId = 0;
	int activeViewId = 0;
	bool resolvedSurfaceCurrent = false;
	bool gpuSkinningEnabled = false;
	bool bindPoseSurface = false;
	bool jointSnapshotValid = false;
};

RtCpuRewriteCaptureAdmissionReason RtCpuRewritePlanCaptureAdmission(
	const RtCpuRewriteCaptureAdmissionFacts& facts);

struct RtCpuRewriteProductSource
{
	PtCanonicalMeshKey meshKey;
	PtCanonicalInstanceKey instanceKey;
	std::uint32_t vertexBegin = 0;
	std::uint32_t vertexCount = 0;
	std::uint32_t indexBegin = 0;
	std::uint32_t indexCount = 0;
	std::uint32_t triangleBegin = 0;
	std::uint32_t triangleCount = 0;
	std::uint32_t surfaceOrdinal = 0;
	std::uint32_t sourceClass = 0;
	std::uint32_t rigidMeshIndex = UINT32_MAX;
	std::uint32_t skinnedMeshIndex = UINT32_MAX;
};
static_assert(sizeof(RtCpuRewriteProductSource) <= 128, "output range/key row cap");

struct RtCpuRewriteRigidMeshView
{
    std::uint64_t sourceContentSignature = 0;
	PtCanonicalMeshKey meshKey;
	std::uint64_t signature = 0;
	std::uint32_t vertexCount = 0;
	std::uint32_t indexCount = 0;
	std::uint32_t triangleCount = 0;
	std::uint32_t sourceClass = 0;
	std::uint32_t materialLogicalId = 0;
	const PathTraceSmokeVertex* vertices = nullptr;
	const std::uint32_t* indexes = nullptr;
};

struct RtCpuRewriteSkinnedMeshView
{
	std::uint64_t sourceContentSignature = 0;
	PtCanonicalMeshKey meshKey;
	std::uint64_t signature = 0;
	std::uint32_t vertexCount = 0;
	std::uint32_t indexCount = 0;
	std::uint32_t triangleCount = 0;
	std::uint32_t sourceClass = 0;
	std::uint32_t materialLogicalId = 0;
	const PathTraceSkinnedSourceVertex* vertices = nullptr;
	const std::uint32_t* indexes = nullptr;
};

struct RtCpuRewriteOverlayRow
{
	std::uint64_t sourceContentSignature = 0;
	PtCanonicalInstanceKey instanceKey;
	PtCanonicalMeshKey meshKey;
	float currentObjectToWorld[16];
	float previousObjectToWorld[16];
	std::uint32_t flags = 0;
	std::uint32_t materialLogicalId = 0;
	std::uint32_t surfaceOrdinal = 0;
	std::uint32_t sourceClass = 0;
	std::uint32_t jointOffset = 0;
	std::uint32_t jointCount = 0;
};

struct RtCpuRewriteOverlayView
{
    std::uint64_t staticRevision = 0;
	std::shared_ptr<const PathTraceDoomAnalyticLightSnapshotData> analyticLights;
	std::uint64_t rootFrame = 0;
	std::uint64_t lifecycleGeneration = 0;
	std::uint32_t rowCount = 0;
	const RtCpuRewriteOverlayRow* rows = nullptr;
	std::uint32_t jointCount = 0;
	const PathTraceSkinnedJointMatrix* joints = nullptr;
};

enum : std::uint32_t
{
	kRtCpuRewriteJoinOk = 0,
	kRtCpuRewriteJoinRouteCountIncomplete = 1,
	kRtCpuRewriteJoinInstanceIdUnresolved = 2,
	kRtCpuRewriteJoinMaskZero = 3,
	kRtCpuRewriteJoinRetainCap = 4
};

enum : std::uint32_t
{
	kRtCpuRewriteTlasBaseInstances = 2u,
	kRtCpuRewriteTlasWalkBudget = 510u,
	kRtCpuRewriteTlasRegistryBudget = 2048u,
	kRtCpuRewriteTlasStaticBucketBudget = 1024u,
	kRtCpuRewriteTlasMaxInstances =
        65536u, // Resident scenes are not limited by the old portal-walk budgets.
	kRtCpuRewriteMaxExtras = kRtCpuRewriteTlasMaxInstances - kRtCpuRewriteTlasBaseInstances
};

constexpr std::uint64_t kRtCpuRewriteOneRetiringGenerationCount =
	static_cast<std::uint64_t>(kRtCpuRewriteTlasMaxInstances) + 1ull;
constexpr std::uint64_t kRtCpuRewriteDefaultRetireFrames = 6ull;
constexpr std::uint64_t kRtCpuRewriteRetiredBoundGenerations =
	1ull + (kRtCpuRewriteDefaultRetireFrames > 3ull ? kRtCpuRewriteDefaultRetireFrames : 3ull);
constexpr std::uint64_t kRtCpuRewriteProductBytes = 256ull * 1024ull * 1024ull;
constexpr std::uint64_t kRtCpuRewriteGpuRigidRetainCapBytes = 256ull * 1024ull * 1024ull;
constexpr std::uint64_t kRtCpuRewriteGpuSkinnedRetainCapBytes = 512ull * 1024ull * 1024ull;
constexpr std::uint64_t kRtCpuRewriteOneRetiringGenerationLogicalBytes =
	kRtCpuRewriteGpuRigidRetainCapBytes + kRtCpuRewriteGpuSkinnedRetainCapBytes +
	kRtCpuRewriteProductBytes;
constexpr std::uint64_t kRtCpuRewriteRetiredSoftCount =
	kRtCpuRewriteRetiredBoundGenerations * kRtCpuRewriteOneRetiringGenerationCount;
constexpr std::uint64_t kRtCpuRewriteRetiredSoftLogicalBytes =
	kRtCpuRewriteRetiredBoundGenerations * kRtCpuRewriteOneRetiringGenerationLogicalBytes;
static_assert(kRtCpuRewriteOneRetiringGenerationCount == 65537ull, "resident generation count changed");
static_assert(kRtCpuRewriteTlasMaxInstances <= 0x00ffffffu, "resident IDs must fit shader InstanceID");
static_assert(kRtCpuRewriteDefaultRetireFrames == 6ull, "reviewed F2 retire-frame default changed");
static_assert(kRtCpuRewriteRetiredSoftCount == 458759ull, "resident retirement count bound changed");
static_assert(kRtCpuRewriteRetiredSoftLogicalBytes == 7168ull * 1024ull * 1024ull,
	"R4-016 full-level logical-byte bound changed");

enum class RtCpuRewriteRetirementDecision : std::uint32_t
{
	Admit = 0,
	CountPressure,
	BytePressure,
	UnknownBytes,
	ArithmeticOverflow
};

struct RtCpuRewriteRetirementAdmissionInput
{
	std::uint64_t currentLiveCount = 0;
	std::uint64_t currentLiveLogicalBytes = 0;
	std::uint64_t proposedCount = 0;
	std::uint64_t proposedLogicalBytes = 0;
	bool proposedBytesKnown = true;
	std::uint64_t countBound = kRtCpuRewriteRetiredSoftCount;
	std::uint64_t logicalByteBound = kRtCpuRewriteRetiredSoftLogicalBytes;
};

struct RtCpuRewriteRetirementLedger
{
	std::uint64_t liveCount = 0;
	std::uint64_t liveLogicalBytes = 0;
	std::uint64_t unscheduledCount = 0;
	std::uint64_t unscheduledLogicalBytes = 0;
	std::uint64_t liveCountHighWater = 0;
	std::uint64_t liveLogicalBytesHighWater = 0;
	std::uint64_t unscheduledCountHighWater = 0;
	std::uint64_t unscheduledLogicalBytesHighWater = 0;
	std::uint64_t enqueuedTotal = 0;
	std::uint64_t scheduledTotal = 0;
	std::uint64_t releasedTotal = 0;
	std::uint64_t countPressureRejects = 0;
	std::uint64_t bytePressureRejects = 0;
	std::uint64_t unknownByteRejects = 0;
	std::uint64_t arithmeticOverflowRejects = 0;
	std::uint64_t metadataReserveFailures = 0;
	std::uint64_t evictionDeferred = 0;
	std::uint64_t forcedDrainOverBound = 0;
	std::uint64_t reconciliationAnomalies = 0;
	std::uint64_t skinnedLiveCount = 0;
	std::uint64_t skinnedLiveLogicalBytes = 0;
};

RtCpuRewriteRetirementDecision RtCpuRewritePlanRetirementAdmission(
	const RtCpuRewriteRetirementAdmissionInput& input);

struct RtCpuRewriteRetirementMemberApplyInput
{
	RtCpuRewriteRetirementDecision decision = RtCpuRewriteRetirementDecision::UnknownBytes;
	bool enqueueSucceeded = false;
};
struct RtCpuRewriteRetirementMemberApplyPlan
{
	bool preservePrevious = true;
	bool applyCandidate = false;
};
RtCpuRewriteRetirementMemberApplyPlan RtCpuRewritePlanRetirementMemberApply(
	const RtCpuRewriteRetirementMemberApplyInput& input);

struct RtCpuRewriteRetirementSite4Input
{
	bool hasBatch = false;
	bool localReserveSucceeded = false;
	bool preflightSucceeded = false;
};
struct RtCpuRewriteRetirementSite4Plan
{
	bool allowCommit = true;
	bool evictionDeferred = false;
	bool enqueueAndCompact = false;
};
RtCpuRewriteRetirementSite4Plan RtCpuRewritePlanRetirementSite4(
	const RtCpuRewriteRetirementSite4Input& input);

void RtCpuRewriteRetirementLedgerEnqueue(RtCpuRewriteRetirementLedger& ledger,
	std::uint64_t count, std::uint64_t logicalBytes,
	std::uint64_t skinnedCount = 0, std::uint64_t skinnedLogicalBytes = 0);
void RtCpuRewriteRetirementLedgerSchedule(RtCpuRewriteRetirementLedger& ledger,
	std::uint64_t count, std::uint64_t logicalBytes);
void RtCpuRewriteRetirementLedgerRelease(RtCpuRewriteRetirementLedger& ledger,
	std::uint64_t count, std::uint64_t logicalBytes,
	std::uint64_t skinnedCount = 0, std::uint64_t skinnedLogicalBytes = 0);
void RtCpuRewriteRetirementLedgerReject(RtCpuRewriteRetirementLedger& ledger,
	RtCpuRewriteRetirementDecision decision);
void RtCpuRewriteRetirementLedgerEvictionDeferred(RtCpuRewriteRetirementLedger& ledger);
void RtCpuRewriteRetirementLedgerForcedDrain(RtCpuRewriteRetirementLedger& ledger, bool overBound);
bool RtCpuRewriteRetirementLedgerReconcile(RtCpuRewriteRetirementLedger& ledger,
	std::uint64_t actualLiveCount, std::uint64_t actualLiveLogicalBytes,
	std::uint64_t actualSkinnedCount, std::uint64_t actualSkinnedLogicalBytes);

struct RtCpuRewriteRetirementDrainInventory
{
	std::uint64_t skinnedCount = 0;
	std::uint64_t staticCount = 0;
	std::uint64_t dedicatedCount = 0;
};
struct RtCpuRewriteRetirementDrainPlan
{
	std::uint64_t forceAdmitSkinnedCount = 0;
	std::uint64_t retainStaticCount = 0;
	std::uint64_t retainDedicatedCount = 0;
};
RtCpuRewriteRetirementDrainPlan RtCpuRewritePlanRetirementDrain(
	const RtCpuRewriteRetirementDrainInventory& inventory);

struct RtCpuRewriteTextureMatrices
{
    float primary[6] = { 1, 0, 0, 0, 1, 0 };
    float normal[6] = { 1, 0, 0, 0, 1, 0 };
};

inline void RtCpuRewriteApplyTextureMatrix(float* uv, const float* matrix)
{
    const float u = uv[0], v = uv[1];
    uv[0] = matrix[0] * u + matrix[1] * v + matrix[2];
    uv[1] = matrix[3] * u + matrix[4] * v + matrix[5];
}

inline uint64_t RtCpuRewriteTextureMatrixSignature(const float* matrix, size_t count)
{
    uint64_t hash = 1469598103934665603ull;
    const auto* bytes = reinterpret_cast<const unsigned char*>(matrix);
    for (size_t i = 0; i < count * sizeof(float); ++i)
        hash = (hash ^ bytes[i]) * 1099511628211ull;
    return hash;
}

// Exact-frame shading patches into an unchanged resident rigid layout.
struct RtCpuRewriteRigidAttributePatch
{
    uint32_t vertexOffset = 0, triangleOffset = 0;
    std::vector<PathTraceSmokeVertex> vertices;
    std::vector<uint32_t> materialIds, materialIndexes;
};
bool RtCpuRewriteBuildRigidAttributePatch(const RtCpuRewriteRigidMeshView& mesh,
    const PathTraceRigidRouteInstance& route, const RtCpuRewriteTextureMatrices& matrices,
    uint32_t packedVertices, uint32_t packedTriangles, bool uvChanged, bool materialChanged,
    RtCpuRewriteRigidAttributePatch& out);

struct RtCpuRewriteJoinedInstance
{
    RtCpuRewriteTextureMatrices textureMatrices;
	PtCanonicalInstanceKey instanceKey;
	std::uint32_t routeRecordIndex = 0;
	std::uint32_t meshIndex = 0;
	std::uint32_t instanceId = 0;
	std::uint32_t instanceMask = 0x02;
	float currentObjectToWorld[16];
	PtCanonicalMeshKey meshKey;
	PathTraceRigidRouteInstance route{};
};

// Sparse light sampling only: evaluate an admitted emissive corner from the same
// owned source/joint bytes used by the GPU. Never used to rebuild geometry.
bool RtCpuRewriteEvaluateEmissiveVertex(const PathTraceSkinnedSourceVertex& source,
    const PathTraceSkinnedJointMatrix* joints, uint32_t jointCount,
    const float objectToWorld[16], const float textureMatrix[6], PathTraceSmokeVertex& out);

struct RtCpuRewriteJoinedSkinned
{
	std::uint64_t sourceContentSignature = 0;
	bool hasTextureMatrix = false;
	float textureMatrix[6] = { 1, 0, 0, 0, 1, 0 };
    float normalTextureMatrix[6] = { 1, 0, 0, 0, 1, 0 };
	std::uint32_t materialLogicalId = 0;
	std::uint32_t materialIndex = 0;
	std::uint32_t meshIndex = 0;
	std::uint32_t instanceId = 0;
	std::uint32_t instanceMask = 0x02;
	std::uint32_t jointOffset = 0;
	std::uint32_t jointCount = 0;
	float currentObjectToWorld[16];
	float previousObjectToWorld[16];
	PtCanonicalMeshKey meshKey;
	PtCanonicalInstanceKey instanceKey;
};

struct RtCpuRewriteJoinResult
{
	std::uint32_t failReason = kRtCpuRewriteJoinOk;
	std::uint32_t rigidRouteVertexCount = 0;
	std::uint32_t rigidRouteIndexCount = 0;
	std::uint32_t rigidRouteTriangleCount = 0;
	std::uint32_t rigidRouteInstanceCount = 0;
	std::uint32_t joinedCount = 0;
	std::uint32_t instanceJoinHit = 0;
	std::uint32_t instanceJoinMiss = 0;
	std::uint32_t omittedGeometryWithoutOverlay = 0;
	std::uint64_t packedBytes = 0;
	std::vector<RtCpuRewriteJoinedInstance> joined;
	std::uint32_t skinnedCount = 0;
	std::vector<RtCpuRewriteJoinedSkinned> skinned;
};

struct RtCpuRewriteLastJoined
{
	PtCanonicalInstanceKey instanceKey;
	PtCanonicalMeshKey meshKey;
	PathTraceRigidRouteInstance route{};
};

struct RtCpuRewriteGpuRetainEntry
{
	PtCanonicalMeshKey meshKey;
	std::uint64_t worldGeneration = 0;
	std::uint64_t signature = 0;
	std::uint64_t bytes = 0;
	bool referenced = false;
};

void RtCpuRewriteBuildAffineFromObjectToWorld(const float objectToWorld[16], float affine3x4[12]);
bool RtCpuRewriteMatrixIsIdentity(const float matrix[16]);
bool RtCpuRewriteValidatePackedRouteCommit(
	std::uint32_t vertexCount,
	std::uint32_t indexCount,
	std::uint32_t triangleCount,
	std::uint32_t instanceCount,
	std::uint32_t extraCount,
	const std::uint32_t* extraMasks,
	std::uint32_t* failReason,
	std::uint32_t rigidExtraCount = 0xFFFFFFFFu);
std::uint32_t RtCpuRewriteSkinnedFirstInstanceId(std::uint32_t rigidExtraCount);
bool RtCpuRewriteSkinnedIdsOverlapRigid(std::uint32_t firstSkinned, std::uint32_t skinnedCount, std::uint32_t rigidExtraCount);
std::uint32_t RtCpuRewriteSkinnedCombinedPreviousJointOffset(std::uint32_t currentTotal, std::uint32_t previousLocal);
std::uint64_t RtCpuRewriteEvictUnreferencedRetain(RtCpuRewriteGpuRetainEntry* entries, std::uint32_t count, std::uint32_t* kept);
bool RtCpuRewriteFitsGpuRetainCap(std::uint64_t currentBytes, std::uint64_t addBytes, std::uint64_t cap);

struct RtCpuRewriteSkinnedRouteInput
{
	PtCanonicalMeshKey meshKey;
	PtCanonicalInstanceKey instanceKey;
	const std::uint32_t* sourceIndexes = nullptr;
	std::uint64_t sourceIndexCount = 0;
	std::uint64_t sourceIndexOffsetBytes = 0;
	std::uint64_t sourceIndexCapacityBytes = 0;
	std::uint64_t outputVertexOffsetBytes = 0;
	std::uint64_t outputVertexCount = 0;
	std::uint64_t outputCapacityBytes = 0;
	std::uint64_t previousPositionOffset = 0;
	std::uint64_t previousPositionCount = 0;
	std::uint64_t sourceGpuIndexGeneration = 0;
	std::uint64_t outputStorageGeneration = 0;
	std::uint32_t materialLogicalId = 0;
	std::uint32_t materialIndex = 0;
	bool previousValid = false;
};

struct RtCpuRewriteSkinnedRoutePackage
{
	PtSkinnedHitRouteBuild build;
	PtSkinnedHitRouteGpuUpload upload;
};

bool RtCpuRewriteBuildSkinnedRoutePackage(
	const std::vector<RtCpuRewriteSkinnedRouteInput>& inputs,
	std::uint32_t rigidExtraCount,
	RtCpuRewriteSkinnedRoutePackage& out, bool cpuTemplates = false);

RtCpuRewriteSkinnedLayoutRow RtCpuRewriteMakeSkinnedLayoutRow(
    const RtCpuRewriteJoinedSkinned& js, const RtCpuRewriteSkinnedMeshView& mesh,
    std::uint64_t vertexOffset, std::uint64_t indexOffset, std::uint64_t jointOffset);

struct RtCpuRewriteSkinnedPreparedView
{
    std::uint32_t count = 0, vertexCount = 0, indexCount = 0, jointCount = 0;
    std::uint64_t capacities[8] = {};
    const RtCpuRewriteSkinnedLayoutRow* rows = nullptr;
    const PathTraceSkinnedSourceVertex* vertices = nullptr;
    const std::uint32_t* indexes = nullptr;
    const std::uint32_t* blasIndexes = nullptr;
    const PathTraceSkinnedSurfaceDispatchRecord* dispatches = nullptr;
    const PathTraceSkinnedHitRouteGpuRecord* records = nullptr;
    const PathTraceSkinnedHitRouteGpuTriangle* triangles = nullptr;
};

bool RtCpuRewritePatchSkinnedRouteRecord(PathTraceSkinnedHitRouteGpuRecord& record,
    const PathTraceSkinnedSurfaceDispatchRecord& dispatch, std::uint64_t sourceGeneration,
    std::uint64_t outputGeneration, std::uint32_t instanceId, std::uint32_t previousPositionCount);

enum class RtCpuRewriteSkinnedReplacementEvent : std::uint32_t
{
	LocalFailure = 0,
	ValidatedReplacement,
	LaterSceneFailure,
	Drain
};

enum class RtCpuRewriteSkinnedRejectReason : std::uint32_t
{
	None = 0,
	InvalidDevice,
	MissingOverlay,
	MissingProduct,
	MissingPipeline,
	InstanceIdOverlap,
	JoinRange,
	Capacity,
	Allocation,
	RoutePackage,
	Blas,
	Replacement,
	RetirementCountPressure,
	RetirementBytePressure,
	RetirementUnknownBytes,
	RetirementArithmeticOverflow,
	NoIdleSlot,
	GenerationExhaustion,
	SerialOverflow,
    WorkerFrameMismatch,
    WorkerMembershipMismatch,
    WorkerSourceMismatch
};

struct RtCpuRewriteSkinnedReplacementPlan
{
	bool keepMembers = false;
	bool retireMembers = false;
	bool swapCandidate = false;
	bool retainRetiredAfterSceneFailure = false;
	bool enqueueBeforeForceSchedule = false;
	bool resetSerialAfterForceSchedule = false;
};

RtCpuRewriteSkinnedReplacementPlan RtCpuRewritePlanSkinnedReplacement(
	RtCpuRewriteSkinnedReplacementEvent event);

enum class RtCpuRewriteSkinnedHandleSource : std::uint32_t
{
	Invalid = 0,
	LiveRewrite,
	PersistentZeroRecord,
	PersistentZeroTriangle,
	PersistentSharedTriangleDispatch,
	PersistentSharedEmissive
};

struct RtCpuRewriteSkinnedHandleSelectionInput
{
	bool haveLiveRoute = false;
	bool liveRecord = false;
	bool liveTriangle = false;
	bool livePrevious = false;
	bool liveDispatch = false;
	bool liveSourceIndex = false;
	bool persistentZeroRecord = false;
	bool persistentZeroTriangle = false;
	bool persistentSharedTriangleDispatch = false;
	bool persistentSharedEmissive = false;
	std::uint32_t recordCount = 0;
	std::uint32_t triangleCount = 0;
	std::uint32_t sourceIndexCount = 0;
	std::uint32_t previousPositionCount = 0;
};

struct RtCpuRewriteSkinnedHandleSelection
{
	RtCpuRewriteSkinnedHandleSource record = RtCpuRewriteSkinnedHandleSource::Invalid;
	RtCpuRewriteSkinnedHandleSource triangle = RtCpuRewriteSkinnedHandleSource::Invalid;
	RtCpuRewriteSkinnedHandleSource previous = RtCpuRewriteSkinnedHandleSource::Invalid;
	RtCpuRewriteSkinnedHandleSource dispatch = RtCpuRewriteSkinnedHandleSource::Invalid;
	RtCpuRewriteSkinnedHandleSource sourceIndex = RtCpuRewriteSkinnedHandleSource::Invalid;
	RtCpuRewriteSkinnedHandleSource triangleDispatch = RtCpuRewriteSkinnedHandleSource::Invalid;
	RtCpuRewriteSkinnedHandleSource emissive = RtCpuRewriteSkinnedHandleSource::Invalid;
	std::uint32_t recordCount = 0;
	std::uint32_t triangleCount = 0;
	std::uint32_t sourceIndexCount = 0;
	std::uint32_t previousPositionCount = 0;
	std::uint32_t surfaceDispatchCount = 0;
	std::uint32_t triangleDispatchIndexCount = 0;
	bool valid = false;
	bool usesWarmupGeometry = false;
};

RtCpuRewriteSkinnedHandleSelection RtCpuRewriteSelectSkinnedHandles(
	const RtCpuRewriteSkinnedHandleSelectionInput& input);

enum class RtCpuRewriteKeepLastFamily : std::uint32_t
{
	None = 0,
	LocalSkinned,
	OtherScene
};

enum class RtCpuRewriteKeepLastEvent : std::uint32_t
{
	KeepLast = 0,
	SceneCommit,
	Drain
};

struct RtCpuRewriteKeepLastState
{
	std::uint32_t consecutive = 0;
	RtCpuRewriteKeepLastFamily family = RtCpuRewriteKeepLastFamily::None;
	bool warned = false;
};

struct RtCpuRewriteKeepLastTransition
{
	RtCpuRewriteKeepLastState next;
	bool warnNow = false;
	bool localSkinnedDegradeEligible = false;
};

RtCpuRewriteKeepLastTransition RtCpuRewritePlanKeepLastTransition(
	const RtCpuRewriteKeepLastState& current,
	RtCpuRewriteKeepLastEvent event,
	RtCpuRewriteKeepLastFamily failureFamily = RtCpuRewriteKeepLastFamily::None);

struct RtCpuRewriteFrozenProductView
{
    std::shared_ptr<const struct RtCpuRewriteResidentDynamic> residentDynamic;
    std::shared_ptr<const struct RtCpuRewriteResidentStatic> residentStatic;
    std::uint64_t staticRevision = 0;
    std::uint32_t staticSourceCount = 0;
    const RtCpuRewriteProductSource* staticSources = nullptr;
    std::uint32_t StaticSourceCount() const { return residentStatic ? staticSourceCount : sourceCount; }
    const RtCpuRewriteProductSource* StaticSources() const { return residentStatic ? staticSources : sources; }
	std::uint64_t staticContentSignature = 0;
	std::uint64_t ticket = 0;
	std::uint64_t inputSeal = 0;
	std::uint64_t lifecycleGeneration = 0;
	std::uint64_t worldGeneration = 0;
	std::uint64_t mapGeneration = 0;
	std::uint64_t configGeneration = 0;
	std::uint64_t contentSignature = 0;
	std::uint32_t vertexCount = 0;
	std::uint32_t indexCount = 0;
	std::uint32_t triangleCount = 0;
	std::uint32_t sourceCount = 0;
	const PathTraceSmokeVertex* vertices = nullptr;
	const std::uint32_t* indexes = nullptr;
	const std::uint32_t* triangleClassAndFlags = nullptr;
	const std::uint32_t* triangleMaterialIds = nullptr;
	const std::uint32_t* triangleMaterialIndexes = nullptr;
	const RtCpuRewriteProductSource* sources = nullptr;
	std::uint64_t rootFrame = 0;
	std::uint32_t rigidMeshCount = 0;
	const RtCpuRewriteRigidMeshView* rigidMeshes = nullptr;
	std::uint32_t skinnedMeshCount = 0;
	const RtCpuRewriteSkinnedMeshView* skinnedMeshes = nullptr;
    RtCpuRewriteSkinnedPreparedView skinnedPrepared;
};

std::uint64_t RtCpuRewriteStaticGeometrySignature(const RtCpuRewriteFrozenProductView& view);

bool RtCpuRewriteFitsResidentRigidBudget(std::uint64_t retainedBytes, std::uint64_t packedBytes);

RtCpuRewriteSkinnedRejectReason RtCpuRewriteValidateSkinnedPrepared(
    const RtCpuRewriteFrozenProductView* view, const RtCpuRewriteOverlayView* overlay,
    const RtCpuRewriteJoinResult& join);

enum : std::uint32_t
{
	kRtCpuRewriteClassStaticWorld = 0,
	kRtCpuRewriteClassRigidEntity = 1,
	kRtCpuRewriteClassSkinnedEntity = 2
};

struct RtCpuRewriteCounters
{
	std::uint64_t inputAcquire = 0;
	std::uint64_t inputSeal = 0;
	std::uint64_t inputAbort = 0;
	std::uint64_t inputDrop = 0;
	std::uint64_t inputPressureDrops = 0;
	std::uint64_t invalidMalformed = 0;
	std::uint64_t invalidCapacity = 0;
	std::uint64_t parallelAddModelsDisabled = 0;
	std::uint64_t workerDispatch = 0;
	std::uint64_t workerComplete = 0;
	std::uint64_t workerCancels = 0;
	std::uint64_t workerFail = 0;
    std::uint64_t convertedMeshHits = 0, convertedMeshMisses = 0;
	std::uint64_t productsReady = 0;
	std::uint64_t productAcquire = 0;
	std::uint64_t productCommit = 0;
	std::uint64_t productReuse = 0;
	std::uint64_t productRejectStale = 0;
	std::uint64_t productRejectWorld = 0;
	std::uint64_t productRejectMap = 0;
	std::uint64_t productRejectConfig = 0;
	std::uint64_t productPressureDrops = 0;
	std::uint64_t latch = 0;
	std::uint64_t rollback = 0;
	std::uint64_t drain = 0;
	std::uint64_t threadsJoined = 0;
	std::uint64_t ticketAge = 0;
	std::uint64_t lifecycleGeneration = 0;
	std::uint64_t configGeneration = 0;
	std::uint64_t lastPublishedTicket = 0;
	std::uint64_t lastConsumedTicket = 0;
	std::uint64_t inputSlotBytes[3] = {};
	std::uint64_t inputSlotHighWater[3] = {};
	std::uint64_t productSlotBytes[3] = {};
	std::uint64_t productSlotHighWater[3] = {};
	std::uint64_t inputHighWater = 0;
	std::uint64_t productHighWater = 0;
	std::uint64_t scratchHighWater = 0;
	std::uint64_t serviceHighWater = 0;
	std::uint64_t overlayAcquire = 0;
	std::uint64_t overlaySeal = 0;
	std::uint64_t overlayDrop = 0;
	std::uint64_t overlayReclaimed = 0;
	std::uint64_t inputReclaimed = 0;
	std::uint64_t overlayReuse = 0;
	std::uint64_t overlayFrameMismatch = 0;
	std::uint64_t meshGpuHit = 0;
	std::uint64_t meshGpuMiss = 0;
	std::uint64_t meshGpuRebuild = 0;
	std::uint64_t vertexUploadBytes = 0;
	std::uint64_t skippedUploadFrames = 0;
	std::uint64_t lateGeometryReuse = 0;
	std::uint64_t instanceJoinHit = 0;
	std::uint64_t instanceJoinMiss = 0;
	std::uint64_t instanceIdUnresolved = 0;
	std::uint64_t routeCountIncomplete = 0;
	std::uint64_t gpuRigidRetainBytes = 0;
	std::uint64_t gpuRigidRetainCap = 256ull * 1024ull * 1024ull;
	std::uint64_t commitTransformOnly = 0;
	std::uint64_t commitFull = 0;
	std::uint64_t packedRouteVertexBytes = 0;
	std::uint64_t packedGeometryFill = 0;
	std::uint64_t packedGeometryFillSkipped = 0;
	std::uint64_t captureEntityCandidates[kRtCpuRewriteCaptureSourceDomainCount] = {};
	std::uint64_t captureEntityProvisional[kRtCpuRewriteCaptureSourceDomainCount] = {};
	std::uint64_t captureEntityRejected[kRtCpuRewriteCaptureSourceDomainCount]
		[kRtCpuRewriteCaptureAdmissionReasonCount] = {};
	std::uint64_t skinnedSurfaceCandidates = 0;
	std::uint64_t skinnedSurfaceAdmitted = 0;
	std::uint64_t skinnedSurfaceRejected[kRtCpuRewriteCaptureAdmissionReasonCount] = {};
	std::uint64_t skinnedJointRows = 0;
	std::uint64_t skinnedJointRowsHighWater = 0;
	RtCpuRewriteRetirementLedger retirement;
};

struct RtCpuRewriteCommitTailPredicate
{
	bool rewriteOnly = false;
	bool allDedicatedHits = false;
	bool dedicatedSetUnchanged = false;
	bool staticSignatureUnchangedOrNoStatic = false;
	bool extraFitsIdleSlot = false;
	bool packedCountsUnchanged = false;
};

bool RtCpuRewriteIsTransformOnlyCommit(const RtCpuRewriteCommitTailPredicate& pred);

struct RtCpuRewritePackedRangeWitness
{
    std::uint64_t textureMatrixSignature = 0, sourceContentSignature = 0, blasToken = 0;
    std::uint32_t vertexOffset = 0, indexOffset = 0, triangleOffset = 0;
    std::uint32_t vertexCount = 0, indexCount = 0, triangleCount = 0;
    std::uint32_t materialId = 0, materialIndex = 0;
};
bool RtCpuRewritePackedRangeReusable(const RtCpuRewritePackedRangeWitness& a,
    const RtCpuRewritePackedRangeWitness& b);

struct RtCpuRewritePackedLayoutPredicate
{
	bool allDedicatedHits = false;
	bool dedicatedSetUnchanged = false;
	bool packedCountsUnchanged = false;
	bool packedOffsetIdentity = false;
};
bool RtCpuRewriteShouldSkipPackedRouteGeometryFill(const RtCpuRewritePackedLayoutPredicate& pred);
bool RtCpuRewriteDynamicBlasCacheHit(bool haveStatic, bool signatureMatchesLastCommitted, bool hasDynamicBlas);


// Exact-frame CPU-only material preparation; no engine objects or GPU resources.
struct RtCpuRewriteMaterialJob
{
    RtCpuRewriteMaterialInput input;
    // Separate from immutable input: the owner publishes this once under the
    // service mutex, and only the worker reads it after Ready.
    enum class BindingState { Pending, Ready, Cancelled, Closed };
    RtCpuMaterialBindingInput deferredBindings;
    BindingState bindingState = BindingState::Ready; // service mutex
    bool deferBindings = false; // immutable after submit
    uint64_t inputCharge = 0;
    std::atomic<bool> numericPrepared{false}; // progress only, not output publication
    std::vector<RtPathTraceRuntimeMaterialEvalPod> output;
    RtCpuRewriteMaterialFrame frame;
    uint64_t lifecycle = 0;
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false, success = false;
};

// Closure captures only owned CPU inputs/results; never renderer/GPU objects.
struct RtCpuRewriteLightJob
{
    std::function<bool()> prepare;
    uint64_t rootFrame = 0, lifecycle = 0, chargedBytes = 0;
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false, success = false;
    bool rigidPreparation = false;
    bool lightManagerPreparation = false;
    bool materialBindingPreparation = false;
};

class RtCpuProducerRewriteService
{
public:
	static constexpr std::uint32_t kInputSlots = 3;
	static constexpr std::uint32_t kProductSlots = 3;
	static constexpr std::uint32_t kOverlaySlots = 3;
	static constexpr std::size_t kInputBytes = 32ull * 1024ull * 1024ull;
	static constexpr std::size_t kProductBytes = kRtCpuRewriteProductBytes;
	static constexpr std::size_t kScratchBytes = 64ull * 1024ull * 1024ull;
    // Route vectors/maps are separate temporary allocations, not this fixed arena.
    static constexpr std::uint64_t kSkinnedHeapScratchBytes = 512ull * 1024ull * 1024ull;
	static constexpr std::size_t kOverlayBytes = 1ull * 1024ull * 1024ull;
	static constexpr std::size_t kOverlayJointBytes = 4ull * 1024ull * 1024ull;
    static constexpr std::size_t kOverlayMaxBytes = 32ull * 1024ull * 1024ull;
    static constexpr std::size_t kOverlayJointMaxBytes = 64ull * 1024ull * 1024ull;
	static constexpr std::uint64_t kGpuRigidRetainCapBytes = kRtCpuRewriteGpuRigidRetainCapBytes;
	static constexpr std::uint64_t kGpuSkinnedRetainCapBytes = kRtCpuRewriteGpuSkinnedRetainCapBytes;

	RtCpuProducerRewriteService();
	~RtCpuProducerRewriteService();

	RtCpuProducerRewriteService(const RtCpuProducerRewriteService&) = delete;
	RtCpuProducerRewriteService& operator=(const RtCpuProducerRewriteService&) = delete;

	void Init();
	void Shutdown();
    std::shared_ptr<RtCpuRewriteMaterialJob> SubmitMaterials(RtCpuRewriteMaterialInput&& input, bool deferBindings = false);
    bool PublishMaterialBindings(const std::shared_ptr<RtCpuRewriteMaterialJob>& job, RtCpuMaterialBindingInput&& bindings);
    void CancelMaterialBindings(const std::shared_ptr<RtCpuRewriteMaterialJob>& job);
    bool FinishMaterials(const std::shared_ptr<RtCpuRewriteMaterialJob>& job);
    std::shared_ptr<RtCpuRewriteLightJob> SubmitLightPreparation(uint64_t rootFrame,
        uint64_t chargedBytes, std::function<bool()> prepare);
    bool FinishLightPreparation(const std::shared_ptr<RtCpuRewriteLightJob>& job, uint64_t rootFrame);
    std::shared_ptr<RtCpuRewriteLightJob> SubmitLightManagerPreparation(uint64_t rootFrame,
        uint64_t chargedBytes, std::function<bool()> prepare);
    bool FinishLightManagerPreparation(const std::shared_ptr<RtCpuRewriteLightJob>& job, uint64_t rootFrame);
    std::shared_ptr<RtCpuRewriteLightJob> SubmitMaterialBindingPreparation(uint64_t rootFrame,
        uint64_t chargedBytes, std::function<bool()> prepare);
    bool FinishMaterialBindingPreparation(const std::shared_ptr<RtCpuRewriteLightJob>& job, uint64_t rootFrame);
    std::shared_ptr<RtCpuRewriteLightJob> SubmitMaterialRecords(uint64_t rootFrame,
        uint64_t chargedBytes, std::function<bool()> prepare);
    bool FinishMaterialRecords(const std::shared_ptr<RtCpuRewriteLightJob>& job, uint64_t rootFrame);
    std::shared_ptr<RtCpuRewriteLightJob> SubmitRigidPreparation(uint64_t rootFrame,
        uint64_t chargedBytes, std::function<bool()> prepare);
    bool FinishRigidPreparation(const std::shared_ptr<RtCpuRewriteLightJob>& job, uint64_t rootFrame);


	void Invalidate(RtCpuRewriteInvalidReason reason);
	void BeginLevelLoad();
	void EndLevelLoad();
	void SampleRequestedRoute(int cvarValue);
    // Owner-only: frontend capture and backend scene consumption must be idle.
    void ApplyRouteAtFrameBoundary(int cvarValue);
    void NotifyBackendDrained();
    void RequestRecovery(std::uint32_t reason);
    // Diagnostic inspection must not consume the request before the idle boundary.
    std::uint32_t PendingRecoveryReason() const { return m_pendingRecovery.load(); }
    std::uint32_t RecoveryReason() const { return m_recoveryReason.load(); }

	bool TryAcquireRootInput(std::uint64_t rootFrame, std::uint64_t worldGeneration, std::uint64_t mapGeneration);
    // Called by the frontend before its static capture. Revision changes are cold
    // population events; normal frames reference the immutable worker package.
    bool PrepareResidentStaticInput(std::uint64_t revision, bool& populate);
    std::shared_ptr<const RtCpuRewriteRetainedGeometry> RetainGeometry(const RtCpuRewriteSurfaceWrite& write);
	bool WriteSurface(const RtCpuRewriteSurfaceWrite& write);
	void MarkCapturingInvalid(RtCpuRewriteInvalidReason reason);
	bool SealRootInput();
	void AbortCapturingInput();

	bool WaitForReadyProduct(std::uint32_t timeoutMs, std::uint64_t minimumTicket = 0);
	bool TryAcquireNewestCompatibleProduct(
		std::uint64_t lifecycleGeneration,
		std::uint64_t worldGeneration,
		std::uint64_t mapGeneration,
		std::uint64_t configGeneration,
		std::uint64_t maxRootFrame = UINT64_MAX, std::uint64_t minRootFrame = 0);
	bool TryAcquireExactProduct(std::uint64_t rootFrame, std::uint64_t worldGeneration,
		std::uint64_t mapGeneration, std::uint32_t timeoutMs);
	void DiscardOverlaysThrough(std::uint64_t rootFrame);
	const RtCpuRewriteFrozenProductView* ProductView() const;
	void ReleaseConsumedProduct(bool retain = false);
	void CancelInFlight();
	bool SealOverlayFromCapturingInput(std::uint64_t rootFrame,
		std::shared_ptr<const PathTraceDoomAnalyticLightSnapshotData> analyticLights = {});
	bool TryAcquireOverlay(std::uint64_t viewRootFrame);
	const RtCpuRewriteOverlayView* OverlayView() const;
    // Discarded attempts never advance motion history. Only final scene commit opts in.
	void ReleaseConsumedOverlay(bool committed = false);
    std::uint64_t LastCommittedRootFrame() const { return m_lastOverlay.rootFrame; }
	bool BuildJoinPlan(std::uint64_t viewRootFrame, RtCpuRewriteJoinResult& out);
	bool BuildLateJoinPlan(std::uint64_t viewRootFrame, RtCpuRewriteJoinResult& out);
	void NoteMeshGpuHit();
	void NoteMeshGpuMiss(std::uint64_t uploadBytes, bool rebuiltBlas);
	void NoteSkippedUploadFrame();
	void NoteLateGeometryReuse();
	void NoteGpuRetainBytes(std::uint64_t bytes);
	void NoteJoinCommitFail(std::uint32_t failReason);
	void NoteCommitTail(bool transformOnly, std::uint64_t packedVertBytes);
	void NotePackedGeometryFill(bool skipped);
	void NoteCaptureEntityDecision(PtCanonicalMeshSourceDomain domain,
		RtCpuRewriteCaptureAdmissionReason reason);
	void NoteCaptureSkinnedSurfaceDecision(RtCpuRewriteCaptureAdmissionReason reason,
		std::uint32_t jointRows = 0);
	void NoteRetirementLedger(const RtCpuRewriteRetirementLedger& ledger);

	RtCpuProducerRewriteRoute Route() const;
	RtCpuRewriteCounters Counters() const;
	std::uint64_t LifecycleGeneration() const;
	std::uint64_t ConfigGeneration() const;
	bool HasCapturingSlot() const;

	void SetGeometryBuildStallForHarness(bool stall);
	bool WaitUntilBuildingForHarness(std::uint32_t timeoutMs);
	void AbandonAcquiredInputForHarness();
	void ReleaseHeldInputsForHarness();
	void AdvanceRootFrameForHarness();
	bool WaitForDrainForHarness(std::uint32_t timeoutMs);
	bool TamperSealedManifestForHarness();
	bool TamperManifestKeyForHarness();
	bool TamperJointSpanForHarness();
	bool TamperManifestOffsetForHarness();
	bool TamperReceiptCycleForHarness();
	bool TamperProductBoundaryForHarness();
	void SetWriteStallForHarness(bool stall);
	void SetWorkerReadingStallForHarness(bool stall);
	bool WaitUntilWriterEnteredForHarness(std::uint32_t timeoutMs);
	bool WaitUntilWorkerReadingForHarness(std::uint32_t timeoutMs);

	void NotifyGpuCommit(std::uint64_t ticket);
	void NotifyGpuReuse();
	void EnterDrainingToLegacy();

private:
    std::shared_ptr<RtCpuRewriteLightJob> SubmitPlanningPreparation(uint64_t rootFrame,
        uint64_t chargedBytes, std::function<bool()> prepare, bool rigid);
	struct InputSlot
	{
        std::mutex geometryMutex;
        std::vector<std::shared_ptr<const RtCpuRewriteRetainedGeometry>> geometryOwners;
        std::uint64_t staticRevision = 0;
        bool populateStatic = false;
        std::unique_ptr<std::uint8_t[]> initialArena;
        std::uint8_t* Data() const { return initialArena ? initialArena.get() : arena; }
        std::uint64_t Limit() const { return initialArena ? 128ull * 1024 * 1024 : kInputBytes; }
		std::atomic<RtCpuRewriteInputSlotState> state{ RtCpuRewriteInputSlotState::Free };
		std::uint64_t ticket = 0;
		std::uint64_t lifecycleGeneration = 0;
		std::uint64_t worldGeneration = 0;
		std::uint64_t mapGeneration = 0;
		std::uint64_t configGeneration = 0;
		std::uint64_t rootFrame = 0;
		std::atomic<std::uint64_t> cursor{ 0 };
		std::atomic<std::uint32_t> writerCount{ 0 };
		std::atomic<std::uint32_t> invalid{ 0 };
		std::atomic<std::uint32_t> receipts{ 0 };
		std::atomic<std::uint32_t> receiptHead{ 0 };
		std::uint64_t canonicalSeal = 0;
		std::uint8_t* arena = nullptr;
	};

	struct ProductSlot
	{
        std::unique_ptr<std::uint8_t[]> initialArena;
        std::uint8_t* Data() const { return initialArena ? initialArena.get() : arena; }
        std::uint64_t Limit() const { return initialArena ? 256ull * 1024 * 1024 : kProductBytes; }
		std::atomic<RtCpuRewriteProductSlotState> state{ RtCpuRewriteProductSlotState::Free };
		std::uint64_t ticket = 0;
		std::uint64_t inputSeal = 0;
		std::uint64_t lifecycleGeneration = 0;
		std::uint64_t worldGeneration = 0;
		std::uint64_t mapGeneration = 0;
		std::uint64_t configGeneration = 0;
		std::uint64_t contentSignature = 0;
		std::uint64_t rootFrame = 0;
		std::atomic<std::uint64_t> usedBytes{ 0 };
		RtCpuRewriteFrozenProductView view{};
		std::uint8_t* arena = nullptr;
	};

	struct SurfaceManifest
	{
        uint32_t geometryRef = UINT32_MAX;
		std::uint64_t sourceContentSignature = 0;
		PtCanonicalMeshKey meshKey;
		PtCanonicalInstanceKey instanceKey;
		std::uint32_t sourceClass = 0;
		float modelMatrix[16];
		float bounds[6];
		std::uint32_t materialLogicalId = 0;
		std::uint32_t activeEmissiveStage = 0;
		std::uint32_t surfaceOrdinal = 0;
		std::uint32_t vertexOffset = 0;
		std::uint32_t vertexCount = 0;
		std::uint32_t indexOffset = 0;
		std::uint32_t indexCount = 0;
		std::uint32_t jointOffset = 0;
		std::uint32_t jointCount = 0;
	};
	static_assert(sizeof(SurfaceManifest) <= 256, "manifest row cap");

	struct ShardReceipt
	{
		std::uint32_t nextOffset = 0;
		std::uint32_t manifestOffset = 0;
	};

	bool ReserveBytes(InputSlot& slot, std::uint64_t bytes, std::uint64_t alignment, std::uint64_t& outOffset);
	bool PublishReceipt(InputSlot& slot, std::uint32_t receiptOffset);
	bool ValidateAndSealInput(InputSlot& slot);
	bool BuildFrozenProduct(InputSlot& input, ProductSlot& product);
    bool WriteSurfaceReference(const RtCpuRewriteSurfaceWrite& write);
    const RtCpuRewriteOwnedVertex* InputVertices(const InputSlot& input, const SurfaceManifest& man) const;
    const uint32_t* InputIndexes(const InputSlot& input, const SurfaceManifest& man) const;
    std::shared_ptr<std::atomic<uint64_t>> m_geometryBudget = std::make_shared<std::atomic<uint64_t>>(0);
    std::shared_ptr<const RtCpuRewriteResidentDynamic> m_residentDynamic;
	void FreeInputSlot(InputSlot& slot);
	void FreeProductSlot(ProductSlot& slot);
	void GeometryWorkerMain();
	void MaterialRecordsWorkerMain();
    void ShadingWorkerMain();
    void LightManagerWorkerMain();
    std::shared_ptr<RtCpuRewriteLightJob> SubmitManagerSlot(uint64_t rootFrame,
        uint64_t chargedBytes, std::function<bool()> prepare, bool bindings);
    bool FinishManagerSlot(const std::shared_ptr<RtCpuRewriteLightJob>& job, uint64_t rootFrame, bool bindings);
	void WakeWorkers();
	void WaitForInFlightIdle();
	void NoteSlotBytes();
	std::uint8_t* ScratchAlloc(std::uint64_t bytes, std::uint64_t alignment, std::uint64_t& used);

	enum class OverlaySlotState : std::uint32_t
	{
		Free = 0,
		Writing,
		Sealed,
		Consuming
	};

	struct OverlaySlot
	{
		std::atomic<OverlaySlotState> state{ OverlaySlotState::Free };
		std::uint64_t rootFrame = 0;
		std::uint64_t lifecycleGeneration = 0;
		std::uint32_t rowCount = 0;
		RtCpuRewriteOverlayView view{};
		std::uint8_t* arena = nullptr;
		std::uint8_t* jointArena = nullptr;
		std::uint32_t jointCount = 0;
        std::uint64_t rowCapacity = kOverlayBytes, jointCapacity = kOverlayJointBytes;
	};

	void FreeOverlaySlot(OverlaySlot& slot);
	bool CopyOverlayFromInput(InputSlot& input, OverlaySlot& overlay, std::uint64_t rootFrame);

	InputSlot m_inputs[kInputSlots];
    // Cache is geometry-worker owned; immutable shared owners in products survive
    // replacement. Population state is the frontend/worker completion receipt.
    std::shared_ptr<const RtCpuRewriteResidentStatic> m_residentStatic;
    std::atomic<std::uint64_t> m_staticRequestedRevision{ 0 };
    std::atomic<std::uint32_t> m_staticPopulationState{ 0 }; // 0 needed, 1 queued, 2 built
	ProductSlot m_products[kProductSlots];
	std::mutex m_overlayMutex;
	OverlaySlot m_overlays[kOverlaySlots];
	OverlaySlot m_lastOverlay;
	std::atomic<int> m_currentOverlay{ -1 };
	std::vector<RtCpuRewriteLastJoined> m_lastJoined;
	std::vector<RtCpuRewriteJoinedSkinned> m_lastSkinnedJoined;
	std::uint32_t m_lastPackedVertexCount = 0;
	std::uint32_t m_lastPackedIndexCount = 0;
	std::uint32_t m_lastPackedTriangleCount = 0;
	std::uint8_t* m_scratch = nullptr;
	std::atomic<std::uint64_t> m_scratchUsed{ 0 };
	std::atomic<int> m_currentInput{ -1 };
	std::atomic<int> m_currentProduct{ -1 };
	std::atomic<std::uint64_t> m_nextTicket{ 1 };
	std::atomic<std::uint64_t> m_lifecycleGeneration{ 1 };
	std::atomic<std::uint64_t> m_configGeneration{ 1 };
	std::atomic<RtCpuProducerRewriteRoute> m_route{ RtCpuProducerRewriteRoute::LegacyOnly };
	std::atomic<int> m_requestedRoute{ 0 };
	std::atomic<bool> m_stop{ false };
	std::atomic<bool> m_cancel{ false };
	std::atomic<bool> m_hasWork{ false };
	std::atomic<bool> m_stallBuild{ false };
	std::atomic<bool> m_inBuilding{ false };
	std::atomic<bool> m_acceptance{ true };
	std::atomic<bool> m_drainComplete{ false };
    std::atomic<bool> m_backendDrainComplete{ false };
    std::atomic<std::uint32_t> m_pendingRecovery{ 0 }, m_recoveryReason{ 0 };
	std::atomic<std::uint64_t> m_rootFrame{ 0 };
	std::atomic<bool> m_stallWrite{ false };
	std::atomic<bool> m_stallWorkerReading{ false };
	std::atomic<bool> m_inWorkerReading{ false };
	mutable std::mutex m_mutex;
	std::condition_variable m_cv;
	std::thread m_geometryThread;
	std::thread m_shadingThread;
    std::shared_ptr<RtCpuRewriteMaterialJob> m_materialJob;
    bool m_materialBusy = false;
    std::shared_ptr<RtCpuRewriteLightJob> m_lightJob;
    bool m_lightBusy = false;
    std::thread m_lightManagerThread;
    std::shared_ptr<RtCpuRewriteLightJob> m_lightManagerJob;
    bool m_lightManagerBusy = false;
    std::shared_ptr<RtCpuRewriteLightJob> m_materialRecordsJob;
    bool m_materialRecordsBusy = false;
	std::thread m_planningThread;
	bool m_started = false;
	RtCpuRewriteCounters m_counters{};
	mutable std::mutex m_counterMutex;
};

void RtCpuProducerRewrite_InitService();
void RtCpuProducerRewrite_ShutdownService();
void RtCpuProducerRewrite_Invalidate(RtCpuRewriteInvalidReason reason);
void RtCpuProducerRewrite_BeginLevelLoad();
void RtCpuProducerRewrite_EndLevelLoad();
RtCpuProducerRewriteService* RtCpuProducerRewrite_GetService();

#if !defined(RT_PT_CPU_PRODUCER_REWRITE_HARNESS)
void RtCpuProducerRewrite_OnFrameBoundary();
void RtCpuProducerRewrite_OnRootViewBegin(viewDef_t* parms);
void RtCpuProducerRewrite_OnRootViewSealOrAbort(viewDef_t* parms);
void RtCpuProducerRewrite_CaptureAdmittedModel(viewEntity_t* vEntity, idRenderModel* resolvedSurfaceModel);
#endif
