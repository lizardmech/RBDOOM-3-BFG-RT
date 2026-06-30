#pragma once

// CPU-side ReSTIR PT pass plan.
//
// This is the first split boundary in front of the legacy monolithic smoke
// raygen. It names which ReSTIR PT stages a debug mode is exercising and owns
// the RTXDI context resampling-mode decision. The current shader still executes
// the old combined validation paths; later slices can replace individual plan
// stages with separate dispatches without changing mode policy again.

#include "PathTraceRestirPT.h"
#include "PathTraceMaterialFeaturePasses.h"

#include <cstdint>

enum class RtPathTraceRestirPassKind : uint8_t
{
    Disabled = 0,
    InitialReservoir,
    TemporalReservoir,
    SpatialReservoir,
    ReservoirShading,
    DebugVisualize
};

enum RtPathTraceRestirPassFlags : uint32_t
{
    RT_RESTIR_PASS_NONE = 0,
    RT_RESTIR_PASS_WRITES_INITIAL = 1u << 0,
    RT_RESTIR_PASS_WRITES_TEMPORAL = 1u << 1,
    RT_RESTIR_PASS_WRITES_SPATIAL = 1u << 2,
    RT_RESTIR_PASS_CONSUMES_CURRENT_SURFACE = 1u << 3,
    RT_RESTIR_PASS_CONSUMES_PREVIOUS_SURFACE = 1u << 4,
    RT_RESTIR_PASS_CONSUMES_PREVIOUS_RESERVOIR = 1u << 5,
    RT_RESTIR_PASS_SHADES_RESERVOIR = 1u << 6,
    RT_RESTIR_PASS_TRACES_VISIBILITY = 1u << 7,
    RT_RESTIR_PASS_DEBUG_VISUALIZE = 1u << 8,
    RT_RESTIR_PASS_SOURCE_ATTRIBUTION = 1u << 9,
    RT_RESTIR_PASS_PRIMARY_SURFACE_DEBUG = 1u << 10,
    RT_RESTIR_PASS_PREVIEW_SAFETY_CAP = 1u << 11,
    RT_RESTIR_PASS_REQUIRES_TEMPORAL_PREPASS = 1u << 12,
    RT_RESTIR_PASS_REQUIRES_SPATIAL_PREPASS = 1u << 13,
    RT_RESTIR_PASS_REQUIRES_INITIAL_PREPASS = 1u << 14
};

struct RtPathTraceRestirPassPlan
{
    int debugMode = 0;
    bool restirDebugMode = false;
    RtPathTraceRestirPassKind producer = RtPathTraceRestirPassKind::Disabled;
    RtPathTraceRestirPassKind output = RtPathTraceRestirPassKind::Disabled;
    RtRestirPTResamplingMode resamplingMode = RtRestirPTResamplingMode::None;
    uint32_t flags = RT_RESTIR_PASS_NONE;
    const char* label = "disabled";
};

struct RtPathTraceRestirPassBufferSelection
{
    uint32_t initialOutput = 0;
    uint32_t temporalInput = 0;
    uint32_t temporalOutput = 0;
    uint32_t spatialInput = 0;
    uint32_t spatialOutput = 0;
    uint32_t finalShadingInput = 0;
    uint32_t debugInput = 0;
};

