#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCVars.h"
#include "PathTraceTextureRegistry.h"
#include "../Image.h"

#include <unordered_map>


namespace {

std::vector<RtSmokeMaterialTextureInfo> g_smokeMaterialTextureRegistry;
std::unordered_map<uint32_t, int> g_smokeMaterialTextureRegistryLookup;
std::unordered_map<uint32_t, uint32_t> g_smokeMaterialTextureVariantBases;
uint64 g_smokeMaterialTextureRegistryGeneration = 1;

}

bool IsSmokeDiffuseTextureSafeForRayTracing(nvrhi::ITexture* texture)
{
    if (!texture)
    {
        return false;
    }

    const nvrhi::TextureDesc& desc = texture->getDesc();
    if (!desc.isShaderResource || desc.isRenderTarget || desc.isUAV)
    {
        return false;
    }
    if (desc.dimension != nvrhi::TextureDimension::Texture2D || desc.sampleCount != 1)
    {
        return false;
    }

    switch (desc.format)
    {
        case nvrhi::Format::UNKNOWN:
        case nvrhi::Format::R8_UINT:
        case nvrhi::Format::R8_SINT:
        case nvrhi::Format::RG8_UINT:
        case nvrhi::Format::RG8_SINT:
        case nvrhi::Format::R16_UINT:
        case nvrhi::Format::R16_SINT:
        case nvrhi::Format::R32_UINT:
        case nvrhi::Format::R32_SINT:
        case nvrhi::Format::RG16_UINT:
        case nvrhi::Format::RG16_SINT:
        case nvrhi::Format::RG32_UINT:
        case nvrhi::Format::RG32_SINT:
        case nvrhi::Format::RGB32_UINT:
        case nvrhi::Format::RGB32_SINT:
        case nvrhi::Format::RGBA8_UINT:
        case nvrhi::Format::RGBA8_SINT:
        case nvrhi::Format::RGBA16_UINT:
        case nvrhi::Format::RGBA16_SINT:
        case nvrhi::Format::RGBA32_UINT:
        case nvrhi::Format::RGBA32_SINT:
        case nvrhi::Format::D16:
        case nvrhi::Format::D24S8:
        case nvrhi::Format::X24G8_UINT:
        case nvrhi::Format::D32:
        case nvrhi::Format::D32S8:
        case nvrhi::Format::X32G8_UINT:
            return false;
        default:
            return true;
    }
}

bool IsSmokeImageNameSafeForRayTracing(const char* imageName)
{
    if (!imageName || !imageName[0])
    {
        return false;
    }

    idStr name = imageName;
    name.BackSlashesToSlashes();

    // Runtime GUI/cinematic/scratch images can be render-target backed or replaced
    // while in-world terminals redraw. Keep mode 8 on stable material textures only.
    if (name[0] == '_' ||
        name.Icmpn("guis/", 5) == 0 ||
        name.Icmpn("gui/", 4) == 0 ||
        name.Icmpn("video/", 6) == 0 ||
        name.Icmpn("videos/", 7) == 0 ||
        name.Icmpn("cinematics/", 11) == 0 ||
        name.Icmpn("generated/", 10) == 0 ||
        name.Find("cinematic", false) >= 0 ||
        name.Find("scratch", false) >= 0 ||
        name.Find("render", false) >= 0)
    {
        return false;
    }

    return true;
}

bool IsSmokeImageNameGuiLike(const char* imageName)
{
    if (!imageName || !imageName[0])
    {
        return false;
    }

    idStr name = imageName;
    name.BackSlashesToSlashes();

    return name[0] == '_' ||
        name.Icmpn("guis/", 5) == 0 ||
        name.Icmpn("gui/", 4) == 0 ||
        name.Icmpn("video/", 6) == 0 ||
        name.Icmpn("videos/", 7) == 0 ||
        name.Icmpn("cinematics/", 11) == 0 ||
        name.Icmpn("generated/", 10) == 0 ||
        name.Find("cinematic", false) >= 0 ||
        name.Find("scratch", false) >= 0 ||
        name.Find("render", false) >= 0 ||
        name.Find(".swf", false) >= 0;
}

