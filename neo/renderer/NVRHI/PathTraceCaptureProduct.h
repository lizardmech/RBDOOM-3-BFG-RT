#pragma once

#include "PathTraceCpuProducerApplyGate.h"
#include "PathTraceCommittedReferencedSetReceipt.h"
#include "PathTraceDoomMaterialClassifierKernel.h"
#include "PathTraceGeometry.h"
#include "PathTraceProducerLaneContract.h"
#include "PathTraceRuntimeMaterialEvalKernel.h"
#include "PathTraceRigidPreparedPayload.h"
#include "PathTraceSemanticConfig.h"
#include "PathTraceSceneCapture.h"
#include "PathTraceSkinnedHitRoute.h"
#include "PathTraceSurfaceClassification.h"
#include "PathTraceUniversePlanningSnapshot.h"
#include "../../idlib/geometry/DrawVert.h"
#include "../../idlib/geometry/JointTransform.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

struct drawSurf_t;
struct viewDef_t;
class RtPathTraceInstanceUniverse;
class RtSmokeGeometryUniverse;
struct PtGeometryIdentityBinding;
struct PtGeometrySourceRecord;

constexpr std::size_t RT_PT_CAPTURE_MATERIAL_NAME_BYTES = 1024;
constexpr std::size_t RT_PT_CAPTURE_MODEL_NAME_BYTES = 256;
constexpr std::uint32_t RT_PT_CAPTURE_INVALID_MODEL_TABLE = UINT32_MAX;

inline std::uint32_t BuildPathTraceCaptureTriangleClassWordFromPod(
    std::uint32_t surfaceClassId,
    bool activeEmissiveStage,
    bool invalidNormalTriangle)
{
    return surfaceClassId |
        (activeEmissiveStage ? 0u : RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) |
        (invalidNormalTriangle ? RT_SMOKE_TRIANGLE_FORCE_GEOMETRIC_NORMAL : 0u);
}

inline std::uint32_t BuildPathTraceCaptureTriangleIdentityFromPod(
    std::int32_t entityIndex,
    std::int32_t entityNum,
    std::uint32_t baseMaterialId,
    std::uint64_t triIdentityBits,
    std::uint32_t emittedTriangleOrdinal)
{
    std::uint64_t hash = 14695981039346656037ull;
    const auto add = [&hash](const void* data, std::size_t size)
    {
        const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
        for (std::size_t index = 0; index < size; ++index)
        {
            hash ^= bytes[index];
            hash *= 1099511628211ull;
        }
    };
    add(&entityIndex, sizeof(entityIndex));
    add(&entityNum, sizeof(entityNum));
    add(&baseMaterialId, sizeof(baseMaterialId));
    const std::uintptr_t triBits = static_cast<std::uintptr_t>(triIdentityBits);
    add(&triBits, sizeof(triBits));
    add(&emittedTriangleOrdinal, sizeof(emittedTriangleOrdinal));
    const std::uint32_t identity = static_cast<std::uint32_t>(hash) ^
        static_cast<std::uint32_t>(hash >> 32);
    return identity != 0 ? identity : 1;
}

inline bool PathTraceCaptureShouldOmitRoutedRigidFromPod(
    bool removeRoutedRigidDynamic,
    RtSmokeSurfaceClass surfaceClass,
    bool rigidReadyByMesh,
    bool rigidReadyByResident)
{
    return removeRoutedRigidDynamic &&
        surfaceClass == RtSmokeSurfaceClass::RigidEntity &&
        (rigidReadyByMesh || rigidReadyByResident);
}

struct RtPathTraceCaptureModelTokenTablePod
{
    std::uint64_t modelBits = 0;
    std::uint64_t modelEpoch = 0;
    std::uint32_t tokenOffset = 0;
    std::uint32_t tokenCount = 0;
};
static_assert(std::is_trivially_copyable<
    RtPathTraceCaptureModelTokenTablePod>::value,
    "model token tables must remain pointer-free value records");

inline std::uint32_t FindPathTraceCaptureModelTokenTableFromPod(
    std::uint64_t modelBits,
    std::uint64_t modelEpoch,
    const RtPathTraceCaptureModelTokenTablePod* tables,
    std::size_t tableCount)
{
    if (tables == nullptr)
    {
        return RT_PT_CAPTURE_INVALID_MODEL_TABLE;
    }
    for (std::size_t index = 0; index < tableCount; ++index)
    {
        if (tables[index].modelBits == modelBits &&
            tables[index].modelEpoch == modelEpoch)
        {
            return index <= UINT32_MAX ? static_cast<std::uint32_t>(index)
                : RT_PT_CAPTURE_INVALID_MODEL_TABLE;
        }
    }
    return RT_PT_CAPTURE_INVALID_MODEL_TABLE;
}

enum class RtPathTraceCaptureSafetyDisposition : std::uint8_t
{
    Ready,
    NullSurface,
    MissingGeometry,
    MissingMaterial,
    MissingSpace,
    WorldContainmentRejected,
    MissingPayload,
    InvalidCounts,
    NonCurrentCache,
    CopyFailed
};

enum class RtPathTraceCaptureTerminal : std::uint8_t
{
    SafetyRejected,
    ApplyGateSkip,
    SemanticRejected,
    StaticMatched,
    RoutedRigidReady,
    AdmissionRejected,
    RolledBack,
    Accepted
};
static_assert(static_cast<std::uint32_t>(RtPathTraceCaptureTerminal::Accepted) ==
        RT_PT_CAPTURE_TERMINAL_ACCEPTED_SCALAR,
    "accepted-set telemetry scalar must track the production terminal ABI");

