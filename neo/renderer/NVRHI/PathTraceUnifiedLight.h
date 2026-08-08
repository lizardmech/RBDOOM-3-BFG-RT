#pragma once

// Unified local-light record shape for the ReSTIR-friendly light-universe
// rebuild. This is a layout contract only; upload/binding comes later.

#include <cstdint>
#include <vector>

struct PathTraceSmokeEmissiveTriangle;
struct PathTraceEmissiveLightRemap;
struct PathTraceEmissiveDistributionEntry;
struct PathTraceDoomAnalyticLightCandidate;
struct PathTraceDoomAnalyticLightCandidateIdentity;
struct PathTraceDoomAnalyticLightRemap;

static constexpr uint32_t PATH_TRACE_UNIFIED_LIGHT_TYPE_INVALID = 0u;
static constexpr uint32_t PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE = 1u;
static constexpr uint32_t PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC = 2u;
static constexpr uint32_t PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX = 0xffffffffu;

static constexpr uint32_t PATH_TRACE_RLU_LIGHT_FLAG_CURRENT_SAMPLEABLE = 1u << 24;
static constexpr uint32_t PATH_TRACE_RLU_LIGHT_FLAG_STABLE_CACHEABLE = 1u << 25;
static constexpr uint32_t PATH_TRACE_RLU_LIGHT_FLAG_UNSTABLE_DYNAMIC = 1u << 26;
static constexpr uint32_t PATH_TRACE_RLU_LIGHT_CLASSIFICATION_FLAG_MASK =
    PATH_TRACE_RLU_LIGHT_FLAG_CURRENT_SAMPLEABLE |
    PATH_TRACE_RLU_LIGHT_FLAG_STABLE_CACHEABLE |
    PATH_TRACE_RLU_LIGHT_FLAG_UNSTABLE_DYNAMIC;

static constexpr uint32_t PATH_TRACE_RLU_STABILITY_REASON_NO_REMAP = 1u << 16;
static constexpr uint32_t PATH_TRACE_RLU_STABILITY_REASON_PAYLOAD_CHANGED = 1u << 17;
static constexpr uint32_t PATH_TRACE_RLU_STABILITY_REASON_UNPROVEN_CONTINUITY = 1u << 18;
static constexpr uint32_t PATH_TRACE_RLU_STABILITY_REASON_UNKNOWN_IDENTITY = 1u << 19;
static constexpr uint32_t PATH_TRACE_RLU_STABILITY_REASON_DUPLICATE_IDENTITY = 1u << 20;
static constexpr uint32_t PATH_TRACE_RLU_STABILITY_REASON_PORTAL_DISCONNECTED = 1u << 21;
static constexpr uint32_t PATH_TRACE_RLU_STABILITY_REASON_OUT_OF_SELECTED_AREA = 1u << 22;
static constexpr uint32_t PATH_TRACE_RLU_STABILITY_REASON_FLAG_MASK =
    PATH_TRACE_RLU_STABILITY_REASON_NO_REMAP |
    PATH_TRACE_RLU_STABILITY_REASON_PAYLOAD_CHANGED |
    PATH_TRACE_RLU_STABILITY_REASON_UNPROVEN_CONTINUITY |
    PATH_TRACE_RLU_STABILITY_REASON_UNKNOWN_IDENTITY |
    PATH_TRACE_RLU_STABILITY_REASON_DUPLICATE_IDENTITY |
    PATH_TRACE_RLU_STABILITY_REASON_PORTAL_DISCONNECTED |
    PATH_TRACE_RLU_STABILITY_REASON_OUT_OF_SELECTED_AREA;
static constexpr uint32_t PATH_TRACE_RLU_STABILITY_FLAG_MASK =
    PATH_TRACE_RLU_LIGHT_CLASSIFICATION_FLAG_MASK |
    PATH_TRACE_RLU_STABILITY_REASON_FLAG_MASK;

struct PathTraceUnifiedLightRecord
{
    float positionAndRadius[4] = {};
    float normalAndArea[4] = {};
    float radianceAndLuminance[4] = {};
    float uvOrDoomParams[4] = {};

    uint32_t type = PATH_TRACE_UNIFIED_LIGHT_TYPE_INVALID;
    uint32_t sourceIndex = PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX;
    uint32_t flags = 0;
    uint32_t materialOrLightId = 0;

    uint32_t instanceId = 0;
    uint32_t primitiveIndex = 0;
    uint32_t identityA = 0;
    uint32_t identityB = 0;

    float sourcePdf = 0.0f;
    float sourceWeight = 0.0f;
    uint32_t previousIndex = PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX;
    uint32_t padding0 = 0;
};
static_assert(sizeof(PathTraceUnifiedLightRecord) == 112, "PathTraceUnifiedLightRecord must match HLSL layout");
static_assert((sizeof(PathTraceUnifiedLightRecord) % 16) == 0, "PathTraceUnifiedLightRecord must stay 16-byte aligned for HLSL StructuredBuffer reads");

struct PathTraceUnifiedLightBuild
{
    std::vector<PathTraceUnifiedLightRecord> currentLights;
    std::vector<PathTraceUnifiedLightRecord> previousLights;
    std::vector<uint32_t> currentToPreviousRemap;
    uint32_t currentEmissiveLightCount = 0;
    uint32_t previousEmissiveLightCount = 0;
    uint32_t currentAnalyticLightCount = 0;
    uint32_t previousAnalyticLightCount = 0;
};

static constexpr uint32_t PATH_TRACE_UNIFIED_EMISSIVE_LOOKUP_MAX_PROBES = 16u;

struct PathTraceUnifiedEmissiveLookupEntry
{
    uint32_t instanceId = 0;
    uint32_t primitiveIndex = 0;
    uint32_t denseLightIndex = PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX;
    uint32_t occupied = 0;
    // Exact conditional identity probability from the current emissive CDF.
    // The shader applies the typed-domain trial fraction separately.
    float conditionalIdentityPdf = 0.0f;
};
static_assert(sizeof(PathTraceUnifiedEmissiveLookupEntry) == 20,
    "PathTraceUnifiedEmissiveLookupEntry must match Slang scalar layout");

struct PathTraceUnifiedEmissiveLookupBuild
{
    std::vector<PathTraceUnifiedEmissiveLookupEntry> entries;
    uint64_t signature = 0;
    bool exact = false;
};

PathTraceUnifiedLightBuild BuildPathTraceUnifiedLights(
    const std::vector<PathTraceSmokeEmissiveTriangle>& currentEmissiveTriangles,
    const std::vector<PathTraceSmokeEmissiveTriangle>& previousEmissiveTriangles,
    const std::vector<PathTraceEmissiveLightRemap>& emissiveRemap,
    const std::vector<PathTraceDoomAnalyticLightCandidate>& currentAnalyticLights,
    const std::vector<PathTraceDoomAnalyticLightCandidate>& previousAnalyticLights,
    const std::vector<PathTraceDoomAnalyticLightCandidateIdentity>& currentAnalyticIdentities,
    const std::vector<PathTraceDoomAnalyticLightCandidateIdentity>& previousAnalyticIdentities,
    const std::vector<PathTraceDoomAnalyticLightRemap>& analyticRemap,
    float analyticStateCompatibilityTolerance);

PathTraceUnifiedEmissiveLookupBuild BuildPathTraceUnifiedEmissiveLookup(
    const std::vector<PathTraceUnifiedLightRecord>& currentLights,
    const std::vector<PathTraceEmissiveDistributionEntry>& emissiveDistribution,
    uint32_t emissiveRangeStart,
    uint32_t emissiveRangeCount);
