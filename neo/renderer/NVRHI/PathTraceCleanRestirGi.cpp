#include "precompiled.h"
#pragma hdrstop

// Clean-room Remix ReSTIR GI lane dispatch (docs/restir_remix_gi_cleanroom).
//
// RGI-01: isolated route with its own cvars, reservoir pages, producer
// textures, and a sentinel debug view.
// RGI-02: producer dispatch with the shared scene/light bindings so the GI
// raygen can trace the bounce ray and shade the secondary vertex.
// The lane never reads DI reservoirs and never writes anything when
// r_pathTracingCleanRestirGiEnable is 0.

#include "PathTraceCleanRestirGi.h"
#include "PathTraceCVars.h"
#include "../RenderCommon.h"
#include "../../sys/DeviceManager.h"

#include <cstring>

#include <nvrhi/utils.h>

#include <Rtxdi/RtxdiUtils.h>
#include <Rtxdi/GI/ReSTIRGIParameters.h>

extern DeviceManager* deviceManager;

namespace {

// Page roles inside the single GI reservoir buffer (RAB_GIReservoirBridge
// page info: x=init, y=temporalInput, z=temporalOutput, w=spatialOutput).
const uint32_t CLEAN_RESTIR_GI_PAGE_INIT = 0u;
const uint32_t CLEAN_RESTIR_GI_PAGE_TEMPORAL_INPUT = 1u;
const uint32_t CLEAN_RESTIR_GI_PAGE_TEMPORAL_OUTPUT = 2u;
const uint32_t CLEAN_RESTIR_GI_PAGE_SPATIAL_OUTPUT = 3u;
const uint32_t CLEAN_RESTIR_GI_PAGE_COUNT = 4u;

// Must match the DI sentinel constants blob size mirrored at the head of the
// GI cbuffer (PathTraceCleanRtxdiDiSentinelConstants).
const uint32_t CLEAN_RESTIR_GI_DI_BLOB_SIZE = 480u;
const uint32_t CLEAN_RESTIR_GI_DI_ANALYTIC_LIGHT_COUNT_OFFSET = 4u * sizeof(uint32_t);

// GI-owned cbuffer tail; layout must match the trailing fields of
// PathTraceCleanRestirGiConstants in pathtrace_clean_restir_gi.rt.hlsl.
struct PathTraceCleanRestirGiConstantsTail
{
    uint32_t view;
    uint32_t temporalEnabled;
    uint32_t spatialEnabled;
    uint32_t biasCorrection;
    uint32_t jacobianEnabled;
    uint32_t maxHistoryLength;
    uint32_t maxReservoirAge;
    float fireflyThreshold;
    uint32_t neeCacheSeedEnabled;
    uint32_t frameIndex;
    uint32_t phase;
    uint32_t resolveEnabled;
    uint32_t specularProducerEnabled;
    uint32_t rrHitDistanceEnabled;
    uint32_t rrSpecularInputEnabled;
    uint32_t neeCacheSecondaryEnabled;
    uint32_t neeCacheSecondaryMode;
    float neeCacheSecondaryRoughness;
    float neeCacheSecondaryProbability;
    uint32_t maxBounces;
    uint32_t continuationRouletteEnabled;
    float continuationRouletteMin;
    float continuationRouletteMax;
    float continuationDirectProbability;
    float secondaryDirectProbability;
    uint32_t continuationOpaqueTrace;
    uint32_t producerOpaqueTrace;
    uint32_t secondaryDirectSamples;
    uint32_t secondaryRluCandidateCount;
    float contributionFireflyThreshold;
    uint32_t blueNoiseEnabled;
    uint32_t producerRayQueryHitIdMode;
    uint32_t spatialVisibilityMode;
    uint32_t glossySecondRayEnabled;
    float glossySecondRayMaxRoughness;
    uint32_t finalMixMode;
    RTXDI_ReservoirBufferParameters reservoirParams;
    uint32_t pageInfo[4];
};
static_assert(sizeof(PathTraceCleanRestirGiConstantsTail) == 176, "GI constants tail must match the HLSL cbuffer tail layout");

const uint32_t CLEAN_RESTIR_GI_CONSTANTS_SIZE = CLEAN_RESTIR_GI_DI_BLOB_SIZE + sizeof(PathTraceCleanRestirGiConstantsTail);

bool CleanRestirGiEnsurePipeline(PathTraceCleanRestirGiState& state, const PathTraceCleanRestirGiDispatchInputs& inputs)
{
    if (state.shaderTable)
    {
        return true;
    }
    if (state.pipelineInitAttempted)
    {
        return false;
    }
    state.pipelineInitAttempted = true;

    const char* shaderPath = nullptr;
    if (inputs.isD3D12)
    {
        shaderPath = "renderprogs2/dxil/builtin/pathtracing/remix_restir_gi/pathtrace_clean_restir_gi.rt.bin";
    }
    else if (inputs.isVulkan)
    {
        shaderPath = "renderprogs2/spirv/builtin/pathtracing/remix_restir_gi/pathtrace_clean_restir_gi.rt.bin";
    }
    else
    {
        common->Printf("PathTraceCleanRestirGi: unsupported graphics API\n");
        return false;
    }

    void* shaderData = nullptr;
    ID_TIME_T shaderTimestamp = 0;
    const int shaderSize = fileSystem->ReadFile(shaderPath, &shaderData, &shaderTimestamp);
    if (shaderSize <= 0 || !shaderData)
    {
        common->Printf("PathTraceCleanRestirGi: couldn't read GI shader %s\n", shaderPath);
        return false;
    }
    state.shaderLibrary = inputs.device->createShaderLibrary(shaderData, shaderSize);
    Mem_Free(shaderData);
    if (!state.shaderLibrary)
    {
        common->Printf("PathTraceCleanRestirGi: failed to create GI shader library\n");
        return false;
    }

    if (!state.bindingLayout)
    {
        nvrhi::BindingLayoutDesc layoutDesc;
        layoutDesc.visibility = nvrhi::ShaderType::AllRayTracing;
        layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
            .setShaderResourceOffset(0)
            .setConstantBufferOffset(0)
            .setUnorderedAccessViewOffset(0);
        layoutDesc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(1));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(2));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(8));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(9));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(10));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(11));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(12));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(13));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(14));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(16));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(22));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(23));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(24));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(25));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(26));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(27));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(46));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(66));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(74));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(75));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(76));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(77));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(30));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(31));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(39));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(40));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(80));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(92)); // producer trace/shade G-buffer
        layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(81));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(82));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(83));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(84));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(85));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(86));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(48));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(51));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(54));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(127)); // blue-noise mask array (RBPT_ENABLE_BLUE_NOISE)
        layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
        state.bindingLayout = inputs.device->createBindingLayout(layoutDesc);
        if (!state.bindingLayout)
        {
            common->Printf("PathTraceCleanRestirGi: failed to create GI binding layout\n");
            return false;
        }
    }

    nvrhi::ShaderHandle producerTraceRayGen = state.shaderLibrary->getShader("FirstIndirectTraceRayGen", nvrhi::ShaderType::RayGeneration);
    nvrhi::ShaderHandle producerSimpleRayGen = state.shaderLibrary->getShader("FirstIndirectSimpleRayGen", nvrhi::ShaderType::RayGeneration);
    nvrhi::ShaderHandle producerLeanTraceRayGen = state.shaderLibrary->getShader("FirstIndirectLeanTraceRayGen", nvrhi::ShaderType::RayGeneration);
    nvrhi::ShaderHandle producerLeanShadeRayGen = state.shaderLibrary->getShader("FirstIndirectLeanShadeRayGen", nvrhi::ShaderType::RayGeneration);
    nvrhi::ShaderHandle producerRoughFallbackRayGen = state.shaderLibrary->getShader("FirstIndirectTraceRoughFallbackRayGen", nvrhi::ShaderType::RayGeneration);
    nvrhi::ShaderHandle producerShadeRayGen = state.shaderLibrary->getShader("FirstIndirectShadeRayGen", nvrhi::ShaderType::RayGeneration);
    nvrhi::ShaderHandle producerShadeFastRayGen = state.shaderLibrary->getShader("FirstIndirectShadeFastRayGen", nvrhi::ShaderType::RayGeneration);
    nvrhi::ShaderHandle seedRayGen = state.shaderLibrary->getShader("SeedRayGen", nvrhi::ShaderType::RayGeneration);
    nvrhi::ShaderHandle seedNoSpecRayGen = state.shaderLibrary->getShader("SeedNoSpecRayGen", nvrhi::ShaderType::RayGeneration);
    nvrhi::ShaderHandle specularSeedTraceRayGen = state.shaderLibrary->getShader("FirstIndirectSpecularTraceRayGen", nvrhi::ShaderType::RayGeneration);
    nvrhi::ShaderHandle specularSeedShadeRayGen = state.shaderLibrary->getShader("FirstIndirectSpecularShadeRayGen", nvrhi::ShaderType::RayGeneration);
    nvrhi::ShaderHandle specularSeedShadeFastRayGen = state.shaderLibrary->getShader("FirstIndirectSpecularShadeFastRayGen", nvrhi::ShaderType::RayGeneration);
    nvrhi::ShaderHandle reuseRayGen = state.shaderLibrary->getShader("ReuseRayGen", nvrhi::ShaderType::RayGeneration);
    nvrhi::ShaderHandle miss = state.shaderLibrary->getShader("Miss", nvrhi::ShaderType::Miss);
    nvrhi::ShaderHandle shadowMiss = state.shaderLibrary->getShader("ShadowMiss", nvrhi::ShaderType::Miss);
    nvrhi::ShaderHandle closestHit = state.shaderLibrary->getShader("ClosestHit", nvrhi::ShaderType::ClosestHit);
    nvrhi::ShaderHandle anyHit = state.shaderLibrary->getShader("AnyHit", nvrhi::ShaderType::AnyHit);
    nvrhi::ShaderHandle shadowClosestHit = state.shaderLibrary->getShader("ShadowClosestHit", nvrhi::ShaderType::ClosestHit);
    nvrhi::ShaderHandle shadowAnyHit = state.shaderLibrary->getShader("ShadowAnyHit", nvrhi::ShaderType::AnyHit);
    if (!producerTraceRayGen || !producerSimpleRayGen || !producerLeanTraceRayGen || !producerLeanShadeRayGen ||
        !producerRoughFallbackRayGen || !producerShadeRayGen || !producerShadeFastRayGen || !seedRayGen || !seedNoSpecRayGen ||
        !specularSeedTraceRayGen || !specularSeedShadeRayGen || !specularSeedShadeFastRayGen || !reuseRayGen ||
        !miss || !shadowMiss || !closestHit || !anyHit || !shadowClosestHit || !shadowAnyHit)
    {
        common->Printf("PathTraceCleanRestirGi: GI shader library is missing required entry points\n");
        return false;
    }

    nvrhi::rt::PipelineDesc pipelineDesc;
    pipelineDesc.globalBindingLayouts = { state.bindingLayout, inputs.textureBindlessLayout };
    pipelineDesc.shaders = {
        { "", producerTraceRayGen, nullptr },
        { "", producerSimpleRayGen, nullptr },
        { "", producerLeanTraceRayGen, nullptr },
        { "", producerLeanShadeRayGen, nullptr },
        { "", producerRoughFallbackRayGen, nullptr },
        { "", producerShadeRayGen, nullptr },
        { "", producerShadeFastRayGen, nullptr },
        { "", seedRayGen, nullptr },
        { "", seedNoSpecRayGen, nullptr },
        { "", specularSeedTraceRayGen, nullptr },
        { "", specularSeedShadeRayGen, nullptr },
        { "", specularSeedShadeFastRayGen, nullptr },
        { "", reuseRayGen, nullptr },
        { "", miss, nullptr },
        { "", shadowMiss, nullptr }
    };
    pipelineDesc.hitGroups = {
        { "HitGroup", closestHit, anyHit, nullptr, nullptr, false },
        { "ShadowHitGroup", shadowClosestHit, shadowAnyHit, nullptr, nullptr, false }
    };
    pipelineDesc.maxPayloadSize = 64;
    pipelineDesc.maxAttributeSize = 8;
    pipelineDesc.maxRecursionDepth = 1;

    state.pipeline = inputs.device->createRayTracingPipeline(pipelineDesc);
    if (!state.pipeline)
    {
        common->Printf("PathTraceCleanRestirGi: failed to create GI pipeline\n");
        return false;
    }
    state.producerShaderTable = state.pipeline->createShaderTable();
    state.producerSimpleShaderTable = state.pipeline->createShaderTable();
    state.producerLeanTraceShaderTable = state.pipeline->createShaderTable();
    state.producerLeanShadeShaderTable = state.pipeline->createShaderTable();
    state.producerRoughFallbackShaderTable = state.pipeline->createShaderTable();
    state.shadeShaderTable = state.pipeline->createShaderTable();
    state.shadeFastShaderTable = state.pipeline->createShaderTable();
    state.seedShaderTable = state.pipeline->createShaderTable();
    state.seedNoSpecShaderTable = state.pipeline->createShaderTable();
    state.specularSeedTraceShaderTable = state.pipeline->createShaderTable();
    state.specularSeedShadeShaderTable = state.pipeline->createShaderTable();
    state.specularSeedShadeFastShaderTable = state.pipeline->createShaderTable();
    state.reuseShaderTable = state.pipeline->createShaderTable();
    if (!state.producerShaderTable || !state.producerSimpleShaderTable ||
        !state.producerLeanTraceShaderTable || !state.producerLeanShadeShaderTable ||
        !state.producerRoughFallbackShaderTable ||
        !state.shadeShaderTable || !state.shadeFastShaderTable || !state.seedShaderTable ||
        !state.seedNoSpecShaderTable || !state.specularSeedTraceShaderTable ||
        !state.specularSeedShadeShaderTable || !state.specularSeedShadeFastShaderTable ||
        !state.reuseShaderTable)
    {
        common->Printf("PathTraceCleanRestirGi: failed to create GI shader table\n");
        state.pipeline = nullptr;
        return false;
    }
    state.producerShaderTable->setRayGenerationShader("FirstIndirectTraceRayGen");
    state.producerShaderTable->addMissShader("Miss");
    state.producerShaderTable->addMissShader("ShadowMiss");
    state.producerShaderTable->addHitGroup("HitGroup");
    state.producerShaderTable->addHitGroup("ShadowHitGroup");

    state.producerSimpleShaderTable->setRayGenerationShader("FirstIndirectSimpleRayGen");
    state.producerSimpleShaderTable->addMissShader("Miss");
    state.producerSimpleShaderTable->addMissShader("ShadowMiss");
    state.producerSimpleShaderTable->addHitGroup("HitGroup");
    state.producerSimpleShaderTable->addHitGroup("ShadowHitGroup");

    state.producerLeanTraceShaderTable->setRayGenerationShader("FirstIndirectLeanTraceRayGen");
    state.producerLeanTraceShaderTable->addMissShader("Miss");
    state.producerLeanTraceShaderTable->addMissShader("ShadowMiss");
    state.producerLeanTraceShaderTable->addHitGroup("HitGroup");
    state.producerLeanTraceShaderTable->addHitGroup("ShadowHitGroup");

    state.producerLeanShadeShaderTable->setRayGenerationShader("FirstIndirectLeanShadeRayGen");
    state.producerLeanShadeShaderTable->addMissShader("Miss");
    state.producerLeanShadeShaderTable->addMissShader("ShadowMiss");
    state.producerLeanShadeShaderTable->addHitGroup("HitGroup");
    state.producerLeanShadeShaderTable->addHitGroup("ShadowHitGroup");

    state.producerRoughFallbackShaderTable->setRayGenerationShader("FirstIndirectTraceRoughFallbackRayGen");
    state.producerRoughFallbackShaderTable->addMissShader("Miss");
    state.producerRoughFallbackShaderTable->addMissShader("ShadowMiss");
    state.producerRoughFallbackShaderTable->addHitGroup("HitGroup");
    state.producerRoughFallbackShaderTable->addHitGroup("ShadowHitGroup");

    state.shadeShaderTable->setRayGenerationShader("FirstIndirectShadeRayGen");
    state.shadeShaderTable->addMissShader("Miss");
    state.shadeShaderTable->addMissShader("ShadowMiss");
    state.shadeShaderTable->addHitGroup("HitGroup");
    state.shadeShaderTable->addHitGroup("ShadowHitGroup");

    state.shadeFastShaderTable->setRayGenerationShader("FirstIndirectShadeFastRayGen");
    state.shadeFastShaderTable->addMissShader("Miss");
    state.shadeFastShaderTable->addMissShader("ShadowMiss");
    state.shadeFastShaderTable->addHitGroup("HitGroup");
    state.shadeFastShaderTable->addHitGroup("ShadowHitGroup");

    state.seedShaderTable->setRayGenerationShader("SeedRayGen");
    state.seedShaderTable->addMissShader("Miss");
    state.seedShaderTable->addMissShader("ShadowMiss");
    state.seedShaderTable->addHitGroup("HitGroup");
    state.seedShaderTable->addHitGroup("ShadowHitGroup");

    state.seedNoSpecShaderTable->setRayGenerationShader("SeedNoSpecRayGen");
    state.seedNoSpecShaderTable->addMissShader("Miss");
    state.seedNoSpecShaderTable->addMissShader("ShadowMiss");
    state.seedNoSpecShaderTable->addHitGroup("HitGroup");
    state.seedNoSpecShaderTable->addHitGroup("ShadowHitGroup");

    state.specularSeedTraceShaderTable->setRayGenerationShader("FirstIndirectSpecularTraceRayGen");
    state.specularSeedTraceShaderTable->addMissShader("Miss");
    state.specularSeedTraceShaderTable->addMissShader("ShadowMiss");
    state.specularSeedTraceShaderTable->addHitGroup("HitGroup");
    state.specularSeedTraceShaderTable->addHitGroup("ShadowHitGroup");

    state.specularSeedShadeShaderTable->setRayGenerationShader("FirstIndirectSpecularShadeRayGen");
    state.specularSeedShadeShaderTable->addMissShader("Miss");
    state.specularSeedShadeShaderTable->addMissShader("ShadowMiss");
    state.specularSeedShadeShaderTable->addHitGroup("HitGroup");
    state.specularSeedShadeShaderTable->addHitGroup("ShadowHitGroup");

    state.specularSeedShadeFastShaderTable->setRayGenerationShader("FirstIndirectSpecularShadeFastRayGen");
    state.specularSeedShadeFastShaderTable->addMissShader("Miss");
    state.specularSeedShadeFastShaderTable->addMissShader("ShadowMiss");
    state.specularSeedShadeFastShaderTable->addHitGroup("HitGroup");
    state.specularSeedShadeFastShaderTable->addHitGroup("ShadowHitGroup");

    state.reuseShaderTable->setRayGenerationShader("ReuseRayGen");
    state.reuseShaderTable->addMissShader("Miss");
    state.reuseShaderTable->addMissShader("ShadowMiss");
    state.reuseShaderTable->addHitGroup("HitGroup");
    state.reuseShaderTable->addHitGroup("ShadowHitGroup");

    state.shaderTable = state.reuseShaderTable;
    common->Printf("PathTraceCleanRestirGi: GI pipeline initialized\n");
    return true;
}

