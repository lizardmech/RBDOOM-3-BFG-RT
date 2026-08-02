#include "precompiled.h"
#pragma hdrstop

#include "PathTraceUnifiedPt.h"

#include <nvrhi/utils.h>

#include <limits>

namespace {

static constexpr uint32_t UPT04_RESERVOIR_STRIDE = 64u;
static constexpr uint32_t UPT04_PUSH_CONSTANT_BYTES = 128u;
static constexpr uint32_t UPT04_FAMILY_LOCAL_LIGHT = 1u << 0u;
static constexpr uint32_t UPT04_FAMILY_INDIRECT = 1u << 1u;
static constexpr uint32_t UPT04_ROUTE_STATIC_BUCKETS = 1u << 1u;
static constexpr uint32_t UPT04_EMISSIVE_LOOKUP_EXACT = 1u << 3u;
static constexpr uint32_t UPT04_TRANSPORT_K_MAX = 2u;
static constexpr uint32_t UPT04_TRANSPORT_POLICY_ID = 1u;
static constexpr uint32_t UPT04_NEE_RIS_CANDIDATE_COUNT = 1u;
static constexpr uint32_t UPT04_ABI_VERSION = 5u;
static constexpr uint32_t UPT04_DIAGNOSTIC_COUNTER_COUNT = 16u;
static constexpr uint32_t UPT04_DIAGNOSTIC_BYTES =
    UPT04_DIAGNOSTIC_COUNTER_COUNT * sizeof(uint32_t);
static constexpr uint32_t UPT05_PUSH_CONSTANT_BYTES = 16u;

static const char* Upt04BackendName(PathTraceUnifiedPtBackend backend)
{
    return backend == PathTraceUnifiedPtBackend::RayQuery
        ? "rayquery"
        : "raygen";
}

static const char* Upt04FamilyName(PathTraceUnifiedPtFamily family)
{
    switch (family)
    {
    case PathTraceUnifiedPtFamily::Unified:
        return "unified";
    case PathTraceUnifiedPtFamily::IndirectOnly:
        return "indirect-only";
    default:
        return "direct-only";
    }
}

static uint32_t Upt04FamilyMask(PathTraceUnifiedPtFamily family)
{
    switch (family)
    {
    case PathTraceUnifiedPtFamily::Unified:
        return UPT04_FAMILY_LOCAL_LIGHT | UPT04_FAMILY_INDIRECT;
    case PathTraceUnifiedPtFamily::IndirectOnly:
        return UPT04_FAMILY_INDIRECT;
    default:
        return UPT04_FAMILY_LOCAL_LIGHT;
    }
}

static const char* Upt04InitialShaderPath(
    PathTraceUnifiedPtBackend backend,
    PathTraceUnifiedPtFamily family)
{
    if (backend == PathTraceUnifiedPtBackend::RayQuery)
    {
        switch (family)
        {
        case PathTraceUnifiedPtFamily::Unified:
            return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery.bin";
        case PathTraceUnifiedPtFamily::IndirectOnly:
            return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_indirect_only_rayquery.bin";
        default:
            return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_direct_only_rayquery.bin";
        }
    }

    switch (family)
    {
    case PathTraceUnifiedPtFamily::Unified:
        return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_raygen.bin";
    case PathTraceUnifiedPtFamily::IndirectOnly:
        return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_indirect_only_raygen.bin";
    default:
        return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_direct_only_raygen.bin";
    }
}

static const char* Upt04DiagnosticShaderPath()
{
    return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_diagnostics.bin";
}

static uint32_t Upt04PipelineVariant(const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (inputs.diagnostics)
    {
        return 0u;
    }
    return inputs.shaderProofMode >= 7u && inputs.shaderProofMode <= 13u
        ? inputs.shaderProofMode
        : 0u;
}

static bool Upt04UsesCompactProbeLayout(uint32_t pipelineVariant)
{
    return pipelineVariant == 7u || pipelineVariant == 8u;
}

static bool Upt04UsesMinimalProductionSlotLayout(uint32_t pipelineVariant)
{
    return pipelineVariant == 12u || pipelineVariant == 13u;
}

static bool Upt04UsesPushConstants(uint32_t pipelineVariant)
{
    return pipelineVariant != 7u && pipelineVariant != 8u &&
        pipelineVariant != 12u;
}

static bool Upt04UsesBindlessSet(uint32_t pipelineVariant)
{
    return pipelineVariant != 7u && pipelineVariant != 8u &&
        pipelineVariant != 11u && pipelineVariant != 12u &&
        pipelineVariant != 13u;
}

static bool Upt04UsesDirectOnlyProductionLayout(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    return Upt04PipelineVariant(inputs) == 0u &&
        !inputs.diagnostics &&
        inputs.family == PathTraceUnifiedPtFamily::DirectOnly;
}

static const char* Upt04LiveTlasProbePath(uint32_t pipelineVariant)
{
    switch (pipelineVariant)
    {
    case 7u:
        return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_live_tlas_probe_slang.bin";
    case 8u:
        return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_live_tlas_probe_dxc.bin";
    case 9u:
        return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_live_tlas_full_layout_probe.bin";
    default:
        return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_live_tlas_full_host_probe.bin";
    }
}

static const char* Upt04LiveTlasProbeDebugName(uint32_t pipelineVariant)
{
    switch (pipelineVariant)
    {
    case 7u: return "PathTraceUnifiedPtLiveTlasProbeSlang";
    case 8u: return "PathTraceUnifiedPtLiveTlasProbeDxc";
    case 9u: return "PathTraceUnifiedPtLiveTlasFullLayoutProbeSlang";
    case 10u: return "PathTraceUnifiedPtLiveTlasFullHostProbeSlang";
    case 11u: return "PathTraceUnifiedPtLiveTlasSet0OnlyProbeSlang";
    case 12u: return "PathTraceUnifiedPtLiveTlasB0B4ProbeSlang";
    default: return "PathTraceUnifiedPtLiveTlasB0B4PushProbeSlang";
    }
}

static const char* Upt04LiveTlasProbeMarkerName(uint32_t pipelineVariant)
{
    switch (pipelineVariant)
    {
    case 7u: return "UPT.D0 LiveTLAS Probe Slang 1x1";
    case 8u: return "UPT.D0 LiveTLAS Probe DXC 1x1";
    case 9u: return "UPT.D0 LiveTLAS FullLayout Probe Slang 1x1";
    case 10u: return "UPT.D0 LiveTLAS FullHost Probe Slang 1x1";
    case 11u: return "UPT.D0 LiveTLAS Set0Only Probe Slang 1x1";
    case 12u: return "UPT.D0 LiveTLAS B0B4 Probe Slang 1x1";
    default: return "UPT.D0 LiveTLAS B0B4Push Probe Slang 1x1";
    }
}

static uint64_t Upt04HashBytes(const void* data, size_t size)
{
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i)
    {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static uint64_t Upt04HashValue(uint64_t hash, uint64_t value)
{
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash;
}

static uint64_t Upt04BuildPageGeneration(
    const PathTraceUnifiedPtDispatchInputs& dispatch,
    uint32_t enabledFamilyMask,
    uint32_t specializationIdentity,
    uint32_t availabilityFlags)
{
    const RtPathTraceSceneInputs& inputs = *dispatch.sceneInputs;
    uint64_t hash = 1469598103934665603ull;
    hash = Upt04HashValue(hash, UPT04_ABI_VERSION);
    // These words define the proposal mixture and stored-PDF interpretation.
    // They must invalidate a future history page even when scene identity did
    // not change.
    hash = Upt04HashValue(hash, enabledFamilyMask);
    hash = Upt04HashValue(hash, specializationIdentity);
    hash = Upt04HashValue(hash, availabilityFlags);
    hash = Upt04HashValue(hash, UPT04_TRANSPORT_K_MAX);
    hash = Upt04HashValue(hash, UPT04_TRANSPORT_POLICY_ID);
    hash = Upt04HashValue(hash, UPT04_NEE_RIS_CANDIDATE_COUNT);
    hash = Upt04HashValue(hash, inputs.lights.restirLightManagerEmissiveRangeOffset);
    hash = Upt04HashValue(hash, inputs.lights.restirLightManagerEmissiveRangeCount);
    hash = Upt04HashValue(hash, inputs.lights.restirLightManagerDoomAnalyticRangeOffset);
    hash = Upt04HashValue(hash, inputs.lights.restirLightManagerDoomAnalyticSampleableCount);
    hash = Upt04HashValue(hash, inputs.signatures.geometryMembership);
    hash = Upt04HashValue(hash, inputs.signatures.materialTable);
    hash = Upt04HashValue(hash, inputs.signatures.lightMembership);
    hash = Upt04HashValue(hash, inputs.signatures.outputResolution);
    hash = Upt04HashValue(hash, inputs.signatures.cameraProjection);
    hash = Upt04HashValue(hash, inputs.signatures.cpuUploadGeneration);
    hash = Upt04HashValue(hash, inputs.signatures.reservoirScene);
    hash = Upt04HashValue(hash, inputs.materials.textureDescriptorGeneration);
    hash = Upt04HashValue(hash, inputs.lights.restirLightManagerStructuralSignature);
    hash = Upt04HashValue(hash, inputs.lights.restirLightManagerMappingSignature);
    hash = Upt04HashValue(hash, inputs.lights.restirLightManagerPayloadSignature);
    hash = Upt04HashValue(hash, inputs.lights.unifiedPtEmissiveLookupSignature);
    hash = Upt04HashValue(hash, inputs.geometry.staticBucketRouteGeneration);
    return hash;
}

struct Upt04InitialControl
{
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t surfaceCount;
    uint32_t frameSampleIndex;
    uint32_t enabledFamilyMask;
    uint32_t specializationIdentity;
    uint32_t emissiveRangeStart;
    uint32_t emissiveRangeCount;
    uint32_t analyticRangeStart;
    uint32_t analyticRangeCount;
    uint32_t availabilityFlags;
    uint32_t logicalTextureCount;
    uint32_t pageGenerationLo;
    uint32_t pageGenerationHi;
    uint32_t shaderProofMode;
    uint32_t materialCount;
    uint32_t currentLightCount;
    uint32_t staticVertexCount;
    uint32_t staticIndexCount;
    uint32_t staticTriangleCount;
    uint32_t dynamicVertexCount;
    uint32_t dynamicIndexCount;
    uint32_t dynamicTriangleCount;
    uint32_t rigidVertexCount;
    uint32_t rigidIndexCount;
    uint32_t rigidTriangleCount;
    uint32_t rigidInstanceCount;
    uint32_t skinnedRouteRecordCount;
    uint32_t skinnedRouteTriangleCount;
    uint32_t skinnedSourceIndexCount;
    uint32_t skinnedCurrentVertexCount;
    uint32_t emissiveReplayCount;
};
static_assert(sizeof(Upt04InitialControl) == UPT04_PUSH_CONSTANT_BYTES,
    "UPT-04 host push constants must match Slang reflection");

struct Upt05ResolveControl
{
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t surfaceCount;
    uint32_t view;
};
static_assert(sizeof(Upt05ResolveControl) == UPT05_PUSH_CONSTANT_BYTES,
    "UPT-05 host push constants must match Slang reflection");

class Upt04MarkerScope
{
public:
    Upt04MarkerScope(nvrhi::ICommandList* commandList, const char* name, bool enabled)
        : m_commandList(enabled ? commandList : nullptr)
    {
        if (m_commandList)
        {
            m_commandList->beginMarker(name);
        }
    }

    ~Upt04MarkerScope()
    {
        if (m_commandList)
        {
            m_commandList->endMarker();
        }
    }

private:
    nvrhi::ICommandList* m_commandList;
};

static bool Upt04ReadShader(
    const char* path,
    void*& data,
    int& size,
    ID_TIME_T& timestamp,
    uint64_t& hash)
{
    data = nullptr;
    size = fileSystem->ReadFile(path, &data, &timestamp);
    if (size <= 0 || !data)
    {
        common->Printf("PathTraceUnifiedPt: couldn't read Slang SPIR-V %s\n", path);
        return false;
    }
    hash = Upt04HashBytes(data, static_cast<size_t>(size));
    return true;
}

static nvrhi::BufferHandle Upt04SkinnedIndexBuffer(const RtPathTraceSceneInputs& inputs)
{
    return inputs.geometry.skinnedSourceIndexBuffer
        ? inputs.geometry.skinnedSourceIndexBuffer
        : inputs.geometry.dynamicIndexBuffer;
}

static nvrhi::BufferHandle Upt04SkinnedVertexBuffer(const RtPathTraceSceneInputs& inputs)
{
    return inputs.geometry.skinnedCurrentOutputVertexBuffer
        ? inputs.geometry.skinnedCurrentOutputVertexBuffer
        : inputs.geometry.dynamicVertexBuffer;
}

static bool Upt04InputsValid(const PathTraceUnifiedPtDispatchInputs& dispatch)
{
    if (!dispatch.device || !dispatch.commandList || !dispatch.sceneInputs ||
        !dispatch.sceneInputs->valid || dispatch.width == 0 || dispatch.height == 0 ||
        !dispatch.primarySurfaceBuffer)
    {
        return false;
    }

    const RtPathTraceSceneInputs& inputs = *dispatch.sceneInputs;
    const RtPathTraceSceneInputGeometry& geometry = inputs.geometry;
    const RtPathTraceSceneInputMaterials& materials = inputs.materials;
    const RtPathTraceSceneInputLights& lights = inputs.lights;
    const bool commonGeometryValid = geometry.tlas &&
        geometry.staticVertexBuffer && geometry.staticIndexBuffer &&
        geometry.staticTriangleMaterialIndexBuffer &&
        geometry.dynamicVertexBuffer && geometry.dynamicIndexBuffer &&
        geometry.dynamicTriangleMaterialIndexBuffer &&
        geometry.rigidRouteVertexBuffer && geometry.rigidRouteIndexBuffer &&
        geometry.rigidRouteInstanceBuffer && geometry.skinnedHitRouteRecordBuffer &&
        geometry.skinnedHitRouteTriangleBuffer && Upt04SkinnedIndexBuffer(inputs) &&
        Upt04SkinnedVertexBuffer(inputs) &&
        lights.restirLightManagerCurrentPayloadBuffer &&
        lights.emissiveTriangleBuffer;
    if (!commonGeometryValid)
    {
        return false;
    }
    if (Upt04UsesDirectOnlyProductionLayout(dispatch))
    {
        return true;
    }
    return geometry.staticTriangleClassBuffer &&
        geometry.dynamicTriangleClassBuffer && materials.materialTableBuffer &&
        materials.textureBindlessLayout && materials.textureDescriptorTable &&
        materials.textureSampler && lights.unifiedPtEmissiveLookupBuffer &&
        lights.unifiedPtEmissiveLookupExact;
}

static void Upt04AddBindingLayoutItems(
    nvrhi::BindingLayoutDesc& desc,
    bool diagnostics)
{
    desc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2));
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3));
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(4));
    desc.addItem(nvrhi::BindingLayoutItem::Sampler(5));
    for (uint32_t slot = 6; slot <= 21; ++slot)
    {
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(slot));
    }
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(22));
    if (diagnostics)
    {
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(23));
    }
    desc.addItem(nvrhi::BindingLayoutItem::PushConstants(0, UPT04_PUSH_CONSTANT_BYTES));
}

