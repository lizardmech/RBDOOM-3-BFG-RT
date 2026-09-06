#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCVars.h"
#include "PathTraceTextureRegistry.h"
#include "PathTraceMaterialFrameKernel.h"
#include "PathTraceCaptureProduct.h"
#include "../Image.h"

#include <unordered_map>
#include <map>
#include <array>
#include <tuple>
#include <unordered_set>
#include <new>
#include <stdexcept>
#include <type_traits>


namespace {

std::vector<RtSmokeMaterialTextureInfo> g_smokeMaterialTextureRegistry;
std::unordered_map<uint32_t, int> g_smokeMaterialTextureRegistryLookup;
std::unordered_map<uint32_t, uint32_t> g_smokeMaterialTextureVariantBases;
uint64 g_smokeMaterialTextureRegistryGeneration = 1;
uint64_t g_bindingDefinitionRevision = 1, g_bindingMembershipRevision = 1;
RtSmokeMaterialTextureRegistryBumpStats g_smokeMaterialTextureRegistryBumpStats;

using SmokeMaterialRegistry = std::vector<RtSmokeMaterialTextureInfo>;
using SmokeMaterialRegistryLookup = std::unordered_map<uint32_t, int>;
using SmokeMaterialVariantBases = std::unordered_map<uint32_t, uint32_t>;

static_assert(std::is_nothrow_move_constructible<RtSmokeMaterialTextureInfo>::value,
    "registry row publication requires a nonthrowing move");
static_assert(std::is_nothrow_swappable<SmokeMaterialRegistry>::value,
    "registry publication requires a nonthrowing vector swap");
static_assert(std::is_nothrow_swappable<SmokeMaterialRegistryLookup>::value,
    "registry publication requires a nonthrowing lookup swap");
static_assert(std::is_nothrow_swappable<SmokeMaterialVariantBases>::value,
    "registry publication requires a nonthrowing ownership swap");
static_assert(std::is_nothrow_swappable<RtSmokeMaterialTextureInfo>::value,
    "existing registry row replacement requires a nonthrowing swap");
static_assert(noexcept(std::declval<SmokeMaterialRegistryLookup::hasher>()(
        std::declval<uint32_t>())),
    "lookup node publication requires a nonthrowing hash");
static_assert(noexcept(std::declval<SmokeMaterialRegistryLookup::key_equal>()(
        std::declval<uint32_t>(), std::declval<uint32_t>())),
    "lookup node publication requires nonthrowing equality");

#if defined(RT_PT_TEXTURE_REGISTRY_HARNESS)
enum class AtomicPublishFailurePoint
{
    None,
    VectorReserve,
    LookupReserve,
    VariantBaseReserve,
    LookupNodeStage,
    VariantBaseNodeStage,
    LookupInsert,
    VariantBaseInsert,
    FinalPublish
};
AtomicPublishFailurePoint g_atomicPublishFailurePoint =
    AtomicPublishFailurePoint::None;

void MaybeFailAtomicPublish(AtomicPublishFailurePoint point)
{
    if (g_atomicPublishFailurePoint == point)
    {
        throw std::bad_alloc();
    }
}
#else
enum class AtomicPublishFailurePoint
{
    VectorReserve,
    LookupReserve,
    VariantBaseReserve,
    LookupNodeStage,
    VariantBaseNodeStage,
    LookupInsert,
    VariantBaseInsert,
    FinalPublish
};
void MaybeFailAtomicPublish(AtomicPublishFailurePoint)
{
}
#endif

enum class RtSmokeMaterialTextureRegistryBumpSite
{
    AddMaterial,
    UpdateVariantFacts,
    AddVariant,
    ClearVariants,
    ClearRegistry,
    RefreshTextureHandles
};

bool SmokeMaterialTextureRegistryBumpTelemetryEnabled()
{
#if defined(RT_PT_TEXTURE_REGISTRY_HARNESS)
    return false;
#else
    return r_pathTracingResidency.GetInteger() != 0 &&
        r_pathTracingResidencyMaterial.GetInteger() != 0 &&
        r_pathTracingResidencyDump.GetInteger() != 0;
#endif
}

void AdvanceSmokeMaterialTextureRegistryBumpFrame()
{
#if defined(RT_PT_TEXTURE_REGISTRY_HARNESS)
    return;
#else
    if (g_smokeMaterialTextureRegistryBumpStats.frameNumber == tr.frameCount)
    {
        return;
    }
    g_smokeMaterialTextureRegistryBumpStats.frameNumber = tr.frameCount;
    g_smokeMaterialTextureRegistryBumpStats.thisFrame = RtSmokeMaterialTextureRegistryBumpCounts();
#endif
}

void IncrementSmokeMaterialTextureRegistryBumpCount(
    RtSmokeMaterialTextureRegistryBumpCounts& counts,
    RtSmokeMaterialTextureRegistryBumpSite site)
{
    switch (site)
    {
        case RtSmokeMaterialTextureRegistryBumpSite::AddMaterial:
            ++counts.addMaterial;
            break;
        case RtSmokeMaterialTextureRegistryBumpSite::UpdateVariantFacts:
            ++counts.updateVariantFacts;
            break;
        case RtSmokeMaterialTextureRegistryBumpSite::AddVariant:
            ++counts.addVariant;
            break;
        case RtSmokeMaterialTextureRegistryBumpSite::ClearVariants:
            ++counts.clearVariants;
            break;
        case RtSmokeMaterialTextureRegistryBumpSite::ClearRegistry:
            ++counts.clearRegistry;
            break;
        case RtSmokeMaterialTextureRegistryBumpSite::RefreshTextureHandles:
            ++counts.refreshTextureHandles;
            break;
    }
}

void RecordSmokeMaterialTextureRegistryBump(RtSmokeMaterialTextureRegistryBumpSite site)
{
    if (!SmokeMaterialTextureRegistryBumpTelemetryEnabled())
    {
        return;
    }
    AdvanceSmokeMaterialTextureRegistryBumpFrame();
    g_smokeMaterialTextureRegistryBumpStats.enabled = true;
    IncrementSmokeMaterialTextureRegistryBumpCount(
        g_smokeMaterialTextureRegistryBumpStats.thisFrame,
        site);
    IncrementSmokeMaterialTextureRegistryBumpCount(
        g_smokeMaterialTextureRegistryBumpStats.cumulative,
        site);
}

template<typename Map>
void EraseSmokeMaterialNodeNoThrow(Map& map, typename Map::iterator iterator) noexcept
{
    // [unord.req] guarantees iterator erase does not throw. Using the retained
    // iterator avoids a second hash/equality call during rollback.
    map.erase(iterator);
}

void FinishSmokeMaterialRegistryPublish(
    RtSmokeMaterialTextureRegistryBumpSite site) noexcept
{
    ++g_smokeMaterialTextureRegistryGeneration;
    // Bump counters are diagnostic only. They are intentionally outside the
    // rendering-semantic row/lookup/ownership transaction.
    RecordSmokeMaterialTextureRegistryBump(site);
}

}

static bool RefreshSmokeMaterialTextureHandleStateImpl(
    RtSmokeMaterialTextureInfo& info,
    bool publishGeneration, const RtSmokeMaterialBindingFrame* frame = nullptr, uint32_t base = 0);

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

bool IsSmokeDiffuseImageSafeForRayTracingWithHandle(
    idImage* image,
    const nvrhi::TextureHandle& texture)
{
    if (!image)
    {
        return false;
    }

    const idImageOpts& opts = image->GetOpts();
    const bool guiTextureOverride =
#if defined(RT_PT_TEXTURE_REGISTRY_HARNESS)
        false;
#else
        r_pathTracingAllowGuiTextures.GetInteger() != 0 &&
        IsSmokeImageNameGuiLike(image->GetName());
#endif
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

    return IsSmokeDiffuseTextureSafeForRayTracing(texture);
}

bool IsSmokeMaterialTextureSnapshotSafeForRayTracing(
    const nvrhi::TextureHandle& texture,
    const idStr& storedImageName,
    nvrhi::TextureDimension requiredDimension)
{
    if (!texture)
    {
        return false;
    }
    const bool guiTextureOverride =
#if defined(RT_PT_TEXTURE_REGISTRY_HARNESS)
        false;
#else
        r_pathTracingAllowGuiTextures.GetInteger() != 0 &&
        IsSmokeImageNameGuiLike(storedImageName.c_str());
#endif
    if (!IsSmokeImageNameSafeForRayTracing(storedImageName.c_str()) &&
        !guiTextureOverride)
    {
        return false;
    }
    const nvrhi::TextureDesc& desc = texture->getDesc();
    if (!desc.isShaderResource || desc.isRenderTarget || desc.isUAV ||
        desc.sampleCount != 1 || desc.dimension != requiredDimension)
    {
        return false;
    }
    return requiredDimension == nvrhi::TextureDimension::Texture2D
        ? IsSmokeDiffuseTextureSafeForRayTracing(texture)
        : true;
}

