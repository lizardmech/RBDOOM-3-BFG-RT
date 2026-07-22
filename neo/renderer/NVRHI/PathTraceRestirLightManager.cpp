#include "precompiled.h"
#pragma hdrstop

#include "PathTraceRestirLightManager.h"
#include "PathTraceDoomLights.h"
#include "PathTraceEmissiveCandidates.h"

#include <algorithm>
#include <cmath>

namespace {

uint64 HashLightManagerBytes(uint64 hash, const void* data, size_t size)
{
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= static_cast<uint64>(bytes[i]);
        hash *= 1099511628211ull;
    }
    return hash;
}

uint64 HashLightManagerValue(uint64 hash, uint32_t value)
{
    return HashLightManagerBytes(hash, &value, sizeof(value));
}

uint64 HashLightManagerValue64(uint64 hash, uint64_t value)
{
    return HashLightManagerBytes(hash, &value, sizeof(value));
}

void AccumulateInvalidReasonStats(
    const uint32_t invalidReasonFlags,
    PathTraceRestirLightInvalidReasonStats& stats)
{
    if ((invalidReasonFlags & PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_NEW_LIGHT) != 0u)
    {
        ++stats.newLight;
    }
    if ((invalidReasonFlags & PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_MISSING_LIGHT) != 0u)
    {
        ++stats.missingLight;
    }
    if ((invalidReasonFlags & PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_TEMPORARY) != 0u)
    {
        ++stats.temporary;
    }
    if ((invalidReasonFlags & PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_PROJECTILE) != 0u)
    {
        ++stats.projectile;
    }
    if ((invalidReasonFlags & PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_DUPLICATE) != 0u)
    {
        ++stats.duplicate;
    }
    if ((invalidReasonFlags & PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_UNKNOWN_IDENTITY) != 0u)
    {
        ++stats.unknownIdentity;
    }
    if ((invalidReasonFlags & PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_UNSUPPORTED_SOURCE) != 0u)
    {
        ++stats.unsupportedSource;
    }
    if ((invalidReasonFlags & PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_STRUCTURAL_RESET) != 0u)
    {
        ++stats.structuralReset;
    }
    if ((invalidReasonFlags & PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_UNPROVEN_CONTINUITY) != 0u)
    {
        ++stats.unprovenContinuity;
    }
    if ((invalidReasonFlags & PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_ZERO_RADIANCE) != 0u)
    {
        ++stats.zeroRadiance;
    }
    if ((invalidReasonFlags & PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_SUPPRESSED) != 0u)
    {
        ++stats.suppressed;
    }
    if ((invalidReasonFlags & PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_OUT_OF_SELECTED_AREA) != 0u)
    {
        ++stats.outOfSelectedArea;
    }
    if ((invalidReasonFlags & PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_DISCONNECTED_OR_PORTAL) != 0u)
    {
        ++stats.disconnectedOrPortal;
    }
    if ((invalidReasonFlags & PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_INVALID_SHAPE) != 0u)
    {
        ++stats.invalidShape;
    }
    if ((invalidReasonFlags & PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_CANDIDATE_CAP) != 0u)
    {
        ++stats.candidateCap;
    }
    if ((invalidReasonFlags & PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_INCOMPATIBLE_SOURCE) != 0u)
    {
        ++stats.incompatibleSource;
    }
    if ((invalidReasonFlags & PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_DELETED) != 0u)
    {
        ++stats.deleted;
    }
}

PathTraceRestirPreviousLightRecord MakePreviousRecord(const PathTraceRestirCurrentLightRecord& currentRecord)
{
    PathTraceRestirPreviousLightRecord previousRecord;
    previousRecord.sourceType = currentRecord.sourceType;
    previousRecord.payloadSourceIndex = currentRecord.payloadSourceIndex;
    previousRecord.identityKeyLo = currentRecord.identityKeyLo;
    previousRecord.identityKeyHi = currentRecord.identityKeyHi;
    previousRecord.compatibilityKey0 = currentRecord.compatibilityKey0;
    previousRecord.compatibilityKey1 = currentRecord.compatibilityKey1;
    previousRecord.compatibilityKey2 = currentRecord.compatibilityKey2;
    previousRecord.continuityClass = currentRecord.continuityClass;
    previousRecord.payloadHashLo = currentRecord.payloadHashLo;
    previousRecord.payloadHashHi = currentRecord.payloadHashHi;
    previousRecord.flags = currentRecord.flags & PATH_TRACE_RESTIR_LIGHT_RECORD_STABLE_IDENTITY;
    previousRecord.invalidReasonFlags = currentRecord.invalidReasonFlags & (
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_DUPLICATE |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_UNKNOWN_IDENTITY |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_UNSUPPORTED_SOURCE |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_UNPROVEN_CONTINUITY |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_ZERO_RADIANCE |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_SUPPRESSED |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_OUT_OF_SELECTED_AREA |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_DISCONNECTED_OR_PORTAL |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_INVALID_SHAPE |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_CANDIDATE_CAP |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_INCOMPATIBLE_SOURCE |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_DELETED);
    return previousRecord;
}

float Max3(const float x, const float y, const float z)
{
    return std::max(std::max(x, y), z);
}
float LightManagerLuminance(const float rgb[3])
{
    return rgb[0] * 0.2126f + rgb[1] * 0.7152f + rgb[2] * 0.0722f;
}

