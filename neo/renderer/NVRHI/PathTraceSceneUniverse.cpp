#include "precompiled.h"
#pragma hdrstop

#include "PathTraceSceneUniverse.h"
#include "PathTraceAccelerationPlan.h"
#include "PathTraceCVars.h"
#include "PathTraceDoomMaterialClassifier.h"
#include "PathTraceDynamicMaterialState.h"
#include "PathTraceGeometryUniverse.h"
#include "PathTraceSceneCapture.h"
#include "PathTraceTextureRegistry.h"
#include "../Material.h"
#include "../Model.h"
#include "../GLMatrix.h"
#include "../RenderCommon.h"
#include "../RenderWorld_local.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace {

const int PT_SCENE_UNIVERSE_MAX_AREA_REFS = 8;
const int PT_SCENE_UNIVERSE_MAX_SELECTION_AREAS = 16;

int SceneUniverseStaticWorldPortalArea(
    const idRenderModel* model,
    int portalAreaCount)
{
    const char* modelName = model ? model->Name() : nullptr;
    if (!modelName || idStr::Cmpn(modelName, "_area", 5) != 0 ||
        modelName[5] == '\0')
    {
        return RT_SMOKE_STATIC_BUCKET_FALLBACK_AREA;
    }

    int portalArea = 0;
    for (const char* digit = modelName + 5; *digit; ++digit)
    {
        if (*digit < '0' || *digit > '9')
        {
            return RT_SMOKE_STATIC_BUCKET_FALLBACK_AREA;
        }
        const int value = *digit - '0';
        if (portalArea > (INT_MAX - value) / 10)
        {
            return RT_SMOKE_STATIC_BUCKET_FALLBACK_AREA;
        }
        portalArea = portalArea * 10 + value;
    }
    return portalArea >= 0 && portalArea < portalAreaCount
        ? portalArea
        : RT_SMOKE_STATIC_BUCKET_FALLBACK_AREA;
}

void DumpSceneUniverseResidencyStatsIfNeeded(const RtPathTraceSceneUniverseBuildStats& stats)
{
    if (r_pathTracingResidencyDump.GetInteger() != 1)
    {
        return;
    }

    static int lastDumpFrame = -120;
    if (tr.frameCount - lastDumpFrame < 120)
    {
        return;
    }
    lastDumpFrame = tr.frameCount;
    common->Printf(
        "PathTracePrimaryPass: RES staticArea visited=%d derived=%d hits=%d misses=%d\n",
        stats.residencyVisited,
        stats.residencyDerived,
        stats.residencyCacheHits,
        stats.residencyCacheMisses);
}

uint64 HashSceneUniverseValue(uint64 hash, uint64 value)
{
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash;
}

uint64 HashSceneUniverseString(uint64 hash, const char* text)
{
    const char* cursor = text ? text : "<none>";
    while (*cursor)
    {
        hash = HashSceneUniverseValue(hash, static_cast<uint8_t>(*cursor));
        ++cursor;
    }
    return HashSceneUniverseValue(hash, 0xffu);
}

uint64 HashSceneUniverseBytes(uint64 hash, const void* data, size_t size)
{
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t index = 0; index < size; ++index)
    {
        hash ^= bytes[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

uint64 BuildSceneUniverseSurfaceKey(const idRenderModel* model, int entityIndex, int surfaceIndex, const idMaterial* material, const srfTriangles_t* tri)
{
    uint64 hash = 1469598103934665603ull;
    hash = HashSceneUniverseString(hash, model ? model->Name() : "<none>");
    hash = HashSceneUniverseString(hash, material ? material->GetName() : "<none>");
    hash = HashSceneUniverseValue(hash, static_cast<uint64>(entityIndex + 0x80000000u));
    hash = HashSceneUniverseValue(hash, static_cast<uint64>(surfaceIndex + 0x80000000u));
    hash = HashSceneUniverseValue(hash, static_cast<uint64>(tri ? tri->numVerts : 0));
    hash = HashSceneUniverseValue(hash, static_cast<uint64>(tri ? tri->numIndexes : 0));
    return hash;
}

uint64 BuildSceneUniverseLegacyDrawSurfKey(const idRenderEntityLocal* entity, const idMaterial* material, const srfTriangles_t* tri)
{
    uint64 hash = 14695981039346656037ull;
    const uintptr_t triPtr = reinterpret_cast<uintptr_t>(tri);
    const uintptr_t materialPtr = reinterpret_cast<uintptr_t>(material);
    hash = HashSceneUniverseBytes(hash, &triPtr, sizeof(triPtr));
    hash = HashSceneUniverseBytes(hash, &materialPtr, sizeof(materialPtr));
    if (tri)
    {
        hash = HashSceneUniverseBytes(hash, &tri->numVerts, sizeof(tri->numVerts));
        hash = HashSceneUniverseBytes(hash, &tri->numIndexes, sizeof(tri->numIndexes));
        hash = HashSceneUniverseBytes(hash, &tri->ambientCache, sizeof(tri->ambientCache));
        hash = HashSceneUniverseBytes(hash, &tri->indexCache, sizeof(tri->indexCache));
    }
    if (entity)
    {
        hash = HashSceneUniverseBytes(hash, entity->modelMatrix, sizeof(entity->modelMatrix));
    }
    return hash;
}

bool SceneUniverseStageIsAdditiveOrGlowLike(const shaderStage_t* stage)
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

float SceneUniverseDynamicEvalColorLuminance(const float color[4])
{
    return Max(0.0f, color[0]) * 0.2126f +
        Max(0.0f, color[1]) * 0.7152f +
        Max(0.0f, color[2]) * 0.0722f;
}

bool SceneUniverseDynamicEvalSampleShouldReplace(const RtSmokeDynamicMaterialEvalSample& current, const RtSmokeDynamicMaterialEvalSample& candidate)
{
    if (!current.valid)
    {
        return true;
    }
    if (candidate.stagePriority != current.stagePriority)
    {
        return candidate.stagePriority > current.stagePriority;
    }
    return SceneUniverseDynamicEvalColorLuminance(candidate.color) > SceneUniverseDynamicEvalColorLuminance(current.color);
}

void SceneUniverseDynamicEvalCopySelectedStage(RtSmokeDynamicMaterialEvalSample& dst, const RtSmokeDynamicMaterialEvalSample& src)
{
    dst.valid = src.valid;
    dst.stageIndex = src.stageIndex;
    dst.stagePriority = src.stagePriority;
    dst.selectedStageEmissive = src.selectedStageEmissive;
    dst.condition = src.condition;
    dst.alphaTest = src.alphaTest;
    dst.image = src.image;
    for (int component = 0; component < 4; ++component)
    {
        dst.color[component] = src.color[component];
    }
    for (int row = 0; row < 2; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            dst.texMatrix[row][column] = src.texMatrix[row][column];
        }
    }
}

void SceneUniverseDynamicEvalAccumulateSample(RtSmokeDynamicMaterialEvalSample& sample, const RtSmokeDynamicMaterialEvalSample& surfaceSample, int indexes)
{
    ++sample.surfaces;
    sample.triangles += indexes / 3;
    sample.enabledStages += surfaceSample.enabledStages;
    sample.disabledStages += surfaceSample.disabledStages;
    sample.colorStages += surfaceSample.colorStages;
    sample.alphaStages += surfaceSample.alphaStages;
    sample.alphaTestStages += surfaceSample.alphaTestStages;
    sample.texMatrixStages += surfaceSample.texMatrixStages;
    sample.dynamicImageStages += surfaceSample.dynamicImageStages;
    sample.cinematicStages += surfaceSample.cinematicStages;
    sample.guiRenderTargetStages += surfaceSample.guiRenderTargetStages;
    sample.programStages += surfaceSample.programStages;
    if (SceneUniverseDynamicEvalSampleShouldReplace(sample, surfaceSample))
    {
        SceneUniverseDynamicEvalCopySelectedStage(sample, surfaceSample);
    }
}

void SceneUniverseDynamicEvalAddMaterialSample(RtSmokeMaterialStats& stats, const RtSmokeDynamicMaterialEvalSample& surfaceSample, int indexes)
{
    for (RtSmokeDynamicMaterialEvalSample& sample : stats.dynamicEvalMaterialSamples)
    {
        if (sample.id == surfaceSample.id)
        {
            SceneUniverseDynamicEvalAccumulateSample(sample, surfaceSample, indexes);
            return;
        }
    }

    RtSmokeDynamicMaterialEvalSample sample = surfaceSample;
    sample.surfaces = 1;
    sample.triangles = indexes / 3;
    stats.dynamicEvalMaterialSamples.push_back(sample);
}

bool SceneUniverseStageConditionCanBeActive(const idMaterial* material, const shaderStage_t* stage)
{
    if (!material || !stage)
    {
        return false;
    }

    const float* constantRegisters = material->ConstantRegisters();
    const int registerCount = material->GetNumRegisters();
    if (constantRegisters && stage->conditionRegister >= 0 && stage->conditionRegister < registerCount)
    {
        return constantRegisters[stage->conditionRegister] != 0.0f;
    }
    return true;
}

bool SceneUniverseMaterialCanEmit(const idMaterial* material)
{
    if (!material)
    {
        return false;
    }

    const RtSmokeTranslucentClassifierInfo classifier = BuildSmokeTranslucentClassifierInfo(material);
    const bool nameLooksEmissive = !classifier.hasAddDefault0200Texture && (classifier.nameLooksGlow || classifier.nameLooksSignage);
    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || stage->lighting != SL_AMBIENT || !SceneUniverseStageConditionCanBeActive(material, stage))
        {
            continue;
        }

        bool filterBlackKey = false;
        const bool filterStage = SmokeStageIsFilterBlend(stage, filterBlackKey);
        const bool stageLooksEmissive =
            SceneUniverseStageIsAdditiveOrGlowLike(stage) ||
            (nameLooksEmissive && classifier.hasAmbientBlendStage && !classifier.nameLooksDecal && !filterStage);
        if (stageLooksEmissive)
        {
            return true;
        }
    }

    return false;
}

bool SceneUniverseBoundsSafeForAreaQuery(const idBounds& bounds)
{
    if (bounds.IsCleared())
    {
        return false;
    }

    return
        bounds[1].x - bounds[0].x < 9999.0f &&
        bounds[1].y - bounds[0].y < 9999.0f &&
        bounds[1].z - bounds[0].z < 9999.0f;
}

