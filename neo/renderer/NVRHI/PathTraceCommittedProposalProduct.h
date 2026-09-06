#pragma once

#include "PathTraceCommittedSemanticFacts.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>
#include <vector>

enum class RtPathTraceCommittedProposalFamily : std::uint8_t
{
    MaterialInfoIntent,
    MaterialVariant,
    InstanceObservation,
    RigidCandidate
};

using RtPathTraceCommittedProposalProvenance =
    RtPathTraceCaptureProductProvenance;

// Serial authority:
// PathTraceSceneCapture.cpp:3299-3302 registers material info before variant;
// PathTraceDrawSurfCapture.cpp:735-790 records the instance before the rigid
// candidate.  Material selection precedes the draw-surface identity walk.
constexpr std::array<RtPathTraceCommittedProposalFamily, 4>
    RT_PT_COMMITTED_PROPOSAL_APPLY_ORDER = {
        RtPathTraceCommittedProposalFamily::MaterialInfoIntent,
        RtPathTraceCommittedProposalFamily::MaterialVariant,
        RtPathTraceCommittedProposalFamily::InstanceObservation,
        RtPathTraceCommittedProposalFamily::RigidCandidate
    };

struct RtPathTraceCommittedProposalCapacityPlan
{
    RtPathTraceCaptureCapacityCounts counts;
    std::size_t inputOwnedBytes = 0;
    std::size_t proposalOwnedBytes = 0;
    std::size_t proposalBudgetBytes = 0;
    std::size_t peakOwnedBytes = 0;
};

struct RtPathTraceCommittedProposalProduct
{
    RtPathTracePlanningSnapshotEpoch planningEpoch;
    RtPathTraceCaptureCapacityCounts capacityCounts;
    std::vector<RtPathTraceMaterialInfoRegistrationIntent> materialInfoIntents;
    std::vector<RtPathTraceMaterialVariantRegistrationProposal> materialVariants;
    std::vector<RtPathTraceInstanceObservationProposal> instanceObservations;
    std::vector<RtPathTraceRigidCandidateProposal> rigidCandidates;
    RtPathTraceCommittedReferencedSetReceiptHeader receiptHeader;
    std::vector<RtPathTraceCommittedReferencedSurfaceReceiptRow> receiptSurfaces;
    std::size_t inputOwnedBytes = 0;
    std::size_t actualOwnedBytes = 0;
    RtPathTraceCommittedProposalProvenance provenance =
        RtPathTraceCommittedProposalProvenance::None;
    bool complete = false;

    std::size_t OwnedBytes() const noexcept;
};
static_assert(std::is_nothrow_move_assignable<
    RtPathTraceCommittedProposalProduct>::value,
    "P2c-2 proposal publication must remain non-throwing");

template<typename T>
inline bool RtPathTracePureSemanticProposalOrdinalsStrict(
    const T* values, std::size_t valueCount,
    std::size_t surfaceCount) noexcept
{
    if (valueCount != 0 && values == nullptr)
    {
        return false;
    }
    for (std::size_t index = 0; index < valueCount; ++index)
    {
        if (values[index].surfaceOrdinal >= surfaceCount ||
            (index != 0 && values[index - 1].surfaceOrdinal >=
                values[index].surfaceOrdinal))
        {
            return false;
        }
    }
    return true;
}

// Shared PureSemantic payload authority for both the committed proposal
// product and the unified capture product. Product-specific owned-byte and
// provenance checks remain at their respective publication boundaries.
inline bool RtPathTracePureSemanticProposalPayloadShapeValid(
    const RtPathTraceCaptureCapacityCounts& counts,
    const RtPathTracePlanningSnapshotEpoch& epoch,
    const RtPathTraceCommittedReferencedSetReceiptHeader& receiptHeader,
    const RtPathTraceCommittedReferencedSurfaceReceiptRow* receiptRows,
    std::size_t receiptRowCount,
    const RtPathTraceMaterialInfoRegistrationIntent* materialInfoIntents,
    std::size_t materialInfoIntentCount,
    const RtPathTraceMaterialVariantRegistrationProposal* materialVariants,
    std::size_t materialVariantCount,
    const RtPathTraceInstanceObservationProposal* instanceObservations,
    std::size_t instanceObservationCount,
    const RtPathTraceRigidCandidateProposal* rigidCandidates,
    std::size_t rigidCandidateCount) noexcept
{
    if (materialInfoIntentCount != counts.materialInfoIntents ||
        materialVariantCount != counts.materialVariantProposals ||
        instanceObservationCount != counts.instanceObservationProposals ||
        rigidCandidateCount != counts.rigidCandidateProposals ||
        receiptRowCount != counts.receiptSurfaces ||
        receiptRowCount != counts.surfaces ||
        materialInfoIntentCount != materialVariantCount ||
        !RtPathTraceCommittedReferencedSetReceiptStorageShapeValid(
            receiptHeader, receiptRows, receiptRowCount) ||
        receiptHeader.sealedPrimaryViewToken == 0 ||
        epoch.generation == UINT64_MAX ||
        receiptHeader.sealedPrimaryViewToken != epoch.generation + 1 ||
        receiptHeader.ownerUniverseFrameIndex != epoch.frameIndex ||
        receiptHeader.mapTimeStamp != epoch.mapTimeStamp ||
        receiptHeader.mapLoadSerial != epoch.mapLoadSerial ||
        std::memcmp(receiptHeader.mapName, epoch.mapName,
            sizeof(receiptHeader.mapName)) != 0 ||
        receiptHeader.worldLifecycleGeneration == 0 ||
        receiptHeader.barrierGeneration == 0 ||
        receiptHeader.materialRegistryGeneration == 0 ||
        receiptHeader.residentMaterialFactsGeneration == 0 ||
        receiptHeader.configFingerprint == 0 ||
        receiptHeader.instanceUniverseGeneration == 0 ||
        receiptHeader.geometryUniverseGeneration == 0 ||
        receiptHeader.staticMaterialGeneration == 0 ||
        receiptHeader.canonicalSourceIndexPoolGeneration == 0 ||
        receiptHeader.staticResidentPayloadGeneration == 0 ||
        !RtPathTracePureSemanticProposalOrdinalsStrict(
            materialInfoIntents, materialInfoIntentCount, counts.surfaces) ||
        !RtPathTracePureSemanticProposalOrdinalsStrict(
            materialVariants, materialVariantCount, counts.surfaces) ||
        !RtPathTracePureSemanticProposalOrdinalsStrict(
            instanceObservations, instanceObservationCount, counts.surfaces) ||
        !RtPathTracePureSemanticProposalOrdinalsStrict(
            rigidCandidates, rigidCandidateCount, counts.surfaces))
    {
        return false;
    }
    for (std::size_t index = 0; index < materialInfoIntentCount; ++index)
    {
        if (materialInfoIntents[index].surfaceOrdinal !=
                materialVariants[index].surfaceOrdinal ||
            !materialInfoIntents[index].callPresent ||
            materialInfoIntents[index].reason != 1)
        {
            return false;
        }
    }
    return true;
}