PathTraceUnifiedLightRecord BuildRestirManagerEmissivePayloadRecord(
    const PathTraceSmokeEmissiveTriangle& emissiveTriangle,
    uint32_t sourceIndex,
    uint32_t previousIndex)
{
    PathTraceUnifiedLightRecord record;
    record.positionAndRadius[0] = emissiveTriangle.centerAndArea[0];
    record.positionAndRadius[1] = emissiveTriangle.centerAndArea[1];
    record.positionAndRadius[2] = emissiveTriangle.centerAndArea[2];
    record.positionAndRadius[3] = 0.0f;
    record.normalAndArea[0] = emissiveTriangle.normalAndLuminance[0];
    record.normalAndArea[1] = emissiveTriangle.normalAndLuminance[1];
    record.normalAndArea[2] = emissiveTriangle.normalAndLuminance[2];
    record.normalAndArea[3] = std::max(emissiveTriangle.centerAndArea[3], 0.0f);
    record.radianceAndLuminance[0] = std::max(emissiveTriangle.estimatedRadianceAndLuminance[0], 0.0f);
    record.radianceAndLuminance[1] = std::max(emissiveTriangle.estimatedRadianceAndLuminance[1], 0.0f);
    record.radianceAndLuminance[2] = std::max(emissiveTriangle.estimatedRadianceAndLuminance[2], 0.0f);
    record.radianceAndLuminance[3] = std::max(emissiveTriangle.estimatedRadianceAndLuminance[3], LightManagerLuminance(record.radianceAndLuminance));
    record.uvOrDoomParams[0] = emissiveTriangle.uvBounds[0];
    record.uvOrDoomParams[1] = emissiveTriangle.uvBounds[1];
    record.uvOrDoomParams[2] = emissiveTriangle.uvBounds[2];
    record.uvOrDoomParams[3] = emissiveTriangle.uvBounds[3];
    record.type = PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE;
    record.sourceIndex = sourceIndex;
    record.flags = emissiveTriangle.flags | emissiveTriangle.padding0;
    record.materialOrLightId = emissiveTriangle.materialIndex;
    record.instanceId = emissiveTriangle.instanceId;
    record.primitiveIndex = emissiveTriangle.primitiveIndex;
    record.identityA = emissiveTriangle.identityHashLo;
    record.identityB = emissiveTriangle.identityHashHi;
    record.sourcePdf = std::max(emissiveTriangle.sampleWeightAndPdf[1], 0.0f);
    record.sourceWeight = std::max(
        std::max(emissiveTriangle.sampleWeightAndPdf[0], emissiveTriangle.estimatedRadianceAndLuminance[3]),
        emissiveTriangle.centerAndArea[3] > 1.0e-6f ? 1.0e-6f : 0.0f);
    record.previousIndex = previousIndex;
    return record;
}

PathTraceUnifiedLightRecord BuildRestirManagerDoomAnalyticPayloadRecord(
    const PathTraceDoomAnalyticLightCandidate& analyticLight,
    const PathTraceDoomAnalyticLightCandidateIdentity* identity,
    uint32_t sourceIndex,
    uint32_t previousIndex)
{
    const float radius = std::max(analyticLight.originAndRadius[3], 0.01f);
    const float influenceRadius = std::max(analyticLight.doomRadiusAndArea[0], 1.0f);

    PathTraceUnifiedLightRecord record;
    record.positionAndRadius[0] = analyticLight.originAndRadius[0];
    record.positionAndRadius[1] = analyticLight.originAndRadius[1];
    record.positionAndRadius[2] = analyticLight.originAndRadius[2];
    record.positionAndRadius[3] = radius;
    record.normalAndArea[0] = 0.0f;
    record.normalAndArea[1] = 0.0f;
    record.normalAndArea[2] = 1.0f;
    record.normalAndArea[3] = 4.0f * idMath::PI * radius * radius;
    record.radianceAndLuminance[0] = std::max(analyticLight.colorAndIntensity[0], 0.0f);
    record.radianceAndLuminance[1] = std::max(analyticLight.colorAndIntensity[1], 0.0f);
    record.radianceAndLuminance[2] = std::max(analyticLight.colorAndIntensity[2], 0.0f);
    record.radianceAndLuminance[3] = LightManagerLuminance(record.radianceAndLuminance);
    record.uvOrDoomParams[0] = influenceRadius;
    record.uvOrDoomParams[1] = analyticLight.doomRadiusAndArea[1];
    record.uvOrDoomParams[2] = analyticLight.doomRadiusAndArea[2];
    record.uvOrDoomParams[3] = analyticLight.doomRadiusAndArea[3];
    record.type = PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC;
    record.sourceIndex = sourceIndex;
    record.flags = analyticLight.flags | (identity ? identity->flags : 0u);
    record.materialOrLightId = analyticLight.renderLightIndex;
    record.instanceId = 0;
    record.primitiveIndex = 0;
    record.identityA = analyticLight.renderLightIndex;
    record.identityB = analyticLight.entityNumber;
    record.sourcePdf = 0.0f;
    record.sourceWeight = Max3(record.radianceAndLuminance[0], record.radianceAndLuminance[1], record.radianceAndLuminance[2]) *
        record.normalAndArea[3] * influenceRadius;
    record.previousIndex = previousIndex;
    return record;
}

PathTraceRestirCurrentLightRecord MakeCurrentRecordFromObservation(
    const PathTraceRestirLightObservation& observation)
{
    PathTraceRestirCurrentLightRecord record;
    record.sourceType = observation.sourceType;
    record.payloadSourceIndex = observation.payloadSourceIndex;
    record.identityKeyLo = observation.identityKeyLo;
    record.identityKeyHi = observation.identityKeyHi;
    record.compatibilityKey0 = observation.compatibilityKey0;
    record.compatibilityKey1 = observation.compatibilityKey1;
    record.compatibilityKey2 = observation.compatibilityKey2;
    record.payloadHashLo = observation.payloadHashLo;
    record.payloadHashHi = observation.payloadHashHi;
    record.flags = observation.flags;
    record.invalidReasonFlags = observation.invalidReasonFlags;
    if ((record.flags & PATH_TRACE_RESTIR_LIGHT_RECORD_STABLE_IDENTITY) != 0u)
    {
        record.continuityClass = record.invalidReasonFlags != PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_NONE
            ? PATH_TRACE_RESTIR_LIGHT_CONTINUITY_REMAP_INVALID
            : PATH_TRACE_RESTIR_LIGHT_CONTINUITY_STABLE;
    }
    else
    {
        record.flags |= PATH_TRACE_RESTIR_LIGHT_RECORD_CURRENT_ONLY;
        record.continuityClass = PATH_TRACE_RESTIR_LIGHT_CONTINUITY_CURRENT_ONLY;
        if (record.invalidReasonFlags == PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_NONE)
        {
            record.invalidReasonFlags |= PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_UNKNOWN_IDENTITY;
        }
    }
    return record;
}

bool PayloadChanged(
    const PathTraceRestirCurrentLightRecord& currentRecord,
    const PathTraceRestirPreviousLightRecord& previousRecord)
{
    return currentRecord.payloadHashLo != previousRecord.payloadHashLo ||
        currentRecord.payloadHashHi != previousRecord.payloadHashHi;
}

bool RecordsCompatible(
    const PathTraceRestirCurrentLightRecord& currentRecord,
    const PathTraceRestirPreviousLightRecord& previousRecord)
{
    if (currentRecord.sourceType != previousRecord.sourceType)
    {
        return false;
    }
    return true;
}

