#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCommittedProposalProduct.h"
#include "PathTraceRigidIdentity.h"

#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace {

template<typename T>
std::size_t VectorOwnedBytes(const std::vector<T>& values) noexcept
{
    return values.capacity() * sizeof(T);
}

bool IncrementBounded(std::size_t& value, std::size_t bound) noexcept
{
    if (value >= bound)
    {
        return false;
    }
    ++value;
    return true;
}

bool DerivedDecisionStateValid(const RtPathTraceCaptureRawSurface& raw) noexcept
{
    if (!raw.derivedRuntimeMaterialPresent)
    {
        return RtPathTraceRuntimeMaterialDecisionPodIsDefault(
                raw.derivedRuntimeMaterial) &&
            raw.derivedBaseMaterialId == 0 &&
            raw.derivedChosenMaterialId == 0 &&
            !raw.rigidIdentityPresent;
    }
    if (raw.derivedChosenMaterialId !=
            raw.derivedRuntimeMaterial.chosenMaterialId)
    {
        return false;
    }
    if (!raw.rigidIdentityPresent)
    {
        return raw.derivedMeshHash == 0 &&
            raw.derivedMaterialClassSignature == 0 &&
            raw.derivedResolvedModelSurfaceIndex == -1;
    }
    return true;
}

std::uint32_t DerivedRigidSourceFlags(
    const RtPathTraceCaptureRawSurface& raw,
    const RtPathTraceCommittedPlanningBaseline& baseline) noexcept
{
    RtPathTraceSourceFlagInput input;
    input.surfaceClass = RtSmokeSurfaceClass::RigidEntity;
    input.guiSurface = raw.classify.material.guiSurface;
    input.hasJointCache = raw.classify.hasJointCache;
    input.hasStaticModelWithJoints = raw.classify.hasStaticModelWithJoints;
    input.hasRenderEntityJoints = raw.classify.hasRenderEntityJoints;
    input.entityCallbackPresent = raw.entityCallbackPresent;
    input.entityForceUpdate = raw.entityForceUpdate;
    input.dynamicModelPresent = raw.dynamicModelPresent;
    input.cachedDynamicModelPresent = raw.cachedDynamicModelPresent;
    input.materialDeformed = raw.classify.material.deform != DFRM_NONE;
    input.customShaderPresent = raw.customShaderPresent;
    input.customSkinPresent = raw.customSkinPresent;
    std::uint32_t flags = RtPathTraceSourceFlagsFromPod(input);
    if (HasStaticSurfaceFromCommittedBaselinePod(baseline.geometryUniverse,
            baseline.epoch,
            RtPathTraceLegacyStaticSurfaceKeyFromRaw(raw)))
    {
        flags |= RT_PT_INSTANCE_SOURCE_STATIC_CACHE_MATCH;
    }
    return flags;
}

bool RowNeedsRecordAllIdentity(
    const RtPathTraceCaptureRawSurface& raw,
    const RtPathTraceCaptureOwnerSnapshot& snapshot) noexcept
{
    return snapshot.recordAllInstanceClasses &&
        raw.safety == RtPathTraceCaptureSafetyDisposition::Ready &&
        !raw.rigidIdentityPresent;
}

