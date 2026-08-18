#pragma once

// Isolated runtime owner for the clean Slang unified ReSTIR PT lane.
//
// UPT-06 admits exactly two 64-byte reservoir pages plus host-owned page
// metadata. UPT-07 temporal overwrites the current role in place; the default-
// off UPT-09 spatial baseline fully writes the history role and makes it the
// next frame's history. There is no third page or per-frame full-page clear. A
// separate one-shot diagnostic specialization owns one fixed counter/readback
// pair.

#include "PathTraceSceneInputs.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <memory>

struct PathTraceUnifiedLightRecord;
struct PathTraceSmokeEmissiveTriangle;

enum class PathTraceUnifiedPtBackend : uint32_t
{
    RayQuery = 0,
    RayGeneration = 1
};

enum class PathTraceUnifiedPtFamily : uint32_t
{
    Unified = 0,
    DirectOnly = 1,
    IndirectOnly = 2
};

enum PathTraceUnifiedPtMaterialPolicyFlags : uint32_t
{
    PATH_TRACE_UPT_MATERIAL_USE_SPECULAR_MAPS = 1u << 0u,
    PATH_TRACE_UPT_MATERIAL_LEGACY_SPECMAP_TO_PBR = 1u << 1u,
    PATH_TRACE_UPT_MATERIAL_DECODE_TEXTURES = 1u << 2u
};

struct PathTraceUnifiedPtDispatchInputs
{
    nvrhi::IDevice* device = nullptr;
    nvrhi::ICommandList* commandList = nullptr;
    const RtPathTraceSceneInputs* sceneInputs = nullptr;
    // CPU mirror of the exact manager payload bound through sceneInputs. This
    // is diagnostic-only: delayed reservoir probes use the stable fingerprint
    // to identify the current record without trusting a frame-local index.
    const PathTraceUnifiedLightRecord* currentLightRecords = nullptr;
    uint32_t currentLightRecordCount = 0;
    const PathTraceSmokeEmissiveTriangle* currentEmissiveTriangles = nullptr;
    uint32_t currentEmissiveTriangleCount = 0;
    nvrhi::BufferHandle primarySurfaceBuffer;
    nvrhi::BufferHandle primarySurfaceCurrentBuffer;
    nvrhi::BufferHandle primarySurfacePreviousBuffer;
    nvrhi::BufferHandle primaryHistorySidecarCurrentBuffer;
    nvrhi::BufferHandle primaryHistorySidecarPreviousBuffer;
    nvrhi::TextureHandle rrGuideSpecularAlbedo;
    nvrhi::TextureHandle skyEnvironment;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frameSampleIndex = 0;
    float emissiveScale = 1.0f;
    float skyBrightness = 1.0f;
    uint32_t materialPolicyFlags = 0;
    uint32_t proofStage = 1;
    uint32_t shaderProofMode = 1;
    PathTraceUnifiedPtBackend backend = PathTraceUnifiedPtBackend::RayQuery;
    PathTraceUnifiedPtFamily family = PathTraceUnifiedPtFamily::DirectOnly;
    // Diagnostic proposal restriction while retaining the family-0 pipeline
    // and reuse topology. Zero selects the normal mask implied by family.
    uint32_t diagnosticProposalFamilyMask = 0;
    bool nsightMarkers = false;
    bool diagnostics = false;
    uint32_t primaryReceiverMode = 0;
    bool compactPrimaryHistory = false;
    bool compactGeometry = false;
    bool compactLights = false;
    bool compactMaterials = false;
    bool splitInitial = false;
    bool threeVertexInitial = false;
    bool threeVertexSplit = false;
    bool geometryNoSkinned = false;
    bool splitContinuation = false;
    bool lambertDiagnostic = false;
    bool staticAnalyticOnly = false;
    bool emissiveCompact = false;
    bool frozenStaticDiagnostic = false;
    bool frozenLightDiagnostic = false;
    uint32_t temporalBottleneckProbe = 0;
    bool directProposalParity = false;
    bool lightTiles = false;
    bool temporal = false;
    bool duplication = false;
    bool spatial = false;
    // Private half-resolution glass-reflection domain.  This selects the
    // compact48 receiver specializations and forces direct-only reuse without
    // changing the main UPT estimator or its two persistent pages.
    bool reflectionReuseDomain = false;
    bool primarySurfaceHistoryValid = false;
    uint64_t historyEpoch = 0;
    uint32_t historyResetReasonFlags = 0;
    float primaryCameraOrigin[3] = {};
    float previousCameraOrigin[3] = {};
    float previousCameraForward[3] = {};
    float previousCameraLeft[3] = {};
    float previousCameraUp[3] = {};
    float previousCameraTanX = 1.0f;
    float previousCameraTanY = 1.0f;
    float previousCameraJitterPixels[2] = {};
};

