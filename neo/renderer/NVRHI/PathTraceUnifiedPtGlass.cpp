#include "precompiled.h"
#pragma hdrstop

#include "PathTraceUnifiedPtGlass.h"

#include <nvrhi/utils.h>

namespace {

static constexpr uint32_t UPT45_PRODUCER_PUSH_BYTES = 112u;
static constexpr uint32_t UPT45_COMPOSE_PUSH_BYTES = 16u;

struct Upt45Control
{
    uint32_t width;
    uint32_t height;
    uint32_t surfaceCount;
    uint32_t materialCount;
    uint32_t logicalTextureCount;
    uint32_t staticVertexCount;
    uint32_t staticIndexCount;
    uint32_t staticTriangleCount;
    uint32_t dynamicVertexCount;
    uint32_t dynamicIndexCount;
    uint32_t dynamicTriangleCount;
    uint32_t rigidVertexCount;
    uint32_t rigidIndexCount;
    uint32_t rigidInstanceCount;
    uint32_t skinnedVertexCount;
    uint32_t skinnedIndexCount;
    uint32_t skinnedRouteCount;
    uint32_t skinnedTriangleCount;
    uint32_t skinnedPreviousCount;
    uint32_t flags;
    float cameraOrigin[3];
    float emissiveScale;
    float forwardOffset;
    float transmissionStrength;
    float glassTintStrength;
    float reserved0;
};
static_assert(sizeof(Upt45Control) == UPT45_PRODUCER_PUSH_BYTES,
    "UPT-45 producer push ABI mismatch");

struct Upt45ComposeControl
{
    uint32_t width;
    uint32_t height;
    float overlayStrength;
    uint32_t reserved0;
};
static_assert(sizeof(Upt45ComposeControl) == UPT45_COMPOSE_PUSH_BYTES,
    "UPT-45 compose push ABI mismatch");

class Upt45MarkerScope
{
public:
    Upt45MarkerScope(nvrhi::ICommandList* commandList, const char* name, bool enabled)
        : m_commandList(enabled ? commandList : nullptr)
    {
        if (m_commandList)
            m_commandList->beginMarker(name);
    }
    ~Upt45MarkerScope()
    {
        if (m_commandList)
            m_commandList->endMarker();
    }
private:
    nvrhi::ICommandList* m_commandList;
};

static bool Upt45ReadShader(
    const char* path,
    void*& data,
    int& size,
    ID_TIME_T& timestamp)
{
    size = fileSystem->ReadFile(path, &data, &timestamp);
    if (!data || size <= 0)
    {
        common->Printf("PathTraceUnifiedPt: couldn't read native glass SPIR-V %s\n", path);
        return false;
    }
    return true;
}

static void Upt45AddProducerLayoutItems(nvrhi::BindingLayoutDesc& desc)
{
    desc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3));
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(4));
    desc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(5));
    for (uint32_t slot = 6u; slot <= 24u; ++slot)
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(slot));
    desc.addItem(nvrhi::BindingLayoutItem::Sampler(25));
    desc.addItem(nvrhi::BindingLayoutItem::PushConstants(
        0, UPT45_PRODUCER_PUSH_BYTES));
}

static bool Upt45InputsValid(const PathTraceUnifiedPtGlassInputs& inputs)
{
    if (!inputs.device || !inputs.commandList || !inputs.sceneInputs ||
        !inputs.sceneInputs->valid || !inputs.primarySurface32 ||
        !inputs.primaryHistorySidecar || inputs.width == 0u || inputs.height == 0u)
        return false;
    const RtPathTraceSceneInputGeometry& g = inputs.sceneInputs->geometry;
    const RtPathTraceSceneInputMaterials& m = inputs.sceneInputs->materials;
    return g.tlas && g.staticVertexBuffer && g.staticIndexBuffer &&
        g.staticTriangleClassBuffer && g.staticTriangleMaterialBuffer &&
        g.staticTriangleMaterialIndexBuffer && g.dynamicVertexBuffer &&
        g.dynamicIndexBuffer && g.dynamicTriangleClassBuffer &&
        g.dynamicTriangleMaterialBuffer && g.dynamicTriangleMaterialIndexBuffer &&
        g.rigidRouteVertexBuffer && g.rigidRouteIndexBuffer &&
        g.rigidRouteInstanceBuffer && g.skinnedCurrentOutputVertexBuffer &&
        g.skinnedSourceIndexBuffer && g.skinnedHitRouteRecordBuffer &&
        g.skinnedHitRouteTriangleBuffer && g.skinnedPreviousPositionBuffer &&
        m.materialTableBuffer && m.textureBindlessLayout &&
        m.textureDescriptorTable && m.textureSampler;
}