struct RtPathTraceCaptureRawSurface
{
    std::uint32_t ordinal = 0;
    std::int32_t entityIndex = -1;
    std::int32_t entityNum = -1;
    std::int32_t requestedModelSurfaceIndex = -1;
    std::uint32_t modelTableIndex = RT_PT_CAPTURE_INVALID_MODEL_TABLE;
    std::int32_t currentArea = -1;
    std::int32_t jointIndex = -1;
    std::uint64_t triIdentityBits = 0;
    std::uint64_t currentTriToken = 0;
    std::uint64_t entityDefBits = 0;
    std::uint64_t modelBits = 0;
    std::uint64_t materialIdentityBits = 0;
    std::uint64_t modelEpoch = 0;
    std::uint64_t renderWorldIdentity = 0;
    std::int32_t renderDefIndex = -1;
    std::uint32_t renderDefGeneration = 0;
    std::uint64_t ambientHandle = 0;
    std::uint64_t indexHandle = 0;
    std::uint64_t jointHandle = 0;
    std::uint64_t vertexBufferIdentity = 0;
    std::uint64_t indexBufferIdentity = 0;
    std::uint64_t extraGLState = 0;
    std::uint32_t vertexOffset = 0;
    std::uint32_t vertexCount = 0;
    // indexOffset begins one owned full-triangle span. indexCount is the
    // draw-surface prefix consumed by dynamic geometry; sourceTriIndexCount is
    // the full span consumed by rigid preparation.
    std::uint32_t indexOffset = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t sourceTriIndexCount = 0;
    std::uint32_t jointOffset = 0;
    std::uint32_t jointCount = 0;
    PtCanonicalInstanceKey canonicalInstance;
    std::uintptr_t jointSource = 0;
    std::uint32_t triNumVerts = 0;
    std::uint32_t triNumIndexes = 0;
    std::uint32_t triTriangleCount = 0;
    std::uint32_t classifierStageOffset = 0;
    std::uint32_t classifierStageCount = 0;
    std::uint32_t runtimeStageOffset = 0;
    std::uint32_t runtimeStageCount = 0;
    std::uint32_t registerOffset = 0;
    std::uint32_t registerCount = 0;
    RtPathTraceCaptureSafetyDisposition safety =
        RtPathTraceCaptureSafetyDisposition::NullSurface;
    RtSmokeSurfaceClassifyInput classify;
    RtSmokeTranslucentClassifierInput classifier;
    char materialName[RT_PT_CAPTURE_MATERIAL_NAME_BYTES] = {};
    char modelName[RT_PT_CAPTURE_MODEL_NAME_BYTES] = {};
    float modelMatrix[16] = {};
    // The first SL_BUMP texture matrix is also the rigid worker's
    // normalTexMatrix input. Keep one captured authority for both consumers.
    float bumpMatrix[6] = {};
    float objectToWorld[16] = {};
    float boundsCenter[3] = {};
    float triangleBoundsMin[3] = {};
    float triangleBoundsMax[3] = {};
    bool materialInfoRegistrationRequested = false;
    bool registersPresent = false;
    bool guiAllowed = false;
    bool callbackAllowed = false;
    bool rtCpuSkinned = false;
    std::uint32_t skinnedCaptureAdmission = static_cast<std::uint32_t>(
        PtSkinnedCaptureAdmissionResult::GateDisabled);
    std::uint32_t skinnedOmitVertexCount = 0;
    std::uint32_t skinnedOmitIndexCount = 0;
    RtPathTraceRuntimeMaterialDecisionPod derivedRuntimeMaterial;
    std::uint64_t derivedMeshHash = 0;
    std::uint32_t derivedChosenMaterialId = 0;
    std::uint32_t derivedMaterialClassSignature = 0;
    std::int32_t derivedResolvedModelSurfaceIndex = -1;
    std::uint32_t derivedBaseMaterialId = 0;
    bool derivedRuntimeMaterialPresent = false;
    bool rigidIdentityPresent = false;
    bool rigidReadyByMesh = false;
    bool rigidReadyByResident = false;
    bool semanticFactsPresent = false;
    bool semanticFactsDerived = false;
    bool activeEmissiveStage = false;
    bool entityCallbackPresent = false;
    bool entityForceUpdate = false;
    bool dynamicModelPresent = false;
    bool cachedDynamicModelPresent = false;
    bool customShaderPresent = false;
    bool customSkinPresent = false;
    bool particleCompositeEnabled = false;
    bool liquidPoolEnabled = false;
    bool weaponDepthHack = false;
    bool allowSurfaceInView = false;
    bool unifiedPtEnabled = false;
    bool removeAlphaClipEnabled = false;
};
static_assert(std::is_trivially_copyable<RtPathTraceCaptureRawSurface>::value,
    "Lane A surface snapshot rows must remain pointer-free value records");

inline std::uint64_t RtPathTraceLegacyStaticSurfaceKeyFromRaw(
    const RtPathTraceCaptureRawSurface& surface) noexcept
{
    std::uint64_t hash = 14695981039346656037ull;
    const auto append = [&hash](const void* bytes, std::size_t byteCount)
    {
        const std::uint8_t* input = static_cast<const std::uint8_t*>(bytes);
        for (std::size_t index = 0; index < byteCount; ++index)
        {
            hash ^= input[index];
            hash *= 1099511628211ull;
        }
    };
    const std::uintptr_t tri =
        static_cast<std::uintptr_t>(surface.triIdentityBits);
    const std::uintptr_t material =
        static_cast<std::uintptr_t>(surface.materialIdentityBits);
    const int vertexCount = static_cast<int>(surface.vertexCount);
    const int indexCount = static_cast<int>(surface.sourceTriIndexCount);
    append(&tri, sizeof(tri));
    append(&material, sizeof(material));
    append(&vertexCount, sizeof(vertexCount));
    append(&indexCount, sizeof(indexCount));
    append(&surface.ambientHandle, sizeof(surface.ambientHandle));
    append(&surface.indexHandle, sizeof(surface.indexHandle));
    append(surface.modelMatrix, sizeof(surface.modelMatrix));
    return hash;
}

