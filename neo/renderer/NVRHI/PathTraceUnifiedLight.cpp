#include "precompiled.h"
#pragma hdrstop

#include "PathTraceUnifiedLight.h"
#include "PathTraceDoomLights.h"
#include "PathTraceEmissiveCandidates.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace {

float Max3(const float x, const float y, const float z)
{
    return std::max(std::max(x, y), z);
}

float Luminance(const float rgb[3])
{
    return rgb[0] * 0.2126f + rgb[1] * 0.7152f + rgb[2] * 0.0722f;
}

uint32_t EmissiveLookupHash(uint32_t instanceId, uint32_t primitiveIndex)
{
    uint32_t value = instanceId ^ (primitiveIndex + 0x9e3779b9u +
        (instanceId << 6u) + (instanceId >> 2u));
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

uint64_t EmissiveLookupHashValue(uint64_t hash, uint64_t value)
{
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
    return hash;
}

uint32_t NextPowerOfTwoAtLeastTwo(uint64_t value)
{
    value = std::max<uint64_t>(value, 2u);
    if (value > (uint64_t(1u) << 31u))
    {
        return 0u;
    }
    uint32_t result = 2u;
    while (result < value)
    {
        result <<= 1u;
    }
    return result;
}

bool DoomAnalyticIdentitySampleable(const PathTraceDoomAnalyticLightCandidateIdentity& identity)
{
    return identity.universeIndex != PATH_TRACE_DOOM_ANALYTIC_LIGHT_INVALID_INDEX &&
        identity.remapIndex != PATH_TRACE_DOOM_ANALYTIC_LIGHT_INVALID_INDEX &&
        (identity.flags & PATH_TRACE_DOOM_ANALYTIC_IDENTITY_VALID) != 0u &&
        (identity.flags & PATH_TRACE_DOOM_ANALYTIC_IDENTITY_SAMPLEABLE) != 0u;
}

bool DoomAnalyticRemapValid(const PathTraceDoomAnalyticLightRemap& remap)
{
    return (remap.flags & PATH_TRACE_DOOM_ANALYTIC_IDENTITY_REMAP_VALID) != 0u;
}

bool EmissiveRemapValid(const PathTraceEmissiveLightRemap& remap)
{
    return (remap.flags & RT_SMOKE_EMISSIVE_REMAP_VALID) != 0u;
}

bool Finite3(const float value[3])
{
    return std::isfinite(value[0]) && std::isfinite(value[1]) && std::isfinite(value[2]);
}

bool Finite4(const float value[4])
{
    return std::isfinite(value[0]) &&
        std::isfinite(value[1]) &&
        std::isfinite(value[2]) &&
        std::isfinite(value[3]);
}

float DoomAnalyticLightColorMagnitude(const PathTraceDoomAnalyticLightCandidate& light)
{
    return Max3(
        std::max(light.colorAndIntensity[0], 0.0f),
        std::max(light.colorAndIntensity[1], 0.0f),
        std::max(light.colorAndIntensity[2], 0.0f));
}

bool DoomAnalyticLightStateCompatible(
    const PathTraceDoomAnalyticLightCandidate& currentLight,
    const PathTraceDoomAnalyticLightCandidate& previousLight,
    float tolerance)
{
    tolerance = std::max(0.0f, std::min(tolerance, 1.0f));
    const float currentColor[3] = {
        std::max(currentLight.colorAndIntensity[0], 0.0f),
        std::max(currentLight.colorAndIntensity[1], 0.0f),
        std::max(currentLight.colorAndIntensity[2], 0.0f)
    };
    const float previousColor[3] = {
        std::max(previousLight.colorAndIntensity[0], 0.0f),
        std::max(previousLight.colorAndIntensity[1], 0.0f),
        std::max(previousLight.colorAndIntensity[2], 0.0f)
    };
    const float colorScale = std::max(std::max(DoomAnalyticLightColorMagnitude(currentLight), DoomAnalyticLightColorMagnitude(previousLight)), 1.0e-4f);
    for (int i = 0; i < 3; ++i)
    {
        if (std::fabs(currentColor[i] - previousColor[i]) > colorScale * tolerance + 1.0e-4f)
        {
            return false;
        }
    }

    const float currentIntensity = std::max(currentLight.colorAndIntensity[3], 0.0f);
    const float previousIntensity = std::max(previousLight.colorAndIntensity[3], 0.0f);
    const float intensityScale = std::max(std::max(currentIntensity, previousIntensity), 1.0e-4f);
    if (std::fabs(currentIntensity - previousIntensity) > intensityScale * tolerance + 1.0e-4f)
    {
        return false;
    }

    const float currentInfluenceRadius = std::max(currentLight.doomRadiusAndArea[0], 1.0f);
    const float previousInfluenceRadius = std::max(previousLight.doomRadiusAndArea[0], 1.0f);
    const float positionTolerance = std::max(std::max(currentInfluenceRadius, previousInfluenceRadius) * 0.005f, 0.5f);
    const float dx = currentLight.originAndRadius[0] - previousLight.originAndRadius[0];
    const float dy = currentLight.originAndRadius[1] - previousLight.originAndRadius[1];
    const float dz = currentLight.originAndRadius[2] - previousLight.originAndRadius[2];
    if (std::sqrt(dx * dx + dy * dy + dz * dz) > positionTolerance)
    {
        return false;
    }

    const float radiusScale = std::max(std::max(std::fabs(currentLight.originAndRadius[3]), std::fabs(previousLight.originAndRadius[3])), 1.0e-4f);
    if (std::fabs(currentLight.originAndRadius[3] - previousLight.originAndRadius[3]) > radiusScale * tolerance + 1.0e-4f)
    {
        return false;
    }

    return true;
}

bool EmissiveTriangleInputReplayable(const PathTraceSmokeEmissiveTriangle& emissiveTriangle)
{
    if (!Finite4(emissiveTriangle.centerAndArea) ||
        !Finite4(emissiveTriangle.normalAndLuminance) ||
        !Finite4(emissiveTriangle.estimatedRadianceAndLuminance) ||
        !Finite4(emissiveTriangle.sampleWeightAndPdf))
    {
        return false;
    }

    const float area = emissiveTriangle.centerAndArea[3];
    if (area <= 1.0e-6f)
    {
        return false;
    }

    const float radiance[3] = {
        std::max(emissiveTriangle.estimatedRadianceAndLuminance[0], 0.0f),
        std::max(emissiveTriangle.estimatedRadianceAndLuminance[1], 0.0f),
        std::max(emissiveTriangle.estimatedRadianceAndLuminance[2], 0.0f)
    };
    const float luminance = std::max(emissiveTriangle.estimatedRadianceAndLuminance[3], Luminance(radiance));
    if (luminance <= 0.0f)
    {
        return false;
    }

    const float sourceWeight = std::max(
        std::max(emissiveTriangle.sampleWeightAndPdf[0], luminance),
        area > 1.0e-6f ? 1.0e-6f : 0.0f);
    return sourceWeight > 0.0f;
}

PathTraceUnifiedLightRecord BuildUnifiedEmissiveLightRecord(
    const PathTraceSmokeEmissiveTriangle& emissiveTriangle,
    uint32_t sourceIndex)
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
    record.radianceAndLuminance[3] = std::max(emissiveTriangle.estimatedRadianceAndLuminance[3], Luminance(record.radianceAndLuminance));
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
    return record;
}

