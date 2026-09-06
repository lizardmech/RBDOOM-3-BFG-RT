#pragma once

#include "PathTraceRuntimeMaterialEvalKernel.h"
#include "PathTraceMaterialBindingKernel.h"
#include "PathTraceMaterialRecordKernel.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <unordered_map>
#include <vector>

// Owned CPU frame package. Resource discovery/publication stays with its owner.
struct RtCpuRewriteMaterialIdentity
{
    uint32_t id = 0, variantBase = 0;
    bool registeredStatic = false;
};
struct RtCpuRewriteMaterialSource
{
    std::vector<RtPathTraceRuntimeMaterialStagePod> stages;
    bool opaqueCompatibility = false;
    int primaryFallbackStage = -1, normalStage = -1;
};
struct RtCpuRewriteMaterialSurface
{
    uint32_t source = 0, materialId = 0, registerBegin = 0, registerCount = 0;
    int entityIndex = -1, entityNum = -1, modelSurfaceIndex = -1;
    float origin[3] = {};
    bool hasRegisters = false;
};
struct RtCpuRewriteMaterialFrameSurface
{
    uint32_t entityIndex = 0, modelSurfaceIndex = 0, baseId = 0, id = 0, ordinal = 0;
    float primary[6] = { 1,0,0,0,1,0 }, normal[6] = { 1,0,0,0,1,0 };
    auto Key() const { return std::make_tuple(entityIndex, modelSurfaceIndex, baseId); }
};
struct RtCpuRewriteMaterialInput
{
    static constexpr size_t kMaxBytes = 32ull * 1024ull * 1024ull;
    static constexpr size_t kMaxSurfaces = 65535;
    uint64_t rootFrame = 0, registryGeneration = 0;
    bool prepareFrame = false;
    RtCpuMaterialBindingInput bindings;
    std::vector<RtCpuRewriteMaterialIdentity> registry;
    std::vector<RtCpuRewriteMaterialSource> sources;
    std::vector<RtCpuRewriteMaterialSurface> surfaces;
    std::vector<RtCpuRewriteMaterialFrameSurface> constantSurfaces;
    std::vector<float> registers;
    bool WithinCapacity() const;
    uint64_t ChargedBytes() const;
};
struct RtCpuRewriteMaterialFrame
{
    std::vector<RtPathTraceRuntimeMaterialDecisionPod> decisions;
    std::vector<RtCpuRewriteMaterialFrameSurface> surfaces;
    std::vector<uint32_t> activeIds, firstSurfaceOrdinals;
    std::vector<uint8_t> bindingSafety;
    std::vector<RtCpuMaterialRecordSample> recordSamples;
    std::vector<uint32_t> emissiveSampleOrdinals;
    const RtCpuRewriteMaterialFrameSurface* Find(uint32_t entity, uint32_t surface, uint32_t base) const
    {
        const auto key = std::make_tuple(entity, surface, base);
        const auto found = std::lower_bound(surfaces.begin(), surfaces.end(), key,
            [](const auto& row, const auto& value) { return row.Key() < value; });
        return found != surfaces.end() && found->Key() == key ? &*found : nullptr;
    }
};

