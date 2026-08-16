#include "precompiled.h"
#pragma hdrstop

// Runtime material table assembly and cache for captured RT smoke triangles.
//
// The table produced here is the CPU-side mirror of what the shader consumes.
// Keep the cache signature in sync with any inputs that can change material
// records, texture slots, or probe/fallback behavior.

#include "PathTraceCVars.h"
#include "PathTraceDynamicMaterialState.h"
#include "PathTraceMaterialClassifier.h"
#include "PathTraceMaterialFeatureParameters.h"
#include "PathTraceMaterialTextureDiscovery.h"
#include "PathTraceTextureRegistry.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <unordered_map>


namespace {

struct RtSmokeMaterialTableCache
{
    bool valid = false;
    uint64 signature = 0;
    bool materialIdSetSignatureValid = false;
    uint64 materialIdSetSignature = 0;
    uint64 staticMaterialIdSequenceSignature = 0;
    uint64 dynamicMaterialIdSequenceSignature = 0;
    RtSmokeMaterialTableBuild table;
    int hits = 0;
    int misses = 0;
};

RtSmokeMaterialTableCache g_smokeMaterialTableCache;
RtSmokeMaterialTableBuildStats g_smokeMaterialTableBuildStats;
std::unordered_set<uint32_t> g_smokeZeroRoughnessMaterialOverrides;
std::unordered_set<uint32_t> g_smokeFullMetalMaterialOverrides;
uint32_t g_smokeMaterialOverrideGeneration = 1u;
bool g_smokeCrosshairZeroRoughnessToggleRequested = false;
bool g_smokeCrosshairFullMetalToggleRequested = false;

uint64 HashSmokeMaterialCacheValue(uint64 hash, uint64 value)
{
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash;
}

uint64 ComputeSmokeMaterialIdSetSignature(const std::vector<uint32_t>& staticMaterialIds, const std::vector<uint32_t>& dynamicMaterialIds)
{
    std::unordered_map<uint32_t, bool> materialSeen;
    const int expectedUniqueMaterials = SmokeMaterialTextureRegistrySize() + 64;
    materialSeen.reserve(expectedUniqueMaterials);
    std::vector<uint32_t> uniqueMaterialIds;
    uniqueMaterialIds.reserve(Min(static_cast<int>(staticMaterialIds.size() + dynamicMaterialIds.size()), expectedUniqueMaterials));
    for (uint32_t materialId : staticMaterialIds)
    {
        if (materialSeen.emplace(materialId, true).second)
        {
            uniqueMaterialIds.push_back(materialId);
        }
    }
    for (uint32_t materialId : dynamicMaterialIds)
    {
        if (materialSeen.emplace(materialId, true).second)
        {
            uniqueMaterialIds.push_back(materialId);
        }
    }
    std::sort(uniqueMaterialIds.begin(), uniqueMaterialIds.end());

    uint64 hash = 1469598103934665603ull;
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(uniqueMaterialIds.size()));
    for (uint32_t materialId : uniqueMaterialIds)
    {
        hash = HashSmokeMaterialCacheValue(hash, materialId);
    }
    return hash;
}

uint64 ComputeSmokeMaterialTableSignatureFromIdSet(uint64 materialIdSetSignature, bool enableTextureProbe, int minimumTextureTableLimit, uint32_t latchedTextureProbeMaterialId, int latchedTextureProbeRequestedIndex)
{
    uint64 hash = materialIdSetSignature;
    hash = HashSmokeMaterialCacheValue(hash, RT_PATH_TRACE_MATERIAL_FEATURE_RECORD_ABI_VERSION);
    hash = HashSmokeMaterialCacheValue(hash, RT_PATH_TRACE_LIQUID_POOL_PARAMETER_ABI_VERSION);
    hash = HashSmokeMaterialCacheValue(hash, enableTextureProbe ? 1u : 0u);
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(GetSmokeTextureTableEffectiveLimitWithMinimum(minimumTextureTableLimit)));
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(Max(0, r_pathTracingTextureTableStart.GetInteger())));
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(r_pathTracingTextureSampleEnable.GetInteger() != 0 ? 1 : 0));
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(idMath::ClampInt(0, 2, r_pathTracingTextureSampleMethod.GetInteger())));
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(r_pathTracingTextureFilter.GetInteger() != 0 ? 1 : 0));
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(r_pathTracingTextureDecode.GetInteger() != 0 ? 1 : 0));
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(r_pathTracingTextureBindlessEnable.GetInteger() != 0 ? 1 : 0));
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(r_pathTracingTextureForceFallback.GetInteger() != 0 ? 1 : 0));
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(r_pathTracingAdditiveDecalKey.GetInteger() != 0 ? 1 : 0));
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(r_pathTracingAllowGuiTextures.GetInteger() != 0 ? 1 : 0));
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(r_pathTracingEmissiveFallbackWithoutTexture.GetInteger() != 0 ? 1 : 0));
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(Max(0, r_pathTracingEmissiveProposalSuppressMaterialId.GetInteger())));
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(Max(0, r_pathTracingEmissiveSurfaceSuppressMaterialId.GetInteger())));
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(r_pathTracingTextureProbeIndex.GetInteger() + 0x80000000u));
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(r_pathTracingTextureProbeReset.GetInteger() != 0 ? 1 : 0));
    hash = HashSmokeMaterialCacheValue(hash, latchedTextureProbeMaterialId);
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(latchedTextureProbeRequestedIndex + 0x80000000u));
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(SmokeMaterialTextureRegistrySize()));
    hash = HashSmokeMaterialCacheValue(hash, SmokeMaterialTextureRegistryGeneration());
    if (r_pathTracingResidency.GetInteger() != 0 && r_pathTracingResidencyMaterial.GetInteger() != 0)
    {
        hash = HashSmokeMaterialCacheValue(hash, SmokeResidentMaterialFactsGeneration());
    }
    hash = HashSmokeMaterialCacheValue(hash, SmokeMaterialOverrideGeneration());
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(r_pathTracingMatClassEnable.GetInteger() != 0 ? 1 : 0));
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(r_pathTracingMatClassUseRmao.GetInteger() != 0 ? 1 : 0));
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(r_pathTracingMatClassDriveLegacySpec.GetInteger() != 0 ? 1 : 0));
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(idMath::ClampInt(0, 2, r_pathTracingMatClassNormalDecodeMode.GetInteger())));
    hash = HashSmokeMaterialCacheValue(hash, GetPathTraceMaterialClassifierGeneration());
    return hash;
}


uint64 ComputeSmokeMaterialIdSequenceSignature(const std::vector<uint32_t>& materialIds)
{
    uint64 hash = 1469598103934665603ull;
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(materialIds.size()));
    for (uint32_t materialId : materialIds)
    {
        hash = HashSmokeMaterialCacheValue(hash, materialId);
    }
    return hash;
}

}

int GetSmokeTextureTableRequestedLimit()
{
    return idMath::ClampInt(0, RT_SMOKE_TEXTURE_DESCRIPTOR_CAPACITY, r_pathTracingTextureTableLimit.GetInteger());
}

int GetSmokeTextureTableEffectiveLimit()
{
    return Min(GetSmokeTextureTableRequestedLimit(), RT_SMOKE_TEXTURE_EXPERIMENTAL_ACTIVE_CAP);
}

int GetSmokeTextureTableEffectiveLimitWithMinimum(int minimumLimit)
{
    const int requestedLimit = GetSmokeTextureTableRequestedLimit();
    const int resolvedLimit = Max(requestedLimit, idMath::ClampInt(0, RT_SMOKE_TEXTURE_EXPERIMENTAL_ACTIVE_CAP, minimumLimit));
    return Min(resolvedLimit, RT_SMOKE_TEXTURE_EXPERIMENTAL_ACTIVE_CAP);
}

void ArmSmokeCrosshairZeroRoughnessToggle()
{
    g_smokeCrosshairZeroRoughnessToggleRequested = true;
}

bool ConsumeSmokeCrosshairZeroRoughnessToggleRequest()
{
    const bool requested = g_smokeCrosshairZeroRoughnessToggleRequested;
    g_smokeCrosshairZeroRoughnessToggleRequested = false;
    return requested;
}

bool ToggleSmokeMaterialZeroRoughnessOverride(uint32_t materialId, const char* materialName)
{
    if (materialId == 0u)
    {
        common->Printf("PathTracePrimaryPass: zero-roughness material toggle ignored invalid material id 0\n");
        return false;
    }

    const auto existing = g_smokeZeroRoughnessMaterialOverrides.find(materialId);
    const bool enabled = existing == g_smokeZeroRoughnessMaterialOverrides.end();
    if (enabled)
    {
        g_smokeZeroRoughnessMaterialOverrides.insert(materialId);
    }
    else
    {
        g_smokeZeroRoughnessMaterialOverrides.erase(existing);
    }

    ++g_smokeMaterialOverrideGeneration;
    common->Printf("PathTracePrimaryPass: zero-roughness material override %s material='%s' id=%u activeOverrides=%d generation=%u\n",
        enabled ? "enabled" : "disabled",
        materialName && materialName[0] ? materialName : "<unknown>",
        materialId,
        static_cast<int>(g_smokeZeroRoughnessMaterialOverrides.size()),
        g_smokeMaterialOverrideGeneration);
    return enabled;
}

bool SmokeMaterialHasZeroRoughnessOverride(uint32_t materialId)
{
    return g_smokeZeroRoughnessMaterialOverrides.find(materialId) != g_smokeZeroRoughnessMaterialOverrides.end();
}