PathTraceRestirPreviousLightRecord MakeMissingPreviousRecord(PathTraceRestirPreviousLightRecord previousRecord)
{
    previousRecord.flags &= ~PATH_TRACE_RESTIR_LIGHT_RECORD_REMAP_VALID;
    previousRecord.flags |= PATH_TRACE_RESTIR_LIGHT_RECORD_PREVIOUS_ONLY;
    previousRecord.continuityClass = PATH_TRACE_RESTIR_LIGHT_CONTINUITY_PREVIOUS_ONLY;
    previousRecord.invalidReasonFlags |= PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_MISSING_LIGHT;
    return previousRecord;
}

void MarkDuplicate(PathTraceRestirCurrentLightRecord& record)
{
    record.flags &= ~PATH_TRACE_RESTIR_LIGHT_RECORD_REMAP_VALID;
    record.continuityClass = PATH_TRACE_RESTIR_LIGHT_CONTINUITY_REMAP_INVALID;
    record.invalidReasonFlags |= PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_DUPLICATE;
}

void MarkIncompatible(PathTraceRestirCurrentLightRecord& record)
{
    record.flags &= ~PATH_TRACE_RESTIR_LIGHT_RECORD_REMAP_VALID;
    record.continuityClass = PATH_TRACE_RESTIR_LIGHT_CONTINUITY_REMAP_INVALID;
    record.invalidReasonFlags |= PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_INCOMPATIBLE_SOURCE;
}

void MarkIncompatible(PathTraceRestirPreviousLightRecord& record)
{
    record.flags &= ~PATH_TRACE_RESTIR_LIGHT_RECORD_REMAP_VALID;
    record.continuityClass = PATH_TRACE_RESTIR_LIGHT_CONTINUITY_REMAP_INVALID;
    record.invalidReasonFlags |= PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_INCOMPATIBLE_SOURCE;
}

void MarkDuplicate(PathTraceRestirPreviousLightRecord& record)
{
    record.flags &= ~PATH_TRACE_RESTIR_LIGHT_RECORD_REMAP_VALID;
    record.continuityClass = PATH_TRACE_RESTIR_LIGHT_CONTINUITY_REMAP_INVALID;
    record.invalidReasonFlags |= PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_DUPLICATE;
}

PathTraceRestirLightManager::StableKey MakeStableKey(const PathTraceRestirCurrentLightRecord& record)
{
    PathTraceRestirLightManager::StableKey key;
    key.sourceType = record.sourceType;
    key.keyLo = record.identityKeyLo;
    key.keyHi = record.identityKeyHi;
    return key;
}

PathTraceRestirLightManager::StableKey MakeStableKey(const PathTraceRestirPreviousLightRecord& record)
{
    PathTraceRestirLightManager::StableKey key;
    key.sourceType = record.sourceType;
    key.keyLo = record.identityKeyLo;
    key.keyHi = record.identityKeyHi;
    return key;
}

bool RecordHasStableIdentity(const PathTraceRestirCurrentLightRecord& record)
{
    return (record.flags & PATH_TRACE_RESTIR_LIGHT_RECORD_STABLE_IDENTITY) != 0u;
}

bool RecordHasStableIdentity(const PathTraceRestirPreviousLightRecord& record)
{
    return (record.flags & PATH_TRACE_RESTIR_LIGHT_RECORD_STABLE_IDENTITY) != 0u;
}

bool RecordRemapBlocked(const PathTraceRestirCurrentLightRecord& record)
{
    return record.invalidReasonFlags != PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_NONE;
}

bool RecordRemapBlocked(const PathTraceRestirPreviousLightRecord& record)
{
    return record.invalidReasonFlags != PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_NONE;
}

bool IsSampleableRestirLightSource(uint32_t sourceType)
{
    return sourceType == PATH_TRACE_RESTIR_LIGHT_SOURCE_EMISSIVE_TRIANGLE ||
        sourceType == PATH_TRACE_RESTIR_LIGHT_SOURCE_DOOM_ANALYTIC;
}

uint32_t ActiveDomainDisqualifyingReasons()
{
    return
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_TEMPORARY |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_PROJECTILE |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_DUPLICATE |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_UNKNOWN_IDENTITY |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_UNSUPPORTED_SOURCE |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_STRUCTURAL_RESET |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_UNPROVEN_CONTINUITY |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_ZERO_RADIANCE |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_SUPPRESSED |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_OUT_OF_SELECTED_AREA |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_DISCONNECTED_OR_PORTAL |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_INVALID_SHAPE |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_CANDIDATE_CAP |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_INCOMPATIBLE_SOURCE |
        PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_DELETED;
}

bool CurrentRecordIsActiveSampleable(const PathTraceRestirCurrentLightRecord& record)
{
    return IsSampleableRestirLightSource(record.sourceType) &&
        record.payloadSourceIndex != PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX &&
        (record.flags & PATH_TRACE_RESTIR_LIGHT_RECORD_STABLE_IDENTITY) != 0u &&
        (record.invalidReasonFlags & ActiveDomainDisqualifyingReasons()) == 0u;
}

bool PreviousRecordIsActiveSampleable(const PathTraceRestirPreviousLightRecord& record)
{
    return IsSampleableRestirLightSource(record.sourceType) &&
        record.payloadSourceIndex != PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX &&
        (record.flags & PATH_TRACE_RESTIR_LIGHT_RECORD_STABLE_IDENTITY) != 0u &&
        (record.invalidReasonFlags & ActiveDomainDisqualifyingReasons()) == 0u;
}

} // namespace

size_t PathTraceRestirLightManager::StableKeyHash::operator()(const StableKey& key) const
{
    uint64 hash = 1469598103934665603ull;
    hash = HashLightManagerValue(hash, key.sourceType);
    hash = HashLightManagerValue(hash, key.keyLo);
    hash = HashLightManagerValue(hash, key.keyHi);
    return static_cast<size_t>(hash);
}

void PathTraceRestirLightManager::Clear()
{
    m_currentLightRecords.clear();
    m_previousLightRecords.clear();
    m_currentToPreviousRemap.clear();
    m_previousToCurrentRemap.clear();
    m_activeCurrentLightRecords.clear();
    m_activePreviousLightRecords.clear();
    m_activeCurrentToPreviousRemap.clear();
    m_activePreviousToCurrentRemap.clear();
    m_activeCurrentLightRanges.clear();
    m_activeCurrentPayloadRecords.clear();
    m_activePreviousPayloadRecords.clear();
    m_previousStableLookup.clear();
    m_stats = {};
    m_lastStructuralSignature = 0;
    m_lastMappingIdentitySignature = 0;
    m_lastAnimatedPayloadSignature = 0;
    m_haveLastSignatures = false;
}

