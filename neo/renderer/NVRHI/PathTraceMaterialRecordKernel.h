#pragma once
// Owned CPU material-record preparation. No renderer pointers, handles or globals.
#include "PathTraceDynamicMaterialRecord.h"
#include "PathTraceRuntimeMaterialEvalKernel.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <vector>

struct RtCpuMaterialPoint { float x=0, y=0, z=0; };
struct RtCpuMaterialColor {
    float x,y,z,w;
    RtCpuMaterialColor(float a,float b,float c,float d):x(a),y(b),z(c),w(d) {}
};
struct RtCpuMaterialRecordStage
{
    int stageIndex = -1;
    bool enabled = false;
    bool emissive = false;
    bool hasAlphaTest = false;
    bool hasTexMatrix = false;
    float condition = 1.0f;
    float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float alphaTest = 0.0f;
    float texMatrix[2][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
};

struct RtCpuMaterialRecordSample
{
    bool valid = false;
    uint32_t id = 0;
    int surfaces = 0;
    int triangles = 0;
    int stageIndex = -1;
    int enabledStages = 0;
    int disabledStages = 0;
    int colorStages = 0;
    int alphaStages = 0;
    int alphaTestStages = 0;
    int texMatrixStages = 0;
    int dynamicImageStages = 0;
    int cinematicStages = 0;
    int guiRenderTargetStages = 0;
    int programStages = 0;
    int stagePriority = -1;
    bool selectedStageEmissive = false;
    float condition = 1.0f;
    float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float alphaTest = 0.0f;
    float texMatrix[2][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    // Detail-decal channels: the DIFFUSE stage's evaluated color (generic stage
    // selection can pick a white bump stage over the authored tint), and a
    // representative world position for spectrum light association.
    bool hasDiffuseStageColor = false;
    float diffuseStageColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float diffuseStageCondition = 1.0f;
    RtCpuMaterialRecordStage orderedStages[8];
    int orderedStageCount = 0;
    bool orderedStageOverflow = false;
    bool hasSurfaceOrigin = false;
    RtCpuMaterialPoint surfaceOrigin;
};


struct RtCpuMaterialRecordRow {
    uint32_t id=0;
    bool residentStatic=false, variant=false, detailDecal=false;
    int detailDecalSpectrum=0;
};
struct RtCpuMaterialSpectrumLight {
    int spectrum=0; RtCpuMaterialPoint origin;
    float radius=300.0f;
    RtCpuMaterialColor color {0,0,0,1};
};
struct RtCpuMaterialRecordsInput {
    std::vector<RtCpuMaterialRecordRow> rows;
    std::vector<RtCpuMaterialRecordSample> samples;
    std::vector<RtCpuMaterialSpectrumLight> spectrumLights;
};
inline float RtCpuMaterialClamp(float lo,float hi,float value) { return value < lo ? lo : value > hi ? hi : value; }
inline bool RtCpuMaterialHasMatrix(const RtCpuMaterialRecordSample& sample)
{
    return sample.texMatrixStages > 0 ||
        std::fabs(sample.texMatrix[0][0]-1)>1e-6f || std::fabs(sample.texMatrix[0][1])>1e-6f || std::fabs(sample.texMatrix[0][2])>1e-6f ||
        std::fabs(sample.texMatrix[1][0])>1e-6f || std::fabs(sample.texMatrix[1][1]-1)>1e-6f || std::fabs(sample.texMatrix[1][2])>1e-6f;
}
inline bool RtCpuChooseSpectrum(const std::vector<RtCpuMaterialSpectrumLight>& lights, int spectrum,
    const RtCpuMaterialPoint* origin, RtCpuMaterialColor& color)
{
    bool found=false; float bestScore=0, bestLuminance=-1;
    for (const auto& light:lights) {
        if (light.spectrum!=spectrum) continue;
        const float luminance=std::max(light.color.x,std::max(light.color.y,light.color.z));
        if (origin) {
            const float x=light.origin.x-origin->x, y=light.origin.y-origin->y, z=light.origin.z-origin->z;
            const float score=std::sqrt(x*x+y*y+z*z)/light.radius;
            if (!found || score<bestScore) { found=true;bestScore=score;color=light.color; }
        } else if (!found || luminance>bestLuminance) { found=true;bestLuminance=luminance;color=light.color; }
    }
    return found;
}

template<class Sample>
inline RtCpuMaterialRecordSample RtCpuCaptureMaterialRecordSample(const Sample& src)
{
    RtCpuMaterialRecordSample out;
    out.valid=src.valid;
    out.id=src.id;
    out.surfaces=src.surfaces;
    out.triangles=src.triangles;
    out.stageIndex=src.stageIndex;
    out.enabledStages=src.enabledStages;
    out.disabledStages=src.disabledStages;
    out.colorStages=src.colorStages;
    out.alphaStages=src.alphaStages;
    out.alphaTestStages=src.alphaTestStages;
    out.texMatrixStages=src.texMatrixStages;
    out.dynamicImageStages=src.dynamicImageStages;
    out.cinematicStages=src.cinematicStages;
    out.guiRenderTargetStages=src.guiRenderTargetStages;
    out.programStages=src.programStages;
    out.stagePriority=src.stagePriority;
    out.selectedStageEmissive=src.selectedStageEmissive;
    out.condition=src.condition;
    out.alphaTest=src.alphaTest;
    out.hasDiffuseStageColor=src.hasDiffuseStageColor;
    out.diffuseStageCondition=src.diffuseStageCondition;
    out.orderedStageCount=src.orderedStageCount;
    out.orderedStageOverflow=src.orderedStageOverflow;
    out.hasSurfaceOrigin=src.hasSurfaceOrigin;
    if (src.orderedStageCount<0 || src.orderedStageCount>8) throw std::invalid_argument("material stage span");
    std::memcpy(out.color,src.color,sizeof(out.color));
    std::memcpy(out.texMatrix,src.texMatrix,sizeof(out.texMatrix));
    std::memcpy(out.diffuseStageColor,src.diffuseStageColor,sizeof(out.diffuseStageColor));
    out.surfaceOrigin={src.surfaceOrigin.x,src.surfaceOrigin.y,src.surfaceOrigin.z};
    for (int i=0;i<src.orderedStageCount;++i) {
        out.orderedStages[i].stageIndex=src.orderedStages[i].stageIndex;
        out.orderedStages[i].enabled=src.orderedStages[i].enabled;
        out.orderedStages[i].emissive=src.orderedStages[i].emissive;
        out.orderedStages[i].hasAlphaTest=src.orderedStages[i].hasAlphaTest;
        out.orderedStages[i].hasTexMatrix=src.orderedStages[i].hasTexMatrix;
        out.orderedStages[i].condition=src.orderedStages[i].condition;
        out.orderedStages[i].alphaTest=src.orderedStages[i].alphaTest;
        std::memcpy(out.orderedStages[i].color,src.orderedStages[i].color,sizeof(out.orderedStages[i].color));
        std::memcpy(out.orderedStages[i].texMatrix,src.orderedStages[i].texMatrix,sizeof(out.orderedStages[i].texMatrix));
    }
    return out;
}

// The authoritative frame sample keeps every numeric field of the legacy owner
// adapter. Resource selection is represented separately by a frame-local ordinal.
inline RtCpuMaterialRecordSample RtCpuMaterialRecordSampleFromEvaluation(
    const RtPathTraceRuntimeMaterialEvalPod& evaluated, uint32_t materialId)
{
    static_assert(RT_PT_RUNTIME_ORDERED_STAGE_CAPACITY == 8,"record stage capacity");
    if (evaluated.orderedStageCount > 8) throw std::invalid_argument("material stage span");
    RtCpuMaterialRecordSample surfaceSample;
    surfaceSample.valid =
        evaluated.result == RtPathTraceRuntimeEvalBuildResult::Built;
    surfaceSample.id = materialId;
    surfaceSample.stageIndex = evaluated.selectedStageIndex;
    surfaceSample.stagePriority = evaluated.selectedStagePriority;
    surfaceSample.enabledStages = evaluated.enabledStages;
    surfaceSample.disabledStages = evaluated.disabledStages;
    surfaceSample.colorStages = evaluated.colorStages;
    surfaceSample.alphaStages = evaluated.alphaStages;
    surfaceSample.alphaTestStages = evaluated.alphaTestStages;
    surfaceSample.texMatrixStages = evaluated.texMatrixStages;
    surfaceSample.dynamicImageStages = evaluated.dynamicImageStages;
    surfaceSample.cinematicStages = evaluated.cinematicStages;
    surfaceSample.guiRenderTargetStages = evaluated.guiRenderTargetStages;
    surfaceSample.programStages = evaluated.programStages;
    surfaceSample.selectedStageEmissive = evaluated.selectedStageEmissive;
    surfaceSample.condition = evaluated.condition;
    surfaceSample.alphaTest = evaluated.alphaTest;
    std::memcpy(surfaceSample.color, evaluated.color, sizeof(surfaceSample.color));
    std::memcpy(surfaceSample.texMatrix, evaluated.texMatrix,
        sizeof(surfaceSample.texMatrix));
    surfaceSample.hasDiffuseStageColor = evaluated.hasDiffuseStageColor;
    std::memcpy(surfaceSample.diffuseStageColor,
        evaluated.diffuseStageColor, sizeof(surfaceSample.diffuseStageColor));
    surfaceSample.diffuseStageCondition = evaluated.diffuseStageCondition;
    surfaceSample.orderedStageCount = static_cast<int>(evaluated.orderedStageCount);
    surfaceSample.orderedStageOverflow = evaluated.orderedStageOverflow;
    for (std::uint32_t index = 0; index < evaluated.orderedStageCount; ++index)
    {
        const RtPathTraceRuntimeStageEvalPod& source =
            evaluated.orderedStages[index];
        RtCpuMaterialRecordStage& destination = surfaceSample.orderedStages[index];
        destination.stageIndex = source.stageIndex;
        destination.enabled = source.enabled;
        destination.emissive = source.emissive;
        destination.hasAlphaTest = source.hasAlphaTest;
        destination.hasTexMatrix = source.hasTexMatrix;
        destination.condition = source.condition;
        destination.alphaTest = source.alphaTest;
        std::memcpy(destination.color, source.color, sizeof(destination.color));
        std::memcpy(destination.texMatrix, source.texMatrix,
            sizeof(destination.texMatrix));
    }
    surfaceSample.hasSurfaceOrigin = evaluated.hasSurfaceOrigin;
    surfaceSample.surfaceOrigin={evaluated.surfaceOrigin[0],
        evaluated.surfaceOrigin[1], evaluated.surfaceOrigin[2]};
    return surfaceSample;
}

inline std::vector<PathTraceDynamicMaterialRecord> BuildRtCpuMaterialRecords(const RtCpuMaterialRecordsInput& input)
{
    const int materialCount=static_cast<int>(input.rows.size());
    if (!materialCount) return {};
    for (const auto& sample:input.samples)
        if (sample.orderedStageCount<0 || sample.orderedStageCount>8) throw std::invalid_argument("material stage span");
    std::vector<PathTraceDynamicMaterialRecord> records(materialCount);
    std::vector<std::vector<PathTraceDynamicMaterialRecord>> orderedStageRecords(materialCount);
    int validRecordCount=0;
    const auto& dynamicSamples=input.samples;
    const auto& spectrumLights=input.spectrumLights;
    std::unordered_map<uint32_t,int> byId;
    byId.reserve(input.rows.size());
    for (int i=0;i<materialCount;++i) byId.emplace(input.rows[i].id,i);
    const auto findRow=[&](uint32_t id) { auto it=byId.find(id);return it==byId.end() ? -1 : it->second; };
    for (const RtCpuMaterialRecordSample& sample : dynamicSamples)
    {
        if (!sample.valid)
        {
            continue;
        }

        const int materialIndex = findRow(sample.id);
        if (materialIndex < 0)
        {
            continue;
        }
        if (input.rows[materialIndex].residentStatic)
        {
            continue;
        }

        PathTraceDynamicMaterialRecord record;
        record.color[0] = std::max(0.0f, sample.color[0]);
        record.color[1] = std::max(0.0f, sample.color[1]);
        record.color[2] = std::max(0.0f, sample.color[2]);
        record.color[3] = RtCpuMaterialClamp(0.0f, 1.0f, sample.color[3]);
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
        if (input.rows[materialIndex].variant)
        {
            record.flags |= RT_SMOKE_DYNAMIC_MATERIAL_RECORD_REPLACE_EMISSIVE;
        }
        if (RtCpuMaterialHasMatrix(sample))
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
        if (materialIndex < static_cast<int>(input.rows.size()))
        {
            const RtCpuMaterialRecordRow& info = input.rows[materialIndex];
            if (info.detailDecal && sample.hasDiffuseStageColor)
            {
                record.color[0] = std::max(0.0f, sample.diffuseStageColor[0]);
                record.color[1] = std::max(0.0f, sample.diffuseStageColor[1]);
                record.color[2] = std::max(0.0f, sample.diffuseStageColor[2]);
                record.color[3] = RtCpuMaterialClamp(0.0f, 1.0f, sample.diffuseStageColor[3]);
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
                RtCpuMaterialColor lightColor(0.0f, 0.0f, 0.0f, 1.0f);
                RtCpuChooseSpectrum(
                    spectrumLights,
                    info.detailDecalSpectrum,
                    sample.hasSurfaceOrigin ? &sample.surfaceOrigin : nullptr,
                    lightColor);
                record.color[0] *= lightColor.x;
                record.color[1] *= lightColor.y;
                record.color[2] *= lightColor.z;
                record.flags |= RT_SMOKE_DYNAMIC_MATERIAL_RECORD_SELECTED_EMISSIVE;
                if (std::max(record.color[0], std::max(record.color[1], record.color[2])) <= 0.0f)
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
            const RtCpuMaterialRecordStage& stage = sample.orderedStages[orderedIndex];
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
    const int infoCount = std::min(materialCount, static_cast<int>(input.rows.size()));
    for (int materialIndex = 0; materialIndex < infoCount; ++materialIndex)
    {
        const RtCpuMaterialRecordRow& info = input.rows[materialIndex];
        if (input.rows[materialIndex].residentStatic)
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

        RtCpuMaterialColor lightColor(0.0f, 0.0f, 0.0f, 1.0f);
        RtCpuChooseSpectrum(spectrumLights, info.detailDecalSpectrum, nullptr, lightColor);
        const bool lit = lightColor.x > 0.0f || lightColor.y > 0.0f || lightColor.z > 0.0f;

        PathTraceDynamicMaterialRecord record;
        record.color[0] = lightColor.x;
        record.color[1] = lightColor.y;
        record.color[2] = lightColor.z;
        record.color[3] = 1.0f;
        record.texMatrix0[3] = lit ? 1.0f : 0.0f;
        record.materialIndex = static_cast<uint32_t>(materialIndex);
        record.materialId = input.rows[materialIndex].id;
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
        const uint32_t selectedStage = selectedStageRaw == 0xffu ? 0xffu : std::min(selectedStageRaw, 0xfeu);
        const uint32_t stageCount = std::min(static_cast<uint32_t>(materialStageRecords.size()), 0xfu);
        const uint32_t stageOffset = std::min(static_cast<uint32_t>(records.size()), 0xffffu);
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
