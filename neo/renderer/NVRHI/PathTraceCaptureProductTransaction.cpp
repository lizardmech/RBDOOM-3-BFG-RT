#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCaptureProduct.h"
#include "PathTraceCommittedProposalProduct.h"
#include "PathTraceProducerLaneContract.h"
#include "PathTraceRigidIdentity.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace
{
void HashCaptureReceiptBytes(
    std::uint64_t& hash, const void* bytes, std::size_t byteCount)
{
    const std::uint8_t* source = static_cast<const std::uint8_t*>(bytes);
    for (std::size_t index = 0; index < byteCount; ++index)
    {
        hash ^= source[index];
        hash *= 1099511628211ull;
    }
}

template <typename T>
void HashCaptureReceiptValue(std::uint64_t& hash, const T& value)
{
    HashCaptureReceiptBytes(hash, &value, sizeof(value));
}
}

bool CompatibleForLateConsume(
    const RtPathTraceLateConsumeToken& product,
    const RtPathTraceLateConsumeToken& current)
{
    return product.mapTimeStamp == current.mapTimeStamp &&
        product.mapLoadSerial == current.mapLoadSerial &&
        std::memcmp(product.mapName, current.mapName,
            RT_PT_PLANNING_MAP_NAME_CAPACITY) == 0 &&
        product.capturedAfterBeginFrame && current.capturedAfterBeginFrame &&
        product.capturedAfterStaticPreload && current.capturedAfterStaticPreload &&
        product.configFingerprint == current.configFingerprint;
}

bool AttachPathTraceOwnerDecisionTable(
    RtPathTraceCaptureOwnerSnapshot& snapshot,
    const RtPathTraceCaptureSurfaceProduct* decisions,
    std::size_t decisionCount,
    const RtPathTraceCaptureMembershipReceipt& authoritativeReceipt)
{
    snapshot.ownerDecisionsComplete = false;
    snapshot.ownerMembershipReceipt = {};
    const bool alreadyInSnapshot = decisions != nullptr &&
        decisions == snapshot.ownerDecisions.data() &&
        decisionCount == snapshot.ownerDecisions.size();
    if (!alreadyInSnapshot)
    {
        snapshot.ownerDecisions.clear();
    }
    if (!snapshot.complete || decisionCount != snapshot.surfaces.size() ||
        decisionCount > snapshot.ownerDecisions.capacity() ||
        (decisionCount != 0 && decisions == nullptr) ||
        !authoritativeReceipt.complete ||
        authoritativeReceipt.surfaceCount != decisionCount)
    {
        return false;
    }
    for (std::size_t index = 0; index < decisionCount; ++index)
    {
        if (decisions[index].ordinal != index)
        {
            snapshot.ownerDecisions.clear();
            return false;
        }
        if (!alreadyInSnapshot)
        {
            snapshot.ownerDecisions.push_back(decisions[index]);
        }
    }
    const RtPathTraceCaptureMembershipReceipt reconstructed =
        BuildPathTraceCaptureMembershipReceipt(
            snapshot.ownerDecisions.data(), snapshot.ownerDecisions.size());
    if (!PathTraceCaptureMembershipReceiptsMatch(
            reconstructed, authoritativeReceipt))
    {
        snapshot.ownerDecisions.clear();
        return false;
    }
    snapshot.ownerMembershipReceipt = authoritativeReceipt;
    snapshot.ownerDecisionsComplete = true;
    return true;
}

bool PathTraceCaptureMembershipReceiptsMatch(
    const RtPathTraceCaptureMembershipReceipt& product,
    const RtPathTraceCaptureMembershipReceipt& current)
{
    return product.complete && current.complete &&
        product.hash == current.hash &&
        product.surfaceCount == current.surfaceCount &&
        product.skinnedSurfaceCount == current.skinnedSurfaceCount &&
        product.cpuSkinnedAcceptedCount == current.cpuSkinnedAcceptedCount;
}

RtPathTraceCaptureMembershipReceipt BuildPathTraceCaptureMembershipReceipt(
    const RtPathTraceCaptureSurfaceProduct* surfaces,
    std::size_t surfaceCount)
{
    RtPathTraceCaptureMembershipReceipt receipt;
    if (surfaceCount != 0 && surfaces == nullptr)
    {
        return receipt;
    }
    receipt.hash = 14695981039346656037ull;
    for (std::size_t index = 0; index < surfaceCount; ++index)
    {
        AppendPathTraceCaptureMembershipReceipt(receipt, surfaces[index]);
    }
    receipt.surfaceCount = surfaceCount <= UINT32_MAX
        ? static_cast<std::uint32_t>(surfaceCount) : UINT32_MAX;
    receipt.complete = surfaceCount <= UINT32_MAX;
    return receipt;
}

void AppendPathTraceCaptureMembershipReceipt(
    RtPathTraceCaptureMembershipReceipt& receipt,
    const RtPathTraceCaptureSurfaceProduct& surface)
{
    if (receipt.hash == 0)
    {
        receipt.hash = 14695981039346656037ull;
    }
    HashCaptureReceiptValue(receipt.hash, surface.ordinal);
    HashCaptureReceiptValue(receipt.hash, surface.terminal);
    HashCaptureReceiptValue(receipt.hash, surface.surfaceClass);
    HashCaptureReceiptValue(receipt.hash, surface.materialId);
    HashCaptureReceiptValue(receipt.hash, surface.instanceId);
    HashCaptureReceiptValue(receipt.hash, surface.meshHash);
    HashCaptureReceiptValue(receipt.hash, surface.vertexCount);
    HashCaptureReceiptValue(receipt.hash, surface.indexCount);
    HashCaptureReceiptValue(receipt.hash, surface.skinnedAdmission);
    HashCaptureReceiptValue(receipt.hash, surface.rigidReadyByMesh);
    HashCaptureReceiptValue(receipt.hash, surface.rigidReadyByResident);
    ++receipt.surfaceCount;
    if (surface.surfaceClass == RtSmokeSurfaceClass::SkinnedDeformed)
    {
        ++receipt.skinnedSurfaceCount;
        if (surface.terminal == RtPathTraceCaptureTerminal::Accepted &&
            !surface.skinnedCaptureOmitted)
        {
            ++receipt.cpuSkinnedAcceptedCount;
        }
    }
}

