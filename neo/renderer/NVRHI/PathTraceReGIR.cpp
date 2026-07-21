#include "precompiled.h"
#pragma hdrstop

#include "PathTraceReGIR.h"
#include "PathTraceCVars.h"

#include <algorithm>
#include <limits>

namespace {

struct PathTraceReGIRCandidateRecord
{
    uint32_t lightIndex;
    uint32_t lightClass;
    float sourcePdf;
    float invSourcePdf;
    uint32_t cellIndex;
    uint32_t slotIndex;
    uint32_t flags;
    uint32_t globalIdentity;
};

static_assert(sizeof(PathTraceReGIRCandidateRecord) == 32, "ReGIR candidate cache shell stride must stay explicit");

uint32_t ClampCVarUInt(idCVar& cvar, int minValue, int maxValue)
{
    return static_cast<uint32_t>(idMath::ClampInt(minValue, maxValue, cvar.GetInteger()));
}

uint32_t ActiveDomainCount(const PathTraceReGIRSettings& settings, const PathTraceReGIRLightCounts& lightCounts)
{
    if (settings.lightDomain == 0)
    {
        return lightCounts.analyticCount;
    }
    if (settings.lightDomain == 1)
    {
        return lightCounts.emissiveCount;
    }
    return lightCounts.analyticCount + lightCounts.emissiveCount;
}

bool BufferMatches(const nvrhi::BufferHandle& buffer, const PathTraceReGIRResourceDesc& desc)
{
    if (!buffer || !desc.structuralValid || desc.candidateBytes == 0)
    {
        return false;
    }

    const nvrhi::BufferDesc& bufferDesc = buffer->getDesc();
    return
        bufferDesc.byteSize >= desc.candidateBytes &&
        bufferDesc.structStride == desc.candidateStride &&
        bufferDesc.canHaveUAVs;
}

bool PlaceholderBufferMatches(const nvrhi::BufferHandle& buffer)
{
    if (!buffer)
    {
        return false;
    }

    const nvrhi::BufferDesc& bufferDesc = buffer->getDesc();
    return
        bufferDesc.byteSize >= 256 &&
        bufferDesc.structStride == sizeof(uint32_t) &&
        !bufferDesc.canHaveUAVs;
}

}

PathTraceReGIRSettings BuildPathTraceReGIRSettingsFromCVars()
{
    PathTraceReGIRSettings settings;
    settings.enabled = r_pathTracingReGIREnable.GetInteger() != 0;
    settings.debugView = idMath::ClampInt(0, 10, r_pathTracingReGIRDebugView.GetInteger());
    settings.mode = idMath::ClampInt(0, 2, r_pathTracingReGIRMode.GetInteger());
    settings.cellSize = idMath::ClampFloat(16.0f, 8192.0f, r_pathTracingReGIRCellSize.GetFloat());
    settings.gridX = ClampCVarUInt(r_pathTracingReGIRGridX, 1, 128);
    settings.gridY = ClampCVarUInt(r_pathTracingReGIRGridY, 1, 128);
    settings.gridZ = ClampCVarUInt(r_pathTracingReGIRGridZ, 1, 64);
    settings.lightsPerCell = ClampCVarUInt(r_pathTracingReGIRLightsPerCell, 1, 512);
    settings.buildSamples = ClampCVarUInt(r_pathTracingReGIRBuildSamples, 1, 64);
    settings.lightDomain = idMath::ClampInt(0, 2, r_pathTracingReGIRLightDomain.GetInteger());
    settings.centerMode = idMath::ClampInt(0, 2, r_pathTracingReGIRCenterMode.GetInteger());
    settings.manualCenter[0] = r_pathTracingReGIRManualCenterX.GetFloat();
    settings.manualCenter[1] = r_pathTracingReGIRManualCenterY.GetFloat();
    settings.manualCenter[2] = r_pathTracingReGIRManualCenterZ.GetFloat();
    if (!settings.enabled)
    {
        settings.debugView = 0;
    }
    if (settings.mode == 0)
    {
        settings.enabled = false;
        settings.debugView = 0;
    }
    return settings;
}