void PathTraceRestirLightManager::BeginFrame()
{
    m_previousLightRecords.clear();
    m_previousLightRecords.reserve(m_currentLightRecords.size());
    for (const PathTraceRestirCurrentLightRecord& currentRecord : m_currentLightRecords)
    {
        m_previousLightRecords.push_back(MakePreviousRecord(currentRecord));
    }

    m_currentLightRecords.clear();
    m_currentToPreviousRemap.clear();
    m_previousToCurrentRemap.clear();
    m_activeCurrentLightRecords.clear();
    m_activePreviousLightRecords.clear();
    m_activeCurrentToPreviousRemap.clear();
    m_activePreviousToCurrentRemap.clear();
    m_activeCurrentLightRanges.clear();
    m_activeCurrentPayloadRecords.clear();
    m_activePreviousPayloadRecords.clear();
    RebuildPreviousLookup();
    m_stats = {};
    m_stats.previousLightCount = static_cast<uint32_t>(m_previousLightRecords.size());
}

void PathTraceRestirLightManager::UpdateFromObservations(
    const std::vector<PathTraceRestirLightObservation>& observations)
{
    m_currentLightRecords.clear();
    m_currentLightRecords.reserve(observations.size());

    std::unordered_map<StableKey, uint32_t, StableKeyHash> currentStableLookup;

    auto addCurrentRecord = [&](PathTraceRestirCurrentLightRecord record) {
        if (RecordHasStableIdentity(record))
        {
            const StableKey stableKey = MakeStableKey(record);
            auto currentIt = currentStableLookup.find(stableKey);
            if (currentIt != currentStableLookup.end())
            {
                MarkDuplicate(m_currentLightRecords[currentIt->second]);
                MarkDuplicate(record);
                auto previousIt = m_previousStableLookup.find(stableKey);
                if (previousIt != m_previousStableLookup.end())
                {
                    MarkDuplicate(m_previousLightRecords[previousIt->second]);
                }
            }
            else
            {
                currentStableLookup[stableKey] = static_cast<uint32_t>(m_currentLightRecords.size());
            }
        }

        m_currentLightRecords.push_back(record);
    };

    for (const PathTraceRestirLightObservation& observation : observations)
    {
        addCurrentRecord(MakeCurrentRecordFromObservation(observation));
    }

    RebuildCpuRemaps();
    RebuildActiveDomains();
}

void PathTraceRestirLightManager::UpdateActivePayloadRecords(
    const std::vector<PathTraceSmokeEmissiveTriangle>& emissiveTriangles,
    const std::vector<PathTraceSmokeEmissiveTriangle>& previousEmissiveTriangles,
    const std::vector<PathTraceDoomAnalyticLightCandidate>& doomAnalyticLights,
    const std::vector<PathTraceDoomAnalyticLightCandidate>& previousDoomAnalyticLights,
    const std::vector<PathTraceDoomAnalyticLightCandidateIdentity>& doomAnalyticIdentities,
    const std::vector<PathTraceDoomAnalyticLightCandidateIdentity>& previousDoomAnalyticIdentities)
{
    RebuildActivePayloadRecords(
        emissiveTriangles,
        previousEmissiveTriangles,
        doomAnalyticLights,
        previousDoomAnalyticLights,
        doomAnalyticIdentities,
        previousDoomAnalyticIdentities);
}

void PathTraceRestirLightManager::EndFrame()
{
    RebuildStats();
}

PathTraceRestirLightManagerStats PathTraceRestirLightManager::GetStats() const
{
    return m_stats;
}

const std::vector<PathTraceRestirCurrentLightRecord>& PathTraceRestirLightManager::GetCurrentLightRecords() const
{
    return m_currentLightRecords;
}

const std::vector<PathTraceRestirPreviousLightRecord>& PathTraceRestirLightManager::GetPreviousLightRecords() const
{
    return m_previousLightRecords;
}

const std::vector<uint32_t>& PathTraceRestirLightManager::GetCurrentToPreviousRemap() const
{
    return m_currentToPreviousRemap;
}

const std::vector<uint32_t>& PathTraceRestirLightManager::GetPreviousToCurrentRemap() const
{
    return m_previousToCurrentRemap;
}

const std::vector<PathTraceRestirCurrentLightRecord>& PathTraceRestirLightManager::GetActiveCurrentLightRecords() const
{
    return m_activeCurrentLightRecords;
}

const std::vector<PathTraceRestirPreviousLightRecord>& PathTraceRestirLightManager::GetActivePreviousLightRecords() const
{
    return m_activePreviousLightRecords;
}

const std::vector<uint32_t>& PathTraceRestirLightManager::GetActiveCurrentToPreviousRemap() const
{
    return m_activeCurrentToPreviousRemap;
}

const std::vector<uint32_t>& PathTraceRestirLightManager::GetActivePreviousToCurrentRemap() const
{
    return m_activePreviousToCurrentRemap;
}

const std::vector<PathTraceRestirLightActiveRange>& PathTraceRestirLightManager::GetActiveCurrentLightRanges() const
{
    return m_activeCurrentLightRanges;
}

const std::vector<PathTraceUnifiedLightRecord>& PathTraceRestirLightManager::GetActiveCurrentPayloadRecords() const
{
    return m_activeCurrentPayloadRecords;
}

const std::vector<PathTraceUnifiedLightRecord>& PathTraceRestirLightManager::GetActivePreviousPayloadRecords() const
{
    return m_activePreviousPayloadRecords;
}