void CleanRestirGiAddCommonComputeBindingLayoutItems(nvrhi::BindingLayoutDesc& layoutDesc)
{
    layoutDesc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(1));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(2));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(8));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(9));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(10));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(11));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(12));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(13));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(14));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(16));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(22));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(23));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(24));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(25));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(26));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(27));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(46));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(66));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(74));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(75));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(76));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(77));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(30));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(31));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(39));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(40));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(80));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(92));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(81));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(82));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(83));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(84));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(85));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(86));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(48));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(51));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(54));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(127));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
}

bool CleanRestirGiEnsureTemporalComputePipeline(PathTraceCleanRestirGiState& state, const PathTraceCleanRestirGiDispatchInputs& inputs)
{
    if (state.temporalComputePipeline)
    {
        return true;
    }
    if (state.temporalComputeInitAttempted)
    {
        return false;
    }
    state.temporalComputeInitAttempted = true;

    const char* shaderPath = nullptr;
    if (inputs.isD3D12)
    {
        shaderPath = "renderprogs2/dxil/builtin/pathtracing/remix_restir_gi/pathtrace_clean_restir_gi_temporal.cs.bin";
    }
    else if (inputs.isVulkan)
    {
        shaderPath = "renderprogs2/spirv/builtin/pathtracing/remix_restir_gi/pathtrace_clean_restir_gi_temporal.cs.bin";
    }
    else
    {
        return false;
    }

    void* shaderData = nullptr;
    ID_TIME_T shaderTimestamp = 0;
    const int shaderSize = fileSystem->ReadFile(shaderPath, &shaderData, &shaderTimestamp);
    if (shaderSize <= 0 || !shaderData)
    {
        common->Printf("PathTraceCleanRestirGi: couldn't read GI temporal compute shader %s\n", shaderPath);
        return false;
    }

    nvrhi::ShaderDesc csDesc;
    csDesc.shaderType = nvrhi::ShaderType::Compute;
    csDesc.entryName = "main";
    csDesc.debugName = "PathTraceCleanRestirGiTemporalCS";
    state.temporalComputeShader = inputs.device->createShader(csDesc, shaderData, shaderSize);
    Mem_Free(shaderData);
    if (!state.temporalComputeShader)
    {
        common->Printf("PathTraceCleanRestirGi: failed to create GI temporal compute shader\n");
        return false;
    }

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setConstantBufferOffset(0)
        .setUnorderedAccessViewOffset(0);
    CleanRestirGiAddCommonComputeBindingLayoutItems(layoutDesc);
    state.temporalComputeBindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!state.temporalComputeBindingLayout)
    {
        common->Printf("PathTraceCleanRestirGi: failed to create GI temporal compute binding layout\n");
        state.temporalComputeShader = nullptr;
        return false;
    }

    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = state.temporalComputeShader;
    pipelineDesc.bindingLayouts = { state.temporalComputeBindingLayout, inputs.textureBindlessLayout };
    state.temporalComputePipeline = inputs.device->createComputePipeline(pipelineDesc);
    if (!state.temporalComputePipeline)
    {
        common->Printf("PathTraceCleanRestirGi: failed to create GI temporal compute pipeline\n");
        state.temporalComputeBindingLayout = nullptr;
        state.temporalComputeShader = nullptr;
        return false;
    }

    common->Printf("PathTraceCleanRestirGi: GI temporal compute pipeline initialized\n");
    return true;
}