static void Upt04AddDirectBindingLayoutItems(nvrhi::BindingLayoutDesc& desc)
{
    desc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2));
    desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(4));
    for (const uint32_t slot : { 6u, 7u, 8u, 10u, 11u, 12u, 14u,
            15u, 16u, 17u, 18u, 19u, 20u, 21u })
    {
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(slot));
    }
    desc.addItem(nvrhi::BindingLayoutItem::PushConstants(
        0, UPT04_PUSH_CONSTANT_BYTES));
}

static nvrhi::BindingSetDesc Upt04BuildBindingSetDesc(
    const PathTraceUnifiedPtDispatchInputs& dispatch,
    nvrhi::BufferHandle page0,
    nvrhi::BufferHandle diagnosticCounters)
{
    const RtPathTraceSceneInputs& inputs = *dispatch.sceneInputs;
    const RtPathTraceSceneInputGeometry& geometry = inputs.geometry;
    const RtPathTraceSceneInputMaterials& materials = inputs.materials;
    const RtPathTraceSceneInputLights& lights = inputs.lights;

    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, geometry.tlas));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, dispatch.primarySurfaceBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, lights.restirLightManagerCurrentPayloadBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, materials.materialTableBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(4, page0));
    desc.addItem(nvrhi::BindingSetItem::Sampler(5, materials.textureSampler));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, lights.emissiveTriangleBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, geometry.staticVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(8, geometry.staticIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(9, geometry.staticTriangleClassBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(10, geometry.staticTriangleMaterialIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(11, geometry.dynamicVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(12, geometry.dynamicIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(13, geometry.dynamicTriangleClassBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(14, geometry.dynamicTriangleMaterialIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(15, geometry.rigidRouteVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(16, geometry.rigidRouteIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(17, geometry.rigidRouteInstanceBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(18, Upt04SkinnedVertexBuffer(inputs)));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(19, Upt04SkinnedIndexBuffer(inputs)));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(20, geometry.skinnedHitRouteRecordBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(21, geometry.skinnedHitRouteTriangleBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(22, lights.unifiedPtEmissiveLookupBuffer));
    if (dispatch.diagnostics)
    {
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
            23, diagnosticCounters));
    }
    desc.addItem(nvrhi::BindingSetItem::PushConstants(0, UPT04_PUSH_CONSTANT_BYTES));
    return desc;
}

static nvrhi::BindingSetDesc Upt04BuildDirectBindingSetDesc(
    const PathTraceUnifiedPtDispatchInputs& dispatch,
    nvrhi::BufferHandle page0)
{
    const RtPathTraceSceneInputs& inputs = *dispatch.sceneInputs;
    const RtPathTraceSceneInputGeometry& geometry = inputs.geometry;
    const RtPathTraceSceneInputLights& lights = inputs.lights;
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, geometry.tlas));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, dispatch.primarySurfaceBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, lights.restirLightManagerCurrentPayloadBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(4, page0));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, lights.emissiveTriangleBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, geometry.staticVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(8, geometry.staticIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(10, geometry.staticTriangleMaterialIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(11, geometry.dynamicVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(12, geometry.dynamicIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(14, geometry.dynamicTriangleMaterialIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(15, geometry.rigidRouteVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(16, geometry.rigidRouteIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(17, geometry.rigidRouteInstanceBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(18, Upt04SkinnedVertexBuffer(inputs)));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(19, Upt04SkinnedIndexBuffer(inputs)));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(20, geometry.skinnedHitRouteRecordBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(21, geometry.skinnedHitRouteTriangleBuffer));
    desc.addItem(nvrhi::BindingSetItem::PushConstants(0, UPT04_PUSH_CONSTANT_BYTES));
    return desc;
}

static void Upt04SetSrvStates(
    nvrhi::ICommandList* commandList,
    const PathTraceUnifiedPtDispatchInputs& dispatch)
{
    const RtPathTraceSceneInputs& inputs = *dispatch.sceneInputs;
    const RtPathTraceSceneInputGeometry& geometry = inputs.geometry;
    commandList->setAccelStructState(geometry.tlas, nvrhi::ResourceStates::AccelStructRead);
    commandList->setBufferState(dispatch.primarySurfaceBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.lights.restirLightManagerCurrentPayloadBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.materials.materialTableBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.lights.emissiveTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.staticVertexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.staticIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.staticTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.staticTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.dynamicVertexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.dynamicIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.dynamicTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.dynamicTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.rigidRouteVertexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.rigidRouteIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.rigidRouteInstanceBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(Upt04SkinnedVertexBuffer(inputs), nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(Upt04SkinnedIndexBuffer(inputs), nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.skinnedHitRouteRecordBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.skinnedHitRouteTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.lights.unifiedPtEmissiveLookupBuffer, nvrhi::ResourceStates::ShaderResource);
}

static void Upt04SetDirectSrvStates(
    nvrhi::ICommandList* commandList,
    const PathTraceUnifiedPtDispatchInputs& dispatch)
{
    const RtPathTraceSceneInputs& inputs = *dispatch.sceneInputs;
    const RtPathTraceSceneInputGeometry& geometry = inputs.geometry;
    commandList->setAccelStructState(geometry.tlas, nvrhi::ResourceStates::AccelStructRead);
    commandList->setBufferState(dispatch.primarySurfaceBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.lights.restirLightManagerCurrentPayloadBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.lights.emissiveTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.staticVertexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.staticIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.staticTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.dynamicVertexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.dynamicIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.dynamicTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.rigidRouteVertexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.rigidRouteIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.rigidRouteInstanceBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(Upt04SkinnedVertexBuffer(inputs), nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(Upt04SkinnedIndexBuffer(inputs), nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.skinnedHitRouteRecordBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(geometry.skinnedHitRouteTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
}

static Upt04InitialControl Upt04BuildControl(const PathTraceUnifiedPtDispatchInputs& dispatch)
{
    const RtPathTraceSceneInputs& inputs = *dispatch.sceneInputs;
    const RtPathTraceSceneInputGeometry& geometry = inputs.geometry;
    const RtPathTraceSceneInputMaterials& materials = inputs.materials;
    const RtPathTraceSceneInputLights& lights = inputs.lights;
    const uint32_t enabledFamilyMask = Upt04FamilyMask(dispatch.family);
    const uint32_t specializationIdentity =
        static_cast<uint32_t>(dispatch.family) |
        (static_cast<uint32_t>(dispatch.backend) << 8u);
    const uint32_t availabilityFlags =
        (geometry.staticBucketRoutePublicationValid
            ? UPT04_ROUTE_STATIC_BUCKETS
            : 0u) |
        (lights.unifiedPtEmissiveLookupExact
            ? UPT04_EMISSIVE_LOOKUP_EXACT
            : 0u);
    const uint64_t pageGeneration = Upt04BuildPageGeneration(
        dispatch,
        enabledFamilyMask,
        specializationIdentity,
        availabilityFlags);
    const uint64_t surfaceCount64 = uint64_t(dispatch.width) * uint64_t(dispatch.height);

    Upt04InitialControl control = {};
    control.renderWidth = dispatch.width;
    control.renderHeight = dispatch.height;
    control.surfaceCount = static_cast<uint32_t>(surfaceCount64);
    control.frameSampleIndex = dispatch.frameSampleIndex;
    control.enabledFamilyMask = enabledFamilyMask;
    control.specializationIdentity = specializationIdentity;
    control.emissiveRangeStart = lights.restirLightManagerEmissiveRangeOffset;
    control.emissiveRangeCount = lights.restirLightManagerEmissiveRangeCount;
    control.analyticRangeStart = lights.restirLightManagerDoomAnalyticRangeOffset;
    control.analyticRangeCount = lights.restirLightManagerDoomAnalyticSampleableCount;
    control.availabilityFlags = availabilityFlags;
    control.logicalTextureCount = static_cast<uint32_t>(Max(0, materials.logicalTextureDescriptorCount));
    control.pageGenerationLo = static_cast<uint32_t>(pageGeneration);
    control.pageGenerationHi = static_cast<uint32_t>(pageGeneration >> 32u);
    control.shaderProofMode = dispatch.shaderProofMode;
    control.materialCount = static_cast<uint32_t>(Max(0, materials.materialTableEntryCount));
    control.currentLightCount = static_cast<uint32_t>(Max(0, lights.restirLightManagerCurrentPayloadCount));
    control.staticVertexCount = static_cast<uint32_t>(Max(0, geometry.staticVertexCount));
    control.staticIndexCount = static_cast<uint32_t>(Max(0, geometry.staticIndexCount));
    control.staticTriangleCount = static_cast<uint32_t>(Max(0, geometry.staticTriangleCount));
    control.dynamicVertexCount = static_cast<uint32_t>(Max(0, geometry.dynamicVertexCount));
    control.dynamicIndexCount = static_cast<uint32_t>(Max(0, geometry.dynamicIndexCount));
    control.dynamicTriangleCount = static_cast<uint32_t>(Max(0, geometry.dynamicTriangleCount));
    control.rigidVertexCount = static_cast<uint32_t>(Max(0, geometry.rigidRouteVertexCount));
    control.rigidIndexCount = static_cast<uint32_t>(Max(0, geometry.rigidRouteIndexCount));
    control.rigidTriangleCount = static_cast<uint32_t>(Max(0, geometry.rigidRouteTriangleCount));
    control.rigidInstanceCount = static_cast<uint32_t>(Max(0, geometry.rigidRouteInstanceCount));
    control.skinnedRouteRecordCount = static_cast<uint32_t>(Max(0, geometry.skinnedHitRouteRecordCount));
    control.skinnedRouteTriangleCount = static_cast<uint32_t>(Max(0, geometry.skinnedHitRouteTriangleCount));
    control.skinnedSourceIndexCount = static_cast<uint32_t>(Max(0, geometry.skinnedSourceIndexCount));
    control.skinnedCurrentVertexCount = static_cast<uint32_t>(Max(0, geometry.skinnedGpuComputeVertexCount));
    control.emissiveReplayCount = static_cast<uint32_t>(Max(0, lights.emissiveTriangleCount));
    return control;
}

} // namespace

void PathTraceUnifiedPtState::ReleasePipeline()
{
    m_bindingSet = nullptr;
    m_bindingSetDesc = nvrhi::BindingSetDesc();
    m_bindingSetDescValid = false;
    m_shaderTable = nullptr;
    m_rayPipeline = nullptr;
    m_rayGenerationLibrary = nullptr;
    m_missLibrary = nullptr;
    m_closestHitLibrary = nullptr;
    m_computePipeline = nullptr;
    m_computeShader = nullptr;
    m_bindingLayout = nullptr;
    m_pipelineAttempted = false;
}

void PathTraceUnifiedPtState::Release()
{
    ReleasePipeline();
    ReleaseResolve();
    m_page0 = nullptr;
    m_pageWidth = 0;
    m_pageHeight = 0;
    m_pageBytes = 0;
    m_pageNeedsClear = false;
    m_diagnosticCounters = nullptr;
    m_diagnosticReadback = nullptr;
    m_diagnosticReadbackPending = false;
    m_diagnosticReadbackDelayFrames = 0;
    m_pipelineVariant = 0;
    m_selectionValid = false;
    m_diagnostics = false;
    m_resourceFailureLogged = false;
    m_reportedProofStage = UINT32_MAX;
}

void PathTraceUnifiedPtState::ReleaseResolve()
{
    m_resolveBindingSet = nullptr;
    m_resolveBindingSetDesc = nvrhi::BindingSetDesc();
    m_resolveBindingSetDescValid = false;
    m_resolvePipeline = nullptr;
    m_resolveShader = nullptr;
    m_resolveBindingLayout = nullptr;
    m_resolveOutput = nullptr;
    m_resolveWidth = 0;
    m_resolveHeight = 0;
    m_resolvePipelineAttempted = false;
    m_resolveFailureLogged = false;
    m_resolveReady = false;
}

void PathTraceUnifiedPtState::ReportProofStage(
    uint32_t stage,
    const char* label,
    PathTraceUnifiedPtBackend backend,
    PathTraceUnifiedPtFamily family)
{
    if (m_reportedProofStage == stage)
    {
        return;
    }
    common->Printf(
        "PathTraceUnifiedPt: proofStage=%u reached=%s backend=%s family=%s\n",
        stage,
        label,
        Upt04BackendName(backend),
        Upt04FamilyName(family));
    m_reportedProofStage = stage;
}

bool PathTraceUnifiedPtState::EnsurePage(const PathTraceUnifiedPtDispatchInputs& inputs)
{
    const uint64_t count = uint64_t(inputs.width) * uint64_t(inputs.height);
    if (count == 0 || count > std::numeric_limits<uint32_t>::max() ||
        count > std::numeric_limits<uint64_t>::max() / UPT04_RESERVOIR_STRIDE)
    {
        return false;
    }
    const uint64_t bytes = count * UPT04_RESERVOIR_STRIDE;
    if (m_page0 && m_pageWidth == inputs.width && m_pageHeight == inputs.height &&
        m_page0->getDesc().structStride == UPT04_RESERVOIR_STRIDE &&
        m_page0->getDesc().byteSize >= bytes)
    {
        return true;
    }

    nvrhi::BufferDesc desc;
    desc.debugName = "PathTraceUnifiedPtReservoirPage0";
    desc.byteSize = bytes;
    desc.structStride = UPT04_RESERVOIR_STRIDE;
    desc.canHaveUAVs = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    nvrhi::BufferHandle page = inputs.device->createBuffer(desc);
    if (!page)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to allocate page 0 (%ux%u, %llu bytes)\n",
            inputs.width,
            inputs.height,
            static_cast<unsigned long long>(bytes));
        return false;
    }

    m_page0 = page;
    m_pageWidth = inputs.width;
    m_pageHeight = inputs.height;
    m_pageBytes = bytes;
    m_pageNeedsClear = true;
    m_bindingSet = nullptr;
    m_bindingSetDescValid = false;
    m_resolveBindingSet = nullptr;
    m_resolveBindingSetDescValid = false;
    common->Printf(
        "PathTraceUnifiedPt: allocated page 0 %ux%u records=%llu bytes=%llu stride=%u\n",
        inputs.width,
        inputs.height,
        static_cast<unsigned long long>(count),
        static_cast<unsigned long long>(bytes),
        UPT04_RESERVOIR_STRIDE);
    return true;
}

bool PathTraceUnifiedPtState::EnsurePipeline(const PathTraceUnifiedPtDispatchInputs& inputs)
{
    const uint32_t pipelineVariant = Upt04PipelineVariant(inputs);
    const bool liveTlasProbe = pipelineVariant != 0u;
    const bool compactLiveTlasProbe = Upt04UsesCompactProbeLayout(pipelineVariant);
    const bool minimalProductionSlotLayout =
        Upt04UsesMinimalProductionSlotLayout(pipelineVariant);
    const bool usesPushConstants = Upt04UsesPushConstants(pipelineVariant);
    const bool directOnlyProduction =
        Upt04UsesDirectOnlyProductionLayout(inputs);
    const bool usesBindlessSet = Upt04UsesBindlessSet(pipelineVariant) &&
        !directOnlyProduction;
    if (!m_selectionValid || m_backend != inputs.backend || m_family != inputs.family ||
        m_pipelineVariant != pipelineVariant || m_diagnostics != inputs.diagnostics)
    {
        ReleasePipeline();
        m_backend = inputs.backend;
        m_family = inputs.family;
        m_pipelineVariant = pipelineVariant;
        m_diagnostics = inputs.diagnostics;
        m_selectionValid = true;
        m_resourceFailureLogged = false;
    }

    if (((liveTlasProbe || m_backend == PathTraceUnifiedPtBackend::RayQuery) && m_computePipeline) ||
        (m_backend == PathTraceUnifiedPtBackend::RayGeneration && m_shaderTable))
    {
        return true;
    }
    if (m_pipelineAttempted)
    {
        return false;
    }
    m_pipelineAttempted = true;

    if (inputs.diagnostics && inputs.backend != PathTraceUnifiedPtBackend::RayQuery)
    {
        common->Printf(
            "PathTraceUnifiedPt: diagnostics require the RayQuery backend; dispatch skipped\n");
        return false;
    }

    if ((liveTlasProbe || m_backend == PathTraceUnifiedPtBackend::RayQuery) &&
        !inputs.device->queryFeatureSupport(nvrhi::Feature::RayQuery))
    {
        common->Printf("PathTraceUnifiedPt: RayQuery backend requested but unsupported\n");
        return false;
    }

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = liveTlasProbe || m_backend == PathTraceUnifiedPtBackend::RayQuery
        ? nvrhi::ShaderType::Compute
        : nvrhi::ShaderType::AllRayTracing;
    // UPT Slang shaders declare explicit Vulkan descriptor sets. Keep NVRHI's
    // host-side set mapping explicit as well; this is the same contract used by
    // the standalone UPT-00 RayQuery harness.
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);
    // Keep NVRHI's default constant-buffer offset (256). Its old Vulkan
    // backend represents PushConstants as a zero-count descriptor-layout item
    // for binding-index bookkeeping. Flattening b0 to Vulkan binding 0 would
    // collide with the TLAS at t0 and make traversal consume an undefined AS
    // descriptor even though push constants are not shader descriptors.
    if (compactLiveTlasProbe)
    {
        layoutDesc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1));
    }
    else if (minimalProductionSlotLayout)
    {
        layoutDesc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(4));
        if (usesPushConstants)
        {
            layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
                0, UPT04_PUSH_CONSTANT_BYTES));
        }
    }
    else if (directOnlyProduction)
    {
        Upt04AddDirectBindingLayoutItems(layoutDesc);
    }
    else
    {
        Upt04AddBindingLayoutItems(layoutDesc, inputs.diagnostics);
    }
    m_bindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!m_bindingLayout)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create %s binding layout\n",
            compactLiveTlasProbe
                ? "compact live-TLAS probe"
                : (minimalProductionSlotLayout
                    ? "minimal production-slot probe"
                    : (directOnlyProduction
                        ? "direct-only production"
                        : "24-descriptor")));
        return false;
    }

    const char* initialPath = inputs.diagnostics
        ? Upt04DiagnosticShaderPath()
        : (liveTlasProbe
        ? Upt04LiveTlasProbePath(pipelineVariant)
        : Upt04InitialShaderPath(m_backend, m_family));
    void* initialData = nullptr;
    int initialSize = 0;
    ID_TIME_T initialTimestamp = 0;
    uint64_t initialHash = 0;
    if (!Upt04ReadShader(initialPath, initialData, initialSize, initialTimestamp, initialHash))
    {
        return false;
    }

    if (liveTlasProbe || m_backend == PathTraceUnifiedPtBackend::RayQuery)
    {
        nvrhi::ShaderDesc shaderDesc;
        shaderDesc.shaderType = nvrhi::ShaderType::Compute;
        shaderDesc.entryName = "main";
        shaderDesc.debugName = liveTlasProbe
            ? Upt04LiveTlasProbeDebugName(pipelineVariant)
            : (inputs.diagnostics
                ? "PathTraceUnifiedPtInitialDiagnostics"
                : "PathTraceUnifiedPtInitialRayQuery");
        m_computeShader = inputs.device->createShader(shaderDesc, initialData, initialSize);
        Mem_Free(initialData);
        if (!m_computeShader)
        {
            common->Printf("PathTraceUnifiedPt: failed to create RayQuery shader\n");
            return false;
        }

        nvrhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.CS = m_computeShader;
        pipelineDesc.bindingLayouts = { m_bindingLayout };
        if (usesBindlessSet)
        {
            pipelineDesc.bindingLayouts.push_back(
                inputs.sceneInputs->materials.textureBindlessLayout);
        }
        const uint64_t pipelineStartUs = Sys_Microseconds();
        m_computePipeline = inputs.device->createComputePipeline(pipelineDesc);
        const uint64_t pipelineUs = Sys_Microseconds() - pipelineStartUs;
        if (!m_computePipeline)
        {
            common->Printf("PathTraceUnifiedPt: failed to create RayQuery pipeline\n");
            return false;
        }
        common->Printf(
            "PathTraceUnifiedPt: pipeline backend=%s family=%s variant=%u compiler=%s blobBytes=%d hash=%016llx timestamp=%lld groups=%s bindlessSet=%d payload=0 createUs=%llu deferredHost=0 driverCache=opaque\n",
            Upt04BackendName(m_backend),
            Upt04FamilyName(m_family),
            pipelineVariant,
            pipelineVariant == 8u ? "dxc" : "slang",
            initialSize,
            static_cast<unsigned long long>(initialHash),
            static_cast<long long>(initialTimestamp),
            liveTlasProbe ? "1x1" : "8x8",
            usesBindlessSet ? 1 : 0,
            static_cast<unsigned long long>(pipelineUs));
        return true;
    }

    m_rayGenerationLibrary = inputs.device->createShaderLibrary(initialData, initialSize);
    Mem_Free(initialData);
    if (!m_rayGenerationLibrary)
    {
        common->Printf("PathTraceUnifiedPt: failed to create raygen shader library\n");
        return false;
    }

    const char* missPath =
        "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_miss.bin";
    const char* closestHitPath =
        "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_closest_hit.bin";
    void* missData = nullptr;
    int missSize = 0;
    ID_TIME_T missTimestamp = 0;
    uint64_t missHash = 0;
    if (!Upt04ReadShader(missPath, missData, missSize, missTimestamp, missHash))
    {
        return false;
    }
    m_missLibrary = inputs.device->createShaderLibrary(missData, missSize);
    Mem_Free(missData);
    if (!m_missLibrary)
    {
        common->Printf("PathTraceUnifiedPt: failed to create miss shader library\n");
        return false;
    }

    void* closestHitData = nullptr;
    int closestHitSize = 0;
    ID_TIME_T closestHitTimestamp = 0;
    uint64_t closestHitHash = 0;
    if (!Upt04ReadShader(
            closestHitPath,
            closestHitData,
            closestHitSize,
            closestHitTimestamp,
            closestHitHash))
    {
        return false;
    }
    m_closestHitLibrary = inputs.device->createShaderLibrary(closestHitData, closestHitSize);
    Mem_Free(closestHitData);
    if (!m_closestHitLibrary)
    {
        common->Printf("PathTraceUnifiedPt: failed to create closest-hit shader library\n");
        return false;
    }

    nvrhi::ShaderHandle rayGeneration =
        m_rayGenerationLibrary->getShader("main", nvrhi::ShaderType::RayGeneration);
    nvrhi::ShaderHandle miss =
        m_missLibrary->getShader("main", nvrhi::ShaderType::Miss);
    nvrhi::ShaderHandle closestHit =
        m_closestHitLibrary->getShader("main", nvrhi::ShaderType::ClosestHit);
    if (!rayGeneration || !miss || !closestHit)
    {
        common->Printf("PathTraceUnifiedPt: Slang raygen libraries are missing main entry points\n");
        return false;
    }

    nvrhi::rt::PipelineDesc pipelineDesc;
    pipelineDesc.globalBindingLayouts = { m_bindingLayout };
    if (usesBindlessSet)
    {
        pipelineDesc.globalBindingLayouts.push_back(
            inputs.sceneInputs->materials.textureBindlessLayout);
    }
    pipelineDesc.shaders = {
        { "Upt04RayGen", rayGeneration, nullptr },
        { "Upt04Miss", miss, nullptr }
    };
    // The live TLAS contributes SBT record 0 for legacy/static/rigid geometry
    // and record 2 for skinned geometry. Record 1 is retained only so record 2
    // is addressable; all three use the same trace-only closest-hit decoder.
    pipelineDesc.hitGroups = {
        { "Upt04HitGroupLegacy", closestHit, nullptr, nullptr, nullptr, false },
        { "Upt04HitGroupUnusedShadow", closestHit, nullptr, nullptr, nullptr, false },
        { "Upt04HitGroupSkinned", closestHit, nullptr, nullptr, nullptr, false }
    };
    pipelineDesc.maxPayloadSize = 32;
    pipelineDesc.maxAttributeSize = 8;
    pipelineDesc.maxRecursionDepth = 1;
    pipelineDesc.useDeferredHostOperations = false;
    const uint64_t pipelineStartUs = Sys_Microseconds();
    m_rayPipeline = inputs.device->createRayTracingPipeline(pipelineDesc);
    const uint64_t pipelineUs = Sys_Microseconds() - pipelineStartUs;
    if (!m_rayPipeline)
    {
        common->Printf("PathTraceUnifiedPt: failed to create raygen pipeline\n");
        return false;
    }
    m_shaderTable = m_rayPipeline->createShaderTable();
    if (!m_shaderTable)
    {
        common->Printf("PathTraceUnifiedPt: failed to create raygen shader table\n");
        m_rayPipeline = nullptr;
        return false;
    }
    m_shaderTable->setRayGenerationShader("Upt04RayGen");
    m_shaderTable->addMissShader("Upt04Miss");
    m_shaderTable->addHitGroup("Upt04HitGroupLegacy");
    m_shaderTable->addHitGroup("Upt04HitGroupUnusedShadow");
    m_shaderTable->addHitGroup("Upt04HitGroupSkinned");

    common->Printf(
        "PathTraceUnifiedPt: pipeline backend=%s family=%s raygenBytes=%d raygenHash=%016llx missBytes=%d missHash=%016llx hitBytes=%d hitHash=%016llx sbtHitGroups=3 payload=32 attribute=8 recursion=1 createUs=%llu deferredHost=0 driverCache=opaque\n",
        Upt04BackendName(m_backend),
        Upt04FamilyName(m_family),
        initialSize,
        static_cast<unsigned long long>(initialHash),
        missSize,
        static_cast<unsigned long long>(missHash),
        closestHitSize,
        static_cast<unsigned long long>(closestHitHash),
        static_cast<unsigned long long>(pipelineUs));
    return true;
}

bool PathTraceUnifiedPtState::EnsureBindingSet(const PathTraceUnifiedPtDispatchInputs& inputs)
{
    const bool compactLiveTlasProbe =
        Upt04UsesCompactProbeLayout(Upt04PipelineVariant(inputs));
    const bool minimalProductionSlotLayout =
        Upt04UsesMinimalProductionSlotLayout(Upt04PipelineVariant(inputs));
    const bool usesPushConstants =
        Upt04UsesPushConstants(Upt04PipelineVariant(inputs));
    const bool directOnlyProduction =
        Upt04UsesDirectOnlyProductionLayout(inputs);
    nvrhi::BindingSetDesc desc;
    if (compactLiveTlasProbe)
    {
        desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(
            0, inputs.sceneInputs->geometry.tlas));
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, m_page0));
    }
    else if (minimalProductionSlotLayout)
    {
        desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(
            0, inputs.sceneInputs->geometry.tlas));
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(4, m_page0));
        if (usesPushConstants)
        {
            desc.addItem(nvrhi::BindingSetItem::PushConstants(
                0, UPT04_PUSH_CONSTANT_BYTES));
        }
    }
    else if (directOnlyProduction)
    {
        desc = Upt04BuildDirectBindingSetDesc(inputs, m_page0);
    }
    else
    {
        desc = Upt04BuildBindingSetDesc(
            inputs, m_page0, m_diagnosticCounters);
    }
    if (m_bindingSet && m_bindingSetDescValid && m_bindingSetDesc == desc)
    {
        return true;
    }
    m_bindingSet = inputs.device->createBindingSet(desc, m_bindingLayout);
    if (!m_bindingSet)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create %s set-0 binding set\n",
            compactLiveTlasProbe
                ? "compact live-TLAS probe"
                : (minimalProductionSlotLayout
                    ? "minimal production-slot probe"
                    : (directOnlyProduction
                        ? "direct-only production"
                        : "UPT-04")));
        return false;
    }
    m_bindingSetDesc = desc;
    m_bindingSetDescValid = true;
    return true;
}