bool IsSmokeDiffuseImageSafeForRayTracing(idImage* image)
{
    return IsSmokeDiffuseImageSafeForRayTracingWithHandle(
        image,
        image ? image->GetTextureHandle() : nullptr);
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

static RtSmokeMaterialTextureInfo* LookupSmokeMaterialTextureInfo(uint32_t materialId)
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

RtSmokeMaterialTextureInfo* FindSmokeMaterialTextureInfo(uint32_t materialId)
{
    auto* info = LookupSmokeMaterialTextureInfo(materialId);
    if (info) info->bindingMutableExposed = true;
    return info;
}

const RtSmokeMaterialTextureInfo* FindSmokeMaterialTextureInfoReadOnly(uint32_t materialId)
{
    return LookupSmokeMaterialTextureInfo(materialId);
}

static bool SmokeBindingDefinitionEqual(const RtSmokeMaterialTextureInfo& a,
    const RtSmokeMaterialTextureInfo& b)
{
    return a.materialId == b.materialId &&
        a.diffuseImage == b.diffuseImage && a.diffuseImageName == b.diffuseImageName &&
        a.alphaImage == b.alphaImage && a.alphaImageName == b.alphaImageName &&
        a.normalImage == b.normalImage && a.normalImageName == b.normalImageName &&
        a.specularImage == b.specularImage && a.specularImageName == b.specularImageName &&
        a.emissiveImage == b.emissiveImage && a.emissiveImageName == b.emissiveImageName &&
        a.skyImage == b.skyImage && a.skyImageName == b.skyImageName;
}

bool SmokeMaterialTextureInfoEqual(const RtSmokeMaterialTextureInfo& a,
    const RtSmokeMaterialTextureInfo& b)
{
    return a.materialId == b.materialId
        && a.materialName == b.materialName
        && a.diffuseImageName == b.diffuseImageName
        && a.alphaImageName == b.alphaImageName
        && a.normalImageName == b.normalImageName
        && a.specularImageName == b.specularImageName
        && a.emissiveImageName == b.emissiveImageName
        && a.skyImageName == b.skyImageName
        && a.fallbackReason == b.fallbackReason
        && a.alphaReason == b.alphaReason
        && a.normalReason == b.normalReason
        && a.specularReason == b.specularReason
        && a.emissiveReason == b.emissiveReason
        && a.diffuseImage == b.diffuseImage
        && a.alphaImage == b.alphaImage
        && a.normalImage == b.normalImage
        && a.specularImage == b.specularImage
        && a.emissiveImage == b.emissiveImage
        && a.skyImage == b.skyImage
        && a.diffuseTexture == b.diffuseTexture
        && a.alphaTexture == b.alphaTexture
        && a.normalTexture == b.normalTexture
        && a.specularTexture == b.specularTexture
        && a.emissiveTexture == b.emissiveTexture
        && a.skyTexture == b.skyTexture
        && a.hasDiffuseImage == b.hasDiffuseImage
        && a.hasAlphaImage == b.hasAlphaImage
        && a.hasNormalImage == b.hasNormalImage
        && a.hasSpecularImage == b.hasSpecularImage
        && a.hasEmissiveImage == b.hasEmissiveImage
        && a.hasTextureHandle == b.hasTextureHandle
        && a.hasAlphaTextureHandle == b.hasAlphaTextureHandle
        && a.hasNormalTextureHandle == b.hasNormalTextureHandle
        && a.hasSpecularTextureHandle == b.hasSpecularTextureHandle
        && a.hasEmissiveTextureHandle == b.hasEmissiveTextureHandle
        && a.hasSafeTexture == b.hasSafeTexture
        && a.hasSafeAlphaTexture == b.hasSafeAlphaTexture
        && a.hasSafeNormalTexture == b.hasSafeNormalTexture
        && a.hasSafeSpecularTexture == b.hasSafeSpecularTexture
        && a.hasSafeEmissiveTexture == b.hasSafeEmissiveTexture
        && a.hasSkyTextureHandle == b.hasSkyTextureHandle
        && a.hasSafeSkyTexture == b.hasSafeSkyTexture
        && a.hardwareOpaqueGeometry == b.hardwareOpaqueGeometry
        && a.hasAlphaTest == b.hasAlphaTest
        && a.additiveDecal == b.additiveDecal
        && a.additiveDecalWhiteKey == b.additiveDecalWhiteKey
        && a.filterDecal == b.filterDecal
        && a.filterDecalBlackKey == b.filterDecalBlackKey
        && a.detailDecal == b.detailDecal
        && a.detailDecalDynamic == b.detailDecalDynamic
        && a.detailDecalDiffuseLit == b.detailDecalDiffuseLit
        && a.detailDecalLiquidPool == b.detailDecalLiquidPool
        && a.liquidFilmHasBloodSemantic == b.liquidFilmHasBloodSemantic
        && a.liquidFilmHasWetReflectStage == b.liquidFilmHasWetReflectStage
        && a.liquidFilmHasCoverageSource == b.liquidFilmHasCoverageSource
        && a.liquidFilmHasWetNormalSource == b.liquidFilmHasWetNormalSource
        && a.liquidFilmExactOverride == b.liquidFilmExactOverride
        && a.liquidFilmCandidate == b.liquidFilmCandidate
        && a.liquidFilmCoverageImageName == b.liquidFilmCoverageImageName
        && a.liquidFilmOverrideReason == b.liquidFilmOverrideReason
        && a.liquidFilmReason == b.liquidFilmReason
        && a.isDynamic == b.isDynamic
        && a.detailDecalSpectrum == b.detailDecalSpectrum
        && a.alphaFromDiffuseLuma == b.alphaFromDiffuseLuma
        && a.forceFallbackAlbedo == b.forceFallbackAlbedo
        && a.alphaFromDiffuseDarkKey == b.alphaFromDiffuseDarkKey
        && a.alphaFromDiffuseMagentaKey == b.alphaFromDiffuseMagentaKey
        && a.portalWindowFallback == b.portalWindowFallback
        && a.objectGlassFallback == b.objectGlassFallback
        && a.emissive == b.emissive
        && a.emissiveLightCandidate == b.emissiveLightCandidate
        && a.skyEnvironment == b.skyEnvironment
        && a.alphaCutoff == b.alphaCutoff
        && a.emissiveColor == b.emissiveColor
        && a.skyColor == b.skyColor
        && a.fallbackAlbedo == b.fallbackAlbedo
        && a.hasFallbackAlbedo == b.hasFallbackAlbedo
        && a.diffuseUsage == b.diffuseUsage
        && a.alphaUsage == b.alphaUsage
        && a.normalUsage == b.normalUsage
        && a.specularUsage == b.specularUsage
        && a.emissiveUsage == b.emissiveUsage
        && a.diffuseColorFormat == b.diffuseColorFormat
        && a.alphaColorFormat == b.alphaColorFormat
        && a.normalColorFormat == b.normalColorFormat
        && a.specularColorFormat == b.specularColorFormat
        && a.emissiveColorFormat == b.emissiveColorFormat
        && a.coverage == b.coverage
        && a.tableIndex == b.tableIndex;
}

namespace
{
struct SmokeMaterialEntryPublishRollback
{
    RtSmokeMaterialTextureInfo* replacement = nullptr;
    int replacementIndex = -1;
    bool replacementSwapped = false;
    bool rowAppended = false;
    SmokeMaterialVariantBases::iterator ownershipIterator;
    bool ownershipInserted = false;
    bool committed = false;

    ~SmokeMaterialEntryPublishRollback() noexcept
    {
        if (committed)
        {
            return;
        }
        if (replacementSwapped)
        {
            using std::swap;
            swap(g_smokeMaterialTextureRegistry[replacementIndex], *replacement);
        }
        if (rowAppended)
        {
            g_smokeMaterialTextureRegistry.pop_back();
        }
        if (ownershipInserted)
        {
            EraseSmokeMaterialNodeNoThrow(
                g_smokeMaterialTextureVariantBases, ownershipIterator);
        }
    }
};

RtSmokeMaterialTextureInfo& PublishCompleteSmokeMaterialTextureInfoImpl(
    RtSmokeMaterialTextureInfo&& completeInfo,
    uint32_t authoritativeVariantBaseId,
    RtSmokeMaterialTextureRegistryBumpSite addSite,
    RtSmokeMaterialTextureRegistryBumpSite updateSite)
{
    const uint32_t materialId = completeInfo.materialId;
    const auto ownership = g_smokeMaterialTextureVariantBases.find(materialId);
    if ((authoritativeVariantBaseId == 0u &&
         ownership != g_smokeMaterialTextureVariantBases.end()) ||
        (authoritativeVariantBaseId != 0u &&
         ownership != g_smokeMaterialTextureVariantBases.end() &&
         ownership->second != authoritativeVariantBaseId))
    {
        throw std::logic_error("unproven smoke material variant ownership");
    }

    const auto lookup = g_smokeMaterialTextureRegistryLookup.find(materialId);
    if (lookup != g_smokeMaterialTextureRegistryLookup.end())
    {
        if (authoritativeVariantBaseId != 0u &&
            ownership == g_smokeMaterialTextureVariantBases.end())
        {
            throw std::logic_error("missing smoke material variant ownership");
        }
        const int index = lookup->second;
        if (index < 0 || index >=
                static_cast<int>(g_smokeMaterialTextureRegistry.size()) ||
            g_smokeMaterialTextureRegistry[index].materialId != materialId)
        {
            throw std::logic_error("incoherent smoke material registry lookup");
        }
        MaybeFailAtomicPublish(AtomicPublishFailurePoint::FinalPublish);
        // Resident cache hits may republish identical rows. Generation describes
        // changed values, not the number of publication attempts.
        if (SmokeMaterialTextureInfoEqual(g_smokeMaterialTextureRegistry[index], completeInfo))
            return g_smokeMaterialTextureRegistry[index];
        const auto& previous = g_smokeMaterialTextureRegistry[index];
        const bool bindingChanged = !SmokeBindingDefinitionEqual(previous, completeInfo);
        if (bindingChanged && g_bindingDefinitionRevision == UINT64_MAX)
            throw std::overflow_error("binding definition revision exhausted");
        completeInfo.bindingDefinitionRevision = bindingChanged
            ? g_bindingDefinitionRevision + 1 : previous.bindingDefinitionRevision;
        completeInfo.bindingMutableExposed = previous.bindingMutableExposed;
        using std::swap;
        swap(g_smokeMaterialTextureRegistry[index], completeInfo);
        if (bindingChanged) ++g_bindingDefinitionRevision;
        FinishSmokeMaterialRegistryPublish(updateSite);
        return g_smokeMaterialTextureRegistry[index];
    }

    int hiddenIndex = -1;
    for (int index = 0;
         index < static_cast<int>(g_smokeMaterialTextureRegistry.size());
         ++index)
    {
        if (g_smokeMaterialTextureRegistry[index].materialId != materialId)
        {
            continue;
        }
        if (hiddenIndex >= 0)
        {
            throw std::logic_error("duplicate hidden smoke material rows");
        }
        hiddenIndex = index;
    }
    if (hiddenIndex >= 0 &&
        (authoritativeVariantBaseId == 0u ||
         ownership == g_smokeMaterialTextureVariantBases.end()))
    {
        throw std::logic_error("hidden base row has no repair authority");
    }

    const int publishIndex = hiddenIndex >= 0
        ? hiddenIndex
        : static_cast<int>(g_smokeMaterialTextureRegistry.size());
    if (g_bindingDefinitionRevision == UINT64_MAX || g_bindingMembershipRevision == UINT64_MAX)
        throw std::overflow_error("binding membership revision exhausted");
    completeInfo.bindingDefinitionRevision = g_bindingDefinitionRevision + 1;
    completeInfo.bindingMutableExposed = hiddenIndex >= 0 &&
        g_smokeMaterialTextureRegistry[hiddenIndex].bindingMutableExposed;

    MaybeFailAtomicPublish(AtomicPublishFailurePoint::VectorReserve);
    g_smokeMaterialTextureRegistry.reserve(
        g_smokeMaterialTextureRegistry.size() + (hiddenIndex < 0 ? 1u : 0u));
    MaybeFailAtomicPublish(AtomicPublishFailurePoint::LookupReserve);
    g_smokeMaterialTextureRegistryLookup.reserve(
        g_smokeMaterialTextureRegistryLookup.size() + 1u);
    if (authoritativeVariantBaseId != 0u && hiddenIndex < 0)
    {
        MaybeFailAtomicPublish(AtomicPublishFailurePoint::VariantBaseReserve);
        g_smokeMaterialTextureVariantBases.reserve(
            g_smokeMaterialTextureVariantBases.size() + 1u);
    }

    MaybeFailAtomicPublish(AtomicPublishFailurePoint::LookupNodeStage);
    SmokeMaterialRegistryLookup stagedLookup;
    stagedLookup.emplace(materialId, publishIndex);
    SmokeMaterialRegistryLookup::node_type lookupNode =
        stagedLookup.extract(stagedLookup.begin());

    SmokeMaterialVariantBases::node_type ownershipNode;
    if (authoritativeVariantBaseId != 0u && hiddenIndex < 0)
    {
        MaybeFailAtomicPublish(AtomicPublishFailurePoint::VariantBaseNodeStage);
        SmokeMaterialVariantBases stagedOwnership;
        stagedOwnership.emplace(materialId, authoritativeVariantBaseId);
        ownershipNode = stagedOwnership.extract(stagedOwnership.begin());
    }

    SmokeMaterialEntryPublishRollback rollback;
    rollback.replacement = &completeInfo;
    rollback.replacementIndex = hiddenIndex;
    if (!ownershipNode.empty())
    {
        MaybeFailAtomicPublish(AtomicPublishFailurePoint::VariantBaseInsert);
        const auto inserted =
            g_smokeMaterialTextureVariantBases.insert(std::move(ownershipNode));
        if (!inserted.inserted)
        {
            throw std::logic_error("smoke material variant ownership collision");
        }
        rollback.ownershipIterator = inserted.position;
        rollback.ownershipInserted = true;
    }

    if (hiddenIndex >= 0)
    {
        using std::swap;
        swap(g_smokeMaterialTextureRegistry[hiddenIndex], completeInfo);
        rollback.replacementSwapped = true;
    }
    else
    {
        g_smokeMaterialTextureRegistry.emplace_back(std::move(completeInfo));
        rollback.rowAppended = true;
    }

    MaybeFailAtomicPublish(AtomicPublishFailurePoint::FinalPublish);
    MaybeFailAtomicPublish(AtomicPublishFailurePoint::LookupInsert);
    const auto insertedLookup =
        g_smokeMaterialTextureRegistryLookup.insert(std::move(lookupNode));
    if (!insertedLookup.inserted)
    {
        throw std::logic_error("smoke material registry lookup collision");
    }

    rollback.committed = true;
    ++g_bindingDefinitionRevision;
    ++g_bindingMembershipRevision;
    FinishSmokeMaterialRegistryPublish(addSite);
    return g_smokeMaterialTextureRegistry[publishIndex];
}
}

RtSmokeMaterialTextureInfo& PublishCompleteSmokeMaterialTextureInfo(
    RtSmokeMaterialTextureInfo&& completeInfo)
{
    auto& published = PublishCompleteSmokeMaterialTextureInfoImpl(
        std::move(completeInfo), 0u,
        RtSmokeMaterialTextureRegistryBumpSite::AddMaterial,
        RtSmokeMaterialTextureRegistryBumpSite::UpdateVariantFacts);
    published.bindingMutableExposed = true;
    return published;
}

RtSmokeMaterialTextureInfo& AddSmokeMaterialTextureInfo(uint32_t materialId, const char* materialName)
{
    if (RtSmokeMaterialTextureInfo* existing = LookupSmokeMaterialTextureInfo(materialId))
    {
        existing->bindingMutableExposed = true;
        return *existing;
    }

    RtSmokeMaterialTextureInfo newInfo;
    newInfo.materialId = materialId;
    newInfo.materialName = materialName ? materialName : "<none>";

    return PublishCompleteSmokeMaterialTextureInfo(std::move(newInfo));
}

// Shared CPU-only definitions. Image addresses are identity tokens, never retained
// dereference authority. Every new frame obtains pointers from live registry rows.
struct SmokeBindingRowWitness
{
    uint32_t index=0,materialId=0;
    uint64_t revision=0;
    std::array<uint32_t,6> rules{};
};
struct SmokeMaterialBindingDefinition
{
    std::vector<SmokeBindingRowWitness> witnesses;
    std::vector<uint32_t> activeBases;
    bool filtered=false;
    RtCpuMaterialBindingPlan plan;
    std::vector<uintptr_t> imageIdentities;
    uint64_t chargedBytes=0;
    bool allowGui=false;
};
namespace {
std::shared_ptr<const SmokeMaterialBindingDefinition> retainedBindingDefinition;
uint64_t bindingClearEpoch=1,retainedBindingMembershipRevision=0;
}

// This context never enters a worker closure. It pins this frame's resource
// snapshots while the material worker consumes only copied names/eligibility.
struct RtSmokeMaterialBindingFrame
{
    struct Image { idImage* image=nullptr; nvrhi::TextureHandle texture; bool twoD=false, cube=false; };
    std::vector<Image> images;
    std::shared_ptr<const SmokeMaterialBindingDefinition> definition;
    size_t expectedRows=0;
    uint64_t clearEpoch=0,definitionCharge=0;
    uint64_t snapshotDefinitionRevision=0,snapshotMembershipRevision=0;
    std::vector<SmokeBindingRowWitness> witnesses;
    std::vector<uint32_t> activeBases;
    bool filtered=false;
    uint32_t revisionRowsSkipped=0,mutableRowsChecked=0,membershipScans=0;
    bool allowGui=false,reusedDefinition=false;
    std::vector<uint8_t> safety;
    bool resourcesReady=false,ready=false;
    mutable uint32_t witnessHits=0,witnessMisses=0,refreshHits=0,refreshMisses=0;
    ~RtSmokeMaterialBindingFrame()
    {
        OPTICK_TAG("materialBindingWitnessHits",witnessHits);
        OPTICK_TAG("materialBindingWitnessMisses",witnessMisses);
        OPTICK_TAG("materialBindingRefreshHits",refreshHits);
        OPTICK_TAG("materialBindingRefreshMisses",refreshMisses);
    }
};
namespace {
struct SmokeBindingSlot
{
    idImage* RtSmokeMaterialTextureInfo::*image;
    idStr RtSmokeMaterialTextureInfo::*name;
    nvrhi::TextureHandle RtSmokeMaterialTextureInfo::*texture;
    bool RtSmokeMaterialTextureInfo::*present;
    bool RtSmokeMaterialTextureInfo::*safe;
    bool cube;
};
const SmokeBindingSlot smokeBindingSlots[] = {
    {&RtSmokeMaterialTextureInfo::diffuseImage,&RtSmokeMaterialTextureInfo::diffuseImageName,&RtSmokeMaterialTextureInfo::diffuseTexture,&RtSmokeMaterialTextureInfo::hasTextureHandle,&RtSmokeMaterialTextureInfo::hasSafeTexture,false},
    {&RtSmokeMaterialTextureInfo::alphaImage,&RtSmokeMaterialTextureInfo::alphaImageName,&RtSmokeMaterialTextureInfo::alphaTexture,&RtSmokeMaterialTextureInfo::hasAlphaTextureHandle,&RtSmokeMaterialTextureInfo::hasSafeAlphaTexture,false},
    {&RtSmokeMaterialTextureInfo::normalImage,&RtSmokeMaterialTextureInfo::normalImageName,&RtSmokeMaterialTextureInfo::normalTexture,&RtSmokeMaterialTextureInfo::hasNormalTextureHandle,&RtSmokeMaterialTextureInfo::hasSafeNormalTexture,false},
    {&RtSmokeMaterialTextureInfo::specularImage,&RtSmokeMaterialTextureInfo::specularImageName,&RtSmokeMaterialTextureInfo::specularTexture,&RtSmokeMaterialTextureInfo::hasSpecularTextureHandle,&RtSmokeMaterialTextureInfo::hasSafeSpecularTexture,false},
    {&RtSmokeMaterialTextureInfo::emissiveImage,&RtSmokeMaterialTextureInfo::emissiveImageName,&RtSmokeMaterialTextureInfo::emissiveTexture,&RtSmokeMaterialTextureInfo::hasEmissiveTextureHandle,&RtSmokeMaterialTextureInfo::hasSafeEmissiveTexture,false},
    {&RtSmokeMaterialTextureInfo::skyImage,&RtSmokeMaterialTextureInfo::skyImageName,&RtSmokeMaterialTextureInfo::skyTexture,&RtSmokeMaterialTextureInfo::hasSkyTextureHandle,&RtSmokeMaterialTextureInfo::hasSafeSkyTexture,true},
};
const std::array<uint32_t,6>* FindPreparedSmokeBindings(const RtSmokeMaterialBindingFrame* frame,
    const RtSmokeMaterialTextureInfo& info, uint32_t base=0)
{
    if (!frame || !frame->ready || frame->clearEpoch!=bindingClearEpoch) return nullptr;
    auto found=frame->definition->plan.rows.find(info.materialId);
    if (found==frame->definition->plan.rows.end()) {
        if (!base) base=SmokeMaterialTextureVariantBase(info.materialId);
        found=frame->definition->plan.rows.find(base);
    }
    if (found==frame->definition->plan.rows.end()) return nullptr;
    for (size_t i=0;i<6;++i) {
        const auto& rule=frame->definition->plan.rules[found->second[i]];
        if (info.*smokeBindingSlots[i].image != frame->images[rule.resource].image ||
            rule.name != (info.*smokeBindingSlots[i].name).c_str()) return nullptr;
    }
    return &found->second;
}
bool ApplyPreparedSmokeBindings(RtSmokeMaterialTextureInfo& info, const RtSmokeMaterialBindingFrame& frame,
    const std::array<uint32_t,6>& row, bool publishGeneration)
{
    bool changed=false;
    for (size_t i=0;i<6;++i) {
        const auto& slot=smokeBindingSlots[i];
        const auto& texture=frame.images[frame.definition->plan.rules[row[i]].resource].texture;
        const bool present=texture != nullptr,safe=frame.safety[row[i]] != 0;
        changed=changed || (info.*slot.texture).Get()!=texture.Get() || info.*slot.present!=present || info.*slot.safe!=safe;
        info.*slot.texture=texture;info.*slot.present=present;info.*slot.safe=safe;
    }
    if (changed && publishGeneration) {
        ++g_smokeMaterialTextureRegistryGeneration;
        RecordSmokeMaterialTextureRegistryBump(RtSmokeMaterialTextureRegistryBumpSite::RefreshTextureHandles);
    }
    return changed;
}
}

namespace {
// Returns a miss reason rather than trusting the broad registry generation: public
// mutable rows can change slot names/images without publishing a generation bump.
uint32_t ReuseSmokeMaterialBindingDefinition(RtSmokeMaterialBindingFrame& frame,
    const std::vector<uint32_t>* activeBases,uint64_t budget,bool allowGui)
{
    OPTICK_EVENT("PT CPU Material Binding Definition Check");
    const auto definition=retainedBindingDefinition;
    if (!definition) return 1;
    if (definition->allowGui!=allowGui) return 2;
    if (definition->chargedBytes>budget) return 3;
    if (definition->filtered!=(activeBases!=nullptr) ||
        (activeBases && *activeBases!=definition->activeBases)) return 4;
    // Only membership mutations require the registry lookup walk. An unrelated
    // base addition may still leave this exact selected definition reusable.
    if (retainedBindingMembershipRevision!=g_bindingMembershipRevision) {
        ++frame.membershipScans;
        size_t count=0;
        for (const auto& entry:g_smokeMaterialTextureRegistryLookup) {
            if (activeBases && !std::binary_search(activeBases->begin(),activeBases->end(),entry.first) &&
                !std::binary_search(activeBases->begin(),activeBases->end(),SmokeMaterialTextureVariantBase(entry.first))) continue;
            const auto* info=LookupSmokeMaterialTextureInfo(entry.first);
            if (!info) continue;
            if (info->materialId!=entry.first || definition->plan.rows.count(entry.first)==0) return 4;
            ++count;
        }
        if (count!=definition->witnesses.size()) return 4;
        retainedBindingMembershipRevision=g_bindingMembershipRevision;
    }
    frame.images.resize(definition->imageIdentities.size());
    for (const auto& witness:definition->witnesses) {
        if (witness.index>=g_smokeMaterialTextureRegistry.size()) return 4;
        const auto& info=g_smokeMaterialTextureRegistry[witness.index];
        if (info.materialId!=witness.materialId) return 4;
        if (info.bindingDefinitionRevision!=witness.revision) return 5;
        if (info.bindingMutableExposed) ++frame.mutableRowsChecked;
        else ++frame.revisionRowsSkipped;
        for (size_t i=0;i<6;++i) {
            const auto& slot=smokeBindingSlots[i];
            const auto& rule=definition->plan.rules[witness.rules[i]];
            idImage* current=info.*slot.image;
            if (info.bindingMutableExposed && (rule.cube!=slot.cube ||
                rule.name!=(info.*slot.name).c_str() ||
                definition->imageIdentities[rule.resource]!=reinterpret_cast<uintptr_t>(current))) return 5;
            // Fresh owner pointers, including trusted rows; never dereference a
            // retained identity token as resource ownership authority.
            frame.images[rule.resource].image=current;
        }
    }
    frame.definition=definition;frame.expectedRows=definition->witnesses.size();frame.reusedDefinition=true;
    frame.definitionCharge=definition->chargedBytes;
    return 0;
}
}

bool SmokeMaterialBindingNeedsPreparation(const RtSmokeMaterialBindingFrame& frame)
{
    return !frame.definition;
}

std::shared_ptr<RtSmokeMaterialBindingFrame> SnapshotSmokeMaterialBindingFrame(RtCpuMaterialBindingPlanInput& input, uint64_t budget,
    const std::vector<uint32_t>* baseIds)
{
    OPTICK_EVENT("PT CPU Material Bindings Snapshot");
    input={};
    budget=std::min(budget,input.kMaxBytes);
#if !defined(RT_PT_TEXTURE_REGISTRY_HARNESS)
    input.allowGui=r_pathTracingAllowGuiTextures.GetInteger()!=0;
#endif
    auto frame=std::make_shared<RtSmokeMaterialBindingFrame>();
    frame->clearEpoch=bindingClearEpoch;frame->allowGui=input.allowGui;
    frame->snapshotDefinitionRevision=g_bindingDefinitionRevision;
    frame->snapshotMembershipRevision=g_bindingMembershipRevision;
    frame->filtered=baseIds!=nullptr;
    std::unordered_map<idImage*,uint32_t> images;
    uint64_t charged=0;
    const auto charge=[&](uint64_t bytes) { if (bytes>budget-charged) return false;charged+=bytes;return true; };
    if (!charge(256)) return {};
    if (baseIds) {
        if (!charge(uint64_t(baseIds->size())*32)) return {};
        frame->activeBases=*baseIds;
        std::sort(frame->activeBases.begin(),frame->activeBases.end());
        frame->activeBases.erase(std::unique(frame->activeBases.begin(),frame->activeBases.end()),frame->activeBases.end());
    }
    const uint32_t miss=ReuseSmokeMaterialBindingDefinition(*frame,baseIds ? &frame->activeBases : nullptr,budget,input.allowGui);
    OPTICK_TAG("materialBindingRevisionRowsSkipped",frame->revisionRowsSkipped);
    OPTICK_TAG("materialBindingMutableRowsChecked",frame->mutableRowsChecked);
    OPTICK_TAG("materialBindingMembershipScans",frame->membershipScans);
    OPTICK_TAG("materialBindingDefinitionMissReason",miss);
    OPTICK_TAG("materialBindingDefinitionReused",miss==0 ? 1u : 0u);
    if (!miss) {
        OPTICK_TAG("materialBindingDefinitionRows",static_cast<uint32_t>(frame->expectedRows));
        OPTICK_TAG("materialBindingDefinitionRules",static_cast<uint32_t>(frame->definition->plan.rules.size()));
        OPTICK_TAG("materialBindingDefinitionBytes",frame->definitionCharge);
        return frame;
    }
    frame->images.clear();
    const std::unordered_set<uint32_t> activeBases(frame->activeBases.begin(),frame->activeBases.end());
    OPTICK_EVENT("PT CPU Material Bindings Raw Capture");
    for (const auto& entry:g_smokeMaterialTextureRegistryLookup) {
        if (baseIds && activeBases.count(entry.first)==0 &&
            activeBases.count(SmokeMaterialTextureVariantBase(entry.first))==0) continue;
        const auto* info=LookupSmokeMaterialTextureInfo(entry.first);
        if (!info) continue;
        if (input.rows.size()>=65535 || !charge(160+6*512+2*sizeof(SmokeBindingRowWitness))) return {};
        RtCpuMaterialBindingRow row;
        row.materialId=info->materialId;
        for (size_t i=0;i<6;++i) {
            const auto& slot=smokeBindingSlots[i];
            idImage* image=info->*slot.image;
            auto imageIt=images.find(image);
            if (imageIt==images.end()) {
                if (!charge(128)) return {};
                RtSmokeMaterialBindingFrame::Image frozen; frozen.image=image;
                const uint32_t index=static_cast<uint32_t>(frame->images.size());
                frame->images.push_back(std::move(frozen));imageIt=images.emplace(image,index).first;
            }
            const auto& name=info->*slot.name;
            if (!charge(uint64_t(name.Length())*5)) return {};
            // Name/index preparation is independent of resource resolution.
            // Actual descriptor eligibility is intersected only at owner apply.
            row.slots[i]={name.c_str(),imageIt->second,slot.cube,true};
        }
        frame->witnesses.push_back({static_cast<uint32_t>(entry.second),info->materialId,info->bindingDefinitionRevision,{}});
        input.rows.push_back(std::move(row));
    }
    frame->expectedRows=input.rows.size();
    input.ownerCharge=charged;
    frame->definitionCharge=input.ChargedBytes();
    OPTICK_TAG("materialBindingSnapshotImages",static_cast<uint32_t>(frame->images.size()));
    OPTICK_TAG("materialBindingSnapshotRows",static_cast<uint32_t>(input.rows.size()));
    OPTICK_TAG("materialBindingSnapshotChargedBytes",input.ChargedBytes());
    return frame;
}

bool ResolveSmokeMaterialBindingResources(RtSmokeMaterialBindingFrame& frame, nvrhi::ICommandList* commandList)
{
    OPTICK_EVENT("PT CPU Material Binding Resources");
    if (frame.ready || frame.resourcesReady || frame.clearEpoch!=bindingClearEpoch) return false;
#if !defined(RT_PT_TEXTURE_REGISTRY_HARNESS)
    if (!commandList) return false;
#endif
    uint32_t loadAttempts=0,loaded=0,defaulted=0;
    for (auto& frozen:frame.images) {
#if !defined(RT_PT_TEXTURE_REGISTRY_HARNESS)
        if (frozen.image) {
            // Match backend SetCurrentImage first-use behavior. Frontend image
            // preparation can leave GPU allocation deferred when no list exists.
            if (!frozen.image->IsLoaded() && !frozen.image->IsDefaulted()) {
                ++loadAttempts;
                frozen.image->ActuallyLoadImage(true,commandList);
                if (frozen.image->IsLoaded()) ++loaded;
            }
            if (frozen.image->IsDefaulted()) ++defaulted;
            frozen.texture=frozen.image->GetPathTracingTextureBindingSnapshot().texture;
        }
#endif
        if (frozen.texture) {
            frozen.twoD=IsSmokeDiffuseTextureSafeForRayTracing(frozen.texture);
            const auto& desc=frozen.texture->getDesc();
            frozen.cube=desc.isShaderResource && !desc.isRenderTarget && !desc.isUAV &&
                desc.sampleCount==1 && desc.dimension==nvrhi::TextureDimension::TextureCube;
        }
    }
    OPTICK_TAG("materialBindingImageLoadAttempts",loadAttempts);
    OPTICK_TAG("materialBindingImagesLoaded",loaded);
    OPTICK_TAG("materialBindingImagesDefaulted",defaulted);
    frame.resourcesReady=true;
    return true;
}

bool CompleteSmokeMaterialBindingFrame(RtSmokeMaterialBindingFrame& frame, RtCpuMaterialBindingPlan&& plan)
{
    OPTICK_EVENT("PT CPU Material Bindings Apply");
    if (frame.ready || !frame.resourcesReady || frame.clearEpoch!=bindingClearEpoch ||
        frame.snapshotDefinitionRevision!=g_bindingDefinitionRevision ||
        frame.snapshotMembershipRevision!=g_bindingMembershipRevision) return false;
    std::shared_ptr<const SmokeMaterialBindingDefinition> definition=frame.definition;
    if (!definition) {
        if (plan.rows.size()!=frame.expectedRows || plan.safety.size()!=plan.rules.size() ||
            std::any_of(plan.safety.begin(),plan.safety.end(),[](uint8_t v) {return v>1;})) return false;
        for (const auto& rule:plan.rules) if (rule.resource>=frame.images.size()) return false;
        for (const auto& row:plan.rows) for (uint32_t rule:row.second) if (rule>=plan.rules.size()) return false;
        auto candidate=std::make_shared<SmokeMaterialBindingDefinition>();
        candidate->plan=std::move(plan);candidate->allowGui=frame.allowGui;
        candidate->chargedBytes=frame.definitionCharge;
        candidate->activeBases=frame.activeBases;candidate->filtered=frame.filtered;
        candidate->witnesses=frame.witnesses;
        for (auto& witness:candidate->witnesses) {
            const auto row=candidate->plan.rows.find(witness.materialId);
            if (row==candidate->plan.rows.end()) return false;
            witness.rules=row->second;
        }
        for (const auto& image:frame.images) candidate->imageIdentities.push_back(reinterpret_cast<uintptr_t>(image.image));
        definition=std::move(candidate);
    } else if (!plan.rows.empty() || !plan.rules.empty() || !plan.safety.empty()) return false;
    // Start from immutable name policy every frame, never prior resource eligibility.
    auto safety=definition->plan.safety;
    for (size_t i=0;i<definition->plan.rules.size();++i) {
        const auto& rule=definition->plan.rules[i];const auto& resource=frame.images[rule.resource];
        safety[i]=safety[i] && (rule.cube ? resource.cube : resource.twoD) ? 1u : 0u;
    }
    frame.definition=definition;frame.safety=std::move(safety);frame.ready=true;
    retainedBindingDefinition=std::move(definition);
    retainedBindingMembershipRevision=frame.snapshotMembershipRevision;
    OPTICK_TAG("materialBindingDefinitionRebuilt",frame.reusedDefinition ? 0u : 1u);
    OPTICK_TAG("materialBindingDefinitionConsumed",1u);
    OPTICK_TAG("materialBindingOwnerReplaySkipped",1u);
    return true;
}

// Owner-only binding witness. Compare before copying large strings/metadata.
// A changed binding still takes the original transactional candidate path.
static bool SmokeMaterialTextureHandlesCurrent(const RtSmokeMaterialTextureInfo& info, const RtSmokeMaterialBindingFrame* frame)
{
    if (const auto* row=FindPreparedSmokeBindings(frame,info)) {
        ++frame->witnessHits;
        for (size_t i=0;i<6;++i) {
            const auto& slot=smokeBindingSlots[i];
            const auto& texture=frame->images[frame->definition->plan.rules[(*row)[i]].resource].texture;
            if ((info.*slot.texture).Get()!=texture.Get() || info.*slot.present!=(texture!=nullptr) ||
                info.*slot.safe!=(frame->safety[(*row)[i]]!=0)) return false;
        }
        return true;
    }
    if (frame) ++frame->witnessMisses;
#if defined(RT_PT_TEXTURE_REGISTRY_HARNESS)
    return true; // same no-op binding semantics as the harness refresh helper
#else
    const auto matches = [](idImage* image, const nvrhi::TextureHandle& texture,
        const idStr& name, bool hasHandle, bool safe, nvrhi::TextureDimension dimension) {
        const auto binding = image ? image->GetPathTracingTextureBindingSnapshot() : idPathTracingTextureBindingSnapshot();
        const bool present = binding.texture != nullptr;
        return binding.texture.Get() == texture.Get() && present == hasHandle &&
            safe == (present && IsSmokeMaterialTextureSnapshotSafeForRayTracing(binding.texture, name, dimension));
    };
    return matches(info.diffuseImage, info.diffuseTexture, info.diffuseImageName, info.hasTextureHandle, info.hasSafeTexture, nvrhi::TextureDimension::Texture2D) &&
        matches(info.alphaImage, info.alphaTexture, info.alphaImageName, info.hasAlphaTextureHandle, info.hasSafeAlphaTexture, nvrhi::TextureDimension::Texture2D) &&
        matches(info.normalImage, info.normalTexture, info.normalImageName, info.hasNormalTextureHandle, info.hasSafeNormalTexture, nvrhi::TextureDimension::Texture2D) &&
        matches(info.specularImage, info.specularTexture, info.specularImageName, info.hasSpecularTextureHandle, info.hasSafeSpecularTexture, nvrhi::TextureDimension::Texture2D) &&
        matches(info.emissiveImage, info.emissiveTexture, info.emissiveImageName, info.hasEmissiveTextureHandle, info.hasSafeEmissiveTexture, nvrhi::TextureDimension::Texture2D) &&
        matches(info.skyImage, info.skyTexture, info.skyImageName, info.hasSkyTextureHandle, info.hasSafeSkyTexture, nvrhi::TextureDimension::TextureCube);
#endif
}

bool RegisterSmokeMaterialTextureVariant(uint32_t variantMaterialId, uint32_t baseMaterialId, const RtSmokeMaterialBindingFrame* frame)
{
    if (variantMaterialId == 0u || baseMaterialId == 0u || variantMaterialId == baseMaterialId)
    {
        return false;
    }

    const RtSmokeMaterialTextureInfo* baseInfo = LookupSmokeMaterialTextureInfo(baseMaterialId);
    if (!baseInfo)
    {
        return false;
    }

    RtSmokeMaterialTextureInfo* existing = LookupSmokeMaterialTextureInfo(variantMaterialId);
    const auto ownedVariant =
        g_smokeMaterialTextureVariantBases.find(variantMaterialId);
    if (!existing && ownedVariant != g_smokeMaterialTextureVariantBases.end())
    {
        // A pre-atomic-publication failure could leave a complete row and its
        // authoritative ownership record without a usable primary lookup.
        // Repair only that proven case; ownership is never inferred from row
        // contents or the proposed base.
        if (ownedVariant->second != baseMaterialId)
        {
            return false;
        }
        int hiddenIndex = -1;
        for (int index = 0;
             index < static_cast<int>(g_smokeMaterialTextureRegistry.size());
             ++index)
        {
            if (g_smokeMaterialTextureRegistry[index].materialId !=
                variantMaterialId)
            {
                continue;
            }
            if (hiddenIndex >= 0)
            {
                return false;
            }
            hiddenIndex = index;
        }
        if (hiddenIndex < 0)
        {
            return false;
        }

        RtSmokeMaterialTextureInfo updated =
            g_smokeMaterialTextureRegistry[hiddenIndex];
        updated.liquidFilmHasBloodSemantic = baseInfo->liquidFilmHasBloodSemantic;
        updated.liquidFilmHasWetReflectStage = baseInfo->liquidFilmHasWetReflectStage;
        updated.liquidFilmHasCoverageSource = baseInfo->liquidFilmHasCoverageSource;
        updated.liquidFilmHasWetNormalSource = baseInfo->liquidFilmHasWetNormalSource;
        updated.liquidFilmExactOverride = baseInfo->liquidFilmExactOverride;
        updated.liquidFilmCandidate = baseInfo->liquidFilmCandidate;
        updated.liquidFilmCoverageImageName = baseInfo->liquidFilmCoverageImageName;
        updated.liquidFilmOverrideReason = baseInfo->liquidFilmOverrideReason;
        updated.liquidFilmReason = baseInfo->liquidFilmReason;
        updated.hardwareOpaqueGeometry =
            ComputeSmokeMaterialHardwareOpaqueGeometry(updated);
        RefreshSmokeMaterialTextureHandleStateImpl(updated, false, frame, baseMaterialId);

        PublishCompleteSmokeMaterialTextureInfoImpl(
            std::move(updated), baseMaterialId,
            RtSmokeMaterialTextureRegistryBumpSite::AddVariant,
            RtSmokeMaterialTextureRegistryBumpSite::UpdateVariantFacts);
        return true;
    }
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
        if (!liquidFilmFactsChanged &&
            existing->hardwareOpaqueGeometry == ComputeSmokeMaterialHardwareOpaqueGeometry(*existing) &&
            SmokeMaterialTextureHandlesCurrent(*existing, frame)) return true;
        RtSmokeMaterialTextureInfo updated = *existing;
        updated.liquidFilmHasBloodSemantic = baseInfo->liquidFilmHasBloodSemantic;
        updated.liquidFilmHasWetReflectStage = baseInfo->liquidFilmHasWetReflectStage;
        updated.liquidFilmHasCoverageSource = baseInfo->liquidFilmHasCoverageSource;
        updated.liquidFilmHasWetNormalSource = baseInfo->liquidFilmHasWetNormalSource;
        updated.liquidFilmExactOverride = baseInfo->liquidFilmExactOverride;
        updated.liquidFilmCandidate = baseInfo->liquidFilmCandidate;
        updated.liquidFilmCoverageImageName = baseInfo->liquidFilmCoverageImageName;
        updated.liquidFilmOverrideReason = baseInfo->liquidFilmOverrideReason;
        updated.liquidFilmReason = baseInfo->liquidFilmReason;
        const bool oldHardwareOpaqueGeometry = updated.hardwareOpaqueGeometry;
        updated.hardwareOpaqueGeometry =
            ComputeSmokeMaterialHardwareOpaqueGeometry(updated);
        const bool handleFactsChanged =
            RefreshSmokeMaterialTextureHandleStateImpl(updated, false, frame, baseMaterialId);
        if (!liquidFilmFactsChanged &&
            oldHardwareOpaqueGeometry == updated.hardwareOpaqueGeometry &&
            !handleFactsChanged)
        {
            return true;
        }

        PublishCompleteSmokeMaterialTextureInfoImpl(
            std::move(updated), baseMaterialId,
            RtSmokeMaterialTextureRegistryBumpSite::AddVariant,
            RtSmokeMaterialTextureRegistryBumpSite::UpdateVariantFacts);
        return true;
    }

    RtSmokeMaterialTextureInfo variantInfo = *baseInfo;
    variantInfo.materialId = variantMaterialId;
    variantInfo.tableIndex = -1;
    variantInfo.hardwareOpaqueGeometry =
        ComputeSmokeMaterialHardwareOpaqueGeometry(variantInfo);
    RefreshSmokeMaterialTextureHandleStateImpl(variantInfo, false, frame, baseMaterialId);

    PublishCompleteSmokeMaterialTextureInfoImpl(
        std::move(variantInfo), baseMaterialId,
        RtSmokeMaterialTextureRegistryBumpSite::AddVariant,
        RtSmokeMaterialTextureRegistryBumpSite::UpdateVariantFacts);
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
    retainedBindingDefinition.reset();++bindingClearEpoch;
    retainedBindingMembershipRevision=0;
    g_bindingDefinitionRevision=1;g_bindingMembershipRevision=1;
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
    RecordSmokeMaterialTextureRegistryBump(RtSmokeMaterialTextureRegistryBumpSite::ClearVariants);
    return removedCount;
}

