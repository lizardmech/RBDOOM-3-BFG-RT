#pragma once

// Primary-surface history resource ownership.

#include <nvrhi/nvrhi.h>

#include <cstdint>

#include "PathTracePrimarySurface.h"

struct RtRestirPTPrimarySurfaceHistoryBufferHandles
{
    nvrhi::BufferHandle current;
    nvrhi::BufferHandle previous;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t surfaceCount = 0;
    uint64_t surfaceBytes = 0;

    bool IsValidFor(uint32_t requestedWidth, uint32_t requestedHeight) const;
    void Reset();
};

struct RtRestirPTPrimarySurfaceHistoryBufferCreateDesc
{
    nvrhi::IDevice* device = nullptr;
    RtRestirPTPrimarySurfaceHistoryBufferHandles existingBuffers;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct RtRestirPTPrimarySurfaceHistoryBufferCreateResult
{
    RtRestirPTPrimarySurfaceHistoryBufferHandles buffers;
    const char* errorMessage = nullptr;

    bool Succeeded() const { return errorMessage == nullptr && buffers.current != nullptr && buffers.previous != nullptr; }
};

RtRestirPTPrimarySurfaceHistoryBufferCreateResult CreateRestirPTPrimarySurfaceHistoryBuffers(const RtRestirPTPrimarySurfaceHistoryBufferCreateDesc& desc);
bool ClearRestirPTPrimarySurfaceHistoryBuffers(nvrhi::ICommandList* commandList, const RtRestirPTPrimarySurfaceHistoryBufferHandles& buffers);

// Single primary-surface-record buffer (same stride/count as one history page).
// Used for mirror secondary G-buffer (L1 Strategy B).
bool RestirPTPrimarySurfaceHistoryBufferHasCapacity(nvrhi::BufferHandle buffer, uint32_t width, uint32_t height);
nvrhi::BufferHandle ReuseOrCreateRestirPTPrimarySurfaceHistoryBuffer(
    nvrhi::IDevice* device,
    nvrhi::BufferHandle existingBuffer,
    const char* debugName,
    uint32_t width,
    uint32_t height);