static nvrhi::TextureHandle Upt45CreateTexture(
    nvrhi::IDevice* device,
    uint32_t width,
    uint32_t height,
    const char* name)
{
    nvrhi::TextureDesc desc;
    desc.width = width;
    desc.height = height;
    desc.mipLevels = 1;
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.isUAV = true;
    desc.debugName = name;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    return device->createTexture(desc);
}

} // namespace

bool PathTraceUnifiedPtGlassState::EnsureResources(
    const PathTraceUnifiedPtGlassInputs& inputs)
{
    if (m_compositionToken &&
        m_width == inputs.width && m_height == inputs.height)
        return true;
    m_compositionToken = Upt45CreateTexture(
        inputs.device, inputs.width, inputs.height,
        "PathTraceUnifiedPtGlassCompositionToken");
    if (!m_compositionToken)
        return false;
    m_width = inputs.width;
    m_height = inputs.height;
    m_producerBindingSet = nullptr;
    m_producerBindingSetDescValid = false;
    m_composeBindingSet = nullptr;
    m_composeBindingSetDescValid = false;
    common->Printf(
        "PathTraceUnifiedPt: native glass resources %ux%u token=RGBA16F bytes=%llu clear=never composedOutput=shared-frame\n",
        inputs.width, inputs.height,
        static_cast<unsigned long long>(
            uint64_t(inputs.width) * uint64_t(inputs.height) * 8ull));
    return true;
}

bool PathTraceUnifiedPtGlassState::EnsureProducerPipeline(
    const PathTraceUnifiedPtGlassInputs& inputs)
{
    if (m_producerPipeline)
        return true;
    if (m_producerPipelineAttempted)
        return false;
    m_producerPipelineAttempted = true;
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);
    Upt45AddProducerLayoutItems(layoutDesc);
    m_producerLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!m_producerLayout)
        return false;
    void* data = nullptr;
    int size = 0;
    ID_TIME_T timestamp = 0;
    if (!Upt45ReadShader(
            "renderprogs2/spirv/builtin/pathtracing/slang_upt45/upt45_clear_window_psr.bin",
            data, size, timestamp))
        return false;
    nvrhi::ShaderDesc shaderDesc;
    shaderDesc.shaderType = nvrhi::ShaderType::Compute;
    shaderDesc.entryName = "main";
    shaderDesc.debugName = "PathTraceUnifiedPtNativeGlassPsr";
    m_producerShader = inputs.device->createShader(shaderDesc, data, size);
    Mem_Free(data);
    if (!m_producerShader)
        return false;
    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = m_producerShader;
    pipelineDesc.bindingLayouts = {
        m_producerLayout,
        inputs.sceneInputs->materials.textureBindlessLayout };
    m_producerPipeline = inputs.device->createComputePipeline(pipelineDesc);
    if (!m_producerPipeline)
        return false;
    common->Printf(
        "PathTraceUnifiedPt: native glass producer compiler=slang blobBytes=%d timestamp=%lld groups=8x8 raysMax=1 wideReceiver=0 cleanDiBindings=0\n",
        size, static_cast<long long>(timestamp));
    return true;
}