namespace
{
static_assert(static_cast<int>(RtSmokeSurfaceClass::StaticWorld) == 0 &&
    static_cast<int>(RtSmokeSurfaceClass::RigidEntity) == 1 &&
    static_cast<int>(RtSmokeSurfaceClass::SkinnedDeformed) == 2 &&
    static_cast<int>(RtSmokeSurfaceClass::ParticleAlpha) == 3 &&
    static_cast<int>(RtSmokeSurfaceClass::Unknown) == 4,
    "bucket-major partition requires the canonical surface-class ordinals");

template<typename T>
std::size_t VectorOwnedBytes(const std::vector<T>& values)
{
    return values.capacity() * sizeof(T);
}

bool BeforeReserve(
    RtPathTraceCaptureReserveTestSeam* seam,
    bool& skip,
    std::size_t& inflateBy)
{
    skip = false;
    inflateBy = 0;
    if (seam == nullptr)
    {
        return true;
    }
    const std::size_t ordinal = seam->reserveCalls++;
    skip = ordinal == seam->skipAtReserve;
    inflateBy = ordinal == seam->inflateAtReserve ? seam->inflateBy : 0;
    if (ordinal != seam->failAtReserve)
    {
        return true;
    }
    if (seam->failure == RtPathTraceCaptureReserveFailure::BadAlloc)
    {
        throw std::bad_alloc();
    }
    if (seam->failure == RtPathTraceCaptureReserveFailure::LengthError)
    {
        throw std::length_error("PathTrace capture reserve test seam");
    }
    return true;
}

template<typename T>
bool AppendNoGrow(std::vector<T>& destination, const T* source, std::size_t count)
{
    if (count == 0)
    {
        return true;
    }
    if (count > destination.capacity() - destination.size())
    {
        return false;
    }
    destination.insert(destination.end(), source, source + count);
    return true;
}

template<typename T>
bool PushNoGrow(std::vector<T>& destination, const T& value)
{
    if (destination.size() == destination.capacity())
    {
        return false;
    }
    destination.push_back(value);
    return true;
}

bool OwnerStorageReady(const RtPathTraceCaptureOwnerSnapshot& value)
{
    const RtPathTraceCaptureCapacityCounts& count = value.capacityCounts;
    return value.applyGate.omittedSkinKeys.capacity() >= count.applyGateKeys &&
        value.instanceUniverse.meshes.capacity() >= count.instanceMeshes &&
        value.instanceUniverse.histories.capacity() >= count.instanceHistories &&
        value.geometryUniverse.staticSurfaces.capacity() >= count.geometryStaticSurfaces &&
        value.geometryUniverse.rigidRoutes.capacity() >= count.geometryRigidRoutes &&
        value.geometryUniverse.rigidResidents.capacity() >= count.geometryRigidResidents &&
        value.variantBases.capacity() >= count.variantBases &&
        value.registryMaterials.capacity() >= count.registryMaterials &&
        value.modelTables.capacity() >= count.modelTables &&
        value.modelSurfaceTokens.capacity() >= count.modelSurfaceTokens &&
        value.surfaces.capacity() >= count.surfaces &&
        value.ownerDecisions.capacity() >= count.surfaces &&
        value.classifierStages.capacity() >= count.classifierStages &&
        value.runtimeStages.capacity() >= count.runtimeStages &&
        value.registers.capacity() >= count.registers &&
        value.vertices.capacity() >= count.vertices &&
        value.indexes.capacity() >= count.indexes &&
        value.joints.capacity() >= count.joints;
}

bool ProductWithinBounds(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    const RtPathTraceCaptureProduct& value)
{
    const std::size_t surfaces = snapshot.capacityCounts.surfaces;
    const std::size_t triangles = snapshot.capacityCounts.indexes / 3;
    std::size_t preparedVertices = 0;
    std::size_t preparedIndexes = 0;
    for (const RtPathTraceRigidPreparedPayload& payload :
         value.preparedRigidPayloads)
    {
        if (preparedVertices > snapshot.capacityCounts.preparedRigidVertices ||
            payload.localVertices.size() >
                snapshot.capacityCounts.preparedRigidVertices - preparedVertices ||
            preparedIndexes > snapshot.capacityCounts.preparedRigidIndexes ||
            payload.localIndexes.size() >
                snapshot.capacityCounts.preparedRigidIndexes - preparedIndexes)
        {
            return false;
        }
        preparedVertices += payload.localVertices.size();
        preparedIndexes += payload.localIndexes.size();
    }
    return value.surfaces.size() <= surfaces &&
        value.vertices.size() <= snapshot.capacityCounts.vertices &&
        value.indexes.size() <= snapshot.capacityCounts.indexes &&
        value.triangleClasses.size() <= triangles &&
        value.triangleMaterials.size() <= triangles &&
        value.triangleInstances.size() <= triangles &&
        value.triangleIdentities.size() <= triangles &&
        value.materialInfoIntents.size() <= snapshot.capacityCounts.materialInfoIntents &&
        value.materialVariants.size() <= snapshot.capacityCounts.materialVariantProposals &&
        value.instanceObservations.size() <= snapshot.capacityCounts.instanceObservationProposals &&
        value.rigidCandidates.size() <= snapshot.capacityCounts.rigidCandidateProposals &&
        value.preparedRigidPayloads.size() <=
            snapshot.capacityCounts.preparedRigidPayloads &&
        value.frameMaterialIds.size() <= snapshot.capacityCounts.frameMaterialIds &&
        value.receiptSurfaces.size() <= snapshot.capacityCounts.receiptSurfaces &&
        value.staticMembershipSurfaces.size() <= surfaces &&
        value.routedReadySkipSurfaces.size() <= surfaces;
}

template<typename T>
bool RangeValid(std::size_t offset, std::size_t count, const std::vector<T>& values)
{
    return offset <= values.size() && count <= values.size() - offset;
}

bool PreparedRigidPayloadShapeValid(
    const RtPathTraceCaptureProduct& product,
    bool requireReceiptWitness) noexcept
{
    std::size_t distinctProposalCount = 0;
    std::size_t preparedVertexCount = 0;
    std::size_t preparedIndexCount = 0;
    for (std::size_t proposalIndex = 0;
         proposalIndex < product.rigidCandidates.size(); ++proposalIndex)
    {
        const RtPathTraceRigidCandidateProposal& proposal =
            product.rigidCandidates[proposalIndex];
        if (proposal.meshHash == 0 || proposal.materialId == 0 ||
            proposal.surfaceOrdinal >= product.surfaces.size() ||
            (requireReceiptWitness &&
                proposal.surfaceOrdinal >= product.receiptSurfaces.size()))
        {
            return false;
        }
        const RtPathTraceCaptureSurfaceProduct& surface =
            product.surfaces[proposal.surfaceOrdinal];
        const bool exactOwnerSurfaceWitness =
            surface.meshHash == proposal.meshHash &&
            surface.instanceId == proposal.instanceId &&
            surface.materialId == proposal.materialId &&
            RtPathTraceSourceFlagsAreDurableRigid(surface.sourceFlags);
        const bool receiptBackedSurfaceWitness = requireReceiptWitness &&
            (surface.meshHash == 0 || surface.meshHash == proposal.meshHash) &&
            (surface.materialId == 0 ||
                surface.materialId == proposal.materialId);
        if (!exactOwnerSurfaceWitness && !receiptBackedSurfaceWitness)
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
        bool seen = false;
        for (std::size_t prior = 0; prior < proposalIndex; ++prior)
        {
            if (product.rigidCandidates[prior].meshHash == proposal.meshHash)
            {
                seen = true;
                break;
            }
        }
        distinctProposalCount += seen ? 0u : 1u;
    }
    if (product.preparedRigidPayloads.size() != distinctProposalCount)
    {
        return false;
    }
    for (std::size_t payloadIndex = 0;
         payloadIndex < product.preparedRigidPayloads.size(); ++payloadIndex)
    {
        const RtPathTraceRigidPreparedPayload& payload =
            product.preparedRigidPayloads[payloadIndex];
        std::size_t occurrenceCount = 0;
        const RtPathTraceRigidCandidateProposal* firstProposal = nullptr;
        for (const RtPathTraceRigidCandidateProposal& proposal :
             product.rigidCandidates)
        {
            if (proposal.meshHash == payload.meshHash)
            {
                if (firstProposal == nullptr) firstProposal = &proposal;
                ++occurrenceCount;
            }
        }
        if (payload.meshHash == 0 || payload.materialId == 0 ||
            payload.occurrenceCount == 0 ||
            payload.surfaceOrdinal >= product.surfaces.size() ||
            firstProposal == nullptr ||
            payload.occurrenceCount != occurrenceCount ||
            payload.surfaceOrdinal != firstProposal->surfaceOrdinal ||
            payload.instanceId != firstProposal->instanceId ||
            payload.materialId != firstProposal->materialId ||
            (requireReceiptWitness &&
                payload.surfaceOrdinal >= product.receiptSurfaces.size()) ||
            payload.localVertices.size() != payload.fullTriangleVertexCount ||
            payload.localIndexes.size() != payload.fullTriangleIndexCount ||
            payload.localVertices.empty() || payload.localIndexes.empty() ||
            (payload.localIndexes.size() % 3u) != 0u ||
            !RtPathTraceValidateRigidPreparedPayload(payload) ||
            payload.materialName[RT_PT_RIGID_PREPARED_NAME_BYTES - 1u] != '\0' ||
            payload.modelName[RT_PT_RIGID_PREPARED_NAME_BYTES - 1u] != '\0')
        {
            return false;
        }
        const RtPathTraceCaptureSurfaceProduct& surface =
            product.surfaces[payload.surfaceOrdinal];
        if ((!requireReceiptWitness &&
                (surface.meshHash != payload.meshHash ||
                    surface.materialId != payload.materialId)) ||
            (requireReceiptWitness &&
                ((surface.meshHash != 0 &&
                    surface.meshHash != payload.meshHash) ||
                 (surface.materialId != 0 &&
                    surface.materialId != payload.materialId))))
        {
            return false;
        }
        if (requireReceiptWitness)
        {
            const RtPathTraceCommittedReferencedSurfaceReceiptRow& receipt =
                product.receiptSurfaces[payload.surfaceOrdinal];
            if (receipt.persistedMeshHash != payload.meshHash ||
                receipt.persistedChosenMaterialId != payload.materialId)
            {
                return false;
            }
        }
        if (preparedVertexCount > product.capacityCounts.preparedRigidVertices ||
            payload.localVertices.size() >
                product.capacityCounts.preparedRigidVertices - preparedVertexCount ||
            preparedIndexCount > product.capacityCounts.preparedRigidIndexes ||
            payload.localIndexes.size() >
                product.capacityCounts.preparedRigidIndexes - preparedIndexCount)
        {
            return false;
        }
        preparedVertexCount += payload.localVertices.size();
        preparedIndexCount += payload.localIndexes.size();
        for (std::size_t prior = 0; prior < payloadIndex; ++prior)
        {
            if (product.preparedRigidPayloads[prior].meshHash == payload.meshHash)
            {
                return false;
            }
        }
    }
    return true;
}
} // namespace

void RtPathTraceCaptureOwnerSnapshot::ResetAndRelease()
{
    RtPathTraceCaptureOwnerSnapshot empty;
    *this = std::move(empty);
}

std::size_t RtPathTraceCaptureOwnerSnapshot::OwnedBytes() const
{
    return sizeof(*this) +
        (instanceUniverse.OwnedBytes() - sizeof(instanceUniverse)) +
        (geometryUniverse.OwnedBytes() - sizeof(geometryUniverse)) +
        (applyGate.OwnedBytes() - sizeof(applyGate)) +
        VectorOwnedBytes(variantBases) + VectorOwnedBytes(registryMaterials) +
        VectorOwnedBytes(modelTables) + VectorOwnedBytes(modelSurfaceTokens) +
        VectorOwnedBytes(surfaces) + VectorOwnedBytes(ownerDecisions) +
        VectorOwnedBytes(classifierStages) +
        VectorOwnedBytes(runtimeStages) +
        VectorOwnedBytes(registers) + VectorOwnedBytes(vertices) +
        VectorOwnedBytes(indexes) + VectorOwnedBytes(joints);
}

void RtPathTraceCaptureProduct::ResetAndRelease()
{
    RtPathTraceCaptureProduct empty;
    *this = std::move(empty);
}

std::size_t RtPathTraceCaptureProduct::OwnedBytes() const
{
    std::size_t preparedNestedBytes = 0;
    for (const RtPathTraceRigidPreparedPayload& payload : preparedRigidPayloads)
    {
        preparedNestedBytes += VectorOwnedBytes(payload.localVertices) +
            VectorOwnedBytes(payload.localIndexes);
    }
    return sizeof(*this) + VectorOwnedBytes(surfaces) + VectorOwnedBytes(vertices) +
        VectorOwnedBytes(indexes) + VectorOwnedBytes(triangleClasses) +
        VectorOwnedBytes(triangleMaterials) + VectorOwnedBytes(triangleInstances) +
        VectorOwnedBytes(triangleIdentities) + VectorOwnedBytes(materialInfoIntents) +
        VectorOwnedBytes(materialVariants) + VectorOwnedBytes(instanceObservations) +
        VectorOwnedBytes(rigidCandidates) + VectorOwnedBytes(preparedRigidPayloads) +
        preparedNestedBytes + VectorOwnedBytes(frameMaterialIds) +
        VectorOwnedBytes(receiptSurfaces) +
        VectorOwnedBytes(staticMembershipSurfaces) +
        VectorOwnedBytes(routedReadySkipSurfaces);
}

void RtPathTraceCaptureOracle::ResetAndRelease()
{
    RtPathTraceCaptureOracle empty;
    *this = std::move(empty);
}

std::size_t RtPathTraceCaptureOracle::OwnedBytes() const
{
    return sizeof(*this) + VectorOwnedBytes(vertices) + VectorOwnedBytes(indexes) +
        VectorOwnedBytes(triangleClasses) + VectorOwnedBytes(triangleMaterials) +
        VectorOwnedBytes(triangleInstances) + VectorOwnedBytes(triangleIdentities) +
        VectorOwnedBytes(materialInfoIntents) + VectorOwnedBytes(materialVariants) +
        VectorOwnedBytes(instanceObservations) + VectorOwnedBytes(rigidCandidates) +
        VectorOwnedBytes(staticMembershipSurfaces) +
        VectorOwnedBytes(routedReadySkipSurfaces) + VectorOwnedBytes(surfaces);
}

