#include "precompiled.h"
#pragma hdrstop

// Doom material texture discovery for the RT smoke texture registry.
//
// This pass walks visible material declarations and records the texture metadata
// that the dynamic material table can safely bind. Rejections are preserved as
// diagnostics because many Doom materials are still intentionally unsupported.
//
// RES-31 material fact ownership:
// STATIC resident facts: material identity/name, coverage, selected stable image
// names and safe handles, fallback albedo, constant alpha cutoff, decal routing,
// emissive base color, texture formats/usages, and classifier/static surface flags.
// DYNAMIC per-frame override facts: runtime register-driven condition/color/alpha/
// texture matrix values, cinematic/video/currentRender/GUI/render-target images,
// alpha test thresholds driven by registers, program-stage output, animated stages,
// and time/predefined-register-dependent stage state.

#include "PathTraceCVars.h"
#include "PathTraceMaterialTextureDiscovery.h"
#include "PathTraceDebugDumps.h"
#include "PathTraceDoomMaterialClassifier.h"
#include "PathTraceDynamicMaterialState.h"
#include "PathTraceMaterialClassifier.h"
#include "PathTraceSurfaceClassification.h"
#include "PathTraceSceneCapture.h"
#include "PathTraceTextureRegistry.h"
#include "../RenderCommon.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

RtSmokeMaterialMetadataFrameStats g_smokeMaterialMetadataFrameStats;

namespace {
bool IsSmokePortalWindowFallbackMaterial(const idMaterial* material);
bool IsSmokeObjectGlassFallbackMaterial(const idMaterial* material);
bool IsSmokeTranslucentOverlayCardMaterial(const idMaterial* material, const RtSmokeTranslucentClassifierInfo& classifier);

struct RtResidentMaterialFacts
{
    bool valid = false;
    uint64 materialGeneration = 0;
    RtSmokeMaterialTextureInfo info;
};

struct RtSmokeMaterialHydrationSetCache
{
    bool valid = false;
    uint64 signature = 0;
    int materialCount = 0;
    std::vector<uint32_t> refreshMaterialIds;
};

std::unordered_map<uint32_t, RtResidentMaterialFacts> g_residentMaterialFacts;
uint64 g_residentMaterialFactsGeneration = 1;
RtSmokeMaterialHydrationSetCache g_smokeMaterialHydrationSetCache;

bool PathTraceMaterialClassifierRequested()
{
    return r_pathTracingMatClassEnable.GetInteger() != 0 ||
        r_pathTracingMatClassDebugList.GetInteger() != 0 ||
        r_pathTracingLiquidPoolMode.GetInteger() != 0 ||
        r_pathTracingLiquidPoolDebug.GetInteger() != 0;
}

void CountSmokeResidentMaterialDynamicSplit(int& residentStatic, int& residentDynamic)
{
    residentStatic = 0;
    residentDynamic = 0;
    for (const std::pair<const uint32_t, RtResidentMaterialFacts>& resident : g_residentMaterialFacts)
    {
        if (!resident.second.valid)
        {
            continue;
        }
        if (resident.second.info.isDynamic)
        {
            ++residentDynamic;
        }
        else
        {
            ++residentStatic;
        }
    }
}

void DumpSmokeMaterialResidencyStatsIfNeeded()
{
    if (r_pathTracingResidencyDump.GetInteger() == 0)
    {
        return;
    }

    static int lastDumpFrame = -120;
    if (tr.frameCount - lastDumpFrame < 120)
    {
        return;
    }
    lastDumpFrame = tr.frameCount;
    int residentStatic = 0;
    int residentDynamic = 0;
    CountSmokeResidentMaterialDynamicSplit(residentStatic, residentDynamic);
    common->Printf(
        "PathTracePrimaryPass: RES materialDiscover visited=%d derived=%d hits=%d misses=%d residentStatic=%d residentDynamic=%d\n",
        g_smokeMaterialMetadataFrameStats.registrations,
        g_smokeMaterialMetadataFrameStats.fullDiscovers,
        g_smokeMaterialMetadataFrameStats.cacheRefreshes,
        g_smokeMaterialMetadataFrameStats.fullDiscovers,
        residentStatic,
        residentDynamic);
    common->Printf(
        "PathTracePrimaryPass: RES materialHydration visited=%d derived=%d hits=%d misses=%d refresh/skip=%d/%d\n",
        g_smokeMaterialMetadataFrameStats.idHydrationVisited,
        g_smokeMaterialMetadataFrameStats.idHydrationDerived,
        g_smokeMaterialMetadataFrameStats.idHydrationCacheHits,
        g_smokeMaterialMetadataFrameStats.idHydrationCacheMisses,
        g_smokeMaterialMetadataFrameStats.idHydrationRefreshes,
        g_smokeMaterialMetadataFrameStats.idHydrationRefreshSkips);
}

enum class RtSmokeTextureCodeHint
{
    Unknown,
    Specular0000,
    DiffuseAlpha0100,
    Normal0300,
    AlphaClip1000
};

const char* SmokeTextureCodeHintName(RtSmokeTextureCodeHint hint)
{
    switch (hint)
    {
        case RtSmokeTextureCodeHint::Specular0000:
            return "0000/specular";
        case RtSmokeTextureCodeHint::DiffuseAlpha0100:
            return "0100/diffuse-alpha";
        case RtSmokeTextureCodeHint::Normal0300:
            return "0300/normal";
        case RtSmokeTextureCodeHint::AlphaClip1000:
            return "1000/alpha-clip";
        default:
            return "unknown";
    }
}

uint64 HashSmokeResidentMaterialValue(uint64 hash, uint64 value)
{
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash;
}

uint64 ComputeSmokeMaterialHydrationSetSignature(const std::vector<uint32_t>& materialIds)
{
    uint64 hash = 1469598103934665603ull;
    const uint64 version = 1;
    hash = HashSmokeResidentMaterialValue(hash, version);
    hash = HashSmokeResidentMaterialValue(hash, static_cast<uint64>(materialIds.size()));
    hash = HashSmokeResidentMaterialValue(hash, g_residentMaterialFactsGeneration);
    hash = HashSmokeResidentMaterialValue(hash, SmokeMaterialTextureRegistryGeneration());
    for (uint32_t materialId : materialIds)
    {
        hash = HashSmokeResidentMaterialValue(hash, materialId);
    }
    return hash;
}

void UpdateSmokeMaterialHydrationSetCache(
    const std::vector<uint32_t>& materialIds,
    const std::vector<uint32_t>& refreshMaterialIds)
{
    g_smokeMaterialHydrationSetCache.valid = true;
    g_smokeMaterialHydrationSetCache.signature = ComputeSmokeMaterialHydrationSetSignature(materialIds);
    g_smokeMaterialHydrationSetCache.materialCount = static_cast<int>(materialIds.size());
    g_smokeMaterialHydrationSetCache.refreshMaterialIds = refreshMaterialIds;
}

uint64 HashSmokeResidentMaterialString(uint64 hash, const char* value)
{
    const char* cursor = value ? value : "";
    while (*cursor)
    {
        hash = HashSmokeResidentMaterialValue(hash, static_cast<uint8_t>(*cursor));
        ++cursor;
    }
    return HashSmokeResidentMaterialValue(hash, 0xffu);
}

uint64 HashSmokeResidentMaterialFloat(uint64 hash, float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return HashSmokeResidentMaterialValue(hash, bits);
}

bool SmokeMaterialResidencyEnabled()
{
    return r_pathTracingResidency.GetInteger() != 0 &&
        r_pathTracingResidencyMaterial.GetInteger() != 0;
}

bool SmokeMaterialTextureInfoNeedsHydrationRefresh(const RtSmokeMaterialTextureInfo& info)
{
    return (info.hasDiffuseImage && !info.hasTextureHandle) ||
        (info.hasAlphaImage && !info.hasAlphaTextureHandle) ||
        (info.hasNormalImage && !info.hasNormalTextureHandle) ||
        (info.hasSpecularImage && !info.hasSpecularTextureHandle) ||
        (info.hasEmissiveImage && !info.hasEmissiveTextureHandle);
}

bool SmokeResidentMaterialRegisterDependsOnRuntime(const idMaterial* material, int registerIndex)
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

bool SmokeResidentMaterialImageIsDynamic(idImage* image)
{
    if (!image)
    {
        return false;
    }
    const auto& opts = image->GetOpts();
    return opts.isRenderTarget ||
        opts.isUAV ||
        IsSmokeImageNameGuiLike(image->GetName()) ||
        !IsSmokeImageNameSafeForRayTracing(image->GetName());
}

bool SmokeMaterialTextureInfoHasDynamicHydrationSource(const RtSmokeMaterialTextureInfo& info)
{
    return SmokeResidentMaterialImageIsDynamic(info.diffuseImage) ||
        SmokeResidentMaterialImageIsDynamic(info.alphaImage) ||
        SmokeResidentMaterialImageIsDynamic(info.normalImage) ||
        SmokeResidentMaterialImageIsDynamic(info.specularImage) ||
        SmokeResidentMaterialImageIsDynamic(info.emissiveImage);
}

bool SmokeMaterialTextureInfoCanSkipStaticHydrationRefresh(const RtSmokeMaterialTextureInfo& info)
{
    return SmokeMaterialResidencyEnabled() &&
        !SmokeMaterialTextureInfoNeedsHydrationRefresh(info) &&
        !SmokeMaterialTextureInfoHasDynamicHydrationSource(info);
}

bool SmokeResidentMaterialStageHasDynamicFacts(const idMaterial* material, const shaderStage_t* stage)
{
    if (!material || !stage)
    {
        return false;
    }

    if (SmokeResidentMaterialRegisterDependsOnRuntime(material, stage->conditionRegister))
    {
        return true;
    }
    if (stage->hasAlphaTest && SmokeResidentMaterialRegisterDependsOnRuntime(material, stage->alphaTestRegister))
    {
        return true;
    }
    for (int component = 0; component < 4; ++component)
    {
        if (SmokeResidentMaterialRegisterDependsOnRuntime(material, stage->color.registers[component]))
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
                if (SmokeResidentMaterialRegisterDependsOnRuntime(material, stage->texture.matrix[row][column]))
                {
                    return true;
                }
            }
        }
    }

    return stage->texture.cinematic != nullptr ||
        stage->texture.dynamic != DI_STATIC ||
        stage->texture.dynamicFrameCount > 0 ||
        stage->texture.texgen == TG_SCREEN ||
        stage->texture.texgen == TG_SCREEN2 ||
        stage->texture.texgen == TG_GLASSWARP ||
        SmokeStageIsRenderMap(stage) ||
        stage->newStage != nullptr ||
        SmokeResidentMaterialImageIsDynamic(stage->texture.image);
}