void PathTraceRestirLightManager::RebuildCpuRemaps()
{
    m_currentToPreviousRemap.assign(m_currentLightRecords.size(), PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX);
    m_previousToCurrentRemap.assign(m_previousLightRecords.size(), PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX);
    std::vector<bool> previousMatched(m_previousLightRecords.size(), false);

    for (size_t currentIndex = 0; currentIndex < m_currentLightRecords.size(); ++currentIndex)
    {
        PathTraceRestirCurrentLightRecord& currentRecord = m_currentLightRecords[currentIndex];
        if (!RecordHasStableIdentity(currentRecord))
        {
            continue;
        }

        const StableKey stableKey = MakeStableKey(currentRecord);
        auto previousIt = m_previousStableLookup.find(stableKey);
        if (previousIt == m_previousStableLookup.end())
        {
            if (!RecordRemapBlocked(currentRecord))
            {
                currentRecord.flags |= PATH_TRACE_RESTIR_LIGHT_RECORD_CURRENT_ONLY;
                currentRecord.continuityClass = PATH_TRACE_RESTIR_LIGHT_CONTINUITY_CURRENT_ONLY;
                currentRecord.invalidReasonFlags |= PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_NEW_LIGHT;
            }
            continue;
        }

        const uint32_t previousIndex = previousIt->second;
        PathTraceRestirPreviousLightRecord& previousRecord = m_previousLightRecords[previousIndex];
        previousMatched[previousIndex] = true;
        if (RecordRemapBlocked(currentRecord) || RecordRemapBlocked(previousRecord))
        {
            continue;
        }
        if (!RecordsCompatible(currentRecord, previousRecord))
        {
            MarkIncompatible(currentRecord);
            MarkIncompatible(previousRecord);
            continue;
        }

        currentRecord.flags |= PATH_TRACE_RESTIR_LIGHT_RECORD_REMAP_VALID;
        previousRecord.flags |= PATH_TRACE_RESTIR_LIGHT_RECORD_REMAP_VALID;
        currentRecord.continuityClass = PATH_TRACE_RESTIR_LIGHT_CONTINUITY_STABLE;
        previousRecord.continuityClass = PATH_TRACE_RESTIR_LIGHT_CONTINUITY_STABLE;
        m_currentToPreviousRemap[currentIndex] = previousIndex;
        m_previousToCurrentRemap[previousIndex] = static_cast<uint32_t>(currentIndex);
        if (PayloadChanged(currentRecord, previousRecord))
        {
            currentRecord.flags |= PATH_TRACE_RESTIR_LIGHT_RECORD_PAYLOAD_CHANGED;
        }
    }

    for (size_t i = 0; i < m_previousLightRecords.size(); ++i)
    {
        PathTraceRestirPreviousLightRecord& previousRecord = m_previousLightRecords[i];
        if (RecordHasStableIdentity(previousRecord) && !previousMatched[i] && !RecordRemapBlocked(previousRecord))
        {
            previousRecord = MakeMissingPreviousRecord(previousRecord);
        }
    }
}

void PathTraceRestirLightManager::RebuildActiveDomains()
{
    m_activeCurrentLightRecords.clear();
    m_activePreviousLightRecords.clear();
    m_activeCurrentToPreviousRemap.clear();
    m_activePreviousToCurrentRemap.clear();
    m_activeCurrentLightRanges.clear();

    std::vector<uint32_t> currentDiagnosticToActive(m_currentLightRecords.size(), PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX);
    std::vector<uint32_t> previousDiagnosticToActive(m_previousLightRecords.size(), PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX);

    const uint32_t sourceOrder[] = {
        PATH_TRACE_RESTIR_LIGHT_SOURCE_EMISSIVE_TRIANGLE,
        PATH_TRACE_RESTIR_LIGHT_SOURCE_DOOM_ANALYTIC
    };

    for (const uint32_t sourceType : sourceOrder)
    {
        PathTraceRestirLightActiveRange range;
        range.sourceType = sourceType;
        range.offset = static_cast<uint32_t>(m_activeCurrentLightRecords.size());

        for (size_t diagnosticIndex = 0; diagnosticIndex < m_currentLightRecords.size(); ++diagnosticIndex)
        {
            const PathTraceRestirCurrentLightRecord& record = m_currentLightRecords[diagnosticIndex];
            if (record.sourceType != sourceType || !CurrentRecordIsActiveSampleable(record))
            {
                continue;
            }

            currentDiagnosticToActive[diagnosticIndex] = static_cast<uint32_t>(m_activeCurrentLightRecords.size());
            m_activeCurrentLightRecords.push_back(record);
            ++range.count;
        }

        m_activeCurrentLightRanges.push_back(range);
    }

    for (const uint32_t sourceType : sourceOrder)
    {
        for (size_t diagnosticIndex = 0; diagnosticIndex < m_previousLightRecords.size(); ++diagnosticIndex)
        {
            const PathTraceRestirPreviousLightRecord& record = m_previousLightRecords[diagnosticIndex];
            if (record.sourceType != sourceType || !PreviousRecordIsActiveSampleable(record))
            {
                continue;
            }

            previousDiagnosticToActive[diagnosticIndex] = static_cast<uint32_t>(m_activePreviousLightRecords.size());
            m_activePreviousLightRecords.push_back(record);
        }
    }

    m_activeCurrentToPreviousRemap.assign(m_activeCurrentLightRecords.size(), PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX);
    m_activePreviousToCurrentRemap.assign(m_activePreviousLightRecords.size(), PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX);

    for (size_t currentDiagnosticIndex = 0; currentDiagnosticIndex < m_currentToPreviousRemap.size(); ++currentDiagnosticIndex)
    {
        const uint32_t activeCurrentIndex = currentDiagnosticIndex < currentDiagnosticToActive.size()
            ? currentDiagnosticToActive[currentDiagnosticIndex]
            : PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX;
        if (activeCurrentIndex == PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX)
        {
            continue;
        }

        const uint32_t previousDiagnosticIndex = m_currentToPreviousRemap[currentDiagnosticIndex];
        if (previousDiagnosticIndex == PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX ||
            previousDiagnosticIndex >= previousDiagnosticToActive.size())
        {
            continue;
        }

        const uint32_t activePreviousIndex = previousDiagnosticToActive[previousDiagnosticIndex];
        if (activePreviousIndex != PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX)
        {
            m_activeCurrentToPreviousRemap[activeCurrentIndex] = activePreviousIndex;
        }
    }

    for (size_t previousDiagnosticIndex = 0; previousDiagnosticIndex < m_previousToCurrentRemap.size(); ++previousDiagnosticIndex)
    {
        const uint32_t activePreviousIndex = previousDiagnosticIndex < previousDiagnosticToActive.size()
            ? previousDiagnosticToActive[previousDiagnosticIndex]
            : PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX;
        if (activePreviousIndex == PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX)
        {
            continue;
        }

        const uint32_t currentDiagnosticIndex = m_previousToCurrentRemap[previousDiagnosticIndex];
        if (currentDiagnosticIndex == PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX ||
            currentDiagnosticIndex >= currentDiagnosticToActive.size())
        {
            continue;
        }

        const uint32_t activeCurrentIndex = currentDiagnosticToActive[currentDiagnosticIndex];
        if (activeCurrentIndex != PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX)
        {
            m_activePreviousToCurrentRemap[activePreviousIndex] = activeCurrentIndex;
        }
    }
}

