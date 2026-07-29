#include "precompiled.h"
#pragma hdrstop

// Per-frame scene build orchestration for the RT smoke/path tracing path.
//
// This file keeps the top-level build order visible: capture Doom surfaces,
// build material/emissive data, create and upload buffers, submit acceleration
// structures, create bindings, commit resources, then run scene diagnostics.
// Lower-level classification, capture, resource, and diagnostic work stays in
// the narrower PathTrace* modules.

#include "PathTraceAcceleration.h"
#include "PathTraceAccelerationPlan.h"
#include "PathTraceCVars.h"
#include "PathTraceCpuWork.h"
#include "PathTraceDebugDumps.h"
#include "PathTraceDoomLights.h"
#include "PathTraceDrawSurfCapture.h"
#include "PathTraceDynamicMaterialState.h"
#include "PathTraceEmissiveCandidates.h"
#include "PathTraceEntityFeed.h"
#include "PathTraceGeometryAttributeSurvey.h"
#include "PathTraceJointCacheCopyPlan.h"
#include "PathTraceMaterialClassifier.h"
#include "PathTraceMaterialUniverse.h"
#include "PathTraceMaterialTextureDiscovery.h"
#include "PathTraceParticleCapture.h"
#include "PathTracePrimaryPass.h"
#include "PathTraceRemixFramePrepare.h"
#include "PathTraceRemixLightManager.h"
#include "PathTraceDebugModes.h"
#include "PathTraceSceneCapture.h"
#include "PathTraceSceneUniverse.h"
#include "PathTraceSkinnedConsumerAudit.h"
#include "PathTraceSkinnedHitRoute.h"
#include "PathTraceSkinnedHistoryPolicy.h"
#include "PathTraceSkinning.h"
#include "PathTraceSmokeResources.h"
#include "PathTraceSurfaceDebugDumps.h"
#include "PathTraceSurfaceClassification.h"
#include "PathTraceTextureRegistry.h"
#include "PathTraceUnifiedLight.h"
#include "../RenderBackend.h"
#include "../Image.h"
#include "../Material.h"
#include "../Model_local.h"
#include "../Passes/CommonPasses.h"
#include "../../framework/Common_local.h"
#include "../../sys/DeviceManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <nvrhi/utils.h>

extern DeviceManager* deviceManager;
extern idCVar r_lightScale;

namespace {

const int RT_SMOKE_SCENE_LOG_INTERVAL_FRAMES = 120;
const int RT_SMOKE_MAX_EMISSIVE_TRIANGLE_RECORDS = 65536;
const int RT_SMOKE_GEOMETRY_VALIDATION_DUMP_RECORDS = 16;
const int RT_SMOKE_GEOMETRY_RANGE_DUMP_RECORDS = 16;
const int RT_PT_RESIDENT_BOUNDS_OVERLAY_SAFE_BOXES = 64;
const float RT_SMOKE_SKINNED_TELEPORT_DISTANCE = 1024.0f;
const int RT_SMOKE_RUNTIME_MATERIAL_APPLY_SAMPLES = 64;
const char* RT_SMOKE_SKY_ENVIRONMENT_FALLBACK_NAME = "<white-terminal-fallback>";
static_assert(
    sizeof(idJointMat) == PT_JOINT_CACHE_MATRIX_BYTES,
    "GEO-07 compact joint planner must match the renderer joint ABI");
int g_smokeLastSceneTimingLogMs = -1000000;
uint64 g_smokeLastGeometryValidationDumpGeneration = 0;
int g_smokeLastGeometryValidationDumpErrors = 0;

void AppendRenderedAttributeSurveyRange(
    const std::vector<PathTraceSmokeVertex>& vertices,
    int vertexOffset,
    int vertexCount,
    const PtRenderedGeometrySurveyRecord& metadata,
    PtRenderedGeometrySurvey& survey)
{
    if (vertexOffset < 0 ||
        vertexCount <= 0 ||
        static_cast<size_t>(vertexOffset) > vertices.size() ||
        static_cast<size_t>(vertexCount) >
            vertices.size() - static_cast<size_t>(vertexOffset))
    {
        PtRecordRenderedGeometrySurveyInvalidRange(survey);
        return;
    }

    std::vector<PtRenderedGeometrySurveyVertex> values;
    values.resize(static_cast<size_t>(vertexCount));
    for (int vertexIndex = 0;
        vertexIndex < vertexCount;
        ++vertexIndex)
    {
        const PathTraceSmokeVertex& source =
            vertices[static_cast<size_t>(vertexOffset + vertexIndex)];
        PtRenderedGeometrySurveyVertex& destination =
            values[static_cast<size_t>(vertexIndex)];
        memcpy(destination.position, source.position, sizeof(destination.position));
        memcpy(destination.normal, source.normal, sizeof(destination.normal));
        memcpy(destination.texCoord, source.texCoord, sizeof(destination.texCoord));
        memcpy(destination.color, source.color, sizeof(destination.color));
        memcpy(destination.color2, source.color2, sizeof(destination.color2));
        memcpy(destination.tangent, source.tangent, sizeof(destination.tangent));
        memcpy(destination.bitangent, source.bitangent, sizeof(destination.bitangent));
    }
    PtAppendRenderedGeometrySurveyRecord(
        metadata,
        values.data(),
        values.size(),
        survey);
}

const RtPathTraceSceneUniverseSurface* FindSceneUniverseSurfaceForStaticRecord(
    const RtSmokePersistentStaticSurfaceRecord& record,
    const std::vector<RtPathTraceSceneUniverseSurface>& surfaces)
{
    for (const RtPathTraceSceneUniverseSurface& surface : surfaces)
    {
        if ((record.bucketSurfaceKey != 0 &&
                surface.key == record.bucketSurfaceKey) ||
            (record.key != 0 &&
                surface.legacyDrawSurfKey == record.key))
        {
            return &surface;
        }
    }
    return nullptr;
}

void DumpRenderedAttributeSurvey(
    int requestedPage,
    int sceneSource,
    const std::vector<PathTraceSmokeVertex>& staticVertices,
    const std::vector<RtSmokePersistentStaticSurfaceRecord>& staticRecords,
    const std::vector<RtPathTraceSceneUniverseSurface>& sceneSurfaces,
    const std::vector<PathTraceSmokeVertex>& dynamicVertices,
    const std::vector<RtSmokeCapturedSurfaceRecord>& dynamicRecords)
{
    PtRenderedGeometrySurvey survey;
    PtBeginRenderedGeometrySurvey(survey);

    for (const RtSmokePersistentStaticSurfaceRecord& source : staticRecords)
    {
        if (!source.valid)
        {
            continue;
        }
        const RtPathTraceSceneUniverseSurface* identity =
            FindSceneUniverseSurfaceForStaticRecord(source, sceneSurfaces);
        PtRenderedGeometrySurveyRecord metadata;
        metadata.domain =
            PtRenderedGeometrySurveyDomain::StaticResident;
        metadata.identity =
            source.bucketSurfaceKey != 0
                ? source.bucketSurfaceKey
                : source.key;
        metadata.surfaceClassId = source.surfaceClassId;
        metadata.materialId = source.materialId;
        metadata.portalArea = source.portalArea;
        if (identity)
        {
            metadata.entityIndex = identity->entityIndex;
            metadata.modelSurfaceIndex = identity->surfaceIndex;
            metadata.modelName = identity->modelName.c_str();
            metadata.materialName = identity->materialName.c_str();
        }
        else
        {
            metadata.modelName = "<unjoined-static>";
            metadata.materialName = "<unjoined-static>";
        }
        AppendRenderedAttributeSurveyRange(
            staticVertices,
            source.currentRange.vertices.offset,
            source.currentRange.vertices.count,
            metadata,
            survey);
    }

    for (const RtSmokeCapturedSurfaceRecord& source : dynamicRecords)
    {
        PtRenderedGeometrySurveyRecord metadata;
        metadata.domain =
            PtRenderedGeometrySurveyDomain::DynamicFallback;
        metadata.identity =
            (static_cast<uint64>(static_cast<uint32>(source.entityIndex + 1))
                << 32) ^
            static_cast<uint32>(source.drawSurfIndex + 1);
        metadata.surfaceClassId =
            source.triangleClassAndFlags &
            RT_SMOKE_TRIANGLE_CLASS_MASK;
        metadata.materialId = source.materialId;
        metadata.entityIndex = source.entityIndex;
        metadata.modelSurfaceIndex = source.modelSurfaceIndex;
        metadata.modelName = source.modelName.c_str();
        metadata.materialName = source.materialName.c_str();
        AppendRenderedAttributeSurveyRange(
            dynamicVertices,
            source.currentVertexOffset,
            source.vertexCount,
            metadata,
            survey);
    }

    const PtRenderedGeometrySurveyStats& totals = survey.totals;
    const PtGeometryAttributeSurveyStats& values = totals.values;
    const int rowsPerPage = 32;
    const int pageCount = Max(
        1,
        (static_cast<int>(survey.records.size()) +
            rowsPerPage - 1) /
            rowsPerPage);
    const int page = idMath::ClampInt(1, pageCount, requestedPage);
    const int rowBegin = (page - 1) * rowsPerPage;
    const int rowEnd = Min(
        static_cast<int>(survey.records.size()),
        rowBegin + rowsPerPage);
    const uint64 currentBytes =
        values.currentPositionBytes +
        values.currentAttributeBytes;
    uint64 staticClassRecords[RT_SMOKE_CLASS_COUNT] = {};
    uint64 dynamicClassRecords[RT_SMOKE_CLASS_COUNT] = {};
    for (const PtRenderedGeometrySurveyRecord& record :
        survey.records)
    {
        const int classIndex = idMath::ClampInt(
            0,
            RT_SMOKE_CLASS_COUNT - 1,
            static_cast<int>(
                record.surfaceClassId &
                RT_SMOKE_TRIANGLE_CLASS_MASK));
        uint64* classRecords =
            record.domain ==
                    PtRenderedGeometrySurveyDomain::StaticResident
                ? staticClassRecords
                : dynamicClassRecords;
        ++classRecords[classIndex];
    }

    common->Printf(
        "PathTracePrimaryPass: GEO12 rendered attribute survey page=%d/%d rows=%d..%d records(total/static/dynamic/invalidRange)=%d/%llu/%llu/%llu sceneSource=%d vertices=%llu currentBytes=%llu\n",
        page,
        pageCount,
        rowBegin + (rowBegin < rowEnd ? 1 : 0),
        rowEnd,
        static_cast<int>(survey.records.size()),
        static_cast<unsigned long long>(survey.staticRecordCount),
        static_cast<unsigned long long>(survey.dynamicRecordCount),
        static_cast<unsigned long long>(survey.invalidRangeRecordCount),
        sceneSource,
        static_cast<unsigned long long>(values.vertexCount),
        static_cast<unsigned long long>(currentBytes));
    common->Printf(
        "PathTracePrimaryPass: GEO12 rendered class census static(world/rigid/skinned/particle/unknown)=%llu/%llu/%llu/%llu/%llu dynamic=%llu/%llu/%llu/%llu/%llu\n",
        static_cast<unsigned long long>(staticClassRecords[0]),
        static_cast<unsigned long long>(staticClassRecords[1]),
        static_cast<unsigned long long>(staticClassRecords[2]),
        static_cast<unsigned long long>(staticClassRecords[3]),
        static_cast<unsigned long long>(staticClassRecords[4]),
        static_cast<unsigned long long>(dynamicClassRecords[0]),
        static_cast<unsigned long long>(dynamicClassRecords[1]),
        static_cast<unsigned long long>(dynamicClassRecords[2]),
        static_cast<unsigned long long>(dynamicClassRecords[3]),
        static_cast<unsigned long long>(dynamicClassRecords[4]));
    common->Printf(
        "PathTracePrimaryPass: GEO12 rendered ranges position=(%.9g %.9g %.9g)..(%.9g %.9g %.9g) diffuseUV=(%.9g %.9g)..(%.9g %.9g) normalUV=(%.9g %.9g)..(%.9g %.9g) nonfinite(pos/diffuseUV/normalUV/basis/color)=%llu/%llu/%llu/%llu/%llu\n",
        values.positionMin[0], values.positionMin[1], values.positionMin[2],
        values.positionMax[0], values.positionMax[1], values.positionMax[2],
        values.texCoordMin[0], values.texCoordMin[1],
        values.texCoordMax[0], values.texCoordMax[1],
        totals.normalMapTexCoordMin[0], totals.normalMapTexCoordMin[1],
        totals.normalMapTexCoordMax[0], totals.normalMapTexCoordMax[1],
        static_cast<unsigned long long>(values.nonFinitePositionComponents),
        static_cast<unsigned long long>(values.nonFiniteTexCoordComponents),
        static_cast<unsigned long long>(totals.nonFiniteNormalMapTexCoordComponents),
        static_cast<unsigned long long>(values.nonFiniteBasisComponents),
        static_cast<unsigned long long>(values.nonFiniteColorComponents));
    common->Printf(
        "PathTracePrimaryPass: GEO12 rendered half diffuse(samples/overflow/underflow/maxAbs/maxRel)=%llu/%llu/%llu/%.9g/%.9g normal(samples/overflow/underflow/maxAbs/maxRel)=%llu/%llu/%llu/%.9g/%.9g\n",
        static_cast<unsigned long long>(values.halfTexCoordComponents),
        static_cast<unsigned long long>(values.halfTexCoordOverflowComponents),
        static_cast<unsigned long long>(values.halfTexCoordUnderflowToZeroComponents),
        values.halfTexCoordMaxAbsError,
        values.halfTexCoordMaxRelativeError,
        static_cast<unsigned long long>(totals.halfNormalMapTexCoordComponents),
        static_cast<unsigned long long>(totals.halfNormalMapTexCoordOverflowComponents),
        static_cast<unsigned long long>(totals.halfNormalMapTexCoordUnderflowToZeroComponents),
        totals.halfNormalMapTexCoordMaxAbsError,
        totals.halfNormalMapTexCoordMaxRelativeError);
    common->Printf(
        "PathTracePrimaryPass: GEO12 rendered basis octMaxDegrees(normal/tangent)=%.9g/%.9g degenerate(N/T/B)=%llu/%llu/%llu deriveBitangent(samples/invalid/maxDegrees)=%llu/%llu/%.9g colorExact=%llu/%llu color2Exact=%llu/%llu\n",
        values.normalOct16MaxAngularErrorDegrees,
        values.tangentOct16MaxAngularErrorDegrees,
        static_cast<unsigned long long>(values.normalDegenerateVertices),
        static_cast<unsigned long long>(values.tangentDegenerateVertices),
        static_cast<unsigned long long>(values.bitangentDegenerateVertices),
        static_cast<unsigned long long>(values.bitangentReconstructionSamples),
        static_cast<unsigned long long>(values.bitangentReconstructionInvalid),
        values.bitangentReconstructionMaxAngularErrorDegrees,
        static_cast<unsigned long long>(values.colorUnorm8ExactComponents),
        static_cast<unsigned long long>(values.colorComponents),
        static_cast<unsigned long long>(values.color2Unorm8ExactComponents),
        static_cast<unsigned long long>(values.color2Components));
    common->Printf(
        "PathTracePrimaryPass: GEO12 rendered independentCandidateSavingsBytes halfDiffuse=%llu halfNormalMap=%llu octNormal=%llu octTangent=%llu deriveBitangent=%llu unormColor=%llu unormColor2=%llu combined=not-admitted\n",
        static_cast<unsigned long long>(values.vertexCount * 4ull),
        static_cast<unsigned long long>(values.vertexCount * 4ull),
        static_cast<unsigned long long>(values.vertexCount * 12ull),
        static_cast<unsigned long long>(values.vertexCount * 12ull),
        static_cast<unsigned long long>(values.vertexCount * 16ull),
        static_cast<unsigned long long>(values.vertexCount * 12ull),
        static_cast<unsigned long long>(values.vertexCount * 12ull));

    for (int row = rowBegin; row < rowEnd; ++row)
    {
        const PtRenderedGeometrySurveyRecord& record =
            survey.records[static_cast<size_t>(row)];
        const PtGeometryAttributeSurveyStats& stats =
            record.stats.values;
        common->Printf(
            "PathTracePrimaryPass: GEO12 rendered row=%d domain=%s identity=%llu class=%u material=%u entity/surface/area=%d/%d/%d verts=%llu diffuseUV=(%.7g %.7g)..(%.7g %.7g) normalUV=(%.7g %.7g)..(%.7g %.7g) halfOverflow(diffuse/normal)=%llu/%llu octMax(N/T)=%.6g/%.6g model='%s' materialName='%s'\n",
            row + 1,
            record.domain ==
                    PtRenderedGeometrySurveyDomain::StaticResident
                ? "static"
                : "dynamic",
            static_cast<unsigned long long>(record.identity),
            record.surfaceClassId,
            record.materialId,
            record.entityIndex,
            record.modelSurfaceIndex,
            record.portalArea,
            static_cast<unsigned long long>(stats.vertexCount),
            stats.texCoordMin[0], stats.texCoordMin[1],
            stats.texCoordMax[0], stats.texCoordMax[1],
            record.stats.normalMapTexCoordMin[0],
            record.stats.normalMapTexCoordMin[1],
            record.stats.normalMapTexCoordMax[0],
            record.stats.normalMapTexCoordMax[1],
            static_cast<unsigned long long>(
                stats.halfTexCoordOverflowComponents),
            static_cast<unsigned long long>(
                record.stats.
                    halfNormalMapTexCoordOverflowComponents),
            stats.normalOct16MaxAngularErrorDegrees,
            stats.tangentOct16MaxAngularErrorDegrees,
            record.modelName.c_str(),
            record.materialName.c_str());
    }
}

nvrhi::TextureHandle CreateSmokeSkyEnvironmentCube(
    nvrhi::ICommandList* commandList,
    nvrhi::IDevice* device,
    idImage* sourceImage)
{
    if (!commandList || !device)
    {
        return nullptr;
    }

    byte* facePixels[6] = {};
    int faceSize = 0;
    ID_TIME_T sourceTimestamp = 0;
    const idStr requestedSourceName = sourceImage
        ? sourceImage->GetName()
        : RT_SMOKE_SKY_ENVIRONMENT_FALLBACK_NAME;
    const int requestedCubeFiles = sourceImage
        ? static_cast<int>(sourceImage->GetCubeFiles())
        : 0;
    const bool sourceLoaded = sourceImage && R_LoadCubeImages(
        sourceImage->GetName(),
        sourceImage->GetCubeFiles(),
        facePixels,
        &faceSize,
        &sourceTimestamp) && faceSize > 0;
    if (sourceImage && !sourceLoaded)
    {
        common->Printf(
            "PathTracePrimaryPass: isolated sky-cube upload couldn't load source='%s' cubeFiles=%d; binding white fallback\n",
            requestedSourceName.c_str(),
            requestedCubeFiles);
        for (byte*& face : facePixels)
        {
            if (face)
            {
                Mem_Free(face);
                face = nullptr;
            }
        }
    }
    byte fallbackPixel[4] = { 255, 255, 255, 255 };
    if (!sourceLoaded)
    {
        faceSize = 1;
        for (byte*& face : facePixels)
        {
            face = fallbackPixel;
        }
    }

    nvrhi::TextureDesc cubeDesc;
    cubeDesc.width = static_cast<uint32_t>(faceSize);
    cubeDesc.height = static_cast<uint32_t>(faceSize);
    cubeDesc.arraySize = 6;
    cubeDesc.mipLevels = 1;
    cubeDesc.format = nvrhi::Format::RGBA8_UNORM;
    cubeDesc.dimension = nvrhi::TextureDimension::TextureCube;
    cubeDesc.isShaderResource = true;
    cubeDesc.initialState = nvrhi::ResourceStates::Common;
    cubeDesc.keepInitialState = false;
    cubeDesc.debugName = "PathTraceSkyEnvironmentCube";
    nvrhi::TextureHandle cubeTexture = device->createTexture(cubeDesc);
    if (cubeTexture)
    {
        commandList->beginTrackingTextureState(
            cubeTexture,
            nvrhi::AllSubresources,
            nvrhi::ResourceStates::Common);
        for (uint32_t faceIndex = 0; faceIndex < 6u; ++faceIndex)
        {
            commandList->writeTexture(
                cubeTexture,
                faceIndex,
                0,
                facePixels[faceIndex],
                static_cast<size_t>(faceSize) * 4u);
        }
        commandList->setPermanentTextureState(cubeTexture, nvrhi::ResourceStates::ShaderResource);
        commandList->commitBarriers();
    }

    for (byte*& face : facePixels)
    {
        if (sourceLoaded && face)
        {
            Mem_Free(face);
            face = nullptr;
        }
    }

    if (!cubeTexture)
    {
        common->Printf(
            "PathTracePrimaryPass: isolated sky-cube upload failed to create %dx%d cube source='%s'\n",
            faceSize,
            faceSize,
            requestedSourceName.c_str());
        return nullptr;
    }

    common->Printf(
        "PathTracePrimaryPass: isolated sky-cube upload ready source='%s' size=%d cubeFiles=%d timestamp=%llu\n",
        requestedSourceName.c_str(),
        faceSize,
        sourceLoaded ? requestedCubeFiles : 0,
        static_cast<unsigned long long>(sourceTimestamp));
    return cubeTexture;
}

struct RtSmokeRuntimeMaterialApplySample
{
    int tableIndex = -1;
    uint32_t materialId = 0;
    idStr materialName;
    idVec4 stageColor = idVec4(1.0f, 1.0f, 1.0f, 1.0f);
    idVec4 emissiveColor = idVec4(0.0f, 0.0f, 0.0f, 1.0f);
    idStr source;
    bool disabled = false;
};

struct RtSmokeRuntimeMaterialApplyStats
{
    int candidates = 0;
    int evaluated = 0;
    int skipped = 0;
    int emissiveScaled = 0;
    int emissiveDisabled = 0;
    RtSmokeRuntimeMaterialApplySample samples[RT_SMOKE_RUNTIME_MATERIAL_APPLY_SAMPLES];
    int sampleCount = 0;
};

struct CleanRtxdiDiAnalyticDomainFreezeState
{
    bool valid = false;
    idRenderWorldLocal* renderWorld = nullptr;
    idStr mapName;
    ID_TIME_T mapTimeStamp = 0;
    int lastRefreshMs = 0;
    int intervalMs = 0;
    std::vector<PathTraceDoomAnalyticLightCandidate> lights;
    PathTraceDoomAnalyticLightGpuRemap remap;

    void Reset()
    {
        valid = false;
        renderWorld = nullptr;
        mapName.Clear();
        mapTimeStamp = 0;
        lastRefreshMs = 0;
        intervalMs = 0;
        lights.clear();
        remap = PathTraceDoomAnalyticLightGpuRemap();
    }
};

CleanRtxdiDiAnalyticDomainFreezeState g_cleanRtxdiDiAnalyticDomainFreeze;

struct CleanRtxdiDiBypassLightUniverseState
{
    bool valid = false;
    idRenderWorldLocal* renderWorld = nullptr;
    idStr mapName;
    ID_TIME_T mapTimeStamp = 0;
    std::vector<PathTraceDoomAnalyticLightCandidate> previousLights;
    std::vector<uint32_t> previousKeys;

    void Reset()
    {
        valid = false;
        renderWorld = nullptr;
        mapName.Clear();
        mapTimeStamp = 0;
        previousLights.clear();
        previousKeys.clear();
    }
};

CleanRtxdiDiBypassLightUniverseState g_cleanRtxdiDiBypassLightUniverse;

bool SmokeRuntimeMaterialEvalRegister(const float* regs, int registerCount, int registerIndex, float fallback, float& value)
{
    if (regs && registerIndex >= 0 && registerIndex < registerCount)
    {
        value = regs[registerIndex];
        return true;
    }
    value = fallback;
    return false;
}

bool SmokeRuntimeMaterialStageIsEmissiveLike(const shaderStage_t* stage)
{
    if (!stage)
    {
        return false;
    }

    const uint64 srcBlend = stage->drawStateBits & GLS_SRCBLEND_BITS;
    const uint64 dstBlend = stage->drawStateBits & GLS_DSTBLEND_BITS;
    return (srcBlend == GLS_SRCBLEND_ONE || srcBlend == GLS_SRCBLEND_SRC_ALPHA) &&
        dstBlend == GLS_DSTBLEND_ONE;
}

bool SmokeRuntimeMaterialCanApplyTableWide(const char* materialName)
{
    if (!materialName || !materialName[0])
    {
        return false;
    }

    // Swinglight materials are switched through entity/material parms. A shared
    // material-table row cannot represent their per-instance on/off state yet.
    if (idStr::FindText(materialName, "swinglight", false) >= 0)
    {
        return false;
    }
    return true;
}

float SmokeRuntimeMaterialLuminance(const idVec4& color);

bool FindSmokeRuntimeMaterialEvalSample(const RtSmokeMaterialStats& materialStats, uint32_t materialId, idVec4& color, bool& disabled, idImage*& image)
{
    for (const RtSmokeDynamicMaterialEvalSample& sample : materialStats.dynamicEvalMaterialSamples)
    {
        if (!sample.valid || sample.id != materialId)
        {
            continue;
        }
        if (!sample.selectedStageEmissive)
        {
            continue;
        }

        color = idVec4(
            Max(0.0f, sample.color[0]),
            Max(0.0f, sample.color[1]),
            Max(0.0f, sample.color[2]),
            idMath::ClampFloat(0.0f, 1.0f, sample.color[3]));
        disabled = sample.condition == 0.0f ||
            sample.enabledStages <= 0 ||
            SmokeRuntimeMaterialLuminance(color) <= 1.0e-5f;
        image = sample.image;
        return true;
    }

    for (int sampleIndex = 0; sampleIndex < materialStats.dynamicEvalSampleCount; ++sampleIndex)
    {
        const RtSmokeDynamicMaterialEvalSample& sample = materialStats.dynamicEvalSamples[sampleIndex];
        if (!sample.valid || sample.id != materialId)
        {
            continue;
        }
        if (!sample.selectedStageEmissive)
        {
            continue;
        }

        color = idVec4(
            Max(0.0f, sample.color[0]),
            Max(0.0f, sample.color[1]),
            Max(0.0f, sample.color[2]),
            idMath::ClampFloat(0.0f, 1.0f, sample.color[3]));
        disabled = sample.condition == 0.0f ||
            sample.enabledStages <= 0 ||
            SmokeRuntimeMaterialLuminance(color) <= 1.0e-5f;
        image = sample.image;
        return true;
    }

    color = idVec4(0.0f, 0.0f, 0.0f, 0.0f);
    disabled = true;
    image = nullptr;
    return false;
}

bool SmokeTableMaterialIsResidentStatic(const RtSmokeMaterialTableBuild& table, int materialIndex)
{
    if (r_pathTracingResidency.GetInteger() == 0 || r_pathTracingResidencyMaterial.GetInteger() == 0)
    {
        return false;
    }
    if (materialIndex < 0 || materialIndex >= static_cast<int>(table.materialInfos.size()))
    {
        return false;
    }
    if (materialIndex < static_cast<int>(table.materialIds.size()) &&
        IsSmokeMaterialTextureVariant(table.materialIds[materialIndex]))
    {
        return false;
    }
    if (materialIndex < static_cast<int>(table.materials.size()))
    {
        const uint32_t dynamicFlags = table.materials[materialIndex].padding0 & (
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_RUNTIME_REGS |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_COLOR |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_ALPHA |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_CONDITION);
        if (dynamicFlags != 0u)
        {
            return false;
        }
    }
    const RtSmokeMaterialTextureInfo& info = table.materialInfos[materialIndex];
    return SmokeMaterialTextureInfoHasMaterialMetadata(info) && !info.isDynamic;
}

idVec4 SmokeRuntimeMaterialStageColor(const idMaterial* material, const shaderStage_t* stage, const float* regs)
{
    idVec4 color(1.0f, 1.0f, 1.0f, 1.0f);
    if (!material || !stage || !regs)
    {
        return color;
    }

    const int registerCount = material->GetNumRegisters();
    SmokeRuntimeMaterialEvalRegister(regs, registerCount, stage->color.registers[0], 1.0f, color.x);
    SmokeRuntimeMaterialEvalRegister(regs, registerCount, stage->color.registers[1], 1.0f, color.y);
    SmokeRuntimeMaterialEvalRegister(regs, registerCount, stage->color.registers[2], 1.0f, color.z);
    SmokeRuntimeMaterialEvalRegister(regs, registerCount, stage->color.registers[3], 1.0f, color.w);
    color.x = Max(0.0f, color.x);
    color.y = Max(0.0f, color.y);
    color.z = Max(0.0f, color.z);
    color.w = idMath::ClampFloat(0.0f, 1.0f, color.w);
    return color;
}

float SmokeRuntimeMaterialLuminance(const idVec4& color)
{
    return Max(0.0f, color.x) * 0.2126f +
        Max(0.0f, color.y) * 0.7152f +
        Max(0.0f, color.z) * 0.0722f;
}

int FindSmokeMaterialTableIndexById(const RtSmokeMaterialTableBuild& table, uint32_t materialId)
{
    const int materialCount = static_cast<int>(table.materialIds.size());
    for (int materialIndex = 0; materialIndex < materialCount; ++materialIndex)
    {
        if (table.materialIds[materialIndex] == materialId)
        {
            return materialIndex;
        }
    }
    return -1;
}

bool SmokeDynamicMaterialSampleHasTexMatrix(const RtSmokeDynamicMaterialEvalSample& sample)
{
    return sample.texMatrixStages > 0 ||
        idMath::Fabs(sample.texMatrix[0][0] - 1.0f) > 1.0e-6f ||
        idMath::Fabs(sample.texMatrix[0][1]) > 1.0e-6f ||
        idMath::Fabs(sample.texMatrix[0][2]) > 1.0e-6f ||
        idMath::Fabs(sample.texMatrix[1][0]) > 1.0e-6f ||
        idMath::Fabs(sample.texMatrix[1][1] - 1.0f) > 1.0e-6f ||
        idMath::Fabs(sample.texMatrix[1][2]) > 1.0e-6f;
}

// Matching-spectrum lights in view this frame, with their volumes. Spectrum
// surfaces ("invisible writing") only receive matching-spectrum lights, and a
// room can hold several with INDEPENDENT animations - each decal must follow
// the light whose volume covers it, never a global per-spectrum max (that
// cross-poisons timings and colors between set pieces).
struct RtSmokeSpectrumLight
{
    int spectrum = 0;
    idVec3 origin = idVec3(0.0f, 0.0f, 0.0f);
    float radius = 300.0f;
    idVec4 color = idVec4(0.0f, 0.0f, 0.0f, 1.0f);
};

static void BuildSmokeSpectrumLights(const viewDef_t* viewDef, std::vector<RtSmokeSpectrumLight>& spectrumLights)
{
    if (!viewDef)
    {
        return;
    }

    const float lightScale = r_lightScale.GetFloat();
    const float intensityCap = 8.0f;
    for (const viewLight_t* vLight = viewDef->viewLights; vLight != NULL; vLight = vLight->next)
    {
        if (!vLight->lightShader || !vLight->shaderRegisters || vLight->removeFromList)
        {
            continue;
        }
        const int spectrum = vLight->lightShader->Spectrum();
        if (spectrum <= 0)
        {
            continue;
        }

        RtSmokeSpectrumLight spectrumLight;
        spectrumLight.spectrum = spectrum;
        spectrumLight.origin = vLight->globalLightOrigin;
        if (vLight->lightDef)
        {
            const idVec3& lightRadiusVec = vLight->lightDef->parms.lightRadius;
            spectrumLight.radius = Max(1.0f, Max(lightRadiusVec.x, Max(lightRadiusVec.y, lightRadiusVec.z)));
        }

        const idMaterial* lightShader = vLight->lightShader;
        const float* lightRegs = vLight->shaderRegisters;
        for (int lightStageNum = 0; lightStageNum < lightShader->GetNumStages(); lightStageNum++)
        {
            const shaderStage_t* lightStage = lightShader->GetStage(lightStageNum);
            if (!lightStage || !lightRegs[lightStage->conditionRegister])
            {
                continue;
            }
            const int* registers = lightStage->color.registers;
            idVec4 color(
                Max(0.0f, lightScale * lightRegs[registers[0]]),
                Max(0.0f, lightScale * lightRegs[registers[1]]),
                Max(0.0f, lightScale * lightRegs[registers[2]]),
                1.0f);
            float luminance = Max(color.x, Max(color.y, color.z));
            if (luminance > intensityCap)
            {
                const float scale = intensityCap / luminance;
                color.x *= scale;
                color.y *= scale;
                color.z *= scale;
                luminance = intensityCap;
            }
            if (luminance > Max(spectrumLight.color.x, Max(spectrumLight.color.y, spectrumLight.color.z)))
            {
                spectrumLight.color = color;
            }
        }

        // A currently-dark matching light still claims its volume: the decal it
        // owns must go dark with it, not adopt another light's animation.
        spectrumLights.push_back(spectrumLight);
    }
}

// Nearest matching-spectrum light by volume-normalized distance when a surface
// position is known; brightest match otherwise.
static bool ChooseSmokeSpectrumLightColor(
    const std::vector<RtSmokeSpectrumLight>& spectrumLights,
    int spectrum,
    const idVec3* surfaceOrigin,
    idVec4& chosenColor)
{
    bool found = false;
    float bestScore = idMath::INFINITUM;
    float bestLuminance = -1.0f;
    for (const RtSmokeSpectrumLight& spectrumLight : spectrumLights)
    {
        if (spectrumLight.spectrum != spectrum)
        {
            continue;
        }
        const float luminance = Max(spectrumLight.color.x, Max(spectrumLight.color.y, spectrumLight.color.z));
        if (surfaceOrigin)
        {
            const float score = (spectrumLight.origin - *surfaceOrigin).Length() / spectrumLight.radius;
            if (!found || score < bestScore)
            {
                found = true;
                bestScore = score;
                chosenColor = spectrumLight.color;
            }
        }
        else if (!found || luminance > bestLuminance)
        {
            found = true;
            bestLuminance = luminance;
            chosenColor = spectrumLight.color;
        }
    }
    return found;
}

std::vector<PathTraceDynamicMaterialRecord> BuildSmokeDynamicMaterialRecords(
    const RtSmokeMaterialTableBuild& table,
    const RtSmokeMaterialStats& materialStats,
    const viewDef_t* viewDef)
{
    std::vector<PathTraceDynamicMaterialRecord> records;
    const int materialCount = Min(static_cast<int>(table.materials.size()), static_cast<int>(table.materialIds.size()));
    if (materialCount <= 0)
    {
        return records;
    }

    records.resize(materialCount);
    std::vector<std::vector<PathTraceDynamicMaterialRecord>> orderedStageRecords(materialCount);
    int validRecordCount = 0;
    std::vector<RtSmokeSpectrumLight> spectrumLights;
    bool spectrumLightsBuilt = false;

    // Dynamic-material evaluation used to be fed only while geometry was
    // appended to the dynamic fallback. Rigid-route promotion legitimately
    // removes that geometry, but it must not remove the per-draw-surface
    // shader registers that animate texture matrices, stage conditions, and
    // alpha tests. Supplement the capture samples from the visible draw list
    // so routed materials (for example textures/object/fanspin) retain their
    // runtime record.
    std::vector<RtSmokeDynamicMaterialEvalSample> dynamicSamples = materialStats.dynamicEvalMaterialSamples;
    std::unordered_set<uint32_t> sampledMaterialIds;
    for (const RtSmokeDynamicMaterialEvalSample& sample : dynamicSamples)
    {
        if (sample.valid)
        {
            sampledMaterialIds.insert(sample.id);
        }
    }
    if (viewDef && viewDef->drawSurfs)
    {
        for (int surfaceIndex = 0; surfaceIndex < viewDef->numDrawSurfs; ++surfaceIndex)
        {
            const drawSurf_t* drawSurf = viewDef->drawSurfs[surfaceIndex];
            const idMaterial* material = drawSurf ? drawSurf->material : nullptr;
            if (!material)
            {
                continue;
            }

            const uint32_t baseMaterialId = SmokeMaterialId(material);
            const uint32_t runtimeMaterialId = SmokeRuntimeMaterialTableIdForDrawSurf(drawSurf, baseMaterialId);
            if (sampledMaterialIds.find(runtimeMaterialId) != sampledMaterialIds.end())
            {
                continue;
            }

            const int materialIndex = FindSmokeMaterialTableIndexById(table, runtimeMaterialId);
            if (materialIndex < 0 || SmokeTableMaterialIsResidentStatic(table, materialIndex))
            {
                continue;
            }

            RtSmokeDynamicMaterialEvalSample sample;
            if (!BuildSmokeDynamicMaterialEvalSampleForDrawSurf(drawSurf, runtimeMaterialId, sample))
            {
                continue;
            }
            dynamicSamples.push_back(sample);
            sampledMaterialIds.insert(runtimeMaterialId);
        }
    }

    for (const RtSmokeDynamicMaterialEvalSample& sample : dynamicSamples)
    {
        if (!sample.valid)
        {
            continue;
        }

        const int materialIndex = FindSmokeMaterialTableIndexById(table, sample.id);
        if (materialIndex < 0)
        {
            continue;
        }
        if (SmokeTableMaterialIsResidentStatic(table, materialIndex))
        {
            continue;
        }

        PathTraceDynamicMaterialRecord record;
        record.color[0] = Max(0.0f, sample.color[0]);
        record.color[1] = Max(0.0f, sample.color[1]);
        record.color[2] = Max(0.0f, sample.color[2]);
        record.color[3] = idMath::ClampFloat(0.0f, 1.0f, sample.color[3]);
        record.texMatrix0[0] = sample.texMatrix[0][0];
        record.texMatrix0[1] = sample.texMatrix[0][1];
        record.texMatrix0[2] = sample.texMatrix[0][2];
        record.texMatrix0[3] = sample.condition;
        record.texMatrix1[0] = sample.texMatrix[1][0];
        record.texMatrix1[1] = sample.texMatrix[1][1];
        record.texMatrix1[2] = sample.texMatrix[1][2];
        record.texMatrix1[3] = sample.alphaTest;
        record.materialIndex = static_cast<uint32_t>(materialIndex);
        record.materialId = sample.id;
        record.stageIndex = sample.stageIndex >= 0 ? static_cast<uint32_t>(sample.stageIndex) : UINT32_MAX;
        record.flags = RT_SMOKE_DYNAMIC_MATERIAL_RECORD_VALID;
        if (sample.condition != 0.0f && sample.enabledStages > 0)
        {
            record.flags |= RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED;
        }
        if (sample.selectedStageEmissive)
        {
            record.flags |= RT_SMOKE_DYNAMIC_MATERIAL_RECORD_SELECTED_EMISSIVE;
        }
        if (IsSmokeMaterialTextureVariant(record.materialId))
        {
            record.flags |= RT_SMOKE_DYNAMIC_MATERIAL_RECORD_REPLACE_EMISSIVE;
        }
        if (SmokeDynamicMaterialSampleHasTexMatrix(sample))
        {
            record.flags |= RT_SMOKE_DYNAMIC_MATERIAL_RECORD_HAS_TEX_MATRIX;
        }
        if (sample.alphaTestStages > 0 || sample.alphaTest > 0.0f)
        {
            record.flags |= RT_SMOKE_DYNAMIC_MATERIAL_RECORD_HAS_ALPHA_TEST;
        }
        if (sample.dynamicImageStages > 0)
        {
            record.flags |= RT_SMOKE_DYNAMIC_MATERIAL_RECORD_DYNAMIC_IMAGE;
        }
        if (sample.cinematicStages > 0)
        {
            record.flags |= RT_SMOKE_DYNAMIC_MATERIAL_RECORD_CINEMATIC;
        }
        if (sample.guiRenderTargetStages > 0)
        {
            record.flags |= RT_SMOKE_DYNAMIC_MATERIAL_RECORD_GUI_RENDER_TARGET;
        }
        if (sample.programStages > 0)
        {
            record.flags |= RT_SMOKE_DYNAMIC_MATERIAL_RECORD_PROGRAM;
        }
        // Detail-decal overrides: the composite consumes record.color as the
        // decal layer's tint, which must be the DIFFUSE stage's evaluated color
        // (the generic selection above prefers the brightest stage and loses
        // e.g. alphabet4's yellow to a white bump stage). Spectrum decals are
        // additionally gated/tinted by their ASSOCIATED matching-spectrum light.
        if (materialIndex < static_cast<int>(table.materialInfos.size()))
        {
            const RtSmokeMaterialTextureInfo& info = table.materialInfos[materialIndex];
            if (info.detailDecal && sample.hasDiffuseStageColor)
            {
                record.color[0] = Max(0.0f, sample.diffuseStageColor[0]);
                record.color[1] = Max(0.0f, sample.diffuseStageColor[1]);
                record.color[2] = Max(0.0f, sample.diffuseStageColor[2]);
                record.color[3] = idMath::ClampFloat(0.0f, 1.0f, sample.diffuseStageColor[3]);
                record.texMatrix0[3] = sample.diffuseStageCondition;
                if (sample.diffuseStageCondition != 0.0f)
                {
                    record.flags |= RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED;
                }
                else
                {
                    record.flags &= ~RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED;
                }
            }
            if (info.detailDecal && info.detailDecalSpectrum > 0)
            {
                if (!spectrumLightsBuilt)
                {
                    BuildSmokeSpectrumLights(viewDef, spectrumLights);
                    spectrumLightsBuilt = true;
                }
                idVec4 lightColor(0.0f, 0.0f, 0.0f, 1.0f);
                ChooseSmokeSpectrumLightColor(
                    spectrumLights,
                    info.detailDecalSpectrum,
                    sample.hasSurfaceOrigin ? &sample.surfaceOrigin : nullptr,
                    lightColor);
                record.color[0] *= lightColor.x;
                record.color[1] *= lightColor.y;
                record.color[2] *= lightColor.z;
                record.flags |= RT_SMOKE_DYNAMIC_MATERIAL_RECORD_SELECTED_EMISSIVE;
                if (Max(record.color[0], Max(record.color[1], record.color[2])) <= 0.0f)
                {
                    record.flags &= ~RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED;
                }
            }
        }
        if ((records[materialIndex].flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_VALID) == 0u)
        {
            ++validRecordCount;
        }
        records[materialIndex] = record;
        std::vector<PathTraceDynamicMaterialRecord>& materialStageRecords = orderedStageRecords[materialIndex];
        materialStageRecords.clear();
        materialStageRecords.reserve(sample.orderedStageCount);
        for (int orderedIndex = 0; orderedIndex < sample.orderedStageCount; ++orderedIndex)
        {
            const RtSmokeDynamicStageEval& stage = sample.orderedStages[orderedIndex];
            PathTraceDynamicMaterialRecord stageRecord;
            for (int component = 0; component < 4; ++component)
            {
                stageRecord.color[component] = stage.color[component];
            }
            stageRecord.texMatrix0[0] = stage.texMatrix[0][0];
            stageRecord.texMatrix0[1] = stage.texMatrix[0][1];
            stageRecord.texMatrix0[2] = stage.texMatrix[0][2];
            stageRecord.texMatrix0[3] = stage.condition;
            stageRecord.texMatrix1[0] = stage.texMatrix[1][0];
            stageRecord.texMatrix1[1] = stage.texMatrix[1][1];
            stageRecord.texMatrix1[2] = stage.texMatrix[1][2];
            stageRecord.texMatrix1[3] = stage.alphaTest;
            stageRecord.materialIndex = static_cast<uint32_t>(materialIndex);
            stageRecord.materialId = sample.id;
            stageRecord.stageIndex = stage.stageIndex >= 0 ? static_cast<uint32_t>(stage.stageIndex) : UINT32_MAX;
            stageRecord.flags = RT_SMOKE_DYNAMIC_MATERIAL_RECORD_VALID | RT_SMOKE_DYNAMIC_MATERIAL_RECORD_ORDERED_STAGE_VALUE;
            if (stage.enabled)
            {
                stageRecord.flags |= RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED;
            }
            if (stage.emissive)
            {
                stageRecord.flags |= RT_SMOKE_DYNAMIC_MATERIAL_RECORD_SELECTED_EMISSIVE;
            }
            if (stage.hasTexMatrix)
            {
                stageRecord.flags |= RT_SMOKE_DYNAMIC_MATERIAL_RECORD_HAS_TEX_MATRIX;
            }
            if (stage.hasAlphaTest)
            {
                stageRecord.flags |= RT_SMOKE_DYNAMIC_MATERIAL_RECORD_HAS_ALPHA_TEST;
            }
            materialStageRecords.push_back(stageRecord);
        }
        if (sample.orderedStageOverflow)
        {
            records[materialIndex].stageIndex |= RT_SMOKE_DYNAMIC_MATERIAL_HEADER_STAGE_OVERFLOW;
        }
    }

    // Synthesize records for spectrum detail decals with NO eval record
    // (constant-register materials, e.g. pentastic1_spectrum): visibility and
    // tint track the associated matching-spectrum light. No matching light ->
    // stage disabled -> the composite drops the layer (invisible writing).
    const int infoCount = Min(materialCount, static_cast<int>(table.materialInfos.size()));
    for (int materialIndex = 0; materialIndex < infoCount; ++materialIndex)
    {
        const RtSmokeMaterialTextureInfo& info = table.materialInfos[materialIndex];
        if (SmokeTableMaterialIsResidentStatic(table, materialIndex))
        {
            continue;
        }
        if (!info.detailDecal || info.detailDecalSpectrum <= 0)
        {
            continue;
        }
        if ((records[materialIndex].flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_VALID) != 0u)
        {
            continue;
        }
        if (!spectrumLightsBuilt)
        {
            BuildSmokeSpectrumLights(viewDef, spectrumLights);
            spectrumLightsBuilt = true;
        }

        idVec4 lightColor(0.0f, 0.0f, 0.0f, 1.0f);
        ChooseSmokeSpectrumLightColor(spectrumLights, info.detailDecalSpectrum, nullptr, lightColor);
        const bool lit = lightColor.x > 0.0f || lightColor.y > 0.0f || lightColor.z > 0.0f;

        PathTraceDynamicMaterialRecord record;
        record.color[0] = lightColor.x;
        record.color[1] = lightColor.y;
        record.color[2] = lightColor.z;
        record.color[3] = 1.0f;
        record.texMatrix0[3] = lit ? 1.0f : 0.0f;
        record.materialIndex = static_cast<uint32_t>(materialIndex);
        record.materialId = table.materialIds[materialIndex];
        record.stageIndex = UINT32_MAX;
        // SELECTED_EMISSIVE marks the layer as self-revealing in the composite:
        // it contributes as emissive tinted by the matching light, so the
        // reveal pulses with the light instead of riding local DI.
        record.flags = RT_SMOKE_DYNAMIC_MATERIAL_RECORD_VALID | RT_SMOKE_DYNAMIC_MATERIAL_RECORD_SELECTED_EMISSIVE;
        if (lit)
        {
            record.flags |= RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED;
        }
        records[materialIndex] = record;
        ++validRecordCount;
    }

    if (validRecordCount == 0)
    {
        records.clear();
        return records;
    }

    size_t orderedRecordCount = 0;
    for (const std::vector<PathTraceDynamicMaterialRecord>& materialStageRecords : orderedStageRecords)
    {
        orderedRecordCount += materialStageRecords.size();
    }
    records.reserve(records.size() + orderedRecordCount);
    for (int materialIndex = 0; materialIndex < materialCount; ++materialIndex)
    {
        std::vector<PathTraceDynamicMaterialRecord>& materialStageRecords = orderedStageRecords[materialIndex];
        if (materialStageRecords.empty())
        {
            continue;
        }

        PathTraceDynamicMaterialRecord& header = records[materialIndex];
        const uint32_t selectedStageRaw = header.stageIndex & RT_SMOKE_DYNAMIC_MATERIAL_HEADER_SELECTED_STAGE_MASK;
        const uint32_t selectedStage = selectedStageRaw == 0xffu ? 0xffu : Min(selectedStageRaw, 0xfeu);
        const uint32_t stageCount = Min(static_cast<uint32_t>(materialStageRecords.size()), 0xfu);
        const uint32_t stageOffset = Min(static_cast<uint32_t>(records.size()), 0xffffu);
        const bool overflow =
            (header.stageIndex & RT_SMOKE_DYNAMIC_MATERIAL_HEADER_STAGE_OVERFLOW) != 0u ||
            records.size() > 0xffffu ||
            materialStageRecords.size() > 0xfu;
        header.stageIndex = selectedStage |
            (stageCount << RT_SMOKE_DYNAMIC_MATERIAL_HEADER_STAGE_COUNT_SHIFT) |
            (stageOffset << RT_SMOKE_DYNAMIC_MATERIAL_HEADER_STAGE_OFFSET_SHIFT) |
            (overflow ? RT_SMOKE_DYNAMIC_MATERIAL_HEADER_STAGE_OVERFLOW : 0u);
        header.flags |= RT_SMOKE_DYNAMIC_MATERIAL_RECORD_HAS_ORDERED_STAGE_VALUES;
        records.insert(records.end(), materialStageRecords.begin(), materialStageRecords.end());
    }
    return records;
}

bool SmokeDynamicMaterialRecordHasTexMatrix(const std::vector<PathTraceDynamicMaterialRecord>& records, uint32_t materialIndex)
{
    if (materialIndex >= records.size())
    {
        return false;
    }

    const PathTraceDynamicMaterialRecord& record = records[materialIndex];
    return (record.flags & (RT_SMOKE_DYNAMIC_MATERIAL_RECORD_VALID | RT_SMOKE_DYNAMIC_MATERIAL_RECORD_HAS_TEX_MATRIX)) ==
            (RT_SMOKE_DYNAMIC_MATERIAL_RECORD_VALID | RT_SMOKE_DYNAMIC_MATERIAL_RECORD_HAS_TEX_MATRIX) &&
        record.materialIndex == materialIndex;
}

idVec2 SmokeApplyDynamicMaterialTexMatrix(const PathTraceDynamicMaterialRecord& record, const idVec2& texCoord)
{
    return idVec2(
        record.texMatrix0[0] * texCoord.x + record.texMatrix0[1] * texCoord.y + record.texMatrix0[2],
        record.texMatrix1[0] * texCoord.x + record.texMatrix1[1] * texCoord.y + record.texMatrix1[2]);
}

int ApplySmokeDynamicMaterialTexMatricesToVertices(
    std::vector<PathTraceSmokeVertex>& vertices,
    const std::vector<uint32_t>& indexes,
    const std::vector<uint32_t>& triangleMaterialIndexes,
    const std::vector<PathTraceDynamicMaterialRecord>& records,
    int* firstTransformedVertex = nullptr,
    int* lastTransformedVertex = nullptr)
{
    if (firstTransformedVertex)
    {
        *firstTransformedVertex = -1;
    }
    if (lastTransformedVertex)
    {
        *lastTransformedVertex = -1;
    }
    if (vertices.empty() || indexes.empty() || triangleMaterialIndexes.empty() || records.empty())
    {
        return 0;
    }

    int transformedVertices = 0;
    std::vector<uint32_t> transformedMaterial(vertices.size(), UINT32_MAX);
    const int triangleCount = Min(static_cast<int>(triangleMaterialIndexes.size()), static_cast<int>(indexes.size() / 3));
    for (int triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex)
    {
        const uint32_t materialIndex = triangleMaterialIndexes[triangleIndex];
        if (!SmokeDynamicMaterialRecordHasTexMatrix(records, materialIndex))
        {
            continue;
        }

        const PathTraceDynamicMaterialRecord& record = records[materialIndex];
        const int indexOffset = triangleIndex * 3;
        for (int corner = 0; corner < 3; ++corner)
        {
            const uint32_t vertexIndex = indexes[indexOffset + corner];
            if (vertexIndex >= vertices.size())
            {
                continue;
            }

            uint32_t& transformedForMaterial = transformedMaterial[vertexIndex];
            if (transformedForMaterial != UINT32_MAX)
            {
                continue;
            }

            PathTraceSmokeVertex& vertex = vertices[vertexIndex];
            const idVec2 transformed = SmokeApplyDynamicMaterialTexMatrix(record, idVec2(vertex.texCoord[0], vertex.texCoord[1]));
            vertex.texCoord[0] = transformed.x;
            vertex.texCoord[1] = transformed.y;
            transformedForMaterial = materialIndex;
            if (firstTransformedVertex && (*firstTransformedVertex < 0 || static_cast<int>(vertexIndex) < *firstTransformedVertex))
            {
                *firstTransformedVertex = static_cast<int>(vertexIndex);
            }
            if (lastTransformedVertex && static_cast<int>(vertexIndex) > *lastTransformedVertex)
            {
                *lastTransformedVertex = static_cast<int>(vertexIndex);
            }
            ++transformedVertices;
        }
    }
    return transformedVertices;
}

bool SmokeRuntimeMaterialEvaluateRegisters(
    const viewDef_t* viewDef,
    const idMaterial* material,
    const float*& regs,
    float dynamicRegs[MAX_EXPRESSION_REGISTERS])
{
    regs = material ? material->ConstantRegisters() : nullptr;
    if (!viewDef || !material)
    {
        return false;
    }
    if (regs)
    {
        return true;
    }

    float localShaderParms[MAX_ENTITY_SHADER_PARMS] = {};
    localShaderParms[0] = 1.0f;
    localShaderParms[1] = 1.0f;
    localShaderParms[2] = 1.0f;
    localShaderParms[3] = 1.0f;
    material->EvaluateRegisters(
        dynamicRegs,
        localShaderParms,
        viewDef->renderView.shaderParms,
        viewDef->renderView.time[0] * 0.001f,
        nullptr);
    regs = dynamicRegs;
    return true;
}

void AddSmokeRuntimeMaterialApplySample(
    RtSmokeRuntimeMaterialApplyStats& stats,
    int tableIndex,
    uint32_t materialId,
    const char* materialName,
    const idVec4& stageColor,
    const PathTraceSmokeMaterial& material,
    const char* source,
    bool disabled)
{
    const int maxSamples = r_pathTracingMatClassDebugMax.GetInteger() > 0
        ? idMath::ClampInt(1, RT_SMOKE_RUNTIME_MATERIAL_APPLY_SAMPLES, r_pathTracingMatClassDebugMax.GetInteger())
        : 8;
    if (stats.sampleCount >= maxSamples)
    {
        return;
    }

    RtSmokeRuntimeMaterialApplySample& sample = stats.samples[stats.sampleCount++];
    sample.tableIndex = tableIndex;
    sample.materialId = materialId;
    sample.materialName = materialName && materialName[0] ? materialName : "<unknown>";
    sample.stageColor = stageColor;
    sample.source = source && source[0] ? source : "<unknown>";
    sample.emissiveColor = idVec4(
        material.emissiveColor[0],
        material.emissiveColor[1],
        material.emissiveColor[2],
        material.emissiveColor[3]);
    sample.disabled = disabled;
}

RtSmokeRuntimeMaterialApplyStats ApplySmokeRuntimeMaterialRegistersToTable(const viewDef_t* viewDef, RtSmokeMaterialTableBuild& table, const RtSmokeMaterialStats& materialStats, int materialTextureTableMinimum)
{
    RtSmokeRuntimeMaterialApplyStats stats;
    if (!viewDef || r_pathTracingMatClassEnable.GetInteger() == 0)
    {
        return stats;
    }

    const int materialCount = Min(static_cast<int>(table.materials.size()), static_cast<int>(table.materialIds.size()));
    for (int materialIndex = 0; materialIndex < materialCount; ++materialIndex)
    {
        PathTraceSmokeMaterial& material = table.materials[materialIndex];
        const uint32_t dynamicFlags = material.padding0 & (
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_RUNTIME_REGS |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_COLOR |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_ALPHA |
            RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_CONDITION);
        if (dynamicFlags == 0u || (material.flags & RT_SMOKE_MATERIAL_EMISSIVE) == 0u)
        {
            continue;
        }

        const RtSmokeMaterialTextureInfo* info = materialIndex < static_cast<int>(table.materialInfos.size()) ? &table.materialInfos[materialIndex] : nullptr;
        if (SmokeTableMaterialIsResidentStatic(table, materialIndex))
        {
            continue;
        }
        ++stats.candidates;
        const uint32_t materialId = table.materialIds[materialIndex];
        const char* materialName = info ? info->materialName.c_str() : nullptr;
        const bool runtimeVariant = IsSmokeMaterialTextureVariant(materialId);
        const bool canApplySharedRow = SmokeRuntimeMaterialCanApplyTableWide(materialName);
        idVec4 selectedStageColor(0.0f, 0.0f, 0.0f, 0.0f);
        if (!runtimeVariant && !canApplySharedRow)
        {
            ++stats.skipped;
            AddSmokeRuntimeMaterialApplySample(stats, materialIndex, materialId, materialName, selectedStageColor, material, "skip:shared-row", true);
            continue;
        }

        float selectedLuminance = -1.0f;
        bool activeEmissiveStage = false;
        bool disableFromLiveSample = false;
        bool selectedFromLiveSample = false;
        idImage* selectedImage = nullptr;
        const char* applySource = "decl";
        if (FindSmokeRuntimeMaterialEvalSample(materialStats, materialId, selectedStageColor, disableFromLiveSample, selectedImage))
        {
            ++stats.evaluated;
            selectedLuminance = SmokeRuntimeMaterialLuminance(selectedStageColor);
            activeEmissiveStage = !disableFromLiveSample && selectedLuminance > 1.0e-5f;
            selectedFromLiveSample = true;
            applySource = "live";
        }
        else
        {
            if (runtimeVariant)
            {
                ++stats.skipped;
                AddSmokeRuntimeMaterialApplySample(stats, materialIndex, materialId, materialName, selectedStageColor, material, "skip:no-live-emissive", true);
                continue;
            }

            const idMaterial* materialDecl = materialName && materialName[0] ? declManager->FindMaterial(materialName, false) : nullptr;
            if (!materialDecl)
            {
                ++stats.skipped;
                AddSmokeRuntimeMaterialApplySample(stats, materialIndex, materialId, materialName, selectedStageColor, material, "skip:no-decl", true);
                continue;
            }

            float dynamicRegs[MAX_EXPRESSION_REGISTERS];
            const float* regs = nullptr;
            if (!SmokeRuntimeMaterialEvaluateRegisters(viewDef, materialDecl, regs, dynamicRegs))
            {
                ++stats.skipped;
                AddSmokeRuntimeMaterialApplySample(stats, materialIndex, materialId, materialName, selectedStageColor, material, "skip:no-registers", true);
                continue;
            }
            ++stats.evaluated;

            const int registerCount = materialDecl->GetNumRegisters();
            for (int stageIndex = 0; stageIndex < materialDecl->GetNumStages(); ++stageIndex)
            {
                const shaderStage_t* stage = materialDecl->GetStage(stageIndex);
                if (!SmokeRuntimeMaterialStageIsEmissiveLike(stage) &&
                    !(stageIndex == 0 && SmokeMaterialUsesOpaqueSwinglightCompatibility(materialDecl)))
                {
                    continue;
                }

                float condition = 1.0f;
                SmokeRuntimeMaterialEvalRegister(regs, registerCount, stage->conditionRegister, 1.0f, condition);
                if (condition == 0.0f)
                {
                    continue;
                }

                const idVec4 stageColor = SmokeRuntimeMaterialStageColor(materialDecl, stage, regs);
                const float luminance = SmokeRuntimeMaterialLuminance(stageColor);
                if (luminance > selectedLuminance)
                {
                    selectedLuminance = luminance;
                    selectedStageColor = stageColor;
                }
                activeEmissiveStage = true;
            }
        }

        if (!activeEmissiveStage || selectedLuminance <= 1.0e-5f)
        {
            material.flags &= ~(RT_SMOKE_MATERIAL_EMISSIVE | RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE);
            material.emissiveColor[0] = 0.0f;
            material.emissiveColor[1] = 0.0f;
            material.emissiveColor[2] = 0.0f;
            material.emissiveColor[3] = 1.0f;
            ++stats.emissiveDisabled;
            AddSmokeRuntimeMaterialApplySample(stats, materialIndex, materialId, materialName, selectedStageColor, material, applySource, true);
            continue;
        }

        if (runtimeVariant && selectedFromLiveSample)
        {
            material.emissiveColor[0] = selectedStageColor.x * selectedStageColor.w;
            material.emissiveColor[1] = selectedStageColor.y * selectedStageColor.w;
            material.emissiveColor[2] = selectedStageColor.z * selectedStageColor.w;
        }
        else
        {
            material.emissiveColor[0] *= selectedStageColor.x * selectedStageColor.w;
            material.emissiveColor[1] *= selectedStageColor.y * selectedStageColor.w;
            material.emissiveColor[2] *= selectedStageColor.z * selectedStageColor.w;
        }
        material.emissiveColor[3] = selectedStageColor.w;
        if (SmokeRuntimeMaterialLuminance(idVec4(material.emissiveColor[0], material.emissiveColor[1], material.emissiveColor[2], material.emissiveColor[3])) <= 1.0e-5f)
        {
            material.flags &= ~(RT_SMOKE_MATERIAL_EMISSIVE | RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE);
            ++stats.emissiveDisabled;
            AddSmokeRuntimeMaterialApplySample(stats, materialIndex, materialId, materialName, selectedStageColor, material, applySource, true);
        }
        else
        {
            if (runtimeVariant && selectedFromLiveSample && selectedImage)
            {
                BindSmokeMaterialRuntimeEmissiveTexture(table, materialIndex, selectedImage, materialTextureTableMinimum);
            }
            ++stats.emissiveScaled;
            AddSmokeRuntimeMaterialApplySample(stats, materialIndex, materialId, materialName, selectedStageColor, material, applySource, false);
        }
    }

    if (r_pathTracingSmokeLog.GetInteger() != 0 && stats.candidates > 0)
    {
        common->Printf("PathTracePrimaryPass: RT smoke runtime material register apply candidates=%d evaluated=%d skipped=%d emissiveScaled=%d emissiveDisabled=%d samples=",
            stats.candidates,
            stats.evaluated,
            stats.skipped,
            stats.emissiveScaled,
            stats.emissiveDisabled);
        for (int sampleIndex = 0; sampleIndex < stats.sampleCount; ++sampleIndex)
        {
            const RtSmokeRuntimeMaterialApplySample& sample = stats.samples[sampleIndex];
            common->Printf("%sindex=%d id=%u material='%s' source=%s disabled=%d stageColor=(%.3f %.3f %.3f %.3f) emissive=(%.3f %.3f %.3f %.3f)",
                sampleIndex == 0 ? "" : ", ",
                sample.tableIndex,
                sample.materialId,
                sample.materialName.c_str(),
                sample.source.c_str(),
                sample.disabled ? 1 : 0,
                sample.stageColor.x,
                sample.stageColor.y,
                sample.stageColor.z,
                sample.stageColor.w,
                sample.emissiveColor.x,
                sample.emissiveColor.y,
                sample.emissiveColor.z,
                sample.emissiveColor.w);
        }
        common->Printf("\n");
    }
    return stats;
}

void LogSmokeMaterialClassifierLiveSummary(const RtSmokeMaterialTableBuild& table, const RtMaterialClassifierStats& stats)
{
    if (r_pathTracingSmokeLog.GetInteger() == 0 || r_pathTracingMatClassEnable.GetInteger() == 0)
    {
        return;
    }

    common->Printf("PathTracePrimaryPass: RT smoke material classifier live records=%d hits=%d misses=%d rebuilds=%d frame=%d/%d/%d frameRoutes(rmao/legacy/fallback)=%d/%d/%d frameConfidence(auth/flag/heur/fallback)=%d/%d/%d/%d compositing(stages/max/>4/>8)=%d/%d/%d/%d ops(opaque/add/mul/invert/over/clip/interaction/unknown)=%d/%d/%d/%d/%d/%d/%d/%d\n",
        stats.records,
        stats.hits,
        stats.misses,
        stats.rebuilds,
        stats.frameHits,
        stats.frameMisses,
        stats.frameRebuilds,
        stats.routeRealPbr,
        stats.routeLegacySpec,
        stats.routeFallback,
        stats.confidenceAuthoritative,
        stats.confidenceFlag,
        stats.confidenceHeuristic,
        stats.confidenceFallbackNone,
        stats.compositingStages,
        stats.maxCompositingStages,
        stats.materialsOverFourStages,
        stats.materialsOverEightStages,
        stats.compositingOpaque,
        stats.compositingAdditive,
        stats.compositingMultiply,
        stats.compositingInverted,
        stats.compositingAlphaOver,
        stats.compositingAlphaClip,
        stats.compositingInteraction,
        stats.compositingUnknown);

    int orderedSafeMaterials = 0;
    int orderedStageTextureMaterials = 0;
    int orderedRuntimeValueMaterials = 0;
    int orderedOverflowMaterials = 0;
    int orderedUnhandledMaterials = 0;
    std::vector<const RtMaterialRecord*> orderedStageTextureSamples;
    std::vector<const RtMaterialRecord*> orderedUnhandledSamples;
    for (uint32_t materialId : table.materialIds)
    {
        const RtMaterialRecord* record = FindPathTraceMaterialRecord(materialId);
        if (!record || !record->valid)
        {
            continue;
        }

        std::vector<idStr> effectImageNames;
        bool needsRuntimeValues = false;
        bool hasUnhandledEquation = false;
        for (const RtMaterialCompositingStageFact& stage : record->compositingStages)
        {
            const bool destinationPreserve =
                stage.srcBlendBits == GLS_SRCBLEND_ZERO &&
                stage.dstBlendBits == GLS_DSTBLEND_ONE;
            switch (stage.operation)
            {
                case RtMaterialCompositingOp::Additive:
                case RtMaterialCompositingOp::MultiplyFilter:
                case RtMaterialCompositingOp::InvertedFilterBlackKey:
                case RtMaterialCompositingOp::SourceAlphaOver:
                    if (!stage.imageName.IsEmpty() &&
                        std::find(effectImageNames.begin(), effectImageNames.end(), stage.imageName) == effectImageNames.end())
                    {
                        effectImageNames.push_back(stage.imageName);
                    }
                    break;
                case RtMaterialCompositingOp::Unknown:
                    hasUnhandledEquation |= !destinationPreserve;
                    break;
                default:
                    break;
            }
            needsRuntimeValues |=
                stage.conditionIsDynamic ||
                stage.dynamicImage != DI_STATIC ||
                stage.vertexProgram >= 0 ||
                stage.fragmentProgram >= 0 ||
                stage.glslProgram >= 0;
        }

        const bool overflow = record->compositingStages.size() > 8;
        const bool needsStageTextures = effectImageNames.size() > 1;
        orderedOverflowMaterials += overflow ? 1 : 0;
        orderedUnhandledMaterials += hasUnhandledEquation ? 1 : 0;
        orderedRuntimeValueMaterials += needsRuntimeValues ? 1 : 0;
        orderedStageTextureMaterials += needsStageTextures ? 1 : 0;
        orderedSafeMaterials += !overflow && !hasUnhandledEquation && !needsRuntimeValues && !needsStageTextures ? 1 : 0;
        if (needsStageTextures && orderedStageTextureSamples.size() < 8)
        {
            orderedStageTextureSamples.push_back(record);
        }
        if (hasUnhandledEquation && orderedUnhandledSamples.size() < 8)
        {
            orderedUnhandledSamples.push_back(record);
        }
    }
    common->Printf(
        "PathTracePrimaryPass: RT smoke ordered consumer gate materials=%d safeSingleEffect=%d requireStageTextures=%d requireRuntimeValues=%d overflow=%d unhandledEquation=%d\n",
        static_cast<int>(table.materialIds.size()),
        orderedSafeMaterials,
        orderedStageTextureMaterials,
        orderedRuntimeValueMaterials,
        orderedOverflowMaterials,
        orderedUnhandledMaterials);
    if (!orderedStageTextureSamples.empty() || !orderedUnhandledSamples.empty())
    {
        common->Printf("PathTracePrimaryPass: RT smoke ordered consumer blocker samples stageTextures=");
        for (int sampleIndex = 0; sampleIndex < static_cast<int>(orderedStageTextureSamples.size()); ++sampleIndex)
        {
            const RtMaterialRecord* record = orderedStageTextureSamples[sampleIndex];
            common->Printf("%s%u:'%s'", sampleIndex > 0 ? ", " : "", record->materialId, record->materialName.c_str());
        }
        common->Printf(" unhandled=");
        for (int sampleIndex = 0; sampleIndex < static_cast<int>(orderedUnhandledSamples.size()); ++sampleIndex)
        {
            const RtMaterialRecord* record = orderedUnhandledSamples[sampleIndex];
            common->Printf("%s%u:'%s'", sampleIndex > 0 ? ", " : "", record->materialId, record->materialName.c_str());
            common->Printf("[");
            bool printedStage = false;
            for (const RtMaterialCompositingStageFact& stage : record->compositingStages)
            {
                const bool destinationPreserve =
                    stage.srcBlendBits == GLS_SRCBLEND_ZERO &&
                    stage.dstBlendBits == GLS_DSTBLEND_ONE;
                if (stage.operation != RtMaterialCompositingOp::Unknown || destinationPreserve)
                {
                    continue;
                }
                common->Printf("%sstage=%d lighting=%d blend=0x%llx/0x%llx image='%s'",
                    printedStage ? ";" : "",
                    stage.stageIndex,
                    static_cast<int>(stage.lighting),
                    static_cast<unsigned long long>(stage.srcBlendBits),
                    static_cast<unsigned long long>(stage.dstBlendBits),
                    stage.imageName.c_str());
                printedStage = true;
            }
            common->Printf("]");
        }
        common->Printf("\n");
    }

    const int materialCount = Min(static_cast<int>(table.materials.size()), static_cast<int>(table.materialIds.size()));
    int routeFallbackTotal = 0;
    int confidenceFallbackTotal = 0;
    for (int materialIndex = 0; materialIndex < materialCount; ++materialIndex)
    {
        const RtMaterialRecord* record = FindPathTraceMaterialRecord(table.materialIds[materialIndex]);
        if (!record || !record->valid)
        {
            continue;
        }
        const bool routeFallback = record->route == RtMaterialBsdfRoute::SurfaceTypeFallback;
        const bool confidenceFallback = record->surfaceClassConfidence == RtMaterialClassConfidence::FallbackNone;
        routeFallbackTotal += routeFallback ? 1 : 0;
        confidenceFallbackTotal += confidenceFallback ? 1 : 0;
    }
    if (routeFallbackTotal <= 0 && confidenceFallbackTotal <= 0)
    {
        return;
    }

    const int maxFallbackSamples = r_pathTracingMatClassDebugMax.GetInteger() > 0
        ? idMath::ClampInt(1, 64, r_pathTracingMatClassDebugMax.GetInteger())
        : 8;

    common->Printf("PathTracePrimaryPass: RT smoke material classifier fallback samples routeFallback=%d confidenceFallback=%d samples=",
        routeFallbackTotal,
        confidenceFallbackTotal);
    int sampleCount = 0;
    for (int materialIndex = 0; materialIndex < materialCount; ++materialIndex)
    {
        const RtMaterialRecord* record = FindPathTraceMaterialRecord(table.materialIds[materialIndex]);
        if (!record || !record->valid)
        {
            continue;
        }
        const bool routeFallback = record->route == RtMaterialBsdfRoute::SurfaceTypeFallback;
        const bool confidenceFallback = record->surfaceClassConfidence == RtMaterialClassConfidence::FallbackNone;
        if (!routeFallback && !confidenceFallback)
        {
            continue;
        }
        if (sampleCount < maxFallbackSamples)
        {
            common->Printf("%sindex=%d id=%u material='%s' route=%s routeReason=%s class=%s classReason=%s confidence=%s evidence='%s'",
                sampleCount == 0 ? "" : ", ",
                materialIndex,
                table.materialIds[materialIndex],
                record->materialName.c_str(),
                RtMaterialBsdfRouteName(record->route),
                RtMaterialBsdfRouteReasonName(record->routeReason),
                RtMaterialSurfaceClassName(record->surfaceClass),
                RtMaterialSurfaceClassReasonName(record->surfaceClassReason),
                RtMaterialClassConfidenceName(record->surfaceClassConfidence),
                record->surfaceClassEvidence.c_str());
        }
        ++sampleCount;
    }
    common->Printf("%s\n", sampleCount > maxFallbackSamples ? ", ..." : "");
}

void ApplyCleanRtxdiDiAnalyticDomainFreeze(
    const viewDef_t* viewDef,
    std::vector<PathTraceDoomAnalyticLightCandidate>& doomAnalyticLights,
    PathTraceDoomAnalyticLightGpuRemap& doomAnalyticRemap)
{
    CleanRtxdiDiAnalyticDomainFreezeState& freeze = g_cleanRtxdiDiAnalyticDomainFreeze;
    const int intervalMs = idMath::ClampInt(0, 600000, r_pathTracingCleanRtxdiDiAnalyticDomainFreezeMs.GetInteger());
    if (intervalMs <= 0 || r_pathTracingCleanRtxdiDiEnable.GetInteger() == 0)
    {
        if (freeze.valid)
        {
            freeze.Reset();
        }
        return;
    }

    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    const char* mapName = renderWorld ? renderWorld->mapName.c_str() : "";
    const ID_TIME_T mapTimeStamp = renderWorld ? renderWorld->mapTimeStamp : 0;
    if (!freeze.valid ||
        freeze.renderWorld != renderWorld ||
        freeze.mapName.Icmp(mapName) != 0 ||
        freeze.mapTimeStamp != mapTimeStamp ||
        freeze.intervalMs != intervalMs)
    {
        freeze.valid = true;
        freeze.renderWorld = renderWorld;
        freeze.mapName = mapName;
        freeze.mapTimeStamp = mapTimeStamp;
        freeze.intervalMs = intervalMs;
        freeze.lastRefreshMs = Sys_Milliseconds();
        freeze.lights = doomAnalyticLights;
        freeze.remap = doomAnalyticRemap;
        return;
    }

    const int nowMs = Sys_Milliseconds();
    if (nowMs - freeze.lastRefreshMs >= intervalMs)
    {
        freeze.lastRefreshMs = nowMs;
        freeze.lights = doomAnalyticLights;
        freeze.remap = doomAnalyticRemap;
        return;
    }

    doomAnalyticLights = freeze.lights;
    doomAnalyticRemap = freeze.remap;
}

uint32_t CleanRtxdiDiBypassUniverseIndex(const PathTraceDoomAnalyticLightCandidate& light, uint32_t fallbackIndex)
{
    uint32_t key = light.renderLightIndex != PATH_TRACE_DOOM_ANALYTIC_LIGHT_INVALID_INDEX
        ? light.renderLightIndex
        : fallbackIndex;
    key = (key * 16777619u) ^ light.entityNumber;
    return key != PATH_TRACE_DOOM_ANALYTIC_LIGHT_INVALID_INDEX ? key : fallbackIndex;
}

PathTraceDoomAnalyticLightGpuRemap BuildCleanRtxdiDiBypassLightUniverseRemap(
    const viewDef_t* viewDef,
    const std::vector<PathTraceDoomAnalyticLightCandidate>& doomAnalyticLights)
{
    CleanRtxdiDiBypassLightUniverseState& state = g_cleanRtxdiDiBypassLightUniverse;
    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    const char* mapName = renderWorld ? renderWorld->mapName.c_str() : "";
    const ID_TIME_T mapTimeStamp = renderWorld ? renderWorld->mapTimeStamp : 0;
    if (!state.valid ||
        state.renderWorld != renderWorld ||
        state.mapName.Icmp(mapName) != 0 ||
        state.mapTimeStamp != mapTimeStamp)
    {
        state.valid = true;
        state.renderWorld = renderWorld;
        state.mapName = mapName;
        state.mapTimeStamp = mapTimeStamp;
        state.previousLights = doomAnalyticLights;
        state.previousKeys.resize(doomAnalyticLights.size());
        for (int i = 0; i < static_cast<int>(doomAnalyticLights.size()); ++i)
        {
            state.previousKeys[i] = CleanRtxdiDiBypassUniverseIndex(doomAnalyticLights[i], static_cast<uint32_t>(i));
        }
    }

    PathTraceDoomAnalyticLightGpuRemap remap;
    const uint32_t sampleableFlags =
        PATH_TRACE_DOOM_ANALYTIC_IDENTITY_VALID |
        PATH_TRACE_DOOM_ANALYTIC_IDENTITY_SAMPLEABLE;
    const uint32_t remappableFlags =
        PATH_TRACE_DOOM_ANALYTIC_IDENTITY_VALID |
        PATH_TRACE_DOOM_ANALYTIC_IDENTITY_SAMPLEABLE |
        PATH_TRACE_DOOM_ANALYTIC_IDENTITY_REMAP_VALID;

    std::vector<uint32_t> currentKeys(doomAnalyticLights.size());
    for (int i = 0; i < static_cast<int>(doomAnalyticLights.size()); ++i)
    {
        currentKeys[i] = CleanRtxdiDiBypassUniverseIndex(doomAnalyticLights[i], static_cast<uint32_t>(i));
    }

    std::unordered_map<uint32_t, int> currentIndexByKey;
    currentIndexByKey.reserve(currentKeys.size());
    for (int i = 0; i < static_cast<int>(currentKeys.size()); ++i)
    {
        currentIndexByKey.emplace(currentKeys[i], i);
    }

    std::unordered_map<uint32_t, int> previousIndexByKey;
    previousIndexByKey.reserve(state.previousKeys.size());
    for (int i = 0; i < static_cast<int>(state.previousKeys.size()); ++i)
    {
        previousIndexByKey.emplace(state.previousKeys[i], i);
    }

    std::vector<uint32_t> remapKeys = currentKeys;
    for (const uint32_t previousKey : state.previousKeys)
    {
        if (currentIndexByKey.find(previousKey) == currentIndexByKey.end())
        {
            remapKeys.push_back(previousKey);
        }
    }

    remap.previousCandidates = state.previousLights;
    remap.currentCandidateIdentities.resize(doomAnalyticLights.size());
    remap.previousCandidateIdentities.resize(state.previousLights.size());
    remap.universeRemap.resize(remapKeys.size());

    for (int remapSlot = 0; remapSlot < static_cast<int>(remapKeys.size()); ++remapSlot)
    {
        const uint32_t key = remapKeys[remapSlot];
        const auto currentIt = currentIndexByKey.find(key);
        const auto previousIt = previousIndexByKey.find(key);
        const bool hasCurrent = currentIt != currentIndexByKey.end();
        const bool hasPrevious = previousIt != previousIndexByKey.end();
        const uint32_t flags = hasCurrent && hasPrevious ? remappableFlags : sampleableFlags;

        PathTraceDoomAnalyticLightRemap& entry = remap.universeRemap[remapSlot];
        entry.previousToCurrentCandidateIndex = hasCurrent ? currentIt->second : -1;
        entry.currentToPreviousCandidateIndex = hasPrevious ? previousIt->second : -1;
        entry.flags = flags;
        entry.invalidReasonFlags = 0;

        if (hasCurrent)
        {
            PathTraceDoomAnalyticLightCandidateIdentity& identity = remap.currentCandidateIdentities[currentIt->second];
            identity.universeIndex = key;
            identity.flags = flags;
            identity.invalidReasonFlags = 0;
            identity.remapIndex = static_cast<uint32_t>(remapSlot);
        }
        if (hasPrevious)
        {
            PathTraceDoomAnalyticLightCandidateIdentity& identity = remap.previousCandidateIdentities[previousIt->second];
            identity.universeIndex = key;
            identity.flags = flags;
            identity.invalidReasonFlags = 0;
            identity.remapIndex = static_cast<uint32_t>(remapSlot);
        }
    }

    state.previousLights = doomAnalyticLights;
    state.previousKeys = currentKeys;
    return remap;
}

template< typename T >
RtSmokePlanDataSpan MakeSmokePlanDataSpan(const std::vector<T>& data)
{
    RtSmokePlanDataSpan span;
    span.data = data.empty() ? nullptr : data.data();
    span.elementSize = sizeof(T);
    span.elementCount = data.size();
    return span;
}

size_t SmokeRequiredBufferBytes(size_t byteSize, size_t structStride)
{
    return byteSize > structStride ? byteSize : structStride;
}

bool SmokeBufferHasPayloadCapacity(nvrhi::BufferHandle buffer, size_t byteSize, size_t structStride)
{
    return buffer && buffer->getDesc().byteSize >= SmokeRequiredBufferBytes(byteSize, structStride);
}

size_t SmokeBufferCapacityElements(nvrhi::BufferHandle buffer, size_t structStride)
{
    if (!buffer || structStride == 0)
    {
        return 0;
    }
    return buffer->getDesc().byteSize / structStride;
}

template< typename T >
uint64_t BuildSmokeVectorUploadSignature(const std::vector<T>& data)
{
    const RtSmokePlanDataSpan spans[] = {
        MakeSmokePlanDataSpan(data)
    };
    return BuildSmokePlanDataSpanSignature(
        spans,
        static_cast<int>(sizeof(spans) / sizeof(spans[0])));
}

uint64_t BuildSmokePreviousStaticGeometryUploadSignature(
    uint64_t snapshotGeneration,
    uint64_t snapshotMaterialGeneration,
    size_t vertexCount,
    size_t indexCount,
    size_t triangleClassCount,
    size_t triangleMaterialCount)
{
    uint64_t hash = 1469598103934665603ull;
    const uint64_t version = 2;
    hash = HashSmokeBytes(hash, &version, sizeof(version));
    hash = HashSmokeBytes(hash, &snapshotGeneration, sizeof(snapshotGeneration));
    hash = HashSmokeBytes(hash, &snapshotMaterialGeneration, sizeof(snapshotMaterialGeneration));
    const uint64_t counts[] = {
        static_cast<uint64_t>(vertexCount),
        static_cast<uint64_t>(indexCount),
        static_cast<uint64_t>(triangleClassCount),
        static_cast<uint64_t>(triangleMaterialCount)
    };
    hash = HashSmokeBytes(hash, counts, sizeof(counts));
    return hash;
}

bool SmokeBufferCanSkipVectorUpload(
    nvrhi::BufferHandle currentBuffer,
    nvrhi::BufferHandle previousBuffer,
    size_t byteSize,
    size_t structStride,
    bool previousSignatureValid,
    uint64_t previousSignature,
    uint64_t currentSignature)
{
    return
        currentBuffer &&
        currentBuffer == previousBuffer &&
        SmokeBufferHasPayloadCapacity(currentBuffer, byteSize, structStride) &&
        previousSignatureValid &&
        previousSignature == currentSignature;
}

template< typename T >
bool FindSmokeVectorChangedRange(
    const std::vector<T>& previousRecords,
    const std::vector<T>& currentRecords,
    int& firstChanged,
    int& changedCount)
{
    firstChanged = -1;
    changedCount = 0;

    int lastChanged = -1;
    const int previousCount = static_cast<int>(previousRecords.size());
    const int currentCount = static_cast<int>(currentRecords.size());
    const int sharedCount = Min(previousCount, currentCount);
    for (int recordIndex = 0; recordIndex < sharedCount; ++recordIndex)
    {
        if (std::memcmp(&previousRecords[recordIndex], &currentRecords[recordIndex], sizeof(T)) == 0)
        {
            continue;
        }
        if (firstChanged < 0)
        {
            firstChanged = recordIndex;
        }
        lastChanged = recordIndex;
    }
    if (currentCount > previousCount)
    {
        if (firstChanged < 0)
        {
            firstChanged = previousCount;
        }
        lastChanged = currentCount - 1;
    }

    if (firstChanged < 0)
    {
        return true;
    }
    changedCount = lastChanged - firstChanged + 1;
    return true;
}

const int RT_SMOKE_UPLOAD_DIFF_SAMPLE_COUNT = 4;

const PathTraceDynamicMaterialRecord* FindSmokeOrderedDynamicStageRecord(
    uint32_t materialIndex,
    const std::vector<PathTraceDynamicMaterialRecord>& records,
    const PathTraceDynamicMaterialRecord& header,
    uint32_t requiredFlags,
    bool preferBrightestEnabled)
{
    if ((header.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_HAS_ORDERED_STAGE_VALUES) == 0u)
    {
        return nullptr;
    }
    const uint32_t stageCount =
        (header.stageIndex & RT_SMOKE_DYNAMIC_MATERIAL_HEADER_STAGE_COUNT_MASK) >>
        RT_SMOKE_DYNAMIC_MATERIAL_HEADER_STAGE_COUNT_SHIFT;
    const uint32_t stageOffset =
        (header.stageIndex & RT_SMOKE_DYNAMIC_MATERIAL_HEADER_STAGE_OFFSET_MASK) >>
        RT_SMOKE_DYNAMIC_MATERIAL_HEADER_STAGE_OFFSET_SHIFT;
    if (stageCount == 0u || stageOffset >= records.size() || stageCount > records.size() - stageOffset)
    {
        return nullptr;
    }

    const PathTraceDynamicMaterialRecord* selected = nullptr;
    float selectedScore = -1.0f;
    for (uint32_t stageSlot = 0; stageSlot < stageCount; ++stageSlot)
    {
        const PathTraceDynamicMaterialRecord& stage = records[stageOffset + stageSlot];
        if (stage.materialIndex != materialIndex ||
            (stage.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_ORDERED_STAGE_VALUE) == 0u ||
            (stage.flags & requiredFlags) != requiredFlags)
        {
            continue;
        }
        if (!preferBrightestEnabled)
        {
            return &stage;
        }

        const bool enabled = (stage.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED) != 0u;
        const float luminance = SmokeRuntimeMaterialLuminance(
            idVec4(stage.color[0], stage.color[1], stage.color[2], stage.color[3]));
        const float score = (enabled ? 1000000.0f : 0.0f) + luminance;
        if (!selected || score > selectedScore)
        {
            selected = &stage;
            selectedScore = score;
        }
    }
    return selected;
}

void ApplySmokeDynamicMaterialRecordToGpuMaterial(uint32_t materialIndex, const std::vector<PathTraceDynamicMaterialRecord>& records, PathTraceSmokeMaterial& material)
{
    if (materialIndex >= records.size())
    {
        return;
    }

    const PathTraceDynamicMaterialRecord& header = records[materialIndex];
    if ((header.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_VALID) == 0u ||
        header.materialIndex != materialIndex)
    {
        return;
    }
    const PathTraceDynamicMaterialRecord* orderedRecord = FindSmokeOrderedDynamicStageRecord(
        materialIndex,
        records,
        header,
        RT_SMOKE_DYNAMIC_MATERIAL_RECORD_SELECTED_EMISSIVE,
        true);
    const PathTraceDynamicMaterialRecord* selectedRecord = orderedRecord;
    if (!selectedRecord && (header.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_SELECTED_EMISSIVE) != 0u)
    {
        selectedRecord = &header;
    }
    if (!selectedRecord)
    {
        return;
    }
    const PathTraceDynamicMaterialRecord& record = *selectedRecord;
    const uint32_t dynamicFlags = material.padding0 & (
        RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_RUNTIME_REGS |
        RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_COLOR |
        RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_ALPHA |
        RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_CONDITION);
    if (dynamicFlags == 0u || (material.flags & RT_SMOKE_MATERIAL_EMISSIVE) == 0u)
    {
        return;
    }

    const bool stageEnabled =
        (record.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED) != 0u &&
        record.texMatrix0[3] != 0.0f &&
        Max(Max(record.color[0], record.color[1]), record.color[2]) > 1.0e-5f;
    if (!stageEnabled)
    {
        material.emissiveColor[0] = 0.0f;
        material.emissiveColor[1] = 0.0f;
        material.emissiveColor[2] = 0.0f;
        material.emissiveColor[3] = 1.0f;
        material.flags &= ~(RT_SMOKE_MATERIAL_EMISSIVE | RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE);
        return;
    }

    const float stageAlpha = idMath::ClampFloat(0.0f, 1.0f, record.color[3]);
    const float stageScale[3] = {
        Max(0.0f, record.color[0]) * stageAlpha,
        Max(0.0f, record.color[1]) * stageAlpha,
        Max(0.0f, record.color[2]) * stageAlpha
    };
    if ((header.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_REPLACE_EMISSIVE) != 0u)
    {
        material.emissiveColor[0] = stageScale[0];
        material.emissiveColor[1] = stageScale[1];
        material.emissiveColor[2] = stageScale[2];
    }
    else
    {
        material.emissiveColor[0] *= stageScale[0];
        material.emissiveColor[1] *= stageScale[1];
        material.emissiveColor[2] *= stageScale[2];
    }
    material.emissiveColor[3] = stageAlpha;
    if (SmokeRuntimeMaterialLuminance(idVec4(material.emissiveColor[0], material.emissiveColor[1], material.emissiveColor[2], material.emissiveColor[3])) <= 1.0e-5f)
    {
        material.flags &= ~(RT_SMOKE_MATERIAL_EMISSIVE | RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE);
    }
    else
    {
        material.flags |= RT_SMOKE_MATERIAL_EMISSIVE;
    }
}

void ApplySmokeDynamicAlphaRecordToGpuMaterial(uint32_t materialIndex, const std::vector<PathTraceDynamicMaterialRecord>& records, PathTraceSmokeMaterial& material)
{
    if (materialIndex >= records.size())
    {
        return;
    }

    const PathTraceDynamicMaterialRecord& header = records[materialIndex];
    if ((header.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_VALID) == 0u ||
        header.materialIndex != materialIndex)
    {
        return;
    }
    const PathTraceDynamicMaterialRecord* orderedRecord = FindSmokeOrderedDynamicStageRecord(
        materialIndex,
        records,
        header,
        RT_SMOKE_DYNAMIC_MATERIAL_RECORD_HAS_ALPHA_TEST,
        false);
    const PathTraceDynamicMaterialRecord* selectedRecord = orderedRecord;
    if (!selectedRecord && (header.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_HAS_ALPHA_TEST) != 0u)
    {
        selectedRecord = &header;
    }
    if (!selectedRecord)
    {
        return;
    }
    const PathTraceDynamicMaterialRecord& record = *selectedRecord;

    const bool stageEnabled =
        (record.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED) != 0u &&
        record.texMatrix0[3] != 0.0f;
    if (!stageEnabled)
    {
        material.flags &= ~RT_SMOKE_MATERIAL_ALPHA_TEST;
        return;
    }

    material.flags |= RT_SMOKE_MATERIAL_ALPHA_TEST;
    material.alphaCutoff = idMath::ClampFloat(0.0f, 1.0f, record.texMatrix1[3]);
}

bool CanUploadStableSmokeMaterialTableWithDynamicOverrides(
    const std::vector<PathTraceSmokeMaterial>& stableMaterials,
    const std::vector<PathTraceSmokeMaterial>& liveMaterials,
    const std::vector<PathTraceDynamicMaterialRecord>& dynamicRecords)
{
    if (stableMaterials.size() != liveMaterials.size())
    {
        return false;
    }

    for (int materialIndex = 0; materialIndex < static_cast<int>(liveMaterials.size()); ++materialIndex)
    {
        if (materialIndex < static_cast<int>(dynamicRecords.size()) &&
            (dynamicRecords[materialIndex].flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_HAS_ALPHA_TEST) != 0u)
        {
            // Shader-side dynamic records currently overlay emission only. Keep
            // dynamic alpha in the live per-variant material row until that ABI is
            // generalized, otherwise the stable table restores the static cutoff.
            return false;
        }
        const PathTraceSmokeMaterial& stable = stableMaterials[materialIndex];
        const PathTraceSmokeMaterial& live = liveMaterials[materialIndex];
        PathTraceSmokeMaterial overlaid = stable;
        ApplySmokeDynamicMaterialRecordToGpuMaterial(static_cast<uint32_t>(materialIndex), dynamicRecords, overlaid);
        if (std::memcmp(&overlaid, &live, sizeof(PathTraceSmokeMaterial)) != 0)
        {
            return false;
        }
    }

    return true;
}

uint64_t BuildSmokeRigidRouteMaterialIdSignature(const std::vector<uint32_t>& materialTableIds)
{
    uint64_t hash = 14695981039346656037ull;
    const uint64_t materialCount = static_cast<uint64_t>(materialTableIds.size());
    hash = HashSmokeBytes(hash, &materialCount, sizeof(materialCount));
    if (!materialTableIds.empty())
    {
        hash = HashSmokeBytes(hash, materialTableIds.data(), materialTableIds.size() * sizeof(materialTableIds[0]));
    }
    return hash;
}

uint64_t BuildSmokeRigidRouteStructureToken(
    const RtSmokeRigidTlasPlan& plan,
    const std::vector<uint32_t>& materialTableIds)
{
    uint64_t hash = BuildSmokeRigidRouteMaterialIdSignature(materialTableIds);
    const int instanceCount = static_cast<int>(plan.instances.size());
    hash = HashSmokeBytes(hash, &plan.visibleInstances, sizeof(plan.visibleInstances));
    hash = HashSmokeBytes(hash, &plan.rigidInstances, sizeof(plan.rigidInstances));
    hash = HashSmokeBytes(hash, &plan.emittedInstances, sizeof(plan.emittedInstances));
    hash = HashSmokeBytes(hash, &plan.rejectedNonRigid, sizeof(plan.rejectedNonRigid));
    hash = HashSmokeBytes(hash, &plan.rejectedMissingMesh, sizeof(plan.rejectedMissingMesh));
    hash = HashSmokeBytes(hash, &plan.rejectedStaleMesh, sizeof(plan.rejectedStaleMesh));
    hash = HashSmokeBytes(hash, &plan.rejectedMissingBlas, sizeof(plan.rejectedMissingBlas));
    hash = HashSmokeBytes(hash, &instanceCount, sizeof(instanceCount));
    for (const RtSmokePlanTlasInstance& instance : plan.instances)
    {
        hash = HashSmokeBytes(hash, &instance.kind, sizeof(instance.kind));
        hash = HashSmokeBytes(hash, &instance.instanceId, sizeof(instance.instanceId));
        hash = HashSmokeBytes(hash, &instance.instanceMask, sizeof(instance.instanceMask));
        hash = HashSmokeBytes(hash, &instance.hitGroupContribution, sizeof(instance.hitGroupContribution));
        hash = HashSmokeBytes(hash, &instance.meshHash, sizeof(instance.meshHash));
        hash = HashSmokeBytes(hash, &instance.sourceInstanceId, sizeof(instance.sourceInstanceId));
        hash = HashSmokeBytes(hash, &instance.routeRecordIndex, sizeof(instance.routeRecordIndex));
    }
    return hash;
}

uint64_t BuildSmokeRigidRoutePayloadToken(const RtPathTraceRigidRouteBuildSnapshot& snapshot)
{
    uint64_t hash = 14695981039346656037ull;
    const uint64_t meshCount = static_cast<uint64_t>(snapshot.meshes.size());
    hash = HashSmokeBytes(hash, &meshCount, sizeof(meshCount));
    for (const RtPathTraceRigidRouteMeshSnapshot& mesh : snapshot.meshes)
    {
        const uint32_t valid = mesh.valid ? 1u : 0u;
        const uint32_t routeReady = mesh.routeReady ? 1u : 0u;
        const uint32_t localBoundsValid = mesh.localBoundsValid ? 1u : 0u;
        const uint64_t vertexCount = static_cast<uint64_t>(mesh.vertexCount);
        const uint64_t indexCount = static_cast<uint64_t>(mesh.indexCount);
        hash = HashSmokeBytes(hash, &mesh.routeRecordIndex, sizeof(mesh.routeRecordIndex));
        hash = HashSmokeBytes(hash, &mesh.meshHash, sizeof(mesh.meshHash));
        hash = HashSmokeBytes(hash, &mesh.gpuUploadSignature, sizeof(mesh.gpuUploadSignature));
        hash = HashSmokeBytes(hash, &mesh.materialId, sizeof(mesh.materialId));
        hash = HashSmokeBytes(hash, &mesh.surfaceClassId, sizeof(mesh.surfaceClassId));
        hash = HashSmokeBytes(hash, &mesh.triangleClassAndFlags, sizeof(mesh.triangleClassAndFlags));
        hash = HashSmokeBytes(hash, &valid, sizeof(valid));
        hash = HashSmokeBytes(hash, &routeReady, sizeof(routeReady));
        hash = HashSmokeBytes(hash, &localBoundsValid, sizeof(localBoundsValid));
        hash = HashSmokeBytes(hash, &vertexCount, sizeof(vertexCount));
        hash = HashSmokeBytes(hash, &indexCount, sizeof(indexCount));
        if (mesh.localBoundsValid)
        {
            const idVec3& boundsMins = mesh.localBounds[0];
            const idVec3& boundsMaxs = mesh.localBounds[1];
            const float boundsValues[6] = {
                boundsMins.x,
                boundsMins.y,
                boundsMins.z,
                boundsMaxs.x,
                boundsMaxs.y,
                boundsMaxs.z
            };
            hash = HashSmokeBytes(hash, boundsValues, sizeof(boundsValues));
        }
    }
    return hash;
}

void CopySmokeRigidRouteTransformRows(float dst[12], const float objectToWorld[16])
{
    dst[0] = objectToWorld[0];
    dst[1] = objectToWorld[4];
    dst[2] = objectToWorld[8];
    dst[3] = objectToWorld[12];
    dst[4] = objectToWorld[1];
    dst[5] = objectToWorld[5];
    dst[6] = objectToWorld[9];
    dst[7] = objectToWorld[13];
    dst[8] = objectToWorld[2];
    dst[9] = objectToWorld[6];
    dst[10] = objectToWorld[10];
    dst[11] = objectToWorld[14];
}

bool RefreshSmokeRigidRouteBuildInstanceTransforms(
    RtPathTraceRigidRouteBuild& build,
    const RtSmokeRigidTlasPlan& plan)
{
    build.stats.previousTransformInstances = 0;
    build.stats.transformContinuousInstances = 0;
    build.stats.emittedSeenThisFrame = 0;
    build.stats.emittedFromCache = 0;
    bool instanceRecordsChanged = false;

    const int instanceCount = Min(
        static_cast<int>(build.instances.size()),
        static_cast<int>(plan.instances.size()));
    for (int instanceIndex = 0; instanceIndex < instanceCount; ++instanceIndex)
    {
        PathTraceRigidRouteInstance& routeInstance = build.instances[instanceIndex];
        const PathTraceRigidRouteInstance previousRouteInstance = routeInstance;
        const RtSmokePlanTlasInstance& plannedInstance = plan.instances[instanceIndex];
        routeInstance.instanceIdLo = static_cast<uint32_t>(plannedInstance.sourceInstanceId & 0xffffffffull);
        routeInstance.instanceIdHi = static_cast<uint32_t>((plannedInstance.sourceInstanceId >> 32) & 0xffffffffull);
        routeInstance.flags &=
            ~(PT_RIGID_ROUTE_HAS_PREVIOUS_TRANSFORM |
                PT_RIGID_ROUTE_TRANSFORM_CONTINUOUS |
                PT_RIGID_ROUTE_CACHED_SOURCE);
        if (plannedInstance.hasPreviousTransform)
        {
            routeInstance.flags |= PT_RIGID_ROUTE_HAS_PREVIOUS_TRANSFORM;
        }
        if (plannedInstance.transformContinuous)
        {
            routeInstance.flags |= PT_RIGID_ROUTE_TRANSFORM_CONTINUOUS;
        }
        if (!plannedInstance.sourceSeenThisFrame)
        {
            routeInstance.flags |= PT_RIGID_ROUTE_CACHED_SOURCE;
        }
        CopySmokeRigidRouteTransformRows(routeInstance.currentObjectToWorld, plannedInstance.transform);
        CopySmokeRigidRouteTransformRows(routeInstance.previousObjectToWorld, plannedInstance.hasPreviousTransform ? plannedInstance.previousTransform : plannedInstance.transform);
        if (instanceIndex < static_cast<int>(build.instanceSeenThisFrame.size()))
        {
            build.instanceSeenThisFrame[instanceIndex] = plannedInstance.sourceSeenThisFrame ? 1u : 0u;
        }
        if (instanceIndex < static_cast<int>(build.instanceObjectToWorld.size()))
        {
            for (int elementIndex = 0; elementIndex < 16; ++elementIndex)
            {
                build.instanceObjectToWorld[instanceIndex][elementIndex] = plannedInstance.transform[elementIndex];
            }
        }

        if (std::memcmp(&previousRouteInstance, &routeInstance, sizeof(routeInstance)) != 0)
        {
            instanceRecordsChanged = true;
        }
        if (routeInstance.triangleCount == 0)
        {
            continue;
        }
        if (plannedInstance.hasPreviousTransform)
        {
            ++build.stats.previousTransformInstances;
        }
        if (plannedInstance.transformContinuous)
        {
            ++build.stats.transformContinuousInstances;
        }
        if (plannedInstance.sourceSeenThisFrame)
        {
            ++build.stats.emittedSeenThisFrame;
        }
        else
        {
            ++build.stats.emittedFromCache;
        }
    }

    return instanceRecordsChanged;
}

const RtSmokeRigidTlasObservation* FindSmokeRigidTlasObservationForPlanInstance(
    const RtSmokeRigidTlasPlanSnapshot& snapshot,
    const RtSmokePlanTlasInstance& instance)
{
    for (const RtSmokeRigidTlasObservation& observation : snapshot.observations)
    {
        if (observation.meshHash == instance.meshHash &&
            observation.instanceId == instance.sourceInstanceId &&
            observation.routeRecordIndex == instance.routeRecordIndex)
        {
            return &observation;
        }
    }
    return nullptr;
}

void RefreshSmokeRigidTlasPlanTransforms(
    RtSmokeRigidTlasPlan& plan,
    const RtSmokeRigidTlasPlanSnapshot& snapshot)
{
    for (RtSmokePlanTlasInstance& instance : plan.instances)
    {
        const RtSmokeRigidTlasObservation* observation =
            FindSmokeRigidTlasObservationForPlanInstance(snapshot, instance);
        if (!observation)
        {
            continue;
        }

        instance.sourceSeenThisFrame = observation->seenThisFrame;
        instance.hasPreviousTransform = observation->hasPreviousObjectToWorld;
        instance.transformContinuous =
            observation->hasPreviousObjectToWorld && observation->transformContinuous;
        std::memcpy(instance.transform, observation->objectToWorld, sizeof(instance.transform));
        if (observation->hasPreviousObjectToWorld)
        {
            std::memcpy(instance.previousTransform, observation->previousObjectToWorld, sizeof(instance.previousTransform));
        }
        else
        {
            std::memcpy(instance.previousTransform, observation->objectToWorld, sizeof(instance.previousTransform));
        }
    }

    UpdateSmokeRigidTlasPlanInstanceSignature(plan, snapshot);
}


bool SmokeRigidRouteSideBufferSlotHasGeometryCapacity(
    const RtSmokeRigidRouteSideBufferSlot& slot,
    const RtPathTraceRigidRouteBuild& build)
{
    return
        SmokeBufferHasPayloadCapacity(slot.vertexBuffer, build.vertices.size() * sizeof(PathTraceSmokeVertex), sizeof(PathTraceSmokeVertex)) &&
        SmokeBufferHasPayloadCapacity(slot.indexBuffer, build.indexes.size() * sizeof(uint32_t), sizeof(uint32_t)) &&
        SmokeBufferHasPayloadCapacity(slot.triangleMaterialBuffer, build.triangleMaterials.size() * sizeof(uint32_t), sizeof(uint32_t)) &&
        SmokeBufferHasPayloadCapacity(slot.triangleMaterialIndexBuffer, build.triangleMaterialIndexes.size() * sizeof(uint32_t), sizeof(uint32_t));
}

bool SmokeRigidRouteSideBufferSlotHasInstanceCapacity(
    const RtSmokeRigidRouteSideBufferSlot& slot,
    const RtPathTraceRigidRouteBuild& build)
{
    return SmokeBufferHasPayloadCapacity(
        slot.instanceBuffer,
        build.instances.size() * sizeof(PathTraceRigidRouteInstance),
        sizeof(PathTraceRigidRouteInstance));
}

bool SmokeRigidRouteSideBufferSlotHandlesMatch(
    const RtSmokeRigidRouteSideBufferSlot& slot,
    const RtSmokeSceneBufferHandles& buffers)
{
    return
        slot.vertexBuffer == buffers.rigidRouteVertexBuffer &&
        slot.indexBuffer == buffers.rigidRouteIndexBuffer &&
        slot.triangleMaterialBuffer == buffers.rigidRouteTriangleMaterialBuffer &&
        slot.triangleMaterialIndexBuffer == buffers.rigidRouteTriangleMaterialIndexBuffer &&
        slot.instanceBuffer == buffers.rigidRouteInstanceBuffer;
}

bool SmokeRigidRouteSideBufferSlotCanSkipGeometryUpload(
    const RtSmokeRigidRouteSideBufferSlot& slot,
    const RtPathTraceRigidRouteBuild& build,
    uint64_t geometryUploadSignature)
{
    return
        slot.geometryUploadSignatureValid &&
        slot.geometryUploadSignature == geometryUploadSignature &&
        SmokeRigidRouteSideBufferSlotHasGeometryCapacity(slot, build);
}

bool SmokeRigidRouteSideBufferSlotCanSkipInstanceUpload(
    const RtSmokeRigidRouteSideBufferSlot& slot,
    const RtPathTraceRigidRouteBuild& build,
    uint64_t instanceUploadSignature)
{
    return
        slot.instanceUploadSignatureValid &&
        slot.instanceUploadSignature == instanceUploadSignature &&
        SmokeRigidRouteSideBufferSlotHasInstanceCapacity(slot, build);
}

template< typename T >
RtSmokeBufferUploadItem MakeSmokeVectorUploadItem(
    nvrhi::BufferHandle buffer,
    const std::vector<T>& data,
    nvrhi::ResourceStates finalState,
    bool skip,
    int elementOffset = -1,
    int elementCount = 0)
{
    RtSmokeBufferUploadItem item;
    item.buffer = buffer;
    item.data = data.data();
    item.finalState = finalState;
    const RtSmokeUploadPlanMetadata uploadPlan = BuildSmokeVectorUploadPlanMetadata(
        data.size(),
        sizeof(T),
        skip,
        elementOffset,
        elementCount);
    item.byteSize = uploadPlan.byteSize;
    item.skip = uploadPlan.skip;
    item.sourceOffsetBytes = uploadPlan.sourceOffsetBytes;
    item.destOffsetBytes = uploadPlan.destOffsetBytes;
    return item;
}

RtSmokeBufferUploadItem MakeSmokeBufferStateItem(nvrhi::BufferHandle buffer, nvrhi::ResourceStates finalState)
{
    RtSmokeBufferUploadItem item;
    item.buffer = buffer;
    item.finalState = finalState;
    return item;
}

uint64_t SumSmokeUploadBytes(const RtSmokeBufferUploadItem* items, int firstItem, int itemCount)
{
    uint64_t bytes = 0;
    for (int itemIndex = firstItem; itemIndex < firstItem + itemCount; ++itemIndex)
    {
        const RtSmokeBufferUploadItem& item = items[itemIndex];
        if (!item.skip && item.buffer && item.data && item.byteSize > 0)
        {
            bytes += static_cast<uint64_t>(item.byteSize);
        }
    }
    return bytes;
}

uint64_t SumSmokeSkippedUploadBytes(const RtSmokeBufferUploadItem* items, int firstItem, int itemCount)
{
    uint64_t bytes = 0;
    for (int itemIndex = firstItem; itemIndex < firstItem + itemCount; ++itemIndex)
    {
        const RtSmokeBufferUploadItem& item = items[itemIndex];
        if (item.skip && item.buffer && item.data && item.byteSize > 0)
        {
            bytes += static_cast<uint64_t>(item.byteSize);
        }
    }
    return bytes;
}

uint64 ComputeSmokeReservoirStructuralSignature(
    uint64 materialTableSignature,
    uint64 staticBlasSignature,
    uint64 restirLightManagerStructuralSignature)
{
    uint64 hash = 1469598103934665603ull;
    const uint64 version = 1;
    hash = HashSmokeBytes(hash, &version, sizeof(version));
    hash = HashSmokeBytes(hash, &materialTableSignature, sizeof(materialTableSignature));
    hash = HashSmokeBytes(hash, &staticBlasSignature, sizeof(staticBlasSignature));
    hash = HashSmokeBytes(hash, &restirLightManagerStructuralSignature, sizeof(restirLightManagerStructuralSignature));
    return hash;
}

struct RtSmokeStaticDrawSurfCounts
{
    int surfaces = 0;
    int triangles = 0;
};

struct RtSmokeSkinnedGpuScaffoldBuild
{
    std::vector<PathTraceSkinnedSourceVertex> sourceVertices;
    std::vector<PathTraceSmokeVertex> currentOutputVertices;
    std::vector<PathTraceSkinnedPreviousPosition> previousPositions;
    std::vector<PathTraceSkinnedSurfaceDispatchRecord> dispatchRecords;
    std::vector<uint32_t> dynamicTriangleDispatchIndexes;
    int mappedDynamicTriangles = 0;
    std::vector<PathTraceSkinnedJointMatrix> currentJointMatrices;
    std::vector<PathTraceSkinnedJointMatrix> previousJointMatrices;
    enum class Result
    {
        EligibleGpu,
        DispatchedGpu,
        NotReadyJointData,
        UnsupportedLayout,
        InvalidWeightsOrJoints,
        InvalidCanonicalSource,
        InvalidPersistentOutput,
        AllocationFailure,
        ExplicitSingleBoneRoute,
        CpuFallback,
        Count
    };
    std::vector<Result> recordResults;
    int singleBoneObserved = 0;
    bool canonicalSourceOutputRoute = false;
    int canonicalSourceResolved = 0;
    int canonicalSourcePacked = 0;
    int canonicalSourceReused = 0;
    int canonicalOutputExact = 0;
    int canonicalDispatches = 0;
    uint64 canonicalSourceVertices = 0;
    uint64 canonicalOutputCapacityVertices = 0;
};

struct RtSmokeJointCacheStageCopy
{
    nvrhi::BufferHandle sourceBuffer;
    uint64 sourceOffsetBytes = 0;
    uint64 destinationOffsetBytes = 0;
    uint64 byteCount = 0;
    size_t dispatchIndex = 0;
};

struct RtSmokeJointCacheStageBuild
{
    bool requested = false;
    bool ready = false;
    bool submitted = false;
    PtJointCacheCopyPlanner planner;
    std::vector<PtJointCacheCopyPlanResult> dispatchResults;
    std::vector<PtJointCacheCopyPlan> dispatchPlans;
    std::vector<RtSmokeJointCacheStageCopy> copies;
    int accepted = 0;
    int rejected = 0;
    int resolved = 0;
    int stale = 0;
    uint64 submittedBytes = 0;
};

struct RtSmokeSkinnedHistoryAudit
{
    PtCanonicalHistoryOwnerKey owner;
    PtSkinnedHistoryRoute route =
        PtSkinnedHistoryRoute::SubviewOrInvalidNoHistory;
    bool ownerGate = false;
    bool primaryView = false;
    bool ownerValid = false;
    bool previousStateFound = false;
    bool readAllowed = false;
    bool writeAllowed = false;
    int currentRecords = 0;
    int previousRecords = 0;
    int ownerMatches = 0;
    int ownerMismatches = 0;
    uint64 previousUpdateSerial = 0;
    uint64 nextUpdateSerial = 0;
};

struct RtSmokeSkinnedOutputAudit
{
    bool masterGate = false;
    bool sourceRegistryGate = false;
    bool worldValid = false;
    int candidates = 0;
    int sourceEligible = 0;
    int added = 0;
    int reused = 0;
    int resized = 0;
    int rejected = 0;
    int retireApplied = 0;
    int retireMissing = 0;
    int rangesFound = 0;
    int exactRanges = 0;
    int overlapPairs = 0;
    int sharedSourcePairs = 0;
    int sharedSourceNonAliasedPairs = 0;
    uint64 requestedVertices = 0;
    std::vector<PtCanonicalMeshKey> uniqueSources;
    PtSkinnedOutputAllocatorStats stats;
};

struct RtSmokeSkinnedBlasShadowAudit
{
    bool gate = false;
    bool worldValid = false;
    bool physicalBufferExact = false;
    bool physicalBufferChanged = false;
    bool physicalBufferUav = false;
    bool physicalBufferAsInput = false;
    uint64 allocatorStorageGeneration = 0;
    uint64 physicalStorageGeneration = 0;
    uint64 expectedOutputBytes = 0;
    uint64 physicalOutputBytes = 0;
    int candidates = 0;
    int dispatched = 0;
    int sourceGpuExact = 0;
    int outputExact = 0;
    int observed = 0;
    int buildRequired = 0;
    int updateRequired = 0;
    int rebuildRequired = 0;
    int alreadyPending = 0;
    int rejected = 0;
    int retireApplied = 0;
    int retireMissing = 0;
    PtSkinnedBlasStats stats;
};

struct RtSmokeSkinnedComparisonBlasAudit
{
    bool gate = false;
    int candidates = 0;
    int buildPending = 0;
    int updatePending = 0;
    int rebuildPending = 0;
    int exactContracts = 0;
    int created = 0;
    int reused = 0;
    int replaced = 0;
    int buildSubmitted = 0;
    int updateSubmitted = 0;
    int rebuildSubmitted = 0;
    int replacementDeferred = 0;
    int failed = 0;
    int activeResources = 0;
};

struct RtSmokeSkinnedComparisonRetirementAudit
{
    int activeRetired = 0;
    int statePackagesQueued = 0;
    int emptyStatePackagesQueued = 0;
    int pendingPackages = 0;
};

RtSmokeSkinnedHistoryState* FindSmokeSkinnedHistoryState(
    std::vector<RtSmokeSkinnedHistoryState>& states,
    const PtCanonicalHistoryOwnerKey& owner)
{
    for (RtSmokeSkinnedHistoryState& state : states)
    {
        if (state.owner == owner)
        {
            return &state;
        }
    }
    return nullptr;
}

const char* SmokeSkinnedGpuResultName(RtSmokeSkinnedGpuScaffoldBuild::Result result)
{
    using Result = RtSmokeSkinnedGpuScaffoldBuild::Result;
    switch (result)
    {
        case Result::EligibleGpu: return "eligibleGpu";
        case Result::DispatchedGpu: return "dispatchedGpu";
        case Result::NotReadyJointData: return "notReadyJointData";
        case Result::UnsupportedLayout: return "unsupportedLayout";
        case Result::InvalidWeightsOrJoints: return "invalidWeightsOrJoints";
        case Result::InvalidCanonicalSource: return "invalidCanonicalSource";
        case Result::InvalidPersistentOutput: return "invalidPersistentOutput";
        case Result::AllocationFailure: return "allocationFailure";
        case Result::ExplicitSingleBoneRoute: return "explicitSingleBoneRoute";
        case Result::CpuFallback: return "cpuFallback";
        default: return "unknown";
    }
}

bool SmokeSkinnedSourceLayoutAndWeightsValid(
    const srfTriangles_t* tri,
    int vertexCount,
    int jointCount)
{
    if (!tri || !tri->verts || vertexCount <= 0 || vertexCount > tri->numVerts || jointCount <= 0)
    {
        return false;
    }
    for (int vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
    {
        const idDrawVert& vertex = tri->verts[vertexIndex];
        if (!SmokeVec3IsFinite(vertex.xyz))
        {
            return false;
        }
        int weightSum = 0;
        for (int component = 0; component < 4; ++component)
        {
            if (static_cast<int>(vertex.color[component]) >= jointCount)
            {
                return false;
            }
            weightSum += static_cast<int>(vertex.color2[component]);
        }
        if (weightSum < 254 || weightSum > 256)
        {
            return false;
        }
    }
    return true;
}

void FinalizeSmokeSkinnedGpuFunnel(
    RtSmokeSkinnedGpuScaffoldBuild& build,
    bool computeDispatched)
{
    using Result = RtSmokeSkinnedGpuScaffoldBuild::Result;
    for (Result& result : build.recordResults)
    {
        if (result == Result::EligibleGpu)
        {
            result = computeDispatched ? Result::DispatchedGpu : Result::AllocationFailure;
        }
    }
}

bool SmokeSkinnedOutputRangesOverlap(
    const PtSkinnedOutputRange& lhs,
    const PtSkinnedOutputRange& rhs)
{
    const uint64 lhsEnd =
        lhs.vertexOffset + lhs.vertexCount;
    const uint64 rhsEnd =
        rhs.vertexOffset + rhs.vertexCount;
    return lhs.vertexOffset < rhsEnd &&
        rhs.vertexOffset < lhsEnd;
}

RtSmokeSkinnedOutputAudit UpdateSmokeSkinnedOutputAllocator(
    const viewDef_t* viewDef,
    const std::vector<RtSmokeSkinnedSurfaceRecord>& records,
    const RtSmokeGeometryUniverse& geometryUniverse,
    PtSkinnedOutputAllocator& allocator,
    bool masterGate,
    bool sourceRegistryGate,
    uint64 frameIndex)
{
    RtSmokeSkinnedOutputAudit audit;
    audit.masterGate = masterGate;
    audit.sourceRegistryGate = sourceRegistryGate;
    audit.candidates = static_cast<int>(records.size());
    if (!masterGate || !sourceRegistryGate || viewDef == nullptr)
    {
        audit.stats = allocator.Stats();
        return audit;
    }

    const PtCanonicalWorldKey world =
        PtGeometryLifecycle::CanonicalWorldKey(
            viewDef->renderWorld);
    audit.worldValid =
        PtCanonicalWorldKeyIsValid(world) &&
        allocator.BeginWorld(
            world.worldGeneration,
            frameIndex);
    if (!audit.worldValid)
    {
        audit.stats = allocator.Stats();
        return audit;
    }

    const PtGeometryIdentityTransportSnapshot* identitySnapshot =
        viewDef->pathTraceGeometryIdentitySnapshot;
    if (identitySnapshot != nullptr)
    {
        for (uint64 index = 0;
            index < identitySnapshot->recordCount;
            ++index)
        {
            const PtGeometryIdentityTransportRecord& transport =
                identitySnapshot->records[index];
            if (transport.operation !=
                    PtGeometryIdentityOperation::Remove ||
                transport.instanceKey.subInstanceKind !=
                    PtCanonicalSubInstanceKind::SkinnedSurface)
            {
                continue;
            }
            const PtSkinnedOutputRetireResult result =
                allocator.Retire(
                    transport.instanceKey,
                    frameIndex);
            if (result ==
                PtSkinnedOutputRetireResult::Retired)
            {
                ++audit.retireApplied;
            }
            else
            {
                ++audit.retireMissing;
            }
        }
    }

    struct Candidate
    {
        PtCanonicalInstanceKey instance;
        PtCanonicalMeshKey mesh;
        PtSkinnedOutputRange range;
        bool hasRange = false;
        int vertexCount = 0;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(records.size());
    for (const RtSmokeSkinnedSurfaceRecord& record : records)
    {
        const PtGeometryIdentityBinding* binding =
            geometryUniverse.FindCanonicalIdentityBinding(
                record.canonicalInstance);
        if (binding == nullptr)
        {
            ++audit.rejected;
            continue;
        }
        const PtGeometrySourceRecord* source =
            geometryUniverse.FindCanonicalSourceRecord(
                binding->meshKey);
        if (source == nullptr ||
            source->key != binding->meshKey ||
            source->key.sourceDomain !=
                PtCanonicalMeshSourceDomain::SkinnedBindSource ||
            source->key.deformationClass !=
                PtCanonicalDeformationClass::Skinned ||
            source->key.vertexCount !=
                static_cast<uint32_t>(record.vertexCount) ||
            source->sourceChecksum == 0)
        {
            ++audit.rejected;
            continue;
        }

        ++audit.sourceEligible;
        const uint64 requestedVertices =
            static_cast<uint64>(record.vertexCount);
        if (audit.requestedVertices >
            std::numeric_limits<uint64>::max() -
                requestedVertices)
        {
            ++audit.rejected;
            continue;
        }
        audit.requestedVertices += requestedVertices;
        if (std::find(
                audit.uniqueSources.begin(),
                audit.uniqueSources.end(),
                source->key) ==
            audit.uniqueSources.end())
        {
            audit.uniqueSources.push_back(source->key);
        }
        const PtSkinnedOutputObserveResult result =
            allocator.Observe(
                record.canonicalInstance,
                static_cast<uint64>(record.vertexCount),
                frameIndex);
        switch (result)
        {
            case PtSkinnedOutputObserveResult::Added:
                ++audit.added;
                break;
            case PtSkinnedOutputObserveResult::Reused:
                ++audit.reused;
                break;
            case PtSkinnedOutputObserveResult::Resized:
                ++audit.resized;
                break;
            default:
                ++audit.rejected;
                break;
        }

        Candidate candidate;
        candidate.instance = record.canonicalInstance;
        candidate.mesh = binding->meshKey;
        candidate.vertexCount = record.vertexCount;
        candidates.push_back(candidate);
    }

    for (Candidate& candidate : candidates)
    {
        const PtSkinnedOutputRange* range =
            allocator.Find(candidate.instance);
        if (range != nullptr)
        {
            candidate.range = *range;
            candidate.hasRange = true;
            ++audit.rangesFound;
            if (candidate.range.vertexCount ==
                    static_cast<uint64>(
                        candidate.vertexCount) &&
                candidate.range.storageGeneration ==
                    allocator.Stats().storageGeneration)
            {
                ++audit.exactRanges;
            }
        }
    }

    for (size_t first = 0; first < candidates.size(); ++first)
    {
        for (size_t second = first + 1;
            second < candidates.size();
            ++second)
        {
            if (candidates[first].hasRange &&
                candidates[second].hasRange &&
                candidates[first].instance !=
                    candidates[second].instance &&
                SmokeSkinnedOutputRangesOverlap(
                    candidates[first].range,
                    candidates[second].range))
            {
                ++audit.overlapPairs;
            }
            if (candidates[first].mesh ==
                    candidates[second].mesh &&
                candidates[first].instance !=
                    candidates[second].instance)
            {
                ++audit.sharedSourcePairs;
                if (candidates[first].hasRange &&
                    candidates[second].hasRange &&
                    !SmokeSkinnedOutputRangesOverlap(
                        candidates[first].range,
                        candidates[second].range))
                {
                    ++audit.sharedSourceNonAliasedPairs;
                }
            }
        }
    }
    audit.stats = allocator.Stats();
    return audit;
}

RtSmokeSkinnedBlasShadowAudit
UpdateSmokeSkinnedBlasShadowState(
    const viewDef_t* viewDef,
    const std::vector<RtSmokeSkinnedSurfaceRecord>& records,
    const RtSmokeSkinnedGpuScaffoldBuild& gpuBuild,
    const RtSmokeGeometryUniverse& geometryUniverse,
    const PtSkinnedOutputAllocator& outputAllocator,
    PtSkinnedBlasStateTable& stateTable,
    nvrhi::BufferHandle outputBuffer,
    uint64 outputBufferGeneration,
    bool outputBufferChanged,
    bool gate,
    uint64 frameIndex)
{
    RtSmokeSkinnedBlasShadowAudit audit;
    audit.gate = gate;
    audit.physicalBufferChanged = outputBufferChanged;
    audit.candidates = static_cast<int>(records.size());
    audit.allocatorStorageGeneration =
        outputAllocator.Stats().storageGeneration;
    audit.physicalStorageGeneration =
        outputBufferGeneration;
    if (!gate)
    {
        stateTable.Clear();
        audit.stats = stateTable.Stats();
        return audit;
    }
    if (viewDef == nullptr)
    {
        audit.stats = stateTable.Stats();
        return audit;
    }

    stateTable.ResetIntervalStats();
    const PtCanonicalWorldKey world =
        PtGeometryLifecycle::CanonicalWorldKey(
            viewDef->renderWorld);
    audit.worldValid =
        PtCanonicalWorldKeyIsValid(world) &&
        stateTable.BeginWorld(
            world.worldGeneration,
            frameIndex);
    if (!audit.worldValid)
    {
        audit.stats = stateTable.Stats();
        return audit;
    }

    const PtGeometryIdentityTransportSnapshot* identitySnapshot =
        viewDef->pathTraceGeometryIdentitySnapshot;
    if (identitySnapshot != nullptr)
    {
        for (uint64 recordIndex = 0;
            recordIndex < identitySnapshot->recordCount;
            ++recordIndex)
        {
            const PtGeometryIdentityTransportRecord& transport =
                identitySnapshot->records[recordIndex];
            if (transport.operation !=
                    PtGeometryIdentityOperation::Remove ||
                transport.instanceKey.subInstanceKind !=
                    PtCanonicalSubInstanceKind::SkinnedSurface)
            {
                continue;
            }
            const PtSkinnedBlasRetireResult result =
                stateTable.Retire(
                    transport.instanceKey,
                    frameIndex);
            if (result == PtSkinnedBlasRetireResult::Retired)
            {
                ++audit.retireApplied;
            }
            else
            {
                ++audit.retireMissing;
            }
        }
    }

    uint64 expectedOutputBytes = 0;
    const bool expectedBytesValid =
        PtCheckedMulU64(
            outputAllocator.Stats().capacityVertices,
            sizeof(PathTraceSmokeVertex),
            expectedOutputBytes);
    audit.expectedOutputBytes = expectedOutputBytes;
    if (outputBuffer)
    {
        const nvrhi::BufferDesc& desc = outputBuffer->getDesc();
        audit.physicalOutputBytes = desc.byteSize;
        audit.physicalBufferUav = desc.canHaveUAVs;
        audit.physicalBufferAsInput =
            desc.isAccelStructBuildInput;
        audit.physicalBufferExact =
            expectedBytesValid &&
            outputAllocator.Stats().storageGeneration != 0 &&
            outputBufferGeneration ==
                outputAllocator.Stats().storageGeneration &&
            desc.byteSize >= expectedOutputBytes &&
            desc.structStride == sizeof(PathTraceSmokeVertex) &&
            desc.canHaveUAVs &&
            desc.isAccelStructBuildInput;
    }
    if (!audit.physicalBufferExact)
    {
        audit.rejected = audit.candidates;
        audit.stats = stateTable.Stats();
        return audit;
    }

    const uint64 sourceIndexGeneration =
        geometryUniverse.
            CanonicalSourceIndexPoolGeneration();
    const uint64 sourceIndexCapacity =
        geometryUniverse.
            CanonicalSourceIndexPoolCapacityBytes();
    const size_t recordCount = std::min(
        records.size(),
        gpuBuild.recordResults.size());
    for (size_t recordIndex = 0;
        recordIndex < recordCount;
        ++recordIndex)
    {
        if (gpuBuild.recordResults[recordIndex] !=
            RtSmokeSkinnedGpuScaffoldBuild::Result::DispatchedGpu)
        {
            ++audit.rejected;
            continue;
        }
        ++audit.dispatched;
        const RtSmokeSkinnedSurfaceRecord& record =
            records[recordIndex];
        const PtGeometryIdentityBinding* binding =
            geometryUniverse.FindCanonicalIdentityBinding(
                record.canonicalInstance);
        const PtGeometrySourceRecord* source =
            binding != nullptr
                ? geometryUniverse.FindCanonicalSourceRecord(
                    binding->meshKey)
                : nullptr;
        const PtGeometryGpuPoolRecord* sourceGpu =
            binding != nullptr
                ? geometryUniverse.FindCanonicalSourceGpuRecord(
                    binding->meshKey)
                : nullptr;
        uint64 expectedSourceIndexBytes = 0;
        uint64 sourceIndexEndBytes = 0;
        const bool sourceIndexRangeExact =
            source != nullptr &&
            sourceGpu != nullptr &&
            PtCheckedMulU64(
                static_cast<uint64>(
                    source->key.indexCount),
                sizeof(uint32_t),
                expectedSourceIndexBytes) &&
            PtCheckedAddU64(
                sourceGpu->indexes.offsetBytes,
                expectedSourceIndexBytes,
                sourceIndexEndBytes) &&
            sourceIndexEndBytes <= sourceIndexCapacity;
        if (binding == nullptr ||
            source == nullptr ||
            sourceGpu == nullptr ||
            source->key != binding->meshKey ||
            sourceGpu->key != binding->meshKey ||
            sourceGpu->sourceChecksum !=
                source->sourceChecksum ||
            sourceGpu->indexes.storageGeneration !=
                sourceIndexGeneration ||
            !sourceIndexRangeExact ||
            sourceGpu->indexes.sizeBytes !=
                expectedSourceIndexBytes)
        {
            ++audit.rejected;
            continue;
        }
        ++audit.sourceGpuExact;

        const PtSkinnedOutputRange* outputRange =
            outputAllocator.Find(record.canonicalInstance);
        uint64 expectedOutputRangeBytes = 0;
        uint64 outputEndBytes = 0;
        const bool outputRangeExact =
            outputRange != nullptr &&
            PtCheckedMulU64(
                outputRange->vertexCount,
                sizeof(PathTraceSmokeVertex),
                expectedOutputRangeBytes) &&
            outputRange->byteCount ==
                expectedOutputRangeBytes &&
            PtCheckedAddU64(
                outputRange->byteOffset,
                outputRange->byteCount,
                outputEndBytes) &&
            outputEndBytes <= audit.physicalOutputBytes;
        if (outputRange == nullptr ||
            outputRange->storageGeneration !=
                outputBufferGeneration ||
            outputRange->vertexCount !=
                static_cast<uint64>(record.vertexCount) ||
            !outputRangeExact)
        {
            ++audit.rejected;
            continue;
        }
        ++audit.outputExact;

        PtSkinnedBlasCandidate candidate;
        candidate.instanceKey = record.canonicalInstance;
        candidate.meshKey = binding->meshKey;
        candidate.sourceChecksum = source->sourceChecksum;
        candidate.sourceGpuIndexGeneration =
            sourceIndexGeneration;
        candidate.sourceIndexOffsetBytes =
            sourceGpu->indexes.offsetBytes;
        candidate.sourceIndexCapacityBytes =
            sourceIndexCapacity;
        candidate.outputStorageGeneration =
            outputRange->storageGeneration;
        candidate.outputVertexOffsetBytes =
            outputRange->byteOffset;
        candidate.outputVertexCount =
            outputRange->vertexCount;
        candidate.outputCapacityBytes =
            audit.physicalOutputBytes;
        candidate.frameIndex = frameIndex;
        candidate.dispatchReady = true;
        const PtSkinnedBlasObserveResult result =
            stateTable.Observe(candidate);
        ++audit.observed;
        switch (result)
        {
            case PtSkinnedBlasObserveResult::BuildRequired:
                ++audit.buildRequired;
                break;
            case PtSkinnedBlasObserveResult::UpdateRequired:
                ++audit.updateRequired;
                break;
            case PtSkinnedBlasObserveResult::RebuildRequired:
                ++audit.rebuildRequired;
                break;
            case PtSkinnedBlasObserveResult::AlreadyPending:
                ++audit.alreadyPending;
                break;
            default:
                ++audit.rejected;
                break;
        }
    }
    if (recordCount < records.size())
    {
        audit.rejected += static_cast<int>(
            records.size() - recordCount);
    }
    audit.stats = stateTable.Stats();
    return audit;
}

void DumpSmokeSkinnedBlasShadowAudit(
    const RtSmokeSkinnedBlasShadowAudit& audit,
    uint64 frameIndex)
{
    common->Printf(
        "PathTracePrimaryPass: GEO08 skinned BLAS shadow frame=%llu gate/world/physicalExact/changed/uav/asInput=%d/%d/%d/%d/%d/%d storage(allocator/physical)=%llu/%llu outputBytes(expected/physical)=%llu/%llu candidates/dispatched/sourceGpuExact/outputExact/observed=%d/%d/%d/%d/%d plan(build/update/rebuild/pending/reject)=%d/%d/%d/%d/%d states(active/build/ready/update/rebuild/failed/retiring)=%llu/%llu/%llu/%llu/%llu/%llu/%llu retire(applied/missing/released)=%d/%d/%llu route=shadow-no-as\n",
        static_cast<unsigned long long>(frameIndex),
        audit.gate ? 1 : 0,
        audit.worldValid ? 1 : 0,
        audit.physicalBufferExact ? 1 : 0,
        audit.physicalBufferChanged ? 1 : 0,
        audit.physicalBufferUav ? 1 : 0,
        audit.physicalBufferAsInput ? 1 : 0,
        static_cast<unsigned long long>(
            audit.allocatorStorageGeneration),
        static_cast<unsigned long long>(
            audit.physicalStorageGeneration),
        static_cast<unsigned long long>(
            audit.expectedOutputBytes),
        static_cast<unsigned long long>(
            audit.physicalOutputBytes),
        audit.candidates,
        audit.dispatched,
        audit.sourceGpuExact,
        audit.outputExact,
        audit.observed,
        audit.buildRequired,
        audit.updateRequired,
        audit.rebuildRequired,
        audit.alreadyPending,
        audit.rejected,
        static_cast<unsigned long long>(
            audit.stats.activeRecords),
        static_cast<unsigned long long>(
            audit.stats.buildPending),
        static_cast<unsigned long long>(
            audit.stats.ready),
        static_cast<unsigned long long>(
            audit.stats.updatePending),
        static_cast<unsigned long long>(
            audit.stats.rebuildPending),
        static_cast<unsigned long long>(
            audit.stats.failed),
        static_cast<unsigned long long>(
            audit.stats.pendingRetirements),
        audit.retireApplied,
        audit.retireMissing,
        static_cast<unsigned long long>(
            audit.stats.retirementsReleased));
}

RtSmokeSkinnedComparisonBlasResource*
FindSmokeSkinnedComparisonBlasResource(
    std::vector<RtSmokeSkinnedComparisonBlasResource>& resources,
    const PtCanonicalInstanceKey& instanceKey)
{
    for (RtSmokeSkinnedComparisonBlasResource& resource :
        resources)
    {
        if (resource.instanceKey == instanceKey)
        {
            return &resource;
        }
    }
    return nullptr;
}

bool SmokeSkinnedComparisonBlasContractMatches(
    const RtSmokeSkinnedComparisonBlasResource& resource,
    const PtSkinnedBlasRecord& state,
    nvrhi::BufferHandle vertexBuffer,
    nvrhi::BufferHandle indexBuffer)
{
    return
        resource.instanceKey == state.instanceKey &&
        resource.meshKey == state.meshKey &&
        resource.sourceChecksum == state.sourceChecksum &&
        resource.sourceGpuIndexGeneration ==
            state.sourceGpuIndexGeneration &&
        resource.sourceIndexOffsetBytes ==
            state.sourceIndexOffsetBytes &&
        resource.sourceIndexBytes == state.sourceIndexBytes &&
        resource.outputStorageGeneration ==
            state.outputStorageGeneration &&
        resource.outputVertexOffsetBytes ==
            state.outputVertexOffsetBytes &&
        resource.outputVertexCount ==
            state.outputVertexCount &&
        resource.blasGeneration ==
            state.blasGeneration &&
        resource.vertexBuffer == vertexBuffer &&
        resource.indexBuffer == indexBuffer &&
        resource.blas;
}

bool SmokeSkinnedTlasRouteResourceContractMatches(
    const PtSkinnedHitRouteRecord& route,
    const RtSmokeSkinnedComparisonBlasResource& resource,
    const PtSkinnedBlasRecord& state,
    nvrhi::BufferHandle vertexBuffer,
    nvrhi::BufferHandle indexBuffer)
{
    return
        route.instanceKey == state.instanceKey &&
        route.meshKey == state.meshKey &&
        route.sourceChecksum == state.sourceChecksum &&
        route.sourceGpuIndexGeneration ==
            state.sourceGpuIndexGeneration &&
        route.outputStorageGeneration ==
            state.outputStorageGeneration &&
        static_cast<uint64>(route.sourceIndexOffset) *
                sizeof(uint32_t) ==
            state.sourceIndexOffsetBytes &&
        static_cast<uint64>(route.indexCount) *
                sizeof(uint32_t) ==
            state.sourceIndexBytes &&
        static_cast<uint64>(route.outputVertexOffset) *
                sizeof(PathTraceSmokeVertex) ==
            state.outputVertexOffsetBytes &&
        route.vertexCount == state.outputVertexCount &&
        resource.instanceKey == state.instanceKey &&
        resource.meshKey == state.meshKey &&
        resource.sourceChecksum == state.sourceChecksum &&
        resource.sourceGpuIndexGeneration ==
            state.sourceGpuIndexGeneration &&
        resource.sourceIndexOffsetBytes ==
            state.sourceIndexOffsetBytes &&
        resource.sourceIndexBytes ==
            state.sourceIndexBytes &&
        resource.outputStorageGeneration ==
            state.outputStorageGeneration &&
        resource.outputVertexOffsetBytes ==
            state.outputVertexOffsetBytes &&
        resource.outputVertexCount ==
            state.outputVertexCount &&
        resource.blasGeneration ==
            state.blasGeneration &&
        resource.vertexBuffer == vertexBuffer &&
        resource.indexBuffer == indexBuffer;
}

enum class SmokeSkinnedCaptureLiveResult
{
    Live = 0,
    MissingBuffer,
    MissingState,
    StateNotRoutable,
    MissingResource,
    ResourceContractMismatch,
    MissingBlas
};

const char* SmokeSkinnedCaptureLiveResultName(
    SmokeSkinnedCaptureLiveResult result)
{
    switch (result)
    {
        case SmokeSkinnedCaptureLiveResult::Live:
            return "live";
        case SmokeSkinnedCaptureLiveResult::MissingBuffer:
            return "missing-buffer";
        case SmokeSkinnedCaptureLiveResult::MissingState:
            return "missing-state";
        case SmokeSkinnedCaptureLiveResult::StateNotRoutable:
            return "state-not-routable";
        case SmokeSkinnedCaptureLiveResult::MissingResource:
            return "missing-resource";
        case SmokeSkinnedCaptureLiveResult::
            ResourceContractMismatch:
            return "resource-contract-mismatch";
        case SmokeSkinnedCaptureLiveResult::MissingBlas:
            return "missing-blas";
        default:
            return "unknown";
    }
}

SmokeSkinnedCaptureLiveResult
ValidateSmokeSkinnedCaptureAcceptedBuildLive(
    const PtSkinnedHitRouteBuild& build,
    const PtSkinnedBlasStateTable& stateTable,
    const std::vector<RtSmokeSkinnedComparisonBlasResource>&
        resources,
    nvrhi::BufferHandle vertexBuffer,
    nvrhi::BufferHandle indexBuffer)
{
    if (!vertexBuffer || !indexBuffer)
    {
        return SmokeSkinnedCaptureLiveResult::MissingBuffer;
    }
    for (const PtSkinnedHitRouteRecord& route :
        build.records)
    {
        const PtSkinnedBlasRecord* state =
            stateTable.Find(route.instanceKey);
        if (state == nullptr)
        {
            return SmokeSkinnedCaptureLiveResult::MissingState;
        }
        if (state->state != PtSkinnedBlasState::Ready &&
            state->state != PtSkinnedBlasState::UpdatePending &&
            state->state != PtSkinnedBlasState::RebuildPending)
        {
            return
                SmokeSkinnedCaptureLiveResult::StateNotRoutable;
        }
        const RtSmokeSkinnedComparisonBlasResource* resource =
            nullptr;
        for (const RtSmokeSkinnedComparisonBlasResource& candidate :
            resources)
        {
            if (candidate.instanceKey == route.instanceKey)
            {
                resource = &candidate;
                break;
            }
        }
        if (resource == nullptr)
        {
            return SmokeSkinnedCaptureLiveResult::MissingResource;
        }
        if (!SmokeSkinnedTlasRouteResourceContractMatches(
                route,
                *resource,
                *state,
                vertexBuffer,
                indexBuffer))
        {
            return SmokeSkinnedCaptureLiveResult::
                ResourceContractMismatch;
        }
        if (!resource->blas)
        {
            return SmokeSkinnedCaptureLiveResult::MissingBlas;
        }
    }
    return SmokeSkinnedCaptureLiveResult::Live;
}

bool SmokeSkinnedHitRouteBuildUsesSourceOnlyMetadata(
    const PtSkinnedHitRouteBuild& build)
{
    return
        !build.records.empty() &&
        std::all_of(
            build.records.begin(),
            build.records.end(),
            [](const PtSkinnedHitRouteRecord& route)
            {
                return (route.flags &
                    PT_SKINNED_HIT_ROUTE_HAS_SOURCE_ONLY_PRIMITIVES) !=
                    0u;
            });
}

bool SmokeSkinnedHitRouteBuildUsesAnySourceOnlyMetadata(
    const PtSkinnedHitRouteBuild& build)
{
    return std::any_of(
        build.records.begin(),
        build.records.end(),
        [](const PtSkinnedHitRouteRecord& route)
        {
            return (route.flags &
                PT_SKINNED_HIT_ROUTE_HAS_SOURCE_ONLY_PRIMITIVES) !=
                0u;
        });
}

bool SmokeSkinnedHitRouteBuildContainsInstanceSet(
    const PtSkinnedHitRouteBuild& container,
    const PtSkinnedHitRouteBuild& subset)
{
    if (container.records.size() < subset.records.size())
    {
        return false;
    }
    std::vector<bool> matched(container.records.size(), false);
    for (const PtSkinnedHitRouteRecord& subsetRoute :
        subset.records)
    {
        bool found = false;
        for (size_t containerIndex = 0;
            containerIndex < container.records.size();
            ++containerIndex)
        {
            if (!matched[containerIndex] &&
                container.records[containerIndex].instanceKey ==
                    subsetRoute.instanceKey)
            {
                matched[containerIndex] = true;
                found = true;
                break;
            }
        }
        if (!found)
        {
            return false;
        }
    }
    return true;
}

bool SmokeSkinnedHitRoutesMatchOmittedCapture(
    const std::vector<PtSkinnedHitRouteRecord>& routes,
    const std::vector<RtSmokeSkinnedSurfaceRecord>& records,
    int omittedSurfaceCount)
{
    if (routes.empty())
    {
        return omittedSurfaceCount <= 0;
    }
    const bool exactOmittedSubset =
        routes.size() ==
            static_cast<size_t>(Max(omittedSurfaceCount, 0));
    const bool completeCurrentSet =
        routes.size() == records.size();
    // A transition upload is valid in either of two forms: the exact
    // previously accepted subset whose CPU copies were omitted, or the
    // complete current set used to pre-admit newly ready routes at mask zero.
    // Unrelated or partial-in-between InstanceKey sets remain suppressed.
    if (!exactOmittedSubset && !completeCurrentSet)
    {
        return false;
    }
    std::vector<bool> matched(records.size(), false);
    int matchedOmittedSurfaces = 0;
    for (const PtSkinnedHitRouteRecord& route : routes)
    {
        if ((route.flags &
                PT_SKINNED_HIT_ROUTE_HAS_SOURCE_ONLY_PRIMITIVES) ==
            0u)
        {
            return false;
        }
        bool found = false;
        for (size_t recordIndex = 0;
            recordIndex < records.size();
            ++recordIndex)
        {
            const RtSmokeSkinnedSurfaceRecord& record =
                records[recordIndex];
            if (!matched[recordIndex] &&
                record.canonicalInstance == route.instanceKey)
            {
                matched[recordIndex] = true;
                if (record.cpuCaptureOmitted)
                {
                    ++matchedOmittedSurfaces;
                }
                found = true;
                break;
            }
        }
        if (!found)
        {
            return false;
        }
    }
    return
        matchedOmittedSurfaces == omittedSurfaceCount &&
        (exactOmittedSubset || completeCurrentSet);
}

bool SmokeSkinnedCaptureInstanceWasOmitted(
    const std::vector<RtSmokeSkinnedSurfaceRecord>& records,
    const PtCanonicalInstanceKey& instanceKey)
{
    for (const RtSmokeSkinnedSurfaceRecord& record : records)
    {
        if (record.canonicalInstance == instanceKey)
        {
            return record.cpuCaptureOmitted;
        }
    }
    return false;
}

bool SmokeSkinnedComparisonStateRetirementQueued(
    const std::deque<
        RtRetiredSmokeSkinnedComparisonBlasPackage>& packages,
    uint64 stateRetirementToken)
{
    if (stateRetirementToken == 0)
    {
        return false;
    }
    for (const RtRetiredSmokeSkinnedComparisonBlasPackage& package :
        packages)
    {
        if (package.stateRetirementToken ==
            stateRetirementToken)
        {
            return true;
        }
    }
    return false;
}

void QueueSmokeSkinnedComparisonBlasRetirement(
    RtSmokeSkinnedComparisonBlasResource&& resource,
    uint64 stateRetirementToken,
    std::deque<
        RtRetiredSmokeSkinnedComparisonBlasPackage>& packages,
    nvrhi::IDevice* device,
    uint64 currentFrame,
    int retireFrames,
    bool& queryFailureLogged)
{
    RtRetiredSmokeSkinnedComparisonBlasPackage package;
    package.retireFrame =
        currentFrame +
        static_cast<uint64>(Max(retireFrames, 0));
    package.stateRetirementToken =
        stateRetirementToken;
    package.completionQuery =
        device ? device->createEventQuery() : nullptr;
    package.resource = std::move(resource);
    if (!package.completionQuery &&
        !queryFailureLogged)
    {
        common->Printf(
            "PathTracePrimaryPass: GEO08 skinned comparison retirement has no GPU completion query; retaining until reset\n");
        queryFailureLogged = true;
    }
    packages.push_back(std::move(package));
}

RtSmokeSkinnedComparisonRetirementAudit
SynchronizeSmokeSkinnedComparisonBlasRetirements(
    PtSkinnedBlasStateTable& stateTable,
    std::vector<RtSmokeSkinnedComparisonBlasResource>& resources,
    std::deque<
        RtRetiredSmokeSkinnedComparisonBlasPackage>& packages,
    nvrhi::IDevice* device,
    uint64 currentFrame,
    int retireFrames,
    bool& queryFailureLogged)
{
    RtSmokeSkinnedComparisonRetirementAudit audit;

    // Preserve state-table token order in the completion queue. That makes
    // ReleaseRetirementsThrough exact even when multiple removals arrive in
    // one frame.
    for (const PtSkinnedBlasRetiredRecord& retired :
        stateTable.PendingRetirements())
    {
        if (SmokeSkinnedComparisonStateRetirementQueued(
                packages,
                retired.retirementToken))
        {
            continue;
        }
        auto resourceIt = std::find_if(
            resources.begin(),
            resources.end(),
            [&retired](
                const RtSmokeSkinnedComparisonBlasResource& resource)
            {
                return
                    resource.instanceKey ==
                        retired.instanceKey &&
                    resource.blasGeneration ==
                        retired.blasGeneration &&
                    resource.outputStorageGeneration ==
                        retired.outputStorageGeneration;
            });
        RtSmokeSkinnedComparisonBlasResource resource;
        if (resourceIt != resources.end())
        {
            resource = std::move(*resourceIt);
            resources.erase(resourceIt);
            ++audit.activeRetired;
        }
        else
        {
            ++audit.emptyStatePackagesQueued;
        }
        QueueSmokeSkinnedComparisonBlasRetirement(
            std::move(resource),
            retired.retirementToken,
            packages,
            device,
            currentFrame,
            retireFrames,
            queryFailureLogged);
        ++audit.statePackagesQueued;
    }

    // Gate-off Clear intentionally removes pure state immediately. Retain any
    // remaining physical owners behind the same exact queue-completion fence.
    for (size_t resourceIndex = 0;
        resourceIndex < resources.size();)
    {
        if (stateTable.Find(
                resources[resourceIndex].instanceKey) != nullptr)
        {
            ++resourceIndex;
            continue;
        }
        RtSmokeSkinnedComparisonBlasResource resource =
            std::move(resources[resourceIndex]);
        resources.erase(
            resources.begin() +
                static_cast<std::ptrdiff_t>(
                    resourceIndex));
        QueueSmokeSkinnedComparisonBlasRetirement(
            std::move(resource),
            0,
            packages,
            device,
            currentFrame,
            retireFrames,
            queryFailureLogged);
        ++audit.activeRetired;
    }
    audit.pendingPackages =
        static_cast<int>(packages.size());
    return audit;
}

RtSmokeSkinnedComparisonBlasAudit
SubmitSmokeSkinnedComparisonBlases(
    const std::vector<RtSmokeSkinnedSurfaceRecord>& records,
    const RtSmokeGeometryUniverse& geometryUniverse,
    PtSkinnedBlasStateTable& stateTable,
    std::vector<RtSmokeSkinnedComparisonBlasResource>& resources,
    std::deque<
        RtRetiredSmokeSkinnedComparisonBlasPackage>& retiredResources,
    nvrhi::IDevice* device,
    nvrhi::ICommandList* commandList,
    nvrhi::BufferHandle outputBuffer,
    uint64 outputBufferGeneration,
    bool gate,
    uint64 frameIndex,
    int retireFrames,
    bool& retirementQueryFailureLogged)
{
    RtSmokeSkinnedComparisonBlasAudit audit;
    audit.gate = gate;
    audit.candidates = static_cast<int>(records.size());
    if (!gate ||
        device == nullptr ||
        commandList == nullptr ||
        !outputBuffer ||
        outputBufferGeneration == 0)
    {
        audit.activeResources =
            static_cast<int>(resources.size());
        return audit;
    }

    const uint64 sourceIndexGeneration =
        geometryUniverse.
            CanonicalSourceIndexPoolGeneration();
    const uint64 sourceIndexCapacity =
        geometryUniverse.
            CanonicalSourceIndexPoolCapacityBytes();
    const nvrhi::BufferHandle sourceIndexBuffer =
        geometryUniverse.CanonicalSourceIndexBuffer();
    if (!sourceIndexBuffer ||
        sourceIndexGeneration == 0 ||
        sourceIndexCapacity == 0)
    {
        audit.failed = audit.candidates;
        audit.activeResources =
            static_cast<int>(resources.size());
        return audit;
    }

    bool inputBarriersCommitted = false;
    for (const RtSmokeSkinnedSurfaceRecord& record : records)
    {
        const PtSkinnedBlasRecord* state =
            stateTable.Find(record.canonicalInstance);
        if (state == nullptr)
        {
            continue;
        }
        PtSkinnedBlasAction action =
            PtSkinnedBlasAction::None;
        switch (state->state)
        {
            case PtSkinnedBlasState::BuildPending:
                ++audit.buildPending;
                action = PtSkinnedBlasAction::Build;
                break;
            case PtSkinnedBlasState::UpdatePending:
                ++audit.updatePending;
                action = PtSkinnedBlasAction::Update;
                break;
            case PtSkinnedBlasState::RebuildPending:
                ++audit.rebuildPending;
                action = PtSkinnedBlasAction::Rebuild;
                break;
            default:
                continue;
        }

        const PtGeometryIdentityBinding* binding =
            geometryUniverse.FindCanonicalIdentityBinding(
                record.canonicalInstance);
        const PtGeometrySourceRecord* source =
            binding != nullptr
                ? geometryUniverse.FindCanonicalSourceRecord(
                    binding->meshKey)
                : nullptr;
        const PtGeometryGpuPoolRecord* sourceGpu =
            binding != nullptr
                ? geometryUniverse.FindCanonicalSourceGpuRecord(
                    binding->meshKey)
                : nullptr;
        uint64 sourceIndexEnd = 0;
        uint64 outputVertexBytes = 0;
        uint64 outputEnd = 0;
        const bool exact =
            binding != nullptr &&
            source != nullptr &&
            sourceGpu != nullptr &&
            binding->meshKey == state->meshKey &&
            source->key == state->meshKey &&
            source->sourceChecksum == state->sourceChecksum &&
            sourceGpu->key == state->meshKey &&
            sourceGpu->sourceChecksum == state->sourceChecksum &&
            sourceGpu->indexes.storageGeneration ==
                state->sourceGpuIndexGeneration &&
            state->sourceGpuIndexGeneration ==
                sourceIndexGeneration &&
            sourceGpu->indexes.offsetBytes ==
                state->sourceIndexOffsetBytes &&
            sourceGpu->indexes.sizeBytes ==
                state->sourceIndexBytes &&
            PtCheckedAddU64(
                state->sourceIndexOffsetBytes,
                state->sourceIndexBytes,
                sourceIndexEnd) &&
            sourceIndexEnd <= sourceIndexCapacity &&
            state->outputStorageGeneration ==
                outputBufferGeneration &&
            PtCheckedMulU64(
                state->outputVertexCount,
                sizeof(PathTraceSmokeVertex),
                outputVertexBytes) &&
            outputVertexBytes == state->outputVertexBytes &&
            PtCheckedAddU64(
                state->outputVertexOffsetBytes,
                state->outputVertexBytes,
                outputEnd) &&
            outputEnd <= outputBuffer->getDesc().byteSize &&
            state->outputVertexCount ==
                static_cast<uint64>(record.vertexCount);
        if (!exact)
        {
            stateTable.MarkSubmitted(
                record.canonicalInstance,
                action,
                false,
                frameIndex);
            ++audit.failed;
            continue;
        }
        ++audit.exactContracts;

        RtSmokeSkinnedComparisonBlasResource* resource =
            FindSmokeSkinnedComparisonBlasResource(
                resources,
                record.canonicalInstance);
        const bool reusable =
            resource != nullptr &&
            SmokeSkinnedComparisonBlasContractMatches(
                *resource,
                *state,
                outputBuffer,
                sourceIndexBuffer);
        if (!reusable &&
            action == PtSkinnedBlasAction::Update)
        {
            stateTable.MarkSubmitted(
                record.canonicalInstance,
                action,
                false,
                frameIndex);
            ++audit.failed;
            continue;
        }
        if (!reusable)
        {
            nvrhi::rt::GeometryTriangles triangles;
            triangles.vertexBuffer = outputBuffer;
            triangles.indexBuffer = sourceIndexBuffer;
            triangles.vertexFormat = nvrhi::Format::RGB32_FLOAT;
            triangles.indexFormat = nvrhi::Format::R32_UINT;
            triangles.vertexOffset =
                state->outputVertexOffsetBytes;
            triangles.indexOffset =
                state->sourceIndexOffsetBytes;
            triangles.vertexCount =
                static_cast<uint32_t>(
                    state->outputVertexCount);
            triangles.indexCount =
                state->meshKey.indexCount;
            triangles.vertexStride =
                sizeof(PathTraceSmokeVertex);
            nvrhi::rt::GeometryDesc geometry;
            geometry.setTriangles(triangles);

            RtSmokeSkinnedComparisonBlasResource next;
            next.instanceKey = state->instanceKey;
            next.meshKey = state->meshKey;
            next.sourceChecksum = state->sourceChecksum;
            next.sourceGpuIndexGeneration =
                state->sourceGpuIndexGeneration;
            next.sourceIndexOffsetBytes =
                state->sourceIndexOffsetBytes;
            next.sourceIndexBytes =
                state->sourceIndexBytes;
            next.outputStorageGeneration =
                state->outputStorageGeneration;
            next.outputVertexOffsetBytes =
                state->outputVertexOffsetBytes;
            next.outputVertexCount =
                state->outputVertexCount;
            next.vertexBuffer = outputBuffer;
            next.indexBuffer = sourceIndexBuffer;
            next.blasDesc = nvrhi::rt::AccelStructDesc()
                .addBottomLevelGeometry(geometry)
                .setBuildFlags(
                    nvrhi::rt::AccelStructBuildFlags::AllowUpdate |
                    nvrhi::rt::AccelStructBuildFlags::PreferFastBuild)
                .setDebugName(
                    "PathTraceSkinnedComparisonBLAS");
            next.blas =
                device->createAccelStruct(next.blasDesc);
            if (!next.blas)
            {
                stateTable.MarkSubmitted(
                    record.canonicalInstance,
                    action,
                    false,
                    frameIndex);
                ++audit.failed;
                continue;
            }
            if (resource != nullptr)
            {
                RtSmokeSkinnedComparisonBlasResource retired =
                    std::move(*resource);
                *resource = std::move(next);
                QueueSmokeSkinnedComparisonBlasRetirement(
                    std::move(retired),
                    0,
                    retiredResources,
                    device,
                    frameIndex,
                    retireFrames,
                    retirementQueryFailureLogged);
                ++audit.replaced;
            }
            else
            {
                resources.push_back(std::move(next));
                resource = &resources.back();
                ++audit.created;
            }
        }
        else
        {
            ++audit.reused;
        }

        if (!inputBarriersCommitted)
        {
            commandList->setBufferState(
                outputBuffer,
                nvrhi::ResourceStates::
                    AccelStructBuildInput);
            commandList->setBufferState(
                sourceIndexBuffer,
                nvrhi::ResourceStates::
                    AccelStructBuildInput);
            commandList->commitBarriers();
            inputBarriersCommitted = true;
        }
        nvrhi::rt::AccelStructDesc submitDesc =
            resource->blasDesc;
        if (action == PtSkinnedBlasAction::Update)
        {
            submitDesc.buildFlags =
                submitDesc.buildFlags |
                nvrhi::rt::AccelStructBuildFlags::
                    PerformUpdate;
        }
        nvrhi::utils::BuildBottomLevelAccelStruct(
            commandList,
            resource->blas,
            submitDesc);
        if (stateTable.MarkSubmitted(
                record.canonicalInstance,
                action,
                true,
                frameIndex) ==
            PtSkinnedBlasSubmitResult::Succeeded)
        {
            const PtSkinnedBlasRecord* submittedState =
                stateTable.Find(
                    record.canonicalInstance);
            if (submittedState != nullptr)
            {
                resource->blasGeneration =
                    submittedState->blasGeneration;
            }
            switch (action)
            {
                case PtSkinnedBlasAction::Build:
                    ++audit.buildSubmitted;
                    break;
                case PtSkinnedBlasAction::Update:
                    ++audit.updateSubmitted;
                    break;
                case PtSkinnedBlasAction::Rebuild:
                    ++audit.rebuildSubmitted;
                    break;
                default:
                    break;
            }
        }
        else
        {
            ++audit.failed;
        }
    }
    audit.activeResources =
        static_cast<int>(resources.size());
    return audit;
}

PtSkinnedHitRouteBuild BuildSmokeSkinnedHitRouteShadow(
    const std::vector<RtSmokeSkinnedSurfaceRecord>& records,
    const std::vector<PathTraceSkinnedSurfaceDispatchRecord>&
        dispatchRecords,
    const RtSmokeGeometryUniverse& geometryUniverse,
    const PtSkinnedBlasStateTable& stateTable,
    const std::vector<RtSmokeSkinnedComparisonBlasResource>&
        comparisonResources,
    const std::vector<uint32_t>& dynamicIndexes,
    const std::vector<uint32_t>& dynamicTriangleClasses,
    const std::vector<uint32_t>& dynamicTriangleMaterialIds,
    const std::vector<uint32_t>&
        dynamicTriangleMaterialIndexes,
    const RtSmokeMaterialTableBuild& materialTable,
    nvrhi::BufferHandle outputBuffer,
    uint64 previousPositionCount,
    uint64 outputCapacityBytes,
    uint64 firstShaderInstanceId,
    bool gate,
    bool sourceOnlyMetadata)
{
    PtSkinnedHitRouteLegacyView legacy;
    legacy.indexes = dynamicIndexes.data();
    legacy.indexCount = dynamicIndexes.size();
    legacy.triangleClasses =
        dynamicTriangleClasses.data();
    legacy.triangleClassCount =
        dynamicTriangleClasses.size();
    legacy.triangleMaterialIds =
        dynamicTriangleMaterialIds.data();
    legacy.triangleMaterialIdCount =
        dynamicTriangleMaterialIds.size();
    legacy.triangleMaterialIndexes =
        dynamicTriangleMaterialIndexes.data();
    legacy.triangleMaterialIndexCount =
        dynamicTriangleMaterialIndexes.size();

    std::vector<PtSkinnedHitRouteCandidate> candidates;
    if (!gate)
    {
        return PtBuildSkinnedHitRoutes(
            candidates,
            legacy,
            firstShaderInstanceId);
    }
    candidates.reserve(dispatchRecords.size());
    const uint64 sourceIndexGeneration =
        geometryUniverse.
            CanonicalSourceIndexPoolGeneration();
    const uint64 sourceIndexCapacity =
        geometryUniverse.
            CanonicalSourceIndexPoolCapacityBytes();
    const nvrhi::BufferHandle sourceIndexBuffer =
        geometryUniverse.CanonicalSourceIndexBuffer();

    for (const PathTraceSkinnedSurfaceDispatchRecord& dispatch :
        dispatchRecords)
    {
        PtSkinnedHitRouteCandidate candidate;
        if (dispatch.surfaceRecordIndex >= records.size())
        {
            candidates.push_back(candidate);
            continue;
        }
        const RtSmokeSkinnedSurfaceRecord& record =
            records[dispatch.surfaceRecordIndex];
        candidate.instanceKey = record.canonicalInstance;
        candidate.fallbackMaterialId = record.materialId;
        candidate.fallbackTriangleClassAndFlags =
            record.triangleClassAndFlags;
        candidate.previousPositionOffset =
            dispatch.previousPositionOffset;
        candidate.previousPositionCount =
            previousPositionCount;
        candidate.previousValid =
            dispatch.previousPositionOffset != UINT32_MAX &&
            (dispatch.flags &
                PT_SKINNED_DISPATCH_HAS_VALID_PREVIOUS) != 0u;
        // Once the canonical source/output route is enabled, build its hit
        // metadata from immutable source primitives even while a CPU copy is
        // retained for fail-closed bootstrap. This makes the route safe to
        // pre-admit without depending on the legacy merged-buffer capacity.
        candidate.legacyVertexOffset =
            sourceOnlyMetadata
                ? 0u
                : dispatch.dynamicVertexOffset;
        candidate.legacyVertexCount =
            sourceOnlyMetadata
                ? 0u
                : dispatch.vertexCount;
        candidate.legacyIndexOffset =
            sourceOnlyMetadata
                ? 0u
                : dispatch.dynamicIndexOffset;
        candidate.legacyIndexCount =
            sourceOnlyMetadata
                ? 0u
                : static_cast<uint64>(
                    dispatch.triangleCount) * 3;
        candidate.legacyTriangleOffset =
            sourceOnlyMetadata
                ? 0u
                : dispatch.dynamicTriangleOffset;
        candidate.legacyTriangleCount =
            sourceOnlyMetadata
                ? 0u
                : dispatch.triangleCount;
        candidate.legacyCapturePresent =
            !sourceOnlyMetadata;
        const int fallbackMaterialIndex =
            FindSmokeMaterialTableIndexById(
                materialTable,
                record.materialId);
        if (fallbackMaterialIndex >= 0)
        {
            candidate.fallbackMaterialIndex =
                static_cast<uint32_t>(
                    fallbackMaterialIndex);
        }
        if (!sourceOnlyMetadata &&
            dispatch.dynamicTriangleOffset <
            dynamicTriangleMaterialIndexes.size())
        {
            candidate.fallbackMaterialIndex =
                dynamicTriangleMaterialIndexes[
                    dispatch.dynamicTriangleOffset];
        }
        if (!sourceOnlyMetadata &&
            dispatch.dynamicTriangleOffset <
            dynamicTriangleClasses.size())
        {
            candidate.fallbackTriangleClassAndFlags =
                dynamicTriangleClasses[
                    dispatch.dynamicTriangleOffset];
        }

        const PtSkinnedBlasRecord* state =
            stateTable.Find(record.canonicalInstance);
        const PtGeometryIdentityBinding* binding =
            geometryUniverse.FindCanonicalIdentityBinding(
                record.canonicalInstance);
        const PtGeometrySourceRecord* source =
            binding != nullptr
                ? geometryUniverse.FindCanonicalSourceRecord(
                    binding->meshKey)
                : nullptr;
        const PtGeometryGpuPoolRecord* sourceGpu =
            binding != nullptr
                ? geometryUniverse.
                    FindCanonicalSourceGpuRecord(
                        binding->meshKey)
                : nullptr;
        const RtSmokeSkinnedComparisonBlasResource* resource =
            nullptr;
        for (const RtSmokeSkinnedComparisonBlasResource&
            comparisonResource : comparisonResources)
        {
            if (comparisonResource.instanceKey ==
                record.canonicalInstance)
            {
                resource = &comparisonResource;
                break;
            }
        }
        if (state != nullptr)
        {
            candidate.meshKey = state->meshKey;
            candidate.sourceChecksum =
                state->sourceChecksum;
            candidate.sourceGpuIndexGeneration =
                state->sourceGpuIndexGeneration;
            candidate.sourceIndexOffsetBytes =
                state->sourceIndexOffsetBytes;
            candidate.outputStorageGeneration =
                state->outputStorageGeneration;
            candidate.outputVertexOffsetBytes =
                state->outputVertexOffsetBytes;
            candidate.outputVertexCount =
                state->outputVertexCount;
        }
        candidate.sourceIndexCapacityBytes =
            sourceIndexCapacity;
        candidate.outputCapacityBytes =
            outputCapacityBytes;
        if (source != nullptr)
        {
            candidate.sourceIndexes =
                source->payload.indexes.data();
            candidate.sourceIndexCount =
                source->payload.indexes.size();
        }
        candidate.dispatchReady =
            state != nullptr &&
            binding != nullptr &&
            source != nullptr &&
            sourceGpu != nullptr &&
            resource != nullptr &&
            sourceIndexBuffer &&
            outputBuffer &&
            sourceIndexGeneration != 0 &&
            binding->meshKey == state->meshKey &&
            source->key == state->meshKey &&
            source->sourceChecksum ==
                state->sourceChecksum &&
            sourceGpu->key == state->meshKey &&
            sourceGpu->sourceChecksum ==
                state->sourceChecksum &&
            sourceGpu->indexes.storageGeneration ==
                sourceIndexGeneration &&
            state->sourceGpuIndexGeneration ==
                sourceIndexGeneration &&
            sourceGpu->indexes.offsetBytes ==
                state->sourceIndexOffsetBytes &&
            sourceGpu->indexes.sizeBytes ==
                state->sourceIndexBytes &&
            resource->instanceKey ==
                state->instanceKey &&
            resource->meshKey == state->meshKey &&
            resource->sourceChecksum ==
                state->sourceChecksum &&
            resource->sourceGpuIndexGeneration ==
                state->sourceGpuIndexGeneration &&
            resource->sourceIndexOffsetBytes ==
                state->sourceIndexOffsetBytes &&
            resource->sourceIndexBytes ==
                state->sourceIndexBytes &&
            resource->outputStorageGeneration ==
                state->outputStorageGeneration &&
            resource->outputVertexOffsetBytes ==
                state->outputVertexOffsetBytes &&
            resource->outputVertexCount ==
                state->outputVertexCount &&
            resource->blasGeneration ==
                state->blasGeneration &&
            resource->vertexBuffer == outputBuffer &&
            resource->indexBuffer ==
                sourceIndexBuffer &&
            resource->blas;
        candidates.push_back(candidate);
    }

    return PtBuildSkinnedHitRoutes(
        candidates,
        legacy,
        firstShaderInstanceId);
}

void DumpSmokeSkinnedGpuFunnel(
    const RtSmokeSkinnedGpuScaffoldBuild& build,
    const RtSmokeJointCacheStageBuild& jointCacheStage,
    const RtSmokeSkinnedHistoryAudit& historyAudit,
    const RtSmokeSkinnedOutputAudit& outputAudit,
    const std::vector<RtSmokeSkinnedSurfaceRecord>& records,
    const RtSmokeGeometryUniverse& geometryUniverse,
    int mode,
    uint64 frameIndex)
{
    using Result = RtSmokeSkinnedGpuScaffoldBuild::Result;
    int counts[static_cast<int>(Result::Count)] = {};
    for (Result result : build.recordResults)
    {
        const int index = static_cast<int>(result);
        if (index >= 0 && index < static_cast<int>(Result::Count))
        {
            ++counts[index];
        }
    }
    int resultTotal = 0;
    for (int resultIndex = 0; resultIndex < static_cast<int>(Result::Count); ++resultIndex)
    {
        resultTotal += counts[resultIndex];
    }
    const bool exact = resultTotal == static_cast<int>(build.recordResults.size());
    common->Printf(
        "PathTracePrimaryPass: PT GPU skinning funnel frame=%llu mode=%d candidates=%llu eligibleGpu=%d dispatchedGpu=%d notReadyJointData=%d unsupportedLayout=%d invalidWeightsOrJoints=%d invalidCanonicalSource=%d invalidPersistentOutput=%d allocationFailure=%d explicitSingleBoneRoute=%d cpuFallback=%d singleBoneObserved=%d resultTotal=%d exact=%d\n",
        static_cast<unsigned long long>(frameIndex),
        mode,
        static_cast<unsigned long long>(build.recordResults.size()),
        counts[static_cast<int>(Result::EligibleGpu)],
        counts[static_cast<int>(Result::DispatchedGpu)],
        counts[static_cast<int>(Result::NotReadyJointData)],
        counts[static_cast<int>(Result::UnsupportedLayout)],
        counts[static_cast<int>(Result::InvalidWeightsOrJoints)],
        counts[static_cast<int>(Result::InvalidCanonicalSource)],
        counts[static_cast<int>(Result::InvalidPersistentOutput)],
        counts[static_cast<int>(Result::AllocationFailure)],
        counts[static_cast<int>(Result::ExplicitSingleBoneRoute)],
        counts[static_cast<int>(Result::CpuFallback)],
        build.singleBoneObserved,
        resultTotal,
        exact ? 1 : 0);
    assert(exact);

    int jointHandlePresent = 0;
    int jointRangeResolved = 0;
    int jointRangeStale = 0;
    int jointRangeSizeMatch = 0;
    int jointRangeSizeMismatch = 0;
    int canonicalInstanceValid = 0;
    int historyOwnerValid = 0;
    int jointSnapshotAvailable = 0;
    int jointSnapshotComparable = 0;
    int jointSnapshotSourceChanged = 0;
    uint64 resolvedJointBytes = 0;
    std::unordered_set<nvrhi::IBuffer*> resolvedJointBuffers;
    PtJointCacheCopyPlanner copyPlanner;
    const PtJointCacheCopyPlanResult plannerInit =
        PtInitializeJointCacheCopyPlanner(
            copyPlanner,
            std::numeric_limits<std::uint64_t>::max());
    assert(plannerInit == PtJointCacheCopyPlanResult::PlannedCopy);
    int copyPlanNew = 0;
    int copyPlanReused = 0;
    int copyPlanRejected = 0;
    std::vector<PtJointCacheCopyPlanResult> copyPlanResults(
        records.size(),
        PtJointCacheCopyPlanResult::InvalidState);
    std::vector<PtJointCacheCopyPlan> copyPlans(records.size());
    for (size_t recordIndex = 0; recordIndex < records.size(); ++recordIndex)
    {
        const RtSmokeSkinnedSurfaceRecord& record = records[recordIndex];
        canonicalInstanceValid +=
            PtCanonicalInstanceKeyIsValid(record.canonicalInstance)
                ? 1
                : 0;
        historyOwnerValid +=
            PtCanonicalHistoryOwnerKeyIsValid(record.historyOwner)
                ? 1
                : 0;
        const bool snapshotAvailable =
            record.jointCacheCpuSnapshot != 0 &&
            record.jointCacheCpuSnapshotCount == record.jointCount &&
            record.jointCount > 0;
        jointSnapshotAvailable += snapshotAvailable ? 1 : 0;
        jointSnapshotComparable +=
            record.jointCacheCpuSourceComparable ? 1 : 0;
        jointSnapshotSourceChanged +=
            record.jointCacheCpuSourceChanged ? 1 : 0;
        if (record.jointCacheHandle == 0)
        {
            continue;
        }
        ++jointHandlePresent;
        idUniformBuffer jointRange;
        if (!vertexCache.GetJointBuffer(
                static_cast<vertCacheHandle_t>(
                    record.jointCacheHandle),
                &jointRange))
        {
            ++jointRangeStale;
            continue;
        }
        ++jointRangeResolved;
        const uint64 rangeBytes =
            static_cast<uint64>(jointRange.GetSize());
        uint64 expectedBytes = 0;
        const bool expectedBytesValid =
            record.jointCount > 0 &&
            PtCheckedMulU64(
                static_cast<uint64>(record.jointCount),
                sizeof(idJointMat),
                expectedBytes);
        resolvedJointBytes += rangeBytes;
        resolvedJointBuffers.insert(jointRange.GetAPIObject());
        if (expectedBytesValid && rangeBytes == expectedBytes)
        {
            ++jointRangeSizeMatch;
        }
        else
        {
            ++jointRangeSizeMismatch;
        }

        PtJointCacheCopyRequest copyRequest;
        copyRequest.instance = record.canonicalInstance;
        copyRequest.sourceBufferIdentity = static_cast<uint64>(
            reinterpret_cast<std::uintptr_t>(
                jointRange.GetAPIObject()));
        copyRequest.sourceBufferBytes =
            jointRange.GetAPIObject()
                ? static_cast<uint64>(
                    jointRange.GetAPIObject()->getDesc().byteSize)
                : 0;
        copyRequest.sourceOffsetBytes =
            static_cast<uint64>(jointRange.GetOffset());
        copyRequest.sourceRangeBytes = rangeBytes;
        copyRequest.jointCount =
            record.jointCount > 0
                ? static_cast<uint64>(record.jointCount)
                : 0;
        copyPlanResults[recordIndex] =
            PtPlanJointCacheCopy(
                copyPlanner,
                copyRequest,
                copyPlans[recordIndex]);
        switch (copyPlanResults[recordIndex])
        {
            case PtJointCacheCopyPlanResult::PlannedCopy:
                ++copyPlanNew;
                break;
            case PtJointCacheCopyPlanResult::ReusedCopy:
                ++copyPlanReused;
                break;
            default:
                ++copyPlanRejected;
                break;
        }
    }
    common->Printf(
        "PathTracePrimaryPass: GEO07 jointCache audit frame=%llu gate=%d candidates=%llu canonicalInstance/historyOwner=%d/%d handle(present/resolved/stale)=%d/%d/%d range(match/mismatch/bytes)=%d/%d/%llu buffers=%llu copyPlan(new/reused/rejected/bytes)=%d/%d/%d/%llu stage(requested/ready/submitted/copies/bytes)=%d/%d/%d/%llu/%llu oracle(snapshot/comparable/sourceChanged)=%d/%d/%d source=renderer-drawList-jointCache route=%s\n",
        static_cast<unsigned long long>(frameIndex),
        r_pathTracingGeometryAuthoritativeGpuSkinning.GetInteger(),
        static_cast<unsigned long long>(records.size()),
        canonicalInstanceValid,
        historyOwnerValid,
        jointHandlePresent,
        jointRangeResolved,
        jointRangeStale,
        jointRangeSizeMatch,
        jointRangeSizeMismatch,
        static_cast<unsigned long long>(resolvedJointBytes),
        static_cast<unsigned long long>(
            resolvedJointBuffers.size()),
        copyPlanNew,
        copyPlanReused,
        copyPlanRejected,
        static_cast<unsigned long long>(copyPlanner.usedBytes),
        jointCacheStage.requested ? 1 : 0,
        jointCacheStage.ready ? 1 : 0,
        jointCacheStage.submitted ? 1 : 0,
        static_cast<unsigned long long>(
            jointCacheStage.copies.size()),
        static_cast<unsigned long long>(
            jointCacheStage.submittedBytes),
        jointSnapshotAvailable,
        jointSnapshotComparable,
        jointSnapshotSourceChanged,
        jointCacheStage.submitted
            ? "authoritative-current-copy"
            : "plan-only");
    common->Printf(
        "PathTracePrimaryPass: GEO07 skinned history frame=%llu gate/primary/ownerValid/stateFound/read/write=%d/%d/%d/%d/%d/%d records(current/previous/ownerMatch/ownerMismatch)=%d/%d/%d/%d owner(world/generation/kind/role)=%llu/%llu/%u/%u serial(previous/next)=%llu/%llu route=%s\n",
        static_cast<unsigned long long>(frameIndex),
        historyAudit.ownerGate ? 1 : 0,
        historyAudit.primaryView ? 1 : 0,
        historyAudit.ownerValid ? 1 : 0,
        historyAudit.previousStateFound ? 1 : 0,
        historyAudit.readAllowed ? 1 : 0,
        historyAudit.writeAllowed ? 1 : 0,
        historyAudit.currentRecords,
        historyAudit.previousRecords,
        historyAudit.ownerMatches,
        historyAudit.ownerMismatches,
        static_cast<unsigned long long>(
            historyAudit.owner.worldGeneration),
        static_cast<unsigned long long>(
            historyAudit.owner.ownerGeneration),
        static_cast<unsigned int>(
            historyAudit.owner.ownerKind),
        static_cast<unsigned int>(
            historyAudit.owner.ownerRole),
        static_cast<unsigned long long>(
            historyAudit.previousUpdateSerial),
        static_cast<unsigned long long>(
            historyAudit.nextUpdateSerial),
        PtSkinnedHistoryRouteName(historyAudit.route));

    common->Printf(
        "PathTracePrimaryPass: GEO07 skinned output allocator frame=%llu gates(master/source)/worldValid=%d/%d/%d candidates/sourceEligible/ranges/exact=%d/%d/%d/%d observe(add/reuse/resize/reject)=%d/%d/%d/%d retire(applied/missing)=%d/%d vertices(requested/active/used/capacity)=%llu/%llu/%llu/%llu storage(generation/pending)=%llu/%llu sources(unique/sharedPairs/nonAliasedPairs)=%llu/%d/%d overlapPairs=%d strideBytes=%llu route=shadow-full-instance-output-plan\n",
        static_cast<unsigned long long>(frameIndex),
        outputAudit.masterGate ? 1 : 0,
        outputAudit.sourceRegistryGate ? 1 : 0,
        outputAudit.worldValid ? 1 : 0,
        outputAudit.candidates,
        outputAudit.sourceEligible,
        outputAudit.rangesFound,
        outputAudit.exactRanges,
        outputAudit.added,
        outputAudit.reused,
        outputAudit.resized,
        outputAudit.rejected,
        outputAudit.retireApplied,
        outputAudit.retireMissing,
        static_cast<unsigned long long>(
            outputAudit.requestedVertices),
        static_cast<unsigned long long>(
            outputAudit.stats.activeVertices),
        static_cast<unsigned long long>(
            outputAudit.stats.usedVertices),
        static_cast<unsigned long long>(
            outputAudit.stats.capacityVertices),
        static_cast<unsigned long long>(
            outputAudit.stats.storageGeneration),
        static_cast<unsigned long long>(
            outputAudit.stats.pendingStorageGenerations),
        static_cast<unsigned long long>(
            outputAudit.uniqueSources.size()),
        outputAudit.sharedSourcePairs,
        outputAudit.sharedSourceNonAliasedPairs,
        outputAudit.overlapPairs,
        static_cast<unsigned long long>(
            sizeof(PathTraceSmokeVertex)));

    common->Printf(
        "PathTracePrimaryPass: GEO07 skinned source-output dispatch frame=%llu routeEnabled=%d candidates/sourceResolved/sourcePacked/sourceReused/outputExact/dispatches=%llu/%d/%d/%d/%d/%d sourceVertices(unique/uploaded)=%llu/%llu outputVertices(capacity/vector)=%llu/%llu route=%s\n",
        static_cast<unsigned long long>(frameIndex),
        build.canonicalSourceOutputRoute ? 1 : 0,
        static_cast<unsigned long long>(
            build.recordResults.size()),
        build.canonicalSourceResolved,
        build.canonicalSourcePacked,
        build.canonicalSourceReused,
        build.canonicalOutputExact,
        build.canonicalDispatches,
        static_cast<unsigned long long>(
            build.canonicalSourceVertices),
        static_cast<unsigned long long>(
            build.sourceVertices.size()),
        static_cast<unsigned long long>(
            build.canonicalOutputCapacityVertices),
        static_cast<unsigned long long>(
            build.currentOutputVertices.size()),
        build.canonicalSourceOutputRoute
            ? "immutable-source-to-instance-output"
            : "legacy-transient-source-output");

    int bindSourceBindings = 0;
    int bindSourceRecords = 0;
    int bindSourceContractExact = 0;
    int bindSourceChecksumValid = 0;
    uint64 bindSourceRetainedBytes = 0;
    std::unordered_set<uint64> bindSourceMeshes;
    for (const RtSmokeSkinnedSurfaceRecord& record : records)
    {
        const PtGeometryIdentityBinding* binding =
            geometryUniverse.FindCanonicalIdentityBinding(
                record.canonicalInstance);
        if (binding == nullptr)
        {
            continue;
        }
        ++bindSourceBindings;
        const PtGeometrySourceRecord* source =
            geometryUniverse.FindCanonicalSourceRecord(
                binding->meshKey);
        if (source == nullptr)
        {
            continue;
        }
        ++bindSourceRecords;
        const bool exactContract =
            source->key ==
                binding->meshKey &&
            source->key.sourceDomain ==
                PtCanonicalMeshSourceDomain::SkinnedBindSource &&
            source->key.deformationClass ==
                PtCanonicalDeformationClass::Skinned &&
            source->key.vertexCount ==
                static_cast<uint32_t>(record.vertexCount) &&
            source->key.indexCount ==
                static_cast<uint32_t>(record.indexCount);
        bindSourceContractExact += exactContract ? 1 : 0;
        bindSourceChecksumValid +=
            source->sourceChecksum != 0 ? 1 : 0;
        if (bindSourceMeshes.insert(source->meshHash).second)
        {
            bindSourceRetainedBytes +=
                source->retainedBytes;
        }
    }
    const int sharedBindSourceInstances =
        bindSourceRecords -
        static_cast<int>(bindSourceMeshes.size());
    common->Printf(
        "PathTracePrimaryPass: GEO07 skinned bind source frame=%llu candidates=%llu binding/source/exact/checksum=%d/%d/%d/%d uniqueMeshes/sharedInstances=%llu/%d retainedBytes=%llu source=md5-deformInfo-bind-pose route=canonical-instance-to-immutable-source\n",
        static_cast<unsigned long long>(frameIndex),
        static_cast<unsigned long long>(records.size()),
        bindSourceBindings,
        bindSourceRecords,
        bindSourceContractExact,
        bindSourceChecksumValid,
        static_cast<unsigned long long>(
            bindSourceMeshes.size()),
        sharedBindSourceInstances,
        static_cast<unsigned long long>(
            bindSourceRetainedBytes));

    int detailCount = 0;
    for (int pass = 0; pass < 2 && detailCount < 16; ++pass)
    {
        for (size_t recordIndex = 0;
            recordIndex < build.recordResults.size() &&
            recordIndex < records.size() &&
            detailCount < 16;
            ++recordIndex)
        {
            const Result result = build.recordResults[recordIndex];
            const bool successfulDispatch = result == Result::DispatchedGpu;
            if ((pass == 0 && successfulDispatch) || (pass == 1 && !successfulDispatch))
            {
                continue;
            }
            const RtSmokeSkinnedSurfaceRecord& record = records[recordIndex];
            const srfTriangles_t* tri =
                reinterpret_cast<const srfTriangles_t*>(record.key.tri);
            idUniformBuffer jointRange;
            const bool jointRangeResolved =
                record.jointCacheHandle != 0 &&
                vertexCache.GetJointBuffer(
                    static_cast<vertCacheHandle_t>(
                        record.jointCacheHandle),
                    &jointRange);
            common->Printf(
                "PathTracePrimaryPass: PT GPU skinning funnel detail=%d record=%llu entity/model/surface=%d/'%s'/%d vertices/joints=%d/%d singleBone=%d previousValid=%d invalid=0x%08x temporal=0x%08x canonical(instance/history)=%d/%d gpu(source/output/previous)=%d/%lld/%d jointCache(handle/resolved/bytes)=0x%llx/%d/%d oracle(snapshot/comparable/sourceChanged)=%d/%d/%d copyPlan(result/dst/bytes)=%s/%llu/%llu result=%s\n",
                detailCount,
                static_cast<unsigned long long>(recordIndex),
                record.entityIndex,
                record.modelName.c_str(),
                record.drawSurfIndex,
                record.vertexCount,
                record.jointCount,
                IsEntityFeedSingleBoneSurface(tri) ? 1 : 0,
                record.previousValid ? 1 : 0,
                record.invalidReasonFlags,
                record.temporalStateFlags,
                PtCanonicalInstanceKeyIsValid(
                    record.canonicalInstance) ? 1 : 0,
                PtCanonicalHistoryOwnerKeyIsValid(
                    record.historyOwner) ? 1 : 0,
                record.gpuSourceVertexOffset,
                static_cast<long long>(
                    record.gpuOutputVertexOffset),
                record.gpuPreviousPositionOffset,
                static_cast<unsigned long long>(
                    record.jointCacheHandle),
                jointRangeResolved ? 1 : 0,
                jointRangeResolved ? jointRange.GetSize() : 0,
                record.jointCacheCpuSnapshot != 0 &&
                    record.jointCacheCpuSnapshotCount ==
                        record.jointCount
                    ? 1
                    : 0,
                record.jointCacheCpuSourceComparable ? 1 : 0,
                record.jointCacheCpuSourceChanged ? 1 : 0,
                PtJointCacheCopyPlanResultName(
                    copyPlanResults[recordIndex]),
                static_cast<unsigned long long>(
                    copyPlans[recordIndex].destinationOffsetBytes),
                static_cast<unsigned long long>(
                    copyPlans[recordIndex].byteCount),
                SmokeSkinnedGpuResultName(result));
            ++detailCount;
        }
    }
}

static void BuildSmokeSkinnedTriangleDispatchIndex(
    RtSmokeSkinnedGpuScaffoldBuild& build,
    int dynamicTriangleCount)
{
    build.dynamicTriangleDispatchIndexes.assign(Max(0, dynamicTriangleCount), UINT32_MAX);
    build.mappedDynamicTriangles = 0;
    for (int dispatchIndex = 0; dispatchIndex < static_cast<int>(build.dispatchRecords.size()); ++dispatchIndex)
    {
        const PathTraceSkinnedSurfaceDispatchRecord& dispatch = build.dispatchRecords[dispatchIndex];
        if (dispatch.dynamicTriangleOffset == UINT32_MAX || dispatch.triangleCount == 0u)
        {
            continue;
        }
        const uint64 start = static_cast<uint64>(dispatch.dynamicTriangleOffset);
        const uint64 end = start + static_cast<uint64>(dispatch.triangleCount);
        if (start >= static_cast<uint64>(build.dynamicTriangleDispatchIndexes.size()))
        {
            continue;
        }
        const uint64 clampedEnd = Min<uint64>(end, static_cast<uint64>(build.dynamicTriangleDispatchIndexes.size()));
        for (uint64 triangleIndex = start; triangleIndex < clampedEnd; ++triangleIndex)
        {
            if (build.dynamicTriangleDispatchIndexes[static_cast<size_t>(triangleIndex)] == UINT32_MAX)
            {
                ++build.mappedDynamicTriangles;
            }
            build.dynamicTriangleDispatchIndexes[static_cast<size_t>(triangleIndex)] = static_cast<uint32_t>(dispatchIndex);
        }
    }
}

bool SmokeSkinnedSurfaceKeysEqual(const RtSmokeSkinnedSurfaceKey& a, const RtSmokeSkinnedSurfaceKey& b)
{
    return a.entityIndex == b.entityIndex &&
        a.entityDef == b.entityDef &&
        a.model == b.model &&
        a.tri == b.tri &&
        a.materialId == b.materialId &&
        a.surfaceClassId == b.surfaceClassId;
}

bool SmokeSkinnedSurfaceLooseKeysEqual(const RtSmokeSkinnedSurfaceKey& a, const RtSmokeSkinnedSurfaceKey& b)
{
    return a.entityIndex == b.entityIndex &&
        a.entityDef == b.entityDef &&
        a.model == b.model &&
        a.tri == b.tri;
}

const RtSmokeSkinnedSurfaceRecord* FindSmokeSkinnedPreviousRecord(
    const std::vector<RtSmokeSkinnedSurfaceRecord>& previousRecords,
    const RtSmokeSkinnedSurfaceRecord& current)
{
    for (const RtSmokeSkinnedSurfaceRecord& previous : previousRecords)
    {
        if (SmokeSkinnedSurfaceKeysEqual(previous.key, current.key))
        {
            return &previous;
        }
    }
    return nullptr;
}

const RtSmokeSkinnedSurfaceRecord* FindSmokeSkinnedPreviousLooseRecord(
    const std::vector<RtSmokeSkinnedSurfaceRecord>& previousRecords,
    const RtSmokeSkinnedSurfaceRecord& current)
{
    for (const RtSmokeSkinnedSurfaceRecord& previous : previousRecords)
    {
        if (SmokeSkinnedSurfaceLooseKeysEqual(previous.key, current.key))
        {
            return &previous;
        }
    }
    return nullptr;
}

bool SmokeSkinnedCurrentVertexRangeValid(const RtSmokeSkinnedSurfaceRecord& record, const std::vector<PathTraceSmokeVertex>& dynamicVertexData)
{
    return record.currentVertexOffset >= 0 &&
        record.vertexCount > 0 &&
        record.currentVertexOffset <= static_cast<int>(dynamicVertexData.size()) &&
        record.vertexCount <= static_cast<int>(dynamicVertexData.size()) - record.currentVertexOffset;
}

void UpdateSmokeSkinnedPreviousCpuBridge(
    std::vector<RtSmokeSkinnedSurfaceRecord>& currentRecords,
    const std::vector<RtSmokeSkinnedSurfaceRecord>& previousRecords,
    const std::vector<PathTraceSmokeVertex>& previousSkinnedVertexData,
    const std::vector<PathTraceSmokeVertex>& dynamicVertexData,
    std::vector<PathTraceSmokeVertex>& nextPreviousSkinnedVertexData)
{
    const bool hadPreviousFrame = !previousRecords.empty() || !previousSkinnedVertexData.empty();
    const bool dumpIdentityMisses = r_pathTracingGpuSkinningParityDump.GetInteger() != 0;
    int identityMissDetailCount = 0;
    const float teleportDistanceSqr = RT_SMOKE_SKINNED_TELEPORT_DISTANCE * RT_SMOKE_SKINNED_TELEPORT_DISTANCE;

    nextPreviousSkinnedVertexData.clear();
    for (RtSmokeSkinnedSurfaceRecord& current : currentRecords)
    {
        const RtSmokeSkinnedSurfaceRecord* previous = FindSmokeSkinnedPreviousRecord(previousRecords, current);
        const RtSmokeSkinnedSurfaceRecord* loosePrevious =
            previous
                ? previous
                : FindSmokeSkinnedPreviousLooseRecord(
                    previousRecords,
                    current);

        PtSkinnedSurfaceTemporalPolicyInput policyInput;
        policyInput.rtCpuSkinned = current.rtCpuSkinned;
        policyInput.hadPreviousFrame = hadPreviousFrame;
        policyInput.previousSurfaceFound = previous != nullptr;
        policyInput.loosePreviousSurfaceFound =
            loosePrevious != nullptr;
        if (loosePrevious)
        {
            policyInput.materialStable =
                loosePrevious->key.materialId ==
                    current.key.materialId;
            policyInput.surfaceClassStable =
                loosePrevious->key.surfaceClassId ==
                    current.key.surfaceClassId;
        }
        if (previous)
        {
            policyInput.vertexCountStable =
                previous->vertexCount == current.vertexCount;
            policyInput.indexCountStable =
                previous->indexCount == current.indexCount;
            policyInput.triangleCountStable =
                previous->triangleCount == current.triangleCount;
            policyInput.skeletonStable =
                previous->jointCount == current.jointCount &&
                previous->jointSource == current.jointSource;
            policyInput.transformContinuous =
                !(
                    previous->hasEntityOrigin &&
                    current.hasEntityOrigin &&
                    (current.entityOrigin -
                        previous->entityOrigin).LengthSqr() >
                        teleportDistanceSqr);
            policyInput.previousBufferAvailable =
                previous->retainedVertexOffset >= 0 &&
                previous->vertexCount > 0 &&
                previous->retainedVertexOffset <=
                    static_cast<int>(
                        previousSkinnedVertexData.size()) &&
                previous->vertexCount <=
                    static_cast<int>(
                        previousSkinnedVertexData.size()) -
                        previous->retainedVertexOffset;
        }

        if (hadPreviousFrame && !previous)
        {
            if (dumpIdentityMisses)
            {
                int sameEntityCandidates = 0;
                for (const RtSmokeSkinnedSurfaceRecord& candidate : previousRecords)
                {
                    if (candidate.entityIndex != current.entityIndex)
                    {
                        continue;
                    }
                    ++sameEntityCandidates;
                    if (identityMissDetailCount >= 16)
                    {
                        continue;
                    }
                    common->Printf(
                        "PathTracePrimaryPass: PT skinned previous identity miss detail=%d entity=%d model='%s' current(modelSurface/entityDef/model/tri)=%d/%p/%p/%p previous(modelSurface/entityDef/model/tri)=%d/%p/%p/%p equal(entityDef/model/tri/modelSurface)=%d/%d/%d/%d\n",
                        identityMissDetailCount,
                        current.entityIndex,
                        current.modelName.c_str(),
                        current.modelSurfaceIndex,
                        reinterpret_cast<const void*>(current.key.entityDef),
                        reinterpret_cast<const void*>(current.key.model),
                        reinterpret_cast<const void*>(current.key.tri),
                        candidate.modelSurfaceIndex,
                        reinterpret_cast<const void*>(candidate.key.entityDef),
                        reinterpret_cast<const void*>(candidate.key.model),
                        reinterpret_cast<const void*>(candidate.key.tri),
                        candidate.key.entityDef == current.key.entityDef ? 1 : 0,
                        candidate.key.model == current.key.model ? 1 : 0,
                        candidate.key.tri == current.key.tri ? 1 : 0,
                        candidate.modelSurfaceIndex == current.modelSurfaceIndex ? 1 : 0);
                    ++identityMissDetailCount;
                }
                if (sameEntityCandidates == 0 && identityMissDetailCount < 16)
                {
                    common->Printf(
                        "PathTracePrimaryPass: PT skinned previous identity miss detail=%d entity=%d model='%s' current(modelSurface/entityDef/model/tri)=%d/%p/%p/%p sameEntityCandidates=0\n",
                        identityMissDetailCount,
                        current.entityIndex,
                        current.modelName.c_str(),
                        current.modelSurfaceIndex,
                        reinterpret_cast<const void*>(current.key.entityDef),
                        reinterpret_cast<const void*>(current.key.model),
                        reinterpret_cast<const void*>(current.key.tri));
                    ++identityMissDetailCount;
                }
            }
        }

        const PtSkinnedSurfaceTemporalPolicyDecision policy =
            PtSelectSkinnedSurfaceTemporalPolicy(policyInput);
        if (policy.previousValid)
        {
            current.previousValid = true;
            current.previousVertexOffset = previous->retainedVertexOffset;
            current.previousIndexOffset = previous->currentIndexOffset;
            current.previousTriangleOffset = previous->currentTriangleOffset;
        }
        else
        {
            current.previousValid = false;
        }

        current.invalidReasonFlags =
            policy.invalidReasonFlags;
        current.temporalStateFlags =
            policy.temporalStateFlags;
    }

    nextPreviousSkinnedVertexData.reserve(dynamicVertexData.size());
    for (RtSmokeSkinnedSurfaceRecord& current : currentRecords)
    {
        current.retainedVertexOffset = -1;
        if (!current.rtCpuSkinned || !SmokeSkinnedCurrentVertexRangeValid(current, dynamicVertexData))
        {
            continue;
        }

        current.retainedVertexOffset = static_cast<int>(nextPreviousSkinnedVertexData.size());
        nextPreviousSkinnedVertexData.insert(
            nextPreviousSkinnedVertexData.end(),
            dynamicVertexData.begin() + current.currentVertexOffset,
            dynamicVertexData.begin() + current.currentVertexOffset + current.vertexCount);
    }
}

PathTraceSkinnedSourceVertex BuildSmokeSkinnedSourceVertex(const idDrawVert& drawVert)
{
    const idVec3 normal = drawVert.GetNormal();
    const idVec3 tangent = drawVert.GetTangent();
    const idVec2 texCoord = drawVert.GetTexCoord();

    PathTraceSkinnedSourceVertex vertex = {};
    vertex.localPosition[0] = drawVert.xyz.x;
    vertex.localPosition[1] = drawVert.xyz.y;
    vertex.localPosition[2] = drawVert.xyz.z;
    vertex.localPosition[3] = 1.0f;
    vertex.localNormal[0] = normal.x;
    vertex.localNormal[1] = normal.y;
    vertex.localNormal[2] = normal.z;
    vertex.localNormal[3] = 0.0f;
    vertex.localTangent[0] = tangent.x;
    vertex.localTangent[1] = tangent.y;
    vertex.localTangent[2] = tangent.z;
    vertex.localTangent[3] = drawVert.GetBiTangentSign();
    vertex.texCoord[0] = texCoord.x;
    vertex.texCoord[1] = texCoord.y;
    vertex.texCoord[2] = texCoord.x;
    vertex.texCoord[3] = texCoord.y;
    for (int component = 0; component < 4; ++component)
    {
        vertex.color[component] = drawVert.color[component] * (1.0f / 255.0f);
        vertex.jointIndices[component] = static_cast<uint32_t>(drawVert.color[component]);
        vertex.jointWeights[component] = drawVert.color2[component] * (1.0f / 255.0f);
    }
    return vertex;
}

PathTraceSkinnedSourceVertex BuildSmokeSkinnedSourceVertex(
    const PtGeometrySourcePosition& position,
    const PtGeometrySourceAttribute& attribute)
{
    PathTraceSkinnedSourceVertex vertex = {};
    vertex.localPosition[0] = position.xyz[0];
    vertex.localPosition[1] = position.xyz[1];
    vertex.localPosition[2] = position.xyz[2];
    vertex.localPosition[3] = 1.0f;
    vertex.localNormal[0] = attribute.normal[0];
    vertex.localNormal[1] = attribute.normal[1];
    vertex.localNormal[2] = attribute.normal[2];
    vertex.localNormal[3] = 0.0f;
    vertex.localTangent[0] = attribute.tangent[0];
    vertex.localTangent[1] = attribute.tangent[1];
    vertex.localTangent[2] = attribute.tangent[2];
    vertex.localTangent[3] = attribute.bitangentSign;
    vertex.texCoord[0] = attribute.texCoord[0];
    vertex.texCoord[1] = attribute.texCoord[1];
    vertex.texCoord[2] = attribute.texCoord[0];
    vertex.texCoord[3] = attribute.texCoord[1];
    for (int component = 0; component < 4; ++component)
    {
        vertex.color[component] =
            attribute.color[component];
        vertex.jointIndices[component] =
            static_cast<uint32_t>(
                idMath::ClampInt(
                    0,
                    255,
                    static_cast<int>(
                        attribute.color[component] *
                            255.0f +
                        0.5f)));
        vertex.jointWeights[component] =
            attribute.color2[component];
    }
    return vertex;
}

bool SmokeSkinnedCanonicalSourceLayoutAndWeightsValid(
    const PtGeometrySourceRecord* source,
    int vertexCount,
    int jointCount)
{
    if (source == nullptr ||
        vertexCount <= 0 ||
        jointCount <= 0 ||
        source->payload.positions.size() !=
            static_cast<size_t>(vertexCount) ||
        source->payload.AttributeCount() !=
            static_cast<size_t>(vertexCount))
    {
        return false;
    }
    for (int vertexIndex = 0;
        vertexIndex < vertexCount;
        ++vertexIndex)
    {
        const PtGeometrySourcePosition& position =
            source->payload.positions[
                static_cast<size_t>(vertexIndex)];
        PtGeometrySourceAttribute attribute;
        if (!source->payload.DecodeAttribute(
                static_cast<size_t>(vertexIndex),
                attribute))
        {
            return false;
        }
        if (!std::isfinite(position.xyz[0]) ||
            !std::isfinite(position.xyz[1]) ||
            !std::isfinite(position.xyz[2]))
        {
            return false;
        }
        float weightSum = 0.0f;
        for (int component = 0; component < 4; ++component)
        {
            const int jointIndex =
                static_cast<int>(
                    attribute.color[component] *
                        255.0f +
                    0.5f);
            if (jointIndex < 0 ||
                jointIndex >= jointCount ||
                !std::isfinite(
                    attribute.color2[component]))
            {
                return false;
            }
            weightSum += attribute.color2[component];
        }
        if (weightSum < 254.0f / 255.0f ||
            weightSum > 256.0f / 255.0f)
        {
            return false;
        }
    }
    return true;
}

void CopySmokeObjectToWorldRows(float dst[12], const float src[12])
{
    for (int i = 0; i < 12; ++i)
    {
        dst[i] = src[i];
    }
}

void CopySmokeJointMatrixRows(PathTraceSkinnedJointMatrix& dst, const idJointMat& src)
{
    const float* rows = src.ToFloatPtr();
    for (int i = 0; i < 12; ++i)
    {
        dst.rows[i] = rows[i];
    }
}

const idJointMat* SmokeSkinnedRecordJoints(const RtSmokeSkinnedSurfaceRecord& record)
{
    if (record.jointCacheCpuSnapshot != 0 &&
        record.jointCacheCpuSnapshotCount == record.jointCount &&
        record.jointCount > 0)
    {
        return reinterpret_cast<const idJointMat*>(
            record.jointCacheCpuSnapshot);
    }
    const srfTriangles_t* tri = reinterpret_cast<const srfTriangles_t*>(record.key.tri);
    return GetSmokeRtCpuSkinningJoints(tri);
}

bool SmokeSkinnedJointRangeValid(const RtSmokeSkinnedSurfaceRecord& record, const std::vector<PathTraceSkinnedJointMatrix>& retainedJointMatrices)
{
    return record.retainedJointOffset >= 0 &&
        record.jointCount > 0 &&
        record.retainedJointOffset <= static_cast<int>(retainedJointMatrices.size()) &&
        record.jointCount <= static_cast<int>(retainedJointMatrices.size()) - record.retainedJointOffset;
}

bool AppendSmokeSkinnedJointMatrices(const idJointMat* joints, int jointCount, std::vector<PathTraceSkinnedJointMatrix>& jointMatrices, int& jointOffset)
{
    jointOffset = -1;
    if (!joints || jointCount <= 0)
    {
        return false;
    }

    jointOffset = static_cast<int>(jointMatrices.size());
    jointMatrices.resize(jointMatrices.size() + jointCount);
    for (int jointIndex = 0; jointIndex < jointCount; ++jointIndex)
    {
        CopySmokeJointMatrixRows(jointMatrices[jointOffset + jointIndex], joints[jointIndex]);
    }
    return true;
}

void RetainSmokeSkinnedCurrentJointMatrices(
    std::vector<RtSmokeSkinnedSurfaceRecord>& currentRecords,
    std::vector<PathTraceSkinnedJointMatrix>& nextPreviousSkinnedJointMatrices)
{
    nextPreviousSkinnedJointMatrices.clear();
    for (RtSmokeSkinnedSurfaceRecord& current : currentRecords)
    {
        current.retainedJointOffset = -1;
        if (!current.rtCpuSkinned || current.jointCount <= 0)
        {
            continue;
        }

        int jointOffset = -1;
        if (AppendSmokeSkinnedJointMatrices(
                SmokeSkinnedRecordJoints(current),
                current.jointCount,
                nextPreviousSkinnedJointMatrices,
                jointOffset))
        {
            current.retainedJointOffset = jointOffset;
        }
    }
}

RtSmokeSkinnedGpuScaffoldBuild BuildSmokeSkinnedGpuScaffold(
    int scaffoldMode,
    int gpuSkinningMode,
    bool buildGpuSkinningInputs,
    bool canonicalSourceOutputRoute,
    const RtSmokeGeometryUniverse* geometryUniverse,
    const PtSkinnedOutputAllocator* outputAllocator,
    std::vector<RtSmokeSkinnedSurfaceRecord>& currentRecords,
    const std::vector<RtSmokeSkinnedSurfaceRecord>& previousRecords,
    const std::vector<PathTraceSmokeVertex>& dynamicVertexData,
    const std::vector<PathTraceSmokeVertex>& previousSkinnedVertexData,
    const std::vector<PathTraceSkinnedJointMatrix>& previousSkinnedJointMatrices)
{
    RtSmokeSkinnedGpuScaffoldBuild build;
    using Result = RtSmokeSkinnedGpuScaffoldBuild::Result;
    build.canonicalSourceOutputRoute =
        canonicalSourceOutputRoute;
    if (canonicalSourceOutputRoute &&
        outputAllocator != nullptr)
    {
        build.canonicalOutputCapacityVertices =
            outputAllocator->Stats().capacityVertices;
        if (buildGpuSkinningInputs &&
            build.canonicalOutputCapacityVertices <=
                static_cast<uint64>(
                    std::numeric_limits<size_t>::max()) &&
            build.canonicalOutputCapacityVertices <=
                UINT32_MAX)
        {
            build.currentOutputVertices.resize(
                static_cast<size_t>(
                    build.canonicalOutputCapacityVertices));
        }
    }
    build.recordResults.assign(currentRecords.size(), Result::CpuFallback);
    if (currentRecords.empty())
    {
        return build;
    }

    struct CanonicalSourcePack
    {
        PtCanonicalMeshKey key;
        uint64 checksum = 0;
        int vertexOffset = -1;
    };
    std::vector<CanonicalSourcePack> canonicalSourcePacks;

    for (int recordIndex = 0; recordIndex < static_cast<int>(currentRecords.size()); ++recordIndex)
    {
        RtSmokeSkinnedSurfaceRecord& record = currentRecords[recordIndex];
        const srfTriangles_t* tri = reinterpret_cast<const srfTriangles_t*>(record.key.tri);
        const PtGeometrySourceRecord* canonicalSource = nullptr;
        const PtSkinnedOutputRange* persistentOutput = nullptr;
        if (canonicalSourceOutputRoute &&
            geometryUniverse != nullptr &&
            outputAllocator != nullptr)
        {
            const PtGeometryIdentityBinding* binding =
                geometryUniverse->
                    FindCanonicalIdentityBinding(
                        record.canonicalInstance);
            canonicalSource =
                binding != nullptr
                    ? geometryUniverse->
                        FindCanonicalSourceRecord(
                            binding->meshKey)
                    : nullptr;
            if (canonicalSource == nullptr ||
                binding == nullptr ||
                canonicalSource->key != binding->meshKey ||
                canonicalSource->key.sourceDomain !=
                    PtCanonicalMeshSourceDomain::SkinnedBindSource ||
                canonicalSource->key.deformationClass !=
                    PtCanonicalDeformationClass::Skinned ||
                canonicalSource->key.vertexCount !=
                    static_cast<uint32_t>(record.vertexCount) ||
                canonicalSource->sourceChecksum == 0)
            {
                canonicalSource = nullptr;
            }
            else
            {
                ++build.canonicalSourceResolved;
            }
            persistentOutput =
                outputAllocator->Find(
                    record.canonicalInstance);
            if (persistentOutput != nullptr &&
                persistentOutput->vertexCount ==
                    static_cast<uint64>(record.vertexCount) &&
                persistentOutput->storageGeneration ==
                    outputAllocator->Stats().
                        storageGeneration &&
                persistentOutput->vertexOffset <=
                    build.canonicalOutputCapacityVertices &&
                persistentOutput->vertexCount <=
                    build.canonicalOutputCapacityVertices -
                        persistentOutput->vertexOffset &&
                persistentOutput->vertexOffset <=
                    UINT32_MAX)
            {
                ++build.canonicalOutputExact;
            }
            else
            {
                persistentOutput = nullptr;
            }
        }
        record.gpuSourceVertexOffset = -1;
        record.gpuOutputVertexOffset = -1;
        record.gpuPreviousPositionOffset = -1;

        if (IsEntityFeedSingleBoneSurface(tri))
        {
            ++build.singleBoneObserved;
        }

        const bool currentCpuRangeValid =
            SmokeSkinnedCurrentVertexRangeValid(
                record,
                dynamicVertexData);
        const bool layoutUnsupported =
            record.vertexCount <= 0 ||
            (!record.cpuCaptureOmitted &&
                !currentCpuRangeValid) ||
            (!canonicalSourceOutputRoute &&
                ((tri && !tri->verts) ||
                    (tri &&
                        record.vertexCount >
                            tri->numVerts)));
        if (gpuSkinningMode > 0)
        {
            if (canonicalSourceOutputRoute &&
                canonicalSource == nullptr)
            {
                build.recordResults[recordIndex] =
                    Result::InvalidCanonicalSource;
            }
            else if (canonicalSourceOutputRoute &&
                persistentOutput == nullptr)
            {
                build.recordResults[recordIndex] =
                    Result::InvalidPersistentOutput;
            }
            else if (layoutUnsupported ||
                (!canonicalSourceOutputRoute &&
                    (!tri || !tri->verts)))
            {
                build.recordResults[recordIndex] = Result::UnsupportedLayout;
            }
            else if (!record.rtCpuSkinned ||
                record.jointCount <= 0 ||
                !SmokeSkinnedRecordJoints(record))
            {
                build.recordResults[recordIndex] = Result::NotReadyJointData;
            }
            else if (canonicalSourceOutputRoute
                ? !SmokeSkinnedCanonicalSourceLayoutAndWeightsValid(
                    canonicalSource,
                    record.vertexCount,
                    record.jointCount)
                : !SmokeSkinnedSourceLayoutAndWeightsValid(
                    tri,
                    record.vertexCount,
                    record.jointCount))
            {
                build.recordResults[recordIndex] = Result::InvalidWeightsOrJoints;
            }
            else
            {
                build.recordResults[recordIndex] = Result::EligibleGpu;
            }
        }

        if (scaffoldMode <= 0 ||
            !record.rtCpuSkinned ||
            record.vertexCount <= 0 ||
            (!canonicalSourceOutputRoute &&
                (!tri ||
                    !tri->verts ||
                    record.vertexCount > tri->numVerts)) ||
            (canonicalSourceOutputRoute &&
                (canonicalSource == nullptr ||
                    persistentOutput == nullptr)) ||
            (!record.cpuCaptureOmitted &&
                !currentCpuRangeValid) ||
            (gpuSkinningMode > 0 && build.recordResults[recordIndex] != Result::EligibleGpu))
        {
            continue;
        }

        if (buildGpuSkinningInputs)
        {
            if (canonicalSourceOutputRoute)
            {
                const CanonicalSourcePack* sourcePack = nullptr;
                for (const CanonicalSourcePack& candidate :
                    canonicalSourcePacks)
                {
                    if (candidate.key ==
                            canonicalSource->key &&
                        candidate.checksum ==
                            canonicalSource->
                                sourceChecksum)
                    {
                        sourcePack = &candidate;
                        break;
                    }
                }
                if (sourcePack == nullptr)
                {
                    if (build.sourceVertices.size() >
                            static_cast<size_t>(INT_MAX) ||
                        canonicalSource->payload.positions.
                                size() >
                            static_cast<size_t>(INT_MAX) -
                                build.sourceVertices.size())
                    {
                        build.recordResults[recordIndex] =
                            Result::InvalidCanonicalSource;
                        continue;
                    }
                    CanonicalSourcePack added;
                    added.key = canonicalSource->key;
                    added.checksum =
                        canonicalSource->sourceChecksum;
                    added.vertexOffset =
                        static_cast<int>(
                            build.sourceVertices.size());
                    for (int vertexIndex = 0;
                        vertexIndex < record.vertexCount;
                        ++vertexIndex)
                    {
                        PtGeometrySourceAttribute attribute;
                        canonicalSource->payload.DecodeAttribute(
                            static_cast<size_t>(vertexIndex),
                            attribute);
                        build.sourceVertices.push_back(
                            BuildSmokeSkinnedSourceVertex(
                                canonicalSource->
                                    payload.positions[
                                        static_cast<size_t>(
                                            vertexIndex)],
                                attribute));
                    }
                    build.canonicalSourceVertices +=
                        static_cast<uint64>(
                            record.vertexCount);
                    ++build.canonicalSourcePacked;
                    canonicalSourcePacks.push_back(added);
                    sourcePack =
                        &canonicalSourcePacks.back();
                }
                else
                {
                    ++build.canonicalSourceReused;
                }

                const uint64 outputOffset =
                    persistentOutput->vertexOffset;
                if (outputOffset >
                        static_cast<uint64>(
                            std::numeric_limits<int64>::max()) ||
                    outputOffset >
                        build.currentOutputVertices.size() ||
                    static_cast<uint64>(
                        record.vertexCount) >
                        build.currentOutputVertices.size() -
                            outputOffset)
                {
                    build.recordResults[recordIndex] =
                        Result::InvalidPersistentOutput;
                    continue;
                }
                record.gpuSourceVertexOffset =
                    sourcePack->vertexOffset;
                record.gpuOutputVertexOffset =
                    static_cast<int64>(outputOffset);
                if (!record.cpuCaptureOmitted)
                {
                    for (int vertexIndex = 0;
                        vertexIndex < record.vertexCount;
                        ++vertexIndex)
                    {
                        build.currentOutputVertices[
                            static_cast<size_t>(
                                outputOffset) +
                            static_cast<size_t>(
                                vertexIndex)] =
                            dynamicVertexData[
                                record.currentVertexOffset +
                                vertexIndex];
                    }
                }
            }
            else
            {
                record.gpuSourceVertexOffset =
                    static_cast<int>(
                        build.sourceVertices.size());
                record.gpuOutputVertexOffset =
                    static_cast<int64>(
                        build.currentOutputVertices.size());
                for (int vertexIndex = 0;
                    vertexIndex < record.vertexCount;
                    ++vertexIndex)
                {
                    build.sourceVertices.push_back(
                        BuildSmokeSkinnedSourceVertex(
                            tri->verts[vertexIndex]));
                    build.currentOutputVertices.push_back(
                        dynamicVertexData[
                            record.currentVertexOffset +
                            vertexIndex]);
                }
            }
        }

        const RtSmokeSkinnedSurfaceRecord* previousRecord =
            FindSmokeSkinnedPreviousRecord(
                previousRecords,
                record);
        const bool hasPreviousPositionRange =
            record.previousValid &&
            record.previousVertexOffset >= 0 &&
            record.previousVertexOffset <= static_cast<int>(previousSkinnedVertexData.size()) &&
            record.vertexCount <= static_cast<int>(previousSkinnedVertexData.size()) - record.previousVertexOffset;
        if (hasPreviousPositionRange)
        {
            record.gpuPreviousPositionOffset = static_cast<int>(build.previousPositions.size());
            for (int vertexIndex = 0; vertexIndex < record.vertexCount; ++vertexIndex)
            {
                const PathTraceSmokeVertex& previousVertex = previousSkinnedVertexData[record.previousVertexOffset + vertexIndex];
                PathTraceSkinnedPreviousPosition previousPosition = {};
                previousPosition.previousPosition[0] = previousVertex.position[0];
                previousPosition.previousPosition[1] = previousVertex.position[1];
                previousPosition.previousPosition[2] = previousVertex.position[2];
                previousPosition.previousPosition[3] = 1.0f;
                build.previousPositions.push_back(previousPosition);
            }
        }
        const uint32_t requiredGpuPreviousFlags =
            RT_SMOKE_SKINNED_TEMPORAL_TOPOLOGY_STABLE |
            RT_SMOKE_SKINNED_TEMPORAL_LOD_STABLE |
            RT_SMOKE_SKINNED_TEMPORAL_TRANSFORM_CONTINUOUS |
            RT_SMOKE_SKINNED_TEMPORAL_DEFORMATION_CONTINUOUS;
        const bool hasGpuPreviousJointPose =
            previousRecord != nullptr &&
            (record.temporalStateFlags &
                requiredGpuPreviousFlags) ==
                    requiredGpuPreviousFlags &&
            previousRecord->jointCount == record.jointCount &&
            SmokeSkinnedJointRangeValid(
                *previousRecord,
                previousSkinnedJointMatrices);
        if (!hasPreviousPositionRange &&
            hasGpuPreviousJointPose)
        {
            record.gpuPreviousPositionOffset =
                static_cast<int>(
                    build.previousPositions.size());
            build.previousPositions.resize(
                build.previousPositions.size() +
                    static_cast<size_t>(
                        record.vertexCount));
        }

        PathTraceSkinnedSurfaceDispatchRecord dispatch = {};
        dispatch.sourceVertexOffset = record.gpuSourceVertexOffset >= 0 ? static_cast<uint32_t>(record.gpuSourceVertexOffset) : UINT32_MAX;
        dispatch.outputVertexOffset =
            record.gpuOutputVertexOffset >= 0 &&
                static_cast<uint64>(
                    record.gpuOutputVertexOffset) <=
                    UINT32_MAX
                ? static_cast<uint32_t>(
                    record.gpuOutputVertexOffset)
                : UINT32_MAX;
        dispatch.previousPositionOffset = record.gpuPreviousPositionOffset >= 0 ? static_cast<uint32_t>(record.gpuPreviousPositionOffset) : UINT32_MAX;
        dispatch.vertexCount = static_cast<uint32_t>(record.vertexCount);
        dispatch.currentJointOffset = UINT32_MAX;
        dispatch.previousJointOffset = UINT32_MAX;
        dispatch.surfaceRecordIndex = static_cast<uint32_t>(recordIndex);
        dispatch.flags = PT_SKINNED_DISPATCH_RT_CPU_SKINNED | PT_SKINNED_DISPATCH_SOURCE_READY;
        dispatch.dynamicVertexOffset =
            record.cpuCaptureOmitted
                ? UINT32_MAX
                : static_cast<uint32_t>(
                    record.currentVertexOffset);
        dispatch.dynamicIndexOffset =
            record.cpuCaptureOmitted
                ? UINT32_MAX
                : static_cast<uint32_t>(
                    record.currentIndexOffset);
        dispatch.dynamicTriangleOffset =
            record.cpuCaptureOmitted
                ? UINT32_MAX
                : static_cast<uint32_t>(
                    record.currentTriangleOffset);
        dispatch.triangleCount =
            static_cast<uint32_t>(record.triangleCount);
        if ((hasPreviousPositionRange ||
                hasGpuPreviousJointPose) &&
            record.gpuPreviousPositionOffset >= 0)
        {
            dispatch.flags |= PT_SKINNED_DISPATCH_HAS_VALID_PREVIOUS;
        }
        CopySmokeObjectToWorldRows(dispatch.currentObjectToWorld, record.objectToWorld);
        CopySmokeObjectToWorldRows(dispatch.previousObjectToWorld, previousRecord ? previousRecord->objectToWorld : record.objectToWorld);
        if (buildGpuSkinningInputs)
        {
            int currentJointOffset = -1;
            if (AppendSmokeSkinnedJointMatrices(
                    SmokeSkinnedRecordJoints(record),
                    record.jointCount,
                    build.currentJointMatrices,
                    currentJointOffset))
            {
                dispatch.currentJointOffset = static_cast<uint32_t>(currentJointOffset);
                dispatch.flags |= PT_SKINNED_DISPATCH_HAS_CURRENT_JOINTS;
            }
            else
            {
                build.recordResults[recordIndex] = Result::NotReadyJointData;
            }
            if ((hasPreviousPositionRange ||
                    hasGpuPreviousJointPose) &&
                previousRecord &&
                previousRecord->jointCount == record.jointCount &&
                SmokeSkinnedJointRangeValid(*previousRecord, previousSkinnedJointMatrices))
            {
                dispatch.previousJointOffset = static_cast<uint32_t>(build.previousJointMatrices.size());
                build.previousJointMatrices.insert(
                    build.previousJointMatrices.end(),
                    previousSkinnedJointMatrices.begin() + previousRecord->retainedJointOffset,
                    previousSkinnedJointMatrices.begin() + previousRecord->retainedJointOffset + previousRecord->jointCount);
                dispatch.flags |= PT_SKINNED_DISPATCH_HAS_PREVIOUS_JOINTS;
            }
        }
        build.dispatchRecords.push_back(dispatch);
        if (canonicalSourceOutputRoute)
        {
            ++build.canonicalDispatches;
        }
    }

    return build;
}

struct RtSmokeSkinnedMaterialStateAudit
{
    uint64 dispatches = 0;
    uint64 triangles = 0;
    uint64 tableRows = 0;
    uint64 dynamicRecords = 0;
    uint64 texMatrixDispatches = 0;
    uint64 texMatrixVertices = 0;
    uint64 invalidTriangleRange = 0;
    uint64 mixedMaterialDispatches = 0;
    uint64 invalidMaterialIndex = 0;
    uint64 materialIdMismatch = 0;
    uint64 incompleteTableRow = 0;
    uint64 dynamicRecordMismatch = 0;
    uint64 routedUnsafeDiffuse = 0;
    uint64 routedUnsafeAlpha = 0;
    uint64 routedUnsafeNormal = 0;
    uint64 routedUnsafeSpecular = 0;
    uint64 routedUnsafeEmissive = 0;

    bool Accepted() const
    {
        return dispatches > 0 &&
            triangles > 0 &&
            invalidTriangleRange == 0 &&
            mixedMaterialDispatches == 0 &&
            invalidMaterialIndex == 0 &&
            materialIdMismatch == 0 &&
            incompleteTableRow == 0 &&
            dynamicRecordMismatch == 0;
    }
};

RtSmokeSkinnedMaterialStateAudit ApplySmokeSkinnedMaterialStateToDispatches(
    RtSmokeSkinnedGpuScaffoldBuild& scaffold,
    const std::vector<RtSmokeSkinnedSurfaceRecord>& surfaceRecords,
    const std::vector<uint32_t>& triangleMaterialIds,
    const std::vector<uint32_t>& triangleMaterialIndexes,
    const RtSmokeMaterialTableBuild& table,
    const std::vector<PathTraceDynamicMaterialRecord>& records)
{
    RtSmokeSkinnedMaterialStateAudit stats;
    std::unordered_set<uint32_t> auditedTableRows;
    for (PathTraceSkinnedSurfaceDispatchRecord& dispatch :
        scaffold.dispatchRecords)
    {
        dispatch.flags &= ~PT_SKINNED_DISPATCH_HAS_TEX_MATRIX;
        dispatch.texMatrix0[0] = 1.0f;
        dispatch.texMatrix0[1] = 0.0f;
        dispatch.texMatrix0[2] = 0.0f;
        dispatch.texMatrix0[3] = 0.0f;
        dispatch.texMatrix1[0] = 0.0f;
        dispatch.texMatrix1[1] = 1.0f;
        dispatch.texMatrix1[2] = 0.0f;
        dispatch.texMatrix1[3] = 0.0f;
        ++stats.dispatches;

        if (dispatch.surfaceRecordIndex >=
                surfaceRecords.size() ||
            dispatch.triangleCount == 0)
        {
            ++stats.invalidTriangleRange;
            continue;
        }
        const RtSmokeSkinnedSurfaceRecord& surfaceRecord =
            surfaceRecords[dispatch.surfaceRecordIndex];
        const uint32_t materialId =
            surfaceRecord.materialId;
        const int resolvedMaterialIndex =
            FindSmokeMaterialTableIndexById(
                table,
                materialId);
        if (resolvedMaterialIndex < 0)
        {
            ++stats.invalidMaterialIndex;
            continue;
        }
        const uint32_t materialIndex =
            static_cast<uint32_t>(resolvedMaterialIndex);
        const uint64 triangleOffset =
            dispatch.dynamicTriangleOffset;
        const uint64 triangleCount =
            dispatch.triangleCount;
        stats.triangles += triangleCount;
        if (!surfaceRecord.cpuCaptureOmitted)
        {
            if (triangleOffset == UINT32_MAX ||
                triangleOffset >
                    triangleMaterialIds.size() ||
                triangleCount >
                    triangleMaterialIds.size() -
                        triangleOffset ||
                triangleOffset >
                    triangleMaterialIndexes.size() ||
                triangleCount >
                    triangleMaterialIndexes.size() -
                        triangleOffset)
            {
                ++stats.invalidTriangleRange;
                continue;
            }
            bool uniformMaterial = true;
            for (uint64 triangle = 0;
                triangle < triangleCount;
                ++triangle)
            {
                const size_t triangleIndex =
                    static_cast<size_t>(
                        triangleOffset + triangle);
                if (triangleMaterialIds[triangleIndex] !=
                        materialId ||
                    triangleMaterialIndexes[triangleIndex] !=
                        materialIndex)
                {
                    uniformMaterial = false;
                    break;
                }
            }
            if (!uniformMaterial)
            {
                ++stats.mixedMaterialDispatches;
                continue;
            }
        }
        if (materialIndex >= table.materialIds.size() ||
            materialIndex >= table.materials.size() ||
            materialIndex >= table.materialInfos.size())
        {
            ++stats.invalidMaterialIndex;
            continue;
        }
        if (table.materialIds[materialIndex] != materialId)
        {
            ++stats.materialIdMismatch;
            continue;
        }
        if (materialIndex >= table.materialFacts.size() ||
            materialIndex >= table.materialFeatures.size() ||
            materialIndex >= table.materialFeatureParameters.size())
        {
            ++stats.incompleteTableRow;
            continue;
        }

        if (auditedTableRows.insert(materialIndex).second)
        {
            ++stats.tableRows;
            const RtSmokeMaterialTextureInfo& info =
                table.materialInfos[materialIndex];
            stats.routedUnsafeDiffuse +=
                info.diffuseImage && !info.hasSafeTexture ? 1u : 0u;
            stats.routedUnsafeAlpha +=
                info.alphaImage && !info.hasSafeAlphaTexture ? 1u : 0u;
            stats.routedUnsafeNormal +=
                info.normalImage && !info.hasSafeNormalTexture ? 1u : 0u;
            stats.routedUnsafeSpecular +=
                info.specularImage && !info.hasSafeSpecularTexture ? 1u : 0u;
            stats.routedUnsafeEmissive +=
                info.emissiveImage && !info.hasSafeEmissiveTexture ? 1u : 0u;
        }

        if (materialIndex < records.size() &&
            (records[materialIndex].flags &
                RT_SMOKE_DYNAMIC_MATERIAL_RECORD_VALID) != 0u)
        {
            const PathTraceDynamicMaterialRecord& record =
                records[materialIndex];
            if (record.materialIndex != materialIndex ||
                record.materialId != materialId)
            {
                ++stats.dynamicRecordMismatch;
                continue;
            }
            ++stats.dynamicRecords;
        }
        if (!SmokeDynamicMaterialRecordHasTexMatrix(
                records,
                materialIndex))
        {
            continue;
        }

        const PathTraceDynamicMaterialRecord& record =
            records[materialIndex];
        dispatch.flags |= PT_SKINNED_DISPATCH_HAS_TEX_MATRIX;
        memcpy(
            dispatch.texMatrix0,
            record.texMatrix0,
            sizeof(dispatch.texMatrix0));
        memcpy(
            dispatch.texMatrix1,
            record.texMatrix1,
            sizeof(dispatch.texMatrix1));
        ++stats.texMatrixDispatches;

        const uint64 outputOffset = dispatch.outputVertexOffset;
        const uint64 vertexCount = dispatch.vertexCount;
        if (outputOffset == UINT32_MAX ||
            outputOffset > scaffold.currentOutputVertices.size() ||
            vertexCount >
                scaffold.currentOutputVertices.size() - outputOffset)
        {
            ++stats.invalidTriangleRange;
            dispatch.flags &= ~PT_SKINNED_DISPATCH_HAS_TEX_MATRIX;
            continue;
        }
        for (uint64 vertex = 0; vertex < vertexCount; ++vertex)
        {
            PathTraceSmokeVertex& output =
                scaffold.currentOutputVertices[
                    static_cast<size_t>(outputOffset + vertex)];
            const idVec2 transformed =
                SmokeApplyDynamicMaterialTexMatrix(
                    record,
                    idVec2(
                        output.texCoord[0],
                        output.texCoord[1]));
            output.texCoord[0] = transformed.x;
            output.texCoord[1] = transformed.y;
            ++stats.texMatrixVertices;
        }
    }
    return stats;
}

RtSmokeJointCacheStageBuild BuildSmokeJointCacheStage(
    bool requested,
    const std::vector<RtSmokeSkinnedSurfaceRecord>& records,
    RtSmokeSkinnedGpuScaffoldBuild& scaffold)
{
    RtSmokeJointCacheStageBuild stage;
    stage.requested = requested;
    if (!requested)
    {
        return stage;
    }

    uint64 capacityBytes = 0;
    if (!PtCheckedMulU64(
            static_cast<uint64>(scaffold.currentJointMatrices.size()),
            PT_JOINT_CACHE_MATRIX_BYTES,
            capacityBytes) ||
        PtInitializeJointCacheCopyPlanner(
            stage.planner,
            capacityBytes) != PtJointCacheCopyPlanResult::PlannedCopy)
    {
        stage.rejected =
            static_cast<int>(scaffold.dispatchRecords.size());
        return stage;
    }

    stage.dispatchResults.assign(
        scaffold.dispatchRecords.size(),
        PtJointCacheCopyPlanResult::InvalidState);
    stage.dispatchPlans.resize(scaffold.dispatchRecords.size());
    for (size_t dispatchIndex = 0;
        dispatchIndex < scaffold.dispatchRecords.size();
        ++dispatchIndex)
    {
        const PathTraceSkinnedSurfaceDispatchRecord& dispatch =
            scaffold.dispatchRecords[dispatchIndex];
        if ((dispatch.flags & PT_SKINNED_DISPATCH_HAS_CURRENT_JOINTS) == 0u ||
            dispatch.surfaceRecordIndex >= records.size())
        {
            ++stage.rejected;
            continue;
        }

        const RtSmokeSkinnedSurfaceRecord& record =
            records[dispatch.surfaceRecordIndex];
        idUniformBuffer jointRange;
        if (record.jointCacheHandle == 0 ||
            !vertexCache.GetJointBuffer(
                static_cast<vertCacheHandle_t>(
                    record.jointCacheHandle),
                &jointRange))
        {
            ++stage.stale;
            ++stage.rejected;
            continue;
        }
        ++stage.resolved;

        nvrhi::IBuffer* sourceBuffer = jointRange.GetAPIObject();
        PtJointCacheCopyRequest request;
        request.instance = record.canonicalInstance;
        request.sourceBufferIdentity = static_cast<uint64>(
            reinterpret_cast<std::uintptr_t>(sourceBuffer));
        request.sourceBufferBytes =
            sourceBuffer
                ? static_cast<uint64>(
                    sourceBuffer->getDesc().byteSize)
                : 0;
        request.sourceOffsetBytes =
            static_cast<uint64>(jointRange.GetOffset());
        request.sourceRangeBytes =
            static_cast<uint64>(jointRange.GetSize());
        request.jointCount =
            record.jointCount > 0
                ? static_cast<uint64>(record.jointCount)
                : 0;

        PtJointCacheCopyPlan& plan =
            stage.dispatchPlans[dispatchIndex];
        const PtJointCacheCopyPlanResult result =
            PtPlanJointCacheCopy(
                stage.planner,
                request,
                plan);
        stage.dispatchResults[dispatchIndex] = result;
        if (result != PtJointCacheCopyPlanResult::PlannedCopy &&
            result != PtJointCacheCopyPlanResult::ReusedCopy)
        {
            ++stage.rejected;
            continue;
        }

        const uint64 destinationMatrixOffset =
            plan.destinationOffsetBytes /
                PT_JOINT_CACHE_MATRIX_BYTES;
        if (plan.destinationOffsetBytes %
                PT_JOINT_CACHE_MATRIX_BYTES != 0 ||
            destinationMatrixOffset > UINT32_MAX)
        {
            stage.dispatchResults[dispatchIndex] =
                PtJointCacheCopyPlanResult::DestinationMisaligned;
            ++stage.rejected;
            continue;
        }

        ++stage.accepted;
        if (result == PtJointCacheCopyPlanResult::PlannedCopy)
        {
            RtSmokeJointCacheStageCopy copy;
            copy.sourceBuffer = sourceBuffer;
            copy.sourceOffsetBytes = plan.sourceOffsetBytes;
            copy.destinationOffsetBytes =
                plan.destinationOffsetBytes;
            copy.byteCount = plan.byteCount;
            copy.dispatchIndex = dispatchIndex;
            stage.copies.push_back(copy);
        }
    }

    stage.ready =
        !stage.dispatchResults.empty() &&
        stage.accepted ==
            static_cast<int>(stage.dispatchResults.size()) &&
        stage.rejected == 0 &&
        !stage.copies.empty() &&
        stage.planner.usedBytes != 0;
    if (stage.ready)
    {
        for (size_t dispatchIndex = 0;
            dispatchIndex < scaffold.dispatchRecords.size();
            ++dispatchIndex)
        {
            scaffold.dispatchRecords[dispatchIndex].currentJointOffset =
                static_cast<uint32_t>(
                    stage.dispatchPlans[dispatchIndex].
                        destinationOffsetBytes /
                    PT_JOINT_CACHE_MATRIX_BYTES);
        }
    }
    return stage;
}

bool SubmitSmokeJointCacheStageCopies(
    nvrhi::ICommandList* commandList,
    nvrhi::BufferHandle destinationBuffer,
    RtSmokeJointCacheStageBuild& stage)
{
    stage.submitted = false;
    stage.submittedBytes = 0;
    if (!commandList ||
        !destinationBuffer ||
        !stage.ready ||
        stage.copies.empty())
    {
        return false;
    }

    const uint64 destinationBufferBytes =
        static_cast<uint64>(
            destinationBuffer->getDesc().byteSize);
    if (stage.planner.usedBytes > destinationBufferBytes)
    {
        return false;
    }
    for (const RtSmokeJointCacheStageCopy& copy : stage.copies)
    {
        if (!copy.sourceBuffer ||
            copy.byteCount == 0 ||
            copy.destinationOffsetBytes >
                destinationBufferBytes ||
            copy.byteCount >
                destinationBufferBytes -
                    copy.destinationOffsetBytes)
        {
            return false;
        }
        const uint64 sourceBufferBytes =
            static_cast<uint64>(
                copy.sourceBuffer->getDesc().byteSize);
        if (copy.sourceOffsetBytes > sourceBufferBytes ||
            copy.byteCount >
                sourceBufferBytes - copy.sourceOffsetBytes)
        {
            return false;
        }
    }

    for (const RtSmokeJointCacheStageCopy& copy : stage.copies)
    {
        commandList->copyBuffer(
            destinationBuffer,
            copy.destinationOffsetBytes,
            copy.sourceBuffer,
            copy.sourceOffsetBytes,
            copy.byteCount);
        stage.submittedBytes += copy.byteCount;
    }
    commandList->setBufferState(
        destinationBuffer,
        nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();
    stage.submitted =
        stage.submittedBytes == stage.planner.usedBytes;
    return stage.submitted;
}

int SmokeSkinnedGpuComputeVertexCount(const std::vector<PathTraceSkinnedSurfaceDispatchRecord>& dispatchRecords)
{
    int vertexCount = 0;
    for (const PathTraceSkinnedSurfaceDispatchRecord& dispatch : dispatchRecords)
    {
        if ((dispatch.flags & PT_SKINNED_DISPATCH_HAS_CURRENT_JOINTS) != 0)
        {
            vertexCount += static_cast<int>(dispatch.vertexCount);
        }
    }
    return vertexCount;
}

int SmokeSkinnedGpuComputeMaxVertexCount(const std::vector<PathTraceSkinnedSurfaceDispatchRecord>& dispatchRecords)
{
    uint32_t maxVertexCount = 0;
    for (const PathTraceSkinnedSurfaceDispatchRecord& dispatch : dispatchRecords)
    {
        if ((dispatch.flags & PT_SKINNED_DISPATCH_HAS_CURRENT_JOINTS) != 0)
        {
            maxVertexCount = Max(maxVertexCount, dispatch.vertexCount);
        }
    }
    return static_cast<int>(maxVertexCount);
}

void ApplySmokeRoutedScenePreset(int debugMode, int requestedPreset, const char* label)
{
    const int preset = idMath::ClampInt(0, 4, requestedPreset);
    if (debugMode != 18)
    {
        return;
    }

    r_pathTracingDebugMode.SetInteger(debugMode);
    r_pathTracingSceneSource.SetInteger(3);
    r_pathTracingRigidBlasGpuScaffold.SetInteger(1);
    r_pathTracingRigidBlasGpuBuild.SetInteger(1);
    r_pathTracingRigidTlasRoute.SetInteger(1);
    r_pathTracingRigidRouteMode18.SetInteger(1);
    r_pathTracingRigidRouteRemoveDynamic.SetInteger(1);
    r_pathTracingRigidRouteEmissiveCards.SetInteger(1);
    r_pathTracingRigidResidency.SetInteger(1);
    r_pathTracingGeometryResidencyV2.SetInteger(1);
    r_pathTracingEntityFeed.SetInteger(1);
    r_pathTracingStaticAreaPreload.SetInteger(1);

    const int portalSteps = 4;
    r_pathTracingRigidResidencyPortalSteps.SetInteger(portalSteps);
    r_pathTracingStaticAreaPreloadPortalSteps.SetInteger(portalSteps);
    r_pathTracingLightAreaPortalSteps.SetInteger(portalSteps);

    const int presetRigidRouteMaxInstances = 510;
    if (r_pathTracingRigidRouteMaxInstances.GetInteger() < presetRigidRouteMaxInstances)
    {
        r_pathTracingRigidRouteMaxInstances.SetInteger(presetRigidRouteMaxInstances);
    }

    common->Printf("PathTracePrimaryPass: applied %s preset %d source3=1 rigidRoute=1 rigidResidency=1 residencyV2=1 entityFeed=1 staticPreload=1 rigidEmissiveCards=1 portalSteps=%d bvhValidation=%d rigidRouteMax=%d tlasMax=512\n",
        label ? label : "mode test",
        preset,
        portalSteps,
        preset == 4 ? 1 : 0,
        r_pathTracingRigidRouteMaxInstances.GetInteger());
}

nvrhi::ObjectType GetPathTraceCommandObjectType()
{
    if (deviceManager && deviceManager->GetGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
    {
        return nvrhi::ObjectTypes::VK_CommandBuffer;
    }
    return nvrhi::ObjectTypes::D3D12_GraphicsCommandList;
}

RtSmokeStaticDrawSurfCounts CountCurrentStaticDrawSurfs(const viewDef_t* viewDef)
{
    RtSmokeStaticDrawSurfCounts counts;
    if (!viewDef || !viewDef->drawSurfs)
    {
        return counts;
    }

    for (int surfaceIndex = 0; surfaceIndex < viewDef->numDrawSurfs; ++surfaceIndex)
    {
        const drawSurf_t* drawSurf = viewDef->drawSurfs[surfaceIndex];
        const srfTriangles_t* tri = nullptr;
        if (!ValidateSmokeDrawSurface(viewDef, drawSurf, tri, nullptr))
        {
            continue;
        }
        if (ClassifySmokeSurface(viewDef, drawSurf, tri) != RtSmokeSurfaceClass::StaticWorld)
        {
            continue;
        }
        ++counts.surfaces;
        counts.triangles += tri->numIndexes / 3;
    }
    return counts;
}

void AppendRigidResidencyBoundsOverlayLines(
    const std::vector<RtPathTraceRigidResidencyBoundsBox>& boxes,
    std::vector<RtPathTraceBoundsOverlayLine>& lines)
{
    static const int edgeStarts[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3 };
    static const int edgeEnds[12] = { 1, 2, 3, 0, 5, 6, 7, 4, 4, 5, 6, 7 };

    for (const RtPathTraceRigidResidencyBoundsBox& box : boxes)
    {
        if (!box.valid || lines.size() + 12 > RT_PT_BOUNDS_OVERLAY_MAX_LINES)
        {
            continue;
        }

        for (int edgeIndex = 0; edgeIndex < 12; ++edgeIndex)
        {
            RtPathTraceBoundsOverlayLine line;
            line.startAndPad = idVec4(box.corners[edgeStarts[edgeIndex]].x, box.corners[edgeStarts[edgeIndex]].y, box.corners[edgeStarts[edgeIndex]].z, 0.0f);
            line.endAndPad = idVec4(box.corners[edgeEnds[edgeIndex]].x, box.corners[edgeEnds[edgeIndex]].y, box.corners[edgeEnds[edgeIndex]].z, 0.0f);
            line.color = box.color;
            lines.push_back(line);
        }
    }
}

std::vector<uint32_t> BuildSortedUniqueMaterialIds(const std::vector<uint32_t>& materialIds)
{
    std::vector<uint32_t> uniqueIds = materialIds;
    std::sort(uniqueIds.begin(), uniqueIds.end());
    uniqueIds.erase(std::unique(uniqueIds.begin(), uniqueIds.end()), uniqueIds.end());
    if (!uniqueIds.empty() && uniqueIds[0] == 0u)
    {
        uniqueIds.erase(uniqueIds.begin());
    }
    return uniqueIds;
}

uint64 BuildSortedUniqueMaterialIdSignature(const std::vector<uint32_t>& materialIds)
{
    if (materialIds.empty())
    {
        return 0ull;
    }
    const std::vector<uint32_t> uniqueIds = BuildSortedUniqueMaterialIds(materialIds);
    return uniqueIds.empty() ? 0ull : BuildSmokeVectorUploadSignature(uniqueIds);
}

std::vector<uint32_t> BuildUniqueMaterialIdsPreservingOrder(const std::vector<uint32_t>& materialIds)
{
    std::vector<uint32_t> uniqueIds;
    uniqueIds.reserve(Min(static_cast<int>(materialIds.size()), 4096));

    std::unordered_set<uint32_t> visited;
    visited.reserve(uniqueIds.capacity());
    for (uint32_t materialId : materialIds)
    {
        if (materialId == 0u || !visited.insert(materialId).second)
        {
            continue;
        }
        uniqueIds.push_back(materialId);
    }
    return uniqueIds;
}

struct RtSmokeStaticBucketFramePublication
{
    bool enabled = false;
    bool auditRequested = false;
    bool auditReady = false;
    bool blasEnabled = false;
    bool portalMaskValid = false;
    bool activeMaskValid = false;
    bool activeMaskForcedFullResident = false;
    bool reflectionPortalHalo = false;
    bool materialIndexUploaded = false;
    int portalAreaCount = 0;
    int frontendVisibleAreaCount = 0;
    int selectedPortalAreaCount = 0;
    int portalSteps = 0;
    int maxVerticesPerBucket = 0;
    int maxIndexesPerBucket = 0;
    int maxTrianglesPerBucket = 0;
    int missingActiveMaterialIndexes = 0;
    uint64 sourceGeneration = 0;
    uint64 materialBindingSignature = 0;
    uint64 totalCpuMicroseconds = 0;
    uint64 cpuMicrosecondsWithoutValidation = 0;
    uint64 sourceBuildMicroseconds = 0;
    uint64 portalMaskMicroseconds = 0;
    uint64 universeStatsMicroseconds = 0;
    uint64 validationMicroseconds = 0;
    uint64 assignmentMicroseconds = 0;
    uint64 residentPackMicroseconds = 0;
    uint64 materialIndexMicroseconds = 0;
    uint64 blasScaffoldMicroseconds = 0;
    uint64 publicationMicroseconds = 0;
    uint64 materialUploadMicroseconds = 0;
    bool residentPackCacheHit = false;
    bool materialIndexCacheHit = false;
    bool assignmentPlanCacheHit = false;
    RtPathTraceSceneUniverseBuildStats sourceBuildStats;
    RtSmokeGeometryUniverseStats universeStats;
    RtSmokeStaticBucketAssignmentPlan assignmentPlan;
    const RtSmokeStaticBucketGeometryPack* geometryPack = nullptr;
    const std::vector<uint32_t>* materialIndexes = nullptr;
    RtPathTraceStaticBucketBlasGpuStats gpuStats;
    RtSmokeStaticBucketWorkPlan shadowWorkPlan;
    RtPathTraceStaticBucketActivePublication activePublication;
    RtPathTraceStaticBucketActivePublication portalActivePublication;
    std::vector<bool> portalActiveAreas;
};

RtSmokeStaticBucketFramePublication BuildSmokeStaticBucketFramePublication(
    const viewDef_t* viewDef,
    RtPathTraceSceneUniverse& sceneUniverse,
    RtSmokeGeometryUniverse& staticBucketGeometryUniverse,
    const std::vector<uint32_t>& materialIds,
    nvrhi::IDevice* device,
    nvrhi::ICommandList* commandList,
    uint64 frameIndex,
    ID_TIME_T mapTimeStamp,
    int portalSteps,
    bool reflectionPortalHalo,
    bool forceFullResidentActiveSet)
{
    OPTICK_EVENT("PT Static Bucket Frame Publication");

    using StaticBucketClock = std::chrono::steady_clock;
    const auto totalStart = StaticBucketClock::now();
    const auto elapsedMicroseconds =
        [](const StaticBucketClock::time_point& start,
            const StaticBucketClock::time_point& end) -> uint64
    {
        return static_cast<uint64>(
            std::chrono::duration_cast<
                std::chrono::microseconds>(end - start).count());
    };

    RtSmokeStaticBucketFramePublication frame;
    frame.auditRequested =
        r_pathTracingGeometryStaticBucketAudit.GetInteger() != 0;
    frame.blasEnabled =
        r_pathTracingGeometryStaticBucketBlas.GetInteger() != 0;
    if (frame.auditRequested && !frame.blasEnabled)
    {
        // The audit validates a ready BLAS-backed publication. Running its
        // source/material publication while BLAS ownership is disabled can
        // never satisfy that contract. More importantly, the disabled BLAS
        // update retires the material-index buffer before the later audit
        // upload recreates it, producing an unbounded create/retire loop.
        // Consume the one-shot request without performing any GPU mutation.
        if (!staticBucketGeometryUniverse.
                StaticSurfaceRecords().empty())
        {
            staticBucketGeometryUniverse.Clear();
        }
        else
        {
            staticBucketGeometryUniverse.
                ReleaseStaticBucketBlasGpuScaffold();
        }
        common->Printf(
            "PathTracePrimaryPass: GEO10 static bucket audit blocked "
            "reason=blas-disabled consumed=1 gpuMutation=0; set "
            "r_pathTracingGeometryStaticBucketBlas 1 before retrying\n");
        r_pathTracingGeometryStaticBucketAudit.SetInteger(0);
        frame.auditRequested = false;
        return frame;
    }
    frame.portalSteps = idMath::ClampInt(0, 8, portalSteps);
    frame.reflectionPortalHalo = reflectionPortalHalo;
    frame.enabled = frame.auditRequested || frame.blasEnabled;
    if (!frame.enabled)
    {
        if (!staticBucketGeometryUniverse.StaticSurfaceRecords().empty())
        {
            staticBucketGeometryUniverse.Clear();
        }
        return frame;
    }

    RtSmokeSurfaceClassStats classStats;
    RtSmokeSurfaceSkipStats skipStats;
    RtSmokeAttributeStats attributeStats;
    RtSmokeMaterialStats materialStats;
    RtSmokeBucketRanges ranges;
    const auto sourceBuildStart = StaticBucketClock::now();
    staticBucketGeometryUniverse.BeginFrame(
        frameIndex,
        viewDef ? viewDef->renderWorld : nullptr,
        false);
    frame.sourceBuildStats = sceneUniverse.BuildFullStaticBucketGeometry(
        viewDef,
        staticBucketGeometryUniverse,
        classStats,
        skipStats,
        attributeStats,
        materialStats,
        ranges);
    staticBucketGeometryUniverse.EndFrame();
    if (frame.sourceBuildStats.built &&
        !frame.sourceBuildStats.cacheHit &&
        staticBucketGeometryUniverse.PruneMissingStaticSurfaces())
    {
        // A genuine source replacement (map/world topology change) is built
        // into the persistent universe before its old records can be retired.
        // Compact immediately after the complete enumeration so duplicate or
        // stale records cannot poison the exact assignment/publication path.
        common->Printf(
            "PathTracePrimaryPass: GEO10 static bucket source replacement "
            "pruned stale resident records frame=%llu sourceGeneration=%llu\n",
            static_cast<unsigned long long>(frameIndex),
            static_cast<unsigned long long>(
                sceneUniverse.GetStats().generation));
    }
    frame.sourceBuildMicroseconds = elapsedMicroseconds(
        sourceBuildStart,
        StaticBucketClock::now());

    const auto portalMaskStart = StaticBucketClock::now();
    frame.portalAreaCount =
        viewDef && viewDef->renderWorld
            ? viewDef->renderWorld->NumAreas()
            : 0;
    frame.portalMaskValid = sceneUniverse.BuildPortalAreaActiveMask(
        viewDef,
        frame.portalSteps,
        frame.portalActiveAreas,
        &frame.frontendVisibleAreaCount,
        &frame.selectedPortalAreaCount);
    std::vector<bool> activeAreas =
        frame.portalActiveAreas;
    frame.activeMaskValid = frame.portalMaskValid;
    // Route mode 2 is a decoder-only diagnostic. Keep every resident bucket
    // addressable so portal-neighborhood policy cannot masquerade as a shader
    // decode failure. Production route modes continue to use the portal mask.
    if (forceFullResidentActiveSet && frame.portalAreaCount > 0)
    {
        activeAreas.assign(frame.portalAreaCount, true);
        frame.activeMaskValid = true;
        frame.activeMaskForcedFullResident = true;
    }
    frame.portalMaskMicroseconds = elapsedMicroseconds(
        portalMaskStart,
        StaticBucketClock::now());
    frame.maxVerticesPerBucket = Max(
        1,
        r_pathTracingGeometryStaticBucketMaxVertices.GetInteger());
    frame.maxIndexesPerBucket = Max(
        3,
        r_pathTracingGeometryStaticBucketMaxIndexes.GetInteger());
    frame.maxTrianglesPerBucket = Max(
        1,
        r_pathTracingGeometryStaticBucketMaxTriangles.GetInteger());
    frame.sourceGeneration = sceneUniverse.GetStats().generation;
    const auto universeStatsStart =
        StaticBucketClock::now();
    frame.universeStats =
        staticBucketGeometryUniverse.GetStats(false);
    frame.universeStatsMicroseconds = elapsedMicroseconds(
        universeStatsStart,
        StaticBucketClock::now());
    if (frame.auditRequested)
    {
        const auto validationStart =
            StaticBucketClock::now();
        frame.universeStats =
            staticBucketGeometryUniverse.GetStats(true);
        frame.validationMicroseconds = elapsedMicroseconds(
            validationStart,
            StaticBucketClock::now());
    }
    const auto assignmentStart = StaticBucketClock::now();
    frame.assignmentPlan =
        staticBucketGeometryUniverse.BuildStaticBucketAssignmentPlan(
            static_cast<uint64>(mapTimeStamp),
            frame.sourceGeneration,
            frame.portalAreaCount,
            frame.maxVerticesPerBucket,
            frame.maxIndexesPerBucket,
            frame.maxTrianglesPerBucket,
            frame.activeMaskValid ? &activeAreas : nullptr,
            &frame.assignmentPlanCacheHit);
    frame.assignmentMicroseconds = elapsedMicroseconds(
        assignmentStart,
        StaticBucketClock::now());
    const auto residentPackStart = StaticBucketClock::now();
    frame.geometryPack =
        &staticBucketGeometryUniverse.
            GetOrBuildStaticBucketResidentGeometryPack(
                frame.assignmentPlan,
                frame.residentPackCacheHit);
    frame.residentPackMicroseconds = elapsedMicroseconds(
        residentPackStart,
        StaticBucketClock::now());
    const RtSmokeStaticBucketGeometryPack& geometryPack =
        *frame.geometryPack;
    const std::vector<int>*
        missingMaterialIndexesByBucket = nullptr;
    const auto materialIndexStart = StaticBucketClock::now();
    if (!staticBucketGeometryUniverse.
            GetOrBuildStaticBucketMaterialIndexes(
                geometryPack,
                materialIds,
                frame.materialIndexCacheHit,
                frame.materialBindingSignature,
                frame.materialIndexes,
                missingMaterialIndexesByBucket) ||
        frame.materialIndexes == nullptr ||
        missingMaterialIndexesByBucket == nullptr ||
        missingMaterialIndexesByBucket->size() !=
            geometryPack.buckets.size())
    {
        return frame;
    }
    frame.materialIndexMicroseconds = elapsedMicroseconds(
        materialIndexStart,
        StaticBucketClock::now());

    for (size_t bucketIndex = 0;
         bucketIndex < geometryPack.buckets.size();
         ++bucketIndex)
    {
        const RtSmokeStaticBucketPackedRecord& bucket =
            geometryPack.buckets[bucketIndex];
        if (!bucket.active ||
            bucket.range.triangleOffset < 0 ||
            bucket.range.triangleCount <= 0)
        {
            continue;
        }
        frame.missingActiveMaterialIndexes +=
            (*missingMaterialIndexesByBucket)[bucketIndex];
    }

    const bool submitBuilds =
        frame.blasEnabled &&
        r_pathTracingGeometryStaticBucketBlasBuild.GetInteger() != 0;
    const auto blasScaffoldStart = StaticBucketClock::now();
    frame.gpuStats =
        staticBucketGeometryUniverse.UpdateStaticBucketBlasGpuScaffold(
            device,
            commandList,
            geometryPack,
            frame.blasEnabled,
            submitBuilds,
            idMath::ClampInt(
                0,
                1024,
                r_pathTracingGeometryStaticBucketBlasBuildLimit.GetInteger()),
            static_cast<uint64>(
                idMath::ClampInt(
                    0,
                    1048576,
                    r_pathTracingGeometryStaticBucketBlasResultBudgetKB.
                        GetInteger())) *
                1024ull,
            r_pathTracingGeometryStaticBucketBlasForceRebuild.GetInteger() != 0,
            frame.auditRequested);
    frame.blasScaffoldMicroseconds = elapsedMicroseconds(
        blasScaffoldStart,
        StaticBucketClock::now());

    const auto publicationStart = StaticBucketClock::now();
    std::vector<RtSmokeStaticTlasBucketObservation> tlasObservations;
    staticBucketGeometryUniverse.BuildStaticBucketTlasObservations(
        geometryPack,
        tlasObservations);
    RtSmokeStaticBucketWorkPlanInput workInput;
    workInput.buckets =
        tlasObservations.empty() ? nullptr : tlasObservations.data();
    workInput.bucketCount =
        static_cast<int>(tlasObservations.size());
    workInput.geometryContentSignature =
        geometryPack.contentSignature;
    workInput.materialGeneration =
        frame.universeStats.staticMaterialGeneration;
    workInput.totalVertexCount =
        geometryPack.stats.packedVertices;
    workInput.totalIndexCount =
        geometryPack.stats.packedIndexes;
    workInput.totalTriangleCount =
        geometryPack.stats.packedTriangles;
    workInput.monolithicStaticBlas = false;
    workInput.hasStaticBlas = frame.gpuStats.readyBuckets > 0;
    workInput.enableStaticRoutes = true;
    workInput.shaderSupportsStaticBucketRoutes = false;
    frame.shadowWorkPlan = BuildSmokeStaticBucketWorkPlan(workInput);

    frame.activePublication =
        staticBucketGeometryUniverse.BuildStaticBucketActivePublication(
            geometryPack,
            frame.sourceGeneration,
            frame.universeStats.staticGeometryGeneration,
            frame.universeStats.staticMaterialGeneration,
            frame.missingActiveMaterialIndexes,
            0x01u);
    if (frame.auditRequested &&
        frame.portalMaskValid)
    {
        int portalMissingMaterialIndexes = 0;
        for (size_t bucketIndex = 0;
             bucketIndex < geometryPack.buckets.size();
             ++bucketIndex)
        {
            const RtSmokeStaticBucketPackedRecord& bucket =
                geometryPack.buckets[bucketIndex];
            const bool bucketActive =
                bucket.portalArea ==
                    RT_SMOKE_STATIC_BUCKET_FALLBACK_AREA ||
                (bucket.portalArea >= 0 &&
                    bucket.portalArea <
                        static_cast<int>(
                            frame.portalActiveAreas.size()) &&
                    frame.portalActiveAreas[
                        bucket.portalArea]);
            if (bucketActive)
            {
                portalMissingMaterialIndexes +=
                    (*missingMaterialIndexesByBucket)[
                        bucketIndex];
            }
        }
        frame.portalActivePublication =
            staticBucketGeometryUniverse.
                BuildStaticBucketActivePublication(
                    geometryPack,
                    frame.sourceGeneration,
                    frame.universeStats.
                        staticGeometryGeneration,
                    frame.universeStats.
                        staticMaterialGeneration,
                    portalMissingMaterialIndexes,
                    0x01u,
                    &frame.portalActiveAreas);
    }
    frame.publicationMicroseconds = elapsedMicroseconds(
        publicationStart,
        StaticBucketClock::now());
    const auto materialUploadStart = StaticBucketClock::now();
    frame.materialIndexUploaded =
        staticBucketGeometryUniverse.UpdateStaticBucketMaterialIndexGpuScaffold(
            device,
            commandList,
            geometryPack,
            *frame.materialIndexes,
            frame.materialBindingSignature);
    frame.materialUploadMicroseconds = elapsedMicroseconds(
        materialUploadStart,
        StaticBucketClock::now());
    frame.auditReady =
        IsSmokeStaticBucketAuditReady(
            frame.auditRequested,
            frame.portalActivePublication.valid,
            frame.activeMaskForcedFullResident,
            frame.activePublication.valid);
    frame.totalCpuMicroseconds = elapsedMicroseconds(
        totalStart,
        StaticBucketClock::now());
    frame.cpuMicrosecondsWithoutValidation =
        frame.totalCpuMicroseconds >=
                frame.validationMicroseconds
            ? frame.totalCpuMicroseconds -
                frame.validationMicroseconds
            : frame.totalCpuMicroseconds;
    return frame;
}

}

void PathTracePrimaryPass::BuildRayTracingSmokeTestScene(const viewDef_t* viewDef)
{
    OPTICK_EVENT("PT Build Scene");

    BuildPathTraceParticleCompositeCapture(viewDef, m_particleCapture);

    const int sceneStartMs = Sys_Milliseconds();
    const int mode18Preset = r_pathTracingMode18TestPreset.GetInteger();
    if (mode18Preset != 0)
    {
        ApplySmokeRoutedScenePreset(18, mode18Preset, "mode18 test");
        r_pathTracingMode18TestPreset.SetInteger(0);
    }
    m_smokeSceneBuilt = false;
    m_smokeBoundsOverlayLines.clear();
    m_smokeBoundsOverlayLineCount = 0;
    m_smokeBoundsOverlayViewValid = false;
    const int requestedDebugMode = NormalizePathTraceDebugMode(idMath::ClampInt(0, 58, r_pathTracingDebugMode.GetInteger()));
    const int cleanRtxdiDiSceneBuildView = r_pathTracingCleanRtxdiDiView.GetInteger();
    const int cleanRtxdiDiSceneBuildResolveView =
        (cleanRtxdiDiSceneBuildView >= 18 && cleanRtxdiDiSceneBuildView <= 23) ? 16 : cleanRtxdiDiSceneBuildView;
    const bool cleanRtxdiDiSceneBuildRoute =
        requestedDebugMode == 0 &&
        r_pathTracingCleanRtxdiDiEnable.GetInteger() != 0 &&
        r_pathTracingCleanRtxdiDiLightMode.GetInteger() == 1 &&
        r_pathTracingRemixLightUniverseUseForCleanRtxdiDi.GetInteger() != 0 &&
        (cleanRtxdiDiSceneBuildView == 8 || cleanRtxdiDiSceneBuildView == 12 || cleanRtxdiDiSceneBuildView == 13 || cleanRtxdiDiSceneBuildView == 14 || cleanRtxdiDiSceneBuildView == 15 || cleanRtxdiDiSceneBuildResolveView == 16);
    const bool cleanRtxdiDiMaterialClassifierProofRoute =
        requestedDebugMode == 0 &&
        r_pathTracingCleanRtxdiDiEnable.GetInteger() != 0 &&
        (cleanRtxdiDiSceneBuildView == 12 || cleanRtxdiDiSceneBuildView == 24);
    const bool cleanRtxdiDiPsrMaskRoute =
        requestedDebugMode == 0 &&
        r_pathTracingCleanRtxdiDiEnable.GetInteger() != 0 &&
        cleanRtxdiDiSceneBuildView == 25;
    const int cleanRtxdiDiSceneBuildDomain = idMath::ClampInt(0, 2,
        r_pathTracingRemixLightUniverseEnable.GetInteger() != 0
            ? r_pathTracingRemixLightUniverseDomain.GetInteger()
            : (cleanRtxdiDiSceneBuildRoute ? 2 : r_pathTracingRemixLightUniverseDomain.GetInteger()));
    const bool pdfNeeRluCurrentProducerSceneBuildRequested =
        requestedDebugMode == 0 &&
        r_pathTracingRestirPdfNeeVerifierEnable.GetInteger() != 0;
    const int pdfNeeRluSceneBuildDomain = idMath::ClampInt(0, 2,
        r_pathTracingRemixLightUniverseEnable.GetInteger() != 0
            ? r_pathTracingRemixLightUniverseDomain.GetInteger()
            : 2);
    const bool pdfNeeRluSceneBuildEmissives =
        pdfNeeRluCurrentProducerSceneBuildRequested &&
        (pdfNeeRluSceneBuildDomain == 1 || pdfNeeRluSceneBuildDomain == 2);
    const bool cleanRtxdiDiSceneBuildRluEmissives =
        pdfNeeRluSceneBuildEmissives ||
        (cleanRtxdiDiSceneBuildRoute &&
            (cleanRtxdiDiSceneBuildDomain == 1 || cleanRtxdiDiSceneBuildDomain == 2));
    const int neeCacheSceneBuildSourceDomain = idMath::ClampInt(0, 3, r_pathTracingNeeCacheSourceDomain.GetInteger());
    const bool neeCacheSceneBuildRluEmissives =
        r_pathTracingNeeCacheEnable.GetInteger() != 0 &&
        r_pathTracingNeeCacheMode.GetInteger() != 0 &&
        (neeCacheSceneBuildSourceDomain == 0 || neeCacheSceneBuildSourceDomain == 1 || neeCacheSceneBuildSourceDomain == 3);
    const bool cleanRtxdiDiMaterialValidationRoute =
        requestedDebugMode == 0 &&
        r_pathTracingCleanRtxdiDiEnable.GetInteger() != 0 &&
        (cleanRtxdiDiSceneBuildResolveView == 16 || cleanRtxdiDiMaterialClassifierProofRoute || cleanRtxdiDiPsrMaskRoute);
    const bool enableTextureProbe =
        PathTraceDebugModeNeedsTextureProbe(requestedDebugMode) ||
        cleanRtxdiDiSceneBuildRluEmissives ||
        cleanRtxdiDiMaterialValidationRoute ||
        neeCacheSceneBuildRluEmissives;

    if (!m_smokeTlas || !m_smokeBindingLayout || !m_smokeTextureBindlessLayout || !m_frameResources.outputTexture || !m_frameResources.accumulationTexture || !m_frameResources.rrInputColorTexture || !m_frameResources.motionVectorTexture || !m_frameResources.rrMotionVectorTexture || !m_frameResources.motionVectorMaskTexture || !m_frameResources.rrGuideAlbedoTexture || !m_frameResources.rrGuideSpecularAlbedoTexture || !m_frameResources.rrGuideNormalRoughnessTexture || !m_frameResources.rrGuideDepthTexture || !m_frameResources.rrGuideHitDistanceTexture || !m_frameResources.rrGuideResetMaskTexture || !m_frameResources.rrGuidePositionTexture || !m_smokeConstantsBuffer || !m_smokeBoundsOverlayLineBuffer)
    {
        return;
    }
    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    if (!renderWorld)
    {
        if (m_smokeSceneRenderWorld || m_smokeSceneMapName.Length() > 0)
        {
            common->Printf("PathTracePrimaryPass: PT render world unavailable; clearing scene caches for '%s'\n",
                m_smokeSceneMapName.c_str());
            nvrhi::IDevice* resetDevice = deviceManager ? deviceManager->GetDevice() : nullptr;
            if (resetDevice)
            {
                resetDevice->waitForIdle();
            }
            ResetRayTracingSmokeSceneResources();
            m_smokeGeometryUniverse.ClearRetiredRigidBlas();
            m_frameResources.MarkResetReason(RT_FRAME_RESET_SCENE_RESOURCES);
        }
        return;
    }
    const bool renderWorldChanged = m_smokeSceneRenderWorld != renderWorld;
    const bool mapChanged = m_smokeSceneMapName.Icmp(renderWorld->mapName) != 0 || m_smokeSceneMapTimeStamp != renderWorld->mapTimeStamp;
    const bool mapLoadChanged = m_smokeSceneMapLoadSerial != renderWorld->mapLoadSerial;
    if (renderWorldChanged || mapChanged || mapLoadChanged)
    {
        if (m_smokeSceneRenderWorld || m_smokeSceneMapName.Length() > 0)
        {
            common->Printf("PathTracePrimaryPass: PT render world map changed '%s' -> '%s' serial %llu -> %llu; clearing scene caches\n",
                m_smokeSceneMapName.c_str(),
                renderWorld->mapName.c_str(),
                static_cast<unsigned long long>(m_smokeSceneMapLoadSerial),
                static_cast<unsigned long long>(renderWorld->mapLoadSerial));
        }
        nvrhi::IDevice* resetDevice = deviceManager ? deviceManager->GetDevice() : nullptr;
        if (resetDevice)
        {
            resetDevice->waitForIdle();
        }
        ResetRayTracingSmokeSceneResources();
        m_smokeGeometryUniverse.ClearRetiredRigidBlas();
        m_frameResources.MarkResetReason(RT_FRAME_RESET_SCENE_RESOURCES);
        m_smokeSceneRenderWorld = renderWorld;
        m_smokeSceneMapName = renderWorld->mapName;
        m_smokeSceneMapTimeStamp = renderWorld->mapTimeStamp;
        m_smokeSceneMapLoadSerial = renderWorld->mapLoadSerial;
    }
    nvrhi::IDevice* device = deviceManager ? deviceManager->GetDevice() : nullptr;
    if (!device)
    {
        return;
    }
    if (!m_smokeTextureDescriptorTable)
    {
        m_smokeTextureDescriptorTable = device->createDescriptorTable(m_smokeTextureBindlessLayout);
        if (!m_smokeTextureDescriptorTable)
        {
            common->Printf("PathTracePrimaryPass: failed to recreate RT smoke texture descriptor table after reset\n");
            return;
        }
    }

    if (viewDef)
    {
        for (int matrixElement = 0; matrixElement < 16; ++matrixElement)
        {
            m_smokeBoundsOverlayModelViewMatrix[matrixElement] = viewDef->worldSpace.modelViewMatrix[matrixElement];
            m_smokeBoundsOverlayProjectionMatrix[matrixElement] = viewDef->projectionMatrix[matrixElement];
        }
        m_smokeBoundsOverlayViewValid = true;
    }

    nvrhi::ICommandList* commandList = m_backend ? m_backend->GL_GetCommandList() : nullptr;
    if (!commandList)
    {
        return;
    }
    for (GeometrySkinnedGpuTimerSlot& timer :
        m_geometrySkinnedGpuTimers)
    {
        if (!timer.pending ||
            !timer.query ||
            idLib::frameNumber < timer.earliestPollFrame ||
            !device->pollTimerQuery(timer.query))
        {
            continue;
        }
        const float gpuSeconds =
            device->getTimerQueryTime(timer.query);
        common->Printf(
            "PathTracePrimaryPass: GEO08 timing frame=%llu kind=%s submitted=%d gpuUs=%.1f counts(build/update/rebuild/reuse)=%u/%u/%u/%u skinned(surfaces/sourceIndexes/cpuCapturedIndexes/cpuSkinUs)=%u/%u/%u/%llu dynamicBlasIndexes=%u cpuSubmitUs(blas/tlas/accel)=%llu/%llu/%llu\n",
            static_cast<unsigned long long>(
                timer.geometryFrame),
            timer.skinnedBlas
                ? "per-instance-skinned-blas"
                : "merged-dynamic-blas",
            timer.submitted ? 1 : 0,
            static_cast<double>(gpuSeconds) * 1000000.0,
            timer.buildCount,
            timer.updateCount,
            timer.rebuildCount,
            timer.reuseCount,
            timer.skinnedSurfaceCount,
            timer.skinnedSourceIndexes,
            timer.cpuCapturedSkinnedIndexes,
            static_cast<unsigned long long>(
                timer.cpuSkinUs),
            timer.dynamicBlasIndexes,
            static_cast<unsigned long long>(
                timer.cpuBlasSubmitUs),
            static_cast<unsigned long long>(
                timer.cpuTlasSubmitUs),
            static_cast<unsigned long long>(
                timer.cpuAccelSubmitUs));
        timer.pending = false;
    }
    const bool optickGpuMarkers = r_pathTracingOptickGpuMarkers.GetInteger() != 0;
    if (optickGpuMarkers)
    {
        OPTICK_GPU_CONTEXT((void*)commandList->getNativeObject(GetPathTraceCommandObjectType()));
    }

    std::vector<PathTraceSmokeVertex> dynamicVertexData;
    std::vector<uint32_t> dynamicIndexData;
    std::vector<uint32_t> dynamicTriangleClassData;
    std::vector<uint32_t> dynamicTriangleMaterialData;
    std::vector<uint32_t> dynamicTriangleInstanceData;
    std::vector<uint32_t> dynamicTriangleIdentityData;
    std::vector<RtSmokeSkinnedSurfaceRecord> currentSkinnedSurfaceRecords;
    std::vector<RtSmokeCapturedSurfaceRecord>
        currentCapturedSurfaceRecords;
    uint64 skinnedCaptureViewSignature = 0;
    PtSkinnedHitRouteBuild skinnedHitRouteUploadBuild;
    uint64 skinnedHitRouteUploadBuildSignature = 0;
    const std::vector<PtSkinnedHitRouteRecord>*
        skinnedCaptureAdmissionRoutes = nullptr;
    RtSmokeSkinnedGpuScaffoldBuild skinnedGpuScaffold;
    RtSmokeJointCacheStageBuild jointCacheStage;
    RtSmokeSkinnedOutputAudit skinnedOutputAudit;
    int sourceSurfaces = 0;
    int sourceVerts = 0;
    int sourceIndexes = 0;
    int anchorTriangle = -1;
    RtSmokeSurfaceClassStats classStats;
    RtSmokeSurfaceSkipStats skipStats;
    RtSmokeDynamicGeometryStats dynamicStats;
    RtSmokeAttributeStats attributeStats;
    RtSmokeMaterialStats materialStats;
    RtSmokeBucketRanges bucketRanges;
    bool staticCacheChanged = false;
    RtSmokeSceneCaptureTiming captureTiming;
    std::vector<PathTraceSmokeVertex>& staticVertexCache = m_smokeGeometryUniverse.StaticVertices();
    std::vector<uint32_t>& staticIndexCache = m_smokeGeometryUniverse.StaticIndexes();
    std::vector<uint32_t>& staticTriangleClassCache = m_smokeGeometryUniverse.StaticTriangleClasses();
    std::vector<uint32_t>& staticTriangleMaterialCache = m_smokeGeometryUniverse.StaticTriangleMaterials();
    const std::vector<PathTraceSmokeVertex>& previousStaticVertexCache = m_smokeGeometryUniverse.PreviousStaticVertices();
    const std::vector<uint32_t>& previousStaticIndexCache = m_smokeGeometryUniverse.PreviousStaticIndexes();
    const std::vector<uint32_t>& previousStaticTriangleClassCache = m_smokeGeometryUniverse.PreviousStaticTriangleClasses();
    const std::vector<uint32_t>& previousStaticTriangleMaterialCache = m_smokeGeometryUniverse.PreviousStaticTriangleMaterials();
    const std::vector<uint32_t>& previousStaticTriangleMaterialIndexCache = m_smokePreviousStaticTriangleMaterialIndexes;
    const int captureStartMs = Sys_Milliseconds();
    bool usingDoomSurfaces = false;
    const int sceneSource = idMath::ClampInt(0, 3, r_pathTracingSceneSource.GetInteger());
    const bool useSceneUniverseStaticGeometry = sceneSource == 2;
    const bool useDrawSurfMirrorDynamicFrame = sceneSource == 3;
    const bool routeResidencyV2Mode =
        useDrawSurfMirrorDynamicFrame &&
        r_pathTracingGeometryResidencyV2.GetInteger() != 0 &&
        r_pathTracingRigidResidency.GetInteger() != 0;
    const bool enableRigidRouteForMode =
        useDrawSurfMirrorDynamicFrame &&
        r_pathTracingRigidTlasRoute.GetInteger() != 0 &&
        r_pathTracingRigidBlasGpuScaffold.GetInteger() != 0 &&
        r_pathTracingRigidBlasGpuBuild.GetInteger() != 0 &&
        (routeResidencyV2Mode ||
            PathTraceDebugModeUsesRigidRoute(requestedDebugMode) ||
            cleanRtxdiDiSceneBuildRluEmissives ||
            (requestedDebugMode == 18 && r_pathTracingRigidRouteMode18.GetInteger() != 0));
    const bool rigidResidencyBoundsDebug = IsPathTraceBoundsOverlayDebugMode(requestedDebugMode);
    const bool rigidResidencyEnabled =
        r_pathTracingRigidResidency.GetInteger() != 0 &&
        (enableRigidRouteForMode || rigidResidencyBoundsDebug);
    const int source2RigidEntities = sceneSource == 2 ? idMath::ClampInt(0, 2, r_pathTracingSceneSource2RigidEntities.GetInteger()) : 0;
    const int liquidPoolOffsetEnabled = r_pathTracingLiquidPoolMode.GetInteger() != 0 ? 1 : 0;
    const bool dumpInstanceUniverse = r_pathTracingInstanceUniverseDump.GetInteger() != 0;
    const bool dumpRigidMeshUniverse = r_pathTracingRigidMeshUniverseDump.GetInteger() != 0;
    {
        OPTICK_EVENT("PT EntityFeed Debug Probes");
        DumpEntityFeedSingleBoneDiagnostics(viewDef);
        DumpEntityFeedJointAdvanceProbe(viewDef);
        DumpEntityFeedReachableCandidateStats(viewDef);
    }
    const RtSmokeStaticDrawSurfCounts currentStaticDrawSurfs = useSceneUniverseStaticGeometry ? CountCurrentStaticDrawSurfs(viewDef) : RtSmokeStaticDrawSurfCounts();
    if (sceneSource != m_smokeSceneSourceLast ||
        (useSceneUniverseStaticGeometry && source2RigidEntities != m_smokeSceneSource2RigidEntitiesLast) ||
        liquidPoolOffsetEnabled != m_smokeLiquidPoolOffsetEnabledLast)
    {
        common->Printf("PathTracePrimaryPass: PT static geometry policy changed source=%d/%d->%d/%d liquidOffset=%d->%d; clearing static geometry cache\n",
            m_smokeSceneSourceLast,
            m_smokeSceneSource2RigidEntitiesLast,
            sceneSource,
            source2RigidEntities,
            m_smokeLiquidPoolOffsetEnabledLast,
            liquidPoolOffsetEnabled);
        m_smokeGeometryUniverse.Clear();
        m_smokeSkinnedSurfaceRecords.clear();
        m_smokeSkinnedCaptureRouteSets.clear();
        m_smokeLegacySkinnedHistoryState = RtSmokeSkinnedHistoryState();
        m_smokeSkinnedHistoryStates.clear();
        m_smokeSkinnedHistoryUpdateSerial = 0;
        m_smokePreviousStaticTriangleMaterialIndexes.clear();
        m_smokePreviousStaticSnapshotUploadSignature = 0;
        m_smokePreviousStaticMaterialIndexUploadSignature = 0;
        m_smokeStaticTriangleMaterialUploadSignature = 0;
        m_smokeStaticTriangleMaterialIndexUploadSignature = 0;
        m_smokeStaticTriangleMaterialUploadSignatureValid = false;
        m_smokeStaticTriangleMaterialIndexUploadSignatureValid = false;
        m_smokeStaticBlasCacheValid = false;
        m_smokeStaticBlasSignature = 0;
        m_smokeStaticBlasGeometryGeneration = 0;
        m_smokeSceneUniverseStaticBuildGeneration = 0;
        m_smokeSceneRebuildLogged = false;
        m_smokeSceneSourceLast = sceneSource;
        m_smokeSceneSource2RigidEntitiesLast = source2RigidEntities;
        m_smokeLiquidPoolOffsetEnabledLast = liquidPoolOffsetEnabled;
    }
    uint64 sceneUniverseGeneration = 0;
    if (useSceneUniverseStaticGeometry && m_sceneUniverse.EnsureBuilt(viewDef))
    {
        sceneUniverseGeneration = m_sceneUniverse.GetStats().generation;
        if (m_smokeSceneUniverseStaticBuildGeneration != 0 && m_smokeSceneUniverseStaticBuildGeneration != sceneUniverseGeneration)
        {
            common->Printf("PathTracePrimaryPass: PT scene universe generation changed %llu -> %llu; clearing source-2 static geometry cache\n",
                static_cast<unsigned long long>(m_smokeSceneUniverseStaticBuildGeneration),
                static_cast<unsigned long long>(sceneUniverseGeneration));
            m_smokeGeometryUniverse.Clear();
            m_smokeSkinnedSurfaceRecords.clear();
            m_smokeSkinnedCaptureRouteSets.clear();
            m_smokeLegacySkinnedHistoryState = RtSmokeSkinnedHistoryState();
            m_smokeSkinnedHistoryStates.clear();
            m_smokeSkinnedHistoryUpdateSerial = 0;
            m_smokePreviousStaticTriangleMaterialIndexes.clear();
            m_smokePreviousStaticSnapshotUploadSignature = 0;
            m_smokePreviousStaticMaterialIndexUploadSignature = 0;
            m_smokeStaticTriangleMaterialUploadSignature = 0;
            m_smokeStaticTriangleMaterialIndexUploadSignature = 0;
            m_smokeStaticTriangleMaterialUploadSignatureValid = false;
            m_smokeStaticTriangleMaterialIndexUploadSignatureValid = false;
            m_smokeStaticBlasCacheValid = false;
            m_smokeStaticBlasSignature = 0;
            m_smokeStaticBlasGeometryGeneration = 0;
            m_smokeSceneUniverseStaticBuildGeneration = 0;
            m_smokeSceneRebuildLogged = false;
        }
    }
    if (useSceneUniverseStaticGeometry && source2RigidEntities != 0)
    {
        m_smokeGeometryUniverse.Clear();
        m_smokeSkinnedSurfaceRecords.clear();
        m_smokeSkinnedCaptureRouteSets.clear();
        m_smokeLegacySkinnedHistoryState = RtSmokeSkinnedHistoryState();
        m_smokeSkinnedHistoryStates.clear();
        m_smokeSkinnedHistoryUpdateSerial = 0;
        m_smokePreviousStaticTriangleMaterialIndexes.clear();
        m_smokePreviousStaticSnapshotUploadSignature = 0;
        m_smokePreviousStaticMaterialIndexUploadSignature = 0;
        m_smokeStaticTriangleMaterialUploadSignature = 0;
        m_smokeStaticTriangleMaterialIndexUploadSignature = 0;
        m_smokeStaticTriangleMaterialUploadSignatureValid = false;
        m_smokeStaticTriangleMaterialIndexUploadSignatureValid = false;
        m_smokeStaticBlasCacheValid = false;
        m_smokeStaticBlasSignature = 0;
        m_smokeStaticBlasGeometryGeneration = 0;
        m_smokeSceneUniverseStaticBuildGeneration = 0;
    }
    RtPathTraceSceneUniverseBuildStats sceneUniverseStaticBuildStats;
    bool drawSurfMirrorFrameProducedFromDynamicCapture = false;
    const bool geometrySourceDumpRequested =
        r_pathTracingGeometryShadowRegistryDump.GetInteger() != 0;
    {
        OPTICK_EVENT("PT Capture Doom Surfaces");
        m_smokeGeometryUniverse.BeginFrame(++m_smokeGeometryFrameIndex, viewDef ? viewDef->renderWorld : nullptr);
        m_smokeGeometryUniverse.ImportCanonicalSourceSnapshot(
            viewDef ? viewDef->pathTraceGeometrySourceSnapshot : nullptr);
        m_smokeGeometryUniverse.ImportCanonicalIdentitySnapshot(
            viewDef ? viewDef->pathTraceGeometryIdentitySnapshot : nullptr);
        m_smokeGeometryUniverse.UpdateCanonicalSourceGpuPools(
            device,
            commandList);
        if (useDrawSurfMirrorDynamicFrame)
        {
            skinnedCaptureViewSignature =
                BuildPathTraceSkinnedCaptureViewSignature(
                    viewDef);
            for (SmokeSkinnedCaptureRouteSetState& routeSet :
                m_smokeSkinnedCaptureRouteSets)
            {
                if (routeSet.signature !=
                    skinnedCaptureViewSignature)
                {
                    continue;
                }
                routeSet.lastUsedFrame =
                    m_smokeGeometryFrameIndex;
                const bool acceptedSourceOnly =
                    SmokeSkinnedHitRouteBuildUsesSourceOnlyMetadata(
                        routeSet.acceptedBuild);
                const bool pendingSourceOnly =
                    SmokeSkinnedHitRouteBuildUsesSourceOnlyMetadata(
                        routeSet.pendingBuild);
                const bool pendingAnySourceOnly =
                    SmokeSkinnedHitRouteBuildUsesAnySourceOnlyMetadata(
                        routeSet.pendingBuild);
                const bool pendingContainsAcceptedInstanceSet =
                    SmokeSkinnedHitRouteBuildContainsInstanceSet(
                        routeSet.pendingBuild,
                        routeSet.acceptedBuild);
                const bool pendingAcceptedInstanceSetExact =
                    routeSet.pendingBuild.records.size() ==
                        routeSet.acceptedBuild.records.size() &&
                    pendingContainsAcceptedInstanceSet;
                // Preserve the last accepted source-only build only when a
                // same-set legacy bootstrap artifact appears. A changed set
                // instead uploads its current source-only shadow so new
                // instances can pass mask-zero pre-admission.
                const bool retainAcceptedSourceOnly =
                    acceptedSourceOnly &&
                    !pendingAnySourceOnly &&
                    pendingAcceptedInstanceSetExact;
                if (retainAcceptedSourceOnly)
                {
                    skinnedHitRouteUploadBuild =
                        routeSet.acceptedBuild;
                    skinnedHitRouteUploadBuildSignature =
                        routeSet.acceptedBuildSignature;
                }
                else
                {
                    skinnedHitRouteUploadBuild =
                        routeSet.pendingBuild;
                    skinnedHitRouteUploadBuildSignature =
                        routeSet.pendingBuildSignature;
                }
                // Source-only material indexes and other frame-local metadata
                // can legitimately rebuild while the exact skinned set and
                // its live AS resources stay unchanged. That current
                // source-only build is validated again by upload/TLAS
                // planning below; it does not require a full CPU bootstrap
                // frame merely because its byte signature changed.
                const bool acceptedAdmissionCompatible =
                    (acceptedSourceOnly &&
                        pendingSourceOnly &&
                        pendingContainsAcceptedInstanceSet) ||
                    (pendingAcceptedInstanceSetExact &&
                     skinnedHitRouteUploadBuildSignature != 0 &&
                        skinnedHitRouteUploadBuildSignature ==
                            routeSet.acceptedBuildSignature);
                if (!routeSet.acceptedBuild.records.empty() &&
                    acceptedAdmissionCompatible)
                {
                    const SmokeSkinnedCaptureLiveResult
                        liveResult =
                            ValidateSmokeSkinnedCaptureAcceptedBuildLive(
                                routeSet.acceptedBuild,
                                m_smokeSkinnedBlasStateTable,
                                m_smokeSkinnedComparisonBlases,
                                m_smokeSkinnedCurrentOutputVertexBuffer,
                                m_smokeGeometryUniverse.
                                    CanonicalSourceIndexBuffer());
                    if (liveResult ==
                        SmokeSkinnedCaptureLiveResult::Live)
                    {
                        skinnedCaptureAdmissionRoutes =
                            &routeSet.acceptedBuild.records;
                    }
                    else
                    {
                        common->Printf(
                            "PathTracePrimaryPass: GEO08 capture route-set invalidated before capture frame=%llu signature=%llu records=%zu reason=%s activeBlas=%zu retiredBlas=%zu outputGeneration=%llu action=cpu-fallback\n",
                            static_cast<unsigned long long>(
                                m_smokeGeometryFrameIndex),
                            static_cast<unsigned long long>(
                                routeSet.signature),
                            routeSet.acceptedBuild.records.size(),
                            SmokeSkinnedCaptureLiveResultName(
                                liveResult),
                            m_smokeSkinnedComparisonBlases.size(),
                            m_retiredSmokeSkinnedComparisonBlases.size(),
                            static_cast<unsigned long long>(
                                m_smokeSkinnedOutputBufferGeneration));
                        routeSet.acceptedBuild =
                            PtSkinnedHitRouteBuild();
                        routeSet.acceptedBuildSignature = 0;
                        skinnedHitRouteUploadBuild =
                            PtSkinnedHitRouteBuild();
                        skinnedHitRouteUploadBuildSignature = 0;
                        ++m_smokeSkinnedCapturePreCaptureInvalidations;
                    }
                }
                else if (!routeSet.acceptedBuild.records.empty())
                {
                    common->Printf(
                        "PathTracePrimaryPass: GEO08 capture route-set comparison upgrade frame=%llu signature=%llu pending/accepted=%zu/%zu buildSignature(pending/accepted)=%llu/%llu view(id/sub/mirror/xray/gui/eye/viewport)=%d/%d/%d/%d/%d/%d/%d,%d,%d,%d action=full-cpu-until-tlas-accepted\n",
                        static_cast<unsigned long long>(
                            m_smokeGeometryFrameIndex),
                        static_cast<unsigned long long>(
                            routeSet.signature),
                        skinnedHitRouteUploadBuild.records.size(),
                        routeSet.acceptedBuild.records.size(),
                        static_cast<unsigned long long>(
                            skinnedHitRouteUploadBuildSignature),
                        static_cast<unsigned long long>(
                            routeSet.acceptedBuildSignature),
                        viewDef ? viewDef->renderView.viewID : -1,
                        viewDef && viewDef->isSubview ? 1 : 0,
                        viewDef && viewDef->isMirror ? 1 : 0,
                        viewDef && viewDef->isXraySubview ? 1 : 0,
                        viewDef && viewDef->is2Dgui ? 1 : 0,
                        viewDef
                            ? viewDef->renderView.viewEyeBuffer
                            : -2,
                        viewDef ? viewDef->viewport.x1 : -1,
                        viewDef ? viewDef->viewport.y1 : -1,
                        viewDef ? viewDef->viewport.x2 : -1,
                        viewDef ? viewDef->viewport.y2 : -1);
                }
                break;
            }
        }
        if (geometrySourceDumpRequested)
        {
            m_smokeGeometryUniverse.DumpCanonicalSourceImportStats();
            m_smokeGeometryUniverse.DumpCanonicalIdentityImportStats();
            m_smokeGeometryUniverse.DumpCanonicalSourceGpuPoolStats();
            m_smokeGeometryUniverse.DumpCanonicalOffsetBlasProbeStats();
            const RtPathTraceCanonicalRigidCompareStats
                canonicalRigidSourceCompareStats =
                    m_smokeGeometryUniverse.
                        BuildCanonicalRigidSourceCompareStats();
            m_smokeGeometryUniverse.DumpCanonicalRigidSourceCompareStats(
                canonicalRigidSourceCompareStats);
        }
        if (useSceneUniverseStaticGeometry)
        {
            OPTICK_EVENT("PT Build Scene Universe Static Geometry");
            RtSmokeSurfaceClassStats sceneUniverseClassStats;
            RtSmokeSurfaceSkipStats sceneUniverseSkipStats;
            RtSmokeAttributeStats sceneUniverseAttributeStats;
            RtSmokeMaterialStats sceneUniverseMaterialStats;
            RtSmokeBucketRanges sceneUniverseBucketRanges;
            sceneUniverseStaticBuildStats = m_sceneUniverse.BuildFullStaticGeometry(viewDef, m_smokeGeometryUniverse, sceneUniverseClassStats, sceneUniverseSkipStats, sceneUniverseAttributeStats, sceneUniverseMaterialStats, sceneUniverseBucketRanges);
            skipStats.invalidIndexCount += sceneUniverseSkipStats.invalidIndexCount;
            skipStats.limitExceeded += sceneUniverseSkipStats.limitExceeded;
            skipStats.zeroAreaOnly += sceneUniverseSkipStats.zeroAreaOnly;
            skipStats.geometryStaticSurfaceBudgetExceeded +=
                sceneUniverseSkipStats.
                    geometryStaticSurfaceBudgetExceeded;
            skipStats.geometryStaticByteBudgetExceeded +=
                sceneUniverseSkipStats.
                    geometryStaticByteBudgetExceeded;
            skipStats.geometryStaticAdmissionInvalid +=
                sceneUniverseSkipStats.geometryStaticAdmissionInvalid;
            skipStats.geometryStaticAdmissionOverflow +=
                sceneUniverseSkipStats.
                    geometryStaticAdmissionOverflow;
            skipStats.geometryStaticAdmittedBytes =
                sceneUniverseSkipStats.geometryStaticAdmittedBytes;
            skipStats.geometryStaticRejectedBytes +=
                sceneUniverseSkipStats.geometryStaticRejectedBytes;
            skipStats.geometryStaticAdmittedSurfaces =
                sceneUniverseSkipStats.
                    geometryStaticAdmittedSurfaces;
        }
        if (useDrawSurfMirrorDynamicFrame)
        {
            {
                OPTICK_EVENT("PT Capture Visible Doom Surfaces");
                usingDoomSurfaces = CaptureDoomSurfacesForSmokeTest(viewDef, dynamicVertexData, dynamicIndexData, dynamicTriangleClassData, dynamicTriangleMaterialData, &dynamicTriangleInstanceData, &dynamicTriangleIdentityData, m_smokeGeometryUniverse, staticCacheChanged, m_smokeSceneOrigin, sourceSurfaces, sourceVerts, sourceIndexes, anchorTriangle, classStats, skipStats, dynamicStats, attributeStats, materialStats, bucketRanges, captureTiming, &currentSkinnedSurfaceRecords, false, false, true);
            }
            const bool staticAreaPreloadEnabled =
                r_pathTracingStaticAreaPreload.GetInteger() != 0 ||
                r_pathTracingPortalBruteforceFullMap.GetInteger() != 0;
            if (staticAreaPreloadEnabled)
            {
                OPTICK_EVENT("PT Static Area Preload");
                const int staticRecordsBefore = static_cast<int>(m_smokeGeometryUniverse.StaticSurfaceRecords().size());
                const int staticVertsBefore = static_cast<int>(m_smokeGeometryUniverse.StaticVertices().size());
                const RtPathTraceSceneUniverseBuildStats staticAreaPreloadStats = m_sceneUniverse.BuildSelectedStaticGeometry(
                    viewDef,
                    m_smokeGeometryUniverse,
                    classStats,
                    skipStats,
                    attributeStats,
                    materialStats,
                    bucketRanges,
                    idMath::ClampInt(0, 8, r_pathTracingStaticAreaPreloadPortalSteps.GetInteger()));
                if (staticAreaPreloadStats.built)
                {
                    usingDoomSurfaces = true;
                    sourceSurfaces += staticAreaPreloadStats.surfaces;
                    sourceVerts += staticAreaPreloadStats.vertices;
                    sourceIndexes += staticAreaPreloadStats.indexes;
                    if (static_cast<int>(m_smokeGeometryUniverse.StaticSurfaceRecords().size()) != staticRecordsBefore ||
                        static_cast<int>(m_smokeGeometryUniverse.StaticVertices().size()) != staticVertsBefore)
                    {
                        staticCacheChanged = true;
                    }
                }
            }
            const RtSmokeSurfaceClassStats staticClassStats = classStats;
            const RtSmokeSurfaceSkipStats staticSkipStats = skipStats;
            const RtSmokeBucketRange staticBucketRange = bucketRanges.buckets[0];
            const int staticSourceSurfaces = classStats.staticWorldSurfaces;
            const int staticSourceVerts = classStats.staticWorldVerts;
            const int staticSourceIndexes = classStats.staticWorldIndexes;

            RtSmokeSurfaceClassStats mirrorClassStats;
            RtSmokeSurfaceSkipStats mirrorSkipStats;
            RtSmokeDynamicGeometryStats mirrorDynamicStats;
            RtSmokeAttributeStats mirrorAttributeStats;
            RtSmokeMaterialStats mirrorMaterialStats;
            RtSmokeBucketRanges mirrorBucketRanges;
            RtSmokeSceneCaptureTiming mirrorCaptureTiming;
            int mirrorSourceSurfaces = 0;
            int mirrorSourceVerts = 0;
            int mirrorSourceIndexes = 0;
            {
                OPTICK_EVENT("PT Instance Universe BeginFrame");
                m_instanceUniverse.BeginFrame(m_smokeGeometryFrameIndex, viewDef);
                drawSurfMirrorFrameProducedFromDynamicCapture = true;
            }
            const bool drawSurfMirrorFullDiagnostics =
                dumpInstanceUniverse ||
                r_pathTracingSmokeLog.GetInteger() != 0 ||
                r_pathTracingSceneBoundsOverlay.GetInteger() != 0 ||
                rigidResidencyBoundsDebug;
            const bool usingMirrorDynamicFrame = CapturePathTraceDynamicFrameFromDrawSurfMirror(viewDef, nullptr, &m_smokeGeometryUniverse, dynamicVertexData, dynamicIndexData, dynamicTriangleClassData, dynamicTriangleMaterialData, &dynamicTriangleInstanceData, &dynamicTriangleIdentityData, mirrorSourceSurfaces, mirrorSourceVerts, mirrorSourceIndexes, mirrorClassStats, mirrorSkipStats, mirrorDynamicStats, mirrorAttributeStats, mirrorMaterialStats, mirrorBucketRanges, mirrorCaptureTiming, &currentSkinnedSurfaceRecords, r_pathTracingGeometryRenderedAttributeSurveyDump.GetInteger() != 0 ? &currentCapturedSurfaceRecords : nullptr, nullptr, &m_instanceUniverse, &m_smokeBoundsOverlayLines, drawSurfMirrorFullDiagnostics, skinnedCaptureAdmissionRoutes);

            {
                OPTICK_EVENT("PT Merge Mirror Capture Stats");
                classStats = RtSmokeSurfaceClassStats();
                classStats.staticWorldSurfaces = staticClassStats.staticWorldSurfaces;
                classStats.staticWorldVerts = staticClassStats.staticWorldVerts;
                classStats.staticWorldIndexes = staticClassStats.staticWorldIndexes;
                classStats.staticWorldTriangles = staticClassStats.staticWorldTriangles;
                classStats.rigidEntitySurfaces = mirrorClassStats.rigidEntitySurfaces;
                classStats.rigidEntityVerts = mirrorClassStats.rigidEntityVerts;
                classStats.rigidEntityIndexes = mirrorClassStats.rigidEntityIndexes;
                classStats.rigidEntityTriangles = mirrorClassStats.rigidEntityTriangles;
                classStats.skinnedDeformedSurfaces = mirrorClassStats.skinnedDeformedSurfaces;
                classStats.skinnedDeformedVerts = mirrorClassStats.skinnedDeformedVerts;
                classStats.skinnedDeformedIndexes = mirrorClassStats.skinnedDeformedIndexes;
                classStats.skinnedDeformedTriangles = mirrorClassStats.skinnedDeformedTriangles;
                classStats.particleAlphaSurfaces = mirrorClassStats.particleAlphaSurfaces;
                classStats.particleAlphaVerts = mirrorClassStats.particleAlphaVerts;
                classStats.particleAlphaIndexes = mirrorClassStats.particleAlphaIndexes;
                classStats.particleAlphaTriangles = mirrorClassStats.particleAlphaTriangles;
                classStats.unknownSurfaces = mirrorClassStats.unknownSurfaces;
                classStats.unknownVerts = mirrorClassStats.unknownVerts;
                classStats.unknownIndexes = mirrorClassStats.unknownIndexes;
                classStats.unknownTriangles = mirrorClassStats.unknownTriangles;
                skipStats = mirrorSkipStats;
                skipStats.limitExceeded +=
                    staticSkipStats.limitExceeded;
                skipStats.geometryStaticSurfaceBudgetExceeded +=
                    staticSkipStats.
                        geometryStaticSurfaceBudgetExceeded;
                skipStats.geometryStaticByteBudgetExceeded +=
                    staticSkipStats.geometryStaticByteBudgetExceeded;
                skipStats.geometryStaticAdmissionInvalid +=
                    staticSkipStats.geometryStaticAdmissionInvalid;
                skipStats.geometryStaticAdmissionOverflow +=
                    staticSkipStats.geometryStaticAdmissionOverflow;
                skipStats.geometryStaticAdmittedBytes =
                    staticSkipStats.geometryStaticAdmittedBytes;
                skipStats.geometryStaticRejectedBytes +=
                    staticSkipStats.geometryStaticRejectedBytes;
                skipStats.geometryStaticAdmittedSurfaces =
                    staticSkipStats.geometryStaticAdmittedSurfaces;
                dynamicStats = mirrorDynamicStats;
                attributeStats = mirrorAttributeStats;
                materialStats = mirrorMaterialStats;
                bucketRanges = mirrorBucketRanges;
                bucketRanges.buckets[0] = staticBucketRange;
                captureTiming.dynamicPassClassifyMs += mirrorCaptureTiming.dynamicPassClassifyMs;
                captureTiming.dynamicAppendMs += mirrorCaptureTiming.dynamicAppendMs;
                captureTiming.rtCpuSkinningAppendMs += mirrorCaptureTiming.rtCpuSkinningAppendMs;
                captureTiming.rtCpuSkinningAppendUs += mirrorCaptureTiming.rtCpuSkinningAppendUs;
                captureTiming.skinnedCaptureAdmissionRoutes += mirrorCaptureTiming.skinnedCaptureAdmissionRoutes;
                captureTiming.skinnedCaptureOmittedSurfaces += mirrorCaptureTiming.skinnedCaptureOmittedSurfaces;
                captureTiming.skinnedCaptureOmittedVerts += mirrorCaptureTiming.skinnedCaptureOmittedVerts;
                captureTiming.skinnedCaptureOmittedIndexes += mirrorCaptureTiming.skinnedCaptureOmittedIndexes;
                captureTiming.skinnedCaptureFallbackGate += mirrorCaptureTiming.skinnedCaptureFallbackGate;
                captureTiming.skinnedCaptureFallbackPriorRoute += mirrorCaptureTiming.skinnedCaptureFallbackPriorRoute;
                captureTiming.skinnedCaptureFallbackCurrentContract += mirrorCaptureTiming.skinnedCaptureFallbackCurrentContract;
                captureTiming.skinnedCaptureFallbackJointData += mirrorCaptureTiming.skinnedCaptureFallbackJointData;
                captureTiming.bucketMergeMs += mirrorCaptureTiming.bucketMergeMs;
                captureTiming.appendMs += mirrorCaptureTiming.appendMs;
                captureTiming.validationMs += mirrorCaptureTiming.validationMs;
                sourceSurfaces = staticSourceSurfaces + mirrorSourceSurfaces;
                sourceVerts = staticSourceVerts + mirrorSourceVerts;
                sourceIndexes = staticSourceIndexes + mirrorSourceIndexes;
                usingDoomSurfaces = usingDoomSurfaces || usingMirrorDynamicFrame;
            }
        }
        else
        {
            {
                OPTICK_EVENT("PT Capture Legacy Doom Surfaces");
                usingDoomSurfaces = CaptureDoomSurfacesForSmokeTest(viewDef, dynamicVertexData, dynamicIndexData, dynamicTriangleClassData, dynamicTriangleMaterialData, &dynamicTriangleInstanceData, &dynamicTriangleIdentityData, m_smokeGeometryUniverse, staticCacheChanged, m_smokeSceneOrigin, sourceSurfaces, sourceVerts, sourceIndexes, anchorTriangle, classStats, skipStats, dynamicStats, attributeStats, materialStats, bucketRanges, captureTiming, &currentSkinnedSurfaceRecords, useSceneUniverseStaticGeometry, source2RigidEntities != 0);
            }
        }
        {
            OPTICK_EVENT("PT DrawSurf Mirror");
            if (drawSurfMirrorFrameProducedFromDynamicCapture)
            {
                OPTICK_EVENT("PT DrawSurf Mirror EndFrame");
                m_instanceUniverse.EndFrame();
            }
            else
            {
                {
                    OPTICK_EVENT("PT Instance Universe BeginFrame");
                    m_instanceUniverse.BeginFrame(m_smokeGeometryFrameIndex, viewDef);
                }
                {
                    OPTICK_EVENT("PT DrawSurf Mirror Visible Producer");
                    CapturePathTraceDrawSurfMirror(viewDef, useSceneUniverseStaticGeometry ? &m_sceneUniverse : nullptr, &m_smokeGeometryUniverse, m_instanceUniverse, &m_smokeBoundsOverlayLines);
                }
            }
            {
                OPTICK_EVENT("PT EntityFeed Producer");
                ProduceEntityFeedRigidEntities(viewDef, m_smokeGeometryUniverse, m_instanceUniverse, materialStats);
            }
            const bool residencyV2 = r_pathTracingGeometryResidencyV2.GetInteger() != 0;
            // V2 diagnostics must consume the engine/feed snapshot below. The
            // legacy portal-area walk dereferences mutable entity hModel data
            // and is not a safe diagnostic source after map reload or during
            // ordinary front-end updates.
            if (rigidResidencyEnabled && !residencyV2)
            {
                OPTICK_EVENT("PT Legacy Residency Area Walk");
                m_smokeGeometryUniverse.RefreshRigidResidencyAreaWalk(
                    viewDef,
                    m_instanceUniverse,
                    idMath::ClampInt(0, 8, r_pathTracingRigidResidencyPortalSteps.GetInteger()),
                    !residencyV2,
                    &materialStats);
            }
            m_smokeBoundsOverlayLineCount = static_cast<int>(m_smokeBoundsOverlayLines.size());
        }
        if (useSceneUniverseStaticGeometry)
        {
            if (sceneUniverseStaticBuildStats.built)
            {
                staticCacheChanged = staticCacheChanged || !sceneUniverseStaticBuildStats.cacheHit;
                sourceSurfaces += sceneUniverseStaticBuildStats.surfaces;
                sourceVerts += sceneUniverseStaticBuildStats.vertices;
                sourceIndexes += sceneUniverseStaticBuildStats.indexes;
                const int staticSceneUniverseSurfaces = Max(0, sceneUniverseStaticBuildStats.surfaces - sceneUniverseStaticBuildStats.rigidEntitySurfaces);
                const int staticSceneUniverseTriangles = Max(0, sceneUniverseStaticBuildStats.triangles - sceneUniverseStaticBuildStats.rigidEntityTriangles);
                classStats.staticWorldSurfaces += staticSceneUniverseSurfaces;
                classStats.staticWorldIndexes += staticSceneUniverseTriangles * 3;
                classStats.staticWorldTriangles += staticSceneUniverseTriangles;
                classStats.rigidEntitySurfaces += sceneUniverseStaticBuildStats.rigidEntitySurfaces;
                classStats.rigidEntityIndexes += sceneUniverseStaticBuildStats.rigidEntityTriangles * 3;
                classStats.rigidEntityTriangles += sceneUniverseStaticBuildStats.rigidEntityTriangles;
                bucketRanges.buckets[0].surfaceCount += sceneUniverseStaticBuildStats.surfaces;
                usingDoomSurfaces = true;
                sceneUniverseGeneration = m_sceneUniverse.GetStats().generation;
                m_smokeSceneUniverseStaticBuildGeneration = sceneUniverseGeneration;
            }
        }
        {
            OPTICK_EVENT("PT Geometry Universe EndFrame");
            m_smokeGeometryUniverse.EndFrame();
        }
        if (useDrawSurfMirrorDynamicFrame && r_pathTracingStaticGeometryPruneMissing.GetInteger() != 0)
        {
            OPTICK_EVENT("PT Prune Missing Static Surfaces");
            staticCacheChanged = m_smokeGeometryUniverse.PruneMissingStaticSurfaces() || staticCacheChanged;
        }
    }
    if (r_pathTracingGeometryAdmissionDump.GetInteger() != 0)
    {
        const RtSmokeGeometryAdmissionBudget dynamicAdmissionBudget =
            BuildSmokeDynamicGeometryAdmissionBudget();
        const RtSmokeGeometryAdmissionBudget staticAdmissionBudget =
            BuildSmokeStaticGeometryAdmissionBudget();
        common->Printf(
            "PathTracePrimaryPass: GEO13 dynamic admission budget(bytes/surfaces)=%llu/%llu admitted(bytes/surfaces)=%llu/%llu rejected(surface/bytes/invalid/overflow/bytes)=%d/%d/%d/%d/%llu static admission budget(bytes/surfaces)=%llu/%llu admitted(bytes/surfaces)=%llu/%llu rejected(surface/bytes/invalid/overflow/bytes)=%d/%d/%d/%d/%llu limitTotal=%d\n",
            static_cast<unsigned long long>(
                dynamicAdmissionBudget.maxBytes),
            static_cast<unsigned long long>(
                dynamicAdmissionBudget.maxSurfaces),
            static_cast<unsigned long long>(
                skipStats.geometryAdmittedBytes),
            static_cast<unsigned long long>(
                skipStats.geometryAdmittedSurfaces),
            skipStats.geometrySurfaceBudgetExceeded,
            skipStats.geometryByteBudgetExceeded,
            skipStats.geometryAdmissionInvalid,
            skipStats.geometryAdmissionOverflow,
            static_cast<unsigned long long>(
                skipStats.geometryRejectedBytes),
            static_cast<unsigned long long>(
                staticAdmissionBudget.maxBytes),
            static_cast<unsigned long long>(
                staticAdmissionBudget.maxSurfaces),
            static_cast<unsigned long long>(
                skipStats.geometryStaticAdmittedBytes),
            static_cast<unsigned long long>(
                skipStats.geometryStaticAdmittedSurfaces),
            skipStats.geometryStaticSurfaceBudgetExceeded,
            skipStats.geometryStaticByteBudgetExceeded,
            skipStats.geometryStaticAdmissionInvalid,
            skipStats.geometryStaticAdmissionOverflow,
            static_cast<unsigned long long>(
                skipStats.geometryStaticRejectedBytes),
            skipStats.limitExceeded);
        r_pathTracingGeometryAdmissionDump.SetInteger(0);
    }
    skinnedOutputAudit =
        UpdateSmokeSkinnedOutputAllocator(
            viewDef,
            currentSkinnedSurfaceRecords,
            m_smokeGeometryUniverse,
            m_smokeSkinnedOutputAllocator,
            r_pathTracingGeometryAuthoritativeGpuSkinning.
                GetInteger() != 0,
            r_pathTracingGeometryShadowRegistry.
                GetInteger() != 0,
            m_smokeGeometryFrameIndex);
    std::vector<PathTraceSmokeVertex> nextPreviousSkinnedVertexData;
    std::vector<PathTraceSkinnedJointMatrix> nextPreviousSkinnedJointMatrices;
    const std::vector<RtSmokeSkinnedSurfaceRecord> emptyPreviousSkinnedRecords;
    const std::vector<PathTraceSmokeVertex> emptyPreviousSkinnedVertexData;
    const std::vector<PathTraceSkinnedJointMatrix> emptyPreviousSkinnedJointMatrices;
    RtSmokeSkinnedHistoryAudit skinnedHistoryAudit;
    skinnedHistoryAudit.ownerGate =
        r_pathTracingGeometryAuthoritativeGpuSkinning.GetInteger() != 0;
    const int skinnedHistoryOwnerGate =
        skinnedHistoryAudit.ownerGate ? 1 : 0;
    if (skinnedHistoryOwnerGate !=
        m_smokeSkinnedHistoryOwnerGateLast)
    {
        m_smokeLegacySkinnedHistoryState =
            RtSmokeSkinnedHistoryState();
        m_smokeSkinnedHistoryStates.clear();
        m_smokeSkinnedHistoryUpdateSerial = 0;
        m_smokeSkinnedHistoryOwnerGateLast =
            skinnedHistoryOwnerGate;
    }
    const bool skinnedHistoryHasView =
        viewDef != nullptr;
    const bool skinnedHistoryIsSubview =
        viewDef != nullptr &&
        viewDef->isSubview;
    skinnedHistoryAudit.primaryView =
        skinnedHistoryHasView &&
        !skinnedHistoryIsSubview;
    if (skinnedHistoryAudit.ownerGate &&
        skinnedHistoryAudit.primaryView)
    {
        skinnedHistoryAudit.owner =
            PtGeometryLifecycle::PrimaryHistoryOwnerKey(
                viewDef->renderWorld);
        skinnedHistoryAudit.ownerValid =
            PtCanonicalHistoryOwnerKeyIsValid(
                skinnedHistoryAudit.owner);
    }
    skinnedHistoryAudit.currentRecords =
        static_cast<int>(
            currentSkinnedSurfaceRecords.size());
    for (const RtSmokeSkinnedSurfaceRecord& record :
        currentSkinnedSurfaceRecords)
    {
        if (!skinnedHistoryAudit.ownerGate ||
            (skinnedHistoryAudit.ownerValid &&
                record.historyOwner ==
                    skinnedHistoryAudit.owner))
        {
            ++skinnedHistoryAudit.ownerMatches;
        }
        else
        {
            ++skinnedHistoryAudit.ownerMismatches;
        }
    }
    RtSmokeSkinnedHistoryState* matchingOwnerHistoryState =
        skinnedHistoryAudit.ownerGate &&
            skinnedHistoryAudit.primaryView &&
            PtCanonicalHistoryOwnerKeyIsValid(
                skinnedHistoryAudit.owner) &&
            skinnedHistoryAudit.ownerMismatches == 0
        ? FindSmokeSkinnedHistoryState(
            m_smokeSkinnedHistoryStates,
            skinnedHistoryAudit.owner)
        : nullptr;
    PtSkinnedHistoryPolicyInput skinnedHistoryPolicyInput;
    skinnedHistoryPolicyInput.ownerGate =
        skinnedHistoryAudit.ownerGate;
    skinnedHistoryPolicyInput.hasView =
        skinnedHistoryHasView;
    skinnedHistoryPolicyInput.isSubview =
        skinnedHistoryIsSubview;
    skinnedHistoryPolicyInput.owner =
        skinnedHistoryAudit.owner;
    skinnedHistoryPolicyInput.ownerMismatchCount =
        skinnedHistoryAudit.ownerMismatches;
    skinnedHistoryPolicyInput.matchingOwnerStateFound =
        matchingOwnerHistoryState != nullptr;
    skinnedHistoryPolicyInput.legacyStateFound =
        m_smokeLegacySkinnedHistoryState.updateSerial != 0;
    const PtSkinnedHistoryPolicyDecision
        skinnedHistoryDecision =
            PtSelectSkinnedHistoryPolicy(
                skinnedHistoryPolicyInput);
    skinnedHistoryAudit.route =
        skinnedHistoryDecision.route;
    skinnedHistoryAudit.primaryView =
        skinnedHistoryDecision.primaryView;
    skinnedHistoryAudit.ownerValid =
        skinnedHistoryDecision.ownerValid;
    skinnedHistoryAudit.previousStateFound =
        skinnedHistoryDecision.previousStateFound;
    skinnedHistoryAudit.readAllowed =
        skinnedHistoryDecision.readAllowed;
    skinnedHistoryAudit.writeAllowed =
        skinnedHistoryDecision.writeAllowed;
    RtSmokeSkinnedHistoryState* previousSkinnedHistoryState =
        skinnedHistoryAudit.readAllowed
            ? (skinnedHistoryAudit.ownerGate
                ? matchingOwnerHistoryState
                : &m_smokeLegacySkinnedHistoryState)
            : nullptr;
    if (previousSkinnedHistoryState)
    {
        skinnedHistoryAudit.previousRecords =
            static_cast<int>(
                previousSkinnedHistoryState->records.size());
        skinnedHistoryAudit.previousUpdateSerial =
            previousSkinnedHistoryState->updateSerial;
    }
    const std::vector<RtSmokeSkinnedSurfaceRecord>&
        previousSkinnedRecords =
            previousSkinnedHistoryState
                ? previousSkinnedHistoryState->records
                : emptyPreviousSkinnedRecords;
    const std::vector<PathTraceSmokeVertex>&
        previousSkinnedVertexData =
            previousSkinnedHistoryState
                ? previousSkinnedHistoryState->vertices
                : emptyPreviousSkinnedVertexData;
    const std::vector<PathTraceSkinnedJointMatrix>&
        previousSkinnedJointMatrices =
            previousSkinnedHistoryState
                ? previousSkinnedHistoryState->joints
                : emptyPreviousSkinnedJointMatrices;
    {
        OPTICK_EVENT("PT Skinned Previous Bridge");
        UpdateSmokeSkinnedPreviousCpuBridge(
            currentSkinnedSurfaceRecords,
            previousSkinnedRecords,
            previousSkinnedVertexData,
            dynamicVertexData,
            nextPreviousSkinnedVertexData);
    }
    const int gpuSkinningMode = idMath::ClampInt(0, 2, r_pathTracingGpuSkinning.GetInteger());
    const bool skinnedMotionBridgeNeedsScaffold =
        r_pathTracingMotionVectorExport.GetInteger() != 0 ||
        r_pathTracingDLSSRRGuideDebugView.GetInteger() != 0;
    const int skinnedScaffoldMode = skinnedMotionBridgeNeedsScaffold ? Max(1, gpuSkinningMode) : gpuSkinningMode;
    const bool buildSkinnedGpuSkinningInputs = gpuSkinningMode > 0;
    const bool canonicalSkinnedSourceOutputRoute =
        gpuSkinningMode == 1 &&
        r_pathTracingGeometryAuthoritativeGpuSkinning.
            GetInteger() != 0 &&
        r_pathTracingGeometryShadowRegistry.
            GetInteger() != 0;
    {
        OPTICK_EVENT("PT Skinned GPU Scaffold");
        skinnedGpuScaffold = BuildSmokeSkinnedGpuScaffold(
            skinnedScaffoldMode,
            gpuSkinningMode,
            buildSkinnedGpuSkinningInputs,
            canonicalSkinnedSourceOutputRoute,
            &m_smokeGeometryUniverse,
            &m_smokeSkinnedOutputAllocator,
            currentSkinnedSurfaceRecords,
            previousSkinnedRecords,
            dynamicVertexData,
            previousSkinnedVertexData,
            previousSkinnedJointMatrices);
    }
    {
        OPTICK_EVENT("PT Skinned Triangle Dispatch Index");
        BuildSmokeSkinnedTriangleDispatchIndex(skinnedGpuScaffold, static_cast<int>(dynamicIndexData.size() / 3));
    }
    {
        OPTICK_EVENT("PT Skinned JointCache Stage Plan");
        const bool authoritativeCurrentJointsRequested =
            r_pathTracingGeometryAuthoritativeGpuSkinning.GetInteger() != 0 &&
            gpuSkinningMode > 0;
        jointCacheStage = BuildSmokeJointCacheStage(
            authoritativeCurrentJointsRequested,
            currentSkinnedSurfaceRecords,
            skinnedGpuScaffold);
    }
    {
        OPTICK_EVENT("PT Retain Skinned Joints");
        RetainSmokeSkinnedCurrentJointMatrices(
            currentSkinnedSurfaceRecords,
            nextPreviousSkinnedJointMatrices);
    }
    m_smokeSkinnedSurfaceRecords = currentSkinnedSurfaceRecords;
    if (skinnedHistoryAudit.writeAllowed)
    {
        RtSmokeSkinnedHistoryState* nextHistoryState = nullptr;
        if (!skinnedHistoryAudit.ownerGate)
        {
            nextHistoryState =
                &m_smokeLegacySkinnedHistoryState;
        }
        else
        {
            nextHistoryState =
                FindSmokeSkinnedHistoryState(
                    m_smokeSkinnedHistoryStates,
                    skinnedHistoryAudit.owner);
            if (!nextHistoryState)
            {
                if (m_smokeSkinnedHistoryStates.size() >= 4)
                {
                    const auto oldest = std::min_element(
                        m_smokeSkinnedHistoryStates.begin(),
                        m_smokeSkinnedHistoryStates.end(),
                        [](const RtSmokeSkinnedHistoryState& lhs,
                            const RtSmokeSkinnedHistoryState& rhs)
                        {
                            return lhs.updateSerial <
                                rhs.updateSerial;
                        });
                    if (oldest !=
                        m_smokeSkinnedHistoryStates.end())
                    {
                        m_smokeSkinnedHistoryStates.erase(oldest);
                    }
                }
                m_smokeSkinnedHistoryStates.emplace_back();
                nextHistoryState =
                    &m_smokeSkinnedHistoryStates.back();
                nextHistoryState->owner =
                    skinnedHistoryAudit.owner;
            }
        }
        nextHistoryState->records =
            m_smokeSkinnedSurfaceRecords;
        nextHistoryState->vertices.swap(
            nextPreviousSkinnedVertexData);
        nextHistoryState->joints.swap(
            nextPreviousSkinnedJointMatrices);
        nextHistoryState->updateSerial =
            ++m_smokeSkinnedHistoryUpdateSerial;
        skinnedHistoryAudit.nextUpdateSerial =
            nextHistoryState->updateSerial;
    }

    if (useDrawSurfMirrorDynamicFrame && r_pathTracingRigidMeshValidate.GetInteger() != 0)
    {
        const RtPathTraceRigidMeshValidationStats rigidMeshValidationStats =
            m_smokeGeometryUniverse.ValidateRigidMeshCandidatesAgainstDynamicPayload(
                dynamicTriangleClassData,
                dynamicTriangleMaterialData,
                RT_SMOKE_TRIANGLE_CLASS_MASK,
                SmokeSurfaceClassId(RtSmokeSurfaceClass::RigidEntity));
        m_smokeGeometryUniverse.DumpRigidMeshValidationStats(rigidMeshValidationStats, sceneSource);
        r_pathTracingRigidMeshValidate.SetInteger(0);
    }
    if (useDrawSurfMirrorDynamicFrame && r_pathTracingRigidBlasPlanDump.GetInteger() != 0)
    {
        const RtPathTraceRigidBlasPlanStats rigidBlasPlanStats = m_smokeGeometryUniverse.BuildRigidBlasPlanStats(&classStats);
        m_smokeGeometryUniverse.DumpRigidBlasPlanStats(rigidBlasPlanStats, sceneSource);
        r_pathTracingRigidBlasPlanDump.SetInteger(0);
    }
    if (useDrawSurfMirrorDynamicFrame && r_pathTracingRigidBlasInputDump.GetInteger() != 0)
    {
        const RtPathTraceRigidBlasInputStats rigidBlasInputStats = m_smokeGeometryUniverse.BuildRigidBlasInputStats();
        m_smokeGeometryUniverse.DumpRigidBlasInputStats(rigidBlasInputStats, sceneSource);
        r_pathTracingRigidBlasInputDump.SetInteger(0);
    }
    if (useDrawSurfMirrorDynamicFrame)
    {
        const bool rigidBlasGpuScaffold = r_pathTracingRigidBlasGpuScaffold.GetInteger() != 0;
        const bool rigidBlasGpuBuild = rigidBlasGpuScaffold && r_pathTracingRigidBlasGpuBuild.GetInteger() != 0;
        const bool dumpRigidBlasGpu = r_pathTracingRigidBlasGpuDump.GetInteger() != 0;
        if (rigidBlasGpuScaffold)
        {
            const RtPathTraceRigidBlasGpuStats rigidBlasGpuStats = [&]() {
                OPTICK_EVENT("PT Rigid BLAS GPU Scaffold");
                return m_smokeGeometryUniverse.UpdateRigidBlasGpuScaffold(
                    device,
                    commandList,
                    rigidBlasGpuBuild,
                    idMath::ClampInt(
                        0,
                        1024,
                        r_pathTracingRigidBlasGpuBuildLimit.GetInteger()),
                    static_cast<uint64>(
                        idMath::ClampInt(
                            0,
                            1048576,
                            r_pathTracingRigidBlasGpuResultBudgetKB.
                                GetInteger())) *
                        1024ull,
                    dumpRigidBlasGpu);
            }();
            if (dumpRigidBlasGpu)
            {
                m_smokeGeometryUniverse.DumpRigidBlasGpuStats(rigidBlasGpuStats, sceneSource, true, rigidBlasGpuBuild);
                r_pathTracingRigidBlasGpuDump.SetInteger(0);
            }
        }
        else
        {
            m_smokeGeometryUniverse.ReleaseRigidBlasGpuScaffold();
            if (dumpRigidBlasGpu)
            {
                RtPathTraceRigidBlasGpuStats rigidBlasGpuStats;
                rigidBlasGpuStats.frameIndex = m_smokeGeometryFrameIndex;
                m_smokeGeometryUniverse.DumpRigidBlasGpuStats(rigidBlasGpuStats, sceneSource, false, false);
                r_pathTracingRigidBlasGpuDump.SetInteger(0);
            }
        }
    }
    RtPathTraceRigidResidencyStats rigidResidencyStats;
    if (useDrawSurfMirrorDynamicFrame)
    {
        {
            OPTICK_EVENT("PT Rigid Residency Update");
            rigidResidencyStats = m_smokeGeometryUniverse.UpdateRigidResidency(
                viewDef,
                m_instanceUniverse,
                rigidResidencyEnabled,
                idMath::ClampInt(0, 8, r_pathTracingRigidResidencyPortalSteps.GetInteger()));
        }
        if (geometrySourceDumpRequested)
        {
            const RtPathTraceCanonicalRigidIdentityStats identityStats =
                m_smokeGeometryUniverse.BuildCanonicalRigidIdentityStats(
                    m_instanceUniverse);
            m_smokeGeometryUniverse.DumpCanonicalRigidIdentityStats(
                identityStats);
        }
        m_smokeGeometryUniverse.UpdateCanonicalRigidBlasScaffold(
            device,
            commandList,
            m_instanceUniverse,
            r_pathTracingGeometryCanonicalRigidBlas.GetInteger() != 0);
        if (geometrySourceDumpRequested)
        {
            m_smokeGeometryUniverse.DumpCanonicalRigidBlasStats();
        }
        const int boundsOverlayMode = r_pathTracingSceneBoundsOverlay.GetInteger();
        const bool appendRigidResidencyBounds =
            rigidResidencyEnabled &&
            (boundsOverlayMode == 3 || boundsOverlayMode == 5 || (rigidResidencyBoundsDebug && boundsOverlayMode < 3));
        const bool appendStaticCacheBounds =
            boundsOverlayMode == 4 || boundsOverlayMode == 5 || boundsOverlayMode == 6;
        if ((appendRigidResidencyBounds || appendStaticCacheBounds) &&
            (rigidResidencyBoundsDebug || r_pathTracingSceneBoundsOverlay.GetInteger() != 0) &&
            static_cast<int>(m_smokeBoundsOverlayLines.size()) < RT_PT_BOUNDS_OVERLAY_MAX_LINES)
        {
            OPTICK_EVENT("PT Bounds Overlay Build");
            const int remainingLines = RT_PT_BOUNDS_OVERLAY_MAX_LINES - static_cast<int>(m_smokeBoundsOverlayLines.size());
            const int requestedResidentBoxes = Max(0, r_pathTracingSceneBoundsOverlayMax.GetInteger());
            int remainingBoxes = Min(Min(requestedResidentBoxes, RT_PT_RESIDENT_BOUNDS_OVERLAY_SAFE_BOXES), remainingLines / 12);
            if (appendStaticCacheBounds && remainingBoxes > 0)
            {
                std::vector<RtPathTraceRigidResidencyBoundsBox> staticBoundsBoxes;
                staticBoundsBoxes.reserve(remainingBoxes);
                m_smokeGeometryUniverse.CollectStaticSurfaceBoundsBoxes(staticBoundsBoxes, remainingBoxes, boundsOverlayMode == 6);
                AppendRigidResidencyBoundsOverlayLines(staticBoundsBoxes, m_smokeBoundsOverlayLines);
                remainingBoxes -= static_cast<int>(staticBoundsBoxes.size());
            }
            if (appendRigidResidencyBounds && remainingBoxes > 0)
            {
                std::vector<RtPathTraceRigidResidencyBoundsBox> residentBoundsBoxes;
                residentBoundsBoxes.reserve(remainingBoxes);
                m_smokeGeometryUniverse.CollectRigidResidencyBoundsBoxes(residentBoundsBoxes, remainingBoxes);
                AppendRigidResidencyBoundsOverlayLines(residentBoundsBoxes, m_smokeBoundsOverlayLines);
            }
            m_smokeBoundsOverlayLineCount = static_cast<int>(m_smokeBoundsOverlayLines.size());
        }
        if (r_pathTracingResidencyDebug.GetInteger() != 0)
        {
            m_smokeGeometryUniverse.DumpRigidResidencyStats(rigidResidencyStats, sceneSource, false);
        }
        if (r_pathTracingRigidResidencyDump.GetInteger() != 0)
        {
            m_smokeGeometryUniverse.DumpRigidResidencyStats(rigidResidencyStats, sceneSource);
            r_pathTracingRigidResidencyDump.SetInteger(0);
        }
    }
    ReleaseCompletedRetiredRigidGpuResources(
        m_smokeGeometryFrameIndex);
    RtSmokeRetiredRigidGpuResources
        retiredRigidGpuResources;
    if (m_smokeGeometryUniverse.
            TakeRetiredRigidGpuResources(
                retiredRigidGpuResources))
    {
        PushRetiredRigidGpuResources(
            retiredRigidGpuResources,
            m_smokeGeometryFrameIndex);
    }
    if (useDrawSurfMirrorDynamicFrame && r_pathTracingRigidTlasPlanDump.GetInteger() != 0)
    {
        const RtPathTraceRigidTlasPlanStats rigidTlasPlanStats = m_smokeGeometryUniverse.BuildRigidTlasPlanStats(m_instanceUniverse, &classStats);
        m_smokeGeometryUniverse.DumpRigidTlasPlanStats(rigidTlasPlanStats, sceneSource);
        r_pathTracingRigidTlasPlanDump.SetInteger(0);
    }
    const int captureMs = Sys_Milliseconds() - captureStartMs;
    if (dumpInstanceUniverse || r_pathTracingSmokeLog.GetInteger() != 0)
    {
        OPTICK_EVENT("PT Instance Universe Diagnostics");
        RtPathTraceInstanceUniverseDiagnosticDesc instanceDiagnosticDesc;
        instanceDiagnosticDesc.dumpRequested = dumpInstanceUniverse;
        instanceDiagnosticDesc.sceneSource = sceneSource;
        instanceDiagnosticDesc.legacySourceSurfaces = sourceSurfaces;
        instanceDiagnosticDesc.legacyClassStats = &classStats;
        instanceDiagnosticDesc.legacySkipStats = &skipStats;
        m_instanceUniverse.RunDiagnostics(instanceDiagnosticDesc);
    }
    if (dumpRigidMeshUniverse || r_pathTracingSmokeLog.GetInteger() != 0)
    {
        OPTICK_EVENT("PT Rigid Mesh Diagnostics");
        m_smokeGeometryUniverse.RunRigidMeshCandidateDiagnostics(dumpRigidMeshUniverse, sceneSource, &classStats);
    }
    if (!usingDoomSurfaces)
    {
        if (!m_smokeWaitingForDoomSurfaceLogged)
        {
            common->Printf("PathTracePrimaryPass: waiting for center camera ray Doom surface hit to build RT smoke BLAS\n");
            m_smokeWaitingForDoomSurfaceLogged = true;
        }
        return;
    }

    const int staticBucketRouteMode = idMath::ClampInt(
        RT_SMOKE_STATIC_BUCKET_ROUTE_DISABLED,
        RT_SMOKE_STATIC_BUCKET_ROUTE_PRIMARY_OPAQUE_PROBE,
        r_pathTracingGeometryStaticBucketRoute.GetInteger());
    const bool staticBucketPrimaryOpaqueProbe =
        IsSmokeStaticBucketPrimaryOpaqueProbeSupported(
            staticBucketRouteMode,
            r_pathTracingCleanRtxdiDiEnable.GetInteger() != 0,
            r_pathTracingCleanRtxdiDiView.GetInteger(),
            r_pathTracingNsightGpuMarkers.GetInteger() != 0,
            r_pathTracingCleanRestirGiEnable.GetInteger() != 0);
    const int staticBucketSecondaryProbeStage =
        r_pathTracingGeometryStaticBucketSecondaryProbeStage.GetInteger();
    const bool staticBucketProductionRoute =
        IsSmokeStaticBucketProductionRouteSupported(
            staticBucketRouteMode,
            r_pathTracingCleanRtxdiDiEnable.GetInteger() != 0,
            r_pathTracingCleanRtxdiDiView.GetInteger(),
            r_pathTracingNsightGpuMarkers.GetInteger() != 0,
            r_pathTracingCleanRestirGiEnable.GetInteger() != 0,
            r_pathTracingCleanRtxdiDiExternalPdfNeeCurrent.GetInteger() != 0 ||
                r_pathTracingRestirPdfNeeVerifierEnable.GetInteger() != 0,
            r_pathTracingDLSSRR.GetInteger() != 0,
            r_pathTracingCleanRtxdiDiTransmissionProducer.GetInteger() != 0 &&
                r_pathTracingCleanRtxdiDiTransmissionCompose.GetInteger() != 0,
            r_pathTracingCleanRtxdiDiGlassReflectionPsr.GetInteger() != 0,
            r_pathTracingReflectionSecondaryShadows.GetInteger() != 0,
            r_pathTracingReflectionOpaqueMirror.GetInteger() != 0,
            r_pathTracingCleanRtxdiDiGlassRefractedPsr.GetInteger() != 0,
            staticBucketSecondaryProbeStage);
    const bool staticBucketSecondaryOpticalPortalHalo =
        staticBucketProductionRoute &&
        (r_pathTracingCleanRtxdiDiGlassReflectionPsr.GetInteger() != 0 ||
            r_pathTracingReflectionOpaqueMirror.GetInteger() != 0 ||
            r_pathTracingCleanRtxdiDiGlassRefractedPsr.GetInteger() != 0);
    const int staticBucketPortalSteps =
        ResolveSmokeStaticBucketPortalSteps(
            r_pathTracingGeometryStaticBucketPortalSteps.GetInteger(),
            staticBucketSecondaryOpticalPortalHalo,
            r_pathTracingGeometryStaticBucketReflectionPortalSteps.GetInteger());
    const bool staticBucketCleanDiSecondaryIsolation =
        IsSmokeStaticBucketCleanDiSecondaryIsolationSupported(
            staticBucketRouteMode,
            r_pathTracingCleanRtxdiDiEnable.GetInteger() != 0,
            r_pathTracingCleanRtxdiDiView.GetInteger(),
            r_pathTracingNsightGpuMarkers.GetInteger() != 0,
            r_pathTracingCleanRestirGiEnable.GetInteger() != 0,
            r_pathTracingCleanRtxdiDiExternalPdfNeeCurrent.GetInteger() != 0 ||
                r_pathTracingRestirPdfNeeVerifierEnable.GetInteger() != 0,
            r_pathTracingCleanRtxdiDiTransmissionProducer.GetInteger() != 0 &&
                r_pathTracingCleanRtxdiDiTransmissionCompose.GetInteger() != 0,
            staticBucketSecondaryProbeStage);
    const bool staticBucketConsumerProbe =
        staticBucketPrimaryOpaqueProbe ||
        staticBucketCleanDiSecondaryIsolation;
    const bool staticBucketResidentMaterialRoute =
        staticBucketProductionRoute ||
        staticBucketConsumerProbe;

    RtSmokeMaterialMetadataRegistrationTiming metadataTiming;
    {
        OPTICK_EVENT("PT Register Material Metadata");
        metadataTiming = RegisterSmokeMaterialTextureInfoForFrame(viewDef, enableTextureProbe);
        if (r_pathTracingWorldStaticEmissives.GetInteger() != 0 ||
            useSceneUniverseStaticGeometry ||
            staticBucketResidentMaterialRoute)
        {
            const RtSmokeMaterialMetadataRegistrationTiming worldStaticMetadataTiming =
                RegisterSmokeWorldStaticMaterialTextureInfo(
                    viewDef,
                    enableTextureProbe ||
                        staticBucketResidentMaterialRoute);
            metadataTiming.metadataMs += worldStaticMetadataTiming.metadataMs;
            metadataTiming.registrationMs += worldStaticMetadataTiming.registrationMs;
        }
    }

    ProcessSmokeCrosshairZeroRoughnessToggle(viewDef);
    ProcessSmokeCrosshairFullMetalToggle(viewDef);

    const int materialStartMs = Sys_Milliseconds();
    RtSmokeMaterialTableBuild materialTable;
    const int rigidRouteMaxInstances = idMath::ClampInt(1, 510, r_pathTracingRigidRouteMaxInstances.GetInteger());
    const int asyncBvhRequestedJobs = r_pathTracingAsyncBvhJobs.GetInteger();
    const int asyncBvhJobCount = idMath::ClampInt(1, 16, asyncBvhRequestedJobs);
    const bool asyncBvhFramePlanning =
        r_pathTracingAsyncBvh.GetInteger() != 0 &&
        asyncBvhRequestedJobs > 0;
    const bool asyncCpuPlanning =
        asyncBvhFramePlanning ||
        r_pathTracingCpuPlanningAsync.GetInteger() != 0;
    RtSmokeRigidTlasPlanSnapshot rigidTlasSnapshot;
    RtSmokeRigidTlasPlan rigidTlasPlan;
    uint64_t rigidTlasPlanInputToken = 0;
    int rigidTlasPlanMs = 0;
    bool rigidTlasPlanValid = false;
    bool rigidTlasPlanAcceptedFromAsync = false;
    bool rigidTlasAsyncPlanCached = false;
    bool rigidTlasAsyncPlanQueued = false;
    if (enableRigidRouteForMode)
    {
        OPTICK_EVENT("PT Rigid TLAS Plan");
        const int rigidTlasSnapshotStartMs = Sys_Milliseconds();
        {
            OPTICK_EVENT("PT Rigid TLAS Snapshot");
            rigidTlasSnapshot = m_smokeGeometryUniverse.CaptureRigidTlasInstancePlanSnapshot(
                m_instanceUniverse,
                2,
                0x02,
                rigidRouteMaxInstances);
        }
        {
            OPTICK_EVENT("PT Rigid TLAS Input Token");
            rigidTlasPlanInputToken = BuildSmokeRigidTlasPlanInputToken(rigidTlasSnapshot);
        }
        const int rigidTlasSnapshotMs = Sys_Milliseconds() - rigidTlasSnapshotStartMs;

        RtPathTraceCpuWorkGeneration rigidTlasPlanGeneration;
        rigidTlasPlanGeneration.frameIndex = 0;
        rigidTlasPlanGeneration.sceneGeneration = m_smokeSceneUniverseStaticBuildGeneration;
        // The rigid TLAS worker produces membership/identity. Current-frame
        // transforms are refreshed from rigidTlasSnapshot after accept, so do
        // not key the worker generation on broad scene transform churn.
        rigidTlasPlanGeneration.geometryGeneration = 0;
        rigidTlasPlanGeneration.materialGeneration = 0;
        rigidTlasPlanGeneration.lightGeneration = rigidTlasPlanInputToken;
        RtPathTraceCpuWorkPublishSnapshot(m_smokeRigidTlasCpuWorkState, rigidTlasPlanGeneration);

        if (asyncCpuPlanning && m_smokeRigidTlasPlanFuture.valid())
        {
            OPTICK_EVENT("PT Rigid TLAS Async Accept");
            const std::future_status futureStatus =
                m_smokeRigidTlasPlanFuture.wait_for(std::chrono::seconds(0));
            if (futureStatus == std::future_status::ready)
            {
                const RtSmokeRigidTlasPlanTimedResult timedResult = m_smokeRigidTlasPlanFuture.get();
                m_smokeRigidTlasPlanAsyncGenerationValid = false;
                RtPathTraceCpuWorkResultEnvelope asyncEnvelope;
                asyncEnvelope.completed = true;
                asyncEnvelope.generation = m_smokeRigidTlasPlanAsyncGeneration;
                asyncEnvelope.timing = m_smokeRigidTlasPlanAsyncTiming;
                asyncEnvelope.timing.workerExecutionMs = static_cast<double>(timedResult.planningTimeMicros) / 1000.0;
                const double asyncOutstandingMs =
                    static_cast<double>(Max(0, Sys_Milliseconds() - m_smokeRigidTlasPlanAsyncLaunchMs));
                asyncEnvelope.timing.queueWaitMs = Max(0.0, asyncOutstandingMs - asyncEnvelope.timing.workerExecutionMs);
                RtPathTraceCpuWorkPublishCompletedResult(m_smokeRigidTlasCpuWorkState, asyncEnvelope);

                const RtPathTraceCpuWorkFrameDecision asyncDecision =
                    RtPathTraceCpuWorkAcceptLatest(m_smokeRigidTlasCpuWorkState, rigidTlasPlanGeneration, &asyncEnvelope, false);
                if (asyncDecision.accepted)
                {
                    rigidTlasPlan = timedResult.plan;
                    rigidTlasPlanMs = static_cast<int>(asyncEnvelope.timing.workerExecutionMs + 0.5);
                    m_smokeRigidTlasPlanAsyncCachedPlan = timedResult.plan;
                    m_smokeRigidTlasPlanAsyncCachedGeneration = m_smokeRigidTlasPlanAsyncGeneration;
                    m_smokeRigidTlasPlanAsyncCachedPlanValid = true;
                    rigidTlasPlanValid = true;
                    rigidTlasPlanAcceptedFromAsync = true;
                }
            }
            else
            {
                RtPathTraceCpuWorkAcceptLatest(m_smokeRigidTlasCpuWorkState, rigidTlasPlanGeneration, nullptr, true);
            }
        }

        if (!rigidTlasPlanValid &&
            asyncCpuPlanning &&
            m_smokeRigidTlasPlanAsyncCachedPlanValid &&
            RtPathTraceCpuWorkGenerationEquals(m_smokeRigidTlasPlanAsyncCachedGeneration, rigidTlasPlanGeneration))
        {
            rigidTlasPlan = m_smokeRigidTlasPlanAsyncCachedPlan;
            rigidTlasPlanValid = true;
            rigidTlasPlanAcceptedFromAsync = true;
            rigidTlasAsyncPlanCached = true;
        }

        if (!rigidTlasPlanValid)
        {
            const int rigidTlasPlanStartMs = Sys_Milliseconds();
            {
                OPTICK_EVENT("PT Rigid TLAS Sync Build");
                rigidTlasPlan = BuildSmokeRigidTlasPlan(rigidTlasSnapshot);
            }
            rigidTlasPlanMs = Sys_Milliseconds() - rigidTlasPlanStartMs;
            rigidTlasPlanValid = true;
            RtPathTraceCpuWorkResultEnvelope rigidTlasEnvelope;
            rigidTlasEnvelope.completed = true;
            rigidTlasEnvelope.generation = rigidTlasPlanGeneration;
            rigidTlasEnvelope.timing.snapshotCaptureMs = static_cast<double>(rigidTlasSnapshotMs);
            rigidTlasEnvelope.timing.workerExecutionMs = static_cast<double>(rigidTlasPlanMs);
            RtPathTraceCpuWorkPublishCompletedResult(m_smokeRigidTlasCpuWorkState, rigidTlasEnvelope);
            RtPathTraceCpuWorkAcceptLatest(m_smokeRigidTlasCpuWorkState, rigidTlasPlanGeneration, nullptr, true);
        }

        {
            OPTICK_EVENT("PT Rigid TLAS Refresh Transforms");
            RefreshSmokeRigidTlasPlanTransforms(rigidTlasPlan, rigidTlasSnapshot);
        }

        const bool rigidAsyncPlanAlreadyCached =
            m_smokeRigidTlasPlanAsyncCachedPlanValid &&
            RtPathTraceCpuWorkGenerationEquals(m_smokeRigidTlasPlanAsyncCachedGeneration, rigidTlasPlanGeneration);
        const bool rigidAsyncPlanAlreadyQueued =
            m_smokeRigidTlasPlanAsyncGenerationValid &&
            RtPathTraceCpuWorkGenerationEquals(m_smokeRigidTlasPlanAsyncGeneration, rigidTlasPlanGeneration);
        if (asyncCpuPlanning &&
            !m_smokeRigidTlasPlanFuture.valid() &&
            !rigidAsyncPlanAlreadyCached &&
            !rigidAsyncPlanAlreadyQueued)
        {
            OPTICK_EVENT("PT Rigid TLAS Queue");
            m_smokeRigidTlasPlanAsyncTiming = RtPathTraceCpuWorkTiming();
            m_smokeRigidTlasPlanAsyncTiming.snapshotCaptureMs = static_cast<double>(rigidTlasSnapshotMs);
            m_smokeRigidTlasPlanAsyncGeneration = rigidTlasPlanGeneration;
            m_smokeRigidTlasPlanAsyncGenerationValid = true;
            m_smokeRigidTlasPlanAsyncLaunchMs = Sys_Milliseconds();
            m_smokeRigidTlasPlanFuture.Start(
                [rigidTlasSnapshot]() {
                    return BuildSmokeRigidTlasPlanTimedResult(rigidTlasSnapshot);
                });
        }
        rigidTlasAsyncPlanQueued = m_smokeRigidTlasPlanFuture.valid();
    }
    std::vector<uint32_t> fullLevelStaticEmissiveMaterialIds;
    if (r_pathTracingWorldStaticEmissives.GetInteger() != 0)
    {
        fullLevelStaticEmissiveMaterialIds = [&]() {
            OPTICK_EVENT("PT World Static Emissive Material IDs");
            return BuildSmokeWorldStaticEmissiveMaterialIds(viewDef);
        }();
    }
    std::vector<uint32_t> rigidRouteMaterialIds;
    std::vector<uint32_t> materialTableStaticIds;
    {
        OPTICK_EVENT("PT Material Static ID List");
        materialTableStaticIds = staticTriangleMaterialCache;
        materialTableStaticIds.insert(materialTableStaticIds.end(), fullLevelStaticEmissiveMaterialIds.begin(), fullLevelStaticEmissiveMaterialIds.end());
        if (rigidTlasPlanValid)
        {
            rigidRouteMaterialIds = [&]() {
                OPTICK_EVENT("PT Rigid Route Material IDs");
                return m_smokeGeometryUniverse.CollectRigidRouteMaterialIds(rigidTlasPlan);
            }();
            materialTableStaticIds.insert(materialTableStaticIds.end(), rigidRouteMaterialIds.begin(), rigidRouteMaterialIds.end());
        }
        if (staticBucketResidentMaterialRoute)
        {
            // Every route that can admit resident buckets must cover the full
            // resident triangle-material stream before portal movement makes
            // another bucket active. Keep that stable universe first so
            // portal-selected monolithic membership cannot shift or omit the
            // table indexes consumed by bucket triangles.
            const std::vector<uint32_t>&
                staticBucketTriangleMaterialIds =
                    m_staticBucketGeometryUniverse.
                        StaticTriangleMaterials();
            const std::vector<uint32_t>
                staticBucketProbeMaterialIds =
                    BuildUniqueMaterialIdsPreservingOrder(
                        staticBucketTriangleMaterialIds);
            std::vector<uint32_t> stableBucketMaterialIds =
                staticBucketProbeMaterialIds;
            stableBucketMaterialIds.insert(
                stableBucketMaterialIds.end(),
                materialTableStaticIds.begin(),
                materialTableStaticIds.end());
            materialTableStaticIds.swap(
                stableBucketMaterialIds);
        }
    }
    const std::vector<uint32_t>* materialHydrationIds = nullptr;
    {
        OPTICK_EVENT("PT Hydrate Cached Material Metadata");
        const uint64 materialHydrationStaticGeneration = m_smokeGeometryUniverse.StaticMaterialGeneration();
        const size_t materialHydrationStaticTriangleMaterialCount = staticTriangleMaterialCache.size();
        const uint64 materialHydrationEmissiveSignature =
            BuildSortedUniqueMaterialIdSignature(fullLevelStaticEmissiveMaterialIds);
        const uint64 materialHydrationRigidSignature =
            BuildSortedUniqueMaterialIdSignature(rigidRouteMaterialIds);
        const uint64 materialHydrationStaticBucketProbeSignature =
            staticBucketResidentMaterialRoute
                ? BuildSortedUniqueMaterialIdSignature(
                    m_staticBucketGeometryUniverse.
                        StaticTriangleMaterials())
                : 0;
        const bool materialHydrationIdsCacheHit =
            m_smokeMaterialHydrationIdsValid &&
            m_smokeMaterialHydrationStaticGeneration == materialHydrationStaticGeneration &&
            m_smokeMaterialHydrationStaticTriangleMaterialCount == materialHydrationStaticTriangleMaterialCount &&
            m_smokeMaterialHydrationEmissiveSignature == materialHydrationEmissiveSignature &&
            m_smokeMaterialHydrationRigidSignature == materialHydrationRigidSignature &&
            m_smokeMaterialHydrationStaticBucketProbeSignature ==
                materialHydrationStaticBucketProbeSignature;
        if (!materialHydrationIdsCacheHit)
        {
            OPTICK_EVENT("PT Material Hydration Unique IDs");
            m_smokeMaterialHydrationIds = BuildSortedUniqueMaterialIds(materialTableStaticIds);
            m_smokeMaterialHydrationIdsValid = true;
            m_smokeMaterialHydrationStaticGeneration = materialHydrationStaticGeneration;
            m_smokeMaterialHydrationStaticTriangleMaterialCount = materialHydrationStaticTriangleMaterialCount;
            m_smokeMaterialHydrationEmissiveSignature = materialHydrationEmissiveSignature;
            m_smokeMaterialHydrationRigidSignature = materialHydrationRigidSignature;
            m_smokeMaterialHydrationStaticBucketProbeSignature =
                materialHydrationStaticBucketProbeSignature;
        }
        materialHydrationIds = &m_smokeMaterialHydrationIds;
        const RtSmokeMaterialMetadataRegistrationTiming cachedStaticMetadataTiming =
            RegisterSmokeMaterialTextureInfoForMaterialIds(
                *materialHydrationIds,
                enableTextureProbe ||
                    staticBucketResidentMaterialRoute);
        metadataTiming.metadataMs += cachedStaticMetadataTiming.metadataMs;
        metadataTiming.registrationMs += cachedStaticMetadataTiming.registrationMs;
    }
    const int metadataMs = metadataTiming.metadataMs;
    const int metadataValidationMs = metadataTiming.validationMs;
    const int metadataRegistrationMs = metadataTiming.registrationMs;
    uint64 materialTableSignature = 0;
    bool materialTableCacheHit = false;
    const bool useMaterialUniverseTable = r_pathTracingMaterialUniverseTable.GetInteger() != 0;
    const char* materialTablePath = useMaterialUniverseTable ? "universe" : "legacy";
    const int materialTextureTableMinimum = cleanRtxdiDiMaterialValidationRoute ? RT_SMOKE_TEXTURE_EXPERIMENTAL_ACTIVE_CAP : 0;
    std::vector<uint32_t> materialTableSupplementalIds;
    for (const RtSmokeSkinnedSurfaceRecord& record :
        currentSkinnedSurfaceRecords)
    {
        if (record.cpuCaptureOmitted &&
            record.materialId != UINT32_MAX)
        {
            // Source-only skinned routes still need a material-table entry
            // after their legacy per-triangle material stream is removed.
            if (materialTableSupplementalIds.empty())
            {
                materialTableSupplementalIds =
                    dynamicTriangleMaterialData;
            }
            materialTableSupplementalIds.push_back(
                record.materialId);
        }
    }
    const std::vector<uint32_t>& materialTableDynamicIds =
        materialTableSupplementalIds.empty()
            ? dynamicTriangleMaterialData
            : materialTableSupplementalIds;
    {
        OPTICK_EVENT("PT Material Table Build");
        BeginSmokeMaterialUniverseFrame();
        if (useMaterialUniverseTable)
        {
            BuildSmokeMaterialTableFromUniverseCached(materialTable, materialTableStaticIds, materialTableDynamicIds, m_smokeTextureProbeMaterialId, m_smokeTextureProbeRequestedIndex, enableTextureProbe, materialTextureTableMinimum, materialTableSignature, materialTableCacheHit);
        }
        else
        {
            BuildSmokeMaterialTableCached(materialTable, materialTableStaticIds, materialTableDynamicIds, m_smokeTextureProbeMaterialId, m_smokeTextureProbeRequestedIndex, enableTextureProbe, materialTextureTableMinimum, materialTableSignature, materialTableCacheHit);
        }
        if (!materialTableSupplementalIds.empty())
        {
            // The supplemental IDs populate the shared table but do not
            // describe legacy dynamic triangles. Preserve the exact
            // triangle-index ABI.
            materialTable.dynamicMaterialIndexes.resize(
                dynamicTriangleMaterialData.size());
        }
    }
    const RtSmokeMaterialTableCacheStats materialTableCacheStats = GetSmokeMaterialTableCacheStats();
    const RtSmokeMaterialTableBuildStats materialTableBuildStats = GetSmokeMaterialTableBuildStats();
    const RtMaterialClassifierStats materialClassifierStats = GetPathTraceMaterialClassifierStats();
    const RtSmokeMaterialUniverseStats materialUniverseStats = GetSmokeMaterialUniverseStats();
    if (!ValidateSmokeMaterialIndexes(materialTable))
    {
        common->Printf("PathTracePrimaryPass: invalid RT smoke material table, skipping scene build\n");
        return;
    }
    if ((cleanRtxdiDiSceneBuildRluEmissives || neeCacheSceneBuildRluEmissives) && !cleanRtxdiDiMaterialValidationRoute)
    {
        for (PathTraceSmokeMaterial& material : materialTable.materials)
        {
            material.diffuseTextureIndex = UINT32_MAX;
            material.alphaTextureIndex = UINT32_MAX;
            material.normalTextureIndex = UINT32_MAX;
            material.specularTextureIndex = UINT32_MAX;
            material.textureWidth = 1;
            material.textureHeight = 1;
            material.alphaTextureWidth = 1;
            material.alphaTextureHeight = 1;
            material.normalTextureWidth = 1;
            material.normalTextureHeight = 1;
            material.specularTextureWidth = 1;
            material.specularTextureHeight = 1;
        }
    }

    idImage* skyEnvironmentSource = nullptr;
    for (const RtSmokeMaterialTextureInfo& materialInfo : materialTable.materialInfos)
    {
        if (materialInfo.skyEnvironment && materialInfo.skyImage)
        {
            skyEnvironmentSource = materialInfo.skyImage;
            break;
        }
    }
    const char* requestedSkyEnvironmentName = skyEnvironmentSource
        ? skyEnvironmentSource->GetName()
        : RT_SMOKE_SKY_ENVIRONMENT_FALLBACK_NAME;
    const bool skyEnvironmentSourceUnchanged =
        m_smokeSkyEnvironmentSourceName.Icmp(requestedSkyEnvironmentName) == 0;
    nvrhi::TextureHandle skyEnvironmentCube = skyEnvironmentSourceUnchanged
        ? m_smokeSkyEnvironmentCube
        : nullptr;
    nvrhi::BindingSetHandle skyCubeProbeBindingSet = skyEnvironmentSourceUnchanged
        ? m_smokeSkyCubeProbeBindingSet
        : nullptr;

    if (!skyEnvironmentCube)
    {
        skyEnvironmentCube = CreateSmokeSkyEnvironmentCube(commandList, device, skyEnvironmentSource);
    }
    if (skyEnvironmentCube &&
        !skyCubeProbeBindingSet &&
        m_smokeSkyCubeProbeBindingLayout &&
        m_smokeSkyCubeProbeOutputTexture &&
        m_backend)
    {
        nvrhi::BindingSetDesc skyCubeProbeBindingSetDesc;
        skyCubeProbeBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(
            0,
            skyEnvironmentCube,
            nvrhi::Format::UNKNOWN,
            nvrhi::AllSubresources,
            nvrhi::TextureDimension::TextureCube));
        skyCubeProbeBindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(
            1,
            m_smokeSkyCubeProbeOutputTexture));
        skyCubeProbeBindingSetDesc.addItem(nvrhi::BindingSetItem::Sampler(
            0,
            m_backend->GetCommonPasses().m_LinearClampSampler));
        skyCubeProbeBindingSet = device->createBindingSet(
            skyCubeProbeBindingSetDesc,
            m_smokeSkyCubeProbeBindingLayout);
        if (!skyCubeProbeBindingSet)
        {
            common->Printf("PathTracePrimaryPass: failed to create isolated sky-cube probe binding set\n");
        }
    }

    if (m_smokeSkyEnvironmentSourceName.Icmp(requestedSkyEnvironmentName) != 0)
    {
        m_smokeSkyEnvironmentSourceName = requestedSkyEnvironmentName;
        common->Printf(
            "PathTracePrimaryPass: sky environment source='%s' live=%s cube=%d%s\n",
            m_smokeSkyEnvironmentSourceName.c_str(),
            r_pathTracingSkyCubeEnvironment.GetInteger() != 0 ? "primary-cube" : "white-fallback",
            skyEnvironmentCube ? 1 : 0,
            skyEnvironmentSource ? "" : " fallback");
    }

    const std::vector<PathTraceSmokeMaterial> stableGpuMaterialTableMaterials = materialTable.materials;
    {
        OPTICK_EVENT("PT Runtime Material Registers");
        ApplySmokeRuntimeMaterialRegistersToTable(viewDef, materialTable, materialStats, materialTextureTableMinimum);
    }
    std::vector<PathTraceDynamicMaterialRecord> dynamicMaterialRecords = [&]() {
        OPTICK_EVENT("PT Dynamic Material Records");
        return BuildSmokeDynamicMaterialRecords(materialTable, materialStats, viewDef);
    }();
    const RtSmokeSkinnedMaterialStateAudit skinnedMaterialStateAudit =
        ApplySmokeSkinnedMaterialStateToDispatches(
            skinnedGpuScaffold,
            currentSkinnedSurfaceRecords,
            dynamicTriangleMaterialData,
            materialTable.dynamicMaterialIndexes,
            materialTable,
            dynamicMaterialRecords);
    // Persistent/static geometry owns immutable authored vertices. Runtime
    // material variants can still live in that route (for example fanspin), so
    // apply their matrices to a per-frame upload copy rather than accumulating
    // transforms into the persistent cache.
    std::vector<PathTraceSmokeVertex> staticVertexFrameData = staticVertexCache;
    for (int materialIndex = 0; materialIndex < static_cast<int>(materialTable.materials.size()); ++materialIndex)
    {
        ApplySmokeDynamicAlphaRecordToGpuMaterial(
            static_cast<uint32_t>(materialIndex),
            dynamicMaterialRecords,
            materialTable.materials[materialIndex]);
    }
    const bool stableGpuMaterialTableCovered =
        r_pathTracingResidency.GetInteger() != 0 &&
        r_pathTracingResidencyMaterial.GetInteger() != 0 &&
        CanUploadStableSmokeMaterialTableWithDynamicOverrides(
            stableGpuMaterialTableMaterials,
            materialTable.materials,
            dynamicMaterialRecords);
    const std::vector<PathTraceSmokeMaterial>& gpuMaterialTableMaterials =
        stableGpuMaterialTableCovered ? stableGpuMaterialTableMaterials : materialTable.materials;
    LogSmokeMaterialClassifierLiveSummary(materialTable, materialClassifierStats);
    RtSmokeTextureCoverageStats textureCoverageStats;
    const bool needTextureCoverageStats = enableTextureProbe && r_pathTracingSmokeLog.GetInteger() != 0;
    if (needTextureCoverageStats)
    {
        textureCoverageStats = BuildSmokeTextureCoverageStats(
            materialTable,
            staticTriangleClassCache,
            materialTable.staticMaterialIndexes,
            dynamicTriangleClassData,
            materialTable.dynamicMaterialIndexes);
    }
    const int materialMs = Sys_Milliseconds() - materialStartMs;
    RtSmokeMaterialDiagnosticTriggerDesc materialDiagnosticDesc;
    materialDiagnosticDesc.materialTable = &materialTable;
    materialDiagnosticDesc.enableTextureProbe = enableTextureProbe;
    const bool buildRigidRouteBuffers = enableRigidRouteForMode;
    RtPathTraceRigidRouteBuild rigidRouteBuild;
    int rigidRouteBuildMs = 0;
    bool rigidRouteBuildAcceptedFromAsync = false;
    bool rigidRouteBuildAsyncCached = false;
    bool rigidRouteBuildAsyncQueued = false;
    uint64_t rigidRouteGeometryUploadSignature = 0;
    uint64_t rigidRouteInstanceUploadSignature = 0;
    bool rigidRouteGeometryUploadSignatureValid = false;
    bool rigidRouteInstanceUploadSignatureValid = false;
    if (buildRigidRouteBuffers)
    {
        OPTICK_EVENT("PT Rigid Route Buffers");
        if (asyncBvhFramePlanning)
        {
            OPTICK_EVENT("PT Rigid Route Async Orchestration");
            int rigidRouteSnapshotMs = 0;
            const int rigidRouteMetadataSnapshotStartMs = Sys_Milliseconds();
            const RtPathTraceRigidRouteBuildSnapshot rigidRouteMetadataSnapshot = [&]() {
                OPTICK_EVENT("PT Rigid Route Metadata Snapshot");
                return m_smokeGeometryUniverse.CaptureRigidRouteBuildSnapshot(rigidTlasPlan, materialTable.materialIds, false);
            }();
            rigidRouteSnapshotMs += Sys_Milliseconds() - rigidRouteMetadataSnapshotStartMs;
            RtPathTraceRigidRouteBuildSnapshot rigidRouteSnapshot;
            bool rigidRouteSnapshotValid = false;
            const auto CaptureRigidRoutePayloadSnapshot = [&]() -> const RtPathTraceRigidRouteBuildSnapshot& {
                if (!rigidRouteSnapshotValid)
                {
                    const int payloadSnapshotStartMs = Sys_Milliseconds();
                    {
                        OPTICK_EVENT("PT Rigid Route Payload Snapshot");
                        rigidRouteSnapshot =
                            m_smokeGeometryUniverse.CaptureRigidRouteBuildSnapshot(rigidTlasPlan, materialTable.materialIds, true);
                    }
                    rigidRouteSnapshotMs += Sys_Milliseconds() - payloadSnapshotStartMs;
                    rigidRouteSnapshotValid = true;
                }
                return rigidRouteSnapshot;
            };
            const uint64_t rigidRouteMaterialIdSignature = [&]() {
                OPTICK_EVENT("PT Rigid Route Material Signature");
                return BuildSmokeRigidRouteMaterialIdSignature(materialTable.materialIds);
            }();
            const uint64_t rigidRouteBuildStructureToken = [&]() {
                OPTICK_EVENT("PT Rigid Route Structure Token");
                return BuildSmokeRigidRouteStructureToken(rigidTlasPlan, materialTable.materialIds);
            }();
            const uint64_t rigidRoutePayloadToken = [&]() {
                OPTICK_EVENT("PT Rigid Route Payload Token");
                return BuildSmokeRigidRoutePayloadToken(rigidRouteMetadataSnapshot);
            }();

            RtPathTraceCpuWorkGeneration rigidRouteBuildGeneration;
            rigidRouteBuildGeneration.frameIndex = 0;
            rigidRouteBuildGeneration.sceneGeneration = m_smokeSceneUniverseStaticBuildGeneration;
            rigidRouteBuildGeneration.geometryGeneration = rigidRoutePayloadToken;
            rigidRouteBuildGeneration.materialGeneration = rigidRouteMaterialIdSignature;
            rigidRouteBuildGeneration.lightGeneration = rigidRouteBuildStructureToken;
            RtPathTraceCpuWorkPublishSnapshot(m_smokeRigidRouteBuildCpuWorkState, rigidRouteBuildGeneration);

            if (m_smokeRigidRouteBuildFuture.valid())
            {
                OPTICK_EVENT("PT Rigid Route Async Accept");
                const std::future_status futureStatus =
                    m_smokeRigidRouteBuildFuture.wait_for(std::chrono::seconds(0));
                if (futureStatus == std::future_status::ready)
                {
                    const RtPathTraceRigidRouteBuildTimedResult timedResult = m_smokeRigidRouteBuildFuture.get();
                    m_smokeRigidRouteBuildAsyncGenerationValid = false;
                    RtPathTraceCpuWorkResultEnvelope asyncEnvelope;
                    asyncEnvelope.completed = true;
                    asyncEnvelope.generation = m_smokeRigidRouteBuildAsyncGeneration;
                    asyncEnvelope.timing = m_smokeRigidRouteBuildAsyncTiming;
                    asyncEnvelope.timing.workerExecutionMs = static_cast<double>(timedResult.buildTimeMicros) / 1000.0;
                    const double asyncOutstandingMs =
                        static_cast<double>(Max(0, Sys_Milliseconds() - m_smokeRigidRouteBuildAsyncLaunchMs));
                    asyncEnvelope.timing.queueWaitMs = Max(0.0, asyncOutstandingMs - asyncEnvelope.timing.workerExecutionMs);
                    RtPathTraceCpuWorkPublishCompletedResult(m_smokeRigidRouteBuildCpuWorkState, asyncEnvelope);

                    const RtPathTraceCpuWorkFrameDecision asyncDecision =
                        RtPathTraceCpuWorkAcceptLatest(m_smokeRigidRouteBuildCpuWorkState, rigidRouteBuildGeneration, &asyncEnvelope, false);
                    if (asyncDecision.accepted)
                    {
                        rigidRouteBuild = timedResult.build;
                        rigidRouteGeometryUploadSignature = timedResult.geometryUploadSignature;
                        rigidRouteInstanceUploadSignature = timedResult.instanceUploadSignature;
                        rigidRouteGeometryUploadSignatureValid = timedResult.geometryUploadSignatureValid;
                        rigidRouteInstanceUploadSignatureValid = timedResult.instanceUploadSignatureValid;
                        rigidRouteBuildMs = 0;
                        m_smokeRigidRouteBuildAsyncCachedBuild = timedResult.build;
                        m_smokeRigidRouteBuildAsyncCachedGeometryUploadSignature = timedResult.geometryUploadSignature;
                        m_smokeRigidRouteBuildAsyncCachedInstanceUploadSignature = timedResult.instanceUploadSignature;
                        m_smokeRigidRouteBuildAsyncCachedGeometryUploadSignatureValid = timedResult.geometryUploadSignatureValid;
                        m_smokeRigidRouteBuildAsyncCachedInstanceUploadSignatureValid = timedResult.instanceUploadSignatureValid;
                        m_smokeRigidRouteBuildAsyncCachedGeneration = m_smokeRigidRouteBuildAsyncGeneration;
                        m_smokeRigidRouteBuildAsyncCachedBuildValid = true;
                        rigidRouteBuildAcceptedFromAsync = true;
                    }
                }
                else
                {
                    RtPathTraceCpuWorkAcceptLatest(m_smokeRigidRouteBuildCpuWorkState, rigidRouteBuildGeneration, nullptr, true);
                }
            }

            if (!rigidRouteBuildAcceptedFromAsync &&
                m_smokeRigidRouteBuildAsyncCachedBuildValid &&
                RtPathTraceCpuWorkGenerationEquals(m_smokeRigidRouteBuildAsyncCachedGeneration, rigidRouteBuildGeneration))
            {
                rigidRouteBuild = m_smokeRigidRouteBuildAsyncCachedBuild;
                rigidRouteGeometryUploadSignature = m_smokeRigidRouteBuildAsyncCachedGeometryUploadSignature;
                rigidRouteInstanceUploadSignature = m_smokeRigidRouteBuildAsyncCachedInstanceUploadSignature;
                rigidRouteGeometryUploadSignatureValid = m_smokeRigidRouteBuildAsyncCachedGeometryUploadSignatureValid;
                rigidRouteInstanceUploadSignatureValid = m_smokeRigidRouteBuildAsyncCachedInstanceUploadSignatureValid;
                rigidRouteBuildMs = 0;
                rigidRouteBuildAcceptedFromAsync = true;
                rigidRouteBuildAsyncCached = true;
            }

            if (!rigidRouteBuildAcceptedFromAsync)
            {
                const int rigidRouteBuildStartMs = Sys_Milliseconds();
                {
                    OPTICK_EVENT("PT Rigid Route Sync Build");
                    rigidRouteBuild = BuildRigidRouteBuffersFromSnapshot(CaptureRigidRoutePayloadSnapshot());
                    rigidRouteGeometryUploadSignature = BuildRigidRouteGeometryUploadSignature(rigidRouteBuild);
                    rigidRouteInstanceUploadSignature = BuildRigidRouteInstanceUploadSignature(rigidRouteBuild);
                    rigidRouteGeometryUploadSignatureValid = true;
                    rigidRouteInstanceUploadSignatureValid = true;
                }
                rigidRouteBuildMs = Sys_Milliseconds() - rigidRouteBuildStartMs;
                RtPathTraceCpuWorkResultEnvelope rigidRouteEnvelope;
                rigidRouteEnvelope.completed = true;
                rigidRouteEnvelope.generation = rigidRouteBuildGeneration;
                rigidRouteEnvelope.timing.snapshotCaptureMs = static_cast<double>(rigidRouteSnapshotMs);
                rigidRouteEnvelope.timing.workerExecutionMs = static_cast<double>(rigidRouteBuildMs);
                RtPathTraceCpuWorkPublishCompletedResult(m_smokeRigidRouteBuildCpuWorkState, rigidRouteEnvelope);
                RtPathTraceCpuWorkAcceptLatest(m_smokeRigidRouteBuildCpuWorkState, rigidRouteBuildGeneration, nullptr, true);
            }

            const bool rigidRouteBuildAlreadyCached =
                m_smokeRigidRouteBuildAsyncCachedBuildValid &&
                RtPathTraceCpuWorkGenerationEquals(m_smokeRigidRouteBuildAsyncCachedGeneration, rigidRouteBuildGeneration);
            const bool rigidRouteBuildAlreadyQueued =
                m_smokeRigidRouteBuildAsyncGenerationValid &&
                RtPathTraceCpuWorkGenerationEquals(m_smokeRigidRouteBuildAsyncGeneration, rigidRouteBuildGeneration);
            if (!m_smokeRigidRouteBuildFuture.valid() &&
                !rigidRouteBuildAlreadyCached &&
                !rigidRouteBuildAlreadyQueued)
            {
                OPTICK_EVENT("PT Rigid Route Queue");
                m_smokeRigidRouteBuildAsyncTiming = RtPathTraceCpuWorkTiming();
                m_smokeRigidRouteBuildAsyncTiming.snapshotCaptureMs = static_cast<double>(rigidRouteSnapshotMs);
                m_smokeRigidRouteBuildAsyncGeneration = rigidRouteBuildGeneration;
                m_smokeRigidRouteBuildAsyncGenerationValid = true;
                m_smokeRigidRouteBuildAsyncLaunchMs = Sys_Milliseconds();
                const RtPathTraceRigidRouteBuildSnapshot queuedRigidRouteSnapshot =
                    CaptureRigidRoutePayloadSnapshot();
                m_smokeRigidRouteBuildFuture.Start(
                    [queuedRigidRouteSnapshot]() {
                        return BuildRigidRouteBuffersTimedResult(queuedRigidRouteSnapshot);
                    });
            }
            if (!rigidRouteBuildAcceptedFromAsync)
            {
                m_smokeRigidRouteBuildAsyncCachedBuild = rigidRouteBuild;
                m_smokeRigidRouteBuildAsyncCachedGeometryUploadSignature = rigidRouteGeometryUploadSignature;
                m_smokeRigidRouteBuildAsyncCachedInstanceUploadSignature = rigidRouteInstanceUploadSignature;
                m_smokeRigidRouteBuildAsyncCachedGeometryUploadSignatureValid = rigidRouteGeometryUploadSignatureValid;
                m_smokeRigidRouteBuildAsyncCachedInstanceUploadSignatureValid = rigidRouteInstanceUploadSignatureValid;
                m_smokeRigidRouteBuildAsyncCachedGeneration = rigidRouteBuildGeneration;
                m_smokeRigidRouteBuildAsyncCachedBuildValid = true;
            }
            {
                OPTICK_EVENT("PT Rigid Route Refresh Transforms");
                if (RefreshSmokeRigidRouteBuildInstanceTransforms(rigidRouteBuild, rigidTlasPlan))
                {
                    rigidRouteInstanceUploadSignatureValid = false;
                }
            }
            rigidRouteBuildAsyncQueued = m_smokeRigidRouteBuildFuture.valid();
        }
        else
        {
            const int rigidRouteBuildStartMs = Sys_Milliseconds();
            {
                OPTICK_EVENT("PT Rigid Route Direct Build");
                rigidRouteBuild = m_smokeGeometryUniverse.BuildRigidRouteBuffers(rigidTlasPlan, materialTable.materialIds);
            }
            rigidRouteBuildMs = Sys_Milliseconds() - rigidRouteBuildStartMs;
        }
        if (r_pathTracingSmokeLog.GetInteger() != 0 && (m_smokeGeometryFrameIndex % 120ull) == 1ull)
        {
            const uint64_t rigidRouteGeometryBytes =
                rigidRouteBuild.vertices.size() * sizeof(PathTraceSmokeVertex) +
                rigidRouteBuild.indexes.size() * sizeof(uint32_t) +
                rigidRouteBuild.triangleMaterials.size() * sizeof(uint32_t) +
                rigidRouteBuild.triangleMaterialIndexes.size() * sizeof(uint32_t);
            const uint64_t rigidRouteInstanceBytes = rigidRouteBuild.instances.size() * sizeof(PathTraceRigidRouteInstance);
            common->Printf("PathTracePrimaryPass: PT rigid route buffers instances=%d uniqueMeshes=%d max=%d seen/cache=%d/%d prevXform/continuous=%d/%d verts/indexes/tris=%d/%d/%d bytes(geom/inst)=%llu/%llu buildMs=%d async/cache/queued=%d/%d/%d skipped nonRigid/missingMesh/missingBlas=%d/%d/%d missingMaterialIndex=%d\n",
                rigidRouteBuild.stats.emittedInstances,
                rigidRouteBuild.stats.emittedUniqueMeshes,
                rigidRouteMaxInstances,
                rigidRouteBuild.stats.emittedSeenThisFrame,
                rigidRouteBuild.stats.emittedFromCache,
                rigidRouteBuild.stats.previousTransformInstances,
                rigidRouteBuild.stats.transformContinuousInstances,
                rigidRouteBuild.stats.vertices,
                rigidRouteBuild.stats.indexes,
                rigidRouteBuild.stats.triangles,
                static_cast<unsigned long long>(rigidRouteGeometryBytes),
                static_cast<unsigned long long>(rigidRouteInstanceBytes),
                rigidRouteBuildMs,
                rigidRouteBuildAcceptedFromAsync ? 1 : 0,
                rigidRouteBuildAsyncCached ? 1 : 0,
                rigidRouteBuildAsyncQueued ? 1 : 0,
                rigidRouteBuild.stats.skippedNonRigid,
                rigidRouteBuild.stats.skippedMissingMesh,
                rigidRouteBuild.stats.skippedMissingBlas,
                rigidRouteBuild.stats.missingMaterialTableIndex);
        }
    }
    const int dynamicTexMatrixVertices = [&]() {
        OPTICK_EVENT("PT Dynamic Tex Matrix Apply");
        return ApplySmokeDynamicMaterialTexMatricesToVertices(
            dynamicVertexData,
            dynamicIndexData,
            materialTable.dynamicMaterialIndexes,
            dynamicMaterialRecords);
    }();
    int staticTexMatrixFirstVertex = -1;
    int staticTexMatrixLastVertex = -1;
    const int staticTexMatrixVertices = [&]() {
        OPTICK_EVENT("PT Static Route Tex Matrix Apply");
        return ApplySmokeDynamicMaterialTexMatricesToVertices(
            staticVertexFrameData,
            staticIndexCache,
            materialTable.staticMaterialIndexes,
            dynamicMaterialRecords,
            &staticTexMatrixFirstVertex,
            &staticTexMatrixLastVertex);
    }();
    // Rigid geometry is shared across instances. Its runtime texture matrix is
    // applied from PathTraceRigidRouteInstance.materialIndex during shader hit
    // reconstruction; baking one variant into these vertices is incorrect when
    // instances use different parm3/parm4 values.
    const int rigidRouteTexMatrixVertices = 0;
    if (rigidRouteTexMatrixVertices > 0)
    {
        rigidRouteGeometryUploadSignatureValid = false;
    }
    if (buildRigidRouteBuffers && !rigidRouteGeometryUploadSignatureValid)
    {
        OPTICK_EVENT("PT Rigid Route Geometry Upload Signature");
        rigidRouteGeometryUploadSignature = BuildRigidRouteGeometryUploadSignature(rigidRouteBuild);
        rigidRouteGeometryUploadSignatureValid = true;
    }
    if (buildRigidRouteBuffers && !rigidRouteInstanceUploadSignatureValid)
    {
        OPTICK_EVENT("PT Rigid Route Instance Upload Signature");
        rigidRouteInstanceUploadSignature = BuildRigidRouteInstanceUploadSignature(rigidRouteBuild);
        rigidRouteInstanceUploadSignatureValid = true;
    }
    if (r_pathTracingSmokeLog.GetInteger() != 0 &&
        (staticTexMatrixVertices > 0 || dynamicTexMatrixVertices > 0 || rigidRouteTexMatrixVertices > 0) &&
        (m_smokeGeometryFrameIndex % 120ull) == 1ull)
    {
        common->Printf(
            "PathTracePrimaryPass: RT smoke dynamic material tex matrices applied staticVerts=%d dynamicVerts=%d rigidRouteVerts=%d records=%d\n",
            staticTexMatrixVertices,
            dynamicTexMatrixVertices,
            rigidRouteTexMatrixVertices,
            static_cast<int>(dynamicMaterialRecords.size()));
    }
    if (r_pathTracingGeometryRenderedAttributeSurveyDump.
            GetInteger() != 0)
    {
        const int requestedPage =
            r_pathTracingGeometryRenderedAttributeSurveyDump.
                GetInteger();
        DumpRenderedAttributeSurvey(
            requestedPage,
            sceneSource,
            staticVertexFrameData,
            m_smokeGeometryUniverse.StaticSurfaceRecords(),
            m_sceneUniverse.Surfaces(),
            dynamicVertexData,
            currentCapturedSurfaceRecords);
        r_pathTracingGeometryRenderedAttributeSurveyDump.
            SetInteger(0);
    }
    {
        OPTICK_EVENT("PT Material Diagnostic Triggers");
        RunSmokeMaterialDiagnosticTriggers(materialDiagnosticDesc);
    }

    const bool staticBucketRouteRequested =
        staticBucketRouteMode !=
            RT_SMOKE_STATIC_BUCKET_ROUTE_DISABLED;
    const RtSmokeStaticBucketFramePublication
        staticBucketFramePublication =
            BuildSmokeStaticBucketFramePublication(
                viewDef,
                m_sceneUniverse,
                m_staticBucketGeometryUniverse,
                materialTable.materialIds,
                device,
                commandList,
                m_smokeGeometryFrameIndex,
                m_smokeSceneMapTimeStamp,
                staticBucketPortalSteps,
                staticBucketSecondaryOpticalPortalHalo,
                staticBucketConsumerProbe);
    ReleaseCompletedRetiredStaticBucketGpuResources(
        m_smokeGeometryFrameIndex);
    RtSmokeRetiredStaticBucketGpuResources
        retiredStaticBucketGpuResources;
    if (m_staticBucketGeometryUniverse.
            TakeRetiredStaticBucketGpuResources(
                retiredStaticBucketGpuResources))
    {
        PushRetiredStaticBucketGpuResources(
            retiredStaticBucketGpuResources,
            m_smokeGeometryFrameIndex);
    }
    // Diagnostic routes force the resident mask only to remove portal-policy
    // ambiguity. The explicit route-1 production checkpoint retains the
    // portal-active mask.
    const bool staticBucketRouteConsumerSupported =
        staticBucketProductionRoute ||
        staticBucketConsumerProbe;
    RtSmokeStaticBucketCutoverInput
        staticBucketCutoverInput;
    staticBucketCutoverInput.residentBuckets =
        staticBucketFramePublication.
            activePublication.residentBuckets;
    staticBucketCutoverInput.activeBuckets =
        staticBucketFramePublication.
            activePublication.activeBuckets;
    staticBucketCutoverInput.readyBuckets =
        staticBucketFramePublication.gpuStats.readyBuckets;
    staticBucketCutoverInput.tlasInstances =
        static_cast<int>(
            staticBucketFramePublication.
                activePublication.tlasInstances.size());
    staticBucketCutoverInput.routeRecords =
        static_cast<int>(
            staticBucketFramePublication.
                activePublication.routeRecords.size());
    staticBucketCutoverInput.requested =
        staticBucketRouteRequested;
    staticBucketCutoverInput.consumerSupported =
        staticBucketRouteConsumerSupported;
    staticBucketCutoverInput.publicationValid =
        staticBucketFramePublication.activePublication.valid;
    staticBucketCutoverInput.routeUploaded =
        staticBucketFramePublication.materialIndexUploaded;
    const RtSmokeStaticBucketCutoverPlan
        staticBucketCutoverPlan =
            BuildSmokeStaticBucketCutoverPlan(
                staticBucketCutoverInput);
    const bool staticBucketRouteAccepted =
        staticBucketCutoverPlan.accepted;
    if (staticBucketRouteRequested &&
        (staticBucketFramePublication.auditRequested ||
            (m_smokeGeometryFrameIndex % 120ull) == 1ull))
    {
        common->Printf(
            "PathTracePrimaryPass: GEO10 static bucket cutover mode=%d requested/accepted=%d/%d consumerSupported=%d productionRoute=%d primaryOpaqueProbe=%d secondaryIsolation=%d secondaryStage=%d fullResidentProbe=%d allResidentReady=%d publicationExact=%d buckets(active/resident/ready)=%d/%d/%d outputs(tlas/routes)=%zu/%zu materialIndexMissingActive=%d traversal=%s\n",
            staticBucketRouteMode,
            1,
            staticBucketRouteAccepted ? 1 : 0,
            staticBucketRouteConsumerSupported ? 1 : 0,
            staticBucketProductionRoute ? 1 : 0,
            staticBucketPrimaryOpaqueProbe ? 1 : 0,
            staticBucketCleanDiSecondaryIsolation ? 1 : 0,
            staticBucketCleanDiSecondaryIsolation
                ? staticBucketSecondaryProbeStage
                : 0,
            staticBucketFramePublication.
                activeMaskForcedFullResident
                    ? 1
                    : 0,
            staticBucketCutoverPlan.allResidentReady ? 1 : 0,
            staticBucketCutoverPlan.publicationExact ? 1 : 0,
            staticBucketFramePublication.
                activePublication.activeBuckets,
            staticBucketFramePublication.
                activePublication.residentBuckets,
            staticBucketFramePublication.gpuStats.readyBuckets,
            staticBucketFramePublication.
                activePublication.tlasInstances.size(),
            staticBucketFramePublication.
                activePublication.routeRecords.size(),
            staticBucketFramePublication.
                missingActiveMaterialIndexes,
            staticBucketRouteAccepted
                ? "bucket-resident-pool"
                : "monolithic");
    }

    RtSmokeEmissiveInventoryStats emissiveInventoryStats;
    const bool staticBucketEmissiveRouteAccepted =
        staticBucketRouteAccepted &&
        !staticBucketPrimaryOpaqueProbe;
    const int emissiveStartMs = Sys_Milliseconds();
    std::vector<PathTraceSmokeEmissiveTriangle> emissiveTriangles;
    std::vector<PathTraceSmokeEmissiveTriangle>
        previousEmissiveTriangles =
            m_sceneInputs.valid
                ? m_smokePreviousEmissiveTriangles
                : std::vector<
                    PathTraceSmokeEmissiveTriangle>();
    std::vector<PtSkinnedEmissiveAuditTriangle>
        skinnedEmissiveSourceTriangles;
    PtSkinnedEmissiveAuditInventory
        skinnedEmissiveInventory;
    std::vector<PathTraceSkinnedEmissiveGpuWork>
        skinnedEmissiveGpuWork;
    size_t skinnedEmissiveCurrentBase = 0;
    size_t skinnedEmissivePreviousBase = 0;
    const bool skinnedEmissivePublishValidation =
        r_pathTracingGeometrySkinnedEmissiveAudit.
            GetInteger() == 1;
    int skinnedEmissivePublishValidationForcedMaterials = 0;
    uint32 skinnedEmissivePublishValidationFallbackMaterial =
        UINT32_MAX;
    RtSmokeEmissiveDistributionBuild emissiveDistribution;
    std::vector<PathTraceSmokeLightCandidate> lightCandidates;
    std::vector<PathTraceDoomAnalyticLightCandidate> doomAnalyticLights;
    PathTraceDoomAnalyticLightGpuRemap doomAnalyticRemap;
    const int maxEmissiveRecords = idMath::ClampInt(1, RT_SMOKE_MAX_EMISSIVE_TRIANGLE_RECORDS, r_pathTracingEmissiveInventoryMaxTriangles.GetInteger());
    {
        OPTICK_EVENT("PT Emissive Inventory");
        emissiveTriangles = BuildSmokeEmissiveTriangleInventory(
            materialTable.materialIds,
            materialTable.materials,
            staticVertexCache,
            staticIndexCache,
            staticTriangleClassCache,
            materialTable.staticMaterialIndexes,
            staticBucketEmissiveRouteAccepted
                ? staticBucketFramePublication.geometryPack
                : nullptr,
            staticBucketEmissiveRouteAccepted
                ? staticBucketFramePublication.materialIndexes
                : nullptr,
            staticBucketEmissiveRouteAccepted
                ? &staticBucketFramePublication.activePublication
                : nullptr,
            dynamicVertexData,
            dynamicIndexData,
            dynamicTriangleClassData,
            materialTable.dynamicMaterialIndexes,
            dynamicTriangleInstanceData,
            dynamicTriangleIdentityData,
            RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE,
            RT_SMOKE_TRIANGLE_CLASS_MASK,
            static_cast<uint32_t>(RtSmokeSurfaceClass::SkinnedDeformed),
            maxEmissiveRecords,
            emissiveInventoryStats);
        if (staticBucketFramePublication.auditReady &&
            !staticBucketEmissiveRouteAccepted &&
            staticBucketFramePublication.geometryPack != nullptr &&
            staticBucketFramePublication.materialIndexes != nullptr)
        {
            const RtSmokeStaticBucketGeometryPack&
                staticBucketGeometryPack =
                    *staticBucketFramePublication.geometryPack;
            std::vector<
                RtSmokeStaticBucketMonolithicSurfaceBinding>
                monolithicStaticSurfaceBindings;
            monolithicStaticSurfaceBindings.reserve(
                m_smokeGeometryUniverse.
                    StaticSurfaceRecords().size());
            for (const RtSmokePersistentStaticSurfaceRecord& record :
                 m_smokeGeometryUniverse.StaticSurfaceRecords())
            {
                if (!record.valid ||
                    record.currentRange.triangles.offset < 0 ||
                    record.currentRange.triangles.count <= 0)
                {
                    continue;
                }
                const uint64_t bucketSurfaceKey =
                    record.bucketSurfaceKey != 0
                        ? record.bucketSurfaceKey
                        : record.key;
                RtSmokeStaticBucketMonolithicSurfaceBinding binding;
                binding.surfaceKey = bucketSurfaceKey;
                binding.triangleOffset =
                    static_cast<uint32_t>(
                        record.currentRange.triangles.offset);
                binding.triangleCount =
                    static_cast<uint32_t>(
                        record.currentRange.triangles.count);
                monolithicStaticSurfaceBindings.push_back(
                    binding);
            }
            const RtSmokeStaticBucketMonolithicPrimitiveRemap
                monolithicPrimitiveRemap =
                    BuildSmokeStaticBucketMonolithicPrimitiveRemap(
                        staticBucketGeometryPack,
                        staticBucketFramePublication.
                            portalActivePublication.
                                activeBucketMask,
                        monolithicStaticSurfaceBindings);
            const RtSmokeStaticBucketMonolithicStateOverlay
                monolithicStateOverlay =
                    BuildSmokeStaticBucketMonolithicStateOverlay(
                        staticBucketGeometryPack,
                        *staticBucketFramePublication.
                            materialIndexes,
                        monolithicPrimitiveRemap,
                        staticTriangleClassCache,
                        materialTable.staticMaterialIndexes,
                        RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF);

            RtSmokeEmissiveInventoryStats
                staticBucketEmissiveStats;
            std::vector<PathTraceSmokeEmissiveTriangle>
                staticBucketEmissiveTriangles;
            AppendSmokeStaticBucketEmissiveTriangleInventory(
                materialTable.materialIds,
                materialTable.materials,
                staticBucketGeometryPack,
                *staticBucketFramePublication.materialIndexes,
                staticBucketFramePublication.
                    portalActivePublication,
                RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE,
                RT_SMOKE_TRIANGLE_CLASS_MASK,
                static_cast<uint32_t>(
                    RtSmokeSurfaceClass::SkinnedDeformed),
                maxEmissiveRecords,
                staticBucketEmissiveTriangles,
                staticBucketEmissiveStats,
                &monolithicPrimitiveRemap.primitiveIndexes,
                &monolithicStateOverlay.triangleClasses,
                &monolithicStateOverlay.
                    triangleMaterialIndexes);
            const RtSmokeStaticBucketCanonicalAddressPlan
                staticBucketCanonicalAddressPlan =
                    BuildSmokeStaticBucketCanonicalAddressPlan(
                        staticBucketGeometryPack);
            int staticBucketReplayMapped = 0;
            int staticBucketReplayInvalidInstance = 0;
            int staticBucketReplayInvalidSurface = 0;
            int staticBucketReplayInvalidPrimitive = 0;
            int staticBucketReplayDuplicate = 0;
            int staticBucketReplayClassMismatch = 0;
            int staticBucketReplayMaterialMismatch = 0;
            std::unordered_set<uint32_t>
                staticBucketReplayPackedTriangles;
            for (const PathTraceSmokeEmissiveTriangle& record :
                 staticBucketEmissiveTriangles)
            {
                uint32_t surfaceRecordIndex = 0;
                if (!TryDecodeSmokeStaticBucketSurfaceBaseInstanceId(
                        record.instanceId,
                        surfaceRecordIndex))
                {
                    ++staticBucketReplayInvalidInstance;
                    continue;
                }
                if (surfaceRecordIndex >=
                    staticBucketGeometryPack.surfaceRecords.size())
                {
                    ++staticBucketReplayInvalidSurface;
                    continue;
                }
                const RtSmokeStaticBucketSurfaceRecord& surfaceRecord =
                    staticBucketGeometryPack.
                        surfaceRecords[surfaceRecordIndex];
                if ((surfaceRecord.flags &
                        RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_VALID_MASK) !=
                        RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_VALID)
                {
                    ++staticBucketReplayInvalidSurface;
                    continue;
                }
                const uint32_t sourceTriangleOffset =
                    surfaceRecord.flags >>
                    RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_SOURCE_TRIANGLE_SHIFT;
                if (record.primitiveIndex < sourceTriangleOffset)
                {
                    ++staticBucketReplayInvalidPrimitive;
                    continue;
                }
                const uint32_t localPrimitiveIndex =
                    record.primitiveIndex - sourceTriangleOffset;
                if (localPrimitiveIndex >=
                    surfaceRecord.triangleCount ||
                    surfaceRecord.triangleOffset >=
                        staticBucketGeometryPack.triangleClasses.size() ||
                    localPrimitiveIndex >=
                        staticBucketGeometryPack.triangleClasses.size() -
                            surfaceRecord.triangleOffset)
                {
                    ++staticBucketReplayInvalidPrimitive;
                    continue;
                }
                const uint32_t packedTriangleIndex =
                    surfaceRecord.triangleOffset +
                    localPrimitiveIndex;
                if (!staticBucketReplayPackedTriangles.
                    insert(packedTriangleIndex).second)
                {
                    ++staticBucketReplayDuplicate;
                    continue;
                }
                if (packedTriangleIndex >=
                        monolithicStateOverlay.triangleClasses.size() ||
                    packedTriangleIndex >=
                        monolithicStateOverlay.
                            triangleMaterialIndexes.size())
                {
                    ++staticBucketReplayInvalidPrimitive;
                    continue;
                }
                if (record.padding0 !=
                    monolithicStateOverlay.
                        triangleClasses[packedTriangleIndex])
                {
                    ++staticBucketReplayClassMismatch;
                }
                if (record.materialIndex !=
                    monolithicStateOverlay.
                        triangleMaterialIndexes[packedTriangleIndex])
                {
                    ++staticBucketReplayMaterialMismatch;
                }
                ++staticBucketReplayMapped;
            }
            const bool staticBucketReplayAddressExact =
                staticBucketCanonicalAddressPlan.exact &&
                staticBucketReplayMapped ==
                    static_cast<int>(
                        staticBucketEmissiveTriangles.size()) &&
                staticBucketReplayInvalidInstance == 0 &&
                staticBucketReplayInvalidSurface == 0 &&
                staticBucketReplayInvalidPrimitive == 0 &&
                staticBucketReplayDuplicate == 0 &&
                staticBucketReplayClassMismatch == 0 &&
                staticBucketReplayMaterialMismatch == 0;
            std::unordered_set<uint64_t>
                staticBucketEmissiveIdentities;
            std::unordered_set<uint64_t>
                monolithicStaticEmissiveIdentities;
            int staticBucketEmissiveZeroIdentities = 0;
            int staticBucketEmissiveIdentityCollisions = 0;
            int monolithicStaticEmissiveZeroIdentities = 0;
            int monolithicStaticEmissiveIdentityCollisions = 0;
            int portalMonolithicStaticEmissiveTriangles = 0;
            for (const PathTraceSmokeEmissiveTriangle& record :
                 emissiveTriangles)
            {
                if (record.instanceId != 0u)
                {
                    continue;
                }
                bool portalActivePrimitive = false;
                for (const
                     RtSmokeStaticBucketMonolithicSurfaceBinding&
                         activeSurface :
                     monolithicPrimitiveRemap.
                        activeMonolithicSurfaces)
                {
                    if (static_cast<uint64_t>(
                            record.primitiveIndex) >=
                            activeSurface.triangleOffset &&
                        static_cast<uint64_t>(
                            record.primitiveIndex) <
                            static_cast<uint64_t>(
                                activeSurface.triangleOffset) +
                                activeSurface.triangleCount)
                    {
                        portalActivePrimitive = true;
                        break;
                    }
                }
                if (!portalActivePrimitive)
                {
                    continue;
                }
                ++portalMonolithicStaticEmissiveTriangles;
                const uint64_t identity =
                    static_cast<uint64_t>(
                        record.identityHashLo) |
                    (static_cast<uint64_t>(
                        record.identityHashHi) << 32);
                if (identity == 0)
                {
                    ++monolithicStaticEmissiveZeroIdentities;
                }
                else if (!monolithicStaticEmissiveIdentities.
                    insert(identity).second)
                {
                    ++monolithicStaticEmissiveIdentityCollisions;
                }
            }
            for (const PathTraceSmokeEmissiveTriangle& record :
                 staticBucketEmissiveTriangles)
            {
                const uint64_t identity =
                    static_cast<uint64_t>(record.identityHashLo) |
                    (static_cast<uint64_t>(
                        record.identityHashHi) << 32);
                if (identity == 0)
                {
                    ++staticBucketEmissiveZeroIdentities;
                }
                else if (!staticBucketEmissiveIdentities.
                    insert(identity).second)
                {
                    ++staticBucketEmissiveIdentityCollisions;
                }
            }
            int staticBucketEmissiveMissingIdentities = 0;
            for (uint64_t identity :
                 monolithicStaticEmissiveIdentities)
            {
                if (staticBucketEmissiveIdentities.find(identity) ==
                    staticBucketEmissiveIdentities.end())
                {
                    ++staticBucketEmissiveMissingIdentities;
                }
            }
            int staticBucketEmissiveExtraIdentities = 0;
            for (uint64_t identity :
                 staticBucketEmissiveIdentities)
            {
                if (monolithicStaticEmissiveIdentities.find(identity) ==
                    monolithicStaticEmissiveIdentities.end())
                {
                    ++staticBucketEmissiveExtraIdentities;
                }
            }
            const bool staticBucketEmissiveIdentityExact =
                staticBucketFramePublication.
                    portalActivePublication.valid &&
                monolithicStaticEmissiveZeroIdentities == 0 &&
                staticBucketEmissiveZeroIdentities == 0 &&
                monolithicStaticEmissiveIdentityCollisions == 0 &&
                staticBucketEmissiveIdentityCollisions == 0 &&
                staticBucketEmissiveMissingIdentities == 0 &&
                staticBucketEmissiveExtraIdentities == 0 &&
                staticBucketReplayAddressExact &&
                monolithicPrimitiveRemap.exact &&
                monolithicStateOverlay.exact &&
                staticBucketEmissiveStats.
                    skippedInvalidMaterialTriangles == 0 &&
                staticBucketEmissiveStats.staticTriangles ==
                    portalMonolithicStaticEmissiveTriangles &&
                monolithicStaticEmissiveIdentities.size() ==
                    staticBucketEmissiveIdentities.size();
            common->Printf(
                "PathTracePrimaryPass: GEO10 static bucket emissive identity exact=%d portalPublicationValid=%d routes(active/resident)=%d/%zu surfaces(active/matched/missing/duplicate)=%zu/%d/%d/%d mapping(mapped/missing/invalid)=%d/%d/%d state(mapped/invalid/class/stage/nonStage/materialIndexMismatch)=%d/%d/%d/%d/%d/%d replay(records/mapped/invalidInstance/invalidSurface/invalidPrimitive/duplicate/class/material)=%zu/%d/%d/%d/%d/%d/%d/%d triangles(monolithicPortal/bucket/captured/invalid)=%d/%d/%d/%d identities(monolithic/bucket/zeroMonolithic/zeroBucket/collisionMonolithic/collisionBucket/missing/extra)=%zu/%zu/%d/%d/%d/%d/%d/%d traversal=portal-mask-canonical-surface-live-state-shadow-only\n",
                staticBucketEmissiveIdentityExact ? 1 : 0,
                staticBucketFramePublication.
                    portalActivePublication.valid
                        ? 1
                        : 0,
                staticBucketFramePublication.
                    portalActivePublication.activeBuckets,
                staticBucketFramePublication.
                    portalActivePublication.routeRecords.size(),
                static_cast<size_t>(
                    monolithicPrimitiveRemap.stats.
                        activeSurfaces),
                monolithicPrimitiveRemap.stats.
                    matchedSurfaces,
                monolithicPrimitiveRemap.stats.
                    missingSurfaces,
                monolithicPrimitiveRemap.stats.
                    duplicateBindings,
                monolithicPrimitiveRemap.stats.
                    mappedTriangles,
                monolithicPrimitiveRemap.stats.
                    missingTriangles,
                monolithicPrimitiveRemap.stats.
                    invalidTriangleIdentities,
                monolithicStateOverlay.stats.
                    mappedTriangles,
                monolithicStateOverlay.stats.
                    invalidMonolithicRanges,
                monolithicStateOverlay.stats.
                    classMismatches,
                monolithicStateOverlay.stats.
                    stageStateMismatches,
                monolithicStateOverlay.stats.
                    nonStageClassMismatches,
                monolithicStateOverlay.stats.
                    materialIndexMismatches,
                staticBucketEmissiveTriangles.size(),
                staticBucketReplayMapped,
                staticBucketReplayInvalidInstance,
                staticBucketReplayInvalidSurface,
                staticBucketReplayInvalidPrimitive,
                staticBucketReplayDuplicate,
                staticBucketReplayClassMismatch,
                staticBucketReplayMaterialMismatch,
                portalMonolithicStaticEmissiveTriangles,
                staticBucketEmissiveStats.staticTriangles,
                staticBucketEmissiveStats.capturedTriangles,
                staticBucketEmissiveStats.
                    skippedInvalidMaterialTriangles,
                monolithicStaticEmissiveIdentities.size(),
                staticBucketEmissiveIdentities.size(),
                monolithicStaticEmissiveZeroIdentities,
                staticBucketEmissiveZeroIdentities,
                monolithicStaticEmissiveIdentityCollisions,
                staticBucketEmissiveIdentityCollisions,
                staticBucketEmissiveMissingIdentities,
                staticBucketEmissiveExtraIdentities);
        }
        if (enableRigidRouteForMode && (cleanRtxdiDiSceneBuildRluEmissives || neeCacheSceneBuildRluEmissives))
        {
            AppendSmokeRigidRouteEmissiveTriangleInventory(
                materialTable.materialIds,
                materialTable.materials,
                rigidRouteBuild,
                RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE,
                maxEmissiveRecords,
                emissiveTriangles,
                emissiveInventoryStats);
        }
        if (!staticBucketEmissiveRouteAccepted &&
            r_pathTracingWorldStaticEmissives.GetInteger() != 0)
        {
            const int fullLevelStaticSupplementCap = idMath::ClampInt(0, maxEmissiveRecords, r_pathTracingWorldStaticEmissiveMaxTriangles.GetInteger());
            const int fullLevelStaticSupplementLimit = Min(maxEmissiveRecords, static_cast<int>(emissiveTriangles.size()) + fullLevelStaticSupplementCap);
            AppendSmokeWorldStaticEmissiveTriangleInventory(
                viewDef,
                materialTable.materialIds,
                materialTable.materials,
                RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE,
                SmokeSurfaceClassId(RtSmokeSurfaceClass::StaticWorld),
                fullLevelStaticSupplementLimit,
                emissiveTriangles,
                emissiveInventoryStats);
        }

        std::vector<PathTraceSmokeVertex>
            skinnedEmissivePlaceholderVertices =
                skinnedGpuScaffold.currentOutputVertices;
        std::vector<PathTraceSkinnedPreviousPosition>
            skinnedEmissivePlaceholderPrevious =
                skinnedGpuScaffold.previousPositions;
        for (const PtSkinnedHitRouteRecord& route :
            skinnedHitRouteUploadBuild.records)
        {
            if (!SmokeSkinnedCaptureInstanceWasOmitted(
                    currentSkinnedSurfaceRecords,
                    route.instanceKey))
            {
                continue;
            }
            const PtGeometrySourceRecord* source =
                m_smokeGeometryUniverse.
                    FindCanonicalSourceRecord(route.meshKey);
            if (!source ||
                source->payload.positions.size() !=
                    route.vertexCount ||
                source->payload.AttributeCount() !=
                    route.vertexCount ||
                route.outputVertexOffset >
                    skinnedEmissivePlaceholderVertices.size() ||
                route.vertexCount >
                    skinnedEmissivePlaceholderVertices.size() -
                        route.outputVertexOffset)
            {
                continue;
            }

            for (uint32_t vertexIndex = 0;
                 vertexIndex < route.vertexCount;
                 ++vertexIndex)
            {
                PtGeometrySourceAttribute attribute;
                if (!source->payload.DecodeAttribute(
                        vertexIndex,
                        attribute))
                {
                    continue;
                }
                const PathTraceSkinnedSourceVertex sourceVertex =
                    BuildSmokeSkinnedSourceVertex(
                        source->payload.positions[vertexIndex],
                        attribute);
                PathTraceSmokeVertex placeholder = {};
                for (int component = 0;
                     component < 4;
                     ++component)
                {
                    placeholder.position[component] =
                        sourceVertex.localPosition[component];
                    placeholder.normal[component] =
                        sourceVertex.localNormal[component];
                    placeholder.texCoord[component] =
                        sourceVertex.texCoord[component];
                    placeholder.color[component] =
                        sourceVertex.color[component];
                    placeholder.color2[component] =
                        sourceVertex.jointWeights[component];
                    placeholder.tangent[component] =
                        sourceVertex.localTangent[component];
                }
                skinnedEmissivePlaceholderVertices[
                    route.outputVertexOffset +
                    vertexIndex] = placeholder;
                if ((route.flags &
                        PT_SKINNED_HIT_ROUTE_HAS_PREVIOUS) !=
                        0u &&
                    route.previousPositionOffset !=
                        PT_SKINNED_HIT_ROUTE_INVALID_INDEX &&
                    route.previousPositionOffset <
                        skinnedEmissivePlaceholderPrevious.
                            size() &&
                    vertexIndex <
                        skinnedEmissivePlaceholderPrevious.
                            size() -
                            route.previousPositionOffset)
                {
                    PathTraceSkinnedPreviousPosition&
                        previous =
                            skinnedEmissivePlaceholderPrevious[
                                route.previousPositionOffset +
                                vertexIndex];
                    for (int component = 0;
                         component < 4;
                         ++component)
                    {
                        previous.previousPosition[component] =
                            sourceVertex.localPosition[component];
                    }
                }
            }

            for (uint32_t localPrimitive = 0;
                 localPrimitive < route.triangleCount;
                 ++localPrimitive)
            {
                const uint64 metadataIndex =
                    static_cast<uint64>(
                        route.triangleMetadataOffset) +
                    localPrimitive;
                if (metadataIndex >=
                    skinnedHitRouteUploadBuild.triangles.
                        size())
                {
                    continue;
                }
                const PtSkinnedHitRouteTriangle& triangle =
                    skinnedHitRouteUploadBuild.triangles[
                        static_cast<size_t>(metadataIndex)];
                const uint64 sourceIndexOffset =
                    static_cast<uint64>(
                        triangle.sourcePrimitiveIndex) *
                    3ull;
                if (sourceIndexOffset + 2ull >=
                    source->payload.indexes.size())
                {
                    continue;
                }

                PtSkinnedEmissiveAuditTriangle item = {};
                item.materialIndex = triangle.materialIndex;
                item.materialId = triangle.materialId;
                item.instanceId = route.shaderInstanceId;
                item.primitiveIndex =
                    triangle.sourcePrimitiveIndex;
                item.triangleClassAndFlags =
                    triangle.triangleClassAndFlags;
                item.identityHash =
                    triangle.emissiveIdentityHash;
                item.hasPrevious =
                    (route.flags &
                        PT_SKINNED_HIT_ROUTE_HAS_PREVIOUS) !=
                        0u &&
                    route.previousPositionOffset !=
                        PT_SKINNED_HIT_ROUTE_INVALID_INDEX;
                for (int corner = 0; corner < 3; ++corner)
                {
                    const uint32_t localVertex =
                        source->payload.indexes[
                            static_cast<size_t>(
                                sourceIndexOffset +
                                corner)];
                    item.currentVertexIndexes[corner] =
                        route.outputVertexOffset +
                        localVertex;
                    item.previousPositionIndexes[corner] =
                        item.hasPrevious
                            ? route.previousPositionOffset +
                                localVertex
                            : UINT32_MAX;
                }
                skinnedEmissiveSourceTriangles.push_back(
                    item);
            }
        }

        const int skinnedEmissiveCapacity =
            Max(
                0,
                maxEmissiveRecords -
                    static_cast<int>(
                        emissiveTriangles.size()));
        std::vector<PathTraceSmokeMaterial>
            skinnedEmissiveMaterialViews;
        std::vector<PtSkinnedEmissiveAuditTriangle>
            skinnedEmissiveValidationTriangleViews;
        const std::vector<PathTraceSmokeMaterial>*
            skinnedEmissiveMaterials =
                &materialTable.materials;
        const std::vector<PtSkinnedEmissiveAuditTriangle>*
            skinnedEmissiveTriangles =
                &skinnedEmissiveSourceTriangles;
        if (skinnedEmissivePublishValidation)
        {
            skinnedEmissiveMaterialViews =
                materialTable.materials;
            std::unordered_set<uint32_t>
                visitedMaterialIndexes;
            for (const PtSkinnedEmissiveAuditTriangle& source :
                skinnedEmissiveSourceTriangles)
            {
                if (!visitedMaterialIndexes.insert(
                        source.materialIndex).second ||
                    source.materialIndex >=
                        skinnedEmissiveMaterialViews.size() ||
                    source.materialIndex >=
                        materialTable.materialInfos.size())
                {
                    continue;
                }
                PathTraceSmokeMaterial& material =
                    skinnedEmissiveMaterialViews[
                        source.materialIndex];
                if ((material.flags &
                        RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE) !=
                    0u)
                {
                    continue;
                }
                const RtSmokeMaterialTextureInfo& info =
                    materialTable.materialInfos[
                        source.materialIndex];
                if (!info.emissive ||
                    !info.emissiveLightCandidate)
                {
                    continue;
                }
                material.flags |=
                    RT_SMOKE_MATERIAL_EMISSIVE |
                    RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE;
                material.emissiveColor[0] =
                    info.emissiveColor.x;
                material.emissiveColor[1] =
                    info.emissiveColor.y;
                material.emissiveColor[2] =
                    info.emissiveColor.z;
                material.emissiveColor[3] =
                    info.emissiveColor.w;
                ++skinnedEmissivePublishValidationForcedMaterials;
            }
            if (skinnedEmissivePublishValidationForcedMaterials ==
                    0 &&
                !skinnedEmissiveSourceTriangles.empty())
            {
                const uint32 materialIndex =
                    skinnedEmissiveSourceTriangles.front().
                        materialIndex;
                if (materialIndex <
                        skinnedEmissiveMaterialViews.size())
                {
                    PathTraceSmokeMaterial& material =
                        skinnedEmissiveMaterialViews[
                            materialIndex];
                    material.flags |=
                        RT_SMOKE_MATERIAL_EMISSIVE |
                        RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE;
                    material.emissiveColor[0] = 1.0f;
                    material.emissiveColor[1] = 0.5f;
                    material.emissiveColor[2] = 0.25f;
                    material.emissiveColor[3] = 1.0f;
                    skinnedEmissivePublishValidationFallbackMaterial =
                        materialIndex;
                    ++skinnedEmissivePublishValidationForcedMaterials;
                    skinnedEmissiveValidationTriangleViews =
                        skinnedEmissiveSourceTriangles;
                    for (PtSkinnedEmissiveAuditTriangle& source :
                        skinnedEmissiveValidationTriangleViews)
                    {
                        if (source.materialIndex == materialIndex)
                        {
                            source.triangleClassAndFlags &=
                                ~RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF;
                        }
                    }
                    skinnedEmissiveTriangles =
                        &skinnedEmissiveValidationTriangleViews;
                }
            }
            skinnedEmissiveMaterials =
                &skinnedEmissiveMaterialViews;
        }
        if (skinnedEmissiveCapacity > 0 &&
            !skinnedEmissiveSourceTriangles.empty())
        {
            const int skinnedEmissiveRecordLimit =
                skinnedEmissivePublishValidation
                    ? Min(skinnedEmissiveCapacity, 24)
                    : skinnedEmissiveCapacity;
            skinnedEmissiveInventory =
                BuildSmokeCanonicalSkinnedEmissiveAuditInventory(
                    materialTable.materialIds,
                    *skinnedEmissiveMaterials,
                    skinnedEmissivePlaceholderVertices,
                    skinnedEmissivePlaceholderPrevious,
                    *skinnedEmissiveTriangles,
                    RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE,
                    skinnedEmissiveRecordLimit);
            skinnedEmissiveCurrentBase =
                emissiveTriangles.size();
            emissiveTriangles.insert(
                emissiveTriangles.end(),
                skinnedEmissiveInventory.current.begin(),
                skinnedEmissiveInventory.current.end());
            if (skinnedEmissivePublishValidation)
            {
                skinnedEmissivePreviousBase =
                    previousEmissiveTriangles.size();
                previousEmissiveTriangles.insert(
                    previousEmissiveTriangles.end(),
                    skinnedEmissiveInventory.previous.begin(),
                    skinnedEmissiveInventory.previous.end());
            }
            skinnedEmissiveGpuWork.reserve(
                skinnedEmissiveInventory.current.size());
            for (size_t recordIndex = 0;
                 recordIndex <
                    skinnedEmissiveInventory.current.size();
                 ++recordIndex)
            {
                const uint32_t sourceTriangleIndex =
                    skinnedEmissiveInventory.
                        currentSourceTriangleIndexes[
                            recordIndex];
                const PtSkinnedEmissiveAuditTriangle& source =
                    skinnedEmissiveSourceTriangles[
                        sourceTriangleIndex];
                PathTraceSkinnedEmissiveGpuWork work = {};
                for (int corner = 0; corner < 3; ++corner)
                {
                    work.currentVertexIndexes[corner] =
                        source.currentVertexIndexes[corner];
                    work.previousPositionIndexes[corner] =
                        source.previousPositionIndexes[corner];
                }
                work.currentEmissiveIndex =
                    static_cast<uint32_t>(
                        skinnedEmissiveCurrentBase +
                        recordIndex);
                skinnedEmissiveGpuWork.push_back(work);
            }
            if (skinnedEmissivePublishValidation &&
                !skinnedEmissiveGpuWork.empty())
            {
                common->Printf(
                    "PathTracePrimaryPass: GEO09 skinned emissive publication validation inventory frame=%llu work=%zu forcedMaterials=%d cap=%d defaultProductionBehaviorChanged=0 validationLightInjection=1\n",
                    static_cast<unsigned long long>(
                        m_smokeGeometryFrameIndex),
                    skinnedEmissiveGpuWork.size(),
                    skinnedEmissivePublishValidationForcedMaterials,
                    skinnedEmissiveRecordLimit);
            }
        }
        const int fullLevelStaticEmissiveTriangles = emissiveInventoryStats.fullLevelStaticTriangles;
        const int routedRigidEmissiveTriangles = emissiveInventoryStats.routedRigidTriangles;
        const int routedRigidInstances = emissiveInventoryStats.routedRigidInstances;
        const int routedRigidSeenInstances = emissiveInventoryStats.routedRigidSeenInstances;
        const int routedRigidCacheInstances = emissiveInventoryStats.routedRigidCacheInstances;
        const int routedRigidEmissiveInstances = emissiveInventoryStats.routedRigidEmissiveInstances;
        const int routedRigidEmissiveSeenInstances = emissiveInventoryStats.routedRigidEmissiveSeenInstances;
        const int routedRigidEmissiveCacheInstances = emissiveInventoryStats.routedRigidEmissiveCacheInstances;
        const int routedRigidCapturedTriangles = emissiveInventoryStats.routedRigidCapturedTriangles;
        const int routedRigidCappedTriangles = emissiveInventoryStats.routedRigidCappedTriangles;
        const int routedRigidInvalidTriangles = emissiveInventoryStats.routedRigidInvalidTriangles;
        const int routedRigidNonEmissiveTriangles = emissiveInventoryStats.routedRigidNonEmissiveTriangles;
        const float routedRigidArea = emissiveInventoryStats.routedRigidArea;
        const float routedRigidWeightedLuminance = emissiveInventoryStats.routedRigidWeightedLuminance;
        const int runtimeInactiveEmissiveTrianglesBeforeStatsRebuild = emissiveInventoryStats.skippedRuntimeInactiveTriangles;
        emissiveInventoryStats = BuildSmokeEmissiveInventoryStatsForRecords(materialTable.materialIds, emissiveTriangles);
        emissiveInventoryStats.fullLevelStaticTriangles = fullLevelStaticEmissiveTriangles;
        emissiveInventoryStats.routedRigidTriangles = routedRigidEmissiveTriangles;
        emissiveInventoryStats.routedRigidInstances = routedRigidInstances;
        emissiveInventoryStats.routedRigidSeenInstances = routedRigidSeenInstances;
        emissiveInventoryStats.routedRigidCacheInstances = routedRigidCacheInstances;
        emissiveInventoryStats.routedRigidEmissiveInstances = routedRigidEmissiveInstances;
        emissiveInventoryStats.routedRigidEmissiveSeenInstances = routedRigidEmissiveSeenInstances;
        emissiveInventoryStats.routedRigidEmissiveCacheInstances = routedRigidEmissiveCacheInstances;
        emissiveInventoryStats.routedRigidCapturedTriangles = routedRigidCapturedTriangles;
        emissiveInventoryStats.routedRigidCappedTriangles = routedRigidCappedTriangles;
        emissiveInventoryStats.routedRigidInvalidTriangles = routedRigidInvalidTriangles;
        emissiveInventoryStats.routedRigidNonEmissiveTriangles = routedRigidNonEmissiveTriangles;
        emissiveInventoryStats.routedRigidArea = routedRigidArea;
        emissiveInventoryStats.routedRigidWeightedLuminance = routedRigidWeightedLuminance;
        emissiveInventoryStats.skippedRuntimeInactiveTriangles = runtimeInactiveEmissiveTrianglesBeforeStatsRebuild;
        FinalizeSmokeEmissiveTriangleSamplingFields(emissiveTriangles, emissiveInventoryStats);
        lightCandidates = BuildSmokeLightCandidateBufferRecords(emissiveInventoryStats);
    }
    const std::vector<PathTraceEmissiveLightRemap> emissiveLightRemap = [&]() {
        OPTICK_EVENT("PT Emissive Light Remap");
        return BuildSmokeCanonicalEmissiveLightRemap(emissiveTriangles, previousEmissiveTriangles);
    }();
    const int cleanRtxdiDiView = r_pathTracingCleanRtxdiDiView.GetInteger();
    const int cleanRtxdiDiResolveView =
        (cleanRtxdiDiView >= 18 && cleanRtxdiDiView <= 23) ? 16 : cleanRtxdiDiView;
    const bool cleanRtxdiDiRealAnalyticRoute =
        r_pathTracingCleanRtxdiDiEnable.GetInteger() != 0 &&
        r_pathTracingCleanRtxdiDiLightMode.GetInteger() == 1 &&
        (cleanRtxdiDiView == 8 || cleanRtxdiDiView == 12 || cleanRtxdiDiView == 13 || cleanRtxdiDiView == 14 || cleanRtxdiDiView == 15 || cleanRtxdiDiResolveView == 16);
    const int regirSceneLightDomain = idMath::ClampInt(0, 2, r_pathTracingReGIRLightDomain.GetInteger());
    const bool regirAnalyticLightUniverseRequested =
        r_pathTracingReGIREnable.GetInteger() != 0 &&
        r_pathTracingReGIRMode.GetInteger() != 0 &&
        (regirSceneLightDomain == 0 || regirSceneLightDomain == 2);
    const bool enableDoomAnalyticLightCandidates = r_pathTracingAnalyticLightCandidates.GetInteger() != 0;
    PathTraceDoomAnalyticLightBuildOptions doomAnalyticBuildOptions;
    if (cleanRtxdiDiRealAnalyticRoute)
    {
        doomAnalyticBuildOptions.forceBuild = true;
        doomAnalyticBuildOptions.requireProvenContinuity = r_pathTracingCleanRtxdiDiRequireProvenDoomLights.GetInteger() != 0;
    }
    if (regirAnalyticLightUniverseRequested)
    {
        doomAnalyticBuildOptions.forceBuild = true;
        doomAnalyticBuildOptions.stableReservoirOrder = true;
        doomAnalyticBuildOptions.includeOutOfSelectedArea = true;
        doomAnalyticBuildOptions.ignoreConfiguredCandidateCap = true;
    }
    {
        OPTICK_EVENT("PT Doom Analytic Lights");
        doomAnalyticLights = BuildPathTraceDoomAnalyticLightCandidates(viewDef, doomAnalyticBuildOptions);
        doomAnalyticRemap = GetPathTraceDoomAnalyticLightGpuRemap();
        if (cleanRtxdiDiRealAnalyticRoute && r_pathTracingCleanRtxdiDiBypassLightUniverse.GetInteger() != 0)
        {
            doomAnalyticRemap = BuildCleanRtxdiDiBypassLightUniverseRemap(viewDef, doomAnalyticLights);
        }
        else
        {
            g_cleanRtxdiDiBypassLightUniverse.Reset();
        }
        ApplyCleanRtxdiDiAnalyticDomainFreeze(viewDef, doomAnalyticLights, doomAnalyticRemap);
    }

    int doomAnalyticPortalRegionLightCount = 0;
    for (const PathTraceDoomAnalyticLightCandidate& light : doomAnalyticLights)
    {
        if (light.doomRadiusAndArea[2] > 0.5f)
        {
            break;
        }
        ++doomAnalyticPortalRegionLightCount;
    }
    if (r_pathTracingSmokeLog.GetInteger() != 0 && enableDoomAnalyticLightCandidates && (m_smokeGeometryFrameIndex % 120ull) == 1ull)
    {
        common->Printf("PathTracePrimaryPass: Doom analytic lights gpu=%d bytes=%d intensityScale=%.3f\n",
            static_cast<int>(doomAnalyticLights.size()),
            static_cast<int>(doomAnalyticLights.size() * sizeof(PathTraceDoomAnalyticLightCandidate)),
            idMath::ClampFloat(0.0f, 16.0f, r_pathTracingAnalyticLightIntensityScale.GetFloat()));
    }
    const int emissiveMs = Sys_Milliseconds() - emissiveStartMs;
    {
        OPTICK_EVENT("PT Remix Frame Prepare");
        PathTraceRemixFramePrepareDesc remixFramePrepareDesc;
        remixFramePrepareDesc.frameIndex = m_smokeGeometryFrameIndex;
        remixFramePrepareDesc.resetReasonFlags = m_frameResources.settings.resetReasonFlags;
        m_remixFramePrepare.BeginFrame(remixFramePrepareDesc);
    }
    const bool regirLightUniverseRequested =
        r_pathTracingReGIREnable.GetInteger() != 0 &&
        r_pathTracingReGIRMode.GetInteger() != 0;
    const bool cleanRtxdiDiRluRequested =
        cleanRtxdiDiRealAnalyticRoute &&
        r_pathTracingRemixLightUniverseUseForCleanRtxdiDi.GetInteger() != 0;
    const bool pdfNeeRluCurrentProducerRequested =
        r_pathTracingRestirPdfNeeVerifierEnable.GetInteger() != 0;
    const bool neeCacheRluCurrentProducerRequested =
        r_pathTracingNeeCacheEnable.GetInteger() != 0 &&
        r_pathTracingNeeCacheMode.GetInteger() != 0;
    const bool remixLightUniverseEnabled =
        r_pathTracingRemixLightUniverseEnable.GetInteger() != 0 ||
        regirLightUniverseRequested ||
        cleanRtxdiDiRluRequested ||
        pdfNeeRluCurrentProducerRequested ||
        neeCacheRluCurrentProducerRequested;
    const bool currentRluDenseProducerRequested =
        cleanRtxdiDiRluRequested ||
        pdfNeeRluCurrentProducerRequested ||
        neeCacheRluCurrentProducerRequested;
    const uint32_t remixLightUniverseDomain = static_cast<uint32_t>(
        idMath::ClampInt(0, 2, r_pathTracingRemixLightUniverseEnable.GetInteger() != 0
            ? r_pathTracingRemixLightUniverseDomain.GetInteger()
            : (currentRluDenseProducerRequested ? 2 : regirSceneLightDomain)));
    const bool remixLightUniverseStrictMapping =
        r_pathTracingRemixLightUniverseStrictRemixMapping.GetInteger() != 0;
    const bool remixLightUniverseIncludeAnalytic =
        !remixLightUniverseEnabled || remixLightUniverseDomain == 0u || remixLightUniverseDomain == 2u;
    const bool remixLightUniverseIncludeEmissive =
        !remixLightUniverseEnabled || remixLightUniverseDomain == 1u || remixLightUniverseDomain == 2u;
    const std::vector<PathTraceSmokeEmissiveTriangle> emptyEmissiveTriangles;
    const std::vector<PathTraceEmissiveLightRemap> emptyEmissiveRemap;
    const std::vector<PathTraceDoomAnalyticLightCandidate> emptyAnalyticLights;
    const std::vector<PathTraceDoomAnalyticLightCandidateIdentity> emptyAnalyticIdentities;
    const std::vector<PathTraceDoomAnalyticLightRemap> emptyAnalyticRemap;
    PathTraceRemixLightManagerPrepareDesc remixLightPrepareDesc;
    remixLightPrepareDesc.framePackage = &m_remixFramePrepare.GetObservationPackage();
    remixLightPrepareDesc.currentEmissiveTriangles = &(remixLightUniverseIncludeEmissive ? emissiveTriangles : emptyEmissiveTriangles);
    remixLightPrepareDesc.previousEmissiveTriangles = &(remixLightUniverseIncludeEmissive ? previousEmissiveTriangles : emptyEmissiveTriangles);
    remixLightPrepareDesc.emissiveRemap = &(remixLightUniverseIncludeEmissive ? emissiveLightRemap : emptyEmissiveRemap);
    remixLightPrepareDesc.currentAnalyticLights = &(remixLightUniverseIncludeAnalytic ? doomAnalyticLights : emptyAnalyticLights);
    remixLightPrepareDesc.previousAnalyticLights = &(remixLightUniverseIncludeAnalytic ? doomAnalyticRemap.previousCandidates : emptyAnalyticLights);
    remixLightPrepareDesc.currentAnalyticIdentities = &(remixLightUniverseIncludeAnalytic ? doomAnalyticRemap.currentCandidateIdentities : emptyAnalyticIdentities);
    remixLightPrepareDesc.previousAnalyticIdentities = &(remixLightUniverseIncludeAnalytic ? doomAnalyticRemap.previousCandidateIdentities : emptyAnalyticIdentities);
    remixLightPrepareDesc.analyticRemap = &(remixLightUniverseIncludeAnalytic ? doomAnalyticRemap.universeRemap : emptyAnalyticRemap);
    remixLightPrepareDesc.emissiveSampleCount = remixLightUniverseIncludeEmissive ? static_cast<uint32_t>(idMath::ClampInt(0, 64, r_pathTracingReservoirCandidateTrials.GetInteger())) : 0u;
    remixLightPrepareDesc.doomAnalyticSampleCount = remixLightUniverseIncludeAnalytic ? static_cast<uint32_t>(idMath::ClampInt(0, 256, r_pathTracingRestirPTAnalyticLightTrials.GetInteger())) : 0u;
    remixLightPrepareDesc.analyticStateCompatibilityTolerance = idMath::ClampFloat(0.0f, 1.0f, r_pathTracingRestirPTTemporalAnalyticLightChangeTolerance.GetFloat());
    remixLightPrepareDesc.domain = remixLightUniverseEnabled ? remixLightUniverseDomain : 2u;
    remixLightPrepareDesc.strictRemixMapping = remixLightUniverseStrictMapping;
    remixLightPrepareDesc.lightUniverseEnabled = remixLightUniverseEnabled;
    {
        OPTICK_EVENT("PT Remix Light Manager Prepare");
        m_remixLightManager.PrepareSceneData(remixLightPrepareDesc);
    }
    const std::vector<uint32_t>& restirLightManagerCurrentToPreviousRemap =
        m_remixLightManager.GetCurrentToPreviousMap();
    const std::vector<uint32_t>& restirLightManagerPreviousToCurrentRemap =
        m_remixLightManager.GetPreviousToCurrentMap();
    const std::vector<PathTraceUnifiedLightRecord>& restirLightManagerCurrentPayloadRecords =
        m_remixLightManager.GetCurrentLightPayloads();
    const std::vector<PathTraceUnifiedLightRecord>& restirLightManagerPreviousPayloadRecords =
        m_remixLightManager.GetPreviousLightPayloads();
    emissiveDistribution = [&]() {
        OPTICK_EVENT("PT Emissive Distribution");
        return BuildSmokeEmissiveDistribution(emissiveTriangles);
    }();
    const PathTraceUnifiedLightBuild unifiedLights = [&]() {
        OPTICK_EVENT("PT Unified Light Build");
        return BuildPathTraceUnifiedLights(
            emissiveTriangles,
            previousEmissiveTriangles,
            emissiveLightRemap,
            doomAnalyticLights,
            doomAnalyticRemap.previousCandidates,
            doomAnalyticRemap.currentCandidateIdentities,
            doomAnalyticRemap.previousCandidateIdentities,
            doomAnalyticRemap.universeRemap,
            idMath::ClampFloat(0.0f, 1.0f, r_pathTracingRestirPTTemporalAnalyticLightChangeTolerance.GetFloat()));
    }();
    const auto findEmissiveLightRecord =
        [](const std::vector<PathTraceUnifiedLightRecord>& records,
           uint32_t sourceIndex) -> uint32_t
        {
            for (uint32_t recordIndex = 0;
                 recordIndex < records.size();
                 ++recordIndex)
            {
                const PathTraceUnifiedLightRecord& record =
                    records[recordIndex];
                if (record.type ==
                        PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE &&
                    record.sourceIndex == sourceIndex)
                {
                    return recordIndex;
                }
            }
            return PT_SKINNED_EMISSIVE_GPU_INVALID_INDEX;
        };
    for (PathTraceSkinnedEmissiveGpuWork& work :
        skinnedEmissiveGpuWork)
    {
        if (work.currentEmissiveIndex >=
                emissiveLightRemap.size())
        {
            continue;
        }
        work.currentUnifiedIndex =
            findEmissiveLightRecord(
                unifiedLights.currentLights,
                work.currentEmissiveIndex);
        if (work.currentUnifiedIndex !=
            PT_SKINNED_EMISSIVE_GPU_INVALID_INDEX)
        {
            work.flags |=
                PT_SKINNED_EMISSIVE_GPU_WRITE_CURRENT_UNIFIED;
        }
        work.currentPayloadIndex =
            findEmissiveLightRecord(
                restirLightManagerCurrentPayloadRecords,
                work.currentEmissiveIndex);
        if (work.currentPayloadIndex !=
            PT_SKINNED_EMISSIVE_GPU_INVALID_INDEX)
        {
            work.flags |=
                PT_SKINNED_EMISSIVE_GPU_WRITE_CURRENT_PAYLOAD;
        }

        const PathTraceEmissiveLightRemap& remap =
            emissiveLightRemap[
                work.currentEmissiveIndex];
        const bool previousVertexIndexesValid =
            work.previousPositionIndexes[0] != UINT32_MAX &&
            work.previousPositionIndexes[1] != UINT32_MAX &&
            work.previousPositionIndexes[2] != UINT32_MAX;
        if ((remap.flags & RT_SMOKE_EMISSIVE_REMAP_VALID) == 0u ||
            remap.currentToPreviousIndex < 0 ||
            !previousVertexIndexesValid)
        {
            continue;
        }
        work.previousEmissiveIndex =
            static_cast<uint32_t>(
                remap.currentToPreviousIndex);
        if (work.previousEmissiveIndex >=
            previousEmissiveTriangles.size())
        {
            work.previousEmissiveIndex =
                PT_SKINNED_EMISSIVE_GPU_INVALID_INDEX;
            continue;
        }
        work.flags |=
            PT_SKINNED_EMISSIVE_GPU_WRITE_PREVIOUS;
        work.previousUnifiedIndex =
            findEmissiveLightRecord(
                unifiedLights.previousLights,
                work.previousEmissiveIndex);
        if (work.previousUnifiedIndex !=
            PT_SKINNED_EMISSIVE_GPU_INVALID_INDEX)
        {
            work.flags |=
                PT_SKINNED_EMISSIVE_GPU_WRITE_PREVIOUS_UNIFIED;
        }
        work.previousPayloadIndex =
            findEmissiveLightRecord(
                restirLightManagerPreviousPayloadRecords,
                work.previousEmissiveIndex);
        if (work.previousPayloadIndex !=
            PT_SKINNED_EMISSIVE_GPU_INVALID_INDEX)
        {
            work.flags |=
                PT_SKINNED_EMISSIVE_GPU_WRITE_PREVIOUS_PAYLOAD;
        }
    }
    for (PathTraceSkinnedEmissiveGpuWork& work :
        skinnedEmissiveGpuWork)
    {
        work.workItemCount =
            static_cast<uint32_t>(
                skinnedEmissiveGpuWork.size());
    }
    if (r_pathTracingSmokeLog.GetInteger() != 0 && (m_smokeGeometryFrameIndex % 120ull) == 1ull)
    {
        common->Printf("PathTracePrimaryPass: RT smoke emissive distribution entries=%d valid=%d zeroPdf=%d fallback=%d fallbackWeight=%.3f totalPdf=%.6f cvar=%d\n",
            static_cast<int>(emissiveDistribution.entries.size()),
            emissiveDistribution.valid ? 1 : 0,
            emissiveDistribution.zeroPdfSkipped,
            emissiveDistribution.fallbackIndex == UINT32_MAX ? -1 : static_cast<int>(emissiveDistribution.fallbackIndex),
            emissiveDistribution.fallbackWeight,
            emissiveDistribution.totalPdf,
            r_pathTracingEmissiveDistribution.GetInteger());
    }
    if (captureTiming.skinnedCaptureOmittedSurfaces > 0 &&
        canonicalSkinnedSourceOutputRoute &&
        m_smokeSkinnedCurrentOutputVertexBuffer)
    {
        // The cached build is only pre-capture admission evidence. Its CPU
        // route IDs and per-triangle material indexes belong to the frame in
        // which that view set was last observed. Rebuild those route records
        // from this frame's draw surfaces, rigid-route end, and material table
        // before creating the upload. This keeps alternating portal sets from
        // addressing stale current-frame slots.
        const uint64 currentOutputCapacity =
            m_smokeSkinnedCurrentOutputVertexBuffer->
                getDesc().byteSize;
        PtSkinnedHitRouteBuild currentFrameUploadBuild =
            BuildSmokeSkinnedHitRouteShadow(
                currentSkinnedSurfaceRecords,
                skinnedGpuScaffold.dispatchRecords,
                m_smokeGeometryUniverse,
                m_smokeSkinnedBlasStateTable,
                m_smokeSkinnedComparisonBlases,
                dynamicIndexData,
                dynamicTriangleClassData,
                dynamicTriangleMaterialData,
                materialTable.dynamicMaterialIndexes,
                materialTable,
                m_smokeSkinnedCurrentOutputVertexBuffer,
                skinnedGpuScaffold.previousPositions.size(),
                currentOutputCapacity,
                2ull + rigidRouteBuild.instances.size(),
                true,
                true);
        if (SmokeSkinnedHitRoutesMatchOmittedCapture(
                currentFrameUploadBuild.records,
                currentSkinnedSurfaceRecords,
                captureTiming.skinnedCaptureOmittedSurfaces))
        {
            skinnedHitRouteUploadBuild =
                std::move(currentFrameUploadBuild);
            skinnedHitRouteUploadBuildSignature =
                PtBuildSkinnedHitRouteGpuUpload(
                    skinnedHitRouteUploadBuild,
                    0).signature;
        }
        else
        {
            common->Printf(
                "PathTracePrimaryPass: GEO08 current-frame route refresh rejected frame=%llu omitted/current/routes/rejected=%d/%zu/%zu/%llu action=suppress-and-revoke\n",
                static_cast<unsigned long long>(
                    m_smokeGeometryFrameIndex),
                captureTiming.skinnedCaptureOmittedSurfaces,
                currentSkinnedSurfaceRecords.size(),
                currentFrameUploadBuild.records.size(),
                static_cast<unsigned long long>(
                    currentFrameUploadBuild.stats.rejected));
            skinnedHitRouteUploadBuild =
                PtSkinnedHitRouteBuild();
            skinnedHitRouteUploadBuildSignature = 0;
        }
    }
    const int bufferCreateStartMs = Sys_Milliseconds();
    const std::vector<PtSkinnedHitRouteRecord>
        skinnedHitRouteUploadCpuRecords =
            r_pathTracingGeometrySkinnedTlasCompare.
                    GetInteger() != 0
                ? skinnedHitRouteUploadBuild.records
                : std::vector<
                    PtSkinnedHitRouteRecord>();
    const PtSkinnedHitRouteGpuUpload skinnedHitRouteGpuUpload =
        PtBuildSkinnedHitRouteGpuUpload(
            skinnedHitRouteUploadBuild,
            static_cast<uint32_t>(
                2ull + rigidRouteBuild.instances.size()));
    RtPathTraceCpuWorkGeneration rigidRouteSideBufferGeneration;
    rigidRouteSideBufferGeneration.frameIndex = 0;
    rigidRouteSideBufferGeneration.sceneGeneration = m_smokeSceneUniverseStaticBuildGeneration;
    rigidRouteSideBufferGeneration.geometryGeneration = sceneUniverseGeneration;
    rigidRouteSideBufferGeneration.materialGeneration = materialTableSignature;
    rigidRouteSideBufferGeneration.lightGeneration =
        rigidTlasPlanValid ? rigidTlasPlan.tlasInstanceSignature : rigidTlasPlanInputToken;
    const bool asyncRigidRouteSideBufferRing =
        asyncBvhFramePlanning &&
        buildRigidRouteBuffers;
    int rigidRouteSideBufferWriteSlot = -1;
    if (asyncRigidRouteSideBufferRing)
    {
        rigidRouteSideBufferWriteSlot = m_smokeRigidRouteSideBufferReadSlot >= 0
            ? (m_smokeRigidRouteSideBufferReadSlot + 1) % RT_SMOKE_RIGID_ROUTE_SIDE_BUFFER_SLOTS
            : m_smokeRigidRouteSideBufferWriteSlot;
        if (rigidRouteSideBufferWriteSlot == m_smokeRigidRouteSideBufferReadSlot)
        {
            rigidRouteSideBufferWriteSlot =
                (rigidRouteSideBufferWriteSlot + 1) % RT_SMOKE_RIGID_ROUTE_SIDE_BUFFER_SLOTS;
        }
        m_smokeRigidRouteSideBufferWriteSlot = rigidRouteSideBufferWriteSlot;
    }
    bool skipRigidRouteSideBufferUpload = false;
    bool skipRigidRouteInstanceBufferUpload = false;
    if (asyncRigidRouteSideBufferRing && rigidRouteSideBufferWriteSlot >= 0)
    {
        const RtSmokeRigidRouteSideBufferSlot& sideSlot =
            m_smokeRigidRouteSideBufferSlots[rigidRouteSideBufferWriteSlot];
        skipRigidRouteSideBufferUpload =
            SmokeRigidRouteSideBufferSlotCanSkipGeometryUpload(
                sideSlot,
                rigidRouteBuild,
                rigidRouteGeometryUploadSignature);
        skipRigidRouteInstanceBufferUpload =
            SmokeRigidRouteSideBufferSlotCanSkipInstanceUpload(
                sideSlot,
                rigidRouteBuild,
                rigidRouteInstanceUploadSignature);
    }

    RtSmokeSceneBufferCreateDesc bufferCreateDesc;
    {
        OPTICK_EVENT("PT Scene Buffer Desc Build");
    bufferCreateDesc.device = device;
    bufferCreateDesc.existingBuffers.staticVertexBuffer = m_smokeStaticVertexBuffer;
    bufferCreateDesc.existingBuffers.staticIndexBuffer = m_smokeStaticIndexBuffer;
    bufferCreateDesc.existingBuffers.staticTriangleClassBuffer = m_smokeStaticTriangleClassBuffer;
    bufferCreateDesc.existingBuffers.staticTriangleMaterialBuffer = m_smokeStaticTriangleMaterialBuffer;
    bufferCreateDesc.existingBuffers.staticTriangleMaterialIndexBuffer = m_smokeStaticTriangleMaterialIndexBuffer;
    bufferCreateDesc.existingBuffers.previousStaticVertexBuffer = m_smokePreviousStaticVertexBuffer;
    bufferCreateDesc.existingBuffers.previousStaticIndexBuffer = m_smokePreviousStaticIndexBuffer;
    bufferCreateDesc.existingBuffers.previousStaticTriangleClassBuffer = m_smokePreviousStaticTriangleClassBuffer;
    bufferCreateDesc.existingBuffers.previousStaticTriangleMaterialBuffer = m_smokePreviousStaticTriangleMaterialBuffer;
    bufferCreateDesc.existingBuffers.previousStaticTriangleMaterialIndexBuffer = m_smokePreviousStaticTriangleMaterialIndexBuffer;
    bufferCreateDesc.existingBuffers.dynamicVertexBuffer = m_smokeDynamicVertexBuffer;
    bufferCreateDesc.existingBuffers.dynamicIndexBuffer = m_smokeDynamicIndexBuffer;
    bufferCreateDesc.existingBuffers.dynamicTriangleClassBuffer = m_smokeDynamicTriangleClassBuffer;
    bufferCreateDesc.existingBuffers.dynamicTriangleMaterialBuffer = m_smokeDynamicTriangleMaterialBuffer;
    bufferCreateDesc.existingBuffers.dynamicTriangleMaterialIndexBuffer = m_smokeDynamicTriangleMaterialIndexBuffer;
    bufferCreateDesc.existingBuffers.materialTableBuffer = m_smokeMaterialTableBuffer;
    bufferCreateDesc.existingBuffers.materialFeatureBuffer = m_smokeMaterialFeatureBuffer;
    bufferCreateDesc.existingBuffers.materialFeatureParameterBuffer = m_smokeMaterialFeatureParameterBuffer;
    bufferCreateDesc.existingBuffers.dynamicMaterialBuffer = m_smokeDynamicMaterialBuffer;
    bufferCreateDesc.existingBuffers.emissiveTriangleBuffer = m_smokeEmissiveTriangleBuffer;
    bufferCreateDesc.existingBuffers.previousEmissiveTriangleBuffer = m_smokePreviousEmissiveTriangleBuffer;
    bufferCreateDesc.existingBuffers.emissiveRemapBuffer = m_smokeEmissiveRemapBuffer;
    bufferCreateDesc.existingBuffers.emissiveDistributionBuffer = m_smokeEmissiveDistributionBuffer;
    bufferCreateDesc.existingBuffers.lightCandidateBuffer = m_smokeLightCandidateBuffer;
    bufferCreateDesc.existingBuffers.doomAnalyticLightBuffer = m_smokeDoomAnalyticLightBuffer;
    bufferCreateDesc.existingBuffers.doomAnalyticPreviousLightBuffer = m_smokeDoomAnalyticPreviousLightBuffer;
    bufferCreateDesc.existingBuffers.doomAnalyticCurrentIdentityBuffer = m_smokeDoomAnalyticCurrentIdentityBuffer;
    bufferCreateDesc.existingBuffers.doomAnalyticPreviousIdentityBuffer = m_smokeDoomAnalyticPreviousIdentityBuffer;
    bufferCreateDesc.existingBuffers.doomAnalyticRemapBuffer = m_smokeDoomAnalyticRemapBuffer;
    bufferCreateDesc.existingBuffers.unifiedLightBuffer = m_smokeUnifiedLightBuffer;
    bufferCreateDesc.existingBuffers.unifiedPreviousLightBuffer = m_smokeUnifiedPreviousLightBuffer;
    bufferCreateDesc.existingBuffers.unifiedLightRemapBuffer = m_smokeUnifiedLightRemapBuffer;
    bufferCreateDesc.existingBuffers.restirLightManagerCurrentToPreviousBuffer = m_smokeRestirLightManagerCurrentToPreviousBuffer;
    bufferCreateDesc.existingBuffers.restirLightManagerPreviousToCurrentBuffer = m_smokeRestirLightManagerPreviousToCurrentBuffer;
    bufferCreateDesc.existingBuffers.restirLightManagerCurrentPayloadBuffer = m_smokeRestirLightManagerCurrentPayloadBuffer;
    bufferCreateDesc.existingBuffers.restirLightManagerPreviousPayloadBuffer = m_smokeRestirLightManagerPreviousPayloadBuffer;
    if (asyncRigidRouteSideBufferRing && rigidRouteSideBufferWriteSlot >= 0)
    {
        const RtSmokeRigidRouteSideBufferSlot& sideSlot =
            m_smokeRigidRouteSideBufferSlots[rigidRouteSideBufferWriteSlot];
        bufferCreateDesc.existingBuffers.rigidRouteVertexBuffer = sideSlot.vertexBuffer;
        bufferCreateDesc.existingBuffers.rigidRouteIndexBuffer = sideSlot.indexBuffer;
        bufferCreateDesc.existingBuffers.rigidRouteTriangleMaterialBuffer = sideSlot.triangleMaterialBuffer;
        bufferCreateDesc.existingBuffers.rigidRouteTriangleMaterialIndexBuffer = sideSlot.triangleMaterialIndexBuffer;
        bufferCreateDesc.existingBuffers.rigidRouteInstanceBuffer = sideSlot.instanceBuffer;
    }
    else
    {
        bufferCreateDesc.existingBuffers.rigidRouteVertexBuffer = m_smokeRigidRouteVertexBuffer;
        bufferCreateDesc.existingBuffers.rigidRouteIndexBuffer = m_smokeRigidRouteIndexBuffer;
        bufferCreateDesc.existingBuffers.rigidRouteTriangleMaterialBuffer = m_smokeRigidRouteTriangleMaterialBuffer;
        bufferCreateDesc.existingBuffers.rigidRouteTriangleMaterialIndexBuffer = m_smokeRigidRouteTriangleMaterialIndexBuffer;
        bufferCreateDesc.existingBuffers.rigidRouteInstanceBuffer = m_smokeRigidRouteInstanceBuffer;
    }
    bufferCreateDesc.existingBuffers.skinnedHitRouteRecordBuffer = m_smokeSkinnedHitRouteRecordBuffer;
    bufferCreateDesc.existingBuffers.skinnedHitRouteTriangleBuffer = m_smokeSkinnedHitRouteTriangleBuffer;
    bufferCreateDesc.existingBuffers.skinnedSourceVertexBuffer = m_smokeSkinnedSourceVertexBuffer;
    bufferCreateDesc.existingBuffers.skinnedCurrentOutputVertexBuffer = m_smokeSkinnedCurrentOutputVertexBuffer;
    bufferCreateDesc.existingSkinnedOutputStorageGeneration =
        m_smokeSkinnedOutputBufferGeneration;
    bufferCreateDesc.existingBuffers.skinnedPreviousPositionBuffer = m_smokeSkinnedPreviousPositionBuffer;
    bufferCreateDesc.existingBuffers.skinnedSurfaceDispatchBuffer = m_smokeSkinnedSurfaceDispatchBuffer;
    bufferCreateDesc.existingBuffers.skinnedTriangleDispatchIndexBuffer = m_smokeSkinnedTriangleDispatchIndexBuffer;
    bufferCreateDesc.existingBuffers.skinnedCurrentJointMatrixBuffer = m_smokeSkinnedCurrentJointMatrixBuffer;
    bufferCreateDesc.existingBuffers.skinnedPreviousJointMatrixBuffer = m_smokeSkinnedPreviousJointMatrixBuffer;
    bufferCreateDesc.existingBuffers.skinnedEmissiveWorkBuffer =
        m_smokeSkinnedEmissiveWorkBuffer;
    bufferCreateDesc.staticVertexBytes = staticVertexCache.size() * sizeof(staticVertexCache[0]);
    bufferCreateDesc.staticIndexBytes = staticIndexCache.size() * sizeof(staticIndexCache[0]);
    bufferCreateDesc.staticTriangleClassBytes = staticTriangleClassCache.size() * sizeof(staticTriangleClassCache[0]);
    bufferCreateDesc.staticTriangleMaterialBytes = staticTriangleMaterialCache.size() * sizeof(staticTriangleMaterialCache[0]);
    bufferCreateDesc.staticTriangleMaterialIndexBytes = materialTable.staticMaterialIndexes.size() * sizeof(materialTable.staticMaterialIndexes[0]);
    bufferCreateDesc.previousStaticVertexBytes = previousStaticVertexCache.size() * sizeof(previousStaticVertexCache[0]);
    bufferCreateDesc.previousStaticIndexBytes = previousStaticIndexCache.size() * sizeof(previousStaticIndexCache[0]);
    bufferCreateDesc.previousStaticTriangleClassBytes = previousStaticTriangleClassCache.size() * sizeof(previousStaticTriangleClassCache[0]);
    bufferCreateDesc.previousStaticTriangleMaterialBytes = previousStaticTriangleMaterialCache.size() * sizeof(previousStaticTriangleMaterialCache[0]);
    bufferCreateDesc.previousStaticTriangleMaterialIndexBytes = previousStaticTriangleMaterialIndexCache.size() * sizeof(previousStaticTriangleMaterialIndexCache[0]);
    bufferCreateDesc.dynamicVertexBytes = dynamicVertexData.size() * sizeof(dynamicVertexData[0]);
    bufferCreateDesc.dynamicIndexBytes = dynamicIndexData.size() * sizeof(dynamicIndexData[0]);
    bufferCreateDesc.dynamicTriangleClassBytes = dynamicTriangleClassData.size() * sizeof(dynamicTriangleClassData[0]);
    bufferCreateDesc.dynamicTriangleMaterialBytes = dynamicTriangleMaterialData.size() * sizeof(dynamicTriangleMaterialData[0]);
    bufferCreateDesc.dynamicTriangleMaterialIndexBytes = materialTable.dynamicMaterialIndexes.size() * sizeof(materialTable.dynamicMaterialIndexes[0]);
    bufferCreateDesc.materialTableBytes = gpuMaterialTableMaterials.size() * sizeof(gpuMaterialTableMaterials[0]);
    bufferCreateDesc.materialFeatureBytes = materialTable.materialFeatures.size() * sizeof(materialTable.materialFeatures[0]);
    bufferCreateDesc.materialFeatureParameterBytes = materialTable.materialFeatureParameters.size() * sizeof(materialTable.materialFeatureParameters[0]);
    bufferCreateDesc.dynamicMaterialBytes = dynamicMaterialRecords.size() * sizeof(dynamicMaterialRecords[0]);
    bufferCreateDesc.emissiveTriangleBytes = emissiveTriangles.size() * sizeof(emissiveTriangles[0]);
    bufferCreateDesc.previousEmissiveTriangleBytes = previousEmissiveTriangles.size() * sizeof(PathTraceSmokeEmissiveTriangle);
    bufferCreateDesc.emissiveRemapBytes = emissiveLightRemap.size() * sizeof(PathTraceEmissiveLightRemap);
    bufferCreateDesc.emissiveDistributionBytes = emissiveDistribution.entries.size() * sizeof(PathTraceEmissiveDistributionEntry);
    bufferCreateDesc.lightCandidateBytes = lightCandidates.size() * sizeof(lightCandidates[0]);
    bufferCreateDesc.doomAnalyticLightBytes = doomAnalyticLights.size() * sizeof(PathTraceDoomAnalyticLightCandidate);
    bufferCreateDesc.doomAnalyticPreviousLightBytes = doomAnalyticRemap.previousCandidates.size() * sizeof(PathTraceDoomAnalyticLightCandidate);
    bufferCreateDesc.doomAnalyticCurrentIdentityBytes = doomAnalyticRemap.currentCandidateIdentities.size() * sizeof(PathTraceDoomAnalyticLightCandidateIdentity);
    bufferCreateDesc.doomAnalyticPreviousIdentityBytes = doomAnalyticRemap.previousCandidateIdentities.size() * sizeof(PathTraceDoomAnalyticLightCandidateIdentity);
    bufferCreateDesc.doomAnalyticRemapBytes = doomAnalyticRemap.universeRemap.size() * sizeof(PathTraceDoomAnalyticLightRemap);
    bufferCreateDesc.unifiedLightBytes = unifiedLights.currentLights.size() * sizeof(PathTraceUnifiedLightRecord);
    bufferCreateDesc.unifiedPreviousLightBytes = unifiedLights.previousLights.size() * sizeof(PathTraceUnifiedLightRecord);
    bufferCreateDesc.unifiedLightRemapBytes = unifiedLights.currentToPreviousRemap.size() * sizeof(uint32_t);
    bufferCreateDesc.restirLightManagerCurrentToPreviousBytes = restirLightManagerCurrentToPreviousRemap.size() * sizeof(uint32_t);
    bufferCreateDesc.restirLightManagerPreviousToCurrentBytes = restirLightManagerPreviousToCurrentRemap.size() * sizeof(uint32_t);
    bufferCreateDesc.restirLightManagerCurrentPayloadBytes = restirLightManagerCurrentPayloadRecords.size() * sizeof(PathTraceUnifiedLightRecord);
    bufferCreateDesc.restirLightManagerPreviousPayloadBytes = restirLightManagerPreviousPayloadRecords.size() * sizeof(PathTraceUnifiedLightRecord);
    bufferCreateDesc.rigidRouteVertexBytes = rigidRouteBuild.vertices.size() * sizeof(PathTraceSmokeVertex);
    bufferCreateDesc.rigidRouteIndexBytes = rigidRouteBuild.indexes.size() * sizeof(uint32_t);
    bufferCreateDesc.rigidRouteTriangleMaterialBytes = rigidRouteBuild.triangleMaterials.size() * sizeof(uint32_t);
    bufferCreateDesc.rigidRouteTriangleMaterialIndexBytes = rigidRouteBuild.triangleMaterialIndexes.size() * sizeof(uint32_t);
    bufferCreateDesc.rigidRouteInstanceBytes = rigidRouteBuild.instances.size() * sizeof(PathTraceRigidRouteInstance);
    bufferCreateDesc.skinnedHitRouteRecordBytes = skinnedHitRouteGpuUpload.records.size() * sizeof(PathTraceSkinnedHitRouteGpuRecord);
    bufferCreateDesc.skinnedHitRouteTriangleBytes = skinnedHitRouteGpuUpload.triangles.size() * sizeof(PathTraceSkinnedHitRouteGpuTriangle);
    bufferCreateDesc.skinnedSourceVertexBytes = skinnedGpuScaffold.sourceVertices.size() * sizeof(PathTraceSkinnedSourceVertex);
    bufferCreateDesc.skinnedCurrentOutputVertexBytes = skinnedGpuScaffold.currentOutputVertices.size() * sizeof(PathTraceSmokeVertex);
    bufferCreateDesc.skinnedOutputStorageGeneration =
        canonicalSkinnedSourceOutputRoute
            ? m_smokeSkinnedOutputAllocator.Stats().
                storageGeneration
            : 0;
    bufferCreateDesc.skinnedPreviousPositionBytes = skinnedGpuScaffold.previousPositions.size() * sizeof(PathTraceSkinnedPreviousPosition);
    bufferCreateDesc.skinnedSurfaceDispatchBytes = skinnedGpuScaffold.dispatchRecords.size() * sizeof(PathTraceSkinnedSurfaceDispatchRecord);
    bufferCreateDesc.skinnedTriangleDispatchIndexBytes = skinnedGpuScaffold.dynamicTriangleDispatchIndexes.size() * sizeof(uint32_t);
    bufferCreateDesc.skinnedCurrentJointMatrixBytes =
        jointCacheStage.ready
            ? static_cast<size_t>(
                jointCacheStage.planner.usedBytes)
            : skinnedGpuScaffold.currentJointMatrices.size() *
                sizeof(PathTraceSkinnedJointMatrix);
    bufferCreateDesc.skinnedPreviousJointMatrixBytes = skinnedGpuScaffold.previousJointMatrices.size() * sizeof(PathTraceSkinnedJointMatrix);
    bufferCreateDesc.skinnedEmissiveWorkBytes =
        skinnedEmissiveGpuWork.size() *
        sizeof(PathTraceSkinnedEmissiveGpuWork);
    }
    RtSmokeSceneBufferCreateResult bufferCreateResult;
    {
        OPTICK_EVENT("PT Create Scene Buffers");
        bufferCreateResult = CreateSmokeSceneBuffers(bufferCreateDesc);
    }
    if (!bufferCreateResult.Succeeded())
    {
        FinalizeSmokeSkinnedGpuFunnel(skinnedGpuScaffold, false);
        if (r_pathTracingGpuSkinningParityDump.GetInteger() != 0)
        {
            DumpSmokeSkinnedGpuFunnel(
                skinnedGpuScaffold,
                jointCacheStage,
                skinnedHistoryAudit,
                skinnedOutputAudit,
                currentSkinnedSurfaceRecords,
                m_smokeGeometryUniverse,
                gpuSkinningMode,
                m_smokeGeometryFrameIndex);
            common->Printf("PathTracePrimaryPass: PT GPU skinning parity readback unavailable because scene buffer allocation failed\n");
            r_pathTracingGpuSkinningParityDump.SetInteger(0);
        }
        common->Printf("PathTracePrimaryPass: %s\n", bufferCreateResult.errorMessage ? bufferCreateResult.errorMessage : "failed to create RT smoke geometry buffers");
        return;
    }
    RtSmokeSceneBufferHandles smokeBuffers = bufferCreateResult.buffers;
    if (asyncRigidRouteSideBufferRing && rigidRouteSideBufferWriteSlot >= 0)
    {
        RtSmokeRigidRouteSideBufferSlot& sideSlot =
            m_smokeRigidRouteSideBufferSlots[rigidRouteSideBufferWriteSlot];
        const bool rigidRouteSideBufferHandlesChanged =
            !SmokeRigidRouteSideBufferSlotHandlesMatch(sideSlot, smokeBuffers);
        if (skipRigidRouteSideBufferUpload && rigidRouteSideBufferHandlesChanged)
        {
            skipRigidRouteSideBufferUpload = false;
        }
        if (skipRigidRouteInstanceBufferUpload && rigidRouteSideBufferHandlesChanged)
        {
            skipRigidRouteInstanceBufferUpload = false;
        }
        sideSlot.vertexBuffer = smokeBuffers.rigidRouteVertexBuffer;
        sideSlot.indexBuffer = smokeBuffers.rigidRouteIndexBuffer;
        sideSlot.triangleMaterialBuffer = smokeBuffers.rigidRouteTriangleMaterialBuffer;
        sideSlot.triangleMaterialIndexBuffer = smokeBuffers.rigidRouteTriangleMaterialIndexBuffer;
        sideSlot.instanceBuffer = smokeBuffers.rigidRouteInstanceBuffer;
        sideSlot.generation = rigidRouteSideBufferGeneration;
        sideSlot.generationValid = true;
        if (rigidRouteSideBufferHandlesChanged)
        {
            sideSlot.geometryUploadSignature = 0;
            sideSlot.instanceUploadSignature = 0;
            sideSlot.geometryUploadSignatureValid = false;
            sideSlot.instanceUploadSignatureValid = false;
        }
    }
    nvrhi::BufferHandle smokeStaticVertexBuffer = smokeBuffers.staticVertexBuffer;
    nvrhi::BufferHandle smokeStaticIndexBuffer = smokeBuffers.staticIndexBuffer;
    nvrhi::BufferHandle smokeStaticTriangleClassBuffer = smokeBuffers.staticTriangleClassBuffer;
    nvrhi::BufferHandle smokeStaticTriangleMaterialBuffer = smokeBuffers.staticTriangleMaterialBuffer;
    nvrhi::BufferHandle smokeStaticTriangleMaterialIndexBuffer = smokeBuffers.staticTriangleMaterialIndexBuffer;
    nvrhi::BufferHandle smokePreviousStaticVertexBuffer = smokeBuffers.previousStaticVertexBuffer;
    nvrhi::BufferHandle smokePreviousStaticIndexBuffer = smokeBuffers.previousStaticIndexBuffer;
    nvrhi::BufferHandle smokePreviousStaticTriangleClassBuffer = smokeBuffers.previousStaticTriangleClassBuffer;
    nvrhi::BufferHandle smokePreviousStaticTriangleMaterialBuffer = smokeBuffers.previousStaticTriangleMaterialBuffer;
    nvrhi::BufferHandle smokePreviousStaticTriangleMaterialIndexBuffer = smokeBuffers.previousStaticTriangleMaterialIndexBuffer;
    nvrhi::BufferHandle smokeDynamicVertexBuffer = smokeBuffers.dynamicVertexBuffer;
    nvrhi::BufferHandle smokeDynamicIndexBuffer = smokeBuffers.dynamicIndexBuffer;
    nvrhi::BufferHandle smokeDynamicTriangleClassBuffer = smokeBuffers.dynamicTriangleClassBuffer;
    nvrhi::BufferHandle smokeDynamicTriangleMaterialBuffer = smokeBuffers.dynamicTriangleMaterialBuffer;
    nvrhi::BufferHandle smokeDynamicTriangleMaterialIndexBuffer = smokeBuffers.dynamicTriangleMaterialIndexBuffer;
    nvrhi::BufferHandle smokeMaterialTableBuffer = smokeBuffers.materialTableBuffer;
    nvrhi::BufferHandle smokeMaterialFeatureBuffer = smokeBuffers.materialFeatureBuffer;
    nvrhi::BufferHandle smokeMaterialFeatureParameterBuffer = smokeBuffers.materialFeatureParameterBuffer;
    nvrhi::BufferHandle smokeDynamicMaterialBuffer = smokeBuffers.dynamicMaterialBuffer;
    nvrhi::BufferHandle smokeEmissiveTriangleBuffer = smokeBuffers.emissiveTriangleBuffer;
    nvrhi::BufferHandle smokePreviousEmissiveTriangleBuffer = smokeBuffers.previousEmissiveTriangleBuffer;
    nvrhi::BufferHandle smokeEmissiveRemapBuffer = smokeBuffers.emissiveRemapBuffer;
    nvrhi::BufferHandle smokeEmissiveDistributionBuffer = smokeBuffers.emissiveDistributionBuffer;
    nvrhi::BufferHandle smokeLightCandidateBuffer = smokeBuffers.lightCandidateBuffer;
    nvrhi::BufferHandle smokeDoomAnalyticLightBuffer = smokeBuffers.doomAnalyticLightBuffer;
    nvrhi::BufferHandle smokeDoomAnalyticPreviousLightBuffer = smokeBuffers.doomAnalyticPreviousLightBuffer;
    nvrhi::BufferHandle smokeDoomAnalyticCurrentIdentityBuffer = smokeBuffers.doomAnalyticCurrentIdentityBuffer;
    nvrhi::BufferHandle smokeDoomAnalyticPreviousIdentityBuffer = smokeBuffers.doomAnalyticPreviousIdentityBuffer;
    nvrhi::BufferHandle smokeDoomAnalyticRemapBuffer = smokeBuffers.doomAnalyticRemapBuffer;
    nvrhi::BufferHandle smokeUnifiedLightBuffer = smokeBuffers.unifiedLightBuffer;
    nvrhi::BufferHandle smokeUnifiedPreviousLightBuffer = smokeBuffers.unifiedPreviousLightBuffer;
    nvrhi::BufferHandle smokeUnifiedLightRemapBuffer = smokeBuffers.unifiedLightRemapBuffer;
    nvrhi::BufferHandle smokeRestirLightManagerCurrentToPreviousBuffer = smokeBuffers.restirLightManagerCurrentToPreviousBuffer;
    nvrhi::BufferHandle smokeRestirLightManagerPreviousToCurrentBuffer = smokeBuffers.restirLightManagerPreviousToCurrentBuffer;
    nvrhi::BufferHandle smokeRestirLightManagerCurrentPayloadBuffer = smokeBuffers.restirLightManagerCurrentPayloadBuffer;
    nvrhi::BufferHandle smokeRestirLightManagerPreviousPayloadBuffer = smokeBuffers.restirLightManagerPreviousPayloadBuffer;
    nvrhi::BufferHandle smokeRigidRouteVertexBuffer = smokeBuffers.rigidRouteVertexBuffer;
    nvrhi::BufferHandle smokeRigidRouteIndexBuffer = smokeBuffers.rigidRouteIndexBuffer;
    nvrhi::BufferHandle smokeRigidRouteTriangleMaterialBuffer = smokeBuffers.rigidRouteTriangleMaterialBuffer;
    nvrhi::BufferHandle smokeRigidRouteTriangleMaterialIndexBuffer = smokeBuffers.rigidRouteTriangleMaterialIndexBuffer;
    nvrhi::BufferHandle smokeRigidRouteInstanceBuffer = smokeBuffers.rigidRouteInstanceBuffer;
    nvrhi::BufferHandle smokeSkinnedHitRouteRecordBuffer = smokeBuffers.skinnedHitRouteRecordBuffer;
    nvrhi::BufferHandle smokeSkinnedHitRouteTriangleBuffer = smokeBuffers.skinnedHitRouteTriangleBuffer;
    nvrhi::BufferHandle smokeSkinnedSourceVertexBuffer = smokeBuffers.skinnedSourceVertexBuffer;
    nvrhi::BufferHandle smokeSkinnedCurrentOutputVertexBuffer = smokeBuffers.skinnedCurrentOutputVertexBuffer;
    const bool skinnedOutputBufferChanged =
        smokeSkinnedCurrentOutputVertexBuffer !=
            m_smokeSkinnedCurrentOutputVertexBuffer;
    nvrhi::BufferHandle smokeSkinnedPreviousPositionBuffer = smokeBuffers.skinnedPreviousPositionBuffer;
    nvrhi::BufferHandle smokeSkinnedSurfaceDispatchBuffer = smokeBuffers.skinnedSurfaceDispatchBuffer;
    nvrhi::BufferHandle smokeSkinnedTriangleDispatchIndexBuffer = smokeBuffers.skinnedTriangleDispatchIndexBuffer;
    nvrhi::BufferHandle smokeSkinnedCurrentJointMatrixBuffer = smokeBuffers.skinnedCurrentJointMatrixBuffer;
    nvrhi::BufferHandle smokeSkinnedPreviousJointMatrixBuffer = smokeBuffers.skinnedPreviousJointMatrixBuffer;
    nvrhi::BufferHandle smokeSkinnedEmissiveWorkBuffer =
        smokeBuffers.skinnedEmissiveWorkBuffer;
    const int bufferCreateMs = Sys_Milliseconds() - bufferCreateStartMs;

    const int staticVertexCount = static_cast<int>(staticVertexCache.size());
    const int dynamicVertexCount = static_cast<int>(dynamicVertexData.size());
    const int staticIndexCount = bucketRanges.buckets[0].indexCount;
    const int dynamicIndexCount =
        bucketRanges.buckets[1].indexCount +
        bucketRanges.buckets[2].indexCount +
        bucketRanges.buckets[3].indexCount +
        bucketRanges.buckets[4].indexCount;
    const bool hasStaticBlas = staticIndexCount > 0;
    const bool hasDynamicBlas = dynamicIndexCount > 0;
    const RtSmokeBucketRange& staticBucketRange = bucketRanges.buckets[0];
    RtSmokePlanGeometryRange staticGeometryRange;
    staticGeometryRange.vertexOffset = staticBucketRange.vertexOffset;
    staticGeometryRange.vertexCount = staticBucketRange.vertexCount;
    staticGeometryRange.indexOffset = staticBucketRange.indexOffset;
    staticGeometryRange.indexCount = staticBucketRange.indexCount;
    staticGeometryRange.triangleOffset = staticBucketRange.triangleOffset;
    staticGeometryRange.triangleCount = staticBucketRange.triangleCount;
    const bool validateGeometryUniverse = r_pathTracingGeometryUniverseValidate.GetInteger() != 0;
    const RtSmokeGeometryUniverseStats geometryUniverseStats = [&]() {
        OPTICK_EVENT("PT Geometry Universe Stats");
        return m_smokeGeometryUniverse.GetStats(validateGeometryUniverse);
    }();
    ReleaseCompletedRetiredSmokeSkinnedComparisonBlases(
        geometryUniverseStats.frameIndex);
    if (r_pathTracingGeometryUniverseRangeDump.GetInteger() != 0)
    {
        m_smokeGeometryUniverse.LogStaticRangeHistory(RT_SMOKE_GEOMETRY_RANGE_DUMP_RECORDS);
        r_pathTracingGeometryUniverseRangeDump.SetInteger(0);
    }
    if (validateGeometryUniverse && geometryUniverseStats.staticValidationErrors > 0)
    {
        if (g_smokeLastGeometryValidationDumpGeneration != geometryUniverseStats.generation ||
            g_smokeLastGeometryValidationDumpErrors != geometryUniverseStats.staticValidationErrors)
        {
            m_smokeGeometryUniverse.LogStaticValidationFailures(RT_SMOKE_GEOMETRY_VALIDATION_DUMP_RECORDS);
            g_smokeLastGeometryValidationDumpGeneration = geometryUniverseStats.generation;
            g_smokeLastGeometryValidationDumpErrors = geometryUniverseStats.staticValidationErrors;
        }
    }
    else if (geometryUniverseStats.staticValidationErrors == 0)
    {
        g_smokeLastGeometryValidationDumpErrors = 0;
    }
    const int staticVertexCacheCount = geometryUniverseStats.staticVerts;
    const int staticIndexCacheCount = geometryUniverseStats.staticIndexes;
    const int staticTriangleCacheCount = geometryUniverseStats.staticTriangles;
    const int staticCacheBytesKB = geometryUniverseStats.staticBytesKB;
    const int forceStaticBlasRebuildMode =
        idMath::ClampInt(0, 2, r_pathTracingStaticBlasForceRebuild.GetInteger());
    const bool forceStaticBlasRebuild = forceStaticBlasRebuildMode != 0;
    const bool forceStaticBlasRebuildWithoutUpload = forceStaticBlasRebuildMode == 2;
    const uint64 cachedStaticBlasGeometryGeneration = m_smokeStaticBlasGeometryGeneration;
    const bool staticBlasGenerationMismatch =
        r_pathTracingStaticBlasGenerationGuard.GetBool() &&
        m_smokeStaticBlasCacheValid &&
        cachedStaticBlasGeometryGeneration != geometryUniverseStats.staticGeometryGeneration;
    if (staticBlasGenerationMismatch)
    {
        common->Printf(
            "PathTracePrimaryPass: PT static BLAS generation guard invalidated cache frame=%llu cached=%llu current=%llu\n",
            static_cast<unsigned long long>(geometryUniverseStats.frameIndex),
            static_cast<unsigned long long>(cachedStaticBlasGeometryGeneration),
            static_cast<unsigned long long>(geometryUniverseStats.staticGeometryGeneration));
    }
    RtSmokeAccelerationPlanInput accelerationPlanInput;
    {
        OPTICK_EVENT("PT Acceleration Plan Input Desc");
        accelerationPlanInput.staticSignature.vertices = staticVertexCache.empty() ? nullptr : staticVertexCache.data();
        accelerationPlanInput.staticSignature.vertexStride = sizeof(PathTraceSmokeVertex);
        accelerationPlanInput.staticSignature.totalVertexCount = static_cast<int>(staticVertexCache.size());
        accelerationPlanInput.staticSignature.indexes = staticIndexCache.empty() ? nullptr : staticIndexCache.data();
        accelerationPlanInput.staticSignature.totalIndexCount = static_cast<int>(staticIndexCache.size());
        accelerationPlanInput.staticSignature.triangleClasses = staticTriangleClassCache.empty() ? nullptr : staticTriangleClassCache.data();
        accelerationPlanInput.staticSignature.triangleMaterials = staticTriangleMaterialCache.empty() ? nullptr : staticTriangleMaterialCache.data();
        accelerationPlanInput.staticSignature.totalTriangleCount = static_cast<int>(Min(staticTriangleClassCache.size(), staticTriangleMaterialCache.size()));
        accelerationPlanInput.staticSignature.staticRange.vertexOffset = staticGeometryRange.vertexOffset;
        accelerationPlanInput.staticSignature.staticRange.vertexCount = staticGeometryRange.vertexCount;
        accelerationPlanInput.staticSignature.staticRange.indexOffset = staticGeometryRange.indexOffset;
        accelerationPlanInput.staticSignature.staticRange.indexCount = staticGeometryRange.indexCount;
        accelerationPlanInput.staticSignature.staticRange.triangleOffset = staticGeometryRange.triangleOffset;
        accelerationPlanInput.staticSignature.staticRange.triangleCount = staticGeometryRange.triangleCount;
        accelerationPlanInput.staticSignature.sceneOrigin.x = vec3_origin.x;
        accelerationPlanInput.staticSignature.sceneOrigin.y = vec3_origin.y;
        accelerationPlanInput.staticSignature.sceneOrigin.z = vec3_origin.z;
        accelerationPlanInput.staticCache.hasStaticBlas = hasStaticBlas;
        accelerationPlanInput.staticCache.cacheValid = m_smokeStaticBlasCacheValid;
        accelerationPlanInput.staticCache.cacheResourcesReady =
            m_smokeStaticBlas &&
            m_smokeStaticVertexBuffer &&
            m_smokeStaticIndexBuffer &&
            m_smokeStaticTriangleClassBuffer &&
            m_smokeStaticTriangleMaterialBuffer &&
            m_smokeStaticTriangleMaterialIndexBuffer;
        accelerationPlanInput.staticCache.staticCacheChanged =
            staticCacheChanged || forceStaticBlasRebuild || staticBlasGenerationMismatch;
        accelerationPlanInput.staticCache.previousSignatureHash = m_smokeStaticBlasSignature;
        accelerationPlanInput.staticVertexCount = staticVertexCount;
        accelerationPlanInput.staticIndexCount = staticIndexCount;
        accelerationPlanInput.dynamicVertexCount = dynamicVertexCount;
        accelerationPlanInput.dynamicIndexCount = dynamicIndexCount;
    }

    RtSmokeAccelerationPlan accelerationPlan;
    RtPathTraceCpuWorkGeneration accelerationPlanGeneration;
    accelerationPlanGeneration.frameIndex = 0;
    accelerationPlanGeneration.sceneGeneration = m_smokeSceneUniverseStaticBuildGeneration;
    // BuildSmokeAccelerationPlan consumes only accelerationPlanInput. The
    // input token below covers static signature/cache state and dynamic counts,
    // so avoid invalidating async work on unrelated geometry-universe churn.
    accelerationPlanGeneration.geometryGeneration = 0;
    // BuildSmokeAccelerationPlan consumes only geometry BLAS inputs. Material
    // payload changes are tracked by the material table path, not this worker.
    accelerationPlanGeneration.materialGeneration = 0;
    accelerationPlanGeneration.lightGeneration = [&]() {
        OPTICK_EVENT("PT Acceleration Plan Input Token");
        return BuildSmokeAccelerationPlanInputToken(accelerationPlanInput);
    }();
    RtPathTraceCpuWorkPublishSnapshot(m_smokeCpuWorkState, accelerationPlanGeneration);

    bool accelerationPlanAcceptedFromAsync = false;
    int staticBlasSignatureMs = 0;
    if (asyncCpuPlanning && m_smokeAccelerationPlanFuture.valid())
    {
        OPTICK_EVENT("PT Acceleration Async Accept");
        const std::future_status futureStatus =
            m_smokeAccelerationPlanFuture.wait_for(std::chrono::seconds(0));
        if (futureStatus == std::future_status::ready)
        {
            const RtSmokeAccelerationPlanTimedResult timedResult = m_smokeAccelerationPlanFuture.get();
            m_smokeAccelerationPlanAsyncGenerationValid = false;
            RtPathTraceCpuWorkResultEnvelope asyncEnvelope;
            asyncEnvelope.completed = timedResult.result.valid;
            asyncEnvelope.generation = m_smokeAccelerationPlanAsyncGeneration;
            asyncEnvelope.timing = m_smokeAccelerationPlanAsyncTiming;
            asyncEnvelope.timing.workerExecutionMs = timedResult.workerExecutionMs;
            const double asyncOutstandingMs =
                static_cast<double>(Max(0, Sys_Milliseconds() - m_smokeAccelerationPlanAsyncLaunchMs));
            asyncEnvelope.timing.queueWaitMs = Max(0.0, asyncOutstandingMs - timedResult.workerExecutionMs);
            RtPathTraceCpuWorkPublishCompletedResult(m_smokeCpuWorkState, asyncEnvelope);

            const RtPathTraceCpuWorkFrameDecision asyncDecision =
                RtPathTraceCpuWorkAcceptLatest(m_smokeCpuWorkState, accelerationPlanGeneration, &asyncEnvelope, false);
            if (asyncDecision.accepted && timedResult.result.valid)
            {
                accelerationPlan = timedResult.result.plan;
                staticBlasSignatureMs = static_cast<int>(timedResult.workerExecutionMs + 0.5);
                m_smokeAccelerationPlanAsyncCachedPlan = timedResult.result.plan;
                m_smokeAccelerationPlanAsyncCachedGeneration = m_smokeAccelerationPlanAsyncGeneration;
                m_smokeAccelerationPlanAsyncCachedPlanValid = true;
                accelerationPlanAcceptedFromAsync = true;
            }
        }
        else
        {
            RtPathTraceCpuWorkAcceptLatest(m_smokeCpuWorkState, accelerationPlanGeneration, nullptr, true);
        }
    }
    if (!accelerationPlanAcceptedFromAsync &&
        asyncCpuPlanning &&
        m_smokeAccelerationPlanAsyncCachedPlanValid &&
        RtPathTraceCpuWorkGenerationEquals(m_smokeAccelerationPlanAsyncCachedGeneration, accelerationPlanGeneration))
    {
        accelerationPlan = m_smokeAccelerationPlanAsyncCachedPlan;
        accelerationPlanAcceptedFromAsync = true;
    }

    if (!accelerationPlanAcceptedFromAsync)
    {
        const int staticSignatureStartMs = Sys_Milliseconds();
        {
            OPTICK_EVENT("PT CPU Acceleration Plan");
            accelerationPlan = BuildSmokeAccelerationPlan(accelerationPlanInput);
        }
        staticBlasSignatureMs = Sys_Milliseconds() - staticSignatureStartMs;
        RtPathTraceCpuWorkResultEnvelope accelerationPlanEnvelope;
        accelerationPlanEnvelope.completed = true;
        accelerationPlanEnvelope.generation = accelerationPlanGeneration;
        accelerationPlanEnvelope.timing.workerExecutionMs = static_cast<double>(staticBlasSignatureMs);
        RtPathTraceCpuWorkPublishCompletedResult(m_smokeCpuWorkState, accelerationPlanEnvelope);
        const RtPathTraceCpuWorkFrameDecision accelerationPlanDecision =
            RtPathTraceCpuWorkAcceptLatest(m_smokeCpuWorkState, accelerationPlanGeneration, nullptr, true);
        if (!accelerationPlanDecision.accepted && !accelerationPlanDecision.syncFallback)
        {
            common->Printf("PathTracePrimaryPass: failed to accept RT smoke CPU acceleration plan\n");
            return;
        }

        m_smokeAccelerationPlanAsyncCachedPlan = accelerationPlan;
        m_smokeAccelerationPlanAsyncCachedGeneration = accelerationPlanGeneration;
        m_smokeAccelerationPlanAsyncCachedPlanValid = true;
    }

    const bool asyncPlanAlreadyCached =
        m_smokeAccelerationPlanAsyncCachedPlanValid &&
        RtPathTraceCpuWorkGenerationEquals(m_smokeAccelerationPlanAsyncCachedGeneration, accelerationPlanGeneration);
    const bool asyncPlanAlreadyQueued =
        m_smokeAccelerationPlanAsyncGenerationValid &&
        RtPathTraceCpuWorkGenerationEquals(m_smokeAccelerationPlanAsyncGeneration, accelerationPlanGeneration);
    if (asyncCpuPlanning &&
        !m_smokeAccelerationPlanFuture.valid() &&
        !asyncPlanAlreadyCached &&
        !asyncPlanAlreadyQueued)
    {
        OPTICK_EVENT("PT Acceleration Queue");
        const int snapshotStartMs = Sys_Milliseconds();
        const RtSmokeAccelerationPlanSnapshot accelerationPlanSnapshot = [&]() {
            OPTICK_EVENT("PT Acceleration Queue Snapshot");
            return CaptureSmokeAccelerationPlanSnapshot(accelerationPlanInput);
        }();
        m_smokeAccelerationPlanAsyncTiming = RtPathTraceCpuWorkTiming();
        m_smokeAccelerationPlanAsyncTiming.snapshotCaptureMs =
            static_cast<double>(Sys_Milliseconds() - snapshotStartMs);
        m_smokeAccelerationPlanAsyncGeneration = accelerationPlanGeneration;
        m_smokeAccelerationPlanAsyncGenerationValid = true;
        m_smokeAccelerationPlanAsyncLaunchMs = Sys_Milliseconds();
        m_smokeAccelerationPlanFuture.Start(
            [accelerationPlanSnapshot]() {
                return BuildSmokeAccelerationPlanTimedResult(accelerationPlanSnapshot);
            });
    }

    RtSmokePlanStaticBlasSignature staticSignature;
    staticSignature.hash = accelerationPlan.staticSignature.hash;
    staticSignature.vertexCount = accelerationPlan.staticSignature.vertexCount;
    staticSignature.indexCount = accelerationPlan.staticSignature.indexCount;
    staticSignature.triangleCount = accelerationPlan.staticSignature.triangleCount;
    const bool staticBlasSignatureReused = accelerationPlan.staticSignatureReused;
    const PathTraceRemixLightManagerStats remixLightManagerSignatureStats = m_remixLightManager.GetStats();
    const uint64 reservoirSceneSignature = ComputeSmokeReservoirStructuralSignature(
        materialTableSignature,
        staticSignature.hash,
        remixLightManagerSignatureStats.structuralSignature);
    bool staticBlasCacheHit = accelerationPlan.staticCacheHit;
    if (staticBlasCacheHit)
    {
        const bool staticBlasCacheBuffersReady =
            SmokeBufferHasPayloadCapacity(m_smokeStaticVertexBuffer, bufferCreateDesc.staticVertexBytes, sizeof(PathTraceSmokeVertex)) &&
            SmokeBufferHasPayloadCapacity(m_smokeStaticIndexBuffer, bufferCreateDesc.staticIndexBytes, sizeof(uint32_t)) &&
            SmokeBufferHasPayloadCapacity(m_smokeStaticTriangleClassBuffer, bufferCreateDesc.staticTriangleClassBytes, sizeof(uint32_t));
        if (!staticBlasCacheBuffersReady)
        {
            staticBlasCacheHit = false;
        }
    }
    if (staticBlasCacheHit)
    {
        smokeStaticVertexBuffer = m_smokeStaticVertexBuffer;
        smokeStaticIndexBuffer = m_smokeStaticIndexBuffer;
        smokeStaticTriangleClassBuffer = m_smokeStaticTriangleClassBuffer;
        smokeBuffers.staticVertexBuffer = smokeStaticVertexBuffer;
        smokeBuffers.staticIndexBuffer = smokeStaticIndexBuffer;
        smokeBuffers.staticTriangleClassBuffer = smokeStaticTriangleClassBuffer;
    }

    if (!hasStaticBlas && !hasDynamicBlas)
    {
        common->Printf("PathTracePrimaryPass: no RT smoke BLAS ranges to build\n");
        return;
    }

    nvrhi::rt::AccelStructDesc smokeStaticBlasDesc;
    nvrhi::rt::AccelStructHandle smokeStaticBlas;
    if (hasStaticBlas)
    {
        if (staticBlasCacheHit)
        {
            smokeStaticBlas = m_smokeStaticBlas;
            smokeStaticBlasDesc = m_smokeStaticBlasDesc;
            ++m_smokeStaticBlasCacheHitCount;
        }
        else
        {
            RtSmokeBlasCreateDesc staticBlasCreateDesc;
            staticBlasCreateDesc.device = device;
            staticBlasCreateDesc.vertexBuffer = smokeStaticVertexBuffer;
            staticBlasCreateDesc.indexBuffer = smokeStaticIndexBuffer;
            staticBlasCreateDesc.vertexCount = accelerationPlan.staticBlas.vertexCount;
            staticBlasCreateDesc.indexCount = accelerationPlan.staticBlas.indexCount;
            staticBlasCreateDesc.debugName = accelerationPlan.staticBlas.debugName;
            RtSmokeBlasCreateResult staticBlasCreateResult;
            {
                OPTICK_EVENT("PT Create Static BLAS");
                staticBlasCreateResult = CreateSmokeBlas(staticBlasCreateDesc);
            }
            if (!staticBlasCreateResult.Succeeded())
            {
                common->Printf("PathTracePrimaryPass: failed to create RT smoke static BLAS\n");
                return;
            }
            smokeStaticBlasDesc = staticBlasCreateResult.accelStructDesc;
            smokeStaticBlas = staticBlasCreateResult.accelStruct;
            ++m_smokeStaticBlasCacheMissCount;
        }
    }

    nvrhi::rt::AccelStructDesc smokeDynamicBlasDesc;
    nvrhi::rt::AccelStructHandle smokeDynamicBlas;
    if (hasDynamicBlas)
    {
        RtSmokeBlasCreateDesc dynamicBlasCreateDesc;
        dynamicBlasCreateDesc.device = device;
        dynamicBlasCreateDesc.vertexBuffer = smokeDynamicVertexBuffer;
        dynamicBlasCreateDesc.indexBuffer = smokeDynamicIndexBuffer;
        dynamicBlasCreateDesc.vertexCount = accelerationPlan.dynamicBlas.vertexCount;
        dynamicBlasCreateDesc.indexCount = accelerationPlan.dynamicBlas.indexCount;
        dynamicBlasCreateDesc.debugName = accelerationPlan.dynamicBlas.debugName;
        RtSmokeBlasCreateResult dynamicBlasCreateResult;
        {
            OPTICK_EVENT("PT Create Dynamic BLAS");
            dynamicBlasCreateResult = CreateSmokeBlas(dynamicBlasCreateDesc);
        }
        if (!dynamicBlasCreateResult.Succeeded())
        {
            common->Printf("PathTracePrimaryPass: failed to create RT smoke dynamic BLAS\n");
            return;
        }
        smokeDynamicBlasDesc = dynamicBlasCreateResult.accelStructDesc;
        smokeDynamicBlas = dynamicBlasCreateResult.accelStruct;
    }

    const bool staticGeometryBuffersReused =
        smokeStaticVertexBuffer && smokeStaticVertexBuffer == m_smokeStaticVertexBuffer &&
        smokeStaticIndexBuffer && smokeStaticIndexBuffer == m_smokeStaticIndexBuffer &&
        smokeStaticTriangleClassBuffer && smokeStaticTriangleClassBuffer == m_smokeStaticTriangleClassBuffer &&
        smokeStaticTriangleMaterialBuffer && smokeStaticTriangleMaterialBuffer == m_smokeStaticTriangleMaterialBuffer;
    RtSmokeStaticDirtyUploadPlanInput staticDirtyUploadPlanInput;
    staticDirtyUploadPlanInput.staticBlasCacheHit = staticBlasCacheHit;
    staticDirtyUploadPlanInput.staticCacheChanged = staticCacheChanged;
    staticDirtyUploadPlanInput.staticGeometryBuffersReused = staticGeometryBuffersReused;
    staticDirtyUploadPlanInput.staticDirtyCount = geometryUniverseStats.staticDirty;
    staticDirtyUploadPlanInput.dirtyVertexOffset = geometryUniverseStats.staticDirtyVertexOffset;
    staticDirtyUploadPlanInput.dirtyVertexCount = geometryUniverseStats.staticDirtyVertexCount;
    staticDirtyUploadPlanInput.totalVertexCount = staticVertexCache.size();
    staticDirtyUploadPlanInput.dirtyIndexOffset = geometryUniverseStats.staticDirtyIndexOffset;
    staticDirtyUploadPlanInput.dirtyIndexCount = geometryUniverseStats.staticDirtyIndexCount;
    staticDirtyUploadPlanInput.totalIndexCount = staticIndexCache.size();
    staticDirtyUploadPlanInput.dirtyTriangleOffset = geometryUniverseStats.staticDirtyTriangleOffset;
    staticDirtyUploadPlanInput.dirtyTriangleCount = geometryUniverseStats.staticDirtyTriangleCount;
    staticDirtyUploadPlanInput.totalTriangleClassCount = staticTriangleClassCache.size();
    staticDirtyUploadPlanInput.totalTriangleMaterialCount = staticTriangleMaterialCache.size();
    const RtSmokeStaticDirtyUploadPlan staticDirtyUploadPlan = [&]() {
        OPTICK_EVENT("PT Static Dirty Upload Plan");
        return BuildSmokeStaticDirtyUploadPlan(staticDirtyUploadPlanInput);
    }();
    const bool staticDirtyRangesValid = staticDirtyUploadPlan.dirtyRangesValid;
    const bool useStaticDirtyRangeUploads = staticDirtyUploadPlan.useDirtyRangeUploads;
    const bool previousStaticGeometryBuffersReused =
        smokePreviousStaticVertexBuffer && smokePreviousStaticVertexBuffer == m_smokePreviousStaticVertexBuffer &&
        smokePreviousStaticIndexBuffer && smokePreviousStaticIndexBuffer == m_smokePreviousStaticIndexBuffer &&
        smokePreviousStaticTriangleClassBuffer && smokePreviousStaticTriangleClassBuffer == m_smokePreviousStaticTriangleClassBuffer &&
        smokePreviousStaticTriangleMaterialBuffer && smokePreviousStaticTriangleMaterialBuffer == m_smokePreviousStaticTriangleMaterialBuffer;
    const bool previousStaticMaterialIndexBufferReused =
        smokePreviousStaticTriangleMaterialIndexBuffer && smokePreviousStaticTriangleMaterialIndexBuffer == m_smokePreviousStaticTriangleMaterialIndexBuffer;
    const bool previousStaticGeometryDataAvailable =
        !previousStaticVertexCache.empty() &&
        !previousStaticIndexCache.empty() &&
        !previousStaticTriangleClassCache.empty() &&
        !previousStaticTriangleMaterialCache.empty();
    const bool previousStaticMaterialIndexDataAvailable =
        !previousStaticTriangleMaterialIndexCache.empty();
    const bool previousStaticSnapshotDataAvailable =
        previousStaticGeometryDataAvailable &&
        previousStaticMaterialIndexDataAvailable;
    const uint64 previousStaticSnapshotUploadSignature = previousStaticGeometryDataAvailable
        ? [&]() {
            OPTICK_EVENT("PT Previous Static Upload Signature");
            return BuildSmokePreviousStaticGeometryUploadSignature(
                geometryUniverseStats.previousStaticSnapshotGeneration,
                geometryUniverseStats.previousStaticSnapshotMaterialGeneration,
                previousStaticVertexCache.size(),
                previousStaticIndexCache.size(),
                previousStaticTriangleClassCache.size(),
                previousStaticTriangleMaterialCache.size());
        }()
        : 0;
    RtSmokePreviousStaticSnapshotUploadPlanInput previousStaticSnapshotUploadPlanInput;
    previousStaticSnapshotUploadPlanInput.dataAvailable = previousStaticGeometryDataAvailable;
    previousStaticSnapshotUploadPlanInput.buffersReused = previousStaticGeometryBuffersReused;
    previousStaticSnapshotUploadPlanInput.previousUploadSignature = m_smokePreviousStaticSnapshotUploadSignature;
    previousStaticSnapshotUploadPlanInput.currentUploadSignature = previousStaticSnapshotUploadSignature;
    const RtSmokePreviousStaticSnapshotUploadPlan previousStaticSnapshotUploadPlan = [&]() {
        OPTICK_EVENT("PT Previous Static Upload Plan");
        return BuildSmokePreviousStaticSnapshotUploadPlan(previousStaticSnapshotUploadPlanInput);
    }();
    const bool skipPreviousStaticGeometryUpload = previousStaticSnapshotUploadPlan.skipUpload;
    const uint64 previousStaticMaterialIndexUploadSignature = previousStaticMaterialIndexDataAvailable
        ? [&]() {
            OPTICK_EVENT("PT Previous Static Material Index Upload Signature");
            return BuildSmokeVectorUploadSignature(previousStaticTriangleMaterialIndexCache);
        }()
        : 0;
    RtSmokePreviousStaticSnapshotUploadPlanInput previousStaticMaterialIndexUploadPlanInput;
    previousStaticMaterialIndexUploadPlanInput.dataAvailable = previousStaticMaterialIndexDataAvailable;
    previousStaticMaterialIndexUploadPlanInput.buffersReused = previousStaticMaterialIndexBufferReused;
    previousStaticMaterialIndexUploadPlanInput.previousUploadSignature = m_smokePreviousStaticMaterialIndexUploadSignature;
    previousStaticMaterialIndexUploadPlanInput.currentUploadSignature = previousStaticMaterialIndexUploadSignature;
    const RtSmokePreviousStaticSnapshotUploadPlan previousStaticMaterialIndexUploadPlan = [&]() {
        OPTICK_EVENT("PT Previous Static Material Index Upload Plan");
        return BuildSmokePreviousStaticSnapshotUploadPlan(previousStaticMaterialIndexUploadPlanInput);
    }();
    const bool skipPreviousStaticMaterialIndexUpload = previousStaticMaterialIndexUploadPlan.skipUpload;
    const uint64 staticTriangleMaterialUploadSignature = [&]() {
        OPTICK_EVENT("PT Static Material Upload Signature");
        return BuildSmokeVectorUploadSignature(staticTriangleMaterialCache);
    }();
    const uint64 staticTriangleMaterialIndexUploadSignature = [&]() {
        OPTICK_EVENT("PT Static Material Index Upload Signature");
        return BuildSmokeVectorUploadSignature(materialTable.staticMaterialIndexes);
    }();
    const bool skipStaticTriangleMaterialUpload =
        SmokeBufferCanSkipVectorUpload(
            smokeStaticTriangleMaterialBuffer,
            m_smokeStaticTriangleMaterialBuffer,
            bufferCreateDesc.staticTriangleMaterialBytes,
            sizeof(uint32_t),
            m_smokeStaticTriangleMaterialUploadSignatureValid,
            m_smokeStaticTriangleMaterialUploadSignature,
            staticTriangleMaterialUploadSignature);
    const bool skipStaticTriangleMaterialIndexUpload =
        SmokeBufferCanSkipVectorUpload(
            smokeStaticTriangleMaterialIndexBuffer,
            m_smokeStaticTriangleMaterialIndexBuffer,
            bufferCreateDesc.staticTriangleMaterialIndexBytes,
            sizeof(uint32_t),
            m_smokeStaticTriangleMaterialIndexUploadSignatureValid,
            m_smokeStaticTriangleMaterialIndexUploadSignature,
            staticTriangleMaterialIndexUploadSignature);
    const uint64 materialTableUploadSignature = [&]() {
        OPTICK_EVENT("PT Material Table Upload Signature");
        return BuildSmokeVectorUploadSignature(gpuMaterialTableMaterials);
    }();
    const uint64 dynamicMaterialUploadSignature = [&]() {
        OPTICK_EVENT("PT Dynamic Material Upload Signature");
        return BuildSmokeVectorUploadSignature(dynamicMaterialRecords);
    }();
    int materialTableDirtyOffset = -1;
    int materialTableDirtyCount = 0;
    const bool conservativeMaterialUpload = true;
    const bool materialTableBufferReused =
        smokeMaterialTableBuffer &&
        smokeMaterialTableBuffer == m_smokeMaterialTableBuffer &&
        SmokeBufferHasPayloadCapacity(
            smokeMaterialTableBuffer,
            bufferCreateDesc.materialTableBytes,
            sizeof(PathTraceSmokeMaterial));
    const bool materialTableRangeValid =
        !conservativeMaterialUpload &&
        r_pathTracingResidency.GetInteger() != 0 &&
        r_pathTracingResidencyMaterial.GetInteger() != 0 &&
        materialTableBufferReused &&
        FindSmokeVectorChangedRange(
            m_smokeMaterialTableMaterials,
            gpuMaterialTableMaterials,
            materialTableDirtyOffset,
            materialTableDirtyCount);
    int dynamicMaterialDirtyOffset = -1;
    int dynamicMaterialDirtyCount = 0;
    const bool dynamicMaterialBufferReused =
        smokeDynamicMaterialBuffer &&
        smokeDynamicMaterialBuffer == m_smokeDynamicMaterialBuffer &&
        SmokeBufferHasPayloadCapacity(
            smokeDynamicMaterialBuffer,
            bufferCreateDesc.dynamicMaterialBytes,
            sizeof(PathTraceDynamicMaterialRecord));
    const bool dynamicMaterialRangeValid =
        !conservativeMaterialUpload &&
        r_pathTracingResidency.GetInteger() != 0 &&
        r_pathTracingResidencyMaterial.GetInteger() != 0 &&
        dynamicMaterialBufferReused &&
        FindSmokeVectorChangedRange(
            m_smokeDynamicMaterialRecords,
            dynamicMaterialRecords,
            dynamicMaterialDirtyOffset,
            dynamicMaterialDirtyCount);
    const bool skipMaterialTableUpload =
        !conservativeMaterialUpload &&
        (materialTableRangeValid
            ? materialTableDirtyOffset < 0
            : SmokeBufferCanSkipVectorUpload(
                smokeMaterialTableBuffer,
                m_smokeMaterialTableBuffer,
                bufferCreateDesc.materialTableBytes,
                sizeof(PathTraceSmokeMaterial),
                m_smokeMaterialTableUploadSignatureValid,
                m_smokeMaterialTableUploadSignature,
                materialTableUploadSignature));
    const bool skipDynamicMaterialUpload =
        !conservativeMaterialUpload &&
        (dynamicMaterialRangeValid
            ? dynamicMaterialDirtyOffset < 0
            : SmokeBufferCanSkipVectorUpload(
                smokeDynamicMaterialBuffer,
                m_smokeDynamicMaterialBuffer,
                bufferCreateDesc.dynamicMaterialBytes,
                sizeof(PathTraceDynamicMaterialRecord),
                m_smokeDynamicMaterialUploadSignatureValid,
                m_smokeDynamicMaterialUploadSignature,
                dynamicMaterialUploadSignature));
    const int materialTableUploadOffset =
        !skipMaterialTableUpload && materialTableRangeValid && materialTableDirtyOffset >= 0
            ? materialTableDirtyOffset
            : -1;
    const int materialTableUploadCount =
        materialTableUploadOffset >= 0 ? materialTableDirtyCount : 0;
    const int dynamicMaterialUploadOffset =
        !skipDynamicMaterialUpload && dynamicMaterialRangeValid && dynamicMaterialDirtyOffset >= 0
            ? dynamicMaterialDirtyOffset
            : -1;
    const int dynamicMaterialUploadCount =
        dynamicMaterialUploadOffset >= 0 ? dynamicMaterialDirtyCount : 0;
    const int skinnedGpuComputeVertexCount = SmokeSkinnedGpuComputeVertexCount(skinnedGpuScaffold.dispatchRecords);
    const int skinnedGpuComputeMaxVertexCount = SmokeSkinnedGpuComputeMaxVertexCount(skinnedGpuScaffold.dispatchRecords);
    std::vector<PathTraceSkinnedSurfaceDispatchRecord> skinnedGpuComputeDispatchRecords = skinnedGpuScaffold.dispatchRecords;
    const bool skinnedGpuComputeTargetsDynamicVertices = gpuSkinningMode >= 2;
    if (skinnedGpuComputeTargetsDynamicVertices)
    {
        for (PathTraceSkinnedSurfaceDispatchRecord& dispatch : skinnedGpuComputeDispatchRecords)
        {
            dispatch.outputVertexOffset = dispatch.dynamicVertexOffset;
        }
    }
    nvrhi::BufferHandle smokeSkinnedGpuComputeOutputBuffer =
        skinnedGpuComputeTargetsDynamicVertices ? smokeDynamicVertexBuffer : smokeSkinnedCurrentOutputVertexBuffer;
    const bool skinnedGpuComputeReady =
        gpuSkinningMode >= 1 &&
        m_smokeSkinnedGpuSkinningPipeline &&
        m_smokeSkinnedGpuSkinningBindingLayout &&
        smokeSkinnedSourceVertexBuffer &&
        smokeSkinnedGpuComputeOutputBuffer &&
        smokeSkinnedSurfaceDispatchBuffer &&
        smokeSkinnedCurrentJointMatrixBuffer &&
        smokeSkinnedPreviousPositionBuffer &&
        skinnedGpuComputeVertexCount > 0 &&
        skinnedGpuComputeMaxVertexCount > 0;
    const bool skinnedGpuComputeWritesPreviousPositions =
        skinnedGpuComputeReady &&
        smokeSkinnedPreviousPositionBuffer &&
        !skinnedGpuScaffold.previousPositions.empty() &&
        !skinnedGpuScaffold.previousJointMatrices.empty();
    bool skinnedGpuComputeDispatched = false;
    const bool skinnedGpuParitySentinelRequested =
        skinnedGpuComputeReady &&
        r_pathTracingGpuSkinningParityDump.GetInteger() != 0;
    std::vector<PathTraceSmokeVertex> skinnedGpuDynamicVertexSentinel;
    std::vector<PathTraceSmokeVertex> skinnedGpuCurrentOutputSentinel;
    std::vector<PathTraceSkinnedPreviousPosition> skinnedGpuPreviousPositionSentinel;
    const std::vector<PathTraceSmokeVertex>* dynamicVertexUploadData = &dynamicVertexData;
    const std::vector<PathTraceSkinnedPreviousPosition>* skinnedPreviousPositionUploadData =
        &skinnedGpuScaffold.previousPositions;
    int skinnedGpuCurrentSentinelVertices = 0;
    int skinnedGpuPreviousSentinelPositions = 0;
    if (skinnedGpuParitySentinelRequested)
    {
        if (skinnedGpuComputeTargetsDynamicVertices)
        {
            skinnedGpuDynamicVertexSentinel = dynamicVertexData;
            for (const PathTraceSkinnedSurfaceDispatchRecord& dispatch : skinnedGpuComputeDispatchRecords)
            {
                if ((dispatch.flags & PT_SKINNED_DISPATCH_HAS_CURRENT_JOINTS) == 0u ||
                    dispatch.dynamicVertexOffset > skinnedGpuDynamicVertexSentinel.size() ||
                    dispatch.vertexCount >
                        skinnedGpuDynamicVertexSentinel.size() - dispatch.dynamicVertexOffset)
                {
                    continue;
                }
                for (uint32_t vertexIndex = 0; vertexIndex < dispatch.vertexCount; ++vertexIndex)
                {
                    skinnedGpuDynamicVertexSentinel[dispatch.dynamicVertexOffset + vertexIndex] =
                        PathTraceSmokeVertex();
                }
                skinnedGpuCurrentSentinelVertices += static_cast<int>(dispatch.vertexCount);
            }
            dynamicVertexUploadData = &skinnedGpuDynamicVertexSentinel;
        }
        else
        {
            skinnedGpuCurrentOutputSentinel.resize(skinnedGpuScaffold.currentOutputVertices.size());
            skinnedGpuCurrentSentinelVertices =
                static_cast<int>(skinnedGpuCurrentOutputSentinel.size());
        }

        skinnedGpuPreviousPositionSentinel = skinnedGpuScaffold.previousPositions;
        for (const PathTraceSkinnedSurfaceDispatchRecord& dispatch : skinnedGpuComputeDispatchRecords)
        {
            if ((dispatch.flags & PT_SKINNED_DISPATCH_HAS_VALID_PREVIOUS) == 0u ||
                (dispatch.flags & PT_SKINNED_DISPATCH_HAS_PREVIOUS_JOINTS) == 0u ||
                dispatch.previousPositionOffset == UINT32_MAX ||
                dispatch.previousPositionOffset > skinnedGpuPreviousPositionSentinel.size() ||
                dispatch.vertexCount >
                    skinnedGpuPreviousPositionSentinel.size() - dispatch.previousPositionOffset)
            {
                continue;
            }
            for (uint32_t vertexIndex = 0; vertexIndex < dispatch.vertexCount; ++vertexIndex)
            {
                skinnedGpuPreviousPositionSentinel[dispatch.previousPositionOffset + vertexIndex] =
                    PathTraceSkinnedPreviousPosition();
            }
            skinnedGpuPreviousSentinelPositions += static_cast<int>(dispatch.vertexCount);
        }
        skinnedPreviousPositionUploadData = &skinnedGpuPreviousPositionSentinel;
    }
    const RtSmokeBufferUploadItem skinnedCurrentOutputUploadItem =
        skinnedGpuComputeReady && !skinnedGpuComputeTargetsDynamicVertices
            ? (skinnedGpuParitySentinelRequested
                ? MakeSmokeVectorUploadItem(
                    smokeSkinnedCurrentOutputVertexBuffer,
                    skinnedGpuCurrentOutputSentinel,
                    nvrhi::ResourceStates::UnorderedAccess,
                    false)
                : MakeSmokeBufferStateItem(
                    smokeSkinnedCurrentOutputVertexBuffer,
                    nvrhi::ResourceStates::UnorderedAccess))
            : (skinnedGpuComputeTargetsDynamicVertices
                ? MakeSmokeBufferStateItem(
                    smokeSkinnedCurrentOutputVertexBuffer,
                    nvrhi::ResourceStates::ShaderResource)
                : MakeSmokeVectorUploadItem(
                    smokeSkinnedCurrentOutputVertexBuffer,
                    skinnedGpuScaffold.currentOutputVertices,
                    nvrhi::ResourceStates::ShaderResource,
                    false));
    const RtSmokeBufferUploadItem skinnedCurrentJointUploadItem =
        jointCacheStage.ready
            ? MakeSmokeBufferStateItem(
                smokeSkinnedCurrentJointMatrixBuffer,
                nvrhi::ResourceStates::CopyDest)
            : MakeSmokeVectorUploadItem(
                smokeSkinnedCurrentJointMatrixBuffer,
                skinnedGpuScaffold.currentJointMatrices,
                nvrhi::ResourceStates::ShaderResource,
                false);

    RtSmokeStaticVertexUploadPlanInput staticVertexUploadPlanInput;
    staticVertexUploadPlanInput.forceRebuildWithoutUpload = forceStaticBlasRebuildWithoutUpload;
    staticVertexUploadPlanInput.staticBlasCacheHit = staticBlasCacheHit;
    staticVertexUploadPlanInput.useDirtyRangeUploads = useStaticDirtyRangeUploads;
    staticVertexUploadPlanInput.fullUploadOnCacheMissWithTexMatrices =
        r_pathTracingStaticVertexFullUploadOnCacheMiss.GetBool();
    staticVertexUploadPlanInput.dirtyVertexOffset = geometryUniverseStats.staticDirtyVertexOffset;
    staticVertexUploadPlanInput.dirtyVertexCount = geometryUniverseStats.staticDirtyVertexCount;
    staticVertexUploadPlanInput.texMatrixVertexCount = staticTexMatrixVertices;
    staticVertexUploadPlanInput.texMatrixFirstVertex = staticTexMatrixFirstVertex;
    staticVertexUploadPlanInput.texMatrixLastVertex = staticTexMatrixLastVertex;
    staticVertexUploadPlanInput.totalVertexCount = staticVertexFrameData.size();
    const RtSmokeStaticVertexUploadPlan staticVertexUploadPlan =
        BuildSmokeStaticVertexUploadPlan(staticVertexUploadPlanInput);

    const RtSmokeBufferUploadItem uploadItems[] = {
        MakeSmokeVectorUploadItem(
            smokeStaticVertexBuffer,
            staticVertexFrameData,
            nvrhi::ResourceStates::AccelStructBuildInput,
            staticVertexUploadPlan.skipUpload,
            staticVertexUploadPlan.elementOffset,
            staticVertexUploadPlan.elementCount),
        MakeSmokeVectorUploadItem(smokeStaticIndexBuffer, staticIndexCache, nvrhi::ResourceStates::AccelStructBuildInput, forceStaticBlasRebuildWithoutUpload || staticBlasCacheHit, useStaticDirtyRangeUploads ? geometryUniverseStats.staticDirtyIndexOffset : -1, geometryUniverseStats.staticDirtyIndexCount),
        MakeSmokeVectorUploadItem(smokeStaticTriangleClassBuffer, staticTriangleClassCache, nvrhi::ResourceStates::ShaderResource, staticBlasCacheHit, useStaticDirtyRangeUploads ? geometryUniverseStats.staticDirtyTriangleOffset : -1, geometryUniverseStats.staticDirtyTriangleCount),
        MakeSmokeVectorUploadItem(smokeStaticTriangleMaterialBuffer, staticTriangleMaterialCache, nvrhi::ResourceStates::ShaderResource, skipStaticTriangleMaterialUpload),
        MakeSmokeVectorUploadItem(smokeStaticTriangleMaterialIndexBuffer, materialTable.staticMaterialIndexes, nvrhi::ResourceStates::ShaderResource, skipStaticTriangleMaterialIndexUpload),
        MakeSmokeVectorUploadItem(smokePreviousStaticVertexBuffer, previousStaticVertexCache, nvrhi::ResourceStates::ShaderResource, skipPreviousStaticGeometryUpload),
        MakeSmokeVectorUploadItem(smokePreviousStaticIndexBuffer, previousStaticIndexCache, nvrhi::ResourceStates::ShaderResource, skipPreviousStaticGeometryUpload),
        MakeSmokeVectorUploadItem(smokePreviousStaticTriangleClassBuffer, previousStaticTriangleClassCache, nvrhi::ResourceStates::ShaderResource, skipPreviousStaticGeometryUpload),
        MakeSmokeVectorUploadItem(smokePreviousStaticTriangleMaterialBuffer, previousStaticTriangleMaterialCache, nvrhi::ResourceStates::ShaderResource, skipPreviousStaticGeometryUpload),
        MakeSmokeVectorUploadItem(smokePreviousStaticTriangleMaterialIndexBuffer, previousStaticTriangleMaterialIndexCache, nvrhi::ResourceStates::ShaderResource, skipPreviousStaticMaterialIndexUpload),
        MakeSmokeVectorUploadItem(smokeDynamicVertexBuffer, *dynamicVertexUploadData, skinnedGpuComputeReady && skinnedGpuComputeTargetsDynamicVertices ? nvrhi::ResourceStates::UnorderedAccess : nvrhi::ResourceStates::AccelStructBuildInput, false),
        MakeSmokeVectorUploadItem(smokeDynamicIndexBuffer, dynamicIndexData, nvrhi::ResourceStates::AccelStructBuildInput, false),
        MakeSmokeVectorUploadItem(smokeDynamicTriangleClassBuffer, dynamicTriangleClassData, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeDynamicTriangleMaterialBuffer, dynamicTriangleMaterialData, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeDynamicTriangleMaterialIndexBuffer, materialTable.dynamicMaterialIndexes, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeMaterialTableBuffer, gpuMaterialTableMaterials, nvrhi::ResourceStates::ShaderResource, skipMaterialTableUpload, materialTableUploadOffset, materialTableUploadCount),
        MakeSmokeVectorUploadItem(smokeMaterialFeatureBuffer, materialTable.materialFeatures, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeMaterialFeatureParameterBuffer, materialTable.materialFeatureParameters, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeDynamicMaterialBuffer, dynamicMaterialRecords, nvrhi::ResourceStates::ShaderResource, skipDynamicMaterialUpload, dynamicMaterialUploadOffset, dynamicMaterialUploadCount),
        MakeSmokeVectorUploadItem(smokeEmissiveTriangleBuffer, emissiveTriangles, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokePreviousEmissiveTriangleBuffer, previousEmissiveTriangles, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeEmissiveRemapBuffer, emissiveLightRemap, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeEmissiveDistributionBuffer, emissiveDistribution.entries, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeLightCandidateBuffer, lightCandidates, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeDoomAnalyticLightBuffer, doomAnalyticLights, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeDoomAnalyticPreviousLightBuffer, doomAnalyticRemap.previousCandidates, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeDoomAnalyticCurrentIdentityBuffer, doomAnalyticRemap.currentCandidateIdentities, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeDoomAnalyticPreviousIdentityBuffer, doomAnalyticRemap.previousCandidateIdentities, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeDoomAnalyticRemapBuffer, doomAnalyticRemap.universeRemap, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeUnifiedLightBuffer, unifiedLights.currentLights, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeUnifiedPreviousLightBuffer, unifiedLights.previousLights, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeUnifiedLightRemapBuffer, unifiedLights.currentToPreviousRemap, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeRestirLightManagerCurrentToPreviousBuffer, restirLightManagerCurrentToPreviousRemap, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeRestirLightManagerPreviousToCurrentBuffer, restirLightManagerPreviousToCurrentRemap, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeRestirLightManagerCurrentPayloadBuffer, restirLightManagerCurrentPayloadRecords, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeRestirLightManagerPreviousPayloadBuffer, restirLightManagerPreviousPayloadRecords, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeRigidRouteVertexBuffer, rigidRouteBuild.vertices, nvrhi::ResourceStates::ShaderResource, skipRigidRouteSideBufferUpload),
        MakeSmokeVectorUploadItem(smokeRigidRouteIndexBuffer, rigidRouteBuild.indexes, nvrhi::ResourceStates::ShaderResource, skipRigidRouteSideBufferUpload),
        MakeSmokeVectorUploadItem(smokeRigidRouteTriangleMaterialBuffer, rigidRouteBuild.triangleMaterials, nvrhi::ResourceStates::ShaderResource, skipRigidRouteSideBufferUpload),
        MakeSmokeVectorUploadItem(smokeRigidRouteTriangleMaterialIndexBuffer, rigidRouteBuild.triangleMaterialIndexes, nvrhi::ResourceStates::ShaderResource, skipRigidRouteSideBufferUpload),
        MakeSmokeVectorUploadItem(smokeRigidRouteInstanceBuffer, rigidRouteBuild.instances, nvrhi::ResourceStates::ShaderResource, skipRigidRouteInstanceBufferUpload),
        MakeSmokeVectorUploadItem(smokeSkinnedHitRouteRecordBuffer, skinnedHitRouteGpuUpload.records, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeSkinnedHitRouteTriangleBuffer, skinnedHitRouteGpuUpload.triangles, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeSkinnedSourceVertexBuffer, skinnedGpuScaffold.sourceVertices, nvrhi::ResourceStates::ShaderResource, false),
        skinnedCurrentOutputUploadItem,
        MakeSmokeVectorUploadItem(smokeSkinnedPreviousPositionBuffer, *skinnedPreviousPositionUploadData, skinnedGpuComputeReady ? nvrhi::ResourceStates::UnorderedAccess : nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeSkinnedSurfaceDispatchBuffer, skinnedGpuComputeDispatchRecords, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeSkinnedTriangleDispatchIndexBuffer, skinnedGpuScaffold.dynamicTriangleDispatchIndexes, nvrhi::ResourceStates::ShaderResource, false),
        skinnedCurrentJointUploadItem,
        MakeSmokeVectorUploadItem(smokeSkinnedPreviousJointMatrixBuffer, skinnedGpuScaffold.previousJointMatrices, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeSkinnedEmissiveWorkBuffer, skinnedEmissiveGpuWork, nvrhi::ResourceStates::ShaderResource, false)
    };
    RtSmokeBufferUploadBatchDesc uploadBatchDesc;
    uploadBatchDesc.commandList = commandList;
    uploadBatchDesc.items = uploadItems;
    uploadBatchDesc.itemCount = static_cast<int>(sizeof(uploadItems) / sizeof(uploadItems[0]));
    int bufferUploadMs = 0;
    {
        OPTICK_EVENT("PT Upload Scene Buffers");
        if (optickGpuMarkers)
        {
            OPTICK_GPU_EVENT("PT GPU Upload Scene Buffers");
            bufferUploadMs = UploadSmokeAccelerationBuffers(uploadBatchDesc);
        }
        else
        {
            bufferUploadMs = UploadSmokeAccelerationBuffers(uploadBatchDesc);
        }
    }
    if (!m_skinnedHitRouteReadbackCompleted &&
        !m_skinnedHitRouteReadbackQueued &&
        !skinnedHitRouteUploadBuild.records.empty())
    {
        QueueSkinnedHitRouteReadback(
            commandList,
            smokeSkinnedHitRouteRecordBuffer,
            smokeSkinnedHitRouteTriangleBuffer,
            skinnedHitRouteGpuUpload,
            m_smokeGeometryFrameIndex);
    }
    if (jointCacheStage.ready)
    {
        OPTICK_EVENT("PT Skinned JointCache Stage Copies");
        if (!SubmitSmokeJointCacheStageCopies(
                commandList,
                smokeSkinnedCurrentJointMatrixBuffer,
                jointCacheStage))
        {
            common->Printf(
                "PathTracePrimaryPass: GEO07 renderer jointCache staging copy validation failed; GPU comparison dispatch suppressed\n");
        }
    }
    const bool skinnedGpuJointInputReady =
        !jointCacheStage.ready ||
        jointCacheStage.submitted;
    {
        OPTICK_EVENT("PT Skinned GPU Compute Dispatch");
        if (skinnedGpuComputeReady &&
            skinnedGpuJointInputReady)
        {
            nvrhi::BufferHandle previousJointMatrixBuffer = smokeSkinnedPreviousJointMatrixBuffer ? smokeSkinnedPreviousJointMatrixBuffer : smokeSkinnedCurrentJointMatrixBuffer;
            const nvrhi::BufferHandle previousBoundPreviousJointMatrixBuffer =
                m_smokeSkinnedPreviousJointMatrixBuffer ? m_smokeSkinnedPreviousJointMatrixBuffer : m_smokeSkinnedCurrentJointMatrixBuffer;
            const bool skinningBindingSetReusable =
                m_smokeSkinnedGpuSkinningBindingSet &&
                smokeSkinnedSourceVertexBuffer == m_smokeSkinnedSourceVertexBuffer &&
                smokeSkinnedGpuComputeOutputBuffer == m_smokeSkinnedGpuSkinningOutputBuffer &&
                smokeSkinnedPreviousPositionBuffer == m_smokeSkinnedGpuSkinningPreviousPositionBuffer &&
                smokeSkinnedSurfaceDispatchBuffer == m_smokeSkinnedSurfaceDispatchBuffer &&
                smokeSkinnedCurrentJointMatrixBuffer == m_smokeSkinnedCurrentJointMatrixBuffer &&
                previousJointMatrixBuffer == previousBoundPreviousJointMatrixBuffer;
            if (!skinningBindingSetReusable)
            {
                nvrhi::BindingSetDesc skinningBindingSetDesc;
                skinningBindingSetDesc.bindings = {
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(0, smokeSkinnedSourceVertexBuffer),
                    nvrhi::BindingSetItem::StructuredBuffer_UAV(0, smokeSkinnedGpuComputeOutputBuffer),
                    nvrhi::BindingSetItem::StructuredBuffer_UAV(1, smokeSkinnedPreviousPositionBuffer),
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(1, smokeSkinnedSurfaceDispatchBuffer),
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(2, smokeSkinnedCurrentJointMatrixBuffer),
                    nvrhi::BindingSetItem::StructuredBuffer_SRV(3, previousJointMatrixBuffer)
                };
                m_smokeSkinnedGpuSkinningBindingSet = device->createBindingSet(skinningBindingSetDesc, m_smokeSkinnedGpuSkinningBindingLayout);
                m_smokeSkinnedGpuSkinningOutputBuffer = m_smokeSkinnedGpuSkinningBindingSet ? smokeSkinnedGpuComputeOutputBuffer : nullptr;
                m_smokeSkinnedGpuSkinningPreviousPositionBuffer = m_smokeSkinnedGpuSkinningBindingSet ? smokeSkinnedPreviousPositionBuffer : nullptr;
            }
            if (m_smokeSkinnedGpuSkinningBindingSet)
            {
                nvrhi::ComputeState skinningComputeState;
                skinningComputeState.pipeline = m_smokeSkinnedGpuSkinningPipeline;
                skinningComputeState.bindings = { m_smokeSkinnedGpuSkinningBindingSet };
                commandList->setComputeState(skinningComputeState);
                const uint32_t groupsX = (static_cast<uint32_t>(skinnedGpuComputeMaxVertexCount) + 63u) / 64u;
                commandList->dispatch(groupsX, static_cast<uint32_t>(skinnedGpuComputeDispatchRecords.size()), 1);
                commandList->setBufferState(
                    smokeSkinnedGpuComputeOutputBuffer,
                    skinnedGpuComputeTargetsDynamicVertices ? nvrhi::ResourceStates::AccelStructBuildInput : nvrhi::ResourceStates::ShaderResource);
                commandList->setBufferState(smokeSkinnedPreviousPositionBuffer, nvrhi::ResourceStates::ShaderResource);
                commandList->commitBarriers();
                skinnedGpuComputeDispatched = true;
            }
        }
        else
        {
            m_smokeSkinnedGpuSkinningBindingSet = nullptr;
            m_smokeSkinnedGpuSkinningOutputBuffer = nullptr;
            m_smokeSkinnedGpuSkinningPreviousPositionBuffer = nullptr;
        }
    }
    FinalizeSmokeSkinnedGpuFunnel(skinnedGpuScaffold, skinnedGpuComputeDispatched);
    if (r_pathTracingGeometrySkinnedEmissiveAudit.GetInteger() == 1 &&
        skinnedGpuComputeDispatched &&
        !skinnedHitRouteUploadBuild.records.empty())
    {
        std::unordered_set<uint32_t> routedMaterialIndexes;
        for (const PtSkinnedHitRouteTriangle& triangle :
            skinnedHitRouteUploadBuild.triangles)
        {
            if (triangle.materialIndex <
                materialTable.materials.size())
            {
                routedMaterialIndexes.insert(
                    triangle.materialIndex);
            }
        }

        std::vector<PathTraceSmokeMaterial>
            auditMaterialViews = materialTable.materials;
        int productionEligibleMaterialCount = 0;
        int forcedMaterialCount = 0;
        for (uint32_t materialIndex :
            routedMaterialIndexes)
        {
            PathTraceSmokeMaterial& material =
                auditMaterialViews[materialIndex];
            if ((material.flags &
                    RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE) !=
                0u)
            {
                ++productionEligibleMaterialCount;
                continue;
            }
            if (materialIndex >=
                materialTable.materialInfos.size())
            {
                continue;
            }
            const RtSmokeMaterialTextureInfo& info =
                materialTable.materialInfos[materialIndex];
            if (!info.emissive ||
                !info.emissiveLightCandidate)
            {
                continue;
            }

            // Validation only: the production material universe correctly
            // rejects an authored emissive image without a safe RT texture
            // handle. Use its discovered constant stage color solely to prove
            // canonical GPU-output geometry, identity, and temporal remap.
            material.flags |=
                RT_SMOKE_MATERIAL_EMISSIVE |
                RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE;
            material.emissiveColor[0] =
                info.emissiveColor.x;
            material.emissiveColor[1] =
                info.emissiveColor.y;
            material.emissiveColor[2] =
                info.emissiveColor.z;
            material.emissiveColor[3] =
                info.emissiveColor.w;
            ++forcedMaterialCount;
        }
        if (forcedMaterialCount == 0 &&
            skinnedEmissivePublishValidationFallbackMaterial !=
                UINT32_MAX &&
            skinnedEmissivePublishValidationFallbackMaterial <
                auditMaterialViews.size())
        {
            PathTraceSmokeMaterial& material =
                auditMaterialViews[
                    skinnedEmissivePublishValidationFallbackMaterial];
            material.flags |=
                RT_SMOKE_MATERIAL_EMISSIVE |
                RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE;
            material.emissiveColor[0] = 1.0f;
            material.emissiveColor[1] = 0.5f;
            material.emissiveColor[2] = 0.25f;
            material.emissiveColor[3] = 1.0f;
            ++forcedMaterialCount;
        }

        std::vector<PtSkinnedEmissiveAuditTriangle>
            auditTriangles;
        auditTriangles.reserve(
            skinnedHitRouteUploadBuild.triangles.size());
        for (const PtSkinnedHitRouteRecord& route :
            skinnedHitRouteUploadBuild.records)
        {
            const PtGeometrySourceRecord* source =
                m_smokeGeometryUniverse.
                    FindCanonicalSourceRecord(route.meshKey);
            for (uint32_t localPrimitive = 0;
                localPrimitive < route.triangleCount;
                ++localPrimitive)
            {
                const uint64 metadataIndex =
                    static_cast<uint64>(
                        route.triangleMetadataOffset) +
                    localPrimitive;
                if (metadataIndex >=
                    skinnedHitRouteUploadBuild.
                        triangles.size())
                {
                    continue;
                }
                const PtSkinnedHitRouteTriangle& triangle =
                    skinnedHitRouteUploadBuild.triangles[
                        static_cast<size_t>(metadataIndex)];
                PtSkinnedEmissiveAuditTriangle audit = {};
                audit.materialIndex =
                    triangle.materialIndex;
                audit.materialId = triangle.materialId;
                audit.instanceId = route.shaderInstanceId;
                audit.primitiveIndex =
                    triangle.sourcePrimitiveIndex;
                audit.triangleClassAndFlags =
                    triangle.triangleClassAndFlags;
                if (skinnedEmissivePublishValidationFallbackMaterial !=
                        UINT32_MAX &&
                    audit.materialIndex ==
                        skinnedEmissivePublishValidationFallbackMaterial)
                {
                    audit.triangleClassAndFlags &=
                        ~RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF;
                }
                audit.identityHash =
                    triangle.emissiveIdentityHash;
                audit.hasPrevious =
                    (route.flags &
                        PT_SKINNED_HIT_ROUTE_HAS_PREVIOUS) !=
                        0u &&
                    route.previousPositionOffset !=
                        PT_SKINNED_HIT_ROUTE_INVALID_INDEX;
                for (int corner = 0; corner < 3; ++corner)
                {
                    audit.currentVertexIndexes[corner] =
                        UINT32_MAX;
                    audit.previousPositionIndexes[corner] =
                        UINT32_MAX;
                }
                const uint64 sourceIndexOffset =
                    static_cast<uint64>(
                        triangle.sourcePrimitiveIndex) *
                    3ull;
                if (source &&
                    sourceIndexOffset + 2ull <
                        source->payload.indexes.size())
                {
                    for (int corner = 0;
                        corner < 3;
                        ++corner)
                    {
                        const uint32_t localVertex =
                            source->payload.indexes[
                                static_cast<size_t>(
                                    sourceIndexOffset +
                                    corner)];
                        audit.currentVertexIndexes[corner] =
                            route.outputVertexOffset +
                            localVertex;
                        if (audit.hasPrevious)
                        {
                            audit.previousPositionIndexes[
                                corner] =
                                    route.
                                        previousPositionOffset +
                                    localVertex;
                        }
                    }
                }
                auditTriangles.push_back(audit);
            }
        }

        QueueSkinnedEmissiveAudit(
            commandList,
            smokeSkinnedGpuComputeOutputBuffer,
            smokeSkinnedPreviousPositionBuffer,
            skinnedGpuComputeTargetsDynamicVertices
                ? nvrhi::ResourceStates::
                    AccelStructBuildInput
                : nvrhi::ResourceStates::ShaderResource,
            auditTriangles,
            materialTable.materialIds,
            auditMaterialViews,
            skinnedGpuScaffold.currentOutputVertices,
            skinnedGpuScaffold.previousPositions,
            geometryUniverseStats.frameIndex,
            forcedMaterialCount,
            productionEligibleMaterialCount);
    }
    if (r_pathTracingGpuSkinningParityDump.GetInteger() != 0)
    {
        DumpSmokeSkinnedGpuFunnel(
            skinnedGpuScaffold,
            jointCacheStage,
            skinnedHistoryAudit,
            skinnedOutputAudit,
            currentSkinnedSurfaceRecords,
            m_smokeGeometryUniverse,
            gpuSkinningMode,
            geometryUniverseStats.frameIndex);
        common->Printf(
            "PathTracePrimaryPass: PT GPU skinning parity sentinel mode=%d currentVertices=%d previousPositions=%d applied=%d\n",
            gpuSkinningMode,
            skinnedGpuCurrentSentinelVertices,
            skinnedGpuPreviousSentinelPositions,
            skinnedGpuParitySentinelRequested ? 1 : 0);
        common->Printf(
            "PathTracePrimaryPass: GEO09 skinned material-state audit accepted=%d sharedTable=1 dispatches=%llu triangles=%llu tableRows=%llu dynamicRecords=%llu texMatrix(dispatches/vertices)=%llu/%llu mismatch(range/mixedMaterial/index/id/tableRow/dynamicRecord)=%llu/%llu/%llu/%llu/%llu/%llu routedUnsafe(diffuse/alpha/normal/specular/emissive)=%llu/%llu/%llu/%llu/%llu\n",
            skinnedMaterialStateAudit.Accepted() ? 1 : 0,
            static_cast<unsigned long long>(
                skinnedMaterialStateAudit.dispatches),
            static_cast<unsigned long long>(
                skinnedMaterialStateAudit.triangles),
            static_cast<unsigned long long>(
                skinnedMaterialStateAudit.tableRows),
            static_cast<unsigned long long>(
                skinnedMaterialStateAudit.dynamicRecords),
            static_cast<unsigned long long>(
                skinnedMaterialStateAudit.texMatrixDispatches),
            static_cast<unsigned long long>(
                skinnedMaterialStateAudit.texMatrixVertices),
            static_cast<unsigned long long>(
                skinnedMaterialStateAudit.invalidTriangleRange),
            static_cast<unsigned long long>(
                skinnedMaterialStateAudit.mixedMaterialDispatches),
            static_cast<unsigned long long>(
                skinnedMaterialStateAudit.invalidMaterialIndex),
            static_cast<unsigned long long>(
                skinnedMaterialStateAudit.materialIdMismatch),
            static_cast<unsigned long long>(
                skinnedMaterialStateAudit.incompleteTableRow),
            static_cast<unsigned long long>(
                skinnedMaterialStateAudit.dynamicRecordMismatch),
            static_cast<unsigned long long>(
                skinnedMaterialStateAudit.routedUnsafeDiffuse),
            static_cast<unsigned long long>(
                skinnedMaterialStateAudit.routedUnsafeAlpha),
            static_cast<unsigned long long>(
                skinnedMaterialStateAudit.routedUnsafeNormal),
            static_cast<unsigned long long>(
                skinnedMaterialStateAudit.routedUnsafeSpecular),
            static_cast<unsigned long long>(
                skinnedMaterialStateAudit.routedUnsafeEmissive));

        if (skinnedGpuComputeDispatched)
        {
            constexpr int maxParitySamples = 24;
            std::vector<GpuSkinningParitySample> paritySamples;
            paritySamples.reserve(maxParitySamples);
            std::unordered_set<uint64> paritySampleKeys;
            std::vector<idStr> sampledModelNames;
            auto appendParitySample = [&](int dispatchIndex, uint32_t localVertex)
            {
                if (dispatchIndex < 0 ||
                    dispatchIndex >= static_cast<int>(skinnedGpuComputeDispatchRecords.size()) ||
                    dispatchIndex >= static_cast<int>(skinnedGpuScaffold.dispatchRecords.size()) ||
                    static_cast<int>(paritySamples.size()) >= maxParitySamples)
                {
                    return;
                }
                const PathTraceSkinnedSurfaceDispatchRecord& dispatch =
                    skinnedGpuComputeDispatchRecords[dispatchIndex];
                if ((dispatch.flags & PT_SKINNED_DISPATCH_HAS_CURRENT_JOINTS) == 0u ||
                    dispatch.surfaceRecordIndex >= currentSkinnedSurfaceRecords.size() ||
                    localVertex >= dispatch.vertexCount)
                {
                    return;
                }
                const PathTraceSkinnedSurfaceDispatchRecord& scaffoldDispatch =
                    skinnedGpuScaffold.dispatchRecords[dispatchIndex];
                const RtSmokeSkinnedSurfaceRecord& record =
                    currentSkinnedSurfaceRecords[dispatch.surfaceRecordIndex];
                if (scaffoldDispatch.sourceVertexOffset == UINT32_MAX ||
                    scaffoldDispatch.outputVertexOffset == UINT32_MAX)
                {
                    return;
                }
                const uint64 sampleKey =
                    (static_cast<uint64>(static_cast<uint32_t>(dispatchIndex)) << 32ull) |
                    static_cast<uint64>(localVertex);
                if (!paritySampleKeys.insert(sampleKey).second)
                {
                    return;
                }
                const uint64 sourceIndex =
                    static_cast<uint64>(scaffoldDispatch.sourceVertexOffset) + localVertex;
                const uint64 cpuCurrentIndex =
                    static_cast<uint64>(scaffoldDispatch.outputVertexOffset) + localVertex;
                if (sourceIndex >= skinnedGpuScaffold.sourceVertices.size() ||
                    cpuCurrentIndex >= skinnedGpuScaffold.currentOutputVertices.size())
                {
                    return;
                }

                GpuSkinningParitySample sample;
                sample.modelName = record.modelName;
                sample.entityIndex = record.entityIndex;
                sample.drawSurfIndex = record.drawSurfIndex;
                sample.surfaceRecordIndex = static_cast<int>(dispatch.surfaceRecordIndex);
                sample.vertexIndex = static_cast<int>(localVertex);
                sample.currentByteOffset =
                    (static_cast<uint64>(dispatch.outputVertexOffset) + localVertex) *
                    sizeof(PathTraceSmokeVertex);
                sample.previousInvalidReasonFlags = record.invalidReasonFlags;
                sample.temporalStateFlags = record.temporalStateFlags;
                const int sampleMaterialIndex =
                    FindSmokeMaterialTableIndexById(
                        materialTable,
                        record.materialId);
                if (sampleMaterialIndex >= 0)
                {
                    sample.materialIndex =
                        static_cast<uint32_t>(
                            sampleMaterialIndex);
                }
                sample.hasDynamicTexMatrix =
                    (dispatch.flags &
                        PT_SKINNED_DISPATCH_HAS_TEX_MATRIX) != 0u;
                sample.source =
                    skinnedGpuScaffold.sourceVertices[static_cast<size_t>(sourceIndex)];
                sample.cpuCurrent =
                    skinnedGpuScaffold.currentOutputVertices[static_cast<size_t>(cpuCurrentIndex)];
                sample.hasPrevious =
                    (dispatch.flags & PT_SKINNED_DISPATCH_HAS_VALID_PREVIOUS) != 0u &&
                    (dispatch.flags & PT_SKINNED_DISPATCH_HAS_PREVIOUS_JOINTS) != 0u &&
                    scaffoldDispatch.previousPositionOffset != UINT32_MAX;
                if (sample.hasPrevious)
                {
                    const uint64 previousIndex =
                        static_cast<uint64>(scaffoldDispatch.previousPositionOffset) + localVertex;
                    if (previousIndex >= skinnedGpuScaffold.previousPositions.size())
                    {
                        sample.hasPrevious = false;
                    }
                    else
                    {
                        sample.previousByteOffset =
                            previousIndex * sizeof(PathTraceSkinnedPreviousPosition);
                        sample.cpuPrevious =
                            skinnedGpuScaffold.previousPositions[static_cast<size_t>(previousIndex)];
                    }
                }
                paritySamples.push_back(sample);
            };
            auto representativeVertex = [&](int dispatchIndex) -> uint32_t
            {
                if (dispatchIndex < 0 ||
                    dispatchIndex >= static_cast<int>(skinnedGpuComputeDispatchRecords.size()) ||
                    dispatchIndex >= static_cast<int>(skinnedGpuScaffold.dispatchRecords.size()))
                {
                    return UINT32_MAX;
                }
                const PathTraceSkinnedSurfaceDispatchRecord& dispatch =
                    skinnedGpuComputeDispatchRecords[dispatchIndex];
                const PathTraceSkinnedSurfaceDispatchRecord& scaffoldDispatch =
                    skinnedGpuScaffold.dispatchRecords[dispatchIndex];
                if (dispatch.vertexCount == 0u ||
                    scaffoldDispatch.sourceVertexOffset == UINT32_MAX)
                {
                    return UINT32_MAX;
                }
                uint32_t bestVertex = 0u;
                int bestInfluenceCount = -1;
                for (uint32_t localVertex = 0u; localVertex < dispatch.vertexCount; ++localVertex)
                {
                    const uint64 sourceIndex =
                        static_cast<uint64>(scaffoldDispatch.sourceVertexOffset) + localVertex;
                    if (sourceIndex >= skinnedGpuScaffold.sourceVertices.size())
                    {
                        break;
                    }
                    const PathTraceSkinnedSourceVertex& source =
                        skinnedGpuScaffold.sourceVertices[static_cast<size_t>(sourceIndex)];
                    int influenceCount = 0;
                    for (int component = 0; component < 4; ++component)
                    {
                        influenceCount += source.jointWeights[component] > 0.0f ? 1 : 0;
                    }
                    if (influenceCount > bestInfluenceCount)
                    {
                        bestInfluenceCount = influenceCount;
                        bestVertex = localVertex;
                    }
                }
                return bestVertex;
            };
            auto modelAlreadySampled = [&](const idStr& modelName)
            {
                for (const idStr& sampledModelName : sampledModelNames)
                {
                    if (idStr::Icmp(sampledModelName.c_str(), modelName.c_str()) == 0)
                    {
                        return true;
                    }
                }
                return false;
            };

            // First reserve every observed dynamic texture-matrix surface. UV
            // parity is the GEO-09 material-state seam and must not be crowded
            // out by the broader position/motion sample set.
            for (int dispatchIndex = 0;
                dispatchIndex <
                    static_cast<int>(
                        skinnedGpuComputeDispatchRecords.size()) &&
                static_cast<int>(paritySamples.size()) <
                    maxParitySamples;
                ++dispatchIndex)
            {
                if ((skinnedGpuComputeDispatchRecords[dispatchIndex].flags &
                        PT_SKINNED_DISPATCH_HAS_TEX_MATRIX) != 0u)
                {
                    appendParitySample(
                        dispatchIndex,
                        representativeVertex(dispatchIndex));
                }
            }
            // Then reserve samples for the player and requested monster probes.
            for (int dispatchIndex = 0;
                dispatchIndex < static_cast<int>(skinnedGpuComputeDispatchRecords.size()) &&
                static_cast<int>(paritySamples.size()) < maxParitySamples;
                ++dispatchIndex)
            {
                const PathTraceSkinnedSurfaceDispatchRecord& dispatch =
                    skinnedGpuComputeDispatchRecords[dispatchIndex];
                if (dispatch.surfaceRecordIndex >= currentSkinnedSurfaceRecords.size())
                {
                    continue;
                }
                const RtSmokeSkinnedSurfaceRecord& record =
                    currentSkinnedSurfaceRecords[dispatch.surfaceRecordIndex];
                const bool priorityIdentity =
                    record.entityIndex == 0 ||
                    idStr::FindText(record.modelName.c_str(), "zfat", false) >= 0 ||
                    idStr::FindText(record.modelName.c_str(), "zombie", false) >= 0 ||
                    idStr::FindText(record.modelName.c_str(), "zsec", false) >= 0;
                if (priorityIdentity)
                {
                    appendParitySample(dispatchIndex, representativeVertex(dispatchIndex));
                }
            }
            // Then cover each distinct model and every observed single-bone surface.
            for (int dispatchIndex = 0;
                dispatchIndex < static_cast<int>(skinnedGpuComputeDispatchRecords.size()) &&
                static_cast<int>(paritySamples.size()) < maxParitySamples;
                ++dispatchIndex)
            {
                const PathTraceSkinnedSurfaceDispatchRecord& dispatch =
                    skinnedGpuComputeDispatchRecords[dispatchIndex];
                if (dispatch.surfaceRecordIndex >= currentSkinnedSurfaceRecords.size())
                {
                    continue;
                }
                const RtSmokeSkinnedSurfaceRecord& record =
                    currentSkinnedSurfaceRecords[dispatch.surfaceRecordIndex];
                if (!modelAlreadySampled(record.modelName))
                {
                    appendParitySample(dispatchIndex, representativeVertex(dispatchIndex));
                    sampledModelNames.push_back(record.modelName);
                }
            }
            for (int dispatchIndex = 0;
                dispatchIndex < static_cast<int>(skinnedGpuComputeDispatchRecords.size()) &&
                static_cast<int>(paritySamples.size()) < maxParitySamples;
                ++dispatchIndex)
            {
                const PathTraceSkinnedSurfaceDispatchRecord& dispatch =
                    skinnedGpuComputeDispatchRecords[dispatchIndex];
                if (dispatch.surfaceRecordIndex >= currentSkinnedSurfaceRecords.size())
                {
                    continue;
                }
                const RtSmokeSkinnedSurfaceRecord& record =
                    currentSkinnedSurfaceRecords[dispatch.surfaceRecordIndex];
                if (IsEntityFeedSingleBoneSurface(
                        reinterpret_cast<const srfTriangles_t*>(record.key.tri)))
                {
                    appendParitySample(dispatchIndex, representativeVertex(dispatchIndex));
                }
            }
            // Cover as many remaining surfaces as the bound permits, then add
            // first/middle/last vertices for additional within-surface spread.
            for (int dispatchIndex = 0;
                dispatchIndex < static_cast<int>(skinnedGpuComputeDispatchRecords.size()) &&
                static_cast<int>(paritySamples.size()) < maxParitySamples;
                ++dispatchIndex)
            {
                appendParitySample(dispatchIndex, representativeVertex(dispatchIndex));
            }
            for (int dispatchIndex = 0;
                dispatchIndex < static_cast<int>(skinnedGpuComputeDispatchRecords.size()) &&
                static_cast<int>(paritySamples.size()) < maxParitySamples;
                ++dispatchIndex)
            {
                const PathTraceSkinnedSurfaceDispatchRecord& dispatch =
                    skinnedGpuComputeDispatchRecords[dispatchIndex];
                appendParitySample(dispatchIndex, 0u);
                appendParitySample(dispatchIndex, dispatch.vertexCount / 2u);
                appendParitySample(
                    dispatchIndex,
                    dispatch.vertexCount > 0u ? dispatch.vertexCount - 1u : 0u);
            }

            QueueGpuSkinningParitySamples(
                commandList,
                smokeSkinnedGpuComputeOutputBuffer,
                smokeSkinnedPreviousPositionBuffer,
                skinnedGpuComputeTargetsDynamicVertices
                    ? nvrhi::ResourceStates::AccelStructBuildInput
                    : nvrhi::ResourceStates::ShaderResource,
                paritySamples,
                gpuSkinningMode,
                geometryUniverseStats.frameIndex);
        }
        else if (gpuSkinningMode > 0)
        {
            common->Printf("PathTracePrimaryPass: PT GPU skinning parity readback unavailable because compute did not dispatch\n");
        }
        else
        {
            common->Printf("PathTracePrimaryPass: PT GPU skinning parity readback requires r_pathTracingGpuSkinning 1 or 2\n");
        }
        r_pathTracingGpuSkinningParityDump.SetInteger(0);
    }

    uint32 skinnedSourceIndexCount = 0;
    for (const RtSmokeSkinnedSurfaceRecord& record :
        currentSkinnedSurfaceRecords)
    {
        skinnedSourceIndexCount +=
            static_cast<uint32>(Max(record.indexCount, 0));
    }
    GeometrySkinnedGpuTimerSlot* skinnedBlasGpuTimer =
        nullptr;
    GeometrySkinnedGpuTimerSlot* dynamicBlasGpuTimer =
        nullptr;
    const int requestedGeometryTimingFrames =
        idMath::ClampInt(
            0,
            240,
            r_pathTracingGeometrySkinnedTiming.
                GetInteger());
    if (requestedGeometryTimingFrames > 0)
    {
        auto acquireGeometryTimer =
            [&](bool skinnedBlas)
                -> GeometrySkinnedGpuTimerSlot*
            {
                for (int slotOffset = 0;
                    slotOffset <
                        GEOMETRY_SKINNED_GPU_TIMER_SLOTS;
                    ++slotOffset)
                {
                    const uint32 slotIndex =
                        (m_geometrySkinnedGpuTimerCursor +
                            static_cast<uint32>(
                                slotOffset)) %
                        GEOMETRY_SKINNED_GPU_TIMER_SLOTS;
                    GeometrySkinnedGpuTimerSlot& candidate =
                        m_geometrySkinnedGpuTimers[
                            slotIndex];
                    if (candidate.pending)
                    {
                        continue;
                    }
                    if (!candidate.query)
                    {
                        candidate.query =
                            device->createTimerQuery();
                    }
                    if (!candidate.query)
                    {
                        continue;
                    }
                    m_geometrySkinnedGpuTimerCursor =
                        (slotIndex + 1u) %
                        GEOMETRY_SKINNED_GPU_TIMER_SLOTS;
                    candidate.pending = true;
                    candidate.skinnedBlas = skinnedBlas;
                    candidate.submitted = false;
                    candidate.earliestPollFrame =
                        idLib::frameNumber +
                        static_cast<int>(NUM_FRAME_DATA);
                    candidate.geometryFrame =
                        geometryUniverseStats.frameIndex;
                    candidate.buildCount = 0;
                    candidate.updateCount = 0;
                    candidate.rebuildCount = 0;
                    candidate.reuseCount = 0;
                    candidate.skinnedSurfaceCount =
                        static_cast<uint32>(
                            currentSkinnedSurfaceRecords.
                                size());
                    candidate.skinnedSourceIndexes =
                        skinnedSourceIndexCount;
                    candidate.cpuCapturedSkinnedIndexes =
                        skinnedSourceIndexCount >=
                                static_cast<uint32>(Max(
                                    captureTiming.
                                        skinnedCaptureOmittedIndexes,
                                    0))
                            ? skinnedSourceIndexCount -
                                static_cast<uint32>(Max(
                                    captureTiming.
                                        skinnedCaptureOmittedIndexes,
                                    0))
                            : 0u;
                    candidate.dynamicBlasIndexes =
                        static_cast<uint32>(
                            dynamicIndexData.size());
                    candidate.cpuSkinUs =
                        captureTiming.
                            rtCpuSkinningAppendUs;
                    candidate.cpuBlasSubmitUs = 0;
                    candidate.cpuTlasSubmitUs = 0;
                    candidate.cpuAccelSubmitUs = 0;
                    return &candidate;
                }
                return nullptr;
            };
        skinnedBlasGpuTimer =
            acquireGeometryTimer(true);
        dynamicBlasGpuTimer =
            acquireGeometryTimer(false);
        if (skinnedBlasGpuTimer ||
            dynamicBlasGpuTimer)
        {
            r_pathTracingGeometrySkinnedTiming.
                SetInteger(
                    requestedGeometryTimingFrames - 1);
        }
    }

    if (skinnedBlasGpuTimer)
    {
        commandList->beginTimerQuery(
            skinnedBlasGpuTimer->query);
    }
    const RtSmokeSkinnedComparisonBlasAudit
        skinnedComparisonBlasAudit =
            SubmitSmokeSkinnedComparisonBlases(
                currentSkinnedSurfaceRecords,
                m_smokeGeometryUniverse,
                m_smokeSkinnedBlasStateTable,
                m_smokeSkinnedComparisonBlases,
                m_retiredSmokeSkinnedComparisonBlases,
                device,
                commandList,
                smokeSkinnedCurrentOutputVertexBuffer,
                bufferCreateDesc.
                    skinnedOutputStorageGeneration,
                canonicalSkinnedSourceOutputRoute &&
                    r_pathTracingGeometrySkinnedTlasCompare.
                        GetInteger() != 0,
                geometryUniverseStats.frameIndex,
                idMath::ClampInt(
                    0,
                    32,
                    r_pathTracingSceneRetireFrames.
                        GetInteger()),
                m_smokeSkinnedComparisonCompletionQueryFailureLogged);
    if (skinnedBlasGpuTimer)
    {
        commandList->endTimerQuery(
            skinnedBlasGpuTimer->query);
        skinnedBlasGpuTimer->submitted =
            skinnedComparisonBlasAudit.buildSubmitted > 0 ||
            skinnedComparisonBlasAudit.updateSubmitted > 0 ||
            skinnedComparisonBlasAudit.rebuildSubmitted > 0;
        skinnedBlasGpuTimer->buildCount =
            static_cast<uint32>(Max(
                skinnedComparisonBlasAudit.buildSubmitted,
                0));
        skinnedBlasGpuTimer->updateCount =
            static_cast<uint32>(Max(
                skinnedComparisonBlasAudit.updateSubmitted,
                0));
        skinnedBlasGpuTimer->rebuildCount =
            static_cast<uint32>(Max(
                skinnedComparisonBlasAudit.rebuildSubmitted,
                0));
        skinnedBlasGpuTimer->reuseCount =
            static_cast<uint32>(Max(
                skinnedComparisonBlasAudit.reused,
                0));
    }
    if (skinnedComparisonBlasAudit.buildSubmitted > 0 &&
        !m_smokeSkinnedComparisonBlasBuildLogged)
    {
        common->Printf(
            "PathTracePrimaryPass: GEO08 skinned comparison BLAS initial frame=%llu gate=%d candidates/pending(build/update/rebuild)/exact=%d/%d/%d/%d/%d resources(created/reused/replaced/active)=%d/%d/%d/%d submitted(build/update/rebuild)/deferred/failed=%d/%d/%d/%d/%d flags=allow-update+prefer-fast-build tlas=excluded\n",
            static_cast<unsigned long long>(
                geometryUniverseStats.frameIndex),
            skinnedComparisonBlasAudit.gate ? 1 : 0,
            skinnedComparisonBlasAudit.candidates,
            skinnedComparisonBlasAudit.buildPending,
            skinnedComparisonBlasAudit.updatePending,
            skinnedComparisonBlasAudit.rebuildPending,
            skinnedComparisonBlasAudit.exactContracts,
            skinnedComparisonBlasAudit.created,
            skinnedComparisonBlasAudit.reused,
            skinnedComparisonBlasAudit.replaced,
            skinnedComparisonBlasAudit.activeResources,
            skinnedComparisonBlasAudit.buildSubmitted,
            skinnedComparisonBlasAudit.updateSubmitted,
            skinnedComparisonBlasAudit.rebuildSubmitted,
            skinnedComparisonBlasAudit.replacementDeferred,
            skinnedComparisonBlasAudit.failed);
        m_smokeSkinnedComparisonBlasBuildLogged = true;
    }
    if (skinnedComparisonBlasAudit.updateSubmitted > 0 &&
        !m_smokeSkinnedComparisonBlasUpdateLogged)
    {
        common->Printf(
            "PathTracePrimaryPass: GEO08 skinned comparison BLAS update frame=%llu pending=%d exact=%d reused=%d submitted=%d deferred=%d failed=%d flags=allow-update+perform-update tlas=excluded\n",
            static_cast<unsigned long long>(
                geometryUniverseStats.frameIndex),
            skinnedComparisonBlasAudit.updatePending,
            skinnedComparisonBlasAudit.exactContracts,
            skinnedComparisonBlasAudit.reused,
            skinnedComparisonBlasAudit.updateSubmitted,
            skinnedComparisonBlasAudit.replacementDeferred,
            skinnedComparisonBlasAudit.failed);
        m_smokeSkinnedComparisonBlasUpdateLogged = true;
    }
    if (skinnedComparisonBlasAudit.rebuildSubmitted > 0 &&
        !m_smokeSkinnedComparisonBlasRebuildLogged)
    {
        common->Printf(
            "PathTracePrimaryPass: GEO08 skinned comparison BLAS cadence rebuild frame=%llu pending=%d exact=%d reused=%d submitted=%d deferred=%d failed=%d flags=allow-update+prefer-fast-build tlas=excluded\n",
            static_cast<unsigned long long>(
                geometryUniverseStats.frameIndex),
            skinnedComparisonBlasAudit.rebuildPending,
            skinnedComparisonBlasAudit.exactContracts,
            skinnedComparisonBlasAudit.reused,
            skinnedComparisonBlasAudit.rebuildSubmitted,
            skinnedComparisonBlasAudit.replacementDeferred,
            skinnedComparisonBlasAudit.failed);
        m_smokeSkinnedComparisonBlasRebuildLogged = true;
    }
    if (skinnedComparisonBlasAudit.replaced > 0 &&
        !m_smokeSkinnedComparisonBlasReplacementLogged)
    {
        common->Printf(
            "PathTracePrimaryPass: GEO08 skinned comparison BLAS replacement frame=%llu replaced=%d active=%d submitted(build/update/rebuild)=%d/%d/%d deferred=%d failed=%d retirementPending=%zu authority=gpu-event-query tlas=excluded\n",
            static_cast<unsigned long long>(
                geometryUniverseStats.frameIndex),
            skinnedComparisonBlasAudit.replaced,
            skinnedComparisonBlasAudit.activeResources,
            skinnedComparisonBlasAudit.buildSubmitted,
            skinnedComparisonBlasAudit.updateSubmitted,
            skinnedComparisonBlasAudit.rebuildSubmitted,
            skinnedComparisonBlasAudit.replacementDeferred,
            skinnedComparisonBlasAudit.failed,
            m_retiredSmokeSkinnedComparisonBlases.size());
        m_smokeSkinnedComparisonBlasReplacementLogged = true;
    }

    const uint64 skinnedHitRouteOutputCapacity =
        smokeSkinnedCurrentOutputVertexBuffer
            ? smokeSkinnedCurrentOutputVertexBuffer->
                getDesc().byteSize
            : 0;
    const uint64 firstSkinnedHitRouteInstanceId =
        2ull + rigidRouteBuild.instances.size();
    const PtSkinnedHitRouteBuild skinnedHitRouteShadow =
        BuildSmokeSkinnedHitRouteShadow(
            currentSkinnedSurfaceRecords,
            skinnedGpuScaffold.dispatchRecords,
            m_smokeGeometryUniverse,
            m_smokeSkinnedBlasStateTable,
            m_smokeSkinnedComparisonBlases,
            dynamicIndexData,
            dynamicTriangleClassData,
            dynamicTriangleMaterialData,
            materialTable.dynamicMaterialIndexes,
            materialTable,
            smokeSkinnedCurrentOutputVertexBuffer,
            skinnedGpuScaffold.previousPositions.size(),
            skinnedHitRouteOutputCapacity,
            firstSkinnedHitRouteInstanceId,
            canonicalSkinnedSourceOutputRoute,
            canonicalSkinnedSourceOutputRoute);
    const PtSkinnedHitRouteBuild skinnedHitRouteLegacyAuditShadow =
        (r_pathTracingGeometrySkinnedConsumerAudit.GetInteger() != 0 ||
            r_pathTracingGeometrySkinnedHitAudit.GetInteger() != 0)
            ? BuildSmokeSkinnedHitRouteShadow(
                currentSkinnedSurfaceRecords,
                skinnedGpuScaffold.dispatchRecords,
                m_smokeGeometryUniverse,
                m_smokeSkinnedBlasStateTable,
                m_smokeSkinnedComparisonBlases,
                dynamicIndexData,
                dynamicTriangleClassData,
                dynamicTriangleMaterialData,
                materialTable.dynamicMaterialIndexes,
                materialTable,
                smokeSkinnedCurrentOutputVertexBuffer,
                skinnedGpuScaffold.previousPositions.size(),
                skinnedHitRouteOutputCapacity,
                firstSkinnedHitRouteInstanceId,
                canonicalSkinnedSourceOutputRoute,
                false)
            : PtSkinnedHitRouteBuild();
    m_smokeSkinnedHitRouteUploadShadow =
        skinnedHitRouteShadow;
    const int skinnedShadowAccepted =
        static_cast<int>(
            skinnedHitRouteShadow.stats.accepted);
    if (skinnedShadowAccepted !=
            m_smokeSkinnedCaptureLastShadowAccepted &&
        m_smokeSkinnedCaptureShadowTransitionsLogged < 16)
    {
        const char* firstReject = "none";
        for (PtSkinnedHitRouteResult result :
            skinnedHitRouteShadow.results)
        {
            if (result != PtSkinnedHitRouteResult::Accepted)
            {
                firstReject =
                    PtSkinnedHitRouteResultName(result);
                break;
            }
        }
        common->Printf(
            "PathTracePrimaryPass: GEO08 capture route-shadow transition frame=%llu previous/current=%d/%d candidates/rejected=%llu/%llu omitted=%d firstReject=%s scaffold(dispatches/previousPositions)=%zu/%zu\n",
            static_cast<unsigned long long>(
                geometryUniverseStats.frameIndex),
            m_smokeSkinnedCaptureLastShadowAccepted,
            skinnedShadowAccepted,
            static_cast<unsigned long long>(
                skinnedHitRouteShadow.stats.candidates),
            static_cast<unsigned long long>(
                skinnedHitRouteShadow.stats.rejected),
            captureTiming.skinnedCaptureOmittedSurfaces,
            firstReject,
            skinnedGpuScaffold.dispatchRecords.size(),
            skinnedGpuScaffold.previousPositions.size());
        ++m_smokeSkinnedCaptureShadowTransitionsLogged;
    }
    m_smokeSkinnedCaptureLastShadowAccepted =
        skinnedShadowAccepted;
    if (skinnedCaptureViewSignature != 0)
    {
        SmokeSkinnedCaptureRouteSetState* routeSet = nullptr;
        for (SmokeSkinnedCaptureRouteSetState& candidate :
            m_smokeSkinnedCaptureRouteSets)
        {
            if (candidate.signature ==
                skinnedCaptureViewSignature)
            {
                routeSet = &candidate;
                break;
            }
        }
        if (routeSet == nullptr)
        {
            const size_t routeSetLimit =
                static_cast<size_t>(idMath::ClampInt(
                    1,
                    8,
                    r_pathTracingGeometrySkinnedCaptureRouteSetLimit.
                        GetInteger()));
            if (m_smokeSkinnedCaptureRouteSets.size() >=
                routeSetLimit)
            {
                const auto oldest = std::min_element(
                    m_smokeSkinnedCaptureRouteSets.begin(),
                    m_smokeSkinnedCaptureRouteSets.end(),
                    [](const SmokeSkinnedCaptureRouteSetState& lhs,
                        const SmokeSkinnedCaptureRouteSetState& rhs)
                    {
                        return lhs.lastUsedFrame <
                            rhs.lastUsedFrame;
                    });
                if (oldest !=
                    m_smokeSkinnedCaptureRouteSets.end())
                {
                    common->Printf(
                        "PathTracePrimaryPass: GEO08 capture route-set evicted frame=%llu signature=%llu pending/accepted=%zu/%zu age=%llu entries/limit=%zu/%zu action=cpu-fallback-on-return\n",
                        static_cast<unsigned long long>(
                            m_smokeGeometryFrameIndex),
                        static_cast<unsigned long long>(
                            oldest->signature),
                        oldest->pendingBuild.records.size(),
                        oldest->acceptedBuild.records.size(),
                        static_cast<unsigned long long>(
                            m_smokeGeometryFrameIndex -
                            oldest->lastUsedFrame),
                        m_smokeSkinnedCaptureRouteSets.size(),
                        routeSetLimit);
                    m_smokeSkinnedCaptureRouteSets.erase(
                        oldest);
                    ++m_smokeSkinnedCaptureRouteSetEvictions;
                }
            }
            m_smokeSkinnedCaptureRouteSets.emplace_back();
            routeSet =
                &m_smokeSkinnedCaptureRouteSets.back();
            routeSet->signature =
                skinnedCaptureViewSignature;
        }
        routeSet->pendingBuild =
            skinnedHitRouteShadow;
        routeSet->pendingBuildSignature =
            PtBuildSkinnedHitRouteGpuUpload(
                skinnedHitRouteShadow,
                0).signature;
        routeSet->lastUsedFrame =
            m_smokeGeometryFrameIndex;
    }
    if (skinnedHitRouteShadow.stats.accepted >
            m_smokeSkinnedCaptureSplitShadowMaxLogged &&
        captureTiming.skinnedCaptureOmittedSurfaces > 0 &&
        skinnedHitRouteShadow.stats.accepted > 0 &&
        skinnedHitRouteShadow.stats.rejected == 0 &&
        skinnedHitRouteShadow.stats.accepted ==
            static_cast<uint64>(
                captureTiming.
                    skinnedCaptureOmittedSurfaces) &&
        skinnedHitRouteShadow.stats.legacyTriangles == 0 &&
        skinnedHitRouteShadow.stats.sourceOnlyTriangles ==
            skinnedHitRouteShadow.stats.sourceTriangles)
    {
        common->Printf(
            "PathTracePrimaryPass: GEO08 capture-split route shadow frame=%llu accepted/rejected=%llu/%llu triangles(source/legacy/sourceOnly)=%llu/%llu/%llu motion(ready/missing)=%llu/%llu metadata=source-only\n",
            static_cast<unsigned long long>(
                geometryUniverseStats.frameIndex),
            static_cast<unsigned long long>(
                skinnedHitRouteShadow.stats.accepted),
            static_cast<unsigned long long>(
                skinnedHitRouteShadow.stats.rejected),
            static_cast<unsigned long long>(
                skinnedHitRouteShadow.stats.sourceTriangles),
            static_cast<unsigned long long>(
                skinnedHitRouteShadow.stats.legacyTriangles),
            static_cast<unsigned long long>(
                skinnedHitRouteShadow.stats.sourceOnlyTriangles),
            static_cast<unsigned long long>(
                skinnedHitRouteShadow.stats.motionReady),
            static_cast<unsigned long long>(
                skinnedHitRouteShadow.stats.motionMissing));
        m_smokeSkinnedCaptureSplitShadowMaxLogged =
            static_cast<uint32>(
                skinnedHitRouteShadow.stats.accepted);
    }
    if (skinnedHitRouteShadow.stats.accepted > 0 &&
        !m_smokeSkinnedHitRouteShadowLogged)
    {
        const char* firstReject = "none";
        for (PtSkinnedHitRouteResult result :
            skinnedHitRouteShadow.results)
        {
            if (result !=
                PtSkinnedHitRouteResult::Accepted)
            {
                firstReject =
                    PtSkinnedHitRouteResultName(result);
                break;
            }
        }
        const uint32 firstInstanceId =
            !skinnedHitRouteShadow.records.empty()
                ? skinnedHitRouteShadow.records.front().
                    shaderInstanceId
                : 0;
        const uint32 lastInstanceId =
            !skinnedHitRouteShadow.records.empty()
                ? skinnedHitRouteShadow.records.back().
                    shaderInstanceId
                : 0;
        common->Printf(
            "PathTracePrimaryPass: GEO08 skinned hit-route shadow frame=%llu candidates/accepted/rejected=%llu/%llu/%llu shaderInstance(first/last)=%u/%u triangles(source/legacy/mapped/sourceOnly)=%llu/%llu/%llu/%llu motion(ready/missing)=%llu/%llu identity(instanceHashCollisions/emissiveCollisions)=%llu/%llu firstReject=%s primitiveAuthority=source-local tlas=excluded\n",
            static_cast<unsigned long long>(
                geometryUniverseStats.frameIndex),
            static_cast<unsigned long long>(
                skinnedHitRouteShadow.stats.candidates),
            static_cast<unsigned long long>(
                skinnedHitRouteShadow.stats.accepted),
            static_cast<unsigned long long>(
                skinnedHitRouteShadow.stats.rejected),
            firstInstanceId,
            lastInstanceId,
            static_cast<unsigned long long>(
                skinnedHitRouteShadow.stats.sourceTriangles),
            static_cast<unsigned long long>(
                skinnedHitRouteShadow.stats.legacyTriangles),
            static_cast<unsigned long long>(
                skinnedHitRouteShadow.stats.
                    mappedLegacyTriangles),
            static_cast<unsigned long long>(
                skinnedHitRouteShadow.stats.sourceOnlyTriangles),
            static_cast<unsigned long long>(
                skinnedHitRouteShadow.stats.motionReady),
            static_cast<unsigned long long>(
                skinnedHitRouteShadow.stats.motionMissing),
            static_cast<unsigned long long>(
                skinnedHitRouteShadow.stats.
                    canonicalHashCollisions),
            static_cast<unsigned long long>(
                skinnedHitRouteShadow.stats.
                    emissiveIdentityCollisions),
            firstReject);
        m_smokeSkinnedHitRouteShadowLogged = true;
    }

    RtSmokeAccelSubmitDesc accelSubmitDesc;
    std::vector<nvrhi::rt::InstanceDesc> rigidTlasRouteInstances;
    bool canonicalRigidTraversalSelected = false;
    const bool routeRigidTlasInstances = enableRigidRouteForMode;
    {
        OPTICK_EVENT("PT Rigid TLAS Instance Descs");
        if (routeRigidTlasInstances)
        {
            if (!rigidTlasPlanValid)
            {
                const int rigidTlasPlanStartMs = Sys_Milliseconds();
                rigidTlasSnapshot = m_smokeGeometryUniverse.CaptureRigidTlasInstancePlanSnapshot(
                    m_instanceUniverse,
                    2,
                    0x02,
                    rigidRouteMaxInstances);
                rigidTlasPlanInputToken = BuildSmokeRigidTlasPlanInputToken(rigidTlasSnapshot);
                rigidTlasPlan = BuildSmokeRigidTlasPlan(rigidTlasSnapshot);
                rigidTlasPlanMs = Sys_Milliseconds() - rigidTlasPlanStartMs;
                rigidTlasPlanValid = true;
            }
            const int routedRigidInstances =
                m_smokeGeometryUniverse.BuildRigidTlasInstanceDescs(rigidTlasPlan, rigidTlasRouteInstances);
            const bool canonicalRigidTraversalRequested =
                r_pathTracingGeometryCanonicalRigidTraversal.GetInteger() != 0;
            if (canonicalRigidTraversalRequested ||
                (geometrySourceDumpRequested &&
                    r_pathTracingGeometryCanonicalRigidBlas.GetInteger() != 0))
            {
                std::vector<nvrhi::rt::InstanceDesc>
                    canonicalRigidTlasInstances;
                RtPathTraceCanonicalRigidTlasStats
                    canonicalRigidTlasStats =
                        m_smokeGeometryUniverse.
                            BuildCanonicalRigidTlasInstanceDescs(
                                rigidTlasPlan,
                                routedRigidInstances,
                                canonicalRigidTlasInstances);
                RtSmokeCanonicalRigidTlasSelectionInput
                    canonicalSelectionInput;
                canonicalSelectionInput.providerEnabled =
                    canonicalRigidTlasStats.enabled != 0;
                canonicalSelectionInput.traversalRequested =
                    canonicalRigidTraversalRequested;
                canonicalSelectionInput.legacyDescriptors =
                    canonicalRigidTlasStats.legacyDescriptors;
                canonicalSelectionInput.canonicalDescriptors =
                    canonicalRigidTlasStats.canonicalDescriptors;
                canonicalSelectionInput.exactRecordMappings =
                    canonicalRigidTlasStats.exactRecordMappings;
                canonicalSelectionInput.missingRecordIndex =
                    canonicalRigidTlasStats.missingRecordIndex;
                canonicalSelectionInput.meshHashMismatch =
                    canonicalRigidTlasStats.meshHashMismatch;
                canonicalSelectionInput.missingBlas =
                    canonicalRigidTlasStats.missingBlas;
                const RtSmokeCanonicalRigidTlasSelection
                    canonicalSelection =
                        BuildSmokeCanonicalRigidTlasSelection(
                            canonicalSelectionInput);
                canonicalRigidTlasStats.traversalRequested =
                    canonicalRigidTraversalRequested ? 1 : 0;
                canonicalRigidTlasStats.exactParity =
                    canonicalSelection.exactParity ? 1 : 0;
                canonicalRigidTlasStats.selectedForSubmit =
                    canonicalSelection.selectCanonical ? 1 : 0;
                if (canonicalSelection.selectCanonical)
                {
                    rigidTlasRouteInstances.swap(
                        canonicalRigidTlasInstances);
                    canonicalRigidTraversalSelected = true;
                }
                else if (canonicalRigidTraversalRequested &&
                    (m_smokeGeometryFrameIndex % 120ull) == 1ull)
                {
                    common->Printf(
                        "PathTracePrimaryPass: GEO06 canonical rigid traversal fail-closed to legacy descriptors=%d/%d exact=%d failures=%d/%d/%d\n",
                        canonicalRigidTlasStats.legacyDescriptors,
                        canonicalRigidTlasStats.canonicalDescriptors,
                        canonicalRigidTlasStats.exactRecordMappings,
                        canonicalRigidTlasStats.missingRecordIndex,
                        canonicalRigidTlasStats.meshHashMismatch,
                        canonicalRigidTlasStats.missingBlas);
                }
                if (geometrySourceDumpRequested)
                {
                    m_smokeGeometryUniverse.DumpCanonicalRigidTlasStats(
                        canonicalRigidTlasStats);
                }
            }
            if (r_pathTracingSmokeLog.GetInteger() != 0 && routedRigidInstances > 0 && (m_smokeGeometryFrameIndex % 120ull) == 1ull)
            {
                common->Printf("PathTracePrimaryPass: PT rigid TLAS route debug mode active mode=%d routedInstances=%d renderPath=dynamicFallback traceMask=%s\n",
                    requestedDebugMode,
                    routedRigidInstances,
                    requestedDebugMode == 23 ? "rigidOnly" : (requestedDebugMode == 24 ? "fallbackAndRigidValidation" : (requestedDebugMode == 25 ? "fallbackAndRigidLighting" : "routedIntegration")));
            }
        }
    }

    const bool skinnedTlasCompareRequested =
        r_pathTracingGeometrySkinnedTlasCompare.
            GetInteger() != 0;
    const bool skinnedUploadMatchesOmittedCapture =
        SmokeSkinnedHitRoutesMatchOmittedCapture(
            skinnedHitRouteUploadCpuRecords,
            currentSkinnedSurfaceRecords,
            captureTiming.skinnedCaptureOmittedSurfaces);
    const bool skinnedTlasCompareGate =
        skinnedTlasCompareRequested &&
        canonicalSkinnedSourceOutputRoute &&
        skinnedUploadMatchesOmittedCapture &&
        deviceManager &&
        deviceManager->GetGraphicsAPI() ==
            nvrhi::GraphicsAPI::VULKAN;
    if (skinnedTlasCompareRequested &&
        captureTiming.skinnedCaptureOmittedSurfaces > 0 &&
        !skinnedUploadMatchesOmittedCapture)
    {
        common->Printf(
            "PathTracePrimaryPass: GEO08 capture/TLAS omitted-set mismatch frame=%llu omitted/routes=%d/%zu action=suppress-and-revoke\n",
            static_cast<unsigned long long>(
                geometryUniverseStats.frameIndex),
            captureTiming.skinnedCaptureOmittedSurfaces,
            skinnedHitRouteUploadCpuRecords.size());
    }
    const uint32 skinnedUploadedRouteCount =
        skinnedHitRouteGpuUpload.records.empty()
            ? 0u
            : skinnedHitRouteGpuUpload.records.front().
                routeCount;
    PtSkinnedTlasRoutePlanInput skinnedTlasPlanInput;
    skinnedTlasPlanInput.gate = skinnedTlasCompareGate;
    skinnedTlasPlanInput.baseInstanceCount =
        (hasStaticBlas ? 1u : 0u) +
        (hasDynamicBlas ? 1u : 0u);
    skinnedTlasPlanInput.existingExtraInstanceCount =
        static_cast<uint32>(
            rigidTlasRouteInstances.size());
    skinnedTlasPlanInput.maxInstanceCount = 512u;
    skinnedTlasPlanInput.shaderTableRecordCount = 4u;
    skinnedTlasPlanInput.uploadedRouteCount =
        skinnedUploadedRouteCount;
    std::vector<
        RtSmokeSkinnedComparisonBlasResource*>
        skinnedTlasCandidateResources;
    if (skinnedTlasCompareGate)
    {
        skinnedTlasPlanInput.candidates.reserve(
            skinnedHitRouteUploadCpuRecords.size());
        skinnedTlasCandidateResources.reserve(
            skinnedHitRouteUploadCpuRecords.size());
        const nvrhi::BufferHandle sourceIndexBuffer =
            m_smokeGeometryUniverse.
                CanonicalSourceIndexBuffer();
        for (size_t routeIndex = 0;
             routeIndex <
                skinnedHitRouteUploadCpuRecords.size();
             ++routeIndex)
        {
            const PtSkinnedHitRouteRecord& cpuRoute =
                skinnedHitRouteUploadCpuRecords[
                    routeIndex];
            RtSmokeSkinnedComparisonBlasResource*
                resource =
                    FindSmokeSkinnedComparisonBlasResource(
                        m_smokeSkinnedComparisonBlases,
                        cpuRoute.instanceKey);
            const PtSkinnedBlasRecord* state =
                m_smokeSkinnedBlasStateTable.Find(
                    cpuRoute.instanceKey);

            PtSkinnedTlasRouteCandidate candidate;
            candidate.cpuRoute = &cpuRoute;
            candidate.gpuRoute =
                routeIndex <
                    skinnedHitRouteGpuUpload.records.size()
                    ? &skinnedHitRouteGpuUpload.records[
                        routeIndex]
                    : nullptr;
            candidate.resourceFound =
                resource != nullptr;
            candidate.resourceContractExact =
                resource != nullptr &&
                state != nullptr &&
                SmokeSkinnedTlasRouteResourceContractMatches(
                    cpuRoute,
                    *resource,
                    *state,
                    smokeSkinnedCurrentOutputVertexBuffer,
                    sourceIndexBuffer);
            candidate.blasReady =
                resource != nullptr &&
                state != nullptr &&
                state->state ==
                    PtSkinnedBlasState::Ready &&
                resource->blas;
            skinnedTlasPlanInput.candidates.push_back(
                candidate);
            skinnedTlasCandidateResources.push_back(
                resource);
        }
    }

    const PtSkinnedTlasRoutePlan skinnedTlasPlan =
        PtPlanSkinnedTlasRoutes(
            skinnedTlasPlanInput);
    const size_t firstSkinnedTlasDesc =
        rigidTlasRouteInstances.size();
    uint32 skinnedTlasValidatedDescriptorCount = 0;
    uint32 skinnedTlasActiveDescriptorCount = 0;
    if (skinnedTlasPlan.result ==
        PtSkinnedTlasRouteResult::Accepted)
    {
        for (const PtSkinnedTlasRouteRecord& route :
            skinnedTlasPlan.records)
        {
            if (route.candidateIndex >=
                    skinnedTlasCandidateResources.size() ||
                !skinnedTlasCandidateResources[
                    route.candidateIndex] ||
                !skinnedTlasCandidateResources[
                    route.candidateIndex]->blas)
            {
                continue;
            }
            nvrhi::rt::AffineTransform transform;
            transform[0] = 1.0f;
            transform[1] = 0.0f;
            transform[2] = 0.0f;
            transform[3] = 0.0f;
            transform[4] = 0.0f;
            transform[5] = 1.0f;
            transform[6] = 0.0f;
            transform[7] = 0.0f;
            transform[8] = 0.0f;
            transform[9] = 0.0f;
            transform[10] = 1.0f;
            transform[11] = 0.0f;

            nvrhi::rt::InstanceDesc instanceDesc;
            const bool cpuCaptureOmitted =
                SmokeSkinnedCaptureInstanceWasOmitted(
                    currentSkinnedSurfaceRecords,
                    route.instanceKey);
            const bool skinnedHitAuditActive =
                requestedDebugMode == 58 &&
                r_pathTracingGeometrySkinnedHitAudit.
                    GetInteger() != 0;
            ++skinnedTlasValidatedDescriptorCount;
            if (!cpuCaptureOmitted &&
                !skinnedHitAuditActive)
            {
                // Validation of a new source-only route set is sufficient to
                // promote it for next-frame capture omission. Do not submit
                // its freshly built/updated BLAS to the TLAS at mask zero:
                // zero-mask instances cannot trace, still participate in the
                // Vulkan TLAS build, and have produced repeatable driver TDRs
                // on the pre-admission frame.
                continue;
            }
            instanceDesc
                .setInstanceID(route.shaderInstanceId)
                .setInstanceMask(route.instanceMask)
                .setInstanceContributionToHitGroupIndex(
                    route.hitGroupContribution)
                .setFlags(
                    nvrhi::rt::InstanceFlags::
                        TriangleCullDisable)
                .setTransform(transform)
                .setBLAS(
                    skinnedTlasCandidateResources[
                        route.candidateIndex]->blas);
            rigidTlasRouteInstances.push_back(
                instanceDesc);
            ++skinnedTlasActiveDescriptorCount;
        }
    }
    const uint32 skinnedTlasDescriptorCount =
        static_cast<uint32>(
            rigidTlasRouteInstances.size() -
            firstSkinnedTlasDesc);
    if (r_pathTracingGeometrySkinnedConsumerAudit.GetInteger() != 0 &&
        !skinnedHitRouteUploadBuild.records.empty() &&
        skinnedTlasPlan.result == PtSkinnedTlasRouteResult::Accepted)
    {
        PtSkinnedConsumerAuditInput auditInput;
        auditInput.cpuBuild = &skinnedHitRouteUploadBuild;
        auditInput.gpuUpload = &skinnedHitRouteGpuUpload;
        auditInput.tlasPlan = &skinnedTlasPlan;
        auditInput.legacyTriangleClasses = &dynamicTriangleClassData;
        auditInput.legacyTriangleMaterialIds = &dynamicTriangleMaterialData;
        auditInput.legacyTriangleMaterialIndexes =
            &materialTable.dynamicMaterialIndexes;
        auditInput.materialTableIds = &materialTable.materialIds;
        auditInput.currentOutputVertexCount =
            skinnedGpuScaffold.currentOutputVertices.size();
        auditInput.previousPositionCount =
            skinnedGpuScaffold.previousPositions.size();
        const PtSkinnedConsumerAuditStats liveAudit =
            PtAuditSkinnedConsumerContract(auditInput);

        const PtSkinnedHitRouteGpuUpload shadowUpload =
            PtBuildSkinnedHitRouteGpuUpload(
                skinnedHitRouteLegacyAuditShadow,
                static_cast<uint32_t>(
                    firstSkinnedHitRouteInstanceId));
        PtSkinnedConsumerAuditInput shadowAuditInput =
            auditInput;
        shadowAuditInput.cpuBuild =
            &skinnedHitRouteLegacyAuditShadow;
        shadowAuditInput.gpuUpload = &shadowUpload;
        shadowAuditInput.tlasPlan = nullptr;
        shadowAuditInput.requireTlasPlan = false;
        const PtSkinnedConsumerAuditStats shadowAudit =
            PtAuditSkinnedConsumerContract(shadowAuditInput);
        common->Printf(
            "PathTracePrimaryPass: GEO09 skinned consumer audit frame=%llu live(accepted/routes/triangles/legacyMapped/sourceOnly/motionReady/missing)=%d/%llu/%llu/%llu/%llu/%llu/%llu shadow(accepted/routes/triangles/legacyMapped/sourceOnly/motionReady/missing)=%d/%llu/%llu/%llu/%llu/%llu/%llu failuresLive(input/routeCount/routeUpload/tlas/triangleRange/triangleUpload/legacyMetadata/materialTable/currentRange/previousRange/primitiveIdentity/emissiveIdentity/emissiveCollision)=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu failuresShadow=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu\n",
            static_cast<unsigned long long>(m_smokeGeometryFrameIndex),
            liveAudit.Accepted() ? 1 : 0,
            static_cast<unsigned long long>(liveAudit.routes),
            static_cast<unsigned long long>(liveAudit.triangles),
            static_cast<unsigned long long>(liveAudit.mappedLegacyTriangles),
            static_cast<unsigned long long>(liveAudit.sourceOnlyTriangles),
            static_cast<unsigned long long>(liveAudit.motionReadyRoutes),
            static_cast<unsigned long long>(liveAudit.motionMissingRoutes),
            shadowAudit.Accepted() ? 1 : 0,
            static_cast<unsigned long long>(shadowAudit.routes),
            static_cast<unsigned long long>(shadowAudit.triangles),
            static_cast<unsigned long long>(shadowAudit.mappedLegacyTriangles),
            static_cast<unsigned long long>(shadowAudit.sourceOnlyTriangles),
            static_cast<unsigned long long>(shadowAudit.motionReadyRoutes),
            static_cast<unsigned long long>(shadowAudit.motionMissingRoutes),
            static_cast<unsigned long long>(liveAudit.missingInput),
            static_cast<unsigned long long>(liveAudit.routeCountMismatch),
            static_cast<unsigned long long>(liveAudit.routeUploadMismatch),
            static_cast<unsigned long long>(liveAudit.tlasRouteMismatch),
            static_cast<unsigned long long>(liveAudit.triangleRangeMismatch),
            static_cast<unsigned long long>(liveAudit.triangleUploadMismatch),
            static_cast<unsigned long long>(liveAudit.legacyMetadataMismatch),
            static_cast<unsigned long long>(liveAudit.materialTableMismatch),
            static_cast<unsigned long long>(liveAudit.currentRangeMismatch),
            static_cast<unsigned long long>(liveAudit.previousRangeMismatch),
            static_cast<unsigned long long>(liveAudit.primitiveIdentityInvalid),
            static_cast<unsigned long long>(liveAudit.emissiveIdentityInvalid),
            static_cast<unsigned long long>(liveAudit.emissiveIdentityCollision),
            static_cast<unsigned long long>(shadowAudit.missingInput),
            static_cast<unsigned long long>(shadowAudit.routeCountMismatch),
            static_cast<unsigned long long>(shadowAudit.routeUploadMismatch),
            static_cast<unsigned long long>(shadowAudit.tlasRouteMismatch),
            static_cast<unsigned long long>(shadowAudit.triangleRangeMismatch),
            static_cast<unsigned long long>(shadowAudit.triangleUploadMismatch),
            static_cast<unsigned long long>(shadowAudit.legacyMetadataMismatch),
            static_cast<unsigned long long>(shadowAudit.materialTableMismatch),
            static_cast<unsigned long long>(shadowAudit.currentRangeMismatch),
            static_cast<unsigned long long>(shadowAudit.previousRangeMismatch),
            static_cast<unsigned long long>(shadowAudit.primitiveIdentityInvalid),
            static_cast<unsigned long long>(shadowAudit.emissiveIdentityInvalid),
            static_cast<unsigned long long>(shadowAudit.emissiveIdentityCollision));
        r_pathTracingGeometrySkinnedConsumerAudit.SetInteger(0);
    }
    const int skinnedEmissiveAuditCountdown =
        r_pathTracingGeometrySkinnedEmissiveAudit.GetInteger();
    if (skinnedEmissiveAuditCountdown > 1 &&
        !skinnedHitRouteUploadBuild.records.empty())
    {
        r_pathTracingGeometrySkinnedEmissiveAudit.SetInteger(
            skinnedEmissiveAuditCountdown - 1);
    }
    else if (skinnedEmissiveAuditCountdown == 1 &&
        !skinnedHitRouteUploadBuild.records.empty())
    {
        uint64 emissiveAuditTriangles = 0;
        uint64 emissiveAuditInvalid = 0;
        uint64 emissiveAuditNonEmissive = 0;
        uint64 emissiveAuditNonCandidate = 0;
        uint64 emissiveAuditStageOff = 0;
        uint64 emissiveAuditEligible = 0;
        uint64 emissiveAuditZeroIdentity = 0;
        uint64 emissiveAuditIdentityCollisions = 0;
        std::unordered_set<uint64> emissiveAuditIdentities;
        std::unordered_set<uint64> emissiveAuditInstances;
        std::unordered_set<uint32_t> emissiveAuditMaterials;
        std::unordered_set<uint32_t> emissiveAuditRoutedMaterials;
        for (const PtSkinnedHitRouteRecord& route :
            skinnedHitRouteUploadBuild.records)
        {
            for (uint32_t primitiveIndex = 0;
                primitiveIndex < route.triangleCount;
                ++primitiveIndex)
            {
                ++emissiveAuditTriangles;
                const uint64 triangleIndex =
                    static_cast<uint64>(
                        route.triangleMetadataOffset) +
                    primitiveIndex;
                if (triangleIndex >=
                    skinnedHitRouteUploadBuild.triangles.size())
                {
                    ++emissiveAuditInvalid;
                    continue;
                }
                const PtSkinnedHitRouteTriangle& triangle =
                    skinnedHitRouteUploadBuild.triangles[
                        static_cast<size_t>(triangleIndex)];
                if (triangle.materialIndex >=
                    materialTable.materials.size())
                {
                    ++emissiveAuditInvalid;
                    continue;
                }
                const PathTraceSmokeMaterial& material =
                    materialTable.materials[
                        triangle.materialIndex];
                emissiveAuditRoutedMaterials.insert(
                    triangle.materialIndex);
                if ((material.flags &
                        RT_SMOKE_MATERIAL_EMISSIVE) == 0u)
                {
                    ++emissiveAuditNonEmissive;
                    continue;
                }
                if ((material.flags &
                        RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE) == 0u)
                {
                    ++emissiveAuditNonCandidate;
                    continue;
                }
                if ((triangle.triangleClassAndFlags &
                        RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) !=
                    0u)
                {
                    ++emissiveAuditStageOff;
                    continue;
                }
                ++emissiveAuditEligible;
                emissiveAuditInstances.insert(
                    route.instanceHash);
                emissiveAuditMaterials.insert(
                    triangle.materialIndex);
                if (triangle.emissiveIdentityHash == 0)
                {
                    ++emissiveAuditZeroIdentity;
                }
                else if (!emissiveAuditIdentities.insert(
                        triangle.emissiveIdentityHash).second)
                {
                    ++emissiveAuditIdentityCollisions;
                }
            }
        }
        const bool accepted =
            emissiveAuditTriangles > 0 &&
            emissiveAuditInvalid == 0 &&
            emissiveAuditZeroIdentity == 0 &&
            emissiveAuditIdentityCollisions == 0 &&
                emissiveAuditTriangles ==
                    emissiveAuditInvalid +
                    emissiveAuditNonEmissive +
                    emissiveAuditNonCandidate +
                    emissiveAuditStageOff +
                    emissiveAuditEligible;
        common->Printf(
            "PathTracePrimaryPass: GEO09 skinned emissive source census frame=%llu accepted=%d routes=%llu triangles(total/invalid/nonEmissive/nonCandidate/stageOff/eligible)=%llu/%llu/%llu/%llu/%llu/%llu eligible(instances/materials/identities/zero/collisions)=%llu/%llu/%llu/%llu/%llu routedMaterials=%llu route=canonical-source-local currentPositionSource=gpu-output-pending\n",
            static_cast<unsigned long long>(
                m_smokeGeometryFrameIndex),
            accepted ? 1 : 0,
            static_cast<unsigned long long>(
                skinnedHitRouteUploadBuild.records.size()),
            static_cast<unsigned long long>(
                emissiveAuditTriangles),
            static_cast<unsigned long long>(
                emissiveAuditInvalid),
            static_cast<unsigned long long>(
                emissiveAuditNonEmissive),
            static_cast<unsigned long long>(
                emissiveAuditNonCandidate),
            static_cast<unsigned long long>(
                emissiveAuditStageOff),
            static_cast<unsigned long long>(
                emissiveAuditEligible),
            static_cast<unsigned long long>(
                emissiveAuditInstances.size()),
            static_cast<unsigned long long>(
                emissiveAuditMaterials.size()),
            static_cast<unsigned long long>(
                emissiveAuditIdentities.size()),
            static_cast<unsigned long long>(
                emissiveAuditZeroIdentity),
            static_cast<unsigned long long>(
                emissiveAuditIdentityCollisions),
            static_cast<unsigned long long>(
                emissiveAuditRoutedMaterials.size()));
        int emissiveAuditMaterialPrintCount = 0;
        for (uint32_t materialIndex : emissiveAuditRoutedMaterials)
        {
            if (emissiveAuditMaterialPrintCount >= 32 ||
                materialIndex >= materialTable.materials.size() ||
                materialIndex >= materialTable.materialIds.size())
            {
                continue;
            }
            const PathTraceSmokeMaterial& material =
                materialTable.materials[materialIndex];
            const RtSmokeMaterialTextureInfo* info =
                materialIndex < materialTable.materialInfos.size()
                    ? &materialTable.materialInfos[materialIndex]
                    : nullptr;
            common->Printf(
                "PathTracePrimaryPass: GEO09 skinned emissive material tableIndex=%u materialId=%u name=%s flags=0x%08x emissive=%d candidate=%d image/handle/safe=%d/%d/%d reason=%s\n",
                materialIndex,
                materialTable.materialIds[materialIndex],
                info ? info->materialName.c_str() : "<missing-info>",
                material.flags,
                info && info->emissive ? 1 : 0,
                info && info->emissiveLightCandidate ? 1 : 0,
                info && info->hasEmissiveImage ? 1 : 0,
                info && info->hasEmissiveTextureHandle ? 1 : 0,
                info && info->hasSafeEmissiveTexture ? 1 : 0,
                info ? info->emissiveReason.c_str() : "<missing-info>");
            ++emissiveAuditMaterialPrintCount;
        }
        r_pathTracingGeometrySkinnedEmissiveAudit.SetInteger(0);
    }
    const int skinnedHitAuditCountdown =
        r_pathTracingGeometrySkinnedHitAudit.GetInteger();
    if (skinnedHitAuditCountdown > 1 &&
        requestedDebugMode == 58 &&
        !m_skinnedHitAuditRequested &&
        !m_skinnedHitAuditReadbackQueued &&
        !skinnedHitRouteUploadBuild.records.empty() &&
        !skinnedHitRouteLegacyAuditShadow.records.empty() &&
        skinnedTlasPlan.result == PtSkinnedTlasRouteResult::Accepted)
    {
        r_pathTracingGeometrySkinnedHitAudit.SetInteger(
            skinnedHitAuditCountdown - 1);
    }
    else if (skinnedHitAuditCountdown == 1 &&
        requestedDebugMode == 58 &&
        !m_skinnedHitAuditRequested &&
        !m_skinnedHitAuditReadbackQueued &&
        !skinnedHitRouteUploadBuild.records.empty() &&
        !skinnedHitRouteLegacyAuditShadow.records.empty() &&
        skinnedTlasPlan.result == PtSkinnedTlasRouteResult::Accepted)
    {
        m_skinnedHitAuditLegacyShadow =
            skinnedHitRouteLegacyAuditShadow;
        m_skinnedHitAuditFrame = m_smokeGeometryFrameIndex;
        m_skinnedHitAuditRequested = true;
        r_pathTracingGeometrySkinnedHitAudit.SetInteger(0);
        common->Printf(
            "PathTracePrimaryPass: GEO09 skinned hit audit armed frame=%llu routes=%llu triangles=%llu mode=58\n",
            static_cast<unsigned long long>(
                m_skinnedHitAuditFrame),
            static_cast<unsigned long long>(
                m_skinnedHitAuditLegacyShadow.records.size()),
            static_cast<unsigned long long>(
                m_skinnedHitAuditLegacyShadow.triangles.size()));
    }
    const bool skinnedTlasUsesSourceOnlyMetadata =
        !skinnedHitRouteUploadCpuRecords.empty() &&
        std::all_of(
            skinnedHitRouteUploadCpuRecords.begin(),
            skinnedHitRouteUploadCpuRecords.end(),
            [](const PtSkinnedHitRouteRecord& route)
            {
                return (route.flags &
                    PT_SKINNED_HIT_ROUTE_HAS_SOURCE_ONLY_PRIMITIVES) !=
                    0u;
            });
    if (skinnedTlasPlan.result ==
            PtSkinnedTlasRouteResult::Accepted &&
        skinnedTlasValidatedDescriptorCount >
            skinnedTlasDescriptorCount)
    {
        common->Printf(
            "PathTracePrimaryPass: GEO08 capture-split TLAS pre-admission frame=%llu routes/descriptors(validated/submitted/deferred)=%zu/%u/%u/%u omitted=%d metadata=source-only action=exclude-cpu-fallback-until-next-frame\n",
            static_cast<unsigned long long>(
                geometryUniverseStats.frameIndex),
            skinnedHitRouteUploadCpuRecords.size(),
            skinnedTlasValidatedDescriptorCount,
            skinnedTlasDescriptorCount,
            skinnedTlasValidatedDescriptorCount -
                skinnedTlasDescriptorCount,
            captureTiming.skinnedCaptureOmittedSurfaces);
    }
    const size_t skinnedTlasMotionReady =
        static_cast<size_t>(std::count_if(
            skinnedHitRouteUploadCpuRecords.begin(),
            skinnedHitRouteUploadCpuRecords.end(),
            [](const PtSkinnedHitRouteRecord& route)
            {
                return (route.flags &
                    PT_SKINNED_HIT_ROUTE_HAS_PREVIOUS) !=
                    0u;
            }));
    SmokeSkinnedCaptureRouteSetState* activeCaptureRouteSet =
        nullptr;
    for (SmokeSkinnedCaptureRouteSetState& routeSet :
        m_smokeSkinnedCaptureRouteSets)
    {
        if (routeSet.signature ==
            skinnedCaptureViewSignature)
        {
            activeCaptureRouteSet = &routeSet;
            break;
        }
    }
    if (!skinnedTlasCompareRequested)
    {
        m_smokeSkinnedCaptureRouteSets.clear();
        activeCaptureRouteSet = nullptr;
    }
    else if (activeCaptureRouteSet != nullptr &&
             skinnedTlasPlan.result ==
                 PtSkinnedTlasRouteResult::Accepted &&
             skinnedTlasValidatedDescriptorCount ==
                 skinnedTlasPlan.records.size() &&
             !skinnedHitRouteUploadBuild.records.empty())
    {
        // Promote the exact upload whose descriptor/resource contracts were
        // validated this frame. Pre-admission descriptors remain outside the
        // TLAS while their CPU fallback is retained; the omitted InstanceKey
        // set is checked against this upload before next-frame submission.
        activeCaptureRouteSet->acceptedBuild =
            skinnedHitRouteUploadBuild;
        activeCaptureRouteSet->acceptedBuildSignature =
            skinnedHitRouteUploadBuildSignature;
        activeCaptureRouteSet->lastUsedFrame =
            m_smokeGeometryFrameIndex;
    }
    else if (activeCaptureRouteSet != nullptr &&
             (captureTiming.skinnedCaptureOmittedSurfaces > 0 ||
              !skinnedHitRouteUploadBuild.records.empty()))
    {
        // A late route/resource/BLAS failure after CPU capture omission
        // suppresses this view and revokes only its exact set for next time.
        activeCaptureRouteSet->acceptedBuild =
            PtSkinnedHitRouteBuild();
        activeCaptureRouteSet->acceptedBuildSignature = 0;
        ++m_smokeSkinnedCaptureLateRevocations;
    }
    if (skinnedTlasDescriptorCount >
            m_smokeSkinnedCaptureSplitTlasMaxLogged &&
        captureTiming.skinnedCaptureOmittedSurfaces > 0 &&
        skinnedTlasPlan.result ==
            PtSkinnedTlasRouteResult::Accepted &&
        skinnedTlasDescriptorCount ==
            skinnedTlasPlan.records.size() &&
        skinnedTlasDescriptorCount > 0 &&
        skinnedTlasDescriptorCount ==
            static_cast<uint32>(
                captureTiming.
                    skinnedCaptureOmittedSurfaces) &&
        skinnedTlasUsesSourceOnlyMetadata)
    {
        common->Printf(
            "PathTracePrimaryPass: GEO08 capture-split TLAS accepted frame=%llu omitted(surfaces/verts/indexes)=%d/%d/%d cpuSkinAppendMs=%d routes/descriptors/motionReady=%zu/%u/%zu metadata=source-only lateFailurePolicy=suppress\n",
            static_cast<unsigned long long>(
                geometryUniverseStats.frameIndex),
            captureTiming.skinnedCaptureOmittedSurfaces,
            captureTiming.skinnedCaptureOmittedVerts,
            captureTiming.skinnedCaptureOmittedIndexes,
            captureTiming.rtCpuSkinningAppendMs,
            skinnedHitRouteUploadCpuRecords.size(),
            skinnedTlasDescriptorCount,
            skinnedTlasMotionReady);
        m_smokeSkinnedCaptureSplitTlasMaxLogged =
            skinnedTlasDescriptorCount;
    }
    if (r_pathTracingSmokeLog.GetInteger() != 0 &&
        (m_smokeGeometryFrameIndex % 120ull) == 1ull)
    {
        size_t acceptedRouteSets = 0;
        size_t acceptedRoutes = 0;
        for (const SmokeSkinnedCaptureRouteSetState& routeSet :
            m_smokeSkinnedCaptureRouteSets)
        {
            if (!routeSet.acceptedBuild.records.empty())
            {
                ++acceptedRouteSets;
                acceptedRoutes +=
                    routeSet.acceptedBuild.records.size();
            }
        }
        common->Printf(
            "PathTracePrimaryPass: GEO08 capture lifecycle frame=%llu routeSets(total/accepted/routes/limit)=%zu/%zu/%zu/%d events(evicted/preCaptureInvalidated/lateRevoked)=%llu/%llu/%llu active/retiredBlas=%zu/%zu outputGeneration=%llu authority=exact-live-resource-set\n",
            static_cast<unsigned long long>(
                m_smokeGeometryFrameIndex),
            m_smokeSkinnedCaptureRouteSets.size(),
            acceptedRouteSets,
            acceptedRoutes,
            idMath::ClampInt(
                1,
                8,
                r_pathTracingGeometrySkinnedCaptureRouteSetLimit.
                    GetInteger()),
            static_cast<unsigned long long>(
                m_smokeSkinnedCaptureRouteSetEvictions),
            static_cast<unsigned long long>(
                m_smokeSkinnedCapturePreCaptureInvalidations),
            static_cast<unsigned long long>(
                m_smokeSkinnedCaptureLateRevocations),
            m_smokeSkinnedComparisonBlases.size(),
            m_retiredSmokeSkinnedComparisonBlases.size(),
            static_cast<unsigned long long>(
                m_smokeSkinnedOutputBufferGeneration));
    }
    const bool skinnedTlasDumpRequested =
        r_pathTracingGeometrySkinnedTlasCompareDump.
            GetInteger() != 0;
    const bool skinnedTlasDumpReady =
        !skinnedTlasCompareGate ||
        skinnedTlasPlan.result !=
            PtSkinnedTlasRouteResult::Accepted ||
        skinnedUploadedRouteCount > 0u ||
        m_smokeGeometryFrameIndex >= 120ull;
    if ((skinnedTlasDumpRequested &&
            skinnedTlasDumpReady) ||
        (skinnedTlasCompareGate &&
            skinnedTlasPlan.result !=
                PtSkinnedTlasRouteResult::Accepted &&
            (m_smokeGeometryFrameIndex % 120ull) == 1ull))
    {
        const uint32 firstInstanceId =
            skinnedTlasPlan.records.empty()
                ? 0u
                : skinnedTlasPlan.records.front().
                    shaderInstanceId;
        const uint32 lastInstanceId =
            skinnedTlasPlan.records.empty()
                ? 0u
                : skinnedTlasPlan.records.back().
                    shaderInstanceId;
        common->Printf(
            "PathTracePrimaryPass: GEO08 skinned TLAS compare frame=%llu requested/gate/canonical=%d/%d/%d result=%s cpu/upload/candidates/accepted/rejected/descriptors=%zu/%u/%u/%u/%u/%u shaderInstance(first/last)=%u/%u tlas(base/rigid/skinned/total/max)=%u/%zu/%u/%zu/%u failures(uploadCount/cpu/gpu/uploadContract/resource/resourceContract/blas/sbt/capacity)=%u/%u/%u/%u/%u/%u/%u/%u/%u legacyDynamic=retained sbtRecords=%u\n",
            static_cast<unsigned long long>(
                geometryUniverseStats.frameIndex),
            skinnedTlasCompareRequested ? 1 : 0,
            skinnedTlasCompareGate ? 1 : 0,
            canonicalSkinnedSourceOutputRoute ? 1 : 0,
            PtSkinnedTlasRouteResultName(
                skinnedTlasPlan.result),
            skinnedHitRouteUploadCpuRecords.size(),
            skinnedUploadedRouteCount,
            skinnedTlasPlan.stats.candidates,
            skinnedTlasPlan.stats.accepted,
            skinnedTlasPlan.stats.rejected,
            skinnedTlasDescriptorCount,
            firstInstanceId,
            lastInstanceId,
            skinnedTlasPlanInput.baseInstanceCount,
            firstSkinnedTlasDesc,
            skinnedTlasDescriptorCount,
            static_cast<size_t>(
                skinnedTlasPlanInput.baseInstanceCount) +
                rigidTlasRouteInstances.size(),
            skinnedTlasPlanInput.maxInstanceCount,
            skinnedTlasPlan.stats.
                uploadRouteCountMismatch,
            skinnedTlasPlan.stats.missingCpuRoute,
            skinnedTlasPlan.stats.missingGpuRoute,
            skinnedTlasPlan.stats.
                uploadContractMismatch,
            skinnedTlasPlan.stats.missingResource,
            skinnedTlasPlan.stats.
                resourceContractMismatch,
            skinnedTlasPlan.stats.missingBlas,
            skinnedTlasPlan.stats.
                invalidSbtSelection,
            skinnedTlasPlan.stats.
                tlasCapacityExceeded,
            skinnedTlasPlanInput.
                shaderTableRecordCount);
        if (skinnedTlasDumpRequested &&
            skinnedTlasDumpReady)
        {
            const size_t sampleCount = Min(
                static_cast<size_t>(8),
                skinnedTlasPlan.records.size());
            for (size_t sampleIndex = 0;
                 sampleIndex < sampleCount;
                 ++sampleIndex)
            {
                const PtSkinnedTlasRouteRecord& route =
                    skinnedTlasPlan.records[sampleIndex];
                const PtSkinnedHitRouteRecord* cpuRoute =
                    route.candidateIndex <
                        skinnedTlasPlanInput.
                            candidates.size()
                        ? skinnedTlasPlanInput.candidates[
                            route.candidateIndex].cpuRoute
                        : nullptr;
                common->Printf(
                    "PathTracePrimaryPass: GEO08 skinned TLAS sample=%zu instanceHash=%016llx shaderInstance=%u sourceGen=%llu outputGen=%llu sourceIndexOffset/count=%u/%u outputVertexOffset/count=%u/%u\n",
                    sampleIndex,
                    static_cast<unsigned long long>(
                        cpuRoute
                            ? cpuRoute->instanceHash
                            : 0ull),
                    route.shaderInstanceId,
                    static_cast<unsigned long long>(
                        cpuRoute
                            ? cpuRoute->
                                sourceGpuIndexGeneration
                            : 0ull),
                    static_cast<unsigned long long>(
                        cpuRoute
                            ? cpuRoute->
                                outputStorageGeneration
                            : 0ull),
                    cpuRoute
                        ? cpuRoute->sourceIndexOffset
                        : 0u,
                    cpuRoute
                        ? cpuRoute->indexCount
                        : 0u,
                    cpuRoute
                        ? cpuRoute->outputVertexOffset
                        : 0u,
                    cpuRoute
                        ? cpuRoute->vertexCount
                        : 0u);
            }
            r_pathTracingGeometrySkinnedTlasCompareDump.
                SetInteger(0);
        }
    }

    const bool skinnedEmissivePublishAccepted =
        skinnedGpuComputeDispatched &&
        !skinnedEmissiveGpuWork.empty() &&
        skinnedTlasPlan.result ==
            PtSkinnedTlasRouteResult::Accepted &&
        skinnedTlasDescriptorCount ==
            skinnedTlasPlan.records.size() &&
        skinnedTlasActiveDescriptorCount ==
            static_cast<uint32>(Max(
                captureTiming.skinnedCaptureOmittedSurfaces,
                0)) &&
        captureTiming.skinnedCaptureOmittedSurfaces > 0;
    if (!skinnedEmissiveGpuWork.empty())
    {
        for (PathTraceSkinnedEmissiveGpuWork& work :
            skinnedEmissiveGpuWork)
        {
            if (skinnedEmissivePublishAccepted)
            {
                work.flags |=
                    PT_SKINNED_EMISSIVE_GPU_PUBLISH_ENABLED;
            }
            else
            {
                work.flags &=
                    ~PT_SKINNED_EMISSIVE_GPU_PUBLISH_ENABLED;
            }
        }

        commandList->writeBuffer(
            smokeSkinnedEmissiveWorkBuffer,
            skinnedEmissiveGpuWork.data(),
            skinnedEmissiveGpuWork.size() *
                sizeof(PathTraceSkinnedEmissiveGpuWork));

        const bool skinnedEmissivePublishResourcesReady =
            m_smokeSkinnedEmissivePublishPipeline &&
            m_smokeSkinnedEmissivePublishBindingLayout &&
            smokeSkinnedCurrentOutputVertexBuffer &&
            smokeSkinnedPreviousPositionBuffer &&
            smokeSkinnedEmissiveWorkBuffer &&
            smokeEmissiveTriangleBuffer &&
            smokePreviousEmissiveTriangleBuffer &&
            smokeUnifiedLightBuffer &&
            smokeUnifiedPreviousLightBuffer &&
            smokeRestirLightManagerCurrentPayloadBuffer &&
            smokeRestirLightManagerPreviousPayloadBuffer;
        if (skinnedEmissivePublishResourcesReady)
        {
            nvrhi::BindingSetDesc publishBindingSetDesc;
            publishBindingSetDesc.bindings = {
                nvrhi::BindingSetItem::StructuredBuffer_SRV(
                    0,
                    smokeSkinnedCurrentOutputVertexBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(
                    1,
                    smokeSkinnedPreviousPositionBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_SRV(
                    2,
                    smokeSkinnedEmissiveWorkBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(
                    0,
                    smokeEmissiveTriangleBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(
                    1,
                    smokePreviousEmissiveTriangleBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(
                    2,
                    smokeUnifiedLightBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(
                    3,
                    smokeUnifiedPreviousLightBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(
                    4,
                    smokeRestirLightManagerCurrentPayloadBuffer),
                nvrhi::BindingSetItem::StructuredBuffer_UAV(
                    5,
                    smokeRestirLightManagerPreviousPayloadBuffer)
            };
            m_smokeSkinnedEmissivePublishBindingSet =
                device->createBindingSet(
                    publishBindingSetDesc,
                    m_smokeSkinnedEmissivePublishBindingLayout);
            if (m_smokeSkinnedEmissivePublishBindingSet)
            {
                nvrhi::ComputeState publishComputeState;
                publishComputeState.pipeline =
                    m_smokeSkinnedEmissivePublishPipeline;
                publishComputeState.bindings = {
                    m_smokeSkinnedEmissivePublishBindingSet
                };
                commandList->setComputeState(
                    publishComputeState);
                commandList->dispatch(
                    (static_cast<uint32>(
                        skinnedEmissiveGpuWork.size()) +
                        63u) /
                        64u,
                    1,
                    1);
                commandList->setBufferState(
                    smokeSkinnedCurrentOutputVertexBuffer,
                    nvrhi::ResourceStates::ShaderResource);
                commandList->setBufferState(
                    smokeSkinnedPreviousPositionBuffer,
                    nvrhi::ResourceStates::ShaderResource);
                commandList->setBufferState(
                    smokeSkinnedEmissiveWorkBuffer,
                    nvrhi::ResourceStates::ShaderResource);
                commandList->setBufferState(
                    smokeEmissiveTriangleBuffer,
                    nvrhi::ResourceStates::ShaderResource);
                commandList->setBufferState(
                    smokePreviousEmissiveTriangleBuffer,
                    nvrhi::ResourceStates::ShaderResource);
                commandList->setBufferState(
                    smokeUnifiedLightBuffer,
                    nvrhi::ResourceStates::ShaderResource);
                commandList->setBufferState(
                    smokeUnifiedPreviousLightBuffer,
                    nvrhi::ResourceStates::ShaderResource);
                commandList->setBufferState(
                    smokeRestirLightManagerCurrentPayloadBuffer,
                    nvrhi::ResourceStates::ShaderResource);
                commandList->setBufferState(
                    smokeRestirLightManagerPreviousPayloadBuffer,
                    nvrhi::ResourceStates::ShaderResource);
                commandList->commitBarriers();
                if (skinnedEmissivePublishValidation &&
                    skinnedEmissivePublishAccepted)
                {
                    QueueSkinnedEmissivePublishAudit(
                        commandList,
                        smokeEmissiveTriangleBuffer,
                        smokePreviousEmissiveTriangleBuffer,
                        static_cast<uint32>(
                            skinnedEmissiveCurrentBase),
                        static_cast<uint32>(
                            skinnedEmissivePreviousBase),
                        static_cast<uint32>(
                            skinnedEmissiveGpuWork.size()));
                }
            }
        }

        if (skinnedEmissiveGpuWork.size() >
                m_smokeSkinnedEmissivePublishMaxLogged ||
            (r_pathTracingSmokeLog.GetInteger() != 0 &&
                (m_smokeGeometryFrameIndex % 120ull) == 1ull))
        {
            const size_t previousMapped =
                static_cast<size_t>(std::count_if(
                    skinnedEmissiveGpuWork.begin(),
                    skinnedEmissiveGpuWork.end(),
                    [](const PathTraceSkinnedEmissiveGpuWork& work)
                    {
                        return (work.flags &
                            PT_SKINNED_EMISSIVE_GPU_WRITE_PREVIOUS) !=
                            0u;
                    }));
            common->Printf(
                "PathTracePrimaryPass: GEO09 skinned emissive publication frame=%llu accepted=%d work=%zu previousMapped=%zu pipeline=%d route=canonical-gpu-output\n",
                static_cast<unsigned long long>(
                    geometryUniverseStats.frameIndex),
                skinnedEmissivePublishAccepted ? 1 : 0,
                skinnedEmissiveGpuWork.size(),
                previousMapped,
                m_smokeSkinnedEmissivePublishBindingSet
                    ? 1
                    : 0);
            m_smokeSkinnedEmissivePublishMaxLogged =
                static_cast<uint32>(
                    skinnedEmissiveGpuWork.size());
        }
    }
    else
    {
        m_smokeSkinnedEmissivePublishBindingSet = nullptr;
    }

    std::vector<nvrhi::rt::InstanceDesc>
        liveExtraTlasInstances;
    liveExtraTlasInstances.reserve(
        rigidTlasRouteInstances.size() +
        (staticBucketRouteAccepted
            ? staticBucketFramePublication.
                activePublication.tlasInstances.size()
            : 0u));
    if (staticBucketRouteAccepted)
    {
        liveExtraTlasInstances.insert(
            liveExtraTlasInstances.end(),
            staticBucketFramePublication.
                activePublication.tlasInstances.begin(),
            staticBucketFramePublication.
                activePublication.tlasInstances.end());
    }
    liveExtraTlasInstances.insert(
        liveExtraTlasInstances.end(),
        rigidTlasRouteInstances.begin(),
        rigidTlasRouteInstances.end());

    accelSubmitDesc.commandList = commandList;
    accelSubmitDesc.tlas = m_smokeTlas;
    accelSubmitDesc.staticBlas = smokeStaticBlas;
    accelSubmitDesc.dynamicBlas = smokeDynamicBlas;
    accelSubmitDesc.staticBlasDesc = smokeStaticBlasDesc;
    accelSubmitDesc.dynamicBlasDesc = smokeDynamicBlasDesc;
    accelSubmitDesc.dynamicBlasTimerQuery =
        dynamicBlasGpuTimer
            ? dynamicBlasGpuTimer->query
            : nullptr;
    accelSubmitDesc.extraTlasInstances =
        !liveExtraTlasInstances.empty()
            ? &liveExtraTlasInstances
            : nullptr;
    accelSubmitDesc.hasStaticBlas = hasStaticBlas;
    accelSubmitDesc.hasDynamicBlas = hasDynamicBlas;
    accelSubmitDesc.staticBlasCacheHit = staticBlasCacheHit;
    accelSubmitDesc.includeStaticBlasInTlas =
        !staticBucketRouteAccepted;
    accelSubmitDesc.diagnosticMarkers =
        r_pathTracingNsightGpuMarkers.GetInteger() != 0;
    RtSmokeAccelSubmitTiming accelSubmitTiming;
    bool accelSubmitSucceeded = false;
    if (optickGpuMarkers)
    {
        OPTICK_GPU_EVENT("PT GPU Submit Acceleration Builds");
        accelSubmitSucceeded = SubmitSmokeAccelerationBuilds(accelSubmitDesc, accelSubmitTiming);
    }
    else
    {
        accelSubmitSucceeded = SubmitSmokeAccelerationBuilds(accelSubmitDesc, accelSubmitTiming);
    }
    if (!accelSubmitSucceeded)
    {
        if (dynamicBlasGpuTimer &&
            !accelSubmitTiming.dynamicBlasTimerRecorded)
        {
            dynamicBlasGpuTimer->pending = false;
        }
        common->Printf("PathTracePrimaryPass: failed to submit RT smoke acceleration structures\n");
        return;
    }
    const int blasSubmitMs = accelSubmitTiming.blasSubmitMs;
    const int tlasSubmitMs = accelSubmitTiming.tlasSubmitMs;
    const int accelSubmitMs = accelSubmitTiming.accelSubmitMs;
    if (dynamicBlasGpuTimer)
    {
        dynamicBlasGpuTimer->cpuBlasSubmitUs =
            accelSubmitTiming.blasSubmitMicroseconds;
        dynamicBlasGpuTimer->cpuTlasSubmitUs =
            accelSubmitTiming.tlasSubmitMicroseconds;
        dynamicBlasGpuTimer->cpuAccelSubmitUs =
            accelSubmitTiming.accelSubmitMicroseconds;
    }
    RtPathTraceCpuWorkRecordRenderSubmit(m_smokeCpuWorkState, accelerationPlanGeneration, static_cast<double>(accelSubmitMs));
    const int instanceCount = accelSubmitTiming.instanceCount;
    if (dynamicBlasGpuTimer)
    {
        if (accelSubmitTiming.
                dynamicBlasTimerRecorded)
        {
            dynamicBlasGpuTimer->submitted = true;
            dynamicBlasGpuTimer->buildCount = 1;
        }
        else
        {
            // No timer commands were recorded, so the slot cannot be polled.
            dynamicBlasGpuTimer->pending = false;
        }
    }

    const nvrhi::TextureHandle fallbackTexture = globalImages && globalImages->whiteImage ? globalImages->whiteImage->GetTextureHandle() : nullptr;
    if (!fallbackTexture)
    {
        common->Printf("PathTracePrimaryPass: failed to find RT smoke fallback material texture\n");
        return;
    }

    const bool useStaticBucketResidentPool =
        staticBucketRouteAccepted;
    const nvrhi::BufferHandle routedStaticVertexBuffer =
        useStaticBucketResidentPool
            ? m_staticBucketGeometryUniverse.
                StaticBucketVertexBuffer()
            : smokeStaticVertexBuffer;
    const nvrhi::BufferHandle routedStaticIndexBuffer =
        useStaticBucketResidentPool
            ? m_staticBucketGeometryUniverse.
                StaticBucketIndexBuffer()
            : smokeStaticIndexBuffer;
    const nvrhi::BufferHandle routedStaticTriangleClassBuffer =
        useStaticBucketResidentPool
            ? m_staticBucketGeometryUniverse.
                StaticBucketTriangleClassBuffer()
            : smokeStaticTriangleClassBuffer;
    const nvrhi::BufferHandle routedStaticTriangleMaterialBuffer =
        useStaticBucketResidentPool
            ? m_staticBucketGeometryUniverse.
                StaticBucketTriangleMaterialBuffer()
            : smokeStaticTriangleMaterialBuffer;
    const nvrhi::BufferHandle
        routedStaticTriangleMaterialIndexBuffer =
            useStaticBucketResidentPool
                ? m_staticBucketGeometryUniverse.
                    StaticBucketTriangleMaterialIndexBuffer()
                : smokeStaticTriangleMaterialIndexBuffer;
    const RtSmokeStaticBucketGeometryPack*
        routedStaticGeometryPack =
            useStaticBucketResidentPool
                ? staticBucketFramePublication.geometryPack
                : nullptr;
    const int routedStaticVertexCount =
        routedStaticGeometryPack
            ? routedStaticGeometryPack->stats.packedVertices
            : staticVertexCacheCount;
    const int routedStaticIndexCount =
        routedStaticGeometryPack
            ? routedStaticGeometryPack->stats.packedIndexes
            : staticIndexCacheCount;
    const int routedStaticTriangleCount =
        routedStaticGeometryPack
            ? routedStaticGeometryPack->stats.packedTriangles
            : staticTriangleCacheCount;
    RtSmokeSceneBufferHandles routedSmokeBuffers =
        smokeBuffers;
    routedSmokeBuffers.staticVertexBuffer =
        routedStaticVertexBuffer;
    routedSmokeBuffers.staticIndexBuffer =
        routedStaticIndexBuffer;
    routedSmokeBuffers.staticTriangleClassBuffer =
        routedStaticTriangleClassBuffer;
    routedSmokeBuffers.staticTriangleMaterialBuffer =
        routedStaticTriangleMaterialBuffer;
    routedSmokeBuffers.staticTriangleMaterialIndexBuffer =
        routedStaticTriangleMaterialIndexBuffer;
    if (useStaticBucketResidentPool)
    {
        // Static world buckets do not deform. Reusing the stable resident pool
        // for previous-static reads preserves the same global triangle address
        // across active-set-only TLAS changes.
        routedSmokeBuffers.previousStaticVertexBuffer =
            routedStaticVertexBuffer;
        routedSmokeBuffers.previousStaticIndexBuffer =
            routedStaticIndexBuffer;
        routedSmokeBuffers.previousStaticTriangleClassBuffer =
            routedStaticTriangleClassBuffer;
        routedSmokeBuffers.previousStaticTriangleMaterialBuffer =
            routedStaticTriangleMaterialBuffer;
        routedSmokeBuffers.previousStaticTriangleMaterialIndexBuffer =
            routedStaticTriangleMaterialIndexBuffer;
    }

    if (asyncRigidRouteSideBufferRing && rigidRouteSideBufferWriteSlot >= 0)
    {
        const RtSmokeRigidRouteSideBufferSlot& sideSlot =
            m_smokeRigidRouteSideBufferSlots[rigidRouteSideBufferWriteSlot];
        if (!sideSlot.generationValid ||
            !RtPathTraceCpuWorkGenerationEquals(sideSlot.generation, rigidRouteSideBufferGeneration))
        {
            common->Printf("PathTracePrimaryPass: async BVH side-buffer generation mismatch slot=%d expected(scene=%llu geometry=%llu material=%llu input=%llu)\n",
                rigidRouteSideBufferWriteSlot,
                static_cast<unsigned long long>(rigidRouteSideBufferGeneration.sceneGeneration),
                static_cast<unsigned long long>(rigidRouteSideBufferGeneration.geometryGeneration),
                static_cast<unsigned long long>(rigidRouteSideBufferGeneration.materialGeneration),
                static_cast<unsigned long long>(rigidRouteSideBufferGeneration.lightGeneration));
        }
    }

    RtSmokeBindingBuildDesc bindingBuildDesc;
    {
        OPTICK_EVENT("PT Binding Desc Build");
    bindingBuildDesc.device = device;
    bindingBuildDesc.tlas = m_smokeTlas;
    bindingBuildDesc.outputTexture = m_frameResources.outputTexture;
    bindingBuildDesc.accumulationTexture = m_frameResources.accumulationTexture;
    bindingBuildDesc.restirPTReflectionTexture = m_frameResources.restirPTReflectionTexture;
    bindingBuildDesc.rrInputColorTexture = m_frameResources.rrInputColorTexture;
    bindingBuildDesc.motionVectorTexture = m_frameResources.motionVectorTexture;
    bindingBuildDesc.rrMotionVectorTexture = m_frameResources.rrMotionVectorTexture;
    bindingBuildDesc.motionVectorMaskTexture = m_frameResources.motionVectorMaskTexture;
    bindingBuildDesc.rrGuideAlbedoTexture = m_frameResources.rrGuideAlbedoTexture;
    bindingBuildDesc.rrGuideSpecularAlbedoTexture = m_frameResources.rrGuideSpecularAlbedoTexture;
    bindingBuildDesc.rrGuideNormalRoughnessTexture = m_frameResources.rrGuideNormalRoughnessTexture;
    bindingBuildDesc.rrGuideDepthTexture = m_frameResources.rrGuideDepthTexture;
    bindingBuildDesc.rrGuideHitDistanceTexture = m_frameResources.rrGuideHitDistanceTexture;
    bindingBuildDesc.rrGuideResetMaskTexture = m_frameResources.rrGuideResetMaskTexture;
    bindingBuildDesc.rrGuidePositionTexture = m_frameResources.rrGuidePositionTexture;
    bindingBuildDesc.fallbackTexture = fallbackTexture;
    bindingBuildDesc.skyEnvironmentCube = skyEnvironmentCube;
    bindingBuildDesc.constantsBuffer = m_smokeConstantsBuffer;
    bindingBuildDesc.boundsOverlayLineBuffer = m_smokeBoundsOverlayLineBuffer;
    bindingBuildDesc.liquidPoolStatusBuffer = m_liquidPoolStatusBuffer;
    bindingBuildDesc.bindingLayout = m_smokeBindingLayout;
    bindingBuildDesc.textureBindlessLayout = m_smokeTextureBindlessLayout;
    const int sceneRetireFrames = idMath::ClampInt(0, 32, r_pathTracingSceneRetireFrames.GetInteger());
    bindingBuildDesc.existingTextureDescriptorTable = m_smokeTextureDescriptorTable;
    bindingBuildDesc.existingActiveTextureTable = &m_smokeActiveTextureTable;
    bindingBuildDesc.allowExistingTextureDescriptorTableWrites = sceneRetireFrames <= 0;
    bindingBuildDesc.sampler = m_backend->GetCommonPasses().m_AnisotropicWrapSampler;
    bindingBuildDesc.buffers = routedSmokeBuffers;
    bindingBuildDesc.skinnedSourceIndexBuffer =
        m_smokeGeometryUniverse.CanonicalSourceIndexBuffer();
    bindingBuildDesc.primarySurfaceHistoryBuffers = m_frameResources.primarySurfaceHistoryBuffers;
    bindingBuildDesc.enableTextureProbe = enableTextureProbe;
    bindingBuildDesc.forceFallbackTexture = r_pathTracingTextureForceFallback.GetInteger() != 0;
    bindingBuildDesc.maxActiveTextures = RT_SMOKE_TEXTURE_EXPERIMENTAL_ACTIVE_CAP;
    }

    RtSmokeBindingBuildResult bindingBuildResult;
    {
        OPTICK_EVENT("PT Create Binding Resources");
        bindingBuildResult = CreateSmokeBindingResources(bindingBuildDesc, materialTable);
    }
    if (!bindingBuildResult.Succeeded())
    {
        if (bindingBuildResult.failedTextureSlot >= 0)
        {
            common->Printf("PathTracePrimaryPass: %s %d\n", bindingBuildResult.errorMessage, bindingBuildResult.failedTextureSlot);
        }
        else
        {
            common->Printf("PathTracePrimaryPass: %s\n", bindingBuildResult.errorMessage ? bindingBuildResult.errorMessage : "failed to create RT smoke binding resources");
        }
        return;
    }

    uint64 sceneInputCameraSignature = 1469598103934665603ull;
    if (viewDef)
    {
        sceneInputCameraSignature = HashSmokeBytes(sceneInputCameraSignature, &viewDef->renderView.vieworg, sizeof(viewDef->renderView.vieworg));
        sceneInputCameraSignature = HashSmokeBytes(sceneInputCameraSignature, &viewDef->renderView.viewaxis, sizeof(viewDef->renderView.viewaxis));
        sceneInputCameraSignature = HashSmokeBytes(sceneInputCameraSignature, &viewDef->renderView.fov_x, sizeof(viewDef->renderView.fov_x));
        sceneInputCameraSignature = HashSmokeBytes(sceneInputCameraSignature, &viewDef->renderView.fov_y, sizeof(viewDef->renderView.fov_y));
    }

    uint64 sceneInputLightSignature = 1469598103934665603ull;
    sceneInputLightSignature = HashSmokeBytes(sceneInputLightSignature, &emissiveInventoryStats.capturedTriangles, sizeof(emissiveInventoryStats.capturedTriangles));
    sceneInputLightSignature = HashSmokeBytes(sceneInputLightSignature, &emissiveInventoryStats.candidateMaterials, sizeof(emissiveInventoryStats.candidateMaterials));
    const int doomAnalyticLightCountForSignature = static_cast<int>(doomAnalyticLights.size());
    sceneInputLightSignature = HashSmokeBytes(sceneInputLightSignature, &doomAnalyticLightCountForSignature, sizeof(doomAnalyticLightCountForSignature));
    const int doomAnalyticRemapCountForSignature = static_cast<int>(doomAnalyticRemap.universeRemap.size());
    sceneInputLightSignature = HashSmokeBytes(sceneInputLightSignature, &doomAnalyticRemapCountForSignature, sizeof(doomAnalyticRemapCountForSignature));

    const uint64_t staticUploadBytes = SumSmokeUploadBytes(uploadItems, 0, 5);
    const uint64_t previousStaticUploadBytes = SumSmokeUploadBytes(uploadItems, 5, 5);
    const uint64_t previousStaticUploadSkippedBytes = SumSmokeSkippedUploadBytes(uploadItems, 5, 5);
    const uint64_t dynamicUploadBytes = SumSmokeUploadBytes(uploadItems, 10, 5);
    const uint64_t materialUploadBytes = SumSmokeUploadBytes(uploadItems, 15, 3);
    const uint64_t lightUploadBytes = SumSmokeUploadBytes(uploadItems, 18, 17);
    const uint64_t rigidRouteGeometryBytes =
        rigidRouteBuild.vertices.size() * sizeof(PathTraceSmokeVertex) +
        rigidRouteBuild.indexes.size() * sizeof(uint32_t) +
        rigidRouteBuild.triangleMaterials.size() * sizeof(uint32_t) +
        rigidRouteBuild.triangleMaterialIndexes.size() * sizeof(uint32_t);
    const uint64_t rigidRouteInstanceBytes = rigidRouteBuild.instances.size() * sizeof(PathTraceRigidRouteInstance);
    const uint64_t rigidRouteUploadBytes =
        (skipRigidRouteSideBufferUpload ? 0ull : rigidRouteGeometryBytes) +
        (skipRigidRouteInstanceBufferUpload ? 0ull : rigidRouteInstanceBytes);
    const uint64_t rigidRouteSkippedUploadBytes =
        (skipRigidRouteSideBufferUpload ? rigidRouteGeometryBytes : 0ull) +
        (skipRigidRouteInstanceBufferUpload ? rigidRouteInstanceBytes : 0ull);
    if (r_pathTracingRigidRouteDump.GetInteger() != 0)
    {
        common->Printf("PathTracePrimaryPass: PT rigid route dump source=%d frame=%llu enabled=%d instances=%d uniqueMeshes=%d max=%d seen/cache=%d/%d prevXform/continuous=%d/%d verts/indexes/tris=%d/%d/%d bytes(geom/inst/upload/skip)=%llu/%llu/%llu/%llu buildMs=%d async/cache/queued=%d/%d/%d sideRing(skipGeom/skipInst/slot/read)=%d/%d/%d/%d residency(cached/resident/retained/meshLive/meshAged/retiredBlas/feedCap)=%d/%d/%d/%d/%d/%d/%d skipped nonRigid/missingMesh/missingBlas=%d/%d/%d missingMaterialIndex=%d\n",
            sceneSource,
            static_cast<unsigned long long>(m_smokeGeometryFrameIndex),
            buildRigidRouteBuffers ? 1 : 0,
            rigidRouteBuild.stats.emittedInstances,
            rigidRouteBuild.stats.emittedUniqueMeshes,
            rigidRouteMaxInstances,
            rigidRouteBuild.stats.emittedSeenThisFrame,
            rigidRouteBuild.stats.emittedFromCache,
            rigidRouteBuild.stats.previousTransformInstances,
            rigidRouteBuild.stats.transformContinuousInstances,
            rigidRouteBuild.stats.vertices,
            rigidRouteBuild.stats.indexes,
            rigidRouteBuild.stats.triangles,
            static_cast<unsigned long long>(rigidRouteGeometryBytes),
            static_cast<unsigned long long>(rigidRouteInstanceBytes),
            static_cast<unsigned long long>(rigidRouteUploadBytes),
            static_cast<unsigned long long>(rigidRouteSkippedUploadBytes),
            rigidRouteBuildMs,
            rigidRouteBuildAcceptedFromAsync ? 1 : 0,
            rigidRouteBuildAsyncCached ? 1 : 0,
            rigidRouteBuildAsyncQueued ? 1 : 0,
            skipRigidRouteSideBufferUpload ? 1 : 0,
            skipRigidRouteInstanceBufferUpload ? 1 : 0,
            rigidRouteSideBufferWriteSlot,
            m_smokeRigidRouteSideBufferReadSlot,
            rigidResidencyStats.cachedRigidInstances,
            rigidResidencyStats.residentInstances,
            rigidResidencyStats.residentRetainedOffscreen,
            rigidResidencyStats.meshLive,
            rigidResidencyStats.meshAgedOut,
            rigidResidencyStats.retiredBlasPending,
            rigidResidencyStats.residencyEntityFeedCap,
            rigidRouteBuild.stats.skippedNonRigid,
            rigidRouteBuild.stats.skippedMissingMesh,
            rigidRouteBuild.stats.skippedMissingBlas,
            rigidRouteBuild.stats.missingMaterialTableIndex);
        r_pathTracingRigidRouteDump.SetInteger(0);
    }

    RtPathTraceSceneInputs sceneInputs;
    {
        OPTICK_EVENT("PT Scene Input Populate");
    sceneInputs.valid = true;
    sceneInputs.sceneSource = sceneSource;
    sceneInputs.debugMode = requestedDebugMode;
    sceneInputs.outputWidth = m_frameResources.width;
    sceneInputs.outputHeight = m_frameResources.height;
    sceneInputs.capabilityFlags = RT_SCENE_INPUT_MATERIAL_STOPGAP_CLASSIFIER |
        RT_SCENE_INPUT_MATERIAL_IDTECH4_SEMANTICS_RESERVED |
        RT_SCENE_INPUT_MATERIAL_PBR_ROLES_RESERVED |
        RT_SCENE_INPUT_GEOMETRY_PREVIOUS_TRANSFORM_RESERVED |
        RT_SCENE_INPUT_GEOMETRY_PREVIOUS_VERTEX_RESERVED |
        RT_SCENE_INPUT_SKINNED_SOURCE_GEOMETRY_RESERVED |
        RT_SCENE_INPUT_SKINNED_GPU_SKINNING_RESERVED |
        RT_SCENE_INPUT_LIGHT_PREVIOUS_IDENTITY_RESERVED;
    if (sceneSource == 3)
    {
        sceneInputs.capabilityFlags |= RT_SCENE_INPUT_SOURCE3_BASELINE | RT_SCENE_INPUT_PORTAL_AREA_RESIDENCY | RT_SCENE_INPUT_PORTAL_BLOCK_VIEW_REPORTED;
    }
    if (sceneSource == 0)
    {
        sceneInputs.capabilityFlags |= RT_SCENE_INPUT_SOURCE0_EMERGENCY_FALLBACK;
    }
    sceneInputs.signatures.geometryMembership = staticSignature.hash;
    sceneInputs.signatures.materialTable = materialTableSignature;
    sceneInputs.signatures.lightMembership = sceneInputLightSignature;
    sceneInputs.signatures.outputResolution = (static_cast<uint64>(m_frameResources.width) << 32) | static_cast<uint32_t>(m_frameResources.height);
    sceneInputs.signatures.cameraProjection = sceneInputCameraSignature;
    sceneInputs.signatures.debugFeaturePolicy = static_cast<uint64>(requestedDebugMode);
    sceneInputs.signatures.cpuUploadGeneration = m_smokeGeometryFrameIndex;
    sceneInputs.signatures.reservoirScene = reservoirSceneSignature;

    sceneInputs.portalPolicy.sceneSource = sceneSource;
    sceneInputs.portalPolicy.viewArea = viewDef ? viewDef->areaNum : -1;
    sceneInputs.portalPolicy.currentArea = viewDef ? viewDef->areaNum : -1;
    sceneInputs.portalPolicy.totalAreas = 0;
    sceneInputs.portalPolicy.staticAreaPreloadSteps = idMath::ClampInt(0, 8, r_pathTracingStaticAreaPreloadPortalSteps.GetInteger());
    sceneInputs.portalPolicy.rigidResidencySteps = idMath::ClampInt(0, 8, r_pathTracingRigidResidencyPortalSteps.GetInteger());
    sceneInputs.portalPolicy.lightAreaSteps = idMath::ClampInt(0, 8, r_pathTracingLightAreaPortalSteps.GetInteger());
    sceneInputs.portalPolicy.selectedAreaCount = 0;
    sceneInputs.portalPolicy.portalEdges = 0;
    sceneInputs.portalPolicy.blockedPortalEdges = 0;
    sceneInputs.portalPolicy.rigidSelectedAreaCount = rigidResidencyStats.selectedAreas;
    sceneInputs.portalPolicy.rigidPortalEdges = rigidResidencyStats.portalEdges;
    sceneInputs.portalPolicy.rigidBlockedPortalEdges = rigidResidencyStats.blockedPortalEdges;
    sceneInputs.portalPolicy.bruteForceFullMap = r_pathTracingPortalBruteforceFullMap.GetInteger() != 0;
    sceneInputs.portalPolicy.defaultPolicyEquivalent =
        sceneInputs.portalPolicy.staticAreaPreloadSteps == sceneInputs.portalPolicy.rigidResidencySteps &&
        sceneInputs.portalPolicy.rigidResidencySteps == sceneInputs.portalPolicy.lightAreaSteps;

    sceneInputs.geometry.tlas = m_smokeTlas;
    sceneInputs.geometry.staticBlas = smokeStaticBlas;
    sceneInputs.geometry.dynamicBlas = smokeDynamicBlas;
    sceneInputs.geometry.staticVertexBuffer =
        routedStaticVertexBuffer;
    sceneInputs.geometry.staticIndexBuffer =
        routedStaticIndexBuffer;
    sceneInputs.geometry.staticTriangleClassBuffer =
        routedStaticTriangleClassBuffer;
    sceneInputs.geometry.staticTriangleMaterialBuffer =
        routedStaticTriangleMaterialBuffer;
    sceneInputs.geometry.staticTriangleMaterialIndexBuffer =
        routedStaticTriangleMaterialIndexBuffer;
    sceneInputs.geometry.previousStaticVertexBuffer =
        useStaticBucketResidentPool
            ? routedStaticVertexBuffer
            : smokePreviousStaticVertexBuffer;
    sceneInputs.geometry.previousStaticIndexBuffer =
        useStaticBucketResidentPool
            ? routedStaticIndexBuffer
            : smokePreviousStaticIndexBuffer;
    sceneInputs.geometry.previousStaticTriangleClassBuffer =
        useStaticBucketResidentPool
            ? routedStaticTriangleClassBuffer
            : smokePreviousStaticTriangleClassBuffer;
    sceneInputs.geometry.previousStaticTriangleMaterialBuffer =
        useStaticBucketResidentPool
            ? routedStaticTriangleMaterialBuffer
            : smokePreviousStaticTriangleMaterialBuffer;
    sceneInputs.geometry.previousStaticTriangleMaterialIndexBuffer =
        useStaticBucketResidentPool
            ? routedStaticTriangleMaterialIndexBuffer
            : smokePreviousStaticTriangleMaterialIndexBuffer;
    sceneInputs.geometry.dynamicVertexBuffer = smokeDynamicVertexBuffer;
    sceneInputs.geometry.dynamicIndexBuffer = smokeDynamicIndexBuffer;
    sceneInputs.geometry.dynamicTriangleClassBuffer = smokeDynamicTriangleClassBuffer;
    sceneInputs.geometry.dynamicTriangleMaterialBuffer = smokeDynamicTriangleMaterialBuffer;
    sceneInputs.geometry.dynamicTriangleMaterialIndexBuffer = smokeDynamicTriangleMaterialIndexBuffer;
    sceneInputs.geometry.rigidRouteVertexBuffer = smokeRigidRouteVertexBuffer;
    sceneInputs.geometry.rigidRouteIndexBuffer = smokeRigidRouteIndexBuffer;
    sceneInputs.geometry.rigidRouteTriangleMaterialBuffer = smokeRigidRouteTriangleMaterialBuffer;
    sceneInputs.geometry.rigidRouteTriangleMaterialIndexBuffer = smokeRigidRouteTriangleMaterialIndexBuffer;
    sceneInputs.geometry.rigidRouteInstanceBuffer = smokeRigidRouteInstanceBuffer;
    sceneInputs.geometry.skinnedHitRouteRecordBuffer = smokeSkinnedHitRouteRecordBuffer;
    sceneInputs.geometry.skinnedHitRouteTriangleBuffer = smokeSkinnedHitRouteTriangleBuffer;
    sceneInputs.geometry.skinnedSourceIndexBuffer =
        m_smokeGeometryUniverse.CanonicalSourceIndexBuffer();
    sceneInputs.geometry.skinnedSourceVertexBuffer = smokeSkinnedSourceVertexBuffer;
    sceneInputs.geometry.skinnedCurrentOutputVertexBuffer = smokeSkinnedCurrentOutputVertexBuffer;
    sceneInputs.geometry.skinnedPreviousPositionBuffer = smokeSkinnedPreviousPositionBuffer;
    sceneInputs.geometry.skinnedSurfaceDispatchBuffer = smokeSkinnedSurfaceDispatchBuffer;
    sceneInputs.geometry.skinnedTriangleDispatchIndexBuffer = smokeSkinnedTriangleDispatchIndexBuffer;
    sceneInputs.geometry.skinnedCurrentJointMatrixBuffer = smokeSkinnedCurrentJointMatrixBuffer;
    sceneInputs.geometry.skinnedPreviousJointMatrixBuffer = smokeSkinnedPreviousJointMatrixBuffer;
    sceneInputs.geometry.staticBucketRouteFirstInstanceId =
        RT_PATH_TRACE_STATIC_BUCKET_INSTANCE_ID_BASE;
    sceneInputs.geometry.staticBucketTriangleCount =
        staticBucketRouteAccepted &&
            staticBucketFramePublication.activePublication.valid &&
            staticBucketFramePublication.materialIndexUploaded &&
            staticBucketFramePublication.geometryPack
            ? static_cast<uint32_t>(
                staticBucketFramePublication.
                    geometryPack->triangleClasses.size())
            : 0u;
    sceneInputs.geometry.staticBucketRouteGeneration =
        sceneInputs.geometry.staticBucketTriangleCount > 0
            ? staticBucketFramePublication.
                activePublication.publicationGeneration
            : 0u;
    sceneInputs.geometry.staticBucketRoutePublicationValid =
        staticBucketRouteAccepted &&
        sceneInputs.geometry.staticBucketTriangleCount > 0 &&
        staticBucketFramePublication.activePublication.valid &&
        staticBucketFramePublication.materialIndexUploaded;
    sceneInputs.geometry.staticVertexCount =
        routedStaticVertexCount;
    sceneInputs.geometry.staticIndexCount =
        routedStaticIndexCount;
    sceneInputs.geometry.staticTriangleCount =
        routedStaticTriangleCount;
    sceneInputs.geometry.staticMaterialIndexCount =
        useStaticBucketResidentPool
            ? static_cast<int>(
                staticBucketFramePublication.
                    materialIndexes->size())
            : static_cast<int>(
                materialTable.staticMaterialIndexes.size());
    sceneInputs.geometry.previousStaticVertexCount = m_sceneInputs.geometry.staticVertexCount;
    sceneInputs.geometry.previousStaticIndexCount = m_sceneInputs.geometry.staticIndexCount;
    sceneInputs.geometry.previousStaticTriangleCount = m_sceneInputs.geometry.staticTriangleCount;
    sceneInputs.geometry.previousStaticMaterialIndexCount = m_sceneInputs.geometry.staticMaterialIndexCount;
    sceneInputs.geometry.previousStaticCpuVertexCount = geometryUniverseStats.previousStaticVerts;
    sceneInputs.geometry.previousStaticCpuIndexCount = geometryUniverseStats.previousStaticIndexes;
    sceneInputs.geometry.previousStaticCpuTriangleCount = geometryUniverseStats.previousStaticTriangles;
    sceneInputs.geometry.previousStaticCpuMaterialIndexCount = static_cast<int>(previousStaticTriangleMaterialIndexCache.size());
    sceneInputs.geometry.previousStaticCpuBytesKB = geometryUniverseStats.previousStaticBytesKB;
    sceneInputs.geometry.staticSeenSurfaceCount = geometryUniverseStats.staticSeenThisFrame;
    sceneInputs.geometry.staticNewSurfaceCount = geometryUniverseStats.staticNewThisFrame;
    sceneInputs.geometry.staticGoneSurfaceCount = geometryUniverseStats.staticDisappearedThisFrame;
    sceneInputs.geometry.staticHistoryValidSurfaceCount = geometryUniverseStats.staticHistoryValid;
    sceneInputs.geometry.staticPreviousRangeValidSurfaceCount = geometryUniverseStats.staticPreviousRangeValid;
    sceneInputs.geometry.staticDirtySurfaceCount = geometryUniverseStats.staticDirty;
    sceneInputs.geometry.staticDirtyVertexOffset = geometryUniverseStats.staticDirtyVertexOffset;
    sceneInputs.geometry.staticDirtyVertexCount = geometryUniverseStats.staticDirtyVertexCount;
    sceneInputs.geometry.staticDirtyIndexOffset = geometryUniverseStats.staticDirtyIndexOffset;
    sceneInputs.geometry.staticDirtyIndexCount = geometryUniverseStats.staticDirtyIndexCount;
    sceneInputs.geometry.staticDirtyTriangleOffset = geometryUniverseStats.staticDirtyTriangleOffset;
    sceneInputs.geometry.staticDirtyTriangleCount = geometryUniverseStats.staticDirtyTriangleCount;
    sceneInputs.geometry.staticDirtyRangeUploadUsed = useStaticDirtyRangeUploads;
    sceneInputs.geometry.staticPreviousCountsMatch =
        m_sceneInputs.valid &&
        m_sceneInputs.geometry.staticVertexCount == staticVertexCacheCount &&
        m_sceneInputs.geometry.staticIndexCount == staticIndexCacheCount &&
        m_sceneInputs.geometry.staticTriangleCount == staticTriangleCacheCount;
    const bool staticPreviousMaterialIndexCountsMatch =
        sceneInputs.geometry.previousStaticMaterialIndexCount > 0 &&
        sceneInputs.geometry.previousStaticCpuMaterialIndexCount == sceneInputs.geometry.previousStaticMaterialIndexCount;
    sceneInputs.geometry.staticPreviousRangesComplete =
        sceneInputs.geometry.staticSeenSurfaceCount > 0 &&
        sceneInputs.geometry.staticPreviousRangeValidSurfaceCount == sceneInputs.geometry.staticSeenSurfaceCount;
    sceneInputs.geometry.staticPreviousBuffersAvailable =
        sceneInputs.geometry.staticPreviousCountsMatch &&
        sceneInputs.geometry.staticPreviousRangesComplete &&
        sceneInputs.geometry.previousStaticVertexBuffer &&
        sceneInputs.geometry.previousStaticIndexBuffer &&
        sceneInputs.geometry.previousStaticTriangleClassBuffer &&
        sceneInputs.geometry.previousStaticTriangleMaterialBuffer &&
        sceneInputs.geometry.previousStaticTriangleMaterialIndexBuffer &&
        staticPreviousMaterialIndexCountsMatch;
    sceneInputs.geometry.staticPreviousMaterialIndexBufferAvailable =
        sceneInputs.geometry.staticPreviousBuffersAvailable &&
        sceneInputs.geometry.previousStaticTriangleMaterialIndexBuffer;
    sceneInputs.geometry.staticPreviousGpuSnapshotAvailable =
        sceneInputs.geometry.staticPreviousBuffersAvailable &&
        sceneInputs.geometry.previousStaticVertexBuffer &&
        sceneInputs.geometry.previousStaticIndexBuffer &&
        sceneInputs.geometry.previousStaticTriangleClassBuffer &&
        sceneInputs.geometry.previousStaticTriangleMaterialBuffer &&
        sceneInputs.geometry.previousStaticTriangleMaterialIndexBuffer;
    sceneInputs.geometry.staticPreviousGpuSnapshotUploadUsed =
        previousStaticSnapshotDataAvailable &&
        sceneInputs.geometry.staticPreviousGpuSnapshotAvailable &&
        (!skipPreviousStaticGeometryUpload || !skipPreviousStaticMaterialIndexUpload);
    sceneInputs.geometry.staticPreviousBuffersAliasCurrent =
        sceneInputs.geometry.staticPreviousBuffersAvailable &&
        sceneInputs.geometry.previousStaticVertexBuffer == sceneInputs.geometry.staticVertexBuffer &&
        sceneInputs.geometry.previousStaticIndexBuffer == sceneInputs.geometry.staticIndexBuffer &&
        sceneInputs.geometry.previousStaticTriangleClassBuffer == sceneInputs.geometry.staticTriangleClassBuffer &&
        sceneInputs.geometry.previousStaticTriangleMaterialBuffer == sceneInputs.geometry.staticTriangleMaterialBuffer &&
        sceneInputs.geometry.previousStaticTriangleMaterialIndexBuffer == sceneInputs.geometry.staticTriangleMaterialIndexBuffer;
    sceneInputs.geometry.staticPreviousCpuSnapshotAvailable = geometryUniverseStats.previousStaticCpuSnapshotAvailable;
    sceneInputs.geometry.dynamicVertexCount = dynamicVertexCount;
    sceneInputs.geometry.dynamicIndexCount = dynamicIndexCount;
    sceneInputs.geometry.dynamicTriangleCount = dynamicIndexCount / 3;
    sceneInputs.geometry.dynamicClassifiedSurfaceCount =
        dynamicStats.rigidSurfaces +
        dynamicStats.skinnedCpuCurrentSurfaces +
        dynamicStats.skinnedLikelyBasePoseSurfaces +
        dynamicStats.skinnedRtCpuSkinnedSurfaces +
        dynamicStats.particleAlphaSurfaces +
        dynamicStats.unknownSurfaces;
    sceneInputs.geometry.dynamicClassifiedTriangleCount =
        (dynamicStats.rigidIndexes +
        dynamicStats.skinnedCpuCurrentIndexes +
        dynamicStats.skinnedLikelyBasePoseIndexes +
        dynamicStats.skinnedRtCpuSkinnedIndexes +
        dynamicStats.particleAlphaIndexes +
        dynamicStats.unknownIndexes) / 3;
    sceneInputs.geometry.dynamicClassifiedTriangleDelta =
        sceneInputs.geometry.dynamicTriangleCount - sceneInputs.geometry.dynamicClassifiedTriangleCount;
    sceneInputs.geometry.dynamicClassifiedCountsMatch =
        sceneInputs.geometry.dynamicClassifiedTriangleDelta == 0;
    sceneInputs.geometry.dynamicRigidSurfaceCount = dynamicStats.rigidSurfaces;
    sceneInputs.geometry.dynamicRigidTriangleCount = dynamicStats.rigidIndexes / 3;
    sceneInputs.geometry.dynamicSkinnedCpuCurrentSurfaceCount = dynamicStats.skinnedCpuCurrentSurfaces;
    sceneInputs.geometry.dynamicSkinnedCpuCurrentTriangleCount = dynamicStats.skinnedCpuCurrentIndexes / 3;
    sceneInputs.geometry.dynamicSkinnedLikelyBasePoseSurfaceCount = dynamicStats.skinnedLikelyBasePoseSurfaces;
    sceneInputs.geometry.dynamicSkinnedLikelyBasePoseTriangleCount = dynamicStats.skinnedLikelyBasePoseIndexes / 3;
    sceneInputs.geometry.dynamicSkinnedRtCpuSurfaceCount = dynamicStats.skinnedRtCpuSkinnedSurfaces;
    sceneInputs.geometry.dynamicSkinnedRtCpuTriangleCount = dynamicStats.skinnedRtCpuSkinnedIndexes / 3;
    sceneInputs.geometry.dynamicParticleAlphaSurfaceCount = dynamicStats.particleAlphaSurfaces;
    sceneInputs.geometry.dynamicParticleAlphaTriangleCount = dynamicStats.particleAlphaIndexes / 3;
    sceneInputs.geometry.dynamicUnknownSurfaceCount = dynamicStats.unknownSurfaces;
    sceneInputs.geometry.dynamicUnknownTriangleCount = dynamicStats.unknownIndexes / 3;
    sceneInputs.geometry.dynamicRetainedOccluderSurfaceCount = dynamicStats.retainedOccluderSurfaces;
    sceneInputs.geometry.dynamicRetainedOccluderTriangleCount = dynamicStats.retainedOccluderIndexes / 3;
    sceneInputs.geometry.rigidRouteVertexCount = rigidRouteBuild.stats.vertices;
    sceneInputs.geometry.rigidRouteIndexCount = rigidRouteBuild.stats.indexes;
    sceneInputs.geometry.rigidRouteTriangleCount = rigidRouteBuild.stats.triangles;
    sceneInputs.geometry.rigidRouteInstanceCount = rigidRouteBuild.stats.emittedInstances;
    sceneInputs.geometry.rigidRoutePreviousTransformCount = rigidRouteBuild.stats.previousTransformInstances;
    sceneInputs.geometry.skinnedHitRouteRecordCount =
        skinnedHitRouteGpuUpload.records.empty()
            ? 0
            : static_cast<int>(
                skinnedHitRouteGpuUpload.records.front().routeCount);
    sceneInputs.geometry.skinnedHitRouteTriangleCount =
        skinnedHitRouteGpuUpload.records.empty()
            ? 0
            : static_cast<int>(
                skinnedHitRouteGpuUpload.records.front().
                    triangleMetadataCount);
    sceneInputs.geometry.skinnedPreviousPositionCount = static_cast<int>(skinnedGpuScaffold.previousPositions.size());
    sceneInputs.geometry.skinnedSurfaceDispatchCount = static_cast<int>(skinnedGpuScaffold.dispatchRecords.size());
    sceneInputs.geometry.skinnedTriangleDispatchIndexCount = static_cast<int>(skinnedGpuScaffold.dynamicTriangleDispatchIndexes.size());
    sceneInputs.geometry.skinnedGpuComputeVertexCount = skinnedGpuComputeReady ? skinnedGpuComputeVertexCount : 0;
    sceneInputs.geometry.skinnedGpuComputeMaxVertexCount = skinnedGpuComputeReady ? skinnedGpuComputeMaxVertexCount : 0;
    sceneInputs.geometry.currentGeometryValid = hasStaticBlas || hasDynamicBlas;
    sceneInputs.geometry.previousTransformAvailable = rigidRouteBuild.stats.previousTransformInstances > 0;
    sceneInputs.geometry.skinnedPreviousPositionBufferAvailable =
        smokeSkinnedPreviousPositionBuffer &&
        !skinnedGpuScaffold.previousPositions.empty();
    sceneInputs.geometry.skinnedGpuComputeDispatched = skinnedGpuComputeDispatched;
    sceneInputs.geometry.skinnedGpuComputeWritesPreviousPositions = skinnedGpuComputeDispatched && skinnedGpuComputeWritesPreviousPositions;
    sceneInputs.geometry.capabilityFlags =
        RT_SCENE_INPUT_GEOMETRY_PREVIOUS_TRANSFORM_RESERVED |
        RT_SCENE_INPUT_GEOMETRY_PREVIOUS_VERTEX_RESERVED |
        RT_SCENE_INPUT_SKINNED_SOURCE_GEOMETRY_RESERVED |
        RT_SCENE_INPUT_SKINNED_GPU_SKINNING_RESERVED;

    sceneInputs.materials.materialTableBuffer = smokeMaterialTableBuffer;
    sceneInputs.materials.materialFeatureBuffer = smokeMaterialFeatureBuffer;
    sceneInputs.materials.materialFeatureParameterBuffer = smokeMaterialFeatureParameterBuffer;
    sceneInputs.materials.dynamicMaterialBuffer = smokeDynamicMaterialBuffer;
    sceneInputs.materials.textureDescriptorTable = bindingBuildResult.textureDescriptorTable;
    sceneInputs.materials.materialTableEntryCount = static_cast<int>(materialTable.materials.size());
    sceneInputs.materials.materialFeatureRecordCount = static_cast<int>(materialTable.materialFeatures.size());
    sceneInputs.materials.materialFeatureParameterRecordCount = static_cast<int>(materialTable.materialFeatureParameters.size());
    sceneInputs.materials.dynamicMaterialRecordCount = static_cast<int>(dynamicMaterialRecords.size());
    sceneInputs.materials.materialTableGpuStable = stableGpuMaterialTableCovered;
    sceneInputs.materials.activeTextureCount = static_cast<int>(bindingBuildResult.activeTextureTable.size());
    sceneInputs.materials.materialTablePath = materialTablePath;
    sceneInputs.materials.capabilityFlags = RT_SCENE_INPUT_MATERIAL_STOPGAP_CLASSIFIER | RT_SCENE_INPUT_MATERIAL_IDTECH4_SEMANTICS_RESERVED | RT_SCENE_INPUT_MATERIAL_PBR_ROLES_RESERVED;
    if (!dynamicMaterialRecords.empty())
    {
        sceneInputs.materials.capabilityFlags |= RT_SCENE_INPUT_MATERIAL_DYNAMIC_CHANNEL_RESERVED;
        sceneInputs.capabilityFlags |= RT_SCENE_INPUT_MATERIAL_DYNAMIC_CHANNEL_RESERVED;
    }

    sceneInputs.lights.emissiveTriangleBuffer = smokeEmissiveTriangleBuffer;
    sceneInputs.lights.previousEmissiveTriangleBuffer = smokePreviousEmissiveTriangleBuffer;
    sceneInputs.lights.emissiveRemapBuffer = smokeEmissiveRemapBuffer;
    sceneInputs.lights.emissiveDistributionBuffer = smokeEmissiveDistributionBuffer;
    sceneInputs.lights.lightCandidateBuffer = smokeLightCandidateBuffer;
    sceneInputs.lights.doomAnalyticLightBuffer = smokeDoomAnalyticLightBuffer;
    sceneInputs.lights.doomAnalyticPreviousLightBuffer = smokeDoomAnalyticPreviousLightBuffer;
    sceneInputs.lights.doomAnalyticCurrentIdentityBuffer = smokeDoomAnalyticCurrentIdentityBuffer;
    sceneInputs.lights.doomAnalyticPreviousIdentityBuffer = smokeDoomAnalyticPreviousIdentityBuffer;
    sceneInputs.lights.doomAnalyticRemapBuffer = smokeDoomAnalyticRemapBuffer;
    sceneInputs.lights.unifiedLightBuffer = smokeUnifiedLightBuffer;
    sceneInputs.lights.unifiedPreviousLightBuffer = smokeUnifiedPreviousLightBuffer;
    sceneInputs.lights.unifiedLightRemapBuffer = smokeUnifiedLightRemapBuffer;
    sceneInputs.lights.restirLightManagerCurrentPayloadBuffer = smokeRestirLightManagerCurrentPayloadBuffer;
    sceneInputs.lights.restirLightManagerPreviousPayloadBuffer = smokeRestirLightManagerPreviousPayloadBuffer;
    sceneInputs.lights.emissiveTriangleCount = emissiveInventoryStats.capturedTriangles;
    sceneInputs.lights.emissiveDistributionCount = static_cast<int>(emissiveDistribution.entries.size());
    sceneInputs.lights.emissiveDistributionZeroPdfSkipped = emissiveDistribution.zeroPdfSkipped;
    sceneInputs.lights.emissiveDistributionFallbackIndex = emissiveDistribution.fallbackIndex == UINT32_MAX ? -1 : static_cast<int>(emissiveDistribution.fallbackIndex);
    sceneInputs.lights.emissiveStaticTriangleCount = emissiveInventoryStats.staticTriangles;
    sceneInputs.lights.emissiveDynamicTriangleCount = emissiveInventoryStats.dynamicTriangles;
    sceneInputs.lights.lightCandidateCount = emissiveInventoryStats.candidateMaterials;
    sceneInputs.lights.texturedLightCandidateCount = emissiveInventoryStats.texturedCandidateMaterials;
    sceneInputs.lights.doomAnalyticLightCount = static_cast<int>(doomAnalyticLights.size());
    sceneInputs.lights.doomAnalyticPreviousLightCount = static_cast<int>(doomAnalyticRemap.previousCandidates.size());
    sceneInputs.lights.doomAnalyticCurrentIdentityCount = static_cast<int>(doomAnalyticRemap.currentCandidateIdentities.size());
    sceneInputs.lights.doomAnalyticPreviousIdentityCount = static_cast<int>(doomAnalyticRemap.previousCandidateIdentities.size());
    sceneInputs.lights.doomAnalyticRemapCount = static_cast<int>(doomAnalyticRemap.universeRemap.size());
    sceneInputs.lights.doomAnalyticInvalidRemapCount = doomAnalyticRemap.invalidRemapCount;
    sceneInputs.lights.previousEmissiveTriangleCount = static_cast<int>(previousEmissiveTriangles.size());
    sceneInputs.lights.unifiedLightCount = static_cast<int>(unifiedLights.currentLights.size());
    sceneInputs.lights.unifiedPreviousLightCount = static_cast<int>(unifiedLights.previousLights.size());
    sceneInputs.lights.unifiedLightRemapCount = static_cast<int>(unifiedLights.currentToPreviousRemap.size());
    sceneInputs.lights.restirLightManagerCurrentPayloadCount = static_cast<int>(restirLightManagerCurrentPayloadRecords.size());
    sceneInputs.lights.restirLightManagerPreviousPayloadCount = static_cast<int>(restirLightManagerPreviousPayloadRecords.size());
    sceneInputs.lights.emissiveDistributionTotalPdf = emissiveDistribution.totalPdf;
    sceneInputs.lights.emissiveDistributionFallbackWeight = emissiveDistribution.fallbackWeight;
    sceneInputs.lights.emissiveDistributionValid = emissiveDistribution.valid;
    sceneInputs.lights.capabilityFlags = RT_SCENE_INPUT_LIGHT_PREVIOUS_IDENTITY_RESERVED;
    sceneInputs.diagnostics.geometryUploadBytes = staticUploadBytes + previousStaticUploadBytes + dynamicUploadBytes + rigidRouteUploadBytes;
    sceneInputs.diagnostics.staticUploadBytes = staticUploadBytes;
    sceneInputs.diagnostics.previousStaticUploadBytes = previousStaticUploadBytes;
    sceneInputs.diagnostics.previousStaticUploadSkippedBytes = previousStaticUploadSkippedBytes;
    sceneInputs.diagnostics.dynamicUploadBytes = dynamicUploadBytes;
    sceneInputs.diagnostics.rigidRouteUploadBytes = rigidRouteUploadBytes;
    sceneInputs.diagnostics.materialUploadBytes = materialUploadBytes;
    sceneInputs.diagnostics.lightUploadBytes = lightUploadBytes;
    sceneInputs.diagnostics.sceneBuildMs = Sys_Milliseconds() - sceneStartMs;
    sceneInputs.diagnostics.captureMs = captureMs;
    sceneInputs.diagnostics.materialMs = materialMs;
    sceneInputs.diagnostics.emissiveMs = emissiveMs;
    sceneInputs.diagnostics.bufferCreateMs = bufferCreateMs;
    sceneInputs.diagnostics.bufferUploadMs = bufferUploadMs;
    sceneInputs.diagnostics.accelSubmitMs = accelSubmitMs;
    }

    RtSmokeSceneResourceCommitBuildDesc resourceCommitBuildDesc;
    resourceCommitBuildDesc.sceneInputs = sceneInputs;
    resourceCommitBuildDesc.buffers = smokeBuffers;
    resourceCommitBuildDesc.staticBlasDesc = smokeStaticBlasDesc;
    resourceCommitBuildDesc.staticBlas = smokeStaticBlas;
    resourceCommitBuildDesc.dynamicBlas = smokeDynamicBlas;
    resourceCommitBuildDesc.tlas = m_smokeTlas;
    resourceCommitBuildDesc.hasStaticBlas = hasStaticBlas;
    resourceCommitBuildDesc.staticBlasSignature = staticSignature.hash;
    resourceCommitBuildDesc.staticBlasGeometryGeneration =
        staticBlasCacheHit
            ? cachedStaticBlasGeometryGeneration
            : geometryUniverseStats.staticGeometryGeneration;
    resourceCommitBuildDesc.skinnedOutputStorageGeneration =
        bufferCreateDesc.skinnedOutputStorageGeneration;
    resourceCommitBuildDesc.bindingSet = bindingBuildResult.bindingSet;
    resourceCommitBuildDesc.textureDescriptorTable = bindingBuildResult.textureDescriptorTable;
    resourceCommitBuildDesc.activeTextureTable = &bindingBuildResult.activeTextureTable;
    resourceCommitBuildDesc.skyEnvironmentCube = skyEnvironmentCube;
    resourceCommitBuildDesc.skyCubeProbeBindingSet = skyCubeProbeBindingSet;
    resourceCommitBuildDesc.textureDescriptorTableCreated = bindingBuildResult.textureDescriptorTableCreated;
    resourceCommitBuildDesc.textureDescriptorTableWritten = bindingBuildResult.textureDescriptorTableWritten;
    resourceCommitBuildDesc.materialTableEntryCount = static_cast<int>(materialTable.materials.size());
    resourceCommitBuildDesc.emissiveTriangleCount = emissiveInventoryStats.capturedTriangles;
    resourceCommitBuildDesc.emissiveStaticTriangleCount = emissiveInventoryStats.staticTriangles;
    resourceCommitBuildDesc.lightCandidateCount = emissiveInventoryStats.candidateMaterials;
    resourceCommitBuildDesc.doomAnalyticLightCount = static_cast<int>(doomAnalyticLights.size());
    resourceCommitBuildDesc.doomAnalyticPortalRegionLightCount = doomAnalyticPortalRegionLightCount;
    resourceCommitBuildDesc.doomAnalyticPreviousLightCount = static_cast<int>(doomAnalyticRemap.previousCandidates.size());
    resourceCommitBuildDesc.doomAnalyticCurrentIdentityCount = static_cast<int>(doomAnalyticRemap.currentCandidateIdentities.size());
    resourceCommitBuildDesc.doomAnalyticPreviousIdentityCount = static_cast<int>(doomAnalyticRemap.previousCandidateIdentities.size());
    resourceCommitBuildDesc.doomAnalyticRemapCount = static_cast<int>(doomAnalyticRemap.universeRemap.size());
    resourceCommitBuildDesc.previousEmissiveTriangleCount = sceneInputs.lights.previousEmissiveTriangleCount;
    resourceCommitBuildDesc.unifiedLightCount = sceneInputs.lights.unifiedLightCount;
    resourceCommitBuildDesc.unifiedPreviousLightCount = sceneInputs.lights.unifiedPreviousLightCount;
    resourceCommitBuildDesc.unifiedLightRemapCount = sceneInputs.lights.unifiedLightRemapCount;
    resourceCommitBuildDesc.restirLightManagerCurrentPayloadCount = sceneInputs.lights.restirLightManagerCurrentPayloadCount;
    resourceCommitBuildDesc.restirLightManagerPreviousPayloadCount = sceneInputs.lights.restirLightManagerPreviousPayloadCount;
    RtSmokeSceneResourceCommitDesc resourceCommitDesc;
    {
        OPTICK_EVENT("PT Commit Scene Desc Build");
        resourceCommitDesc = CreateSmokeSceneResourceCommitDesc(resourceCommitBuildDesc);
    }
    {
        OPTICK_EVENT("PT Commit Scene Resources");
        CommitRayTracingSmokeSceneResources(resourceCommitDesc);
    }
    const RtSmokeSkinnedBlasShadowAudit skinnedBlasShadowAudit =
        UpdateSmokeSkinnedBlasShadowState(
            viewDef,
            currentSkinnedSurfaceRecords,
            skinnedGpuScaffold,
            m_smokeGeometryUniverse,
            m_smokeSkinnedOutputAllocator,
            m_smokeSkinnedBlasStateTable,
            m_smokeSkinnedCurrentOutputVertexBuffer,
            m_smokeSkinnedOutputBufferGeneration,
            skinnedOutputBufferChanged,
            canonicalSkinnedSourceOutputRoute,
            geometryUniverseStats.frameIndex);
    const RtSmokeSkinnedComparisonRetirementAudit
        skinnedComparisonRetirementAudit =
            SynchronizeSmokeSkinnedComparisonBlasRetirements(
                m_smokeSkinnedBlasStateTable,
                m_smokeSkinnedComparisonBlases,
                m_retiredSmokeSkinnedComparisonBlases,
                device,
                geometryUniverseStats.frameIndex,
                idMath::ClampInt(
                    0,
                    32,
                    r_pathTracingSceneRetireFrames.
                        GetInteger()),
                m_smokeSkinnedComparisonCompletionQueryFailureLogged);
    if (skinnedComparisonRetirementAudit.activeRetired > 0 ||
        skinnedComparisonRetirementAudit.statePackagesQueued > 0)
    {
        common->Printf(
            "PathTracePrimaryPass: GEO08 skinned comparison retirement queued frame=%llu activeRetired=%d statePackages=%d emptyStatePackages=%d active=%zu pending=%d authority=gpu-event-query tlas=excluded\n",
            static_cast<unsigned long long>(
                geometryUniverseStats.frameIndex),
            skinnedComparisonRetirementAudit.activeRetired,
            skinnedComparisonRetirementAudit.statePackagesQueued,
            skinnedComparisonRetirementAudit.emptyStatePackagesQueued,
            m_smokeSkinnedComparisonBlases.size(),
            skinnedComparisonRetirementAudit.pendingPackages);
    }
    if (skinnedGpuParitySentinelRequested)
    {
        DumpSmokeSkinnedBlasShadowAudit(
            skinnedBlasShadowAudit,
            geometryUniverseStats.frameIndex);
    }
    {
        OPTICK_EVENT("PT Post Commit State Cache");
        if (asyncRigidRouteSideBufferRing && rigidRouteSideBufferWriteSlot >= 0)
        {
            RtSmokeRigidRouteSideBufferSlot& sideSlot =
                m_smokeRigidRouteSideBufferSlots[rigidRouteSideBufferWriteSlot];
            sideSlot.geometryUploadSignature = rigidRouteGeometryUploadSignature;
            sideSlot.instanceUploadSignature = rigidRouteInstanceUploadSignature;
            sideSlot.geometryUploadSignatureValid = true;
            sideSlot.instanceUploadSignatureValid = true;
            m_smokeRigidRouteSideBufferReadSlot = rigidRouteSideBufferWriteSlot;
        }
        m_smokePreviousStaticTriangleMaterialIndexes = materialTable.staticMaterialIndexes;
        m_smokeMaterialTableMaterials = gpuMaterialTableMaterials;
        m_smokeDynamicMaterialRecords = dynamicMaterialRecords;
        m_smokePreviousEmissiveTriangles = emissiveTriangles;
        m_smokePreviousStaticSnapshotUploadSignature = previousStaticSnapshotUploadSignature;
        m_smokePreviousStaticMaterialIndexUploadSignature = previousStaticMaterialIndexUploadSignature;
        m_smokeStaticTriangleMaterialUploadSignature = staticTriangleMaterialUploadSignature;
        m_smokeStaticTriangleMaterialIndexUploadSignature = staticTriangleMaterialIndexUploadSignature;
        m_smokeMaterialTableUploadSignature = materialTableUploadSignature;
        m_smokeDynamicMaterialUploadSignature = dynamicMaterialUploadSignature;
        m_smokeStaticTriangleMaterialUploadSignatureValid =
            SmokeBufferHasPayloadCapacity(
                smokeStaticTriangleMaterialBuffer,
                bufferCreateDesc.staticTriangleMaterialBytes,
                sizeof(uint32_t));
        m_smokeStaticTriangleMaterialIndexUploadSignatureValid =
            SmokeBufferHasPayloadCapacity(
                smokeStaticTriangleMaterialIndexBuffer,
                bufferCreateDesc.staticTriangleMaterialIndexBytes,
                sizeof(uint32_t));
        m_smokeMaterialTableUploadSignatureValid =
            SmokeBufferHasPayloadCapacity(
                smokeMaterialTableBuffer,
                bufferCreateDesc.materialTableBytes,
                sizeof(PathTraceSmokeMaterial));
        m_smokeDynamicMaterialUploadSignatureValid =
            SmokeBufferHasPayloadCapacity(
                smokeDynamicMaterialBuffer,
                bufferCreateDesc.dynamicMaterialBytes,
                sizeof(PathTraceDynamicMaterialRecord));
    }

    const int sceneMs = Sys_Milliseconds() - sceneStartMs;
    RtSmokeSceneBuildDiagnosticLogDesc sceneLogDesc;
    sceneLogDesc.sceneMs = sceneMs;
    sceneLogDesc.captureMs = captureMs;
    sceneLogDesc.metadataMs = metadataMs;
    sceneLogDesc.metadataValidationMs = metadataValidationMs;
    sceneLogDesc.metadataRegistrationMs = metadataRegistrationMs;
    sceneLogDesc.materialMs = materialMs;
    sceneLogDesc.emissiveMs = emissiveMs;
    sceneLogDesc.bufferCreateMs = bufferCreateMs;
    sceneLogDesc.bufferUploadMs = bufferUploadMs;
    sceneLogDesc.accelSubmitMs = accelSubmitMs;
    sceneLogDesc.blasSubmitMs = blasSubmitMs;
    sceneLogDesc.tlasSubmitMs = tlasSubmitMs;
    sceneLogDesc.sourceSurfaces = sourceSurfaces;
    sceneLogDesc.sourceVerts = sourceVerts;
    sceneLogDesc.sourceIndexes = sourceIndexes;
    sceneLogDesc.anchorTriangle = anchorTriangle;
    sceneLogDesc.staticIndexCount = staticIndexCount;
    sceneLogDesc.staticVertexCount = staticVertexCount;
    sceneLogDesc.dynamicIndexCount = dynamicIndexCount;
    sceneLogDesc.dynamicVertexCount = dynamicVertexCount;
    sceneLogDesc.instanceCount = instanceCount;
    sceneLogDesc.rigidTlasInstanceCount = static_cast<int>(rigidTlasRouteInstances.size());
    sceneLogDesc.rigidRouteInstanceCount = rigidRouteBuild.stats.emittedInstances;
    sceneLogDesc.rigidRouteUniqueMeshes = rigidRouteBuild.stats.emittedUniqueMeshes;
    sceneLogDesc.rigidRouteVertexCount = rigidRouteBuild.stats.vertices;
    sceneLogDesc.rigidRouteIndexCount = rigidRouteBuild.stats.indexes;
    sceneLogDesc.rigidRouteTriangleCount = rigidRouteBuild.stats.triangles;
    {
        OPTICK_EVENT("PT BVH Frame Planning");
    const bool staticBucketAuditRequested =
        staticBucketFramePublication.auditRequested;
    const bool staticBucketAuditReady =
        staticBucketFramePublication.auditReady;
    const bool staticBucketBlasEnabled =
        staticBucketFramePublication.blasEnabled;
    if (staticBucketFramePublication.enabled)
    {
        const RtPathTraceSceneUniverseBuildStats&
            staticBucketSourceBuildStats =
                staticBucketFramePublication.sourceBuildStats;
        const bool staticBucketActiveMaskValid =
            staticBucketFramePublication.activeMaskValid;
        const int portalAreaCount =
            staticBucketFramePublication.portalAreaCount;
        const int maxVerticesPerBucket =
            staticBucketFramePublication.maxVerticesPerBucket;
        const int maxIndexesPerBucket =
            staticBucketFramePublication.maxIndexesPerBucket;
        const int maxTrianglesPerBucket =
            staticBucketFramePublication.maxTrianglesPerBucket;
        const uint64 staticBucketSourceGeneration =
            staticBucketFramePublication.sourceGeneration;
        const RtSmokeGeometryUniverseStats&
            staticBucketUniverseStats =
                staticBucketFramePublication.universeStats;
        const RtSmokeStaticBucketAssignmentPlan& assignmentPlan =
            staticBucketFramePublication.assignmentPlan;
        const RtSmokeStaticBucketAssignmentStats& assignmentStats =
            assignmentPlan.stats;
        const RtSmokeStaticBucketGeometryPack& geometryPack =
            *staticBucketFramePublication.geometryPack;
        const RtSmokeStaticBucketGeometryPackStats& packStats =
            geometryPack.stats;
        const int staticBucketMissingActiveMaterialIndexes =
            staticBucketFramePublication.
                missingActiveMaterialIndexes;
        const RtPathTraceStaticBucketBlasGpuStats&
            staticBucketGpuStats =
                staticBucketFramePublication.gpuStats;
        const RtSmokeStaticBucketWorkPlan&
            staticBucketShadowWorkPlan =
                staticBucketFramePublication.shadowWorkPlan;
        const RtPathTraceStaticBucketActivePublication&
            staticBucketActivePublication =
                staticBucketFramePublication.activePublication;
        const bool staticBucketMaterialIndexUploaded =
            staticBucketFramePublication.materialIndexUploaded;
        if (staticBucketAuditRequested ||
            staticBucketGpuStats.buffersCreated > 0 ||
            staticBucketGpuStats.bufferUploads > 0 ||
            staticBucketGpuStats.blasCreated > 0 ||
            staticBucketGpuStats.blasBuilt > 0 ||
            staticBucketGpuStats.blasRetired > 0)
        {
            m_staticBucketGeometryUniverse.
                DumpStaticBucketBlasGpuStats(
                    staticBucketGpuStats);
            m_staticBucketGeometryUniverse.
                DumpStaticBucketActivePublication(
                    staticBucketActivePublication);
            common->Printf(
                "PathTracePrimaryPass: GEO10 static bucket active-set sourceBuilt/cacheHit=%d/%d residentPack/materialIndexCacheHit=%d/%d maskValid=%d portalMaskValid=%d fullResidentProbe=%d portalSteps=%d reflectionHalo=%d areas(frontendVisible/selected)=%d/%d buckets(resident/active/inactive/ready/emitted)=%d/%d/%d/%d/%d triangles(resident/active)=%d/%d signatures(plan/active/resident/materialBinding/tlas)=%llu/%llu/%llu/%llu/%llu routes(shaderSupport/blocked/gpuUpload)=%d/%d/%d materialIndexMissingActive=%d epochs(source/storage/material)=%llu/%llu/%llu traversal=shadow-only\n",
                staticBucketSourceBuildStats.built ? 1 : 0,
                staticBucketSourceBuildStats.cacheHit ? 1 : 0,
                staticBucketFramePublication.
                    residentPackCacheHit
                        ? 1
                        : 0,
                staticBucketFramePublication.
                    materialIndexCacheHit
                        ? 1
                        : 0,
                staticBucketActiveMaskValid ? 1 : 0,
                staticBucketFramePublication.portalMaskValid ? 1 : 0,
                staticBucketFramePublication.
                    activeMaskForcedFullResident
                        ? 1
                        : 0,
                staticBucketFramePublication.portalSteps,
                staticBucketFramePublication.
                    reflectionPortalHalo
                        ? 1
                        : 0,
                staticBucketFramePublication.
                    frontendVisibleAreaCount,
                staticBucketFramePublication.
                    selectedPortalAreaCount,
                staticBucketShadowWorkPlan.activeSetPlan.
                    residentBuckets,
                staticBucketShadowWorkPlan.activeSetPlan.
                    activeBuckets,
                staticBucketShadowWorkPlan.activeSetPlan.
                    inactiveResidentBuckets,
                staticBucketGpuStats.readyBuckets,
                staticBucketShadowWorkPlan.activeSetPlan.
                    emittedInstances,
                staticBucketShadowWorkPlan.activeSetPlan.
                    residentTriangleCount,
                staticBucketShadowWorkPlan.activeSetPlan.
                    activeTriangleCount,
                static_cast<unsigned long long>(
                    staticBucketShadowWorkPlan.planSignature),
                static_cast<unsigned long long>(
                    staticBucketShadowWorkPlan.activeSetPlan.
                        activeSetSignature),
                static_cast<unsigned long long>(
                    staticBucketShadowWorkPlan.activeSetPlan.
                        residentSetSignature),
                static_cast<unsigned long long>(
                    staticBucketFramePublication.
                        materialBindingSignature),
                static_cast<unsigned long long>(
                    staticBucketShadowWorkPlan.activeSetPlan.
                        tlasInstanceSignature),
                0,
                staticBucketShadowWorkPlan.routeNamespace.
                    staticRoutesBlocked
                        ? 1
                        : 0,
                staticBucketMaterialIndexUploaded ? 1 : 0,
                staticBucketMissingActiveMaterialIndexes,
                static_cast<unsigned long long>(
                    staticBucketSourceGeneration),
                static_cast<unsigned long long>(
                    staticBucketUniverseStats.
                        staticGeometryGeneration),
                static_cast<unsigned long long>(
                    staticBucketUniverseStats.
                        staticMaterialGeneration));
        }
        if (staticBucketAuditReady)
        {
        common->Printf(
            "PathTracePrimaryPass: GEO10 static bucket assignment exact=%d signature=%llu generations(world/source/storage)=%llu/%llu/%llu limits(v/i/t)=%d/%d/%d areas=%d surfaces(input/assigned/duplicate/unassigned/invalidArea/invalidRange/oversized)=%d/%d/%d/%d/%d/%d/%d primitives(assigned/retained)=%d/%d buckets(total/active/fallback/split/keyCollision)=%d/%d/%d/%d/%d route=shadow-only\n",
            assignmentPlan.exactCoverage ? 1 : 0,
            static_cast<unsigned long long>(
                assignmentPlan.planSignature),
            static_cast<unsigned long long>(
                static_cast<uint64>(m_smokeSceneMapTimeStamp)),
            static_cast<unsigned long long>(
                staticBucketSourceGeneration),
            static_cast<unsigned long long>(
                staticBucketUniverseStats.staticGeometryGeneration),
            maxVerticesPerBucket,
            maxIndexesPerBucket,
            maxTrianglesPerBucket,
            portalAreaCount,
            assignmentStats.inputSurfaces,
            assignmentStats.assignedSurfaces,
            assignmentStats.duplicateSurfaces,
            assignmentStats.unassignedAreaSurfaces,
            assignmentStats.invalidAreaSurfaces,
            assignmentStats.invalidRangeSurfaces,
            assignmentStats.oversizedSurfaces,
            assignmentStats.assignedPrimitives,
            staticBucketUniverseStats.staticTriangles,
            assignmentStats.buckets,
            assignmentStats.activeBuckets,
            assignmentStats.fallbackBuckets,
            assignmentStats.splitBuckets,
            assignmentStats.bucketKeyCollisions);
        common->Printf(
            "PathTracePrimaryPass: GEO10 static bucket geometry exact=%d cacheHit=%d signature=%llu buckets(input/packed)=%d/%d surfaces(input/packed)=%d/%d geometry(sourceV/sourceI/sourceT/packedV/packedI/packedT)=%d/%d/%d/%d/%d/%d bytes(vertex/index/classPrefix/t5/material/identity/surfaceRecords)=%llu/%llu/%llu/%llu/%llu/%llu/%llu failures(bucketRange/assignment/sourceRange/indexRange/localPrimitiveOffset/address/surfaceRecord/surfaceAddress/classMetadata/count)=%d/%d/%d/%d/%d/%d/%d/%d/%d/%d route=shadow-only\n",
            geometryPack.exact ? 1 : 0,
            staticBucketFramePublication.
                residentPackCacheHit
                    ? 1
                    : 0,
            static_cast<unsigned long long>(
                geometryPack.contentSignature),
            packStats.inputBuckets,
            packStats.packedBuckets,
            packStats.inputAssignments,
            packStats.packedSurfaces,
            staticBucketUniverseStats.staticVerts,
            staticBucketUniverseStats.staticIndexes,
            staticBucketUniverseStats.staticTriangles,
            packStats.packedVertices,
            packStats.packedIndexes,
            packStats.packedTriangles,
            static_cast<unsigned long long>(
                geometryPack.vertexBytes.size()),
            static_cast<unsigned long long>(
                geometryPack.indexes.size() *
                sizeof(geometryPack.indexes[0])),
            static_cast<unsigned long long>(
                geometryPack.triangleClasses.size() *
                sizeof(geometryPack.triangleClasses[0])),
            static_cast<unsigned long long>(
                geometryPack.staticClassMetadataWords.size() *
                sizeof(
                    geometryPack.staticClassMetadataWords[0])),
            static_cast<unsigned long long>(
                geometryPack.triangleMaterials.size() *
                sizeof(geometryPack.triangleMaterials[0])),
            static_cast<unsigned long long>(
                geometryPack.triangleIdentities.size() *
                sizeof(geometryPack.triangleIdentities[0])),
            static_cast<unsigned long long>(
                geometryPack.surfaceRecords.size() *
                sizeof(geometryPack.surfaceRecords[0])),
            packStats.invalidBucketRanges,
            packStats.invalidAssignments,
            packStats.sourceRangeMismatches,
            packStats.indexRangeErrors,
            packStats.localPrimitiveOffsetErrors,
            packStats.addressContractErrors,
            packStats.surfaceRecordErrors,
            packStats.surfaceAddressContractErrors,
            packStats.classMetadataLayoutErrors,
            packStats.countMismatches);
        const uint64 residentPackBytes =
            geometryPack.vertexBytes.size() +
            geometryPack.indexes.size() *
                sizeof(geometryPack.indexes[0]) +
            geometryPack.triangleClasses.size() *
                sizeof(geometryPack.triangleClasses[0]) +
            geometryPack.staticClassMetadataWords.size() *
                sizeof(geometryPack.staticClassMetadataWords[0]) +
            geometryPack.triangleMaterials.size() *
                sizeof(geometryPack.triangleMaterials[0]) +
            geometryPack.triangleIdentities.size() *
                sizeof(geometryPack.triangleIdentities[0]) +
            geometryPack.surfaceRecords.size() *
                sizeof(geometryPack.surfaceRecords[0]) +
            (staticBucketFramePublication.materialIndexes
                ? staticBucketFramePublication.materialIndexes->size() *
                    sizeof(
                        (*staticBucketFramePublication.materialIndexes)[0])
                : 0);
        const uint64 gpuInputBytes =
            staticBucketGpuStats.vertexBytes +
            staticBucketGpuStats.indexBytes +
            staticBucketGpuStats.metadataBytes +
            (staticBucketFramePublication.materialIndexes
                ? staticBucketFramePublication.materialIndexes->size() *
                    sizeof(
                        (*staticBucketFramePublication.materialIndexes)[0])
                : 0);
        common->Printf(
            "PathTracePrimaryPass: GEO10 step9 cache(source/assignment/residentPack/materialIndex/sourceFastPath)=%d/%d/%d/%d/%d timingsUs(total/withoutValidation/source/portal/universeStats/validation/assignment/residentPack/materialIndex/blasScaffold/publication/materialUpload)=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu retainedKB(source/previous/residentPack/gpuInputs)=%d/%d/%llu/%llu warmUploadBytes=%llu blas(build/reuse/retire)=%d/%d/%d\n",
            staticBucketSourceBuildStats.cacheHit ? 1 : 0,
            staticBucketFramePublication.assignmentPlanCacheHit ? 1 : 0,
            staticBucketFramePublication.residentPackCacheHit ? 1 : 0,
            staticBucketFramePublication.materialIndexCacheHit ? 1 : 0,
            staticBucketSourceBuildStats.frameFastPath ? 1 : 0,
            static_cast<unsigned long long>(
                staticBucketFramePublication.totalCpuMicroseconds),
            static_cast<unsigned long long>(
                staticBucketFramePublication.
                    cpuMicrosecondsWithoutValidation),
            static_cast<unsigned long long>(
                staticBucketFramePublication.sourceBuildMicroseconds),
            static_cast<unsigned long long>(
                staticBucketFramePublication.portalMaskMicroseconds),
            static_cast<unsigned long long>(
                staticBucketFramePublication.
                    universeStatsMicroseconds),
            static_cast<unsigned long long>(
                staticBucketFramePublication.
                    validationMicroseconds),
            static_cast<unsigned long long>(
                staticBucketFramePublication.assignmentMicroseconds),
            static_cast<unsigned long long>(
                staticBucketFramePublication.residentPackMicroseconds),
            static_cast<unsigned long long>(
                staticBucketFramePublication.materialIndexMicroseconds),
            static_cast<unsigned long long>(
                staticBucketFramePublication.blasScaffoldMicroseconds),
            static_cast<unsigned long long>(
                staticBucketFramePublication.publicationMicroseconds),
            static_cast<unsigned long long>(
                staticBucketFramePublication.materialUploadMicroseconds),
            staticBucketUniverseStats.staticBytesKB,
            staticBucketUniverseStats.previousStaticBytesKB,
            static_cast<unsigned long long>(
                (residentPackBytes + 1023ull) / 1024ull),
            static_cast<unsigned long long>(
                (gpuInputBytes + 1023ull) / 1024ull),
            static_cast<unsigned long long>(
                staticBucketGpuStats.uploadBytes),
            staticBucketGpuStats.blasBuilt,
            staticBucketGpuStats.blasReused,
            staticBucketGpuStats.blasRetired);
        const size_t bucketSampleCount =
            std::min(
                assignmentPlan.buckets.size(),
                static_cast<size_t>(16));
        for (size_t bucketIndex = 0;
            bucketIndex < bucketSampleCount;
            ++bucketIndex)
        {
            const RtSmokeStaticBucketAssignmentBucket& bucket =
                assignmentPlan.buckets[bucketIndex];
            const RtSmokeStaticBucketPackedRecord* packedBucket =
                bucketIndex < geometryPack.buckets.size()
                    ? &geometryPack.buckets[bucketIndex]
                    : nullptr;
            common->Printf(
                "PathTracePrimaryPass: GEO10 static bucket sample index=%llu key=%llu area/split=%d/%u assignments=%u rangeCounts(v/i/t)=%d/%d/%d packedOffsets(v/i/t)=%d/%d/%d packedBytes(v/i/t)=%llu/%llu/%llu active/oversized=%d/%d\n",
                static_cast<unsigned long long>(bucketIndex),
                static_cast<unsigned long long>(bucket.bucketKey),
                bucket.portalArea,
                bucket.splitIndex,
                bucket.assignmentCount,
                bucket.vertexCount,
                bucket.indexCount,
                bucket.triangleCount,
                packedBucket
                    ? packedBucket->range.vertexOffset
                    : -1,
                packedBucket
                    ? packedBucket->range.indexOffset
                    : -1,
                packedBucket
                    ? packedBucket->range.triangleOffset
                    : -1,
                static_cast<unsigned long long>(
                    packedBucket
                        ? packedBucket->vertexByteSize
                        : 0),
                static_cast<unsigned long long>(
                    packedBucket
                        ? packedBucket->indexByteSize
                        : 0),
                static_cast<unsigned long long>(
                    packedBucket
                        ? packedBucket->
                            triangleMetadataByteSize
                        : 0),
                bucket.active ? 1 : 0,
                bucket.oversized ? 1 : 0);
        }
        r_pathTracingGeometryStaticBucketAudit.SetInteger(0);
        }
    }
    std::vector<RtSmokeStaticTlasBucketObservation> staticActiveBuckets;
    m_smokeGeometryUniverse.BuildStaticTlasBucketObservations(
        staticActiveBuckets,
        staticBlasCacheHit || smokeStaticBlas != nullptr,
        RT_SMOKE_STATIC_ACTIVE_SELECTED_AREA);
    if (staticActiveBuckets.empty())
    {
        RtSmokeStaticTlasBucketObservation staticActiveBucket;
        staticActiveBucket.bucketKey = staticSignature.hash;
        staticActiveBucket.resident = hasStaticBlas;
        staticActiveBucket.active = hasStaticBlas && staticIndexCount > 0;
        staticActiveBucket.hasBlas = staticBlasCacheHit || smokeStaticBlas != nullptr;
        staticActiveBucket.activeReasonFlags = RT_SMOKE_STATIC_ACTIVE_SELECTED_AREA;
        staticActiveBucket.residentSurfaceCount = geometryUniverseStats.staticSurfaces;
        staticActiveBucket.residentVertexCount = staticVertexCacheCount;
        staticActiveBucket.residentIndexCount = staticIndexCacheCount;
        staticActiveBucket.residentTriangleCount = staticTriangleCacheCount;
        staticActiveBucket.activeSurfaceCount = classStats.staticWorldSurfaces;
        staticActiveBucket.activeVertexCount = staticVertexCount;
        staticActiveBucket.activeIndexCount = staticIndexCount;
        staticActiveBucket.activeTriangleCount = staticIndexCount / 3;
        staticActiveBuckets.push_back(staticActiveBucket);
    }
    const int rigidTlasInstanceCount = static_cast<int>(rigidTlasRouteInstances.size());
    RtSmokeStaticBucketWorkPlanInput staticBucketWorkInput;
    staticBucketWorkInput.buckets = staticActiveBuckets.empty() ? nullptr : staticActiveBuckets.data();
    staticBucketWorkInput.bucketCount = static_cast<int>(staticActiveBuckets.size());
    staticBucketWorkInput.geometryContentSignature = geometryUniverseStats.generation;
    staticBucketWorkInput.materialGeneration = materialTableSignature;
    staticBucketWorkInput.totalVertexCount = staticVertexCacheCount;
    staticBucketWorkInput.totalIndexCount = staticIndexCacheCount;
    staticBucketWorkInput.totalTriangleCount = staticTriangleCacheCount;
    staticBucketWorkInput.monolithicStaticBlas = true;
    staticBucketWorkInput.hasStaticBlas = hasStaticBlas;
    staticBucketWorkInput.enableStaticRoutes = true;
    staticBucketWorkInput.shaderSupportsStaticBucketRoutes = false;
    staticBucketWorkInput.rigidRouteRecordCount = rigidTlasInstanceCount;
    RtSmokeBvhFramePlanningInput bvhFramePlanningInput;
    bvhFramePlanningInput.staticBucketWorkInput = staticBucketWorkInput;
    bvhFramePlanningInput.previousDirtyTokenValid = m_smokeBvhDirtyPreviousTokenValid;
    bvhFramePlanningInput.previousDirtyToken = m_smokeBvhDirtyPreviousToken;
    bvhFramePlanningInput.frameTokenInput.staticBlasSignature = staticSignature.hash;
    bvhFramePlanningInput.frameTokenInput.geometryGeneration = geometryUniverseStats.generation;
    bvhFramePlanningInput.frameTokenInput.materialGeneration = materialTableSignature;
    bvhFramePlanningInput.frameTokenInput.dynamicVertexCount = dynamicVertexCount;
    bvhFramePlanningInput.frameTokenInput.dynamicIndexCount = dynamicIndexCount;
    bvhFramePlanningInput.frameTokenInput.rigidRouteVertexCount = rigidRouteBuild.stats.vertices;
    bvhFramePlanningInput.frameTokenInput.rigidRouteIndexCount = rigidRouteBuild.stats.indexes;
    bvhFramePlanningInput.frameTokenInput.rigidRouteTriangleCount = rigidRouteBuild.stats.triangles;
    bvhFramePlanningInput.frameTokenInput.rigidRouteInstanceCount = rigidRouteBuild.stats.emittedInstances;
    bvhFramePlanningInput.frameTokenInput.rigidRouteSeenThisFrameCount = rigidRouteBuild.stats.emittedSeenThisFrame;
    bvhFramePlanningInput.frameTokenInput.rigidRouteCachedInstanceCount = rigidRouteBuild.stats.emittedFromCache;
    bvhFramePlanningInput.frameTokenInput.rigidTlasInstanceSignature =
        rigidTlasPlanValid ? rigidTlasPlan.tlasInstanceSignature : 0;
    bvhFramePlanningInput.frameTokenInput.baseTlasInstanceCount = instanceCount - rigidTlasInstanceCount;
    bvhFramePlanningInput.frameTokenInput.rigidTlasInstanceCount = rigidTlasInstanceCount;
    bvhFramePlanningInput.frameTokenInput.hasStaticBlas = hasStaticBlas;
    bvhFramePlanningInput.frameTokenInput.hasDynamicBlas = hasDynamicBlas;
    const int bvhFramePlanStartMs = Sys_Milliseconds();
    const int bvhFrameSnapshotStartMs = Sys_Milliseconds();
    const RtSmokeBvhFramePlanningSnapshot bvhFramePlanningSnapshot =
        CaptureSmokeBvhFramePlanningSnapshot(bvhFramePlanningInput);
    const int bvhFrameSnapshotMs = Sys_Milliseconds() - bvhFrameSnapshotStartMs;
    RtPathTraceCpuWorkGeneration bvhFramePlanningGeneration;
    bvhFramePlanningGeneration.frameIndex = 0;
    bvhFramePlanningGeneration.sceneGeneration = m_smokeSceneUniverseStaticBuildGeneration;
    bvhFramePlanningGeneration.geometryGeneration = geometryUniverseStats.generation;
    bvhFramePlanningGeneration.materialGeneration = materialTableSignature;
    bvhFramePlanningGeneration.lightGeneration =
        BuildSmokeBvhFramePlanningInputToken(bvhFramePlanningSnapshot);
    RtPathTraceCpuWorkPublishSnapshot(m_smokeBvhFramePlanningCpuWorkState, bvhFramePlanningGeneration);

    RtSmokeBvhFramePlanningResult bvhFramePlanningResult;
    int bvhFramePlanMs = 0;
    bool bvhFramePlanningResultValid = false;
    bool bvhFramePlanningAcceptedGeneration = false;
    if (asyncBvhFramePlanning && m_smokeBvhFramePlanningFuture.valid())
    {
        const std::future_status futureStatus =
            m_smokeBvhFramePlanningFuture.wait_for(std::chrono::seconds(0));
        if (futureStatus == std::future_status::ready)
        {
            const RtSmokeBvhFramePlanningTimedResult timedResult =
                m_smokeBvhFramePlanningFuture.get();
            m_smokeBvhFramePlanningAsyncGenerationValid = false;

            RtPathTraceCpuWorkResultEnvelope asyncEnvelope;
            asyncEnvelope.completed = true;
            asyncEnvelope.generation = m_smokeBvhFramePlanningAsyncGeneration;
            asyncEnvelope.timing = m_smokeBvhFramePlanningAsyncTiming;
            asyncEnvelope.timing.workerExecutionMs =
                static_cast<double>(timedResult.planningTimeMicros) / 1000.0;
            const double asyncOutstandingMs =
                static_cast<double>(Max(0, Sys_Milliseconds() - m_smokeBvhFramePlanningAsyncLaunchMs));
            asyncEnvelope.timing.queueWaitMs =
                Max(0.0, asyncOutstandingMs - asyncEnvelope.timing.workerExecutionMs);
            RtPathTraceCpuWorkPublishCompletedResult(m_smokeBvhFramePlanningCpuWorkState, asyncEnvelope);

            const RtPathTraceCpuWorkFrameDecision asyncDecision =
                RtPathTraceCpuWorkAcceptLatest(
                    m_smokeBvhFramePlanningCpuWorkState,
                    bvhFramePlanningGeneration,
                    &asyncEnvelope,
                    false);
            if (asyncDecision.accepted)
            {
                bvhFramePlanningResult = timedResult.result;
                bvhFramePlanMs = static_cast<int>(asyncEnvelope.timing.workerExecutionMs + 0.5);
                m_smokeBvhFramePlanningAsyncCachedResult = timedResult.result;
                m_smokeBvhFramePlanningAsyncCachedGeneration =
                    m_smokeBvhFramePlanningAsyncGeneration;
                m_smokeBvhFramePlanningAsyncCachedResultValid = true;
                bvhFramePlanningResultValid = true;
                bvhFramePlanningAcceptedGeneration = true;
            }
        }
        else
        {
            RtPathTraceCpuWorkAcceptLatest(
                m_smokeBvhFramePlanningCpuWorkState,
                bvhFramePlanningGeneration,
                nullptr,
                false);
        }
    }
    if (!bvhFramePlanningResultValid &&
        asyncBvhFramePlanning &&
        m_smokeBvhFramePlanningAsyncCachedResultValid &&
        RtPathTraceCpuWorkGenerationEquals(
            m_smokeBvhFramePlanningAsyncCachedGeneration,
            bvhFramePlanningGeneration))
    {
        bvhFramePlanningResult = m_smokeBvhFramePlanningAsyncCachedResult;
        bvhFramePlanningResultValid = true;
        bvhFramePlanningAcceptedGeneration = true;
    }

    if (!bvhFramePlanningResultValid)
    {
        const RtSmokeBvhFramePlanningTimedResult timedResult =
            BuildSmokeBvhFramePlanningTimedResult(bvhFramePlanningSnapshot);
        bvhFramePlanningResult = timedResult.result;
        bvhFramePlanMs = Sys_Milliseconds() - bvhFramePlanStartMs;
        bvhFramePlanningResultValid = true;

        RtPathTraceCpuWorkResultEnvelope bvhFrameEnvelope;
        bvhFrameEnvelope.completed = true;
        bvhFrameEnvelope.generation = bvhFramePlanningGeneration;
        bvhFrameEnvelope.timing.snapshotCaptureMs = static_cast<double>(bvhFrameSnapshotMs);
        bvhFrameEnvelope.timing.workerExecutionMs =
            static_cast<double>(timedResult.planningTimeMicros) / 1000.0;
        RtPathTraceCpuWorkPublishCompletedResult(
            m_smokeBvhFramePlanningCpuWorkState,
            bvhFrameEnvelope);
        RtPathTraceCpuWorkAcceptLatest(
            m_smokeBvhFramePlanningCpuWorkState,
            bvhFramePlanningGeneration,
            nullptr,
            true);
        bvhFramePlanningAcceptedGeneration = true;
    }

    const bool bvhFramePlanningAlreadyCached =
        m_smokeBvhFramePlanningAsyncCachedResultValid &&
        RtPathTraceCpuWorkGenerationEquals(
            m_smokeBvhFramePlanningAsyncCachedGeneration,
            bvhFramePlanningGeneration);
    const bool bvhFramePlanningAlreadyQueued =
        m_smokeBvhFramePlanningAsyncGenerationValid &&
        RtPathTraceCpuWorkGenerationEquals(
            m_smokeBvhFramePlanningAsyncGeneration,
            bvhFramePlanningGeneration);
    if (asyncBvhFramePlanning &&
        !m_smokeBvhFramePlanningFuture.valid() &&
        !bvhFramePlanningAlreadyCached &&
        !bvhFramePlanningAlreadyQueued)
    {
        m_smokeBvhFramePlanningAsyncTiming = RtPathTraceCpuWorkTiming();
        m_smokeBvhFramePlanningAsyncTiming.snapshotCaptureMs =
            static_cast<double>(bvhFrameSnapshotMs);
        m_smokeBvhFramePlanningAsyncGeneration = bvhFramePlanningGeneration;
        m_smokeBvhFramePlanningAsyncGenerationValid = true;
        m_smokeBvhFramePlanningAsyncLaunchMs = Sys_Milliseconds();
        m_smokeBvhFramePlanningFuture.Start(
            [bvhFramePlanningSnapshot]() {
                return BuildSmokeBvhFramePlanningTimedResult(bvhFramePlanningSnapshot);
            });
    }
    const RtSmokeStaticBucketWorkPlan& staticBucketWorkPlan =
        bvhFramePlanningResult.staticBucketWorkPlan;
    sceneLogDesc.staticBvhResidentBuckets = staticBucketWorkPlan.activeSetPlan.residentBuckets;
    sceneLogDesc.bvhFramePlanMs = bvhFramePlanMs;
    sceneLogDesc.staticBvhActiveBuckets = staticBucketWorkPlan.activeSetPlan.activeBuckets;
    sceneLogDesc.staticBvhInactiveResidentBuckets = staticBucketWorkPlan.activeSetPlan.inactiveResidentBuckets;
    sceneLogDesc.staticBvhEmittedInstances = staticBucketWorkPlan.activeSetPlan.emittedInstances;
    sceneLogDesc.staticMonolithicInactiveIncluded = staticBucketWorkPlan.activeSetPlan.inactiveResidentGeometryIncluded;
    sceneLogDesc.staticRequiresBucketedBlas = staticBucketWorkPlan.activeSetPlan.requiresBucketedStaticBlas;
    sceneLogDesc.staticBucketBlasRecords = staticBucketWorkPlan.bucketBlasPlan.emittedRecords;
    sceneLogDesc.staticBucketBlasSkippedInactive = staticBucketWorkPlan.bucketBlasPlan.skippedInactive;
    sceneLogDesc.staticBucketBlasSkippedInvalid = staticBucketWorkPlan.bucketBlasPlan.skippedInvalid;
    sceneLogDesc.staticBucketBlasOverflow = staticBucketWorkPlan.bucketBlasPlan.overflow;
    sceneLogDesc.staticBucketTraversalRouteRequired = staticBucketWorkPlan.traversalCompatibility.requiresShaderRouteMetadata;
    sceneLogDesc.staticBucketTraversalCurrentShaderCompatible = staticBucketWorkPlan.traversalCompatibility.currentStaticShaderCompatible;
    sceneLogDesc.staticBucketTraversalExactMonolithic = staticBucketWorkPlan.traversalCompatibility.exactMonolithicRecord;
    sceneLogDesc.staticBucketTraversalNonZeroOffsetRecords = staticBucketWorkPlan.traversalCompatibility.nonZeroOffsetRecords;
    sceneLogDesc.staticRouteNamespaceBlocked = staticBucketWorkPlan.routeNamespace.staticRoutesBlocked;
    sceneLogDesc.staticRouteNamespaceFirst = staticBucketWorkPlan.routeNamespace.staticFirstInstanceId;
    sceneLogDesc.rigidRouteNamespaceFirst = staticBucketWorkPlan.routeNamespace.rigidFirstInstanceId;
    sceneLogDesc.staticRouteNamespaceCount = staticBucketWorkPlan.routeNamespace.staticRouteInstanceCount;
    sceneLogDesc.rigidRouteNamespaceShifted = staticBucketWorkPlan.routeNamespace.rigidRouteBaseShifted;
    RtSmokeBvhDirtyPlanInput bvhDirtyInput;
    bvhDirtyInput.previousValid = m_smokeBvhDirtyPreviousTokenValid;
    bvhDirtyInput.previous = m_smokeBvhDirtyPreviousToken;
    bvhDirtyInput.current = bvhFramePlanningResult.frameToken.dirtyToken;
    bvhFramePlanningResult.dirtyPlan = BuildSmokeBvhDirtyPlan(bvhDirtyInput);

    const RtSmokeBvhFrameToken& bvhFrameToken = bvhFramePlanningResult.frameToken;
    const RtSmokeBvhDirtyPlan& bvhDirtyPlan = bvhFramePlanningResult.dirtyPlan;
    if (bvhFramePlanningAcceptedGeneration)
    {
        m_smokeBvhDirtyPreviousToken = bvhFrameToken.dirtyToken;
        m_smokeBvhDirtyPreviousTokenValid = true;
    }
    sceneLogDesc.bvhDirtyPreviousValid = bvhFramePlanningInput.previousDirtyTokenValid;
    sceneLogDesc.bvhGeometryContentChanged = bvhDirtyPlan.geometryContentChanged;
    sceneLogDesc.bvhActiveGeometryContentChanged = bvhDirtyPlan.activeGeometryContentChanged;
    sceneLogDesc.bvhResidentSetChanged = bvhDirtyPlan.residentSetChanged;
    sceneLogDesc.bvhMaterialChanged = bvhDirtyPlan.materialChanged;
    sceneLogDesc.bvhActiveMembershipChanged = bvhDirtyPlan.activeMembershipChanged;
    sceneLogDesc.bvhTlasInstanceChanged = bvhDirtyPlan.tlasInstanceChanged;
    sceneLogDesc.bvhBlasInputDirty = bvhDirtyPlan.blasInputDirty;
    sceneLogDesc.bvhTlasDirty = bvhDirtyPlan.tlasDirty;
    sceneLogDesc.bvhActiveSetSignature = bvhFrameToken.dirtyToken.activeSetSignature;
    sceneLogDesc.bvhResidentSetSignature = bvhFrameToken.residentSetSignature;
    sceneLogDesc.bvhGeometryContentSignature = bvhFrameToken.dirtyToken.geometryContentSignature;
    sceneLogDesc.bvhActiveBlasInputSignature = bvhFrameToken.dirtyToken.activeBlasInputSignature;
    sceneLogDesc.bvhTlasInstanceSignature = bvhFrameToken.dirtyToken.tlasInstanceSignature;
    }
    sceneLogDesc.requestedDebugMode = requestedDebugMode;
    sceneLogDesc.staticUploadBytes = staticUploadBytes;
    sceneLogDesc.previousStaticUploadBytes = previousStaticUploadBytes;
    sceneLogDesc.previousStaticUploadSkippedBytes = previousStaticUploadSkippedBytes;
    sceneLogDesc.dynamicUploadBytes = dynamicUploadBytes;
    sceneLogDesc.rigidRouteUploadBytes = rigidRouteUploadBytes;
    sceneLogDesc.rigidRouteGeometryBytes = rigidRouteGeometryBytes;
    sceneLogDesc.rigidRouteInstanceBytes = rigidRouteInstanceBytes;
    sceneLogDesc.rigidRouteBuildMs = rigidRouteBuildMs;
    sceneLogDesc.staticBlasBuildSubmitted = accelSubmitTiming.staticBlasBuildSubmitted;
    sceneLogDesc.staticBlasBuildSkipped = accelSubmitTiming.staticBlasBuildSkipped;
    sceneLogDesc.dynamicBlasBuildSubmitted = accelSubmitTiming.dynamicBlasBuildSubmitted;
    sceneLogDesc.dynamicBlasBuildSkipped = accelSubmitTiming.dynamicBlasBuildSkipped;
    sceneLogDesc.staticSurfaceCacheSize = geometryUniverseStats.staticSurfaces;
    sceneLogDesc.staticVertexCacheCount = staticVertexCacheCount;
    sceneLogDesc.staticIndexCacheCount = staticIndexCacheCount;
    sceneLogDesc.staticTriangleCacheCount = staticTriangleCacheCount;
    sceneLogDesc.staticSeenThisFrame = geometryUniverseStats.staticSeenThisFrame;
    sceneLogDesc.staticNewThisFrame = geometryUniverseStats.staticNewThisFrame;
    sceneLogDesc.staticDisappearedThisFrame = geometryUniverseStats.staticDisappearedThisFrame;
    sceneLogDesc.staticHistoryValid = geometryUniverseStats.staticHistoryValid;
    sceneLogDesc.staticPreviousRangeValid = geometryUniverseStats.staticPreviousRangeValid;
    sceneLogDesc.staticDirty = geometryUniverseStats.staticDirty;
    sceneLogDesc.staticValidationErrors = geometryUniverseStats.staticValidationErrors;
    sceneLogDesc.staticRangeErrors = geometryUniverseStats.staticRangeErrors;
    sceneLogDesc.staticDuplicateKeys = geometryUniverseStats.staticDuplicateKeys;
    sceneLogDesc.staticHistoryErrors = geometryUniverseStats.staticHistoryErrors;
    sceneLogDesc.staticKeyVectorMismatches = geometryUniverseStats.staticKeyVectorMismatches;
    sceneLogDesc.staticCacheBytesKB = staticCacheBytesKB;
    sceneLogDesc.staticBlasSignatureReused = staticBlasSignatureReused;
    sceneLogDesc.staticBlasSignatureMs = staticBlasSignatureMs;
    sceneLogDesc.staticBlasCacheHitCount = m_smokeStaticBlasCacheHitCount;
    sceneLogDesc.staticBlasCacheMissCount = m_smokeStaticBlasCacheMissCount;
    sceneLogDesc.sceneCaptureLogIntervalFrames = RT_SMOKE_SCENE_LOG_INTERVAL_FRAMES;
    sceneLogDesc.staticBlasCacheHit = staticBlasCacheHit;
    sceneLogDesc.materialTableCacheHit = materialTableCacheHit;
    sceneLogDesc.enableTextureProbe = enableTextureProbe;
    sceneLogDesc.staticBlasSignature = staticSignature.hash;
    sceneLogDesc.materialTableSignature = materialTableSignature;
    sceneLogDesc.materialTablePath = materialTablePath;
    sceneLogDesc.captureTiming = captureTiming;
    sceneLogDesc.classStats = &classStats;
    sceneLogDesc.skipStats = &skipStats;
    sceneLogDesc.dynamicStats = &dynamicStats;
    sceneLogDesc.attributeStats = &attributeStats;
    sceneLogDesc.materialStats = &materialStats;
    sceneLogDesc.bucketRanges = &bucketRanges;
    sceneLogDesc.materialTable = &materialTable;
    sceneLogDesc.emissiveInventoryStats = &emissiveInventoryStats;
    sceneLogDesc.lightCandidateBytes = static_cast<int>(lightCandidates.size() * sizeof(lightCandidates[0]));
    sceneLogDesc.materialTableCacheStats = &materialTableCacheStats;
    sceneLogDesc.materialTableBuildStats = &materialTableBuildStats;
    sceneLogDesc.materialClassifierStats = &materialClassifierStats;
    sceneLogDesc.materialUniverseStats = &materialUniverseStats;
    sceneLogDesc.textureCoverageStats = &textureCoverageStats;
    sceneLogDesc.lastSceneTimingLogMs = &g_smokeLastSceneTimingLogMs;
    sceneLogDesc.sceneRebuildLogged = &m_smokeSceneRebuildLogged;
    sceneLogDesc.sceneLogCooldownFrames = &m_smokeSceneLogCooldownFrames;
    if (r_pathTracingGeometryCanonicalRigidHitDump.GetInteger() != 0)
    {
        r_pathTracingGeometryCanonicalRigidHitDump.SetInteger(0);
        if (!m_staticContractShaderReadbackRequested &&
            !m_staticContractShaderReadbackQueued)
        {
            m_staticContractShaderReadbackRequested = true;
            m_staticContractShaderSampleFrame =
                geometryUniverseStats.frameIndex;
            m_staticContractShaderSampleWidth = m_frameResources.width;
            m_staticContractShaderSampleHeight = m_frameResources.height;
            m_staticContractShaderSampleX =
                Max(0, m_frameResources.width / 2);
            m_staticContractShaderSampleY =
                Max(0, m_frameResources.height / 2);
            m_staticContractExpectedInstance = 0u;
            m_staticContractExpectedPrimitiveFirst = UINT32_MAX;
            m_staticContractExpectedPrimitiveCount = 0u;
            m_staticContractExpectedMaterialId = 0u;
            m_staticContractExpectedMaterialIndex = UINT32_MAX;
            m_canonicalRigidHitSample = true;
            m_canonicalRigidHitTraversalSelected =
                canonicalRigidTraversalSelected;
            m_canonicalRigidHitFirstInstance = 2u;
            m_canonicalRigidHitInstanceCount =
                static_cast<uint32_t>(
                    rigidTlasRouteInstances.size());
            m_canonicalRigidHitRouteContexts.clear();
            m_canonicalRigidHitRouteContexts.reserve(
                rigidTlasPlan.instances.size());
            for (const RtSmokePlanTlasInstance& plannedInstance :
                rigidTlasPlan.instances)
            {
                CanonicalRigidHitRouteContext context;
                context.instanceId = plannedInstance.instanceId;
                context.sourceInstanceId =
                    plannedInstance.sourceInstanceId;
                context.legacyMeshHash = plannedInstance.meshHash;
                context.canonicalMeshHash =
                    plannedInstance.canonicalMeshHash;
                context.routeRecordIndex =
                    plannedInstance.routeRecordIndex;
                context.canonicalBlasRecordIndex =
                    plannedInstance.canonicalBlasRecordIndex;
                context.currentTransformHash = HashSmokeBytes(
                    1469598103934665603ull,
                    plannedInstance.transform,
                    sizeof(plannedInstance.transform));
                context.previousTransformHash = HashSmokeBytes(
                    1469598103934665603ull,
                    plannedInstance.previousTransform,
                    sizeof(plannedInstance.previousTransform));
                context.flags =
                    (plannedInstance.sourceSeenThisFrame ? 1u : 0u) |
                    (plannedInstance.hasPreviousTransform ? 2u : 0u) |
                    (plannedInstance.transformContinuous ? 4u : 0u);
                m_canonicalRigidHitRouteContexts.push_back(context);
            }
            m_canonicalRigidHitEmissiveContexts.clear();
            for (const PathTraceSmokeEmissiveTriangle& triangle :
                emissiveTriangles)
            {
                if (triangle.instanceId <
                        m_canonicalRigidHitFirstInstance ||
                    triangle.instanceId -
                            m_canonicalRigidHitFirstInstance >=
                        m_canonicalRigidHitInstanceCount)
                {
                    continue;
                }
                CanonicalRigidHitEmissiveContext context;
                context.instanceId = triangle.instanceId;
                context.primitiveIndex = triangle.primitiveIndex;
                context.identity =
                    (static_cast<uint64_t>(
                        triangle.identityHashHi) << 32ull) |
                    static_cast<uint64_t>(
                        triangle.identityHashLo);
                context.materialId = triangle.materialId;
                context.materialIndex =
                    triangle.universeMaterialIndex;
                context.emissiveTextureIndex =
                    triangle.emissiveTextureIndex;
                m_canonicalRigidHitEmissiveContexts.push_back(
                    context);
            }
        }
        else
        {
            common->Printf(
                "PathTracePrimaryPass: GEO06 canonical rigid hit sample busy; retry after the queued readback completes\n");
        }
    }
    if (r_pathTracingStaticContractDump.GetInteger() != 0)
    {
        r_pathTracingStaticContractDump.SetInteger(0);
        if (!m_staticContractShaderReadbackRequested &&
            !m_staticContractShaderReadbackQueued)
        {
            m_staticContractShaderReadbackRequested = true;
            m_staticContractShaderSampleFrame = geometryUniverseStats.frameIndex;
            m_staticContractShaderSampleWidth = m_frameResources.width;
            m_staticContractShaderSampleHeight = m_frameResources.height;
            m_staticContractShaderSampleX = Max(0, m_frameResources.width / 2);
            m_staticContractShaderSampleY = Max(0, m_frameResources.height / 2);
            m_staticContractExpectedInstance = 0u;
            m_staticContractExpectedPrimitiveFirst = UINT32_MAX;
            m_staticContractExpectedPrimitiveCount = 0u;
            m_staticContractExpectedMaterialId = 0u;
            m_staticContractExpectedMaterialIndex = UINT32_MAX;
        }

        const bool committedCountsMatch =
            m_sceneInputs.geometry.staticVertexCount == staticVertexCacheCount &&
            m_sceneInputs.geometry.staticIndexCount == staticIndexCacheCount &&
            m_sceneInputs.geometry.staticTriangleCount == staticTriangleCacheCount &&
            accelerationPlan.staticBlas.vertexCount == staticVertexCacheCount &&
            accelerationPlan.staticBlas.indexCount == staticIndexCacheCount;
        const bool committedHandlesMatch =
            m_sceneInputs.geometry.staticVertexBuffer == smokeStaticVertexBuffer &&
            m_sceneInputs.geometry.staticIndexBuffer == smokeStaticIndexBuffer &&
            m_sceneInputs.geometry.staticTriangleClassBuffer == smokeStaticTriangleClassBuffer &&
            m_sceneInputs.geometry.staticTriangleMaterialBuffer == smokeStaticTriangleMaterialBuffer &&
            m_sceneInputs.geometry.staticTriangleMaterialIndexBuffer == smokeStaticTriangleMaterialIndexBuffer &&
            m_sceneInputs.geometry.staticBlas == smokeStaticBlas &&
            m_sceneInputs.geometry.tlas == m_smokeTlas;
        const bool committedSignatureMatches =
            m_sceneInputs.signatures.geometryMembership == staticSignature.hash &&
            m_smokeStaticBlasSignature == staticSignature.hash;
        const bool staleBlasHandle =
            m_sceneInputs.geometry.staticBlas != smokeStaticBlas ||
            smokeStaticBlas != m_smokeStaticBlas;
        const bool tlasBlasFrameMismatch =
            m_sceneInputs.signatures.cpuUploadGeneration != m_smokeGeometryFrameIndex ||
            m_sceneInputs.geometry.tlas != m_smokeTlas ||
            m_sceneInputs.geometry.staticBlas != m_smokeStaticBlas;

        common->Printf(
            "PathTracePrimaryPass: PT static contract tuple map='%s' frame=%llu area=%d worldGen=%llu storageGen=%llu staticGen(cache/current/mismatch/guard)=%llu/%llu/%d/%d activeSetGen=%llu uploadGen=%llu routeGen=%llu counts(cache/blas/shader v/i/t)=%d/%d/%d %d/%d/%d %d/%d/%d handles(v/i/class/material/remap/blas/tlas)=%p/%p/%p/%p/%p/%p/%p staticSig=%llu cacheHit=%d forceRebuild=%d build(submit/skip)=%d/%d tlasInstances=%d tupleMismatch(counts/handles/signature/staleBlas/frameSlot)=%d/%d/%d/%d/%d shaderSample=unobserved\n",
            m_smokeSceneMapName.c_str(),
            static_cast<unsigned long long>(geometryUniverseStats.frameIndex),
            viewDef ? viewDef->areaNum : -1,
            static_cast<unsigned long long>(m_sceneUniverse.GetStats().generation),
            static_cast<unsigned long long>(geometryUniverseStats.generation),
            static_cast<unsigned long long>(cachedStaticBlasGeometryGeneration),
            static_cast<unsigned long long>(geometryUniverseStats.staticGeometryGeneration),
            staticBlasGenerationMismatch ? 1 : 0,
            r_pathTracingStaticBlasGenerationGuard.GetBool() ? 1 : 0,
            static_cast<unsigned long long>(sceneLogDesc.bvhActiveSetSignature),
            static_cast<unsigned long long>(m_sceneInputs.signatures.cpuUploadGeneration),
            static_cast<unsigned long long>(sceneLogDesc.bvhTlasInstanceSignature),
            staticVertexCacheCount,
            staticIndexCacheCount,
            staticTriangleCacheCount,
            accelerationPlan.staticBlas.vertexCount,
            accelerationPlan.staticBlas.indexCount,
            accelerationPlan.staticBlas.indexCount / 3,
            m_sceneInputs.geometry.staticVertexCount,
            m_sceneInputs.geometry.staticIndexCount,
            m_sceneInputs.geometry.staticTriangleCount,
            smokeStaticVertexBuffer.Get(),
            smokeStaticIndexBuffer.Get(),
            smokeStaticTriangleClassBuffer.Get(),
            smokeStaticTriangleMaterialBuffer.Get(),
            smokeStaticTriangleMaterialIndexBuffer.Get(),
            smokeStaticBlas.Get(),
            m_smokeTlas.Get(),
            static_cast<unsigned long long>(staticSignature.hash),
            staticBlasCacheHit ? 1 : 0,
            forceStaticBlasRebuildMode,
            accelSubmitTiming.staticBlasBuildSubmitted ? 1 : 0,
            accelSubmitTiming.staticBlasBuildSkipped ? 1 : 0,
            instanceCount,
            committedCountsMatch ? 0 : 1,
            committedHandlesMatch ? 0 : 1,
            committedSignatureMatches ? 0 : 1,
            staleBlasHandle ? 1 : 0,
            tlasBlasFrameMismatch ? 1 : 0);
        common->Printf(
            "PathTracePrimaryPass: PT static contract vertexUpload frame=%llu gate=%d skip/full/texRange/dirtyRange=%d/%d/%d/%d offset/count=%d/%d bytes=%llu texMatrix(count/first/last)=%d/%d/%d bufferReused=%d\n",
            static_cast<unsigned long long>(geometryUniverseStats.frameIndex),
            r_pathTracingStaticVertexFullUploadOnCacheMiss.GetBool() ? 1 : 0,
            staticVertexUploadPlan.skipUpload ? 1 : 0,
            staticVertexUploadPlan.fullUpload ? 1 : 0,
            staticVertexUploadPlan.texMatrixRangeUpload ? 1 : 0,
            staticVertexUploadPlan.dirtyRangeUpload ? 1 : 0,
            staticVertexUploadPlan.elementOffset,
            staticVertexUploadPlan.elementCount,
            static_cast<unsigned long long>(uploadItems[0].byteSize),
            staticTexMatrixVertices,
            staticTexMatrixFirstVertex,
            staticTexMatrixLastVertex,
            staticGeometryBuffersReused ? 1 : 0);

        modelTrace_t rasterTrace = {};
        bool rasterHit = false;
        modelTrace_t receiverTrace = {};
        bool receiverHit = false;
        bool rasterHitWasDecal = false;
        idVec3 traceStart = vec3_origin;
        idVec3 traceEnd = vec3_origin;
        if (viewDef && viewDef->renderWorld)
        {
            traceStart = viewDef->renderView.vieworg;
            traceEnd = traceStart + viewDef->renderView.viewaxis[0] * 65536.0f;
            rasterHit = viewDef->renderWorld->Trace(rasterTrace, traceStart, traceEnd, 0.0f, true, false);
            rasterHitWasDecal =
                rasterHit &&
                rasterTrace.material &&
                idStr::FindText(rasterTrace.material->GetName(), "textures/decals/", false) >= 0;
            if (rasterHitWasDecal)
            {
                const idVec3 traceDirection = viewDef->renderView.viewaxis[0];
                const idVec3 receiverTraceStart = rasterTrace.point + traceDirection * 0.05f;
                receiverHit = viewDef->renderWorld->Trace(
                    receiverTrace,
                    receiverTraceStart,
                    traceEnd,
                    0.0f,
                    true,
                    false);
            }
        }

        common->Printf(
            "PathTracePrimaryPass: PT static contract rasterProbe hit=%d start=(%.2f %.2f %.2f) end=(%.2f %.2f %.2f) fraction=%.6f point=(%.2f %.2f %.2f) entity=%p material='%s'\n",
            rasterHit ? 1 : 0,
            traceStart.x,
            traceStart.y,
            traceStart.z,
            traceEnd.x,
            traceEnd.y,
            traceEnd.z,
            rasterTrace.fraction,
            rasterTrace.point.x,
            rasterTrace.point.y,
            rasterTrace.point.z,
            rasterTrace.entity,
            rasterTrace.material ? rasterTrace.material->GetName() : "<none>");

        if (rasterHitWasDecal)
        {
            common->Printf(
                "PathTracePrimaryPass: PT static contract receiverProbe hit=%d peelDistance=0.05 fraction=%.6f point=(%.2f %.2f %.2f) entity=%p material='%s'\n",
                receiverHit ? 1 : 0,
                receiverTrace.fraction,
                receiverTrace.point.x,
                receiverTrace.point.y,
                receiverTrace.point.z,
                receiverTrace.entity,
                receiverTrace.material ? receiverTrace.material->GetName() : "<none>");
        }

        const modelTrace_t& contractTrace = receiverHit ? receiverTrace : rasterTrace;
        const bool contractHit = receiverHit || rasterHit;
        int matchingSurfaceCount = 0;
        int boundedSurfaceCount = 0;
        if (contractHit)
        {
            for (const RtPathTraceSceneUniverseSurface& surface : m_sceneUniverse.Surfaces())
            {
                const bool entityMatches =
                    surface.entity && contractTrace.entity == &surface.entity->parms;
                const bool materialMatches =
                    surface.material == contractTrace.material ||
                    (surface.material && contractTrace.material &&
                        idStr::Icmp(surface.material->GetName(), contractTrace.material->GetName()) == 0);
                if (!entityMatches || !materialMatches)
                {
                    continue;
                }

                ++matchingSurfaceCount;
                const idBounds& bounds = surface.bounds;
                const bool pointInsideExpandedBounds =
                    !bounds.IsCleared() &&
                    contractTrace.point.x >= bounds[0].x - 4.0f &&
                    contractTrace.point.y >= bounds[0].y - 4.0f &&
                    contractTrace.point.z >= bounds[0].z - 4.0f &&
                    contractTrace.point.x <= bounds[1].x + 4.0f &&
                    contractTrace.point.y <= bounds[1].y + 4.0f &&
                    contractTrace.point.z <= bounds[1].z + 4.0f;
                if (pointInsideExpandedBounds)
                {
                    ++boundedSurfaceCount;
                }
            }
        }

        int emittedCandidates = 0;
        int missingRecordCount = 0;
        int inactiveRecordCount = 0;
        int invalidRangeCount = 0;
        int missingMaterialCount = 0;
        for (const RtPathTraceSceneUniverseSurface& surface : m_sceneUniverse.Surfaces())
        {
            if (!contractHit || emittedCandidates >= 8)
            {
                break;
            }

            const bool entityMatches =
                surface.entity && contractTrace.entity == &surface.entity->parms;
            const bool materialMatches =
                surface.material == contractTrace.material ||
                (surface.material && contractTrace.material &&
                    idStr::Icmp(surface.material->GetName(), contractTrace.material->GetName()) == 0);
            if (!entityMatches || !materialMatches)
            {
                continue;
            }

            const idBounds& bounds = surface.bounds;
            const bool pointInsideExpandedBounds =
                !bounds.IsCleared() &&
                contractTrace.point.x >= bounds[0].x - 4.0f &&
                contractTrace.point.y >= bounds[0].y - 4.0f &&
                contractTrace.point.z >= bounds[0].z - 4.0f &&
                contractTrace.point.x <= bounds[1].x + 4.0f &&
                contractTrace.point.y <= bounds[1].y + 4.0f &&
                contractTrace.point.z <= bounds[1].z + 4.0f;
            if (boundedSurfaceCount > 0 && !pointInsideExpandedBounds)
            {
                continue;
            }

            const RtSmokePersistentStaticSurfaceRecord* record =
                m_smokeGeometryUniverse.FindStaticSurface(surface.legacyDrawSurfKey);
            const bool recordPresent = record && record->valid;
            const int vertexOffset = recordPresent ? record->currentRange.vertices.offset : -1;
            const int vertexCount = recordPresent ? record->currentRange.vertices.count : 0;
            const int indexOffset = recordPresent ? record->currentRange.indexes.offset : -1;
            const int indexCount = recordPresent ? record->currentRange.indexes.count : 0;
            const int triangleOffset = recordPresent ? record->currentRange.triangles.offset : -1;
            const int triangleCount = recordPresent ? record->currentRange.triangles.count : 0;
            const bool rangeValid =
                recordPresent &&
                vertexOffset >= 0 &&
                vertexCount > 0 &&
                vertexOffset + vertexCount <= staticVertexCacheCount &&
                indexOffset >= 0 &&
                indexCount > 0 &&
                (indexCount % 3) == 0 &&
                indexOffset + indexCount <= staticIndexCacheCount &&
                triangleOffset >= 0 &&
                triangleCount == indexCount / 3 &&
                triangleOffset + triangleCount <= staticTriangleCacheCount &&
                triangleOffset + triangleCount <= static_cast<int>(materialTable.staticMaterialIndexes.size());
            const uint32_t materialIndex =
                rangeValid ? materialTable.staticMaterialIndexes[triangleOffset] : UINT32_MAX;
            const bool materialPresent =
                rangeValid &&
                record->materialId != 0u &&
                materialIndex < static_cast<uint32_t>(materialTable.materials.size());
            const bool routePresent =
                rangeValid &&
                hasStaticBlas &&
                smokeStaticBlas &&
                m_smokeTlas &&
                instanceCount > 0;
            bool cacheRayHit = false;
            int cacheRayPrimitive = -1;
            int cacheRayValidTriangles = 0;
            float cacheRayHitDistance = 0.0f;
            idBounds cacheBounds;
            cacheBounds.Clear();
            if (rangeValid)
            {
                for (int vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
                {
                    cacheBounds.AddPoint(SmokeVertexPosition(staticVertexCache[vertexOffset + vertexIndex]));
                }

                idVec3 cacheRayDirection = traceEnd - traceStart;
                cacheRayDirection.Normalize();
                float closestHitDistance = 1.0e30f;
                for (int localTriangle = 0; localTriangle < triangleCount; ++localTriangle)
                {
                    const int localIndexOffset = indexOffset + localTriangle * 3;
                    const uint32_t i0 = staticIndexCache[localIndexOffset + 0];
                    const uint32_t i1 = staticIndexCache[localIndexOffset + 1];
                    const uint32_t i2 = staticIndexCache[localIndexOffset + 2];
                    if (i0 >= staticVertexCache.size() ||
                        i1 >= staticVertexCache.size() ||
                        i2 >= staticVertexCache.size())
                    {
                        continue;
                    }
                    ++cacheRayValidTriangles;
                    float hitDistance = 0.0f;
                    if (IntersectRayTriangle(
                            traceStart,
                            cacheRayDirection,
                            SmokeVertexPosition(staticVertexCache[i0]),
                            SmokeVertexPosition(staticVertexCache[i1]),
                            SmokeVertexPosition(staticVertexCache[i2]),
                            hitDistance) &&
                        hitDistance < closestHitDistance)
                    {
                        cacheRayHit = true;
                        cacheRayPrimitive = triangleOffset + localTriangle;
                        cacheRayHitDistance = hitDistance;
                        closestHitDistance = hitDistance;
                    }
                }
            }
            if (emittedCandidates == 0 && rangeValid &&
                m_staticContractShaderReadbackRequested &&
                m_staticContractShaderSampleFrame == geometryUniverseStats.frameIndex)
            {
                m_staticContractExpectedPrimitiveFirst = static_cast<uint32_t>(triangleOffset);
                m_staticContractExpectedPrimitiveCount = static_cast<uint32_t>(triangleCount);
                m_staticContractExpectedMaterialId = record->materialId;
                m_staticContractExpectedMaterialIndex = materialIndex;
            }
            if (emittedCandidates == 0 && rangeValid)
            {
                QueueStaticContractGeometrySample(
                    commandList,
                    smokeStaticVertexBuffer,
                    smokeStaticIndexBuffer,
                    staticVertexCache.data(),
                    vertexOffset,
                    vertexCount,
                    staticIndexCache.data(),
                    indexOffset,
                    indexCount,
                    geometryUniverseStats.frameIndex);
            }

            missingRecordCount += recordPresent ? 0 : 1;
            inactiveRecordCount += recordPresent && !record->seenThisFrame ? 1 : 0;
            invalidRangeCount += recordPresent && !rangeValid ? 1 : 0;
            missingMaterialCount += rangeValid && !materialPresent ? 1 : 0;

            common->Printf(
                "PathTracePrimaryPass: PT static contract candidate index=%d universeKey=%llu storageKey=%llu model='%s' material='%s' entity/surface=%d/%d areas(center/off/count)=%d/%d/%d boundsHit=%d source(v/i/t)=%d/%d/%d record(present/seen)=%d/%d range(v/i/t)=%d/%d %d/%d %d/%d rangeValid=%d material(id/index/table/present)=%u/%u/%d/%d route(expectedInstance/present)=%d/%d shader(instance/primitive/reject)=unobserved\n",
                emittedCandidates,
                static_cast<unsigned long long>(surface.key),
                static_cast<unsigned long long>(surface.legacyDrawSurfKey),
                surface.modelName.c_str(),
                surface.materialName.c_str(),
                surface.entityIndex,
                surface.surfaceIndex,
                surface.centerArea,
                surface.offCenterArea,
                surface.areaCount,
                pointInsideExpandedBounds ? 1 : 0,
                surface.numVerts,
                surface.numIndexes,
                surface.triangles,
                recordPresent ? 1 : 0,
                recordPresent && record->seenThisFrame ? 1 : 0,
                vertexOffset,
                vertexCount,
                indexOffset,
                indexCount,
                triangleOffset,
                triangleCount,
                rangeValid ? 1 : 0,
                recordPresent ? record->materialId : 0u,
                materialIndex,
                static_cast<int>(materialTable.materials.size()),
                materialPresent ? 1 : 0,
                0,
                routePresent ? 1 : 0);
            common->Printf(
                "PathTracePrimaryPass: PT static contract cacheRay candidate=%d validTriangles=%d hit=%d primitive=%d hitT=%.4f receiverT=%.4f delta=%.4f bounds=(%.2f %.2f %.2f)-(%.2f %.2f %.2f)\n",
                emittedCandidates,
                cacheRayValidTriangles,
                cacheRayHit ? 1 : 0,
                cacheRayPrimitive,
                cacheRayHitDistance,
                (contractTrace.point - traceStart).Length(),
                cacheRayHit ? idMath::Fabs(cacheRayHitDistance - (contractTrace.point - traceStart).Length()) : 0.0f,
                cacheBounds[0].x,
                cacheBounds[0].y,
                cacheBounds[0].z,
                cacheBounds[1].x,
                cacheBounds[1].y,
                cacheBounds[1].z);
            ++emittedCandidates;
        }

        common->Printf(
            "PathTracePrimaryPass: PT static contract counters rasterMatches=%d boundsMatches=%d emitted=%d generationMismatch=%d missingRecord=%d inactiveTraversal=%d outOfRangePrimitive=%d missingRoute=%d missingMaterial=%d staleBlas=%d tlasBlasFrameSlotMismatch=%d remainingBoundary=%s\n",
            matchingSurfaceCount,
            boundedSurfaceCount,
            emittedCandidates,
            committedSignatureMatches ? 0 : 1,
            missingRecordCount,
            inactiveRecordCount,
            invalidRangeCount,
            emittedCandidates > 0 && (!hasStaticBlas || !smokeStaticBlas || !m_smokeTlas || instanceCount <= 0) ? emittedCandidates : 0,
            missingMaterialCount,
            staleBlasHandle ? 1 : 0,
            tlasBlasFrameMismatch ? 1 : 0,
            emittedCandidates > 0 && missingRecordCount == 0 && invalidRangeCount == 0 &&
                    missingMaterialCount == 0 && committedCountsMatch && committedHandlesMatch &&
                    committedSignatureMatches && !staleBlasHandle && !tlasBlasFrameMismatch
                ? "shader_instance_primitive_route"
                : "cpu_producer_storage_or_publication");
    }
    {
        OPTICK_EVENT("PT Scene Diagnostic Logs");
        RunSmokeSceneBuildDiagnosticLogs(sceneLogDesc);
    }
}
