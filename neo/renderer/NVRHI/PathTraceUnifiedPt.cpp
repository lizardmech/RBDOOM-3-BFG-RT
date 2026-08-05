#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCVars.h"
#include "PathTraceUnifiedPt.h"
#include "PathTraceUnifiedPtPrimaryReceiver.h"

#include <nvrhi/utils.h>

#include <limits>

namespace {

static constexpr uint32_t UPT04_RESERVOIR_STRIDE = 64u;
static constexpr uint32_t UPT04_COMPACT_VERTEX_STRIDE = 48u;
static constexpr uint32_t UPT04_COMPACT_GEOMETRY_PUSH_CONSTANT_BYTES = 16u;
static constexpr uint32_t UPT04_COMPACT_LIGHT_STRIDE = 64u;
static constexpr uint32_t UPT04_COMPACT_LIGHT_PUSH_CONSTANT_BYTES = 4u;
static constexpr uint32_t UPT04_COMPACT_MATERIAL_STRIDE = 48u;
static constexpr uint32_t UPT04_COMPACT_MATERIAL_PUSH_CONSTANT_BYTES = 4u;
static constexpr uint32_t UPT04_CONTINUATION_HIT_STRIDE = 32u;
static constexpr uint32_t UPT04_PUSH_CONSTANT_BYTES = 128u;
static constexpr uint32_t UPT04_FAMILY_LOCAL_LIGHT = 1u << 0u;
static constexpr uint32_t UPT04_FAMILY_INDIRECT = 1u << 1u;
static constexpr uint32_t UPT04_ROUTE_STATIC_BUCKETS = 1u << 1u;
static constexpr uint32_t UPT04_EMISSIVE_LOOKUP_EXACT = 1u << 3u;
static constexpr uint32_t UPT04_MATERIAL_USE_SPECULAR_MAPS = 1u << 4u;
static constexpr uint32_t UPT04_MATERIAL_LEGACY_SPECMAP_TO_PBR = 1u << 5u;
static constexpr uint32_t UPT04_DIRECT_TARGET_PDF_PARITY = 1u << 29u;
static constexpr uint32_t UPT04_DIRECT_PROPOSAL_PARITY = 1u << 30u;
static constexpr uint32_t UPT04_ANALYTIC_PORTAL_DOMAIN = 1u << 28u;
static constexpr uint32_t UPT04_TRANSPORT_K_MAX = 2u;
static constexpr uint32_t UPT04_TRANSPORT_POLICY_ID = 1u;
static constexpr uint32_t UPT04_NEE_RIS_BASELINE_CANDIDATE_COUNT = 8u;
static constexpr uint32_t UPT04_NEE_RIS_PARITY_MAX_CANDIDATE_COUNT = 33u;
static constexpr uint32_t UPT04_ABI_VERSION = 7u;
static constexpr uint32_t UPT04_DIAGNOSTIC_COUNTER_COUNT = 27u;
static constexpr uint32_t UPT04_DIAGNOSTIC_BYTES =
    UPT04_DIAGNOSTIC_COUNTER_COUNT * sizeof(uint32_t);
static constexpr uint32_t UPT05_PUSH_CONSTANT_BYTES = 16u;
static constexpr uint32_t UPT07_PUSH_CONSTANT_BYTES = 144u;
static constexpr uint32_t UPT07_MAXIMUM_HISTORY_AGE = 63u;
static constexpr uint32_t UPT07_MAXIMUM_HISTORY_CONTRIBUTION_RATIO = 32u;
static constexpr uint32_t UPT07_GEOMETRY_FLAG_PREVIOUS_BEST_SEED = 1u << 31u;
static constexpr uint32_t UPT07_GEOMETRY_FLAG_PAIRWISE_MIS = 1u << 27u;
static constexpr uint32_t UPT07_GEOMETRY_FLAG_DUPLICATION_MAP = 1u << 26u;
static constexpr uint32_t UPT08_PUSH_CONSTANT_BYTES = 16u;
static constexpr uint32_t UPT09_PUSH_CONSTANT_BYTES = 80u;
static constexpr uint32_t UPT09_MAXIMUM_INPUT_M = 32u;
static constexpr uint32_t UPT09_REGULAR_NEIGHBOR_COUNT = 3u;
static constexpr uint32_t UPT09_RESCUE_NEIGHBOR_COUNT = 12u;
static constexpr float UPT09_NEIGHBOR_RADIUS = 30.0f;

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

static const char* Upt04ReceiverName(uint32_t mode)
{
    return mode == 2u
        ? "compact32"
        : (mode == 1u ? "compact48" : "legacy176");
}

static const char* Upt04InitialShaderPath(
    PathTraceUnifiedPtBackend backend,
    PathTraceUnifiedPtFamily family,
    uint32_t primaryReceiverMode,
    bool compactGeometry,
    bool compactLights,
    bool compactMaterials,
    bool splitContinuation)
{
    if (backend == PathTraceUnifiedPtBackend::RayQuery)
    {
        switch (family)
        {
        case PathTraceUnifiedPtFamily::Unified:
            return splitContinuation
                ? (compactMaterials
                    ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_geometry48_light64_material48_continuation32.bin"
                    : "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_geometry48_light64_continuation32.bin")
                : (compactMaterials
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_geometry48_light64_material48.bin"
                : (compactLights
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_geometry48_light64.bin"
                : (compactGeometry
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32_geometry48.bin"
                : (primaryReceiverMode == 2u
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact32.bin"
                : (primaryReceiverMode == 1u
                    ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery_compact.bin"
                    : "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_rayquery.bin")))));
        case PathTraceUnifiedPtFamily::IndirectOnly:
            return compactMaterials
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_indirect_only_rayquery_compact32_geometry48_light64_material48.bin"
                : (compactLights
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_indirect_only_rayquery_compact32_geometry48_light64.bin"
                : (compactGeometry
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_indirect_only_rayquery_compact32_geometry48.bin"
                : (primaryReceiverMode == 2u
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_indirect_only_rayquery_compact32.bin"
                : (primaryReceiverMode == 1u
                    ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_indirect_only_rayquery_compact.bin"
                    : "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_indirect_only_rayquery.bin"))));
        default:
            return compactMaterials
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_direct_only_rayquery_compact32_geometry48_light64_material48.bin"
                : (compactLights
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_direct_only_rayquery_compact32_geometry48_light64.bin"
                : (compactGeometry
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_direct_only_rayquery_compact32_geometry48.bin"
                : (primaryReceiverMode == 2u
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_direct_only_rayquery_compact32.bin"
                : (primaryReceiverMode == 1u
                    ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_direct_only_rayquery_compact.bin"
                    : "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_direct_only_rayquery.bin"))));
        }
    }

    switch (family)
    {
    case PathTraceUnifiedPtFamily::Unified:
        return primaryReceiverMode == 2u
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_raygen_compact32.bin"
            : (primaryReceiverMode == 1u
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_raygen_compact.bin"
                : "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_raygen.bin");
    case PathTraceUnifiedPtFamily::IndirectOnly:
        return primaryReceiverMode == 2u
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_indirect_only_raygen_compact32.bin"
            : (primaryReceiverMode == 1u
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_indirect_only_raygen_compact.bin"
                : "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_indirect_only_raygen.bin");
    default:
        return primaryReceiverMode == 2u
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_direct_only_raygen_compact32.bin"
            : (primaryReceiverMode == 1u
                ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_direct_only_raygen_compact.bin"
                : "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_direct_only_raygen.bin");
    }
}

static const char* Upt04SplitDirectShaderPath()
{
    return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_split_direct_rayquery_compact32_geometry48_light64.bin";
}

static const char* Upt04SplitIndirectShaderPath(bool compactMaterials)
{
    return compactMaterials
        ? "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_split_indirect_rayquery_compact32_geometry48_light64_material48.bin"
        : "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_initial_split_indirect_rayquery_compact32_geometry48_light64.bin";
}

static const char* Upt04ContinuationTraceShaderPath()
{
    return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_continuation_trace_rayquery_compact32.bin";
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
    return inputs.shaderProofMode >= 7u && inputs.shaderProofMode <= 14u
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

static bool Upt04UsesTraversalIsolationLayout(uint32_t pipelineVariant)
{
    return pipelineVariant == 14u;
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
        pipelineVariant != 13u && pipelineVariant != 14u;
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
    case 14u:
        return "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_full_frame_traversal_probe.bin";
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
    case 14u: return "PathTraceUnifiedPtFullFrameTraversalProbeSlang";
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
    case 14u: return "UPT.D0 TraversalOnly RayQuery";
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

static uint64_t Upt06BuildContentGeneration(
    const PathTraceUnifiedPtDispatchInputs& dispatch,
    uint32_t enabledFamilyMask,
    uint32_t specializationIdentity)
{
    // Full-width temporal compatibility fingerprint. It describes the stored
    // proposal/estimator ABI, not live scene contents. Moving geometry and
    // changing light payloads are handled by reprojection plus per-sample
    // identity replay; hashing those live signatures invalidated alternating
    // pages almost every frame and made temporal admission nondeterministic.
    // Camera cuts and resource resets are owned by the separate historyEpoch.
    uint64_t hash = 1469598103934665603ull;
    hash = Upt04HashValue(hash, UPT04_ABI_VERSION);
    // These words define the proposal mixture and stored-PDF interpretation.
    // They must invalidate a future history page even when scene identity did
    // not change.
    hash = Upt04HashValue(hash, enabledFamilyMask);
    hash = Upt04HashValue(hash, specializationIdentity);
    hash = Upt04HashValue(hash, UPT04_TRANSPORT_K_MAX);
    hash = Upt04HashValue(hash, UPT04_TRANSPORT_POLICY_ID);
    hash = Upt04HashValue(hash, dispatch.directProposalParity
        ? UPT04_NEE_RIS_PARITY_MAX_CANDIDATE_COUNT
        : UPT04_NEE_RIS_BASELINE_CANDIDATE_COUNT);
    hash = Upt04HashValue(hash,
        r_pathTracingUnifiedPtDirectTargetPdfParity.GetBool() ? 1u : 0u);
    hash = Upt04HashValue(hash,
        r_pathTracingUnifiedPtAnalyticPortalDomain.GetBool() ? 1u : 0u);
    hash = Upt04HashValue(hash, dispatch.width);
    hash = Upt04HashValue(hash, dispatch.height);
    hash = Upt04HashValue(hash, dispatch.materialPolicyFlags);
    return hash;
}

struct Upt04InitialControl
{
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t surfaceCount;
    uint32_t frameSampleIndex;
    uint32_t enabledFamilyMask;
    float primaryCameraOriginX;
    uint32_t emissiveRangeStart;
    uint32_t emissiveRangeCount;
    uint32_t analyticRangeStart;
    uint32_t analyticRangeCount;
    uint32_t availabilityFlags;
    uint32_t logicalTextureCount;
    float primaryCameraOriginY;
    float primaryCameraOriginZ;
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

struct Upt07TemporalDirectControl
{
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t surfaceCount;
    uint32_t historyAvailable;
    uint32_t emissiveRangeStart;
    uint32_t emissiveRangeCount;
    uint32_t analyticRangeStart;
    uint32_t analyticRangeCount;
    uint32_t currentLightCount;
    uint32_t frameSampleIndex;
    uint32_t maximumHistoryM;
    uint32_t maximumHistoryAge;
    float previousCameraOrigin[3];
    uint32_t previousCameraValid;
    float previousCameraForward[3];
    float previousCameraTanX;
    float previousCameraLeft[3];
    float previousCameraTanY;
    float previousCameraUp[3];
    uint32_t previousCameraHistorySearchMode;
    uint32_t geometryAvailabilityFlags;
    uint32_t emissiveReplayCount;
    uint32_t previousToCurrentLightCount;
    uint32_t maximumHistoryContributionRatio;
    float primaryCameraOrigin[3];
    uint32_t compactPrimaryHistory;
};
static_assert(sizeof(Upt07TemporalDirectControl) == UPT07_PUSH_CONSTANT_BYTES,
    "UPT-07 host push constants must match Slang reflection");

struct Upt08Control
{
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t surfaceCount;
    uint32_t packedRowPitch;
};
static_assert(sizeof(Upt08Control) == UPT08_PUSH_CONSTANT_BYTES,
    "UPT-08 host push constants must match Slang reflection");

struct Upt09SpatialDirectControl
{
    uint32_t renderWidth;
    uint32_t renderHeight;
    uint32_t surfaceCount;
    uint32_t frameSampleIndex;
    uint32_t emissiveRangeStart;
    uint32_t emissiveRangeCount;
    uint32_t analyticRangeStart;
    uint32_t analyticRangeCount;
    uint32_t currentLightCount;
    uint32_t maximumInputM;
    uint32_t regularNeighborCount;
    uint32_t rescueNeighborCount;
    float neighborRadius;
    uint32_t geometryAvailabilityFlags;
    uint32_t emissiveReplayCount;
    uint32_t proofMode;
    float primaryCameraOrigin[3];
    uint32_t compactPrimaryHistory;
};
static_assert(sizeof(Upt09SpatialDirectControl) == UPT09_PUSH_CONSTANT_BYTES,
    "UPT-09 host push constants must match Slang reflection");

struct Upt04CompactGeometryPackControl
{
    uint32_t staticVertexCount;
    uint32_t dynamicVertexCount;
    uint32_t rigidVertexCount;
    uint32_t skinnedVertexCount;
};
static_assert(
    sizeof(Upt04CompactGeometryPackControl) ==
        UPT04_COMPACT_GEOMETRY_PUSH_CONSTANT_BYTES,
    "UPT compact-geometry push constants must match Slang reflection");

struct Upt04CompactLightPackControl
{
    uint32_t lightCount;
};
static_assert(
    sizeof(Upt04CompactLightPackControl) == UPT04_COMPACT_LIGHT_PUSH_CONSTANT_BYTES,
    "UPT compact-light push constants must match Slang reflection");

struct Upt04CompactMaterialPackControl
{
    uint32_t materialCount;
};
static_assert(
    sizeof(Upt04CompactMaterialPackControl) ==
        UPT04_COMPACT_MATERIAL_PUSH_CONSTANT_BYTES,
    "UPT compact-material push constants must match Slang reflection");

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
    const uint32_t expectedPrimaryStride = dispatch.primaryReceiverMode == 2u
        ? PATH_TRACE_UNIFIED_PT_PRIMARY_RECEIVER32_STRIDE
        : (dispatch.primaryReceiverMode == 1u
            ? PATH_TRACE_UNIFIED_PT_PRIMARY_RECEIVER_STRIDE
            : 176u);
    if (dispatch.primarySurfaceBuffer->getDesc().structStride !=
        expectedPrimaryStride)
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
    bool diagnostics,
    bool splitContinuation)
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
    if (splitContinuation)
    {
        desc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(24));
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
    nvrhi::BufferHandle diagnosticCounters,
    nvrhi::BufferHandle compactStaticVertices,
    nvrhi::BufferHandle compactDynamicVertices,
    nvrhi::BufferHandle compactRigidVertices,
    nvrhi::BufferHandle compactSkinnedVertices,
    nvrhi::BufferHandle compactLights,
    nvrhi::BufferHandle compactMaterials,
    nvrhi::BufferHandle continuationHits)
{
    const RtPathTraceSceneInputs& inputs = *dispatch.sceneInputs;
    const RtPathTraceSceneInputGeometry& geometry = inputs.geometry;
    const RtPathTraceSceneInputMaterials& materials = inputs.materials;
    const RtPathTraceSceneInputLights& lights = inputs.lights;

    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, geometry.tlas));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, dispatch.primarySurfaceBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        2, dispatch.compactLights ? compactLights : lights.restirLightManagerCurrentPayloadBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        3, dispatch.compactMaterials ? compactMaterials : materials.materialTableBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(4, page0));
    desc.addItem(nvrhi::BindingSetItem::Sampler(5, materials.textureSampler));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, lights.emissiveTriangleBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        7, dispatch.compactGeometry ? compactStaticVertices : geometry.staticVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(8, geometry.staticIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(9, geometry.staticTriangleClassBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(10, geometry.staticTriangleMaterialIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        11, dispatch.compactGeometry ? compactDynamicVertices : geometry.dynamicVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(12, geometry.dynamicIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(13, geometry.dynamicTriangleClassBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(14, geometry.dynamicTriangleMaterialIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        15, dispatch.compactGeometry ? compactRigidVertices : geometry.rigidRouteVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(16, geometry.rigidRouteIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(17, geometry.rigidRouteInstanceBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        18, dispatch.compactGeometry ? compactSkinnedVertices : Upt04SkinnedVertexBuffer(inputs)));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(19, Upt04SkinnedIndexBuffer(inputs)));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(20, geometry.skinnedHitRouteRecordBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(21, geometry.skinnedHitRouteTriangleBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(22, lights.unifiedPtEmissiveLookupBuffer));
    if (dispatch.diagnostics)
    {
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
            23, diagnosticCounters));
    }
    if (dispatch.splitContinuation)
    {
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            24, continuationHits));
    }
    desc.addItem(nvrhi::BindingSetItem::PushConstants(0, UPT04_PUSH_CONSTANT_BYTES));
    return desc;
}

static nvrhi::BindingSetDesc Upt04BuildDirectBindingSetDesc(
    const PathTraceUnifiedPtDispatchInputs& dispatch,
    nvrhi::BufferHandle page0,
    nvrhi::BufferHandle compactStaticVertices,
    nvrhi::BufferHandle compactDynamicVertices,
    nvrhi::BufferHandle compactRigidVertices,
    nvrhi::BufferHandle compactSkinnedVertices,
    nvrhi::BufferHandle compactLights)
{
    const RtPathTraceSceneInputs& inputs = *dispatch.sceneInputs;
    const RtPathTraceSceneInputGeometry& geometry = inputs.geometry;
    const RtPathTraceSceneInputLights& lights = inputs.lights;
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, geometry.tlas));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, dispatch.primarySurfaceBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        2, dispatch.compactLights ? compactLights : lights.restirLightManagerCurrentPayloadBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(4, page0));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, lights.emissiveTriangleBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        7, dispatch.compactGeometry ? compactStaticVertices : geometry.staticVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(8, geometry.staticIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(10, geometry.staticTriangleMaterialIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        11, dispatch.compactGeometry ? compactDynamicVertices : geometry.dynamicVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(12, geometry.dynamicIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(14, geometry.dynamicTriangleMaterialIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        15, dispatch.compactGeometry ? compactRigidVertices : geometry.rigidRouteVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(16, geometry.rigidRouteIndexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(17, geometry.rigidRouteInstanceBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        18, dispatch.compactGeometry ? compactSkinnedVertices : Upt04SkinnedVertexBuffer(inputs)));
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
            : 0u) |
        ((dispatch.materialPolicyFlags
                & PATH_TRACE_UPT_MATERIAL_USE_SPECULAR_MAPS) != 0u
            ? UPT04_MATERIAL_USE_SPECULAR_MAPS
            : 0u) |
        ((dispatch.materialPolicyFlags
                & PATH_TRACE_UPT_MATERIAL_LEGACY_SPECMAP_TO_PBR) != 0u
            ? UPT04_MATERIAL_LEGACY_SPECMAP_TO_PBR
            : 0u) |
        (dispatch.directProposalParity
            ? UPT04_DIRECT_PROPOSAL_PARITY
            : 0u) |
        (r_pathTracingUnifiedPtDirectTargetPdfParity.GetBool()
            ? UPT04_DIRECT_TARGET_PDF_PARITY
            : 0u) |
        (r_pathTracingUnifiedPtAnalyticPortalDomain.GetBool()
            ? UPT04_ANALYTIC_PORTAL_DOMAIN
            : 0u);
    const uint64_t surfaceCount64 = uint64_t(dispatch.width) * uint64_t(dispatch.height);

    Upt04InitialControl control = {};
    control.renderWidth = dispatch.width;
    control.renderHeight = dispatch.height;
    control.surfaceCount = static_cast<uint32_t>(surfaceCount64);
    control.frameSampleIndex = dispatch.frameSampleIndex;
    control.enabledFamilyMask = enabledFamilyMask;
    control.primaryCameraOriginX = dispatch.primaryCameraOrigin[0];
    control.emissiveRangeStart = lights.restirLightManagerEmissiveRangeOffset;
    control.emissiveRangeCount = lights.restirLightManagerEmissiveRangeCount;
    control.analyticRangeStart = lights.restirLightManagerDoomAnalyticRangeOffset;
    control.analyticRangeCount = lights.restirLightManagerDoomAnalyticSampleableCount;
    control.availabilityFlags = availabilityFlags;
    control.logicalTextureCount = static_cast<uint32_t>(Max(0, materials.logicalTextureDescriptorCount));
    control.primaryCameraOriginY = dispatch.primaryCameraOrigin[1];
    control.primaryCameraOriginZ = dispatch.primaryCameraOrigin[2];
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
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_bindingSets[page] = nullptr;
        m_bindingSetDescs[page] = nvrhi::BindingSetDesc();
        m_bindingSetDescValid[page] = false;
    }
    m_shaderTable = nullptr;
    m_rayPipeline = nullptr;
    m_rayGenerationLibrary = nullptr;
    m_missLibrary = nullptr;
    m_closestHitLibrary = nullptr;
    m_splitIndirectComputePipeline = nullptr;
    m_splitIndirectComputeShader = nullptr;
    m_computePipeline = nullptr;
    m_computeShader = nullptr;
    m_bindingLayout = nullptr;
    m_pipelineAttempted = false;
}

void PathTraceUnifiedPtState::ReleaseCompactGeometry()
{
    m_compactGeometryBindingSet = nullptr;
    m_compactGeometryBindingSetDesc = nvrhi::BindingSetDesc();
    m_compactGeometryBindingSetDescValid = false;
    m_compactGeometryPipeline = nullptr;
    m_compactGeometryShader = nullptr;
    m_compactGeometryBindingLayout = nullptr;
    m_compactGeometryPipelineAttempted = false;
    m_compactStaticVertices = nullptr;
    m_compactDynamicVertices = nullptr;
    m_compactRigidVertices = nullptr;
    m_compactSkinnedVertices = nullptr;
    m_compactStaticVertexCapacity = 0;
    m_compactDynamicVertexCapacity = 0;
    m_compactRigidVertexCapacity = 0;
    m_compactSkinnedVertexCapacity = 0;
}

void PathTraceUnifiedPtState::ReleaseCompactLights()
{
    m_compactLightBindingSet = nullptr;
    m_compactLightBindingSetDesc = nvrhi::BindingSetDesc();
    m_compactLightBindingSetDescValid = false;
    m_compactLightPipeline = nullptr;
    m_compactLightShader = nullptr;
    m_compactLightBindingLayout = nullptr;
    m_compactLightPipelineAttempted = false;
    m_compactLightsBuffer = nullptr;
    m_compactLightCapacity = 0;
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_temporalBindingSets[page] = nullptr;
        m_temporalBindingSetDescValid[page] = false;
        m_spatialBindingSets[page] = nullptr;
        m_spatialBindingSetDescValid[page] = false;
    }
}

