#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCaptureProduct.h"

#include "PathTraceCommittedCapture.h"
#include "PathTraceCommittedBaseline.h"
#include "PathTraceGeometryUniverse.h"
#include "PathTraceGeometryAdmissionPlan.h"
#include "PathTraceInstanceUniverse.h"
#include "PathTraceMaterialIdKernel.h"
#include "PathTraceProducerLaneContract.h"
#include "PathTraceRigidIdentity.h"
#include "PathTraceCpuProducerPublish.h"
#include "PathTraceTextureRegistry.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <type_traits>

namespace
{

uint32_t CaptureSurfaceClassAndSubtypeId(
    RtSmokeSurfaceClass surfaceClass,
    RtSmokeTranslucentSubtype subtype)
{
    uint32_t id = SmokeSurfaceClassId(surfaceClass);
    if (surfaceClass == RtSmokeSurfaceClass::ParticleAlpha)
    {
        id |= (SmokeTranslucentSubtypeId(subtype) <<
            RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT) &
            RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK;
    }
    return id;
}

bool CommittedApplyGateShouldSkip(
    const RtPtCpuProducerApplyGateSnapshot& gate,
    int entityIndex,
    int modelSurfaceIndex)
{
    if (!gate.enabled || !gate.acceptedSkinnedBuildLive || entityIndex < 0)
    {
        return false;
    }
    const uint32_t surface = modelSurfaceIndex >= 0
        ? static_cast<uint32_t>(modelSurfaceIndex) : UINT32_MAX;
    const uint64_t key =
        (static_cast<uint64_t>(static_cast<uint32_t>(entityIndex)) << 32) |
        static_cast<uint64_t>(surface);
    return key != 0 && std::binary_search(gate.omittedSkinKeys.begin(),
        gate.omittedSkinKeys.end(), key);
}

RtPathTraceCaptureOracle g_serialOracle;
std::uint32_t g_serialOracleSurface = 0;
bool g_serialOracleActive = false;
const RtPathTraceCaptureRawSurface* g_serialOracleRawSurfaces = nullptr;
std::size_t g_serialOracleRawSurfaceCount = 0;
const RtPathTraceCaptureModelTokenTablePod* g_serialOracleModelTables = nullptr;
std::size_t g_serialOracleModelTableCount = 0;
const std::uint64_t* g_serialOracleModelSurfaceTokens = nullptr;
std::size_t g_serialOracleModelSurfaceTokenCount = 0;
RtPathTracePlanningSnapshotEpoch g_serialOracleInstanceEpoch;
const RtPathTraceInstanceHistoryPod* g_serialOracleInstanceHistories = nullptr;
std::size_t g_serialOracleInstanceHistoryCount = 0;

void ClearSerialOracleSnapshotView()
{
    g_serialOracleRawSurfaces = nullptr;
    g_serialOracleRawSurfaceCount = 0;
    g_serialOracleModelTables = nullptr;
    g_serialOracleModelTableCount = 0;
    g_serialOracleModelSurfaceTokens = nullptr;
    g_serialOracleModelSurfaceTokenCount = 0;
    g_serialOracleInstanceEpoch = {};
    g_serialOracleInstanceHistories = nullptr;
    g_serialOracleInstanceHistoryCount = 0;
}

bool CaptureModelTokenSpan(
    const RtPathTraceCaptureRawSurface& raw,
    const RtPathTraceCaptureModelTokenTablePod* tables,
    std::size_t tableCount,
    const std::uint64_t* tokens,
    std::size_t tokenCount,
    const std::uint64_t*& surfaceTokens,
    std::size_t& surfaceTokenCount)
{
    surfaceTokens = nullptr;
    surfaceTokenCount = 0;
    if (raw.modelTableIndex == RT_PT_CAPTURE_INVALID_MODEL_TABLE)
    {
        return raw.modelBits == 0;
    }
    if (tables == nullptr || raw.modelTableIndex >= tableCount)
    {
        return false;
    }
    const RtPathTraceCaptureModelTokenTablePod& table =
        tables[raw.modelTableIndex];
    if (table.modelBits != raw.modelBits || table.modelEpoch != raw.modelEpoch ||
        table.tokenOffset > tokenCount ||
        table.tokenCount > tokenCount - table.tokenOffset ||
        (table.tokenCount != 0 && tokens == nullptr))
    {
        return false;
    }
    surfaceTokens = table.tokenCount != 0 ? tokens + table.tokenOffset : nullptr;
    surfaceTokenCount = table.tokenCount;
    return true;
}

template<typename T>
bool OracleCanAppend(const std::vector<T>& values)
{
    if (values.size() < values.capacity())
    {
        return true;
    }
    g_serialOracle.complete = false;
    g_serialOracleActive = false;
    ClearSerialOracleSnapshotView();
    return false;
}

std::uint32_t Hash32(std::uint32_t hash, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
    {
        hash ^= (value >> shift) & 0xffu;
        hash *= 16777619u;
    }
    return hash;
}

std::uint64_t HashBytes64(
    std::uint64_t hash, const void* bytes, std::size_t byteCount)
{
    const std::uint8_t* input = static_cast<const std::uint8_t*>(bytes);
    for (std::size_t index = 0; index < byteCount; ++index)
    {
        hash ^= input[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

bool RuntimeStageEnabled(
    const RtPathTraceRuntimeMaterialStagePod& stage,
    const float* registers,
    std::size_t registerCount)
{
    return stage.valid && (registers == nullptr || stage.conditionRegister < 0 ||
        static_cast<std::size_t>(stage.conditionRegister) >= registerCount ||
        registers[stage.conditionRegister] != 0.0f);
}

bool ParticleCompositeStageSupported(
    const RtPathTraceRuntimeMaterialStagePod& stage,
    std::uint64_t extraGLState)
{
    const std::uint64_t drawState = stage.drawStateBits | extraGLState;
    const std::uint64_t srcBlend = drawState & GLS_SRCBLEND_BITS;
    const std::uint64_t dstBlend = drawState & GLS_DSTBLEND_BITS;
    const bool blendSupported =
        (srcBlend == GLS_SRCBLEND_SRC_ALPHA &&
            (dstBlend == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA ||
                dstBlend == GLS_DSTBLEND_ONE)) ||
        (srcBlend == GLS_SRCBLEND_ONE && dstBlend == GLS_DSTBLEND_ONE) ||
        (srcBlend == GLS_SRCBLEND_ZERO &&
            dstBlend == GLS_DSTBLEND_ONE_MINUS_SRC_COLOR);
    return stage.lighting == SL_AMBIENT && stage.texgen == TG_EXPLICIT &&
        stage.imageDynamic == DI_STATIC && !stage.cinematic && !stage.program &&
        stage.imagePresent && blendSupported;
}

std::uint32_t ParticleCompositeRouteFromRaw(
    const RtPathTraceCaptureRawSurface& raw,
    const RtSmokeTranslucentClassifierInfo& classifier,
    const RtPathTraceRuntimeMaterialStagePod* stages,
    const float* registers)
{
    if (!raw.particleCompositeEnabled ||
        raw.safety != RtPathTraceCaptureSafetyDisposition::Ready ||
        raw.vertexCount < 3 || raw.sourceTriIndexCount < 3 ||
        (raw.sourceTriIndexCount % 3) != 0)
    {
        return 0;
    }
    const bool explicitEffectContext = raw.weaponDepthHack ||
        raw.allowSurfaceInView || idStr::FindText(raw.modelName, ".prt", false) >= 0;
    const int deform = raw.classify.material.deform;
    const bool supportedDeform = deform == DFRM_PARTICLE ||
        deform == DFRM_PARTICLE2 || deform == DFRM_SPRITE ||
        deform == DFRM_TUBE || deform == DFRM_FLARE;
    std::uint32_t supportedStageCount = 0;
    bool hasActiveNonCompositeStage = false;
    for (std::uint32_t index = 0; index < raw.runtimeStageCount; ++index)
    {
        const RtPathTraceRuntimeMaterialStagePod& stage = stages[index];
        if (!RuntimeStageEnabled(stage, registers, raw.registerCount))
        {
            continue;
        }
        if (ParticleCompositeStageSupported(stage, raw.extraGLState))
        {
            ++supportedStageCount;
        }
        else
        {
            hasActiveNonCompositeStage = true;
        }
    }
    const bool accepted =
        (supportedDeform || classifier.nameLooksParticle || explicitEffectContext) &&
        supportedStageCount > 0 && !raw.classify.material.guiSurface &&
        !classifier.nameLooksGui && !classifier.nameLooksGlass &&
        !classifier.sortIsDecal && !classifier.polygonOffsetDecal &&
        !classifier.nameLooksDecal && !classifier.sortIsPostProcess &&
        !classifier.sortIsGuiOrSubview && !classifier.hasScreenTexgen;
    return accepted && !hasActiveNonCompositeStage ? 1u : 0u;
}

bool RegistryMaterialHasLiquidPool(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    std::uint32_t materialId)
{
    const auto found = std::lower_bound(snapshot.registryMaterials.begin(),
        snapshot.registryMaterials.end(), materialId,
        [](const auto& lhs, std::uint32_t rhs)
        {
            return lhs.materialId < rhs;
        });
    return found != snapshot.registryMaterials.end() &&
        found->materialId == materialId && found->detailDecalLiquidPool;
}

} // namespace

bool BeginPathTraceCaptureSerialOracle(
    const RtPathTracePlanningSnapshotEpoch& epoch,
    std::uint64_t viewIdentity,
    const RtPathTraceCaptureCapacityCounts& counts,
    std::size_t snapshotAndProductOwnedBytes,
    std::size_t* reservedOracleBytes)
{
    if (reservedOracleBytes)
    {
        *reservedOracleBytes = 0;
    }
    g_serialOracle.ResetAndRelease();
    g_serialOracleActive = false;
    ClearSerialOracleSnapshotView();
    if (!ReservePathTraceCaptureOracleStorage(counts,
            snapshotAndProductOwnedBytes, g_serialOracle))
    {
        return false;
    }
    g_serialOracle.epoch = epoch;
    g_serialOracle.viewIdentity = viewIdentity;
    g_serialOracleSurface = 0;
    g_serialOracleActive = RtPathTracePlanningEpochValid(epoch);
    if (g_serialOracleActive && reservedOracleBytes)
    {
        *reservedOracleBytes = g_serialOracle.OwnedBytes();
    }
    return g_serialOracleActive;
}

bool SeedPathTraceCaptureSerialOracleSources(
    const RtPathTraceCaptureOwnerSnapshot& snapshot)
{
    if (!g_serialOracleActive ||
        snapshot.epoch.generation != g_serialOracle.epoch.generation ||
        snapshot.viewIdentity != g_serialOracle.viewIdentity ||
        snapshot.surfaces.size() > g_serialOracle.surfaces.capacity())
    {
        g_serialOracleActive = false;
        ClearSerialOracleSnapshotView();
        g_serialOracle.ResetAndRelease();
        return false;
    }
    g_serialOracle.surfaces.resize(snapshot.surfaces.size());
    g_serialOracleRawSurfaces = snapshot.surfaces.data();
    g_serialOracleRawSurfaceCount = snapshot.surfaces.size();
    g_serialOracleModelTables = snapshot.modelTables.data();
    g_serialOracleModelTableCount = snapshot.modelTables.size();
    g_serialOracleModelSurfaceTokens = snapshot.modelSurfaceTokens.data();
    g_serialOracleModelSurfaceTokenCount = snapshot.modelSurfaceTokens.size();
    g_serialOracleInstanceEpoch = snapshot.instanceUniverse.epoch;
    g_serialOracleInstanceHistories = snapshot.instanceUniverse.histories.data();
    g_serialOracleInstanceHistoryCount = snapshot.instanceUniverse.histories.size();
    for (std::size_t index = 0; index < snapshot.surfaces.size(); ++index)
    {
        RtPathTraceCaptureOracleSurface& destination =
            g_serialOracle.surfaces[index];
        destination.ordinal = static_cast<std::uint32_t>(index);
        destination.sourceState = static_cast<std::uint32_t>(
            snapshot.surfaces[index].safety);
        destination.decisionPresence = RT_PT_CAPTURE_DECISION_FILTER;
    }
    return true;
}

bool BuildPathTraceCaptureSerialRuntimeMaterialVariantKey(
    std::uint32_t baseMaterialId,
    std::int32_t entityIndex,
    std::int32_t entityNum,
    RtPathTraceRuntimeMaterialVariantPod& key,
    bool& snapshotOracleActive)
{
    key = {};
    snapshotOracleActive = g_serialOracleActive;
    if (!snapshotOracleActive)
    {
        return false;
    }
    if (g_serialOracleSurface >= g_serialOracleRawSurfaceCount ||
        g_serialOracleRawSurfaces == nullptr)
    {
        g_serialOracle.complete = false;
        g_serialOracleActive = false;
        ClearSerialOracleSnapshotView();
        return false;
    }
    const RtPathTraceCaptureRawSurface& raw =
        g_serialOracleRawSurfaces[g_serialOracleSurface];
    const std::uint64_t* tokens = nullptr;
    std::size_t tokenCount = 0;
    if (!CaptureModelTokenSpan(raw,
            g_serialOracleModelTables, g_serialOracleModelTableCount,
            g_serialOracleModelSurfaceTokens,
            g_serialOracleModelSurfaceTokenCount, tokens, tokenCount))
    {
        g_serialOracle.complete = false;
        g_serialOracleActive = false;
        ClearSerialOracleSnapshotView();
        return false;
    }
    key = BuildPathTraceRuntimeMaterialVariantKeyFromPod(
        baseMaterialId, entityIndex, entityNum,
        raw.requestedModelSurfaceIndex, raw.currentTriToken,
        tokens, tokenCount);
    return true;
}

bool ResolvePathTraceCaptureSerialRigidIdentitySurfaceFromSnapshot(
    std::uint64_t modelBits,
    std::uint64_t modelEpoch,
    std::int32_t requestedModelSurfaceIndex,
    std::uint64_t currentTriToken,
    std::int32_t& resolvedModelSurfaceIndex,
    bool& resolvedSurfaceMatchesCurrent,
    bool& comparedCall)
{
    resolvedModelSurfaceIndex = -1;
    resolvedSurfaceMatchesCurrent = false;
    comparedCall = false;
    if (!g_serialOracleActive || g_serialOracleRawSurfaces == nullptr ||
        g_serialOracleSurface >= g_serialOracleRawSurfaceCount)
    {
        return false;
    }
    const RtPathTraceCaptureRawSurface& raw =
        g_serialOracleRawSurfaces[g_serialOracleSurface];
    comparedCall = true;
    const std::uint64_t* tokens = nullptr;
    std::size_t tokenCount = 0;
    const bool tokenSpanValid = CaptureModelTokenSpan(raw,
        g_serialOracleModelTables, g_serialOracleModelTableCount,
        g_serialOracleModelSurfaceTokens,
        g_serialOracleModelSurfaceTokenCount, tokens, tokenCount);
    const RtPathTraceSerialRigidSnapshotResult result =
        PlanPathTraceSerialRigidSnapshotUseFromPod(
            true,
            raw.modelBits, raw.modelEpoch,
            raw.requestedModelSurfaceIndex, raw.currentTriToken,
            modelBits, modelEpoch, requestedModelSurfaceIndex, currentTriToken,
            tokenSpanValid, tokens, tokenCount);
    resolvedModelSurfaceIndex = result.resolvedModelSurfaceIndex;
    resolvedSurfaceMatchesCurrent = result.resolvedSurfaceMatchesCurrent;
    if (!result.useSnapshot)
    {
        g_serialOracle.complete = false;
        g_serialOracleActive = false;
        ClearSerialOracleSnapshotView();
        return false;
    }
    return true;
}

std::uint32_t PathTraceCaptureSerialOracleSourceState(
    std::uint32_t surfaceOrdinal)
{
    return g_serialOracleActive && surfaceOrdinal < g_serialOracle.surfaces.size()
        ? g_serialOracle.surfaces[surfaceOrdinal].sourceState : 0;
}

void CancelPathTraceCaptureSerialOracle()
{
    g_serialOracleActive = false;
    g_serialOracleSurface = 0;
    ClearSerialOracleSnapshotView();
    g_serialOracle.ResetAndRelease();
}

void SetPathTraceCaptureSerialOracleSurface(std::uint32_t surfaceOrdinal)
{
    if (g_serialOracleActive)
    {
        g_serialOracleSurface = surfaceOrdinal;
    }
}

void NotePathTraceCaptureSerialTerminal(
    std::uint32_t surfaceOrdinal,
    RtPathTraceCaptureTerminal terminal)
{
    if (!g_serialOracleActive)
    {
        return;
    }
    if (surfaceOrdinal >= g_serialOracle.surfaces.capacity())
    {
        g_serialOracleActive = false;
        g_serialOracle.complete = false;
        return;
    }
    if (g_serialOracle.surfaces.size() <= surfaceOrdinal)
    {
        const std::size_t oldSize = g_serialOracle.surfaces.size();
        g_serialOracle.surfaces.resize(
            static_cast<std::size_t>(surfaceOrdinal) + 1);
        for (std::size_t index = oldSize; index < g_serialOracle.surfaces.size(); ++index)
        {
            g_serialOracle.surfaces[index].ordinal =
                static_cast<std::uint32_t>(index);
        }
    }
    g_serialOracle.surfaces[surfaceOrdinal].terminal = terminal;
}

void NotePathTraceCaptureSerialDerivedSurface(
    std::uint32_t surfaceOrdinal,
    RtSmokeSurfaceClass surfaceClass,
    RtSmokeTranslucentSubtype translucentSubtype,
    std::uint32_t surfaceClassId,
    std::uint32_t materialId,
    std::uint32_t materialClassSignature)
{
    if (!g_serialOracleActive)
    {
        return;
    }
    NotePathTraceCaptureSerialTerminal(
        surfaceOrdinal, RtPathTraceCaptureTerminal::SemanticRejected);
    if (!g_serialOracleActive)
    {
        return;
    }
    RtPathTraceCaptureOracleSurface& surface =
        g_serialOracle.surfaces[surfaceOrdinal];
    surface.surfaceClass = surfaceClass;
    surface.translucentSubtype = translucentSubtype;
    surface.surfaceClassId = surfaceClassId;
    surface.materialId = materialId;
    surface.materialClassSignature = materialClassSignature;
    surface.decisionPresence |= RT_PT_CAPTURE_DECISION_ROUTE |
        RT_PT_CAPTURE_DECISION_IDENTITY;
}

void NotePathTraceCaptureSerialAppend(
    std::uint32_t surfaceOrdinal,
    std::int32_t invalidIndexCount,
    std::int32_t zeroAreaTriangleCount,
    std::uint32_t emittedIndexCount)
{
    if (!g_serialOracleActive)
    {
        return;
    }
    NotePathTraceCaptureSerialTerminal(
        surfaceOrdinal, RtPathTraceCaptureTerminal::RolledBack);
    if (!g_serialOracleActive)
    {
        return;
    }
    RtPathTraceCaptureOracleSurface& surface =
        g_serialOracle.surfaces[surfaceOrdinal];
    surface.invalidIndexCount = invalidIndexCount;
    surface.zeroAreaTriangleCount = zeroAreaTriangleCount;
    surface.indexCount = emittedIndexCount;
    surface.triangleCount = emittedIndexCount / 3;
    surface.decisionPresence |= RT_PT_CAPTURE_DECISION_APPEND;
    surface.appendAttempted = true;
    surface.rollbackApplied = emittedIndexCount == 0;
}

void NotePathTraceCaptureSerialRuntimeMaterial(
    std::uint32_t surfaceOrdinal,
    const RtPathTraceRuntimeMaterialDecisionPod& decision)
{
    if (!g_serialOracleActive)
    {
        return;
    }
    if (surfaceOrdinal == UINT32_MAX)
    {
        surfaceOrdinal = g_serialOracleSurface;
    }
    NotePathTraceCaptureSerialTerminal(
        surfaceOrdinal, RtPathTraceCaptureTerminal::SemanticRejected);
    if (!g_serialOracleActive)
    {
        return;
    }
    RtPathTraceCaptureOracleSurface& surface =
        g_serialOracle.surfaces[surfaceOrdinal];
    surface.runtimeMaterial = decision;
    surface.decisionPresence |= RT_PT_CAPTURE_DECISION_RUNTIME_MATERIAL;
}

void NotePathTraceCaptureSerialDecision(
    std::uint32_t surfaceOrdinal,
    const RtPathTraceCaptureSurfaceProduct& decision,
    std::uint64_t presenceMask)
{
    if (!g_serialOracleActive)
    {
        return;
    }
    NotePathTraceCaptureSerialTerminal(surfaceOrdinal, decision.terminal);
    if (!g_serialOracleActive)
    {
        return;
    }
    RtPathTraceCaptureOracleSurface& destination =
        g_serialOracle.surfaces[surfaceOrdinal];
    destination.ordinal = surfaceOrdinal;
    destination.terminal = decision.terminal;
    if ((presenceMask & RT_PT_CAPTURE_DECISION_FILTER) != 0)
    {
        destination.compositeRoute = decision.compositeRoute;
        destination.alphaDiagnosticRejected = decision.alphaDiagnosticRejected;
        destination.applyGateSkip = decision.applyGateSkip;
        destination.sourceState = decision.sourceState;
    }
    if ((presenceMask & (RT_PT_CAPTURE_DECISION_ROUTE |
            RT_PT_CAPTURE_DECISION_IDENTITY)) != 0)
    {
        destination.surfaceClass = decision.surfaceClass;
        destination.translucentSubtype = decision.translucentSubtype;
        destination.surfaceClassId = decision.surfaceClassId;
        destination.materialId = decision.materialId;
        destination.materialClassSignature = decision.materialClassSignature;
        destination.sourceFlags = decision.sourceFlags;
        destination.meshHash = decision.meshHash;
        destination.instanceId = decision.instanceId;
        destination.staticMatch = decision.staticMatch;
        destination.rigidReadyByMesh = decision.rigidReadyByMesh;
        destination.rigidReadyByResident = decision.rigidReadyByResident;
        destination.liquidPoolPromoted = decision.liquidPoolPromoted;
    }
    if ((presenceMask & RT_PT_CAPTURE_DECISION_SKINNED) != 0)
    {
        const bool newlyOmitted = decision.skinnedCaptureOmitted &&
            !destination.skinnedCaptureOmitted;
        destination.skinnedAdmission = decision.skinnedAdmission;
        destination.skinnedCaptureOmitted = decision.skinnedCaptureOmitted;
        if (newlyOmitted)
        {
            ++g_serialOracle.liveCardinality.skinnedOmittedSurfaces;
            g_serialOracle.liveCardinality.skinnedOmittedVertices +=
                decision.skinnedRecordVertexCount;
            g_serialOracle.liveCardinality.skinnedOmittedIndexes +=
                decision.skinnedRecordIndexCount;
        }
    }
    if ((presenceMask & RT_PT_CAPTURE_DECISION_ADMISSION) != 0)
    {
        destination.admissionPrecheckPassed = decision.admissionPrecheckPassed;
    }
    if ((presenceMask & RT_PT_CAPTURE_DECISION_APPEND) != 0)
    {
        destination.sourceState = decision.sourceState;
        destination.bucketIndex = decision.bucketIndex;
        destination.preAppendVertexOffset = decision.preAppendVertexOffset;
        destination.preAppendIndexOffset = decision.preAppendIndexOffset;
        destination.preAppendTriangleOffset = decision.preAppendTriangleOffset;
        destination.vertexOffset = decision.vertexOffset;
        destination.vertexCount = decision.vertexCount;
        destination.indexOffset = decision.indexOffset;
        destination.indexCount = decision.indexCount;
        destination.triangleOffset = decision.triangleOffset;
        destination.triangleCount = decision.triangleCount;
        destination.invalidIndexCount = decision.invalidIndexCount;
        destination.zeroAreaTriangleCount = decision.zeroAreaTriangleCount;
        destination.appendAttempted = decision.appendAttempted;
        destination.rollbackApplied = decision.rollbackApplied;
    }
    if ((presenceMask & RT_PT_CAPTURE_DECISION_CAPTURED_RECORD) != 0)
    {
        destination.capturedRecordPresent = decision.capturedRecordPresent;
        destination.capturedRecordVertexCount = decision.capturedRecordVertexCount;
        destination.capturedRecordIndexCount = decision.capturedRecordIndexCount;
        destination.capturedRecordTriangleCount = decision.capturedRecordTriangleCount;
    }
    if ((presenceMask & RT_PT_CAPTURE_DECISION_SKINNED_RECORD) != 0)
    {
        destination.skinnedRecordPresent = decision.skinnedRecordPresent;
        destination.skinnedRecordVertexCount = decision.skinnedRecordVertexCount;
        destination.skinnedRecordIndexCount = decision.skinnedRecordIndexCount;
        destination.skinnedRecordTriangleCount = decision.skinnedRecordTriangleCount;
    }
    if ((presenceMask & RT_PT_CAPTURE_DECISION_MERGED_WALK) != 0)
    {
        destination.mergedWalkRecordPresent = decision.mergedWalkRecordPresent;
        destination.mergedWalkVertexCount = decision.mergedWalkVertexCount;
        destination.mergedWalkIndexCount = decision.mergedWalkIndexCount;
        destination.mergedWalkTriangleCount = decision.mergedWalkTriangleCount;
        destination.mergedCompanionRigidId = decision.mergedCompanionRigidId;
        destination.mergedCompanionSkinnedId = decision.mergedCompanionSkinnedId;
    }
    if ((presenceMask & RT_PT_CAPTURE_DECISION_BUCKET_PUBLICATION) != 0)
    {
        destination.bucketRangePublished = decision.bucketRangePublished;
    }
    destination.decisionPresence |= presenceMask;
}

void NotePathTraceCaptureSerialMaterialVariant(
    std::uint32_t baseMaterialId,
    std::uint32_t initialCandidate,
    std::uint32_t chosenId,
    std::uint32_t collisionCount,
    bool fallbackUsed,
    const char* materialName)
{
    if (!g_serialOracleActive || initialCandidate == baseMaterialId)
    {
        return;
    }
    RtPathTraceMaterialInfoRegistrationIntent intent;
    intent.generation = g_serialOracle.epoch.generation;
    intent.surfaceOrdinal = g_serialOracleSurface;
    intent.baseMaterialId = baseMaterialId;
    RtPathTracePlanningCopyName(intent.materialName,
        sizeof(intent.materialName), materialName);
    intent.reason = 1;
    intent.callPresent = true;
    if (!OracleCanAppend(g_serialOracle.materialInfoIntents) ||
        !OracleCanAppend(g_serialOracle.materialVariants))
    {
        return;
    }
    g_serialOracle.materialInfoIntents.push_back(intent);
    RtPathTraceMaterialVariantRegistrationProposal proposal;
    proposal.surfaceOrdinal = g_serialOracleSurface;
    proposal.baseMaterialId = baseMaterialId;
    proposal.candidateId = initialCandidate;
    proposal.chosenId = chosenId;
    proposal.collisionCount = collisionCount;
    proposal.fallbackUsed = fallbackUsed;
    g_serialOracle.materialVariants.push_back(proposal);
}

void NotePathTraceCaptureSerialStaticMembership(std::uint32_t surfaceOrdinal)
{
    if (g_serialOracleActive)
    {
        if (!OracleCanAppend(g_serialOracle.staticMembershipSurfaces))
        {
            return;
        }
        g_serialOracle.staticMembershipSurfaces.push_back(surfaceOrdinal);
    }
}

void NotePathTraceCaptureSerialRoutedReady(
    std::uint32_t surfaceOrdinal,
    std::uint32_t vertexCount,
    std::uint32_t indexCount)
{
    if (g_serialOracleActive)
    {
        if (!OracleCanAppend(g_serialOracle.routedReadySkipSurfaces))
        {
            return;
        }
        g_serialOracle.routedReadySkipSurfaces.push_back(surfaceOrdinal);
        ++g_serialOracle.liveCardinality.rigidRemovedSurfaces;
        g_serialOracle.liveCardinality.rigidRemovedVertices += vertexCount;
        g_serialOracle.liveCardinality.rigidRemovedIndexes += indexCount;
    }
}

void NotePathTraceCaptureSerialInstanceObservation(
    std::uint32_t surfaceOrdinal,
    std::uint64_t instanceId,
    std::uint64_t meshHash,
    const float currentObjectToWorld[16])
{
    if (!g_serialOracleActive)
    {
        return;
    }
    if (!OracleCanAppend(g_serialOracle.instanceObservations))
    {
        return;
    }
    RtPathTraceInstanceObservationProposal proposal;
    proposal.surfaceOrdinal = surfaceOrdinal;
    proposal.instanceId = instanceId;
    proposal.meshHash = meshHash;
    const RtPathTraceInstanceHistoryApplication history =
        ApplyInstanceHistoryRowsFromPod(
            g_serialOracleInstanceEpoch,
            g_serialOracleInstanceHistories,
            g_serialOracleInstanceHistoryCount,
            instanceId, currentObjectToWorld,
            g_serialOracle.epoch.frameIndex);
    proposal.hasPrevious = history.hasPreviousObjectToWorld;
    proposal.transformContinuous = history.transformContinuous;
    std::memcpy(proposal.currentObjectToWorld,
        history.currentObjectToWorld, sizeof(proposal.currentObjectToWorld));
    std::memcpy(proposal.previousObjectToWorld,
        history.previousObjectToWorld, sizeof(proposal.previousObjectToWorld));
    g_serialOracle.instanceObservations.push_back(proposal);
}

void NotePathTraceCaptureSerialRigidCandidate(
    std::uint32_t surfaceOrdinal,
    std::uint64_t meshHash,
    std::uint64_t instanceId,
    std::uint32_t materialId)
{
    if (!g_serialOracleActive)
    {
        return;
    }
    if (!OracleCanAppend(g_serialOracle.rigidCandidates))
    {
        return;
    }
    RtPathTraceRigidCandidateProposal proposal;
    proposal.surfaceOrdinal = surfaceOrdinal;
    proposal.meshHash = meshHash;
    proposal.instanceId = instanceId;
    proposal.materialId = materialId;
    g_serialOracle.rigidCandidates.push_back(proposal);
}

RtPathTraceCaptureOracle FinishPathTraceCaptureSerialOracle(
    const std::vector<PathTraceSmokeVertex>& vertices,
    const std::vector<std::uint32_t>& indexes,
    const std::vector<std::uint32_t>& triangleClasses,
    const std::vector<std::uint32_t>& triangleMaterials,
    const std::vector<std::uint32_t>& triangleInstances,
    const std::vector<std::uint32_t>& triangleIdentities)
{
    if (!g_serialOracleActive)
    {
        return RtPathTraceCaptureOracle{};
    }
    if (vertices.size() > g_serialOracle.vertices.capacity() ||
        indexes.size() > g_serialOracle.indexes.capacity() ||
        triangleClasses.size() > g_serialOracle.triangleClasses.capacity() ||
        triangleMaterials.size() > g_serialOracle.triangleMaterials.capacity() ||
        triangleInstances.size() > g_serialOracle.triangleInstances.capacity() ||
        triangleIdentities.size() > g_serialOracle.triangleIdentities.capacity())
    {
        g_serialOracle.ResetAndRelease();
        g_serialOracleActive = false;
        ClearSerialOracleSnapshotView();
        return RtPathTraceCaptureOracle{};
    }
    g_serialOracle.vertices.assign(vertices.begin(), vertices.end());
    g_serialOracle.indexes.assign(indexes.begin(), indexes.end());
    g_serialOracle.triangleClasses.assign(triangleClasses.begin(), triangleClasses.end());
    g_serialOracle.triangleMaterials.assign(triangleMaterials.begin(), triangleMaterials.end());
    g_serialOracle.triangleInstances.clear();
    for (std::uint32_t value : triangleInstances)
    {
        g_serialOracle.triangleInstances.push_back(value);
    }
    g_serialOracle.triangleIdentities.assign(
        triangleIdentities.begin(), triangleIdentities.end());

    std::uint64_t bucketVertexCounts[RT_SMOKE_CLASS_COUNT] = {};
    std::uint64_t bucketIndexCounts[RT_SMOKE_CLASS_COUNT] = {};
    std::uint64_t bucketTriangleCounts[RT_SMOKE_CLASS_COUNT] = {};
    for (const RtPathTraceCaptureOracleSurface& surface : g_serialOracle.surfaces)
    {
        if (surface.terminal != RtPathTraceCaptureTerminal::Accepted ||
            surface.bucketIndex < 0 || surface.bucketIndex >= RT_SMOKE_CLASS_COUNT)
        {
            continue;
        }
        const std::size_t bucket = static_cast<std::size_t>(surface.bucketIndex);
        bucketVertexCounts[bucket] = Max(bucketVertexCounts[bucket],
            static_cast<std::uint64_t>(surface.vertexOffset) + surface.vertexCount);
        bucketIndexCounts[bucket] = Max(bucketIndexCounts[bucket],
            static_cast<std::uint64_t>(surface.indexOffset) + surface.indexCount);
        bucketTriangleCounts[bucket] = Max(bucketTriangleCounts[bucket],
            static_cast<std::uint64_t>(surface.triangleOffset) + surface.triangleCount);
    }
    std::uint64_t dynamicVertexCount = 0;
    std::uint64_t dynamicIndexCount = 0;
    std::uint64_t dynamicTriangleCount = 0;
    for (int bucket = 0; bucket < RT_SMOKE_CLASS_COUNT; ++bucket)
    {
        dynamicVertexCount += bucketVertexCounts[bucket];
        dynamicIndexCount += bucketIndexCounts[bucket];
        dynamicTriangleCount += bucketTriangleCounts[bucket];
    }
    if (dynamicVertexCount > g_serialOracle.vertices.size() ||
        dynamicIndexCount > g_serialOracle.indexes.size() ||
        dynamicTriangleCount > g_serialOracle.triangleClasses.size())
    {
        g_serialOracle.ResetAndRelease();
        g_serialOracleActive = false;
        ClearSerialOracleSnapshotView();
        return RtPathTraceCaptureOracle{};
    }
    std::uint64_t vertexBase = g_serialOracle.vertices.size() - dynamicVertexCount;
    std::uint64_t indexBase = g_serialOracle.indexes.size() - dynamicIndexCount;
    std::uint64_t triangleBase =
        g_serialOracle.triangleClasses.size() - dynamicTriangleCount;
    std::uint64_t bucketVertexBases[RT_SMOKE_CLASS_COUNT] = {};
    std::uint64_t bucketIndexBases[RT_SMOKE_CLASS_COUNT] = {};
    std::uint64_t bucketTriangleBases[RT_SMOKE_CLASS_COUNT] = {};
    for (int bucket = 0; bucket < RT_SMOKE_CLASS_COUNT; ++bucket)
    {
        bucketVertexBases[bucket] = vertexBase;
        bucketIndexBases[bucket] = indexBase;
        bucketTriangleBases[bucket] = triangleBase;
        vertexBase += bucketVertexCounts[bucket];
        indexBase += bucketIndexCounts[bucket];
        triangleBase += bucketTriangleCounts[bucket];
    }
    for (RtPathTraceCaptureOracleSurface& surface : g_serialOracle.surfaces)
    {
        if (surface.terminal != RtPathTraceCaptureTerminal::Accepted ||
            surface.bucketIndex < 0 || surface.bucketIndex >= RT_SMOKE_CLASS_COUNT)
        {
            continue;
        }
        const std::size_t bucket = static_cast<std::size_t>(surface.bucketIndex);
        const std::uint64_t rebasedVertex =
            bucketVertexBases[bucket] + surface.vertexOffset;
        const std::uint64_t rebasedIndex =
            bucketIndexBases[bucket] + surface.indexOffset;
        const std::uint64_t rebasedTriangle =
            bucketTriangleBases[bucket] + surface.triangleOffset;
        if (rebasedVertex > UINT32_MAX || rebasedIndex > UINT32_MAX ||
            rebasedTriangle > UINT32_MAX)
        {
            g_serialOracle.ResetAndRelease();
            g_serialOracleActive = false;
            ClearSerialOracleSnapshotView();
            return RtPathTraceCaptureOracle{};
        }
        surface.vertexOffset = static_cast<std::uint32_t>(rebasedVertex);
        surface.indexOffset = static_cast<std::uint32_t>(rebasedIndex);
        surface.triangleOffset = static_cast<std::uint32_t>(rebasedTriangle);
        surface.preAppendVertexOffset = surface.vertexOffset;
        surface.preAppendIndexOffset = surface.indexOffset;
        surface.preAppendTriangleOffset = surface.triangleOffset;
    }
    g_serialOracle.complete = true;
    g_serialOracleActive = false;
    ClearSerialOracleSnapshotView();
    return std::move(g_serialOracle);
}

bool PlanPathTraceCaptureProductCapacity(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    std::size_t oracleOwnedBytes,
    RtPathTraceCaptureProductCapacityPlan& plan)
{
    if (!PlanPathTraceCompleteSlotCapacity(snapshot.capacityCounts,
            sizeof(snapshot), plan))
    {
        return false;
    }
    const std::size_t cap = RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES;
    std::size_t actual = snapshot.OwnedBytes();
    return RtPathTraceProducerCheckedAddBytes(actual, plan.candidateBytes, cap) &&
        RtPathTraceProducerCheckedAddBytes(actual,
            Max(plan.oracleBytes, oracleOwnedBytes), cap);
}

bool BuildPathTraceOwnerDecisionsFromFinalizedSemantic(
    RtPathTraceCaptureOwnerSnapshot& snapshot,
    const RtPathTraceCommittedPlanningBaseline& baseline)
{
    if (!snapshot.complete || !baseline.Valid() ||
        !RtPathTraceCommittedBaselineMayDispatch(baseline.dispatchGuard) ||
        snapshot.surfaces.size() > snapshot.ownerDecisions.capacity())
    {
        return false;
    }
    snapshot.ownerDecisions.clear();
    RtSmokeGeometryAdmissionBudget admissionBudget;
    admissionBudget.maxBytes = snapshot.admissionMaxBytes;
    admissionBudget.maxSurfaces = snapshot.admissionMaxSurfaces;
    std::uint64_t admissionBytes = 0;
    std::uint64_t admissionSurfaces = 0;

    const auto publish = [&](RtPathTraceCaptureSurfaceProduct& surface)
    {
        FinalizePathTraceCaptureSurfaceDecision(surface);
        snapshot.ownerDecisions.push_back(surface);
    };

    for (const RtPathTraceCaptureRawSurface& raw : snapshot.surfaces)
    {
        RtPathTraceCaptureSurfaceProduct surface;
        surface.ordinal = raw.ordinal;
        surface.sourceState = static_cast<std::uint32_t>(raw.safety);
        surface.decisionPresence = RT_PT_CAPTURE_DECISION_FILTER;
        if (raw.ordinal != snapshot.ownerDecisions.size() ||
            !RtPathTraceCaptureSemanticFactsReadable(raw))
        {
            return false;
        }
        if (raw.safety != RtPathTraceCaptureSafetyDisposition::Ready)
        {
            surface.terminal = RtPathTraceCaptureTerminal::SafetyRejected;
            publish(surface);
            continue;
        }

        const std::uint64_t* modelSurfaceTokens = nullptr;
        std::size_t modelSurfaceTokenCount = 0;
        if (!CaptureModelTokenSpan(raw, snapshot.modelTables.data(),
                snapshot.modelTables.size(), snapshot.modelSurfaceTokens.data(),
                snapshot.modelSurfaceTokens.size(), modelSurfaceTokens,
                modelSurfaceTokenCount))
        {
            surface.terminal = RtPathTraceCaptureTerminal::SemanticRejected;
            publish(surface);
            continue;
        }
        surface.applyGateSkip = CommittedApplyGateShouldSkip(
            baseline.applyGate, raw.entityIndex,
            raw.requestedModelSurfaceIndex);
        if (surface.applyGateSkip)
        {
            surface.terminal = RtPathTraceCaptureTerminal::ApplyGateSkip;
            publish(surface);
            continue;
        }
        if (raw.classifierStageOffset > snapshot.classifierStages.size() ||
            raw.classifierStageCount > snapshot.classifierStages.size() -
                raw.classifierStageOffset ||
            raw.runtimeStageOffset > snapshot.runtimeStages.size() ||
            raw.runtimeStageCount > snapshot.runtimeStages.size() -
                raw.runtimeStageOffset ||
            raw.registerOffset > snapshot.registers.size() ||
            raw.registerCount > snapshot.registers.size() - raw.registerOffset)
        {
            return false;
        }
        RtSmokeSurfaceClassifyInput classifyInput = raw.classify;
        for (std::uint32_t stageIndex = 0;
            stageIndex < raw.runtimeStageCount; ++stageIndex)
        {
            classifyInput.material.hasAlphaTest =
                classifyInput.material.hasAlphaTest ||
                snapshot.runtimeStages[raw.runtimeStageOffset + stageIndex].hasAlphaTest;
        }
        bool hasActiveStage = classifyInput.material.materialPresent;
        if (raw.registersPresent)
        {
            hasActiveStage = false;
            for (std::uint32_t stageIndex = 0;
                stageIndex < raw.classifierStageCount; ++stageIndex)
            {
                const auto& stage = snapshot.classifierStages[
                    raw.classifierStageOffset + stageIndex];
                if (!stage.valid)
                {
                    continue;
                }
                const int reg = stage.conditionRegister;
                const float condition = reg >= 0 &&
                    static_cast<std::uint32_t>(reg) < raw.registerCount
                    ? snapshot.registers[raw.registerOffset + reg] : 1.0f;
                if (condition != 0.0f)
                {
                    hasActiveStage = true;
                    break;
                }
            }
        }
        if (!hasActiveStage ||
            (!raw.guiAllowed && classifyInput.material.guiSurface) ||
            !raw.callbackAllowed)
        {
            surface.terminal = RtPathTraceCaptureTerminal::SemanticRejected;
            publish(surface);
            continue;
        }
        const RtSmokeTranslucentClassifierInfo classifier =
            BuildSmokeTranslucentClassifierInfo(raw.classifier,
                raw.classifierStageCount != 0
                    ? snapshot.classifierStages.data() + raw.classifierStageOffset
                    : nullptr,
                static_cast<std::int32_t>(raw.classifierStageCount));
        surface.compositeRoute = ParticleCompositeRouteFromRaw(raw, classifier,
            raw.runtimeStageCount != 0
                ? snapshot.runtimeStages.data() + raw.runtimeStageOffset : nullptr,
            raw.registersPresent && raw.registerCount != 0
                ? snapshot.registers.data() + raw.registerOffset : nullptr);
        if (surface.compositeRoute != 0)
        {
            surface.terminal = RtPathTraceCaptureTerminal::SemanticRejected;
            publish(surface);
            continue;
        }
        surface.alphaDiagnosticRejected = raw.unifiedPtEnabled &&
            raw.removeAlphaClipEnabled && classifyInput.material.hasAlphaTest;
        if (surface.alphaDiagnosticRejected ||
            !raw.derivedRuntimeMaterialPresent)
        {
            surface.terminal = RtPathTraceCaptureTerminal::SemanticRejected;
            publish(surface);
            continue;
        }

        const RtSmokeSurfaceClass classified =
            ClassifySmokeSurfaceFromPod(classifyInput, classifier);
        const bool commonPromotion =
            classified == RtSmokeSurfaceClass::ParticleAlpha &&
            classifyInput.hasEntityDef && !classifyInput.material.guiSurface &&
            !classifyInput.hasJointCache &&
            !classifyInput.hasStaticModelWithJoints &&
            !classifyInput.hasRenderEntityJoints &&
            !raw.entityCallbackPresent && !raw.entityForceUpdate &&
            !raw.dynamicModelPresent && !raw.cachedDynamicModelPresent &&
            classifyInput.material.deform == DFRM_NONE &&
            classifyInput.modelDepthHack == 0.0f;
        const bool promotedEmissive = snapshot.rigidRouteEmissiveCards &&
            commonPromotion && SmokeMaterialCanPromoteRigidEmissiveCardFromPod(
                classifyInput.material, false, classifier);
        const bool promotedLiquid = commonPromotion && raw.liquidPoolEnabled &&
            RegistryMaterialHasLiquidPool(snapshot, raw.derivedBaseMaterialId);
        surface.surfaceClass = promotedEmissive || promotedLiquid
            ? RtSmokeSurfaceClass::RigidEntity : classified;
        surface.translucentSubtype = ClassifySmokeTranslucentSubtypeFromPod(
            classifyInput.material, classifier);
        surface.surfaceClassId = CaptureSurfaceClassAndSubtypeId(
            surface.surfaceClass, surface.translucentSubtype);
        surface.materialClassSignature =
            SmokeMaterialRouteClassSignatureFromPod(classifyInput.material,
                surface.surfaceClass, surface.translucentSubtype, classifier);
        surface.runtimeMaterial = raw.derivedRuntimeMaterial;
        surface.materialId = raw.derivedChosenMaterialId;
        surface.decisionPresence |= RT_PT_CAPTURE_DECISION_RUNTIME_MATERIAL |
            RT_PT_CAPTURE_DECISION_ROUTE | RT_PT_CAPTURE_DECISION_IDENTITY;
        surface.liquidPoolPromoted = promotedLiquid;

        RtPathTraceSourceFlagInput sourceInput;
        sourceInput.surfaceClass = surface.surfaceClass;
        sourceInput.guiSurface = classifyInput.material.guiSurface;
        sourceInput.hasJointCache = classifyInput.hasJointCache;
        sourceInput.hasStaticModelWithJoints =
            classifyInput.hasStaticModelWithJoints;
        sourceInput.hasRenderEntityJoints = classifyInput.hasRenderEntityJoints;
        sourceInput.entityCallbackPresent = raw.entityCallbackPresent;
        sourceInput.entityForceUpdate = raw.entityForceUpdate;
        sourceInput.dynamicModelPresent = raw.dynamicModelPresent;
        sourceInput.cachedDynamicModelPresent = raw.cachedDynamicModelPresent;
        sourceInput.materialDeformed = classifyInput.material.deform != DFRM_NONE;
        sourceInput.customShaderPresent = raw.customShaderPresent;
        sourceInput.customSkinPresent = raw.customSkinPresent;
        surface.sourceFlags = RtPathTraceSourceFlagsFromPod(sourceInput);
        surface.staticMatch = HasStaticSurfaceFromCommittedBaselinePod(
            baseline.geometryUniverse, baseline.epoch,
            RtPathTraceLegacyStaticSurfaceKeyFromRaw(raw));
        if (surface.staticMatch)
        {
            surface.sourceFlags |= RT_PT_INSTANCE_SOURCE_STATIC_CACHE_MATCH;
        }
        if (raw.rigidIdentityPresent)
        {
            surface.meshHash = raw.derivedMeshHash;
            RtPathTraceRigidInstanceIdentityPod identity{};
            identity.meshHash = raw.derivedMeshHash;
            identity.renderWorldIdentity = raw.renderWorldIdentity;
            identity.renderDefIndex = raw.renderDefIndex;
            identity.renderDefGeneration = raw.renderDefGeneration;
            identity.modelEpoch = static_cast<std::uint32_t>(raw.modelEpoch);
            identity.entityIndex = raw.entityIndex;
            identity.renderEntityNum = raw.entityNum;
            identity.modelSurfaceIndex = raw.derivedResolvedModelSurfaceIndex;
            identity.materialId = raw.derivedChosenMaterialId;
            identity.jointIndex = raw.jointIndex;
            surface.instanceId = BuildPathTraceRigidInstanceIdFromPod(identity);
        }
        if (surface.surfaceClass == RtSmokeSurfaceClass::StaticWorld ||
            surface.staticMatch)
        {
            surface.terminal = RtPathTraceCaptureTerminal::StaticMatched;
            publish(surface);
            continue;
        }
        surface.rigidReadyByMesh = raw.rigidReadyByMesh;
        surface.rigidReadyByResident = raw.rigidReadyByResident;
        if (PathTraceCaptureShouldOmitRoutedRigidFromPod(
                snapshot.removeRoutedRigidDynamic, surface.surfaceClass,
                surface.rigidReadyByMesh, surface.rigidReadyByResident))
        {
            surface.terminal = RtPathTraceCaptureTerminal::RoutedRigidReady;
            publish(surface);
            continue;
        }
        if (surface.surfaceClass == RtSmokeSurfaceClass::SkinnedDeformed &&
            ApplyPathTraceSkinnedCaptureAdmissionFromPod(
                raw.skinnedCaptureAdmission, raw.skinnedOmitVertexCount,
                raw.skinnedOmitIndexCount, surface))
        {
            publish(surface);
            continue;
        }
        if (raw.vertexOffset > snapshot.vertices.size() ||
            raw.vertexCount > snapshot.vertices.size() - raw.vertexOffset ||
            raw.indexOffset > snapshot.indexes.size() ||
            raw.indexCount > snapshot.indexes.size() - raw.indexOffset ||
            raw.jointOffset > snapshot.joints.size() ||
            raw.jointCount > snapshot.joints.size() - raw.jointOffset)
        {
            return false;
        }

        std::uint32_t emittedIndexes = 0;
        RtPathTraceCommittedVertexInput vertexInput;
        vertexInput.vertices = snapshot.vertices.data() + raw.vertexOffset;
        vertexInput.joints = raw.jointCount != 0
            ? snapshot.joints.data() + raw.jointOffset : nullptr;
        std::memcpy(vertexInput.modelMatrix, raw.modelMatrix,
            sizeof(vertexInput.modelMatrix));
        std::memcpy(vertexInput.bumpMatrix, raw.bumpMatrix,
            sizeof(vertexInput.bumpMatrix));
        for (std::uint32_t sourceIndex = 0;
            sourceIndex + 2 < raw.indexCount; sourceIndex += 3)
        {
            const triIndex_t i0 = snapshot.indexes[raw.indexOffset + sourceIndex];
            const triIndex_t i1 = snapshot.indexes[raw.indexOffset + sourceIndex + 1];
            const triIndex_t i2 = snapshot.indexes[raw.indexOffset + sourceIndex + 2];
            if (i0 >= raw.vertexCount || i1 >= raw.vertexCount ||
                i2 >= raw.vertexCount)
            {
                continue;
            }
            const PathTraceSmokeVertex v0 =
                BuildPathTraceCommittedVertexFromOwned(vertexInput, i0);
            const PathTraceSmokeVertex v1 =
                BuildPathTraceCommittedVertexFromOwned(vertexInput, i1);
            const PathTraceSmokeVertex v2 =
                BuildPathTraceCommittedVertexFromOwned(vertexInput, i2);
            if (!IsZeroAreaSmokeTriangle(SmokeVertexPosition(v0),
                    SmokeVertexPosition(v1), SmokeVertexPosition(v2)))
            {
                emittedIndexes += 3;
            }
        }
        RtSmokeGeometryAdmissionInput admissionInput;
        admissionInput.currentBytes = admissionBytes;
        admissionInput.currentSurfaces = admissionSurfaces;
        admissionInput.candidateVertexCount = raw.vertexCount;
        admissionInput.candidateIndexCount = emittedIndexes;
        admissionInput.vertexStride = sizeof(PathTraceSmokeVertex);
        admissionInput.indexStride = sizeof(std::uint32_t);
        admissionInput.triangleMetadataStride = sizeof(std::uint32_t) * 4ull;
        const RtSmokeGeometryAdmissionPlan admission =
            BuildSmokeGeometryAdmissionPlan(admissionBudget, admissionInput);
        surface.decisionPresence |= RT_PT_CAPTURE_DECISION_ADMISSION;
        surface.admissionPrecheckPassed = admission.Admitted();
        if (emittedIndexes == 0 || !admission.Admitted())
        {
            surface.terminal = emittedIndexes == 0
                ? RtPathTraceCaptureTerminal::RolledBack
                : RtPathTraceCaptureTerminal::AdmissionRejected;
            surface.rollbackApplied = emittedIndexes == 0;
            publish(surface);
            continue;
        }
        admissionBytes = admission.totalBytes;
        admissionSurfaces = admission.totalSurfaces;
        surface.terminal = RtPathTraceCaptureTerminal::Accepted;
        surface.bucketIndex = static_cast<int>(SmokeSurfaceClassId(
            surface.surfaceClass));
        surface.vertexCount = raw.vertexCount;
        surface.indexCount = raw.indexCount;
        surface.triangleCount = raw.indexCount / 3u;
        publish(surface);
    }
    snapshot.ownerMembershipReceipt = BuildPathTraceCaptureMembershipReceipt(
        snapshot.ownerDecisions.data(), snapshot.ownerDecisions.size());
    snapshot.ownerDecisionsComplete = snapshot.ownerMembershipReceipt.complete;
    return snapshot.ownerDecisionsComplete;
}

bool BuildPathTraceGeometryFromOwnerDecisions(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    RtPathTraceCaptureProduct& candidate)
{
    if (!snapshot.ownerDecisionsComplete ||
        snapshot.ownerDecisions.size() != snapshot.surfaces.size() ||
        !PathTraceCaptureMembershipReceiptsMatch(
            BuildPathTraceCaptureMembershipReceipt(
                snapshot.ownerDecisions.data(), snapshot.ownerDecisions.size()),
            snapshot.ownerMembershipReceipt))
    {
        return false;
    }
    candidate.membershipReceipt = snapshot.ownerMembershipReceipt;
    candidate.ownerDecisionGeometryOnly = true;
    candidate.surfaces.resize(snapshot.surfaces.size());
    for (std::size_t ordinal = 0; ordinal < snapshot.surfaces.size(); ++ordinal)
    {
        const RtPathTraceCaptureRawSurface& raw = snapshot.surfaces[ordinal];
        RtPathTraceCaptureSurfaceProduct surface =
            snapshot.ownerDecisions[ordinal];
        if (raw.ordinal != ordinal || surface.ordinal != ordinal)
        {
            return false;
        }
        if (surface.terminal == RtPathTraceCaptureTerminal::StaticMatched &&
            surface.staticMatch)
        {
            candidate.staticMembershipSurfaces.push_back(raw.ordinal);
        }
        if (surface.terminal == RtPathTraceCaptureTerminal::RoutedRigidReady)
        {
            candidate.routedReadySkipSurfaces.push_back(raw.ordinal);
        }
        if (PathTraceOwnerDecisionEmitsGeometry(surface) &&
            (raw.safety != RtPathTraceCaptureSafetyDisposition::Ready ||
            surface.vertexCount != raw.vertexCount ||
            surface.indexCount != raw.indexCount ||
            raw.vertexOffset > snapshot.vertices.size() ||
            raw.vertexCount > snapshot.vertices.size() - raw.vertexOffset ||
            raw.indexOffset > snapshot.indexes.size() ||
            raw.indexCount > snapshot.indexes.size() - raw.indexOffset ||
            raw.jointOffset > snapshot.joints.size() ||
            raw.jointCount > snapshot.joints.size() - raw.jointOffset ||
            surface.bucketIndex < 0 ||
            surface.bucketIndex >= RT_SMOKE_CLASS_COUNT))
        {
            return false;
        }
        candidate.surfaces[ordinal] = surface;
    }

    // Emit directly in final bucket-major order.  The ordinal surface table is
    // already complete, so no second full geometry product is needed.
    for (int bucket = 0; bucket < RT_SMOKE_CLASS_COUNT; ++bucket)
    {
      for (std::size_t ordinal = 0; ordinal < snapshot.surfaces.size(); ++ordinal)
      {
        const RtPathTraceCaptureRawSurface& raw = snapshot.surfaces[ordinal];
        RtPathTraceCaptureSurfaceProduct& surface = candidate.surfaces[ordinal];
        if (!RtPathTraceProducerPartitionSelects(bucket,
                static_cast<int>(surface.surfaceClass),
                PathTraceOwnerDecisionEmitsGeometry(surface)))
        {
            continue;
        }
        const std::uint32_t vertexStart = static_cast<std::uint32_t>(
            candidate.vertices.size());
        const std::uint32_t indexStart = static_cast<std::uint32_t>(
            candidate.indexes.size());
        surface.vertexOffset = vertexStart;
        surface.indexOffset = indexStart;
        surface.triangleOffset = static_cast<std::uint32_t>(
            candidate.triangleClasses.size());
        surface.preAppendVertexOffset = surface.vertexOffset;
        surface.preAppendIndexOffset = surface.indexOffset;
        surface.preAppendTriangleOffset = surface.triangleOffset;
        surface.invalidIndexCount = 0;
        surface.zeroAreaTriangleCount = 0;
        surface.rollbackApplied = false;

        RtPathTraceCommittedVertexInput vertexInput;
        vertexInput.vertices = snapshot.vertices.data() + raw.vertexOffset;
        vertexInput.joints = raw.jointCount != 0
            ? snapshot.joints.data() + raw.jointOffset : nullptr;
        std::memcpy(vertexInput.modelMatrix, raw.modelMatrix,
            sizeof(vertexInput.modelMatrix));
        std::memcpy(vertexInput.bumpMatrix, raw.bumpMatrix,
            sizeof(vertexInput.bumpMatrix));
        for (std::uint32_t vertexIndex = 0; vertexIndex < raw.vertexCount;
            ++vertexIndex)
        {
            candidate.vertices.push_back(
                BuildPathTraceCommittedVertexFromOwned(vertexInput, vertexIndex));
        }
        const std::uint32_t baseMaterialId =
            HashPathTraceMaterialName(raw.materialName);
        for (std::uint32_t sourceIndex = 0;
            sourceIndex + 2 < raw.indexCount; sourceIndex += 3)
        {
            const triIndex_t i0 = snapshot.indexes[raw.indexOffset + sourceIndex];
            const triIndex_t i1 = snapshot.indexes[raw.indexOffset + sourceIndex + 1];
            const triIndex_t i2 = snapshot.indexes[raw.indexOffset + sourceIndex + 2];
            if (i0 >= raw.vertexCount || i1 >= raw.vertexCount ||
                i2 >= raw.vertexCount)
            {
                ++surface.invalidIndexCount;
                continue;
            }
            if (IsZeroAreaSmokeTriangle(
                    SmokeVertexPosition(candidate.vertices[vertexStart + i0]),
                    SmokeVertexPosition(candidate.vertices[vertexStart + i1]),
                    SmokeVertexPosition(candidate.vertices[vertexStart + i2])))
            {
                ++surface.zeroAreaTriangleCount;
                continue;
            }
            const std::uint32_t emittedTriangleOrdinal =
                static_cast<std::uint32_t>(
                    (candidate.indexes.size() - indexStart) / 3u);
            candidate.indexes.push_back(vertexStart + i0);
            candidate.indexes.push_back(vertexStart + i1);
            candidate.indexes.push_back(vertexStart + i2);
            const bool invalidNormalTriangle =
                !SmokeNormalIsUsable(SmokeVertexNormal(
                    candidate.vertices[vertexStart + i0])) ||
                !SmokeNormalIsUsable(SmokeVertexNormal(
                    candidate.vertices[vertexStart + i1])) ||
                !SmokeNormalIsUsable(SmokeVertexNormal(
                    candidate.vertices[vertexStart + i2]));
            candidate.triangleClasses.push_back(
                BuildPathTraceCaptureTriangleClassWordFromPod(
                    surface.surfaceClassId, raw.activeEmissiveStage,
                    invalidNormalTriangle));
            candidate.triangleMaterials.push_back(surface.materialId);
            candidate.triangleInstances.push_back(static_cast<std::uint64_t>(
                Max(1, raw.entityIndex + 1)));
            candidate.triangleIdentities.push_back(
                BuildPathTraceCaptureTriangleIdentityFromPod(
                    raw.entityIndex, raw.entityNum, baseMaterialId,
                    raw.triIdentityBits, emittedTriangleOrdinal));
        }
        const std::uint32_t emitted = static_cast<std::uint32_t>(
            candidate.indexes.size()) - indexStart;
        if (emitted == 0)
        {
            candidate.vertices.resize(vertexStart);
            surface.vertexOffset = surface.vertexCount = 0;
            surface.indexOffset = surface.indexCount = 0;
            surface.triangleOffset = surface.triangleCount = 0;
            surface.preAppendVertexOffset = 0;
            surface.preAppendIndexOffset = 0;
            surface.preAppendTriangleOffset = 0;
            surface.terminal = RtPathTraceCaptureTerminal::RolledBack;
            surface.rollbackApplied = true;
            surface.capturedRecordPresent = false;
            surface.skinnedRecordPresent = false;
            surface.mergedWalkRecordPresent = false;
            surface.bucketRangePublished = false;
            continue;
        }
        surface.vertexCount = raw.vertexCount;
        surface.indexCount = emitted;
        surface.triangleCount = emitted / 3u;
        surface.decisionPresence |= RT_PT_CAPTURE_DECISION_CAPTURED_RECORD |
            RT_PT_CAPTURE_DECISION_BUCKET_PUBLICATION;
        surface.capturedRecordPresent = true;
        surface.capturedRecordVertexCount = surface.vertexCount;
        surface.capturedRecordIndexCount = surface.indexCount;
        surface.capturedRecordTriangleCount = surface.triangleCount;
        surface.bucketRangePublished = true;
        if (bucket >= 1)
        {
            surface.decisionPresence |= RT_PT_CAPTURE_DECISION_MERGED_WALK;
            surface.mergedWalkRecordPresent = true;
            surface.mergedWalkVertexCount = surface.vertexCount;
            surface.mergedWalkIndexCount = surface.indexCount;
            surface.mergedWalkTriangleCount = surface.triangleCount;
            if (surface.surfaceClass == RtSmokeSurfaceClass::RigidEntity)
            {
                surface.mergedCompanionRigidId = surface.instanceId;
            }
        }
        if (surface.surfaceClass == RtSmokeSurfaceClass::SkinnedDeformed)
        {
            surface.decisionPresence |= RT_PT_CAPTURE_DECISION_SKINNED_RECORD;
            surface.skinnedRecordPresent = true;
            surface.skinnedRecordVertexCount = surface.vertexCount;
            surface.skinnedRecordIndexCount = surface.indexCount;
            surface.skinnedRecordTriangleCount = surface.triangleCount;
        }
      }
    }
    return true;
}

static bool BuildPathTraceOwnerRigidCandidateProposals(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    RtPathTraceCaptureProduct& product)
{
    if (!product.rigidCandidates.empty() ||
        product.surfaces.size() != snapshot.surfaces.size())
    {
        return false;
    }
    for (std::size_t ordinal = 0; ordinal < snapshot.surfaces.size(); ++ordinal)
    {
        const RtPathTraceCaptureRawSurface& raw = snapshot.surfaces[ordinal];
        const RtPathTraceCaptureSurfaceProduct& surface = product.surfaces[ordinal];
        if (!RtPathTraceSourceFlagsAreDurableRigid(surface.sourceFlags))
        {
            continue;
        }
        if (raw.ordinal != ordinal || surface.ordinal != ordinal ||
            surface.meshHash == 0 || surface.materialId == 0 ||
            raw.vertexCount == 0 ||
            raw.sourceTriIndexCount == 0 ||
            (raw.sourceTriIndexCount % 3u) != 0u ||
            raw.triNumVerts == 0 || raw.triNumIndexes == 0 ||
            (raw.triNumIndexes % 3u) != 0u ||
            product.rigidCandidates.size() >=
                product.capacityCounts.rigidCandidateProposals)
        {
            return false;
        }
        RtPathTraceRigidCandidateProposal proposal;
        proposal.surfaceOrdinal = static_cast<std::uint32_t>(ordinal);
        proposal.meshHash = surface.meshHash;
        proposal.instanceId = surface.instanceId;
        proposal.materialId = surface.materialId;
        product.rigidCandidates.push_back(proposal);
    }
    return true;
}

bool BuildPathTraceRigidPreparedPayloads(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    RtPathTraceCaptureProduct& product,
    bool requireReceiptWitness)
{
    if (!product.preparedRigidPayloads.empty() ||
        product.surfaces.size() != snapshot.surfaces.size() ||
        (requireReceiptWitness &&
            product.receiptSurfaces.size() != snapshot.surfaces.size()))
    {
        return false;
    }
    std::size_t preparedVertexCount = 0;
    std::size_t preparedIndexCount = 0;
    for (const RtPathTraceRigidCandidateProposal& proposal :
         product.rigidCandidates)
    {
        if (proposal.surfaceOrdinal >= snapshot.surfaces.size())
        {
            return false;
        }
        const RtPathTraceCaptureRawSurface& raw =
            snapshot.surfaces[proposal.surfaceOrdinal];
        const RtPathTraceCaptureSurfaceProduct& surface =
            product.surfaces[proposal.surfaceOrdinal];
        const bool surfaceIdentityMatches =
            surface.meshHash == proposal.meshHash &&
            surface.instanceId == proposal.instanceId &&
            surface.materialId == proposal.materialId &&
            RtPathTraceSourceFlagsAreDurableRigid(surface.sourceFlags);
        const bool receiptBackedRawIdentityMatches = requireReceiptWitness &&
            raw.derivedMeshHash == proposal.meshHash &&
            raw.derivedChosenMaterialId == proposal.materialId &&
            (surface.meshHash == 0 || surface.meshHash == proposal.meshHash) &&
            (surface.materialId == 0 ||
                surface.materialId == proposal.materialId);
        if (raw.ordinal != proposal.surfaceOrdinal ||
            surface.ordinal != proposal.surfaceOrdinal ||
            proposal.meshHash == 0 || proposal.materialId == 0 ||
            (!surfaceIdentityMatches && !receiptBackedRawIdentityMatches))
        {
            return false;
        }
        if (requireReceiptWitness)
        {
            const RtPathTraceCommittedReferencedSurfaceReceiptRow& receipt =
                product.receiptSurfaces[proposal.surfaceOrdinal];
            if (receipt.persistedMeshHash != proposal.meshHash ||
                receipt.persistedChosenMaterialId != proposal.materialId)
            {
                return false;
            }
        }
        const std::size_t fullVertexCount = raw.triNumVerts;
        const std::size_t fullIndexCount = raw.triNumIndexes;
        if (fullVertexCount == 0 || fullIndexCount == 0 ||
            (fullIndexCount % 3u) != 0u ||
            raw.vertexCount < fullVertexCount ||
            raw.sourceTriIndexCount != fullIndexCount ||
            raw.vertexOffset > snapshot.vertices.size() ||
            fullVertexCount > snapshot.vertices.size() - raw.vertexOffset ||
            raw.indexOffset > snapshot.indexes.size() ||
            fullIndexCount > snapshot.indexes.size() - raw.indexOffset)
        {
            return false;
        }

        bool duplicate = false;
        for (RtPathTraceRigidPreparedPayload& prior :
             product.preparedRigidPayloads)
        {
            duplicate = prior.meshHash == proposal.meshHash;
            if (duplicate)
            {
                if (prior.occurrenceCount == UINT32_MAX) return false;
                ++prior.occurrenceCount;
                break;
            }
        }
        if (duplicate) continue;

        if (product.preparedRigidPayloads.size() >=
                product.preparedRigidPayloads.capacity() ||
            fullVertexCount > product.capacityCounts.preparedRigidVertices ||
            fullIndexCount > product.capacityCounts.preparedRigidIndexes ||
            preparedVertexCount >
                product.capacityCounts.preparedRigidVertices - fullVertexCount ||
            preparedIndexCount >
                product.capacityCounts.preparedRigidIndexes - fullIndexCount)
        {
            return false;
        }

        RtPathTraceRigidPreparedPayload payload;
        payload.surfaceOrdinal = proposal.surfaceOrdinal;
        payload.meshHash = proposal.meshHash;
        payload.instanceId = proposal.instanceId;
        payload.vertexBufferIdentity =
            static_cast<std::uintptr_t>(raw.vertexBufferIdentity);
        payload.indexBufferIdentity =
            static_cast<std::uintptr_t>(raw.indexBufferIdentity);
        payload.sourceFlags = surface.sourceFlags;
        payload.materialId = proposal.materialId;
        payload.materialClassSignature = surface.materialClassSignature;
        payload.surfaceClassId = surface.surfaceClassId;
        payload.triangleClassAndFlags = surface.surfaceClassId |
            (raw.activeEmissiveStage
                ? 0u : RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF);
        payload.vertexFormat = static_cast<std::uint32_t>(
            RtSmokeGeometryBufferFormat::LegacySmokeVertex);
        payload.drawSurfIndex = static_cast<std::int32_t>(proposal.surfaceOrdinal);
        payload.entityIndex = raw.entityIndex;
        payload.renderEntityNum = raw.entityNum;
        payload.modelSurfaceIndex = raw.derivedResolvedModelSurfaceIndex;
        payload.modelEpoch = static_cast<std::uint32_t>(raw.modelEpoch);
        payload.jointIndex = raw.jointIndex;
        payload.fullTriangleVertexCount = raw.triNumVerts;
        payload.fullTriangleIndexCount = raw.triNumIndexes;
        std::memcpy(payload.normalTexMatrix, raw.bumpMatrix,
            sizeof(payload.normalTexMatrix));
        std::memcpy(payload.materialName, raw.materialName,
            sizeof(payload.materialName));
        std::memcpy(payload.modelName, raw.modelName,
            sizeof(payload.modelName));
        payload.materialName[sizeof(payload.materialName) - 1u] = '\0';
        payload.modelName[sizeof(payload.modelName) - 1u] = '\0';
        RtPathTraceRigidOwnedCpuCache cache;
        if (!RtPathTraceBuildRigidOwnedCpuCache(
                snapshot.vertices.data() + raw.vertexOffset, fullVertexCount,
                snapshot.indexes.data() + raw.indexOffset, fullIndexCount,
                payload.normalTexMatrix, cache))
        {
            return false;
        }
        payload.localVertices = std::move(cache.vertices);
        payload.localIndexes = std::move(cache.indexes);
        std::memcpy(payload.triangleBoundsMin, cache.boundsMin,
            sizeof(payload.triangleBoundsMin));
        std::memcpy(payload.triangleBoundsMax, cache.boundsMax,
            sizeof(payload.triangleBoundsMax));
        payload.contentSignature = cache.contentSignature;
        preparedVertexCount += fullVertexCount;
        preparedIndexCount += fullIndexCount;
        product.preparedRigidPayloads.push_back(std::move(payload));
    }
    return product.preparedRigidPayloads.size() <=
            product.capacityCounts.preparedRigidPayloads &&
        preparedVertexCount <= product.capacityCounts.preparedRigidVertices &&
        preparedIndexCount <= product.capacityCounts.preparedRigidIndexes;
}

bool BuildPathTraceCaptureProduct(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    RtPathTraceCaptureProduct& product,
    std::size_t oracleOwnedBytes,
    std::size_t* peakSlotBytes)
{
    if (!snapshot.complete || !snapshot.ownerDecisionsComplete ||
        !RtPathTracePlanningEpochValid(snapshot.epoch) ||
        !RtPathTracePlanningSnapshotsCoherent(snapshot.instanceUniverse,
            snapshot.geometryUniverse) ||
        !RtPathTracePlanningEpochsMatch(snapshot.epoch, snapshot.instanceUniverse.epoch))
    {
        return false;
    }

    RtPathTraceCaptureProductCapacityPlan capacityPlan;
    if (!PlanPathTraceCaptureProductCapacity(
            snapshot, oracleOwnedBytes, capacityPlan))
    {
        return false;
    }
    if (peakSlotBytes)
    {
        *peakSlotBytes = capacityPlan.peakSlotBytes;
    }

    const std::uint64_t startUs = Sys_Microseconds();
    RtPathTraceCaptureProduct candidate;
    candidate.epoch = snapshot.epoch;
    candidate.lateConsumeToken = snapshot.lateConsumeToken;
    candidate.viewIdentity = snapshot.viewIdentity;
    candidate.capacityCounts = snapshot.capacityCounts;
    candidate.inputOwnedBytes = snapshot.OwnedBytes();
    const std::size_t oracleBudgetBytes = Max(
        capacityPlan.oracleBytes, oracleOwnedBytes);
    std::size_t candidateOtherBytes = snapshot.OwnedBytes();
    if (!RtPathTraceProducerCheckedAddBytes(candidateOtherBytes,
            oracleBudgetBytes, RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES) ||
        !ReservePathTraceCaptureProductStorage(snapshot, candidate, candidateOtherBytes,
            capacityPlan.candidateBytes))
    {
        candidate.ResetAndRelease();
        return false;
    }
    std::size_t prefillBytes = 0;
    if (!RtPathTraceProducerCheckedAddBytes(prefillBytes,
            snapshot.OwnedBytes(), RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES) ||
        !RtPathTraceProducerCheckedAddBytes(prefillBytes,
            oracleOwnedBytes, RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES) ||
        !RtPathTraceProducerCheckedAddBytes(prefillBytes,
            candidate.OwnedBytes(), RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES))
    {
        candidate.ResetAndRelease();
        return false;
    }
    const RtPathTraceCaptureProductCapacityStamp candidateCapacity =
        CapturePathTraceProductCapacityStamp(candidate);
#if 0
    // Superseded by the authoritative Owner Harvest decision handoff below.
    // Keeping the old semantic builder disabled in this dirty integration tree
    // makes the boundary explicit while the reviewed change remains isolated.
    RtSmokeGeometryAdmissionBudget admissionBudget;
    admissionBudget.maxBytes = snapshot.admissionMaxBytes;
    admissionBudget.maxSurfaces = snapshot.admissionMaxSurfaces;
    std::uint64_t admissionBytes = 0;
    std::uint64_t admissionSurfaces = 0;
    const auto appendSurface = [&](RtPathTraceCaptureSurfaceProduct& surface)
    {
        FinalizePathTraceCaptureSurfaceDecision(surface);
        candidate.surfaces.push_back(surface);
    };

    for (const RtPathTraceCaptureRawSurface& raw : snapshot.surfaces)
    {
        const std::uint32_t baseMaterialId =
            HashPathTraceMaterialName(raw.materialName);
        RtPathTraceCaptureSurfaceProduct surface;
        surface.ordinal = raw.ordinal;
        surface.decisionPresence = RT_PT_CAPTURE_DECISION_FILTER;
        surface.sourceState = static_cast<std::uint32_t>(raw.safety);
        if (raw.safety != RtPathTraceCaptureSafetyDisposition::Ready)
        {
            surface.terminal = RtPathTraceCaptureTerminal::SafetyRejected;
            appendSurface(surface);
            continue;
        }
        const std::uint64_t* modelSurfaceTokens = nullptr;
        std::size_t modelSurfaceTokenCount = 0;
        if (!CaptureModelTokenSpan(raw,
                snapshot.modelTables.data(), snapshot.modelTables.size(),
                snapshot.modelSurfaceTokens.data(),
                snapshot.modelSurfaceTokens.size(),
                modelSurfaceTokens, modelSurfaceTokenCount))
        {
            surface.terminal = RtPathTraceCaptureTerminal::SemanticRejected;
            appendSurface(surface);
            continue;
        }
        const std::int32_t resolvedModelSurfaceIndex =
            ResolvePathTraceModelSurfaceIndexFromPod(
                raw.requestedModelSurfaceIndex, raw.currentTriToken,
                modelSurfaceTokens, modelSurfaceTokenCount);
        surface.applyGateSkip = CommittedApplyGateShouldSkip(
            snapshot.applyGate, raw.entityIndex,
            raw.requestedModelSurfaceIndex);
        if (surface.applyGateSkip)
        {
            surface.terminal = RtPathTraceCaptureTerminal::ApplyGateSkip;
            appendSurface(surface);
            continue;
        }
        if (raw.runtimeStageOffset > snapshot.runtimeStages.size() ||
            raw.runtimeStageCount >
                snapshot.runtimeStages.size() - raw.runtimeStageOffset)
        {
            surface.terminal = RtPathTraceCaptureTerminal::SemanticRejected;
            appendSurface(surface);
            continue;
        }
        RtSmokeSurfaceClassifyInput classifyInput = raw.classify;
        for (std::uint32_t stageIndex = 0;
            stageIndex < raw.runtimeStageCount; ++stageIndex)
        {
            classifyInput.material.hasAlphaTest =
                classifyInput.material.hasAlphaTest ||
                snapshot.runtimeStages[raw.runtimeStageOffset + stageIndex].hasAlphaTest;
        }
        bool hasActiveStage = classifyInput.material.materialPresent;
        if (raw.registersPresent)
        {
            hasActiveStage = false;
            if (raw.registerOffset > snapshot.registers.size() ||
                raw.registerCount > snapshot.registers.size() - raw.registerOffset)
            {
                surface.terminal = RtPathTraceCaptureTerminal::SemanticRejected;
                appendSurface(surface);
                continue;
            }
            for (std::uint32_t stageIndex = 0;
                stageIndex < raw.classifierStageCount; ++stageIndex)
            {
                const auto& stage = snapshot.classifierStages[
                    raw.classifierStageOffset + stageIndex];
                if (!stage.valid)
                {
                    continue;
                }
                const int reg = stage.conditionRegister;
                const float condition = reg >= 0 &&
                    static_cast<std::uint32_t>(reg) < raw.registerCount
                    ? snapshot.registers[raw.registerOffset + reg] : 1.0f;
                if (condition != 0.0f)
                {
                    hasActiveStage = true;
                    break;
                }
            }
        }
        if (!hasActiveStage || (!raw.guiAllowed && classifyInput.material.guiSurface) ||
            !raw.callbackAllowed)
        {
            surface.terminal = RtPathTraceCaptureTerminal::SemanticRejected;
            appendSurface(surface);
            continue;
        }

        if (raw.classifierStageOffset > snapshot.classifierStages.size() ||
            raw.classifierStageCount >
                snapshot.classifierStages.size() - raw.classifierStageOffset ||
            raw.classifier.stageCount !=
                static_cast<std::int32_t>(raw.classifierStageCount))
        {
            surface.terminal = RtPathTraceCaptureTerminal::SemanticRejected;
            appendSurface(surface);
            continue;
        }
        const RtSmokeTranslucentClassifierInfo classifier =
            BuildSmokeTranslucentClassifierInfo(
                raw.classifier,
                raw.classifierStageCount != 0
                    ? snapshot.classifierStages.data() + raw.classifierStageOffset
                    : nullptr,
                static_cast<std::int32_t>(raw.classifierStageCount));
        surface.compositeRoute = ParticleCompositeRouteFromRaw(raw, classifier,
            raw.runtimeStageCount != 0
                ? snapshot.runtimeStages.data() + raw.runtimeStageOffset : nullptr,
            raw.registersPresent && raw.registerCount != 0
                ? snapshot.registers.data() + raw.registerOffset : nullptr);
        if (surface.compositeRoute != 0)
        {
            surface.terminal = RtPathTraceCaptureTerminal::SemanticRejected;
            appendSurface(surface);
            continue;
        }
        surface.alphaDiagnosticRejected = raw.unifiedPtEnabled &&
            raw.removeAlphaClipEnabled && classifyInput.material.hasAlphaTest;
        if (surface.alphaDiagnosticRejected)
        {
            surface.terminal = RtPathTraceCaptureTerminal::SemanticRejected;
            appendSurface(surface);
            continue;
        }
        const RtSmokeSurfaceClass classifiedSurfaceClass =
            ClassifySmokeSurfaceFromPod(classifyInput, classifier);
        const bool promotedEmissiveCard = snapshot.rigidRouteEmissiveCards &&
            classifiedSurfaceClass == RtSmokeSurfaceClass::ParticleAlpha &&
            classifyInput.hasEntityDef &&
            !classifyInput.material.guiSurface &&
            !classifyInput.hasJointCache &&
            !classifyInput.hasStaticModelWithJoints &&
            !classifyInput.hasRenderEntityJoints &&
            !raw.entityCallbackPresent && !raw.entityForceUpdate &&
            !raw.dynamicModelPresent && !raw.cachedDynamicModelPresent &&
            classifyInput.material.deform == DFRM_NONE &&
            classifyInput.modelDepthHack == 0.0f &&
            SmokeMaterialCanPromoteRigidEmissiveCardFromPod(
                classifyInput.material, false, classifier);
        const bool promotedLiquidPoolCard = raw.liquidPoolEnabled &&
            classifiedSurfaceClass == RtSmokeSurfaceClass::ParticleAlpha &&
            classifyInput.hasEntityDef && !classifyInput.material.guiSurface &&
            !classifyInput.hasJointCache && !classifyInput.hasStaticModelWithJoints &&
            !classifyInput.hasRenderEntityJoints && !raw.entityCallbackPresent &&
            !raw.entityForceUpdate && !raw.dynamicModelPresent &&
            !raw.cachedDynamicModelPresent &&
            classifyInput.material.deform == DFRM_NONE &&
            classifyInput.modelDepthHack == 0.0f &&
            RegistryMaterialHasLiquidPool(snapshot, baseMaterialId);
        surface.surfaceClass = (promotedEmissiveCard || promotedLiquidPoolCard)
            ? RtSmokeSurfaceClass::RigidEntity : classifiedSurfaceClass;
        surface.translucentSubtype =
            ClassifySmokeTranslucentSubtypeFromPod(classifyInput.material, classifier);
        surface.surfaceClassId = CaptureSurfaceClassAndSubtypeId(
            surface.surfaceClass, surface.translucentSubtype);
        surface.materialClassSignature = SmokeMaterialRouteClassSignatureFromPod(
            classifyInput.material, surface.surfaceClass,
            surface.translucentSubtype, classifier);

        if (raw.registerOffset > snapshot.registers.size() ||
            raw.registerCount > snapshot.registers.size() - raw.registerOffset ||
            raw.runtimeStageOffset > snapshot.runtimeStages.size() ||
            raw.runtimeStageCount >
                snapshot.runtimeStages.size() - raw.runtimeStageOffset)
        {
            surface.terminal = RtPathTraceCaptureTerminal::SemanticRejected;
            appendSurface(surface);
            continue;
        }
        float surfaceOrigin[3] = {
            raw.modelMatrix[0] * raw.boundsCenter[0] +
                raw.modelMatrix[4] * raw.boundsCenter[1] +
                raw.modelMatrix[8] * raw.boundsCenter[2] + raw.modelMatrix[12],
            raw.modelMatrix[1] * raw.boundsCenter[0] +
                raw.modelMatrix[5] * raw.boundsCenter[1] +
                raw.modelMatrix[9] * raw.boundsCenter[2] + raw.modelMatrix[13],
            raw.modelMatrix[2] * raw.boundsCenter[0] +
                raw.modelMatrix[6] * raw.boundsCenter[1] +
                raw.modelMatrix[10] * raw.boundsCenter[2] + raw.modelMatrix[14]
        };
        const bool opaqueCompatibility =
            SmokeMaterialUsesOpaqueSwinglightCompatibilityFromPod(
                classifyInput.material, classifier);
        const RtPathTraceRuntimeMaterialEvalPod runtimeEval =
            BuildPathTraceRuntimeMaterialEvalFromPod(
                classifyInput.material.materialPresent,
                baseMaterialId,
                raw.runtimeStageCount != 0
                    ? snapshot.runtimeStages.data() + raw.runtimeStageOffset
                    : nullptr,
                raw.runtimeStageCount,
                raw.registersPresent && raw.registerCount != 0
                    ? snapshot.registers.data() + raw.registerOffset : nullptr,
                raw.registerCount,
                opaqueCompatibility,
                surfaceOrigin,
                true);
        const RtPathTraceRuntimeMaterialVariantPod variantKey =
            BuildPathTraceRuntimeMaterialVariantKeyFromPod(
                baseMaterialId, raw.entityIndex, raw.entityNum,
                raw.requestedModelSurfaceIndex, raw.currentTriToken,
                modelSurfaceTokens, modelSurfaceTokenCount);
        surface.runtimeMaterial = BuildPathTraceRuntimeMaterialDecisionFromPod(
            variantKey, runtimeEval,
            snapshot.variantBases.data(), snapshot.variantBases.size(),
            snapshot.registryMaterials.data(), snapshot.registryMaterials.size());
        surface.decisionPresence |= RT_PT_CAPTURE_DECISION_RUNTIME_MATERIAL;
        RtPathTraceMaterialVariantRegistrationProposal variant;
        variant.surfaceOrdinal = raw.ordinal;
        variant.baseMaterialId = baseMaterialId;
        variant.candidateId = surface.runtimeMaterial.initialCandidateId;
        variant.chosenId = surface.runtimeMaterial.chosenMaterialId;
        variant.collisionCount = surface.runtimeMaterial.collisionCount;
        variant.fallbackUsed = surface.runtimeMaterial.fallbackUsed;
        surface.materialId = variant.chosenId;
        if (variant.chosenId != variant.baseMaterialId)
        {
            candidate.materialVariants.push_back(variant);
            RtPathTraceMaterialInfoRegistrationIntent intent;
            intent.generation = snapshot.epoch.generation;
            intent.surfaceOrdinal = raw.ordinal;
            intent.baseMaterialId = baseMaterialId;
            RtPathTracePlanningCopyName(intent.materialName,
                sizeof(intent.materialName), raw.materialName);
            intent.reason = 1;
            intent.callPresent = true;
            candidate.materialInfoIntents.push_back(intent);
        }

        const std::uint64_t legacyStaticKey =
            RtPathTraceLegacyStaticSurfaceKeyFromRaw(raw);
        surface.staticMatch = HasStaticSurfaceFromPod(
            snapshot.geometryUniverse, legacyStaticKey);
        surface.decisionPresence |= RT_PT_CAPTURE_DECISION_ROUTE |
            RT_PT_CAPTURE_DECISION_IDENTITY;
        surface.liquidPoolPromoted = promotedLiquidPoolCard;
        RtPathTraceSourceFlagInput sourceFlagInput;
        sourceFlagInput.surfaceClass = surface.surfaceClass;
        sourceFlagInput.guiSurface = classifyInput.material.guiSurface;
        sourceFlagInput.hasJointCache = classifyInput.hasJointCache;
        sourceFlagInput.hasStaticModelWithJoints =
            classifyInput.hasStaticModelWithJoints;
        sourceFlagInput.hasRenderEntityJoints =
            classifyInput.hasRenderEntityJoints;
        sourceFlagInput.entityCallbackPresent = raw.entityCallbackPresent;
        sourceFlagInput.entityForceUpdate = raw.entityForceUpdate;
        sourceFlagInput.dynamicModelPresent = raw.dynamicModelPresent;
        sourceFlagInput.cachedDynamicModelPresent = raw.cachedDynamicModelPresent;
        sourceFlagInput.materialDeformed = classifyInput.material.deform != DFRM_NONE;
        sourceFlagInput.customShaderPresent = raw.customShaderPresent;
        sourceFlagInput.customSkinPresent = raw.customSkinPresent;
        surface.sourceFlags = RtPathTraceSourceFlagsFromPod(sourceFlagInput);
        if (surface.staticMatch)
        {
            surface.sourceFlags |= RT_PT_INSTANCE_SOURCE_STATIC_CACHE_MATCH;
        }
        const std::int32_t rigidIdentityModelSurfaceIndex =
            raw.requestedModelSurfaceIndex >= 0 ||
                RtPathTraceSourceFlagsAreDurableRigid(surface.sourceFlags)
            ? resolvedModelSurfaceIndex : -1;

        RtPathTraceRigidMeshIdentityPod meshIdentity;
        meshIdentity.modelIdentity = raw.modelBits;
        meshIdentity.modelEpoch = static_cast<std::uint32_t>(raw.modelEpoch);
        meshIdentity.modelSurfaceIndex = rigidIdentityModelSurfaceIndex;
        meshIdentity.jointIndex = raw.jointIndex;
        meshIdentity.vertexBufferIdentity = static_cast<std::uintptr_t>(
            cpu_producer_publish::RigidMeshVertexCacheIdentity(raw.ambientHandle));
        meshIdentity.indexBufferIdentity = static_cast<std::uintptr_t>(
            cpu_producer_publish::RigidMeshIndexCacheIdentity(raw.indexHandle));
        meshIdentity.numVerts = static_cast<int>(raw.vertexCount);
        meshIdentity.numIndexes = static_cast<int>(raw.sourceTriIndexCount);
        meshIdentity.vertexFormat = static_cast<std::uint32_t>(
            RtSmokeGeometryBufferFormat::LegacySmokeVertex);
        meshIdentity.materialId = surface.materialId;
        meshIdentity.materialClassSignature = surface.materialClassSignature;
        meshIdentity.sourceKind = SmokeSurfaceClassId(surface.surfaceClass);
        surface.meshHash = BuildPathTraceRigidMeshHashFromPod(meshIdentity);
        RtPathTraceRigidInstanceIdentityPod instanceIdentity;
        instanceIdentity.meshHash = surface.meshHash;
        instanceIdentity.renderWorldIdentity = raw.renderWorldIdentity;
        instanceIdentity.renderDefIndex = raw.renderDefIndex;
        instanceIdentity.renderDefGeneration = raw.renderDefGeneration;
        instanceIdentity.modelEpoch = static_cast<std::uint32_t>(raw.modelEpoch);
        instanceIdentity.entityIndex = raw.entityIndex;
        instanceIdentity.renderEntityNum = raw.entityNum;
        instanceIdentity.modelSurfaceIndex = rigidIdentityModelSurfaceIndex;
        instanceIdentity.materialId = surface.materialId;
        instanceIdentity.jointIndex = raw.jointIndex;
        surface.instanceId = BuildPathTraceRigidInstanceIdFromPod(instanceIdentity);

        if (snapshot.recordAllInstanceClasses ||
            RtPathTraceSourceFlagsAreDurableRigid(surface.sourceFlags))
        {
            RtPathTraceInstanceObservationProposal observation;
            observation.surfaceOrdinal = raw.ordinal;
            observation.instanceId = surface.instanceId;
            observation.meshHash = surface.meshHash;
            const RtPathTraceInstanceHistoryApplication history =
                ApplyInstanceHistoryFromPod(snapshot.instanceUniverse,
                    surface.instanceId, raw.objectToWorld,
                    snapshot.epoch.frameIndex);
            observation.hasPrevious = history.hasPreviousObjectToWorld;
            observation.transformContinuous = history.transformContinuous;
            std::memcpy(observation.currentObjectToWorld,
                history.currentObjectToWorld,
                sizeof(observation.currentObjectToWorld));
            std::memcpy(observation.previousObjectToWorld,
                history.previousObjectToWorld,
                sizeof(observation.previousObjectToWorld));
            candidate.instanceObservations.push_back(observation);
            if (RtPathTraceSourceFlagsAreDurableRigid(surface.sourceFlags) &&
                surface.meshHash != 0 && surface.materialId != 0 &&
                raw.vertexCount != 0 && raw.sourceTriIndexCount != 0 &&
                (raw.sourceTriIndexCount % 3) == 0)
            {
                RtPathTraceRigidCandidateProposal rigid;
                rigid.surfaceOrdinal = raw.ordinal;
                rigid.meshHash = surface.meshHash;
                rigid.instanceId = surface.instanceId;
                rigid.materialId = surface.materialId;
                candidate.rigidCandidates.push_back(rigid);
            }
        }
        if (surface.surfaceClass == RtSmokeSurfaceClass::StaticWorld)
        {
            surface.terminal = RtPathTraceCaptureTerminal::StaticMatched;
            appendSurface(surface);
            continue;
        }
        if (surface.staticMatch)
        {
            surface.terminal = RtPathTraceCaptureTerminal::StaticMatched;
            candidate.staticMembershipSurfaces.push_back(raw.ordinal);
            appendSurface(surface);
            continue;
        }

        if (!RtPathTraceCaptureSemanticFactsReadable(raw))
        {
            return false;
        }
        surface.rigidReadyByMesh = raw.rigidReadyByMesh;
        surface.rigidReadyByResident = raw.rigidReadyByResident;
        if (PathTraceCaptureShouldOmitRoutedRigidFromPod(
                snapshot.removeRoutedRigidDynamic, surface.surfaceClass,
                surface.rigidReadyByMesh, surface.rigidReadyByResident))
        {
            surface.terminal = RtPathTraceCaptureTerminal::RoutedRigidReady;
            candidate.routedReadySkipSurfaces.push_back(raw.ordinal);
            appendSurface(surface);
            continue;
        }

        if (surface.surfaceClass == RtSmokeSurfaceClass::SkinnedDeformed)
        {
            if (ApplyPathTraceSkinnedCaptureAdmissionFromPod(
                    raw.skinnedCaptureAdmission, raw.skinnedOmitVertexCount,
                    raw.skinnedOmitIndexCount, surface))
            {
                appendSurface(surface);
                continue;
            }
        }

        RtSmokeGeometryAdmissionInput admissionInput;
        admissionInput.currentBytes = admissionBytes;
        admissionInput.currentSurfaces = admissionSurfaces;
        admissionInput.candidateVertexCount = raw.vertexCount;
        admissionInput.candidateIndexCount = raw.sourceTriIndexCount;
        admissionInput.vertexStride = sizeof(PathTraceSmokeVertex);
        admissionInput.indexStride = sizeof(std::uint32_t);
        admissionInput.triangleMetadataStride = sizeof(std::uint32_t) * 4ull;
        const RtSmokeGeometryAdmissionPlan admissionPlan =
            BuildSmokeGeometryAdmissionPlan(admissionBudget, admissionInput);
        surface.decisionPresence |= RT_PT_CAPTURE_DECISION_ADMISSION;
        surface.admissionPrecheckPassed = admissionPlan.Admitted();
        if (!admissionPlan.Admitted())
        {
            surface.terminal = RtPathTraceCaptureTerminal::AdmissionRejected;
            appendSurface(surface);
            continue;
        }

        const int bucket = static_cast<int>(SmokeSurfaceClassId(surface.surfaceClass));
        if (bucket < 0 || bucket >= RT_SMOKE_CLASS_COUNT ||
            raw.vertexOffset > snapshot.vertices.size() ||
            raw.vertexCount > snapshot.vertices.size() - raw.vertexOffset ||
            raw.indexOffset > snapshot.indexes.size() ||
            raw.indexCount > snapshot.indexes.size() - raw.indexOffset ||
            raw.jointOffset > snapshot.joints.size() ||
            raw.jointCount > snapshot.joints.size() - raw.jointOffset)
        {
            surface.terminal = RtPathTraceCaptureTerminal::SemanticRejected;
            appendSurface(surface);
            continue;
        }
        std::vector<PathTraceSmokeVertex>& verts = candidate.vertices;
        std::vector<std::uint32_t>& inds = candidate.indexes;
        const std::uint32_t vertexStart = static_cast<std::uint32_t>(verts.size());
        const std::uint32_t indexStart = static_cast<std::uint32_t>(inds.size());
        surface.decisionPresence |= RT_PT_CAPTURE_DECISION_APPEND;
        surface.appendAttempted = true;
        surface.bucketIndex = bucket;
        surface.preAppendVertexOffset = vertexStart;
        surface.preAppendIndexOffset = indexStart;
        surface.preAppendTriangleOffset = indexStart / 3;
        RtPathTraceCommittedVertexInput vertexInput;
        vertexInput.vertices = snapshot.vertices.data() + raw.vertexOffset;
        vertexInput.joints = raw.jointCount != 0
            ? snapshot.joints.data() + raw.jointOffset : nullptr;
        std::memcpy(vertexInput.modelMatrix, raw.modelMatrix,
            sizeof(vertexInput.modelMatrix));
        std::memcpy(vertexInput.bumpMatrix, raw.bumpMatrix,
            sizeof(vertexInput.bumpMatrix));
        for (std::uint32_t vertexIndex = 0; vertexIndex < raw.vertexCount; ++vertexIndex)
        {
            verts.push_back(BuildPathTraceCommittedVertexFromOwned(
                vertexInput, vertexIndex));
        }
        for (std::uint32_t sourceIndex = 0; sourceIndex + 2 < raw.indexCount;
            sourceIndex += 3)
        {
            const triIndex_t i0 = snapshot.indexes[raw.indexOffset + sourceIndex];
            const triIndex_t i1 = snapshot.indexes[raw.indexOffset + sourceIndex + 1];
            const triIndex_t i2 = snapshot.indexes[raw.indexOffset + sourceIndex + 2];
            if (i0 >= raw.vertexCount || i1 >= raw.vertexCount || i2 >= raw.vertexCount)
            {
                ++surface.invalidIndexCount;
                continue;
            }
            if (IsZeroAreaSmokeTriangle(
                    SmokeVertexPosition(verts[vertexStart + i0]),
                    SmokeVertexPosition(verts[vertexStart + i1]),
                    SmokeVertexPosition(verts[vertexStart + i2])))
            {
                ++surface.zeroAreaTriangleCount;
                continue;
            }
            const std::uint32_t emittedTriangleOrdinal =
                static_cast<std::uint32_t>((inds.size() - indexStart) / 3u);
            inds.push_back(vertexStart + i0);
            inds.push_back(vertexStart + i1);
            inds.push_back(vertexStart + i2);
            const bool invalidNormalTriangle =
                !SmokeNormalIsUsable(SmokeVertexNormal(verts[vertexStart + i0])) ||
                !SmokeNormalIsUsable(SmokeVertexNormal(verts[vertexStart + i1])) ||
                !SmokeNormalIsUsable(SmokeVertexNormal(verts[vertexStart + i2]));
            const std::uint32_t triangleClassAndFlags =
                BuildPathTraceCaptureTriangleClassWordFromPod(
                    surface.surfaceClassId, raw.activeEmissiveStage,
                    invalidNormalTriangle);
            candidate.triangleClasses.push_back(triangleClassAndFlags);
            candidate.triangleMaterials.push_back(surface.materialId);
            candidate.triangleInstances.push_back(static_cast<std::uint64_t>(
                Max(1, raw.entityIndex + 1)));
            candidate.triangleIdentities.push_back(
                BuildPathTraceCaptureTriangleIdentityFromPod(
                    raw.entityIndex, raw.entityNum, baseMaterialId,
                    raw.triIdentityBits, emittedTriangleOrdinal));
        }
        const std::uint32_t emitted = static_cast<std::uint32_t>(inds.size()) - indexStart;
        if (emitted == 0)
        {
            verts.resize(vertexStart);
            surface.terminal = RtPathTraceCaptureTerminal::RolledBack;
            surface.rollbackApplied = true;
            appendSurface(surface);
            continue;
        }
        admissionInput.candidateVertexCount = raw.vertexCount;
        admissionInput.candidateIndexCount = emitted;
        const RtSmokeGeometryAdmissionPlan actualAdmissionPlan =
            BuildSmokeGeometryAdmissionPlan(admissionBudget, admissionInput);
        if (!actualAdmissionPlan.Admitted())
        {
            verts.resize(vertexStart);
            inds.resize(indexStart);
            candidate.triangleClasses.resize(indexStart / 3);
            candidate.triangleMaterials.resize(indexStart / 3);
            candidate.triangleInstances.resize(indexStart / 3);
            candidate.triangleIdentities.resize(indexStart / 3);
            surface.terminal = RtPathTraceCaptureTerminal::RolledBack;
            surface.rollbackApplied = true;
            appendSurface(surface);
            continue;
        }
        admissionBytes = actualAdmissionPlan.totalBytes;
        admissionSurfaces = actualAdmissionPlan.totalSurfaces;
        surface.terminal = RtPathTraceCaptureTerminal::Accepted;
        surface.vertexOffset = vertexStart;
        surface.vertexCount = raw.vertexCount;
        surface.indexOffset = indexStart;
        surface.indexCount = emitted;
        surface.triangleOffset = indexStart / 3;
        surface.triangleCount = emitted / 3;
        surface.decisionPresence |= RT_PT_CAPTURE_DECISION_CAPTURED_RECORD |
            RT_PT_CAPTURE_DECISION_BUCKET_PUBLICATION;
        surface.capturedRecordPresent = true;
        surface.capturedRecordVertexCount = raw.vertexCount;
        surface.capturedRecordIndexCount = emitted;
        surface.capturedRecordTriangleCount = emitted / 3;
        surface.bucketRangePublished = true;
        if (bucket >= 1)
        {
            surface.decisionPresence |= RT_PT_CAPTURE_DECISION_MERGED_WALK;
            surface.mergedWalkRecordPresent = true;
            surface.mergedWalkVertexCount = raw.vertexCount;
            surface.mergedWalkIndexCount = emitted;
            surface.mergedWalkTriangleCount = emitted / 3;
            if (surface.surfaceClass == RtSmokeSurfaceClass::RigidEntity)
            {
                surface.mergedCompanionRigidId = surface.instanceId;
            }
        }
        if (surface.surfaceClass == RtSmokeSurfaceClass::SkinnedDeformed)
        {
            surface.decisionPresence |= RT_PT_CAPTURE_DECISION_SKINNED_RECORD;
            surface.skinnedRecordPresent = true;
            surface.skinnedRecordVertexCount = raw.vertexCount;
            surface.skinnedRecordIndexCount = emitted;
            surface.skinnedRecordTriangleCount = emitted / 3;
        }
        appendSurface(surface);
    }
#endif
    if (!BuildPathTraceGeometryFromOwnerDecisions(snapshot, candidate) ||
        ![&]()
        {
            OPTICK_EVENT("PT Lane A Rigid Prepared Payload Build");
            return BuildPathTraceOwnerRigidCandidateProposals(
                    snapshot, candidate) &&
                BuildPathTraceRigidPreparedPayloads(
                    snapshot, candidate, false);
        }() ||
        !PathTraceCaptureProductStorageReady(snapshot, candidate) ||
        !PathTraceCaptureProductCapacityUnchanged(candidate, candidateCapacity))
    {
        candidate.ResetAndRelease();
        return false;
    }
    candidate.workerCpuUs = Sys_Microseconds() - startUs;
    candidate.provenance = RtPathTraceCaptureProductProvenance::OwnerDecisions;
    candidate.complete = true;
    candidate.actualOwnedBytes = candidate.OwnedBytes();
    std::size_t actualPeak = snapshot.OwnedBytes();
    if (!RtPathTraceProducerCheckedAddBytes(actualPeak, oracleBudgetBytes,
            RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES) ||
        !RtPathTraceProducerCheckedAddBytes(actualPeak, candidate.OwnedBytes(),
            RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES) ||
        !RtPathTraceCaptureProductShapeValid(candidate))
    {
        return false;
    }
    if (peakSlotBytes)
    {
        *peakSlotBytes = Max(*peakSlotBytes,
            Max(actualPeak, capacityPlan.peakSlotBytes));
    }
    product = std::move(candidate);
    return true;
}
