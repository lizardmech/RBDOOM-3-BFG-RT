#include "precompiled.h"
#pragma hdrstop

#include "PathTracePureSemanticWorkerPipeline.h"
#include "PathTraceOwnerSemanticKernel.h"

#include <cstring>
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace
{
bool AddProductArray(std::size_t count, std::size_t element,
    std::size_t& bytes) noexcept
{
    return RtPathTraceProducerCheckedArrayBytes(count, element, bytes,
        RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES);
}

bool PlanPureSemanticProductBytes(
    const RtPathTraceCaptureCapacityCounts& counts,
    std::size_t& productBytes) noexcept
{
    const std::size_t triangles = counts.indexes / 3u;
    productBytes = sizeof(RtPathTraceCaptureProduct);
    return AddProductArray(counts.surfaces,
            sizeof(RtPathTraceCaptureSurfaceProduct), productBytes) &&
        AddProductArray(counts.vertices, sizeof(PathTraceSmokeVertex),
            productBytes) &&
        AddProductArray(counts.indexes, sizeof(std::uint32_t), productBytes) &&
        AddProductArray(triangles, sizeof(std::uint32_t), productBytes) &&
        AddProductArray(triangles, sizeof(std::uint32_t), productBytes) &&
        AddProductArray(triangles, sizeof(std::uint64_t), productBytes) &&
        AddProductArray(triangles, sizeof(std::uint32_t), productBytes) &&
        AddProductArray(counts.materialInfoIntents,
            sizeof(RtPathTraceMaterialInfoRegistrationIntent), productBytes) &&
        AddProductArray(counts.materialVariantProposals,
            sizeof(RtPathTraceMaterialVariantRegistrationProposal),
            productBytes) &&
        AddProductArray(counts.instanceObservationProposals,
            sizeof(RtPathTraceInstanceObservationProposal), productBytes) &&
        AddProductArray(counts.rigidCandidateProposals,
            sizeof(RtPathTraceRigidCandidateProposal), productBytes) &&
        AddProductArray(counts.preparedRigidPayloads,
            sizeof(RtPathTraceRigidPreparedPayload), productBytes) &&
        AddProductArray(counts.preparedRigidVertices,
            sizeof(PathTraceSmokeVertex), productBytes) &&
        AddProductArray(counts.preparedRigidIndexes,
            sizeof(std::uint32_t), productBytes) &&
        AddProductArray(counts.frameMaterialIds, sizeof(std::uint32_t),
            productBytes) &&
        AddProductArray(counts.receiptSurfaces,
            sizeof(RtPathTraceCommittedReferencedSurfaceReceiptRow),
            productBytes) &&
        AddProductArray(counts.surfaces, sizeof(std::uint32_t), productBytes) &&
        AddProductArray(counts.surfaces, sizeof(std::uint32_t), productBytes);
}

} // namespace

std::size_t RtPathTraceCountPureSemanticFrameMaterialIds(
    const RtPathTraceCaptureOwnerSnapshot& snapshot) noexcept
{
    std::size_t uniqueCount = 0;
    const std::size_t flattenedCount = snapshot.surfaces.size() * 2u;
    for (std::size_t position = 0; position < flattenedCount; ++position)
    {
        const RtPathTraceCaptureRawSurface& row =
            snapshot.surfaces[position / 2u];
        const std::uint32_t id = (position & 1u) == 0u
            ? row.derivedBaseMaterialId
            : row.derivedChosenMaterialId;
        bool seen = false;
        for (std::size_t priorPosition = 0;
             priorPosition < position && !seen; ++priorPosition)
        {
            const RtPathTraceCaptureRawSurface& prior =
                snapshot.surfaces[priorPosition / 2u];
            const std::uint32_t priorId = (priorPosition & 1u) == 0u
                ? prior.derivedBaseMaterialId
                : prior.derivedChosenMaterialId;
            seen = priorId == id;
        }
        uniqueCount += seen ? 0u : 1u;
    }
    return uniqueCount;
}

