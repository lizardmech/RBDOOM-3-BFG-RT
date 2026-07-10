#include "precompiled.h"
#pragma hdrstop

#include "PathTraceRestirPTReservoirs.h"

namespace {

uint32_t RestirPTReservoirDimension(uint32_t value)
{
    return value > 0 ? value : 1;
}

RtRestirPTReservoirBufferParameters RestirPTReservoirParameters(uint32_t width, uint32_t height, RtRestirPTCheckerboardMode checkerboardMode)
{
    return rbdoom::restir_pt::CalculateReservoirBufferParameters(
        RestirPTReservoirDimension(width),
        RestirPTReservoirDimension(height),
        checkerboardMode);
}

uint64_t RestirPTReservoirElementCount64(uint32_t width, uint32_t height, RtRestirPTCheckerboardMode checkerboardMode)
{
    const RtRestirPTReservoirBufferParameters params = RestirPTReservoirParameters(width, height, checkerboardMode);
    return static_cast<uint64_t>(params.reservoirArrayPitch) * static_cast<uint64_t>(rbdoom::restir_pt::kNumReservoirBuffers);
}

uint64_t RestirPTReservoirByteSize(uint32_t width, uint32_t height, RtRestirPTCheckerboardMode checkerboardMode)
{
    return RestirPTReservoirElementCount64(width, height, checkerboardMode) * static_cast<uint64_t>(sizeof(RtRestirPTPackedReservoir));
}

bool RestirPTReservoirBufferHasCapacity(nvrhi::BufferHandle buffer, uint32_t width, uint32_t height, RtRestirPTCheckerboardMode checkerboardMode)
{
    return
        buffer &&
        buffer->getDesc().structStride == sizeof(RtRestirPTPackedReservoir) &&
        buffer->getDesc().byteSize >= RestirPTReservoirByteSize(width, height, checkerboardMode);
}

nvrhi::BufferHandle CreateRestirPTReservoirBuffer(nvrhi::IDevice* device, uint32_t width, uint32_t height, RtRestirPTCheckerboardMode checkerboardMode)
{
    if (!device)
    {
        return nullptr;
    }

    nvrhi::BufferDesc desc;
    desc.debugName = "PathTraceRestirPTReservoirs";
    desc.byteSize = RestirPTReservoirByteSize(width, height, checkerboardMode);
    desc.structStride = sizeof(RtRestirPTPackedReservoir);
    desc.canHaveUAVs = true;
    desc.canHaveTypedViews = false;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    return device->createBuffer(desc);
}

nvrhi::BufferHandle ReuseOrCreateRestirPTReservoirBuffer(nvrhi::IDevice* device, nvrhi::BufferHandle existingBuffer, uint32_t width, uint32_t height, RtRestirPTCheckerboardMode checkerboardMode)
{
    if (RestirPTReservoirBufferHasCapacity(existingBuffer, width, height, checkerboardMode))
    {
        return existingBuffer;
    }

    return CreateRestirPTReservoirBuffer(device, width, height, checkerboardMode);
}

} // namespace

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

bool RtRestirPTReservoirBufferHandles::IsValidFor(uint32_t requestedWidth, uint32_t requestedHeight, RtRestirPTCheckerboardMode checkerboardMode) const
{
    // Local helpers live in the anonymous namespace above; call via same-TU visibility.
    const uint32_t requiredWidth = requestedWidth > 0 ? requestedWidth : 1;
    const uint32_t requiredHeight = requestedHeight > 0 ? requestedHeight : 1;
    const RtRestirPTReservoirBufferParameters requiredParams =
        rbdoom::restir_pt::CalculateReservoirBufferParameters(requiredWidth, requiredHeight, checkerboardMode);
    const uint64_t requiredElementCount =
        static_cast<uint64_t>(requiredParams.reservoirArrayPitch) *
        static_cast<uint64_t>(rbdoom::restir_pt::kNumReservoirBuffers);
    const uint64_t requiredBytes = requiredElementCount * static_cast<uint64_t>(sizeof(RtRestirPTPackedReservoir));

    return
        reservoirs &&
        width == requiredWidth &&
        height == requiredHeight &&
        reservoirParams.reservoirBlockRowPitch == requiredParams.reservoirBlockRowPitch &&
        reservoirParams.reservoirArrayPitch == requiredParams.reservoirArrayPitch &&
        reservoirElementCount >= requiredElementCount &&
        reservoirBytes >= requiredBytes &&
        reservoirs->getDesc().structStride == sizeof(RtRestirPTPackedReservoir) &&
        reservoirs->getDesc().byteSize >= requiredBytes;
}

void RtRestirPTReservoirBufferHandles::Reset()
{
    reservoirs = nullptr;
    reservoirParams = {};
    width = 0;
    height = 0;
    reservoirElementCount = 0;
    reservoirBytes = 0;
}

RtRestirPTReservoirBufferCreateResult CreateRestirPTReservoirBuffers(const RtRestirPTReservoirBufferCreateDesc& desc)
{
    RtRestirPTReservoirBufferCreateResult result;
    result.buffers.width = RestirPTReservoirDimension(desc.width);
    result.buffers.height = RestirPTReservoirDimension(desc.height);
    result.buffers.reservoirParams = RestirPTReservoirParameters(desc.width, desc.height, desc.checkerboardMode);

    const uint64_t elementCount = RestirPTReservoirElementCount64(desc.width, desc.height, desc.checkerboardMode);
    if (elementCount > UINT32_MAX)
    {
        result.errorMessage = "RT ReSTIR PT reservoir element count exceeds 32-bit handle metadata";
        return result;
    }

    result.buffers.reservoirElementCount = static_cast<uint32_t>(elementCount);
    result.buffers.reservoirBytes = RestirPTReservoirByteSize(desc.width, desc.height, desc.checkerboardMode);
    result.buffers.reservoirs = ReuseOrCreateRestirPTReservoirBuffer(desc.device, desc.existingBuffers.reservoirs, desc.width, desc.height, desc.checkerboardMode);

    if (!result.buffers.IsValidFor(desc.width, desc.height, desc.checkerboardMode))
    {
        result.errorMessage = "failed to create RT ReSTIR PT packed reservoir buffer";
    }
    return result;
}

bool ClearRestirPTReservoirBuffers(nvrhi::ICommandList* commandList, const RtRestirPTReservoirBufferHandles& buffers)
{
    if (!commandList || !buffers.reservoirs)
    {
        return false;
    }

    commandList->setBufferState(buffers.reservoirs, nvrhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();
    commandList->clearBufferUInt(buffers.reservoirs, 0);
    return true;
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