inline void RtPathTracePersistDerivedRuntimeMaterialDecision(
    RtPathTraceCaptureRawSurface& raw, std::uint32_t baseMaterialId,
    const RtPathTraceRuntimeMaterialDecisionPod& decision) noexcept
{
    raw.derivedRuntimeMaterial = decision;
    raw.derivedBaseMaterialId = baseMaterialId;
    raw.derivedChosenMaterialId = decision.chosenMaterialId;
    raw.derivedRuntimeMaterialPresent = true;
}

inline bool RtPathTraceDerivedRuntimeMaterialEmitsVariant(
    const RtPathTraceCaptureRawSurface& raw) noexcept
{
    return raw.derivedRuntimeMaterialPresent &&
        raw.derivedRuntimeMaterial.chosenMaterialId != raw.derivedBaseMaterialId;
}

inline bool RtPathTraceCaptureSourceSurfaceComplete(
    const RtPathTraceCaptureRawSurface& raw,
    std::size_t classifierStageCount,
    std::size_t runtimeStageCount,
    std::size_t registerCount,
    std::size_t vertexCount,
    std::size_t indexCount,
    std::size_t jointCount)
{
    return !raw.semanticFactsDerived && !raw.semanticFactsPresent &&
        RtPathTraceRuntimeMaterialDecisionPodIsDefault(
            raw.derivedRuntimeMaterial) &&
        !raw.derivedRuntimeMaterialPresent &&
        raw.derivedMeshHash == 0 && raw.derivedChosenMaterialId == 0 &&
        raw.derivedMaterialClassSignature == 0 &&
        raw.derivedResolvedModelSurfaceIndex == -1 &&
        raw.derivedBaseMaterialId == 0 && !raw.rigidIdentityPresent &&
        raw.classifierStageOffset <= classifierStageCount &&
        raw.classifierStageCount <= classifierStageCount - raw.classifierStageOffset &&
        raw.runtimeStageOffset <= runtimeStageCount &&
        raw.runtimeStageCount <= runtimeStageCount - raw.runtimeStageOffset &&
        raw.registerOffset <= registerCount &&
        raw.registerCount <= registerCount - raw.registerOffset &&
        raw.vertexOffset <= vertexCount &&
        raw.vertexCount <= vertexCount - raw.vertexOffset &&
        raw.indexOffset <= indexCount &&
        raw.indexCount <= indexCount - raw.indexOffset &&
        raw.jointOffset <= jointCount &&
        raw.jointCount <= jointCount - raw.jointOffset;
}

inline bool RtPathTraceCaptureSemanticFactsReadable(
    const RtPathTraceCaptureRawSurface& raw)
{
    return raw.semanticFactsDerived && raw.semanticFactsPresent;
}

class RtPathTraceCaptureSemanticFactsProvider
{
public:
    virtual ~RtPathTraceCaptureSemanticFactsProvider() = default;
    virtual bool IsRigidRouteReady(std::uint64_t meshHash) const = 0;
    virtual bool IsRigidRouteResidentReadyForEntityMaterial(
        std::int32_t entityIndex, std::int32_t entityNum,
        std::uint32_t materialId) const = 0;
    virtual const PtGeometryIdentityBinding* FindCanonicalIdentityBinding(
        const PtCanonicalInstanceKey& instance) const = 0;
    virtual const PtGeometrySourceRecord* FindCanonicalSourceRecord(
        const PtCanonicalMeshKey& mesh) const = 0;
};

// Allocation-free upper bounds captured by the owner before any slot-owned
// vector is reserved.  The same plan controls snapshot, worker candidate/final
// product, and serial-oracle storage; it is therefore the complete-slot budget
// authority rather than a post-build diagnostic.
struct RtPathTraceCaptureCapacityCounts
{
    std::size_t surfaces = 0;
    std::size_t classifierStages = 0;
    std::size_t runtimeStages = 0;
    std::size_t registers = 0;
    std::size_t vertices = 0;
    std::size_t indexes = 0;
    std::size_t joints = 0;
    std::size_t variantBases = 0;
    std::size_t registryMaterials = 0;
    std::size_t modelTables = 0;
    std::size_t modelSurfaceTokens = 0;
    std::size_t applyGateKeys = 0;
    std::size_t instanceMeshes = 0;
    std::size_t instanceHistories = 0;
    std::size_t geometryStaticSurfaces = 0;
    std::size_t geometryRigidRoutes = 0;
    std::size_t geometryRigidResidents = 0;
    std::size_t materialInfoIntents = 0;
    std::size_t materialVariantProposals = 0;
    std::size_t instanceObservationProposals = 0;
    std::size_t rigidCandidateProposals = 0;
    std::size_t preparedRigidPayloads = 0;
    std::size_t preparedRigidVertices = 0;
    std::size_t preparedRigidIndexes = 0;
    std::size_t receiptSurfaces = 0;
    std::size_t frameMaterialIds = 0;
};

// Pointer-free compatibility contract for the deliberately one-frame-late
// Lane-A geometry path.  Frame/generation and view addresses are intentionally
// excluded: they identify a publication, not whether its geometry policy is
// still valid for the current owner harvest.  The three owner revision fields
// remain telemetry only; final membership/readiness is coupled by the receipt.
struct RtPathTraceLateConsumeToken
{
    std::uint64_t mapTimeStamp = 0;
    std::uint64_t mapLoadSerial = 0;
    char mapName[RT_PT_PLANNING_MAP_NAME_CAPACITY] = {};
    std::uint64_t registryGeneration = 0;
    std::uint64_t residentMaterialFactsGeneration = 0;
    std::uint64_t instanceUniverseGeneration = 0;
    std::uint64_t geometryUniverseGeneration = 0;
    std::uint64_t configFingerprint = 0;
    bool capturedAfterBeginFrame = false;
    bool capturedAfterStaticPreload = false;
};

struct RtPathTraceCaptureMembershipReceipt
{
    std::uint64_t hash = 0;
    std::uint32_t surfaceCount = 0;
    std::uint32_t skinnedSurfaceCount = 0;
    std::uint32_t cpuSkinnedAcceptedCount = 0;
    bool complete = false;
};

