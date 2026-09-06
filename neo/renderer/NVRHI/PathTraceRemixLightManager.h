#pragma once

// CPU-owned RTX Remix-shaped light manager contract.
//
// This owns the current/previous light payload domain, bidirectional remaps,
// light type ranges, sample-count metadata, and signatures. It does not own GPU
// buffers, RAB routing, RTXDI dispatch, or reservoir clear policy.

#include "PathTraceRemixFramePrepare.h"
#include "PathTraceUnifiedLight.h"

#include <array>
#include <cstdint>
#include <vector>

struct PathTraceDoomAnalyticLightCandidate;
struct PathTraceDoomAnalyticLightCandidateIdentity;
struct PathTraceDoomAnalyticLightRemap;
struct PathTraceEmissiveLightRemap;
struct PathTraceSmokeEmissiveTriangle;

static constexpr uint32_t PATH_TRACE_REMIX_LIGHT_INVALID_INDEX = 0xffffffffu;

enum PathTraceRemixLightType : uint32_t
{
    PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE = 0u,
    PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC = 1u,
    PATH_TRACE_REMIX_LIGHT_TYPE_COUNT = 2u
};

struct PathTraceRemixLightRange
{
    uint32_t firstLightIndex = 0;
    uint32_t lightCount = 0;
    uint32_t sampleCount = 0;
    uint32_t padding0 = 0;
};

struct PathTraceRemixLightEventSample
{
    uint32_t index = PATH_TRACE_REMIX_LIGHT_INVALID_INDEX;
    uint32_t type = PATH_TRACE_UNIFIED_LIGHT_TYPE_INVALID;
    uint32_t sourceIndex = PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX;
    uint32_t materialOrLightId = PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX;
    uint32_t identityA = PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX;
    uint32_t identityB = PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX;
    uint32_t flags = 0;
    uint32_t padding0 = 0;
    float positionAndRadius[4] = {};
    float radianceAndLuminance[4] = {};
};

struct PathTraceRemixLightManagerStats
{
    uint64_t frameIndex = 0;
    uint32_t enabled = 0;
    uint32_t domain = 0;
    uint32_t strictRemixMapping = 1;
    uint32_t resetReasonFlags = 0;
    uint32_t currentLightCount = 0;
    uint32_t previousLightCount = 0;
    uint32_t currentToPreviousCount = 0;
    uint32_t previousToCurrentCount = 0;
    uint32_t currentMappedCount = 0;
    uint32_t currentInvalidCount = 0;
    uint32_t currentOnlyCount = 0;
    uint32_t previousMappedCount = 0;
    uint32_t previousInvalidCount = 0;
    uint32_t previousOnlyCount = 0;
    uint32_t invalidDuplicateIdentityCount = 0;
    uint32_t emissiveRangeOffset = 0;
    uint32_t emissiveRangeCount = 0;
    uint32_t doomAnalyticRangeOffset = 0;
    uint32_t doomAnalyticRangeCount = 0;
    uint32_t emissiveSampleCount = 0;
    uint32_t doomAnalyticSampleCount = 0;
    uint32_t totalSampleCount = 0;
    uint32_t nonEmptyRangeCount = 0;
    uint32_t payloadOnlyChange = 0;
    uint32_t mappedPayloadChangedCount = 0;
    uint32_t currentMappedByType[PATH_TRACE_REMIX_LIGHT_TYPE_COUNT] = {};
    uint32_t currentOnlyByType[PATH_TRACE_REMIX_LIGHT_TYPE_COUNT] = {};
    uint32_t previousMappedByType[PATH_TRACE_REMIX_LIGHT_TYPE_COUNT] = {};
    uint32_t previousOnlyByType[PATH_TRACE_REMIX_LIGHT_TYPE_COUNT] = {};
    uint32_t mappedPayloadChangedByType[PATH_TRACE_REMIX_LIGHT_TYPE_COUNT] = {};
    uint32_t duplicateIdentityByType[PATH_TRACE_REMIX_LIGHT_TYPE_COUNT] = {};
    uint32_t doomAnalyticCurrentSampleableCount = 0;
    uint32_t doomAnalyticStableCacheableCount = 0;
    uint32_t doomAnalyticUnstableDynamicCount = 0;
    uint32_t doomAnalyticRejectNoRemapCount = 0;
    uint32_t doomAnalyticRejectPayloadChangedCount = 0;
    uint32_t doomAnalyticRejectUnprovenContinuityCount = 0;
    uint32_t doomAnalyticRejectUnknownIdentityCount = 0;
    uint32_t doomAnalyticRejectDuplicateIdentityCount = 0;
    uint32_t doomAnalyticRejectPortalDisconnectedCount = 0;
    uint32_t doomAnalyticRejectOutOfSelectedAreaCount = 0;
    uint32_t structuralSignatureChanged = 0;
    uint32_t mappingSignatureChanged = 0;
    uint32_t payloadSignatureChanged = 0;
    uint32_t oldSmokeReservoirSignatureConsulted = 0;
    uint32_t resourceAllocationCount = 0;
    uint32_t shaderRouteCount = 0;
    uint32_t firstFailingContract = 0;
    uint64_t structuralSignature = 0;
    uint64_t mappingSignature = 0;
    uint64_t payloadSignature = 0;
    PathTraceRemixLightEventSample firstPayloadChangedCurrent;
    PathTraceRemixLightEventSample firstPayloadChangedPrevious;
    PathTraceRemixLightEventSample firstCurrentOnly;
    PathTraceRemixLightEventSample firstPreviousOnly;
};