inline bool BuildRtCpuMaterialFrame(const RtCpuRewriteMaterialInput& input,
    const std::vector<RtPathTraceRuntimeMaterialEvalPod>& evaluations,
    RtCpuRewriteMaterialFrame& output)
{
    if (!input.WithinCapacity() || evaluations.size() != input.surfaces.size()) return false;
    output = {};
    std::unordered_map<uint32_t, RtCpuRewriteMaterialIdentity> registry;
    registry.reserve(input.registry.size() + input.surfaces.size());
    for (const auto& row : input.registry)
        if (!registry.emplace(row.id, row).second) return false;
    output.decisions.reserve(evaluations.size());
    output.surfaces.reserve(evaluations.size() + input.constantSurfaces.size());
    output.activeIds.reserve(evaluations.size());
    for (size_t i = 0; i < evaluations.size(); ++i)
    {
        const auto& surface = input.surfaces[i];
        const auto& source = input.sources[surface.source];
        const auto& eval = evaluations[i];
        const auto base = registry.find(surface.materialId);
        const auto key = BuildPathTraceRuntimeMaterialVariantKeyFromPod(surface.materialId,
            surface.entityIndex, surface.entityNum, surface.modelSurfaceIndex, 0, nullptr, 0);
        auto decision = SelectPathTraceRuntimeMaterialVariant(key, eval, base != registry.end(),
            base != registry.end() && base->second.registeredStatic,
            [&](uint32_t candidate) { return registry.find(candidate) != registry.end(); },
            [&](uint32_t candidate, uint32_t parent) {
                const auto found = registry.find(candidate);
                return found != registry.end() && found->second.variantBase == parent;
            });
        if (decision.chosenMaterialId != surface.materialId)
        {
            if (base == registry.end()) return false; // owner must hydrate before publication
            registry.emplace(decision.chosenMaterialId,
                RtCpuRewriteMaterialIdentity { decision.chosenMaterialId, surface.materialId, false });
        }
        output.decisions.push_back(decision);
        RtCpuRewriteMaterialFrameSurface row;
        row.entityIndex = static_cast<uint32_t>(surface.entityIndex);
        row.modelSurfaceIndex = static_cast<uint32_t>(surface.modelSurfaceIndex);
        row.baseId = surface.materialId; row.id = decision.chosenMaterialId;
        row.ordinal = static_cast<uint32_t>(i);
        const auto matrix = [&](int stageIndex, float* destination) {
            if (!surface.hasRegisters || stageIndex < 0 || size_t(stageIndex) >= source.stages.size()) return;
            const auto& stage = source.stages[stageIndex];
            if (!stage.valid || !stage.hasTexMatrix) return;
            for (size_t component = 0; component < 6; ++component)
            {
                const int reg = stage.texMatrixRegisters[component];
                if (reg >= 0 && uint32_t(reg) < surface.registerCount)
                    destination[component] = input.registers[surface.registerBegin + reg];
            }
        };
        int primary = eval.result == RtPathTraceRuntimeEvalBuildResult::Built ? eval.selectedStageIndex : -1;
        if (primary < 0 || size_t(primary) >= source.stages.size() ||
            !source.stages[primary].valid || !source.stages[primary].hasTexMatrix)
            primary = source.primaryFallbackStage;
        matrix(primary, row.primary);
        matrix(source.normalStage, row.normal);
        output.surfaces.push_back(row);
        output.activeIds.push_back(row.id);
    }
    // Constant rows have current membership but no numeric sample or variant.
    for (const auto& row : input.constantSurfaces)
    {
        if (!row.baseId || row.id != row.baseId || row.ordinal != UINT32_MAX) return false;
        for (float value : row.primary) if (!std::isfinite(value)) return false;
        for (float value : row.normal) if (!std::isfinite(value)) return false;
        output.surfaces.push_back(row);
        output.activeIds.push_back(row.id);
    }
    // Stable ordering preserves the old map's first-surface rule.
    std::stable_sort(output.surfaces.begin(), output.surfaces.end(),
        [](const auto& a, const auto& b) { return a.Key() < b.Key(); });
    for (size_t i = 1; i < output.surfaces.size(); ++i)
        if (output.surfaces[i-1].Key() == output.surfaces[i].Key() &&
            (output.surfaces[i-1].id != output.surfaces[i].id ||
             (output.surfaces[i-1].ordinal == UINT32_MAX) != (output.surfaces[i].ordinal == UINT32_MAX))) return false;
    output.surfaces.erase(std::unique(output.surfaces.begin(), output.surfaces.end(),
        [](const auto& a, const auto& b) { return a.Key() == b.Key(); }), output.surfaces.end());
    for (const auto& row : output.surfaces)
        if (row.ordinal != UINT32_MAX) output.firstSurfaceOrdinals.push_back(row.ordinal);
    std::sort(output.firstSurfaceOrdinals.begin(), output.firstSurfaceOrdinals.end());
    // Match the former owner first-surface filter exactly. Record building uses
    // all valid samples in this order; emission uses the first emissive sample
    // for each ID, including disabled emission that must suppress later samples.
    output.recordSamples.reserve(output.firstSurfaceOrdinals.size());
    std::unordered_map<uint32_t,bool> emissiveSeen;
    for (uint32_t ordinal : output.firstSurfaceOrdinals)
    {
        const auto& evaluated = evaluations[ordinal];
        if (evaluated.result != RtPathTraceRuntimeEvalBuildResult::Built) continue;
        const uint32_t id = output.decisions[ordinal].chosenMaterialId;
        output.recordSamples.push_back(RtCpuMaterialRecordSampleFromEvaluation(evaluated,id));
        if (evaluated.selectedStageEmissive && emissiveSeen.emplace(id,true).second)
            output.emissiveSampleOrdinals.push_back(ordinal);
    }
    std::sort(output.activeIds.begin(), output.activeIds.end());
    output.activeIds.erase(std::unique(output.activeIds.begin(), output.activeIds.end()), output.activeIds.end());
    return true;
}

// Cold definition proof. No sample may be dropped: even constant-looking stages
// take the live path if the established evaluator produces any runtime state.
inline bool BuildRtCpuConstantMaterialRoute(bool engineConstant, uint32_t materialId,
    const RtCpuRewriteMaterialSource& source, const float* registers, size_t registerCount,
    RtCpuRewriteMaterialFrameSurface& route)
{
    if (!engineConstant || !materialId || !registers || source.opaqueCompatibility) return false;
    for (const auto& stage : source.stages)
        if (stage.valid && (stage.usesPerSurfaceState || stage.dynamicImage || stage.cinematic ||
            stage.guiRenderTarget || stage.program || stage.emissiveLike)) return false;
    const auto evaluated=BuildPathTraceRuntimeMaterialEvalFromPod(true,materialId,
        source.stages.data(),source.stages.size(),registers,registerCount,false,nullptr,false);
    if (evaluated.result != RtPathTraceRuntimeEvalBuildResult::NoSelectedStage) return false;
    RtCpuRewriteMaterialFrameSurface candidate;
    candidate.baseId=candidate.id=materialId;candidate.ordinal=UINT32_MAX;
    const auto matrix=[&](int index,float* destination) {
        if (index<0 || size_t(index)>=source.stages.size()) return;
        const auto& stage=source.stages[index];
        if (!stage.valid || !stage.hasTexMatrix) return;
        for (size_t i=0;i<6;++i) {
            const int reg=stage.texMatrixRegisters[i];
            if (reg>=0 && size_t(reg)<registerCount) destination[i]=registers[reg];
        }
    };
    matrix(source.primaryFallbackStage,candidate.primary);
    matrix(source.normalStage,candidate.normal);
    for (float value:candidate.primary) if (!std::isfinite(value)) return false;
    for (float value:candidate.normal) if (!std::isfinite(value)) return false;
    route=candidate;
    return true;
}
