#pragma once

// CPU-side material feature pass descriptors.
//
// Feature shaders live behind these descriptors so render pass plumbing can
// bind resources and shader tables without embedding per-feature shader paths
// or output slots in the core dispatch code.

#include "PathTracePrimarySurface.h"

#include <cstddef>
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
    RT_MATERIAL_FEATURE_RESOURCE_TRANSMISSION_OUTPUT = 1u << 12,
    RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_SIDECAR = 1u << 13,
    RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_RUNTIME_CONSTANTS = 1u << 14
};

struct RtPathTraceMaterialFeaturePassDesc
{
    RtPathTraceMaterialFeaturePassKind kind = RtPathTraceMaterialFeaturePassKind::Disabled;
    RtPathTraceMaterialFeatureShaderTable shaderTable = RtPathTraceMaterialFeatureShaderTable::None;
    const char* featureId = "disabled";
    uint32_t materialCapsConsumed = 0;
    uint32_t materialPassSupport = 0;
    uint32_t resourceInputs = RT_MATERIAL_FEATURE_RESOURCE_NONE;
    uint32_t resourceOutputs = RT_MATERIAL_FEATURE_RESOURCE_NONE;
    uint32_t primaryOutputResource = RT_MATERIAL_FEATURE_RESOURCE_NONE;
    uint32_t sharedOutputPriority = 0;
    bool enabled = false;
    const char* debugLabel = "disabled";
};

struct RtPathTraceMaterialFeatureRayTracingPipelineDesc
{
    const char* rayGenerationShader = "RayGen";
    const char* missShader = "Miss";
    const char* shadowMissShader = "ShadowMiss";
    const char* closestHitShader = "ClosestHit";
    const char* anyHitShader = "AnyHit";
    const char* shadowClosestHitShader = "ShadowClosestHit";
    const char* shadowAnyHitShader = "ShadowAnyHit";
    const char* hitGroupName = "HitGroup";
    const char* shadowHitGroupName = "ShadowHitGroup";
    uint32_t maxPayloadSize = 64;
    uint32_t maxAttributeSize = 8;
    uint32_t maxRecursionDepth = 1;
};

struct RtPathTraceMaterialFeatureShaderDesc
{
    const char* label = "disabled";
    const char* shaderBlobPath = nullptr;
    RtPathTraceMaterialFeatureRayTracingPipelineDesc rtPipeline;
};

enum class RtPathTraceMaterialFeatureBindingKind : uint8_t
{
    Unknown = 0,
    StructuredBufferSrv,
    StructuredBufferUav,
    ConstantBuffer,
    TextureUav
};

struct RtPathTraceMaterialFeatureBindingDesc
{
    uint32_t resource = RT_MATERIAL_FEATURE_RESOURCE_NONE;
    uint32_t slot = 0xffffffffu;
    RtPathTraceMaterialFeatureBindingKind kind = RtPathTraceMaterialFeatureBindingKind::Unknown;
    const char* debugName = "unknown";
};

struct RtPathTraceMaterialFeatureRuntimeInfo
{
    float writesOutputColor = 0.0f;
    float ready = 0.0f;
    float debugMode = 0.0f;
    float frameIndex = 0.0f;
    float featureParams0[4] = {};
    float featureParams1[4] = {};
};

struct RtPathTraceMaterialFeatureRuntimeConstants
{
    float runtimeInfo[4] = {};
    float featureParams0[4] = {};
    float featureParams1[4] = {};
};

using RtPathTraceMaterialFeatureRuntimeInfoCallback = void (*)(
    RtPathTraceMaterialFeatureRuntimeInfo& runtimeInfo,
    const RtPathTraceMaterialFeaturePassDesc& passDesc);

struct RtPathTraceMaterialFeatureValidationDesc
{
    const char* buildProof = "none";
    const char* runtimeRoute = "none";
    const char* supportedMaterialTest = "none";
    const char* unsupportedMaterialTest = "none";
    const char* baselineRegressionCheck = "none";
    const char* resourceBindingProof = "none";
    const char* cpuShaderAbiProof = "none";
};

static constexpr uint32_t RT_PATH_TRACE_MATERIAL_FEATURE_SHADER_STATE_INVALID = 0xffffffffu;
static constexpr size_t RT_PATH_TRACE_MATERIAL_FEATURE_SHADER_STATE_CAPACITY = 32;

struct RtPathTraceMaterialFeaturePassRegistration
{
    RtPathTraceMaterialFeaturePassDesc passDesc;
    RtPathTraceMaterialFeatureShaderDesc shaderDesc;
    uint32_t shaderStateIndex = RT_PATH_TRACE_MATERIAL_FEATURE_SHADER_STATE_INVALID;
    const RtPathTraceMaterialFeatureBindingDesc* bindingMetadata = nullptr;
    size_t bindingMetadataCount = 0;
    RtPathTraceMaterialFeatureRuntimeInfoCallback runtimeInfoCallback = nullptr;
    RtPathTraceMaterialFeatureValidationDesc validation;
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
        desc.resourceOutputs != RT_MATERIAL_FEATURE_RESOURCE_NONE &&
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

inline bool PathTraceMaterialFeatureBindingMetadataCoversResource(
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    uint32_t resource)
{
    for (size_t i = 0; registration.bindingMetadata && i < registration.bindingMetadataCount; ++i)
    {
        if (registration.bindingMetadata[i].resource == resource &&
            registration.bindingMetadata[i].slot != 0xffffffffu &&
            registration.bindingMetadata[i].kind != RtPathTraceMaterialFeatureBindingKind::Unknown)
        {
            return true;
        }
    }
    return false;
}

inline bool PathTraceMaterialFeatureBindingMetadataCoversResources(
    const RtPathTraceMaterialFeaturePassRegistration& registration,
    uint32_t resources)
{
    for (uint32_t resource = 1u; resource != 0u; resource <<= 1u)
    {
        if ((resources & resource) != 0u &&
            !PathTraceMaterialFeatureBindingMetadataCoversResource(registration, resource))
        {
            return false;
        }
    }
    return true;
}

inline bool PathTraceMaterialFeatureBindingMetadataCoversPass(
    const RtPathTraceMaterialFeaturePassRegistration& registration)
{
    return PathTraceMaterialFeatureBindingMetadataCoversResources(registration, registration.passDesc.resourceInputs) &&
        PathTraceMaterialFeatureBindingMetadataCoversResources(registration, registration.passDesc.resourceOutputs);
}

inline RtPathTraceMaterialFeaturePassDesc BuildPathTracePrimarySurfaceFeaturePassDesc(bool enabled)
{
    RtPathTraceMaterialFeaturePassDesc desc;
    desc.kind = RtPathTraceMaterialFeaturePassKind::PrimarySurface;
    desc.shaderTable = RtPathTraceMaterialFeatureShaderTable::PrimarySurfaceProducer;
    desc.featureId = "primary-surface-producer";
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
