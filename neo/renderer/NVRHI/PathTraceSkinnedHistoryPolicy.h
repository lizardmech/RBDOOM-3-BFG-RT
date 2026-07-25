#pragma once

#include "PathTraceCanonicalGeometryIdentity.h"

enum class PtSkinnedHistoryRoute
{
    LegacyLastView = 0,
    PrimaryOwner,
    SubviewOrInvalidNoHistory
};

struct PtSkinnedHistoryPolicyInput
{
    bool ownerGate = false;
    bool hasView = false;
    bool isSubview = false;
    PtCanonicalHistoryOwnerKey owner;
    int ownerMismatchCount = 0;
    bool matchingOwnerStateFound = false;
    bool legacyStateFound = false;
};

struct PtSkinnedHistoryPolicyDecision
{
    bool primaryView = false;
    bool ownerValid = false;
    bool previousStateFound = false;
    bool readAllowed = false;
    bool writeAllowed = false;
    PtSkinnedHistoryRoute route =
        PtSkinnedHistoryRoute::SubviewOrInvalidNoHistory;
};

inline PtSkinnedHistoryPolicyDecision PtSelectSkinnedHistoryPolicy(
    const PtSkinnedHistoryPolicyInput& input)
{
    PtSkinnedHistoryPolicyDecision decision;
    decision.primaryView =
        input.hasView &&
        !input.isSubview;

    if (!input.ownerGate)
    {
        decision.previousStateFound =
            input.legacyStateFound;
        decision.readAllowed =
            input.legacyStateFound;
        decision.writeAllowed = true;
        decision.route =
            PtSkinnedHistoryRoute::LegacyLastView;
        return decision;
    }

    decision.ownerValid =
        decision.primaryView &&
        PtCanonicalHistoryOwnerKeyIsValid(input.owner);
    decision.writeAllowed =
        decision.ownerValid &&
        input.ownerMismatchCount == 0;
    decision.previousStateFound =
        decision.writeAllowed &&
        input.matchingOwnerStateFound;
    decision.readAllowed =
        decision.previousStateFound;
    decision.route =
        decision.ownerValid
            ? PtSkinnedHistoryRoute::PrimaryOwner
            : PtSkinnedHistoryRoute::SubviewOrInvalidNoHistory;
    return decision;
}

inline const char* PtSkinnedHistoryRouteName(
    PtSkinnedHistoryRoute route)
{
    switch (route)
    {
        case PtSkinnedHistoryRoute::LegacyLastView:
            return "legacy-last-view";
        case PtSkinnedHistoryRoute::PrimaryOwner:
            return "primary-owner";
        case PtSkinnedHistoryRoute::SubviewOrInvalidNoHistory:
        default:
            return "subview-or-invalid-no-history";
    }
}