bool SmokeResidentMaterialHasDynamicFacts(const idMaterial* material, const RtSmokeTranslucentClassifierInfo& classifier)
{
    if (!material)
    {
        return true;
    }
    if (material->HasGui() ||
        classifier.nameLooksGui ||
        classifier.sortIsGuiOrSubview ||
        classifier.sortIsPostProcess ||
        classifier.hasScreenTexgen)
    {
        return true;
    }

    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        if (SmokeResidentMaterialStageHasDynamicFacts(material, material->GetStage(stageIndex)))
        {
            return true;
        }
    }
    return false;
}

uint64 ComputeSmokeResidentMaterialGeneration(const idMaterial* material, uint32_t materialId, const char* materialName)
{
    const RtSmokeTranslucentClassifierInfo classifier = BuildSmokeTranslucentClassifierInfo(material);
    uint64 hash = 1469598103934665603ull;
    hash = HashSmokeResidentMaterialValue(hash, materialId);
    hash = HashSmokeResidentMaterialString(hash, materialName);
    hash = HashSmokeResidentMaterialValue(hash, material ? static_cast<uint64>(material->Coverage()) : 0u);
    hash = HashSmokeResidentMaterialValue(hash, material ? static_cast<uint64>(material->GetNumStages()) : 0u);
    hash = HashSmokeResidentMaterialValue(hash, material ? static_cast<uint64>(material->GetContentFlags()) : 0u);
    hash = HashSmokeResidentMaterialValue(hash, material && material->HasGui() ? 1u : 0u);
    hash = HashSmokeResidentMaterialValue(hash, material ? static_cast<uint64>(material->Spectrum()) : 0u);
    hash = HashSmokeResidentMaterialValue(hash, material && material->ConstantRegisters() ? 1u : 0u);
    hash = HashSmokeResidentMaterialFloat(hash, material ? material->GetSort() : 0.0f);
    hash = HashSmokeResidentMaterialValue(hash, SmokeResidentMaterialHasDynamicFacts(material, classifier) ? 1u : 0u);
    hash = HashSmokeResidentMaterialValue(hash, r_pathTracingAllowGuiTextures.GetInteger() != 0 ? 1u : 0u);
    hash = HashSmokeResidentMaterialValue(hash, r_pathTracingForceTextureCodeUse.GetInteger() != 0 ? 1u : 0u);
    hash = HashSmokeResidentMaterialValue(hash, r_pathTracingSceneSource2RigidEntities.GetInteger() != 0 ? 1u : 0u);
    hash = HashSmokeResidentMaterialValue(hash, static_cast<uint64>(GetPathTraceMaterialClassifierGeneration()));
    if (material)
    {
        const float* constantRegisters = material->ConstantRegisters();
        const int registerCount = material->GetNumRegisters();
        for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
        {
            const shaderStage_t* stage = material->GetStage(stageIndex);
            hash = HashSmokeResidentMaterialValue(hash, stage ? static_cast<uint64>(stage->texture.texgen) : 0u);
            hash = HashSmokeResidentMaterialString(hash, stage && stage->texture.image ? stage->texture.image->GetName() : "<none>");
            if (stage && constantRegisters && stage->conditionRegister >= 0 && stage->conditionRegister < registerCount)
            {
                hash = HashSmokeResidentMaterialFloat(hash, constantRegisters[stage->conditionRegister]);
            }
        }
    }
    return hash;
}

bool TryRegisterResidentMaterialFacts(const idMaterial* material, uint32_t materialId, uint64 materialGeneration)
{
    const std::unordered_map<uint32_t, RtResidentMaterialFacts>::const_iterator resident =
        g_residentMaterialFacts.find(materialId);
    if (resident == g_residentMaterialFacts.end() ||
        !resident->second.valid ||
        resident->second.materialGeneration != materialGeneration)
    {
        return false;
    }

    RtSmokeMaterialTextureInfo* info = FindSmokeMaterialTextureInfo(materialId);
    if (!info)
    {
        ++g_smokeMaterialMetadataFrameStats.newEntries;
        info = &AddSmokeMaterialTextureInfo(materialId, resident->second.info.materialName.c_str());
    }

    *info = resident->second.info;
    info->tableIndex = -1;
    RefreshSmokeMaterialTextureHandleState(*info);
    if (PathTraceMaterialClassifierRequested())
    {
        RegisterPathTraceMaterialRecord(material, *info);
    }
    ++g_smokeMaterialMetadataFrameStats.cacheRefreshes;
    return true;
}

void StoreResidentMaterialFacts(uint32_t materialId, uint64 materialGeneration, const RtSmokeMaterialTextureInfo& info)
{
    RtResidentMaterialFacts& resident = g_residentMaterialFacts[materialId];
    const bool changed =
        !resident.valid ||
        resident.materialGeneration != materialGeneration ||
        resident.info.isDynamic != info.isDynamic;
    resident.valid = true;
    resident.materialGeneration = materialGeneration;
    resident.info = info;
    resident.info.tableIndex = -1;
    if (changed)
    {
        ++g_residentMaterialFactsGeneration;
    }
}

RtSmokeTextureCodeHint SmokeTextureCodeHintFromImageName(const char* imageName)
{
    if (!imageName || !imageName[0])
    {
        return RtSmokeTextureCodeHint::Unknown;
    }

    idStr name = imageName;
    name.BackSlashesToSlashes();
    const int length = name.Length();
    for (int start = Max(0, length - 12); start <= length - 4; ++start)
    {
        const bool fourDigits =
            name[start + 0] >= '0' && name[start + 0] <= '9' &&
            name[start + 1] >= '0' && name[start + 1] <= '9' &&
            name[start + 2] >= '0' && name[start + 2] <= '9' &&
            name[start + 3] >= '0' && name[start + 3] <= '9';
        if (!fourDigits)
        {
            continue;
        }

        const bool boundedBefore = start == 0 || name[start - 1] == '_' || name[start - 1] == '-' || name[start - 1] == '/' || name[start - 1] == '.';
        const bool boundedAfter = start + 4 >= length || name[start + 4] == '_' || name[start + 4] == '-' || name[start + 4] == '.' || name[start + 4] == '/';
        if (!boundedBefore || !boundedAfter)
        {
            continue;
        }

        if (idStr::Cmpn(name.c_str() + start, "0000", 4) == 0)
        {
            return RtSmokeTextureCodeHint::Specular0000;
        }
        if (idStr::Cmpn(name.c_str() + start, "0100", 4) == 0)
        {
            return RtSmokeTextureCodeHint::DiffuseAlpha0100;
        }
        if (idStr::Cmpn(name.c_str() + start, "0300", 4) == 0)
        {
            return RtSmokeTextureCodeHint::Normal0300;
        }
        if (idStr::Cmpn(name.c_str() + start, "1000", 4) == 0)
        {
            return RtSmokeTextureCodeHint::AlphaClip1000;
        }
    }

    return RtSmokeTextureCodeHint::Unknown;
}

bool SmokeImageNameLooksSpecularSuffix(const char* imageName)
{
    if (!imageName || !imageName[0])
    {
        return false;
    }

    idStr name = imageName;
    name.BackSlashesToSlashes();
    name.ToLower();
    name.StripFileExtension();

    const char* text = name.c_str();
    const int length = name.Length();
    int segmentStart = 0;
    for (int index = length - 1; index >= 0; --index)
    {
        if (text[index] == '/')
        {
            segmentStart = index + 1;
            break;
        }
    }

    for (int index = segmentStart; index < length - 1; ++index)
    {
        if (text[index] != '_' || text[index + 1] != 's')
        {
            continue;
        }
        if (index + 2 == length)
        {
            return true;
        }
        const char afterSpecToken = text[index + 2];
        return afterSpecToken == '_' ||
            afterSpecToken == '-' ||
            (afterSpecToken >= '0' && afterSpecToken <= '9');
    }

    return false;
}

bool SmokeStageConditionCanBeActive(const idMaterial* material, const shaderStage_t* stage)
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

idImage* FindSmokeImageByTextureCode(const idMaterial* material, RtSmokeTextureCodeHint wantedHint, idStr& reason)
{
    if (!material || wantedHint == RtSmokeTextureCodeHint::Unknown || r_pathTracingForceTextureCodeUse.GetInteger() == 0)
    {
        return nullptr;
    }

    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || !SmokeStageConditionCanBeActive(material, stage) || !stage->texture.image)
        {
            continue;
        }

        const RtSmokeTextureCodeHint hint = SmokeTextureCodeHintFromImageName(stage->texture.image->GetName());
        if (hint == wantedHint)
        {
            reason = va("stage %d texture code %s", stageIndex, SmokeTextureCodeHintName(hint));
            return stage->texture.image;
        }
    }

    return nullptr;
}