void PathTraceUnifiedPtState::ReleaseCompactMaterials()
{
    m_compactMaterialBindingSet = nullptr;
    m_compactMaterialBindingSetDesc = nvrhi::BindingSetDesc();
    m_compactMaterialBindingSetDescValid = false;
    m_compactMaterialPipeline = nullptr;
    m_compactMaterialShader = nullptr;
    m_compactMaterialBindingLayout = nullptr;
    m_compactMaterialPipelineAttempted = false;
    m_compactMaterialsBuffer = nullptr;
    m_compactMaterialCapacity = 0;
}

void PathTraceUnifiedPtState::ReleaseContinuation()
{
    m_continuationBindingSet = nullptr;
    m_continuationBindingSetDesc = nvrhi::BindingSetDesc();
    m_continuationBindingSetDescValid = false;
    m_continuationPipeline = nullptr;
    m_continuationShader = nullptr;
    m_continuationBindingLayout = nullptr;
    m_continuationPipelineAttempted = false;
    m_continuationHits = nullptr;
    m_continuationCapacity = 0;
    // The final D0 binding set owns the same sidecar as an SRV at slot 24.
    // Never retain a descriptor set that points at a released/replaced buffer.
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_bindingSets[page] = nullptr;
        m_bindingSetDescValid[page] = false;
    }
}

void PathTraceUnifiedPtState::ReleaseTemporal()
{
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_temporalBindingSets[page] = nullptr;
        m_temporalBindingSetDescs[page] = nvrhi::BindingSetDesc();
        m_temporalBindingSetDescValid[page] = false;
    }
    m_temporalPipeline = nullptr;
    m_temporalShader = nullptr;
    m_temporalBindingLayout = nullptr;
    m_temporalPipelineAttempted = false;
    m_reportedTemporalHistoryAvailable = -1;
    m_reportedTemporalSkipReason = -1;
}

void PathTraceUnifiedPtState::ReleaseDuplication()
{
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_duplicationFillBindingSets[page] = nullptr;
        m_duplicationComputeBindingSets[page] = nullptr;
        m_duplicationScores[page] = nullptr;
        m_duplicationMetadata[page].Invalidate();
        m_temporalBindingSets[page] = nullptr;
        m_temporalBindingSetDescValid[page] = false;
    }
    m_duplicationSampleIds = nullptr;
    m_duplicationFillPipeline = nullptr;
    m_duplicationComputePipeline = nullptr;
    m_duplicationFillShader = nullptr;
    m_duplicationComputeShader = nullptr;
    m_duplicationBindingLayout = nullptr;
    m_duplicationPipelineAttempted = false;
    m_duplicationWidth = 0u;
    m_duplicationHeight = 0u;
    m_duplicationPackedRowPitch = 0u;
}

void PathTraceUnifiedPtState::ReleaseSpatial()
{
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_spatialBindingSets[page] = nullptr;
        m_spatialBindingSetDescs[page] = nvrhi::BindingSetDesc();
        m_spatialBindingSetDescValid[page] = false;
    }
    m_spatialPipeline = nullptr;
    m_spatialShader = nullptr;
    m_spatialBindingLayout = nullptr;
    m_spatialPipelineAttempted = false;
    m_spatialExecutedThisFrame = false;
}

void PathTraceUnifiedPtState::Release()
{
    ReleasePipeline();
    ReleaseCompactGeometry();
    ReleaseCompactLights();
    ReleaseCompactMaterials();
    ReleaseContinuation();
    ReleaseTemporal();
    ReleaseDuplication();
    ReleaseSpatial();
    ReleaseResolve();
    m_page0 = nullptr;
    m_page1 = nullptr;
    m_page0Metadata.Invalidate();
    m_page1Metadata.Invalidate();
    m_currentPageIndex = 0u;
    m_historyPageIndex = 1u;
    m_observedHistoryEpoch = 0;
    m_lastPublishedFrameSerial = 0;
    m_pageWidth = 0;
    m_pageHeight = 0;
    m_pageBytes = 0;
    m_page0NeedsAllocationClear = false;
    m_diagnosticCounters = nullptr;
    m_diagnosticReadback = nullptr;
    m_diagnosticReadbackPending = false;
    m_diagnosticReadbackDelayFrames = 0;
    m_diagnosticReadbackSampleIndex = 0;
    m_diagnosticReadbackWidth = 0;
    m_diagnosticReadbackHeight = 0;
    m_diagnosticReadbackFamily = PathTraceUnifiedPtFamily::DirectOnly;
    m_pipelineVariant = 0;
    m_selectionValid = false;
    m_diagnostics = false;
    m_primaryReceiverMode = 0;
    m_compactGeometry = false;
    m_compactLights = false;
    m_compactMaterials = false;
    m_splitInitial = false;
    m_splitContinuation = false;
    m_temporalModeActive = false;
    m_initialPublishedThisFrame = false;
    m_spatialModeActive = false;
    m_temporalCompactLights = false;
    m_temporalDuplication = false;
    m_spatialCompactLights = false;
    m_resourceFailureLogged = false;
    m_reportedProofStage = UINT32_MAX;
}

void PathTraceUnifiedPtState::ReleaseResolve()
{
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_resolveBindingSets[page] = nullptr;
        m_resolveBindingSetDescs[page] = nvrhi::BindingSetDesc();
        m_resolveBindingSetDescValid[page] = false;
    }
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

nvrhi::BufferHandle PathTraceUnifiedPtState::CurrentPage() const
{
    return m_currentPageIndex == 0u ? m_page0 : m_page1;
}

nvrhi::BufferHandle PathTraceUnifiedPtState::HistoryPage() const
{
    return m_historyPageIndex == 0u ? m_page0 : m_page1;
}

PathTraceUnifiedPtPageMetadata& PathTraceUnifiedPtState::CurrentPageMetadata()
{
    return m_currentPageIndex == 0u ? m_page0Metadata : m_page1Metadata;
}

const PathTraceUnifiedPtPageMetadata& PathTraceUnifiedPtState::CurrentPageMetadata() const
{
    return m_currentPageIndex == 0u ? m_page0Metadata : m_page1Metadata;
}

PathTraceUnifiedPtPageMetadata& PathTraceUnifiedPtState::HistoryPageMetadata()
{
    return m_historyPageIndex == 0u ? m_page0Metadata : m_page1Metadata;
}