bool CleanRestirGiEnsureProducerRayQueryComputePipeline(PathTraceCleanRestirGiState& state, const PathTraceCleanRestirGiDispatchInputs& inputs)
{
    if (state.producerRayQueryComputePipeline)
    {
        return true;
    }
    if (state.producerRayQueryComputeInitAttempted)
    {
        return false;
    }
    state.producerRayQueryComputeInitAttempted = true;

    if (!inputs.device->queryFeatureSupport(nvrhi::Feature::RayQuery))
    {
        common->Printf("PathTraceCleanRestirGi: ray-query producer requested but RayQuery is not supported\n");
        return false;
    }

    const char* shaderPath = nullptr;
    if (inputs.isD3D12)
    {
        shaderPath = "renderprogs2/dxil/builtin/pathtracing/remix_restir_gi/pathtrace_clean_restir_gi_producer_rayquery.cs.bin";
    }
    else if (inputs.isVulkan)
    {
        shaderPath = "renderprogs2/spirv/builtin/pathtracing/remix_restir_gi/pathtrace_clean_restir_gi_producer_rayquery.cs.bin";
    }
    else
    {
        return false;
    }

    void* shaderData = nullptr;
    ID_TIME_T shaderTimestamp = 0;
    const int shaderSize = fileSystem->ReadFile(shaderPath, &shaderData, &shaderTimestamp);
    if (shaderSize <= 0 || !shaderData)
    {
        common->Printf("PathTraceCleanRestirGi: couldn't read GI producer ray-query compute shader %s\n", shaderPath);
        return false;
    }

    nvrhi::ShaderDesc csDesc;
    csDesc.shaderType = nvrhi::ShaderType::Compute;
    csDesc.entryName = "main";
    csDesc.debugName = "PathTraceCleanRestirGiProducerRayQueryCS";
    state.producerRayQueryComputeShader = inputs.device->createShader(csDesc, shaderData, shaderSize);
    Mem_Free(shaderData);
    if (!state.producerRayQueryComputeShader)
    {
        common->Printf("PathTraceCleanRestirGi: failed to create GI producer ray-query compute shader\n");
        return false;
    }

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setConstantBufferOffset(0)
        .setUnorderedAccessViewOffset(0);
    CleanRestirGiAddCommonComputeBindingLayoutItems(layoutDesc);
    state.producerRayQueryComputeBindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!state.producerRayQueryComputeBindingLayout)
    {
        common->Printf("PathTraceCleanRestirGi: failed to create GI producer ray-query compute binding layout\n");
        state.producerRayQueryComputeShader = nullptr;
        return false;
    }

    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = state.producerRayQueryComputeShader;
    pipelineDesc.bindingLayouts = { state.producerRayQueryComputeBindingLayout, inputs.textureBindlessLayout };
    state.producerRayQueryComputePipeline = inputs.device->createComputePipeline(pipelineDesc);
    if (!state.producerRayQueryComputePipeline)
    {
        common->Printf("PathTraceCleanRestirGi: failed to create GI producer ray-query compute pipeline\n");
        state.producerRayQueryComputeBindingLayout = nullptr;
        state.producerRayQueryComputeShader = nullptr;
        return false;
    }

    common->Printf("PathTraceCleanRestirGi: GI producer ray-query compute pipeline initialized\n");
    return true;
}

struct PathTraceCleanRestirGiBoilingFilterConstants
{
    uint32_t width;
    uint32_t height;
    float thresholdMin;
    float thresholdMax;
    uint32_t resolveEnabled;
    uint32_t rrInputResolveEnabled;
    uint32_t rrSpecularInputEnabled;
    float resolveGain;
};
static_assert(sizeof(PathTraceCleanRestirGiBoilingFilterConstants) == 32, "GI boiling filter constants size must match HLSL packing");

bool CleanRestirGiEnsureBoilingFilterPipeline(PathTraceCleanRestirGiState& state, const PathTraceCleanRestirGiDispatchInputs& inputs)
{
    if (state.boilingFilterPipeline)
    {
        return true;
    }
    if (state.boilingFilterInitAttempted)
    {
        return false;
    }
    state.boilingFilterInitAttempted = true;

    const char* shaderPath = nullptr;
    if (inputs.isD3D12)
    {
        shaderPath = "renderprogs2/dxil/builtin/pathtracing/remix_restir_gi/pathtrace_clean_restir_gi_boiling_filter.cs.bin";
    }
    else if (inputs.isVulkan)
    {
        shaderPath = "renderprogs2/spirv/builtin/pathtracing/remix_restir_gi/pathtrace_clean_restir_gi_boiling_filter.cs.bin";
    }
    else
    {
        return false;
    }

    void* shaderData = nullptr;
    ID_TIME_T shaderTimestamp = 0;
    const int shaderSize = fileSystem->ReadFile(shaderPath, &shaderData, &shaderTimestamp);
    if (shaderSize <= 0 || !shaderData)
    {
        common->Printf("PathTraceCleanRestirGi: couldn't read GI boiling-filter shader %s\n", shaderPath);
        return false;
    }
    nvrhi::ShaderDesc csDesc;
    csDesc.shaderType = nvrhi::ShaderType::Compute;
    csDesc.entryName = "main";
    csDesc.debugName = "PathTraceCleanRestirGiBoilingFilterCS";
    state.boilingFilterShader = inputs.device->createShader(csDesc, shaderData, shaderSize);
    Mem_Free(shaderData);
    if (!state.boilingFilterShader)
    {
        common->Printf("PathTraceCleanRestirGi: failed to create GI boiling-filter shader\n");
        return false;
    }

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setConstantBufferOffset(0)
        .setUnorderedAccessViewOffset(0);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(1));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(2));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(3));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(5));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(6));
    state.boilingFilterBindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!state.boilingFilterBindingLayout)
    {
        common->Printf("PathTraceCleanRestirGi: failed to create GI boiling-filter binding layout\n");
        return false;
    }

    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = state.boilingFilterShader;
    pipelineDesc.bindingLayouts = { state.boilingFilterBindingLayout };
    state.boilingFilterPipeline = inputs.device->createComputePipeline(pipelineDesc);
    if (!state.boilingFilterPipeline)
    {
        common->Printf("PathTraceCleanRestirGi: failed to create GI boiling-filter pipeline\n");
        return false;
    }

    nvrhi::BufferDesc constantsDesc;
    constantsDesc.byteSize = sizeof(PathTraceCleanRestirGiBoilingFilterConstants);
    constantsDesc.debugName = "PathTraceCleanRestirGiBoilingFilterConstants";
    constantsDesc.isConstantBuffer = true;
    constantsDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    constantsDesc.keepInitialState = true;
    state.boilingFilterConstantsBuffer = inputs.device->createBuffer(constantsDesc);
    if (!state.boilingFilterConstantsBuffer)
    {
        common->Printf("PathTraceCleanRestirGi: failed to create GI boiling-filter constants buffer\n");
        state.boilingFilterPipeline = nullptr;
        return false;
    }
    common->Printf("PathTraceCleanRestirGi: GI boiling-filter pipeline initialized\n");
    return true;
}