int ClearSmokeMaterialTextureRegistry()
{
    retainedBindingDefinition.reset();++bindingClearEpoch;
    retainedBindingMembershipRevision=0;
    g_bindingDefinitionRevision=1;g_bindingMembershipRevision=1;
    const int removedCount = static_cast<int>(g_smokeMaterialTextureRegistry.size());
    g_smokeMaterialTextureRegistry.clear();
    g_smokeMaterialTextureRegistryLookup.clear();
    g_smokeMaterialTextureVariantBases.clear();
    ++g_smokeMaterialTextureRegistryGeneration;
    RecordSmokeMaterialTextureRegistryBump(RtSmokeMaterialTextureRegistryBumpSite::ClearRegistry);
    return removedCount;
}

static bool RefreshSmokeMaterialTextureHandleStateImpl(
    RtSmokeMaterialTextureInfo& info,
    bool publishGeneration, const RtSmokeMaterialBindingFrame* frame, uint32_t base)
{
    if (const auto* row=FindPreparedSmokeBindings(frame,info,base)) {
        ++frame->refreshHits;
        return ApplyPreparedSmokeBindings(info,*frame,*row,publishGeneration);
    }
    if (frame) ++frame->refreshMisses;
#if defined(RT_PT_TEXTURE_REGISTRY_HARNESS)
    // The atomic-publication harness uses pointer-free/default handle rows;
    // handle-refresh behavior remains covered by the renderer target.
    (void)info;
    return false;
#else
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

    const idPathTracingTextureBindingSnapshot diffuseBinding = info.diffuseImage
        ? info.diffuseImage->GetPathTracingTextureBindingSnapshot()
        : idPathTracingTextureBindingSnapshot();
    const idPathTracingTextureBindingSnapshot alphaBinding = info.alphaImage
        ? info.alphaImage->GetPathTracingTextureBindingSnapshot()
        : idPathTracingTextureBindingSnapshot();
    const idPathTracingTextureBindingSnapshot normalBinding = info.normalImage
        ? info.normalImage->GetPathTracingTextureBindingSnapshot()
        : idPathTracingTextureBindingSnapshot();
    const idPathTracingTextureBindingSnapshot specularBinding = info.specularImage
        ? info.specularImage->GetPathTracingTextureBindingSnapshot()
        : idPathTracingTextureBindingSnapshot();
    const idPathTracingTextureBindingSnapshot emissiveBinding = info.emissiveImage
        ? info.emissiveImage->GetPathTracingTextureBindingSnapshot()
        : idPathTracingTextureBindingSnapshot();
    const idPathTracingTextureBindingSnapshot skyBinding = info.skyImage
        ? info.skyImage->GetPathTracingTextureBindingSnapshot()
        : idPathTracingTextureBindingSnapshot();
    info.diffuseTexture = diffuseBinding.texture;
    info.alphaTexture = alphaBinding.texture;
    info.normalTexture = normalBinding.texture;
    info.specularTexture = specularBinding.texture;
    info.emissiveTexture = emissiveBinding.texture;
    info.skyTexture = skyBinding.texture;
    info.hasTextureHandle = info.diffuseTexture != nullptr;
    info.hasAlphaTextureHandle = info.alphaTexture != nullptr;
    info.hasNormalTextureHandle = info.normalTexture != nullptr;
    info.hasSpecularTextureHandle = info.specularTexture != nullptr;
    info.hasEmissiveTextureHandle = info.emissiveTexture != nullptr;
    info.hasSkyTextureHandle = info.skyTexture != nullptr;
    info.hasSafeTexture = info.hasTextureHandle &&
        IsSmokeMaterialTextureSnapshotSafeForRayTracing(
            diffuseBinding.texture,
            info.diffuseImageName,
            nvrhi::TextureDimension::Texture2D);
    info.hasSafeAlphaTexture = info.hasAlphaTextureHandle &&
        IsSmokeMaterialTextureSnapshotSafeForRayTracing(
            alphaBinding.texture,
            info.alphaImageName,
            nvrhi::TextureDimension::Texture2D);
    info.hasSafeNormalTexture = info.hasNormalTextureHandle &&
        IsSmokeMaterialTextureSnapshotSafeForRayTracing(
            normalBinding.texture,
            info.normalImageName,
            nvrhi::TextureDimension::Texture2D);
    info.hasSafeSpecularTexture = info.hasSpecularTextureHandle &&
        IsSmokeMaterialTextureSnapshotSafeForRayTracing(
            specularBinding.texture,
            info.specularImageName,
            nvrhi::TextureDimension::Texture2D);
    info.hasSafeEmissiveTexture = info.hasEmissiveTextureHandle &&
        IsSmokeMaterialTextureSnapshotSafeForRayTracing(
            emissiveBinding.texture,
            info.emissiveImageName,
            nvrhi::TextureDimension::Texture2D);
    info.hasSafeSkyTexture = info.hasSkyTextureHandle &&
        IsSmokeMaterialTextureSnapshotSafeForRayTracing(
            skyBinding.texture,
            info.skyImageName,
            nvrhi::TextureDimension::TextureCube);

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
    if (changed && publishGeneration)
    {
        ++g_smokeMaterialTextureRegistryGeneration;
        RecordSmokeMaterialTextureRegistryBump(RtSmokeMaterialTextureRegistryBumpSite::RefreshTextureHandles);
    }
    return changed;
#endif
}