bool PathTraceUnifiedPtGlassState::EnsureProducerBindingSet(
    const PathTraceUnifiedPtGlassInputs& inputs)
{
    const RtPathTraceSceneInputGeometry& g = inputs.sceneInputs->geometry;
    const RtPathTraceSceneInputMaterials& m = inputs.sceneInputs->materials;
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, g.tlas));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(3, inputs.primarySurface32));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(4, inputs.primaryHistorySidecar));
    desc.addItem(nvrhi::BindingSetItem::Texture_UAV(5, m_compositionToken));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, g.staticVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, g.staticIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(8, g.staticTriangleClassBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(9, g.staticTriangleMaterialBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(10, g.staticTriangleMaterialIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(11, g.dynamicVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(12, g.dynamicIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(13, g.dynamicTriangleClassBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(14, g.dynamicTriangleMaterialBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(15, g.dynamicTriangleMaterialIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(16, m.materialTableBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(17, g.rigidRouteVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(18, g.rigidRouteIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(19, g.rigidRouteInstanceBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(20, g.skinnedCurrentOutputVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(21, g.skinnedSourceIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(22, g.skinnedHitRouteRecordBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(23, g.skinnedHitRouteTriangleBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(24, g.skinnedPreviousPositionBuffer));
    desc.addItem(nvrhi::BindingSetItem::Sampler(25, m.textureSampler));
    desc.addItem(nvrhi::BindingSetItem::PushConstants(0, UPT45_PRODUCER_PUSH_BYTES));
    if (m_producerBindingSet && m_producerBindingSetDescValid &&
        m_producerBindingSetDesc == desc)
        return true;
    m_producerBindingSet = inputs.device->createBindingSet(desc, m_producerLayout);
    if (!m_producerBindingSet)
        return false;
    m_producerBindingSetDesc = desc;
    m_producerBindingSetDescValid = true;
    return true;
}

bool PathTraceUnifiedPtGlassState::ExecuteProducer(
    const PathTraceUnifiedPtGlassInputs& inputs)
{
    if (!Upt45InputsValid(inputs) || !EnsureResources(inputs) ||
        !EnsureProducerPipeline(inputs) || !EnsureProducerBindingSet(inputs))
        return false;
    const RtPathTraceSceneInputGeometry& g = inputs.sceneInputs->geometry;
    const RtPathTraceSceneInputMaterials& m = inputs.sceneInputs->materials;
    Upt45Control control = {};
    control.width = inputs.width;
    control.height = inputs.height;
    control.surfaceCount = inputs.width * inputs.height;
    control.materialCount = Max(0, m.materialTableEntryCount);
    control.logicalTextureCount = Max(0, m.logicalTextureDescriptorCount);
    control.staticVertexCount = Max(0, g.staticVertexCount);
    control.staticIndexCount = Max(0, g.staticIndexCount);
    control.staticTriangleCount = Max(0, g.staticTriangleCount);
    control.dynamicVertexCount = Max(0, g.dynamicVertexCount);
    control.dynamicIndexCount = Max(0, g.dynamicIndexCount);
    control.dynamicTriangleCount = Max(0, g.dynamicTriangleCount);
    control.rigidVertexCount = Max(0, g.rigidRouteVertexCount);
    control.rigidIndexCount = Max(0, g.rigidRouteIndexCount);
    control.rigidInstanceCount = Max(0, g.rigidRouteInstanceCount);
    if (g.skinnedCurrentOutputVertexBuffer)
    {
        const nvrhi::BufferDesc& skinnedDesc =
            g.skinnedCurrentOutputVertexBuffer->getDesc();
        control.skinnedVertexCount = skinnedDesc.structStride != 0u
            ? static_cast<uint32_t>(skinnedDesc.byteSize / skinnedDesc.structStride)
            : 0u;
    }
    control.skinnedIndexCount = Max(0, g.skinnedSourceIndexCount);
    control.skinnedRouteCount = Max(0, g.skinnedHitRouteRecordCount);
    control.skinnedTriangleCount = Max(0, g.skinnedHitRouteTriangleCount);
    control.skinnedPreviousCount = Max(0, g.skinnedPreviousPositionCount);
    memcpy(control.cameraOrigin, inputs.cameraOrigin, sizeof(control.cameraOrigin));
    control.emissiveScale = inputs.emissiveScale;
    control.forwardOffset = inputs.forwardOffset;
    control.transmissionStrength = inputs.transmissionStrength;
    control.glassTintStrength = inputs.glassTintStrength;

    inputs.commandList->setBufferState(inputs.primarySurface32, nvrhi::ResourceStates::UnorderedAccess);
    inputs.commandList->setBufferState(inputs.primaryHistorySidecar, nvrhi::ResourceStates::UnorderedAccess);
    inputs.commandList->setTextureState(m_compositionToken, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    inputs.commandList->commitBarriers();
    nvrhi::ComputeState state;
    state.pipeline = m_producerPipeline;
    state.bindings = {
        m_producerBindingSet,
        inputs.sceneInputs->materials.textureDescriptorTable };
    inputs.commandList->setComputeState(state);
    inputs.commandList->setPushConstants(&control, sizeof(control));
    {
        Upt45MarkerScope marker(inputs.commandList,
            "UPT.PSR NativeClearWindow", inputs.nsightMarkers);
        inputs.commandList->dispatch(
            (inputs.width + 7u) / 8u,
            (inputs.height + 7u) / 8u,
            1u);
    }
    nvrhi::utils::BufferUavBarrier(inputs.commandList, inputs.primarySurface32);
    nvrhi::utils::BufferUavBarrier(inputs.commandList, inputs.primaryHistorySidecar);
    nvrhi::utils::TextureUavBarrier(inputs.commandList, m_compositionToken);
    return true;
}

bool PathTraceUnifiedPtGlassState::EnsureComposePipeline(
    const PathTraceUnifiedPtGlassInputs& inputs)
{
    if (m_composePipeline)
        return true;
    if (m_composePipelineAttempted)
        return false;
    m_composePipelineAttempted = true;
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(1));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(2));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
        0, UPT45_COMPOSE_PUSH_BYTES));
    m_composeLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!m_composeLayout)
        return false;
    void* data = nullptr;
    int size = 0;
    ID_TIME_T timestamp = 0;
    if (!Upt45ReadShader(
            "renderprogs2/spirv/builtin/pathtracing/slang_upt45/upt45_glass_compose.bin",
            data, size, timestamp))
        return false;
    nvrhi::ShaderDesc shaderDesc;
    shaderDesc.shaderType = nvrhi::ShaderType::Compute;
    shaderDesc.entryName = "main";
    shaderDesc.debugName = "PathTraceUnifiedPtNativeGlassCompose";
    m_composeShader = inputs.device->createShader(shaderDesc, data, size);
    Mem_Free(data);
    if (!m_composeShader)
        return false;
    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = m_composeShader;
    pipelineDesc.bindingLayouts = { m_composeLayout };
    m_composePipeline = inputs.device->createComputePipeline(pipelineDesc);
    if (!m_composePipeline)
        return false;
    common->Printf(
        "PathTraceUnifiedPt: native glass compose compiler=slang blobBytes=%d timestamp=%lld groups=8x8 rays=0\n",
        size, static_cast<long long>(timestamp));
    return true;
}

bool PathTraceUnifiedPtGlassState::EnsureComposeBindingSet(
    const PathTraceUnifiedPtGlassInputs& inputs,
    nvrhi::TextureHandle resolvedColor)
{
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::Texture_SRV(0, resolvedColor));
    desc.addItem(nvrhi::BindingSetItem::Texture_SRV(1, m_compositionToken));
    desc.addItem(nvrhi::BindingSetItem::Texture_UAV(2, inputs.compositionOutput));
    desc.addItem(nvrhi::BindingSetItem::PushConstants(0, UPT45_COMPOSE_PUSH_BYTES));
    if (m_composeBindingSet && m_composeBindingSetDescValid &&
        m_composeBindingSetDesc == desc)
        return true;
    m_composeBindingSet = inputs.device->createBindingSet(desc, m_composeLayout);
    if (!m_composeBindingSet)
        return false;
    m_composeBindingSetDesc = desc;
    m_composeBindingSetDescValid = true;
    return true;
}

bool PathTraceUnifiedPtGlassState::ExecuteCompose(
    const PathTraceUnifiedPtGlassInputs& inputs,
    nvrhi::TextureHandle resolvedColor)
{
    if (!resolvedColor || !m_compositionToken || !inputs.compositionOutput ||
        !EnsureComposePipeline(inputs) ||
        !EnsureComposeBindingSet(inputs, resolvedColor))
        return false;
    Upt45ComposeControl control = {
        inputs.width, inputs.height, inputs.overlayStrength, 0u };
    inputs.commandList->setTextureState(resolvedColor, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    inputs.commandList->setTextureState(m_compositionToken, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    inputs.commandList->setTextureState(inputs.compositionOutput, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    inputs.commandList->commitBarriers();
    nvrhi::ComputeState state;
    state.pipeline = m_composePipeline;
    state.bindings = { m_composeBindingSet };
    inputs.commandList->setComputeState(state);
    inputs.commandList->setPushConstants(&control, sizeof(control));
    {
        Upt45MarkerScope marker(inputs.commandList,
            "UPT.Glass NativeCompose Last", inputs.nsightMarkers);
        inputs.commandList->dispatch(
            (inputs.width + 7u) / 8u,
            (inputs.height + 7u) / 8u,
            1u);
    }
    nvrhi::utils::TextureUavBarrier(inputs.commandList, inputs.compositionOutput);
    inputs.commandList->setTextureState(inputs.compositionOutput, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    inputs.commandList->commitBarriers();
    m_lastComposedOutput = inputs.compositionOutput;
    return true;
}

void PathTraceUnifiedPtGlassState::Release()
{
    m_width = 0;
    m_height = 0;
    m_compositionToken = nullptr;
    m_lastComposedOutput = nullptr;
    m_producerLayout = nullptr;
    m_producerBindingSet = nullptr;
    m_producerBindingSetDescValid = false;
    m_producerShader = nullptr;
    m_producerPipeline = nullptr;
    m_producerPipelineAttempted = false;
    m_composeLayout = nullptr;
    m_composeBindingSet = nullptr;
    m_composeBindingSetDescValid = false;
    m_composeShader = nullptr;
    m_composePipeline = nullptr;
    m_composePipelineAttempted = false;
}
