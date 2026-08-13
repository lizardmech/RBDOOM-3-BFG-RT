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
    uint32_t width = 0;
    uint32_t height = 0;
    float cameraOrigin[3] = {};
    float emissiveScale = 1.0f;
    float forwardOffset = 0.5f;
    float transmissionStrength = 0.92f;
    float glassTintStrength = 0.35f;
    float overlayStrength = 0.015f;
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
