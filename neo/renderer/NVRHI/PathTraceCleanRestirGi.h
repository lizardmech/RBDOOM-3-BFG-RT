#pragma once

// Clean-room Remix ReSTIR GI lane (docs/restir_remix_gi_cleanroom).
//
// Owns the GI lane's GPU state: reservoir pages, producer textures, constants,
// pipeline, and the per-frame dispatch. Gated entirely by the
// r_pathTracingCleanRestirGi* cvars; when r_pathTracingCleanRestirGiEnable is
// 0 nothing is dispatched and nothing is written.

#include <nvrhi/nvrhi.h>

#include "PathTraceBlueNoise.h"

#include <cstdint>

struct PathTraceCleanRestirGiState
{
    nvrhi::BufferHandle constantsBuffer;
    nvrhi::BufferHandle reservoirBuffer;
    uint32_t reservoirWidth = 0;
    uint32_t reservoirHeight = 0;
    uint32_t reservoirArrayPitch = 0;
    uint32_t reservoirBlockRowPitch = 0;
    uint64_t reservoirBytes = 0;
    bool reservoirClearPending = true;
    nvrhi::TextureHandle producerRadianceTexture;
    nvrhi::TextureHandle producerHitPositionTexture;
    nvrhi::TextureHandle producerHitNormalTexture;
    nvrhi::TextureHandle continuationRadianceTexture; // staged continuation throughput/radiance scratch
    nvrhi::BufferHandle producerSurfaceBuffer;   // trace->shade first-indirect candidate surface per pixel
    nvrhi::TextureHandle indirectDiffuseTexture;
    nvrhi::TextureHandle indirectDiffuseLobeTexture;
    nvrhi::TextureHandle indirectSpecularLobeTexture;
    nvrhi::BufferHandle placeholderSrvBuffer;
    nvrhi::BindingLayoutHandle bindingLayout;
    nvrhi::ShaderLibraryHandle shaderLibrary;
    nvrhi::rt::PipelineHandle pipeline;
    nvrhi::rt::ShaderTableHandle shaderTable;
    nvrhi::rt::ShaderTableHandle producerShaderTable;   // FirstIndirectTraceRayGen
    nvrhi::rt::ShaderTableHandle producerSimpleShaderTable;
    nvrhi::rt::ShaderTableHandle producerLeanTraceShaderTable;
    nvrhi::rt::ShaderTableHandle producerLeanShadeShaderTable;
    nvrhi::rt::ShaderTableHandle producerRoughFallbackShaderTable;
    nvrhi::rt::ShaderTableHandle continuationShaderTable;      // combined fallback for split specular seed
    nvrhi::rt::ShaderTableHandle continuationTraceShaderTable; // FirstIndirectContinuationTraceRayGen
    nvrhi::rt::ShaderTableHandle continuationShadeShaderTable; // FirstIndirectContinuationShadeRayGen
    nvrhi::rt::ShaderTableHandle shadeShaderTable;      // FirstIndirectShadeRayGen
    nvrhi::rt::ShaderTableHandle shadeFastShaderTable;  // FirstIndirectShadeFastRayGen
    nvrhi::rt::ShaderTableHandle seedShaderTable;       // SeedRayGen (INIT-page seeds)
    nvrhi::rt::ShaderTableHandle seedNoSpecShaderTable; // SeedNoSpecRayGen (INIT clear + NEE seed)
    nvrhi::rt::ShaderTableHandle specularSeedTraceShaderTable;
    nvrhi::rt::ShaderTableHandle specularSeedShadeShaderTable;
    nvrhi::rt::ShaderTableHandle specularSeedShadeFastShaderTable;
    nvrhi::rt::ShaderTableHandle reuseShaderTable;
    PathTraceBlueNoiseState blueNoise;
    nvrhi::ShaderHandle producerRayQueryComputeShader;
    nvrhi::BindingLayoutHandle producerRayQueryComputeBindingLayout;
    nvrhi::ComputePipelineHandle producerRayQueryComputePipeline;
    bool producerRayQueryComputeInitAttempted = false;
    nvrhi::ShaderHandle temporalComputeShader;
    nvrhi::BindingLayoutHandle temporalComputeBindingLayout;
    nvrhi::ComputePipelineHandle temporalComputePipeline;
    bool temporalComputeInitAttempted = false;
    nvrhi::BufferHandle skyResolveRadianceConstantsBuffer;
    nvrhi::BufferHandle skyResolveSurfaceConstantsBuffer;
    nvrhi::ShaderHandle skyResolveShader;
    nvrhi::BindingLayoutHandle skyResolveBindingLayout;
    nvrhi::ComputePipelineHandle skyResolvePipeline;
    bool skyResolveInitAttempted = false;
    nvrhi::BufferHandle boilingFilterConstantsBuffer;
    nvrhi::ShaderHandle boilingFilterShader;
    nvrhi::BindingLayoutHandle boilingFilterBindingLayout;
    nvrhi::ComputePipelineHandle boilingFilterPipeline;
    bool boilingFilterInitAttempted = false;
    bool pipelineInitAttempted = false;
    uint32_t frameIndex = 0;
    bool dispatchLogged = false;

