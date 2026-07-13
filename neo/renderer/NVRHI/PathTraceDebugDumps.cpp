#include "precompiled.h"
#pragma hdrstop

// Console diagnostics and one-shot dump orchestration for RT smoke state.
//
// Keep formatting and CVar-trigger reset behavior here so the scene builder and
// dispatch path can stay focused on data flow rather than terminal output.

#include "PathTraceCVars.h"
#include "PathTraceDebugDumps.h"
#include "PathTraceEmissiveCandidates.h"
#include "PathTraceMaterialClassifier.h"
#include "PathTraceMaterialTextureDiscovery.h"
#include "PathTraceSurfaceDebugDumps.h"
#include "PathTraceTextureRegistry.h"


namespace {

const int RT_SMOKE_DEBUG_MATERIAL_REASON_SAMPLES = 12;
const int RT_SMOKE_TEXTURE_PROBE_CANDIDATE_SAMPLES = 24;
const int RT_SMOKE_TEXTURE_PROBE_DUMP_CANDIDATES = 64;
const uint32_t RT_SMOKE_TRIANGLE_CLASS_MASK = 0x0000ffffu;

const char* SmokeTextureCoverageClassNameByIndex(int classIndex)
{
    switch (classIndex)
    {
        case 0:
            return "static";
        case 1:
            return "rigid-entity";
        case 2:
            return "skinned";
        case 3:
            return "particle/alpha";
        case 4:
            return "unknown";
        default:
            return "invalid";
    }
}

const char* SmokeTextureFallbackReason(const RtSmokeMaterialTableBuild& table, int tableIndex, const RtSmokeMaterialTextureInfo& info)
{
    if (!SmokeMaterialTableIndexIsValid(table, tableIndex))
    {
        return "invalid material index";
    }

    const PathTraceSmokeMaterial& material = table.materials[tableIndex];
    if (material.diffuseTextureIndex != UINT32_MAX)
    {
        return "bound";
    }
    if (!info.diffuseImage)
    {
        return "no diffuse image";
    }
    if (!info.hasTextureHandle)
    {
        return "no texture handle";
    }
    if (!info.hasSafeTexture)
    {
        return info.fallbackReason.c_str();
    }
    return "safe but outside active window";
}

void AccumulateSmokeTextureCoverageTriangles(
    const RtSmokeMaterialTableBuild& table,
    const std::vector<uint32_t>& triangleClassData,
    const std::vector<uint32_t>& triangleMaterialIndexes,
    RtSmokeTextureCoverageStats& stats)
{
    const int triangleCount = Min(static_cast<int>(triangleClassData.size()), static_cast<int>(triangleMaterialIndexes.size()));
    for (int triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex)
    {
        const int classIndex = idMath::ClampInt(0, RT_SMOKE_DEBUG_TEXTURE_COVERAGE_CLASS_COUNT - 1, static_cast<int>(triangleClassData[triangleIndex] & RT_SMOKE_TRIANGLE_CLASS_MASK));
        RtSmokeTextureCoverageClassStats& classStats = stats.classes[classIndex];
        ++classStats.triangles;

        const uint32_t materialIndex = triangleMaterialIndexes[triangleIndex];
        if (!SmokeMaterialTableIndexIsValid(table, static_cast<int>(materialIndex)))
        {
            ++classStats.invalidMaterialTriangles;
            ++classStats.fallbackTriangles;
            continue;
        }

        if (table.materials[materialIndex].diffuseTextureIndex != UINT32_MAX)
        {
            ++classStats.boundTriangles;
        }
        else
        {
            ++classStats.fallbackTriangles;
        }
    }
}

}

const char* SmokeCoverageName(materialCoverage_t coverage)
{
    switch (coverage)
    {
        case MC_OPAQUE:
            return "opaque";
        case MC_PERFORATED:
            return "perforated";
        case MC_TRANSLUCENT:
            return "translucent";
        default:
            return "bad";
    }
}

const char* SmokeStageLightingName(stageLighting_t lighting)
{
    switch (lighting)
    {
        case SL_AMBIENT:
            return "ambient";
        case SL_BUMP:
            return "bump";
        case SL_DIFFUSE:
            return "diffuse";
        case SL_SPECULAR:
            return "specular";
        case SL_COVERAGE:
            return "coverage";
        default:
            return "unknown";
    }
}

const char* SmokeTexgenName(texgen_t texgen)
{
    switch (texgen)
    {
        case TG_EXPLICIT:
            return "explicit";
        case TG_DIFFUSE_CUBE:
            return "diffuse-cube";
        case TG_REFLECT_CUBE:
            return "reflect-cube";
        case TG_REFLECT_CUBE2:
            return "reflect-cube2";
        case TG_SKYBOX_CUBE:
            return "skybox-cube";
        case TG_WOBBLESKY_CUBE:
            return "wobblesky-cube";
        case TG_SCREEN:
            return "screen";
        case TG_SCREEN2:
            return "screen2";
        case TG_GLASSWARP:
            return "glasswarp";
        default:
            return "unknown";
    }
}

const char* SmokeTextureUsageName(textureUsage_t usage)
{
    switch (usage)
    {
        case TD_SPECULAR:
            return "TD_SPECULAR";
        case TD_DIFFUSE:
            return "TD_DIFFUSE";
        case TD_DEFAULT:
            return "TD_DEFAULT";
        case TD_BUMP:
            return "TD_BUMP";
        case TD_COVERAGE:
            return "TD_COVERAGE";
        default:
            return va("TD_%d", static_cast<int>(usage));
    }
}

const char* SmokeTextureColorFormatName(textureColor_t colorFormat)
{
    switch (colorFormat)
    {
        case CFM_DEFAULT:
            return "CFM_DEFAULT";
        case CFM_NORMAL_DXT5:
            return "CFM_NORMAL_DXT5";
        case CFM_YCOCG_DXT5:
            return "CFM_YCOCG_DXT5";
        case CFM_GREEN_ALPHA:
            return "CFM_GREEN_ALPHA";
        default:
            return va("CFM_%d", static_cast<int>(colorFormat));
    }
}

const char* SmokeDeformName(deform_t deform)
{
    switch (deform)
    {
        case DFRM_NONE:
            return "none";
        case DFRM_SPRITE:
            return "sprite";
        case DFRM_TUBE:
            return "tube";
        case DFRM_FLARE:
            return "flare";
        case DFRM_EXPAND:
            return "expand";
        case DFRM_MOVE:
            return "move";
        case DFRM_EYEBALL:
            return "eyeball";
        case DFRM_PARTICLE:
            return "particle";
        case DFRM_PARTICLE2:
            return "particle2";
        case DFRM_TURB:
            return "turb";
        default:
            return "unknown";
    }
}

const char* SmokeDynamicModelName(dynamicModel_t dynamicModel)
{
    switch (dynamicModel)
    {
        case DM_STATIC:
            return "static";
        case DM_CACHED:
            return "cached";
        case DM_CONTINUOUS:
            return "continuous";
        default:
            return "unknown";
    }
}

RtPathTraceDebugModeInfo GetPathTraceDebugModeInfo(int debugMode)
{
    RtPathTraceDebugModeInfo info;
    info.mode = debugMode;

    switch (debugMode)
    {
    case 0: info.name = "hit/miss"; info.category = "pass-output-visualizer"; info.output = "primary hit mask"; break;
    case 1: info.name = "depth"; info.category = "pass-output-visualizer"; info.output = "primary depth"; break;
    case 2: info.name = "interpolated normal"; info.category = "pass-output-visualizer"; info.output = "primary interpolated normal"; break;
    case 3: info.name = "surface class"; info.category = "scene-geometry-diagnostic"; info.output = "triangle surface class"; break;
    case 4: info.name = "uv"; info.category = "material-texture-diagnostic"; info.output = "primary UV"; break;
    case 5: info.name = "geometric normal"; info.category = "pass-output-visualizer"; info.output = "primary geometric normal"; break;
    case 6: info.name = "material ID hash color"; info.category = "material-texture-diagnostic"; info.output = "material identity hash"; break;
    case 7: info.name = "material table debug color"; info.category = "material-texture-diagnostic"; info.output = "material table record"; break;
    case 8: info.name = "sampled diffuse texture probe"; info.category = "material-texture-diagnostic"; info.output = "diffuse texture sample"; break;
    case 9: info.name = "alpha/coverage preview"; info.category = "material-texture-diagnostic"; info.output = "alpha coverage"; break;
    case 10: info.name = "albedo"; info.category = "material-texture-diagnostic"; info.output = "diffuse albedo"; break;
    case 11: info.name = "translucent overlay inspection"; info.category = "material-texture-diagnostic"; info.output = "translucent overlay"; break;
    case 12: info.name = "translucent subtype classification"; info.category = "material-texture-diagnostic"; info.output = "translucent subtype"; break;
    case 13: info.name = "fixed-light Lambert shading"; info.category = "temporary-behavior-experiment"; info.output = "fixed diagnostic light"; info.behaviorChanging = true; info.temporary = true; break;
    case 14: info.name = "selected point-light direct lighting"; info.category = "light-diagnostic"; info.output = "selected-light direct term"; break;
    case 15: info.name = "selected point-light influence"; info.category = "light-diagnostic"; info.output = "selected-light influence"; break;
    case 16: info.name = "normal map preview"; info.category = "material-texture-diagnostic"; info.output = "decoded normal map"; break;
    case 17: info.name = "specular map preview"; info.category = "material-texture-diagnostic"; info.output = "decoded specular map"; break;
    case 18: info.name = "toy path trace"; info.category = "temporary-behavior-experiment"; info.output = "path tracer color"; info.owner = "path-tracer-core"; info.behaviorChanging = true; info.temporary = true; break;
    case 19: info.name = "emissive triangle inventory"; info.category = "light-diagnostic"; info.output = "emissive candidate inventory"; break;
    case 20: info.name = "single-frame reservoir direct lighting"; info.category = "temporary-behavior-experiment"; info.output = "smoke reservoir direct light"; info.owner = "smoke-reservoir"; info.behaviorChanging = true; info.temporary = true; break;
    case 21: info.name = "solid drawSurf mirror bounds boxes"; info.category = "scene-geometry-diagnostic"; info.output = "drawSurf bounds"; break;
    case 22: info.name = "wireframe drawSurf mirror bounds boxes"; info.category = "scene-geometry-diagnostic"; info.output = "drawSurf bounds wireframe"; break;
    case 23: info.name = "experimental routed rigid TLAS instances"; info.category = "scene-geometry-diagnostic"; info.output = "rigid route instances"; info.temporary = true; break;
    case 24: info.name = "fallback-vs-rigid-route overlap"; info.category = "scene-geometry-diagnostic"; info.output = "route overlap validation"; break;
    case 25: info.name = "routed rigid lighting"; info.category = "scene-geometry-diagnostic"; info.output = "rigid route lighting validation"; break;
    case 26: info.name = "ReSTIR PT initial reservoir diagnostics"; info.category = "pass-output-visualizer"; info.output = "initial reservoir metadata"; info.owner = "restir-pt"; break;
    case 27: info.name = "ReSTIR PT initial reservoir shading"; info.category = "pass-output-visualizer"; info.output = "initial reservoir shading"; info.owner = "restir-pt"; break;
    case 28: info.name = "ReSTIR PT initial visibility preview"; info.category = "pass-output-visualizer"; info.output = "initial reservoir visibility/shading"; info.owner = "restir-pt"; break;
    case 29: info.name = "ReSTIR PT primary-surface history"; info.category = "pass-output-visualizer"; info.output = "primary surface history fields"; info.owner = "primary-surface"; break;
    case 30: info.name = "ReSTIR PT screen-space reprojection"; info.category = "temporary-behavior-experiment"; info.output = "primary surface reprojection"; info.owner = "primary-surface"; info.temporary = true; break;
    case 31: info.name = "ReSTIR PT temporal reservoir"; info.category = "pass-output-visualizer"; info.output = "temporal reservoir metadata"; info.owner = "restir-pt"; break;
    case 32: info.name = "ReSTIR PT temporal reservoir shading"; info.category = "temporary-behavior-experiment"; info.output = "temporal reservoir shading"; info.owner = "restir-pt"; info.temporary = true; break;
    case 33: info.name = "ReSTIR PT temporal light-source attribution"; info.category = "pass-output-visualizer"; info.output = "temporal reservoir source attribution"; info.owner = "restir-pt"; break;
    case 34: info.name = "path tracer path-depth visualizer"; info.category = "pass-output-visualizer"; info.output = "path depth / SPP / bounce state"; info.owner = "path-tracer-core"; break;
    case 35: info.name = "path tracer reflection hit/miss"; info.category = "pass-output-visualizer"; info.output = "reflection hit/miss"; info.owner = "path-tracer-core"; break;
    case 36: info.name = "path tracer reflection roughness/F0 gate"; info.category = "pass-output-visualizer"; info.output = "reflection/specular gate"; info.owner = "path-tracer-core"; break;
    case 37: info.name = "path tracer max-depth / Russian roulette"; info.category = "pass-output-visualizer"; info.output = "depth limit / Russian-roulette state"; info.owner = "path-tracer-core"; break;
    case 38: info.name = "skinned object-motion vector"; info.category = "pass-output-visualizer"; info.output = "skinned current-to-previous object motion"; info.owner = "primary-surface"; break;
    case 39: info.name = "routed-rigid object-motion eligibility"; info.category = "scene-geometry-diagnostic"; info.output = "routed rigid previous-transform eligibility"; info.owner = "primary-surface"; break;
    case 40: info.name = "routed-rigid object-motion vector"; info.category = "pass-output-visualizer"; info.output = "routed rigid current-to-previous object motion"; info.owner = "primary-surface"; break;
    case 41: info.name = "combined object-motion vector"; info.category = "pass-output-visualizer"; info.output = "skinned and routed rigid current-to-previous object motion"; info.owner = "primary-surface"; break;
    case 42: info.name = "packed primary object-motion flags"; info.category = "pass-output-visualizer"; info.output = "primary surface object-motion record flags"; info.owner = "primary-surface"; break;
    case 43: info.name = "packed object-motion reprojection match"; info.category = "pass-output-visualizer"; info.output = "object-motion previous primary-surface match"; info.owner = "primary-surface"; break;
    case 44: info.name = "previous static snapshot binding"; info.category = "scene-geometry-diagnostic"; info.output = "previous static snapshot lookup validation"; info.owner = "primary-surface"; break;
    case 45: info.name = "previous static reprojection match"; info.category = "pass-output-visualizer"; info.output = "previous static snapshot reprojection match"; info.owner = "primary-surface"; break;
    case 46: info.name = "previous static motion vector"; info.category = "pass-output-visualizer"; info.output = "static current-to-previous object motion"; info.owner = "primary-surface"; break;
    case 47: info.name = "combined geometry motion vector"; info.category = "pass-output-visualizer"; info.output = "static, skinned, and routed rigid current-to-previous motion"; info.owner = "primary-surface"; break;
    case 48: info.name = "combined geometry reprojection match"; info.category = "pass-output-visualizer"; info.output = "static, skinned, and routed rigid previous primary-surface match"; info.owner = "primary-surface"; break;
    case 49: info.name = "combined geometry motion source"; info.category = "pass-output-visualizer"; info.output = "static, skinned, and routed rigid motion source coverage"; info.owner = "primary-surface"; break;
    case 50: info.name = "ReSTIR PT spatial reservoir shading"; info.category = "temporary-behavior-experiment"; info.output = "temporal plus spatial reservoir shading"; info.owner = "restir-pt"; info.temporary = true; break;
    case 51: info.name = "ReSTIR PT spatial source attribution"; info.category = "pass-output-visualizer"; info.output = "spatial reservoir source attribution"; info.owner = "restir-pt"; break;
    case 52: info.name = "routed-rigid transform parity"; info.category = "scene-geometry-diagnostic"; info.output = "routed metadata transform vs TLAS hit transform error"; info.owner = "primary-surface"; break;
    case 53: info.name = "ReSTIR PT indirect reservoir diagnostics"; info.category = "pass-output-visualizer"; info.output = "indirect/GI initial reservoir metadata"; info.owner = "restir-pt"; break;
    case 54: info.name = "ReSTIR PT indirect reservoir shading"; info.category = "temporary-behavior-experiment"; info.output = "indirect/GI initial reservoir shading"; info.owner = "restir-pt"; info.temporary = true; break;
    case 55: info.name = "ReSTIR PT indirect path attribution"; info.category = "pass-output-visualizer"; info.output = "indirect/GI source and path attribution"; info.owner = "restir-pt"; break;
    case 56: info.name = "ReSTIR PT combined direct+GI preview"; info.category = "temporary-behavior-experiment"; info.output = "combined direct spatial reservoir and indirect/GI initial reservoir shading"; info.owner = "restir-pt"; info.temporary = true; break;
    case 57: info.name = "material classifier GPU decode"; info.category = "material-texture-diagnostic"; info.output = "classifier route/class/roughness/F0 packed fields"; info.owner = "material-classifier"; break;
    default: break;
    }

    return info;
}

