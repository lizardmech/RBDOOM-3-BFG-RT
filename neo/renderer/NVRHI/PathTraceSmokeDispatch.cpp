#include "precompiled.h"
#pragma hdrstop

// Ray tracing dispatch for the RT smoke/path tracing path.
//
// Builds the per-frame raygen constants, transitions committed scene resources
// for shader access, dispatches the smoke RT pipeline, manages accumulation, and
// queues optional readback. Scene capture/resource ownership is intentionally
// outside this module.

#include "PathTraceCVars.h"
#include "PathTraceCleanRtxdiDiGui.h"
#include "PathTraceCleanRtxdiDiMaterialFeatures.h"
#include "PathTraceSmokeDispatch.h"
#include "PathTracePrimaryPass.h"
#include "PathTraceAcceleration.h"
#include "PathTraceAccelerationPlan.h"
#include "PathTraceDebugDumps.h"
#include "PathTraceDoomLights.h"
#include "PathTraceLightSelection.h"
#include "PathTraceMaterialFeatureBindings.h"
#include "PathTraceNeeCache.h"
#include "PathTraceReGIR.h"
#include "PathTraceDebugModes.h"
#include "PathTraceDLSSRRBridge.h"
#include "../RenderBackend.h"

#include <Rtxdi/DI/ReSTIRDI.h>
#include "../RenderCommon.h"
#include "../../sys/DeviceManager.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <nvrhi/utils.h>

extern idCVar r_forceAmbient;
extern DeviceManager* deviceManager;

namespace {

const int RT_SMOKE_MAX_EMISSIVE_TRIANGLE_RECORDS = 65536;
const uint32_t CLEAN_RTXDI_DI_FLAG_EXTERNAL_PDFNEE_CURRENT = 1u << 0u;
const uint32_t CLEAN_RTXDI_DI_FLAG_REMIX_LIGHT_UNIVERSE = 1u << 10u;
const uint32_t CLEAN_RTXDI_DI_FLAG_NEE_CACHE_PROVIDER = 1u << 11u;
const uint32_t CLEAN_RTXDI_DI_FLAG_PREVIOUS_BEST_APPROXIMATION = 1u << 12u;
const uint32_t CLEAN_RTXDI_DI_FLAG_DUMMY_EMISSIVE_NORMALS = 1u << 13u;
const uint32_t CLEAN_RTXDI_DI_FLAG_FORCE_EMISSIVE_VISIBILITY = 1u << 14u;
const uint32_t CLEAN_RTXDI_DI_FLAG_SPATIAL_REUSE = 1u << 15u;
const uint32_t CLEAN_RTXDI_DI_FLAG_SPATIAL_TEMPORAL_PREPASS = 1u << 16u;
const uint32_t CLEAN_RTXDI_DI_FLAG_INITIAL_VISIBILITY = 1u << 17u;
const uint32_t CLEAN_RTXDI_DI_FLAG_RESOLVE_SOLID_ANGLE_PDF = 1u << 18u;
const uint32_t CLEAN_RTXDI_DI_FLAG_DISABLE_RIGID_EMISSIVE_TEMPORAL = 1u << 19u;
const uint32_t CLEAN_RTXDI_DI_FLAG_TRANSMISSION_PSR_PHASE = 1u << 20u;
const uint32_t CLEAN_RTXDI_DI_FLAG_LIQUID_MODIFIER_VISIBILITY = 1u << 21u;
const uint32_t CLEAN_RTXDI_DI_FLAG_GLASS_DISTORTION = 1u << 22u;
const uint32_t CLEAN_RTXDI_DI_FLAG_GLASS_REFRACTED_PSR = 1u << 23u;
const uint32_t CLEAN_RTXDI_DI_FLAG_BLUE_NOISE = 1u << 24u;
const uint32_t CLEAN_RTXDI_DI_FLAG_GLASS_REFLECTION_PSR = 1u << 25u;
const uint32_t CLEAN_RTXDI_DI_FLAG_REFLECTION_SECONDARY_NO_SHADOWS = 1u << 26u;
const uint32_t CLEAN_RTXDI_DI_FLAG_OPAQUE_MIRROR_REFLECTION = 1u << 27u;
const uint32_t CLEAN_RTXDI_DI_FLAG_TRANSMISSION_TRACE_PROBE_SHIFT = 28u;
const uint32_t CLEAN_RTXDI_DI_FLAG_TRANSMISSION_TRACE_PROBE_MASK =
    7u << CLEAN_RTXDI_DI_FLAG_TRANSMISSION_TRACE_PROBE_SHIFT;
const uint32_t CLEAN_RTXDI_DI_FLAG_TRANSMISSION_ITERATIVE_RESOLVE = 1u << 31u;
const uint32_t LIQUID_POOL_CONTROL_TELEMETRY_READY = 1u << 0u;
const uint32_t LIQUID_POOL_CONTROL_REQUESTED = 1u << 1u;
const uint32_t LIQUID_POOL_CONTROL_ROUTE_DISABLED = 1u << 2u;
const uint32_t LIQUID_POOL_CONTROL_PARAMETERS_READY = 1u << 3u;
const uint32_t CLEAN_RTXDI_DI_LIQUID_MODE_SHIFT = 2u;
const uint32_t CLEAN_RTXDI_DI_LIQUID_DEBUG_SHIFT = 4u;
const uint32_t CLEAN_RTXDI_DI_LIQUID_PAGE_SHIFT = 7u;
const uint32_t CLEAN_RTXDI_DI_LIQUID_CONTROL_SHIFT = 9u;
const uint32_t RT_SMOKE_TEXTURE_FLAG_OPENPBR_BRDF_MODE_SHIFT = 9u;
const uint32_t RT_SMOKE_TEXTURE_FLAG_OPENPBR_BRDF_MODE_MASK = 7u << RT_SMOKE_TEXTURE_FLAG_OPENPBR_BRDF_MODE_SHIFT;
const uint32_t RT_SMOKE_TEXTURE_FLAG_SKY_CUBE = 1u << 12u;
const uint32_t CLEAN_RTXDI_DI_RESOLVE_BRDF_TARGET_ENABLE = 1u << 0u;
const uint32_t CLEAN_RTXDI_DI_RESOLVE_BRDF_MODE_SHIFT = 8u;
const uint32_t CLEAN_RTXDI_DI_RESOLVE_BRDF_MODE_MASK = 7u << CLEAN_RTXDI_DI_RESOLVE_BRDF_MODE_SHIFT;
int g_smokeLastDispatchTimingLogMs = -1000000;
PathTraceCleanRtxdiDiGuiSnapshot g_cleanRtxdiDiGuiSnapshot;

uint32_t PathTraceOpenPbrBrdfModeValue()
{
    return static_cast<uint32_t>(idMath::ClampInt(0, 4, r_pathTracingOpenPbrBrdfMode.GetInteger()));
}

uint32_t PackPathTraceOpenPbrBrdfMode()
{
    const uint32_t mode = PathTraceOpenPbrBrdfModeValue();
    return (mode << RT_SMOKE_TEXTURE_FLAG_OPENPBR_BRDF_MODE_SHIFT) & RT_SMOKE_TEXTURE_FLAG_OPENPBR_BRDF_MODE_MASK;
}

uint32_t PackCleanRtxdiDiResolveBrdfTarget()
{
    const uint32_t enabled = r_pathTracingCleanRtxdiDiResolveBrdfTarget.GetInteger() != 0
        ? CLEAN_RTXDI_DI_RESOLVE_BRDF_TARGET_ENABLE
        : 0u;
    return enabled |
        ((PathTraceOpenPbrBrdfModeValue() << CLEAN_RTXDI_DI_RESOLVE_BRDF_MODE_SHIFT) &
            CLEAN_RTXDI_DI_RESOLVE_BRDF_MODE_MASK);
}

int CleanRtxdiDiTemporalBiasCorrectionValue()
{
    const int requested = idMath::ClampInt(0, 3, r_pathTracingCleanRtxdiDiTemporalBiasCorrection.GetInteger());
    // This tree builds against RTXDI-main, where ray-traced DI bias correction is 3.
    // Keep console value 2 as the clean-route request for ray-traced parity with RTX Remix docs/builds.
    return requested >= 2 ? 3 : requested;
}

struct PathTraceCleanRtxdiDiBoilingFilterConstants
{
    uint32_t width = 0;
    uint32_t height = 0;
    float threshold = 5.0f;
    uint32_t enabled = 0;
};

struct PathTraceSkySurfaceResolveConstants
{
    uint32_t width = 0;
    uint32_t height = 0;
    float brightness = 1.0f;
    uint32_t enabled = 0;
};

float PathTraceDLSSRRHalton(uint32_t index, uint32_t base)
{
    float result = 0.0f;
    float fraction = 1.0f;
    while (index > 0u)
    {
        fraction /= static_cast<float>(base);
        result += fraction * static_cast<float>(index % base);
        index /= base;
    }
    return result;
}

idVec2 PathTraceDLSSRRPixelJitter(const viewDef_t* viewDef, uint32_t frameIndex, bool enabled)
{
    (void)viewDef;
    if (!enabled)
    {
        return idVec2(0.0f, 0.0f);
    }

    const uint32_t sequenceIndex = (frameIndex % 1024u) + 1u;
    return idVec2(
        PathTraceDLSSRRHalton(sequenceIndex, 2u) - 0.5f,
        PathTraceDLSSRRHalton(sequenceIndex, 3u) - 0.5f);
}

void ReplaceStructuredBufferSrv(nvrhi::BindingSetDesc& desc, uint32_t slot, nvrhi::BufferHandle buffer)
{
    const nvrhi::BindingSetItem item = nvrhi::BindingSetItem::StructuredBuffer_SRV(slot, buffer);
    for (nvrhi::BindingSetItem& binding : desc.bindings)
    {
        if (binding.slot == slot && binding.type == nvrhi::ResourceType::StructuredBuffer_SRV)
        {
            binding = item;
            return;
        }
    }
    desc.addItem(item);
}

}

void PathTraceCleanRtxdiDiPublishGuiSnapshot(const PathTraceCleanRtxdiDiGuiSnapshot& snapshot)
{
    const bool temporalAuditValid = g_cleanRtxdiDiGuiSnapshot.temporalAuditValid;
    const unsigned int temporalAuditPixels = g_cleanRtxdiDiGuiSnapshot.temporalAuditPixels;
    const unsigned int temporalAuditCurrentValid = g_cleanRtxdiDiGuiSnapshot.temporalAuditCurrentValid;
    const unsigned int temporalAuditCurrentCandidate = g_cleanRtxdiDiGuiSnapshot.temporalAuditCurrentCandidate;
    const unsigned int temporalAuditSurfaceValid = g_cleanRtxdiDiGuiSnapshot.temporalAuditSurfaceValid;
    const unsigned int temporalAuditMotionValid = g_cleanRtxdiDiGuiSnapshot.temporalAuditMotionValid;
    const unsigned int temporalAuditCameraFallback = g_cleanRtxdiDiGuiSnapshot.temporalAuditCameraFallback;
    const unsigned int temporalAuditPreviousPixelInBounds = g_cleanRtxdiDiGuiSnapshot.temporalAuditPreviousPixelInBounds;
    const unsigned int temporalAuditPreviousSurfaceValid = g_cleanRtxdiDiGuiSnapshot.temporalAuditPreviousSurfaceValid;
    const unsigned int temporalAuditPreviousReservoirValid = g_cleanRtxdiDiGuiSnapshot.temporalAuditPreviousReservoirValid;
    const unsigned int temporalAuditPreviousLightMapped = g_cleanRtxdiDiGuiSnapshot.temporalAuditPreviousLightMapped;
    const unsigned int temporalAuditPreviousTargetAtCurrent = g_cleanRtxdiDiGuiSnapshot.temporalAuditPreviousTargetAtCurrent;
    const unsigned int temporalAuditSdkCalled = g_cleanRtxdiDiGuiSnapshot.temporalAuditSdkCalled;
    const unsigned int temporalAuditSdkTemporalSamplePixelValid = g_cleanRtxdiDiGuiSnapshot.temporalAuditSdkTemporalSamplePixelValid;
    const unsigned int temporalAuditOutputReservoirValid = g_cleanRtxdiDiGuiSnapshot.temporalAuditOutputReservoirValid;
    const unsigned int temporalAuditSdkSelectedPrevious = g_cleanRtxdiDiGuiSnapshot.temporalAuditSdkSelectedPrevious;
    const unsigned int temporalAuditSdkReusedPrevious = g_cleanRtxdiDiGuiSnapshot.temporalAuditSdkReusedPrevious;
    const unsigned int temporalAuditOutputChanged = g_cleanRtxdiDiGuiSnapshot.temporalAuditOutputChanged;
    const float temporalAuditAvgPreviousM = g_cleanRtxdiDiGuiSnapshot.temporalAuditAvgPreviousM;
    const float temporalAuditAvgOutputM = g_cleanRtxdiDiGuiSnapshot.temporalAuditAvgOutputM;

    g_cleanRtxdiDiGuiSnapshot = snapshot;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditValid = temporalAuditValid;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditPixels = temporalAuditPixels;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditCurrentValid = temporalAuditCurrentValid;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditCurrentCandidate = temporalAuditCurrentCandidate;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditSurfaceValid = temporalAuditSurfaceValid;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditMotionValid = temporalAuditMotionValid;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditCameraFallback = temporalAuditCameraFallback;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditPreviousPixelInBounds = temporalAuditPreviousPixelInBounds;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditPreviousSurfaceValid = temporalAuditPreviousSurfaceValid;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditPreviousReservoirValid = temporalAuditPreviousReservoirValid;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditPreviousLightMapped = temporalAuditPreviousLightMapped;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditPreviousTargetAtCurrent = temporalAuditPreviousTargetAtCurrent;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditSdkCalled = temporalAuditSdkCalled;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditSdkTemporalSamplePixelValid = temporalAuditSdkTemporalSamplePixelValid;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditOutputReservoirValid = temporalAuditOutputReservoirValid;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditSdkSelectedPrevious = temporalAuditSdkSelectedPrevious;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditSdkReusedPrevious = temporalAuditSdkReusedPrevious;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditOutputChanged = temporalAuditOutputChanged;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditAvgPreviousM = temporalAuditAvgPreviousM;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditAvgOutputM = temporalAuditAvgOutputM;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditEnabled = snapshot.temporalAuditEnabled;
    if (!snapshot.temporalAuditEnabled)
    {
        g_cleanRtxdiDiGuiSnapshot.temporalAuditValid = false;
    }
}

void PathTraceCleanRtxdiDiPublishTemporalAudit(const PathTraceCleanRtxdiDiGuiSnapshot& snapshot)
{
    g_cleanRtxdiDiGuiSnapshot.temporalAuditValid = snapshot.temporalAuditValid;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditPixels = snapshot.temporalAuditPixels;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditCurrentValid = snapshot.temporalAuditCurrentValid;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditCurrentCandidate = snapshot.temporalAuditCurrentCandidate;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditSurfaceValid = snapshot.temporalAuditSurfaceValid;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditMotionValid = snapshot.temporalAuditMotionValid;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditCameraFallback = snapshot.temporalAuditCameraFallback;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditPreviousPixelInBounds = snapshot.temporalAuditPreviousPixelInBounds;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditPreviousSurfaceValid = snapshot.temporalAuditPreviousSurfaceValid;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditPreviousReservoirValid = snapshot.temporalAuditPreviousReservoirValid;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditPreviousLightMapped = snapshot.temporalAuditPreviousLightMapped;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditPreviousTargetAtCurrent = snapshot.temporalAuditPreviousTargetAtCurrent;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditSdkCalled = snapshot.temporalAuditSdkCalled;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditSdkTemporalSamplePixelValid = snapshot.temporalAuditSdkTemporalSamplePixelValid;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditOutputReservoirValid = snapshot.temporalAuditOutputReservoirValid;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditSdkSelectedPrevious = snapshot.temporalAuditSdkSelectedPrevious;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditSdkReusedPrevious = snapshot.temporalAuditSdkReusedPrevious;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditOutputChanged = snapshot.temporalAuditOutputChanged;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditAvgPreviousM = snapshot.temporalAuditAvgPreviousM;
    g_cleanRtxdiDiGuiSnapshot.temporalAuditAvgOutputM = snapshot.temporalAuditAvgOutputM;
}

bool PathTraceCleanRtxdiDiGetGuiSnapshot(PathTraceCleanRtxdiDiGuiSnapshot& snapshot)
{
    snapshot = g_cleanRtxdiDiGuiSnapshot;
    return snapshot.valid;
}

namespace {

void SetBufferStateIfPresent(nvrhi::ICommandList* commandList, const nvrhi::BufferHandle& buffer, nvrhi::ResourceStates state)
{
    if (commandList && buffer)
    {
        commandList->setBufferState(buffer, state);
    }
}

float SnapPathTraceReGIRCenterCoord(float value, float cellSize)
{
    if (!std::isfinite(value) || !std::isfinite(cellSize) || cellSize <= 0.0f)
    {
        return value;
    }

    return static_cast<float>(std::floor(static_cast<double>(value) / static_cast<double>(cellSize) + 0.5) * static_cast<double>(cellSize));
}

idVec3 SnapPathTraceReGIRCenterToCell(const idVec3& center, const PathTraceReGIRSettings& settings)
{
    return idVec3(
        SnapPathTraceReGIRCenterCoord(center.x, settings.cellSize),
        SnapPathTraceReGIRCenterCoord(center.y, settings.cellSize),
        SnapPathTraceReGIRCenterCoord(center.z, settings.cellSize));
}

idVec3 ResolvePathTraceReGIRCenter(const RtSmokeGeometryUniverse& geometryUniverse, const PathTraceReGIRSettings& settings, const idVec3& fallbackCenter)
{
    if (settings.centerMode == 2)
    {
        return idVec3(settings.manualCenter[0], settings.manualCenter[1], settings.manualCenter[2]);
    }

    if (settings.centerMode != 1)
    {
        return SnapPathTraceReGIRCenterToCell(fallbackCenter, settings);
    }

    const std::vector<PathTraceSmokeVertex>& staticVertices = geometryUniverse.StaticVertices();
    if (staticVertices.empty())
    {
        return SnapPathTraceReGIRCenterToCell(fallbackCenter, settings);
    }

    idBounds bounds;
    bounds.Clear();
    int validPoints = 0;
    for (const PathTraceSmokeVertex& vertex : staticVertices)
    {
        const idVec3 position = SmokeVertexPosition(vertex);
        if (!SmokeVec3IsFinite(position))
        {
            continue;
        }
        bounds.AddPoint(position);
        ++validPoints;
    }

    if (validPoints <= 0)
    {
        return SnapPathTraceReGIRCenterToCell(fallbackCenter, settings);
    }

    idVec3 center = bounds.GetCenter();
    const idVec3 halfGridExtent(
        Max(settings.cellSize * static_cast<float>(settings.gridX) * 0.5f, settings.cellSize * 0.5f),
        Max(settings.cellSize * static_cast<float>(settings.gridY) * 0.5f, settings.cellSize * 0.5f),
        Max(settings.cellSize * static_cast<float>(settings.gridZ) * 0.5f, settings.cellSize * 0.5f));

    if (SmokeVec3IsFinite(fallbackCenter))
    {
        for (int axis = 0; axis < 3; ++axis)
        {
            const float mapMin = bounds[0][axis];
            const float mapMax = bounds[1][axis];
            const float minCenter = mapMin + halfGridExtent[axis];
            const float maxCenter = mapMax - halfGridExtent[axis];
            if (minCenter <= maxCenter)
            {
                center[axis] = idMath::ClampFloat(minCenter, maxCenter, fallbackCenter[axis]);
            }
        }
    }

    return SnapPathTraceReGIRCenterToCell(center, settings);
}

struct PathTraceCleanRtxdiDiSentinelConstants
{
    uint32_t view = 0;
    uint32_t status = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t analyticLightCount = 0;
    uint32_t analyticIdentityCount = 0;
    uint32_t lightMode = 0;
    uint32_t frameIndex = 0;
    uint32_t reservoirCount = 0;
    uint32_t candidateCount = 0;
    uint32_t flags = 0;
    uint32_t previousAnalyticLightCount = 0;
    uint32_t previousAnalyticIdentityCount = 0;
    uint32_t analyticRemapCount = 0;
    uint32_t temporalFlags = 0;
    uint32_t historyResetCount = 0;
    uint32_t view8Band = 0xffffffffu;
    uint32_t resolveVisibilityReuse = 0;
    uint32_t resolveBrdfTarget = 0;
    uint32_t referenceRab = 0;
    uint32_t rluCurrentLightCount = 0;
    uint32_t rluPreviousLightCount = 0;
    uint32_t rluCurrentToPreviousCount = 0;
    uint32_t rluPreviousToCurrentCount = 0;
    uint32_t temporalAudit = 0;
    uint32_t staticTriangleCount = 0;
    uint32_t dynamicTriangleCount = 0;
    uint32_t rigidRouteTriangleCount = 0;
    uint32_t currentEmissiveTriangleCount = 0;
    uint32_t previousEmissiveTriangleCount = 0;
    uint32_t rluDoomAnalyticRangeOffset = 0;
    uint32_t rluDoomAnalyticRangeCount = 0;
    uint32_t doomAnalyticFullCurrentCount = 0;
    uint32_t doomAnalyticFullPreviousCount = 0;
    uint32_t rluDomain = 0;
    uint32_t temporalFireflyClamp = 0;
    float textureInfo[4] = {};
    float prevCameraOriginAndValid[4] = {};
    float prevCameraForwardAndTanX[4] = {};
    float prevCameraLeftAndTanY[4] = {};
    float prevCameraUpAndTanY[4] = {};
    float cameraOriginAndValid[4] = {};
    float cameraForwardAndTanX[4] = {};
    float cameraLeftAndTanY[4] = {};
    float cameraUpAndTanY[4] = {};
    float doomAnalyticLightInfo[4] = {};
    float motionVectorInfo[4] = {};
    float restirPTSurfaceInfo[4] = {};
    float neeCacheInfo0[4] = {};
    float neeCacheInfo1[4] = {};
    float rluRangeInfo[4] = {};
    float rluSampleInfo[4] = {};
    float toyPathInfo[4] = {};
    float geometryInfo0[4] = {};
    float geometryInfo1[4] = {};
    float spatialInfo[4] = {};
    float emissiveDistributionInfo[4] = {};
    uint32_t staticBucketRouteInfo[4] = {};
};

static_assert(sizeof(PathTraceCleanRtxdiDiSentinelConstants) <= 512, "PathTraceCleanRtxdiDiSentinelConstants exceeds allocated constant buffer size");

nvrhi::ObjectType GetPathTraceCommandObjectType()
{
    if (deviceManager && deviceManager->GetGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
    {
        return nvrhi::ObjectTypes::VK_CommandBuffer;
    }
    return nvrhi::ObjectTypes::D3D12_GraphicsCommandList;
}

class PathTraceGpuMarkerScope
{
public:
    PathTraceGpuMarkerScope(nvrhi::ICommandList* commandList, const char* name, bool enabled)
        : m_commandList(enabled ? commandList : nullptr)
    {
        if (m_commandList && name && name[0])
        {
            m_commandList->beginMarker(name);
        }
    }

    ~PathTraceGpuMarkerScope()
    {
        if (m_commandList)
        {
            m_commandList->endMarker();
        }
    }

private:
    nvrhi::ICommandList* m_commandList = nullptr;
};

struct PathTraceSmokeConstants
{
    float cameraOriginAndTMax[4];
    float cameraForwardAndTanX[4];
    float cameraLeftAndTanY[4];
    float cameraUpAndDebugMode[4];
    float textureInfo[4];
    float lightOriginAndRadius[RT_SMOKE_MAX_DEBUG_LIGHTS][4];
    float lightColorAndIntensity[RT_SMOKE_MAX_DEBUG_LIGHTS][4];
    float lightInfo[4];
    float portalWindowInfo[4];
    float lightSpriteInfo[4];
    float toyPathInfo[4];
    float emissiveInfo[4];
    float emissiveDistributionInfo[4];
    float boundsOverlayInfo[4];
    float doomAnalyticLightInfo[4];
    float doomAnalyticLightRemapInfo[4];
    float restirPTInfo[4];
    float integratorInfo[4];
    float integratorInfo2[4];
    float prevCameraOriginAndValid[4];
    float prevCameraForwardAndTanX[4];
    float prevCameraLeftAndTanY[4];
    float prevCameraUpAndTanY[4];
    float safetyInfo[4];
    float geometryInfo0[4];
    float geometryInfo1[4];
    float geometryInfo2[4];
    float geometryInfo3[4];
    float geometryInfo4[4];
    float dispatchTileInfo[4];
    float neeInfo[4];
    float motionVectorInfo[4];
    float restirPTSurfaceInfo[4];
    float reservedRestirPTDirectInfo[4];
    float reservedRestirPTSparsityInfo[4];
    float reservedRestirPTIndirectInfo[4];
    float rayReconstructionInfo[4];
    float unifiedLightInfo[4];
    float restirLightManagerInfo[4];
    float restirLightManagerControlInfo[4];
    float restirLightManagerRangeInfo[4];
    float restirLightManagerSampleInfo[4];
    float reservedRestirPdfNeeInfo[4];
    float restirPdfNeeRluCurrentControlInfo[4];
    float reservedRestirPTDiDebugInfo[4];
    uint32_t reservedRestirPTRemixDiReservoirInfo[4];
    uint32_t reservedRestirPTRemixDiReservoirPageInfo[4];
    float reservedRestirPTGiDebugInfo[4];
    float regirInfo0[4];
    float regirInfo1[4];
    float regirInfo2[4];
    float regirInfo3[4];
    float regirInfo4[4];
    float neeCacheInfo0[4];
    float neeCacheInfo1[4];
    float neeCacheInfo2[4];
    float neeCacheInfo3[4];
    float neeCacheConsumerInfo[4];
    float decalInfo[4];
    float decalInfo2[4];
    float liquidPoolInfo[4];
    uint32_t staticBucketRouteInfo[4];
};

static_assert(offsetof(PathTraceSmokeConstants, liquidPoolInfo) == offsetof(PathTraceSmokeConstants, decalInfo2) + sizeof(float) * 4,
    "PathTraceSmokeConstants liquid-pool control offset must mirror HLSL");
static_assert(offsetof(PathTraceSmokeConstants, staticBucketRouteInfo) == offsetof(PathTraceSmokeConstants, liquidPoolInfo) + sizeof(float) * 4,
    "PathTraceSmokeConstants static-bucket route control must follow liquid-pool control");
static_assert(sizeof(PathTraceSmokeConstants) == offsetof(PathTraceSmokeConstants, staticBucketRouteInfo) + sizeof(uint32_t) * 4,
    "PathTraceSmokeConstants static-bucket route control must remain the final uint4");

static void PopulatePathTraceDecalAndLiquidPoolControls(
    PathTraceSmokeConstants& constants,
    int effectiveLiquidPoolMode,
    bool liquidPoolTelemetryReady,
    int dynamicMaterialRecordCount,
    int materialOverlayRecordCount)
{
    constants.decalInfo[0] = static_cast<float>(idMath::ClampInt(0, 4, r_pathTracingDecalComposite.GetInteger()));
    constants.decalInfo[1] = Max(0.0f, r_pathTracingDecalOffsetStep.GetFloat());
    constants.decalInfo[2] = static_cast<float>(Max(1, r_pathTracingDecalMaxOffsetIndex.GetInteger()));
    constants.decalInfo[3] = idMath::ClampFloat(0.0f, 1.0f, r_pathTracingDecalModulateFloor.GetFloat());
    constants.decalInfo2[0] = static_cast<float>(Max(0, dynamicMaterialRecordCount));
    constants.decalInfo2[1] = static_cast<float>(Max(0, materialOverlayRecordCount));
    constants.liquidPoolInfo[0] = static_cast<float>(idMath::ClampInt(0, 3, effectiveLiquidPoolMode));
    constants.liquidPoolInfo[1] = static_cast<float>(idMath::ClampInt(0, 6, r_pathTracingLiquidPoolDebug.GetInteger()));
    constants.liquidPoolInfo[2] = static_cast<float>(idMath::ClampInt(0, 3, r_pathTracingLiquidPoolDebugPage.GetInteger()));
    const int requestedLiquidPoolMode = idMath::ClampInt(0, 3, r_pathTracingLiquidPoolMode.GetInteger());
    uint32_t controlFlags = liquidPoolTelemetryReady ? LIQUID_POOL_CONTROL_TELEMETRY_READY : 0u;
    controlFlags |= requestedLiquidPoolMode != 0 ? LIQUID_POOL_CONTROL_REQUESTED : 0u;
    controlFlags |= requestedLiquidPoolMode != 0 && effectiveLiquidPoolMode == 0
        ? LIQUID_POOL_CONTROL_ROUTE_DISABLED
        : 0u;
    controlFlags |= materialOverlayRecordCount > 0 ? LIQUID_POOL_CONTROL_PARAMETERS_READY : 0u;
    constants.liquidPoolInfo[3] = static_cast<float>(controlFlags);
}

uint32_t PackCleanRtxdiDiLiquidPoolControls(int effectiveMode, bool telemetryReady, bool parametersReady)
{
    const uint32_t mode = static_cast<uint32_t>(idMath::ClampInt(0, 3, effectiveMode));
    const uint32_t debug = static_cast<uint32_t>(idMath::ClampInt(0, 6, r_pathTracingLiquidPoolDebug.GetInteger()));
    const uint32_t page = static_cast<uint32_t>(idMath::ClampInt(0, 3, r_pathTracingLiquidPoolDebugPage.GetInteger()));
    const int requestedMode = idMath::ClampInt(0, 3, r_pathTracingLiquidPoolMode.GetInteger());
    uint32_t controlFlags = telemetryReady ? LIQUID_POOL_CONTROL_TELEMETRY_READY : 0u;
    controlFlags |= requestedMode != 0 ? LIQUID_POOL_CONTROL_REQUESTED : 0u;
    controlFlags |= requestedMode != 0 && mode == 0u ? LIQUID_POOL_CONTROL_ROUTE_DISABLED : 0u;
    controlFlags |= parametersReady ? LIQUID_POOL_CONTROL_PARAMETERS_READY : 0u;
    return (mode << CLEAN_RTXDI_DI_LIQUID_MODE_SHIFT) |
        (debug << CLEAN_RTXDI_DI_LIQUID_DEBUG_SHIFT) |
        (page << CLEAN_RTXDI_DI_LIQUID_PAGE_SHIFT) |
        (controlFlags << CLEAN_RTXDI_DI_LIQUID_CONTROL_SHIFT);
}

struct PathTraceIntegratorSettings
{
    int samplesPerPixel = 1;
    int maxPathDepth = 2;
    int diffuseBounceLimit = 1;
    int specularBounceLimit = 0;
    int transmissionBounceLimit = 0;
    int reflectionMode = 0;
    int russianRouletteDepth = 0;
    int nextEventEstimation = 1;
    int secondaryNeeMode = 1;
    int secondaryNeeVisibility = 1;
    int secondaryAnalyticNeeMode = 2;
    int secondaryAnalyticNeeSamples = 1;
};

struct PathTraceDispatchTileSettings
{
    bool enabled = false;
    int tileWidth = 0;
    int tileHeight = 0;
    int tileColumns = 1;
    int tileRows = 1;
    int tileCount = 1;
    uint64 estimatedRaysPerTile = 0;
    uint64 estimatedRaysFullFrame = 0;
};

enum PathTraceSafetyDisableBits : uint32_t
{
    RT_PT_SAFETY_DISABLE_ANY_HIT_ALPHA = 1u << 0,
    RT_PT_SAFETY_DISABLE_SELECTED_LIGHT_LOOP = 1u << 1,
    RT_PT_SAFETY_DISABLE_ANALYTIC_LIGHT_LOOP = 1u << 2,
    RT_PT_SAFETY_DISABLE_EMISSIVE_TRIANGLE_SAMPLING = 1u << 3,
    RT_PT_SAFETY_DISABLE_DIFFUSE_SECONDARY_RAY = 1u << 4,
    RT_PT_SAFETY_DISABLE_REFLECTION_RAY = 1u << 5,
    RT_PT_SAFETY_DISABLE_PRIMARY_SURFACE_HISTORY = 1u << 6,
    RT_PT_SAFETY_DISABLE_RESTIR_VISIBILITY_RAY = 1u << 8,
};

uint64 HashSmokeDispatchValue(uint64 hash, uint64 value)
{
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash;
}

PathTraceIntegratorSettings BuildPathTraceIntegratorSettings()
{
    PathTraceIntegratorSettings settings;
    settings.samplesPerPixel = idMath::ClampInt(1, 4, r_pathTracingSamplesPerPixel.GetInteger());
    settings.maxPathDepth = idMath::ClampInt(1, 4, r_pathTracingMaxPathDepth.GetInteger());
    settings.diffuseBounceLimit = idMath::ClampInt(0, 3, r_pathTracingDiffuseBounceLimit.GetInteger());
    settings.specularBounceLimit = idMath::ClampInt(0, 2, r_pathTracingSpecularBounceLimit.GetInteger());
    settings.transmissionBounceLimit = idMath::ClampInt(0, 1, r_pathTracingTransmissionBounceLimit.GetInteger());
    settings.reflectionMode = idMath::ClampInt(0, 2, r_pathTracingReflectionMode.GetInteger());
    settings.russianRouletteDepth = idMath::ClampInt(0, 8, r_pathTracingRussianRouletteDepth.GetInteger());
    settings.nextEventEstimation = r_pathTracingNextEventEstimation.GetInteger() != 0 ? 1 : 0;
    settings.secondaryNeeMode = idMath::ClampInt(0, 2, r_pathTracingSecondaryNeeMode.GetInteger());
    settings.secondaryNeeVisibility = r_pathTracingSecondaryNeeVisibility.GetInteger() != 0 ? 1 : 0;
    settings.secondaryAnalyticNeeMode = idMath::ClampInt(0, 2, r_pathTracingSecondaryAnalyticNeeMode.GetInteger());
    settings.secondaryAnalyticNeeSamples = idMath::ClampInt(0, 8, r_pathTracingSecondaryAnalyticNeeSamples.GetInteger());
    return settings;
}

uint32_t BuildPathTraceSafetyDisableMask()
{
    uint32_t mask = 0u;
    mask |= r_pathTracingDisableAnyHitAlpha.GetInteger() != 0 ? RT_PT_SAFETY_DISABLE_ANY_HIT_ALPHA : 0u;
    mask |= r_pathTracingDisableSelectedLightLoop.GetInteger() != 0 ? RT_PT_SAFETY_DISABLE_SELECTED_LIGHT_LOOP : 0u;
    mask |= r_pathTracingDisableAnalyticLightLoop.GetInteger() != 0 ? RT_PT_SAFETY_DISABLE_ANALYTIC_LIGHT_LOOP : 0u;
    mask |= r_pathTracingDisableEmissiveTriangleSampling.GetInteger() != 0 ? RT_PT_SAFETY_DISABLE_EMISSIVE_TRIANGLE_SAMPLING : 0u;
    mask |= r_pathTracingDisableDiffuseSecondaryRay.GetInteger() != 0 ? RT_PT_SAFETY_DISABLE_DIFFUSE_SECONDARY_RAY : 0u;
    mask |= r_pathTracingDisableReflectionRay.GetInteger() != 0 ? RT_PT_SAFETY_DISABLE_REFLECTION_RAY : 0u;
    mask |= r_pathTracingDisablePrimarySurfaceHistory.GetInteger() != 0 ? RT_PT_SAFETY_DISABLE_PRIMARY_SURFACE_HISTORY : 0u;
    mask |= r_pathTracingDisableRestirVisibilityRay.GetInteger() != 0 ? RT_PT_SAFETY_DISABLE_RESTIR_VISIBILITY_RAY : 0u;
    return mask;
}

bool PathTraceSafetyDisabled(uint32_t mask, PathTraceSafetyDisableBits bit)
{
    return (mask & static_cast<uint32_t>(bit)) != 0u;
}

PathTraceIntegratorSettings ApplyPathTraceSafetyKillSwitches(PathTraceIntegratorSettings settings, uint32_t safetyDisableMask)
{
    if (PathTraceSafetyDisabled(safetyDisableMask, RT_PT_SAFETY_DISABLE_DIFFUSE_SECONDARY_RAY))
    {
        settings.diffuseBounceLimit = 0;
    }
    if (PathTraceSafetyDisabled(safetyDisableMask, RT_PT_SAFETY_DISABLE_REFLECTION_RAY))
    {
        settings.specularBounceLimit = 0;
        settings.reflectionMode = 0;
    }
    return settings;
}

uint64 HashPathTraceIntegratorSettings(uint64 hash, const PathTraceIntegratorSettings& settings)
{
    hash = HashSmokeDispatchValue(hash, static_cast<uint64>(settings.samplesPerPixel));
    hash = HashSmokeDispatchValue(hash, static_cast<uint64>(settings.maxPathDepth));
    hash = HashSmokeDispatchValue(hash, static_cast<uint64>(settings.diffuseBounceLimit));
    hash = HashSmokeDispatchValue(hash, static_cast<uint64>(settings.specularBounceLimit));
    hash = HashSmokeDispatchValue(hash, static_cast<uint64>(settings.transmissionBounceLimit));
    hash = HashSmokeDispatchValue(hash, static_cast<uint64>(settings.reflectionMode));
    hash = HashSmokeDispatchValue(hash, static_cast<uint64>(settings.russianRouletteDepth));
    hash = HashSmokeDispatchValue(hash, static_cast<uint64>(settings.nextEventEstimation));
    hash = HashSmokeDispatchValue(hash, static_cast<uint64>(settings.secondaryNeeMode));
    hash = HashSmokeDispatchValue(hash, static_cast<uint64>(settings.secondaryNeeVisibility));
    hash = HashSmokeDispatchValue(hash, static_cast<uint64>(settings.secondaryAnalyticNeeMode));
    hash = HashSmokeDispatchValue(hash, static_cast<uint64>(settings.secondaryAnalyticNeeSamples));
    return hash;
}

int EstimatePathTraceRaysPerPixel(const PathTraceIntegratorSettings& settings, int selectedLightCount, int analyticLightCount)
{
    const int diffuseRayCount = (settings.maxPathDepth > 1 && settings.diffuseBounceLimit > 0) ? 1 : 0;
    const int reflectionRayCount = (settings.maxPathDepth > 1 && settings.specularBounceLimit > 0 && settings.reflectionMode > 0) ? 1 : 0;
    const int transmissionRayCount = (settings.maxPathDepth > 1 && settings.transmissionBounceLimit > 0) ? 1 : 0;
    const int secondarySurfaceCount = diffuseRayCount + reflectionRayCount + transmissionRayCount;
    int neeTrials = 0;
    if (settings.nextEventEstimation != 0)
    {
        neeTrials += selectedLightCount + analyticLightCount;
        const int secondarySelectedTrials = settings.secondaryNeeMode == 2 ? selectedLightCount : (settings.secondaryNeeMode == 1 && selectedLightCount > 0 ? 1 : 0);
        const int secondaryAnalyticTrials = settings.secondaryAnalyticNeeMode == 2
            ? analyticLightCount
            : (settings.secondaryAnalyticNeeMode == 1 && analyticLightCount > 0 ? Min(settings.secondaryAnalyticNeeSamples, analyticLightCount) : 0);
        neeTrials += secondarySurfaceCount * (secondarySelectedTrials + secondaryAnalyticTrials);
    }
    return settings.samplesPerPixel * (1 + diffuseRayCount + reflectionRayCount + transmissionRayCount + neeTrials);
}

PathTraceDispatchTileSettings BuildPathTraceDispatchTileSettings(int outputWidth, int outputHeight, int estimatedRaysPerPixel)
{
    PathTraceDispatchTileSettings settings;
    const int safeOutputWidth = Max(0, outputWidth);
    const int safeOutputHeight = Max(0, outputHeight);
    const uint64 estimatedRaysPerOutputPixel = static_cast<uint64>(Max(1, estimatedRaysPerPixel));
    settings.tileWidth = safeOutputWidth;
    settings.tileHeight = safeOutputHeight;
    settings.estimatedRaysFullFrame =
        static_cast<uint64>(safeOutputWidth) *
        static_cast<uint64>(safeOutputHeight) *
        estimatedRaysPerOutputPixel;

    if (safeOutputWidth <= 0 || safeOutputHeight <= 0 || r_pathTracingDispatchTileEnable.GetInteger() == 0)
    {
        settings.estimatedRaysPerTile = settings.estimatedRaysFullFrame;
        return settings;
    }

    settings.enabled = true;
    settings.tileWidth = idMath::ClampInt(1, safeOutputWidth, r_pathTracingDispatchTileWidth.GetInteger());
    settings.tileHeight = idMath::ClampInt(1, safeOutputHeight, r_pathTracingDispatchTileHeight.GetInteger());
    settings.tileColumns = (safeOutputWidth + settings.tileWidth - 1) / settings.tileWidth;
    settings.tileRows = (safeOutputHeight + settings.tileHeight - 1) / settings.tileHeight;
    settings.tileCount = settings.tileColumns * settings.tileRows;
    settings.estimatedRaysPerTile =
        static_cast<uint64>(settings.tileWidth) *
        static_cast<uint64>(settings.tileHeight) *
        estimatedRaysPerOutputPixel;
    return settings;
}

double PathTraceMicrosecondsToMilliseconds(uint64 elapsedUs)
{
    return static_cast<double>(elapsedUs) / 1000.0;
}

}

size_t GetPathTraceSmokeConstantsSize()
{
    return sizeof(PathTraceSmokeConstants);
}
void PathTracePrimaryPass::ExecuteRayTracingSmokeTest(const viewDef_t* viewDef)
{
    OPTICK_EVENT("PT Dispatch");

    const uint64 executeStartUs = Sys_Microseconds();
    // Scene construction may route static hits through the portal-bucket
    // resident pool while retaining the monolithic buffers as owned fallback
    // resources. Every dispatch-local binding must follow the published scene
    // handles so InstanceID + PrimitiveIndex addresses the same geometry that
    // was used to build the active TLAS.
    const nvrhi::BufferHandle dispatchStaticVertexBuffer =
        m_sceneInputs.geometry.staticVertexBuffer
            ? m_sceneInputs.geometry.staticVertexBuffer
            : m_smokeStaticVertexBuffer;
    const nvrhi::BufferHandle dispatchStaticIndexBuffer =
        m_sceneInputs.geometry.staticIndexBuffer
            ? m_sceneInputs.geometry.staticIndexBuffer
            : m_smokeStaticIndexBuffer;
    const nvrhi::BufferHandle dispatchStaticTriangleClassBuffer =
        m_sceneInputs.geometry.staticTriangleClassBuffer
            ? m_sceneInputs.geometry.staticTriangleClassBuffer
            : m_smokeStaticTriangleClassBuffer;
    const nvrhi::BufferHandle dispatchStaticTriangleMaterialBuffer =
        m_sceneInputs.geometry.staticTriangleMaterialBuffer
            ? m_sceneInputs.geometry.staticTriangleMaterialBuffer
            : m_smokeStaticTriangleMaterialBuffer;
    const nvrhi::BufferHandle dispatchStaticTriangleMaterialIndexBuffer =
        m_sceneInputs.geometry.staticTriangleMaterialIndexBuffer
            ? m_sceneInputs.geometry.staticTriangleMaterialIndexBuffer
            : m_smokeStaticTriangleMaterialIndexBuffer;
    const nvrhi::BufferHandle dispatchPreviousStaticVertexBuffer =
        m_sceneInputs.geometry.previousStaticVertexBuffer
            ? m_sceneInputs.geometry.previousStaticVertexBuffer
            : m_smokePreviousStaticVertexBuffer;
    const nvrhi::BufferHandle dispatchPreviousStaticIndexBuffer =
        m_sceneInputs.geometry.previousStaticIndexBuffer
            ? m_sceneInputs.geometry.previousStaticIndexBuffer
            : m_smokePreviousStaticIndexBuffer;
    const nvrhi::BufferHandle dispatchPreviousStaticTriangleClassBuffer =
        m_sceneInputs.geometry.previousStaticTriangleClassBuffer
            ? m_sceneInputs.geometry.previousStaticTriangleClassBuffer
            : m_smokePreviousStaticTriangleClassBuffer;
    const nvrhi::BufferHandle dispatchPreviousStaticTriangleMaterialBuffer =
        m_sceneInputs.geometry.previousStaticTriangleMaterialBuffer
            ? m_sceneInputs.geometry.previousStaticTriangleMaterialBuffer
            : m_smokePreviousStaticTriangleMaterialBuffer;
    const nvrhi::BufferHandle dispatchPreviousStaticTriangleMaterialIndexBuffer =
        m_sceneInputs.geometry.previousStaticTriangleMaterialIndexBuffer
            ? m_sceneInputs.geometry.previousStaticTriangleMaterialIndexBuffer
            : m_smokePreviousStaticTriangleMaterialIndexBuffer;
    const bool cleanRtxdiDiDumpRequested = r_pathTracingCleanRtxdiDiDump.GetInteger() != 0;
    auto cleanRtxdiDiRouteLabel = [](int view) -> const char*
    {
        if (view == 1)
        {
            return "sentinel";
        }
        if (view == 2)
        {
            return "primary-surface";
        }
        if (view == 3)
        {
            return "analytic-light";
        }
        if (view == 4)
        {
            return "initial-reservoir";
        }
        if (view == 5)
        {
            return "temporal-reservoir";
        }
        if (view == 6)
        {
            return "temporal-reservoir";
        }
        if (view == 7)
        {
            return "initial-reservoir";
        }
        if (view == 8)
        {
            return "reservoir-diagnostics";
        }
        if (view == 9)
        {
            return "synthetic-temporal";
        }
        if (view == 10)
        {
            return "synthetic-analytic-temporal";
        }
        if (view == 11)
        {
            return "synthetic-overlap-temporal";
        }
        if (view == 12)
        {
            return "material-classifier-proof";
        }
        if (view == 13)
        {
            return "real-analytic-one-sample-diagnostic";
        }
        if (view == 14)
        {
            return "real-analytic-target-factor-diagnostic";
        }
        if (view == 15)
        {
            return "real-analytic-binary-gate-diagnostic";
        }
        if (view == 16)
        {
            return "real-analytic-material-validation";
        }
        if (view == 17)
        {
            return "rr-motion-vector";
        }
        if (view == 18)
        {
            return "rr-input-mosaic";
        }
        if (view == 19)
        {
            return "rr-guide-albedo";
        }
        if (view == 20)
        {
            return "rr-guide-specular-albedo";
        }
        if (view == 21)
        {
            return "rr-depth-contract";
        }
        if (view == 22)
        {
            return "primary-hit-reprojection";
        }
        if (view == 23)
        {
            return "previous-hit-reprojection";
        }
        if (view == 24)
        {
            return "material-classifier";
        }
        if (view == 25)
        {
            return "transmission-psr-mask";
        }
        return "disabled";
    };
    auto cleanRtxdiDiBehaviorLabel = [](int view) -> const char*
    {
        if (view == 1)
        {
            return "sentinel";
        }
        if (view == 2)
        {
            return "primary-surface-status";
        }
        if (view == 3)
        {
            return "analytic-light-status";
        }
        if (view == 4)
        {
            return "raw-flat-current";
        }
        if (view == 5)
        {
            return "raw-flat-temporal";
        }
        if (view == 6)
        {
            return "raw-flat-current-vs-temporal";
        }
        if (view == 7)
        {
            return "selected-light-m-history";
        }
        if (view == 8)
        {
            return "reservoir-weight-target-pdf-or-temporal-gates";
        }
        if (view == 9)
        {
            return "synthetic-constant-light-temporal";
        }
        if (view == 10)
        {
            return "synthetic-one-analytic-payload-temporal";
        }
        if (view == 11)
        {
            return "synthetic-overlapping-lights-temporal";
        }
        if (view == 12)
        {
            return "live-smoke-material-classifier-texture-proof";
        }
        if (view == 13)
        {
            return "shared-rab-one-real-doom-analytic-sample-scalars";
        }
        if (view == 14)
        {
            return "shared-rab-one-real-doom-analytic-target-factors";
        }
        if (view == 15)
        {
            return "shared-rab-one-real-doom-analytic-binary-gates";
        }
        if (view == 16)
        {
            return "shared-rab-real-doom-analytic-material-resolve";
        }
        if (view == 17)
        {
            return "clean-primary-surface-rr-motion-vector";
        }
        if (view == 18)
        {
            return "clean-primary-surface-rr-input-mosaic";
        }
        if (view == 19)
        {
            return "clean-primary-surface-rr-guide-albedo";
        }
        if (view == 20)
        {
            return "clean-primary-surface-rr-guide-specular-albedo";
        }
        if (view == 21)
        {
            return "clean-primary-surface-rr-depth-contract";
        }
        if (view == 22)
        {
            return "clean-primary-surface-hit-reprojection";
        }
        if (view == 23)
        {
            return "clean-primary-surface-previous-hit-reprojection";
        }
        if (view == 24)
        {
            return "clean-primary-surface-material-classifier";
        }
        if (view == 25)
        {
            return "clean-primary-surface-transmission-psr-mask";
        }
        return "none";
    };
    const bool cleanRtxdiDiEnabled = r_pathTracingCleanRtxdiDiEnable.GetInteger() != 0;
    const int cleanRtxdiDiView = cleanRtxdiDiEnabled ? r_pathTracingCleanRtxdiDiView.GetInteger() : 0;
    const bool cleanRtxdiDiProductionView = cleanRtxdiDiView == 16;
    const int staticBucketSecondaryProbeStage =
        r_pathTracingGeometryStaticBucketSecondaryProbeStage.GetInteger();
    const int staticBucketRouteMode =
        r_pathTracingGeometryStaticBucketRoute.GetInteger();
    const bool staticBucketBoundedTransmissionResolverRequired =
        IsSmokeStaticBucketBoundedTransmissionResolverRequired(
            staticBucketRouteMode,
            m_sceneInputs.geometry.staticBucketRoutePublicationValid);
    const bool staticBucketSecondaryMonolithicControlRequested =
        cleanRtxdiDiProductionView &&
        staticBucketRouteMode == RT_SMOKE_STATIC_BUCKET_ROUTE_DISABLED &&
        staticBucketSecondaryProbeStage == 18;
    const bool staticBucketSecondaryIsolationRequested =
        cleanRtxdiDiProductionView &&
        ((staticBucketRouteMode ==
                RT_SMOKE_STATIC_BUCKET_ROUTE_PRIMARY_OPAQUE_PROBE &&
            ((staticBucketSecondaryProbeStage >= 1 &&
                    staticBucketSecondaryProbeStage <= 17) ||
                staticBucketSecondaryProbeStage == 19 ||
                staticBucketSecondaryProbeStage == 20 ||
                staticBucketSecondaryProbeStage == 21 ||
                staticBucketSecondaryProbeStage == 22 ||
                staticBucketSecondaryProbeStage == 23 ||
                staticBucketSecondaryProbeStage == 24)) ||
            staticBucketSecondaryMonolithicControlRequested);
    const bool staticBucketSecondaryIsolationSupported =
        IsSmokeStaticBucketCleanDiSecondaryIsolationSupported(
            staticBucketRouteMode,
            cleanRtxdiDiEnabled,
            cleanRtxdiDiView,
            r_pathTracingNsightGpuMarkers.GetInteger() != 0,
            r_pathTracingCleanRestirGiEnable.GetInteger() != 0,
            r_pathTracingCleanRtxdiDiExternalPdfNeeCurrent.GetInteger() != 0 ||
                r_pathTracingRestirPdfNeeVerifierEnable.GetInteger() != 0,
            r_pathTracingCleanRtxdiDiTransmissionProducer.GetInteger() != 0 &&
                r_pathTracingCleanRtxdiDiTransmissionCompose.GetInteger() != 0,
            staticBucketSecondaryProbeStage);
    const RtSmokeStaticBucketSecondaryIsolationDispatchPlan
        staticBucketSecondaryIsolation =
            BuildSmokeStaticBucketSecondaryIsolationDispatchPlan(
                cleanRtxdiDiProductionView,
                staticBucketSecondaryIsolationRequested,
                staticBucketSecondaryIsolationSupported,
                staticBucketSecondaryMonolithicControlRequested ||
                    m_sceneInputs.geometry.
                        staticBucketRoutePublicationValid,
                staticBucketSecondaryProbeStage);
    const bool staticBucketSecondaryIsolationActive =
        staticBucketSecondaryIsolation.active;
    if (staticBucketSecondaryIsolationActive &&
        !staticBucketSecondaryIsolation.primaryPipelineCreation)
    {
        if ((m_smokeGeometryFrameIndex % 120ull) == 1ull)
        {
            common->Printf(
                "PathTracePrimaryPass: GEO-10 view-16 isolation fail-closed before pipeline creation stage=%d supported=%d publication=%d\n",
                staticBucketSecondaryIsolation.stage,
                staticBucketSecondaryIsolation.supported ? 1 : 0,
                staticBucketSecondaryIsolation.routePublicationValid ? 1 : 0);
        }
        return;
    }
    const bool cleanRtxdiDiMaterialClassifierProofView = cleanRtxdiDiView == 12 || cleanRtxdiDiView == 24;
    const bool cleanRtxdiDiTemporalEnabled =
        r_pathTracingCleanRtxdiDiTemporal.GetInteger() != 0 &&
        !cleanRtxdiDiMaterialClassifierProofView;
    const bool cleanRtxdiDiRrInputMosaicView = cleanRtxdiDiView == 18;
    const bool cleanRtxdiDiRrGuideDebugView = cleanRtxdiDiView >= 18 && cleanRtxdiDiView <= 23;
    const bool cleanRtxdiDiPsrMaskView = cleanRtxdiDiView == 25;
    const int cleanRtxdiDiResolveView = cleanRtxdiDiRrGuideDebugView ? 16 : cleanRtxdiDiView;
    const int cleanRtxdiDiMaterialFeatureView = cleanRtxdiDiPsrMaskView ? 16 : cleanRtxdiDiResolveView;
    const int cleanRtxdiDiView18Tile = idMath::ClampInt(-1, 7, r_pathTracingCleanRtxdiDiView18Tile.GetInteger());
    const uint32_t cleanRtxdiDiFrameIndexForDispatch = r_pathTracingCleanRtxdiDiFrameFreeze.GetInteger() != 0
        ? 0u
        : m_smokeCleanRtxdiDiFrameIndex;
    const bool cleanRtxdiDiRouteRequested = cleanRtxdiDiView >= 1 && cleanRtxdiDiView <= 25;
    const bool cleanRtxdiDiSpatialEnabled =
        r_pathTracingCleanRtxdiDiSpatial.GetInteger() != 0 &&
        r_cleanDiSpatial.GetInteger() != 0 &&
        r_cleanSpatial.GetInteger() != 0;
    const bool cleanRtxdiDiSpatialShaderRequested =
        cleanRtxdiDiRouteRequested &&
        !staticBucketSecondaryIsolationActive &&
        !cleanRtxdiDiMaterialClassifierProofView &&
        (cleanRtxdiDiView == 16 ||
            cleanRtxdiDiRrGuideDebugView ||
            (cleanRtxdiDiSpatialEnabled &&
                (cleanRtxdiDiView == 12 ||
                    (cleanRtxdiDiView == 8 && idMath::ClampInt(-1, 16, r_pathTracingCleanRtxdiDiView8Band.GetInteger()) == 16))));
    const bool staticBucketMaterialFeatureRuntimeRequested =
        staticBucketSecondaryIsolationActive &&
        (staticBucketSecondaryIsolation.materialFeatureRuntimeBindings ||
            staticBucketSecondaryIsolation.transmissionPsr ||
            staticBucketSecondaryIsolation.materialFeatureCompose);
    const RtPathTraceCleanRtxdiDiMaterialFeaturePasses cleanRtxdiDiMaterialFeaturePasses = BuildPathTraceCleanRtxdiDiMaterialFeaturePasses(
        cleanRtxdiDiRouteRequested &&
            (!staticBucketSecondaryIsolationActive ||
                staticBucketMaterialFeatureRuntimeRequested),
        cleanRtxdiDiMaterialFeatureView,
        m_smokeCleanRtxdiDiMaterialFeatures);
    const bool cleanExternalPdfNeeRequested = r_pathTracingCleanRtxdiDiExternalPdfNeeCurrent.GetInteger() != 0;
    const int pdfNeeVerifierEntryVisibility = idMath::ClampInt(0, 1, r_pathTracingRestirPdfNeeVerifierVisibility.GetInteger());
    const int pdfNeeVerifierSelectedVisibilityPolicy = pdfNeeVerifierEntryVisibility != 0
        ? Max(1, idMath::ClampInt(0, 2, r_pathTracingRestirPTVisibilityPolicy.GetInteger()))
        : 0;
    const bool pdfNeeRluCurrentProducerRequested = r_pathTracingRestirPdfNeeVerifierEnable.GetInteger() != 0;
    const PathTraceRemixLightManagerStats& regirRemixLightManagerStats = m_remixLightManager.GetStats();
    const bool regirRequestsRemixRabSource =
        r_pathTracingRemixLightManagerRAB.GetInteger() != 0 ||
        (r_pathTracingReGIREnable.GetInteger() != 0 && r_pathTracingReGIRMode.GetInteger() != 0);
    const bool regirUseCurrentRabLightUniverse =
        regirRequestsRemixRabSource &&
        regirRemixLightManagerStats.enabled != 0u &&
        regirRemixLightManagerStats.currentLightCount > 0u &&
        m_smokeRestirLightManagerCurrentPayloadBuffer;
    PathTraceReGIRSettings regirSettings = BuildPathTraceReGIRSettingsFromCVars();
    const bool regirSourceViewRequiresRlu =
        regirSettings.debugView >= 4 &&
        regirSettings.debugView <= 10;
    const bool regirLegacyFallbackDisabled = regirSourceViewRequiresRlu;
    PathTraceReGIRLightCounts regirLightCounts;
    regirLightCounts.analyticCount = regirUseCurrentRabLightUniverse
        ? regirRemixLightManagerStats.doomAnalyticRangeCount
        : (regirLegacyFallbackDisabled ? 0u : static_cast<uint32_t>(Max(0, m_smokeDoomAnalyticLightCount)));
    regirLightCounts.emissiveCount = regirUseCurrentRabLightUniverse
        ? regirRemixLightManagerStats.emissiveRangeCount
        : (regirLegacyFallbackDisabled ? 0u : static_cast<uint32_t>(Max(0, m_smokeEmissiveTriangleCount)));
    regirLightCounts.unifiedCount = regirUseCurrentRabLightUniverse
        ? regirRemixLightManagerStats.currentLightCount
        : (regirLegacyFallbackDisabled ? 0u : static_cast<uint32_t>(Max(0, m_smokeUnifiedLightCount)));
    PathTraceReGIRResourceDesc regirDesc = BuildPathTraceReGIRResourceDesc(regirSettings, regirLightCounts);
    nvrhi::IDevice* regirDevice = deviceManager ? deviceManager->GetDevice() : nullptr;
    const bool regirResourceReady = m_smokeReGIRState.EnsureResources(regirDevice, regirSettings, regirDesc);
    PathTraceNeeCacheSettings neeCacheSettings = BuildPathTraceNeeCacheSettingsFromCVars();
    PathTraceNeeCacheRluInputs neeCacheRluInputs;
    neeCacheRluInputs.currentLightCount = regirRemixLightManagerStats.currentLightCount;
    neeCacheRluInputs.emissiveRangeOffset = regirRemixLightManagerStats.emissiveRangeOffset;
    neeCacheRluInputs.emissiveRangeCount = regirRemixLightManagerStats.emissiveRangeCount;
    neeCacheRluInputs.doomAnalyticRangeOffset = regirRemixLightManagerStats.doomAnalyticRangeOffset;
    neeCacheRluInputs.doomAnalyticRangeCount = regirRemixLightManagerStats.doomAnalyticRangeCount;
    neeCacheRluInputs.nonEmptyRangeCount = regirRemixLightManagerStats.nonEmptyRangeCount;
    neeCacheRluInputs.remixDenseDomain =
        regirRemixLightManagerStats.enabled != 0u &&
        regirRemixLightManagerStats.currentLightCount > 0u &&
        m_smokeRestirLightManagerCurrentPayloadBuffer;
    PathTraceNeeCacheResourceDesc neeCacheDesc = BuildPathTraceNeeCacheResourceDesc(neeCacheSettings);
    const bool neeCacheResourceReady = m_smokeNeeCacheState.EnsureResources(regirDevice, neeCacheSettings, neeCacheDesc);
    const int neeCacheSecondaryVisualRefresh =
        idMath::ClampInt(0, 2, r_pathTracingNeeCacheSecondaryVisualRefresh.GetInteger());
    const bool neeCacheSecondaryVisualBandActive =
        cleanRtxdiDiRouteRequested &&
        cleanRtxdiDiView == 8 &&
        r_pathTracingCleanRtxdiDiView8Band.GetInteger() == 10;
    const bool neeCacheSecondaryVisualBandExited =
        m_smokeNeeCacheState.secondaryVisualBandActiveLastFrame &&
        !neeCacheSecondaryVisualBandActive;
    m_smokeNeeCacheState.secondaryVisualBandActiveLastFrame = neeCacheSecondaryVisualBandActive;
    if (neeCacheSecondaryVisualBandExited)
    {
        m_smokeNeeCacheState.secondaryVisualSnapshotHoldActive = false;
        m_smokeNeeCacheState.cleanProviderSnapshotHoldActive = false;
        // Re-arm clean-provider learn so we do not fall into delay=0/refresh=0/hold=false
        // and accidentally rebuild the NEE cache every frame after leaving view-8 band 10.
        m_smokeNeeCacheState.cleanProviderStartupDelayFrames = PATH_TRACE_NEE_CACHE_CLEAN_PROVIDER_STARTUP_DELAY_FRAMES;
        m_smokeNeeCacheState.cleanProviderStartupRefreshFrames = 0u;
        m_smokeNeeCacheState.cleanProviderStableViewFrames = 0u;
        m_smokeNeeCacheState.pendingInvalidationFlags |= PATH_TRACE_NEE_CACHE_INVALIDATE_DIAGNOSTIC_OWNERSHIP;
        m_smokeNeeCacheState.lastInvalidationFlags = m_smokeNeeCacheState.pendingInvalidationFlags;
        m_smokeNeeCacheState.taskClearPending = true;
    }
    const bool neeCacheSecondaryVisualRefreshRequested =
        neeCacheSecondaryVisualRefresh != 0 &&
        neeCacheSecondaryVisualBandActive;
    if (!neeCacheSecondaryVisualBandActive)
    {
        m_smokeNeeCacheState.secondaryVisualSnapshotHoldActive = false;
    }
    else
    {
        m_smokeNeeCacheState.cleanProviderSnapshotHoldActive = false;
    }
    const bool neeCacheSecondaryVisualSnapshotHold =
        neeCacheSecondaryVisualBandActive &&
        neeCacheSecondaryVisualRefresh == 0 &&
        m_smokeNeeCacheState.secondaryVisualSnapshotHoldActive;
    const bool cleanRestirGiNeeCacheDiagnosticView =
        r_pathTracingCleanRestirGiEnable.GetInteger() != 0 &&
        r_pathTracingCleanRestirGiView.GetInteger() == 20;
    const bool cleanRestirGiNeeCacheProviderRequested =
        r_pathTracingCleanRestirGiEnable.GetInteger() != 0 &&
        (r_pathTracingCleanRestirGiNeeCacheSeed.GetInteger() != 0 ||
            r_pathTracingCleanRestirGiNeeCacheSecondary.GetInteger() != 0 ||
            cleanRestirGiNeeCacheDiagnosticView);
    const bool cleanNeeCacheProviderRequestedEarly =
        (cleanRtxdiDiRouteRequested &&
            r_pathTracingCleanRtxdiDiNeeCacheProvider.GetInteger() != 0) ||
        cleanRestirGiNeeCacheProviderRequested;
    const bool cleanNeeCacheProviderJustRequested =
        cleanNeeCacheProviderRequestedEarly &&
        !m_smokeNeeCacheState.cleanProviderRequestedLastFrame;
    m_smokeNeeCacheState.cleanProviderRequestedLastFrame = cleanNeeCacheProviderRequestedEarly;
    if (!cleanNeeCacheProviderRequestedEarly)
    {
        m_smokeNeeCacheState.cleanProviderSnapshotHoldActive = false;
        m_smokeNeeCacheState.cleanProviderStartupDelayFrames = 0u;
        m_smokeNeeCacheState.cleanProviderStartupRefreshFrames = 0u;
        m_smokeNeeCacheState.cleanProviderStableViewFrames = 0u;
        m_smokeNeeCacheState.cleanProviderLastViewValid = false;
    }
    else if (cleanNeeCacheProviderJustRequested)
    {
        m_smokeNeeCacheState.cleanProviderSnapshotHoldActive = false;
        m_smokeNeeCacheState.cleanProviderStartupDelayFrames = PATH_TRACE_NEE_CACHE_CLEAN_PROVIDER_STARTUP_DELAY_FRAMES;
        m_smokeNeeCacheState.cleanProviderStartupRefreshFrames = 0u;
        m_smokeNeeCacheState.cleanProviderStableViewFrames = 0u;
        m_smokeNeeCacheState.cleanProviderLastViewValid = false;
    }
    bool cleanNeeCacheProviderViewStable = false;
    bool cleanNeeCacheProviderViewLargeJump = false;
    if (cleanNeeCacheProviderRequestedEarly && viewDef)
    {
        idVec3 cleanProviderForward = viewDef->renderView.viewaxis[0];
        cleanProviderForward.Normalize();
        const idVec3& cleanProviderOrigin = viewDef->renderView.vieworg;
        if (m_smokeNeeCacheState.cleanProviderLastViewValid)
        {
            const float dx = cleanProviderOrigin.x - m_smokeNeeCacheState.cleanProviderLastViewOrigin[0];
            const float dy = cleanProviderOrigin.y - m_smokeNeeCacheState.cleanProviderLastViewOrigin[1];
            const float dz = cleanProviderOrigin.z - m_smokeNeeCacheState.cleanProviderLastViewOrigin[2];
            const float movementSq = dx * dx + dy * dy + dz * dz;
            const float forwardDot =
                cleanProviderForward.x * m_smokeNeeCacheState.cleanProviderLastViewForward[0] +
                cleanProviderForward.y * m_smokeNeeCacheState.cleanProviderLastViewForward[1] +
                cleanProviderForward.z * m_smokeNeeCacheState.cleanProviderLastViewForward[2];
            // Mild look/walk is fine; only used for diagnostics / large-jump detection.
            // Old 0.25 / 0.9999 never settled under mouse look.
            cleanNeeCacheProviderViewStable = movementSq <= 16.0f && forwardDot >= 0.98f;
            // Teleport / hard cut: force a full relearn. Mild motion keeps hold.
            cleanNeeCacheProviderViewLargeJump = movementSq > 1024.0f || forwardDot < 0.5f;
        }
        else
        {
            // First frame after enable: treat as stable for diagnostics.
            cleanNeeCacheProviderViewStable = true;
        }
        m_smokeNeeCacheState.cleanProviderLastViewOrigin[0] = cleanProviderOrigin.x;
        m_smokeNeeCacheState.cleanProviderLastViewOrigin[1] = cleanProviderOrigin.y;
        m_smokeNeeCacheState.cleanProviderLastViewOrigin[2] = cleanProviderOrigin.z;
        m_smokeNeeCacheState.cleanProviderLastViewForward[0] = cleanProviderForward.x;
        m_smokeNeeCacheState.cleanProviderLastViewForward[1] = cleanProviderForward.y;
        m_smokeNeeCacheState.cleanProviderLastViewForward[2] = cleanProviderForward.z;
        m_smokeNeeCacheState.cleanProviderLastViewValid = true;
    }
    if (cleanNeeCacheProviderViewLargeJump && cleanNeeCacheProviderRequestedEarly)
    {
        // Only re-arm the full startup sequence on large camera jumps.
        m_smokeNeeCacheState.cleanProviderSnapshotHoldActive = false;
        m_smokeNeeCacheState.cleanProviderStartupDelayFrames = PATH_TRACE_NEE_CACHE_CLEAN_PROVIDER_STARTUP_DELAY_FRAMES;
        m_smokeNeeCacheState.cleanProviderStartupRefreshFrames = 0u;
        m_smokeNeeCacheState.cleanProviderStableViewFrames = 0u;
    }
    else if (cleanNeeCacheProviderViewStable)
    {
        ++m_smokeNeeCacheState.cleanProviderStableViewFrames;
    }
    const bool cleanNeeCacheProviderStartupDelayActive =
        cleanNeeCacheProviderRequestedEarly &&
        m_smokeNeeCacheState.cleanProviderStartupDelayFrames > 0u;
    // Count delay/refresh in wall frames so mild mouse look cannot stall the
    // learn forever and leave the provider in a permanent clear/rebuild thrash.
    if (cleanNeeCacheProviderStartupDelayActive)
    {
        --m_smokeNeeCacheState.cleanProviderStartupDelayFrames;
        if (m_smokeNeeCacheState.cleanProviderStartupDelayFrames == 0u)
        {
            m_smokeNeeCacheState.cleanProviderSnapshotHoldActive = false;
            m_smokeNeeCacheState.cleanProviderStartupRefreshFrames = PATH_TRACE_NEE_CACHE_CLEAN_PROVIDER_STARTUP_REFRESH_FRAMES;
        }
    }
    uint32_t neeCacheRluInvalidationFlags = PATH_TRACE_NEE_CACHE_INVALIDATE_NONE;
    if (regirRemixLightManagerStats.structuralSignatureChanged != 0u)
    {
        neeCacheRluInvalidationFlags |= PATH_TRACE_NEE_CACHE_INVALIDATE_RLU_STRUCTURAL;
    }
    if (regirRemixLightManagerStats.mappingSignatureChanged != 0u)
    {
        neeCacheRluInvalidationFlags |= PATH_TRACE_NEE_CACHE_INVALIDATE_RLU_MAPPING;
    }
    // Clean DI NEE-cache provider freezes a snapshot once held. Payload-only
    // luminance flicker must not force a full 4-buffer clear every frame while
    // the learn is in flight or the hold is off — that was a major perf thrash.
    if (!cleanNeeCacheProviderRequestedEarly)
    {
        if (regirRemixLightManagerStats.payloadSignatureChanged != 0u)
        {
            neeCacheRluInvalidationFlags |= PATH_TRACE_NEE_CACHE_INVALIDATE_RLU_PAYLOAD;
        }
        if (regirRemixLightManagerStats.payloadOnlyChange != 0u)
        {
            neeCacheRluInvalidationFlags |= PATH_TRACE_NEE_CACHE_INVALIDATE_RLU_PAYLOAD_ONLY;
        }
    }
    else if (
        (neeCacheRluInvalidationFlags &
            (PATH_TRACE_NEE_CACHE_INVALIDATE_RLU_STRUCTURAL | PATH_TRACE_NEE_CACHE_INVALIDATE_RLU_MAPPING)) != 0u)
    {
        // Light universe membership changed: one relearn, not a permanent thrash.
        m_smokeNeeCacheState.cleanProviderSnapshotHoldActive = false;
        m_smokeNeeCacheState.cleanProviderStartupDelayFrames = PATH_TRACE_NEE_CACHE_CLEAN_PROVIDER_STARTUP_DELAY_FRAMES;
        m_smokeNeeCacheState.cleanProviderStartupRefreshFrames = 0u;
        m_smokeNeeCacheState.cleanProviderStableViewFrames = 0u;
    }
    if (neeCacheResourceReady && neeCacheSettings.enabled && neeCacheRluInputs.remixDenseDomain && !neeCacheSecondaryVisualSnapshotHold && !m_smokeNeeCacheState.cleanProviderSnapshotHoldActive)
    {
        m_smokeNeeCacheState.ObserveRluSignatures(
            regirRemixLightManagerStats.structuralSignature,
            regirRemixLightManagerStats.mappingSignature,
            regirRemixLightManagerStats.payloadSignature,
            neeCacheRluInvalidationFlags);
    }
    const bool neeCacheDebugRouteRequested =
        neeCacheSettings.enabled &&
        (neeCacheSettings.debugView >= 1 && neeCacheSettings.debugView <= 12);
    const bool neeCacheSelectedSourceDomainAvailable =
        neeCacheRluInputs.remixDenseDomain &&
        neeCacheRluInputs.currentLightCount > 0u &&
        (neeCacheSettings.sourceDomain == 0 ||
            (neeCacheSettings.sourceDomain == 1 && neeCacheRluInputs.emissiveRangeCount > 0u) ||
            (neeCacheSettings.sourceDomain == 2 && neeCacheRluInputs.doomAnalyticRangeCount > 0u) ||
            (neeCacheSettings.sourceDomain == 3 && (neeCacheRluInputs.emissiveRangeCount > 0u || neeCacheRluInputs.doomAnalyticRangeCount > 0u)));
    const bool neeCacheCandidateBuildRequested =
        neeCacheSettings.enabled &&
        neeCacheResourceReady &&
        neeCacheSelectedSourceDomainAvailable;
    const bool neeCacheSecondaryConsumeRequested =
        r_pathTracingNeeCacheSecondaryEnable.GetInteger() != 0;
    const bool neeCacheSecondaryEmissiveDomainAvailable =
        neeCacheRluInputs.emissiveRangeCount > 0u &&
        (neeCacheSettings.sourceDomain == 0 ||
            neeCacheSettings.sourceDomain == 1 ||
            neeCacheSettings.sourceDomain == 3);
    const bool neeCacheSecondaryConsumeReady =
        neeCacheSecondaryConsumeRequested &&
        neeCacheSettings.enabled &&
        neeCacheCandidateBuildRequested &&
        neeCacheSecondaryEmissiveDomainAvailable &&
        m_smokeNeeCacheState.providerResultBuffer &&
        m_smokeNeeCacheState.cellBuffer &&
        m_smokeNeeCacheState.candidateBuffer;
    const bool cleanNeeCacheProviderBuildDeferredByClear =
        cleanNeeCacheProviderRequestedEarly &&
        m_smokeNeeCacheState.taskClearPending;
    const uint32_t cleanNeeCacheProviderDeferredClearFlags =
        cleanNeeCacheProviderBuildDeferredByClear ? m_smokeNeeCacheState.pendingInvalidationFlags : PATH_TRACE_NEE_CACHE_INVALIDATE_NONE;
    const bool cleanNeeCacheProviderClearFrameSnapshotUnsafe =
        cleanNeeCacheProviderBuildDeferredByClear &&
        (cleanNeeCacheProviderDeferredClearFlags &
            (PATH_TRACE_NEE_CACHE_INVALIDATE_RESOURCE_ALLOCATION | PATH_TRACE_NEE_CACHE_INVALIDATE_DIAGNOSTIC_OWNERSHIP)) != 0u;
    const bool cleanNeeCacheProviderStartupRefreshActive =
        cleanNeeCacheProviderRequestedEarly &&
        !cleanNeeCacheProviderStartupDelayActive &&
        m_smokeNeeCacheState.cleanProviderStartupRefreshFrames > 0u;
    // Prepass is expensive (full-screen CS over primary surfaces into 64k-cell
    // NEE buffers). Only the short post-delay refresh burst (or diagnostics) —
    // never every frame while hold is off.
    const bool cleanNeeCacheProviderBuildPrepassRequested =
        cleanNeeCacheProviderRequestedEarly &&
        !neeCacheSecondaryVisualBandActive &&
        neeCacheCandidateBuildRequested &&
        !cleanNeeCacheProviderStartupDelayActive &&
        cleanNeeCacheProviderStartupRefreshActive &&
        !neeCacheSecondaryVisualSnapshotHold;
    const bool cleanNeeCacheBuildPrepassRequested =
        cleanNeeCacheProviderBuildPrepassRequested ||
        neeCacheSecondaryVisualRefreshRequested;
    const bool neeCacheRouteRequested = neeCacheDebugRouteRequested || neeCacheCandidateBuildRequested;
    const bool regirDebugRouteRequested =
        regirSettings.enabled &&
        (regirSettings.debugView >= 1 && regirSettings.debugView <= 10);
    const bool standaloneDebugRouteRequested = regirDebugRouteRequested || neeCacheDebugRouteRequested;
    const idVec3 regirResolvedCenter = ResolvePathTraceReGIRCenter(m_smokeGeometryUniverse, regirSettings, m_smokeSceneOrigin);
    auto printCleanRtxdiDiDump = [&](const char* stage, const char* earlyReturn, int selectedCleanShaderTable)
    {
        const bool cleanEnabledNow = r_pathTracingCleanRtxdiDiEnable.GetInteger() != 0;
        const int cleanViewNow = cleanEnabledNow ? r_pathTracingCleanRtxdiDiView.GetInteger() : 0;
        const bool cleanRouteNow = cleanEnabledNow && cleanViewNow >= 1 && cleanViewNow <= 25;
        const int bindingSetReady = cleanRouteNow
            ? (m_smokeCleanRtxdiDiSentinelBindingLayout && m_frameResources.outputTexture ? 1 : 0)
            : (m_smokeBindingSet ? 1 : 0);
        const bool cleanDumpView12FullAnalyticDomain =
            cleanViewNow == 12 &&
            r_pathTracingCleanRtxdiDiView12FullAnalyticDomain.GetInteger() != 0;
        const bool cleanDumpPortalProofDomain = (!cleanDumpView12FullAnalyticDomain && cleanViewNow == 12) || cleanViewNow == 13 || cleanViewNow == 14 || cleanViewNow == 15 || cleanViewNow == 16 ||
            (cleanViewNow == 8 && r_pathTracingCleanRtxdiDiTemporal.GetInteger() != 0) ||
            (cleanViewNow == 10 && r_pathTracingCleanRtxdiDiView10PortalDomain.GetInteger() != 0);
        const uint32_t cleanDumpAnalyticDomainCount = cleanDumpPortalProofDomain
            ? static_cast<uint32_t>(Min(Max(0, m_smokeDoomAnalyticPortalRegionLightCount), Max(0, m_smokeDoomAnalyticLightCount)))
            : static_cast<uint32_t>(Max(0, m_smokeDoomAnalyticLightCount));
        const PathTraceRemixLightManagerStats& cleanDumpRluStats = m_remixLightManager.GetStats();
        const uint32_t cleanDumpRluCurrentLightCount = static_cast<uint32_t>(Max(0, m_smokeRestirLightManagerCurrentPayloadCount));
        const uint32_t cleanDumpRluPreviousLightCount = static_cast<uint32_t>(Max(0, m_smokeRestirLightManagerPreviousPayloadCount));
        const uint32_t cleanDumpRluCurrentToPreviousCount = cleanDumpRluStats.currentToPreviousCount;
        const uint32_t cleanDumpRluPreviousToCurrentCount = cleanDumpRluStats.previousToCurrentCount;
        const bool cleanDumpRluDomainAllowed =
            cleanDumpRluStats.domain == 0u ||
            cleanDumpRluStats.domain == 2u;
        const bool cleanDumpRluRoute =
            cleanEnabledNow &&
            (r_pathTracingRemixLightUniverseUseForCleanRtxdiDi.GetInteger() != 0 || pdfNeeRluCurrentProducerRequested) &&
            cleanDumpRluStats.enabled != 0u &&
            cleanDumpRluDomainAllowed &&
            cleanDumpRluCurrentLightCount > 0u &&
            m_smokeRestirLightManagerCurrentToPreviousBuffer &&
            m_smokeRestirLightManagerPreviousToCurrentBuffer &&
            m_smokeRestirLightManagerCurrentPayloadBuffer &&
            m_smokeRestirLightManagerPreviousPayloadBuffer;
        const uint32_t cleanDumpCandidateOverride = static_cast<uint32_t>(idMath::ClampInt(1, 128, r_pathTracingCleanRtxdiDiCandidateCount.GetInteger()));
        const uint32_t cleanDumpRluDoomRangeOffset = Min(cleanDumpRluStats.doomAnalyticRangeOffset, cleanDumpRluCurrentLightCount);
        const uint32_t cleanDumpRluDoomRangeCount = Min(cleanDumpRluStats.doomAnalyticRangeCount, cleanDumpRluCurrentLightCount - cleanDumpRluDoomRangeOffset);
        const uint32_t cleanDumpRluEmissiveRangeOffset = Min(cleanDumpRluStats.emissiveRangeOffset, cleanDumpRluCurrentLightCount);
        const uint32_t cleanDumpRluEmissiveRangeCount = Min(cleanDumpRluStats.emissiveRangeCount, cleanDumpRluCurrentLightCount - cleanDumpRluEmissiveRangeOffset);
        const uint32_t cleanDumpCandidateDomainCount = cleanDumpRluRoute
            ? (cleanDumpRluDoomRangeCount > 0u ? cleanDumpRluDoomRangeCount : cleanDumpRluCurrentLightCount)
            : cleanDumpAnalyticDomainCount;
        const uint32_t cleanDumpCandidateCount = (cleanViewNow == 8 || cleanViewNow == 12 || cleanViewNow == 16)
            ? Min(cleanDumpCandidateDomainCount, cleanDumpCandidateOverride)
            : 1u;
        const bool cleanDumpNeeCacheProviderRequested =
            r_pathTracingCleanRtxdiDiNeeCacheProvider.GetInteger() != 0 ||
            cleanRestirGiNeeCacheProviderRequested;
        const bool cleanDumpNeeCacheProviderReady =
            cleanDumpNeeCacheProviderRequested &&
            cleanDumpRluRoute &&
            neeCacheResourceReady &&
            neeCacheCandidateBuildRequested &&
            !cleanNeeCacheProviderStartupDelayActive &&
            !cleanNeeCacheProviderBuildDeferredByClear &&
            !m_smokeNeeCacheState.taskClearPending &&
            m_smokeNeeCacheState.cleanProviderSnapshotHoldActive &&
            m_smokeNeeCacheState.providerResultBuffer != nullptr &&
            m_smokeNeeCacheState.cellBuffer != nullptr &&
            m_smokeNeeCacheState.candidateBuffer != nullptr;
        const PathTraceRemixLightEventSample cleanDumpEmptyRluSample;
        const PathTraceRemixLightEventSample& cleanDumpPayloadCurrent = cleanDumpRluRoute ? cleanDumpRluStats.firstPayloadChangedCurrent : cleanDumpEmptyRluSample;
        const PathTraceRemixLightEventSample& cleanDumpPayloadPrevious = cleanDumpRluRoute ? cleanDumpRluStats.firstPayloadChangedPrevious : cleanDumpEmptyRluSample;
        const PathTraceRemixLightEventSample& cleanDumpCurrentOnly = cleanDumpRluRoute ? cleanDumpRluStats.firstCurrentOnly : cleanDumpEmptyRluSample;
        const PathTraceRemixLightEventSample& cleanDumpPreviousOnly = cleanDumpRluRoute ? cleanDumpRluStats.firstPreviousOnly : cleanDumpEmptyRluSample;
        common->Printf(
            "PathTracePrimaryPass: clean-room RTXDI DI dump stage=%s earlyReturn=%s enable=%d view=%d temporal=%d spatial=%d bestLights=%d lightMode=%d doomRadiusCutoff=%d frameFreeze=%d analyticDomainFreezeMs=%d bypassLightUniverse=%d doomColorSource=%d requireProvenDoomLights=%d temporalBiasCorrection=%d temporalMaxHistory=%d candidateOverride=%u view8Band=%d resolveVisibilityMode=%d resolveSolidAnglePdf=%d initialVisibility=%d resolveBrdfTarget=%d referenceRab=%d view10LightStart=%d view10LightCount=%d view10PortalDomain=%d cleanCandidates=%u remixLightUniverseRoute=%d rlu current/previous/currentToPrevious/previousToCurrent=%u/%u/%u/%u rluRanges emissive=%u+%u doomAnalytic=%u+%u rluLocal payloadChangedMapped/currentOnly/previousOnly/duplicates=%u/%u/%u/%u rluFirstPayload currentIndex/previousIndex/type/light/ids/currentXYZR/previousXYZR/currentLum/previousLum=%u/%u/%u/%u/%u:%u/%.1f:%.1f:%.1f:%.1f/%.1f:%.1f:%.1f:%.1f/%.3f/%.3f rluFirstCurrentOnly index/type/light/ids/xyzr/lum=%u/%u/%u/%u:%u/%.1f:%.1f:%.1f:%.1f/%.3f rluFirstPreviousOnly index/type/light/ids/xyzr/lum=%u/%u/%u/%u:%u/%.1f:%.1f:%.1f:%.1f/%.3f externalPdfNeeCurrent=%d cleanToyEmissiveScale=%.3f cleanAnalyticScale=%.3f cleanUseEmissiveMaps=%d cleanDisableEmissiveTriangles=%d cleanAnalyticCandidates=%d cleanNeeCacheProvider requested/ready=%d/%d buildPrepass=%d startupDelay=%u startupRefresh=%u stableView=%d stableFrames=%u deferredByClear=%d providerSnapshotHold=%d band10ExitedClear=%d providerResultSrv=t74 cellSrv=t75 candidateSrv=t77 fallbackProbability=%.3f sourceDomain=%d(%s) cellResolution=%d cellFrame=world-anchored-fixed-lod cleanNeeCacheProducer=nee-cache-stable-view-delayed-refresh-burst-or-existing-clean-initial,RTXDI_StreamSample,RTXDI_FinalizeResampling providerMissFallback=existing-clean-initial cleanReGIR enable=%d mode=%d centerMode=%d cellSize=%.2f grid=%ux%ux%u lightsPerCell=%u buildSamples=%u candidateSlots=%u firstMissing=%s route=%s behavior=%s output=%dx%d viewDef subview=%d mirror=%d superView=%d area=%d drawSurfs=%d sceneBuilt=%d coreShader=%d cleanShader=%d selectedCleanShader=%d bindingSet=%d textureTable=%d outputTex=%d accumulation=%d readback=%d cleanCurrentAnalytic=%d cleanPortalAnalytic=%d cleanCurrentAnalyticIdentity=%d cleanPreviousAnalytic=%d cleanPreviousAnalyticIdentity=%d cleanAnalyticRemap=%d cleanCurrentReservoir=%d cleanTemporalReservoir=%d cleanPreviousReservoir=%d cleanSpatialReservoir=%d cleanPreviousReservoirValid=%d cleanPreviousResetReason=%u cleanHistoryResetCount=%u cleanHistorySignature=%llu commandList=%d pages current=%s temporal=%s previous=%s spatial=%s spatialParams samples/disocclusion/radius=%d/%d/%.1f\n",
            stage ? stage : "unknown",
            earlyReturn ? earlyReturn : "none",
            cleanEnabledNow ? 1 : 0,
            cleanViewNow,
            r_pathTracingCleanRtxdiDiTemporal.GetInteger(),
            cleanRtxdiDiSpatialEnabled ? 1 : 0,
            r_pathTracingCleanRtxdiDiBestLights.GetInteger(),
            r_pathTracingCleanRtxdiDiLightMode.GetInteger(),
            r_pathTracingAnalyticLightDoomRadiusCutoff.GetBool() ? 1 : 0,
            r_pathTracingCleanRtxdiDiFrameFreeze.GetInteger() != 0 ? 1 : 0,
            r_pathTracingCleanRtxdiDiAnalyticDomainFreezeMs.GetInteger(),
            r_pathTracingCleanRtxdiDiBypassLightUniverse.GetInteger() != 0 ? 1 : 0,
            idMath::ClampInt(0, 2, r_pathTracingCleanRtxdiDiDoomColorSource.GetInteger()),
            r_pathTracingCleanRtxdiDiRequireProvenDoomLights.GetInteger() != 0 ? 1 : 0,
            r_pathTracingCleanRtxdiDiTemporalBiasCorrection.GetInteger(),
            r_pathTracingCleanRtxdiDiTemporalMaxHistory.GetInteger(),
            cleanDumpCandidateOverride,
            cleanRtxdiDiRrInputMosaicView ? cleanRtxdiDiView18Tile : idMath::ClampInt(-1, 16, r_pathTracingCleanRtxdiDiView8Band.GetInteger()),
            idMath::ClampInt(0, 3, r_pathTracingCleanRtxdiDiResolveVisibilityReuse.GetInteger()),
            r_pathTracingCleanRtxdiDiResolveSolidAnglePdf.GetInteger() != 0 ? 1 : 0,
            r_pathTracingCleanRtxdiDiInitialVisibility.GetInteger() != 0 ? 1 : 0,
            r_pathTracingCleanRtxdiDiResolveBrdfTarget.GetInteger() != 0 ? 1 : 0,
            idMath::ClampInt(0, 10, r_pathTracingCleanRtxdiDiReferenceRab.GetInteger()),
            idMath::ClampInt(0, 64, r_pathTracingCleanRtxdiDiView10LightStart.GetInteger()),
            idMath::ClampInt(1, 8, r_pathTracingCleanRtxdiDiView10LightCount.GetInteger()),
            r_pathTracingCleanRtxdiDiView10PortalDomain.GetInteger() != 0 ? 1 : 0,
            cleanDumpCandidateCount,
            cleanDumpRluRoute ? 1 : 0,
            cleanDumpRluRoute ? cleanDumpRluCurrentLightCount : 0u,
            cleanDumpRluRoute ? cleanDumpRluPreviousLightCount : 0u,
            cleanDumpRluRoute ? cleanDumpRluCurrentToPreviousCount : 0u,
            cleanDumpRluRoute ? cleanDumpRluPreviousToCurrentCount : 0u,
            cleanDumpRluRoute ? cleanDumpRluEmissiveRangeOffset : 0u,
            cleanDumpRluRoute ? cleanDumpRluEmissiveRangeCount : 0u,
            cleanDumpRluRoute ? cleanDumpRluDoomRangeOffset : 0u,
            cleanDumpRluRoute ? cleanDumpRluDoomRangeCount : 0u,
            cleanDumpRluRoute ? cleanDumpRluStats.mappedPayloadChangedCount : 0u,
            cleanDumpRluRoute ? cleanDumpRluStats.currentOnlyCount : 0u,
            cleanDumpRluRoute ? cleanDumpRluStats.previousOnlyCount : 0u,
            cleanDumpRluRoute ? cleanDumpRluStats.invalidDuplicateIdentityCount : 0u,
            cleanDumpPayloadCurrent.index,
            cleanDumpPayloadPrevious.index,
            cleanDumpPayloadCurrent.type,
            cleanDumpPayloadCurrent.materialOrLightId,
            cleanDumpPayloadCurrent.identityA,
            cleanDumpPayloadCurrent.identityB,
            cleanDumpPayloadCurrent.positionAndRadius[0],
            cleanDumpPayloadCurrent.positionAndRadius[1],
            cleanDumpPayloadCurrent.positionAndRadius[2],
            cleanDumpPayloadCurrent.positionAndRadius[3],
            cleanDumpPayloadPrevious.positionAndRadius[0],
            cleanDumpPayloadPrevious.positionAndRadius[1],
            cleanDumpPayloadPrevious.positionAndRadius[2],
            cleanDumpPayloadPrevious.positionAndRadius[3],
            cleanDumpPayloadCurrent.radianceAndLuminance[3],
            cleanDumpPayloadPrevious.radianceAndLuminance[3],
            cleanDumpCurrentOnly.index,
            cleanDumpCurrentOnly.type,
            cleanDumpCurrentOnly.materialOrLightId,
            cleanDumpCurrentOnly.identityA,
            cleanDumpCurrentOnly.identityB,
            cleanDumpCurrentOnly.positionAndRadius[0],
            cleanDumpCurrentOnly.positionAndRadius[1],
            cleanDumpCurrentOnly.positionAndRadius[2],
            cleanDumpCurrentOnly.positionAndRadius[3],
            cleanDumpCurrentOnly.radianceAndLuminance[3],
            cleanDumpPreviousOnly.index,
            cleanDumpPreviousOnly.type,
            cleanDumpPreviousOnly.materialOrLightId,
            cleanDumpPreviousOnly.identityA,
            cleanDumpPreviousOnly.identityB,
            cleanDumpPreviousOnly.positionAndRadius[0],
            cleanDumpPreviousOnly.positionAndRadius[1],
            cleanDumpPreviousOnly.positionAndRadius[2],
            cleanDumpPreviousOnly.positionAndRadius[3],
            cleanDumpPreviousOnly.radianceAndLuminance[3],
            r_pathTracingCleanRtxdiDiExternalPdfNeeCurrent.GetInteger() != 0 ? 1 : 0,
            idMath::ClampFloat(0.0f, 32.0f, r_pathTracingToyEmissiveScale.GetFloat()),
            idMath::ClampFloat(0.0f, 16.0f, r_pathTracingAnalyticLightIntensityScale.GetFloat()),
            r_pathTracingUseEmissiveMaps.GetInteger() != 0 ? 1 : 0,
            r_pathTracingDisableEmissiveTriangleSampling.GetInteger() != 0 ? 1 : 0,
            r_pathTracingAnalyticLightCandidates.GetInteger() != 0 ? 1 : 0,
            cleanDumpNeeCacheProviderRequested ? 1 : 0,
            cleanDumpNeeCacheProviderReady ? 1 : 0,
            cleanNeeCacheProviderBuildPrepassRequested ? 1 : 0,
            m_smokeNeeCacheState.cleanProviderStartupDelayFrames,
            m_smokeNeeCacheState.cleanProviderStartupRefreshFrames,
            cleanNeeCacheProviderViewStable ? 1 : 0,
            m_smokeNeeCacheState.cleanProviderStableViewFrames,
            cleanNeeCacheProviderBuildDeferredByClear ? 1 : 0,
            m_smokeNeeCacheState.cleanProviderSnapshotHoldActive ? 1 : 0,
            neeCacheSecondaryVisualBandExited ? 1 : 0,
            neeCacheSettings.fallbackProbability,
            neeCacheSettings.sourceDomain,
            PathTraceNeeCacheSourceDomainName(neeCacheSettings.sourceDomain),
            neeCacheSettings.cellResolution,
            regirSettings.enabled ? 1 : 0,
            regirSettings.mode,
            regirSettings.centerMode,
            regirSettings.cellSize,
            regirSettings.gridX,
            regirSettings.gridY,
            regirSettings.gridZ,
            regirSettings.lightsPerCell,
            regirSettings.buildSamples,
            regirDesc.slotCount,
            regirDesc.firstMissingContract ? regirDesc.firstMissingContract : "unknown",
            cleanRtxdiDiRouteLabel(cleanViewNow),
            cleanRtxdiDiBehaviorLabel(cleanViewNow),
            m_frameResources.width,
            m_frameResources.height,
            viewDef && viewDef->isSubview ? 1 : 0,
            viewDef && viewDef->isMirror ? 1 : 0,
            viewDef && viewDef->superView ? 1 : 0,
            viewDef ? viewDef->areaNum : -1,
            viewDef ? viewDef->numDrawSurfs : 0,
            m_smokeSceneBuilt ? 1 : 0,
            m_smokeShaderTable ? 1 : 0,
            m_smokeCleanRtxdiDiSentinelShaderTable ? 1 : 0,
            selectedCleanShaderTable,
            bindingSetReady,
            m_smokeTextureDescriptorTable ? 1 : 0,
            m_frameResources.outputTexture ? 1 : 0,
            m_frameResources.accumulationTexture ? 1 : 0,
            m_frameResources.readbackTexture ? 1 : 0,
            m_smokeDoomAnalyticLightBuffer ? m_smokeDoomAnalyticLightCount : 0,
            m_smokeDoomAnalyticLightBuffer ? m_smokeDoomAnalyticPortalRegionLightCount : 0,
            m_smokeDoomAnalyticCurrentIdentityBuffer ? m_smokeDoomAnalyticCurrentIdentityCount : 0,
            m_smokeDoomAnalyticPreviousLightBuffer ? m_smokeDoomAnalyticPreviousLightCount : 0,
            m_smokeDoomAnalyticPreviousIdentityBuffer ? m_smokeDoomAnalyticPreviousIdentityCount : 0,
            m_smokeDoomAnalyticRemapBuffer ? m_smokeDoomAnalyticRemapCount : 0,
            m_smokeCleanRtxdiDiCurrentReservoirBuffer ? 1 : 0,
            m_smokeCleanRtxdiDiTemporalReservoirBuffer ? 1 : 0,
            m_smokeCleanRtxdiDiPreviousReservoirBuffer ? 1 : 0,
            m_smokeCleanRtxdiDiSpatialReservoirBuffer ? 1 : 0,
            m_smokeCleanRtxdiDiPreviousReservoirValid ? 1 : 0,
            m_smokeCleanRtxdiDiPreviousReservoirResetReason,
            m_smokeCleanRtxdiDiHistoryResetCount,
            static_cast<unsigned long long>(m_smokeCleanRtxdiDiHistorySignature),
            (m_backend && m_backend->GL_GetCommandList()) ? 1 : 0,
            m_smokeCleanRtxdiDiCurrentReservoirBuffer ? "u69" : "none",
            m_smokeCleanRtxdiDiTemporalReservoirBuffer ? "u70" : "none",
            m_smokeCleanRtxdiDiPreviousReservoirBuffer ? "u71" : "none",
            m_smokeCleanRtxdiDiSpatialReservoirBuffer ? "u72" : "none",
            idMath::ClampInt(1, 16, r_cleanDiSpatialSamples.GetInteger()),
            idMath::ClampInt(1, 16, r_cleanDiSpatialDisocclusionSamples.GetInteger()),
            idMath::ClampFloat(1.0f, 128.0f, r_cleanDiSpatialRadius.GetFloat()));
        common->Printf(
            "PathTracePrimaryPass: clean-room RTXDI DI blue-noise proof requested=%d maskValid=%d textureBound=%d flagWillSet=%d binding=t127 eligible='DI initial/temporal/spatial plus glass reflection material-feature' whiteNoise='NEE-cache direct-seed/replay paths'\n",
            r_pathTracingCleanRtxdiDiBlueNoise.GetInteger() != 0 ? 1 : 0,
            m_smokeCleanRtxdiDiBlueNoise.valid ? 1 : 0,
            m_smokeCleanRtxdiDiBlueNoise.texture ? 1 : 0,
            (m_smokeCleanRtxdiDiBlueNoise.valid && r_pathTracingCleanRtxdiDiBlueNoise.GetInteger() != 0) ? 1 : 0);
    };
    if (cleanRtxdiDiDumpRequested && cleanRtxdiDiEnabled && !cleanRtxdiDiRouteRequested)
    {
        printCleanRtxdiDiDump("dispatch-entry", "clean-view-out-of-range", 0);
        r_pathTracingCleanRtxdiDiDump.SetInteger(0);
    }
    const bool cleanRtxdiDiSubview = viewDef && viewDef->isSubview;
    if (cleanRtxdiDiRouteRequested &&
        cleanRtxdiDiSubview &&
        r_pathTracingCleanRtxdiDiSubviewDispatch.GetInteger() == 0)
    {
        if (cleanRtxdiDiDumpRequested)
        {
            printCleanRtxdiDiDump("dispatch-entry", viewDef->isMirror ? "clean-mirror-subview-disabled" : "clean-subview-disabled", 0);
            r_pathTracingCleanRtxdiDiDump.SetInteger(0);
        }
        return;
    }
    const bool cleanRtxdiDiBaseResourcesValid =
        viewDef && m_smokeCleanRtxdiDiSentinelBindingLayout && m_smokeTextureDescriptorTable &&
        m_smokeCleanRtxdiDiSentinelConstantsBuffer && m_smokeMaterialFeatureRuntimeConstantsBuffer &&
        m_smokeSceneBuilt && m_smokeTlas && m_frameResources.outputTexture &&
        PathTraceCleanRtxdiDiMaterialFeatureOutputsAvailable(cleanRtxdiDiMaterialFeaturePasses, m_frameResources) &&
        dispatchStaticTriangleMaterialIndexBuffer && m_smokeDynamicTriangleMaterialIndexBuffer &&
        m_smokeRigidRouteTriangleMaterialIndexBuffer && m_smokeRigidRouteInstanceBuffer;
    const bool pdfNeeVerifierBaseResourcesValid =
        viewDef && m_smokeSceneBuilt && m_smokePdfNeeVerifierBindingLayout && m_smokeTextureDescriptorTable &&
        m_smokeTlas && m_frameResources.outputTexture && m_smokeConstantsBuffer;
    const bool regirCandidateDebugView =
        regirSettings.debugView >= 4 && regirSettings.debugView <= 10;
    const bool regirDebugCanUseAnalyticDomain =
        regirCandidateDebugView &&
        !regirSourceViewRequiresRlu &&
        (regirSettings.lightDomain == 0 || regirSettings.lightDomain == 2) &&
        regirLightCounts.analyticCount > 0;
    const bool regirDebugCanUseEmissiveDomain =
        regirCandidateDebugView &&
        !regirSourceViewRequiresRlu &&
        (regirSettings.lightDomain == 1 || regirSettings.lightDomain == 2) &&
        regirLightCounts.emissiveCount > 0;
    const bool regirDebugNeedsRluSourceBuffers =
        regirSourceViewRequiresRlu && regirCandidateDebugView;
    const bool regirDebugNeedsRabReplayBuffers =
        (regirSettings.debugView == 7 || regirSettings.debugView == 10) && regirCandidateDebugView;
    const bool regirDebugNeedsPrimarySurfaceBuffers =
        (regirSettings.debugView == 7 || regirSettings.debugView == 10) && regirCandidateDebugView;
    const bool regirDebugBaseResourcesValid =
        viewDef && m_smokeSceneBuilt && m_smokeReGIRDebugBindingLayout && m_smokeTextureDescriptorTable &&
        m_smokeTlas && m_frameResources.outputTexture && m_smokeConstantsBuffer &&
        m_smokeReGIRState.candidateCacheBuffer && m_smokeReGIRState.placeholderSrvBuffer &&
        (!regirDebugNeedsRluSourceBuffers || regirUseCurrentRabLightUniverse || regirRemixLightManagerStats.currentLightCount == 0u) &&
        (!regirDebugCanUseEmissiveDomain || m_smokeEmissiveTriangleBuffer) &&
        (!regirDebugCanUseAnalyticDomain || (m_smokeDoomAnalyticLightBuffer && (!regirDebugNeedsRabReplayBuffers || m_smokeDoomAnalyticCurrentIdentityBuffer))) &&
        (!regirDebugNeedsPrimarySurfaceBuffers || (
            dispatchStaticVertexBuffer && dispatchStaticIndexBuffer && dispatchStaticTriangleClassBuffer &&
            dispatchStaticTriangleMaterialBuffer && dispatchStaticTriangleMaterialIndexBuffer &&
            m_smokeDynamicVertexBuffer && m_smokeDynamicIndexBuffer && m_smokeDynamicTriangleClassBuffer &&
            m_smokeDynamicTriangleMaterialBuffer && m_smokeDynamicTriangleMaterialIndexBuffer &&
            m_smokeMaterialTableBuffer && m_smokeMaterialFeatureBuffer &&
            m_smokeRigidRouteVertexBuffer && m_smokeRigidRouteIndexBuffer &&
            m_smokeRigidRouteTriangleMaterialBuffer && m_smokeRigidRouteTriangleMaterialIndexBuffer &&
            m_smokeRigidRouteInstanceBuffer)) &&
        (!regirDebugNeedsRabReplayBuffers || (
            (!regirDebugCanUseEmissiveDomain || m_smokeEmissiveTriangleBuffer) &&
            (!regirDebugCanUseAnalyticDomain || (m_smokeDoomAnalyticLightBuffer && m_smokeDoomAnalyticCurrentIdentityBuffer))));
    const bool neeCacheDebugBaseResourcesValid =
        viewDef && m_smokeSceneBuilt && m_smokeNeeCacheDebugBindingLayout && m_smokeTextureDescriptorTable &&
        m_smokeTlas && m_frameResources.outputTexture && m_smokeConstantsBuffer &&
        m_smokeNeeCacheState.providerResultBuffer &&
        m_smokeNeeCacheState.cellBuffer &&
        m_smokeNeeCacheState.taskBuffer &&
        m_smokeNeeCacheState.candidateBuffer;
    const bool smokeBaseResourcesValid =
        viewDef && m_smokeSceneBuilt && m_smokeShaderTable && m_smokeBindingSet && m_smokeTextureDescriptorTable &&
        m_frameResources.outputTexture && m_frameResources.accumulationTexture && m_frameResources.restirPTReflectionTexture &&
        m_frameResources.rrInputColorTexture && m_frameResources.rrMotionVectorTexture && m_frameResources.rrGuideAlbedoTexture && m_frameResources.rrGuideSpecularAlbedoTexture &&
        m_frameResources.rrGuideNormalRoughnessTexture && m_frameResources.rrGuideDepthTexture && m_frameResources.rrGuideHitDistanceTexture &&
        m_frameResources.rrGuideResetMaskTexture && m_frameResources.rrGuidePositionTexture && m_frameResources.readbackTexture && m_smokeConstantsBuffer &&
        m_smokeBoundsOverlayLineBuffer && dispatchStaticVertexBuffer && dispatchStaticIndexBuffer && dispatchStaticTriangleClassBuffer &&
        dispatchStaticTriangleMaterialBuffer && dispatchStaticTriangleMaterialIndexBuffer && m_smokeDynamicVertexBuffer &&
        m_smokeDynamicIndexBuffer && m_smokeDynamicTriangleClassBuffer && m_smokeDynamicTriangleMaterialBuffer &&
        m_smokeDynamicTriangleMaterialIndexBuffer && m_smokeMaterialTableBuffer && m_smokeMaterialFeatureBuffer && m_smokeMaterialFeatureParameterBuffer && m_smokeEmissiveTriangleBuffer &&
        m_smokePreviousEmissiveTriangleBuffer && m_smokeEmissiveRemapBuffer && m_smokeEmissiveDistributionBuffer &&
        m_smokeLightCandidateBuffer && m_smokeDoomAnalyticLightBuffer && m_smokeDoomAnalyticPreviousLightBuffer &&
        m_smokeDoomAnalyticCurrentIdentityBuffer && m_smokeDoomAnalyticPreviousIdentityBuffer && m_smokeDoomAnalyticRemapBuffer &&
        m_smokeRigidRouteVertexBuffer && m_smokeRigidRouteIndexBuffer && m_smokeRigidRouteTriangleMaterialBuffer &&
        m_smokeRigidRouteTriangleMaterialIndexBuffer && m_smokeRigidRouteInstanceBuffer;
    const bool baseResourcesValid = neeCacheDebugRouteRequested ? neeCacheDebugBaseResourcesValid :
        (regirDebugRouteRequested ? regirDebugBaseResourcesValid :
        (cleanRtxdiDiRouteRequested ? cleanRtxdiDiBaseResourcesValid :
        (pdfNeeRluCurrentProducerRequested ? pdfNeeVerifierBaseResourcesValid : smokeBaseResourcesValid)));
    if (!baseResourcesValid)
    {
        if (cleanRtxdiDiDumpRequested)
        {
            printCleanRtxdiDiDump("dispatch-entry", "base-resource", 0);
            r_pathTracingCleanRtxdiDiDump.SetInteger(0);
        }
        return;
    }
    if (!standaloneDebugRouteRequested && !cleanRtxdiDiRouteRequested && !pdfNeeRluCurrentProducerRequested && !m_frameResources.primarySurfaceHistoryBuffers.IsValidFor(static_cast<uint32_t>(m_frameResources.width), static_cast<uint32_t>(m_frameResources.height)))
    {
        if (cleanRtxdiDiDumpRequested)
        {
            printCleanRtxdiDiDump("dispatch-entry", "primary-history", 0);
            r_pathTracingCleanRtxdiDiDump.SetInteger(0);
        }
        return;
    }

    nvrhi::ICommandList* commandList = m_backend ? m_backend->GL_GetCommandList() : nullptr;
    if (!commandList)
    {
        if (cleanRtxdiDiDumpRequested)
        {
            printCleanRtxdiDiDump("dispatch-entry", "command-list", 0);
            r_pathTracingCleanRtxdiDiDump.SetInteger(0);
        }
        return;
    }
    nvrhi::IDevice* device = deviceManager ? deviceManager->GetDevice() : nullptr;
    const bool optickGpuMarkers = r_pathTracingOptickGpuMarkers.GetInteger() != 0;
    const bool nsightGpuMarkers = r_pathTracingNsightGpuMarkers.GetInteger() != 0;
    if (optickGpuMarkers)
    {
        OPTICK_GPU_CONTEXT((void*)commandList->getNativeObject(GetPathTraceCommandObjectType()));
    }

    if (r_pathTracingSkyCubeProbe.GetInteger() != 0)
    {
        const bool skyCubeProbeReady =
            device &&
            m_smokeSkyEnvironmentCube &&
            m_smokeSkyCubeProbeBindingSet &&
            m_smokeSkyCubeProbePipeline &&
            m_smokeSkyCubeProbeOutputTexture &&
            m_smokeSkyCubeProbeReadbackTexture;
        if (skyCubeProbeReady && !m_smokeSkyCubeProbeReadbackQueued)
        {
            commandList->setTextureState(
                m_smokeSkyCubeProbeOutputTexture,
                nvrhi::AllSubresources,
                nvrhi::ResourceStates::UnorderedAccess);
            commandList->commitBarriers();
            commandList->clearTextureFloat(
                m_smokeSkyCubeProbeOutputTexture,
                nvrhi::AllSubresources,
                nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));

            nvrhi::ComputeState skyCubeProbeState;
            skyCubeProbeState.pipeline = m_smokeSkyCubeProbePipeline;
            skyCubeProbeState.bindings = { m_smokeSkyCubeProbeBindingSet };
            commandList->setComputeState(skyCubeProbeState);
            commandList->dispatch(6, 1, 1);
            nvrhi::utils::TextureUavBarrier(commandList, m_smokeSkyCubeProbeOutputTexture);

            commandList->setTextureState(
                m_smokeSkyCubeProbeOutputTexture,
                nvrhi::AllSubresources,
                nvrhi::ResourceStates::CopySource);
            commandList->commitBarriers();
            commandList->copyTexture(
                m_smokeSkyCubeProbeReadbackTexture,
                nvrhi::TextureSlice(),
                m_smokeSkyCubeProbeOutputTexture,
                nvrhi::TextureSlice());
            commandList->setTextureState(
                m_smokeSkyCubeProbeOutputTexture,
                nvrhi::AllSubresources,
                nvrhi::ResourceStates::UnorderedAccess);
            commandList->commitBarriers();

            m_smokeSkyCubeProbeReadbackQueued = true;
            m_smokeSkyCubeProbeReadbackDelayFrames = 2;
            common->Printf(
                "PathTracePrimaryPass: isolated sky-cube compute probe dispatched source='%s'\n",
                m_smokeSkyEnvironmentSourceName.c_str());
        }
        else if (!m_smokeSkyCubeProbeReadbackQueued)
        {
            common->Printf(
                "PathTracePrimaryPass: isolated sky-cube compute probe unavailable cube/binding/pipeline/output/readback=%d/%d/%d/%d/%d\n",
                m_smokeSkyEnvironmentCube ? 1 : 0,
                m_smokeSkyCubeProbeBindingSet ? 1 : 0,
                m_smokeSkyCubeProbePipeline ? 1 : 0,
                m_smokeSkyCubeProbeOutputTexture ? 1 : 0,
                m_smokeSkyCubeProbeReadbackTexture ? 1 : 0);
        }
        r_pathTracingSkyCubeProbe.SetInteger(0);
    }

    if (cleanRtxdiDiRouteRequested)
    {
        const bool cleanExternalPdfNeeCurrent = cleanExternalPdfNeeRequested || pdfNeeRluCurrentProducerRequested;
        if (!staticBucketSecondaryIsolationActive)
        {
            if ((r_pathTracingCleanRtxdiDiNeeCacheProvider.GetInteger() != 0 ||
                cleanRestirGiNeeCacheProviderRequested) &&
                m_smokeNeeCacheState.taskClearPending &&
                m_smokeNeeCacheState.providerResultBuffer &&
                m_smokeNeeCacheState.taskBuffer &&
                m_smokeNeeCacheState.cellBuffer &&
                m_smokeNeeCacheState.candidateBuffer)
            {
                SetBufferStateIfPresent(commandList, m_smokeNeeCacheState.providerResultBuffer, nvrhi::ResourceStates::UnorderedAccess);
                SetBufferStateIfPresent(commandList, m_smokeNeeCacheState.taskBuffer, nvrhi::ResourceStates::UnorderedAccess);
                SetBufferStateIfPresent(commandList, m_smokeNeeCacheState.cellBuffer, nvrhi::ResourceStates::UnorderedAccess);
                SetBufferStateIfPresent(commandList, m_smokeNeeCacheState.candidateBuffer, nvrhi::ResourceStates::UnorderedAccess);
                commandList->commitBarriers();
                commandList->clearBufferUInt(m_smokeNeeCacheState.providerResultBuffer, 0u);
                commandList->clearBufferUInt(m_smokeNeeCacheState.taskBuffer, 0u);
                commandList->clearBufferUInt(m_smokeNeeCacheState.cellBuffer, 0u);
                commandList->clearBufferUInt(m_smokeNeeCacheState.candidateBuffer, 0u);
                nvrhi::utils::BufferUavBarrier(commandList, m_smokeNeeCacheState.providerResultBuffer);
                nvrhi::utils::BufferUavBarrier(commandList, m_smokeNeeCacheState.taskBuffer);
                nvrhi::utils::BufferUavBarrier(commandList, m_smokeNeeCacheState.cellBuffer);
                nvrhi::utils::BufferUavBarrier(commandList, m_smokeNeeCacheState.candidateBuffer);
                m_smokeNeeCacheState.pendingInvalidationFlags = PATH_TRACE_NEE_CACHE_INVALIDATE_NONE;
                m_smokeNeeCacheState.taskClearPending = false;
            }
        }
        if (!staticBucketSecondaryIsolationActive ||
            staticBucketSecondaryIsolation.cleanDiPipelineCreation)
        {
            const bool cleanDiCorePipelinesMissing =
                !m_smokeCleanRtxdiDiSentinelShaderTable ||
                !m_smokeCleanRtxdiDiInitialShaderTable ||
                !m_smokeCleanRtxdiDiTemporalShaderTable ||
                (cleanRtxdiDiProductionView &&
                    (!m_smokeCleanRtxdiDiInitialProductionShaderTable ||
                        !m_smokeCleanRtxdiDiTemporalProductionShaderTable));
            if (cleanDiCorePipelinesMissing)
            {
                if (staticBucketSecondaryIsolationActive)
                {
                    common->Printf(
                        "PathTracePrimaryPass: GEO-10 core DI pipeline creation begin (stage=%d deferredHost=1)\n",
                        staticBucketSecondaryIsolation.stage);
                }
                InitRayTracingSmokeRestirPipeline(15);
            }
            if (!m_smokeCleanRtxdiDiSentinelShaderTable ||
                !m_smokeCleanRtxdiDiInitialShaderTable ||
                !m_smokeCleanRtxdiDiTemporalShaderTable ||
                (cleanRtxdiDiProductionView &&
                    (!m_smokeCleanRtxdiDiInitialProductionShaderTable ||
                        !m_smokeCleanRtxdiDiTemporalProductionShaderTable)))
            {
                if (cleanRtxdiDiDumpRequested)
                {
                    printCleanRtxdiDiDump("dispatch-entry", "clean-shader", 0);
                    r_pathTracingCleanRtxdiDiDump.SetInteger(0);
                }
                return;
            }
            if (staticBucketSecondaryIsolationActive &&
                cleanDiCorePipelinesMissing)
            {
                common->Printf(
                    "PathTracePrimaryPass: GEO-10 core DI pipeline creation completed (stage=%d); secondary DispatchRays still blocked\n",
                    staticBucketSecondaryIsolation.stage);
            }
        }
        const bool staticBucketSpatialPipelineCreation =
            staticBucketSecondaryIsolationActive &&
            staticBucketSecondaryIsolation.spatialPipelineCreation;
        const bool cleanSpatialPipelineRequested =
            cleanRtxdiDiSpatialShaderRequested ||
            staticBucketSpatialPipelineCreation;
        const bool cleanSpatialPipelineMissing =
            !m_smokeCleanRtxdiDiSpatialShaderTable ||
            (cleanRtxdiDiProductionView &&
                !m_smokeCleanRtxdiDiSpatialProductionShaderTable);
        if (cleanSpatialPipelineRequested &&
            cleanSpatialPipelineMissing)
        {
            if (staticBucketSpatialPipelineCreation)
            {
                common->Printf(
                    "PathTracePrimaryPass: GEO-10 spatial pipeline creation begin (stage=%d deferredHost=1)\n",
                    staticBucketSecondaryIsolation.stage);
            }
            InitRayTracingSmokeRestirPipeline(20);
        }
        if (cleanSpatialPipelineRequested &&
            (!m_smokeCleanRtxdiDiSpatialShaderTable ||
                (cleanRtxdiDiProductionView &&
                    !m_smokeCleanRtxdiDiSpatialProductionShaderTable)))
        {
            if (cleanRtxdiDiDumpRequested)
            {
                printCleanRtxdiDiDump("dispatch-entry", "clean-spatial-shader", 0);
                r_pathTracingCleanRtxdiDiDump.SetInteger(0);
            }
            return;
        }
        if (staticBucketSpatialPipelineCreation &&
            cleanSpatialPipelineMissing)
        {
            common->Printf(
                "PathTracePrimaryPass: GEO-10 spatial pipeline creation completed (stage=%d); spatial DispatchRays still blocked\n",
                staticBucketSecondaryIsolation.stage);
        }
        const bool staticBucketMaterialFeaturePipelineCreation =
            staticBucketSecondaryIsolationActive &&
            staticBucketSecondaryIsolation.materialFeaturePipelineCreation;
        if (staticBucketMaterialFeaturePipelineCreation)
        {
            const RtPathTraceCleanRtxdiDiPipelineContext
                cleanRtxdiDiPipelineContext =
                    BuildPathTraceCleanRtxdiDiPipelineContext(
                        m_smokeTestInitialized,
                        m_smokeCleanRtxdiDiSentinelBindingLayout,
                        m_smokeTextureBindlessLayout);
            if (!m_smokeTestDispatched)
            {
                common->Printf(
                    "PathTracePrimaryPass: GEO-10 material-feature pipeline creation begin (stage=%d deferredHost=1)\n",
                    staticBucketSecondaryIsolation.stage);
            }
            if (!EnsurePathTraceCleanRtxdiDiMaterialFeatureLayoutPipelines(
                cleanRtxdiDiMaterialFeaturePasses,
                cleanRtxdiDiPipelineContext))
            {
                if (cleanRtxdiDiDumpRequested)
                {
                    printCleanRtxdiDiDump(
                        "dispatch-entry",
                        "clean-material-feature-layout-shader",
                        0);
                    r_pathTracingCleanRtxdiDiDump.SetInteger(0);
                }
                return;
            }
            if (!m_smokeTestDispatched)
            {
                common->Printf(
                    "PathTracePrimaryPass: GEO-10 material-feature pipeline creation completed (stage=%d); transmission/glass DispatchRays still blocked\n",
                    staticBucketSecondaryIsolation.stage);
            }
        }
        if (!staticBucketSecondaryIsolationActive)
        {
            const RtPathTraceCleanRtxdiDiPipelineContext cleanRtxdiDiPipelineContext =
                BuildPathTraceCleanRtxdiDiPipelineContext(
                    m_smokeTestInitialized,
                    m_smokeCleanRtxdiDiSentinelBindingLayout,
                    m_smokeTextureBindlessLayout);
            if (!EnsurePathTraceCleanRtxdiDiMaterialFeaturePassPipelines(
                cleanRtxdiDiMaterialFeaturePasses,
                cleanRtxdiDiPipelineContext))
            {
                if (cleanRtxdiDiDumpRequested)
                {
                    printCleanRtxdiDiDump("dispatch-entry", "clean-transmission-shader", 0);
                    r_pathTracingCleanRtxdiDiDump.SetInteger(0);
                }
                return;
            }
            if (cleanExternalPdfNeeCurrent && !m_smokeRestirPdfNeeRluCurrentShaderTable)
            {
                InitRayTracingSmokeRestirPipeline(18);
            }
            if (cleanExternalPdfNeeCurrent && !m_smokeRestirPdfNeeRluCurrentShaderTable)
            {
                if (cleanRtxdiDiDumpRequested)
                {
                    printCleanRtxdiDiDump("dispatch-entry", "pdfnee-rlu-current-producer-shader", 0);
                    r_pathTracingCleanRtxdiDiDump.SetInteger(0);
                }
                return;
            }
        }
        nvrhi::IDevice* device = deviceManager ? deviceManager->GetDevice() : nullptr;
        if (!device)
        {
            if (cleanRtxdiDiDumpRequested)
            {
                printCleanRtxdiDiDump("dispatch-entry", "device", 0);
                r_pathTracingCleanRtxdiDiDump.SetInteger(0);
            }
            return;
        }
        if (cleanNeeCacheBuildPrepassRequested &&
            staticBucketSecondaryIsolation.neeCachePrimaryUpdate &&
            (!m_smokeNeeCachePrimarySurfaceUpdatePipeline || !m_smokeNeeCachePrimarySurfaceUpdateBindingLayout))
        {
            if (cleanRtxdiDiDumpRequested)
            {
                printCleanRtxdiDiDump("dispatch-entry", "nee-cache-primary-surface-update-shader", 0);
                r_pathTracingCleanRtxdiDiDump.SetInteger(0);
            }
            return;
        }

        if (cleanRtxdiDiView >= 2 && cleanRtxdiDiView <= 25)
        {
            if (!m_smokePrimarySurfaceProducerShaderTable)
            {
                if (staticBucketSecondaryIsolationActive)
                {
                    common->Printf(
                        "PathTracePrimaryPass: GEO-10 primary pipeline creation begin (stage=%d deferredHost=1)\n",
                        staticBucketSecondaryIsolation.stage);
                }
                InitRayTracingSmokeRestirPipeline(9);
            }
            if (!m_smokePrimarySurfaceProducerShaderTable)
            {
                if (cleanRtxdiDiDumpRequested)
                {
                    printCleanRtxdiDiDump("dispatch-entry", "primary-surface-shader", 0);
                    r_pathTracingCleanRtxdiDiDump.SetInteger(0);
                }
                return;
            }
            if (staticBucketSecondaryIsolationActive &&
                !staticBucketSecondaryIsolation.primaryDispatch)
            {
                if (!m_smokeTestDispatched)
                {
                    common->Printf(
                        "PathTracePrimaryPass: GEO-10 primary pipeline creation completed; stage 1 returns before DispatchRays\n");
                }
                m_smokeTestDispatched = true;
                return;
            }

            const bool primarySurfaceAdapterResourcesValid =
                m_smokeSceneBuilt && m_smokeBindingSet && m_smokeTextureDescriptorTable && m_smokeConstantsBuffer &&
                dispatchStaticVertexBuffer && dispatchStaticIndexBuffer && dispatchStaticTriangleClassBuffer &&
                dispatchStaticTriangleMaterialBuffer && dispatchStaticTriangleMaterialIndexBuffer &&
                m_smokeDynamicVertexBuffer && m_smokeDynamicIndexBuffer && m_smokeDynamicTriangleClassBuffer &&
                m_smokeDynamicTriangleMaterialBuffer && m_smokeDynamicTriangleMaterialIndexBuffer &&
                m_smokeMaterialTableBuffer && m_smokeMaterialFeatureBuffer && m_smokeMaterialFeatureParameterBuffer && m_smokeRigidRouteVertexBuffer && m_smokeRigidRouteIndexBuffer &&
                m_smokeRigidRouteTriangleMaterialBuffer && m_smokeRigidRouteTriangleMaterialIndexBuffer &&
                m_smokeRigidRouteInstanceBuffer && dispatchPreviousStaticVertexBuffer && dispatchPreviousStaticIndexBuffer &&
                dispatchPreviousStaticTriangleClassBuffer && dispatchPreviousStaticTriangleMaterialBuffer &&
                dispatchPreviousStaticTriangleMaterialIndexBuffer &&
                m_frameResources.primarySurfaceHistoryBuffers.IsValidFor(static_cast<uint32_t>(m_frameResources.width), static_cast<uint32_t>(m_frameResources.height)) &&
                m_frameResources.motionVectorTexture && m_frameResources.rrMotionVectorTexture && m_frameResources.motionVectorMaskTexture &&
                m_frameResources.rrGuideAlbedoTexture && m_frameResources.rrGuideSpecularAlbedoTexture &&
                m_frameResources.rrGuideNormalRoughnessTexture && m_frameResources.rrGuideDepthTexture &&
                m_frameResources.rrGuideHitDistanceTexture && m_frameResources.rrGuideResetMaskTexture &&
                m_frameResources.rrGuidePositionTexture;
            if (!primarySurfaceAdapterResourcesValid)
            {
                if (cleanRtxdiDiDumpRequested)
                {
                    printCleanRtxdiDiDump("dispatch-entry", "primary-surface-input", 0);
                    r_pathTracingCleanRtxdiDiDump.SetInteger(0);
                }
                return;
            }

            idVec3 cleanCameraOrigin = viewDef->renderView.vieworg;
            idVec3 cleanCameraForward = viewDef->renderView.viewaxis[0];
            idVec3 cleanCameraLeft = viewDef->renderView.viewaxis[1];
            idVec3 cleanCameraUp = viewDef->renderView.viewaxis[2];
            cleanCameraForward.Normalize();
            cleanCameraLeft.Normalize();
            cleanCameraUp.Normalize();
            const bool cleanDlssRrJitterEnabled =
                !cleanRtxdiDiMaterialClassifierProofView &&
                (cleanRtxdiDiView == 12 || cleanRtxdiDiResolveView == 16) &&
                r_pathTracingDLSSRR.GetInteger() != 0;
            const idVec2 cleanDlssRrJitterPixels = PathTraceDLSSRRPixelJitter(viewDef, cleanRtxdiDiFrameIndexForDispatch, cleanDlssRrJitterEnabled);

            PathTraceSmokeConstants primarySurfaceConstants = {};
            primarySurfaceConstants.cameraOriginAndTMax[0] = cleanCameraOrigin.x;
            primarySurfaceConstants.cameraOriginAndTMax[1] = cleanCameraOrigin.y;
            primarySurfaceConstants.cameraOriginAndTMax[2] = cleanCameraOrigin.z;
            primarySurfaceConstants.cameraOriginAndTMax[3] = 100000.0f;
            primarySurfaceConstants.cameraForwardAndTanX[0] = cleanCameraForward.x;
            primarySurfaceConstants.cameraForwardAndTanX[1] = cleanCameraForward.y;
            primarySurfaceConstants.cameraForwardAndTanX[2] = cleanCameraForward.z;
            primarySurfaceConstants.cameraForwardAndTanX[3] = idMath::Tan(DEG2RAD(viewDef->renderView.fov_x * 0.5f));
            primarySurfaceConstants.cameraLeftAndTanY[0] = cleanCameraLeft.x;
            primarySurfaceConstants.cameraLeftAndTanY[1] = cleanCameraLeft.y;
            primarySurfaceConstants.cameraLeftAndTanY[2] = cleanCameraLeft.z;
            primarySurfaceConstants.cameraLeftAndTanY[3] = idMath::Tan(DEG2RAD(viewDef->renderView.fov_y * 0.5f));
            primarySurfaceConstants.cameraUpAndDebugMode[0] = cleanCameraUp.x;
            primarySurfaceConstants.cameraUpAndDebugMode[1] = cleanCameraUp.y;
            primarySurfaceConstants.cameraUpAndDebugMode[2] = cleanCameraUp.z;
            primarySurfaceConstants.cameraUpAndDebugMode[3] = 0.0f;
            const bool cleanPreviousHistoryViewValid =
                m_frameResources.primarySurfaceHistoryView.valid &&
                m_frameResources.primarySurfaceHistoryView.width == m_frameResources.width &&
                m_frameResources.primarySurfaceHistoryView.height == m_frameResources.height &&
                !m_frameResources.primarySurfaceHistoryNeedsClear;
            primarySurfaceConstants.prevCameraOriginAndValid[0] = m_frameResources.primarySurfaceHistoryView.origin.x;
            primarySurfaceConstants.prevCameraOriginAndValid[1] = m_frameResources.primarySurfaceHistoryView.origin.y;
            primarySurfaceConstants.prevCameraOriginAndValid[2] = m_frameResources.primarySurfaceHistoryView.origin.z;
            primarySurfaceConstants.prevCameraOriginAndValid[3] = cleanPreviousHistoryViewValid ? 1.0f : 0.0f;
            primarySurfaceConstants.prevCameraForwardAndTanX[0] = m_frameResources.primarySurfaceHistoryView.forward.x;
            primarySurfaceConstants.prevCameraForwardAndTanX[1] = m_frameResources.primarySurfaceHistoryView.forward.y;
            primarySurfaceConstants.prevCameraForwardAndTanX[2] = m_frameResources.primarySurfaceHistoryView.forward.z;
            primarySurfaceConstants.prevCameraForwardAndTanX[3] = m_frameResources.primarySurfaceHistoryView.tanX;
            primarySurfaceConstants.prevCameraLeftAndTanY[0] = m_frameResources.primarySurfaceHistoryView.left.x;
            primarySurfaceConstants.prevCameraLeftAndTanY[1] = m_frameResources.primarySurfaceHistoryView.left.y;
            primarySurfaceConstants.prevCameraLeftAndTanY[2] = m_frameResources.primarySurfaceHistoryView.left.z;
            primarySurfaceConstants.prevCameraLeftAndTanY[3] = m_frameResources.primarySurfaceHistoryView.tanY;
            primarySurfaceConstants.prevCameraUpAndTanY[0] = m_frameResources.primarySurfaceHistoryView.up.x;
            primarySurfaceConstants.prevCameraUpAndTanY[1] = m_frameResources.primarySurfaceHistoryView.up.y;
            primarySurfaceConstants.prevCameraUpAndTanY[2] = m_frameResources.primarySurfaceHistoryView.up.z;
            primarySurfaceConstants.prevCameraUpAndTanY[3] = m_frameResources.primarySurfaceHistoryView.tanY;
            primarySurfaceConstants.textureInfo[0] = static_cast<float>(Max(0, static_cast<int>(m_smokeActiveTextureTable.size()) - 1));
            primarySurfaceConstants.textureInfo[1] = r_pathTracingTextureSampleEnable.GetInteger() != 0
                ? static_cast<float>(idMath::ClampInt(0, 2, r_pathTracingTextureSampleMethod.GetInteger()))
                : 0.0f;
            primarySurfaceConstants.textureInfo[2] = static_cast<float>(Max(0, m_smokeMaterialTableEntryCount));
            uint32_t primarySurfaceTextureFlags =
                (r_pathTracingTextureBindlessEnable.GetInteger() != 0 ? 1u : 0u) |
                (r_pathTracingTextureFilter.GetInteger() != 0 ? 2u : 0u) |
                (r_pathTracingTextureDecode.GetInteger() != 0 ? 4u : 0u) |
                (r_pathTracingUseNormalMaps.GetInteger() != 0 ? 8u : 0u) |
                (r_pathTracingUseSpecularMaps.GetInteger() != 0 ? 16u : 0u) |
                (r_pathTracingUseEmissiveMaps.GetInteger() != 0 && (cleanRtxdiDiResolveView == 16 || cleanRtxdiDiPsrMaskView) ? 32u : 0u) |
                (r_pathTracingToyFakePBRSpecular.GetInteger() != 0 && (cleanRtxdiDiResolveView == 16 || cleanRtxdiDiMaterialClassifierProofView || cleanRtxdiDiPsrMaskView) ? 128u : 0u) |
                PackPathTraceOpenPbrBrdfMode();
            if (r_pathTracingSkyCubeEnvironment.GetInteger() != 0 && m_smokeSkyEnvironmentCube)
            {
                primarySurfaceTextureFlags |= RT_SMOKE_TEXTURE_FLAG_SKY_CUBE;
            }
            primarySurfaceConstants.textureInfo[3] = static_cast<float>(primarySurfaceTextureFlags);
            primarySurfaceConstants.safetyInfo[0] = static_cast<float>(BuildPathTraceSafetyDisableMask());
            primarySurfaceConstants.safetyInfo[1] =
                idMath::ClampFloat(0.0f, 64.0f, r_pathTracingSkyCubeBrightness.GetFloat());
            primarySurfaceConstants.geometryInfo0[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.staticVertexCount));
            primarySurfaceConstants.geometryInfo0[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.staticIndexCount));
            primarySurfaceConstants.geometryInfo0[2] = static_cast<float>(Max(0, m_sceneInputs.geometry.staticTriangleCount));
            primarySurfaceConstants.geometryInfo0[3] = static_cast<float>(Max(0, m_sceneInputs.geometry.dynamicVertexCount));
            primarySurfaceConstants.geometryInfo1[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.dynamicIndexCount));
            primarySurfaceConstants.geometryInfo1[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.dynamicTriangleCount));
            primarySurfaceConstants.geometryInfo1[2] = static_cast<float>(Max(0, m_sceneInputs.geometry.rigidRouteVertexCount));
            primarySurfaceConstants.geometryInfo1[3] = static_cast<float>(Max(0, m_sceneInputs.geometry.rigidRouteIndexCount));
            primarySurfaceConstants.geometryInfo2[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.rigidRouteTriangleCount));
            primarySurfaceConstants.geometryInfo2[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.rigidRouteInstanceCount));
            primarySurfaceConstants.geometryInfo2[2] = static_cast<float>(m_frameResources.primarySurfaceHistoryBuffers.surfaceCount);
            primarySurfaceConstants.geometryInfo3[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.skinnedPreviousPositionCount));
            primarySurfaceConstants.geometryInfo3[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.skinnedSurfaceDispatchCount));
            primarySurfaceConstants.geometryInfo3[2] = static_cast<float>(Max(0, m_sceneInputs.geometry.skinnedTriangleDispatchIndexCount));
            primarySurfaceConstants.geometryInfo4[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.previousStaticVertexCount));
            primarySurfaceConstants.geometryInfo4[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.previousStaticIndexCount));
            primarySurfaceConstants.geometryInfo4[2] = static_cast<float>(Max(0, m_sceneInputs.geometry.previousStaticTriangleCount));
            primarySurfaceConstants.geometryInfo4[3] = static_cast<float>(Max(0, m_sceneInputs.geometry.previousStaticMaterialIndexCount));
            primarySurfaceConstants.staticBucketRouteInfo[0] =
                m_sceneInputs.geometry.staticBucketRouteFirstInstanceId;
            primarySurfaceConstants.staticBucketRouteInfo[1] =
                m_sceneInputs.geometry.staticBucketRoutePublicationValid
                    ? m_sceneInputs.geometry.
                        staticBucketTriangleCount
                    : 0u;
            primarySurfaceConstants.staticBucketRouteInfo[2] =
                static_cast<uint32_t>(
                    m_sceneInputs.geometry.
                        staticBucketRouteGeneration);
            primarySurfaceConstants.staticBucketRouteInfo[3] =
                static_cast<uint32_t>(
                    m_sceneInputs.geometry.
                        staticBucketRouteGeneration >> 32);
            primarySurfaceConstants.dispatchTileInfo[2] = static_cast<float>(Max(0, m_frameResources.width));
            primarySurfaceConstants.dispatchTileInfo[3] = static_cast<float>(Max(0, m_frameResources.height));
            primarySurfaceConstants.motionVectorInfo[0] = cleanRtxdiDiView >= 5 || r_pathTracingMotionVectorExport.GetInteger() != 0 ? 1.0f : 0.0f;
            primarySurfaceConstants.motionVectorInfo[3] = r_pathTracingMotionVectorDisableRigid.GetBool() ? 1.0f : 0.0f;
            primarySurfaceConstants.restirPTInfo[0] = static_cast<float>(cleanRtxdiDiFrameIndexForDispatch);
            primarySurfaceConstants.restirPTInfo[1] = r_pathTracingNormalMapFlipGreen.GetInteger() != 0 ? 1.0f : 0.0f;
            primarySurfaceConstants.rayReconstructionInfo[0] = cleanDlssRrJitterPixels.x;
            primarySurfaceConstants.rayReconstructionInfo[1] = cleanDlssRrJitterPixels.y;
            primarySurfaceConstants.rayReconstructionInfo[2] = cleanDlssRrJitterEnabled ? 1.0f : 0.0f;
            primarySurfaceConstants.rayReconstructionInfo[3] = r_znear.GetFloat();
            // RRProjectionDepthInfo for the clean-path LINEAR depth contract:
            //   .x = near, .y = far, .z = depth mode (0 normalized [0,1], 1 raw view-Z), .w unused.
            // Linear view-Z depth spreads uniformly across the range instead of the hyperbolic
            // cram that made our depth the outlier vs other DLSS-RR games. The bridge tags this
            // kBufferTypeLinearDepth with depthInverted=false and matching cameraNear/cameraFar.
            {
                const float rrNearCvar = r_pathTracingDLSSRRCameraNear.GetFloat();
                const float rrNear = Max( rrNearCvar > 0.0f ? rrNearCvar : r_znear.GetFloat(), 1.0e-4f );
                const float rrFarCvar = r_pathTracingDLSSRRCameraFar.GetFloat();
                const float rrFar = rrFarCvar > rrNear ? rrFarCvar : 100000.0f;
                primarySurfaceConstants.unifiedLightInfo[0] = rrNear;
                primarySurfaceConstants.unifiedLightInfo[1] = rrFar;
                primarySurfaceConstants.unifiedLightInfo[2] = static_cast<float>( idMath::ClampInt( 0, 2, r_pathTracingDLSSRRDepthMode.GetInteger() ) );
                primarySurfaceConstants.unifiedLightInfo[3] = 0.0f;
            }
            primarySurfaceConstants.toyPathInfo[2] = cleanRtxdiDiResolveView == 16
                ? idMath::ClampFloat(0.0f, 32.0f, r_pathTracingToyEmissiveScale.GetFloat())
                : 0.0f;
            const int primarySurfaceDynamicRecordCount = Max(0, m_sceneInputs.materials.dynamicMaterialRecordCount);
            const int primarySurfaceMaterialOverlayRecordCount = m_sceneInputs.materials.materialTableGpuStable ? primarySurfaceDynamicRecordCount : 0;
            const int requestedLiquidPoolMode = idMath::ClampInt(0, 3, r_pathTracingLiquidPoolMode.GetInteger());
            const bool liquidPoolTelemetryReady = m_liquidPoolStatusBuffer && m_liquidPoolStatusReadbackBuffer;
            const int effectiveLiquidPoolMode = liquidPoolTelemetryReady ? requestedLiquidPoolMode : 0;
            PopulatePathTraceDecalAndLiquidPoolControls(
                primarySurfaceConstants,
                effectiveLiquidPoolMode,
                liquidPoolTelemetryReady,
                primarySurfaceDynamicRecordCount,
                primarySurfaceMaterialOverlayRecordCount);
            if (requestedLiquidPoolMode != 0 && !liquidPoolTelemetryReady)
            {
                static bool liquidPoolTelemetryWarningPrinted = false;
                if (!liquidPoolTelemetryWarningPrinted)
                {
                    liquidPoolTelemetryWarningPrinted = true;
                    common->Printf("PathTracePrimaryPass: liquid-pool mode requested but status telemetry is unavailable; primary producer fails closed to mode 0\n");
                }
            }
            {
                static int lastLoggedDecalStage = -1;
                const int decalStageNow = static_cast<int>(primarySurfaceConstants.decalInfo[0]);
                if (decalStageNow != lastLoggedDecalStage)
                {
                    lastLoggedDecalStage = decalStageNow;
                    common->Printf("PathTracePrimaryPass: decal composite stage=%d offsetStep=%.3f maxOffsetIndex=%.0f modulateFloor=%.2f dynamicMaterialRecords=%d\n",
                        decalStageNow,
                        primarySurfaceConstants.decalInfo[1],
                        primarySurfaceConstants.decalInfo[2],
                        primarySurfaceConstants.decalInfo[3],
                        m_sceneInputs.materials.dynamicMaterialRecordCount);
                }
            }

            commandList->setBufferState(dispatchStaticVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(dispatchStaticIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(dispatchStaticTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(dispatchStaticTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(dispatchStaticTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            SetBufferStateIfPresent(commandList, m_smokeDynamicMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeMaterialTableBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeMaterialFeatureParameterBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteInstanceBuffer, nvrhi::ResourceStates::ShaderResource);
            SetBufferStateIfPresent(commandList, m_smokeSkinnedHitRouteRecordBuffer, nvrhi::ResourceStates::ShaderResource);
            SetBufferStateIfPresent(commandList, m_smokeSkinnedHitRouteTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
            SetBufferStateIfPresent(commandList, m_sceneInputs.geometry.skinnedSourceIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            SetBufferStateIfPresent(commandList, m_smokeSkinnedCurrentOutputVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            SetBufferStateIfPresent(commandList, m_smokeSkinnedPreviousPositionBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(dispatchPreviousStaticVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(dispatchPreviousStaticIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(dispatchPreviousStaticTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(dispatchPreviousStaticTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(dispatchPreviousStaticTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_frameResources.primarySurfaceHistoryBuffers.current, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_frameResources.primarySurfaceHistoryBuffers.previous, nvrhi::ResourceStates::UnorderedAccess);
            for (nvrhi::TextureHandle texture : m_smokeActiveTextureTable)
            {
                if (texture)
                {
                    commandList->setTextureState(texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
                }
            }
            commandList->setTextureState(m_frameResources.motionVectorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrMotionVectorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.motionVectorMaskTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideAlbedoTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideSpecularAlbedoTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideNormalRoughnessTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideDepthTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideHitDistanceTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideResetMaskTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuidePositionTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->commitBarriers();
            commandList->writeBuffer(m_smokeConstantsBuffer, &primarySurfaceConstants, sizeof(primarySurfaceConstants));
            if (requestedLiquidPoolMode != 0 && liquidPoolTelemetryReady)
            {
                commandList->setBufferState(m_liquidPoolStatusBuffer, nvrhi::ResourceStates::UnorderedAccess);
                commandList->commitBarriers();
                commandList->clearBufferUInt(m_liquidPoolStatusBuffer, 0u);
            }

            nvrhi::rt::State primarySurfaceState;
            primarySurfaceState.shaderTable = m_smokePrimarySurfaceProducerShaderTable;
            primarySurfaceState.bindings = { m_smokeBindingSet, m_smokeTextureDescriptorTable };
            commandList->setRayTracingState(primarySurfaceState);

            nvrhi::rt::DispatchRaysArguments primarySurfaceArgs;
            primarySurfaceArgs.width = m_frameResources.width;
            primarySurfaceArgs.height = m_frameResources.height;
            primarySurfaceArgs.depth = 1;
            {
                PathTraceGpuMarkerScope nsightMarker(
                    commandList,
                    staticBucketSecondaryIsolationActive
                        ? "GEO10.View16.Stage2 PrimarySurface DispatchRays"
                        : "CleanDI.P0 PrimarySurface DispatchRays",
                    nsightGpuMarkers);
                commandList->dispatchRays(primarySurfaceArgs);
            }

            nvrhi::utils::BufferUavBarrier(commandList, m_frameResources.primarySurfaceHistoryBuffers.current);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.motionVectorTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrMotionVectorTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.motionVectorMaskTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideAlbedoTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideSpecularAlbedoTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideNormalRoughnessTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideDepthTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideHitDistanceTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideResetMaskTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuidePositionTexture);

            if (cleanNeeCacheBuildPrepassRequested &&
                staticBucketSecondaryIsolation.neeCachePrimaryUpdate &&
                m_smokeNeeCachePrimarySurfaceUpdatePipeline &&
                m_smokeNeeCachePrimarySurfaceUpdateBindingLayout)
            {
                nvrhi::BindingSetDesc cleanNeeCacheBuildBindingSetDesc;
                auto cleanNeeCacheOptionalSrv = [&](const nvrhi::BufferHandle& buffer) -> nvrhi::BufferHandle {
                    return buffer ? buffer : m_smokeNeeCacheState.placeholderSrvBuffer;
                };
                cleanNeeCacheBuildBindingSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(2, m_smokeConstantsBuffer));
                cleanNeeCacheBuildBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(16, cleanNeeCacheOptionalSrv(m_smokeEmissiveTriangleBuffer)));
                cleanNeeCacheBuildBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(27, cleanNeeCacheOptionalSrv(m_smokeDoomAnalyticLightBuffer)));
                cleanNeeCacheBuildBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(30, m_frameResources.primarySurfaceHistoryBuffers.current));
                cleanNeeCacheBuildBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(42, cleanNeeCacheOptionalSrv(m_smokeDoomAnalyticCurrentIdentityBuffer)));
                cleanNeeCacheBuildBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(43, cleanNeeCacheOptionalSrv(m_smokeDoomAnalyticPreviousIdentityBuffer)));
                cleanNeeCacheBuildBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(44, cleanNeeCacheOptionalSrv(m_smokeDoomAnalyticRemapBuffer)));
                cleanNeeCacheBuildBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(45, cleanNeeCacheOptionalSrv(m_smokeDoomAnalyticPreviousLightBuffer)));
                cleanNeeCacheBuildBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(57, cleanNeeCacheOptionalSrv(m_smokePreviousEmissiveTriangleBuffer)));
                cleanNeeCacheBuildBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(58, cleanNeeCacheOptionalSrv(m_smokeEmissiveRemapBuffer)));
                cleanNeeCacheBuildBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(59, cleanNeeCacheOptionalSrv(m_smokeUnifiedLightBuffer)));
                cleanNeeCacheBuildBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(60, cleanNeeCacheOptionalSrv(m_smokeUnifiedPreviousLightBuffer)));
                cleanNeeCacheBuildBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(61, cleanNeeCacheOptionalSrv(m_smokeUnifiedLightRemapBuffer)));
                cleanNeeCacheBuildBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(64, cleanNeeCacheOptionalSrv(m_smokeRestirLightManagerCurrentToPreviousBuffer)));
                cleanNeeCacheBuildBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(65, cleanNeeCacheOptionalSrv(m_smokeRestirLightManagerPreviousToCurrentBuffer)));
                cleanNeeCacheBuildBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(66, cleanNeeCacheOptionalSrv(m_smokeRestirLightManagerCurrentPayloadBuffer)));
                cleanNeeCacheBuildBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(67, cleanNeeCacheOptionalSrv(m_smokeRestirLightManagerPreviousPayloadBuffer)));
                cleanNeeCacheBuildBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(PATH_TRACE_NEE_CACHE_BINDING_PROVIDER_RESULT_UAV, m_smokeNeeCacheState.providerResultBuffer));
                cleanNeeCacheBuildBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(PATH_TRACE_NEE_CACHE_BINDING_CELL_UAV, m_smokeNeeCacheState.cellBuffer));
                cleanNeeCacheBuildBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(PATH_TRACE_NEE_CACHE_BINDING_TASK_UAV, m_smokeNeeCacheState.taskBuffer));
                cleanNeeCacheBuildBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(PATH_TRACE_NEE_CACHE_BINDING_CANDIDATE_UAV, m_smokeNeeCacheState.candidateBuffer));
                nvrhi::BindingSetHandle cleanNeeCacheBuildBindingSet = device->createBindingSet(cleanNeeCacheBuildBindingSetDesc, m_smokeNeeCachePrimarySurfaceUpdateBindingLayout);
                if (cleanNeeCacheBuildBindingSet)
                {
                    PathTraceSmokeConstants cleanNeeCacheBuildConstants = primarySurfaceConstants;
                    cleanNeeCacheBuildConstants.restirLightManagerInfo[0] = static_cast<float>(neeCacheRluInputs.currentLightCount);
                    cleanNeeCacheBuildConstants.restirLightManagerInfo[1] = static_cast<float>(regirRemixLightManagerStats.previousLightCount);
                    cleanNeeCacheBuildConstants.restirLightManagerInfo[2] = static_cast<float>(regirRemixLightManagerStats.currentToPreviousCount);
                    cleanNeeCacheBuildConstants.restirLightManagerInfo[3] = static_cast<float>(regirRemixLightManagerStats.previousToCurrentCount);
                    cleanNeeCacheBuildConstants.restirLightManagerControlInfo[0] = 1.0f;
                    cleanNeeCacheBuildConstants.restirLightManagerControlInfo[1] = 2.0f;
                    cleanNeeCacheBuildConstants.restirLightManagerControlInfo[2] = static_cast<float>(regirRemixLightManagerStats.doomAnalyticStableCacheableCount);
                    cleanNeeCacheBuildConstants.restirLightManagerControlInfo[3] = static_cast<float>(regirRemixLightManagerStats.doomAnalyticUnstableDynamicCount);
                    cleanNeeCacheBuildConstants.restirLightManagerRangeInfo[0] = static_cast<float>(neeCacheRluInputs.emissiveRangeOffset);
                    cleanNeeCacheBuildConstants.restirLightManagerRangeInfo[1] = static_cast<float>(neeCacheRluInputs.emissiveRangeCount);
                    cleanNeeCacheBuildConstants.restirLightManagerRangeInfo[2] = static_cast<float>(neeCacheRluInputs.doomAnalyticRangeOffset);
                    cleanNeeCacheBuildConstants.restirLightManagerRangeInfo[3] = static_cast<float>(neeCacheRluInputs.doomAnalyticRangeCount);
                    cleanNeeCacheBuildConstants.restirLightManagerSampleInfo[0] = static_cast<float>(neeCacheRluInputs.emissiveRangeCount);
                    cleanNeeCacheBuildConstants.restirLightManagerSampleInfo[1] = static_cast<float>(regirRemixLightManagerStats.doomAnalyticStableCacheableCount);
                    cleanNeeCacheBuildConstants.restirLightManagerSampleInfo[2] = static_cast<float>(neeCacheRluInputs.emissiveRangeCount + regirRemixLightManagerStats.doomAnalyticStableCacheableCount);
                    cleanNeeCacheBuildConstants.restirLightManagerSampleInfo[3] = static_cast<float>((neeCacheRluInputs.emissiveRangeCount > 0u ? 1u : 0u) + (regirRemixLightManagerStats.doomAnalyticStableCacheableCount > 0u ? 1u : 0u));
                    cleanNeeCacheBuildConstants.doomAnalyticLightInfo[0] = static_cast<float>(Max(0, m_smokeDoomAnalyticLightCount));
                    cleanNeeCacheBuildConstants.doomAnalyticLightInfo[1] = static_cast<float>(idMath::ClampInt(0, 1024, r_pathTracingAnalyticLightMaxGpu.GetInteger()));
                    cleanNeeCacheBuildConstants.doomAnalyticLightInfo[2] = idMath::ClampFloat(0.0f, 16.0f, r_pathTracingAnalyticLightIntensityScale.GetFloat());
                    cleanNeeCacheBuildConstants.doomAnalyticLightInfo[3] = static_cast<float>(
                        (r_pathTracingAnalyticLightCandidates.GetBool() ? 1u : 0u) |
                        (r_pathTracingAnalyticLightReplaceSelected.GetBool() ? 2u : 0u) |
                        (r_pathTracingAnalyticLightDoomRadiusCutoff.GetBool() ? 4u : 0u));
                    cleanNeeCacheBuildConstants.toyPathInfo[2] = idMath::ClampFloat(0.0f, 32.0f, r_pathTracingToyEmissiveScale.GetFloat());
                    cleanNeeCacheBuildConstants.neeCacheInfo0[0] = neeCacheSettings.enabled ? 1.0f : 0.0f;
                    cleanNeeCacheBuildConstants.neeCacheInfo0[1] = static_cast<float>(neeCacheSettings.debugView);
                    cleanNeeCacheBuildConstants.neeCacheInfo0[2] = static_cast<float>(neeCacheSettings.mode);
                    cleanNeeCacheBuildConstants.neeCacheInfo0[3] = static_cast<float>(neeCacheSettings.sourceDomain);
                    cleanNeeCacheBuildConstants.neeCacheInfo1[0] = static_cast<float>(neeCacheSettings.cellResolution);
                    cleanNeeCacheBuildConstants.neeCacheInfo1[1] = neeCacheSettings.minRange;
                    cleanNeeCacheBuildConstants.neeCacheInfo1[2] = static_cast<float>(neeCacheSettings.cellCount);
                    cleanNeeCacheBuildConstants.neeCacheInfo1[3] = static_cast<float>(neeCacheSettings.candidateSlots);
                    cleanNeeCacheBuildConstants.neeCacheInfo2[0] = static_cast<float>(neeCacheSettings.taskSlots);
                    cleanNeeCacheBuildConstants.neeCacheInfo2[1] = neeCacheSettings.fallbackProbability;
                    cleanNeeCacheBuildConstants.neeCacheInfo2[2] = static_cast<float>(neeCacheDesc.providerResultCount);
                    cleanNeeCacheBuildConstants.neeCacheInfo2[3] = static_cast<float>(neeCacheDesc.cellCount);
                    cleanNeeCacheBuildConstants.neeCacheInfo3[0] = 3.0f;
                    cleanNeeCacheBuildConstants.neeCacheInfo3[1] = neeCacheRluInputs.remixDenseDomain ? 1.0f : 0.0f;
                    cleanNeeCacheBuildConstants.neeCacheInfo3[2] = static_cast<float>(neeCacheRluInputs.currentLightCount);
                    cleanNeeCacheBuildConstants.neeCacheInfo3[3] = static_cast<float>(m_frameResources.restirPTFrameIndex);

                    commandList->setBufferState(m_frameResources.primarySurfaceHistoryBuffers.current, nvrhi::ResourceStates::ShaderResource);
                    SetBufferStateIfPresent(commandList, m_smokeNeeCacheState.providerResultBuffer, nvrhi::ResourceStates::UnorderedAccess);
                    SetBufferStateIfPresent(commandList, m_smokeNeeCacheState.cellBuffer, nvrhi::ResourceStates::UnorderedAccess);
                    SetBufferStateIfPresent(commandList, m_smokeNeeCacheState.taskBuffer, nvrhi::ResourceStates::UnorderedAccess);
                    SetBufferStateIfPresent(commandList, m_smokeNeeCacheState.candidateBuffer, nvrhi::ResourceStates::UnorderedAccess);
                    commandList->commitBarriers();
                    commandList->writeBuffer(m_smokeConstantsBuffer, &cleanNeeCacheBuildConstants, sizeof(cleanNeeCacheBuildConstants));

                    nvrhi::ComputeState cleanNeeCacheBuildState;
                    cleanNeeCacheBuildState.pipeline = m_smokeNeeCachePrimarySurfaceUpdatePipeline;
                    cleanNeeCacheBuildState.bindings = { cleanNeeCacheBuildBindingSet };
                    commandList->setComputeState(cleanNeeCacheBuildState);
                    commandList->dispatch(
                        static_cast<uint32_t>((m_frameResources.width + 7) / 8),
                        static_cast<uint32_t>((m_frameResources.height + 7) / 8),
                        1);
                    nvrhi::utils::BufferUavBarrier(commandList, m_smokeNeeCacheState.providerResultBuffer);
                    nvrhi::utils::BufferUavBarrier(commandList, m_smokeNeeCacheState.cellBuffer);
                    nvrhi::utils::BufferUavBarrier(commandList, m_smokeNeeCacheState.candidateBuffer);
                    commandList->setBufferState(m_frameResources.primarySurfaceHistoryBuffers.current, nvrhi::ResourceStates::UnorderedAccess);
                    commandList->setBufferState(m_smokeNeeCacheState.providerResultBuffer, nvrhi::ResourceStates::ShaderResource);
                    commandList->setBufferState(m_smokeNeeCacheState.cellBuffer, nvrhi::ResourceStates::ShaderResource);
                    commandList->setBufferState(m_smokeNeeCacheState.candidateBuffer, nvrhi::ResourceStates::ShaderResource);
                    commandList->commitBarriers();
                    if (cleanNeeCacheProviderBuildPrepassRequested && !cleanNeeCacheProviderClearFrameSnapshotUnsafe)
                    {
                        if (m_smokeNeeCacheState.cleanProviderStartupRefreshFrames > 0u)
                        {
                            --m_smokeNeeCacheState.cleanProviderStartupRefreshFrames;
                        }
                        m_smokeNeeCacheState.cleanProviderSnapshotHoldActive =
                            m_smokeNeeCacheState.cleanProviderStartupRefreshFrames == 0u;
                    }
                    if (neeCacheSecondaryVisualRefresh == 1)
                    {
                        m_smokeNeeCacheState.secondaryVisualSnapshotHoldActive = true;
                        r_pathTracingNeeCacheSecondaryVisualRefresh.SetInteger(0);
                    }
                    else if (neeCacheSecondaryVisualRefresh == 2)
                    {
                        m_smokeNeeCacheState.secondaryVisualSnapshotHoldActive = false;
                    }
                }
            }
        }

        if (staticBucketSecondaryIsolationActive &&
            !staticBucketSecondaryIsolation.initial)
        {
            if (!m_smokeTestDispatched)
            {
                if (staticBucketSecondaryIsolation.cleanDiPipelineCreation)
                {
                    common->Printf(
                        "PathTracePrimaryPass: GEO-10 view-16 stage-3 core DI pipelines and primary dispatch completed (%dx%d); secondary DispatchRays skipped\n",
                        m_frameResources.width,
                        m_frameResources.height);
                }
                else
                {
                    common->Printf(
                        "PathTracePrimaryPass: GEO-10 view-16 stage-2 primary-only dispatch completed (%dx%d); secondary pipeline creation and dispatch skipped\n",
                        m_frameResources.width,
                        m_frameResources.height);
                }
            }
            m_smokeTestDispatched = true;
            return;
        }

        const bool cleanRtxdiDiNeedsPrimarySurface = cleanRtxdiDiView >= 2;
        const bool cleanRtxdiDiNeedsCurrentAnalytic = !cleanRtxdiDiMaterialClassifierProofView &&
            ((cleanRtxdiDiView >= 3 && cleanRtxdiDiView <= 8) || cleanRtxdiDiView == 10 || cleanRtxdiDiView == 12 || cleanRtxdiDiView == 13 || cleanRtxdiDiView == 14 || cleanRtxdiDiView == 15 || cleanRtxdiDiResolveView == 16);
        const bool cleanRtxdiDiNeedsAnalyticTemporalInputs = cleanRtxdiDiView == 5 || cleanRtxdiDiView == 6 ||
            (!cleanRtxdiDiMaterialClassifierProofView && cleanRtxdiDiView == 12) ||
            cleanRtxdiDiResolveView == 16 ||
            (cleanRtxdiDiView == 8 && cleanRtxdiDiTemporalEnabled);
        const bool cleanRtxdiDiBindingInputsValid =
            m_liquidPoolStatusBuffer &&
            (!cleanRtxdiDiNeedsPrimarySurface ||
                (m_frameResources.primarySurfaceHistoryBuffers.current && m_frameResources.primarySurfaceHistoryBuffers.previous &&
                    m_frameResources.motionVectorTexture && m_frameResources.rrMotionVectorTexture && m_frameResources.motionVectorMaskTexture)) &&
            (!cleanRtxdiDiNeedsCurrentAnalytic ||
                (m_smokeDoomAnalyticLightBuffer && m_smokeDoomAnalyticCurrentIdentityBuffer)) &&
            (!cleanRtxdiDiNeedsAnalyticTemporalInputs ||
                (m_smokeDoomAnalyticPreviousLightBuffer && m_smokeDoomAnalyticPreviousIdentityBuffer && m_smokeDoomAnalyticRemapBuffer));
        if (!cleanRtxdiDiBindingInputsValid)
        {
            if (cleanRtxdiDiDumpRequested)
            {
                printCleanRtxdiDiDump("dispatch-entry", cleanRtxdiDiNeedsAnalyticTemporalInputs ? "clean-temporal-input" : (cleanRtxdiDiNeedsCurrentAnalytic ? "clean-analytic-input" : "clean-primary-input"), 0);
                r_pathTracingCleanRtxdiDiDump.SetInteger(0);
            }
            return;
        }

        const uint64 cleanReservoirBlockSize = 16;
        const uint64 cleanReservoirBlocksX = (static_cast<uint64>(Max(1, m_frameResources.width)) + cleanReservoirBlockSize - 1ull) / cleanReservoirBlockSize;
        const uint64 cleanReservoirBlocksY = (static_cast<uint64>(Max(1, m_frameResources.height)) + cleanReservoirBlockSize - 1ull) / cleanReservoirBlockSize;
        const uint64 cleanReservoirCount64 = cleanReservoirBlocksX * cleanReservoirBlocksY * cleanReservoirBlockSize * cleanReservoirBlockSize;
        if (cleanReservoirCount64 > 0xffffffffull)
        {
            if (cleanRtxdiDiDumpRequested)
            {
                printCleanRtxdiDiDump("dispatch-entry", "clean-current-reservoir-size", 0);
                r_pathTracingCleanRtxdiDiDump.SetInteger(0);
            }
            return;
        }
        const uint32_t cleanReservoirCount = static_cast<uint32_t>(cleanReservoirCount64);
        const uint64_t cleanReservoirBytes = cleanReservoirCount64 * static_cast<uint64_t>(sizeof(RTXDI_PackedDIReservoir));
        auto ensureCleanReservoir = [&](nvrhi::BufferHandle& buffer, uint32_t& count, uint64_t& bytes, const char* debugName) -> bool
        {
            const bool valid =
                buffer &&
                buffer->getDesc().structStride == sizeof(RTXDI_PackedDIReservoir) &&
                buffer->getDesc().byteSize >= cleanReservoirBytes;
            if (valid)
            {
                return true;
            }

            nvrhi::BufferDesc cleanReservoirDesc;
            cleanReservoirDesc.debugName = debugName;
            cleanReservoirDesc.byteSize = cleanReservoirBytes;
            cleanReservoirDesc.structStride = sizeof(RTXDI_PackedDIReservoir);
            cleanReservoirDesc.canHaveUAVs = true;
            cleanReservoirDesc.canHaveTypedViews = false;
            cleanReservoirDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            cleanReservoirDesc.keepInitialState = true;
            buffer = device->createBuffer(cleanReservoirDesc);
            count = buffer ? cleanReservoirCount : 0u;
            bytes = buffer ? cleanReservoirBytes : 0ull;
            m_smokeCleanRtxdiDiPreviousReservoirValid = false;
            m_smokeCleanRtxdiDiPreviousReservoirResetReason = 2u;
            return buffer != nullptr;
        };
        if (!ensureCleanReservoir(m_smokeCleanRtxdiDiCurrentReservoirBuffer, m_smokeCleanRtxdiDiCurrentReservoirCount, m_smokeCleanRtxdiDiCurrentReservoirBytes, "PathTraceCleanRtxdiDiCurrentReservoirs") ||
            !ensureCleanReservoir(m_smokeCleanRtxdiDiTemporalReservoirBuffer, m_smokeCleanRtxdiDiTemporalReservoirCount, m_smokeCleanRtxdiDiTemporalReservoirBytes, "PathTraceCleanRtxdiDiTemporalReservoirs") ||
            !ensureCleanReservoir(m_smokeCleanRtxdiDiPreviousReservoirBuffer, m_smokeCleanRtxdiDiPreviousReservoirCount, m_smokeCleanRtxdiDiPreviousReservoirBytes, "PathTraceCleanRtxdiDiPreviousReservoirs") ||
            !ensureCleanReservoir(m_smokeCleanRtxdiDiSpatialReservoirBuffer, m_smokeCleanRtxdiDiSpatialReservoirCount, m_smokeCleanRtxdiDiSpatialReservoirBytes, "PathTraceCleanRtxdiDiSpatialReservoirs"))
        {
            if (cleanRtxdiDiDumpRequested)
            {
                printCleanRtxdiDiDump("dispatch-entry", "clean-reservoir-pages", 0);
                r_pathTracingCleanRtxdiDiDump.SetInteger(0);
            }
            return;
        }

        if (cleanExternalPdfNeeCurrent)
        {
            const nvrhi::TextureHandle pdfNeeFallbackTexture = !m_smokeActiveTextureTable.empty() ? m_smokeActiveTextureTable[0] : nullptr;
            if (!pdfNeeFallbackTexture)
            {
                if (cleanRtxdiDiDumpRequested)
                {
                    printCleanRtxdiDiDump("dispatch-entry", "pdfnee-producer-fallback-texture", 0);
                    r_pathTracingCleanRtxdiDiDump.SetInteger(0);
                }
                return;
            }

            const nvrhi::BufferHandle pdfNeePrepassReGIRCandidateSrv =
                m_smokeReGIRState.candidateCacheBuffer ? m_smokeReGIRState.candidateCacheBuffer : m_smokeLightCandidateBuffer;
            const nvrhi::BufferHandle pdfNeePrepassNeeCacheProviderSrv =
        m_smokeNeeCacheState.providerResultBuffer ? m_smokeNeeCacheState.providerResultBuffer : m_smokeLightCandidateBuffer;
            const nvrhi::BufferHandle pdfNeePrepassNeeCacheCellSrv =
                m_smokeNeeCacheState.cellBuffer ? m_smokeNeeCacheState.cellBuffer : m_smokeLightCandidateBuffer;
            const nvrhi::BufferHandle pdfNeePrepassNeeCacheCandidateSrv =
                m_smokeNeeCacheState.candidateBuffer ? m_smokeNeeCacheState.candidateBuffer : m_smokeLightCandidateBuffer;
            nvrhi::BindingSetDesc pdfNeePrepassBindingSetDesc;
            pdfNeePrepassBindingSetDesc.bindings = {
                nvrhi::BindingSetItem::RayTracingAccelStruct(0, m_smokeTlas),
                nvrhi::BindingSetItem::Texture_UAV(1, m_frameResources.outputTexture),
                nvrhi::BindingSetItem::ConstantBuffer(2, m_smokeConstantsBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(3, dispatchStaticVertexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(4, dispatchStaticIndexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(5, dispatchStaticTriangleClassBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(6, m_smokeDynamicVertexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(7, m_smokeDynamicIndexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(8, m_smokeDynamicTriangleClassBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(9, dispatchStaticTriangleMaterialBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(10, m_smokeDynamicTriangleMaterialBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(11, dispatchStaticTriangleMaterialIndexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(12, m_smokeDynamicTriangleMaterialIndexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(13, m_smokeMaterialTableBuffer),
                nvrhi::BindingSetItem::Texture_SRV(14, pdfNeeFallbackTexture),
                nvrhi::BindingSetItem::Texture_UAV(15, m_frameResources.accumulationTexture),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(16, m_smokeEmissiveTriangleBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(57, m_smokePreviousEmissiveTriangleBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(58, m_smokeEmissiveRemapBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(46, m_smokeEmissiveDistributionBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(17, m_smokeLightCandidateBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(21, m_smokeBoundsOverlayLineBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(22, m_smokeRigidRouteVertexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(23, m_smokeRigidRouteIndexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(24, m_smokeRigidRouteTriangleMaterialBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(25, m_smokeRigidRouteTriangleMaterialIndexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(26, m_smokeRigidRouteInstanceBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(18, m_smokeSkinnedHitRouteRecordBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(19, m_smokeSkinnedHitRouteTriangleBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(
                    28,
                    m_sceneInputs.geometry.skinnedSourceIndexBuffer
                        ? m_sceneInputs.geometry.skinnedSourceIndexBuffer
                        : m_smokeDynamicIndexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(
                    29,
                    m_smokeSkinnedCurrentOutputVertexBuffer
                        ? m_smokeSkinnedCurrentOutputVertexBuffer
                        : m_smokeDynamicVertexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(27, m_smokeDoomAnalyticLightBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(45, m_smokeDoomAnalyticPreviousLightBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(30, m_frameResources.primarySurfaceHistoryBuffers.current),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(31, m_frameResources.primarySurfaceHistoryBuffers.previous),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(32, m_smokeSkinnedPreviousPositionBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(33, m_smokeSkinnedSurfaceDispatchBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(34, dispatchPreviousStaticVertexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(35, dispatchPreviousStaticIndexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(36, dispatchPreviousStaticTriangleClassBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(37, dispatchPreviousStaticTriangleMaterialBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(38, dispatchPreviousStaticTriangleMaterialIndexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(42, m_smokeDoomAnalyticCurrentIdentityBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(43, m_smokeDoomAnalyticPreviousIdentityBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(44, m_smokeDoomAnalyticRemapBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(59, m_smokeUnifiedLightBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(60, m_smokeUnifiedPreviousLightBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(61, m_smokeUnifiedLightRemapBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(64, m_smokeRestirLightManagerCurrentToPreviousBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(65, m_smokeRestirLightManagerPreviousToCurrentBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(66, m_smokeRestirLightManagerCurrentPayloadBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(67, m_smokeRestirLightManagerPreviousPayloadBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(74, pdfNeePrepassNeeCacheProviderSrv),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(75, pdfNeePrepassNeeCacheCellSrv),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(77, pdfNeePrepassNeeCacheCandidateSrv),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(41, m_smokeSkinnedTriangleDispatchIndexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(76, m_smokeDynamicMaterialBuffer ? m_smokeDynamicMaterialBuffer : m_smokeLightCandidateBuffer),
                nvrhi::BindingSetItem::Texture_UAV(39, m_frameResources.motionVectorTexture),
                nvrhi::BindingSetItem::Texture_UAV(40, m_frameResources.motionVectorMaskTexture),
                nvrhi::BindingSetItem::Texture_UAV(47, m_frameResources.restirPTReflectionTexture),
                nvrhi::BindingSetItem::Texture_UAV(48, m_frameResources.rrGuideAlbedoTexture),
                nvrhi::BindingSetItem::Texture_UAV(49, m_frameResources.rrGuideNormalRoughnessTexture),
                nvrhi::BindingSetItem::Texture_UAV(50, m_frameResources.rrGuideDepthTexture),
                nvrhi::BindingSetItem::Texture_UAV(51, m_frameResources.rrGuideHitDistanceTexture),
                nvrhi::BindingSetItem::Texture_UAV(52, m_frameResources.rrGuideResetMaskTexture),
                nvrhi::BindingSetItem::Texture_UAV(53, m_frameResources.rrGuideSpecularAlbedoTexture),
                nvrhi::BindingSetItem::Texture_UAV(54, m_frameResources.rrInputColorTexture),
                nvrhi::BindingSetItem::Texture_UAV(78, m_frameResources.rrMotionVectorTexture),
                nvrhi::BindingSetItem::Texture_UAV(79, m_frameResources.rrGuidePositionTexture),
                nvrhi::BindingSetItem::Sampler(0, m_backend->GetCommonPasses().m_AnisotropicWrapSampler),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(69, m_smokeCleanRtxdiDiCurrentReservoirBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(70, m_smokeCleanRtxdiDiTemporalReservoirBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(71, m_smokeCleanRtxdiDiPreviousReservoirBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(73, pdfNeePrepassReGIRCandidateSrv)
            };
            nvrhi::BindingSetHandle pdfNeePrepassBindingSet = device->createBindingSet(pdfNeePrepassBindingSetDesc, m_smokePdfNeeVerifierBindingLayout);
            if (!pdfNeePrepassBindingSet)
            {
                if (cleanRtxdiDiDumpRequested)
                {
                    printCleanRtxdiDiDump("dispatch-entry", "pdfnee-producer-binding-set", 0);
                    r_pathTracingCleanRtxdiDiDump.SetInteger(0);
                }
                return;
            }

            idVec3 pdfNeeCameraOrigin = viewDef->renderView.vieworg;
            idVec3 pdfNeeCameraForward = viewDef->renderView.viewaxis[0];
            idVec3 pdfNeeCameraLeft = viewDef->renderView.viewaxis[1];
            idVec3 pdfNeeCameraUp = viewDef->renderView.viewaxis[2];
            pdfNeeCameraForward.Normalize();
            pdfNeeCameraLeft.Normalize();
            pdfNeeCameraUp.Normalize();

            const uint32_t pdfNeeSafetyDisableMask = BuildPathTraceSafetyDisableMask();
            const bool pdfNeeDisableEmissiveTriangleSampling = PathTraceSafetyDisabled(pdfNeeSafetyDisableMask, RT_PT_SAFETY_DISABLE_EMISSIVE_TRIANGLE_SAMPLING);
            const bool pdfNeeDisableAnalyticLightLoop = PathTraceSafetyDisabled(pdfNeeSafetyDisableMask, RT_PT_SAFETY_DISABLE_ANALYTIC_LIGHT_LOOP);
            PathTraceSmokeConstants pdfNeeProducerConstants = {};
            pdfNeeProducerConstants.cameraOriginAndTMax[0] = pdfNeeCameraOrigin.x;
            pdfNeeProducerConstants.cameraOriginAndTMax[1] = pdfNeeCameraOrigin.y;
            pdfNeeProducerConstants.cameraOriginAndTMax[2] = pdfNeeCameraOrigin.z;
            pdfNeeProducerConstants.cameraOriginAndTMax[3] = 100000.0f;
            pdfNeeProducerConstants.cameraForwardAndTanX[0] = pdfNeeCameraForward.x;
            pdfNeeProducerConstants.cameraForwardAndTanX[1] = pdfNeeCameraForward.y;
            pdfNeeProducerConstants.cameraForwardAndTanX[2] = pdfNeeCameraForward.z;
            pdfNeeProducerConstants.cameraForwardAndTanX[3] = idMath::Tan(DEG2RAD(viewDef->renderView.fov_x * 0.5f));
            pdfNeeProducerConstants.cameraLeftAndTanY[0] = pdfNeeCameraLeft.x;
            pdfNeeProducerConstants.cameraLeftAndTanY[1] = pdfNeeCameraLeft.y;
            pdfNeeProducerConstants.cameraLeftAndTanY[2] = pdfNeeCameraLeft.z;
            pdfNeeProducerConstants.cameraLeftAndTanY[3] = idMath::Tan(DEG2RAD(viewDef->renderView.fov_y * 0.5f));
            pdfNeeProducerConstants.cameraUpAndDebugMode[0] = pdfNeeCameraUp.x;
            pdfNeeProducerConstants.cameraUpAndDebugMode[1] = pdfNeeCameraUp.y;
            pdfNeeProducerConstants.cameraUpAndDebugMode[2] = pdfNeeCameraUp.z;
            pdfNeeProducerConstants.cameraUpAndDebugMode[3] = 0.0f;
            pdfNeeProducerConstants.toyPathInfo[2] = idMath::ClampFloat(0.0f, 32.0f, r_pathTracingToyEmissiveScale.GetFloat());
            pdfNeeProducerConstants.textureInfo[0] = static_cast<float>(Max(0, static_cast<int>(m_smokeActiveTextureTable.size()) - 1));
            pdfNeeProducerConstants.textureInfo[1] = r_pathTracingTextureSampleEnable.GetInteger() != 0
                ? static_cast<float>(idMath::ClampInt(0, 2, r_pathTracingTextureSampleMethod.GetInteger()))
                : 0.0f;
            pdfNeeProducerConstants.textureInfo[2] = static_cast<float>(Max(0, m_smokeMaterialTableEntryCount));
            const uint32_t pdfNeeTextureFlags =
                (r_pathTracingTextureBindlessEnable.GetInteger() != 0 ? 1u : 0u) |
                (r_pathTracingTextureFilter.GetInteger() != 0 ? 2u : 0u) |
                (r_pathTracingTextureDecode.GetInteger() != 0 ? 4u : 0u) |
                (r_pathTracingUseNormalMaps.GetInteger() != 0 ? 8u : 0u) |
                (r_pathTracingUseSpecularMaps.GetInteger() != 0 ? 16u : 0u) |
                (r_pathTracingUseEmissiveMaps.GetInteger() != 0 ? 32u : 0u) |
                (r_pathTracingReservoirTwoSidedEmissives.GetInteger() != 0 ? 64u : 0u) |
                PackPathTraceOpenPbrBrdfMode();
            pdfNeeProducerConstants.textureInfo[3] = static_cast<float>(pdfNeeTextureFlags);
            pdfNeeProducerConstants.emissiveInfo[0] = static_cast<float>(pdfNeeDisableEmissiveTriangleSampling ? 0 : m_smokeEmissiveTriangleCount);
            pdfNeeProducerConstants.emissiveInfo[1] = static_cast<float>(pdfNeeDisableEmissiveTriangleSampling ? 0 : m_smokeEmissiveStaticTriangleCount);
            pdfNeeProducerConstants.emissiveInfo[2] = static_cast<float>(idMath::ClampInt(1, 16, r_pathTracingReservoirCandidateTrials.GetInteger()));
            pdfNeeProducerConstants.emissiveInfo[3] = static_cast<float>(pdfNeeDisableEmissiveTriangleSampling ? 0 : m_smokeLightCandidateCount);
            const int pdfNeeEmissiveDistributionCount = !pdfNeeDisableEmissiveTriangleSampling && r_pathTracingEmissiveDistribution.GetInteger() != 0 && m_sceneInputs.lights.emissiveDistributionValid
                ? m_sceneInputs.lights.emissiveDistributionCount
                : 0;
            pdfNeeProducerConstants.emissiveDistributionInfo[0] = static_cast<float>(Max(0, pdfNeeEmissiveDistributionCount));
            pdfNeeProducerConstants.emissiveDistributionInfo[1] = pdfNeeEmissiveDistributionCount > 0 ? 1.0f : 0.0f;
            pdfNeeProducerConstants.emissiveDistributionInfo[2] = static_cast<float>(Max(0, m_sceneInputs.lights.emissiveDistributionFallbackIndex));
            pdfNeeProducerConstants.emissiveDistributionInfo[3] = static_cast<float>(
                m_sceneInputs.materials.materialTableGpuStable ? Max(0, m_sceneInputs.materials.dynamicMaterialRecordCount) : 0);
            pdfNeeProducerConstants.doomAnalyticLightInfo[0] = static_cast<float>(pdfNeeDisableAnalyticLightLoop ? 0 : m_smokeDoomAnalyticLightCount);
            pdfNeeProducerConstants.doomAnalyticLightInfo[1] = static_cast<float>(idMath::ClampInt(0, 1024, r_pathTracingAnalyticLightMaxGpu.GetInteger()));
            pdfNeeProducerConstants.doomAnalyticLightInfo[2] = idMath::ClampFloat(0.0f, 16.0f, r_pathTracingAnalyticLightIntensityScale.GetFloat());
            pdfNeeProducerConstants.doomAnalyticLightInfo[3] =
                (!pdfNeeDisableAnalyticLightLoop && m_smokeDoomAnalyticLightCount > 0 ? 1.0f : 0.0f) +
                (r_pathTracingAnalyticLightDoomRadiusCutoff.GetBool() ? 4.0f : 0.0f);
            pdfNeeProducerConstants.doomAnalyticLightRemapInfo[0] = static_cast<float>(m_smokeDoomAnalyticCurrentIdentityCount);
            pdfNeeProducerConstants.doomAnalyticLightRemapInfo[1] = static_cast<float>(m_smokeDoomAnalyticPreviousIdentityCount);
            pdfNeeProducerConstants.doomAnalyticLightRemapInfo[2] = static_cast<float>(m_smokeDoomAnalyticRemapCount);
            pdfNeeProducerConstants.doomAnalyticLightRemapInfo[3] = static_cast<float>(m_smokePreviousEmissiveTriangleCount);
            pdfNeeProducerConstants.restirPTInfo[0] = static_cast<float>(m_smokeCleanRtxdiDiFrameIndex);
            pdfNeeProducerConstants.restirPTInfo[1] = r_pathTracingNormalMapFlipGreen.GetInteger() != 0 ? 1.0f : 0.0f;
            pdfNeeProducerConstants.unifiedLightInfo[0] = static_cast<float>(Max(0, m_smokeUnifiedLightCount));
            pdfNeeProducerConstants.unifiedLightInfo[1] = static_cast<float>(Max(0, m_smokeUnifiedPreviousLightCount));
            pdfNeeProducerConstants.unifiedLightInfo[3] = static_cast<float>(Max(0, m_smokeUnifiedLightRemapCount));
            const PathTraceRemixLightManagerStats& pdfNeeRluStats = m_remixLightManager.GetStats();
            pdfNeeProducerConstants.restirLightManagerInfo[0] = static_cast<float>(pdfNeeRluStats.currentLightCount);
            pdfNeeProducerConstants.restirLightManagerInfo[1] = static_cast<float>(pdfNeeRluStats.previousLightCount);
            pdfNeeProducerConstants.restirLightManagerInfo[2] = static_cast<float>(pdfNeeRluStats.currentToPreviousCount);
            pdfNeeProducerConstants.restirLightManagerInfo[3] = static_cast<float>(pdfNeeRluStats.previousToCurrentCount);
            pdfNeeProducerConstants.restirLightManagerControlInfo[0] = pdfNeeRluStats.enabled != 0u ? 1.0f : 0.0f;
            pdfNeeProducerConstants.restirLightManagerControlInfo[1] = pdfNeeRluStats.enabled != 0u ? 2.0f : 0.0f;
            pdfNeeProducerConstants.restirLightManagerRangeInfo[0] = static_cast<float>(pdfNeeRluStats.emissiveRangeOffset);
            pdfNeeProducerConstants.restirLightManagerRangeInfo[1] = static_cast<float>(pdfNeeRluStats.emissiveRangeCount);
            pdfNeeProducerConstants.restirLightManagerRangeInfo[2] = static_cast<float>(pdfNeeRluStats.doomAnalyticRangeOffset);
            pdfNeeProducerConstants.restirLightManagerRangeInfo[3] = static_cast<float>(pdfNeeRluStats.doomAnalyticRangeCount);
            pdfNeeProducerConstants.restirLightManagerSampleInfo[0] = static_cast<float>(pdfNeeRluStats.emissiveSampleCount);
            pdfNeeProducerConstants.restirLightManagerSampleInfo[1] = static_cast<float>(pdfNeeRluStats.doomAnalyticSampleCount);
            pdfNeeProducerConstants.restirLightManagerSampleInfo[2] = static_cast<float>(pdfNeeRluStats.totalSampleCount);
            pdfNeeProducerConstants.restirLightManagerSampleInfo[3] = static_cast<float>(pdfNeeRluStats.nonEmptyRangeCount);
            pdfNeeProducerConstants.reservedRestirPdfNeeInfo[0] = 0.0f;
            pdfNeeProducerConstants.reservedRestirPdfNeeInfo[1] = 0.0f;
            pdfNeeProducerConstants.reservedRestirPdfNeeInfo[2] = 0.0f;
            pdfNeeProducerConstants.reservedRestirPdfNeeInfo[3] = 0.0f;
            pdfNeeProducerConstants.restirPdfNeeRluCurrentControlInfo[0] = static_cast<float>(idMath::ClampInt(1, 64, r_pathTracingRestirPdfNeeVerifierSamples.GetInteger()));
            pdfNeeProducerConstants.restirPdfNeeRluCurrentControlInfo[1] = static_cast<float>(pdfNeeVerifierEntryVisibility);
            pdfNeeProducerConstants.restirPdfNeeRluCurrentControlInfo[2] = static_cast<float>(idMath::ClampInt(0, 2, r_pathTracingRestirPdfNeeVerifierSourcePolicy.GetInteger()));
            pdfNeeProducerConstants.restirPdfNeeRluCurrentControlInfo[3] = 0.0f;
            pdfNeeProducerConstants.regirInfo0[0] = regirSettings.enabled ? 1.0f : 0.0f;
            pdfNeeProducerConstants.regirInfo0[1] = 0.0f;
            pdfNeeProducerConstants.regirInfo0[2] = static_cast<float>(regirSettings.mode);
            pdfNeeProducerConstants.regirInfo0[3] = static_cast<float>(regirSettings.centerMode);
            pdfNeeProducerConstants.regirInfo1[0] = regirSettings.cellSize;
            pdfNeeProducerConstants.regirInfo1[1] = static_cast<float>(regirSettings.gridX);
            pdfNeeProducerConstants.regirInfo1[2] = static_cast<float>(regirSettings.gridY);
            pdfNeeProducerConstants.regirInfo1[3] = static_cast<float>(regirSettings.gridZ);
            pdfNeeProducerConstants.regirInfo2[0] = static_cast<float>(regirSettings.lightsPerCell);
            pdfNeeProducerConstants.regirInfo2[1] = static_cast<float>(regirSettings.buildSamples);
            pdfNeeProducerConstants.regirInfo2[2] = 0.0f;
            pdfNeeProducerConstants.regirInfo2[3] = static_cast<float>(regirDesc.cellCount);
            pdfNeeProducerConstants.regirInfo3[0] = 0.0f;
            pdfNeeProducerConstants.regirInfo3[1] = static_cast<float>(regirDesc.slotCount);
            pdfNeeProducerConstants.regirInfo3[2] = 0.0f;
            pdfNeeProducerConstants.regirInfo3[3] = 0.0f;
            pdfNeeProducerConstants.regirInfo4[0] = regirResolvedCenter.x;
            pdfNeeProducerConstants.regirInfo4[1] = regirResolvedCenter.y;
            pdfNeeProducerConstants.regirInfo4[2] = regirResolvedCenter.z;
            pdfNeeProducerConstants.regirInfo4[3] = 1.0f;
            pdfNeeProducerConstants.safetyInfo[0] = static_cast<float>(pdfNeeSafetyDisableMask);
            pdfNeeProducerConstants.safetyInfo[1] = pdfNeeProducerConstants.textureInfo[0];
            pdfNeeProducerConstants.safetyInfo[2] = static_cast<float>(pdfNeeVerifierSelectedVisibilityPolicy * 16);
            pdfNeeProducerConstants.geometryInfo0[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.staticVertexCount));
            pdfNeeProducerConstants.geometryInfo0[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.staticIndexCount));
            pdfNeeProducerConstants.geometryInfo0[2] = static_cast<float>(Max(0, m_sceneInputs.geometry.staticTriangleCount));
            pdfNeeProducerConstants.geometryInfo0[3] = static_cast<float>(Max(0, m_sceneInputs.geometry.dynamicVertexCount));
            pdfNeeProducerConstants.geometryInfo1[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.dynamicIndexCount));
            pdfNeeProducerConstants.geometryInfo1[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.dynamicTriangleCount));
            pdfNeeProducerConstants.geometryInfo1[2] = static_cast<float>(Max(0, m_sceneInputs.geometry.rigidRouteVertexCount));
            pdfNeeProducerConstants.geometryInfo1[3] = static_cast<float>(Max(0, m_sceneInputs.geometry.rigidRouteIndexCount));
            pdfNeeProducerConstants.geometryInfo2[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.rigidRouteTriangleCount));
            pdfNeeProducerConstants.geometryInfo2[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.rigidRouteInstanceCount));
            pdfNeeProducerConstants.geometryInfo2[2] = static_cast<float>(m_frameResources.primarySurfaceHistoryBuffers.surfaceCount);
            pdfNeeProducerConstants.geometryInfo2[3] = 0.0f;
            pdfNeeProducerConstants.geometryInfo3[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.skinnedPreviousPositionCount));
            pdfNeeProducerConstants.geometryInfo3[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.skinnedSurfaceDispatchCount));
            pdfNeeProducerConstants.geometryInfo3[2] = static_cast<float>(Max(0, m_sceneInputs.geometry.skinnedTriangleDispatchIndexCount));
            pdfNeeProducerConstants.geometryInfo4[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.previousStaticVertexCount));
            pdfNeeProducerConstants.geometryInfo4[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.previousStaticIndexCount));
            pdfNeeProducerConstants.geometryInfo4[2] = static_cast<float>(Max(0, m_sceneInputs.geometry.previousStaticTriangleCount));
            pdfNeeProducerConstants.geometryInfo4[3] = static_cast<float>(Max(0, m_sceneInputs.geometry.previousStaticMaterialIndexCount));
            pdfNeeProducerConstants.staticBucketRouteInfo[0] =
                m_sceneInputs.geometry.staticBucketRouteFirstInstanceId;
            pdfNeeProducerConstants.staticBucketRouteInfo[1] =
                m_sceneInputs.geometry.staticBucketRoutePublicationValid
                    ? m_sceneInputs.geometry.
                        staticBucketTriangleCount
                    : 0u;
            pdfNeeProducerConstants.staticBucketRouteInfo[2] =
                static_cast<uint32_t>(
                    m_sceneInputs.geometry.
                        staticBucketRouteGeneration);
            pdfNeeProducerConstants.staticBucketRouteInfo[3] =
                static_cast<uint32_t>(
                    m_sceneInputs.geometry.
                        staticBucketRouteGeneration >> 32);
            pdfNeeProducerConstants.dispatchTileInfo[2] = static_cast<float>(Max(0, m_frameResources.width));
            pdfNeeProducerConstants.dispatchTileInfo[3] = static_cast<float>(Max(0, m_frameResources.height));
            pdfNeeProducerConstants.motionVectorInfo[3] = r_pathTracingMotionVectorDisableRigid.GetBool() ? 1.0f : 0.0f;

            commandList->setBufferState(dispatchStaticVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(dispatchStaticIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(dispatchStaticTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(dispatchStaticTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(dispatchStaticTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeMaterialTableBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeEmissiveTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokePreviousEmissiveTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeEmissiveRemapBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeEmissiveDistributionBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeLightCandidateBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDoomAnalyticLightBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDoomAnalyticPreviousLightBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDoomAnalyticCurrentIdentityBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDoomAnalyticPreviousIdentityBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDoomAnalyticRemapBuffer, nvrhi::ResourceStates::ShaderResource);
            SetBufferStateIfPresent(commandList, m_smokeSkinnedHitRouteRecordBuffer, nvrhi::ResourceStates::ShaderResource);
            SetBufferStateIfPresent(commandList, m_smokeSkinnedHitRouteTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
            SetBufferStateIfPresent(commandList, m_sceneInputs.geometry.skinnedSourceIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            SetBufferStateIfPresent(commandList, m_smokeSkinnedCurrentOutputVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            SetBufferStateIfPresent(commandList, m_smokeSkinnedPreviousPositionBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteInstanceBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeBoundsOverlayLineBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_frameResources.primarySurfaceHistoryBuffers.current, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_frameResources.primarySurfaceHistoryBuffers.previous, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_smokeCleanRtxdiDiCurrentReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_smokeCleanRtxdiDiTemporalReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_smokeCleanRtxdiDiPreviousReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
            for (nvrhi::TextureHandle texture : m_smokeActiveTextureTable)
            {
                if (texture)
                {
                    commandList->setTextureState(texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
                }
            }
            commandList->setTextureState(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.accumulationTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.motionVectorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.motionVectorMaskTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideAlbedoTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideSpecularAlbedoTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideNormalRoughnessTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideDepthTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideHitDistanceTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideResetMaskTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->commitBarriers();

            commandList->writeBuffer(m_smokeConstantsBuffer, &pdfNeeProducerConstants, sizeof(pdfNeeProducerConstants));

            nvrhi::rt::State pdfNeePrepassState;
            pdfNeePrepassState.shaderTable = m_smokeRestirPdfNeeRluCurrentShaderTable;
            pdfNeePrepassState.bindings = { pdfNeePrepassBindingSet, m_smokeTextureDescriptorTable };
            commandList->setRayTracingState(pdfNeePrepassState);

            nvrhi::rt::DispatchRaysArguments pdfNeePrepassArgs;
            pdfNeePrepassArgs.width = m_frameResources.width;
            pdfNeePrepassArgs.height = m_frameResources.height;
            pdfNeePrepassArgs.depth = 1;
            {
                PathTraceGpuMarkerScope nsightMarker(commandList, "CleanDI.P1 PdfNeeCurrent DispatchRays", nsightGpuMarkers);
                commandList->dispatchRays(pdfNeePrepassArgs);
            }
            nvrhi::utils::BufferUavBarrier(commandList, m_smokeCleanRtxdiDiCurrentReservoirBuffer);
            nvrhi::utils::BufferUavBarrier(commandList, m_frameResources.primarySurfaceHistoryBuffers.current);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.outputTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.motionVectorTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.motionVectorMaskTexture);
        }

        const nvrhi::TextureHandle cleanFallbackTexture = !m_smokeActiveTextureTable.empty() ? m_smokeActiveTextureTable[0] : nullptr;
        if (!cleanFallbackTexture)
        {
            if (cleanRtxdiDiDumpRequested)
            {
                printCleanRtxdiDiDump("dispatch-entry", "clean-fallback-texture", 0);
                r_pathTracingCleanRtxdiDiDump.SetInteger(0);
            }
            return;
        }
        auto cleanOptionalSrv = [&](const nvrhi::BufferHandle& buffer) -> nvrhi::BufferHandle {
            return buffer ? buffer : m_smokeReGIRState.placeholderSrvBuffer;
        };
        const nvrhi::BufferHandle cleanNeeCacheProviderSrv = m_smokeNeeCacheState.providerResultBuffer
            ? m_smokeNeeCacheState.providerResultBuffer
            : cleanOptionalSrv(m_smokeNeeCacheState.placeholderSrvBuffer);
        const nvrhi::BufferHandle cleanNeeCacheCellSrv = m_smokeNeeCacheState.cellBuffer
            ? m_smokeNeeCacheState.cellBuffer
            : cleanOptionalSrv(m_smokeNeeCacheState.placeholderSrvBuffer);
        const nvrhi::BufferHandle cleanNeeCacheCandidateSrv = m_smokeNeeCacheState.candidateBuffer
            ? m_smokeNeeCacheState.candidateBuffer
            : cleanOptionalSrv(m_smokeNeeCacheState.placeholderSrvBuffer);
        PathTraceEnsureBlueNoise(
            m_smokeCleanRtxdiDiBlueNoise,
            device,
            "PathTracePrimaryPass: clean-room RTXDI DI",
            "PathTraceCleanRtxdiDiBlueNoise");
        if (!m_smokeCleanRtxdiDiBlueNoise.texture)
        {
            if (cleanRtxdiDiDumpRequested)
            {
                printCleanRtxdiDiDump("dispatch-entry", "clean-blue-noise-texture", 0);
                r_pathTracingCleanRtxdiDiDump.SetInteger(0);
            }
            return;
        }

        nvrhi::BindingSetDesc cleanBindingSetDesc;
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, m_smokeTlas));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(1, m_frameResources.outputTexture));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(2, m_smokeCleanRtxdiDiSentinelConstantsBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, dispatchStaticVertexBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, dispatchStaticIndexBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(5, dispatchStaticTriangleClassBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, m_smokeDynamicVertexBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, m_smokeDynamicIndexBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(8, m_smokeDynamicTriangleClassBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(9, dispatchStaticTriangleMaterialBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(10, m_smokeDynamicTriangleMaterialBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(11, dispatchStaticTriangleMaterialIndexBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(12, m_smokeDynamicTriangleMaterialIndexBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(13, m_smokeMaterialTableBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(14, cleanFallbackTexture));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(15, cleanOptionalSrv(m_smokeDynamicMaterialBuffer)));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(16, cleanOptionalSrv(m_smokeEmissiveTriangleBuffer)));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(22, m_smokeRigidRouteVertexBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(23, m_smokeRigidRouteIndexBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(24, m_smokeRigidRouteTriangleMaterialBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(25, m_smokeRigidRouteTriangleMaterialIndexBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(26, m_smokeRigidRouteInstanceBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(18, m_smokeSkinnedHitRouteRecordBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(19, m_smokeSkinnedHitRouteTriangleBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(28, cleanOptionalSrv(m_sceneInputs.geometry.skinnedSourceIndexBuffer)));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(29, cleanOptionalSrv(m_smokeSkinnedCurrentOutputVertexBuffer)));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(32, cleanOptionalSrv(m_smokeSkinnedPreviousPositionBuffer)));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(27, cleanOptionalSrv(m_smokeDoomAnalyticLightBuffer)));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(30, m_frameResources.primarySurfaceHistoryBuffers.current));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(31, m_frameResources.primarySurfaceHistoryBuffers.previous));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(39, m_frameResources.motionVectorTexture));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(40, m_frameResources.motionVectorMaskTexture));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(42, cleanOptionalSrv(m_smokeDoomAnalyticCurrentIdentityBuffer)));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(43, cleanOptionalSrv(m_smokeDoomAnalyticPreviousIdentityBuffer)));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(44, cleanOptionalSrv(m_smokeDoomAnalyticRemapBuffer)));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(45, cleanOptionalSrv(m_smokeDoomAnalyticPreviousLightBuffer)));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(46, cleanOptionalSrv(m_smokeEmissiveDistributionBuffer)));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(48, m_frameResources.rrGuideAlbedoTexture));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(49, m_frameResources.rrGuideNormalRoughnessTexture));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(50, m_frameResources.rrGuideDepthTexture));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(51, m_frameResources.rrGuideHitDistanceTexture));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(52, m_frameResources.rrGuideResetMaskTexture));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(53, m_frameResources.rrGuideSpecularAlbedoTexture));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(54, m_frameResources.rrInputColorTexture));
        AddPathTraceCleanRtxdiDiMaterialFeatureBindings(
            cleanBindingSetDesc,
            cleanRtxdiDiMaterialFeaturePasses,
            m_smokeMaterialTableBuffer,
            m_smokeMaterialFeatureBuffer,
            m_smokeMaterialFeatureParameterBuffer,
            m_smokeMaterialFeatureRuntimeConstantsBuffer,
            m_frameResources);
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(79, m_frameResources.rrGuidePositionTexture));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(57, cleanOptionalSrv(m_smokePreviousEmissiveTriangleBuffer)));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(64, cleanOptionalSrv(m_smokeRestirLightManagerCurrentToPreviousBuffer)));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(65, cleanOptionalSrv(m_smokeRestirLightManagerPreviousToCurrentBuffer)));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(66, cleanOptionalSrv(m_smokeRestirLightManagerCurrentPayloadBuffer)));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(67, cleanOptionalSrv(m_smokeRestirLightManagerPreviousPayloadBuffer)));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(69, m_smokeCleanRtxdiDiCurrentReservoirBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(70, m_smokeCleanRtxdiDiTemporalReservoirBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(71, m_smokeCleanRtxdiDiPreviousReservoirBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(72, m_smokeCleanRtxdiDiSpatialReservoirBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(74, cleanNeeCacheProviderSrv));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(75, cleanNeeCacheCellSrv));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(77, cleanNeeCacheCandidateSrv));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(78, m_frameResources.rrMotionVectorTexture));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(94, m_liquidPoolStatusBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(PATH_TRACE_BLUE_NOISE_BINDING, m_smokeCleanRtxdiDiBlueNoise.texture));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::Sampler(0, m_backend->GetCommonPasses().m_AnisotropicWrapSampler));
        nvrhi::BindingSetHandle cleanBindingSet = device->createBindingSet(cleanBindingSetDesc, m_smokeCleanRtxdiDiSentinelBindingLayout);
        if (!cleanBindingSet)
        {
            if (cleanRtxdiDiDumpRequested)
            {
                printCleanRtxdiDiDump("dispatch-entry", "clean-binding-set", 0);
                r_pathTracingCleanRtxdiDiDump.SetInteger(0);
            }
            return;
        }

        nvrhi::rt::State cleanState;
        cleanState.shaderTable = m_smokeCleanRtxdiDiSentinelShaderTable;
        cleanState.bindings = { cleanBindingSet, m_smokeTextureDescriptorTable };

        PathTraceUploadBlueNoise(m_smokeCleanRtxdiDiBlueNoise, commandList);
        commandList->setTextureState(m_smokeCleanRtxdiDiBlueNoise.texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        commandList->setTextureState(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(m_liquidPoolStatusBuffer, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(dispatchStaticVertexBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(dispatchStaticIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(dispatchStaticTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(dispatchStaticTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeDynamicVertexBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeDynamicIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeDynamicTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeDynamicTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(dispatchStaticTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeDynamicTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeMaterialTableBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeMaterialFeatureBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeMaterialFeatureParameterBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeDynamicMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeEmissiveTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokePreviousEmissiveTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeReGIRState.placeholderSrvBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeRigidRouteVertexBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeRigidRouteIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeRigidRouteTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeRigidRouteTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeRigidRouteInstanceBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeSkinnedHitRouteRecordBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeSkinnedHitRouteTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_sceneInputs.geometry.skinnedSourceIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeSkinnedCurrentOutputVertexBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeSkinnedPreviousPositionBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeDoomAnalyticLightBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeDoomAnalyticCurrentIdentityBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeDoomAnalyticPreviousLightBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeDoomAnalyticPreviousIdentityBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeDoomAnalyticRemapBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeRestirLightManagerCurrentToPreviousBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeRestirLightManagerPreviousToCurrentBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeRestirLightManagerCurrentPayloadBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeRestirLightManagerPreviousPayloadBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeNeeCacheState.providerResultBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeNeeCacheState.cellBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeNeeCacheState.candidateBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeNeeCacheState.placeholderSrvBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_frameResources.primarySurfaceHistoryBuffers.current, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(m_frameResources.primarySurfaceHistoryBuffers.previous, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(m_frameResources.motionVectorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(m_frameResources.motionVectorMaskTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(m_frameResources.rrGuideAlbedoTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(m_frameResources.rrGuideSpecularAlbedoTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(m_frameResources.rrGuideNormalRoughnessTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(m_frameResources.rrGuideDepthTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(m_frameResources.rrGuideHitDistanceTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(m_frameResources.rrGuideResetMaskTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setTextureState(m_frameResources.rrInputColorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        SetPathTraceCleanRtxdiDiMaterialFeatureOutputsUnorderedAccess(
            commandList,
            cleanRtxdiDiMaterialFeaturePasses,
            m_frameResources);
        for (nvrhi::TextureHandle texture : m_smokeActiveTextureTable)
        {
            if (texture)
            {
                commandList->setTextureState(texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            }
        }
        commandList->setBufferState(m_smokeCleanRtxdiDiCurrentReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(m_smokeCleanRtxdiDiTemporalReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(m_smokeCleanRtxdiDiPreviousReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(m_smokeCleanRtxdiDiSpatialReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
        commandList->commitBarriers();
        ClearPathTraceCleanRtxdiDiMaterialFeatureOutputs(
            commandList,
            cleanRtxdiDiMaterialFeaturePasses,
            m_frameResources);
        const int cleanRtxdiDiLightMode = idMath::ClampInt(0, 3, r_pathTracingCleanRtxdiDiLightMode.GetInteger());
        const bool cleanView12FullAnalyticDomain =
            cleanRtxdiDiView == 12 &&
            r_pathTracingCleanRtxdiDiView12FullAnalyticDomain.GetInteger() != 0;
        const bool cleanPortalProofDomain = (!cleanRtxdiDiMaterialClassifierProofView && !cleanView12FullAnalyticDomain && cleanRtxdiDiView == 12) || cleanRtxdiDiView == 13 || cleanRtxdiDiView == 14 || cleanRtxdiDiView == 15 || cleanRtxdiDiResolveView == 16 ||
            (cleanRtxdiDiView == 8 && cleanRtxdiDiTemporalEnabled) ||
            (cleanRtxdiDiView == 10 && r_pathTracingCleanRtxdiDiView10PortalDomain.GetInteger() != 0);
        const uint32_t cleanAvailableAnalyticLightCount = cleanPortalProofDomain
            ? static_cast<uint32_t>(Min(Max(0, m_smokeDoomAnalyticPortalRegionLightCount), Max(0, m_smokeDoomAnalyticLightCount)))
            : static_cast<uint32_t>(Max(0, m_smokeDoomAnalyticLightCount));
        const uint32_t cleanAnalyticLightCount = cleanRtxdiDiLightMode == 1
            ? cleanAvailableAnalyticLightCount
            : 0u;
        const uint32_t cleanAnalyticIdentityCount = cleanRtxdiDiLightMode == 1
            ? Min(cleanAnalyticLightCount, static_cast<uint32_t>(Max(0, m_smokeDoomAnalyticCurrentIdentityCount)))
            : 0u;
        const uint32_t cleanPreviousAnalyticLightCount = cleanRtxdiDiLightMode == 1
            ? static_cast<uint32_t>(Max(0, m_smokeDoomAnalyticPreviousLightCount))
            : 0u;
        const uint32_t cleanPreviousAnalyticIdentityCount = cleanRtxdiDiLightMode == 1
            ? static_cast<uint32_t>(Max(0, m_smokeDoomAnalyticPreviousIdentityCount))
            : 0u;
        const uint32_t cleanAnalyticRemapCount = cleanRtxdiDiLightMode == 1
            ? static_cast<uint32_t>(Max(0, m_smokeDoomAnalyticRemapCount))
            : 0u;
        const PathTraceRemixLightManagerStats& cleanRluStats = m_remixLightManager.GetStats();
        const uint32_t cleanRluCurrentLightCount = cleanRtxdiDiLightMode == 1
            ? static_cast<uint32_t>(Max(0, m_smokeRestirLightManagerCurrentPayloadCount))
            : 0u;
        const uint32_t cleanRluPreviousLightCount = cleanRtxdiDiLightMode == 1
            ? static_cast<uint32_t>(Max(0, m_smokeRestirLightManagerPreviousPayloadCount))
            : 0u;
        const uint32_t cleanRluCurrentToPreviousCount = cleanRtxdiDiLightMode == 1
            ? cleanRluStats.currentToPreviousCount
            : 0u;
        const uint32_t cleanRluPreviousToCurrentCount = cleanRtxdiDiLightMode == 1
            ? cleanRluStats.previousToCurrentCount
            : 0u;
        const bool cleanRluDomainAllowed =
            cleanRluStats.domain == 0u ||
            cleanRluStats.domain == 2u;
        const bool cleanRluRoute =
            (r_pathTracingRemixLightUniverseUseForCleanRtxdiDi.GetInteger() != 0 ||
                pdfNeeRluCurrentProducerRequested ||
                cleanRestirGiNeeCacheProviderRequested) &&
            cleanRluStats.enabled != 0u &&
            cleanRluDomainAllowed &&
            cleanRluCurrentLightCount > 0u &&
            m_smokeRestirLightManagerCurrentToPreviousBuffer &&
            m_smokeRestirLightManagerPreviousToCurrentBuffer &&
            m_smokeRestirLightManagerCurrentPayloadBuffer &&
            m_smokeRestirLightManagerPreviousPayloadBuffer;
        const bool cleanNeeCacheProviderRequested =
            r_pathTracingCleanRtxdiDiNeeCacheProvider.GetInteger() != 0 ||
            cleanRestirGiNeeCacheProviderRequested;
        const bool cleanNeeCacheProviderReady =
            cleanNeeCacheProviderRequested &&
            cleanRluRoute &&
            neeCacheResourceReady &&
            neeCacheCandidateBuildRequested &&
            !cleanNeeCacheProviderStartupDelayActive &&
            !cleanNeeCacheProviderBuildDeferredByClear &&
            !m_smokeNeeCacheState.taskClearPending &&
            m_smokeNeeCacheState.cleanProviderSnapshotHoldActive &&
            m_smokeNeeCacheState.providerResultBuffer != nullptr &&
            m_smokeNeeCacheState.cellBuffer != nullptr &&
            m_smokeNeeCacheState.candidateBuffer != nullptr;
        const uint32_t cleanCandidateOverride = static_cast<uint32_t>(idMath::ClampInt(1, 128, r_pathTracingCleanRtxdiDiCandidateCount.GetInteger()));
        const uint32_t cleanRluDoomRangeOffset = Min(cleanRluStats.doomAnalyticRangeOffset, cleanRluCurrentLightCount);
        const uint32_t cleanRluDoomRangeCount = Min(cleanRluStats.doomAnalyticRangeCount, cleanRluCurrentLightCount - cleanRluDoomRangeOffset);
        const uint32_t cleanCandidateDomainCount = cleanRluRoute
            ? (cleanRluDoomRangeCount > 0u ? cleanRluDoomRangeCount : cleanRluCurrentLightCount)
            : cleanAnalyticLightCount;
        const uint32_t cleanCandidateCount = (cleanRtxdiDiView == 8 || cleanRtxdiDiView == 12 || cleanRtxdiDiResolveView == 16)
            ? Min(cleanCandidateDomainCount, cleanCandidateOverride)
            : 1u;
        const bool cleanSyntheticTemporalProofView =
            cleanRtxdiDiView == 9 ||
            cleanRtxdiDiView == 10 ||
            cleanRtxdiDiView == 11;
        const bool cleanStableRemapProofView = cleanSyntheticTemporalProofView;
        uint64 cleanHistorySignature = 1469598103934665603ull;
        cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(cleanRtxdiDiView));
        cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(cleanRtxdiDiLightMode));
        cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>((r_pathTracingCleanRtxdiDiExternalPdfNeeCurrent.GetInteger() != 0 || pdfNeeRluCurrentProducerRequested) ? 1 : 0));
        cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(cleanNeeCacheProviderReady ? 1 : 0));
        if (cleanNeeCacheProviderReady)
        {
            cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(neeCacheSettings.sourceDomain));
            cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(neeCacheSettings.cellResolution));
            cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(neeCacheSettings.candidateSlots));
            cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(idMath::Ftoi(neeCacheSettings.fallbackProbability * 1000.0f)));
        }
        cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(r_pathTracingAnalyticLightDoomRadiusCutoff.GetBool() ? 1 : 0));
        cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(cleanView12FullAnalyticDomain ? 1 : 0));
        cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(r_pathTracingCleanRtxdiDiBypassLightUniverse.GetInteger() != 0 ? 1 : 0));
        cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(idMath::ClampInt(0, 2, r_pathTracingCleanRtxdiDiDoomColorSource.GetInteger())));
        cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(r_pathTracingCleanRtxdiDiRequireProvenDoomLights.GetInteger() != 0 ? 1 : 0));
        cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(cleanRluRoute ? 1 : 0));
        if (cleanRluRoute && cleanRluStats.domain != 0u)
        {
            cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(cleanRluStats.domain));
        }
        if (!cleanStableRemapProofView)
        {
            cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(cleanCandidateCount));
            if (!cleanRluRoute)
            {
                cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(cleanAnalyticLightCount));
                cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(cleanAnalyticIdentityCount));
                if (!cleanPortalProofDomain)
                {
                    cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(cleanPreviousAnalyticLightCount));
                    cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(cleanPreviousAnalyticIdentityCount));
                    cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(cleanAnalyticRemapCount));
                }
            }
        }
        cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(cleanReservoirCount));
        cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(Max(0, m_frameResources.width)));
        cleanHistorySignature = HashSmokeDispatchValue(cleanHistorySignature, static_cast<uint64>(Max(0, m_frameResources.height)));
        if (m_smokeCleanRtxdiDiHistorySignature != cleanHistorySignature)
        {
            m_smokeCleanRtxdiDiHistorySignature = cleanHistorySignature;
            m_smokeCleanRtxdiDiPreviousReservoirValid = false;
            m_smokeCleanRtxdiDiPreviousReservoirResetReason = 4u;
            ++m_smokeCleanRtxdiDiHistoryResetCount;
        }
        uint32_t cleanTemporalFlags = 0u;
        if (cleanRtxdiDiTemporalEnabled)
        {
            cleanTemporalFlags |= 1u;
        }
        if (m_smokeCleanRtxdiDiPreviousReservoirValid)
        {
            cleanTemporalFlags |= 2u;
        }
        const bool cleanPromoteSubviewReservoir =
            !cleanRtxdiDiSubview ||
            r_pathTracingCleanRtxdiDiSubviewReservoirPromote.GetInteger() != 0;
        const bool cleanPromoteSubviewSurface =
            !cleanRtxdiDiSubview ||
            r_pathTracingCleanRtxdiDiSubviewSurfacePromote.GetInteger() != 0;
        const bool cleanSpatialRoute =
            !cleanRtxdiDiMaterialClassifierProofView &&
            ((cleanRtxdiDiView == 16 &&
                    cleanRtxdiDiTemporalEnabled &&
                    cleanRtxdiDiSpatialEnabled) ||
                cleanRtxdiDiRrGuideDebugView ||
                (cleanRtxdiDiSpatialEnabled &&
                    cleanRtxdiDiTemporalEnabled &&
                    (cleanRtxdiDiView == 12 ||
                        (cleanRtxdiDiView == 8 && idMath::ClampInt(-1, 16, r_pathTracingCleanRtxdiDiView8Band.GetInteger()) == 16)))) &&
            cleanPromoteSubviewReservoir &&
            m_smokeCleanRtxdiDiSpatialShaderTable != nullptr;
        const bool cleanProductionInitialOnly =
            cleanRtxdiDiProductionView &&
            !cleanRtxdiDiTemporalEnabled;
        const bool cleanProductionTemporalOnly =
            cleanRtxdiDiProductionView &&
            cleanRtxdiDiTemporalEnabled &&
            !cleanRtxdiDiSpatialEnabled;
        const bool cleanProductionSpatialOnly =
            cleanRtxdiDiProductionView &&
            cleanRtxdiDiTemporalEnabled &&
            cleanRtxdiDiSpatialEnabled &&
            r_pathTracingCleanRtxdiDiStopAfterSpatial.GetInteger() != 0;
        const bool cleanProductionStageIsolation =
            cleanProductionInitialOnly ||
            cleanProductionTemporalOnly;
        const int cleanRequestedLiquidPoolMode = idMath::ClampInt(0, 3, r_pathTracingLiquidPoolMode.GetInteger());
        const bool cleanLiquidPoolTelemetryReady = m_liquidPoolStatusBuffer && m_liquidPoolStatusReadbackBuffer;
        const bool cleanLiquidPoolParametersReady =
            m_smokeMaterialFeatureParameterBuffer &&
            m_sceneInputs.materials.materialFeatureParameterRecordCount > 0;
        const int cleanEffectiveLiquidPoolMode = cleanLiquidPoolTelemetryReady && cleanLiquidPoolParametersReady
            ? cleanRequestedLiquidPoolMode
            : 0;
        cleanTemporalFlags |= PackCleanRtxdiDiLiquidPoolControls(
            cleanEffectiveLiquidPoolMode,
            cleanLiquidPoolTelemetryReady,
            cleanLiquidPoolParametersReady);
        uint32_t cleanFlags = 0u;
        if (r_pathTracingCleanRtxdiDiExternalPdfNeeCurrent.GetInteger() != 0 || pdfNeeRluCurrentProducerRequested)
        {
            cleanFlags |= CLEAN_RTXDI_DI_FLAG_EXTERNAL_PDFNEE_CURRENT;
        }
        if (r_pathTracingCleanRtxdiDiRelaxBrdfGates.GetInteger() != 0)
        {
            cleanFlags |= 1u << 8u;
        }
        if (r_pathTracingCleanRtxdiDiDoomTargetFloor.GetInteger() != 0)
        {
            cleanFlags |= 1u << 9u;
        }
        if (r_pathTracingCleanRtxdiDiDummyEmissiveNormals.GetInteger() != 0)
        {
            cleanFlags |= CLEAN_RTXDI_DI_FLAG_DUMMY_EMISSIVE_NORMALS;
        }
        if (r_pathTracingCleanRtxdiDiForceEmissiveVisibility.GetInteger() != 0)
        {
            cleanFlags |= CLEAN_RTXDI_DI_FLAG_FORCE_EMISSIVE_VISIBILITY;
        }
        if (r_pathTracingCleanRtxdiDiTemporalRigidEmissives.GetInteger() == 0)
        {
            cleanFlags |= CLEAN_RTXDI_DI_FLAG_DISABLE_RIGID_EMISSIVE_TEMPORAL;
        }
        if (r_pathTracingCleanRtxdiDiInitialVisibility.GetInteger() != 0)
        {
            cleanFlags |= CLEAN_RTXDI_DI_FLAG_INITIAL_VISIBILITY;
        }
        if (r_pathTracingCleanRtxdiDiResolveSolidAnglePdf.GetInteger() != 0)
        {
            cleanFlags |= CLEAN_RTXDI_DI_FLAG_RESOLVE_SOLID_ANGLE_PDF;
        }
        if (cleanEffectiveLiquidPoolMode != 0)
        {
            cleanFlags |= CLEAN_RTXDI_DI_FLAG_LIQUID_MODIFIER_VISIBILITY;
        }
        if (cleanRluRoute)
        {
            cleanFlags |= CLEAN_RTXDI_DI_FLAG_REMIX_LIGHT_UNIVERSE;
        }
        if (cleanNeeCacheProviderReady)
        {
            cleanFlags |= CLEAN_RTXDI_DI_FLAG_NEE_CACHE_PROVIDER;
        }
        if (cleanRluRoute && r_pathTracingCleanRtxdiDiBestLights.GetInteger() != 0)
        {
            cleanFlags |= CLEAN_RTXDI_DI_FLAG_PREVIOUS_BEST_APPROXIMATION;
        }
        if (cleanSpatialRoute && cleanRtxdiDiSpatialEnabled)
        {
            cleanFlags |= CLEAN_RTXDI_DI_FLAG_SPATIAL_REUSE;
        }
        if (r_pathTracingCleanRtxdiDiGlassReflectionPsr.GetInteger() != 0)
        {
            cleanFlags |= CLEAN_RTXDI_DI_FLAG_GLASS_REFLECTION_PSR;
        }
        if (r_pathTracingReflectionOpaqueMirror.GetInteger() != 0)
        {
            cleanFlags |= CLEAN_RTXDI_DI_FLAG_OPAQUE_MIRROR_REFLECTION;
        }
        if (r_pathTracingReflectionSecondaryShadows.GetInteger() == 0)
        {
            cleanFlags |= CLEAN_RTXDI_DI_FLAG_REFLECTION_SECONDARY_NO_SHADOWS;
        }
        const bool glassReflectionProducerActive =
            !cleanProductionStageIsolation &&
            r_pathTracingCleanRtxdiDiTransmissionProducer.GetInteger() != 0 &&
            r_pathTracingCleanRtxdiDiTransmissionCompose.GetInteger() != 0;
        if (r_pathTracingCleanRtxdiDiGlassDistortion.GetInteger() != 0)
        {
            cleanFlags |= CLEAN_RTXDI_DI_FLAG_GLASS_DISTORTION;
        }
        if (r_pathTracingCleanRtxdiDiGlassRefractedPsr.GetInteger() != 0)
        {
            cleanFlags |= CLEAN_RTXDI_DI_FLAG_GLASS_REFRACTED_PSR;
        }
        PathTraceCleanRtxdiDiSentinelConstants cleanConstants = {};
        cleanConstants.view = static_cast<uint32_t>(cleanRtxdiDiResolveView);
        cleanConstants.status = cleanRtxdiDiView == 1 ? 1u : 2u;
        cleanConstants.width = static_cast<uint32_t>(Max(0, m_frameResources.width));
        cleanConstants.height = static_cast<uint32_t>(Max(0, m_frameResources.height));
        cleanConstants.analyticLightCount = cleanAnalyticLightCount;
        cleanConstants.analyticIdentityCount = cleanAnalyticIdentityCount;
        cleanConstants.lightMode = static_cast<uint32_t>(cleanRtxdiDiLightMode);
        cleanConstants.frameIndex = cleanRtxdiDiFrameIndexForDispatch;
        if (r_pathTracingCleanRtxdiDiFrameFreeze.GetInteger() == 0)
        {
            ++m_smokeCleanRtxdiDiFrameIndex;
        }
        cleanConstants.reservoirCount = cleanReservoirCount;
        cleanConstants.candidateCount = cleanCandidateCount;
        cleanConstants.flags = cleanFlags;
        if (m_smokeCleanRtxdiDiBlueNoise.valid && r_pathTracingCleanRtxdiDiBlueNoise.GetInteger() != 0)
        {
            cleanConstants.flags |= CLEAN_RTXDI_DI_FLAG_BLUE_NOISE;
        }
        cleanConstants.previousAnalyticLightCount = cleanPreviousAnalyticLightCount;
        cleanConstants.previousAnalyticIdentityCount = cleanPreviousAnalyticIdentityCount;
        cleanConstants.analyticRemapCount = cleanAnalyticRemapCount;
        cleanConstants.temporalFlags = cleanTemporalFlags;
        cleanConstants.historyResetCount = m_smokeCleanRtxdiDiHistoryResetCount;
        cleanConstants.view8Band = static_cast<uint32_t>(cleanRtxdiDiRrInputMosaicView
            ? cleanRtxdiDiView18Tile
            : idMath::ClampInt(-1, 16, r_pathTracingCleanRtxdiDiView8Band.GetInteger()));
        cleanConstants.resolveVisibilityReuse = static_cast<uint32_t>(idMath::ClampInt(0, 3, r_pathTracingCleanRtxdiDiResolveVisibilityReuse.GetInteger()));
        cleanConstants.resolveBrdfTarget = PackCleanRtxdiDiResolveBrdfTarget();
        cleanConstants.referenceRab = static_cast<uint32_t>(idMath::ClampInt(0, 10, r_pathTracingCleanRtxdiDiReferenceRab.GetInteger()));
        cleanConstants.rluCurrentLightCount = cleanRluRoute ? cleanRluCurrentLightCount : 0u;
        cleanConstants.rluPreviousLightCount = cleanRluRoute ? cleanRluPreviousLightCount : 0u;
        cleanConstants.rluCurrentToPreviousCount = cleanRluRoute ? cleanRluCurrentToPreviousCount : 0u;
        cleanConstants.rluPreviousToCurrentCount = cleanRluRoute ? cleanRluPreviousToCurrentCount : 0u;
        cleanConstants.temporalAudit = cleanRtxdiDiProductionView
            ? 0u
            : static_cast<uint32_t>(idMath::ClampInt(0, 1, r_pathTracingCleanRtxdiDiTemporalAudit.GetInteger()));
        cleanConstants.staticTriangleCount = static_cast<uint32_t>(Max(0, m_sceneInputs.geometry.staticTriangleCount));
        cleanConstants.dynamicTriangleCount = static_cast<uint32_t>(Max(0, m_sceneInputs.geometry.dynamicTriangleCount));
        cleanConstants.rigidRouteTriangleCount = static_cast<uint32_t>(Max(0, m_sceneInputs.geometry.rigidRouteTriangleCount));
        const bool cleanDisableEmissiveTriangleSampling =
            PathTraceSafetyDisabled(BuildPathTraceSafetyDisableMask(), RT_PT_SAFETY_DISABLE_EMISSIVE_TRIANGLE_SAMPLING);
        cleanConstants.currentEmissiveTriangleCount = cleanDisableEmissiveTriangleSampling ? 0u : static_cast<uint32_t>(Max(0, m_smokeEmissiveTriangleCount));
        cleanConstants.previousEmissiveTriangleCount = cleanDisableEmissiveTriangleSampling ? 0u : static_cast<uint32_t>(Max(0, m_smokePreviousEmissiveTriangleCount));
        cleanConstants.rluDoomAnalyticRangeOffset = cleanRluRoute ? cleanRluStats.doomAnalyticRangeOffset : 0u;
        cleanConstants.rluDoomAnalyticRangeCount = cleanRluRoute ? cleanRluStats.doomAnalyticRangeCount : 0u;
        cleanConstants.doomAnalyticFullCurrentCount = static_cast<uint32_t>(Max(0, m_smokeDoomAnalyticLightCount));
        cleanConstants.doomAnalyticFullPreviousCount = static_cast<uint32_t>(Max(0, m_smokeDoomAnalyticPreviousLightCount));
        cleanConstants.rluDomain = cleanRluRoute ? cleanRluStats.domain : 0u;
        cleanConstants.temporalFireflyClamp = static_cast<uint32_t>(idMath::ClampInt(0, 1024, r_pathTracingCleanRtxdiDiTemporalFireflyClamp.GetInteger()));
        const uint32_t cleanRluShaderEmissiveSampleCount = (cleanRluRoute && !cleanDisableEmissiveTriangleSampling) ? cleanRluStats.emissiveSampleCount : 0u;
        const uint32_t cleanRluShaderDoomSampleCount = cleanRluRoute ? cleanRluStats.doomAnalyticSampleCount : 0u;
        const uint32_t cleanRluShaderTotalSampleCount = cleanRluShaderEmissiveSampleCount + cleanRluShaderDoomSampleCount;
        const uint32_t cleanRluShaderNonEmptyRangeCount =
            (cleanRluShaderEmissiveSampleCount > 0u ? 1u : 0u) +
            (cleanRluShaderDoomSampleCount > 0u ? 1u : 0u);
        cleanConstants.rluRangeInfo[0] = static_cast<float>(cleanRluRoute ? cleanRluStats.emissiveRangeOffset : 0u);
        cleanConstants.rluRangeInfo[1] = static_cast<float>((cleanRluRoute && !cleanDisableEmissiveTriangleSampling) ? cleanRluStats.emissiveRangeCount : 0u);
        cleanConstants.rluRangeInfo[2] = static_cast<float>(cleanRluRoute ? cleanRluStats.doomAnalyticRangeOffset : 0u);
        cleanConstants.rluRangeInfo[3] = static_cast<float>(cleanRluRoute ? cleanRluStats.doomAnalyticRangeCount : 0u);
        cleanConstants.rluSampleInfo[0] = static_cast<float>(cleanRluShaderEmissiveSampleCount);
        cleanConstants.rluSampleInfo[1] = static_cast<float>(cleanRluShaderDoomSampleCount);
        cleanConstants.rluSampleInfo[2] = static_cast<float>(cleanRluShaderTotalSampleCount);
        cleanConstants.rluSampleInfo[3] = static_cast<float>(cleanRluShaderNonEmptyRangeCount);
        const int cleanEmissiveDistributionCount =
            !cleanDisableEmissiveTriangleSampling &&
            r_pathTracingEmissiveDistribution.GetInteger() != 0 &&
            m_sceneInputs.lights.emissiveDistributionValid
                ? m_sceneInputs.lights.emissiveDistributionCount
                : 0;
        cleanConstants.emissiveDistributionInfo[0] = static_cast<float>(Max(0, cleanEmissiveDistributionCount));
        cleanConstants.emissiveDistributionInfo[1] = cleanEmissiveDistributionCount > 0 ? 1.0f : 0.0f;
        cleanConstants.emissiveDistributionInfo[2] = static_cast<float>(Max(0, m_sceneInputs.lights.emissiveDistributionFallbackIndex));
        const int cleanMaterialOverlayRecordCount =
            m_sceneInputs.materials.materialTableGpuStable ? Max(0, m_sceneInputs.materials.dynamicMaterialRecordCount) : 0;
        cleanConstants.emissiveDistributionInfo[3] = static_cast<float>(cleanMaterialOverlayRecordCount);
        const int cleanTextureSampleMethod = r_pathTracingTextureSampleEnable.GetInteger() != 0
            ? idMath::ClampInt(0, 2, r_pathTracingTextureSampleMethod.GetInteger())
            : 0;
        const uint32_t cleanTextureFlags =
            (r_pathTracingTextureBindlessEnable.GetInteger() != 0 ? 1u : 0u) |
            (r_pathTracingTextureFilter.GetInteger() != 0 ? 2u : 0u) |
            (r_pathTracingTextureDecode.GetInteger() != 0 ? 4u : 0u) |
            (r_pathTracingUseNormalMaps.GetInteger() != 0 ? 8u : 0u) |
            (r_pathTracingUseSpecularMaps.GetInteger() != 0 ? 16u : 0u) |
            (r_pathTracingUseEmissiveMaps.GetInteger() != 0 ? 32u : 0u) |
            64u |
            (r_pathTracingToyFakePBRSpecular.GetInteger() != 0 ? 128u : 0u) |
            (r_pathTracingNormalMapFlipGreen.GetInteger() != 0 ? 256u : 0u) |
            (r_pathTracingSkyCubeEnvironment.GetInteger() != 0 && m_smokeSkyEnvironmentCube
                ? RT_SMOKE_TEXTURE_FLAG_SKY_CUBE
                : 0u);
        cleanConstants.textureInfo[0] = static_cast<float>(Max(0, static_cast<int>(m_smokeActiveTextureTable.size()) - 1));
        cleanConstants.textureInfo[1] = static_cast<float>(cleanTextureSampleMethod);
        cleanConstants.textureInfo[2] = static_cast<float>(Max(0, m_smokeMaterialTableEntryCount));
        cleanConstants.textureInfo[3] = static_cast<float>(cleanTextureFlags);
        PathTraceCleanRtxdiDiGuiSnapshot cleanGuiSnapshot;
        cleanGuiSnapshot.valid = true;
        cleanGuiSnapshot.enabled = cleanRtxdiDiEnabled;
        cleanGuiSnapshot.routeReady = true;
        cleanGuiSnapshot.temporal = cleanRtxdiDiTemporalEnabled;
        cleanGuiSnapshot.spatial = cleanRtxdiDiSpatialEnabled;
        cleanGuiSnapshot.bestLights = r_pathTracingCleanRtxdiDiBestLights.GetInteger() != 0;
        cleanGuiSnapshot.externalPdfNeeCurrent = r_pathTracingCleanRtxdiDiExternalPdfNeeCurrent.GetInteger() != 0 || pdfNeeRluCurrentProducerRequested;
        cleanGuiSnapshot.regirEnabled = regirSettings.enabled;
        cleanGuiSnapshot.subview = viewDef && viewDef->isSubview;
        cleanGuiSnapshot.mirror = viewDef && viewDef->isMirror;
        cleanGuiSnapshot.superView = viewDef && viewDef->superView;
        cleanGuiSnapshot.sceneBuilt = m_smokeSceneBuilt;
        cleanGuiSnapshot.cleanShader = m_smokeCleanRtxdiDiSentinelShaderTable != nullptr;
        cleanGuiSnapshot.bindingSet = cleanBindingSet != nullptr;
        cleanGuiSnapshot.textureTable = m_smokeTextureDescriptorTable != nullptr;
        cleanGuiSnapshot.outputTexture = m_frameResources.outputTexture != nullptr;
        cleanGuiSnapshot.currentReservoir = m_smokeCleanRtxdiDiCurrentReservoirBuffer != nullptr;
        cleanGuiSnapshot.temporalReservoir = m_smokeCleanRtxdiDiTemporalReservoirBuffer != nullptr;
        cleanGuiSnapshot.previousReservoir = m_smokeCleanRtxdiDiPreviousReservoirBuffer != nullptr;
        cleanGuiSnapshot.previousReservoirValid = m_smokeCleanRtxdiDiPreviousReservoirValid;
        cleanGuiSnapshot.portalProofDomain = cleanPortalProofDomain;
        cleanGuiSnapshot.fullAnalyticDomain = cleanView12FullAnalyticDomain;
        cleanGuiSnapshot.doomRadiusCutoff = r_pathTracingAnalyticLightDoomRadiusCutoff.GetBool();
        cleanGuiSnapshot.relaxBrdfGates = r_pathTracingCleanRtxdiDiRelaxBrdfGates.GetInteger() != 0;
        cleanGuiSnapshot.doomTargetFloor = r_pathTracingCleanRtxdiDiDoomTargetFloor.GetInteger() != 0;
        cleanGuiSnapshot.bypassLightUniverse = r_pathTracingCleanRtxdiDiBypassLightUniverse.GetInteger() != 0;
        cleanGuiSnapshot.requireProvenDoomLights = r_pathTracingCleanRtxdiDiRequireProvenDoomLights.GetInteger() != 0;
        cleanGuiSnapshot.view = cleanRtxdiDiView;
        cleanGuiSnapshot.lightMode = cleanRtxdiDiLightMode;
        cleanGuiSnapshot.area = viewDef ? viewDef->areaNum : -1;
        cleanGuiSnapshot.drawSurfs = viewDef ? viewDef->numDrawSurfs : 0;
        cleanGuiSnapshot.width = m_frameResources.width;
        cleanGuiSnapshot.height = m_frameResources.height;
        cleanGuiSnapshot.regirMode = regirSettings.mode;
        cleanGuiSnapshot.regirCenterMode = regirSettings.centerMode;
        cleanGuiSnapshot.regirLightsPerCell = regirSettings.lightsPerCell;
        cleanGuiSnapshot.regirBuildSamples = regirSettings.buildSamples;
        cleanGuiSnapshot.analyticDomainFreezeMs = r_pathTracingCleanRtxdiDiAnalyticDomainFreezeMs.GetInteger();
        cleanGuiSnapshot.doomColorSource = idMath::ClampInt(0, 2, r_pathTracingCleanRtxdiDiDoomColorSource.GetInteger());
        cleanGuiSnapshot.temporalBiasCorrection = CleanRtxdiDiTemporalBiasCorrectionValue();
        cleanGuiSnapshot.temporalMaxHistory = idMath::ClampInt(0, 64, r_pathTracingCleanRtxdiDiTemporalMaxHistory.GetInteger());
        cleanGuiSnapshot.candidateOverride = static_cast<int>(cleanCandidateOverride);
        cleanGuiSnapshot.view10LightCount = idMath::ClampInt(1, 8, r_pathTracingCleanRtxdiDiView10LightCount.GetInteger());
        cleanGuiSnapshot.view10LightStart = idMath::ClampInt(0, 64, r_pathTracingCleanRtxdiDiView10LightStart.GetInteger());
        cleanGuiSnapshot.view10PortalDomain = r_pathTracingCleanRtxdiDiView10PortalDomain.GetInteger() != 0;
        cleanGuiSnapshot.frameIndex = static_cast<int>(cleanConstants.frameIndex);
        cleanGuiSnapshot.cleanCandidates = cleanCandidateCount;
        cleanGuiSnapshot.cleanCurrentAnalytic = static_cast<unsigned int>(Max(0, m_smokeDoomAnalyticLightCount));
        cleanGuiSnapshot.cleanPortalAnalytic = static_cast<unsigned int>(Max(0, m_smokeDoomAnalyticPortalRegionLightCount));
        cleanGuiSnapshot.cleanCurrentAnalyticIdentity = cleanAnalyticIdentityCount;
        cleanGuiSnapshot.cleanPreviousAnalytic = cleanPreviousAnalyticLightCount;
        cleanGuiSnapshot.cleanPreviousAnalyticIdentity = cleanPreviousAnalyticIdentityCount;
        cleanGuiSnapshot.cleanAnalyticRemap = cleanAnalyticRemapCount;
        cleanGuiSnapshot.cleanReservoirCount = cleanReservoirCount;
        cleanGuiSnapshot.cleanHistoryResetCount = m_smokeCleanRtxdiDiHistoryResetCount;
        cleanGuiSnapshot.cleanHistorySignature = static_cast<unsigned long long>(m_smokeCleanRtxdiDiHistorySignature);
        cleanGuiSnapshot.temporalFlags = cleanTemporalFlags;
        cleanGuiSnapshot.temporalAuditEnabled = cleanConstants.temporalAudit != 0u;
        cleanGuiSnapshot.regirCellSize = regirSettings.cellSize;
        cleanGuiSnapshot.regirGridX = regirSettings.gridX;
        cleanGuiSnapshot.regirGridY = regirSettings.gridY;
        cleanGuiSnapshot.regirGridZ = regirSettings.gridZ;
        cleanGuiSnapshot.regirCandidateSlots = regirDesc.slotCount;
        cleanGuiSnapshot.route = cleanRtxdiDiRouteLabel(cleanRtxdiDiView);
        cleanGuiSnapshot.behavior = cleanRtxdiDiBehaviorLabel(cleanRtxdiDiView);
        cleanGuiSnapshot.regirFirstMissing = regirDesc.firstMissingContract ? regirDesc.firstMissingContract : "unknown";
        PathTraceCleanRtxdiDiPublishGuiSnapshot(cleanGuiSnapshot);
        cleanConstants.prevCameraOriginAndValid[0] = m_frameResources.primarySurfaceHistoryView.origin.x;
        cleanConstants.prevCameraOriginAndValid[1] = m_frameResources.primarySurfaceHistoryView.origin.y;
        cleanConstants.prevCameraOriginAndValid[2] = m_frameResources.primarySurfaceHistoryView.origin.z;
        cleanConstants.prevCameraOriginAndValid[3] = m_frameResources.primarySurfaceHistoryView.valid ? 1.0f : 0.0f;
        cleanConstants.prevCameraForwardAndTanX[0] = m_frameResources.primarySurfaceHistoryView.forward.x;
        cleanConstants.prevCameraForwardAndTanX[1] = m_frameResources.primarySurfaceHistoryView.forward.y;
        cleanConstants.prevCameraForwardAndTanX[2] = m_frameResources.primarySurfaceHistoryView.forward.z;
        cleanConstants.prevCameraForwardAndTanX[3] = m_frameResources.primarySurfaceHistoryView.tanX;
        cleanConstants.prevCameraLeftAndTanY[0] = m_frameResources.primarySurfaceHistoryView.left.x;
        cleanConstants.prevCameraLeftAndTanY[1] = m_frameResources.primarySurfaceHistoryView.left.y;
        cleanConstants.prevCameraLeftAndTanY[2] = m_frameResources.primarySurfaceHistoryView.left.z;
        cleanConstants.prevCameraLeftAndTanY[3] = m_frameResources.primarySurfaceHistoryView.tanY;
        cleanConstants.prevCameraUpAndTanY[0] = m_frameResources.primarySurfaceHistoryView.up.x;
        cleanConstants.prevCameraUpAndTanY[1] = m_frameResources.primarySurfaceHistoryView.up.y;
        cleanConstants.prevCameraUpAndTanY[2] = m_frameResources.primarySurfaceHistoryView.up.z;
        cleanConstants.prevCameraUpAndTanY[3] = m_frameResources.primarySurfaceHistoryView.tanY;
        cleanConstants.cameraOriginAndValid[0] = viewDef->renderView.vieworg.x;
        cleanConstants.cameraOriginAndValid[1] = viewDef->renderView.vieworg.y;
        cleanConstants.cameraOriginAndValid[2] = viewDef->renderView.vieworg.z;
        cleanConstants.cameraOriginAndValid[3] = 1.0f;
        cleanConstants.cameraForwardAndTanX[0] = viewDef->renderView.viewaxis[0].x;
        cleanConstants.cameraForwardAndTanX[1] = viewDef->renderView.viewaxis[0].y;
        cleanConstants.cameraForwardAndTanX[2] = viewDef->renderView.viewaxis[0].z;
        cleanConstants.cameraForwardAndTanX[3] = idMath::Tan(DEG2RAD(viewDef->renderView.fov_x * 0.5f));
        cleanConstants.cameraLeftAndTanY[0] = viewDef->renderView.viewaxis[1].x;
        cleanConstants.cameraLeftAndTanY[1] = viewDef->renderView.viewaxis[1].y;
        cleanConstants.cameraLeftAndTanY[2] = viewDef->renderView.viewaxis[1].z;
        cleanConstants.cameraLeftAndTanY[3] = idMath::Tan(DEG2RAD(viewDef->renderView.fov_y * 0.5f));
        cleanConstants.cameraUpAndTanY[0] = viewDef->renderView.viewaxis[2].x;
        cleanConstants.cameraUpAndTanY[1] = viewDef->renderView.viewaxis[2].y;
        cleanConstants.cameraUpAndTanY[2] = viewDef->renderView.viewaxis[2].z;
        cleanConstants.cameraUpAndTanY[3] = idMath::Tan(DEG2RAD(viewDef->renderView.fov_y * 0.5f));
        cleanConstants.doomAnalyticLightInfo[0] = static_cast<float>(cleanAnalyticLightCount);
        cleanConstants.doomAnalyticLightInfo[1] = static_cast<float>(idMath::ClampInt(0, 1024, r_pathTracingAnalyticLightMaxGpu.GetInteger()));
        cleanConstants.doomAnalyticLightInfo[2] = idMath::ClampFloat(0.0f, 16.0f, r_pathTracingAnalyticLightIntensityScale.GetFloat());
        const uint32_t cleanAnalyticLightFlags =
            (r_pathTracingAnalyticLightCandidates.GetBool() ? 1u : 0u) |
            (r_pathTracingAnalyticLightReplaceSelected.GetBool() ? 2u : 0u) |
            (r_pathTracingAnalyticLightDoomRadiusCutoff.GetBool() ? 4u : 0u);
        cleanConstants.doomAnalyticLightInfo[3] = static_cast<float>(cleanAnalyticLightFlags);
        cleanConstants.motionVectorInfo[0] = cleanRtxdiDiView >= 5 || r_pathTracingMotionVectorExport.GetInteger() != 0 ? 1.0f : 0.0f;
        // y = bounded analytic reflection RIS candidate count M (1..16).
        cleanConstants.motionVectorInfo[1] = static_cast<float>(
            idMath::ClampInt(1, 16, r_pathTracingReflectionSecondarySamples.GetInteger()));
        cleanConstants.motionVectorInfo[2] = static_cast<float>(idMath::ClampInt(1, 128, r_pathTracingRestirPTAnalyticLightTrials.GetInteger()));
        cleanConstants.motionVectorInfo[3] = idMath::ClampFloat(0.0f, 1.0f, r_pathTracingRestirPTTemporalAnalyticLightChangeTolerance.GetFloat());
        cleanConstants.restirPTSurfaceInfo[0] = static_cast<float>(idMath::ClampInt(0, 64, r_pathTracingCleanRtxdiDiView10LightStart.GetInteger()));
        cleanConstants.restirPTSurfaceInfo[1] = static_cast<float>(idMath::ClampInt(0, 64, r_pathTracingCleanRtxdiDiTemporalMaxHistory.GetInteger()));
        cleanConstants.restirPTSurfaceInfo[2] = static_cast<float>(idMath::ClampInt(1, 8, r_pathTracingCleanRtxdiDiView10LightCount.GetInteger()));
        cleanConstants.restirPTSurfaceInfo[3] = static_cast<float>(CleanRtxdiDiTemporalBiasCorrectionValue());
        cleanConstants.neeCacheInfo0[0] = cleanNeeCacheProviderReady ? 1.0f : 0.0f;
        cleanConstants.neeCacheInfo0[1] = neeCacheSettings.fallbackProbability;
        cleanConstants.neeCacheInfo0[2] = static_cast<float>(neeCacheSettings.sourceDomain);
        cleanConstants.neeCacheInfo0[3] = static_cast<float>(neeCacheSettings.candidateSlots);
        cleanConstants.neeCacheInfo1[0] = static_cast<float>(neeCacheSettings.cellResolution);
        cleanConstants.neeCacheInfo1[1] = neeCacheSettings.minRange;
        cleanConstants.neeCacheInfo1[2] = static_cast<float>(neeCacheDesc.cellCount);
        cleanConstants.neeCacheInfo1[3] = static_cast<float>(neeCacheDesc.providerResultCount);
        cleanConstants.toyPathInfo[0] = idMath::ClampFloat(0.0f, 1.0f, r_pathTracingCleanRtxdiDiGlassRefractedPsrStrength.GetFloat());
        // y = RR cameraNear for glass PSR depth encode (matches primary RR contract).
        {
            const float rrNearCvar = r_pathTracingDLSSRRCameraNear.GetFloat();
            cleanConstants.toyPathInfo[1] = Max(rrNearCvar > 0.0f ? rrNearCvar : r_znear.GetFloat(), 1.0e-4f);
        }
        cleanConstants.toyPathInfo[2] = idMath::ClampFloat(0.0f, 32.0f, r_pathTracingToyEmissiveScale.GetFloat());
        cleanConstants.toyPathInfo[3] = static_cast<float>(Max(0, m_sceneInputs.geometry.rigidRouteInstanceCount));
        cleanConstants.geometryInfo0[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.staticVertexCount));
        cleanConstants.geometryInfo0[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.staticIndexCount));
        cleanConstants.geometryInfo0[2] = static_cast<float>(Max(0, m_sceneInputs.geometry.staticTriangleCount));
        cleanConstants.geometryInfo0[3] = static_cast<float>(Max(0, m_sceneInputs.geometry.dynamicVertexCount));
        cleanConstants.geometryInfo1[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.dynamicIndexCount));
        cleanConstants.geometryInfo1[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.dynamicTriangleCount));
        cleanConstants.geometryInfo1[2] = static_cast<float>(Max(0, m_sceneInputs.geometry.rigidRouteVertexCount));
        cleanConstants.geometryInfo1[3] = static_cast<float>(Max(0, m_sceneInputs.geometry.rigidRouteIndexCount));
        cleanConstants.staticBucketRouteInfo[0] =
            m_sceneInputs.geometry.staticBucketRouteFirstInstanceId;
        cleanConstants.staticBucketRouteInfo[1] =
            m_sceneInputs.geometry.staticBucketRoutePublicationValid
                ? m_sceneInputs.geometry.
                    staticBucketTriangleCount
                : 0u;
        cleanConstants.staticBucketRouteInfo[2] =
            static_cast<uint32_t>(
                m_sceneInputs.geometry.
                    staticBucketRouteGeneration);
        cleanConstants.staticBucketRouteInfo[3] =
            static_cast<uint32_t>(
                m_sceneInputs.geometry.
                    staticBucketRouteGeneration >> 32);
        cleanConstants.spatialInfo[0] = static_cast<float>(idMath::ClampInt(1, 16, r_cleanDiSpatialSamples.GetInteger()));
        cleanConstants.spatialInfo[1] = static_cast<float>(idMath::ClampInt(1, 16, r_cleanDiSpatialDisocclusionSamples.GetInteger()));
        cleanConstants.spatialInfo[2] = idMath::ClampFloat(1.0f, 128.0f, r_cleanDiSpatialRadius.GetFloat());
        cleanConstants.spatialInfo[3] =
            cleanSpatialRoute &&
                cleanRtxdiDiSpatialEnabled &&
                staticBucketSecondaryIsolation.spatialNeighborReuse
            ? 1.0f
            : 0.0f;
        commandList->setRayTracingState(cleanState);

        if (cleanRtxdiDiDumpRequested)
        {
            printCleanRtxdiDiDump("route-ready", "none", cleanSpatialRoute ? 2 : 1);
            r_pathTracingCleanRtxdiDiDump.SetInteger(0);
        }

        nvrhi::rt::DispatchRaysArguments cleanArgs;
        cleanArgs.width = m_frameResources.width;
        cleanArgs.height = m_frameResources.height;
        cleanArgs.depth = 1;
        const bool cleanInitialRaygenView =
            cleanRtxdiDiResolveView == 4 ||
            cleanRtxdiDiResolveView == 7 ||
            (cleanRtxdiDiResolveView == 8 && !cleanRtxdiDiTemporalEnabled) ||
            (cleanRtxdiDiResolveView == 16 &&
                !cleanRtxdiDiTemporalEnabled);
        const bool cleanTemporalRaygenView =
            cleanRtxdiDiResolveView == 5 ||
            cleanRtxdiDiResolveView == 6 ||
            cleanRtxdiDiResolveView == 8 ||
            cleanRtxdiDiResolveView == 9 ||
            cleanRtxdiDiResolveView == 10 ||
            cleanRtxdiDiResolveView == 11 ||
            (cleanRtxdiDiResolveView == 16 &&
                cleanRtxdiDiTemporalEnabled);
        const bool cleanSplitRaygenView = cleanInitialRaygenView || cleanTemporalRaygenView;
        PathTraceCleanRtxdiDiSentinelConstants dispatchConstants = cleanConstants;
        if (cleanSpatialRoute)
        {
            dispatchConstants.flags |= CLEAN_RTXDI_DI_FLAG_SPATIAL_TEMPORAL_PREPASS;
        }
        commandList->writeBuffer(m_smokeCleanRtxdiDiSentinelConstantsBuffer, &dispatchConstants, sizeof(dispatchConstants));
        if (glassReflectionProducerActive &&
            staticBucketSecondaryIsolation.transmissionPsr)
        {
            // Primary surface replacement for thin glass: trace through glass
            // pixels and swap their primary-surface records for the behind-glass
            // hit before any DI/GI pass consumes them.
            PathTraceCleanRtxdiDiSentinelConstants psrConstants = dispatchConstants;
            psrConstants.flags |= CLEAN_RTXDI_DI_FLAG_TRANSMISSION_PSR_PHASE;
            if (staticBucketBoundedTransmissionResolverRequired)
            {
                psrConstants.flags |=
                    CLEAN_RTXDI_DI_FLAG_TRANSMISSION_ITERATIVE_RESOLVE;
            }
            const char* staticBucketTransmissionMarker =
                "CleanDI.TransmissionPSR";
            if (staticBucketSecondaryIsolationActive)
            {
                // Keep the GEO-10 traversal probes exact regardless of the
                // normal glass defaults. Only the straight-through
                // transmission lane is admitted. Stages 10-16 isolate the
                // legacy single-TraceRay path; stages 17-24 select the bounded
                // forced-opaque iterative resolver. Stage 18 uses monolithic
                // traversal; the others use buckets. Stages 19 and 20 stop
                // after initial and temporal; stage 21 skips temporal and
                // presents initial through spatial with neighbor reuse off;
                // stage 22 writes a producer-local tuple diagnostic; stage 23
                // splits closest-hit world position from raygen reconstruction;
                // stage 24 admits material-feature composition.
                psrConstants.flags &= ~(
                    CLEAN_RTXDI_DI_FLAG_GLASS_REFLECTION_PSR |
                    CLEAN_RTXDI_DI_FLAG_OPAQUE_MIRROR_REFLECTION |
                    CLEAN_RTXDI_DI_FLAG_GLASS_REFRACTED_PSR |
                    CLEAN_RTXDI_DI_FLAG_TRANSMISSION_TRACE_PROBE_MASK |
                    CLEAN_RTXDI_DI_FLAG_TRANSMISSION_ITERATIVE_RESOLVE);
                psrConstants.flags |=
                    (static_cast<uint32_t>(
                        staticBucketSecondaryIsolation.
                            transmissionTraceProbeMode) <<
                        CLEAN_RTXDI_DI_FLAG_TRANSMISSION_TRACE_PROBE_SHIFT) &
                    CLEAN_RTXDI_DI_FLAG_TRANSMISSION_TRACE_PROBE_MASK;
                if (staticBucketSecondaryIsolation.
                        transmissionIterativeResolve)
                {
                    psrConstants.flags |=
                        CLEAN_RTXDI_DI_FLAG_TRANSMISSION_ITERATIVE_RESOLVE;
                }
                switch (staticBucketSecondaryIsolation.stage)
                {
                    case 10:
                        staticBucketTransmissionMarker =
                            "GEO10.View16.Stage10 TransmissionRaygenNoTrace";
                        break;
                    case 11:
                        staticBucketTransmissionMarker =
                            "GEO10.View16.Stage11 TransmissionTraversalNoHitShaders";
                        break;
                    case 12:
                        staticBucketTransmissionMarker =
                            "GEO10.View16.Stage12 TransmissionAnyHitOnly";
                        break;
                    case 13:
                        staticBucketTransmissionMarker =
                            "GEO10.View16.Stage13 TransmissionAnyHitEntryOnly";
                        break;
                    case 14:
                        staticBucketTransmissionMarker =
                            "GEO10.View16.Stage14 TransmissionAnyHitContentOnce";
                        break;
                    case 15:
                        staticBucketTransmissionMarker =
                            "GEO10.View16.Stage15 TransmissionAnyHitBounded";
                        break;
                    case 16:
                        staticBucketTransmissionMarker =
                            "GEO10.View16.Stage16 TransmissionAnyHitIgnoreOnce";
                        break;
                    case 18:
                        staticBucketTransmissionMarker =
                            "GEO10.View16.Stage18 TransmissionIterativeResolveMonolithic";
                        break;
                    case 19:
                        staticBucketTransmissionMarker =
                            "GEO10.View16.Stage19 TransmissionIterativeResolveInitialOnly";
                        break;
                    case 20:
                        staticBucketTransmissionMarker =
                            "GEO10.View16.Stage20 TransmissionIterativeResolveTemporal";
                        break;
                    case 21:
                        staticBucketTransmissionMarker =
                            "GEO10.View16.Stage21 TransmissionIterativeResolveInitialPresented";
                        break;
                    case 22:
                        staticBucketTransmissionMarker =
                            "GEO10.View16.Stage22 TransmissionBucketTupleDiagnostic";
                        break;
                    case 23:
                        staticBucketTransmissionMarker =
                            "GEO10.View16.Stage23 TransmissionClosestHitPositionDiagnostic";
                        break;
                    case 24:
                        staticBucketTransmissionMarker =
                            "GEO10.View16.Stage24 TransmissionBeforeMaterialCompose";
                        break;
                    default:
                        staticBucketTransmissionMarker =
                            "GEO10.View16.Stage17 TransmissionIterativeResolve";
                        break;
                }
            }
            else
            {
                const int transmissionIsolationStage = idMath::ClampInt(
                    0,
                    3,
                    r_pathTracingCleanRtxdiDiTransmissionIsolationStage.GetInteger());
                if (transmissionIsolationStage == 1)
                {
                    psrConstants.flags &=
                        ~CLEAN_RTXDI_DI_FLAG_TRANSMISSION_TRACE_PROBE_MASK;
                    psrConstants.flags |=
                        1u << CLEAN_RTXDI_DI_FLAG_TRANSMISSION_TRACE_PROBE_SHIFT;
                    staticBucketTransmissionMarker =
                        "CleanDI.TransmissionPSR.SourceDecodeNoTrace";
                }
                else if (transmissionIsolationStage == 2)
                {
                    psrConstants.flags &=
                        ~CLEAN_RTXDI_DI_FLAG_TRANSMISSION_TRACE_PROBE_MASK;
                    psrConstants.flags |=
                        CLEAN_RTXDI_DI_FLAG_TRANSMISSION_ITERATIVE_RESOLVE |
                        (6u << CLEAN_RTXDI_DI_FLAG_TRANSMISSION_TRACE_PROBE_SHIFT);
                    staticBucketTransmissionMarker =
                        "CleanDI.TransmissionPSR.TraceClosestHitNoResolve";
                }
                else if (transmissionIsolationStage == 3)
                {
                    psrConstants.flags &=
                        ~CLEAN_RTXDI_DI_FLAG_TRANSMISSION_TRACE_PROBE_MASK;
                    psrConstants.flags |=
                        CLEAN_RTXDI_DI_FLAG_TRANSMISSION_ITERATIVE_RESOLVE |
                        (7u << CLEAN_RTXDI_DI_FLAG_TRANSMISSION_TRACE_PROBE_SHIFT);
                    staticBucketTransmissionMarker =
                        "CleanDI.TransmissionPSR.ResolvedTupleNoPublish";
                }
            }
            {
                PathTraceGpuMarkerScope nsightMarker(
                    commandList,
                    staticBucketTransmissionMarker,
                    nsightGpuMarkers &&
                        staticBucketSecondaryIsolationActive);
                DispatchPathTraceCleanRtxdiDiTransmissionPsrPass(
                    commandList,
                    cleanState,
                    cleanArgs,
                    m_smokeCleanRtxdiDiSentinelConstantsBuffer,
                    &psrConstants,
                    sizeof(psrConstants),
                    m_smokeMaterialFeatureRuntimeConstantsBuffer,
                    cleanRtxdiDiMaterialFeaturePasses,
                    m_frameResources,
                    nsightGpuMarkers);
            }
            nvrhi::utils::BufferUavBarrier(commandList, m_frameResources.primarySurfaceHistoryBuffers.current);
            if (r_pathTracingSkyCubeEnvironment.GetInteger() != 0 &&
                m_smokeSkyEnvironmentCube &&
                m_smokeSkySurfaceResolvePipeline &&
                m_smokeSkySurfaceResolveBindingLayout &&
                m_smokeSkySurfaceResolveConstantsBuffer &&
                m_frameResources.rrGuideSpecularAlbedoTexture &&
                m_frameResources.reflectionSidecarTexture &&
                m_frameResources.rrGuidePositionTexture)
            {
                nvrhi::utils::TextureUavBarrier(
                    commandList,
                    m_frameResources.reflectionSidecarTexture);
                nvrhi::utils::TextureUavBarrier(
                    commandList,
                    m_frameResources.rrGuidePositionTexture);
                nvrhi::BindingSetDesc skySurfaceResolveBindingSetDesc;
                skySurfaceResolveBindingSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(
                    0,
                    m_smokeSkySurfaceResolveConstantsBuffer));
                skySurfaceResolveBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(
                    0,
                    m_smokeSkyEnvironmentCube,
                    nvrhi::Format::UNKNOWN,
                    nvrhi::AllSubresources,
                    nvrhi::TextureDimension::TextureCube));
                skySurfaceResolveBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(
                    0,
                    m_frameResources.primarySurfaceHistoryBuffers.current));
                skySurfaceResolveBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(
                    1,
                    m_frameResources.rrGuideSpecularAlbedoTexture));
                skySurfaceResolveBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(
                    2,
                    m_frameResources.reflectionSidecarTexture));
                skySurfaceResolveBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(
                    3,
                    m_frameResources.rrGuidePositionTexture));
                skySurfaceResolveBindingSetDesc.addItem(nvrhi::BindingSetItem::Sampler(
                    0,
                    m_backend->GetCommonPasses().m_LinearClampSampler));
                const nvrhi::BindingSetHandle skySurfaceResolveBindingSet = device->createBindingSet(
                    skySurfaceResolveBindingSetDesc,
                    m_smokeSkySurfaceResolveBindingLayout);
                if (skySurfaceResolveBindingSet)
                {
                    PathTraceSkySurfaceResolveConstants skySurfaceResolveConstants;
                    skySurfaceResolveConstants.width = cleanArgs.width;
                    skySurfaceResolveConstants.height = cleanArgs.height;
                    skySurfaceResolveConstants.brightness =
                        idMath::ClampFloat(0.0f, 64.0f, r_pathTracingSkyCubeBrightness.GetFloat());
                    skySurfaceResolveConstants.enabled = 1u;
                    commandList->writeBuffer(
                        m_smokeSkySurfaceResolveConstantsBuffer,
                        &skySurfaceResolveConstants,
                        sizeof(skySurfaceResolveConstants));

                    nvrhi::ComputeState skySurfaceResolveState;
                    skySurfaceResolveState.pipeline = m_smokeSkySurfaceResolvePipeline;
                    skySurfaceResolveState.bindings = { skySurfaceResolveBindingSet };
                    commandList->setComputeState(skySurfaceResolveState);
                    commandList->dispatch(
                        (cleanArgs.width + 7u) / 8u,
                        (cleanArgs.height + 7u) / 8u,
                        1u);
                    nvrhi::utils::BufferUavBarrier(
                        commandList,
                        m_frameResources.primarySurfaceHistoryBuffers.current);
                    nvrhi::utils::TextureUavBarrier(
                        commandList,
                        m_frameResources.rrGuideSpecularAlbedoTexture);
                    nvrhi::utils::TextureUavBarrier(
                        commandList,
                        m_frameResources.reflectionSidecarTexture);
                    nvrhi::utils::TextureUavBarrier(
                        commandList,
                        m_frameResources.rrGuidePositionTexture);
                }
            }
            if (m_frameResources.transmissionTexture)
            {
                nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.transmissionTexture);
            }
            if (m_frameResources.reflectionSidecarTexture)
            {
                nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.reflectionSidecarTexture);
            }
            if (m_frameResources.glassDistortionSidecarTexture)
            {
                nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.glassDistortionSidecarTexture);
            }
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.motionVectorTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.motionVectorMaskTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideAlbedoTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideSpecularAlbedoTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideNormalRoughnessTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideDepthTexture);
            if (m_frameResources.rrGuideHitDistanceTexture)
            {
                nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideHitDistanceTexture);
            }
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideResetMaskTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuidePositionTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrMotionVectorTexture);
            // Restore the DI constants the PSR dispatch overwrote.
            commandList->writeBuffer(m_smokeCleanRtxdiDiSentinelConstantsBuffer, &dispatchConstants, sizeof(dispatchConstants));
        }
        if (staticBucketSecondaryIsolationActive &&
            (staticBucketSecondaryIsolation.transmissionTupleDiagnostic ||
                staticBucketSecondaryIsolation.
                    transmissionClosestHitPositionDiagnostic))
        {
            if (!m_smokeTestDispatched)
            {
                if (staticBucketSecondaryIsolation.
                        transmissionClosestHitPositionDiagnostic)
                {
                    common->Printf(
                        "PathTracePrimaryPass: GEO-10 view-16 stage-23 closest-hit world position versus raygen/replay diagnostic completed (%dx%d); initial, temporal, spatial, post-DI composition, and later consumers skipped\n",
                        m_frameResources.width,
                        m_frameResources.height);
                }
                else
                {
                    common->Printf(
                        "PathTracePrimaryPass: GEO-10 view-16 stage-22 bucket hardware-hit versus packed-replay tuple diagnostic completed (%dx%d); initial, temporal, spatial, post-DI composition, and later consumers skipped\n",
                        m_frameResources.width,
                        m_frameResources.height);
                }
            }
            m_smokeTestDispatched = true;
            return;
        }
        if (cleanSplitRaygenView)
        {
            if (staticBucketSecondaryIsolation.initial)
            {
                cleanState.shaderTable = cleanRtxdiDiProductionView
                    ? m_smokeCleanRtxdiDiInitialProductionShaderTable
                    : m_smokeCleanRtxdiDiInitialShaderTable;
                commandList->setRayTracingState(cleanState);
                {
                    PathTraceGpuMarkerScope nsightMarker(
                        commandList,
                        staticBucketSecondaryIsolationActive
                            ? "GEO10.View16.Stage4 Initial DispatchRays"
                            : "CleanDI.0 Initial DispatchRays",
                        nsightGpuMarkers);
                    commandList->dispatchRays(cleanArgs);
                }
                nvrhi::utils::BufferUavBarrier(commandList, m_smokeCleanRtxdiDiCurrentReservoirBuffer);
                nvrhi::utils::BufferUavBarrier(commandList, m_smokeCleanRtxdiDiTemporalReservoirBuffer);
                nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.outputTexture);

                if (cleanTemporalRaygenView &&
                    staticBucketSecondaryIsolation.temporal)
                {
                    cleanState.shaderTable = cleanRtxdiDiProductionView
                        ? m_smokeCleanRtxdiDiTemporalProductionShaderTable
                        : m_smokeCleanRtxdiDiTemporalShaderTable;
                    commandList->setRayTracingState(cleanState);
                    {
                        PathTraceGpuMarkerScope nsightMarker(
                            commandList,
                            staticBucketSecondaryIsolationActive
                                ? "GEO10.View16.Stage5 Temporal DispatchRays"
                                : "CleanDI.1 Temporal DispatchRays",
                            nsightGpuMarkers);
                        commandList->dispatchRays(cleanArgs);
                    }
                }
            }
        }
        else
        {
            cleanState.shaderTable = m_smokeCleanRtxdiDiSentinelShaderTable;
            commandList->setRayTracingState(cleanState);
            {
                PathTraceGpuMarkerScope nsightMarker(commandList, "CleanDI.0 Sentinel DispatchRays", nsightGpuMarkers);
                commandList->dispatchRays(cleanArgs);
            }
        }
        nvrhi::utils::BufferUavBarrier(commandList, m_smokeCleanRtxdiDiCurrentReservoirBuffer);
        nvrhi::utils::BufferUavBarrier(commandList, m_smokeCleanRtxdiDiTemporalReservoirBuffer);
        nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.outputTexture);
        if (cleanProductionInitialOnly ||
            (staticBucketSecondaryIsolationActive &&
                staticBucketSecondaryIsolation.initial &&
                !staticBucketSecondaryIsolation.temporal &&
                !staticBucketSecondaryIsolation.spatial))
        {
            if (!m_smokeTestDispatched)
            {
                if (cleanProductionInitialOnly)
                {
                    common->Printf(
                        "PathTracePrimaryPass: clean DI view-16 initial-only dispatch completed (%dx%d); transmission PSR, temporal, spatial, material-feature composition, GI, RR, and later consumers skipped\n",
                        m_frameResources.width,
                        m_frameResources.height);
                }
                else if (staticBucketSecondaryIsolation.stage == 19)
                {
                    common->Printf(
                        "PathTracePrimaryPass: GEO-10 view-16 stage-19 bucket bounded forced-opaque iterative transmission resolve plus initial-only DI completed (%dx%d); maxInteractions=8, legacy any-hit, temporal, spatial, post-DI composition, and later consumers skipped\n",
                        m_frameResources.width,
                        m_frameResources.height);
                }
                else
                {
                    common->Printf(
                        "PathTracePrimaryPass: GEO-10 view-16 stage-4 initial-only dispatch completed (%dx%d); temporal and later consumers skipped\n",
                        m_frameResources.width,
                        m_frameResources.height);
                }
            }
            m_smokeTestDispatched = true;
            return;
        }
        if (cleanProductionTemporalOnly ||
            (staticBucketSecondaryIsolationActive &&
                staticBucketSecondaryIsolation.temporal &&
                !staticBucketSecondaryIsolation.spatial))
        {
            if (!m_smokeTestDispatched)
            {
                if (cleanProductionTemporalOnly)
                {
                    common->Printf(
                        "PathTracePrimaryPass: clean DI view-16 initial-plus-temporal dispatch completed (%dx%d); transmission PSR, spatial, material-feature composition, GI, RR, and later consumers skipped\n",
                        m_frameResources.width,
                        m_frameResources.height);
                }
                else if (staticBucketSecondaryIsolation.stage == 20)
                {
                    common->Printf(
                        "PathTracePrimaryPass: GEO-10 view-16 stage-20 bucket bounded forced-opaque iterative transmission resolve plus initial-and-temporal dispatch completed (%dx%d); maxInteractions=8, legacy any-hit, spatial, post-DI composition, and later consumers skipped\n",
                        m_frameResources.width,
                        m_frameResources.height);
                }
                else if (staticBucketSecondaryIsolation.spatialPipelineCreation)
                {
                    common->Printf(
                        "PathTracePrimaryPass: GEO-10 view-16 stage-6 spatial pipelines plus initial-and-temporal dispatch completed (%dx%d); spatial DispatchRays and later consumers skipped\n",
                        m_frameResources.width,
                        m_frameResources.height);
                }
                else
                {
                    common->Printf(
                        "PathTracePrimaryPass: GEO-10 view-16 stage-5 initial-plus-temporal dispatch completed (%dx%d); spatial and later consumers skipped\n",
                        m_frameResources.width,
                        m_frameResources.height);
                }
            }
            m_smokeTestDispatched = true;
            return;
        }
        if (cleanSpatialRoute &&
            staticBucketSecondaryIsolation.spatial)
        {
            commandList->setBufferState(m_smokeCleanRtxdiDiTemporalReservoirBuffer, nvrhi::ResourceStates::CopySource);
            commandList->setBufferState(m_smokeCleanRtxdiDiPreviousReservoirBuffer, nvrhi::ResourceStates::CopyDest);
            commandList->commitBarriers();
            commandList->copyBuffer(
                m_smokeCleanRtxdiDiPreviousReservoirBuffer,
                0,
                m_smokeCleanRtxdiDiTemporalReservoirBuffer,
                0,
                cleanReservoirBytes);
            m_smokeCleanRtxdiDiPreviousReservoirValid = cleanRtxdiDiTemporalEnabled;
            commandList->setBufferState(m_smokeCleanRtxdiDiPreviousReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_smokeCleanRtxdiDiSpatialReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->commitBarriers();
            nvrhi::rt::State cleanSpatialState = cleanState;
            cleanSpatialState.shaderTable = cleanRtxdiDiProductionView
                ? m_smokeCleanRtxdiDiSpatialProductionShaderTable
                : m_smokeCleanRtxdiDiSpatialShaderTable;
            commandList->setRayTracingState(cleanSpatialState);
            PathTraceCleanRtxdiDiSentinelConstants cleanSpatialConstants = cleanConstants;
            cleanSpatialConstants.emissiveDistributionInfo[3] = static_cast<float>(cleanMaterialOverlayRecordCount);
            commandList->writeBuffer(m_smokeCleanRtxdiDiSentinelConstantsBuffer, &cleanSpatialConstants, sizeof(cleanSpatialConstants));
            {
                PathTraceGpuMarkerScope nsightMarker(
                    commandList,
                    staticBucketSecondaryIsolationActive
                        ? "GEO10.View16.Stage7 Spatial DispatchRays"
                        : "CleanDI.2 Spatial DispatchRays",
                    nsightGpuMarkers);
                commandList->dispatchRays(cleanArgs);
            }
            nvrhi::utils::BufferUavBarrier(commandList, m_smokeCleanRtxdiDiSpatialReservoirBuffer);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.outputTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrInputColorTexture);
        }
        if (cleanProductionSpatialOnly)
        {
            if (!m_smokeTestDispatched)
            {
                common->Printf(
                    "PathTracePrimaryPass: clean DI view-16 optional-transmission-plus-initial-plus-temporal-plus-spatial dispatch completed (%dx%d); transmissionActive=%d material-feature composition, GI, RR, and later consumers skipped\n",
                    m_frameResources.width,
                    m_frameResources.height,
                    glassReflectionProducerActive ? 1 : 0);
            }
            m_smokeTestDispatched = true;
            return;
        }
        if (staticBucketSecondaryIsolationActive &&
            staticBucketSecondaryIsolation.spatial &&
            !staticBucketSecondaryIsolation.materialFeatureCompose)
        {
            if (!m_smokeTestDispatched)
            {
                if (staticBucketSecondaryIsolation.transmissionPsr)
                {
                    if (staticBucketSecondaryIsolation.stage == 10)
                    {
                        common->Printf(
                            "PathTracePrimaryPass: GEO-10 view-16 stage-10 transmission raygen without TraceRay plus initial-temporal-spatial dispatch completed (%dx%d); hit shaders, post-DI composition, and later consumers skipped\n",
                            m_frameResources.width,
                            m_frameResources.height);
                    }
                    else if (staticBucketSecondaryIsolation.stage == 11)
                    {
                        common->Printf(
                            "PathTracePrimaryPass: GEO-10 view-16 stage-11 transmission traversal without hit shaders plus initial-temporal-spatial dispatch completed (%dx%d); any-hit, closest-hit, post-DI composition, and later consumers skipped\n",
                            m_frameResources.width,
                            m_frameResources.height);
                    }
                    else if (staticBucketSecondaryIsolation.stage == 12)
                    {
                        common->Printf(
                            "PathTracePrimaryPass: GEO-10 view-16 stage-12 transmission any-hit-only traversal plus initial-temporal-spatial dispatch completed (%dx%d); closest-hit, post-DI composition, and later consumers skipped\n",
                            m_frameResources.width,
                            m_frameResources.height);
                    }
                    else if (staticBucketSecondaryIsolation.stage == 13)
                    {
                        common->Printf(
                            "PathTracePrimaryPass: GEO-10 view-16 stage-13 transmission any-hit entry-only traversal plus initial-temporal-spatial dispatch completed (%dx%d); geometry/material decode, closest-hit, post-DI composition, and later consumers skipped\n",
                            m_frameResources.width,
                            m_frameResources.height);
                    }
                    else if (staticBucketSecondaryIsolation.stage == 14)
                    {
                        common->Printf(
                            "PathTracePrimaryPass: GEO-10 view-16 stage-14 one complete transmission any-hit invocation plus initial-temporal-spatial dispatch completed (%dx%d); repeated IgnoreHit traversal, closest-hit, post-DI composition, and later consumers skipped\n",
                            m_frameResources.width,
                            m_frameResources.height);
                    }
                    else if (staticBucketSecondaryIsolation.stage == 15)
                    {
                        common->Printf(
                            "PathTracePrimaryPass: GEO-10 view-16 stage-15 bounded repeated transmission any-hit traversal plus initial-temporal-spatial dispatch completed (%dx%d); TMax=4096, closest-hit, post-DI composition, and later consumers skipped\n",
                            m_frameResources.width,
                            m_frameResources.height);
                    }
                    else if (staticBucketSecondaryIsolation.stage == 16)
                    {
                        common->Printf(
                            "PathTracePrimaryPass: GEO-10 view-16 stage-16 one decode-free transmission IgnoreHit plus initial-temporal-spatial dispatch completed (%dx%d); second intersection accepted, TMax=4096, geometry/material decode, closest-hit, post-DI composition, and later consumers skipped\n",
                            m_frameResources.width,
                            m_frameResources.height);
                    }
                    else if (staticBucketSecondaryIsolation.stage == 18)
                    {
                        common->Printf(
                            "PathTracePrimaryPass: GEO-10 view-16 stage-18 bounded forced-opaque iterative transmission resolve against monolithic static traversal plus initial-temporal-spatial dispatch completed (%dx%d); maxInteractions=8, legacy any-hit skipped, post-DI transmission/glass composition and later consumers skipped\n",
                            m_frameResources.width,
                            m_frameResources.height);
                    }
                    else if (staticBucketSecondaryIsolation.stage == 21)
                    {
                        common->Printf(
                            "PathTracePrimaryPass: GEO-10 view-16 stage-21 bucket bounded forced-opaque iterative transmission resolve plus initial-reservoir production presentation completed (%dx%d); maxInteractions=8, temporal=0, spatialNeighborReuse=0, legacy any-hit, post-DI composition, and later consumers skipped\n",
                            m_frameResources.width,
                            m_frameResources.height);
                    }
                    else
                    {
                        common->Printf(
                            "PathTracePrimaryPass: GEO-10 view-16 stage-17 bounded forced-opaque iterative transmission resolve plus initial-temporal-spatial dispatch completed (%dx%d); maxInteractions=8, legacy any-hit skipped, post-DI transmission/glass composition and later consumers skipped\n",
                            m_frameResources.width,
                            m_frameResources.height);
                    }
                }
                else if (staticBucketSecondaryIsolation.
                    materialFeatureRuntimeBindings)
                {
                    common->Printf(
                        "PathTracePrimaryPass: GEO-10 view-16 stage-9 material-feature runtime bindings plus initial-temporal-spatial dispatch completed (%dx%d); transmission/glass DispatchRays and later consumers skipped\n",
                        m_frameResources.width,
                        m_frameResources.height);
                }
                else if (staticBucketSecondaryIsolation.
                    materialFeaturePipelineCreation)
                {
                    common->Printf(
                        "PathTracePrimaryPass: GEO-10 view-16 stage-8 material-feature pipelines plus initial-temporal-spatial dispatch completed (%dx%d); transmission/glass DispatchRays and later consumers skipped\n",
                        m_frameResources.width,
                        m_frameResources.height);
                }
                else
                {
                    common->Printf(
                        "PathTracePrimaryPass: GEO-10 view-16 stage-7 initial-plus-temporal-plus-spatial dispatch completed (%dx%d); post-DI consumers skipped\n",
                        m_frameResources.width,
                        m_frameResources.height);
                }
            }
            m_smokeTestDispatched = true;
            return;
        }
        auto dispatchCleanMaterialFeatureCompose = [&]()
        {
            if (cleanRtxdiDiPsrMaskView ||
                !staticBucketSecondaryIsolation.
                    materialFeatureCompose)
            {
                return;
            }
            if (PathTraceCleanRtxdiDiMaterialFeatureNeedsOutputColorSource(cleanRtxdiDiMaterialFeaturePasses))
            {
                commandList->setTextureState(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
                commandList->setTextureState(m_frameResources.accumulationTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::CopyDest);
                commandList->commitBarriers();
                commandList->copyTexture(
                    m_frameResources.accumulationTexture,
                    nvrhi::TextureSlice(),
                    m_frameResources.outputTexture,
                    nvrhi::TextureSlice());
                commandList->setTextureState(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
                commandList->setTextureState(m_frameResources.accumulationTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
                commandList->commitBarriers();
            }
            PathTraceGpuMarkerScope nsightMarker(
                commandList,
                "GEO10.View16.Stage6 MaterialFeatureCompose",
                nsightGpuMarkers &&
                    staticBucketSecondaryIsolationActive);
            DispatchPathTraceCleanRtxdiDiMaterialFeaturePasses(
                commandList,
                cleanState,
                cleanArgs,
                m_smokeCleanRtxdiDiSentinelConstantsBuffer,
                &cleanConstants,
                sizeof(cleanConstants),
                m_smokeMaterialFeatureRuntimeConstantsBuffer,
                cleanRtxdiDiMaterialFeaturePasses,
                m_frameResources,
                nsightGpuMarkers);
        };
        dispatchCleanMaterialFeatureCompose();
        if (staticBucketSecondaryIsolationActive &&
            staticBucketSecondaryIsolation.stage == 24)
        {
            if (!m_smokeTestDispatched)
            {
                common->Printf(
                    "PathTracePrimaryPass: GEO-10 view-16 stage-24 bounded bucket transmission, initial-temporal-spatial DI, and material-feature composition completed (%dx%d); GI, RR, and later consumers skipped\n",
                    m_frameResources.width,
                    m_frameResources.height);
            }
            m_smokeTestDispatched = true;
            return;
        }
        auto dispatchCleanRestirGi = [&](bool resolveToRrInputColor, bool dlssRrActive) -> bool
        {
            PathTraceCleanRestirGiDispatchInputs giInputs;
            giInputs.device = device;
            giInputs.commandList = commandList;
            giInputs.outputTexture = m_frameResources.outputTexture;
            giInputs.textureDescriptorTable = m_smokeTextureDescriptorTable;
            giInputs.textureBindlessLayout = m_smokeTextureBindlessLayout;
            giInputs.isD3D12 = deviceManager->GetGraphicsAPI() == nvrhi::GraphicsAPI::D3D12;
            giInputs.isVulkan = deviceManager->GetGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN;
            giInputs.width = m_frameResources.width;
            giInputs.height = m_frameResources.height;
            PathTraceCleanRtxdiDiSentinelConstants cleanGiConstants = cleanConstants;
            cleanGiConstants.emissiveDistributionInfo[3] = static_cast<float>(cleanMaterialOverlayRecordCount);
            giInputs.diConstantsBlob = &cleanGiConstants;
            giInputs.diConstantsSize = static_cast<uint32_t>(sizeof(cleanGiConstants));
            giInputs.doomAnalyticLightCountOverride = static_cast<uint32_t>(Max(0, m_smokeDoomAnalyticLightCount));
            giInputs.tlas = m_smokeTlas;
            giInputs.staticVertexBuffer = dispatchStaticVertexBuffer;
            giInputs.staticIndexBuffer = dispatchStaticIndexBuffer;
            giInputs.dynamicVertexBuffer = m_smokeDynamicVertexBuffer;
            giInputs.dynamicIndexBuffer = m_smokeDynamicIndexBuffer;
            giInputs.staticTriangleClassBuffer = dispatchStaticTriangleClassBuffer;
            giInputs.dynamicTriangleClassBuffer = m_smokeDynamicTriangleClassBuffer;
            giInputs.staticTriangleMaterialBuffer = dispatchStaticTriangleMaterialBuffer;
            giInputs.dynamicTriangleMaterialBuffer = m_smokeDynamicTriangleMaterialBuffer;
            giInputs.staticTriangleMaterialIndexBuffer = dispatchStaticTriangleMaterialIndexBuffer;
            giInputs.dynamicTriangleMaterialIndexBuffer = m_smokeDynamicTriangleMaterialIndexBuffer;
            giInputs.materialTableBuffer = m_smokeMaterialTableBuffer;
            giInputs.materialFeatureParameterBuffer = m_smokeMaterialFeatureParameterBuffer;
            giInputs.materialFeatureParameterCount = static_cast<uint32_t>(
                Max(0, m_sceneInputs.materials.materialFeatureParameterRecordCount));
            giInputs.liquidPoolStatusBuffer = m_liquidPoolStatusBuffer;
            giInputs.dynamicMaterialBuffer = m_smokeDynamicMaterialBuffer;
            giInputs.fallbackTexture = cleanFallbackTexture;
            giInputs.skyEnvironmentCube = m_smokeSkyEnvironmentCube;
            const bool cleanGiNeedsEmissiveTriangles = cleanConstants.currentEmissiveTriangleCount > 0u;
            const bool cleanGiNeedsEmissiveDistribution = cleanEmissiveDistributionCount > 0u;
            const bool cleanGiNeedsDoomAnalyticLights =
                cleanConstants.analyticLightCount > 0u || cleanConstants.doomAnalyticFullCurrentCount > 0u;
            const bool cleanGiNeedsRluCurrentLights = cleanConstants.rluCurrentLightCount > 0u;
            giInputs.emissiveTriangleBuffer = cleanGiNeedsEmissiveTriangles
                ? m_smokeEmissiveTriangleBuffer
                : cleanOptionalSrv(m_smokeEmissiveTriangleBuffer);
            giInputs.emissiveDistributionBuffer = cleanGiNeedsEmissiveDistribution
                ? m_smokeEmissiveDistributionBuffer
                : cleanOptionalSrv(m_smokeEmissiveDistributionBuffer);
            giInputs.rigidRouteVertexBuffer = m_smokeRigidRouteVertexBuffer;
            giInputs.rigidRouteIndexBuffer = m_smokeRigidRouteIndexBuffer;
            giInputs.rigidRouteTriangleMaterialBuffer = m_smokeRigidRouteTriangleMaterialBuffer;
            giInputs.rigidRouteTriangleMaterialIndexBuffer = m_smokeRigidRouteTriangleMaterialIndexBuffer;
            giInputs.rigidRouteInstanceBuffer = m_smokeRigidRouteInstanceBuffer;
            giInputs.skinnedHitRouteRecordBuffer = m_smokeSkinnedHitRouteRecordBuffer;
            giInputs.skinnedHitRouteTriangleBuffer = m_smokeSkinnedHitRouteTriangleBuffer;
            // Match the primary RT binding contract: before a skinned route
            // exists, t28/t29 use type-compatible legacy geometry buffers.
            // No skinned TLAS contribution can index these fallbacks.
            giInputs.skinnedSourceIndexBuffer =
                m_sceneInputs.geometry.skinnedSourceIndexBuffer
                    ? m_sceneInputs.geometry.skinnedSourceIndexBuffer
                    : m_smokeDynamicIndexBuffer;
            giInputs.skinnedCurrentOutputVertexBuffer =
                m_smokeSkinnedCurrentOutputVertexBuffer
                    ? m_smokeSkinnedCurrentOutputVertexBuffer
                    : m_smokeDynamicVertexBuffer;
            giInputs.skinnedPreviousPositionBuffer = cleanOptionalSrv(m_smokeSkinnedPreviousPositionBuffer);
            giInputs.doomAnalyticLightBuffer = cleanGiNeedsDoomAnalyticLights
                ? m_smokeDoomAnalyticLightBuffer
                : cleanOptionalSrv(m_smokeDoomAnalyticLightBuffer);
            giInputs.rluCurrentLightBuffer = cleanGiNeedsRluCurrentLights
                ? m_smokeRestirLightManagerCurrentPayloadBuffer
                : cleanOptionalSrv(m_smokeRestirLightManagerCurrentPayloadBuffer);
            giInputs.neeCacheProviderResultBuffer = cleanNeeCacheProviderSrv;
            giInputs.neeCacheCellBuffer = cleanNeeCacheCellSrv;
            giInputs.neeCacheCandidateBuffer = cleanNeeCacheCandidateSrv;
            giInputs.diReservoirBuffer = cleanSpatialRoute
                ? m_smokeCleanRtxdiDiSpatialReservoirBuffer
                : (cleanRtxdiDiTemporalEnabled
                    ? m_smokeCleanRtxdiDiTemporalReservoirBuffer
                    : m_smokeCleanRtxdiDiCurrentReservoirBuffer);
            giInputs.primarySurfaceCurrentBuffer = m_frameResources.primarySurfaceHistoryBuffers.current;
            giInputs.primarySurfacePreviousBuffer = m_frameResources.primarySurfaceHistoryBuffers.previous;
            giInputs.motionVectorTexture = m_frameResources.motionVectorTexture;
            giInputs.motionVectorMaskTexture = m_frameResources.motionVectorMaskTexture;
            giInputs.rrInputColorTexture = m_frameResources.rrInputColorTexture;
            giInputs.rrGuideAlbedoTexture = m_frameResources.rrGuideAlbedoTexture;
            giInputs.rrGuideHitDistanceTexture = m_frameResources.rrGuideHitDistanceTexture;
            giInputs.materialSampler = m_backend->GetCommonPasses().m_AnisotropicWrapSampler;
            giInputs.resolveToRrInputColor = resolveToRrInputColor;
            giInputs.dlssRrActive = dlssRrActive;
            return PathTraceCleanRestirGiExecute(m_cleanRestirGiState, giInputs);
        };
        const bool cleanDlssRrEvaluateRequested =
            !staticBucketSecondaryIsolationActive &&
            cleanSpatialRoute &&
            (cleanRtxdiDiView == 12 || cleanRtxdiDiView == 16) &&
            r_pathTracingDLSSRR.GetInteger() != 0 &&
            r_pathTracingDLSSRRGuideDebugView.GetInteger() == 0;
        const bool cleanGiDispatchRequested =
            r_pathTracingCleanRestirGiEnable.GetInteger() != 0;
        const bool cleanGiView0ResolveRequested =
            r_pathTracingCleanRestirGiView.GetInteger() == 0 &&
            r_pathTracingCleanRestirGiResolve.GetInteger() != 0;
        const bool cleanGiRrExportRequested =
            cleanGiDispatchRequested &&
            r_pathTracingCleanRestirGiView.GetInteger() == 0 &&
            r_pathTracingCleanRestirGiSpecularProducer.GetInteger() != 0 &&
            (r_pathTracingCleanRestirGiRrHitDistance.GetInteger() != 0 ||
                r_pathTracingCleanRestirGiRrSpecularInput.GetInteger() != 0);
        const bool cleanGiPreDlssRequested =
            cleanGiDispatchRequested &&
            cleanDlssRrEvaluateRequested &&
            (cleanGiView0ResolveRequested || cleanGiRrExportRequested);
        bool cleanGiDispatchedBeforeRr = false;
        if (cleanDlssRrEvaluateRequested)
        {
            const idVec2 cleanDlssRrJitterPixels = PathTraceDLSSRRPixelJitter(viewDef, cleanConstants.frameIndex, true);
            if (cleanRtxdiDiView == 12)
            {
                commandList->clearTextureFloat(m_frameResources.rrGuideNormalRoughnessTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 1.0f, 1.0f));
            }
            // Do not clear specular hit distance when either reflection route
            // wrote real mirror ray lengths into this buffer.
            if (r_pathTracingCleanRtxdiDiGlassReflectionPsr.GetInteger() == 0 &&
                r_pathTracingReflectionOpaqueMirror.GetInteger() == 0)
            {
                commandList->clearTextureFloat(m_frameResources.rrGuideHitDistanceTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));
            }
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.outputTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrInputColorTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.motionVectorTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrMotionVectorTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideAlbedoTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideSpecularAlbedoTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideNormalRoughnessTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideDepthTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideHitDistanceTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideResetMaskTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuidePositionTexture);

            const bool cleanDiBoilingFilterEnabled =
                r_cleanDiBoilingFilter.GetInteger() != 0 &&
                r_cleanDiBoilingThreshold.GetFloat() > 1.0f &&
                m_smokeCleanRtxdiDiBoilingFilterPipeline &&
                m_smokeCleanRtxdiDiBoilingFilterBindingLayout &&
                m_smokeCleanRtxdiDiBoilingFilterConstantsBuffer &&
                m_frameResources.rrInputColorTexture &&
                m_frameResources.cleanRtxdiDiBoilingFilterTexture;
            if (cleanDiBoilingFilterEnabled)
            {
                if (!m_smokeCleanRtxdiDiBoilingFilterBindingSet ||
                    m_smokeCleanRtxdiDiBoilingFilterInputTexture != m_frameResources.cleanRtxdiDiBoilingFilterTexture ||
                    m_smokeCleanRtxdiDiBoilingFilterOutputTexture != m_frameResources.rrInputColorTexture)
                {
                    nvrhi::BindingSetDesc cleanDiBoilingFilterBindingSetDesc;
                    cleanDiBoilingFilterBindingSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(0, m_smokeCleanRtxdiDiBoilingFilterConstantsBuffer));
                    cleanDiBoilingFilterBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(1, m_frameResources.cleanRtxdiDiBoilingFilterTexture));
                    cleanDiBoilingFilterBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(2, m_frameResources.rrInputColorTexture));
                    m_smokeCleanRtxdiDiBoilingFilterBindingSet = device->createBindingSet(cleanDiBoilingFilterBindingSetDesc, m_smokeCleanRtxdiDiBoilingFilterBindingLayout);
                    m_smokeCleanRtxdiDiBoilingFilterInputTexture = m_smokeCleanRtxdiDiBoilingFilterBindingSet ? m_frameResources.cleanRtxdiDiBoilingFilterTexture : nullptr;
                    m_smokeCleanRtxdiDiBoilingFilterOutputTexture = m_smokeCleanRtxdiDiBoilingFilterBindingSet ? m_frameResources.rrInputColorTexture : nullptr;
                }

                if (m_smokeCleanRtxdiDiBoilingFilterBindingSet)
                {
                    PathTraceCleanRtxdiDiBoilingFilterConstants boilingConstants;
                    boilingConstants.width = static_cast<uint32_t>(Max(m_frameResources.width, 0));
                    boilingConstants.height = static_cast<uint32_t>(Max(m_frameResources.height, 0));
                    boilingConstants.threshold = idMath::ClampFloat(1.0f, 1024.0f, r_cleanDiBoilingThreshold.GetFloat());
                    boilingConstants.enabled = 1u;
                    commandList->writeBuffer(m_smokeCleanRtxdiDiBoilingFilterConstantsBuffer, &boilingConstants, sizeof(boilingConstants));

                    commandList->setTextureState(m_frameResources.rrInputColorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
                    commandList->setTextureState(m_frameResources.cleanRtxdiDiBoilingFilterTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::CopyDest);
                    commandList->commitBarriers();
                    commandList->copyTexture(m_frameResources.cleanRtxdiDiBoilingFilterTexture, nvrhi::TextureSlice(), m_frameResources.rrInputColorTexture, nvrhi::TextureSlice());

                    commandList->setTextureState(m_frameResources.cleanRtxdiDiBoilingFilterTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
                    commandList->setTextureState(m_frameResources.rrInputColorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
                    commandList->commitBarriers();

                    nvrhi::ComputeState cleanDiBoilingFilterState;
                    cleanDiBoilingFilterState.pipeline = m_smokeCleanRtxdiDiBoilingFilterPipeline;
                    cleanDiBoilingFilterState.bindings = { m_smokeCleanRtxdiDiBoilingFilterBindingSet };
                    commandList->setComputeState(cleanDiBoilingFilterState);
                    {
                        PathTraceGpuMarkerScope nsightMarker(commandList, "CleanDI.4 BoilingFilter Dispatch", nsightGpuMarkers);
                        commandList->dispatch(
                            static_cast<uint32_t>((m_frameResources.width + 7) / 8),
                            static_cast<uint32_t>((m_frameResources.height + 7) / 8),
                            1);
                    }

                    nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrInputColorTexture);
                }
            }

            if (cleanGiPreDlssRequested)
            {
                dispatchCleanRestirGi(cleanGiView0ResolveRequested, true);
                dispatchCleanMaterialFeatureCompose();
                cleanGiDispatchedBeforeRr = true;
            }

            commandList->setTextureState(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_frameResources.rrInputColorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_frameResources.motionVectorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_frameResources.rrMotionVectorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_frameResources.rrGuideAlbedoTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_frameResources.rrGuideSpecularAlbedoTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_frameResources.rrGuideNormalRoughnessTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_frameResources.rrGuideDepthTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_frameResources.rrGuideHitDistanceTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_frameResources.rrGuideResetMaskTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_frameResources.rrGuidePositionTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            commandList->setTextureState(m_frameResources.accumulationTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->commitBarriers();

            const bool cleanRrForceReset = r_pathTracingDLSSRRForceReset.GetInteger() != 0;
            // Only TRUE history discontinuities warrant resetting DLSS-RR's temporal history:
            // resolution/backbuffer resize and a full scene/map rebuild. The reservoir-scene
            // signature, reservoir-dispatch signature and GPU-idle-wait reasons are internal
            // ReSTIR bookkeeping that churns every frame during motion -- forwarding them to
            // DLSS-RR wiped its history continuously and was the motion-boiling root cause.
            const uint32_t kRrHistoryResetMask =
                RT_FRAME_RESET_OUTPUT_RESIZE |
                RT_FRAME_RESET_BACKBUFFER_RESIZE |
                RT_FRAME_RESET_SCENE_RESOURCES;
            const bool cleanRrHistoryReset =
                cleanRrForceReset ||
                !m_frameResources.primarySurfaceHistoryView.valid ||
                m_frameResources.primarySurfaceHistoryNeedsClear ||
                ( m_frameResources.settings.resetReasonFlags & kRrHistoryResetMask ) != 0;
            // Diagnostic: DLSS-RR history reset cadence. If this spams while merely moving,
            // we are resetting RR history on internal scene/reservoir bookkeeping rather than
            // on a true history discontinuity (the suspected motion-boiling root cause).
            if( r_pathTracingDLSSRRVerbose.GetInteger() != 0 && cleanRrHistoryReset )
            {
                common->Printf(
                    "PathTraceDLSSRR: HISTORY RESET force=%d historyViewValid=%d needsClear=%d resetReasonFlags=0x%08x\n",
                    cleanRrForceReset ? 1 : 0,
                    m_frameResources.primarySurfaceHistoryView.valid ? 1 : 0,
                    m_frameResources.primarySurfaceHistoryNeedsClear ? 1 : 0,
                    static_cast<unsigned int>( m_frameResources.settings.resetReasonFlags ) );
            }
            const bool cleanRrEvaluated = PathTraceDLSSRRBridge_Evaluate(
                commandList,
                m_frameResources.rrInputColorTexture,
                m_frameResources.accumulationTexture,
                m_frameResources.rrGuideAlbedoTexture,
                m_frameResources.rrGuideSpecularAlbedoTexture,
                m_frameResources.rrGuideNormalRoughnessTexture,
                m_frameResources.rrGuidePositionTexture,
                m_frameResources.rrGuideDepthTexture,
                m_frameResources.rrMotionVectorTexture,
                m_frameResources.rrGuideHitDistanceTexture,
                nullptr,
                viewDef,
                cleanConstants.frameIndex,
                m_frameResources.width,
                m_frameResources.height,
                m_frameResources.outputWidth,
                m_frameResources.outputHeight,
                cleanDlssRrJitterPixels.x,
                cleanDlssRrJitterPixels.y,
                cleanRrHistoryReset ? nullptr : &m_frameResources.primarySurfaceHistoryView,
                cleanRrHistoryReset);
            if (cleanRrEvaluated)
            {
                commandList->setTextureState(m_frameResources.accumulationTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
                commandList->setTextureState(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::CopyDest);
                commandList->commitBarriers();
                commandList->copyTexture(m_frameResources.outputTexture, nvrhi::TextureSlice(), m_frameResources.accumulationTexture, nvrhi::TextureSlice());
                commandList->setTextureState(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
                commandList->setTextureState(m_frameResources.accumulationTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
                commandList->commitBarriers();
            }
        }
        if (!cleanSpatialRoute && cleanRtxdiDiView >= 4 && cleanPromoteSubviewReservoir)
        {
            commandList->setBufferState(m_smokeCleanRtxdiDiTemporalReservoirBuffer, nvrhi::ResourceStates::CopySource);
            commandList->setBufferState(m_smokeCleanRtxdiDiPreviousReservoirBuffer, nvrhi::ResourceStates::CopyDest);
            commandList->commitBarriers();
            commandList->copyBuffer(
                m_smokeCleanRtxdiDiPreviousReservoirBuffer,
                0,
                m_smokeCleanRtxdiDiTemporalReservoirBuffer,
                0,
                cleanReservoirBytes);
            m_smokeCleanRtxdiDiPreviousReservoirValid = cleanRtxdiDiTemporalEnabled;
        }
        if (cleanGiDispatchRequested && !cleanGiDispatchedBeforeRr)
        {
            // Clean-room ReSTIR GI lane (docs/restir_remix_gi_cleanroom).
            // It must run before primary-surface history promotion below:
            // GI temporal reuse samples the previous-frame surface buffer,
            // and copying current->previous first turns camera-motion
            // reprojection into current-frame lookups.
            dispatchCleanRestirGi(false, false);
            dispatchCleanMaterialFeatureCompose();
        }
        if (cleanRtxdiDiRrGuideDebugView)
        {
            // Draw after all guide producers, including Clean GI. Clearing or
            // drawing before GI made specular hit distance permanently empty
            // in this diagnostic even when the live DLSS-RR input was valid.
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.outputTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrInputColorTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.motionVectorTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrMotionVectorTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideAlbedoTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideSpecularAlbedoTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideNormalRoughnessTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideDepthTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideHitDistanceTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideResetMaskTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuidePositionTexture);
            nvrhi::rt::State cleanMosaicState = cleanState;
            cleanMosaicState.shaderTable = m_smokeCleanRtxdiDiSentinelShaderTable;
            commandList->setRayTracingState(cleanMosaicState);
            PathTraceCleanRtxdiDiSentinelConstants mosaicConstants = cleanConstants;
            mosaicConstants.view = static_cast<uint32_t>(cleanRtxdiDiView);
            commandList->writeBuffer(m_smokeCleanRtxdiDiSentinelConstantsBuffer, &mosaicConstants, sizeof(mosaicConstants));
            {
                PathTraceGpuMarkerScope nsightMarker(commandList, "CleanDI.3 RrGuideMosaic DispatchRays", nsightGpuMarkers);
                commandList->dispatchRays(cleanArgs);
            }
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.outputTexture);
        }
        ExecutePathTraceParticleComposite(commandList, viewDef);
        QueueStaticContractShaderSample(commandList);
        if (cleanRtxdiDiView >= 2 && cleanPromoteSubviewSurface)
        {
            commandList->setBufferState(m_frameResources.primarySurfaceHistoryBuffers.current, nvrhi::ResourceStates::CopySource);
            commandList->setBufferState(m_frameResources.primarySurfaceHistoryBuffers.previous, nvrhi::ResourceStates::CopyDest);
            commandList->commitBarriers();
            commandList->copyBuffer(
                m_frameResources.primarySurfaceHistoryBuffers.previous,
                0,
                m_frameResources.primarySurfaceHistoryBuffers.current,
                0,
                m_frameResources.primarySurfaceHistoryBuffers.surfaceBytes);

            idVec3 cleanHistoryForward = viewDef->renderView.viewaxis[0];
            idVec3 cleanHistoryLeft = viewDef->renderView.viewaxis[1];
            idVec3 cleanHistoryUp = viewDef->renderView.viewaxis[2];
            cleanHistoryForward.Normalize();
            cleanHistoryLeft.Normalize();
            cleanHistoryUp.Normalize();

            RtPathTraceFrameCameraState currentHistoryView;
            currentHistoryView.valid = true;
            currentHistoryView.width = m_frameResources.width;
            currentHistoryView.height = m_frameResources.height;
            currentHistoryView.origin = viewDef->renderView.vieworg;
            currentHistoryView.forward = cleanHistoryForward;
            currentHistoryView.left = cleanHistoryLeft;
            currentHistoryView.up = cleanHistoryUp;
            currentHistoryView.tanX = idMath::Tan(DEG2RAD(viewDef->renderView.fov_x * 0.5f));
            currentHistoryView.tanY = idMath::Tan(DEG2RAD(viewDef->renderView.fov_y * 0.5f));
            const bool objectMotionAvailable =
                (m_sceneInputs.geometry.skinnedPreviousPositionBufferAvailable && m_sceneInputs.geometry.skinnedSurfaceDispatchCount > 0) ||
                (m_sceneInputs.geometry.previousTransformAvailable && m_sceneInputs.geometry.rigidRouteInstanceCount > 0);
            m_frameResources.SetPrimarySurfaceHistoryView(currentHistoryView, objectMotionAvailable);
            m_frameResources.primarySurfaceHistoryNeedsClear = false;
        }
        if (!m_smokeTestDispatched)
        {
            common->Printf("PathTracePrimaryPass: dispatched clean-room RTXDI DI sentinel raygen (%dx%d, view=%d)\n", m_frameResources.width, m_frameResources.height, cleanRtxdiDiView);
        }
        m_smokeTestDispatched = true;
        if (cleanRequestedLiquidPoolMode != 0 && cleanLiquidPoolTelemetryReady)
        {
            nvrhi::utils::BufferUavBarrier(commandList, m_liquidPoolStatusBuffer);
            if (!m_liquidPoolStatusReadbackQueued)
            {
                commandList->setBufferState(m_liquidPoolStatusBuffer, nvrhi::ResourceStates::CopySource);
                commandList->setBufferState(m_liquidPoolStatusReadbackBuffer, nvrhi::ResourceStates::CopyDest);
                commandList->commitBarriers();
                commandList->copyBuffer(
                    m_liquidPoolStatusReadbackBuffer,
                    0,
                    m_liquidPoolStatusBuffer,
                    0,
                    sizeof(uint32_t) * 16u);
                commandList->setBufferState(m_liquidPoolStatusBuffer, nvrhi::ResourceStates::UnorderedAccess);
                commandList->commitBarriers();
                m_liquidPoolStatusReadbackQueued = true;
                m_liquidPoolStatusReadbackDelayFrames = 3;
            }
        }
        const int cleanLiquidDebug = idMath::ClampInt(0, 6, r_pathTracingLiquidPoolDebug.GetInteger());
        if (cleanLiquidDebug != 0 && r_pathTracingReadbackEnable.GetInteger() != 0 &&
            !m_frameResources.readbackQueued && m_frameResources.readbackTexture)
        {
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.outputTexture);
            commandList->setTextureState(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
            commandList->commitBarriers();
            commandList->copyTexture(
                m_frameResources.readbackTexture,
                nvrhi::TextureSlice(),
                m_frameResources.outputTexture,
                nvrhi::TextureSlice());
            commandList->setTextureState(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->commitBarriers();
            m_frameResources.readbackQueued = true;
            m_frameResources.readbackDelayFrames = 2;
            m_frameResources.readbackCooldownFrames = 0;
        }
        if (!cleanRtxdiDiProductionView && r_pathTracingCleanRtxdiDiTemporalAudit.GetInteger() != 0)
        {
            if (!m_frameResources.readbackQueued && m_frameResources.readbackTexture)
            {
                commandList->setTextureState(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
                commandList->commitBarriers();
                commandList->copyTexture(m_frameResources.readbackTexture, nvrhi::TextureSlice(), m_frameResources.outputTexture, nvrhi::TextureSlice());
                m_frameResources.readbackQueued = true;
                m_frameResources.readbackDelayFrames = 2;
                m_frameResources.RecordReadbackQueued();
                if (r_pathTracingSmokeLog.GetInteger() != 0)
                {
                    common->Printf("PathTracePrimaryPass: queued clean RTXDI DI temporal audit readback\n");
                }
            }
            else if (!m_frameResources.readbackTexture && r_pathTracingSmokeLog.GetInteger() != 0)
            {
                common->Printf("PathTracePrimaryPass: clean RTXDI DI temporal audit readback missing staging texture\n");
            }
        }
        return;
    }
    if (pdfNeeRluCurrentProducerRequested)
    {
        if (!m_smokeRestirPdfNeeRluCurrentShaderTable)
        {
            InitRayTracingSmokeRestirPipeline(18);
        }
        if (!m_smokeRestirPdfNeeRluCurrentShaderTable)
        {
            return;
        }
    }
    if (regirDebugRouteRequested)
    {
        if (!m_smokeReGIRDebugShaderTable)
        {
            InitRayTracingSmokeRestirPipeline(17);
        }
        if (!m_smokeReGIRDebugShaderTable)
        {
            return;
        }
    }
    if (neeCacheRouteRequested)
    {
        if (!m_smokeNeeCacheDebugShaderTable)
        {
            InitRayTracingSmokeRestirPipeline(19);
        }
        if (!m_smokeNeeCacheDebugShaderTable)
        {
            return;
        }
    }

    nvrhi::BindingSetHandle neeCacheDebugBindingSet;
    if (neeCacheRouteRequested)
    {
        if (!device)
        {
            return;
        }
        if (!m_smokeNeeCacheState.providerResultBuffer ||
            !m_smokeNeeCacheState.cellBuffer ||
            !m_smokeNeeCacheState.taskBuffer ||
            !m_smokeNeeCacheState.candidateBuffer)
        {
            return;
        }

        nvrhi::BindingSetDesc neeCacheBindingSetDesc;
        auto neeCacheOptionalSrv = [&](const nvrhi::BufferHandle& buffer) -> nvrhi::BufferHandle {
            return buffer ? buffer : m_smokeNeeCacheState.placeholderSrvBuffer;
        };
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, m_smokeTlas));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(1, m_frameResources.outputTexture));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(2, m_smokeConstantsBuffer));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, neeCacheOptionalSrv(dispatchStaticVertexBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, neeCacheOptionalSrv(dispatchStaticIndexBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, neeCacheOptionalSrv(m_smokeDynamicVertexBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, neeCacheOptionalSrv(m_smokeDynamicIndexBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(16, neeCacheOptionalSrv(m_smokeEmissiveTriangleBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(22, neeCacheOptionalSrv(m_smokeRigidRouteVertexBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(23, neeCacheOptionalSrv(m_smokeRigidRouteIndexBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(26, neeCacheOptionalSrv(m_smokeRigidRouteInstanceBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(18, neeCacheOptionalSrv(m_smokeSkinnedHitRouteRecordBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(19, neeCacheOptionalSrv(m_smokeSkinnedHitRouteTriangleBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(28, neeCacheOptionalSrv(m_sceneInputs.geometry.skinnedSourceIndexBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(29, neeCacheOptionalSrv(m_smokeSkinnedCurrentOutputVertexBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(32, neeCacheOptionalSrv(m_smokeSkinnedPreviousPositionBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(27, neeCacheOptionalSrv(m_smokeDoomAnalyticLightBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(42, neeCacheOptionalSrv(m_smokeDoomAnalyticCurrentIdentityBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(43, neeCacheOptionalSrv(m_smokeDoomAnalyticPreviousIdentityBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(44, neeCacheOptionalSrv(m_smokeDoomAnalyticRemapBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(45, neeCacheOptionalSrv(m_smokeDoomAnalyticPreviousLightBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(57, neeCacheOptionalSrv(m_smokePreviousEmissiveTriangleBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(58, neeCacheOptionalSrv(m_smokeEmissiveRemapBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(59, neeCacheOptionalSrv(m_smokeUnifiedLightBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(60, neeCacheOptionalSrv(m_smokeUnifiedPreviousLightBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(61, neeCacheOptionalSrv(m_smokeUnifiedLightRemapBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(64, neeCacheOptionalSrv(m_smokeRestirLightManagerCurrentToPreviousBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(65, neeCacheOptionalSrv(m_smokeRestirLightManagerPreviousToCurrentBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(66, neeCacheOptionalSrv(m_smokeRestirLightManagerCurrentPayloadBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(67, neeCacheOptionalSrv(m_smokeRestirLightManagerPreviousPayloadBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(PATH_TRACE_NEE_CACHE_BINDING_PROVIDER_RESULT_UAV, m_smokeNeeCacheState.providerResultBuffer));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(PATH_TRACE_NEE_CACHE_BINDING_CELL_UAV, m_smokeNeeCacheState.cellBuffer));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(PATH_TRACE_NEE_CACHE_BINDING_TASK_UAV, m_smokeNeeCacheState.taskBuffer));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(PATH_TRACE_NEE_CACHE_BINDING_CANDIDATE_UAV, m_smokeNeeCacheState.candidateBuffer));
        neeCacheDebugBindingSet = device->createBindingSet(neeCacheBindingSetDesc, m_smokeNeeCacheDebugBindingLayout);
        if (!neeCacheDebugBindingSet)
        {
            return;
        }
    }

    nvrhi::BindingSetHandle regirDebugBindingSet;
    if (regirDebugRouteRequested)
    {
        if (!device)
        {
            return;
        }
        if (!m_smokeReGIRState.candidateCacheBuffer)
        {
            return;
        }

        auto regirOptionalSrv = [&](const nvrhi::BufferHandle& buffer) -> nvrhi::BufferHandle {
            return buffer ? buffer : m_smokeReGIRState.placeholderSrvBuffer;
        };

        nvrhi::BindingSetDesc regirBindingSetDesc;
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, m_smokeTlas));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(1, m_frameResources.outputTexture));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(2, m_smokeConstantsBuffer));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, regirOptionalSrv(dispatchStaticVertexBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, regirOptionalSrv(dispatchStaticIndexBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(5, regirOptionalSrv(dispatchStaticTriangleClassBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, regirOptionalSrv(m_smokeDynamicVertexBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, regirOptionalSrv(m_smokeDynamicIndexBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(8, regirOptionalSrv(m_smokeDynamicTriangleClassBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(9, regirOptionalSrv(dispatchStaticTriangleMaterialBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(10, regirOptionalSrv(m_smokeDynamicTriangleMaterialBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(11, regirOptionalSrv(dispatchStaticTriangleMaterialIndexBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(12, regirOptionalSrv(m_smokeDynamicTriangleMaterialIndexBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(13, regirOptionalSrv(m_smokeMaterialTableBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(16, regirOptionalSrv(m_smokeEmissiveTriangleBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(22, regirOptionalSrv(m_smokeRigidRouteVertexBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(23, regirOptionalSrv(m_smokeRigidRouteIndexBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(24, regirOptionalSrv(m_smokeRigidRouteTriangleMaterialBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(25, regirOptionalSrv(m_smokeRigidRouteTriangleMaterialIndexBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(26, regirOptionalSrv(m_smokeRigidRouteInstanceBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(18, regirOptionalSrv(m_smokeSkinnedHitRouteRecordBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(19, regirOptionalSrv(m_smokeSkinnedHitRouteTriangleBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(28, regirOptionalSrv(m_sceneInputs.geometry.skinnedSourceIndexBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(29, regirOptionalSrv(m_smokeSkinnedCurrentOutputVertexBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(32, regirOptionalSrv(m_smokeSkinnedPreviousPositionBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(27, regirOptionalSrv(m_smokeDoomAnalyticLightBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(42, regirOptionalSrv(m_smokeDoomAnalyticCurrentIdentityBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(43, regirOptionalSrv(m_smokeDoomAnalyticPreviousIdentityBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(44, regirOptionalSrv(m_smokeDoomAnalyticRemapBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(45, regirOptionalSrv(m_smokeDoomAnalyticPreviousLightBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(57, regirOptionalSrv(m_smokePreviousEmissiveTriangleBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(58, regirOptionalSrv(m_smokeEmissiveRemapBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(59, regirOptionalSrv(m_smokeUnifiedLightBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(60, regirOptionalSrv(m_smokeUnifiedPreviousLightBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(61, regirOptionalSrv(m_smokeUnifiedLightRemapBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(64, regirOptionalSrv(m_smokeRestirLightManagerCurrentToPreviousBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(65, regirOptionalSrv(m_smokeRestirLightManagerPreviousToCurrentBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(66, regirOptionalSrv(m_smokeRestirLightManagerCurrentPayloadBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(67, regirOptionalSrv(m_smokeRestirLightManagerPreviousPayloadBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(72, m_smokeReGIRState.candidateCacheBuffer));
        regirDebugBindingSet = device->createBindingSet(regirBindingSetDesc, m_smokeReGIRDebugBindingLayout);
        if (!regirDebugBindingSet)
        {
            return;
        }
    }

    nvrhi::BindingSetHandle pdfNeeVerifierBindingSet;
    if (pdfNeeRluCurrentProducerRequested)
    {
        if (!device)
        {
            return;
        }
        const nvrhi::TextureHandle pdfNeeFallbackTexture = !m_smokeActiveTextureTable.empty() ? m_smokeActiveTextureTable[0] : nullptr;
        if (!pdfNeeFallbackTexture)
        {
            return;
        }

        const uint64 pdfNeeCleanReservoirBlockSize = 16;
        const uint64 pdfNeeCleanReservoirBlocksX = (static_cast<uint64>(Max(1, m_frameResources.width)) + pdfNeeCleanReservoirBlockSize - 1ull) / pdfNeeCleanReservoirBlockSize;
        const uint64 pdfNeeCleanReservoirBlocksY = (static_cast<uint64>(Max(1, m_frameResources.height)) + pdfNeeCleanReservoirBlockSize - 1ull) / pdfNeeCleanReservoirBlockSize;
        const uint64 pdfNeeCleanReservoirCount64 = pdfNeeCleanReservoirBlocksX * pdfNeeCleanReservoirBlocksY * pdfNeeCleanReservoirBlockSize * pdfNeeCleanReservoirBlockSize;
        if (pdfNeeCleanReservoirCount64 > 0xffffffffull)
        {
            return;
        }

        const uint32_t pdfNeeCleanReservoirCount = static_cast<uint32_t>(pdfNeeCleanReservoirCount64);
        const uint64_t pdfNeeCleanReservoirBytes = pdfNeeCleanReservoirCount64 * static_cast<uint64_t>(sizeof(RTXDI_PackedDIReservoir));
        auto ensurePdfNeeCleanReservoir = [&](nvrhi::BufferHandle& buffer, uint32_t& count, uint64_t& bytes, const char* debugName) -> bool
        {
            const bool valid =
                buffer &&
                buffer->getDesc().structStride == sizeof(RTXDI_PackedDIReservoir) &&
                buffer->getDesc().byteSize >= pdfNeeCleanReservoirBytes;
            if (valid)
            {
                return true;
            }

            nvrhi::BufferDesc cleanReservoirDesc;
            cleanReservoirDesc.debugName = debugName;
            cleanReservoirDesc.byteSize = pdfNeeCleanReservoirBytes;
            cleanReservoirDesc.structStride = sizeof(RTXDI_PackedDIReservoir);
            cleanReservoirDesc.canHaveUAVs = true;
            cleanReservoirDesc.canHaveTypedViews = false;
            cleanReservoirDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
            cleanReservoirDesc.keepInitialState = true;
            buffer = device->createBuffer(cleanReservoirDesc);
            count = buffer ? pdfNeeCleanReservoirCount : 0u;
            bytes = buffer ? pdfNeeCleanReservoirBytes : 0ull;
            m_smokeCleanRtxdiDiPreviousReservoirValid = false;
            m_smokeCleanRtxdiDiPreviousReservoirResetReason = 3u;
            return buffer != nullptr;
        };

        if (!ensurePdfNeeCleanReservoir(m_smokeCleanRtxdiDiCurrentReservoirBuffer, m_smokeCleanRtxdiDiCurrentReservoirCount, m_smokeCleanRtxdiDiCurrentReservoirBytes, "PathTraceCleanRtxdiDiCurrentReservoirs") ||
            !ensurePdfNeeCleanReservoir(m_smokeCleanRtxdiDiTemporalReservoirBuffer, m_smokeCleanRtxdiDiTemporalReservoirCount, m_smokeCleanRtxdiDiTemporalReservoirBytes, "PathTraceCleanRtxdiDiTemporalReservoirs") ||
            !ensurePdfNeeCleanReservoir(m_smokeCleanRtxdiDiPreviousReservoirBuffer, m_smokeCleanRtxdiDiPreviousReservoirCount, m_smokeCleanRtxdiDiPreviousReservoirBytes, "PathTraceCleanRtxdiDiPreviousReservoirs"))
        {
            return;
        }

        const nvrhi::BufferHandle pdfNeeReGIRCandidateSrv =
            m_smokeReGIRState.candidateCacheBuffer ? m_smokeReGIRState.candidateCacheBuffer : m_smokeLightCandidateBuffer;
        const nvrhi::BufferHandle pdfNeeNeeCacheProviderSrv =
            m_smokeNeeCacheState.providerResultBuffer ? m_smokeNeeCacheState.providerResultBuffer : m_smokeLightCandidateBuffer;
        const nvrhi::BufferHandle pdfNeeNeeCacheCellSrv =
            m_smokeNeeCacheState.cellBuffer ? m_smokeNeeCacheState.cellBuffer : m_smokeLightCandidateBuffer;
        const nvrhi::BufferHandle pdfNeeNeeCacheCandidateSrv =
            m_smokeNeeCacheState.candidateBuffer ? m_smokeNeeCacheState.candidateBuffer : m_smokeLightCandidateBuffer;
        nvrhi::BindingSetDesc pdfNeeBindingSetDesc;
        pdfNeeBindingSetDesc.bindings = {
            nvrhi::BindingSetItem::RayTracingAccelStruct(0, m_smokeTlas),
            nvrhi::BindingSetItem::Texture_UAV(1, m_frameResources.outputTexture),
            nvrhi::BindingSetItem::ConstantBuffer(2, m_smokeConstantsBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(3, dispatchStaticVertexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(4, dispatchStaticIndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(5, dispatchStaticTriangleClassBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(6, m_smokeDynamicVertexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(7, m_smokeDynamicIndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(8, m_smokeDynamicTriangleClassBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(9, dispatchStaticTriangleMaterialBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(10, m_smokeDynamicTriangleMaterialBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(11, dispatchStaticTriangleMaterialIndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(12, m_smokeDynamicTriangleMaterialIndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(13, m_smokeMaterialTableBuffer),
            nvrhi::BindingSetItem::Texture_SRV(14, pdfNeeFallbackTexture),
            nvrhi::BindingSetItem::Texture_UAV(15, m_frameResources.accumulationTexture),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(16, m_smokeEmissiveTriangleBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(57, m_smokePreviousEmissiveTriangleBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(58, m_smokeEmissiveRemapBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(46, m_smokeEmissiveDistributionBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(17, m_smokeLightCandidateBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(21, m_smokeBoundsOverlayLineBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(22, m_smokeRigidRouteVertexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(23, m_smokeRigidRouteIndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(24, m_smokeRigidRouteTriangleMaterialBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(25, m_smokeRigidRouteTriangleMaterialIndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(26, m_smokeRigidRouteInstanceBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(18, m_smokeSkinnedHitRouteRecordBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(19, m_smokeSkinnedHitRouteTriangleBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(
                28,
                m_sceneInputs.geometry.skinnedSourceIndexBuffer
                    ? m_sceneInputs.geometry.skinnedSourceIndexBuffer
                    : m_smokeDynamicIndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(
                29,
                m_smokeSkinnedCurrentOutputVertexBuffer
                    ? m_smokeSkinnedCurrentOutputVertexBuffer
                    : m_smokeDynamicVertexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(27, m_smokeDoomAnalyticLightBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(45, m_smokeDoomAnalyticPreviousLightBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(30, m_frameResources.primarySurfaceHistoryBuffers.current),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(31, m_frameResources.primarySurfaceHistoryBuffers.previous),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(32, m_smokeSkinnedPreviousPositionBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(33, m_smokeSkinnedSurfaceDispatchBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(34, dispatchPreviousStaticVertexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(35, dispatchPreviousStaticIndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(36, dispatchPreviousStaticTriangleClassBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(37, dispatchPreviousStaticTriangleMaterialBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(38, dispatchPreviousStaticTriangleMaterialIndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(42, m_smokeDoomAnalyticCurrentIdentityBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(43, m_smokeDoomAnalyticPreviousIdentityBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(44, m_smokeDoomAnalyticRemapBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(59, m_smokeUnifiedLightBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(60, m_smokeUnifiedPreviousLightBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(61, m_smokeUnifiedLightRemapBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(64, m_smokeRestirLightManagerCurrentToPreviousBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(65, m_smokeRestirLightManagerPreviousToCurrentBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(66, m_smokeRestirLightManagerCurrentPayloadBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(67, m_smokeRestirLightManagerPreviousPayloadBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(74, pdfNeeNeeCacheProviderSrv),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(75, pdfNeeNeeCacheCellSrv),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(77, pdfNeeNeeCacheCandidateSrv),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(41, m_smokeSkinnedTriangleDispatchIndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(76, m_smokeDynamicMaterialBuffer ? m_smokeDynamicMaterialBuffer : m_smokeLightCandidateBuffer),
            nvrhi::BindingSetItem::Texture_UAV(39, m_frameResources.motionVectorTexture),
            nvrhi::BindingSetItem::Texture_UAV(40, m_frameResources.motionVectorMaskTexture),
            nvrhi::BindingSetItem::Texture_UAV(47, m_frameResources.restirPTReflectionTexture),
            nvrhi::BindingSetItem::Texture_UAV(48, m_frameResources.rrGuideAlbedoTexture),
            nvrhi::BindingSetItem::Texture_UAV(49, m_frameResources.rrGuideNormalRoughnessTexture),
            nvrhi::BindingSetItem::Texture_UAV(50, m_frameResources.rrGuideDepthTexture),
            nvrhi::BindingSetItem::Texture_UAV(51, m_frameResources.rrGuideHitDistanceTexture),
            nvrhi::BindingSetItem::Texture_UAV(52, m_frameResources.rrGuideResetMaskTexture),
            nvrhi::BindingSetItem::Texture_UAV(53, m_frameResources.rrGuideSpecularAlbedoTexture),
            nvrhi::BindingSetItem::Texture_UAV(54, m_frameResources.rrInputColorTexture),
            nvrhi::BindingSetItem::Texture_UAV(78, m_frameResources.rrMotionVectorTexture),
            nvrhi::BindingSetItem::Texture_UAV(79, m_frameResources.rrGuidePositionTexture),
            nvrhi::BindingSetItem::Sampler(0, m_backend->GetCommonPasses().m_AnisotropicWrapSampler),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(69, m_smokeCleanRtxdiDiCurrentReservoirBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(70, m_smokeCleanRtxdiDiTemporalReservoirBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(71, m_smokeCleanRtxdiDiPreviousReservoirBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(73, pdfNeeReGIRCandidateSrv)
        };
        pdfNeeVerifierBindingSet = device->createBindingSet(pdfNeeBindingSetDesc, m_smokePdfNeeVerifierBindingLayout);
        if (!pdfNeeVerifierBindingSet)
        {
            return;
        }
    }

    int debugMode = standaloneDebugRouteRequested ? 0 : NormalizePathTraceDebugMode(idMath::ClampInt(0, 58, r_pathTracingDebugMode.GetInteger()));
    m_frameResources.settings.debugMode = debugMode;
    if (PathTraceDebugModeNeedsTextureTable(debugMode) && r_pathTracingTextureTableLimit.GetInteger() <= 0)
    {
        debugMode = 7;
    }
    const uint32_t safetyDisableMask = BuildPathTraceSafetyDisableMask();
    const bool disableSelectedLightLoop = PathTraceSafetyDisabled(safetyDisableMask, RT_PT_SAFETY_DISABLE_SELECTED_LIGHT_LOOP);
    const bool disableAnalyticLightLoop = PathTraceSafetyDisabled(safetyDisableMask, RT_PT_SAFETY_DISABLE_ANALYTIC_LIGHT_LOOP);
    const bool disableEmissiveTriangleSampling = PathTraceSafetyDisabled(safetyDisableMask, RT_PT_SAFETY_DISABLE_EMISSIVE_TRIANGLE_SAMPLING);
    const bool disablePrimarySurfaceHistory = PathTraceSafetyDisabled(safetyDisableMask, RT_PT_SAFETY_DISABLE_PRIMARY_SURFACE_HISTORY);
    const bool motionVectorExportEnabled = r_pathTracingMotionVectorExport.GetInteger() != 0;
    nvrhi::rt::State state;
    if (neeCacheDebugRouteRequested && m_smokeNeeCacheDebugShaderTable)
    {
        state.shaderTable = m_smokeNeeCacheDebugShaderTable;
    }
    else if (regirDebugRouteRequested && m_smokeReGIRDebugShaderTable)
    {
        state.shaderTable = m_smokeReGIRDebugShaderTable;
    }
    else if (pdfNeeRluCurrentProducerRequested && m_smokeRestirPdfNeeRluCurrentShaderTable)
    {
        state.shaderTable = m_smokeRestirPdfNeeRluCurrentShaderTable;
    }
    else
    {
        state.shaderTable = m_smokeShaderTable;
    }
    nvrhi::BindingSetHandle neeCacheSecondaryBindingSet;
    if (neeCacheSecondaryConsumeReady &&
        !neeCacheDebugRouteRequested &&
        m_smokeBindingSet &&
        m_smokeBindingLayout &&
        regirDevice)
    {
        nvrhi::BindingSetDesc neeCacheSecondaryBindingSetDesc = *m_smokeBindingSet->getDesc();
        ReplaceStructuredBufferSrv(neeCacheSecondaryBindingSetDesc, 74u, m_smokeNeeCacheState.providerResultBuffer);
        ReplaceStructuredBufferSrv(neeCacheSecondaryBindingSetDesc, 75u, m_smokeNeeCacheState.cellBuffer);
        ReplaceStructuredBufferSrv(neeCacheSecondaryBindingSetDesc, 77u, m_smokeNeeCacheState.candidateBuffer);
        neeCacheSecondaryBindingSet = regirDevice->createBindingSet(neeCacheSecondaryBindingSetDesc, m_smokeBindingLayout);
    }
    nvrhi::BindingSetHandle activeBindingSet =
        neeCacheDebugRouteRequested && neeCacheDebugBindingSet
            ? neeCacheDebugBindingSet
            : (regirDebugRouteRequested && regirDebugBindingSet
            ? regirDebugBindingSet
            : (pdfNeeRluCurrentProducerRequested && pdfNeeVerifierBindingSet
             ? pdfNeeVerifierBindingSet
             : (neeCacheSecondaryBindingSet ? neeCacheSecondaryBindingSet : m_smokeBindingSet)));
    state.bindings = { activeBindingSet, m_smokeTextureDescriptorTable };
    const PathTraceIntegratorSettings integratorSettings = ApplyPathTraceSafetyKillSwitches(BuildPathTraceIntegratorSettings(), safetyDisableMask);
    const RtPathTraceDebugModeInfo debugModeInfo = GetPathTraceDebugModeInfo(debugMode);

    idVec3 cameraOrigin = viewDef->renderView.vieworg;
    idVec3 cameraForward = viewDef->renderView.viewaxis[0];
    idVec3 cameraLeft = viewDef->renderView.viewaxis[1];
    idVec3 cameraUp = viewDef->renderView.viewaxis[2];
    cameraForward.Normalize();
    cameraLeft.Normalize();
    cameraUp.Normalize();

    const int requestedLightCount = idMath::ClampInt(0, RT_SMOKE_MAX_DEBUG_LIGHTS, r_pathTracingLightCount.GetInteger());
    const int toyLightTraceCap = idMath::ClampInt(0, RT_SMOKE_MAX_DEBUG_LIGHTS, r_pathTracingToyLightTraceCap.GetInteger());
    const int selectedLightRequestCount = debugMode == 18 ? Min(requestedLightCount, toyLightTraceCap) : requestedLightCount;
    const int lightSelectionMode = idMath::ClampInt(0, 1, r_pathTracingLightSelection.GetInteger());
    const float toyLightScale = idMath::ClampFloat(0.0f, 16.0f, r_pathTracingToyLightScale.GetFloat());
    const float toyEmissiveScale = idMath::ClampFloat(0.0f, 32.0f, r_pathTracingToyEmissiveScale.GetFloat());
    const float analyticLightIntensityScale = idMath::ClampFloat(0.0f, 16.0f, r_pathTracingAnalyticLightIntensityScale.GetFloat());
    const float effectiveAnalyticLightIntensityScale = analyticLightIntensityScale;
    float toyMaxRayDistance = idMath::ClampFloat(64.0f, 100000.0f, r_pathTracingToyMaxRayDistance.GetFloat());
    if (r_pathTracingSceneSource.GetInteger() == 2)
    {
        toyMaxRayDistance = 100000.0f;
    }

    uint64 accumulationSignature = 1469598103934665603ull;
    accumulationSignature = HashSmokeBytes(accumulationSignature, &debugMode, sizeof(debugMode));
    accumulationSignature = HashSmokeBytes(accumulationSignature, &m_frameResources.width, sizeof(m_frameResources.width));
    accumulationSignature = HashSmokeBytes(accumulationSignature, &m_frameResources.height, sizeof(m_frameResources.height));
    accumulationSignature = HashSmokeFloatQuantized(accumulationSignature, cameraOrigin.x, 100.0f);
    accumulationSignature = HashSmokeFloatQuantized(accumulationSignature, cameraOrigin.y, 100.0f);
    accumulationSignature = HashSmokeFloatQuantized(accumulationSignature, cameraOrigin.z, 100.0f);
    accumulationSignature = HashSmokeFloatQuantized(accumulationSignature, cameraForward.x, 10000.0f);
    accumulationSignature = HashSmokeFloatQuantized(accumulationSignature, cameraForward.y, 10000.0f);
    accumulationSignature = HashSmokeFloatQuantized(accumulationSignature, cameraForward.z, 10000.0f);
    accumulationSignature = HashSmokeFloatQuantized(accumulationSignature, cameraLeft.x, 10000.0f);
    accumulationSignature = HashSmokeFloatQuantized(accumulationSignature, cameraLeft.y, 10000.0f);
    accumulationSignature = HashSmokeFloatQuantized(accumulationSignature, cameraLeft.z, 10000.0f);
    accumulationSignature = HashSmokeFloatQuantized(accumulationSignature, cameraUp.x, 10000.0f);
    accumulationSignature = HashSmokeFloatQuantized(accumulationSignature, cameraUp.y, 10000.0f);
    accumulationSignature = HashSmokeFloatQuantized(accumulationSignature, cameraUp.z, 10000.0f);
    accumulationSignature = HashSmokeFloatQuantized(accumulationSignature, viewDef->renderView.fov_x, 100.0f);
    accumulationSignature = HashSmokeFloatQuantized(accumulationSignature, viewDef->renderView.fov_y, 100.0f);
    accumulationSignature = HashSmokeFloatQuantized(accumulationSignature, r_forceAmbient.GetFloat(), 1000.0f);
    accumulationSignature = HashSmokeFloatQuantized(accumulationSignature, toyLightScale, 1000.0f);
    accumulationSignature = HashSmokeFloatQuantized(accumulationSignature, toyEmissiveScale, 1000.0f);
    accumulationSignature = HashSmokeFloatQuantized(accumulationSignature, analyticLightIntensityScale, 1000.0f);
    accumulationSignature = HashSmokeFloatQuantized(accumulationSignature, effectiveAnalyticLightIntensityScale, 1000.0f);
    accumulationSignature = HashSmokeFloatQuantized(accumulationSignature, toyMaxRayDistance, 10.0f);
    accumulationSignature = HashSmokeDispatchValue(accumulationSignature, static_cast<uint64>(m_smokeDoomAnalyticLightCount));
    accumulationSignature = HashSmokeDispatchValue(accumulationSignature, static_cast<uint64>(idMath::ClampInt(0, 1024, r_pathTracingAnalyticLightMaxGpu.GetInteger())));
    accumulationSignature = HashSmokeDispatchValue(
        accumulationSignature,
        static_cast<uint64>(idMath::ClampInt(1, 16, r_pathTracingReservoirCandidateTrials.GetInteger())));
    accumulationSignature = HashSmokeDispatchValue(accumulationSignature, static_cast<uint64>(requestedLightCount));
    accumulationSignature = HashSmokeDispatchValue(
        accumulationSignature,
        static_cast<uint64>(idMath::ClampInt(0, RT_SMOKE_MAX_DEBUG_LIGHTS, r_pathTracingToyLightTraceCap.GetInteger())));
    accumulationSignature = HashSmokeDispatchValue(accumulationSignature, static_cast<uint64>(lightSelectionMode));
    accumulationSignature = HashSmokeDispatchValue(accumulationSignature, static_cast<uint64>(r_pathTracingToyFakePBRSpecular.GetInteger() != 0 ? 1 : 0));
    accumulationSignature = HashSmokeDispatchValue(accumulationSignature, static_cast<uint64>(idMath::ClampInt(0, 4, r_pathTracingOpenPbrBrdfMode.GetInteger())));
    accumulationSignature = HashSmokeDispatchValue(accumulationSignature, static_cast<uint64>(r_pathTracingToyAccumulation.GetInteger() != 0 ? 1 : 0));
    accumulationSignature = HashSmokeDispatchValue(accumulationSignature, static_cast<uint64>(safetyDisableMask));
    accumulationSignature = HashPathTraceIntegratorSettings(accumulationSignature, integratorSettings);
    if (debugMode != 18 || r_pathTracingToyAccumulation.GetInteger() == 0 || accumulationSignature != m_frameResources.smokeAccumulationSignature)
    {
        m_frameResources.smokeAccumulationSignature = accumulationSignature;
        m_frameResources.smokeAccumulationFrameCount = 0;
    }
    const int accumulationMaxFrames = idMath::ClampInt(1, 4096, r_pathTracingToyAccumMaxFrames.GetInteger());
    const bool mode18AccumulationActive = debugMode == 18 && r_pathTracingToyAccumulation.GetInteger() != 0;
    const int accumulationFrameCount = mode18AccumulationActive
        ? Min(m_frameResources.smokeAccumulationFrameCount, accumulationMaxFrames - 1)
        : 0;
    const bool accumulationTextureActive = mode18AccumulationActive;

    const uint64 setupCompleteUs = Sys_Microseconds();
    const uint32_t restirPTFrameIndex = m_frameResources.restirPTFrameIndex++;
    m_frameResources.settings.frameIndex = restirPTFrameIndex;

    PathTraceSmokeConstants constants = {};
    constants.cameraOriginAndTMax[0] = cameraOrigin.x;
    constants.cameraOriginAndTMax[1] = cameraOrigin.y;
    constants.cameraOriginAndTMax[2] = cameraOrigin.z;
    constants.cameraOriginAndTMax[3] = 100000.0f;
    constants.cameraForwardAndTanX[0] = cameraForward.x;
    constants.cameraForwardAndTanX[1] = cameraForward.y;
    constants.cameraForwardAndTanX[2] = cameraForward.z;
    constants.cameraForwardAndTanX[3] = idMath::Tan(DEG2RAD(viewDef->renderView.fov_x * 0.5f));
    constants.cameraLeftAndTanY[0] = cameraLeft.x;
    constants.cameraLeftAndTanY[1] = cameraLeft.y;
    constants.cameraLeftAndTanY[2] = cameraLeft.z;
    constants.cameraLeftAndTanY[3] = idMath::Tan(DEG2RAD(viewDef->renderView.fov_y * 0.5f));
    constants.cameraUpAndDebugMode[0] = cameraUp.x;
    constants.cameraUpAndDebugMode[1] = cameraUp.y;
    constants.cameraUpAndDebugMode[2] = cameraUp.z;
    constants.cameraUpAndDebugMode[3] = static_cast<float>(debugMode);
    constants.textureInfo[0] = static_cast<float>(Max(0, static_cast<int>(m_smokeActiveTextureTable.size()) - 1));
    const int textureSampleMethod = r_pathTracingTextureSampleEnable.GetInteger() != 0
        ? idMath::ClampInt(0, 2, r_pathTracingTextureSampleMethod.GetInteger())
        : 0;
    constants.textureInfo[1] = static_cast<float>(textureSampleMethod);
    constants.textureInfo[2] = static_cast<float>(Max(0, m_smokeMaterialTableEntryCount));
    const bool integratorUsesSpecular = integratorSettings.reflectionMode > 0 || r_pathTracingToyFakePBRSpecular.GetInteger() != 0;
    const bool toyFakePBRSpecularEnabled = r_pathTracingToyFakePBRSpecular.GetInteger() != 0 && debugMode == 18;
    const uint32_t textureFlags =
        (r_pathTracingTextureBindlessEnable.GetInteger() != 0 ? 1u : 0u) |
        (r_pathTracingTextureFilter.GetInteger() != 0 ? 2u : 0u) |
        (r_pathTracingTextureDecode.GetInteger() != 0 ? 4u : 0u) |
        (r_pathTracingUseNormalMaps.GetInteger() != 0 && (debugMode == 14 || debugMode == 18) ? 8u : 0u) |
        (r_pathTracingUseSpecularMaps.GetInteger() != 0 && (debugMode == 14 || debugMode == 57 || (integratorUsesSpecular && debugMode == 18)) ? 16u : 0u) |
        (r_pathTracingUseEmissiveMaps.GetInteger() != 0 && (debugMode == 14 || debugMode == 18 || cleanRtxdiDiRouteRequested) ? 32u : 0u) |
        (r_pathTracingReservoirTwoSidedEmissives.GetInteger() != 0 && (debugMode == 18 || cleanRtxdiDiRouteRequested) ? 64u : 0u) |
        (toyFakePBRSpecularEnabled ? 128u : 0u) |
        PackPathTraceOpenPbrBrdfMode();
    constants.textureInfo[3] = static_cast<float>(textureFlags);
    const bool previousHistoryViewValid =
        !disablePrimarySurfaceHistory &&
        m_frameResources.primarySurfaceHistoryView.valid &&
        m_frameResources.primarySurfaceHistoryView.width == m_frameResources.width &&
        m_frameResources.primarySurfaceHistoryView.height == m_frameResources.height &&
        !m_frameResources.primarySurfaceHistoryNeedsClear;
    constants.prevCameraOriginAndValid[0] = m_frameResources.primarySurfaceHistoryView.origin.x;
    constants.prevCameraOriginAndValid[1] = m_frameResources.primarySurfaceHistoryView.origin.y;
    constants.prevCameraOriginAndValid[2] = m_frameResources.primarySurfaceHistoryView.origin.z;
    constants.prevCameraOriginAndValid[3] = previousHistoryViewValid ? 1.0f : 0.0f;
    constants.prevCameraForwardAndTanX[0] = m_frameResources.primarySurfaceHistoryView.forward.x;
    constants.prevCameraForwardAndTanX[1] = m_frameResources.primarySurfaceHistoryView.forward.y;
    constants.prevCameraForwardAndTanX[2] = m_frameResources.primarySurfaceHistoryView.forward.z;
    constants.prevCameraForwardAndTanX[3] = m_frameResources.primarySurfaceHistoryView.tanX;
    constants.prevCameraLeftAndTanY[0] = m_frameResources.primarySurfaceHistoryView.left.x;
    constants.prevCameraLeftAndTanY[1] = m_frameResources.primarySurfaceHistoryView.left.y;
    constants.prevCameraLeftAndTanY[2] = m_frameResources.primarySurfaceHistoryView.left.z;
    constants.prevCameraLeftAndTanY[3] = m_frameResources.primarySurfaceHistoryView.tanY;
    constants.prevCameraUpAndTanY[0] = m_frameResources.primarySurfaceHistoryView.up.x;
    constants.prevCameraUpAndTanY[1] = m_frameResources.primarySurfaceHistoryView.up.y;
    constants.prevCameraUpAndTanY[2] = m_frameResources.primarySurfaceHistoryView.up.z;
    constants.prevCameraUpAndTanY[3] = m_frameResources.primarySurfaceHistoryView.tanY;
    RtSmokeSelectedLight selectedLights[RT_SMOKE_MAX_DEBUG_LIGHTS];
    const bool enableDoomAnalyticLights = !disableAnalyticLightLoop && r_pathTracingAnalyticLightCandidates.GetInteger() != 0;
    const bool replaceSelectedLightsWithAnalytic = enableDoomAnalyticLights && r_pathTracingAnalyticLightReplaceSelected.GetInteger() != 0;
    const int selectedLightCount = !disableSelectedLightLoop && (debugMode == 14 || debugMode == 15 || debugMode == 18) && !replaceSelectedLightsWithAnalytic
        ? CollectSelectedSmokePointLights(viewDef, cameraOrigin, selectedLights, selectedLightRequestCount, lightSelectionMode)
        : 0;
    const int analyticLightTraceCount = enableDoomAnalyticLights ? Min(m_smokeDoomAnalyticLightCount, idMath::ClampInt(0, 1024, r_pathTracingAnalyticLightMaxGpu.GetInteger())) : 0;
    const int estimatedRaysPerPixel = EstimatePathTraceRaysPerPixel(integratorSettings, selectedLightCount, analyticLightTraceCount);
    const PathTraceDispatchTileSettings dispatchTileSettings = BuildPathTraceDispatchTileSettings(m_frameResources.width, m_frameResources.height, estimatedRaysPerPixel);
    constants.lightInfo[0] = static_cast<float>(selectedLightCount);
    constants.lightInfo[1] = static_cast<float>(lightSelectionMode);
    constants.lightInfo[2] = idMath::ClampFloat(0.0f, 1.0f, r_pathTracingSmokeParticleAlphaScale.GetFloat());
    constants.lightInfo[3] =
        (r_pathTracingSmokeParticleDither.GetInteger() != 0 ? 1.0f : 0.0f) +
        (r_pathTracingSmokeParticleEdgeFade.GetInteger() != 0 ? 2.0f : 0.0f);
    constants.portalWindowInfo[0] = r_pathTracingPortalWindowStochastic.GetInteger() != 0 ? 1.0f : 0.0f;
    constants.portalWindowInfo[1] = idMath::ClampFloat(0.0f, 1.0f, r_pathTracingPortalWindowAlphaScale.GetFloat());
    constants.portalWindowInfo[2] = idMath::ClampFloat(0.0f, 1.0f, r_pathTracingPortalWindowMinOpacity.GetFloat());
    constants.portalWindowInfo[3] = idMath::ClampFloat(0.0f, 1.0f, r_pathTracingPortalWindowShadowOpacity.GetFloat());
    constants.lightSpriteInfo[0] = r_pathTracingLightSpriteProxies.GetInteger() != 0 ? 1.0f : 0.0f;
    constants.lightSpriteInfo[1] = idMath::ClampFloat(0.001f, 0.25f, r_pathTracingLightSpriteRadiusScale.GetFloat());
    constants.lightSpriteInfo[2] = idMath::ClampFloat(0.0f, 16.0f, r_pathTracingLightSpriteIntensity.GetFloat());
    constants.lightSpriteInfo[3] = idMath::ClampFloat(0.0f, 1.0f, r_forceAmbient.GetFloat());
    constants.toyPathInfo[0] = toyMaxRayDistance;
    constants.toyPathInfo[1] = toyLightScale;
    constants.toyPathInfo[2] = toyEmissiveScale;
    constants.toyPathInfo[3] = static_cast<float>(accumulationFrameCount);
    constants.emissiveInfo[0] = static_cast<float>(disableEmissiveTriangleSampling ? 0 : m_smokeEmissiveTriangleCount);
    constants.emissiveInfo[1] = static_cast<float>(disableEmissiveTriangleSampling ? 0 : m_smokeEmissiveStaticTriangleCount);
    constants.emissiveInfo[2] = static_cast<float>(idMath::ClampInt(1, 16, r_pathTracingReservoirCandidateTrials.GetInteger()));
    constants.emissiveInfo[3] = static_cast<float>(disableEmissiveTriangleSampling ? 0 : m_smokeLightCandidateCount);
    const int emissiveDistributionCount = !disableEmissiveTriangleSampling && r_pathTracingEmissiveDistribution.GetInteger() != 0 && m_sceneInputs.lights.emissiveDistributionValid
        ? m_sceneInputs.lights.emissiveDistributionCount
        : 0;
    constants.emissiveDistributionInfo[0] = static_cast<float>(Max(0, emissiveDistributionCount));
    constants.emissiveDistributionInfo[1] = emissiveDistributionCount > 0 ? 1.0f : 0.0f;
    constants.emissiveDistributionInfo[2] = static_cast<float>(Max(0, m_sceneInputs.lights.emissiveDistributionFallbackIndex));
    constants.emissiveDistributionInfo[3] = static_cast<float>(
        m_sceneInputs.materials.materialTableGpuStable ? Max(0, m_sceneInputs.materials.dynamicMaterialRecordCount) : 0);
    const int requestedLiquidPoolMode = idMath::ClampInt(0, 3, r_pathTracingLiquidPoolMode.GetInteger());
    const bool liquidPoolTelemetryReady = m_liquidPoolStatusBuffer && m_liquidPoolStatusReadbackBuffer;
    const int effectiveLiquidPoolMode = 0;
    PopulatePathTraceDecalAndLiquidPoolControls(
        constants,
        effectiveLiquidPoolMode,
        liquidPoolTelemetryReady,
        Max(0, m_sceneInputs.materials.dynamicMaterialRecordCount),
        m_sceneInputs.materials.materialTableGpuStable ? Max(0, m_sceneInputs.materials.dynamicMaterialRecordCount) : 0);
    if (requestedLiquidPoolMode != 0 && !liquidPoolTelemetryReady)
    {
        static bool liquidPoolTelemetryWarningPrinted = false;
        if (!liquidPoolTelemetryWarningPrinted)
        {
            liquidPoolTelemetryWarningPrinted = true;
            common->Printf("PathTracePrimaryPass: liquid-pool mode requested but status telemetry is unavailable; effective mode is 0\n");
        }
    }
    const bool enableGpuBoundsOverlay = r_pathTracingSceneBoundsOverlayGpu.GetInteger() != 0;
    const bool enableBoundsBoxDebugMode = IsPathTraceBoundsOverlayDebugMode(debugMode);
    const int gpuBoundsOverlayLineCount = (enableGpuBoundsOverlay || enableBoundsBoxDebugMode) ? idMath::ClampInt(0, RT_PT_BOUNDS_OVERLAY_MAX_LINES, m_smokeBoundsOverlayLineCount) : 0;
    constants.boundsOverlayInfo[0] = static_cast<float>(gpuBoundsOverlayLineCount);
    constants.boundsOverlayInfo[1] = 1.35f;
    constants.boundsOverlayInfo[2] = enableGpuBoundsOverlay ? 1.0f : 0.0f;
    constants.boundsOverlayInfo[3] = 0.0f;
    constants.doomAnalyticLightInfo[0] = static_cast<float>(disableAnalyticLightLoop ? 0 : m_smokeDoomAnalyticLightCount);
    constants.doomAnalyticLightInfo[1] = static_cast<float>(idMath::ClampInt(0, 1024, r_pathTracingAnalyticLightMaxGpu.GetInteger()));
    constants.doomAnalyticLightInfo[2] = effectiveAnalyticLightIntensityScale;
    constants.doomAnalyticLightInfo[3] =
        (enableDoomAnalyticLights ? 1.0f : 0.0f) +
        (replaceSelectedLightsWithAnalytic ? 2.0f : 0.0f) +
        (r_pathTracingAnalyticLightDoomRadiusCutoff.GetBool() ? 4.0f : 0.0f);
    constants.doomAnalyticLightRemapInfo[0] = static_cast<float>(m_smokeDoomAnalyticCurrentIdentityCount);
    constants.doomAnalyticLightRemapInfo[1] = static_cast<float>(m_smokeDoomAnalyticPreviousIdentityCount);
    constants.doomAnalyticLightRemapInfo[2] = static_cast<float>(m_smokeDoomAnalyticRemapCount);
    constants.doomAnalyticLightRemapInfo[3] = static_cast<float>(m_smokePreviousEmissiveTriangleCount);
    const PathTraceRemixLightManagerStats remixLightManagerStats = m_remixLightManager.GetStats();
    const bool useRemixLightManagerRabSource = remixLightManagerStats.enabled != 0u;
    constants.restirLightManagerInfo[0] = static_cast<float>(useRemixLightManagerRabSource ? remixLightManagerStats.currentLightCount : 0u);
    constants.restirLightManagerInfo[1] = static_cast<float>(useRemixLightManagerRabSource ? remixLightManagerStats.previousLightCount : 0u);
    constants.restirLightManagerInfo[2] = static_cast<float>(useRemixLightManagerRabSource ? remixLightManagerStats.currentToPreviousCount : 0u);
    constants.restirLightManagerInfo[3] = static_cast<float>(useRemixLightManagerRabSource ? remixLightManagerStats.previousToCurrentCount : 0u);
    constants.restirLightManagerControlInfo[0] = useRemixLightManagerRabSource ? 1.0f : 0.0f;
    constants.restirLightManagerControlInfo[1] = useRemixLightManagerRabSource ? 2.0f : 0.0f;
    constants.restirLightManagerControlInfo[2] = static_cast<float>(useRemixLightManagerRabSource
        ? remixLightManagerStats.doomAnalyticStableCacheableCount
        : 0u);
    constants.restirLightManagerControlInfo[3] = static_cast<float>(useRemixLightManagerRabSource
        ? remixLightManagerStats.doomAnalyticUnstableDynamicCount
        : 0u);
    const uint32_t emissiveRangeOffset = useRemixLightManagerRabSource ? remixLightManagerStats.emissiveRangeOffset : 0u;
    const uint32_t emissiveRangeCount = useRemixLightManagerRabSource ? remixLightManagerStats.emissiveRangeCount : 0u;
    const uint32_t doomAnalyticRangeOffset = useRemixLightManagerRabSource ? remixLightManagerStats.doomAnalyticRangeOffset : 0u;
    const uint32_t doomAnalyticRangeCount = useRemixLightManagerRabSource ? remixLightManagerStats.doomAnalyticRangeCount : 0u;
    constants.restirLightManagerRangeInfo[0] = static_cast<float>(emissiveRangeOffset);
    constants.restirLightManagerRangeInfo[1] = static_cast<float>(emissiveRangeCount);
    constants.restirLightManagerRangeInfo[2] = static_cast<float>(doomAnalyticRangeOffset);
    constants.restirLightManagerRangeInfo[3] = static_cast<float>(doomAnalyticRangeCount);
    constants.restirLightManagerSampleInfo[0] = static_cast<float>(useRemixLightManagerRabSource ? remixLightManagerStats.emissiveSampleCount : 0u);
    constants.restirLightManagerSampleInfo[1] = static_cast<float>(useRemixLightManagerRabSource ? remixLightManagerStats.doomAnalyticSampleCount : 0u);
    constants.restirLightManagerSampleInfo[2] = static_cast<float>(useRemixLightManagerRabSource ? remixLightManagerStats.totalSampleCount : 0u);
    constants.restirLightManagerSampleInfo[3] = static_cast<float>(useRemixLightManagerRabSource ? remixLightManagerStats.nonEmptyRangeCount : 0u);
    constants.restirPTInfo[0] = static_cast<float>(restirPTFrameIndex);
    constants.restirPTInfo[1] = r_pathTracingNormalMapFlipGreen.GetInteger() != 0 ? 1.0f : 0.0f;
    constants.restirPTInfo[2] = 0.0f;
    constants.integratorInfo[0] = static_cast<float>(integratorSettings.samplesPerPixel);
    constants.integratorInfo[1] = static_cast<float>(integratorSettings.maxPathDepth);
    constants.integratorInfo[2] = static_cast<float>(integratorSettings.diffuseBounceLimit);
    constants.integratorInfo[3] = static_cast<float>(integratorSettings.specularBounceLimit);
    constants.integratorInfo2[0] = static_cast<float>(integratorSettings.transmissionBounceLimit);
    constants.integratorInfo2[1] = static_cast<float>(integratorSettings.reflectionMode);
    constants.integratorInfo2[2] = static_cast<float>(integratorSettings.russianRouletteDepth);
    constants.integratorInfo2[3] = static_cast<float>(integratorSettings.nextEventEstimation);
    constants.neeInfo[0] = static_cast<float>(integratorSettings.secondaryNeeMode);
    constants.neeInfo[1] = static_cast<float>(integratorSettings.secondaryNeeVisibility);
    constants.neeInfo[2] = static_cast<float>(integratorSettings.secondaryAnalyticNeeMode);
    constants.neeInfo[3] = static_cast<float>(integratorSettings.secondaryAnalyticNeeSamples);
    constants.motionVectorInfo[0] = motionVectorExportEnabled ? 1.0f : 0.0f;
    constants.motionVectorInfo[1] = r_pathTracingRestirPTTemporalAnalyticNeeReuse.GetInteger() != 0 ? 1.0f : 0.0f;
    constants.motionVectorInfo[2] = static_cast<float>(idMath::ClampInt(1, 128, r_pathTracingRestirPTAnalyticLightTrials.GetInteger()));
    constants.motionVectorInfo[3] = idMath::ClampFloat(0.0f, 1.0f, r_pathTracingRestirPTTemporalAnalyticLightChangeTolerance.GetFloat());
    constants.restirPTSurfaceInfo[0] = static_cast<float>(idMath::ClampInt(0, 5, r_pathTracingRestirPTMaterialSimilarityMode.GetInteger()));
    constants.restirPTSurfaceInfo[2] = r_pathTracingRestirPTUnifiedPrevToCurrentScan.GetBool() ? 1.0f : 0.0f;
    constants.restirPTSurfaceInfo[3] = r_pathTracingMotionVectorDisableRigid.GetBool() ? 1.0f : 0.0f;
    constants.rayReconstructionInfo[0] = static_cast<float>(idMath::ClampInt(0, 10, r_pathTracingDLSSRRGuideDebugView.GetInteger()));
    constants.rayReconstructionInfo[1] = 0.0f;
    constants.rayReconstructionInfo[2] = 0.0f;
    constants.rayReconstructionInfo[3] = 0.0f;
    constants.unifiedLightInfo[0] = static_cast<float>(Max(0, m_smokeUnifiedLightCount));
    constants.unifiedLightInfo[1] = static_cast<float>(Max(0, m_smokeUnifiedPreviousLightCount));
    constants.unifiedLightInfo[2] =
        (useRemixLightManagerRabSource || r_pathTracingRestirPTUnifiedLightLoad.GetInteger() != 0 ? 1.0f : 0.0f) +
        (useRemixLightManagerRabSource || r_pathTracingRestirPTUnifiedLightSample.GetInteger() != 0 ? 2.0f : 0.0f) +
        (r_pathTracingRestirPTUnifiedNee.GetInteger() != 0 ? 4.0f : 0.0f);
    constants.unifiedLightInfo[3] = static_cast<float>(Max(0, m_smokeUnifiedLightRemapCount));
    const int pdfNeeVerifierSamples = idMath::ClampInt(1, 64, r_pathTracingRestirPdfNeeVerifierSamples.GetInteger());
    const int pdfNeeVerifierVisibility = pdfNeeVerifierEntryVisibility;
    const int pdfNeeVerifierSourcePolicy = idMath::ClampInt(0, 2, r_pathTracingRestirPdfNeeVerifierSourcePolicy.GetInteger());
    constants.reservedRestirPdfNeeInfo[0] = 0.0f;
    constants.reservedRestirPdfNeeInfo[1] = 0.0f;
    constants.reservedRestirPdfNeeInfo[2] = 0.0f;
    constants.reservedRestirPdfNeeInfo[3] = 0.0f;
    constants.restirPdfNeeRluCurrentControlInfo[0] = static_cast<float>(pdfNeeVerifierSamples);
    constants.restirPdfNeeRluCurrentControlInfo[1] = static_cast<float>(pdfNeeVerifierVisibility);
    constants.restirPdfNeeRluCurrentControlInfo[2] = static_cast<float>(pdfNeeVerifierSourcePolicy);
    const bool pdfNeeNeeCacheProviderRequested = pdfNeeVerifierSourcePolicy == 2;
    const bool pdfNeeNeeCacheProviderReady =
        pdfNeeNeeCacheProviderRequested &&
        neeCacheResourceReady &&
        neeCacheCandidateBuildRequested &&
        m_smokeNeeCacheState.providerResultBuffer != nullptr &&
        m_smokeNeeCacheState.cellBuffer != nullptr &&
        m_smokeNeeCacheState.candidateBuffer != nullptr;
    constants.restirPdfNeeRluCurrentControlInfo[3] = pdfNeeNeeCacheProviderReady ? 1.0f : 0.0f;
    constants.regirInfo0[0] = regirSettings.enabled ? 1.0f : 0.0f;
    constants.regirInfo0[1] = static_cast<float>(regirSettings.debugView);
    constants.regirInfo0[2] = static_cast<float>(regirSettings.mode);
    constants.regirInfo0[3] = static_cast<float>(regirSettings.centerMode);
    constants.regirInfo1[0] = regirSettings.cellSize;
    constants.regirInfo1[1] = static_cast<float>(regirSettings.gridX);
    constants.regirInfo1[2] = static_cast<float>(regirSettings.gridY);
    constants.regirInfo1[3] = static_cast<float>(regirSettings.gridZ);
    constants.regirInfo2[0] = static_cast<float>(regirSettings.lightsPerCell);
    constants.regirInfo2[1] = static_cast<float>(regirSettings.buildSamples);
    constants.regirInfo2[2] = static_cast<float>(regirSettings.lightDomain);
    constants.regirInfo2[3] = static_cast<float>(regirDesc.cellCount);
    constants.regirInfo3[0] = regirDebugRouteRequested ? 2.0f : 0.0f;
    constants.regirInfo3[1] = static_cast<float>(regirDesc.slotCount);
    constants.regirInfo3[2] = 0.0f;
    constants.regirInfo3[3] = 0.0f;
    constants.regirInfo4[0] = regirResolvedCenter.x;
    constants.regirInfo4[1] = regirResolvedCenter.y;
    constants.regirInfo4[2] = regirResolvedCenter.z;
    constants.regirInfo4[3] = 1.0f;
    constants.neeCacheInfo0[0] = neeCacheSettings.enabled ? 1.0f : 0.0f;
    constants.neeCacheInfo0[1] = static_cast<float>(neeCacheSettings.debugView);
    constants.neeCacheInfo0[2] = static_cast<float>(neeCacheSettings.mode);
    constants.neeCacheInfo0[3] = static_cast<float>(neeCacheSettings.sourceDomain);
    constants.neeCacheInfo1[0] = static_cast<float>(neeCacheSettings.cellResolution);
    constants.neeCacheInfo1[1] = neeCacheSettings.minRange;
    constants.neeCacheInfo1[2] = static_cast<float>(neeCacheSettings.cellCount);
    constants.neeCacheInfo1[3] = static_cast<float>(neeCacheSettings.candidateSlots);
    constants.neeCacheInfo2[0] = static_cast<float>(neeCacheSettings.taskSlots);
    constants.neeCacheInfo2[1] = neeCacheSettings.fallbackProbability;
    constants.neeCacheInfo2[2] = static_cast<float>(neeCacheDesc.providerResultCount);
    constants.neeCacheInfo2[3] = static_cast<float>(neeCacheDesc.cellCount);
    constants.neeCacheInfo3[0] = neeCacheDebugRouteRequested ? 1.0f : 0.0f;
    constants.neeCacheInfo3[1] = neeCacheRluInputs.remixDenseDomain ? 1.0f : 0.0f;
    constants.neeCacheInfo3[2] = static_cast<float>(neeCacheRluInputs.currentLightCount);
    constants.neeCacheInfo3[3] = static_cast<float>(restirPTFrameIndex);
    constants.neeCacheConsumerInfo[0] = neeCacheSecondaryConsumeReady ? 1.0f : 0.0f;
    constants.neeCacheConsumerInfo[1] = neeCacheSecondaryConsumeRequested ? 1.0f : 0.0f;
    constants.neeCacheConsumerInfo[2] = integratorSettings.secondaryNeeVisibility != 0 ? 1.0f : 0.0f;
    constants.neeCacheConsumerInfo[3] = 0.0f;
    if (regirDebugRouteRequested && !regirUseCurrentRabLightUniverse)
    {
        // Candidate-cache views 4-10 must not fall back to local split-domain
        // light identities; cell-only views 1-3 do not consume light identity.
        constants.unifiedLightInfo[2] = 0.0f;
        constants.restirLightManagerControlInfo[0] = 0.0f;
    }
    constants.safetyInfo[0] = static_cast<float>(safetyDisableMask);
    constants.safetyInfo[1] = static_cast<float>(Max(0, static_cast<int>(m_smokeActiveTextureTable.size()) - 1));
    const int restirPTVisibilityPolicy = pdfNeeRluCurrentProducerRequested
        ? pdfNeeVerifierSelectedVisibilityPolicy
        : idMath::ClampInt(0, 2, r_pathTracingRestirPTVisibilityPolicy.GetInteger());
    constants.safetyInfo[2] = static_cast<float>(restirPTVisibilityPolicy * 16);
    constants.safetyInfo[3] = 0.0f;
    constants.geometryInfo0[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.staticVertexCount));
    constants.geometryInfo0[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.staticIndexCount));
    constants.geometryInfo0[2] = static_cast<float>(Max(0, m_sceneInputs.geometry.staticTriangleCount));
    constants.geometryInfo0[3] = static_cast<float>(Max(0, m_sceneInputs.geometry.dynamicVertexCount));
    constants.geometryInfo1[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.dynamicIndexCount));
    constants.geometryInfo1[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.dynamicTriangleCount));
    constants.geometryInfo1[2] = static_cast<float>(Max(0, m_sceneInputs.geometry.rigidRouteVertexCount));
    constants.geometryInfo1[3] = static_cast<float>(Max(0, m_sceneInputs.geometry.rigidRouteIndexCount));
    constants.staticBucketRouteInfo[0] =
        m_sceneInputs.geometry.staticBucketRouteFirstInstanceId;
    constants.staticBucketRouteInfo[1] =
        m_sceneInputs.geometry.staticBucketRoutePublicationValid
            ? m_sceneInputs.geometry.
                staticBucketTriangleCount
            : 0u;
    constants.staticBucketRouteInfo[2] =
        static_cast<uint32_t>(
            m_sceneInputs.geometry.staticBucketRouteGeneration);
    constants.staticBucketRouteInfo[3] =
        static_cast<uint32_t>(
            m_sceneInputs.geometry.staticBucketRouteGeneration >> 32);
    constants.geometryInfo2[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.rigidRouteTriangleCount));
    constants.geometryInfo2[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.rigidRouteInstanceCount));
    constants.geometryInfo2[2] = static_cast<float>(m_frameResources.primarySurfaceHistoryBuffers.surfaceCount);
    constants.geometryInfo2[3] = 0.0f;
    constants.geometryInfo3[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.skinnedPreviousPositionCount));
    constants.geometryInfo3[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.skinnedSurfaceDispatchCount));
    constants.geometryInfo3[2] = static_cast<float>(Max(0, m_sceneInputs.geometry.skinnedTriangleDispatchIndexCount));
    constants.geometryInfo3[3] = 0.0f;
    constants.geometryInfo4[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.previousStaticVertexCount));
    constants.geometryInfo4[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.previousStaticIndexCount));
    constants.geometryInfo4[2] = static_cast<float>(Max(0, m_sceneInputs.geometry.previousStaticTriangleCount));
    constants.geometryInfo4[3] = static_cast<float>(Max(0, m_sceneInputs.geometry.previousStaticMaterialIndexCount));
    constants.dispatchTileInfo[0] = 0.0f;
    constants.dispatchTileInfo[1] = 0.0f;
    constants.dispatchTileInfo[2] = static_cast<float>(Max(0, m_frameResources.width));
    constants.dispatchTileInfo[3] = static_cast<float>(Max(0, m_frameResources.height));
    for (int i = 0; i < selectedLightCount; i++)
    {
        constants.lightOriginAndRadius[i][0] = selectedLights[i].origin.x;
        constants.lightOriginAndRadius[i][1] = selectedLights[i].origin.y;
        constants.lightOriginAndRadius[i][2] = selectedLights[i].origin.z;
        constants.lightOriginAndRadius[i][3] = selectedLights[i].radius;
        constants.lightColorAndIntensity[i][0] = selectedLights[i].color.x;
        constants.lightColorAndIntensity[i][1] = selectedLights[i].color.y;
        constants.lightColorAndIntensity[i][2] = selectedLights[i].color.z;
        constants.lightColorAndIntensity[i][3] = selectedLights[i].spriteProxy ? 1.0f : 0.0f;
    }
    const uint64 constantsStartUs = Sys_Microseconds();
    if (optickGpuMarkers)
    {
        OPTICK_GPU_EVENT("PT GPU Write Dispatch Constants");
        commandList->writeBuffer(m_smokeConstantsBuffer, &constants, sizeof(constants));
        if (gpuBoundsOverlayLineCount > 0 && !m_smokeBoundsOverlayLines.empty())
        {
            commandList->writeBuffer(m_smokeBoundsOverlayLineBuffer, m_smokeBoundsOverlayLines.data(), sizeof(RtPathTraceBoundsOverlayLine) * gpuBoundsOverlayLineCount);
        }
    }
    else
    {
        commandList->writeBuffer(m_smokeConstantsBuffer, &constants, sizeof(constants));
        if (gpuBoundsOverlayLineCount > 0 && !m_smokeBoundsOverlayLines.empty())
        {
            commandList->writeBuffer(m_smokeBoundsOverlayLineBuffer, m_smokeBoundsOverlayLines.data(), sizeof(RtPathTraceBoundsOverlayLine) * gpuBoundsOverlayLineCount);
        }
    }
    const uint64 constantsCompleteUs = Sys_Microseconds();
    const uint64 barrierStartUs = constantsCompleteUs;
    const bool primarySurfaceGeometryBarriersRequired = !standaloneDebugRouteRequested || regirDebugNeedsPrimarySurfaceBuffers;
    if (optickGpuMarkers)
    {
        OPTICK_GPU_EVENT("PT GPU Dispatch Resource Barriers");
        if (primarySurfaceGeometryBarriersRequired)
        {
            commandList->setBufferState(dispatchStaticVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(dispatchStaticIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(dispatchStaticTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(dispatchStaticTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(dispatchStaticTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeMaterialTableBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeMaterialFeatureParameterBuffer, nvrhi::ResourceStates::ShaderResource);
        }
        SetBufferStateIfPresent(commandList, m_smokeEmissiveTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokePreviousEmissiveTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeEmissiveRemapBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeEmissiveDistributionBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeLightCandidateBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeDoomAnalyticLightBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeDoomAnalyticPreviousLightBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeDoomAnalyticCurrentIdentityBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeDoomAnalyticPreviousIdentityBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeDoomAnalyticRemapBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeUnifiedLightBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeUnifiedPreviousLightBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeUnifiedLightRemapBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeRestirLightManagerCurrentToPreviousBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeRestirLightManagerPreviousToCurrentBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeRestirLightManagerCurrentPayloadBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeRestirLightManagerPreviousPayloadBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeSkinnedHitRouteRecordBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeSkinnedHitRouteTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_sceneInputs.geometry.skinnedSourceIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeSkinnedCurrentOutputVertexBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeSkinnedPreviousPositionBuffer, nvrhi::ResourceStates::ShaderResource);
        if (primarySurfaceGeometryBarriersRequired)
        {
            commandList->setBufferState(m_smokeRigidRouteVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteInstanceBuffer, nvrhi::ResourceStates::ShaderResource);
        }
        if (!standaloneDebugRouteRequested)
        {
            commandList->setBufferState(m_smokeBoundsOverlayLineBuffer, nvrhi::ResourceStates::ShaderResource);
        }
        if (m_smokeCleanRtxdiDiCurrentReservoirBuffer)
        {
            commandList->setBufferState(m_smokeCleanRtxdiDiCurrentReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_smokeCleanRtxdiDiTemporalReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_smokeCleanRtxdiDiPreviousReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_smokeCleanRtxdiDiSpatialReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
        }
        if (m_smokeReGIRState.candidateCacheBuffer)
        {
            commandList->setBufferState(m_smokeReGIRState.candidateCacheBuffer, nvrhi::ResourceStates::UnorderedAccess);
        }
        SetBufferStateIfPresent(commandList, m_smokeReGIRState.placeholderSrvBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeNeeCacheState.providerResultBuffer, nvrhi::ResourceStates::UnorderedAccess);
        SetBufferStateIfPresent(commandList, m_smokeNeeCacheState.cellBuffer, nvrhi::ResourceStates::UnorderedAccess);
        SetBufferStateIfPresent(commandList, m_smokeNeeCacheState.taskBuffer, nvrhi::ResourceStates::UnorderedAccess);
        SetBufferStateIfPresent(commandList, m_smokeNeeCacheState.candidateBuffer, nvrhi::ResourceStates::UnorderedAccess);
        SetBufferStateIfPresent(commandList, m_smokeNeeCacheState.placeholderSrvBuffer, nvrhi::ResourceStates::ShaderResource);
        if (!standaloneDebugRouteRequested)
        {
            commandList->setBufferState(m_frameResources.primarySurfaceHistoryBuffers.current, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_frameResources.primarySurfaceHistoryBuffers.previous, nvrhi::ResourceStates::UnorderedAccess);
        }
        for (nvrhi::TextureHandle texture : m_smokeActiveTextureTable)
        {
            if (texture)
            {
                commandList->setTextureState(texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            }
        }
        commandList->setTextureState(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        if (!standaloneDebugRouteRequested)
        {
            commandList->setTextureState(m_frameResources.accumulationTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.restirPTReflectionTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrInputColorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.motionVectorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.motionVectorMaskTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideAlbedoTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideSpecularAlbedoTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideNormalRoughnessTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideDepthTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideHitDistanceTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideResetMaskTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuidePositionTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        }
        commandList->commitBarriers();
    }
    else
    {
        if (primarySurfaceGeometryBarriersRequired)
        {
            commandList->setBufferState(dispatchStaticVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(dispatchStaticIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(dispatchStaticTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(dispatchStaticTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(dispatchStaticTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeMaterialTableBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeMaterialFeatureParameterBuffer, nvrhi::ResourceStates::ShaderResource);
        }
        SetBufferStateIfPresent(commandList, m_smokeEmissiveTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokePreviousEmissiveTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeEmissiveRemapBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeEmissiveDistributionBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeLightCandidateBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeDoomAnalyticLightBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeDoomAnalyticPreviousLightBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeDoomAnalyticCurrentIdentityBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeDoomAnalyticPreviousIdentityBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeDoomAnalyticRemapBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeUnifiedLightBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeUnifiedPreviousLightBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeUnifiedLightRemapBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeRestirLightManagerCurrentToPreviousBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeRestirLightManagerPreviousToCurrentBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeRestirLightManagerCurrentPayloadBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeRestirLightManagerPreviousPayloadBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeSkinnedHitRouteRecordBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeSkinnedHitRouteTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_sceneInputs.geometry.skinnedSourceIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeSkinnedCurrentOutputVertexBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeSkinnedPreviousPositionBuffer, nvrhi::ResourceStates::ShaderResource);
        if (primarySurfaceGeometryBarriersRequired)
        {
            commandList->setBufferState(m_smokeRigidRouteVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteInstanceBuffer, nvrhi::ResourceStates::ShaderResource);
        }
        if (!standaloneDebugRouteRequested)
        {
            commandList->setBufferState(m_smokeBoundsOverlayLineBuffer, nvrhi::ResourceStates::ShaderResource);
        }
        if (m_smokeCleanRtxdiDiCurrentReservoirBuffer)
        {
            commandList->setBufferState(m_smokeCleanRtxdiDiCurrentReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_smokeCleanRtxdiDiTemporalReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_smokeCleanRtxdiDiPreviousReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_smokeCleanRtxdiDiSpatialReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
        }
        if (m_smokeReGIRState.candidateCacheBuffer)
        {
            commandList->setBufferState(m_smokeReGIRState.candidateCacheBuffer, nvrhi::ResourceStates::UnorderedAccess);
        }
        SetBufferStateIfPresent(commandList, m_smokeReGIRState.placeholderSrvBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeNeeCacheState.providerResultBuffer, nvrhi::ResourceStates::UnorderedAccess);
        SetBufferStateIfPresent(commandList, m_smokeNeeCacheState.cellBuffer, nvrhi::ResourceStates::UnorderedAccess);
        SetBufferStateIfPresent(commandList, m_smokeNeeCacheState.taskBuffer, nvrhi::ResourceStates::UnorderedAccess);
        SetBufferStateIfPresent(commandList, m_smokeNeeCacheState.candidateBuffer, nvrhi::ResourceStates::UnorderedAccess);
        SetBufferStateIfPresent(commandList, m_smokeNeeCacheState.placeholderSrvBuffer, nvrhi::ResourceStates::ShaderResource);
        if (!standaloneDebugRouteRequested)
        {
            commandList->setBufferState(m_frameResources.primarySurfaceHistoryBuffers.current, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_frameResources.primarySurfaceHistoryBuffers.previous, nvrhi::ResourceStates::UnorderedAccess);
        }
        for (nvrhi::TextureHandle texture : m_smokeActiveTextureTable)
        {
            if (texture)
            {
                commandList->setTextureState(texture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
            }
        }
        commandList->setTextureState(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        if (!standaloneDebugRouteRequested)
        {
            commandList->setTextureState(m_frameResources.accumulationTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.restirPTReflectionTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrInputColorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.motionVectorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.motionVectorMaskTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideAlbedoTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideSpecularAlbedoTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideNormalRoughnessTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideDepthTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideHitDistanceTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuideResetMaskTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setTextureState(m_frameResources.rrGuidePositionTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        }
        commandList->commitBarriers();
    }
    const uint64 barrierCompleteUs = Sys_Microseconds();
    if (m_smokeNeeCacheState.taskClearPending && m_smokeNeeCacheState.providerResultBuffer && m_smokeNeeCacheState.taskBuffer && m_smokeNeeCacheState.cellBuffer && m_smokeNeeCacheState.candidateBuffer)
    {
        if (optickGpuMarkers)
        {
            OPTICK_GPU_EVENT("PT GPU Clear NEE Cache");
        }
        commandList->clearBufferUInt(m_smokeNeeCacheState.providerResultBuffer, 0u);
        commandList->clearBufferUInt(m_smokeNeeCacheState.taskBuffer, 0u);
        commandList->clearBufferUInt(m_smokeNeeCacheState.cellBuffer, 0u);
        commandList->clearBufferUInt(m_smokeNeeCacheState.candidateBuffer, 0u);
        m_smokeNeeCacheState.pendingInvalidationFlags = PATH_TRACE_NEE_CACHE_INVALIDATE_NONE;
        m_smokeNeeCacheState.taskClearPending = false;
    }
    if (!standaloneDebugRouteRequested && disablePrimarySurfaceHistory &&
        (!m_frameResources.primarySurfaceHistoryNeedsClear ||
            m_frameResources.primarySurfaceHistoryView.valid ||
            m_frameResources.primarySurfaceHistoryState.currentValid ||
            m_frameResources.primarySurfaceHistoryState.previousValid))
    {
        m_frameResources.InvalidatePrimarySurfaceHistory(RT_FRAME_RESET_PRIMARY_HISTORY);
    }
    const bool primaryHistoryClearRequested = !standaloneDebugRouteRequested && !disablePrimarySurfaceHistory && m_frameResources.primarySurfaceHistoryNeedsClear;
    const uint64 primaryHistoryClearStartUs = barrierCompleteUs;
    if (primaryHistoryClearRequested)
    {
        if (optickGpuMarkers)
        {
            OPTICK_GPU_EVENT("PT GPU Clear Primary Surface History");
        }
        if (ClearRestirPTPrimarySurfaceHistoryBuffers(commandList, m_frameResources.primarySurfaceHistoryBuffers))
        {
            m_frameResources.primarySurfaceHistoryNeedsClear = false;
        }
    }
    const uint64 primaryHistoryClearCompleteUs = Sys_Microseconds();
    const uint64 targetClearStartUs = primaryHistoryClearCompleteUs;
    if (optickGpuMarkers)
    {
        OPTICK_GPU_EVENT("PT GPU Clear Dispatch Targets");
        commandList->clearTextureFloat(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::Color(0.25f, 0.50f, 0.75f, 1.0f));
        if (!standaloneDebugRouteRequested)
        {
            commandList->clearTextureFloat(m_frameResources.restirPTReflectionTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
            commandList->clearTextureFloat(m_frameResources.rrInputColorTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
            commandList->clearTextureFloat(m_frameResources.rrGuideAlbedoTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
            commandList->clearTextureFloat(m_frameResources.rrGuideSpecularAlbedoTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
            commandList->clearTextureFloat(m_frameResources.rrGuideNormalRoughnessTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 1.0f, 1.0f));
            commandList->clearTextureFloat(m_frameResources.rrGuideDepthTexture, nvrhi::AllSubresources, nvrhi::Color(1.0f, 1.0f, 1.0f, 1.0f));
            commandList->clearTextureFloat(m_frameResources.rrGuideHitDistanceTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));
            commandList->clearTextureFloat(m_frameResources.rrGuidePositionTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));
            commandList->clearTextureUInt(m_frameResources.rrGuideResetMaskTexture, nvrhi::AllSubresources, 0xffffffffu);
            if (accumulationTextureActive && accumulationFrameCount == 0)
            {
                commandList->clearTextureFloat(m_frameResources.accumulationTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));
            }
            if (!motionVectorExportEnabled)
            {
                commandList->clearTextureUInt(m_frameResources.motionVectorMaskTexture, nvrhi::AllSubresources, 0u);
            }
        }
    }
    else
    {
        commandList->clearTextureFloat(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::Color(0.25f, 0.50f, 0.75f, 1.0f));
        if (!standaloneDebugRouteRequested)
        {
            commandList->clearTextureFloat(m_frameResources.restirPTReflectionTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
            commandList->clearTextureFloat(m_frameResources.rrInputColorTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
            commandList->clearTextureFloat(m_frameResources.rrGuideAlbedoTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
            commandList->clearTextureFloat(m_frameResources.rrGuideSpecularAlbedoTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
            commandList->clearTextureFloat(m_frameResources.rrGuideNormalRoughnessTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 1.0f, 1.0f));
            commandList->clearTextureFloat(m_frameResources.rrGuideDepthTexture, nvrhi::AllSubresources, nvrhi::Color(1.0f, 1.0f, 1.0f, 1.0f));
            commandList->clearTextureFloat(m_frameResources.rrGuideHitDistanceTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));
            commandList->clearTextureFloat(m_frameResources.rrGuidePositionTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));
            commandList->clearTextureUInt(m_frameResources.rrGuideResetMaskTexture, nvrhi::AllSubresources, 0xffffffffu);
            if (accumulationTextureActive && accumulationFrameCount == 0)
            {
                commandList->clearTextureFloat(m_frameResources.accumulationTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));
            }
            if (!motionVectorExportEnabled)
            {
                commandList->clearTextureUInt(m_frameResources.motionVectorMaskTexture, nvrhi::AllSubresources, 0u);
            }
        }
    }
    const uint64 targetClearCompleteUs = Sys_Microseconds();
    nvrhi::rt::DispatchRaysArguments args;
    args.width = m_frameResources.width;
    args.height = m_frameResources.height;
    args.depth = 1;
    int timingDispatchWidth = args.width;
    int timingDispatchHeight = args.height;

    auto dispatchSmokeRays = [&](const nvrhi::rt::DispatchRaysArguments& dispatchArgs, int domainWidth, int domainHeight)
    {
        if (dispatchTileSettings.enabled)
        {
            timingDispatchWidth = dispatchTileSettings.tileWidth;
            timingDispatchHeight = dispatchTileSettings.tileHeight;
            for (int tileY = 0; tileY < domainHeight; tileY += dispatchTileSettings.tileHeight)
            {
                for (int tileX = 0; tileX < domainWidth; tileX += dispatchTileSettings.tileWidth)
                {
                    PathTraceSmokeConstants tileConstants = constants;
                    tileConstants.dispatchTileInfo[0] = static_cast<float>(tileX);
                    tileConstants.dispatchTileInfo[1] = static_cast<float>(tileY);
                    commandList->writeBuffer(m_smokeConstantsBuffer, &tileConstants, sizeof(tileConstants));

                    nvrhi::rt::DispatchRaysArguments tileArgs;
                    tileArgs.width = Min(dispatchTileSettings.tileWidth, domainWidth - tileX);
                    tileArgs.height = Min(dispatchTileSettings.tileHeight, domainHeight - tileY);
                    tileArgs.depth = 1;
                    commandList->dispatchRays(tileArgs);
                }
            }
        }
        else
        {
            PathTraceSmokeConstants dispatchConstants = constants;
            dispatchConstants.dispatchTileInfo[0] = 0.0f;
            dispatchConstants.dispatchTileInfo[1] = 0.0f;
            commandList->writeBuffer(m_smokeConstantsBuffer, &dispatchConstants, sizeof(dispatchConstants));
            commandList->dispatchRays(dispatchArgs);
        }
    };

    const uint64 setStateStartUs = targetClearCompleteUs;
    if (requestedLiquidPoolMode != 0 && liquidPoolTelemetryReady)
    {
        commandList->setBufferState(m_liquidPoolStatusBuffer, nvrhi::ResourceStates::UnorderedAccess);
        commandList->commitBarriers();
        commandList->clearBufferUInt(m_liquidPoolStatusBuffer, 0u);
    }
    if (optickGpuMarkers)
    {
        OPTICK_GPU_EVENT("PT GPU Set Ray Tracing State");
        commandList->setRayTracingState(state);
    }
    else
    {
        commandList->setRayTracingState(state);
    }
    const uint64 setStateCompleteUs = Sys_Microseconds();

    const uint64 dispatchRaysStartUs = setStateCompleteUs;
    {
        PathTraceGpuMarkerScope finalDispatchNsightMarker(commandList, "PT Final DispatchRays", nsightGpuMarkers);
        if (regirDebugRouteRequested)
        {
            PathTraceSmokeConstants regirBuildConstants = constants;
            regirBuildConstants.regirInfo3[0] = 1.0f;
            regirBuildConstants.dispatchTileInfo[0] = 0.0f;
            regirBuildConstants.dispatchTileInfo[1] = 0.0f;
            regirBuildConstants.dispatchTileInfo[2] = static_cast<float>(m_frameResources.width);
            regirBuildConstants.dispatchTileInfo[3] = static_cast<float>(m_frameResources.height);
            commandList->writeBuffer(m_smokeConstantsBuffer, &regirBuildConstants, sizeof(regirBuildConstants));
            commandList->dispatchRays(args);
            nvrhi::utils::BufferUavBarrier(commandList, m_smokeReGIRState.candidateCacheBuffer);
            commandList->commitBarriers();
            commandList->writeBuffer(m_smokeConstantsBuffer, &constants, sizeof(constants));
        }
        if (neeCacheCandidateBuildRequested && neeCacheDebugBindingSet && m_smokeNeeCacheDebugShaderTable && m_smokeNeeCacheState.candidateBuffer)
        {
            PathTraceSmokeConstants neeCandidateBuildConstants = constants;
            neeCandidateBuildConstants.neeCacheInfo3[0] = 3.0f;
            neeCandidateBuildConstants.dispatchTileInfo[0] = 0.0f;
            neeCandidateBuildConstants.dispatchTileInfo[1] = 0.0f;
            neeCandidateBuildConstants.dispatchTileInfo[2] = static_cast<float>(m_frameResources.width);
            neeCandidateBuildConstants.dispatchTileInfo[3] = static_cast<float>(m_frameResources.height);
            commandList->writeBuffer(m_smokeConstantsBuffer, &neeCandidateBuildConstants, sizeof(neeCandidateBuildConstants));
            if (!neeCacheDebugRouteRequested)
            {
                nvrhi::rt::State neeCandidateBuildState;
                neeCandidateBuildState.shaderTable = m_smokeNeeCacheDebugShaderTable;
                neeCandidateBuildState.bindings = { neeCacheDebugBindingSet, m_smokeTextureDescriptorTable };
                commandList->setRayTracingState(neeCandidateBuildState);
            }
            commandList->dispatchRays(args);
            nvrhi::utils::BufferUavBarrier(commandList, m_smokeNeeCacheState.providerResultBuffer);
            nvrhi::utils::BufferUavBarrier(commandList, m_smokeNeeCacheState.cellBuffer);
            nvrhi::utils::BufferUavBarrier(commandList, m_smokeNeeCacheState.candidateBuffer);
            if (((pdfNeeRluCurrentProducerRequested && pdfNeeNeeCacheProviderReady) || neeCacheSecondaryConsumeReady) && !neeCacheDebugRouteRequested)
            {
                commandList->setBufferState(m_smokeNeeCacheState.providerResultBuffer, nvrhi::ResourceStates::ShaderResource);
                commandList->setBufferState(m_smokeNeeCacheState.cellBuffer, nvrhi::ResourceStates::ShaderResource);
                commandList->setBufferState(m_smokeNeeCacheState.candidateBuffer, nvrhi::ResourceStates::ShaderResource);
            }
            commandList->commitBarriers();
            commandList->writeBuffer(m_smokeConstantsBuffer, &constants, sizeof(constants));
            if (!neeCacheDebugRouteRequested)
            {
                commandList->setRayTracingState(state);
            }
        }
        if (neeCacheDebugRouteRequested && neeCacheSettings.debugView == 4 && m_smokeNeeCacheState.taskBuffer)
        {
            PathTraceSmokeConstants neeTaskDecayConstants = constants;
            neeTaskDecayConstants.neeCacheInfo3[0] = 2.0f;
            neeTaskDecayConstants.dispatchTileInfo[0] = 0.0f;
            neeTaskDecayConstants.dispatchTileInfo[1] = 0.0f;
            neeTaskDecayConstants.dispatchTileInfo[2] = static_cast<float>(m_frameResources.width);
            neeTaskDecayConstants.dispatchTileInfo[3] = static_cast<float>(m_frameResources.height);
            commandList->writeBuffer(m_smokeConstantsBuffer, &neeTaskDecayConstants, sizeof(neeTaskDecayConstants));
            commandList->dispatchRays(args);
            nvrhi::utils::BufferUavBarrier(commandList, m_smokeNeeCacheState.taskBuffer);
            nvrhi::utils::BufferUavBarrier(commandList, m_smokeNeeCacheState.cellBuffer);
            commandList->commitBarriers();
            commandList->writeBuffer(m_smokeConstantsBuffer, &constants, sizeof(constants));
        }
        if (dispatchTileSettings.enabled)
        {
            if (optickGpuMarkers)
            {
                OPTICK_GPU_EVENT("PT GPU Dispatch Ray Tiles");
            }
            dispatchSmokeRays(args, m_frameResources.width, m_frameResources.height);
        }
        else
        {
            if (optickGpuMarkers)
            {
                OPTICK_GPU_EVENT("PT GPU Dispatch Rays");
                dispatchSmokeRays(args, m_frameResources.width, m_frameResources.height);
            }
            else
            {
                dispatchSmokeRays(args, m_frameResources.width, m_frameResources.height);
            }
        }
    }
    const uint64 dispatchRaysCompleteUs = Sys_Microseconds();
    ExecutePathTraceParticleComposite(commandList, viewDef);
    QueueStaticContractShaderSample(commandList);
    QueueSkinnedHitAuditSamples(commandList);
    if (requestedLiquidPoolMode != 0 && liquidPoolTelemetryReady)
    {
        nvrhi::utils::BufferUavBarrier(commandList, m_liquidPoolStatusBuffer);
        if (!m_liquidPoolStatusReadbackQueued)
        {
            commandList->setBufferState(m_liquidPoolStatusBuffer, nvrhi::ResourceStates::CopySource);
            commandList->setBufferState(m_liquidPoolStatusReadbackBuffer, nvrhi::ResourceStates::CopyDest);
            commandList->commitBarriers();
            commandList->copyBuffer(
                m_liquidPoolStatusReadbackBuffer,
                0,
                m_liquidPoolStatusBuffer,
                0,
                sizeof(uint32_t) * 16u);
            commandList->setBufferState(m_liquidPoolStatusBuffer, nvrhi::ResourceStates::UnorderedAccess);
            commandList->commitBarriers();
            m_liquidPoolStatusReadbackQueued = true;
            m_liquidPoolStatusReadbackDelayFrames = 3;
        }
    }
    const uint64 historyCopyStartUs = Sys_Microseconds();
    if (!standaloneDebugRouteRequested && !disablePrimarySurfaceHistory)
    {
        commandList->setBufferState(m_frameResources.primarySurfaceHistoryBuffers.current, nvrhi::ResourceStates::CopySource);
        commandList->setBufferState(m_frameResources.primarySurfaceHistoryBuffers.previous, nvrhi::ResourceStates::CopyDest);
        commandList->commitBarriers();
        commandList->copyBuffer(
            m_frameResources.primarySurfaceHistoryBuffers.previous,
            0,
            m_frameResources.primarySurfaceHistoryBuffers.current,
            0,
            m_frameResources.primarySurfaceHistoryBuffers.surfaceBytes);
    }
    const uint64 historyCopyCompleteUs = Sys_Microseconds();
    if (!standaloneDebugRouteRequested && !disablePrimarySurfaceHistory)
    {
        RtPathTraceFrameCameraState currentHistoryView;
        currentHistoryView.valid = true;
        currentHistoryView.width = m_frameResources.width;
        currentHistoryView.height = m_frameResources.height;
        currentHistoryView.origin = cameraOrigin;
        currentHistoryView.forward = cameraForward;
        currentHistoryView.left = cameraLeft;
        currentHistoryView.up = cameraUp;
        currentHistoryView.tanX = constants.cameraForwardAndTanX[3];
        currentHistoryView.tanY = constants.cameraLeftAndTanY[3];
        const bool objectMotionAvailable =
            (m_sceneInputs.geometry.skinnedPreviousPositionBufferAvailable && m_sceneInputs.geometry.skinnedSurfaceDispatchCount > 0) ||
            (m_sceneInputs.geometry.previousTransformAvailable && m_sceneInputs.geometry.rigidRouteInstanceCount > 0);
        m_frameResources.SetPrimarySurfaceHistoryView(currentHistoryView, objectMotionAvailable);
    }
    if (mode18AccumulationActive)
    {
        m_frameResources.smokeAccumulationFrameCount = Min(m_frameResources.smokeAccumulationFrameCount + 1, accumulationMaxFrames);
    }
    else
    {
        m_frameResources.smokeAccumulationFrameCount = 0;
    }
    const bool forceOverlapReadback = debugMode == 24 && r_pathTracingRigidRouteOverlapDump.GetInteger() != 0;
    const bool forceCleanTemporalAuditReadback =
        !cleanRtxdiDiProductionView &&
        r_pathTracingCleanRtxdiDiTemporalAudit.GetInteger() != 0;
    bool readbackQueuedThisFrame = false;
    const uint64 readbackCopyStartUs = historyCopyCompleteUs;
    if ((r_pathTracingReadbackEnable.GetInteger() != 0 || forceOverlapReadback || forceCleanTemporalAuditReadback) && !m_frameResources.readbackQueued && (m_frameResources.readbackCooldownFrames <= 0 || forceOverlapReadback || forceCleanTemporalAuditReadback))
    {
        if (optickGpuMarkers)
        {
            OPTICK_GPU_EVENT("PT GPU Readback Copy");
            commandList->setTextureState(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
            commandList->commitBarriers();
            commandList->copyTexture(m_frameResources.readbackTexture, nvrhi::TextureSlice(), m_frameResources.outputTexture, nvrhi::TextureSlice());
        }
        else
        {
            commandList->setTextureState(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
            commandList->commitBarriers();
            commandList->copyTexture(m_frameResources.readbackTexture, nvrhi::TextureSlice(), m_frameResources.outputTexture, nvrhi::TextureSlice());
        }
        m_frameResources.readbackQueued = true;
        m_frameResources.readbackDelayFrames = 2;
        m_frameResources.RecordReadbackQueued();
        readbackQueuedThisFrame = true;
        if (r_pathTracingSmokeLog.GetInteger() != 0 || forceCleanTemporalAuditReadback)
        {
            common->Printf("PathTracePrimaryPass: queued RT smoke UAV readback\n");
        }
    }
    const uint64 readbackCopyCompleteUs = Sys_Microseconds();

    const uint64 totalSubmitUs = readbackCopyCompleteUs - executeStartUs;
    const int totalSubmitMsForThrottle = static_cast<int>((totalSubmitUs + 999u) / 1000u);
    if (ShouldLogSmokeTiming(totalSubmitMsForThrottle, Sys_Milliseconds(), g_smokeLastDispatchTimingLogMs))
    {
        RtPathTraceDispatchTimingLogDesc timingDesc;
        timingDesc.totalSubmitMs = PathTraceMicrosecondsToMilliseconds(totalSubmitUs);
        timingDesc.setupMs = PathTraceMicrosecondsToMilliseconds(setupCompleteUs - executeStartUs);
        timingDesc.constantsMs = PathTraceMicrosecondsToMilliseconds(constantsCompleteUs - constantsStartUs);
        timingDesc.barrierMs = PathTraceMicrosecondsToMilliseconds(barrierCompleteUs - barrierStartUs);
        timingDesc.primaryHistoryClearMs = PathTraceMicrosecondsToMilliseconds(primaryHistoryClearCompleteUs - primaryHistoryClearStartUs);
        timingDesc.targetClearMs = PathTraceMicrosecondsToMilliseconds(targetClearCompleteUs - targetClearStartUs);
        timingDesc.setStateMs = PathTraceMicrosecondsToMilliseconds(setStateCompleteUs - setStateStartUs);
        timingDesc.dispatchSubmitMs = PathTraceMicrosecondsToMilliseconds(dispatchRaysCompleteUs - dispatchRaysStartUs);
        timingDesc.historyCopyMs = PathTraceMicrosecondsToMilliseconds(historyCopyCompleteUs - historyCopyStartUs);
        timingDesc.readbackCopyMs = PathTraceMicrosecondsToMilliseconds(readbackCopyCompleteUs - readbackCopyStartUs);
        timingDesc.outputWidth = m_frameResources.outputWidth;
        timingDesc.outputHeight = m_frameResources.outputHeight;
        timingDesc.dispatchWidth = timingDispatchWidth;
        timingDesc.dispatchHeight = timingDispatchHeight;
        timingDesc.debugMode = debugMode;
        timingDesc.samplesPerPixel = integratorSettings.samplesPerPixel;
        timingDesc.maxPathDepth = integratorSettings.maxPathDepth;
        timingDesc.estimatedRaysPerPixel = estimatedRaysPerPixel;
        timingDesc.selectedLights = selectedLightRequestCount;
        timingDesc.analyticLights = analyticLightTraceCount;
        timingDesc.primaryHistoryClearRequested = primaryHistoryClearRequested;
        timingDesc.readbackQueued = readbackQueuedThisFrame;
        timingDesc.optickGpuMarkers = optickGpuMarkers;
        timingDesc.nsightGpuMarkers = nsightGpuMarkers;
        timingDesc.debugModeInfo = &debugModeInfo;
        LogPathTraceDispatchTiming(timingDesc);
    }

    if (!m_smokeTestDispatched)
    {
        common->Printf("PathTracePrimaryPass: dispatched RT smoke camera raygen (%dx%d, debugMode=%d)\n", m_frameResources.width, m_frameResources.height, debugMode);
    }
    m_smokeTestDispatched = true;
}