struct PathTraceUnifiedPtPageMetadata
{
    bool fullyWritten = false;
    uint32_t width = 0;
    uint32_t height = 0;
    uint64_t contentGeneration = 0;
    uint64_t historyEpoch = 0;
    uint64_t frameSerial = 0;

    void Invalidate()
    {
        fullyWritten = false;
        width = 0;
        height = 0;
        contentGeneration = 0;
        historyEpoch = 0;
        frameSerial = 0;
    }
};

struct PathTraceUnifiedPtGlassComposeInputs
{
    nvrhi::IDevice* device = nullptr;
    nvrhi::ICommandList* commandList = nullptr;
    nvrhi::TextureHandle source;
    nvrhi::TextureHandle transmission;
    nvrhi::TextureHandle reflection;
    nvrhi::TextureHandle reflectionReuse;
    nvrhi::TextureHandle distortion;
    nvrhi::TextureHandle output;
    nvrhi::TextureHandle rrGuideAlbedo;
    nvrhi::TextureHandle rrGuideSpecularAlbedo;
    nvrhi::TextureHandle rrGuideNormalRoughness;
    nvrhi::TextureHandle rrGuidePosition;
    nvrhi::TextureHandle rrGuideDepth;
    nvrhi::TextureHandle rrMotionVectors;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frameSampleIndex = 0;
    uint32_t resolveView = 0;
    uint32_t reflectionDeclusterMode = 0;
    float reflectionBoost = 1.0f;
    float transmissionFloor = 0.0f;
    bool legacySidecarEncoding = false;
    bool distortionEnabled = true;
    bool nsightMarkers = false;
};

struct PathTraceUnifiedPtGlassOpticalInputs
{
    const PathTraceUnifiedPtDispatchInputs* dispatch = nullptr;
    nvrhi::BufferHandle opticalSurfaceBuffer;
    nvrhi::BufferHandle canonicalPrimaryReceiver32Buffer;
    nvrhi::TextureHandle skyEnvironment;
    nvrhi::TextureHandle rrGuideAlbedo;
    nvrhi::TextureHandle rrGuideSpecularAlbedo;
    nvrhi::TextureHandle rrGuideHitDistance;
    float reflectionComposeBoost = 1.0f;
    float skyBrightness = 1.0f;
    float rayTMax = 100000.0f;
};

