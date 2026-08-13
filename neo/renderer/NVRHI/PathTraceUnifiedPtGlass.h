#pragma once

#include "PathTraceSceneInputs.h"

#include <nvrhi/nvrhi.h>

struct PathTraceUnifiedPtGlassInputs
{
    nvrhi::IDevice* device = nullptr;
    nvrhi::ICommandList* commandList = nullptr;
    const RtPathTraceSceneInputs* sceneInputs = nullptr;
    nvrhi::BufferHandle primarySurface32;
    nvrhi::BufferHandle primaryHistorySidecar;
    nvrhi::TextureHandle compositionOutput;
    nvrhi::TextureHandle motionVectorTexture;
    nvrhi::TextureHandle rrMotionVectorTexture;
    nvrhi::TextureHandle motionVectorMaskTexture;
    nvrhi::TextureHandle rrGuideAlbedoTexture;
    nvrhi::TextureHandle rrGuideSpecularAlbedoTexture;
    nvrhi::TextureHandle rrGuideNormalRoughnessTexture;
    nvrhi::TextureHandle rrGuideDepthTexture;
    nvrhi::TextureHandle rrGuideResetMaskTexture;
    nvrhi::TextureHandle rrGuidePositionTexture;
    uint32_t width = 0;
    uint32_t height = 0;
    float cameraOrigin[3] = {};
    float cameraForward[3] = {};
    float cameraLeft[3] = {};
    float cameraUp[3] = {};
    float cameraTanX = 1.0f;
    float cameraTanY = 1.0f;
    float previousCameraOrigin[3] = {};
    float previousCameraForward[3] = {};
    float previousCameraLeft[3] = {};
    float previousCameraUp[3] = {};
    float previousCameraTanX = 1.0f;
    float previousCameraTanY = 1.0f;
    float rrNear = 0.2f;
    float emissiveScale = 1.0f;
    float forwardOffset = 0.5f;
    float transmissionStrength = 0.92f;
    float glassTintStrength = 0.35f;
    float overlayStrength = 0.015f;
    bool writeRrGuides = false;
    bool previousCameraValid = false;
    bool nsightMarkers = false;
};

class PathTraceUnifiedPtGlassState
{
public:
    bool ExecuteProducer(const PathTraceUnifiedPtGlassInputs& inputs);
    bool ExecuteCompose(
        const PathTraceUnifiedPtGlassInputs& inputs,
        nvrhi::TextureHandle resolvedColor);
    nvrhi::TextureHandle GetComposedOutput() const { return m_lastComposedOutput; }
    void Release();

private:
    bool EnsureResources(const PathTraceUnifiedPtGlassInputs& inputs);
    bool EnsureProducerPipeline(const PathTraceUnifiedPtGlassInputs& inputs);
    bool EnsureProducerBindingSet(const PathTraceUnifiedPtGlassInputs& inputs);
    bool EnsureComposePipeline(const PathTraceUnifiedPtGlassInputs& inputs);
    bool EnsureComposeBindingSet(
        const PathTraceUnifiedPtGlassInputs& inputs,
        nvrhi::TextureHandle resolvedColor);

    uint32_t m_width = 0;
    uint32_t m_height = 0;
    nvrhi::TextureHandle m_compositionToken;
    nvrhi::TextureHandle m_lastComposedOutput;

    nvrhi::BindingLayoutHandle m_producerLayout;
    nvrhi::BindingSetHandle m_producerBindingSet;
    nvrhi::BindingSetDesc m_producerBindingSetDesc;
    bool m_producerBindingSetDescValid = false;
    nvrhi::ShaderHandle m_producerShader;
    nvrhi::ComputePipelineHandle m_producerPipeline;
    bool m_producerPipelineAttempted = false;

    nvrhi::BindingLayoutHandle m_composeLayout;
    nvrhi::BindingSetHandle m_composeBindingSet;
    nvrhi::BindingSetDesc m_composeBindingSetDesc;
    bool m_composeBindingSetDescValid = false;
    nvrhi::ShaderHandle m_composeShader;
    nvrhi::ComputePipelineHandle m_composePipeline;
    bool m_composePipelineAttempted = false;
};
