bool PathTraceCleanRoomTemporalReuseEnabled()
{
    return (CleanRtxdiDiTemporalFlags & CLEAN_TEMPORAL_FLAG_ENABLE) != 0u &&
        (CleanRtxdiDiTemporalFlags & CLEAN_TEMPORAL_FLAG_PREVIOUS_VALID) != 0u;
}

bool PathTraceCleanRoomSurfaceIsTransmissionPsrResolved(PathTracePrimarySurfaceRecord surface)
{
    return (surface.header.w & CLEAN_SURFACE_FLAG_TRANSMISSION_PSR_RESOLVED) != 0u;
}

bool PathTraceCleanRoomTemporalRigidEmissiveBypassEnabled()
{
    return (CleanRtxdiDiFlags & CLEAN_RAB_DIAGNOSTIC_DISABLE_RIGID_EMISSIVE_TEMPORAL) != 0u &&
        PathTraceCleanRoomRemixLightUniverseEnabled();
}

bool PathTraceCleanRoomLightIsTemporalUnsafeEmissive(PathTraceUnifiedLightRecord light)
{
    const bool dynamicHistory = (light.flags & RT_SMOKE_EMISSIVE_TRIANGLE_HISTORY_DYNAMIC) != 0u;
    return light.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE &&
        (light.instanceId != 0u || dynamicHistory);
}

bool PathTraceCleanRoomCurrentReservoirSelectsTemporalUnsafeEmissive(RTXDI_DIReservoir reservoir)
{
    const uint lightIndex = RTXDI_GetDIReservoirLightIndex(reservoir);
    if (lightIndex >= CleanRtxdiDiRluCurrentLightCount)
    {
        return false;
    }

    const PathTraceUnifiedLightRecord light = CleanRtxdiDiRluCurrentLights[lightIndex];
    return PathTraceCleanRoomLightIsTemporalUnsafeEmissive(light);
}

bool PathTraceCleanRoomPreviousReservoirMapsToTemporalUnsafeEmissive(RTXDI_DIReservoir reservoir)
{
    const uint previousLightIndex = RTXDI_GetDIReservoirLightIndex(reservoir);
    if (previousLightIndex >= CleanRtxdiDiRluPreviousToCurrentCount)
    {
        return false;
    }

    const uint currentLightIndex = CleanRtxdiDiRluPreviousToCurrent[previousLightIndex];
    if (currentLightIndex >= CleanRtxdiDiRluCurrentLightCount)
    {
        return false;
    }

    const PathTraceUnifiedLightRecord light = CleanRtxdiDiRluCurrentLights[currentLightIndex];
    return PathTraceCleanRoomLightIsTemporalUnsafeEmissive(light);
}

bool PathTraceCleanRoomProjectCameraMotion(PathTracePrimarySurfaceRecord currentSurface, uint2 pixel, uint2 dimensions, out float3 screenSpaceMotion)
{
    screenSpaceMotion = float3(0.0, 0.0, 0.0);
    if (CleanRtxdiDiPrevCameraOriginAndValid.w < 0.5)
    {
        return false;
    }

    const float3 delta = currentSurface.worldPositionAndViewDepth.xyz - CleanRtxdiDiPrevCameraOriginAndValid.xyz;
    const float forwardDistance = dot(delta, CleanRtxdiDiPrevCameraForwardAndTanX.xyz);
    if (forwardDistance <= 0.05)
    {
        return false;
    }

    const float ndcX = -dot(delta, CleanRtxdiDiPrevCameraLeftAndTanY.xyz) / max(forwardDistance * CleanRtxdiDiPrevCameraForwardAndTanX.w, 1.0e-5);
    const float ndcY = -dot(delta, CleanRtxdiDiPrevCameraUpAndTanY.xyz) / max(forwardDistance * CleanRtxdiDiPrevCameraLeftAndTanY.w, 1.0e-5);
    if (abs(ndcX) > 1.0 || abs(ndcY) > 1.0)
    {
        return false;
    }

    const float2 previousPixelFloat = (float2(ndcX, ndcY) * 0.5 + 0.5) * float2(dimensions);
    if (!all(previousPixelFloat == previousPixelFloat) ||
        previousPixelFloat.x < 0.0 || previousPixelFloat.y < 0.0 ||
        previousPixelFloat.x >= (float)dimensions.x || previousPixelFloat.y >= (float)dimensions.y)
    {
        return false;
    }

    const float previousLinearDepth = length(delta);
    screenSpaceMotion = float3(previousPixelFloat - (float2(pixel) + 0.5), previousLinearDepth - currentSurface.worldPositionAndViewDepth.w);
    return all(screenSpaceMotion == screenSpaceMotion);
}

bool PathTraceCleanRoomLoadMotion(uint2 pixel, PathTracePrimarySurfaceRecord currentSurface, uint2 dimensions, out float3 screenSpaceMotion, out bool cameraReprojected)
{
    screenSpaceMotion = float3(0.0, 0.0, 0.0);
    cameraReprojected = false;
    if (pixel.x >= CleanRtxdiDiWidth || pixel.y >= CleanRtxdiDiHeight)
    {
        return false;
    }

    const uint motionMask = PathTraceMotionVectorMask[pixel];
    if ((motionMask & PT_MOTION_VECTOR_MASK_VALID) != 0u)
    {
        screenSpaceMotion = PathTraceMotionVectors[pixel].xyz;
        return all(screenSpaceMotion == screenSpaceMotion);
    }

    cameraReprojected = PathTraceCleanRoomProjectCameraMotion(currentSurface, pixel, dimensions, screenSpaceMotion);
    return cameraReprojected;
}