bool RefreshSmokeMaterialTextureHandleState(RtSmokeMaterialTextureInfo& info)
{
    return RefreshSmokeMaterialTextureHandleStateImpl(info, true);
}

bool RefreshUnpublishedSmokeMaterialTextureHandleState(
    RtSmokeMaterialTextureInfo& info)
{
    return RefreshSmokeMaterialTextureHandleStateImpl(info, false);
}

namespace
{
template<typename Record, typename FindRecord, typename RefreshRecord>
RtSmokeMaterialActiveTextureRefreshResult RefreshSmokeMaterialTextureHandlesForActiveIdsImpl(
    const std::vector<uint32_t>& staticMaterialIds,
    const std::vector<uint32_t>& dynamicMaterialIds,
    FindRecord findRecord,
    RefreshRecord refreshRecord)
{
    RtSmokeMaterialActiveTextureRefreshResult result;
    std::unordered_set<uint32_t> uniqueMaterialIds;
    uniqueMaterialIds.reserve(staticMaterialIds.size() + dynamicMaterialIds.size());
    const auto visit = [&](const std::vector<uint32_t>& materialIds) {
        for (uint32_t materialId : materialIds)
        {
            if (!uniqueMaterialIds.insert(materialId).second)
            {
                continue;
            }
            Record* record = findRecord(materialId);
            if (!record)
            {
                continue;
            }
            ++result.visited;
            if (refreshRecord(*record))
            {
                result.changedMaterialIds.push_back(materialId);
            }
        }
    };
    visit(staticMaterialIds);
    visit(dynamicMaterialIds);
    return result;
}
}