void LogPathTraceDebugModeInfo(const RtPathTraceDebugModeInfo& info)
{
    common->Printf("PathTracePrimaryPass: debug mode %d name='%s' category=%s output='%s' owner=%s behaviorChanging=%d temporary=%d\n",
        info.mode,
        info.name,
        info.category,
        info.output,
        info.owner,
        info.behaviorChanging ? 1 : 0,
        info.temporary ? 1 : 0);
}

bool ShouldLogSmokeTiming(int elapsedMs, int nowMs, int& lastLogMs)
{
    if (r_pathTracingTimingLog.GetInteger() == 0)
    {
        return false;
    }

    if (elapsedMs < Max(0, r_pathTracingTimingThreshold.GetInteger()))
    {
        return false;
    }

    const int intervalMs = Max(0, r_pathTracingTimingLogInterval.GetInteger());
    if (intervalMs > 0 && nowMs - lastLogMs < intervalMs)
    {
        return false;
    }

    lastLogMs = nowMs;
    return true;
}

void LogPathTraceDispatchTiming(const RtPathTraceDispatchTimingLogDesc& desc)
{
    const RtPathTraceDebugModeInfo fallbackInfo = GetPathTraceDebugModeInfo(desc.debugMode);
    const RtPathTraceDebugModeInfo& info = desc.debugModeInfo ? *desc.debugModeInfo : fallbackInfo;
    common->Printf("PathTracePrimaryPass: PT pass timing total=%.3fms setup=%.3f restirContext=%.3f constants=%.3f barriers=%.3f clear(reservoir=%.3f primaryHistory=%.3f targets=%.3f) setState=%.3f dispatchSubmit=%.3f historyCopy=%.3f readback=%.3f output=%dx%d dispatch=%dx%d mode=%d '%s' category=%s output='%s' owner=%s restirPass=%s resampling=%d spp=%d maxDepth=%d estimatedRaysPerPixel=%d lights selected/analytic=%d/%d restirVisibility=%d previewMaxPixels=%d requestedClears reservoir/primaryHistory=%d/%d readbackQueued=%d optickGpuMarkers=%d nsightGpuMarkers=%d\n",
        desc.totalSubmitMs,
        desc.setupMs,
        desc.restirContextMs,
        desc.constantsMs,
        desc.barrierMs,
        desc.reservoirClearMs,
        desc.primaryHistoryClearMs,
        desc.targetClearMs,
        desc.setStateMs,
        desc.dispatchSubmitMs,
        desc.historyCopyMs,
        desc.readbackCopyMs,
        desc.outputWidth,
        desc.outputHeight,
        desc.dispatchWidth,
        desc.dispatchHeight,
        desc.debugMode,
        info.name,
        info.category,
        info.output,
        info.owner,
        desc.restirPassLabel ? desc.restirPassLabel : "disabled",
        desc.restirResamplingMode,
        desc.samplesPerPixel,
        desc.maxPathDepth,
        desc.estimatedRaysPerPixel,
        desc.selectedLights,
        desc.analyticLights,
        desc.restirPreviewVisibility,
        desc.restirPreviewMaxPixels,
        desc.reservoirClearRequested ? 1 : 0,
        desc.primaryHistoryClearRequested ? 1 : 0,
        desc.readbackQueued ? 1 : 0,
        desc.optickGpuMarkers ? 1 : 0,
        desc.nsightGpuMarkers ? 1 : 0);
}

void LogSmokeSlowSceneBuild(const RtSmokeSlowSceneBuildLogDesc& desc)
{
    common->Printf("PathTracePrimaryPass: RT smoke slow scene build %d ms (capture=%d anchor=%d validate=%d staticPassClassify=%d staticCacheLookup=%d staticAppend=%d dynamicPassClassify=%d dynamicAppend=%d rtCpuSkinningAppend=%d append=%d merge=%d metadata=%d metaValidate=%d metaRegister=%d material=%d emissive=%d bufferCreate=%d bufferSubmit=%d bvhPlan=%d accelSubmit=%d blas=%d tlas=%d) surfaces=%d verts=%d indexes=%d dynamicIndexes=%d staticCached/new=%d/%d staticCache=%d/%d/%d/%d/%dKB staticLife=%d/%d/%d hist=%d/%d dirty=%d staticValidate=%d/%d/%d/%d/%d staticSig=%d/%dms anchorCull=%d/%d/%d skinnedRtCpu=%d(%di) staticCacheHit=%d materialTablePath=%s materialCacheHit=%d materialCache=%d/%d materialBuild=%d/%d/%d/%d/%d/%d/%d counts=%d/%d/%d materialUniverse=%d/%d/%d/%d/%d sig=%d frame=%d/%d/%d/%d validate=%d/%d frameValidate=%d/%d universeTableCompare=%d/%d material=%d/%d/%d indexes=%d/%d textures=%d/%d metadataCache=%d metadataFrame=%d/%d/%d/%d/%d metadataRegistry=%d guiTextures=%d/%d/%d additiveDecals=%d lightCandidates=%d/%d(%db) lightCount=%d debugMode=%d\n",
        desc.sceneMs,
        desc.captureMs,
        desc.captureAnchorMs,
        desc.captureValidationMs,
        desc.captureStaticPassClassifyMs,
        desc.captureStaticCacheLookupMs,
        desc.captureStaticAppendMs,
        desc.captureDynamicPassClassifyMs,
        desc.captureDynamicAppendMs,
        desc.captureRtCpuSkinningAppendMs,
        desc.captureAppendMs,
        desc.captureBucketMergeMs,
        desc.metadataMs,
        desc.metadataValidationMs,
        desc.metadataRegistrationMs,
        desc.materialMs,
        desc.emissiveMs,
        desc.bufferCreateMs,
        desc.bufferUploadMs,
        desc.bvhFramePlanMs,
        desc.accelSubmitMs,
        desc.blasSubmitMs,
        desc.tlasSubmitMs,
        desc.sourceSurfaces,
        desc.sourceVerts,
        desc.sourceIndexes,
        desc.dynamicIndexCount,
        desc.staticCachedSurfaces,
        desc.staticNewSurfaces,
        desc.staticSurfaceCacheSize,
        desc.staticVertexCacheCount,
        desc.staticIndexCacheCount,
        desc.staticTriangleCacheCount,
        desc.staticCacheBytesKB,
        desc.staticSeenThisFrame,
        desc.staticNewThisFrame,
        desc.staticDisappearedThisFrame,
        desc.staticHistoryValid,
        desc.staticPreviousRangeValid,
        desc.staticDirty,
        desc.staticValidationErrors,
        desc.staticRangeErrors,
        desc.staticDuplicateKeys,
        desc.staticHistoryErrors,
        desc.staticKeyVectorMismatches,
        desc.staticBlasSignatureReused ? 1 : 0,
        desc.staticBlasSignatureMs,
        desc.anchorSurfaceTests,
        desc.anchorBoundsRejects,
        desc.anchorTriangleTests,
        desc.skinnedRtCpuSurfaces,
        desc.skinnedRtCpuIndexes,
        desc.staticBlasCacheHit ? 1 : 0,
        desc.materialTablePath ? desc.materialTablePath : "unknown",
        desc.materialTableCacheHit ? 1 : 0,
        desc.materialTableCacheHits,
        desc.materialTableCacheMisses,
        desc.materialTableBuildStats.entryMs,
        desc.materialTableBuildStats.populateMs,
        desc.materialTableBuildStats.resetMs,
        desc.materialTableBuildStats.diagnosticMs,
        desc.materialTableBuildStats.safeOrderMs,
        desc.materialTableBuildStats.descriptorMs,
        desc.materialTableBuildStats.probeMs,
        desc.materialTableBuildStats.tableMaterials,
        desc.materialTableBuildStats.safeMaterials,
        desc.materialTableBuildStats.descriptorTextures,
        desc.materialUniverseStats.records,
        desc.materialUniverseMaterialCount,
        desc.materialUniverseStats.hits,
        desc.materialUniverseStats.misses,
        desc.materialUniverseStats.rebuilds,
        desc.materialUniverseStats.signatureChecks,
        desc.materialUniverseStats.frameHits,
        desc.materialUniverseStats.frameMisses,
        desc.materialUniverseStats.frameRebuilds,
        desc.materialUniverseStats.frameSignatureChecks,
        desc.materialUniverseStats.validationChecks,
        desc.materialUniverseStats.validationMismatches,
        desc.materialUniverseStats.frameValidationChecks,
        desc.materialUniverseStats.frameValidationMismatches,
        desc.materialUniverseTableCompareStats.checks,
        desc.materialUniverseTableCompareStats.mismatches,
        desc.materialUniverseTableCompareStats.materialCountMismatches,
        desc.materialUniverseTableCompareStats.materialIdMismatches,
        desc.materialUniverseTableCompareStats.materialRecordMismatches,
        desc.materialUniverseTableCompareStats.staticIndexMismatches,
        desc.materialUniverseTableCompareStats.dynamicIndexMismatches,
        desc.materialUniverseTableCompareStats.textureCountMismatches,
        desc.materialUniverseTableCompareStats.textureHandleMismatches,
        desc.materialMetadataCacheEnabled ? 1 : 0,
        desc.metadataCacheRefreshes,
        desc.metadataFullDiscovers,
        desc.metadataNewEntries,
        desc.metadataRegistrations,
        desc.metadataDuplicateSkips,
        desc.metadataRegistrySize,
        desc.guiTextureCandidates,
        desc.guiTexturesAccepted,
        desc.guiTexturesRejected,
        desc.additiveDecals,
        desc.lightCandidateCount,
        desc.texturedLightCandidateCount,
        desc.lightCandidateBytes,
        desc.lightCount,
        desc.debugMode);
}