PathTraceUnifiedLightRecord BuildUnifiedDoomAnalyticLightRecord(
    const PathTraceDoomAnalyticLightCandidate& analyticLight,
    const PathTraceDoomAnalyticLightCandidateIdentity* identity,
    uint32_t sourceIndex)
{
    const float rawRadius = analyticLight.originAndRadius[3];
    const float rawInfluenceRadius = analyticLight.doomRadiusAndArea[0];
    const float radius = std::isfinite(rawRadius) && rawRadius > 0.0f ? rawRadius : 0.0f;
    const float influenceRadius = std::isfinite(rawInfluenceRadius) && rawInfluenceRadius > 0.0f ? rawInfluenceRadius : 0.0f;
    const bool finitePosition = Finite3(analyticLight.originAndRadius);

    PathTraceUnifiedLightRecord record;
    record.positionAndRadius[0] = finitePosition ? analyticLight.originAndRadius[0] : 0.0f;
    record.positionAndRadius[1] = finitePosition ? analyticLight.originAndRadius[1] : 0.0f;
    record.positionAndRadius[2] = finitePosition ? analyticLight.originAndRadius[2] : 0.0f;
    record.positionAndRadius[3] = radius;
    record.normalAndArea[0] = 0.0f;
    record.normalAndArea[1] = 0.0f;
    record.normalAndArea[2] = 1.0f;
    record.normalAndArea[3] = 4.0f * idMath::PI * radius * radius;
    record.radianceAndLuminance[0] = std::max(analyticLight.colorAndIntensity[0], 0.0f);
    record.radianceAndLuminance[1] = std::max(analyticLight.colorAndIntensity[1], 0.0f);
    record.radianceAndLuminance[2] = std::max(analyticLight.colorAndIntensity[2], 0.0f);
    record.radianceAndLuminance[3] = Luminance(record.radianceAndLuminance);
    record.uvOrDoomParams[0] = influenceRadius;
    record.uvOrDoomParams[1] = record.normalAndArea[3];
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
    const bool positiveRadiance = record.radianceAndLuminance[3] > 0.0f &&
        std::isfinite(record.radianceAndLuminance[0]) &&
        std::isfinite(record.radianceAndLuminance[1]) &&
        std::isfinite(record.radianceAndLuminance[2]);
    const bool sampleablePayload = finitePosition && radius > 0.0f && influenceRadius > 0.0f && positiveRadiance;
    if (!sampleablePayload)
    {
        record.flags &= ~PATH_TRACE_DOOM_ANALYTIC_IDENTITY_SAMPLEABLE;
    }
    record.sourceWeight = sampleablePayload
        ? Max3(record.radianceAndLuminance[0], record.radianceAndLuminance[1], record.radianceAndLuminance[2]) * record.normalAndArea[3] * influenceRadius
        : 0.0f;
    return record;
}

} // namespace