bool IsSmokeDiffuseImageSafeForRayTracing(idImage* image)
{
    if (!image)
    {
        return false;
    }

    const idImageOpts& opts = image->GetOpts();
    const bool guiTextureOverride =
        r_pathTracingAllowGuiTextures.GetInteger() != 0 &&
        IsSmokeImageNameGuiLike(image->GetName());
    if (opts.samples != 1 || opts.textureType != DTT_2D)
    {
        return false;
    }

    if ((opts.isRenderTarget || opts.isUAV) && !guiTextureOverride)
    {
        return false;
    }

    if (!IsSmokeImageNameSafeForRayTracing(image->GetName()) &&
        !guiTextureOverride)
    {
        return false;
    }

    return IsSmokeDiffuseTextureSafeForRayTracing(image->GetTextureHandle());
}

bool IsSmokeTextureHandleSafeForDescriptor(nvrhi::TextureHandle texture)
{
    return texture && IsSmokeDiffuseTextureSafeForRayTracing(texture);
}

bool SmokeTextureHandleListsEqual(const std::vector<nvrhi::TextureHandle>& lhs, const std::vector<nvrhi::TextureHandle>& rhs)
{
    if (lhs.size() != rhs.size())
    {
        return false;
    }

    for (int i = 0; i < static_cast<int>(lhs.size()); ++i)
    {
        if (lhs[i].Get() != rhs[i].Get())
        {
            return false;
        }
    }

    return true;
}

RtSmokeMaterialTextureInfo* FindSmokeMaterialTextureInfo(uint32_t materialId)
{
    std::unordered_map<uint32_t, int>::const_iterator lookup = g_smokeMaterialTextureRegistryLookup.find(materialId);
    if (lookup == g_smokeMaterialTextureRegistryLookup.end())
    {
        return nullptr;
    }

    const int index = lookup->second;
    if (index < 0 || index >= static_cast<int>(g_smokeMaterialTextureRegistry.size()))
    {
        return nullptr;
    }

    RtSmokeMaterialTextureInfo& info = g_smokeMaterialTextureRegistry[index];
    return info.materialId == materialId ? &info : nullptr;
}

RtSmokeMaterialTextureInfo& AddSmokeMaterialTextureInfo(uint32_t materialId, const char* materialName)
{
    RtSmokeMaterialTextureInfo newInfo;
    newInfo.materialId = materialId;
    newInfo.materialName = materialName ? materialName : "<none>";
    g_smokeMaterialTextureRegistry.push_back(newInfo);
    g_smokeMaterialTextureRegistryLookup[materialId] = static_cast<int>(g_smokeMaterialTextureRegistry.size() - 1);
    ++g_smokeMaterialTextureRegistryGeneration;
    return g_smokeMaterialTextureRegistry.back();
}

