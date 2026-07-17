#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCVars.h"
#include "PathTraceParticleCapture.h"
#include "PathTracePrimaryPass.h"
#include "../RenderCommon.h"
#include "../../sys/DeviceManager.h"

extern DeviceManager* deviceManager;

namespace {

struct ParticleCompositeConstants
{
    idVec4 cameraOriginAndTanX;
    idVec4 cameraForwardAndTanY;
    idVec4 cameraLeftAndAmbient;
    idVec4 cameraUpAndEmissiveScale;
    idVec4 outputAndRenderSize;
    idVec4 batchInfo;
    idVec4 modelInfo;
};

static_assert(sizeof(ParticleCompositeVertex) == 28, "Particle composite vertex ABI mismatch");
static_assert(sizeof(ParticleCompositeConstants) == 112, "Particle composite constants ABI mismatch");

nvrhi::BlendState::RenderTarget ParticleCompositeBlendState(RtPathTraceParticleBlendClass blendClass)
{
    nvrhi::BlendState::RenderTarget blend;
    blend.blendEnable = true;
    if (blendClass == RtPathTraceParticleBlendClass::AlphaLit)
    {
        blend.setSrcBlend(nvrhi::BlendFactor::SrcAlpha);
        blend.setDestBlend(nvrhi::BlendFactor::OneMinusSrcAlpha);
        blend.setSrcBlendAlpha(nvrhi::BlendFactor::One);
        blend.setDestBlendAlpha(nvrhi::BlendFactor::OneMinusSrcAlpha);
    }
    else if (blendClass == RtPathTraceParticleBlendClass::AlphaEmissive)
    {
        blend.setSrcBlend(nvrhi::BlendFactor::SrcAlpha);
        blend.setDestBlend(nvrhi::BlendFactor::One);
        blend.setSrcBlendAlpha(nvrhi::BlendFactor::One);
        blend.setDestBlendAlpha(nvrhi::BlendFactor::One);
    }
    else
    {
        blend.setSrcBlend(nvrhi::BlendFactor::One);
        blend.setDestBlend(nvrhi::BlendFactor::One);
        blend.setSrcBlendAlpha(nvrhi::BlendFactor::One);
        blend.setDestBlendAlpha(nvrhi::BlendFactor::One);
    }
    return blend;
}

nvrhi::BufferHandle ParticleCompositeEnsureBuffer(
    nvrhi::IDevice* device,
    nvrhi::BufferHandle buffer,
    const char* name,
    uint64_t byteSize,
    uint32_t stride,
    bool indexBuffer)
{
    const uint64_t requiredBytes = Max(byteSize, static_cast<uint64_t>(stride));
    if (buffer && buffer->getDesc().byteSize >= requiredBytes)
    {
        return buffer;
    }
    nvrhi::BufferDesc desc;
    desc.byteSize = requiredBytes;
    desc.structStride = indexBuffer ? 0u : stride;
    desc.isIndexBuffer = indexBuffer;
    desc.initialState = indexBuffer ? nvrhi::ResourceStates::IndexBuffer : nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = false;
    desc.debugName = name;
    return device->createBuffer(desc);
}

}