const PathTraceUnifiedPtPageMetadata& PathTraceUnifiedPtState::HistoryPageMetadata() const
{
    return m_historyPageIndex == 0u ? m_page0Metadata : m_page1Metadata;
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

bool PathTraceUnifiedPtState::EnsurePages(const PathTraceUnifiedPtDispatchInputs& inputs)
{
    const uint64_t count = uint64_t(inputs.width) * uint64_t(inputs.height);
    if (count == 0 || count > std::numeric_limits<uint32_t>::max() ||
        count > std::numeric_limits<uint64_t>::max() / UPT04_RESERVOIR_STRIDE)
    {
        return false;
    }
    const uint64_t bytes = count * UPT04_RESERVOIR_STRIDE;
    if (m_page0 && m_page1 && m_pageWidth == inputs.width && m_pageHeight == inputs.height &&
        m_page0->getDesc().structStride == UPT04_RESERVOIR_STRIDE &&
        m_page0->getDesc().byteSize >= bytes &&
        m_page1->getDesc().structStride == UPT04_RESERVOIR_STRIDE &&
        m_page1->getDesc().byteSize >= bytes)
    {
        return true;
    }

    // UPT-08 resources are extent-coupled and their per-page metadata follows
    // the physical reservoir roles. Reallocate them lazily only if the proof
    // cvar remains active; disabled means no retained correlation resources.
    ReleaseDuplication();

    nvrhi::BufferDesc desc;
    desc.debugName = "PathTraceUnifiedPtReservoirPage0";
    desc.byteSize = bytes;
    desc.structStride = UPT04_RESERVOIR_STRIDE;
    desc.canHaveUAVs = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    nvrhi::BufferHandle page0 = inputs.device->createBuffer(desc);
    desc.debugName = "PathTraceUnifiedPtReservoirPage1";
    nvrhi::BufferHandle page1 = inputs.device->createBuffer(desc);
    if (!page0 || !page1)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to allocate two-page history set (%ux%u, %llu bytes/page)\n",
            inputs.width,
            inputs.height,
            static_cast<unsigned long long>(bytes));
        return false;
    }

    m_page0 = page0;
    m_page1 = page1;
    m_page0Metadata.Invalidate();
    m_page1Metadata.Invalidate();
    m_currentPageIndex = 0u;
    m_historyPageIndex = 1u;
    m_pageWidth = inputs.width;
    m_pageHeight = inputs.height;
    m_pageBytes = bytes;
    // Allocation-only deterministic initialization protects partial proof
    // stages. Production D0 fully overwrites the page, including canonical
    // empty records. Never set this for camera cuts or history invalidation;
    // future reuse invalidates small per-page metadata instead.
    m_page0NeedsAllocationClear = true;
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_bindingSets[page] = nullptr;
        m_bindingSetDescValid[page] = false;
        m_temporalBindingSets[page] = nullptr;
        m_temporalBindingSetDescValid[page] = false;
        m_spatialBindingSets[page] = nullptr;
        m_spatialBindingSetDescValid[page] = false;
        m_resolveBindingSets[page] = nullptr;
        m_resolveBindingSetDescValid[page] = false;
    }
    common->Printf(
        "PathTraceUnifiedPt: allocated pages 0+1 %ux%u records/page=%llu bytes/page=%llu totalBytes=%llu stride=%u page1Clear=never\n",
        inputs.width,
        inputs.height,
        static_cast<unsigned long long>(count),
        static_cast<unsigned long long>(bytes),
        static_cast<unsigned long long>(bytes * 2ull),
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
    const bool traversalIsolationLayout =
        Upt04UsesTraversalIsolationLayout(pipelineVariant);
    const bool usesPushConstants = Upt04UsesPushConstants(pipelineVariant);
    const bool directOnlyProduction =
        Upt04UsesDirectOnlyProductionLayout(inputs);
    const bool usesBindlessSet = Upt04UsesBindlessSet(pipelineVariant) &&
        !directOnlyProduction;
    if (!m_selectionValid || m_backend != inputs.backend || m_family != inputs.family ||
        m_pipelineVariant != pipelineVariant || m_diagnostics != inputs.diagnostics ||
        m_primaryReceiverMode != inputs.primaryReceiverMode ||
        m_compactGeometry != inputs.compactGeometry ||
        m_compactLights != inputs.compactLights ||
        m_compactMaterials != inputs.compactMaterials ||
        m_splitInitial != inputs.splitInitial ||
        m_splitContinuation != inputs.splitContinuation)
    {
        ReleasePipeline();
        m_backend = inputs.backend;
        m_family = inputs.family;
        m_pipelineVariant = pipelineVariant;
        m_diagnostics = inputs.diagnostics;
        m_primaryReceiverMode = inputs.primaryReceiverMode;
        m_compactGeometry = inputs.compactGeometry;
        m_compactLights = inputs.compactLights;
        m_compactMaterials = inputs.compactMaterials;
        m_splitInitial = inputs.splitInitial;
        m_splitContinuation = inputs.splitContinuation;
        m_selectionValid = true;
        m_resourceFailureLogged = false;
    }

    if (((liveTlasProbe || m_backend == PathTraceUnifiedPtBackend::RayQuery) &&
            m_computePipeline &&
            (!inputs.splitInitial || m_splitIndirectComputePipeline)) ||
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
    else if (traversalIsolationLayout)
    {
        layoutDesc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(4));
        layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
            0, UPT04_PUSH_CONSTANT_BYTES));
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
        Upt04AddBindingLayoutItems(
            layoutDesc,
            inputs.diagnostics,
            inputs.splitContinuation);
    }
    m_bindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!m_bindingLayout)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create %s binding layout\n",
            compactLiveTlasProbe
                ? "compact live-TLAS probe"
                : (traversalIsolationLayout
                    ? "full-frame traversal-isolation"
                    : (minimalProductionSlotLayout
                    ? "minimal production-slot probe"
                    : (directOnlyProduction
                        ? "direct-only production"
                        : "24-descriptor"))));
        return false;
    }

    const char* initialPath = inputs.diagnostics
        ? Upt04DiagnosticShaderPath()
        : (liveTlasProbe
        ? Upt04LiveTlasProbePath(pipelineVariant)
        : (inputs.splitInitial
        ? Upt04SplitDirectShaderPath()
        : Upt04InitialShaderPath(
            m_backend,
            m_family,
            inputs.primaryReceiverMode,
            inputs.compactGeometry,
            inputs.compactLights,
            inputs.compactMaterials,
            inputs.splitContinuation)));
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
        int splitIndirectSize = 0;
        ID_TIME_T splitIndirectTimestamp = 0;
        uint64_t splitIndirectHash = 0;
        uint64_t splitIndirectPipelineUs = 0;
        if (inputs.splitInitial)
        {
            void* splitIndirectData = nullptr;
            if (!Upt04ReadShader(
                    Upt04SplitIndirectShaderPath(inputs.compactMaterials),
                    splitIndirectData,
                    splitIndirectSize,
                    splitIndirectTimestamp,
                    splitIndirectHash))
            {
                return false;
            }
            nvrhi::ShaderDesc splitShaderDesc;
            splitShaderDesc.shaderType = nvrhi::ShaderType::Compute;
            splitShaderDesc.entryName = "main";
            splitShaderDesc.debugName = "PathTraceUnifiedPtSplitIndirectRayQuery";
            m_splitIndirectComputeShader = inputs.device->createShader(
                splitShaderDesc,
                splitIndirectData,
                splitIndirectSize);
            Mem_Free(splitIndirectData);
            if (!m_splitIndirectComputeShader)
            {
                common->Printf(
                    "PathTraceUnifiedPt: failed to create split indirect RayQuery shader\n");
                return false;
            }
            pipelineDesc.CS = m_splitIndirectComputeShader;
            const uint64_t splitPipelineStartUs = Sys_Microseconds();
            m_splitIndirectComputePipeline =
                inputs.device->createComputePipeline(pipelineDesc);
            splitIndirectPipelineUs = Sys_Microseconds() - splitPipelineStartUs;
            if (!m_splitIndirectComputePipeline)
            {
                common->Printf(
                    "PathTraceUnifiedPt: failed to create split indirect RayQuery pipeline\n");
                return false;
            }
        }
        common->Printf(
            "PathTraceUnifiedPt: pipeline backend=%s family=%s variant=%u compiler=%s blobBytes=%d hash=%016llx timestamp=%lld groups=%s bindlessSet=%d receiver=%s geometry=%s lights=%s materials=%s split=%s continuation=%s payload=0 createUs=%llu deferredHost=0 driverCache=opaque\n",
            Upt04BackendName(m_backend),
            Upt04FamilyName(m_family),
            pipelineVariant,
            pipelineVariant == 8u ? "dxc" : "slang",
            initialSize,
            static_cast<unsigned long long>(initialHash),
            static_cast<long long>(initialTimestamp),
            traversalIsolationLayout || !liveTlasProbe ? "8x8" : "1x1",
            usesBindlessSet ? 1 : 0,
            Upt04ReceiverName(inputs.primaryReceiverMode),
            inputs.compactGeometry ? "compact48" : "legacy112",
            inputs.compactLights ? "compact64" : "legacy112",
            inputs.compactMaterials ? "compact48" : "legacy112",
            inputs.splitInitial ? "direct+indirect" : "monolithic",
            inputs.splitContinuation ? "split-hit32" : "inline",
            static_cast<unsigned long long>(pipelineUs));
        if (inputs.splitInitial)
        {
            common->Printf(
                "PathTraceUnifiedPt: split indirect compiler=slang blobBytes=%d hash=%016llx timestamp=%lld groups=8x8 intermediate=page0x64 exactM=1 createUs=%llu\n",
                splitIndirectSize,
                static_cast<unsigned long long>(splitIndirectHash),
                static_cast<long long>(splitIndirectTimestamp),
                static_cast<unsigned long long>(splitIndirectPipelineUs));
        }
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
        "PathTraceUnifiedPt: pipeline backend=%s family=%s receiver=%s raygenBytes=%d raygenHash=%016llx missBytes=%d missHash=%016llx hitBytes=%d hitHash=%016llx sbtHitGroups=3 payload=32 attribute=8 recursion=1 createUs=%llu deferredHost=0 driverCache=opaque\n",
        Upt04BackendName(m_backend),
        Upt04FamilyName(m_family),
        Upt04ReceiverName(inputs.primaryReceiverMode),
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
    const uint32_t pageIndex = m_currentPageIndex;
    const nvrhi::BufferHandle currentPage = CurrentPage();
    const bool compactLiveTlasProbe =
        Upt04UsesCompactProbeLayout(Upt04PipelineVariant(inputs));
    const bool minimalProductionSlotLayout =
        Upt04UsesMinimalProductionSlotLayout(Upt04PipelineVariant(inputs));
    const bool traversalIsolationLayout =
        Upt04UsesTraversalIsolationLayout(Upt04PipelineVariant(inputs));
    const bool usesPushConstants =
        Upt04UsesPushConstants(Upt04PipelineVariant(inputs));
    const bool directOnlyProduction =
        Upt04UsesDirectOnlyProductionLayout(inputs);
    nvrhi::BindingSetDesc desc;
    if (compactLiveTlasProbe)
    {
        desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(
            0, inputs.sceneInputs->geometry.tlas));
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(1, currentPage));
    }
    else if (traversalIsolationLayout)
    {
        desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(
            0, inputs.sceneInputs->geometry.tlas));
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            1, inputs.primarySurfaceBuffer));
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(4, currentPage));
        desc.addItem(nvrhi::BindingSetItem::PushConstants(
            0, UPT04_PUSH_CONSTANT_BYTES));
    }
    else if (minimalProductionSlotLayout)
    {
        desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(
            0, inputs.sceneInputs->geometry.tlas));
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(4, currentPage));
        if (usesPushConstants)
        {
            desc.addItem(nvrhi::BindingSetItem::PushConstants(
                0, UPT04_PUSH_CONSTANT_BYTES));
        }
    }
    else if (directOnlyProduction)
    {
        desc = Upt04BuildDirectBindingSetDesc(
            inputs,
            currentPage,
            m_compactStaticVertices,
            m_compactDynamicVertices,
            m_compactRigidVertices,
            m_compactSkinnedVertices,
            m_compactLightsBuffer);
    }
    else
    {
        desc = Upt04BuildBindingSetDesc(
            inputs,
            currentPage,
            m_diagnosticCounters,
            m_compactStaticVertices,
            m_compactDynamicVertices,
            m_compactRigidVertices,
            m_compactSkinnedVertices,
            m_compactLightsBuffer,
            m_compactMaterialsBuffer,
            m_continuationHits);
    }
    if (m_bindingSets[pageIndex] && m_bindingSetDescValid[pageIndex] &&
        m_bindingSetDescs[pageIndex] == desc)
    {
        return true;
    }
    m_bindingSets[pageIndex] = inputs.device->createBindingSet(desc, m_bindingLayout);
    if (!m_bindingSets[pageIndex])
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create %s set-0 binding set\n",
            compactLiveTlasProbe
                ? "compact live-TLAS probe"
                : (traversalIsolationLayout
                    ? "full-frame traversal-isolation"
                    : (minimalProductionSlotLayout
                    ? "minimal production-slot probe"
                    : (directOnlyProduction
                        ? "direct-only production"
                        : "UPT-04"))));
        return false;
    }
    m_bindingSetDescs[pageIndex] = desc;
    m_bindingSetDescValid[pageIndex] = true;
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

bool PathTraceUnifiedPtState::EnsureCompactGeometryResources(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactGeometry)
    {
        return true;
    }

    const RtPathTraceSceneInputGeometry& geometry = inputs.sceneInputs->geometry;
    const uint32_t requestedCounts[4] = {
        static_cast<uint32_t>(Max(0, geometry.staticVertexCount)),
        static_cast<uint32_t>(Max(0, geometry.dynamicVertexCount)),
        static_cast<uint32_t>(Max(0, geometry.rigidRouteVertexCount)),
        static_cast<uint32_t>(Max(0, geometry.skinnedGpuComputeVertexCount))
    };
    const char* debugNames[4] = {
        "PathTraceUnifiedPtCompactStaticVertices",
        "PathTraceUnifiedPtCompactDynamicVertices",
        "PathTraceUnifiedPtCompactRigidVertices",
        "PathTraceUnifiedPtCompactSkinnedVertices"
    };

    bool replaced = false;
    const auto ensureRouteBuffer = [&inputs, &replaced](
        nvrhi::BufferHandle& buffer,
        uint32_t& storedCapacity,
        uint32_t requestedCount,
        uint32_t route,
        const char* debugName)
    {
        const uint32_t capacity = Max(1u, requestedCount);
        const uint64_t bytes = uint64_t(capacity) * UPT04_COMPACT_VERTEX_STRIDE;
        if (buffer &&
            buffer->getDesc().structStride == UPT04_COMPACT_VERTEX_STRIDE &&
            buffer->getDesc().byteSize >= bytes)
        {
            return true;
        }
        nvrhi::BufferDesc desc;
        desc.debugName = debugName;
        desc.byteSize = bytes;
        desc.structStride = UPT04_COMPACT_VERTEX_STRIDE;
        desc.canHaveUAVs = true;
        desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
        desc.keepInitialState = true;
        buffer = inputs.device->createBuffer(desc);
        if (!buffer)
        {
            common->Printf(
                "PathTraceUnifiedPt: failed to allocate compact geometry route %u count=%u bytes=%llu\n",
                route,
                requestedCount,
                static_cast<unsigned long long>(bytes));
            return false;
        }
        storedCapacity = capacity;
        replaced = true;
        return true;
    };
    if (!ensureRouteBuffer(
            m_compactStaticVertices,
            m_compactStaticVertexCapacity,
            requestedCounts[0],
            0u,
            debugNames[0]) ||
        !ensureRouteBuffer(
            m_compactDynamicVertices,
            m_compactDynamicVertexCapacity,
            requestedCounts[1],
            1u,
            debugNames[1]) ||
        !ensureRouteBuffer(
            m_compactRigidVertices,
            m_compactRigidVertexCapacity,
            requestedCounts[2],
            2u,
            debugNames[2]) ||
        !ensureRouteBuffer(
            m_compactSkinnedVertices,
            m_compactSkinnedVertexCapacity,
            requestedCounts[3],
            3u,
            debugNames[3]))
    {
        return false;
    }
    if (replaced)
    {
        m_compactGeometryBindingSet = nullptr;
        m_compactGeometryBindingSetDescValid = false;
        for (uint32_t page = 0; page < 2u; ++page)
        {
            m_bindingSets[page] = nullptr;
            m_bindingSetDescValid[page] = false;
        }
        common->Printf(
            "PathTraceUnifiedPt: compact geometry sidecars stride=%u capacities=%u/%u/%u/%u bytes=%llu\n",
            UPT04_COMPACT_VERTEX_STRIDE,
            m_compactStaticVertexCapacity,
            m_compactDynamicVertexCapacity,
            m_compactRigidVertexCapacity,
            m_compactSkinnedVertexCapacity,
            static_cast<unsigned long long>(
                uint64_t(m_compactStaticVertexCapacity +
                    m_compactDynamicVertexCapacity +
                    m_compactRigidVertexCapacity +
                    m_compactSkinnedVertexCapacity) *
                UPT04_COMPACT_VERTEX_STRIDE));
    }
    return true;
}

bool PathTraceUnifiedPtState::EnsureCompactGeometryPipeline(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactGeometry || m_compactGeometryPipeline)
    {
        return true;
    }
    if (m_compactGeometryPipelineAttempted)
    {
        return false;
    }
    m_compactGeometryPipelineAttempted = true;

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);
    for (uint32_t slot = 0; slot < 4u; ++slot)
    {
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(slot));
    }
    for (uint32_t slot = 4; slot < 8u; ++slot)
    {
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(slot));
    }
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
        0, UPT04_COMPACT_GEOMETRY_PUSH_CONSTANT_BYTES));
    m_compactGeometryBindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!m_compactGeometryBindingLayout)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact geometry binding layout\n");
        return false;
    }

    void* shaderData = nullptr;
    int shaderSize = 0;
    ID_TIME_T shaderTimestamp = 0;
    uint64_t shaderHash = 0;
    if (!Upt04ReadShader(
            "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_compact_geometry_pack.bin",
            shaderData,
            shaderSize,
            shaderTimestamp,
            shaderHash))
    {
        return false;
    }
    nvrhi::ShaderDesc shaderDesc;
    shaderDesc.shaderType = nvrhi::ShaderType::Compute;
    shaderDesc.entryName = "main";
    shaderDesc.debugName = "PathTraceUnifiedPtCompactGeometryPack";
    m_compactGeometryShader = inputs.device->createShader(
        shaderDesc, shaderData, shaderSize);
    Mem_Free(shaderData);
    if (!m_compactGeometryShader)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact geometry shader\n");
        return false;
    }
    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = m_compactGeometryShader;
    pipelineDesc.bindingLayouts = { m_compactGeometryBindingLayout };
    const uint64_t pipelineStartUs = Sys_Microseconds();
    m_compactGeometryPipeline = inputs.device->createComputePipeline(pipelineDesc);
    const uint64_t pipelineUs = Sys_Microseconds() - pipelineStartUs;
    if (!m_compactGeometryPipeline)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact geometry pipeline\n");
        return false;
    }
    common->Printf(
        "PathTraceUnifiedPt: compact geometry pack compiler=slang blobBytes=%d hash=%016llx timestamp=%lld groups=128x1 stride=48 createUs=%llu\n",
        shaderSize,
        static_cast<unsigned long long>(shaderHash),
        static_cast<long long>(shaderTimestamp),
        static_cast<unsigned long long>(pipelineUs));
    return true;
}