std::vector<uint32_t> PrepareSmokeMaterialHydrationIds(const std::vector<uint32_t>& ids,
    const RtSmokeMaterialBindingFrame* frame)
{
    OPTICK_EVENT("PT CPU Material Variant Hydration");
    if (ids.size()>65535) throw std::length_error("material hydration partition capacity");
    std::vector<uint32_t> pending;
    pending.reserve(ids.size());
    std::unordered_set<uint32_t> refreshed;
    for (uint32_t id:ids) {
        const auto owner=g_smokeMaterialTextureVariantBases.find(id);
        auto* info=owner!=g_smokeMaterialTextureVariantBases.end() ? LookupSmokeMaterialTextureInfo(id) : nullptr;
        const auto* base=info ? LookupSmokeMaterialTextureInfo(owner->second) : nullptr;
        if (!info || !base || !SmokeMaterialTextureInfoHasMaterialMetadata(*info) ||
            !SmokeMaterialTextureInfoHasMaterialMetadata(*base)) {
            pending.push_back(id);
            continue;
        }
        // Same current handle/safety refresh as existing-ID discovery, with no
        // mutable address escaping registry ownership. Never clear an escape flag.
        if (refreshed.insert(id).second) RefreshSmokeMaterialTextureHandleStateImpl(*info,true,frame);
    }
    OPTICK_TAG("materialHydrationOwnedVariants",static_cast<uint32_t>(refreshed.size()));
    OPTICK_TAG("materialHydrationDiscoveryIds",static_cast<uint32_t>(pending.size()));
    return pending;
}

