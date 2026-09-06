#pragma once

#include "PathTraceCommittedBaseline.h"
#include "PathTraceCommittedCapture.h"
#include "PathTraceCommittedReferencedSetReceipt.h"
#include "PathTraceCaptureProduct.h"

#include <cstddef>
#include <cstdint>
#include <type_traits>

static_assert(RT_PT_COMMITTED_REFERENCED_SET_MAP_NAME_CAPACITY ==
    RT_PT_COMMITTED_BASELINE_MAP_NAME_CAPACITY,
    "receipt and committed-baseline map identity widths must remain exact");

// Phase-2a only: dependency-light semantic facts over an immutable committed
// baseline.  No production caller is introduced by this slice.
class RtPathTraceCommittedSemanticFactsProvider final :
    public RtPathTraceCaptureSemanticFactsProvider,
    public RtPathTraceCommittedReferencedSetFactsProvider
{
public:
    explicit RtPathTraceCommittedSemanticFactsProvider(
        const RtPathTraceCommittedPlanningBaseline& baseline) noexcept;

    bool Complete() const noexcept override;
    bool IsRigidRouteReady(std::uint64_t meshHash) const override;
    bool IsRigidRouteResidentReadyForEntityMaterial(
        std::int32_t entityIndex, std::int32_t renderEntityNum,
        std::uint32_t materialId) const override;
    const PtGeometryIdentityBinding* FindCanonicalIdentityBinding(
        const PtCanonicalInstanceKey& instance) const override;
    const PtGeometrySourceRecord* FindCanonicalSourceRecord(
        const PtCanonicalMeshKey& mesh) const override;

    std::size_t CanonicalQueryViolationCount() const noexcept;
    std::size_t RigidReadyQueryCount() const noexcept;
    std::size_t ResidentReadyQueryCount() const noexcept;

private:
    const RtPathTraceCommittedPlanningBaseline& baseline;
    mutable std::size_t canonicalQueryViolations = 0;
    mutable std::size_t rigidReadyQueries = 0;
    mutable std::size_t residentReadyQueries = 0;
};

enum class RtPathTracePrimarySemanticBridgeFailure : std::uint8_t
{
    None,
    BadAlloc,
    LengthError
};

struct RtPathTracePrimarySemanticBridgeTestSeam
{
    std::size_t failAtAllocation = SIZE_MAX;
    std::size_t allocationCalls = 0;
    RtPathTracePrimarySemanticBridgeFailure failure =
        RtPathTracePrimarySemanticBridgeFailure::None;
};

struct RtPathTracePrimarySemanticScratch
{
    std::uint64_t sealedPrimaryViewToken = 0;
    RtPathTraceCaptureOwnerSnapshot snapshot;
    std::size_t ownedBytes = 0;
    bool complete = false;
};
static_assert(std::is_nothrow_move_assignable<
    RtPathTracePrimarySemanticScratch>::value,
    "semantic scratch publication must remain non-throwing");

bool BuildPathTracePrimarySemanticScratch(
    const RtPathTracePrimarySemanticDto& dto,
    std::uint64_t expectedSealedPrimaryViewToken,
    const RtPathTraceCommittedSemanticConfig& semanticConfig,
    RtPathTracePrimarySemanticScratch& output,
    RtPathTracePrimarySemanticBridgeTestSeam* testSeam = nullptr) noexcept;
