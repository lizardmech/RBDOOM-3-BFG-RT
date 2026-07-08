float2 PathTraceCleanRoomNeeCacheRandomLightUv(inout RTXDI_RandomSamplerState rng)
{
    return float2(RTXDI_GetNextRandom(rng), RTXDI_GetNextRandom(rng));
}

bool PathTraceCleanRoomNeeCacheCandidateRecordUsable(PathTraceNeeCacheCandidateRecord candidate)
{
    if (candidate.flags == 0u ||
        candidate.denseRluIndex >= CleanRtxdiDiRluCurrentLightCount ||
        candidate.sourcePdf <= 0.0 ||
        candidate.invSourcePdf <= 0.0)
    {
        return false;
    }

    const PathTraceUnifiedLightRecord record = CleanRtxdiDiRluCurrentLights[candidate.denseRluIndex];
    if (record.type != candidate.lightClass)
    {
        return false;
    }
    if (candidate.lightClass == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC &&
        (record.flags & PATH_TRACE_RLU_LIGHT_FLAG_STABLE_CACHEABLE) == 0u)
    {
        return false;
    }

    return RAB_IsLightInfoValid(RAB_LoadLightInfo(candidate.denseRluIndex, false));
}

float PathTraceCleanRoomNeeCacheEmissiveCandidateWeight(PathTraceUnifiedLightRecord record, PathTraceNeeCacheCellDebug cell)
{
    if (record.type != PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
    {
        return 0.0;
    }

    const float area = max(record.normalAndArea.w, 0.0);
    const float sourceWeight = max(record.sourceWeight, record.radianceAndLuminance.w);
    if (area <= 1.0e-6 || sourceWeight <= 0.0)
    {
        return 0.0;
    }

    const float3 cellCenter = (float3(cell.coord) + 0.5) * max(cell.cellSize, 1.0);
    const float3 toCell = cellCenter - record.positionAndRadius.xyz;
    const float distanceSquared = dot(toCell, toCell);
    const float normalFacing = saturate(dot(normalize(toCell + float3(0.0, 0.0, 1.0e-6)), record.normalAndArea.xyz));
    const float cellRadius = max(cell.cellSize, 1.0) * 0.8660254;
    return sourceWeight * area * (0.2 + 0.8 * normalFacing) /
        max(distanceSquared + cellRadius * cellRadius, 1.0);
}

float PathTraceCleanRoomNeeCacheAnalyticCandidateWeight(PathTraceUnifiedLightRecord record, PathTraceNeeCacheCellDebug cell)
{
    if (record.type != PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC ||
        (record.flags & PATH_TRACE_RLU_LIGHT_FLAG_STABLE_CACHEABLE) == 0u)
    {
        return 0.0;
    }

    const float radius = max(record.positionAndRadius.w, 1.0e-3);
    const float influenceRadius = max(record.uvOrDoomParams.x, radius);
    const float luminance = max(record.radianceAndLuminance.w, 0.0);
    const float sourceWeight = max(record.sourceWeight, luminance);
    if (sourceWeight <= 0.0 || luminance <= 0.0)
    {
        return 0.0;
    }

    const float3 cellCenter = (float3(cell.coord) + 0.5) * max(cell.cellSize, 1.0);
    const float cellRadius = max(cell.cellSize, 1.0) * 0.8660254;
    const float3 toCell = cellCenter - record.positionAndRadius.xyz;
    const float distanceToCell = length(toCell);
    const float influenceDistance = influenceRadius + cellRadius;
    const float edgeDistance = max(distanceToCell - radius, 0.0);
    const float falloff = saturate(1.0 - edgeDistance / max(influenceDistance - radius, 1.0));
    return sourceWeight * falloff * falloff /
        max(dot(toCell, toCell) + radius * radius + cellRadius * cellRadius, 1.0);
}

float PathTraceCleanRoomNeeCacheCurrentCandidateWeight(PathTraceNeeCacheCandidateRecord candidate, PathTraceNeeCacheCellDebug cell)
{
    if (!PathTraceCleanRoomNeeCacheCandidateRecordUsable(candidate))
    {
        return 0.0;
    }

    const PathTraceUnifiedLightRecord record = CleanRtxdiDiRluCurrentLights[candidate.denseRluIndex];
    if (candidate.lightClass == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
    {
        return PathTraceCleanRoomNeeCacheAnalyticCandidateWeight(record, cell);
    }
    if (candidate.lightClass == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
    {
        return PathTraceCleanRoomNeeCacheEmissiveCandidateWeight(record, cell);
    }
    return 0.0;
}

float PathTraceCleanRoomNeeCacheCacheIdentityPdf(PathTraceNeeCacheCellDebug cell, uint denseRluIndex, out bool hasCacheDistribution)
{
    hasCacheDistribution = false;

    const PathTraceNeeCacheCellRecord storedCell = CleanRtxdiDiNeeCacheCells[cell.cellIndex];
    if (storedCell.flags == 0u || storedCell.hash != cell.hash)
    {
        return 0.0;
    }

    const uint candidateSlots = PathTraceCleanRoomNeeCacheCandidateSlots();
    const uint baseSlot = cell.cellIndex * candidateSlots;
    float totalWeight = 0.0;
    float selectedIdentityWeight = 0.0;

    [loop]
    for (uint slot = 0u; slot < candidateSlots; ++slot)
    {
        const PathTraceNeeCacheCandidateRecord candidate = CleanRtxdiDiNeeCacheCandidates[baseSlot + slot];
        const float currentWeight = PathTraceCleanRoomNeeCacheCurrentCandidateWeight(candidate, cell);
        if (currentWeight <= 0.0)
        {
            continue;
        }

        totalWeight += currentWeight;
        if (candidate.denseRluIndex == denseRluIndex)
        {
            selectedIdentityWeight += currentWeight;
        }
    }

    hasCacheDistribution = totalWeight > 0.0;
    return hasCacheDistribution ? selectedIdentityWeight / max(totalWeight, 1.0e-8) : 0.0;
}

bool PathTraceCleanRoomNeeCacheSelectCandidate(
    inout RTXDI_RandomSamplerState rng,
    PathTraceNeeCacheCellDebug cell,
    out uint selectedDenseRluIndex)
{
    selectedDenseRluIndex = 0xffffffffu;

    const PathTraceNeeCacheCellRecord storedCell = CleanRtxdiDiNeeCacheCells[cell.cellIndex];
    if (storedCell.flags == 0u || storedCell.hash != cell.hash)
    {
        return false;
    }

    const uint candidateSlots = PathTraceCleanRoomNeeCacheCandidateSlots();
    const uint baseSlot = cell.cellIndex * candidateSlots;
    float totalWeight = 0.0;

    [loop]
    for (uint slot = 0u; slot < candidateSlots; ++slot)
    {
        const float currentWeight = PathTraceCleanRoomNeeCacheCurrentCandidateWeight(
            CleanRtxdiDiNeeCacheCandidates[baseSlot + slot],
            cell);
        if (currentWeight > 0.0)
        {
            totalWeight += currentWeight;
        }
    }

    if (totalWeight <= 0.0)
    {
        return false;
    }

    const float threshold = RTXDI_GetNextRandom(rng) * totalWeight;
    float cumulativeWeight = 0.0;
    PathTraceNeeCacheCandidateRecord selectedCandidate = (PathTraceNeeCacheCandidateRecord)0;
    selectedCandidate.denseRluIndex = 0xffffffffu;

    [loop]
    for (uint selectSlot = 0u; selectSlot < candidateSlots; ++selectSlot)
    {
        const PathTraceNeeCacheCandidateRecord candidate = CleanRtxdiDiNeeCacheCandidates[baseSlot + selectSlot];
        const float currentWeight = PathTraceCleanRoomNeeCacheCurrentCandidateWeight(candidate, cell);
        if (currentWeight <= 0.0)
        {
            continue;
        }

        cumulativeWeight += currentWeight;
        if (selectedCandidate.denseRluIndex == 0xffffffffu && cumulativeWeight >= threshold)
        {
            selectedCandidate = candidate;
        }
    }

    if (selectedCandidate.denseRluIndex == 0xffffffffu)
    {
        return false;
    }

    selectedDenseRluIndex = selectedCandidate.denseRluIndex;
    return true;
}

uint PathTraceCleanRoomNeeCacheStableAnalyticCount(uint analyticOffset, uint analyticAvailableCount)
{
    uint analyticCount = 0u;
    [loop]
    for (uint analyticIndex = 0u; analyticIndex < analyticAvailableCount; ++analyticIndex)
    {
        const PathTraceUnifiedLightRecord record = CleanRtxdiDiRluCurrentLights[analyticOffset + analyticIndex];
        if (record.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC &&
            (record.flags & PATH_TRACE_RLU_LIGHT_FLAG_STABLE_CACHEABLE) != 0u)
        {
            analyticCount++;
        }
    }
    return analyticCount;
}

float PathTraceCleanRoomNeeCacheFallbackIdentityPdf(uint denseRluIndex)
{
    if (CleanRtxdiDiRluCurrentLightCount == 0u ||
        denseRluIndex >= CleanRtxdiDiRluCurrentLightCount)
    {
        return 0.0;
    }

    const uint sourceDomain = PathTraceCleanRoomNeeCacheSourceDomain();
    const uint emissiveOffset = 0u;
    const uint emissiveCount = min(CleanRtxdiDiCurrentEmissiveTriangleCount, CleanRtxdiDiRluCurrentLightCount);

    if (sourceDomain == 0u)
    {
        return 1.0 / max((float)CleanRtxdiDiRluCurrentLightCount, 1.0);
    }
    if (sourceDomain == 1u)
    {
        return denseRluIndex >= emissiveOffset && denseRluIndex < emissiveOffset + emissiveCount
            ? 1.0 / max((float)emissiveCount, 1.0)
            : 0.0;
    }

    const uint analyticOffset = min(CleanRtxdiDiRluDoomAnalyticRangeOffset, CleanRtxdiDiRluCurrentLightCount);
    const uint analyticAvailableCount = min(CleanRtxdiDiRluDoomAnalyticRangeCount, CleanRtxdiDiRluCurrentLightCount - analyticOffset);
    const uint analyticCount = PathTraceCleanRoomNeeCacheStableAnalyticCount(analyticOffset, analyticAvailableCount);

    if (sourceDomain == 2u)
    {
        const PathTraceUnifiedLightRecord record = CleanRtxdiDiRluCurrentLights[denseRluIndex];
        return denseRluIndex >= analyticOffset &&
            denseRluIndex < analyticOffset + analyticAvailableCount &&
            record.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC &&
            (record.flags & PATH_TRACE_RLU_LIGHT_FLAG_STABLE_CACHEABLE) != 0u
            ? 1.0 / max((float)analyticCount, 1.0)
            : 0.0;
    }

    const bool inEmissiveRange =
        denseRluIndex >= emissiveOffset &&
        denseRluIndex < emissiveOffset + emissiveCount;
    if (inEmissiveRange)
    {
        const float classProbability = (emissiveCount > 0u && analyticCount > 0u) ? 0.5 : 1.0;
        return classProbability / max((float)emissiveCount, 1.0);
    }

    const PathTraceUnifiedLightRecord record = CleanRtxdiDiRluCurrentLights[denseRluIndex];
    const bool inStableAnalyticRange =
        denseRluIndex >= analyticOffset &&
        denseRluIndex < analyticOffset + analyticAvailableCount &&
        record.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC &&
        (record.flags & PATH_TRACE_RLU_LIGHT_FLAG_STABLE_CACHEABLE) != 0u;
    if (inStableAnalyticRange)
    {
        const float classProbability = (emissiveCount > 0u && analyticCount > 0u) ? 0.5 : 1.0;
        return classProbability / max((float)analyticCount, 1.0);
    }

    return 0.0;
}

bool PathTraceCleanRoomNeeCacheSelectFallback(
    inout RTXDI_RandomSamplerState rng,
    out uint selectedDenseRluIndex)
{
    selectedDenseRluIndex = 0xffffffffu;

    if (CleanRtxdiDiRluCurrentLightCount == 0u)
    {
        return false;
    }

    const uint sourceDomain = PathTraceCleanRoomNeeCacheSourceDomain();
    const uint emissiveOffset = 0u;
    const uint emissiveCount = min(CleanRtxdiDiCurrentEmissiveTriangleCount, CleanRtxdiDiRluCurrentLightCount);
    const uint analyticOffset = min(CleanRtxdiDiRluDoomAnalyticRangeOffset, CleanRtxdiDiRluCurrentLightCount);
    const uint analyticAvailableCount = min(CleanRtxdiDiRluDoomAnalyticRangeCount, CleanRtxdiDiRluCurrentLightCount - analyticOffset);
    uint analyticCount = 0u;
    if (sourceDomain == 2u || sourceDomain == 3u)
    {
        analyticCount = PathTraceCleanRoomNeeCacheStableAnalyticCount(analyticOffset, analyticAvailableCount);
    }

    uint rangeOffset = 0u;
    uint rangeCount = CleanRtxdiDiRluCurrentLightCount;
    bool stableAnalyticRange = false;

    if (sourceDomain == 1u)
    {
        rangeOffset = emissiveOffset;
        rangeCount = emissiveCount;
    }
    else if (sourceDomain == 2u)
    {
        rangeOffset = analyticOffset;
        rangeCount = analyticCount;
        stableAnalyticRange = true;
    }
    else if (sourceDomain == 3u)
    {
        const bool chooseAnalytic =
            analyticCount > 0u &&
            (emissiveCount == 0u || RTXDI_GetNextRandom(rng) >= 0.5);
        rangeOffset = chooseAnalytic ? analyticOffset : emissiveOffset;
        rangeCount = chooseAnalytic ? analyticCount : emissiveCount;
        stableAnalyticRange = chooseAnalytic;
    }

    if (rangeCount == 0u)
    {
        return false;
    }

    uint denseIndex = 0xffffffffu;
    if (stableAnalyticRange)
    {
        const uint stableLocalIndex = min((uint)(RTXDI_GetNextRandom(rng) * (float)rangeCount), rangeCount - 1u);
        uint stableOrdinal = 0u;
        [loop]
        for (uint scanIndex = 0u; scanIndex < analyticAvailableCount; ++scanIndex)
        {
            const uint candidateIndex = analyticOffset + scanIndex;
            const PathTraceUnifiedLightRecord record = CleanRtxdiDiRluCurrentLights[candidateIndex];
            if (record.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC &&
                (record.flags & PATH_TRACE_RLU_LIGHT_FLAG_STABLE_CACHEABLE) != 0u)
            {
                if (stableOrdinal == stableLocalIndex)
                {
                    denseIndex = candidateIndex;
                    break;
                }
                stableOrdinal++;
            }
        }
    }
    else
    {
        const uint localIndex = min((uint)(RTXDI_GetNextRandom(rng) * (float)rangeCount), rangeCount - 1u);
        denseIndex = rangeOffset + localIndex;
    }

    if (denseIndex >= CleanRtxdiDiRluCurrentLightCount)
    {
        return false;
    }

    selectedDenseRluIndex = denseIndex;
    return PathTraceCleanRoomNeeCacheFallbackIdentityPdf(denseIndex) > 0.0;
}

bool PathTraceCleanRoomNeeCacheStreamLightIntoReservoir(
    inout RTXDI_DIReservoir reservoir,
    inout RTXDI_RandomSamplerState rng,
    RAB_Surface surface,
    uint lightIndex,
    float2 uv,
    float sourceSelectionPdf)
{
    const RAB_LightInfo lightInfo = RAB_LoadLightInfo(lightIndex, false);
    if (!RAB_IsLightInfoValid(lightInfo))
    {
        return false;
    }

    const RAB_LightSample lightSample = RAB_SamplePolymorphicLight(lightInfo, surface, uv);
    if (!RAB_IsReplayableLightSample(lightSample) || lightSample.solidAnglePdf <= 0.0)
    {
        return false;
    }

    const float targetPdf = max(PathTraceCleanRtxdiDiMaterialEvaluateLightSampleTargetPdf(lightSample, surface), 0.0);
    const bool resolveDividesBySolidAngle = (CleanRtxdiDiFlags & CLEAN_FLAG_RESOLVE_SOLID_ANGLE_PDF) != 0u;
    const float sourcePdf = sourceSelectionPdf * (resolveDividesBySolidAngle ? 1.0 : lightSample.solidAnglePdf);
    if (targetPdf <= 0.0 || sourcePdf <= 0.0)
    {
        return false;
    }

    RTXDI_StreamSample(
        reservoir,
        lightIndex,
        uv,
        RTXDI_GetNextRandom(rng),
        targetPdf,
        1.0 / max(sourcePdf, 1.0e-8));
    return true;
}

bool PathTraceCleanRoomNeeCacheStreamProviderIntoReservoir(
    inout RTXDI_DIReservoir reservoir,
    inout RTXDI_RandomSamplerState rng,
    RAB_Surface surface)
{
    if (!PathTraceCleanRoomNeeCacheProviderEnabled() ||
        !PathTraceCleanRoomRemixLightUniverseEnabled() ||
        CleanRtxdiDiRluCurrentLightCount == 0u)
    {
        return false;
    }

    const PathTraceNeeCacheCellDebug cell = PathTraceNeeCacheMapWorldPositionToCell(
        surface.worldPos,
        CleanRtxdiDiCameraOriginAndValid.xyz,
        max((uint)CleanRtxdiDiNeeCacheInfo1.x, 1u),
        max(CleanRtxdiDiNeeCacheInfo1.y, 1.0),
        PathTraceCleanRoomNeeCacheCellCount());
    if (cell.valid == 0u || cell.cellIndex >= PathTraceCleanRoomNeeCacheCellCount())
    {
        return false;
    }

    uint selectedDenseRluIndex = 0xffffffffu;
    const float fallbackProbability = saturate(CleanRtxdiDiNeeCacheInfo0.y);
    const float cacheProbability = 1.0 - fallbackProbability;
    const bool randomFallback = RTXDI_GetNextRandom(rng) < fallbackProbability;
    const bool selectedCacheCandidate =
        !randomFallback &&
        PathTraceCleanRoomNeeCacheSelectCandidate(
            rng,
            cell,
            selectedDenseRluIndex);

    if (!selectedCacheCandidate)
    {
        if (!PathTraceCleanRoomNeeCacheSelectFallback(
            rng,
            selectedDenseRluIndex))
        {
            return false;
        }
    }

    bool hasCacheDistribution = false;
    const float cacheIdentityPdf = PathTraceCleanRoomNeeCacheCacheIdentityPdf(cell, selectedDenseRluIndex, hasCacheDistribution);
    const float fallbackIdentityPdf = PathTraceCleanRoomNeeCacheFallbackIdentityPdf(selectedDenseRluIndex);
    const float sourceSelectionPdf = hasCacheDistribution
        ? cacheProbability * cacheIdentityPdf + fallbackProbability * fallbackIdentityPdf
        : fallbackIdentityPdf;
    if (sourceSelectionPdf <= 0.0)
    {
        return false;
    }

    const float2 lightUv = PathTraceCleanRoomNeeCacheRandomLightUv(rng);
    return PathTraceCleanRoomNeeCacheStreamLightIntoReservoir(
        reservoir,
        rng,
        surface,
        selectedDenseRluIndex,
        lightUv,
        sourceSelectionPdf);
}

PathTraceCleanRtxdiDiInitialResult PathTraceCleanRoomRunNeeCacheProviderProducer(uint2 pixel, uint2 dimensions)
{
    PathTraceCleanRtxdiDiInitialResult result = (PathTraceCleanRtxdiDiInitialResult)0;
    result.reservoir = RTXDI_EmptyDIReservoir();
    result.selectedLightIndex = 0xffffffffu;
    result.status = CLEAN_INITIAL_STATUS_VALID;

    if (CleanRtxdiDiLightMode != 1u)
    {
        result.status = CLEAN_INITIAL_STATUS_DEFERRED_LIGHT_MODE;
        return result;
    }

    if (!PathTraceCleanRoomNeeCacheProviderEnabled() ||
        !PathTraceCleanRoomRemixLightUniverseEnabled() ||
        CleanRtxdiDiRluCurrentLightCount == 0u)
    {
        result.status = CLEAN_INITIAL_STATUS_EXTERNAL_CURRENT_EMPTY;
        return result;
    }

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

    RTXDI_DIInitialSamplingParameters sampleParams = (RTXDI_DIInitialSamplingParameters)0;
    sampleParams.numLocalLightSamples = max(CleanRtxdiDiCandidateCount, 1u);
    sampleParams.localLightSamplingMode = ReSTIRDI_LocalLightSamplingMode_UNIFORM;
    sampleParams.enableInitialVisibility = 0u;

    RTXDI_RandomSamplerState rng = RTXDI_InitRandomSamplerForPass(pixel, CleanRtxdiDiFrameIndex, 0x4e434439u, 0u);
    // NEE-cache provider selection is cell/range sensitive and each streamed
    // proposal consumes fallback/candidate/UV/RIS draws. The shared STBN path
    // only covers the first RBPT_BLUE_NOISE_DIMS dimensions, so a provider loop
    // would blue-noise only the first few candidates then fall back to white
    // noise. Keep this lane white-noise until it has a dedicated per-candidate
    // dimension plan that cannot over-lock light choices.
    rng.useBlueNoise = 0u;
    [loop]
    for (uint sampleIndex = 0u; sampleIndex < sampleParams.numLocalLightSamples; ++sampleIndex)
    {
        PathTraceCleanRoomNeeCacheStreamProviderIntoReservoir(result.reservoir, rng, surface);
    }

    RTXDI_FinalizeResampling(result.reservoir, 1.0, (float)max(sampleParams.numLocalLightSamples, 1u));

    result.status = RTXDI_IsValidDIReservoir(result.reservoir) ? CLEAN_INITIAL_STATUS_VALID : CLEAN_INITIAL_STATUS_ZERO_TARGET_PDF;
    if (result.status != CLEAN_INITIAL_STATUS_VALID)
    {
        return result;
    }

    result.selectedLightIndex = RTXDI_GetDIReservoirLightIndex(result.reservoir);
    if (result.selectedLightIndex >= CleanRtxdiDiRluCurrentLightCount)
    {
        result.status = CLEAN_INITIAL_STATUS_EXTERNAL_UNSUPPORTED_LIGHT;
        result.reservoir = RTXDI_EmptyDIReservoir();
        return result;
    }

    const RAB_LightInfo lightInfo = RAB_LoadLightInfo(result.selectedLightIndex, false);
    if (!RAB_IsLightInfoValid(lightInfo))
    {
        result.status = CLEAN_INITIAL_STATUS_EXTERNAL_UNSUPPORTED_LIGHT;
        result.reservoir = RTXDI_EmptyDIReservoir();
        return result;
    }

    result.sampleUv = RTXDI_GetDIReservoirSampleUV(result.reservoir);
    const RAB_LightSample selectedSample = RAB_SamplePolymorphicLight(lightInfo, surface, result.sampleUv);
    if (!RAB_IsReplayableLightSample(selectedSample) || selectedSample.solidAnglePdf <= 0.0)
    {
        result.status = CLEAN_INITIAL_STATUS_BAD_SAMPLE_PDF;
        result.reservoir = RTXDI_EmptyDIReservoir();
        return result;
    }

    result.samplePosition = selectedSample.position;
    result.sampleRadiance = selectedSample.radiance;
    result.solidAnglePdf = selectedSample.solidAnglePdf;
    result.targetPdf = result.reservoir.targetPdf;
    result.invSourcePdf = RTXDI_GetDIReservoirInvPdf(result.reservoir);
    result.visibility = PathTraceCleanRoomSelectedSampleVisibility(result.surface, result.reservoir, selectedSample);
    RTXDI_StoreVisibilityInDIReservoir(
        result.reservoir,
        result.visibility.xxx,
        PathTraceCleanRoomInitialVisibilityEnabled());
    return result;
}

bool PathTraceCleanRoomNeeCacheReplaySelectedReservoir(
    inout PathTraceCleanRtxdiDiInitialResult result,
    RAB_Surface surface)
{
    result.status = RTXDI_IsValidDIReservoir(result.reservoir)
        ? CLEAN_INITIAL_STATUS_VALID
        : CLEAN_INITIAL_STATUS_ZERO_TARGET_PDF;
    if (result.status != CLEAN_INITIAL_STATUS_VALID)
    {
        return false;
    }

    result.selectedLightIndex = RTXDI_GetDIReservoirLightIndex(result.reservoir);
    if (result.selectedLightIndex >= CleanRtxdiDiRluCurrentLightCount)
    {
        result.status = CLEAN_INITIAL_STATUS_EXTERNAL_UNSUPPORTED_LIGHT;
        result.reservoir = RTXDI_EmptyDIReservoir();
        return false;
    }

    const RAB_LightInfo lightInfo = RAB_LoadLightInfo(result.selectedLightIndex, false);
    if (!RAB_IsLightInfoValid(lightInfo))
    {
        result.status = CLEAN_INITIAL_STATUS_EXTERNAL_UNSUPPORTED_LIGHT;
        result.reservoir = RTXDI_EmptyDIReservoir();
        return false;
    }

    result.sampleUv = RTXDI_GetDIReservoirSampleUV(result.reservoir);
    const RAB_LightSample selectedSample = RAB_SamplePolymorphicLight(lightInfo, surface, result.sampleUv);
    if (!RAB_IsReplayableLightSample(selectedSample) || selectedSample.solidAnglePdf <= 0.0)
    {
        result.status = CLEAN_INITIAL_STATUS_BAD_SAMPLE_PDF;
        result.reservoir = RTXDI_EmptyDIReservoir();
        return false;
    }

    result.samplePosition = selectedSample.position;
    result.sampleRadiance = selectedSample.radiance;
    result.solidAnglePdf = selectedSample.solidAnglePdf;
    result.targetPdf = result.reservoir.targetPdf;
    result.invSourcePdf = RTXDI_GetDIReservoirInvPdf(result.reservoir);
    result.visibility = PathTraceCleanRoomSelectedSampleVisibility(result.surface, result.reservoir, selectedSample);
    RTXDI_StoreVisibilityInDIReservoir(
        result.reservoir,
        result.visibility.xxx,
        PathTraceCleanRoomInitialVisibilityEnabled());
    return true;
}

bool PathTraceCleanRoomTryAugmentInitialResultWithNeeCache(
    uint2 pixel,
    inout PathTraceCleanRtxdiDiInitialResult result)
{
    if (!PathTraceCleanRoomNeeCacheProviderEnabled() ||
        !PathTraceCleanRoomRemixLightUniverseEnabled() ||
        CleanRtxdiDiRluCurrentLightCount == 0u ||
        result.status != CLEAN_INITIAL_STATUS_VALID ||
        !RTXDI_IsValidDIReservoir(result.reservoir))
    {
        return false;
    }

    const RAB_Surface surface = PathTraceCleanRoomSurfaceForView(result.surface);
    if (!RAB_IsSurfaceValid(surface))
    {
        return false;
    }

    RTXDI_DIReservoir mixedReservoir = RTXDI_EmptyDIReservoir();
    RTXDI_RandomSamplerState rng =
        RTXDI_InitRandomSamplerForPass(pixel, CleanRtxdiDiFrameIndex, 0x4e43414du, 0u);
    // See provider path above: the cache augment stream remains white-noise so
    // its first cached proposal is not treated differently from later proposals
    // by the shared 16-dimension STBN cap.
    rng.useBlueNoise = 0u;

    RTXDI_CombineDIReservoirs(
        mixedReservoir,
        result.reservoir,
        RTXDI_GetNextRandom(rng),
        result.reservoir.targetPdf);

    const bool streamedCache =
        PathTraceCleanRoomNeeCacheStreamProviderIntoReservoir(mixedReservoir, rng, surface);
    if (!streamedCache)
    {
        return false;
    }

    RTXDI_FinalizeResampling(mixedReservoir, 1.0, max(mixedReservoir.M, 1.0));
    mixedReservoir.M = 1.0;
    mixedReservoir.packedVisibility = RTXDI_PackedDIReservoir_VisibilityMask;
    mixedReservoir.spatialDistance = int2(0, 0);
    mixedReservoir.age = 0u;
    mixedReservoir.canonicalWeight = 1.0;

    result.reservoir = mixedReservoir;
    result.previousBestSelected = 0u;
    return PathTraceCleanRoomNeeCacheReplaySelectedReservoir(result, surface);
}