RtSmokeMaterialActiveTextureRefreshResult RefreshSmokeMaterialTextureHandlesForActiveIds(
    const std::vector<uint32_t>& staticMaterialIds,
    const std::vector<uint32_t>& dynamicMaterialIds, const RtSmokeMaterialBindingFrame* frame)
{
    return RefreshSmokeMaterialTextureHandlesForActiveIdsImpl<RtSmokeMaterialTextureInfo>(
        staticMaterialIds,
        dynamicMaterialIds,
        [](uint32_t materialId) {
            return LookupSmokeMaterialTextureInfo(materialId);
        },
        [frame](RtSmokeMaterialTextureInfo& info) {
            return RefreshSmokeMaterialTextureHandleStateImpl(info, true, frame);
        });
}

bool SmokeMaterialActiveTextureRefreshSelfTest()
{
    struct TestRecord
    {
        uint32_t materialId = 0;
        bool changed = false;
        int visits = 0;
    };
    std::vector<TestRecord> records = {
        { 10u, false, 0 },
        { 20u, true, 0 }
    };
    const RtSmokeMaterialActiveTextureRefreshResult result =
        RefreshSmokeMaterialTextureHandlesForActiveIdsImpl<TestRecord>(
            { 10u, 20u, 10u, 30u },
            { 20u, 30u },
            [&](uint32_t materialId) -> TestRecord* {
                for (TestRecord& record : records)
                {
                    if (record.materialId == materialId)
                    {
                        return &record;
                    }
                }
                return nullptr;
            },
            [](TestRecord& record) {
                ++record.visits;
                return record.changed;
            });
    return result.visited == 2 &&
        result.changedMaterialIds == std::vector<uint32_t>({ 20u }) &&
        records[0].visits == 1 && records[1].visits == 1;
}

RtSmokeMaterialTextureInfo ResolveSmokeMaterialTextureInfo(uint32_t materialId, int tableIndex)
{
    const RtSmokeMaterialTextureInfo* existing = LookupSmokeMaterialTextureInfo(materialId);
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
        LookupSmokeMaterialTextureInfo(materialId);
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

void EnumerateSmokeMaterialTextureVariantBases(
    std::vector<RtPathTraceMaterialTextureVariantBasePod>& output)
{
    output.clear();
    output.reserve(g_smokeMaterialTextureVariantBases.size());
    for (const auto& entry : g_smokeMaterialTextureVariantBases)
    {
        RtPathTraceMaterialTextureVariantBasePod pod;
        pod.variantId = entry.first;
        pod.baseId = entry.second;
        output.push_back(pod);
    }
    std::sort(output.begin(), output.end(),
        [](const auto& lhs, const auto& rhs)
        {
            return lhs.variantId < rhs.variantId;
        });
}

void EnumerateSmokeMaterialTextureRegistryPod(
    std::vector<RtPathTraceCaptureRegistryMaterialPod>& output)
{
    output.clear();
    output.reserve(g_smokeMaterialTextureRegistry.size());
    for (const RtSmokeMaterialTextureInfo& info : g_smokeMaterialTextureRegistry)
    {
        RtPathTraceCaptureRegistryMaterialPod pod;
        pod.materialId = info.materialId;
        RtPathTracePlanningCopyName(
            pod.materialName, sizeof(pod.materialName), info.materialName.c_str());
        pod.hardwareOpaqueGeometry = info.hardwareOpaqueGeometry;
        pod.hasAlphaTest = info.hasAlphaTest;
        pod.alphaCutoff = info.alphaCutoff;
        pod.emissive = info.emissive;
        pod.skyEnvironment = info.skyEnvironment;
        pod.materialMetadataValid =
            SmokeMaterialTextureInfoHasMaterialMetadata(info);
        pod.isDynamic = info.isDynamic;
        pod.detailDecalLiquidPool = info.detailDecalLiquidPool;
        output.push_back(pod);
    }
    std::sort(output.begin(), output.end(),
        [](const auto& lhs, const auto& rhs)
        {
            return lhs.materialId < rhs.materialId;
        });
}

RtSmokeMaterialTextureRegistryEnumerationCounts
CountSmokeMaterialTextureRegistryEnumeration()
{
    RtSmokeMaterialTextureRegistryEnumerationCounts counts;
    counts.variantBases = g_smokeMaterialTextureVariantBases.size();
    counts.registryMaterials = g_smokeMaterialTextureRegistry.size();
    return counts;
}

bool FillSmokeMaterialTextureRegistryEnumerationPreReserved(
    const RtSmokeMaterialTextureRegistryEnumerationCounts& counts,
    std::vector<RtPathTraceMaterialTextureVariantBasePod>& variantBases,
    std::vector<RtPathTraceCaptureRegistryMaterialPod>& registryMaterials)
{
    if (counts.variantBases != g_smokeMaterialTextureVariantBases.size() ||
        counts.registryMaterials != g_smokeMaterialTextureRegistry.size() ||
        variantBases.capacity() < counts.variantBases ||
        registryMaterials.capacity() < counts.registryMaterials)
    {
        return false;
    }
    variantBases.resize(counts.variantBases);
    std::size_t variantIndex = 0;
    for (const auto& entry : g_smokeMaterialTextureVariantBases)
    {
        variantBases[variantIndex].variantId = entry.first;
        variantBases[variantIndex].baseId = entry.second;
        ++variantIndex;
    }
    std::sort(variantBases.begin(), variantBases.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.variantId < rhs.variantId; });

    registryMaterials.resize(counts.registryMaterials);
    for (std::size_t index = 0; index < counts.registryMaterials; ++index)
    {
        const RtSmokeMaterialTextureInfo& info = g_smokeMaterialTextureRegistry[index];
        RtPathTraceCaptureRegistryMaterialPod& pod = registryMaterials[index];
        pod.materialId = info.materialId;
        RtPathTracePlanningCopyName(pod.materialName, sizeof(pod.materialName),
            info.materialName.c_str());
        pod.hardwareOpaqueGeometry = info.hardwareOpaqueGeometry;
        pod.hasAlphaTest = info.hasAlphaTest;
        pod.alphaCutoff = info.alphaCutoff;
        pod.emissive = info.emissive;
        pod.skyEnvironment = info.skyEnvironment;
        pod.materialMetadataValid =
            SmokeMaterialTextureInfoHasMaterialMetadata(info);
        pod.isDynamic = info.isDynamic;
        pod.detailDecalLiquidPool = info.detailDecalLiquidPool;
    }
    std::sort(registryMaterials.begin(), registryMaterials.end(),
        [](const auto& lhs, const auto& rhs) { return lhs.materialId < rhs.materialId; });
    return true;
}

RtSmokeMaterialTextureRegistryBumpStats SmokeMaterialTextureRegistryBumpStats()
{
    const bool enabled = SmokeMaterialTextureRegistryBumpTelemetryEnabled();
    if (enabled)
    {
        AdvanceSmokeMaterialTextureRegistryBumpFrame();
    }
    g_smokeMaterialTextureRegistryBumpStats.enabled = enabled;
    return g_smokeMaterialTextureRegistryBumpStats;
}

