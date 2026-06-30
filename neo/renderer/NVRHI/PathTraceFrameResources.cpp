#include "precompiled.h"
#pragma hdrstop

#include "PathTraceFrameResources.h"
#include "PathTraceMaterialFeaturePasses.h"
#include "PathTraceMaterialFeatureRuntime.h"

#include <nvrhi/utils.h>

namespace {

uint64_t EstimateRgba32FloatTextureBytes(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return 0;
    }
    return static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 16ull;
}

uint64_t EstimateRg16FloatTextureBytes(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return 0;
    }
    return static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4ull;
}

uint64_t EstimateRgba16FloatTextureBytes(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return 0;
    }
    return static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 8ull;
}

uint64_t EstimateR32FloatTextureBytes(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return 0;
    }
    return static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4ull;
}

uint64_t EstimateR32UintTextureBytes(int width, int height)
{
    if (width <= 0 || height <= 0)
    {
        return 0;
    }
    return static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4ull;
}

void AppendReason(idStr& out, const char* text)
{
    if (out.Length() > 0)
    {
        out.Append("|");
    }
    out.Append(text);
}

bool TextureSizeMatches(const nvrhi::TextureHandle& texture, int width, int height)
{
    if (!texture || width <= 0 || height <= 0)
    {
        return false;
    }
    const nvrhi::TextureDesc& desc = texture->getDesc();
    return desc.width == static_cast<uint32_t>(width) &&
        desc.height == static_cast<uint32_t>(height);
}

}

void RtPathTraceFrameCameraState::Reset()
{
    valid = false;
    width = 0;
    height = 0;
    origin = vec3_origin;
    forward = idVec3(1.0f, 0.0f, 0.0f);
    left = idVec3(0.0f, 1.0f, 0.0f);
    up = idVec3(0.0f, 0.0f, 1.0f);
    tanX = 1.0f;
    tanY = 1.0f;
}

void RtPathTraceFrameResourceDiagnostics::ResetResizeStats()
{
    waitForIdleCalls = 0;
    lastWaitForIdleReason = "";
    outputTexturesCreated = 0;
    diagnosticReadbackResourcesCreated = 0;
    smokeReservoirBuffersReused = 0;
    smokeReservoirBuffersRecreated = 0;
    restirPTReservoirBuffersReused = 0;
    restirPTReservoirBuffersRecreated = 0;
    primarySurfaceHistoryBuffersReused = 0;
    primarySurfaceHistoryBuffersRecreated = 0;
    motionVectorTexturesCreated = 0;
    motionVectorMaskTexturesCreated = 0;
    rrGuideTexturesCreated = 0;
    outputTextureBytes = 0;
    smokeReservoirBytes = 0;
    restirPTReservoirBytes = 0;
    restirPTDiReservoirBytes = 0;
    restirPTGiReservoirBytes = 0;
    primarySurfaceHistoryBytes = 0;
    motionVectorBytes = 0;
    motionVectorMaskBytes = 0;
    rrGuideBytes = 0;
}

bool RtPathTraceFrameResources::IsValidFor(int requestedWidth, int requestedHeight, int requestedOutputWidth, int requestedOutputHeight, RtRestirPTCheckerboardMode checkerboardMode) const
{
    return
        TextureSizeMatches(outputTexture, requestedOutputWidth, requestedOutputHeight) &&
        TextureSizeMatches(accumulationTexture, requestedOutputWidth, requestedOutputHeight) &&
        TextureSizeMatches(restirPTReflectionTexture, requestedWidth, requestedHeight) &&
        TextureSizeMatches(transmissionTexture, requestedWidth, requestedHeight) &&
        TextureSizeMatches(rrInputColorTexture, requestedWidth, requestedHeight) &&
        TextureSizeMatches(cleanRtxdiDiBoilingFilterTexture, requestedWidth, requestedHeight) &&
        TextureSizeMatches(motionVectorTexture, requestedWidth, requestedHeight) &&
        TextureSizeMatches(rrMotionVectorTexture, requestedWidth, requestedHeight) &&
        TextureSizeMatches(motionVectorMaskTexture, requestedWidth, requestedHeight) &&
        TextureSizeMatches(rrGuideAlbedoTexture, requestedWidth, requestedHeight) &&
        TextureSizeMatches(rrGuideSpecularAlbedoTexture, requestedWidth, requestedHeight) &&
        TextureSizeMatches(rrGuideNormalRoughnessTexture, requestedWidth, requestedHeight) &&
        TextureSizeMatches(rrGuideDepthTexture, requestedWidth, requestedHeight) &&
        TextureSizeMatches(rrGuideHitDistanceTexture, requestedWidth, requestedHeight) &&
        TextureSizeMatches(rrGuideResetMaskTexture, requestedWidth, requestedHeight) &&
        TextureSizeMatches(rrGuidePositionTexture, requestedWidth, requestedHeight) &&
        readbackTexture &&
        rrInputColorDumpReadbackTexture &&
        smokeReservoirBuffers.IsValidFor(requestedWidth, requestedHeight) &&
        restirPTReservoirBuffers.IsValidFor(static_cast<uint32_t>(requestedWidth), static_cast<uint32_t>(requestedHeight), checkerboardMode) &&
        restirPTDiReservoirBuffers.IsValidFor(static_cast<uint32_t>(requestedWidth), static_cast<uint32_t>(requestedHeight), checkerboardMode) &&
        restirPTGiReservoirBuffers.IsValidFor(static_cast<uint32_t>(requestedWidth), static_cast<uint32_t>(requestedHeight), checkerboardMode) &&
        primarySurfaceHistoryBuffers.IsValidFor(static_cast<uint32_t>(requestedWidth), static_cast<uint32_t>(requestedHeight)) &&
        width == requestedWidth &&
        height == requestedHeight &&
        outputWidth == requestedOutputWidth &&
        outputHeight == requestedOutputHeight;
}

nvrhi::TextureHandle PathTraceMaterialFeatureOutputTexture(const RtPathTraceFrameResources& frameResources, uint32_t resource)
{
    switch (resource)
    {
    case RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR:
        return frameResources.outputTexture;
    case RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT:
        return frameResources.transmissionTexture;
    default:
        return nullptr;
    }
}

bool PathTraceMaterialFeatureOutputAvailable(const RtPathTraceMaterialFeaturePassDesc& passDesc, const RtPathTraceFrameResources& frameResources, uint32_t resource)
{
    return !PathTraceMaterialFeaturePassWritesAnyOutput(passDesc, resource) ||
        PathTraceMaterialFeatureOutputTexture(frameResources, resource);
}