bool CountProposals(
    const RtPathTracePrimarySemanticScratch& scratch,
    const RtPathTraceCommittedPlanningBaseline& baseline,
    RtPathTraceCaptureCapacityCounts& counts,
    RtPathTraceCommittedProposalBuildReject& rejection) noexcept
{
    counts = scratch.snapshot.capacityCounts;
    counts.materialInfoIntents = 0;
    counts.materialVariantProposals = 0;
    counts.instanceObservationProposals = 0;
    counts.rigidCandidateProposals = 0;
    const std::size_t surfaceCount = scratch.snapshot.surfaces.size();
    if (surfaceCount > UINT32_MAX)
    {
        rejection = RtPathTraceCommittedProposalBuildReject::CapacityInvalid;
        return false;
    }
    counts.surfaces = surfaceCount;
    counts.receiptSurfaces = surfaceCount;
    for (std::size_t index = 0; index < surfaceCount; ++index)
    {
        const RtPathTraceCaptureRawSurface& raw =
            scratch.snapshot.surfaces[index];
        if (raw.ordinal != index || !raw.semanticFactsDerived ||
            !raw.semanticFactsPresent)
        {
            rejection = RtPathTraceCommittedProposalBuildReject::
                SurfaceShapeInvalid;
            return false;
        }
        if (!DerivedDecisionStateValid(raw))
        {
            rejection = RtPathTraceCommittedProposalBuildReject::
                PersistedDecisionInvalid;
            return false;
        }
        if (RtPathTraceDerivedRuntimeMaterialEmitsVariant(raw))
        {
            if (!IncrementBounded(counts.materialInfoIntents, surfaceCount) ||
                !IncrementBounded(counts.materialVariantProposals, surfaceCount))
            {
                rejection = RtPathTraceCommittedProposalBuildReject::CapacityInvalid;
                return false;
            }
        }
        if (RowNeedsRecordAllIdentity(raw, scratch.snapshot))
        {
            rejection = RtPathTraceCommittedProposalBuildReject::
                RecordAllIdentityUnavailable;
            return false;
        }
        if (!raw.rigidIdentityPresent)
        {
            continue;
        }
        const std::uint32_t sourceFlags =
            DerivedRigidSourceFlags(raw, baseline);
        const bool durable = RtPathTraceSourceFlagsAreDurableRigid(sourceFlags);
        if (scratch.snapshot.recordAllInstanceClasses && !durable)
        {
            // P2c-1b persists the exact rigid identity only for the durable
            // domain.  Fail closed rather than reconstruct a record-all hash.
            rejection = RtPathTraceCommittedProposalBuildReject::
                RecordAllIdentityUnavailable;
            return false;
        }
        if (!durable)
        {
            continue;
        }
        if (!IncrementBounded(
                counts.instanceObservationProposals, surfaceCount))
        {
            rejection = RtPathTraceCommittedProposalBuildReject::CapacityInvalid;
            return false;
        }
        if (raw.derivedMeshHash != 0 && raw.derivedChosenMaterialId != 0 &&
            raw.vertexCount != 0 && raw.sourceTriIndexCount != 0 &&
            (raw.sourceTriIndexCount % 3u) == 0u)
        {
            if (!IncrementBounded(counts.rigidCandidateProposals, surfaceCount))
            {
                rejection = RtPathTraceCommittedProposalBuildReject::CapacityInvalid;
                return false;
            }
        }
    }
    return true;
}

template<typename T>
bool ReserveProposal(std::vector<T>& values, std::size_t count,
    RtPathTraceCommittedProposalBuildTestSeam* seam)
{
    std::size_t reserveCount = count;
    if (seam != nullptr)
    {
        const std::size_t call = seam->reserveCalls++;
        if (call == seam->failAtReserve)
        {
            if (seam->failure ==
                RtPathTraceCommittedProposalAllocationFailure::LengthError)
            {
                throw std::length_error("P2c-2 reserve seam");
            }
            throw std::bad_alloc();
        }
        if (call == seam->inflateAtReserve)
        {
            if (seam->inflateBy >
                std::numeric_limits<std::size_t>::max() - reserveCount)
            {
                return false;
            }
            reserveCount += seam->inflateBy;
        }
    }
    values.reserve(reserveCount);
    return values.capacity() >= count;
}

bool ReconcileProposalCapacityWithPlan(
    RtPathTraceCommittedProposalProduct& candidate,
    const RtPathTraceCommittedProposalCapacityPlan& plan) noexcept
{
    candidate.actualOwnedBytes = candidate.OwnedBytes();
    return candidate.inputOwnedBytes == plan.inputOwnedBytes &&
        candidate.actualOwnedBytes <= plan.proposalBudgetBytes &&
        candidate.actualOwnedBytes <= RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES &&
        candidate.inputOwnedBytes <= RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES -
            candidate.actualOwnedBytes;
}