int SmokeMaterialZeroRoughnessOverrideCount()
{
    return static_cast<int>(g_smokeZeroRoughnessMaterialOverrides.size());
}

void ArmSmokeCrosshairFullMetalToggle()
{
    g_smokeCrosshairFullMetalToggleRequested = true;
}

bool ConsumeSmokeCrosshairFullMetalToggleRequest()
{
    const bool requested = g_smokeCrosshairFullMetalToggleRequested;
    g_smokeCrosshairFullMetalToggleRequested = false;
    return requested;
}

bool ToggleSmokeMaterialFullMetalOverride(uint32_t materialId, const char* materialName)
{
    if (materialId == 0u)
    {
        common->Printf("PathTracePrimaryPass: full-metal material toggle ignored invalid material id 0\n");
        return false;
    }

    const auto existing = g_smokeFullMetalMaterialOverrides.find(materialId);
    const bool enabled = existing == g_smokeFullMetalMaterialOverrides.end();
    if (enabled)
    {
        g_smokeFullMetalMaterialOverrides.insert(materialId);
    }
    else
    {
        g_smokeFullMetalMaterialOverrides.erase(existing);
    }

    ++g_smokeMaterialOverrideGeneration;
    common->Printf("PathTracePrimaryPass: full-metal material override %s material='%s' id=%u activeOverrides=%d generation=%u\n",
        enabled ? "enabled" : "disabled",
        materialName && materialName[0] ? materialName : "<unknown>",
        materialId,
        static_cast<int>(g_smokeFullMetalMaterialOverrides.size()),
        g_smokeMaterialOverrideGeneration);
    return enabled;
}

bool SmokeMaterialHasFullMetalOverride(uint32_t materialId)
{
    return g_smokeFullMetalMaterialOverrides.find(materialId) != g_smokeFullMetalMaterialOverrides.end();
}

int SmokeMaterialFullMetalOverrideCount()
{
    return static_cast<int>(g_smokeFullMetalMaterialOverrides.size());
}

uint32_t SmokeMaterialOverrideGeneration()
{
    return g_smokeMaterialOverrideGeneration;
}

uint32_t HashSmokeMaterialName(const char* materialName)
{
    uint32_t hash = 2166136261u;
    const char* cursor = materialName ? materialName : "<none>";
    while (*cursor)
    {
        hash ^= static_cast<uint8_t>(*cursor);
        hash *= 16777619u;
        ++cursor;
    }

    return hash != 0u ? hash : 1u;
}

uint32_t SmokeMaterialId(const idMaterial* material)
{
    return HashSmokeMaterialName(material ? material->GetName() : "<none>");
}

bool SmokeMaterialHasRuntimeConditionalNoOpAlphaStage(const idMaterial* material)
{
    if (!material || material->ConstantRegisters() != nullptr)
    {
        return false;
    }

    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || !stage->hasAlphaTest || stage->ignoreAlphaTest)
        {
            continue;
        }

        const uint64 srcBlend = stage->drawStateBits & GLS_SRCBLEND_BITS;
        const uint64 dstBlend = stage->drawStateBits & GLS_DSTBLEND_BITS;
        if (srcBlend == GLS_SRCBLEND_ZERO && dstBlend == GLS_DSTBLEND_ONE)
        {
            return true;
        }
    }
    return false;
}


PathTraceSmokeMaterial BuildSmokeMaterialTableMaterial(uint32_t materialId, const RtSmokeMaterialTextureInfo& info, const RtSmokePersistentMaterialRecord& record)
{
    PathTraceSmokeMaterial material = record.material;
    const uint32_t surfaceSuppressMaterialId =
        static_cast<uint32_t>(Max(0,
            r_pathTracingEmissiveSurfaceSuppressMaterialId.GetInteger()));
    if (surfaceSuppressMaterialId != 0u &&
        materialId == surfaceSuppressMaterialId)
    {
        // Strong diagnostic isolation. Preserve the exact geometry and all
        // non-emissive material inputs, but prevent this surface from entering
        // either endpoint-emission or emissive proposal/replay control paths.
        material.flags &= ~(RT_SMOKE_MATERIAL_EMISSIVE |
            RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE);
        material.emissiveColor[0] = 0.0f;
        material.emissiveColor[1] = 0.0f;
        material.emissiveColor[2] = 0.0f;
    }
    else if (materialId == static_cast<uint32_t>(Max(0, r_pathTracingEmissiveProposalSuppressMaterialId.GetInteger())))
    {
        // Diagnostic isolation only. Keep visible emission and identical
        // geometry/visibility, but prevent this material from activating the
        // emissive light proposal and replay domains.
        material.flags &= ~RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE;
    }
    const idMaterial* materialDecl = nullptr;
    if (SmokeMaterialTextureInfoHasMaterialMetadata(info))
    {
        materialDecl = declManager ? declManager->FindMaterial(info.materialName.c_str(), false) : nullptr;
    }
    if (SmokeMaterialHasRuntimeConditionalNoOpAlphaStage(materialDecl))
    {
        // `if parm7 { blend GL_ZERO,GL_ONE; alphaTest ... }` is a dormant
        // dissolve controller. Fail opaque until the per-entity runtime record
        // explicitly enables it; never apply its mask to a living character.
        material.flags &= ~RT_SMOKE_MATERIAL_ALPHA_TEST;
    }
    if (SmokeMaterialHasZeroRoughnessOverride(materialId))
    {
        material.padding0 |= RT_SMOKE_MATERIAL_OVERRIDE_ZERO_ROUGHNESS;
    }
    if (SmokeMaterialHasFullMetalOverride(materialId))
    {
        material.padding0 |= RT_SMOKE_MATERIAL_OVERRIDE_FULL_METAL;
    }
    if (r_pathTracingMatClassEnable.GetInteger() != 0)
    {
        const RtMaterialRecord* materialClassRecord = nullptr;
        if (SmokeMaterialTextureInfoHasMaterialMetadata(info))
        {
            materialClassRecord = &RegisterPathTraceMaterialRecord(materialDecl, info);
        }
        else
        {
            materialClassRecord = FindPathTraceMaterialRecord(materialId);
        }
        if (materialClassRecord && materialClassRecord->valid)
        {
            material.padding0 |= PackPathTraceMaterialClassifierDynamicFlags(*materialClassRecord);
            material.padding1 = PackPathTraceMaterialClassifierFlags(*materialClassRecord);
            material.padding2 = PackPathTraceMaterialClassifierParams(*materialClassRecord);
            if (r_pathTracingMatClassDriveLegacySpec.GetInteger() != 0 &&
                materialClassRecord->route == RtMaterialBsdfRoute::LegacySpecGloss &&
                materialClassRecord->surfaceClass == RtMaterialSurfaceClass::Ricochet)
            {
                material.padding0 |= RT_SMOKE_MATERIAL_CLASSIFIER_DRIVE_LEGACY_SPEC;
            }
        }
    }
    return material;
}

RtPathTraceMaterialModifierKind BuildSmokeMaterialFeatureModifierKind(const RtSmokeMaterialUniverseFacts& facts)
{
    if (facts.liquidFilmCandidate)
    {
        return RT_PATH_TRACE_MATERIAL_MODIFIER_LIQUID_POOL_UNION;
    }
    if ((facts.materialFlags & RT_SMOKE_MATERIAL_DETAIL_DECAL_DIFFUSE_LIT) != 0)
    {
        return RT_PATH_TRACE_MATERIAL_MODIFIER_DIFFUSE_LIT;
    }
    if (facts.filterDecal)
    {
        return RT_PATH_TRACE_MATERIAL_MODIFIER_MODULATE_FILTER;
    }
    if (facts.additiveDecal)
    {
        return RT_PATH_TRACE_MATERIAL_MODIFIER_ADDITIVE_EMISSIVE;
    }
    if (facts.detailDecal)
    {
        return RT_PATH_TRACE_MATERIAL_MODIFIER_OVER;
    }
    return RT_PATH_TRACE_MATERIAL_MODIFIER_NONE;
}

static const int RT_PATH_TRACE_ORDERED_STAGE_CAPACITY = 8;

uint32_t PackSmokeMaterialOrderedStage(const RtMaterialCompositingStageFact& stage)
{
    const uint32_t stageIndex = static_cast<uint32_t>(idMath::ClampInt(0, 15, stage.stageIndex));
    const uint32_t lighting = static_cast<uint32_t>(stage.lighting) & 0x7u;
    const uint32_t operation = static_cast<uint32_t>(stage.operation) & 0x7u;
    const uint32_t srcBlend = static_cast<uint32_t>(stage.srcBlendBits) & 0x7u;
    const uint32_t dstBlend = (static_cast<uint32_t>(stage.dstBlendBits) >> 3u) & 0x7u;
    const uint32_t texgen = static_cast<uint32_t>(stage.texgen) & 0xfu;
    const uint32_t vertexColor = static_cast<uint32_t>(stage.vertexColor) & 0x3u;
    const bool hasProgram = stage.vertexProgram >= 0 || stage.fragmentProgram >= 0 || stage.glslProgram >= 0;

    return stageIndex |
        (lighting << 4u) |
        (operation << 7u) |
        (srcBlend << 10u) |
        (dstBlend << 13u) |
        (stage.hasAlphaTest ? (1u << 16u) : 0u) |
        (stage.ignoreAlphaTest ? (1u << 17u) : 0u) |
        (stage.conditionIsDynamic ? (1u << 18u) : 0u) |
        (stage.conditionCanBeActive ? (1u << 19u) : 0u) |
        (stage.hasTextureMatrix ? (1u << 20u) : 0u) |
        (stage.dynamicImage != DI_STATIC ? (1u << 21u) : 0u) |
        (hasProgram ? (1u << 22u) : 0u) |
        (1u << 23u) |
        (texgen << 24u) |
        (vertexColor << 28u);
}

