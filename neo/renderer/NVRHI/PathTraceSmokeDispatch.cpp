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
#include "PathTraceSmokeDispatch.h"
#include "PathTracePrimaryPass.h"
#include "PathTraceAcceleration.h"
#include "PathTraceDebugDumps.h"
#include "PathTraceDoomLights.h"
#include "PathTraceLightSelection.h"
#include "PathTraceMaterialFeatureBindings.h"
#include "PathTraceMaterialFeatureRuntime.h"
#include "PathTraceNeeCache.h"
#include "PathTraceReGIR.h"
#include "PathTraceRemixRtxdiResourceGate.h"
#include "PathTraceRestirPasses.h"
#include "PathTraceDLSSRRBridge.h"
#include "../RenderBackend.h"
#include "../RenderCommon.h"
#include "../../sys/DeviceManager.h"

#include <algorithm>
#include <cmath>

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
int g_smokeLastDispatchTimingLogMs = -1000000;
PathTraceCleanRtxdiDiGuiSnapshot g_cleanRtxdiDiGuiSnapshot;

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

const char* PathTraceReGIRCenterPolicyName(const PathTraceReGIRSettings& settings)
{
    if (settings.centerMode == 2)
    {
        return "manual";
    }
    if (settings.centerMode == 1)
    {
        return "map-bounds-clamp-to-view-when-grid-smaller-than-map-cell-snapped";
    }
    return "camera-cell-snapped";
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
};

static_assert(sizeof(PathTraceCleanRtxdiDiSentinelConstants) <= 480, "PathTraceCleanRtxdiDiSentinelConstants exceeds allocated constant buffer size");

uint32_t RrxDiRequestedInitialSampleBudget(uint32_t emissiveSampleCount, uint32_t doomAnalyticSampleCount)
{
    const uint64_t requestedTotal =
        static_cast<uint64_t>(emissiveSampleCount) + static_cast<uint64_t>(doomAnalyticSampleCount);
    if (requestedTotal == 0)
    {
        return 0;
    }

    return static_cast<uint32_t>(std::min<uint64_t>(requestedTotal, 32ull));
}

uint32_t RrxDiBoundedRangeSampleCount(uint32_t rangeCount, uint32_t totalLightCount, uint32_t requestedTotal)
{
    if (rangeCount == 0 || totalLightCount == 0 || requestedTotal == 0)
    {
        return 0;
    }

    const uint64_t roundedSamples =
        (static_cast<uint64_t>(rangeCount) * static_cast<uint64_t>(requestedTotal) +
            static_cast<uint64_t>(totalLightCount / 2u)) /
        static_cast<uint64_t>(totalLightCount);
    const uint32_t nonEmptyRangeSamples = static_cast<uint32_t>(std::max<uint64_t>(1ull, roundedSamples));
    return std::min(rangeCount, nonEmptyRangeSamples);
}

enum PathTraceGpuTimingStage
{
    PT_GPU_TIMING_PRIMARY_SURFACE = 0,
    PT_GPU_TIMING_GI_INITIAL,
    PT_GPU_TIMING_DIRECT_TEMPORAL,
    PT_GPU_TIMING_DIRECT_SPATIAL,
    PT_GPU_TIMING_REFLECTION,
    PT_GPU_TIMING_FINAL_RESOLVE,
    PT_GPU_TIMING_COUNT
};

struct PathTraceGpuTimingFrame
{
    bool pending = false;
    uint64 serial = 0;
    int debugMode = 0;
    int width = 0;
    int height = 0;
    char restirPassLabel[64] = {};
    bool stageUsed[PT_GPU_TIMING_COUNT] = {};
    nvrhi::TimerQueryHandle queries[PT_GPU_TIMING_COUNT];
};

struct PathTraceGpuTimingCapture
{
    PathTraceGpuTimingFrame* frame = nullptr;
};

PathTraceGpuTimingFrame g_pathTraceGpuTimingFrames[4];
int g_pathTraceGpuTimingWriteIndex = 0;
uint64 g_pathTraceGpuTimingSerial = 0;