PathTraceUnifiedLightBuild BuildPathTraceUnifiedLights(
    const std::vector<PathTraceSmokeEmissiveTriangle>& currentEmissiveTriangles,
    const std::vector<PathTraceSmokeEmissiveTriangle>& previousEmissiveTriangles,
    const std::vector<PathTraceEmissiveLightRemap>& emissiveRemap,
    const std::vector<PathTraceDoomAnalyticLightCandidate>& currentAnalyticLights,
    const std::vector<PathTraceDoomAnalyticLightCandidate>& previousAnalyticLights,
    const std::vector<PathTraceDoomAnalyticLightCandidateIdentity>& currentAnalyticIdentities,
    const std::vector<PathTraceDoomAnalyticLightCandidateIdentity>& previousAnalyticIdentities,
    const std::vector<PathTraceDoomAnalyticLightRemap>& analyticRemap,
    float analyticStateCompatibilityTolerance)
{
    PathTraceUnifiedLightBuild build;
    build.currentLights.reserve(currentEmissiveTriangles.size() + currentAnalyticLights.size());
    build.previousLights.reserve(previousEmissiveTriangles.size() + previousAnalyticLights.size());

    std::vector<uint32_t> currentEmissiveSourceToDense(currentEmissiveTriangles.size(), PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX);
    std::vector<uint32_t> previousEmissiveSourceToDense(previousEmissiveTriangles.size(), PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX);
    for (uint32_t emissiveIndex = 0; emissiveIndex < currentEmissiveTriangles.size(); ++emissiveIndex)
    {
        if (!EmissiveTriangleInputReplayable(currentEmissiveTriangles[emissiveIndex]))
        {
            continue;
        }
        currentEmissiveSourceToDense[emissiveIndex] = static_cast<uint32_t>(build.currentLights.size());
        build.currentLights.push_back(BuildUnifiedEmissiveLightRecord(currentEmissiveTriangles[emissiveIndex], emissiveIndex));
    }
    build.currentEmissiveLightCount = static_cast<uint32_t>(build.currentLights.size());
    for (uint32_t analyticIndex = 0; analyticIndex < currentAnalyticLights.size(); ++analyticIndex)
    {
        const PathTraceDoomAnalyticLightCandidateIdentity* identity =
            analyticIndex < currentAnalyticIdentities.size() ? &currentAnalyticIdentities[analyticIndex] : nullptr;
        build.currentLights.push_back(BuildUnifiedDoomAnalyticLightRecord(currentAnalyticLights[analyticIndex], identity, analyticIndex));
    }
    build.currentAnalyticLightCount = static_cast<uint32_t>(build.currentLights.size()) - build.currentEmissiveLightCount;

    for (uint32_t emissiveIndex = 0; emissiveIndex < previousEmissiveTriangles.size(); ++emissiveIndex)
    {
        if (!EmissiveTriangleInputReplayable(previousEmissiveTriangles[emissiveIndex]))
        {
            continue;
        }
        previousEmissiveSourceToDense[emissiveIndex] = static_cast<uint32_t>(build.previousLights.size());
        build.previousLights.push_back(BuildUnifiedEmissiveLightRecord(previousEmissiveTriangles[emissiveIndex], emissiveIndex));
    }
    build.previousEmissiveLightCount = static_cast<uint32_t>(build.previousLights.size());
    for (uint32_t analyticIndex = 0; analyticIndex < previousAnalyticLights.size(); ++analyticIndex)
    {
        const PathTraceDoomAnalyticLightCandidateIdentity* identity =
            analyticIndex < previousAnalyticIdentities.size() ? &previousAnalyticIdentities[analyticIndex] : nullptr;
        build.previousLights.push_back(BuildUnifiedDoomAnalyticLightRecord(previousAnalyticLights[analyticIndex], identity, analyticIndex));
    }
    build.previousAnalyticLightCount = static_cast<uint32_t>(build.previousLights.size()) - build.previousEmissiveLightCount;

    build.currentToPreviousRemap.assign(build.currentLights.size(), PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX);
    for (uint32_t emissiveIndex = 0; emissiveIndex < currentEmissiveTriangles.size(); ++emissiveIndex)
    {
        if (emissiveIndex >= emissiveRemap.size() || !EmissiveRemapValid(emissiveRemap[emissiveIndex]) || emissiveRemap[emissiveIndex].currentToPreviousIndex < 0)
        {
            continue;
        }

        const uint32_t previousIndex = static_cast<uint32_t>(emissiveRemap[emissiveIndex].currentToPreviousIndex);
        const uint32_t currentDenseIndex = currentEmissiveSourceToDense[emissiveIndex];
        const uint32_t previousDenseIndex = previousIndex < previousEmissiveSourceToDense.size()
            ? previousEmissiveSourceToDense[previousIndex]
            : PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX;
        if (currentDenseIndex < build.currentToPreviousRemap.size() && previousDenseIndex < build.previousLights.size())
        {
            build.currentToPreviousRemap[currentDenseIndex] = previousDenseIndex;
            build.currentLights[currentDenseIndex].previousIndex = previousDenseIndex;
        }
    }

    for (uint32_t analyticIndex = 0; analyticIndex < currentAnalyticLights.size(); ++analyticIndex)
    {
        if (analyticIndex >= currentAnalyticIdentities.size())
        {
            continue;
        }

        const PathTraceDoomAnalyticLightCandidateIdentity& identity = currentAnalyticIdentities[analyticIndex];
        if (!DoomAnalyticIdentitySampleable(identity) || identity.remapIndex >= analyticRemap.size())
        {
            continue;
        }

        const PathTraceDoomAnalyticLightRemap& remap = analyticRemap[identity.remapIndex];
        if (!DoomAnalyticRemapValid(remap) || remap.currentToPreviousCandidateIndex < 0)
        {
            continue;
        }

        const uint32_t previousAnalyticIndex = static_cast<uint32_t>(remap.currentToPreviousCandidateIndex);
        if (previousAnalyticIndex >= previousAnalyticLights.size())
        {
            continue;
        }
        if (!DoomAnalyticLightStateCompatible(currentAnalyticLights[analyticIndex], previousAnalyticLights[previousAnalyticIndex], analyticStateCompatibilityTolerance))
        {
            continue;
        }

        const uint32_t unifiedIndex = build.currentEmissiveLightCount + analyticIndex;
        const uint32_t previousUnifiedIndex = build.previousEmissiveLightCount + previousAnalyticIndex;
        if (unifiedIndex < build.currentToPreviousRemap.size() && previousUnifiedIndex < build.previousLights.size())
        {
            build.currentToPreviousRemap[unifiedIndex] = previousUnifiedIndex;
            build.currentLights[unifiedIndex].previousIndex = previousUnifiedIndex;
        }
    }

    return build;
}