bool PathTraceUnifiedPtState::EnsureCompactGeometryBindingSet(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactGeometry)
    {
        return true;
    }
    const RtPathTraceSceneInputs& scene = *inputs.sceneInputs;
    const RtPathTraceSceneInputGeometry& geometry = scene.geometry;
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, geometry.staticVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(1, geometry.dynamicVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, geometry.rigidRouteVertexBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, Upt04SkinnedVertexBuffer(scene)));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(4, m_compactStaticVertices));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(5, m_compactDynamicVertices));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(6, m_compactRigidVertices));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(7, m_compactSkinnedVertices));
    desc.addItem(nvrhi::BindingSetItem::PushConstants(
        0, UPT04_COMPACT_GEOMETRY_PUSH_CONSTANT_BYTES));
    if (m_compactGeometryBindingSet &&
        m_compactGeometryBindingSetDescValid &&
        m_compactGeometryBindingSetDesc == desc)
    {
        return true;
    }
    m_compactGeometryBindingSet = inputs.device->createBindingSet(
        desc, m_compactGeometryBindingLayout);
    if (!m_compactGeometryBindingSet)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact geometry binding set\n");
        return false;
    }
    m_compactGeometryBindingSetDesc = desc;
    m_compactGeometryBindingSetDescValid = true;
    return true;
}

bool PathTraceUnifiedPtState::ExecuteCompactGeometryPack(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactGeometry)
    {
        return true;
    }
    const RtPathTraceSceneInputs& scene = *inputs.sceneInputs;
    const RtPathTraceSceneInputGeometry& geometry = scene.geometry;
    const Upt04CompactGeometryPackControl control = {
        static_cast<uint32_t>(Max(0, geometry.staticVertexCount)),
        static_cast<uint32_t>(Max(0, geometry.dynamicVertexCount)),
        static_cast<uint32_t>(Max(0, geometry.rigidRouteVertexCount)),
        static_cast<uint32_t>(Max(0, geometry.skinnedGpuComputeVertexCount))
    };
    const uint32_t maxVertexCount = Max(
        Max(control.staticVertexCount, control.dynamicVertexCount),
        Max(control.rigidVertexCount, control.skinnedVertexCount));

    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.G0 CompactGeometry48 Bind+Barriers",
            inputs.nsightMarkers);
        inputs.commandList->setBufferState(
            geometry.staticVertexBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.dynamicVertexBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.rigidRouteVertexBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            Upt04SkinnedVertexBuffer(scene), nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            m_compactStaticVertices, nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->setBufferState(
            m_compactDynamicVertices, nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->setBufferState(
            m_compactRigidVertices, nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->setBufferState(
            m_compactSkinnedVertices, nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->commitBarriers();

        nvrhi::ComputeState state;
        state.pipeline = m_compactGeometryPipeline;
        state.bindings = { m_compactGeometryBindingSet };
        inputs.commandList->setComputeState(state);
        inputs.commandList->setPushConstants(&control, sizeof(control));
    }
    if (maxVertexCount > 0u)
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.G0 CompactGeometry48 Dispatch",
            inputs.nsightMarkers);
        inputs.commandList->dispatch((maxVertexCount + 127u) / 128u, 1u, 1u);
    }
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.G0 CompactGeometry48 OutputBarrier",
            inputs.nsightMarkers);
        nvrhi::utils::BufferUavBarrier(inputs.commandList, m_compactStaticVertices);
        nvrhi::utils::BufferUavBarrier(inputs.commandList, m_compactDynamicVertices);
        nvrhi::utils::BufferUavBarrier(inputs.commandList, m_compactRigidVertices);
        nvrhi::utils::BufferUavBarrier(inputs.commandList, m_compactSkinnedVertices);
        inputs.commandList->setBufferState(
            m_compactStaticVertices, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            m_compactDynamicVertices, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            m_compactRigidVertices, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            m_compactSkinnedVertices, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->commitBarriers();
    }
    return true;
}

bool PathTraceUnifiedPtState::EnsureCompactLightResources(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactLights)
    {
        return true;
    }
    const uint32_t requestedCount = static_cast<uint32_t>(Max(
        0, inputs.sceneInputs->lights.restirLightManagerCurrentPayloadCount));
    const uint32_t capacity = Max(1u, requestedCount);
    const uint64_t bytes = uint64_t(capacity) * UPT04_COMPACT_LIGHT_STRIDE;
    if (m_compactLightsBuffer &&
        m_compactLightsBuffer->getDesc().structStride == UPT04_COMPACT_LIGHT_STRIDE &&
        m_compactLightsBuffer->getDesc().byteSize >= bytes)
    {
        return true;
    }

    nvrhi::BufferDesc desc;
    desc.debugName = "PathTraceUnifiedPtCompactCurrentLights";
    desc.byteSize = bytes;
    desc.structStride = UPT04_COMPACT_LIGHT_STRIDE;
    desc.canHaveUAVs = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    m_compactLightsBuffer = inputs.device->createBuffer(desc);
    if (!m_compactLightsBuffer)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to allocate compact lights count=%u bytes=%llu\n",
            requestedCount,
            static_cast<unsigned long long>(bytes));
        return false;
    }
    m_compactLightCapacity = capacity;
    m_compactLightBindingSet = nullptr;
    m_compactLightBindingSetDescValid = false;
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_bindingSets[page] = nullptr;
        m_bindingSetDescValid[page] = false;
        m_temporalBindingSets[page] = nullptr;
        m_temporalBindingSetDescValid[page] = false;
        m_spatialBindingSets[page] = nullptr;
        m_spatialBindingSetDescValid[page] = false;
    }
    common->Printf(
        "PathTraceUnifiedPt: compact light sidecar stride=%u capacity=%u bytes=%llu\n",
        UPT04_COMPACT_LIGHT_STRIDE,
        capacity,
        static_cast<unsigned long long>(bytes));
    return true;
}

bool PathTraceUnifiedPtState::EnsureCompactLightPipeline(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactLights || m_compactLightPipeline)
    {
        return true;
    }
    if (m_compactLightPipelineAttempted)
    {
        return false;
    }
    m_compactLightPipelineAttempted = true;

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
        0, UPT04_COMPACT_LIGHT_PUSH_CONSTANT_BYTES));
    m_compactLightBindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!m_compactLightBindingLayout)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact light binding layout\n");
        return false;
    }

    void* shaderData = nullptr;
    int shaderSize = 0;
    ID_TIME_T shaderTimestamp = 0;
    uint64_t shaderHash = 0;
    if (!Upt04ReadShader(
            "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_compact_light_pack.bin",
            shaderData,
            shaderSize,
            shaderTimestamp,
            shaderHash))
    {
        return false;
    }
    nvrhi::ShaderDesc shaderDesc;
    shaderDesc.shaderType = nvrhi::ShaderType::Compute;
    shaderDesc.entryName = "main";
    shaderDesc.debugName = "PathTraceUnifiedPtCompactLightPack";
    m_compactLightShader = inputs.device->createShader(
        shaderDesc, shaderData, shaderSize);
    Mem_Free(shaderData);
    if (!m_compactLightShader)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact light shader\n");
        return false;
    }
    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = m_compactLightShader;
    pipelineDesc.bindingLayouts = { m_compactLightBindingLayout };
    const uint64_t pipelineStartUs = Sys_Microseconds();
    m_compactLightPipeline = inputs.device->createComputePipeline(pipelineDesc);
    const uint64_t pipelineUs = Sys_Microseconds() - pipelineStartUs;
    if (!m_compactLightPipeline)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact light pipeline\n");
        return false;
    }
    common->Printf(
        "PathTraceUnifiedPt: compact light pack compiler=slang blobBytes=%d hash=%016llx timestamp=%lld groups=128x1 stride=64 createUs=%llu\n",
        shaderSize,
        static_cast<unsigned long long>(shaderHash),
        static_cast<long long>(shaderTimestamp),
        static_cast<unsigned long long>(pipelineUs));
    return true;
}

bool PathTraceUnifiedPtState::EnsureCompactLightBindingSet(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactLights)
    {
        return true;
    }
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        0, inputs.sceneInputs->lights.restirLightManagerCurrentPayloadBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
        1, m_compactLightsBuffer));
    desc.addItem(nvrhi::BindingSetItem::PushConstants(
        0, UPT04_COMPACT_LIGHT_PUSH_CONSTANT_BYTES));
    if (m_compactLightBindingSet && m_compactLightBindingSetDescValid &&
        m_compactLightBindingSetDesc == desc)
    {
        return true;
    }
    m_compactLightBindingSet = inputs.device->createBindingSet(
        desc, m_compactLightBindingLayout);
    if (!m_compactLightBindingSet)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact light binding set\n");
        return false;
    }
    m_compactLightBindingSetDesc = desc;
    m_compactLightBindingSetDescValid = true;
    return true;
}

bool PathTraceUnifiedPtState::ExecuteCompactLightPack(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactLights)
    {
        return true;
    }
    const uint32_t lightCount = static_cast<uint32_t>(Max(
        0, inputs.sceneInputs->lights.restirLightManagerCurrentPayloadCount));
    const Upt04CompactLightPackControl control = { lightCount };
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.L0 CompactLight64 Bind+Barriers",
            inputs.nsightMarkers);
        inputs.commandList->setBufferState(
            inputs.sceneInputs->lights.restirLightManagerCurrentPayloadBuffer,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            m_compactLightsBuffer, nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->commitBarriers();
        nvrhi::ComputeState state;
        state.pipeline = m_compactLightPipeline;
        state.bindings = { m_compactLightBindingSet };
        inputs.commandList->setComputeState(state);
        inputs.commandList->setPushConstants(&control, sizeof(control));
    }
    if (lightCount > 0u)
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.L0 CompactLight64 Dispatch",
            inputs.nsightMarkers);
        inputs.commandList->dispatch((lightCount + 127u) / 128u, 1u, 1u);
    }
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.L0 CompactLight64 OutputBarrier",
            inputs.nsightMarkers);
        nvrhi::utils::BufferUavBarrier(inputs.commandList, m_compactLightsBuffer);
        inputs.commandList->setBufferState(
            m_compactLightsBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->commitBarriers();
    }
    return true;
}

bool PathTraceUnifiedPtState::EnsureCompactMaterialResources(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactMaterials)
    {
        return true;
    }
    const uint32_t requestedCount = static_cast<uint32_t>(Max(
        0, inputs.sceneInputs->materials.materialTableEntryCount));
    const uint32_t capacity = Max(1u, requestedCount);
    const uint64_t bytes = uint64_t(capacity) * UPT04_COMPACT_MATERIAL_STRIDE;
    if (m_compactMaterialsBuffer &&
        m_compactMaterialsBuffer->getDesc().structStride == UPT04_COMPACT_MATERIAL_STRIDE &&
        m_compactMaterialsBuffer->getDesc().byteSize >= bytes)
    {
        return true;
    }

    nvrhi::BufferDesc desc;
    desc.debugName = "PathTraceUnifiedPtCompactMaterials";
    desc.byteSize = bytes;
    desc.structStride = UPT04_COMPACT_MATERIAL_STRIDE;
    desc.canHaveUAVs = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    m_compactMaterialsBuffer = inputs.device->createBuffer(desc);
    if (!m_compactMaterialsBuffer)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to allocate compact materials count=%u bytes=%llu\n",
            requestedCount,
            static_cast<unsigned long long>(bytes));
        return false;
    }
    m_compactMaterialCapacity = capacity;
    m_compactMaterialBindingSet = nullptr;
    m_compactMaterialBindingSetDescValid = false;
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_bindingSets[page] = nullptr;
        m_bindingSetDescValid[page] = false;
    }
    common->Printf(
        "PathTraceUnifiedPt: compact material sidecar stride=%u capacity=%u bytes=%llu\n",
        UPT04_COMPACT_MATERIAL_STRIDE,
        capacity,
        static_cast<unsigned long long>(bytes));
    return true;
}

bool PathTraceUnifiedPtState::EnsureCompactMaterialPipeline(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactMaterials || m_compactMaterialPipeline)
    {
        return true;
    }
    if (m_compactMaterialPipelineAttempted)
    {
        return false;
    }
    m_compactMaterialPipelineAttempted = true;

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
        0, UPT04_COMPACT_MATERIAL_PUSH_CONSTANT_BYTES));
    m_compactMaterialBindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!m_compactMaterialBindingLayout)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact material binding layout\n");
        return false;
    }

    void* shaderData = nullptr;
    int shaderSize = 0;
    ID_TIME_T shaderTimestamp = 0;
    uint64_t shaderHash = 0;
    if (!Upt04ReadShader(
            "renderprogs2/spirv/builtin/pathtracing/slang_upt04/upt04_compact_material_pack.bin",
            shaderData,
            shaderSize,
            shaderTimestamp,
            shaderHash))
    {
        return false;
    }
    nvrhi::ShaderDesc shaderDesc;
    shaderDesc.shaderType = nvrhi::ShaderType::Compute;
    shaderDesc.entryName = "main";
    shaderDesc.debugName = "PathTraceUnifiedPtCompactMaterialPack";
    m_compactMaterialShader = inputs.device->createShader(
        shaderDesc, shaderData, shaderSize);
    Mem_Free(shaderData);
    if (!m_compactMaterialShader)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact material shader\n");
        return false;
    }
    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = m_compactMaterialShader;
    pipelineDesc.bindingLayouts = { m_compactMaterialBindingLayout };
    const uint64_t pipelineStartUs = Sys_Microseconds();
    m_compactMaterialPipeline = inputs.device->createComputePipeline(pipelineDesc);
    const uint64_t pipelineUs = Sys_Microseconds() - pipelineStartUs;
    if (!m_compactMaterialPipeline)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact material pipeline\n");
        return false;
    }
    common->Printf(
        "PathTraceUnifiedPt: compact material pack compiler=slang blobBytes=%d hash=%016llx timestamp=%lld groups=128x1 stride=48 createUs=%llu\n",
        shaderSize,
        static_cast<unsigned long long>(shaderHash),
        static_cast<long long>(shaderTimestamp),
        static_cast<unsigned long long>(pipelineUs));
    return true;
}

bool PathTraceUnifiedPtState::EnsureCompactMaterialBindingSet(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactMaterials)
    {
        return true;
    }
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        0, inputs.sceneInputs->materials.materialTableBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
        1, m_compactMaterialsBuffer));
    desc.addItem(nvrhi::BindingSetItem::PushConstants(
        0, UPT04_COMPACT_MATERIAL_PUSH_CONSTANT_BYTES));
    if (m_compactMaterialBindingSet && m_compactMaterialBindingSetDescValid &&
        m_compactMaterialBindingSetDesc == desc)
    {
        return true;
    }
    m_compactMaterialBindingSet = inputs.device->createBindingSet(
        desc, m_compactMaterialBindingLayout);
    if (!m_compactMaterialBindingSet)
    {
        common->Printf("PathTraceUnifiedPt: failed to create compact material binding set\n");
        return false;
    }
    m_compactMaterialBindingSetDesc = desc;
    m_compactMaterialBindingSetDescValid = true;
    return true;
}