template<typename T>
bool ReserveProposalChecked(
    RtPathTraceCommittedProposalProduct& candidate,
    std::vector<T>& values,
    std::size_t count,
    const RtPathTraceCommittedProposalCapacityPlan& plan,
    RtPathTraceCommittedProposalBuildTestSeam* seam)
{
    return ReserveProposal(values, count, seam) &&
        ReconcileProposalCapacityWithPlan(candidate, plan);
}

} // namespace

std::size_t RtPathTraceCommittedProposalProduct::OwnedBytes() const noexcept
{
    return sizeof(*this) + VectorOwnedBytes(materialInfoIntents) +
        VectorOwnedBytes(materialVariants) +
        VectorOwnedBytes(instanceObservations) +
        VectorOwnedBytes(rigidCandidates) +
        VectorOwnedBytes(receiptSurfaces);
}

bool PlanPathTraceCommittedProposalCapacity(
    std::size_t inputOwnedBytes,
    const RtPathTraceCaptureCapacityCounts& counts,
    RtPathTraceCommittedProposalCapacityPlan& plan) noexcept
{
    plan = {};
    if (counts.materialInfoIntents != counts.materialVariantProposals ||
        counts.materialInfoIntents > counts.surfaces ||
        counts.instanceObservationProposals > counts.surfaces ||
        counts.rigidCandidateProposals > counts.surfaces ||
        counts.receiptSurfaces != counts.surfaces ||
        inputOwnedBytes > RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES)
    {
        return false;
    }
    std::size_t proposalBytes = sizeof(RtPathTraceCommittedProposalProduct);
    const auto add = [&proposalBytes](std::size_t count, std::size_t element)
    {
        return RtPathTraceProducerCheckedArrayBytes(count, element,
            proposalBytes, RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES);
    };
    if (!add(counts.materialInfoIntents,
            sizeof(RtPathTraceMaterialInfoRegistrationIntent)) ||
        !add(counts.materialVariantProposals,
            sizeof(RtPathTraceMaterialVariantRegistrationProposal)) ||
        !add(counts.instanceObservationProposals,
            sizeof(RtPathTraceInstanceObservationProposal)) ||
        !add(counts.rigidCandidateProposals,
            sizeof(RtPathTraceRigidCandidateProposal)) ||
        !add(counts.receiptSurfaces,
            sizeof(RtPathTraceCommittedReferencedSurfaceReceiptRow)))
    {
        return false;
    }
    std::size_t peak = inputOwnedBytes;
    if (!RtPathTraceProducerCheckedAddBytes(peak, proposalBytes,
            RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES))
    {
        return false;
    }
    plan.counts = counts;
    plan.inputOwnedBytes = inputOwnedBytes;
    plan.proposalOwnedBytes = proposalBytes;
    plan.proposalBudgetBytes = RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES -
        inputOwnedBytes;
    plan.peakOwnedBytes = peak;
    return true;
}

namespace
{
bool RtPathTraceCommittedProposalProductPayloadShapeValid(
    const RtPathTraceCommittedProposalProduct& product) noexcept
{
    return product.actualOwnedBytes == product.OwnedBytes() &&
        RtPathTracePureSemanticProposalPayloadShapeValid(
            product.capacityCounts, product.planningEpoch,
            product.receiptHeader, product.receiptSurfaces.data(),
            product.receiptSurfaces.size(), product.materialInfoIntents.data(),
            product.materialInfoIntents.size(), product.materialVariants.data(),
            product.materialVariants.size(), product.instanceObservations.data(),
            product.instanceObservations.size(), product.rigidCandidates.data(),
            product.rigidCandidates.size());
}
}