bool RegisterSmokeMaterialTextureVariant(uint32_t variantMaterialId, uint32_t baseMaterialId)
{
    if (variantMaterialId == 0u || baseMaterialId == 0u || variantMaterialId == baseMaterialId)
    {
        return false;
    }

    const RtSmokeMaterialTextureInfo* baseInfo = FindSmokeMaterialTextureInfo(baseMaterialId);
    if (!baseInfo)
    {
        return false;
    }

    RtSmokeMaterialTextureInfo* existing = FindSmokeMaterialTextureInfo(variantMaterialId);
    if (existing)
    {
        const std::unordered_map<uint32_t, uint32_t>::const_iterator variant = g_smokeMaterialTextureVariantBases.find(variantMaterialId);
        if (variant == g_smokeMaterialTextureVariantBases.end() || variant->second != baseMaterialId)
        {
            return false;
        }
        const bool liquidFilmFactsChanged =
            existing->liquidFilmHasBloodSemantic != baseInfo->liquidFilmHasBloodSemantic ||
            existing->liquidFilmHasWetReflectStage != baseInfo->liquidFilmHasWetReflectStage ||
            existing->liquidFilmHasCoverageSource != baseInfo->liquidFilmHasCoverageSource ||
            existing->liquidFilmHasWetNormalSource != baseInfo->liquidFilmHasWetNormalSource ||
            existing->liquidFilmExactOverride != baseInfo->liquidFilmExactOverride ||
            existing->liquidFilmCandidate != baseInfo->liquidFilmCandidate ||
            existing->liquidFilmCoverageImageName != baseInfo->liquidFilmCoverageImageName ||
            existing->liquidFilmOverrideReason != baseInfo->liquidFilmOverrideReason ||
            existing->liquidFilmReason != baseInfo->liquidFilmReason;
        existing->liquidFilmHasBloodSemantic = baseInfo->liquidFilmHasBloodSemantic;
        existing->liquidFilmHasWetReflectStage = baseInfo->liquidFilmHasWetReflectStage;
        existing->liquidFilmHasCoverageSource = baseInfo->liquidFilmHasCoverageSource;
        existing->liquidFilmHasWetNormalSource = baseInfo->liquidFilmHasWetNormalSource;
        existing->liquidFilmExactOverride = baseInfo->liquidFilmExactOverride;
        existing->liquidFilmCandidate = baseInfo->liquidFilmCandidate;
        existing->liquidFilmCoverageImageName = baseInfo->liquidFilmCoverageImageName;
        existing->liquidFilmOverrideReason = baseInfo->liquidFilmOverrideReason;
        existing->liquidFilmReason = baseInfo->liquidFilmReason;
        existing->hardwareOpaqueGeometry =
            ComputeSmokeMaterialHardwareOpaqueGeometry(*existing);
        if (liquidFilmFactsChanged)
        {
            ++g_smokeMaterialTextureRegistryGeneration;
        }
        RefreshSmokeMaterialTextureHandleState(*existing);
        return true;
    }

    RtSmokeMaterialTextureInfo variantInfo = *baseInfo;
    variantInfo.materialId = variantMaterialId;
    variantInfo.tableIndex = -1;
    g_smokeMaterialTextureRegistry.push_back(variantInfo);
    g_smokeMaterialTextureRegistryLookup[variantMaterialId] = static_cast<int>(g_smokeMaterialTextureRegistry.size() - 1);
    g_smokeMaterialTextureVariantBases[variantMaterialId] = baseMaterialId;
    ++g_smokeMaterialTextureRegistryGeneration;
    return true;
}

bool IsSmokeMaterialTextureVariant(uint32_t materialId)
{
    return g_smokeMaterialTextureVariantBases.find(materialId) != g_smokeMaterialTextureVariantBases.end();
}

uint32_t SmokeMaterialTextureVariantBase(uint32_t materialId)
{
    const std::unordered_map<uint32_t, uint32_t>::const_iterator variant = g_smokeMaterialTextureVariantBases.find(materialId);
    return variant != g_smokeMaterialTextureVariantBases.end() ? variant->second : materialId;
}

int ClearSmokeMaterialTextureVariants()
{
    if (g_smokeMaterialTextureVariantBases.empty())
    {
        return 0;
    }

    const int removedCount = static_cast<int>(g_smokeMaterialTextureVariantBases.size());
    std::vector<RtSmokeMaterialTextureInfo> retainedRegistry;
    retainedRegistry.reserve(g_smokeMaterialTextureRegistry.size() - g_smokeMaterialTextureVariantBases.size());
    for (const RtSmokeMaterialTextureInfo& info : g_smokeMaterialTextureRegistry)
    {
        if (g_smokeMaterialTextureVariantBases.find(info.materialId) == g_smokeMaterialTextureVariantBases.end())
        {
            retainedRegistry.push_back(info);
        }
    }

    g_smokeMaterialTextureRegistry.swap(retainedRegistry);
    g_smokeMaterialTextureRegistryLookup.clear();
    g_smokeMaterialTextureRegistryLookup.reserve(g_smokeMaterialTextureRegistry.size());
    for (int index = 0; index < static_cast<int>(g_smokeMaterialTextureRegistry.size()); ++index)
    {
        g_smokeMaterialTextureRegistryLookup[g_smokeMaterialTextureRegistry[index].materialId] = index;
    }
    g_smokeMaterialTextureVariantBases.clear();
    ++g_smokeMaterialTextureRegistryGeneration;
    return removedCount;
}

int ClearSmokeMaterialTextureRegistry()
{
    const int removedCount = static_cast<int>(g_smokeMaterialTextureRegistry.size());
    g_smokeMaterialTextureRegistry.clear();
    g_smokeMaterialTextureRegistryLookup.clear();
    g_smokeMaterialTextureVariantBases.clear();
    ++g_smokeMaterialTextureRegistryGeneration;
    return removedCount;
}