bool PathTraceMaterialFeaturePrimaryOutputAvailable(const RtPathTraceMaterialFeaturePassDesc& passDesc, const RtPathTraceFrameResources& frameResources)
{
    return PathTraceMaterialFeatureOutputAvailable(passDesc, frameResources, passDesc.primaryOutputResource);
}

bool PathTraceMaterialFeaturePrimaryOutputAvailable(const RtPathTraceMaterialFeatureRuntimePass& pass, const RtPathTraceFrameResources& frameResources)
{
    return !pass.ready || PathTraceMaterialFeaturePrimaryOutputAvailable(pass.desc, frameResources);
}

void SetPathTraceMaterialFeatureOutputState(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceFrameResources& frameResources,
    uint32_t resource,
    nvrhi::ResourceStates state)
{
    if (!PathTraceMaterialFeaturePassWritesAnyOutput(passDesc, resource))
    {
        return;
    }

    const nvrhi::TextureHandle texture = PathTraceMaterialFeatureOutputTexture(frameResources, resource);
    if (commandList && texture)
    {
        commandList->setTextureState(texture, nvrhi::AllSubresources, state);
    }
}

void SetPathTraceMaterialFeaturePrimaryOutputState(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceFrameResources& frameResources,
    nvrhi::ResourceStates state)
{
    SetPathTraceMaterialFeatureOutputState(commandList, passDesc, frameResources, passDesc.primaryOutputResource, state);
}

void SetPathTraceMaterialFeaturePrimaryOutputState(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeatureRuntimePass& pass,
    const RtPathTraceFrameResources& frameResources,
    nvrhi::ResourceStates state)
{
    if (pass.ready)
    {
        SetPathTraceMaterialFeaturePrimaryOutputState(commandList, pass.desc, frameResources, state);
    }
}

void ClearPathTraceMaterialFeatureOutput(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceFrameResources& frameResources,
    uint32_t resource,
    const nvrhi::Color& color)
{
    if (!PathTraceMaterialFeaturePassWritesAnyOutput(passDesc, resource))
    {
        return;
    }

    const nvrhi::TextureHandle texture = PathTraceMaterialFeatureOutputTexture(frameResources, resource);
    if (commandList && texture)
    {
        commandList->clearTextureFloat(texture, nvrhi::AllSubresources, color);
    }
}

void ClearPathTraceMaterialFeaturePrimaryOutput(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceFrameResources& frameResources,
    const nvrhi::Color& color)
{
    ClearPathTraceMaterialFeatureOutput(commandList, passDesc, frameResources, passDesc.primaryOutputResource, color);
}

void ClearPathTraceMaterialFeaturePrimaryOutput(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeatureRuntimePass& pass,
    const RtPathTraceFrameResources& frameResources,
    const nvrhi::Color& color)
{
    if (pass.ready)
    {
        ClearPathTraceMaterialFeaturePrimaryOutput(commandList, pass.desc, frameResources, color);
    }
}

void BarrierPathTraceMaterialFeatureOutput(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceFrameResources& frameResources,
    uint32_t resource)
{
    if (!PathTraceMaterialFeaturePassWritesAnyOutput(passDesc, resource))
    {
        return;
    }

    const nvrhi::TextureHandle texture = PathTraceMaterialFeatureOutputTexture(frameResources, resource);
    if (commandList && texture)
    {
        nvrhi::utils::TextureUavBarrier(commandList, texture);
    }
}

void BarrierPathTraceMaterialFeatureOutputs(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeaturePassDesc& passDesc,
    const RtPathTraceFrameResources& frameResources)
{
    for (uint32_t resource = 1u; resource != 0u; resource <<= 1u)
    {
        if (PathTraceMaterialFeaturePassWritesAnyOutput(passDesc, resource))
        {
            BarrierPathTraceMaterialFeatureOutput(commandList, passDesc, frameResources, resource);
        }
    }
}

void BarrierPathTraceMaterialFeatureOutputs(
    nvrhi::ICommandList* commandList,
    const RtPathTraceMaterialFeatureRuntimePass& pass,
    const RtPathTraceFrameResources& frameResources)
{
    if (pass.ready)
    {
        BarrierPathTraceMaterialFeatureOutputs(commandList, pass.desc, frameResources);
    }
}

bool RtPathTraceFrameResources::HasAnyOutputSizedResource() const
{
    return
        outputTexture ||
        accumulationTexture ||
        restirPTReflectionTexture ||
        transmissionTexture ||
        rrInputColorTexture ||
        cleanRtxdiDiBoilingFilterTexture ||
        motionVectorTexture ||
        rrMotionVectorTexture ||
        motionVectorMaskTexture ||
        rrGuideAlbedoTexture ||
        rrGuideSpecularAlbedoTexture ||
        rrGuideNormalRoughnessTexture ||
        rrGuideDepthTexture ||
        rrGuideHitDistanceTexture ||
        rrGuideResetMaskTexture ||
        rrGuidePositionTexture ||
        readbackTexture ||
        rrInputColorDumpReadbackTexture ||
        smokeReservoirBuffers.current ||
        smokeReservoirBuffers.previous ||
        smokeReservoirBuffers.spatialScratch ||
        restirPTReservoirBuffers.reservoirs ||
        restirPTDiReservoirBuffers.reservoirs ||
        restirPTGiReservoirBuffers.reservoirs ||
        primarySurfaceHistoryBuffers.current ||
        primarySurfaceHistoryBuffers.previous;
}

