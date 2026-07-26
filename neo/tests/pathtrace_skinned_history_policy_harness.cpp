#include "../renderer/NVRHI/PathTraceSkinnedHistoryPolicy.h"

#include <cstdio>

namespace {

int g_failures = 0;

void Expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::printf("FAIL: %s\n", message);
        ++g_failures;
    }
}

PtCanonicalHistoryOwnerKey MakePrimaryOwner()
{
    PtCanonicalHistoryOwnerKey owner;
    owner.worldGeneration = 7;
    owner.ownerGeneration = 11;
    owner.ownerKind =
        PtCanonicalHistoryOwnerKind::RenderTarget;
    owner.ownerRole =
        PtCanonicalHistoryOwnerRole::PrimaryGameplay;
    return owner;
}

void TestGatedPrimaryWithHistory()
{
    PtSkinnedHistoryPolicyInput input;
    input.ownerGate = true;
    input.hasView = true;
    input.owner = MakePrimaryOwner();
    input.matchingOwnerStateFound = true;

    const PtSkinnedHistoryPolicyDecision decision =
        PtSelectSkinnedHistoryPolicy(input);
    Expect(
        decision.primaryView &&
            decision.ownerValid &&
            decision.previousStateFound &&
            decision.readAllowed &&
            decision.writeAllowed,
        "gated primary owner should read and write matching history");
    Expect(
        decision.route == PtSkinnedHistoryRoute::PrimaryOwner,
        "gated primary owner should select the primary-owner route");
}

void TestGatedSubviewCannotBorrowPrimaryHistory()
{
    PtSkinnedHistoryPolicyInput input;
    input.ownerGate = true;
    input.hasView = true;
    input.isSubview = true;
    input.owner = MakePrimaryOwner();
    input.matchingOwnerStateFound = true;

    const PtSkinnedHistoryPolicyDecision decision =
        PtSelectSkinnedHistoryPolicy(input);
    Expect(
        !decision.primaryView &&
            !decision.ownerValid &&
            !decision.previousStateFound &&
            !decision.readAllowed &&
            !decision.writeAllowed,
        "gated subview must not read, borrow, or overwrite primary history");
    Expect(
        decision.route ==
            PtSkinnedHistoryRoute::SubviewOrInvalidNoHistory,
        "gated subview should select the no-history route");
}

void TestGatedPrimaryFirstObservation()
{
    PtSkinnedHistoryPolicyInput input;
    input.ownerGate = true;
    input.hasView = true;
    input.owner = MakePrimaryOwner();

    const PtSkinnedHistoryPolicyDecision decision =
        PtSelectSkinnedHistoryPolicy(input);
    Expect(
        decision.primaryView &&
            decision.ownerValid &&
            !decision.previousStateFound &&
            !decision.readAllowed &&
            decision.writeAllowed,
        "first primary observation should create but not read history");
}

void TestGatedInvalidOwner()
{
    PtSkinnedHistoryPolicyInput input;
    input.ownerGate = true;
    input.hasView = true;
    input.matchingOwnerStateFound = true;

    const PtSkinnedHistoryPolicyDecision decision =
        PtSelectSkinnedHistoryPolicy(input);
    Expect(
        decision.primaryView &&
            !decision.ownerValid &&
            !decision.previousStateFound &&
            !decision.readAllowed &&
            !decision.writeAllowed,
        "invalid primary owner must not read or write history");
}

void TestGatedOwnerMismatch()
{
    PtSkinnedHistoryPolicyInput input;
    input.ownerGate = true;
    input.hasView = true;
    input.owner = MakePrimaryOwner();
    input.ownerMismatchCount = 1;
    input.matchingOwnerStateFound = true;

    const PtSkinnedHistoryPolicyDecision decision =
        PtSelectSkinnedHistoryPolicy(input);
    Expect(
        decision.ownerValid &&
            !decision.previousStateFound &&
            !decision.readAllowed &&
            !decision.writeAllowed,
        "owner mismatch must reject both history read and update");
}