bool PlanPathTraceCompleteSlotCapacity(
    const RtPathTraceCaptureCapacityCounts& counts,
    std::size_t snapshotFixedBytes,
    RtPathTraceCaptureProductCapacityPlan& plan)
{
    RtPathTraceCompleteSlotCardinality cardinality;
    cardinality.surfaces = counts.surfaces;
    cardinality.stages = counts.classifierStages;
    cardinality.runtimeStages = counts.runtimeStages;
    cardinality.registers = counts.registers;
    cardinality.vertices = counts.vertices;
    cardinality.indexes = counts.indexes;
    cardinality.joints = counts.joints;
    cardinality.variantBases = counts.variantBases;
    cardinality.registryMaterials = counts.registryMaterials;
    cardinality.modelTables = counts.modelTables;
    cardinality.modelSurfaceTokens = counts.modelSurfaceTokens;
    cardinality.applyGateKeys = counts.applyGateKeys;
    cardinality.instanceMeshes = counts.instanceMeshes;
    cardinality.instanceHistories = counts.instanceHistories;
    cardinality.geometryStatic = counts.geometryStaticSurfaces;
    cardinality.geometryRoutes = counts.geometryRigidRoutes;
    cardinality.geometryResidents = counts.geometryRigidResidents;
    cardinality.receiptSurfaces = counts.receiptSurfaces;
    cardinality.materialInfoIntents = counts.materialInfoIntents;
    cardinality.materialVariants = counts.materialVariantProposals;
    cardinality.instanceObservations = counts.instanceObservationProposals;
    cardinality.rigidCandidates = counts.rigidCandidateProposals;
    cardinality.preparedRigidPayloads = counts.preparedRigidPayloads;
    cardinality.preparedRigidVertices = counts.preparedRigidVertices;
    cardinality.preparedRigidIndexes = counts.preparedRigidIndexes;
    cardinality.frameMaterialIds = counts.frameMaterialIds;
    RtPathTraceCompleteSlotLayout layout;
    layout.snapshotFixed = snapshotFixedBytes;
    layout.rawSurface = sizeof(RtPathTraceCaptureRawSurface);
    layout.ownerDecision = sizeof(RtPathTraceCaptureSurfaceProduct);
    layout.stage = sizeof(RtSmokeTranslucentClassifierStageInput);
    layout.runtimeStage = sizeof(RtPathTraceRuntimeMaterialStagePod);
    layout.registerValue = sizeof(float);
    layout.rawVertex = sizeof(idDrawVert);
    layout.rawIndex = sizeof(triIndex_t);
    layout.joint = sizeof(idJointMat);
    layout.variantBase = sizeof(RtPathTraceMaterialTextureVariantBasePod);
    layout.registryMaterial = sizeof(RtPathTraceCaptureRegistryMaterialPod);
    layout.modelTable = sizeof(RtPathTraceCaptureModelTokenTablePod);
    layout.modelSurfaceToken = sizeof(std::uint64_t);
    layout.applyGateKey = sizeof(std::uint64_t);
    layout.instanceMesh = sizeof(RtPathTraceInstanceMeshRecordPod);
    layout.instanceHistory = sizeof(RtPathTraceInstanceHistoryPod);
    layout.geometryStatic = sizeof(RtSmokeStaticSurfacePod);
    layout.geometryRoute = sizeof(RtSmokeRigidRouteReadyPod);
    layout.geometryResident = sizeof(RtSmokeRigidResidentReadyPod);
    layout.productFixed = sizeof(RtPathTraceCaptureProduct);
    layout.productSurface = sizeof(RtPathTraceCaptureSurfaceProduct);
    layout.productVertex = sizeof(PathTraceSmokeVertex);
    layout.productIndex = sizeof(std::uint32_t);
    layout.triangleClass = sizeof(std::uint32_t);
    layout.triangleMaterial = sizeof(std::uint32_t);
    layout.triangleInstance = sizeof(std::uint64_t);
    layout.triangleIdentity = sizeof(std::uint32_t);
    layout.materialIntent = sizeof(RtPathTraceMaterialInfoRegistrationIntent);
    layout.materialVariant = sizeof(RtPathTraceMaterialVariantRegistrationProposal);
    layout.instanceObservation = sizeof(RtPathTraceInstanceObservationProposal);
    layout.rigidCandidate = sizeof(RtPathTraceRigidCandidateProposal);
    layout.preparedRigidPayload = sizeof(RtPathTraceRigidPreparedPayload);
    layout.preparedRigidVertex = sizeof(PathTraceSmokeVertex);
    layout.preparedRigidIndex = sizeof(std::uint32_t);
    layout.frameMaterialId = sizeof(std::uint32_t);
    layout.membershipOrdinal = sizeof(std::uint32_t);
    layout.receiptSurface =
        sizeof(RtPathTraceCommittedReferencedSurfaceReceiptRow);
    layout.oracleFixed = sizeof(RtPathTraceCaptureOracle);
    layout.oracleSurface = sizeof(RtPathTraceCaptureOracleSurface);
    RtPathTraceCompleteSlotPlan sharedPlan;
    if (!RtPathTracePlanCompleteSlot(cardinality, layout,
            RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES, sharedPlan))
    {
        plan = {};
        return false;
    }
    plan.snapshotBytes = sharedPlan.snapshotBytes;
    plan.candidateBytes = sharedPlan.candidateBytes;
    plan.finalProductBytes = sharedPlan.finalProductBytes;
    plan.oracleBytes = sharedPlan.oracleBytes;
    plan.peakSlotBytes = sharedPlan.peakBytes;
    return true;
}

bool ReservePathTraceCaptureOwnerSnapshotStorage(
    RtPathTraceCaptureOwnerSnapshot& snapshot,
    std::size_t existingSlotBytes,
    const RtPathTraceCaptureProductCapacityPlan& plan,
    RtPathTraceCaptureReserveTestSeam* testSeam)
{
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try
    {
#endif
        if (plan.snapshotBytes < sizeof(snapshot) + existingSlotBytes)
        {
            snapshot.ResetAndRelease();
            return false;
        }
        std::size_t remaining = plan.snapshotBytes - sizeof(snapshot) - existingSlotBytes;
        std::size_t fixedAndOther = existingSlotBytes;
        if (!RtPathTraceProducerCheckedAddBytes(fixedAndOther,
                plan.candidateBytes, RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES) ||
            !RtPathTraceProducerCheckedAddBytes(fixedAndOther,
                plan.oracleBytes, RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES))
        {
            snapshot.ResetAndRelease();
            return false;
        }
        auto reserve = [&](auto& values, std::size_t count)
        {
            using Value = typename std::decay_t<decltype(values)>::value_type;
            if (count > std::numeric_limits<std::size_t>::max() / sizeof(Value))
            {
                return false;
            }
            const std::size_t requested = count * sizeof(Value);
            if (requested > remaining)
            {
                return false;
            }
            bool skip = false;
            std::size_t inflateBy = 0;
            BeforeReserve(testSeam, skip, inflateBy);
            if (!skip)
            {
                if (inflateBy > std::numeric_limits<std::size_t>::max() - count)
                {
                    return false;
                }
                values.reserve(count + inflateBy);
            }
            remaining -= requested;
            return RtPathTraceReconcileReservedCapacity(fixedAndOther,
                snapshot.OwnedBytes(), remaining,
                RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES);
        };
        const RtPathTraceCaptureCapacityCounts& count = snapshot.capacityCounts;
        if (!reserve(snapshot.applyGate.omittedSkinKeys, count.applyGateKeys) ||
            !reserve(snapshot.instanceUniverse.meshes, count.instanceMeshes) ||
            !reserve(snapshot.instanceUniverse.histories, count.instanceHistories) ||
            !reserve(snapshot.geometryUniverse.staticSurfaces, count.geometryStaticSurfaces) ||
            !reserve(snapshot.geometryUniverse.rigidRoutes, count.geometryRigidRoutes) ||
            !reserve(snapshot.geometryUniverse.rigidResidents, count.geometryRigidResidents) ||
            !reserve(snapshot.variantBases, count.variantBases) ||
            !reserve(snapshot.registryMaterials, count.registryMaterials) ||
            !reserve(snapshot.modelTables, count.modelTables) ||
            !reserve(snapshot.modelSurfaceTokens, count.modelSurfaceTokens) ||
            !reserve(snapshot.surfaces, count.surfaces) ||
            !reserve(snapshot.ownerDecisions, count.surfaces) ||
            !reserve(snapshot.classifierStages, count.classifierStages) ||
            !reserve(snapshot.runtimeStages, count.runtimeStages) ||
            !reserve(snapshot.registers, count.registers) ||
            !reserve(snapshot.vertices, count.vertices) ||
            !reserve(snapshot.indexes, count.indexes) ||
            !reserve(snapshot.joints, count.joints) ||
            remaining != 0 || !OwnerStorageReady(snapshot))
        {
            snapshot.ResetAndRelease();
            return false;
        }
        return true;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    }
    catch (const std::bad_alloc&)
    {
        snapshot.ResetAndRelease();
        return false;
    }
    catch (const std::length_error&)
    {
        snapshot.ResetAndRelease();
        return false;
    }
#endif
}

bool PathTraceCaptureProductStorageReady(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    const RtPathTraceCaptureProduct& value)
{
    const std::size_t surfaces = snapshot.capacityCounts.surfaces;
    const std::size_t triangles = snapshot.capacityCounts.indexes / 3;
    return value.surfaces.capacity() >= surfaces &&
        value.vertices.capacity() >= snapshot.capacityCounts.vertices &&
        value.indexes.capacity() >= snapshot.capacityCounts.indexes &&
        value.triangleClasses.capacity() >= triangles &&
        value.triangleMaterials.capacity() >= triangles &&
        value.triangleInstances.capacity() >= triangles &&
        value.triangleIdentities.capacity() >= triangles &&
        value.materialInfoIntents.capacity() >= snapshot.capacityCounts.materialInfoIntents &&
        value.materialVariants.capacity() >= snapshot.capacityCounts.materialVariantProposals &&
        value.instanceObservations.capacity() >= snapshot.capacityCounts.instanceObservationProposals &&
        value.rigidCandidates.capacity() >= snapshot.capacityCounts.rigidCandidateProposals &&
        value.preparedRigidPayloads.capacity() >=
            snapshot.capacityCounts.preparedRigidPayloads &&
        value.frameMaterialIds.capacity() >= snapshot.capacityCounts.frameMaterialIds &&
        value.receiptSurfaces.capacity() >=
            snapshot.capacityCounts.receiptSurfaces &&
        value.staticMembershipSurfaces.capacity() >= surfaces &&
        value.routedReadySkipSurfaces.capacity() >= surfaces &&
        ProductWithinBounds(snapshot, value);
}

RtPathTraceCaptureProductCapacityStamp CapturePathTraceProductCapacityStamp(
    const RtPathTraceCaptureProduct& value)
{
    RtPathTraceCaptureProductCapacityStamp stamp;
    stamp.capacities = {
        value.surfaces.capacity(),
        value.vertices.capacity(),
        value.indexes.capacity(),
        value.triangleClasses.capacity(),
        value.triangleMaterials.capacity(),
        value.triangleInstances.capacity(),
        value.triangleIdentities.capacity(),
        value.materialInfoIntents.capacity(),
        value.materialVariants.capacity(),
        value.instanceObservations.capacity(),
        value.rigidCandidates.capacity(),
        value.preparedRigidPayloads.capacity(),
        value.frameMaterialIds.capacity(),
        value.receiptSurfaces.capacity(),
        value.staticMembershipSurfaces.capacity(),
        value.routedReadySkipSurfaces.capacity()
    };
    return stamp;
}

