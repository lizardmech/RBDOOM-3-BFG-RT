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
#include "PathTraceMaterialClassifier.h"
#include "PathTraceMaterialUniverse.h"
#include "PathTraceMaterialTextureDiscovery.h"
#include "PathTracePrimaryPass.h"
#include "PathTraceRemixFramePrepare.h"
#include "PathTraceRemixLightManager.h"
#include "PathTraceRemixRtxdiResourceGate.h"
#include "PathTraceRemixRtxdiResources.h"
#include "PathTraceRestirLightManager.h"
#include "PathTraceRestirPasses.h"
#include "PathTraceSceneCapture.h"
#include "PathTraceSceneUniverse.h"
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
#include <cstring>
#include <future>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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

int g_smokeLastSceneTimingLogMs = -1000000;
uint64 g_smokeLastGeometryValidationDumpGeneration = 0;
int g_smokeLastGeometryValidationDumpErrors = 0;

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
    return (stage->lighting == SL_AMBIENT && dstBlend == GLS_DSTBLEND_ONE) ||
        (srcBlend == GLS_SRCBLEND_ONE && dstBlend == GLS_DSTBLEND_ONE);
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
    int validRecordCount = 0;
    std::vector<RtSmokeSpectrumLight> spectrumLights;
    bool spectrumLightsBuilt = false;

    for (const RtSmokeDynamicMaterialEvalSample& sample : materialStats.dynamicEvalMaterialSamples)
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
    const std::vector<PathTraceDynamicMaterialRecord>& records)
{
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
                if (!SmokeRuntimeMaterialStageIsEmissiveLike(stage))
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
            material.flags &= ~RT_SMOKE_MATERIAL_EMISSIVE;
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
            material.flags &= ~RT_SMOKE_MATERIAL_EMISSIVE;
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

    common->Printf("PathTracePrimaryPass: RT smoke material classifier live records=%d hits=%d misses=%d rebuilds=%d frame=%d/%d/%d frameRoutes(rmao/legacy/fallback)=%d/%d/%d frameConfidence(auth/flag/heur/fallback)=%d/%d/%d/%d\n",
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
        stats.confidenceFallbackNone);

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

uint32_t RestirSourceTypeFromUnifiedLightType(uint32_t type)
{
    if (type == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
    {
        return PATH_TRACE_RESTIR_LIGHT_SOURCE_EMISSIVE_TRIANGLE;
    }
    if (type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
    {
        return PATH_TRACE_RESTIR_LIGHT_SOURCE_DOOM_ANALYTIC;
    }
    return PATH_TRACE_RESTIR_LIGHT_SOURCE_INVALID;
}

const char* RemixLightUniverseContractStatusName(uint32_t status)
{
    switch (status)
    {
    case 0u:
        return "ok";
    case 1u:
        return "disabled";
    case 2u:
        return "no-current-domain";
    case 3u:
        return "map-size-mismatch";
    case 4u:
        return "duplicate-identity";
    default:
        return "unknown";
    }
}

std::vector<PathTraceRestirCurrentLightRecord> BuildRestirRecordsFromRemixCurrentLights(
    const std::vector<PathTraceUnifiedLightRecord>& records,
    const std::vector<uint32_t>& currentToPrevious)
{
    std::vector<PathTraceRestirCurrentLightRecord> result;
    result.reserve(records.size());
    for (size_t i = 0; i < records.size(); ++i)
    {
        const PathTraceUnifiedLightRecord& source = records[i];
        PathTraceRestirCurrentLightRecord record;
        record.sourceType = RestirSourceTypeFromUnifiedLightType(source.type);
        record.payloadSourceIndex = source.sourceIndex;
        record.identityKeyLo = source.identityA;
        record.identityKeyHi = source.identityB;
        record.flags = record.sourceType != PATH_TRACE_RESTIR_LIGHT_SOURCE_INVALID ? PATH_TRACE_RESTIR_LIGHT_RECORD_STABLE_IDENTITY : 0u;
        if (i < currentToPrevious.size() && currentToPrevious[i] != PATH_TRACE_REMIX_LIGHT_INVALID_INDEX)
        {
            record.flags |= PATH_TRACE_RESTIR_LIGHT_RECORD_REMAP_VALID;
        }
        else
        {
            record.flags |= PATH_TRACE_RESTIR_LIGHT_RECORD_CURRENT_ONLY;
        }
        result.push_back(record);
    }
    return result;
}

std::vector<PathTraceRestirPreviousLightRecord> BuildRestirRecordsFromRemixPreviousLights(
    const std::vector<PathTraceUnifiedLightRecord>& records,
    const std::vector<uint32_t>& previousToCurrent)
{
    std::vector<PathTraceRestirPreviousLightRecord> result;
    result.reserve(records.size());
    for (size_t i = 0; i < records.size(); ++i)
    {
        const PathTraceUnifiedLightRecord& source = records[i];
        PathTraceRestirPreviousLightRecord record;
        record.sourceType = RestirSourceTypeFromUnifiedLightType(source.type);
        record.payloadSourceIndex = source.sourceIndex;
        record.identityKeyLo = source.identityA;
        record.identityKeyHi = source.identityB;
        record.flags = record.sourceType != PATH_TRACE_RESTIR_LIGHT_SOURCE_INVALID ? PATH_TRACE_RESTIR_LIGHT_RECORD_STABLE_IDENTITY : 0u;
        if (i < previousToCurrent.size() && previousToCurrent[i] != PATH_TRACE_REMIX_LIGHT_INVALID_INDEX)
        {
            record.flags |= PATH_TRACE_RESTIR_LIGHT_RECORD_REMAP_VALID;
        }
        else
        {
            record.flags |= PATH_TRACE_RESTIR_LIGHT_RECORD_PREVIOUS_ONLY;
        }
        result.push_back(record);
    }
    return result;
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

struct RtSmokeMaterialUploadDiffSummary
{
    int rows = 0;
    int firstRow = -1;
    int lastRow = -1;
    int debugAlbedoRows = 0;
    int emissiveRows = 0;
    int textureRows = 0;
    int alphaRows = 0;
    int flagRows = 0;
    int classifierRows = 0;
    int otherRows = 0;
    int sampleCount = 0;
    int sampleRows[RT_SMOKE_UPLOAD_DIFF_SAMPLE_COUNT] = {};
    uint32_t sampleMaterialIds[RT_SMOKE_UPLOAD_DIFF_SAMPLE_COUNT] = {};
};

struct RtSmokeDynamicMaterialUploadDiffSummary
{
    int rows = 0;
    int firstRow = -1;
    int lastRow = -1;
    int colorRows = 0;
    int texMatrixRows = 0;
    int identityRows = 0;
    int flagRows = 0;
    int otherRows = 0;
    int sampleCount = 0;
    int sampleRows[RT_SMOKE_UPLOAD_DIFF_SAMPLE_COUNT] = {};
    uint32_t sampleMaterialIds[RT_SMOKE_UPLOAD_DIFF_SAMPLE_COUNT] = {};
};

bool SmokeFloat4Changed(const float previous[4], const float current[4])
{
    return std::memcmp(previous, current, sizeof(float) * 4) != 0;
}

bool SmokeMaterialTextureStateChanged(const PathTraceSmokeMaterial& previous, const PathTraceSmokeMaterial& current)
{
    return previous.diffuseTextureIndex != current.diffuseTextureIndex ||
        previous.alphaTextureIndex != current.alphaTextureIndex ||
        previous.normalTextureIndex != current.normalTextureIndex ||
        previous.specularTextureIndex != current.specularTextureIndex ||
        previous.emissiveTextureIndex != current.emissiveTextureIndex ||
        previous.textureWidth != current.textureWidth ||
        previous.textureHeight != current.textureHeight ||
        previous.alphaTextureWidth != current.alphaTextureWidth ||
        previous.alphaTextureHeight != current.alphaTextureHeight ||
        previous.normalTextureWidth != current.normalTextureWidth ||
        previous.normalTextureHeight != current.normalTextureHeight ||
        previous.specularTextureWidth != current.specularTextureWidth ||
        previous.specularTextureHeight != current.specularTextureHeight ||
        previous.emissiveTextureWidth != current.emissiveTextureWidth ||
        previous.emissiveTextureHeight != current.emissiveTextureHeight;
}

void AddSmokeMaterialUploadDiffSample(RtSmokeMaterialUploadDiffSummary& summary, int row, uint32_t materialId)
{
    if (summary.sampleCount >= RT_SMOKE_UPLOAD_DIFF_SAMPLE_COUNT)
    {
        return;
    }
    summary.sampleRows[summary.sampleCount] = row;
    summary.sampleMaterialIds[summary.sampleCount] = materialId;
    ++summary.sampleCount;
}

void AddSmokeDynamicMaterialUploadDiffSample(RtSmokeDynamicMaterialUploadDiffSummary& summary, int row, uint32_t materialId)
{
    if (summary.sampleCount >= RT_SMOKE_UPLOAD_DIFF_SAMPLE_COUNT)
    {
        return;
    }
    summary.sampleRows[summary.sampleCount] = row;
    summary.sampleMaterialIds[summary.sampleCount] = materialId;
    ++summary.sampleCount;
}

RtSmokeMaterialUploadDiffSummary BuildSmokeMaterialUploadDiffSummary(
    const std::vector<PathTraceSmokeMaterial>& previousRecords,
    const std::vector<PathTraceSmokeMaterial>& currentRecords,
    const std::vector<uint32_t>& currentMaterialIds,
    int firstChanged,
    int changedCount)
{
    RtSmokeMaterialUploadDiffSummary summary;
    if (firstChanged < 0 || changedCount <= 0)
    {
        return summary;
    }

    const int previousCount = static_cast<int>(previousRecords.size());
    const int currentCount = static_cast<int>(currentRecords.size());
    const int beginRow = idMath::ClampInt(0, currentCount, firstChanged);
    const int endRow = idMath::ClampInt(beginRow, currentCount, firstChanged + changedCount);
    for (int row = beginRow; row < endRow; ++row)
    {
        const PathTraceSmokeMaterial& current = currentRecords[row];
        const bool hasPrevious = row < previousCount;
        if (hasPrevious && std::memcmp(&previousRecords[row], &current, sizeof(current)) == 0)
        {
            continue;
        }

        ++summary.rows;
        if (summary.firstRow < 0)
        {
            summary.firstRow = row;
        }
        summary.lastRow = row;
        const uint32_t materialId = row < static_cast<int>(currentMaterialIds.size()) ? currentMaterialIds[row] : 0;
        AddSmokeMaterialUploadDiffSample(summary, row, materialId);

        if (!hasPrevious)
        {
            ++summary.otherRows;
            continue;
        }

        const PathTraceSmokeMaterial& previous = previousRecords[row];
        if (SmokeFloat4Changed(previous.debugAlbedo, current.debugAlbedo))
        {
            ++summary.debugAlbedoRows;
        }
        if (SmokeFloat4Changed(previous.emissiveColor, current.emissiveColor))
        {
            ++summary.emissiveRows;
        }
        if (SmokeMaterialTextureStateChanged(previous, current))
        {
            ++summary.textureRows;
        }
        if (previous.alphaCutoff != current.alphaCutoff)
        {
            ++summary.alphaRows;
        }
        if (previous.flags != current.flags)
        {
            ++summary.flagRows;
        }
        if (previous.padding0 != current.padding0 ||
            previous.padding1 != current.padding1 ||
            previous.padding2 != current.padding2)
        {
            ++summary.classifierRows;
        }
    }
    return summary;
}

RtSmokeDynamicMaterialUploadDiffSummary BuildSmokeDynamicMaterialUploadDiffSummary(
    const std::vector<PathTraceDynamicMaterialRecord>& previousRecords,
    const std::vector<PathTraceDynamicMaterialRecord>& currentRecords,
    int firstChanged,
    int changedCount)
{
    RtSmokeDynamicMaterialUploadDiffSummary summary;
    if (firstChanged < 0 || changedCount <= 0)
    {
        return summary;
    }

    const int previousCount = static_cast<int>(previousRecords.size());
    const int currentCount = static_cast<int>(currentRecords.size());
    const int beginRow = idMath::ClampInt(0, currentCount, firstChanged);
    const int endRow = idMath::ClampInt(beginRow, currentCount, firstChanged + changedCount);
    for (int row = beginRow; row < endRow; ++row)
    {
        const PathTraceDynamicMaterialRecord& current = currentRecords[row];
        const bool hasPrevious = row < previousCount;
        if (hasPrevious && std::memcmp(&previousRecords[row], &current, sizeof(current)) == 0)
        {
            continue;
        }

        ++summary.rows;
        if (summary.firstRow < 0)
        {
            summary.firstRow = row;
        }
        summary.lastRow = row;
        AddSmokeDynamicMaterialUploadDiffSample(summary, row, current.materialId);

        if (!hasPrevious)
        {
            ++summary.otherRows;
            continue;
        }

        const PathTraceDynamicMaterialRecord& previous = previousRecords[row];
        if (SmokeFloat4Changed(previous.color, current.color))
        {
            ++summary.colorRows;
        }
        if (SmokeFloat4Changed(previous.texMatrix0, current.texMatrix0) ||
            SmokeFloat4Changed(previous.texMatrix1, current.texMatrix1))
        {
            ++summary.texMatrixRows;
        }
        if (previous.materialIndex != current.materialIndex ||
            previous.materialId != current.materialId ||
            previous.stageIndex != current.stageIndex)
        {
            ++summary.identityRows;
        }
        if (previous.flags != current.flags)
        {
            ++summary.flagRows;
        }
    }
    return summary;
}

int SmokeMaterialDiffSampleRow(const RtSmokeMaterialUploadDiffSummary& summary, int sampleIndex)
{
    return sampleIndex < summary.sampleCount ? summary.sampleRows[sampleIndex] : -1;
}

uint32_t SmokeMaterialDiffSampleId(const RtSmokeMaterialUploadDiffSummary& summary, int sampleIndex)
{
    return sampleIndex < summary.sampleCount ? summary.sampleMaterialIds[sampleIndex] : 0;
}

int SmokeDynamicMaterialDiffSampleRow(const RtSmokeDynamicMaterialUploadDiffSummary& summary, int sampleIndex)
{
    return sampleIndex < summary.sampleCount ? summary.sampleRows[sampleIndex] : -1;
}

uint32_t SmokeDynamicMaterialDiffSampleId(const RtSmokeDynamicMaterialUploadDiffSummary& summary, int sampleIndex)
{
    return sampleIndex < summary.sampleCount ? summary.sampleMaterialIds[sampleIndex] : 0;
}

void ApplySmokeDynamicMaterialRecordToGpuMaterial(uint32_t materialIndex, const std::vector<PathTraceDynamicMaterialRecord>& records, PathTraceSmokeMaterial& material)
{
    if (materialIndex >= records.size())
    {
        return;
    }

    const PathTraceDynamicMaterialRecord& record = records[materialIndex];
    if ((record.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_VALID) == 0u ||
        record.materialIndex != materialIndex ||
        (record.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_SELECTED_EMISSIVE) == 0u)
    {
        return;
    }
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
        material.flags &= ~RT_SMOKE_MATERIAL_EMISSIVE;
        return;
    }

    const float stageAlpha = idMath::ClampFloat(0.0f, 1.0f, record.color[3]);
    const float stageScale[3] = {
        Max(0.0f, record.color[0]) * stageAlpha,
        Max(0.0f, record.color[1]) * stageAlpha,
        Max(0.0f, record.color[2]) * stageAlpha
    };
    if ((record.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_REPLACE_EMISSIVE) != 0u)
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
        material.flags &= ~RT_SMOKE_MATERIAL_EMISSIVE;
    }
    else
    {
        material.flags |= RT_SMOKE_MATERIAL_EMISSIVE;
    }
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

bool SmokeRigidRouteSideBufferSlotHasCapacity(
    const RtSmokeRigidRouteSideBufferSlot& slot,
    const RtPathTraceRigidRouteBuild& build)
{
    return
        SmokeBufferHasPayloadCapacity(slot.vertexBuffer, build.vertices.size() * sizeof(PathTraceSmokeVertex), sizeof(PathTraceSmokeVertex)) &&
        SmokeBufferHasPayloadCapacity(slot.indexBuffer, build.indexes.size() * sizeof(uint32_t), sizeof(uint32_t)) &&
        SmokeBufferHasPayloadCapacity(slot.triangleMaterialBuffer, build.triangleMaterials.size() * sizeof(uint32_t), sizeof(uint32_t)) &&
        SmokeBufferHasPayloadCapacity(slot.triangleMaterialIndexBuffer, build.triangleMaterialIndexes.size() * sizeof(uint32_t), sizeof(uint32_t)) &&
        SmokeBufferHasPayloadCapacity(slot.instanceBuffer, build.instances.size() * sizeof(PathTraceRigidRouteInstance), sizeof(PathTraceRigidRouteInstance));
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

uint64_t SmokeUploadItemWriteBytes(const RtSmokeBufferUploadItem& item)
{
    return (!item.skip && item.buffer && item.data && item.byteSize > 0)
        ? static_cast<uint64_t>(item.byteSize)
        : 0ull;
}

uint64_t SmokeUploadItemSkippedBytes(const RtSmokeBufferUploadItem& item)
{
    return (item.skip && item.buffer && item.data && item.byteSize > 0)
        ? static_cast<uint64_t>(item.byteSize)
        : 0ull;
}

uint64_t SmokeUploadItemBufferCapacity(const RtSmokeBufferUploadItem& item)
{
    return item.buffer ? static_cast<uint64_t>(item.buffer->getDesc().byteSize) : 0ull;
}

uint64_t SmokeEmissiveIdentityKey(const PathTraceSmokeEmissiveTriangle& triangle)
{
    return (static_cast<uint64_t>(triangle.identityHashHi) << 32ull) | static_cast<uint64_t>(triangle.identityHashLo);
}

bool SmokeEmissiveLightRecordsCompatible(const PathTraceSmokeEmissiveTriangle& current, const PathTraceSmokeEmissiveTriangle& previous)
{
    return current.identityHashLo == previous.identityHashLo &&
        current.identityHashHi == previous.identityHashHi &&
        current.materialId == previous.materialId &&
        current.universeMaterialIndex == previous.universeMaterialIndex &&
        current.emissiveTextureIndex == previous.emissiveTextureIndex;
}

void BuildSmokeEmissiveIdentityMap(
    const std::vector<PathTraceSmokeEmissiveTriangle>& triangles,
    std::unordered_map<uint64_t, int>& identityToIndex)
{
    identityToIndex.clear();
    identityToIndex.reserve(triangles.size());
    for (int triangleIndex = 0; triangleIndex < static_cast<int>(triangles.size()); ++triangleIndex)
    {
        const uint64_t identityKey = SmokeEmissiveIdentityKey(triangles[triangleIndex]);
        if (identityKey == 0)
        {
            continue;
        }

        const auto insertResult = identityToIndex.emplace(identityKey, triangleIndex);
        if (!insertResult.second)
        {
            insertResult.first->second = -1;
        }
    }
}

std::vector<PathTraceEmissiveLightRemap> BuildSmokeEmissiveLightRemap(
    const std::vector<PathTraceSmokeEmissiveTriangle>& currentTriangles,
    const std::vector<PathTraceSmokeEmissiveTriangle>& previousTriangles)
{
    std::vector<PathTraceEmissiveLightRemap> remap(std::max(currentTriangles.size(), previousTriangles.size()));
    std::unordered_map<uint64_t, int> currentByIdentity;
    std::unordered_map<uint64_t, int> previousByIdentity;
    BuildSmokeEmissiveIdentityMap(currentTriangles, currentByIdentity);
    BuildSmokeEmissiveIdentityMap(previousTriangles, previousByIdentity);

    for (int currentIndex = 0; currentIndex < static_cast<int>(currentTriangles.size()); ++currentIndex)
    {
        const uint64_t identityKey = SmokeEmissiveIdentityKey(currentTriangles[currentIndex]);
        if (identityKey == 0)
        {
            if (currentIndex < static_cast<int>(remap.size()))
            {
                remap[currentIndex].flags |= RT_SMOKE_EMISSIVE_REMAP_CURRENT_ZERO_IDENTITY;
            }
            continue;
        }

        const auto currentIt = currentByIdentity.find(identityKey);
        if (currentIt == currentByIdentity.end() || currentIt->second != currentIndex)
        {
            if (currentIndex < static_cast<int>(remap.size()))
            {
                remap[currentIndex].flags |= RT_SMOKE_EMISSIVE_REMAP_CURRENT_DUPLICATE;
            }
            continue;
        }

        const auto previousIt = previousByIdentity.find(identityKey);
        if (previousIt == previousByIdentity.end())
        {
            if (currentIndex < static_cast<int>(remap.size()))
            {
                remap[currentIndex].flags |= RT_SMOKE_EMISSIVE_REMAP_PREVIOUS_MISSING;
            }
            continue;
        }
        if (previousIt->second < 0)
        {
            if (currentIndex < static_cast<int>(remap.size()))
            {
                remap[currentIndex].flags |= RT_SMOKE_EMISSIVE_REMAP_PREVIOUS_DUPLICATE;
            }
            continue;
        }

        const int previousIndex = previousIt->second;
        if (previousIndex >= static_cast<int>(previousTriangles.size()) ||
            !SmokeEmissiveLightRecordsCompatible(currentTriangles[currentIndex], previousTriangles[previousIndex]))
        {
            if (currentIndex < static_cast<int>(remap.size()))
            {
                remap[currentIndex].flags |= RT_SMOKE_EMISSIVE_REMAP_INCOMPATIBLE;
            }
            if (previousIndex >= 0 && previousIndex < static_cast<int>(remap.size()))
            {
                remap[previousIndex].flags |= RT_SMOKE_EMISSIVE_REMAP_INCOMPATIBLE;
            }
            continue;
        }

        if (currentIndex < static_cast<int>(remap.size()))
        {
            remap[currentIndex].currentToPreviousIndex = previousIndex;
            remap[currentIndex].flags |= RT_SMOKE_EMISSIVE_REMAP_VALID;
        }
        if (previousIndex < static_cast<int>(remap.size()))
        {
            remap[previousIndex].previousToCurrentIndex = currentIndex;
            remap[previousIndex].flags |= RT_SMOKE_EMISSIVE_REMAP_VALID;
        }
    }

    for (int previousIndex = 0; previousIndex < static_cast<int>(previousTriangles.size()); ++previousIndex)
    {
        const uint64_t identityKey = SmokeEmissiveIdentityKey(previousTriangles[previousIndex]);
        if (identityKey == 0)
        {
            if (previousIndex < static_cast<int>(remap.size()))
            {
                remap[previousIndex].flags |= RT_SMOKE_EMISSIVE_REMAP_PREVIOUS_ZERO_IDENTITY;
            }
            continue;
        }

        const auto previousIt = previousByIdentity.find(identityKey);
        if (previousIt == previousByIdentity.end() || previousIt->second != previousIndex)
        {
            if (previousIndex < static_cast<int>(remap.size()))
            {
                remap[previousIndex].flags |= RT_SMOKE_EMISSIVE_REMAP_PREVIOUS_DUPLICATE;
            }
            continue;
        }

        const auto currentIt = currentByIdentity.find(identityKey);
        if (currentIt == currentByIdentity.end())
        {
            if (previousIndex < static_cast<int>(remap.size()))
            {
                remap[previousIndex].flags |= RT_SMOKE_EMISSIVE_REMAP_CURRENT_MISSING;
            }
            continue;
        }
        if (currentIt->second < 0)
        {
            if (previousIndex < static_cast<int>(remap.size()))
            {
                remap[previousIndex].flags |= RT_SMOKE_EMISSIVE_REMAP_CURRENT_DUPLICATE;
            }
        }
    }

    return remap;
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
};

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

void AddSmokeSkinnedInvalidReasonStats(RtSmokeSkinnedPreviousFrameStats& stats, uint32_t reasons)
{
    if ((reasons & RT_SMOKE_SKINNED_INVALID_NO_PREVIOUS_FRAME) != 0u) { ++stats.noPreviousFrameCount; }
    if ((reasons & RT_SMOKE_SKINNED_INVALID_NO_PREVIOUS_SURFACE) != 0u) { ++stats.noPreviousSurfaceCount; }
    if ((reasons & RT_SMOKE_SKINNED_INVALID_VERTEX_COUNT_MISMATCH) != 0u) { ++stats.vertexCountMismatchCount; }
    if ((reasons & RT_SMOKE_SKINNED_INVALID_INDEX_COUNT_MISMATCH) != 0u) { ++stats.indexCountMismatchCount; }
    if ((reasons & RT_SMOKE_SKINNED_INVALID_TRIANGLE_COUNT_MISMATCH) != 0u) { ++stats.triangleCountMismatchCount; }
    if ((reasons & RT_SMOKE_SKINNED_INVALID_MATERIAL_CHANGED) != 0u) { ++stats.materialChangedCount; }
    if ((reasons & RT_SMOKE_SKINNED_INVALID_SURFACE_CLASS_CHANGED) != 0u) { ++stats.surfaceClassChangedCount; }
    if ((reasons & RT_SMOKE_SKINNED_INVALID_NOT_RT_CPU_SKINNED) != 0u) { ++stats.notRtCpuSkinnedCount; }
    if ((reasons & RT_SMOKE_SKINNED_INVALID_SKELETON_CHANGED) != 0u) { ++stats.skeletonChangedCount; }
    if ((reasons & RT_SMOKE_SKINNED_INVALID_TRANSFORM_DISCONTINUITY) != 0u) { ++stats.transformDiscontinuityCount; }
    if ((reasons & RT_SMOKE_SKINNED_INVALID_PREVIOUS_BUFFER_UNAVAILABLE) != 0u) { ++stats.previousBufferUnavailableCount; }
}

bool SmokeSkinnedCurrentVertexRangeValid(const RtSmokeSkinnedSurfaceRecord& record, const std::vector<PathTraceSmokeVertex>& dynamicVertexData)
{
    return record.currentVertexOffset >= 0 &&
        record.vertexCount > 0 &&
        record.currentVertexOffset <= static_cast<int>(dynamicVertexData.size()) &&
        record.vertexCount <= static_cast<int>(dynamicVertexData.size()) - record.currentVertexOffset;
}

RtSmokeSkinnedPreviousFrameStats UpdateSmokeSkinnedPreviousCpuBridge(
    std::vector<RtSmokeSkinnedSurfaceRecord>& currentRecords,
    const std::vector<RtSmokeSkinnedSurfaceRecord>& previousRecords,
    const std::vector<PathTraceSmokeVertex>& previousSkinnedVertexData,
    const std::vector<PathTraceSmokeVertex>& dynamicVertexData,
    std::vector<PathTraceSmokeVertex>& nextPreviousSkinnedVertexData)
{
    RtSmokeSkinnedPreviousFrameStats stats;
    const bool hadPreviousFrame = !previousRecords.empty() || !previousSkinnedVertexData.empty();
    const float teleportDistanceSqr = RT_SMOKE_SKINNED_TELEPORT_DISTANCE * RT_SMOKE_SKINNED_TELEPORT_DISTANCE;

    nextPreviousSkinnedVertexData.clear();
    for (RtSmokeSkinnedSurfaceRecord& current : currentRecords)
    {
        ++stats.currentSurfaceCount;
        stats.currentTriangleCount += current.triangleCount;
        if (current.rtCpuSkinned)
        {
            ++stats.currentRtCpuSkinnedSurfaceCount;
        }

        uint32_t reasons = RT_SMOKE_SKINNED_INVALID_NONE;
        uint32_t temporalFlags = 0;
        const RtSmokeSkinnedSurfaceRecord* previous = FindSmokeSkinnedPreviousRecord(previousRecords, current);
        if (!current.rtCpuSkinned)
        {
            reasons |= RT_SMOKE_SKINNED_INVALID_NOT_RT_CPU_SKINNED;
        }
        if (!hadPreviousFrame)
        {
            reasons |= RT_SMOKE_SKINNED_INVALID_NO_PREVIOUS_FRAME;
        }
        else if (!previous)
        {
            const RtSmokeSkinnedSurfaceRecord* loosePrevious = FindSmokeSkinnedPreviousLooseRecord(previousRecords, current);
            if (loosePrevious && loosePrevious->key.surfaceClassId != current.key.surfaceClassId)
            {
                reasons |= RT_SMOKE_SKINNED_INVALID_SURFACE_CLASS_CHANGED;
            }
            else if (loosePrevious && loosePrevious->key.materialId != current.key.materialId)
            {
                reasons |= RT_SMOKE_SKINNED_INVALID_MATERIAL_CHANGED;
            }
            else
            {
                reasons |= RT_SMOKE_SKINNED_INVALID_NO_PREVIOUS_SURFACE;
            }
        }
        else
        {
            if (previous->vertexCount != current.vertexCount)
            {
                reasons |= RT_SMOKE_SKINNED_INVALID_VERTEX_COUNT_MISMATCH;
            }
            if (previous->indexCount != current.indexCount)
            {
                reasons |= RT_SMOKE_SKINNED_INVALID_INDEX_COUNT_MISMATCH;
            }
            if (previous->triangleCount != current.triangleCount)
            {
                reasons |= RT_SMOKE_SKINNED_INVALID_TRIANGLE_COUNT_MISMATCH;
            }
            if (previous->jointCount != current.jointCount || previous->jointSource != current.jointSource)
            {
                reasons |= RT_SMOKE_SKINNED_INVALID_SKELETON_CHANGED;
            }
            if (previous->hasEntityOrigin && current.hasEntityOrigin && (current.entityOrigin - previous->entityOrigin).LengthSqr() > teleportDistanceSqr)
            {
                reasons |= RT_SMOKE_SKINNED_INVALID_TRANSFORM_DISCONTINUITY;
            }
            if (previous->retainedVertexOffset < 0 ||
                previous->vertexCount <= 0 ||
                previous->retainedVertexOffset > static_cast<int>(previousSkinnedVertexData.size()) ||
                previous->vertexCount > static_cast<int>(previousSkinnedVertexData.size()) - previous->retainedVertexOffset)
            {
                reasons |= RT_SMOKE_SKINNED_INVALID_PREVIOUS_BUFFER_UNAVAILABLE;
            }

            if ((reasons & (RT_SMOKE_SKINNED_INVALID_VERTEX_COUNT_MISMATCH | RT_SMOKE_SKINNED_INVALID_INDEX_COUNT_MISMATCH | RT_SMOKE_SKINNED_INVALID_TRIANGLE_COUNT_MISMATCH)) == 0u)
            {
                temporalFlags |= RT_SMOKE_SKINNED_TEMPORAL_TOPOLOGY_STABLE;
            }
            temporalFlags |= RT_SMOKE_SKINNED_TEMPORAL_LOD_STABLE;
            if ((reasons & RT_SMOKE_SKINNED_INVALID_TRANSFORM_DISCONTINUITY) == 0u)
            {
                temporalFlags |= RT_SMOKE_SKINNED_TEMPORAL_TRANSFORM_CONTINUOUS;
            }
            if ((reasons & RT_SMOKE_SKINNED_INVALID_SKELETON_CHANGED) == 0u && current.rtCpuSkinned)
            {
                temporalFlags |= RT_SMOKE_SKINNED_TEMPORAL_DEFORMATION_CONTINUOUS;
            }
            temporalFlags |= RT_SMOKE_SKINNED_TEMPORAL_MATERIAL_STABLE;
            if ((reasons & RT_SMOKE_SKINNED_INVALID_PREVIOUS_BUFFER_UNAVAILABLE) == 0u)
            {
                temporalFlags |= RT_SMOKE_SKINNED_TEMPORAL_PREVIOUS_BUFFER_VALID;
            }
        }

        if (previous && reasons == RT_SMOKE_SKINNED_INVALID_NONE)
        {
            current.previousValid = true;
            current.previousVertexOffset = previous->retainedVertexOffset;
            current.previousIndexOffset = previous->currentIndexOffset;
            current.previousTriangleOffset = previous->currentTriangleOffset;
            temporalFlags |= RT_SMOKE_SKINNED_TEMPORAL_HAS_VALID_PREVIOUS;
            ++stats.previousMatchedSurfaceCount;
        }
        else
        {
            current.previousValid = false;
            ++stats.previousInvalidSurfaceCount;
        }

        current.invalidReasonFlags = reasons;
        current.temporalStateFlags = temporalFlags;
        AddSmokeSkinnedInvalidReasonStats(stats, reasons);
        if ((temporalFlags & RT_SMOKE_SKINNED_TEMPORAL_TOPOLOGY_STABLE) != 0u) { ++stats.topologyStableCount; }
        if ((temporalFlags & RT_SMOKE_SKINNED_TEMPORAL_LOD_STABLE) != 0u) { ++stats.lodStableCount; }
        if ((temporalFlags & RT_SMOKE_SKINNED_TEMPORAL_TRANSFORM_CONTINUOUS) != 0u) { ++stats.transformContinuousCount; }
        if ((temporalFlags & RT_SMOKE_SKINNED_TEMPORAL_DEFORMATION_CONTINUOUS) != 0u) { ++stats.deformationContinuousCount; }
        if ((temporalFlags & RT_SMOKE_SKINNED_TEMPORAL_MATERIAL_STABLE) != 0u) { ++stats.materialStableCount; }
        if ((temporalFlags & RT_SMOKE_SKINNED_TEMPORAL_PREVIOUS_BUFFER_VALID) != 0u) { ++stats.previousBufferValidCount; }
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
    stats.previousRetainedVertexCount = static_cast<int>(nextPreviousSkinnedVertexData.size());
    return stats;
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
    vertex.texCoord[2] = 0.0f;
    vertex.texCoord[3] = 0.0f;
    for (int component = 0; component < 4; ++component)
    {
        vertex.color[component] = drawVert.color[component] * (1.0f / 255.0f);
        vertex.jointIndices[component] = static_cast<uint32_t>(drawVert.color[component]);
        vertex.jointWeights[component] = drawVert.color2[component] * (1.0f / 255.0f);
    }
    return vertex;
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
    bool buildGpuSkinningInputs,
    std::vector<RtSmokeSkinnedSurfaceRecord>& currentRecords,
    const std::vector<RtSmokeSkinnedSurfaceRecord>& previousRecords,
    const std::vector<PathTraceSmokeVertex>& dynamicVertexData,
    const std::vector<PathTraceSmokeVertex>& previousSkinnedVertexData,
    const std::vector<PathTraceSkinnedJointMatrix>& previousSkinnedJointMatrices)
{
    RtSmokeSkinnedGpuScaffoldBuild build;
    if (scaffoldMode <= 0 || currentRecords.empty())
    {
        return build;
    }

    for (int recordIndex = 0; recordIndex < static_cast<int>(currentRecords.size()); ++recordIndex)
    {
        RtSmokeSkinnedSurfaceRecord& record = currentRecords[recordIndex];
        const srfTriangles_t* tri = reinterpret_cast<const srfTriangles_t*>(record.key.tri);
        if (!record.rtCpuSkinned ||
            !tri ||
            !tri->verts ||
            record.vertexCount <= 0 ||
            record.vertexCount > tri->numVerts ||
            !SmokeSkinnedCurrentVertexRangeValid(record, dynamicVertexData))
        {
            continue;
        }

        record.gpuSourceVertexOffset = -1;
        record.gpuOutputVertexOffset = -1;
        record.gpuPreviousPositionOffset = -1;

        if (buildGpuSkinningInputs)
        {
            record.gpuSourceVertexOffset = static_cast<int>(build.sourceVertices.size());
            record.gpuOutputVertexOffset = static_cast<int>(build.currentOutputVertices.size());
            for (int vertexIndex = 0; vertexIndex < record.vertexCount; ++vertexIndex)
            {
                build.sourceVertices.push_back(BuildSmokeSkinnedSourceVertex(tri->verts[vertexIndex]));
                build.currentOutputVertices.push_back(dynamicVertexData[record.currentVertexOffset + vertexIndex]);
            }
        }

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

        PathTraceSkinnedSurfaceDispatchRecord dispatch = {};
        dispatch.sourceVertexOffset = record.gpuSourceVertexOffset >= 0 ? static_cast<uint32_t>(record.gpuSourceVertexOffset) : UINT32_MAX;
        dispatch.outputVertexOffset = record.gpuOutputVertexOffset >= 0 ? static_cast<uint32_t>(record.gpuOutputVertexOffset) : UINT32_MAX;
        dispatch.previousPositionOffset = record.gpuPreviousPositionOffset >= 0 ? static_cast<uint32_t>(record.gpuPreviousPositionOffset) : UINT32_MAX;
        dispatch.vertexCount = static_cast<uint32_t>(record.vertexCount);
        dispatch.currentJointOffset = UINT32_MAX;
        dispatch.previousJointOffset = UINT32_MAX;
        dispatch.surfaceRecordIndex = static_cast<uint32_t>(recordIndex);
        dispatch.flags = PT_SKINNED_DISPATCH_RT_CPU_SKINNED | PT_SKINNED_DISPATCH_SOURCE_READY;
        dispatch.dynamicVertexOffset = static_cast<uint32_t>(record.currentVertexOffset);
        dispatch.dynamicIndexOffset = static_cast<uint32_t>(record.currentIndexOffset);
        dispatch.dynamicTriangleOffset = static_cast<uint32_t>(record.currentTriangleOffset);
        dispatch.triangleCount = static_cast<uint32_t>(record.triangleCount);
        if (record.previousValid && record.gpuPreviousPositionOffset >= 0)
        {
            dispatch.flags |= PT_SKINNED_DISPATCH_HAS_VALID_PREVIOUS;
        }
        CopySmokeObjectToWorldRows(dispatch.currentObjectToWorld, record.objectToWorld);
        const RtSmokeSkinnedSurfaceRecord* previousRecord = FindSmokeSkinnedPreviousRecord(previousRecords, record);
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
            if (record.previousValid &&
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
    }

    return build;
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
    const bool mode18 = debugMode == 18;
    const bool mode20 = debugMode == 20;
    if (!mode18 && !mode20)
    {
        return;
    }

    r_pathTracingDebugMode.SetInteger(debugMode);
    r_pathTracingSceneSource.SetInteger(3);
    r_pathTracingRigidBlasGpuScaffold.SetInteger(1);
    r_pathTracingRigidBlasGpuBuild.SetInteger(1);
    r_pathTracingRigidTlasRoute.SetInteger(1);
    r_pathTracingRigidRouteMode18.SetInteger(mode18 ? 1 : r_pathTracingRigidRouteMode18.GetInteger());
    r_pathTracingRigidRouteMode20.SetInteger(mode20 ? 1 : r_pathTracingRigidRouteMode20.GetInteger());
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

    r_pathTracingLightAreaFilter.SetInteger(mode20 && preset >= 2 ? 1 : 0);
    r_pathTracingLightAreaFilterApply.SetInteger(mode20 && preset == 3 ? 1 : 0);
    r_pathTracingLightAreaOverflowMax.SetInteger(512);
    r_pathTracingLightUniverseChurn.SetInteger(mode20 && preset >= 2 ? 1 : 0);

    const int presetRigidRouteMaxInstances = 510;
    if (r_pathTracingRigidRouteMaxInstances.GetInteger() < presetRigidRouteMaxInstances)
    {
        r_pathTracingRigidRouteMaxInstances.SetInteger(presetRigidRouteMaxInstances);
    }

    common->Printf("PathTracePrimaryPass: applied %s preset %d source3=1 rigidRoute=1 rigidResidency=1 residencyV2=1 entityFeed=1 staticPreload=1 rigidEmissiveCards=1 portalSteps=%d lightAreaDiag=%d lightAreaApply=%d bvhValidation=%d churn=%d lightOverflowMax=512 rigidRouteMax=%d tlasMax=512\n",
        label ? label : "mode test",
        preset,
        portalSteps,
        mode20 && preset >= 2 ? 1 : 0,
        mode20 && preset == 3 ? 1 : 0,
        preset == 4 ? 1 : 0,
        mode20 && preset >= 2 ? 1 : 0,
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

int CountSmokeDynamicSurfaces(const RtSmokeSurfaceClassStats& stats)
{
    return stats.rigidEntitySurfaces + stats.skinnedDeformedSurfaces + stats.particleAlphaSurfaces + stats.unknownSurfaces;
}

int CountSmokeDynamicTriangles(const RtSmokeSurfaceClassStats& stats)
{
    return stats.rigidEntityTriangles + stats.skinnedDeformedTriangles + stats.particleAlphaTriangles + stats.unknownTriangles;
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

int CountUniqueMaterialIds(const std::vector<uint32_t>& materialIds)
{
    std::unordered_set<uint32_t> visited;
    visited.reserve(Min(static_cast<int>(materialIds.size()), 4096));
    for (uint32_t materialId : materialIds)
    {
        if (materialId == 0u)
        {
            continue;
        }
        visited.insert(materialId);
    }
    return static_cast<int>(visited.size());
}

struct RtSmokeSourceCompareMaterialDiff
{
    int oldUnique = 0;
    int source3Unique = 0;
    int missing = 0;
    int extra = 0;
    uint32_t missingSamples[8] = {};
    uint32_t extraSamples[8] = {};
    int missingSampleCount = 0;
    int extraSampleCount = 0;
};

RtSmokeSourceCompareMaterialDiff CompareSmokeDynamicMaterialIds(
    const std::vector<uint32_t>& oldMaterialIds,
    const std::vector<uint32_t>& source3MaterialIds)
{
    RtSmokeSourceCompareMaterialDiff diff;
    const std::vector<uint32_t> oldUnique = BuildSortedUniqueMaterialIds(oldMaterialIds);
    const std::vector<uint32_t> source3Unique = BuildSortedUniqueMaterialIds(source3MaterialIds);
    diff.oldUnique = static_cast<int>(oldUnique.size());
    diff.source3Unique = static_cast<int>(source3Unique.size());

    size_t oldIndex = 0;
    size_t source3Index = 0;
    while (oldIndex < oldUnique.size() || source3Index < source3Unique.size())
    {
        if (source3Index >= source3Unique.size() || (oldIndex < oldUnique.size() && oldUnique[oldIndex] < source3Unique[source3Index]))
        {
            if (diff.missingSampleCount < static_cast<int>(sizeof(diff.missingSamples) / sizeof(diff.missingSamples[0])))
            {
                diff.missingSamples[diff.missingSampleCount++] = oldUnique[oldIndex];
            }
            ++diff.missing;
            ++oldIndex;
        }
        else if (oldIndex >= oldUnique.size() || source3Unique[source3Index] < oldUnique[oldIndex])
        {
            if (diff.extraSampleCount < static_cast<int>(sizeof(diff.extraSamples) / sizeof(diff.extraSamples[0])))
            {
                diff.extraSamples[diff.extraSampleCount++] = source3Unique[source3Index];
            }
            ++diff.extra;
            ++source3Index;
        }
        else
        {
            ++oldIndex;
            ++source3Index;
        }
    }
    return diff;
}

void PrintSmokeMaterialIdSamples(const char* label, const uint32_t* samples, int sampleCount)
{
    if (sampleCount <= 0)
    {
        return;
    }

    char sampleText[256];
    sampleText[0] = '\0';
    for (int sampleIndex = 0; sampleIndex < sampleCount; ++sampleIndex)
    {
        idStr::Append(sampleText, sizeof(sampleText), va("%s%u", sampleIndex == 0 ? "" : ",", samples[sampleIndex]));
    }
    common->Printf("PathTracePrimaryPass: PT source3 compare %s materialIds=[%s]\n", label, sampleText);
}

void DumpSource3CaptureCompare(
    const viewDef_t* viewDef,
    uint64 frameIndex,
    const RtSmokeSurfaceClassStats& source3ClassStats,
    const RtSmokeSurfaceSkipStats& source3SkipStats,
    int source3Surfaces,
    int source3Verts,
    int source3Indexes,
    const std::vector<uint32_t>& source3DynamicIndexes,
    const std::vector<uint32_t>& source3DynamicTriangleMaterialIds)
{
    std::vector<PathTraceSmokeVertex> oldDynamicVertices;
    std::vector<uint32_t> oldDynamicIndexes;
    std::vector<uint32_t> oldDynamicTriangleClasses;
    std::vector<uint32_t> oldDynamicTriangleMaterialIds;
    RtSmokeGeometryUniverse oldGeometryUniverse;
    bool oldStaticCacheChanged = false;
    idVec3 oldSceneOrigin = vec3_origin;
    int oldSurfaces = 0;
    int oldVerts = 0;
    int oldIndexes = 0;
    int oldAnchorTriangle = -1;
    RtSmokeSurfaceClassStats oldClassStats;
    RtSmokeSurfaceSkipStats oldSkipStats;
    RtSmokeDynamicGeometryStats oldDynamicStats;
    RtSmokeAttributeStats oldAttributeStats;
    RtSmokeMaterialStats oldMaterialStats;
    RtSmokeBucketRanges oldBucketRanges;
    RtSmokeSceneCaptureTiming oldCaptureTiming;

    oldGeometryUniverse.BeginFrame(frameIndex);
    const bool oldCaptured = CaptureDoomSurfacesForSmokeTest(
        viewDef,
        oldDynamicVertices,
        oldDynamicIndexes,
        oldDynamicTriangleClasses,
        oldDynamicTriangleMaterialIds,
        nullptr,
        nullptr,
        oldGeometryUniverse,
        oldStaticCacheChanged,
        oldSceneOrigin,
        oldSurfaces,
        oldVerts,
        oldIndexes,
        oldAnchorTriangle,
        oldClassStats,
        oldSkipStats,
        oldDynamicStats,
        oldAttributeStats,
        oldMaterialStats,
        oldBucketRanges,
        oldCaptureTiming,
        nullptr,
        nullptr,
        false,
        false,
        false);
    oldGeometryUniverse.EndFrame();

    const RtSmokeGeometryUniverseStats oldGeometryStats = oldGeometryUniverse.GetStats(false);
    const RtSmokeSourceCompareMaterialDiff materialDiff = CompareSmokeDynamicMaterialIds(oldDynamicTriangleMaterialIds, source3DynamicTriangleMaterialIds);

    common->Printf("PathTracePrimaryPass: PT source3 compare capturedOld=%d totals old/source3 surfaces=%d/%d verts=%d/%d indexes=%d/%d staticTris=%d/%d dynTris=%d/%d oldStaticCache tris=%d records=%d\n",
        oldCaptured ? 1 : 0,
        oldSurfaces,
        source3Surfaces,
        oldVerts,
        source3Verts,
        oldIndexes,
        source3Indexes,
        oldClassStats.staticWorldTriangles,
        source3ClassStats.staticWorldTriangles,
        CountSmokeDynamicTriangles(oldClassStats),
        CountSmokeDynamicTriangles(source3ClassStats),
        oldGeometryStats.staticTriangles,
        oldGeometryStats.staticRecords);
    common->Printf("PathTracePrimaryPass: PT source3 compare classes old/source3 static=%d/%d rigid=%d/%d skinned=%d/%d particle=%d/%d unknown=%d/%d dynamicSurfaces=%d/%d dynamicIndexes=%d/%d\n",
        oldClassStats.staticWorldSurfaces,
        source3ClassStats.staticWorldSurfaces,
        oldClassStats.rigidEntitySurfaces,
        source3ClassStats.rigidEntitySurfaces,
        oldClassStats.skinnedDeformedSurfaces,
        source3ClassStats.skinnedDeformedSurfaces,
        oldClassStats.particleAlphaSurfaces,
        source3ClassStats.particleAlphaSurfaces,
        oldClassStats.unknownSurfaces,
        source3ClassStats.unknownSurfaces,
        CountSmokeDynamicSurfaces(oldClassStats),
        CountSmokeDynamicSurfaces(source3ClassStats),
        static_cast<int>(oldDynamicIndexes.size()),
        static_cast<int>(source3DynamicIndexes.size()));
    common->Printf("PathTracePrimaryPass: PT source3 compare dynamicMaterials unique old/source3=%d/%d missing=%d extra=%d triangleIds=%d/%d\n",
        materialDiff.oldUnique,
        materialDiff.source3Unique,
        materialDiff.missing,
        materialDiff.extra,
        static_cast<int>(oldDynamicTriangleMaterialIds.size()),
        static_cast<int>(source3DynamicTriangleMaterialIds.size()));
    PrintSmokeMaterialIdSamples("missingFromSource3", materialDiff.missingSamples, materialDiff.missingSampleCount);
    PrintSmokeMaterialIdSamples("extraInSource3", materialDiff.extraSamples, materialDiff.extraSampleCount);
    common->Printf("PathTracePrimaryPass: PT source3 compare skips old/source3 null=%d/%d geom=%d/%d material=%d/%d space=%d/%d model=%d/%d invalid=%d/%d nonCurrent=%d/%d limits=%d/%d zero=%d/%d gui=%d/%d callback=%d/%d\n",
        oldSkipStats.nullSurface,
        source3SkipStats.nullSurface,
        oldSkipStats.missingGeometry,
        source3SkipStats.missingGeometry,
        oldSkipStats.nullMaterial,
        source3SkipStats.nullMaterial,
        oldSkipStats.nullSpace,
        source3SkipStats.nullSpace,
        oldSkipStats.nullModel,
        source3SkipStats.nullModel,
        oldSkipStats.invalidIndexCount,
        source3SkipStats.invalidIndexCount,
        oldSkipStats.nonCurrentCache,
        source3SkipStats.nonCurrentCache,
        oldSkipStats.limitExceeded,
        source3SkipStats.limitExceeded,
        oldSkipStats.zeroAreaOnly,
        source3SkipStats.zeroAreaOnly,
        oldSkipStats.guiSurface,
        source3SkipStats.guiSurface,
        oldSkipStats.callbackEntity,
        source3SkipStats.callbackEntity);
}

}

void PathTracePrimaryPass::BuildRayTracingSmokeTestScene(const viewDef_t* viewDef)
{
    OPTICK_EVENT("PT Build Scene");

    const int sceneStartMs = Sys_Milliseconds();
    const int mode18Preset = r_pathTracingMode18TestPreset.GetInteger();
    if (mode18Preset != 0)
    {
        ApplySmokeRoutedScenePreset(18, mode18Preset, "mode18 test");
        r_pathTracingMode18TestPreset.SetInteger(0);
    }
    const int mode20Preset = r_pathTracingMode20TestPreset.GetInteger();
    if (mode20Preset != 0)
    {
        ApplySmokeRoutedScenePreset(20, mode20Preset, "mode20 test");
        r_pathTracingMode20TestPreset.SetInteger(0);
    }
    m_smokeSceneBuilt = false;
    m_smokeBoundsOverlayLines.clear();
    m_smokeBoundsOverlayLineCount = 0;
    m_smokeBoundsOverlayViewValid = false;
    const int requestedDebugMode = idMath::ClampInt(0, 57, r_pathTracingDebugMode.GetInteger());
    const bool restirPTDebugMode = IsPathTraceRestirPTDebugMode(requestedDebugMode);
    const bool integratorDebugMode = requestedDebugMode >= 34 && requestedDebugMode <= 37;
    const int pdfNeeVerifierSceneBuildView = idMath::ClampInt(0, 8, r_pathTracingRestirPdfNeeVerifierView.GetInteger());
    const int pdfNeeVerifierSceneBuildLightMode = idMath::ClampInt(0, 8, r_pathTracingRestirPdfNeeVerifierLightMode.GetInteger());
    const bool pdfNeeVerifierStaticEmissiveProducerPolicy =
        requestedDebugMode == 0 &&
        r_pathTracingRestirPdfNeeVerifierEnable.GetInteger() != 0 &&
        pdfNeeVerifierSceneBuildView > 0 &&
        pdfNeeVerifierSceneBuildLightMode == 7;
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
        requestedDebugMode != 56 &&
        r_pathTracingNeeCacheEnable.GetInteger() != 0 &&
        r_pathTracingNeeCacheMode.GetInteger() != 0 &&
        (neeCacheSceneBuildSourceDomain == 0 || neeCacheSceneBuildSourceDomain == 1 || neeCacheSceneBuildSourceDomain == 3);
    const bool currentFrameStaticEmissiveProducerPolicy =
        restirPTDebugMode || pdfNeeVerifierStaticEmissiveProducerPolicy;
    const bool cleanRtxdiDiMaterialValidationRoute =
        requestedDebugMode == 0 &&
        r_pathTracingCleanRtxdiDiEnable.GetInteger() != 0 &&
        (cleanRtxdiDiSceneBuildResolveView == 16 || cleanRtxdiDiMaterialClassifierProofRoute);
    const bool enableTextureProbe = (requestedDebugMode >= 8 && requestedDebugMode <= 20) || currentFrameStaticEmissiveProducerPolicy || cleanRtxdiDiSceneBuildRluEmissives || cleanRtxdiDiMaterialValidationRoute || neeCacheSceneBuildRluEmissives || integratorDebugMode || requestedDebugMode == 38 || requestedDebugMode == 39 || requestedDebugMode == 40 || requestedDebugMode == 41 || requestedDebugMode == 42 || requestedDebugMode == 43 || requestedDebugMode == 44 || requestedDebugMode == 45 || requestedDebugMode == 46 || requestedDebugMode == 47 || requestedDebugMode == 48 || requestedDebugMode == 49 || requestedDebugMode == 57;

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
            m_frameResources.MarkResetReason(RT_FRAME_RESET_SCENE_RESOURCES | RT_FRAME_RESET_RESERVOIR_SCENE_SIGNATURE);
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
        m_frameResources.smokeReservoirNeedsClear = true;
        m_frameResources.smokeReservoirResetCount = 0;
        m_frameResources.smokeReservoirClearCount = 0;
        m_frameResources.MarkResetReason(RT_FRAME_RESET_SCENE_RESOURCES | RT_FRAME_RESET_RESERVOIR_SCENE_SIGNATURE);
        m_smokeSceneRenderWorld = renderWorld;
        m_smokeSceneMapName = renderWorld->mapName;
        m_smokeSceneMapTimeStamp = renderWorld->mapTimeStamp;
        m_smokeSceneMapLoadSerial = renderWorld->mapLoadSerial;
    }
    if (viewDef && m_smokeLightUniverseRenderWorld != viewDef->renderWorld)
    {
        m_smokeLightUniverse.Clear();
        m_remixFramePrepare.Clear();
        m_remixLightManager.Clear();
        m_remixRtxdiResources.Clear();
        m_restirLightManager.Clear();
        m_smokeLightUniverseRenderWorld = viewDef->renderWorld;
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
        m_smokeBoundsOverlayCameraOrigin = viewDef->renderView.vieworg;
        m_smokeBoundsOverlayCameraForward = viewDef->renderView.viewaxis[0];
        m_smokeBoundsOverlayCameraLeft = viewDef->renderView.viewaxis[1];
        m_smokeBoundsOverlayCameraUp = viewDef->renderView.viewaxis[2];
        m_smokeBoundsOverlayCameraForward.Normalize();
        m_smokeBoundsOverlayCameraLeft.Normalize();
        m_smokeBoundsOverlayCameraUp.Normalize();
        m_smokeBoundsOverlayTanX = idMath::Tan(DEG2RAD(viewDef->renderView.fov_x * 0.5f));
        m_smokeBoundsOverlayTanY = idMath::Tan(DEG2RAD(viewDef->renderView.fov_y * 0.5f));
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
    RtSmokeSkinnedGpuScaffoldBuild skinnedGpuScaffold;
    int skinnedPreviousBridgeMs = 0;
    int skinnedGpuScaffoldMs = 0;
    int skinnedTriangleDispatchIndexMs = 0;
    int skinnedRetainJointsMs = 0;
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
    const bool dumpClassReasons = r_pathTracingClassDump.GetInteger() != 0;
    RtSmokeSurfaceClassReasonSamples reasonSamples;
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
            requestedDebugMode == 23 || requestedDebugMode == 24 || requestedDebugMode == 25 ||
            requestedDebugMode == 39 ||
            requestedDebugMode == 40 ||
            requestedDebugMode == 41 ||
            requestedDebugMode == 47 ||
            requestedDebugMode == 48 ||
            requestedDebugMode == 49 ||
            requestedDebugMode == 52 ||
            requestedDebugMode == 42 ||
            requestedDebugMode == 43 ||
            cleanRtxdiDiSceneBuildRluEmissives ||
            restirPTDebugMode ||
            integratorDebugMode ||
            (requestedDebugMode == 18 && r_pathTracingRigidRouteMode18.GetInteger() != 0) ||
            (requestedDebugMode == 20 && r_pathTracingRigidRouteMode20.GetInteger() != 0));
    const bool rigidResidencyBoundsDebug = requestedDebugMode == 21 || requestedDebugMode == 22;
    const bool rigidResidencyEnabled =
        r_pathTracingRigidResidency.GetInteger() != 0 &&
        (enableRigidRouteForMode || rigidResidencyBoundsDebug);
    const int source2RigidEntities = sceneSource == 2 ? idMath::ClampInt(0, 2, r_pathTracingSceneSource2RigidEntities.GetInteger()) : 0;
    const bool dumpSceneUniverse = r_pathTracingSceneUniverseDump.GetInteger() != 0;
    const bool dumpInstanceUniverse = r_pathTracingInstanceUniverseDump.GetInteger() != 0;
    const bool dumpRigidMeshUniverse = r_pathTracingRigidMeshUniverseDump.GetInteger() != 0;
    {
        OPTICK_EVENT("PT EntityFeed Debug Probes");
        DumpEntityFeedSingleBoneDiagnostics(viewDef);
        DumpEntityFeedJointAdvanceProbe(viewDef);
        DumpEntityFeedReachableCandidateStats(viewDef);
    }
    const RtSmokeStaticDrawSurfCounts currentStaticDrawSurfs = useSceneUniverseStaticGeometry ? CountCurrentStaticDrawSurfs(viewDef) : RtSmokeStaticDrawSurfCounts();
    if (sceneSource != m_smokeSceneSourceLast || (useSceneUniverseStaticGeometry && source2RigidEntities != m_smokeSceneSource2RigidEntitiesLast))
    {
        common->Printf("PathTracePrimaryPass: PT scene source changed %d/%d -> %d/%d; clearing static geometry cache\n",
            m_smokeSceneSourceLast,
            m_smokeSceneSource2RigidEntitiesLast,
            sceneSource,
            source2RigidEntities);
        m_smokeGeometryUniverse.Clear();
        m_smokeSkinnedSurfaceRecords.clear();
        m_smokePreviousSkinnedSurfaceRecords.clear();
        m_smokePreviousSkinnedVertexData.clear();
        m_smokePreviousSkinnedJointMatrices.clear();
        m_smokePreviousStaticTriangleMaterialIndexes.clear();
        m_smokePreviousStaticSnapshotUploadSignature = 0;
        m_smokePreviousStaticMaterialIndexUploadSignature = 0;
        m_smokeStaticTriangleMaterialUploadSignature = 0;
        m_smokeStaticTriangleMaterialIndexUploadSignature = 0;
        m_smokeStaticTriangleMaterialUploadSignatureValid = false;
        m_smokeStaticTriangleMaterialIndexUploadSignatureValid = false;
        m_smokeSkinnedPreviousStats = RtSmokeSkinnedPreviousFrameStats();
        m_smokeStaticBlasCacheValid = false;
        m_smokeStaticBlasSignature = 0;
        m_smokeSceneUniverseStaticBuildGeneration = 0;
        m_smokeSceneRebuildLogged = false;
        m_smokeSceneSourceLast = sceneSource;
        m_smokeSceneSource2RigidEntitiesLast = source2RigidEntities;
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
            m_smokePreviousSkinnedSurfaceRecords.clear();
            m_smokePreviousSkinnedVertexData.clear();
            m_smokePreviousSkinnedJointMatrices.clear();
            m_smokePreviousStaticTriangleMaterialIndexes.clear();
            m_smokePreviousStaticSnapshotUploadSignature = 0;
            m_smokePreviousStaticMaterialIndexUploadSignature = 0;
            m_smokeStaticTriangleMaterialUploadSignature = 0;
            m_smokeStaticTriangleMaterialIndexUploadSignature = 0;
            m_smokeStaticTriangleMaterialUploadSignatureValid = false;
            m_smokeStaticTriangleMaterialIndexUploadSignatureValid = false;
            m_smokeSkinnedPreviousStats = RtSmokeSkinnedPreviousFrameStats();
            m_smokeStaticBlasCacheValid = false;
            m_smokeStaticBlasSignature = 0;
            m_smokeSceneUniverseStaticBuildGeneration = 0;
            m_smokeSceneRebuildLogged = false;
        }
    }
    if (useSceneUniverseStaticGeometry && source2RigidEntities != 0)
    {
        m_smokeGeometryUniverse.Clear();
        m_smokeSkinnedSurfaceRecords.clear();
        m_smokePreviousSkinnedSurfaceRecords.clear();
        m_smokePreviousSkinnedVertexData.clear();
        m_smokePreviousSkinnedJointMatrices.clear();
        m_smokePreviousStaticTriangleMaterialIndexes.clear();
        m_smokePreviousStaticSnapshotUploadSignature = 0;
        m_smokePreviousStaticMaterialIndexUploadSignature = 0;
        m_smokeStaticTriangleMaterialUploadSignature = 0;
        m_smokeStaticTriangleMaterialIndexUploadSignature = 0;
        m_smokeStaticTriangleMaterialUploadSignatureValid = false;
        m_smokeStaticTriangleMaterialIndexUploadSignatureValid = false;
        m_smokeSkinnedPreviousStats = RtSmokeSkinnedPreviousFrameStats();
        m_smokeStaticBlasCacheValid = false;
        m_smokeStaticBlasSignature = 0;
        m_smokeSceneUniverseStaticBuildGeneration = 0;
    }
    RtPathTraceSceneUniverseBuildStats sceneUniverseStaticBuildStats;
    bool drawSurfMirrorFrameProducedFromDynamicCapture = false;
    {
        OPTICK_EVENT("PT Capture Doom Surfaces");
        m_smokeGeometryUniverse.BeginFrame(++m_smokeGeometryFrameIndex, viewDef ? viewDef->renderWorld : nullptr);
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
        }
        if (useDrawSurfMirrorDynamicFrame)
        {
            {
                OPTICK_EVENT("PT Capture Visible Doom Surfaces");
                usingDoomSurfaces = CaptureDoomSurfacesForSmokeTest(viewDef, dynamicVertexData, dynamicIndexData, dynamicTriangleClassData, dynamicTriangleMaterialData, &dynamicTriangleInstanceData, &dynamicTriangleIdentityData, m_smokeGeometryUniverse, staticCacheChanged, m_smokeSceneOrigin, sourceSurfaces, sourceVerts, sourceIndexes, anchorTriangle, classStats, skipStats, dynamicStats, attributeStats, materialStats, bucketRanges, captureTiming, dumpClassReasons ? &reasonSamples : nullptr, &currentSkinnedSurfaceRecords, false, false, true);
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
                    idMath::ClampInt(0, 8, r_pathTracingStaticAreaPreloadPortalSteps.GetInteger()),
                    r_pathTracingStaticAreaPreloadDump.GetInteger() != 0);
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
                if (r_pathTracingStaticAreaPreloadDump.GetInteger() != 0)
                {
                    r_pathTracingStaticAreaPreloadDump.SetInteger(0);
                }
            }
            const RtSmokeSurfaceClassStats staticClassStats = classStats;
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
            RtSmokeSurfaceClassReasonSamples mirrorReasonSamples;
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
            const bool usingMirrorDynamicFrame = CapturePathTraceDynamicFrameFromDrawSurfMirror(viewDef, nullptr, &m_smokeGeometryUniverse, dynamicVertexData, dynamicIndexData, dynamicTriangleClassData, dynamicTriangleMaterialData, &dynamicTriangleInstanceData, &dynamicTriangleIdentityData, mirrorSourceSurfaces, mirrorSourceVerts, mirrorSourceIndexes, mirrorClassStats, mirrorSkipStats, mirrorDynamicStats, mirrorAttributeStats, mirrorMaterialStats, mirrorBucketRanges, mirrorCaptureTiming, dumpClassReasons ? &mirrorReasonSamples : nullptr, &currentSkinnedSurfaceRecords, nullptr, &m_instanceUniverse, &m_smokeBoundsOverlayLines, drawSurfMirrorFullDiagnostics);

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
                dynamicStats = mirrorDynamicStats;
                attributeStats = mirrorAttributeStats;
                materialStats = mirrorMaterialStats;
                bucketRanges = mirrorBucketRanges;
                bucketRanges.buckets[0] = staticBucketRange;
                captureTiming.dynamicPassClassifyMs += mirrorCaptureTiming.dynamicPassClassifyMs;
                captureTiming.dynamicAppendMs += mirrorCaptureTiming.dynamicAppendMs;
                captureTiming.rtCpuSkinningAppendMs += mirrorCaptureTiming.rtCpuSkinningAppendMs;
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
                usingDoomSurfaces = CaptureDoomSurfacesForSmokeTest(viewDef, dynamicVertexData, dynamicIndexData, dynamicTriangleClassData, dynamicTriangleMaterialData, &dynamicTriangleInstanceData, &dynamicTriangleIdentityData, m_smokeGeometryUniverse, staticCacheChanged, m_smokeSceneOrigin, sourceSurfaces, sourceVerts, sourceIndexes, anchorTriangle, classStats, skipStats, dynamicStats, attributeStats, materialStats, bucketRanges, captureTiming, dumpClassReasons ? &reasonSamples : nullptr, &currentSkinnedSurfaceRecords, useSceneUniverseStaticGeometry, source2RigidEntities != 0);
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
            const bool diagnosticAreaWalk = residencyV2 && r_pathTracingRigidResidencyDump.GetInteger() != 0;
            if (rigidResidencyEnabled && (!residencyV2 || diagnosticAreaWalk))
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
    const int previousSkinnedSurfaceRecordCountBeforeBridge = static_cast<int>(m_smokePreviousSkinnedSurfaceRecords.size());
    const int previousSkinnedVertexDataCountBeforeBridge = static_cast<int>(m_smokePreviousSkinnedVertexData.size());
    const int previousSkinnedJointMatrixCountBeforeBridge = static_cast<int>(m_smokePreviousSkinnedJointMatrices.size());
    std::vector<PathTraceSmokeVertex> nextPreviousSkinnedVertexData;
    std::vector<PathTraceSkinnedJointMatrix> nextPreviousSkinnedJointMatrices;
    {
        OPTICK_EVENT("PT Skinned Previous Bridge");
        const int skinnedPreviousBridgeStartMs = Sys_Milliseconds();
        m_smokeSkinnedPreviousStats = UpdateSmokeSkinnedPreviousCpuBridge(
            currentSkinnedSurfaceRecords,
            m_smokePreviousSkinnedSurfaceRecords,
            m_smokePreviousSkinnedVertexData,
            dynamicVertexData,
            nextPreviousSkinnedVertexData);
        skinnedPreviousBridgeMs = Sys_Milliseconds() - skinnedPreviousBridgeStartMs;
    }
    const int gpuSkinningMode = idMath::ClampInt(0, 2, r_pathTracingGpuSkinning.GetInteger());
    const bool rrGuideNeedsSkinnedHistory =
        requestedDebugMode == 56 &&
        r_pathTracingRestirPTPrimarySurfacePrepass.GetInteger() != 0;
    const bool skinnedMotionBridgeNeedsScaffold =
        rrGuideNeedsSkinnedHistory ||
        r_pathTracingMotionVectorExport.GetInteger() != 0 ||
        r_pathTracingDLSSRRGuideDebugView.GetInteger() != 0;
    const int skinnedScaffoldMode = skinnedMotionBridgeNeedsScaffold ? Max(1, gpuSkinningMode) : gpuSkinningMode;
    const bool buildSkinnedGpuSkinningInputs = gpuSkinningMode > 0;
    {
        OPTICK_EVENT("PT Skinned GPU Scaffold");
        const int skinnedGpuScaffoldStartMs = Sys_Milliseconds();
        skinnedGpuScaffold = BuildSmokeSkinnedGpuScaffold(
            skinnedScaffoldMode,
            buildSkinnedGpuSkinningInputs,
            currentSkinnedSurfaceRecords,
            m_smokePreviousSkinnedSurfaceRecords,
            dynamicVertexData,
            m_smokePreviousSkinnedVertexData,
            m_smokePreviousSkinnedJointMatrices);
        skinnedGpuScaffoldMs = Sys_Milliseconds() - skinnedGpuScaffoldStartMs;
    }
    {
        OPTICK_EVENT("PT Skinned Triangle Dispatch Index");
        const int skinnedTriangleDispatchIndexStartMs = Sys_Milliseconds();
        BuildSmokeSkinnedTriangleDispatchIndex(skinnedGpuScaffold, static_cast<int>(dynamicIndexData.size() / 3));
        skinnedTriangleDispatchIndexMs = Sys_Milliseconds() - skinnedTriangleDispatchIndexStartMs;
    }
    {
        OPTICK_EVENT("PT Retain Skinned Joints");
        const int skinnedRetainJointsStartMs = Sys_Milliseconds();
        RetainSmokeSkinnedCurrentJointMatrices(
            currentSkinnedSurfaceRecords,
            nextPreviousSkinnedJointMatrices);
        skinnedRetainJointsMs = Sys_Milliseconds() - skinnedRetainJointsStartMs;
    }
    m_smokeSkinnedSurfaceRecords = currentSkinnedSurfaceRecords;
    m_smokePreviousSkinnedSurfaceRecords = m_smokeSkinnedSurfaceRecords;
    m_smokePreviousSkinnedVertexData.swap(nextPreviousSkinnedVertexData);
    m_smokePreviousSkinnedJointMatrices.swap(nextPreviousSkinnedJointMatrices);

    if (useDrawSurfMirrorDynamicFrame && r_pathTracingSceneSourceCompare.GetInteger() != 0)
    {
        DumpSource3CaptureCompare(
            viewDef,
            m_smokeGeometryFrameIndex,
            classStats,
            skipStats,
            sourceSurfaces,
            sourceVerts,
            sourceIndexes,
            dynamicIndexData,
            dynamicTriangleMaterialData);
        r_pathTracingSceneSourceCompare.SetInteger(0);
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
                return m_smokeGeometryUniverse.UpdateRigidBlasGpuScaffold(device, commandList, rigidBlasGpuBuild);
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
    if (sceneSource > 0 || dumpSceneUniverse)
    {
        OPTICK_EVENT("PT Scene Universe Diagnostics");
        const int drawSurfStaticSurfaces = useSceneUniverseStaticGeometry ? currentStaticDrawSurfs.surfaces : classStats.staticWorldSurfaces;
        const int drawSurfStaticTriangles = useSceneUniverseStaticGeometry ? currentStaticDrawSurfs.triangles : classStats.staticWorldTriangles;
        m_sceneUniverse.RunDiagnostics(viewDef, &m_smokeGeometryUniverse, sceneSource, dumpSceneUniverse, drawSurfStaticSurfaces, drawSurfStaticTriangles);
        if (dumpSceneUniverse && useSceneUniverseStaticGeometry)
        {
            common->Printf("PathTracePrimaryPass: PT scene source2 staticBuild built=%d cacheHit=%d surfaces=%d triangles=%d verts=%d indexes=%d emissiveSurfaces=%d rigidEntities=%d/%d skipped invalid/limits/zero=%d/%d/%d sourceTotals=%d/%d/%d\n",
                sceneUniverseStaticBuildStats.built ? 1 : 0,
                sceneUniverseStaticBuildStats.cacheHit ? 1 : 0,
                sceneUniverseStaticBuildStats.surfaces,
                sceneUniverseStaticBuildStats.triangles,
                sceneUniverseStaticBuildStats.vertices,
                sceneUniverseStaticBuildStats.indexes,
                sceneUniverseStaticBuildStats.emissiveCapableSurfaces,
                sceneUniverseStaticBuildStats.rigidEntitySurfaces,
                sceneUniverseStaticBuildStats.rigidEntityTriangles,
                sceneUniverseStaticBuildStats.skippedInvalid,
                sceneUniverseStaticBuildStats.skippedLimits,
                sceneUniverseStaticBuildStats.skippedZeroArea,
                sourceSurfaces,
                sourceVerts,
                sourceIndexes);
        }
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

    RtSmokeMaterialMetadataRegistrationTiming metadataTiming;
    {
        OPTICK_EVENT("PT Register Material Metadata");
        metadataTiming = RegisterSmokeMaterialTextureInfoForFrame(viewDef, enableTextureProbe);
        if (r_pathTracingWorldStaticEmissives.GetInteger() != 0 || useSceneUniverseStaticGeometry)
        {
            const RtSmokeMaterialMetadataRegistrationTiming worldStaticMetadataTiming = RegisterSmokeWorldStaticMaterialTextureInfo(viewDef, enableTextureProbe);
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
        const bool materialHydrationIdsCacheHit =
            m_smokeMaterialHydrationIdsValid &&
            m_smokeMaterialHydrationStaticGeneration == materialHydrationStaticGeneration &&
            m_smokeMaterialHydrationStaticTriangleMaterialCount == materialHydrationStaticTriangleMaterialCount &&
            m_smokeMaterialHydrationEmissiveSignature == materialHydrationEmissiveSignature &&
            m_smokeMaterialHydrationRigidSignature == materialHydrationRigidSignature;
        if (!materialHydrationIdsCacheHit)
        {
            OPTICK_EVENT("PT Material Hydration Unique IDs");
            m_smokeMaterialHydrationIds = BuildSortedUniqueMaterialIds(materialTableStaticIds);
            m_smokeMaterialHydrationIdsValid = true;
            m_smokeMaterialHydrationStaticGeneration = materialHydrationStaticGeneration;
            m_smokeMaterialHydrationStaticTriangleMaterialCount = materialHydrationStaticTriangleMaterialCount;
            m_smokeMaterialHydrationEmissiveSignature = materialHydrationEmissiveSignature;
            m_smokeMaterialHydrationRigidSignature = materialHydrationRigidSignature;
        }
        materialHydrationIds = &m_smokeMaterialHydrationIds;
        const RtSmokeMaterialMetadataRegistrationTiming cachedStaticMetadataTiming =
            RegisterSmokeMaterialTextureInfoForMaterialIds(*materialHydrationIds, enableTextureProbe);
        metadataTiming.metadataMs += cachedStaticMetadataTiming.metadataMs;
        metadataTiming.registrationMs += cachedStaticMetadataTiming.registrationMs;
    }
    const int metadataMs = metadataTiming.metadataMs;
    const int metadataValidationMs = metadataTiming.validationMs;
    const int metadataRegistrationMs = metadataTiming.registrationMs;
    uint64 materialTableSignature = 0;
    bool materialTableCacheHit = false;
    RtSmokeMaterialTableCompareStats materialUniverseTableCompareStats;
    const bool useMaterialUniverseTable = r_pathTracingMaterialUniverseTable.GetInteger() != 0;
    const bool validateMaterialUniverseTable = r_pathTracingMaterialUniverseTableValidate.GetInteger() != 0;
    const char* materialTablePath = useMaterialUniverseTable ? "universe" : "legacy";
    const int materialTextureTableMinimum = cleanRtxdiDiMaterialValidationRoute ? RT_SMOKE_TEXTURE_EXPERIMENTAL_ACTIVE_CAP : 0;
    {
        OPTICK_EVENT("PT Material Table Build");
        BeginSmokeMaterialUniverseFrame();
        if (useMaterialUniverseTable)
        {
            if (validateMaterialUniverseTable)
            {
                RtSmokeMaterialTableBuild legacyMaterialTable;
                uint32_t legacyLatchedTextureProbeMaterialId = m_smokeTextureProbeMaterialId;
                int legacyLatchedTextureProbeRequestedIndex = m_smokeTextureProbeRequestedIndex;
                BuildSmokeMaterialTableCached(legacyMaterialTable, materialTableStaticIds, dynamicTriangleMaterialData, legacyLatchedTextureProbeMaterialId, legacyLatchedTextureProbeRequestedIndex, enableTextureProbe, materialTextureTableMinimum, materialTableSignature, materialTableCacheHit);
                BuildSmokeMaterialTableFromUniverseCached(materialTable, materialTableStaticIds, dynamicTriangleMaterialData, m_smokeTextureProbeMaterialId, m_smokeTextureProbeRequestedIndex, enableTextureProbe, materialTextureTableMinimum, materialTableSignature, materialTableCacheHit);
                materialUniverseTableCompareStats = CompareSmokeMaterialTables(legacyMaterialTable, materialTable);
                if (materialUniverseTableCompareStats.mismatches > 0)
                {
                    common->Printf("PathTracePrimaryPass: RT smoke material universe table mismatch, falling back to legacy table for this frame (mismatches=%d material=%d/%d/%d indexes=%d/%d textures=%d/%d)\n",
                        materialUniverseTableCompareStats.mismatches,
                        materialUniverseTableCompareStats.materialCountMismatches,
                        materialUniverseTableCompareStats.materialIdMismatches,
                        materialUniverseTableCompareStats.materialRecordMismatches,
                        materialUniverseTableCompareStats.staticIndexMismatches,
                        materialUniverseTableCompareStats.dynamicIndexMismatches,
                        materialUniverseTableCompareStats.textureCountMismatches,
                        materialUniverseTableCompareStats.textureHandleMismatches);
                    materialTable = legacyMaterialTable;
                    m_smokeTextureProbeMaterialId = legacyLatchedTextureProbeMaterialId;
                    m_smokeTextureProbeRequestedIndex = legacyLatchedTextureProbeRequestedIndex;
                    materialTablePath = "legacyFallback";
                }
            }
            else
            {
                BuildSmokeMaterialTableFromUniverseCached(materialTable, materialTableStaticIds, dynamicTriangleMaterialData, m_smokeTextureProbeMaterialId, m_smokeTextureProbeRequestedIndex, enableTextureProbe, materialTextureTableMinimum, materialTableSignature, materialTableCacheHit);
            }
        }
        else
        {
            BuildSmokeMaterialTableCached(materialTable, materialTableStaticIds, dynamicTriangleMaterialData, m_smokeTextureProbeMaterialId, m_smokeTextureProbeRequestedIndex, enableTextureProbe, materialTextureTableMinimum, materialTableSignature, materialTableCacheHit);
            if (validateMaterialUniverseTable)
            {
                RtSmokeMaterialTableBuild universeMaterialTable;
                uint32_t universeLatchedTextureProbeMaterialId = m_smokeTextureProbeMaterialId;
                int universeLatchedTextureProbeRequestedIndex = m_smokeTextureProbeRequestedIndex;
                BuildSmokeMaterialTableFromUniverse(universeMaterialTable, materialTableStaticIds, dynamicTriangleMaterialData, universeLatchedTextureProbeMaterialId, universeLatchedTextureProbeRequestedIndex, enableTextureProbe, materialTextureTableMinimum);
                materialUniverseTableCompareStats = CompareSmokeMaterialTables(materialTable, universeMaterialTable);
            }
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
    const std::vector<PathTraceSmokeMaterial> stableGpuMaterialTableMaterials = materialTable.materials;
    {
        OPTICK_EVENT("PT Runtime Material Registers");
        ApplySmokeRuntimeMaterialRegistersToTable(viewDef, materialTable, materialStats, materialTextureTableMinimum);
    }
    const std::vector<PathTraceDynamicMaterialRecord> dynamicMaterialRecords = [&]() {
        OPTICK_EVENT("PT Dynamic Material Records");
        return BuildSmokeDynamicMaterialRecords(materialTable, materialStats, viewDef);
    }();
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
    materialDiagnosticDesc.viewDef = viewDef;
    materialDiagnosticDesc.materialTable = &materialTable;
    materialDiagnosticDesc.materialStats = &materialStats;
    materialDiagnosticDesc.enableTextureProbe = enableTextureProbe;
    {
        OPTICK_EVENT("PT Material Diagnostic Triggers");
        RunSmokeMaterialDiagnosticTriggers(materialDiagnosticDesc);
    }

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
            dynamicTriangleMaterialData,
            dynamicMaterialRecords);
    }();
    const int rigidRouteTexMatrixVertices = [&]() {
        OPTICK_EVENT("PT Rigid Route Tex Matrix Apply");
        return ApplySmokeDynamicMaterialTexMatricesToVertices(
            rigidRouteBuild.vertices,
            rigidRouteBuild.indexes,
            rigidRouteBuild.triangleMaterialIndexes,
            dynamicMaterialRecords);
    }();
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
        (dynamicTexMatrixVertices > 0 || rigidRouteTexMatrixVertices > 0) &&
        (m_smokeGeometryFrameIndex % 120ull) == 1ull)
    {
        common->Printf(
            "PathTracePrimaryPass: RT smoke dynamic material tex matrices applied dynamicVerts=%d rigidRouteVerts=%d records=%d\n",
            dynamicTexMatrixVertices,
            rigidRouteTexMatrixVertices,
            static_cast<int>(dynamicMaterialRecords.size()));
    }

    RtSmokeEmissiveInventoryStats emissiveInventoryStats;
    const int emissiveStartMs = Sys_Milliseconds();
    std::vector<PathTraceSmokeEmissiveTriangle> emissiveTriangles;
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
            dynamicVertexData,
            dynamicIndexData,
            dynamicTriangleClassData,
            materialTable.dynamicMaterialIndexes,
            dynamicTriangleInstanceData,
            dynamicTriangleIdentityData,
            RT_SMOKE_MATERIAL_EMISSIVE,
            RT_SMOKE_TRIANGLE_CLASS_MASK,
            static_cast<uint32_t>(RtSmokeSurfaceClass::SkinnedDeformed),
            maxEmissiveRecords,
            emissiveInventoryStats);
        if (enableRigidRouteForMode && (requestedDebugMode == 20 || cleanRtxdiDiSceneBuildRluEmissives || neeCacheSceneBuildRluEmissives || restirPTDebugMode || integratorDebugMode))
        {
            AppendSmokeRigidRouteEmissiveTriangleInventory(
                materialTable.materialIds,
                materialTable.materials,
                rigidRouteBuild,
                RT_SMOKE_MATERIAL_EMISSIVE,
                maxEmissiveRecords,
                emissiveTriangles,
                emissiveInventoryStats);
        }
        if (r_pathTracingWorldStaticEmissives.GetInteger() != 0)
        {
            const int fullLevelStaticSupplementCap = idMath::ClampInt(0, maxEmissiveRecords, r_pathTracingWorldStaticEmissiveMaxTriangles.GetInteger());
            const int fullLevelStaticSupplementLimit = Min(maxEmissiveRecords, static_cast<int>(emissiveTriangles.size()) + fullLevelStaticSupplementCap);
            AppendSmokeWorldStaticEmissiveTriangleInventory(
                viewDef,
                materialTable.materialIds,
                materialTable.materials,
                RT_SMOKE_MATERIAL_EMISSIVE,
                SmokeSurfaceClassId(RtSmokeSurfaceClass::StaticWorld),
                fullLevelStaticSupplementLimit,
                emissiveTriangles,
                emissiveInventoryStats);
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
        FinalizeSmokeEmissiveTriangleSamplingFields(emissiveTriangles, emissiveInventoryStats);
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
        lightCandidates = BuildSmokeLightCandidateBufferRecords(emissiveInventoryStats);
    }
    const std::vector<PathTraceSmokeEmissiveTriangle> previousEmissiveTriangles = m_sceneInputs.valid
        ? m_smokePreviousEmissiveTriangles
        : std::vector<PathTraceSmokeEmissiveTriangle>();
    const std::vector<PathTraceEmissiveLightRemap> emissiveLightRemap = [&]() {
        OPTICK_EVENT("PT Emissive Light Remap");
        return BuildSmokeEmissiveLightRemap(emissiveTriangles, previousEmissiveTriangles);
    }();
    const bool restirPTAnalyticLightCandidates = restirPTDebugMode && r_pathTracingRestirPTAnalyticLightCandidates.GetInteger() != 0;
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
    const bool enableDoomAnalyticLightCandidates = r_pathTracingAnalyticLightCandidates.GetInteger() != 0 || restirPTAnalyticLightCandidates;
    PathTraceDoomAnalyticLightBuildOptions doomAnalyticBuildOptions;
    if (restirPTAnalyticLightCandidates)
    {
        doomAnalyticBuildOptions.forceBuild = true;
        doomAnalyticBuildOptions.preserveZeroRadianceSlots = true;
        doomAnalyticBuildOptions.stableReservoirOrder = true;
        doomAnalyticBuildOptions.includeOutOfSelectedArea = true;
        doomAnalyticBuildOptions.ignoreConfiguredCandidateCap = true;
    }
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
    RtSmokeEmissiveInventoryDiagnosticTriggerDesc emissiveInventoryDiagnosticDesc;
    emissiveInventoryDiagnosticDesc.materialTable = &materialTable;
    emissiveInventoryDiagnosticDesc.emissiveTriangles = &emissiveTriangles;
    emissiveInventoryDiagnosticDesc.emissiveInventoryStats = &emissiveInventoryStats;
    {
        OPTICK_EVENT("PT Emissive Inventory Diagnostics");
        RunSmokeEmissiveInventoryDiagnosticTriggers(emissiveInventoryDiagnosticDesc);
    }
    const int runtimeInactiveEmissiveTriangles = emissiveInventoryStats.skippedRuntimeInactiveTriangles;
    // The old RT smoke emissive light universe is intentionally no longer a
    // producer. The Remix Light Universe below owns current/previous light
    // domains and remaps; keep raw emissive extraction as its input.
    m_smokeLightUniverse.Clear();
    const std::vector<PathTraceRestirLightObservation> restirLightManagerObservations = [&]() {
        OPTICK_EVENT("PT Restir Light Observations");
        return BuildPathTraceRestirLightManagerObservations(
            emissiveTriangles,
            doomAnalyticLights,
            doomAnalyticRemap.currentCandidateIdentities);
    }();
    {
        OPTICK_EVENT("PT Remix Frame Prepare");
        PathTraceRemixFramePrepareDesc remixFramePrepareDesc;
        remixFramePrepareDesc.frameIndex = m_smokeGeometryFrameIndex;
        remixFramePrepareDesc.resetReasonFlags = m_frameResources.settings.resetReasonFlags;
        remixFramePrepareDesc.previousSceneInputsValid = m_sceneInputs.valid;
        remixFramePrepareDesc.sceneSource = sceneSource;
        remixFramePrepareDesc.debugMode = requestedDebugMode;
        remixFramePrepareDesc.outputWidth = m_frameResources.width;
        remixFramePrepareDesc.outputHeight = m_frameResources.height;
        m_remixFramePrepare.BeginFrame(remixFramePrepareDesc);
        PathTraceRemixFramePrepareLightInputs remixFramePrepareLightInputs;
        remixFramePrepareLightInputs.emissiveObservationCount = static_cast<uint32_t>(emissiveTriangles.size());
        remixFramePrepareLightInputs.previousEmissiveObservationCount = static_cast<uint32_t>(previousEmissiveTriangles.size());
        remixFramePrepareLightInputs.doomAnalyticObservationCount = static_cast<uint32_t>(doomAnalyticLights.size());
        remixFramePrepareLightInputs.previousDoomAnalyticObservationCount = static_cast<uint32_t>(doomAnalyticRemap.previousCandidates.size());
        remixFramePrepareLightInputs.doomAnalyticIdentityCount = static_cast<uint32_t>(doomAnalyticRemap.currentCandidateIdentities.size());
        remixFramePrepareLightInputs.previousDoomAnalyticIdentityCount = static_cast<uint32_t>(doomAnalyticRemap.previousCandidateIdentities.size());
        remixFramePrepareLightInputs.restirLightObservationCount = static_cast<uint32_t>(restirLightManagerObservations.size());
        m_remixFramePrepare.SetLightInputs(remixFramePrepareLightInputs);
        m_remixFramePrepare.EndFrame();
    }
    if (r_pathTracingRemixFramePrepareDump.GetInteger() != 0)
    {
        const PathTraceRemixFramePrepareObservationPackage& remixFramePackage = m_remixFramePrepare.GetObservationPackage();
        const PathTraceRemixFramePrepareStats& remixFrameStats = m_remixFramePrepare.GetStats();
        common->Printf("PathTracePrimaryPass: Remix frame prepare frame=%llu source=%d debugMode=%d output=%dx%d previousScene=%d resetFlags=0x%x structuralReset=0x%x reservoirReset=0x%x lights emissive current/previous=%u/%u doomAnalytic current/previous=%u/%u identities current/previous=%u/%u restirObservations=%u counts begin/end/lightUpdates=%u/%u/%u payloadObservations=%u mappingObservations=%u oldSmokeReservoirSignatureConsulted=%u resourceAllocations=%u shaderRoutes=%u behavior=cpu-diagnostics-only\n",
            static_cast<unsigned long long>(remixFramePackage.frameIndex),
            remixFramePackage.sceneSource,
            remixFramePackage.debugMode,
            remixFramePackage.outputWidth,
            remixFramePackage.outputHeight,
            remixFramePackage.previousSceneInputsValid ? 1 : 0,
            remixFramePackage.resetReasonFlags,
            remixFrameStats.structuralResetReasonFlags,
            remixFrameStats.reservoirResetReasonFlags,
            remixFramePackage.lights.emissiveObservationCount,
            remixFramePackage.lights.previousEmissiveObservationCount,
            remixFramePackage.lights.doomAnalyticObservationCount,
            remixFramePackage.lights.previousDoomAnalyticObservationCount,
            remixFramePackage.lights.doomAnalyticIdentityCount,
            remixFramePackage.lights.previousDoomAnalyticIdentityCount,
            remixFramePackage.lights.restirLightObservationCount,
            remixFrameStats.beginFrameCount,
            remixFrameStats.endFrameCount,
            remixFrameStats.lightInputUpdateCount,
            remixFrameStats.payloadObservationCount,
            remixFrameStats.mappingObservationCount,
            remixFrameStats.oldSmokeReservoirSignatureConsulted,
            remixFrameStats.resourceAllocationCount,
            remixFrameStats.shaderRouteCount);
        r_pathTracingRemixFramePrepareDump.SetInteger(0);
    }
    const int requestedRestirPTDiDebugView = idMath::ClampInt(0, 77, r_pathTracingRestirPTDiDebugView.GetInteger());
    const bool rrxDiLightUniverseRequested =
        requestedDebugMode == 56 &&
        ((requestedRestirPTDiDebugView >= 60 && requestedRestirPTDiDebugView <= 77) ||
            r_pathTracingRestirPTRrxFinalConsumerOutput.GetInteger() != 0);
    const bool regirLightUniverseRequested =
        r_pathTracingReGIREnable.GetInteger() != 0 &&
        r_pathTracingReGIRMode.GetInteger() != 0;
    const bool cleanRtxdiDiRluRequested =
        cleanRtxdiDiRealAnalyticRoute &&
        r_pathTracingRemixLightUniverseUseForCleanRtxdiDi.GetInteger() != 0;
    const bool pdfNeeRluCurrentProducerRequested =
        requestedDebugMode != 56 &&
        r_pathTracingRestirPdfNeeVerifierEnable.GetInteger() != 0;
    const bool neeCacheRluCurrentProducerRequested =
        requestedDebugMode != 56 &&
        r_pathTracingNeeCacheEnable.GetInteger() != 0 &&
        r_pathTracingNeeCacheMode.GetInteger() != 0;
    const bool remixLightUniverseEnabled =
        r_pathTracingRemixLightUniverseEnable.GetInteger() != 0 ||
        rrxDiLightUniverseRequested ||
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
            : (currentRluDenseProducerRequested ? 2 : (rrxDiLightUniverseRequested ? 2 : regirSceneLightDomain))));
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
    const int remixLightUniverseDump = r_pathTracingRemixLightUniverseDump.GetInteger();
    if (r_pathTracingRemixLightManagerDump.GetInteger() != 0 || remixLightUniverseDump != 0)
    {
        const PathTraceRemixLightManagerStats& remixLightStats = m_remixLightManager.GetStats();
        const char* remixLightUniverseBehavior = remixLightStats.enabled != 0
            ? "rlu-02-dense-current-previous-domain"
            : "legacy-remix-light-manager-compat";
        common->Printf("PathTracePrimaryPass: Remix light universe frame=%llu enabled=%u domain=%u strictMapping=%u resetReason=0x%x current=%u previous=%u maps currentToPrevious size/mapped/invalid/currentOnly=%u/%u/%u/%u previousToCurrent size/mapped/invalid/previousOnly=%u/%u/%u/%u duplicateIdentity=%u typeCounts currentMapped emissive/doom=%u/%u currentOnly emissive/doom=%u/%u previousMapped emissive/doom=%u/%u previousOnly emissive/doom=%u/%u mappedPayloadChanged emissive/doom=%u/%u duplicateIdentity emissive/doom=%u/%u ranges emissive offset/count/samples=%u/%u/%u doomAnalytic offset/count/samples=%u/%u/%u analyticStability currentSampleable/stableCacheable/unstableDynamic=%u/%u/%u reject noRemap/payloadChanged/unprovenContinuity/unknownIdentity/duplicateIdentity/portalDisconnected/outOfSelectedArea=%u/%u/%u/%u/%u/%u/%u sampleContract total/nonEmpty=%u/%u signatures structural/mapping/payload=%llu/%llu/%llu changed=%u/%u/%u payloadOnlyChange=%u firstFailingContract=%u:%s oldSmokeReservoirSignatureConsulted=%u resourceAllocations=%u shaderRoutes=%u behavior=%s\n",
            static_cast<unsigned long long>(remixLightStats.frameIndex),
            remixLightStats.enabled,
            remixLightStats.domain,
            remixLightStats.strictRemixMapping,
            remixLightStats.resetReasonFlags,
            remixLightStats.currentLightCount,
            remixLightStats.previousLightCount,
            remixLightStats.currentToPreviousCount,
            remixLightStats.currentMappedCount,
            remixLightStats.currentInvalidCount,
            remixLightStats.currentOnlyCount,
            remixLightStats.previousToCurrentCount,
            remixLightStats.previousMappedCount,
            remixLightStats.previousInvalidCount,
            remixLightStats.previousOnlyCount,
            remixLightStats.invalidDuplicateIdentityCount,
            remixLightStats.currentMappedByType[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE],
            remixLightStats.currentMappedByType[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC],
            remixLightStats.currentOnlyByType[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE],
            remixLightStats.currentOnlyByType[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC],
            remixLightStats.previousMappedByType[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE],
            remixLightStats.previousMappedByType[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC],
            remixLightStats.previousOnlyByType[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE],
            remixLightStats.previousOnlyByType[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC],
            remixLightStats.mappedPayloadChangedByType[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE],
            remixLightStats.mappedPayloadChangedByType[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC],
            remixLightStats.duplicateIdentityByType[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE],
            remixLightStats.duplicateIdentityByType[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC],
            remixLightStats.emissiveRangeOffset,
            remixLightStats.emissiveRangeCount,
            remixLightStats.emissiveSampleCount,
            remixLightStats.doomAnalyticRangeOffset,
            remixLightStats.doomAnalyticRangeCount,
            remixLightStats.doomAnalyticSampleCount,
            remixLightStats.doomAnalyticCurrentSampleableCount,
            remixLightStats.doomAnalyticStableCacheableCount,
            remixLightStats.doomAnalyticUnstableDynamicCount,
            remixLightStats.doomAnalyticRejectNoRemapCount,
            remixLightStats.doomAnalyticRejectPayloadChangedCount,
            remixLightStats.doomAnalyticRejectUnprovenContinuityCount,
            remixLightStats.doomAnalyticRejectUnknownIdentityCount,
            remixLightStats.doomAnalyticRejectDuplicateIdentityCount,
            remixLightStats.doomAnalyticRejectPortalDisconnectedCount,
            remixLightStats.doomAnalyticRejectOutOfSelectedAreaCount,
            remixLightStats.totalSampleCount,
            remixLightStats.nonEmptyRangeCount,
            static_cast<unsigned long long>(remixLightStats.structuralSignature),
            static_cast<unsigned long long>(remixLightStats.mappingSignature),
            static_cast<unsigned long long>(remixLightStats.payloadSignature),
            remixLightStats.structuralSignatureChanged,
            remixLightStats.mappingSignatureChanged,
            remixLightStats.payloadSignatureChanged,
            remixLightStats.payloadOnlyChange,
            remixLightStats.firstFailingContract,
            RemixLightUniverseContractStatusName(remixLightStats.firstFailingContract),
            remixLightStats.oldSmokeReservoirSignatureConsulted,
            remixLightStats.resourceAllocationCount,
            remixLightStats.shaderRouteCount,
            remixLightUniverseBehavior);
        r_pathTracingRemixLightManagerDump.SetInteger(0);
        if (remixLightUniverseDump == 1)
        {
            r_pathTracingRemixLightUniverseDump.SetInteger(0);
        }
    }
    const bool dumpRemixRtxdiResources = r_pathTracingRemixRtxdiResourcesDump.GetInteger() != 0;
    PathTraceRemixRtxdiResourceGateDesc remixRtxdiResourceGateDesc;
    remixRtxdiResourceGateDesc.restirPTCombinedMode = requestedDebugMode == 56;
    remixRtxdiResourceGateDesc.restirPTDiDebugView = requestedRestirPTDiDebugView;
    remixRtxdiResourceGateDesc.remixRtxdiResourcesEnabled = r_pathTracingRemixRtxdiResourcesEnable.GetInteger() != 0;
    remixRtxdiResourceGateDesc.debugFlatContribution = r_pathTracingRestirPTRrxDebugFlatContribution.GetInteger() != 0;
    remixRtxdiResourceGateDesc.rrxFinalConsumerOutput = r_pathTracingRestirPTRrxFinalConsumerOutput.GetInteger() != 0;
    const bool useRemixRtxdiResources =
        r_pathTracingRemixRtxdiResourcesEnable.GetInteger() != 0 ||
        dumpRemixRtxdiResources ||
        PathTraceRemixRtxdiResourceGateRequestsDiResources(remixRtxdiResourceGateDesc);
    bool remixRtxdiResourcesReady = false;
    if (useRemixRtxdiResources)
    {
        PathTraceRemixRtxdiResourcePrepareDesc remixRtxdiResourceDesc;
        remixRtxdiResourceDesc.device = device;
        remixRtxdiResourceDesc.framePackage = m_remixFramePrepare.GetObservationPackage();
        remixRtxdiResourceDesc.lightManagerStats = m_remixLightManager.GetStats();
        remixRtxdiResourceDesc.checkerboardMode = rtxdi::CheckerboardMode::Off;
        {
            OPTICK_EVENT("PT Remix RTXDI Resources Prepare");
            remixRtxdiResourcesReady = m_remixRtxdiResources.PrepareOutputSizedResources(remixRtxdiResourceDesc);
        }
    }
    else
    {
        m_remixRtxdiResources.Clear();
    }
    if (dumpRemixRtxdiResources)
    {
        const PathTraceRemixRtxdiResourceStats& remixRtxdiStats = m_remixRtxdiResources.GetStats();
        const PathTraceRemixRtxdiDiClearSource remixDiClearSource = PathTraceRemixRtxdiResourceGateDiClearSource(remixRtxdiResourceGateDesc);
        const PathTraceRemixRtxdiReservoirDomain& remixDiDomain = m_remixRtxdiResources.GetDomain(PATH_TRACE_REMIX_RTXDI_RESERVOIR_DOMAIN_DI);
        const PathTraceRemixRtxdiReservoirDomain& remixGiDomain = m_remixRtxdiResources.GetDomain(PATH_TRACE_REMIX_RTXDI_RESERVOIR_DOMAIN_GI);
        common->Printf("PathTracePrimaryPass: Remix RTXDI resources frame=%llu output=%ux%u checkerboard=%u enabled=%u ready=%u reset input/allowed/ignoredSmoke=0x%x/0x%x/0x%x oldSmokeNeedsClear smoke/restir/di/gi=%u/%u/%u/%u DI recreate/reuse/clearPending/clearReason/reset=%u/%u/%u/%u/0x%x GI recreate/reuse/clearPending/clearReason/reset=%u/%u/%u/%u/0x%x arrays DI/stride/elements/bytes=%u/%u/%u/%llu GI/stride/elements/bytes=%u/%u/%u/%llu lightSignatures structural/mapping/payload=%llu/%llu/%llu structuralSignatureChanged=%u mappingSignatureChanged=%u payloadSignatureChanged=%u payloadOnlyChange=%u oldSmokeReservoirSignatureConsulted=%u smokeDoomAnalyticLightCountConsulted=%u activeDiClearSource=%u activeDiClearRequested=%u shaderRoutes=%u bindingHandoffs=%u behavior=rrx-clear-firewall\n",
            static_cast<unsigned long long>(remixRtxdiStats.frameIndex),
            remixRtxdiStats.outputWidth,
            remixRtxdiStats.outputHeight,
            remixRtxdiStats.checkerboardMode,
            useRemixRtxdiResources ? 1u : 0u,
            remixRtxdiResourcesReady ? 1u : 0u,
            remixRtxdiStats.resetReasonFlags,
            remixRtxdiStats.allowedResetReasonFlags,
            remixRtxdiStats.ignoredSmokeResetReasonFlags,
            m_frameResources.smokeReservoirNeedsClear ? 1u : 0u,
            m_frameResources.restirPTReservoirNeedsClear ? 1u : 0u,
            m_frameResources.restirPTDiReservoirNeedsClear ? 1u : 0u,
            m_frameResources.restirPTGiReservoirNeedsClear ? 1u : 0u,
            remixRtxdiStats.diRecreated,
            remixRtxdiStats.diReused,
            remixRtxdiStats.diClearPending,
            remixRtxdiStats.diClearReason,
            remixDiDomain.resetReasonFlags,
            remixRtxdiStats.giRecreated,
            remixRtxdiStats.giReused,
            remixRtxdiStats.giClearPending,
            remixRtxdiStats.giClearReason,
            remixGiDomain.resetReasonFlags,
            remixRtxdiStats.diArrayCount,
            remixRtxdiStats.diStructStride,
            remixRtxdiStats.diElementCount,
            static_cast<unsigned long long>(remixRtxdiStats.diReservoirBytes),
            remixRtxdiStats.giArrayCount,
            remixRtxdiStats.giStructStride,
            remixRtxdiStats.giElementCount,
            static_cast<unsigned long long>(remixRtxdiStats.giReservoirBytes),
            static_cast<unsigned long long>(remixRtxdiStats.lightStructuralSignature),
            static_cast<unsigned long long>(remixRtxdiStats.lightMappingSignature),
            static_cast<unsigned long long>(remixRtxdiStats.lightPayloadSignature),
            remixRtxdiStats.lightStructuralSignatureChanged,
            remixRtxdiStats.lightMappingSignatureChanged,
            remixRtxdiStats.lightPayloadSignatureChanged,
            remixRtxdiStats.lightPayloadOnlyChange,
            remixRtxdiStats.oldSmokeReservoirSignatureConsulted,
            remixRtxdiStats.smokeDoomAnalyticLightCountConsulted,
            static_cast<uint32_t>(remixDiClearSource),
            (remixDiClearSource != PATH_TRACE_REMIX_RTXDI_DI_CLEAR_SOURCE_NONE && remixDiDomain.clearPending) ? 1u : 0u,
            remixRtxdiStats.shaderRouteCount,
            remixRtxdiStats.bindingHandoffCount);
        r_pathTracingRemixRtxdiResourcesDump.SetInteger(0);
    }
    // Legacy ReSTIR light-manager ownership is purged. The buffers named
    // PathTraceRestirLightManager* remain as a shader ABI bridge, but their
    // contents are built from the Remix Light Universe records below.
    m_restirLightManager.Clear();
    const bool dumpRestirLightManager = r_pathTracingRestirLightManagerDump.GetInteger() != 0;
    const bool useRemixLightManagerRabSource = true;
    const PathTraceRemixLightManagerStats& remixLightManagerActiveStats = m_remixLightManager.GetStats();
    const std::vector<PathTraceRestirCurrentLightRecord> remixRestirCurrentRecords = [&]() {
        OPTICK_EVENT("PT Restir Current Records");
        return BuildRestirRecordsFromRemixCurrentLights(m_remixLightManager.GetCurrentLightPayloads(), m_remixLightManager.GetCurrentToPreviousMap());
    }();
    const std::vector<PathTraceRestirPreviousLightRecord> remixRestirPreviousRecords = [&]() {
        OPTICK_EVENT("PT Restir Previous Records");
        return BuildRestirRecordsFromRemixPreviousLights(m_remixLightManager.GetPreviousLightPayloads(), m_remixLightManager.GetPreviousToCurrentMap());
    }();
    const std::vector<PathTraceRestirCurrentLightRecord>& restirLightManagerCurrentRecords =
        useRemixLightManagerRabSource ? remixRestirCurrentRecords : m_restirLightManager.GetActiveCurrentLightRecords();
    const std::vector<PathTraceRestirPreviousLightRecord>& restirLightManagerPreviousRecords =
        useRemixLightManagerRabSource ? remixRestirPreviousRecords : m_restirLightManager.GetActivePreviousLightRecords();
    const std::vector<uint32_t>& restirLightManagerCurrentToPreviousRemap =
        useRemixLightManagerRabSource ? m_remixLightManager.GetCurrentToPreviousMap() : m_restirLightManager.GetActiveCurrentToPreviousRemap();
    const std::vector<uint32_t>& restirLightManagerPreviousToCurrentRemap =
        useRemixLightManagerRabSource ? m_remixLightManager.GetPreviousToCurrentMap() : m_restirLightManager.GetActivePreviousToCurrentRemap();
    const std::vector<PathTraceUnifiedLightRecord>& restirLightManagerCurrentPayloadRecords =
        useRemixLightManagerRabSource ? m_remixLightManager.GetCurrentLightPayloads() : m_restirLightManager.GetActiveCurrentPayloadRecords();
    const std::vector<PathTraceUnifiedLightRecord>& restirLightManagerPreviousPayloadRecords =
        useRemixLightManagerRabSource ? m_remixLightManager.GetPreviousLightPayloads() : m_restirLightManager.GetActivePreviousPayloadRecords();
    if (remixLightUniverseDump != 0 && remixLightManagerActiveStats.enabled != 0u)
    {
        auto recordFinite = [](const PathTraceUnifiedLightRecord& record) {
            return
                std::isfinite(record.positionAndRadius[0]) &&
                std::isfinite(record.positionAndRadius[1]) &&
                std::isfinite(record.positionAndRadius[2]) &&
                std::isfinite(record.positionAndRadius[3]) &&
                std::isfinite(record.normalAndArea[0]) &&
                std::isfinite(record.normalAndArea[1]) &&
                std::isfinite(record.normalAndArea[2]) &&
                std::isfinite(record.normalAndArea[3]) &&
                std::isfinite(record.radianceAndLuminance[0]) &&
                std::isfinite(record.radianceAndLuminance[1]) &&
                std::isfinite(record.radianceAndLuminance[2]) &&
                std::isfinite(record.radianceAndLuminance[3]) &&
                std::isfinite(record.uvOrDoomParams[0]) &&
                std::isfinite(record.uvOrDoomParams[1]) &&
                std::isfinite(record.uvOrDoomParams[2]) &&
                std::isfinite(record.uvOrDoomParams[3]) &&
                std::isfinite(record.sourcePdf) &&
                std::isfinite(record.sourceWeight);
        };
        auto remixTypeIndex = [](uint32_t type) {
            if (type == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
            {
                return static_cast<uint32_t>(PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE);
            }
            if (type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
            {
                return static_cast<uint32_t>(PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC);
            }
            return static_cast<uint32_t>(PATH_TRACE_REMIX_LIGHT_TYPE_COUNT);
        };
        auto sourceCountForRecord = [&](
            const PathTraceUnifiedLightRecord& record,
            bool previousFrame) {
            if (record.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
            {
                return previousFrame
                    ? static_cast<uint32_t>(previousEmissiveTriangles.size())
                    : static_cast<uint32_t>(emissiveTriangles.size());
            }
            if (record.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
            {
                return previousFrame
                    ? static_cast<uint32_t>(doomAnalyticRemap.previousCandidates.size())
                    : static_cast<uint32_t>(doomAnalyticLights.size());
            }
            return 0u;
        };
        auto emissiveTriangleGeometryReplayable = [&](const PathTraceSmokeEmissiveTriangle& triangle) {
            if (triangle.materialIndex >= static_cast<uint32_t>(materialTable.materials.size()) ||
                triangle.centerAndArea[3] <= 1.0e-6f ||
                triangle.sampleWeightAndPdf[0] <= 0.0f ||
                triangle.estimatedRadianceAndLuminance[3] <= 0.0f)
            {
                return false;
            }

            if (triangle.instanceId == 0u)
            {
                return triangle.primitiveIndex < static_cast<uint32_t>(staticIndexCache.size() / 3);
            }
            if (triangle.instanceId == 1u)
            {
                return triangle.primitiveIndex < static_cast<uint32_t>(dynamicIndexData.size() / 3);
            }

            const uint32_t routeInstanceIndex = triangle.instanceId - 2u;
            return routeInstanceIndex < static_cast<uint32_t>(Max(0, rigidRouteBuild.stats.emittedInstances)) &&
                triangle.primitiveIndex < static_cast<uint32_t>(Max(0, rigidRouteBuild.stats.triangles));
        };
        auto recordReplayable = [&](const PathTraceUnifiedLightRecord& record, bool previousFrame) {
            if (!recordFinite(record) ||
                record.sourceIndex >= sourceCountForRecord(record, previousFrame) ||
                record.sourceWeight <= 0.0f)
            {
                return false;
            }
            if (record.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_EMISSIVE_TRIANGLE)
            {
                const PathTraceSmokeEmissiveTriangle& emissiveTriangle = previousFrame
                    ? previousEmissiveTriangles[record.sourceIndex]
                    : emissiveTriangles[record.sourceIndex];
                return record.normalAndArea[3] > 1.0e-6f &&
                    record.radianceAndLuminance[3] > 0.0f &&
                    emissiveTriangleGeometryReplayable(emissiveTriangle);
            }
            if (record.type == PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
            {
                return record.positionAndRadius[3] > 0.0f &&
                    record.uvOrDoomParams[0] > 0.0f &&
                    record.normalAndArea[3] > 1.0e-6f &&
                    record.radianceAndLuminance[3] > 0.0f;
            }
            return false;
        };
        auto printReplaySample = [&](const char* label, uint32_t index, bool previousFrame) {
            const std::vector<PathTraceUnifiedLightRecord>& records = previousFrame
                ? restirLightManagerPreviousPayloadRecords
                : restirLightManagerCurrentPayloadRecords;
            if (index >= records.size())
            {
                return;
            }
            const PathTraceUnifiedLightRecord& record = records[index];
            const uint32_t mappedIndex = previousFrame
                ? (index < restirLightManagerPreviousToCurrentRemap.size() ? restirLightManagerPreviousToCurrentRemap[index] : PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX)
                : (index < restirLightManagerCurrentToPreviousRemap.size() ? restirLightManagerCurrentToPreviousRemap[index] : PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX);
            common->Printf(
                "PathTracePrimaryPass: RLU CPU payload replay audit %s frame=%s index=%u mappedIndex=%u replayable=%u type=%u sourceIndex=%u radius=%.6g influenceRadius=%.6g area=%.6g luminance=%.6g sourceWeight=%.6g flags=0x%x identity=%u:%u materialOrLightId=%u previousIndex=%u behavior=rlu-09-pre-upload-payload-replay-audit\n",
                label,
                previousFrame ? "previous" : "current",
                index,
                mappedIndex,
                recordReplayable(record, previousFrame) ? 1u : 0u,
                record.type,
                record.sourceIndex,
                record.positionAndRadius[3],
                record.uvOrDoomParams[0],
                record.normalAndArea[3],
                record.radianceAndLuminance[3],
                record.sourceWeight,
                record.flags,
                record.identityA,
                record.identityB,
                record.materialOrLightId,
                record.previousIndex);
        };

        uint32_t currentReplayableByType[PATH_TRACE_REMIX_LIGHT_TYPE_COUNT] = {};
        uint32_t currentInvalidByType[PATH_TRACE_REMIX_LIGHT_TYPE_COUNT] = {};
        uint32_t previousReplayableByType[PATH_TRACE_REMIX_LIGHT_TYPE_COUNT] = {};
        uint32_t previousInvalidByType[PATH_TRACE_REMIX_LIGHT_TYPE_COUNT] = {};
        uint32_t mappedPairsByType[PATH_TRACE_REMIX_LIGHT_TYPE_COUNT] = {};
        uint32_t mappedReplayFailuresByType[PATH_TRACE_REMIX_LIGHT_TYPE_COUNT] = {};
        uint32_t firstInvalidCurrent[PATH_TRACE_REMIX_LIGHT_TYPE_COUNT] = {
            PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX,
            PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX
        };
        uint32_t firstInvalidPrevious[PATH_TRACE_REMIX_LIGHT_TYPE_COUNT] = {
            PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX,
            PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX
        };
        uint32_t firstMappedFailureCurrent[PATH_TRACE_REMIX_LIGHT_TYPE_COUNT] = {
            PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX,
            PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX
        };
        uint32_t firstMappedFailurePrevious[PATH_TRACE_REMIX_LIGHT_TYPE_COUNT] = {
            PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX,
            PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX
        };

        for (uint32_t currentIndex = 0u; currentIndex < restirLightManagerCurrentPayloadRecords.size(); ++currentIndex)
        {
            const PathTraceUnifiedLightRecord& record = restirLightManagerCurrentPayloadRecords[currentIndex];
            const uint32_t typeIndex = remixTypeIndex(record.type);
            if (typeIndex >= PATH_TRACE_REMIX_LIGHT_TYPE_COUNT)
            {
                continue;
            }
            if (recordReplayable(record, false))
            {
                ++currentReplayableByType[typeIndex];
            }
            else
            {
                ++currentInvalidByType[typeIndex];
                if (firstInvalidCurrent[typeIndex] == PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX)
                {
                    firstInvalidCurrent[typeIndex] = currentIndex;
                }
            }

            const uint32_t previousIndex = currentIndex < restirLightManagerCurrentToPreviousRemap.size()
                ? restirLightManagerCurrentToPreviousRemap[currentIndex]
                : PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX;
            if (previousIndex >= restirLightManagerPreviousPayloadRecords.size())
            {
                continue;
            }
            const PathTraceUnifiedLightRecord& previous = restirLightManagerPreviousPayloadRecords[previousIndex];
            ++mappedPairsByType[typeIndex];
            const bool roundTripValid =
                previousIndex < restirLightManagerPreviousToCurrentRemap.size() &&
                restirLightManagerPreviousToCurrentRemap[previousIndex] == currentIndex;
            if (previous.type != record.type ||
                !roundTripValid ||
                !recordReplayable(previous, true))
            {
                ++mappedReplayFailuresByType[typeIndex];
                if (firstMappedFailureCurrent[typeIndex] == PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX)
                {
                    firstMappedFailureCurrent[typeIndex] = currentIndex;
                    firstMappedFailurePrevious[typeIndex] = previousIndex;
                }
            }
        }

        for (uint32_t previousIndex = 0u; previousIndex < restirLightManagerPreviousPayloadRecords.size(); ++previousIndex)
        {
            const PathTraceUnifiedLightRecord& record = restirLightManagerPreviousPayloadRecords[previousIndex];
            const uint32_t typeIndex = remixTypeIndex(record.type);
            if (typeIndex >= PATH_TRACE_REMIX_LIGHT_TYPE_COUNT)
            {
                continue;
            }
            if (recordReplayable(record, true))
            {
                ++previousReplayableByType[typeIndex];
            }
            else
            {
                ++previousInvalidByType[typeIndex];
                if (firstInvalidPrevious[typeIndex] == PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX)
                {
                    firstInvalidPrevious[typeIndex] = previousIndex;
                }
            }
        }

        common->Printf(
            "PathTracePrimaryPass: RLU CPU payload replay audit current replayable/invalid emissive=%u/%u doom=%u/%u previous replayable/invalid emissive=%u/%u doom=%u/%u mappedPairs emissive/doom=%u/%u mappedReplayFailures emissive/doom=%u/%u currentPayloads=%u previousPayloads=%u behavior=rlu-09-pre-upload-payload-replay-audit\n",
            currentReplayableByType[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE],
            currentInvalidByType[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE],
            currentReplayableByType[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC],
            currentInvalidByType[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC],
            previousReplayableByType[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE],
            previousInvalidByType[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE],
            previousReplayableByType[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC],
            previousInvalidByType[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC],
            mappedPairsByType[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE],
            mappedPairsByType[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC],
            mappedReplayFailuresByType[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE],
            mappedReplayFailuresByType[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC],
            static_cast<uint32_t>(restirLightManagerCurrentPayloadRecords.size()),
            static_cast<uint32_t>(restirLightManagerPreviousPayloadRecords.size()));
        if (firstInvalidCurrent[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE] != PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX)
        {
            printReplaySample("firstInvalidCurrentEmissive", firstInvalidCurrent[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE], false);
        }
        if (firstInvalidCurrent[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC] != PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX)
        {
            printReplaySample("firstInvalidCurrentDoomAnalytic", firstInvalidCurrent[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC], false);
        }
        if (firstInvalidPrevious[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE] != PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX)
        {
            printReplaySample("firstInvalidPreviousEmissive", firstInvalidPrevious[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE], true);
        }
        if (firstInvalidPrevious[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC] != PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX)
        {
            printReplaySample("firstInvalidPreviousDoomAnalytic", firstInvalidPrevious[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC], true);
        }
        if (firstMappedFailureCurrent[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE] != PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX)
        {
            printReplaySample("firstMappedFailureCurrentEmissive", firstMappedFailureCurrent[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE], false);
            printReplaySample("firstMappedFailurePreviousEmissive", firstMappedFailurePrevious[PATH_TRACE_REMIX_LIGHT_TYPE_EMISSIVE_TRIANGLE], true);
        }
        if (firstMappedFailureCurrent[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC] != PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX)
        {
            printReplaySample("firstMappedFailureCurrentDoomAnalytic", firstMappedFailureCurrent[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC], false);
            printReplaySample("firstMappedFailurePreviousDoomAnalytic", firstMappedFailurePrevious[PATH_TRACE_REMIX_LIGHT_TYPE_DOOM_ANALYTIC], true);
        }
    }
    if (remixLightUniverseDump != 0 && remixLightManagerActiveStats.enabled != 0u && remixLightManagerActiveStats.domain == 2u)
    {
        auto recordFinite = [](const PathTraceUnifiedLightRecord& record) {
            return
                std::isfinite(record.positionAndRadius[0]) &&
                std::isfinite(record.positionAndRadius[1]) &&
                std::isfinite(record.positionAndRadius[2]) &&
                std::isfinite(record.positionAndRadius[3]) &&
                std::isfinite(record.uvOrDoomParams[0]) &&
                std::isfinite(record.normalAndArea[3]) &&
                std::isfinite(record.radianceAndLuminance[3]) &&
                std::isfinite(record.sourceWeight);
        };
        auto printMappedDoomPair = [&](const char* label, uint32_t currentIndex, uint32_t previousIndex, uint32_t previousToCurrentIndex) {
            const PathTraceUnifiedLightRecord& current = restirLightManagerCurrentPayloadRecords[currentIndex];
            const PathTraceUnifiedLightRecord& previous = restirLightManagerPreviousPayloadRecords[previousIndex];
            common->Printf(
                "PathTracePrimaryPass: RLU domain2 CPU mapped Doom analytic payload %s currentIndex=%u previousIndex=%u previousToCurrent=%u type current/previous=%u/%u sourceIndex current/previous=%u/%u radius current/previous=%.6g/%.6g influence current/previous=%.6g/%.6g area current/previous=%.6g/%.6g luminance current/previous=%.6g/%.6g sourceWeight current/previous=%.6g/%.6g flags current/previous=0x%x/0x%x identity current/previous=%u:%u/%u:%u behavior=pre-upload-rlu-domain2-payload-audit\n",
                label,
                currentIndex,
                previousIndex,
                previousToCurrentIndex,
                current.type,
                previous.type,
                current.sourceIndex,
                previous.sourceIndex,
                current.positionAndRadius[3],
                previous.positionAndRadius[3],
                current.uvOrDoomParams[0],
                previous.uvOrDoomParams[0],
                current.normalAndArea[3],
                previous.normalAndArea[3],
                current.radianceAndLuminance[3],
                previous.radianceAndLuminance[3],
                current.sourceWeight,
                previous.sourceWeight,
                current.flags,
                previous.flags,
                current.identityA,
                current.identityB,
                previous.identityA,
                previous.identityB);
        };
        auto duplicateIdentityCountForKey = [](const std::vector<PathTraceUnifiedLightRecord>& records, const PathTraceUnifiedLightRecord& key) {
            uint32_t duplicateCount = 0u;
            for (const PathTraceUnifiedLightRecord& record : records)
            {
                if (record.type == key.type &&
                    record.identityA == key.identityA &&
                    record.identityB == key.identityB &&
                    record.materialOrLightId == key.materialOrLightId)
                {
                    ++duplicateCount;
                }
            }
            return duplicateCount;
        };
        auto sourcePayloadIdentityMatches = [](const PathTraceUnifiedLightRecord& record, const PathTraceDoomAnalyticLightCandidate& payload) {
            return record.materialOrLightId == payload.renderLightIndex &&
                record.identityA == payload.renderLightIndex &&
                record.identityB == payload.entityNumber;
        };
        auto printPreviousReplayFailure = [&](const char* reason, uint32_t currentIndex, uint32_t previousIndex, uint32_t previousToCurrentIndex) {
            const PathTraceUnifiedLightRecord& current = restirLightManagerCurrentPayloadRecords[currentIndex];
            const bool previousIndexValid = previousIndex < restirLightManagerPreviousPayloadRecords.size();
            const PathTraceUnifiedLightRecord previous = previousIndexValid
                ? restirLightManagerPreviousPayloadRecords[previousIndex]
                : PathTraceUnifiedLightRecord();
            const bool currentSourceValid = current.sourceIndex < doomAnalyticLights.size();
            const bool previousSourceValid = previousIndexValid && previous.sourceIndex < doomAnalyticRemap.previousCandidates.size();
            const uint32_t currentSourceRenderLightIndex = currentSourceValid
                ? doomAnalyticLights[current.sourceIndex].renderLightIndex
                : PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX;
            const uint32_t currentSourceEntityNumber = currentSourceValid
                ? doomAnalyticLights[current.sourceIndex].entityNumber
                : PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX;
            const uint32_t previousSourceRenderLightIndex = previousSourceValid
                ? doomAnalyticRemap.previousCandidates[previous.sourceIndex].renderLightIndex
                : PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX;
            const uint32_t previousSourceEntityNumber = previousSourceValid
                ? doomAnalyticRemap.previousCandidates[previous.sourceIndex].entityNumber
                : PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX;
            const uint32_t currentDuplicateIdentityCount = duplicateIdentityCountForKey(restirLightManagerCurrentPayloadRecords, current);
            const uint32_t previousDuplicateIdentityCount = previousIndexValid
                ? duplicateIdentityCountForKey(restirLightManagerPreviousPayloadRecords, previous)
                : 0u;
            const uint32_t duplicateIdentity = (currentDuplicateIdentityCount > 1u || previousDuplicateIdentityCount > 1u) ? 1u : 0u;
            common->Printf(
                "PathTracePrimaryPass: RLU domain2 Doom analytic previous replay failure reason=%s currentDenseIndex=%u previousDenseIndex=%u type current/previous=%u/%u sourceIndex current/previous=%u/%u identityA current/previous=%u/%u identityB current/previous=%u/%u materialOrLightId current/previous=%u/%u sourcePayload renderLightIndex current/previous=%u/%u entityNumber current/previous=%u/%u duplicateIdentity=%u duplicateIdentityCount current/previous=%u/%u currentToPrevious=%u previousToCurrent=%u behavior=pre-upload-rlu-domain2-previous-replay-audit\n",
                reason,
                currentIndex,
                previousIndex,
                current.type,
                previousIndexValid ? previous.type : PATH_TRACE_UNIFIED_LIGHT_TYPE_INVALID,
                current.sourceIndex,
                previousIndexValid ? previous.sourceIndex : PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX,
                current.identityA,
                previousIndexValid ? previous.identityA : PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX,
                current.identityB,
                previousIndexValid ? previous.identityB : PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX,
                current.materialOrLightId,
                previousIndexValid ? previous.materialOrLightId : PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX,
                currentSourceRenderLightIndex,
                previousSourceRenderLightIndex,
                currentSourceEntityNumber,
                previousSourceEntityNumber,
                duplicateIdentity,
                currentDuplicateIdentityCount,
                previousDuplicateIdentityCount,
                previousIndex,
                previousToCurrentIndex);
        };

        uint32_t mappedDoomAnalyticPairs = 0u;
        uint32_t previousDoomBadRadius = 0u;
        uint32_t previousDoomBadSourceWeight = 0u;
        uint32_t previousDoomNonfinite = 0u;
        uint32_t typeMismatches = 0u;
        uint32_t previousReplayFailures = 0u;
        uint32_t firstCurrentIndex = PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX;
        uint32_t firstPreviousIndex = PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX;
        uint32_t firstPreviousToCurrentIndex = PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX;
        uint32_t firstFailCurrentIndex = PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX;
        uint32_t firstFailPreviousIndex = PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX;
        uint32_t firstFailPreviousToCurrentIndex = PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX;

        const uint32_t currentToPreviousCount = static_cast<uint32_t>(restirLightManagerCurrentToPreviousRemap.size());
        for (uint32_t currentIndex = 0u; currentIndex < currentToPreviousCount && currentIndex < restirLightManagerCurrentPayloadRecords.size(); ++currentIndex)
        {
            const PathTraceUnifiedLightRecord& current = restirLightManagerCurrentPayloadRecords[currentIndex];
            if (current.type != PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
            {
                continue;
            }

            const uint32_t previousIndex = restirLightManagerCurrentToPreviousRemap[currentIndex];
            if (previousIndex >= restirLightManagerPreviousPayloadRecords.size())
            {
                ++previousReplayFailures;
                printPreviousReplayFailure("currentToPrevious-invalid", currentIndex, previousIndex, PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX);
                continue;
            }

            const uint32_t previousToCurrentIndex = previousIndex < restirLightManagerPreviousToCurrentRemap.size()
                ? restirLightManagerPreviousToCurrentRemap[previousIndex]
                : PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX;
            const PathTraceUnifiedLightRecord& previous = restirLightManagerPreviousPayloadRecords[previousIndex];
            ++mappedDoomAnalyticPairs;
            if (firstCurrentIndex == PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX)
            {
                firstCurrentIndex = currentIndex;
                firstPreviousIndex = previousIndex;
                firstPreviousToCurrentIndex = previousToCurrentIndex;
            }

            bool pairFailed = false;
            if (previous.type != PATH_TRACE_UNIFIED_LIGHT_TYPE_DOOM_ANALYTIC)
            {
                ++typeMismatches;
                pairFailed = true;
            }
            else
            {
                const bool finitePrevious = recordFinite(previous);
                if (!finitePrevious)
                {
                    ++previousDoomNonfinite;
                    pairFailed = true;
                }
                if (!finitePrevious || previous.positionAndRadius[3] <= 0.0f)
                {
                    ++previousDoomBadRadius;
                    pairFailed = true;
                }
                if (!finitePrevious || previous.sourceWeight <= 0.0f)
                {
                    ++previousDoomBadSourceWeight;
                    pairFailed = true;
                }
            }
            const bool currentSourceValid = current.sourceIndex < doomAnalyticLights.size();
            const bool previousSourceValid = previous.sourceIndex < doomAnalyticRemap.previousCandidates.size();
            const bool roundTripValid = previousToCurrentIndex == currentIndex;
            const bool duplicateIdentity =
                duplicateIdentityCountForKey(restirLightManagerCurrentPayloadRecords, current) > 1u ||
                duplicateIdentityCountForKey(restirLightManagerPreviousPayloadRecords, previous) > 1u;
            const bool currentSourceIdentityValid = currentSourceValid && sourcePayloadIdentityMatches(current, doomAnalyticLights[current.sourceIndex]);
            const bool previousSourceIdentityValid = previousSourceValid && sourcePayloadIdentityMatches(previous, doomAnalyticRemap.previousCandidates[previous.sourceIndex]);
            const bool replayFailed =
                pairFailed ||
                !roundTripValid ||
                !currentSourceValid ||
                !previousSourceValid ||
                !currentSourceIdentityValid ||
                !previousSourceIdentityValid ||
                duplicateIdentity;
            if (replayFailed)
            {
                ++previousReplayFailures;
                const char* replayFailureReason =
                    pairFailed ? "previous-payload-invalid" :
                    (!roundTripValid ? "previousToCurrent-mismatch" :
                    (!currentSourceValid ? "current-source-invalid" :
                    (!previousSourceValid ? "previous-source-invalid" :
                    (!currentSourceIdentityValid ? "current-source-identity-mismatch" :
                    (!previousSourceIdentityValid ? "previous-source-identity-mismatch" : "duplicate-identity")))));
                printPreviousReplayFailure(replayFailureReason, currentIndex, previousIndex, previousToCurrentIndex);
            }

            if (pairFailed && firstFailCurrentIndex == PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX)
            {
                firstFailCurrentIndex = currentIndex;
                firstFailPreviousIndex = previousIndex;
                firstFailPreviousToCurrentIndex = previousToCurrentIndex;
            }
        }

        common->Printf(
            "PathTracePrimaryPass: RLU domain2 CPU mapped Doom analytic payload audit mappedPairs=%u previousBadRadius=%u previousBadSourceWeight=%u previousNonfinite=%u typeMismatches=%u previousReplayFailures=%u currentPayloads=%u previousPayloads=%u currentToPrevious=%u previousToCurrent=%u behavior=pre-upload-rlu-domain2-payload-audit\n",
            mappedDoomAnalyticPairs,
            previousDoomBadRadius,
            previousDoomBadSourceWeight,
            previousDoomNonfinite,
            typeMismatches,
            previousReplayFailures,
            static_cast<uint32_t>(restirLightManagerCurrentPayloadRecords.size()),
            static_cast<uint32_t>(restirLightManagerPreviousPayloadRecords.size()),
            static_cast<uint32_t>(restirLightManagerCurrentToPreviousRemap.size()),
            static_cast<uint32_t>(restirLightManagerPreviousToCurrentRemap.size()));
        if (firstCurrentIndex != PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX)
        {
            printMappedDoomPair("first", firstCurrentIndex, firstPreviousIndex, firstPreviousToCurrentIndex);
        }
        if (firstFailCurrentIndex != PATH_TRACE_UNIFIED_LIGHT_INVALID_INDEX)
        {
            printMappedDoomPair("firstFailing", firstFailCurrentIndex, firstFailPreviousIndex, firstFailPreviousToCurrentIndex);
        }
    }
    if (dumpRestirLightManager)
    {
        common->Printf("PathTracePrimaryPass: legacy ReSTIR light manager purged; active light domains are owned by Remix Light Universe behavior=legacy-light-universe-purged\n");
        r_pathTracingRestirLightManagerDump.SetInteger(0);
    }
    if (r_pathTracingRestirLightManagerDump.GetInteger() != 0)
    {
        const PathTraceRestirLightObservationStats restirLightManagerStats = BuildPathTraceRestirLightManagerDebugObservations(
            restirLightManagerObservations);
        const PathTraceRestirLightManagerStats restirLightManagerPersistentStats = m_restirLightManager.GetStats();
        common->Printf("PathTracePrimaryPass: ReSTIR light manager observations total=%u stableIdentity=%u unknownIdentity=%u payloadSourceValid=%u mappedReady=%u remapInvalid=%u unsupported=%u emissive total/stable/unknown=%u/%u/%u doomAnalytic total/stable/unknown=%u/%u/%u persistent current=%u previous=%u stable=%u payloadChanged=%u stableMapped=%u payloadChangedMapped=%u currentOnly=%u previousOnly=%u remap valid/invalid=%u/%u maps currentToPrevious size/mapped/invalid=%u/%u/%u previousToCurrent size/mapped/invalid=%u/%u/%u mapSizeMismatches=%u signatures structural/mapping/payload=%llu/%llu/%llu changed=%u/%u/%u topPayloadChanged current/previous/source/key/hashCurrent/hashPrevious=%u/%u/%u/%u:%u/%u:%u/%u:%u invalid new/missing/temp/projectile/duplicate/unknown/unsupported/reset/unproven/zero/suppressed/outOfArea/disconnectedOrPortal/invalidShape/candidateCap/incompatible/deleted=%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u behavior=cpu-debug-only\n",
            restirLightManagerStats.totalObservationCount,
            restirLightManagerStats.stableIdentityCount,
            restirLightManagerStats.unknownIdentityCount,
            restirLightManagerStats.payloadSourceValidCount,
            restirLightManagerStats.stableMappedReadyCount,
            restirLightManagerStats.remapInvalidObservationCount,
            restirLightManagerStats.unsupportedObservationCount,
            restirLightManagerStats.emissiveObservationCount,
            restirLightManagerStats.emissiveStableIdentityCount,
            restirLightManagerStats.emissiveUnknownIdentityCount,
            restirLightManagerStats.doomAnalyticObservationCount,
            restirLightManagerStats.doomAnalyticStableIdentityCount,
            restirLightManagerStats.doomAnalyticUnknownIdentityCount,
            restirLightManagerPersistentStats.currentLightCount,
            restirLightManagerPersistentStats.previousLightCount,
            restirLightManagerPersistentStats.stableIdentityCount,
            restirLightManagerPersistentStats.payloadChangedCount,
            restirLightManagerPersistentStats.stableMappedCount,
            restirLightManagerPersistentStats.payloadChangedMappedCount,
            restirLightManagerPersistentStats.currentOnlyCount,
            restirLightManagerPersistentStats.previousOnlyCount,
            restirLightManagerPersistentStats.remapValidCount,
            restirLightManagerPersistentStats.remapInvalidCount,
            restirLightManagerPersistentStats.currentToPreviousCount,
            restirLightManagerPersistentStats.currentMappedCount,
            restirLightManagerPersistentStats.currentInvalidCount,
            restirLightManagerPersistentStats.previousToCurrentCount,
            restirLightManagerPersistentStats.previousMappedCount,
            restirLightManagerPersistentStats.previousInvalidCount,
            restirLightManagerPersistentStats.mapSizeMismatchCount,
            static_cast<unsigned long long>(restirLightManagerPersistentStats.structuralSignature),
            static_cast<unsigned long long>(restirLightManagerPersistentStats.mappingIdentitySignature),
            static_cast<unsigned long long>(restirLightManagerPersistentStats.animatedPayloadSignature),
            restirLightManagerPersistentStats.structuralSignatureChanged,
            restirLightManagerPersistentStats.mappingIdentitySignatureChanged,
            restirLightManagerPersistentStats.animatedPayloadSignatureChanged,
            restirLightManagerPersistentStats.topPayloadChangedCurrentIndex,
            restirLightManagerPersistentStats.topPayloadChangedPreviousIndex,
            restirLightManagerPersistentStats.topPayloadChangedSourceType,
            restirLightManagerPersistentStats.topPayloadChangedIdentityKeyLo,
            restirLightManagerPersistentStats.topPayloadChangedIdentityKeyHi,
            restirLightManagerPersistentStats.topPayloadChangedCurrentHashLo,
            restirLightManagerPersistentStats.topPayloadChangedCurrentHashHi,
            restirLightManagerPersistentStats.topPayloadChangedPreviousHashLo,
            restirLightManagerPersistentStats.topPayloadChangedPreviousHashHi,
            restirLightManagerPersistentStats.invalidReasons.newLight,
            restirLightManagerPersistentStats.invalidReasons.missingLight,
            restirLightManagerPersistentStats.invalidReasons.temporary,
            restirLightManagerPersistentStats.invalidReasons.projectile,
            restirLightManagerPersistentStats.invalidReasons.duplicate,
            restirLightManagerPersistentStats.invalidReasons.unknownIdentity,
            restirLightManagerPersistentStats.invalidReasons.unsupportedSource,
            restirLightManagerPersistentStats.invalidReasons.structuralReset,
            restirLightManagerPersistentStats.invalidReasons.unprovenContinuity,
            restirLightManagerPersistentStats.invalidReasons.zeroRadiance,
            restirLightManagerPersistentStats.invalidReasons.suppressed,
            restirLightManagerPersistentStats.invalidReasons.outOfSelectedArea,
            restirLightManagerPersistentStats.invalidReasons.disconnectedOrPortal,
            restirLightManagerPersistentStats.invalidReasons.invalidShape,
            restirLightManagerPersistentStats.invalidReasons.candidateCap,
            restirLightManagerPersistentStats.invalidReasons.incompatibleSource,
            restirLightManagerPersistentStats.invalidReasons.deleted);
        common->Printf("PathTracePrimaryPass: ReSTIR light manager active current/previous=%u/%u payload current/previous=%u/%u map currentToPrevious/previousToCurrent=%u/%u payloadMapTotal=%u mismatch=%u inactive current/previous=%u/%u inactive zero/suppressed=%u/%u ranges emissive offset/count=%u/%u doomAnalytic offset/count=%u/%u sampleContract emissive/doom/total/nonEmptyRanges=%u/%u/%u/%u behavior=payload-upload-staged\n",
            restirLightManagerPersistentStats.activeCurrentLightCount,
            restirLightManagerPersistentStats.activePreviousLightCount,
            restirLightManagerPersistentStats.activeCurrentPayloadCount,
            restirLightManagerPersistentStats.activePreviousPayloadCount,
            restirLightManagerPersistentStats.activeCurrentToPreviousCount,
            restirLightManagerPersistentStats.activePreviousToCurrentCount,
            restirLightManagerPersistentStats.activePayloadMapCount,
            restirLightManagerPersistentStats.activePayloadCountMismatch,
            restirLightManagerPersistentStats.inactiveCurrentLightCount,
            restirLightManagerPersistentStats.inactivePreviousLightCount,
            restirLightManagerPersistentStats.inactiveZeroRadianceCount,
            restirLightManagerPersistentStats.inactiveSuppressedCount,
            restirLightManagerPersistentStats.activeEmissiveCurrentRangeOffset,
            restirLightManagerPersistentStats.activeEmissiveCurrentRangeCount,
            restirLightManagerPersistentStats.activeDoomAnalyticCurrentRangeOffset,
            restirLightManagerPersistentStats.activeDoomAnalyticCurrentRangeCount,
            restirLightManagerPersistentStats.activeEmissiveCurrentRangeCount,
            restirLightManagerPersistentStats.activeDoomAnalyticCurrentRangeCount,
            restirLightManagerPersistentStats.activeEmissiveCurrentRangeCount + restirLightManagerPersistentStats.activeDoomAnalyticCurrentRangeCount,
            (restirLightManagerPersistentStats.activeEmissiveCurrentRangeCount > 0 ? 1u : 0u) +
                (restirLightManagerPersistentStats.activeDoomAnalyticCurrentRangeCount > 0 ? 1u : 0u));
        common->Printf("PathTracePrimaryPass: ReSTIR light manager active RAB source activeLightSource=%s current=%u previous=%u currentToPrevious mapped/invalid=%u/%u previousToCurrent mapped/invalid=%u/%u payloadOnlyChange=%u structuralSignatureChanged=%u mappingSignatureChanged=%u payloadSignatureChanged=%u behavior=active-rab-source\n",
            useRemixLightManagerRabSource ? "remix-light-manager" : "legacy-restir-manager",
            useRemixLightManagerRabSource ? remixLightManagerActiveStats.currentLightCount : restirLightManagerPersistentStats.activeCurrentLightCount,
            useRemixLightManagerRabSource ? remixLightManagerActiveStats.previousLightCount : restirLightManagerPersistentStats.activePreviousLightCount,
            useRemixLightManagerRabSource ? remixLightManagerActiveStats.currentMappedCount : restirLightManagerPersistentStats.currentMappedCount,
            useRemixLightManagerRabSource ? remixLightManagerActiveStats.currentInvalidCount : restirLightManagerPersistentStats.currentInvalidCount,
            useRemixLightManagerRabSource ? remixLightManagerActiveStats.previousMappedCount : restirLightManagerPersistentStats.previousMappedCount,
            useRemixLightManagerRabSource ? remixLightManagerActiveStats.previousInvalidCount : restirLightManagerPersistentStats.previousInvalidCount,
            useRemixLightManagerRabSource ? remixLightManagerActiveStats.payloadOnlyChange : 0u,
            useRemixLightManagerRabSource ? remixLightManagerActiveStats.structuralSignatureChanged : restirLightManagerPersistentStats.structuralSignatureChanged,
            useRemixLightManagerRabSource ? remixLightManagerActiveStats.mappingSignatureChanged : restirLightManagerPersistentStats.mappingIdentitySignatureChanged,
            useRemixLightManagerRabSource ? remixLightManagerActiveStats.payloadSignatureChanged : restirLightManagerPersistentStats.animatedPayloadSignatureChanged);
    }
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
    const RtSmokeLightUniverseStats lightUniverseStats = m_smokeLightUniverse.GetStats();
    if (r_pathTracingLightUniverseDump.GetInteger() != 0)
    {
        common->Printf("PathTracePrimaryPass: legacy RT smoke light universe purged; raw emissive extraction feeds Remix Light Universe directly behavior=legacy-light-universe-purged\n");
        r_pathTracingLightUniverseDump.SetInteger(0);
    }
    if (r_pathTracingLightUniverseDump.GetInteger() != 0)
    {
        common->Printf("PathTracePrimaryPass: RT smoke light universe static=%d seen=%d new=%d updated=%d missing=%d semiStatic=%d dynSeen=%d dynPromoted=%d dynUpdated=%d dynMissing=%d dynAged=%d dynamicFrame=%d merged=%d area current/total=%d/%d selected steps/areas/edges/blocked=%d/%d/%d/%d staticKnown/unknown=%d/%d dynamicKnown/unknown=%d/%d mergedKnown/unknown=%d/%d mergedCurrent/selected/connected/disconnected=%d/%d/%d/%d connectedUnselected=%d portalSweep areas=%d/%d/%d/%d/%d merged=%d/%d/%d/%d/%d portalDepthBins depth0/1/2/3/4/>4/disconnected/unknown=%d/%d/%d/%d/%d/%d/%d/%d areaFilter enabled/applied=%d/%d steps=%d overflowMax=%d selected=%d connectedOverflow=%d disconnected=%d unknown=%d wouldUpload=%d wouldDrop=%d area %.2f/%.2f drop=%.2f weight %.3f/%.3f drop=%.3f dropWeight overflow/disconnected/unknown=%.3f/%.3f/%.3f persistDynamic=%d injectMissingDynamic=%d minSeen=%d maxMissing=%d generation=%llu\n",
            lightUniverseStats.persistentStaticTriangles,
            lightUniverseStats.staticSeenThisFrame,
            lightUniverseStats.staticNewThisFrame,
            lightUniverseStats.staticUpdatedThisFrame,
            lightUniverseStats.staticMissingThisFrame,
            lightUniverseStats.persistentDynamicTriangles,
            lightUniverseStats.dynamicSeenThisFrame,
            lightUniverseStats.dynamicPromotedThisFrame,
            lightUniverseStats.dynamicUpdatedThisFrame,
            lightUniverseStats.dynamicMissingThisFrame,
            lightUniverseStats.dynamicAgedOutThisFrame,
            lightUniverseStats.dynamicFrameTriangles,
            lightUniverseStats.mergedTriangles,
            lightUniverseStats.currentArea,
            lightUniverseStats.totalAreas,
            lightUniverseStats.selectedPortalSteps,
            lightUniverseStats.selectedAreaCount,
            lightUniverseStats.selectedPortalEdges,
            lightUniverseStats.selectedBlockedPortalEdges,
            lightUniverseStats.staticAreaKnownTriangles,
            lightUniverseStats.staticAreaUnknownTriangles,
            lightUniverseStats.dynamicAreaKnownTriangles,
            lightUniverseStats.dynamicAreaUnknownTriangles,
            lightUniverseStats.mergedAreaKnownTriangles,
            lightUniverseStats.mergedAreaUnknownTriangles,
            lightUniverseStats.mergedCurrentAreaTriangles,
            lightUniverseStats.mergedSelectedAreaTriangles,
            lightUniverseStats.mergedConnectedAreaTriangles,
            lightUniverseStats.mergedDisconnectedAreaTriangles,
            lightUniverseStats.mergedConnectedUnselectedAreaTriangles,
            lightUniverseStats.portalStepSelectedAreas[0],
            lightUniverseStats.portalStepSelectedAreas[1],
            lightUniverseStats.portalStepSelectedAreas[2],
            lightUniverseStats.portalStepSelectedAreas[3],
            lightUniverseStats.portalStepSelectedAreas[4],
            lightUniverseStats.portalStepMergedSelectedTriangles[0],
            lightUniverseStats.portalStepMergedSelectedTriangles[1],
            lightUniverseStats.portalStepMergedSelectedTriangles[2],
            lightUniverseStats.portalStepMergedSelectedTriangles[3],
            lightUniverseStats.portalStepMergedSelectedTriangles[4],
            lightUniverseStats.mergedPortalDepthBins[0],
            lightUniverseStats.mergedPortalDepthBins[1],
            lightUniverseStats.mergedPortalDepthBins[2],
            lightUniverseStats.mergedPortalDepthBins[3],
            lightUniverseStats.mergedPortalDepthBins[4],
            lightUniverseStats.mergedPortalDepthBins[5],
            lightUniverseStats.mergedPortalDepthBins[6],
            lightUniverseStats.mergedPortalDepthBins[7],
            lightUniverseStats.areaFilterEnabled,
            lightUniverseStats.areaFilterApplied,
            lightUniverseStats.areaFilterPortalSteps,
            lightUniverseStats.areaFilterOverflowMax,
            lightUniverseStats.areaFilterSelectedCandidates,
            lightUniverseStats.areaFilterConnectedOverflowCandidates,
            lightUniverseStats.areaFilterDisconnectedCandidates,
            lightUniverseStats.areaFilterUnknownCandidates,
            lightUniverseStats.areaFilterWouldUploadCandidates,
            lightUniverseStats.areaFilterWouldDropCandidates,
            lightUniverseStats.areaFilterPreArea,
            lightUniverseStats.areaFilterPostArea,
            lightUniverseStats.areaFilterDroppedArea,
            lightUniverseStats.areaFilterPreWeight,
            lightUniverseStats.areaFilterPostWeight,
            lightUniverseStats.areaFilterDroppedWeight,
            lightUniverseStats.areaFilterDroppedOverflowWeight,
            lightUniverseStats.areaFilterDroppedDisconnectedWeight,
            lightUniverseStats.areaFilterDroppedUnknownWeight,
            r_pathTracingLightUniversePersistDynamic.GetInteger() != 0 ? 1 : 0,
            r_pathTracingLightUniverseInjectMissingDynamic.GetInteger() != 0 ? 1 : 0,
            idMath::ClampInt(1, 120, r_pathTracingLightUniverseDynamicMinSeenFrames.GetInteger()),
            idMath::ClampInt(1, 3600, r_pathTracingLightUniverseDynamicMaxMissingFrames.GetInteger()),
            static_cast<unsigned long long>(lightUniverseStats.generation));
        if (lightUniverseStats.overflowSampleCount > 0)
        {
            for (int sampleIndex = 0; sampleIndex < lightUniverseStats.overflowSampleCount; ++sampleIndex)
            {
                const RtSmokeLightUniverseCandidateSample& sample = lightUniverseStats.overflowSamples[sampleIndex];
                if (!sample.valid)
                {
                    continue;
                }
                common->Printf("PathTracePrimaryPass: RT smoke light area overflow sample %d area=%d material=%u materialIndex=%u triArea=%.2f weight=%.3f distance=%.2f\n",
                    sampleIndex,
                    sample.areaNum,
                    sample.materialId,
                    sample.materialIndex,
                    sample.area,
                    sample.weight,
                    sample.distance);
            }
        }
        if (lightUniverseStats.droppedSampleCount > 0)
        {
            for (int sampleIndex = 0; sampleIndex < lightUniverseStats.droppedSampleCount; ++sampleIndex)
            {
                const RtSmokeLightUniverseCandidateSample& sample = lightUniverseStats.droppedSamples[sampleIndex];
                if (!sample.valid)
                {
                    continue;
                }
                common->Printf("PathTracePrimaryPass: RT smoke light area dropped sample %d reason=%s area=%d material=%u materialIndex=%u triArea=%.2f weight=%.3f distance=%.2f\n",
                    sampleIndex,
                    sample.reason ? sample.reason : "<unknown>",
                    sample.areaNum,
                    sample.materialId,
                    sample.materialIndex,
                    sample.area,
                    sample.weight,
                    sample.distance);
            }
        }
        common->Printf("PathTracePrimaryPass: RT smoke light origins persistentStatic=%d/%d currentDynamic=%d frameOnlyDynamic=%d persistableDynamic=%d promotedDynamic=%d unpromotedDynamic=%d injectedMissingDynamic=%d runtimeActive=%d runtimeInactiveSkipped=%d spatialMembership=unassigned temporalReuse=off\n",
            lightUniverseStats.staticMergedSeenTriangles,
            lightUniverseStats.staticMergedMissingTriangles,
            lightUniverseStats.dynamicFrameTriangles,
            lightUniverseStats.dynamicFrameOnlyTriangles,
            lightUniverseStats.dynamicPersistableFrameTriangles,
            lightUniverseStats.dynamicPromotedFrameTriangles,
            lightUniverseStats.dynamicUnpromotedFrameTriangles,
            lightUniverseStats.injectedMissingDynamicTriangles,
            emissiveInventoryStats.capturedTriangles,
            runtimeInactiveEmissiveTriangles);
        common->Printf("PathTracePrimaryPass: RT smoke light churn enabled=%d previous/current/stayed/entered/left=%d/%d/%d/%d/%d weight previous/current/stayed/entered/left=%.3f/%.3f/%.3f/%.3f/%.3f\n",
            lightUniverseStats.activeChurnEnabled,
            lightUniverseStats.activeChurnPrevious,
            lightUniverseStats.activeChurnCurrent,
            lightUniverseStats.activeChurnStayed,
            lightUniverseStats.activeChurnEntered,
            lightUniverseStats.activeChurnLeft,
            lightUniverseStats.activeChurnPreviousWeight,
            lightUniverseStats.activeChurnCurrentWeight,
            lightUniverseStats.activeChurnStayedWeight,
            lightUniverseStats.activeChurnEnteredWeight,
            lightUniverseStats.activeChurnLeftWeight);
        int emissiveRemapValid = 0;
        int emissiveCurrentToPrevious = 0;
        int emissivePreviousToCurrent = 0;
        int emissiveCurrentZeroIdentity = 0;
        int emissivePreviousZeroIdentity = 0;
        int emissiveCurrentDuplicate = 0;
        int emissivePreviousDuplicate = 0;
        int emissiveCurrentMissing = 0;
        int emissivePreviousMissing = 0;
        int emissiveIncompatible = 0;
        for (const PathTraceEmissiveLightRemap& remap : emissiveLightRemap)
        {
            if ((remap.flags & RT_SMOKE_EMISSIVE_REMAP_VALID) != 0u)
            {
                ++emissiveRemapValid;
            }
            if (remap.currentToPreviousIndex >= 0)
            {
                ++emissiveCurrentToPrevious;
            }
            if (remap.previousToCurrentIndex >= 0)
            {
                ++emissivePreviousToCurrent;
            }
            if ((remap.flags & RT_SMOKE_EMISSIVE_REMAP_CURRENT_ZERO_IDENTITY) != 0u)
            {
                ++emissiveCurrentZeroIdentity;
            }
            if ((remap.flags & RT_SMOKE_EMISSIVE_REMAP_PREVIOUS_ZERO_IDENTITY) != 0u)
            {
                ++emissivePreviousZeroIdentity;
            }
            if ((remap.flags & RT_SMOKE_EMISSIVE_REMAP_CURRENT_DUPLICATE) != 0u)
            {
                ++emissiveCurrentDuplicate;
            }
            if ((remap.flags & RT_SMOKE_EMISSIVE_REMAP_PREVIOUS_DUPLICATE) != 0u)
            {
                ++emissivePreviousDuplicate;
            }
            if ((remap.flags & RT_SMOKE_EMISSIVE_REMAP_CURRENT_MISSING) != 0u)
            {
                ++emissiveCurrentMissing;
            }
            if ((remap.flags & RT_SMOKE_EMISSIVE_REMAP_PREVIOUS_MISSING) != 0u)
            {
                ++emissivePreviousMissing;
            }
            if ((remap.flags & RT_SMOKE_EMISSIVE_REMAP_INCOMPATIBLE) != 0u)
            {
                ++emissiveIncompatible;
            }
        }
        common->Printf("PathTracePrimaryPass: RT smoke emissive remap current=%d previous=%d records=%d valid=%d currentToPrevious=%d previousToCurrent=%d currentZeroId=%d previousZeroId=%d currentDuplicate=%d previousDuplicate=%d currentMissing=%d previousMissing=%d incompatible=%d\n",
            static_cast<int>(emissiveTriangles.size()),
            static_cast<int>(previousEmissiveTriangles.size()),
            static_cast<int>(emissiveLightRemap.size()),
            emissiveRemapValid,
            emissiveCurrentToPrevious,
            emissivePreviousToCurrent,
            emissiveCurrentZeroIdentity,
            emissivePreviousZeroIdentity,
            emissiveCurrentDuplicate,
            emissivePreviousDuplicate,
            emissiveCurrentMissing,
            emissivePreviousMissing,
            emissiveIncompatible);
        common->Printf("PathTracePrimaryPass: RTXDI unified uploaded-domain diagnostic currentUnified=%d previousUnified=%d remap=%d currentEmissive=%d previousEmissive=%d currentAnalytic=%d previousAnalytic=%d persistentEmissiveStatic=%d persistentEmissiveDynamic=%d emissiveMerged=%d emissiveSelected=%d emissiveConnected=%d emissiveDisconnected=%d emissiveWouldUpload=%d emissiveWouldDrop=%d analyticCurrentUploaded=%d analyticPreviousUploaded=%d analyticRemap=%d behavior=cpu-diagnostics-only\n",
            static_cast<int>(unifiedLights.currentLights.size()),
            static_cast<int>(unifiedLights.previousLights.size()),
            static_cast<int>(unifiedLights.currentToPreviousRemap.size()),
            static_cast<int>(emissiveTriangles.size()),
            static_cast<int>(previousEmissiveTriangles.size()),
            static_cast<int>(doomAnalyticLights.size()),
            static_cast<int>(doomAnalyticRemap.previousCandidates.size()),
            lightUniverseStats.persistentStaticTriangles,
            lightUniverseStats.persistentDynamicTriangles,
            lightUniverseStats.mergedTriangles,
            lightUniverseStats.mergedSelectedAreaTriangles,
            lightUniverseStats.mergedConnectedAreaTriangles,
            lightUniverseStats.mergedDisconnectedAreaTriangles,
            lightUniverseStats.areaFilterWouldUploadCandidates,
            lightUniverseStats.areaFilterWouldDropCandidates,
            static_cast<int>(doomAnalyticRemap.currentCandidateIdentities.size()),
            static_cast<int>(doomAnalyticRemap.previousCandidateIdentities.size()),
            static_cast<int>(doomAnalyticRemap.universeRemap.size()));
        if (lightUniverseStats.enteredSampleCount > 0)
        {
            for (int sampleIndex = 0; sampleIndex < lightUniverseStats.enteredSampleCount; ++sampleIndex)
            {
                const RtSmokeLightUniverseCandidateSample& sample = lightUniverseStats.enteredSamples[sampleIndex];
                if (!sample.valid)
                {
                    continue;
                }
                common->Printf("PathTracePrimaryPass: RT smoke light churn entered sample %d area=%d material=%u materialIndex=%u triArea=%.2f weight=%.3f distance=%.2f\n",
                    sampleIndex,
                    sample.areaNum,
                    sample.materialId,
                    sample.materialIndex,
                    sample.area,
                    sample.weight,
                    sample.distance);
            }
        }
        if (lightUniverseStats.leftSampleCount > 0)
        {
            for (int sampleIndex = 0; sampleIndex < lightUniverseStats.leftSampleCount; ++sampleIndex)
            {
                const RtSmokeLightUniverseCandidateSample& sample = lightUniverseStats.leftSamples[sampleIndex];
                if (!sample.valid)
                {
                    continue;
                }
                common->Printf("PathTracePrimaryPass: RT smoke light churn left sample %d area=%d material=%u materialIndex=%u triArea=%.2f weight=%.3f distance=%.2f\n",
                    sampleIndex,
                    sample.areaNum,
                    sample.materialId,
                    sample.materialIndex,
                    sample.area,
                    sample.weight,
                    sample.distance);
            }
        }
        r_pathTracingLightUniverseDump.SetInteger(0);
    }

    const int bufferCreateStartMs = Sys_Milliseconds();
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
    bufferCreateDesc.existingBuffers.restirLightManagerCurrentBuffer = m_smokeRestirLightManagerCurrentBuffer;
    bufferCreateDesc.existingBuffers.restirLightManagerPreviousBuffer = m_smokeRestirLightManagerPreviousBuffer;
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
    bufferCreateDesc.existingBuffers.skinnedSourceVertexBuffer = m_smokeSkinnedSourceVertexBuffer;
    bufferCreateDesc.existingBuffers.skinnedCurrentOutputVertexBuffer = m_smokeSkinnedCurrentOutputVertexBuffer;
    bufferCreateDesc.existingBuffers.skinnedPreviousPositionBuffer = m_smokeSkinnedPreviousPositionBuffer;
    bufferCreateDesc.existingBuffers.skinnedSurfaceDispatchBuffer = m_smokeSkinnedSurfaceDispatchBuffer;
    bufferCreateDesc.existingBuffers.skinnedTriangleDispatchIndexBuffer = m_smokeSkinnedTriangleDispatchIndexBuffer;
    bufferCreateDesc.existingBuffers.skinnedCurrentJointMatrixBuffer = m_smokeSkinnedCurrentJointMatrixBuffer;
    bufferCreateDesc.existingBuffers.skinnedPreviousJointMatrixBuffer = m_smokeSkinnedPreviousJointMatrixBuffer;
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
    bufferCreateDesc.restirLightManagerCurrentBytes = restirLightManagerCurrentRecords.size() * sizeof(PathTraceRestirCurrentLightRecord);
    bufferCreateDesc.restirLightManagerPreviousBytes = restirLightManagerPreviousRecords.size() * sizeof(PathTraceRestirPreviousLightRecord);
    bufferCreateDesc.restirLightManagerCurrentToPreviousBytes = restirLightManagerCurrentToPreviousRemap.size() * sizeof(uint32_t);
    bufferCreateDesc.restirLightManagerPreviousToCurrentBytes = restirLightManagerPreviousToCurrentRemap.size() * sizeof(uint32_t);
    bufferCreateDesc.restirLightManagerCurrentPayloadBytes = restirLightManagerCurrentPayloadRecords.size() * sizeof(PathTraceUnifiedLightRecord);
    bufferCreateDesc.restirLightManagerPreviousPayloadBytes = restirLightManagerPreviousPayloadRecords.size() * sizeof(PathTraceUnifiedLightRecord);
    bufferCreateDesc.rigidRouteVertexBytes = rigidRouteBuild.vertices.size() * sizeof(PathTraceSmokeVertex);
    bufferCreateDesc.rigidRouteIndexBytes = rigidRouteBuild.indexes.size() * sizeof(uint32_t);
    bufferCreateDesc.rigidRouteTriangleMaterialBytes = rigidRouteBuild.triangleMaterials.size() * sizeof(uint32_t);
    bufferCreateDesc.rigidRouteTriangleMaterialIndexBytes = rigidRouteBuild.triangleMaterialIndexes.size() * sizeof(uint32_t);
    bufferCreateDesc.rigidRouteInstanceBytes = rigidRouteBuild.instances.size() * sizeof(PathTraceRigidRouteInstance);
    bufferCreateDesc.skinnedSourceVertexBytes = skinnedGpuScaffold.sourceVertices.size() * sizeof(PathTraceSkinnedSourceVertex);
    bufferCreateDesc.skinnedCurrentOutputVertexBytes = skinnedGpuScaffold.currentOutputVertices.size() * sizeof(PathTraceSmokeVertex);
    bufferCreateDesc.skinnedPreviousPositionBytes = skinnedGpuScaffold.previousPositions.size() * sizeof(PathTraceSkinnedPreviousPosition);
    bufferCreateDesc.skinnedSurfaceDispatchBytes = skinnedGpuScaffold.dispatchRecords.size() * sizeof(PathTraceSkinnedSurfaceDispatchRecord);
    bufferCreateDesc.skinnedTriangleDispatchIndexBytes = skinnedGpuScaffold.dynamicTriangleDispatchIndexes.size() * sizeof(uint32_t);
    bufferCreateDesc.skinnedCurrentJointMatrixBytes = skinnedGpuScaffold.currentJointMatrices.size() * sizeof(PathTraceSkinnedJointMatrix);
    bufferCreateDesc.skinnedPreviousJointMatrixBytes = skinnedGpuScaffold.previousJointMatrices.size() * sizeof(PathTraceSkinnedJointMatrix);
    }
    RtSmokeSceneBufferCreateResult bufferCreateResult;
    {
        OPTICK_EVENT("PT Create Scene Buffers");
        bufferCreateResult = CreateSmokeSceneBuffers(bufferCreateDesc);
    }
    if (!bufferCreateResult.Succeeded())
    {
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
    nvrhi::BufferHandle smokeRestirLightManagerCurrentBuffer = smokeBuffers.restirLightManagerCurrentBuffer;
    nvrhi::BufferHandle smokeRestirLightManagerPreviousBuffer = smokeBuffers.restirLightManagerPreviousBuffer;
    nvrhi::BufferHandle smokeRestirLightManagerCurrentToPreviousBuffer = smokeBuffers.restirLightManagerCurrentToPreviousBuffer;
    nvrhi::BufferHandle smokeRestirLightManagerPreviousToCurrentBuffer = smokeBuffers.restirLightManagerPreviousToCurrentBuffer;
    nvrhi::BufferHandle smokeRestirLightManagerCurrentPayloadBuffer = smokeBuffers.restirLightManagerCurrentPayloadBuffer;
    nvrhi::BufferHandle smokeRestirLightManagerPreviousPayloadBuffer = smokeBuffers.restirLightManagerPreviousPayloadBuffer;
    nvrhi::BufferHandle smokeRigidRouteVertexBuffer = smokeBuffers.rigidRouteVertexBuffer;
    nvrhi::BufferHandle smokeRigidRouteIndexBuffer = smokeBuffers.rigidRouteIndexBuffer;
    nvrhi::BufferHandle smokeRigidRouteTriangleMaterialBuffer = smokeBuffers.rigidRouteTriangleMaterialBuffer;
    nvrhi::BufferHandle smokeRigidRouteTriangleMaterialIndexBuffer = smokeBuffers.rigidRouteTriangleMaterialIndexBuffer;
    nvrhi::BufferHandle smokeRigidRouteInstanceBuffer = smokeBuffers.rigidRouteInstanceBuffer;
    nvrhi::BufferHandle smokeSkinnedSourceVertexBuffer = smokeBuffers.skinnedSourceVertexBuffer;
    nvrhi::BufferHandle smokeSkinnedCurrentOutputVertexBuffer = smokeBuffers.skinnedCurrentOutputVertexBuffer;
    nvrhi::BufferHandle smokeSkinnedPreviousPositionBuffer = smokeBuffers.skinnedPreviousPositionBuffer;
    nvrhi::BufferHandle smokeSkinnedSurfaceDispatchBuffer = smokeBuffers.skinnedSurfaceDispatchBuffer;
    nvrhi::BufferHandle smokeSkinnedTriangleDispatchIndexBuffer = smokeBuffers.skinnedTriangleDispatchIndexBuffer;
    nvrhi::BufferHandle smokeSkinnedCurrentJointMatrixBuffer = smokeBuffers.skinnedCurrentJointMatrixBuffer;
    nvrhi::BufferHandle smokeSkinnedPreviousJointMatrixBuffer = smokeBuffers.skinnedPreviousJointMatrixBuffer;
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
        accelerationPlanInput.staticCache.staticCacheChanged = staticCacheChanged;
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

    if (r_pathTracingCpuPlanningDump.GetInteger() != 0)
    {
        const bool asyncFuturePending = m_smokeAccelerationPlanFuture.valid();
        common->Printf(
            "PathTracePrimaryPass: PT CPU planning async=%d asyncBvh=%d asyncBvhJobs=%d acceptedFromAsync=%d cached=%d queued=%d rigid(accepted/cached/queued)=%d/%d/%d accepted=%llu stale=%llu late=%llu syncFallback=%llu snapshotMs=%.3f queueMs=%.3f workerMs=%.3f renderSubmitMs=%.3f rigidCounters(accepted/stale/late/sync)=%llu/%llu/%llu/%llu rigidTiming(snapshot/queue/worker/render)=%.3f/%.3f/%.3f/%.3f rigidPlanMs=%d gen(frame=%llu scene=%llu geometry=%llu material=%llu input=%llu rigidInput=%llu)\n",
            asyncCpuPlanning ? 1 : 0,
            asyncBvhFramePlanning ? 1 : 0,
            asyncBvhJobCount,
            accelerationPlanAcceptedFromAsync ? 1 : 0,
            asyncPlanAlreadyCached ? 1 : 0,
            asyncFuturePending ? 1 : 0,
            rigidTlasPlanAcceptedFromAsync ? 1 : 0,
            rigidTlasAsyncPlanCached ? 1 : 0,
            rigidTlasAsyncPlanQueued ? 1 : 0,
            static_cast<unsigned long long>(m_smokeCpuWorkState.acceptedResultCount),
            static_cast<unsigned long long>(m_smokeCpuWorkState.rejectedStaleResultCount),
            static_cast<unsigned long long>(m_smokeCpuWorkState.lateResultCount),
            static_cast<unsigned long long>(m_smokeCpuWorkState.syncFallbackCount),
            m_smokeCpuWorkState.lastAcceptedTiming.snapshotCaptureMs,
            m_smokeCpuWorkState.lastAcceptedTiming.queueWaitMs,
            m_smokeCpuWorkState.lastAcceptedTiming.workerExecutionMs,
            m_smokeCpuWorkState.lastAcceptedTiming.renderSubmitMs,
            static_cast<unsigned long long>(m_smokeRigidTlasCpuWorkState.acceptedResultCount),
            static_cast<unsigned long long>(m_smokeRigidTlasCpuWorkState.rejectedStaleResultCount),
            static_cast<unsigned long long>(m_smokeRigidTlasCpuWorkState.lateResultCount),
            static_cast<unsigned long long>(m_smokeRigidTlasCpuWorkState.syncFallbackCount),
            m_smokeRigidTlasCpuWorkState.lastAcceptedTiming.snapshotCaptureMs,
            m_smokeRigidTlasCpuWorkState.lastAcceptedTiming.queueWaitMs,
            m_smokeRigidTlasCpuWorkState.lastAcceptedTiming.workerExecutionMs,
            m_smokeRigidTlasCpuWorkState.lastAcceptedTiming.renderSubmitMs,
            rigidTlasPlanMs,
            static_cast<unsigned long long>(accelerationPlanGeneration.frameIndex),
            static_cast<unsigned long long>(accelerationPlanGeneration.sceneGeneration),
            static_cast<unsigned long long>(accelerationPlanGeneration.geometryGeneration),
            static_cast<unsigned long long>(accelerationPlanGeneration.materialGeneration),
            static_cast<unsigned long long>(accelerationPlanGeneration.lightGeneration),
            static_cast<unsigned long long>(rigidTlasPlanInputToken));
        r_pathTracingCpuPlanningDump.SetInteger(0);
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

    const char* uploadItemNames[] = {
        "staticVertex",
        "staticIndex",
        "staticTriangleClass",
        "staticTriangleMaterial",
        "staticTriangleMaterialIndex",
        "previousStaticVertex",
        "previousStaticIndex",
        "previousStaticTriangleClass",
        "previousStaticTriangleMaterial",
        "previousStaticTriangleMaterialIndex",
        "dynamicVertex",
        "dynamicIndex",
        "dynamicTriangleClass",
        "dynamicTriangleMaterial",
        "dynamicTriangleMaterialIndex",
        "materialTable",
        "dynamicMaterial",
        "emissiveTriangle",
        "previousEmissiveTriangle",
        "emissiveRemap",
        "emissiveDistribution",
        "lightCandidate",
        "doomAnalyticLight",
        "doomAnalyticPreviousLight",
        "doomAnalyticCurrentIdentity",
        "doomAnalyticPreviousIdentity",
        "doomAnalyticRemap",
        "unifiedLight",
        "unifiedPreviousLight",
        "unifiedLightRemap",
        "restirLightManagerCurrent",
        "restirLightManagerPrevious",
        "restirLightManagerCurrentToPrevious",
        "restirLightManagerPreviousToCurrent",
        "restirLightManagerCurrentPayload",
        "restirLightManagerPreviousPayload",
        "rigidRouteVertex",
        "rigidRouteIndex",
        "rigidRouteTriangleMaterial",
        "rigidRouteTriangleMaterialIndex",
        "rigidRouteInstance",
        "skinnedSourceVertex",
        "skinnedCurrentOutputVertex",
        "skinnedPreviousPosition",
        "skinnedSurfaceDispatch",
        "skinnedTriangleDispatchIndex",
        "skinnedCurrentJointMatrix",
        "skinnedPreviousJointMatrix"
    };
    const RtSmokeBufferUploadItem uploadItems[] = {
        MakeSmokeVectorUploadItem(smokeStaticVertexBuffer, staticVertexCache, nvrhi::ResourceStates::AccelStructBuildInput, staticBlasCacheHit, useStaticDirtyRangeUploads ? geometryUniverseStats.staticDirtyVertexOffset : -1, geometryUniverseStats.staticDirtyVertexCount),
        MakeSmokeVectorUploadItem(smokeStaticIndexBuffer, staticIndexCache, nvrhi::ResourceStates::AccelStructBuildInput, staticBlasCacheHit, useStaticDirtyRangeUploads ? geometryUniverseStats.staticDirtyIndexOffset : -1, geometryUniverseStats.staticDirtyIndexCount),
        MakeSmokeVectorUploadItem(smokeStaticTriangleClassBuffer, staticTriangleClassCache, nvrhi::ResourceStates::ShaderResource, staticBlasCacheHit, useStaticDirtyRangeUploads ? geometryUniverseStats.staticDirtyTriangleOffset : -1, geometryUniverseStats.staticDirtyTriangleCount),
        MakeSmokeVectorUploadItem(smokeStaticTriangleMaterialBuffer, staticTriangleMaterialCache, nvrhi::ResourceStates::ShaderResource, skipStaticTriangleMaterialUpload),
        MakeSmokeVectorUploadItem(smokeStaticTriangleMaterialIndexBuffer, materialTable.staticMaterialIndexes, nvrhi::ResourceStates::ShaderResource, skipStaticTriangleMaterialIndexUpload),
        MakeSmokeVectorUploadItem(smokePreviousStaticVertexBuffer, previousStaticVertexCache, nvrhi::ResourceStates::ShaderResource, skipPreviousStaticGeometryUpload),
        MakeSmokeVectorUploadItem(smokePreviousStaticIndexBuffer, previousStaticIndexCache, nvrhi::ResourceStates::ShaderResource, skipPreviousStaticGeometryUpload),
        MakeSmokeVectorUploadItem(smokePreviousStaticTriangleClassBuffer, previousStaticTriangleClassCache, nvrhi::ResourceStates::ShaderResource, skipPreviousStaticGeometryUpload),
        MakeSmokeVectorUploadItem(smokePreviousStaticTriangleMaterialBuffer, previousStaticTriangleMaterialCache, nvrhi::ResourceStates::ShaderResource, skipPreviousStaticGeometryUpload),
        MakeSmokeVectorUploadItem(smokePreviousStaticTriangleMaterialIndexBuffer, previousStaticTriangleMaterialIndexCache, nvrhi::ResourceStates::ShaderResource, skipPreviousStaticMaterialIndexUpload),
        MakeSmokeVectorUploadItem(smokeDynamicVertexBuffer, dynamicVertexData, skinnedGpuComputeReady && skinnedGpuComputeTargetsDynamicVertices ? nvrhi::ResourceStates::UnorderedAccess : nvrhi::ResourceStates::AccelStructBuildInput, false),
        MakeSmokeVectorUploadItem(smokeDynamicIndexBuffer, dynamicIndexData, nvrhi::ResourceStates::AccelStructBuildInput, false),
        MakeSmokeVectorUploadItem(smokeDynamicTriangleClassBuffer, dynamicTriangleClassData, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeDynamicTriangleMaterialBuffer, dynamicTriangleMaterialData, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeDynamicTriangleMaterialIndexBuffer, materialTable.dynamicMaterialIndexes, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeMaterialTableBuffer, gpuMaterialTableMaterials, nvrhi::ResourceStates::ShaderResource, skipMaterialTableUpload, materialTableUploadOffset, materialTableUploadCount),
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
        MakeSmokeVectorUploadItem(smokeRestirLightManagerCurrentBuffer, restirLightManagerCurrentRecords, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeRestirLightManagerPreviousBuffer, restirLightManagerPreviousRecords, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeRestirLightManagerCurrentToPreviousBuffer, restirLightManagerCurrentToPreviousRemap, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeRestirLightManagerPreviousToCurrentBuffer, restirLightManagerPreviousToCurrentRemap, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeRestirLightManagerCurrentPayloadBuffer, restirLightManagerCurrentPayloadRecords, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeRestirLightManagerPreviousPayloadBuffer, restirLightManagerPreviousPayloadRecords, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeRigidRouteVertexBuffer, rigidRouteBuild.vertices, nvrhi::ResourceStates::ShaderResource, skipRigidRouteSideBufferUpload),
        MakeSmokeVectorUploadItem(smokeRigidRouteIndexBuffer, rigidRouteBuild.indexes, nvrhi::ResourceStates::ShaderResource, skipRigidRouteSideBufferUpload),
        MakeSmokeVectorUploadItem(smokeRigidRouteTriangleMaterialBuffer, rigidRouteBuild.triangleMaterials, nvrhi::ResourceStates::ShaderResource, skipRigidRouteSideBufferUpload),
        MakeSmokeVectorUploadItem(smokeRigidRouteTriangleMaterialIndexBuffer, rigidRouteBuild.triangleMaterialIndexes, nvrhi::ResourceStates::ShaderResource, skipRigidRouteSideBufferUpload),
        MakeSmokeVectorUploadItem(smokeRigidRouteInstanceBuffer, rigidRouteBuild.instances, nvrhi::ResourceStates::ShaderResource, skipRigidRouteInstanceBufferUpload),
        MakeSmokeVectorUploadItem(smokeSkinnedSourceVertexBuffer, skinnedGpuScaffold.sourceVertices, nvrhi::ResourceStates::ShaderResource, false),
        skinnedGpuComputeReady && !skinnedGpuComputeTargetsDynamicVertices
            ? MakeSmokeBufferStateItem(smokeSkinnedCurrentOutputVertexBuffer, nvrhi::ResourceStates::UnorderedAccess)
            : (skinnedGpuComputeTargetsDynamicVertices
                ? MakeSmokeBufferStateItem(smokeSkinnedCurrentOutputVertexBuffer, nvrhi::ResourceStates::ShaderResource)
                : MakeSmokeVectorUploadItem(smokeSkinnedCurrentOutputVertexBuffer, skinnedGpuScaffold.currentOutputVertices, nvrhi::ResourceStates::ShaderResource, false)),
        MakeSmokeVectorUploadItem(smokeSkinnedPreviousPositionBuffer, skinnedGpuScaffold.previousPositions, skinnedGpuComputeReady ? nvrhi::ResourceStates::UnorderedAccess : nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeSkinnedSurfaceDispatchBuffer, skinnedGpuComputeDispatchRecords, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeSkinnedTriangleDispatchIndexBuffer, skinnedGpuScaffold.dynamicTriangleDispatchIndexes, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeSkinnedCurrentJointMatrixBuffer, skinnedGpuScaffold.currentJointMatrices, nvrhi::ResourceStates::ShaderResource, false),
        MakeSmokeVectorUploadItem(smokeSkinnedPreviousJointMatrixBuffer, skinnedGpuScaffold.previousJointMatrices, nvrhi::ResourceStates::ShaderResource, false)
    };
    static_assert(
        sizeof(uploadItemNames) / sizeof(uploadItemNames[0]) == sizeof(uploadItems) / sizeof(uploadItems[0]),
        "RT smoke upload item names must match upload item order");
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
    if (dumpRestirLightManager)
    {
        common->Printf("PathTracePrimaryPass: ReSTIR light manager GPU buffers uploaded current=%u previous=%u currentToPrevious=%u previousToCurrent=%u payloadCurrent=%u payloadPrevious=%u bytes=%llu/%llu/%llu/%llu payloadBytes=%llu/%llu behavior=payload-bound payloadBindings=t66/t67\n",
            static_cast<uint32_t>(restirLightManagerCurrentRecords.size()),
            static_cast<uint32_t>(restirLightManagerPreviousRecords.size()),
            static_cast<uint32_t>(restirLightManagerCurrentToPreviousRemap.size()),
            static_cast<uint32_t>(restirLightManagerPreviousToCurrentRemap.size()),
            static_cast<uint32_t>(restirLightManagerCurrentPayloadRecords.size()),
            static_cast<uint32_t>(restirLightManagerPreviousPayloadRecords.size()),
            static_cast<unsigned long long>(bufferCreateDesc.restirLightManagerCurrentBytes),
            static_cast<unsigned long long>(bufferCreateDesc.restirLightManagerPreviousBytes),
            static_cast<unsigned long long>(bufferCreateDesc.restirLightManagerCurrentToPreviousBytes),
            static_cast<unsigned long long>(bufferCreateDesc.restirLightManagerPreviousToCurrentBytes),
            static_cast<unsigned long long>(bufferCreateDesc.restirLightManagerCurrentPayloadBytes),
            static_cast<unsigned long long>(bufferCreateDesc.restirLightManagerPreviousPayloadBytes));
        r_pathTracingRestirLightManagerDump.SetInteger(0);
    }
    {
        OPTICK_EVENT("PT Skinned GPU Compute Dispatch");
        if (skinnedGpuComputeReady)
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

    RtSmokeAccelSubmitDesc accelSubmitDesc;
    std::vector<nvrhi::rt::InstanceDesc> rigidTlasRouteInstances;
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
            if (r_pathTracingSmokeLog.GetInteger() != 0 && routedRigidInstances > 0 && (m_smokeGeometryFrameIndex % 120ull) == 1ull)
            {
                common->Printf("PathTracePrimaryPass: PT rigid TLAS route debug mode active mode=%d routedInstances=%d renderPath=dynamicFallback traceMask=%s\n",
                    requestedDebugMode,
                    routedRigidInstances,
                    requestedDebugMode == 23 ? "rigidOnly" : (requestedDebugMode == 24 ? "fallbackAndRigidValidation" : (requestedDebugMode == 25 ? "fallbackAndRigidLighting" : (requestedDebugMode == 29 ? "mode29RestirPTPrimaryHistory" : (requestedDebugMode == 28 ? "mode28RestirPTInitialVisibility" : (requestedDebugMode == 27 ? "mode27RestirPTInitialShading" : (requestedDebugMode == 26 ? "mode26RestirPTInitial" : (requestedDebugMode == 20 ? "mode20Integration" : "mode18Integration"))))))));
            }
        }
    }
    accelSubmitDesc.commandList = commandList;
    accelSubmitDesc.tlas = m_smokeTlas;
    accelSubmitDesc.staticBlas = smokeStaticBlas;
    accelSubmitDesc.dynamicBlas = smokeDynamicBlas;
    accelSubmitDesc.staticBlasDesc = smokeStaticBlasDesc;
    accelSubmitDesc.dynamicBlasDesc = smokeDynamicBlasDesc;
    accelSubmitDesc.extraTlasInstances = routeRigidTlasInstances ? &rigidTlasRouteInstances : nullptr;
    accelSubmitDesc.hasStaticBlas = hasStaticBlas;
    accelSubmitDesc.hasDynamicBlas = hasDynamicBlas;
    accelSubmitDesc.staticBlasCacheHit = staticBlasCacheHit;
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
        common->Printf("PathTracePrimaryPass: failed to submit RT smoke acceleration structures\n");
        return;
    }
    const int blasSubmitMs = accelSubmitTiming.blasSubmitMs;
    const int tlasSubmitMs = accelSubmitTiming.tlasSubmitMs;
    const int accelSubmitMs = accelSubmitTiming.accelSubmitMs;
    RtPathTraceCpuWorkRecordRenderSubmit(m_smokeCpuWorkState, accelerationPlanGeneration, static_cast<double>(accelSubmitMs));
    const int instanceCount = accelSubmitTiming.instanceCount;

    const nvrhi::TextureHandle fallbackTexture = globalImages && globalImages->whiteImage ? globalImages->whiteImage->GetTextureHandle() : nullptr;
    if (!fallbackTexture)
    {
        common->Printf("PathTracePrimaryPass: failed to find RT smoke fallback material texture\n");
        return;
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
    bindingBuildDesc.constantsBuffer = m_smokeConstantsBuffer;
    bindingBuildDesc.restirPTConstantsBuffer = m_restirPTConstantsBuffer;
    bindingBuildDesc.boundsOverlayLineBuffer = m_smokeBoundsOverlayLineBuffer;
    bindingBuildDesc.bindingLayout = m_smokeBindingLayout;
    bindingBuildDesc.textureBindlessLayout = m_smokeTextureBindlessLayout;
    const int sceneRetireFrames = idMath::ClampInt(0, 32, r_pathTracingSceneRetireFrames.GetInteger());
    bindingBuildDesc.existingTextureDescriptorTable = m_smokeTextureDescriptorTable;
    bindingBuildDesc.existingActiveTextureTable = &m_smokeActiveTextureTable;
    bindingBuildDesc.allowExistingTextureDescriptorTableWrites = sceneRetireFrames <= 0;
    bindingBuildDesc.sampler = m_backend->GetCommonPasses().m_AnisotropicWrapSampler;
    bindingBuildDesc.buffers = smokeBuffers;
    bindingBuildDesc.reservoirBuffers = m_frameResources.smokeReservoirBuffers;
    bindingBuildDesc.restirPTReservoirBuffers = m_frameResources.restirPTReservoirBuffers;
    bindingBuildDesc.restirPTDiReservoirBuffers = m_frameResources.restirPTDiReservoirBuffers;
    bindingBuildDesc.restirPTGiReservoirBuffers = m_frameResources.restirPTGiReservoirBuffers;
    bindingBuildDesc.remixRtxdiDiReservoirBuffer = m_remixRtxdiResources.GetDomain(PATH_TRACE_REMIX_RTXDI_RESERVOIR_DOMAIN_DI).reservoirs;
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
    sceneInputLightSignature = HashSmokeBytes(sceneInputLightSignature, &lightUniverseStats.generation, sizeof(lightUniverseStats.generation));
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
    const uint64_t materialUploadBytes = SumSmokeUploadBytes(uploadItems, 15, 2);
    const uint64_t lightUploadBytes = SumSmokeUploadBytes(uploadItems, 17, 17);
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
    const uint64_t skinnedUploadBytes = SumSmokeUploadBytes(uploadItems, 41, 7);
    const uint64_t skinnedSkippedUploadBytes = SumSmokeSkippedUploadBytes(uploadItems, 41, 7);
    if (r_pathTracingBufferUploadDump.GetInteger() != 0)
    {
        const uint64_t staticSkippedUploadBytes = SumSmokeSkippedUploadBytes(uploadItems, 0, 5);
        const uint64_t dynamicSkippedUploadBytes = SumSmokeSkippedUploadBytes(uploadItems, 10, 5);
        const uint64_t materialSkippedUploadBytes = SumSmokeSkippedUploadBytes(uploadItems, 15, 2);
        const uint64_t bufferDumpLightUploadBytes = SumSmokeUploadBytes(uploadItems, 17, 19);
        const uint64_t lightSkippedUploadBytes = SumSmokeSkippedUploadBytes(uploadItems, 17, 19);
        common->Printf(
            "PathTracePrimaryPass: PT buffer upload dump frame=%llu residency=%d staticBlasCacheHit=%d dirtyRange=%d bufferUploadMs=%d totals write(static/prevStatic/dynamic/material/light/rigid/skinned)=%llu/%llu/%llu/%llu/%llu/%llu/%llu skip(static/prevStatic/dynamic/material/light/rigid/skinned)=%llu/%llu/%llu/%llu/%llu/%llu/%llu\n",
            static_cast<unsigned long long>(m_smokeGeometryFrameIndex),
            r_pathTracingResidency.GetInteger() != 0 ? 1 : 0,
            staticBlasCacheHit ? 1 : 0,
            useStaticDirtyRangeUploads ? 1 : 0,
            bufferUploadMs,
            static_cast<unsigned long long>(staticUploadBytes),
            static_cast<unsigned long long>(previousStaticUploadBytes),
            static_cast<unsigned long long>(dynamicUploadBytes),
            static_cast<unsigned long long>(materialUploadBytes),
            static_cast<unsigned long long>(bufferDumpLightUploadBytes),
            static_cast<unsigned long long>(rigidRouteUploadBytes),
            static_cast<unsigned long long>(skinnedUploadBytes),
            static_cast<unsigned long long>(staticSkippedUploadBytes),
            static_cast<unsigned long long>(previousStaticUploadSkippedBytes),
            static_cast<unsigned long long>(dynamicSkippedUploadBytes),
            static_cast<unsigned long long>(materialSkippedUploadBytes),
            static_cast<unsigned long long>(lightSkippedUploadBytes),
            static_cast<unsigned long long>(rigidRouteSkippedUploadBytes),
            static_cast<unsigned long long>(skinnedSkippedUploadBytes));
        const int uploadItemCount = static_cast<int>(sizeof(uploadItems) / sizeof(uploadItems[0]));
        for (int uploadItemIndex = 0; uploadItemIndex < uploadItemCount; ++uploadItemIndex)
        {
            const RtSmokeBufferUploadItem& item = uploadItems[uploadItemIndex];
            common->Printf(
                "PathTracePrimaryPass: PT buffer upload item %02d name=%s buffer=%d data=%d skip=%d writeBytes=%llu skippedBytes=%llu plannedBytes=%llu srcOffset=%llu dstOffset=%llu capacity=%llu\n",
                uploadItemIndex,
                uploadItemNames[uploadItemIndex],
                item.buffer ? 1 : 0,
                item.data ? 1 : 0,
                item.skip ? 1 : 0,
                static_cast<unsigned long long>(SmokeUploadItemWriteBytes(item)),
                static_cast<unsigned long long>(SmokeUploadItemSkippedBytes(item)),
                static_cast<unsigned long long>(item.byteSize),
                static_cast<unsigned long long>(item.sourceOffsetBytes),
                static_cast<unsigned long long>(item.destOffsetBytes),
                static_cast<unsigned long long>(SmokeUploadItemBufferCapacity(item)));
        }
        r_pathTracingBufferUploadDump.SetInteger(0);
    }
    if (r_pathTracingMaterialUploadDump.GetInteger() != 0)
    {
        const RtSmokeBufferUploadItem& materialTableUpload = uploadItems[15];
        const RtSmokeBufferUploadItem& dynamicMaterialUpload = uploadItems[16];
        const RtSmokeMaterialUploadDiffSummary materialDiffSummary = BuildSmokeMaterialUploadDiffSummary(
            m_smokeMaterialTableMaterials,
            gpuMaterialTableMaterials,
            materialTable.materialIds,
            materialTableUploadOffset,
            materialTableUploadCount);
        const RtSmokeDynamicMaterialUploadDiffSummary dynamicDiffSummary = BuildSmokeDynamicMaterialUploadDiffSummary(
            m_smokeDynamicMaterialRecords,
            dynamicMaterialRecords,
            dynamicMaterialUploadOffset,
            dynamicMaterialUploadCount);
        common->Printf("PathTracePrimaryPass: PT material upload dump frame=%llu residency=%d materialResidency=%d materialCacheHit=%d materialGpuStable=%d descriptor(active/created/written/slots/overLimit)=%d/%d/%d/%d/%d materialTable entries(prev/current)=%d/%d bufferReused=%d fullBytes=%llu uploadBytes=%llu skip=%d range(valid/offset/count)=%d/%d/%d diff(rows/first/last/debug/emissive/texture/alpha/flags/classifier/other)=%d/%d/%d/%d/%d/%d/%d/%d/%d/%d samples=%d:%u,%d:%u,%d:%u,%d:%u dynamicRecords(prev/current)=%d/%d bufferReused=%d fullBytes=%llu uploadBytes=%llu skip=%d range(valid/offset/count)=%d/%d/%d diff(rows/first/last/color/texMatrix/identity/flags/other)=%d/%d/%d/%d/%d/%d/%d/%d samples=%d:%u,%d:%u,%d:%u,%d:%u materialIds(staticTri/u=%d/%d staticTable/u=%d/%d emissive/u=%d/%d rigid/u=%d/%d dynamic/u=%d/%d hydrate=%d table=%d) totalMaterialUploadBytes=%llu signatures material=%llu dynamic=%llu\n",
            static_cast<unsigned long long>(m_smokeGeometryFrameIndex),
            r_pathTracingResidency.GetInteger() != 0 ? 1 : 0,
            r_pathTracingResidencyMaterial.GetInteger() != 0 ? 1 : 0,
            materialTableCacheHit ? 1 : 0,
            stableGpuMaterialTableCovered ? 1 : 0,
            Max(0, static_cast<int>(bindingBuildResult.activeTextureTable.size()) - 1),
            bindingBuildResult.textureDescriptorTableCreated ? 1 : 0,
            bindingBuildResult.textureDescriptorTableWritten ? 1 : 0,
            bindingBuildResult.textureDescriptorSlotsWritten,
            materialTable.materialsOverTextureSlotLimit,
            static_cast<int>(m_smokeMaterialTableMaterials.size()),
            static_cast<int>(gpuMaterialTableMaterials.size()),
            materialTableBufferReused ? 1 : 0,
            static_cast<unsigned long long>(bufferCreateDesc.materialTableBytes),
            static_cast<unsigned long long>(materialTableUpload.byteSize),
            materialTableUpload.skip ? 1 : 0,
            materialTableRangeValid ? 1 : 0,
            materialTableUploadOffset,
            materialTableUploadCount,
            materialDiffSummary.rows,
            materialDiffSummary.firstRow,
            materialDiffSummary.lastRow,
            materialDiffSummary.debugAlbedoRows,
            materialDiffSummary.emissiveRows,
            materialDiffSummary.textureRows,
            materialDiffSummary.alphaRows,
            materialDiffSummary.flagRows,
            materialDiffSummary.classifierRows,
            materialDiffSummary.otherRows,
            SmokeMaterialDiffSampleRow(materialDiffSummary, 0),
            SmokeMaterialDiffSampleId(materialDiffSummary, 0),
            SmokeMaterialDiffSampleRow(materialDiffSummary, 1),
            SmokeMaterialDiffSampleId(materialDiffSummary, 1),
            SmokeMaterialDiffSampleRow(materialDiffSummary, 2),
            SmokeMaterialDiffSampleId(materialDiffSummary, 2),
            SmokeMaterialDiffSampleRow(materialDiffSummary, 3),
            SmokeMaterialDiffSampleId(materialDiffSummary, 3),
            static_cast<int>(m_smokeDynamicMaterialRecords.size()),
            static_cast<int>(dynamicMaterialRecords.size()),
            dynamicMaterialBufferReused ? 1 : 0,
            static_cast<unsigned long long>(bufferCreateDesc.dynamicMaterialBytes),
            static_cast<unsigned long long>(dynamicMaterialUpload.byteSize),
            dynamicMaterialUpload.skip ? 1 : 0,
            dynamicMaterialRangeValid ? 1 : 0,
            dynamicMaterialUploadOffset,
            dynamicMaterialUploadCount,
            dynamicDiffSummary.rows,
            dynamicDiffSummary.firstRow,
            dynamicDiffSummary.lastRow,
            dynamicDiffSummary.colorRows,
            dynamicDiffSummary.texMatrixRows,
            dynamicDiffSummary.identityRows,
            dynamicDiffSummary.flagRows,
            dynamicDiffSummary.otherRows,
            SmokeDynamicMaterialDiffSampleRow(dynamicDiffSummary, 0),
            SmokeDynamicMaterialDiffSampleId(dynamicDiffSummary, 0),
            SmokeDynamicMaterialDiffSampleRow(dynamicDiffSummary, 1),
            SmokeDynamicMaterialDiffSampleId(dynamicDiffSummary, 1),
            SmokeDynamicMaterialDiffSampleRow(dynamicDiffSummary, 2),
            SmokeDynamicMaterialDiffSampleId(dynamicDiffSummary, 2),
            SmokeDynamicMaterialDiffSampleRow(dynamicDiffSummary, 3),
            SmokeDynamicMaterialDiffSampleId(dynamicDiffSummary, 3),
            static_cast<int>(staticTriangleMaterialCache.size()),
            CountUniqueMaterialIds(staticTriangleMaterialCache),
            static_cast<int>(materialTableStaticIds.size()),
            CountUniqueMaterialIds(materialTableStaticIds),
            static_cast<int>(fullLevelStaticEmissiveMaterialIds.size()),
            CountUniqueMaterialIds(fullLevelStaticEmissiveMaterialIds),
            static_cast<int>(rigidRouteMaterialIds.size()),
            CountUniqueMaterialIds(rigidRouteMaterialIds),
            static_cast<int>(dynamicTriangleMaterialData.size()),
            CountUniqueMaterialIds(dynamicTriangleMaterialData),
            materialHydrationIds ? static_cast<int>(materialHydrationIds->size()) : 0,
            static_cast<int>(materialTable.materialIds.size()),
            static_cast<unsigned long long>(materialUploadBytes),
            static_cast<unsigned long long>(materialTableUploadSignature),
            static_cast<unsigned long long>(dynamicMaterialUploadSignature));
        r_pathTracingMaterialUploadDump.SetInteger(0);
    }
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
    uint64 debugFeaturePolicy = static_cast<uint64>(requestedDebugMode);
    if (pdfNeeVerifierStaticEmissiveProducerPolicy)
    {
        debugFeaturePolicy |= 1ull << 32;
    }
    sceneInputs.signatures.debugFeaturePolicy = debugFeaturePolicy;
    sceneInputs.signatures.cpuUploadGeneration = m_smokeGeometryFrameIndex;
    sceneInputs.signatures.reservoirScene = reservoirSceneSignature;

    sceneInputs.portalPolicy.sceneSource = sceneSource;
    sceneInputs.portalPolicy.viewArea = viewDef ? viewDef->areaNum : -1;
    sceneInputs.portalPolicy.currentArea = lightUniverseStats.currentArea >= 0 ? lightUniverseStats.currentArea : (viewDef ? viewDef->areaNum : -1);
    sceneInputs.portalPolicy.totalAreas = lightUniverseStats.totalAreas;
    sceneInputs.portalPolicy.staticAreaPreloadSteps = idMath::ClampInt(0, 8, r_pathTracingStaticAreaPreloadPortalSteps.GetInteger());
    sceneInputs.portalPolicy.rigidResidencySteps = idMath::ClampInt(0, 8, r_pathTracingRigidResidencyPortalSteps.GetInteger());
    sceneInputs.portalPolicy.lightAreaSteps = idMath::ClampInt(0, 8, r_pathTracingLightAreaPortalSteps.GetInteger());
    sceneInputs.portalPolicy.sceneUniverseSteps = idMath::ClampInt(0, 8, r_pathTracingScenePortalSteps.GetInteger());
    sceneInputs.portalPolicy.selectedAreaCount = lightUniverseStats.selectedAreaCount;
    sceneInputs.portalPolicy.portalEdges = lightUniverseStats.selectedPortalEdges;
    sceneInputs.portalPolicy.blockedPortalEdges = lightUniverseStats.selectedBlockedPortalEdges;
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
    sceneInputs.geometry.staticVertexBuffer = smokeStaticVertexBuffer;
    sceneInputs.geometry.staticIndexBuffer = smokeStaticIndexBuffer;
    sceneInputs.geometry.staticTriangleClassBuffer = smokeStaticTriangleClassBuffer;
    sceneInputs.geometry.staticTriangleMaterialBuffer = smokeStaticTriangleMaterialBuffer;
    sceneInputs.geometry.staticTriangleMaterialIndexBuffer = smokeStaticTriangleMaterialIndexBuffer;
    sceneInputs.geometry.previousStaticVertexBuffer = smokePreviousStaticVertexBuffer;
    sceneInputs.geometry.previousStaticIndexBuffer = smokePreviousStaticIndexBuffer;
    sceneInputs.geometry.previousStaticTriangleClassBuffer = smokePreviousStaticTriangleClassBuffer;
    sceneInputs.geometry.previousStaticTriangleMaterialBuffer = smokePreviousStaticTriangleMaterialBuffer;
    sceneInputs.geometry.previousStaticTriangleMaterialIndexBuffer = smokePreviousStaticTriangleMaterialIndexBuffer;
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
    sceneInputs.geometry.skinnedSourceVertexBuffer = smokeSkinnedSourceVertexBuffer;
    sceneInputs.geometry.skinnedCurrentOutputVertexBuffer = smokeSkinnedCurrentOutputVertexBuffer;
    sceneInputs.geometry.skinnedPreviousPositionBuffer = smokeSkinnedPreviousPositionBuffer;
    sceneInputs.geometry.skinnedSurfaceDispatchBuffer = smokeSkinnedSurfaceDispatchBuffer;
    sceneInputs.geometry.skinnedTriangleDispatchIndexBuffer = smokeSkinnedTriangleDispatchIndexBuffer;
    sceneInputs.geometry.skinnedCurrentJointMatrixBuffer = smokeSkinnedCurrentJointMatrixBuffer;
    sceneInputs.geometry.skinnedPreviousJointMatrixBuffer = smokeSkinnedPreviousJointMatrixBuffer;
    sceneInputs.geometry.staticVertexCount = staticVertexCacheCount;
    sceneInputs.geometry.staticIndexCount = staticIndexCacheCount;
    sceneInputs.geometry.staticTriangleCount = staticTriangleCacheCount;
    sceneInputs.geometry.staticMaterialIndexCount = static_cast<int>(materialTable.staticMaterialIndexes.size());
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
    sceneInputs.geometry.rigidRouteTransformContinuousCount = rigidRouteBuild.stats.transformContinuousInstances;
    sceneInputs.geometry.skinnedSurfaceCount = classStats.skinnedDeformedSurfaces;
    sceneInputs.geometry.skinnedTriangleCount = classStats.skinnedDeformedTriangles;
    sceneInputs.geometry.skinnedRtCpuSurfaceCount = m_smokeSkinnedPreviousStats.currentRtCpuSkinnedSurfaceCount;
    sceneInputs.geometry.skinnedPreviousMatchedSurfaceCount = m_smokeSkinnedPreviousStats.previousMatchedSurfaceCount;
    sceneInputs.geometry.skinnedPreviousInvalidSurfaceCount = m_smokeSkinnedPreviousStats.previousInvalidSurfaceCount;
    sceneInputs.geometry.skinnedPreviousRetainedVertexCount = m_smokeSkinnedPreviousStats.previousRetainedVertexCount;
    sceneInputs.geometry.skinnedPreviousNoFrameCount = m_smokeSkinnedPreviousStats.noPreviousFrameCount;
    sceneInputs.geometry.skinnedPreviousNoSurfaceCount = m_smokeSkinnedPreviousStats.noPreviousSurfaceCount;
    sceneInputs.geometry.skinnedPreviousCountMismatchCount =
        m_smokeSkinnedPreviousStats.vertexCountMismatchCount +
        m_smokeSkinnedPreviousStats.indexCountMismatchCount +
        m_smokeSkinnedPreviousStats.triangleCountMismatchCount;
    sceneInputs.geometry.skinnedPreviousMaterialChangedCount = m_smokeSkinnedPreviousStats.materialChangedCount;
    sceneInputs.geometry.skinnedPreviousSurfaceClassChangedCount = m_smokeSkinnedPreviousStats.surfaceClassChangedCount;
    sceneInputs.geometry.skinnedPreviousNotRtCpuSkinnedCount = m_smokeSkinnedPreviousStats.notRtCpuSkinnedCount;
    sceneInputs.geometry.skinnedPreviousSkeletonChangedCount = m_smokeSkinnedPreviousStats.skeletonChangedCount;
    sceneInputs.geometry.skinnedPreviousTransformDiscontinuityCount = m_smokeSkinnedPreviousStats.transformDiscontinuityCount;
    sceneInputs.geometry.skinnedPreviousBufferUnavailableCount = m_smokeSkinnedPreviousStats.previousBufferUnavailableCount;
    sceneInputs.geometry.skinnedTemporalTopologyStableCount = m_smokeSkinnedPreviousStats.topologyStableCount;
    sceneInputs.geometry.skinnedTemporalLodStableCount = m_smokeSkinnedPreviousStats.lodStableCount;
    sceneInputs.geometry.skinnedTemporalTransformContinuousCount = m_smokeSkinnedPreviousStats.transformContinuousCount;
    sceneInputs.geometry.skinnedTemporalDeformationContinuousCount = m_smokeSkinnedPreviousStats.deformationContinuousCount;
    sceneInputs.geometry.skinnedTemporalMaterialStableCount = m_smokeSkinnedPreviousStats.materialStableCount;
    sceneInputs.geometry.skinnedTemporalPreviousBufferValidCount = m_smokeSkinnedPreviousStats.previousBufferValidCount;
    sceneInputs.geometry.skinnedGpuSkinningMode = gpuSkinningMode;
    sceneInputs.geometry.skinnedSourceVertexCount = static_cast<int>(skinnedGpuScaffold.sourceVertices.size());
    sceneInputs.geometry.skinnedCurrentOutputVertexCount = static_cast<int>(skinnedGpuScaffold.currentOutputVertices.size());
    sceneInputs.geometry.skinnedPreviousPositionCount = static_cast<int>(skinnedGpuScaffold.previousPositions.size());
    sceneInputs.geometry.skinnedSurfaceDispatchCount = static_cast<int>(skinnedGpuScaffold.dispatchRecords.size());
    sceneInputs.geometry.skinnedTriangleDispatchIndexCount = static_cast<int>(skinnedGpuScaffold.dynamicTriangleDispatchIndexes.size());
    sceneInputs.geometry.skinnedTriangleDispatchMappedCount = skinnedGpuScaffold.mappedDynamicTriangles;
    for (const PathTraceSkinnedSurfaceDispatchRecord& dispatch : skinnedGpuScaffold.dispatchRecords)
    {
        if ((dispatch.flags & PT_SKINNED_DISPATCH_HAS_VALID_PREVIOUS) == 0u || dispatch.previousPositionOffset == UINT32_MAX)
        {
            continue;
        }

        const uint64 previousEnd = static_cast<uint64>(dispatch.previousPositionOffset) + static_cast<uint64>(dispatch.vertexCount);
        sceneInputs.geometry.skinnedPreviousDispatchMaxEnd = Max(sceneInputs.geometry.skinnedPreviousDispatchMaxEnd, static_cast<int>(Min<uint64>(previousEnd, static_cast<uint64>(INT_MAX))));
        if (previousEnd <= static_cast<uint64>(skinnedGpuScaffold.previousPositions.size()))
        {
            ++sceneInputs.geometry.skinnedPreviousDispatchValidCount;
        }
        else
        {
            ++sceneInputs.geometry.skinnedPreviousDispatchOutOfRangeCount;
        }
    }
    sceneInputs.geometry.skinnedCurrentJointMatrixCount = static_cast<int>(skinnedGpuScaffold.currentJointMatrices.size());
    sceneInputs.geometry.skinnedPreviousJointMatrixCount = static_cast<int>(skinnedGpuScaffold.previousJointMatrices.size());
    sceneInputs.geometry.skinnedGpuComputeDispatchCount = skinnedGpuComputeReady ? static_cast<int>(skinnedGpuScaffold.dispatchRecords.size()) : 0;
    sceneInputs.geometry.skinnedGpuComputeVertexCount = skinnedGpuComputeReady ? skinnedGpuComputeVertexCount : 0;
    sceneInputs.geometry.skinnedGpuComputeMaxVertexCount = skinnedGpuComputeReady ? skinnedGpuComputeMaxVertexCount : 0;
    sceneInputs.geometry.currentGeometryValid = hasStaticBlas || hasDynamicBlas;
    sceneInputs.geometry.previousTransformAvailable = rigidRouteBuild.stats.previousTransformInstances > 0;
    sceneInputs.geometry.skinnedPreviousCpuVertexDataRetained = m_smokeSkinnedPreviousStats.previousRetainedVertexCount > 0;
    sceneInputs.geometry.skinnedSourceGeometryAvailable =
        smokeSkinnedSourceVertexBuffer &&
        smokeSkinnedCurrentOutputVertexBuffer &&
        smokeSkinnedSurfaceDispatchBuffer &&
        smokeSkinnedTriangleDispatchIndexBuffer &&
        !skinnedGpuScaffold.sourceVertices.empty() &&
        !skinnedGpuScaffold.currentOutputVertices.empty() &&
        !skinnedGpuScaffold.dispatchRecords.empty() &&
        !skinnedGpuScaffold.dynamicTriangleDispatchIndexes.empty();
    sceneInputs.geometry.skinnedPreviousPositionBufferAvailable =
        smokeSkinnedPreviousPositionBuffer &&
        !skinnedGpuScaffold.previousPositions.empty();
    sceneInputs.geometry.skinnedGpuComputePipelineAvailable = gpuSkinningMode >= 1 && m_smokeSkinnedGpuSkinningPipeline != nullptr;
    sceneInputs.geometry.skinnedGpuComputeDispatched = skinnedGpuComputeDispatched;
    sceneInputs.geometry.skinnedGpuComputeTargetsDynamicVertexBuffer = skinnedGpuComputeDispatched && skinnedGpuComputeTargetsDynamicVertices;
    sceneInputs.geometry.skinnedGpuComputeWritesPreviousPositions = skinnedGpuComputeDispatched && skinnedGpuComputeWritesPreviousPositions;
    sceneInputs.geometry.skinnedGpuSkinningAvailable = skinnedGpuComputeDispatched;
    if (r_pathTracingSkinnedDump.GetInteger() != 0)
    {
        common->Printf("PathTracePrimaryPass: PT skinned dump frame=%llu source=%d motionExport=%d rrGuideHistory=%d rrDebug=%d gpuSkinning=%d scaffoldMode=%d computeInputs=%d current records=%d surfaces/tris=%d/%d rtCpu=%d dynamicTris total/skinnedCpu/basePose/rtCpu=%d/%d/%d/%d previousBefore records/verts/joints=%d/%d/%d bridge matched/invalid/retainedVerts=%d/%d/%d invalid noFrame/noSurface/count/material/class/notRtCpu/skeleton/teleport/prevBuf=%d/%d/%d/%d/%d/%d/%d/%d/%d temporal topology/lod/transform/deform/material/prevBuf=%d/%d/%d/%d/%d/%d scaffold source/current/prevPos/dispatch/mapped/index/currentJoints/prevJoints=%d/%d/%d/%d/%d/%d/%d/%d compute ready/dispatched/targetDyn/writePrev/verts/max=%d/%d/%d/%d/%d/%d bytes source/current/prevPos/dispatch/index/currentJoints/prevJoints/upload/skip=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu capacity source/current/prevPos/dispatch/index/currentJoints/prevJoints=%llu/%llu/%llu/%llu/%llu/%llu/%llu dynamicCapacity v/i/class/mat/matIndex=%llu/%llu/%llu/%llu/%llu timing captureClass/append/rtCpuAppend/bucket/bridge/scaffold/dispatchIndex/retainJoints/bufferCreate/upload=%d/%d/%d/%d/%d/%d/%d/%d/%d/%d\n",
            static_cast<unsigned long long>(m_smokeGeometryFrameIndex),
            sceneSource,
            r_pathTracingMotionVectorExport.GetInteger() != 0 ? 1 : 0,
            rrGuideNeedsSkinnedHistory ? 1 : 0,
            r_pathTracingDLSSRRGuideDebugView.GetInteger() != 0 ? 1 : 0,
            gpuSkinningMode,
            skinnedScaffoldMode,
            buildSkinnedGpuSkinningInputs ? 1 : 0,
            static_cast<int>(currentSkinnedSurfaceRecords.size()),
            sceneInputs.geometry.skinnedSurfaceCount,
            sceneInputs.geometry.skinnedTriangleCount,
            sceneInputs.geometry.skinnedRtCpuSurfaceCount,
            sceneInputs.geometry.dynamicTriangleCount,
            sceneInputs.geometry.dynamicSkinnedCpuCurrentTriangleCount,
            sceneInputs.geometry.dynamicSkinnedLikelyBasePoseTriangleCount,
            sceneInputs.geometry.dynamicSkinnedRtCpuTriangleCount,
            previousSkinnedSurfaceRecordCountBeforeBridge,
            previousSkinnedVertexDataCountBeforeBridge,
            previousSkinnedJointMatrixCountBeforeBridge,
            sceneInputs.geometry.skinnedPreviousMatchedSurfaceCount,
            sceneInputs.geometry.skinnedPreviousInvalidSurfaceCount,
            sceneInputs.geometry.skinnedPreviousRetainedVertexCount,
            sceneInputs.geometry.skinnedPreviousNoFrameCount,
            sceneInputs.geometry.skinnedPreviousNoSurfaceCount,
            sceneInputs.geometry.skinnedPreviousCountMismatchCount,
            sceneInputs.geometry.skinnedPreviousMaterialChangedCount,
            sceneInputs.geometry.skinnedPreviousSurfaceClassChangedCount,
            sceneInputs.geometry.skinnedPreviousNotRtCpuSkinnedCount,
            sceneInputs.geometry.skinnedPreviousSkeletonChangedCount,
            sceneInputs.geometry.skinnedPreviousTransformDiscontinuityCount,
            sceneInputs.geometry.skinnedPreviousBufferUnavailableCount,
            sceneInputs.geometry.skinnedTemporalTopologyStableCount,
            sceneInputs.geometry.skinnedTemporalLodStableCount,
            sceneInputs.geometry.skinnedTemporalTransformContinuousCount,
            sceneInputs.geometry.skinnedTemporalDeformationContinuousCount,
            sceneInputs.geometry.skinnedTemporalMaterialStableCount,
            sceneInputs.geometry.skinnedTemporalPreviousBufferValidCount,
            sceneInputs.geometry.skinnedSourceVertexCount,
            sceneInputs.geometry.skinnedCurrentOutputVertexCount,
            sceneInputs.geometry.skinnedPreviousPositionCount,
            sceneInputs.geometry.skinnedSurfaceDispatchCount,
            sceneInputs.geometry.skinnedTriangleDispatchMappedCount,
            sceneInputs.geometry.skinnedTriangleDispatchIndexCount,
            sceneInputs.geometry.skinnedCurrentJointMatrixCount,
            sceneInputs.geometry.skinnedPreviousJointMatrixCount,
            skinnedGpuComputeReady ? 1 : 0,
            skinnedGpuComputeDispatched ? 1 : 0,
            skinnedGpuComputeTargetsDynamicVertices ? 1 : 0,
            skinnedGpuComputeWritesPreviousPositions ? 1 : 0,
            skinnedGpuComputeVertexCount,
            skinnedGpuComputeMaxVertexCount,
            static_cast<unsigned long long>(bufferCreateDesc.skinnedSourceVertexBytes),
            static_cast<unsigned long long>(bufferCreateDesc.skinnedCurrentOutputVertexBytes),
            static_cast<unsigned long long>(bufferCreateDesc.skinnedPreviousPositionBytes),
            static_cast<unsigned long long>(bufferCreateDesc.skinnedSurfaceDispatchBytes),
            static_cast<unsigned long long>(bufferCreateDesc.skinnedTriangleDispatchIndexBytes),
            static_cast<unsigned long long>(bufferCreateDesc.skinnedCurrentJointMatrixBytes),
            static_cast<unsigned long long>(bufferCreateDesc.skinnedPreviousJointMatrixBytes),
            static_cast<unsigned long long>(skinnedUploadBytes),
            static_cast<unsigned long long>(skinnedSkippedUploadBytes),
            static_cast<unsigned long long>(SmokeBufferCapacityElements(smokeSkinnedSourceVertexBuffer, sizeof(PathTraceSkinnedSourceVertex))),
            static_cast<unsigned long long>(SmokeBufferCapacityElements(smokeSkinnedCurrentOutputVertexBuffer, sizeof(PathTraceSmokeVertex))),
            static_cast<unsigned long long>(SmokeBufferCapacityElements(smokeSkinnedPreviousPositionBuffer, sizeof(PathTraceSkinnedPreviousPosition))),
            static_cast<unsigned long long>(SmokeBufferCapacityElements(smokeSkinnedSurfaceDispatchBuffer, sizeof(PathTraceSkinnedSurfaceDispatchRecord))),
            static_cast<unsigned long long>(SmokeBufferCapacityElements(smokeSkinnedTriangleDispatchIndexBuffer, sizeof(uint32_t))),
            static_cast<unsigned long long>(SmokeBufferCapacityElements(smokeSkinnedCurrentJointMatrixBuffer, sizeof(PathTraceSkinnedJointMatrix))),
            static_cast<unsigned long long>(SmokeBufferCapacityElements(smokeSkinnedPreviousJointMatrixBuffer, sizeof(PathTraceSkinnedJointMatrix))),
            static_cast<unsigned long long>(SmokeBufferCapacityElements(smokeDynamicVertexBuffer, sizeof(PathTraceSmokeVertex))),
            static_cast<unsigned long long>(SmokeBufferCapacityElements(smokeDynamicIndexBuffer, sizeof(uint32_t))),
            static_cast<unsigned long long>(SmokeBufferCapacityElements(smokeDynamicTriangleClassBuffer, sizeof(uint32_t))),
            static_cast<unsigned long long>(SmokeBufferCapacityElements(smokeDynamicTriangleMaterialBuffer, sizeof(uint32_t))),
            static_cast<unsigned long long>(SmokeBufferCapacityElements(smokeDynamicTriangleMaterialIndexBuffer, sizeof(uint32_t))),
            captureTiming.dynamicPassClassifyMs,
            captureTiming.dynamicAppendMs,
            captureTiming.rtCpuSkinningAppendMs,
            captureTiming.bucketMergeMs,
            skinnedPreviousBridgeMs,
            skinnedGpuScaffoldMs,
            skinnedTriangleDispatchIndexMs,
            skinnedRetainJointsMs,
            bufferCreateMs,
            bufferUploadMs);
        r_pathTracingSkinnedDump.SetInteger(0);
    }
    sceneInputs.geometry.capabilityFlags =
        RT_SCENE_INPUT_GEOMETRY_PREVIOUS_TRANSFORM_RESERVED |
        RT_SCENE_INPUT_GEOMETRY_PREVIOUS_VERTEX_RESERVED |
        RT_SCENE_INPUT_SKINNED_SOURCE_GEOMETRY_RESERVED |
        RT_SCENE_INPUT_SKINNED_GPU_SKINNING_RESERVED;

    sceneInputs.materials.materialTableBuffer = smokeMaterialTableBuffer;
    sceneInputs.materials.dynamicMaterialBuffer = smokeDynamicMaterialBuffer;
    sceneInputs.materials.textureDescriptorTable = bindingBuildResult.textureDescriptorTable;
    sceneInputs.materials.materialTableEntryCount = static_cast<int>(materialTable.materials.size());
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
    sceneInputs.lights.lightUniverseGeneration = lightUniverseStats.generation;
    sceneInputs.lights.capabilityFlags = RT_SCENE_INPUT_LIGHT_PREVIOUS_IDENTITY_RESERVED;
    if (r_pathTracingEmissiveBridgeDump.GetInteger() != 0)
    {
        common->Printf("PathTracePrimaryPass: RT smoke emissive bridge producerPolicy pdfNeeStaticEmissive=%d restirPT=%d enableTextureProbe=%d staticAreaPreloadCvar=%d portalFullMapCvar=%d fullWorldStaticEmissivesCvar=%d fullWorldAppended=%d rigidRouteEnabled=%d routedRigidAppended=%d captured=%d static=%d dynamic=%d distribution=%d unifiedCurrent=%d managerCurrentPayload=%d lightUniverseGeneration=%llu behavior=current-frame-producer-diagnostics-only\n",
            pdfNeeVerifierStaticEmissiveProducerPolicy ? 1 : 0,
            restirPTDebugMode ? 1 : 0,
            enableTextureProbe ? 1 : 0,
            r_pathTracingStaticAreaPreload.GetInteger() != 0 ? 1 : 0,
            r_pathTracingPortalBruteforceFullMap.GetInteger() != 0 ? 1 : 0,
            r_pathTracingWorldStaticEmissives.GetInteger() != 0 ? 1 : 0,
            emissiveInventoryStats.fullLevelStaticTriangles,
            enableRigidRouteForMode ? 1 : 0,
            emissiveInventoryStats.routedRigidTriangles,
            emissiveInventoryStats.capturedTriangles,
            emissiveInventoryStats.staticTriangles,
            emissiveInventoryStats.dynamicTriangles,
            static_cast<int>(emissiveDistribution.entries.size()),
            static_cast<int>(unifiedLights.currentLights.size()),
            static_cast<int>(restirLightManagerCurrentPayloadRecords.size()),
            static_cast<unsigned long long>(lightUniverseStats.generation));
        r_pathTracingEmissiveBridgeDump.SetInteger(0);
    }

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
    resourceCommitBuildDesc.dynamicBlasDesc = smokeDynamicBlasDesc;
    resourceCommitBuildDesc.staticBlas = smokeStaticBlas;
    resourceCommitBuildDesc.dynamicBlas = smokeDynamicBlas;
    resourceCommitBuildDesc.tlas = m_smokeTlas;
    resourceCommitBuildDesc.hasStaticBlas = hasStaticBlas;
    resourceCommitBuildDesc.staticBlasSignature = staticSignature.hash;
    resourceCommitBuildDesc.bindingSet = bindingBuildResult.bindingSet;
    resourceCommitBuildDesc.textureDescriptorTable = bindingBuildResult.textureDescriptorTable;
    resourceCommitBuildDesc.activeTextureTable = &bindingBuildResult.activeTextureTable;
    resourceCommitBuildDesc.textureDescriptorTableCreated = bindingBuildResult.textureDescriptorTableCreated;
    resourceCommitBuildDesc.textureDescriptorTableWritten = bindingBuildResult.textureDescriptorTableWritten;
    resourceCommitBuildDesc.materialTableEntryCount = static_cast<int>(materialTable.materials.size());
    resourceCommitBuildDesc.emissiveTriangleCount = emissiveInventoryStats.capturedTriangles;
    resourceCommitBuildDesc.emissiveStaticTriangleCount = emissiveInventoryStats.staticTriangles;
    resourceCommitBuildDesc.emissiveDynamicTriangleCount = emissiveInventoryStats.dynamicTriangles;
    resourceCommitBuildDesc.lightCandidateCount = emissiveInventoryStats.candidateMaterials;
    resourceCommitBuildDesc.texturedLightCandidateCount = emissiveInventoryStats.texturedCandidateMaterials;
    resourceCommitBuildDesc.lightCandidateBytes = static_cast<int>(lightCandidates.size() * sizeof(lightCandidates[0]));
    resourceCommitBuildDesc.doomAnalyticLightCount = static_cast<int>(doomAnalyticLights.size());
    resourceCommitBuildDesc.doomAnalyticPortalRegionLightCount = doomAnalyticPortalRegionLightCount;
    resourceCommitBuildDesc.doomAnalyticLightBytes = static_cast<int>(doomAnalyticLights.size() * sizeof(PathTraceDoomAnalyticLightCandidate));
    resourceCommitBuildDesc.doomAnalyticPreviousLightCount = static_cast<int>(doomAnalyticRemap.previousCandidates.size());
    resourceCommitBuildDesc.doomAnalyticCurrentIdentityCount = static_cast<int>(doomAnalyticRemap.currentCandidateIdentities.size());
    resourceCommitBuildDesc.doomAnalyticPreviousIdentityCount = static_cast<int>(doomAnalyticRemap.previousCandidateIdentities.size());
    resourceCommitBuildDesc.doomAnalyticRemapCount = static_cast<int>(doomAnalyticRemap.universeRemap.size());
    resourceCommitBuildDesc.doomAnalyticInvalidRemapCount = doomAnalyticRemap.invalidRemapCount;
    resourceCommitBuildDesc.previousEmissiveTriangleCount = sceneInputs.lights.previousEmissiveTriangleCount;
    resourceCommitBuildDesc.unifiedLightCount = sceneInputs.lights.unifiedLightCount;
    resourceCommitBuildDesc.unifiedPreviousLightCount = sceneInputs.lights.unifiedPreviousLightCount;
    resourceCommitBuildDesc.unifiedLightRemapCount = sceneInputs.lights.unifiedLightRemapCount;
    resourceCommitBuildDesc.restirLightManagerCurrentPayloadCount = sceneInputs.lights.restirLightManagerCurrentPayloadCount;
    resourceCommitBuildDesc.restirLightManagerPreviousPayloadCount = sceneInputs.lights.restirLightManagerPreviousPayloadCount;
    resourceCommitBuildDesc.reservoirSceneSignature = reservoirSceneSignature;
    RtSmokeSceneResourceCommitDesc resourceCommitDesc;
    {
        OPTICK_EVENT("PT Commit Scene Desc Build");
        resourceCommitDesc = CreateSmokeSceneResourceCommitDesc(resourceCommitBuildDesc);
    }
    {
        OPTICK_EVENT("PT Commit Scene Resources");
        CommitRayTracingSmokeSceneResources(resourceCommitDesc);
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
    sceneLogDesc.dumpClassReasons = dumpClassReasons;
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
    sceneLogDesc.materialUniverseTableCompareStats = &materialUniverseTableCompareStats;
    sceneLogDesc.textureCoverageStats = &textureCoverageStats;
    sceneLogDesc.reasonSamples = &reasonSamples;
    sceneLogDesc.lastSceneTimingLogMs = &g_smokeLastSceneTimingLogMs;
    sceneLogDesc.sceneRebuildLogged = &m_smokeSceneRebuildLogged;
    sceneLogDesc.sceneLogCooldownFrames = &m_smokeSceneLogCooldownFrames;
    {
        OPTICK_EVENT("PT Scene Diagnostic Logs");
        RunSmokeSceneBuildDiagnosticLogs(sceneLogDesc);
    }
}
