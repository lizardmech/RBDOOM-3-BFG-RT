#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCVars.h"
#include "PathTraceParticleCapture.h"
#include "PathTracePrimaryPass.h"
#include "../RenderCommon.h"
#include "../../sys/DeviceManager.h"

#include <algorithm>
#include <cstddef>

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

struct ParticleLightingConstants
{
    uint32_t taskCount = 0;
    uint32_t lightCount = 0;
    uint32_t candidateCount = 0;
    uint32_t traceVisibility = 0;
    float ambientFloor = 0.16f;
    float rayTMin = 0.1f;
    float rayTMaxBias = 0.2f;
    float directClamp = 8.0f;
};

// DXC's Vulkan StructuredBuffer layout aligns float3 to 16 bytes. Keep this
// record at the reflected offsets 0/16/24/28 and ArrayStride 32.
static_assert(offsetof(ParticleCompositeVertex, texCoord) == 16, "Particle composite texcoord ABI mismatch");
static_assert(offsetof(ParticleCompositeVertex, packedColor) == 24, "Particle composite color ABI mismatch");
static_assert(offsetof(ParticleCompositeVertex, lightingTaskIndex) == 32, "Particle composite lighting-task ABI mismatch");
static_assert(sizeof(ParticleCompositeVertex) == 48, "Particle composite vertex ABI mismatch");
static_assert(sizeof(ParticleCompositeConstants) == 112, "Particle composite constants ABI mismatch");
static_assert(sizeof(ParticleCompositeLightingTask) == 16, "Particle lighting-task ABI mismatch");
static_assert(sizeof(ParticleLightingConstants) == 32, "Particle lighting constants ABI mismatch");

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
    else if (blendClass == RtPathTraceParticleBlendClass::MultiplicativeDarken)
    {
        // Legacy black smoke uses GL_ZERO, GL_ONE_MINUS_SRC_COLOR: retain the
        // resolved scene and attenuate it by the smoke card's RGB coverage.
        blend.setSrcBlend(nvrhi::BlendFactor::Zero);
        blend.setDestBlend(nvrhi::BlendFactor::OneMinusSrcColor);
        blend.setSrcBlendAlpha(nvrhi::BlendFactor::Zero);
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
    // These upload buffers persist across command lists. NVRHI validation needs
    // their declared state retained so writeBuffer/setBufferState can begin from
    // a known state on the first composite frame and every frame thereafter.
    desc.keepInitialState = true;
    desc.debugName = name;
    return device->createBuffer(desc);
}

nvrhi::BufferHandle ParticleCompositeEnsureLightingOutput(
    nvrhi::IDevice* device,
    nvrhi::BufferHandle buffer,
    uint64_t byteSize)
{
    const uint64_t requiredBytes = Max(byteSize, static_cast<uint64_t>(sizeof(idVec4)));
    if (buffer && buffer->getDesc().byteSize >= requiredBytes)
    {
        return buffer;
    }
    nvrhi::BufferDesc desc;
    desc.byteSize = requiredBytes;
    desc.structStride = sizeof(idVec4);
    desc.canHaveUAVs = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    desc.debugName = "PathTraceParticleLightingOutput";
    return device->createBuffer(desc);
}