PathTraceCleanRtxdiDiTemporalResult PathTraceCleanRoomRunTemporalProducer(uint2 pixel, uint2 dimensions, RTXDI_DIReservoir currentReservoir, PathTracePrimarySurfaceRecord currentSurface)
{
    PathTraceCleanRtxdiDiTemporalResult result = (PathTraceCleanRtxdiDiTemporalResult)0;
    result.reservoir = currentReservoir;
    result.previousPixel = int2(-1, -1);
    result.temporalSamplePixel = int2(-1, -1);

    if (RTXDI_IsValidDIReservoir(currentReservoir))
    {
        result.flags |= CLEAN_TEMPORAL_DIAG_CURRENT_VALID;
    }
    if (currentReservoir.M > 0.0)
    {
        result.flags |= CLEAN_TEMPORAL_DIAG_CURRENT_CANDIDATE;
    }
    if ((CleanRtxdiDiTemporalFlags & CLEAN_TEMPORAL_FLAG_ENABLE) != 0u)
    {
        result.flags |= CLEAN_TEMPORAL_DIAG_ENABLED;
    }
    if ((CleanRtxdiDiTemporalFlags & CLEAN_TEMPORAL_FLAG_PREVIOUS_VALID) != 0u)
    {
        result.flags |= CLEAN_TEMPORAL_DIAG_PREVIOUS_FRAME_VALID;
    }
    if (currentReservoir.M <= 0.0 || !PathTraceCleanRoomTemporalReuseEnabled())
    {
        return result;
    }
    if (PathTraceCleanRoomSurfaceIsTransmissionPsrResolved(currentSurface))
    {
        return result;
    }
    const bool rigidEmissiveTemporalBypass = PathTraceCleanRoomTemporalRigidEmissiveBypassEnabled();
    if (rigidEmissiveTemporalBypass &&
        PathTraceCleanRoomCurrentReservoirSelectsTemporalUnsafeEmissive(currentReservoir))
    {
        return result;
    }

    const RAB_Surface surface = PathTraceCleanRoomSurfaceForView(currentSurface);
    if (RAB_IsSurfaceValid(surface))
    {
        result.flags |= CLEAN_TEMPORAL_DIAG_CURRENT_SURFACE_VALID;
    }
    else
    {
        return result;
    }

    float3 screenSpaceMotion;
    bool cameraReprojected = false;
    if (!PathTraceCleanRoomLoadMotion(pixel, currentSurface, dimensions, screenSpaceMotion, cameraReprojected))
    {
        return result;
    }
    result.flags |= CLEAN_TEMPORAL_DIAG_MOTION_VALID;
    if (cameraReprojected)
    {
        result.flags |= CLEAN_TEMPORAL_DIAG_CAMERA_REPROJECTED;
    }

    const int2 previousPixel = int2(round(float2(pixel) + screenSpaceMotion.xy));
    result.previousPixel = previousPixel;
    PathTracePrimarySurfaceRecord previousSurfaceRecord;
    if (previousPixel.x >= 0 && previousPixel.y >= 0 && (uint)previousPixel.x < dimensions.x && (uint)previousPixel.y < dimensions.y)
    {
        result.flags |= CLEAN_TEMPORAL_DIAG_PREVIOUS_PIXEL_IN_BOUNDS;
    }
    if (PathTraceCleanRoomLoadSurfaceRecordSigned(previousPixel, dimensions, true, previousSurfaceRecord))
    {
        result.flags |= CLEAN_TEMPORAL_DIAG_PREVIOUS_SURFACE_VALID;
    }

    if (previousPixel.x >= 0 && previousPixel.y >= 0 && (uint)previousPixel.x < dimensions.x && (uint)previousPixel.y < dimensions.y)
    {
        const RTXDI_DIReservoir previousReservoir = RTXDI_LoadDIReservoir(PathTraceCleanRoomReservoirParams(dimensions), (uint2)previousPixel, 0u);
        if (RTXDI_IsValidDIReservoir(previousReservoir))
        {
            result.flags |= CLEAN_TEMPORAL_DIAG_PREVIOUS_RESERVOIR_VALID;
            result.previousM = previousReservoir.M;
            result.previousTargetPdf = previousReservoir.targetPdf;
            result.previousInvPdf = RTXDI_GetDIReservoirInvPdf(previousReservoir);
            if (rigidEmissiveTemporalBypass &&
                PathTraceCleanRoomPreviousReservoirMapsToTemporalUnsafeEmissive(previousReservoir))
            {
                return result;
            }
            const int mappedPreviousLightIndex = RAB_TranslateLightIndex(RTXDI_GetDIReservoirLightIndex(previousReservoir), false);
            if (mappedPreviousLightIndex >= 0)
            {
                result.flags |= CLEAN_TEMPORAL_DIAG_PREVIOUS_LIGHT_MAPPED;

                const RAB_LightInfo mappedPreviousLight = RAB_LoadLightInfo((uint)mappedPreviousLightIndex, false);
                const RAB_LightSample mappedPreviousSample = RAB_SamplePolymorphicLight(
                    mappedPreviousLight, surface, RTXDI_GetDIReservoirSampleUV(previousReservoir));
                result.previousTargetAtCurrentPdf = PathTraceCleanRtxdiDiMaterialEvaluateLightSampleTargetPdf(mappedPreviousSample, surface);
                if (result.previousTargetAtCurrentPdf > 1.0e-8)
                {
                    result.flags |= CLEAN_TEMPORAL_DIAG_PREVIOUS_TARGET_AT_CURRENT;
                }
            }
        }
    }

    RTXDI_RuntimeParameters runtimeParams = (RTXDI_RuntimeParameters)0;
    runtimeParams.activeCheckerboardField = 0u;
    runtimeParams.frameIndex = CleanRtxdiDiFrameIndex;

    RTXDI_DITemporalResamplingParameters temporalParams = (RTXDI_DITemporalResamplingParameters)0;
    temporalParams.maxHistoryLength = (uint)clamp(CleanRtxdiDiRestirPTSurfaceInfo.y, 0.0, 64.0);
    temporalParams.biasCorrectionMode = (uint)clamp(
        CleanRtxdiDiRestirPTSurfaceInfo.w,
        0.0,
        (float)RTXDI_BIAS_CORRECTION_RAY_TRACED);
    temporalParams.depthThreshold = 0.10;
    temporalParams.normalThreshold = 0.35;
    temporalParams.enableVisibilityShortcut = 0u;
    temporalParams.enablePermutationSampling = 0u;
    temporalParams.uniformRandomNumber = PathTraceCleanRoomHash(CleanRtxdiDiFrameIndex ^ 0x711ad151u);
    temporalParams.permutationSamplingThreshold = 0.0;
    temporalParams.fireflyClampRatio = (float)CleanRtxdiDiTemporalFireflyClamp;

    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSamplerForPass(pixel, CleanRtxdiDiFrameIndex, 0x52525805u, 0u);
    PathTraceCleanRtxdiDiApplyBlueNoiseToggle(rng);
    int2 temporalSamplePixel = int2(-1, -1);
    RAB_LightSample selectedLightSample = RAB_EmptyLightSample();
    result.flags |= CLEAN_TEMPORAL_DIAG_SDK_CALLED;
    const RTXDI_DIReservoir temporalReservoir = RTXDI_DITemporalResampling(
        pixel,
        surface,
        currentReservoir,
        rng,
        runtimeParams,
        PathTraceCleanRoomReservoirParams(dimensions),
        screenSpaceMotion,
        0u,
        temporalParams,
        temporalSamplePixel,
        selectedLightSample);

    result.temporalSamplePixel = temporalSamplePixel;
    if (temporalSamplePixel.x >= 0 && temporalSamplePixel.y >= 0 && (uint)temporalSamplePixel.x < dimensions.x && (uint)temporalSamplePixel.y < dimensions.y)
    {
        result.flags |= CLEAN_TEMPORAL_DIAG_TEMPORAL_SAMPLE_PIXEL_VALID;
    }
    if (RTXDI_IsValidDIReservoir(temporalReservoir))
    {
        result.flags |= CLEAN_TEMPORAL_DIAG_TEMPORAL_RESERVOIR_VALID;
        result.reservoir = temporalReservoir;
        result.temporalTargetPdf = temporalReservoir.targetPdf;
        result.temporalInvPdf = RTXDI_GetDIReservoirInvPdf(temporalReservoir);
        if (RAB_IsReplayableLightSample(selectedLightSample))
        {
            result.flags |= CLEAN_TEMPORAL_DIAG_SDK_SELECTED_PREVIOUS_SAMPLE;
        }
        if (RTXDI_GetDIReservoirLightIndex(temporalReservoir) != RTXDI_GetDIReservoirLightIndex(currentReservoir) ||
            any(abs(RTXDI_GetDIReservoirSampleUV(temporalReservoir) - RTXDI_GetDIReservoirSampleUV(currentReservoir)) > 1.0e-6))
        {
            result.flags |= CLEAN_TEMPORAL_DIAG_TEMPORAL_OUTPUT_CHANGED;
        }
        if (temporalReservoir.M > currentReservoir.M)
        {
            result.flags |= CLEAN_TEMPORAL_DIAG_SDK_REUSED_PREVIOUS;
        }
    }
    return result;
}

#if defined(CLEAN_RTXDI_DI_TEMPORAL_ENTRY)
bool PathTraceCleanRoomTemporalDiagnosticsNeeded(uint view)
{
    if (CleanRtxdiDiTemporalAudit != 0u)
    {
        return true;
    }

    return view != 12u && view != 16u;
}

RTXDI_DIReservoir PathTraceCleanRoomRunTemporalProducerFast(uint2 pixel, uint2 dimensions, RTXDI_DIReservoir currentReservoir, PathTracePrimarySurfaceRecord currentSurface)
{
    if (currentReservoir.M <= 0.0 || !PathTraceCleanRoomTemporalReuseEnabled())
    {
        return currentReservoir;
    }
    if (PathTraceCleanRoomSurfaceIsTransmissionPsrResolved(currentSurface))
    {
        return currentReservoir;
    }

    const bool rigidEmissiveTemporalBypass = PathTraceCleanRoomTemporalRigidEmissiveBypassEnabled();
    if (rigidEmissiveTemporalBypass &&
        PathTraceCleanRoomCurrentReservoirSelectsTemporalUnsafeEmissive(currentReservoir))
    {
        return currentReservoir;
    }

    const RAB_Surface surface = PathTraceCleanRoomSurfaceForView(currentSurface);
    if (!RAB_IsSurfaceValid(surface))
    {
        return currentReservoir;
    }

    float3 screenSpaceMotion;
    bool cameraReprojected = false;
    if (!PathTraceCleanRoomLoadMotion(pixel, currentSurface, dimensions, screenSpaceMotion, cameraReprojected))
    {
        return currentReservoir;
    }

    RTXDI_RuntimeParameters runtimeParams = (RTXDI_RuntimeParameters)0;
    runtimeParams.activeCheckerboardField = 0u;
    runtimeParams.frameIndex = CleanRtxdiDiFrameIndex;

    RTXDI_DITemporalResamplingParameters temporalParams = (RTXDI_DITemporalResamplingParameters)0;
    temporalParams.maxHistoryLength = (uint)clamp(CleanRtxdiDiRestirPTSurfaceInfo.y, 0.0, 64.0);
    temporalParams.biasCorrectionMode = (uint)clamp(
        CleanRtxdiDiRestirPTSurfaceInfo.w,
        0.0,
        (float)RTXDI_BIAS_CORRECTION_RAY_TRACED);
    temporalParams.depthThreshold = 0.10;
    temporalParams.normalThreshold = 0.35;
    temporalParams.enableVisibilityShortcut = 0u;
    temporalParams.enablePermutationSampling = 0u;
    temporalParams.uniformRandomNumber = PathTraceCleanRoomHash(CleanRtxdiDiFrameIndex ^ 0x711ad151u);
    temporalParams.permutationSamplingThreshold = 0.0;
    temporalParams.fireflyClampRatio = (float)CleanRtxdiDiTemporalFireflyClamp;

    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSamplerForPass(pixel, CleanRtxdiDiFrameIndex, 0x52525805u, 0u);
    PathTraceCleanRtxdiDiApplyBlueNoiseToggle(rng);
    int2 temporalSamplePixel = int2(-1, -1);
    RAB_LightSample selectedLightSample = RAB_EmptyLightSample();
    return RTXDI_DITemporalResampling(
        pixel,
        surface,
        currentReservoir,
        rng,
        runtimeParams,
        PathTraceCleanRoomReservoirParams(dimensions),
        screenSpaceMotion,
        0u,
        temporalParams,
        temporalSamplePixel,
        selectedLightSample);
}
#endif