void PathTraceRestirLightManager::RebuildActivePayloadRecords(
    const std::vector<PathTraceSmokeEmissiveTriangle>& emissiveTriangles,
    const std::vector<PathTraceSmokeEmissiveTriangle>& previousEmissiveTriangles,
    const std::vector<PathTraceDoomAnalyticLightCandidate>& doomAnalyticLights,
    const std::vector<PathTraceDoomAnalyticLightCandidate>& previousDoomAnalyticLights,
    const std::vector<PathTraceDoomAnalyticLightCandidateIdentity>& doomAnalyticIdentities,
    const std::vector<PathTraceDoomAnalyticLightCandidateIdentity>& previousDoomAnalyticIdentities)
{
    m_activeCurrentPayloadRecords.clear();
    m_activePreviousPayloadRecords.clear();
    m_activeCurrentPayloadRecords.reserve(m_activeCurrentLightRecords.size());
    m_activePreviousPayloadRecords.reserve(m_activePreviousLightRecords.size());

    for (size_t activeIndex = 0; activeIndex < m_activeCurrentLightRecords.size(); ++activeIndex)
    {
        const PathTraceRestirCurrentLightRecord& record = m_activeCurrentLightRecords[activeIndex];
        const uint32_t previousIndex = activeIndex < m_activeCurrentToPreviousRemap.size()
            ? m_activeCurrentToPreviousRemap[activeIndex]
            : PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX;
        if (record.sourceType == PATH_TRACE_RESTIR_LIGHT_SOURCE_EMISSIVE_TRIANGLE)
        {
            if (record.payloadSourceIndex < emissiveTriangles.size())
            {
                m_activeCurrentPayloadRecords.push_back(BuildRestirManagerEmissivePayloadRecord(
                    emissiveTriangles[record.payloadSourceIndex],
                    record.payloadSourceIndex,
                    previousIndex));
            }
            continue;
        }
        if (record.sourceType == PATH_TRACE_RESTIR_LIGHT_SOURCE_DOOM_ANALYTIC)
        {
            if (record.payloadSourceIndex < doomAnalyticLights.size())
            {
                const PathTraceDoomAnalyticLightCandidateIdentity* identity =
                    record.payloadSourceIndex < doomAnalyticIdentities.size() ? &doomAnalyticIdentities[record.payloadSourceIndex] : nullptr;
                m_activeCurrentPayloadRecords.push_back(BuildRestirManagerDoomAnalyticPayloadRecord(
                    doomAnalyticLights[record.payloadSourceIndex],
                    identity,
                    record.payloadSourceIndex,
                    previousIndex));
            }
        }
    }

    for (size_t activeIndex = 0; activeIndex < m_activePreviousLightRecords.size(); ++activeIndex)
    {
        const PathTraceRestirPreviousLightRecord& record = m_activePreviousLightRecords[activeIndex];
        const uint32_t currentIndex = activeIndex < m_activePreviousToCurrentRemap.size()
            ? m_activePreviousToCurrentRemap[activeIndex]
            : PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX;
        if (record.sourceType == PATH_TRACE_RESTIR_LIGHT_SOURCE_EMISSIVE_TRIANGLE)
        {
            if (record.payloadSourceIndex < previousEmissiveTriangles.size())
            {
                m_activePreviousPayloadRecords.push_back(BuildRestirManagerEmissivePayloadRecord(
                    previousEmissiveTriangles[record.payloadSourceIndex],
                    record.payloadSourceIndex,
                    currentIndex));
            }
            continue;
        }
        if (record.sourceType == PATH_TRACE_RESTIR_LIGHT_SOURCE_DOOM_ANALYTIC)
        {
            if (record.payloadSourceIndex < previousDoomAnalyticLights.size())
            {
                const PathTraceDoomAnalyticLightCandidateIdentity* identity =
                    record.payloadSourceIndex < previousDoomAnalyticIdentities.size() ? &previousDoomAnalyticIdentities[record.payloadSourceIndex] : nullptr;
                m_activePreviousPayloadRecords.push_back(BuildRestirManagerDoomAnalyticPayloadRecord(
                    previousDoomAnalyticLights[record.payloadSourceIndex],
                    identity,
                    record.payloadSourceIndex,
                    currentIndex));
            }
        }
    }
}