bool RtPathTraceEmitPureSemanticFrameMaterialIds(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    RtPathTraceCaptureProduct& product)
{
    for (const RtPathTraceCaptureRawSurface& row : snapshot.surfaces)
    {
        const std::uint32_t ids[2] = {
            row.derivedBaseMaterialId,
            row.derivedChosenMaterialId
        };
        for (std::uint32_t id : ids)
        {
            if (std::find(product.frameMaterialIds.begin(),
                    product.frameMaterialIds.end(), id) ==
                product.frameMaterialIds.end())
            {
                if (product.frameMaterialIds.size() >=
                    product.frameMaterialIds.capacity())
                {
                    return false;
                }
                product.frameMaterialIds.push_back(id);
            }
        }
    }
    std::sort(product.frameMaterialIds.begin(),
        product.frameMaterialIds.end());
    return true;
}

namespace
{

void ReconstructLateConsumeToken(
    const RtPathTraceCommittedPlanningBaseline& baseline,
    RtPathTraceLateConsumeToken& token) noexcept
{
    token = {};
    token.mapTimeStamp = baseline.epoch.mapTimeStamp;
    token.mapLoadSerial = baseline.epoch.mapLoadSerial;
    std::memcpy(token.mapName, baseline.epoch.mapName, sizeof(token.mapName));
    token.registryGeneration = baseline.epoch.materialRegistryGeneration;
    token.residentMaterialFactsGeneration =
        baseline.epoch.residentMaterialFactsGeneration;
    token.instanceUniverseGeneration =
        baseline.epoch.instanceUniverseGeneration;
    token.geometryUniverseGeneration =
        baseline.epoch.geometryUniverseGeneration;
    token.configFingerprint = baseline.semanticConfig.configFingerprint;
    token.capturedAfterBeginFrame = true;
    token.capturedAfterStaticPreload = true;
}
}

