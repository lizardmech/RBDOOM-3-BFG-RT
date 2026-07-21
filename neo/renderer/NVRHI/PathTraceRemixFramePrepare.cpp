#include "precompiled.h"
#pragma hdrstop

#include "PathTraceRemixFramePrepare.h"
#include "PathTraceFrameResources.h"

namespace {

uint32_t RemixStructuralResetFlags(uint32_t resetReasonFlags)
{
    return resetReasonFlags & (
        RT_FRAME_RESET_OUTPUT_RESIZE |
        RT_FRAME_RESET_BACKBUFFER_RESIZE |
        RT_FRAME_RESET_SCENE_RESOURCES |
        RT_FRAME_RESET_GPU_IDLE_WAIT);
}

}

void PathTraceRemixFramePrepare::Clear()
{
    m_observationPackage = PathTraceRemixFramePrepareObservationPackage();
    m_stats = PathTraceRemixFramePrepareStats();
    m_frameOpen = false;
}

void PathTraceRemixFramePrepare::BeginFrame(const PathTraceRemixFramePrepareDesc& desc)
{
    m_observationPackage = PathTraceRemixFramePrepareObservationPackage();
    m_observationPackage.frameIndex = desc.frameIndex;
    m_observationPackage.resetReasonFlags = desc.resetReasonFlags;
    m_observationPackage.previousSceneInputsValid = desc.previousSceneInputsValid;
    m_observationPackage.sceneSource = desc.sceneSource;
    m_observationPackage.debugMode = desc.debugMode;
    m_observationPackage.outputWidth = desc.outputWidth;
    m_observationPackage.outputHeight = desc.outputHeight;

    m_stats.preparedFrameIndex = desc.frameIndex;
    ++m_stats.beginFrameCount;
    m_stats.structuralResetReasonFlags = RemixStructuralResetFlags(desc.resetReasonFlags);
    m_stats.payloadObservationCount = 0;
    m_stats.mappingObservationCount = 0;
    m_stats.outputWidth = static_cast<uint32_t>(desc.outputWidth > 0 ? desc.outputWidth : 0);
    m_stats.outputHeight = static_cast<uint32_t>(desc.outputHeight > 0 ? desc.outputHeight : 0);
    m_stats.resourceAllocationCount = 0;
    m_stats.shaderRouteCount = 0;
    m_frameOpen = true;
}

void PathTraceRemixFramePrepare::SetLightInputs(const PathTraceRemixFramePrepareLightInputs& inputs)
{
    m_observationPackage.lights = inputs;
    ++m_stats.lightInputUpdateCount;
    m_stats.payloadObservationCount =
        inputs.emissiveObservationCount +
        inputs.previousEmissiveObservationCount +
        inputs.doomAnalyticObservationCount +
        inputs.previousDoomAnalyticObservationCount;
    m_stats.mappingObservationCount =
        inputs.doomAnalyticIdentityCount +
        inputs.previousDoomAnalyticIdentityCount +
        inputs.restirLightObservationCount;
}

void PathTraceRemixFramePrepare::EndFrame()
{
    if (!m_frameOpen)
    {
        return;
    }

    ++m_stats.endFrameCount;
    m_frameOpen = false;
}

const PathTraceRemixFramePrepareObservationPackage& PathTraceRemixFramePrepare::GetObservationPackage() const
{
    return m_observationPackage;
}

const PathTraceRemixFramePrepareStats& PathTraceRemixFramePrepare::GetStats() const
{
    return m_stats;
}