bool SceneUniverseRigidEntityEligible(const idRenderEntityLocal* entity, const idRenderModel* model)
{
    const renderEntity_t* renderEntity = entity ? &entity->parms : nullptr;
    if (!entity || !renderEntity || !model || model->IsStaticWorldModel())
    {
        return false;
    }
    if (model->IsDynamicModel() != DM_STATIC)
    {
        return false;
    }
    if (renderEntity->joints != nullptr || renderEntity->numJoints > 0)
    {
        return false;
    }
    if (renderEntity->callback != nullptr || renderEntity->forceUpdate != 0)
    {
        return false;
    }
    if (renderEntity->weaponDepthHack || renderEntity->modelDepthHack != 0.0f)
    {
        return false;
    }
    return true;
}

const idMaterial* SceneUniverseResolveEntitySurfaceMaterial(const idRenderEntityLocal* entity, const modelSurface_t* surface)
{
    const idMaterial* material = surface ? surface->shader : nullptr;
    if (!entity || !material)
    {
        return material;
    }

    if (entity->parms.customShader != nullptr)
    {
        if (material->Deform())
        {
            return nullptr;
        }
        return entity->parms.customShader;
    }

    if (entity->parms.customSkin)
    {
        material = entity->parms.customSkin->RemapShaderBySkin(material);
    }
    return material;
}

bool SceneUniverseEntityHasEmissiveSurface(const idRenderEntityLocal* entity, const idRenderModel* model)
{
    if (!entity || !model)
    {
        return false;
    }

    for (int surfaceIndex = 0; surfaceIndex < model->NumSurfaces(); ++surfaceIndex)
    {
        const modelSurface_t* surface = model->Surface(surfaceIndex);
        const idMaterial* material = SceneUniverseResolveEntitySurfaceMaterial(entity, surface);
        const srfTriangles_t* tri = surface ? surface->geometry : nullptr;
        if (!material || !tri || !tri->verts || !tri->indexes || tri->numVerts < 3 || tri->numIndexes < 3 || !material->IsDrawn())
        {
            continue;
        }
        if (SceneUniverseMaterialCanEmit(material))
        {
            return true;
        }
    }

    return false;
}

idBounds SceneUniverseSurfaceWorldBounds(const idRenderEntityLocal* entity, const srfTriangles_t* tri)
{
    idBounds localBounds;
    localBounds.Clear();
    if (tri)
    {
        localBounds = tri->bounds;
        if (localBounds.IsCleared() && tri->verts)
        {
            for (int vertexIndex = 0; vertexIndex < tri->numVerts; ++vertexIndex)
            {
                localBounds.AddPoint(tri->verts[vertexIndex].xyz);
            }
        }
    }

    idBounds worldBounds;
    worldBounds.Clear();
    if (!entity || localBounds.IsCleared())
    {
        return worldBounds;
    }

    worldBounds.FromTransformedBounds(localBounds, entity->parms.origin, entity->parms.axis);
    return worldBounds;
}

void SceneUniverseAddAreaStat(
    std::vector<RtPathTraceSceneUniverseAreaStats>& areaStats,
    int area,
    int triangles,
    bool emissiveCapable,
    int portals)
{
    if (area < 0)
    {
        return;
    }

    for (RtPathTraceSceneUniverseAreaStats& stats : areaStats)
    {
        if (stats.area == area)
        {
            ++stats.surfaces;
            stats.triangles += triangles;
            stats.emissiveCapableSurfaces += emissiveCapable ? 1 : 0;
            stats.portals = portals;
            return;
        }
    }

    RtPathTraceSceneUniverseAreaStats stats;
    stats.area = area;
    stats.surfaces = 1;
    stats.triangles = triangles;
    stats.emissiveCapableSurfaces = emissiveCapable ? 1 : 0;
    stats.portals = portals;
    areaStats.push_back(stats);
}

}

bool SceneUniverseSurfaceTouchesSelectedArea(const RtPathTraceSceneUniverseSurface& surface, const std::vector<bool>& selectedAreas)
{
    for (int areaIndex = 0; areaIndex < surface.areaCount; ++areaIndex)
    {
        const int area = surface.areas[areaIndex];
        if (area >= 0 && area < static_cast<int>(selectedAreas.size()) && selectedAreas[area])
        {
            return true;
        }
    }
    return false;
}

void SceneUniverseAddSmokeMaterialStats(RtSmokeMaterialStats& stats, const idMaterial* material, int indexes)
{
    const char* materialName = material ? material->GetName() : "<none>";
    const uint32_t materialId = HashSmokeMaterialName(materialName);
    ++stats.totalSurfaces;
    stats.totalTriangles += indexes / 3;

    if (std::find(stats.materialIds.begin(), stats.materialIds.end(), materialId) == stats.materialIds.end())
    {
        stats.materialIds.push_back(materialId);
        ++stats.uniqueMaterials;
    }

    for (int sampleIndex = 0; sampleIndex < stats.sampleCount; ++sampleIndex)
    {
        RtSmokeMaterialSample& sample = stats.samples[sampleIndex];
        if (sample.id == materialId)
        {
            ++sample.surfaces;
            sample.triangles += indexes / 3;
            return;
        }
    }

    if (stats.sampleCount < RT_SMOKE_MATERIAL_REASON_SAMPLES)
    {
        RtSmokeMaterialSample& sample = stats.samples[stats.sampleCount++];
        sample.id = materialId;
        sample.surfaces = 1;
        sample.triangles = indexes / 3;
        sample.name = materialName;
    }
}

bool SceneUniverseEvalRegister(const float* regs, int registerCount, int registerIndex, float fallback, float& value)
{
    if (regs && registerIndex >= 0 && registerIndex < registerCount)
    {
        value = regs[registerIndex];
        return true;
    }
    value = fallback;
    return false;
}

bool SceneUniverseRegisterDependsOnRuntime(const idMaterial* material, int registerIndex)
{
    if (registerIndex < 0)
    {
        return false;
    }
    if (registerIndex < EXP_REG_NUM_PREDEFINED)
    {
        return true;
    }
    return material && material->ConstantRegisters() == nullptr;
}

bool SceneUniverseStageUsesPerSurfaceMaterialState(const idMaterial* material, const shaderStage_t* stage)
{
    if (!material || !stage)
    {
        return false;
    }

    if (SceneUniverseRegisterDependsOnRuntime(material, stage->conditionRegister))
    {
        return true;
    }
    if (stage->hasAlphaTest && SceneUniverseRegisterDependsOnRuntime(material, stage->alphaTestRegister))
    {
        return true;
    }
    for (int component = 0; component < 4; ++component)
    {
        if (SceneUniverseRegisterDependsOnRuntime(material, stage->color.registers[component]))
        {
            return true;
        }
    }
    if (stage->texture.hasMatrix)
    {
        for (int row = 0; row < 2; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                if (SceneUniverseRegisterDependsOnRuntime(material, stage->texture.matrix[row][column]))
                {
                    return true;
                }
            }
        }
    }
    return stage->texture.dynamic != DI_STATIC ||
        stage->texture.dynamicFrameCount > 0 ||
        stage->texture.cinematic != nullptr ||
        stage->texture.texgen == TG_SCREEN ||
        stage->texture.texgen == TG_SCREEN2 ||
        stage->newStage != nullptr;
}