void CompileSmokeMaterialOrderedStages(uint32_t materialId, RtPathTraceMaterialFeatureParameterRecord& parameters)
{
    const RtMaterialRecord* record = FindPathTraceMaterialRecord(materialId);
    if (!record)
    {
        return;
    }

    const int stageCount = Min(static_cast<int>(record->compositingStages.size()), RT_PATH_TRACE_ORDERED_STAGE_CAPACITY);
    for (int stageIndex = 0; stageIndex < stageCount; ++stageIndex)
    {
        parameters.orderedStageWords[stageIndex] = PackSmokeMaterialOrderedStage(record->compositingStages[stageIndex]);
    }
    if (static_cast<int>(record->compositingStages.size()) > RT_PATH_TRACE_ORDERED_STAGE_CAPACITY)
    {
        parameters.orderedStageWords[RT_PATH_TRACE_ORDERED_STAGE_CAPACITY - 1] |= 1u << 31u;
    }
}

RtPathTraceMaterialFeatureParameterRecord BuildSmokeMaterialFeatureParameters(
    uint32_t materialId,
    const RtSmokeMaterialUniverseFacts& facts)
{
    RtPathTraceMaterialFeatureParameterRecord parameters = BuildSmokeMaterialFeatureParameterRecord(facts);
    CompileSmokeMaterialOrderedStages(materialId, parameters);
    return parameters;
}

RtPathTraceMaterialFeatureRecord BuildSmokeMaterialFeatureRecord(const RtSmokeMaterialUniverseFacts& facts, uint32_t tableIndex)
{
    RtPathTraceMaterialFeatureRecord feature;
    feature.passSupport = RT_PATH_TRACE_MATERIAL_PASS_DEBUG_VISUALIZER;
    feature.modifierKind = BuildSmokeMaterialFeatureModifierKind(facts);
    feature.parameterRecordIndex = tableIndex;

    if (facts.skyEnvironment)
    {
        // A sky shell is terminal emitted radiance, not an opaque receiver
        // which happens to glow.  In particular it must not enter the DI/GI
        // reservoir domains or export diffuse RR guides.
        feature.materialKind = RT_PATH_TRACE_MATERIAL_KIND_SKY_OR_ENVIRONMENT;
        feature.materialCaps = RT_PATH_TRACE_MATERIAL_CAP_SHADOW_OCCLUSION;
        feature.lobeCaps = RT_PATH_TRACE_MATERIAL_LOBE_EMISSIVE;
        feature.modifierKind = RT_PATH_TRACE_MATERIAL_MODIFIER_NONE;
        feature.passSupport |=
            RT_PATH_TRACE_MATERIAL_PASS_PRIMARY_SURFACE |
            RT_PATH_TRACE_MATERIAL_PASS_PATH_INTEGRATOR;
        return feature;
    }

    if (facts.objectGlassFallback || facts.portalWindowFallback)
    {
        feature.materialKind = RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_GLASS;
        feature.materialCaps = RT_PATH_TRACE_MATERIAL_CAP_PATH_TRANSMISSION;
        feature.lobeCaps = RT_PATH_TRACE_MATERIAL_LOBE_SPECULAR_TRANSMISSION;
        feature.passSupport |=
            RT_PATH_TRACE_MATERIAL_PASS_PATH_INTEGRATOR |
            RT_PATH_TRACE_MATERIAL_PASS_TRANSMISSION_PRODUCER;
        return feature;
    }

    if (facts.liquidFilmCandidate)
    {
        feature.materialKind = RT_PATH_TRACE_MATERIAL_KIND_LIQUID_POOL_MODIFIER;
        feature.materialCaps =
            RT_PATH_TRACE_MATERIAL_CAP_RECEIVER_MODIFIER |
            RT_PATH_TRACE_MATERIAL_CAP_IDEMPOTENT_MODIFIER_BLEND;
        feature.lobeCaps = 0;
        feature.passSupport = RT_PATH_TRACE_MATERIAL_PASS_DEBUG_VISUALIZER;
        feature.modifierKind = RT_PATH_TRACE_MATERIAL_MODIFIER_LIQUID_POOL_UNION;
        return feature;
    }

    if (feature.modifierKind == RT_PATH_TRACE_MATERIAL_MODIFIER_LIQUID_POOL_UNION)
    {
        feature.materialKind = RT_PATH_TRACE_MATERIAL_KIND_LIQUID_POOL_MODIFIER;
    }
    else if (feature.modifierKind != RT_PATH_TRACE_MATERIAL_MODIFIER_NONE)
    {
        feature.materialKind = RT_PATH_TRACE_MATERIAL_KIND_DECAL_MODIFIER;
    }
    else if (facts.alphaTested)
    {
        feature.materialKind = RT_PATH_TRACE_MATERIAL_KIND_ALPHA_TESTED;
    }
    else
    {
        feature.materialKind = RT_PATH_TRACE_MATERIAL_KIND_OPAQUE;
    }

    feature.materialCaps =
        RT_PATH_TRACE_MATERIAL_CAP_OPAQUE_DIRECT |
        RT_PATH_TRACE_MATERIAL_CAP_OPAQUE_GI |
        RT_PATH_TRACE_MATERIAL_CAP_PATH_DIFFUSE |
        RT_PATH_TRACE_MATERIAL_CAP_SHADOW_OCCLUSION |
        RT_PATH_TRACE_MATERIAL_CAP_RR_DIFFUSE_GUIDE;
    feature.lobeCaps = RT_PATH_TRACE_MATERIAL_LOBE_DIFFUSE_REFLECTION;
    feature.passSupport |=
        RT_PATH_TRACE_MATERIAL_PASS_PRIMARY_SURFACE |
        RT_PATH_TRACE_MATERIAL_PASS_PATH_INTEGRATOR |
        RT_PATH_TRACE_MATERIAL_PASS_DIRECT_RESERVOIR |
        RT_PATH_TRACE_MATERIAL_PASS_GI_RESERVOIR |
        RT_PATH_TRACE_MATERIAL_PASS_RR_GUIDE_EXPORT;

    if (facts.alphaTested)
    {
        feature.materialCaps |= RT_PATH_TRACE_MATERIAL_CAP_VISIBILITY_RAY_ALPHA_TEST;
    }
    if (facts.emissive)
    {
        feature.lobeCaps |= RT_PATH_TRACE_MATERIAL_LOBE_EMISSIVE;
    }
    if (feature.modifierKind != RT_PATH_TRACE_MATERIAL_MODIFIER_NONE)
    {
        feature.materialCaps |= RT_PATH_TRACE_MATERIAL_CAP_RECEIVER_MODIFIER;
    }

    return feature;
}

bool SmokeMaterialFeatureRecordAbiValid(const RtPathTraceMaterialFeatureRecord& feature)
{
    return feature.recordAbiVersion == RT_PATH_TRACE_MATERIAL_FEATURE_RECORD_ABI_VERSION;
}

bool ValidateSmokeMaterialFeatureRecords(const RtSmokeMaterialTableBuild& table)
{
    if (table.materialFeatures.size() != table.materials.size() ||
        table.materialFeatureParameters.size() != table.materials.size() ||
        table.materialFacts.size() != table.materials.size() ||
        !ValidatePathTraceLiquidPoolMaterialFeatureParameterContract())
    {
        return false;
    }

    for (size_t materialIndex = 0; materialIndex < table.materialFeatures.size(); ++materialIndex)
    {
        const RtPathTraceMaterialFeatureRecord& feature = table.materialFeatures[materialIndex];
        if (!SmokeMaterialFeatureRecordAbiValid(feature))
        {
            return false;
        }
        if (feature.parameterRecordIndex != materialIndex)
        {
            return false;
        }

        const bool liquidCandidate = table.materialFacts[materialIndex].liquidFilmCandidate;
        if (liquidCandidate)
        {
            const uint32_t expectedCaps =
                RT_PATH_TRACE_MATERIAL_CAP_RECEIVER_MODIFIER |
                RT_PATH_TRACE_MATERIAL_CAP_IDEMPOTENT_MODIFIER_BLEND;
            if (feature.materialKind != RT_PATH_TRACE_MATERIAL_KIND_LIQUID_POOL_MODIFIER ||
                feature.materialCaps != expectedCaps ||
                feature.lobeCaps != 0u ||
                feature.passSupport != RT_PATH_TRACE_MATERIAL_PASS_DEBUG_VISUALIZER ||
                feature.modifierKind != RT_PATH_TRACE_MATERIAL_MODIFIER_LIQUID_POOL_UNION ||
                !PathTraceLiquidPoolMaterialFeatureParametersAreValid(table.materialFeatureParameters[materialIndex]))
            {
                return false;
            }
        }
        else if (feature.materialKind == RT_PATH_TRACE_MATERIAL_KIND_LIQUID_POOL_MODIFIER ||
            feature.modifierKind == RT_PATH_TRACE_MATERIAL_MODIFIER_LIQUID_POOL_UNION ||
            (feature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_IDEMPOTENT_MODIFIER_BLEND) != 0u)
        {
            return false;
        }
    }

    return true;
}