bool RtPathTraceFrameResources::ResizeOutputSizedResources(nvrhi::IDevice* device, int requestedWidth, int requestedHeight, int requestedOutputWidth, int requestedOutputHeight, RtRestirPTCheckerboardMode checkerboardMode)
{
    if (!device)
    {
        return false;
    }

    settings.width = requestedWidth;
    settings.height = requestedHeight;
    settings.outputWidth = requestedOutputWidth;
    settings.outputHeight = requestedOutputHeight;
    settings.checkerboardMode = checkerboardMode;
    settings.frameIndex = restirPTFrameIndex;
    settings.resetReasonFlags = RT_FRAME_RESET_NONE;
    diagnostics.ResetResizeStats();

    if (IsValidFor(requestedWidth, requestedHeight, requestedOutputWidth, requestedOutputHeight, checkerboardMode))
    {
        return true;
    }

    const bool replacingExistingOutput = HasAnyOutputSizedResource();
    if (replacingExistingOutput)
    {
        common->Printf("PathTraceFrameResources: resizing PT frame oldRender=%dx%d oldOutput=%dx%d newRender=%dx%d newOutput=%dx%d; waitForIdle reason=output-sized-resource-replacement\n",
            width, height, outputWidth, outputHeight, requestedWidth, requestedHeight, requestedOutputWidth, requestedOutputHeight);
        device->waitForIdle();
        device->runGarbageCollection();
        diagnostics.waitForIdleCalls++;
        diagnostics.lastWaitForIdleReason = "output-sized-resource-replacement";
        MarkResetReason(RT_FRAME_RESET_GPU_IDLE_WAIT);
    }

    nvrhi::TextureDesc outputDesc;
    outputDesc.width = requestedOutputWidth;
    outputDesc.height = requestedOutputHeight;
    outputDesc.mipLevels = 1;
    outputDesc.arraySize = 1;
    outputDesc.format = nvrhi::Format::RGBA32_FLOAT;
    outputDesc.dimension = nvrhi::TextureDimension::Texture2D;
    outputDesc.isUAV = true;
    outputDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    outputDesc.keepInitialState = true;
    outputDesc.debugName = "PathTraceSmokeOutput";
    nvrhi::TextureHandle newOutputTexture = device->createTexture(outputDesc);

    if (!newOutputTexture)
    {
        common->Printf("PathTraceFrameResources: failed to create PT output UAV (%dx%d)\n", requestedOutputWidth, requestedOutputHeight);
        return false;
    }

    outputDesc.debugName = "PathTraceSmokeAccumulation";
    nvrhi::TextureHandle newAccumulationTexture = device->createTexture(outputDesc);
    if (!newAccumulationTexture)
    {
        common->Printf("PathTraceFrameResources: failed to create PT accumulation UAV (%dx%d)\n", requestedOutputWidth, requestedOutputHeight);
        return false;
    }

    nvrhi::TextureDesc renderDesc = outputDesc;
    renderDesc.width = requestedWidth;
    renderDesc.height = requestedHeight;

    renderDesc.debugName = "PathTraceRestirPTReflection";
    nvrhi::TextureHandle newRestirPTReflectionTexture = device->createTexture(renderDesc);
    if (!newRestirPTReflectionTexture)
    {
        common->Printf("PathTraceFrameResources: failed to create PT ReSTIR reflection UAV (%dx%d)\n", requestedWidth, requestedHeight);
        return false;
    }

    renderDesc.debugName = "PathTraceTransmission";
    nvrhi::TextureHandle newTransmissionTexture = device->createTexture(renderDesc);
    if (!newTransmissionTexture)
    {
        common->Printf("PathTraceFrameResources: failed to create PT transmission UAV (%dx%d)\n", requestedWidth, requestedHeight);
        return false;
    }

    renderDesc.debugName = "PathTraceRRInputColor";
    nvrhi::TextureHandle newRrInputColorTexture = device->createTexture(renderDesc);
    if (!newRrInputColorTexture)
    {
        common->Printf("PathTraceFrameResources: failed to create PT RR input-color UAV (%dx%d)\n", requestedWidth, requestedHeight);
        return false;
    }

    nvrhi::TextureDesc motionVectorDesc = renderDesc;
    motionVectorDesc.format = nvrhi::Format::RGBA16_FLOAT;
    motionVectorDesc.debugName = "PathTraceSmokeMotionVectors";
    nvrhi::TextureHandle newMotionVectorTexture = device->createTexture(motionVectorDesc);
    if (!newMotionVectorTexture)
    {
        common->Printf("PathTraceFrameResources: failed to create PT motion-vector UAV (%dx%d)\n", requestedWidth, requestedHeight);
        return false;
    }

    renderDesc.debugName = "PathTraceCleanRtxdiDiBoilingFilter";
    nvrhi::TextureHandle newCleanRtxdiDiBoilingFilterTexture = device->createTexture(renderDesc);
    if (!newCleanRtxdiDiBoilingFilterTexture)
    {
        common->Printf("PathTraceFrameResources: failed to create clean RTXDI DI boiling-filter scratch UAV (%dx%d)\n", requestedWidth, requestedHeight);
        return false;
    }

    nvrhi::TextureDesc rrMotionVectorDesc = renderDesc;
    rrMotionVectorDesc.format = nvrhi::Format::RG16_FLOAT;
    rrMotionVectorDesc.debugName = "PathTraceRRMotionVectors";
    nvrhi::TextureHandle newRrMotionVectorTexture = device->createTexture(rrMotionVectorDesc);
    if (!newRrMotionVectorTexture)
    {
        common->Printf("PathTraceFrameResources: failed to create PT RR motion-vector UAV (%dx%d)\n", requestedWidth, requestedHeight);
        return false;
    }

    nvrhi::TextureDesc motionVectorMaskDesc = renderDesc;
    motionVectorMaskDesc.format = nvrhi::Format::R32_UINT;
    motionVectorMaskDesc.debugName = "PathTraceSmokeMotionVectorMask";
    nvrhi::TextureHandle newMotionVectorMaskTexture = device->createTexture(motionVectorMaskDesc);
    if (!newMotionVectorMaskTexture)
    {
        common->Printf("PathTraceFrameResources: failed to create PT motion-vector mask UAV (%dx%d)\n", requestedWidth, requestedHeight);
        return false;
    }

    nvrhi::TextureDesc rrGuideRgba16Desc = renderDesc;
    rrGuideRgba16Desc.format = nvrhi::Format::RGBA16_FLOAT;
    rrGuideRgba16Desc.debugName = "PathTraceRRGuideAlbedo";
    nvrhi::TextureHandle newRrGuideAlbedoTexture = device->createTexture(rrGuideRgba16Desc);
    if (!newRrGuideAlbedoTexture)
    {
        common->Printf("PathTraceFrameResources: failed to create PT RR albedo guide UAV (%dx%d)\n", requestedWidth, requestedHeight);
        return false;
    }

    rrGuideRgba16Desc.debugName = "PathTraceRRGuideNormalRoughness";
    nvrhi::TextureHandle newRrGuideNormalRoughnessTexture = device->createTexture(rrGuideRgba16Desc);
    if (!newRrGuideNormalRoughnessTexture)
    {
        common->Printf("PathTraceFrameResources: failed to create PT RR normal/roughness guide UAV (%dx%d)\n", requestedWidth, requestedHeight);
        return false;
    }

    rrGuideRgba16Desc.debugName = "PathTraceRRGuideSpecularAlbedo";
    nvrhi::TextureHandle newRrGuideSpecularAlbedoTexture = device->createTexture(rrGuideRgba16Desc);
    if (!newRrGuideSpecularAlbedoTexture)
    {
        common->Printf("PathTraceFrameResources: failed to create PT RR specular-albedo guide UAV (%dx%d)\n", requestedWidth, requestedHeight);
        return false;
    }

    nvrhi::TextureDesc rrGuidePositionDesc = renderDesc;
    rrGuidePositionDesc.format = nvrhi::Format::RGBA32_FLOAT;
    rrGuidePositionDesc.debugName = "PathTraceRRGuidePosition";
    nvrhi::TextureHandle newRrGuidePositionTexture = device->createTexture(rrGuidePositionDesc);
    if (!newRrGuidePositionTexture)
    {
        common->Printf("PathTraceFrameResources: failed to create PT RR position guide UAV (%dx%d)\n", requestedWidth, requestedHeight);
        return false;
    }

    nvrhi::TextureDesc rrGuideR32Desc = renderDesc;
    rrGuideR32Desc.format = nvrhi::Format::R32_FLOAT;
    rrGuideR32Desc.debugName = "PathTraceRRGuideDepth";
    nvrhi::TextureHandle newRrGuideDepthTexture = device->createTexture(rrGuideR32Desc);
    if (!newRrGuideDepthTexture)
    {
        common->Printf("PathTraceFrameResources: failed to create PT RR depth guide UAV (%dx%d)\n", requestedWidth, requestedHeight);
        return false;
    }

    rrGuideR32Desc.debugName = "PathTraceRRGuideHitDistance";
    nvrhi::TextureHandle newRrGuideHitDistanceTexture = device->createTexture(rrGuideR32Desc);
    if (!newRrGuideHitDistanceTexture)
    {
        common->Printf("PathTraceFrameResources: failed to create PT RR hit-distance guide UAV (%dx%d)\n", requestedWidth, requestedHeight);
        return false;
    }

    nvrhi::TextureDesc rrGuideResetMaskDesc = renderDesc;
    rrGuideResetMaskDesc.format = nvrhi::Format::R32_UINT;
    rrGuideResetMaskDesc.debugName = "PathTraceRRGuideResetMask";
    nvrhi::TextureHandle newRrGuideResetMaskTexture = device->createTexture(rrGuideResetMaskDesc);
    if (!newRrGuideResetMaskTexture)
    {
        common->Printf("PathTraceFrameResources: failed to create PT RR reset/disocclusion mask UAV (%dx%d)\n", requestedWidth, requestedHeight);
        return false;
    }

    nvrhi::TextureDesc readbackDesc = outputDesc;
    readbackDesc.isShaderResource = false;
    readbackDesc.isUAV = false;
    readbackDesc.initialState = nvrhi::ResourceStates::Unknown;
    readbackDesc.keepInitialState = false;
    readbackDesc.debugName = "PathTraceSmokeReadback";
    nvrhi::StagingTextureHandle newReadbackTexture = device->createStagingTexture(readbackDesc, nvrhi::CpuAccessMode::Read);

    if (!newReadbackTexture)
    {
        common->Printf("PathTraceFrameResources: failed to create PT readback texture (%dx%d)\n", requestedOutputWidth, requestedOutputHeight);
        return false;
    }

    nvrhi::TextureDesc rrInputReadbackDesc = renderDesc;
    rrInputReadbackDesc.isShaderResource = false;
    rrInputReadbackDesc.isUAV = false;
    rrInputReadbackDesc.initialState = nvrhi::ResourceStates::Unknown;
    rrInputReadbackDesc.keepInitialState = false;
    rrInputReadbackDesc.debugName = "PathTraceRRInputColorDumpReadback";

    nvrhi::StagingTextureHandle newRrInputColorDumpReadbackTexture = device->createStagingTexture(rrInputReadbackDesc, nvrhi::CpuAccessMode::Read);
    if (!newRrInputColorDumpReadbackTexture)
    {
        common->Printf("PathTraceFrameResources: failed to create PT RR input-color dump readback texture (%dx%d)\n", requestedWidth, requestedHeight);
        return false;
    }

    const bool smokeReservoirWasValid = smokeReservoirBuffers.IsValidFor(requestedWidth, requestedHeight);
    const bool restirReservoirWasValid = restirPTReservoirBuffers.IsValidFor(static_cast<uint32_t>(requestedWidth), static_cast<uint32_t>(requestedHeight), checkerboardMode);
    const bool restirDiReservoirWasValid = restirPTDiReservoirBuffers.IsValidFor(static_cast<uint32_t>(requestedWidth), static_cast<uint32_t>(requestedHeight), checkerboardMode);
    const bool restirGiReservoirWasValid = restirPTGiReservoirBuffers.IsValidFor(static_cast<uint32_t>(requestedWidth), static_cast<uint32_t>(requestedHeight), checkerboardMode);
    const bool primaryHistoryWasValid = primarySurfaceHistoryBuffers.IsValidFor(static_cast<uint32_t>(requestedWidth), static_cast<uint32_t>(requestedHeight));

    outputTexture = newOutputTexture;
    accumulationTexture = newAccumulationTexture;
    restirPTReflectionTexture = newRestirPTReflectionTexture;
    transmissionTexture = newTransmissionTexture;
    rrInputColorTexture = newRrInputColorTexture;
    cleanRtxdiDiBoilingFilterTexture = newCleanRtxdiDiBoilingFilterTexture;
    motionVectorTexture = newMotionVectorTexture;
    rrMotionVectorTexture = newRrMotionVectorTexture;
    motionVectorMaskTexture = newMotionVectorMaskTexture;
    rrGuideAlbedoTexture = newRrGuideAlbedoTexture;
    rrGuideSpecularAlbedoTexture = newRrGuideSpecularAlbedoTexture;
    rrGuideNormalRoughnessTexture = newRrGuideNormalRoughnessTexture;
    rrGuideDepthTexture = newRrGuideDepthTexture;
    rrGuideHitDistanceTexture = newRrGuideHitDistanceTexture;
    rrGuideResetMaskTexture = newRrGuideResetMaskTexture;
    rrGuidePositionTexture = newRrGuidePositionTexture;
    readbackTexture = newReadbackTexture;
    rrInputColorDumpReadbackTexture = newRrInputColorDumpReadbackTexture;
    width = requestedWidth;
    height = requestedHeight;
    outputWidth = requestedOutputWidth;
    outputHeight = requestedOutputHeight;
    diagnostics.outputTexturesCreated += 6;
    diagnostics.motionVectorTexturesCreated += 2;
    diagnostics.motionVectorMaskTexturesCreated++;
    diagnostics.rrGuideTexturesCreated += 7;
    diagnostics.diagnosticReadbackResourcesCreated += 2;
    diagnostics.outputTextureBytes =
        EstimateRgba32FloatTextureBytes(outputWidth, outputHeight) * 2ull +
        EstimateRgba32FloatTextureBytes(width, height) * 4ull;
    diagnostics.motionVectorBytes = EstimateRgba16FloatTextureBytes(width, height) + EstimateRg16FloatTextureBytes(width, height);
    diagnostics.motionVectorMaskBytes = EstimateR32UintTextureBytes(width, height);
    diagnostics.rrGuideBytes =
        EstimateRgba16FloatTextureBytes(width, height) * 3ull +
        EstimateRgba32FloatTextureBytes(width, height) +
        EstimateR32FloatTextureBytes(width, height) * 2ull +
        EstimateR32UintTextureBytes(width, height);
    MarkResetReason(RT_FRAME_RESET_OUTPUT_RESIZE);

    RtSmokeReservoirBufferCreateDesc reservoirDesc;
    reservoirDesc.device = device;
    reservoirDesc.existingBuffers = smokeReservoirBuffers;
    reservoirDesc.width = requestedWidth;
    reservoirDesc.height = requestedHeight;
    const RtSmokeReservoirBufferCreateResult reservoirResult = CreateSmokeReservoirBuffers(reservoirDesc);
    if (!reservoirResult.Succeeded())
    {
        common->Printf("PathTraceFrameResources: %s (%dx%d)\n", reservoirResult.errorMessage ? reservoirResult.errorMessage : "failed to create RT smoke reservoir buffers", requestedWidth, requestedHeight);
        return false;
    }
    smokeReservoirBuffers = reservoirResult.buffers;
    diagnostics.smokeReservoirBytes = static_cast<uint64_t>(smokeReservoirBuffers.reservoirBytes);
    if (smokeReservoirWasValid)
    {
        diagnostics.smokeReservoirBuffersReused += 3;
    }
    else
    {
        diagnostics.smokeReservoirBuffersRecreated += 3;
    }

    RtRestirPTReservoirBufferCreateDesc restirPTReservoirDesc;
    restirPTReservoirDesc.device = device;
    restirPTReservoirDesc.existingBuffers = restirPTReservoirBuffers;
    restirPTReservoirDesc.width = static_cast<uint32_t>(requestedWidth);
    restirPTReservoirDesc.height = static_cast<uint32_t>(requestedHeight);
    restirPTReservoirDesc.checkerboardMode = checkerboardMode;
    const RtRestirPTReservoirBufferCreateResult restirPTReservoirResult = CreateRestirPTReservoirBuffers(restirPTReservoirDesc);
    if (!restirPTReservoirResult.Succeeded())
    {
        common->Printf("PathTraceFrameResources: %s (%dx%d)\n", restirPTReservoirResult.errorMessage ? restirPTReservoirResult.errorMessage : "failed to create RT ReSTIR PT packed reservoir buffer", requestedWidth, requestedHeight);
        return false;
    }
    restirPTReservoirBuffers = restirPTReservoirResult.buffers;
    diagnostics.restirPTReservoirBytes = restirPTReservoirBuffers.reservoirBytes;
    if (restirReservoirWasValid)
    {
        diagnostics.restirPTReservoirBuffersReused++;
    }
    else
    {
        diagnostics.restirPTReservoirBuffersRecreated++;
    }

    RtRestirPTReservoirBufferCreateDesc restirPTDiReservoirDesc;
    restirPTDiReservoirDesc.device = device;
    restirPTDiReservoirDesc.existingBuffers = restirPTDiReservoirBuffers;
    restirPTDiReservoirDesc.width = static_cast<uint32_t>(requestedWidth);
    restirPTDiReservoirDesc.height = static_cast<uint32_t>(requestedHeight);
    restirPTDiReservoirDesc.checkerboardMode = checkerboardMode;
    const RtRestirPTReservoirBufferCreateResult restirPTDiReservoirResult = CreateRestirPTReservoirBuffers(restirPTDiReservoirDesc);
    if (!restirPTDiReservoirResult.Succeeded())
    {
        common->Printf("PathTraceFrameResources: %s (%dx%d)\n", restirPTDiReservoirResult.errorMessage ? restirPTDiReservoirResult.errorMessage : "failed to create RT ReSTIR PT DI packed reservoir buffer", requestedWidth, requestedHeight);
        return false;
    }
    restirPTDiReservoirBuffers = restirPTDiReservoirResult.buffers;
    diagnostics.restirPTDiReservoirBytes = restirPTDiReservoirBuffers.reservoirBytes;
    if (restirDiReservoirWasValid)
    {
        diagnostics.restirPTReservoirBuffersReused++;
    }
    else
    {
        diagnostics.restirPTReservoirBuffersRecreated++;
    }

    RtRestirPTReservoirBufferCreateDesc restirPTGiReservoirDesc;
    restirPTGiReservoirDesc.device = device;
    restirPTGiReservoirDesc.existingBuffers = restirPTGiReservoirBuffers;
    restirPTGiReservoirDesc.width = static_cast<uint32_t>(requestedWidth);
    restirPTGiReservoirDesc.height = static_cast<uint32_t>(requestedHeight);
    restirPTGiReservoirDesc.checkerboardMode = checkerboardMode;
    const RtRestirPTReservoirBufferCreateResult restirPTGiReservoirResult = CreateRestirPTReservoirBuffers(restirPTGiReservoirDesc);
    if (!restirPTGiReservoirResult.Succeeded())
    {
        common->Printf("PathTraceFrameResources: %s (%dx%d)\n", restirPTGiReservoirResult.errorMessage ? restirPTGiReservoirResult.errorMessage : "failed to create RT ReSTIR PT GI packed reservoir buffer", requestedWidth, requestedHeight);
        return false;
    }
    restirPTGiReservoirBuffers = restirPTGiReservoirResult.buffers;
    diagnostics.restirPTGiReservoirBytes = restirPTGiReservoirBuffers.reservoirBytes;
    if (restirGiReservoirWasValid)
    {
        diagnostics.restirPTReservoirBuffersReused++;
    }
    else
    {
        diagnostics.restirPTReservoirBuffersRecreated++;
    }

    RtRestirPTPrimarySurfaceHistoryBufferCreateDesc primaryHistoryDesc;
    primaryHistoryDesc.device = device;
    primaryHistoryDesc.existingBuffers = primarySurfaceHistoryBuffers;
    primaryHistoryDesc.width = static_cast<uint32_t>(requestedWidth);
    primaryHistoryDesc.height = static_cast<uint32_t>(requestedHeight);
    const RtRestirPTPrimarySurfaceHistoryBufferCreateResult primaryHistoryResult = CreateRestirPTPrimarySurfaceHistoryBuffers(primaryHistoryDesc);
    if (!primaryHistoryResult.Succeeded())
    {
        common->Printf("PathTraceFrameResources: %s (%dx%d)\n", primaryHistoryResult.errorMessage ? primaryHistoryResult.errorMessage : "failed to create RT ReSTIR PT primary-surface history buffers", requestedWidth, requestedHeight);
        return false;
    }
    primarySurfaceHistoryBuffers = primaryHistoryResult.buffers;
    diagnostics.primarySurfaceHistoryBytes = primarySurfaceHistoryBuffers.surfaceBytes * 2ull;
    if (primaryHistoryWasValid)
    {
        diagnostics.primarySurfaceHistoryBuffersReused += 2;
    }
    else
    {
        diagnostics.primarySurfaceHistoryBuffersRecreated += 2;
    }

    InvalidatePrimarySurfaceHistory(RT_FRAME_RESET_PRIMARY_HISTORY);
    ResetReadbackQueue();
    smokeAccumulationSignature = 0;
    smokeAccumulationFrameCount = 0;
    mode56AccumulationSignature = 0;
    mode56AccumulationFrameCount = 0;

    RtRestirPTContextUpdateDesc restirPTContextDesc;
    restirPTContextDesc.width = static_cast<uint32_t>(requestedWidth);
    restirPTContextDesc.height = static_cast<uint32_t>(requestedHeight);
    restirPTContextDesc.frameIndex = restirPTFrameIndex;
    restirPTContextDesc.checkerboardMode = checkerboardMode;
    restirPTContextDesc.resamplingMode = RtRestirPTResamplingMode::None;
    if (!UpdateRestirPTContextState(restirPTContextState, restirPTContextDesc))
    {
        common->Printf("PathTraceFrameResources: failed to initialize RT ReSTIR PT context (%dx%d)\n", requestedWidth, requestedHeight);
        return false;
    }

    common->Printf("PathTraceFrameResources: RT ReSTIR PT packed reservoirs render=%dx%d output=%dx%d elements=%u bytes=%llu arrayPitch=%u blockRowPitch=%u slices=%u stride=%u\n",
        requestedWidth,
        requestedHeight,
        requestedOutputWidth,
        requestedOutputHeight,
        restirPTReservoirBuffers.reservoirElementCount,
        static_cast<unsigned long long>(restirPTReservoirBuffers.reservoirBytes),
        restirPTReservoirBuffers.reservoirParams.reservoirArrayPitch,
        restirPTReservoirBuffers.reservoirParams.reservoirBlockRowPitch,
        rbdoom::restir_pt::kNumReservoirBuffers,
        static_cast<uint32_t>(sizeof(RtRestirPTPackedReservoir)));
    common->Printf("PathTraceFrameResources: RT ReSTIR PT DI temporal reservoirs render=%dx%d output=%dx%d elements=%u bytes=%llu arrayPitch=%u blockRowPitch=%u slices=%u stride=%u uav=u56\n",
        requestedWidth,
        requestedHeight,
        requestedOutputWidth,
        requestedOutputHeight,
        restirPTDiReservoirBuffers.reservoirElementCount,
        static_cast<unsigned long long>(restirPTDiReservoirBuffers.reservoirBytes),
        restirPTDiReservoirBuffers.reservoirParams.reservoirArrayPitch,
        restirPTDiReservoirBuffers.reservoirParams.reservoirBlockRowPitch,
        rbdoom::restir_pt::kNumReservoirBuffers,
        static_cast<uint32_t>(sizeof(RtRestirPTPackedReservoir)));
    common->Printf("PathTraceFrameResources: RT ReSTIR PT GI temporal reservoirs render=%dx%d output=%dx%d elements=%u bytes=%llu arrayPitch=%u blockRowPitch=%u slices=%u stride=%u uav=u55\n",
        requestedWidth,
        requestedHeight,
        requestedOutputWidth,
        requestedOutputHeight,
        restirPTGiReservoirBuffers.reservoirElementCount,
        static_cast<unsigned long long>(restirPTGiReservoirBuffers.reservoirBytes),
        restirPTGiReservoirBuffers.reservoirParams.reservoirArrayPitch,
        restirPTGiReservoirBuffers.reservoirParams.reservoirBlockRowPitch,
        rbdoom::restir_pt::kNumReservoirBuffers,
        static_cast<uint32_t>(sizeof(RtRestirPTPackedReservoir)));

    common->Printf("PathTraceFrameResources: RT ReSTIR PT primary-surface history render=%dx%d output=%dx%d records=%u bytes=%llu stride=%u\n",
        requestedWidth,
        requestedHeight,
        requestedOutputWidth,
        requestedOutputHeight,
        primarySurfaceHistoryBuffers.surfaceCount,
        static_cast<unsigned long long>(primarySurfaceHistoryBuffers.surfaceBytes),
        RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_STRIDE);

    common->Printf("PathTraceFrameResources: RT motion-vector export scaffold render=%dx%d output=%dx%d vectorFormat=RGBA16_FLOAT/u39 rrVectorFormat=RG16_FLOAT/u78 vectorBytes=%llu maskFormat=R32_UINT maskBytes=%llu maskUav=u40 consumer=debug-and-rr\n",
        requestedWidth,
        requestedHeight,
        requestedOutputWidth,
        requestedOutputHeight,
        static_cast<unsigned long long>(diagnostics.motionVectorBytes),
        static_cast<unsigned long long>(diagnostics.motionVectorMaskBytes));

    common->Printf("PathTraceFrameResources: RT DLSS RR guide scaffold render=%dx%d output=%dx%d albedo=RGBA16_FLOAT/u48 normalRoughness=RGBA16_FLOAT/u49 depth=R32_FLOAT/u50 hitDistance=R32_FLOAT/u51 resetMask=R32_UINT/u52 specularAlbedo=RGBA16_FLOAT/u53 rrInputColor=RGBA32_FLOAT/u54 position=RGBA32_FLOAT/u79 guideBytes=%llu producer=primary-surface-prepass\n",
        requestedWidth,
        requestedHeight,
        requestedOutputWidth,
        requestedOutputHeight,
        static_cast<unsigned long long>(diagnostics.rrGuideBytes));

    common->Printf("PathTraceFrameResources: RT smoke output UAV initialized render=%dx%d output=%dx%d reflectionUav=u47 rrInputColorUav=u54 cleanDiBoilingScratch=RGBA32_FLOAT\n", requestedWidth, requestedHeight, requestedOutputWidth, requestedOutputHeight);
    return true;
}