void SceneUniverseAccumulateDynamicMaterialEvalStats(RtSmokeMaterialStats& stats, const idMaterial* material, const float* regs, int indexes, uint32_t materialId)
{
    if (!material || !regs)
    {
        ++stats.dynamicEvalNoRegisterSurfaces;
        return;
    }

    const int registerCount = material->GetNumRegisters();
    RtSmokeDynamicMaterialEvalSample surfaceSample;
    surfaceSample.valid = false;
    surfaceSample.id = materialId != 0u ? materialId : SmokeMaterialId(material);
    surfaceSample.name = material->GetName();

    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!SceneUniverseStageUsesPerSurfaceMaterialState(material, stage))
        {
            continue;
        }

        float condition = 1.0f;
        SceneUniverseEvalRegister(regs, registerCount, stage->conditionRegister, 1.0f, condition);
        const bool enabled = condition != 0.0f;
        ++surfaceSample.enabledStages;
        if (!enabled)
        {
            --surfaceSample.enabledStages;
            ++surfaceSample.disabledStages;
        }

        bool hasColor = false;
        float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        for (int component = 0; component < 4; ++component)
        {
            hasColor = SceneUniverseEvalRegister(regs, registerCount, stage->color.registers[component], 1.0f, color[component]) || hasColor;
        }
        if (hasColor)
        {
            ++surfaceSample.colorStages;
        }
        if (hasColor && idMath::Fabs(color[3] - 1.0f) > 1.0e-4f)
        {
            ++surfaceSample.alphaStages;
        }

        float alphaTest = 0.0f;
        const bool hasAlphaTest = stage->hasAlphaTest &&
            SceneUniverseEvalRegister(regs, registerCount, stage->alphaTestRegister, 0.0f, alphaTest);
        if (hasAlphaTest)
        {
            ++surfaceSample.alphaTestStages;
        }

        bool hasTexMatrix = false;
        float texMatrix[2][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
        if (stage->texture.hasMatrix)
        {
            hasTexMatrix = true;
            ++surfaceSample.texMatrixStages;
            for (int row = 0; row < 2; ++row)
            {
                for (int column = 0; column < 3; ++column)
                {
                    const float fallback = row == column ? 1.0f : 0.0f;
                    SceneUniverseEvalRegister(regs, registerCount, stage->texture.matrix[row][column], fallback, texMatrix[row][column]);
                }
            }
        }
        if (stage->texture.dynamic != DI_STATIC || stage->texture.dynamicFrameCount > 0)
        {
            ++surfaceSample.dynamicImageStages;
        }
        if (stage->texture.cinematic != nullptr)
        {
            ++surfaceSample.cinematicStages;
        }
        if (stage->texture.texgen == TG_SCREEN ||
            stage->texture.texgen == TG_SCREEN2 ||
            SmokeStageIsRenderMap(stage))
        {
            ++surfaceSample.guiRenderTargetStages;
        }
        if (stage->newStage != nullptr)
        {
            ++surfaceSample.programStages;
        }

        const bool stageEmissive = SceneUniverseStageIsAdditiveOrGlowLike(stage);
        const int stagePriority = (stage->hasAlphaTest ? 8 : 0) + (enabled ? 4 : 0) + (stageEmissive ? 2 : 0);
        RtSmokeDynamicMaterialEvalSample stageSample;
        stageSample.valid = true;
        stageSample.stageIndex = stageIndex;
        stageSample.stagePriority = stagePriority;
        stageSample.selectedStageEmissive = stageEmissive;
        stageSample.condition = condition;
        stageSample.alphaTest = alphaTest;
        stageSample.image = stage->texture.image;
        for (int component = 0; component < 4; ++component)
        {
            stageSample.color[component] = color[component];
        }
        if (hasTexMatrix)
        {
            for (int row = 0; row < 2; ++row)
            {
                for (int column = 0; column < 3; ++column)
                {
                    stageSample.texMatrix[row][column] = texMatrix[row][column];
                }
            }
        }
        if (SceneUniverseDynamicEvalSampleShouldReplace(surfaceSample, stageSample))
        {
            SceneUniverseDynamicEvalCopySelectedStage(surfaceSample, stageSample);
        }
    }

    if (!surfaceSample.valid)
    {
        ++stats.dynamicEvalNoSelectedStageSurfaces;
        return;
    }

    ++stats.dynamicEvalSurfaces;
    stats.dynamicEvalTriangles += indexes / 3;
    stats.dynamicEvalStages += surfaceSample.enabledStages + surfaceSample.disabledStages;
    stats.dynamicEvalEnabledStages += surfaceSample.enabledStages;
    stats.dynamicEvalDisabledStages += surfaceSample.disabledStages;
    stats.dynamicEvalColorStages += surfaceSample.colorStages;
    stats.dynamicEvalAlphaStages += surfaceSample.alphaStages;
    stats.dynamicEvalAlphaTestStages += surfaceSample.alphaTestStages;
    stats.dynamicEvalTexMatrixStages += surfaceSample.texMatrixStages;
    stats.dynamicEvalDynamicImageStages += surfaceSample.dynamicImageStages;
    stats.dynamicEvalCinematicStages += surfaceSample.cinematicStages;
    stats.dynamicEvalGuiRenderTargetStages += surfaceSample.guiRenderTargetStages;
    stats.dynamicEvalProgramStages += surfaceSample.programStages;

    for (int sampleIndex = 0; sampleIndex < stats.dynamicEvalSampleCount; ++sampleIndex)
    {
        RtSmokeDynamicMaterialEvalSample& sample = stats.dynamicEvalSamples[sampleIndex];
        if (sample.id == surfaceSample.id)
        {
            SceneUniverseDynamicEvalAccumulateSample(sample, surfaceSample, indexes);
            SceneUniverseDynamicEvalAddMaterialSample(stats, surfaceSample, indexes);
            return;
        }
    }

    if (stats.dynamicEvalSampleCount < RT_SMOKE_DYNAMIC_MATERIAL_REASON_SAMPLES)
    {
        RtSmokeDynamicMaterialEvalSample& sample = stats.dynamicEvalSamples[stats.dynamicEvalSampleCount++];
        sample = surfaceSample;
        sample.surfaces = 1;
        sample.triangles = indexes / 3;
    }
    SceneUniverseDynamicEvalAddMaterialSample(stats, surfaceSample, indexes);
}

void SceneUniverseAddDynamicMaterialEvalStats(
    RtSmokeMaterialStats& stats,
    const viewDef_t* viewDef,
    const idRenderEntityLocal* entity,
    const idMaterial* material,
    int indexes)
{
    SceneUniverseAddDynamicMaterialEvalStatsForId(stats, viewDef, entity, material, indexes, SmokeMaterialId(material));
}

void SceneUniverseAddDynamicMaterialEvalStatsForId(
    RtSmokeMaterialStats& stats,
    const viewDef_t* viewDef,
    const idRenderEntityLocal* entity,
    const idMaterial* material,
    int indexes,
    uint32_t materialId)
{
    if (!viewDef || !entity || !material)
    {
        return;
    }
    if (r_pathTracingResidency.GetInteger() != 0 && r_pathTracingResidencyMaterial.GetInteger() != 0)
    {
        const RtSmokeMaterialTextureInfo* info = FindSmokeMaterialTextureInfoReadOnly(materialId);
        if (info && SmokeMaterialTextureInfoHasMaterialMetadata(*info) && !info->isDynamic)
        {
            return;
        }
    }

    const renderEntity_t& renderEntity = entity->parms;
    const float* shaderParms = renderEntity.shaderParms;
    const float* globalParms = viewDef->renderView.shaderParms;
    const int timeGroup = idMath::ClampInt(0, 1, renderEntity.timeGroup);
    float floatTime = viewDef->renderView.time[timeGroup] * 0.001f;

    float generatedShaderParms[MAX_ENTITY_SHADER_PARMS];
    if (renderEntity.referenceShader != nullptr)
    {
        float refRegs[MAX_EXPRESSION_REGISTERS];
        renderEntity.referenceShader->EvaluateRegisters(
            refRegs,
            shaderParms,
            globalParms,
            floatTime,
            renderEntity.referenceSound);

        const shaderStage_t* referenceStage = renderEntity.referenceShader->GetStage(0);
        memcpy(generatedShaderParms, shaderParms, sizeof(generatedShaderParms));
        if (referenceStage)
        {
            const int referenceRegisterCount = renderEntity.referenceShader->GetNumRegisters();
            SceneUniverseEvalRegister(refRegs, referenceRegisterCount, referenceStage->color.registers[0], generatedShaderParms[0], generatedShaderParms[0]);
            SceneUniverseEvalRegister(refRegs, referenceRegisterCount, referenceStage->color.registers[1], generatedShaderParms[1], generatedShaderParms[1]);
            SceneUniverseEvalRegister(refRegs, referenceRegisterCount, referenceStage->color.registers[2], generatedShaderParms[2], generatedShaderParms[2]);
        }
        shaderParms = generatedShaderParms;
    }

    const float* regs = material->ConstantRegisters();
    float dynamicRegs[MAX_EXPRESSION_REGISTERS];
    if (!regs)
    {
        material->EvaluateRegisters(
            dynamicRegs,
            shaderParms,
            globalParms,
            floatTime,
            renderEntity.referenceSound);
        regs = dynamicRegs;
    }
    SceneUniverseAccumulateDynamicMaterialEvalStats(stats, material, regs, indexes, materialId);
}