bool RefreshSmokeMaterialTextureHandleState(RtSmokeMaterialTextureInfo& info)
{
    const bool oldHasTextureHandle = info.hasTextureHandle;
    const bool oldHasAlphaTextureHandle = info.hasAlphaTextureHandle;
    const bool oldHasNormalTextureHandle = info.hasNormalTextureHandle;
    const bool oldHasSpecularTextureHandle = info.hasSpecularTextureHandle;
    const bool oldHasEmissiveTextureHandle = info.hasEmissiveTextureHandle;
    const bool oldHasSkyTextureHandle = info.hasSkyTextureHandle;
    const bool oldHasSafeTexture = info.hasSafeTexture;
    const bool oldHasSafeAlphaTexture = info.hasSafeAlphaTexture;
    const bool oldHasSafeNormalTexture = info.hasSafeNormalTexture;
    const bool oldHasSafeSpecularTexture = info.hasSafeSpecularTexture;
    const bool oldHasSafeEmissiveTexture = info.hasSafeEmissiveTexture;
    const bool oldHasSafeSkyTexture = info.hasSafeSkyTexture;
    const nvrhi::TextureHandle oldDiffuseTexture = info.diffuseTexture;
    const nvrhi::TextureHandle oldAlphaTexture = info.alphaTexture;
    const nvrhi::TextureHandle oldNormalTexture = info.normalTexture;
    const nvrhi::TextureHandle oldSpecularTexture = info.specularTexture;
    const nvrhi::TextureHandle oldEmissiveTexture = info.emissiveTexture;
    const nvrhi::TextureHandle oldSkyTexture = info.skyTexture;

    info.diffuseTexture = info.diffuseImage ? info.diffuseImage->GetTextureHandle() : nullptr;
    info.alphaTexture = info.alphaImage ? info.alphaImage->GetTextureHandle() : nullptr;
    info.normalTexture = info.normalImage ? info.normalImage->GetTextureHandle() : nullptr;
    info.specularTexture = info.specularImage ? info.specularImage->GetTextureHandle() : nullptr;
    info.emissiveTexture = info.emissiveImage ? info.emissiveImage->GetTextureHandle() : nullptr;
    info.skyTexture = info.skyImage ? info.skyImage->GetTextureHandle() : nullptr;
    info.hasTextureHandle = info.diffuseTexture != nullptr;
    info.hasAlphaTextureHandle = info.alphaTexture != nullptr;
    info.hasNormalTextureHandle = info.normalTexture != nullptr;
    info.hasSpecularTextureHandle = info.specularTexture != nullptr;
    info.hasEmissiveTextureHandle = info.emissiveTexture != nullptr;
    info.hasSkyTextureHandle = info.skyTexture != nullptr;
    info.hasSafeTexture = info.hasTextureHandle && IsSmokeDiffuseImageSafeForRayTracing(info.diffuseImage);
    info.hasSafeAlphaTexture = info.hasAlphaTextureHandle && IsSmokeDiffuseImageSafeForRayTracing(info.alphaImage);
    info.hasSafeNormalTexture = info.hasNormalTextureHandle && IsSmokeDiffuseImageSafeForRayTracing(info.normalImage);
    info.hasSafeSpecularTexture = info.hasSpecularTextureHandle && IsSmokeDiffuseImageSafeForRayTracing(info.specularImage);
    info.hasSafeEmissiveTexture = info.hasEmissiveTextureHandle && IsSmokeDiffuseImageSafeForRayTracing(info.emissiveImage);
    info.hasSafeSkyTexture =
        info.hasSkyTextureHandle &&
        info.skyImage &&
        info.skyImage->GetOpts().textureType == DTT_CUBIC &&
        info.skyTexture->getDesc().dimension == nvrhi::TextureDimension::TextureCube;

    const bool changed =
        oldHasTextureHandle != info.hasTextureHandle ||
        oldHasAlphaTextureHandle != info.hasAlphaTextureHandle ||
        oldHasNormalTextureHandle != info.hasNormalTextureHandle ||
        oldHasSpecularTextureHandle != info.hasSpecularTextureHandle ||
        oldHasEmissiveTextureHandle != info.hasEmissiveTextureHandle ||
        oldHasSkyTextureHandle != info.hasSkyTextureHandle ||
        oldHasSafeTexture != info.hasSafeTexture ||
        oldHasSafeAlphaTexture != info.hasSafeAlphaTexture ||
        oldHasSafeNormalTexture != info.hasSafeNormalTexture ||
        oldHasSafeSpecularTexture != info.hasSafeSpecularTexture ||
        oldHasSafeEmissiveTexture != info.hasSafeEmissiveTexture ||
        oldHasSafeSkyTexture != info.hasSafeSkyTexture ||
        oldDiffuseTexture.Get() != info.diffuseTexture.Get() ||
        oldAlphaTexture.Get() != info.alphaTexture.Get() ||
        oldNormalTexture.Get() != info.normalTexture.Get() ||
        oldSpecularTexture.Get() != info.specularTexture.Get() ||
        oldEmissiveTexture.Get() != info.emissiveTexture.Get() ||
        oldSkyTexture.Get() != info.skyTexture.Get();
    if (changed)
    {
        ++g_smokeMaterialTextureRegistryGeneration;
    }
    return changed;
}