struct PathTraceRemixLightManagerPrepareDesc
{
    const PathTraceRemixFramePrepareObservationPackage* framePackage = nullptr;
    const std::vector<PathTraceSmokeEmissiveTriangle>* currentEmissiveTriangles = nullptr;
    const std::vector<PathTraceSmokeEmissiveTriangle>* previousEmissiveTriangles = nullptr;
    const std::vector<PathTraceEmissiveLightRemap>* emissiveRemap = nullptr;
    const std::vector<PathTraceDoomAnalyticLightCandidate>* currentAnalyticLights = nullptr;
    const std::vector<PathTraceDoomAnalyticLightCandidate>* previousAnalyticLights = nullptr;
    const std::vector<PathTraceDoomAnalyticLightCandidateIdentity>* currentAnalyticIdentities = nullptr;
    const std::vector<PathTraceDoomAnalyticLightCandidateIdentity>* previousAnalyticIdentities = nullptr;
    const std::vector<PathTraceDoomAnalyticLightRemap>* analyticRemap = nullptr;
    uint32_t emissiveSampleCount = 0;
    uint32_t doomAnalyticSampleCount = 0;
    float analyticStateCompatibilityTolerance = 0.0f;
    uint32_t domain = 2;
    bool strictRemixMapping = true;
    bool lightUniverseEnabled = false;
};

struct PathTraceRemixLightManagerPrepareResult
{
    std::vector<PathTraceUnifiedLightRecord> currentLightPayloads;
    std::vector<PathTraceUnifiedLightRecord> previousLightPayloads;
    std::vector<uint32_t> currentToPreviousMap;
    std::vector<uint32_t> previousToCurrentMap;
    std::array<PathTraceRemixLightRange, PATH_TRACE_REMIX_LIGHT_TYPE_COUNT> lightRanges;
    PathTraceRemixLightManagerStats stats;
    uint64_t lastStructuralSignature = 0;
    uint64_t lastMappingSignature = 0;
    uint64_t lastPayloadSignature = 0;
    bool haveLastSignatures = false;
    bool lightUniverseHistoryValid = false;
    bool lastPrepareWasLightUniverse = false;
};

class PathTraceRemixLightManager
{
public:
    void Clear();
    void PrepareDisabled(
        const PathTraceRemixFramePrepareObservationPackage& framePackage,
        uint32_t domain,
        bool strictRemixMapping);
    void PrepareSceneData(
        const PathTraceRemixFramePrepareObservationPackage& framePackage,
        const std::vector<PathTraceSmokeEmissiveTriangle>& currentEmissiveTriangles,
        const std::vector<PathTraceSmokeEmissiveTriangle>& previousEmissiveTriangles,
        const std::vector<PathTraceEmissiveLightRemap>& emissiveRemap,
        const std::vector<PathTraceDoomAnalyticLightCandidate>& currentAnalyticLights,
        const std::vector<PathTraceDoomAnalyticLightCandidate>& previousAnalyticLights,
        const std::vector<PathTraceDoomAnalyticLightCandidateIdentity>& currentAnalyticIdentities,
        const std::vector<PathTraceDoomAnalyticLightCandidateIdentity>& previousAnalyticIdentities,
        const std::vector<PathTraceDoomAnalyticLightRemap>& analyticRemap,
        uint32_t emissiveSampleCount,
        uint32_t doomAnalyticSampleCount,
        float analyticStateCompatibilityTolerance,
        uint32_t domain,
        bool strictRemixMapping,
        bool lightUniverseEnabled);
    void PrepareSceneData(
        const PathTraceRemixLightManagerPrepareDesc& desc);
    PathTraceRemixLightManagerPrepareResult BuildPrepareResult(
        const PathTraceRemixLightManagerPrepareDesc& desc) const;
    void ApplyPrepareResult(
        PathTraceRemixLightManagerPrepareResult&& result);

    const std::vector<PathTraceUnifiedLightRecord>& GetCurrentLightPayloads() const;
    const std::vector<PathTraceUnifiedLightRecord>& GetPreviousLightPayloads() const;
    const std::vector<uint32_t>& GetCurrentToPreviousMap() const;
    const std::vector<uint32_t>& GetPreviousToCurrentMap() const;
    const PathTraceRemixLightRange* GetLightRanges() const;
    const PathTraceRemixLightManagerStats& GetStats() const;

private:
    struct IdentityIndexes;
    void RebuildPreviousToCurrentMap();
    uint32_t RebuildCurrentToPreviousMapByStableIdentity(const IdentityIndexes& identities);
    void RebuildAnalyticStabilityClassification(const IdentityIndexes& identities);
    void SortCurrentDoomAnalyticRangeByCacheability(
        uint32_t currentEmissiveCount,
        uint32_t currentAnalyticCount);
    void RebuildLightRanges(
        uint32_t currentEmissiveCount,
        uint32_t currentAnalyticCount,
        uint32_t emissiveSampleCount,
        uint32_t doomAnalyticSampleCount);
    void RebuildStats(const PathTraceRemixFramePrepareObservationPackage& framePackage,
        const IdentityIndexes& identities);
    void RebuildSignatures();

    std::vector<PathTraceUnifiedLightRecord> m_currentLightPayloads;
    std::vector<PathTraceUnifiedLightRecord> m_previousLightPayloads;
    std::vector<uint32_t> m_currentToPreviousMap;
    std::vector<uint32_t> m_previousToCurrentMap;
    PathTraceRemixLightRange m_lightRanges[PATH_TRACE_REMIX_LIGHT_TYPE_COUNT];
    PathTraceRemixLightManagerStats m_stats;
    uint64_t m_lastStructuralSignature = 0;
    uint64_t m_lastMappingSignature = 0;
    uint64_t m_lastPayloadSignature = 0;
    bool m_haveLastSignatures = false;
    bool m_lightUniverseHistoryValid = false;
    bool m_lastPrepareWasLightUniverse = false;
};