    void ReleaseResources();
};

struct PathTraceCleanRestirGiDispatchInputs
{
    nvrhi::IDevice* device = nullptr;
    nvrhi::ICommandList* commandList = nullptr;
    nvrhi::ITexture* outputTexture = nullptr;
    nvrhi::IDescriptorTable* textureDescriptorTable = nullptr;
    nvrhi::IBindingLayout* textureBindlessLayout = nullptr;
    bool isD3D12 = false;
    bool isVulkan = false;
    int width = 0;
    int height = 0;

    // Live DI sentinel constants blob (PathTraceCleanRtxdiDiSentinelConstants,
    // 480 bytes). The GI cbuffer mirrors that layout in its leading block so
    // shared DI-lane helper code sees identical values.
    const void* diConstantsBlob = nullptr;
    uint32_t diConstantsSize = 0;
    uint32_t doomAnalyticLightCountOverride = 0;

    // Scene resources shared read-only with the DI lane (GI io_whitelist
    // input classes GI-I-01/04/05/06/09).
    nvrhi::rt::IAccelStruct* tlas = nullptr;
    nvrhi::IBuffer* staticVertexBuffer = nullptr;
    nvrhi::IBuffer* staticIndexBuffer = nullptr;
    nvrhi::IBuffer* dynamicVertexBuffer = nullptr;
    nvrhi::IBuffer* dynamicIndexBuffer = nullptr;
    nvrhi::IBuffer* staticTriangleClassBuffer = nullptr;
    nvrhi::IBuffer* dynamicTriangleClassBuffer = nullptr;
    nvrhi::IBuffer* staticTriangleMaterialBuffer = nullptr;
    nvrhi::IBuffer* dynamicTriangleMaterialBuffer = nullptr;
    nvrhi::IBuffer* staticTriangleMaterialIndexBuffer = nullptr;
    nvrhi::IBuffer* dynamicTriangleMaterialIndexBuffer = nullptr;
    nvrhi::IBuffer* materialTableBuffer = nullptr;
    nvrhi::IBuffer* dynamicMaterialBuffer = nullptr;
    nvrhi::ITexture* fallbackTexture = nullptr;
    nvrhi::ITexture* skyEnvironmentCube = nullptr;
    nvrhi::IBuffer* emissiveTriangleBuffer = nullptr;
    nvrhi::IBuffer* emissiveDistributionBuffer = nullptr;
    nvrhi::IBuffer* rigidRouteVertexBuffer = nullptr;
    nvrhi::IBuffer* rigidRouteIndexBuffer = nullptr;
    nvrhi::IBuffer* rigidRouteTriangleMaterialBuffer = nullptr;
    nvrhi::IBuffer* rigidRouteTriangleMaterialIndexBuffer = nullptr;
    nvrhi::IBuffer* rigidRouteInstanceBuffer = nullptr;
    nvrhi::IBuffer* doomAnalyticLightBuffer = nullptr;
    nvrhi::IBuffer* rluCurrentLightBuffer = nullptr;
    nvrhi::IBuffer* neeCacheProviderResultBuffer = nullptr;
    nvrhi::IBuffer* neeCacheCellBuffer = nullptr;
    nvrhi::IBuffer* neeCacheCandidateBuffer = nullptr;
    nvrhi::IBuffer* primarySurfaceCurrentBuffer = nullptr;
    nvrhi::IBuffer* primarySurfacePreviousBuffer = nullptr;
    nvrhi::ITexture* motionVectorTexture = nullptr;
    nvrhi::ITexture* motionVectorMaskTexture = nullptr;
    nvrhi::ITexture* rrInputColorTexture = nullptr;
    nvrhi::ITexture* rrGuideAlbedoTexture = nullptr;
    nvrhi::ITexture* rrGuideHitDistanceTexture = nullptr;
    nvrhi::ISampler* materialSampler = nullptr;

    // Set only when the caller schedules view-0 GI before DLSS-RR evaluation.
    // In that route the beauty resolve must also update PathTraceRRInputColor
    // so RR consumes the combined DI+GI signal instead of seeing DI only.
    bool resolveToRrInputColor = false;
};

// Runs the GI lane for this frame if its cvars request it. Returns true when
// a GI debug view was written to the output texture (so callers know the
// output now shows the GI lane).
bool PathTraceCleanRestirGiExecute(
    PathTraceCleanRestirGiState& state,
    const PathTraceCleanRestirGiDispatchInputs& inputs);
