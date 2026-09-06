#pragma once

#include "PathTraceRigidIdentityKernel.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

constexpr std::size_t RT_PT_RUNTIME_MATERIAL_NAME_BYTES = 1024;
constexpr std::size_t RT_PT_RUNTIME_ORDERED_STAGE_CAPACITY = 8;
constexpr std::uint32_t RT_PT_RUNTIME_MATERIAL_MAX_COLLISION_PROBES = 16;

enum class RtPathTraceRuntimeEvalBuildResult : std::uint8_t
{
    NoMaterial,
    NoRegisters,
    NoSelectedStage,
    Built
};

struct RtPathTraceRuntimeMaterialStagePod
{
    std::int32_t stageIndex = -1;
    std::int32_t conditionRegister = -1;
    std::int32_t colorRegisters[4] = { -1, -1, -1, -1 };
    std::int32_t alphaTestRegister = -1;
    std::int32_t texMatrixRegisters[6] = { -1, -1, -1, -1, -1, -1 };
    bool valid = false;
    bool usesPerSurfaceState = false;
    bool diffuse = false;
    bool hasAlphaTest = false;
    bool hasTexMatrix = false;
    bool dynamicImage = false;
    bool cinematic = false;
    bool guiRenderTarget = false;
    bool program = false;
    bool emissiveLike = false;
    std::uint64_t drawStateBits = 0;
    std::int32_t lighting = 0;
    std::int32_t texgen = 0;
    std::int32_t imageDynamic = 0;
    bool imagePresent = false;
};

