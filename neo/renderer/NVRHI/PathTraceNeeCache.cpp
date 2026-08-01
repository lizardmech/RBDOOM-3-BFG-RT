#include "precompiled.h"
#pragma hdrstop

#include "PathTraceNeeCache.h"
#include "PathTraceCVars.h"

#include <limits>

namespace {

uint32_t ClampCVarUInt(idCVar& cvar, int minValue, int maxValue)
{
    return static_cast<uint32_t>(idMath::ClampInt(minValue, maxValue, cvar.GetInteger()));
}

bool BufferMatches(const nvrhi::BufferHandle& buffer, uint64_t byteSize, uint32_t structStride)
{
    if (!buffer || byteSize == 0u || structStride == 0u)
    {
        return false;
    }

    const nvrhi::BufferDesc& bufferDesc = buffer->getDesc();
    return
        bufferDesc.byteSize >= byteSize &&
        bufferDesc.structStride == structStride &&
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

nvrhi::BufferHandle CreateNeeCacheUavBuffer(nvrhi::IDevice* device, const char* debugName, uint64_t byteSize, uint32_t structStride)
{
    if (!device || byteSize == 0u || structStride == 0u)
    {
        return nullptr;
    }

    nvrhi::BufferDesc desc;
    desc.debugName = debugName;
    desc.byteSize = byteSize;
    desc.structStride = structStride;
    desc.canHaveUAVs = true;
    desc.canHaveTypedViews = false;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    return device->createBuffer(desc);
}

nvrhi::BufferHandle CreateNeeCachePlaceholderSrvBuffer(nvrhi::IDevice* device)
{
    if (!device)
    {
        return nullptr;
    }

    nvrhi::BufferDesc desc;
    desc.debugName = "PathTraceNeeCachePlaceholderSRV";
    desc.byteSize = 256;
    desc.structStride = sizeof(uint32_t);
    desc.canHaveUAVs = false;
    desc.canHaveTypedViews = false;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    return device->createBuffer(desc);
}

}

PathTraceNeeCacheSettings BuildPathTraceNeeCacheSettingsFromCVars()
{
    PathTraceNeeCacheSettings settings;
    settings.enabled = r_pathTracingNeeCacheEnable.GetInteger() != 0;
    settings.mode = idMath::ClampInt(0, 3, r_pathTracingNeeCacheMode.GetInteger());
    settings.debugView = idMath::ClampInt(0, 12, r_pathTracingNeeCacheDebugView.GetInteger());
    settings.cellResolution = idMath::ClampInt(1, 4096, r_pathTracingNeeCacheCellResolution.GetInteger());
    settings.minRange = idMath::ClampFloat(1.0f, 65536.0f, r_pathTracingNeeCacheMinRange.GetFloat());
    settings.cellCount = ClampCVarUInt(r_pathTracingNeeCacheCellCount, 1, 1048576);
    settings.candidateSlots = ClampCVarUInt(r_pathTracingNeeCacheCandidateSlots, 1, 256);
    settings.taskSlots = ClampCVarUInt(r_pathTracingNeeCacheTaskSlots, 1, 256);
    settings.fallbackProbability = idMath::ClampFloat(0.0f, 1.0f, r_pathTracingNeeCacheFallbackProbability.GetFloat());
    settings.sourceDomain = idMath::ClampInt(0, 3, r_pathTracingNeeCacheSourceDomain.GetInteger());

    if (!settings.enabled || settings.mode == 0)
    {
        settings.enabled = false;
        settings.debugView = 0;
    }
    return settings;
}

PathTraceNeeCacheResourceDesc BuildPathTraceNeeCacheResourceDesc(const PathTraceNeeCacheSettings& settings)
{
    PathTraceNeeCacheResourceDesc desc;
    desc.requested = settings.enabled;
    desc.providerResultStride = static_cast<uint32_t>(sizeof(PathTraceNeeCacheProviderResult));
    desc.cellStride = static_cast<uint32_t>(sizeof(PathTraceNeeCacheCellRecord));
    desc.taskStride = static_cast<uint32_t>(sizeof(PathTraceNeeCacheTaskRecord));
    desc.candidateStride = static_cast<uint32_t>(sizeof(PathTraceNeeCacheCandidateRecord));

    if (!settings.enabled)
    {
        return desc;
    }
    if (settings.mode < 1 || settings.mode > 3)
    {
        return desc;
    }
    if (settings.cellResolution <= 0 || settings.minRange <= 0.0f || settings.cellCount == 0u)
    {
        return desc;
    }
    if (settings.candidateSlots == 0u || settings.taskSlots == 0u)
    {
        return desc;
    }

    const uint64_t cellCount64 = static_cast<uint64_t>(settings.cellCount);
    const uint64_t taskCount64 = cellCount64 * static_cast<uint64_t>(settings.taskSlots);
    const uint64_t candidateCount64 = cellCount64 * static_cast<uint64_t>(settings.candidateSlots);
    if (taskCount64 > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) ||
        candidateCount64 > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
    {
        return desc;
    }

    desc.providerResultCount = settings.cellCount;
    desc.cellCount = settings.cellCount;
    desc.taskCount = static_cast<uint32_t>(taskCount64);
    desc.candidateCount = static_cast<uint32_t>(candidateCount64);
    desc.providerResultBytes = static_cast<uint64_t>(desc.providerResultCount) * desc.providerResultStride;
    desc.cellBytes = static_cast<uint64_t>(desc.cellCount) * desc.cellStride;
    desc.taskBytes = static_cast<uint64_t>(desc.taskCount) * desc.taskStride;
    desc.candidateBytes = static_cast<uint64_t>(desc.candidateCount) * desc.candidateStride;
    desc.totalBytes = desc.providerResultBytes + desc.cellBytes + desc.taskBytes + desc.candidateBytes;
    desc.structuralValid =
        desc.providerResultBytes != 0u &&
        desc.cellBytes != 0u &&
        desc.taskBytes != 0u &&
        desc.candidateBytes != 0u;

    return desc;
}

const char* PathTraceNeeCacheSourceDomainName(int sourceDomain)
{
    switch (sourceDomain)
    {
    case 0:
        return "all-current-rlu";
    case 1:
        return "emissive-range-only";
    case 2:
        return "analytic-range-only";
    case 3:
        return "typed-candidate-lists";
    default:
        return "invalid";
    }
}

void PathTraceNeeCacheState::Clear()
{
    providerResultBuffer = nullptr;
    cellBuffer = nullptr;
    taskBuffer = nullptr;
    candidateBuffer = nullptr;
    placeholderSrvBuffer = nullptr;
    settings = PathTraceNeeCacheSettings();
    resourceDesc = PathTraceNeeCacheResourceDesc();
    allocationSerial = 0u;
    invalidationSerial = 0u;
    observedRluStructuralSignature = 0u;
    observedRluMappingSignature = 0u;
    observedRluPayloadSignature = 0u;
    pendingInvalidationFlags = PATH_TRACE_NEE_CACHE_INVALIDATE_NONE;
    lastInvalidationFlags = PATH_TRACE_NEE_CACHE_INVALIDATE_NONE;
    cleanProviderStartupDelayFrames = 0u;
    cleanProviderStartupRefreshFrames = 0u;
    cleanProviderStableViewFrames = 0u;
    cleanProviderLastViewOrigin[0] = cleanProviderLastViewOrigin[1] = cleanProviderLastViewOrigin[2] = 0.0f;
    cleanProviderLastViewForward[0] = cleanProviderLastViewForward[1] = cleanProviderLastViewForward[2] = 0.0f;
    observedRluSignaturesValid = false;
    taskClearPending = false;
    cleanProviderSnapshotHoldActive = false;
    cleanProviderRequestedLastFrame = false;
    cleanProviderLastViewValid = false;
    secondaryVisualSnapshotHoldActive = false;
    secondaryVisualBandActiveLastFrame = false;
}

bool PathTraceNeeCacheState::EnsureResources(nvrhi::IDevice* device, const PathTraceNeeCacheSettings& nextSettings, const PathTraceNeeCacheResourceDesc& nextDesc)
{
    settings = nextSettings;
    resourceDesc = nextDesc;

    if (!nextDesc.requested || !nextDesc.structuralValid || nextDesc.totalBytes == 0u)
    {
        providerResultBuffer = nullptr;
        cellBuffer = nullptr;
        taskBuffer = nullptr;
        candidateBuffer = nullptr;
        // Clean DI/GI shader libraries retain statically reachable NEE-cache
        // descriptor loads even when the provider is disabled. Keep one
        // bounded SRV alive so those descriptors are always valid.
        if (!PlaceholderBufferMatches(placeholderSrvBuffer))
        {
            placeholderSrvBuffer = CreateNeeCachePlaceholderSrvBuffer(device);
        }
        observedRluStructuralSignature = 0u;
        observedRluMappingSignature = 0u;
        observedRluPayloadSignature = 0u;
        pendingInvalidationFlags = PATH_TRACE_NEE_CACHE_INVALIDATE_NONE;
        lastInvalidationFlags = PATH_TRACE_NEE_CACHE_INVALIDATE_NONE;
        cleanProviderStartupDelayFrames = 0u;
        cleanProviderStartupRefreshFrames = 0u;
        cleanProviderStableViewFrames = 0u;
        cleanProviderLastViewOrigin[0] = cleanProviderLastViewOrigin[1] = cleanProviderLastViewOrigin[2] = 0.0f;
        cleanProviderLastViewForward[0] = cleanProviderLastViewForward[1] = cleanProviderLastViewForward[2] = 0.0f;
        observedRluSignaturesValid = false;
        taskClearPending = false;
        cleanProviderSnapshotHoldActive = false;
        cleanProviderRequestedLastFrame = false;
        cleanProviderLastViewValid = false;
        secondaryVisualSnapshotHoldActive = false;
        secondaryVisualBandActiveLastFrame = false;
        return !device || placeholderSrvBuffer != nullptr;
    }

    const bool buffersMatch =
        BufferMatches(providerResultBuffer, nextDesc.providerResultBytes, nextDesc.providerResultStride) &&
        BufferMatches(cellBuffer, nextDesc.cellBytes, nextDesc.cellStride) &&
        BufferMatches(taskBuffer, nextDesc.taskBytes, nextDesc.taskStride) &&
        BufferMatches(candidateBuffer, nextDesc.candidateBytes, nextDesc.candidateStride) &&
        PlaceholderBufferMatches(placeholderSrvBuffer);
    if (buffersMatch)
    {
        return true;
    }

    if (!device)
    {
        providerResultBuffer = nullptr;
        cellBuffer = nullptr;
        taskBuffer = nullptr;
        candidateBuffer = nullptr;
        placeholderSrvBuffer = nullptr;
        return false;
    }

    bool allocated = false;
    if (!BufferMatches(providerResultBuffer, nextDesc.providerResultBytes, nextDesc.providerResultStride))
    {
        providerResultBuffer = CreateNeeCacheUavBuffer(device, "PathTraceNeeCacheProviderResult", nextDesc.providerResultBytes, nextDesc.providerResultStride);
        allocated = true;
    }
    if (!BufferMatches(cellBuffer, nextDesc.cellBytes, nextDesc.cellStride))
    {
        cellBuffer = CreateNeeCacheUavBuffer(device, "PathTraceNeeCacheCells", nextDesc.cellBytes, nextDesc.cellStride);
        allocated = true;
    }
    if (!BufferMatches(taskBuffer, nextDesc.taskBytes, nextDesc.taskStride))
    {
        taskBuffer = CreateNeeCacheUavBuffer(device, "PathTraceNeeCacheTasks", nextDesc.taskBytes, nextDesc.taskStride);
        allocated = true;
    }
    if (!BufferMatches(candidateBuffer, nextDesc.candidateBytes, nextDesc.candidateStride))
    {
        candidateBuffer = CreateNeeCacheUavBuffer(device, "PathTraceNeeCacheCandidates", nextDesc.candidateBytes, nextDesc.candidateStride);
        allocated = true;
    }
    if (!PlaceholderBufferMatches(placeholderSrvBuffer))
    {
        placeholderSrvBuffer = CreateNeeCachePlaceholderSrvBuffer(device);
        allocated = true;
    }

    const bool ready =
        providerResultBuffer != nullptr &&
        cellBuffer != nullptr &&
        taskBuffer != nullptr &&
        candidateBuffer != nullptr &&
        placeholderSrvBuffer != nullptr;
    if (allocated && ready)
    {
        ++allocationSerial;
        ++invalidationSerial;
        pendingInvalidationFlags |= PATH_TRACE_NEE_CACHE_INVALIDATE_RESOURCE_ALLOCATION;
        lastInvalidationFlags = pendingInvalidationFlags;
        taskClearPending = true;
        cleanProviderSnapshotHoldActive = false;
        cleanProviderStartupDelayFrames = PATH_TRACE_NEE_CACHE_CLEAN_PROVIDER_STARTUP_DELAY_FRAMES;
        cleanProviderStartupRefreshFrames = 0u;
        cleanProviderStableViewFrames = 0u;
        cleanProviderLastViewValid = false;
        secondaryVisualSnapshotHoldActive = false;
        secondaryVisualBandActiveLastFrame = false;
    }
    return ready;
}

void PathTraceNeeCacheState::ObserveRluSignatures(uint64_t structuralSignature, uint64_t mappingSignature, uint64_t payloadSignature, uint32_t changeFlags)
{
    uint32_t invalidationFlags = changeFlags;
    if (observedRluSignaturesValid)
    {
        if (structuralSignature != observedRluStructuralSignature)
        {
            invalidationFlags |= PATH_TRACE_NEE_CACHE_INVALIDATE_RLU_STRUCTURAL;
        }
        if (mappingSignature != observedRluMappingSignature)
        {
            invalidationFlags |= PATH_TRACE_NEE_CACHE_INVALIDATE_RLU_MAPPING;
        }
        // Payload-only differences are caller-gated via changeFlags. Auto-flagging
        // them here forced a full 4-buffer clear every frame under animated lights
        // whenever the clean DI NEE-cache provider had not yet entered snapshot hold.
    }

    observedRluStructuralSignature = structuralSignature;
    observedRluMappingSignature = mappingSignature;
    observedRluPayloadSignature = payloadSignature;
    observedRluSignaturesValid = true;

    if (invalidationFlags != PATH_TRACE_NEE_CACHE_INVALIDATE_NONE)
    {
        pendingInvalidationFlags |= invalidationFlags;
        lastInvalidationFlags = pendingInvalidationFlags;
        ++invalidationSerial;
        taskClearPending = true;
    }
}