static void LogSmokeSceneBuildCommonSummary(const RtSmokeSceneBuildSummaryLogDesc& desc)
{
    common->Printf("PathTracePrimaryPass: RT smoke BLAS split static-world=%d indexes, dynamic-candidate=%d indexes, TLAS instances=%d\n",
        desc.staticIndexCount, desc.dynamicIndexCount, desc.instanceCount);
    common->Printf("PathTracePrimaryPass: PT BVH active AS staticActive(surf/v/i)=%d/%d/%d staticRetained(surf/v/i)=%d/%d/%d staticBuckets(active/resident/inactive/emitted)=%d/%d/%d/%d staticBucketBlas(records/skipInactive/skipInvalid/overflow)=%d/%d/%d/%d staticBucketShader(routeRequired/compatible/exactMono/nonZeroOffsets)=%d/%d/%d/%d routeNs(staticBlocked/staticFirst/rigidFirst/staticCount/rigidShift)=%d/%u/%u/%d/%d staticPolicy(monolithicIncludesInactive/requiresBuckets)=%d/%d dynamicActive(v/i)=%d/%d rigidRoute(inst/unique/v/i/t geomBytes/instBytes)=%d/%d/%d/%d/%d %llu/%llu tlas(base+rigid/total)=%d+%d/%d dirty(prev geom/activeGeom/resident/material/active/tlas blas/tlas)=%d %d/%d/%d/%d/%d/%d %d/%d sig(active/resident/geom/activeBlas/tlas)=%llu/%llu/%llu/%llu/%llu blasBuild(static/dynamic submit skip)=%d/%d %d/%d uploadBytes(static/prev/prevSkip/dynamic/rigidRoute)=%llu/%llu/%llu/%llu/%llu timings(sig/routeBuild/bvhPlan/upload/blas/tlas/accel)=%d/%d/%d/%d/%d/%d/%d\n",
        desc.classStats.staticWorldSurfaces,
        desc.staticVertexCount,
        desc.staticIndexCount,
        desc.staticSurfaceCacheSize,
        desc.staticVertexCacheCount,
        desc.staticIndexCacheCount,
        desc.staticBvhActiveBuckets,
        desc.staticBvhResidentBuckets,
        desc.staticBvhInactiveResidentBuckets,
        desc.staticBvhEmittedInstances,
        desc.staticBucketBlasRecords,
        desc.staticBucketBlasSkippedInactive,
        desc.staticBucketBlasSkippedInvalid,
        desc.staticBucketBlasOverflow ? 1 : 0,
        desc.staticBucketTraversalRouteRequired ? 1 : 0,
        desc.staticBucketTraversalCurrentShaderCompatible ? 1 : 0,
        desc.staticBucketTraversalExactMonolithic ? 1 : 0,
        desc.staticBucketTraversalNonZeroOffsetRecords,
        desc.staticRouteNamespaceBlocked ? 1 : 0,
        desc.staticRouteNamespaceFirst,
        desc.rigidRouteNamespaceFirst,
        desc.staticRouteNamespaceCount,
        desc.rigidRouteNamespaceShifted ? 1 : 0,
        desc.staticMonolithicInactiveIncluded ? 1 : 0,
        desc.staticRequiresBucketedBlas ? 1 : 0,
        desc.dynamicVertexCount,
        desc.dynamicIndexCount,
        desc.rigidRouteInstanceCount,
        desc.rigidRouteUniqueMeshes,
        desc.rigidRouteVertexCount,
        desc.rigidRouteIndexCount,
        desc.rigidRouteTriangleCount,
        static_cast<unsigned long long>(desc.rigidRouteGeometryBytes),
        static_cast<unsigned long long>(desc.rigidRouteInstanceBytes),
        desc.instanceCount - desc.rigidTlasInstanceCount,
        desc.rigidTlasInstanceCount,
        desc.instanceCount,
        desc.bvhDirtyPreviousValid ? 1 : 0,
        desc.bvhGeometryContentChanged ? 1 : 0,
        desc.bvhActiveGeometryContentChanged ? 1 : 0,
        desc.bvhResidentSetChanged ? 1 : 0,
        desc.bvhMaterialChanged ? 1 : 0,
        desc.bvhActiveMembershipChanged ? 1 : 0,
        desc.bvhTlasInstanceChanged ? 1 : 0,
        desc.bvhBlasInputDirty ? 1 : 0,
        desc.bvhTlasDirty ? 1 : 0,
        static_cast<unsigned long long>(desc.bvhActiveSetSignature),
        static_cast<unsigned long long>(desc.bvhResidentSetSignature),
        static_cast<unsigned long long>(desc.bvhGeometryContentSignature),
        static_cast<unsigned long long>(desc.bvhActiveBlasInputSignature),
        static_cast<unsigned long long>(desc.bvhTlasInstanceSignature),
        desc.staticBlasBuildSubmitted ? 1 : 0,
        desc.dynamicBlasBuildSubmitted ? 1 : 0,
        desc.staticBlasBuildSkipped ? 1 : 0,
        desc.dynamicBlasBuildSkipped ? 1 : 0,
        static_cast<unsigned long long>(desc.staticUploadBytes),
        static_cast<unsigned long long>(desc.previousStaticUploadBytes),
        static_cast<unsigned long long>(desc.previousStaticUploadSkippedBytes),
        static_cast<unsigned long long>(desc.dynamicUploadBytes),
        static_cast<unsigned long long>(desc.rigidRouteUploadBytes),
        desc.staticBlasSignatureMs,
        desc.rigidRouteBuildMs,
        desc.bvhFramePlanMs,
        desc.bufferUploadMs,
        desc.blasSubmitMs,
        desc.tlasSubmitMs,
        desc.accelSubmitMs);
    common->Printf("PathTracePrimaryPass: RT smoke dynamic geometry rigid=%d(%di) skinnedCpu=%d(%di) skinnedRtCpu=%d(%di) skinnedLikelyBasePose=%d(%di) particle/alpha=%d(%di) unknown=%d(%di)\n",
        desc.dynamicStats.rigidSurfaces, desc.dynamicStats.rigidIndexes,
        desc.dynamicStats.skinnedCpuCurrentSurfaces, desc.dynamicStats.skinnedCpuCurrentIndexes,
        desc.dynamicStats.skinnedRtCpuSkinnedSurfaces, desc.dynamicStats.skinnedRtCpuSkinnedIndexes,
        desc.dynamicStats.skinnedLikelyBasePoseSurfaces, desc.dynamicStats.skinnedLikelyBasePoseIndexes,
        desc.dynamicStats.particleAlphaSurfaces, desc.dynamicStats.particleAlphaIndexes,
        desc.dynamicStats.unknownSurfaces, desc.dynamicStats.unknownIndexes);
    common->Printf("PathTracePrimaryPass: RT smoke static BLAS cache %s signature=%llu cacheSurfaces=%d verts=%d indexes=%d triangles=%d bytes=%dKB seen/new/gone=%d/%d/%d history=%d/%d dirty=%d validate=%d/%d/%d/%d/%d signatureReuse=%d signatureMs=%d hits=%d misses=%d\n",
        desc.staticBlasCacheHit ? "hit" : "rebuild",
        static_cast<unsigned long long>(desc.staticBlasSignature),
        desc.staticSurfaceCacheSize,
        desc.staticVertexCacheCount,
        desc.staticIndexCacheCount,
        desc.staticTriangleCacheCount,
        desc.staticCacheBytesKB,
        desc.staticSeenThisFrame,
        desc.staticNewThisFrame,
        desc.staticDisappearedThisFrame,
        desc.staticHistoryValid,
        desc.staticPreviousRangeValid,
        desc.staticDirty,
        desc.staticValidationErrors,
        desc.staticRangeErrors,
        desc.staticDuplicateKeys,
        desc.staticHistoryErrors,
        desc.staticKeyVectorMismatches,
        desc.staticBlasSignatureReused ? 1 : 0,
        desc.staticBlasSignatureMs,
        desc.staticBlasCacheHitCount,
        desc.staticBlasCacheMissCount);
    common->Printf("PathTracePrimaryPass: RT smoke material table path=%s cache=%s signature=%llu hits=%d misses=%d materials=%d textures=%d buildMs=%d/%d/%d/%d/%d/%d/%d counts=%d/%d/%d\n",
        desc.materialTablePath ? desc.materialTablePath : "unknown",
        desc.materialTableCacheHit ? "hit" : "rebuild",
        static_cast<unsigned long long>(desc.materialTableSignature),
        desc.materialTableCacheStats.hits,
        desc.materialTableCacheStats.misses,
        static_cast<int>(desc.materialTable->materials.size()),
        static_cast<int>(desc.materialTable->diffuseTextures.size()),
        desc.materialTableBuildStats.entryMs,
        desc.materialTableBuildStats.populateMs,
        desc.materialTableBuildStats.resetMs,
        desc.materialTableBuildStats.diagnosticMs,
        desc.materialTableBuildStats.safeOrderMs,
        desc.materialTableBuildStats.descriptorMs,
        desc.materialTableBuildStats.probeMs,
        desc.materialTableBuildStats.tableMaterials,
        desc.materialTableBuildStats.safeMaterials,
        desc.materialTableBuildStats.descriptorTextures);
    common->Printf("PathTracePrimaryPass: RT smoke material universe records=%d universeMaterials=%d hits=%d misses=%d rebuilds=%d sig=%d frame=%d/%d/%d/%d validation=%d/%d frameValidation=%d/%d\n",
        desc.materialUniverseStats.records,
        desc.materialUniverseStats.universeMaterials,
        desc.materialUniverseStats.hits,
        desc.materialUniverseStats.misses,
        desc.materialUniverseStats.rebuilds,
        desc.materialUniverseStats.signatureChecks,
        desc.materialUniverseStats.frameHits,
        desc.materialUniverseStats.frameMisses,
        desc.materialUniverseStats.frameRebuilds,
        desc.materialUniverseStats.frameSignatureChecks,
        desc.materialUniverseStats.validationChecks,
        desc.materialUniverseStats.validationMismatches,
        desc.materialUniverseStats.frameValidationChecks,
        desc.materialUniverseStats.frameValidationMismatches);
    common->Printf("PathTracePrimaryPass: RT smoke material universe table compare checks=%d mismatches=%d material=%d/%d/%d indexes=%d/%d textures=%d/%d\n",
        desc.materialUniverseTableCompareStats.checks,
        desc.materialUniverseTableCompareStats.mismatches,
        desc.materialUniverseTableCompareStats.materialCountMismatches,
        desc.materialUniverseTableCompareStats.materialIdMismatches,
        desc.materialUniverseTableCompareStats.materialRecordMismatches,
        desc.materialUniverseTableCompareStats.staticIndexMismatches,
        desc.materialUniverseTableCompareStats.dynamicIndexMismatches,
        desc.materialUniverseTableCompareStats.textureCountMismatches,
        desc.materialUniverseTableCompareStats.textureHandleMismatches);
    if (r_pathTracingMatClassEnable.GetInteger() != 0)
    {
        common->Printf("PathTracePrimaryPass: RT smoke material classifier records=%d hits=%d misses=%d rebuilds=%d frame=%d/%d/%d routes(rmao/legacy/fallback)=%d/%d/%d confidence(auth/flag/heur/fallback)=%d/%d/%d/%d compositing(stages/max/>4/>8)=%d/%d/%d/%d ops(opaque/add/mul/invert/over/clip/unknown)=%d/%d/%d/%d/%d/%d/%d\n",
            desc.materialClassifierStats.records,
            desc.materialClassifierStats.hits,
            desc.materialClassifierStats.misses,
            desc.materialClassifierStats.rebuilds,
            desc.materialClassifierStats.frameHits,
            desc.materialClassifierStats.frameMisses,
            desc.materialClassifierStats.frameRebuilds,
            desc.materialClassifierStats.routeRealPbr,
            desc.materialClassifierStats.routeLegacySpec,
            desc.materialClassifierStats.routeFallback,
            desc.materialClassifierStats.confidenceAuthoritative,
            desc.materialClassifierStats.confidenceFlag,
            desc.materialClassifierStats.confidenceHeuristic,
            desc.materialClassifierStats.confidenceFallbackNone,
            desc.materialClassifierStats.compositingStages,
            desc.materialClassifierStats.maxCompositingStages,
            desc.materialClassifierStats.materialsOverFourStages,
            desc.materialClassifierStats.materialsOverEightStages,
            desc.materialClassifierStats.compositingOpaque,
            desc.materialClassifierStats.compositingAdditive,
            desc.materialClassifierStats.compositingMultiply,
            desc.materialClassifierStats.compositingInverted,
            desc.materialClassifierStats.compositingAlphaOver,
            desc.materialClassifierStats.compositingAlphaClip,
            desc.materialClassifierStats.compositingUnknown);
    }
    if (desc.emissiveInventoryStats)
    {
        common->Printf("PathTracePrimaryPass: RT smoke light candidates materials=%d textured=%d untextured=%d bufferBytes=%d uploaded=%d\n",
            desc.emissiveInventoryStats->candidateMaterials,
            desc.emissiveInventoryStats->texturedCandidateMaterials,
            desc.emissiveInventoryStats->untexturedCandidateMaterials,
            desc.lightCandidateBytes,
            desc.lightCandidateBytes > 0 ? 1 : 0);
    }
    LogSmokeMaterialStats(*desc.materialStats);
    LogSmokeMaterialTable(*desc.materialTable);
    if (desc.enableTextureProbe)
    {
        LogSmokeTextureProbe(*desc.materialTable);
        LogSmokeTextureCoverage(*desc.textureCoverageStats);
        LogSmokeMaterialTextureDiscovery(*desc.materialTable);
    }
    LogSmokeAttributeStats(*desc.attributeStats);
    LogSmokeBucketRanges(*desc.bucketRanges);
}

void LogSmokeSceneRebuildSummary(const RtSmokeSceneBuildSummaryLogDesc& desc)
{
    common->Printf("PathTracePrimaryPass: rebuilding RT smoke BLAS/TLAS every frame from current visible Doom surfaces (first frame: %d surfaces, %d verts, %d indexes, anchor triangle %d)\n",
        desc.sourceSurfaces, desc.sourceVerts, desc.sourceIndexes, desc.anchorTriangle);
    common->Printf("PathTracePrimaryPass: RT smoke capture buckets static-world=%d rigid-entity=%d skinned=%d particle/alpha=%d unknown=%d\n",
        desc.classStats.staticWorldSurfaces,
        desc.classStats.rigidEntitySurfaces,
        desc.classStats.skinnedDeformedSurfaces,
        desc.classStats.particleAlphaSurfaces,
        desc.classStats.unknownSurfaces);
    LogSmokeSceneBuildCommonSummary(desc);
}

void LogSmokeSceneCaptureSummary(const RtSmokeSceneBuildSummaryLogDesc& desc)
{
    common->Printf("PathTracePrimaryPass: RT smoke scene capture %d surfaces (dynamic cap %d), %d/%d verts, %d/%d indexes, anchor triangle %d; skipped null=%d geo=%d material=%d gui=%d allowGuiSurfaces=%d space=%d model=%d callback=%d skipCallbacks=%d indexes=%d conditionedOff=%d cache=%d limits=%d zeroArea=%d emptyClass=%d; buckets static-world=%d(%dv/%di/%dt) rigid-entity=%d(%dv/%di/%dt) skinned=%d(%dv/%di/%dt) particle/alpha=%d(%dv/%di/%dt) unknown=%d(%dv/%di/%dt)\n",
        desc.sourceSurfaces, RT_SMOKE_MAX_SURFACES,
        desc.sourceVerts, RT_SMOKE_MAX_VERTS,
        desc.sourceIndexes, RT_SMOKE_MAX_INDEXES,
        desc.anchorTriangle,
        desc.skipStats.nullSurface,
        desc.skipStats.missingGeometry,
        desc.skipStats.nullMaterial,
        desc.skipStats.guiSurface,
        desc.allowGuiSurfaces ? 1 : 0,
        desc.skipStats.nullSpace,
        desc.skipStats.nullModel,
        desc.skipStats.callbackEntity,
        desc.skipCallbackEntities ? 1 : 0,
        desc.skipStats.invalidIndexCount,
        desc.skipStats.conditionedOff,
        desc.skipStats.nonCurrentCache,
        desc.skipStats.limitExceeded,
        desc.skipStats.zeroAreaOnly,
        desc.skipStats.emptyClassBuffer,
        desc.classStats.staticWorldSurfaces, desc.classStats.staticWorldVerts, desc.classStats.staticWorldIndexes, desc.classStats.staticWorldTriangles,
        desc.classStats.rigidEntitySurfaces, desc.classStats.rigidEntityVerts, desc.classStats.rigidEntityIndexes, desc.classStats.rigidEntityTriangles,
        desc.classStats.skinnedDeformedSurfaces, desc.classStats.skinnedDeformedVerts, desc.classStats.skinnedDeformedIndexes, desc.classStats.skinnedDeformedTriangles,
        desc.classStats.particleAlphaSurfaces, desc.classStats.particleAlphaVerts, desc.classStats.particleAlphaIndexes, desc.classStats.particleAlphaTriangles,
        desc.classStats.unknownSurfaces, desc.classStats.unknownVerts, desc.classStats.unknownIndexes, desc.classStats.unknownTriangles);
    LogSmokeSceneBuildCommonSummary(desc);
}

