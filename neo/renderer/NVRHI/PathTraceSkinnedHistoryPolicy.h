#pragma once

#include "PathTraceCanonicalGeometryIdentity.h"

#include <cstdint>

enum RtSmokeSkinnedSurfaceInvalidReasonFlags : std::uint32_t
{
    RT_SMOKE_SKINNED_INVALID_NONE = 0u,
    RT_SMOKE_SKINNED_INVALID_NO_PREVIOUS_FRAME = 1u << 0,
    RT_SMOKE_SKINNED_INVALID_NO_PREVIOUS_SURFACE = 1u << 1,
    RT_SMOKE_SKINNED_INVALID_VERTEX_COUNT_MISMATCH = 1u << 2,
    RT_SMOKE_SKINNED_INVALID_INDEX_COUNT_MISMATCH = 1u << 3,
    RT_SMOKE_SKINNED_INVALID_TRIANGLE_COUNT_MISMATCH = 1u << 4,
    RT_SMOKE_SKINNED_INVALID_MATERIAL_CHANGED = 1u << 5,
    RT_SMOKE_SKINNED_INVALID_SURFACE_CLASS_CHANGED = 1u << 6,
    RT_SMOKE_SKINNED_INVALID_NOT_RT_CPU_SKINNED = 1u << 7,
    RT_SMOKE_SKINNED_INVALID_SKELETON_CHANGED = 1u << 8,
    RT_SMOKE_SKINNED_INVALID_TRANSFORM_DISCONTINUITY = 1u << 9,
    RT_SMOKE_SKINNED_INVALID_PREVIOUS_BUFFER_UNAVAILABLE = 1u << 10
};

enum RtSmokeSkinnedSurfaceTemporalStateFlags : std::uint32_t
{
    RT_SMOKE_SKINNED_TEMPORAL_HAS_VALID_PREVIOUS = 1u << 0,
    RT_SMOKE_SKINNED_TEMPORAL_TOPOLOGY_STABLE = 1u << 1,
    RT_SMOKE_SKINNED_TEMPORAL_LOD_STABLE = 1u << 2,
    RT_SMOKE_SKINNED_TEMPORAL_TRANSFORM_CONTINUOUS = 1u << 3,
    RT_SMOKE_SKINNED_TEMPORAL_DEFORMATION_CONTINUOUS = 1u << 4,
    RT_SMOKE_SKINNED_TEMPORAL_MATERIAL_STABLE = 1u << 5,
    RT_SMOKE_SKINNED_TEMPORAL_PREVIOUS_BUFFER_VALID = 1u << 6
};

struct PtSkinnedSurfaceTemporalPolicyInput
{
    bool rtCpuSkinned = false;
    bool hadPreviousFrame = false;
    bool previousSurfaceFound = false;
    bool loosePreviousSurfaceFound = false;
    bool materialStable = true;
    bool surfaceClassStable = true;
    bool vertexCountStable = true;
    bool indexCountStable = true;
    bool triangleCountStable = true;
    bool skeletonStable = true;
    bool transformContinuous = true;
    bool previousBufferAvailable = true;
};

struct PtSkinnedSurfaceTemporalPolicyDecision
{
    std::uint32_t invalidReasonFlags =
        RT_SMOKE_SKINNED_INVALID_NONE;
    std::uint32_t temporalStateFlags = 0u;
    bool previousValid = false;
};

inline PtSkinnedSurfaceTemporalPolicyDecision
PtSelectSkinnedSurfaceTemporalPolicy(
    const PtSkinnedSurfaceTemporalPolicyInput& input)
{
    PtSkinnedSurfaceTemporalPolicyDecision decision;
    if (!input.rtCpuSkinned)
    {
        decision.invalidReasonFlags |=
            RT_SMOKE_SKINNED_INVALID_NOT_RT_CPU_SKINNED;
    }
    if (!input.hadPreviousFrame)
    {
        decision.invalidReasonFlags |=
            RT_SMOKE_SKINNED_INVALID_NO_PREVIOUS_FRAME;
    }
    else if (!input.previousSurfaceFound)
    {
        if (input.loosePreviousSurfaceFound &&
            !input.surfaceClassStable)
        {
            decision.invalidReasonFlags |=
                RT_SMOKE_SKINNED_INVALID_SURFACE_CLASS_CHANGED;
        }
        else if (input.loosePreviousSurfaceFound &&
            !input.materialStable)
        {
            decision.invalidReasonFlags |=
                RT_SMOKE_SKINNED_INVALID_MATERIAL_CHANGED;
        }
        else
        {
            decision.invalidReasonFlags |=
                RT_SMOKE_SKINNED_INVALID_NO_PREVIOUS_SURFACE;
        }
    }
    else
    {
        if (!input.vertexCountStable)
        {
            decision.invalidReasonFlags |=
                RT_SMOKE_SKINNED_INVALID_VERTEX_COUNT_MISMATCH;
        }
        if (!input.indexCountStable)
        {
            decision.invalidReasonFlags |=
                RT_SMOKE_SKINNED_INVALID_INDEX_COUNT_MISMATCH;
        }
        if (!input.triangleCountStable)
        {
            decision.invalidReasonFlags |=
                RT_SMOKE_SKINNED_INVALID_TRIANGLE_COUNT_MISMATCH;
        }
        if (!input.skeletonStable)
        {
            decision.invalidReasonFlags |=
                RT_SMOKE_SKINNED_INVALID_SKELETON_CHANGED;
        }
        if (!input.transformContinuous)
        {
            decision.invalidReasonFlags |=
                RT_SMOKE_SKINNED_INVALID_TRANSFORM_DISCONTINUITY;
        }
        if (!input.previousBufferAvailable)
        {
            decision.invalidReasonFlags |=
                RT_SMOKE_SKINNED_INVALID_PREVIOUS_BUFFER_UNAVAILABLE;
        }

        const std::uint32_t topologyReasons =
            RT_SMOKE_SKINNED_INVALID_VERTEX_COUNT_MISMATCH |
            RT_SMOKE_SKINNED_INVALID_INDEX_COUNT_MISMATCH |
            RT_SMOKE_SKINNED_INVALID_TRIANGLE_COUNT_MISMATCH;
        if ((decision.invalidReasonFlags & topologyReasons) == 0u)
        {
            decision.temporalStateFlags |=
                RT_SMOKE_SKINNED_TEMPORAL_TOPOLOGY_STABLE;
        }
        decision.temporalStateFlags |=
            RT_SMOKE_SKINNED_TEMPORAL_LOD_STABLE |
            RT_SMOKE_SKINNED_TEMPORAL_MATERIAL_STABLE;
        if (input.transformContinuous)
        {
            decision.temporalStateFlags |=
                RT_SMOKE_SKINNED_TEMPORAL_TRANSFORM_CONTINUOUS;
        }
        if (input.skeletonStable && input.rtCpuSkinned)
        {
            decision.temporalStateFlags |=
                RT_SMOKE_SKINNED_TEMPORAL_DEFORMATION_CONTINUOUS;
        }
        if (input.previousBufferAvailable)
        {
            decision.temporalStateFlags |=
                RT_SMOKE_SKINNED_TEMPORAL_PREVIOUS_BUFFER_VALID;
        }
    }

    decision.previousValid =
        input.previousSurfaceFound &&
        decision.invalidReasonFlags == RT_SMOKE_SKINNED_INVALID_NONE;
    if (decision.previousValid)
    {
        decision.temporalStateFlags |=
            RT_SMOKE_SKINNED_TEMPORAL_HAS_VALID_PREVIOUS;
    }
    return decision;
}

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