bool PathTraceCaptureProductCapacityUnchanged(
    const RtPathTraceCaptureProduct& value,
    const RtPathTraceCaptureProductCapacityStamp& stamp)
{
    return CapturePathTraceProductCapacityStamp(value).capacities ==
        stamp.capacities;
}

bool RtPathTraceCaptureProductShapeValid(
    const RtPathTraceCaptureProduct& value) noexcept
{
    const RtPathTraceCaptureCapacityCounts& count = value.capacityCounts;
    const std::size_t triangles = count.indexes / 3u;
    if (!value.complete || value.provenance ==
            RtPathTraceCaptureProductProvenance::None ||
        value.inputOwnedBytes == 0 ||
        value.actualOwnedBytes != value.OwnedBytes() ||
        value.surfaces.size() != count.surfaces ||
        value.vertices.size() > count.vertices ||
        value.indexes.size() > count.indexes ||
        value.triangleClasses.size() > triangles ||
        value.triangleMaterials.size() > triangles ||
        value.triangleInstances.size() > triangles ||
        value.triangleIdentities.size() > triangles ||
        value.materialInfoIntents.size() > count.materialInfoIntents ||
        value.materialVariants.size() > count.materialVariantProposals ||
        value.instanceObservations.size() > count.instanceObservationProposals ||
        value.rigidCandidates.size() > count.rigidCandidateProposals ||
        value.preparedRigidPayloads.size() > count.preparedRigidPayloads ||
        value.frameMaterialIds.size() > count.frameMaterialIds ||
        value.staticMembershipSurfaces.size() > count.surfaces ||
        value.routedReadySkipSurfaces.size() > count.surfaces ||
        !PathTraceCaptureProductSpansCanonical(value))
    {
        return false;
    }

    if (value.provenance == RtPathTraceCaptureProductProvenance::OwnerDecisions)
    {
        const bool legacyPreparedPayloadAbsent =
            value.rigidCandidates.empty() && value.preparedRigidPayloads.empty();
        const bool preparedPayloadComplete =
            !value.rigidCandidates.empty() &&
            PreparedRigidPayloadShapeValid(value, false);
        return value.ownerDecisionGeometryOnly &&
            value.membershipReceipt.complete &&
            !value.receiptHeader.complete && value.receiptSurfaces.empty() &&
            value.materialInfoIntents.empty() && value.materialVariants.empty() &&
            value.instanceObservations.empty() &&
            (legacyPreparedPayloadAbsent || preparedPayloadComplete) &&
            value.frameMaterialIds.empty() && count.frameMaterialIds == 0 &&
            value.finalizedMaterialIdCount == 0 &&
            value.finalizedMaterialIdHash == 0 &&
            value.receiptHeader.finalizedMaterialIdCount == 0 &&
            value.receiptHeader.finalizedMaterialIdHash == 0;
    }
    if (value.provenance != RtPathTraceCaptureProductProvenance::PureSemantic ||
        value.ownerDecisionGeometryOnly ||
        !value.receiptHeader.complete ||
        !PreparedRigidPayloadShapeValid(value, true) ||
        (value.surfaces.empty() != value.frameMaterialIds.empty()) ||
        value.frameMaterialIds.size() != count.frameMaterialIds ||
        value.finalizedMaterialIdCount != value.frameMaterialIds.size() ||
        value.finalizedMaterialIdHash != RtPathTraceFinalizedMaterialIdHash(
            value.frameMaterialIds.data(), value.frameMaterialIds.size()) ||
        value.finalizedMaterialIdCount !=
            value.receiptHeader.finalizedMaterialIdCount ||
        value.finalizedMaterialIdHash !=
            value.receiptHeader.finalizedMaterialIdHash ||
        value.receiptHeader.finalizedMaterialIdCount !=
            value.frameMaterialIds.size() ||
        value.receiptHeader.finalizedMaterialIdHash !=
            RtPathTraceFinalizedMaterialIdHash(
                value.frameMaterialIds.data(), value.frameMaterialIds.size()) ||
        !RtPathTracePureSemanticProposalPayloadShapeValid(
            count, value.epoch, value.receiptHeader,
            value.receiptSurfaces.data(), value.receiptSurfaces.size(),
            value.materialInfoIntents.data(), value.materialInfoIntents.size(),
            value.materialVariants.data(), value.materialVariants.size(),
            value.instanceObservations.data(), value.instanceObservations.size(),
            value.rigidCandidates.data(), value.rigidCandidates.size()))
    {
        return false;
    }
    const RtPathTraceCommittedReferencedSetReceiptHeader& receipt =
        value.receiptHeader;
    if (value.frameMaterialIds.size() > value.surfaces.size() &&
        value.frameMaterialIds.size() - value.surfaces.size() >
            value.surfaces.size())
    {
        return false;
    }
    for (std::size_t index = 1; index < value.frameMaterialIds.size(); ++index)
    {
        if (value.frameMaterialIds[index - 1] >= value.frameMaterialIds[index])
        {
            return false;
        }
    }
    return receipt.sealedPrimaryViewToken != 0 &&
        receipt.ownerUniverseFrameIndex != 0 &&
        receipt.baselineFrameIndex != 0 &&
        receipt.worldLifecycleGeneration != 0 && receipt.mapLoadSerial != 0 &&
        receipt.barrierGeneration != 0 &&
        receipt.materialRegistryGeneration != 0 &&
        receipt.residentMaterialFactsGeneration != 0 &&
        receipt.configFingerprint != 0 &&
        receipt.instanceUniverseGeneration != 0 &&
        receipt.geometryUniverseGeneration != 0 &&
        receipt.staticMaterialGeneration != 0 &&
        receipt.canonicalSourceIndexPoolGeneration != 0 &&
        receipt.staticResidentPayloadGeneration != 0;
}

bool ReservePathTraceCaptureProductStorage(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    RtPathTraceCaptureProduct& value,
    std::size_t otherOwnedBytes,
    std::size_t plannedProductBytes,
    RtPathTraceCaptureReserveTestSeam* testSeam)
{
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try
    {
#endif
        if (plannedProductBytes < sizeof(value))
        {
            value.ResetAndRelease();
            return false;
        }
        const RtPathTraceCaptureCapacityCounts& count = snapshot.capacityCounts;
        const std::size_t surfaces = count.surfaces;
        const std::size_t triangles = count.indexes / 3;
        std::size_t requestedProductBytes = sizeof(value);
        const auto addRequested = [&](std::size_t cardinality,
                                      std::size_t elementBytes)
        {
            return RtPathTraceProducerCheckedArrayBytes(cardinality,
                elementBytes, requestedProductBytes, plannedProductBytes);
        };
        if (!addRequested(surfaces, sizeof(RtPathTraceCaptureSurfaceProduct)) ||
            !addRequested(count.vertices, sizeof(PathTraceSmokeVertex)) ||
            !addRequested(count.indexes, sizeof(std::uint32_t)) ||
            !addRequested(triangles, sizeof(std::uint32_t)) ||
            !addRequested(triangles, sizeof(std::uint32_t)) ||
            !addRequested(triangles, sizeof(std::uint32_t)) ||
            !addRequested(triangles, sizeof(std::uint64_t)) ||
            !addRequested(count.materialInfoIntents,
                sizeof(RtPathTraceMaterialInfoRegistrationIntent)) ||
            !addRequested(count.materialVariantProposals,
                sizeof(RtPathTraceMaterialVariantRegistrationProposal)) ||
            !addRequested(count.instanceObservationProposals,
                sizeof(RtPathTraceInstanceObservationProposal)) ||
            !addRequested(count.rigidCandidateProposals,
                sizeof(RtPathTraceRigidCandidateProposal)) ||
            !addRequested(count.preparedRigidPayloads,
                sizeof(RtPathTraceRigidPreparedPayload)) ||
            !addRequested(count.preparedRigidVertices,
                sizeof(PathTraceSmokeVertex)) ||
            !addRequested(count.preparedRigidIndexes,
                sizeof(std::uint32_t)) ||
            !addRequested(count.frameMaterialIds, sizeof(std::uint32_t)) ||
            !addRequested(count.receiptSurfaces,
                sizeof(RtPathTraceCommittedReferencedSurfaceReceiptRow)) ||
            !addRequested(surfaces, sizeof(std::uint32_t)) ||
            !addRequested(surfaces, sizeof(std::uint32_t)))
        {
            value.ResetAndRelease();
            return false;
        }
        std::size_t remaining = requestedProductBytes - sizeof(value);
        auto reserve = [&](auto& values, std::size_t count)
        {
            using Value = typename std::decay_t<decltype(values)>::value_type;
            if (count > std::numeric_limits<std::size_t>::max() / sizeof(Value))
            {
                return false;
            }
            const std::size_t requested = count * sizeof(Value);
            if (requested > remaining)
            {
                return false;
            }
            bool skip = false;
            std::size_t inflateBy = 0;
            BeforeReserve(testSeam, skip, inflateBy);
            if (!skip)
            {
                if (inflateBy > std::numeric_limits<std::size_t>::max() - count)
                {
                    return false;
                }
                values.reserve(count + inflateBy);
            }
            remaining -= requested;
            std::size_t projectedProductBytes = value.OwnedBytes();
            if (!RtPathTraceProducerCheckedAddBytes(projectedProductBytes,
                    remaining, plannedProductBytes))
            {
                return false;
            }
            std::size_t projectedSlotBytes = otherOwnedBytes;
            return RtPathTraceProducerCheckedAddBytes(projectedSlotBytes,
                projectedProductBytes, RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES);
        };
        if (!reserve(value.surfaces, surfaces) ||
            !reserve(value.vertices, snapshot.capacityCounts.vertices) ||
            !reserve(value.indexes, snapshot.capacityCounts.indexes) ||
            !reserve(value.triangleClasses, triangles) ||
            !reserve(value.triangleMaterials, triangles) ||
            !reserve(value.triangleInstances, triangles) ||
            !reserve(value.triangleIdentities, triangles) ||
            !reserve(value.materialInfoIntents,
                snapshot.capacityCounts.materialInfoIntents) ||
            !reserve(value.materialVariants,
                snapshot.capacityCounts.materialVariantProposals) ||
            !reserve(value.instanceObservations,
                snapshot.capacityCounts.instanceObservationProposals) ||
            !reserve(value.rigidCandidates,
                snapshot.capacityCounts.rigidCandidateProposals) ||
            !reserve(value.preparedRigidPayloads,
                snapshot.capacityCounts.preparedRigidPayloads) ||
            !reserve(value.frameMaterialIds,
                snapshot.capacityCounts.frameMaterialIds) ||
            !reserve(value.receiptSurfaces,
                snapshot.capacityCounts.receiptSurfaces) ||
            !reserve(value.staticMembershipSurfaces, surfaces) ||
            !reserve(value.routedReadySkipSurfaces, surfaces) ||
            !PathTraceCaptureProductStorageReady(snapshot, value))
        {
            value.ResetAndRelease();
            return false;
        }
        std::size_t preparedNestedBytes = 0;
        if (!RtPathTraceProducerCheckedArrayBytes(
                count.preparedRigidVertices, sizeof(PathTraceSmokeVertex),
                preparedNestedBytes, plannedProductBytes) ||
            !RtPathTraceProducerCheckedArrayBytes(
                count.preparedRigidIndexes, sizeof(std::uint32_t),
                preparedNestedBytes, plannedProductBytes) ||
            remaining != preparedNestedBytes)
        {
            value.ResetAndRelease();
            return false;
        }
        return true;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    }
    catch (const std::bad_alloc&)
    {
        value.ResetAndRelease();
        return false;
    }
    catch (const std::length_error&)
    {
        value.ResetAndRelease();
        return false;
    }
#endif
}