idImage* FindSmokeImageBySpecularSuffix(const idMaterial* material, idStr& reason)
{
    if (!material)
    {
        return nullptr;
    }

    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || !SmokeStageConditionCanBeActive(material, stage) || !stage->texture.image)
        {
            continue;
        }

        if (SmokeImageNameLooksSpecularSuffix(stage->texture.image->GetName()))
        {
            reason = va("stage %d _s suffix", stageIndex);
            return stage->texture.image;
        }
    }

    return nullptr;
}

idImage* FindSmokeImageByUsageAndFormat(const idMaterial* material, textureUsage_t wantedUsage, textureColor_t wantedFormat, stageLighting_t wantedLighting, idStr& reason)
{
    if (!material)
    {
        return nullptr;
    }

    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || !SmokeStageConditionCanBeActive(material, stage) || !stage->texture.image)
        {
            continue;
        }
        if (wantedLighting != SL_AMBIENT && stage->lighting != wantedLighting)
        {
            continue;
        }

        idImage* image = stage->texture.image;
        if (image->GetUsage() == wantedUsage || image->GetOpts().colorFormat == wantedFormat)
        {
            reason = va("stage %d %s/%s", stageIndex, SmokeTextureUsageName(image->GetUsage()), SmokeTextureColorFormatName(image->GetOpts().colorFormat));
            return image;
        }
    }

    return nullptr;
}

idImage* FindSmokeDiffuseImage(const idMaterial* material, idStr& reason)
{
    if (!material)
    {
        reason = "null material";
        return nullptr;
    }

    idImage* image = material->GetFastPathDiffuseImage();
    if (image)
    {
        reason = "fastPathDiffuse";
        return image;
    }

    image = FindSmokeImageByUsageAndFormat(material, TD_DIFFUSE, CFM_YCOCG_DXT5, SL_DIFFUSE, reason);
    if (image)
    {
        return image;
    }

    image = FindSmokeImageByTextureCode(material, RtSmokeTextureCodeHint::DiffuseAlpha0100, reason);
    if (image)
    {
        return image;
    }

    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || !SmokeStageConditionCanBeActive(material, stage) || stage->lighting != SL_DIFFUSE || !stage->texture.image)
        {
            continue;
        }

        reason = va("stage %d SL_DIFFUSE", stageIndex);
        return stage->texture.image;
    }

    const RtSmokeTranslucentClassifierInfo classifier = BuildSmokeTranslucentClassifierInfo(material);
    if (r_pathTracingAllowGuiTextures.GetInteger() != 0 && (material->HasGui() || classifier.nameLooksGui || classifier.sortIsGuiOrSubview))
    {
        for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
        {
            const shaderStage_t* stage = material->GetStage(stageIndex);
            if (!stage || !SmokeStageConditionCanBeActive(material, stage) || !stage->texture.image)
            {
                continue;
            }

            if (stage->texture.texgen == TG_SCREEN ||
                stage->texture.texgen == TG_SCREEN2 ||
                SmokeStageIsRenderMap(stage))
            {
                reason = va("stage %d GUI screen/dynamic", stageIndex);
                return stage->texture.image;
            }
        }

        for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
        {
            const shaderStage_t* stage = material->GetStage(stageIndex);
            if (!stage || !SmokeStageConditionCanBeActive(material, stage) || !stage->texture.image)
            {
                continue;
            }

            if (stage->lighting == SL_AMBIENT || stage->lighting == SL_DIFFUSE)
            {
                reason = va("stage %d GUI ambient/diffuse", stageIndex);
                return stage->texture.image;
            }
        }
    }

    if (IsSmokeTranslucentOverlayCardMaterial(material, classifier))
    {
        for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
        {
            const shaderStage_t* stage = material->GetStage(stageIndex);
            if (!stage || !SmokeStageConditionCanBeActive(material, stage) || stage->lighting != SL_AMBIENT || !stage->texture.image)
            {
                continue;
            }

            reason = va("stage %d SL_AMBIENT translucent blend", stageIndex);
            return stage->texture.image;
        }
    }

    reason = "no fast-path or diffuse-stage image";
    return nullptr;
}

idImage* FindSmokeNormalImage(const idMaterial* material, idStr& reason)
{
    if (!material)
    {
        reason = "null material";
        return nullptr;
    }

    idImage* image = material->GetFastPathBumpImage();
    if (image)
    {
        reason = "fastPathBump";
        return image;
    }

    image = FindSmokeImageByUsageAndFormat(material, TD_BUMP, CFM_NORMAL_DXT5, SL_BUMP, reason);
    if (image)
    {
        return image;
    }

    image = FindSmokeImageByTextureCode(material, RtSmokeTextureCodeHint::Normal0300, reason);
    if (image)
    {
        return image;
    }

    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || !SmokeStageConditionCanBeActive(material, stage) || stage->lighting != SL_BUMP || !stage->texture.image)
        {
            continue;
        }

        reason = va("stage %d SL_BUMP", stageIndex);
        return stage->texture.image;
    }

    reason = "no fast-path or bump-stage image";
    return nullptr;
}

idImage* FindSmokeSpecularImage(const idMaterial* material, idStr& reason)
{
    if (!material)
    {
        reason = "null material";
        return nullptr;
    }

    idImage* image = material->GetFastPathSpecularImage();
    if (image)
    {
        reason = "fastPathSpecular";
        return image;
    }

    image = FindSmokeImageByUsageAndFormat(material, TD_SPECULAR, CFM_DEFAULT, SL_SPECULAR, reason);
    if (image)
    {
        return image;
    }

    image = FindSmokeImageByTextureCode(material, RtSmokeTextureCodeHint::Specular0000, reason);
    if (image)
    {
        return image;
    }

    image = FindSmokeImageBySpecularSuffix(material, reason);
    if (image)
    {
        return image;
    }

    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || !SmokeStageConditionCanBeActive(material, stage) || stage->lighting != SL_SPECULAR || !stage->texture.image)
        {
            continue;
        }

        reason = va("stage %d SL_SPECULAR", stageIndex);
        return stage->texture.image;
    }

    reason = "no fast-path or specular-stage image";
    return nullptr;
}

bool SmokeStageIsAdditiveOrGlowLike(const shaderStage_t* stage)
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

bool SmokeStageTextureIsAnimatedOrViewDependent(const shaderStage_t* stage)
{
    if (!stage)
    {
        return false;
    }

    return stage->texture.cinematic != nullptr ||
        stage->texture.dynamic != DI_STATIC ||
        stage->texture.dynamicFrameCount > 0 ||
        stage->texture.texgen == TG_SCREEN ||
        stage->texture.texgen == TG_SCREEN2 ||
        stage->texture.texgen == TG_GLASSWARP;
}

bool SmokeStageConstantColor(const idMaterial* material, const shaderStage_t* stage, idVec4& color)
{
    color = idVec4(1.0f, 1.0f, 1.0f, 1.0f);
    if (!material || !stage)
    {
        return false;
    }

    const float* constantRegisters = material->ConstantRegisters();
    const int registerCount = material->GetNumRegisters();
    if (!constantRegisters)
    {
        return false;
    }

    for (int component = 0; component < 4; ++component)
    {
        const int registerIndex = stage->color.registers[component];
        if (registerIndex < 0 || registerIndex >= registerCount)
        {
            return false;
        }
        color[component] = constantRegisters[registerIndex];
    }
    color.x = Max(0.0f, color.x);
    color.y = Max(0.0f, color.y);
    color.z = Max(0.0f, color.z);
    color.w = idMath::ClampFloat(0.0f, 1.0f, color.w);
    return true;
}

bool IsSmokeReflectiveEyewearMaterial(const idMaterial* material)
{
    if (!material || material->Coverage() != MC_PERFORATED)
    {
        return false;
    }

    idStr materialName = material->GetName();
    static const char* eyewearTokens[] = { "glasses", "goggles", "gogs", "visor" };
    if (!SmokeNameContainsAny(materialName, eyewearTokens, sizeof(eyewearTokens) / sizeof(eyewearTokens[0])))
    {
        return false;
    }

    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (stage && stage->lighting == SL_SPECULAR && stage->texture.image)
        {
            return true;
        }
    }
    return false;
}

bool IsSmokeAbsorbingBlackMaterial(const idMaterial* material)
{
    if (!material)
    {
        return false;
    }

    idStr materialName = material->GetName();
    materialName.BackSlashesToSlashes();
    return materialName.Icmp("textures/sfx/black") == 0;
}

bool SmokeNameLooksParticleOrSfxCard(const char* name)
{
    if (!name || !name[0])
    {
        return false;
    }

    idStr path = name;
    path.BackSlashesToSlashes();
    path.ToLower();
    return idStr::FindText(path.c_str(), "/particles/", false) >= 0 ||
        idStr::FindText(path.c_str(), "particles/", false) == 0 ||
        idStr::FindText(path.c_str(), "/sfx/", false) >= 0 ||
        idStr::FindText(path.c_str(), "sfx/", false) == 0;
}

bool SmokeImageNameLooksExplicitAlpha(const char* name)
{
    if (!name || !name[0])
    {
        return false;
    }

    idStr path = name;
    path.BackSlashesToSlashes();
    path.ToLower();
    path.StripFileExtension();

    const char* text = path.c_str();
    const int length = path.Length();
    int segmentStart = 0;
    for (int index = length - 1; index >= 0; --index)
    {
        if (text[index] == '/')
        {
            segmentStart = index + 1;
            break;
        }
    }

    const char* segment = text + segmentStart;
    return idStr::FindText(segment, "alpha", false) >= 0 ||
        idStr::FindText(segment, "alph", false) >= 0 ||
        idStr::FindText(segment, "mask", false) >= 0 ||
        idStr::FindText(segment, "_a", false) >= 0 ||
        idStr::FindText(segment, "-a", false) >= 0 ||
        idStr::FindText(segment, "_coverage", false) >= 0 ||
        idStr::FindText(segment, "-coverage", false) >= 0;
}

