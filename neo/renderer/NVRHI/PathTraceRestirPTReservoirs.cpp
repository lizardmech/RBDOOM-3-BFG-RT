#include "precompiled.h"
#pragma hdrstop

#include "PathTraceRestirPTReservoirs.h"

namespace {

uint32_t RestirPTPrimarySurfaceHistoryCountLocal(uint32_t width, uint32_t height)
{
    return (width > 0 ? width : 1) * (height > 0 ? height : 1);
}

uint64_t RestirPTPrimarySurfaceHistoryByteSizeLocal(uint32_t width, uint32_t height)
{
    return static_cast<uint64_t>(RestirPTPrimarySurfaceHistoryCountLocal(width, height)) *
        static_cast<uint64_t>(RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_STRIDE);
}

} // namespace

bool RestirPTPrimarySurfaceHistoryBufferHasCapacity(nvrhi::BufferHandle buffer, uint32_t width, uint32_t height)
{
    return
        buffer &&
        buffer->getDesc().structStride == RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_STRIDE &&
        buffer->getDesc().byteSize >= RestirPTPrimarySurfaceHistoryByteSizeLocal(width, height);
}

nvrhi::BufferHandle ReuseOrCreateRestirPTPrimarySurfaceHistoryBuffer(
    nvrhi::IDevice* device,
    nvrhi::BufferHandle existingBuffer,
    const char* debugName,
    uint32_t width,
    uint32_t height)
{
    if (RestirPTPrimarySurfaceHistoryBufferHasCapacity(existingBuffer, width, height))
    {
        return existingBuffer;
    }
    if (!device)
    {
        return nullptr;
    }

    nvrhi::BufferDesc desc;
    desc.debugName = debugName;
    desc.byteSize = RestirPTPrimarySurfaceHistoryByteSizeLocal(width, height);
    desc.structStride = RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_STRIDE;
    desc.canHaveUAVs = true;
    desc.canHaveTypedViews = false;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    return device->createBuffer(desc);
}

bool RtRestirPTPrimarySurfaceHistoryBufferHandles::IsValidFor(uint32_t requestedWidth, uint32_t requestedHeight) const
{
    const uint32_t requiredWidth = requestedWidth > 0 ? requestedWidth : 1;
    const uint32_t requiredHeight = requestedHeight > 0 ? requestedHeight : 1;
    const uint32_t requiredCount = RestirPTPrimarySurfaceHistoryCountLocal(requestedWidth, requestedHeight);
    const uint64_t requiredBytes = RestirPTPrimarySurfaceHistoryByteSizeLocal(requestedWidth, requestedHeight);
    return
        current &&
        previous &&
        width == requiredWidth &&
        height == requiredHeight &&
        surfaceCount >= requiredCount &&
        surfaceBytes >= requiredBytes &&
        current->getDesc().structStride == RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_STRIDE &&
        previous->getDesc().structStride == RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_STRIDE &&
        current->getDesc().byteSize >= requiredBytes &&
        previous->getDesc().byteSize >= requiredBytes;
}

void RtRestirPTPrimarySurfaceHistoryBufferHandles::Reset()
{
    current = nullptr;
    previous = nullptr;
    width = 0;
    height = 0;
    surfaceCount = 0;
    surfaceBytes = 0;
}

RtRestirPTPrimarySurfaceHistoryBufferCreateResult CreateRestirPTPrimarySurfaceHistoryBuffers(const RtRestirPTPrimarySurfaceHistoryBufferCreateDesc& desc)
{
    RtRestirPTPrimarySurfaceHistoryBufferCreateResult result;
    result.buffers.width = desc.width > 0 ? desc.width : 1;
    result.buffers.height = desc.height > 0 ? desc.height : 1;
    result.buffers.surfaceCount = RestirPTPrimarySurfaceHistoryCountLocal(desc.width, desc.height);
    result.buffers.surfaceBytes = RestirPTPrimarySurfaceHistoryByteSizeLocal(desc.width, desc.height);
    result.buffers.current = ReuseOrCreateRestirPTPrimarySurfaceHistoryBuffer(
        desc.device, desc.existingBuffers.current, "PathTraceRestirPTPrimarySurfaceCurrent", desc.width, desc.height);
    result.buffers.previous = ReuseOrCreateRestirPTPrimarySurfaceHistoryBuffer(
        desc.device, desc.existingBuffers.previous, "PathTraceRestirPTPrimarySurfacePrevious", desc.width, desc.height);

    if (!result.buffers.IsValidFor(desc.width, desc.height))
    {
        result.errorMessage = "failed to create RT ReSTIR PT primary-surface history buffers";
    }
    return result;
}

bool ClearRestirPTPrimarySurfaceHistoryBuffers(nvrhi::ICommandList* commandList, const RtRestirPTPrimarySurfaceHistoryBufferHandles& buffers)
{
    if (!commandList || !buffers.current || !buffers.previous)
    {
        return false;
    }

    commandList->setBufferState(buffers.current, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setBufferState(buffers.previous, nvrhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();
    commandList->clearBufferUInt(buffers.current, 0);
    commandList->clearBufferUInt(buffers.previous, 0);
    return true;
}