PathTraceSmokeVertex BuildSceneUniverseStaticVertex(const idRenderEntityLocal* entity, const idMaterial* material, const srfTriangles_t* tri, int vertexIndex)
{
    const idDrawVert& drawVert = tri->verts[vertexIndex];
    idVec3 worldPosition;
    idVec3 worldNormal = drawVert.GetNormal();
    idVec3 worldTangent = drawVert.GetTangent();
    idVec3 worldBitangent = drawVert.GetBiTangent();
    const float bitangentSign = drawVert.GetBiTangentSign();
    if (entity)
    {
        R_LocalPointToGlobal(entity->modelMatrix, drawVert.xyz, worldPosition);
        R_LocalVectorToGlobal(entity->modelMatrix, worldNormal, worldNormal);
        R_LocalVectorToGlobal(entity->modelMatrix, worldTangent, worldTangent);
        R_LocalVectorToGlobal(entity->modelMatrix, worldBitangent, worldBitangent);
    }
    else
    {
        worldPosition = drawVert.xyz;
    }
    worldNormal.Normalize();
    if (worldTangent.Normalize() == 0.0f)
    {
        worldTangent.Set(1.0f, 0.0f, 0.0f);
    }
    if (worldBitangent.Normalize() == 0.0f)
    {
        worldBitangent.Cross(worldNormal, worldTangent);
        worldBitangent *= bitangentSign;
        worldBitangent.Normalize();
    }

    const idVec2 texCoord = drawVert.GetTexCoord();
    idVec2 normalTexCoord = texCoord;
    const float* materialRegisters = material ? material->ConstantRegisters() : nullptr;
    const int materialRegisterCount = material ? material->GetNumRegisters() : 0;
    if (material && materialRegisters)
    {
        for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
        {
            const shaderStage_t* stage = material->GetStage(stageIndex);
            if (!stage || stage->lighting != SL_BUMP || !stage->texture.hasMatrix)
            {
                continue;
            }

            float matrix[2][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
            for (int row = 0; row < 2; ++row)
            {
                for (int column = 0; column < 3; ++column)
                {
                    SceneUniverseEvalRegister(
                        materialRegisters,
                        materialRegisterCount,
                        stage->texture.matrix[row][column],
                        row == column ? 1.0f : 0.0f,
                        matrix[row][column]);
                }
            }
            normalTexCoord.Set(
                matrix[0][0] * texCoord.x + matrix[0][1] * texCoord.y + matrix[0][2],
                matrix[1][0] * texCoord.x + matrix[1][1] * texCoord.y + matrix[1][2]);
            break;
        }
    }
    PathTraceSmokeVertex vertex = {};
    vertex.position[0] = worldPosition.x;
    vertex.position[1] = worldPosition.y;
    vertex.position[2] = worldPosition.z;
    vertex.position[3] = 1.0f;
    vertex.normal[0] = worldNormal.x;
    vertex.normal[1] = worldNormal.y;
    vertex.normal[2] = worldNormal.z;
    vertex.normal[3] = 0.0f;
    vertex.texCoord[0] = texCoord.x;
    vertex.texCoord[1] = texCoord.y;
    vertex.texCoord[2] = normalTexCoord.x;
    vertex.texCoord[3] = normalTexCoord.y;
    vertex.color[0] = drawVert.color[0] * (1.0f / 255.0f);
    vertex.color[1] = drawVert.color[1] * (1.0f / 255.0f);
    vertex.color[2] = drawVert.color[2] * (1.0f / 255.0f);
    vertex.color[3] = drawVert.color[3] * (1.0f / 255.0f);
    vertex.color2[0] = drawVert.color2[0] * (1.0f / 255.0f);
    vertex.color2[1] = drawVert.color2[1] * (1.0f / 255.0f);
    vertex.color2[2] = drawVert.color2[2] * (1.0f / 255.0f);
    vertex.color2[3] = drawVert.color2[3] * (1.0f / 255.0f);
    vertex.tangent[0] = worldTangent.x;
    vertex.tangent[1] = worldTangent.y;
    vertex.tangent[2] = worldTangent.z;
    vertex.tangent[3] = bitangentSign;
    vertex.bitangent[0] = worldBitangent.x;
    vertex.bitangent[1] = worldBitangent.y;
    vertex.bitangent[2] = worldBitangent.z;
    vertex.bitangent[3] = 0.0f;
    return vertex;
}

int AppendSceneUniverseStaticSurfaceGeometry(
    const idRenderEntityLocal* entity,
    const idMaterial* material,
    const srfTriangles_t* tri,
    uint32_t surfaceClassId,
    uint32_t materialId,
    bool emissiveCapable,
    std::vector<PathTraceSmokeVertex>& vertices,
    std::vector<uint32_t>& indexes,
    std::vector<uint32_t>& triangleClasses,
    std::vector<uint32_t>& triangleMaterials,
    RtSmokeSurfaceSkipStats& skipStats,
    RtSmokeAttributeStats& attributeStats)
{
    const size_t vertexStart = vertices.size();
    const size_t indexStart = indexes.size();
    const size_t classStart = triangleClasses.size();
    const size_t materialStart = triangleMaterials.size();
    const uint32_t indexBase = static_cast<uint32_t>(vertices.size());
    const int classIndex = idMath::ClampInt(0, RT_SMOKE_CLASS_COUNT - 1, static_cast<int>(surfaceClassId & RT_SMOKE_TRIANGLE_CLASS_MASK));
    const uint32_t perSurfaceTriangleFlags = emissiveCapable ? 0u : RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF;

    for (int vertexIndex = 0; vertexIndex < tri->numVerts; ++vertexIndex)
    {
        PathTraceSmokeVertex vertex = BuildSceneUniverseStaticVertex(entity, material, tri, vertexIndex);
        const idVec3 normal = SmokeVertexNormal(vertex);
        const idVec2 texCoord = SmokeVertexTexCoord(vertex);
        if (!SmokeNormalIsUsable(normal))
        {
            ++attributeStats.classes[classIndex].invalidNormalVerts;
            vertex.normal[0] = 0.0f;
            vertex.normal[1] = 0.0f;
            vertex.normal[2] = 0.0f;
        }
        if (!SmokeTexCoordIsUsable(texCoord))
        {
            ++attributeStats.classes[classIndex].invalidUvVerts;
            vertex.texCoord[0] = 0.0f;
            vertex.texCoord[1] = 0.0f;
        }
        vertices.push_back(vertex);
    }

    for (int sourceIndex = 0; sourceIndex + 2 < tri->numIndexes; sourceIndex += 3)
    {
        const int i0 = tri->indexes[sourceIndex + 0];
        const int i1 = tri->indexes[sourceIndex + 1];
        const int i2 = tri->indexes[sourceIndex + 2];
        if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= tri->numVerts || i1 >= tri->numVerts || i2 >= tri->numVerts)
        {
            ++skipStats.invalidIndexCount;
            continue;
        }

        const PathTraceSmokeVertex& v0 = vertices[indexBase + static_cast<uint32_t>(i0)];
        const PathTraceSmokeVertex& v1 = vertices[indexBase + static_cast<uint32_t>(i1)];
        const PathTraceSmokeVertex& v2 = vertices[indexBase + static_cast<uint32_t>(i2)];
        const idVec3 p0 = SmokeVertexPosition(v0);
        const idVec3 p1 = SmokeVertexPosition(v1);
        const idVec3 p2 = SmokeVertexPosition(v2);
        if (IsZeroAreaSmokeTriangle(p0, p1, p2))
        {
            continue;
        }

        indexes.push_back(indexBase + static_cast<uint32_t>(i0));
        indexes.push_back(indexBase + static_cast<uint32_t>(i1));
        indexes.push_back(indexBase + static_cast<uint32_t>(i2));
        const bool invalidNormalTriangle =
            !SmokeNormalIsUsable(SmokeVertexNormal(v0)) ||
            !SmokeNormalIsUsable(SmokeVertexNormal(v1)) ||
            !SmokeNormalIsUsable(SmokeVertexNormal(v2));
        const bool invalidUvTriangle =
            !SmokeTexCoordIsUsable(SmokeVertexTexCoord(v0)) ||
            !SmokeTexCoordIsUsable(SmokeVertexTexCoord(v1)) ||
            !SmokeTexCoordIsUsable(SmokeVertexTexCoord(v2));
        if (invalidNormalTriangle)
        {
            ++attributeStats.classes[classIndex].invalidNormalTriangles;
        }
        if (invalidUvTriangle)
        {
            ++attributeStats.classes[classIndex].invalidUvTriangles;
        }
        if (invalidNormalTriangle)
        {
            ++attributeStats.classes[classIndex].forcedGeometricNormalTriangles;
        }

        triangleClasses.push_back(surfaceClassId | perSurfaceTriangleFlags | (invalidNormalTriangle ? RT_SMOKE_TRIANGLE_FORCE_GEOMETRIC_NORMAL : 0u));
        triangleMaterials.push_back(materialId);
    }

    const int emittedIndexes = static_cast<int>(indexes.size() - indexStart);
    if (emittedIndexes <= 0 || triangleClasses.size() == classStart || triangleMaterials.size() == materialStart)
    {
        vertices.resize(vertexStart);
        indexes.resize(indexStart);
        triangleClasses.resize(classStart);
        triangleMaterials.resize(materialStart);
        ++skipStats.zeroAreaOnly;
        return 0;
    }

    return emittedIndexes;
}

void RtPathTraceSceneUniverse::Clear()
{
    m_renderWorld = nullptr;
    m_renderWorldMapName.Clear();
    m_renderWorldMapTimeStamp = 0;
    m_stats = RtPathTraceSceneUniverseStats();
    m_surfaces.clear();
    m_areaStats.clear();
    m_areaSurfaceIndices.clear();
    m_surfaceSelectionStamps.clear();
    m_surfaceSelectionStamp = 0;
    m_fullStaticGeometryGeneration = 0;
    m_fullStaticGeometryRigidMode = 0;
    m_fullStaticBucketGeometryGeneration = 0;
    ++m_generation;
}

bool RtPathTraceSceneUniverse::EnsureBuilt(const viewDef_t* viewDef)
{
    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    if (!renderWorld)
    {
        Clear();
        return false;
    }

    const bool renderWorldChanged = m_renderWorld != renderWorld;
    const bool mapChanged = m_renderWorldMapName.Icmp(renderWorld->mapName) != 0 || m_renderWorldMapTimeStamp != renderWorld->mapTimeStamp;
    // This universe contains authored static-world surfaces only. Runtime
    // entity spawns and removals change entityDefs.Num(), but they do not
    // change that source geometry. Treating the broad entity count as a world
    // shape token forced a full resident bucket rebuild during ordinary map
    // traversal. Map identity/timestamp and the portal-area topology remain
    // the actual invalidation contract for this immutable source.
    const bool worldShapeChanged =
        m_stats.valid &&
        m_areaSurfaceIndices.size() !=
            static_cast<size_t>(renderWorld->NumAreas());
    if (renderWorldChanged || mapChanged || worldShapeChanged)
    {
        Clear();
        m_renderWorld = renderWorld;
        m_renderWorldMapName = renderWorld->mapName;
        m_renderWorldMapTimeStamp = renderWorld->mapTimeStamp;
    }

    if (m_stats.valid)
    {
        return true;
    }

    return Build(viewDef);
}

