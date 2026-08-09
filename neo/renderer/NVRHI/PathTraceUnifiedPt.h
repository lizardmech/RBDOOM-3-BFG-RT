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
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frameSampleIndex = 0;
    float emissiveScale = 1.0f;
    uint32_t materialPolicyFlags = 0;
    uint32_t proofStage = 1;
    uint32_t shaderProofMode = 1;
    PathTraceUnifiedPtBackend backend = PathTraceUnifiedPtBackend::RayQuery;
    PathTraceUnifiedPtFamily family = PathTraceUnifiedPtFamily::DirectOnly;
    bool nsightMarkers = false;
    bool diagnostics = false;
    uint32_t primaryReceiverMode = 0;
    bool compactPrimaryHistory = false;
    bool compactGeometry = false;
    bool compactLights = false;
    bool compactMaterials = false;
    bool splitInitial = false;
    bool splitContinuation = false;
    bool directProposalParity = false;
    bool lightTiles = false;
    bool temporal = false;
    bool duplication = false;
    bool spatial = false;
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
    void CompleteFrame();
    nvrhi::TextureHandle GetOutputTexture() const
    {
        return m_resolveReady ? m_resolveOutput : nullptr;
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
    bool EnsureTemporalPipeline(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureTemporalBindingSet(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureTemporalDiagnosticBuffers(const PathTraceUnifiedPtDispatchInputs& inputs);
    void DrainTemporalDiagnosticReadback(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureDuplicationResources(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureDuplicationPipeline(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureDuplicationBindingSets(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureSpatialPipeline(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureSpatialBindingSet(const PathTraceUnifiedPtDispatchInputs& inputs);
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
    bool m_splitContinuation = false;
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
    bool m_temporalEarlyReconnect = false;
    bool m_temporalPipelineAttempted = false;
    nvrhi::BindingLayoutHandle m_temporalBindingLayout;
    std::array<nvrhi::BindingSetHandle, 2> m_temporalBindingSets;
    std::array<nvrhi::BindingSetDesc, 2> m_temporalBindingSetDescs;
    std::array<bool, 2> m_temporalBindingSetDescValid = { false, false };
    nvrhi::ShaderHandle m_temporalShader;
    nvrhi::ComputePipelineHandle m_temporalPipeline;
    nvrhi::BufferHandle m_temporalDiagnosticCounters;
    nvrhi::BufferHandle m_temporalDiagnosticReadback;
    bool m_temporalDiagnosticReadbackPending = false;
    int m_temporalDiagnosticReadbackDelayFrames = 0;

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
    bool m_spatialPipelineAttempted = false;
    nvrhi::BindingLayoutHandle m_spatialBindingLayout;
    std::array<nvrhi::BindingSetHandle, 2> m_spatialBindingSets;
    std::array<nvrhi::BindingSetDesc, 2> m_spatialBindingSetDescs;
    std::array<bool, 2> m_spatialBindingSetDescValid = { false, false };
    nvrhi::ShaderHandle m_spatialShader;
    nvrhi::ComputePipelineHandle m_spatialPipeline;

    uint32_t m_resolveWidth = 0;
    uint32_t m_resolveHeight = 0;
    uint32_t m_resolvePrimaryReceiverMode = UINT32_MAX;
    bool m_resolvePipelineAttempted = false;
    bool m_resolveFailureLogged = false;
    bool m_resolveReady = false;
    nvrhi::TextureHandle m_resolveOutput;
    nvrhi::BindingLayoutHandle m_resolveBindingLayout;
    std::array<nvrhi::BindingSetHandle, 2> m_resolveBindingSets;
    std::array<nvrhi::BindingSetDesc, 2> m_resolveBindingSetDescs;
    std::array<bool, 2> m_resolveBindingSetDescValid = { false, false };
    nvrhi::ShaderHandle m_resolveShader;
    nvrhi::ComputePipelineHandle m_resolvePipeline;
};