void RtPathTraceFrameResources::ResetOutputSizedResources(uint32_t reasonFlags)
{
    outputTexture = nullptr;
    accumulationTexture = nullptr;
    restirPTReflectionTexture = nullptr;
    transmissionTexture = nullptr;
    rrInputColorTexture = nullptr;
    cleanRtxdiDiBoilingFilterTexture = nullptr;
    motionVectorTexture = nullptr;
    rrMotionVectorTexture = nullptr;
    motionVectorMaskTexture = nullptr;
    rrGuideAlbedoTexture = nullptr;
    rrGuideSpecularAlbedoTexture = nullptr;
    rrGuideNormalRoughnessTexture = nullptr;
    rrGuideDepthTexture = nullptr;
    rrGuideHitDistanceTexture = nullptr;
    rrGuideResetMaskTexture = nullptr;
    rrGuidePositionTexture = nullptr;
    readbackTexture = nullptr;
    rrInputColorDumpReadbackTexture = nullptr;
    width = 0;
    height = 0;
    outputWidth = 0;
    outputHeight = 0;
    smokeReservoirBuffers.Reset();
    restirPTReservoirBuffers.Reset();
    restirPTDiReservoirBuffers.Reset();
    restirPTGiReservoirBuffers.Reset();
    primarySurfaceHistoryBuffers.Reset();
    restirPTContextState.Reset();
    smokeReservoirSceneSignature = 0;
    smokeReservoirDispatchSignature = 0;
    smokeReservoirNeedsClear = false;
    restirPTReservoirNeedsClear = true;
    restirPTDiReservoirNeedsClear = true;
    restirPTGiReservoirNeedsClear = true;
    primarySurfaceHistoryNeedsClear = true;
    primarySurfaceHistoryState.Reset(reasonFlags);
    primarySurfaceHistoryView.Reset();
    smokeReservoirResetCount = 0;
    smokeReservoirClearCount = 0;
    restirPTReservoirClearCount = 0;
    restirPTDiReservoirClearCount = 0;
    restirPTGiReservoirClearCount = 0;
    smokeAccumulationSignature = 0;
    smokeAccumulationFrameCount = 0;
    mode56AccumulationSignature = 0;
    mode56AccumulationFrameCount = 0;
    ResetReadbackQueue();
    MarkResetReason(reasonFlags);
}