bool RtPathTraceSceneUniverse::Build(const viewDef_t* viewDef)
{
    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    if (!renderWorld)
    {
        return false;
    }

    m_stats = RtPathTraceSceneUniverseStats();
    m_surfaces.clear();
    m_areaStats.clear();
    m_areaSurfaceIndices.clear();
    m_surfaceSelectionStamps.clear();
    m_surfaceSelectionStamp = 0;
    m_stats.valid = true;
    m_stats.generation = m_generation;
    m_stats.mapName = renderWorld->mapName;
    m_stats.entityDefs = renderWorld->entityDefs.Num();
    m_stats.bounds.Clear();
    const int buildAreaCount = renderWorld->NumAreas();
    if (buildAreaCount > 0)
    {
        m_areaSurfaceIndices.resize(buildAreaCount);
    }

    std::unordered_map<uint32_t, bool> uniqueMaterials;
    std::unordered_map<uint32_t, bool> emissiveMaterials;
    uniqueMaterials.reserve(512);
    emissiveMaterials.reserve(128);

    for (int entityIndex = 0; entityIndex < renderWorld->entityDefs.Num(); ++entityIndex)
    {
        const idRenderEntityLocal* entity = renderWorld->entityDefs[entityIndex];
        const idRenderModel* model = entity ? entity->parms.hModel : nullptr;
        if (!model || !model->IsStaticWorldModel())
        {
            continue;
        }

        ++m_stats.staticWorldEntities;
        const int staticWorldPortalArea =
            SceneUniverseStaticWorldPortalArea(model, buildAreaCount);
        for (int surfaceIndex = 0; surfaceIndex < model->NumSurfaces(); ++surfaceIndex)
        {
            const modelSurface_t* modelSurface = model->Surface(surfaceIndex);
            const idMaterial* material = SceneUniverseResolveEntitySurfaceMaterial(entity, modelSurface);
            const srfTriangles_t* tri = modelSurface ? modelSurface->geometry : nullptr;
            if (!material || !tri || tri->numIndexes < 3)
            {
                continue;
            }

            RtPathTraceSceneUniverseSurface surface;
            surface.entity = entity;
            surface.material = material;
            surface.tri = tri;
            surface.entityIndex = entityIndex;
            surface.surfaceIndex = surfaceIndex;
            surface.baseMaterialId = SmokeMaterialId(material);
            surface.surfaceClassId = SmokeSurfaceClassAndSubtypeId(RtSmokeSurfaceClass::StaticWorld, RtSmokeTranslucentSubtype::Unknown);
            surface.materialId = surface.baseMaterialId;
            surface.numVerts = tri->numVerts;
            surface.numIndexes = tri->numIndexes;
            surface.triangles = tri->numIndexes / 3;
            surface.portalArea = staticWorldPortalArea;
            surface.key = BuildSceneUniverseSurfaceKey(model, entityIndex, surfaceIndex, material, tri);
            surface.legacyDrawSurfKey = BuildSceneUniverseLegacyDrawSurfKey(entity, material, tri);
            surface.bounds = SceneUniverseSurfaceWorldBounds(entity, tri);
            surface.emissiveCapable = SceneUniverseMaterialCanEmit(material);
            surface.validStaticBuild =
                tri->verts != nullptr &&
                tri->indexes != nullptr &&
                tri->numVerts >= 3 &&
                tri->numIndexes >= 3 &&
                (tri->numIndexes % 3) == 0 &&
                material->IsDrawn();
            surface.modelName = model->Name();
            surface.materialName = material->GetName();

            uniqueMaterials.emplace(surface.materialId, true);
            if (surface.emissiveCapable)
            {
                emissiveMaterials.emplace(surface.materialId, true);
            }

            if (!surface.bounds.IsCleared())
            {
                m_stats.bounds.AddBounds(surface.bounds);
                const idVec3 center = surface.bounds.GetCenter();
                surface.centerArea = renderWorld->PointInArea(center);
                const idVec3 offCenter = center + (surface.bounds[1] - center) * 0.125f;
                surface.offCenterArea = renderWorld->PointInArea(offCenter);
                surface.areaBoundsSkipped = !SceneUniverseBoundsSafeForAreaQuery(surface.bounds);
                if (!surface.areaBoundsSkipped)
                {
                    surface.areaCount = renderWorld->BoundsInAreas(surface.bounds, surface.areas, PT_SCENE_UNIVERSE_MAX_AREA_REFS);
                }
                else
                {
                    ++m_stats.boundsAreaSkippedSurfaces;
                }

                if (surface.areaCount == 0 && surface.centerArea >= 0)
                {
                    surface.areas[0] = surface.centerArea;
                    surface.areaCount = 1;
                }
                if (surface.areaCount == 0 && surface.offCenterArea >= 0)
                {
                    surface.areas[0] = surface.offCenterArea;
                    surface.areaCount = 1;
                }
            }

            if (surface.centerArea >= 0)
            {
                ++m_stats.centerAreaSurfaces;
            }
            if (surface.offCenterArea >= 0)
            {
                ++m_stats.offCenterAreaSurfaces;
            }
            if (surface.areaCount > 0)
            {
                ++m_stats.assignedSurfaces;
                m_stats.totalAreaRefs += surface.areaCount;
                if (surface.areaCount > 1)
                {
                    ++m_stats.multiAreaSurfaces;
                }
                for (int areaIndex = 0; areaIndex < surface.areaCount; ++areaIndex)
                {
                    const int area = surface.areas[areaIndex];
                    const int portals = area >= 0 ? renderWorld->NumPortalsInArea(area) : 0;
                    surface.portalCount += portals;
                    SceneUniverseAddAreaStat(m_areaStats, area, surface.triangles, surface.emissiveCapable, portals);
                }
                m_stats.totalPortalsReferenced += surface.portalCount;
            }
            else
            {
                ++m_stats.unassignedSurfaces;
            }

            ++m_stats.staticSurfaces;
            m_stats.staticTriangles += surface.triangles;
            if (surface.emissiveCapable)
            {
                ++m_stats.emissiveCapableSurfaces;
                m_stats.emissiveCapableTriangles += surface.triangles;
            }
            const int sceneSurfaceIndex = static_cast<int>(m_surfaces.size());
            for (int areaIndex = 0; areaIndex < surface.areaCount; ++areaIndex)
            {
                const int area = surface.areas[areaIndex];
                if (area >= 0 && area < static_cast<int>(m_areaSurfaceIndices.size()))
                {
                    m_areaSurfaceIndices[area].push_back(sceneSurfaceIndex);
                }
            }
            m_surfaces.push_back(surface);
        }
    }
    m_surfaceSelectionStamps.assign(m_surfaces.size(), 0);

    std::sort(m_areaStats.begin(), m_areaStats.end(),
        [](const RtPathTraceSceneUniverseAreaStats& lhs, const RtPathTraceSceneUniverseAreaStats& rhs)
        {
            if (lhs.surfaces != rhs.surfaces)
            {
                return lhs.surfaces > rhs.surfaces;
            }
            return lhs.area < rhs.area;
        });

    m_stats.uniqueMaterials = static_cast<int>(uniqueMaterials.size());
    m_stats.emissiveCapableMaterials = static_cast<int>(emissiveMaterials.size());
    m_stats.distinctAreas = static_cast<int>(m_areaStats.size());
    m_stats.generation = m_generation;
    return true;
}

RtPathTraceSceneUniverseBuildStats RtPathTraceSceneUniverse::BuildFullStaticGeometry(
    const viewDef_t* viewDef,
    RtSmokeGeometryUniverse& geometryUniverse,
    RtSmokeSurfaceClassStats& classStats,
    RtSmokeSurfaceSkipStats& skipStats,
    RtSmokeAttributeStats& attributeStats,
    RtSmokeMaterialStats& materialStats,
    RtSmokeBucketRanges& bucketRanges)
{
    RtPathTraceSceneUniverseBuildStats stats =
        BuildFullStaticGeometryInternal(
        viewDef,
        geometryUniverse,
        classStats,
        skipStats,
        attributeStats,
        materialStats,
        bucketRanges,
        false);
    UpdateSmokeStaticGeometryAdmissionTotals(
        skipStats, geometryUniverse);
    return stats;
}

RtPathTraceSceneUniverseBuildStats RtPathTraceSceneUniverse::BuildFullStaticBucketGeometry(
    const viewDef_t* viewDef,
    RtSmokeGeometryUniverse& geometryUniverse,
    RtSmokeSurfaceClassStats& classStats,
    RtSmokeSurfaceSkipStats& skipStats,
    RtSmokeAttributeStats& attributeStats,
    RtSmokeMaterialStats& materialStats,
    RtSmokeBucketRanges& bucketRanges)
{
    RtPathTraceSceneUniverseBuildStats stats =
        BuildFullStaticGeometryInternal(
        viewDef,
        geometryUniverse,
        classStats,
        skipStats,
        attributeStats,
        materialStats,
        bucketRanges,
        true);
    UpdateSmokeStaticGeometryAdmissionTotals(
        skipStats, geometryUniverse);
    return stats;
}