void LogSmokeSurfaceClassReasonSamples(const RtSmokeSurfaceClassReasonSamples& samples)
{
    for (int classIndex = 0; classIndex < RT_SMOKE_CLASS_COUNT; ++classIndex)
    {
        for (int sampleIndex = 0; sampleIndex < samples.counts[classIndex]; ++sampleIndex)
        {
            const RtSmokeSurfaceClassReason& reason = samples.samples[classIndex][sampleIndex];
            if (!reason.valid)
            {
                continue;
            }

            common->Printf("PathTracePrimaryPass: RT smoke class sample %s surf=%d material='%s' coverage=%s sort=%.1f deform=%s entity=%d model='%s' modelDynamic=%s joints(cache=%d staticModel=%d entity=%d) cache(staticV=%d staticI=%d currentV=%d currentI=%d) cpuVerts=%d rtCpuSkin=%d skinnedBasePose=%d worldSpace=%d staticWorldModel=%d entityDef=%d dynModel=%d cachedDyn=%d callback=%d forceUpdate=%d weaponDepthHack=%d modelDepthHack=%.3f verts=%d indexes=%d\n",
                SmokeSurfaceClassName(reason.finalClass),
                reason.surfaceIndex,
                reason.materialName.c_str(),
                SmokeCoverageName(reason.coverage),
                reason.sort,
                SmokeDeformName(reason.deform),
                reason.entityNum,
                reason.modelName.c_str(),
                SmokeDynamicModelName(reason.dynamicModel),
                reason.hasJointCache ? 1 : 0,
                reason.hasStaticModelWithJoints ? 1 : 0,
                reason.hasRenderEntityJoints ? 1 : 0,
                reason.ambientCacheStatic ? 1 : 0,
                reason.indexCacheStatic ? 1 : 0,
                reason.cpuVertexCacheCurrent ? 1 : 0,
                reason.cpuIndexCacheCurrent ? 1 : 0,
                reason.cpuVertsAvailable ? 1 : 0,
                reason.rtCpuSkinned ? 1 : 0,
                reason.skinnedLikelyBasePose ? 1 : 0,
                reason.isWorldSpace ? 1 : 0,
                reason.isStaticWorldModel ? 1 : 0,
                reason.hasEntityDef ? 1 : 0,
                reason.hasDynamicModel ? 1 : 0,
                reason.hasCachedDynamicModel ? 1 : 0,
                reason.hasCallback ? 1 : 0,
                reason.forceUpdate ? 1 : 0,
                reason.weaponDepthHack ? 1 : 0,
                reason.modelDepthHack,
                reason.verts,
                reason.indexes);

            if (reason.finalClass == RtSmokeSurfaceClass::SkinnedDeformed)
            {
                common->Printf("PathTracePrimaryPass: RT smoke skinned detail entity=%d model='%s' origin=(%.2f %.2f %.2f) axis0=(%.3f %.3f %.3f) axis1=(%.3f %.3f %.3f) axis2=(%.3f %.3f %.3f) joints(cache=%d staticModel=%d entityCount=%d) cpuVerts=%d cacheCurrent=(v%d i%d) rtCpuSkin=%d likelyBasePose=%d\n",
                    reason.entityNum,
                    reason.modelName.c_str(),
                    reason.entityOrigin.x, reason.entityOrigin.y, reason.entityOrigin.z,
                    reason.entityAxis[0].x, reason.entityAxis[0].y, reason.entityAxis[0].z,
                    reason.entityAxis[1].x, reason.entityAxis[1].y, reason.entityAxis[1].z,
                    reason.entityAxis[2].x, reason.entityAxis[2].y, reason.entityAxis[2].z,
                    reason.hasJointCache ? 1 : 0,
                    reason.hasStaticModelWithJoints ? 1 : 0,
                    reason.hasRenderEntityJoints ? 1 : 0,
                    reason.cpuVertsAvailable ? 1 : 0,
                    reason.cpuVertexCacheCurrent ? 1 : 0,
                    reason.cpuIndexCacheCurrent ? 1 : 0,
                    reason.rtCpuSkinned ? 1 : 0,
                    reason.skinnedLikelyBasePose ? 1 : 0);
                if (reason.hasEntityBounds)
                {
                    common->Printf("PathTracePrimaryPass: RT smoke skinned bounds entityLocal=[(%.2f %.2f %.2f)..(%.2f %.2f %.2f)]\n",
                        reason.entityBounds[0].x, reason.entityBounds[0].y, reason.entityBounds[0].z,
                        reason.entityBounds[1].x, reason.entityBounds[1].y, reason.entityBounds[1].z);
                }
                if (reason.hasSurfaceBounds)
                {
                    common->Printf("PathTracePrimaryPass: RT smoke skinned bounds surfaceLocal=[(%.2f %.2f %.2f)..(%.2f %.2f %.2f)]\n",
                        reason.surfaceBounds[0].x, reason.surfaceBounds[0].y, reason.surfaceBounds[0].z,
                        reason.surfaceBounds[1].x, reason.surfaceBounds[1].y, reason.surfaceBounds[1].z);
                }
                if (reason.hasReferenceBounds)
                {
                    common->Printf("PathTracePrimaryPass: RT smoke skinned bounds refLocal=[(%.2f %.2f %.2f)..(%.2f %.2f %.2f)] refGlobal=[(%.2f %.2f %.2f)..(%.2f %.2f %.2f)]\n",
                        reason.localReferenceBounds[0].x, reason.localReferenceBounds[0].y, reason.localReferenceBounds[0].z,
                        reason.localReferenceBounds[1].x, reason.localReferenceBounds[1].y, reason.localReferenceBounds[1].z,
                        reason.globalReferenceBounds[0].x, reason.globalReferenceBounds[0].y, reason.globalReferenceBounds[0].z,
                        reason.globalReferenceBounds[1].x, reason.globalReferenceBounds[1].y, reason.globalReferenceBounds[1].z);
                }
            }
        }
    }

    common->Printf("PathTracePrimaryPass: RT smoke focused skinned sample dump count=%d\n", samples.skinnedCount);
    for (int sampleIndex = 0; sampleIndex < samples.skinnedCount; ++sampleIndex)
    {
        const RtSmokeSurfaceClassReason& reason = samples.skinnedSamples[sampleIndex];
        if (!reason.valid)
        {
            continue;
        }

        common->Printf("PathTracePrimaryPass: RT smoke skinned focused surf=%d entity=%d model='%s' material='%s' origin=(%.2f %.2f %.2f) axis0=(%.3f %.3f %.3f) axis1=(%.3f %.3f %.3f) axis2=(%.3f %.3f %.3f) joints(cache=%d staticModel=%d entityCount=%d) cpuVerts=%d cacheCurrent=(v%d i%d) rtCpuSkin=%d likelyBasePose=%d verts=%d indexes=%d\n",
            reason.surfaceIndex,
            reason.entityNum,
            reason.modelName.c_str(),
            reason.materialName.c_str(),
            reason.entityOrigin.x, reason.entityOrigin.y, reason.entityOrigin.z,
            reason.entityAxis[0].x, reason.entityAxis[0].y, reason.entityAxis[0].z,
            reason.entityAxis[1].x, reason.entityAxis[1].y, reason.entityAxis[1].z,
            reason.entityAxis[2].x, reason.entityAxis[2].y, reason.entityAxis[2].z,
            reason.hasJointCache ? 1 : 0,
            reason.hasStaticModelWithJoints ? 1 : 0,
            reason.hasRenderEntityJoints ? 1 : 0,
            reason.cpuVertsAvailable ? 1 : 0,
            reason.cpuVertexCacheCurrent ? 1 : 0,
            reason.cpuIndexCacheCurrent ? 1 : 0,
            reason.rtCpuSkinned ? 1 : 0,
            reason.skinnedLikelyBasePose ? 1 : 0,
            reason.verts,
            reason.indexes);
        if (reason.hasReferenceBounds)
        {
            common->Printf("PathTracePrimaryPass: RT smoke skinned focused bounds refLocal=[(%.2f %.2f %.2f)..(%.2f %.2f %.2f)] refGlobal=[(%.2f %.2f %.2f)..(%.2f %.2f %.2f)]\n",
                reason.localReferenceBounds[0].x, reason.localReferenceBounds[0].y, reason.localReferenceBounds[0].z,
                reason.localReferenceBounds[1].x, reason.localReferenceBounds[1].y, reason.localReferenceBounds[1].z,
                reason.globalReferenceBounds[0].x, reason.globalReferenceBounds[0].y, reason.globalReferenceBounds[0].z,
                reason.globalReferenceBounds[1].x, reason.globalReferenceBounds[1].y, reason.globalReferenceBounds[1].z);
        }
    }
}

void LogSmokeBucketRanges(const RtSmokeBucketRanges& ranges)
{
    common->Printf("PathTracePrimaryPass: RT smoke bucket ranges static-world v[%d+%d] i[%d+%d] t[%d+%d]; rigid-entity v[%d+%d] i[%d+%d] t[%d+%d]; skinned v[%d+%d] i[%d+%d] t[%d+%d]; particle/alpha v[%d+%d] i[%d+%d] t[%d+%d]; unknown v[%d+%d] i[%d+%d] t[%d+%d]\n",
        ranges.buckets[0].vertexOffset, ranges.buckets[0].vertexCount,
        ranges.buckets[0].indexOffset, ranges.buckets[0].indexCount,
        ranges.buckets[0].triangleOffset, ranges.buckets[0].triangleCount,
        ranges.buckets[1].vertexOffset, ranges.buckets[1].vertexCount,
        ranges.buckets[1].indexOffset, ranges.buckets[1].indexCount,
        ranges.buckets[1].triangleOffset, ranges.buckets[1].triangleCount,
        ranges.buckets[2].vertexOffset, ranges.buckets[2].vertexCount,
        ranges.buckets[2].indexOffset, ranges.buckets[2].indexCount,
        ranges.buckets[2].triangleOffset, ranges.buckets[2].triangleCount,
        ranges.buckets[3].vertexOffset, ranges.buckets[3].vertexCount,
        ranges.buckets[3].indexOffset, ranges.buckets[3].indexCount,
        ranges.buckets[3].triangleOffset, ranges.buckets[3].triangleCount,
        ranges.buckets[4].vertexOffset, ranges.buckets[4].vertexCount,
        ranges.buckets[4].indexOffset, ranges.buckets[4].indexCount,
        ranges.buckets[4].triangleOffset, ranges.buckets[4].triangleCount);
}

void LogSmokeAttributeStats(const RtSmokeAttributeStats& stats)
{
    common->Printf("PathTracePrimaryPass: RT smoke attribute validation invalidNormals static=%dv/%dt rigid=%dv/%dt skinned=%dv/%dt particle/alpha=%dv/%dt unknown=%dv/%dt; invalidUVs static=%dv/%dt rigid=%dv/%dt skinned=%dv/%dt particle/alpha=%dv/%dt unknown=%dv/%dt; geomNormalFallback static=%dt rigid=%dt skinned=%dt particle/alpha=%dt unknown=%dt\n",
        stats.classes[0].invalidNormalVerts, stats.classes[0].invalidNormalTriangles,
        stats.classes[1].invalidNormalVerts, stats.classes[1].invalidNormalTriangles,
        stats.classes[2].invalidNormalVerts, stats.classes[2].invalidNormalTriangles,
        stats.classes[3].invalidNormalVerts, stats.classes[3].invalidNormalTriangles,
        stats.classes[4].invalidNormalVerts, stats.classes[4].invalidNormalTriangles,
        stats.classes[0].invalidUvVerts, stats.classes[0].invalidUvTriangles,
        stats.classes[1].invalidUvVerts, stats.classes[1].invalidUvTriangles,
        stats.classes[2].invalidUvVerts, stats.classes[2].invalidUvTriangles,
        stats.classes[3].invalidUvVerts, stats.classes[3].invalidUvTriangles,
        stats.classes[4].invalidUvVerts, stats.classes[4].invalidUvTriangles,
        stats.classes[0].forcedGeometricNormalTriangles,
        stats.classes[1].forcedGeometricNormalTriangles,
        stats.classes[2].forcedGeometricNormalTriangles,
        stats.classes[3].forcedGeometricNormalTriangles,
        stats.classes[4].forcedGeometricNormalTriangles);
}

void LogSmokeMaterialStats(const RtSmokeMaterialStats& stats)
{
    common->Printf("PathTracePrimaryPass: RT smoke materials unique=%d surfaces=%d triangles=%d translucentUnique=%d translucentSurfaces=%d translucentTriangles=%d samples=",
        stats.uniqueMaterials,
        stats.totalSurfaces,
        stats.totalTriangles,
        stats.translucentUniqueMaterials,
        stats.translucentSurfaces,
        stats.translucentTriangles);

    for (int sampleIndex = 0; sampleIndex < stats.sampleCount; ++sampleIndex)
    {
        const RtSmokeMaterialSample& sample = stats.samples[sampleIndex];
        common->Printf("%s%s(id=%u surfaces=%d triangles=%d)",
            sampleIndex == 0 ? "" : ", ",
            sample.name.c_str(),
            sample.id,
            sample.surfaces,
            sample.triangles);
    }

    common->Printf("\n");

    if (stats.dynamicEvalSurfaces > 0 ||
        stats.dynamicEvalNoRegisterSurfaces > 0 ||
        stats.dynamicEvalNoSelectedStageSurfaces > 0)
    {
        common->Printf("PathTracePrimaryPass: RT smoke dynamic material eval surfaces=%d triangles=%d stages=%d enabled=%d disabled=%d color=%d alpha=%d alphaTest=%d texMatrix=%d dynamicImage=%d cinematic=%d guiRender=%d program=%d noRegs=%d noStage=%d materialSamples=%d samples=",
            stats.dynamicEvalSurfaces,
            stats.dynamicEvalTriangles,
            stats.dynamicEvalStages,
            stats.dynamicEvalEnabledStages,
            stats.dynamicEvalDisabledStages,
            stats.dynamicEvalColorStages,
            stats.dynamicEvalAlphaStages,
            stats.dynamicEvalAlphaTestStages,
            stats.dynamicEvalTexMatrixStages,
            stats.dynamicEvalDynamicImageStages,
            stats.dynamicEvalCinematicStages,
            stats.dynamicEvalGuiRenderTargetStages,
            stats.dynamicEvalProgramStages,
            stats.dynamicEvalNoRegisterSurfaces,
            stats.dynamicEvalNoSelectedStageSurfaces,
            static_cast<int>(stats.dynamicEvalMaterialSamples.size()));

        for (int sampleIndex = 0; sampleIndex < stats.dynamicEvalSampleCount; ++sampleIndex)
        {
            const RtSmokeDynamicMaterialEvalSample& sample = stats.dynamicEvalSamples[sampleIndex];
            common->Printf("%s%s(id=%u surfaces=%d triangles=%d stage=%d priority=%d selectedEmissive=%d enabled=%d disabled=%d colorStages=%d alphaStages=%d alphaTestStages=%d texMatrixStages=%d dynamicImageStages=%d cinematicStages=%d guiRenderStages=%d programStages=%d condition=%.3f color=(%.3f %.3f %.3f %.3f) alphaTest=%.3f texMatrix=((%.3f %.3f %.3f)(%.3f %.3f %.3f)))",
                sampleIndex == 0 ? "" : ", ",
                sample.name.c_str(),
                sample.id,
                sample.surfaces,
                sample.triangles,
                sample.stageIndex,
                sample.stagePriority,
                sample.selectedStageEmissive ? 1 : 0,
                sample.enabledStages,
                sample.disabledStages,
                sample.colorStages,
                sample.alphaStages,
                sample.alphaTestStages,
                sample.texMatrixStages,
                sample.dynamicImageStages,
                sample.cinematicStages,
                sample.guiRenderTargetStages,
                sample.programStages,
                sample.condition,
                sample.color[0],
                sample.color[1],
                sample.color[2],
                sample.color[3],
                sample.alphaTest,
                sample.texMatrix[0][0],
                sample.texMatrix[0][1],
                sample.texMatrix[0][2],
                sample.texMatrix[1][0],
                sample.texMatrix[1][1],
                sample.texMatrix[1][2]);
        }
        common->Printf("\n");
    }

    if (stats.translucentSampleCount <= 0)
    {
        return;
    }

    common->Printf("PathTracePrimaryPass: RT smoke translucent material samples=");
    for (int sampleIndex = 0; sampleIndex < stats.translucentSampleCount; ++sampleIndex)
    {
        const RtSmokeMaterialSample& sample = stats.translucentSamples[sampleIndex];
        common->Printf("%s%s(id=%u surfaces=%d triangles=%d)",
            sampleIndex == 0 ? "" : ", ",
            sample.name.c_str(),
            sample.id,
            sample.surfaces,
            sample.triangles);
    }
    common->Printf("\n");

    common->Printf("PathTracePrimaryPass: RT smoke translucent subtype counts");
    for (int subtypeIndex = 0; subtypeIndex < RT_SMOKE_TRANSLUCENT_SUBTYPE_COUNT; ++subtypeIndex)
    {
        common->Printf(" %s=%d(%dt)",
            SmokeTranslucentSubtypeNameByIndex(subtypeIndex),
            stats.translucentSubtypeSurfaces[subtypeIndex],
            stats.translucentSubtypeTriangles[subtypeIndex]);
    }
    common->Printf("\n");

    for (int subtypeIndex = 0; subtypeIndex < RT_SMOKE_TRANSLUCENT_SUBTYPE_COUNT; ++subtypeIndex)
    {
        const int sampleCount = stats.translucentSubtypeSampleCounts[subtypeIndex];
        if (sampleCount <= 0)
        {
            continue;
        }

        common->Printf("PathTracePrimaryPass: RT smoke translucent subtype %s samples=",
            SmokeTranslucentSubtypeNameByIndex(subtypeIndex));
        for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
        {
            const RtSmokeMaterialSample& sample = stats.translucentSubtypeSamples[subtypeIndex][sampleIndex];
            common->Printf("%s%s(id=%u surfaces=%d triangles=%d)",
                sampleIndex == 0 ? "" : ", ",
                sample.name.c_str(),
                sample.id,
                sample.surfaces,
                sample.triangles);
        }
        common->Printf("\n");
    }
}

