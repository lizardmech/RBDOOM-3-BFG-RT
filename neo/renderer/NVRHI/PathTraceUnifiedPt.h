#pragma once

// Isolated runtime owner for the clean Slang unified ReSTIR PT lane.
//
// UPT-04 owns exactly one 64-byte reservoir page and exactly one selected
// initial-sampling pipeline. UPT-05 adds one trace-free resolve pipeline and
// one RGBA16F output. Neither stage owns history, temporal/spatial reuse,
// queues, or any legacy smoke constants. A separate one-shot diagnostic
// specialization owns one fixed counter/readback pair.

#include "PathTraceSceneInputs.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>

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
    PATH_TRACE_UPT_MATERIAL_LEGACY_SPECMAP_TO_PBR = 1u << 1u
};

struct PathTraceUnifiedPtDispatchInputs
{
    nvrhi::IDevice* device = nullptr;
    nvrhi::ICommandList* commandList = nullptr;
    const RtPathTraceSceneInputs* sceneInputs = nullptr;
    nvrhi::BufferHandle primarySurfaceBuffer;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frameSampleIndex = 0;
    uint32_t materialPolicyFlags = 0;
    uint32_t proofStage = 1;
    uint32_t shaderProofMode = 1;
    PathTraceUnifiedPtBackend backend = PathTraceUnifiedPtBackend::RayQuery;
    PathTraceUnifiedPtFamily family = PathTraceUnifiedPtFamily::DirectOnly;
    bool nsightMarkers = false;
    bool diagnostics = false;
    uint32_t primaryReceiverMode = 0;
    bool compactGeometry = false;
    float primaryCameraOrigin[3] = {};
};

class PathTraceUnifiedPtState
{
public:
    bool ExecuteInitial(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool ExecuteResolve(
        const PathTraceUnifiedPtDispatchInputs& inputs,
        uint32_t view);
    nvrhi::TextureHandle GetOutputTexture() const
    {
        return m_resolveReady ? m_resolveOutput : nullptr;
    }
    void ReleaseResolve();
    void Release();

private:
    bool EnsurePage(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsurePipeline(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureBindingSet(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureDiagnosticBuffers(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureCompactGeometryResources(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureCompactGeometryPipeline(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureCompactGeometryBindingSet(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool ExecuteCompactGeometryPack(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureResolveResources(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureResolvePipeline(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureResolveBindingSet(const PathTraceUnifiedPtDispatchInputs& inputs);
    void DrainDiagnosticReadback(const PathTraceUnifiedPtDispatchInputs& inputs);
    void ReportProofStage(
        uint32_t stage,
        const char* label,
        PathTraceUnifiedPtBackend backend,
        PathTraceUnifiedPtFamily family);
    void ReleasePipeline();
    void ReleaseCompactGeometry();

    PathTraceUnifiedPtBackend m_backend = PathTraceUnifiedPtBackend::RayQuery;
    PathTraceUnifiedPtFamily m_family = PathTraceUnifiedPtFamily::DirectOnly;
    uint32_t m_pipelineVariant = 0;
    bool m_selectionValid = false;
    bool m_diagnostics = false;
    uint32_t m_primaryReceiverMode = 0;
    bool m_compactGeometry = false;
    bool m_pipelineAttempted = false;
    bool m_resourceFailureLogged = false;
    bool m_pageNeedsClear = false;
    uint32_t m_reportedProofStage = UINT32_MAX;

    uint32_t m_pageWidth = 0;
    uint32_t m_pageHeight = 0;
    uint64_t m_pageBytes = 0;
    nvrhi::BufferHandle m_page0;
    nvrhi::BufferHandle m_diagnosticCounters;
    nvrhi::BufferHandle m_diagnosticReadback;
    bool m_diagnosticReadbackPending = false;
    int m_diagnosticReadbackDelayFrames = 0;
    uint32_t m_diagnosticReadbackSampleIndex = 0;
    uint32_t m_diagnosticReadbackWidth = 0;
    uint32_t m_diagnosticReadbackHeight = 0;
    PathTraceUnifiedPtFamily m_diagnosticReadbackFamily =
        PathTraceUnifiedPtFamily::DirectOnly;

    nvrhi::BindingLayoutHandle m_bindingLayout;
    nvrhi::BindingSetHandle m_bindingSet;
    nvrhi::BindingSetDesc m_bindingSetDesc;
    bool m_bindingSetDescValid = false;

    nvrhi::ShaderHandle m_computeShader;
    nvrhi::ComputePipelineHandle m_computePipeline;

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

    nvrhi::ShaderLibraryHandle m_rayGenerationLibrary;
    nvrhi::ShaderLibraryHandle m_missLibrary;
    nvrhi::ShaderLibraryHandle m_closestHitLibrary;
    nvrhi::rt::PipelineHandle m_rayPipeline;
    nvrhi::rt::ShaderTableHandle m_shaderTable;

    uint32_t m_resolveWidth = 0;
    uint32_t m_resolveHeight = 0;
    bool m_resolvePipelineAttempted = false;
    bool m_resolveFailureLogged = false;
    bool m_resolveReady = false;
    nvrhi::TextureHandle m_resolveOutput;
    nvrhi::BindingLayoutHandle m_resolveBindingLayout;
    nvrhi::BindingSetHandle m_resolveBindingSet;
    nvrhi::BindingSetDesc m_resolveBindingSetDesc;
    bool m_resolveBindingSetDescValid = false;
    nvrhi::ShaderHandle m_resolveShader;
    nvrhi::ComputePipelineHandle m_resolvePipeline;
};
