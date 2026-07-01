float3 PathTraceCleanRoomSelectedSampleFailureColor(PathTracePrimarySurfaceRecord surfaceRecord, RTXDI_DIReservoir reservoir, uint initialStatus)
{
    if (initialStatus != CLEAN_INITIAL_STATUS_VALID && initialStatus != CLEAN_INITIAL_STATUS_ZERO_TARGET_PDF)
    {
        return PathTraceCleanRoomStatusColor(initialStatus);
    }
    if (!RTXDI_IsValidDIReservoir(reservoir))
    {
        return float3(1.0, 0.0, 0.0); // red: no selected reservoir sample
    }

    const uint lightIndex = RTXDI_GetDIReservoirLightIndex(reservoir);
    if (!PathTraceCleanRoomSyntheticMode() &&
        !PathTraceCleanRoomExternalPdfNeeCurrentEnabled() &&
        !PathTraceCleanRoomNeeCacheProviderEnabled() &&
        lightIndex >= PathTraceCleanRoomInitialLightCount())
    {
        return float3(1.0, 0.45, 0.0); // orange: selected index outside the active light domain
    }

    const RAB_Surface surface = PathTraceCleanRoomSurfaceFromRecord(surfaceRecord);
    if (!RAB_IsSurfaceValid(surface))
    {
        return float3(0.45, 0.0, 0.65); // purple: invalid primary surface
    }

    const RAB_LightInfo lightInfo = RAB_LoadLightInfo(lightIndex, false);
    const RAB_LightSample lightSample = RAB_SamplePolymorphicLight(lightInfo, surface, RTXDI_GetDIReservoirSampleUV(reservoir));
    const bool selectedEmissive =
        RAB_IsLightInfoValid(lightInfo) &&
        lightInfo.unifiedLightType == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE;
    if (!RAB_IsReplayableLightSample(lightSample))
    {
        return float3(1.0, 0.0, 1.0); // magenta: selected light cannot replay into a valid sample
    }
    if (lightSample.solidAnglePdf <= 1.0e-8 || !(lightSample.solidAnglePdf == lightSample.solidAnglePdf))
    {
        return float3(0.0, 0.18, 1.0); // blue: invalid sampled-light solid-angle PDF
    }
    if (reservoir.targetPdf <= 1.0e-8 || !(reservoir.targetPdf == reservoir.targetPdf))
    {
        return float3(0.0, 0.95, 1.0); // cyan: invalid selected target PDF
    }

    const float radianceLum = RAB_Luminance(max(lightSample.radiance, float3(0.0, 0.0, 0.0)));
    if (radianceLum <= 1.0e-8 || !(radianceLum == radianceLum))
    {
        return float3(1.0, 0.86, 0.0); // yellow: selected light has no usable radiance
    }

    const float3 toSample = lightSample.position - surfaceRecord.worldPositionAndViewDepth.xyz;
    const float3 lightDirection = PathTraceCleanRoomSafeNormalize(toSample, PathTraceCleanRoomSafeNormalize(surfaceRecord.shadingNormalAndOpacity.xyz, surfaceRecord.geometricNormalAndRoughness.xyz));
    const float3 shadingNormal = PathTraceCleanRoomSafeNormalize(surfaceRecord.shadingNormalAndOpacity.xyz, surfaceRecord.geometricNormalAndRoughness.xyz);
    const float ndotl = saturate(dot(shadingNormal, lightDirection));
    if (ndotl <= 1.0e-8 || !(ndotl == ndotl))
    {
        return float3(0.55, 0.0, 1.0); // violet: selected point is behind the shading normal
    }

    const float visibility = PathTraceCleanRoomSyntheticSingleLightMode()
        ? 1.0
        : PathTraceCleanRoomSelectedSampleVisibility(surfaceRecord, reservoir, lightSample);
    if (visibility <= 1.0e-8 || !(visibility == visibility))
    {
        if (selectedEmissive)
        {
            return float3(0.0, 0.45, 0.95); // blue-green: selected emissive is rejected by clean visibility
        }
        return float3(0.0, 0.95, 0.12); // green: selected sample is rejected by clean visibility
    }

    const float3 resolved = PathTraceCleanRoomFlatDiffuseResolveReservoir(surfaceRecord, reservoir);
    const float resolvedLum = RAB_Luminance(max(resolved, float3(0.0, 0.0, 0.0)));
    if (resolvedLum <= 1.0e-8 || !(resolvedLum == resolvedLum))
    {
        return float3(0.45, 0.05, 0.02); // dark red: black output after all explicit gates passed
    }

    if (selectedEmissive)
    {
        return float3(
            saturate(log2(1.0 + max(lightSample.solidAnglePdf, 0.0)) / 8.0),
            saturate(log2(1.0 + max(reservoir.targetPdf, 0.0)) / 6.0),
            saturate(log2(1.0 + max(resolvedLum, 0.0)) / 12.0));
    }

    const float brightness = saturate(log2(1.0 + max(resolvedLum, 0.0)) / 12.0);
    return lerp(float3(0.12, 0.12, 0.12), float3(1.0, 1.0, 1.0), brightness);
}