RtPathTraceSceneUniverseBuildStats RtPathTraceSceneUniverse::BuildFullStaticGeometryInternal(
    const viewDef_t* viewDef,
    RtSmokeGeometryUniverse& geometryUniverse,
    RtSmokeSurfaceClassStats& classStats,
    RtSmokeSurfaceSkipStats& skipStats,
    RtSmokeAttributeStats& attributeStats,
    RtSmokeMaterialStats& materialStats,
    RtSmokeBucketRanges& bucketRanges,
    bool staticWorldOnly)
{
    RtPathTraceSceneUniverseBuildStats buildStats;
    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    if (!renderWorld || !EnsureBuilt(viewDef))
    {
        return buildStats;
    }

    std::vector<PathTraceSmokeVertex>& staticVertices = geometryUniverse.StaticVertices();
    std::vector<uint32_t>& staticIndexes = geometryUniverse.StaticIndexes();
    std::vector<uint32_t>& staticTriangleClasses = geometryUniverse.StaticTriangleClasses();
    std::vector<uint32_t>& staticTriangleMaterials = geometryUniverse.StaticTriangleMaterials();
    const RtSmokeGeometryAdmissionBudget staticAdmissionBudget =
        BuildSmokeStaticGeometryAdmissionBudget();
    const int rigidEntityMode =
        staticWorldOnly
            ? 0
            : (r_pathTracingSceneSource.GetInteger() == 2
                ? idMath::ClampInt(0, 2, r_pathTracingSceneSource2RigidEntities.GetInteger())
                : 0);
    const bool includeRigidEntities = rigidEntityMode > 0;
    uint64& cachedGeometryGeneration =
        staticWorldOnly
            ? m_fullStaticBucketGeometryGeneration
            : m_fullStaticGeometryGeneration;
    const bool canTouchCachedBuild =
        cachedGeometryGeneration == m_generation &&
        (staticWorldOnly || m_fullStaticGeometryRigidMode == rigidEntityMode) &&
        !geometryUniverse.StaticSurfaceRecords().empty() &&
        !staticVertices.empty() &&
        !staticIndexes.empty() &&
        !staticTriangleClasses.empty() &&
        !staticTriangleMaterials.empty();

    buildStats.built = true;
    buildStats.cacheHit = canTouchCachedBuild;

    if (staticWorldOnly && canTouchCachedBuild)
    {
        // The dedicated bucket universe owns immutable full-map world
        // geometry. Once its scene generation and storage agree, the generic
        // render-world/material enumeration only rediscovers the same records
        // and re-registers the same stable material variant IDs. Retain the
        // lifecycle touch, but derive counts directly from the resident
        // records and leave portal activity to the assignment plan below.
        for (const RtSmokePersistentStaticSurfaceRecord& resident :
            geometryUniverse.StaticSurfaceRecords())
        {
            if (!resident.valid)
            {
                ++buildStats.skippedInvalid;
                continue;
            }

            RtSmokePersistentStaticSurfaceRecord* record =
                geometryUniverse.TouchStaticSurface(resident.key);
            if (!record)
            {
                ++buildStats.skippedInvalid;
                continue;
            }

            ++buildStats.surfaces;
            buildStats.vertices +=
                record->currentRange.vertices.count;
            buildStats.indexes +=
                record->currentRange.indexes.count;
            buildStats.triangles +=
                record->currentRange.triangles.count;
            ++classStats.staticWorldSurfaces;
            classStats.staticWorldVerts +=
                record->currentRange.vertices.count;
            classStats.staticWorldIndexes +=
                record->currentRange.indexes.count;
            classStats.staticWorldTriangles +=
                record->currentRange.triangles.count;
        }

        buildStats.emissiveCapableSurfaces =
            m_stats.emissiveCapableSurfaces;
        buildStats.frameFastPath = true;
        RtSmokeBucketRange& staticRange =
            bucketRanges.buckets[0];
        staticRange.vertexOffset = 0;
        staticRange.indexOffset = 0;
        staticRange.triangleOffset = 0;
        staticRange.vertexCount =
            static_cast<int>(staticVertices.size());
        staticRange.indexCount =
            static_cast<int>(staticIndexes.size());
        staticRange.triangleCount =
            static_cast<int>(staticTriangleClasses.size());
        staticRange.surfaceCount = buildStats.surfaces;
        return buildStats;
    }

    geometryUniverse.ReserveStaticSurfaceRecords(m_surfaces.size());

    for (int entityIndex = 0; entityIndex < renderWorld->entityDefs.Num(); ++entityIndex)
    {
        const idRenderEntityLocal* entity = renderWorld->entityDefs[entityIndex];
        const idRenderModel* model = entity ? entity->parms.hModel : nullptr;
        const bool isStaticWorldModel = model && model->IsStaticWorldModel();
        const int staticWorldPortalArea = isStaticWorldModel
            ? SceneUniverseStaticWorldPortalArea(
                model,
                renderWorld->NumAreas())
            : RT_SMOKE_STATIC_BUCKET_FALLBACK_AREA;
        const bool rigidEntityEligible = !isStaticWorldModel && includeRigidEntities && SceneUniverseRigidEntityEligible(entity, model);
        const bool isRigidEntityModel = rigidEntityEligible && (rigidEntityMode == 2 || SceneUniverseEntityHasEmissiveSurface(entity, model));
        if (!model || (!isStaticWorldModel && !isRigidEntityModel))
        {
            continue;
        }

        for (int surfaceIndex = 0; surfaceIndex < model->NumSurfaces(); ++surfaceIndex)
        {
            const modelSurface_t* modelSurface = model->Surface(surfaceIndex);
            const idMaterial* material = SceneUniverseResolveEntitySurfaceMaterial(entity, modelSurface);
            const srfTriangles_t* tri = modelSurface ? modelSurface->geometry : nullptr;
            if (!material || !tri || !tri->verts || !tri->indexes || tri->numVerts < 3 || tri->numIndexes < 3 || !material->IsDrawn())
            {
                ++buildStats.skippedInvalid;
                continue;
            }
            if ((tri->numIndexes % 3) != 0)
            {
                ++skipStats.invalidIndexCount;
                ++buildStats.skippedInvalid;
                continue;
            }
            if (UnifiedPtDiagnosticRemovesAlphaClipSurface(material))
            {
                ++skipStats.alphaClipDiagnostic;
                continue;
            }

            const uint64 key = BuildSceneUniverseLegacyDrawSurfKey(entity, material, tri);
            const uint64 bucketSurfaceKey =
                BuildSceneUniverseSurfaceKey(
                    model,
                    entityIndex,
                    surfaceIndex,
                    material,
                    tri);
            const RtSmokeSurfaceClass surfaceClass = isRigidEntityModel ? RtSmokeSurfaceClass::RigidEntity : RtSmokeSurfaceClass::StaticWorld;
            const uint32_t surfaceClassId = SmokeSurfaceClassAndSubtypeId(surfaceClass, RtSmokeTranslucentSubtype::Unknown);
            const uint32_t baseMaterialId = SmokeMaterialId(material);
            const uint32_t materialId = SmokeRuntimeMaterialTableIdForEntitySurface(entity, surfaceIndex, material, baseMaterialId);
            const bool emissiveCapable = SceneUniverseMaterialCanEmit(material);
            if (emissiveCapable)
            {
                ++buildStats.emissiveCapableSurfaces;
            }

            if (canTouchCachedBuild)
            {
                RtSmokePersistentStaticSurfaceRecord* record = geometryUniverse.TouchStaticSurface(key);
                if (!record)
                {
                    ++buildStats.skippedInvalid;
                    continue;
                }
                geometryUniverse.RefreshStaticSurfaceMaterial(key, materialId);
                geometryUniverse.RefreshStaticSurfacePortalArea(
                    key,
                    staticWorldPortalArea);
                geometryUniverse.RefreshStaticSurfaceBucketKey(
                    key,
                    bucketSurfaceKey);

                ++bucketRanges.buckets[0].surfaceCount;
                ++buildStats.surfaces;
                buildStats.vertices += record->currentRange.vertices.count;
                buildStats.indexes += record->currentRange.indexes.count;
                buildStats.triangles += record->currentRange.triangles.count;
                if (isRigidEntityModel)
                {
                    ++buildStats.rigidEntitySurfaces;
                    buildStats.rigidEntityTriangles += record->currentRange.triangles.count;
                    ++classStats.rigidEntitySurfaces;
                    classStats.rigidEntityVerts += record->currentRange.vertices.count;
                    classStats.rigidEntityIndexes += record->currentRange.indexes.count;
                    classStats.rigidEntityTriangles += record->currentRange.triangles.count;
                }
                else
                {
                    ++classStats.staticWorldSurfaces;
                    classStats.staticWorldVerts += record->currentRange.vertices.count;
                    classStats.staticWorldIndexes += record->currentRange.indexes.count;
                    classStats.staticWorldTriangles += record->currentRange.triangles.count;
                }
                SceneUniverseAddSmokeMaterialStats(materialStats, material, record->currentRange.indexes.count);
                SceneUniverseAddDynamicMaterialEvalStatsForId(materialStats, viewDef, entity, material, record->currentRange.indexes.count, materialId);
                continue;
            }

            const RtSmokeGeometryAdmissionPlan admissionPlan =
                PlanSmokeStaticGeometryAdmission(
                    staticAdmissionBudget,
                    geometryUniverse,
                    tri->numVerts,
                    tri->numIndexes);
            if (!admissionPlan.Admitted())
            {
                RecordSmokeStaticGeometryAdmissionRejection(
                    skipStats, admissionPlan);
                ++buildStats.skippedLimits;
                continue;
            }

            const RtSmokeStaticSurfaceAppend append =
                geometryUniverse.BeginStaticSurfaceAppend(
                    key,
                    surfaceClassId,
                    materialId,
                    tri->numVerts,
                    tri->numIndexes,
                    staticWorldPortalArea,
                    bucketSurfaceKey);
            const int emittedIndexes = AppendSceneUniverseStaticSurfaceGeometry(
                entity,
                material,
                tri,
                surfaceClassId,
                materialId,
                emissiveCapable,
                staticVertices,
                staticIndexes,
                staticTriangleClasses,
                staticTriangleMaterials,
                skipStats,
                attributeStats);
            if (emittedIndexes <= 0)
            {
                ++buildStats.skippedZeroArea;
                continue;
            }

            ApplySmokeDetailDecalNormalOffset(
                material,
                key,
                !isRigidEntityModel,
                staticVertices,
                staticIndexes,
                static_cast<size_t>(append.vertexOffset),
                static_cast<size_t>(append.indexOffset));

            geometryUniverse.CompleteStaticSurfaceAppend(append, emittedIndexes);
            ++bucketRanges.buckets[0].surfaceCount;
            ++buildStats.surfaces;
            buildStats.vertices += tri->numVerts;
            buildStats.indexes += emittedIndexes;
            buildStats.triangles += emittedIndexes / 3;
            if (isRigidEntityModel)
            {
                ++buildStats.rigidEntitySurfaces;
                buildStats.rigidEntityTriangles += emittedIndexes / 3;
                ++classStats.rigidEntitySurfaces;
                classStats.rigidEntityVerts += tri->numVerts;
                classStats.rigidEntityIndexes += emittedIndexes;
                classStats.rigidEntityTriangles += emittedIndexes / 3;
            }
            else
            {
                ++classStats.staticWorldSurfaces;
                classStats.staticWorldVerts += tri->numVerts;
                classStats.staticWorldIndexes += emittedIndexes;
                classStats.staticWorldTriangles += emittedIndexes / 3;
            }
            SceneUniverseAddSmokeMaterialStats(materialStats, material, emittedIndexes);
            SceneUniverseAddDynamicMaterialEvalStatsForId(materialStats, viewDef, entity, material, emittedIndexes, materialId);
        }
    }

    RtSmokeBucketRange& staticRange = bucketRanges.buckets[0];
    staticRange.vertexOffset = 0;
    staticRange.indexOffset = 0;
    staticRange.triangleOffset = 0;
    staticRange.vertexCount = static_cast<int>(staticVertices.size());
    staticRange.indexCount = static_cast<int>(staticIndexes.size());
    staticRange.triangleCount = static_cast<int>(staticTriangleClasses.size());

    if (!canTouchCachedBuild)
    {
        cachedGeometryGeneration = m_generation;
        if (!staticWorldOnly)
        {
            m_fullStaticGeometryRigidMode = rigidEntityMode;
        }
    }
    return buildStats;
}

bool RtPathTraceSceneUniverse::BuildPortalAreaActiveMask(
    const viewDef_t* viewDef,
    int portalSteps,
    std::vector<bool>& selectedAreas,
    int* frontendVisibleAreaCount,
    int* selectedAreaCount)
{
    selectedAreas.clear();
    if (frontendVisibleAreaCount)
    {
        *frontendVisibleAreaCount = 0;
    }
    if (selectedAreaCount)
    {
        *selectedAreaCount = 0;
    }
    idRenderWorldLocal* renderWorld =
        viewDef ? viewDef->renderWorld : nullptr;
    if (!renderWorld || !EnsureBuilt(viewDef))
    {
        return false;
    }

    const bool bruteForceFullMap =
        r_pathTracingPortalBruteforceFullMap.GetInteger() != 0;
    const int areaCount = renderWorld->NumAreas();
    if (areaCount <= 0 ||
        (!bruteForceFullMap &&
            (!viewDef->pathTraceVisibleAreas ||
                viewDef->pathTraceVisibleAreaCount != areaCount)))
    {
        return false;
    }

    std::vector<bool> frontendVisibleAreas(areaCount, false);
    if (viewDef->pathTraceVisibleAreas &&
        viewDef->pathTraceVisibleAreaCount == areaCount)
    {
        for (int area = 0; area < areaCount; ++area)
        {
            frontendVisibleAreas[area] =
                viewDef->pathTraceVisibleAreas[area];
        }
    }

    std::vector<RtSmokePortalAreaEdge> portalEdges;
    if (!bruteForceFullMap && portalSteps > 0)
    {
        for (int area = 0; area < areaCount; ++area)
        {
            const int portalCount =
                renderWorld->NumPortalsInArea(area);
            for (int portalIndex = 0;
                 portalIndex < portalCount;
                 ++portalIndex)
            {
                const exitPortal_t portal =
                    renderWorld->GetPortal(area, portalIndex);
                int nextArea = -1;
                if (portal.areas[0] == area)
                {
                    nextArea = portal.areas[1];
                }
                else if (portal.areas[1] == area)
                {
                    nextArea = portal.areas[0];
                }
                if (nextArea <= area || nextArea >= areaCount)
                {
                    continue;
                }
                RtSmokePortalAreaEdge edge;
                edge.areaA = area;
                edge.areaB = nextArea;
                portalEdges.push_back(edge);
            }
        }
    }

    const RtSmokePortalVisibilityMaskPlan plan =
        BuildSmokePortalVisibilityMaskPlan(
            areaCount,
            frontendVisibleAreas,
            portalEdges,
            idMath::ClampInt(0, 8, portalSteps),
            bruteForceFullMap);
    if (!plan.valid)
    {
        return false;
    }
    selectedAreas = plan.selectedAreas;
    if (frontendVisibleAreaCount)
    {
        *frontendVisibleAreaCount =
            plan.frontendVisibleAreas;
    }
    if (selectedAreaCount)
    {
        *selectedAreaCount =
            plan.selectedAreaCount;
    }
    return true;
}