bool SmokeImageHasExplicitAlphaFormat(const idImage* image)
{
    if (!image)
    {
        return false;
    }

    const idImageOpts& opts = image->GetOpts();
    return image->GetUsage() == TD_COVERAGE ||
        opts.colorFormat == CFM_GREEN_ALPHA ||
        opts.format == FMT_ALPHA ||
        opts.format == FMT_L8A8;
}

bool ShouldSmokeParticleOrSfxUseDiffuseLumaAlpha(const idMaterial* material, const idImage* diffuseImage, const idImage* alphaImage)
{
    if (!material || !diffuseImage || alphaImage)
    {
        return false;
    }

    if (!SmokeNameLooksParticleOrSfxCard(material->GetName()) && !SmokeNameLooksParticleOrSfxCard(diffuseImage->GetName()))
    {
        return false;
    }

    if (SmokeImageHasExplicitAlphaFormat(diffuseImage) || SmokeImageNameLooksExplicitAlpha(diffuseImage->GetName()))
    {
        return false;
    }

    return true;
}

bool SmokeNameLooksSterlightMagentaKey(const char* name)
{
    if (!name || !name[0])
    {
        return false;
    }

    idStr path = name;
    path.BackSlashesToSlashes();
    path.ToLower();
    return idStr::FindText(path.c_str(), "textures/base_light/sterlightdecal", false) >= 0;
}

bool ShouldSmokeAlphaTestUseDiffuseMagentaKey(const idMaterial* material, const idImage* diffuseImage, const idImage* alphaImage)
{
    if (!material || !diffuseImage || alphaImage)
    {
        return false;
    }
    if (material->Coverage() != MC_PERFORATED)
    {
        return false;
    }

    return SmokeNameLooksSterlightMagentaKey(material->GetName()) ||
        SmokeNameLooksSterlightMagentaKey(diffuseImage->GetName());
}

void ForceSmokeAbsorbingBlackMaterialInfo(RtSmokeMaterialTextureInfo& info)
{
    info.diffuseImage = nullptr;
    info.alphaImage = nullptr;
    info.normalImage = nullptr;
    info.specularImage = nullptr;
    info.emissiveImage = nullptr;
    info.diffuseTexture = nullptr;
    info.alphaTexture = nullptr;
    info.normalTexture = nullptr;
    info.specularTexture = nullptr;
    info.emissiveTexture = nullptr;
    info.hasDiffuseImage = false;
    info.hasAlphaImage = false;
    info.hasNormalImage = false;
    info.hasSpecularImage = false;
    info.hasEmissiveImage = false;
    info.hasTextureHandle = false;
    info.hasAlphaTextureHandle = false;
    info.hasNormalTextureHandle = false;
    info.hasSpecularTextureHandle = false;
    info.hasEmissiveTextureHandle = false;
    info.hasSafeTexture = false;
    info.hasSafeAlphaTexture = false;
    info.hasSafeNormalTexture = false;
    info.hasSafeSpecularTexture = false;
    info.hasSafeEmissiveTexture = false;
    info.hasAlphaTest = false;
    info.additiveDecal = false;
    info.additiveDecalWhiteKey = false;
    info.filterDecal = false;
    info.filterDecalBlackKey = false;
    info.detailDecal = false;
    info.detailDecalDynamic = false;
    info.detailDecalLiquidPool = false;
    info.liquidFilmHasBloodSemantic = false;
    info.liquidFilmHasWetReflectStage = false;
    info.liquidFilmHasCoverageSource = false;
    info.liquidFilmHasWetNormalSource = false;
    info.liquidFilmExactOverride = false;
    info.liquidFilmCandidate = false;
    info.liquidFilmCoverageImageName = "<none>";
    info.liquidFilmOverrideReason = "none";
    info.liquidFilmReason = "reject-absorbing-black";
    info.isDynamic = false;
    info.alphaFromDiffuseLuma = false;
    info.forceFallbackAlbedo = true;
    info.alphaFromDiffuseDarkKey = false;
    info.alphaFromDiffuseMagentaKey = false;
    info.portalWindowFallback = false;
    info.objectGlassFallback = false;
    info.emissive = false;
    info.emissiveLightCandidate = false;
    info.alphaCutoff = 0.0f;
    info.emissiveColor = idVec4(0.0f, 0.0f, 0.0f, 1.0f);
    info.fallbackAlbedo = idVec4(0.0f, 0.0f, 0.0f, 1.0f);
    info.hasFallbackAlbedo = true;
    info.diffuseUsage = TD_DEFAULT;
    info.alphaUsage = TD_DEFAULT;
    info.normalUsage = TD_DEFAULT;
    info.specularUsage = TD_DEFAULT;
    info.emissiveUsage = TD_DEFAULT;
    info.diffuseColorFormat = CFM_DEFAULT;
    info.alphaColorFormat = CFM_DEFAULT;
    info.normalColorFormat = CFM_DEFAULT;
    info.specularColorFormat = CFM_DEFAULT;
    info.emissiveColorFormat = CFM_DEFAULT;
    info.diffuseImageName = "<none>";
    info.alphaImageName = "<none>";
    info.normalImageName = "<none>";
    info.specularImageName = "<none>";
    info.emissiveImageName = "<none>";
    info.fallbackReason = "absorbing black material";
    info.alphaReason = "absorbing black material";
    info.normalReason = "absorbing black material";
    info.specularReason = "absorbing black material";
    info.emissiveReason = "absorbing black material";
}

bool IsSmokePortalWindowFallbackMaterial(const idMaterial* material)
{
    if (!material || material->Coverage() != MC_TRANSLUCENT)
    {
        return false;
    }

    const RtSmokeTranslucentClassifierInfo classifier = BuildSmokeTranslucentClassifierInfo(material);
    return classifier.sortIsGuiOrSubview || classifier.sortIsPostProcess || classifier.hasScreenTexgen;
}

bool IsSmokeObjectGlassFallbackMaterial(const idMaterial* material)
{
    if (!material || material->Coverage() != MC_TRANSLUCENT)
    {
        return false;
    }

    const RtSmokeTranslucentClassifierInfo classifier = BuildSmokeTranslucentClassifierInfo(material);
    return classifier.nameLooksGlass && !IsSmokePortalWindowFallbackMaterial(material);
}

bool IsSmokeTranslucentOverlayCardMaterial(const idMaterial* material, const RtSmokeTranslucentClassifierInfo& classifier)
{
    if (!material || material->Coverage() != MC_TRANSLUCENT)
    {
        return false;
    }
    if (IsSmokePortalWindowFallbackMaterial(material))
    {
        return false;
    }
    if (classifier.hasAddDefault0200Texture)
    {
        return false;
    }

    return classifier.sortIsDecal ||
        classifier.polygonOffsetDecal ||
        classifier.nameLooksDecal ||
        (classifier.hasAmbientBlendStage && !classifier.hasDiffuseStage);
}

bool IsSmokeLiquidPoolDetailDecalMaterial(const idMaterial* material, const RtSmokeMaterialTextureInfo& info)
{
    static const char* liquidPoolTokens[] = {
        "bloodpool",
        "blood_pool",
        "liquidpool",
        "liquid_pool",
        "waterpool",
        "water_pool",
        "slimepool",
        "slime_pool",
        "pool_liquid",
        "pool"
    };

    idStr materialName = material ? material->GetName() : info.materialName;
    if (SmokeNameContainsAny(materialName, liquidPoolTokens, sizeof(liquidPoolTokens) / sizeof(liquidPoolTokens[0])))
    {
        return true;
    }

    return SmokeNameContainsAny(info.diffuseImageName, liquidPoolTokens, sizeof(liquidPoolTokens) / sizeof(liquidPoolTokens[0])) ||
        SmokeNameContainsAny(info.alphaImageName, liquidPoolTokens, sizeof(liquidPoolTokens) / sizeof(liquidPoolTokens[0]));
}

bool SmokeLiquidFilmImageIsSafeSource(const idImage* image)
{
    if (!image || !IsSmokeImageNameSafeForRayTracing(image->GetName()))
    {
        return false;
    }

    const idImageOpts& opts = image->GetOpts();
    return opts.samples == 1 && opts.textureType == DTT_2D && !opts.isRenderTarget && !opts.isUAV;
}

bool IsSmokeLiquidFilmExactOverride(const idMaterial* material, idStr& reason)
{
    // LPD-00 found no deployed positive that requires an override: all audited
    // wet families expose an active reflect2 stage. Keep the zero-entry policy
    // explicit so a future exception must add one exact declaration and reason.
    (void)material;
    reason = "none-no-audited-override";
    return false;
}