static_assert(std::is_trivially_copyable<RtPathTraceLateConsumeToken>::value,
    "late-consume token must remain pointer-free POD");
static_assert(std::is_trivially_copyable<RtPathTraceCaptureMembershipReceipt>::value,
    "membership receipt must remain pointer-free POD");

struct RtPathTraceCaptureSurfaceProduct;
struct RtPathTraceCommittedPlanningBaseline;

struct RtPathTraceCaptureOwnerSnapshot
{
    RtPathTracePlanningSnapshotEpoch epoch;
    RtPathTraceLateConsumeToken lateConsumeToken;
    std::uint64_t viewIdentity = 0;
    std::uint64_t registryGeneration = 0;
    std::uint64_t residentMaterialFactsGeneration = 0;
    std::uint64_t historyCopyUs = 0;
    std::uint32_t sourceDrawSurfCount = 0;
    std::uint32_t admissionMaxSurfaces = 0;
    std::uint64_t admissionMaxBytes = 0;
    RtPtCpuProducerApplyGateSnapshot applyGate;
    RtPathTraceInstanceUniverseSnapshot instanceUniverse;
    RtSmokeGeometryUniverseSnapshot geometryUniverse;
    std::vector<RtPathTraceMaterialTextureVariantBasePod> variantBases;
    std::vector<RtPathTraceCaptureRegistryMaterialPod> registryMaterials;
    std::vector<RtPathTraceCaptureModelTokenTablePod> modelTables;
    std::vector<std::uint64_t> modelSurfaceTokens;
    std::vector<RtPathTraceCaptureRawSurface> surfaces;
    // Final owner-authoritative decision handoff, filled only after Harvest
    // and before Lane A is notified.  Source bytes/history remain pre-Harvest.
    std::vector<RtPathTraceCaptureSurfaceProduct> ownerDecisions;
    RtPathTraceCaptureMembershipReceipt ownerMembershipReceipt;
    std::vector<RtSmokeTranslucentClassifierStageInput> classifierStages;
    std::vector<RtPathTraceRuntimeMaterialStagePod> runtimeStages;
    std::vector<float> registers;
    std::vector<idDrawVert> vertices;
    std::vector<triIndex_t> indexes;
    std::vector<idJointMat> joints;
    RtPathTraceCaptureCapacityCounts capacityCounts;
    std::size_t plannedCompleteSlotBytes = 0;
    std::size_t reservedOracleBytes = 0;
    bool complete = false;
    bool recordAllInstanceClasses = false;
    bool removeRoutedRigidDynamic = false;
    bool rigidRouteEmissiveCards = false;
    bool ownerDecisionsComplete = false;

    void ResetAndRelease();
    std::size_t OwnedBytes() const;
};

inline std::uint64_t RtPathTraceOwnerSemanticConfigFingerprint(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    int producerMode, int producerLaneMask,
    bool skinnedCaptureSplitGate) noexcept
{
    std::uint64_t fingerprint = RtPathTraceSemanticConfigFingerprintBegin();
    const auto append = [&fingerprint](const void* bytes, std::size_t count)
    {
        RtPathTraceSemanticConfigFingerprintAppend(fingerprint, bytes, count);
    };
    append(&producerMode, sizeof(producerMode));
    append(&producerLaneMask, sizeof(producerLaneMask));
    append(&snapshot.recordAllInstanceClasses,
        sizeof(snapshot.recordAllInstanceClasses));
    append(&snapshot.removeRoutedRigidDynamic,
        sizeof(snapshot.removeRoutedRigidDynamic));
    append(&snapshot.rigidRouteEmissiveCards,
        sizeof(snapshot.rigidRouteEmissiveCards));
    append(&snapshot.admissionMaxSurfaces,
        sizeof(snapshot.admissionMaxSurfaces));
    append(&snapshot.admissionMaxBytes,
        sizeof(snapshot.admissionMaxBytes));
    append(&skinnedCaptureSplitGate, sizeof(skinnedCaptureSplitGate));
    append(&snapshot.applyGate.enabled, sizeof(snapshot.applyGate.enabled));
    append(&snapshot.applyGate.acceptedSkinnedBuildLive,
        sizeof(snapshot.applyGate.acceptedSkinnedBuildLive));
    for (const RtPathTraceCaptureRawSurface& raw : snapshot.surfaces)
    {
        append(&raw.guiAllowed, sizeof(raw.guiAllowed));
        append(&raw.callbackAllowed, sizeof(raw.callbackAllowed));
        append(&raw.particleCompositeEnabled,
            sizeof(raw.particleCompositeEnabled));
        append(&raw.liquidPoolEnabled, sizeof(raw.liquidPoolEnabled));
        append(&raw.unifiedPtEnabled, sizeof(raw.unifiedPtEnabled));
        append(&raw.removeAlphaClipEnabled,
            sizeof(raw.removeAlphaClipEnabled));
    }
    return fingerprint;
}

struct RtPathTraceMaterialInfoRegistrationIntent
{
    std::uint64_t generation = 0;
    std::uint32_t surfaceOrdinal = 0;
    std::uint32_t baseMaterialId = 0;
    char materialName[RT_PT_CAPTURE_MATERIAL_NAME_BYTES] = {};
    std::uint32_t reason = 0;
    bool callPresent = false;
};

struct RtPathTraceMaterialVariantRegistrationProposal
{
    std::uint32_t surfaceOrdinal = 0;
    std::uint32_t baseMaterialId = 0;
    std::uint32_t candidateId = 0;
    std::uint32_t chosenId = 0;
    std::uint32_t collisionCount = 0;
    bool fallbackUsed = false;
};

struct RtPathTraceInstanceObservationProposal
{
    std::uint32_t surfaceOrdinal = 0;
    std::uint64_t instanceId = 0;
    std::uint64_t meshHash = 0;
    bool hasPrevious = false;
    bool transformContinuous = false;
    float currentObjectToWorld[16] = {};
    float previousObjectToWorld[16] = {};
};