uint32_t AddSmokeMaterialTableEntry(RtSmokeMaterialTableBuild& table, uint32_t materialId)
{
    std::vector<uint32_t>::iterator existing = std::find(table.materialIds.begin(), table.materialIds.end(), materialId);
    if (existing != table.materialIds.end())
    {
        return static_cast<uint32_t>(existing - table.materialIds.begin());
    }

    const RtSmokeMaterialTextureInfo info = ResolveSmokeMaterialTextureInfo(materialId, static_cast<int>(table.materials.size()));
    const RtSmokePersistentMaterialRecord& record = GetSmokePersistentMaterialRecord(materialId, info);
    table.materialIds.push_back(materialId);
    table.materials.push_back(BuildSmokeMaterialTableMaterial(materialId, info, record));
    table.materialInfos.push_back(info);
    table.materialFacts.push_back(record.facts);
    table.materialFeatures.push_back(BuildSmokeMaterialFeatureRecord(record.facts, static_cast<uint32_t>(table.materialFeatures.size())));
    table.materialFeatureParameters.push_back(BuildSmokeMaterialFeatureParameters(materialId, record.facts));
    table.materialsAdditiveDecals += record.additiveDecalContribution;
    return static_cast<uint32_t>(table.materials.size() - 1);
}

bool RefreshSmokeMaterialTableFrameRecords(RtSmokeMaterialTableBuild& table)
{
    if (table.materialIds.size() != table.materials.size() ||
        table.materialInfos.size() != table.materials.size() ||
        table.materialFacts.size() != table.materials.size() ||
        table.materialFeatures.size() != table.materials.size() ||
        table.materialFeatureParameters.size() != table.materials.size())
    {
        return false;
    }

    table.materialsAdditiveDecals = 0;
    for (int tableIndex = 0; tableIndex < static_cast<int>(table.materialIds.size()); ++tableIndex)
    {
        const uint32_t materialId = table.materialIds[tableIndex];
        const RtSmokeMaterialTextureInfo info = ResolveSmokeMaterialTextureInfo(materialId, tableIndex);
        const RtSmokePersistentMaterialRecord& record = GetSmokePersistentMaterialRecord(materialId, info);
        table.materialInfos[tableIndex] = info;
        table.materials[tableIndex] = BuildSmokeMaterialTableMaterial(materialId, info, record);
        table.materialFacts[tableIndex] = record.facts;
        table.materialFeatures[tableIndex] = BuildSmokeMaterialFeatureRecord(record.facts, static_cast<uint32_t>(tableIndex));
        table.materialFeatureParameters[tableIndex] = BuildSmokeMaterialFeatureParameters(materialId, record.facts);
        table.materialsAdditiveDecals += record.additiveDecalContribution;
    }
    return true;
}

bool ValidateSmokeMaterialIndexes(const RtSmokeMaterialTableBuild& table)
{
    const uint32_t materialCount = static_cast<uint32_t>(table.materials.size());
    for (uint32_t materialIndex : table.staticMaterialIndexes)
    {
        if (materialIndex >= materialCount)
        {
            return false;
        }
    }

    for (uint32_t materialIndex : table.dynamicMaterialIndexes)
    {
        if (materialIndex >= materialCount)
        {
            return false;
        }
    }

    return table.materialIds.size() == table.materials.size() &&
        table.materialInfos.size() == table.materials.size() &&
        table.materialFacts.size() == table.materials.size() &&
        ValidateSmokeMaterialFeatureRecords(table) &&
        table.materialFeatureParameters.size() == table.materials.size();
}

bool SmokeMaterialTableIndexIsValid(const RtSmokeMaterialTableBuild& table, int tableIndex)
{
    return tableIndex >= 0 &&
        tableIndex < static_cast<int>(table.materialIds.size()) &&
        tableIndex < static_cast<int>(table.materials.size()) &&
        tableIndex < static_cast<int>(table.materialInfos.size()) &&
        tableIndex < static_cast<int>(table.materialFacts.size()) &&
        tableIndex < static_cast<int>(table.materialFeatures.size()) &&
        SmokeMaterialFeatureRecordAbiValid(table.materialFeatures[tableIndex]) &&
        tableIndex < static_cast<int>(table.materialFeatureParameters.size());
}

std::vector<int> BuildSmokeSafeMaterialIndexOrder(const RtSmokeMaterialTableBuild& table, const std::vector<RtSmokeMaterialTextureInfo>& materialInfos)
{
    const int materialTableCount = Min(static_cast<int>(table.materialIds.size()), static_cast<int>(table.materials.size()));

    std::vector<int> safeMaterialIndexes;
    safeMaterialIndexes.reserve(materialTableCount);

    for (int tableIndex = 0; tableIndex < materialTableCount; ++tableIndex)
    {
        const RtSmokeMaterialTextureInfo& info = materialInfos[tableIndex];
        const RtSmokeMaterialUniverseFacts& facts = table.materialFacts[tableIndex];
        if (info.diffuseImage && facts.hasSafeDiffuseTexture)
        {
            safeMaterialIndexes.push_back(tableIndex);
            continue;
        }

        if (info.alphaImage && facts.hasSafeAlphaTexture)
        {
            safeMaterialIndexes.push_back(tableIndex);
            continue;
        }

        if (info.normalImage && facts.hasSafeNormalTexture)
        {
            safeMaterialIndexes.push_back(tableIndex);
            continue;
        }

        if (info.specularImage && facts.hasSafeSpecularTexture)
        {
            safeMaterialIndexes.push_back(tableIndex);
            continue;
        }

        if (info.emissiveImage && facts.hasSafeEmissiveTexture)
        {
            safeMaterialIndexes.push_back(tableIndex);
        }
    }

    std::stable_sort(safeMaterialIndexes.begin(), safeMaterialIndexes.end(),
        [&table, &materialInfos](int lhs, int rhs)
        {
            if (!SmokeMaterialTableIndexIsValid(table, lhs) || !SmokeMaterialTableIndexIsValid(table, rhs))
            {
                return lhs < rhs;
            }

            const RtSmokeMaterialTextureInfo& leftInfo = materialInfos[lhs];
            const RtSmokeMaterialTextureInfo& rightInfo = materialInfos[rhs];
            const RtSmokeMaterialUniverseFacts& leftFacts = table.materialFacts[lhs];
            const RtSmokeMaterialUniverseFacts& rightFacts = table.materialFacts[rhs];
            const bool leftEmissiveTexture = leftFacts.emissive && leftInfo.emissiveImage && leftFacts.hasSafeEmissiveTexture;
            const bool rightEmissiveTexture = rightFacts.emissive && rightInfo.emissiveImage && rightFacts.hasSafeEmissiveTexture;
            if (leftEmissiveTexture != rightEmissiveTexture)
            {
                return leftEmissiveTexture;
            }
            if (leftEmissiveTexture && rightEmissiveTexture && leftFacts.emissiveLuminance != rightFacts.emissiveLuminance)
            {
                return leftFacts.emissiveLuminance > rightFacts.emissiveLuminance;
            }

            const idStr& leftName = SmokeBestSafeTextureName(leftInfo);
            const idStr& rightName = SmokeBestSafeTextureName(rightInfo);
            const int imageCompare = leftName.Icmp(rightName);
            if (imageCompare != 0)
            {
                return imageCompare < 0;
            }

            const int materialCompare = leftInfo.materialName.Icmp(rightInfo.materialName);
            if (materialCompare != 0)
            {
                return materialCompare < 0;
            }

            return lhs < rhs;
        });

    return safeMaterialIndexes;
}

std::vector<int> BuildSmokeSafeMaterialIndexOrder(const RtSmokeMaterialTableBuild& table)
{
    const int materialTableCount = Min(static_cast<int>(table.materialIds.size()), static_cast<int>(table.materials.size()));
    std::vector<RtSmokeMaterialTextureInfo> materialInfos;
    materialInfos.reserve(materialTableCount);
    for (int tableIndex = 0; tableIndex < materialTableCount; ++tableIndex)
    {
        materialInfos.push_back(ResolveSmokeMaterialTextureInfo(table.materialIds[tableIndex], tableIndex));
    }

    return BuildSmokeSafeMaterialIndexOrder(table, materialInfos);
}

uint32_t AddSmokeMaterialTextureSlot(RtSmokeMaterialTableBuild& table, nvrhi::TextureHandle texture, int textureTableLimit, int textureTableStart, int& skippedUniqueTextures, std::vector<nvrhi::TextureHandle>& skippedTextures)
{
    if (!texture || !IsSmokeTextureHandleSafeForDescriptor(texture))
    {
        ++table.materialsRejectedAtFinalCheck;
        return UINT32_MAX;
    }

    for (int textureIndex = 0; textureIndex < static_cast<int>(table.diffuseTextures.size()); ++textureIndex)
    {
        if (table.diffuseTextures[textureIndex].Get() == texture.Get())
        {
            return static_cast<uint32_t>(textureIndex);
        }
    }

    for (int skippedIndex = 0; skippedIndex < static_cast<int>(skippedTextures.size()); ++skippedIndex)
    {
        if (skippedTextures[skippedIndex].Get() == texture.Get())
        {
            return UINT32_MAX;
        }
    }

    if (skippedUniqueTextures < textureTableStart)
    {
        skippedTextures.push_back(texture);
        ++skippedUniqueTextures;
        ++table.materialsOverTextureSlotLimit;
        return UINT32_MAX;
    }

    if (static_cast<int>(table.diffuseTextures.size()) >= textureTableLimit)
    {
        ++table.materialsOverTextureSlotLimit;
        return UINT32_MAX;
    }

    const uint32_t descriptorIndex = static_cast<uint32_t>(table.diffuseTextures.size());
    table.diffuseTextures.push_back(texture);
    return descriptorIndex;
}