bool PathTraceUnifiedPtState::EnsureDiagnosticBuffers(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.diagnostics)
    {
        return true;
    }
    if (!m_diagnosticCounters)
    {
        nvrhi::BufferDesc desc;
        desc.debugName = "PathTraceUnifiedPtDiagnosticCounters";
        desc.byteSize = UPT04_DIAGNOSTIC_BYTES;
        desc.structStride = sizeof(uint32_t);
        desc.canHaveUAVs = true;
        desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        desc.keepInitialState = true;
        m_diagnosticCounters = inputs.device->createBuffer(desc);
    }
    if (!m_diagnosticReadback)
    {
        nvrhi::BufferDesc desc;
        desc.debugName = "PathTraceUnifiedPtDiagnosticReadback";
        desc.byteSize = UPT04_DIAGNOSTIC_BYTES;
        desc.cpuAccess = nvrhi::CpuAccessMode::Read;
        desc.initialState = nvrhi::ResourceStates::CopyDest;
        desc.keepInitialState = true;
        m_diagnosticReadback = inputs.device->createBuffer(desc);
    }
    if (!m_diagnosticCounters || !m_diagnosticReadback)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to allocate diagnostic counter/readback buffers\n");
        return false;
    }
    return true;
}

void PathTraceUnifiedPtState::DrainDiagnosticReadback(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!m_diagnosticReadbackPending || !m_diagnosticReadback || !inputs.device)
    {
        return;
    }
    if (m_diagnosticReadbackDelayFrames > 0)
    {
        --m_diagnosticReadbackDelayFrames;
        return;
    }
    const uint32_t* counters = static_cast<const uint32_t*>(
        inputs.device->mapBuffer(
            m_diagnosticReadback, nvrhi::CpuAccessMode::Read));
    if (!counters)
    {
        common->Printf("PathTraceUnifiedPt: diagnostic readback map failed\n");
        m_diagnosticReadbackPending = false;
        return;
    }
    common->Printf(
        "PathTraceUnifiedPt: diagnostic receivers(valid/invalid)=%u/%u candidates(direct invalid/zero/positive)=%u/%u/%u candidates(indirect invalid/zero/positive)=%u/%u/%u selected(primaryNee/bsdfEndpoint/secondaryNee)=%u/%u/%u rays(continuation/visibility)=%u/%u canonicalEmpty=%u candidateInputs=%u rayCeilingViolations=%u\n",
        counters[0], counters[1], counters[2], counters[3], counters[4],
        counters[5], counters[6], counters[7], counters[8], counters[9],
        counters[10], counters[11], counters[12], counters[13], counters[14],
        counters[15]);
    inputs.device->unmapBuffer(m_diagnosticReadback);
    m_diagnosticReadbackPending = false;
}