RtPathTraceSceneUniverseBuildStats RtPathTraceSceneUniverse::BuildSelectedStaticGeometry(
    const viewDef_t* viewDef,
    RtSmokeGeometryUniverse& geometryUniverse,
    RtSmokeSurfaceClassStats& classStats,
    RtSmokeSurfaceSkipStats& skipStats,
    RtSmokeAttributeStats& attributeStats,
    RtSmokeMaterialStats& materialStats,
    RtSmokeBucketRanges& bucketRanges,
    int portalSteps)
{
    RtPathTraceSceneUniverseBuildStats buildStats;
    const RtSmokeGeometryAdmissionBudget staticAdmissionBudget =
        BuildSmokeStaticGeometryAdmissionBudget();
    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    if (!renderWorld || !EnsureBuilt(viewDef))
    {
        return buildStats;
    }

    const bool bruteForceFullMap = r_pathTracingPortalBruteforceFullMap.GetInteger() != 0;
    const bool useBakedStaticFacts =
        r_pathTracingResidency.GetInteger() != 0 &&
        r_pathTracingResidencyStatic.GetInteger() != 0;
    RtPathTraceSceneUniverseSelectionStats selection;
    {
        OPTICK_EVENT("PT Static Area Selection");
        selection = BuildSelectionStats(viewDef, idMath::ClampInt(0, 8, portalSteps), !useBakedStaticFacts);
    }
    if (!selection.valid)
    {
        return buildStats;
    }

    std::vector<bool> selectedAreas;
    {
        OPTICK_EVENT("PT Static Area Mask");
        selectedAreas.assign(renderWorld->NumAreas(), bruteForceFullMap);
        if (!bruteForceFullMap)
        {
            for (int areaListIndex = 0; areaListIndex < selection.selectedAreaListCount; ++areaListIndex)
            {
                const int area = selection.selectedAreaList[areaListIndex];
                if (area >= 0 && area < static_cast<int>(selectedAreas.size()))
                {
                    selectedAreas[area] = true;
                }
            }
        }
    }

    std::vector<PathTraceSmokeVertex>& staticVertices = geometryUniverse.StaticVertices();
    std::vector<uint32_t>& staticIndexes = geometryUniverse.StaticIndexes();
    std::vector<uint32_t>& staticTriangleClasses = geometryUniverse.StaticTriangleClasses();
    std::vector<uint32_t>& staticTriangleMaterials = geometryUniverse.StaticTriangleMaterials();

    buildStats.built = true;
    const size_t reserveSurfaceCount = useBakedStaticFacts ? m_surfaces.size() : static_cast<size_t>(selection.selectedSurfaces);
    {
        OPTICK_EVENT("PT Static Area Reserve Records");
        geometryUniverse.ReserveStaticSurfaceRecords(geometryUniverse.StaticSurfaceRecords().size() + reserveSurfaceCount);
    }

    auto processSelectedSurface = [&](const RtPathTraceSceneUniverseSurface& surface) -> void
    {
        const idRenderEntityLocal* entity = nullptr;
        const idMaterial* material = nullptr;
        const srfTriangles_t* tri = nullptr;
        uint64 key = 0;
        uint32_t surfaceClassId = 0;
        uint32_t materialId = 0;
        bool emissiveCapable = false;
        int numVerts = 0;
        int numIndexes = 0;

        if (useBakedStaticFacts)
        {
            entity = surface.entity;
            material = surface.material;
            key = surface.legacyDrawSurfKey;
            surfaceClassId = surface.surfaceClassId;
            materialId = surface.baseMaterialId;
            emissiveCapable = surface.emissiveCapable;
            numVerts = surface.numVerts;
            numIndexes = surface.numIndexes;

            if (!surface.validStaticBuild || !entity || !material || key == 0 || numVerts < 3 || numIndexes < 3)
            {
                ++buildStats.skippedInvalid;
                return;
            }
            if ((numIndexes % 3) != 0)
            {
                ++skipStats.invalidIndexCount;
                ++buildStats.skippedInvalid;
                return;
            }
            materialId = SmokeRuntimeMaterialTableIdForEntitySurface(entity, surface.surfaceIndex, material, surface.baseMaterialId);
        }
        else
        {
            if (surface.entityIndex < 0 || surface.entityIndex >= renderWorld->entityDefs.Num())
            {
                ++buildStats.skippedInvalid;
                return;
            }

            entity = renderWorld->entityDefs[surface.entityIndex];
            const idRenderModel* model = entity ? entity->parms.hModel : nullptr;
            if (!model || !model->IsStaticWorldModel() || surface.surfaceIndex < 0 || surface.surfaceIndex >= model->NumSurfaces())
            {
                ++buildStats.skippedInvalid;
                return;
            }

            const modelSurface_t* modelSurface = model->Surface(surface.surfaceIndex);
            material = SceneUniverseResolveEntitySurfaceMaterial(entity, modelSurface);
            tri = modelSurface ? modelSurface->geometry : nullptr;
            if (!material || !tri || !tri->verts || !tri->indexes || tri->numVerts < 3 || tri->numIndexes < 3 || !material->IsDrawn())
            {
                ++buildStats.skippedInvalid;
                return;
            }
            if ((tri->numIndexes % 3) != 0)
            {
                ++skipStats.invalidIndexCount;
                ++buildStats.skippedInvalid;
                return;
            }

            key = BuildSceneUniverseLegacyDrawSurfKey(entity, material, tri);
            surfaceClassId = SmokeSurfaceClassAndSubtypeId(RtSmokeSurfaceClass::StaticWorld, RtSmokeTranslucentSubtype::Unknown);
            const uint32_t baseMaterialId = SmokeMaterialId(material);
            materialId = SmokeRuntimeMaterialTableIdForEntitySurface(entity, surface.surfaceIndex, material, baseMaterialId);
            emissiveCapable = SceneUniverseMaterialCanEmit(material);
            numVerts = tri->numVerts;
            numIndexes = tri->numIndexes;
        }

        if (emissiveCapable)
        {
            ++buildStats.emissiveCapableSurfaces;
        }
        if (UnifiedPtDiagnosticRemovesAlphaClipSurface(material))
        {
            ++skipStats.alphaClipDiagnostic;
            return;
        }
        ++buildStats.residencyVisited;

        auto touchCachedStaticSurface = [&]() -> bool
        {
            const RtSmokePersistentStaticSurfaceRecord* existingRecord = geometryUniverse.FindStaticSurface(key);
            const bool alreadyCountedThisFrame = existingRecord && existingRecord->seenThisFrame;
            RtSmokePersistentStaticSurfaceRecord* record = geometryUniverse.TouchStaticSurface(key);
            if (!record)
            {
                return false;
            }

            geometryUniverse.RefreshStaticSurfaceMaterial(key, materialId);
            geometryUniverse.RefreshStaticSurfacePortalArea(
                key,
                surface.portalArea);
            geometryUniverse.RefreshStaticSurfaceBucketKey(
                key,
                surface.key);
            ++buildStats.residencyCacheHits;
            if (alreadyCountedThisFrame)
            {
                return true;
            }
            ++bucketRanges.buckets[0].surfaceCount;
            ++buildStats.surfaces;
            buildStats.vertices += record->currentRange.vertices.count;
            buildStats.indexes += record->currentRange.indexes.count;
            buildStats.triangles += record->currentRange.triangles.count;
            ++classStats.staticWorldSurfaces;
            classStats.staticWorldVerts += record->currentRange.vertices.count;
            classStats.staticWorldIndexes += record->currentRange.indexes.count;
            classStats.staticWorldTriangles += record->currentRange.triangles.count;
            SceneUniverseAddSmokeMaterialStats(materialStats, material, record->currentRange.indexes.count);
            SceneUniverseAddDynamicMaterialEvalStatsForId(materialStats, viewDef, entity, material, record->currentRange.indexes.count, materialId);
            return true;
        };

        if (touchCachedStaticSurface())
        {
            return;
        }

        if (useBakedStaticFacts)
        {
            if (surface.entityIndex < 0 || surface.entityIndex >= renderWorld->entityDefs.Num())
            {
                ++buildStats.skippedInvalid;
                return;
            }

            entity = renderWorld->entityDefs[surface.entityIndex];
            const idRenderModel* model = entity ? entity->parms.hModel : nullptr;
            if (!model || !model->IsStaticWorldModel() || surface.surfaceIndex < 0 || surface.surfaceIndex >= model->NumSurfaces())
            {
                ++buildStats.skippedInvalid;
                return;
            }

            const modelSurface_t* modelSurface = model->Surface(surface.surfaceIndex);
            material = SceneUniverseResolveEntitySurfaceMaterial(entity, modelSurface);
            tri = modelSurface ? modelSurface->geometry : nullptr;
            if (!material || !tri || !tri->verts || !tri->indexes || tri->numVerts < 3 || tri->numIndexes < 3 || !material->IsDrawn())
            {
                ++buildStats.skippedInvalid;
                return;
            }
            if ((tri->numIndexes % 3) != 0)
            {
                ++skipStats.invalidIndexCount;
                ++buildStats.skippedInvalid;
                return;
            }

            key = BuildSceneUniverseLegacyDrawSurfKey(entity, material, tri);
            if (key == 0)
            {
                ++buildStats.skippedInvalid;
                return;
            }
            surfaceClassId = SmokeSurfaceClassAndSubtypeId(RtSmokeSurfaceClass::StaticWorld, RtSmokeTranslucentSubtype::Unknown);
            const uint32_t baseMaterialId = SmokeMaterialId(material);
            materialId = SmokeRuntimeMaterialTableIdForEntitySurface(entity, surface.surfaceIndex, material, baseMaterialId);
            emissiveCapable = SceneUniverseMaterialCanEmit(material);
            numVerts = tri->numVerts;
            numIndexes = tri->numIndexes;

            if (touchCachedStaticSurface())
            {
                return;
            }
        }

        ++buildStats.residencyDerived;
        ++buildStats.residencyCacheMisses;

        const RtSmokeGeometryAdmissionPlan admissionPlan =
            PlanSmokeStaticGeometryAdmission(
                staticAdmissionBudget,
                geometryUniverse,
                numVerts,
                numIndexes);
        if (!admissionPlan.Admitted())
        {
            RecordSmokeStaticGeometryAdmissionRejection(
                skipStats, admissionPlan);
            ++buildStats.skippedLimits;
            return;
        }

        const RtSmokeStaticSurfaceAppend append =
            geometryUniverse.BeginStaticSurfaceAppend(
                key,
                surfaceClassId,
                materialId,
                numVerts,
                numIndexes,
                surface.portalArea,
                surface.key);
        const int emittedIndexes = AppendSceneUniverseStaticSurfaceGeometry(
            entity,
            material,
            tri,
            surfaceClassId,
            materialId,
            emissiveCapable,
            staticVertices,
            staticIndexes,
            staticTriangleClasses,
            staticTriangleMaterials,
            skipStats,
            attributeStats);
        if (emittedIndexes <= 0)
        {
            ++buildStats.skippedZeroArea;
            return;
        }

        ApplySmokeDetailDecalNormalOffset(
            material,
            key,
            true,
            staticVertices,
            staticIndexes,
            static_cast<size_t>(append.vertexOffset),
            static_cast<size_t>(append.indexOffset));

        geometryUniverse.CompleteStaticSurfaceAppend(append, emittedIndexes);
        ++bucketRanges.buckets[0].surfaceCount;
        ++buildStats.surfaces;
        buildStats.vertices += numVerts;
        buildStats.indexes += emittedIndexes;
        buildStats.triangles += emittedIndexes / 3;
        ++classStats.staticWorldSurfaces;
        classStats.staticWorldVerts += numVerts;
        classStats.staticWorldIndexes += emittedIndexes;
        classStats.staticWorldTriangles += emittedIndexes / 3;
        SceneUniverseAddSmokeMaterialStats(materialStats, material, emittedIndexes);
        SceneUniverseAddDynamicMaterialEvalStatsForId(materialStats, viewDef, entity, material, emittedIndexes, materialId);
    };

    if (useBakedStaticFacts)
    {
        OPTICK_EVENT("PT Static Area Resident Walk");
        if (m_surfaceSelectionStamps.size() != m_surfaces.size())
        {
            m_surfaceSelectionStamps.assign(m_surfaces.size(), 0);
        }
        ++m_surfaceSelectionStamp;
        if (m_surfaceSelectionStamp == 0)
        {
            std::fill(m_surfaceSelectionStamps.begin(), m_surfaceSelectionStamps.end(), 0);
            m_surfaceSelectionStamp = 1;
        }

        const int areaCount = Min(static_cast<int>(selectedAreas.size()), static_cast<int>(m_areaSurfaceIndices.size()));
        for (int area = 0; area < areaCount; ++area)
        {
            if (!selectedAreas[area])
            {
                continue;
            }

            for (int surfaceIndex : m_areaSurfaceIndices[area])
            {
                if (surfaceIndex < 0 || surfaceIndex >= static_cast<int>(m_surfaces.size()))
                {
                    continue;
                }
                if (m_surfaceSelectionStamps[surfaceIndex] == m_surfaceSelectionStamp)
                {
                    continue;
                }
                m_surfaceSelectionStamps[surfaceIndex] = m_surfaceSelectionStamp;

                const RtPathTraceSceneUniverseSurface& surface = m_surfaces[surfaceIndex];
                processSelectedSurface(surface);
            }
        }
    }
    else
    {
        OPTICK_EVENT("PT Static Area Legacy Walk");
        for (const RtPathTraceSceneUniverseSurface& surface : m_surfaces)
        {
            if (!SceneUniverseSurfaceTouchesSelectedArea(surface, selectedAreas))
            {
                continue;
            }
            processSelectedSurface(surface);
        }
    }

    RtSmokeBucketRange& staticRange = bucketRanges.buckets[0];
    staticRange.vertexOffset = 0;
    staticRange.indexOffset = 0;
    staticRange.triangleOffset = 0;
    staticRange.vertexCount = static_cast<int>(staticVertices.size());
    staticRange.indexCount = static_cast<int>(staticIndexes.size());
    staticRange.triangleCount = static_cast<int>(staticTriangleClasses.size());

    DumpSceneUniverseResidencyStatsIfNeeded(buildStats);

    UpdateSmokeStaticGeometryAdmissionTotals(
        skipStats, geometryUniverse);
    return buildStats;
}