bool ReservePathTraceCaptureOracleStorage(
    const RtPathTraceCaptureCapacityCounts& counts,
    std::size_t snapshotAndProductOwnedBytes,
    RtPathTraceCaptureOracle& oracle,
    RtPathTraceCaptureReserveTestSeam* testSeam)
{
    oracle.ResetAndRelease();
    const std::size_t triangles = counts.indexes / 3;
    std::size_t remaining = 0;
    auto plan = [&](std::size_t count, std::size_t element)
    {
        return RtPathTraceProducerCheckedArrayBytes(count, element, remaining,
            RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES);
    };
    if (!plan(counts.vertices, sizeof(PathTraceSmokeVertex)) ||
        !plan(counts.indexes, sizeof(std::uint32_t)) ||
        !plan(triangles, sizeof(std::uint32_t)) ||
        !plan(triangles, sizeof(std::uint32_t)) ||
        !plan(triangles, sizeof(std::uint64_t)) ||
        !plan(triangles, sizeof(std::uint32_t)) ||
        !plan(counts.surfaces, sizeof(RtPathTraceMaterialInfoRegistrationIntent)) ||
        !plan(counts.surfaces, sizeof(RtPathTraceMaterialVariantRegistrationProposal)) ||
        !plan(counts.surfaces, sizeof(RtPathTraceInstanceObservationProposal)) ||
        !plan(counts.surfaces, sizeof(RtPathTraceRigidCandidateProposal)) ||
        !plan(counts.surfaces, sizeof(std::uint32_t)) ||
        !plan(counts.surfaces, sizeof(std::uint32_t)) ||
        !plan(counts.surfaces, sizeof(RtPathTraceCaptureOracleSurface)) ||
        !RtPathTraceReconcileReservedCapacity(snapshotAndProductOwnedBytes,
            sizeof(oracle), remaining, RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES))
    {
        oracle.ResetAndRelease();
        return false;
    }
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try
    {
#endif
        auto reserve = [&](auto& values, std::size_t count)
        {
            using Value = typename std::decay_t<decltype(values)>::value_type;
            const std::size_t requested = count * sizeof(Value);
            if (requested > remaining)
            {
                return false;
            }
            bool skip = false;
            std::size_t inflateBy = 0;
            BeforeReserve(testSeam, skip, inflateBy);
            if (!skip)
            {
                if (inflateBy > std::numeric_limits<std::size_t>::max() - count)
                {
                    return false;
                }
                values.reserve(count + inflateBy);
            }
            remaining -= requested;
            return RtPathTraceReconcileReservedCapacity(snapshotAndProductOwnedBytes,
                oracle.OwnedBytes(), remaining,
                RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES);
        };
        if (!reserve(oracle.vertices, counts.vertices) ||
            !reserve(oracle.indexes, counts.indexes) ||
            !reserve(oracle.triangleClasses, triangles) ||
            !reserve(oracle.triangleMaterials, triangles) ||
            !reserve(oracle.triangleInstances, triangles) ||
            !reserve(oracle.triangleIdentities, triangles) ||
            !reserve(oracle.materialInfoIntents, counts.surfaces) ||
            !reserve(oracle.materialVariants, counts.surfaces) ||
            !reserve(oracle.instanceObservations, counts.surfaces) ||
            !reserve(oracle.rigidCandidates, counts.surfaces) ||
            !reserve(oracle.staticMembershipSurfaces, counts.surfaces) ||
            !reserve(oracle.routedReadySkipSurfaces, counts.surfaces) ||
            !reserve(oracle.surfaces, counts.surfaces) ||
            remaining != 0)
        {
            oracle.ResetAndRelease();
            return false;
        }
        return true;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    }
    catch (const std::bad_alloc&)
    {
        oracle.ResetAndRelease();
        return false;
    }
    catch (const std::length_error&)
    {
        oracle.ResetAndRelease();
        return false;
    }
#endif
}

namespace
{
bool PathTraceCaptureProductSpansCanonicalImpl(
    const RtPathTraceCaptureProduct& product, bool requireComplete)
{
    const std::uint64_t uint32Max =
        std::numeric_limits<std::uint32_t>::max();
    if ((requireComplete && !product.complete) ||
        product.surfaces.size() > uint32Max ||
        product.vertices.size() > uint32Max ||
        product.indexes.size() > uint32Max ||
        product.triangleClasses.size() > uint32Max ||
        product.triangleClasses.size() != product.triangleMaterials.size() ||
        product.triangleClasses.size() != product.triangleInstances.size() ||
        product.triangleClasses.size() != product.triangleIdentities.size() ||
        (product.indexes.size() % 3u) != 0u ||
        product.indexes.size() / 3u != product.triangleClasses.size())
    {
        return false;
    }

    for (std::size_t ordinal = 0; ordinal < product.surfaces.size(); ++ordinal)
    {
        const RtPathTraceCaptureSurfaceProduct& surface =
            product.surfaces[ordinal];
        if (surface.ordinal != ordinal)
        {
            return false;
        }
        if (!PathTraceOwnerDecisionEmitsGeometry(surface))
        {
            if (surface.vertexOffset != 0 || surface.vertexCount != 0 ||
                surface.indexOffset != 0 || surface.indexCount != 0 ||
                surface.triangleOffset != 0 || surface.triangleCount != 0)
            {
                return false;
            }
            continue;
        }

        const int classBucket = static_cast<int>(surface.surfaceClass);
        if (surface.bucketIndex < 0 ||
            surface.bucketIndex >= RT_SMOKE_CLASS_COUNT ||
            surface.bucketIndex != classBucket || surface.vertexCount == 0 ||
            surface.indexCount == 0 || (surface.indexCount % 3u) != 0u ||
            surface.triangleCount != surface.indexCount / 3u ||
            (surface.indexOffset % 3u) != 0u ||
            surface.triangleOffset != surface.indexOffset / 3u)
        {
            return false;
        }

        const std::uint64_t vertexEnd =
            static_cast<std::uint64_t>(surface.vertexOffset) +
            surface.vertexCount;
        const std::uint64_t indexEnd =
            static_cast<std::uint64_t>(surface.indexOffset) +
            surface.indexCount;
        const std::uint64_t triangleEnd =
            static_cast<std::uint64_t>(surface.triangleOffset) +
            surface.triangleCount;
        if (vertexEnd > product.vertices.size() || vertexEnd > uint32Max ||
            indexEnd > product.indexes.size() || indexEnd > uint32Max ||
            triangleEnd > product.triangleClasses.size() ||
            triangleEnd > uint32Max)
        {
            return false;
        }
    }

    std::uint64_t vertexCursor = 0;
    std::uint64_t indexCursor = 0;
    std::uint64_t triangleCursor = 0;
    for (int bucket = 0; bucket < RT_SMOKE_CLASS_COUNT; ++bucket)
    {
        for (const RtPathTraceCaptureSurfaceProduct& surface : product.surfaces)
        {
            if (!PathTraceOwnerDecisionEmitsGeometry(surface) ||
                surface.bucketIndex != bucket)
            {
                continue;
            }
            if (surface.vertexOffset != vertexCursor ||
                surface.indexOffset != indexCursor ||
                surface.triangleOffset != triangleCursor)
            {
                return false;
            }
            const std::uint64_t vertexEnd = vertexCursor + surface.vertexCount;
            const std::uint64_t indexEnd = indexCursor + surface.indexCount;
            const std::uint64_t triangleEnd =
                triangleCursor + surface.triangleCount;
            if (vertexEnd > product.vertices.size() || vertexEnd > uint32Max ||
                indexEnd > product.indexes.size() || indexEnd > uint32Max ||
                triangleEnd > product.triangleClasses.size() ||
                triangleEnd > uint32Max)
            {
                return false;
            }
            for (std::uint64_t index = indexCursor; index < indexEnd; ++index)
            {
                const std::uint32_t vertex =
                    product.indexes[static_cast<std::size_t>(index)];
                if (vertex < vertexCursor || vertex >= vertexEnd)
                {
                    return false;
                }
            }
            vertexCursor = vertexEnd;
            indexCursor = indexEnd;
            triangleCursor = triangleEnd;
        }
    }
    return vertexCursor == product.vertices.size() &&
        indexCursor == product.indexes.size() &&
        triangleCursor == product.triangleClasses.size();
}
} // namespace