bool PathTraceUnifiedPtState::ExecuteInitial(const PathTraceUnifiedPtDispatchInputs& inputs)
{
    // Presentation is admitted per frame only after the matching UPT-05
    // resolve completes; never expose a previous frame after an early return.
    m_resolveReady = false;
    DrainDiagnosticReadback(inputs);
    if (!Upt04InputsValid(inputs))
    {
        if (!m_resourceFailureLogged)
        {
            common->Printf(
                "PathTraceUnifiedPt: UPT-04 live input closure is incomplete; initial dispatch skipped\n");
            m_resourceFailureLogged = true;
        }
        return false;
    }
    if (inputs.proofStage <= 1u)
    {
        ReportProofStage(1u, "input-closure", inputs.backend, inputs.family);
        return true;
    }
    if (!EnsurePage(inputs))
    {
        return false;
    }
    if (!EnsureDiagnosticBuffers(inputs))
    {
        return false;
    }
    if (inputs.proofStage == 2u)
    {
        ReportProofStage(2u, "page-allocation", inputs.backend, inputs.family);
        return true;
    }
    if (!EnsurePipeline(inputs))
    {
        return false;
    }
    if (inputs.proofStage == 3u)
    {
        ReportProofStage(3u, "pipeline-creation", inputs.backend, inputs.family);
        return true;
    }
    if (!EnsureBindingSet(inputs))
    {
        return false;
    }
    if (inputs.proofStage == 4u)
    {
        ReportProofStage(4u, "descriptor-creation", inputs.backend, inputs.family);
        return true;
    }
    m_resourceFailureLogged = false;

    const bool liveTlasProbe = Upt04PipelineVariant(inputs) != 0u;
    const bool compactLiveTlasProbe =
        Upt04UsesCompactProbeLayout(Upt04PipelineVariant(inputs));
    const bool minimalProductionSlotLayout =
        Upt04UsesMinimalProductionSlotLayout(Upt04PipelineVariant(inputs));
    const bool usesPushConstants =
        Upt04UsesPushConstants(Upt04PipelineVariant(inputs));
    const bool usesBindlessSet =
        Upt04UsesBindlessSet(Upt04PipelineVariant(inputs)) &&
        !Upt04UsesDirectOnlyProductionLayout(inputs);
    const bool directOnlyProduction =
        Upt04UsesDirectOnlyProductionLayout(inputs);
    const Upt04InitialControl control = !usesPushConstants
        ? Upt04InitialControl{}
        : Upt04BuildControl(inputs);
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.D0 Initial Bind+Barriers",
            inputs.nsightMarkers);
        if (compactLiveTlasProbe || minimalProductionSlotLayout)
        {
            inputs.commandList->setAccelStructState(
                inputs.sceneInputs->geometry.tlas,
                nvrhi::ResourceStates::AccelStructRead);
        }
        else if (directOnlyProduction)
        {
            Upt04SetDirectSrvStates(inputs.commandList, inputs);
        }
        else
        {
            Upt04SetSrvStates(inputs.commandList, inputs);
        }
        inputs.commandList->setBufferState(m_page0, nvrhi::ResourceStates::UnorderedAccess);
        if (inputs.diagnostics)
        {
            inputs.commandList->setBufferState(
                m_diagnosticCounters, nvrhi::ResourceStates::UnorderedAccess);
        }
        inputs.commandList->commitBarriers();
        if (m_pageNeedsClear)
        {
            inputs.commandList->clearBufferUInt(m_page0, 0u);
            nvrhi::utils::BufferUavBarrier(inputs.commandList, m_page0);
            m_pageNeedsClear = false;
        }
        if (inputs.diagnostics)
        {
            inputs.commandList->clearBufferUInt(m_diagnosticCounters, 0u);
            nvrhi::utils::BufferUavBarrier(
                inputs.commandList, m_diagnosticCounters);
        }

        if (inputs.proofStage == 5u)
        {
            ReportProofStage(5u, "barriers-and-clear", inputs.backend, inputs.family);
            return true;
        }

        if (liveTlasProbe || m_backend == PathTraceUnifiedPtBackend::RayQuery)
        {
            nvrhi::ComputeState state;
            state.pipeline = m_computePipeline;
            state.bindings = { m_bindingSet };
            if (usesBindlessSet)
            {
                state.bindings.push_back(
                    inputs.sceneInputs->materials.textureDescriptorTable);
            }
            inputs.commandList->setComputeState(state);
        }
        else
        {
            nvrhi::rt::State state;
            state.shaderTable = m_shaderTable;
            state.bindings = { m_bindingSet };
            if (usesBindlessSet)
            {
                state.bindings.push_back(
                    inputs.sceneInputs->materials.textureDescriptorTable);
            }
            inputs.commandList->setRayTracingState(state);
        }
        if (usesPushConstants)
        {
            inputs.commandList->setPushConstants(&control, sizeof(control));
        }

        if (inputs.proofStage == 6u)
        {
            ReportProofStage(6u, "state-and-push-binding", inputs.backend, inputs.family);
            return true;
        }
    }

    {
        const bool oneGroup = inputs.proofStage == 7u;
        const bool oneGroupRow = inputs.proofStage == 8u;
        const char* oneGroupRayQueryMarker = inputs.shaderProofMode == 1u
            ? "UPT.D0 Initial RayQuery 8x8 UavWrite"
            : (inputs.shaderProofMode == 2u
                ? "UPT.D0 Initial RayQuery 8x8 PrimaryRead"
                : (inputs.shaderProofMode == 3u
                    ? "UPT.D0 Initial RayQuery 8x8 ProposalNoTrace"
                    : (inputs.shaderProofMode == 4u
                        ? "UPT.D0 Initial RayQuery 8x8 FixedRayStatus"
                        : (inputs.shaderProofMode == 5u
                            ? "UPT.D0 Initial RayQuery 8x8 ProposedRayStatus"
                            : "UPT.D0 Initial RayQuery 8x8 FullHitMetadata"))));
        const char* markerName = liveTlasProbe
            ? Upt04LiveTlasProbeMarkerName(m_pipelineVariant)
            : (m_backend == PathTraceUnifiedPtBackend::RayQuery
            ? (oneGroup
                ? oneGroupRayQueryMarker
                : (oneGroupRow
                    ? "UPT.D0 Initial RayQuery Dispatch OneGroupRow"
                    : "UPT.D0 Initial RayQuery Dispatch FullFrame"))
            : (oneGroup
                ? "UPT.D0 Initial RayGen DispatchRays 8x8"
                : (oneGroupRow
                    ? "UPT.D0 Initial RayGen DispatchRays OneGroupRow"
                    : "UPT.D0 Initial RayGen DispatchRays FullFrame")));
        Upt04MarkerScope marker(inputs.commandList, markerName, inputs.nsightMarkers);
        if (liveTlasProbe)
        {
            inputs.commandList->dispatch(1u, 1u, 1u);
        }
        else if (m_backend == PathTraceUnifiedPtBackend::RayQuery)
        {
            const uint32_t groupCountX = oneGroup
                ? 1u
                : (inputs.width + 7u) / 8u;
            const uint32_t groupCountY = oneGroup || oneGroupRow
                ? 1u
                : (inputs.height + 7u) / 8u;
            inputs.commandList->dispatch(
                groupCountX,
                groupCountY,
                1u);
        }
        else
        {
            nvrhi::rt::DispatchRaysArguments args;
            args.width = oneGroup ? Min(inputs.width, 8u) : inputs.width;
            args.height = oneGroup || oneGroupRow ? Min(inputs.height, 8u) : inputs.height;
            args.depth = 1u;
            inputs.commandList->dispatchRays(args);
        }
    }

    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.D0 Initial OutputBarrier",
            inputs.nsightMarkers);
        nvrhi::utils::BufferUavBarrier(inputs.commandList, m_page0);
        if (inputs.diagnostics)
        {
            nvrhi::utils::BufferUavBarrier(
                inputs.commandList, m_diagnosticCounters);
            inputs.commandList->setBufferState(
                m_diagnosticCounters, nvrhi::ResourceStates::CopySource);
            inputs.commandList->setBufferState(
                m_diagnosticReadback, nvrhi::ResourceStates::CopyDest);
            inputs.commandList->commitBarriers();
            inputs.commandList->copyBuffer(
                m_diagnosticReadback,
                0,
                m_diagnosticCounters,
                0,
                UPT04_DIAGNOSTIC_BYTES);
            m_diagnosticReadbackPending = true;
            m_diagnosticReadbackDelayFrames = 2;
        }
    }
    if (inputs.proofStage == 7u)
    {
        ReportProofStage(
            7u,
            liveTlasProbe ? "one-invocation-live-tlas-probe" : "one-8x8-group",
            inputs.backend,
            inputs.family);
    }
    else if (inputs.proofStage == 8u)
    {
        ReportProofStage(8u, "one-full-width-group-row", inputs.backend, inputs.family);
    }
    else
    {
        ReportProofStage(9u, "full-frame-dispatch", inputs.backend, inputs.family);
    }
    return true;
}