void LogSmokeTranslucentSubtypeDump(const RtSmokeMaterialStats& stats)
{
    common->Printf("PathTracePrimaryPass: RT smoke translucent subtype dump samples=%d\n",
        stats.translucentDebugSampleCount);

    for (int sampleIndex = 0; sampleIndex < stats.translucentDebugSampleCount; ++sampleIndex)
    {
        const RtSmokeTranslucentSubtypeDebugSample& sample = stats.translucentDebugSamples[sampleIndex];
        if (!sample.valid)
        {
            continue;
        }

        const RtSmokeTranslucentClassifierInfo& info = sample.info;
        common->Printf("PathTracePrimaryPass: RT smoke translucent sample surf=%d subtype=%s material='%s' coverage=%s sort=%.2f deform=%s verts=%d indexes=%d flags guiSort=%d decalSort=%d postSort=%d polyOffset=%d screenTex=%d addDefault0200=%d addBlend=%d ambient=%d ambientBlend=%d diffuse=%d nameGui=%d nameParticle=%d nameDecal=%d nameGlass=%d nameGlow=%d nameSignage=%d\n",
            sample.surfaceIndex,
            SmokeTranslucentSubtypeName(sample.subtype),
            sample.materialName.c_str(),
            SmokeCoverageName(sample.coverage),
            sample.sort,
            SmokeDeformName(sample.deform),
            sample.verts,
            sample.indexes,
            info.sortIsGuiOrSubview ? 1 : 0,
            info.sortIsDecal ? 1 : 0,
            info.sortIsPostProcess ? 1 : 0,
            info.polygonOffsetDecal ? 1 : 0,
            info.hasScreenTexgen ? 1 : 0,
            info.hasAddDefault0200Texture ? 1 : 0,
            info.hasAdditiveBlend ? 1 : 0,
            info.hasAmbientStage ? 1 : 0,
            info.hasAmbientBlendStage ? 1 : 0,
            info.hasDiffuseStage ? 1 : 0,
            info.nameLooksGui ? 1 : 0,
            info.nameLooksParticle ? 1 : 0,
            info.nameLooksDecal ? 1 : 0,
            info.nameLooksGlass ? 1 : 0,
            info.nameLooksGlow ? 1 : 0,
            info.nameLooksSignage ? 1 : 0);
    }
}

void LogSmokeMaterialTable(const RtSmokeMaterialTableBuild& table)
{
    const int materialTableCount = Min(static_cast<int>(table.materialIds.size()), static_cast<int>(table.materials.size()));
    common->Printf("PathTracePrimaryPass: RT smoke material table entries=%d staticTriangles=%d dynamicTriangles=%d textureSlots=%d textured=%d normalTextured=%d specularTextured=%d emissive=%d emissiveTextured=%d alphaTested=%d alphaTextured=%d guiTextures=%d/%d/%d allowGui=%d additiveDecals=%d missingTextures=%d rejectedTextures=%d finalRejected=%d descriptorFallbacks=%d overTextureSlotLimit=%d probeRequest=%d probeBound=%d probeMaterial=%u latch=%d samples=",
        materialTableCount,
        static_cast<int>(table.staticMaterialIndexes.size()),
        static_cast<int>(table.dynamicMaterialIndexes.size()),
        static_cast<int>(table.diffuseTextures.size()),
        table.materialsWithTextures,
        table.materialsWithNormalTextures,
        table.materialsWithSpecularTextures,
        table.materialsEmissive,
        table.materialsWithEmissiveTextures,
        table.materialsAlphaTested,
        table.materialsWithAlphaTextures,
        table.guiTextureCandidates,
        table.guiTexturesAccepted,
        table.guiTexturesRejected,
        r_pathTracingAllowGuiTextures.GetInteger() != 0 ? 1 : 0,
        table.materialsAdditiveDecals,
        table.materialsMissingTextures,
        table.materialsRejectedTextures,
        table.materialsRejectedAtFinalCheck,
        table.descriptorsReplacedWithFallback,
        table.materialsOverTextureSlotLimit,
        table.textureProbeRequestedIndex,
        table.textureProbeBoundIndex,
        table.textureProbeBoundMaterialId,
        table.textureProbeUsedLatch ? 1 : 0);

    const int sampleCount = idMath::ClampInt(0, RT_SMOKE_DEBUG_MATERIAL_REASON_SAMPLES, materialTableCount);
    for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
    {
        const PathTraceSmokeMaterial& material = table.materials[sampleIndex];
        common->Printf("%sindex=%d id=%u color=(%.2f %.2f %.2f) emissive=(%.2f %.2f %.2f) tex=%d normalTex=%d specTex=%d emissiveTex=%d alphaTex=%d alpha=%d additive=%d emissive=%d cutoff=%.2f",
            sampleIndex == 0 ? "" : ", ",
            sampleIndex,
            table.materialIds[sampleIndex],
            material.debugAlbedo[0],
            material.debugAlbedo[1],
            material.debugAlbedo[2],
            material.emissiveColor[0],
            material.emissiveColor[1],
            material.emissiveColor[2],
            material.diffuseTextureIndex == UINT32_MAX ? -1 : static_cast<int>(material.diffuseTextureIndex),
            material.normalTextureIndex == UINT32_MAX ? -1 : static_cast<int>(material.normalTextureIndex),
            material.specularTextureIndex == UINT32_MAX ? -1 : static_cast<int>(material.specularTextureIndex),
            material.emissiveTextureIndex == UINT32_MAX ? -1 : static_cast<int>(material.emissiveTextureIndex),
            material.alphaTextureIndex == UINT32_MAX ? -1 : static_cast<int>(material.alphaTextureIndex),
            (material.flags & RT_SMOKE_MATERIAL_ALPHA_TEST) != 0 ? 1 : 0,
            (material.flags & RT_SMOKE_MATERIAL_ADDITIVE_DECAL) != 0 ? 1 : 0,
            (material.flags & RT_SMOKE_MATERIAL_EMISSIVE) != 0 ? 1 : 0,
            material.alphaCutoff);
    }

    common->Printf("\n");

    int dynamicRecordCount = 0;
    int runtimeRegCount = 0;
    int colorCount = 0;
    int alphaCount = 0;
    int conditionCount = 0;
    int texMatrixCount = 0;
    int videoCount = 0;
    int decalCount = 0;
    int guiRenderCount = 0;
    int programCount = 0;
    int flipbookCount = 0;
    for (int materialIndex = 0; materialIndex < materialTableCount; ++materialIndex)
    {
        const PathTraceSmokeMaterial& material = table.materials[materialIndex];
        const uint32_t dynamicFlags = material.padding0 & (
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_RUNTIME_REGS |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_COLOR |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_ALPHA |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_CONDITION |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_TEX_MATRIX |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_VIDEO |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_DECAL |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_GUI_RENDER |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_PROGRAM |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_FLIPBOOK);
        if (dynamicFlags == 0u)
        {
            continue;
        }

        ++dynamicRecordCount;
        runtimeRegCount += (dynamicFlags & RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_RUNTIME_REGS) != 0u ? 1 : 0;
        colorCount += (dynamicFlags & RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_COLOR) != 0u ? 1 : 0;
        alphaCount += (dynamicFlags & RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_ALPHA) != 0u ? 1 : 0;
        conditionCount += (dynamicFlags & RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_CONDITION) != 0u ? 1 : 0;
        texMatrixCount += (dynamicFlags & RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_TEX_MATRIX) != 0u ? 1 : 0;
        videoCount += (dynamicFlags & RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_VIDEO) != 0u ? 1 : 0;
        decalCount += (dynamicFlags & RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_DECAL) != 0u ? 1 : 0;
        guiRenderCount += (dynamicFlags & RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_GUI_RENDER) != 0u ? 1 : 0;
        programCount += (dynamicFlags & RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_PROGRAM) != 0u ? 1 : 0;
        flipbookCount += (dynamicFlags & RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_FLIPBOOK) != 0u ? 1 : 0;
    }

    if (dynamicRecordCount <= 0)
    {
        return;
    }

    common->Printf("PathTracePrimaryPass: RT smoke material table dynamic flags records=%d runtimeRegs=%d color=%d alpha=%d condition=%d texMatrix=%d video=%d decal=%d guiRender=%d program=%d flipbook=%d samples=",
        dynamicRecordCount,
        runtimeRegCount,
        colorCount,
        alphaCount,
        conditionCount,
        texMatrixCount,
        videoCount,
        decalCount,
        guiRenderCount,
        programCount,
        flipbookCount);

    int dynamicSampleCount = 0;
    for (int materialIndex = 0; materialIndex < materialTableCount && dynamicSampleCount < RT_SMOKE_DEBUG_MATERIAL_REASON_SAMPLES; ++materialIndex)
    {
        const PathTraceSmokeMaterial& material = table.materials[materialIndex];
        const uint32_t dynamicFlags = material.padding0 & (
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_RUNTIME_REGS |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_COLOR |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_ALPHA |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_CONDITION |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_TEX_MATRIX |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_VIDEO |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_DECAL |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_GUI_RENDER |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_PROGRAM |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_FLIPBOOK);
        if (dynamicFlags == 0u)
        {
            continue;
        }

        const RtSmokeMaterialTextureInfo* info = materialIndex < static_cast<int>(table.materialInfos.size()) ? &table.materialInfos[materialIndex] : nullptr;
        common->Printf("%sindex=%d id=%u material='%s' flags=0x%x regs=%d color=%d alpha=%d condition=%d texMatrix=%d video=%d decal=%d guiRender=%d program=%d flipbook=%d",
            dynamicSampleCount == 0 ? "" : ", ",
            materialIndex,
            table.materialIds[materialIndex],
            info ? info->materialName.c_str() : "<unknown>",
            dynamicFlags,
            (dynamicFlags & RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_RUNTIME_REGS) != 0u ? 1 : 0,
            (dynamicFlags & RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_COLOR) != 0u ? 1 : 0,
            (dynamicFlags & RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_ALPHA) != 0u ? 1 : 0,
            (dynamicFlags & RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_CONDITION) != 0u ? 1 : 0,
            (dynamicFlags & RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_TEX_MATRIX) != 0u ? 1 : 0,
            (dynamicFlags & RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_VIDEO) != 0u ? 1 : 0,
            (dynamicFlags & RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_DECAL) != 0u ? 1 : 0,
            (dynamicFlags & RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_GUI_RENDER) != 0u ? 1 : 0,
            (dynamicFlags & RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_PROGRAM) != 0u ? 1 : 0,
            (dynamicFlags & RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_FLIPBOOK) != 0u ? 1 : 0);
        ++dynamicSampleCount;
    }
    common->Printf("\n");
}

void LogSmokeTextureProbe(const RtSmokeMaterialTableBuild& table)
{
    const int materialTableCount = Min(static_cast<int>(table.materialIds.size()), static_cast<int>(table.materials.size()));
    const int textureTableLimit = GetSmokeTextureTableEffectiveLimit();
    const int requestedTextureTableLimit = GetSmokeTextureTableRequestedLimit();
    const int textureTableStart = Max(0, r_pathTracingTextureTableStart.GetInteger());
    const int textureSampleMethod = r_pathTracingTextureSampleEnable.GetInteger() != 0 ? idMath::ClampInt(0, 2, r_pathTracingTextureSampleMethod.GetInteger()) : 0;
    const bool textureBindlessEnabled = r_pathTracingTextureBindlessEnable.GetInteger() != 0;
    common->Printf("PathTracePrimaryPass: RT smoke bindless texture table occupancy=%d/%d requested=%d start=%d capacity=%d activeCap=%d sampleMethod=%d bindless=%d texturedMaterials=%d normalTextured=%d specularTextured=%d emissive=%d emissiveTextured=%d alphaTested=%d alphaTextured=%d guiTextures=%d/%d/%d allowGui=%d missing=%d rejected=%d finalRejected=%d descriptorFallbacks=%d skippedOrOverLimit=%d\n",
        static_cast<int>(table.diffuseTextures.size()),
        textureTableLimit,
        requestedTextureTableLimit,
        textureTableStart,
        RT_SMOKE_TEXTURE_DESCRIPTOR_CAPACITY,
        RT_SMOKE_TEXTURE_EXPERIMENTAL_ACTIVE_CAP,
        textureSampleMethod,
        textureBindlessEnabled ? 1 : 0,
        table.materialsWithTextures,
        table.materialsWithNormalTextures,
        table.materialsWithSpecularTextures,
        table.materialsEmissive,
        table.materialsWithEmissiveTextures,
        table.materialsAlphaTested,
        table.materialsWithAlphaTextures,
        table.guiTextureCandidates,
        table.guiTexturesAccepted,
        table.guiTexturesRejected,
        r_pathTracingAllowGuiTextures.GetInteger() != 0 ? 1 : 0,
        table.materialsMissingTextures,
        table.materialsRejectedTextures,
        table.materialsRejectedAtFinalCheck,
        table.descriptorsReplacedWithFallback,
        table.materialsOverTextureSlotLimit);

    if (table.textureProbeBoundIndex < 0 || table.textureProbeBoundIndex >= materialTableCount)
    {
        common->Printf("PathTracePrimaryPass: RT smoke texture probe no bound texture request=%d tableEntries=%d\n",
            table.textureProbeRequestedIndex,
            materialTableCount);
        return;
    }

    const RtSmokeMaterialTextureInfo info = ResolveSmokeMaterialTextureInfo(table.materialIds[table.textureProbeBoundIndex], table.textureProbeBoundIndex);
    const PathTraceSmokeMaterial& material = table.materials[table.textureProbeBoundIndex];
    nvrhi::TextureHandle texture = info.diffuseImage ? info.diffuseImage->GetTextureHandle() : nullptr;
    if (!texture)
    {
        common->Printf("PathTracePrimaryPass: RT smoke texture probe missing bound texture index=%d material='%s'\n",
            table.textureProbeBoundIndex,
            info.materialName.c_str());
        return;
    }

    const nvrhi::TextureDesc& desc = texture->getDesc();
    common->Printf("PathTracePrimaryPass: RT smoke texture probe slot=%d tableIndex=%d material='%s' diffuse='%s' format=%d size=%ux%u mips=%u reason='%s'\n",
        material.diffuseTextureIndex == UINT32_MAX ? -1 : static_cast<int>(material.diffuseTextureIndex),
        table.textureProbeBoundIndex,
        info.materialName.c_str(),
        info.diffuseImageName.c_str(),
        static_cast<int>(desc.format),
        desc.width,
        desc.height,
        desc.mipLevels,
        info.fallbackReason.c_str());
}