bool PathTraceCaptureProductSpansCanonical(
    const RtPathTraceCaptureProduct& product)
{
    return PathTraceCaptureProductSpansCanonicalImpl(product, true);
}

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
    int& sourceIndexes)
{
    if (!PathTraceCaptureProductSpansCanonical(product) ||
        !RtPathTracePlanningEpochValid(product.epoch))
    {
        return false;
    }

    std::uint64_t acceptedSurfaces = 0;
    std::uint64_t acceptedVertices = 0;
    std::uint64_t acceptedIndexes = 0;
    for (const RtPathTraceCaptureSurfaceProduct& surface : product.surfaces)
    {
        if (!PathTraceOwnerDecisionEmitsGeometry(surface))
        {
            continue;
        }
        ++acceptedSurfaces;
        acceptedVertices += surface.vertexCount;
        acceptedIndexes += surface.indexCount;
    }

    const std::uint64_t intMax = std::numeric_limits<int>::max();
    const std::uint64_t uint32Max =
        std::numeric_limits<std::uint32_t>::max();
    const std::size_t oldVertices = vertices.size();
    const std::size_t oldIndexes = indexes.size();
    const std::size_t oldClasses = triangleClasses.size();
    const std::size_t oldMaterials = triangleMaterials.size();
    const std::size_t oldInstances = triangleInstances.size();
    const std::size_t oldIdentities = triangleIdentities.size();
    if (oldClasses != oldMaterials || oldClasses != oldInstances ||
        oldClasses != oldIdentities || oldIndexes / 3u != oldClasses ||
        (oldIndexes % 3u) != 0u || oldVertices > intMax ||
        oldIndexes > intMax || oldClasses > intMax ||
        product.vertices.size() > intMax - oldVertices ||
        product.indexes.size() > intMax - oldIndexes ||
        product.triangleClasses.size() > intMax - oldClasses ||
        product.vertices.size() > uint32Max - oldVertices ||
        acceptedSurfaces > intMax || acceptedVertices > intMax ||
        acceptedIndexes > intMax || sourceSurfaces < 0 || sourceVerts < 0 ||
        sourceIndexes < 0 || acceptedSurfaces > intMax - sourceSurfaces ||
        acceptedVertices > intMax - sourceVerts ||
        acceptedIndexes > intMax - sourceIndexes)
    {
        return false;
    }

#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try
    {
#endif
        vertices.reserve(oldVertices + product.vertices.size());
        indexes.reserve(oldIndexes + product.indexes.size());
        triangleClasses.reserve(oldClasses + product.triangleClasses.size());
        triangleMaterials.reserve(oldMaterials + product.triangleMaterials.size());
        triangleInstances.reserve(oldInstances + product.triangleInstances.size());
        triangleIdentities.reserve(oldIdentities + product.triangleIdentities.size());
        vertices.insert(vertices.end(), product.vertices.begin(), product.vertices.end());
        const std::uint32_t vertexBase = static_cast<std::uint32_t>(oldVertices);
        for (std::uint32_t index : product.indexes)
        {
            indexes.push_back(vertexBase + index);
        }
        triangleClasses.insert(triangleClasses.end(),
            product.triangleClasses.begin(), product.triangleClasses.end());
        triangleMaterials.insert(triangleMaterials.end(),
            product.triangleMaterials.begin(), product.triangleMaterials.end());
        triangleInstances.insert(triangleInstances.end(),
            product.triangleInstances.begin(), product.triangleInstances.end());
        triangleIdentities.insert(triangleIdentities.end(),
            product.triangleIdentities.begin(), product.triangleIdentities.end());
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    }
    catch (const std::bad_alloc&)
    {
        vertices.resize(oldVertices);
        indexes.resize(oldIndexes);
        triangleClasses.resize(oldClasses);
        triangleMaterials.resize(oldMaterials);
        triangleInstances.resize(oldInstances);
        triangleIdentities.resize(oldIdentities);
        return false;
    }
    catch (const std::length_error&)
    {
        vertices.resize(oldVertices);
        indexes.resize(oldIndexes);
        triangleClasses.resize(oldClasses);
        triangleMaterials.resize(oldMaterials);
        triangleInstances.resize(oldInstances);
        triangleIdentities.resize(oldIdentities);
        return false;
    }
#endif

    for (int bucket = 1; bucket < RT_SMOKE_CLASS_COUNT; ++bucket)
    {
        RtSmokeBucketRange& range = bucketRanges.buckets[bucket];
        range.vertexOffset = static_cast<int>(oldVertices);
        range.indexOffset = static_cast<int>(oldIndexes);
        range.triangleOffset = static_cast<int>(oldClasses);
        range.vertexCount = 0;
        range.indexCount = 0;
        range.triangleCount = 0;
        range.surfaceCount = 0;
        bool found = false;
        for (const RtPathTraceCaptureSurfaceProduct& surface : product.surfaces)
        {
            if (!PathTraceOwnerDecisionEmitsGeometry(surface) ||
                surface.bucketIndex != bucket)
            {
                continue;
            }
            if (!found)
            {
                range.vertexOffset += static_cast<int>(surface.vertexOffset);
                range.indexOffset += static_cast<int>(surface.indexOffset);
                range.triangleOffset += static_cast<int>(surface.triangleOffset);
                found = true;
            }
            range.vertexCount += static_cast<int>(surface.vertexCount);
            range.indexCount += static_cast<int>(surface.indexCount);
            range.triangleCount += static_cast<int>(surface.triangleCount);
            ++range.surfaceCount;
        }
    }
    sourceSurfaces += static_cast<int>(acceptedSurfaces);
    sourceVerts += static_cast<int>(acceptedVertices);
    sourceIndexes += static_cast<int>(acceptedIndexes);
    return true;
}

bool PartitionPathTraceCaptureProductBucketMajor(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    const RtPathTraceCaptureProduct& candidate,
    RtPathTraceCaptureProduct& partitioned)
{
    if (!PathTraceCaptureProductStorageReady(snapshot, partitioned) ||
        !ProductWithinBounds(snapshot, candidate))
    {
        partitioned.ResetAndRelease();
        return false;
    }
    const RtPathTraceCaptureProductCapacityStamp capacity =
        CapturePathTraceProductCapacityStamp(partitioned);
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try
    {
#endif
        partitioned.epoch = candidate.epoch;
        partitioned.lateConsumeToken = candidate.lateConsumeToken;
        partitioned.membershipReceipt = candidate.membershipReceipt;
        partitioned.viewIdentity = candidate.viewIdentity;
        partitioned.capacityCounts = candidate.capacityCounts;
        partitioned.receiptHeader = candidate.receiptHeader;
        partitioned.inputOwnedBytes = candidate.inputOwnedBytes;
        partitioned.ownerDecisionGeometryOnly =
            candidate.ownerDecisionGeometryOnly;
        partitioned.provenance = candidate.provenance;
        if (!AppendNoGrow(partitioned.surfaces, candidate.surfaces.data(),
                candidate.surfaces.size()))
        {
            partitioned.ResetAndRelease();
            return false;
        }
        for (int bucket = 0; bucket < RT_SMOKE_CLASS_COUNT; ++bucket)
        {
            for (std::size_t surfaceIndex = 0;
                surfaceIndex < candidate.surfaces.size(); ++surfaceIndex)
            {
                const RtPathTraceCaptureSurfaceProduct& source =
                    candidate.surfaces[surfaceIndex];
                if (!RtPathTraceProducerPartitionSelects(bucket,
                        static_cast<int>(source.surfaceClass),
                        source.terminal == RtPathTraceCaptureTerminal::Accepted))
                {
                    continue;
                }
                if (!RangeValid(source.vertexOffset, source.vertexCount,
                        candidate.vertices) ||
                    !RangeValid(source.indexOffset, source.indexCount,
                        candidate.indexes) ||
                    !RangeValid(source.triangleOffset, source.triangleCount,
                        candidate.triangleClasses) ||
                    !RangeValid(source.triangleOffset, source.triangleCount,
                        candidate.triangleMaterials) ||
                    !RangeValid(source.triangleOffset, source.triangleCount,
                        candidate.triangleInstances) ||
                    !RangeValid(source.triangleOffset, source.triangleCount,
                        candidate.triangleIdentities) ||
                    partitioned.vertices.size() > UINT32_MAX ||
                    partitioned.indexes.size() > UINT32_MAX ||
                    partitioned.triangleClasses.size() > UINT32_MAX)
                {
                    partitioned.ResetAndRelease();
                    return false;
                }
                RtPathTraceCaptureSurfaceProduct& destination =
                    partitioned.surfaces[surfaceIndex];
                const std::uint32_t baseVertex = static_cast<std::uint32_t>(
                    partitioned.vertices.size());
                destination.vertexOffset = baseVertex;
                destination.indexOffset = static_cast<std::uint32_t>(
                    partitioned.indexes.size());
                destination.triangleOffset = static_cast<std::uint32_t>(
                    partitioned.triangleClasses.size());
                destination.preAppendVertexOffset = destination.vertexOffset;
                destination.preAppendIndexOffset = destination.indexOffset;
                destination.preAppendTriangleOffset = destination.triangleOffset;
                if (!AppendNoGrow(partitioned.vertices,
                        candidate.vertices.data() + source.vertexOffset,
                        source.vertexCount))
                {
                    partitioned.ResetAndRelease();
                    return false;
                }
                for (std::uint32_t index = 0; index < source.indexCount; ++index)
                {
                    std::uint32_t rebased = 0;
                    if (!RtPathTraceProducerRebaseIndex(baseVertex,
                            candidate.indexes[source.indexOffset + index],
                            source.vertexOffset, rebased) ||
                        !PushNoGrow(partitioned.indexes, rebased))
                    {
                        partitioned.ResetAndRelease();
                        return false;
                    }
                }
                const std::size_t triangle = source.triangleOffset;
                if (!AppendNoGrow(partitioned.triangleClasses,
                        candidate.triangleClasses.data() + triangle,
                        source.triangleCount) ||
                    !AppendNoGrow(partitioned.triangleMaterials,
                        candidate.triangleMaterials.data() + triangle,
                        source.triangleCount) ||
                    !AppendNoGrow(partitioned.triangleInstances,
                        candidate.triangleInstances.data() + triangle,
                        source.triangleCount) ||
                    !AppendNoGrow(partitioned.triangleIdentities,
                        candidate.triangleIdentities.data() + triangle,
                        source.triangleCount))
                {
                    partitioned.ResetAndRelease();
                    return false;
                }
            }
        }
        if (!AppendNoGrow(partitioned.materialInfoIntents,
                candidate.materialInfoIntents.data(),
                candidate.materialInfoIntents.size()) ||
            !AppendNoGrow(partitioned.materialVariants,
                candidate.materialVariants.data(), candidate.materialVariants.size()) ||
            !AppendNoGrow(partitioned.instanceObservations,
                candidate.instanceObservations.data(),
                candidate.instanceObservations.size()) ||
            !AppendNoGrow(partitioned.rigidCandidates,
                candidate.rigidCandidates.data(), candidate.rigidCandidates.size()) ||
            !AppendNoGrow(partitioned.receiptSurfaces,
                candidate.receiptSurfaces.data(), candidate.receiptSurfaces.size()) ||
            !AppendNoGrow(partitioned.staticMembershipSurfaces,
                candidate.staticMembershipSurfaces.data(),
                candidate.staticMembershipSurfaces.size()) ||
            !AppendNoGrow(partitioned.routedReadySkipSurfaces,
                candidate.routedReadySkipSurfaces.data(),
                candidate.routedReadySkipSurfaces.size()) ||
            !ProductWithinBounds(snapshot, partitioned) ||
            !PathTraceCaptureProductSpansCanonicalImpl(partitioned, false) ||
            !PathTraceCaptureProductCapacityUnchanged(partitioned, capacity))
        {
            partitioned.ResetAndRelease();
            return false;
        }
        partitioned.workerCpuUs = candidate.workerCpuUs;
        partitioned.complete = true;
        partitioned.actualOwnedBytes = partitioned.OwnedBytes();
        return true;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    }
    catch (const std::bad_alloc&)
    {
        partitioned.ResetAndRelease();
        return false;
    }
    catch (const std::length_error&)
    {
        partitioned.ResetAndRelease();
        return false;
    }
#endif
}