bool PathTraceUnifiedPtState::EnsureResolveResources(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (m_resolveOutput && m_resolveWidth == inputs.width &&
        m_resolveHeight == inputs.height &&
        m_resolveOutput->getDesc().format == nvrhi::Format::RGBA16_FLOAT)
    {
        return true;
    }

    nvrhi::TextureDesc desc;
    desc.width = inputs.width;
    desc.height = inputs.height;
    desc.mipLevels = 1;
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.isUAV = true;
    desc.debugName = "PathTraceUnifiedPtResolveOutput";
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    nvrhi::TextureHandle output = inputs.device->createTexture(desc);
    if (!output)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to allocate UPT-05 RGBA16F output %ux%u\n",
            inputs.width,
            inputs.height);
        return false;
    }

    m_resolveOutput = output;
    m_resolveWidth = inputs.width;
    m_resolveHeight = inputs.height;
    m_resolveBindingSet = nullptr;
    m_resolveBindingSetDescValid = false;
    common->Printf(
        "PathTraceUnifiedPt: allocated UPT-05 output %ux%u format=RGBA16_FLOAT bytes=%llu\n",
        inputs.width,
        inputs.height,
        static_cast<unsigned long long>(
            uint64_t(inputs.width) * uint64_t(inputs.height) * 8ull));
    return true;
}