void RtPathTraceFrameResources::ResetSceneDependentState()
{
    smokeAccumulationSignature = 0;
    smokeAccumulationFrameCount = 0;
    mode56AccumulationSignature = 0;
    mode56AccumulationFrameCount = 0;
    smokeReservoirSceneSignature = 0;
    smokeReservoirDispatchSignature = 0;
    smokeReservoirNeedsClear = false;
    restirPTReservoirNeedsClear = true;
    restirPTDiReservoirNeedsClear = true;
    restirPTGiReservoirNeedsClear = true;
    primarySurfaceHistoryNeedsClear = true;
    primarySurfaceHistoryState.Reset(RT_FRAME_RESET_SCENE_RESOURCES | RT_FRAME_RESET_PRIMARY_HISTORY);
    primarySurfaceHistoryView.Reset();
    smokeReservoirResetCount = 0;
    smokeReservoirClearCount = 0;
    restirPTReservoirClearCount = 0;
    restirPTDiReservoirClearCount = 0;
    restirPTGiReservoirClearCount = 0;
    ResetReadbackQueue();
    MarkResetReason(RT_FRAME_RESET_SCENE_RESOURCES | RT_FRAME_RESET_PRIMARY_HISTORY);
}

void RtPathTraceFrameResources::ResetReadbackQueue()
{
    readbackQueued = false;
    readbackDelayFrames = 0;
    readbackCooldownFrames = 0;
    rrInputColorDumpQueued = false;
    rrInputColorDumpDelayFrames = 0;
    rrInputColorDumpSource = 0;
    rrInputColorDumpFrameIndex = 0;
}