struct RtPathTraceRuntimeStageEvalPod
{
    std::int32_t stageIndex = -1;
    bool enabled = false;
    bool emissive = false;
    bool hasAlphaTest = false;
    bool hasTexMatrix = false;
    float condition = 1.0f;
    float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float alphaTest = 0.0f;
    float texMatrix[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
};

struct RtPathTraceRuntimeMaterialEvalPod
{
    RtPathTraceRuntimeEvalBuildResult result =
        RtPathTraceRuntimeEvalBuildResult::NoMaterial;
    std::uint32_t materialId = 0;
    std::int32_t selectedStageIndex = -1;
    std::int32_t selectedStagePriority = -1;
    std::int32_t enabledStages = 0;
    std::int32_t disabledStages = 0;
    std::int32_t colorStages = 0;
    std::int32_t alphaStages = 0;
    std::int32_t alphaTestStages = 0;
    std::int32_t texMatrixStages = 0;
    std::int32_t dynamicImageStages = 0;
    std::int32_t cinematicStages = 0;
    std::int32_t guiRenderTargetStages = 0;
    std::int32_t programStages = 0;
    bool selectedStageEmissive = false;
    bool hasDiffuseStageColor = false;
    bool orderedStageOverflow = false;
    bool hasSurfaceOrigin = false;
    float condition = 1.0f;
    float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float alphaTest = 0.0f;
    float texMatrix[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    float diffuseStageColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float diffuseStageCondition = 1.0f;
    float surfaceOrigin[3] = {};
    RtPathTraceRuntimeStageEvalPod
        orderedStages[RT_PT_RUNTIME_ORDERED_STAGE_CAPACITY];
    std::uint32_t orderedStageCount = 0;
};

struct RtPathTraceMaterialTextureVariantBasePod
{
    std::uint32_t variantId = 0;
    std::uint32_t baseId = 0;
};

struct RtPathTraceCaptureRegistryMaterialPod
{
    std::uint32_t materialId = 0;
    char materialName[RT_PT_RUNTIME_MATERIAL_NAME_BYTES] = {};
    bool hardwareOpaqueGeometry = false;
    bool hasAlphaTest = false;
    float alphaCutoff = 0.0f;
    bool emissive = false;
    bool skyEnvironment = false;
    bool materialMetadataValid = false;
    bool isDynamic = false;
    bool detailDecalLiquidPool = false;
};

struct RtPathTraceRuntimeMaterialVariantPod
{
    std::uint32_t baseMaterialId = 0;
    std::int32_t entityIndex = -1;
    std::int32_t entityNum = -1;
    std::int32_t modelSurfaceIndex = -1;
    std::uint64_t triIdentityBits = 0;
};

inline RtPathTraceRuntimeMaterialVariantPod
BuildPathTraceRuntimeMaterialVariantKeyFromPod(
    std::uint32_t baseMaterialId,
    std::int32_t entityIndex,
    std::int32_t entityNum,
    std::int32_t requestedModelSurfaceIndex,
    std::uint64_t currentTriToken,
    const std::uint64_t* orderedGeometryTokens,
    std::size_t tokenCount)
{
    RtPathTraceRuntimeMaterialVariantPod key;
    key.baseMaterialId = baseMaterialId;
    key.entityIndex = entityIndex;
    key.entityNum = entityNum;
    key.modelSurfaceIndex = ResolvePathTraceModelSurfaceIndexFromPod(
        requestedModelSurfaceIndex, currentTriToken,
        orderedGeometryTokens, tokenCount);
    key.triIdentityBits = key.modelSurfaceIndex < 0 ? currentTriToken : 0;
    return key;
}

struct RtPathTraceRuntimeMaterialDecisionPod
{
    RtPathTraceRuntimeMaterialEvalPod eval;
    std::uint32_t initialCandidateId = 0;
    std::uint32_t chosenMaterialId = 0;
    std::uint32_t collisionCount = 0;
    bool fallbackUsed = false;
    bool baseMaterialRegistered = false;
};

inline bool RtPathTraceRuntimeMaterialDecisionPodEqual(
    const RtPathTraceRuntimeMaterialDecisionPod& lhs,
    const RtPathTraceRuntimeMaterialDecisionPod& rhs) noexcept
{
    const RtPathTraceRuntimeMaterialEvalPod& le = lhs.eval;
    const RtPathTraceRuntimeMaterialEvalPod& re = rhs.eval;
    if (le.result != re.result || le.materialId != re.materialId ||
        le.selectedStageIndex != re.selectedStageIndex ||
        le.selectedStagePriority != re.selectedStagePriority ||
        le.enabledStages != re.enabledStages ||
        le.disabledStages != re.disabledStages ||
        le.colorStages != re.colorStages ||
        le.alphaStages != re.alphaStages ||
        le.alphaTestStages != re.alphaTestStages ||
        le.texMatrixStages != re.texMatrixStages ||
        le.dynamicImageStages != re.dynamicImageStages ||
        le.cinematicStages != re.cinematicStages ||
        le.guiRenderTargetStages != re.guiRenderTargetStages ||
        le.programStages != re.programStages ||
        le.selectedStageEmissive != re.selectedStageEmissive ||
        le.hasDiffuseStageColor != re.hasDiffuseStageColor ||
        le.orderedStageOverflow != re.orderedStageOverflow ||
        le.hasSurfaceOrigin != re.hasSurfaceOrigin ||
        le.condition != re.condition || le.alphaTest != re.alphaTest ||
        le.diffuseStageCondition != re.diffuseStageCondition ||
        le.orderedStageCount != re.orderedStageCount ||
        std::memcmp(le.color, re.color, sizeof(le.color)) != 0 ||
        std::memcmp(le.texMatrix, re.texMatrix, sizeof(le.texMatrix)) != 0 ||
        std::memcmp(le.diffuseStageColor, re.diffuseStageColor,
            sizeof(le.diffuseStageColor)) != 0 ||
        std::memcmp(le.surfaceOrigin, re.surfaceOrigin,
            sizeof(le.surfaceOrigin)) != 0)
    {
        return false;
    }
    for (std::size_t index = 0;
        index < RT_PT_RUNTIME_ORDERED_STAGE_CAPACITY; ++index)
    {
        const RtPathTraceRuntimeStageEvalPod& ls = le.orderedStages[index];
        const RtPathTraceRuntimeStageEvalPod& rs = re.orderedStages[index];
        if (ls.stageIndex != rs.stageIndex || ls.enabled != rs.enabled ||
            ls.emissive != rs.emissive ||
            ls.hasAlphaTest != rs.hasAlphaTest ||
            ls.hasTexMatrix != rs.hasTexMatrix ||
            ls.condition != rs.condition || ls.alphaTest != rs.alphaTest ||
            std::memcmp(ls.color, rs.color, sizeof(ls.color)) != 0 ||
            std::memcmp(ls.texMatrix, rs.texMatrix,
                sizeof(ls.texMatrix)) != 0)
        {
            return false;
        }
    }
    return lhs.initialCandidateId == rhs.initialCandidateId &&
        lhs.chosenMaterialId == rhs.chosenMaterialId &&
        lhs.collisionCount == rhs.collisionCount &&
        lhs.fallbackUsed == rhs.fallbackUsed &&
        lhs.baseMaterialRegistered == rhs.baseMaterialRegistered;
}

inline bool RtPathTraceRuntimeMaterialDecisionPodIsDefault(
    const RtPathTraceRuntimeMaterialDecisionPod& value) noexcept
{
    const RtPathTraceRuntimeMaterialDecisionPod defaultValue;
    return RtPathTraceRuntimeMaterialDecisionPodEqual(value, defaultValue);
}

inline float RtPathTraceRuntimeEvalRegister(
    const float* registers, std::size_t registerCount,
    std::int32_t registerIndex, float fallback, bool* present = nullptr)
{
    const bool valid = registers != nullptr && registerIndex >= 0 &&
        static_cast<std::size_t>(registerIndex) < registerCount;
    if (present)
    {
        *present = valid;
    }
    return valid ? registers[registerIndex] : fallback;
}

inline std::uint32_t RtPathTraceRuntimeHash32(
    std::uint32_t hash, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
    {
        hash ^= (value >> shift) & 0xffu;
        hash *= 16777619u;
    }
    return hash;
}

inline float RtPathTraceRuntimeLuminance(const float color[4])
{
    const float r = color[0] > 0.0f ? color[0] : 0.0f;
    const float g = color[1] > 0.0f ? color[1] : 0.0f;
    const float b = color[2] > 0.0f ? color[2] : 0.0f;
    return r * 0.2126f + g * 0.7152f + b * 0.0722f;
}

inline RtPathTraceRuntimeMaterialEvalPod BuildPathTraceRuntimeMaterialEvalFromPod(
    bool materialPresent, std::uint32_t materialId,
    const RtPathTraceRuntimeMaterialStagePod* stages, std::size_t stageCount,
    const float* registers, std::size_t registerCount,
    bool opaqueSwinglightCompatibility,
    const float surfaceOrigin[3], bool hasSurfaceOrigin)
{
    RtPathTraceRuntimeMaterialEvalPod output;
    output.materialId = materialId;
    output.hasSurfaceOrigin = hasSurfaceOrigin;
    if (surfaceOrigin)
    {
        std::memcpy(output.surfaceOrigin, surfaceOrigin,
            sizeof(output.surfaceOrigin));
    }
    if (!materialPresent)
    {
        return output;
    }
    if (!registers)
    {
        output.result = RtPathTraceRuntimeEvalBuildResult::NoRegisters;
        return output;
    }
    bool selected = false;
    for (std::size_t index = 0; index < stageCount; ++index)
    {
        const RtPathTraceRuntimeMaterialStagePod& stage = stages[index];
        if (!stage.valid)
        {
            continue;
        }
        if (stage.diffuse && !output.hasDiffuseStageColor)
        {
            output.hasDiffuseStageColor = true;
            output.diffuseStageCondition = RtPathTraceRuntimeEvalRegister(
                registers, registerCount, stage.conditionRegister, 1.0f);
            for (int component = 0; component < 4; ++component)
            {
                output.diffuseStageColor[component] =
                    RtPathTraceRuntimeEvalRegister(registers, registerCount,
                        stage.colorRegisters[component], 1.0f);
            }
        }
        if (!stage.usesPerSurfaceState)
        {
            continue;
        }
        RtPathTraceRuntimeStageEvalPod evaluated;
        evaluated.stageIndex = stage.stageIndex;
        evaluated.condition = RtPathTraceRuntimeEvalRegister(registers,
            registerCount, stage.conditionRegister, 1.0f);
        evaluated.enabled = evaluated.condition != 0.0f;
        output.enabledStages += evaluated.enabled ? 1 : 0;
        output.disabledStages += evaluated.enabled ? 0 : 1;
        bool hasColor = false;
        for (int component = 0; component < 4; ++component)
        {
            bool present = false;
            evaluated.color[component] = RtPathTraceRuntimeEvalRegister(
                registers, registerCount, stage.colorRegisters[component],
                1.0f, &present);
            hasColor = hasColor || present;
        }
        output.colorStages += hasColor ? 1 : 0;
        const float alphaDelta = evaluated.color[3] - 1.0f;
        output.alphaStages += hasColor &&
            (alphaDelta < -1.0e-4f || alphaDelta > 1.0e-4f) ? 1 : 0;
        bool alphaPresent = false;
        evaluated.alphaTest = stage.hasAlphaTest
            ? RtPathTraceRuntimeEvalRegister(registers, registerCount,
                stage.alphaTestRegister, 0.0f, &alphaPresent) : 0.0f;
        evaluated.hasAlphaTest = stage.hasAlphaTest && alphaPresent;
        output.alphaTestStages += evaluated.hasAlphaTest ? 1 : 0;
        evaluated.hasTexMatrix = stage.hasTexMatrix;
        if (stage.hasTexMatrix)
        {
            ++output.texMatrixStages;
            for (int element = 0; element < 6; ++element)
            {
                const float fallback = element == 0 || element == 4
                    ? 1.0f : 0.0f;
                evaluated.texMatrix[element] = RtPathTraceRuntimeEvalRegister(
                    registers, registerCount,
                    stage.texMatrixRegisters[element], fallback);
            }
        }
        output.dynamicImageStages += stage.dynamicImage ? 1 : 0;
        output.cinematicStages += stage.cinematic ? 1 : 0;
        output.guiRenderTargetStages += stage.guiRenderTarget ? 1 : 0;
        output.programStages += stage.program ? 1 : 0;
        evaluated.emissive = stage.emissiveLike ||
            (stage.stageIndex == 0 && opaqueSwinglightCompatibility);
        if (output.orderedStageCount < RT_PT_RUNTIME_ORDERED_STAGE_CAPACITY)
        {
            output.orderedStages[output.orderedStageCount++] = evaluated;
        }
        else
        {
            output.orderedStageOverflow = true;
        }
        const int priority = (stage.hasAlphaTest ? 8 : 0) +
            (evaluated.enabled ? 4 : 0) + (evaluated.emissive ? 2 : 0);
        if (!selected || priority > output.selectedStagePriority ||
            (priority == output.selectedStagePriority &&
                RtPathTraceRuntimeLuminance(evaluated.color) >
                    RtPathTraceRuntimeLuminance(output.color)))
        {
            selected = true;
            output.selectedStageIndex = stage.stageIndex;
            output.selectedStagePriority = priority;
            output.selectedStageEmissive = evaluated.emissive;
            output.condition = evaluated.condition;
            output.alphaTest = evaluated.alphaTest;
            std::memcpy(output.color, evaluated.color, sizeof(output.color));
            std::memcpy(output.texMatrix, evaluated.texMatrix,
                sizeof(output.texMatrix));
        }
    }
    output.result = selected ? RtPathTraceRuntimeEvalBuildResult::Built
        : RtPathTraceRuntimeEvalBuildResult::NoSelectedStage;
    return output;
}

template<typename OccupiedFn, typename OwnedByBaseFn>
inline RtPathTraceRuntimeMaterialDecisionPod
SelectPathTraceRuntimeMaterialVariant(
    const RtPathTraceRuntimeMaterialVariantPod& key,
    const RtPathTraceRuntimeMaterialEvalPod& eval,
    bool baseMaterialRegistered,
    bool registeredStaticShortcut,
    OccupiedFn occupied,
    OwnedByBaseFn ownedByBase)
{
    RtPathTraceRuntimeMaterialDecisionPod output;
    output.eval = eval;
    output.chosenMaterialId = key.baseMaterialId;
    output.baseMaterialRegistered = baseMaterialRegistered;
    if (registeredStaticShortcut)
    {
        output.eval = {};
        output.eval.materialId = key.baseMaterialId;
        output.initialCandidateId = key.baseMaterialId;
        return output;
    }
    if (eval.result != RtPathTraceRuntimeEvalBuildResult::Built ||
        key.baseMaterialId == 0 || key.entityIndex < 0)
    {
        output.initialCandidateId = key.baseMaterialId;
        return output;
    }
    // A built dynamic variant carries a MaterialInfoRegistrationIntent before
    // the variant proposal is applied, so the predicted final decision includes
    // the base registration even when it was absent from the input snapshot.
    output.baseMaterialRegistered = true;
    std::uint32_t hash = 2166136261u;
    hash = RtPathTraceRuntimeHash32(hash,
        key.modelSurfaceIndex >= 0 ? 0x72746573u : 0x72747632u);
    hash = RtPathTraceRuntimeHash32(hash, key.baseMaterialId);
    hash = RtPathTraceRuntimeHash32(hash,
        static_cast<std::uint32_t>(key.entityIndex));
    hash = RtPathTraceRuntimeHash32(hash,
        static_cast<std::uint32_t>(key.entityNum));
    hash = RtPathTraceRuntimeHash32(hash,
        static_cast<std::uint32_t>(key.modelSurfaceIndex));
    if (key.modelSurfaceIndex < 0)
    {
        hash = RtPathTraceRuntimeHash32(hash,
            static_cast<std::uint32_t>(key.triIdentityBits));
        hash = RtPathTraceRuntimeHash32(hash,
            static_cast<std::uint32_t>(key.triIdentityBits >> 32));
    }
    output.initialCandidateId = hash | 0x80000000u;
    if (output.initialCandidateId == 0 ||
        output.initialCandidateId == key.baseMaterialId)
    {
        output.initialCandidateId = (hash ^ 0x5bd1e995u) | 0x80000000u;
    }
    std::uint32_t candidate = output.initialCandidateId;
    for (std::uint32_t attempt = 0;
        attempt < RT_PT_RUNTIME_MATERIAL_MAX_COLLISION_PROBES; ++attempt)
    {
        if (candidate != 0 && candidate != key.baseMaterialId &&
            (!occupied(candidate) || ownedByBase(candidate, key.baseMaterialId)))
        {
            output.chosenMaterialId = candidate;
            output.collisionCount = attempt;
            return output;
        }
        candidate = RtPathTraceRuntimeHash32(
            candidate ^ 0x9e3779b9u, attempt + 1u) | 0x80000000u;
    }
    output.collisionCount = RT_PT_RUNTIME_MATERIAL_MAX_COLLISION_PROBES;
    output.fallbackUsed = true;
    return output;
}

inline RtPathTraceRuntimeMaterialDecisionPod
BuildPathTraceRuntimeMaterialDecisionFromPod(
    const RtPathTraceRuntimeMaterialVariantPod& key,
    const RtPathTraceRuntimeMaterialEvalPod& eval,
    const RtPathTraceMaterialTextureVariantBasePod* variantBases,
    std::size_t variantBaseCount,
    const RtPathTraceCaptureRegistryMaterialPod* registryMaterials,
    std::size_t registryMaterialCount)
{
    bool baseRegistered = false;
    bool registeredStatic = false;
    for (std::size_t index = 0; index < registryMaterialCount; ++index)
    {
        if (registryMaterials[index].materialId == key.baseMaterialId)
        {
            baseRegistered = true;
            registeredStatic = registryMaterials[index].materialMetadataValid &&
                !registryMaterials[index].isDynamic;
        }
    }
    const auto occupied = [&](std::uint32_t candidate)
    {
        for (std::size_t index = 0; index < registryMaterialCount; ++index)
        {
            if (registryMaterials[index].materialId == candidate)
            {
                return true;
            }
        }
        return false;
    };
    const auto owned = [&](std::uint32_t candidate, std::uint32_t base)
    {
        for (std::size_t index = 0; index < variantBaseCount; ++index)
        {
            if (variantBases[index].variantId == candidate)
            {
                return variantBases[index].baseId == base;
            }
        }
        return false;
    };
    return SelectPathTraceRuntimeMaterialVariant(
        key, eval, baseRegistered, registeredStatic, occupied, owned);
}