bool PathTraceUnifiedPtState::ExecuteCompactMaterialPack(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactMaterials)
    {
        return true;
    }
    const uint32_t materialCount = static_cast<uint32_t>(Max(
        0, inputs.sceneInputs->materials.materialTableEntryCount));
    const Upt04CompactMaterialPackControl control = { materialCount };
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.M0 CompactMaterial48 Bind+Barriers",
            inputs.nsightMarkers);
        inputs.commandList->setBufferState(
            inputs.sceneInputs->materials.materialTableBuffer,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            m_compactMaterialsBuffer, nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->commitBarriers();
        nvrhi::ComputeState state;
        state.pipeline = m_compactMaterialPipeline;
        state.bindings = { m_compactMaterialBindingSet };
        inputs.commandList->setComputeState(state);
        inputs.commandList->setPushConstants(&control, sizeof(control));
    }
    if (materialCount > 0u)
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.M0 CompactMaterial48 Dispatch",
            inputs.nsightMarkers);
        inputs.commandList->dispatch((materialCount + 127u) / 128u, 1u, 1u);
    }
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.M0 CompactMaterial48 OutputBarrier",
            inputs.nsightMarkers);
        nvrhi::utils::BufferUavBarrier(inputs.commandList, m_compactMaterialsBuffer);
        inputs.commandList->setBufferState(
            m_compactMaterialsBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->commitBarriers();
    }
    return true;
}

bool PathTraceUnifiedPtState::EnsureContinuationResources(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.splitContinuation)
    {
        return true;
    }
    const uint64_t requestedCount64 = uint64_t(inputs.width) * uint64_t(inputs.height);
    if (requestedCount64 == 0u || requestedCount64 > UINT32_MAX)
    {
        common->Printf(
            "PathTraceUnifiedPt: invalid continuation-hit capacity=%llu\n",
            static_cast<unsigned long long>(requestedCount64));
        return false;
    }
    const uint32_t capacity = static_cast<uint32_t>(requestedCount64);
    const uint64_t bytes = requestedCount64 * UPT04_CONTINUATION_HIT_STRIDE;
    if (m_continuationHits &&
        m_continuationHits->getDesc().structStride == UPT04_CONTINUATION_HIT_STRIDE &&
        m_continuationHits->getDesc().byteSize >= bytes)
    {
        return true;
    }

    nvrhi::BufferDesc desc;
    desc.debugName = "PathTraceUnifiedPtContinuationHits";
    desc.byteSize = bytes;
    desc.structStride = UPT04_CONTINUATION_HIT_STRIDE;
    desc.canHaveUAVs = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    m_continuationHits = inputs.device->createBuffer(desc);
    if (!m_continuationHits)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to allocate continuation hits count=%u bytes=%llu\n",
            capacity,
            static_cast<unsigned long long>(bytes));
        return false;
    }
    m_continuationCapacity = capacity;
    m_continuationBindingSet = nullptr;
    m_continuationBindingSetDescValid = false;
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_bindingSets[page] = nullptr;
        m_bindingSetDescValid[page] = false;
    }
    common->Printf(
        "PathTraceUnifiedPt: continuation-hit sidecar stride=%u capacity=%u bytes=%llu\n",
        UPT04_CONTINUATION_HIT_STRIDE,
        capacity,
        static_cast<unsigned long long>(bytes));
    return true;
}

bool PathTraceUnifiedPtState::EnsureContinuationPipeline(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.splitContinuation || m_continuationPipeline)
    {
        return true;
    }
    if (m_continuationPipelineAttempted)
    {
        return false;
    }
    m_continuationPipelineAttempted = true;

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(24));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
        0, UPT04_PUSH_CONSTANT_BYTES));
    m_continuationBindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!m_continuationBindingLayout)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create continuation-trace binding layout\n");
        return false;
    }

    void* shaderData = nullptr;
    int shaderSize = 0;
    ID_TIME_T shaderTimestamp = 0;
    uint64_t shaderHash = 0;
    if (!Upt04ReadShader(
            Upt04ContinuationTraceShaderPath(),
            shaderData,
            shaderSize,
            shaderTimestamp,
            shaderHash))
    {
        return false;
    }
    nvrhi::ShaderDesc shaderDesc;
    shaderDesc.shaderType = nvrhi::ShaderType::Compute;
    shaderDesc.entryName = "main";
    shaderDesc.debugName = "PathTraceUnifiedPtContinuationTraceRayQuery";
    m_continuationShader = inputs.device->createShader(
        shaderDesc, shaderData, shaderSize);
    Mem_Free(shaderData);
    if (!m_continuationShader)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create continuation-trace shader\n");
        return false;
    }
    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = m_continuationShader;
    pipelineDesc.bindingLayouts = { m_continuationBindingLayout };
    const uint64_t pipelineStartUs = Sys_Microseconds();
    m_continuationPipeline = inputs.device->createComputePipeline(pipelineDesc);
    const uint64_t pipelineUs = Sys_Microseconds() - pipelineStartUs;
    if (!m_continuationPipeline)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create continuation-trace pipeline\n");
        return false;
    }
    common->Printf(
        "PathTraceUnifiedPt: continuation trace compiler=slang blobBytes=%d hash=%016llx timestamp=%lld groups=8x8 hitStride=32 queries=1 createUs=%llu\n",
        shaderSize,
        static_cast<unsigned long long>(shaderHash),
        static_cast<long long>(shaderTimestamp),
        static_cast<unsigned long long>(pipelineUs));
    return true;
}

bool PathTraceUnifiedPtState::EnsureContinuationBindingSet(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.splitContinuation)
    {
        return true;
    }
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(
        0, inputs.sceneInputs->geometry.tlas));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        1, inputs.primarySurfaceBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
        24, m_continuationHits));
    desc.addItem(nvrhi::BindingSetItem::PushConstants(
        0, UPT04_PUSH_CONSTANT_BYTES));
    if (m_continuationBindingSet && m_continuationBindingSetDescValid &&
        m_continuationBindingSetDesc == desc)
    {
        return true;
    }
    m_continuationBindingSet = inputs.device->createBindingSet(
        desc, m_continuationBindingLayout);
    if (!m_continuationBindingSet)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create continuation-trace binding set\n");
        return false;
    }
    m_continuationBindingSetDesc = desc;
    m_continuationBindingSetDescValid = true;
    return true;
}

bool PathTraceUnifiedPtState::ExecuteContinuationTrace(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.splitContinuation)
    {
        return true;
    }
    const Upt04InitialControl control = Upt04BuildControl(inputs);
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.C0 ContinuationTrace32 Bind+Barriers",
            inputs.nsightMarkers);
        inputs.commandList->setAccelStructState(
            inputs.sceneInputs->geometry.tlas,
            nvrhi::ResourceStates::AccelStructRead);
        inputs.commandList->setBufferState(
            inputs.primarySurfaceBuffer,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            m_continuationHits,
            nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->commitBarriers();
        nvrhi::ComputeState state;
        state.pipeline = m_continuationPipeline;
        state.bindings = { m_continuationBindingSet };
        inputs.commandList->setComputeState(state);
        inputs.commandList->setPushConstants(&control, sizeof(control));
    }
    {
        const bool oneGroup = inputs.proofStage == 7u;
        const bool oneGroupRow = inputs.proofStage == 8u;
        const char* markerName = oneGroup
            ? "UPT.C0 ContinuationTrace32 RayQuery 8x8"
            : (oneGroupRow
                ? "UPT.C0 ContinuationTrace32 RayQuery OneGroupRow"
                : "UPT.C0 ContinuationTrace32 RayQuery FullFrame");
        Upt04MarkerScope marker(
            inputs.commandList, markerName, inputs.nsightMarkers);
        inputs.commandList->dispatch(
            oneGroup ? 1u : (inputs.width + 7u) / 8u,
            oneGroup || oneGroupRow ? 1u : (inputs.height + 7u) / 8u,
            1u);
    }
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.C0 ContinuationTrace32 OutputBarrier",
            inputs.nsightMarkers);
        nvrhi::utils::BufferUavBarrier(inputs.commandList, m_continuationHits);
        inputs.commandList->setBufferState(
            m_continuationHits,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->commitBarriers();
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
        "PathTraceUnifiedPt: diagnostic receivers(valid/invalid)=%u/%u candidates(direct invalid/zero/positive)=%u/%u/%u candidates(indirect invalid/zero/positive)=%u/%u/%u selected(primaryNee/bsdfEndpoint/secondaryNee)=%u/%u/%u rays(continuation/visibility)=%u/%u canonicalEmpty=%u candidateInputs=%u rayCeilingViolations=%u reservoirSignature=%08x:%08x:%08x:%08x directReject(selection/record/emissiveReplay/analyticSample/material/pdf/other)=%u/%u/%u/%u/%u/%u/%u sampleIndex=%u family=%s size=%ux%u\n",
        counters[0], counters[1], counters[2], counters[3], counters[4],
        counters[5], counters[6], counters[7], counters[8], counters[9],
        counters[10], counters[11], counters[12], counters[13], counters[14],
        counters[15], counters[16], counters[17], counters[18], counters[19],
        counters[20], counters[21], counters[22], counters[23], counters[24],
        counters[25], counters[26],
        m_diagnosticReadbackSampleIndex,
        Upt04FamilyName(m_diagnosticReadbackFamily),
        m_diagnosticReadbackWidth,
        m_diagnosticReadbackHeight);
    inputs.device->unmapBuffer(m_diagnosticReadback);
    m_diagnosticReadbackPending = false;
}