void PathTracePrimaryPass::ExecutePathTraceParticleComposite(nvrhi::ICommandList* commandList, const viewDef_t* viewDef)
{
    if (!commandList || !viewDef || !m_particleCapture.enabled || m_particleCapture.batches.empty() ||
        m_particleCapture.vertices.empty() || m_particleCapture.indexes.empty() || !m_frameResources.outputTexture ||
        !m_frameResources.rrGuidePositionTexture || !m_backend || !deviceManager ||
        deviceManager->GetGraphicsAPI() != nvrhi::GraphicsAPI::VULKAN)
    {
        return;
    }

    nvrhi::IDevice* device = deviceManager->GetDevice();
    if (!device)
    {
        return;
    }

    if (!m_particleCompositeBindingLayout)
    {
        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::AllGraphics;
        layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets();
        layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ParticleCompositeConstants)));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(1));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(2));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
        m_particleCompositeBindingLayout = device->createBindingLayout(layoutDesc);

        const programInfo_t program = renderProgManager.GetProgramInfo(BUILTIN_PT_PARTICLE_COMPOSITE);
        m_particleCompositeVertexShader = program.vs;
        m_particleCompositePixelShader = program.ps;
    }
    if (!m_particleCompositeBindingLayout || !m_particleCompositeVertexShader || !m_particleCompositePixelShader)
    {
        return;
    }

    if (m_particleCompositeFramebufferTexture.Get() != m_frameResources.outputTexture.Get())
    {
        m_particleCompositeFramebufferTexture = m_frameResources.outputTexture;
        m_particleCompositeFramebuffer = device->createFramebuffer(
            nvrhi::FramebufferDesc().addColorAttachment(m_frameResources.outputTexture));
        for (nvrhi::GraphicsPipelineHandle& pipeline : m_particleCompositePipelines)
        {
            pipeline = nullptr;
        }
    }
    if (!m_particleCompositeFramebuffer)
    {
        return;
    }

    for (int blendIndex = 0; blendIndex < 3; ++blendIndex)
    {
        if (m_particleCompositePipelines[blendIndex])
        {
            continue;
        }
        nvrhi::GraphicsPipelineDesc pipelineDesc;
        pipelineDesc.VS = m_particleCompositeVertexShader;
        pipelineDesc.PS = m_particleCompositePixelShader;
        pipelineDesc.bindingLayouts = { m_particleCompositeBindingLayout };
        pipelineDesc.primType = nvrhi::PrimitiveType::TriangleList;
        pipelineDesc.renderState.rasterState.setCullNone();
        pipelineDesc.renderState.depthStencilState.depthTestEnable = false;
        pipelineDesc.renderState.depthStencilState.stencilEnable = false;
        pipelineDesc.renderState.blendState.targets[0] = ParticleCompositeBlendState(
            static_cast<RtPathTraceParticleBlendClass>(blendIndex));
        m_particleCompositePipelines[blendIndex] = device->createGraphicsPipeline(pipelineDesc, m_particleCompositeFramebuffer);
    }

    const uint64_t vertexBytes = m_particleCapture.vertices.size() * sizeof(ParticleCompositeVertex);
    const uint64_t indexBytes = m_particleCapture.indexes.size() * sizeof(uint32_t);
    m_particleCompositeVertexBuffer = ParticleCompositeEnsureBuffer(
        device, m_particleCompositeVertexBuffer, "PathTraceParticleCompositeVertices", vertexBytes, sizeof(ParticleCompositeVertex), false);
    m_particleCompositeIndexBuffer = ParticleCompositeEnsureBuffer(
        device, m_particleCompositeIndexBuffer, "PathTraceParticleCompositeIndexes", indexBytes, sizeof(uint32_t), true);
    if (!m_particleCompositeVertexBuffer || !m_particleCompositeIndexBuffer)
    {
        return;
    }

    commandList->writeBuffer(m_particleCompositeVertexBuffer, m_particleCapture.vertices.data(), vertexBytes);
    commandList->writeBuffer(m_particleCompositeIndexBuffer, m_particleCapture.indexes.data(), indexBytes);
    commandList->setBufferState(m_particleCompositeVertexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(m_particleCompositeIndexBuffer, nvrhi::ResourceStates::IndexBuffer);
    commandList->setTextureState(m_frameResources.rrGuidePositionTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::RenderTarget);
    commandList->commitBarriers();

    ParticleCompositeConstants constants;
    constants.cameraOriginAndTanX.Set(
        viewDef->renderView.vieworg.x, viewDef->renderView.vieworg.y, viewDef->renderView.vieworg.z,
        idMath::Tan(DEG2RAD(viewDef->renderView.fov_x * 0.5f)));
    constants.cameraForwardAndTanY.Set(
        viewDef->renderView.viewaxis[0].x, viewDef->renderView.viewaxis[0].y, viewDef->renderView.viewaxis[0].z,
        idMath::Tan(DEG2RAD(viewDef->renderView.fov_y * 0.5f)));
    constants.cameraLeftAndAmbient.Set(
        viewDef->renderView.viewaxis[1].x, viewDef->renderView.viewaxis[1].y, viewDef->renderView.viewaxis[1].z,
        Max(0.0f, r_pathTracingParticleAmbient.GetFloat()));
    constants.cameraUpAndEmissiveScale.Set(
        viewDef->renderView.viewaxis[2].x, viewDef->renderView.viewaxis[2].y, viewDef->renderView.viewaxis[2].z,
        Max(0.0f, r_pathTracingParticleEmissiveScale.GetFloat()));
    constants.outputAndRenderSize.Set(
        static_cast<float>(m_frameResources.outputWidth), static_cast<float>(m_frameResources.outputHeight),
        static_cast<float>(m_frameResources.width), static_cast<float>(m_frameResources.height));

    const nvrhi::Viewport viewport(
        static_cast<float>(m_frameResources.outputWidth),
        static_cast<float>(m_frameResources.outputHeight));
    for (const ParticleCompositeBatch& batch : m_particleCapture.batches)
    {
        if (batch.indexCount == 0 || batch.textureIndex >= m_particleCapture.textures.size())
        {
            continue;
        }
        const idImage* image = m_particleCapture.textures[batch.textureIndex];
        nvrhi::TextureHandle texture = image ? const_cast<idImage*>(image)->GetTextureHandle() : nullptr;
        const int blendIndex = idMath::ClampInt(0, 2, static_cast<int>(batch.blendClass));
        if (!texture || !m_particleCompositePipelines[blendIndex])
        {
            continue;
        }

        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(ParticleCompositeConstants)));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_particleCompositeVertexBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(1, texture));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(2, m_frameResources.rrGuidePositionTexture));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Sampler(0, m_backend->GetCommonPasses().m_AnisotropicWrapSampler));
        nvrhi::BindingSetHandle bindingSet = device->createBindingSet(bindingSetDesc, m_particleCompositeBindingLayout);
        if (!bindingSet)
        {
            continue;
        }

        constants.batchInfo.Set(
            static_cast<float>(batch.depthPolicy),
            static_cast<float>(batch.blendClass),
            Max(batch.softDepth, 1.0e-3f),
            m_particleCapture.debugTint ? 1.0f : 0.0f);
        constants.modelInfo.Set(batch.modelDepthHack, Max(r_znear.GetFloat(), 1.0e-4f), 0.0f, 0.0f);

        nvrhi::GraphicsState state;
        state.pipeline = m_particleCompositePipelines[blendIndex];
        state.framebuffer = m_particleCompositeFramebuffer;
        state.bindings = { bindingSet };
        state.indexBuffer = { m_particleCompositeIndexBuffer, nvrhi::Format::R32_UINT, 0 };
        state.viewport.addViewport(viewport);
        state.viewport.addScissorRect(nvrhi::Rect(viewport));
        commandList->setGraphicsState(state);
        commandList->setPushConstants(&constants, sizeof(constants));

        nvrhi::DrawArguments args;
        args.vertexCount = batch.indexCount;
        args.startIndexLocation = batch.firstIndex;
        commandList->drawIndexed(args);
    }

    commandList->setTextureState(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->commitBarriers();
}