void LogSmokeTextureProbeSwitch(const RtSmokeMaterialTableBuild& table)
{
    const int materialTableCount = Min(static_cast<int>(table.materialIds.size()), static_cast<int>(table.materials.size()));
    if (table.textureProbeBoundIndex < 0 || table.textureProbeBoundIndex >= materialTableCount)
    {
        common->Printf("PathTracePrimaryPass: RT smoke texture probe unbound request=%d latchedMaterial=%u visible=0\n",
            table.textureProbeRequestedIndex,
            table.textureProbeBoundMaterialId);
        return;
    }

    const RtSmokeMaterialTextureInfo info = ResolveSmokeMaterialTextureInfo(table.materialIds[table.textureProbeBoundIndex], table.textureProbeBoundIndex);
    const PathTraceSmokeMaterial& material = table.materials[table.textureProbeBoundIndex];
    common->Printf("PathTracePrimaryPass: RT smoke texture probe latched slot=%d tableIndex=%d materialId=%u material='%s' diffuse='%s' latch=%d\n",
        material.diffuseTextureIndex == UINT32_MAX ? -1 : static_cast<int>(material.diffuseTextureIndex),
        table.textureProbeBoundIndex,
        table.textureProbeBoundMaterialId,
        info.materialName.c_str(),
        info.diffuseImageName.c_str(),
        table.textureProbeUsedLatch ? 1 : 0);
}

void LogSmokeAlphaMaterialDump(const RtSmokeMaterialTableBuild& table)
{
    const int materialTableCount = Min(static_cast<int>(table.materialIds.size()), static_cast<int>(table.materials.size()));
    int alphaMaterialCount = 0;
    for (int tableIndex = 0; tableIndex < materialTableCount; ++tableIndex)
    {
        if ((table.materials[tableIndex].flags & RT_SMOKE_MATERIAL_ALPHA_TEST) != 0)
        {
            ++alphaMaterialCount;
        }
    }

    const int maxLogged = RT_SMOKE_TEXTURE_PROBE_DUMP_CANDIDATES;
    common->Printf("PathTracePrimaryPass: RT smoke alpha material dump entries=%d alphaTested=%d slots=%d tableLimit=%d start=%d sampleMethod=%d bindless=%d\n",
        materialTableCount,
        alphaMaterialCount,
        static_cast<int>(table.diffuseTextures.size()),
        GetSmokeTextureTableEffectiveLimit(),
        Max(0, r_pathTracingTextureTableStart.GetInteger()),
        r_pathTracingTextureSampleEnable.GetInteger() != 0 ? idMath::ClampInt(0, 2, r_pathTracingTextureSampleMethod.GetInteger()) : 0,
        r_pathTracingTextureBindlessEnable.GetInteger() != 0 ? 1 : 0);

    int logged = 0;
    for (int tableIndex = 0; tableIndex < materialTableCount && logged < maxLogged; ++tableIndex)
    {
        const PathTraceSmokeMaterial& material = table.materials[tableIndex];
        if ((material.flags & RT_SMOKE_MATERIAL_ALPHA_TEST) == 0)
        {
            continue;
        }

        const RtSmokeMaterialTextureInfo info = ResolveSmokeMaterialTextureInfo(table.materialIds[tableIndex], tableIndex);
        common->Printf("PathTracePrimaryPass: RT smoke alpha material index=%d id=%u material='%s' coverage=%s cutoff=%.3f diffuse='%s' diffuseSlot=%d diffuseSize=%ux%u alpha='%s' alphaSlot=%d alphaSize=%ux%u alphaReason='%s' diffuseReason='%s' safeDiffuse=%d safeAlpha=%d\n",
            tableIndex,
            table.materialIds[tableIndex],
            info.materialName.c_str(),
            SmokeCoverageName(info.coverage),
            material.alphaCutoff,
            info.diffuseImageName.c_str(),
            material.diffuseTextureIndex == UINT32_MAX ? -1 : static_cast<int>(material.diffuseTextureIndex),
            material.textureWidth,
            material.textureHeight,
            info.alphaImageName.c_str(),
            material.alphaTextureIndex == UINT32_MAX ? -1 : static_cast<int>(material.alphaTextureIndex),
            material.alphaTextureWidth,
            material.alphaTextureHeight,
            info.alphaReason.c_str(),
            info.fallbackReason.c_str(),
            info.hasSafeTexture ? 1 : 0,
            info.hasSafeAlphaTexture ? 1 : 0);
        ++logged;
    }

    if (logged == 0)
    {
        common->Printf("PathTracePrimaryPass: RT smoke alpha material dump found no alpha-tested materials in current capture\n");
    }
    else if (logged < alphaMaterialCount)
    {
        common->Printf("PathTracePrimaryPass: RT smoke alpha material dump truncated logged=%d total=%d\n",
            logged,
            alphaMaterialCount);
    }
}

void LogSmokeTextureProbeDump(const RtSmokeMaterialTableBuild& table)
{
    const int materialTableCount = Min(static_cast<int>(table.materialIds.size()), static_cast<int>(table.materials.size()));
    const int textureTableLimit = GetSmokeTextureTableEffectiveLimit();
    const int requestedTextureTableLimit = GetSmokeTextureTableRequestedLimit();
    const int textureTableStart = Max(0, r_pathTracingTextureTableStart.GetInteger());
    const int textureSampleMethod = r_pathTracingTextureSampleEnable.GetInteger() != 0 ? idMath::ClampInt(0, 2, r_pathTracingTextureSampleMethod.GetInteger()) : 0;
    const bool textureBindlessEnabled = r_pathTracingTextureBindlessEnable.GetInteger() != 0;
    common->Printf("PathTracePrimaryPass: RT smoke texture probe dump request=%d bound=%d materialId=%u slots=%d/%d requested=%d start=%d capacity=%d activeCap=%d sampleMethod=%d bindless=%d texturedMaterials=%d alphaTested=%d alphaTextured=%d missing=%d rejected=%d finalRejected=%d descriptorFallbacks=%d skippedOrOverLimit=%d\n",
        table.textureProbeRequestedIndex,
        table.textureProbeBoundIndex,
        table.textureProbeBoundMaterialId,
        static_cast<int>(table.diffuseTextures.size()),
        textureTableLimit,
        requestedTextureTableLimit,
        textureTableStart,
        RT_SMOKE_TEXTURE_DESCRIPTOR_CAPACITY,
        RT_SMOKE_TEXTURE_EXPERIMENTAL_ACTIVE_CAP,
        textureSampleMethod,
        textureBindlessEnabled ? 1 : 0,
        table.materialsWithTextures,
        table.materialsAlphaTested,
        table.materialsWithAlphaTextures,
        table.materialsMissingTextures,
        table.materialsRejectedTextures,
        table.materialsRejectedAtFinalCheck,
        table.descriptorsReplacedWithFallback,
        table.materialsOverTextureSlotLimit);

    if (table.textureProbeBoundIndex >= 0 && table.textureProbeBoundIndex < materialTableCount)
    {
        const RtSmokeMaterialTextureInfo info = ResolveSmokeMaterialTextureInfo(table.materialIds[table.textureProbeBoundIndex], table.textureProbeBoundIndex);
        const PathTraceSmokeMaterial& material = table.materials[table.textureProbeBoundIndex];
        common->Printf("PathTracePrimaryPass: RT smoke texture probe current index=%d slot=%d material='%s' diffuse='%s' safe=%d reason='%s'\n",
            table.textureProbeBoundIndex,
            material.diffuseTextureIndex == UINT32_MAX ? -1 : static_cast<int>(material.diffuseTextureIndex),
            info.materialName.c_str(),
            info.diffuseImageName.c_str(),
            info.hasSafeTexture ? 1 : 0,
            info.fallbackReason.c_str());
        common->Printf("PathTracePrimaryPass: RT smoke texture probe alpha current slot=%d alpha='%s' safe=%d reason='%s'\n",
            material.alphaTextureIndex == UINT32_MAX ? -1 : static_cast<int>(material.alphaTextureIndex),
            info.alphaImageName.c_str(),
            info.hasSafeAlphaTexture ? 1 : 0,
            info.alphaReason.c_str());
    }
    else
    {
        common->Printf("PathTracePrimaryPass: RT smoke texture probe current none\n");
    }

    common->Printf("PathTracePrimaryPass: RT smoke texture probe sampled entries=");
    int logged = 0;
    for (int tableIndex = 0; tableIndex < materialTableCount && logged < RT_SMOKE_TEXTURE_PROBE_CANDIDATE_SAMPLES; ++tableIndex)
    {
        const PathTraceSmokeMaterial& material = table.materials[tableIndex];
        if (material.diffuseTextureIndex == UINT32_MAX)
        {
            continue;
        }

        const RtSmokeMaterialTextureInfo info = ResolveSmokeMaterialTextureInfo(table.materialIds[tableIndex], tableIndex);
        common->Printf("%s%d:slot%d '%s' -> '%s'",
            logged == 0 ? "" : ", ",
            tableIndex,
            static_cast<int>(material.diffuseTextureIndex),
            info.materialName.c_str(),
            info.diffuseImageName.c_str());
        ++logged;
    }
    if (logged == 0)
    {
        common->Printf("<none>");
    }
    common->Printf("\n");

    const int dumpStart = Max(0, r_pathTracingTextureProbeDumpStart.GetInteger());
    const int dumpCount = idMath::ClampInt(1, RT_SMOKE_TEXTURE_PROBE_DUMP_CANDIDATES, r_pathTracingTextureProbeDumpCount.GetInteger());
    common->Printf("PathTracePrimaryPass: RT smoke texture probe safe candidate order page start=%d count=%d\n", dumpStart, dumpCount);
    const std::vector<int> safeMaterialIndexes = BuildSmokeSafeMaterialIndexOrder(table);
    int loggedCandidates = 0;
    for (int candidateIndex = 0; candidateIndex < static_cast<int>(safeMaterialIndexes.size()); ++candidateIndex)
    {
        if (candidateIndex < dumpStart)
        {
            continue;
        }
        if (loggedCandidates >= dumpCount)
        {
            continue;
        }

        const int tableIndex = safeMaterialIndexes[candidateIndex];
        if (!SmokeMaterialTableIndexIsValid(table, tableIndex))
        {
            continue;
        }
        const RtSmokeMaterialTextureInfo info = ResolveSmokeMaterialTextureInfo(table.materialIds[tableIndex], tableIndex);
        const PathTraceSmokeMaterial& material = table.materials[tableIndex];
        const bool sampled = material.diffuseTextureIndex != UINT32_MAX;
        common->Printf("PathTracePrimaryPass: RT smoke texture candidate %d table=%d slot=%d alphaSlot=%d sampled=%d material='%s' diffuse='%s' alpha='%s' reason='%s' alphaReason='%s'\n",
            candidateIndex,
            tableIndex,
            sampled ? static_cast<int>(material.diffuseTextureIndex) : -1,
            material.alphaTextureIndex == UINT32_MAX ? -1 : static_cast<int>(material.alphaTextureIndex),
            sampled ? 1 : 0,
            info.materialName.c_str(),
            info.diffuseImageName.c_str(),
            info.alphaImageName.c_str(),
            info.fallbackReason.c_str(),
            info.alphaReason.c_str());
        ++loggedCandidates;
    }
    if (loggedCandidates == 0)
    {
        common->Printf("PathTracePrimaryPass: RT smoke texture candidate page empty\n");
    }
    common->Printf("PathTracePrimaryPass: RT smoke texture candidate total=%d\n", static_cast<int>(safeMaterialIndexes.size()));
}

void LogSmokeTextureActiveWindow(const RtSmokeMaterialTableBuild& table)
{
    const int textureTableLimit = GetSmokeTextureTableEffectiveLimit();
    if (textureTableLimit <= 0)
    {
        return;
    }

    const int textureTableStart = Max(0, r_pathTracingTextureTableStart.GetInteger());
    const bool forceFallback = r_pathTracingTextureForceFallback.GetInteger() != 0;
    const int textureSampleMethod = r_pathTracingTextureSampleEnable.GetInteger() != 0 ? idMath::ClampInt(0, 2, r_pathTracingTextureSampleMethod.GetInteger()) : 0;
    const bool textureBindlessEnabled = r_pathTracingTextureBindlessEnable.GetInteger() != 0;
    uint32_t activeHash = 2166136261u;
    int sampledCount = 0;
    const int materialTableCount = Min(static_cast<int>(table.materialIds.size()), static_cast<int>(table.materials.size()));
    for (int tableIndex = 0; tableIndex < materialTableCount; ++tableIndex)
    {
        const PathTraceSmokeMaterial& material = table.materials[tableIndex];
        if (material.diffuseTextureIndex == UINT32_MAX)
        {
            continue;
        }
        activeHash = (activeHash ^ table.materialIds[tableIndex]) * 16777619u;
        activeHash = (activeHash ^ material.diffuseTextureIndex) * 16777619u;
        ++sampledCount;
    }

    static int lastLoggedStart = -1;
    static int lastLoggedLimit = -1;
    static int lastLoggedSampledCount = -1;
    static int lastLoggedForceFallback = -1;
    static int lastLoggedSampleMethod = -1;
    static int lastLoggedBindlessEnabled = -1;
    static uint32_t lastLoggedHash = 0;
    if (lastLoggedStart == textureTableStart &&
        lastLoggedLimit == textureTableLimit &&
        lastLoggedSampledCount == sampledCount &&
        lastLoggedForceFallback == (forceFallback ? 1 : 0) &&
        lastLoggedSampleMethod == textureSampleMethod &&
        lastLoggedBindlessEnabled == (textureBindlessEnabled ? 1 : 0) &&
        lastLoggedHash == activeHash)
    {
        return;
    }

    lastLoggedStart = textureTableStart;
    lastLoggedLimit = textureTableLimit;
    lastLoggedSampledCount = sampledCount;
    lastLoggedForceFallback = forceFallback ? 1 : 0;
    lastLoggedSampleMethod = textureSampleMethod;
    lastLoggedBindlessEnabled = textureBindlessEnabled ? 1 : 0;
    lastLoggedHash = activeHash;

    common->Printf("PathTracePrimaryPass: RT smoke active texture window start=%d limit=%d requested=%d activeCap=%d slots=%d sampledMaterials=%d forceFallback=%d sampleMethod=%d bindless=%d\n",
        textureTableStart,
        textureTableLimit,
        GetSmokeTextureTableRequestedLimit(),
        RT_SMOKE_TEXTURE_EXPERIMENTAL_ACTIVE_CAP,
        static_cast<int>(table.diffuseTextures.size()),
        sampledCount,
        forceFallback ? 1 : 0,
        textureSampleMethod,
        textureBindlessEnabled ? 1 : 0);

    const std::vector<int> safeMaterialIndexes = BuildSmokeSafeMaterialIndexOrder(table);
    int logged = 0;
    for (int candidateIndex = 0; candidateIndex < static_cast<int>(safeMaterialIndexes.size()) && logged < RT_SMOKE_TEXTURE_PROBE_CANDIDATE_SAMPLES; ++candidateIndex)
    {
        const int tableIndex = safeMaterialIndexes[candidateIndex];
        if (!SmokeMaterialTableIndexIsValid(table, tableIndex))
        {
            continue;
        }
        const PathTraceSmokeMaterial& material = table.materials[tableIndex];
        if (material.diffuseTextureIndex == UINT32_MAX)
        {
            continue;
        }

        const RtSmokeMaterialTextureInfo info = ResolveSmokeMaterialTextureInfo(table.materialIds[tableIndex], tableIndex);
        common->Printf("PathTracePrimaryPass: RT smoke active texture candidate %d table=%d slot=%d material='%s' diffuse='%s' reason='%s'\n",
            candidateIndex,
            tableIndex,
            static_cast<int>(material.diffuseTextureIndex),
            info.materialName.c_str(),
            info.diffuseImageName.c_str(),
            info.fallbackReason.c_str());
        ++logged;
    }
}