void ResolveSmokeLiquidFilmCandidateFacts(const idMaterial* material, RtSmokeMaterialTextureInfo& info)
{
    info.liquidFilmHasBloodSemantic = material && (material->GetContentFlags() & CONTENTS_BLOOD) != 0;
    info.liquidFilmHasWetReflectStage = false;
    if (material)
    {
        for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
        {
            const shaderStage_t* stage = material->GetStage(stageIndex);
            if (stage && SmokeStageConditionCanBeActive(material, stage) && stage->texture.texgen == TG_REFLECT_CUBE2)
            {
                info.liquidFilmHasWetReflectStage = true;
                break;
            }
        }
    }

    const bool safeAlphaSource = SmokeLiquidFilmImageIsSafeSource(info.alphaImage);
    const bool safeDiffuseSource = SmokeLiquidFilmImageIsSafeSource(info.diffuseImage);
    info.liquidFilmHasCoverageSource = safeAlphaSource || safeDiffuseSource;
    info.liquidFilmCoverageImageName = safeAlphaSource
        ? info.alphaImageName
        : (safeDiffuseSource ? info.diffuseImageName : idStr("<none>"));
    info.liquidFilmHasWetNormalSource = SmokeLiquidFilmImageIsSafeSource(info.normalImage);
    info.liquidFilmExactOverride = IsSmokeLiquidFilmExactOverride(material, info.liquidFilmOverrideReason);

    info.liquidFilmCandidate = false;
    if (!material)
    {
        info.liquidFilmReason = "reject-null-material";
    }
    else if (!info.detailDecal)
    {
        info.liquidFilmReason = "reject-not-detail-decal";
    }
    else if (!info.liquidFilmHasCoverageSource)
    {
        info.liquidFilmReason = "reject-no-safe-coverage";
    }
    else if (!info.liquidFilmHasWetReflectStage && !info.liquidFilmExactOverride)
    {
        info.liquidFilmReason = "reject-no-reflect2-or-override";
    }
    else
    {
        info.liquidFilmCandidate = true;
        info.liquidFilmReason = info.liquidFilmExactOverride ? "accept-exact-override" : "accept-active-reflect2";
    }
}

bool FindSmokeMaterialFallbackAlbedo(const idMaterial* material, idVec4& albedo)
{
    albedo = idVec4(0.0f, 0.0f, 0.0f, 1.0f);
    if (!material)
    {
        return false;
    }

    if (IsSmokeReflectiveEyewearMaterial(material))
    {
        albedo = idVec4(0.08f, 0.10f, 0.11f, 1.0f);
        return true;
    }

    const RtSmokeTranslucentClassifierInfo classifier = BuildSmokeTranslucentClassifierInfo(material);
    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage)
        {
            continue;
        }

        if (stage->texture.texgen == TG_SKYBOX_CUBE || stage->texture.texgen == TG_WOBBLESKY_CUBE)
        {
            albedo = idVec4(0.0f, 0.0f, 0.0f, 1.0f);
            return true;
        }
    }

    if (!IsSmokePortalWindowFallbackMaterial(material))
    {
        return false;
    }

    albedo = idVec4(0.55f, 0.70f, 0.72f, 1.0f);
    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || (stage->lighting != SL_AMBIENT && stage->lighting != SL_DIFFUSE))
        {
            continue;
        }

        idVec4 stageColor;
        if (!SmokeStageConstantColor(material, stage, stageColor))
        {
            continue;
        }

        if (stageColor.x <= 0.0f && stageColor.y <= 0.0f && stageColor.z <= 0.0f)
        {
            continue;
        }

        albedo = stageColor;
        return true;
    }

    return true;
}

idImage* FindSmokeSkyEnvironmentImage(const idMaterial* material, idVec4& skyColor)
{
    skyColor = idVec4(1.0f, 1.0f, 1.0f, 1.0f);
    if (!material)
    {
        return nullptr;
    }

    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || !stage->texture.image ||
            (stage->texture.texgen != TG_SKYBOX_CUBE && stage->texture.texgen != TG_WOBBLESKY_CUBE))
        {
            continue;
        }

        idVec4 stageColor;
        if (SmokeStageConstantColor(material, stage, stageColor))
        {
            skyColor = stageColor;
        }
        return stage->texture.image;
    }
    return nullptr;
}

idImage* FindSmokeEmissiveImage(const idMaterial* material, idStr& reason, idVec4& emissiveColor, bool& lightCandidate)
{
    emissiveColor = idVec4(0.0f, 0.0f, 0.0f, 1.0f);
    lightCandidate = false;
    if (!material)
    {
        reason = "null material";
        return nullptr;
    }

    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage)
        {
            continue;
        }

        const bool skyOrCubeTexgen =
            stage->texture.texgen == TG_DIFFUSE_CUBE ||
            stage->texture.texgen == TG_REFLECT_CUBE ||
            stage->texture.texgen == TG_REFLECT_CUBE2 ||
            stage->texture.texgen == TG_SKYBOX_CUBE ||
            stage->texture.texgen == TG_WOBBLESKY_CUBE;
        if (skyOrCubeTexgen)
        {
            reason = va("stage %d rejected sky/cube material emissive texgen", stageIndex);
            return nullptr;
        }
    }

    const RtSmokeTranslucentClassifierInfo classifier = BuildSmokeTranslucentClassifierInfo(material);
    const bool allowLightPromotion = !(classifier.hasScreenTexgen ||
        classifier.hasAddDefault0200Texture ||
        classifier.sortIsGuiOrSubview ||
        classifier.sortIsDecal ||
        classifier.sortIsPostProcess ||
        classifier.nameLooksGui ||
        classifier.nameLooksParticle ||
        classifier.nameLooksGlass ||
        classifier.nameLooksDecal);

    const bool nameLooksEmissive = !classifier.hasAddDefault0200Texture && (classifier.nameLooksGlow || classifier.nameLooksSignage);
    const float* constantRegisters = material->ConstantRegisters();
    const int registerCount = material->GetNumRegisters();
    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || stage->lighting != SL_AMBIENT)
        {
            continue;
        }
        if (constantRegisters && stage->conditionRegister >= 0 && stage->conditionRegister < registerCount && constantRegisters[stage->conditionRegister] == 0.0f)
        {
            continue;
        }

        const bool additiveStage = SmokeStageIsAdditiveOrGlowLike(stage);
        bool filterBlackKey = false;
        const bool filterStage = SmokeStageIsFilterBlend(stage, filterBlackKey);
        const bool skyOrCubeTexgen =
            stage->texture.texgen == TG_DIFFUSE_CUBE ||
            stage->texture.texgen == TG_REFLECT_CUBE ||
            stage->texture.texgen == TG_REFLECT_CUBE2 ||
            stage->texture.texgen == TG_SKYBOX_CUBE ||
            stage->texture.texgen == TG_WOBBLESKY_CUBE;
        if (skyOrCubeTexgen)
        {
            reason = va("stage %d rejected sky/cube emissive texgen", stageIndex);
            continue;
        }
        const bool stageLooksEmissive = additiveStage || (nameLooksEmissive && classifier.hasAmbientBlendStage && !classifier.nameLooksDecal && !filterStage);
        if (!stageLooksEmissive)
        {
            continue;
        }
        if (SmokeStageTextureIsAnimatedOrViewDependent(stage))
        {
            reason = va("stage %d rejected animated/view-dependent emissive texture", stageIndex);
            continue;
        }

        idVec4 stageColor;
        if (SmokeStageConstantColor(material, stage, stageColor))
        {
            emissiveColor = stageColor;
        }
        else
        {
            emissiveColor = idVec4(1.0f, 1.0f, 1.0f, 1.0f);
        }

        if (stage->texture.image)
        {
            reason = va("stage %d SL_AMBIENT glow/additive", stageIndex);
            lightCandidate = allowLightPromotion;
            return stage->texture.image;
        }

        if (additiveStage && (emissiveColor.x > 0.0f || emissiveColor.y > 0.0f || emissiveColor.z > 0.0f))
        {
            reason = va("stage %d SL_AMBIENT constant glow/additive", stageIndex);
            lightCandidate = allowLightPromotion;
            return nullptr;
        }
    }

    reason = "no emissive ambient/additive stage";
    return nullptr;
}

idImage* FindSmokeAlphaImage(const idMaterial* material, idStr& reason)
{
    if (!material)
    {
        reason = "null material";
        return nullptr;
    }

    // The declaring alpha-test stage is the primary source. Do not require a
    // filename category or MC_PERFORATED: explicit stage behavior is authoritative.
    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || !stage->hasAlphaTest || stage->ignoreAlphaTest || !stage->texture.image)
        {
            continue;
        }

        // YCoCg DXT5 consumes the stored alpha channel for color reconstruction.
        // Prefer a separately parsed coverage image below rather than treating it
        // as authored opacity.
        if (stage->texture.image->GetOpts().colorFormat != CFM_YCOCG_DXT5)
        {
            reason = va("stage %d authored alpha-test", stageIndex);
            return stage->texture.image;
        }
    }

    idImage* image = FindSmokeImageByUsageAndFormat(material, TD_COVERAGE, CFM_GREEN_ALPHA, SL_COVERAGE, reason);
    if (image)
    {
        return image;
    }

    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || stage->ignoreAlphaTest || !stage->texture.image)
        {
            continue;
        }
        if (stage->lighting == SL_COVERAGE)
        {
            reason = va("stage %d SL_COVERAGE", stageIndex);
            return stage->texture.image;
        }
    }

    if (material->Coverage() != MC_PERFORATED)
    {
        reason = "no authored alpha-test or coverage stage";
        return nullptr;
    }

    // Legacy filename-code support is retained only as a compatibility fallback
    // for perforated declarations whose parsed stages expose no usable alpha image.
    image = FindSmokeImageByTextureCode(material, RtSmokeTextureCodeHint::AlphaClip1000, reason);
    if (image)
    {
        return image;
    }

    reason = "no SL_COVERAGE image";
    return nullptr;
}

}

uint64 SmokeResidentMaterialFactsGeneration()
{
    return g_residentMaterialFactsGeneration;
}

int ClearSmokeResidentMaterialFacts()
{
    const int removedCount = static_cast<int>(g_residentMaterialFacts.size());
    g_residentMaterialFacts.clear();
    g_smokeMaterialHydrationSetCache = RtSmokeMaterialHydrationSetCache();
    if (removedCount > 0)
    {
        ++g_residentMaterialFactsGeneration;
    }
    return removedCount;
}