// Cross-phase geometry authority only.  Owner/worker bookkeeping is compared
// separately by the full comparator because its coordinates and presence bits
// are intentionally phase-local.
bool PathTraceCaptureSurfaceAuthorityEqual(
    const RtPathTraceCaptureSurfaceProduct& lhs,
    const RtPathTraceCaptureSurfaceProduct& rhs)
{
    return lhs.ordinal == rhs.ordinal && lhs.terminal == rhs.terminal &&
        lhs.surfaceClass == rhs.surfaceClass &&
        lhs.translucentSubtype == rhs.translucentSubtype &&
        lhs.surfaceClassId == rhs.surfaceClassId &&
        lhs.materialId == rhs.materialId &&
        lhs.materialClassSignature == rhs.materialClassSignature &&
        lhs.sourceFlags == rhs.sourceFlags && lhs.meshHash == rhs.meshHash &&
        lhs.instanceId == rhs.instanceId &&
        lhs.vertexCount == rhs.vertexCount && lhs.indexCount == rhs.indexCount &&
        lhs.triangleCount == rhs.triangleCount &&
        lhs.invalidIndexCount == rhs.invalidIndexCount &&
        lhs.zeroAreaTriangleCount == rhs.zeroAreaTriangleCount &&
        lhs.staticMatch == rhs.staticMatch &&
        lhs.rigidReadyByMesh == rhs.rigidReadyByMesh &&
        lhs.rigidReadyByResident == rhs.rigidReadyByResident &&
        lhs.applyGateSkip == rhs.applyGateSkip &&
        lhs.compositeRoute == rhs.compositeRoute &&
        lhs.skinnedAdmission == rhs.skinnedAdmission &&
        lhs.skinnedCaptureOmitted == rhs.skinnedCaptureOmitted &&
        lhs.liquidPoolPromoted == rhs.liquidPoolPromoted &&
        lhs.alphaDiagnosticRejected == rhs.alphaDiagnosticRejected &&
        lhs.bucketIndex == rhs.bucketIndex;
}

bool PathTraceCaptureSurfaceDecisionsEqual(
    const RtPathTraceCaptureSurfaceProduct& lhs,
    const RtPathTraceCaptureSurfaceProduct& rhs)
{
    const auto bytesEqual = [](const void* a, const void* b, std::size_t size)
    {
        return size == 0 || std::memcmp(a, b, size) == 0;
    };
    const RtPathTraceRuntimeMaterialDecisionPod& lm = lhs.runtimeMaterial;
    const RtPathTraceRuntimeMaterialDecisionPod& rm = rhs.runtimeMaterial;
    const RtPathTraceRuntimeMaterialEvalPod& le = lm.eval;
    const RtPathTraceRuntimeMaterialEvalPod& re = rm.eval;
    bool runtimeEqual = le.result == re.result &&
        le.materialId == re.materialId &&
        le.selectedStageIndex == re.selectedStageIndex &&
        le.selectedStagePriority == re.selectedStagePriority &&
        le.enabledStages == re.enabledStages &&
        le.disabledStages == re.disabledStages &&
        le.colorStages == re.colorStages &&
        le.alphaStages == re.alphaStages &&
        le.alphaTestStages == re.alphaTestStages &&
        le.texMatrixStages == re.texMatrixStages &&
        le.dynamicImageStages == re.dynamicImageStages &&
        le.cinematicStages == re.cinematicStages &&
        le.guiRenderTargetStages == re.guiRenderTargetStages &&
        le.programStages == re.programStages &&
        le.selectedStageEmissive == re.selectedStageEmissive &&
        le.hasDiffuseStageColor == re.hasDiffuseStageColor &&
        le.orderedStageOverflow == re.orderedStageOverflow &&
        le.hasSurfaceOrigin == re.hasSurfaceOrigin &&
        le.condition == re.condition && le.alphaTest == re.alphaTest &&
        le.diffuseStageCondition == re.diffuseStageCondition &&
        le.orderedStageCount == re.orderedStageCount &&
        bytesEqual(le.color, re.color, sizeof(le.color)) &&
        bytesEqual(le.texMatrix, re.texMatrix, sizeof(le.texMatrix)) &&
        bytesEqual(le.diffuseStageColor, re.diffuseStageColor,
            sizeof(le.diffuseStageColor)) &&
        bytesEqual(le.surfaceOrigin, re.surfaceOrigin,
            sizeof(le.surfaceOrigin));
    for (std::uint32_t index = 0;
        runtimeEqual && index < le.orderedStageCount; ++index)
    {
        const RtPathTraceRuntimeStageEvalPod& ls = le.orderedStages[index];
        const RtPathTraceRuntimeStageEvalPod& rs = re.orderedStages[index];
        runtimeEqual = ls.stageIndex == rs.stageIndex &&
            ls.enabled == rs.enabled && ls.emissive == rs.emissive &&
            ls.hasAlphaTest == rs.hasAlphaTest &&
            ls.hasTexMatrix == rs.hasTexMatrix &&
            ls.condition == rs.condition && ls.alphaTest == rs.alphaTest &&
            bytesEqual(ls.color, rs.color, sizeof(ls.color)) &&
            bytesEqual(ls.texMatrix, rs.texMatrix, sizeof(ls.texMatrix));
    }
    runtimeEqual = runtimeEqual &&
        lm.initialCandidateId == rm.initialCandidateId &&
        lm.chosenMaterialId == rm.chosenMaterialId &&
        lm.collisionCount == rm.collisionCount &&
        lm.fallbackUsed == rm.fallbackUsed &&
        lm.baseMaterialRegistered == rm.baseMaterialRegistered;
    return runtimeEqual && PathTraceCaptureSurfaceAuthorityEqual(lhs, rhs) &&
        lhs.vertexOffset == rhs.vertexOffset &&
        lhs.indexOffset == rhs.indexOffset &&
        lhs.triangleOffset == rhs.triangleOffset &&
        lhs.decisionPresence == rhs.decisionPresence &&
        lhs.sourceState == rhs.sourceState &&
        lhs.preAppendVertexOffset == rhs.preAppendVertexOffset &&
        lhs.preAppendIndexOffset == rhs.preAppendIndexOffset &&
        lhs.preAppendTriangleOffset == rhs.preAppendTriangleOffset &&
        lhs.capturedRecordVertexCount == rhs.capturedRecordVertexCount &&
        lhs.capturedRecordIndexCount == rhs.capturedRecordIndexCount &&
        lhs.capturedRecordTriangleCount == rhs.capturedRecordTriangleCount &&
        lhs.skinnedRecordVertexCount == rhs.skinnedRecordVertexCount &&
        lhs.skinnedRecordIndexCount == rhs.skinnedRecordIndexCount &&
        lhs.skinnedRecordTriangleCount == rhs.skinnedRecordTriangleCount &&
        lhs.mergedWalkVertexCount == rhs.mergedWalkVertexCount &&
        lhs.mergedWalkIndexCount == rhs.mergedWalkIndexCount &&
        lhs.mergedWalkTriangleCount == rhs.mergedWalkTriangleCount &&
        lhs.mergedCompanionRigidId == rhs.mergedCompanionRigidId &&
        lhs.mergedCompanionSkinnedId == rhs.mergedCompanionSkinnedId &&
        lhs.capturedRecordPresent == rhs.capturedRecordPresent &&
        lhs.skinnedRecordPresent == rhs.skinnedRecordPresent &&
        lhs.mergedWalkRecordPresent == rhs.mergedWalkRecordPresent &&
        lhs.bucketRangePublished == rhs.bucketRangePublished &&
        lhs.admissionPrecheckPassed == rhs.admissionPrecheckPassed &&
        lhs.appendAttempted == rhs.appendAttempted &&
        lhs.rollbackApplied == rhs.rollbackApplied;
}

namespace
{
template<typename T>
bool CaptureVectorBytesEqual(const std::vector<T>& lhs, const std::vector<T>& rhs)
{
    return lhs.size() == rhs.size() && (lhs.empty() ||
        std::memcmp(lhs.data(), rhs.data(), lhs.size() * sizeof(T)) == 0);
}

template<typename T, typename Equal>
bool CaptureSemanticVectorEqual(
    const std::vector<T>& lhs, const std::vector<T>& rhs, Equal equal)
{
    return lhs.size() == rhs.size() &&
        std::equal(lhs.begin(), lhs.end(), rhs.begin(), equal);
}
} // namespace