#if defined(RT_PT_TEXTURE_REGISTRY_HARNESS)
bool SmokeMaterialTextureRegistryAtomicPublicationSelfTest()
{
    const auto reset = []()
    {
        g_atomicPublishFailurePoint = AtomicPublishFailurePoint::None;
        retainedBindingDefinition.reset();++bindingClearEpoch;retainedBindingMembershipRevision=0;
        g_bindingDefinitionRevision=1;g_bindingMembershipRevision=1;
        g_smokeMaterialTextureRegistry.clear();
        g_smokeMaterialTextureRegistryLookup.clear();
        g_smokeMaterialTextureVariantBases.clear();
        g_smokeMaterialTextureRegistryGeneration = 1;
        g_smokeMaterialTextureRegistryBumpStats = {};
    };
    const auto stateMatches = [](std::size_t rows, std::size_t lookups,
                                 std::size_t variants, uint64 generation)
    {
        return g_smokeMaterialTextureRegistry.size() == rows &&
            g_smokeMaterialTextureRegistryLookup.size() == lookups &&
            g_smokeMaterialTextureVariantBases.size() == variants &&
            g_smokeMaterialTextureRegistryGeneration == generation;
    };
    const auto expectThrow = [](const auto& action)
    {
        try
        {
            action();
        }
        catch (const std::bad_alloc&)
        {
            return true;
        }
        return false;
    };
    const auto enumerationValid = []()
    {
        const RtSmokeMaterialTextureRegistryEnumerationCounts counts =
            CountSmokeMaterialTextureRegistryEnumeration();
        std::vector<RtPathTraceMaterialTextureVariantBasePod> variantPods;
        std::vector<RtPathTraceCaptureRegistryMaterialPod> materialPods;
        variantPods.reserve(counts.variantBases);
        materialPods.reserve(counts.registryMaterials);
        if (!FillSmokeMaterialTextureRegistryEnumerationPreReserved(
                counts, variantPods, materialPods) ||
            variantPods.size() != counts.variantBases ||
            materialPods.size() != counts.registryMaterials)
        {
            return false;
        }
        for (std::size_t index = 1; index < materialPods.size(); ++index)
        {
            if (materialPods[index - 1].materialId >=
                materialPods[index].materialId)
            {
                return false;
            }
        }
        for (std::size_t index = 1; index < variantPods.size(); ++index)
        {
            if (variantPods[index - 1].variantId >=
                variantPods[index].variantId)
            {
                return false;
            }
        }
        return true;
    };
    const auto buildProductionShapedRow = [](uint32_t materialId,
                                             bool failDuringConstruction)
    {
        RtSmokeMaterialTextureInfo row;
        row.materialId = materialId;
        row.materialName = "materials/atomic";
        row.diffuseImageName = "textures/atomic_d";
        row.alphaImageName = "textures/atomic_a";
        row.fallbackReason = "complete fallback";
        if (failDuringConstruction)
        {
            throw std::bad_alloc();
        }
        row.liquidFilmCandidate = true;
        row.liquidFilmHasCoverageSource = true;
        row.liquidFilmCoverageImageName = "textures/liquid";
        row.liquidFilmOverrideReason = "override";
        row.liquidFilmReason = "reason";
        row.hasAlphaTest = true;
        row.alphaCutoff = 0.375f;
        row.detailDecalLiquidPool = true;
        row.emissive = true;
        row.isDynamic = false;
        row.tableIndex = -1;
        row.hardwareOpaqueGeometry =
            ComputeSmokeMaterialHardwareOpaqueGeometry(row);
        RefreshUnpublishedSmokeMaterialTextureHandleState(row);
        return row;
    };
    const auto productionRowComplete = [](const RtSmokeMaterialTextureInfo* row,
                                          uint32_t materialId)
    {
        return row && row->materialId == materialId &&
            row->materialName == "materials/atomic" &&
            row->diffuseImageName == "textures/atomic_d" &&
            row->alphaImageName == "textures/atomic_a" &&
            row->fallbackReason == "complete fallback" &&
            row->liquidFilmCandidate &&
            row->liquidFilmHasCoverageSource &&
            row->liquidFilmCoverageImageName == "textures/liquid" &&
            row->liquidFilmOverrideReason == "override" &&
            row->liquidFilmReason == "reason" && row->hasAlphaTest &&
            row->alphaCutoff == 0.375f && row->detailDecalLiquidPool &&
            row->emissive && !row->isDynamic && row->tableIndex == -1 &&
            row->hardwareOpaqueGeometry ==
                ComputeSmokeMaterialHardwareOpaqueGeometry(*row);
    };

    // Fail-before oracle: the former discovery ordering published a skeletal
    // row before its throwing semantic builder ran.
    reset();
    AddSmokeMaterialTextureInfo(10, "materials/atomic");
    if (!expectThrow([&]() {
            (void)buildProductionShapedRow(10, true);
        }) || !stateMatches(1, 1, 0, 2) ||
        productionRowComplete(LookupSmokeMaterialTextureInfo(10), 10))
    {
        return false;
    }

    // Pass-after contract used by both production discovery paths: construct
    // off-side first, then enter the bounded complete-row publication.
    reset();
    if (!expectThrow([&]() {
            RtSmokeMaterialTextureInfo row =
                buildProductionShapedRow(10, true);
            PublishCompleteSmokeMaterialTextureInfo(std::move(row));
        }) || !stateMatches(0, 0, 0, 1) || !enumerationValid())
    {
        return false;
    }

    const AtomicPublishFailurePoint addFailures[] = {
        AtomicPublishFailurePoint::VectorReserve,
        AtomicPublishFailurePoint::LookupReserve,
        AtomicPublishFailurePoint::LookupNodeStage,
        AtomicPublishFailurePoint::FinalPublish,
        AtomicPublishFailurePoint::LookupInsert
    };
    for (AtomicPublishFailurePoint failure : addFailures)
    {
        reset();
        RtSmokeMaterialTextureInfo completeInfo =
            buildProductionShapedRow(10, false);
        g_atomicPublishFailurePoint = failure;
        if (!expectThrow([&]() {
                PublishCompleteSmokeMaterialTextureInfo(
                    std::move(completeInfo));
            }) ||
            !stateMatches(0, 0, 0, 1) || !enumerationValid())
        {
            return false;
        }
        g_atomicPublishFailurePoint = AtomicPublishFailurePoint::None;
        RtSmokeMaterialTextureInfo retry =
            buildProductionShapedRow(10, false);
        PublishCompleteSmokeMaterialTextureInfo(std::move(retry));
        if (!stateMatches(1, 1, 0, 2) ||
            !productionRowComplete(LookupSmokeMaterialTextureInfo(10), 10) ||
            !enumerationValid())
        {
            return false;
        }
    }

    // R4-014: repeated complete resident publication must not invalidate tables.
    reset();
    PublishCompleteSmokeMaterialTextureInfo(buildProductionShapedRow(10, false));
    const uint64 unchangedGeneration = g_smokeMaterialTextureRegistryGeneration;
    PublishCompleteSmokeMaterialTextureInfo(buildProductionShapedRow(10, false));
    if (g_smokeMaterialTextureRegistryGeneration != unchangedGeneration) return false;
    RtSmokeMaterialTextureInfo changedResident = buildProductionShapedRow(10, false);
    changedResident.alphaCutoff += 0.125f;
    PublishCompleteSmokeMaterialTextureInfo(std::move(changedResident));
    if (g_smokeMaterialTextureRegistryGeneration != unchangedGeneration + 1) return false;

    // Replacement commits by a single nonthrowing row swap. A failure at the
    // boundary preserves every previously published decision-bearing field.
    reset();
    RtSmokeMaterialTextureInfo original =
        buildProductionShapedRow(10, false);
    PublishCompleteSmokeMaterialTextureInfo(std::move(original));
    const uint64 replacementGeneration =
        g_smokeMaterialTextureRegistryGeneration;
    RtSmokeMaterialTextureInfo replacement =
        buildProductionShapedRow(10, false);
    replacement.materialName = "materials/replacement";
    replacement.fallbackReason = "replacement fallback";
    replacement.liquidFilmReason = "replacement liquid";
    replacement.emissive = false;
    replacement.isDynamic = true;
    g_atomicPublishFailurePoint = AtomicPublishFailurePoint::FinalPublish;
    if (!expectThrow([&]() {
            PublishCompleteSmokeMaterialTextureInfo(std::move(replacement));
        }) ||
        g_smokeMaterialTextureRegistryGeneration != replacementGeneration ||
        !productionRowComplete(LookupSmokeMaterialTextureInfo(10), 10) ||
        !enumerationValid())
    {
        return false;
    }
    g_atomicPublishFailurePoint = AtomicPublishFailurePoint::None;
    replacement = buildProductionShapedRow(10, false);
    replacement.materialName = "materials/replacement";
    replacement.fallbackReason = "replacement fallback";
    replacement.liquidFilmReason = "replacement liquid";
    replacement.emissive = false;
    replacement.isDynamic = true;
    PublishCompleteSmokeMaterialTextureInfo(std::move(replacement));
    const RtSmokeMaterialTextureInfo* replaced =
        LookupSmokeMaterialTextureInfo(10);
    if (!replaced || replaced->materialName != "materials/replacement" ||
        replaced->fallbackReason != "replacement fallback" ||
        replaced->liquidFilmReason != "replacement liquid" ||
        replaced->emissive || !replaced->isDynamic ||
        g_smokeMaterialTextureRegistryGeneration != replacementGeneration + 1)
    {
        return false;
    }

    const AtomicPublishFailurePoint variantFailures[] = {
        AtomicPublishFailurePoint::VectorReserve,
        AtomicPublishFailurePoint::LookupReserve,
        AtomicPublishFailurePoint::VariantBaseReserve,
        AtomicPublishFailurePoint::LookupNodeStage,
        AtomicPublishFailurePoint::VariantBaseNodeStage,
        AtomicPublishFailurePoint::VariantBaseInsert,
        AtomicPublishFailurePoint::FinalPublish,
        AtomicPublishFailurePoint::LookupInsert
    };
    const AtomicPublishFailurePoint updateFailures[] = {
        AtomicPublishFailurePoint::FinalPublish
    };
    for (AtomicPublishFailurePoint failure : variantFailures)
    {
        reset();
        RtSmokeMaterialTextureInfo& base =
            AddSmokeMaterialTextureInfo(10, "base/ten");
        base.liquidFilmCandidate = true;
        base.liquidFilmCoverageImageName = "textures/liquid";
        base.liquidFilmOverrideReason = "override";
        base.liquidFilmReason = "reason";
        base.hardwareOpaqueGeometry =
            ComputeSmokeMaterialHardwareOpaqueGeometry(base);
        const uint64 before = g_smokeMaterialTextureRegistryGeneration;
        g_atomicPublishFailurePoint = failure;
        if (!expectThrow([]() { RegisterSmokeMaterialTextureVariant(20, 10); }) ||
            !stateMatches(1, 1, 0, before) || !enumerationValid())
        {
            return false;
        }
        g_atomicPublishFailurePoint = AtomicPublishFailurePoint::None;
        if (!RegisterSmokeMaterialTextureVariant(20, 10) ||
            !stateMatches(2, 2, 1, before + 1) ||
            SmokeMaterialTextureVariantBase(20) != 10)
        {
            return false;
        }
        const RtSmokeMaterialTextureInfo* variant =
            LookupSmokeMaterialTextureInfo(20);
        const RtSmokeMaterialTextureInfo* publishedBase =
            LookupSmokeMaterialTextureInfo(10);
        if (!variant || !variant->liquidFilmCandidate ||
            !publishedBase ||
            variant->liquidFilmCoverageImageName != "textures/liquid" ||
            variant->liquidFilmOverrideReason != "override" ||
            variant->liquidFilmReason != "reason" ||
            variant->hardwareOpaqueGeometry !=
                ComputeSmokeMaterialHardwareOpaqueGeometry(*variant) ||
            variant->diffuseTexture.Get() != publishedBase->diffuseTexture.Get() ||
            variant->alphaTexture.Get() != publishedBase->alphaTexture.Get() ||
            variant->normalTexture.Get() != publishedBase->normalTexture.Get() ||
            variant->specularTexture.Get() != publishedBase->specularTexture.Get() ||
            variant->emissiveTexture.Get() != publishedBase->emissiveTexture.Get() ||
            variant->skyTexture.Get() != publishedBase->skyTexture.Get() ||
            !enumerationValid())
        {
            return false;
        }
    }

    reset();
    RtSmokeMaterialTextureInfo& duplicateBase =
        AddSmokeMaterialTextureInfo(10, "base/ten");
    if (g_smokeMaterialTextureRegistryGeneration != 2 ||
        duplicateBase.materialName != "base/ten" ||
        duplicateBase.diffuseTexture.Get() != nullptr ||
        duplicateBase.alphaTexture.Get() != nullptr ||
        duplicateBase.hasTextureHandle ||
        duplicateBase.hasAlphaTextureHandle ||
        duplicateBase.hardwareOpaqueGeometry ||
        duplicateBase.liquidFilmCandidate ||
        !duplicateBase.liquidFilmCoverageImageName.IsEmpty() ||
        !duplicateBase.liquidFilmOverrideReason.IsEmpty() ||
        !duplicateBase.liquidFilmReason.IsEmpty())
    {
        return false;
    }
    const uint64 duplicateGeneration = g_smokeMaterialTextureRegistryGeneration;
    RtSmokeMaterialTextureInfo& sameBase =
        AddSmokeMaterialTextureInfo(10, "base/ignored");
    if (&duplicateBase != &sameBase ||
        !stateMatches(1, 1, 0, duplicateGeneration))
    {
        return false;
    }

    reset();
    AddSmokeMaterialTextureInfo(10, "base/ten");
    if (!RegisterSmokeMaterialTextureVariant(20, 10))
    {
        return false;
    }
    const uint64 prefixGeneration = g_smokeMaterialTextureRegistryGeneration;
    g_atomicPublishFailurePoint = AtomicPublishFailurePoint::VariantBaseInsert;
    if (!expectThrow([]() { RegisterSmokeMaterialTextureVariant(30, 10); }) ||
        !stateMatches(2, 2, 1, prefixGeneration) ||
        !LookupSmokeMaterialTextureInfo(20) || LookupSmokeMaterialTextureInfo(30) ||
        !enumerationValid())
    {
        return false;
    }

    // Existing-row refresh is candidate-only until the final no-throw publish.
    reset();
    AddSmokeMaterialTextureInfo(10, "base/update");
    if (!RegisterSmokeMaterialTextureVariant(20, 10))
    {
        return false;
    }
    RtSmokeMaterialTextureInfo* updateBase =
        LookupSmokeMaterialTextureInfo(10);
    if (!updateBase)
    {
        return false;
    }
    updateBase->liquidFilmCandidate = true;
    updateBase->liquidFilmCoverageImageName = "textures/updated";
    const uint64 updateGeneration = g_smokeMaterialTextureRegistryGeneration;
    for (AtomicPublishFailurePoint failure : updateFailures)
    {
        g_atomicPublishFailurePoint = failure;
        if (!expectThrow([]() { RegisterSmokeMaterialTextureVariant(20, 10); }) ||
            g_smokeMaterialTextureRegistryGeneration != updateGeneration)
        {
            return false;
        }
        const RtSmokeMaterialTextureInfo* unchanged =
            LookupSmokeMaterialTextureInfo(20);
        if (!unchanged || unchanged->liquidFilmCandidate ||
            !unchanged->liquidFilmCoverageImageName.IsEmpty() ||
            !enumerationValid())
        {
            return false;
        }
    }
    g_atomicPublishFailurePoint = AtomicPublishFailurePoint::None;
    if (!RegisterSmokeMaterialTextureVariant(20, 10) ||
        g_smokeMaterialTextureRegistryGeneration != updateGeneration + 1)
    {
        return false;
    }

    // A hidden pre-atomic row is repairable only when stored ownership proves
    // the proposed base. The same poison without ownership must be rejected.
    reset();
    RtSmokeMaterialTextureInfo& repairBase =
        AddSmokeMaterialTextureInfo(10, "base/repair");
    RtSmokeMaterialTextureInfo hiddenVariant = repairBase;
    hiddenVariant.materialId = 20;
    hiddenVariant.tableIndex = -1;
    g_smokeMaterialTextureRegistry.push_back(hiddenVariant);
    g_smokeMaterialTextureVariantBases.emplace(20, 10);
    const uint64 repairGeneration = g_smokeMaterialTextureRegistryGeneration;
    if (!RegisterSmokeMaterialTextureVariant(20, 10) ||
        !LookupSmokeMaterialTextureInfo(20) ||
        g_smokeMaterialTextureRegistryGeneration != repairGeneration + 1 ||
        !enumerationValid())
    {
        return false;
    }

    reset();
    RtSmokeMaterialTextureInfo& poisonBase =
        AddSmokeMaterialTextureInfo(10, "base/poison");
    RtSmokeMaterialTextureInfo unownedVariant = poisonBase;
    unownedVariant.materialId = 20;
    g_smokeMaterialTextureRegistry.push_back(unownedVariant);
    g_smokeMaterialTextureRegistryLookup.emplace(20, 1);
    const uint64 poisonGeneration = g_smokeMaterialTextureRegistryGeneration;
    if (RegisterSmokeMaterialTextureVariant(20, 10) ||
        g_smokeMaterialTextureRegistryGeneration != poisonGeneration ||
        g_smokeMaterialTextureVariantBases.find(20) !=
            g_smokeMaterialTextureVariantBases.end())
    {
        return false;
    }

    return enumerationValid();
}
#endif