float3 PathTraceCleanRoomTemporalGateColor(uint flags, uint flag)
{
    return (flags & flag) == flag ? float3(0.0, 0.90, 0.20) : float3(0.95, 0.04, 0.10);
}

float3 PathTraceCleanRoomRluAnalyticReplayGateColor(
    bool selectedPresent,
    bool selectedAnalytic,
    bool sourceValid,
    bool lightValid,
    bool sampleValid,
    bool mapped,
    bool mappedAnalytic,
    bool mappedSourceValid,
    bool mappedLightValid,
    bool mappedSampleValid,
    float targetPdf)
{
    if (!selectedPresent)
    {
        return float3(0.95, 0.04, 0.10);
    }
    if (!selectedAnalytic)
    {
        return float3(0.08, 0.12, 0.18);
    }
    if (!sourceValid)
    {
        return float3(1.0, 0.82, 0.0);
    }
    if (!lightValid)
    {
        return float3(1.0, 0.35, 0.0);
    }
    if (!sampleValid)
    {
        return float3(0.0, 0.18, 1.0);
    }
    if (!mapped)
    {
        return float3(0.95, 0.04, 0.10);
    }
    if (!mappedAnalytic)
    {
        return float3(1.0, 0.0, 1.0);
    }
    if (!mappedSourceValid)
    {
        return float3(0.65, 0.0, 1.0);
    }
    if (!mappedLightValid)
    {
        return float3(0.0, 0.95, 1.0);
    }
    if (!mappedSampleValid)
    {
        return float3(0.0, 0.45, 0.95);
    }
    if (targetPdf <= 1.0e-8 || !(targetPdf == targetPdf))
    {
        return float3(0.95, 0.95, 0.95);
    }

    return float3(PathTraceCleanRoomLog01(targetPdf, 6.0), 0.90, 0.18);
}