inline RtPathTraceMaterialFeaturePassDesc BuildPathTraceRestirFeaturePassDesc(const RtPathTraceRestirPassPlan& plan)
{
    RtPathTraceMaterialFeaturePassDesc desc;
    desc.enabled = plan.restirDebugMode;
    desc.debugLabel = plan.label;

    switch (plan.producer)
    {
    case RtPathTraceRestirPassKind::InitialReservoir:
        desc.kind = RtPathTraceMaterialFeaturePassKind::DirectReservoirInitial;
        desc.shaderTable = RtPathTraceMaterialFeatureShaderTable::RestirInitial;
        desc.materialCapsConsumed = RT_PATH_TRACE_MATERIAL_CAP_OPAQUE_DIRECT;
        desc.materialPassSupport = RT_PATH_TRACE_MATERIAL_PASS_DIRECT_RESERVOIR;
        desc.resourceInputs =
            RT_MATERIAL_FEATURE_RESOURCE_CURRENT_PRIMARY_SURFACE |
            RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_TABLE;
        desc.resourceOutputs = RT_MATERIAL_FEATURE_RESOURCE_CURRENT_DIRECT_RESERVOIR;
        break;
    case RtPathTraceRestirPassKind::TemporalReservoir:
        desc.kind = RtPathTraceMaterialFeaturePassKind::DirectReservoirTemporal;
        desc.shaderTable = RtPathTraceMaterialFeatureShaderTable::RestirTemporal;
        desc.materialCapsConsumed = RT_PATH_TRACE_MATERIAL_CAP_OPAQUE_DIRECT;
        desc.materialPassSupport = RT_PATH_TRACE_MATERIAL_PASS_DIRECT_RESERVOIR;
        desc.resourceInputs =
            RT_MATERIAL_FEATURE_RESOURCE_CURRENT_PRIMARY_SURFACE |
            RT_MATERIAL_FEATURE_RESOURCE_PREVIOUS_PRIMARY_SURFACE |
            RT_MATERIAL_FEATURE_RESOURCE_CURRENT_DIRECT_RESERVOIR |
            RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_TABLE;
        desc.resourceOutputs = RT_MATERIAL_FEATURE_RESOURCE_TEMPORAL_DIRECT_RESERVOIR;
        break;
    case RtPathTraceRestirPassKind::SpatialReservoir:
        desc.kind = RtPathTraceMaterialFeaturePassKind::DirectReservoirSpatial;
        desc.shaderTable = RtPathTraceMaterialFeatureShaderTable::RestirSpatialReservoir;
        desc.materialCapsConsumed = RT_PATH_TRACE_MATERIAL_CAP_OPAQUE_DIRECT;
        desc.materialPassSupport = RT_PATH_TRACE_MATERIAL_PASS_DIRECT_RESERVOIR;
        desc.resourceInputs =
            RT_MATERIAL_FEATURE_RESOURCE_CURRENT_PRIMARY_SURFACE |
            RT_MATERIAL_FEATURE_RESOURCE_PREVIOUS_PRIMARY_SURFACE |
            RT_MATERIAL_FEATURE_RESOURCE_TEMPORAL_DIRECT_RESERVOIR |
            RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_TABLE;
        desc.resourceOutputs = RT_MATERIAL_FEATURE_RESOURCE_SPATIAL_DIRECT_RESERVOIR;
        break;
    case RtPathTraceRestirPassKind::DebugVisualize:
        desc.kind = RtPathTraceMaterialFeaturePassKind::DebugVisualize;
        desc.shaderTable = RtPathTraceMaterialFeatureShaderTable::CorePathTrace;
        desc.materialPassSupport = RT_PATH_TRACE_MATERIAL_PASS_DEBUG_VISUALIZER;
        desc.resourceInputs = RT_MATERIAL_FEATURE_RESOURCE_CURRENT_PRIMARY_SURFACE;
        desc.resourceOutputs = RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR;
        break;
    default:
        desc.enabled = false;
        break;
    }

    if (plan.output == RtPathTraceRestirPassKind::ReservoirShading)
    {
        desc.kind = RtPathTraceMaterialFeaturePassKind::DirectReservoirResolve;
        desc.resourceInputs |=
            RT_MATERIAL_FEATURE_RESOURCE_CURRENT_PRIMARY_SURFACE |
            RT_MATERIAL_FEATURE_RESOURCE_CURRENT_DIRECT_RESERVOIR |
            RT_MATERIAL_FEATURE_RESOURCE_TEMPORAL_DIRECT_RESERVOIR |
            RT_MATERIAL_FEATURE_RESOURCE_SPATIAL_DIRECT_RESERVOIR;
        desc.resourceOutputs |= RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR;
    }
    else if (plan.output == RtPathTraceRestirPassKind::DebugVisualize)
    {
        desc.resourceOutputs |= RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR;
    }

    return desc;
}