RtSmokeTextureCoverageStats BuildSmokeTextureCoverageStats(
    const RtSmokeMaterialTableBuild& table,
    const std::vector<uint32_t>& staticTriangleClassData,
    const std::vector<uint32_t>& staticTriangleMaterialIndexes,
    const std::vector<uint32_t>& dynamicTriangleClassData,
    const std::vector<uint32_t>& dynamicTriangleMaterialIndexes)
{
    RtSmokeTextureCoverageStats stats;
    const int materialTableCount = Min(static_cast<int>(table.materialIds.size()), static_cast<int>(table.materials.size()));
    stats.materials = materialTableCount;
    for (int tableIndex = 0; tableIndex < materialTableCount; ++tableIndex)
    {
        if (table.materials[tableIndex].diffuseTextureIndex != UINT32_MAX)
        {
            ++stats.boundMaterials;
        }
        else
        {
            ++stats.fallbackMaterials;
        }
    }

    AccumulateSmokeTextureCoverageTriangles(table, staticTriangleClassData, staticTriangleMaterialIndexes, stats);
    AccumulateSmokeTextureCoverageTriangles(table, dynamicTriangleClassData, dynamicTriangleMaterialIndexes, stats);
    return stats;
}

void LogSmokeTextureCoverage(const RtSmokeTextureCoverageStats& stats)
{
    common->Printf("PathTracePrimaryPass: RT smoke texture coverage materials bound=%d fallback=%d total=%d",
        stats.boundMaterials,
        stats.fallbackMaterials,
        stats.materials);
    for (int classIndex = 0; classIndex < RT_SMOKE_DEBUG_TEXTURE_COVERAGE_CLASS_COUNT; ++classIndex)
    {
        const RtSmokeTextureCoverageClassStats& classStats = stats.classes[classIndex];
        common->Printf("; %s tris=%d bound=%d fallback=%d invalidMat=%d",
            SmokeTextureCoverageClassNameByIndex(classIndex),
            classStats.triangles,
            classStats.boundTriangles,
            classStats.fallbackTriangles,
            classStats.invalidMaterialTriangles);
    }
    common->Printf("\n");
}

void LogSmokeTextureFallbackDump(const RtSmokeMaterialTableBuild& table)
{
    const int materialTableCount = Min(static_cast<int>(table.materialIds.size()), static_cast<int>(table.materials.size()));
    common->Printf("PathTracePrimaryPass: RT smoke texture fallback dump entries=%d textureSlots=%d/%d tableLimit=%d requested=%d activeCap=%d start=%d\n",
        materialTableCount,
        static_cast<int>(table.diffuseTextures.size()),
        RT_SMOKE_TEXTURE_DESCRIPTOR_CAPACITY,
        GetSmokeTextureTableEffectiveLimit(),
        GetSmokeTextureTableRequestedLimit(),
        RT_SMOKE_TEXTURE_EXPERIMENTAL_ACTIVE_CAP,
        Max(0, r_pathTracingTextureTableStart.GetInteger()));

    int logged = 0;
    int fallbackCount = 0;
    for (int tableIndex = 0; tableIndex < materialTableCount; ++tableIndex)
    {
        if (table.materials[tableIndex].diffuseTextureIndex != UINT32_MAX)
        {
            continue;
        }

        ++fallbackCount;
        if (logged >= RT_SMOKE_TEXTURE_PROBE_DUMP_CANDIDATES)
        {
            continue;
        }

        const RtSmokeMaterialTextureInfo info = ResolveSmokeMaterialTextureInfo(table.materialIds[tableIndex], tableIndex);
        common->Printf("PathTracePrimaryPass: RT smoke texture fallback index=%d id=%u material='%s' diffuse='%s' reason='%s' fallback='%s' image=%d handle=%d safe=%d coverage=%s\n",
            tableIndex,
            table.materialIds[tableIndex],
            info.materialName.c_str(),
            info.diffuseImageName.c_str(),
            info.fallbackReason.c_str(),
            SmokeTextureFallbackReason(table, tableIndex, info),
            info.hasDiffuseImage ? 1 : 0,
            info.hasTextureHandle ? 1 : 0,
            info.hasSafeTexture ? 1 : 0,
            SmokeCoverageName(info.coverage));
        ++logged;
    }

    common->Printf("PathTracePrimaryPass: RT smoke texture fallback dump logged=%d total=%d\n",
        logged,
        fallbackCount);
}

void LogSmokeMaterialTextureDiscovery(const RtSmokeMaterialTableBuild& table)
{
    const int materialTableCount = Min(static_cast<int>(table.materialIds.size()), static_cast<int>(table.materials.size()));
    int diffuseImages = 0;
    int textureHandles = 0;
    int safeTextures = 0;
    int missingTextureHandles = 0;
    for (int tableIndex = 0; tableIndex < materialTableCount; ++tableIndex)
    {
        const RtSmokeMaterialTextureInfo info = ResolveSmokeMaterialTextureInfo(table.materialIds[tableIndex], tableIndex);
        if (info.hasDiffuseImage)
        {
            ++diffuseImages;
        }
        if (info.hasTextureHandle)
        {
            ++textureHandles;
        }
        if (info.hasSafeTexture)
        {
            ++safeTextures;
        }
        else
        {
            ++missingTextureHandles;
        }
    }

    common->Printf("PathTracePrimaryPass: RT smoke material texture discovery tableEntries=%d diffuseImages=%d textureHandles=%d safeTextures=%d missingOrRejected=%d samples=",
        materialTableCount,
        diffuseImages,
        textureHandles,
        safeTextures,
        missingTextureHandles);

    const int sampleCount = idMath::ClampInt(0, RT_SMOKE_DEBUG_MATERIAL_REASON_SAMPLES, materialTableCount);
    for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
    {
        const RtSmokeMaterialTextureInfo info = ResolveSmokeMaterialTextureInfo(table.materialIds[sampleIndex], sampleIndex);
        common->Printf("%sindex=%d material='%s' diffuse='%s' image=%d handle=%d safe=%d guiLike=%d reason='%s'",
            sampleIndex == 0 ? "" : ", ",
            info.tableIndex,
            info.materialName.c_str(),
            info.diffuseImageName.c_str(),
            info.hasDiffuseImage ? 1 : 0,
            info.hasTextureHandle ? 1 : 0,
            info.hasSafeTexture ? 1 : 0,
            IsSmokeImageNameGuiLike(info.diffuseImageName.c_str()) ? 1 : 0,
            info.fallbackReason.c_str());
    }

    common->Printf("\n");

    if (missingTextureHandles > 0)
    {
        common->Printf("PathTracePrimaryPass: RT smoke material missing texture handles samples=");
        int loggedMissing = 0;
        for (int tableIndex = 0; tableIndex < materialTableCount && loggedMissing < RT_SMOKE_DEBUG_MATERIAL_REASON_SAMPLES; ++tableIndex)
        {
            const RtSmokeMaterialTextureInfo info = ResolveSmokeMaterialTextureInfo(table.materialIds[tableIndex], tableIndex);
            if (info.hasTextureHandle)
            {
                continue;
            }

            common->Printf("%sindex=%d material='%s' diffuse='%s' image=%d reason='%s'",
                loggedMissing == 0 ? "" : ", ",
                info.tableIndex,
                info.materialName.c_str(),
                info.diffuseImageName.c_str(),
                info.hasDiffuseImage ? 1 : 0,
                info.fallbackReason.c_str());
            ++loggedMissing;
        }

        common->Printf("\n");
    }
}

void RunSmokeMaterialDiagnosticTriggers(const RtSmokeMaterialDiagnosticTriggerDesc& desc)
{
    if (!desc.materialTable || !desc.materialStats)
    {
        return;
    }

    const RtSmokeMaterialTableBuild& materialTable = *desc.materialTable;
    const RtSmokeMaterialStats& materialStats = *desc.materialStats;

    static uint32_t lastLoggedTextureProbeMaterialId = 0;
    if (desc.enableTextureProbe && r_pathTracingSmokeLog.GetInteger() != 0 && materialTable.textureProbeBoundMaterialId != lastLoggedTextureProbeMaterialId)
    {
        LogSmokeTextureProbeSwitch(materialTable);
        lastLoggedTextureProbeMaterialId = materialTable.textureProbeBoundMaterialId;
    }
    if (desc.enableTextureProbe && r_pathTracingTextureProbeDump.GetInteger() != 0)
    {
        LogSmokeTextureProbeDump(materialTable);
        r_pathTracingTextureProbeDump.SetInteger(0);
    }
    if (desc.enableTextureProbe && r_pathTracingAlphaDump.GetInteger() != 0)
    {
        LogSmokeAlphaMaterialDump(materialTable);
        r_pathTracingAlphaDump.SetInteger(0);
    }
    if (desc.enableTextureProbe && r_pathTracingTextureFallbackDump.GetInteger() != 0)
    {
        LogSmokeTextureFallbackDump(materialTable);
        r_pathTracingTextureFallbackDump.SetInteger(0);
    }
    if (r_pathTracingTranslucentDump.GetInteger() != 0)
    {
        LogSmokeTranslucentSubtypeDump(materialStats);
        r_pathTracingTranslucentDump.SetInteger(0);
    }
    if (r_pathTracingCrosshairMaterialDump.GetInteger() != 0)
    {
        LogSmokeCrosshairMaterialDump(
            desc.viewDef,
            materialTable,
            desc.dynamicMaterialRecords,
            desc.dynamicTriangleMaterialIds,
            desc.dynamicTriangleMaterialIndexes,
            desc.staticTriangleMaterialIds,
            desc.staticTriangleMaterialIndexes,
            desc.rigidRouteBuild);
        r_pathTracingCrosshairMaterialDump.SetInteger(0);
    }
    if (r_pathTracingGuiDump.GetInteger() != 0)
    {
        LogSmokeGuiSurfaceDump(desc.viewDef, materialTable);
        r_pathTracingGuiDump.SetInteger(0);
    }
    if (desc.enableTextureProbe && r_pathTracingSmokeLog.GetInteger() != 0)
    {
        LogSmokeTextureActiveWindow(materialTable);
    }
}

void RunSmokeEmissiveInventoryDiagnosticTriggers(const RtSmokeEmissiveInventoryDiagnosticTriggerDesc& desc)
{
    if (!desc.materialTable || !desc.emissiveTriangles || !desc.emissiveInventoryStats)
    {
        return;
    }

    if (r_pathTracingEmissiveInventoryDump.GetInteger() != 0 ||
        r_pathTracingRigidRouteEmissiveDump.GetInteger() != 0)
    {
        LogSmokeEmissiveInventoryDump(desc.materialTable->materialIds, *desc.emissiveTriangles, *desc.emissiveInventoryStats);
        r_pathTracingEmissiveInventoryDump.SetInteger(0);
        r_pathTracingRigidRouteEmissiveDump.SetInteger(0);
    }
}