float3 PathTraceCleanRoomRluAnalyticPayloadFailureColor(PathTraceUnifiedLightRecord record)
{
    if (record.type != PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
    {
        return float3(1.0, 0.0, 1.0);
    }
    if (record.sourceIndex == PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX)
    {
        return float3(1.0, 0.82, 0.0);
    }
    if (!all(record.positionAndRadius.xyz == record.positionAndRadius.xyz) ||
        !all(abs(record.positionAndRadius.xyz) < float3(3.402823e+38, 3.402823e+38, 3.402823e+38)))
    {
        return float3(0.75, 0.0, 1.0);
    }
    if (record.positionAndRadius.w <= 0.0 ||
        record.positionAndRadius.w >= 3.402823e+38 ||
        !(record.positionAndRadius.w == record.positionAndRadius.w))
    {
        return float3(0.0, 0.95, 1.0);
    }
    if (record.uvOrDoomParams.x <= 0.0 ||
        record.uvOrDoomParams.x >= 3.402823e+38 ||
        !(record.uvOrDoomParams.x == record.uvOrDoomParams.x))
    {
        return float3(0.0, 0.18, 1.0);
    }
    if (record.normalAndArea.w <= 1.0e-6 ||
        record.normalAndArea.w >= 3.402823e+38 ||
        !(record.normalAndArea.w == record.normalAndArea.w))
    {
        return float3(1.0, 0.0, 0.55);
    }

    const float3 radiance = max(record.radianceAndLuminance.rgb, float3(0.0, 0.0, 0.0));
    const float luminance = dot(radiance, float3(0.2126, 0.7152, 0.0722));
    if (!all(radiance == radiance) ||
        !all(abs(radiance) < float3(3.402823e+38, 3.402823e+38, 3.402823e+38)) ||
        luminance <= 0.0)
    {
        return float3(1.0, 1.0, 1.0);
    }
    if (record.sourceWeight <= 0.0 ||
        record.sourceWeight >= 3.402823e+38 ||
        !(record.sourceWeight == record.sourceWeight))
    {
        return float3(1.0, 0.35, 0.0);
    }

    return float3(0.0, 0.90, 0.20);
}

float3 PathTraceCleanRoomRluAnalyticDirectCurrentProbeColor()
{
    const uint currentIndex = CleanRtxdiDiRluDoomAnalyticRangeOffset;
    if (CleanRtxdiDiRluDoomAnalyticRangeCount == 0u ||
        currentIndex >= CleanRtxdiDiRluCurrentLightCount)
    {
        return float3(0.95, 0.04, 0.10);
    }

    return PathTraceCleanRoomRluAnalyticPayloadFailureColor(CleanRtxdiDiRluCurrentLights[currentIndex]);
}

float3 PathTraceCleanRoomRluAnalyticDirectPreviousProbeColor()
{
    const uint currentIndex = CleanRtxdiDiRluDoomAnalyticRangeOffset;
    if (CleanRtxdiDiRluDoomAnalyticRangeCount == 0u ||
        currentIndex >= CleanRtxdiDiRluCurrentLightCount ||
        currentIndex >= CleanRtxdiDiRluCurrentToPreviousCount)
    {
        return float3(0.95, 0.04, 0.10);
    }

    const uint previousIndex = CleanRtxdiDiRluCurrentToPrevious[currentIndex];
    if (previousIndex >= CleanRtxdiDiRluPreviousLightCount)
    {
        return float3(1.0, 0.0, 1.0);
    }

    return PathTraceCleanRoomRluAnalyticPayloadFailureColor(CleanRtxdiDiRluPreviousLights[previousIndex]);
}

float3 PathTraceCleanRoomRluReplayNodeColor(
    bool selectedPresent,
    uint lightIndex,
    bool previousFrame,
    RAB_Surface surface,
    float2 sampleUv)
{
    if (!selectedPresent)
    {
        return float3(0.95, 0.04, 0.10);
    }

    const uint lightCount = previousFrame ? CleanRtxdiDiRluPreviousLightCount : CleanRtxdiDiRluCurrentLightCount;
    if (lightIndex >= lightCount)
    {
        return float3(1.0, 1.0, 1.0);
    }

    PathTraceUnifiedLightRecord record = (PathTraceUnifiedLightRecord)0;
    if (previousFrame)
    {
        record = CleanRtxdiDiRluPreviousLights[lightIndex];
    }
    else
    {
        record = CleanRtxdiDiRluCurrentLights[lightIndex];
    }
    if (record.type != PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC &&
        record.type != PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
    {
        return float3(1.0, 0.0, 1.0);
    }

    const uint sourceCount =
        record.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE
            ? (previousFrame ? CleanRtxdiDiPreviousEmissiveTriangleCount : CleanRtxdiDiCurrentEmissiveTriangleCount)
            : (previousFrame ? CleanRtxdiDiDoomAnalyticFullPreviousCount : CleanRtxdiDiDoomAnalyticFullCurrentCount);
    if (record.sourceIndex >= sourceCount)
    {
        return float3(1.0, 0.82, 0.0);
    }

    const RAB_LightInfo lightInfo = RAB_LoadLightInfo(lightIndex, previousFrame);
    if (!RAB_IsLightInfoValid(lightInfo))
    {
        return float3(1.0, 0.35, 0.0);
    }

    const RAB_LightSample sample = RAB_SamplePolymorphicLight(lightInfo, surface, sampleUv);
    if (!RAB_IsReplayableLightSample(sample))
    {
        return float3(0.0, 0.95, 1.0);
    }

    const float targetPdf = PathTraceCleanRtxdiDiMaterialEvaluateLightSampleTargetPdf(sample, surface);
    if (targetPdf <= 1.0e-8 || !(targetPdf == targetPdf))
    {
        return float3(0.95, 0.95, 0.95);
    }

    return float3(PathTraceCleanRoomLog01(targetPdf, 6.0), 0.90, 0.18);
}

float3 PathTraceCleanRoomRluSelectedReplaySplitDiagnosticColor(
    PathTraceCleanRtxdiDiInitialResult initial,
    PathTraceCleanRtxdiDiTemporalResult temporal,
    uint2 pixel,
    uint2 dimensions)
{
    if (!PathTraceCleanRoomMixedRluDomainEnabled())
    {
        return PathTraceCleanRoomTemporalGateColor(temporal.flags, CLEAN_TEMPORAL_DIAG_PREVIOUS_LIGHT_MAPPED);
    }

    const RAB_Surface surface = PathTraceCleanRoomSurfaceForView(initial.surface);
    if (!RAB_IsSurfaceValid(surface))
    {
        return float3(0.45, 0.0, 0.65);
    }

    const uint quarterWidth = max(dimensions.x >> 2u, 1u);
    if (pixel.x < quarterWidth)
    {
        const bool selectedPresent = RTXDI_IsValidDIReservoir(initial.reservoir);
        const uint currentIndex = selectedPresent ? RTXDI_GetDIReservoirLightIndex(initial.reservoir) : 0xffffffffu;
        return PathTraceCleanRoomRluReplayNodeColor(
            selectedPresent,
            currentIndex,
            false,
            surface,
            RTXDI_GetDIReservoirSampleUV(initial.reservoir));
    }
    if (pixel.x < quarterWidth * 2u)
    {
        const bool selectedPresent = RTXDI_IsValidDIReservoir(initial.reservoir);
        const uint currentIndex = selectedPresent ? RTXDI_GetDIReservoirLightIndex(initial.reservoir) : 0xffffffffu;
        const bool currentIndexValid = currentIndex < CleanRtxdiDiRluCurrentLightCount;
        PathTraceUnifiedLightRecord currentRecord = (PathTraceUnifiedLightRecord)0;
        if (currentIndexValid)
        {
            currentRecord = CleanRtxdiDiRluCurrentLights[currentIndex];
        }
        if (!selectedPresent || !currentIndexValid)
        {
            return float3(0.95, 0.04, 0.10);
        }
        if (currentRecord.type != PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
        {
            return currentRecord.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE
                ? float3(0.08, 0.22, 0.95)
                : float3(1.0, 0.0, 1.0);
        }

        const int previousIndexSigned = RAB_TranslateLightIndex(currentIndex, true);
        if (previousIndexSigned < 0)
        {
            return float3(0.95, 0.04, 0.10);
        }
        return PathTraceCleanRoomRluReplayNodeColor(
            true,
            (uint)previousIndexSigned,
            true,
            surface,
            RTXDI_GetDIReservoirSampleUV(initial.reservoir));
    }

    const bool previousPixelInBounds =
        temporal.previousPixel.x >= 0 &&
        temporal.previousPixel.y >= 0 &&
        (uint)temporal.previousPixel.x < dimensions.x &&
        (uint)temporal.previousPixel.y < dimensions.y;
    RTXDI_DIReservoir previousReservoir = RTXDI_EmptyDIReservoir();
    if (previousPixelInBounds)
    {
        previousReservoir = RTXDI_LoadDIReservoir(PathTraceCleanRoomReservoirParams(dimensions), (uint2)temporal.previousPixel, 0u);
    }

    if (pixel.x < quarterWidth * 3u)
    {
        const bool selectedPresent = previousPixelInBounds && RTXDI_IsValidDIReservoir(previousReservoir);
        const uint previousIndex = selectedPresent ? RTXDI_GetDIReservoirLightIndex(previousReservoir) : 0xffffffffu;
        return PathTraceCleanRoomRluReplayNodeColor(
            selectedPresent,
            previousIndex,
            true,
            surface,
            RTXDI_GetDIReservoirSampleUV(previousReservoir));
    }

    const bool selectedPresent = previousPixelInBounds && RTXDI_IsValidDIReservoir(previousReservoir);
    const uint previousIndex = selectedPresent ? RTXDI_GetDIReservoirLightIndex(previousReservoir) : 0xffffffffu;
    const bool previousIndexValid = previousIndex < CleanRtxdiDiRluPreviousLightCount;
    PathTraceUnifiedLightRecord previousRecord = (PathTraceUnifiedLightRecord)0;
    if (previousIndexValid)
    {
        previousRecord = CleanRtxdiDiRluPreviousLights[previousIndex];
    }
    if (!selectedPresent || !previousIndexValid)
    {
        return float3(0.95, 0.04, 0.10);
    }
    if (previousRecord.type != PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
    {
        return previousRecord.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE
            ? float3(0.08, 0.22, 0.95)
            : float3(1.0, 0.0, 1.0);
    }

    const int currentIndexSigned = RAB_TranslateLightIndex(previousIndex, false);
    if (currentIndexSigned < 0)
    {
        return float3(0.95, 0.04, 0.10);
    }
    return PathTraceCleanRoomRluReplayNodeColor(
        true,
        (uint)currentIndexSigned,
        false,
        surface,
        RTXDI_GetDIReservoirSampleUV(previousReservoir));
}

float3 PathTraceCleanRoomRluAnalyticReplayDiagnosticColor(
    PathTraceCleanRtxdiDiInitialResult initial,
    PathTraceCleanRtxdiDiTemporalResult temporal,
    uint2 pixel,
    uint2 dimensions)
{
    if (!PathTraceCleanRoomMixedRluDomainEnabled())
    {
        return PathTraceCleanRoomTemporalGateColor(temporal.flags, CLEAN_TEMPORAL_DIAG_PREVIOUS_LIGHT_MAPPED);
    }

    const uint quarterWidth = max(dimensions.x >> 2u, 1u);
    if (pixel.x < quarterWidth)
    {
        return PathTraceCleanRoomRluAnalyticDirectCurrentProbeColor();
    }
    if (pixel.x < quarterWidth * 2u)
    {
        return PathTraceCleanRoomRluAnalyticDirectPreviousProbeColor();
    }

    const RAB_Surface surface = PathTraceCleanRoomSurfaceForView(initial.surface);
    if (!RAB_IsSurfaceValid(surface))
    {
        return float3(0.45, 0.0, 0.65);
    }

    if (pixel.x < quarterWidth * 3u)
    {
        const bool selectedPresent = RTXDI_IsValidDIReservoir(initial.reservoir);
        const uint currentIndex = selectedPresent ? RTXDI_GetDIReservoirLightIndex(initial.reservoir) : 0xffffffffu;
        const bool currentIndexValid = currentIndex < CleanRtxdiDiRluCurrentLightCount;
        PathTraceUnifiedLightRecord currentRecord = (PathTraceUnifiedLightRecord)0;
        if (currentIndexValid)
        {
            currentRecord = CleanRtxdiDiRluCurrentLights[currentIndex];
        }
        const bool selectedAnalytic = currentIndexValid && currentRecord.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC;
        const bool sourceValid = selectedAnalytic && currentRecord.sourceIndex < CleanRtxdiDiDoomAnalyticFullCurrentCount;
        RAB_LightInfo currentInfo = RAB_EmptyLightInfo();
        if (currentIndexValid)
        {
            currentInfo = RAB_LoadLightInfo(currentIndex, false);
        }
    const RAB_LightSample currentSample = RAB_SamplePolymorphicLight(currentInfo, surface, RTXDI_GetDIReservoirSampleUV(initial.reservoir));

        const int previousIndexSigned = selectedAnalytic ? RAB_TranslateLightIndex(currentIndex, true) : -1;
        const bool mapped = previousIndexSigned >= 0 && (uint)previousIndexSigned < CleanRtxdiDiRluPreviousLightCount;
        const uint previousIndex = mapped ? (uint)previousIndexSigned : 0xffffffffu;
        PathTraceUnifiedLightRecord previousRecord = (PathTraceUnifiedLightRecord)0;
        if (mapped)
        {
            previousRecord = CleanRtxdiDiRluPreviousLights[previousIndex];
        }
        const bool mappedAnalytic = mapped && previousRecord.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC;
        const bool mappedSourceValid = mappedAnalytic && previousRecord.sourceIndex < CleanRtxdiDiDoomAnalyticFullPreviousCount;
        RAB_LightInfo previousInfo = RAB_EmptyLightInfo();
        if (mapped)
        {
            previousInfo = RAB_LoadLightInfo(previousIndex, true);
        }
    const RAB_LightSample previousSampleAtCurrent = RAB_SamplePolymorphicLight(previousInfo, surface, RTXDI_GetDIReservoirSampleUV(initial.reservoir));
        const float previousTargetAtCurrent = PathTraceCleanRtxdiDiMaterialEvaluateLightSampleTargetPdf(previousSampleAtCurrent, surface);
        if (mappedAnalytic && !RAB_IsLightInfoValid(previousInfo))
        {
            return PathTraceCleanRoomRluAnalyticPayloadFailureColor(previousRecord);
        }

        return PathTraceCleanRoomRluAnalyticReplayGateColor(
            selectedPresent,
            selectedAnalytic,
            sourceValid,
            RAB_IsLightInfoValid(currentInfo),
            RAB_IsReplayableLightSample(currentSample),
            mapped,
            mappedAnalytic,
            mappedSourceValid,
            RAB_IsLightInfoValid(previousInfo),
            RAB_IsReplayableLightSample(previousSampleAtCurrent),
            previousTargetAtCurrent);
    }

    const bool previousPixelInBounds =
        temporal.previousPixel.x >= 0 &&
        temporal.previousPixel.y >= 0 &&
        (uint)temporal.previousPixel.x < dimensions.x &&
        (uint)temporal.previousPixel.y < dimensions.y;
    RTXDI_DIReservoir previousReservoir = RTXDI_EmptyDIReservoir();
    if (previousPixelInBounds)
    {
        previousReservoir = RTXDI_LoadDIReservoir(PathTraceCleanRoomReservoirParams(dimensions), (uint2)temporal.previousPixel, 0u);
    }
    const bool selectedPresent = previousPixelInBounds && RTXDI_IsValidDIReservoir(previousReservoir);
    const uint previousIndex = selectedPresent ? RTXDI_GetDIReservoirLightIndex(previousReservoir) : 0xffffffffu;
    const bool previousIndexValid = previousIndex < CleanRtxdiDiRluPreviousLightCount;
    PathTraceUnifiedLightRecord previousRecord = (PathTraceUnifiedLightRecord)0;
    if (previousIndexValid)
    {
        previousRecord = CleanRtxdiDiRluPreviousLights[previousIndex];
    }
    const bool selectedAnalytic = previousIndexValid && previousRecord.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC;
    const bool sourceValid = selectedAnalytic && previousRecord.sourceIndex < CleanRtxdiDiDoomAnalyticFullPreviousCount;
    RAB_LightInfo previousInfo = RAB_EmptyLightInfo();
    if (previousIndexValid)
    {
        previousInfo = RAB_LoadLightInfo(previousIndex, true);
    }
    const RAB_LightSample previousSample = RAB_SamplePolymorphicLight(previousInfo, surface, RTXDI_GetDIReservoirSampleUV(previousReservoir));
    if (selectedAnalytic && !RAB_IsLightInfoValid(previousInfo))
    {
        return PathTraceCleanRoomRluAnalyticPayloadFailureColor(previousRecord);
    }

    const int currentIndexSigned = selectedAnalytic ? RAB_TranslateLightIndex(previousIndex, false) : -1;
    const bool mapped = currentIndexSigned >= 0 && (uint)currentIndexSigned < CleanRtxdiDiRluCurrentLightCount;
    const uint currentIndex = mapped ? (uint)currentIndexSigned : 0xffffffffu;
    PathTraceUnifiedLightRecord currentRecord = (PathTraceUnifiedLightRecord)0;
    if (mapped)
    {
        currentRecord = CleanRtxdiDiRluCurrentLights[currentIndex];
    }
    const bool mappedAnalytic = mapped && currentRecord.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC;
    const bool mappedSourceValid = mappedAnalytic && currentRecord.sourceIndex < CleanRtxdiDiDoomAnalyticFullCurrentCount;
    RAB_LightInfo currentInfo = RAB_EmptyLightInfo();
    if (mapped)
    {
        currentInfo = RAB_LoadLightInfo(currentIndex, false);
    }
    const RAB_LightSample currentSampleAtCurrent = RAB_SamplePolymorphicLight(currentInfo, surface, RTXDI_GetDIReservoirSampleUV(previousReservoir));
    const float currentTargetAtCurrent = PathTraceCleanRtxdiDiMaterialEvaluateLightSampleTargetPdf(currentSampleAtCurrent, surface);

    return PathTraceCleanRoomRluAnalyticReplayGateColor(
        selectedPresent,
        selectedAnalytic,
        sourceValid,
        RAB_IsLightInfoValid(previousInfo),
        RAB_IsReplayableLightSample(previousSample),
        mapped,
        mappedAnalytic,
        mappedSourceValid,
        RAB_IsLightInfoValid(currentInfo),
        RAB_IsReplayableLightSample(currentSampleAtCurrent),
        currentTargetAtCurrent);
}

float3 PathTraceCleanRoomTemporalDiagnosticColor(PathTraceCleanRtxdiDiInitialResult initial, PathTraceCleanRtxdiDiTemporalResult temporal, uint2 pixel, uint2 dimensions)
{
    if (initial.status != CLEAN_INITIAL_STATUS_VALID && initial.status != CLEAN_INITIAL_STATUS_ZERO_TARGET_PDF)
    {
        return PathTraceCleanRoomStatusColor(initial.status);
    }

    const uint bandCount = 16u;
    const bool forcedBand = CleanRtxdiDiView8Band < bandCount;
    const uint band = forcedBand ? CleanRtxdiDiView8Band : min((pixel.y * bandCount) / max(dimensions.y, 1u), bandCount - 1u);
    if (band == 8u)
    {
        return pixel.x < (dimensions.x >> 1u)
            ? PathTraceCleanRoomSelectedSampleFailureColor(initial.surface, initial.reservoir, initial.status)
            : PathTraceCleanRoomSelectedSampleFailureColor(initial.surface, temporal.reservoir, initial.status);
    }
    if (band == 15u)
    {
        return pixel.x < (dimensions.x >> 1u)
            ? PathTraceCleanRoomSelectedLightTypeColor(initial.surface, initial.reservoir, initial.status)
            : PathTraceCleanRoomSelectedLightTypeColor(initial.surface, temporal.reservoir, initial.status);
    }
    if (band == 0u)
    {
        float3 gate = PathTraceCleanRoomTemporalGateColor(temporal.flags, CLEAN_TEMPORAL_DIAG_ENABLED | CLEAN_TEMPORAL_DIAG_PREVIOUS_FRAME_VALID);
        gate.b = PathTraceCleanRoomLog01((float)CleanRtxdiDiHistoryResetCount, 8.0);
        if (PathTraceCleanReferenceRabEnabled())
        {
            gate.r = 0.85;
        }
        return gate;
    }
    if (band == 1u)
    {
        return PathTraceCleanRoomTemporalGateColor(temporal.flags, CLEAN_TEMPORAL_DIAG_CURRENT_CANDIDATE | CLEAN_TEMPORAL_DIAG_CURRENT_SURFACE_VALID);
    }
    if (band == 2u)
    {
        return PathTraceCleanRoomTemporalGateColor(temporal.flags, CLEAN_TEMPORAL_DIAG_MOTION_VALID);
    }
    if (band == 3u)
    {
        return PathTraceCleanRoomTemporalGateColor(temporal.flags, CLEAN_TEMPORAL_DIAG_PREVIOUS_SURFACE_VALID);
    }
    if (band == 4u)
    {
        float3 gate = PathTraceCleanRoomTemporalGateColor(temporal.flags, CLEAN_TEMPORAL_DIAG_PREVIOUS_RESERVOIR_VALID);
        gate.b = PathTraceCleanRoomLog01(temporal.previousM, 5.0);
        return gate;
    }
    if (band == 5u)
    {
        if (PathTraceCleanRoomMixedRluDomainEnabled())
        {
            return PathTraceCleanRoomRluAnalyticReplayDiagnosticColor(initial, temporal, pixel, dimensions);
        }

        if (pixel.x < (dimensions.x >> 1u))
        {
            return PathTraceCleanRoomTemporalGateColor(temporal.flags, CLEAN_TEMPORAL_DIAG_PREVIOUS_LIGHT_MAPPED);
        }

        float3 gate = PathTraceCleanRoomTemporalGateColor(temporal.flags, CLEAN_TEMPORAL_DIAG_PREVIOUS_TARGET_AT_CURRENT);
        gate.b = PathTraceCleanRoomLog01(temporal.previousTargetAtCurrentPdf, 6.0);
        return gate;
    }
    if (band == 6u)
    {
        return pixel.x < (dimensions.x >> 1u)
            ? PathTraceCleanRoomTargetInvPdfColor(initial.targetPdf, RTXDI_GetDIReservoirInvPdf(initial.reservoir))
            : PathTraceCleanRoomTargetInvPdfColor(temporal.temporalTargetPdf, temporal.temporalInvPdf);
    }
    if (band == 9u)
    {
        return PathTraceCleanRoomRluSelectedReplaySplitDiagnosticColor(initial, temporal, pixel, dimensions);
    }
    if (band == 11u)
    {
        return PathTraceCleanRoomTemporalGateColor(initial.previousBestSourceValid != 0u ? CLEAN_TEMPORAL_DIAG_CURRENT_VALID : 0u, CLEAN_TEMPORAL_DIAG_CURRENT_VALID);
    }
    if (band == 12u)
    {
        return PathTraceCleanRoomTemporalGateColor(initial.previousBestTranslationValid != 0u ? CLEAN_TEMPORAL_DIAG_CURRENT_VALID : 0u, CLEAN_TEMPORAL_DIAG_CURRENT_VALID);
    }
    if (band == 13u)
    {
        return PathTraceCleanRoomTemporalGateColor(initial.previousBestCandidateValid != 0u ? CLEAN_TEMPORAL_DIAG_CURRENT_VALID : 0u, CLEAN_TEMPORAL_DIAG_CURRENT_VALID);
    }
    if (band == 14u)
    {
        return PathTraceCleanRoomTemporalGateColor(initial.previousBestSelected != 0u ? CLEAN_TEMPORAL_DIAG_CURRENT_VALID : 0u, CLEAN_TEMPORAL_DIAG_CURRENT_VALID);
    }

    if (pixel.x < (dimensions.x >> 1u))
    {
        const float currentLum = RAB_Luminance(PathTraceCleanRoomFlatDiffuseResolveReservoir(initial.surface, initial.reservoir));
        return float3(PathTraceCleanRoomLog01(currentLum, 12.0), PathTraceCleanRoomLog01(initial.reservoir.M, 5.0), 0.05);
    }

    // SDK's out selectedLightSample is populated by RTXDI when the previous reservoir sample wins the temporal combine.
    float3 gate = PathTraceCleanRoomTemporalGateColor(temporal.flags, CLEAN_TEMPORAL_DIAG_SDK_SELECTED_PREVIOUS_SAMPLE);
    gate.b = PathTraceCleanRoomLog01(max(temporal.reservoir.M - initial.reservoir.M, 0.0), 5.0);
    return gate;
}

float3 PathTraceCleanRoomTemporalAuditColor(PathTraceCleanRtxdiDiInitialResult initial, PathTraceCleanRtxdiDiTemporalResult temporal)
{
    return float3(
        (float)temporal.flags / CLEAN_TEMPORAL_AUDIT_FLAG_SCALE,
        saturate(temporal.previousM / 64.0),
        saturate(temporal.reservoir.M / 64.0));
}

float3 PathTraceCleanRoomPreviousBestDiagnosticColor(PathTraceCleanRtxdiDiInitialResult result)
{
    if ((CleanRtxdiDiFlags & CLEAN_FLAG_PREVIOUS_BEST_APPROXIMATION) == 0u)
    {
        return float3(0.02, 0.04, 0.20);
    }
    if (CleanRtxdiDiView8Band == 11u)
    {
        return result.previousBestSourceValid != 0u ? float3(0.0, 0.90, 0.20) : float3(0.95, 0.04, 0.10);
    }
    if (CleanRtxdiDiView8Band == 12u)
    {
        return result.previousBestTranslationValid != 0u ? float3(0.0, 0.90, 0.20) : float3(0.95, 0.04, 0.10);
    }
    if (CleanRtxdiDiView8Band == 13u)
    {
        return result.previousBestCandidateValid != 0u ? float3(0.0, 0.90, 0.20) : float3(0.95, 0.04, 0.10);
    }
    if (CleanRtxdiDiView8Band == 14u)
    {
        return result.previousBestSelected != 0u ? float3(0.0, 0.90, 0.20) : float3(0.95, 0.04, 0.10);
    }
    return float3(0.02, 0.04, 0.20);
}

#if defined(CLEAN_RTXDI_DI_INITIAL_ENTRY) || defined(CLEAN_RTXDI_DI_TEMPORAL_ENTRY)
PathTraceCleanRtxdiDiInitialResult PathTraceCleanRoomStoreInitialReservoir(uint2 pixel, uint2 dimensions)
{
    const uint reservoirIndex = PathTraceCleanRoomReservoirIndex(pixel, dimensions);
    PathTraceCleanRtxdiDiInitialResult result = PathTraceCleanRoomRunSelectedInitialProducer(pixel, dimensions);
    if (reservoirIndex < CleanRtxdiDiReservoirCount)
    {
        if (!PathTraceCleanRoomExternalPdfNeeCurrentEnabled())
        {
            CleanRtxdiDiCurrentReservoirs[reservoirIndex] = RTXDI_PackDIReservoir(result.reservoir);
        }
        CleanRtxdiDiTemporalReservoirs[reservoirIndex] = RTXDI_PackDIReservoir(result.reservoir);
    }
    return result;
}
#endif

#if defined(CLEAN_RTXDI_DI_TEMPORAL_ENTRY)
PathTraceCleanRtxdiDiInitialResult PathTraceCleanRoomLoadStoredInitialReservoir(uint2 pixel, uint2 dimensions)
{
    PathTraceCleanRtxdiDiInitialResult result = (PathTraceCleanRtxdiDiInitialResult)0;
    result.reservoir = RTXDI_EmptyDIReservoir();
    result.selectedLightIndex = 0xffffffffu;
    result.status = CLEAN_INITIAL_STATUS_VALID;

    if (!PathTraceCleanRoomLoadSurfaceRecord(pixel, dimensions, result.surface))
    {
        result.status = CLEAN_INITIAL_STATUS_INVALID_SURFACE;
        return result;
    }

    const RAB_Surface surface = PathTraceCleanRoomSurfaceForView(result.surface);
    if (!RAB_IsSurfaceValid(surface))
    {
        result.status = CLEAN_INITIAL_STATUS_INVALID_SURFACE;
        return result;
    }

    const uint reservoirIndex = PathTraceCleanRoomReservoirIndex(pixel, dimensions);
    if (reservoirIndex >= CleanRtxdiDiReservoirCount)
    {
        result.status = CLEAN_INITIAL_STATUS_EXTERNAL_CURRENT_EMPTY;
        return result;
    }

    result.reservoir = RTXDI_UnpackDIReservoir(CleanRtxdiDiCurrentReservoirs[reservoirIndex]);
    if (!RTXDI_IsValidDIReservoir(result.reservoir) || result.reservoir.M <= 0.0)
    {
        result.reservoir = RTXDI_EmptyDIReservoir();
        result.status = CLEAN_INITIAL_STATUS_ZERO_TARGET_PDF;
        return result;
    }

    result.selectedLightIndex = RTXDI_GetDIReservoirLightIndex(result.reservoir);
    result.sampleUv = RTXDI_GetDIReservoirSampleUV(result.reservoir);
    result.targetPdf = result.reservoir.targetPdf;
    result.invSourcePdf = RTXDI_GetDIReservoirInvPdf(result.reservoir);

    const RAB_LightInfo lightInfo = RAB_LoadLightInfo(result.selectedLightIndex, false);
    const RAB_LightSample selectedSample = RAB_SamplePolymorphicLight(lightInfo, surface, result.sampleUv);
    if (!RAB_IsLightInfoValid(lightInfo) || !RAB_IsReplayableLightSample(selectedSample))
    {
        result.reservoir = RTXDI_EmptyDIReservoir();
        result.status = CLEAN_INITIAL_STATUS_BAD_SAMPLE_PDF;
        return result;
    }

    result.samplePosition = selectedSample.position;
    result.sampleRadiance = selectedSample.radiance;
    result.solidAnglePdf = selectedSample.solidAnglePdf;
    result.visibility = PathTraceCleanRoomSelectedSampleVisibility(result.surface, result.reservoir, selectedSample);
    return result;
}

PathTraceCleanRtxdiDiInitialResult PathTraceCleanRoomLoadStoredInitialReservoirFast(uint2 pixel, uint2 dimensions)
{
    PathTraceCleanRtxdiDiInitialResult result = (PathTraceCleanRtxdiDiInitialResult)0;
    result.reservoir = RTXDI_EmptyDIReservoir();
    result.selectedLightIndex = 0xffffffffu;
    result.status = CLEAN_INITIAL_STATUS_VALID;

    if (!PathTraceCleanRoomLoadSurfaceRecord(pixel, dimensions, result.surface))
    {
        result.status = CLEAN_INITIAL_STATUS_INVALID_SURFACE;
        return result;
    }

    const uint reservoirIndex = PathTraceCleanRoomReservoirIndex(pixel, dimensions);
    if (reservoirIndex >= CleanRtxdiDiReservoirCount)
    {
        result.status = CLEAN_INITIAL_STATUS_EXTERNAL_CURRENT_EMPTY;
        return result;
    }

    result.reservoir = RTXDI_UnpackDIReservoir(CleanRtxdiDiCurrentReservoirs[reservoirIndex]);
    if (!RTXDI_IsValidDIReservoir(result.reservoir) || result.reservoir.M <= 0.0)
    {
        result.reservoir = RTXDI_EmptyDIReservoir();
        result.status = CLEAN_INITIAL_STATUS_ZERO_TARGET_PDF;
        return result;
    }

    result.selectedLightIndex = RTXDI_GetDIReservoirLightIndex(result.reservoir);
    result.sampleUv = RTXDI_GetDIReservoirSampleUV(result.reservoir);
    result.targetPdf = result.reservoir.targetPdf;
    result.invSourcePdf = RTXDI_GetDIReservoirInvPdf(result.reservoir);
    return result;
}
#endif

float3 PathTraceCleanRoomInitialReservoirOutput(uint2 pixel, uint2 dimensions, uint view)
{
    if (view == 8u && CleanRtxdiDiView8Band == 10u)
    {
        return PathTraceCleanRoomNeeCacheSecondaryCandidateFieldColorForPixel(pixel, dimensions);
    }

#if defined(CLEAN_RTXDI_DI_INITIAL_ENTRY) || defined(CLEAN_RTXDI_DI_TEMPORAL_ENTRY)
    PathTraceCleanRtxdiDiInitialResult result = PathTraceCleanRoomStoreInitialReservoir(pixel, dimensions);
#else
    const uint reservoirIndex = PathTraceCleanRoomReservoirIndex(pixel, dimensions);
    PathTraceCleanRtxdiDiInitialResult result = PathTraceCleanRoomRunSelectedInitialProducer(pixel, dimensions);
    if (reservoirIndex < CleanRtxdiDiReservoirCount)
    {
        if (!PathTraceCleanRoomExternalPdfNeeCurrentEnabled())
        {
            CleanRtxdiDiCurrentReservoirs[reservoirIndex] = RTXDI_PackDIReservoir(result.reservoir);
        }
        CleanRtxdiDiTemporalReservoirs[reservoirIndex] = RTXDI_PackDIReservoir(result.reservoir);
    }
#endif

    if (view == 4u)
    {
        return PathTraceCleanRoomFlatDiffuseResolve(result);
    }
    if (view == 7u)
    {
        return PathTraceCleanRoomReservoirIdentityColor(result, pixel);
    }
    if (view == 8u)
    {
        if (CleanRtxdiDiView8Band == 7u)
        {
            return PathTraceCleanRoomNeeCacheProviderDiagnosticColor(result);
        }
        if (CleanRtxdiDiView8Band == 8u)
        {
            return PathTraceCleanRoomSelectedSampleFailureColor(result.surface, result.reservoir, result.status);
        }
        if (CleanRtxdiDiView8Band >= 11u && CleanRtxdiDiView8Band <= 14u)
        {
            return PathTraceCleanRoomPreviousBestDiagnosticColor(result);
        }
        if (CleanRtxdiDiView8Band == 15u)
        {
            return PathTraceCleanRoomSelectedLightTypeColor(result.surface, result.reservoir, result.status);
        }
        return PathTraceCleanRoomReservoirWeightColor(result, pixel);
    }
    return PathTraceCleanRoomStatusColor(result.status);
}

#if defined(CLEAN_RTXDI_DI_TEMPORAL_ENTRY)
float3 PathTraceCleanRoomTemporalReservoirOutputForInitial(uint2 pixel, uint2 dimensions, uint view, PathTraceCleanRtxdiDiInitialResult initial)
{
    if (view == 8u && CleanRtxdiDiView8Band == 10u)
    {
        return PathTraceCleanRoomNeeCacheSecondaryCandidateFieldColorForPixel(pixel, dimensions);
    }

    const uint reservoirIndex = PathTraceCleanRoomReservoirIndex(pixel, dimensions);
    const bool temporalRequested = view == 5u || view == 6u || view == 8u || view == 9u || view == 10u || view == 11u || view == 12u || view == 16u;
    const bool temporalAllowed = temporalRequested && (CleanRtxdiDiTemporalFlags & CLEAN_TEMPORAL_FLAG_ENABLE) != 0u;
    RTXDI_DIReservoir temporalReservoir = initial.reservoir;
    PathTraceCleanRtxdiDiTemporalResult temporal = (PathTraceCleanRtxdiDiTemporalResult)0;
    temporal.reservoir = initial.reservoir;
    temporal.previousPixel = int2(-1, -1);
    temporal.temporalSamplePixel = int2(-1, -1);
    if (temporalAllowed)
    {
        if (PathTraceCleanRoomTemporalDiagnosticsNeeded(view))
        {
            temporal = PathTraceCleanRoomRunTemporalProducer(pixel, dimensions, initial.reservoir, initial.surface);
            temporalReservoir = temporal.reservoir;
        }
        else
        {
            temporalReservoir = PathTraceCleanRoomRunTemporalProducerFast(pixel, dimensions, initial.reservoir, initial.surface);
            temporal.reservoir = temporalReservoir;
        }
    }

    if (reservoirIndex < CleanRtxdiDiReservoirCount)
    {
        CleanRtxdiDiTemporalReservoirs[reservoirIndex] = RTXDI_PackDIReservoir(temporalReservoir);
    }

    if (CleanRtxdiDiTemporalAudit != 0u)
    {
        return PathTraceCleanRoomTemporalAuditColor(initial, temporal);
    }

    if (view == 6u)
    {
        const float3 currentColor = PathTraceCleanRoomFlatDiffuseResolve(initial);
        const bool temporalSdkCalled = !temporalAllowed || (temporal.flags & CLEAN_TEMPORAL_DIAG_SDK_CALLED) != 0u;
        const bool temporalAcceptedPrevious = !temporalAllowed || (temporal.flags & CLEAN_TEMPORAL_DIAG_SDK_REUSED_PREVIOUS) != 0u;
        const bool temporalRenderable = (initial.status == CLEAN_INITIAL_STATUS_VALID || initial.status == CLEAN_INITIAL_STATUS_ZERO_TARGET_PDF) &&
            RTXDI_IsValidDIReservoir(temporalReservoir);
        const float3 temporalColor = temporalSdkCalled && temporalAcceptedPrevious
            ? (temporalRenderable
                ? PathTraceCleanRoomFlatDiffuseResolveReservoir(initial.surface, temporalReservoir)
                : (initial.status == CLEAN_INITIAL_STATUS_ZERO_TARGET_PDF ? float3(0.0, 0.0, 0.0) : PathTraceCleanRoomStatusColor(initial.status)))
            : float3(0.95, 0.04, 0.10);
        return pixel.x < (dimensions.x >> 1u) ? currentColor : temporalColor;
    }

    if (view == 8u)
    {
        if (CleanRtxdiDiView8Band == 7u)
        {
            return PathTraceCleanRoomNeeCacheProviderDiagnosticColor(initial);
        }
        if (CleanRtxdiDiView8Band >= 11u && CleanRtxdiDiView8Band <= 14u)
        {
            return PathTraceCleanRoomPreviousBestDiagnosticColor(initial);
        }
        return PathTraceCleanRoomTemporalDiagnosticColor(initial, temporal, pixel, dimensions);
    }

    if (view == 9u || view == 10u || view == 11u || view == 12u)
    {
        if (!RTXDI_IsValidDIReservoir(temporalReservoir))
        {
            return float3(0.0, 0.0, 0.0);
        }
        return PathTraceCleanRoomFlatDiffuseResolveReservoir(initial.surface, temporalReservoir);
    }

    if (view == 5u)
    {
        if (temporalAllowed && (temporal.flags & CLEAN_TEMPORAL_DIAG_SDK_CALLED) == 0u)
        {
            return float3(0.95, 0.04, 0.10);
        }
        if (temporalAllowed && (temporal.flags & CLEAN_TEMPORAL_DIAG_SDK_REUSED_PREVIOUS) == 0u)
        {
            return float3(0.95, 0.04, 0.10);
        }
        if (initial.status != CLEAN_INITIAL_STATUS_VALID &&
            !(initial.status == CLEAN_INITIAL_STATUS_ZERO_TARGET_PDF && RTXDI_IsValidDIReservoir(temporalReservoir)))
        {
            return initial.status == CLEAN_INITIAL_STATUS_ZERO_TARGET_PDF ? float3(0.0, 0.0, 0.0) : PathTraceCleanRoomStatusColor(initial.status);
        }
        return PathTraceCleanRoomFlatDiffuseResolveReservoir(initial.surface, temporalReservoir);
    }

    const bool reusedPrevious = (temporal.flags & CLEAN_TEMPORAL_DIAG_SDK_REUSED_PREVIOUS) != 0u;
    const float3 base = reusedPrevious ? float3(0.0, 0.85, 0.18) : float3(0.20, 0.28, 0.95);
    return RTXDI_IsValidDIReservoir(temporalReservoir)
        ? lerp(base, float3(saturate(temporalReservoir.M / 16.0), 0.95, 0.25), 0.45)
        : float3(0.95, 0.05, 0.12);
}

float3 PathTraceCleanRoomTemporalReservoirOutputFromStoredCurrent(uint2 pixel, uint2 dimensions, uint view)
{
    PathTraceCleanRtxdiDiInitialResult initial = (PathTraceCleanRtxdiDiInitialResult)0;
    if (PathTraceCleanRoomTemporalDiagnosticsNeeded(view))
    {
        initial = PathTraceCleanRoomLoadStoredInitialReservoir(pixel, dimensions);
    }
    else
    {
        initial = PathTraceCleanRoomLoadStoredInitialReservoirFast(pixel, dimensions);
    }
    return PathTraceCleanRoomTemporalReservoirOutputForInitial(pixel, dimensions, view, initial);
}
#elif !defined(CLEAN_RTXDI_DI_INITIAL_ENTRY)
float3 PathTraceCleanRoomTemporalReservoirOutput(uint2 pixel, uint2 dimensions, uint view)
{
    if (view == 8u && CleanRtxdiDiView8Band == 10u)
    {
        return PathTraceCleanRoomNeeCacheSecondaryCandidateFieldColorForPixel(pixel, dimensions);
    }

    const uint reservoirIndex = PathTraceCleanRoomReservoirIndex(pixel, dimensions);
    PathTraceCleanRtxdiDiInitialResult initial = PathTraceCleanRoomRunSelectedInitialProducer(pixel, dimensions);
    if (reservoirIndex < CleanRtxdiDiReservoirCount)
    {
        if (!PathTraceCleanRoomExternalPdfNeeCurrentEnabled())
        {
            CleanRtxdiDiCurrentReservoirs[reservoirIndex] = RTXDI_PackDIReservoir(initial.reservoir);
        }
    }

    const bool temporalRequested = view == 5u || view == 6u || view == 8u || view == 9u || view == 10u || view == 11u || view == 12u || view == 16u;
    const bool temporalAllowed = temporalRequested && (CleanRtxdiDiTemporalFlags & CLEAN_TEMPORAL_FLAG_ENABLE) != 0u;
    RTXDI_DIReservoir temporalReservoir = initial.reservoir;
    PathTraceCleanRtxdiDiTemporalResult temporal = (PathTraceCleanRtxdiDiTemporalResult)0;
    temporal.reservoir = initial.reservoir;
    temporal.previousPixel = int2(-1, -1);
    temporal.temporalSamplePixel = int2(-1, -1);
    if (temporalAllowed)
    {
        temporal = PathTraceCleanRoomRunTemporalProducer(pixel, dimensions, initial.reservoir, initial.surface);
        temporalReservoir = temporal.reservoir;
    }

    if (reservoirIndex < CleanRtxdiDiReservoirCount)
    {
        CleanRtxdiDiTemporalReservoirs[reservoirIndex] = RTXDI_PackDIReservoir(temporalReservoir);
    }

    if (CleanRtxdiDiTemporalAudit != 0u)
    {
        return PathTraceCleanRoomTemporalAuditColor(initial, temporal);
    }

    if (view == 6u)
    {
        const float3 currentColor = PathTraceCleanRoomFlatDiffuseResolve(initial);
        const bool temporalSdkCalled = !temporalAllowed || (temporal.flags & CLEAN_TEMPORAL_DIAG_SDK_CALLED) != 0u;
        const bool temporalAcceptedPrevious = !temporalAllowed || (temporal.flags & CLEAN_TEMPORAL_DIAG_SDK_REUSED_PREVIOUS) != 0u;
        const bool temporalRenderable = (initial.status == CLEAN_INITIAL_STATUS_VALID || initial.status == CLEAN_INITIAL_STATUS_ZERO_TARGET_PDF) &&
            RTXDI_IsValidDIReservoir(temporalReservoir);
        const float3 temporalColor = temporalSdkCalled && temporalAcceptedPrevious
            ? (temporalRenderable
                ? PathTraceCleanRoomFlatDiffuseResolveReservoir(initial.surface, temporalReservoir)
                : (initial.status == CLEAN_INITIAL_STATUS_ZERO_TARGET_PDF ? float3(0.0, 0.0, 0.0) : PathTraceCleanRoomStatusColor(initial.status)))
            : float3(0.95, 0.04, 0.10);
        return pixel.x < (dimensions.x >> 1u) ? currentColor : temporalColor;
    }

    if (view == 8u)
    {
        if (CleanRtxdiDiView8Band == 7u)
        {
            return PathTraceCleanRoomNeeCacheProviderDiagnosticColor(initial);
        }
        if (CleanRtxdiDiView8Band >= 11u && CleanRtxdiDiView8Band <= 14u)
        {
            return PathTraceCleanRoomPreviousBestDiagnosticColor(initial);
        }
        return PathTraceCleanRoomTemporalDiagnosticColor(initial, temporal, pixel, dimensions);
    }

    if (view == 9u || view == 10u || view == 11u || view == 12u)
    {
        if (!RTXDI_IsValidDIReservoir(temporalReservoir))
        {
            return float3(0.0, 0.0, 0.0);
        }
        return PathTraceCleanRoomFlatDiffuseResolveReservoir(initial.surface, temporalReservoir);
    }

    if (view == 5u)
    {
        if (temporalAllowed && (temporal.flags & CLEAN_TEMPORAL_DIAG_SDK_CALLED) == 0u)
        {
            return float3(0.95, 0.04, 0.10);
        }
        if (temporalAllowed && (temporal.flags & CLEAN_TEMPORAL_DIAG_SDK_REUSED_PREVIOUS) == 0u)
        {
            return float3(0.95, 0.04, 0.10);
        }
        if (initial.status != CLEAN_INITIAL_STATUS_VALID &&
            !(initial.status == CLEAN_INITIAL_STATUS_ZERO_TARGET_PDF && RTXDI_IsValidDIReservoir(temporalReservoir)))
        {
            return initial.status == CLEAN_INITIAL_STATUS_ZERO_TARGET_PDF ? float3(0.0, 0.0, 0.0) : PathTraceCleanRoomStatusColor(initial.status);
        }
        return PathTraceCleanRoomFlatDiffuseResolveReservoir(initial.surface, temporalReservoir);
    }

    const bool reusedPrevious = (temporal.flags & CLEAN_TEMPORAL_DIAG_SDK_REUSED_PREVIOUS) != 0u;
    const float3 base = reusedPrevious ? float3(0.0, 0.85, 0.18) : float3(0.20, 0.28, 0.95);
    return RTXDI_IsValidDIReservoir(temporalReservoir)
        ? lerp(base, float3(saturate(temporalReservoir.M / 16.0), 0.95, 0.25), 0.45)
        : float3(0.95, 0.05, 0.12);
}
#endif
