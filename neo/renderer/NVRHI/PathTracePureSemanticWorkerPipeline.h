#pragma once

#include "PathTraceCommittedProposalProduct.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

enum class RtPathTracePureSemanticWorkerReject : std::uint8_t
{
    None,
    InputInvalid,
    TokenMismatch,
    GenerationMismatch,
    ConfigMismatch,
    DispatchGuard,
    OwnerDecisionCapacity,
    FinalizerFailure,
    CanonicalQueryViolation,
    DerivedFactsIncomplete,
    DecisionFailure,
    ProposalCountFailure,
    CapacityInvalid,
    ProductReserveFailure,
    GeometryFailure,
    ProposalEmissionFailure,
    OutputShapeInvalid,
    BadAlloc,
    LengthError
};

struct RtPathTracePureSemanticWorkerTestSeam
{
    RtPathTraceCaptureReserveTestSeam* productReserve = nullptr;
    bool failOwnerDecisionReserve = false;
};

struct RtPathTracePureSemanticWorkerResult
{
    RtPathTracePureSemanticWorkerReject rejection =
        RtPathTracePureSemanticWorkerReject::None;
    std::size_t movedScratchOwnedBytes = 0;
    std::size_t baselineOwnedBytes = 0;
    std::size_t productOwnedBytes = 0;
    std::size_t peakOwnedBytes = 0;
    std::size_t finalizerCalls = 0;
    std::size_t canonicalQueryViolations = 0;
    bool inputConsumed = false;
    bool accepted = false;
};

// Complete frame material authority used by the PureSemantic worker product.
// Both finalized base and chosen IDs participate; zero remains an ordinary
// authoritative ID.  These helpers are shared with focused parity tests so
// the test does not carry a second collector.
std::size_t RtPathTraceCountPureSemanticFrameMaterialIds(
    const RtPathTraceCaptureOwnerSnapshot& snapshot) noexcept;
bool RtPathTraceEmitPureSemanticFrameMaterialIds(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    RtPathTraceCaptureProduct& product);

RtPathTracePureSemanticWorkerResult BuildPathTracePureSemanticWorkerProductS3(
    RtPathTracePrimarySemanticScratch&& scratch,
    const RtPathTraceCommittedPlanningBaseline& baseline,
    RtPathTraceCaptureProduct& output,
    const RtPathTracePureSemanticWorkerTestSeam* testSeam = nullptr) noexcept;

using RtPathTracePureSemanticWorkerFunction =
    RtPathTracePureSemanticWorkerResult (*)(
        RtPathTracePrimarySemanticScratch&&,
        const RtPathTraceCommittedPlanningBaseline&,
        RtPathTraceCaptureProduct&,
        const RtPathTracePureSemanticWorkerTestSeam*) noexcept;
static_assert(std::is_same<
    decltype(&BuildPathTracePureSemanticWorkerProductS3),
    RtPathTracePureSemanticWorkerFunction>::value,
    "S3 ownership boundary must accept only an explicit rvalue scratch");