bool RegisterSmokeMaterialTextureInfo(const idMaterial* material)
{
    ++g_smokeMaterialMetadataFrameStats.registrations;

    const char* materialName = material ? material->GetName() : "<none>";
    const uint32_t materialId = HashSmokeMaterialName(materialName);
    const bool residencyEnabled = SmokeMaterialResidencyEnabled();
    const uint64 residentMaterialGeneration = residencyEnabled
        ? ComputeSmokeResidentMaterialGeneration(material, materialId, materialName)
        : 0;
    const bool residentHit =
        residencyEnabled &&
        TryRegisterResidentMaterialFacts(material, materialId, residentMaterialGeneration);
    if (residentHit)
    {
        return false;
    }

    RtSmokeMaterialTextureInfo* info = FindSmokeMaterialTextureInfo(materialId);
    const bool residentNeedsDerive = residencyEnabled && !residentHit;
    if (info && r_pathTracingMaterialMetadataCache.GetInteger() != 0 && !residentNeedsDerive)
    {
        const RtSmokeTranslucentClassifierInfo classifier = BuildSmokeTranslucentClassifierInfo(material);
        const bool forceRediscoverAbsorbingBlack = IsSmokeAbsorbingBlackMaterial(material);
        const bool rediscoverGuiDiffuse =
            r_pathTracingAllowGuiTextures.GetInteger() != 0 &&
            (material && (material->HasGui() || classifier.nameLooksGui || classifier.sortIsGuiOrSubview)) &&
            !info->hasDiffuseImage;
        if (!rediscoverGuiDiffuse && !forceRediscoverAbsorbingBlack)
        {
            ++g_smokeMaterialMetadataFrameStats.cacheRefreshes;
            RefreshSmokeMaterialTextureHandleState(*info);
            if (PathTraceMaterialClassifierRequested())
            {
                RegisterPathTraceMaterialRecord(material, *info);
            }
            if (residencyEnabled)
            {
                StoreResidentMaterialFacts(materialId, residentMaterialGeneration, *info);
            }
            return false;
        }
    }

    OPTICK_EVENT("PT Material Metadata Discover One");

    if (!info)
    {
        ++g_smokeMaterialMetadataFrameStats.newEntries;
        info = &AddSmokeMaterialTextureInfo(materialId, materialName);
    }

    ++g_smokeMaterialMetadataFrameStats.fullDiscovers;

    idStr reason;
    idImage* diffuseImage = FindSmokeDiffuseImage(material, reason);
    idStr alphaReason;
    idImage* alphaImage = FindSmokeAlphaImage(material, alphaReason);
    idStr normalReason;
    idImage* normalImage = FindSmokeNormalImage(material, normalReason);
    idStr specularReason;
    idImage* specularImage = FindSmokeSpecularImage(material, specularReason);
    idStr emissiveReason;
    idVec4 emissiveColor;
    bool emissiveLightCandidate = false;
    idImage* emissiveImage = FindSmokeEmissiveImage(material, emissiveReason, emissiveColor, emissiveLightCandidate);
    idVec4 skyColor;
    idImage* skyImage = FindSmokeSkyEnvironmentImage(material, skyColor);
    idVec4 fallbackAlbedo;
    const bool hasFallbackAlbedo = FindSmokeMaterialFallbackAlbedo(material, fallbackAlbedo);
    info->materialName = materialName;
    info->fallbackReason = reason;
    info->alphaReason = alphaReason;
    info->normalReason = normalReason;
    info->specularReason = specularReason;
    info->emissiveReason = emissiveReason;
    info->diffuseImage = diffuseImage;
    info->alphaImage = alphaImage;
    info->normalImage = normalImage;
    info->specularImage = specularImage;
    info->emissiveImage = emissiveImage;
    info->skyImage = skyImage;
    info->coverage = material ? material->Coverage() : MC_BAD;
    info->hasDiffuseImage = diffuseImage != nullptr;
    info->hasAlphaImage = alphaImage != nullptr;
    info->hasNormalImage = normalImage != nullptr;
    info->hasSpecularImage = specularImage != nullptr;
    info->hasEmissiveImage = emissiveImage != nullptr;
    info->skyEnvironment = skyImage != nullptr;
    info->hasTextureHandle = diffuseImage && diffuseImage->GetTextureHandle();
    info->hasAlphaTextureHandle = alphaImage && alphaImage->GetTextureHandle();
    info->hasNormalTextureHandle = normalImage && normalImage->GetTextureHandle();
    info->hasSpecularTextureHandle = specularImage && specularImage->GetTextureHandle();
    info->hasEmissiveTextureHandle = emissiveImage && emissiveImage->GetTextureHandle();
    info->diffuseImageName = diffuseImage ? diffuseImage->GetName() : "<none>";
    info->alphaImageName = alphaImage ? alphaImage->GetName() : "<none>";
    info->normalImageName = normalImage ? normalImage->GetName() : "<none>";
    info->specularImageName = specularImage ? specularImage->GetName() : "<none>";
    info->emissiveImageName = emissiveImage ? emissiveImage->GetName() : "<none>";
    info->skyImageName = skyImage ? skyImage->GetName() : "<none>";
    info->diffuseUsage = diffuseImage ? diffuseImage->GetUsage() : TD_DEFAULT;
    info->alphaUsage = alphaImage ? alphaImage->GetUsage() : TD_DEFAULT;
    info->normalUsage = normalImage ? normalImage->GetUsage() : TD_DEFAULT;
    info->specularUsage = specularImage ? specularImage->GetUsage() : TD_DEFAULT;
    info->emissiveUsage = emissiveImage ? emissiveImage->GetUsage() : TD_DEFAULT;
    info->diffuseColorFormat = diffuseImage ? diffuseImage->GetOpts().colorFormat : CFM_DEFAULT;
    info->alphaColorFormat = alphaImage ? alphaImage->GetOpts().colorFormat : CFM_DEFAULT;
    info->normalColorFormat = normalImage ? normalImage->GetOpts().colorFormat : CFM_DEFAULT;
    info->specularColorFormat = specularImage ? specularImage->GetOpts().colorFormat : CFM_DEFAULT;
    info->emissiveColorFormat = emissiveImage ? emissiveImage->GetOpts().colorFormat : CFM_DEFAULT;
    ResolveSmokeMaterialAlphaInfo(material, info->hasAlphaTest, info->alphaCutoff);
    info->additiveDecal = IsSmokeAdditiveDecalMaterial(material);
    const RtSmokeTranslucentClassifierInfo classifier = BuildSmokeTranslucentClassifierInfo(material);
    const bool portalWindowFallback = IsSmokePortalWindowFallbackMaterial(material);
    const bool objectGlassFallback = IsSmokeObjectGlassFallbackMaterial(material);
    info->additiveDecalWhiteKey = IsSmokeAdditiveWhiteKeyMaterial(material, classifier);
    const bool rgbKeyedBlendDecal = IsSmokeRgbKeyedBlendDecalMaterial(material, classifier);
    const bool yCoCgDiffuseMapDecal = IsSmokeYCoCgDiffuseMapDecalMaterial(material, classifier);
    const bool particleOrSfxDiffuseLumaAlpha = ShouldSmokeParticleOrSfxUseDiffuseLumaAlpha(material, diffuseImage, alphaImage);
    // Glass can use the same ambient blend shapes as receiver-modulating cards,
    // but it owns a transmission/reflection PSR and must remain the committed
    // primary hit.  Marking it as a filter decal makes the decal-composite ray
    // replace it with the receiver behind the pane before glass transport runs,
    // while the legacy stochastic path can reject it entirely.
    info->filterDecal =
        !info->additiveDecal &&
        !portalWindowFallback &&
        !objectGlassFallback &&
        (IsSmokeTranslucentOverlayCardMaterial(material, classifier) || rgbKeyedBlendDecal);
    info->detailDecal = IsSmokeDetailDecalCardMaterial(material, classifier);
    if (info->skyEnvironment)
    {
        // A skybox stage is a terminal environment surface, not an additive
        // decal layered over a receiver. Its geometry remains opaque so
        // authored black sealing meshes can occlude it.
        info->additiveDecal = false;
        info->additiveDecalWhiteKey = false;
        info->filterDecal = false;
        info->detailDecal = false;
    }
    info->detailDecalDynamic = info->detailDecal && material && material->ConstantRegisters() == NULL;
    info->filterDecalBlackKey = false;
    bool foundFilterBlend = false;
    if (info->filterDecal)
    {
        for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
        {
            const shaderStage_t* stage = material->GetStage(stageIndex);
            if (!stage || stage->lighting != SL_AMBIENT || !stage->texture.image)
            {
                continue;
            }

            bool blackKey = false;
            if (SmokeStageIsFilterBlend(stage, blackKey))
            {
                info->filterDecalBlackKey = blackKey;
                foundFilterBlend = true;
                break;
            }
        }
        if (!foundFilterBlend && yCoCgDiffuseMapDecal)
        {
            info->filterDecalBlackKey = true;
        }
        if (particleOrSfxDiffuseLumaAlpha)
        {
            info->filterDecalBlackKey = true;
        }
    }
    // `blend diffuseMap` detail decals (SL_DIFFUSE route stage, no explicit
    // ambient filter blend) are LIT DIFFUSE interaction layers in raster:
    // litWall + light * tex.rgb * stageColor. Black texels contribute nothing.
    // Treating them as filter/modulate darkens the receiver with the texture
    // (the alphabet4/pentastic1_spectrum failure). Composite them as an
    // additive ALBEDO layer instead. Do NOT exclude on the rgb-keyed/YCoCg
    // decal heuristics - those tag every diffuse-map decal and are exactly the
    // misroute this kind corrects. Materials whose ART lives in an ambient
    // alpha-blend stage (splat-style: ambient art + white diffuse lighting
    // mask) are NOT diffuse-lit; they keep their over/filter routing.
    info->detailDecalDiffuseLit =
        info->detailDecal &&
        classifier.hasDiffuseStage &&
        !classifier.hasAmbientBlendStage &&
        !foundFilterBlend &&
        !info->additiveDecal;
    if (info->detailDecalDiffuseLit)
    {
        info->filterDecal = false;
        info->filterDecalBlackKey = false;
    }
    if (SmokeMaterialUsesOpaqueSwinglightCompatibility(material))
    {
        // This runtime light-switch material is authored as source-alpha-over,
        // but the evaluated stage is the opaque emissive fixture shell. Do not
        // reinterpret it as a receiver-modulating translucent card.
        info->filterDecal = false;
        info->filterDecalBlackKey = false;
    }
    info->detailDecalLiquidPool =
        info->detailDecal &&
        IsSmokeLiquidPoolDetailDecalMaterial(material, *info);
    // Spectrum surfaces ("invisible writing", e.g. pentastic1_spectrum) only
    // receive light from lights with a matching spectrum. The per-frame record
    // synthesis keys off this (PathTraceSmokeSceneBuild).
    info->detailDecalSpectrum = info->detailDecal && material ? material->Spectrum() : 0;
    info->isDynamic =
        SmokeResidentMaterialHasDynamicFacts(material, classifier) ||
        info->detailDecalDynamic ||
        info->detailDecalSpectrum > 0;
    info->alphaFromDiffuseLuma =
        (info->hasAlphaTest &&
            diffuseImage != nullptr &&
            alphaImage == diffuseImage &&
            (diffuseImage->GetUsage() == TD_DIFFUSE || diffuseImage->GetOpts().colorFormat == CFM_YCOCG_DXT5)) ||
        particleOrSfxDiffuseLumaAlpha;
    info->alphaFromDiffuseMagentaKey =
        info->hasAlphaTest &&
        ShouldSmokeAlphaTestUseDiffuseMagentaKey(material, diffuseImage, alphaImage);
    info->forceFallbackAlbedo = IsSmokeReflectiveEyewearMaterial(material);
    info->alphaFromDiffuseDarkKey = info->alphaFromDiffuseLuma && info->forceFallbackAlbedo;
    info->portalWindowFallback = portalWindowFallback;
    info->objectGlassFallback = objectGlassFallback;
    info->emissiveColor = emissiveColor;
    info->emissive = info->hasEmissiveImage || emissiveColor.x > 0.0f || emissiveColor.y > 0.0f || emissiveColor.z > 0.0f;
    info->emissiveLightCandidate = info->emissive && emissiveLightCandidate;
    info->skyColor = skyColor;
    if (info->emissive)
    {
        if (info->hasAlphaTest && info->hasAlphaImage)
        {
            info->alphaReason = va("%s; preserved authored alpha-test with emissive stage", alphaReason.c_str());
        }
    }
    info->fallbackAlbedo = fallbackAlbedo;
    info->hasFallbackAlbedo = hasFallbackAlbedo;
    if (IsSmokeAbsorbingBlackMaterial(material))
    {
        ForceSmokeAbsorbingBlackMaterialInfo(*info);
    }
    RefreshSmokeMaterialTextureHandleState(*info);
    // Detail-decal diffuse rescue (splat16 case): some trigger-spawned decals
    // carry a gui-like mask (e.g. guis/assets/white) in their SL_DIFFUSE stage,
    // which the safety filter rejects, leaving no diffuse texture. The visible
    // art is the first explicit SL_AMBIENT stage image - use it instead.
    if (info->detailDecal && material && (!info->hasDiffuseImage || !info->hasSafeTexture))
    {
        for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
        {
            const shaderStage_t* stage = material->GetStage(stageIndex);
            if (!stage || stage->lighting != SL_AMBIENT || !stage->texture.image ||
                stage->texture.texgen != TG_EXPLICIT)
            {
                continue;
            }
            idImage* stageImage = stage->texture.image;
            if (idStr::FindText(stageImage->GetName(), "makealpha", false) >= 0 ||
                !IsSmokeImageNameSafeForRayTracing(stageImage->GetName()) ||
                !stageImage->GetTextureHandle())
            {
                continue;
            }
            info->diffuseImage = stageImage;
            info->diffuseImageName = stageImage->GetName();
            info->diffuseUsage = stageImage->GetUsage();
            info->diffuseColorFormat = stageImage->GetOpts().colorFormat;
            info->hasDiffuseImage = true;
            info->forceFallbackAlbedo = false;
            info->fallbackReason = va("detail-decal ambient-stage diffuse fallback: stage %d", stageIndex);
            // The rescued art stage raster-blends as src-alpha OVER; route the
            // composite the same way instead of filter/modulate, with coverage
            // from the material's makealpha() stage when it has one.
            info->filterDecal = false;
            info->filterDecalBlackKey = false;
            for (int alphaStageIndex = 0; alphaStageIndex < material->GetNumStages(); ++alphaStageIndex)
            {
                const shaderStage_t* alphaStage = material->GetStage(alphaStageIndex);
                if (!alphaStage || alphaStage->lighting != SL_AMBIENT || !alphaStage->texture.image)
                {
                    continue;
                }
                idImage* alphaImage = alphaStage->texture.image;
                if (idStr::FindText(alphaImage->GetName(), "makealpha", false) < 0 ||
                    !alphaImage->GetTextureHandle())
                {
                    continue;
                }
                info->alphaImage = alphaImage;
                info->alphaImageName = alphaImage->GetName();
                info->alphaUsage = alphaImage->GetUsage();
                info->alphaColorFormat = alphaImage->GetOpts().colorFormat;
                info->hasAlphaImage = true;
                info->alphaReason = va("detail-decal makealpha coverage: stage %d", alphaStageIndex);
                break;
            }
            RefreshSmokeMaterialTextureHandleState(*info);
            break;
        }
    }
    ResolveSmokeLiquidFilmCandidateFacts(material, *info);
    if (info->hasDiffuseImage && !info->hasSafeTexture)
    {
        if (diffuseImage && !IsSmokeImageNameSafeForRayTracing(diffuseImage->GetName()))
        {
            info->fallbackReason = va("%s; rejected dynamic image name", reason.c_str());
        }
        else if (diffuseImage && (diffuseImage->GetOpts().isRenderTarget || diffuseImage->GetOpts().isUAV))
        {
            info->fallbackReason = va("%s; rejected image opts rt=%d uav=%d", reason.c_str(), diffuseImage->GetOpts().isRenderTarget ? 1 : 0, diffuseImage->GetOpts().isUAV ? 1 : 0);
        }
        else
        {
            info->fallbackReason = va("%s; rejected texture desc", reason.c_str());
        }
    }
    // Cache the complete conservative classification once when metadata is
    // derived. BLAS signature walks are per triangle and must never copy the
    // large metadata record or repeat string classification.
    info->hardwareOpaqueGeometry =
        ComputeSmokeMaterialHardwareOpaqueGeometry(*info);
    if (PathTraceMaterialClassifierRequested())
    {
        RegisterPathTraceMaterialRecord(material, *info);
    }
    if (residencyEnabled)
    {
        StoreResidentMaterialFacts(materialId, residentMaterialGeneration, *info);
    }
    return true;
}