PathTraceReGIRResourceDesc BuildPathTraceReGIRResourceDesc(const PathTraceReGIRSettings& settings, const PathTraceReGIRLightCounts& lightCounts)
{
    PathTraceReGIRResourceDesc desc;
    desc.requested = settings.enabled;
    desc.candidateStride = static_cast<uint32_t>(sizeof(PathTraceReGIRCandidateRecord));

    if (!settings.enabled)
    {
        desc.firstMissingContract = "disabled";
        return desc;
    }
    if (settings.mode < 1 || settings.mode > 2)
    {
        desc.firstMissingContract = "invalid-mode";
        return desc;
    }
    if (settings.cellSize <= 0.0f || settings.gridX == 0 || settings.gridY == 0 || settings.gridZ == 0)
    {
        desc.firstMissingContract = "invalid-cell-grid";
        return desc;
    }
    if (settings.lightsPerCell == 0 || settings.buildSamples == 0)
    {
        desc.firstMissingContract = "invalid-sample-budget";
        return desc;
    }

    const uint64_t cellCount64 =
        static_cast<uint64_t>(settings.gridX) *
        static_cast<uint64_t>(settings.gridY) *
        static_cast<uint64_t>(settings.gridZ);
    const uint64_t slotCount64 = cellCount64 * static_cast<uint64_t>(settings.lightsPerCell);
    const uint64_t bytes64 = slotCount64 * static_cast<uint64_t>(desc.candidateStride);
    if (cellCount64 == 0 || cellCount64 > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) ||
        slotCount64 == 0 || slotCount64 > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
    {
        desc.firstMissingContract = "resource-size-overflow";
        return desc;
    }

    desc.cellCount = static_cast<uint32_t>(cellCount64);
    desc.slotCount = static_cast<uint32_t>(slotCount64);
    desc.candidateBytes = bytes64;
    desc.structuralValid = true;

    if (settings.debugView == 5 && settings.lightDomain == 1)
    {
        desc.firstMissingContract = "analytic-domain-required-regir-03";
    }
    else if (settings.debugView == 6 && settings.lightDomain == 0)
    {
        desc.firstMissingContract = "emissive-domain-required-regir-04";
    }
    else if (settings.debugView >= 4 && settings.debugView <= 10)
    {
        if (settings.lightDomain == 0)
        {
            if (lightCounts.analyticCount == 0)
            {
                desc.firstMissingContract = "no-current-analytic-domain";
            }
            else
            {
                desc.firstMissingContract = "none";
            }
        }
        else if (settings.lightDomain == 1)
        {
            if (lightCounts.emissiveCount == 0)
            {
                desc.firstMissingContract = "no-current-emissive-domain";
            }
            else
            {
                desc.firstMissingContract = "none";
            }
        }
        else
        {
            if (lightCounts.analyticCount == 0 && lightCounts.emissiveCount == 0)
            {
                desc.firstMissingContract = "no-current-light-domain";
            }
            else if (lightCounts.analyticCount == 0)
            {
                desc.firstMissingContract = "split-domain-missing-analytic";
            }
            else if (lightCounts.emissiveCount == 0)
            {
                desc.firstMissingContract = "split-domain-missing-emissive";
            }
            else
            {
                desc.firstMissingContract = "none";
            }
        }
    }
    else if (settings.debugView >= 1 && settings.debugView <= 3)
    {
        desc.firstMissingContract = "none";
    }
    else if (ActiveDomainCount(settings, lightCounts) == 0)
    {
        desc.firstMissingContract = "no-current-light-domain";
    }
    else
    {
        desc.firstMissingContract = "candidate-build-not-implemented-regir-03";
    }
    return desc;
}

void PathTraceReGIRState::Clear()
{
    candidateCacheBuffer = nullptr;
    placeholderSrvBuffer = nullptr;
    settings = PathTraceReGIRSettings();
    resourceDesc = PathTraceReGIRResourceDesc();
    allocationSerial = 0;
}

bool PathTraceReGIRState::EnsureResources(nvrhi::IDevice* device, const PathTraceReGIRSettings& nextSettings, const PathTraceReGIRResourceDesc& nextDesc)
{
    settings = nextSettings;
    resourceDesc = nextDesc;

    if (!nextDesc.requested || !nextDesc.structuralValid || nextDesc.candidateBytes == 0)
    {
        candidateCacheBuffer = nullptr;
        placeholderSrvBuffer = nullptr;
        return true;
    }

    if (BufferMatches(candidateCacheBuffer, nextDesc) && PlaceholderBufferMatches(placeholderSrvBuffer))
    {
        return true;
    }

    if (!device)
    {
        candidateCacheBuffer = nullptr;
        placeholderSrvBuffer = nullptr;
        return false;
    }

    bool allocated = false;
    if (!BufferMatches(candidateCacheBuffer, nextDesc))
    {
        candidateCacheBuffer = nullptr;
        nvrhi::BufferDesc candidateDesc;
        candidateDesc.debugName = "PathTraceReGIRCandidateCache";
        candidateDesc.byteSize = nextDesc.candidateBytes;
        candidateDesc.structStride = nextDesc.candidateStride;
        candidateDesc.canHaveUAVs = true;
        candidateDesc.canHaveTypedViews = false;
        candidateDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        candidateDesc.keepInitialState = true;
        candidateCacheBuffer = device->createBuffer(candidateDesc);
        allocated = true;
    }

    if (!PlaceholderBufferMatches(placeholderSrvBuffer))
    {
        placeholderSrvBuffer = nullptr;
        nvrhi::BufferDesc placeholderDesc;
        placeholderDesc.debugName = "PathTraceReGIRPlaceholderSRV";
        placeholderDesc.byteSize = 256;
        placeholderDesc.structStride = sizeof(uint32_t);
        placeholderDesc.canHaveUAVs = false;
        placeholderDesc.canHaveTypedViews = false;
        placeholderDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        placeholderDesc.keepInitialState = true;
        placeholderSrvBuffer = device->createBuffer(placeholderDesc);
        allocated = true;
    }

    if (allocated && candidateCacheBuffer && placeholderSrvBuffer)
    {
        ++allocationSerial;
    }
    return candidateCacheBuffer != nullptr && placeholderSrvBuffer != nullptr;
}