enum class RtPathTraceCommittedProposalBuildReject : std::uint8_t
{
    None,
    InputInvalid,
    TokenMismatch,
    SurfaceShapeInvalid,
    PersistedDecisionInvalid,
    RecordAllIdentityUnavailable,
    CapacityInvalid,
    BadAlloc,
    LengthError,
    OutputShapeInvalid
};

enum class RtPathTraceCommittedProposalAllocationFailure : std::uint8_t
{
    None,
    BadAlloc,
    LengthError
};

struct RtPathTraceCommittedProposalPayloadTarget
{
    RtPathTracePlanningSnapshotEpoch* planningEpoch = nullptr;
    RtPathTraceCaptureCapacityCounts* capacityCounts = nullptr;
    RtPathTraceCommittedReferencedSetReceiptHeader* receiptHeader = nullptr;
    std::vector<RtPathTraceCommittedReferencedSurfaceReceiptRow>*
        receiptSurfaces = nullptr;
    std::vector<RtPathTraceMaterialInfoRegistrationIntent>*
        materialInfoIntents = nullptr;
    std::vector<RtPathTraceMaterialVariantRegistrationProposal>*
        materialVariants = nullptr;
    std::vector<RtPathTraceInstanceObservationProposal>*
        instanceObservations = nullptr;
    std::vector<RtPathTraceRigidCandidateProposal>* rigidCandidates = nullptr;
};

bool CountPathTraceCommittedProposalPayload(
    const RtPathTracePrimarySemanticScratch& scratch,
    const RtPathTraceCommittedPlanningBaseline& baseline,
    RtPathTraceCaptureCapacityCounts& counts,
    RtPathTraceCommittedProposalBuildReject& rejection) noexcept;

bool EmitPathTraceCommittedProposalPayload(
    const RtPathTracePrimarySemanticScratch& scratch,
    const RtPathTraceCommittedPlanningBaseline& baseline,
    const RtPathTraceCommittedProposalPayloadTarget& target) noexcept;

struct RtPathTraceCommittedProposalBuildTestSeam
{
    std::size_t failAtReserve = SIZE_MAX;
    std::size_t inflateAtReserve = SIZE_MAX;
    std::size_t inflateBy = 0;
    std::size_t reserveCalls = 0;
    RtPathTraceCommittedProposalAllocationFailure failure =
        RtPathTraceCommittedProposalAllocationFailure::None;
};

struct RtPathTraceCommittedProposalBuildResult
{
    RtPathTraceCommittedProposalBuildReject rejection =
        RtPathTraceCommittedProposalBuildReject::None;
    RtPathTraceCaptureCapacityCounts counts;
    std::size_t actualOwnedBytes = 0;
    bool accepted = false;
};

bool PlanPathTraceCommittedProposalCapacity(
    std::size_t inputOwnedBytes,
    const RtPathTraceCaptureCapacityCounts& counts,
    RtPathTraceCommittedProposalCapacityPlan& plan) noexcept;

bool RtPathTraceCommittedProposalProductShapeValid(
    const RtPathTraceCommittedProposalProduct& product) noexcept;

RtPathTraceCommittedProposalBuildResult
BuildPathTraceCommittedProposalProductP2c2(
    const RtPathTracePrimarySemanticScratch& scratch,
    const RtPathTraceCommittedPlanningBaseline& baseline,
    RtPathTraceCommittedProposalProduct& output,
    RtPathTraceCommittedProposalBuildTestSeam* testSeam = nullptr) noexcept;