bool PathTraceUnifiedPtState::EnsureResolvePipeline(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (m_resolvePipeline)
    {
        return true;
    }
    if (m_resolvePipelineAttempted)
    {
        return false;
    }
    m_resolvePipelineAttempted = true;

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(1));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
        0, UPT05_PUSH_CONSTANT_BYTES));
    m_resolveBindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!m_resolveBindingLayout)
    {
        common->Printf("PathTraceUnifiedPt: failed to create UPT-05 resolve binding layout\n");
        return false;
    }

    const char* path =
        "renderprogs2/spirv/builtin/pathtracing/slang_upt05/upt05_resolve.bin";
    void* data = nullptr;
    int size = 0;
    ID_TIME_T timestamp = 0;
    uint64_t hash = 0;
    if (!Upt04ReadShader(path, data, size, timestamp, hash))
    {
        return false;
    }
    nvrhi::ShaderDesc shaderDesc;
    shaderDesc.shaderType = nvrhi::ShaderType::Compute;
    shaderDesc.entryName = "main";
    shaderDesc.debugName = "PathTraceUnifiedPtResolve";
    m_resolveShader = inputs.device->createShader(shaderDesc, data, size);
    Mem_Free(data);
    if (!m_resolveShader)
    {
        common->Printf("PathTraceUnifiedPt: failed to create UPT-05 resolve shader\n");
        return false;
    }

    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = m_resolveShader;
    pipelineDesc.bindingLayouts = { m_resolveBindingLayout };
    const uint64_t pipelineStartUs = Sys_Microseconds();
    m_resolvePipeline = inputs.device->createComputePipeline(pipelineDesc);
    const uint64_t pipelineUs = Sys_Microseconds() - pipelineStartUs;
    if (!m_resolvePipeline)
    {
        common->Printf("PathTraceUnifiedPt: failed to create UPT-05 resolve pipeline\n");
        return false;
    }
    common->Printf(
        "PathTraceUnifiedPt: resolve compiler=slang blobBytes=%d hash=%016llx timestamp=%lld groups=8x8 output=RGBA16_FLOAT rays=0 samples=0 createUs=%llu\n",
        size,
        static_cast<unsigned long long>(hash),
        static_cast<long long>(timestamp),
        static_cast<unsigned long long>(pipelineUs));
    return true;
}