void RtPathTraceFrameResources::MarkResetReason(uint32_t reasonFlags)
{
    settings.resetReasonFlags |= reasonFlags;
}

void RtPathTraceFrameResources::ClearResetReasons()
{
    settings.resetReasonFlags = RT_FRAME_RESET_NONE;
}

void RtPathTraceFrameResources::SetPrimarySurfaceHistoryView(const RtPathTraceFrameCameraState& view, bool objectMotionAvailable)
{
    primarySurfaceHistoryView = view;
    primarySurfaceHistoryView.valid = true;
    primarySurfaceHistoryState.currentValid = true;
    primarySurfaceHistoryState.previousValid = true;
    primarySurfaceHistoryState.samePixelHistoryValid = true;
    primarySurfaceHistoryState.cameraReprojectionAvailable = true;
    primarySurfaceHistoryState.objectMotionAvailable = objectMotionAvailable;
}

void RtPathTraceFrameResources::InvalidatePrimarySurfaceHistory(uint32_t reasonFlags)
{
    primarySurfaceHistoryNeedsClear = true;
    primarySurfaceHistoryState.Reset(reasonFlags | RT_FRAME_RESET_PRIMARY_HISTORY);
    primarySurfaceHistoryView.Reset();
    MarkResetReason(reasonFlags | RT_FRAME_RESET_PRIMARY_HISTORY);
}

