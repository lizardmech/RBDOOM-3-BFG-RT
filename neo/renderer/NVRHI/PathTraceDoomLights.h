#pragma once

// Doom renderer light diagnostics for future analytic PT/ReSTIR candidates.
//
// This module inspects renderer lightDefs, prints stable identity / proximity
// information, and builds a separate analytic light candidate buffer. It does
// not add light meshes to the BVH.

#include <cstdint>
#include <vector>

struct viewDef_t;

struct PathTraceDoomAnalyticLightCandidate
{
    float originAndRadius[4];
    float colorAndIntensity[4];
    float doomRadiusAndArea[4];
    uint32_t flags = 0;
    uint32_t renderLightIndex = 0;
    uint32_t entityNumber = 0;
    uint32_t padding0 = 0;
};
static_assert((sizeof(PathTraceDoomAnalyticLightCandidate) % 16) == 0, "PathTraceDoomAnalyticLightCandidate must stay 16-byte aligned for HLSL StructuredBuffer reads");

static constexpr uint32_t PATH_TRACE_DOOM_ANALYTIC_LIGHT_INVALID_INDEX = 0xffffffffu;

enum PathTraceDoomAnalyticLightIdentityFlags : uint32_t
{
    PATH_TRACE_DOOM_ANALYTIC_IDENTITY_VALID = 1u << 0,
    PATH_TRACE_DOOM_ANALYTIC_IDENTITY_SAMPLEABLE = 1u << 1,
    PATH_TRACE_DOOM_ANALYTIC_IDENTITY_REMAP_VALID = 1u << 2
};

enum DoomAnalyticLightUniverseInvalidReason : uint32_t
{
    DOOM_LIGHT_UNIVERSE_INVALID_MISSING_PREVIOUS = 0x00000001u,
    DOOM_LIGHT_UNIVERSE_INVALID_MISSING_CURRENT = 0x00000002u,
    DOOM_LIGHT_UNIVERSE_INVALID_DUPLICATE_KEY = 0x00000004u,
    DOOM_LIGHT_UNIVERSE_INVALID_UNKNOWN_ENTITY = 0x00000008u,
    DOOM_LIGHT_UNIVERSE_INVALID_UNPROVEN_CONTINUITY = 0x00000010u,
    DOOM_LIGHT_UNIVERSE_INVALID_ZERO_RADIANCE = 0x00000020u,
    DOOM_LIGHT_UNIVERSE_INVALID_SUPPRESSED = 0x00000040u,
    DOOM_LIGHT_UNIVERSE_INVALID_OUT_OF_SELECTED_AREA = 0x00000080u,
    DOOM_LIGHT_UNIVERSE_INVALID_NON_POINT_OR_PARALLEL = 0x00000100u,
    DOOM_LIGHT_UNIVERSE_INVALID_RADIUS_INVALID = 0x00000200u,
    DOOM_LIGHT_UNIVERSE_INVALID_CANDIDATE_CAP_DROPPED = 0x00000400u,
    DOOM_LIGHT_UNIVERSE_INVALID_DISCONNECTED_OR_PORTAL = 0x00000800u,
};

struct PathTraceDoomAnalyticLightCandidateIdentity
{
    uint32_t universeIndex = PATH_TRACE_DOOM_ANALYTIC_LIGHT_INVALID_INDEX;
    uint32_t flags = 0;
    uint32_t invalidReasonFlags = 0;
    uint32_t remapIndex = PATH_TRACE_DOOM_ANALYTIC_LIGHT_INVALID_INDEX;
};
static_assert((sizeof(PathTraceDoomAnalyticLightCandidateIdentity) % 16) == 0, "PathTraceDoomAnalyticLightCandidateIdentity must stay 16-byte aligned for HLSL StructuredBuffer reads");

struct PathTraceDoomAnalyticLightRemap
{
    int32_t previousToCurrentCandidateIndex = -1;
    int32_t currentToPreviousCandidateIndex = -1;
    uint32_t flags = 0;
    uint32_t invalidReasonFlags = 0;
};
static_assert((sizeof(PathTraceDoomAnalyticLightRemap) % 16) == 0, "PathTraceDoomAnalyticLightRemap must stay 16-byte aligned for HLSL StructuredBuffer reads");

struct PathTraceDoomAnalyticLightGpuRemap
{
    std::vector<PathTraceDoomAnalyticLightCandidate> previousCandidates;
    std::vector<PathTraceDoomAnalyticLightCandidateIdentity> currentCandidateIdentities;
    std::vector<PathTraceDoomAnalyticLightCandidateIdentity> previousCandidateIdentities;
    std::vector<PathTraceDoomAnalyticLightRemap> universeRemap;
    int invalidRemapCount = 0;
};

struct PathTraceDoomAnalyticLightBuildOptions
{
    bool forceBuild = false;
    bool preserveZeroRadianceSlots = false;
    bool stableReservoirOrder = false;
    bool includeOutOfSelectedArea = false;
    bool ignoreConfiguredCandidateCap = false;
    bool requireProvenContinuity = false;
};

std::vector<PathTraceDoomAnalyticLightCandidate> BuildPathTraceDoomAnalyticLightCandidates(const viewDef_t* viewDef, const PathTraceDoomAnalyticLightBuildOptions& options = PathTraceDoomAnalyticLightBuildOptions());
std::vector<PathTraceDoomAnalyticLightCandidate> BuildPathTraceDoomAnalyticLightCandidates(const viewDef_t* viewDef, bool forceEnable);
const PathTraceDoomAnalyticLightGpuRemap& GetPathTraceDoomAnalyticLightGpuRemap();