std::vector<uint32_t> ParticleCompositeBuildUploadIndexes(const RtPathTraceParticleCapture& capture)
{
    std::vector<uint32_t> uploadIndexes = capture.indexes;
    if (r_pathTracingParticleSortMode.GetInteger() == 0)
    {
        return uploadIndexes;
    }

    for (uint32_t batchIndex = 0; batchIndex < static_cast<uint32_t>(capture.batches.size()); ++batchIndex)
    {
        const ParticleCompositeBatch& batch = capture.batches[batchIndex];
        if (batch.blendClass != RtPathTraceParticleBlendClass::AlphaLit)
        {
            continue;
        }

        std::vector<const ParticleCompositePrimitive*> primitives;
        uint32_t primitiveIndexCount = 0;
        for (const ParticleCompositePrimitive& primitive : capture.primitives)
        {
            if (primitive.batchIndex == batchIndex)
            {
                primitives.push_back(&primitive);
                primitiveIndexCount += primitive.indexCount;
            }
        }
        if (primitives.size() < 2 || primitiveIndexCount != batch.indexCount)
        {
            continue;
        }

        std::stable_sort(
            primitives.begin(),
            primitives.end(),
            [](const ParticleCompositePrimitive* a, const ParticleCompositePrimitive* b)
            {
                return a->viewDepth > b->viewDepth;
            });

        uint32_t destinationIndex = batch.firstIndex;
        for (const ParticleCompositePrimitive* primitive : primitives)
        {
            for (uint32_t localIndex = 0; localIndex < primitive->indexCount; ++localIndex)
            {
                uploadIndexes[destinationIndex++] = capture.indexes[primitive->firstIndex + localIndex];
            }
        }
    }
    return uploadIndexes;
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
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3));
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

    for (int blendIndex = 0; blendIndex < static_cast<int>(RtPathTraceParticleBlendClass::Count); ++blendIndex)
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

    const std::vector<uint32_t> uploadIndexes = ParticleCompositeBuildUploadIndexes(m_particleCapture);
    const uint64_t vertexBytes = m_particleCapture.vertices.size() * sizeof(ParticleCompositeVertex);
    const uint64_t indexBytes = uploadIndexes.size() * sizeof(uint32_t);
    m_particleCompositeVertexBuffer = ParticleCompositeEnsureBuffer(
        device, m_particleCompositeVertexBuffer, "PathTraceParticleCompositeVertices", vertexBytes, sizeof(ParticleCompositeVertex), false);
    m_particleCompositeIndexBuffer = ParticleCompositeEnsureBuffer(
        device, m_particleCompositeIndexBuffer, "PathTraceParticleCompositeIndexes", indexBytes, sizeof(uint32_t), true);
    const uint64_t lightingOutputBytes = m_particleCapture.lightingTasks.size() * sizeof(idVec4);
    m_particleLightingOutputBuffer = ParticleCompositeEnsureLightingOutput(
        device, m_particleLightingOutputBuffer, lightingOutputBytes);
    if (!m_particleCompositeVertexBuffer || !m_particleCompositeIndexBuffer || !m_particleLightingOutputBuffer)
    {
        return;
    }

    commandList->writeBuffer(m_particleCompositeVertexBuffer, m_particleCapture.vertices.data(), vertexBytes);
    commandList->writeBuffer(m_particleCompositeIndexBuffer, uploadIndexes.data(), indexBytes);

    bool particleLightingReady = false;
    const bool particleLightingRequested = r_pathTracingParticleLighting.GetBool() &&
        !m_particleCapture.lightingTasks.empty() && m_smokeTlas &&
        m_smokeRestirLightManagerCurrentPayloadBuffer && m_smokeRestirLightManagerCurrentPayloadCount > 0;
    if (particleLightingRequested)
    {
        if (!m_particleLightingBindingLayout)
        {
            nvrhi::BindingLayoutDesc lightingLayoutDesc;
            lightingLayoutDesc.visibility = nvrhi::ShaderType::Compute;
            lightingLayoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
                .setShaderResourceOffset(0)
                .setUnorderedAccessViewOffset(0)
                .setConstantBufferOffset(0);
            lightingLayoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, sizeof(ParticleLightingConstants)));
            lightingLayoutDesc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
            lightingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
            lightingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2));
            lightingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3));
            m_particleLightingBindingLayout = device->createBindingLayout(lightingLayoutDesc);
            const programInfo_t lightingProgram = renderProgManager.GetProgramInfo(BUILTIN_PT_PARTICLE_LIGHTING_CS);
            m_particleLightingShader = lightingProgram.cs;
            if (m_particleLightingBindingLayout && m_particleLightingShader)
            {
                nvrhi::ComputePipelineDesc pipelineDesc;
                pipelineDesc.CS = m_particleLightingShader;
                pipelineDesc.bindingLayouts = { m_particleLightingBindingLayout };
                m_particleLightingPipeline = device->createComputePipeline(pipelineDesc);
            }
        }

        const uint64_t lightingTaskBytes = m_particleCapture.lightingTasks.size() * sizeof(ParticleCompositeLightingTask);
        m_particleLightingTaskBuffer = ParticleCompositeEnsureBuffer(
            device,
            m_particleLightingTaskBuffer,
            "PathTraceParticleLightingTasks",
            lightingTaskBytes,
            sizeof(ParticleCompositeLightingTask),
            false);
        if (m_particleLightingPipeline && m_particleLightingTaskBuffer)
        {
            commandList->writeBuffer(m_particleLightingTaskBuffer, m_particleCapture.lightingTasks.data(), lightingTaskBytes);
            commandList->setAccelStructState(m_smokeTlas, nvrhi::ResourceStates::AccelStructRead);
            commandList->setBufferState(m_particleLightingTaskBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRestirLightManagerCurrentPayloadBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_particleLightingOutputBuffer, nvrhi::ResourceStates::UnorderedAccess);
            commandList->commitBarriers();

            nvrhi::BindingSetDesc lightingBindingSetDesc;
            lightingBindingSetDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(ParticleLightingConstants)));
            lightingBindingSetDesc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, m_smokeTlas));
            lightingBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_particleLightingTaskBuffer));
            lightingBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_smokeRestirLightManagerCurrentPayloadBuffer));
            lightingBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(3, m_particleLightingOutputBuffer));
            nvrhi::BindingSetHandle lightingBindingSet = device->createBindingSet(
                lightingBindingSetDesc,
                m_particleLightingBindingLayout);
            if (lightingBindingSet)
            {
                ParticleLightingConstants lightingConstants;
                lightingConstants.taskCount = static_cast<uint32_t>(m_particleCapture.lightingTasks.size());
                lightingConstants.lightCount = static_cast<uint32_t>(m_smokeRestirLightManagerCurrentPayloadCount);
                lightingConstants.candidateCount = static_cast<uint32_t>(idMath::ClampInt(
                    1, 32, r_pathTracingParticleLightCandidates.GetInteger()));
                lightingConstants.traceVisibility = r_pathTracingParticleShadowRays.GetInteger() > 0 ? 1u : 0u;
                lightingConstants.ambientFloor = Max(0.0f, r_pathTracingParticleAmbient.GetFloat());

                nvrhi::ComputeState lightingState;
                lightingState.pipeline = m_particleLightingPipeline;
                lightingState.bindings = { lightingBindingSet };
                commandList->setComputeState(lightingState);
                commandList->setPushConstants(&lightingConstants, sizeof(lightingConstants));
                commandList->dispatch((lightingConstants.taskCount + 63u) / 64u, 1u, 1u);
                commandList->setBufferState(m_particleLightingOutputBuffer, nvrhi::ResourceStates::ShaderResource);
                commandList->commitBarriers();
                particleLightingReady = true;
            }
        }
    }

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
        const int blendIndex = idMath::ClampInt(
            0,
            static_cast<int>(RtPathTraceParticleBlendClass::Count) - 1,
            static_cast<int>(batch.blendClass));
        if (!texture || !m_particleCompositePipelines[blendIndex])
        {
            continue;
        }

        nvrhi::BindingSetDesc bindingSetDesc;
        bindingSetDesc.addItem(nvrhi::BindingSetItem::PushConstants(0, sizeof(ParticleCompositeConstants)));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_particleCompositeVertexBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(1, texture));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(2, m_frameResources.rrGuidePositionTexture));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, m_particleLightingOutputBuffer));
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
        constants.modelInfo.Set(
            batch.modelDepthHack,
            Max(r_znear.GetFloat(), 1.0e-4f),
            idMath::ClampFloat(0.0f, 1.0f, r_pathTracingParticleOpacity.GetFloat()),
            particleLightingReady ? 1.0f : 0.0f);

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