void PathTraceRestirLightManager::RebuildStats()
{
    RebuildSignatures();

    m_stats.currentLightCount = static_cast<uint32_t>(m_currentLightRecords.size());
    m_stats.previousLightCount = static_cast<uint32_t>(m_previousLightRecords.size());
    m_stats.currentToPreviousCount = static_cast<uint32_t>(m_currentToPreviousRemap.size());
    m_stats.previousToCurrentCount = static_cast<uint32_t>(m_previousToCurrentRemap.size());
    m_stats.activeCurrentLightCount = static_cast<uint32_t>(m_activeCurrentLightRecords.size());
    m_stats.activePreviousLightCount = static_cast<uint32_t>(m_activePreviousLightRecords.size());
    m_stats.activeCurrentToPreviousCount = static_cast<uint32_t>(m_activeCurrentToPreviousRemap.size());
    m_stats.activePreviousToCurrentCount = static_cast<uint32_t>(m_activePreviousToCurrentRemap.size());
    m_stats.activeCurrentPayloadCount = static_cast<uint32_t>(m_activeCurrentPayloadRecords.size());
    m_stats.activePreviousPayloadCount = static_cast<uint32_t>(m_activePreviousPayloadRecords.size());
    m_stats.activePayloadMapCount = static_cast<uint32_t>(m_activeCurrentToPreviousRemap.size() + m_activePreviousToCurrentRemap.size());
    m_stats.activePayloadCountMismatch =
        (m_activeCurrentPayloadRecords.size() == m_activeCurrentLightRecords.size() &&
            m_activePreviousPayloadRecords.size() == m_activePreviousLightRecords.size())
            ? 0u
            : 1u;
    m_stats.diagnosticCurrentLightCount = m_stats.currentLightCount;
    m_stats.diagnosticPreviousLightCount = m_stats.previousLightCount;
    m_stats.inactiveCurrentLightCount = m_stats.currentLightCount >= m_stats.activeCurrentLightCount
        ? m_stats.currentLightCount - m_stats.activeCurrentLightCount
        : 0u;
    m_stats.inactivePreviousLightCount = m_stats.previousLightCount >= m_stats.activePreviousLightCount
        ? m_stats.previousLightCount - m_stats.activePreviousLightCount
        : 0u;
    m_stats.inactiveZeroRadianceCount = 0;
    m_stats.inactiveSuppressedCount = 0;
    m_stats.activeEmissiveCurrentRangeOffset = 0;
    m_stats.activeEmissiveCurrentRangeCount = 0;
    m_stats.activeDoomAnalyticCurrentRangeOffset = 0;
    m_stats.activeDoomAnalyticCurrentRangeCount = 0;
    m_stats.currentMappedCount = 0;
    m_stats.currentInvalidCount = 0;
    m_stats.previousMappedCount = 0;
    m_stats.previousInvalidCount = 0;
    m_stats.stableIdentityCount = 0;
    m_stats.payloadChangedCount = 0;
    m_stats.stableMappedCount = 0;
    m_stats.payloadChangedMappedCount = 0;
    m_stats.currentOnlyCount = 0;
    m_stats.previousOnlyCount = 0;
    m_stats.remapValidCount = 0;
    m_stats.remapInvalidCount = 0;
    m_stats.mapSizeMismatchCount =
        (m_currentToPreviousRemap.size() == m_currentLightRecords.size() ? 0u : 1u) +
        (m_previousToCurrentRemap.size() == m_previousLightRecords.size() ? 0u : 1u);
    m_stats.topPayloadChangedCurrentIndex = PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX;
    m_stats.topPayloadChangedPreviousIndex = PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX;
    m_stats.topPayloadChangedSourceType = PATH_TRACE_RESTIR_LIGHT_SOURCE_INVALID;
    m_stats.topPayloadChangedIdentityKeyLo = 0;
    m_stats.topPayloadChangedIdentityKeyHi = 0;
    m_stats.topPayloadChangedCurrentHashLo = 0;
    m_stats.topPayloadChangedCurrentHashHi = 0;
    m_stats.topPayloadChangedPreviousHashLo = 0;
    m_stats.topPayloadChangedPreviousHashHi = 0;
    m_stats.invalidReasons = {};

    for (const PathTraceRestirLightActiveRange& range : m_activeCurrentLightRanges)
    {
        if (range.sourceType == PATH_TRACE_RESTIR_LIGHT_SOURCE_EMISSIVE_TRIANGLE)
        {
            m_stats.activeEmissiveCurrentRangeOffset = range.offset;
            m_stats.activeEmissiveCurrentRangeCount = range.count;
        }
        else if (range.sourceType == PATH_TRACE_RESTIR_LIGHT_SOURCE_DOOM_ANALYTIC)
        {
            m_stats.activeDoomAnalyticCurrentRangeOffset = range.offset;
            m_stats.activeDoomAnalyticCurrentRangeCount = range.count;
        }
    }

    for (size_t currentIndex = 0; currentIndex < m_currentLightRecords.size(); ++currentIndex)
    {
        const PathTraceRestirCurrentLightRecord& record = m_currentLightRecords[currentIndex];
        if (!CurrentRecordIsActiveSampleable(record))
        {
            if ((record.invalidReasonFlags & PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_ZERO_RADIANCE) != 0u)
            {
                ++m_stats.inactiveZeroRadianceCount;
            }
            if ((record.invalidReasonFlags & PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_SUPPRESSED) != 0u)
            {
                ++m_stats.inactiveSuppressedCount;
            }
        }
        if ((record.flags & PATH_TRACE_RESTIR_LIGHT_RECORD_STABLE_IDENTITY) != 0u)
        {
            ++m_stats.stableIdentityCount;
        }
        if ((record.flags & PATH_TRACE_RESTIR_LIGHT_RECORD_PAYLOAD_CHANGED) != 0u)
        {
            ++m_stats.payloadChangedCount;
        }
        if ((record.flags & PATH_TRACE_RESTIR_LIGHT_RECORD_REMAP_VALID) != 0u &&
            record.continuityClass == PATH_TRACE_RESTIR_LIGHT_CONTINUITY_STABLE)
        {
            ++m_stats.stableMappedCount;
            if ((record.flags & PATH_TRACE_RESTIR_LIGHT_RECORD_PAYLOAD_CHANGED) != 0u)
            {
                ++m_stats.payloadChangedMappedCount;
                if (m_stats.topPayloadChangedCurrentIndex == PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX &&
                    currentIndex < m_currentToPreviousRemap.size())
                {
                    const uint32_t previousIndex = m_currentToPreviousRemap[currentIndex];
                    if (previousIndex != PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX &&
                        previousIndex < m_previousLightRecords.size())
                    {
                        const PathTraceRestirPreviousLightRecord& previousRecord = m_previousLightRecords[previousIndex];
                        m_stats.topPayloadChangedCurrentIndex = static_cast<uint32_t>(currentIndex);
                        m_stats.topPayloadChangedPreviousIndex = previousIndex;
                        m_stats.topPayloadChangedSourceType = record.sourceType;
                        m_stats.topPayloadChangedIdentityKeyLo = record.identityKeyLo;
                        m_stats.topPayloadChangedIdentityKeyHi = record.identityKeyHi;
                        m_stats.topPayloadChangedCurrentHashLo = record.payloadHashLo;
                        m_stats.topPayloadChangedCurrentHashHi = record.payloadHashHi;
                        m_stats.topPayloadChangedPreviousHashLo = previousRecord.payloadHashLo;
                        m_stats.topPayloadChangedPreviousHashHi = previousRecord.payloadHashHi;
                    }
                }
            }
        }
        if ((record.flags & PATH_TRACE_RESTIR_LIGHT_RECORD_CURRENT_ONLY) != 0u)
        {
            ++m_stats.currentOnlyCount;
        }
        if ((record.flags & PATH_TRACE_RESTIR_LIGHT_RECORD_REMAP_VALID) != 0u)
        {
            ++m_stats.remapValidCount;
        }
        else
        {
            ++m_stats.remapInvalidCount;
        }
        AccumulateInvalidReasonStats(record.invalidReasonFlags, m_stats.invalidReasons);
    }

    for (const PathTraceRestirPreviousLightRecord& record : m_previousLightRecords)
    {
        if (!PreviousRecordIsActiveSampleable(record))
        {
            if ((record.invalidReasonFlags & PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_ZERO_RADIANCE) != 0u)
            {
                ++m_stats.inactiveZeroRadianceCount;
            }
            if ((record.invalidReasonFlags & PATH_TRACE_RESTIR_LIGHT_INVALID_REASON_SUPPRESSED) != 0u)
            {
                ++m_stats.inactiveSuppressedCount;
            }
        }
        if ((record.flags & PATH_TRACE_RESTIR_LIGHT_RECORD_PREVIOUS_ONLY) != 0u)
        {
            ++m_stats.previousOnlyCount;
        }
        if ((record.flags & PATH_TRACE_RESTIR_LIGHT_RECORD_REMAP_VALID) == 0u)
        {
            ++m_stats.remapInvalidCount;
        }
        AccumulateInvalidReasonStats(record.invalidReasonFlags, m_stats.invalidReasons);
    }

    for (const uint32_t remapIndex : m_currentToPreviousRemap)
    {
        if (remapIndex == PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX)
        {
            ++m_stats.currentInvalidCount;
        }
        else
        {
            ++m_stats.currentMappedCount;
        }
    }

    for (const uint32_t remapIndex : m_previousToCurrentRemap)
    {
        if (remapIndex == PATH_TRACE_RESTIR_LIGHT_INVALID_INDEX)
        {
            ++m_stats.previousInvalidCount;
        }
        else
        {
            ++m_stats.previousMappedCount;
        }
    }
}