bool PathTraceUnifiedPtState::EnsureResolveBindingSet(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_page0));
    desc.addItem(nvrhi::BindingSetItem::Texture_UAV(1, m_resolveOutput));
    desc.addItem(nvrhi::BindingSetItem::PushConstants(
        0, UPT05_PUSH_CONSTANT_BYTES));
    if (m_resolveBindingSet && m_resolveBindingSetDescValid &&
        m_resolveBindingSetDesc == desc)
    {
        return true;
    }
    m_resolveBindingSet = inputs.device->createBindingSet(
        desc, m_resolveBindingLayout);
    if (!m_resolveBindingSet)
    {
        common->Printf("PathTraceUnifiedPt: failed to create UPT-05 resolve binding set\n");
        return false;
    }
    m_resolveBindingSetDesc = desc;
    m_resolveBindingSetDescValid = true;
    return true;
}

bool PathTraceUnifiedPtState::ExecuteResolve(
    const PathTraceUnifiedPtDispatchInputs& inputs,
    uint32_t view)
{
    m_resolveReady = false;
    const bool productionFullFrame = inputs.proofStage >= 9u &&
        Upt04PipelineVariant(inputs) == 0u;
    if (!productionFullFrame || !m_page0 || m_pageWidth != inputs.width ||
        m_pageHeight != inputs.height)
    {
        if (!m_resolveFailureLogged)
        {
            common->Printf(
                "PathTraceUnifiedPt: UPT-05 resolve requires production shaderProof 1..6 and proofStage 9; skipped\n");
            m_resolveFailureLogged = true;
        }
        return false;
    }
    if (!EnsureResolveResources(inputs) || !EnsureResolvePipeline(inputs) ||
        !EnsureResolveBindingSet(inputs))
    {
        return false;
    }
    m_resolveFailureLogged = false;

    const uint64_t surfaceCount64 = uint64_t(inputs.width) * uint64_t(inputs.height);
    if (surfaceCount64 > std::numeric_limits<uint32_t>::max())
    {
        return false;
    }
    const Upt05ResolveControl control = {
        inputs.width,
        inputs.height,
        static_cast<uint32_t>(surfaceCount64),
        view
    };
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.R0 Resolve Bind+Barriers",
            inputs.nsightMarkers);
        inputs.commandList->setBufferState(
            m_page0, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setTextureState(
            m_resolveOutput,
            nvrhi::AllSubresources,
            nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->commitBarriers();

        nvrhi::ComputeState state;
        state.pipeline = m_resolvePipeline;
        state.bindings = { m_resolveBindingSet };
        inputs.commandList->setComputeState(state);
        inputs.commandList->setPushConstants(&control, sizeof(control));
    }
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.R0 Resolve Dispatch",
            inputs.nsightMarkers);
        inputs.commandList->dispatch(
            (inputs.width + 7u) / 8u,
            (inputs.height + 7u) / 8u,
            1u);
    }
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.R0 Resolve OutputBarrier",
            inputs.nsightMarkers);
        nvrhi::utils::TextureUavBarrier(inputs.commandList, m_resolveOutput);
        inputs.commandList->setTextureState(
            m_resolveOutput,
            nvrhi::AllSubresources,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->commitBarriers();
    }
    m_resolveReady = true;
    return true;
}