RtSmokeMaterialMetadataRegistrationTiming RegisterSmokeMaterialTextureInfoForFrame(const viewDef_t* viewDef, bool enabled)
{
    OPTICK_EVENT("PT Material Metadata Frame");

    RtSmokeMaterialMetadataRegistrationTiming timing;
    g_smokeMaterialMetadataFrameStats = RtSmokeMaterialMetadataFrameStats();
    if (PathTraceMaterialClassifierRequested())
    {
        BeginPathTraceMaterialClassifierFrame();
        MaybeDumpPathTraceMaterialDeclSurfaceTypeDistribution();
    }

    const int metadataStartMs = Sys_Milliseconds();
    if (enabled && viewDef)
    {
        OPTICK_EVENT("PT Material Metadata Surface Loop");
        std::vector<uint32_t> registeredMaterialIds;
        registeredMaterialIds.reserve(viewDef->numDrawSurfs);
        for (int surfaceIndex = 0; surfaceIndex < viewDef->numDrawSurfs; ++surfaceIndex)
        {
            const drawSurf_t* drawSurf = viewDef->drawSurfs[surfaceIndex];
            const srfTriangles_t* tri = nullptr;
            const int validationStartMs = Sys_Milliseconds();
            if (!ValidateSmokeDrawSurface(viewDef, drawSurf, tri, nullptr))
            {
                timing.validationMs += Sys_Milliseconds() - validationStartMs;
                continue;
            }
            timing.validationMs += Sys_Milliseconds() - validationStartMs;

            const uint32_t materialId = SmokeMaterialId(drawSurf->material);
            if (std::find(registeredMaterialIds.begin(), registeredMaterialIds.end(), materialId) != registeredMaterialIds.end())
            {
                ++g_smokeMaterialMetadataFrameStats.duplicateSkips;
                continue;
            }

            registeredMaterialIds.push_back(materialId);
            const int registrationStartMs = Sys_Milliseconds();
            RegisterSmokeMaterialTextureInfo(drawSurf->material);
            timing.registrationMs += Sys_Milliseconds() - registrationStartMs;
        }
    }
    timing.metadataMs = Sys_Milliseconds() - metadataStartMs;
    return timing;
}