RtPathTraceCaptureComparison ComparePathTraceCaptureProduct(
    const RtPathTraceCaptureProduct& product,
    const RtPathTraceCaptureOracle& oracle)
{
    RtPathTraceCaptureComparison result;
    result.generation = product.epoch.generation;
    const auto noteFirst = [&result](RtPathTraceCaptureFirstFailure failure,
        std::uint32_t surfaceOrdinal = UINT32_MAX)
    {
        if (result.firstFail == RtPathTraceCaptureFirstFailure::None)
        {
            result.firstFail = failure;
            result.firstSurfaceOrdinal = surfaceOrdinal;
        }
    };
    if (!product.complete || !oracle.complete)
    {
        result.geometryMismatches = 1;
        result.proposalMismatches = 1;
        noteFirst(RtPathTraceCaptureFirstFailure::Incomplete);
        return result;
    }
    if (!RtPathTracePlanningEpochsMatch(product.epoch, oracle.epoch))
    {
        result.geometryMismatches = 1;
        result.proposalMismatches = 1;
        noteFirst(RtPathTraceCaptureFirstFailure::Epoch);
        return result;
    }
    if (product.viewIdentity != oracle.viewIdentity)
    {
        result.geometryMismatches = 1;
        result.proposalMismatches = 1;
        noteFirst(RtPathTraceCaptureFirstFailure::ViewIdentity);
        return result;
    }
    if (product.ownerDecisionGeometryOnly &&
        !PathTraceCaptureProductSpansCanonical(product))
    {
        result.geometryMismatches = 1;
        noteFirst(RtPathTraceCaptureFirstFailure::SurfaceDecision);
        return result;
    }
    const auto acceptedSurfaceCount = [](const auto& surfaces)
    {
        std::uint64_t accepted = 0;
        for (const auto& surface : surfaces)
        {
            accepted += surface.terminal == RtPathTraceCaptureTerminal::Accepted;
        }
        return accepted;
    };
    result.cardinality = RtPathTraceBuildCaptureCardinalityAttribution(
        acceptedSurfaceCount(product.surfaces),
        static_cast<std::uint64_t>(product.vertices.size()),
        acceptedSurfaceCount(oracle.surfaces),
        static_cast<std::uint64_t>(oracle.vertices.size()),
        oracle.liveCardinality);
    result.cardinalityAvailable = true;
    const auto hasDuplicateOrdinals = [](const auto& surfaces)
    {
        for (std::size_t lhs = 0; lhs < surfaces.size(); ++lhs)
        {
            for (std::size_t rhs = lhs + 1; rhs < surfaces.size(); ++rhs)
            {
                if (surfaces[lhs].ordinal == surfaces[rhs].ordinal)
                {
                    return true;
                }
            }
        }
        return false;
    };
    if (!hasDuplicateOrdinals(product.surfaces) &&
        !hasDuplicateOrdinals(oracle.surfaces))
    {
        result.acceptedSetDiff.available = true;
        const auto findOrdinal = [](const auto& surfaces, std::uint32_t ordinal)
            -> const RtPathTraceCaptureSurfaceProduct*
        {
            for (const auto& surface : surfaces)
            {
                if (surface.ordinal == ordinal)
                {
                    return &surface;
                }
            }
            return nullptr;
        };
        const auto acceptedAtOrdinal = [&findOrdinal](const auto& surfaces,
            std::uint32_t ordinal)
        {
            const RtPathTraceCaptureSurfaceProduct* row =
                findOrdinal(surfaces, ordinal);
            return row && row->terminal == RtPathTraceCaptureTerminal::Accepted;
        };
        std::uint32_t lowestDifference = UINT32_MAX;
        const auto consider = [&](std::uint32_t ordinal)
        {
            if (acceptedAtOrdinal(product.surfaces, ordinal) !=
                    acceptedAtOrdinal(oracle.surfaces, ordinal) &&
                ordinal < lowestDifference)
            {
                lowestDifference = ordinal;
            }
        };
        for (const auto& surface : product.surfaces) consider(surface.ordinal);
        for (const auto& surface : oracle.surfaces) consider(surface.ordinal);
        if (lowestDifference != UINT32_MAX)
        {
            result.acceptedSetDiff.found = true;
            result.acceptedSetDiff.ordinal = lowestDifference;
            const auto copyScalar = [](const RtPathTraceCaptureSurfaceProduct* source,
                RtPathTraceCaptureAcceptedSurfaceScalar& destination)
            {
                destination = {};
                destination.present = source != nullptr;
                if (!source) return;
                destination.terminal = static_cast<std::uint32_t>(source->terminal);
                destination.surfaceClass = static_cast<std::uint32_t>(source->surfaceClass);
                destination.sourceFlags = source->sourceFlags;
                destination.vertexCount = source->vertexCount;
                destination.indexCount = source->indexCount;
                destination.rigidReadyByMesh = source->rigidReadyByMesh;
                destination.rigidReadyByResident = source->rigidReadyByResident;
                destination.skinnedCaptureOmitted = source->skinnedCaptureOmitted;
                destination.skinnedAdmission = source->skinnedAdmission;
            };
            copyScalar(findOrdinal(product.surfaces, lowestDifference),
                result.acceptedSetDiff.product);
            copyScalar(findOrdinal(oracle.surfaces, lowestDifference),
                result.acceptedSetDiff.oracle);
        }
    }
    const auto noteGeometry = [&result, &noteFirst](bool mismatch,
        RtPathTraceCaptureFirstFailure failure)
    {
        result.geometryMismatches += mismatch;
        if (mismatch)
        {
            noteFirst(failure);
        }
    };
    const auto noteProposal = [&result, &noteFirst](bool mismatch,
        RtPathTraceCaptureFirstFailure failure)
    {
        result.proposalMismatches += mismatch;
        if (mismatch)
        {
            noteFirst(failure);
        }
    };
    noteGeometry(!CaptureVectorBytesEqual(product.vertices, oracle.vertices),
        RtPathTraceCaptureFirstFailure::Vertices);
    noteGeometry(!CaptureVectorBytesEqual(product.indexes, oracle.indexes),
        RtPathTraceCaptureFirstFailure::Indexes);
    noteGeometry(!CaptureVectorBytesEqual(
            product.triangleClasses, oracle.triangleClasses),
        RtPathTraceCaptureFirstFailure::TriangleClasses);
    noteGeometry(!CaptureVectorBytesEqual(
            product.triangleMaterials, oracle.triangleMaterials),
        RtPathTraceCaptureFirstFailure::TriangleMaterials);
    noteGeometry(!CaptureVectorBytesEqual(
            product.triangleInstances, oracle.triangleInstances),
        RtPathTraceCaptureFirstFailure::TriangleInstances);
    noteGeometry(!CaptureVectorBytesEqual(
            product.triangleIdentities, oracle.triangleIdentities),
        RtPathTraceCaptureFirstFailure::TriangleIdentities);
    // Owner Harvest owns registry/universe mutations.  A geometry-only Lane A
    // product compares their final per-surface consequences below, rather than
    // pretending to replay the owner-only mutation proposal streams.
    if (!product.ownerDecisionGeometryOnly)
    {
    noteProposal(!CaptureSemanticVectorEqual(
        product.materialInfoIntents, oracle.materialInfoIntents,
        [](const auto& lhs, const auto& rhs)
        {
            return lhs.generation == rhs.generation &&
                lhs.surfaceOrdinal == rhs.surfaceOrdinal &&
                lhs.baseMaterialId == rhs.baseMaterialId &&
                lhs.reason == rhs.reason && lhs.callPresent == rhs.callPresent &&
                std::memcmp(lhs.materialName, rhs.materialName,
                    sizeof(lhs.materialName)) == 0;
        }), RtPathTraceCaptureFirstFailure::MaterialInfoIntents);
    noteProposal(!CaptureSemanticVectorEqual(
        product.materialVariants, oracle.materialVariants,
        [](const auto& lhs, const auto& rhs)
        {
            return lhs.surfaceOrdinal == rhs.surfaceOrdinal &&
                lhs.baseMaterialId == rhs.baseMaterialId &&
                lhs.candidateId == rhs.candidateId && lhs.chosenId == rhs.chosenId &&
                lhs.collisionCount == rhs.collisionCount &&
                lhs.fallbackUsed == rhs.fallbackUsed;
        }), RtPathTraceCaptureFirstFailure::MaterialVariants);
    noteProposal(!CaptureSemanticVectorEqual(
        product.instanceObservations, oracle.instanceObservations,
        [](const auto& lhs, const auto& rhs)
        {
            return lhs.surfaceOrdinal == rhs.surfaceOrdinal &&
                lhs.instanceId == rhs.instanceId && lhs.meshHash == rhs.meshHash &&
                lhs.hasPrevious == rhs.hasPrevious &&
                lhs.transformContinuous == rhs.transformContinuous &&
                std::memcmp(lhs.currentObjectToWorld, rhs.currentObjectToWorld,
                    sizeof(lhs.currentObjectToWorld)) == 0 &&
                std::memcmp(lhs.previousObjectToWorld, rhs.previousObjectToWorld,
                    sizeof(lhs.previousObjectToWorld)) == 0;
        }), RtPathTraceCaptureFirstFailure::InstanceObservations);
    {
        std::uint32_t lowest = UINT32_MAX;
        const auto consider = [&](std::uint32_t ordinal)
        {
            const RtPathTraceInstanceObservationProposal* produced = nullptr;
            const RtPathTraceInstanceObservationProposal* applied = nullptr;
            for (const auto& row : product.instanceObservations)
            {
                if (row.surfaceOrdinal == ordinal)
                {
                    produced = &row;
                    break;
                }
            }
            for (const auto& row : oracle.instanceObservations)
            {
                if (row.surfaceOrdinal == ordinal)
                {
                    applied = &row;
                    break;
                }
            }
            const bool mismatch = produced == nullptr || applied == nullptr ||
                produced->instanceId != applied->instanceId ||
                produced->meshHash != applied->meshHash ||
                produced->hasPrevious != applied->hasPrevious ||
                produced->transformContinuous != applied->transformContinuous ||
                std::memcmp(produced->currentObjectToWorld,
                    applied->currentObjectToWorld,
                    sizeof(produced->currentObjectToWorld)) != 0 ||
                std::memcmp(produced->previousObjectToWorld,
                    applied->previousObjectToWorld,
                    sizeof(produced->previousObjectToWorld)) != 0;
            if (mismatch && ordinal < lowest)
            {
                lowest = ordinal;
            }
        };
        for (const auto& row : product.instanceObservations)
        {
            consider(row.surfaceOrdinal);
        }
        for (const auto& row : oracle.instanceObservations)
        {
            consider(row.surfaceOrdinal);
        }
        if (lowest != UINT32_MAX)
        {
            result.instanceObservationDiff.found = true;
            result.instanceObservationDiff.ordinal = lowest;
            for (const auto& row : product.instanceObservations)
            {
                if (row.surfaceOrdinal == lowest)
                {
                    result.instanceObservationDiff.productHasPrevious = row.hasPrevious;
                    result.instanceObservationDiff.productTransformContinuous =
                        row.transformContinuous;
                    break;
                }
            }
            for (const auto& row : oracle.instanceObservations)
            {
                if (row.surfaceOrdinal == lowest)
                {
                    result.instanceObservationDiff.oracleHasPrevious = row.hasPrevious;
                    result.instanceObservationDiff.oracleTransformContinuous =
                        row.transformContinuous;
                    break;
                }
            }
        }
    }
    noteProposal(!CaptureSemanticVectorEqual(
        product.rigidCandidates, oracle.rigidCandidates,
        [](const auto& lhs, const auto& rhs)
        {
            return lhs.surfaceOrdinal == rhs.surfaceOrdinal &&
                lhs.meshHash == rhs.meshHash && lhs.instanceId == rhs.instanceId &&
                lhs.materialId == rhs.materialId;
        }), RtPathTraceCaptureFirstFailure::RigidCandidates);
    }
    noteProposal(!CaptureVectorBytesEqual(
            product.staticMembershipSurfaces, oracle.staticMembershipSurfaces),
        RtPathTraceCaptureFirstFailure::StaticMembership);
    noteProposal(!CaptureVectorBytesEqual(
            product.routedReadySkipSurfaces, oracle.routedReadySkipSurfaces),
        RtPathTraceCaptureFirstFailure::RoutedReadySkip);
    if (product.surfaces.size() != oracle.surfaces.size())
    {
        ++result.proposalMismatches;
        noteFirst(RtPathTraceCaptureFirstFailure::SurfaceCount);
    }
    else
    {
        for (std::size_t index = 0; index < product.surfaces.size(); ++index)
        {
            const bool decisionsEqual = product.ownerDecisionGeometryOnly
                ? PathTraceCaptureSurfaceAuthorityEqual(
                    product.surfaces[index], oracle.surfaces[index])
                : PathTraceCaptureSurfaceDecisionsEqual(
                    product.surfaces[index], oracle.surfaces[index]);
            if (!decisionsEqual)
            {
                ++result.proposalMismatches;
                noteFirst(RtPathTraceCaptureFirstFailure::SurfaceDecision,
                    product.surfaces[index].ordinal);
                break;
            }
        }
    }
    result.exact = result.geometryMismatches == 0 &&
        result.proposalMismatches == 0;
    return result;
}