bool PathTraceUnifiedPtState::ExecuteInitial(const PathTraceUnifiedPtDispatchInputs& inputs)
{
    // Presentation is admitted per frame only after the matching UPT-05
    // resolve completes; never expose a previous frame after an early return.
    m_resolveReady = false;
    m_initialPublishedThisFrame = false;
    m_spatialExecutedThisFrame = false;
    DrainDiagnosticReadback(inputs);
    if (!inputs.duplication && m_duplicationSampleIds)
    {
        ReleaseDuplication();
    }
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
    if (inputs.historyEpoch == 0)
    {
        if (!m_resourceFailureLogged)
        {
            common->Printf(
                "PathTraceUnifiedPt: missing full-width history epoch; dispatch skipped\n");
            m_resourceFailureLogged = true;
        }
        return false;
    }
    if (m_observedHistoryEpoch != inputs.historyEpoch)
    {
        m_page0Metadata.Invalidate();
        m_page1Metadata.Invalidate();
        m_duplicationMetadata[0].Invalidate();
        m_duplicationMetadata[1].Invalidate();
        m_reportedTemporalHistoryAvailable = -1;
        m_currentPageIndex = 0u;
        m_historyPageIndex = 1u;
        m_observedHistoryEpoch = inputs.historyEpoch;
        common->Printf(
            "PathTraceUnifiedPt: history metadata invalidated epoch=%llu reasons=0x%08x pageClear=none\n",
            static_cast<unsigned long long>(inputs.historyEpoch),
            inputs.historyResetReasonFlags);
    }
    if (m_temporalModeActive != inputs.temporal ||
        m_spatialModeActive != inputs.spatial)
    {
        m_temporalModeActive = inputs.temporal;
        m_spatialModeActive = inputs.spatial;
        m_currentPageIndex = 0u;
        m_historyPageIndex = 1u;
        m_page0Metadata.Invalidate();
        m_page1Metadata.Invalidate();
        m_duplicationMetadata[0].Invalidate();
        m_duplicationMetadata[1].Invalidate();
        m_reportedTemporalHistoryAvailable = -1;
        common->Printf(
            "PathTraceUnifiedPt: reuse mode temporal=%d spatial=%d pageRoles=current0/history1 historyInvalidated=1 pageClear=none\n",
            inputs.temporal ? 1 : 0,
            inputs.spatial ? 1 : 0);
    }
    if (!EnsurePages(inputs))
    {
        return false;
    }
    if (!EnsureDiagnosticBuffers(inputs))
    {
        return false;
    }
    if (!inputs.splitContinuation && m_continuationHits)
    {
        ReleaseContinuation();
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
    if (inputs.compactGeometry &&
        (!EnsureCompactGeometryResources(inputs) ||
         !EnsureCompactGeometryPipeline(inputs) ||
         !EnsureCompactGeometryBindingSet(inputs)))
    {
        return false;
    }
    if (inputs.compactLights &&
        (!EnsureCompactLightResources(inputs) ||
         !EnsureCompactLightPipeline(inputs) ||
         !EnsureCompactLightBindingSet(inputs)))
    {
        return false;
    }
    if (inputs.compactMaterials &&
        (!EnsureCompactMaterialResources(inputs) ||
         !EnsureCompactMaterialPipeline(inputs) ||
         !EnsureCompactMaterialBindingSet(inputs)))
    {
        return false;
    }
    if (inputs.splitContinuation &&
        (!EnsureContinuationResources(inputs) ||
         !EnsureContinuationPipeline(inputs) ||
         !EnsureContinuationBindingSet(inputs)))
    {
        return false;
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
    if (!ExecuteCompactGeometryPack(inputs))
    {
        return false;
    }
    if (!ExecuteCompactLightPack(inputs))
    {
        return false;
    }
    if (!ExecuteCompactMaterialPack(inputs))
    {
        return false;
    }
    if (!ExecuteContinuationTrace(inputs))
    {
        return false;
    }
    m_resourceFailureLogged = false;

    const bool liveTlasProbe = Upt04PipelineVariant(inputs) != 0u;
    const bool compactLiveTlasProbe =
        Upt04UsesCompactProbeLayout(Upt04PipelineVariant(inputs));
    const bool minimalProductionSlotLayout =
        Upt04UsesMinimalProductionSlotLayout(Upt04PipelineVariant(inputs));
    const bool traversalIsolationLayout =
        Upt04UsesTraversalIsolationLayout(Upt04PipelineVariant(inputs));
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
        else if (traversalIsolationLayout)
        {
            inputs.commandList->setAccelStructState(
                inputs.sceneInputs->geometry.tlas,
                nvrhi::ResourceStates::AccelStructRead);
            inputs.commandList->setBufferState(
                inputs.primarySurfaceBuffer,
                nvrhi::ResourceStates::ShaderResource);
        }
        else if (directOnlyProduction)
        {
            Upt04SetDirectSrvStates(inputs.commandList, inputs);
        }
        else
        {
            Upt04SetSrvStates(inputs.commandList, inputs);
        }
        if (inputs.splitContinuation)
        {
            inputs.commandList->setBufferState(
                m_continuationHits,
                nvrhi::ResourceStates::ShaderResource);
        }
        inputs.commandList->setBufferState(
            CurrentPage(), nvrhi::ResourceStates::UnorderedAccess);
        if (inputs.diagnostics)
        {
            inputs.commandList->setBufferState(
                m_diagnosticCounters, nvrhi::ResourceStates::UnorderedAccess);
        }
        inputs.commandList->commitBarriers();
        if (m_page0NeedsAllocationClear)
        {
            // One-shot allocation clear, not a per-frame reservoir operation.
            inputs.commandList->clearBufferUInt(CurrentPage(), 0u);
            nvrhi::utils::BufferUavBarrier(inputs.commandList, CurrentPage());
            m_page0NeedsAllocationClear = false;
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
            state.bindings = { m_bindingSets[m_currentPageIndex] };
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
            state.bindings = { m_bindingSets[m_currentPageIndex] };
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
            ? (inputs.splitInitial
                ? (oneGroup
                    ? "UPT.D0a Split Direct RayQuery 8x8"
                    : (oneGroupRow
                        ? "UPT.D0a Split Direct RayQuery OneGroupRow"
                        : "UPT.D0a Split Direct RayQuery FullFrame"))
                : (inputs.splitContinuation
                    ? (oneGroup
                        ? "UPT.D0 ContinuationShade RayQuery 8x8"
                        : (oneGroupRow
                            ? "UPT.D0 ContinuationShade RayQuery OneGroupRow"
                            : "UPT.D0 ContinuationShade RayQuery FullFrame"))
                    : (oneGroup
                        ? oneGroupRayQueryMarker
                        : (oneGroupRow
                            ? "UPT.D0 Initial RayQuery Dispatch OneGroupRow"
                            : "UPT.D0 Initial RayQuery Dispatch FullFrame"))))
            : (oneGroup
                ? "UPT.D0 Initial RayGen DispatchRays 8x8"
                : (oneGroupRow
                    ? "UPT.D0 Initial RayGen DispatchRays OneGroupRow"
                    : "UPT.D0 Initial RayGen DispatchRays FullFrame")));
        Upt04MarkerScope marker(inputs.commandList, markerName, inputs.nsightMarkers);
        if (liveTlasProbe && !traversalIsolationLayout)
        {
            inputs.commandList->dispatch(1u, 1u, 1u);
        }
        else if (traversalIsolationLayout ||
            m_backend == PathTraceUnifiedPtBackend::RayQuery)
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

    if (inputs.splitInitial)
    {
        const bool oneGroup = inputs.proofStage == 7u;
        const bool oneGroupRow = inputs.proofStage == 8u;
        {
            Upt04MarkerScope marker(
                inputs.commandList,
                "UPT.D0 Split Intermediate Reservoir Barrier",
                inputs.nsightMarkers);
            nvrhi::utils::BufferUavBarrier(inputs.commandList, CurrentPage());
            inputs.commandList->commitBarriers();
            nvrhi::ComputeState state;
            state.pipeline = m_splitIndirectComputePipeline;
            state.bindings = { m_bindingSets[m_currentPageIndex] };
            state.bindings.push_back(
                inputs.sceneInputs->materials.textureDescriptorTable);
            inputs.commandList->setComputeState(state);
            inputs.commandList->setPushConstants(&control, sizeof(control));
        }
        {
            const char* markerName = oneGroup
                ? "UPT.D0b Split Indirect RayQuery 8x8"
                : (oneGroupRow
                    ? "UPT.D0b Split Indirect RayQuery OneGroupRow"
                    : "UPT.D0b Split Indirect RayQuery FullFrame");
            Upt04MarkerScope marker(
                inputs.commandList,
                markerName,
                inputs.nsightMarkers);
            inputs.commandList->dispatch(
                oneGroup ? 1u : (inputs.width + 7u) / 8u,
                oneGroup || oneGroupRow ? 1u : (inputs.height + 7u) / 8u,
                1u);
        }
    }

    {
        Upt04MarkerScope marker(
            inputs.commandList,
            "UPT.D0 Initial OutputBarrier",
            inputs.nsightMarkers);
        nvrhi::utils::BufferUavBarrier(inputs.commandList, CurrentPage());
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
            m_diagnosticReadbackSampleIndex = inputs.frameSampleIndex;
            m_diagnosticReadbackWidth = inputs.width;
            m_diagnosticReadbackHeight = inputs.height;
            m_diagnosticReadbackFamily = inputs.family;
        }
    }
    const bool productionFullFrame = inputs.proofStage >= 9u &&
        Upt04PipelineVariant(inputs) == 0u;
    if (productionFullFrame)
    {
        const uint32_t enabledFamilyMask = Upt04FamilyMask(inputs.family);
        const uint32_t specializationIdentity =
            static_cast<uint32_t>(inputs.family) |
            (static_cast<uint32_t>(inputs.backend) << 8u);
        const uint64_t contentGeneration = Upt06BuildContentGeneration(
            inputs,
            enabledFamilyMask,
            specializationIdentity);
        PathTraceUnifiedPtPageMetadata& currentMetadata = CurrentPageMetadata();
        const bool firstPublicationForEpoch = !currentMetadata.fullyWritten ||
            currentMetadata.historyEpoch != inputs.historyEpoch;
        currentMetadata.fullyWritten = true;
        currentMetadata.width = inputs.width;
        currentMetadata.height = inputs.height;
        currentMetadata.contentGeneration = contentGeneration;
        currentMetadata.historyEpoch = inputs.historyEpoch;
        currentMetadata.frameSerial = ++m_lastPublishedFrameSerial;
        // D0 is a complete current-frame publication even when a later reuse
        // pass cannot run. CompleteFrame must still promote it alongside the
        // primary-surface history or the next T0 dispatch would pair an N-2
        // reservoir with the N-1 surface published by SmokeDispatch.
        m_initialPublishedThisFrame = true;
        if (firstPublicationForEpoch)
        {
            common->Printf(
                "PathTraceUnifiedPt: page metadata current=%u(full/generation/epoch/serial)=%d/%016llx/%llu/%llu history=%u(full/serial)=%d/%llu clears(allocation/invalidation)=1/0 pages=2\n",
                m_currentPageIndex,
                currentMetadata.fullyWritten ? 1 : 0,
                static_cast<unsigned long long>(contentGeneration),
                static_cast<unsigned long long>(inputs.historyEpoch),
                static_cast<unsigned long long>(currentMetadata.frameSerial),
                m_historyPageIndex,
                HistoryPageMetadata().fullyWritten ? 1 : 0,
                static_cast<unsigned long long>(
                    HistoryPageMetadata().frameSerial));
        }
    }
    if (inputs.proofStage == 7u)
    {
        ReportProofStage(
            7u,
            liveTlasProbe && !traversalIsolationLayout
                ? "one-invocation-live-tlas-probe"
                : "one-8x8-group",
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

bool PathTraceUnifiedPtState::EnsureDuplicationResources(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.duplication)
        return false;
    const uint64_t surfaceCount = uint64_t(inputs.width) * uint64_t(inputs.height);
    const uint32_t packedPitch = (inputs.width + 3u) / 4u;
    const uint64_t packedCount = uint64_t(packedPitch) * uint64_t(inputs.height);
    if (surfaceCount == 0u || surfaceCount > std::numeric_limits<uint32_t>::max()
        || packedCount > std::numeric_limits<uint32_t>::max())
        return false;
    if (m_duplicationSampleIds && m_duplicationScores[0]
        && m_duplicationScores[1] && m_duplicationWidth == inputs.width
        && m_duplicationHeight == inputs.height
        && m_duplicationPackedRowPitch == packedPitch)
        return true;

    ReleaseDuplication();
    nvrhi::BufferDesc desc;
    desc.debugName = "PathTraceUnifiedPtSampleIds";
    desc.byteSize = surfaceCount * sizeof(uint32_t);
    desc.structStride = sizeof(uint32_t);
    desc.canHaveUAVs = true;
    desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    desc.keepInitialState = true;
    m_duplicationSampleIds = inputs.device->createBuffer(desc);
    desc.byteSize = packedCount * sizeof(uint32_t);
    desc.debugName = "PathTraceUnifiedPtDuplicationPage0";
    m_duplicationScores[0] = inputs.device->createBuffer(desc);
    desc.debugName = "PathTraceUnifiedPtDuplicationPage1";
    m_duplicationScores[1] = inputs.device->createBuffer(desc);
    if (!m_duplicationSampleIds || !m_duplicationScores[0]
        || !m_duplicationScores[1])
    {
        common->Printf("PathTraceUnifiedPt: failed to allocate UPT-08 correlation resources\n");
        ReleaseDuplication();
        return false;
    }
    m_duplicationWidth = inputs.width;
    m_duplicationHeight = inputs.height;
    m_duplicationPackedRowPitch = packedPitch;
    common->Printf(
        "PathTraceUnifiedPt: allocated UPT-08 sampleIds=%llu duplicationPages=%llu totalBytes=%llu format=R32+packedUNORM8 clear=never\n",
        static_cast<unsigned long long>(surfaceCount * 4ull),
        static_cast<unsigned long long>(packedCount * 8ull),
        static_cast<unsigned long long>(surfaceCount * 4ull + packedCount * 8ull));
    return true;
}

bool PathTraceUnifiedPtState::EnsureDuplicationPipeline(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (m_duplicationFillPipeline && m_duplicationComputePipeline)
        return true;
    if (m_duplicationPipelineAttempted)
        return false;
    m_duplicationPipelineAttempted = true;

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
        0, UPT08_PUSH_CONSTANT_BYTES));
    m_duplicationBindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!m_duplicationBindingLayout)
        return false;

    const char* paths[2] = {
        "renderprogs2/spirv/builtin/pathtracing/slang_upt08/upt08_fill_sample_id.bin",
        "renderprogs2/spirv/builtin/pathtracing/slang_upt08/upt08_compute_duplication.bin"
    };
    for (uint32_t pass = 0; pass < 2u; ++pass)
    {
        void* data = nullptr;
        int size = 0;
        ID_TIME_T timestamp = 0;
        uint64_t hash = 0;
        if (!Upt04ReadShader(paths[pass], data, size, timestamp, hash))
            return false;
        nvrhi::ShaderDesc shaderDesc;
        shaderDesc.shaderType = nvrhi::ShaderType::Compute;
        shaderDesc.entryName = "main";
        shaderDesc.debugName = pass == 0u
            ? "PathTraceUnifiedPtFillSampleId"
            : "PathTraceUnifiedPtComputeDuplication";
        nvrhi::ShaderHandle shader = inputs.device->createShader(
            shaderDesc, data, size);
        Mem_Free(data);
        if (!shader)
            return false;
        nvrhi::ComputePipelineDesc pipelineDesc;
        pipelineDesc.CS = shader;
        pipelineDesc.bindingLayouts = { m_duplicationBindingLayout };
        nvrhi::ComputePipelineHandle pipeline =
            inputs.device->createComputePipeline(pipelineDesc);
        if (!pipeline)
            return false;
        if (pass == 0u)
        {
            m_duplicationFillShader = shader;
            m_duplicationFillPipeline = pipeline;
        }
        else
        {
            m_duplicationComputeShader = shader;
            m_duplicationComputePipeline = pipeline;
        }
    }
    return true;
}

bool PathTraceUnifiedPtState::EnsureDuplicationBindingSets(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!m_duplicationSampleIds || !m_duplicationScores[0]
        || !m_duplicationScores[1] || !m_page0 || !m_page1)
        return false;
    for (uint32_t page = 0; page < 2u; ++page)
    {
        if (!m_duplicationFillBindingSets[page])
        {
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
                0, page == 0u ? m_page0 : m_page1));
            desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
                1, m_duplicationSampleIds));
            desc.addItem(nvrhi::BindingSetItem::PushConstants(
                0, UPT08_PUSH_CONSTANT_BYTES));
            m_duplicationFillBindingSets[page] = inputs.device->createBindingSet(
                desc, m_duplicationBindingLayout);
        }
        if (!m_duplicationComputeBindingSets[page])
        {
            nvrhi::BindingSetDesc desc;
            desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
                0, m_duplicationSampleIds));
            desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
                1, m_duplicationScores[page]));
            desc.addItem(nvrhi::BindingSetItem::PushConstants(
                0, UPT08_PUSH_CONSTANT_BYTES));
            m_duplicationComputeBindingSets[page] = inputs.device->createBindingSet(
                desc, m_duplicationBindingLayout);
        }
        if (!m_duplicationFillBindingSets[page]
            || !m_duplicationComputeBindingSets[page])
            return false;
    }
    return true;
}

bool PathTraceUnifiedPtState::EnsureTemporalPipeline(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (m_temporalPipeline && m_temporalCompactLights == inputs.compactLights
        && m_temporalDuplication == inputs.duplication)
    {
        return true;
    }
    if (m_temporalCompactLights != inputs.compactLights
        || m_temporalDuplication != inputs.duplication)
    {
        ReleaseTemporal();
        m_temporalCompactLights = inputs.compactLights;
        m_temporalDuplication = inputs.duplication;
    }
    if (m_temporalPipelineAttempted)
    {
        return false;
    }
    m_temporalPipelineAttempted = true;

    if (!inputs.device->queryFeatureSupport(nvrhi::Feature::RayQuery))
    {
        common->Printf(
            "PathTraceUnifiedPt: UPT-07 temporal requires RayQuery support; skipped\n");
        return false;
    }

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5));
    for (const uint32_t slot : { 6u, 7u, 8u, 10u, 11u, 12u, 14u,
            15u, 16u, 17u, 18u, 19u, 20u, 21u, 23u })
    {
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(slot));
    }
    if (inputs.duplication)
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(24));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(25));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(26));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
        0, UPT07_PUSH_CONSTANT_BYTES));
    m_temporalBindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!m_temporalBindingLayout)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create UPT-07 temporal binding layout\n");
        return false;
    }

    const char* path = inputs.duplication
        ? (inputs.compactLights
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_direct_rayquery_light64_duplication.bin"
            : "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_direct_rayquery_duplication.bin")
        : (inputs.compactLights
            ? "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_direct_rayquery_light64.bin"
            : "renderprogs2/spirv/builtin/pathtracing/slang_upt07/upt07_temporal_direct_rayquery.bin");
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
    shaderDesc.debugName = "PathTraceUnifiedPtTemporalDirect";
    m_temporalShader = inputs.device->createShader(shaderDesc, data, size);
    Mem_Free(data);
    if (!m_temporalShader)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create UPT-07 temporal shader\n");
        return false;
    }

    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = m_temporalShader;
    pipelineDesc.bindingLayouts = { m_temporalBindingLayout };
    const uint64_t pipelineStartUs = Sys_Microseconds();
    m_temporalPipeline = inputs.device->createComputePipeline(pipelineDesc);
    const uint64_t pipelineUs = Sys_Microseconds() - pipelineStartUs;
    if (!m_temporalPipeline)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create UPT-07 temporal pipeline\n");
        return false;
    }
    common->Printf(
        "PathTraceUnifiedPt: temporal compiler=slang blobBytes=%d hash=%016llx timestamp=%lld groups=8x8 lightStride=%u historyTaps=9 duplication=%u visibilityRaysMax=1 visibility=winner-only family=direct-basic createUs=%llu\n",
        size,
        static_cast<unsigned long long>(hash),
        static_cast<long long>(timestamp),
        inputs.compactLights ? UPT04_COMPACT_LIGHT_STRIDE : 112u,
        inputs.duplication ? 1u : 0u,
        static_cast<unsigned long long>(pipelineUs));
    return true;
}

bool PathTraceUnifiedPtState::EnsureTemporalBindingSet(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactPrimaryHistory ||
        !inputs.primarySurfaceCurrentBuffer ||
        !inputs.primarySurfacePreviousBuffer ||
        !inputs.primaryHistorySidecarCurrentBuffer ||
        !inputs.primaryHistorySidecarPreviousBuffer ||
        inputs.primarySurfaceCurrentBuffer->getDesc().structStride !=
            PATH_TRACE_UNIFIED_PT_PRIMARY_RECEIVER32_STRIDE ||
        inputs.primarySurfacePreviousBuffer->getDesc().structStride !=
            PATH_TRACE_UNIFIED_PT_PRIMARY_RECEIVER32_STRIDE ||
        inputs.primaryHistorySidecarCurrentBuffer->getDesc().structStride !=
            PATH_TRACE_UNIFIED_PT_PRIMARY_HISTORY_SIDECAR_STRIDE ||
        inputs.primaryHistorySidecarPreviousBuffer->getDesc().structStride !=
            PATH_TRACE_UNIFIED_PT_PRIMARY_HISTORY_SIDECAR_STRIDE)
    {
        common->Printf(
            "PathTraceUnifiedPt: UPT-07 temporal requires compact current/previous 32-byte receivers plus 32-byte history sidecars\n");
        return false;
    }
    const nvrhi::BufferHandle lightBuffer = inputs.compactLights
        ? m_compactLightsBuffer
        : inputs.sceneInputs->lights.restirLightManagerCurrentPayloadBuffer;
    const RtPathTraceSceneInputGeometry& geometry = inputs.sceneInputs->geometry;
    const RtPathTraceSceneInputLights& lights = inputs.sceneInputs->lights;
    if (!lightBuffer || !CurrentPage() || !HistoryPage() || !geometry.tlas
        || (inputs.duplication && !m_duplicationScores[m_historyPageIndex])
        || !lights.restirLightManagerPreviousToCurrentBuffer
        || !lights.emissiveTriangleBuffer
        || !geometry.staticVertexBuffer || !geometry.staticIndexBuffer
        || !geometry.staticTriangleMaterialIndexBuffer
        || !geometry.dynamicVertexBuffer || !geometry.dynamicIndexBuffer
        || !geometry.dynamicTriangleMaterialIndexBuffer
        || !geometry.rigidRouteVertexBuffer || !geometry.rigidRouteIndexBuffer
        || !geometry.rigidRouteInstanceBuffer
        || !Upt04SkinnedVertexBuffer(*inputs.sceneInputs)
        || !Upt04SkinnedIndexBuffer(*inputs.sceneInputs)
        || !geometry.skinnedHitRouteRecordBuffer
        || !geometry.skinnedHitRouteTriangleBuffer)
    {
        return false;
    }

    const uint32_t pageIndex = m_currentPageIndex;
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(
        0, geometry.tlas));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        1, inputs.primarySurfaceCurrentBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        2, inputs.primarySurfacePreviousBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(3, CurrentPage()));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, HistoryPage()));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(5, lightBuffer));
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
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(18, Upt04SkinnedVertexBuffer(*inputs.sceneInputs)));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(19, Upt04SkinnedIndexBuffer(*inputs.sceneInputs)));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(20, geometry.skinnedHitRouteRecordBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(21, geometry.skinnedHitRouteTriangleBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        23, lights.restirLightManagerPreviousToCurrentBuffer));
    if (inputs.duplication)
        desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
            24, m_duplicationScores[m_historyPageIndex]));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        25, inputs.primaryHistorySidecarCurrentBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        26, inputs.primaryHistorySidecarPreviousBuffer));
    desc.addItem(nvrhi::BindingSetItem::PushConstants(
        0, UPT07_PUSH_CONSTANT_BYTES));
    if (m_temporalBindingSets[pageIndex] &&
        m_temporalBindingSetDescValid[pageIndex] &&
        m_temporalBindingSetDescs[pageIndex] == desc)
    {
        return true;
    }
    m_temporalBindingSets[pageIndex] = inputs.device->createBindingSet(
        desc, m_temporalBindingLayout);
    if (!m_temporalBindingSets[pageIndex])
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create UPT-07 temporal binding set\n");
        return false;
    }
    m_temporalBindingSetDescs[pageIndex] = desc;
    m_temporalBindingSetDescValid[pageIndex] = true;
    return true;
}