bool RtPathTraceCommittedProposalProductShapeValid(
    const RtPathTraceCommittedProposalProduct& product) noexcept
{
    return product.provenance ==
            RtPathTraceCommittedProposalProvenance::PureSemantic &&
        product.complete && product.receiptHeader.complete &&
        RtPathTraceCommittedProposalProductPayloadShapeValid(product);
}

bool CountPathTraceCommittedProposalPayload(
    const RtPathTracePrimarySemanticScratch& scratch,
    const RtPathTraceCommittedPlanningBaseline& baseline,
    RtPathTraceCaptureCapacityCounts& counts,
    RtPathTraceCommittedProposalBuildReject& rejection) noexcept
{
    return CountProposals(scratch, baseline, counts, rejection);
}

bool EmitPathTraceCommittedProposalPayload(
    const RtPathTracePrimarySemanticScratch& scratch,
    const RtPathTraceCommittedPlanningBaseline& baseline,
    const RtPathTraceCommittedProposalPayloadTarget& target) noexcept
{
    if (target.planningEpoch == nullptr || target.capacityCounts == nullptr ||
        target.receiptHeader == nullptr || target.receiptSurfaces == nullptr ||
        target.materialInfoIntents == nullptr ||
        target.materialVariants == nullptr ||
        target.instanceObservations == nullptr ||
        target.rigidCandidates == nullptr)
    {
        return false;
    }
    RtPathTracePlanningSnapshotEpoch& epoch = *target.planningEpoch;
    RtPathTraceCaptureCapacityCounts& counts = *target.capacityCounts;
    auto& receipt = *target.receiptHeader;
    auto& receiptRows = *target.receiptSurfaces;
    auto& intents = *target.materialInfoIntents;
    auto& variants = *target.materialVariants;
    auto& observations = *target.instanceObservations;
    auto& rigids = *target.rigidCandidates;
    if (!receiptRows.empty() || !intents.empty() || !variants.empty() ||
        !observations.empty() || !rigids.empty() ||
        receiptRows.capacity() < counts.receiptSurfaces ||
        intents.capacity() < counts.materialInfoIntents ||
        variants.capacity() < counts.materialVariantProposals ||
        observations.capacity() < counts.instanceObservationProposals ||
        rigids.capacity() < counts.rigidCandidateProposals)
    {
        return false;
    }
    epoch = RtPathTraceMapCommittedBaselineToPlanningEpoch(baseline.epoch);
    receipt.sealedPrimaryViewToken = scratch.sealedPrimaryViewToken;
    receipt.ownerUniverseFrameIndex = baseline.epoch.ownerUniverseFrameIndex;
    receipt.baselineFrameIndex = baseline.epoch.baselineFrameIndex;
    receipt.worldLifecycleGeneration = baseline.epoch.worldLifecycleGeneration;
    receipt.mapLoadSerial = baseline.epoch.mapLoadSerial;
    receipt.mapTimeStamp = baseline.epoch.mapTimeStamp;
    std::memcpy(receipt.mapName, baseline.epoch.mapName,
        sizeof(receipt.mapName));
    receipt.barrierGeneration = baseline.epoch.barrierGeneration;
    receipt.materialRegistryGeneration = scratch.snapshot.registryGeneration;
    receipt.residentMaterialFactsGeneration =
        scratch.snapshot.residentMaterialFactsGeneration;
    receipt.configFingerprint = baseline.semanticConfig.configFingerprint;
    receipt.instanceUniverseGeneration =
        baseline.epoch.instanceUniverseGeneration;
    receipt.geometryUniverseGeneration =
        baseline.epoch.geometryUniverseGeneration;
    receipt.staticMaterialGeneration = baseline.epoch.staticMaterialGeneration;
    receipt.canonicalSourceIndexPoolGeneration =
        baseline.epoch.canonicalSourceIndexPoolGeneration;
    receipt.staticResidentPayloadGeneration =
        baseline.epoch.staticResidentPayloadGeneration;
    receipt.surfaceCount = static_cast<std::uint32_t>(counts.receiptSurfaces);
    receipt.recordAllInstanceClasses =
        scratch.snapshot.recordAllInstanceClasses;

    for (const RtPathTraceCaptureRawSurface& raw : scratch.snapshot.surfaces)
    {
        RtPathTraceCommittedReferencedSurfaceReceiptRow receiptRow{};
        std::memset(&receiptRow, 0, sizeof(receiptRow));
        receiptRow.persistedMeshHash = raw.derivedMeshHash;
        receiptRow.persistedChosenMaterialId = raw.derivedChosenMaterialId;
        receiptRow.surfaceOrdinal = raw.ordinal;
        receiptRow.entityIndex = raw.entityIndex;
        receiptRow.renderEntityNum = raw.entityNum;
        receiptRow.expectedReadyByMesh = raw.rigidReadyByMesh;
        receiptRow.expectedReadyByResident = raw.rigidReadyByResident;
        receiptRow.rigidIdentityPresent = raw.rigidIdentityPresent;
        receiptRow.derivedRuntimeMaterialPresent =
            raw.derivedRuntimeMaterialPresent;
        receiptRows.push_back(receiptRow);
        if (RtPathTraceDerivedRuntimeMaterialEmitsVariant(raw))
        {
            RtPathTraceMaterialInfoRegistrationIntent intent{};
            std::memset(&intent, 0, sizeof(intent));
            intent.generation = epoch.generation;
            intent.surfaceOrdinal = raw.ordinal;
            intent.baseMaterialId = raw.derivedBaseMaterialId;
            RtPathTracePlanningCopyName(intent.materialName,
                sizeof(intent.materialName), raw.materialName);
            intent.reason = 1;
            intent.callPresent = true;
            intents.push_back(intent);
            RtPathTraceMaterialVariantRegistrationProposal variant{};
            std::memset(&variant, 0, sizeof(variant));
            variant.surfaceOrdinal = raw.ordinal;
            variant.baseMaterialId = raw.derivedBaseMaterialId;
            variant.candidateId = raw.derivedRuntimeMaterial.initialCandidateId;
            variant.chosenId = raw.derivedRuntimeMaterial.chosenMaterialId;
            variant.collisionCount = raw.derivedRuntimeMaterial.collisionCount;
            variant.fallbackUsed = raw.derivedRuntimeMaterial.fallbackUsed;
            variants.push_back(variant);
        }
        if (!raw.rigidIdentityPresent)
        {
            continue;
        }
        const std::uint32_t sourceFlags = DerivedRigidSourceFlags(raw, baseline);
        if (!RtPathTraceSourceFlagsAreDurableRigid(sourceFlags))
        {
            continue;
        }
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
        const std::uint64_t instanceId =
            BuildPathTraceRigidInstanceIdFromPod(identity);
        const RtPathTraceInstanceHistoryApplication history =
            ApplyInstanceHistoryFromPod(baseline.instanceUniverse,
                baseline.epoch, instanceId, raw.objectToWorld,
                baseline.epoch.ownerUniverseFrameIndex);
        RtPathTraceInstanceObservationProposal observation{};
        std::memset(&observation, 0, sizeof(observation));
        observation.surfaceOrdinal = raw.ordinal;
        observation.instanceId = instanceId;
        observation.meshHash = raw.derivedMeshHash;
        observation.hasPrevious = history.hasPreviousObjectToWorld;
        observation.transformContinuous = history.transformContinuous;
        std::memcpy(observation.currentObjectToWorld,
            history.currentObjectToWorld, sizeof(observation.currentObjectToWorld));
        std::memcpy(observation.previousObjectToWorld,
            history.previousObjectToWorld,
            sizeof(observation.previousObjectToWorld));
        observations.push_back(observation);
        if (raw.derivedMeshHash != 0 && raw.derivedChosenMaterialId != 0 &&
            raw.vertexCount != 0 && raw.sourceTriIndexCount != 0 &&
            (raw.sourceTriIndexCount % 3u) == 0u)
        {
            RtPathTraceRigidCandidateProposal rigid{};
            std::memset(&rigid, 0, sizeof(rigid));
            rigid.surfaceOrdinal = raw.ordinal;
            rigid.meshHash = raw.derivedMeshHash;
            rigid.instanceId = instanceId;
            rigid.materialId = raw.derivedChosenMaterialId;
            rigids.push_back(rigid);
        }
    }
    return receiptRows.size() == counts.receiptSurfaces &&
        intents.size() == counts.materialInfoIntents &&
        variants.size() == counts.materialVariantProposals &&
        observations.size() == counts.instanceObservationProposals &&
        rigids.size() == counts.rigidCandidateProposals;
}