inline bool IsPathTraceRestirPTDebugMode(int debugMode)
{
    return (debugMode >= 26 && debugMode <= 33) || debugMode == 50 || debugMode == 51 || (debugMode >= 53 && debugMode <= 56);
}

inline const char* PathTraceRestirPassKindName(RtPathTraceRestirPassKind pass)
{
    switch (pass)
    {
    case RtPathTraceRestirPassKind::InitialReservoir:
        return "InitialReservoir";
    case RtPathTraceRestirPassKind::TemporalReservoir:
        return "TemporalReservoir";
    case RtPathTraceRestirPassKind::SpatialReservoir:
        return "SpatialReservoir";
    case RtPathTraceRestirPassKind::ReservoirShading:
        return "ReservoirShading";
    case RtPathTraceRestirPassKind::DebugVisualize:
        return "DebugVisualize";
    default:
        return "Disabled";
    }
}

inline RtPathTraceRestirPassPlan BuildPathTraceRestirPassPlan(int debugMode, bool temporalPreviewVisibility)
{
    RtPathTraceRestirPassPlan plan;
    plan.debugMode = debugMode;
    plan.restirDebugMode = IsPathTraceRestirPTDebugMode(debugMode);

    switch (debugMode)
    {
    case 26:
        plan.producer = RtPathTraceRestirPassKind::InitialReservoir;
        plan.output = RtPathTraceRestirPassKind::DebugVisualize;
        plan.flags = RT_RESTIR_PASS_WRITES_INITIAL | RT_RESTIR_PASS_CONSUMES_CURRENT_SURFACE | RT_RESTIR_PASS_DEBUG_VISUALIZE;
        plan.label = "mode26InitialReservoirDebug";
        break;
    case 27:
        plan.producer = RtPathTraceRestirPassKind::InitialReservoir;
        plan.output = RtPathTraceRestirPassKind::ReservoirShading;
        plan.flags = RT_RESTIR_PASS_WRITES_INITIAL | RT_RESTIR_PASS_CONSUMES_CURRENT_SURFACE | RT_RESTIR_PASS_SHADES_RESERVOIR | RT_RESTIR_PASS_DEBUG_VISUALIZE;
        plan.label = "mode27InitialReservoirShading";
        break;
    case 28:
        plan.producer = RtPathTraceRestirPassKind::InitialReservoir;
        plan.output = RtPathTraceRestirPassKind::ReservoirShading;
        plan.flags = RT_RESTIR_PASS_WRITES_INITIAL | RT_RESTIR_PASS_CONSUMES_CURRENT_SURFACE | RT_RESTIR_PASS_SHADES_RESERVOIR | RT_RESTIR_PASS_TRACES_VISIBILITY | RT_RESTIR_PASS_DEBUG_VISUALIZE;
        plan.label = "mode28InitialReservoirVisibility";
        break;
    case 29:
        plan.producer = RtPathTraceRestirPassKind::DebugVisualize;
        plan.output = RtPathTraceRestirPassKind::DebugVisualize;
        plan.flags = RT_RESTIR_PASS_CONSUMES_CURRENT_SURFACE | RT_RESTIR_PASS_PRIMARY_SURFACE_DEBUG | RT_RESTIR_PASS_DEBUG_VISUALIZE;
        plan.label = "mode29PrimarySurfaceHistory";
        break;
    case 30:
        plan.producer = RtPathTraceRestirPassKind::DebugVisualize;
        plan.output = RtPathTraceRestirPassKind::DebugVisualize;
        plan.flags = RT_RESTIR_PASS_CONSUMES_CURRENT_SURFACE | RT_RESTIR_PASS_CONSUMES_PREVIOUS_SURFACE | RT_RESTIR_PASS_PRIMARY_SURFACE_DEBUG | RT_RESTIR_PASS_DEBUG_VISUALIZE;
        plan.label = "mode30PrimarySurfaceReprojection";
        break;
    case 31:
        plan.producer = RtPathTraceRestirPassKind::TemporalReservoir;
        plan.output = RtPathTraceRestirPassKind::DebugVisualize;
        plan.resamplingMode = RtRestirPTResamplingMode::Temporal;
        plan.flags = RT_RESTIR_PASS_WRITES_INITIAL | RT_RESTIR_PASS_WRITES_TEMPORAL | RT_RESTIR_PASS_CONSUMES_CURRENT_SURFACE | RT_RESTIR_PASS_CONSUMES_PREVIOUS_SURFACE | RT_RESTIR_PASS_CONSUMES_PREVIOUS_RESERVOIR | RT_RESTIR_PASS_DEBUG_VISUALIZE;
        plan.label = "mode31TemporalReservoirDebug";
        break;
    case 32:
        plan.producer = RtPathTraceRestirPassKind::TemporalReservoir;
        plan.output = RtPathTraceRestirPassKind::ReservoirShading;
        plan.resamplingMode = RtRestirPTResamplingMode::Temporal;
        plan.flags = RT_RESTIR_PASS_WRITES_INITIAL | RT_RESTIR_PASS_WRITES_TEMPORAL | RT_RESTIR_PASS_CONSUMES_CURRENT_SURFACE | RT_RESTIR_PASS_CONSUMES_PREVIOUS_SURFACE | RT_RESTIR_PASS_CONSUMES_PREVIOUS_RESERVOIR | RT_RESTIR_PASS_SHADES_RESERVOIR | RT_RESTIR_PASS_DEBUG_VISUALIZE | RT_RESTIR_PASS_PREVIEW_SAFETY_CAP;
        if (temporalPreviewVisibility)
        {
            plan.flags |= RT_RESTIR_PASS_TRACES_VISIBILITY;
        }
        plan.label = "mode32RoughTemporalLightingPreview";
        break;
    case 33:
        plan.producer = RtPathTraceRestirPassKind::TemporalReservoir;
        plan.output = RtPathTraceRestirPassKind::DebugVisualize;
        plan.resamplingMode = RtRestirPTResamplingMode::Temporal;
        plan.flags = RT_RESTIR_PASS_WRITES_INITIAL | RT_RESTIR_PASS_WRITES_TEMPORAL | RT_RESTIR_PASS_CONSUMES_CURRENT_SURFACE | RT_RESTIR_PASS_CONSUMES_PREVIOUS_SURFACE | RT_RESTIR_PASS_CONSUMES_PREVIOUS_RESERVOIR | RT_RESTIR_PASS_SOURCE_ATTRIBUTION | RT_RESTIR_PASS_DEBUG_VISUALIZE | RT_RESTIR_PASS_PREVIEW_SAFETY_CAP;
        plan.label = "mode33TemporalSourceAttribution";
        break;
    case 50:
        plan.producer = RtPathTraceRestirPassKind::SpatialReservoir;
        plan.output = RtPathTraceRestirPassKind::ReservoirShading;
        plan.resamplingMode = RtRestirPTResamplingMode::TemporalAndSpatial;
        plan.flags = RT_RESTIR_PASS_WRITES_INITIAL | RT_RESTIR_PASS_WRITES_TEMPORAL | RT_RESTIR_PASS_WRITES_SPATIAL | RT_RESTIR_PASS_CONSUMES_CURRENT_SURFACE | RT_RESTIR_PASS_CONSUMES_PREVIOUS_SURFACE | RT_RESTIR_PASS_CONSUMES_PREVIOUS_RESERVOIR | RT_RESTIR_PASS_SHADES_RESERVOIR | RT_RESTIR_PASS_DEBUG_VISUALIZE | RT_RESTIR_PASS_PREVIEW_SAFETY_CAP | RT_RESTIR_PASS_REQUIRES_TEMPORAL_PREPASS | RT_RESTIR_PASS_REQUIRES_SPATIAL_PREPASS;
        if (temporalPreviewVisibility)
        {
            plan.flags |= RT_RESTIR_PASS_TRACES_VISIBILITY;
        }
        plan.label = "mode50SpatialReservoirShading";
        break;
    case 51:
        plan.producer = RtPathTraceRestirPassKind::SpatialReservoir;
        plan.output = RtPathTraceRestirPassKind::DebugVisualize;
        plan.resamplingMode = RtRestirPTResamplingMode::TemporalAndSpatial;
        plan.flags = RT_RESTIR_PASS_WRITES_INITIAL | RT_RESTIR_PASS_WRITES_TEMPORAL | RT_RESTIR_PASS_WRITES_SPATIAL | RT_RESTIR_PASS_CONSUMES_CURRENT_SURFACE | RT_RESTIR_PASS_CONSUMES_PREVIOUS_SURFACE | RT_RESTIR_PASS_CONSUMES_PREVIOUS_RESERVOIR | RT_RESTIR_PASS_SOURCE_ATTRIBUTION | RT_RESTIR_PASS_DEBUG_VISUALIZE | RT_RESTIR_PASS_PREVIEW_SAFETY_CAP | RT_RESTIR_PASS_REQUIRES_TEMPORAL_PREPASS | RT_RESTIR_PASS_REQUIRES_SPATIAL_PREPASS;
        plan.label = "mode51SpatialSourceAttribution";
        break;
    case 53:
        plan.producer = RtPathTraceRestirPassKind::InitialReservoir;
        plan.output = RtPathTraceRestirPassKind::DebugVisualize;
        plan.flags = RT_RESTIR_PASS_WRITES_INITIAL | RT_RESTIR_PASS_CONSUMES_CURRENT_SURFACE | RT_RESTIR_PASS_DEBUG_VISUALIZE | RT_RESTIR_PASS_REQUIRES_INITIAL_PREPASS;
        plan.label = "mode53IndirectReservoirDebug";
        break;
    case 54:
        plan.producer = RtPathTraceRestirPassKind::InitialReservoir;
        plan.output = RtPathTraceRestirPassKind::ReservoirShading;
        plan.flags = RT_RESTIR_PASS_WRITES_INITIAL | RT_RESTIR_PASS_CONSUMES_CURRENT_SURFACE | RT_RESTIR_PASS_SHADES_RESERVOIR | RT_RESTIR_PASS_DEBUG_VISUALIZE | RT_RESTIR_PASS_REQUIRES_INITIAL_PREPASS;
        plan.label = "mode54IndirectReservoirShading";
        break;
    case 55:
        plan.producer = RtPathTraceRestirPassKind::InitialReservoir;
        plan.output = RtPathTraceRestirPassKind::DebugVisualize;
        plan.flags = RT_RESTIR_PASS_WRITES_INITIAL | RT_RESTIR_PASS_CONSUMES_CURRENT_SURFACE | RT_RESTIR_PASS_SOURCE_ATTRIBUTION | RT_RESTIR_PASS_DEBUG_VISUALIZE | RT_RESTIR_PASS_REQUIRES_INITIAL_PREPASS;
        plan.label = "mode55IndirectPathAttribution";
        break;
    case 56:
        plan.producer = RtPathTraceRestirPassKind::SpatialReservoir;
        plan.output = RtPathTraceRestirPassKind::ReservoirShading;
        plan.resamplingMode = RtRestirPTResamplingMode::TemporalAndSpatial;
        plan.flags = RT_RESTIR_PASS_WRITES_INITIAL | RT_RESTIR_PASS_WRITES_TEMPORAL | RT_RESTIR_PASS_WRITES_SPATIAL | RT_RESTIR_PASS_CONSUMES_CURRENT_SURFACE | RT_RESTIR_PASS_CONSUMES_PREVIOUS_SURFACE | RT_RESTIR_PASS_CONSUMES_PREVIOUS_RESERVOIR | RT_RESTIR_PASS_SHADES_RESERVOIR | RT_RESTIR_PASS_TRACES_VISIBILITY | RT_RESTIR_PASS_DEBUG_VISUALIZE | RT_RESTIR_PASS_PREVIEW_SAFETY_CAP | RT_RESTIR_PASS_REQUIRES_TEMPORAL_PREPASS | RT_RESTIR_PASS_REQUIRES_SPATIAL_PREPASS | RT_RESTIR_PASS_REQUIRES_INITIAL_PREPASS;
        plan.label = "mode56CombinedDirectGiPreview";
        break;
    default:
        break;
    }

    return plan;
}