nvrhi::ObjectType GetPathTraceCommandObjectType()
{
    if (deviceManager && deviceManager->GetGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
    {
        return nvrhi::ObjectTypes::VK_CommandBuffer;
    }
    return nvrhi::ObjectTypes::D3D12_GraphicsCommandList;
}

void PollPathTraceGpuTimingResults()
{
    if (!deviceManager || !deviceManager->GetDevice())
    {
        return;
    }

    for (PathTraceGpuTimingFrame& frame : g_pathTraceGpuTimingFrames)
    {
        if (!frame.pending)
        {
            continue;
        }

        bool ready = true;
        for (int stage = 0; stage < PT_GPU_TIMING_COUNT; ++stage)
        {
            if (frame.stageUsed[stage] && (!frame.queries[stage] || !deviceManager->GetDevice()->pollTimerQuery(frame.queries[stage])))
            {
                ready = false;
                break;
            }
        }
        if (!ready)
        {
            continue;
        }

        float stageMs[PT_GPU_TIMING_COUNT] = {};
        float measuredTotalMs = 0.0f;
        for (int stage = 0; stage < PT_GPU_TIMING_COUNT; ++stage)
        {
            if (!frame.stageUsed[stage])
            {
                continue;
            }
            stageMs[stage] = deviceManager->GetDevice()->getTimerQueryTime(frame.queries[stage]) * 1000.0f;
            measuredTotalMs += stageMs[stage];
        }

        common->Printf(
            "PathTracePrimaryPass: ReSTIR PT GPU timing serial=%llu mode=%d pass=%s output=%dx%d measuredTotal=%.3fms primarySurface=%.3f giInitial=%.3f directTemporal=%.3f directSpatial=%.3f reflection=%.3f finalResolve=%.3f usedMask=0x%02x\n",
            static_cast<unsigned long long>(frame.serial),
            frame.debugMode,
            frame.restirPassLabel[0] ? frame.restirPassLabel : "unknown",
            frame.width,
            frame.height,
            measuredTotalMs,
            stageMs[PT_GPU_TIMING_PRIMARY_SURFACE],
            stageMs[PT_GPU_TIMING_GI_INITIAL],
            stageMs[PT_GPU_TIMING_DIRECT_TEMPORAL],
            stageMs[PT_GPU_TIMING_DIRECT_SPATIAL],
            stageMs[PT_GPU_TIMING_REFLECTION],
            stageMs[PT_GPU_TIMING_FINAL_RESOLVE],
            (frame.stageUsed[PT_GPU_TIMING_PRIMARY_SURFACE] ? (1u << PT_GPU_TIMING_PRIMARY_SURFACE) : 0u) |
            (frame.stageUsed[PT_GPU_TIMING_GI_INITIAL] ? (1u << PT_GPU_TIMING_GI_INITIAL) : 0u) |
            (frame.stageUsed[PT_GPU_TIMING_DIRECT_TEMPORAL] ? (1u << PT_GPU_TIMING_DIRECT_TEMPORAL) : 0u) |
            (frame.stageUsed[PT_GPU_TIMING_DIRECT_SPATIAL] ? (1u << PT_GPU_TIMING_DIRECT_SPATIAL) : 0u) |
            (frame.stageUsed[PT_GPU_TIMING_REFLECTION] ? (1u << PT_GPU_TIMING_REFLECTION) : 0u) |
            (frame.stageUsed[PT_GPU_TIMING_FINAL_RESOLVE] ? (1u << PT_GPU_TIMING_FINAL_RESOLVE) : 0u));

        frame.pending = false;
        for (int stage = 0; stage < PT_GPU_TIMING_COUNT; ++stage)
        {
            frame.stageUsed[stage] = false;
        }
    }
}

bool EnsurePathTraceGpuTimingQueries(PathTraceGpuTimingFrame& frame)
{
    if (!deviceManager || !deviceManager->GetDevice())
    {
        return false;
    }
    for (int stage = 0; stage < PT_GPU_TIMING_COUNT; ++stage)
    {
        if (!frame.queries[stage])
        {
            frame.queries[stage] = deviceManager->GetDevice()->createTimerQuery();
            if (!frame.queries[stage])
            {
                return false;
            }
        }
    }
    return true;
}

PathTraceGpuTimingCapture BeginPathTraceGpuTimingCapture(int debugMode, int width, int height, const char* restirPassLabel)
{
    PathTraceGpuTimingCapture capture;
    if (r_pathTracingRestirPTGpuTimingDump.GetInteger() == 0)
    {
        return capture;
    }
    r_pathTracingRestirPTGpuTimingDump.SetInteger(0);

    if (!glConfig.timerQueryAvailable)
    {
        common->Printf("PathTracePrimaryPass: ReSTIR PT GPU timing unavailable because timer queries are disabled\n");
        return capture;
    }

    PathTraceGpuTimingFrame& frame = g_pathTraceGpuTimingFrames[g_pathTraceGpuTimingWriteIndex];
    g_pathTraceGpuTimingWriteIndex = (g_pathTraceGpuTimingWriteIndex + 1) % static_cast<int>(sizeof(g_pathTraceGpuTimingFrames) / sizeof(g_pathTraceGpuTimingFrames[0]));
    if (frame.pending)
    {
        common->Printf("PathTracePrimaryPass: ReSTIR PT GPU timing skipped because the query ring still has pending results\n");
        return capture;
    }
    if (!EnsurePathTraceGpuTimingQueries(frame))
    {
        common->Printf("PathTracePrimaryPass: ReSTIR PT GPU timing skipped because timer query allocation failed\n");
        return capture;
    }

    frame.pending = true;
    frame.serial = ++g_pathTraceGpuTimingSerial;
    frame.debugMode = debugMode;
    frame.width = width;
    frame.height = height;
    idStr::Copynz(frame.restirPassLabel, restirPassLabel ? restirPassLabel : "unknown", sizeof(frame.restirPassLabel));
    for (int stage = 0; stage < PT_GPU_TIMING_COUNT; ++stage)
    {
        frame.stageUsed[stage] = false;
    }

    common->Printf("PathTracePrimaryPass: ReSTIR PT GPU timing capture queued serial=%llu mode=%d pass=%s output=%dx%d\n",
        static_cast<unsigned long long>(frame.serial),
        frame.debugMode,
        frame.restirPassLabel,
        frame.width,
        frame.height);

    capture.frame = &frame;
    return capture;
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

void DispatchPathTraceCleanMaterialFeaturePass(
    nvrhi::ICommandList* commandList,
    const nvrhi::rt::State& baseState,
    const nvrhi::rt::DispatchRaysArguments& args,
    nvrhi::BufferHandle constantsBuffer,
    const PathTraceCleanRtxdiDiSentinelConstants& baseConstants,
    const RtPathTraceMaterialFeatureRuntimePass& pass,
    const RtPathTraceFrameResources& frameResources,
    bool nsightGpuMarkers)
{
    if (!commandList || !pass.ready || !pass.shader || !pass.shader->shaderTable)
    {
        return;
    }

    nvrhi::rt::State featureState = baseState;
    featureState.shaderTable = pass.shader->shaderTable;
    commandList->setRayTracingState(featureState);

    PathTraceCleanRtxdiDiSentinelConstants featureConstants = baseConstants;
    SetPathTraceMaterialFeatureRuntimeInfo(featureConstants.toyPathInfo, pass);
    commandList->writeBuffer(constantsBuffer, &featureConstants, sizeof(featureConstants));

    {
        PathTraceGpuMarkerScope nsightMarker(commandList, pass.desc.debugLabel, nsightGpuMarkers);
        commandList->dispatchRays(args);
    }
    BarrierPathTraceMaterialFeatureOutputs(commandList, pass, frameResources);
}

class PathTraceGpuTimingStageScope
{
public:
    PathTraceGpuTimingStageScope(const PathTraceGpuTimingCapture& capture, nvrhi::ICommandList* commandList, PathTraceGpuTimingStage stage)
        : m_frame(capture.frame)
        , m_commandList(commandList)
        , m_stage(stage)
    {
        if (!m_frame || !m_commandList || m_stage < 0 || m_stage >= PT_GPU_TIMING_COUNT)
        {
            m_frame = nullptr;
            return;
        }
        m_frame->stageUsed[m_stage] = true;
        m_commandList->beginTimerQuery(m_frame->queries[m_stage]);
    }

    ~PathTraceGpuTimingStageScope()
    {
        if (m_frame && m_commandList)
        {
            m_commandList->endTimerQuery(m_frame->queries[m_stage]);
        }
    }

private:
    PathTraceGpuTimingFrame* m_frame = nullptr;
    nvrhi::ICommandList* m_commandList = nullptr;
    PathTraceGpuTimingStage m_stage = PT_GPU_TIMING_COUNT;
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
    float restirPTDirectInfo[4];
    float restirPTSparsityInfo[4];
    float restirPTIndirectInfo[4];
    float rayReconstructionInfo[4];
    float unifiedLightInfo[4];
    float restirLightManagerInfo[4];
    float restirLightManagerControlInfo[4];
    float restirLightManagerRangeInfo[4];
    float restirLightManagerSampleInfo[4];
    float restirPdfNeeVerifierInfo[4];
    float restirPdfNeeVerifierControlInfo[4];
    float restirPTDiDebugInfo[4];
    uint32_t restirPTRemixDiReservoirInfo[4];
    uint32_t restirPTRemixDiReservoirPageInfo[4];
    float restirPTGiDebugInfo[4];
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
};

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

enum RtRestirPTShaderDispatchMode
{
    RT_RESTIR_PT_SHADER_DISPATCH_FULL = 0,
    RT_RESTIR_PT_SHADER_DISPATCH_DIRECT_TRACE_PRIMARY = 1,
    RT_RESTIR_PT_SHADER_DISPATCH_PRIMARY_SURFACE_ONLY = 2,
    RT_RESTIR_PT_SHADER_DISPATCH_DIRECT_CONSUME_PRIMARY = 3,
    RT_RESTIR_PT_SHADER_DISPATCH_FULL_CONSUME_PRIMARY = 4,
    RT_RESTIR_PT_SHADER_DISPATCH_DIRECT_TRACE_PRIMARY_SPARSE = 5,
    RT_RESTIR_PT_SHADER_DISPATCH_DIRECT_CONSUME_PRIMARY_SPARSE = 6
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
    RT_PT_SAFETY_DISABLE_RESERVOIR_WRITES = 1u << 7,
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
    mask |= r_pathTracingDisableReservoirWrites.GetInteger() != 0 ? RT_PT_SAFETY_DISABLE_RESERVOIR_WRITES : 0u;
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

int PathTraceScaledRestirDimension(int fullDimension, float scale)
{
    if (fullDimension <= 0)
    {
        return 1;
    }
    return Max(1, static_cast<int>(idMath::Ceil(static_cast<float>(fullDimension) * scale)));
}

}

void PathTracePrimaryPass::QueueDLSSRRInputColorDump(nvrhi::ICommandList* commandList, nvrhi::ITexture* inputColor, int source, uint32_t frameIndex)
{
    if (r_pathTracingDLSSRRInputDump.GetInteger() == 0)
    {
        return;
    }
    if (!commandList || !inputColor || !m_frameResources.rrInputColorDumpReadbackTexture)
    {
        common->Printf("PathTraceDLSSRR: input HDR EXR dump armed but missing command/input/staging command=%d input=%d staging=%d\n",
            commandList ? 1 : 0,
            inputColor ? 1 : 0,
            m_frameResources.rrInputColorDumpReadbackTexture ? 1 : 0);
        return;
    }
    if (m_frameResources.rrInputColorDumpQueued)
    {
        return;
    }

    commandList->setTextureState(inputColor, nvrhi::AllSubresources, nvrhi::ResourceStates::CopySource);
    commandList->commitBarriers();
    commandList->copyTexture(m_frameResources.rrInputColorDumpReadbackTexture, nvrhi::TextureSlice(), inputColor, nvrhi::TextureSlice());
    commandList->setTextureState(inputColor, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();

    m_frameResources.rrInputColorDumpQueued = true;
    m_frameResources.rrInputColorDumpDelayFrames = 2;
    m_frameResources.rrInputColorDumpSource = source;
    m_frameResources.rrInputColorDumpFrameIndex = frameIndex;
    m_frameResources.RecordReadbackQueued();
    r_pathTracingDLSSRRInputDump.SetInteger(0);

    common->Printf("PathTraceDLSSRR: queued input HDR EXR dump source=%d frame=%u\n",
        source,
        static_cast<unsigned int>(frameIndex));
}

size_t GetPathTraceSmokeConstantsSize()
{
    return sizeof(PathTraceSmokeConstants);
}
void PathTracePrimaryPass::ExecuteRayTracingSmokeTest(const viewDef_t* viewDef)
{
    OPTICK_EVENT("PT Dispatch");

    const uint64 executeStartUs = Sys_Microseconds();
    if (r_pathTracingRestirPTView68Dump.GetInteger() != 0)
    {
        common->Printf("PathTracePrimaryPass: PT mode56 view68 dispatch entry rawDebug=%d rawDiView=%d sceneBuilt=%d output=%d readback=%d primaryHistory=%d\n",
            r_pathTracingDebugMode.GetInteger(),
            r_pathTracingRestirPTDiDebugView.GetInteger(),
            m_smokeSceneBuilt ? 1 : 0,
            m_frameResources.outputTexture ? 1 : 0,
            m_frameResources.readbackTexture ? 1 : 0,
            m_frameResources.primarySurfaceHistoryBuffers.current ? 1 : 0);
    }
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
        return "none";
    };
    const bool cleanRtxdiDiEnabled = r_pathTracingCleanRtxdiDiEnable.GetInteger() != 0;
    const int cleanRtxdiDiView = cleanRtxdiDiEnabled ? r_pathTracingCleanRtxdiDiView.GetInteger() : 0;
    const bool cleanRtxdiDiMaterialClassifierProofView = cleanRtxdiDiView == 12 || cleanRtxdiDiView == 24;
    const bool cleanRtxdiDiTemporalEnabled =
        r_pathTracingCleanRtxdiDiTemporal.GetInteger() != 0 &&
        !cleanRtxdiDiMaterialClassifierProofView;
    const bool cleanRtxdiDiRrInputMosaicView = cleanRtxdiDiView == 18;
    const bool cleanRtxdiDiRrGuideDebugView = cleanRtxdiDiView >= 18 && cleanRtxdiDiView <= 23;
    const int cleanRtxdiDiResolveView = cleanRtxdiDiRrGuideDebugView ? 16 : cleanRtxdiDiView;
    const int cleanRtxdiDiView18Tile = idMath::ClampInt(-1, 5, r_pathTracingCleanRtxdiDiView18Tile.GetInteger());
    const uint32_t cleanRtxdiDiFrameIndexForDispatch = r_pathTracingCleanRtxdiDiFrameFreeze.GetInteger() != 0
        ? 0u
        : m_smokeCleanRtxdiDiFrameIndex;
    const bool cleanRtxdiDiRouteRequested = cleanRtxdiDiView >= 1 && cleanRtxdiDiView <= 24;
    const bool cleanRtxdiDiSpatialEnabled =
        r_pathTracingCleanRtxdiDiSpatial.GetInteger() != 0 &&
        r_cleanDiSpatial.GetInteger() != 0 &&
        r_cleanSpatial.GetInteger() != 0;
    const bool cleanRtxdiDiSpatialShaderRequested =
        cleanRtxdiDiRouteRequested &&
        !cleanRtxdiDiMaterialClassifierProofView &&
        (cleanRtxdiDiView == 16 ||
            cleanRtxdiDiRrGuideDebugView ||
            (cleanRtxdiDiSpatialEnabled &&
                (cleanRtxdiDiView == 12 ||
                    (cleanRtxdiDiView == 8 && idMath::ClampInt(-1, 16, r_pathTracingCleanRtxdiDiView8Band.GetInteger()) == 16))));
    const RtPathTraceMaterialFeatureRuntimePass cleanRtxdiDiTransmissionPass = BuildPathTraceMaterialFeatureRuntimePass(
        BuildPathTraceCleanRtxdiDiTransmissionFeaturePassDesc(
            cleanRtxdiDiRouteRequested,
            cleanRtxdiDiView,
            r_pathTracingCleanRtxdiDiTransmissionProducer.GetInteger() != 0,
            r_pathTracingCleanRtxdiDiTransmissionDebugView.GetInteger() != 0),
        m_smokeMaterialFeatureShaders.data(),
        m_smokeMaterialFeatureShaders.size());
    const bool cleanExternalPdfNeeRequested = r_pathTracingCleanRtxdiDiExternalPdfNeeCurrent.GetInteger() != 0;
    const bool pdfNeeVerifierDumpRequested = r_pathTracingRestirPdfNeeVerifierDump.GetInteger() != 0;
    const int pdfNeeVerifierEntryView = idMath::ClampInt(0, 8, r_pathTracingRestirPdfNeeVerifierView.GetInteger());
    const int pdfNeeVerifierEntryLightMode = idMath::ClampInt(0, 9, r_pathTracingRestirPdfNeeVerifierLightMode.GetInteger());
    const int pdfNeeVerifierEntryDomain = idMath::ClampInt(0, 2, r_pathTracingRestirPdfNeeVerifierDomain.GetInteger());
    const int pdfNeeVerifierEntryDebugMode = idMath::ClampInt(0, 57, r_pathTracingDebugMode.GetInteger());
    const int pdfNeeVerifierEntryVisibility = idMath::ClampInt(0, 1, r_pathTracingRestirPdfNeeVerifierVisibility.GetInteger());
    const int pdfNeeVerifierSelectedVisibilityPolicy = pdfNeeVerifierEntryVisibility != 0
        ? Max(1, idMath::ClampInt(0, 2, r_pathTracingRestirPTVisibilityPolicy.GetInteger()))
        : 0;
    const bool pdfNeeVerifierEntryForbiddenMode = pdfNeeVerifierEntryDebugMode == 56;
    const bool pdfNeeRluCurrentProducerRequested =
        r_pathTracingRestirPdfNeeVerifierEnable.GetInteger() != 0 &&
        !pdfNeeVerifierEntryForbiddenMode;
    const bool pdfNeeVerifierRouteRequested =
        false &&
        r_pathTracingRestirPdfNeeVerifierEnable.GetInteger() != 0 &&
        pdfNeeVerifierEntryView > 0 &&
        pdfNeeVerifierEntryLightMode != 8 &&
        pdfNeeVerifierEntryLightMode != 9 &&
        !pdfNeeVerifierEntryForbiddenMode;
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
    const bool cleanExternalPdfNeeMode9Blocked =
        pdfNeeVerifierRouteRequested &&
        cleanRtxdiDiRouteRequested &&
        cleanExternalPdfNeeRequested &&
        pdfNeeVerifierEntryLightMode == 9;
    const bool cleanExternalPdfNeeMode9Requested = false;
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
    PathTraceNeeCacheResourceDesc neeCacheDesc = BuildPathTraceNeeCacheResourceDesc(neeCacheSettings, neeCacheRluInputs);
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
    const bool cleanRestirGiNeeCacheLiveDiagnosticRefresh = false;
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
            cleanNeeCacheProviderViewStable = movementSq <= 0.25f && forwardDot >= 0.9999f;
        }
        m_smokeNeeCacheState.cleanProviderLastViewOrigin[0] = cleanProviderOrigin.x;
        m_smokeNeeCacheState.cleanProviderLastViewOrigin[1] = cleanProviderOrigin.y;
        m_smokeNeeCacheState.cleanProviderLastViewOrigin[2] = cleanProviderOrigin.z;
        m_smokeNeeCacheState.cleanProviderLastViewForward[0] = cleanProviderForward.x;
        m_smokeNeeCacheState.cleanProviderLastViewForward[1] = cleanProviderForward.y;
        m_smokeNeeCacheState.cleanProviderLastViewForward[2] = cleanProviderForward.z;
        m_smokeNeeCacheState.cleanProviderLastViewValid = true;
    }
    if (!cleanNeeCacheProviderViewStable &&
        cleanNeeCacheProviderRequestedEarly &&
        !m_smokeNeeCacheState.cleanProviderSnapshotHoldActive &&
        !cleanRestirGiNeeCacheLiveDiagnosticRefresh)
    {
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
        !cleanRestirGiNeeCacheLiveDiagnosticRefresh &&
        m_smokeNeeCacheState.cleanProviderStartupDelayFrames > 0u;
    if (cleanNeeCacheProviderStartupDelayActive && cleanNeeCacheProviderViewStable)
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
    if (regirRemixLightManagerStats.payloadSignatureChanged != 0u)
    {
        neeCacheRluInvalidationFlags |= PATH_TRACE_NEE_CACHE_INVALIDATE_RLU_PAYLOAD;
    }
    if (regirRemixLightManagerStats.payloadOnlyChange != 0u)
    {
        neeCacheRluInvalidationFlags |= PATH_TRACE_NEE_CACHE_INVALIDATE_RLU_PAYLOAD_ONLY;
    }
    if (neeCacheResourceReady && neeCacheSettings.enabled && neeCacheRluInputs.remixDenseDomain && !neeCacheSecondaryVisualSnapshotHold && !m_smokeNeeCacheState.cleanProviderSnapshotHoldActive)
    {
        m_smokeNeeCacheState.ObserveRluSignatures(
            regirRemixLightManagerStats.structuralSignature,
            regirRemixLightManagerStats.mappingSignature,
            regirRemixLightManagerStats.payloadSignature,
            neeCacheRluInvalidationFlags);
    }
    const bool neeCacheDebugForbiddenMode = pdfNeeVerifierEntryForbiddenMode;
    const bool neeCacheDebugRouteRequested =
        neeCacheSettings.enabled &&
        ((neeCacheSettings.debugView >= 1 && neeCacheSettings.debugView <= 12)) &&
        !neeCacheDebugForbiddenMode;
    const bool neeCacheSelectedSourceDomainAvailable =
        neeCacheRluInputs.remixDenseDomain &&
        neeCacheRluInputs.currentLightCount > 0u &&
        (neeCacheSettings.sourceDomain == 0 ||
            (neeCacheSettings.sourceDomain == 1 && neeCacheRluInputs.emissiveRangeCount > 0u) ||
            (neeCacheSettings.sourceDomain == 2 && neeCacheRluInputs.doomAnalyticRangeCount > 0u) ||
            (neeCacheSettings.sourceDomain == 3 && (neeCacheRluInputs.emissiveRangeCount > 0u || neeCacheRluInputs.doomAnalyticRangeCount > 0u)));
    const bool neeCacheCandidateBuildRequested =
        neeCacheSettings.enabled &&
        !neeCacheDebugForbiddenMode &&
        neeCacheResourceReady &&
        neeCacheSelectedSourceDomainAvailable;
    const bool neeCacheSecondaryConsumeRequested =
        r_pathTracingNeeCacheSecondaryEnable.GetInteger() != 0 &&
        !neeCacheDebugForbiddenMode;
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
        cleanNeeCacheProviderViewStable &&
        m_smokeNeeCacheState.cleanProviderStartupRefreshFrames > 0u;
    const bool cleanNeeCacheProviderBuildPrepassRequested =
        cleanNeeCacheProviderRequestedEarly &&
        !neeCacheSecondaryVisualBandActive &&
        neeCacheCandidateBuildRequested &&
        (!cleanNeeCacheProviderStartupDelayActive ||
            cleanRestirGiNeeCacheLiveDiagnosticRefresh) &&
        (!m_smokeNeeCacheState.cleanProviderSnapshotHoldActive ||
            cleanNeeCacheProviderStartupRefreshActive ||
            cleanRestirGiNeeCacheLiveDiagnosticRefresh) &&
        !neeCacheSecondaryVisualSnapshotHold;
    const bool cleanNeeCacheBuildPrepassRequested =
        cleanNeeCacheProviderBuildPrepassRequested ||
        neeCacheSecondaryVisualRefreshRequested;
    const bool neeCacheRouteRequested = neeCacheDebugRouteRequested || neeCacheCandidateBuildRequested;
    auto printNeeCacheDump = [&](const char* stage, const char* earlyReturn)
    {
        const std::vector<PathTraceUnifiedLightRecord>& neeCacheCpuPayloads = m_remixLightManager.GetCurrentLightPayloads();
        const uint32_t neeCacheFirstEmissiveDenseIndex = neeCacheRluInputs.emissiveRangeOffset;
        const bool neeCacheFirstEmissivePayloadValid =
            neeCacheFirstEmissiveDenseIndex < static_cast<uint32_t>(neeCacheCpuPayloads.size());
        const PathTraceUnifiedLightRecord* neeCacheFirstEmissivePayload =
            neeCacheFirstEmissivePayloadValid ? &neeCacheCpuPayloads[neeCacheFirstEmissiveDenseIndex] : nullptr;
        const bool neeCacheBuffersReady =
            m_smokeNeeCacheState.providerResultBuffer &&
            m_smokeNeeCacheState.cellBuffer &&
            m_smokeNeeCacheState.taskBuffer &&
            m_smokeNeeCacheState.candidateBuffer;
        const bool neeCacheDebugRouteReady =
            neeCacheDebugRouteRequested &&
            m_smokeNeeCacheDebugBindingLayout &&
            m_smokeNeeCacheDebugShaderTable &&
            neeCacheBuffersReady;
        const char* firstMissingContract = neeCacheDesc.firstMissingContract;
        if (earlyReturn && idStr::Icmp(earlyReturn, "none") != 0 && neeCacheDesc.requested)
        {
            firstMissingContract = earlyReturn;
        }
        if (!neeCacheResourceReady && neeCacheDesc.requested && neeCacheDesc.structuralValid)
        {
            firstMissingContract = "nee-cache-provider-buffers";
        }
        if (neeCacheDebugRouteReady && neeCacheRluInputs.remixDenseDomain && neeCacheRluInputs.currentLightCount > 0u)
        {
            firstMissingContract = "none";
        }
        if (neeCacheDebugRouteRequested && !neeCacheBuffersReady)
        {
            firstMissingContract = "nee-cache-provider-buffers";
        }
        if (neeCacheDebugRouteRequested && !m_smokeNeeCacheDebugShaderTable)
        {
            firstMissingContract = "nee-cache-debug-shader";
        }
        if (neeCacheDebugRouteRequested && !m_smokeNeeCacheDebugBindingLayout)
        {
            firstMissingContract = "nee-cache-debug-binding-layout";
        }
        if (cleanNeeCacheBuildPrepassRequested &&
            (!m_smokeNeeCachePrimarySurfaceUpdatePipeline || !m_smokeNeeCachePrimarySurfaceUpdateBindingLayout))
        {
            firstMissingContract = "nee-cache-primary-surface-update-shader";
        }
        if (neeCacheDebugForbiddenMode && neeCacheSettings.debugView > 0)
        {
            firstMissingContract = "forbidden-mode-56";
        }
        common->Printf(
            "PathTracePrimaryPass: NEE cache provider shell dump stage=%s earlyReturn=%s enable=%d mode=%d(%s) debugView=%d debugRoute=%d candidateBuild=%d cellResolution=%d minRange=%.2f cellCount=%u candidateSlots=%u taskSlots=%u fallbackProbability=%.3f cacheProbability=%.3f sourceDomain=%d(%s) rluDense=%d rluCurrent=%u rluRanges emissive=%u+%u doomAnalytic=%u+%u rluAnalyticStability currentSampleable/stableCacheable/unstableDynamic=%u/%u/%u reject noRemap/payloadChanged/unprovenContinuity/unknownIdentity/duplicateIdentity/portalDisconnected/outOfSelectedArea=%u/%u/%u/%u/%u/%u/%u shaderActiveRanges emissive=%d+%d doomAnalytic=%d+%d rluNonEmptyRanges=%u cpuPayloads current=%u firstEmissivePayload dense=%u valid=%d type=%u sourceIndex=%u sourcePdf=%.8f sourceWeight=%.8f luminance=%.8f abiOwner=PathTraceNeeCache shaderStruct=PathTraceNeeCacheProviderResult cppStruct=PathTraceNeeCacheProviderResult bindingSlots currentRluPayloadSrv=t66 rabBridgeSrvs=t16,t27,t42,t43,t44,t45,t57,t58,t59,t60,t61,t64,t65,t67 geometrySrvs=t3,t4,t6,t7,t22,t23,t26 providerResultUav=u%u cellUav=u%u taskUav=u%u candidateUav=u%u providerFunction=%s futurePdfNeeBoundary=%s resultStride=%u cellStride=%u taskStride=%u candidateStride=%u resultCount=%u taskCount=%u candidateCount=%u bytes result/cell/task/candidate/total=%llu/%llu/%llu/%llu/%llu buffersReady=%d allocationSerial=%llu taskClearPending=%d rluSignatures structural/mapping/payload=%llu/%llu/%llu rluSignatureChanged structural/mapping/payload/payloadOnly=%u/%u/%u/%u cacheInvalidation pending/last=0x%x/0x%x cacheInvalidationSerial=%llu taskInsertPolicy=debug-primary-visible-hit-slot0-atomic-count taskDecayPolicy=full-cell-prepass-decay-7of8 taskResetPolicy=clear-provider-cell-task-candidate-on-allocation-scene-reset-or-rlu-structural-mapping-payload-or-payloadOnly-change candidateInvalidationPolicy=clear-provider-cell-task-candidate-on-rlu-structural-mapping-payload-or-payloadOnly-change candidateBuildPolicy=primary-visible-hit-persistent-slot-update-bounded-ris-over-source-domain candidateAdmissionPolicy=stable-identity-slot-refresh-duplicates-fill-empty-replace-target-only-if-2x-stronger candidateDebugViewPolicy=views5-10-read-built-cache-only-build-prepass-writes candidateWeightPolicy=build:bounded-ris-cell-importance;select:current-rlu-rab-replay-and-reweight candidateSelectionPolicy=current-rab-replay-valid-weighted-fixed-slot-selection-sum-duplicate-identity-pdf candidateValidityPolicy=reject-zero-area-or-zero-energy-emissive,reject-analytic-not-rlu-stableCacheable,analytic-zero-cell-weight,provider-rejects-rab-replay-failed-or-zero-current-weight candidateSource=current-rlu-sourceDomain-controlled candidateIdentity=dense-current-rlu-index providerResultWrite=PathTraceNeeCacheProviderResults[cellIndex] candidateSlotGenerationPdf=ris-selected-sourcePdf,domain0:bounded-ris-proportional-typed-emissive-or-full-analytic-range,domain1:bounded-ris-emissive-range,domain2:bounded-ris-full-analytic-range-filter-stable,domain3:bounded-ris-typed-stable-mixture providerSourcePdf=cache:currentReplayWeightedCandidateIdentityPdf*cacheProbability,fallback:domainPdf*fallbackProbability candidateInvSourcePdf=1/sourcePdf flatReplay=RAB_LoadActiveRrxLightInfo,RAB_SampleActiveRrxPolymorphicLight,RAB_GetLightSampleTargetPdfForSurface,RAB_GetReflectedBsdfRadianceForSurface sourceLabelEnum=none,cache-analytic,cache-emissive,fallback-full-rlu,fallback-typed-rlu fallbackReasonEnum=none,disabled,no-rlu,empty-cell,invalid-candidate,zero-source-pdf,rab-replay-failed,cache-only-diagnostic selectedDenseCurrentIndex=result.selectedDenseRluIndex sourcePdf=result.sourcePdf invSourcePdf=result.invSourcePdf mixtureProbability=result.mixtureProbability cellFrame=world-anchored-fixed-lod cellMapping=fixed-world-cell-hash debugViews=1:route-status,2:cell-id,3:empty-occupancy,4:task-accumulation,5:emissive-candidate-map,6:analytic-candidate-identity,7:source-pdf,8:cache-fallback-source,9:fallback-reason,10:rlu-payload-replay-validity,11:flat-consumed-candidates,12:flat-full-current-rlu noCandidateReads=%d output=%s consumer=%s finalContribution=0 temporal=0 spatial=0 bestLights=0 mode56=0 oldPdfNee=0 firstMissingContract=%s task=%s\n",
            stage ? stage : "unknown",
            earlyReturn ? earlyReturn : "none",
            neeCacheSettings.enabled ? 1 : 0,
            neeCacheSettings.mode,
            PathTraceNeeCacheModeName(neeCacheSettings.mode),
            neeCacheSettings.debugView,
            neeCacheDebugRouteRequested ? 1 : 0,
            neeCacheCandidateBuildRequested ? 1 : 0,
            neeCacheSettings.cellResolution,
            neeCacheSettings.minRange,
            neeCacheSettings.cellCount,
            neeCacheSettings.candidateSlots,
            neeCacheSettings.taskSlots,
            neeCacheSettings.fallbackProbability,
            1.0f - neeCacheSettings.fallbackProbability,
            neeCacheSettings.sourceDomain,
            PathTraceNeeCacheSourceDomainName(neeCacheSettings.sourceDomain),
            neeCacheRluInputs.remixDenseDomain ? 1 : 0,
            neeCacheRluInputs.currentLightCount,
            neeCacheRluInputs.emissiveRangeOffset,
            neeCacheRluInputs.emissiveRangeCount,
            neeCacheRluInputs.doomAnalyticRangeOffset,
            neeCacheRluInputs.doomAnalyticRangeCount,
            regirRemixLightManagerStats.doomAnalyticCurrentSampleableCount,
            regirRemixLightManagerStats.doomAnalyticStableCacheableCount,
            regirRemixLightManagerStats.doomAnalyticUnstableDynamicCount,
            regirRemixLightManagerStats.doomAnalyticRejectNoRemapCount,
            regirRemixLightManagerStats.doomAnalyticRejectPayloadChangedCount,
            regirRemixLightManagerStats.doomAnalyticRejectUnprovenContinuityCount,
            regirRemixLightManagerStats.doomAnalyticRejectUnknownIdentityCount,
            regirRemixLightManagerStats.doomAnalyticRejectDuplicateIdentityCount,
            regirRemixLightManagerStats.doomAnalyticRejectPortalDisconnectedCount,
            regirRemixLightManagerStats.doomAnalyticRejectOutOfSelectedAreaCount,
            static_cast<int>(neeCacheRluInputs.emissiveRangeOffset),
            static_cast<int>(neeCacheCandidateBuildRequested ? neeCacheRluInputs.emissiveRangeCount : 0u),
            static_cast<int>(neeCacheRluInputs.doomAnalyticRangeOffset),
            static_cast<int>(neeCacheCandidateBuildRequested
                ? std::min(neeCacheRluInputs.doomAnalyticRangeCount, regirRemixLightManagerStats.doomAnalyticStableCacheableCount)
                : 0u),
            neeCacheRluInputs.nonEmptyRangeCount,
            static_cast<uint32_t>(neeCacheCpuPayloads.size()),
            neeCacheFirstEmissiveDenseIndex,
            neeCacheFirstEmissivePayloadValid ? 1 : 0,
            neeCacheFirstEmissivePayload ? neeCacheFirstEmissivePayload->type : PATH_TRACE_UNIFIED_LIGHT_TYPE_INVALID,
            neeCacheFirstEmissivePayload ? neeCacheFirstEmissivePayload->sourceIndex : PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX,
            neeCacheFirstEmissivePayload ? neeCacheFirstEmissivePayload->sourcePdf : 0.0f,
            neeCacheFirstEmissivePayload ? neeCacheFirstEmissivePayload->sourceWeight : 0.0f,
            neeCacheFirstEmissivePayload ? neeCacheFirstEmissivePayload->radianceAndLuminance[3] : 0.0f,
            PATH_TRACE_NEE_CACHE_BINDING_PROVIDER_RESULT_UAV,
            PATH_TRACE_NEE_CACHE_BINDING_CELL_UAV,
            PATH_TRACE_NEE_CACHE_BINDING_TASK_UAV,
            PATH_TRACE_NEE_CACHE_BINDING_CANDIDATE_UAV,
            PathTraceNeeCacheProviderFunctionName(),
            PathTraceNeeCacheFuturePdfNeeBoundaryName(),
            neeCacheDesc.providerResultStride,
            neeCacheDesc.cellStride,
            neeCacheDesc.taskStride,
            neeCacheDesc.candidateStride,
            neeCacheDesc.providerResultCount,
            neeCacheDesc.taskCount,
            neeCacheDesc.candidateCount,
            static_cast<unsigned long long>(neeCacheDesc.providerResultBytes),
            static_cast<unsigned long long>(neeCacheDesc.cellBytes),
            static_cast<unsigned long long>(neeCacheDesc.taskBytes),
            static_cast<unsigned long long>(neeCacheDesc.candidateBytes),
            static_cast<unsigned long long>(neeCacheDesc.totalBytes),
            neeCacheBuffersReady ? 1 : 0,
            static_cast<unsigned long long>(m_smokeNeeCacheState.allocationSerial),
            m_smokeNeeCacheState.taskClearPending ? 1 : 0,
            static_cast<unsigned long long>(regirRemixLightManagerStats.structuralSignature),
            static_cast<unsigned long long>(regirRemixLightManagerStats.mappingSignature),
            static_cast<unsigned long long>(regirRemixLightManagerStats.payloadSignature),
            regirRemixLightManagerStats.structuralSignatureChanged,
            regirRemixLightManagerStats.mappingSignatureChanged,
            regirRemixLightManagerStats.payloadSignatureChanged,
            regirRemixLightManagerStats.payloadOnlyChange,
            m_smokeNeeCacheState.pendingInvalidationFlags,
            m_smokeNeeCacheState.lastInvalidationFlags,
            static_cast<unsigned long long>(m_smokeNeeCacheState.invalidationSerial),
            neeCacheSettings.debugView >= 5 ? 0 : 1,
            neeCacheDebugRouteRequested ? "SmokeOutput" : "none",
            neeCacheDebugRouteRequested ? "nee-cache-debug-route" : "none",
            firstMissingContract,
            neeCacheDebugRouteRequested
                ? ((neeCacheSettings.debugView == 8 || neeCacheSettings.debugView == 9) ? "NEECACHE-06" : ((neeCacheSettings.debugView == 6 || neeCacheSettings.debugView == 7 || neeCacheSettings.debugView == 10 || neeCacheSettings.debugView == 11 || neeCacheSettings.debugView == 12) ? "NEECACHE-05" : (neeCacheSettings.debugView == 5 ? "NEECACHE-04" : (neeCacheSettings.debugView == 4 ? "NEECACHE-03" : "NEECACHE-02"))))
                : "NEECACHE-01");
    };
    const bool regirDebugForbiddenMode = pdfNeeVerifierEntryDebugMode == 56;
    const bool regirDebugRouteRequested =
        regirSettings.enabled &&
        (regirSettings.debugView >= 1 && regirSettings.debugView <= 10) &&
        !regirDebugForbiddenMode;
    const bool standaloneDebugRouteRequested = regirDebugRouteRequested || neeCacheDebugRouteRequested;
    const bool pdfNeeReGIRSourceRouteRequested = false;
    const bool pdfNeeReGIRSourceRouteBlocked =
        pdfNeeVerifierRouteRequested &&
        pdfNeeVerifierEntryLightMode == 9;
    const bool pdfNeeReGIRBuildPrepassRequested =
        pdfNeeReGIRSourceRouteRequested &&
        regirSettings.enabled &&
        regirResourceReady &&
        m_smokeReGIRState.candidateCacheBuffer;
    const idVec3 regirResolvedCenter = ResolvePathTraceReGIRCenter(m_smokeGeometryUniverse, regirSettings, m_smokeSceneOrigin);
    auto printReGIRDump = [&](const char* stage, const char* earlyReturn)
    {
        const bool regirCandidateDebugView =
            regirSettings.debugView >= 4 && regirSettings.debugView <= 10;
        const char* regirSourceDistribution =
            regirSettings.debugView == 9
                ? (regirUseCurrentRabLightUniverse
                    ? "view9DeterministicDenseSource:slotClass=deterministicParity,selectedIndex=hash(cell,slot)%selectedClassRangeCount,noRadianceOrSourceWeightRIS,sourcePdf=classMass/rangeCount,globalIdentity=RABLightManagerDenseIndex"
                    : "rluRequired:legacyFallbackDisabled,emptyReason=no-current-rlu-source")
            : regirUseCurrentRabLightUniverse
                ? (regirSettings.lightDomain == 0 ? "analyticBoundedRIS:proposalCount=min(RLU.doomAnalyticSampleCount,currentRabDoomAnalyticRangeCount),proposalInvPdf=rangeCount/proposalCount,stableCellInfluenceReservoir(noPayloadRadianceOrSourceWeight),storedInvSourcePdf=risWeightSum/selectedCellWeight,sourcePdf=1/storedInvSourcePdf,globalIdentity=RABLightManagerDenseIndex" :
                    (regirSettings.lightDomain == 1 ? "emissiveBoundedRIS:proposalCount=min(RLU.emissiveSampleCount,currentRabEmissiveRangeCount),proposalInvPdf=rangeCount/proposalCount,stableCellInfluenceReservoir(noPayloadRadianceOrSourceWeight),storedInvSourcePdf=risWeightSum/selectedCellWeight,sourcePdf=1/storedInvSourcePdf,globalIdentity=RABLightManagerDenseIndex" :
                    "splitBoundedRIS:slotClass=deterministicParity,analyticClassMass=analyticSlotCount/lightsPerCell,emissiveClassMass=emissiveSlotCount/lightsPerCell,singlePresentClassMass=1,stableSlot=cellHash%lightsPerCellNoFallback,proposalCount=min(RLU.selectedTypeSampleCount,selectedClassRangeCount),proposalInvPdf=rangeCount/(classMass*proposalCount),stableCellInfluenceReservoir(noPayloadRadianceOrSourceWeight),storedInvSourcePdf=risWeightSum/selectedCellWeight,sourcePdf=1/storedInvSourcePdf,consumerMustUseSameSlotClassMass,globalIdentity=RABLightManagerDenseIndex"))
                : (regirLegacyFallbackDisabled ? "rluRequired:legacyFallbackDisabled,emptyReason=no-current-rlu-source" : (regirSettings.lightDomain == 0 ? "analyticBoundedRIS:proposalCount=min(buildSamples,currentDoomAnalyticCount),proposalInvPdf=currentDoomAnalyticCount/proposalCount,cellWeightReservoir,storedInvSourcePdf=risWeightSum/selectedCellWeight,sourcePdf=1/storedInvSourcePdf,globalIdentity=emissiveCount+analyticIndex" :
                    (regirSettings.lightDomain == 1 ? "emissiveBoundedRIS:proposalCount=min(buildSamples,currentEmissiveTriangleCount),proposalInvPdf=currentEmissiveTriangleCount/proposalCount,cellWeightReservoir,storedInvSourcePdf=risWeightSum/selectedCellWeight,sourcePdf=1/storedInvSourcePdf,globalIdentity=emissiveIndex" :
                    "splitBoundedRIS:slotClass=deterministicParity,analyticClassMass=analyticSlotCount/lightsPerCell,emissiveClassMass=emissiveSlotCount/lightsPerCell,singlePresentClassMass=1,stableSlot=cellHash%lightsPerCellNoFallback,proposalCount=min(buildSamples,selectedClassCount),proposalInvPdf=selectedClassCount/(classMass*proposalCount),cellWeightReservoir,storedInvSourcePdf=risWeightSum/selectedCellWeight,sourcePdf=1/storedInvSourcePdf,consumerMustUseSameSlotClassMass,globalIdentity=RABSplitIndex")));
        const char* firstMissingContract =
            regirDebugForbiddenMode && regirSettings.debugView > 0 ? "forbidden-mode-56" :
            regirLegacyFallbackDisabled && !regirUseCurrentRabLightUniverse ? "current-remix-rab-light-universe" :
            !regirResourceReady && regirDesc.requested && regirDesc.structuralValid ? "candidate-cache-buffer" :
            (earlyReturn && idStr::Icmp(earlyReturn, "none") != 0 && regirDesc.requested ? earlyReturn : regirDesc.firstMissingContract);
        common->Printf(
            "PathTracePrimaryPass: ReGIR clean-room shell dump stage=%s earlyReturn=%s enable=%d debugView=%d debugRoute=%d mode=%d(%s) centerMode=%d(%s) centerPolicy=%s center=(%.2f,%.2f,%.2f) cellSize=%.2f grid=%ux%ux%u cellCount=%u lightsPerCell=%u buildSamples=%u lightDomain=%d(%s) lightSource=%s legacyFallback=%s rluCurrent=%u rluRanges emissive=%u+%u doomAnalytic=%u+%u rluSamples emissive/doom/total/nonEmpty=%u/%u/%u/%u selectedDenseCurrentIndex=shaderCandidate.lightIndex selectedType=shaderCandidate.lightClassFromRLUPayload counts analytic=%u emissive=%u split=%u unified=%u candidateStride=%u candidateSlots=%u candidateBytes=%llu bufferReady=%d allocationSerial=%llu dispatchSlots=%u selectedSlotPolicy=view9:cellHash%%lightsPerCell-no-frameIndex consumerOverrides=none sourceDistribution=%s rabReplay=view7/view10:primaryHitSurface+RAB_LoadActiveRrxLightInfo+RAB_SamplePolymorphicLight+RAB_GetLightSampleTargetPdfForSurface firstMissingContract=%s forbiddenPdfNee=0 temporal=0 spatial=0 bestLights=0 mode56=%d rrxPages=0 output=%s task=%s\n",
            stage ? stage : "unknown",
            earlyReturn ? earlyReturn : "none",
            regirSettings.enabled ? 1 : 0,
            regirSettings.debugView,
            regirDebugRouteRequested ? 1 : 0,
            regirSettings.mode,
            PathTraceReGIRModeName(regirSettings.mode),
            regirSettings.centerMode,
            PathTraceReGIRCenterModeName(regirSettings.centerMode),
            PathTraceReGIRCenterPolicyName(regirSettings),
            regirResolvedCenter.x,
            regirResolvedCenter.y,
            regirResolvedCenter.z,
            regirSettings.cellSize,
            regirSettings.gridX,
            regirSettings.gridY,
            regirSettings.gridZ,
            regirDesc.cellCount,
            regirSettings.lightsPerCell,
            regirSettings.buildSamples,
            regirSettings.lightDomain,
            PathTraceReGIRLightDomainName(regirSettings.lightDomain),
            regirUseCurrentRabLightUniverse ? "current-remix-rab-light-universe" : (regirLegacyFallbackDisabled ? "current-remix-rab-light-universe-unavailable" : "legacy-split-current-light-buffers"),
            regirLegacyFallbackDisabled ? "disabled" : "available-for-cell-only-debug-views",
            regirRemixLightManagerStats.currentLightCount,
            regirRemixLightManagerStats.emissiveRangeOffset,
            regirRemixLightManagerStats.emissiveRangeCount,
            regirRemixLightManagerStats.doomAnalyticRangeOffset,
            regirRemixLightManagerStats.doomAnalyticRangeCount,
            regirRemixLightManagerStats.emissiveSampleCount,
            regirRemixLightManagerStats.doomAnalyticSampleCount,
            regirRemixLightManagerStats.totalSampleCount,
            regirRemixLightManagerStats.nonEmptyRangeCount,
            regirLightCounts.analyticCount,
            regirLightCounts.emissiveCount,
            regirLightCounts.analyticCount + regirLightCounts.emissiveCount,
            regirLightCounts.unifiedCount,
            regirDesc.candidateStride,
            regirDesc.slotCount,
            static_cast<unsigned long long>(regirDesc.candidateBytes),
            m_smokeReGIRState.candidateCacheBuffer ? 1 : 0,
            static_cast<unsigned long long>(m_smokeReGIRState.allocationSerial),
            regirDesc.slotCount,
            regirSourceDistribution,
            firstMissingContract,
            regirDebugForbiddenMode ? 1 : 0,
            regirDebugRouteRequested ? "SmokeOutput" : "none",
            regirDebugRouteRequested
                ? ((regirSettings.debugView == 7 || regirSettings.debugView == 9 || regirSettings.debugView == 10)
                    ? "REGIR-08"
                    : (regirSettings.lightDomain == 2 && regirCandidateDebugView ? "REGIR-05" : (regirSettings.lightDomain == 1 && regirCandidateDebugView ? "REGIR-04" : (regirCandidateDebugView ? "REGIR-03" : "REGIR-02"))))
                : "REGIR-01");
    };
    auto printCleanRtxdiDiDump = [&](const char* stage, const char* earlyReturn, int selectedCleanShaderTable)
    {
        const bool cleanEnabledNow = r_pathTracingCleanRtxdiDiEnable.GetInteger() != 0;
        const int cleanViewNow = cleanEnabledNow ? r_pathTracingCleanRtxdiDiView.GetInteger() : 0;
        const bool cleanRouteNow = cleanEnabledNow && cleanViewNow >= 1 && cleanViewNow <= 24;
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
            (m_smokeNeeCacheState.cleanProviderSnapshotHoldActive ||
                cleanRestirGiNeeCacheLiveDiagnosticRefresh) &&
            m_smokeNeeCacheState.providerResultBuffer != nullptr &&
            m_smokeNeeCacheState.cellBuffer != nullptr &&
            m_smokeNeeCacheState.candidateBuffer != nullptr;
        const PathTraceRemixLightEventSample cleanDumpEmptyRluSample;
        const PathTraceRemixLightEventSample& cleanDumpPayloadCurrent = cleanDumpRluRoute ? cleanDumpRluStats.firstPayloadChangedCurrent : cleanDumpEmptyRluSample;
        const PathTraceRemixLightEventSample& cleanDumpPayloadPrevious = cleanDumpRluRoute ? cleanDumpRluStats.firstPayloadChangedPrevious : cleanDumpEmptyRluSample;
        const PathTraceRemixLightEventSample& cleanDumpCurrentOnly = cleanDumpRluRoute ? cleanDumpRluStats.firstCurrentOnly : cleanDumpEmptyRluSample;
        const PathTraceRemixLightEventSample& cleanDumpPreviousOnly = cleanDumpRluRoute ? cleanDumpRluStats.firstPreviousOnly : cleanDumpEmptyRluSample;
        common->Printf(
            "PathTracePrimaryPass: clean-room RTXDI DI dump stage=%s earlyReturn=%s enable=%d view=%d temporal=%d spatial=%d bestLights=%d denoiser=%d fallback=%d lightMode=%d doomRadiusCutoff=%d frameFreeze=%d analyticDomainFreezeMs=%d bypassLightUniverse=%d doomColorSource=%d requireProvenDoomLights=%d temporalBiasCorrection=%d temporalMaxHistory=%d candidateOverride=%u view8Band=%d resolveVisibilityMode=%d resolveSolidAnglePdf=%d initialVisibility=%d resolveBrdfTarget=%d referenceRab=%d view10LightStart=%d view10LightCount=%d view10PortalDomain=%d cleanCandidates=%u remixLightUniverseRoute=%d rlu current/previous/currentToPrevious/previousToCurrent=%u/%u/%u/%u rluRanges emissive=%u+%u doomAnalytic=%u+%u rluLocal payloadChangedMapped/currentOnly/previousOnly/duplicates=%u/%u/%u/%u rluFirstPayload currentIndex/previousIndex/type/light/ids/currentXYZR/previousXYZR/currentLum/previousLum=%u/%u/%u/%u/%u:%u/%.1f:%.1f:%.1f:%.1f/%.1f:%.1f:%.1f:%.1f/%.3f/%.3f rluFirstCurrentOnly index/type/light/ids/xyzr/lum=%u/%u/%u/%u:%u/%.1f:%.1f:%.1f:%.1f/%.3f rluFirstPreviousOnly index/type/light/ids/xyzr/lum=%u/%u/%u/%u:%u/%.1f:%.1f:%.1f:%.1f/%.3f externalPdfNeeCurrent=%d cleanToyEmissiveScale=%.3f cleanAnalyticScale=%.3f cleanUseEmissiveMaps=%d cleanDisableEmissiveTriangles=%d cleanAnalyticCandidates=%d cleanNeeCacheProvider requested/ready=%d/%d buildPrepass=%d startupDelay=%u startupRefresh=%u stableView=%d stableFrames=%u deferredByClear=%d providerSnapshotHold=%d band10ExitedClear=%d providerResultSrv=t74 cellSrv=t75 candidateSrv=t77 fallbackProbability=%.3f sourceDomain=%d(%s) cellResolution=%d cellFrame=world-anchored-fixed-lod cleanNeeCacheProducer=nee-cache-stable-view-delayed-refresh-burst-or-existing-clean-initial,RTXDI_StreamSample,RTXDI_FinalizeResampling providerMissFallback=existing-clean-initial externalPdfNeeCleanIndexBase=%d externalPdfNeeRequestedLightMode=%d cleanExternalPdfNeeMode9=%d cleanReGIR enable=%d mode=%d centerMode=%d cellSize=%.2f grid=%ux%ux%u lightsPerCell=%u buildSamples=%u candidateSlots=%u firstMissing=%s route=%s behavior=%s output=%dx%d viewDef subview=%d mirror=%d superView=%d area=%d drawSurfs=%d sceneBuilt=%d coreShader=%d cleanShader=%d selectedCleanShader=%d bindingSet=%d textureTable=%d outputTex=%d accumulation=%d readback=%d cleanCurrentAnalytic=%d cleanPortalAnalytic=%d cleanCurrentAnalyticIdentity=%d cleanPreviousAnalytic=%d cleanPreviousAnalyticIdentity=%d cleanAnalyticRemap=%d cleanCurrentReservoir=%d cleanTemporalReservoir=%d cleanPreviousReservoir=%d cleanSpatialReservoir=%d cleanPreviousReservoirValid=%d cleanPreviousResetReason=%u cleanHistoryResetCount=%u cleanHistorySignature=%llu commandList=%d pages current=%s temporal=%s previous=%s spatial=%s spatialParams samples/disocclusion/radius=%d/%d/%.1f\n",
            stage ? stage : "unknown",
            earlyReturn ? earlyReturn : "none",
            cleanEnabledNow ? 1 : 0,
            cleanViewNow,
            r_pathTracingCleanRtxdiDiTemporal.GetInteger(),
            cleanRtxdiDiSpatialEnabled ? 1 : 0,
            r_pathTracingCleanRtxdiDiBestLights.GetInteger(),
            r_pathTracingCleanRtxdiDiDenoiser.GetInteger(),
            r_pathTracingCleanRtxdiDiFallbackLighting.GetInteger(),
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
            0,
            pdfNeeVerifierEntryLightMode,
            cleanExternalPdfNeeMode9Blocked ? 1 : 0,
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
    };
    auto printNeeCacheSecondaryDump = [&]()
    {
        const PathTraceIntegratorSettings secondaryDumpIntegratorSettings = BuildPathTraceIntegratorSettings();
        const char* firstMissingContract = "none";
        if (!neeCacheSecondaryConsumeRequested)
        {
            firstMissingContract = "secondary-consumer-disabled";
        }
        else if (!neeCacheSettings.enabled)
        {
            firstMissingContract = "nee-cache-disabled";
        }
        else if (!neeCacheResourceReady)
        {
            firstMissingContract = "nee-cache-provider-buffers";
        }
        else if (!neeCacheRluInputs.remixDenseDomain)
        {
            firstMissingContract = "current-rlu-dense-domain";
        }
        else if (!neeCacheCandidateBuildRequested)
        {
            firstMissingContract = "nee-cache-candidate-build-not-ready";
        }
        else if (!neeCacheSecondaryEmissiveDomainAvailable)
        {
            firstMissingContract = "no-emissive-secondary-consume-domain";
        }
        else if (!m_smokeNeeCacheState.providerResultBuffer || !m_smokeNeeCacheState.cellBuffer || !m_smokeNeeCacheState.candidateBuffer)
        {
            firstMissingContract = "nee-cache-provider-srvs";
        }
        common->Printf(
            "PathTracePrimaryPass: NEE cache secondary consumer route enable=%d requestedEnable=%d nextEvent=%d maxDepth=%d diffuseBounce=%d specularBounce=%d reflectionMode=%d secondaryNeeMode=%d secondaryAnalyticNeeMode=%d secondaryVisibility=%d providerReady=%d candidateBuild=%d debugRoute=%d visualRefresh=%d visualSnapshotHold=%d sourceDomain=%d(%s) rluDense=%d rluCurrent=%u rluRanges emissive=%u+%u doomAnalytic=%u+%u stableAnalytic=%u candidateSlots=%u cellResolution=%d fallbackProbability=%.3f bindingSlots providerResultSrv=t74 cellSrv=t75 candidateSrv=t77 consumeBoundary=RAB_RecordSmokeNeeSample/ReSTIR-PT-GenerateInitialSamples owner=ReSTIR-PT-path-tracer-NEE proposalSource=nee-cache-typed-emissive-and-analytic-candidate-slots replay=RAB_LoadLightInfo,RAB_SamplePolymorphicLight,RAB_GetReflectedBsdfRadianceForSurface,RAB_GetSelectedNeeVisibility pdf=(cacheProbability*weighted-cell-identity-pdf+fallbackProbability*domain-identity-pdf)*solidAnglePdf misInput=RAB_GetMISWeightForNEELight fallback=internal-domain-mixture-or-existing-RAB_RecordSmokeNeeSample-path-on-provider-miss mode18=0 debugView=clean-di-view8-band10-secondary-typed-candidate-field validation=clean-view8-band10-route-diagnostic-plus-normal-render-ab primaryDI=unchanged cleanDI=unchanged temporal=0 spatial=0 bestLights=0 finalCachedRadiance=0 firstMissingContract=%s task=NEECACHE-10\n",
            idStr::Icmp(firstMissingContract, "none") == 0 ? 1 : 0,
            neeCacheSecondaryConsumeRequested ? 1 : 0,
            secondaryDumpIntegratorSettings.nextEventEstimation,
            secondaryDumpIntegratorSettings.maxPathDepth,
            secondaryDumpIntegratorSettings.diffuseBounceLimit,
            secondaryDumpIntegratorSettings.specularBounceLimit,
            secondaryDumpIntegratorSettings.reflectionMode,
            secondaryDumpIntegratorSettings.secondaryNeeMode,
            secondaryDumpIntegratorSettings.secondaryAnalyticNeeMode,
            secondaryDumpIntegratorSettings.secondaryNeeVisibility,
            neeCacheSecondaryConsumeReady ? 1 : 0,
            neeCacheCandidateBuildRequested ? 1 : 0,
            neeCacheDebugRouteRequested ? 1 : 0,
            neeCacheSecondaryVisualRefresh,
            m_smokeNeeCacheState.secondaryVisualSnapshotHoldActive ? 1 : 0,
            neeCacheSettings.sourceDomain,
            PathTraceNeeCacheSourceDomainName(neeCacheSettings.sourceDomain),
            neeCacheRluInputs.remixDenseDomain ? 1 : 0,
            neeCacheRluInputs.currentLightCount,
            neeCacheRluInputs.emissiveRangeOffset,
            neeCacheRluInputs.emissiveRangeCount,
            neeCacheRluInputs.doomAnalyticRangeOffset,
            neeCacheRluInputs.doomAnalyticRangeCount,
            regirRemixLightManagerStats.doomAnalyticStableCacheableCount,
            neeCacheSettings.candidateSlots,
            neeCacheSettings.cellResolution,
            neeCacheSettings.fallbackProbability,
            firstMissingContract);
    };
    if (cleanRtxdiDiDumpRequested && cleanRtxdiDiEnabled && !cleanRtxdiDiRouteRequested)
    {
        printCleanRtxdiDiDump("dispatch-entry", "clean-view-out-of-range", 0);
        r_pathTracingCleanRtxdiDiDump.SetInteger(0);
    }
    if (cleanExternalPdfNeeMode9Blocked)
    {
        if (cleanRtxdiDiDumpRequested)
        {
            printCleanRtxdiDiDump("dispatch-entry", "regir-v06-standalone-required", 0);
            r_pathTracingCleanRtxdiDiDump.SetInteger(0);
        }
        return;
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
    auto printPdfNeeVerifierDump = [&](const char* stage, const char* earlyReturn)
    {
        if (pdfNeeRluCurrentProducerRequested)
        {
            const int managerCount = static_cast<int>(regirRemixLightManagerStats.currentLightCount);
            const int sourcePolicy = idMath::ClampInt(0, 2, r_pathTracingRestirPdfNeeVerifierSourcePolicy.GetInteger());
            const bool neeCacheProviderRequested = sourcePolicy == 2;
            const bool neeCacheProviderReady =
                neeCacheProviderRequested &&
                neeCacheResourceReady &&
                neeCacheCandidateBuildRequested &&
                m_smokeNeeCacheState.providerResultBuffer != nullptr &&
                m_smokeNeeCacheState.cellBuffer != nullptr &&
                m_smokeNeeCacheState.candidateBuffer != nullptr;
            const bool typedPolicy =
                sourcePolicy == 1 &&
                (regirRemixLightManagerStats.emissiveRangeCount > 0u || regirRemixLightManagerStats.doomAnalyticRangeCount > 0u);
            const float emissiveClassProbability = typedPolicy && regirRemixLightManagerStats.emissiveRangeCount > 0u
                ? (regirRemixLightManagerStats.doomAnalyticRangeCount > 0u ? 0.5f : 1.0f)
                : 0.0f;
            const float doomAnalyticClassProbability = typedPolicy && regirRemixLightManagerStats.doomAnalyticRangeCount > 0u
                ? (regirRemixLightManagerStats.emissiveRangeCount > 0u ? 0.5f : 1.0f)
                : 0.0f;
            const char* firstMissingContract =
                pdfNeeVerifierEntryForbiddenMode ? "forbidden-mode-56" :
                (earlyReturn && idStr::Icmp(earlyReturn, "none") != 0 ? earlyReturn :
                (neeCacheProviderRequested && !neeCacheProviderReady ? "nee-cache-provider-not-ready-neecache-07" :
                (managerCount <= 0 ? "current-rlu-dense-domain" : "none")));
            const char* sourcePolicyName = neeCacheProviderRequested
                ? "nee-cache-provider"
                : (typedPolicy ? "typed-stratified-rlu" : "full-domain-uniform-rlu");
            common->Printf(
                "PathTracePrimaryPass: ReSTIR PDF+NEE RLU current producer dump stage=%s earlyReturn=%s enable=%d route=%d samples=%d visibility=%d sourcePolicy=%d(%s) debugMode=%d rluEnabled=%u rluCurrent=%u rluPrevious=%u rluRanges emissive=%u+%u doomAnalytic=%u+%u neeCacheProvider requested/ready=%d/%d providerResultSrv=t74 cellSrv=t75 candidateSrv=t77 classProbability emissive=%.3f doomAnalytic=%.3f sourcePdf=%s sourcePdfFormula=%s invSourcePdfFormula=%s producerHelperSequence=RTXDI_DIInitialSamplingParameters,RTXDI_RandomSamplerState,nee-cache-ris-candidate-or-fallback,RTXDI_StreamSample,RTXDI_FinalizeResampling reservoirM=1 normalizationDenominator=requestedLocalSamples selectedLightIdentity=dense-current-rlu-lightIndex solidAnglePdf=RAB_SampleActiveRrxPolymorphicLight targetPdf=RAB_GetLightSampleTargetPdfForSurface finalContribution=RAB_GetReflectedBsdfRadianceForSurface*reservoirInvPdf/solidAnglePdf*visibility cleanReservoirPage=u69 shader=%d bindingLayout=%d outputTex=%d firstMissingContract=%s temporal=0 spatial=0 bestLights=0 denoiser=0 mode56=%d oldPdfNee=discarded task=%s\n",
                stage ? stage : "unknown",
                earlyReturn ? earlyReturn : "none",
                r_pathTracingRestirPdfNeeVerifierEnable.GetInteger() != 0 ? 1 : 0,
                firstMissingContract[0] == 'n' && idStr::Icmp(firstMissingContract, "none") == 0 ? 1 : 0,
                idMath::ClampInt(1, 64, r_pathTracingRestirPdfNeeVerifierSamples.GetInteger()),
                pdfNeeVerifierEntryVisibility,
                sourcePolicy,
                sourcePolicyName,
                pdfNeeVerifierEntryDebugMode,
                regirRemixLightManagerStats.enabled,
                regirRemixLightManagerStats.currentLightCount,
                regirRemixLightManagerStats.previousLightCount,
                regirRemixLightManagerStats.emissiveRangeOffset,
                regirRemixLightManagerStats.emissiveRangeCount,
                regirRemixLightManagerStats.doomAnalyticRangeOffset,
                regirRemixLightManagerStats.doomAnalyticRangeCount,
                neeCacheProviderRequested ? 1 : 0,
                neeCacheProviderReady ? 1 : 0,
                emissiveClassProbability,
                doomAnalyticClassProbability,
                neeCacheProviderRequested ? "nee-cache-ris-candidate-or-fallback" : (typedPolicy ? "rtxdi-stratified-typed-current" : "rtxdi-stratified-dense-current"),
                neeCacheProviderRequested ? "cache:weightedCandidateIdentityPdf*cacheProbability,fallback:domainPdf*fallbackProbability" : (typedPolicy ? "rangeSampleCount/(rangeCount*totalProposalSamples)" : "1/currentRluLightCount"),
                neeCacheProviderRequested ? "1/sourcePdf" : (typedPolicy ? "(rangeCount*totalProposalSamples)/rangeSampleCount" : "currentRluLightCount"),
                m_smokeRestirPdfNeeRluCurrentShaderTable ? 1 : 0,
                m_smokePdfNeeVerifierBindingLayout ? 1 : 0,
                m_frameResources.outputTexture ? 1 : 0,
                firstMissingContract,
                pdfNeeVerifierEntryForbiddenMode ? 1 : 0,
                "PDFNEE-RLU-04");
            return;
        }

        const int splitCount = Max(0, m_smokeEmissiveTriangleCount) + Max(0, m_smokeDoomAnalyticLightCount);
        const int unifiedCount = Max(0, m_smokeUnifiedLightCount);
        const int realAnalyticOneCount = Max(0, m_smokeDoomAnalyticLightCount) > 0 ? 1 : 0;
        const int realAnalyticTwoCount = Max(0, m_smokeDoomAnalyticLightCount) >= 2 ? 2 : 0;
        const int realAnalyticFullCount = Max(0, m_smokeDoomAnalyticLightCount);
        const int emissiveDomainCount = Max(0, m_smokeEmissiveTriangleCount);
        int activeDomainCount = -1;
        if (pdfNeeVerifierEntryLightMode >= 1 && pdfNeeVerifierEntryLightMode <= 3)
        {
            activeDomainCount = pdfNeeVerifierEntryLightMode == 3 ? 4 : pdfNeeVerifierEntryLightMode;
        }
        else if (pdfNeeVerifierEntryLightMode == 4)
        {
            activeDomainCount = realAnalyticOneCount;
        }
        else if (pdfNeeVerifierEntryLightMode == 5)
        {
            activeDomainCount = realAnalyticTwoCount;
        }
        else if (pdfNeeVerifierEntryLightMode == 6)
        {
            activeDomainCount = realAnalyticFullCount;
        }
        else if (pdfNeeVerifierEntryLightMode == 7)
        {
            activeDomainCount = emissiveDomainCount;
        }
        else if (pdfNeeVerifierEntryLightMode == 8)
        {
            activeDomainCount = static_cast<int>(regirRemixLightManagerStats.currentLightCount);
        }
        else if (pdfNeeVerifierEntryLightMode == 9)
        {
            activeDomainCount = 0;
        }
        else if (pdfNeeVerifierEntryDomain == 0)
        {
            activeDomainCount = splitCount;
        }
        else if (pdfNeeVerifierEntryDomain == 1)
        {
            activeDomainCount = unifiedCount;
        }

        const char* firstMissingContract = "none";
        if (pdfNeeVerifierEntryForbiddenMode)
        {
            firstMissingContract = "forbidden-mode-56";
        }
        else if (!pdfNeeVerifierRouteRequested)
        {
            firstMissingContract = "route-disabled";
        }
        else if (earlyReturn && idStr::Icmp(earlyReturn, "none") != 0)
        {
            firstMissingContract = earlyReturn;
        }
        else if (pdfNeeVerifierEntryLightMode == 9)
        {
            firstMissingContract = "regir-consume-disabled-standalone-lane";
        }
        else if (pdfNeeVerifierEntryLightMode == 8)
        {
            firstMissingContract = "quarantined-failed-rlu-direct-diagnostic";
        }
        else if (pdfNeeVerifierEntryLightMode == 0 || activeDomainCount == 0)
        {
            firstMissingContract = "no-active-proposal-domain";
        }
        else if (pdfNeeVerifierEntryLightMode < 1 || pdfNeeVerifierEntryLightMode > 8)
        {
            firstMissingContract = "estimator-not-implemented-pdfnee-01";
        }
        const char* taskLabel = (pdfNeeVerifierEntryLightMode == 2 || pdfNeeVerifierEntryLightMode == 3) ? "PDFNEE-03" :
            (pdfNeeVerifierEntryLightMode == 9 ? "PDFNEE-11" :
            (pdfNeeVerifierEntryLightMode == 8 ? "PDFNEE-QUARANTINED" :
            (pdfNeeVerifierEntryLightMode == 7 ? "PDFNEE-07" :
            (pdfNeeVerifierEntryLightMode == 6 ? "PDFNEE-06" :
            (pdfNeeVerifierEntryLightMode == 5 ? "PDFNEE-05" :
            (pdfNeeVerifierEntryLightMode == 4 ? "PDFNEE-04" :
            (pdfNeeVerifierEntryLightMode == 1 ? "PDFNEE-02" : "PDFNEE-01")))))));
        common->Printf(
            "PathTracePrimaryPass: PDFNEE verifier dump stage=%s earlyReturn=%s enable=%d route=%d view=%d lightMode=%d domain=%d samples=%d visibility=%d r_pathTracing=%d debugMode=%d cleanRoute=%d output=%dx%d sceneBuilt=%d shader=%d bindingSet=%d textureTable=%d outputTex=%d accumulation=%d readback=%d commandList=%d splitCount=%d unifiedCount=%d managerCount=deferred doomAnalyticCount=%d activeVerifierCount=%d firstMissingContract=%s syntheticOneLightTable={sourcePdf=1.000000 solidAnglePdf=1.000000 targetPdfCenter=0.318310 reservoirInvPdf=1.000000 reflectedRadianceCenter=(0.318310,0.318310,0.318310) finalContributionCenter=(0.318310,0.318310,0.318310)} syntheticOverlapSourcePdfTable={twoLights=(0.500000,0.500000) nLightsCount=4 nLightsEach=0.250000 sourcePdfSum=1.000000} realAnalyticOneLightTable={proposalDomain=first-contributing-doom-analytic-for-surface sourcePdf=1.000000 requiredCalls=RAB_LoadLightInfo,RAB_SamplePolymorphicLight,RAB_GetLightSampleTargetPdfForSurface,RAB_GetReflectedBsdfRadianceForSurface} realAnalyticTwoLightTable={proposalDomain=first-two-contributing-doom-analytics-for-surface sourcePdf=(0.500000,0.500000) sourcePdfSum=1.000000 reservoirInvPdf=2.000000 finalContribution=per-selected-light-reflectedRadiance*2/solidAnglePdf*visibility view8=16-sample-rab-average-sum-of-both-lights requiredCalls=RAB_LoadLightInfo,RAB_SamplePolymorphicLight,RAB_GetLightSampleTargetPdfForSurface,RAB_GetReflectedBsdfRadianceForSurface} realAnalyticFullDomainTable={proposalDomain=all-current-doom-analytics sourcePdfFormula=1/currentDoomAnalyticCount sourcePdfSum=1.000000 invalidPolicy=included-as-zero-contribution reservoirInvPdf=currentDoomAnalyticCount view8=sum-of-all-valid-selected-sample-contributions requiredCalls=RAB_LoadLightInfo,RAB_SamplePolymorphicLight,RAB_GetLightSampleTargetPdfForSurface,RAB_GetReflectedBsdfRadianceForSurface} emissiveDomainTable={proposalDomain=current-emissive-triangles sourcePdf=sampleWeightAndPdf.y fallback=max(uploadedPdf,1/currentEmissiveCount) selection=SelectSmokeWeightedEmissiveTriangle view8=sum-of-valid-emissive-contributions requiredCalls=RAB_LoadLightInfo,RAB_SamplePolymorphicLight,RAB_GetLightSampleTargetPdfForSurface,RAB_GetReflectedBsdfRadianceForSurface} regirDomainTable={proposalDomain=ReGIR-candidate-cache sourcePdf=1/invSourcePdf invSourcePdf=risWeightSum/selectedCellWeight selection=boundedCellSlotScan16to32 view8=full-cell-slot-mean prepass=build-only-u72 consumer=t73 requiredCalls=RAB_LoadLightInfo,RAB_SamplePolymorphicLight,RAB_GetLightSampleTargetPdfForSurface,RAB_GetReflectedBsdfRadianceForSurface} output=owned-current-frame temporal=0 spatial=0 mode56=0 task=%s\n",
            stage ? stage : "unknown",
            earlyReturn ? earlyReturn : "none",
            r_pathTracingRestirPdfNeeVerifierEnable.GetInteger() != 0 ? 1 : 0,
            pdfNeeVerifierRouteRequested ? 1 : 0,
            pdfNeeVerifierEntryView,
            pdfNeeVerifierEntryLightMode,
            pdfNeeVerifierEntryDomain,
            idMath::ClampInt(1, 64, r_pathTracingRestirPdfNeeVerifierSamples.GetInteger()),
            pdfNeeVerifierEntryVisibility,
            r_pathTracing.GetInteger(),
            pdfNeeVerifierEntryDebugMode,
            cleanRtxdiDiRouteRequested ? 1 : 0,
            m_frameResources.width,
            m_frameResources.height,
            m_smokeSceneBuilt ? 1 : 0,
            m_smokeShaderTable ? 1 : 0,
            m_smokeBindingSet ? 1 : 0,
            m_smokeTextureDescriptorTable ? 1 : 0,
            m_frameResources.outputTexture ? 1 : 0,
            m_frameResources.accumulationTexture ? 1 : 0,
            m_frameResources.readbackTexture ? 1 : 0,
            (m_backend && m_backend->GL_GetCommandList()) ? 1 : 0,
            splitCount,
            unifiedCount,
            Max(0, m_smokeDoomAnalyticLightCount),
            activeDomainCount,
            firstMissingContract,
            taskLabel);
    };
    if (pdfNeeReGIRSourceRouteBlocked)
    {
        if (pdfNeeVerifierDumpRequested)
        {
            printPdfNeeVerifierDump("dispatch-entry", "regir-v06-standalone-required");
            r_pathTracingRestirPdfNeeVerifierDump.SetInteger(0);
        }
        return;
    }
    const bool cleanRtxdiDiBaseResourcesValid =
        viewDef && m_smokeCleanRtxdiDiSentinelBindingLayout && m_smokeTextureDescriptorTable && m_smokeCleanRtxdiDiSentinelConstantsBuffer &&
        m_smokeSceneBuilt && m_smokeTlas && m_frameResources.outputTexture &&
        PathTraceMaterialFeaturePrimaryOutputAvailable(cleanRtxdiDiTransmissionPass, m_frameResources) &&
        m_smokeStaticTriangleMaterialIndexBuffer && m_smokeDynamicTriangleMaterialIndexBuffer &&
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
            m_smokeStaticVertexBuffer && m_smokeStaticIndexBuffer && m_smokeStaticTriangleClassBuffer &&
            m_smokeStaticTriangleMaterialBuffer && m_smokeStaticTriangleMaterialIndexBuffer &&
            m_smokeDynamicVertexBuffer && m_smokeDynamicIndexBuffer && m_smokeDynamicTriangleClassBuffer &&
            m_smokeDynamicTriangleMaterialBuffer && m_smokeDynamicTriangleMaterialIndexBuffer &&
            m_smokeMaterialTableBuffer &&
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
        m_frameResources.rrGuideResetMaskTexture && m_frameResources.rrGuidePositionTexture && m_frameResources.readbackTexture && m_smokeConstantsBuffer && m_restirPTConstantsBuffer &&
        m_smokeBoundsOverlayLineBuffer && m_smokeStaticVertexBuffer && m_smokeStaticIndexBuffer && m_smokeStaticTriangleClassBuffer &&
        m_smokeStaticTriangleMaterialBuffer && m_smokeStaticTriangleMaterialIndexBuffer && m_smokeDynamicVertexBuffer &&
        m_smokeDynamicIndexBuffer && m_smokeDynamicTriangleClassBuffer && m_smokeDynamicTriangleMaterialBuffer &&
        m_smokeDynamicTriangleMaterialIndexBuffer && m_smokeMaterialTableBuffer && m_smokeEmissiveTriangleBuffer &&
        m_smokePreviousEmissiveTriangleBuffer && m_smokeEmissiveRemapBuffer && m_smokeEmissiveDistributionBuffer &&
        m_smokeLightCandidateBuffer && m_smokeDoomAnalyticLightBuffer && m_smokeDoomAnalyticPreviousLightBuffer &&
        m_smokeDoomAnalyticCurrentIdentityBuffer && m_smokeDoomAnalyticPreviousIdentityBuffer && m_smokeDoomAnalyticRemapBuffer &&
        m_smokeRigidRouteVertexBuffer && m_smokeRigidRouteIndexBuffer && m_smokeRigidRouteTriangleMaterialBuffer &&
        m_smokeRigidRouteTriangleMaterialIndexBuffer && m_smokeRigidRouteInstanceBuffer;
    const bool baseResourcesValid = neeCacheDebugRouteRequested ? neeCacheDebugBaseResourcesValid :
        (regirDebugRouteRequested ? regirDebugBaseResourcesValid :
        (cleanRtxdiDiRouteRequested ? cleanRtxdiDiBaseResourcesValid :
        ((pdfNeeVerifierRouteRequested || pdfNeeRluCurrentProducerRequested) ? pdfNeeVerifierBaseResourcesValid : smokeBaseResourcesValid)));
    if (!baseResourcesValid)
    {
        if (r_pathTracingNeeCacheDump.GetInteger() != 0)
        {
            printNeeCacheDump("dispatch-entry", "base-resource");
            r_pathTracingNeeCacheDump.SetInteger(0);
        }
        if (r_pathTracingReGIRDump.GetInteger() != 0)
        {
            printReGIRDump("dispatch-entry", "base-resource");
            r_pathTracingReGIRDump.SetInteger(0);
        }
        if (cleanRtxdiDiDumpRequested)
        {
            printCleanRtxdiDiDump("dispatch-entry", "base-resource", 0);
            r_pathTracingCleanRtxdiDiDump.SetInteger(0);
        }
        if (pdfNeeVerifierDumpRequested)
        {
            printPdfNeeVerifierDump("dispatch-entry", "base-resource");
            r_pathTracingRestirPdfNeeVerifierDump.SetInteger(0);
        }
        return;
    }
    if (!standaloneDebugRouteRequested && !cleanRtxdiDiRouteRequested && !pdfNeeVerifierRouteRequested && !pdfNeeRluCurrentProducerRequested && !m_frameResources.smokeReservoirBuffers.IsValidFor(m_frameResources.width, m_frameResources.height))
    {
        if (r_pathTracingReGIRDump.GetInteger() != 0)
        {
            printReGIRDump("dispatch-entry", "smoke-reservoir");
            r_pathTracingReGIRDump.SetInteger(0);
        }
        if (cleanRtxdiDiDumpRequested)
        {
            printCleanRtxdiDiDump("dispatch-entry", "smoke-reservoir", 0);
            r_pathTracingCleanRtxdiDiDump.SetInteger(0);
        }
        if (pdfNeeVerifierDumpRequested)
        {
            printPdfNeeVerifierDump("dispatch-entry", "smoke-reservoir");
            r_pathTracingRestirPdfNeeVerifierDump.SetInteger(0);
        }
        return;
    }
    if (!standaloneDebugRouteRequested && !cleanRtxdiDiRouteRequested && !pdfNeeVerifierRouteRequested && !pdfNeeRluCurrentProducerRequested &&
        (!m_frameResources.restirPTReservoirBuffers.IsValidFor(static_cast<uint32_t>(m_frameResources.width), static_cast<uint32_t>(m_frameResources.height), RtRestirPTCheckerboardMode::Off) ||
            !m_frameResources.restirPTDiReservoirBuffers.IsValidFor(static_cast<uint32_t>(m_frameResources.width), static_cast<uint32_t>(m_frameResources.height), RtRestirPTCheckerboardMode::Off) ||
            !m_frameResources.restirPTGiReservoirBuffers.IsValidFor(static_cast<uint32_t>(m_frameResources.width), static_cast<uint32_t>(m_frameResources.height), RtRestirPTCheckerboardMode::Off)))
    {
        if (r_pathTracingReGIRDump.GetInteger() != 0)
        {
            printReGIRDump("dispatch-entry", "restir-reservoir");
            r_pathTracingReGIRDump.SetInteger(0);
        }
        if (cleanRtxdiDiDumpRequested)
        {
            printCleanRtxdiDiDump("dispatch-entry", "restir-reservoir", 0);
            r_pathTracingCleanRtxdiDiDump.SetInteger(0);
        }
        if (pdfNeeVerifierDumpRequested)
        {
            printPdfNeeVerifierDump("dispatch-entry", "restir-reservoir");
            r_pathTracingRestirPdfNeeVerifierDump.SetInteger(0);
        }
        return;
    }
    if (!standaloneDebugRouteRequested && !cleanRtxdiDiRouteRequested && !pdfNeeVerifierRouteRequested && !pdfNeeRluCurrentProducerRequested && !m_frameResources.primarySurfaceHistoryBuffers.IsValidFor(static_cast<uint32_t>(m_frameResources.width), static_cast<uint32_t>(m_frameResources.height)))
    {
        if (r_pathTracingReGIRDump.GetInteger() != 0)
        {
            printReGIRDump("dispatch-entry", "primary-history");
            r_pathTracingReGIRDump.SetInteger(0);
        }
        if (cleanRtxdiDiDumpRequested)
        {
            printCleanRtxdiDiDump("dispatch-entry", "primary-history", 0);
            r_pathTracingCleanRtxdiDiDump.SetInteger(0);
        }
        if (pdfNeeVerifierDumpRequested)
        {
            printPdfNeeVerifierDump("dispatch-entry", "primary-history");
            r_pathTracingRestirPdfNeeVerifierDump.SetInteger(0);
        }
        return;
    }

    nvrhi::ICommandList* commandList = m_backend ? m_backend->GL_GetCommandList() : nullptr;
    if (!commandList)
    {
        if (r_pathTracingNeeCacheDump.GetInteger() != 0)
        {
            printNeeCacheDump("dispatch-entry", "command-list");
            r_pathTracingNeeCacheDump.SetInteger(0);
        }
        if (r_pathTracingReGIRDump.GetInteger() != 0)
        {
            printReGIRDump("dispatch-entry", "command-list");
            r_pathTracingReGIRDump.SetInteger(0);
        }
        if (cleanRtxdiDiDumpRequested)
        {
            printCleanRtxdiDiDump("dispatch-entry", "command-list", 0);
            r_pathTracingCleanRtxdiDiDump.SetInteger(0);
        }
        if (pdfNeeVerifierDumpRequested)
        {
            printPdfNeeVerifierDump("dispatch-entry", "command-list");
            r_pathTracingRestirPdfNeeVerifierDump.SetInteger(0);
        }
        return;
    }
    PollPathTraceGpuTimingResults();
    nvrhi::IDevice* device = deviceManager ? deviceManager->GetDevice() : nullptr;
    const bool optickGpuMarkers = r_pathTracingOptickGpuMarkers.GetInteger() != 0;
    const bool nsightGpuMarkers = r_pathTracingNsightGpuMarkers.GetInteger() != 0;
    if (optickGpuMarkers)
    {
        OPTICK_GPU_CONTEXT((void*)commandList->getNativeObject(GetPathTraceCommandObjectType()));
    }
    if (cleanRtxdiDiRouteRequested)
    {
        const bool cleanExternalPdfNeeCurrent = cleanExternalPdfNeeRequested || pdfNeeRluCurrentProducerRequested;
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
        if (!m_smokeCleanRtxdiDiSentinelShaderTable ||
            !m_smokeCleanRtxdiDiInitialShaderTable ||
            !m_smokeCleanRtxdiDiTemporalShaderTable)
        {
            InitRayTracingSmokeRestirPipeline(15);
        }
        if (!m_smokeCleanRtxdiDiSentinelShaderTable ||
            !m_smokeCleanRtxdiDiInitialShaderTable ||
            !m_smokeCleanRtxdiDiTemporalShaderTable)
        {
            if (cleanRtxdiDiDumpRequested)
            {
                printCleanRtxdiDiDump("dispatch-entry", "clean-shader", 0);
                r_pathTracingCleanRtxdiDiDump.SetInteger(0);
            }
            return;
        }
        if (cleanRtxdiDiSpatialShaderRequested && !m_smokeCleanRtxdiDiSpatialShaderTable)
        {
            InitRayTracingSmokeRestirPipeline(20);
        }
        if (cleanRtxdiDiSpatialShaderRequested && !m_smokeCleanRtxdiDiSpatialShaderTable)
        {
            if (cleanRtxdiDiDumpRequested)
            {
                printCleanRtxdiDiDump("dispatch-entry", "clean-spatial-shader", 0);
                r_pathTracingCleanRtxdiDiDump.SetInteger(0);
            }
            return;
        }
        if (!EnsurePathTraceMaterialFeatureRuntimePassPipeline(cleanRtxdiDiTransmissionPass))
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
        if (cleanExternalPdfNeeMode9Requested && !m_smokeReGIRDebugShaderTable)
        {
            InitRayTracingSmokeRestirPipeline(17);
        }
        if (cleanExternalPdfNeeMode9Requested && !m_smokeReGIRDebugShaderTable)
        {
            if (cleanRtxdiDiDumpRequested)
            {
                printCleanRtxdiDiDump("dispatch-entry", "regir-build-shader", 0);
                r_pathTracingCleanRtxdiDiDump.SetInteger(0);
            }
            return;
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
            (!m_smokeNeeCachePrimarySurfaceUpdatePipeline || !m_smokeNeeCachePrimarySurfaceUpdateBindingLayout))
        {
            if (cleanRtxdiDiDumpRequested)
            {
                printCleanRtxdiDiDump("dispatch-entry", "nee-cache-primary-surface-update-shader", 0);
                r_pathTracingCleanRtxdiDiDump.SetInteger(0);
            }
            return;
        }

        if (cleanRtxdiDiView >= 2 && cleanRtxdiDiView <= 24)
        {
            if (!m_smokePrimarySurfaceProducerShaderTable)
            {
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

            const bool primarySurfaceAdapterResourcesValid =
                m_smokeSceneBuilt && m_smokeBindingSet && m_smokeTextureDescriptorTable && m_smokeConstantsBuffer &&
                m_smokeStaticVertexBuffer && m_smokeStaticIndexBuffer && m_smokeStaticTriangleClassBuffer &&
                m_smokeStaticTriangleMaterialBuffer && m_smokeStaticTriangleMaterialIndexBuffer &&
                m_smokeDynamicVertexBuffer && m_smokeDynamicIndexBuffer && m_smokeDynamicTriangleClassBuffer &&
                m_smokeDynamicTriangleMaterialBuffer && m_smokeDynamicTriangleMaterialIndexBuffer &&
                m_smokeMaterialTableBuffer && m_smokeRigidRouteVertexBuffer && m_smokeRigidRouteIndexBuffer &&
                m_smokeRigidRouteTriangleMaterialBuffer && m_smokeRigidRouteTriangleMaterialIndexBuffer &&
                m_smokeRigidRouteInstanceBuffer && m_smokePreviousStaticVertexBuffer && m_smokePreviousStaticIndexBuffer &&
                m_smokePreviousStaticTriangleClassBuffer && m_smokePreviousStaticTriangleMaterialBuffer &&
                m_smokePreviousStaticTriangleMaterialIndexBuffer &&
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
                (r_pathTracingUseEmissiveMaps.GetInteger() != 0 && cleanRtxdiDiResolveView == 16 ? 32u : 0u) |
                (r_pathTracingToyFakePBRSpecular.GetInteger() != 0 && (cleanRtxdiDiResolveView == 16 || cleanRtxdiDiMaterialClassifierProofView) ? 128u : 0u);
            primarySurfaceConstants.textureInfo[3] = static_cast<float>(primarySurfaceTextureFlags);
            primarySurfaceConstants.safetyInfo[0] = static_cast<float>(BuildPathTraceSafetyDisableMask());
            primarySurfaceConstants.safetyInfo[1] = primarySurfaceConstants.textureInfo[0];
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
            primarySurfaceConstants.decalInfo[0] = static_cast<float>(idMath::ClampInt(0, 4, r_pathTracingDecalComposite.GetInteger()));
            primarySurfaceConstants.decalInfo[1] = Max(0.0f, r_pathTracingDecalOffsetStep.GetFloat());
            primarySurfaceConstants.decalInfo[2] = static_cast<float>(Max(1, r_pathTracingDecalMaxOffsetIndex.GetInteger()));
            primarySurfaceConstants.decalInfo[3] = idMath::ClampFloat(0.0f, 1.0f, r_pathTracingDecalModulateFloor.GetFloat());
            const int primarySurfaceDynamicRecordCount = Max(0, m_sceneInputs.materials.dynamicMaterialRecordCount);
            const int primarySurfaceMaterialOverlayRecordCount = m_sceneInputs.materials.materialTableGpuStable ? primarySurfaceDynamicRecordCount : 0;
            primarySurfaceConstants.decalInfo2[0] = static_cast<float>(primarySurfaceDynamicRecordCount);
            primarySurfaceConstants.decalInfo2[1] = static_cast<float>(primarySurfaceMaterialOverlayRecordCount);
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

            commandList->setBufferState(m_smokeStaticVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeStaticIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeStaticTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeStaticTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeStaticTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            SetBufferStateIfPresent(commandList, m_smokeDynamicMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeMaterialTableBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteInstanceBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokePreviousStaticVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokePreviousStaticIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokePreviousStaticTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokePreviousStaticTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokePreviousStaticTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
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

            nvrhi::rt::State primarySurfaceState;
            primarySurfaceState.shaderTable = m_smokePrimarySurfaceProducerShaderTable;
            primarySurfaceState.bindings = { m_smokeBindingSet, m_smokeTextureDescriptorTable };
            commandList->setRayTracingState(primarySurfaceState);

            nvrhi::rt::DispatchRaysArguments primarySurfaceArgs;
            primarySurfaceArgs.width = m_frameResources.width;
            primarySurfaceArgs.height = m_frameResources.height;
            primarySurfaceArgs.depth = 1;
            commandList->dispatchRays(primarySurfaceArgs);

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

        const bool cleanRtxdiDiNeedsPrimarySurface = cleanRtxdiDiView >= 2;
        const bool cleanRtxdiDiNeedsCurrentAnalytic = !cleanRtxdiDiMaterialClassifierProofView &&
            ((cleanRtxdiDiView >= 3 && cleanRtxdiDiView <= 8) || cleanRtxdiDiView == 10 || cleanRtxdiDiView == 12 || cleanRtxdiDiView == 13 || cleanRtxdiDiView == 14 || cleanRtxdiDiView == 15 || cleanRtxdiDiResolveView == 16);
        const bool cleanRtxdiDiNeedsAnalyticTemporalInputs = cleanRtxdiDiView == 5 || cleanRtxdiDiView == 6 ||
            (!cleanRtxdiDiMaterialClassifierProofView && cleanRtxdiDiView == 12) ||
            cleanRtxdiDiResolveView == 16 ||
            (cleanRtxdiDiView == 8 && cleanRtxdiDiTemporalEnabled);
        const bool cleanRtxdiDiBindingInputsValid =
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

            nvrhi::BindingSetHandle cleanReGIRBuildBindingSet;
            if (cleanExternalPdfNeeMode9Requested)
            {
                if (!m_smokeReGIRDebugBindingLayout || !m_smokeReGIRState.candidateCacheBuffer || !m_smokeReGIRState.placeholderSrvBuffer)
                {
                    if (cleanRtxdiDiDumpRequested)
                    {
                        printCleanRtxdiDiDump("dispatch-entry", "regir-candidate-cache", 0);
                        r_pathTracingCleanRtxdiDiDump.SetInteger(0);
                    }
                    return;
                }

                auto cleanReGIROptionalSrv = [&](const nvrhi::BufferHandle& buffer) -> nvrhi::BufferHandle {
                    return buffer ? buffer : m_smokeReGIRState.placeholderSrvBuffer;
                };

                nvrhi::BindingSetDesc cleanReGIRBindingSetDesc;
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, m_smokeTlas));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(1, m_frameResources.outputTexture));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(2, m_smokeConstantsBuffer));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, cleanReGIROptionalSrv(m_smokeStaticVertexBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, cleanReGIROptionalSrv(m_smokeStaticIndexBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(5, cleanReGIROptionalSrv(m_smokeStaticTriangleClassBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, cleanReGIROptionalSrv(m_smokeDynamicVertexBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, cleanReGIROptionalSrv(m_smokeDynamicIndexBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(8, cleanReGIROptionalSrv(m_smokeDynamicTriangleClassBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(9, cleanReGIROptionalSrv(m_smokeStaticTriangleMaterialBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(10, cleanReGIROptionalSrv(m_smokeDynamicTriangleMaterialBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(11, cleanReGIROptionalSrv(m_smokeStaticTriangleMaterialIndexBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(12, cleanReGIROptionalSrv(m_smokeDynamicTriangleMaterialIndexBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(13, cleanReGIROptionalSrv(m_smokeMaterialTableBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(16, cleanReGIROptionalSrv(m_smokeEmissiveTriangleBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(22, cleanReGIROptionalSrv(m_smokeRigidRouteVertexBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(23, cleanReGIROptionalSrv(m_smokeRigidRouteIndexBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(24, cleanReGIROptionalSrv(m_smokeRigidRouteTriangleMaterialBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(25, cleanReGIROptionalSrv(m_smokeRigidRouteTriangleMaterialIndexBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(26, cleanReGIROptionalSrv(m_smokeRigidRouteInstanceBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(27, cleanReGIROptionalSrv(m_smokeDoomAnalyticLightBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(42, cleanReGIROptionalSrv(m_smokeDoomAnalyticCurrentIdentityBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(43, cleanReGIROptionalSrv(m_smokeDoomAnalyticPreviousIdentityBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(44, cleanReGIROptionalSrv(m_smokeDoomAnalyticRemapBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(45, cleanReGIROptionalSrv(m_smokeDoomAnalyticPreviousLightBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(57, cleanReGIROptionalSrv(m_smokePreviousEmissiveTriangleBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(58, cleanReGIROptionalSrv(m_smokeEmissiveRemapBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(59, cleanReGIROptionalSrv(m_smokeUnifiedLightBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(60, cleanReGIROptionalSrv(m_smokeUnifiedPreviousLightBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(61, cleanReGIROptionalSrv(m_smokeUnifiedLightRemapBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(64, cleanReGIROptionalSrv(m_smokeRestirLightManagerCurrentToPreviousBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(65, cleanReGIROptionalSrv(m_smokeRestirLightManagerPreviousToCurrentBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(66, cleanReGIROptionalSrv(m_smokeRestirLightManagerCurrentPayloadBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(67, cleanReGIROptionalSrv(m_smokeRestirLightManagerPreviousPayloadBuffer)));
                cleanReGIRBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(72, m_smokeReGIRState.candidateCacheBuffer));
                cleanReGIRBuildBindingSet = device->createBindingSet(cleanReGIRBindingSetDesc, m_smokeReGIRDebugBindingLayout);
                if (!cleanReGIRBuildBindingSet)
                {
                    if (cleanRtxdiDiDumpRequested)
                    {
                        printCleanRtxdiDiDump("dispatch-entry", "regir-build-binding-set", 0);
                        r_pathTracingCleanRtxdiDiDump.SetInteger(0);
                    }
                    return;
                }
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
                nvrhi::BindingSetItem::StructuredBuffer_SRV(3, m_smokeStaticVertexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(4, m_smokeStaticIndexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(5, m_smokeStaticTriangleClassBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(6, m_smokeDynamicVertexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(7, m_smokeDynamicIndexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(8, m_smokeDynamicTriangleClassBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(9, m_smokeStaticTriangleMaterialBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(10, m_smokeDynamicTriangleMaterialBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(11, m_smokeStaticTriangleMaterialIndexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(12, m_smokeDynamicTriangleMaterialIndexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(13, m_smokeMaterialTableBuffer),
                nvrhi::BindingSetItem::Texture_SRV(14, pdfNeeFallbackTexture),
                nvrhi::BindingSetItem::Texture_UAV(15, m_frameResources.accumulationTexture),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(16, m_smokeEmissiveTriangleBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(57, m_smokePreviousEmissiveTriangleBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(58, m_smokeEmissiveRemapBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(46, m_smokeEmissiveDistributionBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(17, m_smokeLightCandidateBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(18, m_frameResources.smokeReservoirBuffers.current),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(19, m_frameResources.smokeReservoirBuffers.previous),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(20, m_frameResources.smokeReservoirBuffers.spatialScratch),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(21, m_smokeBoundsOverlayLineBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(22, m_smokeRigidRouteVertexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(23, m_smokeRigidRouteIndexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(24, m_smokeRigidRouteTriangleMaterialBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(25, m_smokeRigidRouteTriangleMaterialIndexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(26, m_smokeRigidRouteInstanceBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(27, m_smokeDoomAnalyticLightBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(45, m_smokeDoomAnalyticPreviousLightBuffer),
                nvrhi::BindingSetItem::ConstantBuffer(28, m_restirPTConstantsBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(29, m_frameResources.restirPTReservoirBuffers.reservoirs),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(30, m_frameResources.primarySurfaceHistoryBuffers.current),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(31, m_frameResources.primarySurfaceHistoryBuffers.previous),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(32, m_smokeSkinnedPreviousPositionBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(33, m_smokeSkinnedSurfaceDispatchBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(34, m_smokePreviousStaticVertexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(35, m_smokePreviousStaticIndexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(36, m_smokePreviousStaticTriangleClassBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(37, m_smokePreviousStaticTriangleMaterialBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(38, m_smokePreviousStaticTriangleMaterialIndexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(42, m_smokeDoomAnalyticCurrentIdentityBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(43, m_smokeDoomAnalyticPreviousIdentityBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(44, m_smokeDoomAnalyticRemapBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(59, m_smokeUnifiedLightBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(60, m_smokeUnifiedPreviousLightBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(61, m_smokeUnifiedLightRemapBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(62, m_smokeRestirLightManagerCurrentBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(63, m_smokeRestirLightManagerPreviousBuffer),
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
                nvrhi::BindingSetItem::StructuredBuffer_UAV(55, m_frameResources.restirPTGiReservoirBuffers.reservoirs),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(56, m_frameResources.restirPTDiReservoirBuffers.reservoirs),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(68, m_remixRtxdiResources.GetDomain(PATH_TRACE_REMIX_RTXDI_RESERVOIR_DOMAIN_DI).reservoirs ? m_remixRtxdiResources.GetDomain(PATH_TRACE_REMIX_RTXDI_RESERVOIR_DOMAIN_DI).reservoirs : m_frameResources.restirPTDiReservoirBuffers.reservoirs),
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
                (r_pathTracingReservoirTwoSidedEmissives.GetInteger() != 0 ? 64u : 0u);
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
            pdfNeeProducerConstants.restirPdfNeeVerifierInfo[0] = 1.0f;
            pdfNeeProducerConstants.restirPdfNeeVerifierInfo[1] = 2.0f;
            pdfNeeProducerConstants.restirPdfNeeVerifierInfo[2] = 0.0f;
            pdfNeeProducerConstants.restirPdfNeeVerifierInfo[3] = 0.0f;
            pdfNeeProducerConstants.restirPdfNeeVerifierControlInfo[0] = static_cast<float>(idMath::ClampInt(1, 64, r_pathTracingRestirPdfNeeVerifierSamples.GetInteger()));
            pdfNeeProducerConstants.restirPdfNeeVerifierControlInfo[1] = static_cast<float>(pdfNeeVerifierEntryVisibility);
            pdfNeeProducerConstants.restirPdfNeeVerifierControlInfo[2] = static_cast<float>(idMath::ClampInt(0, 2, r_pathTracingRestirPdfNeeVerifierSourcePolicy.GetInteger()));
            pdfNeeProducerConstants.restirPdfNeeVerifierControlInfo[3] = 0.0f;
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
            pdfNeeProducerConstants.geometryInfo2[3] = static_cast<float>(m_frameResources.smokeReservoirBuffers.reservoirCount);
            pdfNeeProducerConstants.geometryInfo3[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.skinnedPreviousPositionCount));
            pdfNeeProducerConstants.geometryInfo3[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.skinnedSurfaceDispatchCount));
            pdfNeeProducerConstants.geometryInfo3[2] = static_cast<float>(Max(0, m_sceneInputs.geometry.skinnedTriangleDispatchIndexCount));
            pdfNeeProducerConstants.geometryInfo4[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.previousStaticVertexCount));
            pdfNeeProducerConstants.geometryInfo4[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.previousStaticIndexCount));
            pdfNeeProducerConstants.geometryInfo4[2] = static_cast<float>(Max(0, m_sceneInputs.geometry.previousStaticTriangleCount));
            pdfNeeProducerConstants.geometryInfo4[3] = static_cast<float>(Max(0, m_sceneInputs.geometry.previousStaticMaterialIndexCount));
            pdfNeeProducerConstants.dispatchTileInfo[2] = static_cast<float>(Max(0, m_frameResources.width));
            pdfNeeProducerConstants.dispatchTileInfo[3] = static_cast<float>(Max(0, m_frameResources.height));
            pdfNeeProducerConstants.motionVectorInfo[3] = r_pathTracingMotionVectorDisableRigid.GetBool() ? 1.0f : 0.0f;

            commandList->setBufferState(m_smokeStaticVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeStaticIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeStaticTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeStaticTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeStaticTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
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
            commandList->setBufferState(m_smokeRigidRouteVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeRigidRouteInstanceBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeBoundsOverlayLineBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_frameResources.smokeReservoirBuffers.current, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_frameResources.smokeReservoirBuffers.previous, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_frameResources.smokeReservoirBuffers.spatialScratch, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_frameResources.restirPTReservoirBuffers.reservoirs, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_frameResources.restirPTDiReservoirBuffers.reservoirs, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_frameResources.restirPTGiReservoirBuffers.reservoirs, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_frameResources.primarySurfaceHistoryBuffers.current, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_frameResources.primarySurfaceHistoryBuffers.previous, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_smokeCleanRtxdiDiCurrentReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_smokeCleanRtxdiDiTemporalReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_smokeCleanRtxdiDiPreviousReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
            if (cleanExternalPdfNeeMode9Requested)
            {
                commandList->setBufferState(m_smokeReGIRState.candidateCacheBuffer, nvrhi::ResourceStates::UnorderedAccess);
            }
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

            if (cleanExternalPdfNeeMode9Requested)
            {
                PathTraceSmokeConstants cleanReGIRBuildConstants = pdfNeeProducerConstants;
                cleanReGIRBuildConstants.regirInfo3[0] = 1.0f;
                cleanReGIRBuildConstants.dispatchTileInfo[0] = 0.0f;
                cleanReGIRBuildConstants.dispatchTileInfo[1] = 0.0f;
                cleanReGIRBuildConstants.dispatchTileInfo[2] = static_cast<float>(m_frameResources.width);
                cleanReGIRBuildConstants.dispatchTileInfo[3] = static_cast<float>(m_frameResources.height);
                commandList->writeBuffer(m_smokeConstantsBuffer, &cleanReGIRBuildConstants, sizeof(cleanReGIRBuildConstants));

                nvrhi::rt::State cleanReGIRBuildState;
                cleanReGIRBuildState.shaderTable = m_smokeReGIRDebugShaderTable;
                cleanReGIRBuildState.bindings = { cleanReGIRBuildBindingSet, m_smokeTextureDescriptorTable };
                commandList->setRayTracingState(cleanReGIRBuildState);

                nvrhi::rt::DispatchRaysArguments cleanReGIRBuildArgs;
                cleanReGIRBuildArgs.width = m_frameResources.width;
                cleanReGIRBuildArgs.height = m_frameResources.height;
                cleanReGIRBuildArgs.depth = 1;
                {
                    PathTraceGpuMarkerScope nsightMarker(commandList, "CleanDI.P0 ReGIRBuild DispatchRays", nsightGpuMarkers);
                    commandList->dispatchRays(cleanReGIRBuildArgs);
                }
                nvrhi::utils::BufferUavBarrier(commandList, m_smokeReGIRState.candidateCacheBuffer);
                commandList->setBufferState(m_smokeReGIRState.candidateCacheBuffer, nvrhi::ResourceStates::ShaderResource);
                commandList->commitBarriers();
            }

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

        nvrhi::BindingSetDesc cleanBindingSetDesc;
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, m_smokeTlas));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(1, m_frameResources.outputTexture));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(2, m_smokeCleanRtxdiDiSentinelConstantsBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, m_smokeStaticVertexBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, m_smokeStaticIndexBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, m_smokeDynamicVertexBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, m_smokeDynamicIndexBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(11, m_smokeStaticTriangleMaterialIndexBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(12, m_smokeDynamicTriangleMaterialIndexBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(13, m_smokeMaterialTableBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(14, cleanFallbackTexture));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(15, cleanOptionalSrv(m_smokeDynamicMaterialBuffer)));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(16, cleanOptionalSrv(m_smokeEmissiveTriangleBuffer)));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(22, m_smokeRigidRouteVertexBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(23, m_smokeRigidRouteIndexBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(25, m_smokeRigidRouteTriangleMaterialIndexBuffer));
        cleanBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(26, m_smokeRigidRouteInstanceBuffer));
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
        AddPathTraceMaterialFeatureOutputBindings(
            cleanBindingSetDesc,
            m_frameResources,
            RtPathTraceMaterialFeatureBindingLayout::CleanRtxdiDi);
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

        commandList->setTextureState(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->setBufferState(m_smokeStaticVertexBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeStaticIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeDynamicVertexBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeDynamicIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeStaticTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeDynamicTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeMaterialTableBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeDynamicMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeEmissiveTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokePreviousEmissiveTriangleBuffer, nvrhi::ResourceStates::ShaderResource);
        SetBufferStateIfPresent(commandList, m_smokeReGIRState.placeholderSrvBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeRigidRouteVertexBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeRigidRouteIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeRigidRouteTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(m_smokeRigidRouteInstanceBuffer, nvrhi::ResourceStates::ShaderResource);
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
        SetPathTraceMaterialFeaturePrimaryOutputState(
            commandList,
            cleanRtxdiDiTransmissionPass,
            m_frameResources,
            nvrhi::ResourceStates::UnorderedAccess);
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
        ClearPathTraceMaterialFeaturePrimaryOutput(
            commandList,
            cleanRtxdiDiTransmissionPass,
            m_frameResources,
            nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f));
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
            (m_smokeNeeCacheState.cleanProviderSnapshotHoldActive ||
                cleanRestirGiNeeCacheLiveDiagnosticRefresh) &&
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
            (cleanRtxdiDiView == 16 ||
                cleanRtxdiDiRrGuideDebugView ||
                (cleanRtxdiDiSpatialEnabled &&
                    cleanRtxdiDiTemporalEnabled &&
                    (cleanRtxdiDiView == 12 ||
                        (cleanRtxdiDiView == 8 && idMath::ClampInt(-1, 16, r_pathTracingCleanRtxdiDiView8Band.GetInteger()) == 16)))) &&
            cleanPromoteSubviewReservoir &&
            m_smokeCleanRtxdiDiSpatialShaderTable != nullptr;
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
        cleanConstants.previousAnalyticLightCount = cleanPreviousAnalyticLightCount;
        cleanConstants.previousAnalyticIdentityCount = cleanPreviousAnalyticIdentityCount;
        cleanConstants.analyticRemapCount = cleanAnalyticRemapCount;
        cleanConstants.temporalFlags = cleanTemporalFlags;
        cleanConstants.historyResetCount = m_smokeCleanRtxdiDiHistoryResetCount;
        cleanConstants.view8Band = static_cast<uint32_t>(cleanRtxdiDiRrInputMosaicView
            ? cleanRtxdiDiView18Tile
            : idMath::ClampInt(-1, 16, r_pathTracingCleanRtxdiDiView8Band.GetInteger()));
        cleanConstants.resolveVisibilityReuse = static_cast<uint32_t>(idMath::ClampInt(0, 3, r_pathTracingCleanRtxdiDiResolveVisibilityReuse.GetInteger()));
        cleanConstants.resolveBrdfTarget = r_pathTracingCleanRtxdiDiResolveBrdfTarget.GetInteger() != 0 ? 1u : 0u;
        cleanConstants.referenceRab = static_cast<uint32_t>(idMath::ClampInt(0, 10, r_pathTracingCleanRtxdiDiReferenceRab.GetInteger()));
        cleanConstants.rluCurrentLightCount = cleanRluRoute ? cleanRluCurrentLightCount : 0u;
        cleanConstants.rluPreviousLightCount = cleanRluRoute ? cleanRluPreviousLightCount : 0u;
        cleanConstants.rluCurrentToPreviousCount = cleanRluRoute ? cleanRluCurrentToPreviousCount : 0u;
        cleanConstants.rluPreviousToCurrentCount = cleanRluRoute ? cleanRluPreviousToCurrentCount : 0u;
        cleanConstants.temporalAudit = static_cast<uint32_t>(idMath::ClampInt(0, 1, r_pathTracingCleanRtxdiDiTemporalAudit.GetInteger()));
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
        cleanConstants.emissiveDistributionInfo[3] = 0.0f;
        const int cleanTextureSampleMethod = r_pathTracingTextureSampleEnable.GetInteger() != 0
            ? idMath::ClampInt(0, 2, r_pathTracingTextureSampleMethod.GetInteger())
            : 0;
        const uint32_t cleanTextureFlags =
            (r_pathTracingTextureBindlessEnable.GetInteger() != 0 ? 1u : 0u) |
            (r_pathTracingTextureFilter.GetInteger() != 0 ? 2u : 0u) |
            (r_pathTracingTextureDecode.GetInteger() != 0 ? 4u : 0u) |
            (r_pathTracingUseEmissiveMaps.GetInteger() != 0 ? 32u : 0u) |
            64u;
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
        cleanGuiSnapshot.denoiser = r_pathTracingCleanRtxdiDiDenoiser.GetInteger() != 0;
        cleanGuiSnapshot.fallback = r_pathTracingCleanRtxdiDiFallbackLighting.GetInteger() != 0;
        cleanGuiSnapshot.externalPdfNeeCurrent = r_pathTracingCleanRtxdiDiExternalPdfNeeCurrent.GetInteger() != 0 || pdfNeeRluCurrentProducerRequested;
        cleanGuiSnapshot.externalPdfNeeMode9 = cleanExternalPdfNeeMode9Requested;
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
        cleanGuiSnapshot.spatialReservoir = m_smokeCleanRtxdiDiSpatialReservoirBuffer != nullptr;
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
        cleanConstants.motionVectorInfo[1] = 1.0f;
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
        SetPathTraceMaterialFeatureRuntimeInfo(
            cleanConstants.toyPathInfo,
            cleanRtxdiDiTransmissionPass);
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
        cleanConstants.spatialInfo[0] = static_cast<float>(idMath::ClampInt(1, 16, r_cleanDiSpatialSamples.GetInteger()));
        cleanConstants.spatialInfo[1] = static_cast<float>(idMath::ClampInt(1, 16, r_cleanDiSpatialDisocclusionSamples.GetInteger()));
        cleanConstants.spatialInfo[2] = idMath::ClampFloat(1.0f, 128.0f, r_cleanDiSpatialRadius.GetFloat());
        cleanConstants.spatialInfo[3] = cleanSpatialRoute && cleanRtxdiDiSpatialEnabled ? 1.0f : 0.0f;
        commandList->setRayTracingState(cleanState);

        if (r_pathTracingNeeCacheDump.GetInteger() != 0)
        {
            printNeeCacheDump("pre-dispatch", "none");
            r_pathTracingNeeCacheDump.SetInteger(0);
        }
        if (cleanRtxdiDiDumpRequested)
        {
            printCleanRtxdiDiDump("route-ready", "none", cleanSpatialRoute ? 2 : 1);
            r_pathTracingCleanRtxdiDiDump.SetInteger(0);
        }
        if (r_pathTracingNeeCacheSecondaryDump.GetInteger() != 0)
        {
            printNeeCacheSecondaryDump();
            r_pathTracingNeeCacheSecondaryDump.SetInteger(0);
        }

        nvrhi::rt::DispatchRaysArguments cleanArgs;
        cleanArgs.width = m_frameResources.width;
        cleanArgs.height = m_frameResources.height;
        cleanArgs.depth = 1;
        const bool cleanInitialRaygenView =
            cleanRtxdiDiResolveView == 4 ||
            cleanRtxdiDiResolveView == 7 ||
            (cleanRtxdiDiResolveView == 8 && !cleanRtxdiDiTemporalEnabled);
        const bool cleanTemporalRaygenView =
            cleanRtxdiDiResolveView == 5 ||
            cleanRtxdiDiResolveView == 6 ||
            cleanRtxdiDiResolveView == 8 ||
            cleanRtxdiDiResolveView == 9 ||
            cleanRtxdiDiResolveView == 10 ||
            cleanRtxdiDiResolveView == 11 ||
            cleanRtxdiDiResolveView == 16;
        const bool cleanSplitRaygenView = cleanInitialRaygenView || cleanTemporalRaygenView;
        PathTraceCleanRtxdiDiSentinelConstants dispatchConstants = cleanConstants;
        if (cleanSpatialRoute)
        {
            dispatchConstants.flags |= CLEAN_RTXDI_DI_FLAG_SPATIAL_TEMPORAL_PREPASS;
        }
        commandList->writeBuffer(m_smokeCleanRtxdiDiSentinelConstantsBuffer, &dispatchConstants, sizeof(dispatchConstants));
        if (cleanSplitRaygenView)
        {
            cleanState.shaderTable = m_smokeCleanRtxdiDiInitialShaderTable;
            commandList->setRayTracingState(cleanState);
            {
                PathTraceGpuMarkerScope nsightMarker(commandList, "CleanDI.0 Initial DispatchRays", nsightGpuMarkers);
                commandList->dispatchRays(cleanArgs);
            }
            nvrhi::utils::BufferUavBarrier(commandList, m_smokeCleanRtxdiDiCurrentReservoirBuffer);
            nvrhi::utils::BufferUavBarrier(commandList, m_smokeCleanRtxdiDiTemporalReservoirBuffer);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.outputTexture);

            if (cleanTemporalRaygenView)
            {
                cleanState.shaderTable = m_smokeCleanRtxdiDiTemporalShaderTable;
                commandList->setRayTracingState(cleanState);
                {
                    PathTraceGpuMarkerScope nsightMarker(commandList, "CleanDI.1 Temporal DispatchRays", nsightGpuMarkers);
                    commandList->dispatchRays(cleanArgs);
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
        if (cleanSpatialRoute)
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
            cleanSpatialState.shaderTable = m_smokeCleanRtxdiDiSpatialShaderTable;
            commandList->setRayTracingState(cleanSpatialState);
            PathTraceCleanRtxdiDiSentinelConstants cleanSpatialConstants = cleanConstants;
            cleanSpatialConstants.emissiveDistributionInfo[3] = static_cast<float>(cleanMaterialOverlayRecordCount);
            commandList->writeBuffer(m_smokeCleanRtxdiDiSentinelConstantsBuffer, &cleanSpatialConstants, sizeof(cleanSpatialConstants));
            {
                PathTraceGpuMarkerScope nsightMarker(commandList, "CleanDI.2 Spatial DispatchRays", nsightGpuMarkers);
                commandList->dispatchRays(cleanArgs);
            }
            nvrhi::utils::BufferUavBarrier(commandList, m_smokeCleanRtxdiDiSpatialReservoirBuffer);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.outputTexture);
            nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrInputColorTexture);
        }
        DispatchPathTraceCleanMaterialFeaturePass(
            commandList,
            cleanState,
            cleanArgs,
            m_smokeCleanRtxdiDiSentinelConstantsBuffer,
            cleanConstants,
            cleanRtxdiDiTransmissionPass,
            m_frameResources,
            nsightGpuMarkers);
        if (cleanRtxdiDiRrGuideDebugView)
        {
            commandList->clearTextureFloat(m_frameResources.rrGuideHitDistanceTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));
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
        auto dispatchCleanRestirGi = [&](bool resolveToRrInputColor) -> bool
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
            giInputs.staticVertexBuffer = m_smokeStaticVertexBuffer;
            giInputs.staticIndexBuffer = m_smokeStaticIndexBuffer;
            giInputs.dynamicVertexBuffer = m_smokeDynamicVertexBuffer;
            giInputs.dynamicIndexBuffer = m_smokeDynamicIndexBuffer;
            giInputs.staticTriangleClassBuffer = m_smokeStaticTriangleClassBuffer;
            giInputs.dynamicTriangleClassBuffer = m_smokeDynamicTriangleClassBuffer;
            giInputs.staticTriangleMaterialBuffer = m_smokeStaticTriangleMaterialBuffer;
            giInputs.dynamicTriangleMaterialBuffer = m_smokeDynamicTriangleMaterialBuffer;
            giInputs.staticTriangleMaterialIndexBuffer = m_smokeStaticTriangleMaterialIndexBuffer;
            giInputs.dynamicTriangleMaterialIndexBuffer = m_smokeDynamicTriangleMaterialIndexBuffer;
            giInputs.materialTableBuffer = m_smokeMaterialTableBuffer;
            giInputs.dynamicMaterialBuffer = m_smokeDynamicMaterialBuffer;
            giInputs.fallbackTexture = cleanFallbackTexture;
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
            giInputs.doomAnalyticLightBuffer = cleanGiNeedsDoomAnalyticLights
                ? m_smokeDoomAnalyticLightBuffer
                : cleanOptionalSrv(m_smokeDoomAnalyticLightBuffer);
            giInputs.rluCurrentLightBuffer = cleanGiNeedsRluCurrentLights
                ? m_smokeRestirLightManagerCurrentPayloadBuffer
                : cleanOptionalSrv(m_smokeRestirLightManagerCurrentPayloadBuffer);
            giInputs.neeCacheProviderResultBuffer = cleanNeeCacheProviderSrv;
            giInputs.neeCacheCellBuffer = cleanNeeCacheCellSrv;
            giInputs.neeCacheCandidateBuffer = cleanNeeCacheCandidateSrv;
            giInputs.primarySurfaceCurrentBuffer = m_frameResources.primarySurfaceHistoryBuffers.current;
            giInputs.primarySurfacePreviousBuffer = m_frameResources.primarySurfaceHistoryBuffers.previous;
            giInputs.motionVectorTexture = m_frameResources.motionVectorTexture;
            giInputs.motionVectorMaskTexture = m_frameResources.motionVectorMaskTexture;
            giInputs.rrInputColorTexture = m_frameResources.rrInputColorTexture;
            giInputs.rrGuideAlbedoTexture = m_frameResources.rrGuideAlbedoTexture;
            giInputs.rrGuideHitDistanceTexture = m_frameResources.rrGuideHitDistanceTexture;
            giInputs.materialSampler = m_backend->GetCommonPasses().m_AnisotropicWrapSampler;
            giInputs.resolveToRrInputColor = resolveToRrInputColor;
            return PathTraceCleanRestirGiExecute(m_cleanRestirGiState, giInputs);
        };
        const bool cleanDlssRrEvaluateRequested =
            cleanSpatialRoute &&
            (cleanRtxdiDiView == 12 || cleanRtxdiDiView == 16) &&
            r_pathTracingDLSSRR.GetInteger() != 0 &&
            r_pathTracingDLSSRRGuideDebugView.GetInteger() == 0;
        const bool cleanGiDispatchRequested =
            r_pathTracingCleanRestirGiEnable.GetInteger() != 0 ||
            r_pathTracingCleanRestirGiDump.GetInteger() != 0;
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
            commandList->clearTextureFloat(m_frameResources.rrGuideHitDistanceTexture, nvrhi::AllSubresources, nvrhi::Color(0.0f, 0.0f, 0.0f, 0.0f));
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
                dispatchCleanRestirGi(cleanGiView0ResolveRequested);
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
            QueueDLSSRRInputColorDump(commandList, m_frameResources.rrInputColorTexture, 1, cleanConstants.frameIndex);
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
            dispatchCleanRestirGi(false);
        }
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
        if (r_pathTracingCleanRtxdiDiTemporalAudit.GetInteger() != 0)
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
            if (pdfNeeVerifierDumpRequested)
            {
                printPdfNeeVerifierDump("dispatch-entry", "pdfnee-rlu-current-producer-shader");
                r_pathTracingRestirPdfNeeVerifierDump.SetInteger(0);
            }
            return;
        }
    }
    else if (pdfNeeVerifierRouteRequested)
    {
        if (!m_smokePdfNeeVerifierShaderTable)
        {
            InitRayTracingSmokeRestirPipeline(16);
        }
        if (!m_smokePdfNeeVerifierShaderTable)
        {
            if (pdfNeeVerifierDumpRequested)
            {
                printPdfNeeVerifierDump("dispatch-entry", "pdfnee-shader");
                r_pathTracingRestirPdfNeeVerifierDump.SetInteger(0);
            }
            return;
        }
    }
    if (regirDebugRouteRequested || pdfNeeReGIRBuildPrepassRequested)
    {
        if (!m_smokeReGIRDebugShaderTable)
        {
            InitRayTracingSmokeRestirPipeline(17);
        }
        if (!m_smokeReGIRDebugShaderTable)
        {
            if (pdfNeeVerifierDumpRequested)
            {
                printPdfNeeVerifierDump("dispatch-entry", "regir-build-shader");
                r_pathTracingRestirPdfNeeVerifierDump.SetInteger(0);
            }
            if (r_pathTracingReGIRDump.GetInteger() != 0)
            {
                printReGIRDump("dispatch-entry", "regir-debug-shader");
                r_pathTracingReGIRDump.SetInteger(0);
            }
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
            if (r_pathTracingNeeCacheDump.GetInteger() != 0)
            {
                printNeeCacheDump("dispatch-entry", "nee-cache-debug-shader");
                r_pathTracingNeeCacheDump.SetInteger(0);
            }
            return;
        }
    }

    nvrhi::BindingSetHandle neeCacheDebugBindingSet;
    if (neeCacheRouteRequested)
    {
        if (!device)
        {
            if (r_pathTracingNeeCacheDump.GetInteger() != 0)
            {
                printNeeCacheDump("dispatch-entry", "device");
                r_pathTracingNeeCacheDump.SetInteger(0);
            }
            return;
        }
        if (!m_smokeNeeCacheState.providerResultBuffer ||
            !m_smokeNeeCacheState.cellBuffer ||
            !m_smokeNeeCacheState.taskBuffer ||
            !m_smokeNeeCacheState.candidateBuffer)
        {
            if (r_pathTracingNeeCacheDump.GetInteger() != 0)
            {
                printNeeCacheDump("dispatch-entry", "nee-cache-provider-buffers");
                r_pathTracingNeeCacheDump.SetInteger(0);
            }
            return;
        }

        nvrhi::BindingSetDesc neeCacheBindingSetDesc;
        auto neeCacheOptionalSrv = [&](const nvrhi::BufferHandle& buffer) -> nvrhi::BufferHandle {
            return buffer ? buffer : m_smokeNeeCacheState.placeholderSrvBuffer;
        };
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, m_smokeTlas));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(1, m_frameResources.outputTexture));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(2, m_smokeConstantsBuffer));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, neeCacheOptionalSrv(m_smokeStaticVertexBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, neeCacheOptionalSrv(m_smokeStaticIndexBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, neeCacheOptionalSrv(m_smokeDynamicVertexBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, neeCacheOptionalSrv(m_smokeDynamicIndexBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(16, neeCacheOptionalSrv(m_smokeEmissiveTriangleBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(22, neeCacheOptionalSrv(m_smokeRigidRouteVertexBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(23, neeCacheOptionalSrv(m_smokeRigidRouteIndexBuffer)));
        neeCacheBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(26, neeCacheOptionalSrv(m_smokeRigidRouteInstanceBuffer)));
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
            if (r_pathTracingNeeCacheDump.GetInteger() != 0)
            {
                printNeeCacheDump("dispatch-entry", "nee-cache-debug-binding-set");
                r_pathTracingNeeCacheDump.SetInteger(0);
            }
            return;
        }
    }

    nvrhi::BindingSetHandle regirDebugBindingSet;
    if (regirDebugRouteRequested || pdfNeeReGIRBuildPrepassRequested)
    {
        if (!device)
        {
            if (pdfNeeVerifierDumpRequested)
            {
                printPdfNeeVerifierDump("dispatch-entry", "device");
                r_pathTracingRestirPdfNeeVerifierDump.SetInteger(0);
            }
            if (r_pathTracingReGIRDump.GetInteger() != 0)
            {
                printReGIRDump("dispatch-entry", "device");
                r_pathTracingReGIRDump.SetInteger(0);
            }
            return;
        }
        if (!m_smokeReGIRState.candidateCacheBuffer)
        {
            if (pdfNeeVerifierDumpRequested)
            {
                printPdfNeeVerifierDump("dispatch-entry", "regir-candidate-cache");
                r_pathTracingRestirPdfNeeVerifierDump.SetInteger(0);
            }
            if (r_pathTracingReGIRDump.GetInteger() != 0)
            {
                printReGIRDump("dispatch-entry", "candidate-cache-buffer");
                r_pathTracingReGIRDump.SetInteger(0);
            }
            return;
        }

        auto regirOptionalSrv = [&](const nvrhi::BufferHandle& buffer) -> nvrhi::BufferHandle {
            return buffer ? buffer : m_smokeReGIRState.placeholderSrvBuffer;
        };

        nvrhi::BindingSetDesc regirBindingSetDesc;
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::RayTracingAccelStruct(0, m_smokeTlas));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(1, m_frameResources.outputTexture));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::ConstantBuffer(2, m_smokeConstantsBuffer));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(3, regirOptionalSrv(m_smokeStaticVertexBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(4, regirOptionalSrv(m_smokeStaticIndexBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(5, regirOptionalSrv(m_smokeStaticTriangleClassBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(6, regirOptionalSrv(m_smokeDynamicVertexBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(7, regirOptionalSrv(m_smokeDynamicIndexBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(8, regirOptionalSrv(m_smokeDynamicTriangleClassBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(9, regirOptionalSrv(m_smokeStaticTriangleMaterialBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(10, regirOptionalSrv(m_smokeDynamicTriangleMaterialBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(11, regirOptionalSrv(m_smokeStaticTriangleMaterialIndexBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(12, regirOptionalSrv(m_smokeDynamicTriangleMaterialIndexBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(13, regirOptionalSrv(m_smokeMaterialTableBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(16, regirOptionalSrv(m_smokeEmissiveTriangleBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(22, regirOptionalSrv(m_smokeRigidRouteVertexBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(23, regirOptionalSrv(m_smokeRigidRouteIndexBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(24, regirOptionalSrv(m_smokeRigidRouteTriangleMaterialBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(25, regirOptionalSrv(m_smokeRigidRouteTriangleMaterialIndexBuffer)));
        regirBindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(26, regirOptionalSrv(m_smokeRigidRouteInstanceBuffer)));
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
            if (pdfNeeVerifierDumpRequested)
            {
                printPdfNeeVerifierDump("dispatch-entry", "regir-build-binding-set");
                r_pathTracingRestirPdfNeeVerifierDump.SetInteger(0);
            }
            if (r_pathTracingReGIRDump.GetInteger() != 0)
            {
                printReGIRDump("dispatch-entry", "regir-debug-binding-set");
                r_pathTracingReGIRDump.SetInteger(0);
            }
            return;
        }
    }

    nvrhi::BindingSetHandle pdfNeeVerifierBindingSet;
    if (pdfNeeVerifierRouteRequested || pdfNeeRluCurrentProducerRequested)
    {
        if (!device)
        {
            if (pdfNeeVerifierDumpRequested)
            {
                printPdfNeeVerifierDump("dispatch-entry", "device");
                r_pathTracingRestirPdfNeeVerifierDump.SetInteger(0);
            }
            return;
        }
        const nvrhi::TextureHandle pdfNeeFallbackTexture = !m_smokeActiveTextureTable.empty() ? m_smokeActiveTextureTable[0] : nullptr;
        if (!pdfNeeFallbackTexture)
        {
            if (pdfNeeVerifierDumpRequested)
            {
                printPdfNeeVerifierDump("dispatch-entry", "fallback-texture");
                r_pathTracingRestirPdfNeeVerifierDump.SetInteger(0);
            }
            return;
        }

        const uint64 pdfNeeCleanReservoirBlockSize = 16;
        const uint64 pdfNeeCleanReservoirBlocksX = (static_cast<uint64>(Max(1, m_frameResources.width)) + pdfNeeCleanReservoirBlockSize - 1ull) / pdfNeeCleanReservoirBlockSize;
        const uint64 pdfNeeCleanReservoirBlocksY = (static_cast<uint64>(Max(1, m_frameResources.height)) + pdfNeeCleanReservoirBlockSize - 1ull) / pdfNeeCleanReservoirBlockSize;
        const uint64 pdfNeeCleanReservoirCount64 = pdfNeeCleanReservoirBlocksX * pdfNeeCleanReservoirBlocksY * pdfNeeCleanReservoirBlockSize * pdfNeeCleanReservoirBlockSize;
        if (pdfNeeCleanReservoirCount64 > 0xffffffffull)
        {
            if (pdfNeeVerifierDumpRequested)
            {
                printPdfNeeVerifierDump("dispatch-entry", "clean-current-reservoir-size");
                r_pathTracingRestirPdfNeeVerifierDump.SetInteger(0);
            }
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
            if (pdfNeeVerifierDumpRequested)
            {
                printPdfNeeVerifierDump("dispatch-entry", "clean-reservoir-pages");
                r_pathTracingRestirPdfNeeVerifierDump.SetInteger(0);
            }
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
            nvrhi::BindingSetItem::StructuredBuffer_SRV(3, m_smokeStaticVertexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(4, m_smokeStaticIndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(5, m_smokeStaticTriangleClassBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(6, m_smokeDynamicVertexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(7, m_smokeDynamicIndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(8, m_smokeDynamicTriangleClassBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(9, m_smokeStaticTriangleMaterialBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(10, m_smokeDynamicTriangleMaterialBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(11, m_smokeStaticTriangleMaterialIndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(12, m_smokeDynamicTriangleMaterialIndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(13, m_smokeMaterialTableBuffer),
            nvrhi::BindingSetItem::Texture_SRV(14, pdfNeeFallbackTexture),
            nvrhi::BindingSetItem::Texture_UAV(15, m_frameResources.accumulationTexture),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(16, m_smokeEmissiveTriangleBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(57, m_smokePreviousEmissiveTriangleBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(58, m_smokeEmissiveRemapBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(46, m_smokeEmissiveDistributionBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(17, m_smokeLightCandidateBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(18, m_frameResources.smokeReservoirBuffers.current),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(19, m_frameResources.smokeReservoirBuffers.previous),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(20, m_frameResources.smokeReservoirBuffers.spatialScratch),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(21, m_smokeBoundsOverlayLineBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(22, m_smokeRigidRouteVertexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(23, m_smokeRigidRouteIndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(24, m_smokeRigidRouteTriangleMaterialBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(25, m_smokeRigidRouteTriangleMaterialIndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(26, m_smokeRigidRouteInstanceBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(27, m_smokeDoomAnalyticLightBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(45, m_smokeDoomAnalyticPreviousLightBuffer),
            nvrhi::BindingSetItem::ConstantBuffer(28, m_restirPTConstantsBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(29, m_frameResources.restirPTReservoirBuffers.reservoirs),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(30, m_frameResources.primarySurfaceHistoryBuffers.current),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(31, m_frameResources.primarySurfaceHistoryBuffers.previous),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(32, m_smokeSkinnedPreviousPositionBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(33, m_smokeSkinnedSurfaceDispatchBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(34, m_smokePreviousStaticVertexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(35, m_smokePreviousStaticIndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(36, m_smokePreviousStaticTriangleClassBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(37, m_smokePreviousStaticTriangleMaterialBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(38, m_smokePreviousStaticTriangleMaterialIndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(42, m_smokeDoomAnalyticCurrentIdentityBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(43, m_smokeDoomAnalyticPreviousIdentityBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(44, m_smokeDoomAnalyticRemapBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(59, m_smokeUnifiedLightBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(60, m_smokeUnifiedPreviousLightBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(61, m_smokeUnifiedLightRemapBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(62, m_smokeRestirLightManagerCurrentBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(63, m_smokeRestirLightManagerPreviousBuffer),
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
            nvrhi::BindingSetItem::StructuredBuffer_UAV(55, m_frameResources.restirPTGiReservoirBuffers.reservoirs),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(56, m_frameResources.restirPTDiReservoirBuffers.reservoirs),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(68, m_remixRtxdiResources.GetDomain(PATH_TRACE_REMIX_RTXDI_RESERVOIR_DOMAIN_DI).reservoirs ? m_remixRtxdiResources.GetDomain(PATH_TRACE_REMIX_RTXDI_RESERVOIR_DOMAIN_DI).reservoirs : m_frameResources.restirPTDiReservoirBuffers.reservoirs),
            nvrhi::BindingSetItem::Sampler(0, m_backend->GetCommonPasses().m_AnisotropicWrapSampler),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(69, m_smokeCleanRtxdiDiCurrentReservoirBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(70, m_smokeCleanRtxdiDiTemporalReservoirBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_UAV(71, m_smokeCleanRtxdiDiPreviousReservoirBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(73, pdfNeeReGIRCandidateSrv)
        };
        pdfNeeVerifierBindingSet = device->createBindingSet(pdfNeeBindingSetDesc, m_smokePdfNeeVerifierBindingLayout);
        if (!pdfNeeVerifierBindingSet)
        {
            if (pdfNeeVerifierDumpRequested)
            {
                printPdfNeeVerifierDump("dispatch-entry", "pdfnee-binding-set");
                r_pathTracingRestirPdfNeeVerifierDump.SetInteger(0);
            }
            return;
        }
    }

    int debugMode = standaloneDebugRouteRequested ? 0 : idMath::ClampInt(0, 57, r_pathTracingDebugMode.GetInteger());
    m_frameResources.settings.debugMode = debugMode;
    m_frameResources.settings.checkerboardMode = RtRestirPTCheckerboardMode::Off;
    const bool requestedRestirPTDebugMode = IsPathTraceRestirPTDebugMode(debugMode);
    const bool requestedIntegratorDebugMode = debugMode >= 34 && debugMode <= 37;
    if ((debugMode == 8 || debugMode == 9 || debugMode == 10 || debugMode == 11 || debugMode == 12 || debugMode == 13 || debugMode == 14 || debugMode == 15 || debugMode == 18 || debugMode == 19 || debugMode == 20 || debugMode == 38 || debugMode == 39 || debugMode == 40 || debugMode == 41 || debugMode == 42 || debugMode == 43 || debugMode == 44 || debugMode == 45 || debugMode == 46 || debugMode == 47 || debugMode == 48 || debugMode == 49 || requestedRestirPTDebugMode || requestedIntegratorDebugMode) && r_pathTracingTextureTableLimit.GetInteger() <= 0)
    {
        debugMode = 7;
    }
    const bool restirPTDebugMode = IsPathTraceRestirPTDebugMode(debugMode);
    const bool mode18RestirDirectMode = debugMode == 18 && r_pathTracingRestirPTDirectLighting.GetInteger() != 0;
    const bool effectiveRestirPTMode = restirPTDebugMode || mode18RestirDirectMode;
    const bool integratorDebugMode = debugMode >= 34 && debugMode <= 37;
    const bool restirPTInitialOnlyMode = (debugMode >= 26 && debugMode <= 28) || (debugMode >= 53 && debugMode <= 55);
    const bool restirPTTemporalMode = debugMode == 31;
    const bool restirPTTemporalShadingMode = debugMode == 32;
    const bool restirPTAttributionMode = debugMode == 33;
    const bool restirPTSpatialShadingMode = debugMode == 50;
    const bool restirPTSpatialAttributionMode = debugMode == 51;
    const bool restirPTCombinedMode = debugMode == 56;
    const int restirPTDiDebugView = restirPTCombinedMode ? idMath::ClampInt(0, 77, r_pathTracingRestirPTDiDebugView.GetInteger()) : 0;
    const uint32_t safetyDisableMask = BuildPathTraceSafetyDisableMask();
    const bool disableSelectedLightLoop = PathTraceSafetyDisabled(safetyDisableMask, RT_PT_SAFETY_DISABLE_SELECTED_LIGHT_LOOP);
    const bool disableAnalyticLightLoop = PathTraceSafetyDisabled(safetyDisableMask, RT_PT_SAFETY_DISABLE_ANALYTIC_LIGHT_LOOP);
    const bool disableEmissiveTriangleSampling = PathTraceSafetyDisabled(safetyDisableMask, RT_PT_SAFETY_DISABLE_EMISSIVE_TRIANGLE_SAMPLING);
    const bool disablePrimarySurfaceHistory = PathTraceSafetyDisabled(safetyDisableMask, RT_PT_SAFETY_DISABLE_PRIMARY_SURFACE_HISTORY);
    const bool disableReservoirWrites = PathTraceSafetyDisabled(safetyDisableMask, RT_PT_SAFETY_DISABLE_RESERVOIR_WRITES);
    const bool dlssRrRuntimeRequested =
        restirPTCombinedMode &&
        r_pathTracingDLSSRR.GetInteger() != 0 &&
        r_pathTracingDLSSRRGuideDebugView.GetInteger() == 0;
    const bool restirPTRequiresMotionVectors = restirPTCombinedMode && !disablePrimarySurfaceHistory;
    const bool motionVectorExportEnabled = r_pathTracingMotionVectorExport.GetInteger() != 0 || dlssRrRuntimeRequested || restirPTRequiresMotionVectors;
    const int restirPTReflectionMode = PathTraceSafetyDisabled(safetyDisableMask, RT_PT_SAFETY_DISABLE_REFLECTION_RAY)
        ? 0
        : idMath::ClampInt(0, 2, r_pathTracingRestirPTReflectionMode.GetInteger());
    const bool restirPTCombinedResolveRequested =
        restirPTCombinedMode &&
        !disablePrimarySurfaceHistory &&
        r_pathTracingRestirPTPrimarySurfacePrepass.GetInteger() != 0;
    if (restirPTCombinedResolveRequested && !m_smokePrimarySurfaceProducerShaderTable)
    {
        InitRayTracingSmokeRestirPipeline(9);
    }
    if (restirPTInitialOnlyMode && !m_smokeRestirInitialShaderTable)
    {
        InitRayTracingSmokeRestirPipeline(0);
    }
    else if (restirPTTemporalMode && !m_smokeRestirShaderTable)
    {
        InitRayTracingSmokeRestirPipeline(1);
    }
    else if (restirPTTemporalShadingMode && !m_smokeRestirTemporalShadingShaderTable)
    {
        InitRayTracingSmokeRestirPipeline(2);
    }
    else if (restirPTAttributionMode && !m_smokeRestirAttributionShaderTable)
    {
        InitRayTracingSmokeRestirPipeline(3);
    }
    if (restirPTCombinedResolveRequested && !m_smokeRestirDirectSpatialReservoirProducerShaderTable)
    {
        InitRayTracingSmokeRestirPipeline(13);
    }
    const bool restirPTStandaloneDirectSpatialReservoirProducerAvailable =
        restirPTCombinedResolveRequested && m_smokeRestirDirectSpatialReservoirProducerShaderTable;
    if ((restirPTSpatialShadingMode || restirPTSpatialAttributionMode ||
        (restirPTCombinedMode && !restirPTStandaloneDirectSpatialReservoirProducerAvailable)) &&
        !m_smokeRestirSpatialReservoirShaderTable)
    {
        InitRayTracingSmokeRestirPipeline(4);
    }
    if (restirPTSpatialShadingMode && !m_smokeRestirSpatialShaderTable)
    {
        InitRayTracingSmokeRestirPipeline(5);
    }
    if (restirPTSpatialAttributionMode && !m_smokeRestirSpatialAttributionShaderTable)
    {
        InitRayTracingSmokeRestirPipeline(6);
    }
    if (restirPTCombinedResolveRequested && !m_smokeRestirDirectTemporalProducerShaderTable)
    {
        InitRayTracingSmokeRestirPipeline(12);
    }
    const bool restirPTStandaloneDirectTemporalProducerAvailable =
        restirPTCombinedResolveRequested && m_smokeRestirDirectTemporalProducerShaderTable;
    if ((restirPTSpatialShadingMode || restirPTSpatialAttributionMode ||
        (restirPTCombinedMode && !restirPTStandaloneDirectTemporalProducerAvailable)) &&
        !m_smokeRestirShaderTable)
    {
        InitRayTracingSmokeRestirPipeline(1);
    }
    if (restirPTCombinedResolveRequested && !m_smokeRestirIndirectInitialProducerShaderTable)
    {
        InitRayTracingSmokeRestirPipeline(11);
    }
    const bool restirPTStandaloneIndirectInitialProducerAvailable =
        restirPTCombinedResolveRequested && m_smokeRestirIndirectInitialProducerShaderTable;
    if ((debugMode >= 53 && debugMode <= 56) &&
        !restirPTStandaloneIndirectInitialProducerAvailable &&
        !m_smokeRestirInitialShaderTable)
    {
        InitRayTracingSmokeRestirPipeline(0);
    }
    if (restirPTCombinedResolveRequested && m_smokePrimarySurfaceProducerShaderTable && !m_smokeRestirCombinedResolveShaderTable)
    {
        InitRayTracingSmokeRestirPipeline(10);
    }
    if (restirPTCombinedResolveRequested && restirPTReflectionMode > 0 && !m_smokeRestirReflectionProducerShaderTable)
    {
        InitRayTracingSmokeRestirPipeline(14);
    }
    if (restirPTCombinedMode && !m_smokeRestirCombinedResolveShaderTable && !m_smokeRestirCombinedShaderTable)
    {
        InitRayTracingSmokeRestirPipeline(8);
    }
    if (mode18RestirDirectMode)
    {
        if (!m_smokeRestirShaderTable)
        {
            InitRayTracingSmokeRestirPipeline(1);
        }
        if (!m_smokeRestirSpatialReservoirShaderTable)
        {
            InitRayTracingSmokeRestirPipeline(4);
        }
        if (!m_smokeMode18RestirHybridShaderTable)
        {
            InitRayTracingSmokeRestirPipeline(7);
        }
    }
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
    else if (pdfNeeVerifierRouteRequested && m_smokePdfNeeVerifierShaderTable)
    {
        state.shaderTable = m_smokePdfNeeVerifierShaderTable;
    }
    else if (mode18RestirDirectMode && m_smokeMode18RestirHybridShaderTable)
    {
        state.shaderTable = m_smokeMode18RestirHybridShaderTable;
    }
    else if (restirPTInitialOnlyMode && m_smokeRestirInitialShaderTable)
    {
        state.shaderTable = m_smokeRestirInitialShaderTable;
    }
    else if (restirPTTemporalMode && m_smokeRestirShaderTable)
    {
        state.shaderTable = m_smokeRestirShaderTable;
    }
    else if (restirPTTemporalShadingMode && m_smokeRestirTemporalShadingShaderTable)
    {
        state.shaderTable = m_smokeRestirTemporalShadingShaderTable;
    }
    else if (restirPTAttributionMode && m_smokeRestirAttributionShaderTable)
    {
        state.shaderTable = m_smokeRestirAttributionShaderTable;
    }
    else if (restirPTSpatialShadingMode && m_smokeRestirSpatialShaderTable)
    {
        state.shaderTable = m_smokeRestirSpatialShaderTable;
    }
    else if (restirPTSpatialAttributionMode && m_smokeRestirSpatialAttributionShaderTable)
    {
        state.shaderTable = m_smokeRestirSpatialAttributionShaderTable;
    }
    else if (restirPTCombinedMode && restirPTCombinedResolveRequested && m_smokePrimarySurfaceProducerShaderTable && m_smokeRestirCombinedResolveShaderTable)
    {
        state.shaderTable = m_smokeRestirCombinedResolveShaderTable;
    }
    else if (restirPTCombinedMode && m_smokeRestirCombinedShaderTable)
    {
        state.shaderTable = m_smokeRestirCombinedShaderTable;
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
            : ((pdfNeeVerifierRouteRequested || pdfNeeRluCurrentProducerRequested) && pdfNeeVerifierBindingSet
            ? pdfNeeVerifierBindingSet
            : (neeCacheSecondaryBindingSet ? neeCacheSecondaryBindingSet : m_smokeBindingSet)));
    state.bindings = { activeBindingSet, m_smokeTextureDescriptorTable };
    const bool restirPTCombinedResolveActive =
        restirPTCombinedMode &&
        restirPTCombinedResolveRequested &&
        state.shaderTable == m_smokeRestirCombinedResolveShaderTable;
    const bool restirPTPreviewVisibility = r_pathTracingRestirPTPreviewVisibility.GetInteger() != 0 && !PathTraceSafetyDisabled(safetyDisableMask, RT_PT_SAFETY_DISABLE_RESTIR_VISIBILITY_RAY);
    RtPathTraceRestirPassPlan restirPTPassPlan = BuildPathTraceRestirPassPlan(debugMode, restirPTPreviewVisibility);
    if (mode18RestirDirectMode)
    {
        restirPTPassPlan.producer = RtPathTraceRestirPassKind::SpatialReservoir;
        restirPTPassPlan.output = RtPathTraceRestirPassKind::ReservoirShading;
        restirPTPassPlan.resamplingMode = RtRestirPTResamplingMode::TemporalAndSpatial;
        restirPTPassPlan.flags =
            RT_RESTIR_PASS_WRITES_INITIAL |
            RT_RESTIR_PASS_WRITES_TEMPORAL |
            RT_RESTIR_PASS_WRITES_SPATIAL |
            RT_RESTIR_PASS_CONSUMES_CURRENT_SURFACE |
            RT_RESTIR_PASS_CONSUMES_PREVIOUS_SURFACE |
            RT_RESTIR_PASS_CONSUMES_PREVIOUS_RESERVOIR |
            RT_RESTIR_PASS_SHADES_RESERVOIR |
            RT_RESTIR_PASS_TRACES_VISIBILITY |
            RT_RESTIR_PASS_REQUIRES_TEMPORAL_PREPASS |
            RT_RESTIR_PASS_REQUIRES_SPATIAL_PREPASS;
        restirPTPassPlan.label = "mode18RestirDirectLightingHybrid";
    }
    const bool stagedRestirDirectLightingMode = restirPTPassPlan.resamplingMode == RtRestirPTResamplingMode::TemporalAndSpatial;
    const float restirPTDirectResolutionScale = stagedRestirDirectLightingMode
        ? idMath::ClampFloat(0.25f, 1.0f, r_pathTracingRestirPTDirectResolutionScale.GetFloat())
        : 1.0f;
    const int restirPTDirectWidth = stagedRestirDirectLightingMode
        ? PathTraceScaledRestirDimension(m_frameResources.width, restirPTDirectResolutionScale)
        : m_frameResources.width;
    const int restirPTDirectHeight = stagedRestirDirectLightingMode
        ? PathTraceScaledRestirDimension(m_frameResources.height, restirPTDirectResolutionScale)
        : m_frameResources.height;
    const int restirPTRaySparsity = stagedRestirDirectLightingMode
        ? idMath::ClampInt(1, 8, r_pathTracingRestirPTRaySparsity.GetInteger())
        : 1;
    const int restirPTRaySparsityPhase = restirPTRaySparsity > 1
        ? static_cast<int>(m_frameResources.restirPTFrameIndex % static_cast<uint32_t>(restirPTRaySparsity))
        : 0;
    const int restirPTRaySparsityPreviousPhase = restirPTRaySparsity > 1
        ? (restirPTRaySparsityPhase + restirPTRaySparsity - 1) % restirPTRaySparsity
        : 0;
    const int restirPTDirectProducerWidth = restirPTRaySparsity > 1
        ? (restirPTDirectWidth + restirPTRaySparsity - 1) / restirPTRaySparsity
        : restirPTDirectWidth;
    const bool stagedRestirGiInitialMode = PathTraceRestirPassRequiresInitialPrepass(restirPTPassPlan);
    const int restirPTGiRaySparsity = stagedRestirGiInitialMode
        ? idMath::ClampInt(1, 8, r_pathTracingRestirPTGiRaySparsity.GetInteger())
        : 1;
    const int restirPTGiRaySparsityPhase = restirPTGiRaySparsity > 1
        ? static_cast<int>(m_frameResources.restirPTFrameIndex % static_cast<uint32_t>(restirPTGiRaySparsity))
        : 0;
    const int restirPTGiProducerWidth = restirPTGiRaySparsity > 1
        ? (m_frameResources.width + restirPTGiRaySparsity - 1) / restirPTGiRaySparsity
        : m_frameResources.width;
    const bool restirPTPrimarySurfacePrepassRequested =
        stagedRestirDirectLightingMode &&
        !disablePrimarySurfaceHistory &&
        r_pathTracingRestirPTPrimarySurfacePrepass.GetInteger() != 0;
    if (restirPTPrimarySurfacePrepassRequested && !m_smokePrimarySurfaceProducerShaderTable)
    {
        InitRayTracingSmokeRestirPipeline(9);
    }
    const bool restirPTPrimarySurfacePrepassEnabled =
        restirPTPrimarySurfacePrepassRequested &&
        m_smokePrimarySurfaceProducerShaderTable;
    const bool restirPTStandalonePrimarySurfacePrepass =
        restirPTPrimarySurfacePrepassEnabled;
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
    const float effectiveAnalyticLightIntensityScale = effectiveRestirPTMode
        ? idMath::ClampFloat(0.0f, 16.0f, analyticLightIntensityScale * toyLightScale)
        : analyticLightIntensityScale;
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
    const bool mode56AccumulationActive = debugMode == 56 && r_pathTracingRestirPTMode56Accumulation.GetInteger() != 0;
    uint64 mode56AccumulationSignature = accumulationSignature;
    mode56AccumulationSignature = HashSmokeDispatchValue(mode56AccumulationSignature, static_cast<uint64>(r_pathTracingRestirPTMode56Accumulation.GetInteger() != 0 ? 1 : 0));
    mode56AccumulationSignature = HashSmokeDispatchValue(mode56AccumulationSignature, static_cast<uint64>(idMath::ClampInt(1, 4096, r_pathTracingRestirPTMode56AccumulationMaxFrames.GetInteger())));
    mode56AccumulationSignature = HashSmokeDispatchValue(mode56AccumulationSignature, static_cast<uint64>(idMath::ClampInt(0, 10, r_pathTracingDLSSRRGuideDebugView.GetInteger())));
    mode56AccumulationSignature = HashSmokeDispatchValue(mode56AccumulationSignature, static_cast<uint64>(idMath::ClampInt(0, 4, r_pathTracingRestirPTGiDebugView.GetInteger())));
    mode56AccumulationSignature = HashSmokeDispatchValue(mode56AccumulationSignature, static_cast<uint64>(restirPTDiDebugView));
    if (!mode56AccumulationActive || mode56AccumulationSignature != m_frameResources.mode56AccumulationSignature)
    {
        m_frameResources.mode56AccumulationSignature = mode56AccumulationSignature;
        m_frameResources.mode56AccumulationFrameCount = 0;
    }
    const int mode56AccumulationMaxFrames = idMath::ClampInt(1, 4096, r_pathTracingRestirPTMode56AccumulationMaxFrames.GetInteger());
    const int accumulationFrameCount = mode18AccumulationActive
        ? Min(m_frameResources.smokeAccumulationFrameCount, accumulationMaxFrames - 1)
        : (mode56AccumulationActive
            ? Min(m_frameResources.mode56AccumulationFrameCount, mode56AccumulationMaxFrames - 1)
            : 0);
    const bool accumulationTextureActive = mode18AccumulationActive || mode56AccumulationActive;

    uint64 reservoirDispatchSignature = 1469598103934665603ull;
    reservoirDispatchSignature = HashSmokeBytes(reservoirDispatchSignature, &m_frameResources.smokeReservoirSceneSignature, sizeof(m_frameResources.smokeReservoirSceneSignature));
    reservoirDispatchSignature = HashSmokeBytes(reservoirDispatchSignature, &m_frameResources.width, sizeof(m_frameResources.width));
    reservoirDispatchSignature = HashSmokeBytes(reservoirDispatchSignature, &m_frameResources.height, sizeof(m_frameResources.height));
    reservoirDispatchSignature = HashSmokeFloatQuantized(reservoirDispatchSignature, cameraOrigin.x, 100.0f);
    reservoirDispatchSignature = HashSmokeFloatQuantized(reservoirDispatchSignature, cameraOrigin.y, 100.0f);
    reservoirDispatchSignature = HashSmokeFloatQuantized(reservoirDispatchSignature, cameraOrigin.z, 100.0f);
    reservoirDispatchSignature = HashSmokeFloatQuantized(reservoirDispatchSignature, cameraForward.x, 10000.0f);
    reservoirDispatchSignature = HashSmokeFloatQuantized(reservoirDispatchSignature, cameraForward.y, 10000.0f);
    reservoirDispatchSignature = HashSmokeFloatQuantized(reservoirDispatchSignature, cameraForward.z, 10000.0f);
    reservoirDispatchSignature = HashSmokeFloatQuantized(reservoirDispatchSignature, cameraLeft.x, 10000.0f);
    reservoirDispatchSignature = HashSmokeFloatQuantized(reservoirDispatchSignature, cameraLeft.y, 10000.0f);
    reservoirDispatchSignature = HashSmokeFloatQuantized(reservoirDispatchSignature, cameraLeft.z, 10000.0f);
    reservoirDispatchSignature = HashSmokeFloatQuantized(reservoirDispatchSignature, cameraUp.x, 10000.0f);
    reservoirDispatchSignature = HashSmokeFloatQuantized(reservoirDispatchSignature, cameraUp.y, 10000.0f);
    reservoirDispatchSignature = HashSmokeFloatQuantized(reservoirDispatchSignature, cameraUp.z, 10000.0f);
    reservoirDispatchSignature = HashSmokeFloatQuantized(reservoirDispatchSignature, viewDef->renderView.fov_x, 100.0f);
    reservoirDispatchSignature = HashSmokeFloatQuantized(reservoirDispatchSignature, viewDef->renderView.fov_y, 100.0f);
    reservoirDispatchSignature = HashSmokeFloatQuantized(reservoirDispatchSignature, toyLightScale, 1000.0f);
    reservoirDispatchSignature = HashSmokeFloatQuantized(reservoirDispatchSignature, toyEmissiveScale, 1000.0f);
    reservoirDispatchSignature = HashSmokeFloatQuantized(reservoirDispatchSignature, effectiveAnalyticLightIntensityScale, 1000.0f);
    reservoirDispatchSignature = HashSmokeDispatchValue(reservoirDispatchSignature, static_cast<uint64>(m_smokeDoomAnalyticLightCount));
    reservoirDispatchSignature = HashSmokeDispatchValue(reservoirDispatchSignature, static_cast<uint64>(idMath::ClampInt(0, 1024, r_pathTracingAnalyticLightMaxGpu.GetInteger())));
    if (reservoirDispatchSignature != m_frameResources.smokeReservoirDispatchSignature)
    {
        m_frameResources.smokeReservoirDispatchSignature = reservoirDispatchSignature;
        m_frameResources.smokeReservoirNeedsClear = true;
        ++m_frameResources.smokeReservoirResetCount;
        m_frameResources.MarkResetReason(RT_FRAME_RESET_RESERVOIR_DISPATCH_SIGNATURE);
    }

    if (r_pathTracingReservoirDump.GetInteger() != 0)
    {
        common->Printf("PathTracePrimaryPass: RT smoke reservoirs output=%dx%d records=%d bytes=%d sceneSig=%llu dispatchSig=%llu needsClear=%d resets=%d clears=%d emissive=%d/%d/%d lightCandidates=%d/%d(%db) doomAnalytic=%d(%db)\n",
            m_frameResources.width,
            m_frameResources.height,
            m_frameResources.smokeReservoirBuffers.reservoirCount,
            m_frameResources.smokeReservoirBuffers.reservoirBytes,
            static_cast<unsigned long long>(m_frameResources.smokeReservoirSceneSignature),
            static_cast<unsigned long long>(m_frameResources.smokeReservoirDispatchSignature),
            m_frameResources.smokeReservoirNeedsClear ? 1 : 0,
            m_frameResources.smokeReservoirResetCount,
            m_frameResources.smokeReservoirClearCount,
            m_smokeEmissiveTriangleCount,
            m_smokeEmissiveStaticTriangleCount,
            m_smokeEmissiveDynamicTriangleCount,
            m_smokeLightCandidateCount,
            m_smokeTexturedLightCandidateCount,
            m_smokeLightCandidateBytes,
            m_smokeDoomAnalyticLightCount,
            m_smokeDoomAnalyticLightBytes);
        m_frameResources.PrintDiagnostics("PathTracePrimaryPass");
        r_pathTracingReservoirDump.SetInteger(0);
    }

    const uint64 setupCompleteUs = Sys_Microseconds();
    const uint32_t restirPTFrameIndex = m_frameResources.restirPTFrameIndex++;
    m_frameResources.settings.frameIndex = restirPTFrameIndex;
    if (effectiveRestirPTMode &&
        !m_frameResources.restirPTContextState.IsValidFor(static_cast<uint32_t>(restirPTDirectWidth), static_cast<uint32_t>(restirPTDirectHeight), RtRestirPTCheckerboardMode::Off))
    {
        m_frameResources.restirPTReservoirNeedsClear = true;
        m_frameResources.restirPTDiReservoirNeedsClear = true;
        m_frameResources.restirPTGiReservoirNeedsClear = true;
    }
    const float restirPTSpatialRadius = idMath::ClampFloat(1.0f, 128.0f,
        restirPTCombinedMode
            ? r_pathTracingRestirPTCombinedSpatialRadius.GetFloat()
            : r_pathTracingRestirPTSpatialRadius.GetFloat());
    const RtRestirPTContextUpdateDesc restirPTContextDesc = BuildRestirPTContextUpdateDesc(
        restirPTPassPlan,
        static_cast<uint32_t>(restirPTDirectWidth),
        static_cast<uint32_t>(restirPTDirectHeight),
        restirPTFrameIndex,
        RtRestirPTCheckerboardMode::Off,
        idMath::ClampFloat(0.0f, 1.0f, r_pathTracingRestirPTTemporalDepthThreshold.GetFloat()),
        idMath::ClampFloat(-1.0f, 1.0f, r_pathTracingRestirPTTemporalNormalThreshold.GetFloat()),
        r_pathTracingRestirPTTemporalReservoirReuse.GetInteger() != 0,
        r_pathTracingRestirPTTemporalFallbackSampling.GetInteger() != 0,
        static_cast<uint32_t>(idMath::ClampInt(1, 32, r_pathTracingRestirPTSpatialSamples.GetInteger())),
        restirPTSpatialRadius);
    if (!UpdateRestirPTContextState(m_frameResources.restirPTContextState, restirPTContextDesc))
    {
        return;
    }
    const uint64 restirContextCompleteUs = Sys_Microseconds();
    const RtPathTraceRestirPassBufferSelection restirPTBufferSelection = ResolveRestirPTPassBufferSelection(
        restirPTPassPlan,
        m_frameResources.restirPTContextState.parameters);
    if (r_pathTracingRestirPTPassDump.GetInteger() != 0)
    {
        const bool giConsumesPrimary = restirPTPrimarySurfacePrepassEnabled && stagedRestirGiInitialMode;
        const bool giInitialStandalone = giConsumesPrimary && restirPTCombinedMode && m_smokeRestirIndirectInitialProducerShaderTable;
        const bool directTemporalStandalone = restirPTPrimarySurfacePrepassEnabled && restirPTCombinedMode && m_smokeRestirDirectTemporalProducerShaderTable;
        const bool directSpatialStandalone = restirPTPrimarySurfacePrepassEnabled && restirPTCombinedMode && m_smokeRestirDirectSpatialReservoirProducerShaderTable;
        const bool finalUsesStandaloneResolve = restirPTPrimarySurfacePrepassEnabled && restirPTCombinedResolveActive;
        const bool reflectionProducer = finalUsesStandaloneResolve && restirPTReflectionMode > 0 && m_smokeRestirReflectionProducerShaderTable;
        const bool finalConsumesPrimary = restirPTPrimarySurfacePrepassEnabled && restirPTCombinedMode;
        const int restirPTVisibilityPolicy = idMath::ClampInt(0, 2, r_pathTracingRestirPTVisibilityPolicy.GetInteger());
        common->Printf("PathTracePrimaryPass: ReSTIR PT pass plan mode=%d label=%s producer=%s output=%s flags=0x%08x resampling=%d output=%dx%d directDomain=%dx%d directDispatch=%dx%d scale=%.3f sparsity=%d phase=%d prevPhase=%d giDispatch=%dx%d giSparsity=%d giPhase=%d primaryPrepass=%d standalonePrimaryPrepass=%d giConsumesPrimary=%d giInitialStandalone=%d directConsumesPrimary=%d directTemporalStandalone=%d directSpatialStandalone=%d finalConsumesPrimary=%d finalResolve=%d reflectionProducer=%d rrGuideDebug=%d diDebugView=%d giDebugView=%d nsightMarkers=%d buffers initialOut=%u temporalIn=%u temporalOut=%u spatialIn=%u spatialOut=%u finalShadingIn=%u debugIn=%u previewVisibility=%d visibilityPolicy=%d reflectionMode=%d toyLight=%.3f toyEmissive=%.3f analyticScale=%.3f maxPixels=%d temporalThresholds depth=%.3f normal=%.3f temporalReuse=%d temporalFallback=%d materialSimilarity=%d temporalNeighborDebug=%d unifiedPrevToCurrentScan=%d spatial samples=%u radius=%.1f\n",
            debugMode,
            restirPTPassPlan.label,
            PathTraceRestirPassKindName(restirPTPassPlan.producer),
            PathTraceRestirPassKindName(restirPTPassPlan.output),
            restirPTPassPlan.flags,
            static_cast<int>(restirPTPassPlan.resamplingMode),
            m_frameResources.width,
            m_frameResources.height,
            restirPTDirectWidth,
            restirPTDirectHeight,
            restirPTDirectProducerWidth,
            restirPTDirectHeight,
            restirPTDirectResolutionScale,
            restirPTRaySparsity,
            restirPTRaySparsityPhase,
            restirPTRaySparsityPreviousPhase,
            restirPTGiProducerWidth,
            m_frameResources.height,
            restirPTGiRaySparsity,
            restirPTGiRaySparsityPhase,
            restirPTPrimarySurfacePrepassEnabled ? 1 : 0,
            restirPTStandalonePrimarySurfacePrepass ? 1 : 0,
            giConsumesPrimary ? 1 : 0,
            giInitialStandalone ? 1 : 0,
            restirPTPrimarySurfacePrepassEnabled ? 1 : 0,
            directTemporalStandalone ? 1 : 0,
            directSpatialStandalone ? 1 : 0,
            finalConsumesPrimary ? 1 : 0,
            finalUsesStandaloneResolve ? 1 : 0,
            reflectionProducer ? 1 : 0,
            idMath::ClampInt(0, 10, r_pathTracingDLSSRRGuideDebugView.GetInteger()),
            restirPTDiDebugView,
            idMath::ClampInt(0, 4, r_pathTracingRestirPTGiDebugView.GetInteger()),
            nsightGpuMarkers ? 1 : 0,
            restirPTBufferSelection.initialOutput,
            restirPTBufferSelection.temporalInput,
            restirPTBufferSelection.temporalOutput,
            restirPTBufferSelection.spatialInput,
            restirPTBufferSelection.spatialOutput,
            restirPTBufferSelection.finalShadingInput,
            restirPTBufferSelection.debugInput,
            (restirPTPassPlan.flags & RT_RESTIR_PASS_TRACES_VISIBILITY) != 0 ? 1 : 0,
            restirPTVisibilityPolicy,
            restirPTReflectionMode,
            toyLightScale,
            toyEmissiveScale,
            effectiveAnalyticLightIntensityScale,
            r_pathTracingRestirPTPreviewMaxPixels.GetInteger(),
            restirPTContextDesc.temporalDepthThreshold,
            restirPTContextDesc.temporalNormalThreshold,
            restirPTContextDesc.temporalReservoirReuse ? 1 : 0,
            restirPTContextDesc.temporalFallbackSampling ? 1 : 0,
            idMath::ClampInt(0, 5, r_pathTracingRestirPTMaterialSimilarityMode.GetInteger()),
            idMath::ClampInt(0, 2, r_pathTracingRestirPTTemporalNeighborDebugMode.GetInteger()),
            r_pathTracingRestirPTUnifiedPrevToCurrentScan.GetBool() ? 1 : 0,
            restirPTContextDesc.spatialSamples,
            restirPTContextDesc.spatialRadius);
        r_pathTracingRestirPTPassDump.SetInteger(0);
    }

    PathTraceGpuTimingCapture gpuTimingCapture = BeginPathTraceGpuTimingCapture(
        debugMode,
        m_frameResources.width,
        m_frameResources.height,
        restirPTPassPlan.label);

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
    const bool toyFakePBRSpecularEnabled = r_pathTracingToyFakePBRSpecular.GetInteger() != 0 && (debugMode == 18 || effectiveRestirPTMode || integratorDebugMode);
    const bool pdfNeeEmissiveVerifierRoute = pdfNeeVerifierRouteRequested && pdfNeeVerifierEntryLightMode == 7;
    const uint32_t textureFlags =
        (r_pathTracingTextureBindlessEnable.GetInteger() != 0 ? 1u : 0u) |
        (r_pathTracingTextureFilter.GetInteger() != 0 ? 2u : 0u) |
        (r_pathTracingTextureDecode.GetInteger() != 0 ? 4u : 0u) |
        (r_pathTracingUseNormalMaps.GetInteger() != 0 && (debugMode == 14 || debugMode == 18 || debugMode == 20 || effectiveRestirPTMode || integratorDebugMode) ? 8u : 0u) |
        (r_pathTracingUseSpecularMaps.GetInteger() != 0 && (debugMode == 14 || debugMode == 57 || (integratorUsesSpecular && (debugMode == 18 || effectiveRestirPTMode || integratorDebugMode))) ? 16u : 0u) |
        (r_pathTracingUseEmissiveMaps.GetInteger() != 0 && (debugMode == 14 || debugMode == 18 || debugMode == 19 || debugMode == 20 || effectiveRestirPTMode || integratorDebugMode || cleanRtxdiDiRouteRequested) ? 32u : 0u) |
        (r_pathTracingReservoirTwoSidedEmissives.GetInteger() != 0 && (debugMode == 18 || debugMode == 20 || effectiveRestirPTMode || pdfNeeEmissiveVerifierRoute || cleanRtxdiDiRouteRequested) ? 64u : 0u) |
        (toyFakePBRSpecularEnabled ? 128u : 0u);
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
    const bool restirPTAnalyticLightCandidates = effectiveRestirPTMode && r_pathTracingRestirPTAnalyticLightCandidates.GetInteger() != 0;
    const bool enableDoomAnalyticLights = !disableAnalyticLightLoop && (r_pathTracingAnalyticLightCandidates.GetInteger() != 0 || restirPTAnalyticLightCandidates);
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
    const bool enableGpuBoundsOverlay = r_pathTracingSceneBoundsOverlayGpu.GetInteger() != 0;
    const bool enableBoundsBoxDebugMode = debugMode == 21 || debugMode == 22;
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
    const PathTraceRestirLightManagerStats restirLightManagerStats = m_restirLightManager.GetStats();
    const PathTraceRemixLightManagerStats remixLightManagerStats = m_remixLightManager.GetStats();
    const bool useRemixLightManagerRabSource = remixLightManagerStats.enabled != 0u;
    const bool useRemixLightManagerDenseRabSource =
        useRemixLightManagerRabSource &&
        remixLightManagerStats.enabled != 0u;
    const bool restirLightManagerPayloadsMatch = restirLightManagerStats.activePayloadCountMismatch == 0;
    const bool useLegacyRestirLightManagerRabSource =
        !useRemixLightManagerRabSource &&
        r_pathTracingRestirLightManagerRAB.GetInteger() != 0 &&
        restirLightManagerPayloadsMatch;
    const bool pdfNeeVerifierForcesSplitRabSource =
        pdfNeeVerifierRouteRequested &&
        pdfNeeVerifierEntryLightMode >= 4 &&
        pdfNeeVerifierEntryLightMode <= 7;
    constants.restirLightManagerInfo[0] = static_cast<float>(useRemixLightManagerRabSource ? remixLightManagerStats.currentLightCount : restirLightManagerStats.activeCurrentPayloadCount);
    constants.restirLightManagerInfo[1] = static_cast<float>(useRemixLightManagerRabSource ? remixLightManagerStats.previousLightCount : restirLightManagerStats.activePreviousPayloadCount);
    constants.restirLightManagerInfo[2] = static_cast<float>(useRemixLightManagerRabSource ? remixLightManagerStats.currentToPreviousCount : restirLightManagerStats.activeCurrentToPreviousCount);
    constants.restirLightManagerInfo[3] = static_cast<float>(useRemixLightManagerRabSource ? remixLightManagerStats.previousToCurrentCount : restirLightManagerStats.activePreviousToCurrentCount);
    constants.restirLightManagerControlInfo[0] = (!pdfNeeVerifierForcesSplitRabSource && (useRemixLightManagerRabSource || useLegacyRestirLightManagerRabSource)) ? 1.0f : 0.0f;
    constants.restirLightManagerControlInfo[1] = !pdfNeeVerifierForcesSplitRabSource && useRemixLightManagerDenseRabSource
        ? 2.0f
        : ((!pdfNeeVerifierForcesSplitRabSource && (useRemixLightManagerRabSource || useLegacyRestirLightManagerRabSource)) ? 1.0f : 0.0f);
    constants.restirLightManagerControlInfo[2] = static_cast<float>(useRemixLightManagerDenseRabSource
        ? remixLightManagerStats.doomAnalyticStableCacheableCount
        : 0u);
    constants.restirLightManagerControlInfo[3] = static_cast<float>(useRemixLightManagerDenseRabSource
        ? remixLightManagerStats.doomAnalyticUnstableDynamicCount
        : 0u);
    const bool rrxEmissiveSamplingEnabled = !disableEmissiveTriangleSampling && toyEmissiveScale > 0.0f;
    const bool rrxDoomAnalyticSamplingEnabled =
        !disableAnalyticLightLoop && enableDoomAnalyticLights && effectiveAnalyticLightIntensityScale > 0.0f;
    const uint32_t rawEmissiveRangeOffset = useRemixLightManagerRabSource ? remixLightManagerStats.emissiveRangeOffset : restirLightManagerStats.activeEmissiveCurrentRangeOffset;
    const uint32_t rawEmissiveRangeCount = useRemixLightManagerRabSource ? remixLightManagerStats.emissiveRangeCount : restirLightManagerStats.activeEmissiveCurrentRangeCount;
    const uint32_t rawDoomAnalyticRangeOffset = useRemixLightManagerRabSource ? remixLightManagerStats.doomAnalyticRangeOffset : restirLightManagerStats.activeDoomAnalyticCurrentRangeOffset;
    const uint32_t rawDoomAnalyticRangeCount = useRemixLightManagerRabSource ? remixLightManagerStats.doomAnalyticRangeCount : restirLightManagerStats.activeDoomAnalyticCurrentRangeCount;
    const bool regirNeedsEmissiveRange =
        regirDebugRouteRequested &&
        useRemixLightManagerDenseRabSource &&
        (regirSettings.lightDomain == 1 || regirSettings.lightDomain == 2);
    const bool regirNeedsDoomAnalyticRange =
        regirDebugRouteRequested &&
        useRemixLightManagerDenseRabSource &&
        (regirSettings.lightDomain == 0 || regirSettings.lightDomain == 2);
    const bool pdfNeeNeedsCurrentRluTypedRanges =
        pdfNeeRluCurrentProducerRequested &&
        useRemixLightManagerDenseRabSource;
    const bool neeCacheNeedsCurrentRluEmissiveRange =
        neeCacheCandidateBuildRequested &&
        useRemixLightManagerDenseRabSource;
    const bool neeCacheNeedsCurrentRluDoomAnalyticRange =
        neeCacheCandidateBuildRequested &&
        useRemixLightManagerDenseRabSource;
    const uint32_t activeEmissiveRangeCount =
        (rrxEmissiveSamplingEnabled || regirNeedsEmissiveRange || pdfNeeNeedsCurrentRluTypedRanges || neeCacheNeedsCurrentRluEmissiveRange) ? rawEmissiveRangeCount : 0u;
    const uint32_t activeDoomAnalyticRangeCount =
        (rrxDoomAnalyticSamplingEnabled || regirNeedsDoomAnalyticRange || pdfNeeNeedsCurrentRluTypedRanges || neeCacheNeedsCurrentRluDoomAnalyticRange) ? rawDoomAnalyticRangeCount : 0u;
    const uint32_t shaderEmissiveRangeCount = useRemixLightManagerRabSource ? rawEmissiveRangeCount : activeEmissiveRangeCount;
    const uint32_t shaderDoomAnalyticRangeCount = useRemixLightManagerRabSource ? rawDoomAnalyticRangeCount : activeDoomAnalyticRangeCount;
    constants.restirLightManagerRangeInfo[0] = static_cast<float>(rawEmissiveRangeOffset);
    constants.restirLightManagerRangeInfo[1] = static_cast<float>(shaderEmissiveRangeCount);
    constants.restirLightManagerRangeInfo[2] = static_cast<float>(rawDoomAnalyticRangeOffset);
    constants.restirLightManagerRangeInfo[3] = static_cast<float>(shaderDoomAnalyticRangeCount);
    const uint32_t activeTotalRangeCount = activeEmissiveRangeCount + activeDoomAnalyticRangeCount;
    const uint32_t legacyRequestedTotal = RrxDiRequestedInitialSampleBudget(
        activeEmissiveRangeCount,
        activeDoomAnalyticRangeCount);
    const bool emissiveSampleBudgetRequested =
        rrxEmissiveSamplingEnabled || regirNeedsEmissiveRange || neeCacheNeedsCurrentRluEmissiveRange;
    const bool doomAnalyticSampleBudgetRequested =
        rrxDoomAnalyticSamplingEnabled || regirNeedsDoomAnalyticRange || neeCacheNeedsCurrentRluDoomAnalyticRange;
    const uint32_t boundedEmissiveSampleCount = !emissiveSampleBudgetRequested
        ? 0u
        : (useRemixLightManagerRabSource
            ? remixLightManagerStats.emissiveSampleCount
            : RrxDiBoundedRangeSampleCount(activeEmissiveRangeCount, activeTotalRangeCount, legacyRequestedTotal));
    const uint32_t boundedDoomAnalyticSampleCount = !doomAnalyticSampleBudgetRequested
        ? 0u
        : (useRemixLightManagerRabSource
            ? remixLightManagerStats.doomAnalyticSampleCount
            : RrxDiBoundedRangeSampleCount(activeDoomAnalyticRangeCount, activeTotalRangeCount, legacyRequestedTotal));
    const uint32_t shaderEmissiveSampleCount = useRemixLightManagerRabSource ? remixLightManagerStats.emissiveSampleCount : boundedEmissiveSampleCount;
    const uint32_t shaderDoomAnalyticSampleCount = useRemixLightManagerRabSource ? remixLightManagerStats.doomAnalyticSampleCount : boundedDoomAnalyticSampleCount;
    const uint32_t shaderTotalSampleCount = useRemixLightManagerRabSource
        ? remixLightManagerStats.totalSampleCount
        : (boundedEmissiveSampleCount + boundedDoomAnalyticSampleCount);
    const uint32_t shaderNonEmptyRangeCount = useRemixLightManagerRabSource
        ? remixLightManagerStats.nonEmptyRangeCount
        : ((activeEmissiveRangeCount > 0 ? 1u : 0u) + (activeDoomAnalyticRangeCount > 0 ? 1u : 0u));
    constants.restirLightManagerSampleInfo[0] = static_cast<float>(shaderEmissiveSampleCount);
    constants.restirLightManagerSampleInfo[1] = static_cast<float>(shaderDoomAnalyticSampleCount);
    constants.restirLightManagerSampleInfo[2] = static_cast<float>(shaderTotalSampleCount);
    constants.restirLightManagerSampleInfo[3] = static_cast<float>(shaderNonEmptyRangeCount);
    constants.restirPTInfo[0] = static_cast<float>(restirPTFrameIndex);
    constants.restirPTInfo[1] = r_pathTracingNormalMapFlipGreen.GetInteger() != 0 ? 1.0f : 0.0f;
    constants.restirPTInfo[2] = (restirPTPassPlan.flags & RT_RESTIR_PASS_TRACES_VISIBILITY) != 0 ? 1.0f : 0.0f;
    constants.restirPTInfo[3] = idMath::ClampFloat(0.0f, 16.0f, r_pathTracingRestirPTPreviewExposure.GetFloat());
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
    constants.restirPTSurfaceInfo[1] = static_cast<float>(idMath::ClampInt(0, 2, r_pathTracingRestirPTTemporalNeighborDebugMode.GetInteger()));
    constants.restirPTSurfaceInfo[2] = r_pathTracingRestirPTUnifiedPrevToCurrentScan.GetBool() ? 1.0f : 0.0f;
    constants.restirPTSurfaceInfo[3] = r_pathTracingMotionVectorDisableRigid.GetBool() ? 1.0f : 0.0f;
    constants.restirPTDirectInfo[0] = static_cast<float>(restirPTDirectWidth);
    constants.restirPTDirectInfo[1] = static_cast<float>(restirPTDirectHeight);
    constants.restirPTDirectInfo[2] = 0.0f;
    constants.restirPTDirectInfo[3] = restirPTDirectResolutionScale;
    constants.restirPTSparsityInfo[0] = static_cast<float>(restirPTRaySparsity);
    constants.restirPTSparsityInfo[1] = static_cast<float>(restirPTRaySparsityPhase);
    constants.restirPTSparsityInfo[2] = stagedRestirDirectLightingMode ? 1.0f : 0.0f;
    constants.restirPTSparsityInfo[3] = static_cast<float>(restirPTRaySparsityPreviousPhase);
    constants.restirPTIndirectInfo[0] = 0.0f;
    constants.restirPTIndirectInfo[1] = stagedRestirGiInitialMode ? 1.0f : 0.0f;
    constants.restirPTIndirectInfo[2] = static_cast<float>(restirPTGiRaySparsity);
    constants.restirPTIndirectInfo[3] = static_cast<float>(restirPTGiRaySparsityPhase);
    constants.rayReconstructionInfo[0] = static_cast<float>(idMath::ClampInt(0, 10, r_pathTracingDLSSRRGuideDebugView.GetInteger()));
    constants.rayReconstructionInfo[1] = 0.0f;
    constants.rayReconstructionInfo[2] = 0.0f;
    constants.rayReconstructionInfo[3] = 0.0f;
    constants.unifiedLightInfo[0] = static_cast<float>(Max(0, m_smokeUnifiedLightCount));
    constants.unifiedLightInfo[1] = static_cast<float>(Max(0, m_smokeUnifiedPreviousLightCount));
    constants.unifiedLightInfo[2] =
        (!pdfNeeVerifierForcesSplitRabSource && (useRemixLightManagerRabSource || r_pathTracingRestirPTUnifiedLightLoad.GetInteger() != 0) ? 1.0f : 0.0f) +
        (!pdfNeeVerifierForcesSplitRabSource && (useRemixLightManagerRabSource || r_pathTracingRestirPTUnifiedLightSample.GetInteger() != 0) ? 2.0f : 0.0f) +
        (!pdfNeeVerifierForcesSplitRabSource && r_pathTracingRestirPTUnifiedNee.GetInteger() != 0 ? 4.0f : 0.0f);
    constants.unifiedLightInfo[3] = static_cast<float>(Max(0, m_smokeUnifiedLightRemapCount));
    const int pdfNeeVerifierView = idMath::ClampInt(0, 8, r_pathTracingRestirPdfNeeVerifierView.GetInteger());
    const int pdfNeeVerifierLightMode = idMath::ClampInt(0, 9, r_pathTracingRestirPdfNeeVerifierLightMode.GetInteger());
    const int pdfNeeVerifierDomain = idMath::ClampInt(0, 2, r_pathTracingRestirPdfNeeVerifierDomain.GetInteger());
    const int pdfNeeVerifierSamples = idMath::ClampInt(1, 64, r_pathTracingRestirPdfNeeVerifierSamples.GetInteger());
    const int pdfNeeVerifierVisibility = pdfNeeVerifierEntryVisibility;
    const int pdfNeeVerifierSourcePolicy = idMath::ClampInt(0, 2, r_pathTracingRestirPdfNeeVerifierSourcePolicy.GetInteger());
    const bool pdfNeeVerifierForbiddenMode = debugMode == 56;
    const bool pdfNeeRluCurrentProducerEnabled =
        r_pathTracingRestirPdfNeeVerifierEnable.GetInteger() != 0 &&
        !pdfNeeVerifierForbiddenMode;
    const bool pdfNeeVerifierRouteEnabled =
        pdfNeeRluCurrentProducerEnabled ||
        (r_pathTracingRestirPdfNeeVerifierEnable.GetInteger() != 0 &&
            pdfNeeVerifierView > 0 &&
            pdfNeeVerifierLightMode != 8 &&
            pdfNeeVerifierLightMode != 9 &&
            !pdfNeeVerifierForbiddenMode);
    constants.restirPdfNeeVerifierInfo[0] = pdfNeeVerifierRouteEnabled ? 1.0f : 0.0f;
    constants.restirPdfNeeVerifierInfo[1] = static_cast<float>(pdfNeeVerifierView);
    constants.restirPdfNeeVerifierInfo[2] = static_cast<float>(pdfNeeVerifierLightMode);
    constants.restirPdfNeeVerifierInfo[3] = static_cast<float>(pdfNeeVerifierDomain);
    constants.restirPdfNeeVerifierControlInfo[0] = static_cast<float>(pdfNeeVerifierSamples);
    constants.restirPdfNeeVerifierControlInfo[1] = static_cast<float>(pdfNeeVerifierVisibility);
    constants.restirPdfNeeVerifierControlInfo[2] = static_cast<float>(pdfNeeVerifierSourcePolicy);
    const bool pdfNeeNeeCacheProviderRequested = pdfNeeVerifierSourcePolicy == 2;
    const bool pdfNeeNeeCacheProviderReady =
        pdfNeeNeeCacheProviderRequested &&
        neeCacheResourceReady &&
        neeCacheCandidateBuildRequested &&
        m_smokeNeeCacheState.providerResultBuffer != nullptr &&
        m_smokeNeeCacheState.cellBuffer != nullptr &&
        m_smokeNeeCacheState.candidateBuffer != nullptr;
    constants.restirPdfNeeVerifierControlInfo[3] = pdfNeeNeeCacheProviderReady ? 1.0f : 0.0f;
    if (r_pathTracingRestirPdfNeeVerifierDump.GetInteger() != 0 && pdfNeeRluCurrentProducerEnabled)
    {
        const int managerCount = static_cast<int>(constants.restirLightManagerInfo[0]);
        const int emissiveCount = static_cast<int>(constants.restirLightManagerRangeInfo[1]);
        const int doomAnalyticCount = static_cast<int>(constants.restirLightManagerRangeInfo[3]);
        const bool typedPolicy = pdfNeeVerifierSourcePolicy == 1 && (emissiveCount > 0 || doomAnalyticCount > 0);
        const float emissiveClassProbability = typedPolicy && emissiveCount > 0
            ? (doomAnalyticCount > 0 ? 0.5f : 1.0f)
            : 0.0f;
        const float doomAnalyticClassProbability = typedPolicy && doomAnalyticCount > 0
            ? (emissiveCount > 0 ? 0.5f : 1.0f)
            : 0.0f;
        const char* firstMissingContract =
            pdfNeeVerifierForbiddenMode ? "forbidden-mode-56" :
            (pdfNeeNeeCacheProviderRequested && !pdfNeeNeeCacheProviderReady ? "nee-cache-provider-not-ready-neecache-07" :
            (managerCount <= 0 ? "current-rlu-dense-domain" : "none"));
        const char* sourcePolicyName = pdfNeeNeeCacheProviderRequested
            ? "nee-cache-provider"
            : (typedPolicy ? "typed-stratified-rlu" : "full-domain-uniform-rlu");
        common->Printf(
            "PathTracePrimaryPass: ReSTIR PDF+NEE RLU current producer route enable=%d requestedEnable=%d samples=%d visibility=%d sourcePolicy=%d(%s) debugMode=%d rluDomain=%u managerCount=%d previousCount=%d rluRanges emissive=%d+%d doomAnalytic=%d+%d rluSampleInfo emissive/doom/total/nonEmpty=%d/%d/%d/%d neeCacheProvider requested/ready=%d/%d providerResultSrv=t74 cellSrv=t75 candidateSrv=t77 classProbability emissive=%.3f doomAnalytic=%.3f sourcePdf=%s sourcePdfFormula=%s invSourcePdfFormula=%s producerHelperSequence=RTXDI_DIInitialSamplingParameters,RTXDI_RandomSamplerState,nee-cache-ris-candidate-or-fallback,RTXDI_StreamSample,RTXDI_FinalizeResampling reservoirM=1 normalizationDenominator=requestedLocalSamples selectedLightIdentity=dense-current-rlu-lightIndex solidAnglePdf=RAB_SampleActiveRrxPolymorphicLight targetPdf=RAB_GetLightSampleTargetPdfForSurface finalContribution=RAB_GetReflectedBsdfRadianceForSurface*reservoirInvPdf/solidAnglePdf*visibility cleanCurrentReservoir=%d cleanTemporalReservoir=%d cleanPreviousReservoir=%d cleanReservoirPage=u69 firstMissingContract=%s temporal=0 spatial=0 bestLights=0 denoiser=0 mode56=%d oldPdfNee=discarded task=%s\n",
            idStr::Icmp(firstMissingContract, "none") == 0 ? 1 : 0,
            r_pathTracingRestirPdfNeeVerifierEnable.GetInteger() != 0 ? 1 : 0,
            pdfNeeVerifierSamples,
            pdfNeeVerifierVisibility,
            pdfNeeVerifierSourcePolicy,
            sourcePolicyName,
            debugMode,
            remixLightManagerStats.domain,
            managerCount,
            static_cast<int>(constants.restirLightManagerInfo[1]),
            static_cast<int>(constants.restirLightManagerRangeInfo[0]),
            emissiveCount,
            static_cast<int>(constants.restirLightManagerRangeInfo[2]),
            doomAnalyticCount,
            static_cast<int>(constants.restirLightManagerSampleInfo[0]),
            static_cast<int>(constants.restirLightManagerSampleInfo[1]),
            static_cast<int>(constants.restirLightManagerSampleInfo[2]),
            static_cast<int>(constants.restirLightManagerSampleInfo[3]),
            pdfNeeNeeCacheProviderRequested ? 1 : 0,
            pdfNeeNeeCacheProviderReady ? 1 : 0,
            emissiveClassProbability,
            doomAnalyticClassProbability,
            pdfNeeNeeCacheProviderRequested ? "nee-cache-ris-candidate-or-fallback" : (typedPolicy ? "rtxdi-stratified-typed-current" : "rtxdi-stratified-dense-current"),
            pdfNeeNeeCacheProviderRequested ? "cache:weightedCandidateIdentityPdf*cacheProbability,fallback:domainPdf*fallbackProbability" : (typedPolicy ? "rangeSampleCount/(rangeCount*totalProposalSamples)" : "1/currentRluLightCount"),
            pdfNeeNeeCacheProviderRequested ? "1/sourcePdf" : (typedPolicy ? "(rangeCount*totalProposalSamples)/rangeSampleCount" : "currentRluLightCount"),
            m_smokeCleanRtxdiDiCurrentReservoirBuffer ? 1 : 0,
            m_smokeCleanRtxdiDiTemporalReservoirBuffer ? 1 : 0,
            m_smokeCleanRtxdiDiPreviousReservoirBuffer ? 1 : 0,
            firstMissingContract,
            pdfNeeVerifierForbiddenMode ? 1 : 0,
            "PDFNEE-RLU-04");
        r_pathTracingRestirPdfNeeVerifierDump.SetInteger(0);
    }
    if (r_pathTracingRestirPdfNeeVerifierDump.GetInteger() != 0)
    {
        const int splitCount = Max(0, m_smokeEmissiveTriangleCount) + Max(0, m_smokeDoomAnalyticLightCount);
        const int unifiedCount = Max(0, m_smokeUnifiedLightCount);
        const int managerCount = static_cast<int>(constants.restirLightManagerInfo[0]);
        const int realAnalyticOneCount = Max(0, m_smokeDoomAnalyticLightCount) > 0 ? 1 : 0;
        const int realAnalyticTwoCount = Max(0, m_smokeDoomAnalyticLightCount) >= 2 ? 2 : 0;
        const int realAnalyticFullCount = Max(0, m_smokeDoomAnalyticLightCount);
        const int emissiveDomainCount = Max(0, m_smokeEmissiveTriangleCount);
        const int activeDomainCount =
            (pdfNeeVerifierLightMode >= 1 && pdfNeeVerifierLightMode <= 3) ? (pdfNeeVerifierLightMode == 3 ? 4 : pdfNeeVerifierLightMode) :
            (pdfNeeVerifierLightMode == 4 ? realAnalyticOneCount :
            (pdfNeeVerifierLightMode == 5 ? realAnalyticTwoCount :
            (pdfNeeVerifierLightMode == 6 ? realAnalyticFullCount :
            (pdfNeeVerifierLightMode == 7 ? emissiveDomainCount :
            (pdfNeeVerifierLightMode == 8 ? managerCount :
            (pdfNeeVerifierLightMode == 9 ? 0 :
            pdfNeeVerifierDomain == 0 ? splitCount :
            (pdfNeeVerifierDomain == 1 ? unifiedCount : managerCount)))))));
        const char* firstMissingContract =
            pdfNeeVerifierForbiddenMode ? "forbidden-mode-56" :
            (pdfNeeVerifierLightMode == 9 ? "regir-consume-disabled-standalone-lane" :
            (pdfNeeVerifierLightMode == 8 ? "quarantined-failed-rlu-direct-diagnostic" :
            (!pdfNeeVerifierRouteEnabled ? "route-disabled" :
            (pdfNeeVerifierLightMode == 0 || activeDomainCount <= 0 ? "no-active-proposal-domain" :
            (pdfNeeVerifierLightMode >= 1 && pdfNeeVerifierLightMode <= 7 ? "none" : "estimator-not-implemented-pdfnee-01")))));
        const char* taskLabel = (pdfNeeVerifierLightMode == 2 || pdfNeeVerifierLightMode == 3) ? "PDFNEE-03" :
            (pdfNeeVerifierLightMode == 9 ? "PDFNEE-11" :
            (pdfNeeVerifierLightMode == 8 ? "PDFNEE-QUARANTINED" :
            (pdfNeeVerifierLightMode == 7 ? "PDFNEE-07" :
            (pdfNeeVerifierLightMode == 6 ? "PDFNEE-06" :
            (pdfNeeVerifierLightMode == 5 ? "PDFNEE-05" :
            (pdfNeeVerifierLightMode == 4 ? "PDFNEE-04" :
            (pdfNeeVerifierLightMode == 1 ? "PDFNEE-02" : "PDFNEE-01")))))));
        common->Printf(
            "PathTracePrimaryPass: PDFNEE verifier route enable=%d requestedEnable=%d view=%d lightMode=%d domain=%d samples=%d visibility=%d debugMode=%d splitCount=%d unifiedCount=%d managerCount=%d doomAnalyticCount=%d activeVerifierCount=%d firstMissingContract=%s cleanCurrentReservoir=%d cleanTemporalReservoir=%d cleanPreviousReservoir=%d cleanReservoirPage=u69 producerHelperSequence=RTXDI_EmptyDIReservoir,RTXDI_StreamSample,RTXDI_FinalizeResampling,RTXDI_PackDIReservoir syntheticOneLightTable={sourcePdf=1.000000 solidAnglePdf=1.000000 targetPdfCenter=0.318310 reservoirInvPdf=1.000000 reflectedRadianceCenter=(0.318310,0.318310,0.318310) finalContributionCenter=(0.318310,0.318310,0.318310)} syntheticOverlapSourcePdfTable={twoLights=(0.500000,0.500000) nLightsCount=4 nLightsEach=0.250000 sourcePdfSum=1.000000} realAnalyticOneLightTable={proposalDomain=first-contributing-doom-analytic-for-surface sourcePdf=1.000000 requiredCalls=RAB_LoadLightInfo,RAB_SamplePolymorphicLight,RAB_GetLightSampleTargetPdfForSurface,RAB_GetReflectedBsdfRadianceForSurface} realAnalyticTwoLightTable={proposalDomain=first-two-contributing-doom-analytics-for-surface sourcePdf=(0.500000,0.500000) sourcePdfSum=1.000000 reservoirInvPdf=2.000000 finalContribution=per-selected-light-reflectedRadiance*2/solidAnglePdf*visibility view8=16-sample-rab-average-sum-of-both-lights requiredCalls=RAB_LoadLightInfo,RAB_SamplePolymorphicLight,RAB_GetLightSampleTargetPdfForSurface,RAB_GetReflectedBsdfRadianceForSurface} realAnalyticFullDomainTable={proposalDomain=all-current-doom-analytics sourcePdfFormula=1/currentDoomAnalyticCount sourcePdfSum=1.000000 invalidPolicy=included-as-zero-contribution reservoirInvPdf=currentDoomAnalyticCount view8=sum-of-all-valid-selected-sample-contributions requiredCalls=RAB_LoadLightInfo,RAB_SamplePolymorphicLight,RAB_GetLightSampleTargetPdfForSurface,RAB_GetReflectedBsdfRadianceForSurface} emissiveDomainTable={proposalDomain=current-emissive-triangles sourcePdf=sampleWeightAndPdf.y fallback=max(uploadedPdf,1/currentEmissiveCount) selection=SelectSmokeWeightedEmissiveTriangle view8=sum-of-valid-emissive-contributions requiredCalls=RAB_LoadLightInfo,RAB_SamplePolymorphicLight,RAB_GetLightSampleTargetPdfForSurface,RAB_GetReflectedBsdfRadianceForSurface} regirDomainTable={proposalDomain=ReGIR-candidate-cache sourcePdf=1/invSourcePdf invSourcePdf=risWeightSum/selectedCellWeight selection=boundedCellSlotScan16to32 view8=full-cell-slot-mean prepass=build-only-u72 consumer=t73 requiredCalls=RAB_LoadLightInfo,RAB_SamplePolymorphicLight,RAB_GetLightSampleTargetPdfForSurface,RAB_GetReflectedBsdfRadianceForSurface} output=owned-current-frame temporal=0 spatial=0 mode56=0 task=%s\n",
            pdfNeeVerifierRouteEnabled ? 1 : 0,
            r_pathTracingRestirPdfNeeVerifierEnable.GetInteger() != 0 ? 1 : 0,
            pdfNeeVerifierView,
            pdfNeeVerifierLightMode,
            pdfNeeVerifierDomain,
            pdfNeeVerifierSamples,
            pdfNeeVerifierVisibility,
            debugMode,
            splitCount,
            unifiedCount,
            managerCount,
            Max(0, m_smokeDoomAnalyticLightCount),
            activeDomainCount,
            firstMissingContract,
            m_smokeCleanRtxdiDiCurrentReservoirBuffer ? 1 : 0,
            m_smokeCleanRtxdiDiTemporalReservoirBuffer ? 1 : 0,
            m_smokeCleanRtxdiDiPreviousReservoirBuffer ? 1 : 0,
            taskLabel);
        r_pathTracingRestirPdfNeeVerifierDump.SetInteger(0);
    }
    if (r_pathTracingNeeCacheDump.GetInteger() != 0)
    {
        printNeeCacheDump("pre-dispatch", "none");
        r_pathTracingNeeCacheDump.SetInteger(0);
    }
    if (r_pathTracingNeeCacheSecondaryDump.GetInteger() != 0)
    {
        printNeeCacheSecondaryDump();
        r_pathTracingNeeCacheSecondaryDump.SetInteger(0);
    }
    if (r_pathTracingReGIRDump.GetInteger() != 0)
    {
        printReGIRDump("pre-dispatch", "none");
        r_pathTracingReGIRDump.SetInteger(0);
    }
    constants.restirPTDiDebugInfo[0] = static_cast<float>(restirPTDiDebugView);
    constants.restirPTDiDebugInfo[1] = 0.0f;
    uint32_t rrxDebugBypassFlags = 0u;
    rrxDebugBypassFlags |= r_pathTracingRestirPTRrxDebugBypassMotion.GetBool() ? (1u << 0) : 0u;
    rrxDebugBypassFlags |= r_pathTracingRestirPTRrxDebugBypassDepth.GetBool() ? (1u << 1) : 0u;
    rrxDebugBypassFlags |= r_pathTracingRestirPTRrxDebugBypassNormal.GetBool() ? (1u << 2) : 0u;
    rrxDebugBypassFlags |= r_pathTracingRestirPTRrxDebugBypassSurfaceSimilarity.GetBool() ? (1u << 3) : 0u;
    rrxDebugBypassFlags |= r_pathTracingRestirPTRrxDebugBypassResetMask.GetBool() ? (1u << 4) : 0u;
    rrxDebugBypassFlags |= r_pathTracingRestirPTRrxDebugBypassPortal.GetBool() ? (1u << 5) : 0u;
    rrxDebugBypassFlags |= r_pathTracingRestirPTRrxTemporalPermutation.GetBool() ? (1u << 6) : 0u;
    rrxDebugBypassFlags |= r_pathTracingRestirPTRrxDisablePreviousBest.GetBool() ? (1u << 7) : 0u;
    rrxDebugBypassFlags |= r_pathTracingRestirPTRrxSyntheticPrimaryPatch.GetBool() ? (1u << 8) : 0u;
    constants.restirPTDiDebugInfo[2] = static_cast<float>(rrxDebugBypassFlags);
    constants.restirPTDiDebugInfo[3] = r_pathTracingRestirPTRrxDebugFlatContribution.GetBool() ? 1.0f : 0.0f;
    auto updateRemixDiReservoirProbeConstants = [&]()
    {
        const PathTraceRemixRtxdiReservoirDomain& remixDiDomain = m_remixRtxdiResources.GetDomain(PATH_TRACE_REMIX_RTXDI_RESERVOIR_DOMAIN_DI);
        const bool remixDiValid = remixDiDomain.IsValidFor(
            static_cast<uint32_t>(Max(0, m_frameResources.width)),
            static_cast<uint32_t>(Max(0, m_frameResources.height)),
            rtxdi::CheckerboardMode::Off,
            rtxdi::c_NumReSTIRDIReservoirBuffers,
            static_cast<uint32_t>(sizeof(RTXDI_PackedDIReservoir)));
        uint32_t remixDiFlags = remixDiValid ? 1u : 0u;
        if (remixDiDomain.clearPending || disableReservoirWrites)
        {
            remixDiFlags |= 2u;
        }
        if (!remixDiValid)
        {
            remixDiFlags |= 4u;
        }
        const uint32_t currentDiagnosticPage = static_cast<uint32_t>(restirPTFrameIndex & 1u);
        const uint32_t previousDiagnosticPage = currentDiagnosticPage ^ 1u;
        const uint32_t spatialOutputPage = remixDiDomain.reservoirArrayCount > 2u ? 2u : currentDiagnosticPage;
        constants.restirPTRemixDiReservoirInfo[0] = remixDiDomain.reservoirParams.reservoirBlockRowPitch;
        constants.restirPTRemixDiReservoirInfo[1] = remixDiDomain.reservoirParams.reservoirArrayPitch;
        constants.restirPTRemixDiReservoirInfo[2] = remixDiDomain.reservoirArrayCount;
        constants.restirPTRemixDiReservoirInfo[3] = remixDiFlags;
        constants.restirPTRemixDiReservoirPageInfo[0] = currentDiagnosticPage;
        constants.restirPTRemixDiReservoirPageInfo[1] = previousDiagnosticPage;
        constants.restirPTRemixDiReservoirPageInfo[2] = spatialOutputPage;
        constants.restirPTRemixDiReservoirPageInfo[3] = spatialOutputPage;
    };
    updateRemixDiReservoirProbeConstants();
    constants.restirPTGiDebugInfo[0] = restirPTCombinedMode ? static_cast<float>(idMath::ClampInt(0, 4, r_pathTracingRestirPTGiDebugView.GetInteger())) : 0.0f;
    constants.restirPTGiDebugInfo[1] = r_pathTracingRestirPTRrxFinalConsumerCurrentOnly.GetBool() ? 1.0f : 0.0f;
    constants.restirPTGiDebugInfo[2] = r_pathTracingRestirPTRrxFinalConsumerOutput.GetBool() ? 1.0f : 0.0f;
    constants.restirPTGiDebugInfo[3] = mode56AccumulationActive ? 1.0f : 0.0f;
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
    constants.safetyInfo[2] =
        static_cast<float>(idMath::ClampInt(0, 2, r_pathTracingRestirPTSpatialDiagnosticView.GetInteger())) +
        static_cast<float>(restirPTVisibilityPolicy * 16);
    constants.safetyInfo[3] =
        static_cast<float>(idMath::ClampInt(0, 4, r_pathTracingRestirPTMode18DebugView.GetInteger())) +
        (r_pathTracingRestirPTMode18HeavyDirect.GetInteger() != 0 ? 16.0f : 0.0f);
    constants.geometryInfo0[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.staticVertexCount));
    constants.geometryInfo0[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.staticIndexCount));
    constants.geometryInfo0[2] = static_cast<float>(Max(0, m_sceneInputs.geometry.staticTriangleCount));
    constants.geometryInfo0[3] = static_cast<float>(Max(0, m_sceneInputs.geometry.dynamicVertexCount));
    constants.geometryInfo1[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.dynamicIndexCount));
    constants.geometryInfo1[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.dynamicTriangleCount));
    constants.geometryInfo1[2] = static_cast<float>(Max(0, m_sceneInputs.geometry.rigidRouteVertexCount));
    constants.geometryInfo1[3] = static_cast<float>(Max(0, m_sceneInputs.geometry.rigidRouteIndexCount));
    constants.geometryInfo2[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.rigidRouteTriangleCount));
    constants.geometryInfo2[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.rigidRouteInstanceCount));
    constants.geometryInfo2[2] = static_cast<float>(m_frameResources.primarySurfaceHistoryBuffers.surfaceCount);
    constants.geometryInfo2[3] = static_cast<float>(m_frameResources.smokeReservoirBuffers.reservoirCount);
    constants.geometryInfo3[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.skinnedPreviousPositionCount));
    constants.geometryInfo3[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.skinnedSurfaceDispatchCount));
    constants.geometryInfo3[2] = static_cast<float>(Max(0, m_sceneInputs.geometry.skinnedTriangleDispatchIndexCount));
    constants.geometryInfo3[3] = static_cast<float>(restirPTReflectionMode);
    constants.geometryInfo4[0] = static_cast<float>(Max(0, m_sceneInputs.geometry.previousStaticVertexCount));
    constants.geometryInfo4[1] = static_cast<float>(Max(0, m_sceneInputs.geometry.previousStaticIndexCount));
    constants.geometryInfo4[2] = static_cast<float>(Max(0, m_sceneInputs.geometry.previousStaticTriangleCount));
    constants.geometryInfo4[3] = static_cast<float>(Max(0, m_sceneInputs.geometry.previousStaticMaterialIndexCount));
    constants.dispatchTileInfo[0] = 0.0f;
    constants.dispatchTileInfo[1] = 0.0f;
    constants.dispatchTileInfo[2] = static_cast<float>(Max(0, m_frameResources.width));
    constants.dispatchTileInfo[3] = static_cast<float>(Max(0, m_frameResources.height));
    if (r_pathTracingIntegratorDump.GetInteger() != 0)
    {
        const uint64 estimatedDispatchRays =
            static_cast<uint64>(Max(0, m_frameResources.width)) *
            static_cast<uint64>(Max(0, m_frameResources.height)) *
            static_cast<uint64>(Max(1, estimatedRaysPerPixel));
        common->Printf("PathTracePrimaryPass: PT integrator settings output=%dx%d spp=%d maxDepth=%d diffuse/spec/trans=%d/%d/%d reflectionMode=%d rrDepth=%d nee=%d secondaryNeeMode=%d secondaryNeeVisibility=%d secondaryAnalyticNeeMode=%d secondaryAnalyticNeeSamples=%d selectedLights=%d analyticLights=%d estimatedRaysPerPixel=%d estimatedDispatchRays=%llu\n",
            m_frameResources.width,
            m_frameResources.height,
            integratorSettings.samplesPerPixel,
            integratorSettings.maxPathDepth,
            integratorSettings.diffuseBounceLimit,
            integratorSettings.specularBounceLimit,
            integratorSettings.transmissionBounceLimit,
            integratorSettings.reflectionMode,
            integratorSettings.russianRouletteDepth,
            integratorSettings.nextEventEstimation,
            integratorSettings.secondaryNeeMode,
            integratorSettings.secondaryNeeVisibility,
            integratorSettings.secondaryAnalyticNeeMode,
            integratorSettings.secondaryAnalyticNeeSamples,
            selectedLightRequestCount,
            analyticLightTraceCount,
            estimatedRaysPerPixel,
            static_cast<unsigned long long>(estimatedDispatchRays));
        r_pathTracingIntegratorDump.SetInteger(0);
    }
    if (r_pathTracingSafetyDump.GetInteger() != 0)
    {
        const int activeDescriptorCount = Max(0, static_cast<int>(m_smokeActiveTextureTable.size()) - 1);
        common->Printf(
            "PathTracePrimaryPass: PT safety pre-dispatch output=%dx%d debugMode=%d spp=%d maxDepth=%d bounce diffuse/spec/trans=%d/%d/%d reflectionMode=%d rrDepth=%d nee=%d secondaryNeeMode=%d secondaryNeeVisibility=%d secondaryAnalyticNeeMode=%d secondaryAnalyticNeeSamples=%d killMask=0x%08x selectedLights actual/effective=%d/%d analytic actual/effective=%d/%d emissive actual/effective=%d/%d lightCandidates actual/effective=%d/%d materialEntries=%d activeTextureDescriptors=%d activeTextureTableSize=%d geometry static(v/i/t)=%d/%d/%d dynamic(v/i/t)=%d/%d/%d rigid(v/i/t/inst)=%d/%d/%d/%d AS tlas/static/dynamic=%d/%d/%d primaryHistory count/bytes=%u/%llu smokeReservoir count/bytes=%d/%llu restirReservoir elements/bytes=%u/%llu bindingSet=%d textureTable=%d\n",
            m_frameResources.width,
            m_frameResources.height,
            debugMode,
            integratorSettings.samplesPerPixel,
            integratorSettings.maxPathDepth,
            integratorSettings.diffuseBounceLimit,
            integratorSettings.specularBounceLimit,
            integratorSettings.transmissionBounceLimit,
            integratorSettings.reflectionMode,
            integratorSettings.russianRouletteDepth,
            integratorSettings.nextEventEstimation,
            integratorSettings.secondaryNeeMode,
            integratorSettings.secondaryNeeVisibility,
            integratorSettings.secondaryAnalyticNeeMode,
            integratorSettings.secondaryAnalyticNeeSamples,
            safetyDisableMask,
            disableSelectedLightLoop ? 0 : selectedLightRequestCount,
            selectedLightCount,
            m_smokeDoomAnalyticLightCount,
            analyticLightTraceCount,
            m_smokeEmissiveTriangleCount,
            disableEmissiveTriangleSampling ? 0 : m_smokeEmissiveTriangleCount,
            m_smokeLightCandidateCount,
            disableEmissiveTriangleSampling ? 0 : m_smokeLightCandidateCount,
            m_smokeMaterialTableEntryCount,
            activeDescriptorCount,
            static_cast<int>(m_smokeActiveTextureTable.size()),
            m_sceneInputs.geometry.staticVertexCount,
            m_sceneInputs.geometry.staticIndexCount,
            m_sceneInputs.geometry.staticTriangleCount,
            m_sceneInputs.geometry.dynamicVertexCount,
            m_sceneInputs.geometry.dynamicIndexCount,
            m_sceneInputs.geometry.dynamicTriangleCount,
            m_sceneInputs.geometry.rigidRouteVertexCount,
            m_sceneInputs.geometry.rigidRouteIndexCount,
            m_sceneInputs.geometry.rigidRouteTriangleCount,
            m_sceneInputs.geometry.rigidRouteInstanceCount,
            m_smokeTlas ? 1 : 0,
            m_smokeStaticBlas ? 1 : 0,
            m_smokeDynamicBlas ? 1 : 0,
            m_frameResources.primarySurfaceHistoryBuffers.surfaceCount,
            static_cast<unsigned long long>(m_frameResources.primarySurfaceHistoryBuffers.surfaceBytes),
            m_frameResources.smokeReservoirBuffers.reservoirCount,
            static_cast<unsigned long long>(m_frameResources.smokeReservoirBuffers.reservoirBytes),
            m_frameResources.restirPTReservoirBuffers.reservoirElementCount,
            static_cast<unsigned long long>(m_frameResources.restirPTReservoirBuffers.reservoirBytes),
            m_smokeBindingSet ? 1 : 0,
            m_smokeTextureDescriptorTable ? 1 : 0);
        r_pathTracingSafetyDump.SetInteger(0);
    }
    if (r_pathTracingDispatchTileDump.GetInteger() != 0)
    {
        common->Printf(
            "PathTracePrimaryPass: PT tiled dispatch enabled=%d output=%dx%d tile=%dx%d tileCount=%d (%dx%d) estimatedRaysPerPixel=%d estimatedRaysPerTile=%llu estimatedRaysFullFrame=%llu\n",
            dispatchTileSettings.enabled ? 1 : 0,
            m_frameResources.width,
            m_frameResources.height,
            dispatchTileSettings.tileWidth,
            dispatchTileSettings.tileHeight,
            dispatchTileSettings.tileCount,
            dispatchTileSettings.tileColumns,
            dispatchTileSettings.tileRows,
            estimatedRaysPerPixel,
            static_cast<unsigned long long>(dispatchTileSettings.estimatedRaysPerTile),
            static_cast<unsigned long long>(dispatchTileSettings.estimatedRaysFullFrame));
        r_pathTracingDispatchTileDump.SetInteger(0);
    }
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
    if ((debugMode == 14 || debugMode == 15 || debugMode == 18) && r_pathTracingLightDump.GetInteger() != 0)
    {
        common->Printf("PathTracePrimaryPass: RT smoke selected %d debug point lights selection=%s\n",
            selectedLightCount,
            lightSelectionMode == 0 ? "nearest" : "cameraInfluence");
        for (int i = 0; i < selectedLightCount; i++)
        {
            common->Printf("  light[%d]: index=%d origin=(%.2f %.2f %.2f) radius=%.2f distance=%.2f score=%.6f color=(%.3f %.3f %.3f) intensity=%.3f sprite=%d shader='%s'\n",
                i,
                selectedLights[i].index,
                selectedLights[i].origin.x,
                selectedLights[i].origin.y,
                selectedLights[i].origin.z,
                selectedLights[i].radius,
                idMath::Sqrt(selectedLights[i].distanceSquared),
                selectedLights[i].score,
                selectedLights[i].color.x,
                selectedLights[i].color.y,
                selectedLights[i].color.z,
                selectedLights[i].color.w,
                selectedLights[i].spriteProxy ? 1 : 0,
                selectedLights[i].shaderName.c_str());
        }
        r_pathTracingLightDump.SetInteger(0);
    }
    RunPathTraceDoomLightDiagnostics(viewDef);

    const uint64 constantsStartUs = Sys_Microseconds();
    if (optickGpuMarkers)
    {
        OPTICK_GPU_EVENT("PT GPU Write Dispatch Constants");
        commandList->writeBuffer(m_smokeConstantsBuffer, &constants, sizeof(constants));
        commandList->writeBuffer(m_restirPTConstantsBuffer, &m_frameResources.restirPTContextState.parameters, sizeof(m_frameResources.restirPTContextState.parameters));
        if (gpuBoundsOverlayLineCount > 0 && !m_smokeBoundsOverlayLines.empty())
        {
            commandList->writeBuffer(m_smokeBoundsOverlayLineBuffer, m_smokeBoundsOverlayLines.data(), sizeof(RtPathTraceBoundsOverlayLine) * gpuBoundsOverlayLineCount);
        }
    }
    else
    {
        commandList->writeBuffer(m_smokeConstantsBuffer, &constants, sizeof(constants));
        commandList->writeBuffer(m_restirPTConstantsBuffer, &m_frameResources.restirPTContextState.parameters, sizeof(m_frameResources.restirPTContextState.parameters));
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
            commandList->setBufferState(m_smokeStaticVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeStaticIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeStaticTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeStaticTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeStaticTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeMaterialTableBuffer, nvrhi::ResourceStates::ShaderResource);
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
        if (!standaloneDebugRouteRequested)
        {
            commandList->setBufferState(m_frameResources.smokeReservoirBuffers.current, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_frameResources.smokeReservoirBuffers.previous, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_frameResources.smokeReservoirBuffers.spatialScratch, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_frameResources.restirPTReservoirBuffers.reservoirs, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_frameResources.restirPTDiReservoirBuffers.reservoirs, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_frameResources.restirPTGiReservoirBuffers.reservoirs, nvrhi::ResourceStates::UnorderedAccess);
        }
        if (m_smokeCleanRtxdiDiCurrentReservoirBuffer)
        {
            commandList->setBufferState(m_smokeCleanRtxdiDiCurrentReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_smokeCleanRtxdiDiTemporalReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_smokeCleanRtxdiDiPreviousReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_smokeCleanRtxdiDiSpatialReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
        }
        if (m_remixRtxdiResources.GetDomain(PATH_TRACE_REMIX_RTXDI_RESERVOIR_DOMAIN_DI).reservoirs)
        {
            commandList->setBufferState(m_remixRtxdiResources.GetDomain(PATH_TRACE_REMIX_RTXDI_RESERVOIR_DOMAIN_DI).reservoirs, nvrhi::ResourceStates::UnorderedAccess);
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
            commandList->setBufferState(m_smokeStaticVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeStaticIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeStaticTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeStaticTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeStaticTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicVertexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleClassBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleMaterialBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeDynamicTriangleMaterialIndexBuffer, nvrhi::ResourceStates::ShaderResource);
            commandList->setBufferState(m_smokeMaterialTableBuffer, nvrhi::ResourceStates::ShaderResource);
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
        if (!standaloneDebugRouteRequested)
        {
            commandList->setBufferState(m_frameResources.smokeReservoirBuffers.current, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_frameResources.smokeReservoirBuffers.previous, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_frameResources.smokeReservoirBuffers.spatialScratch, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_frameResources.restirPTReservoirBuffers.reservoirs, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_frameResources.restirPTDiReservoirBuffers.reservoirs, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_frameResources.restirPTGiReservoirBuffers.reservoirs, nvrhi::ResourceStates::UnorderedAccess);
        }
        if (m_smokeCleanRtxdiDiCurrentReservoirBuffer)
        {
            commandList->setBufferState(m_smokeCleanRtxdiDiCurrentReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_smokeCleanRtxdiDiTemporalReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_smokeCleanRtxdiDiPreviousReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
            commandList->setBufferState(m_smokeCleanRtxdiDiSpatialReservoirBuffer, nvrhi::ResourceStates::UnorderedAccess);
        }
        if (m_remixRtxdiResources.GetDomain(PATH_TRACE_REMIX_RTXDI_RESERVOIR_DOMAIN_DI).reservoirs)
        {
            commandList->setBufferState(m_remixRtxdiResources.GetDomain(PATH_TRACE_REMIX_RTXDI_RESERVOIR_DOMAIN_DI).reservoirs, nvrhi::ResourceStates::UnorderedAccess);
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
    if (!standaloneDebugRouteRequested && disableReservoirWrites)
    {
        m_frameResources.smokeReservoirNeedsClear = true;
        m_frameResources.restirPTReservoirNeedsClear = true;
        m_frameResources.restirPTDiReservoirNeedsClear = true;
        m_frameResources.restirPTGiReservoirNeedsClear = true;
    }
    if (!standaloneDebugRouteRequested && disablePrimarySurfaceHistory &&
        (!m_frameResources.primarySurfaceHistoryNeedsClear ||
            m_frameResources.primarySurfaceHistoryView.valid ||
            m_frameResources.primarySurfaceHistoryState.currentValid ||
            m_frameResources.primarySurfaceHistoryState.previousValid))
    {
        m_frameResources.InvalidatePrimarySurfaceHistory(RT_FRAME_RESET_PRIMARY_HISTORY);
    }
    const bool reservoirClearRequested = !standaloneDebugRouteRequested && !disableReservoirWrites && m_frameResources.smokeReservoirNeedsClear;
    const uint64 reservoirClearStartUs = barrierCompleteUs;
    if (reservoirClearRequested)
    {
        if (optickGpuMarkers)
        {
            OPTICK_GPU_EVENT("PT GPU Clear Reservoirs");
        }
        if (ClearSmokeReservoirBuffers(commandList, m_frameResources.smokeReservoirBuffers))
        {
            m_frameResources.smokeReservoirNeedsClear = false;
            ++m_frameResources.smokeReservoirClearCount;
        }
    }
    const uint64 reservoirClearCompleteUs = Sys_Microseconds();
    const bool restirPTReservoirClearRequested = !standaloneDebugRouteRequested && !disableReservoirWrites && (requestedRestirPTDebugMode || mode18RestirDirectMode) && (m_frameResources.restirPTReservoirNeedsClear || m_frameResources.restirPTDiReservoirNeedsClear || m_frameResources.restirPTGiReservoirNeedsClear);
    const uint64 restirPTReservoirClearStartUs = reservoirClearCompleteUs;
    if (restirPTReservoirClearRequested)
    {
        if (optickGpuMarkers)
        {
            OPTICK_GPU_EVENT("PT GPU Clear ReSTIR PT Reservoirs");
        }
        if (ClearRestirPTReservoirBuffers(commandList, m_frameResources.restirPTReservoirBuffers))
        {
            m_frameResources.restirPTReservoirNeedsClear = false;
            ++m_frameResources.restirPTReservoirClearCount;
        }
        if (ClearRestirPTReservoirBuffers(commandList, m_frameResources.restirPTDiReservoirBuffers))
        {
            m_frameResources.restirPTDiReservoirNeedsClear = false;
            ++m_frameResources.restirPTDiReservoirClearCount;
        }
        if (ClearRestirPTReservoirBuffers(commandList, m_frameResources.restirPTGiReservoirBuffers))
        {
            m_frameResources.restirPTGiReservoirNeedsClear = false;
            ++m_frameResources.restirPTGiReservoirClearCount;
        }
    }
    const bool remixRtxdiDiClearRequested =
        PathTraceRemixRtxdiResourceGateDiClearSource(PathTraceRemixRtxdiResourceGateDesc{
            restirPTCombinedMode,
            restirPTDiDebugView,
            r_pathTracingRemixRtxdiResourcesEnable.GetInteger() != 0,
            r_pathTracingRestirPTRrxDebugFlatContribution.GetInteger() != 0,
            r_pathTracingRestirPTRrxFinalConsumerOutput.GetInteger() != 0,
            disableReservoirWrites }) != PATH_TRACE_REMIX_RTXDI_DI_CLEAR_SOURCE_NONE &&
        m_remixRtxdiResources.GetDomain(PATH_TRACE_REMIX_RTXDI_RESERVOIR_DOMAIN_DI).clearPending;
    if (remixRtxdiDiClearRequested)
    {
        if (optickGpuMarkers)
        {
            OPTICK_GPU_EVENT("PT GPU Clear RRX DI Reservoirs");
        }
        m_remixRtxdiResources.ClearPendingDomain(commandList, PATH_TRACE_REMIX_RTXDI_RESERVOIR_DOMAIN_DI);
        updateRemixDiReservoirProbeConstants();
    }
    const uint64 restirPTReservoirClearCompleteUs = Sys_Microseconds();
    const bool primaryHistoryClearRequested = !standaloneDebugRouteRequested && !disablePrimarySurfaceHistory && m_frameResources.primarySurfaceHistoryNeedsClear;
    const uint64 primaryHistoryClearStartUs = restirPTReservoirClearCompleteUs;
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
    nvrhi::rt::DispatchRaysArguments restirDirectProducerArgs;
    restirDirectProducerArgs.width = restirPTDirectProducerWidth;
    restirDirectProducerArgs.height = restirPTDirectHeight;
    restirDirectProducerArgs.depth = 1;
    nvrhi::rt::DispatchRaysArguments restirGiProducerArgs;
    restirGiProducerArgs.width = restirPTGiProducerWidth;
    restirGiProducerArgs.height = m_frameResources.height;
    restirGiProducerArgs.depth = 1;
    int timingDispatchWidth = args.width;
    int timingDispatchHeight = args.height;

    auto dispatchSmokeRays = [&](const nvrhi::rt::DispatchRaysArguments& dispatchArgs, int domainWidth, int domainHeight, int restirShaderDispatchMode, bool restirIndirectProducerDispatch = false, bool restirIndirectSparseProducerDispatch = false, int restirPTDiTemporalPrepassMode = 0)
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
                    tileConstants.restirPTDirectInfo[2] = static_cast<float>(restirShaderDispatchMode);
                    tileConstants.restirPTIndirectInfo[0] = restirIndirectSparseProducerDispatch ? 2.0f : (restirIndirectProducerDispatch ? 1.0f : 0.0f);
                    tileConstants.restirPTDiDebugInfo[1] = static_cast<float>(restirPTDiTemporalPrepassMode);
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
            dispatchConstants.restirPTDirectInfo[2] = static_cast<float>(restirShaderDispatchMode);
            dispatchConstants.restirPTIndirectInfo[0] = restirIndirectSparseProducerDispatch ? 2.0f : (restirIndirectProducerDispatch ? 1.0f : 0.0f);
            dispatchConstants.restirPTDiDebugInfo[1] = static_cast<float>(restirPTDiTemporalPrepassMode);
            commandList->writeBuffer(m_smokeConstantsBuffer, &dispatchConstants, sizeof(dispatchConstants));
            commandList->dispatchRays(dispatchArgs);
        }
    };

    const uint64 setStateStartUs = targetClearCompleteUs;
    const bool spatialNeedsTemporalPrepass =
        PathTraceRestirPassRequiresTemporalPrepass(restirPTPassPlan);
    nvrhi::rt::ShaderTableHandle temporalPrepassShaderTable =
        restirPTPrimarySurfacePrepassEnabled && restirPTCombinedMode && m_smokeRestirDirectTemporalProducerShaderTable
            ? m_smokeRestirDirectTemporalProducerShaderTable
            : m_smokeRestirShaderTable;
    const bool temporalPrepassNeeded =
        spatialNeedsTemporalPrepass &&
        temporalPrepassShaderTable &&
        state.shaderTable != temporalPrepassShaderTable;
    nvrhi::rt::ShaderTableHandle indirectInitialShaderTable =
        restirPTPrimarySurfacePrepassEnabled && restirPTCombinedMode && m_smokeRestirIndirectInitialProducerShaderTable
            ? m_smokeRestirIndirectInitialProducerShaderTable
            : m_smokeRestirInitialShaderTable;
    const bool indirectNeedsInitialPrepass =
        PathTraceRestirPassRequiresInitialPrepass(restirPTPassPlan) &&
        indirectInitialShaderTable;
    nvrhi::rt::ShaderTableHandle spatialPrepassShaderTable =
        restirPTPrimarySurfacePrepassEnabled && restirPTCombinedMode && m_smokeRestirDirectSpatialReservoirProducerShaderTable
            ? m_smokeRestirDirectSpatialReservoirProducerShaderTable
            : m_smokeRestirSpatialReservoirShaderTable;
    const bool spatialNeedsSpatialPrepass =
        PathTraceRestirPassRequiresSpatialPrepass(restirPTPassPlan) &&
        spatialPrepassShaderTable &&
        state.shaderTable != spatialPrepassShaderTable;
    const int restirDirectProducerDispatchMode = restirPTPrimarySurfacePrepassEnabled
        ? (restirPTRaySparsity > 1 ? RT_RESTIR_PT_SHADER_DISPATCH_DIRECT_CONSUME_PRIMARY_SPARSE : RT_RESTIR_PT_SHADER_DISPATCH_DIRECT_CONSUME_PRIMARY)
        : (restirPTRaySparsity > 1 ? RT_RESTIR_PT_SHADER_DISPATCH_DIRECT_TRACE_PRIMARY_SPARSE : RT_RESTIR_PT_SHADER_DISPATCH_DIRECT_TRACE_PRIMARY);
    const int restirIndirectProducerDispatchMode = restirPTPrimarySurfacePrepassEnabled
        ? RT_RESTIR_PT_SHADER_DISPATCH_FULL_CONSUME_PRIMARY
        : RT_RESTIR_PT_SHADER_DISPATCH_FULL;
    const int finalDispatchMode = restirPTPrimarySurfacePrepassEnabled && restirPTCombinedMode
        ? RT_RESTIR_PT_SHADER_DISPATCH_FULL_CONSUME_PRIMARY
        : RT_RESTIR_PT_SHADER_DISPATCH_FULL;
    if (restirPTStandalonePrimarySurfacePrepass && m_smokePrimarySurfaceProducerShaderTable)
    {
        nvrhi::rt::State primarySurfacePrepassState = state;
        primarySurfacePrepassState.shaderTable = m_smokePrimarySurfaceProducerShaderTable;
        {
            PathTraceGpuMarkerScope nsightMarker(commandList, "PT56.0 PrimarySurfacePrepass DispatchRays", nsightGpuMarkers);
            PathTraceGpuTimingStageScope timingStage(gpuTimingCapture, commandList, PT_GPU_TIMING_PRIMARY_SURFACE);
            if (optickGpuMarkers)
            {
                OPTICK_GPU_EVENT("PT GPU ReSTIR Primary Surface Prepass");
                commandList->setRayTracingState(primarySurfacePrepassState);
                dispatchSmokeRays(args, m_frameResources.width, m_frameResources.height, RT_RESTIR_PT_SHADER_DISPATCH_PRIMARY_SURFACE_ONLY);
            }
            else
            {
                commandList->setRayTracingState(primarySurfacePrepassState);
                dispatchSmokeRays(args, m_frameResources.width, m_frameResources.height, RT_RESTIR_PT_SHADER_DISPATCH_PRIMARY_SURFACE_ONLY);
            }
        }

        nvrhi::utils::BufferUavBarrier(commandList, m_frameResources.primarySurfaceHistoryBuffers.current);
        nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.motionVectorTexture);
        nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.motionVectorMaskTexture);
        nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideAlbedoTexture);
        nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideSpecularAlbedoTexture);
        nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideNormalRoughnessTexture);
        nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideDepthTexture);
        nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideHitDistanceTexture);
        nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideResetMaskTexture);
    }
    if (indirectNeedsInitialPrepass)
    {
        nvrhi::rt::State indirectPrepassState = state;
        indirectPrepassState.shaderTable = indirectInitialShaderTable;
        {
            PathTraceGpuMarkerScope nsightMarker(commandList, "PT56.1 GIInitialProducer DispatchRays", nsightGpuMarkers);
            PathTraceGpuTimingStageScope timingStage(gpuTimingCapture, commandList, PT_GPU_TIMING_GI_INITIAL);
            if (optickGpuMarkers)
            {
                OPTICK_GPU_EVENT("PT GPU ReSTIR Indirect Initial Prepass");
                commandList->setRayTracingState(indirectPrepassState);
                dispatchSmokeRays(restirGiProducerArgs, restirPTGiProducerWidth, m_frameResources.height, restirIndirectProducerDispatchMode, true, restirPTGiRaySparsity > 1);
            }
            else
            {
                commandList->setRayTracingState(indirectPrepassState);
                dispatchSmokeRays(restirGiProducerArgs, restirPTGiProducerWidth, m_frameResources.height, restirIndirectProducerDispatchMode, true, restirPTGiRaySparsity > 1);
            }
        }

        nvrhi::utils::BufferUavBarrier(commandList, m_frameResources.primarySurfaceHistoryBuffers.current);
        nvrhi::utils::BufferUavBarrier(commandList, m_frameResources.restirPTReservoirBuffers.reservoirs);
        nvrhi::utils::BufferUavBarrier(commandList, m_frameResources.restirPTDiReservoirBuffers.reservoirs);
        nvrhi::utils::BufferUavBarrier(commandList, m_frameResources.restirPTGiReservoirBuffers.reservoirs);
        nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.outputTexture);
    }
    if (temporalPrepassNeeded)
    {
        // RTXDI spatial resampling reads completed current-frame neighbor
        // surfaces and temporal reservoirs. Produce those in a separate
        // dispatch before the spatial shader consumes them.
        nvrhi::rt::State temporalPrepassState = state;
        temporalPrepassState.shaderTable = temporalPrepassShaderTable;
        {
            PathTraceGpuMarkerScope nsightMarker(commandList, "PT56.2 DirectTemporalProducer DispatchRays", nsightGpuMarkers);
            PathTraceGpuTimingStageScope timingStage(gpuTimingCapture, commandList, PT_GPU_TIMING_DIRECT_TEMPORAL);
            if (optickGpuMarkers)
            {
                OPTICK_GPU_EVENT("PT GPU ReSTIR Spatial Temporal Prepass");
                commandList->setRayTracingState(temporalPrepassState);
                dispatchSmokeRays(restirDirectProducerArgs, restirPTDirectProducerWidth, restirPTDirectHeight, stagedRestirDirectLightingMode ? restirDirectProducerDispatchMode : RT_RESTIR_PT_SHADER_DISPATCH_FULL);
            }
            else
            {
                commandList->setRayTracingState(temporalPrepassState);
                dispatchSmokeRays(restirDirectProducerArgs, restirPTDirectProducerWidth, restirPTDirectHeight, stagedRestirDirectLightingMode ? restirDirectProducerDispatchMode : RT_RESTIR_PT_SHADER_DISPATCH_FULL);
            }
        }

        nvrhi::utils::BufferUavBarrier(commandList, m_frameResources.primarySurfaceHistoryBuffers.current);
        nvrhi::utils::BufferUavBarrier(commandList, m_frameResources.restirPTReservoirBuffers.reservoirs);
        nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.outputTexture);
    }
    if (spatialNeedsSpatialPrepass)
    {
        nvrhi::rt::State spatialPrepassState = state;
        spatialPrepassState.shaderTable = spatialPrepassShaderTable;
        {
            PathTraceGpuMarkerScope nsightMarker(commandList, "PT56.3 DirectSpatialReservoirProducer DispatchRays", nsightGpuMarkers);
            PathTraceGpuTimingStageScope timingStage(gpuTimingCapture, commandList, PT_GPU_TIMING_DIRECT_SPATIAL);
            if (optickGpuMarkers)
            {
                OPTICK_GPU_EVENT("PT GPU ReSTIR Spatial Reservoir Prepass");
                commandList->setRayTracingState(spatialPrepassState);
                dispatchSmokeRays(restirDirectProducerArgs, restirPTDirectProducerWidth, restirPTDirectHeight, stagedRestirDirectLightingMode ? restirDirectProducerDispatchMode : RT_RESTIR_PT_SHADER_DISPATCH_FULL);
            }
            else
            {
                commandList->setRayTracingState(spatialPrepassState);
                dispatchSmokeRays(restirDirectProducerArgs, restirPTDirectProducerWidth, restirPTDirectHeight, stagedRestirDirectLightingMode ? restirDirectProducerDispatchMode : RT_RESTIR_PT_SHADER_DISPATCH_FULL);
            }
        }

        nvrhi::utils::BufferUavBarrier(commandList, m_frameResources.restirPTReservoirBuffers.reservoirs);
        nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.outputTexture);
    }
    const bool diSpatialDebugTemporalPrepassNeeded =
        restirPTCombinedResolveActive &&
        (restirPTDiDebugView == 3 || restirPTDiDebugView == 5);
    const bool rrxDiSpatialTemporalPrepassNeeded =
        restirPTCombinedResolveActive &&
        (restirPTDiDebugView == 72 || restirPTDiDebugView == 73 ||
         restirPTDiDebugView == 74 || restirPTDiDebugView == 77 ||
         (restirPTDiDebugView == 0 && r_pathTracingRestirPTRrxFinalConsumerOutput.GetBool()));
    if (diSpatialDebugTemporalPrepassNeeded || rrxDiSpatialTemporalPrepassNeeded)
    {
        {
            PathTraceGpuMarkerScope nsightMarker(commandList, "PT56.3b DITemporalPrepass DispatchRays", nsightGpuMarkers);
            PathTraceGpuTimingStageScope timingStage(gpuTimingCapture, commandList, PT_GPU_TIMING_DIRECT_TEMPORAL);
            const int diTemporalPrepassMode = rrxDiSpatialTemporalPrepassNeeded ? 2 : 1;
            if (optickGpuMarkers)
            {
                OPTICK_GPU_EVENT("PT GPU ReSTIR DI Temporal Debug Prepass");
                commandList->setRayTracingState(state);
                dispatchSmokeRays(args, m_frameResources.width, m_frameResources.height, finalDispatchMode, false, false, diTemporalPrepassMode);
            }
            else
            {
                commandList->setRayTracingState(state);
                dispatchSmokeRays(args, m_frameResources.width, m_frameResources.height, finalDispatchMode, false, false, diTemporalPrepassMode);
            }
        }

        const PathTraceRemixRtxdiReservoirDomain& prepassRemixDiDomain = m_remixRtxdiResources.GetDomain(PATH_TRACE_REMIX_RTXDI_RESERVOIR_DOMAIN_DI);
        nvrhi::utils::BufferUavBarrier(commandList, rrxDiSpatialTemporalPrepassNeeded && prepassRemixDiDomain.reservoirs
            ? prepassRemixDiDomain.reservoirs
            : m_frameResources.restirPTDiReservoirBuffers.reservoirs);
    }
    const bool reflectionProducerNeeded =
        restirPTPrimarySurfacePrepassEnabled &&
        restirPTCombinedMode &&
        restirPTReflectionMode > 0 &&
        m_smokeRestirReflectionProducerShaderTable;
    if (reflectionProducerNeeded)
    {
        nvrhi::rt::State reflectionProducerState = state;
        reflectionProducerState.shaderTable = m_smokeRestirReflectionProducerShaderTable;
        {
            PathTraceGpuMarkerScope nsightMarker(commandList, "PT56.4 ReflectionProducer DispatchRays", nsightGpuMarkers);
            PathTraceGpuTimingStageScope timingStage(gpuTimingCapture, commandList, PT_GPU_TIMING_REFLECTION);
            if (optickGpuMarkers)
            {
                OPTICK_GPU_EVENT("PT GPU ReSTIR Reflection Producer");
                commandList->setRayTracingState(reflectionProducerState);
                dispatchSmokeRays(args, m_frameResources.width, m_frameResources.height, RT_RESTIR_PT_SHADER_DISPATCH_FULL_CONSUME_PRIMARY);
            }
            else
            {
                commandList->setRayTracingState(reflectionProducerState);
                dispatchSmokeRays(args, m_frameResources.width, m_frameResources.height, RT_RESTIR_PT_SHADER_DISPATCH_FULL_CONSUME_PRIMARY);
            }
        }

        nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.restirPTReflectionTexture);
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
        PathTraceGpuMarkerScope finalDispatchNsightMarker(
            commandList,
            restirPTCombinedResolveActive ? "PT56.5 CombinedResolve DispatchRays" : "PT Final DispatchRays",
            nsightGpuMarkers);
        PathTraceGpuTimingStageScope timingStage(gpuTimingCapture, commandList, PT_GPU_TIMING_FINAL_RESOLVE);
        if (regirDebugRouteRequested || pdfNeeReGIRBuildPrepassRequested)
        {
            PathTraceSmokeConstants regirBuildConstants = constants;
            regirBuildConstants.regirInfo3[0] = 1.0f;
            regirBuildConstants.dispatchTileInfo[0] = 0.0f;
            regirBuildConstants.dispatchTileInfo[1] = 0.0f;
            regirBuildConstants.dispatchTileInfo[2] = static_cast<float>(m_frameResources.width);
            regirBuildConstants.dispatchTileInfo[3] = static_cast<float>(m_frameResources.height);
            commandList->writeBuffer(m_smokeConstantsBuffer, &regirBuildConstants, sizeof(regirBuildConstants));
            if (pdfNeeReGIRBuildPrepassRequested && !regirDebugRouteRequested)
            {
                nvrhi::rt::State regirBuildState;
                regirBuildState.shaderTable = m_smokeReGIRDebugShaderTable;
                regirBuildState.bindings = { regirDebugBindingSet, m_smokeTextureDescriptorTable };
                commandList->setRayTracingState(regirBuildState);
            }
            commandList->dispatchRays(args);
            nvrhi::utils::BufferUavBarrier(commandList, m_smokeReGIRState.candidateCacheBuffer);
            if (pdfNeeReGIRBuildPrepassRequested && !regirDebugRouteRequested)
            {
                commandList->setBufferState(m_smokeReGIRState.candidateCacheBuffer, nvrhi::ResourceStates::ShaderResource);
            }
            commandList->commitBarriers();
            commandList->writeBuffer(m_smokeConstantsBuffer, &constants, sizeof(constants));
            if (pdfNeeReGIRBuildPrepassRequested && !regirDebugRouteRequested)
            {
                commandList->setRayTracingState(state);
            }
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
            dispatchSmokeRays(args, m_frameResources.width, m_frameResources.height, finalDispatchMode);
        }
        else
        {
            if (optickGpuMarkers)
            {
                OPTICK_GPU_EVENT("PT GPU Dispatch Rays");
                dispatchSmokeRays(args, m_frameResources.width, m_frameResources.height, finalDispatchMode);
            }
            else
            {
                dispatchSmokeRays(args, m_frameResources.width, m_frameResources.height, finalDispatchMode);
            }
        }
    }
    const uint64 dispatchRaysCompleteUs = Sys_Microseconds();
    const bool dlssRrEvaluateRequested =
        restirPTCombinedMode &&
        restirPTPrimarySurfacePrepassEnabled &&
        dlssRrRuntimeRequested;
    const uint64 dlssRrStartUs = dispatchRaysCompleteUs;
    if (dlssRrEvaluateRequested)
    {
        const idVec2 dlssRrJitterPixels = PathTraceDLSSRRPixelJitter(viewDef, static_cast<uint32_t>(restirPTFrameIndex), false);
        nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.outputTexture);
        nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrInputColorTexture);
        nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.motionVectorTexture);
        nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideAlbedoTexture);
        nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideSpecularAlbedoTexture);
        nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideNormalRoughnessTexture);
        nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideDepthTexture);
        nvrhi::utils::TextureUavBarrier(commandList, m_frameResources.rrGuideHitDistanceTexture);
        commandList->setTextureState(m_frameResources.outputTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        commandList->setTextureState(m_frameResources.rrInputColorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        commandList->setTextureState(m_frameResources.motionVectorTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        commandList->setTextureState(m_frameResources.rrGuideAlbedoTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        commandList->setTextureState(m_frameResources.rrGuideSpecularAlbedoTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        commandList->setTextureState(m_frameResources.rrGuideNormalRoughnessTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        commandList->setTextureState(m_frameResources.rrGuideDepthTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        commandList->setTextureState(m_frameResources.rrGuideHitDistanceTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        commandList->setTextureState(m_frameResources.rrGuideResetMaskTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
        commandList->setTextureState(m_frameResources.accumulationTexture, nvrhi::AllSubresources, nvrhi::ResourceStates::UnorderedAccess);
        commandList->commitBarriers();

        const bool rrForceReset = r_pathTracingDLSSRRForceReset.GetInteger() != 0;
        const bool historyReset =
            rrForceReset ||
            !previousHistoryViewValid ||
            m_frameResources.primarySurfaceHistoryNeedsClear ||
            m_frameResources.settings.resetReasonFlags != 0;
        QueueDLSSRRInputColorDump(commandList, m_frameResources.rrInputColorTexture, 2, static_cast<uint32_t>(restirPTFrameIndex));
        const bool rrEvaluated = PathTraceDLSSRRBridge_Evaluate(
            commandList,
            m_frameResources.rrInputColorTexture,
            m_frameResources.accumulationTexture,
            m_frameResources.rrGuideAlbedoTexture,
            m_frameResources.rrGuideSpecularAlbedoTexture,
            m_frameResources.rrGuideNormalRoughnessTexture,
            nullptr,
            m_frameResources.rrGuideDepthTexture,
            m_frameResources.motionVectorTexture,
            m_frameResources.rrGuideHitDistanceTexture,
            nullptr,
            viewDef,
            static_cast<uint32_t>(restirPTFrameIndex),
            m_frameResources.width,
            m_frameResources.height,
            m_frameResources.outputWidth,
            m_frameResources.outputHeight,
            dlssRrJitterPixels.x,
            dlssRrJitterPixels.y,
            historyReset ? nullptr : &m_frameResources.primarySurfaceHistoryView,
            historyReset);
        if (rrEvaluated)
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
    const uint64 dlssRrCompleteUs = Sys_Microseconds();
    const uint64 historyCopyStartUs = dlssRrCompleteUs;
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
    if (mode56AccumulationActive)
    {
        m_frameResources.mode56AccumulationFrameCount = Min(m_frameResources.mode56AccumulationFrameCount + 1, mode56AccumulationMaxFrames);
    }
    else
    {
        m_frameResources.mode56AccumulationFrameCount = 0;
    }
    const bool forceOverlapReadback = debugMode == 24 && r_pathTracingRigidRouteOverlapDump.GetInteger() != 0;
    const bool forceView68Readback = debugMode == 56 && (restirPTDiDebugView == 68 || restirPTDiDebugView == 69) && r_pathTracingRestirPTView68Dump.GetInteger() != 0;
    const bool forceCleanTemporalAuditReadback = r_pathTracingCleanRtxdiDiTemporalAudit.GetInteger() != 0;
    bool readbackQueuedThisFrame = false;
    const uint64 readbackCopyStartUs = historyCopyCompleteUs;
    if (forceView68Readback && r_pathTracingRestirPTView68Dump.GetInteger() > 1)
    {
        common->Printf("PathTracePrimaryPass: PT mode56 view68 readback active queued=%d cooldown=%d delay=%d texture=%d\n",
            m_frameResources.readbackQueued ? 1 : 0,
            m_frameResources.readbackCooldownFrames,
            m_frameResources.readbackDelayFrames,
            m_frameResources.readbackTexture ? 1 : 0);
    }
    if ((r_pathTracingReadbackEnable.GetInteger() != 0 || forceOverlapReadback || forceView68Readback || forceCleanTemporalAuditReadback) && !m_frameResources.readbackQueued && (m_frameResources.readbackCooldownFrames <= 0 || forceOverlapReadback || forceView68Readback || forceCleanTemporalAuditReadback))
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
        if (r_pathTracingSmokeLog.GetInteger() != 0 || forceView68Readback || forceCleanTemporalAuditReadback)
        {
            common->Printf("PathTracePrimaryPass: queued RT smoke UAV readback\n");
        }
    }
    const uint64 readbackCopyCompleteUs = Sys_Microseconds();

    const uint64 totalSubmitUs = readbackCopyCompleteUs - executeStartUs;
    const int totalSubmitMsForThrottle = static_cast<int>((totalSubmitUs + 999u) / 1000u);
    const bool forcePassTimingDump = r_pathTracingPassTimingDump.GetInteger() != 0;
    if (forcePassTimingDump || ShouldLogSmokeTiming(totalSubmitMsForThrottle, Sys_Milliseconds(), g_smokeLastDispatchTimingLogMs))
    {
        RtPathTraceDispatchTimingLogDesc timingDesc;
        timingDesc.totalSubmitMs = PathTraceMicrosecondsToMilliseconds(totalSubmitUs);
        timingDesc.setupMs = PathTraceMicrosecondsToMilliseconds(setupCompleteUs - executeStartUs);
        timingDesc.restirContextMs = PathTraceMicrosecondsToMilliseconds(restirContextCompleteUs - setupCompleteUs);
        timingDesc.constantsMs = PathTraceMicrosecondsToMilliseconds(constantsCompleteUs - constantsStartUs);
        timingDesc.barrierMs = PathTraceMicrosecondsToMilliseconds(barrierCompleteUs - barrierStartUs);
        timingDesc.reservoirClearMs = PathTraceMicrosecondsToMilliseconds(reservoirClearCompleteUs - reservoirClearStartUs);
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
        timingDesc.restirResamplingMode = static_cast<int>(restirPTPassPlan.resamplingMode);
        timingDesc.restirPreviewVisibility = (restirPTPassPlan.flags & RT_RESTIR_PASS_TRACES_VISIBILITY) != 0 ? 1 : 0;
        timingDesc.restirPreviewMaxPixels = r_pathTracingRestirPTPreviewMaxPixels.GetInteger();
        timingDesc.reservoirClearRequested = reservoirClearRequested;
        timingDesc.primaryHistoryClearRequested = primaryHistoryClearRequested;
        timingDesc.readbackQueued = readbackQueuedThisFrame;
        timingDesc.optickGpuMarkers = optickGpuMarkers;
        timingDesc.nsightGpuMarkers = nsightGpuMarkers;
        timingDesc.debugModeInfo = &debugModeInfo;
        timingDesc.restirPassLabel = restirPTPassPlan.label;
        LogPathTraceDispatchTiming(timingDesc);
        if (forcePassTimingDump)
        {
            r_pathTracingPassTimingDump.SetInteger(0);
        }
    }

    if (!m_smokeTestDispatched)
    {
        common->Printf("PathTracePrimaryPass: dispatched RT smoke camera raygen (%dx%d, debugMode=%d)\n", m_frameResources.width, m_frameResources.height, debugMode);
    }
    m_smokeTestDispatched = true;
}