void SnapshotSmokeMaterialIdentities(std::vector<RtCpuRewriteMaterialIdentity>& identities)
{
    identities.clear();
    identities.reserve(g_smokeMaterialTextureRegistryLookup.size());
    for (const auto& entry : g_smokeMaterialTextureRegistryLookup)
    {
        const auto* info = LookupSmokeMaterialTextureInfo(entry.first);
        if (!info) continue;
        const auto variant = g_smokeMaterialTextureVariantBases.find(entry.first);
        identities.push_back({ entry.first, variant == g_smokeMaterialTextureVariantBases.end() ? 0u : variant->second,
            SmokeMaterialTextureInfoHasMaterialMetadata(*info) && !info->isDynamic });
    }
}

#if defined(RT_PT_TEXTURE_REGISTRY_HARNESS)
bool SmokeMaterialBindingDefinitionSelfTest()
{
    ClearSmokeMaterialTextureRegistry();
    RtSmokeMaterialTextureInfo base;base.materialId=310;
    base.diffuseImageName="textures/stone";
    base.diffuseImage=reinterpret_cast<idImage*>(uintptr_t(0x100)); // identity only; harness never dereferences
    PublishCompleteSmokeMaterialTextureInfo(std::move(base));
    const std::vector<uint32_t> active{310};
    const uint64_t budget=RtCpuMaterialBindingPlanInput::kMaxBytes;
    RtCpuMaterialBindingPlanInput input;
    auto cold=SnapshotSmokeMaterialBindingFrame(input,budget,&active);
    RtCpuMaterialBindingPlan plan;
    if (!cold || !SmokeMaterialBindingNeedsPreparation(*cold) || input.rows.size()!=1 ||
        !BuildRtCpuMaterialBindingPlan(input,plan) || !ResolveSmokeMaterialBindingResources(*cold)) return false;
    for (auto& image:cold->images) image.twoD=true;
    if (!CompleteSmokeMaterialBindingFrame(*cold,std::move(plan))) return false;
    const auto definition=cold->definition;
    const uint32_t rule=definition->plan.rows.at(310)[0];
    if (!cold->safety[rule] || !definition->plan.safety[rule]) return false;
    auto hit=SnapshotSmokeMaterialBindingFrame(input,budget,&active);
    if (!hit || SmokeMaterialBindingNeedsPreparation(*hit) || !input.rows.empty() ||
        hit->definition!=definition || !ResolveSmokeMaterialBindingResources(*hit) ||
        !CompleteSmokeMaterialBindingFrame(*hit,{})) return false;
    if (hit->safety[rule] || !definition->plan.safety[rule]) return false;
    auto recovered=SnapshotSmokeMaterialBindingFrame(input,budget,&active);
    if (!recovered || !ResolveSmokeMaterialBindingResources(*recovered)) return false;
    for (auto& image:recovered->images) image.twoD=true;
    if (!CompleteSmokeMaterialBindingFrame(*recovered,{}) || !recovered->safety[rule]) return false;
    // Exact witness sees direct mutable row edits even without registry publication.
    const auto generation=SmokeMaterialTextureRegistryGeneration();
    auto* live=FindSmokeMaterialTextureInfo(310);live->diffuseImageName="textures/changed";
    auto miss=SnapshotSmokeMaterialBindingFrame(input,budget,&active);
    if (!miss || !SmokeMaterialBindingNeedsPreparation(*miss) || input.rows.size()!=1 ||
        generation!=SmokeMaterialTextureRegistryGeneration()) return false;
    live->diffuseImageName="textures/stone";
    live->diffuseImage=reinterpret_cast<idImage*>(uintptr_t(0x200));
    miss=SnapshotSmokeMaterialBindingFrame(input,budget,&active);
    if (!miss || !SmokeMaterialBindingNeedsPreparation(*miss)) return false;
    live->diffuseImage=reinterpret_cast<idImage*>(uintptr_t(0x100));
    std::vector<uint32_t> bases{310};RtSmokeMaterialBindingFrame probe;
    if (ReuseSmokeMaterialBindingDefinition(probe,&bases,budget,true)!=2 ||
        ReuseSmokeMaterialBindingDefinition(probe,&bases,1,false)!=3) return false;
    const std::vector<uint32_t> none{999};
    miss=SnapshotSmokeMaterialBindingFrame(input,budget,&none);
    if (!miss || !SmokeMaterialBindingNeedsPreparation(*miss) || !input.rows.empty()) return false;
    RtSmokeMaterialTextureInfo extra;extra.materialId=311;extra.diffuseImageName="textures/other";
    PublishCompleteSmokeMaterialTextureInfo(std::move(extra));
    const std::vector<uint32_t> both{310,311};
    miss=SnapshotSmokeMaterialBindingFrame(input,budget,&both);
    if (!miss || !SmokeMaterialBindingNeedsPreparation(*miss) || input.rows.size()!=2) return false;
    // Unselected new rows do not invalidate this exact active definition.
    auto pending=SnapshotSmokeMaterialBindingFrame(input,budget,&active);
    if (!pending || SmokeMaterialBindingNeedsPreparation(*pending)) return false;
    ClearSmokeMaterialTextureRegistry();
    if (ResolveSmokeMaterialBindingResources(*pending) || CompleteSmokeMaterialBindingFrame(*pending,{}) ||
        retainedBindingDefinition || !definition->plan.safety[rule]) return false;
    RtSmokeMaterialTextureInfo restored;restored.materialId=310;restored.materialName="materials/stone";restored.diffuseImageName="textures/stone";
    PublishCompleteSmokeMaterialTextureInfo(std::move(restored));
    miss=SnapshotSmokeMaterialBindingFrame(input,budget,&active);
    if (!miss || !SmokeMaterialBindingNeedsPreparation(*miss) || SnapshotSmokeMaterialBindingFrame(input,0,&active)) return false;
    // Internal variants have no mutable-address escape. Stable revisions skip
    // exact names while current resources still resolve on every frame.
    if (!RegisterSmokeMaterialTextureVariant(312,310)) return false;
    if (FindSmokeMaterialTextureInfoReadOnly(312)->bindingMutableExposed) return false;
    auto establish=[&]() {
        auto fresh=SnapshotSmokeMaterialBindingFrame(input,budget,&active);
        RtCpuMaterialBindingPlan prepared;
        if (!fresh || !BuildRtCpuMaterialBindingPlan(input,prepared) ||
            !ResolveSmokeMaterialBindingResources(*fresh) ||
            !CompleteSmokeMaterialBindingFrame(*fresh,std::move(prepared))) return false;
        return true;
    };
    if (!establish()) return false;
    auto trusted=SnapshotSmokeMaterialBindingFrame(input,budget,&active);
    if (!trusted || SmokeMaterialBindingNeedsPreparation(*trusted) ||
        trusted->revisionRowsSkipped!=1 || trusted->mutableRowsChecked!=1 || trusted->membershipScans!=0) return false;
    // Table hydration must not turn internally published variants into escaped rows.
    auto hydrationFrame=SnapshotSmokeMaterialBindingFrame(input,budget,&active);
    if (!hydrationFrame || !ResolveSmokeMaterialBindingResources(*hydrationFrame) ||
        !CompleteSmokeMaterialBindingFrame(*hydrationFrame,{})) return false;
    LookupSmokeMaterialTextureInfo(312)->hasSafeTexture=true; // stale handle fact only
    const auto hydrationRevision=g_bindingDefinitionRevision;
    if (PrepareSmokeMaterialHydrationIds({310,312,312,999,310},hydrationFrame.get())!=
        std::vector<uint32_t>({310,999,310}) ||
        LookupSmokeMaterialTextureInfo(312)->bindingMutableExposed ||
        LookupSmokeMaterialTextureInfo(312)->hasSafeTexture || g_bindingDefinitionRevision!=hydrationRevision) return false;
    auto afterHydration=SnapshotSmokeMaterialBindingFrame(input,budget,&active);
    if (!afterHydration || SmokeMaterialBindingNeedsPreparation(*afterHydration) ||
        afterHydration->revisionRowsSkipped!=1) return false;
    // An incomplete owned variant still requires the original hydration path.
    auto* incomplete=LookupSmokeMaterialTextureInfo(312);
    const idStr savedName=incomplete->materialName;incomplete->materialName="<none>";
    if (PrepareSmokeMaterialHydrationIds({312})!=std::vector<uint32_t>({312})) return false;
    incomplete->materialName=savedName;
    if (!PrepareSmokeMaterialHydrationIds({}).empty()) return false;
    const auto revision=g_bindingDefinitionRevision,membershipRevision=g_bindingMembershipRevision;
    const auto rowRevision=LookupSmokeMaterialTextureInfo(312)->bindingDefinitionRevision;
    auto publishVariant=[&](RtSmokeMaterialTextureInfo&& row) {
        PublishCompleteSmokeMaterialTextureInfoImpl(std::move(row),310,
            RtSmokeMaterialTextureRegistryBumpSite::AddVariant,
            RtSmokeMaterialTextureRegistryBumpSite::UpdateVariantFacts);
    };
    auto update=*LookupSmokeMaterialTextureInfo(312);
    update.hasSafeTexture=!update.hasSafeTexture;publishVariant(std::move(update));
    if (g_bindingDefinitionRevision!=revision || g_bindingMembershipRevision!=membershipRevision ||
        LookupSmokeMaterialTextureInfo(312)->bindingDefinitionRevision!=rowRevision) return false;
    update=*LookupSmokeMaterialTextureInfo(312);update.diffuseImageName="textures/new_name";
    g_atomicPublishFailurePoint=AtomicPublishFailurePoint::FinalPublish;
    bool failed=false;
    try { publishVariant(std::move(update)); } catch (const std::bad_alloc&) { failed=true; }
    g_atomicPublishFailurePoint=AtomicPublishFailurePoint::None;
    if (!failed || g_bindingDefinitionRevision!=revision || g_bindingMembershipRevision!=membershipRevision ||
        LookupSmokeMaterialTextureInfo(312)->bindingDefinitionRevision!=rowRevision) return false;
    update=*LookupSmokeMaterialTextureInfo(312);update.diffuseImageName="textures/new_name";
    publishVariant(std::move(update));
    if (g_bindingDefinitionRevision==revision || LookupSmokeMaterialTextureInfo(312)->bindingMutableExposed ||
        !ResolveSmokeMaterialBindingResources(*trusted) || CompleteSmokeMaterialBindingFrame(*trusted,{})) return false;
    miss=SnapshotSmokeMaterialBindingFrame(input,budget,&active);
    if (!miss || !SmokeMaterialBindingNeedsPreparation(*miss) || !establish()) return false;
    update=*LookupSmokeMaterialTextureInfo(312);
    update.diffuseImage=reinterpret_cast<idImage*>(uintptr_t(0x300));publishVariant(std::move(update));
    miss=SnapshotSmokeMaterialBindingFrame(input,budget,&active);
    if (!miss || !SmokeMaterialBindingNeedsPreparation(*miss) || !establish()) return false;
    // Even a formerly trusted row becomes exact-checked once a mutable pointer escapes.
    auto* exposed=FindSmokeMaterialTextureInfo(312);exposed->diffuseImageName="textures/direct";
    PrepareSmokeMaterialHydrationIds({312});
    if (!exposed->bindingMutableExposed) return false;
    miss=SnapshotSmokeMaterialBindingFrame(input,budget,&active);
    if (!miss || !SmokeMaterialBindingNeedsPreparation(*miss)) return false;
    const auto beforeAdd=g_bindingMembershipRevision;
    g_atomicPublishFailurePoint=AtomicPublishFailurePoint::FinalPublish;
    failed=false;
    try { RegisterSmokeMaterialTextureVariant(313,310); } catch (const std::bad_alloc&) { failed=true; }
    g_atomicPublishFailurePoint=AtomicPublishFailurePoint::None;
    if (!failed || g_bindingMembershipRevision!=beforeAdd || LookupSmokeMaterialTextureInfo(313)) return false;
    ClearSmokeMaterialTextureVariants();
    miss=SnapshotSmokeMaterialBindingFrame(input,budget,&active);
    if (!miss || !SmokeMaterialBindingNeedsPreparation(*miss) || input.rows.size()!=1) return false;
    ClearSmokeMaterialTextureRegistry();
    return true;
}
#endif