inline bool PathTraceRestirPassRequiresTemporalPrepass(const RtPathTraceRestirPassPlan& plan)
{
    return (plan.flags & RT_RESTIR_PASS_REQUIRES_TEMPORAL_PREPASS) != 0;
}

inline bool PathTraceRestirPassRequiresInitialPrepass(const RtPathTraceRestirPassPlan& plan)
{
    return (plan.flags & RT_RESTIR_PASS_REQUIRES_INITIAL_PREPASS) != 0;
}

inline bool PathTraceRestirPassRequiresSpatialPrepass(const RtPathTraceRestirPassPlan& plan)
{
    return (plan.flags & RT_RESTIR_PASS_REQUIRES_SPATIAL_PREPASS) != 0;
}

inline RtRestirPTContextUpdateDesc BuildRestirPTContextUpdateDesc(
    const RtPathTraceRestirPassPlan& plan,
    uint32_t width,
    uint32_t height,
    uint32_t frameIndex,
    RtRestirPTCheckerboardMode checkerboardMode,
    float temporalDepthThreshold,
    float temporalNormalThreshold,
    bool temporalReservoirReuse,
    bool temporalFallbackSampling,
    uint32_t spatialSamples,
    float spatialRadius)
{
    RtRestirPTContextUpdateDesc desc;
    desc.width = width;
    desc.height = height;
    desc.frameIndex = frameIndex;
    desc.checkerboardMode = checkerboardMode;
    desc.resamplingMode = plan.resamplingMode;
    desc.temporalDepthThreshold = temporalDepthThreshold;
    desc.temporalNormalThreshold = temporalNormalThreshold;
    desc.temporalReservoirReuse = temporalReservoirReuse;
    desc.temporalFallbackSampling = temporalFallbackSampling;
    desc.spatialSamples = spatialSamples;
    desc.spatialRadius = spatialRadius;
    return desc;
}

inline RtPathTraceRestirPassBufferSelection ResolveRestirPTPassBufferSelection(
    const RtPathTraceRestirPassPlan& plan,
    const RtRestirPTParameters& parameters)
{
    RtPathTraceRestirPassBufferSelection selection;
    selection.initialOutput = parameters.bufferIndices.initialPathTracerOutputBufferIndex;
    selection.temporalInput = parameters.bufferIndices.temporalResamplingInputBufferIndex;
    selection.temporalOutput = parameters.bufferIndices.temporalResamplingOutputBufferIndex;
    selection.spatialInput = parameters.bufferIndices.spatialResamplingInputBufferIndex;
    selection.spatialOutput = parameters.bufferIndices.spatialResamplingOutputBufferIndex;
    selection.finalShadingInput = parameters.bufferIndices.finalShadingInputBufferIndex;
    selection.debugInput = (plan.flags & RT_RESTIR_PASS_WRITES_SPATIAL) != 0
        ? selection.spatialOutput
        : ((plan.flags & RT_RESTIR_PASS_WRITES_TEMPORAL) != 0
        ? selection.temporalOutput
        : selection.initialOutput);
    return selection;
}
