#include "precompiled.h"
#pragma hdrstop

// Persistent material-record cache for the RT smoke/path tracing path.
//
// The records here intentionally mirror only the stable part of the material
// model. Per-frame texture descriptor indexes and compact shader table row order
// are still assigned by PathTraceDynamicMaterialState after the current visible
// texture set is known.

#include "PathTraceCVars.h"
#include "PathTraceDoomMaterialClassifierKernel.h"
#include "PathTraceDynamicMaterialState.h"
#include "PathTraceMaterialUniverse.h"

#include <unordered_map>

namespace {

struct RtSmokeMaterialUniverseKey
{
    uint32_t materialId = 0;
    uint64 materialNameHash = 0;

    bool operator==(const RtSmokeMaterialUniverseKey& rhs) const
    {
        return materialId == rhs.materialId && materialNameHash == rhs.materialNameHash;
    }
};

struct RtSmokeMaterialUniverseKeyHash
{
    size_t operator()(const RtSmokeMaterialUniverseKey& key) const
    {
        uint64 hash = key.materialNameHash;
        hash ^= static_cast<uint64>(key.materialId) + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
        return static_cast<size_t>(hash ^ (hash >> 32));
    }
};

std::unordered_map<RtSmokeMaterialUniverseKey, RtSmokePersistentMaterialRecord, RtSmokeMaterialUniverseKeyHash> g_smokePersistentMaterialRecords;
RtSmokeMaterialUniverseStats g_smokeMaterialUniverseStats;
uint32_t g_smokeNextMaterialUniverseIndex = 0;

uint64 HashSmokeMaterialUniverseValue(uint64 hash, uint64 value)
{
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash;
}

uint64 HashSmokeMaterialUniverseFloat(uint64 hash, float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return HashSmokeMaterialUniverseValue(hash, bits);
}

uint64 HashSmokeMaterialUniverseString(uint64 hash, const idStr& value)
{
    const char* cursor = value.c_str();
    while (*cursor)
    {
        hash = HashSmokeMaterialUniverseValue(hash, static_cast<uint8_t>(*cursor));
        ++cursor;
    }
    return HashSmokeMaterialUniverseValue(hash, 0xffu);
}

RtSmokeMaterialUniverseKey BuildSmokeMaterialUniverseKey(uint32_t materialId, const RtSmokeMaterialTextureInfo& info)
{
    RtSmokeMaterialUniverseKey key;
    key.materialId = materialId;
    key.materialNameHash = HashSmokeMaterialUniverseString(1469598103934665603ull, info.materialName);
    return key;
}

idVec3 SmokeMaterialIdToDebugColor(uint32_t materialId)
{
    uint32_t hash = materialId;
    hash ^= hash >> 16;
    hash *= 2246822519u;
    hash ^= hash >> 13;
    hash *= 3266489917u;
    hash ^= hash >> 16;

    return idVec3(
        0.15f + static_cast<float>((hash >> 0) & 255u) * (0.85f / 255.0f),
        0.15f + static_cast<float>((hash >> 8) & 255u) * (0.85f / 255.0f),
        0.15f + static_cast<float>((hash >> 16) & 255u) * (0.85f / 255.0f));
}

float SmokeUniverseEmissiveLuminance(const idVec4& color)
{
    const float r = Max(0.0f, color.x);
    const float g = Max(0.0f, color.y);
    const float b = Max(0.0f, color.z);
    return r * 0.2126f + g * 0.7152f + b * 0.0722f;
}

bool SmokeUniverseHasGuiTextureCandidate(const RtSmokeMaterialTextureInfo& info)
{
    return IsSmokeImageNameGuiLike(info.diffuseImageName.c_str()) ||
        IsSmokeImageNameGuiLike(info.alphaImageName.c_str()) ||
        IsSmokeImageNameGuiLike(info.normalImageName.c_str()) ||
        IsSmokeImageNameGuiLike(info.specularImageName.c_str()) ||
        IsSmokeImageNameGuiLike(info.emissiveImageName.c_str());
}

uint64 ComputeSmokePersistentMaterialSignature(uint32_t materialId, const RtSmokeMaterialTextureInfo& info)
{
    uint64 hash = 1469598103934665603ull;
    hash = HashSmokeMaterialUniverseValue(hash, materialId);
    hash = HashSmokeMaterialUniverseString(hash, info.materialName);
    hash = HashSmokeMaterialUniverseValue(hash, info.hasDiffuseImage ? 1u : 0u);
    hash = HashSmokeMaterialUniverseString(hash, info.diffuseImageName);
    hash = HashSmokeMaterialUniverseValue(hash, info.hasTextureHandle ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.hasSafeTexture ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.hasAlphaImage ? 1u : 0u);
    hash = HashSmokeMaterialUniverseString(hash, info.alphaImageName);
    hash = HashSmokeMaterialUniverseValue(hash, info.hasAlphaTextureHandle ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.hasSafeAlphaTexture ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.hasNormalImage ? 1u : 0u);
    hash = HashSmokeMaterialUniverseString(hash, info.normalImageName);
    hash = HashSmokeMaterialUniverseValue(hash, info.hasNormalTextureHandle ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.hasSafeNormalTexture ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.hasSpecularImage ? 1u : 0u);
    hash = HashSmokeMaterialUniverseString(hash, info.specularImageName);
    hash = HashSmokeMaterialUniverseValue(hash, info.hasSpecularTextureHandle ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.hasSafeSpecularTexture ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.hasEmissiveImage ? 1u : 0u);
    hash = HashSmokeMaterialUniverseString(hash, info.emissiveImageName);
    hash = HashSmokeMaterialUniverseValue(hash, info.hasEmissiveTextureHandle ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.hasSafeEmissiveTexture ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.hasFallbackAlbedo ? 1u : 0u);
    hash = HashSmokeMaterialUniverseFloat(hash, info.fallbackAlbedo.x);
    hash = HashSmokeMaterialUniverseFloat(hash, info.fallbackAlbedo.y);
    hash = HashSmokeMaterialUniverseFloat(hash, info.fallbackAlbedo.z);
    hash = HashSmokeMaterialUniverseFloat(hash, info.fallbackAlbedo.w);
    hash = HashSmokeMaterialUniverseValue(hash, info.hasAlphaTest ? 1u : 0u);
    hash = HashSmokeMaterialUniverseFloat(hash, info.alphaCutoff);
    hash = HashSmokeMaterialUniverseValue(hash, static_cast<uint64>(info.diffuseColorFormat));
    hash = HashSmokeMaterialUniverseValue(hash, info.additiveDecal ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.additiveDecalWhiteKey ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, r_pathTracingAdditiveDecalKey.GetInteger() != 0 ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.filterDecal ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.filterDecalBlackKey ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.detailDecal ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.detailDecalDynamic ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.isDynamic ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.detailDecalDiffuseLit ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.detailDecalLiquidPool ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.liquidFilmHasBloodSemantic ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.liquidFilmHasWetReflectStage ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.liquidFilmHasCoverageSource ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.liquidFilmHasWetNormalSource ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.liquidFilmExactOverride ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.liquidFilmCandidate ? 1u : 0u);
    hash = HashSmokeMaterialUniverseString(hash, info.liquidFilmCoverageImageName);
    hash = HashSmokeMaterialUniverseString(hash, info.liquidFilmOverrideReason);
    hash = HashSmokeMaterialUniverseString(hash, info.liquidFilmReason);
    hash = HashSmokeMaterialUniverseValue(hash, static_cast<uint64>(Max(0, info.detailDecalSpectrum)));
    hash = HashSmokeMaterialUniverseValue(hash, info.alphaFromDiffuseLuma ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.forceFallbackAlbedo ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.alphaFromDiffuseDarkKey ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.alphaFromDiffuseMagentaKey ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.portalWindowFallback ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.objectGlassFallback ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.emissive ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.emissiveLightCandidate ? 1u : 0u);
    hash = HashSmokeMaterialUniverseFloat(hash, info.emissiveColor.x);
    hash = HashSmokeMaterialUniverseFloat(hash, info.emissiveColor.y);
    hash = HashSmokeMaterialUniverseFloat(hash, info.emissiveColor.z);
    hash = HashSmokeMaterialUniverseFloat(hash, info.emissiveColor.w);
    hash = HashSmokeMaterialUniverseValue(hash, info.skyEnvironment ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.hasSkyTextureHandle ? 1u : 0u);
    hash = HashSmokeMaterialUniverseValue(hash, info.hasSafeSkyTexture ? 1u : 0u);
    hash = HashSmokeMaterialUniverseString(hash, info.skyImageName);
    hash = HashSmokeMaterialUniverseFloat(hash, info.skyColor.x);
    hash = HashSmokeMaterialUniverseFloat(hash, info.skyColor.y);
    hash = HashSmokeMaterialUniverseFloat(hash, info.skyColor.z);
    hash = HashSmokeMaterialUniverseFloat(hash, info.skyColor.w);
    return hash;
}

RtSmokePersistentMaterialRecord BuildSmokePersistentMaterialRecord(uint32_t materialId, const RtSmokeMaterialTextureInfo& info, uint64 signature, uint32_t universeIndex)
{
    RtSmokePersistentMaterialRecord record;
    record.valid = true;
    record.signature = signature;
    record.universeIndex = universeIndex;

    const idVec3 color = SmokeMaterialIdToDebugColor(materialId);
    record.material.debugAlbedo[0] = color.x;
    record.material.debugAlbedo[1] = color.y;
    record.material.debugAlbedo[2] = color.z;
    record.material.debugAlbedo[3] = 1.0f;
    record.material.emissiveColor[0] = 0.0f;
    record.material.emissiveColor[1] = 0.0f;
    record.material.emissiveColor[2] = 0.0f;
    record.material.emissiveColor[3] = 1.0f;

    if (info.hasFallbackAlbedo)
    {
        record.material.debugAlbedo[0] = info.fallbackAlbedo.x;
        record.material.debugAlbedo[1] = info.fallbackAlbedo.y;
        record.material.debugAlbedo[2] = info.fallbackAlbedo.z;
        record.material.debugAlbedo[3] = info.fallbackAlbedo.w;
    }
    const bool hasDiffuseAlphaCoverage =
        info.alphaFromDiffuseLuma ||
        info.alphaFromDiffuseDarkKey ||
        info.alphaFromDiffuseMagentaKey;
    if (info.hasAlphaTest && (info.hasAlphaImage || hasDiffuseAlphaCoverage))
    {
        record.material.flags |= RT_SMOKE_MATERIAL_ALPHA_TEST;
        record.material.alphaCutoff = info.alphaCutoff;
    }
    if (info.diffuseColorFormat == CFM_YCOCG_DXT5)
    {
        record.material.flags |= RT_SMOKE_MATERIAL_DIFFUSE_YCOCG;
    }
    const bool useAdditiveDecalKey = info.additiveDecalWhiteKey || (info.additiveDecal && r_pathTracingAdditiveDecalKey.GetInteger() != 0);
    if (useAdditiveDecalKey)
    {
        record.material.flags |= RT_SMOKE_MATERIAL_ADDITIVE_DECAL;
        if (info.additiveDecalWhiteKey)
        {
            record.material.flags |= RT_SMOKE_MATERIAL_ADDITIVE_DECAL_WHITE_KEY;
        }
        record.additiveDecalContribution = 1;
    }
    if (info.filterDecal)
    {
        record.material.flags |= RT_SMOKE_MATERIAL_FILTER_DECAL;
    }
    if (info.filterDecalBlackKey)
    {
        record.material.flags |= RT_SMOKE_MATERIAL_FILTER_DECAL_BLACK_KEY;
    }
    if (info.detailDecal)
    {
        record.material.flags |= RT_SMOKE_MATERIAL_DETAIL_DECAL;
        if (info.detailDecalDynamic)
        {
            record.material.flags |= RT_SMOKE_MATERIAL_DETAIL_DECAL_DYNAMIC;
        }
        if (info.detailDecalDiffuseLit)
        {
            record.material.flags |= RT_SMOKE_MATERIAL_DETAIL_DECAL_DIFFUSE_LIT;
        }
        if (info.detailDecalLiquidPool)
        {
            record.material.flags |= RT_SMOKE_MATERIAL_DETAIL_DECAL_LIQUID_POOL;
        }
    }
    if (info.alphaFromDiffuseLuma)
    {
        record.material.flags |= RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_LUMA;
    }
    if (info.forceFallbackAlbedo)
    {
        record.material.flags |= RT_SMOKE_MATERIAL_FORCE_DEBUG_ALBEDO;
    }
    if (info.alphaFromDiffuseDarkKey)
    {
        record.material.flags |= RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_DARK_KEY;
    }
    if (info.alphaFromDiffuseMagentaKey)
    {
        record.material.flags |= RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_MAGENTA_KEY;
    }
    if (info.portalWindowFallback)
    {
        record.material.flags |= RT_SMOKE_MATERIAL_PORTAL_WINDOW_FALLBACK;
    }
    if (info.objectGlassFallback)
    {
        record.material.flags |= RT_SMOKE_MATERIAL_OBJECT_GLASS_FALLBACK;
    }
    if (RtSmokeMaterialEmissiveFactFromLocalFacts(
            info.emissive,
            info.hasEmissiveImage,
            info.hasSafeEmissiveTexture,
            false))
    {
        record.material.flags |= RT_SMOKE_MATERIAL_EMISSIVE;
        if (info.emissiveLightCandidate)
        {
            record.material.flags |= RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE;
        }
        record.material.emissiveColor[0] = info.emissiveColor.x;
        record.material.emissiveColor[1] = info.emissiveColor.y;
        record.material.emissiveColor[2] = info.emissiveColor.z;
        record.material.emissiveColor[3] = info.emissiveColor.w;
    }
    if (info.skyEnvironment)
    {
        // Secondary/transmission material consumers already understand the
        // ordinary emissive flag. Expose the constant sky radiance through
        // that contract so authored transparent windows can see it, while
        // deliberately leaving EMISSIVE_LIGHT_CANDIDATE clear: a sky shell is
        // an environment terminal, not a triangle light to enter ReSTIR.
        record.material.flags |= RT_SMOKE_MATERIAL_SKY_ENVIRONMENT | RT_SMOKE_MATERIAL_EMISSIVE;
        record.material.emissiveColor[0] = info.skyColor.x;
        record.material.emissiveColor[1] = info.skyColor.y;
        record.material.emissiveColor[2] = info.skyColor.z;
        record.material.emissiveColor[3] = info.skyColor.w;
    }

    record.facts.materialId = materialId;
    record.facts.universeIndex = universeIndex;
    record.facts.materialFlags = record.material.flags;
    record.facts.isDynamic = info.isDynamic;
    record.facts.hasFallbackAlbedo = info.hasFallbackAlbedo;
    record.facts.fallbackAlbedo = info.hasFallbackAlbedo ? info.fallbackAlbedo : idVec4(record.material.debugAlbedo[0], record.material.debugAlbedo[1], record.material.debugAlbedo[2], record.material.debugAlbedo[3]);
    record.facts.alphaTested = (record.material.flags & RT_SMOKE_MATERIAL_ALPHA_TEST) != 0;
    record.facts.alphaCutoff = record.material.alphaCutoff;
    record.facts.diffuseYCoCg = (record.material.flags & RT_SMOKE_MATERIAL_DIFFUSE_YCOCG) != 0;
    record.facts.additiveDecal = (record.material.flags & RT_SMOKE_MATERIAL_ADDITIVE_DECAL) != 0;
    record.facts.additiveDecalWhiteKey = (record.material.flags & RT_SMOKE_MATERIAL_ADDITIVE_DECAL_WHITE_KEY) != 0;
    record.facts.filterDecal = (record.material.flags & RT_SMOKE_MATERIAL_FILTER_DECAL) != 0;
    record.facts.filterDecalBlackKey = (record.material.flags & RT_SMOKE_MATERIAL_FILTER_DECAL_BLACK_KEY) != 0;
    record.facts.detailDecal = (record.material.flags & RT_SMOKE_MATERIAL_DETAIL_DECAL) != 0;
    record.facts.detailDecalDynamic = (record.material.flags & RT_SMOKE_MATERIAL_DETAIL_DECAL_DYNAMIC) != 0;
    record.facts.detailDecalLiquidPool = (record.material.flags & RT_SMOKE_MATERIAL_DETAIL_DECAL_LIQUID_POOL) != 0;
    record.facts.liquidFilmHasBloodSemantic = info.liquidFilmHasBloodSemantic;
    record.facts.liquidFilmHasWetReflectStage = info.liquidFilmHasWetReflectStage;
    record.facts.liquidFilmHasCoverageSource = info.liquidFilmHasCoverageSource;
    record.facts.liquidFilmHasWetNormalSource = info.liquidFilmHasWetNormalSource;
    record.facts.liquidFilmExactOverride = info.liquidFilmExactOverride;
    record.facts.liquidFilmCandidate = info.liquidFilmCandidate;
    record.facts.alphaFromDiffuseLuma = (record.material.flags & RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_LUMA) != 0;
    record.facts.forceFallbackAlbedo = (record.material.flags & RT_SMOKE_MATERIAL_FORCE_DEBUG_ALBEDO) != 0;
    record.facts.alphaFromDiffuseDarkKey = (record.material.flags & RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_DARK_KEY) != 0;
    record.facts.alphaFromDiffuseMagentaKey = (record.material.flags & RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_MAGENTA_KEY) != 0;
    record.facts.portalWindowFallback = (record.material.flags & RT_SMOKE_MATERIAL_PORTAL_WINDOW_FALLBACK) != 0;
    record.facts.objectGlassFallback = (record.material.flags & RT_SMOKE_MATERIAL_OBJECT_GLASS_FALLBACK) != 0;
    record.facts.skyEnvironment = (record.material.flags & RT_SMOKE_MATERIAL_SKY_ENVIRONMENT) != 0;
    record.facts.emissive = (record.material.flags & RT_SMOKE_MATERIAL_EMISSIVE) != 0;
    record.facts.emissiveLightCandidate = (record.material.flags & RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE) != 0;
    record.facts.emissiveColor = idVec4(record.material.emissiveColor[0], record.material.emissiveColor[1], record.material.emissiveColor[2], record.material.emissiveColor[3]);
    record.facts.emissiveLuminance = SmokeUniverseEmissiveLuminance(record.facts.emissiveColor);
    record.facts.hasDiffuseImage = info.hasDiffuseImage;
    record.facts.hasSafeDiffuseTexture = info.hasTextureHandle && info.hasSafeTexture;
    record.facts.hasAlphaImage = info.hasAlphaImage;
    record.facts.hasSafeAlphaTexture = info.hasAlphaTextureHandle && info.hasSafeAlphaTexture;
    record.facts.hasNormalImage = info.hasNormalImage;
    record.facts.hasSafeNormalTexture = info.hasNormalTextureHandle && info.hasSafeNormalTexture;
    record.facts.hasSpecularImage = info.hasSpecularImage;
    record.facts.hasSafeSpecularTexture = info.hasSpecularTextureHandle && info.hasSafeSpecularTexture;
    record.facts.hasEmissiveImage = info.hasEmissiveImage;
    record.facts.hasSafeEmissiveTexture = info.hasEmissiveTextureHandle && info.hasSafeEmissiveTexture;
    record.facts.guiTextureCandidate = SmokeUniverseHasGuiTextureCandidate(info);

    return record;
}


}