void PopulateSmokeOrderedStageTextureSlots(RtSmokeMaterialTableBuild& table, int textureTableLimit)
{
    for (RtPathTraceMaterialFeatureParameterRecord& parameters : table.materialFeatureParameters)
    {
        for (uint32_t& textureWord : parameters.orderedStageTextureWords)
        {
            textureWord = 0u;
        }
    }
    if (textureTableLimit <= 0 || !declManager)
    {
        return;
    }

    int skippedUniqueTextures = 0;
    std::vector<nvrhi::TextureHandle> skippedTextures;
    const int materialCount = Min(static_cast<int>(table.materialIds.size()), static_cast<int>(table.materialFeatureParameters.size()));
    for (int materialIndex = 0; materialIndex < materialCount; ++materialIndex)
    {
        const RtMaterialRecord* record = FindPathTraceMaterialRecord(table.materialIds[materialIndex]);
        if (!record || !record->valid || materialIndex >= static_cast<int>(table.materialInfos.size()))
        {
            continue;
        }
        const idMaterial* material = declManager->FindMaterial(table.materialInfos[materialIndex].materialName.c_str(), false);
        if (!material)
        {
            continue;
        }

        RtPathTraceMaterialFeatureParameterRecord& parameters = table.materialFeatureParameters[materialIndex];
        const int stageCount = Min(static_cast<int>(record->compositingStages.size()), RT_PATH_TRACE_ORDERED_STAGE_CAPACITY);
        for (int stageSlot = 0; stageSlot < stageCount; ++stageSlot)
        {
            const int declarationStageIndex = record->compositingStages[stageSlot].stageIndex;
            if (declarationStageIndex < 0 || declarationStageIndex >= material->GetNumStages())
            {
                continue;
            }
            const shaderStage_t* stage = material->GetStage(declarationStageIndex);
            idImage* image = stage ? stage->texture.image : nullptr;
            if (!image || !IsSmokeDiffuseImageSafeForRayTracing(image))
            {
                continue;
            }
            const nvrhi::TextureHandle texture = image->GetTextureHandle();
            if (!texture || !IsSmokeTextureHandleSafeForDescriptor(texture))
            {
                continue;
            }
            const uint32_t descriptorIndex = AddSmokeMaterialTextureSlot(
                table,
                texture,
                textureTableLimit,
                0,
                skippedUniqueTextures,
                skippedTextures);
            if (descriptorIndex != UINT32_MAX)
            {
                parameters.orderedStageTextureWords[stageSlot] = 0x80000000u | descriptorIndex;
            }
        }
    }
}

bool BindSmokeMaterialRuntimeEmissiveTexture(RtSmokeMaterialTableBuild& table, int tableIndex, idImage* image, int minimumTextureTableLimit)
{
    if (!SmokeMaterialTableIndexIsValid(table, tableIndex) || !image || !IsSmokeDiffuseImageSafeForRayTracing(image))
    {
        return false;
    }

    const nvrhi::TextureHandle texture = image->GetTextureHandle();
    if (!texture)
    {
        return false;
    }

    for (int textureIndex = 0; textureIndex < static_cast<int>(table.diffuseTextures.size()); ++textureIndex)
    {
        if (table.diffuseTextures[textureIndex].Get() == texture.Get())
        {
            table.materials[tableIndex].emissiveTextureIndex = static_cast<uint32_t>(textureIndex);
            const nvrhi::TextureDesc& desc = texture->getDesc();
            table.materials[tableIndex].emissiveTextureWidth = Max(1u, desc.width);
            table.materials[tableIndex].emissiveTextureHeight = Max(1u, desc.height);
            return true;
        }
    }

    const int textureTableLimit = GetSmokeTextureTableEffectiveLimitWithMinimum(minimumTextureTableLimit);
    if (textureTableLimit <= 0 || static_cast<int>(table.diffuseTextures.size()) >= textureTableLimit)
    {
        ++table.materialsOverTextureSlotLimit;
        return false;
    }

    table.diffuseTextures.push_back(texture);
    table.materials[tableIndex].emissiveTextureIndex = static_cast<uint32_t>(table.diffuseTextures.size() - 1);
    const nvrhi::TextureDesc& desc = texture->getDesc();
    table.materials[tableIndex].emissiveTextureWidth = Max(1u, desc.width);
    table.materials[tableIndex].emissiveTextureHeight = Max(1u, desc.height);
    ++table.materialsWithEmissiveTextures;
    g_smokeMaterialTableBuildStats.descriptorTextures = static_cast<int>(table.diffuseTextures.size());
    return true;
}

void AccumulateSmokeGuiTextureDiagnostic(RtSmokeMaterialTableBuild& table, const idStr& imageName, bool safe)
{
    if (imageName.IsEmpty() || !IsSmokeImageNameGuiLike(imageName.c_str()))
    {
        return;
    }

    ++table.guiTextureCandidates;
    if (safe)
    {
        ++table.guiTexturesAccepted;
    }
    else
    {
        ++table.guiTexturesRejected;
    }
}