void RtPathTraceFrameResources::RecordSceneResourceCommit(uint64_t uploadBytes, bool rebuiltBindingSet, bool committedAccelerationStructures)
{
    diagnostics.sceneUploadBytes = uploadBytes;
    if (rebuiltBindingSet)
    {
        diagnostics.descriptorBindingSetRebuilds++;
    }
    if (committedAccelerationStructures)
    {
        diagnostics.blasTlasCommits++;
    }
}

void RtPathTraceFrameResources::RecordReadbackQueued()
{
    diagnostics.readbacksQueued++;
}

void RtPathTraceFrameResources::RecordReadbackMapped()
{
    diagnostics.readbacksMapped++;
}

void RtPathTraceFrameResources::RecordReadbackUnmapped()
{
    diagnostics.readbacksUnmapped++;
}

void RtPathTraceFrameResources::DescribeResetReasons(idStr& out) const
{
    out.Clear();
    const uint32_t reasons = settings.resetReasonFlags;
    if (reasons == RT_FRAME_RESET_NONE)
    {
        out = "none";
        return;
    }
    if ((reasons & RT_FRAME_RESET_OUTPUT_RESIZE) != 0)
    {
        AppendReason(out, "output-resize");
    }
    if ((reasons & RT_FRAME_RESET_BACKBUFFER_RESIZE) != 0)
    {
        AppendReason(out, "backbuffer-resize");
    }
    if ((reasons & RT_FRAME_RESET_SCENE_RESOURCES) != 0)
    {
        AppendReason(out, "scene-resources");
    }
    if ((reasons & RT_FRAME_RESET_RESERVOIR_SCENE_SIGNATURE) != 0)
    {
        AppendReason(out, "reservoir-scene-signature");
    }
    if ((reasons & RT_FRAME_RESET_RESERVOIR_DISPATCH_SIGNATURE) != 0)
    {
        AppendReason(out, "reservoir-dispatch-signature");
    }
    if ((reasons & RT_FRAME_RESET_PRIMARY_HISTORY) != 0)
    {
        AppendReason(out, "primary-history");
    }
    if ((reasons & RT_FRAME_RESET_GPU_IDLE_WAIT) != 0)
    {
        AppendReason(out, "gpu-idle-wait");
    }
}