class PathTraceUnifiedPtState
{
public:
    bool ExecuteInitial(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool ExecuteTemporal(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool ExecuteSpatial(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool ExecuteDuplication(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool ExecuteResolve(
        const PathTraceUnifiedPtDispatchInputs& inputs,
        uint32_t view);
    bool ExecuteGlassCompose(
        const PathTraceUnifiedPtGlassComposeInputs& inputs);
    bool ExecuteGlassOpticalTransport(
        const PathTraceUnifiedPtGlassOpticalInputs& inputs);
    nvrhi::TextureHandle GetGlassTransmissionTexture() const
    {
        return m_glassOpticalTransmission;
    }
    nvrhi::TextureHandle GetGlassReflectionTexture() const
    {
        return m_glassOpticalReflection;
    }
    nvrhi::TextureHandle GetGlassReflectionReuseTexture() const
    {
        return m_glassReflectionReuseReady && m_glassReflectionReuseState
            ? m_glassReflectionReuseState->GetResolveOutputTexture()
            : nullptr;
    }
    nvrhi::TextureHandle GetGlassRrGuideAlbedoTexture() const
    {
        return m_glassComposeRrAlbedo;
    }
    nvrhi::TextureHandle GetGlassRrGuideSpecularAlbedoTexture() const
    {
        return m_glassComposeRrSpecularAlbedo;
    }
    nvrhi::TextureHandle GetGlassRrGuideNormalRoughnessTexture() const
    {
        return m_glassComposeRrNormalRoughness;
    }
    nvrhi::TextureHandle GetGlassRrGuidePositionTexture() const
    {
        return m_glassComposeRrPosition;
    }
    nvrhi::TextureHandle GetGlassRrGuideDepthTexture() const
    {
        return m_glassComposeRrDepth;
    }
    nvrhi::TextureHandle GetGlassRrMotionVectorTexture() const
    {
        return m_glassComposeRrMotion;
    }
    void CompleteFrame();
    nvrhi::TextureHandle GetOutputTexture() const
    {
        return m_resolveReady
            ? (m_presentationOutput ? m_presentationOutput : m_resolveOutput)
            : nullptr;
    }
    nvrhi::TextureHandle GetResolveOutputTexture() const
    {
        return m_resolveReady ? m_resolveOutput : nullptr;
    }
    void SetPresentationOutput(nvrhi::TextureHandle output)
    {
        m_presentationOutput = m_resolveReady ? output : nullptr;
    }
    void ReleaseResolve();
    void Release();

private:
    bool EnsurePages(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsurePipeline(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureBindingSet(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureDiagnosticBuffers(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureCompactGeometryResources(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureCompactGeometryPipeline(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureCompactGeometryBindingSet(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool ExecuteCompactGeometryPack(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureCompactLightResources(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureCompactLightPipeline(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureCompactLightBindingSet(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool ExecuteCompactLightPack(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureEmissiveCompactBuffers(const PathTraceUnifiedPtDispatchInputs& inputs);
    void DrainEmissiveCompactReadback(const PathTraceUnifiedPtDispatchInputs& inputs);
    void ReleaseEmissiveCompact();
    bool EnsureX3SplitBuffers(const PathTraceUnifiedPtDispatchInputs& inputs);
    void ReleaseX3Split();
    bool EnsureLightTileResources(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureLightTilePipeline(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureLightTileBindingSet(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool ExecuteLightTilePresample(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureCompactMaterialResources(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureCompactMaterialPipeline(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureCompactMaterialBindingSet(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool ExecuteCompactMaterialPack(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureContinuationResources(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureContinuationPipeline(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureContinuationBindingSet(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool ExecuteContinuationTrace(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureResolveResources(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureResolvePipeline(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureResolveBindingSet(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureGlassComposePipeline(
        const PathTraceUnifiedPtGlassComposeInputs& inputs);
    bool EnsureGlassComposeResources(
        const PathTraceUnifiedPtGlassComposeInputs& inputs);
    bool EnsureGlassComposeBindingSet(
        const PathTraceUnifiedPtGlassComposeInputs& inputs);
    bool EnsureGlassOpticalResources(
        const PathTraceUnifiedPtGlassOpticalInputs& inputs);
    bool EnsureGlassOpticalPipeline(
        const PathTraceUnifiedPtGlassOpticalInputs& inputs);
    bool EnsureGlassOpticalBindingSet(
        const PathTraceUnifiedPtGlassOpticalInputs& inputs);
    void DrainGlassOpticalDiagnosticReadback(
        const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureTemporalPipeline(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureTemporalBindingSet(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureTemporalReplayCompactionBuffers(
        const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureTemporalBoilingFilterBindingSet(
        const PathTraceUnifiedPtDispatchInputs& inputs);
    void DrainTemporalReplayCompactionReadback(
        const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureTemporalBottleneckBuffer(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureTemporalDiagnosticBuffers(const PathTraceUnifiedPtDispatchInputs& inputs);
    void DrainTemporalDiagnosticReadback(const PathTraceUnifiedPtDispatchInputs& inputs);
    void DrainTemporalWorkBudgetReadback(const PathTraceUnifiedPtDispatchInputs& inputs);
    void UpdateTemporalGpuTiming(const PathTraceUnifiedPtDispatchInputs& inputs);
    void PollTemporalGpuTiming(const PathTraceUnifiedPtDispatchInputs& inputs);
    nvrhi::TimerQueryHandle BeginTemporalGpuTiming(
        const PathTraceUnifiedPtDispatchInputs& inputs,
        bool historyAvailable);
    bool EnsureDuplicationResources(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureDuplicationPipeline(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureDuplicationBindingSets(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureSpatialPipeline(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureSpatialReuseTextureResources(
        const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureSpatialBindingSet(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureSpatialBoostBindingSet(
        const PathTraceUnifiedPtDispatchInputs& inputs);
    void DrainSpatialBoostReadback(
        const PathTraceUnifiedPtDispatchInputs& inputs);
    void UpdateSpatialGpuTiming(const PathTraceUnifiedPtDispatchInputs& inputs);
    void PollSpatialGpuTiming(const PathTraceUnifiedPtDispatchInputs& inputs);
    nvrhi::TimerQueryHandle BeginSpatialGpuTiming(
        const PathTraceUnifiedPtDispatchInputs& inputs);
    void DrainDiagnosticReadback(const PathTraceUnifiedPtDispatchInputs& inputs);
    void ReportProofStage(
        uint32_t stage,
        const char* label,
        PathTraceUnifiedPtBackend backend,
        PathTraceUnifiedPtFamily family);
    void ReleasePipeline();
    void ReleaseCompactGeometry();
    void ReleaseCompactLights();
    void ReleaseLightTiles();
    void ReleaseCompactMaterials();
    void ReleaseContinuation();
    void ReleaseTemporal();
    void ReleaseDuplication();
    void ReleaseSpatial();
    nvrhi::BufferHandle CurrentPage() const;
    nvrhi::BufferHandle HistoryPage() const;
    PathTraceUnifiedPtPageMetadata& CurrentPageMetadata();
    const PathTraceUnifiedPtPageMetadata& CurrentPageMetadata() const;
    PathTraceUnifiedPtPageMetadata& HistoryPageMetadata();
    const PathTraceUnifiedPtPageMetadata& HistoryPageMetadata() const;

    PathTraceUnifiedPtBackend m_backend = PathTraceUnifiedPtBackend::RayQuery;
    PathTraceUnifiedPtFamily m_family = PathTraceUnifiedPtFamily::DirectOnly;
    uint32_t m_pipelineVariant = 0;
    bool m_selectionValid = false;
    bool m_diagnostics = false;
    uint32_t m_primaryReceiverMode = 0;
    bool m_compactGeometry = false;
    bool m_compactLights = false;
    bool m_compactMaterials = false;
    bool m_splitInitial = false;
    bool m_threeVertexInitial = false;
    bool m_threeVertexSplit = false;
    bool m_geometryNoSkinned = false;
    bool m_splitContinuation = false;
    bool m_lambertDiagnostic = false;
    bool m_staticAnalyticOnly = false;
    bool m_emissiveCompact = false;
    bool m_frozenStaticDiagnostic = false;
    bool m_frozenLightDiagnostic = false;
    bool m_directProposalParity = false;
    bool m_lightTiles = false;
    bool m_temporalModeActive = false;
    bool m_initialPublishedThisFrame = false;
    bool m_spatialModeActive = false;
    bool m_spatialExecutedThisFrame = false;
    int32_t m_reportedTemporalHistoryAvailable = -1;
    int32_t m_reportedTemporalSkipReason = -1;
    bool m_pipelineAttempted = false;
    bool m_resourceFailureLogged = false;
    bool m_page0NeedsAllocationClear = false;
    uint32_t m_reportedProofStage = UINT32_MAX;

    uint32_t m_pageWidth = 0;
    uint32_t m_pageHeight = 0;
    uint64_t m_pageBytes = 0;
    nvrhi::BufferHandle m_page0;
    nvrhi::BufferHandle m_page1;
    PathTraceUnifiedPtPageMetadata m_page0Metadata;
    PathTraceUnifiedPtPageMetadata m_page1Metadata;
    uint32_t m_currentPageIndex = 0;
    uint32_t m_historyPageIndex = 1;
    uint64_t m_observedHistoryEpoch = 0;
    uint64_t m_lastPublishedFrameSerial = 0;
    nvrhi::BufferHandle m_diagnosticCounters;
    nvrhi::BufferHandle m_diagnosticReadback;
    bool m_diagnosticReadbackPending = false;
    int m_diagnosticReadbackDelayFrames = 0;
    uint32_t m_diagnosticReadbackSampleIndex = 0;
    uint32_t m_diagnosticReadbackWidth = 0;
    uint32_t m_diagnosticReadbackHeight = 0;
    uint32_t m_diagnosticReadbackMaterialPolicyFlags = 0;
    bool m_diagnosticProbeFromHistory = false;
    uint64_t m_diagnosticProbeFrameSerial = 0;
    PathTraceUnifiedPtFamily m_diagnosticReadbackFamily =
        PathTraceUnifiedPtFamily::DirectOnly;

    nvrhi::BindingLayoutHandle m_bindingLayout;
    std::array<nvrhi::BindingSetHandle, 2> m_bindingSets;
    std::array<nvrhi::BindingSetDesc, 2> m_bindingSetDescs;
    std::array<bool, 2> m_bindingSetDescValid = { false, false };

    nvrhi::ShaderHandle m_computeShader;
    nvrhi::ComputePipelineHandle m_computePipeline;
    nvrhi::ShaderHandle m_splitIndirectComputeShader;
    nvrhi::ComputePipelineHandle m_splitIndirectComputePipeline;
    nvrhi::ShaderHandle m_emissiveCompactConsumeShader;
    nvrhi::ComputePipelineHandle m_emissiveCompactConsumePipeline;
    nvrhi::ShaderHandle m_x3ConsumeShader;
    nvrhi::ComputePipelineHandle m_x3ConsumePipeline;
    uint32_t m_x3Capacity = 0u;
    uint32_t m_x3TokenStride = 0u;
    nvrhi::BufferHandle m_x3Queue;
    nvrhi::BufferHandle m_x3Meta;
    nvrhi::BufferHandle m_x3DispatchArgs;
    uint32_t m_emissiveCompactCapacity = 0u;
    nvrhi::BufferHandle m_emissiveCompactQueue;
    nvrhi::BufferHandle m_emissiveCompactMeta;
    nvrhi::BufferHandle m_emissiveCompactDispatchArgs;
    nvrhi::BufferHandle m_emissiveCompactReadback;
    bool m_emissiveCompactReadbackPending = false;
    int m_emissiveCompactReadbackDelayFrames = 0;

    uint32_t m_continuationCapacity = 0;
    nvrhi::BufferHandle m_continuationHits;
    nvrhi::BindingLayoutHandle m_continuationBindingLayout;
    nvrhi::BindingSetHandle m_continuationBindingSet;
    nvrhi::BindingSetDesc m_continuationBindingSetDesc;
    bool m_continuationBindingSetDescValid = false;
    nvrhi::ShaderHandle m_continuationShader;
    nvrhi::ComputePipelineHandle m_continuationPipeline;
    bool m_continuationPipelineAttempted = false;

    uint32_t m_compactStaticVertexCapacity = 0;
    uint32_t m_compactDynamicVertexCapacity = 0;
    uint32_t m_compactRigidVertexCapacity = 0;
    uint32_t m_compactSkinnedVertexCapacity = 0;
    nvrhi::BufferHandle m_compactStaticVertices;
    nvrhi::BufferHandle m_compactDynamicVertices;
    nvrhi::BufferHandle m_compactRigidVertices;
    nvrhi::BufferHandle m_compactSkinnedVertices;
    nvrhi::BindingLayoutHandle m_compactGeometryBindingLayout;
    nvrhi::BindingSetHandle m_compactGeometryBindingSet;
    nvrhi::BindingSetDesc m_compactGeometryBindingSetDesc;
    bool m_compactGeometryBindingSetDescValid = false;
    nvrhi::ShaderHandle m_compactGeometryShader;
    nvrhi::ComputePipelineHandle m_compactGeometryPipeline;
    bool m_compactGeometryPipelineAttempted = false;

    uint32_t m_compactLightCapacity = 0;
    nvrhi::BufferHandle m_compactLightsBuffer;
    nvrhi::BufferHandle m_resolvedEmissiveBuffer;
    nvrhi::BindingLayoutHandle m_compactLightBindingLayout;
    nvrhi::BindingSetHandle m_compactLightBindingSet;
    nvrhi::BindingSetDesc m_compactLightBindingSetDesc;
    bool m_compactLightBindingSetDescValid = false;
    nvrhi::ShaderHandle m_compactLightShader;
    nvrhi::ComputePipelineHandle m_compactLightPipeline;
    bool m_compactLightPipelineAttempted = false;

    nvrhi::BufferHandle m_lightTileBuffer;
    nvrhi::BindingLayoutHandle m_lightTileBindingLayout;
    nvrhi::BindingSetHandle m_lightTileBindingSet;
    nvrhi::BindingSetDesc m_lightTileBindingSetDesc;
    bool m_lightTileBindingSetDescValid = false;
    nvrhi::ShaderHandle m_lightTileShader;
    nvrhi::ComputePipelineHandle m_lightTilePipeline;
    bool m_lightTilePipelineAttempted = false;

    uint32_t m_compactMaterialCapacity = 0;
    nvrhi::BufferHandle m_compactMaterialsBuffer;
    nvrhi::BindingLayoutHandle m_compactMaterialBindingLayout;
    nvrhi::BindingSetHandle m_compactMaterialBindingSet;
    nvrhi::BindingSetDesc m_compactMaterialBindingSetDesc;
    bool m_compactMaterialBindingSetDescValid = false;
    nvrhi::ShaderHandle m_compactMaterialShader;
    nvrhi::ComputePipelineHandle m_compactMaterialPipeline;
    bool m_compactMaterialPipelineAttempted = false;

    nvrhi::ShaderLibraryHandle m_rayGenerationLibrary;
    nvrhi::ShaderLibraryHandle m_missLibrary;
    nvrhi::ShaderLibraryHandle m_closestHitLibrary;
    nvrhi::rt::PipelineHandle m_rayPipeline;
    nvrhi::rt::ShaderTableHandle m_shaderTable;

    bool m_temporalCompactLights = false;
    bool m_temporalDuplication = false;
    bool m_temporalIndirect = false;
    bool m_temporalSharedReuseAdapter = false;
    bool m_temporalCommonGrisMerge = false;
    bool m_temporalThreeVertexReplay = false;
    bool m_temporalReplayCompaction = false;
    bool m_temporalBoilingFilter = false;
    bool m_temporalEarlyReconnect = false;
    bool m_temporalRouteDiagnostics = false;
    bool m_temporalLambertDiagnostic = false;
    bool m_temporalFrozenStaticDiagnostic = false;
    bool m_temporalFrozenLightDiagnostic = false;
    bool m_temporalReflectionReuseDomain = false;
    uint32_t m_temporalBottleneckProbe = 0;
    bool m_temporalPipelineAttempted = false;
    nvrhi::BindingLayoutHandle m_temporalBindingLayout;
    std::array<nvrhi::BindingSetHandle, 2> m_temporalBindingSets;
    std::array<nvrhi::BindingSetDesc, 2> m_temporalBindingSetDescs;
    std::array<bool, 2> m_temporalBindingSetDescValid = { false, false };
    nvrhi::BindingLayoutHandle m_temporalReplayBindingLayout;
    std::array<nvrhi::BindingSetHandle, 2> m_temporalReplayBindingSets;
    std::array<nvrhi::BindingSetDesc, 2> m_temporalReplayBindingSetDescs;
    std::array<bool, 2> m_temporalReplayBindingSetDescValid = { false, false };
    nvrhi::ShaderHandle m_temporalShader;
    nvrhi::ComputePipelineHandle m_temporalPipeline;
    nvrhi::ShaderHandle m_temporalReplayShader;
    nvrhi::ComputePipelineHandle m_temporalReplayPipeline;
    nvrhi::BindingLayoutHandle m_temporalBoilingFilterBindingLayout;
    std::array<nvrhi::BindingSetHandle, 2>
        m_temporalBoilingFilterBindingSets;
    nvrhi::ShaderHandle m_temporalBoilingFilterShader;
    nvrhi::ComputePipelineHandle m_temporalBoilingFilterPipeline;
    uint32_t m_temporalReplayCapacity = 0u;
    nvrhi::BufferHandle m_temporalReplayQueue;
    nvrhi::BufferHandle m_temporalReplayMeta;
    nvrhi::BufferHandle m_temporalReplayDispatchArgs;
    nvrhi::BufferHandle m_temporalReplayReadback;
    bool m_temporalReplayReadbackPending = false;
    bool m_temporalReplayDiagnosticCaptured = false;
    int m_temporalReplayReadbackDelayFrames = 0;
    nvrhi::BufferHandle m_temporalDiagnosticCounters;
    nvrhi::BufferHandle m_temporalDiagnosticReadback;
    uint32_t m_temporalBottleneckCapacity = 0;
    nvrhi::BufferHandle m_temporalBottleneckBuffer;
    nvrhi::BufferHandle m_temporalWorkBudgetReadback;
    bool m_temporalWorkBudgetReadbackPending = false;
    bool m_temporalWorkBudgetCaptureArmed = false;
    uint32_t m_temporalWorkBudgetWarmupFrames = 0u;
    int m_temporalWorkBudgetReadbackDelayFrames = 0;
    bool m_temporalDiagnosticReadbackPending = false;
    bool m_temporalDiagnosticReadbackIsRoute = false;
    int m_temporalDiagnosticReadbackDelayFrames = 0;

    static constexpr uint32_t TEMPORAL_GPU_TIMER_SLOT_COUNT = 8u;
    static constexpr uint32_t TEMPORAL_GPU_TIMING_WARMUP_FRAMES = 16u;
    static constexpr uint32_t TEMPORAL_GPU_TIMING_SAMPLE_COUNT = 64u;
    struct TemporalGpuTimerSlot
    {
        nvrhi::TimerQueryHandle query;
        bool pending = false;
        bool collect = false;
        uint32_t probeMode = UINT32_MAX;
        uint32_t sampleIndex = UINT32_MAX;
        int earliestPollFrame = 0;
    };
    std::array<TemporalGpuTimerSlot, TEMPORAL_GPU_TIMER_SLOT_COUNT>
        m_temporalGpuTimers;
    std::array<double, TEMPORAL_GPU_TIMING_SAMPLE_COUNT>
        m_temporalGpuTimingSamples = {};
    uint32_t m_temporalGpuTimerCursor = 0u;
    uint32_t m_temporalGpuTimingMode = UINT32_MAX;
    uint32_t m_temporalGpuTimingWarmupRemaining = 0u;
    uint32_t m_temporalGpuTimingSubmitted = 0u;
    uint32_t m_temporalGpuTimingCompleted = 0u;
    bool m_temporalGpuTimingBatchComplete = false;
    bool m_temporalGpuTimingQueryFailureLogged = false;

    uint32_t m_duplicationWidth = 0;
    uint32_t m_duplicationHeight = 0;
    uint32_t m_duplicationPackedRowPitch = 0;
    nvrhi::BufferHandle m_duplicationSampleIds;
    std::array<nvrhi::BufferHandle, 2> m_duplicationScores;
    std::array<PathTraceUnifiedPtPageMetadata, 2> m_duplicationMetadata;
    nvrhi::BindingLayoutHandle m_duplicationBindingLayout;
    std::array<nvrhi::BindingSetHandle, 2> m_duplicationFillBindingSets;
    std::array<nvrhi::BindingSetHandle, 2> m_duplicationComputeBindingSets;
    nvrhi::ShaderHandle m_duplicationFillShader;
    nvrhi::ShaderHandle m_duplicationComputeShader;
    nvrhi::ComputePipelineHandle m_duplicationFillPipeline;
    nvrhi::ComputePipelineHandle m_duplicationComputePipeline;
    bool m_duplicationPipelineAttempted = false;

    bool m_spatialCompactLights = false;
    bool m_spatialSharedReuse = false;
    bool m_spatialStoredSourceTarget = false;
    bool m_spatialWorkgroupPairing = false;
    bool m_spatialEmptyRescue = false;
    bool m_spatialMultiNeighbor = false;
    bool m_spatialDisocclusionBoost = false;
    bool m_spatialReuseTexturePairing = false;
    bool m_spatialShiftPrepass = false;
    bool m_spatialThreeVertexReplay = false;
    bool m_spatialLambertDiagnostic = false;
    bool m_spatialReflectionReuseDomain = false;
    bool m_spatialPipelineAttempted = false;
    nvrhi::BufferHandle m_spatialReuseTextureBuffer;
    nvrhi::BufferHandle m_spatialShiftBuffer;
    uint32_t m_spatialShiftCapacity = 0u;
    nvrhi::BindingLayoutHandle m_spatialBindingLayout;
    std::array<nvrhi::BindingSetHandle, 2> m_spatialBindingSets;
    std::array<nvrhi::BindingSetDesc, 2> m_spatialBindingSetDescs;
    std::array<bool, 2> m_spatialBindingSetDescValid = { false, false };
    nvrhi::ShaderHandle m_spatialShader;
    nvrhi::ComputePipelineHandle m_spatialPipeline;
    nvrhi::ShaderHandle m_spatialShiftShader;
    nvrhi::ComputePipelineHandle m_spatialShiftPipeline;
    nvrhi::BindingLayoutHandle m_spatialBoostBindingLayout;
    std::array<nvrhi::BindingSetHandle, 2> m_spatialBoostBindingSets;
    std::array<nvrhi::BindingSetDesc, 2> m_spatialBoostBindingSetDescs;
    std::array<bool, 2> m_spatialBoostBindingSetDescValid = { false, false };
    nvrhi::ShaderHandle m_spatialBoostShader;
    nvrhi::ComputePipelineHandle m_spatialBoostPipeline;
    nvrhi::BufferHandle m_spatialBoostReadback;
    bool m_spatialBoostReadbackPending = false;
    bool m_spatialBoostDiagnosticCaptured = false;
    int m_spatialBoostReadbackDelayFrames = 0;

    static constexpr uint32_t SPATIAL_GPU_TIMER_SLOT_COUNT = 8u;
    static constexpr uint32_t SPATIAL_GPU_TIMING_WARMUP_FRAMES = 16u;
    static constexpr uint32_t SPATIAL_GPU_TIMING_SAMPLE_COUNT = 64u;
    struct SpatialGpuTimerSlot
    {
        nvrhi::TimerQueryHandle query;
        bool pending = false;
        bool collect = false;
        uint32_t mode = UINT32_MAX;
        uint32_t sampleIndex = UINT32_MAX;
        int earliestPollFrame = 0;
    };
    std::array<SpatialGpuTimerSlot, SPATIAL_GPU_TIMER_SLOT_COUNT>
        m_spatialGpuTimers;
    std::array<double, SPATIAL_GPU_TIMING_SAMPLE_COUNT>
        m_spatialGpuTimingSamples = {};
    uint32_t m_spatialGpuTimerCursor = 0u;
    uint32_t m_spatialGpuTimingMode = UINT32_MAX;
    uint32_t m_spatialGpuTimingWarmupRemaining = 0u;
    uint32_t m_spatialGpuTimingSubmitted = 0u;
    uint32_t m_spatialGpuTimingCompleted = 0u;
    bool m_spatialGpuTimingBatchComplete = false;
    bool m_spatialGpuTimingQueryFailureLogged = false;

    uint32_t m_resolveWidth = 0;
    uint32_t m_resolveHeight = 0;
    uint32_t m_resolvePrimaryReceiverMode = UINT32_MAX;
    bool m_resolvePipelineAttempted = false;
    bool m_resolveFailureLogged = false;
    bool m_resolveReady = false;
    nvrhi::TextureHandle m_resolveOutput;
    // Optional output-resolution result owned by an external post-process
    // (currently DLSS Ray Reconstruction). Cleared at every new UPT frame so
    // an evaluation failure cannot expose a stale reconstructed image.
    nvrhi::TextureHandle m_presentationOutput;
    nvrhi::BindingLayoutHandle m_resolveBindingLayout;
    std::array<nvrhi::BindingSetHandle, 2> m_resolveBindingSets;
    std::array<nvrhi::BindingSetDesc, 2> m_resolveBindingSetDescs;
    std::array<bool, 2> m_resolveBindingSetDescValid = { false, false };
    nvrhi::ShaderHandle m_resolveShader;
    nvrhi::ComputePipelineHandle m_resolvePipeline;
    bool m_glassComposePipelineAttempted = false;
    nvrhi::BindingLayoutHandle m_glassComposeBindingLayout;
    nvrhi::BindingSetHandle m_glassComposeBindingSet;
    nvrhi::BindingSetDesc m_glassComposeBindingSetDesc;
    bool m_glassComposeBindingSetDescValid = false;
    nvrhi::ShaderHandle m_glassComposeShader;
    nvrhi::ComputePipelineHandle m_glassComposePipeline;
    nvrhi::TextureHandle m_glassComposeRrAlbedo;
    nvrhi::TextureHandle m_glassComposeRrSpecularAlbedo;
    nvrhi::TextureHandle m_glassComposeRrNormalRoughness;
    nvrhi::TextureHandle m_glassComposeRrPosition;
    nvrhi::TextureHandle m_glassComposeRrDepth;
    nvrhi::TextureHandle m_glassComposeRrMotion;
    uint32_t m_glassComposeWidth = 0;
    uint32_t m_glassComposeHeight = 0;
    bool m_glassOpticalPipelineAttempted = false;
    bool m_glassOpticalCompactLights = false;
    nvrhi::BindingLayoutHandle m_glassOpticalBindingLayout;
    nvrhi::BindingSetHandle m_glassOpticalBindingSet;
    nvrhi::BindingSetDesc m_glassOpticalBindingSetDesc;
    bool m_glassOpticalBindingSetDescValid = false;
    nvrhi::ShaderHandle m_glassOpticalShader;
    nvrhi::ComputePipelineHandle m_glassOpticalPipeline;
    nvrhi::BufferHandle m_glassOpticalConstants;
    nvrhi::BufferHandle m_glassOpticalDiagnosticBuffer;
    nvrhi::BufferHandle m_glassOpticalDiagnosticReadback;
    bool m_glassOpticalDiagnosticReadbackPending = false;
    int m_glassOpticalDiagnosticReadbackDelayFrames = 0;
    nvrhi::TextureHandle m_glassOpticalTransmission;
    nvrhi::TextureHandle m_glassOpticalReflection;
    std::array<nvrhi::BufferHandle, 2> m_glassReflectionReceivers;
    std::array<nvrhi::BufferHandle, 2> m_glassReflectionSidecars;
    nvrhi::TextureHandle m_glassReflectionRrSpecular;
    std::unique_ptr<PathTraceUnifiedPtState> m_glassReflectionReuseState;
    uint32_t m_glassReflectionReceiverCurrentIndex = 0u;
    uint32_t m_glassReflectionWidth = 0u;
    uint32_t m_glassReflectionHeight = 0u;
    uint64_t m_glassReflectionHistoryEpoch = 0u;
    bool m_glassReflectionHistoryValid = false;
    bool m_glassReflectionReuseReady = false;
    uint32_t m_glassOpticalWidth = 0;
    uint32_t m_glassOpticalHeight = 0;
};