void PopulateSmokeMaterialTextureSlots(RtSmokeMaterialTableBuild& table, uint32_t& latchedMaterialId, int& latchedRequestedIndex, bool enableTextureProbe, int minimumTextureTableLimit)
{
    const int populateStartMs = Sys_Milliseconds();
    const int materialTableCount = Min(static_cast<int>(table.materialIds.size()), static_cast<int>(table.materials.size()));

    const int resetStartMs = Sys_Milliseconds();
    table.diffuseTextures.clear();
    table.materialsWithTextures = 0;
    table.materialsWithNormalTextures = 0;
    table.materialsWithSpecularTextures = 0;
    table.materialsWithEmissiveTextures = 0;
    table.materialsEmissive = 0;
    table.materialsMissingTextures = 0;
    table.materialsRejectedTextures = 0;
    table.materialsRejectedAtFinalCheck = 0;
    table.descriptorsReplacedWithFallback = 0;
    table.materialsOverTextureSlotLimit = 0;
    table.materialsWithAlphaTextures = 0;
    table.materialsAlphaTested = 0;
    table.guiTextureCandidates = 0;
    table.guiTexturesAccepted = 0;
    table.guiTexturesRejected = 0;
    table.textureProbeRequestedIndex = r_pathTracingTextureProbeIndex.GetInteger();
    table.textureProbeBoundIndex = -1;
    table.textureProbeBoundMaterialId = 0;
    table.textureProbeUsedLatch = false;

    for (int tableIndex = 0; tableIndex < static_cast<int>(table.materials.size()); ++tableIndex)
    {
        table.materials[tableIndex].diffuseTextureIndex = UINT32_MAX;
        table.materials[tableIndex].alphaTextureIndex = UINT32_MAX;
        table.materials[tableIndex].normalTextureIndex = UINT32_MAX;
        table.materials[tableIndex].specularTextureIndex = UINT32_MAX;
        table.materials[tableIndex].emissiveTextureIndex = UINT32_MAX;
        table.materials[tableIndex].textureWidth = 1;
        table.materials[tableIndex].textureHeight = 1;
        table.materials[tableIndex].alphaTextureWidth = 1;
        table.materials[tableIndex].alphaTextureHeight = 1;
        table.materials[tableIndex].normalTextureWidth = 1;
        table.materials[tableIndex].normalTextureHeight = 1;
        table.materials[tableIndex].specularTextureWidth = 1;
        table.materials[tableIndex].specularTextureHeight = 1;
        table.materials[tableIndex].emissiveTextureWidth = 1;
        table.materials[tableIndex].emissiveTextureHeight = 1;
    }
    g_smokeMaterialTableBuildStats.resetMs += Sys_Milliseconds() - resetStartMs;

    if (!enableTextureProbe)
    {
        g_smokeMaterialTableBuildStats.tableMaterials = materialTableCount;
        g_smokeMaterialTableBuildStats.populateMs += Sys_Milliseconds() - populateStartMs;
        return;
    }

    const std::vector<RtSmokeMaterialTextureInfo>& materialInfos = table.materialInfos;
    const std::vector<RtSmokeMaterialUniverseFacts>& materialFacts = table.materialFacts;

    const int textureTableLimit = GetSmokeTextureTableEffectiveLimitWithMinimum(minimumTextureTableLimit);
    const int textureTableStart = Max(0, r_pathTracingTextureTableStart.GetInteger());

    if (r_pathTracingTextureProbeReset.GetInteger() != 0 || latchedRequestedIndex != table.textureProbeRequestedIndex)
    {
        latchedMaterialId = 0;
        latchedRequestedIndex = table.textureProbeRequestedIndex;
        r_pathTracingTextureProbeReset.SetInteger(0);
    }

    const int diagnosticStartMs = Sys_Milliseconds();
    for (int tableIndex = 0; tableIndex < materialTableCount; ++tableIndex)
    {
        const RtSmokeMaterialTextureInfo& info = materialInfos[tableIndex];
        const RtSmokeMaterialUniverseFacts& facts = materialFacts[tableIndex];
        if (facts.alphaTested)
        {
            ++table.materialsAlphaTested;
        }
        if (facts.emissive)
        {
            ++table.materialsEmissive;
        }

        if (facts.guiTextureCandidate)
        {
            AccumulateSmokeGuiTextureDiagnostic(table, info.diffuseImageName, facts.hasSafeDiffuseTexture);
            AccumulateSmokeGuiTextureDiagnostic(table, info.alphaImageName, facts.hasSafeAlphaTexture);
            AccumulateSmokeGuiTextureDiagnostic(table, info.normalImageName, facts.hasSafeNormalTexture);
            AccumulateSmokeGuiTextureDiagnostic(table, info.specularImageName, facts.hasSafeSpecularTexture);
            AccumulateSmokeGuiTextureDiagnostic(table, info.emissiveImageName, facts.hasSafeEmissiveTexture);
        }

        if (!facts.hasDiffuseImage || !info.hasTextureHandle)
        {
            ++table.materialsMissingTextures;
            continue;
        }
        if (!facts.hasSafeDiffuseTexture)
        {
            ++table.materialsRejectedTextures;
        }

    }
    g_smokeMaterialTableBuildStats.diagnosticMs += Sys_Milliseconds() - diagnosticStartMs;

    const int safeOrderStartMs = Sys_Milliseconds();
    const std::vector<int> safeMaterialIndexes = BuildSmokeSafeMaterialIndexOrder(table, materialInfos);
    g_smokeMaterialTableBuildStats.safeOrderMs += Sys_Milliseconds() - safeOrderStartMs;
    g_smokeMaterialTableBuildStats.safeMaterials = static_cast<int>(safeMaterialIndexes.size());

    if (textureTableLimit <= 0)
    {
        table.materialsOverTextureSlotLimit = static_cast<int>(safeMaterialIndexes.size());
        if (!safeMaterialIndexes.empty())
        {
            int selectedMaterialIndex = -1;
            if (table.textureProbeRequestedIndex >= 0 &&
                std::find(safeMaterialIndexes.begin(), safeMaterialIndexes.end(), table.textureProbeRequestedIndex) != safeMaterialIndexes.end())
            {
                selectedMaterialIndex = table.textureProbeRequestedIndex;
            }
            else
            {
                selectedMaterialIndex = safeMaterialIndexes.front();
            }

            table.textureProbeBoundIndex = selectedMaterialIndex;
            table.textureProbeBoundMaterialId = table.materialIds[selectedMaterialIndex];
        }
        g_smokeMaterialTableBuildStats.tableMaterials = materialTableCount;
        g_smokeMaterialTableBuildStats.populateMs += Sys_Milliseconds() - populateStartMs;
        return;
    }

    const int descriptorStartMs = Sys_Milliseconds();
    int skippedUniqueTextures = 0;
    std::vector<nvrhi::TextureHandle> skippedTextures;
    for (int safeIndex : safeMaterialIndexes)
    {
        if (!SmokeMaterialTableIndexIsValid(table, safeIndex))
        {
            ++table.materialsRejectedAtFinalCheck;
            continue;
        }

        const RtSmokeMaterialTextureInfo& info = materialInfos[safeIndex];
        const RtSmokeMaterialUniverseFacts& facts = materialFacts[safeIndex];
        const nvrhi::TextureHandle texture = info.hasSafeTexture ? info.diffuseTexture : nullptr;
        if (texture && IsSmokeTextureHandleSafeForDescriptor(texture))
        {
            const uint32_t descriptorIndex = AddSmokeMaterialTextureSlot(table, texture, textureTableLimit, textureTableStart, skippedUniqueTextures, skippedTextures);
            if (descriptorIndex != UINT32_MAX)
            {
                table.materials[safeIndex].diffuseTextureIndex = descriptorIndex;
                const nvrhi::TextureDesc& textureDesc = texture->getDesc();
                table.materials[safeIndex].textureWidth = Max(1u, textureDesc.width);
                table.materials[safeIndex].textureHeight = Max(1u, textureDesc.height);
                ++table.materialsWithTextures;
            }
        }

        const nvrhi::TextureHandle alphaTexture = info.hasSafeAlphaTexture ? info.alphaTexture : nullptr;
        // Liquid-film coverage is an authored material input even when the
        // declaration is translucent rather than alpha-tested.  In particular,
        // the splat family carries its footprint in a makealpha stage; skipping
        // that descriptor leaves SmokeAlphaCoverage reading the diffuse alpha
        // channel and deterministically reduces the pool candidate to zero.
        const bool needsAlphaCoverageTexture = info.hasAlphaTest || facts.liquidFilmCandidate;
        if (needsAlphaCoverageTexture && alphaTexture && IsSmokeTextureHandleSafeForDescriptor(alphaTexture))
        {
            const uint32_t alphaDescriptorIndex = AddSmokeMaterialTextureSlot(table, alphaTexture, textureTableLimit, textureTableStart, skippedUniqueTextures, skippedTextures);
            if (alphaDescriptorIndex != UINT32_MAX)
            {
                table.materials[safeIndex].alphaTextureIndex = alphaDescriptorIndex;
                const nvrhi::TextureDesc& alphaTextureDesc = alphaTexture->getDesc();
                table.materials[safeIndex].alphaTextureWidth = Max(1u, alphaTextureDesc.width);
                table.materials[safeIndex].alphaTextureHeight = Max(1u, alphaTextureDesc.height);
                ++table.materialsWithAlphaTextures;
            }
        }

        const nvrhi::TextureHandle normalTexture = info.hasSafeNormalTexture ? info.normalTexture : nullptr;
        if (normalTexture && IsSmokeTextureHandleSafeForDescriptor(normalTexture))
        {
            const uint32_t normalDescriptorIndex = AddSmokeMaterialTextureSlot(table, normalTexture, textureTableLimit, textureTableStart, skippedUniqueTextures, skippedTextures);
            if (normalDescriptorIndex != UINT32_MAX)
            {
                table.materials[safeIndex].normalTextureIndex = normalDescriptorIndex;
                const nvrhi::TextureDesc& normalTextureDesc = normalTexture->getDesc();
                table.materials[safeIndex].normalTextureWidth = Max(1u, normalTextureDesc.width);
                table.materials[safeIndex].normalTextureHeight = Max(1u, normalTextureDesc.height);
                ++table.materialsWithNormalTextures;
            }
        }

        const nvrhi::TextureHandle specularTexture = info.hasSafeSpecularTexture ? info.specularTexture : nullptr;
        if (specularTexture && IsSmokeTextureHandleSafeForDescriptor(specularTexture))
        {
            const uint32_t specularDescriptorIndex = AddSmokeMaterialTextureSlot(table, specularTexture, textureTableLimit, textureTableStart, skippedUniqueTextures, skippedTextures);
            if (specularDescriptorIndex != UINT32_MAX)
            {
                table.materials[safeIndex].specularTextureIndex = specularDescriptorIndex;
                const nvrhi::TextureDesc& specularTextureDesc = specularTexture->getDesc();
                table.materials[safeIndex].specularTextureWidth = Max(1u, specularTextureDesc.width);
                table.materials[safeIndex].specularTextureHeight = Max(1u, specularTextureDesc.height);
                ++table.materialsWithSpecularTextures;
            }
        }

        const nvrhi::TextureHandle emissiveTexture = info.hasSafeEmissiveTexture ? info.emissiveTexture : nullptr;
        if (emissiveTexture && IsSmokeTextureHandleSafeForDescriptor(emissiveTexture))
        {
            const uint32_t emissiveDescriptorIndex = AddSmokeMaterialTextureSlot(table, emissiveTexture, textureTableLimit, textureTableStart, skippedUniqueTextures, skippedTextures);
            if (emissiveDescriptorIndex != UINT32_MAX)
            {
                table.materials[safeIndex].emissiveTextureIndex = emissiveDescriptorIndex;
                const nvrhi::TextureDesc& emissiveTextureDesc = emissiveTexture->getDesc();
                table.materials[safeIndex].emissiveTextureWidth = Max(1u, emissiveTextureDesc.width);
                table.materials[safeIndex].emissiveTextureHeight = Max(1u, emissiveTextureDesc.height);
                ++table.materialsWithEmissiveTextures;
            }
        }
        if (info.emissiveImage && info.emissiveImage == info.diffuseImage && table.materials[safeIndex].emissiveTextureIndex == UINT32_MAX && table.materials[safeIndex].diffuseTextureIndex != UINT32_MAX)
        {
            table.materials[safeIndex].emissiveTextureIndex = table.materials[safeIndex].diffuseTextureIndex;
            table.materials[safeIndex].emissiveTextureWidth = table.materials[safeIndex].textureWidth;
            table.materials[safeIndex].emissiveTextureHeight = table.materials[safeIndex].textureHeight;
            ++table.materialsWithEmissiveTextures;
        }

        if (facts.hasEmissiveImage &&
            table.materials[safeIndex].emissiveTextureIndex == UINT32_MAX &&
            r_pathTracingEmissiveFallbackWithoutTexture.GetInteger() == 0)
        {
            table.materials[safeIndex].flags &= ~(RT_SMOKE_MATERIAL_EMISSIVE | RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE);
            table.materials[safeIndex].emissiveColor[0] = 0.0f;
            table.materials[safeIndex].emissiveColor[1] = 0.0f;
            table.materials[safeIndex].emissiveColor[2] = 0.0f;
            table.materials[safeIndex].emissiveColor[3] = 1.0f;
        }
    }
    PopulateSmokeOrderedStageTextureSlots(table, textureTableLimit);
    g_smokeMaterialTableBuildStats.descriptorMs += Sys_Milliseconds() - descriptorStartMs;
    g_smokeMaterialTableBuildStats.descriptorTextures = static_cast<int>(table.diffuseTextures.size());

    const int probeStartMs = Sys_Milliseconds();
    int selectedMaterialIndex = -1;
    if (latchedMaterialId != 0)
    {
        std::vector<uint32_t>::const_iterator latchedMaterial = std::find(table.materialIds.begin(), table.materialIds.end(), latchedMaterialId);
        if (latchedMaterial != table.materialIds.end())
        {
            const int latchedIndex = static_cast<int>(latchedMaterial - table.materialIds.begin());
            if (std::find(safeMaterialIndexes.begin(), safeMaterialIndexes.end(), latchedIndex) != safeMaterialIndexes.end())
            {
                selectedMaterialIndex = latchedIndex;
                table.textureProbeUsedLatch = true;
            }
        }
    }

    if (selectedMaterialIndex < 0 && table.textureProbeRequestedIndex >= 0)
    {
        if (std::find(safeMaterialIndexes.begin(), safeMaterialIndexes.end(), table.textureProbeRequestedIndex) != safeMaterialIndexes.end())
        {
            selectedMaterialIndex = table.textureProbeRequestedIndex;
        }
    }

    if (selectedMaterialIndex < 0 && !safeMaterialIndexes.empty())
    {
        selectedMaterialIndex = safeMaterialIndexes.front();
    }

    if (selectedMaterialIndex < 0)
    {
        return;
    }

    if (!SmokeMaterialTableIndexIsValid(table, selectedMaterialIndex))
    {
        return;
    }
    const RtSmokeMaterialTextureInfo& selectedInfo = materialInfos[selectedMaterialIndex];
    if (!selectedInfo.diffuseImage || !selectedInfo.hasSafeTexture)
    {
        return;
    }

    table.textureProbeBoundIndex = selectedMaterialIndex;
    table.textureProbeBoundMaterialId = table.materialIds[selectedMaterialIndex];
    if (latchedMaterialId != table.textureProbeBoundMaterialId)
    {
        latchedMaterialId = table.textureProbeBoundMaterialId;
    }
    g_smokeMaterialTableBuildStats.probeMs += Sys_Milliseconds() - probeStartMs;
    g_smokeMaterialTableBuildStats.tableMaterials = materialTableCount;
    g_smokeMaterialTableBuildStats.populateMs += Sys_Milliseconds() - populateStartMs;
}