RtSmokeMaterialMetadataRegistrationTiming RegisterSmokeWorldStaticMaterialTextureInfo(const viewDef_t* viewDef, bool enabled)
{
    OPTICK_EVENT("PT Material Metadata World Static");

    RtSmokeMaterialMetadataRegistrationTiming timing;
    const int metadataStartMs = Sys_Milliseconds();
    if (!enabled || !viewDef || !viewDef->renderWorld)
    {
        timing.metadataMs = Sys_Milliseconds() - metadataStartMs;
        return timing;
    }

    std::vector<uint32_t> registeredMaterialIds;
    registeredMaterialIds.reserve(256);
    idRenderWorldLocal* renderWorld = viewDef->renderWorld;
    for (int entityIndex = 0; entityIndex < renderWorld->entityDefs.Num(); ++entityIndex)
    {
        const idRenderEntityLocal* entity = renderWorld->entityDefs[entityIndex];
        const idRenderModel* model = entity ? entity->parms.hModel : nullptr;
        const bool isStaticWorldModel = model && model->IsStaticWorldModel();
        const renderEntity_t* renderEntity = entity ? &entity->parms : nullptr;
        const bool isRigidEntityModel =
            !isStaticWorldModel &&
            r_pathTracingSceneSource2RigidEntities.GetInteger() != 0 &&
            entity &&
            renderEntity &&
            model &&
            model->IsDynamicModel() == DM_STATIC &&
            renderEntity->joints == nullptr &&
            renderEntity->numJoints <= 0 &&
            renderEntity->callback == nullptr &&
            renderEntity->forceUpdate == 0 &&
            !renderEntity->weaponDepthHack &&
            renderEntity->modelDepthHack == 0.0f;
        if (!model || (!isStaticWorldModel && !isRigidEntityModel))
        {
            continue;
        }

        for (int surfaceIndex = 0; surfaceIndex < model->NumSurfaces(); ++surfaceIndex)
        {
            const modelSurface_t* surface = model->Surface(surfaceIndex);
            const idMaterial* baseMaterial = surface ? surface->shader : nullptr;
            const idMaterial* material = baseMaterial;
            if (entity && baseMaterial && entity->parms.customShader != nullptr)
            {
                material = baseMaterial->Deform() ? nullptr : entity->parms.customShader;
            }
            else if (entity && baseMaterial && entity->parms.customSkin)
            {
                material = entity->parms.customSkin->RemapShaderBySkin(baseMaterial);
            }
            if (!material)
            {
                continue;
            }

            const uint32_t materialId = SmokeMaterialId(material);
            if (std::find(registeredMaterialIds.begin(), registeredMaterialIds.end(), materialId) != registeredMaterialIds.end())
            {
                ++g_smokeMaterialMetadataFrameStats.duplicateSkips;
                continue;
            }

            registeredMaterialIds.push_back(materialId);
            const int registrationStartMs = Sys_Milliseconds();
            RegisterSmokeMaterialTextureInfo(material);
            timing.registrationMs += Sys_Milliseconds() - registrationStartMs;
        }
    }

    timing.metadataMs = Sys_Milliseconds() - metadataStartMs;
    return timing;
}

RtSmokeMaterialMetadataRegistrationTiming RegisterSmokeMaterialTextureInfoForMaterialIds(const std::vector<uint32_t>& materialIds, bool enabled)
{
    OPTICK_EVENT("PT Material Metadata Id Hydration");

    RtSmokeMaterialMetadataRegistrationTiming timing;
    const int metadataStartMs = Sys_Milliseconds();
    if (!enabled || !declManager || materialIds.empty())
    {
        g_smokeMaterialHydrationSetCache = RtSmokeMaterialHydrationSetCache();
        timing.metadataMs = Sys_Milliseconds() - metadataStartMs;
        DumpSmokeMaterialResidencyStatsIfNeeded();
        return timing;
    }
    const bool residencyMaterialEnabled = SmokeMaterialResidencyEnabled();
    if (!residencyMaterialEnabled)
    {
        g_smokeMaterialHydrationSetCache = RtSmokeMaterialHydrationSetCache();
    }
    else
    {
        const uint64 hydrationSetSignature = ComputeSmokeMaterialHydrationSetSignature(materialIds);
        if (g_smokeMaterialHydrationSetCache.valid &&
            g_smokeMaterialHydrationSetCache.materialCount == static_cast<int>(materialIds.size()) &&
            g_smokeMaterialHydrationSetCache.signature == hydrationSetSignature)
        {
            OPTICK_EVENT("PT Material Metadata Cached Refresh Subset");
            bool refreshSubsetValid = true;
            std::vector<uint32_t> nextRefreshMaterialIds;
            nextRefreshMaterialIds.reserve(g_smokeMaterialHydrationSetCache.refreshMaterialIds.size());
            for (uint32_t materialId : g_smokeMaterialHydrationSetCache.refreshMaterialIds)
            {
                if (materialId == 0u)
                {
                    continue;
                }
                ++g_smokeMaterialMetadataFrameStats.idHydrationVisited;
                RtSmokeMaterialTextureInfo* existing = FindSmokeMaterialTextureInfo(materialId);
                if (!existing)
                {
                    refreshSubsetValid = false;
                    break;
                }
                ++g_smokeMaterialMetadataFrameStats.idHydrationCacheHits;
                const bool refreshed = RefreshSmokeMaterialTextureHandleState(*existing);
                if (refreshed || SmokeMaterialTextureInfoNeedsHydrationRefresh(*existing))
                {
                    ++g_smokeMaterialMetadataFrameStats.idHydrationRefreshes;
                }
                else
                {
                    ++g_smokeMaterialMetadataFrameStats.idHydrationRefreshSkips;
                }
                if (!SmokeMaterialTextureInfoCanSkipStaticHydrationRefresh(*existing))
                {
                    nextRefreshMaterialIds.push_back(materialId);
                }
            }
            if (refreshSubsetValid)
            {
                g_smokeMaterialHydrationSetCache.signature = ComputeSmokeMaterialHydrationSetSignature(materialIds);
                g_smokeMaterialHydrationSetCache.refreshMaterialIds = nextRefreshMaterialIds;
                timing.metadataMs = Sys_Milliseconds() - metadataStartMs;
                DumpSmokeMaterialResidencyStatsIfNeeded();
                return timing;
            }
            g_smokeMaterialHydrationSetCache = RtSmokeMaterialHydrationSetCache();
        }
    }

    std::unordered_set<uint32_t> visitedMaterialIds;
    visitedMaterialIds.reserve(materialIds.size());
    std::unordered_set<uint32_t> missingMaterialIds;
    missingMaterialIds.reserve(materialIds.size());
    std::vector<uint32_t> refreshMaterialIds;
    for (uint32_t materialId : materialIds)
    {
        if (materialId == 0u || !visitedMaterialIds.insert(materialId).second)
        {
            continue;
        }
        ++g_smokeMaterialMetadataFrameStats.idHydrationVisited;
        RtSmokeMaterialTextureInfo* existing = FindSmokeMaterialTextureInfo(materialId);
        if (existing)
        {
            ++g_smokeMaterialMetadataFrameStats.idHydrationCacheHits;
            const bool skipRefresh = SmokeMaterialTextureInfoCanSkipStaticHydrationRefresh(*existing);
            const bool refreshed = skipRefresh ? false : RefreshSmokeMaterialTextureHandleState(*existing);
            if (refreshed || SmokeMaterialTextureInfoNeedsHydrationRefresh(*existing))
            {
                ++g_smokeMaterialMetadataFrameStats.idHydrationRefreshes;
            }
            else
            {
                ++g_smokeMaterialMetadataFrameStats.idHydrationRefreshSkips;
            }
            if (!SmokeMaterialTextureInfoCanSkipStaticHydrationRefresh(*existing))
            {
                refreshMaterialIds.push_back(materialId);
            }
            continue;
        }
        ++g_smokeMaterialMetadataFrameStats.idHydrationCacheMisses;
        missingMaterialIds.insert(materialId);
    }

    if (missingMaterialIds.empty())
    {
        if (residencyMaterialEnabled)
        {
            UpdateSmokeMaterialHydrationSetCache(materialIds, refreshMaterialIds);
        }
        timing.metadataMs = Sys_Milliseconds() - metadataStartMs;
        DumpSmokeMaterialResidencyStatsIfNeeded();
        return timing;
    }

    const int declCount = declManager->GetNumDecls(DECL_MATERIAL);
    for (int declIndex = 0; declIndex < declCount && !missingMaterialIds.empty(); ++declIndex)
    {
        const idDecl* decl = declManager->DeclByIndex(DECL_MATERIAL, declIndex, false);
        const idMaterial* material = static_cast<const idMaterial*>(decl);
        if (!material)
        {
            continue;
        }

        const uint32_t materialId = SmokeMaterialId(material);
        std::unordered_set<uint32_t>::iterator missing = missingMaterialIds.find(materialId);
        if (missing == missingMaterialIds.end())
        {
            continue;
        }

        const int registrationStartMs = Sys_Milliseconds();
        const bool fullDiscover = RegisterSmokeMaterialTextureInfo(material);
        if (fullDiscover)
        {
            ++g_smokeMaterialMetadataFrameStats.idHydrationDerived;
        }
        else
        {
            ++g_smokeMaterialMetadataFrameStats.idHydrationCacheHits;
            if (g_smokeMaterialMetadataFrameStats.idHydrationCacheMisses > 0)
            {
                --g_smokeMaterialMetadataFrameStats.idHydrationCacheMisses;
            }
            RtSmokeMaterialTextureInfo* existing = FindSmokeMaterialTextureInfo(materialId);
            if (existing && !SmokeMaterialTextureInfoCanSkipStaticHydrationRefresh(*existing))
            {
                refreshMaterialIds.push_back(materialId);
            }
        }
        timing.registrationMs += Sys_Milliseconds() - registrationStartMs;
        missingMaterialIds.erase(missing);
    }

    if (residencyMaterialEnabled && missingMaterialIds.empty())
    {
        UpdateSmokeMaterialHydrationSetCache(materialIds, refreshMaterialIds);
    }

    timing.metadataMs = Sys_Milliseconds() - metadataStartMs;
    DumpSmokeMaterialResidencyStatsIfNeeded();
    return timing;
}