struct RtPathTraceRigidCandidateProposal
{
    std::uint32_t surfaceOrdinal = 0;
    std::uint64_t meshHash = 0;
    std::uint64_t instanceId = 0;
    std::uint32_t materialId = 0;
};

struct RtPathTraceCaptureSurfaceProduct
{
    std::uint32_t ordinal = 0;
    RtPathTraceCaptureTerminal terminal = RtPathTraceCaptureTerminal::SafetyRejected;
    RtSmokeSurfaceClass surfaceClass = RtSmokeSurfaceClass::Unknown;
    RtSmokeTranslucentSubtype translucentSubtype = RtSmokeTranslucentSubtype::Unknown;
    std::uint32_t surfaceClassId = 0;
    std::uint32_t materialId = 0;
    std::uint32_t materialClassSignature = 0;
    std::uint32_t sourceFlags = 0;
    std::uint64_t meshHash = 0;
    std::uint64_t instanceId = 0;
    std::uint32_t vertexOffset = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexOffset = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t triangleOffset = 0;
    std::uint32_t triangleCount = 0;
    std::int32_t invalidIndexCount = 0;
    std::int32_t zeroAreaTriangleCount = 0;
    bool staticMatch = false;
    bool rigidReadyByMesh = false;
    bool rigidReadyByResident = false;
    bool applyGateSkip = false;
    RtPathTraceRuntimeMaterialDecisionPod runtimeMaterial;
    std::uint64_t decisionPresence = 0;
    std::uint32_t compositeRoute = 0;
    std::uint32_t skinnedAdmission = 0;
    std::uint32_t sourceState = 0;
    std::int32_t bucketIndex = -1;
    std::uint32_t preAppendVertexOffset = 0;
    std::uint32_t preAppendIndexOffset = 0;
    std::uint32_t preAppendTriangleOffset = 0;
    std::uint32_t capturedRecordVertexCount = 0;
    std::uint32_t capturedRecordIndexCount = 0;
    std::uint32_t capturedRecordTriangleCount = 0;
    std::uint32_t skinnedRecordVertexCount = 0;
    std::uint32_t skinnedRecordIndexCount = 0;
    std::uint32_t skinnedRecordTriangleCount = 0;
    std::uint32_t mergedWalkVertexCount = 0;
    std::uint32_t mergedWalkIndexCount = 0;
    std::uint32_t mergedWalkTriangleCount = 0;
    std::uint64_t mergedCompanionRigidId = 0;
    std::uint64_t mergedCompanionSkinnedId = 0;
    bool alphaDiagnosticRejected = false;
    bool liquidPoolPromoted = false;
    bool skinnedCaptureOmitted = false;
    bool capturedRecordPresent = false;
    bool skinnedRecordPresent = false;
    bool mergedWalkRecordPresent = false;
    bool bucketRangePublished = false;
    bool admissionPrecheckPassed = false;
    bool appendAttempted = false;
    bool rollbackApplied = false;
};

inline bool PathTraceOwnerDecisionEmitsGeometry(
    const RtPathTraceCaptureSurfaceProduct& decision)
{
    return decision.terminal == RtPathTraceCaptureTerminal::Accepted;
}

enum RtPathTraceCaptureDecisionPresence : std::uint64_t
{
    RT_PT_CAPTURE_DECISION_FILTER = 1ull << 0,
    RT_PT_CAPTURE_DECISION_RUNTIME_MATERIAL = 1ull << 1,
    RT_PT_CAPTURE_DECISION_ROUTE = 1ull << 2,
    RT_PT_CAPTURE_DECISION_SKINNED = 1ull << 3,
    RT_PT_CAPTURE_DECISION_ADMISSION = 1ull << 4,
    RT_PT_CAPTURE_DECISION_APPEND = 1ull << 5,
    RT_PT_CAPTURE_DECISION_CAPTURED_RECORD = 1ull << 6,
    RT_PT_CAPTURE_DECISION_SKINNED_RECORD = 1ull << 7,
    RT_PT_CAPTURE_DECISION_MERGED_WALK = 1ull << 8,
    RT_PT_CAPTURE_DECISION_BUCKET_PUBLICATION = 1ull << 9,
    RT_PT_CAPTURE_DECISION_IDENTITY = 1ull << 10
};

inline bool ApplyPathTraceSkinnedCaptureAdmissionFromPod(
    std::uint32_t admission, std::uint32_t omitVertexCount,
    std::uint32_t omitIndexCount, RtPathTraceCaptureSurfaceProduct& surface)
{
    surface.decisionPresence |= RT_PT_CAPTURE_DECISION_SKINNED;
    surface.skinnedAdmission = admission;
    const bool omit = admission == static_cast<std::uint32_t>(
        PtSkinnedCaptureAdmissionResult::OmitCpuCapture);
    if (!omit)
    {
        return false;
    }
    if (omitVertexCount == 0 || omitIndexCount == 0 ||
        (omitIndexCount % 3u) != 0u)
    {
        surface.terminal = RtPathTraceCaptureTerminal::SemanticRejected;
        surface.skinnedCaptureOmitted = false;
        return true;
    }
    surface.skinnedCaptureOmitted = true;
    surface.terminal = RtPathTraceCaptureTerminal::AdmissionRejected;
    surface.decisionPresence |= RT_PT_CAPTURE_DECISION_SKINNED_RECORD;
    surface.skinnedRecordPresent = true;
    surface.skinnedRecordVertexCount = omitVertexCount;
    surface.skinnedRecordIndexCount = omitIndexCount;
    surface.skinnedRecordTriangleCount = omitIndexCount / 3u;
    return true;
}