PathTraceUnifiedEmissiveLookupBuild BuildPathTraceUnifiedEmissiveLookup(
    const std::vector<PathTraceUnifiedLightRecord>& currentLights,
    const std::vector<PathTraceEmissiveDistributionEntry>& emissiveDistribution,
    uint32_t emissiveRangeStart,
    uint32_t emissiveRangeCount)
{
    PathTraceUnifiedEmissiveLookupBuild build;
    const uint64_t rangeEnd = uint64_t(emissiveRangeStart) +
        uint64_t(emissiveRangeCount);
    if (rangeEnd > currentLights.size())
    {
        build.entries.resize(2u);
        return build;
    }

    const uint64_t requestedCapacities[2] = {
        uint64_t(emissiveRangeCount) * 2u,
        uint64_t(emissiveRangeCount) * 4u
    };
    for (uint32_t capacityAttempt = 0u; capacityAttempt < 2u; ++capacityAttempt)
    {
        const uint32_t capacity = NextPowerOfTwoAtLeastTwo(
            requestedCapacities[capacityAttempt]);
        if (capacity == 0u || uint64_t(capacity) >
                std::numeric_limits<size_t>::max() /
                    sizeof(PathTraceUnifiedEmissiveLookupEntry))
        {
            break;
        }

        std::vector<PathTraceUnifiedEmissiveLookupEntry> entries(capacity);
        std::vector<float> conditionalIdentityPdfs(emissiveRangeCount, 0.0f);
        bool exact = true;
        float previousCdf = 0.0f;
        for (const PathTraceEmissiveDistributionEntry& distributionEntry :
             emissiveDistribution)
        {
            if (distributionEntry.denseLightIndex < emissiveRangeStart ||
                distributionEntry.denseLightIndex - emissiveRangeStart >=
                    emissiveRangeCount ||
                !std::isfinite(distributionEntry.cumulativePdf) ||
                distributionEntry.cumulativePdf < previousCdf)
            {
                exact = false;
                break;
            }
            const uint32_t localIndex =
                distributionEntry.denseLightIndex - emissiveRangeStart;
            if (conditionalIdentityPdfs[localIndex] != 0.0f)
            {
                exact = false;
                break;
            }
            conditionalIdentityPdfs[localIndex] =
                distributionEntry.cumulativePdf - previousCdf;
            previousCdf = distributionEntry.cumulativePdf;
        }
        if (!exact)
        {
            continue;
        }
        const uint32_t mask = capacity - 1u;
        for (uint32_t localIndex = 0u; localIndex < emissiveRangeCount; ++localIndex)
        {
            const uint32_t denseIndex = emissiveRangeStart + localIndex;
            const PathTraceUnifiedLightRecord& light = currentLights[denseIndex];
            if (light.type != PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
            {
                exact = false;
                break;
            }

            const uint32_t firstSlot = EmissiveLookupHash(
                light.instanceId, light.primitiveIndex) & mask;
            bool inserted = false;
            for (uint32_t probe = 0u;
                 probe < PATH_TRACE_UNIFIED_EMISSIVE_LOOKUP_MAX_PROBES;
                 ++probe)
            {
                PathTraceUnifiedEmissiveLookupEntry& entry =
                    entries[(firstSlot + probe) & mask];
                if (entry.occupied == 0u)
                {
                    entry.instanceId = light.instanceId;
                    entry.primitiveIndex = light.primitiveIndex;
                    entry.denseLightIndex = denseIndex;
                    entry.occupied = 1u;
                    entry.conditionalIdentityPdf =
                        conditionalIdentityPdfs[localIndex];
                    inserted = true;
                    break;
                }
                if (entry.instanceId == light.instanceId &&
                    entry.primitiveIndex == light.primitiveIndex)
                {
                    // Two manager records for one trace identity make the
                    // alternate-technique PDF ambiguous. Refuse biased MIS.
                    inserted = false;
                    break;
                }
            }
            if (!inserted)
            {
                exact = false;
                break;
            }
        }
        if (!exact)
        {
            continue;
        }

        uint64_t signature = 1469598103934665603ull;
        signature = EmissiveLookupHashValue(signature, capacity);
        signature = EmissiveLookupHashValue(signature, emissiveRangeStart);
        signature = EmissiveLookupHashValue(signature, emissiveRangeCount);
        for (const PathTraceUnifiedEmissiveLookupEntry& entry : entries)
        {
            signature = EmissiveLookupHashValue(signature, entry.instanceId);
            signature = EmissiveLookupHashValue(signature, entry.primitiveIndex);
            signature = EmissiveLookupHashValue(signature, entry.denseLightIndex);
            signature = EmissiveLookupHashValue(signature, entry.occupied);
            uint32_t conditionalIdentityPdfBits = 0u;
            std::memcpy(&conditionalIdentityPdfBits,
                &entry.conditionalIdentityPdf,
                sizeof(conditionalIdentityPdfBits));
            signature = EmissiveLookupHashValue(
                signature, conditionalIdentityPdfBits);
        }
        build.entries.swap(entries);
        build.signature = signature;
        build.exact = true;
        return build;
    }

    build.entries.assign(2u, PathTraceUnifiedEmissiveLookupEntry());
    return build;
}
