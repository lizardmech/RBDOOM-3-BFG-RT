#pragma once

// CPU-side material feature pass descriptors.
//
// Feature shaders live behind these descriptors so render pass plumbing can
// bind resources and shader tables without embedding per-feature shader paths
// or output slots in the core dispatch code.

#include "PathTracePrimarySurface.h"

#include <cstdint>

enum class RtPathTraceMaterialFeaturePassKind : uint8_t
{
    Disabled = 0,
    PrimarySurface,
    PathIntegrator,
    DirectReservoirInitial,
    DirectReservoirTemporal,
    DirectReservoirSpatial,
    DirectReservoirResolve,
    GiReservoirInitial,
    ReflectionProducer,
    TransmissionProducer,
    DebugVisualize
};

enum class RtPathTraceMaterialFeatureShaderTable : uint8_t
{
    None = 0,
    CorePathTrace,
    PrimarySurfaceProducer,
    RestirInitial,
    RestirTemporal,
    RestirSpatialReservoir,
    RestirSpatial,
    RestirCombinedResolve,
    RestirIndirectInitialProducer,
    RestirDirectTemporalProducer,
    RestirDirectSpatialReservoirProducer,
    RestirReflectionProducer,
    Count
};

enum RtPathTraceMaterialFeatureResourceMask : uint32_t
{
    RT_MATERIAL_FEATURE_RESOURCE_NONE = 0,
    RT_MATERIAL_FEATURE_RESOURCE_TLAS = 1u << 0,
    RT_MATERIAL_FEATURE_RESOURCE_SCENE_GEOMETRY = 1u << 1,
    RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_TABLE = 1u << 2,
    RT_MATERIAL_FEATURE_RESOURCE_CURRENT_PRIMARY_SURFACE = 1u << 3,
    RT_MATERIAL_FEATURE_RESOURCE_PREVIOUS_PRIMARY_SURFACE = 1u << 4,
    RT_MATERIAL_FEATURE_RESOURCE_CURRENT_DIRECT_RESERVOIR = 1u << 5,
    RT_MATERIAL_FEATURE_RESOURCE_TEMPORAL_DIRECT_RESERVOIR = 1u << 6,
    RT_MATERIAL_FEATURE_RESOURCE_SPATIAL_DIRECT_RESERVOIR = 1u << 7,
    RT_MATERIAL_FEATURE_RESOURCE_GI_RESERVOIR = 1u << 8,
    RT_MATERIAL_FEATURE_RESOURCE_OUTPUT_COLOR = 1u << 9,
    RT_MATERIAL_FEATURE_RESOURCE_MOTION_VECTORS = 1u << 10,
    RT_MATERIAL_FEATURE_RESOURCE_RR_GUIDES = 1u << 11,
    RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT = 1u << 12
};

struct RtPathTraceMaterialFeaturePassDesc
{
    RtPathTraceMaterialFeaturePassKind kind = RtPathTraceMaterialFeaturePassKind::Disabled;
    RtPathTraceMaterialFeatureShaderTable shaderTable = RtPathTraceMaterialFeatureShaderTable::None;
    uint32_t materialCapsConsumed = 0;
    uint32_t materialPassSupport = 0;
    uint32_t resourceInputs = RT_MATERIAL_FEATURE_RESOURCE_NONE;
    uint32_t resourceOutputs = RT_MATERIAL_FEATURE_RESOURCE_NONE;
    uint32_t primaryOutputResource = RT_MATERIAL_FEATURE_RESOURCE_NONE;
    bool enabled = false;
    const char* debugLabel = "disabled";
};

struct RtPathTraceMaterialFeatureShaderDesc
{
    const char* label = "disabled";
    const char* dxilShaderPath = nullptr;
    const char* spirvShaderPath = nullptr;
};

inline bool PathTraceMaterialFeaturePassHasAllInputs(const RtPathTraceMaterialFeaturePassDesc& desc, uint32_t resources)
{
    return (desc.resourceInputs & resources) == resources;
}

inline bool PathTraceMaterialFeaturePassWritesAnyOutput(const RtPathTraceMaterialFeaturePassDesc& desc, uint32_t resources)
{
    return (desc.resourceOutputs & resources) != 0;
}

inline bool PathTraceMaterialFeaturePassWritesAllOutputs(const RtPathTraceMaterialFeaturePassDesc& desc, uint32_t resources)
{
    return (desc.resourceOutputs & resources) == resources;
}

inline bool PathTraceMaterialFeaturePassIsReady(const RtPathTraceMaterialFeaturePassDesc& desc, uint32_t requiredInputs, uint32_t requiredOutputs)
{
    return desc.enabled &&
        PathTraceMaterialFeaturePassHasAllInputs(desc, requiredInputs) &&
        PathTraceMaterialFeaturePassWritesAllOutputs(desc, requiredOutputs);
}

inline bool PathTraceMaterialFeaturePassIsReady(const RtPathTraceMaterialFeaturePassDesc& desc)
{
    const uint32_t requiredOutputs = desc.primaryOutputResource != RT_MATERIAL_FEATURE_RESOURCE_NONE
        ? desc.primaryOutputResource
        : desc.resourceOutputs;
    return PathTraceMaterialFeaturePassIsReady(desc, desc.resourceInputs, requiredOutputs);
}

inline RtPathTraceMaterialFeaturePassDesc BuildPathTracePrimarySurfaceFeaturePassDesc(bool enabled)
{
    RtPathTraceMaterialFeaturePassDesc desc;
    desc.kind = RtPathTraceMaterialFeaturePassKind::PrimarySurface;
    desc.shaderTable = RtPathTraceMaterialFeatureShaderTable::PrimarySurfaceProducer;
    desc.materialPassSupport = RT_PATH_TRACE_MATERIAL_PASS_PRIMARY_SURFACE;
    desc.resourceInputs =
        RT_MATERIAL_FEATURE_RESOURCE_TLAS |
        RT_MATERIAL_FEATURE_RESOURCE_SCENE_GEOMETRY |
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_TABLE;
    desc.resourceOutputs =
        RT_MATERIAL_FEATURE_RESOURCE_CURRENT_PRIMARY_SURFACE |
        RT_MATERIAL_FEATURE_RESOURCE_MOTION_VECTORS |
        RT_MATERIAL_FEATURE_RESOURCE_RR_GUIDES;
    desc.enabled = enabled;
    desc.debugLabel = "primary-surface-producer";
    return desc;
}
