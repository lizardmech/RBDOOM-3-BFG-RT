#pragma once

// Isolated runtime owner for the clean Slang unified ReSTIR PT lane.
//
// UPT-04 owns exactly one 64-byte reservoir page and exactly one selected
// initial-sampling pipeline. It deliberately does not own output, history,
// temporal/spatial reuse, counters, queues, or any legacy smoke constants.

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

struct PathTraceUnifiedPtDispatchInputs
{
    nvrhi::IDevice* device = nullptr;
    nvrhi::ICommandList* commandList = nullptr;
    const RtPathTraceSceneInputs* sceneInputs = nullptr;
    nvrhi::BufferHandle primarySurfaceBuffer;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t frameSampleIndex = 0;
    uint32_t proofStage = 1;
    uint32_t shaderProofMode = 1;
    PathTraceUnifiedPtBackend backend = PathTraceUnifiedPtBackend::RayQuery;
    PathTraceUnifiedPtFamily family = PathTraceUnifiedPtFamily::DirectOnly;
    bool nsightMarkers = false;
};

class PathTraceUnifiedPtState
{
public:
    bool ExecuteInitial(const PathTraceUnifiedPtDispatchInputs& inputs);
    void Release();

private:
    bool EnsurePage(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsurePipeline(const PathTraceUnifiedPtDispatchInputs& inputs);
    bool EnsureBindingSet(const PathTraceUnifiedPtDispatchInputs& inputs);
    void ReportProofStage(
        uint32_t stage,
        const char* label,
        PathTraceUnifiedPtBackend backend,
        PathTraceUnifiedPtFamily family);
    void ReleasePipeline();

    PathTraceUnifiedPtBackend m_backend = PathTraceUnifiedPtBackend::RayQuery;
    PathTraceUnifiedPtFamily m_family = PathTraceUnifiedPtFamily::DirectOnly;
    uint32_t m_pipelineVariant = 0;
    bool m_selectionValid = false;
    bool m_pipelineAttempted = false;
    bool m_resourceFailureLogged = false;
    bool m_pageNeedsClear = false;
    uint32_t m_reportedProofStage = UINT32_MAX;

    uint32_t m_pageWidth = 0;
    uint32_t m_pageHeight = 0;
    uint64_t m_pageBytes = 0;
    nvrhi::BufferHandle m_page0;

    nvrhi::BindingLayoutHandle m_bindingLayout;
    nvrhi::BindingSetHandle m_bindingSet;
    nvrhi::BindingSetDesc m_bindingSetDesc;
    bool m_bindingSetDescValid = false;

    nvrhi::ShaderHandle m_computeShader;
    nvrhi::ComputePipelineHandle m_computePipeline;

    nvrhi::ShaderLibraryHandle m_rayGenerationLibrary;
    nvrhi::ShaderLibraryHandle m_missLibrary;
    nvrhi::ShaderLibraryHandle m_closestHitLibrary;
    nvrhi::rt::PipelineHandle m_rayPipeline;
    nvrhi::rt::ShaderTableHandle m_shaderTable;
};