inline void FinalizePathTraceCaptureSurfaceDecision(
    RtPathTraceCaptureSurfaceProduct& decision)
{
    decision.decisionPresence |= RT_PT_CAPTURE_DECISION_FILTER;
    if (decision.runtimeMaterial.eval.materialId != 0)
    {
        decision.decisionPresence |= RT_PT_CAPTURE_DECISION_RUNTIME_MATERIAL;
    }
    else
    {
        decision.decisionPresence &= ~RT_PT_CAPTURE_DECISION_RUNTIME_MATERIAL;
    }
}

struct RtPathTraceCaptureProduct
{
    RtPathTracePlanningSnapshotEpoch epoch;
    RtPathTraceLateConsumeToken lateConsumeToken;
    RtPathTraceCaptureMembershipReceipt membershipReceipt;
    std::uint64_t viewIdentity = 0;
    std::vector<RtPathTraceCaptureSurfaceProduct> surfaces;
    std::vector<PathTraceSmokeVertex> vertices;
    std::vector<std::uint32_t> indexes;
    std::vector<std::uint32_t> triangleClasses;
    std::vector<std::uint32_t> triangleMaterials;
    std::vector<std::uint64_t> triangleInstances;
    std::vector<std::uint32_t> triangleIdentities;
    std::vector<RtPathTraceMaterialInfoRegistrationIntent> materialInfoIntents;
    std::vector<RtPathTraceMaterialVariantRegistrationProposal> materialVariants;
    std::vector<RtPathTraceInstanceObservationProposal> instanceObservations;
    std::vector<RtPathTraceRigidCandidateProposal> rigidCandidates;
    std::vector<RtPathTraceRigidPreparedPayload> preparedRigidPayloads;
    // Complete frame material authority for PureSemantic products. This is
    // derived from every finalized row's persisted base and chosen IDs, not
    // from the narrower registration-intent proposal family.
    std::vector<std::uint32_t> frameMaterialIds;
    std::uint64_t finalizedMaterialIdHash = 0;
    std::uint32_t finalizedMaterialIdCount = 0;
    RtPathTraceCommittedReferencedSetReceiptHeader receiptHeader;
    std::vector<RtPathTraceCommittedReferencedSurfaceReceiptRow> receiptSurfaces;
    std::vector<std::uint32_t> staticMembershipSurfaces;
    std::vector<std::uint32_t> routedReadySkipSurfaces;
    RtPathTraceCaptureCapacityCounts capacityCounts;
    std::size_t inputOwnedBytes = 0;
    std::size_t actualOwnedBytes = 0;
    std::uint64_t workerCpuUs = 0;
    // Proposals/history are owner-only for this handoff; exactness covers the
    // shared final decision rows plus geometry, not mutation replay.
    bool ownerDecisionGeometryOnly = false;
    RtPathTraceCaptureProductProvenance provenance =
        RtPathTraceCaptureProductProvenance::None;
    bool complete = false;

    void ResetAndRelease();
    std::size_t OwnedBytes() const;
};

inline std::uint64_t RtPathTraceFinalizedMaterialIdHash(
    const std::uint32_t* ids, std::size_t count) noexcept
{
    if ((count != 0 && ids == nullptr) || count > UINT32_MAX)
        return 0;
    std::uint64_t hash = 14695981039346656037ull;
    const auto appendByte = [&hash](std::uint8_t byte)
    {
        hash ^= byte;
        hash *= 1099511628211ull;
    };
    static constexpr char domain[] = "PTFRAMEMATIDS";
    for (std::size_t byte = 0; byte < sizeof(domain); ++byte)
        appendByte(static_cast<std::uint8_t>(domain[byte]));
    const std::uint32_t count32 = static_cast<std::uint32_t>(count);
    for (std::size_t byte = 0; byte < sizeof(count32); ++byte)
        appendByte(static_cast<std::uint8_t>(count32 >> (byte * 8u)));
    for (std::size_t index = 0; index < count; ++index)
    {
        const std::uint32_t id = ids[index];
        for (std::size_t byte = 0; byte < sizeof(id); ++byte)
            appendByte(static_cast<std::uint8_t>(id >> (byte * 8u)));
    }
    return hash;
}

using RtPathTraceCaptureOracleSurface = RtPathTraceCaptureSurfaceProduct;

struct RtPathTraceCaptureOracle
{
    RtPathTracePlanningSnapshotEpoch epoch;
    std::uint64_t viewIdentity = 0;
    RtPathTraceCaptureLiveCardinalityAttribution liveCardinality;
    std::vector<PathTraceSmokeVertex> vertices;
    std::vector<std::uint32_t> indexes;
    std::vector<std::uint32_t> triangleClasses;
    std::vector<std::uint32_t> triangleMaterials;
    std::vector<std::uint64_t> triangleInstances;
    std::vector<std::uint32_t> triangleIdentities;
    std::vector<RtPathTraceMaterialInfoRegistrationIntent> materialInfoIntents;
    std::vector<RtPathTraceMaterialVariantRegistrationProposal> materialVariants;
    std::vector<RtPathTraceInstanceObservationProposal> instanceObservations;
    std::vector<RtPathTraceRigidCandidateProposal> rigidCandidates;
    std::vector<std::uint32_t> staticMembershipSurfaces;
    std::vector<std::uint32_t> routedReadySkipSurfaces;
    std::vector<RtPathTraceCaptureOracleSurface> surfaces;
    bool complete = false;

    void ResetAndRelease();
    std::size_t OwnedBytes() const;
};

struct RtPathTraceCaptureProductCapacityPlan
{
    std::size_t snapshotBytes = 0;
    std::size_t candidateBytes = 0;
    std::size_t finalProductBytes = 0;
    std::size_t oracleBytes = 0;
    std::size_t peakSlotBytes = 0;
};

// Dependency-light failure seam for executable transaction tests. Production
// always passes nullptr; no CVar or runtime policy reaches this seam.
enum class RtPathTraceCaptureReserveFailure : std::uint8_t
{
    None,
    BadAlloc,
    LengthError
};