void BuildSmokeMaterialTable(RtSmokeMaterialTableBuild& table, const std::vector<uint32_t>& staticMaterialIds, const std::vector<uint32_t>& dynamicMaterialIds, uint32_t& latchedTextureProbeMaterialId, int& latchedTextureProbeRequestedIndex, bool enableTextureProbe, int minimumTextureTableLimit)
{
    g_smokeMaterialTableBuildStats = RtSmokeMaterialTableBuildStats();
    ++g_smokeMaterialTableBuildStats.buildCalls;
    table = RtSmokeMaterialTableBuild();
    ReserveSmokeMaterialUniverse(staticMaterialIds.size() + dynamicMaterialIds.size() + 64);
    table.materialIds.reserve(staticMaterialIds.size() + dynamicMaterialIds.size());
    table.materials.reserve(staticMaterialIds.size() + dynamicMaterialIds.size());
    table.materialFeatures.reserve(staticMaterialIds.size() + dynamicMaterialIds.size());
    table.materialFeatureParameters.reserve(staticMaterialIds.size() + dynamicMaterialIds.size());
    table.staticMaterialIndexes.reserve(staticMaterialIds.size());
    table.dynamicMaterialIndexes.reserve(dynamicMaterialIds.size());

    std::unordered_map<uint32_t, uint32_t> materialIndexLookup;
    materialIndexLookup.reserve(staticMaterialIds.size() + dynamicMaterialIds.size());

    const int entryStartMs = Sys_Milliseconds();
    for (uint32_t materialId : staticMaterialIds)
    {
        const std::unordered_map<uint32_t, uint32_t>::const_iterator existing = materialIndexLookup.find(materialId);
        if (existing != materialIndexLookup.end())
        {
            table.staticMaterialIndexes.push_back(existing->second);
            continue;
        }

        const uint32_t tableIndex = AddSmokeMaterialTableEntry(table, materialId);
        materialIndexLookup.emplace(materialId, tableIndex);
        table.staticMaterialIndexes.push_back(tableIndex);
    }

    for (uint32_t materialId : dynamicMaterialIds)
    {
        const std::unordered_map<uint32_t, uint32_t>::const_iterator existing = materialIndexLookup.find(materialId);
        if (existing != materialIndexLookup.end())
        {
            table.dynamicMaterialIndexes.push_back(existing->second);
            continue;
        }

        const uint32_t tableIndex = AddSmokeMaterialTableEntry(table, materialId);
        materialIndexLookup.emplace(materialId, tableIndex);
        table.dynamicMaterialIndexes.push_back(tableIndex);
    }
    g_smokeMaterialTableBuildStats.entryMs += Sys_Milliseconds() - entryStartMs;

    PopulateSmokeMaterialTextureSlots(table, latchedTextureProbeMaterialId, latchedTextureProbeRequestedIndex, enableTextureProbe, minimumTextureTableLimit);
}

void BuildSmokeMaterialTableFromUniverse(RtSmokeMaterialTableBuild& table, const std::vector<uint32_t>& staticMaterialIds, const std::vector<uint32_t>& dynamicMaterialIds, uint32_t& latchedTextureProbeMaterialId, int& latchedTextureProbeRequestedIndex, bool enableTextureProbe, int minimumTextureTableLimit)
{
    g_smokeMaterialTableBuildStats = RtSmokeMaterialTableBuildStats();
    ++g_smokeMaterialTableBuildStats.buildCalls;
    table = RtSmokeMaterialTableBuild();
    ReserveSmokeMaterialUniverse(staticMaterialIds.size() + dynamicMaterialIds.size() + 64);
    table.materialIds.reserve(staticMaterialIds.size() + dynamicMaterialIds.size());
    table.materials.reserve(staticMaterialIds.size() + dynamicMaterialIds.size());
    table.staticMaterialIndexes.reserve(staticMaterialIds.size());
    table.dynamicMaterialIndexes.reserve(dynamicMaterialIds.size());

    std::unordered_map<uint32_t, uint32_t> materialIndexLookup;
    materialIndexLookup.reserve(staticMaterialIds.size() + dynamicMaterialIds.size());

    const int entryStartMs = Sys_Milliseconds();
    for (uint32_t materialId : staticMaterialIds)
    {
        const std::unordered_map<uint32_t, uint32_t>::const_iterator existing = materialIndexLookup.find(materialId);
        if (existing != materialIndexLookup.end())
        {
            table.staticMaterialIndexes.push_back(existing->second);
            continue;
        }

        const uint32_t tableIndex = AddSmokeMaterialTableEntry(table, materialId);
        materialIndexLookup.emplace(materialId, tableIndex);
        table.staticMaterialIndexes.push_back(tableIndex);
    }

    for (uint32_t materialId : dynamicMaterialIds)
    {
        const std::unordered_map<uint32_t, uint32_t>::const_iterator existing = materialIndexLookup.find(materialId);
        if (existing != materialIndexLookup.end())
        {
            table.dynamicMaterialIndexes.push_back(existing->second);
            continue;
        }

        const uint32_t tableIndex = AddSmokeMaterialTableEntry(table, materialId);
        materialIndexLookup.emplace(materialId, tableIndex);
        table.dynamicMaterialIndexes.push_back(tableIndex);
    }
    g_smokeMaterialTableBuildStats.entryMs += Sys_Milliseconds() - entryStartMs;

    PopulateSmokeMaterialTextureSlots(table, latchedTextureProbeMaterialId, latchedTextureProbeRequestedIndex, enableTextureProbe, minimumTextureTableLimit);
}

void RebuildSmokeMaterialIndexesFromCachedTable(RtSmokeMaterialTableBuild& table, const std::vector<uint32_t>& staticMaterialIds, const std::vector<uint32_t>& dynamicMaterialIds)
{
    table.staticMaterialIndexes.clear();
    table.dynamicMaterialIndexes.clear();
    table.staticMaterialIndexes.reserve(staticMaterialIds.size());
    table.dynamicMaterialIndexes.reserve(dynamicMaterialIds.size());

    std::unordered_map<uint32_t, uint32_t> materialIndexLookup;
    materialIndexLookup.reserve(table.materialIds.size());
    for (int tableIndex = 0; tableIndex < static_cast<int>(table.materialIds.size()); ++tableIndex)
    {
        materialIndexLookup.emplace(table.materialIds[tableIndex], static_cast<uint32_t>(tableIndex));
    }

    for (uint32_t materialId : staticMaterialIds)
    {
        const std::unordered_map<uint32_t, uint32_t>::const_iterator existing = materialIndexLookup.find(materialId);
        table.staticMaterialIndexes.push_back(existing != materialIndexLookup.end() ? existing->second : UINT32_MAX);
    }

    for (uint32_t materialId : dynamicMaterialIds)
    {
        const std::unordered_map<uint32_t, uint32_t>::const_iterator existing = materialIndexLookup.find(materialId);
        table.dynamicMaterialIndexes.push_back(existing != materialIndexLookup.end() ? existing->second : UINT32_MAX);
    }
}