bool SmokeMaterialUniverseSemanticFingerprintSelfTest()
{
    const uint32_t materialId = 0xf17e51u;
    RtSmokeMaterialTextureInfo baseline;
    baseline.materialName = "_rt_material_fingerprint_self_test";
    const uint64 baselineSignature =
        ComputeSmokePersistentMaterialSignature(materialId, baseline);

    RtSmokeMaterialTextureInfo normalMutation = baseline;
    normalMutation.hasNormalImage = true;
    normalMutation.normalImageName = "textures/selftest/normal_local";
    const uint64 normalSignature =
        ComputeSmokePersistentMaterialSignature(materialId, normalMutation);
    const RtSmokePersistentMaterialRecord normalRecord =
        BuildSmokePersistentMaterialRecord(
            materialId,
            normalMutation,
            normalSignature,
            7u);

    RtSmokeMaterialTextureInfo specularMutation = baseline;
    specularMutation.hasSpecularImage = true;
    specularMutation.specularImageName = "textures/selftest/specular_s";
    const uint64 specularSignature =
        ComputeSmokePersistentMaterialSignature(materialId, specularMutation);
    const RtSmokePersistentMaterialRecord specularRecord =
        BuildSmokePersistentMaterialRecord(
            materialId,
            specularMutation,
            specularSignature,
            8u);

    RtSmokeMaterialTextureInfo nameOnlyMutation = baseline;
    nameOnlyMutation.normalImageName = "textures/selftest/name_only";
    return baselineSignature != normalSignature &&
        baselineSignature != specularSignature &&
        normalSignature != specularSignature &&
        baselineSignature != ComputeSmokePersistentMaterialSignature(
            materialId,
            nameOnlyMutation) &&
        normalRecord.facts.hasNormalImage &&
        !normalRecord.facts.hasSafeNormalTexture &&
        specularRecord.facts.hasSpecularImage &&
        !specularRecord.facts.hasSafeSpecularTexture;
}