bool CleanRestirGiEnsureResources(PathTraceCleanRestirGiState& state, const PathTraceCleanRestirGiDispatchInputs& inputs)
{
    const uint32_t width = static_cast<uint32_t>(Max(inputs.width, 1));
    const uint32_t height = static_cast<uint32_t>(Max(inputs.height, 1));

    if (!state.constantsBuffer)
    {
        nvrhi::BufferDesc constantsDesc;
        constantsDesc.byteSize = 768;
        constantsDesc.debugName = "PathTraceCleanRestirGiConstants";
        constantsDesc.isConstantBuffer = true;
        constantsDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
        constantsDesc.keepInitialState = true;
        state.constantsBuffer = inputs.device->createBuffer(constantsDesc);
        if (!state.constantsBuffer)
        {
            common->Printf("PathTraceCleanRestirGi: failed to create GI constants buffer\n");
            return false;
        }
    }
    if (!state.placeholderSrvBuffer)
    {
        nvrhi::BufferDesc placeholderDesc;
        placeholderDesc.debugName = "PathTraceCleanRestirGiPlaceholderSRV";
        placeholderDesc.byteSize = 256;
        placeholderDesc.structStride = sizeof(uint32_t);
        placeholderDesc.canHaveUAVs = false;
        placeholderDesc.canHaveTypedViews = false;
        placeholderDesc.initialState = nvrhi::ResourceStates::ShaderResource;
        placeholderDesc.keepInitialState = true;
        state.placeholderSrvBuffer = inputs.device->createBuffer(placeholderDesc);
        if (!state.placeholderSrvBuffer)
        {
            common->Printf("PathTraceCleanRestirGi: failed to create GI placeholder SRV buffer\n");
            return false;
        }
    }

    const RTXDI_ReservoirBufferParameters reservoirParams =
        rtxdi::CalculateReservoirBufferParameters(width, height, rtxdi::CheckerboardMode::Off);
    const uint64_t reservoirBytes =
        static_cast<uint64_t>(reservoirParams.reservoirArrayPitch) *
        static_cast<uint64_t>(CLEAN_RESTIR_GI_PAGE_COUNT) *
        static_cast<uint64_t>(sizeof(RTXDI_PackedGIReservoir));
    const bool reservoirValid =
        state.reservoirBuffer &&
        state.reservoirWidth == width &&
        state.reservoirHeight == height &&
        state.reservoirBuffer->getDesc().byteSize >= reservoirBytes;
    if (!reservoirValid)
    {
        nvrhi::BufferDesc reservoirDesc;
        reservoirDesc.byteSize = reservoirBytes;
        reservoirDesc.structStride = sizeof(RTXDI_PackedGIReservoir);
        reservoirDesc.canHaveUAVs = true;
        reservoirDesc.debugName = "PathTraceCleanRestirGiReservoirs";
        reservoirDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        reservoirDesc.keepInitialState = true;
        state.reservoirBuffer = inputs.device->createBuffer(reservoirDesc);
        if (!state.reservoirBuffer)
        {
            common->Printf("PathTraceCleanRestirGi: failed to create GI reservoir buffer (%llu bytes)\n",
                static_cast<unsigned long long>(reservoirBytes));
            return false;
        }
        state.reservoirWidth = width;
        state.reservoirHeight = height;
        state.reservoirArrayPitch = reservoirParams.reservoirArrayPitch;
        state.reservoirBlockRowPitch = reservoirParams.reservoirBlockRowPitch;
        state.reservoirBytes = reservoirBytes;
        state.reservoirClearPending = true;
    }

    const bool texturesValid =
        state.producerRadianceTexture &&
        state.producerHitPositionTexture &&
        state.producerHitNormalTexture &&
        state.producerSurfaceBuffer &&
        state.indirectDiffuseTexture &&
        state.indirectDiffuseLobeTexture &&
        state.indirectSpecularLobeTexture &&
        state.producerRadianceTexture->getDesc().width == width &&
        state.producerRadianceTexture->getDesc().height == height;
    if (!texturesValid)
    {
        nvrhi::TextureDesc producerDesc;
        producerDesc.width = width;
        producerDesc.height = height;
        producerDesc.mipLevels = 1;
        producerDesc.arraySize = 1;
        producerDesc.dimension = nvrhi::TextureDimension::Texture2D;
        producerDesc.isUAV = true;
        producerDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        producerDesc.keepInitialState = true;

        producerDesc.format = nvrhi::Format::RGBA16_FLOAT;
        producerDesc.debugName = "PathTraceCleanRestirGiProducerRadiance";
        state.producerRadianceTexture = inputs.device->createTexture(producerDesc);

        producerDesc.format = nvrhi::Format::RGBA32_FLOAT;
        producerDesc.debugName = "PathTraceCleanRestirGiProducerHitPosition";
        state.producerHitPositionTexture = inputs.device->createTexture(producerDesc);

        producerDesc.format = nvrhi::Format::RGBA16_FLOAT;
        producerDesc.debugName = "PathTraceCleanRestirGiProducerHitNormal";
        state.producerHitNormalTexture = inputs.device->createTexture(producerDesc);

        producerDesc.format = nvrhi::Format::RGBA16_FLOAT;
        producerDesc.debugName = "PathTraceCleanRestirGiIndirectDiffuse";
        state.indirectDiffuseTexture = inputs.device->createTexture(producerDesc);

        producerDesc.debugName = "PathTraceCleanRestirGiIndirectDiffuseLobe";
        state.indirectDiffuseLobeTexture = inputs.device->createTexture(producerDesc);

        producerDesc.debugName = "PathTraceCleanRestirGiIndirectSpecularLobe";
        state.indirectSpecularLobeTexture = inputs.device->createTexture(producerDesc);

        // Trace->shade first-indirect candidate surface per pixel.
        // structStride MUST match PathTraceFirstIndirectCandidateSurface in
        // pathtrace_first_indirect_candidate.hlsli (144 bytes).
        nvrhi::BufferDesc surfaceBufferDesc;
        surfaceBufferDesc.byteSize = uint64_t(width) * uint64_t(height) * 144ull;
        surfaceBufferDesc.structStride = 144;
        surfaceBufferDesc.canHaveUAVs = true;
        surfaceBufferDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        surfaceBufferDesc.keepInitialState = true;
        surfaceBufferDesc.debugName = "PathTraceFirstIndirectCandidateSurface";
        state.producerSurfaceBuffer = inputs.device->createBuffer(surfaceBufferDesc);

        if (!state.producerRadianceTexture || !state.producerHitPositionTexture || !state.producerHitNormalTexture ||
            !state.producerSurfaceBuffer ||
            !state.indirectDiffuseTexture || !state.indirectDiffuseLobeTexture || !state.indirectSpecularLobeTexture)
        {
            common->Printf("PathTraceCleanRestirGi: failed to create GI producer textures (%ux%u)\n", width, height);
            return false;
        }
    }

    return true;
}

} // namespace

void PathTraceCleanRestirGiState::ReleaseResources()
{
    constantsBuffer = nullptr;
    reservoirBuffer = nullptr;
    reservoirWidth = 0;
    reservoirHeight = 0;
    reservoirArrayPitch = 0;
    reservoirBlockRowPitch = 0;
    reservoirBytes = 0;
    reservoirClearPending = true;
    producerRadianceTexture = nullptr;
    producerHitPositionTexture = nullptr;
    producerHitNormalTexture = nullptr;
    producerSurfaceBuffer = nullptr;
    indirectDiffuseTexture = nullptr;
    indirectDiffuseLobeTexture = nullptr;
    indirectSpecularLobeTexture = nullptr;
    blueNoise.Release();
    placeholderSrvBuffer = nullptr;
    producerRayQueryComputeShader = nullptr;
    producerRayQueryComputeBindingLayout = nullptr;
    producerRayQueryComputePipeline = nullptr;
    producerRayQueryComputeInitAttempted = false;
    temporalComputeShader = nullptr;
    temporalComputeBindingLayout = nullptr;
    temporalComputePipeline = nullptr;
    temporalComputeInitAttempted = false;
    boilingFilterConstantsBuffer = nullptr;
    boilingFilterShader = nullptr;
    boilingFilterBindingLayout = nullptr;
    boilingFilterPipeline = nullptr;
    boilingFilterInitAttempted = false;
    bindingLayout = nullptr;
    shaderLibrary = nullptr;
    pipeline = nullptr;
    shaderTable = nullptr;
    producerShaderTable = nullptr;
    producerSimpleShaderTable = nullptr;
    producerLeanTraceShaderTable = nullptr;
    producerLeanShadeShaderTable = nullptr;
    producerRoughFallbackShaderTable = nullptr;
    shadeShaderTable = nullptr;
    shadeFastShaderTable = nullptr;
    seedShaderTable = nullptr;
    seedNoSpecShaderTable = nullptr;
    specularSeedTraceShaderTable = nullptr;
    specularSeedShadeShaderTable = nullptr;
    specularSeedShadeFastShaderTable = nullptr;
    reuseShaderTable = nullptr;
    pipelineInitAttempted = false;
}