RtSmokeMaterialTextureInfo ResolveSmokeMaterialTextureInfo(uint32_t materialId, int tableIndex)
{
    const RtSmokeMaterialTextureInfo* existing = FindSmokeMaterialTextureInfo(materialId);
    if (existing)
    {
        RtSmokeMaterialTextureInfo resolved = *existing;
        resolved.tableIndex = tableIndex;
        return resolved;
    }

    RtSmokeMaterialTextureInfo missing;
    missing.materialId = materialId;
    missing.tableIndex = tableIndex;
    missing.materialName = "<unseen material>";
    missing.diffuseImageName = "<none>";
    missing.fallbackReason = "material metadata not seen this session";
    return missing;
}

bool ComputeSmokeMaterialHardwareOpaqueGeometry(
    const RtSmokeMaterialTextureInfo& info)
{
    if (!SmokeMaterialTextureInfoHasMaterialMetadata(info) ||
        info.coverage != MC_OPAQUE)
    {
        return false;
    }

    // Any material whose visibility can depend on authored coverage,
    // transmission, or modifier semantics must remain programmable.  This is
    // deliberately conservative: an uncertain material costs performance but
    // can never turn into a false hardware-opaque blocker.
    return !info.hasAlphaTest &&
        !info.additiveDecal &&
        !info.additiveDecalWhiteKey &&
        !info.filterDecal &&
        !info.filterDecalBlackKey &&
        !info.detailDecal &&
        !info.detailDecalDynamic &&
        !info.detailDecalLiquidPool &&
        !info.liquidFilmCandidate &&
        !info.alphaFromDiffuseLuma &&
        !info.alphaFromDiffuseDarkKey &&
        !info.alphaFromDiffuseMagentaKey &&
        !info.portalWindowFallback &&
        !info.objectGlassFallback &&
        !IsSmokeImageNameGuiLike(info.diffuseImageName.c_str()) &&
        !IsSmokeImageNameGuiLike(info.alphaImageName.c_str());
}

bool SmokeMaterialCanUseHardwareOpaqueGeometry(uint32_t materialId)
{
    if (materialId == UINT32_MAX)
    {
        return false;
    }

    const RtSmokeMaterialTextureInfo* info =
        FindSmokeMaterialTextureInfo(materialId);
    return info && info->hardwareOpaqueGeometry;
}

bool SmokeMaterialTextureInfoHasMaterialMetadata(const RtSmokeMaterialTextureInfo& info)
{
    return !info.materialName.IsEmpty() &&
        info.materialName.Icmp("<unseen material>") != 0 &&
        info.materialName.Icmp("<none>") != 0;
}

const idStr& SmokeBestSafeTextureName(const RtSmokeMaterialTextureInfo& info)
{
    if (info.hasSafeTexture)
    {
        return info.diffuseImageName;
    }
    if (info.hasSafeAlphaTexture)
    {
        return info.alphaImageName;
    }
    if (info.hasSafeNormalTexture)
    {
        return info.normalImageName;
    }
    if (info.hasSafeSpecularTexture)
    {
        return info.specularImageName;
    }
    return info.emissiveImageName;
}

int SmokeMaterialTextureRegistrySize()
{
    return static_cast<int>(g_smokeMaterialTextureRegistry.size());
}

uint64 SmokeMaterialTextureRegistryGeneration()
{
    return g_smokeMaterialTextureRegistryGeneration;
}