RtPathTracePureSemanticWorkerResult BuildPathTracePureSemanticWorkerProductS3(
    RtPathTracePrimarySemanticScratch&& sourceScratch,
    const RtPathTraceCommittedPlanningBaseline& baseline,
    RtPathTraceCaptureProduct& output,
    const RtPathTracePureSemanticWorkerTestSeam* testSeam) noexcept
{
    static_assert(std::is_nothrow_move_constructible<
        RtPathTracePrimarySemanticScratch>::value,
        "S3 must acquire scratch ownership without allocation or failure");
    static_assert(std::is_nothrow_move_assignable<
        RtPathTracePrimarySemanticScratch>::value,
        "S3 must clear transferred source ownership without allocation");
    RtPathTracePrimarySemanticScratch scratch(std::move(sourceScratch));
    sourceScratch = RtPathTracePrimarySemanticScratch{};

    RtPathTracePureSemanticWorkerResult result;
    result.inputConsumed = true;
    const auto reject = [&result](RtPathTracePureSemanticWorkerReject reason)
    {
        result.rejection = reason;
        return result;
    };
    if (!scratch.complete || !scratch.snapshot.complete || !baseline.Valid())
    {
        return reject(RtPathTracePureSemanticWorkerReject::InputInvalid);
    }
    if (scratch.sealedPrimaryViewToken == 0 ||
        baseline.epoch.sealedPrimaryViewToken == 0 ||
        scratch.sealedPrimaryViewToken <= baseline.epoch.sealedPrimaryViewToken ||
        scratch.sealedPrimaryViewToken - 1 !=
            baseline.epoch.sealedPrimaryViewToken)
    {
        return reject(RtPathTracePureSemanticWorkerReject::TokenMismatch);
    }
    const RtPathTraceCommittedSemanticConfig& config = baseline.semanticConfig;
    if (!RtPathTraceCommittedSemanticConfigMatches(config,
            scratch.snapshot.recordAllInstanceClasses,
            scratch.snapshot.removeRoutedRigidDynamic,
            scratch.snapshot.rigidRouteEmissiveCards,
            scratch.snapshot.admissionMaxSurfaces,
            scratch.snapshot.admissionMaxBytes,
            scratch.snapshot.lateConsumeToken.configFingerprint))
    {
        return reject(RtPathTracePureSemanticWorkerReject::ConfigMismatch);
    }
    if (!RtPathTraceCommittedBaselineMayDispatch(baseline.dispatchGuard))
    {
        return reject(RtPathTracePureSemanticWorkerReject::DispatchGuard);
    }
    if (scratch.snapshot.registryGeneration == 0 ||
        scratch.snapshot.residentMaterialFactsGeneration == 0 ||
        scratch.snapshot.registryGeneration !=
            baseline.epoch.materialRegistryGeneration ||
        scratch.snapshot.residentMaterialFactsGeneration !=
            baseline.epoch.residentMaterialFactsGeneration)
    {
        return reject(RtPathTracePureSemanticWorkerReject::GenerationMismatch);
    }

#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try
    {
#endif
        const std::size_t scratchBefore = scratch.snapshot.OwnedBytes();
        if (testSeam != nullptr && testSeam->failOwnerDecisionReserve)
        {
            throw std::bad_alloc();
        }
        scratch.snapshot.ownerDecisions.reserve(
            scratch.snapshot.surfaces.size());
        const std::size_t scratchAfter = scratch.snapshot.OwnedBytes();
        if (scratchAfter < scratchBefore ||
            scratchAfter - scratchBefore >
                RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES - scratch.ownedBytes)
        {
            return reject(
                RtPathTracePureSemanticWorkerReject::OwnerDecisionCapacity);
        }
        scratch.ownedBytes += scratchAfter - scratchBefore;

        RtPathTraceCommittedSemanticFactsProvider facts(baseline);
        ++result.finalizerCalls;
        if (!FinalizePathTraceOwnerSemanticSnapshot(
                scratch.snapshot, facts, nullptr, 0, false))
        {
            return reject(RtPathTracePureSemanticWorkerReject::FinalizerFailure);
        }
        result.canonicalQueryViolations =
            facts.CanonicalQueryViolationCount();
        if (result.canonicalQueryViolations != 0)
        {
            return reject(
                RtPathTracePureSemanticWorkerReject::CanonicalQueryViolation);
        }
        for (const RtPathTraceCaptureRawSurface& raw :
            scratch.snapshot.surfaces)
        {
            if (!raw.semanticFactsDerived || !raw.semanticFactsPresent)
            {
                return reject(
                    RtPathTracePureSemanticWorkerReject::DerivedFactsIncomplete);
            }
        }
        scratch.snapshot.epoch =
            RtPathTraceMapCommittedBaselineToPlanningEpoch(baseline.epoch);
        scratch.snapshot.viewIdentity = 0;
        ReconstructLateConsumeToken(baseline,
            scratch.snapshot.lateConsumeToken);
        if (!BuildPathTraceOwnerDecisionsFromFinalizedSemantic(
                scratch.snapshot, baseline))
        {
            return reject(RtPathTracePureSemanticWorkerReject::DecisionFailure);
        }

        RtPathTraceCommittedProposalBuildReject proposalReject =
            RtPathTraceCommittedProposalBuildReject::None;
        RtPathTraceCaptureCapacityCounts proposalCounts;
        if (!CountPathTraceCommittedProposalPayload(
                scratch, baseline, proposalCounts, proposalReject))
        {
            return reject(
                RtPathTracePureSemanticWorkerReject::ProposalCountFailure);
        }
        RtPathTraceCaptureCapacityCounts& counts =
            scratch.snapshot.capacityCounts;
        counts.materialInfoIntents = proposalCounts.materialInfoIntents;
        counts.materialVariantProposals =
            proposalCounts.materialVariantProposals;
        counts.instanceObservationProposals =
            proposalCounts.instanceObservationProposals;
        counts.rigidCandidateProposals = proposalCounts.rigidCandidateProposals;
        counts.preparedRigidPayloads = proposalCounts.rigidCandidateProposals;
        counts.preparedRigidVertices = counts.vertices;
        counts.preparedRigidIndexes = counts.indexes;
        counts.receiptSurfaces = proposalCounts.receiptSurfaces;
        counts.frameMaterialIds =
            RtPathTraceCountPureSemanticFrameMaterialIds(scratch.snapshot);

        std::size_t productPlannedBytes = 0;
        std::size_t otherOwnedBytes = scratch.ownedBytes;
        const std::size_t baselineBytes =
            RtPathTraceCommittedBaselineCarrierBytes(baseline);
        if (!PlanPureSemanticProductBytes(counts, productPlannedBytes) ||
            !RtPathTraceProducerCheckedAddBytes(otherOwnedBytes, baselineBytes,
                RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES) ||
            productPlannedBytes > RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES -
                otherOwnedBytes)
        {
            return reject(RtPathTracePureSemanticWorkerReject::CapacityInvalid);
        }
        result.movedScratchOwnedBytes = scratch.ownedBytes;
        result.baselineOwnedBytes = baselineBytes;
        result.peakOwnedBytes = otherOwnedBytes + productPlannedBytes;

        RtPathTraceCaptureProduct candidate;
        candidate.epoch = scratch.snapshot.epoch;
        candidate.lateConsumeToken = scratch.snapshot.lateConsumeToken;
        candidate.viewIdentity = 0;
        candidate.capacityCounts = counts;
        candidate.inputOwnedBytes = otherOwnedBytes;
        if (!ReservePathTraceCaptureProductStorage(scratch.snapshot, candidate,
                otherOwnedBytes, productPlannedBytes,
                testSeam != nullptr ? testSeam->productReserve : nullptr))
        {
            return reject(
                RtPathTracePureSemanticWorkerReject::ProductReserveFailure);
        }
        const RtPathTraceCaptureProductCapacityStamp capacityStamp =
            CapturePathTraceProductCapacityStamp(candidate);
        if (!BuildPathTraceGeometryFromOwnerDecisions(
                scratch.snapshot, candidate))
        {
            return reject(RtPathTracePureSemanticWorkerReject::GeometryFailure);
        }
        RtPathTraceCommittedProposalPayloadTarget payload;
        payload.planningEpoch = &candidate.epoch;
        payload.capacityCounts = &candidate.capacityCounts;
        payload.receiptHeader = &candidate.receiptHeader;
        payload.receiptSurfaces = &candidate.receiptSurfaces;
        payload.materialInfoIntents = &candidate.materialInfoIntents;
        payload.materialVariants = &candidate.materialVariants;
        payload.instanceObservations = &candidate.instanceObservations;
        payload.rigidCandidates = &candidate.rigidCandidates;
        if (!EmitPathTraceCommittedProposalPayload(
                scratch, baseline, payload) ||
             !RtPathTraceEmitPureSemanticFrameMaterialIds(scratch.snapshot, candidate) ||
            ![&]()
            {
                OPTICK_EVENT("PT Lane A Rigid Prepared Payload Build");
                return BuildPathTraceRigidPreparedPayloads(
                    scratch.snapshot, candidate, true);
            }() ||
            candidate.frameMaterialIds.size() != counts.frameMaterialIds ||
            !PathTraceCaptureProductCapacityUnchanged(candidate, capacityStamp))
        {
            return reject(
                RtPathTracePureSemanticWorkerReject::ProposalEmissionFailure);
        }
        candidate.ownerDecisionGeometryOnly = false;
        candidate.receiptHeader.finalizedMaterialIdCount =
            static_cast<std::uint32_t>(candidate.frameMaterialIds.size());
        candidate.receiptHeader.finalizedMaterialIdHash =
            RtPathTraceFinalizedMaterialIdHash(candidate.frameMaterialIds.data(),
                candidate.frameMaterialIds.size());
        candidate.finalizedMaterialIdCount =
            candidate.receiptHeader.finalizedMaterialIdCount;
        candidate.finalizedMaterialIdHash =
            candidate.receiptHeader.finalizedMaterialIdHash;
        candidate.receiptHeader.complete = true;
        candidate.provenance = RtPathTraceCaptureProductProvenance::PureSemantic;
        candidate.complete = true;
        candidate.actualOwnedBytes = candidate.OwnedBytes();
        result.productOwnedBytes = candidate.actualOwnedBytes;
        std::size_t actualPeak = otherOwnedBytes;
        if (!RtPathTraceProducerCheckedAddBytes(actualPeak,
                candidate.actualOwnedBytes,
                RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES) ||
            !RtPathTraceCaptureProductShapeValid(candidate))
        {
            return reject(RtPathTracePureSemanticWorkerReject::OutputShapeInvalid);
        }
        result.peakOwnedBytes = actualPeak;
        output = std::move(candidate);
        result.rejection = RtPathTracePureSemanticWorkerReject::None;
        result.accepted = true;
        return result;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    }
    catch (const std::bad_alloc&)
    {
        return reject(RtPathTracePureSemanticWorkerReject::BadAlloc);
    }
    catch (const std::length_error&)
    {
        return reject(RtPathTracePureSemanticWorkerReject::LengthError);
    }
#endif
}