struct RtPathTraceCaptureReserveTestSeam
{
    std::size_t failAtReserve = SIZE_MAX;
    std::size_t skipAtReserve = SIZE_MAX;
    std::size_t inflateAtReserve = SIZE_MAX;
    std::size_t inflateBy = 0;
    std::size_t reserveCalls = 0;
    RtPathTraceCaptureReserveFailure failure =
        RtPathTraceCaptureReserveFailure::None;
};

struct RtPathTraceCaptureProductCapacityStamp
{
    std::array<std::size_t, 16> capacities = {};
};

bool PlanPathTraceCompleteSlotCapacity(
    const RtPathTraceCaptureCapacityCounts& counts,
    std::size_t snapshotFixedBytes,
    RtPathTraceCaptureProductCapacityPlan& plan);
bool ReservePathTraceCaptureOwnerSnapshotStorage(
    RtPathTraceCaptureOwnerSnapshot& snapshot,
    std::size_t existingSlotBytes,
    const RtPathTraceCaptureProductCapacityPlan& plan,
    RtPathTraceCaptureReserveTestSeam* testSeam = nullptr);
bool ReservePathTraceCaptureProductStorage(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    RtPathTraceCaptureProduct& product,
    std::size_t otherOwnedBytes,
    std::size_t plannedProductBytes,
    RtPathTraceCaptureReserveTestSeam* testSeam = nullptr);
bool ReservePathTraceCaptureOracleStorage(
    const RtPathTraceCaptureCapacityCounts& counts,
    std::size_t snapshotAndProductOwnedBytes,
    RtPathTraceCaptureOracle& oracle,
    RtPathTraceCaptureReserveTestSeam* testSeam = nullptr);
bool PathTraceCaptureProductStorageReady(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    const RtPathTraceCaptureProduct& product);
RtPathTraceCaptureProductCapacityStamp CapturePathTraceProductCapacityStamp(
    const RtPathTraceCaptureProduct& product);
bool PathTraceCaptureProductCapacityUnchanged(
    const RtPathTraceCaptureProduct& product,
    const RtPathTraceCaptureProductCapacityStamp& stamp);
bool RtPathTraceCaptureProductShapeValid(
    const RtPathTraceCaptureProduct& product) noexcept;
bool PartitionPathTraceCaptureProductBucketMajor(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    const RtPathTraceCaptureProduct& candidate,
    RtPathTraceCaptureProduct& partitioned);
bool PathTraceCaptureProductSpansCanonical(
    const RtPathTraceCaptureProduct& product);
bool PathTraceCaptureSurfaceDecisionsEqual(
    const RtPathTraceCaptureSurfaceProduct& produced,
    const RtPathTraceCaptureSurfaceProduct& applied);
bool PathTraceCaptureSurfaceAuthorityEqual(
    const RtPathTraceCaptureSurfaceProduct& produced,
    const RtPathTraceCaptureSurfaceProduct& applied);
bool CompatibleForLateConsume(
    const RtPathTraceLateConsumeToken& product,
    const RtPathTraceLateConsumeToken& current);
bool PathTraceCaptureMembershipReceiptsMatch(
    const RtPathTraceCaptureMembershipReceipt& product,
    const RtPathTraceCaptureMembershipReceipt& current);
RtPathTraceCaptureMembershipReceipt BuildPathTraceCaptureMembershipReceipt(
    const RtPathTraceCaptureSurfaceProduct* surfaces,
    std::size_t surfaceCount);
void AppendPathTraceCaptureMembershipReceipt(
    RtPathTraceCaptureMembershipReceipt& receipt,
    const RtPathTraceCaptureSurfaceProduct& surface);
bool AttachPathTraceOwnerDecisionTable(
    RtPathTraceCaptureOwnerSnapshot& snapshot,
    const RtPathTraceCaptureSurfaceProduct* decisions,
    std::size_t decisionCount,
    const RtPathTraceCaptureMembershipReceipt& authoritativeReceipt);

struct RtPathTraceCaptureComparison
{
    std::uint64_t generation = 0;
    std::uint32_t geometryMismatches = 0;
    std::uint32_t proposalMismatches = 0;
    RtPathTraceCaptureFirstFailure firstFail =
        RtPathTraceCaptureFirstFailure::None;
    std::uint32_t firstSurfaceOrdinal = UINT32_MAX;
    RtPathTraceCaptureCardinalityAttribution cardinality;
    RtPathTraceCaptureAcceptedSetDiff acceptedSetDiff;
    RtPathTraceCaptureInstanceObservationDiff instanceObservationDiff;
    bool cardinalityAvailable = false;
    bool exact = false;
};

bool CapturePathTraceOwnerSnapshot(
    const viewDef_t* viewDef,
    RtSmokeGeometryUniverse& geometryUniverse,
    RtPathTraceInstanceUniverse& instanceUniverse,
    const RtPathTracePlanningSnapshotEpoch& epoch,
    bool recordAllInstanceClasses,
    bool removeRoutedRigidDynamic,
    bool rigidRouteEmissiveCards,
    const PtSkinnedHitRouteRecord* skinnedAdmissionRoutes,
    std::size_t skinnedAdmissionRouteCount,
    bool skinnedCaptureSplitGate,
    RtPathTraceCaptureOwnerSnapshot& snapshot,
    std::size_t& slotBytes);
bool CapturePathTraceOwnerSourceSnapshot(
    const viewDef_t* viewDef,
    const RtPathTraceCommittedSemanticConfig& semanticConfig,
    RtPathTraceCaptureOwnerSnapshot& snapshot,
    std::size_t& slotBytes);
bool FinalizePathTraceOwnerSemanticSnapshot(
    RtPathTraceCaptureOwnerSnapshot& snapshot,
    const RtPathTraceCaptureSemanticFactsProvider& facts,
    const PtSkinnedHitRouteRecord* skinnedAdmissionRoutes,
    std::size_t skinnedAdmissionRouteCount,
    bool skinnedCaptureSplitGate);
bool BuildPathTraceCaptureProduct(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    RtPathTraceCaptureProduct& product,
    std::size_t oracleOwnedBytes = 0,
    std::size_t* peakSlotBytes = nullptr);

