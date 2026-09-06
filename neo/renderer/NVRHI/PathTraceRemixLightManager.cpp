#include "precompiled.h"
#pragma hdrstop

#include "PathTraceRemixLightManager.h"
#include "PathTraceDoomLights.h"
#include "PathTraceEmissiveCandidates.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace {

enum PathTraceRemixLightContractStatus : uint32_t
{
    PATH_TRACE_REMIX_LIGHT_CONTRACT_OK = 0u,
    PATH_TRACE_REMIX_LIGHT_CONTRACT_DISABLED = 1u,
    PATH_TRACE_REMIX_LIGHT_CONTRACT_NO_CURRENT_DOMAIN = 2u,
    PATH_TRACE_REMIX_LIGHT_CONTRACT_MAP_SIZE_MISMATCH = 3u,
    PATH_TRACE_REMIX_LIGHT_CONTRACT_DUPLICATE_IDENTITY = 4u
};

struct RemixUnifiedIdentityKey
{
    uint32_t type = 0;
    uint32_t identityA = 0;
    uint32_t identityB = 0;
    uint32_t materialOrLightId = 0;
};

bool operator<(const RemixUnifiedIdentityKey& a, const RemixUnifiedIdentityKey& b)
{
    if (a.type != b.type)
    {
        return a.type < b.type;
    }
    if (a.identityA != b.identityA)
    {
        return a.identityA < b.identityA;
    }
    if (a.identityB != b.identityB)
    {
        return a.identityB < b.identityB;
    }
    return a.materialOrLightId < b.materialOrLightId;
}

bool operator==(const RemixUnifiedIdentityKey& a, const RemixUnifiedIdentityKey& b)
{
    return a.type == b.type &&
        a.identityA == b.identityA &&
        a.identityB == b.identityB &&
        a.materialOrLightId == b.materialOrLightId;
}

struct RemixUnifiedIdentityKeyHash
{
    size_t operator()(const RemixUnifiedIdentityKey& key) const
    {
        uint64_t hash = 1469598103934665603ull;
        const auto mix = [&hash](uint32_t value) {
            hash ^= static_cast<uint64_t>(value);
            hash *= 1099511628211ull;
        };
        mix(key.type);
        mix(key.identityA);
        mix(key.identityB);
        mix(key.materialOrLightId);
        return static_cast<size_t>(hash ^ (hash >> 32));
    }
};

using RemixUnifiedIdentityIndexMap = std::unordered_map<RemixUnifiedIdentityKey, int, RemixUnifiedIdentityKeyHash>;