void BeginSmokeMaterialUniverseFrame()
{
    g_smokeMaterialUniverseStats.frameHits = 0;
    g_smokeMaterialUniverseStats.frameMisses = 0;
    g_smokeMaterialUniverseStats.frameRebuilds = 0;
    g_smokeMaterialUniverseStats.frameSignatureChecks = 0;
}

void ReserveSmokeMaterialUniverse(size_t expectedMaterialCount)
{
    if (expectedMaterialCount > g_smokePersistentMaterialRecords.bucket_count())
    {
        g_smokePersistentMaterialRecords.reserve(expectedMaterialCount);
    }
}

void ClearSmokeMaterialUniverse()
{
    g_smokePersistentMaterialRecords.clear();
    g_smokeMaterialUniverseStats = RtSmokeMaterialUniverseStats();
    g_smokeNextMaterialUniverseIndex = 0;
}

const RtSmokePersistentMaterialRecord& GetSmokePersistentMaterialRecord(uint32_t materialId, const RtSmokeMaterialTextureInfo& info)
{
    ++g_smokeMaterialUniverseStats.signatureChecks;
    ++g_smokeMaterialUniverseStats.frameSignatureChecks;
    const uint64 signature = ComputeSmokePersistentMaterialSignature(materialId, info);
    const RtSmokeMaterialUniverseKey key = BuildSmokeMaterialUniverseKey(materialId, info);
    std::pair<std::unordered_map<RtSmokeMaterialUniverseKey, RtSmokePersistentMaterialRecord, RtSmokeMaterialUniverseKeyHash>::iterator, bool> insertResult =
        g_smokePersistentMaterialRecords.emplace(key, RtSmokePersistentMaterialRecord());
    RtSmokePersistentMaterialRecord& record = insertResult.first->second;
    if (!record.valid)
    {
        ++g_smokeMaterialUniverseStats.misses;
        ++g_smokeMaterialUniverseStats.frameMisses;
        const uint32_t universeIndex = g_smokeNextMaterialUniverseIndex++;
        record = BuildSmokePersistentMaterialRecord(materialId, info, signature, universeIndex);
    }
    else if (record.signature != signature)
    {
        ++g_smokeMaterialUniverseStats.rebuilds;
        ++g_smokeMaterialUniverseStats.frameRebuilds;
        record = BuildSmokePersistentMaterialRecord(materialId, info, signature, record.universeIndex);
    }
    else
    {
        ++g_smokeMaterialUniverseStats.hits;
        ++g_smokeMaterialUniverseStats.frameHits;
    }

    return record;
}

const RtSmokeMaterialUniverseFacts& GetSmokeMaterialUniverseFacts(uint32_t materialId, const RtSmokeMaterialTextureInfo& info)
{
    return GetSmokePersistentMaterialRecord(materialId, info).facts;
}

RtSmokeMaterialUniverseStats GetSmokeMaterialUniverseStats()
{
    RtSmokeMaterialUniverseStats stats = g_smokeMaterialUniverseStats;
    stats.records = static_cast<int>(g_smokePersistentMaterialRecords.size());
    stats.universeMaterials = static_cast<int>(g_smokeNextMaterialUniverseIndex);
    return stats;
}