void PathTraceRestirLightManager::RebuildSignatures()
{
    const uint64_t structuralVersion = 1;
    uint64_t structuralHash = 1469598103934665603ull;
    structuralHash = HashLightManagerValue64(structuralHash, structuralVersion);
    structuralHash = HashLightManagerValue(structuralHash, m_currentToPreviousRemap.size() == m_currentLightRecords.size() ? 0u : 1u);
    structuralHash = HashLightManagerValue(structuralHash, m_previousToCurrentRemap.size() == m_previousLightRecords.size() ? 0u : 1u);

    uint64_t mappingHash = 1469598103934665603ull;
    const uint64_t currentCount = static_cast<uint64_t>(m_currentLightRecords.size());
    const uint64_t previousCount = static_cast<uint64_t>(m_previousLightRecords.size());
    mappingHash = HashLightManagerValue64(mappingHash, currentCount);
    for (const PathTraceRestirCurrentLightRecord& record : m_currentLightRecords)
    {
        mappingHash = HashLightManagerValue(mappingHash, record.sourceType);
        mappingHash = HashLightManagerValue(mappingHash, record.identityKeyLo);
        mappingHash = HashLightManagerValue(mappingHash, record.identityKeyHi);
        mappingHash = HashLightManagerValue(mappingHash, record.compatibilityKey0);
        mappingHash = HashLightManagerValue(mappingHash, record.compatibilityKey1);
        mappingHash = HashLightManagerValue(mappingHash, record.compatibilityKey2);
        mappingHash = HashLightManagerValue(mappingHash, record.continuityClass);
        mappingHash = HashLightManagerValue(mappingHash, record.flags & ~PATH_TRACE_RESTIR_LIGHT_RECORD_PAYLOAD_CHANGED);
        mappingHash = HashLightManagerValue(mappingHash, record.invalidReasonFlags);
    }
    mappingHash = HashLightManagerValue64(mappingHash, previousCount);
    for (const PathTraceRestirPreviousLightRecord& record : m_previousLightRecords)
    {
        mappingHash = HashLightManagerValue(mappingHash, record.sourceType);
        mappingHash = HashLightManagerValue(mappingHash, record.identityKeyLo);
        mappingHash = HashLightManagerValue(mappingHash, record.identityKeyHi);
        mappingHash = HashLightManagerValue(mappingHash, record.compatibilityKey0);
        mappingHash = HashLightManagerValue(mappingHash, record.compatibilityKey1);
        mappingHash = HashLightManagerValue(mappingHash, record.compatibilityKey2);
        mappingHash = HashLightManagerValue(mappingHash, record.continuityClass);
        mappingHash = HashLightManagerValue(mappingHash, record.flags & ~PATH_TRACE_RESTIR_LIGHT_RECORD_PAYLOAD_CHANGED);
        mappingHash = HashLightManagerValue(mappingHash, record.invalidReasonFlags);
    }
    mappingHash = HashLightManagerValue64(mappingHash, static_cast<uint64_t>(m_currentToPreviousRemap.size()));
    for (const uint32_t remapIndex : m_currentToPreviousRemap)
    {
        mappingHash = HashLightManagerValue(mappingHash, remapIndex);
    }
    mappingHash = HashLightManagerValue64(mappingHash, static_cast<uint64_t>(m_previousToCurrentRemap.size()));
    for (const uint32_t remapIndex : m_previousToCurrentRemap)
    {
        mappingHash = HashLightManagerValue(mappingHash, remapIndex);
    }

    uint64_t payloadHash = 1469598103934665603ull;
    payloadHash = HashLightManagerValue64(payloadHash, currentCount);
    for (const PathTraceRestirCurrentLightRecord& record : m_currentLightRecords)
    {
        payloadHash = HashLightManagerValue(payloadHash, record.sourceType);
        payloadHash = HashLightManagerValue(payloadHash, record.identityKeyLo);
        payloadHash = HashLightManagerValue(payloadHash, record.identityKeyHi);
        payloadHash = HashLightManagerValue(payloadHash, record.payloadHashLo);
        payloadHash = HashLightManagerValue(payloadHash, record.payloadHashHi);
    }

    m_stats.structuralSignature = structuralHash;
    m_stats.mappingIdentitySignature = mappingHash;
    m_stats.animatedPayloadSignature = payloadHash;
    m_stats.structuralSignatureChanged = !m_haveLastSignatures || structuralHash != m_lastStructuralSignature ? 1u : 0u;
    m_stats.mappingIdentitySignatureChanged = !m_haveLastSignatures || mappingHash != m_lastMappingIdentitySignature ? 1u : 0u;
    m_stats.animatedPayloadSignatureChanged = !m_haveLastSignatures || payloadHash != m_lastAnimatedPayloadSignature ? 1u : 0u;
    m_lastStructuralSignature = structuralHash;
    m_lastMappingIdentitySignature = mappingHash;
    m_lastAnimatedPayloadSignature = payloadHash;
    m_haveLastSignatures = true;
}

void PathTraceRestirLightManager::RebuildPreviousLookup()
{
    m_previousStableLookup.clear();
    for (size_t i = 0; i < m_previousLightRecords.size(); ++i)
    {
        PathTraceRestirPreviousLightRecord& record = m_previousLightRecords[i];
        if (!RecordHasStableIdentity(record))
        {
            continue;
        }

        const StableKey stableKey = MakeStableKey(record);
        auto previousIt = m_previousStableLookup.find(stableKey);
        if (previousIt != m_previousStableLookup.end())
        {
            MarkDuplicate(m_previousLightRecords[previousIt->second]);
            MarkDuplicate(record);
            continue;
        }
        m_previousStableLookup[stableKey] = static_cast<uint32_t>(i);
    }
}