void RtPathTraceFrameResources::PrintDiagnostics(const char* prefix) const
{
    idStr resetReasons;
    DescribeResetReasons(resetReasons);

    common->Printf("%s: PT frame resources render=%dx%d output=%dx%d debugMode=%d checkerboard=%d frame=%u resetReasons=%s valid output/accum/rrInput/motion/motionMask/rrGuides/readback=%d/%d/%d/%d/%d/%d/%d smokeReservoir=%d restirReservoir=%d primaryHistory=%d primaryState current/previous/samePixel/reproject/objectMotion=%d/%d/%d/%d/%d bytes output=%llu motion=%llu motionMask=%llu rrGuides=%llu smokeReservoir=%llu restirReservoir=%llu primaryHistory=%llu sceneUpload=%llu recreate output/motion/motionMask/rrGuides/readback=%d/%d/%d/%d/%d buffers smoke(reuse/recreate)=%d/%d restir(reuse/recreate)=%d/%d primaryHistory(reuse/recreate)=%d/%d descriptors=%d blasTlas=%d readback queued/mapped/unmapped=%d/%d/%d waitForIdle=%d reason=%s\n",
        prefix ? prefix : "PathTraceFrameResources",
        width,
        height,
        outputWidth,
        outputHeight,
        settings.debugMode,
        static_cast<int>(settings.checkerboardMode),
        settings.frameIndex,
        resetReasons.c_str(),
        outputTexture ? 1 : 0,
        accumulationTexture ? 1 : 0,
        rrInputColorTexture ? 1 : 0,
        motionVectorTexture ? 1 : 0,
        motionVectorMaskTexture ? 1 : 0,
        (rrGuideAlbedoTexture && rrGuideSpecularAlbedoTexture && rrGuideNormalRoughnessTexture && rrGuideDepthTexture && rrGuideHitDistanceTexture && rrGuideResetMaskTexture && rrGuidePositionTexture) ? 1 : 0,
        readbackTexture ? 1 : 0,
        smokeReservoirBuffers.IsValidFor(width, height) ? 1 : 0,
        restirPTReservoirBuffers.IsValidFor(static_cast<uint32_t>(width), static_cast<uint32_t>(height), settings.checkerboardMode) ? 1 : 0,
        primarySurfaceHistoryBuffers.IsValidFor(static_cast<uint32_t>(width), static_cast<uint32_t>(height)) ? 1 : 0,
        primarySurfaceHistoryState.currentValid ? 1 : 0,
        primarySurfaceHistoryState.previousValid ? 1 : 0,
        primarySurfaceHistoryState.samePixelHistoryValid ? 1 : 0,
        primarySurfaceHistoryState.cameraReprojectionAvailable ? 1 : 0,
        primarySurfaceHistoryState.objectMotionAvailable ? 1 : 0,
        static_cast<unsigned long long>(diagnostics.outputTextureBytes),
        static_cast<unsigned long long>(diagnostics.motionVectorBytes),
        static_cast<unsigned long long>(diagnostics.motionVectorMaskBytes),
        static_cast<unsigned long long>(diagnostics.rrGuideBytes),
        static_cast<unsigned long long>(diagnostics.smokeReservoirBytes),
        static_cast<unsigned long long>(diagnostics.restirPTReservoirBytes),
        static_cast<unsigned long long>(diagnostics.primarySurfaceHistoryBytes),
        static_cast<unsigned long long>(diagnostics.sceneUploadBytes),
        diagnostics.outputTexturesCreated,
        diagnostics.motionVectorTexturesCreated,
        diagnostics.motionVectorMaskTexturesCreated,
        diagnostics.rrGuideTexturesCreated,
        diagnostics.diagnosticReadbackResourcesCreated,
        diagnostics.smokeReservoirBuffersReused,
        diagnostics.smokeReservoirBuffersRecreated,
        diagnostics.restirPTReservoirBuffersReused,
        diagnostics.restirPTReservoirBuffersRecreated,
        diagnostics.primarySurfaceHistoryBuffersReused,
        diagnostics.primarySurfaceHistoryBuffersRecreated,
        diagnostics.descriptorBindingSetRebuilds,
        diagnostics.blasTlasCommits,
        diagnostics.readbacksQueued,
        diagnostics.readbacksMapped,
        diagnostics.readbacksUnmapped,
        diagnostics.waitForIdleCalls,
        diagnostics.lastWaitForIdleReason ? diagnostics.lastWaitForIdleReason : "");
}