// Dependency-light production geometry kernel used by the owner-decision
// builder and its exact bucket-major parity harness.  It mutates only the
// caller-owned, already-reserved product.
bool BuildPathTraceGeometryFromOwnerDecisions(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    RtPathTraceCaptureProduct& candidate);
// Builds one owned CPU payload per distinct rigid mesh proposal.  PureSemantic
// callers require the committed receipt as an additional witness; the live
// OwnerDecisions path is intentionally receipt-free and is validated directly
// against its captured raw, surface-decision and rigid-proposal authorities.
bool BuildPathTraceRigidPreparedPayloads(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    RtPathTraceCaptureProduct& candidate,
    bool requireReceiptWitness);
bool BuildPathTraceOwnerDecisionsFromFinalizedSemantic(
    RtPathTraceCaptureOwnerSnapshot& snapshot,
    const RtPathTraceCommittedPlanningBaseline& baseline);
bool PlanPathTraceCaptureProductCapacity(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    std::size_t oracleOwnedBytes,
    RtPathTraceCaptureProductCapacityPlan& plan);
RtPathTraceCaptureComparison ComparePathTraceCaptureProduct(
    const RtPathTraceCaptureProduct& product,
    const RtPathTraceCaptureOracle& oracle);
bool ApplyPathTraceCaptureProductToDynamicFrame(
    const RtPathTraceCaptureProduct& product,
    std::vector<PathTraceSmokeVertex>& vertices,
    std::vector<std::uint32_t>& indexes,
    std::vector<std::uint32_t>& triangleClasses,
    std::vector<std::uint32_t>& triangleMaterials,
    std::vector<std::uint32_t>& triangleInstances,
    std::vector<std::uint32_t>& triangleIdentities,
    RtSmokeBucketRanges& bucketRanges,
    int& sourceSurfaces,
    int& sourceVerts,
    int& sourceIndexes);
bool BeginPathTraceCaptureSerialOracle(
    const RtPathTracePlanningSnapshotEpoch& epoch,
    std::uint64_t viewIdentity,
    const RtPathTraceCaptureCapacityCounts& counts,
    std::size_t snapshotAndProductOwnedBytes,
    std::size_t* reservedOracleBytes);
bool SeedPathTraceCaptureSerialOracleSources(
    const RtPathTraceCaptureOwnerSnapshot& snapshot);
bool BuildPathTraceCaptureSerialRuntimeMaterialVariantKey(
    std::uint32_t baseMaterialId,
    std::int32_t entityIndex,
    std::int32_t entityNum,
    RtPathTraceRuntimeMaterialVariantPod& key,
    bool& snapshotOracleActive);
bool ResolvePathTraceCaptureSerialRigidIdentitySurfaceFromSnapshot(
    std::uint64_t modelBits,
    std::uint64_t modelEpoch,
    std::int32_t requestedModelSurfaceIndex,
    std::uint64_t currentTriToken,
    std::int32_t& resolvedModelSurfaceIndex,
    bool& resolvedSurfaceMatchesCurrent,
    bool& comparedCall);
std::uint32_t PathTraceCaptureSerialOracleSourceState(
    std::uint32_t surfaceOrdinal);
void CancelPathTraceCaptureSerialOracle();
void SetPathTraceCaptureSerialOracleSurface(std::uint32_t surfaceOrdinal);
void NotePathTraceCaptureSerialTerminal(
    std::uint32_t surfaceOrdinal,
    RtPathTraceCaptureTerminal terminal);
void NotePathTraceCaptureSerialDerivedSurface(
    std::uint32_t surfaceOrdinal,
    RtSmokeSurfaceClass surfaceClass,
    RtSmokeTranslucentSubtype translucentSubtype,
    std::uint32_t surfaceClassId,
    std::uint32_t materialId,
    std::uint32_t materialClassSignature);
void NotePathTraceCaptureSerialAppend(
    std::uint32_t surfaceOrdinal,
    std::int32_t invalidIndexCount,
    std::int32_t zeroAreaTriangleCount,
    std::uint32_t emittedIndexCount);
void NotePathTraceCaptureSerialRuntimeMaterial(
    std::uint32_t surfaceOrdinal,
    const RtPathTraceRuntimeMaterialDecisionPod& decision);
void NotePathTraceCaptureSerialDecision(
    std::uint32_t surfaceOrdinal,
    const RtPathTraceCaptureSurfaceProduct& decision,
    std::uint64_t presenceMask);
void NotePathTraceCaptureSerialMaterialVariant(
    std::uint32_t baseMaterialId,
    std::uint32_t initialCandidate,
    std::uint32_t chosenId,
    std::uint32_t collisionCount,
    bool fallbackUsed,
    const char* materialName);
void NotePathTraceCaptureSerialStaticMembership(std::uint32_t surfaceOrdinal);
void NotePathTraceCaptureSerialRoutedReady(
    std::uint32_t surfaceOrdinal,
    std::uint32_t vertexCount,
    std::uint32_t indexCount);
void NotePathTraceCaptureSerialInstanceObservation(
    std::uint32_t surfaceOrdinal,
    std::uint64_t instanceId,
    std::uint64_t meshHash,
    const float currentObjectToWorld[16]);
void NotePathTraceCaptureSerialRigidCandidate(
    std::uint32_t surfaceOrdinal,
    std::uint64_t meshHash,
    std::uint64_t instanceId,
    std::uint32_t materialId);
RtPathTraceCaptureOracle FinishPathTraceCaptureSerialOracle(
    const std::vector<PathTraceSmokeVertex>& vertices,
    const std::vector<std::uint32_t>& indexes,
    const std::vector<std::uint32_t>& triangleClasses,
    const std::vector<std::uint32_t>& triangleMaterials,
    const std::vector<std::uint32_t>& triangleInstances,
    const std::vector<std::uint32_t>& triangleIdentities);