bool RebuildSmokeMaterialTableCacheRemaps(
    RtSmokeMaterialTableBuild& table,
    const std::vector<uint32_t>& staticMaterialIds,
    const std::vector<uint32_t>& dynamicMaterialIds,
    uint64 staticMaterialIdSequenceSignature,
    uint64 dynamicMaterialIdSequenceSignature)
{
    const bool staticMaterialRemapChanged =
        table.staticMaterialIndexes.size() != staticMaterialIds.size() ||
        g_smokeMaterialTableCache.staticMaterialIdSequenceSignature != staticMaterialIdSequenceSignature;
    const bool dynamicMaterialRemapChanged =
        table.dynamicMaterialIndexes.size() != dynamicMaterialIds.size() ||
        g_smokeMaterialTableCache.dynamicMaterialIdSequenceSignature != dynamicMaterialIdSequenceSignature;
    if (!staticMaterialRemapChanged && !dynamicMaterialRemapChanged)
    {
        return table.materialIds.size() == table.materials.size() &&
            table.materialInfos.size() == table.materials.size() &&
            table.materialFacts.size() == table.materials.size() &&
            table.materialFeatures.size() == table.materials.size() &&
            table.materialFeatureParameters.size() == table.materials.size();
    }

    RebuildSmokeMaterialIndexesFromCachedTable(table, staticMaterialIds, dynamicMaterialIds);
    if (!ValidateSmokeMaterialIndexes(table))
    {
        return false;
    }

    g_smokeMaterialTableCache.table.staticMaterialIndexes = table.staticMaterialIndexes;
    g_smokeMaterialTableCache.table.dynamicMaterialIndexes = table.dynamicMaterialIndexes;
    g_smokeMaterialTableCache.staticMaterialIdSequenceSignature = staticMaterialIdSequenceSignature;
    g_smokeMaterialTableCache.dynamicMaterialIdSequenceSignature = dynamicMaterialIdSequenceSignature;
    return true;
}




bool BuildSmokeMaterialTableFromUniverseCached(RtSmokeMaterialTableBuild& table, const std::vector<uint32_t>& staticMaterialIds, const std::vector<uint32_t>& dynamicMaterialIds, uint32_t& latchedTextureProbeMaterialId, int& latchedTextureProbeRequestedIndex, bool enableTextureProbe, int minimumTextureTableLimit, uint64& signature, bool& cacheHit)
{
    const uint64 staticMaterialIdSequenceSignature = ComputeSmokeMaterialIdSequenceSignature(staticMaterialIds);
    const uint64 dynamicMaterialIdSequenceSignature = ComputeSmokeMaterialIdSequenceSignature(dynamicMaterialIds);
    const bool reuseMaterialIdSetSignature =
        g_smokeMaterialTableCache.valid &&
        g_smokeMaterialTableCache.materialIdSetSignatureValid &&
        g_smokeMaterialTableCache.staticMaterialIdSequenceSignature == staticMaterialIdSequenceSignature &&
        g_smokeMaterialTableCache.dynamicMaterialIdSequenceSignature == dynamicMaterialIdSequenceSignature;
    const uint64 materialIdSetSignature = reuseMaterialIdSetSignature
        ? g_smokeMaterialTableCache.materialIdSetSignature
        : ComputeSmokeMaterialIdSetSignature(staticMaterialIds, dynamicMaterialIds);
    signature = ComputeSmokeMaterialTableSignatureFromIdSet(materialIdSetSignature, enableTextureProbe, minimumTextureTableLimit, latchedTextureProbeMaterialId, latchedTextureProbeRequestedIndex);
    cacheHit = false;
    if (r_pathTracingMaterialCache.GetInteger() != 0 && g_smokeMaterialTableCache.valid && g_smokeMaterialTableCache.signature == signature)
    {
        table = g_smokeMaterialTableCache.table;
        if (r_pathTracingResidency.GetInteger() != 0 && r_pathTracingResidencyMaterial.GetInteger() != 0)
        {
            g_smokeMaterialTableBuildStats = RtSmokeMaterialTableBuildStats();
            ++g_smokeMaterialTableBuildStats.buildCalls;
            if (!RebuildSmokeMaterialTableCacheRemaps(
                table,
                staticMaterialIds,
                dynamicMaterialIds,
                staticMaterialIdSequenceSignature,
                dynamicMaterialIdSequenceSignature))
            {
                g_smokeMaterialTableCache.valid = false;
                ++g_smokeMaterialTableCache.misses;
                BuildSmokeMaterialTableFromUniverse(table, staticMaterialIds, dynamicMaterialIds, latchedTextureProbeMaterialId, latchedTextureProbeRequestedIndex, enableTextureProbe, minimumTextureTableLimit);
                return false;
            }

            g_smokeMaterialTableBuildStats.tableMaterials = static_cast<int>(table.materials.size());
            g_smokeMaterialTableBuildStats.safeMaterials = table.materialsWithTextures;
            g_smokeMaterialTableBuildStats.descriptorTextures = static_cast<int>(table.diffuseTextures.size());
            cacheHit = true;
            ++g_smokeMaterialTableCache.hits;
            return true;
        }
        if (!RefreshSmokeMaterialTableFrameRecords(table))
        {
            g_smokeMaterialTableCache.valid = false;
            ++g_smokeMaterialTableCache.misses;
            BuildSmokeMaterialTableFromUniverse(table, staticMaterialIds, dynamicMaterialIds, latchedTextureProbeMaterialId, latchedTextureProbeRequestedIndex, enableTextureProbe, minimumTextureTableLimit);
            return false;
        }
        if (!RebuildSmokeMaterialTableCacheRemaps(
            table,
            staticMaterialIds,
            dynamicMaterialIds,
            staticMaterialIdSequenceSignature,
            dynamicMaterialIdSequenceSignature))
        {
            g_smokeMaterialTableCache.valid = false;
            ++g_smokeMaterialTableCache.misses;
            BuildSmokeMaterialTableFromUniverse(table, staticMaterialIds, dynamicMaterialIds, latchedTextureProbeMaterialId, latchedTextureProbeRequestedIndex, enableTextureProbe, minimumTextureTableLimit);
            return false;
        }
        g_smokeMaterialTableBuildStats = RtSmokeMaterialTableBuildStats();
        ++g_smokeMaterialTableBuildStats.buildCalls;
        PopulateSmokeMaterialTextureSlots(table, latchedTextureProbeMaterialId, latchedTextureProbeRequestedIndex, enableTextureProbe, minimumTextureTableLimit);
        if (ValidateSmokeMaterialIndexes(table))
        {
            g_smokeMaterialTableCache.table = table;
            g_smokeMaterialTableCache.materialIdSetSignatureValid = true;
            g_smokeMaterialTableCache.materialIdSetSignature = materialIdSetSignature;
            g_smokeMaterialTableCache.staticMaterialIdSequenceSignature = staticMaterialIdSequenceSignature;
            g_smokeMaterialTableCache.dynamicMaterialIdSequenceSignature = dynamicMaterialIdSequenceSignature;
        }
        cacheHit = true;
        ++g_smokeMaterialTableCache.hits;
        return true;
    }

    ++g_smokeMaterialTableCache.misses;

    BuildSmokeMaterialTableFromUniverse(table, staticMaterialIds, dynamicMaterialIds, latchedTextureProbeMaterialId, latchedTextureProbeRequestedIndex, enableTextureProbe, minimumTextureTableLimit);
    if (r_pathTracingMaterialCache.GetInteger() != 0 && ValidateSmokeMaterialIndexes(table))
    {
        g_smokeMaterialTableCache.valid = true;
        g_smokeMaterialTableCache.signature = signature;
        g_smokeMaterialTableCache.materialIdSetSignatureValid = true;
        g_smokeMaterialTableCache.materialIdSetSignature = materialIdSetSignature;
        g_smokeMaterialTableCache.staticMaterialIdSequenceSignature = staticMaterialIdSequenceSignature;
        g_smokeMaterialTableCache.dynamicMaterialIdSequenceSignature = dynamicMaterialIdSequenceSignature;
        g_smokeMaterialTableCache.table = table;
    }
    return false;
}

bool BuildSmokeMaterialTableCached(RtSmokeMaterialTableBuild& table, const std::vector<uint32_t>& staticMaterialIds, const std::vector<uint32_t>& dynamicMaterialIds, uint32_t& latchedTextureProbeMaterialId, int& latchedTextureProbeRequestedIndex, bool enableTextureProbe, int minimumTextureTableLimit, uint64& signature, bool& cacheHit)
{
    // Disabled while validating a frame-local material-index mapping that remains
    // compatible with cached static BLAS metadata.
    signature = 0;
    cacheHit = false;
    BuildSmokeMaterialTable(table, staticMaterialIds, dynamicMaterialIds, latchedTextureProbeMaterialId, latchedTextureProbeRequestedIndex, enableTextureProbe, minimumTextureTableLimit);
    return false;
}

RtSmokeMaterialTableCacheStats GetSmokeMaterialTableCacheStats()
{
    RtSmokeMaterialTableCacheStats stats;
    stats.hits = g_smokeMaterialTableCache.hits;
    stats.misses = g_smokeMaterialTableCache.misses;
    return stats;
}

void ClearSmokeMaterialTableCache()
{
    const int hits = g_smokeMaterialTableCache.hits;
    const int misses = g_smokeMaterialTableCache.misses;
    g_smokeMaterialTableCache = RtSmokeMaterialTableCache();
    g_smokeMaterialTableCache.hits = hits;
    g_smokeMaterialTableCache.misses = misses;
}

RtSmokeMaterialTableBuildStats GetSmokeMaterialTableBuildStats()
{
    return g_smokeMaterialTableBuildStats;
}
