#pragma once

// ReSTIR PT packed reservoir resource ownership.
//
// This is deliberately separate from PathTraceReservoirs.*. The existing smoke
// reservoirs are an engine-side debug scaffold; this module owns the host-side
// packed PT reservoir stride and addressing metadata.

#include <nvrhi/nvrhi.h>

#include <cstdint>

#include "PathTraceRestirPTParameters.h"
#include "PathTracePrimarySurface.h"

struct RtRestirPTReservoirBufferHandles
{
    nvrhi::BufferHandle reservoirs;
    RtRestirPTReservoirBufferParameters reservoirParams = {};
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t reservoirElementCount = 0;
    uint64_t reservoirBytes = 0;

    bool IsValidFor(uint32_t requestedWidth, uint32_t requestedHeight, RtRestirPTCheckerboardMode checkerboardMode) const;
    void Reset();
};

struct RtRestirPTReservoirBufferCreateDesc
{
    nvrhi::IDevice* device = nullptr;
    RtRestirPTReservoirBufferHandles existingBuffers;
    uint32_t width = 0;
    uint32_t height = 0;
    RtRestirPTCheckerboardMode checkerboardMode = RtRestirPTCheckerboardMode::Off;
};

struct RtRestirPTReservoirBufferCreateResult
{
    RtRestirPTReservoirBufferHandles buffers;
    const char* errorMessage = nullptr;

    bool Succeeded() const { return errorMessage == nullptr && buffers.reservoirs != nullptr; }
};

RtRestirPTReservoirBufferCreateResult CreateRestirPTReservoirBuffers(const RtRestirPTReservoirBufferCreateDesc& desc);
bool ClearRestirPTReservoirBuffers(nvrhi::ICommandList* commandList, const RtRestirPTReservoirBufferHandles& buffers);

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