float3 PathTraceCleanRoomSelectedLightTypeColor(PathTracePrimarySurfaceRecord surfaceRecord, RTXDI_DIReservoir reservoir, uint initialStatus)
{
    if (initialStatus != CLEAN_INITIAL_STATUS_VALID && initialStatus != CLEAN_INITIAL_STATUS_ZERO_TARGET_PDF)
    {
        return PathTraceCleanRoomStatusColor(initialStatus);
    }
    if (!RTXDI_IsValidDIReservoir(reservoir))
    {
        return float3(1.0, 0.0, 0.0);
    }

    const uint lightIndex = RTXDI_GetDIReservoirLightIndex(reservoir);
    if (!PathTraceCleanRoomSyntheticMode() &&
        !PathTraceCleanRoomExternalPdfNeeCurrentEnabled() &&
        !PathTraceCleanRoomNeeCacheProviderEnabled() &&
        lightIndex >= PathTraceCleanRoomInitialLightCount())
    {
        return float3(1.0, 0.0, 1.0);
    }

    const RAB_Surface surface = PathTraceCleanRoomSurfaceFromRecord(surfaceRecord);
    if (!RAB_IsSurfaceValid(surface))
    {
        return float3(0.45, 0.0, 0.65);
    }

    const RAB_LightInfo lightInfo = RAB_LoadLightInfo(lightIndex, false);
    if (!RAB_IsLightInfoValid(lightInfo))
    {
        return float3(1.0, 0.35, 0.0);
    }
    if (lightInfo.unifiedLightType == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
    {
        return float3(0.08, 0.22, 0.95);
    }
    if (lightInfo.unifiedLightType == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
    {
        return float3(0.95, 0.55, 0.05);
    }
    return float3(1.0, 1.0, 1.0);
}

float3 PathTraceCleanRoomReservoirIdentityColor(PathTraceCleanRtxdiDiInitialResult result, uint2 pixel)
{
    if (result.status != CLEAN_INITIAL_STATUS_VALID)
    {
        return PathTraceCleanRoomStatusColor(result.status);
    }
    const float3 identityColor = PathTraceCleanRoomHashColor(result.selectedLightIndex ^ (CleanRtxdiDiFrameIndex * 747796405u));
    const uint band = (pixel.y / 64u) % 3u;
    if (band == 1u)
    {
        return identityColor;
    }
    if (band == 2u)
    {
        return result.visibility > 0.0 ? float3(saturate(result.reservoir.M / 4.0), 0.95, 0.25) : float3(0.95, 0.05, 0.12);
    }
    return lerp(float3(0.0, 0.85, 0.18), identityColor, 0.60);
}

float3 PathTraceCleanRoomReservoirWeightColor(PathTraceCleanRtxdiDiInitialResult result, uint2 pixel)
{
    if (result.status != CLEAN_INITIAL_STATUS_VALID)
    {
        return PathTraceCleanRoomStatusColor(result.status);
    }
    const uint band = (pixel.y / 64u) % 3u;
    if (band == 0u)
    {
        return float3(saturate(log2(1.0 + result.targetPdf) / 8.0), 0.15, 0.85);
    }
    if (band == 1u)
    {
        return float3(0.15, saturate(log2(1.0 + RTXDI_GetDIReservoirInvPdf(result.reservoir)) / 12.0), 0.85);
    }
    return result.visibility > 0.0 ? float3(0.0, 0.95, 0.72) : float3(0.95, 0.05, 0.12);
}

float3 PathTraceCleanRoomNeeCacheProviderDiagnosticColor(PathTraceCleanRtxdiDiInitialResult result)
{
    if (!PathTraceCleanRoomNeeCacheProviderEnabled())
    {
        return float3(0.55, 0.0, 0.85);
    }
    if (result.status != CLEAN_INITIAL_STATUS_VALID)
    {
        return PathTraceCleanRoomStatusColor(result.status);
    }

    const RAB_Surface surface = PathTraceCleanRoomSurfaceFromRecord(result.surface);
    if (!RAB_IsSurfaceValid(surface))
    {
        return PathTraceCleanRoomStatusColor(CLEAN_INITIAL_STATUS_INVALID_SURFACE);
    }

    const PathTraceNeeCacheCellDebug cell = PathTraceNeeCacheMapWorldPositionToCell(
        surface.worldPos,
        CleanRtxdiDiCameraOriginAndValid.xyz,
        max((uint)CleanRtxdiDiNeeCacheInfo1.x, 1u),
        max(CleanRtxdiDiNeeCacheInfo1.y, 1.0),
        PathTraceCleanRoomNeeCacheCellCount());
    const bool cellIndexValid = cell.valid != 0u && cell.cellIndex < PathTraceCleanRoomNeeCacheCellCount();
    PathTraceNeeCacheCellRecord storedCell = (PathTraceNeeCacheCellRecord)0;
    if (cellIndexValid)
    {
        storedCell = CleanRtxdiDiNeeCacheCells[cell.cellIndex];
    }
    const bool cellHashValid =
        cellIndexValid &&
        storedCell.flags != 0u &&
        storedCell.hash == cell.hash;
    const bool cellHasCandidates =
        cellHashValid &&
        storedCell.candidateCount > 0u;

    const RAB_LightInfo lightInfo = RAB_LoadLightInfo(result.selectedLightIndex, false);
    const bool selectedEmissive = RAB_IsLightInfoValid(lightInfo) &&
        lightInfo.unifiedLightType == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE;
    const bool selectedAnalytic = RAB_IsLightInfoValid(lightInfo) &&
        lightInfo.unifiedLightType == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC;

    return float3(
        cellHashValid ? 0.05 : 0.95,
        cellHasCandidates ? 0.95 : 0.08,
        selectedEmissive ? 0.95 : (selectedAnalytic ? 0.35 : 0.02));
}

float PathTraceCleanRoomLog01(float value, float scale)
{
    return saturate(log2(1.0 + max(value, 0.0)) / max(scale, 1.0e-3));
}

float3 PathTraceCleanRoomTargetInvPdfColor(float targetPdf, float invPdf)
{
    return float3(
        PathTraceCleanRoomLog01(targetPdf, 14.0),
        PathTraceCleanRoomLog01(invPdf, 16.0),
        0.15);
}

bool PathTraceCleanRoomNeeCacheSelectBestSecondaryEmissiveCandidate(
    PathTraceNeeCacheCellDebug cell,
    out uint selectedDenseRluIndex,
    out float selectedSourcePdf,
    out float selectedWeight,
    out float totalWeight,
    out uint usableCount)
{
    selectedDenseRluIndex = 0xffffffffu;
    selectedSourcePdf = 0.0;
    selectedWeight = 0.0;
    totalWeight = 0.0;
    usableCount = 0u;

    if (cell.valid == 0u || cell.cellIndex >= PathTraceCleanRoomNeeCacheCellCount())
    {
        return false;
    }

    const PathTraceNeeCacheCellRecord storedCell = CleanRtxdiDiNeeCacheCells[cell.cellIndex];
    if (storedCell.flags == 0u || storedCell.hash != cell.hash)
    {
        return false;
    }

    const uint candidateSlots = PathTraceCleanRoomNeeCacheCandidateSlots();
    const uint baseSlot = cell.cellIndex * candidateSlots;
    [loop]
    for (uint slot = 0u; slot < candidateSlots; ++slot)
    {
        const PathTraceNeeCacheCandidateRecord candidate = CleanRtxdiDiNeeCacheCandidates[baseSlot + slot];
        if (candidate.lightClass != PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE ||
            !PathTraceCleanRoomNeeCacheCandidateRecordUsable(candidate))
        {
            continue;
        }

        const PathTraceUnifiedLightRecord record = CleanRtxdiDiRluCurrentLights[candidate.denseRluIndex];
        const float currentWeight = PathTraceCleanRoomNeeCacheEmissiveCandidateWeight(record, cell);
        if (currentWeight <= 0.0)
        {
            continue;
        }

        usableCount++;
        totalWeight += currentWeight;
        if (currentWeight > selectedWeight)
        {
            selectedWeight = currentWeight;
            selectedDenseRluIndex = candidate.denseRluIndex;
        }
    }

    if (selectedDenseRluIndex == 0xffffffffu || totalWeight <= 0.0)
    {
        return false;
    }

    float selectedIdentityWeight = 0.0;
    [loop]
    for (uint pdfSlot = 0u; pdfSlot < candidateSlots; ++pdfSlot)
    {
        const PathTraceNeeCacheCandidateRecord candidate = CleanRtxdiDiNeeCacheCandidates[baseSlot + pdfSlot];
        if (candidate.denseRluIndex != selectedDenseRluIndex ||
            candidate.lightClass != PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE ||
            !PathTraceCleanRoomNeeCacheCandidateRecordUsable(candidate))
        {
            continue;
        }

        const PathTraceUnifiedLightRecord record = CleanRtxdiDiRluCurrentLights[candidate.denseRluIndex];
        selectedIdentityWeight += PathTraceCleanRoomNeeCacheEmissiveCandidateWeight(record, cell);
    }

    const float cacheProbability = 1.0 - saturate(CleanRtxdiDiNeeCacheInfo0.y);
    selectedSourcePdf = cacheProbability * selectedIdentityWeight / max(totalWeight, 1.0e-8);
    return selectedSourcePdf > 0.0;
}

float3 PathTraceCleanRoomNeeCacheSecondaryCandidateFieldColorFromSurface(PathTracePrimarySurfaceRecord surfaceRecord, uint2 pixel, uint2 dimensions)
{
    if (!PathTraceCleanRoomNeeCacheProviderEnabled())
    {
        return float3(0.55, 0.0, 0.85);
    }

    const RAB_Surface surface = PathTraceCleanRoomSurfaceFromRecord(surfaceRecord);
    if (!RAB_IsSurfaceValid(surface))
    {
        return PathTraceCleanRoomStatusColor(CLEAN_INITIAL_STATUS_INVALID_SURFACE);
    }

    const PathTraceNeeCacheCellDebug cell = PathTraceNeeCacheMapWorldPositionToCell(
        surface.worldPos,
        CleanRtxdiDiCameraOriginAndValid.xyz,
        max((uint)CleanRtxdiDiNeeCacheInfo1.x, 1u),
        max(CleanRtxdiDiNeeCacheInfo1.y, 1.0),
        PathTraceCleanRoomNeeCacheCellCount());
    if (cell.valid == 0u || cell.cellIndex >= PathTraceCleanRoomNeeCacheCellCount())
    {
        return float3(0.95, 0.82, 0.05);
    }

    const PathTraceNeeCacheCellRecord storedCell = CleanRtxdiDiNeeCacheCells[cell.cellIndex];
    if (storedCell.flags == 0u || storedCell.hash != cell.hash)
    {
        return float3(0.50, 0.10, 0.02);
    }

    uint selectedDenseRluIndex;
    float selectedSourcePdf;
    float selectedWeight;
    float totalWeight;
    uint usableCount;
    if (!PathTraceCleanRoomNeeCacheSelectBestSecondaryEmissiveCandidate(
        cell,
        selectedDenseRluIndex,
        selectedSourcePdf,
        selectedWeight,
        totalWeight,
        usableCount))
    {
        const float occupancy = saturate((float)storedCell.candidateCount / max((float)PathTraceCleanRoomNeeCacheCandidateSlots(), 1.0));
        return float3(0.02, 0.08 + occupancy * 0.20, 0.28 + occupancy * 0.32);
    }

    const uint third = max(dimensions.x / 3u, 1u);
    if (pixel.x < third)
    {
        return float3(
            0.05,
            saturate((float)usableCount / max((float)PathTraceCleanRoomNeeCacheCandidateSlots(), 1.0)),
            saturate((float)storedCell.candidateCount / max((float)PathTraceCleanRoomNeeCacheCandidateSlots(), 1.0)));
    }
    if (pixel.x < third * 2u)
    {
        const float3 identityColor = PathTraceCleanRoomHashColor(selectedDenseRluIndex);
        const float3 cellColor = PathTraceNeeCacheCellColor(cell.hash);
        return lerp(identityColor, cellColor, 0.35);
    }

    return float3(
        PathTraceCleanRoomLog01(selectedSourcePdf, 2.0),
        PathTraceCleanRoomLog01(selectedWeight, 12.0),
        PathTraceCleanRoomLog01(totalWeight, 14.0));
}

float3 PathTraceCleanRoomNeeCacheSecondaryCandidateFieldColorForPixel(uint2 pixel, uint2 dimensions)
{
    PathTracePrimarySurfaceRecord surfaceRecord;
    if (!PathTraceCleanRoomLoadSurfaceRecordSigned(int2(pixel), dimensions, false, surfaceRecord))
    {
        return PathTraceCleanRoomStatusColor(CLEAN_INITIAL_STATUS_INVALID_SURFACE);
    }

    return PathTraceCleanRoomNeeCacheSecondaryCandidateFieldColorFromSurface(surfaceRecord, pixel, dimensions);
}

float3 PathTraceCleanRoomRealAnalyticOneSampleDiagnosticColor(uint2 pixel, uint2 dimensions)
{
    PathTraceCleanRtxdiDiInitialResult result = PathTraceCleanRoomRunSelectedInitialProducer(pixel, dimensions);
    if (result.status != CLEAN_INITIAL_STATUS_VALID)
    {
        return PathTraceCleanRoomStatusColor(result.status);
    }

    const float sampledDistance = distance(result.surface.worldPositionAndViewDepth.xyz, result.samplePosition);
    const float resolvedLuminance = RAB_Luminance(PathTraceCleanRoomFlatDiffuseResolveReservoir(result.surface, result.reservoir));
    const uint band = min((pixel.y * 5u) / max(dimensions.y, 1u), 4u);
    if (band == 0u)
    {
        const float v = PathTraceCleanRoomLog01(sampledDistance, 8.0);
        return float3(v, v, v);
    }
    if (band == 1u)
    {
        const float v = PathTraceCleanRoomLog01(result.solidAnglePdf, 8.0);
        return float3(0.0, v, 0.0);
    }
    if (band == 2u)
    {
        const float v = PathTraceCleanRoomLog01(result.targetPdf, 6.0);
        return float3(v, 0.0, 0.0);
    }
    if (band == 3u)
    {
        const float v = PathTraceCleanRoomLog01(resolvedLuminance, 6.0);
        return float3(v, v * 0.75, 0.05);
    }

    return PathTraceCleanRoomHashColor(result.selectedLightIndex);
}

float3 PathTraceCleanRoomRealAnalyticTargetFactorDiagnosticColor(uint2 pixel, uint2 dimensions)
{
    const uint2 samplePixel = min(dimensions - 1u, dimensions >> 1u);
    PathTracePrimarySurfaceRecord surfaceRecord;
    if (!PathTraceCleanRoomLoadSurfaceRecord(samplePixel, dimensions, surfaceRecord))
    {
        return PathTraceCleanRoomStatusColor(CLEAN_INITIAL_STATUS_INVALID_SURFACE);
    }

    const RAB_Surface surface = PathTraceCleanRoomSurfaceFromRecord(surfaceRecord);
    uint firstLightIndex = 0u;
    uint lightCount = 0u;
    PathTraceCleanRoomAnalyticDiagnosticRange(firstLightIndex, lightCount);
    if (!RAB_IsSurfaceValid(surface) || lightCount == 0u || CleanRtxdiDiLightMode != 1u)
    {
        return PathTraceCleanRoomStatusColor(lightCount == 0u ? CLEAN_INITIAL_STATUS_NO_LIGHTS : CLEAN_INITIAL_STATUS_INVALID_SURFACE);
    }

    uint lightIndex = 0u;
    RAB_LightInfo lightInfo = RAB_EmptyLightInfo();
    RAB_LightSample lightSample = RAB_EmptyLightSample();
    const uint scanCount = min(lightCount, 64u);
    [loop]
    for (uint candidateIndex = 0u; candidateIndex < scanCount; ++candidateIndex)
    {
        const uint denseCandidateIndex = firstLightIndex + candidateIndex;
        RAB_LightInfo candidateInfo = RAB_LoadLightInfo(denseCandidateIndex, false);
        RAB_LightSample candidateSample = RAB_SamplePolymorphicLight(candidateInfo, surface, float2(0.5, 0.5));
        if (RAB_IsReplayableLightSample(candidateSample) && RAB_Luminance(candidateSample.radiance) > 1.0e-8)
        {
            lightIndex = denseCandidateIndex;
            lightInfo = candidateInfo;
            lightSample = candidateSample;
            break;
        }
    }
    const float2 sampleUv = float2(0.5, 0.5);
    if (!RAB_IsReplayableLightSample(lightSample))
    {
        return PathTraceCleanRoomHashColor(lightIndex) * float3(0.25, 0.05, 0.05);
    }

    float3 lightDir;
    float lightDistance;
    RAB_GetLightDirDistance(surface, lightSample, lightDir, lightDistance);
    const float shadeNdotL = saturate(dot(RAB_GetSurfaceNormal(surface), lightDir));
    const float geoNdotL = saturate(dot(RAB_GetSurfaceGeoNormal(surface), lightDir));
    const float shadeNdotV = saturate(dot(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceViewDir(surface)));
    const float geoNdotV = saturate(dot(RAB_GetSurfaceGeoNormal(surface), RAB_GetSurfaceViewDir(surface)));
    const float3 brdf = PathTraceCleanRtxdiDiMaterialEvaluateOpaqueDirectBrdf(surface, lightDir, RAB_GetSurfaceViewDir(surface));
    const float radianceLum = RAB_Luminance(lightSample.radiance);
    const float brdfLum = RAB_Luminance(brdf);
    const float targetPdf = PathTraceCleanRtxdiDiMaterialEvaluateLightSampleTargetPdf(lightSample, surface);
    const float reflectedLum = RAB_Luminance(brdf * lightSample.radiance * shadeNdotL);

    const uint band = min((pixel.y * 8u) / max(dimensions.y, 1u), 7u);
    if (band == 0u)
    {
        const float v = PathTraceCleanRoomLog01(lightDistance, 8.0);
        return float3(v, v, v);
    }
    if (band == 1u)
    {
        const float v = PathTraceCleanRoomLog01(lightSample.solidAnglePdf, 8.0);
        return float3(0.0, v, 0.0);
    }
    if (band == 2u)
    {
        const float v = PathTraceCleanRoomLog01(radianceLum, 6.0);
        return float3(v, v * 0.5, 0.0);
    }
    if (band == 3u)
    {
        return pixel.x < (dimensions.x >> 1u)
            ? float3(0.0, shadeNdotL, 0.0)
            : float3(0.0, 0.0, geoNdotL);
    }
    if (band == 4u)
    {
        return pixel.x < (dimensions.x >> 1u)
            ? float3(shadeNdotV, shadeNdotV, 0.0)
            : float3(geoNdotV, 0.0, geoNdotV);
    }
    if (band == 5u)
    {
        const float v = PathTraceCleanRoomLog01(brdfLum, 4.0);
        return float3(v, 0.0, v);
    }
    if (band == 6u)
    {
        const float target = PathTraceCleanRoomLog01(targetPdf, 6.0);
        const float reflected = PathTraceCleanRoomLog01(reflectedLum, 6.0);
        return pixel.x < (dimensions.x >> 1u)
            ? float3(target, 0.0, 0.0)
            : float3(reflected, reflected * 0.75, 0.0);
    }

    return PathTraceCleanRoomHashColor(lightIndex);
}

float3 PathTraceCleanRoomBinaryGateColor(bool pass)
{
    return pass ? float3(0.0, 0.90, 0.15) : float3(0.90, 0.04, 0.04);
}

float3 PathTraceCleanRoomRealAnalyticBinaryGateDiagnosticColor(uint2 pixel, uint2 dimensions)
{
    const uint2 samplePixel = min(dimensions - 1u, dimensions >> 1u);
    PathTracePrimarySurfaceRecord surfaceRecord;
    const bool surfaceRecordValid = PathTraceCleanRoomLoadSurfaceRecord(samplePixel, dimensions, surfaceRecord);
    RAB_Surface surface = RAB_EmptySurface();
    if (surfaceRecordValid)
    {
        surface = PathTraceCleanRoomSurfaceFromRecord(surfaceRecord);
    }
    uint firstLightIndex = 0u;
    uint lightCount = 0u;
    PathTraceCleanRoomAnalyticDiagnosticRange(firstLightIndex, lightCount);
    const bool lightDomainValid = CleanRtxdiDiLightMode == 1u && lightCount > 0u;
    const bool surfaceValid = surfaceRecordValid && RAB_IsSurfaceValid(surface);
    RAB_LightInfo lightInfo = RAB_EmptyLightInfo();
    RAB_LightSample lightSample = RAB_EmptyLightSample();
    if (surfaceValid && lightDomainValid)
    {
        const uint scanCount = min(lightCount, 64u);
        [loop]
        for (uint candidateIndex = 0u; candidateIndex < scanCount; ++candidateIndex)
        {
            const uint denseCandidateIndex = firstLightIndex + candidateIndex;
            RAB_LightInfo candidateInfo = RAB_LoadLightInfo(denseCandidateIndex, false);
            RAB_LightSample candidateSample = RAB_SamplePolymorphicLight(candidateInfo, surface, float2(0.5, 0.5));
            if (RAB_IsReplayableLightSample(candidateSample) && RAB_Luminance(candidateSample.radiance) > 1.0e-8)
            {
                lightInfo = candidateInfo;
                lightSample = candidateSample;
                break;
            }
        }
    }

    float3 lightDir = float3(0.0, 0.0, 1.0);
    float lightDistance = 0.0;
    if (RAB_IsReplayableLightSample(lightSample))
    {
        RAB_GetLightDirDistance(surface, lightSample, lightDir, lightDistance);
    }

    const float shadeNdotL = dot(RAB_GetSurfaceNormal(surface), lightDir);
    const float geoNdotL = dot(RAB_GetSurfaceGeoNormal(surface), lightDir);
    const float shadeNdotV = dot(RAB_GetSurfaceNormal(surface), RAB_GetSurfaceViewDir(surface));
    const float geoNdotV = dot(RAB_GetSurfaceGeoNormal(surface), RAB_GetSurfaceViewDir(surface));
    const float radianceLum = RAB_Luminance(lightSample.radiance);
    const bool brdfSupported = PathTraceCleanRtxdiDiMaterialSupportsOpaqueDirect(surface);
    const float3 brdf = PathTraceCleanRtxdiDiMaterialEvaluateOpaqueDirectBrdf(surface, lightDir, RAB_GetSurfaceViewDir(surface));
    const float brdfLum = RAB_Luminance(brdf);
    const float targetPdf = PathTraceCleanRtxdiDiMaterialEvaluateLightSampleTargetPdf(lightSample, surface);
    const float reflectedLum = RAB_Luminance(brdf * lightSample.radiance * saturate(shadeNdotL));

    const uint band = min((pixel.y * 8u) / max(dimensions.y, 1u), 7u);
    if (band == 0u)
    {
        return PathTraceCleanRoomBinaryGateColor(surfaceValid && lightDomainValid);
    }
    if (band == 1u)
    {
        return PathTraceCleanRoomBinaryGateColor(brdfSupported);
    }
    if (band == 2u)
    {
        return PathTraceCleanRoomBinaryGateColor(RAB_IsReplayableLightSample(lightSample));
    }
    if (band == 3u)
    {
        return PathTraceCleanRoomBinaryGateColor(radianceLum > 1.0e-8);
    }
    if (band == 4u)
    {
        return PathTraceCleanRoomBinaryGateColor(shadeNdotL > 0.0 && geoNdotL > 0.0);
    }
    if (band == 5u)
    {
        return PathTraceCleanRoomBinaryGateColor(shadeNdotV > 0.0 && geoNdotV > 0.0);
    }
    if (band == 6u)
    {
        return PathTraceCleanRoomBinaryGateColor(brdfLum > 1.0e-8);
    }

    return pixel.x < (dimensions.x >> 1u)
        ? PathTraceCleanRoomBinaryGateColor(targetPdf > 1.0e-8)
        : PathTraceCleanRoomBinaryGateColor(reflectedLum > 1.0e-8);
}