bool PathTraceUnifiedPtState::ExecuteTemporal(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.temporal)
    {
        m_reportedTemporalSkipReason = -1;
        return false;
    }
    const auto reportSkip = [&](int32_t reason, const char* label) -> bool
    {
        if (m_reportedTemporalSkipReason != reason)
        {
            const PathTraceUnifiedPtPageMetadata& current =
                CurrentPageMetadata();
            const PathTraceUnifiedPtPageMetadata& history =
                HistoryPageMetadata();
            common->Printf(
                "PathTraceUnifiedPt: temporal skipped reason=%s sampleIndex=%u currentPage=%u serial=%llu historyPage=%u serial=%llu\n",
                label,
                inputs.frameSampleIndex,
                m_currentPageIndex,
                static_cast<unsigned long long>(current.frameSerial),
                m_historyPageIndex,
                static_cast<unsigned long long>(history.frameSerial));
            m_reportedTemporalSkipReason = reason;
        }
        return false;
    };
    const bool productionFullFrame = inputs.proofStage >= 9u &&
        Upt04PipelineVariant(inputs) == 0u;
    const PathTraceUnifiedPtPageMetadata& currentMetadata = CurrentPageMetadata();
    if (!productionFullFrame)
        return reportSkip(1, "not-production-full-frame");
    if (!currentMetadata.fullyWritten)
        return reportSkip(2, "current-page-not-published");
    if (currentMetadata.width != inputs.width ||
        currentMetadata.height != inputs.height)
        return reportSkip(3, "current-page-extent-mismatch");
    if (currentMetadata.historyEpoch != inputs.historyEpoch)
        return reportSkip(4, "current-page-epoch-mismatch");
    if (inputs.duplication && !EnsureDuplicationResources(inputs))
        return reportSkip(8, "duplication-resources-unavailable");
    if (!EnsureTemporalPipeline(inputs))
        return reportSkip(5, "pipeline-unavailable");
    if (!EnsureTemporalBindingSet(inputs))
        return reportSkip(6, "binding-set-unavailable");

    const PathTraceUnifiedPtPageMetadata& historyMetadata = HistoryPageMetadata();
    const bool historyAvailable = inputs.primarySurfaceHistoryValid &&
        historyMetadata.fullyWritten &&
        historyMetadata.width == currentMetadata.width &&
        historyMetadata.height == currentMetadata.height &&
        historyMetadata.contentGeneration == currentMetadata.contentGeneration &&
        historyMetadata.historyEpoch == currentMetadata.historyEpoch &&
        currentMetadata.frameSerial > 1u &&
        historyMetadata.frameSerial == currentMetadata.frameSerial - 1u;
    const PathTraceUnifiedPtPageMetadata& duplicationMetadata =
        m_duplicationMetadata[m_historyPageIndex];
    const bool duplicationAvailable = inputs.duplication && historyAvailable
        && duplicationMetadata.fullyWritten
        && duplicationMetadata.width == historyMetadata.width
        && duplicationMetadata.height == historyMetadata.height
        && duplicationMetadata.contentGeneration == historyMetadata.contentGeneration
        && duplicationMetadata.historyEpoch == historyMetadata.historyEpoch
        && duplicationMetadata.frameSerial == historyMetadata.frameSerial;
    if (m_reportedTemporalHistoryAvailable != (historyAvailable ? 1 : 0))
    {
        common->Printf(
            "PathTraceUnifiedPt: temporal historyAvailable=%d currentPage=%u historyPage=%u pageGeneration=%016llx epoch=%llu serial(current/history)=%llu/%llu\n",
            historyAvailable ? 1 : 0,
            m_currentPageIndex,
            m_historyPageIndex,
            static_cast<unsigned long long>(currentMetadata.contentGeneration),
            static_cast<unsigned long long>(currentMetadata.historyEpoch),
            static_cast<unsigned long long>(currentMetadata.frameSerial),
            static_cast<unsigned long long>(historyMetadata.frameSerial));
        m_reportedTemporalHistoryAvailable = historyAvailable ? 1 : 0;
    }
    const RtPathTraceSceneInputLights& lights = inputs.sceneInputs->lights;
    const RtPathTraceSceneInputGeometry& geometry = inputs.sceneInputs->geometry;
    const uint64_t surfaceCount64 = uint64_t(inputs.width) * uint64_t(inputs.height);
    if (surfaceCount64 > std::numeric_limits<uint32_t>::max())
    {
        return reportSkip(7, "surface-count-overflow");
    }

    Upt07TemporalDirectControl control = {};
    control.renderWidth = inputs.width;
    control.renderHeight = inputs.height;
    control.surfaceCount = static_cast<uint32_t>(surfaceCount64);
    control.historyAvailable = historyAvailable ? 1u : 0u;
    control.emissiveRangeStart = lights.restirLightManagerEmissiveRangeOffset;
    control.emissiveRangeCount = lights.restirLightManagerEmissiveRangeCount;
    control.analyticRangeStart = lights.restirLightManagerDoomAnalyticRangeOffset;
    control.analyticRangeCount = lights.restirLightManagerDoomAnalyticSampleableCount;
    control.currentLightCount = static_cast<uint32_t>(Max(
        0, lights.restirLightManagerCurrentPayloadCount));
    control.frameSampleIndex = inputs.frameSampleIndex;
    control.maximumHistoryM = static_cast<uint32_t>(idMath::ClampInt(
        1,
        1024,
        r_pathTracingUnifiedPtTemporalMaxHistoryM.GetInteger()));
    control.maximumHistoryAge = static_cast<uint32_t>(idMath::ClampInt(
        1,
        static_cast<int>(UPT07_MAXIMUM_HISTORY_AGE),
        r_pathTracingUnifiedPtTemporalMaxAge.GetInteger()));
    for (uint32_t axis = 0; axis < 3u; ++axis)
    {
        control.previousCameraOrigin[axis] = inputs.previousCameraOrigin[axis];
        control.previousCameraForward[axis] = inputs.previousCameraForward[axis];
        control.previousCameraLeft[axis] = inputs.previousCameraLeft[axis];
        control.previousCameraUp[axis] = inputs.previousCameraUp[axis];
    }
    control.previousCameraValid = historyAvailable ? 1u : 0u;
    control.previousCameraTanX = inputs.previousCameraTanX;
    control.previousCameraTanY = inputs.previousCameraTanY;
    control.previousCameraHistorySearchMode = static_cast<uint32_t>(
        idMath::ClampInt(
            0, 2, r_pathTracingUnifiedPtTemporalSearch.GetInteger()));
    control.geometryAvailabilityFlags =
        (geometry.staticBucketRoutePublicationValid
            ? UPT04_ROUTE_STATIC_BUCKETS : 0u)
        | (r_pathTracingUnifiedPtTemporalPreviousBest.GetBool()
            ? UPT07_GEOMETRY_FLAG_PREVIOUS_BEST_SEED : 0u)
        | (r_pathTracingUnifiedPtTemporalPairwise.GetBool()
            ? UPT07_GEOMETRY_FLAG_PAIRWISE_MIS : 0u)
        | (duplicationAvailable
            ? UPT07_GEOMETRY_FLAG_DUPLICATION_MAP : 0u)
        | (r_pathTracingUnifiedPtDirectTargetPdfParity.GetBool()
            ? UPT04_DIRECT_TARGET_PDF_PARITY : 0u)
        | (r_pathTracingUnifiedPtAnalyticPortalDomain.GetBool()
            ? UPT04_ANALYTIC_PORTAL_DOMAIN : 0u);
    control.emissiveReplayCount = static_cast<uint32_t>(Max(
        0, lights.emissiveTriangleCount));
    control.previousToCurrentLightCount = static_cast<uint32_t>(Max(
        0, lights.restirLightManagerPreviousToCurrentCount));
    control.maximumHistoryContributionRatio =
        UPT07_MAXIMUM_HISTORY_CONTRIBUTION_RATIO;
    for (uint32_t axis = 0; axis < 3u; ++axis)
        control.primaryCameraOrigin[axis] = inputs.primaryCameraOrigin[axis];
    control.compactPrimaryHistory = inputs.compactPrimaryHistory ? 1u : 0u;

    const nvrhi::BufferHandle lightBuffer = inputs.compactLights
        ? m_compactLightsBuffer
        : lights.restirLightManagerCurrentPayloadBuffer;
    {
        Upt04MarkerScope marker(
            inputs.commandList,
            historyAvailable
                ? (duplicationAvailable
                    ? "UPT.T0 Temporal Direct Shift AdaptiveDupCap"
                    : "UPT.T0 Temporal Direct Shift History")
                : "UPT.T0 Temporal Direct Shift NoHistory",
            inputs.nsightMarkers);
        inputs.commandList->setAccelStructState(
            inputs.sceneInputs->geometry.tlas,
            nvrhi::ResourceStates::AccelStructRead);
        inputs.commandList->setBufferState(
            inputs.primarySurfaceCurrentBuffer,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            inputs.primarySurfacePreviousBuffer,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            inputs.primaryHistorySidecarCurrentBuffer,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            inputs.primaryHistorySidecarPreviousBuffer,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            CurrentPage(), nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->setBufferState(
            HistoryPage(), nvrhi::ResourceStates::ShaderResource);
        if (inputs.duplication)
            inputs.commandList->setBufferState(
                m_duplicationScores[m_historyPageIndex],
                nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            lightBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            lights.restirLightManagerPreviousToCurrentBuffer,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            lights.emissiveTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.staticVertexBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.staticIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.staticTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.dynamicVertexBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.dynamicIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.dynamicTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.rigidRouteVertexBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.rigidRouteIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.rigidRouteInstanceBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            Upt04SkinnedVertexBuffer(*inputs.sceneInputs), nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            Upt04SkinnedIndexBuffer(*inputs.sceneInputs), nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.skinnedHitRouteRecordBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.skinnedHitRouteTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->commitBarriers();

        nvrhi::ComputeState state;
        state.pipeline = m_temporalPipeline;
        state.bindings = { m_temporalBindingSets[m_currentPageIndex] };
        inputs.commandList->setComputeState(state);
        inputs.commandList->setPushConstants(&control, sizeof(control));
        inputs.commandList->dispatch(
            (inputs.width + 7u) / 8u,
            (inputs.height + 7u) / 8u,
            1u);
        nvrhi::utils::BufferUavBarrier(inputs.commandList, CurrentPage());
    }
    m_reportedTemporalSkipReason = -1;
    return true;
}

bool PathTraceUnifiedPtState::EnsureSpatialPipeline(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (m_spatialPipeline && m_spatialCompactLights == inputs.compactLights)
    {
        return true;
    }
    if (m_spatialCompactLights != inputs.compactLights)
    {
        ReleaseSpatial();
        m_spatialCompactLights = inputs.compactLights;
    }
    if (m_spatialPipelineAttempted)
    {
        return false;
    }
    m_spatialPipelineAttempted = true;

    if (!inputs.device->queryFeatureSupport(nvrhi::Feature::RayQuery))
    {
        common->Printf(
            "PathTraceUnifiedPt: UPT-09 spatial requires RayQuery support; skipped\n");
        return false;
    }

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Compute;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setSamplerOffset(0)
        .setUnorderedAccessViewOffset(0);
    layoutDesc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(3));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
    layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5));
    for (const uint32_t slot : { 6u, 7u, 8u, 10u, 11u, 12u, 14u,
            15u, 16u, 17u, 18u, 19u, 20u, 21u })
    {
        layoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(slot));
    }
    layoutDesc.addItem(nvrhi::BindingLayoutItem::PushConstants(
        0, UPT09_PUSH_CONSTANT_BYTES));
    m_spatialBindingLayout = inputs.device->createBindingLayout(layoutDesc);
    if (!m_spatialBindingLayout)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create UPT-09 spatial binding layout\n");
        return false;
    }

    const char* path = inputs.compactLights
        ? "renderprogs2/spirv/builtin/pathtracing/slang_upt09/upt09_spatial_direct_rayquery_light64.bin"
        : "renderprogs2/spirv/builtin/pathtracing/slang_upt09/upt09_spatial_direct_rayquery.bin";
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
    shaderDesc.debugName = "PathTraceUnifiedPtSpatialDirect";
    m_spatialShader = inputs.device->createShader(shaderDesc, data, size);
    Mem_Free(data);
    if (!m_spatialShader)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create UPT-09 spatial shader\n");
        return false;
    }

    nvrhi::ComputePipelineDesc pipelineDesc;
    pipelineDesc.CS = m_spatialShader;
    pipelineDesc.bindingLayouts = { m_spatialBindingLayout };
    const uint64_t pipelineStartUs = Sys_Microseconds();
    m_spatialPipeline = inputs.device->createComputePipeline(pipelineDesc);
    const uint64_t pipelineUs = Sys_Microseconds() - pipelineStartUs;
    if (!m_spatialPipeline)
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create UPT-09 spatial pipeline\n");
        return false;
    }
    common->Printf(
        "PathTraceUnifiedPt: spatial compiler=slang blobBytes=%d hash=%016llx timestamp=%lld groups=8x8 lightStride=%u attempts=%u/%u radius=%.1f visibilityRaysMax=1 createUs=%llu\n",
        size,
        static_cast<unsigned long long>(hash),
        static_cast<long long>(timestamp),
        inputs.compactLights ? UPT04_COMPACT_LIGHT_STRIDE : 112u,
        UPT09_REGULAR_NEIGHBOR_COUNT,
        UPT09_RESCUE_NEIGHBOR_COUNT,
        UPT09_NEIGHBOR_RADIUS,
        static_cast<unsigned long long>(pipelineUs));
    return true;
}