void TestLegacyHistoryPresent()
{
    PtSkinnedHistoryPolicyInput input;
    input.hasView = true;
    input.isSubview = true;
    input.legacyStateFound = true;

    const PtSkinnedHistoryPolicyDecision decision =
        PtSelectSkinnedHistoryPolicy(input);
    Expect(
        !decision.primaryView &&
            !decision.ownerValid &&
            decision.previousStateFound &&
            decision.readAllowed &&
            decision.writeAllowed,
        "gate-off route should preserve legacy last-view behavior");
    Expect(
        decision.route ==
            PtSkinnedHistoryRoute::LegacyLastView,
        "gate-off route should be reported as legacy-last-view");
}

void TestLegacyFirstObservation()
{
    PtSkinnedHistoryPolicyInput input;

    const PtSkinnedHistoryPolicyDecision decision =
        PtSelectSkinnedHistoryPolicy(input);
    Expect(
        !decision.previousStateFound &&
            !decision.readAllowed &&
            decision.writeAllowed,
        "gate-off first observation should write without reading");
}

PtSkinnedSurfaceTemporalPolicyInput MakeStableSurfaceInput()
{
    PtSkinnedSurfaceTemporalPolicyInput input;
    input.rtCpuSkinned = true;
    input.hadPreviousFrame = true;
    input.previousSurfaceFound = true;
    input.loosePreviousSurfaceFound = true;
    return input;
}

void TestSurfaceFirstObservationRejectsHistory()
{
    PtSkinnedSurfaceTemporalPolicyInput input;
    input.rtCpuSkinned = true;

    const PtSkinnedSurfaceTemporalPolicyDecision decision =
        PtSelectSkinnedSurfaceTemporalPolicy(input);
    Expect(
        !decision.previousValid &&
            decision.invalidReasonFlags ==
                RT_SMOKE_SKINNED_INVALID_NO_PREVIOUS_FRAME &&
            decision.temporalStateFlags == 0u,
        "first skinned observation must reject previous motion");
}

void TestStableAnimatedAndLocomotingSurfaceAcceptsHistory()
{
    const PtSkinnedSurfaceTemporalPolicyDecision decision =
        PtSelectSkinnedSurfaceTemporalPolicy(
            MakeStableSurfaceInput());
    const std::uint32_t requiredFlags =
        RT_SMOKE_SKINNED_TEMPORAL_HAS_VALID_PREVIOUS |
        RT_SMOKE_SKINNED_TEMPORAL_TOPOLOGY_STABLE |
        RT_SMOKE_SKINNED_TEMPORAL_LOD_STABLE |
        RT_SMOKE_SKINNED_TEMPORAL_TRANSFORM_CONTINUOUS |
        RT_SMOKE_SKINNED_TEMPORAL_DEFORMATION_CONTINUOUS |
        RT_SMOKE_SKINNED_TEMPORAL_MATERIAL_STABLE |
        RT_SMOKE_SKINNED_TEMPORAL_PREVIOUS_BUFFER_VALID;
    Expect(
        decision.previousValid &&
            decision.invalidReasonFlags ==
                RT_SMOKE_SKINNED_INVALID_NONE &&
            (decision.temporalStateFlags & requiredFlags) ==
                requiredFlags,
        "stable animated or continuously locomoting surface should accept history");
}

void TestSpawnAndOffscreenReturnRejectHistory()
{
    PtSkinnedSurfaceTemporalPolicyInput input;
    input.rtCpuSkinned = true;
    input.hadPreviousFrame = true;

    const PtSkinnedSurfaceTemporalPolicyDecision decision =
        PtSelectSkinnedSurfaceTemporalPolicy(input);
    Expect(
        !decision.previousValid &&
            decision.invalidReasonFlags ==
                RT_SMOKE_SKINNED_INVALID_NO_PREVIOUS_SURFACE &&
            decision.temporalStateFlags == 0u,
        "spawned or offscreen-returning surface must reject absent previous identity");
}

void TestTeleportRejectsHistory()
{
    PtSkinnedSurfaceTemporalPolicyInput input =
        MakeStableSurfaceInput();
    input.transformContinuous = false;

    const PtSkinnedSurfaceTemporalPolicyDecision decision =
        PtSelectSkinnedSurfaceTemporalPolicy(input);
    Expect(
        !decision.previousValid &&
            (decision.invalidReasonFlags &
                RT_SMOKE_SKINNED_INVALID_TRANSFORM_DISCONTINUITY) !=
                0u &&
            (decision.temporalStateFlags &
                RT_SMOKE_SKINNED_TEMPORAL_TRANSFORM_CONTINUOUS) ==
                0u &&
            (decision.temporalStateFlags &
                RT_SMOKE_SKINNED_TEMPORAL_HAS_VALID_PREVIOUS) ==
                0u,
        "teleported surface must reject history and clear transform continuity");
}