void RunSmokeSceneBuildDiagnosticLogs(const RtSmokeSceneBuildDiagnosticLogDesc& desc)
{
    if (!desc.lastSceneTimingLogMs || !desc.sceneRebuildLogged || !desc.sceneLogCooldownFrames ||
        !desc.classStats || !desc.skipStats || !desc.dynamicStats || !desc.attributeStats ||
        !desc.materialStats || !desc.bucketRanges || !desc.materialTable || !desc.emissiveInventoryStats || !desc.materialTableCacheStats || !desc.materialTableBuildStats || !desc.materialUniverseStats || !desc.materialUniverseTableCompareStats || !desc.textureCoverageStats)
    {
        return;
    }

    if (ShouldLogSmokeTiming(desc.sceneMs, Sys_Milliseconds(), *desc.lastSceneTimingLogMs))
    {
        RtSmokeSlowSceneBuildLogDesc slowLog;
        slowLog.sceneMs = desc.sceneMs;
        slowLog.captureMs = desc.captureMs;
        slowLog.captureAnchorMs = desc.captureTiming.anchorMs;
        slowLog.captureValidationMs = desc.captureTiming.validationMs;
        slowLog.captureStaticPassClassifyMs = desc.captureTiming.staticPassClassifyMs;
        slowLog.captureStaticCacheLookupMs = desc.captureTiming.staticCacheLookupMs;
        slowLog.captureStaticAppendMs = desc.captureTiming.staticAppendMs;
        slowLog.captureDynamicPassClassifyMs = desc.captureTiming.dynamicPassClassifyMs;
        slowLog.captureDynamicAppendMs = desc.captureTiming.dynamicAppendMs;
        slowLog.captureRtCpuSkinningAppendMs = desc.captureTiming.rtCpuSkinningAppendMs;
        slowLog.captureAppendMs = desc.captureTiming.appendMs;
        slowLog.captureBucketMergeMs = desc.captureTiming.bucketMergeMs;
        slowLog.metadataMs = desc.metadataMs;
        slowLog.metadataValidationMs = desc.metadataValidationMs;
        slowLog.metadataRegistrationMs = desc.metadataRegistrationMs;
        slowLog.materialMs = desc.materialMs;
        slowLog.emissiveMs = desc.emissiveMs;
        slowLog.bufferCreateMs = desc.bufferCreateMs;
        slowLog.bufferUploadMs = desc.bufferUploadMs;
        slowLog.bvhFramePlanMs = desc.bvhFramePlanMs;
        slowLog.accelSubmitMs = desc.accelSubmitMs;
        slowLog.blasSubmitMs = desc.blasSubmitMs;
        slowLog.tlasSubmitMs = desc.tlasSubmitMs;
        slowLog.sourceSurfaces = desc.sourceSurfaces;
        slowLog.sourceVerts = desc.sourceVerts;
        slowLog.sourceIndexes = desc.sourceIndexes;
        slowLog.dynamicIndexCount = desc.dynamicIndexCount;
        slowLog.staticCachedSurfaces = desc.captureTiming.staticCachedSurfaces;
        slowLog.staticNewSurfaces = desc.captureTiming.staticNewSurfaces;
        slowLog.staticSurfaceCacheSize = desc.staticSurfaceCacheSize;
        slowLog.staticVertexCacheCount = desc.staticVertexCacheCount;
        slowLog.staticIndexCacheCount = desc.staticIndexCacheCount;
        slowLog.staticTriangleCacheCount = desc.staticTriangleCacheCount;
        slowLog.staticSeenThisFrame = desc.staticSeenThisFrame;
        slowLog.staticNewThisFrame = desc.staticNewThisFrame;
        slowLog.staticDisappearedThisFrame = desc.staticDisappearedThisFrame;
        slowLog.staticHistoryValid = desc.staticHistoryValid;
        slowLog.staticPreviousRangeValid = desc.staticPreviousRangeValid;
        slowLog.staticDirty = desc.staticDirty;
        slowLog.staticValidationErrors = desc.staticValidationErrors;
        slowLog.staticRangeErrors = desc.staticRangeErrors;
        slowLog.staticDuplicateKeys = desc.staticDuplicateKeys;
        slowLog.staticHistoryErrors = desc.staticHistoryErrors;
        slowLog.staticKeyVectorMismatches = desc.staticKeyVectorMismatches;
        slowLog.staticCacheBytesKB = desc.staticCacheBytesKB;
        slowLog.staticBlasSignatureReused = desc.staticBlasSignatureReused;
        slowLog.staticBlasSignatureMs = desc.staticBlasSignatureMs;
        slowLog.anchorSurfaceTests = desc.captureTiming.anchorSurfaceTests;
        slowLog.anchorBoundsRejects = desc.captureTiming.anchorBoundsRejects;
        slowLog.anchorTriangleTests = desc.captureTiming.anchorTriangleTests;
        slowLog.skinnedRtCpuSurfaces = desc.dynamicStats->skinnedRtCpuSkinnedSurfaces;
        slowLog.skinnedRtCpuIndexes = desc.dynamicStats->skinnedRtCpuSkinnedIndexes;
        slowLog.staticBlasCacheHit = desc.staticBlasCacheHit;
        slowLog.materialTableCacheHit = desc.materialTableCacheHit;
        slowLog.materialTablePath = desc.materialTablePath;
        slowLog.materialTableCacheHits = desc.materialTableCacheStats->hits;
        slowLog.materialTableCacheMisses = desc.materialTableCacheStats->misses;
        slowLog.materialTableBuildStats = *desc.materialTableBuildStats;
        slowLog.materialUniverseStats = *desc.materialUniverseStats;
        slowLog.materialUniverseTableCompareStats = *desc.materialUniverseTableCompareStats;
        slowLog.materialUniverseMaterialCount = desc.materialUniverseStats->universeMaterials;
        slowLog.materialMetadataCacheEnabled = r_pathTracingMaterialMetadataCache.GetInteger() != 0;
        slowLog.metadataCacheRefreshes = g_smokeMaterialMetadataFrameStats.cacheRefreshes;
        slowLog.metadataFullDiscovers = g_smokeMaterialMetadataFrameStats.fullDiscovers;
        slowLog.metadataNewEntries = g_smokeMaterialMetadataFrameStats.newEntries;
        slowLog.metadataRegistrations = g_smokeMaterialMetadataFrameStats.registrations;
        slowLog.metadataDuplicateSkips = g_smokeMaterialMetadataFrameStats.duplicateSkips;
        slowLog.metadataRegistrySize = SmokeMaterialTextureRegistrySize();
        slowLog.guiTextureCandidates = desc.materialTable->guiTextureCandidates;
        slowLog.guiTexturesAccepted = desc.materialTable->guiTexturesAccepted;
        slowLog.guiTexturesRejected = desc.materialTable->guiTexturesRejected;
        slowLog.additiveDecals = desc.materialTable->materialsAdditiveDecals;
        slowLog.lightCandidateCount = desc.emissiveInventoryStats->candidateMaterials;
        slowLog.texturedLightCandidateCount = desc.emissiveInventoryStats->texturedCandidateMaterials;
        slowLog.lightCandidateBytes = desc.lightCandidateBytes;
        slowLog.lightCount = r_pathTracingLightCount.GetInteger();
        slowLog.debugMode = desc.requestedDebugMode;
        LogSmokeSlowSceneBuild(slowLog);
    }

    RtSmokeSceneBuildSummaryLogDesc sceneSummaryLog;
    sceneSummaryLog.sourceSurfaces = desc.sourceSurfaces;
    sceneSummaryLog.sourceVerts = desc.sourceVerts;
    sceneSummaryLog.sourceIndexes = desc.sourceIndexes;
    sceneSummaryLog.anchorTriangle = desc.anchorTriangle;
    sceneSummaryLog.staticIndexCount = desc.staticIndexCount;
    sceneSummaryLog.staticVertexCount = desc.staticVertexCount;
    sceneSummaryLog.dynamicIndexCount = desc.dynamicIndexCount;
    sceneSummaryLog.dynamicVertexCount = desc.dynamicVertexCount;
    sceneSummaryLog.instanceCount = desc.instanceCount;
    sceneSummaryLog.rigidTlasInstanceCount = desc.rigidTlasInstanceCount;
    sceneSummaryLog.rigidRouteInstanceCount = desc.rigidRouteInstanceCount;
    sceneSummaryLog.rigidRouteUniqueMeshes = desc.rigidRouteUniqueMeshes;
    sceneSummaryLog.rigidRouteVertexCount = desc.rigidRouteVertexCount;
    sceneSummaryLog.rigidRouteIndexCount = desc.rigidRouteIndexCount;
    sceneSummaryLog.rigidRouteTriangleCount = desc.rigidRouteTriangleCount;
    sceneSummaryLog.staticBvhResidentBuckets = desc.staticBvhResidentBuckets;
    sceneSummaryLog.staticBvhActiveBuckets = desc.staticBvhActiveBuckets;
    sceneSummaryLog.staticBvhInactiveResidentBuckets = desc.staticBvhInactiveResidentBuckets;
    sceneSummaryLog.staticBvhEmittedInstances = desc.staticBvhEmittedInstances;
    sceneSummaryLog.staticBucketBlasRecords = desc.staticBucketBlasRecords;
    sceneSummaryLog.staticBucketBlasSkippedInactive = desc.staticBucketBlasSkippedInactive;
    sceneSummaryLog.staticBucketBlasSkippedInvalid = desc.staticBucketBlasSkippedInvalid;
    sceneSummaryLog.staticBucketBlasOverflow = desc.staticBucketBlasOverflow;
    sceneSummaryLog.staticBucketTraversalRouteRequired = desc.staticBucketTraversalRouteRequired;
    sceneSummaryLog.staticBucketTraversalCurrentShaderCompatible = desc.staticBucketTraversalCurrentShaderCompatible;
    sceneSummaryLog.staticBucketTraversalExactMonolithic = desc.staticBucketTraversalExactMonolithic;
    sceneSummaryLog.staticBucketTraversalNonZeroOffsetRecords = desc.staticBucketTraversalNonZeroOffsetRecords;
    sceneSummaryLog.staticRouteNamespaceBlocked = desc.staticRouteNamespaceBlocked;
    sceneSummaryLog.staticRouteNamespaceFirst = desc.staticRouteNamespaceFirst;
    sceneSummaryLog.rigidRouteNamespaceFirst = desc.rigidRouteNamespaceFirst;
    sceneSummaryLog.staticRouteNamespaceCount = desc.staticRouteNamespaceCount;
    sceneSummaryLog.rigidRouteNamespaceShifted = desc.rigidRouteNamespaceShifted;
    sceneSummaryLog.staticMonolithicInactiveIncluded = desc.staticMonolithicInactiveIncluded;
    sceneSummaryLog.staticRequiresBucketedBlas = desc.staticRequiresBucketedBlas;
    sceneSummaryLog.bvhDirtyPreviousValid = desc.bvhDirtyPreviousValid;
    sceneSummaryLog.bvhGeometryContentChanged = desc.bvhGeometryContentChanged;
    sceneSummaryLog.bvhActiveGeometryContentChanged = desc.bvhActiveGeometryContentChanged;
    sceneSummaryLog.bvhResidentSetChanged = desc.bvhResidentSetChanged;
    sceneSummaryLog.bvhMaterialChanged = desc.bvhMaterialChanged;
    sceneSummaryLog.bvhActiveMembershipChanged = desc.bvhActiveMembershipChanged;
    sceneSummaryLog.bvhTlasInstanceChanged = desc.bvhTlasInstanceChanged;
    sceneSummaryLog.bvhBlasInputDirty = desc.bvhBlasInputDirty;
    sceneSummaryLog.bvhTlasDirty = desc.bvhTlasDirty;
    sceneSummaryLog.bvhActiveSetSignature = desc.bvhActiveSetSignature;
    sceneSummaryLog.bvhResidentSetSignature = desc.bvhResidentSetSignature;
    sceneSummaryLog.bvhGeometryContentSignature = desc.bvhGeometryContentSignature;
    sceneSummaryLog.bvhActiveBlasInputSignature = desc.bvhActiveBlasInputSignature;
    sceneSummaryLog.bvhTlasInstanceSignature = desc.bvhTlasInstanceSignature;
    sceneSummaryLog.staticUploadBytes = desc.staticUploadBytes;
    sceneSummaryLog.previousStaticUploadBytes = desc.previousStaticUploadBytes;
    sceneSummaryLog.previousStaticUploadSkippedBytes = desc.previousStaticUploadSkippedBytes;
    sceneSummaryLog.dynamicUploadBytes = desc.dynamicUploadBytes;
    sceneSummaryLog.rigidRouteUploadBytes = desc.rigidRouteUploadBytes;
    sceneSummaryLog.rigidRouteGeometryBytes = desc.rigidRouteGeometryBytes;
    sceneSummaryLog.rigidRouteInstanceBytes = desc.rigidRouteInstanceBytes;
    sceneSummaryLog.rigidRouteBuildMs = desc.rigidRouteBuildMs;
    sceneSummaryLog.bvhFramePlanMs = desc.bvhFramePlanMs;
    sceneSummaryLog.bufferUploadMs = desc.bufferUploadMs;
    sceneSummaryLog.accelSubmitMs = desc.accelSubmitMs;
    sceneSummaryLog.blasSubmitMs = desc.blasSubmitMs;
    sceneSummaryLog.tlasSubmitMs = desc.tlasSubmitMs;
    sceneSummaryLog.staticBlasBuildSubmitted = desc.staticBlasBuildSubmitted;
    sceneSummaryLog.staticBlasBuildSkipped = desc.staticBlasBuildSkipped;
    sceneSummaryLog.dynamicBlasBuildSubmitted = desc.dynamicBlasBuildSubmitted;
    sceneSummaryLog.dynamicBlasBuildSkipped = desc.dynamicBlasBuildSkipped;
    sceneSummaryLog.classStats = *desc.classStats;
    sceneSummaryLog.skipStats = *desc.skipStats;
    sceneSummaryLog.dynamicStats = *desc.dynamicStats;
    sceneSummaryLog.allowGuiSurfaces = r_pathTracingAllowGuiSurfaces.GetInteger() != 0;
    sceneSummaryLog.skipCallbackEntities = r_pathTracingSkipCallbackEntities.GetInteger() != 0;
    sceneSummaryLog.staticBlasCacheHit = desc.staticBlasCacheHit;
    sceneSummaryLog.staticBlasSignature = desc.staticBlasSignature;
    sceneSummaryLog.staticSurfaceCacheSize = desc.staticSurfaceCacheSize;
    sceneSummaryLog.staticVertexCacheCount = desc.staticVertexCacheCount;
    sceneSummaryLog.staticIndexCacheCount = desc.staticIndexCacheCount;
    sceneSummaryLog.staticTriangleCacheCount = desc.staticTriangleCacheCount;
    sceneSummaryLog.staticSeenThisFrame = desc.staticSeenThisFrame;
    sceneSummaryLog.staticNewThisFrame = desc.staticNewThisFrame;
    sceneSummaryLog.staticDisappearedThisFrame = desc.staticDisappearedThisFrame;
    sceneSummaryLog.staticHistoryValid = desc.staticHistoryValid;
    sceneSummaryLog.staticPreviousRangeValid = desc.staticPreviousRangeValid;
    sceneSummaryLog.staticDirty = desc.staticDirty;
    sceneSummaryLog.staticValidationErrors = desc.staticValidationErrors;
    sceneSummaryLog.staticRangeErrors = desc.staticRangeErrors;
    sceneSummaryLog.staticDuplicateKeys = desc.staticDuplicateKeys;
    sceneSummaryLog.staticHistoryErrors = desc.staticHistoryErrors;
    sceneSummaryLog.staticKeyVectorMismatches = desc.staticKeyVectorMismatches;
    sceneSummaryLog.staticCacheBytesKB = desc.staticCacheBytesKB;
    sceneSummaryLog.staticBlasSignatureReused = desc.staticBlasSignatureReused;
    sceneSummaryLog.staticBlasSignatureMs = desc.staticBlasSignatureMs;
    sceneSummaryLog.staticBlasCacheHitCount = desc.staticBlasCacheHitCount;
    sceneSummaryLog.staticBlasCacheMissCount = desc.staticBlasCacheMissCount;
    sceneSummaryLog.materialTableCacheHit = desc.materialTableCacheHit;
    sceneSummaryLog.materialTablePath = desc.materialTablePath;
    sceneSummaryLog.materialTableSignature = desc.materialTableSignature;
    sceneSummaryLog.materialTableCacheStats = *desc.materialTableCacheStats;
    sceneSummaryLog.materialTableBuildStats = *desc.materialTableBuildStats;
    if (desc.materialClassifierStats)
    {
        sceneSummaryLog.materialClassifierStats = *desc.materialClassifierStats;
    }
    sceneSummaryLog.materialUniverseStats = *desc.materialUniverseStats;
    sceneSummaryLog.materialUniverseTableCompareStats = *desc.materialUniverseTableCompareStats;
    sceneSummaryLog.materialStats = desc.materialStats;
    sceneSummaryLog.materialTable = desc.materialTable;
    sceneSummaryLog.emissiveInventoryStats = desc.emissiveInventoryStats;
    sceneSummaryLog.lightCandidateBytes = desc.lightCandidateBytes;
    sceneSummaryLog.enableTextureProbe = desc.enableTextureProbe;
    sceneSummaryLog.textureCoverageStats = desc.textureCoverageStats;
    sceneSummaryLog.attributeStats = desc.attributeStats;
    sceneSummaryLog.bucketRanges = desc.bucketRanges;

    if (r_pathTracingSmokeLog.GetInteger() != 0 && !*desc.sceneRebuildLogged)
    {
        LogSmokeSceneRebuildSummary(sceneSummaryLog);
        *desc.sceneRebuildLogged = true;
    }

    if (desc.dumpClassReasons && desc.reasonSamples)
    {
        common->Printf("PathTracePrimaryPass: RT smoke one-shot surface classification sample dump\n");
        LogSmokeSurfaceClassReasonSamples(*desc.reasonSamples);
        r_pathTracingClassDump.SetInteger(0);
    }

    if (r_pathTracingSmokeLog.GetInteger() != 0 && *desc.sceneLogCooldownFrames <= 0)
    {
        LogSmokeSceneCaptureSummary(sceneSummaryLog);
        *desc.sceneLogCooldownFrames = desc.sceneCaptureLogIntervalFrames;
    }
    else
    {
        --*desc.sceneLogCooldownFrames;
    }
}