bool PathTraceUnifiedPtState::EnsureSpatialBindingSet(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.compactPrimaryHistory ||
        !inputs.primarySurfaceCurrentBuffer ||
        !inputs.primaryHistorySidecarCurrentBuffer ||
        inputs.primarySurfaceCurrentBuffer->getDesc().structStride !=
            PATH_TRACE_UNIFIED_PT_PRIMARY_RECEIVER32_STRIDE ||
        inputs.primaryHistorySidecarCurrentBuffer->getDesc().structStride !=
            PATH_TRACE_UNIFIED_PT_PRIMARY_HISTORY_SIDECAR_STRIDE)
    {
        common->Printf(
            "PathTraceUnifiedPt: UPT-09 spatial requires a compact 32-byte current receiver plus 32-byte history sidecar\n");
        return false;
    }
    const nvrhi::BufferHandle lightBuffer = inputs.compactLights
        ? m_compactLightsBuffer
        : inputs.sceneInputs->lights.restirLightManagerCurrentPayloadBuffer;
    const RtPathTraceSceneInputGeometry& geometry = inputs.sceneInputs->geometry;
    const RtPathTraceSceneInputLights& lights = inputs.sceneInputs->lights;
    if (!lightBuffer || !CurrentPage() || !HistoryPage()
        || !lights.emissiveTriangleBuffer
        || !geometry.staticVertexBuffer || !geometry.staticIndexBuffer
        || !geometry.staticTriangleMaterialIndexBuffer
        || !geometry.dynamicVertexBuffer || !geometry.dynamicIndexBuffer
        || !geometry.dynamicTriangleMaterialIndexBuffer
        || !geometry.rigidRouteVertexBuffer || !geometry.rigidRouteIndexBuffer
        || !geometry.rigidRouteInstanceBuffer
        || !Upt04SkinnedVertexBuffer(*inputs.sceneInputs)
        || !Upt04SkinnedIndexBuffer(*inputs.sceneInputs)
        || !geometry.skinnedHitRouteRecordBuffer
        || !geometry.skinnedHitRouteTriangleBuffer)
    {
        return false;
    }

    const uint32_t pageIndex = m_currentPageIndex;
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(
        0, inputs.sceneInputs->geometry.tlas));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        1, inputs.primarySurfaceCurrentBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(2, CurrentPage()));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(3, HistoryPage()));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, lightBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(
        5, inputs.primaryHistorySidecarCurrentBuffer));
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
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(18, Upt04SkinnedVertexBuffer(*inputs.sceneInputs)));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(19, Upt04SkinnedIndexBuffer(*inputs.sceneInputs)));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(20, geometry.skinnedHitRouteRecordBuffer));
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(21, geometry.skinnedHitRouteTriangleBuffer));
    desc.addItem(nvrhi::BindingSetItem::PushConstants(
        0, UPT09_PUSH_CONSTANT_BYTES));
    if (m_spatialBindingSets[pageIndex] &&
        m_spatialBindingSetDescValid[pageIndex] &&
        m_spatialBindingSetDescs[pageIndex] == desc)
    {
        return true;
    }
    m_spatialBindingSets[pageIndex] = inputs.device->createBindingSet(
        desc, m_spatialBindingLayout);
    if (!m_spatialBindingSets[pageIndex])
    {
        common->Printf(
            "PathTraceUnifiedPt: failed to create UPT-09 spatial binding set\n");
        return false;
    }
    m_spatialBindingSetDescs[pageIndex] = desc;
    m_spatialBindingSetDescValid[pageIndex] = true;
    return true;
}

bool PathTraceUnifiedPtState::ExecuteSpatial(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    m_spatialExecutedThisFrame = false;
    if (!inputs.spatial)
    {
        return false;
    }
    if (inputs.family != PathTraceUnifiedPtFamily::DirectOnly)
    {
        common->Printf(
            "PathTraceUnifiedPt: UPT-09 spatial baseline is direct-only; dispatch skipped for family=%s\n",
            Upt04FamilyName(inputs.family));
        return false;
    }
    const bool productionFullFrame = inputs.proofStage >= 9u &&
        Upt04PipelineVariant(inputs) == 0u;
    const PathTraceUnifiedPtPageMetadata& currentMetadata = CurrentPageMetadata();
    if (!productionFullFrame || !currentMetadata.fullyWritten ||
        currentMetadata.width != inputs.width ||
        currentMetadata.height != inputs.height ||
        currentMetadata.historyEpoch != inputs.historyEpoch ||
        !EnsureSpatialPipeline(inputs) || !EnsureSpatialBindingSet(inputs))
    {
        return false;
    }

    const RtPathTraceSceneInputLights& lights = inputs.sceneInputs->lights;
    const RtPathTraceSceneInputGeometry& geometry = inputs.sceneInputs->geometry;
    const uint64_t surfaceCount64 = uint64_t(inputs.width) * uint64_t(inputs.height);
    if (surfaceCount64 > std::numeric_limits<uint32_t>::max())
    {
        return false;
    }
    Upt09SpatialDirectControl control = {};
    control.renderWidth = inputs.width;
    control.renderHeight = inputs.height;
    control.surfaceCount = static_cast<uint32_t>(surfaceCount64);
    control.frameSampleIndex = inputs.frameSampleIndex;
    control.emissiveRangeStart = lights.restirLightManagerEmissiveRangeOffset;
    control.emissiveRangeCount = lights.restirLightManagerEmissiveRangeCount;
    control.analyticRangeStart = lights.restirLightManagerDoomAnalyticRangeOffset;
    control.analyticRangeCount = lights.restirLightManagerDoomAnalyticSampleableCount;
    control.currentLightCount = static_cast<uint32_t>(Max(
        0, lights.restirLightManagerCurrentPayloadCount));
    control.maximumInputM = UPT09_MAXIMUM_INPUT_M;
    control.regularNeighborCount = UPT09_REGULAR_NEIGHBOR_COUNT;
    control.rescueNeighborCount = UPT09_RESCUE_NEIGHBOR_COUNT;
    control.neighborRadius = UPT09_NEIGHBOR_RADIUS;
    control.geometryAvailabilityFlags =
        (geometry.staticBucketRoutePublicationValid
            ? UPT04_ROUTE_STATIC_BUCKETS : 0u)
        | (r_pathTracingUnifiedPtDirectTargetPdfParity.GetBool()
            ? UPT04_DIRECT_TARGET_PDF_PARITY : 0u)
        | (r_pathTracingUnifiedPtAnalyticPortalDomain.GetBool()
            ? UPT04_ANALYTIC_PORTAL_DOMAIN : 0u);
    control.emissiveReplayCount = static_cast<uint32_t>(Max(
        0, lights.emissiveTriangleCount));
    control.proofMode = static_cast<uint32_t>(idMath::ClampInt(
        0, 6, r_pathTracingUnifiedPtSpatialProofMode.GetInteger()));
    for (uint32_t axis = 0; axis < 3u; ++axis)
        control.primaryCameraOrigin[axis] = inputs.primaryCameraOrigin[axis];
    control.compactPrimaryHistory = inputs.compactPrimaryHistory ? 1u : 0u;

    const nvrhi::BufferHandle lightBuffer = inputs.compactLights
        ? m_compactLightsBuffer
        : lights.restirLightManagerCurrentPayloadBuffer;
    {
        const char* spatialMarker = "UPT.S0 Spatial Unique BasicCorrection";
        if (control.proofMode == 1u)
            spatialMarker = "UPT.S0 Proof PassThrough";
        else if (control.proofMode == 2u)
            spatialMarker = "UPT.S0 Proof WithReplacement";
        else if (control.proofMode == 3u)
            spatialMarker = "UPT.S0 Proof Unique Standard1OverM";
        else if (control.proofMode == 4u)
            spatialMarker = "UPT.S0 Proof Legacy SelectedOnly";
        else if (control.proofMode == 5u)
            spatialMarker = "UPT.S0 Proof Fresh SourceTarget";
        else if (control.proofMode == 6u)
            spatialMarker = "UPT.S0 Proof PairwiseMIS";
        Upt04MarkerScope marker(
            inputs.commandList,
            spatialMarker,
            inputs.nsightMarkers);
        inputs.commandList->setAccelStructState(
            inputs.sceneInputs->geometry.tlas,
            nvrhi::ResourceStates::AccelStructRead);
        inputs.commandList->setBufferState(
            inputs.primarySurfaceCurrentBuffer,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            inputs.primaryHistorySidecarCurrentBuffer,
            nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            CurrentPage(), nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            HistoryPage(), nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->setBufferState(
            lightBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            lights.emissiveTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.staticVertexBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.staticIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.staticTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.dynamicVertexBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.dynamicIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.dynamicTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.rigidRouteVertexBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.rigidRouteIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.rigidRouteInstanceBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            Upt04SkinnedVertexBuffer(*inputs.sceneInputs), nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            Upt04SkinnedIndexBuffer(*inputs.sceneInputs), nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.skinnedHitRouteRecordBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            geometry.skinnedHitRouteTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->commitBarriers();

        nvrhi::ComputeState state;
        state.pipeline = m_spatialPipeline;
        state.bindings = { m_spatialBindingSets[m_currentPageIndex] };
        inputs.commandList->setComputeState(state);
        inputs.commandList->setPushConstants(&control, sizeof(control));
        inputs.commandList->dispatch(
            (inputs.width + 7u) / 8u,
            (inputs.height + 7u) / 8u,
            1u);
        nvrhi::utils::BufferUavBarrier(inputs.commandList, HistoryPage());
    }
    HistoryPageMetadata() = currentMetadata;
    HistoryPageMetadata().fullyWritten = true;
    m_spatialExecutedThisFrame = true;
    return true;
}

bool PathTraceUnifiedPtState::ExecuteDuplication(
    const PathTraceUnifiedPtDispatchInputs& inputs)
{
    if (!inputs.duplication || inputs.proofStage < 9u
        || Upt04PipelineVariant(inputs) != 0u
        || !EnsureDuplicationResources(inputs)
        || !EnsureDuplicationPipeline(inputs)
        || !EnsureDuplicationBindingSets(inputs))
        return false;

    // S0 publishes into the physical history page. Without S0, D0/T0 leaves
    // the final current-frame reservoir in the physical current page. Publish
    // the correlation map against that exact physical page so CompleteFrame's
    // role transition keeps reservoir and duplication histories aligned.
    const uint32_t finalPageIndex = m_spatialExecutedThisFrame
        ? m_historyPageIndex : m_currentPageIndex;
    const nvrhi::BufferHandle finalPage = finalPageIndex == 0u
        ? m_page0 : m_page1;
    const PathTraceUnifiedPtPageMetadata& finalMetadata = finalPageIndex == 0u
        ? m_page0Metadata : m_page1Metadata;
    if (!finalMetadata.fullyWritten || finalMetadata.width != inputs.width
        || finalMetadata.height != inputs.height
        || finalMetadata.historyEpoch != inputs.historyEpoch)
        return false;

    const uint64_t surfaceCount64 = uint64_t(inputs.width) * uint64_t(inputs.height);
    if (surfaceCount64 > std::numeric_limits<uint32_t>::max())
        return false;
    Upt08Control control = {};
    control.renderWidth = inputs.width;
    control.renderHeight = inputs.height;
    control.surfaceCount = static_cast<uint32_t>(surfaceCount64);
    control.packedRowPitch = m_duplicationPackedRowPitch;

    {
        Upt04MarkerScope marker(
            inputs.commandList, "UPT.U0 Fill SampleID", inputs.nsightMarkers);
        inputs.commandList->setBufferState(
            finalPage, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            m_duplicationSampleIds, nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->commitBarriers();
        nvrhi::ComputeState state;
        state.pipeline = m_duplicationFillPipeline;
        state.bindings = { m_duplicationFillBindingSets[finalPageIndex] };
        inputs.commandList->setComputeState(state);
        inputs.commandList->setPushConstants(&control, sizeof(control));
        inputs.commandList->dispatch(
            (inputs.width + 7u) / 8u, (inputs.height + 7u) / 8u, 1u);
        nvrhi::utils::BufferUavBarrier(
            inputs.commandList, m_duplicationSampleIds);
    }
    {
        Upt04MarkerScope marker(
            inputs.commandList, "UPT.U1 Compute Duplication17x17", inputs.nsightMarkers);
        inputs.commandList->setBufferState(
            m_duplicationSampleIds, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setBufferState(
            m_duplicationScores[finalPageIndex],
            nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->commitBarriers();
        nvrhi::ComputeState state;
        state.pipeline = m_duplicationComputePipeline;
        state.bindings = { m_duplicationComputeBindingSets[finalPageIndex] };
        inputs.commandList->setComputeState(state);
        inputs.commandList->setPushConstants(&control, sizeof(control));
        inputs.commandList->dispatch(
            (inputs.width + 7u) / 8u, (inputs.height + 7u) / 8u, 1u);
        nvrhi::utils::BufferUavBarrier(
            inputs.commandList, m_duplicationScores[finalPageIndex]);
    }
    m_duplicationMetadata[finalPageIndex] = finalMetadata;
    m_duplicationMetadata[finalPageIndex].fullyWritten = true;
    return true;
}

void PathTraceUnifiedPtState::CompleteFrame()
{
    // Spatial writes the final current-frame result directly into the physical
    // history page, so its roles already describe the next frame. Without a
    // spatial publication, the physical current page contains either T0's
    // result or the fresh D0 fallback and must be promoted whenever temporal
    // mode is active. Basing this on T0 success left a stale reservoir page in
    // place when portal/resource churn made T0 skip, while the canonical
    // primary surface was still advanced unconditionally by the caller.
    if (m_temporalModeActive && m_initialPublishedThisFrame &&
        !m_spatialExecutedThisFrame)
    {
        const uint32_t oldCurrent = m_currentPageIndex;
        m_currentPageIndex = m_historyPageIndex;
        m_historyPageIndex = oldCurrent;
    }
    m_initialPublishedThisFrame = false;
    m_spatialExecutedThisFrame = false;
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
    for (uint32_t page = 0; page < 2u; ++page)
    {
        m_resolveBindingSets[page] = nullptr;
        m_resolveBindingSetDescValid[page] = false;
    }
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
    const uint32_t pageIndex = m_spatialExecutedThisFrame
        ? m_historyPageIndex : m_currentPageIndex;
    const nvrhi::BufferHandle resolvePage = m_spatialExecutedThisFrame
        ? HistoryPage() : CurrentPage();
    nvrhi::BindingSetDesc desc;
    desc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(0, resolvePage));
    desc.addItem(nvrhi::BindingSetItem::Texture_UAV(1, m_resolveOutput));
    desc.addItem(nvrhi::BindingSetItem::PushConstants(
        0, UPT05_PUSH_CONSTANT_BYTES));
    if (m_resolveBindingSets[pageIndex] &&
        m_resolveBindingSetDescValid[pageIndex] &&
        m_resolveBindingSetDescs[pageIndex] == desc)
    {
        return true;
    }
    m_resolveBindingSets[pageIndex] = inputs.device->createBindingSet(
        desc, m_resolveBindingLayout);
    if (!m_resolveBindingSets[pageIndex])
    {
        common->Printf("PathTraceUnifiedPt: failed to create UPT-05 resolve binding set\n");
        return false;
    }
    m_resolveBindingSetDescs[pageIndex] = desc;
    m_resolveBindingSetDescValid[pageIndex] = true;
    return true;
}

bool PathTraceUnifiedPtState::ExecuteResolve(
    const PathTraceUnifiedPtDispatchInputs& inputs,
    uint32_t view)
{
    m_resolveReady = false;
    const bool productionFullFrame = inputs.proofStage >= 9u &&
        Upt04PipelineVariant(inputs) == 0u;
    const nvrhi::BufferHandle resolvePage = m_spatialExecutedThisFrame
        ? HistoryPage() : CurrentPage();
    const PathTraceUnifiedPtPageMetadata& resolveMetadata = m_spatialExecutedThisFrame
        ? HistoryPageMetadata() : CurrentPageMetadata();
    const uint32_t resolvePageIndex = m_spatialExecutedThisFrame
        ? m_historyPageIndex : m_currentPageIndex;
    if (!productionFullFrame || !resolvePage || m_pageWidth != inputs.width ||
        m_pageHeight != inputs.height || !resolveMetadata.fullyWritten ||
        resolveMetadata.width != inputs.width ||
        resolveMetadata.height != inputs.height ||
        resolveMetadata.historyEpoch != inputs.historyEpoch)
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
            resolvePage, nvrhi::ResourceStates::ShaderResource);
        inputs.commandList->setTextureState(
            m_resolveOutput,
            nvrhi::AllSubresources,
            nvrhi::ResourceStates::UnorderedAccess);
        inputs.commandList->commitBarriers();

        nvrhi::ComputeState state;
        state.pipeline = m_resolvePipeline;
        state.bindings = { m_resolveBindingSets[resolvePageIndex] };
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