void TestMaterialAndSurfaceClassChangesRejectHistory()
{
    PtSkinnedSurfaceTemporalPolicyInput materialInput;
    materialInput.rtCpuSkinned = true;
    materialInput.hadPreviousFrame = true;
    materialInput.loosePreviousSurfaceFound = true;
    materialInput.materialStable = false;
    const PtSkinnedSurfaceTemporalPolicyDecision materialDecision =
        PtSelectSkinnedSurfaceTemporalPolicy(materialInput);
    Expect(
        !materialDecision.previousValid &&
            materialDecision.invalidReasonFlags ==
                RT_SMOKE_SKINNED_INVALID_MATERIAL_CHANGED,
        "material identity change must reject skinned history");

    PtSkinnedSurfaceTemporalPolicyInput classInput =
        materialInput;
    classInput.materialStable = true;
    classInput.surfaceClassStable = false;
    const PtSkinnedSurfaceTemporalPolicyDecision classDecision =
        PtSelectSkinnedSurfaceTemporalPolicy(classInput);
    Expect(
        !classDecision.previousValid &&
            classDecision.invalidReasonFlags ==
                RT_SMOKE_SKINNED_INVALID_SURFACE_CLASS_CHANGED,
        "surface class change must reject skinned history");
}

void TestTopologySkeletonAndBufferFailuresRejectHistory()
{
    PtSkinnedSurfaceTemporalPolicyInput input =
        MakeStableSurfaceInput();
    input.vertexCountStable = false;
    input.indexCountStable = false;
    input.triangleCountStable = false;
    input.skeletonStable = false;
    input.previousBufferAvailable = false;

    const PtSkinnedSurfaceTemporalPolicyDecision decision =
        PtSelectSkinnedSurfaceTemporalPolicy(input);
    const std::uint32_t requiredReasons =
        RT_SMOKE_SKINNED_INVALID_VERTEX_COUNT_MISMATCH |
        RT_SMOKE_SKINNED_INVALID_INDEX_COUNT_MISMATCH |
        RT_SMOKE_SKINNED_INVALID_TRIANGLE_COUNT_MISMATCH |
        RT_SMOKE_SKINNED_INVALID_SKELETON_CHANGED |
        RT_SMOKE_SKINNED_INVALID_PREVIOUS_BUFFER_UNAVAILABLE;
    Expect(
        !decision.previousValid &&
            (decision.invalidReasonFlags & requiredReasons) ==
                requiredReasons &&
            (decision.temporalStateFlags &
                RT_SMOKE_SKINNED_TEMPORAL_TOPOLOGY_STABLE) ==
                0u &&
            (decision.temporalStateFlags &
                RT_SMOKE_SKINNED_TEMPORAL_DEFORMATION_CONTINUOUS) ==
                0u &&
            (decision.temporalStateFlags &
                RT_SMOKE_SKINNED_TEMPORAL_PREVIOUS_BUFFER_VALID) ==
                0u,
        "topology, skeleton, and previous-buffer failures must reject history");
}

} // namespace

int main()
{
    TestGatedPrimaryWithHistory();
    TestGatedSubviewCannotBorrowPrimaryHistory();
    TestGatedPrimaryFirstObservation();
    TestGatedInvalidOwner();
    TestGatedOwnerMismatch();
    TestLegacyHistoryPresent();
    TestLegacyFirstObservation();
    TestSurfaceFirstObservationRejectsHistory();
    TestStableAnimatedAndLocomotingSurfaceAcceptsHistory();
    TestSpawnAndOffscreenReturnRejectHistory();
    TestTeleportRejectsHistory();
    TestMaterialAndSurfaceClassChangesRejectHistory();
    TestTopologySkeletonAndBufferFailuresRejectHistory();

    if (g_failures != 0)
    {
        std::printf(
            "PathTraceSkinnedHistoryPolicyHarness: %d failure(s)\n",
            g_failures);
        return 1;
    }

    std::printf("PathTraceSkinnedHistoryPolicyHarness: PASS\n");
    return 0;
}
