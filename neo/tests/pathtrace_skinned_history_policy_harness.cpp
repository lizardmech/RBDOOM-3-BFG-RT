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