RtPathTraceSceneUniverseSelectionStats RtPathTraceSceneUniverse::BuildSelectionStats(const viewDef_t* viewDef, int portalSteps, bool countSelectedSurfaces) const
{
    RtPathTraceSceneUniverseSelectionStats selection;
    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    if (!renderWorld || !m_stats.valid)
    {
        return selection;
    }

    const int areaCount = renderWorld->NumAreas();
    std::vector<int> seedAreas;
    auto addSeedArea = [&](const int area) {
        if (area < 0 || area >= areaCount)
        {
            return;
        }
        if (std::find(seedAreas.begin(), seedAreas.end(), area) == seedAreas.end())
        {
            seedAreas.push_back(area);
        }
    };

    addSeedArea(viewDef->areaNum);
    addSeedArea(renderWorld->PointInArea(viewDef->initialViewAreaOrigin));
    addSeedArea(renderWorld->PointInArea(viewDef->renderView.vieworg));

    const idVec3& viewOrigin = viewDef->renderView.vieworg;
    const float probeDistance = 8.0f;
    addSeedArea(renderWorld->PointInArea(viewOrigin + viewDef->renderView.viewaxis[0] * probeDistance));
    addSeedArea(renderWorld->PointInArea(viewOrigin - viewDef->renderView.viewaxis[0] * probeDistance));
    addSeedArea(renderWorld->PointInArea(viewOrigin + viewDef->renderView.viewaxis[1] * probeDistance));
    addSeedArea(renderWorld->PointInArea(viewOrigin - viewDef->renderView.viewaxis[1] * probeDistance));
    addSeedArea(renderWorld->PointInArea(viewOrigin + viewDef->renderView.viewaxis[2] * probeDistance));
    addSeedArea(renderWorld->PointInArea(viewOrigin - viewDef->renderView.viewaxis[2] * probeDistance));

    const bool bruteForceFullMap = r_pathTracingPortalBruteforceFullMap.GetInteger() != 0;
    if (seedAreas.empty() && !bruteForceFullMap)
    {
        return selection;
    }
    selection.valid = true;
    std::vector<bool> selectedAreas(areaCount, false);
    std::vector<int> selectedDepth(areaCount, -1);
    std::vector<int> queue;
    queue.reserve(areaCount);
    if (bruteForceFullMap)
    {
        std::fill(selectedAreas.begin(), selectedAreas.end(), true);
        std::fill(selectedDepth.begin(), selectedDepth.end(), 0);
        for (int areaIndex = 0; areaIndex < areaCount; ++areaIndex)
        {
            queue.push_back(areaIndex);
        }
    }
    else
    {
        for (int seedArea : seedAreas)
        {
            selectedAreas[seedArea] = true;
            selectedDepth[seedArea] = 0;
            queue.push_back(seedArea);
        }

        for (size_t queueIndex = 0; queueIndex < queue.size(); ++queueIndex)
        {
            const int area = queue[queueIndex];
            const int depth = selectedDepth[area];
            if (depth >= portalSteps)
            {
                continue;
            }

            const int portalCount = renderWorld->NumPortalsInArea(area);
            for (int portalIndex = 0; portalIndex < portalCount; ++portalIndex)
            {
                const exitPortal_t portal = renderWorld->GetPortal(area, portalIndex);
                int nextArea = -1;
                if (portal.areas[0] == area)
                {
                    nextArea = portal.areas[1];
                }
                else if (portal.areas[1] == area)
                {
                    nextArea = portal.areas[0];
                }
                if (nextArea < 0 || nextArea >= areaCount)
                {
                    continue;
                }

                if (!selectedAreas[nextArea])
                {
                    selectedAreas[nextArea] = true;
                    selectedDepth[nextArea] = depth + 1;
                    queue.push_back(nextArea);
                }
            }
        }
    }

    for (int area : queue)
    {
        if (selection.selectedAreaListCount < PT_SCENE_UNIVERSE_MAX_SELECTION_AREAS)
        {
            selection.selectedAreaList[selection.selectedAreaListCount++] = area;
        }
    }

    if (!countSelectedSurfaces)
    {
        return selection;
    }

    for (const RtPathTraceSceneUniverseSurface& surface : m_surfaces)
    {
        if (!SceneUniverseSurfaceTouchesSelectedArea(surface, selectedAreas))
        {
            continue;
        }

        ++selection.selectedSurfaces;
    }
    return selection;
}

const RtPathTraceSceneUniverseStats& RtPathTraceSceneUniverse::GetStats() const
{
    return m_stats;
}

const std::vector<RtPathTraceSceneUniverseSurface>& RtPathTraceSceneUniverse::Surfaces() const
{
    return m_surfaces;
}