bool RemixUnifiedIdentityKeyValid(const RemixUnifiedIdentityKey& key)
{
    if (key.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_INVALID)
    {
        return false;
    }
    if (key.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
    {
        return key.identityA != PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX &&
            key.materialOrLightId != PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX;
    }
    return key.identityA != 0u || key.identityB != 0u || key.materialOrLightId != 0u;
}

RemixUnifiedIdentityKey MakeRemixUnifiedIdentityKey(const PathTraceUnifiedLightRecord& record)
{
    RemixUnifiedIdentityKey key;
    key.type = record.type;
    key.identityA = record.identityA;
    key.identityB = record.identityB;
    key.materialOrLightId = record.materialOrLightId;
    return key;
}

void SortUnifiedLightPayloadRangeByStableIdentity(
    std::vector<PathTraceUnifiedLightRecord>& records,
    uint32_t rangeBegin,
    uint32_t rangeCount)
{
    if (rangeCount <= 1u || rangeBegin >= records.size())
    {
        return;
    }

    const size_t begin = static_cast<size_t>(rangeBegin);
    const size_t end = std::min(records.size(), begin + static_cast<size_t>(rangeCount));
    std::stable_sort(
        records.begin() + begin,
        records.begin() + end,
        [](const PathTraceUnifiedLightRecord& a, const PathTraceUnifiedLightRecord& b) {
            const RemixUnifiedIdentityKey keyA = MakeRemixUnifiedIdentityKey(a);
            const RemixUnifiedIdentityKey keyB = MakeRemixUnifiedIdentityKey(b);
            if (!(keyA == keyB))
            {
                return keyA < keyB;
            }
            if (a.sourceIndex != b.sourceIndex)
            {
                return a.sourceIndex < b.sourceIndex;
            }
            if (a.instanceId != b.instanceId)
            {
                return a.instanceId < b.instanceId;
            }
            if (a.primitiveIndex != b.primitiveIndex)
            {
                return a.primitiveIndex < b.primitiveIndex;
            }
            return a.flags < b.flags;
        });
}

uint32_t RemixLightTypeIndexFromUnifiedType(uint32_t unifiedType);

uint32_t BuildUniqueUnifiedIdentityIndex(
    const std::vector<PathTraceUnifiedLightRecord>& records,
    RemixUnifiedIdentityIndexMap& identityToIndex,
    uint32_t duplicateCounts[PATH_TRACE_REMIX_LIGHT_TYPE_COUNT])
{
    std::fill(duplicateCounts, duplicateCounts + PATH_TRACE_REMIX_LIGHT_TYPE_COUNT, 0u);
    identityToIndex.clear();
    identityToIndex.reserve(records.size());
    uint32_t duplicateCount = 0;
    for (int index = 0; index < static_cast<int>(records.size()); ++index)
    {
        const RemixUnifiedIdentityKey key = MakeRemixUnifiedIdentityKey(records[index]);
        if (!RemixUnifiedIdentityKeyValid(key))
        {
            continue;
        }

        const auto insertResult = identityToIndex.emplace(key, index);
        if (!insertResult.second)
        {
            insertResult.first->second = -1;
            ++duplicateCount;
            const uint32_t typeIndex = RemixLightTypeIndexFromUnifiedType(key.type);
            if (typeIndex < PATH_TRACE_REMIX_LIGHT_TYPE_COUNT) ++duplicateCounts[typeIndex];
        }
    }
    return duplicateCount;
}

uint64_t RemixHashBytes(uint64_t hash, const void* data, size_t byteCount)
{
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < byteCount; ++i)
    {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

template< typename T >
uint64_t RemixHashValue(uint64_t hash, const T& value)
{
    return RemixHashBytes(hash, &value, sizeof(T));
}

template< typename T >
uint64_t RemixHashVector(uint64_t hash, const std::vector<T>& values)
{
    const uint64_t count = static_cast<uint64_t>(values.size());
    hash = RemixHashValue(hash, count);
    if (!values.empty())
    {
        hash = RemixHashBytes(hash, values.data(), values.size() * sizeof(T));
    }
    return hash;
}

bool RemixLightMapIndexValid(uint32_t index)
{
    return index != PATH_TRACE_REMIX_LIGHT_INVALID_INDEX;
}

uint32_t RemixLightTypeIndexFromUnifiedType(uint32_t unifiedType)
{
    switch (unifiedType)
    {
    case PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE:
        return PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE;
    case PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC:
        return PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC;
    default:
        return PATH_TRACE_REMIX_LIGHT_TYPE_COUNT;
    }
}

PathTraceRemixLightEventSample MakeRemixLightEventSample(uint32_t index, const PathTraceUnifiedLightRecord& record)
{
    PathTraceRemixLightEventSample sample;
    sample.index = index;
    sample.type = record.type;
    sample.sourceIndex = record.sourceIndex;
    sample.materialOrLightId = record.materialOrLightId;
    sample.identityA = record.identityA;
    sample.identityB = record.identityB;
    sample.flags = record.flags;
    std::memcpy(sample.positionAndRadius, record.positionAndRadius, sizeof(sample.positionAndRadius));
    std::memcpy(sample.radianceAndLuminance, record.radianceAndLuminance, sizeof(sample.radianceAndLuminance));
    return sample;
}

bool RemixMappedPayloadChanged(
    const PathTraceUnifiedLightRecord& current,
    const PathTraceUnifiedLightRecord& previous)
{
    const uint32_t currentPayloadFlags = current.flags & ~PATH_TRACE_RLU_STABILITY_FLAG_MASK;
    const uint32_t previousPayloadFlags = previous.flags & ~PATH_TRACE_RLU_STABILITY_FLAG_MASK;
    return std::memcmp(current.positionAndRadius, previous.positionAndRadius, sizeof(current.positionAndRadius)) != 0 ||
        std::memcmp(current.normalAndArea, previous.normalAndArea, sizeof(current.normalAndArea)) != 0 ||
        std::memcmp(current.radianceAndLuminance, previous.radianceAndLuminance, sizeof(current.radianceAndLuminance)) != 0 ||
        std::memcmp(current.uvOrDoomParams, previous.uvOrDoomParams, sizeof(current.uvOrDoomParams)) != 0 ||
        currentPayloadFlags != previousPayloadFlags ||
        current.sourcePdf != previous.sourcePdf ||
        current.sourceWeight != previous.sourceWeight;
}

bool RemixDoomAnalyticCurrentSampleable(const PathTraceUnifiedLightRecord& record)
{
    return record.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC &&
        (record.flags & PATH_TRACE_DOOM_ANALYTIC_IDENTITY_VALID) != 0u &&
        (record.flags & PATH_TRACE_DOOM_ANALYTIC_IDENTITY_SAMPLEABLE) != 0u &&
        record.sourceIndex != PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX &&
        record.positionAndRadius[3] > 0.0f &&
        record.uvOrDoomParams[0] > 0.0f &&
        record.sourceWeight > 0.0f;
}

bool RemixDoomAnalyticIdentityMappable(const PathTraceDoomAnalyticLightCandidateIdentity& identity)
{
    return identity.universeIndex != PATH_TRACE_DOOM_ANALYTIC_LIGHT_INVALID_INDEX &&
        identity.remapIndex != PATH_TRACE_DOOM_ANALYTIC_LIGHT_INVALID_INDEX &&
        (identity.flags & PATH_TRACE_DOOM_ANALYTIC_IDENTITY_VALID) != 0u &&
        (identity.flags & PATH_TRACE_DOOM_ANALYTIC_IDENTITY_SAMPLEABLE) != 0u;
}

bool RemixDoomAnalyticRemapValid(const PathTraceDoomAnalyticLightRemap& remap)
{
    return (remap.flags & PATH_TRACE_DOOM_ANALYTIC_IDENTITY_REMAP_VALID) != 0u;
}

uint32_t RemixRrxDiRequestedInitialSampleBudget(uint32_t emissiveSampleCount, uint32_t doomAnalyticSampleCount)
{
    const uint64_t requestedTotal =
        static_cast<uint64_t>(emissiveSampleCount) + static_cast<uint64_t>(doomAnalyticSampleCount);
    if (requestedTotal == 0)
    {
        return 0;
    }

    return static_cast<uint32_t>(std::min<uint64_t>(requestedTotal, 32ull));
}

uint32_t RemixRrxDiBoundedRangeSampleCount(uint32_t rangeCount, uint32_t requestedRangeSamples, uint32_t requestedAllSamples, uint32_t boundedTotalSamples)
{
    if (rangeCount == 0 || requestedRangeSamples == 0 || requestedAllSamples == 0 || boundedTotalSamples == 0)
    {
        return 0;
    }

    const uint64_t roundedSamples = (static_cast<uint64_t>(requestedRangeSamples) * static_cast<uint64_t>(boundedTotalSamples) +
        static_cast<uint64_t>(requestedAllSamples / 2u)) / static_cast<uint64_t>(requestedAllSamples);
    const uint32_t nonEmptyRangeSamples = static_cast<uint32_t>(std::max<uint64_t>(1ull, roundedSamples));
    return nonEmptyRangeSamples;
}

}

// Private to one PrepareSceneData invocation. Neither payload flags nor remap
// indices change identity keys; sorting current records does change their indices.
struct PathTraceRemixLightManager::IdentityIndexes
{
    RemixUnifiedIdentityIndexMap current, previous;
    uint32_t currentDuplicates = 0, previousDuplicates = 0;
    uint32_t currentDuplicatesByType[PATH_TRACE_REMIX_LIGHT_TYPE_COUNT] = {};
    uint32_t previousDuplicatesByType[PATH_TRACE_REMIX_LIGHT_TYPE_COUNT] = {};
    void BuildCurrent(const std::vector<PathTraceUnifiedLightRecord>& records) {
        currentDuplicates = BuildUniqueUnifiedIdentityIndex(records, current, currentDuplicatesByType);
    }
    void BuildPrevious(const std::vector<PathTraceUnifiedLightRecord>& records) {
        previousDuplicates = BuildUniqueUnifiedIdentityIndex(records, previous, previousDuplicatesByType);
    }
};

void PathTraceRemixLightManager::Clear()
{
    m_currentLightPayloads.clear();
    m_previousLightPayloads.clear();
    m_currentToPreviousMap.clear();
    m_previousToCurrentMap.clear();
    for (PathTraceRemixLightRange& range : m_lightRanges)
    {
        range = PathTraceRemixLightRange();
    }
    m_stats = PathTraceRemixLightManagerStats();
    m_lastStructuralSignature = 0;
    m_lastMappingSignature = 0;
    m_lastPayloadSignature = 0;
    m_haveLastSignatures = false;
    m_lightUniverseHistoryValid = false;
    m_lastPrepareWasLightUniverse = false;
}

void PathTraceRemixLightManager::PrepareDisabled(
    const PathTraceRemixFramePrepareObservationPackage& framePackage,
    uint32_t domain,
    bool strictRemixMapping)
{
    Clear();
    m_stats.frameIndex = framePackage.frameIndex;
    m_stats.enabled = 0;
    m_stats.domain = domain;
    m_stats.strictRemixMapping = strictRemixMapping ? 1u : 0u;
    m_stats.resetReasonFlags = framePackage.resetReasonFlags;
    m_stats.firstFailingContract = PATH_TRACE_REMIX_LIGHT_CONTRACT_DISABLED;
}

void PathTraceRemixLightManager::PrepareSceneData(
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
    bool lightUniverseEnabled)
{
    std::vector<PathTraceUnifiedLightRecord> lightUniversePreviousPayloads;
    {
        OPTICK_EVENT("PT Remix Light Preserve Previous");
        if (lightUniverseEnabled && m_lastPrepareWasLightUniverse && m_lightUniverseHistoryValid)
        {
            lightUniversePreviousPayloads = m_currentLightPayloads;
        }
    }

    const std::vector<PathTraceSmokeEmissiveTriangle> emptyEmissiveTriangles;
    const std::vector<PathTraceEmissiveLightRemap> emptyEmissiveRemap;
    const std::vector<PathTraceDoomAnalyticLightCandidate> emptyAnalyticLights;
    const std::vector<PathTraceDoomAnalyticLightCandidateIdentity> emptyAnalyticIdentities;
    const std::vector<PathTraceDoomAnalyticLightRemap> emptyAnalyticRemap;

    PathTraceUnifiedLightBuild build;
    {
        OPTICK_EVENT("PT Remix Light Build Unified");
        build = lightUniverseEnabled
            ? BuildPathTraceUnifiedLights(
                currentEmissiveTriangles,
                emptyEmissiveTriangles,
                emptyEmissiveRemap,
                currentAnalyticLights,
                emptyAnalyticLights,
                currentAnalyticIdentities,
                emptyAnalyticIdentities,
                emptyAnalyticRemap,
                analyticStateCompatibilityTolerance)
            : BuildPathTraceUnifiedLights(
                currentEmissiveTriangles,
                previousEmissiveTriangles,
                emissiveRemap,
                currentAnalyticLights,
                previousAnalyticLights,
                currentAnalyticIdentities,
                previousAnalyticIdentities,
                analyticRemap,
                analyticStateCompatibilityTolerance);
    }

    m_currentLightPayloads.swap(build.currentLights);
    if (lightUniverseEnabled)
    {
        const uint32_t currentEmissiveCount = build.currentEmissiveLightCount;
        const uint32_t currentAnalyticCount = build.currentAnalyticLightCount;
        // Keep emissives in source-index order. The clean DI weighted emissive CDF
        // returns SmokeEmissiveTriangles source indices and maps them directly into
        // the RLU emissive range; sorting this range breaks that sampling contract.
        OPTICK_EVENT("PT Remix Light Initial Sort");
        SortUnifiedLightPayloadRangeByStableIdentity(m_currentLightPayloads, currentEmissiveCount, currentAnalyticCount);
    }
    if (lightUniverseEnabled)
    {
        m_previousLightPayloads.swap(lightUniversePreviousPayloads);
    }
    else
    {
        m_previousLightPayloads.swap(build.previousLights);
    }
    m_currentToPreviousMap.swap(build.currentToPreviousRemap);
    IdentityIndexes identities;
    {
        OPTICK_EVENT("PT Remix Light Identity Inputs");
        identities.BuildCurrent(m_currentLightPayloads);
        identities.BuildPrevious(m_previousLightPayloads);
    }
    OPTICK_TAG("lightManagerIdentityCurrentBuilds", lightUniverseEnabled ? 2u : 1u);
    OPTICK_TAG("lightManagerIdentityPreviousBuilds", 1u);
    OPTICK_TAG("lightManagerIdentityRedundantBuildsSkipped", lightUniverseEnabled ? 9u : 4u);
    OPTICK_TAG("lightManagerIdentityCurrentRows", static_cast<uint32_t>(m_currentLightPayloads.size()));
    OPTICK_TAG("lightManagerIdentityPreviousRows", static_cast<uint32_t>(m_previousLightPayloads.size()));
    uint32_t identityDuplicateCount = 0;
    if (lightUniverseEnabled)
    {
        OPTICK_EVENT("PT Remix Light Stable Remap");
        identityDuplicateCount = RebuildCurrentToPreviousMapByStableIdentity(identities);
    }
    else
    {
        const uint32_t currentEmissiveCount = build.currentEmissiveLightCount;
        const uint32_t previousEmissiveCount = build.previousEmissiveLightCount;
        for (uint32_t analyticIndex = 0; analyticIndex < currentAnalyticIdentities.size(); ++analyticIndex)
        {
            const uint32_t currentUnifiedIndex = currentEmissiveCount + analyticIndex;
            if (currentUnifiedIndex >= m_currentToPreviousMap.size())
            {
                continue;
            }

            const PathTraceDoomAnalyticLightCandidateIdentity& identity = currentAnalyticIdentities[analyticIndex];
            if (!RemixDoomAnalyticIdentityMappable(identity) || identity.remapIndex >= analyticRemap.size())
            {
                continue;
            }

            const PathTraceDoomAnalyticLightRemap& remap = analyticRemap[identity.remapIndex];
            if (!RemixDoomAnalyticRemapValid(remap) || remap.currentToPreviousCandidateIndex < 0)
            {
                continue;
            }

            const uint32_t previousAnalyticIndex = static_cast<uint32_t>(remap.currentToPreviousCandidateIndex);
            const uint32_t previousUnifiedIndex = previousEmissiveCount + previousAnalyticIndex;
            if (previousAnalyticIndex < previousAnalyticLights.size() && previousUnifiedIndex < m_previousLightPayloads.size())
            {
                m_currentToPreviousMap[currentUnifiedIndex] = previousUnifiedIndex;
                m_currentLightPayloads[currentUnifiedIndex].previousIndex = previousUnifiedIndex;
            }
        }
    }
    {
        OPTICK_EVENT("PT Remix Light Previous Map");
        RebuildPreviousToCurrentMap();
    }
    {
        OPTICK_EVENT("PT Remix Light Stability");
        RebuildAnalyticStabilityClassification(identities);
    }
    if (lightUniverseEnabled)
    {
        const uint32_t currentEmissiveCount = build.currentEmissiveLightCount;
        const uint32_t currentAnalyticCount = build.currentAnalyticLightCount;
        {
            OPTICK_EVENT("PT Remix Light Cacheability Sort");
            SortCurrentDoomAnalyticRangeByCacheability(currentEmissiveCount, currentAnalyticCount);
        }
        {
            OPTICK_EVENT("PT Remix Light Identity Reorder");
            identities.BuildCurrent(m_currentLightPayloads);
        }
        {
            OPTICK_EVENT("PT Remix Light Stable Remap After Sort");
            identityDuplicateCount = RebuildCurrentToPreviousMapByStableIdentity(identities);
        }
        {
            OPTICK_EVENT("PT Remix Light Previous Map After Sort");
            RebuildPreviousToCurrentMap();
        }
        {
            OPTICK_EVENT("PT Remix Light Stability After Sort");
            RebuildAnalyticStabilityClassification(identities);
        }
    }
    {
        OPTICK_EVENT("PT Remix Light Ranges");
        RebuildLightRanges(
            build.currentEmissiveLightCount,
            build.currentAnalyticLightCount,
            emissiveSampleCount,
            doomAnalyticSampleCount);
    }
    {
        OPTICK_EVENT("PT Remix Light Signatures");
        RebuildSignatures();
    }
    {
        OPTICK_EVENT("PT Remix Light Stats");
        RebuildStats(framePackage, identities);
    }
    m_stats.enabled = lightUniverseEnabled ? 1u : 0u;
    m_stats.domain = domain;
    m_stats.strictRemixMapping = strictRemixMapping ? 1u : 0u;
    if (lightUniverseEnabled)
    {
        m_stats.invalidDuplicateIdentityCount = identityDuplicateCount;
        if (identityDuplicateCount != 0u)
        {
            m_stats.firstFailingContract = PATH_TRACE_REMIX_LIGHT_CONTRACT_DUPLICATE_IDENTITY;
        }
    }
    if (!lightUniverseEnabled)
    {
        m_stats.firstFailingContract = PATH_TRACE_REMIX_LIGHT_CONTRACT_DISABLED;
    }
    m_lightUniverseHistoryValid = lightUniverseEnabled;
    m_lastPrepareWasLightUniverse = lightUniverseEnabled;
}

void PathTraceRemixLightManager::PrepareSceneData(
    const PathTraceRemixLightManagerPrepareDesc& desc)
{
    PrepareSceneData(
        *desc.framePackage,
        *desc.currentEmissiveTriangles,
        *desc.previousEmissiveTriangles,
        *desc.emissiveRemap,
        *desc.currentAnalyticLights,
        *desc.previousAnalyticLights,
        *desc.currentAnalyticIdentities,
        *desc.previousAnalyticIdentities,
        *desc.analyticRemap,
        desc.emissiveSampleCount,
        desc.doomAnalyticSampleCount,
        desc.analyticStateCompatibilityTolerance,
        desc.domain,
        desc.strictRemixMapping,
        desc.lightUniverseEnabled);
}

PathTraceRemixLightManagerPrepareResult PathTraceRemixLightManager::BuildPrepareResult(
    const PathTraceRemixLightManagerPrepareDesc& desc) const
{
    PathTraceRemixLightManager scratch = *this;
    scratch.PrepareSceneData(desc);

    PathTraceRemixLightManagerPrepareResult result;
    result.currentLightPayloads = std::move(scratch.m_currentLightPayloads);
    result.previousLightPayloads = std::move(scratch.m_previousLightPayloads);
    result.currentToPreviousMap = std::move(scratch.m_currentToPreviousMap);
    result.previousToCurrentMap = std::move(scratch.m_previousToCurrentMap);
    for (uint32_t rangeIndex = 0; rangeIndex < PATH_TRACE_REMIX_LIGHT_TYPE_COUNT; ++rangeIndex)
    {
        result.lightRanges[rangeIndex] = scratch.m_lightRanges[rangeIndex];
    }
    result.stats = scratch.m_stats;
    result.lastStructuralSignature = scratch.m_lastStructuralSignature;
    result.lastMappingSignature = scratch.m_lastMappingSignature;
    result.lastPayloadSignature = scratch.m_lastPayloadSignature;
    result.haveLastSignatures = scratch.m_haveLastSignatures;
    result.lightUniverseHistoryValid = scratch.m_lightUniverseHistoryValid;
    result.lastPrepareWasLightUniverse = scratch.m_lastPrepareWasLightUniverse;
    return result;
}

void PathTraceRemixLightManager::ApplyPrepareResult(
    PathTraceRemixLightManagerPrepareResult&& result)
{
    m_currentLightPayloads = std::move(result.currentLightPayloads);
    m_previousLightPayloads = std::move(result.previousLightPayloads);
    m_currentToPreviousMap = std::move(result.currentToPreviousMap);
    m_previousToCurrentMap = std::move(result.previousToCurrentMap);
    for (uint32_t rangeIndex = 0; rangeIndex < PATH_TRACE_REMIX_LIGHT_TYPE_COUNT; ++rangeIndex)
    {
        m_lightRanges[rangeIndex] = result.lightRanges[rangeIndex];
    }
    m_stats = result.stats;
    m_lastStructuralSignature = result.lastStructuralSignature;
    m_lastMappingSignature = result.lastMappingSignature;
    m_lastPayloadSignature = result.lastPayloadSignature;
    m_haveLastSignatures = result.haveLastSignatures;
    m_lightUniverseHistoryValid = result.lightUniverseHistoryValid;
    m_lastPrepareWasLightUniverse = result.lastPrepareWasLightUniverse;
}

const std::vector<PathTraceUnifiedLightRecord>& PathTraceRemixLightManager::GetCurrentLightPayloads() const
{
    return m_currentLightPayloads;
}

const std::vector<PathTraceUnifiedLightRecord>& PathTraceRemixLightManager::GetPreviousLightPayloads() const
{
    return m_previousLightPayloads;
}

const std::vector<uint32_t>& PathTraceRemixLightManager::GetCurrentToPreviousMap() const
{
    return m_currentToPreviousMap;
}

const std::vector<uint32_t>& PathTraceRemixLightManager::GetPreviousToCurrentMap() const
{
    return m_previousToCurrentMap;
}

const PathTraceRemixLightRange* PathTraceRemixLightManager::GetLightRanges() const
{
    return m_lightRanges;
}

const PathTraceRemixLightManagerStats& PathTraceRemixLightManager::GetStats() const
{
    return m_stats;
}

void PathTraceRemixLightManager::RebuildPreviousToCurrentMap()
{
    m_previousToCurrentMap.assign(m_previousLightPayloads.size(), PATH_TRACE_REMIX_LIGHT_INVALID_INDEX);
    for (uint32_t currentIndex = 0; currentIndex < m_currentToPreviousMap.size(); ++currentIndex)
    {
        const uint32_t previousIndex = m_currentToPreviousMap[currentIndex];
        if (previousIndex >= m_previousToCurrentMap.size())
        {
            continue;
        }
        if (!RemixLightMapIndexValid(m_previousToCurrentMap[previousIndex]))
        {
            m_previousToCurrentMap[previousIndex] = currentIndex;
        }
    }
}

uint32_t PathTraceRemixLightManager::RebuildCurrentToPreviousMapByStableIdentity(const IdentityIndexes& identities)
{
    m_currentToPreviousMap.assign(m_currentLightPayloads.size(), PATH_TRACE_REMIX_LIGHT_INVALID_INDEX);
    for (PathTraceUnifiedLightRecord& record : m_currentLightPayloads)
    {
        record.previousIndex = PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX;
    }

    const auto& currentIdentityToIndex = identities.current;
    const auto& previousIdentityToIndex = identities.previous;
    const uint32_t duplicateCount = identities.currentDuplicates + identities.previousDuplicates;

    for (uint32_t currentIndex = 0; currentIndex < m_currentLightPayloads.size(); ++currentIndex)
    {
        const RemixUnifiedIdentityKey key = MakeRemixUnifiedIdentityKey(m_currentLightPayloads[currentIndex]);
        if (!RemixUnifiedIdentityKeyValid(key))
        {
            continue;
        }

        const auto currentIt = currentIdentityToIndex.find(key);
        if (currentIt == currentIdentityToIndex.end() || currentIt->second != static_cast<int>(currentIndex))
        {
            continue;
        }

        const auto previousIt = previousIdentityToIndex.find(key);
        if (previousIt == previousIdentityToIndex.end() || previousIt->second < 0)
        {
            continue;
        }

        const uint32_t previousIndex = static_cast<uint32_t>(previousIt->second);
        if (previousIndex < m_previousLightPayloads.size())
        {
            m_currentToPreviousMap[currentIndex] = previousIndex;
            m_currentLightPayloads[currentIndex].previousIndex = previousIndex;
        }
    }
    return duplicateCount;
}

void PathTraceRemixLightManager::RebuildAnalyticStabilityClassification(const IdentityIndexes& identities)
{
    const auto& currentIdentityToIndex = identities.current;
    const auto& previousIdentityToIndex = identities.previous;

    for (PathTraceUnifiedLightRecord& record : m_currentLightPayloads)
    {
        record.flags &= ~PATH_TRACE_RLU_STABILITY_FLAG_MASK;
    }

    for (uint32_t currentIndex = 0; currentIndex < m_currentLightPayloads.size(); ++currentIndex)
    {
        PathTraceUnifiedLightRecord& current = m_currentLightPayloads[currentIndex];
        if (current.type != PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
        {
            continue;
        }

        uint32_t classificationFlags = 0u;
        uint32_t rejectionReasons = 0u;
        const bool currentSampleable = RemixDoomAnalyticCurrentSampleable(current);
        if (currentSampleable)
        {
            classificationFlags |= PATH_TRACE_RLU_LIGHT_FLAG_CURRENT_SAMPLEABLE;
        }

        const RemixUnifiedIdentityKey key = MakeRemixUnifiedIdentityKey(current);
        const bool identityValid = RemixUnifiedIdentityKeyValid(key);
        if (!identityValid)
        {
            rejectionReasons |= PATH_TRACE_RLU_STABILITY_REASON_UNKNOWN_IDENTITY |
                PATH_TRACE_RLU_STABILITY_REASON_UNPROVEN_CONTINUITY;
        }

        const auto currentIt = currentIdentityToIndex.find(key);
        const bool duplicateCurrentIdentity =
            identityValid &&
            (currentIt == currentIdentityToIndex.end() || currentIt->second != static_cast<int>(currentIndex));
        if (duplicateCurrentIdentity)
        {
            rejectionReasons |= PATH_TRACE_RLU_STABILITY_REASON_DUPLICATE_IDENTITY |
                PATH_TRACE_RLU_STABILITY_REASON_UNPROVEN_CONTINUITY;
        }

        const uint32_t previousIndex = currentIndex < m_currentToPreviousMap.size()
            ? m_currentToPreviousMap[currentIndex]
            : PATH_TRACE_REMIX_LIGHT_INVALID_INDEX;
        const bool previousIndexValid = previousIndex < m_previousLightPayloads.size();
        if (!previousIndexValid)
        {
            rejectionReasons |= PATH_TRACE_RLU_STABILITY_REASON_NO_REMAP |
                PATH_TRACE_RLU_STABILITY_REASON_UNPROVEN_CONTINUITY;
        }

        bool stableCacheable = false;
        if (currentSampleable && identityValid && !duplicateCurrentIdentity && previousIndexValid)
        {
            const PathTraceUnifiedLightRecord& previous = m_previousLightPayloads[previousIndex];
            const RemixUnifiedIdentityKey previousKey = MakeRemixUnifiedIdentityKey(previous);
            const auto previousIt = previousIdentityToIndex.find(previousKey);
            const bool duplicatePreviousIdentity =
                previousIt == previousIdentityToIndex.end() || previousIt->second != static_cast<int>(previousIndex);
            const bool previousBackMapValid =
                previousIndex < m_previousToCurrentMap.size() &&
                m_previousToCurrentMap[previousIndex] == currentIndex;
            const bool previousSampleable = RemixDoomAnalyticCurrentSampleable(previous);
            const bool payloadChanged = RemixMappedPayloadChanged(current, previous);

            if (previous.type != PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC ||
                !(previousKey == key) ||
                !previousBackMapValid)
            {
                rejectionReasons |= PATH_TRACE_RLU_STABILITY_REASON_NO_REMAP |
                    PATH_TRACE_RLU_STABILITY_REASON_UNPROVEN_CONTINUITY;
            }
            if (duplicatePreviousIdentity)
            {
                rejectionReasons |= PATH_TRACE_RLU_STABILITY_REASON_DUPLICATE_IDENTITY |
                    PATH_TRACE_RLU_STABILITY_REASON_UNPROVEN_CONTINUITY;
            }
            if (!previousSampleable)
            {
                rejectionReasons |= PATH_TRACE_RLU_STABILITY_REASON_UNPROVEN_CONTINUITY;
            }
            if (payloadChanged)
            {
                rejectionReasons |= PATH_TRACE_RLU_STABILITY_REASON_PAYLOAD_CHANGED;
            }

            stableCacheable =
                previous.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC &&
                previousKey == key &&
                previousBackMapValid &&
                !duplicatePreviousIdentity &&
                previousSampleable &&
                !payloadChanged &&
                rejectionReasons == 0u;
        }

        if (stableCacheable)
        {
            classificationFlags |= PATH_TRACE_RLU_LIGHT_FLAG_STABLE_CACHEABLE;
        }
        else if (currentSampleable)
        {
            classificationFlags |= PATH_TRACE_RLU_LIGHT_FLAG_UNSTABLE_DYNAMIC;
        }

        current.flags |= classificationFlags | rejectionReasons;
    }
}

void PathTraceRemixLightManager::SortCurrentDoomAnalyticRangeByCacheability(
    uint32_t currentEmissiveCount,
    uint32_t currentAnalyticCount)
{
    if (currentAnalyticCount <= 1u || currentEmissiveCount >= m_currentLightPayloads.size())
    {
        return;
    }

    const size_t begin = static_cast<size_t>(currentEmissiveCount);
    const size_t end = std::min(m_currentLightPayloads.size(), begin + static_cast<size_t>(currentAnalyticCount));
    std::stable_sort(
        m_currentLightPayloads.begin() + begin,
        m_currentLightPayloads.begin() + end,
        [](const PathTraceUnifiedLightRecord& a, const PathTraceUnifiedLightRecord& b) {
            const bool aStable = (a.flags & PATH_TRACE_RLU_LIGHT_FLAG_STABLE_CACHEABLE) != 0u;
            const bool bStable = (b.flags & PATH_TRACE_RLU_LIGHT_FLAG_STABLE_CACHEABLE) != 0u;
            if (aStable != bStable)
            {
                return aStable && !bStable;
            }

            const bool aCurrentSampleable = (a.flags & PATH_TRACE_RLU_LIGHT_FLAG_CURRENT_SAMPLEABLE) != 0u;
            const bool bCurrentSampleable = (b.flags & PATH_TRACE_RLU_LIGHT_FLAG_CURRENT_SAMPLEABLE) != 0u;
            if (aCurrentSampleable != bCurrentSampleable)
            {
                return aCurrentSampleable && !bCurrentSampleable;
            }

            const RemixUnifiedIdentityKey keyA = MakeRemixUnifiedIdentityKey(a);
            const RemixUnifiedIdentityKey keyB = MakeRemixUnifiedIdentityKey(b);
            if (!(keyA == keyB))
            {
                return keyA < keyB;
            }
            if (a.sourceIndex != b.sourceIndex)
            {
                return a.sourceIndex < b.sourceIndex;
            }
            if (a.instanceId != b.instanceId)
            {
                return a.instanceId < b.instanceId;
            }
            if (a.primitiveIndex != b.primitiveIndex)
            {
                return a.primitiveIndex < b.primitiveIndex;
            }
            return a.flags < b.flags;
        });
}

void PathTraceRemixLightManager::RebuildLightRanges(
    uint32_t currentEmissiveCount,
    uint32_t currentAnalyticCount,
    uint32_t emissiveSampleCount,
    uint32_t doomAnalyticSampleCount)
{
    for (PathTraceRemixLightRange& range : m_lightRanges)
    {
        range = PathTraceRemixLightRange();
    }

    m_lightRanges[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE].firstLightIndex = 0;
    m_lightRanges[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE].lightCount =
        std::min(currentEmissiveCount, static_cast<uint32_t>(m_currentLightPayloads.size()));

    m_lightRanges[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC].firstLightIndex =
        m_lightRanges[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE].lightCount;
    m_lightRanges[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC].lightCount =
        std::min(currentAnalyticCount, static_cast<uint32_t>(m_currentLightPayloads.size()) -
            m_lightRanges[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC].firstLightIndex);

    const uint32_t requestedTotal = RemixRrxDiRequestedInitialSampleBudget(
        emissiveSampleCount,
        doomAnalyticSampleCount);
    const uint32_t requestedAllSamples = emissiveSampleCount + doomAnalyticSampleCount;

    m_lightRanges[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE].sampleCount =
        RemixRrxDiBoundedRangeSampleCount(
            m_lightRanges[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE].lightCount,
            emissiveSampleCount,
            requestedAllSamples,
            requestedTotal);
    m_lightRanges[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC].sampleCount =
        RemixRrxDiBoundedRangeSampleCount(
            m_lightRanges[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC].lightCount,
            doomAnalyticSampleCount,
            requestedAllSamples,
            requestedTotal);
}

void PathTraceRemixLightManager::RebuildStats(const PathTraceRemixFramePrepareObservationPackage& framePackage,
    const IdentityIndexes& identities)
{
    const uint64_t structuralSignature = m_stats.structuralSignature;
    const uint64_t mappingSignature = m_stats.mappingSignature;
    const uint64_t payloadSignature = m_stats.payloadSignature;
    const uint32_t structuralChanged = !m_haveLastSignatures || structuralSignature != m_lastStructuralSignature ? 1u : 0u;
    const uint32_t mappingChanged = !m_haveLastSignatures || mappingSignature != m_lastMappingSignature ? 1u : 0u;
    const uint32_t payloadChanged = !m_haveLastSignatures || payloadSignature != m_lastPayloadSignature ? 1u : 0u;

    m_stats.frameIndex = framePackage.frameIndex;
    m_stats.currentLightCount = static_cast<uint32_t>(m_currentLightPayloads.size());
    m_stats.previousLightCount = static_cast<uint32_t>(m_previousLightPayloads.size());
    m_stats.currentToPreviousCount = static_cast<uint32_t>(m_currentToPreviousMap.size());
    m_stats.previousToCurrentCount = static_cast<uint32_t>(m_previousToCurrentMap.size());
    m_stats.currentMappedCount = 0;
    m_stats.currentInvalidCount = 0;
    m_stats.mappedPayloadChangedCount = 0;
    std::fill(m_stats.currentMappedByType, m_stats.currentMappedByType + PATH_TRACE_REMIX_LIGHT_TYPE_COUNT, 0u);
    std::fill(m_stats.currentOnlyByType, m_stats.currentOnlyByType + PATH_TRACE_REMIX_LIGHT_TYPE_COUNT, 0u);
    std::fill(m_stats.previousMappedByType, m_stats.previousMappedByType + PATH_TRACE_REMIX_LIGHT_TYPE_COUNT, 0u);
    std::fill(m_stats.previousOnlyByType, m_stats.previousOnlyByType + PATH_TRACE_REMIX_LIGHT_TYPE_COUNT, 0u);
    std::fill(m_stats.mappedPayloadChangedByType, m_stats.mappedPayloadChangedByType + PATH_TRACE_REMIX_LIGHT_TYPE_COUNT, 0u);
    std::fill(m_stats.duplicateIdentityByType, m_stats.duplicateIdentityByType + PATH_TRACE_REMIX_LIGHT_TYPE_COUNT, 0u);
    m_stats.doomAnalyticCurrentSampleableCount = 0;
    m_stats.doomAnalyticStableCacheableCount = 0;
    m_stats.doomAnalyticUnstableDynamicCount = 0;
    m_stats.doomAnalyticRejectNoRemapCount = 0;
    m_stats.doomAnalyticRejectPayloadChangedCount = 0;
    m_stats.doomAnalyticRejectUnprovenContinuityCount = 0;
    m_stats.doomAnalyticRejectUnknownIdentityCount = 0;
    m_stats.doomAnalyticRejectDuplicateIdentityCount = 0;
    m_stats.doomAnalyticRejectPortalDisconnectedCount = 0;
    m_stats.doomAnalyticRejectOutOfSelectedAreaCount = 0;
    m_stats.firstPayloadChangedCurrent = PathTraceRemixLightEventSample();
    m_stats.firstPayloadChangedPrevious = PathTraceRemixLightEventSample();
    m_stats.firstCurrentOnly = PathTraceRemixLightEventSample();
    m_stats.firstPreviousOnly = PathTraceRemixLightEventSample();
    for (uint32_t previousIndex : m_currentToPreviousMap)
    {
        const uint32_t currentIndex = m_stats.currentMappedCount + m_stats.currentInvalidCount;
        const uint32_t currentType = currentIndex < m_currentLightPayloads.size()
            ? RemixLightTypeIndexFromUnifiedType(m_currentLightPayloads[currentIndex].type)
            : PATH_TRACE_REMIX_LIGHT_TYPE_COUNT;
        if (previousIndex < m_previousLightPayloads.size())
        {
            ++m_stats.currentMappedCount;
            if (currentType < PATH_TRACE_REMIX_LIGHT_TYPE_COUNT)
            {
                ++m_stats.currentMappedByType[currentType];
            }
            if (currentIndex < m_currentLightPayloads.size() &&
                RemixMappedPayloadChanged(m_currentLightPayloads[currentIndex], m_previousLightPayloads[previousIndex]))
            {
                if (m_stats.mappedPayloadChangedCount == 0u)
                {
                    m_stats.firstPayloadChangedCurrent = MakeRemixLightEventSample(currentIndex, m_currentLightPayloads[currentIndex]);
                    m_stats.firstPayloadChangedPrevious = MakeRemixLightEventSample(previousIndex, m_previousLightPayloads[previousIndex]);
                }
                ++m_stats.mappedPayloadChangedCount;
                if (currentType < PATH_TRACE_REMIX_LIGHT_TYPE_COUNT)
                {
                    ++m_stats.mappedPayloadChangedByType[currentType];
                }
            }
        }
        else
        {
            if (m_stats.currentInvalidCount == 0u && currentIndex < m_currentLightPayloads.size())
            {
                m_stats.firstCurrentOnly = MakeRemixLightEventSample(currentIndex, m_currentLightPayloads[currentIndex]);
            }
            ++m_stats.currentInvalidCount;
            if (currentType < PATH_TRACE_REMIX_LIGHT_TYPE_COUNT)
            {
                ++m_stats.currentOnlyByType[currentType];
            }
        }
    }
    m_stats.currentOnlyCount = m_stats.currentInvalidCount;
    m_stats.previousMappedCount = 0;
    m_stats.previousInvalidCount = 0;
    for (uint32_t currentIndex : m_previousToCurrentMap)
    {
        const uint32_t previousIndex = m_stats.previousMappedCount + m_stats.previousInvalidCount;
        const uint32_t previousType = previousIndex < m_previousLightPayloads.size()
            ? RemixLightTypeIndexFromUnifiedType(m_previousLightPayloads[previousIndex].type)
            : PATH_TRACE_REMIX_LIGHT_TYPE_COUNT;
        if (currentIndex < m_currentLightPayloads.size())
        {
            ++m_stats.previousMappedCount;
            if (previousType < PATH_TRACE_REMIX_LIGHT_TYPE_COUNT)
            {
                ++m_stats.previousMappedByType[previousType];
            }
        }
        else
        {
            if (m_stats.previousInvalidCount == 0u && previousIndex < m_previousLightPayloads.size())
            {
                m_stats.firstPreviousOnly = MakeRemixLightEventSample(previousIndex, m_previousLightPayloads[previousIndex]);
            }
            ++m_stats.previousInvalidCount;
            if (previousType < PATH_TRACE_REMIX_LIGHT_TYPE_COUNT)
            {
                ++m_stats.previousOnlyByType[previousType];
            }
        }
    }
    m_stats.previousOnlyCount = m_stats.previousInvalidCount;
    m_stats.invalidDuplicateIdentityCount = identities.currentDuplicates + identities.previousDuplicates;
    const auto& currentDuplicateIdentityByType = identities.currentDuplicatesByType;
    const auto& previousDuplicateIdentityByType = identities.previousDuplicatesByType;
    for (uint32_t typeIndex = 0; typeIndex < PATH_TRACE_REMIX_LIGHT_TYPE_COUNT; ++typeIndex)
    {
        m_stats.duplicateIdentityByType[typeIndex] =
            currentDuplicateIdentityByType[typeIndex] + previousDuplicateIdentityByType[typeIndex];
    }
    for (const PathTraceUnifiedLightRecord& record : m_currentLightPayloads)
    {
        if (record.type != PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
        {
            continue;
        }
        if ((record.flags & PATH_TRACE_RLU_LIGHT_FLAG_CURRENT_SAMPLEABLE) != 0u)
        {
            ++m_stats.doomAnalyticCurrentSampleableCount;
        }
        if ((record.flags & PATH_TRACE_RLU_LIGHT_FLAG_STABLE_CACHEABLE) != 0u)
        {
            ++m_stats.doomAnalyticStableCacheableCount;
        }
        if ((record.flags & PATH_TRACE_RLU_LIGHT_FLAG_UNSTABLE_DYNAMIC) != 0u)
        {
            ++m_stats.doomAnalyticUnstableDynamicCount;
        }
        if ((record.flags & PATH_TRACE_RLU_STABILITY_REASON_NO_REMAP) != 0u)
        {
            ++m_stats.doomAnalyticRejectNoRemapCount;
        }
        if ((record.flags & PATH_TRACE_RLU_STABILITY_REASON_PAYLOAD_CHANGED) != 0u)
        {
            ++m_stats.doomAnalyticRejectPayloadChangedCount;
        }
        if ((record.flags & PATH_TRACE_RLU_STABILITY_REASON_UNPROVEN_CONTINUITY) != 0u)
        {
            ++m_stats.doomAnalyticRejectUnprovenContinuityCount;
        }
        if ((record.flags & PATH_TRACE_RLU_STABILITY_REASON_UNKNOWN_IDENTITY) != 0u)
        {
            ++m_stats.doomAnalyticRejectUnknownIdentityCount;
        }
        if ((record.flags & PATH_TRACE_RLU_STABILITY_REASON_DUPLICATE_IDENTITY) != 0u)
        {
            ++m_stats.doomAnalyticRejectDuplicateIdentityCount;
        }
        if ((record.flags & PATH_TRACE_RLU_STABILITY_REASON_PORTAL_DISCONNECTED) != 0u)
        {
            ++m_stats.doomAnalyticRejectPortalDisconnectedCount;
        }
        if ((record.flags & PATH_TRACE_RLU_STABILITY_REASON_OUT_OF_SELECTED_AREA) != 0u)
        {
            ++m_stats.doomAnalyticRejectOutOfSelectedAreaCount;
        }
    }
    m_stats.emissiveRangeOffset = m_lightRanges[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE].firstLightIndex;
    m_stats.emissiveRangeCount = m_lightRanges[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE].lightCount;
    m_stats.doomAnalyticRangeOffset = m_lightRanges[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC].firstLightIndex;
    m_stats.doomAnalyticRangeCount = m_lightRanges[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC].lightCount;
    m_stats.emissiveSampleCount = m_lightRanges[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE].sampleCount;
    m_stats.doomAnalyticSampleCount = m_lightRanges[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC].sampleCount;
    m_stats.totalSampleCount = m_stats.emissiveSampleCount + m_stats.doomAnalyticSampleCount;
    m_stats.nonEmptyRangeCount =
        (m_stats.emissiveRangeCount > 0 ? 1u : 0u) +
        (m_stats.doomAnalyticRangeCount > 0 ? 1u : 0u);
    m_stats.structuralSignatureChanged = structuralChanged;
    m_stats.mappingSignatureChanged = mappingChanged;
    m_stats.payloadSignatureChanged = payloadChanged;
    m_stats.payloadOnlyChange = structuralChanged == 0u && mappingChanged == 0u && payloadChanged != 0u ? 1u : 0u;
    m_stats.oldSmokeReservoirSignatureConsulted = 0;
    m_stats.resourceAllocationCount = 0;
    m_stats.shaderRouteCount = 0;
    m_stats.resetReasonFlags = framePackage.resetReasonFlags;
    m_stats.firstFailingContract = PATH_TRACE_REMIX_LIGHT_CONTRACT_OK;
    if (m_currentToPreviousMap.size() != m_currentLightPayloads.size() ||
        m_previousToCurrentMap.size() != m_previousLightPayloads.size())
    {
        m_stats.firstFailingContract = PATH_TRACE_REMIX_LIGHT_CONTRACT_MAP_SIZE_MISMATCH;
    }
    else if (m_stats.invalidDuplicateIdentityCount != 0u)
    {
        m_stats.firstFailingContract = PATH_TRACE_REMIX_LIGHT_CONTRACT_DUPLICATE_IDENTITY;
    }
    else if (m_stats.currentLightCount == 0u)
    {
        m_stats.firstFailingContract = PATH_TRACE_REMIX_LIGHT_CONTRACT_NO_CURRENT_DOMAIN;
    }

    m_lastStructuralSignature = structuralSignature;
    m_lastMappingSignature = mappingSignature;
    m_lastPayloadSignature = payloadSignature;
    m_haveLastSignatures = true;
}

void PathTraceRemixLightManager::RebuildSignatures()
{
    uint64_t structuralHash = 1469598103934665603ull;
    const uint64_t structuralVersion = 2;
    structuralHash = RemixHashValue(structuralHash, structuralVersion);
    structuralHash = RemixHashValue(structuralHash, static_cast<uint32_t>(PATH_TRACE_REMIX_LIGHT_TYPE_COUNT));
    structuralHash = RemixHashValue(structuralHash, static_cast<uint32_t>(PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE));
    structuralHash = RemixHashValue(structuralHash, static_cast<uint32_t>(PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC));
    structuralHash = RemixHashValue(structuralHash, static_cast<uint32_t>(PATH_TRACE_REMIX_LIGHT_INVALID_INDEX));
    structuralHash = RemixHashValue(structuralHash, static_cast<uint32_t>(sizeof(PathTraceUnifiedLightRecord)));
    structuralHash = RemixHashValue(structuralHash, static_cast<uint32_t>(sizeof(PathTraceRemixLightRange)));

    uint64_t mappingHash = 1469598103934665603ull;
    const uint64_t mappingVersion = 1;
    mappingHash = RemixHashValue(mappingHash, mappingVersion);
    mappingHash = RemixHashVector(mappingHash, m_currentToPreviousMap);
    mappingHash = RemixHashVector(mappingHash, m_previousToCurrentMap);

    uint64_t payloadHash = 1469598103934665603ull;
    const uint64_t payloadVersion = 1;
    payloadHash = RemixHashValue(payloadHash, payloadVersion);
    payloadHash = RemixHashVector(payloadHash, m_currentLightPayloads);
    payloadHash = RemixHashVector(payloadHash, m_previousLightPayloads);
    payloadHash = RemixHashBytes(payloadHash, m_lightRanges, sizeof(m_lightRanges));

    m_stats.structuralSignature = structuralHash;
    m_stats.mappingSignature = mappingHash;
    m_stats.payloadSignature = payloadHash;
}