bool PathTraceCleanRestirGiExecute(
    PathTraceCleanRestirGiState& state,
    const PathTraceCleanRestirGiDispatchInputs& inputs)
{
    const bool dumpRequested = r_pathTracingCleanRestirGiDump.GetInteger() != 0;
    auto printDump = [&](const char* earlyReturn)
    {
        if (!dumpRequested)
        {
            return;
        }
        common->Printf(
            "PathTraceCleanRestirGi DUMP enable=%d view=%d temporal=%d spatial=%d biasCorrection=%d jacobian=%d "
            "maxHistory=%d maxAge=%d firefly=%.3f neeSeed=%d specProd=%d glossy2=%d glossy2Rough=%.2f rrHitDistance=%d rrSpecInput=%d resolve=%d finalMix=%d size=%dx%d frame=%u "
            "reservoirBuffer=%s pages[init=%u tIn=%u tOut=%u sOut=%u] arrayPitch=%u producerTex=%d pipeline=%d "
            "diBlob=%d lights=%d earlyReturn=%s\n",
            r_pathTracingCleanRestirGiEnable.GetInteger(),
            r_pathTracingCleanRestirGiView.GetInteger(),
            r_pathTracingCleanRestirGiTemporal.GetInteger(),
            r_pathTracingCleanRestirGiSpatial.GetInteger(),
            r_pathTracingCleanRestirGiTemporalBiasCorrection.GetInteger(),
            r_pathTracingCleanRestirGiJacobian.GetInteger(),
            r_pathTracingCleanRestirGiMaxHistoryLength.GetInteger(),
            r_pathTracingCleanRestirGiMaxReservoirAge.GetInteger(),
            r_pathTracingCleanRestirGiFireflyThreshold.GetFloat(),
            r_pathTracingCleanRestirGiNeeCacheSeed.GetInteger(),
            r_pathTracingCleanRestirGiSpecularProducer.GetInteger(),
            r_pathTracingCleanRestirGiGlossySecondRay.GetInteger(),
            r_pathTracingCleanRestirGiGlossySecondRayRoughness.GetFloat(),
            r_pathTracingCleanRestirGiRrHitDistance.GetInteger(),
            r_pathTracingCleanRestirGiRrSpecularInput.GetInteger(),
            r_pathTracingCleanRestirGiResolve.GetInteger(),
            r_pathTracingCleanRestirGiFinalMix.GetInteger(),
            inputs.width,
            inputs.height,
            state.frameIndex,
            state.reservoirBuffer ? "u80" : "none",
            CLEAN_RESTIR_GI_PAGE_INIT,
            CLEAN_RESTIR_GI_PAGE_TEMPORAL_INPUT,
            CLEAN_RESTIR_GI_PAGE_TEMPORAL_OUTPUT,
            CLEAN_RESTIR_GI_PAGE_SPATIAL_OUTPUT,
            state.reservoirArrayPitch,
            (state.producerRadianceTexture && state.producerHitPositionTexture && state.producerHitNormalTexture) ? 1 : 0,
            state.shaderTable ? 1 : 0,
            inputs.diConstantsBlob ? 1 : 0,
            inputs.doomAnalyticLightBuffer ? 1 : 0,
            earlyReturn);
        r_pathTracingCleanRestirGiDump.SetInteger(0);
    };
    auto clearFailureOutput = [&]()
    {
        if (!inputs.commandList || !inputs.outputTexture)
        {
            return;
        }
        inputs.commandList->setTextureState(inputs.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->commitBarriers();
        inputs.commandList->clearTextureFloat(inputs.outputTexture, nvrhi::AllSubresources, nvrhi::Color(0.75f, 0.0f, 0.75f, 1.0f));
    };
    auto missingResource = [&]() -> const char*
    {
        if (!inputs.device) return "device";
        if (!inputs.commandList) return "command-list";
        if (!inputs.outputTexture) return "output-texture";
        if (!inputs.textureBindlessLayout) return "texture-bindless-layout";
        if (!inputs.textureDescriptorTable) return "texture-descriptor-table";
        if (inputs.width <= 0 || inputs.height <= 0) return "extent";
        if (!inputs.diConstantsBlob) return "di-constants";
        if (inputs.diConstantsSize == 0 || inputs.diConstantsSize > CLEAN_RESTIR_GI_DI_BLOB_SIZE) return "di-constants-size";
        if (!inputs.tlas) return "tlas";
        if (!inputs.staticVertexBuffer) return "static-vertex";
        if (!inputs.staticIndexBuffer) return "static-index";
        if (!inputs.dynamicVertexBuffer) return "dynamic-vertex";
        if (!inputs.dynamicIndexBuffer) return "dynamic-index";
        if (!inputs.staticTriangleClassBuffer) return "static-triangle-class";
        if (!inputs.dynamicTriangleClassBuffer) return "dynamic-triangle-class";
        if (!inputs.staticTriangleMaterialBuffer) return "static-triangle-material";
        if (!inputs.dynamicTriangleMaterialBuffer) return "dynamic-triangle-material";
        if (!inputs.staticTriangleMaterialIndexBuffer) return "static-triangle-material-index";
        if (!inputs.dynamicTriangleMaterialIndexBuffer) return "dynamic-triangle-material-index";
        if (!inputs.materialTableBuffer) return "material-table";
        if (!inputs.fallbackTexture) return "fallback-texture";
        if (!inputs.emissiveTriangleBuffer) return "emissive-triangles";
        if (!inputs.emissiveDistributionBuffer) return "emissive-distribution";
        if (!inputs.rigidRouteVertexBuffer) return "rigid-route-vertex";
        if (!inputs.rigidRouteIndexBuffer) return "rigid-route-index";
        if (!inputs.rigidRouteTriangleMaterialBuffer) return "rigid-route-triangle-material";
        if (!inputs.rigidRouteTriangleMaterialIndexBuffer) return "rigid-route-triangle-material-index";
        if (!inputs.rigidRouteInstanceBuffer) return "rigid-route-instance";
        if (!inputs.doomAnalyticLightBuffer) return "doom-analytic-lights";
        if (!inputs.rluCurrentLightBuffer) return "rlu-current-lights";
        if (!inputs.neeCacheProviderResultBuffer && r_pathTracingCleanRestirGiNeeCacheSeed.GetInteger() != 0) return "nee-cache-provider-results";
        if (!inputs.primarySurfaceCurrentBuffer) return "primary-surface-current";
        if (!inputs.primarySurfacePreviousBuffer) return "primary-surface-previous";
        if (!inputs.motionVectorTexture) return "motion-vectors";
        if (!inputs.motionVectorMaskTexture) return "motion-vector-mask";
        if (!inputs.rrInputColorTexture) return "rr-input-color";
        if (!inputs.rrGuideAlbedoTexture) return "rr-guide-albedo";
        if (!inputs.rrGuideHitDistanceTexture) return "rr-guide-hit-distance";
        if (!inputs.materialSampler) return "material-sampler";
        return nullptr;
    };

    if (r_pathTracingCleanRestirGiEnable.GetInteger() == 0)
    {
        printDump("disabled");
        return false;
    }

    const int view = idMath::ClampInt(0, 24, r_pathTracingCleanRestirGiView.GetInteger());
    const int specularProducerMode = idMath::ClampInt(0, 2, r_pathTracingCleanRestirGiSpecularProducer.GetInteger());
    const bool rrHitDistanceRequested =
        r_pathTracingCleanRestirGiRrHitDistance.GetInteger() != 0 &&
        specularProducerMode != 0;
    const bool rrSpecularInputRequested =
        r_pathTracingCleanRestirGiRrSpecularInput.GetInteger() != 0 &&
        specularProducerMode != 0;
    if (view == 0 && r_pathTracingCleanRestirGiResolve.GetInteger() == 0 && !rrHitDistanceRequested && !rrSpecularInputRequested)
    {
        // Nothing consumes the lane yet without a debug view or resolve.
        printDump("no-view");
        return false;
    }

    if (!inputs.device || !inputs.commandList || !inputs.outputTexture || !inputs.textureBindlessLayout || !inputs.textureDescriptorTable ||
        inputs.width <= 0 || inputs.height <= 0 ||
        !inputs.diConstantsBlob || inputs.diConstantsSize == 0 || inputs.diConstantsSize > CLEAN_RESTIR_GI_DI_BLOB_SIZE ||
        !inputs.tlas || !inputs.staticVertexBuffer || !inputs.staticIndexBuffer ||
        !inputs.dynamicVertexBuffer || !inputs.dynamicIndexBuffer ||
        !inputs.staticTriangleClassBuffer || !inputs.dynamicTriangleClassBuffer ||
        !inputs.staticTriangleMaterialBuffer || !inputs.dynamicTriangleMaterialBuffer ||
        !inputs.staticTriangleMaterialIndexBuffer || !inputs.dynamicTriangleMaterialIndexBuffer ||
        !inputs.materialTableBuffer || !inputs.fallbackTexture || !inputs.emissiveTriangleBuffer ||
        !inputs.rigidRouteVertexBuffer || !inputs.rigidRouteIndexBuffer ||
        !inputs.rigidRouteTriangleMaterialBuffer ||
        !inputs.rigidRouteTriangleMaterialIndexBuffer || !inputs.rigidRouteInstanceBuffer ||
        !inputs.doomAnalyticLightBuffer ||
        !inputs.emissiveDistributionBuffer || !inputs.rluCurrentLightBuffer ||
        (!inputs.neeCacheProviderResultBuffer && r_pathTracingCleanRestirGiNeeCacheSeed.GetInteger() != 0) ||
        !inputs.primarySurfaceCurrentBuffer || !inputs.primarySurfacePreviousBuffer ||
        !inputs.motionVectorTexture || !inputs.motionVectorMaskTexture ||
        !inputs.rrInputColorTexture || !inputs.rrGuideAlbedoTexture || !inputs.rrGuideHitDistanceTexture ||
        !inputs.materialSampler)
    {
        clearFailureOutput();
        printDump(missingResource());
        return false;
    }

    if (!CleanRestirGiEnsurePipeline(state, inputs))
    {
        clearFailureOutput();
        printDump("pipeline");
        return false;
    }
    if (!CleanRestirGiEnsureResources(state, inputs))
    {
        clearFailureOutput();
        printDump("resources");
        return false;
    }
    // Non-fatal for missing/malformed masks: the helper still leaves a dummy
    // texture bound at t127 and reports blue noise unavailable to the cbuffer.
    PathTraceEnsureBlueNoise(
        state.blueNoise,
        inputs.device,
        "PathTraceCleanRestirGi",
        "PathTraceCleanRestirGiBlueNoise");
    if (!state.blueNoise.texture)
    {
        clearFailureOutput();
        printDump("blue-noise-texture");
        return false;
    }

    nvrhi::BindingSetDesc bindingSetDesc;
    bindingSetDesc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, inputs.tlas));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(1, inputs.outputTexture));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(2, state.constantsBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, inputs.staticVertexBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, inputs.staticIndexBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(5, inputs.staticTriangleClassBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, inputs.dynamicVertexBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, inputs.dynamicIndexBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(8, inputs.dynamicTriangleClassBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(9, inputs.staticTriangleMaterialBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(10, inputs.dynamicTriangleMaterialBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(11, inputs.staticTriangleMaterialIndexBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(12, inputs.dynamicTriangleMaterialIndexBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(13, inputs.materialTableBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(14, inputs.fallbackTexture));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(16, inputs.emissiveTriangleBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(22, inputs.rigidRouteVertexBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(23, inputs.rigidRouteIndexBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(24, inputs.rigidRouteTriangleMaterialBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(25, inputs.rigidRouteTriangleMaterialIndexBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(26, inputs.rigidRouteInstanceBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(27, inputs.doomAnalyticLightBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(46, inputs.emissiveDistributionBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(66, inputs.rluCurrentLightBuffer));
    nvrhi::IBuffer* neeCacheProviderResultBuffer = inputs.neeCacheProviderResultBuffer ? inputs.neeCacheProviderResultBuffer : state.placeholderSrvBuffer.Get();
    nvrhi::IBuffer* neeCacheCellBuffer = inputs.neeCacheCellBuffer ? inputs.neeCacheCellBuffer : state.placeholderSrvBuffer.Get();
    nvrhi::IBuffer* dynamicMaterialBuffer = inputs.dynamicMaterialBuffer ? inputs.dynamicMaterialBuffer : state.placeholderSrvBuffer.Get();
    nvrhi::IBuffer* neeCacheCandidateBuffer = inputs.neeCacheCandidateBuffer ? inputs.neeCacheCandidateBuffer : state.placeholderSrvBuffer.Get();
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(74, neeCacheProviderResultBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(75, neeCacheCellBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(76, dynamicMaterialBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(77, neeCacheCandidateBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(30, inputs.primarySurfaceCurrentBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(31, inputs.primarySurfacePreviousBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(39, inputs.motionVectorTexture));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(40, inputs.motionVectorMaskTexture));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(80, state.reservoirBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(92, state.producerSurfaceBuffer));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(81, state.producerRadianceTexture));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(82, state.producerHitPositionTexture));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(83, state.producerHitNormalTexture));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(84, state.indirectDiffuseTexture));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(85, state.indirectDiffuseLobeTexture));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(86, state.indirectSpecularLobeTexture));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(48, inputs.rrGuideAlbedoTexture));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(51, inputs.rrGuideHitDistanceTexture));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(54, inputs.rrInputColorTexture));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(127, state.blueNoise.texture));
    bindingSetDesc.addItem(nvrhi::BindingSetItem::Sampler(0, inputs.materialSampler));
    nvrhi::BindingSetHandle bindingSet = inputs.device->createBindingSet(bindingSetDesc, state.bindingLayout);
    if (!bindingSet)
    {
        clearFailureOutput();
        printDump("binding-set");
        return false;
    }

    nvrhi::ICommandList* commandList = inputs.commandList;
    // GPU profiler markers: split the GI raygen dispatches so Nsight resolves
    // producer/temporal/spatial individually instead of one merged "raygen" bar.
    // No-op unless VK_EXT_debug_utils is enabled and the cvar is set.
    const bool nsightGpuMarkers = r_pathTracingNsightGpuMarkers.GetInteger() != 0;
    PathTraceUploadBlueNoise(state.blueNoise, commandList);
    if (state.reservoirClearPending)
    {
        commandList->setBufferState(state.reservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
        commandList->commitBarriers();
        commandList->clearBufferUInt(state.reservoirBuffer, 0);
        state.reservoirClearPending = false;
    }

    uint8_t constants[CLEAN_RESTIR_GI_CONSTANTS_SIZE] = {};
    std::memcpy(constants, inputs.diConstantsBlob, inputs.diConstantsSize);
    if (inputs.doomAnalyticLightCountOverride > 0u &&
        inputs.diConstantsSize >= CLEAN_RESTIR_GI_DI_ANALYTIC_LIGHT_COUNT_OFFSET + sizeof(uint32_t))
    {
        std::memcpy(
            constants + CLEAN_RESTIR_GI_DI_ANALYTIC_LIGHT_COUNT_OFFSET,
            &inputs.doomAnalyticLightCountOverride,
            sizeof(inputs.doomAnalyticLightCountOverride));
    }
    PathTraceCleanRestirGiConstantsTail tail = {};
    tail.view = static_cast<uint32_t>(view);
    tail.temporalEnabled = r_pathTracingCleanRestirGiTemporal.GetInteger() != 0 ? 1u : 0u;
    tail.spatialEnabled = r_pathTracingCleanRestirGiSpatial.GetInteger() != 0 ? 1u : 0u;
    tail.biasCorrection = static_cast<uint32_t>(idMath::ClampInt(0, 1, r_pathTracingCleanRestirGiTemporalBiasCorrection.GetInteger()));
    tail.jacobianEnabled = r_pathTracingCleanRestirGiJacobian.GetInteger() != 0 ? 1u : 0u;
    tail.maxHistoryLength = static_cast<uint32_t>(idMath::ClampInt(0, 255, r_pathTracingCleanRestirGiMaxHistoryLength.GetInteger()));
    tail.maxReservoirAge = static_cast<uint32_t>(idMath::ClampInt(1, 255, r_pathTracingCleanRestirGiMaxReservoirAge.GetInteger()));
    tail.fireflyThreshold = Max(0.0f, r_pathTracingCleanRestirGiFireflyThreshold.GetFloat());
    tail.neeCacheSeedEnabled = r_pathTracingCleanRestirGiNeeCacheSeed.GetInteger() != 0 ? 1u : 0u;
    tail.frameIndex = state.frameIndex;
    tail.resolveEnabled = r_pathTracingCleanRestirGiResolve.GetInteger() != 0 ? 1u : 0u;
    tail.specularProducerEnabled = static_cast<uint32_t>(specularProducerMode);
    tail.rrHitDistanceEnabled = rrHitDistanceRequested ? 1u : 0u;
    tail.rrSpecularInputEnabled = rrSpecularInputRequested ? 1u : 0u;
    tail.neeCacheSecondaryEnabled = r_pathTracingCleanRestirGiNeeCacheSecondary.GetInteger() != 0 ? 1u : 0u;
    tail.neeCacheSecondaryMode = static_cast<uint32_t>(idMath::ClampInt(0, 2, r_pathTracingCleanRestirGiNeeCacheSecondaryMode.GetInteger()));
    tail.neeCacheSecondaryRoughness = idMath::ClampFloat(0.0f, 1.0f, r_pathTracingCleanRestirGiNeeCacheSecondaryRoughness.GetFloat());
    tail.neeCacheSecondaryProbability = idMath::ClampFloat(0.0f, 1.0f, r_pathTracingCleanRestirGiNeeCacheSecondaryProbability.GetFloat());
    tail.maxBounces = static_cast<uint32_t>(idMath::ClampInt(1, 2, r_pathTracingCleanRestirGiMaxBounces.GetInteger()));
    tail.continuationRouletteEnabled = r_pathTracingCleanRestirGiContinuationRoulette.GetInteger() != 0 ? 1u : 0u;
    tail.continuationRouletteMin = idMath::ClampFloat(0.01f, 1.0f, r_pathTracingCleanRestirGiContinuationRouletteMin.GetFloat());
    tail.continuationRouletteMax = idMath::ClampFloat(tail.continuationRouletteMin, 1.0f, r_pathTracingCleanRestirGiContinuationRouletteMax.GetFloat());
    tail.continuationDirectProbability = idMath::ClampFloat(0.0f, 1.0f, r_pathTracingCleanRestirGiContinuationDirectProbability.GetFloat());
    tail.secondaryDirectProbability = idMath::ClampFloat(0.0f, 1.0f, r_pathTracingCleanRestirGiSecondaryDirectProbability.GetFloat());
    tail.continuationOpaqueTrace = r_pathTracingCleanRestirGiContinuationOpaqueTrace.GetInteger() != 0 ? 1u : 0u;
    tail.producerOpaqueTrace = r_pathTracingCleanRestirGiProducerOpaqueTrace.GetInteger() != 0 ? 1u : 0u;
    tail.secondaryDirectSamples = static_cast<uint32_t>(idMath::ClampInt(1, 32, r_pathTracingCleanRestirGiSecondaryDirectSamples.GetInteger()));
    tail.secondaryRluCandidateCount = static_cast<uint32_t>(idMath::ClampInt(1, 16, r_pathTracingCleanRestirGiSecondaryRluCandidates.GetInteger()));
    tail.contributionFireflyThreshold = Max(0.0f, r_pathTracingCleanRestirGiContributionFireflyThreshold.GetFloat());
    tail.blueNoiseEnabled = (state.blueNoise.valid && r_pathTracingCleanRestirGiBlueNoise.GetInteger() != 0) ? 1u : 0u;
    tail.producerRayQueryHitIdMode = static_cast<uint32_t>(
        idMath::ClampInt(0, 2, r_pathTracingCleanRestirGiProducerRayQueryHitIdMode.GetInteger()));
    tail.spatialVisibilityMode = static_cast<uint32_t>(
        idMath::ClampInt(0, 2, r_pathTracingCleanRestirGiSpatialVisibility.GetInteger()));
    tail.glossySecondRayEnabled = r_pathTracingCleanRestirGiGlossySecondRay.GetInteger() != 0 ? 1u : 0u;
    tail.glossySecondRayMaxRoughness = idMath::ClampFloat(0.0f, 1.0f, r_pathTracingCleanRestirGiGlossySecondRayRoughness.GetFloat());
    tail.finalMixMode = static_cast<uint32_t>(idMath::ClampInt(0, 13, r_pathTracingCleanRestirGiFinalMix.GetInteger()));
    tail.reservoirParams.reservoirBlockRowPitch = state.reservoirBlockRowPitch;
    tail.reservoirParams.reservoirArrayPitch = state.reservoirArrayPitch;
    // Page rotation (RGI-04): this frame's temporal output is next frame's
    // temporal input, so the two pages alternate by frame parity. INIT and
    // SPATIAL_OUTPUT are transient within the frame.
    const bool oddFrame = (state.frameIndex & 1u) != 0u;
    tail.pageInfo[0] = CLEAN_RESTIR_GI_PAGE_INIT;
    tail.pageInfo[1] = oddFrame ? CLEAN_RESTIR_GI_PAGE_TEMPORAL_OUTPUT : CLEAN_RESTIR_GI_PAGE_TEMPORAL_INPUT;
    tail.pageInfo[2] = oddFrame ? CLEAN_RESTIR_GI_PAGE_TEMPORAL_INPUT : CLEAN_RESTIR_GI_PAGE_TEMPORAL_OUTPUT;
    tail.pageInfo[3] = CLEAN_RESTIR_GI_PAGE_SPATIAL_OUTPUT;
    std::memcpy(constants + CLEAN_RESTIR_GI_DI_BLOB_SIZE, &tail, sizeof(tail));
    commandList->writeBuffer(state.constantsBuffer, constants, sizeof(constants));

    commandList->setTextureState(inputs.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setTextureState(state.producerRadianceTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setTextureState(state.producerHitPositionTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setTextureState(state.producerHitNormalTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setAccelStructState(inputs.tlas, nvrhi::ResourceStates::AccelStructRead);
    commandList->setBufferState(state.reservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setBufferState(state.producerSurfaceBuffer, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setBufferState(inputs.primarySurfaceCurrentBuffer, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setBufferState(inputs.primarySurfacePreviousBuffer, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setTextureState(inputs.motionVectorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setTextureState(inputs.motionVectorMaskTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setTextureState(state.indirectDiffuseTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setTextureState(state.indirectDiffuseLobeTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setTextureState(state.indirectSpecularLobeTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setTextureState(inputs.rrGuideAlbedoTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    if (tail.rrHitDistanceEnabled != 0u)
    {
        commandList->setTextureState(inputs.rrGuideHitDistanceTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    }
    commandList->setTextureState(inputs.rrInputColorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
    commandList->setBufferState(inputs.staticVertexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.staticIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.dynamicVertexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.dynamicIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.staticTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.dynamicTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.staticTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.dynamicTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.staticTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.dynamicTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.materialTableBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(inputs.fallbackTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->setTextureState(state.blueNoise.texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.emissiveTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.emissiveDistributionBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.rigidRouteVertexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.rigidRouteIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.rigidRouteTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.rigidRouteTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.rigidRouteInstanceBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.doomAnalyticLightBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(inputs.rluCurrentLightBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(neeCacheProviderResultBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(neeCacheCellBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(dynamicMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->setBufferState(neeCacheCandidateBuffer, nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();

    const bool leanProducerActive =
        view != 22 &&
        r_pathTracingCleanRestirGiProducerLeanSplit.GetInteger() != 0;
    const bool simpleProducerActive =
        !leanProducerActive &&
        view != 22 &&
        r_pathTracingCleanRestirGiProducerSimple.GetInteger() != 0;

    nvrhi::BindingSetHandle producerRayQueryComputeBindingSet;
    const bool producerRayQueryComputeRequested =
        !simpleProducerActive &&
        r_pathTracingCleanRestirGiProducerRayQuery.GetInteger() != 0;
    if (producerRayQueryComputeRequested && CleanRestirGiEnsureProducerRayQueryComputePipeline(state, inputs))
    {
        producerRayQueryComputeBindingSet = inputs.device->createBindingSet(bindingSetDesc, state.producerRayQueryComputeBindingLayout);
        if (!producerRayQueryComputeBindingSet)
        {
            common->Printf("PathTraceCleanRestirGi: failed to create GI producer ray-query compute binding set; falling back to trace-rays\n");
        }
    }
    const bool producerRayQueryComputeActive = producerRayQueryComputeBindingSet != nullptr;

    nvrhi::BindingSetHandle temporalComputeBindingSet;
    const bool temporalComputeRequested = view == 0 && tail.spatialEnabled != 0u;
    if (temporalComputeRequested && CleanRestirGiEnsureTemporalComputePipeline(state, inputs))
    {
        temporalComputeBindingSet = inputs.device->createBindingSet(bindingSetDesc, state.temporalComputeBindingLayout);
        if (!temporalComputeBindingSet)
        {
            common->Printf("PathTraceCleanRestirGi: failed to create GI temporal compute binding set; falling back to raygen\n");
        }
    }
    const bool temporalComputeActive = temporalComputeBindingSet != nullptr;
    const bool defaultOneSampleShade =
        view == 0 &&
        tail.neeCacheSecondaryEnabled == 0u &&
        tail.maxBounces <= 1u &&
        tail.secondaryDirectSamples == 1u &&
        tail.secondaryDirectProbability >= 1.0f;

    nvrhi::rt::DispatchRaysArguments giArgs;
    giArgs.width = inputs.width;
    giArgs.height = inputs.height;
    giArgs.depth = 1;

    // Remix-shaped split: first produce indirect candidate radiance/hit
    // geometry, then run GI reuse/final shading as a separate consumer pass.
    // The producer itself is further split into a narrow trace pass (bounce +
    // surface G-buffer) and a shade pass (divergent direct-NEE) so each raygen
    // entry point is small enough to schedule at higher occupancy than the old
    // combined producer megakernel.
    if (leanProducerActive)
    {
        if (producerRayQueryComputeActive)
        {
            nvrhi::ComputeState producerTraceState;
            producerTraceState.pipeline = state.producerRayQueryComputePipeline;
            producerTraceState.bindings = { producerRayQueryComputeBindingSet, inputs.textureDescriptorTable };
            if (nsightGpuMarkers) { commandList->beginMarker("FirstIndirect.0a LeanTraceRayQuery Dispatch"); }
            commandList->setComputeState(producerTraceState);
            commandList->dispatch(
                static_cast<uint32_t>((inputs.width + 15) / 16),
                static_cast<uint32_t>((inputs.height + 7) / 8),
                1);
            if (nsightGpuMarkers) { commandList->endMarker(); }

            if (r_pathTracingCleanRestirGiProducerRayQueryRoughFallback.GetInteger() != 0)
            {
                nvrhi::utils::BufferUavBarrier(commandList, state.producerSurfaceBuffer);

                nvrhi::rt::State roughFallbackState;
                roughFallbackState.shaderTable = state.producerRoughFallbackShaderTable;
                roughFallbackState.bindings = { bindingSet, inputs.textureDescriptorTable };
                if (nsightGpuMarkers) { commandList->beginMarker("FirstIndirect.0a2 LeanTraceRoughFallback DispatchRays"); }
                commandList->setRayTracingState(roughFallbackState);
                commandList->dispatchRays(giArgs);
                if (nsightGpuMarkers) { commandList->endMarker(); }
            }
        }
        else
        {
            nvrhi::rt::State producerTraceState;
            producerTraceState.shaderTable = state.producerLeanTraceShaderTable;
            producerTraceState.bindings = { bindingSet, inputs.textureDescriptorTable };
            if (nsightGpuMarkers) { commandList->beginMarker("FirstIndirect.0a LeanTrace DispatchRays"); }
            commandList->setRayTracingState(producerTraceState);
            commandList->dispatchRays(giArgs);
            if (nsightGpuMarkers) { commandList->endMarker(); }
        }

        nvrhi::utils::BufferUavBarrier(commandList, state.producerSurfaceBuffer);
        nvrhi::utils::TextureUavBarrier(commandList, state.producerHitPositionTexture);
        nvrhi::utils::TextureUavBarrier(commandList, state.producerHitNormalTexture);

        nvrhi::rt::State producerShadeState;
        producerShadeState.shaderTable = state.producerLeanShadeShaderTable;
        producerShadeState.bindings = { bindingSet, inputs.textureDescriptorTable };
        if (nsightGpuMarkers) { commandList->beginMarker("FirstIndirect.0b LeanShade DispatchRays"); }
        commandList->setRayTracingState(producerShadeState);
        commandList->dispatchRays(giArgs);
        if (nsightGpuMarkers) { commandList->endMarker(); }

        nvrhi::utils::TextureUavBarrier(commandList, state.producerRadianceTexture);
    }
    else if (simpleProducerActive)
    {
        nvrhi::rt::State producerSimpleState;
        producerSimpleState.shaderTable = state.producerSimpleShaderTable;
        producerSimpleState.bindings = { bindingSet, inputs.textureDescriptorTable };
        if (nsightGpuMarkers) { commandList->beginMarker("FirstIndirect.0a Simple DispatchRays"); }
        commandList->setRayTracingState(producerSimpleState);
        commandList->dispatchRays(giArgs);
        if (nsightGpuMarkers) { commandList->endMarker(); }

        nvrhi::utils::BufferUavBarrier(commandList, state.producerSurfaceBuffer);
        nvrhi::utils::TextureUavBarrier(commandList, state.producerRadianceTexture);
        nvrhi::utils::TextureUavBarrier(commandList, state.producerHitPositionTexture);
        nvrhi::utils::TextureUavBarrier(commandList, state.producerHitNormalTexture);
    }
    else
    {
        if (producerRayQueryComputeActive)
        {
            nvrhi::ComputeState producerTraceState;
            producerTraceState.pipeline = state.producerRayQueryComputePipeline;
            producerTraceState.bindings = { producerRayQueryComputeBindingSet, inputs.textureDescriptorTable };
            if (nsightGpuMarkers) { commandList->beginMarker("FirstIndirect.0a TraceRayQuery Dispatch"); }
            commandList->setComputeState(producerTraceState);
            commandList->dispatch(
                static_cast<uint32_t>((inputs.width + 15) / 16),
                static_cast<uint32_t>((inputs.height + 7) / 8),
                1);
            if (nsightGpuMarkers) { commandList->endMarker(); }

            const bool runRoughFallback =
                r_pathTracingCleanRestirGiProducerRayQueryRoughFallback.GetInteger() != 0 || view == 22;
            if (runRoughFallback)
            {
                nvrhi::utils::BufferUavBarrier(commandList, state.producerSurfaceBuffer);

                nvrhi::rt::State roughFallbackState;
                roughFallbackState.shaderTable = state.producerRoughFallbackShaderTable;
                roughFallbackState.bindings = { bindingSet, inputs.textureDescriptorTable };
                if (nsightGpuMarkers) { commandList->beginMarker("FirstIndirect.0a2 TraceRoughFallback DispatchRays"); }
                commandList->setRayTracingState(roughFallbackState);
                commandList->dispatchRays(giArgs);
                if (nsightGpuMarkers) { commandList->endMarker(); }
            }
        }
        else
        {
            nvrhi::rt::State producerTraceState;
            producerTraceState.shaderTable = state.producerShaderTable;
            producerTraceState.bindings = { bindingSet, inputs.textureDescriptorTable };
            if (nsightGpuMarkers) { commandList->beginMarker("FirstIndirect.0a Trace DispatchRays"); }
            commandList->setRayTracingState(producerTraceState);
            commandList->dispatchRays(giArgs);
            if (nsightGpuMarkers) { commandList->endMarker(); }
        }

        // Trace outputs consumed by the shade pass: the surface G-buffer + hit
        // geometry textures.
        nvrhi::utils::BufferUavBarrier(commandList, state.producerSurfaceBuffer);
        nvrhi::utils::TextureUavBarrier(commandList, state.producerHitPositionTexture);
        nvrhi::utils::TextureUavBarrier(commandList, state.producerHitNormalTexture);

        nvrhi::rt::State producerShadeState;
        producerShadeState.shaderTable = defaultOneSampleShade ? state.shadeFastShaderTable : state.shadeShaderTable;
        producerShadeState.bindings = { bindingSet, inputs.textureDescriptorTable };
        if (nsightGpuMarkers)
        {
            commandList->beginMarker(defaultOneSampleShade
                ? "FirstIndirect.0b ShadeFast DispatchRays"
                : "FirstIndirect.0b Shade DispatchRays");
        }
        commandList->setRayTracingState(producerShadeState);
        commandList->dispatchRays(giArgs);
        if (nsightGpuMarkers) { commandList->endMarker(); }

        nvrhi::utils::TextureUavBarrier(commandList, state.producerRadianceTexture);
    }

    // INIT-page seed pass: mode 1 splits the specular producer's trace and
    // shade work away from the INIT clear/NEE seed so Nsight can isolate the
    // broad work. Mode 2 keeps specular final-output eligibility active but
    // deliberately skips this extra full-screen first-indirect seed path.
    const bool splitSpecularSeed = tail.specularProducerEnabled == 1u;
    if (splitSpecularSeed)
    {
        nvrhi::rt::State seedNoSpecState;
        seedNoSpecState.shaderTable = state.seedNoSpecShaderTable;
        seedNoSpecState.bindings = { bindingSet, inputs.textureDescriptorTable };
        if (nsightGpuMarkers) { commandList->beginMarker("CleanGI.0c InitSeedClearNee DispatchRays"); }
        commandList->setRayTracingState(seedNoSpecState);
        commandList->dispatchRays(giArgs);
        if (nsightGpuMarkers) { commandList->endMarker(); }
        nvrhi::utils::BufferUavBarrier(commandList, state.reservoirBuffer);

        nvrhi::rt::State specularSeedTraceState;
        specularSeedTraceState.shaderTable = state.specularSeedTraceShaderTable;
        specularSeedTraceState.bindings = { bindingSet, inputs.textureDescriptorTable };
        if (nsightGpuMarkers) { commandList->beginMarker("FirstIndirect.0d SpecularTrace DispatchRays"); }
        commandList->setRayTracingState(specularSeedTraceState);
        commandList->dispatchRays(giArgs);
        if (nsightGpuMarkers) { commandList->endMarker(); }
        nvrhi::utils::BufferUavBarrier(commandList, state.producerSurfaceBuffer);

        nvrhi::rt::State specularSeedShadeState;
        specularSeedShadeState.shaderTable = defaultOneSampleShade ? state.specularSeedShadeFastShaderTable : state.specularSeedShadeShaderTable;
        specularSeedShadeState.bindings = { bindingSet, inputs.textureDescriptorTable };
        if (nsightGpuMarkers)
        {
            commandList->beginMarker(defaultOneSampleShade
                ? "FirstIndirect.0e SpecularShadeFast DispatchRays"
                : "FirstIndirect.0e SpecularShade DispatchRays");
        }
        commandList->setRayTracingState(specularSeedShadeState);
        commandList->dispatchRays(giArgs);
        if (nsightGpuMarkers) { commandList->endMarker(); }
        nvrhi::utils::BufferUavBarrier(commandList, state.reservoirBuffer);
    }
    else
    {
        nvrhi::rt::State seedState;
        seedState.shaderTable = state.seedShaderTable;
        seedState.bindings = { bindingSet, inputs.textureDescriptorTable };
        if (nsightGpuMarkers) { commandList->beginMarker("CleanGI.0c InitSeed DispatchRays"); }
        commandList->setRayTracingState(seedState);
        commandList->dispatchRays(giArgs);
        if (nsightGpuMarkers) { commandList->endMarker(); }
        nvrhi::utils::BufferUavBarrier(commandList, state.reservoirBuffer);
    }

    nvrhi::rt::State reuseState;
    reuseState.shaderTable = state.reuseShaderTable;
    reuseState.bindings = { bindingSet, inputs.textureDescriptorTable };
    if (temporalComputeActive)
    {
        nvrhi::ComputeState temporalState;
        temporalState.pipeline = state.temporalComputePipeline;
        temporalState.bindings = { temporalComputeBindingSet, inputs.textureDescriptorTable };
        if (nsightGpuMarkers) { commandList->beginMarker("CleanGI.1 TemporalReuse Dispatch"); }
        commandList->setComputeState(temporalState);
        commandList->dispatch(
            static_cast<uint32_t>((inputs.width + 15) / 16),
            static_cast<uint32_t>((inputs.height + 7) / 8),
            1);
        if (nsightGpuMarkers) { commandList->endMarker(); }
    }
    else
    {
        if (nsightGpuMarkers) { commandList->beginMarker("CleanGI.1 TemporalReuse DispatchRays"); }
        commandList->setRayTracingState(reuseState);
        commandList->dispatchRays(giArgs);
        if (nsightGpuMarkers) { commandList->endMarker(); }
    }

    nvrhi::utils::TextureUavBarrier(commandList, inputs.outputTexture);
    nvrhi::utils::BufferUavBarrier(commandList, state.reservoirBuffer);

    nvrhi::utils::TextureUavBarrier(commandList, state.indirectDiffuseTexture);
    nvrhi::utils::TextureUavBarrier(commandList, state.indirectDiffuseLobeTexture);
    nvrhi::utils::TextureUavBarrier(commandList, state.indirectSpecularLobeTexture);
    if (tail.rrHitDistanceEnabled != 0u)
    {
        nvrhi::utils::TextureUavBarrier(commandList, inputs.rrGuideHitDistanceTexture);
    }

    // RGI-06: spatial reuse runs as a second dispatch so every pixel's
    // TEMPORAL_OUTPUT page write has completed before neighbors read it.
    // With spatial disabled, phase 0 already passed the temporal output
    // through to the SPATIAL_OUTPUT page.
    if (tail.spatialEnabled != 0u)
    {
        tail.phase = 1u;
        std::memcpy(constants + CLEAN_RESTIR_GI_DI_BLOB_SIZE, &tail, sizeof(tail));
        commandList->writeBuffer(state.constantsBuffer, constants, sizeof(constants));
        if (nsightGpuMarkers) { commandList->beginMarker("CleanGI.2 SpatialReuse DispatchRays"); }
        commandList->setRayTracingState(reuseState);
        commandList->dispatchRays(giArgs);
        if (nsightGpuMarkers) { commandList->endMarker(); }
        nvrhi::utils::TextureUavBarrier(commandList, inputs.outputTexture);
        nvrhi::utils::BufferUavBarrier(commandList, state.reservoirBuffer);
        nvrhi::utils::TextureUavBarrier(commandList, state.indirectDiffuseTexture);
        nvrhi::utils::TextureUavBarrier(commandList, state.indirectDiffuseLobeTexture);
        nvrhi::utils::TextureUavBarrier(commandList, state.indirectSpecularLobeTexture);
        if (tail.rrHitDistanceEnabled != 0u)
        {
            nvrhi::utils::TextureUavBarrier(commandList, inputs.rrGuideHitDistanceTexture);
        }
    }

    // RGI-08: boiling filter + resolve consumer over the GI output. The
    // compute pass clamps shaded outliers to the group average (Remix
    // final-shading diffuse behavior) and performs the resolve add so the
    // combined outputs receive the filtered contribution. Debug views own
    // SmokeOutput, so the resolve add only runs with the views off.
    const float boilingFilterThresholdMin = Max(0.0f, r_pathTracingCleanRestirGiBoilingFilter.GetFloat());
    const float boilingFilterThresholdMax = Max(boilingFilterThresholdMin, r_pathTracingCleanRestirGiBoilingFilterMax.GetFloat());
    const bool resolveAddRequested = tail.resolveEnabled != 0u && view == 0;
    const bool rrSpecularExportRequested = tail.rrSpecularInputEnabled != 0u;
    const bool filterWorkRequested = view != 0 || resolveAddRequested || rrSpecularExportRequested;
    if (filterWorkRequested &&
        (boilingFilterThresholdMin > 0.0f || resolveAddRequested || rrSpecularExportRequested) &&
        CleanRestirGiEnsureBoilingFilterPipeline(state, inputs))
    {
        nvrhi::BindingSetDesc filterSetDesc;
        filterSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, state.boilingFilterConstantsBuffer));
        filterSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(1, state.indirectDiffuseTexture));
        filterSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(2, inputs.outputTexture));
        filterSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(3, inputs.rrInputColorTexture));
        filterSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, inputs.primarySurfaceCurrentBuffer));
        filterSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(5, state.indirectDiffuseLobeTexture));
        filterSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(6, state.indirectSpecularLobeTexture));
        nvrhi::BindingSetHandle filterSet = inputs.device->createBindingSet(filterSetDesc, state.boilingFilterBindingLayout);
        if (filterSet)
        {
            PathTraceCleanRestirGiBoilingFilterConstants filterConstants;
            filterConstants.width = static_cast<uint32_t>(inputs.width);
            filterConstants.height = static_cast<uint32_t>(inputs.height);
            filterConstants.thresholdMin = boilingFilterThresholdMin;
            filterConstants.thresholdMax = boilingFilterThresholdMax;
            filterConstants.resolveEnabled = resolveAddRequested ? 1u : 0u;
            filterConstants.rrInputResolveEnabled =
                resolveAddRequested && inputs.resolveToRrInputColor ? 1u : 0u;
            filterConstants.rrSpecularInputEnabled = rrSpecularExportRequested ? 1u : 0u;
            filterConstants.resolveGain = idMath::ClampFloat(0.0f, 32.0f, r_pathTracingCleanRestirGiResolveGain.GetFloat());
            commandList->writeBuffer(state.boilingFilterConstantsBuffer, &filterConstants, sizeof(filterConstants));

            commandList->setBufferState(inputs.primarySurfaceCurrentBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->commitBarriers();

            nvrhi::ComputeState filterState;
            filterState.pipeline = state.boilingFilterPipeline;
            filterState.bindings = { filterSet };
            if (nsightGpuMarkers) { commandList->beginMarker("CleanGI.3 BoilingFilter Dispatch"); }
            commandList->setComputeState(filterState);
            commandList->dispatch(
                static_cast<uint32_t>((inputs.width + 7) / 8),
                static_cast<uint32_t>((inputs.height + 7) / 8),
                1);
            if (nsightGpuMarkers) { commandList->endMarker(); }
            nvrhi::utils::TextureUavBarrier(commandList, state.indirectDiffuseTexture);
            nvrhi::utils::TextureUavBarrier(commandList, state.indirectDiffuseLobeTexture);
            nvrhi::utils::TextureUavBarrier(commandList, state.indirectSpecularLobeTexture);
            nvrhi::utils::TextureUavBarrier(commandList, inputs.outputTexture);
            nvrhi::utils::TextureUavBarrier(commandList, inputs.rrInputColorTexture);
        }
    }

    if (!state.dispatchLogged)
    {
        common->Printf("PathTraceCleanRestirGi: dispatched GI lane (%dx%d, view=%d)\n", inputs.width, inputs.height, view);
        state.dispatchLogged = true;
    }
    printDump("none");
    state.frameIndex++;
    return view != 0;
}