RtPathTraceCommittedProposalBuildResult
BuildPathTraceCommittedProposalProductP2c2(
    const RtPathTracePrimarySemanticScratch& scratch,
    const RtPathTraceCommittedPlanningBaseline& baseline,
    RtPathTraceCommittedProposalProduct& output,
    RtPathTraceCommittedProposalBuildTestSeam* testSeam) noexcept
{
    RtPathTraceCommittedProposalBuildResult result;
    const auto reject = [&result](RtPathTraceCommittedProposalBuildReject reason)
    {
        result.rejection = reason;
        return result;
    };
    if (!scratch.complete || !scratch.snapshot.complete || !baseline.Valid())
    {
        return reject(RtPathTraceCommittedProposalBuildReject::InputInvalid);
    }
    if (scratch.sealedPrimaryViewToken == 0 ||
        baseline.epoch.sealedPrimaryViewToken == 0 ||
        scratch.sealedPrimaryViewToken <= baseline.epoch.sealedPrimaryViewToken ||
        scratch.sealedPrimaryViewToken - 1 !=
            baseline.epoch.sealedPrimaryViewToken)
    {
        return reject(RtPathTraceCommittedProposalBuildReject::TokenMismatch);
    }
    if (scratch.snapshot.registryGeneration == 0 ||
        scratch.snapshot.residentMaterialFactsGeneration == 0 ||
        baseline.epoch.worldLifecycleGeneration == 0 ||
        baseline.epoch.barrierGeneration == 0 ||
        baseline.semanticConfig.configFingerprint == 0 ||
        baseline.epoch.instanceUniverseGeneration == 0 ||
        baseline.epoch.geometryUniverseGeneration == 0 ||
        baseline.epoch.staticMaterialGeneration == 0 ||
        baseline.epoch.canonicalSourceIndexPoolGeneration == 0 ||
        baseline.epoch.staticResidentPayloadGeneration == 0 ||
        scratch.snapshot.recordAllInstanceClasses !=
            baseline.semanticConfig.recordAllInstanceClasses)
    {
        return reject(RtPathTraceCommittedProposalBuildReject::InputInvalid);
    }
    RtPathTraceCommittedProposalBuildReject countReject =
        RtPathTraceCommittedProposalBuildReject::None;
    RtPathTraceCaptureCapacityCounts counts;
    if (!CountPathTraceCommittedProposalPayload(
            scratch, baseline, counts, countReject))
    {
        return reject(countReject);
    }
    result.counts = counts;
    std::size_t inputOwnedBytes = scratch.ownedBytes;
    if (!RtPathTraceProducerCheckedAddBytes(inputOwnedBytes,
            RtPathTraceCommittedBaselineCarrierBytes(baseline),
            RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES))
    {
        return reject(RtPathTraceCommittedProposalBuildReject::CapacityInvalid);
    }
    RtPathTraceCommittedProposalCapacityPlan plan;
    if (!PlanPathTraceCommittedProposalCapacity(
            inputOwnedBytes, counts, plan))
    {
        return reject(RtPathTraceCommittedProposalBuildReject::CapacityInvalid);
    }

#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try
    {
#endif
        RtPathTraceCommittedProposalProduct candidate;
        candidate.planningEpoch =
            RtPathTraceMapCommittedBaselineToPlanningEpoch(baseline.epoch);
        if (candidate.planningEpoch.frameIndex !=
                baseline.epoch.ownerUniverseFrameIndex)
        {
            return reject(RtPathTraceCommittedProposalBuildReject::InputInvalid);
        }
        candidate.capacityCounts = counts;
        candidate.inputOwnedBytes = inputOwnedBytes;
        if (!ReserveProposalChecked(candidate, candidate.receiptSurfaces,
                counts.receiptSurfaces, plan, testSeam) ||
            !ReserveProposalChecked(candidate, candidate.materialInfoIntents,
                counts.materialInfoIntents, plan, testSeam) ||
            !ReserveProposalChecked(candidate, candidate.materialVariants,
                counts.materialVariantProposals, plan, testSeam) ||
            !ReserveProposalChecked(candidate, candidate.instanceObservations,
                counts.instanceObservationProposals, plan, testSeam) ||
            !ReserveProposalChecked(candidate, candidate.rigidCandidates,
                counts.rigidCandidateProposals, plan, testSeam))
        {
            return reject(RtPathTraceCommittedProposalBuildReject::CapacityInvalid);
        }
        // Keep a visible chain-level guard in addition to each peak check: later
        // reserve sequencing edits must not accidentally remove the aggregate bound.
        if (!ReconcileProposalCapacityWithPlan(candidate, plan))
        {
            return reject(RtPathTraceCommittedProposalBuildReject::CapacityInvalid);
        }

        RtPathTraceCommittedProposalPayloadTarget payloadTarget;
        payloadTarget.planningEpoch = &candidate.planningEpoch;
        payloadTarget.capacityCounts = &candidate.capacityCounts;
        payloadTarget.receiptHeader = &candidate.receiptHeader;
        payloadTarget.receiptSurfaces = &candidate.receiptSurfaces;
        payloadTarget.materialInfoIntents = &candidate.materialInfoIntents;
        payloadTarget.materialVariants = &candidate.materialVariants;
        payloadTarget.instanceObservations = &candidate.instanceObservations;
        payloadTarget.rigidCandidates = &candidate.rigidCandidates;
        if (!EmitPathTraceCommittedProposalPayload(
                scratch, baseline, payloadTarget))
        {
            return reject(RtPathTraceCommittedProposalBuildReject::
                OutputShapeInvalid);
        }
        if (!ReconcileProposalCapacityWithPlan(candidate, plan))
        {
            return reject(RtPathTraceCommittedProposalBuildReject::CapacityInvalid);
        }
        if (!RtPathTraceCommittedProposalProductPayloadShapeValid(candidate))
        {
            return reject(
                RtPathTraceCommittedProposalBuildReject::OutputShapeInvalid);
        }
        candidate.receiptHeader.complete = true;
        candidate.provenance =
            RtPathTraceCommittedProposalProvenance::PureSemantic;
        candidate.complete = true;
        candidate.actualOwnedBytes = candidate.OwnedBytes();
        if (!RtPathTraceCommittedProposalProductShapeValid(candidate))
        {
            return reject(
                RtPathTraceCommittedProposalBuildReject::OutputShapeInvalid);
        }
        result.actualOwnedBytes = candidate.actualOwnedBytes;
        output = std::move(candidate);
        result.accepted = true;
        result.rejection = RtPathTraceCommittedProposalBuildReject::None;
        return result;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    }
    catch (const std::bad_alloc&)
    {
        return reject(RtPathTraceCommittedProposalBuildReject::BadAlloc);
    }
    catch (const std::length_error&)
    {
        return reject(RtPathTraceCommittedProposalBuildReject::LengthError);
    }
#endif
}
