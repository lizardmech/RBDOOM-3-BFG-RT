#include "precompiled.h"
#pragma hdrstop

#include "PathTraceMaterialIdKernel.h"

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

static_assert(sizeof(PathTraceSmokeMaterial) == 112,
    "PathTraceSmokeMaterial GPU ABI must remain 112 bytes");

struct RtSmokeMaterialTableSignatureBreakdown
{
    bool valid = false;
    uint64 materialIdSetSignature = 0;
    uint64 configSignature = 0;
    int registrySize = 0;
    uint64 registryGeneration = 0;
    uint64 residentFactsGeneration = 0;
    uint32_t overrideGeneration = 0;
    uint64 classifierGeneration = 0;
};

struct RtSmokeMaterialTableCache
{
    bool valid = false;
    uint64 signature = 0;
    uint64 structuralSignature = 0;
    uint64 observedRegistryGeneration = 0;
    uint64 observedResidentFactsGeneration = 0;
    uint32_t observedOverrideGeneration = 0;
    uint64 observedOrderedStageBindingEpoch = 0;
    bool awaitingNextQuietEpochCacheHit = false;
    bool materialIdSetSignatureValid = false;
    uint64 materialIdSetSignature = 0;
    uint64 staticMaterialIdSequenceSignature = 0;
    uint64 dynamicMaterialIdSequenceSignature = 0;
    RtSmokeMaterialTableSignatureBreakdown signatureBreakdown;
    RtSmokeMaterialTableBuild table;
    std::unordered_map<uint32_t, uint32_t> materialIndexLookup;
    std::vector<uint64> rowSourceSignatures;
    std::vector<uint8_t> hasOrderedStageSources;
    int hits = 0;
    int misses = 0;
};

RtSmokeMaterialTableCache g_smokeMaterialTableCache;
RtSmokeMaterialTableBuildStats g_smokeMaterialTableBuildStats;
RtSmokeMaterialTableCacheStats g_smokeMaterialTableCacheTelemetry;
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

bool SmokeMaterialTableCacheTelemetryEnabled()
{
    return r_pathTracingResidency.GetInteger() != 0 &&
        r_pathTracingResidencyMaterial.GetInteger() != 0 &&
        r_pathTracingResidencyDump.GetInteger() != 0;
}

RtSmokeMaterialTableSignatureBreakdown CaptureSmokeMaterialTableSignatureBreakdown(
    uint64 materialIdSetSignature,
    bool enableTextureProbe,
    int minimumTextureTableLimit,
    uint32_t latchedTextureProbeMaterialId,
    int latchedTextureProbeRequestedIndex)
{
    RtSmokeMaterialTableSignatureBreakdown result;
    result.valid = true;
    result.materialIdSetSignature = materialIdSetSignature;
    result.registrySize = SmokeMaterialTextureRegistrySize();
    result.registryGeneration = SmokeMaterialTextureRegistryGeneration();
    result.residentFactsGeneration = SmokeResidentMaterialFactsGeneration();
    result.overrideGeneration = SmokeMaterialOverrideGeneration();
    result.classifierGeneration = GetPathTraceMaterialClassifierGeneration();

    uint64 configHash = 1469598103934665603ull;
    configHash = HashSmokeMaterialCacheValue(configHash, RT_PATH_TRACE_MATERIAL_FEATURE_RECORD_ABI_VERSION);
    configHash = HashSmokeMaterialCacheValue(configHash, RT_PATH_TRACE_LIQUID_POOL_PARAMETER_ABI_VERSION);
    configHash = HashSmokeMaterialCacheValue(configHash, enableTextureProbe ? 1u : 0u);
    configHash = HashSmokeMaterialCacheValue(configHash, static_cast<uint64>(GetSmokeTextureTableEffectiveLimitWithMinimum(minimumTextureTableLimit)));
    configHash = HashSmokeMaterialCacheValue(configHash, static_cast<uint64>(Max(0, r_pathTracingTextureTableStart.GetInteger())));
    configHash = HashSmokeMaterialCacheValue(configHash, static_cast<uint64>(r_pathTracingTextureSampleEnable.GetInteger() != 0 ? 1 : 0));
    configHash = HashSmokeMaterialCacheValue(configHash, static_cast<uint64>(idMath::ClampInt(0, 2, r_pathTracingTextureSampleMethod.GetInteger())));
    configHash = HashSmokeMaterialCacheValue(configHash, static_cast<uint64>(r_pathTracingTextureFilter.GetInteger() != 0 ? 1 : 0));
    configHash = HashSmokeMaterialCacheValue(configHash, static_cast<uint64>(r_pathTracingTextureDecode.GetInteger() != 0 ? 1 : 0));
    configHash = HashSmokeMaterialCacheValue(configHash, static_cast<uint64>(r_pathTracingTextureBindlessEnable.GetInteger() != 0 ? 1 : 0));
    configHash = HashSmokeMaterialCacheValue(configHash, static_cast<uint64>(r_pathTracingTextureForceFallback.GetInteger() != 0 ? 1 : 0));
    configHash = HashSmokeMaterialCacheValue(configHash, static_cast<uint64>(r_pathTracingAdditiveDecalKey.GetInteger() != 0 ? 1 : 0));
    configHash = HashSmokeMaterialCacheValue(configHash, static_cast<uint64>(r_pathTracingAllowGuiTextures.GetInteger() != 0 ? 1 : 0));
    configHash = HashSmokeMaterialCacheValue(configHash, static_cast<uint64>(r_pathTracingEmissiveFallbackWithoutTexture.GetInteger() != 0 ? 1 : 0));
    configHash = HashSmokeMaterialCacheValue(configHash, static_cast<uint64>(Max(0, r_pathTracingEmissiveProposalSuppressMaterialId.GetInteger())));
    configHash = HashSmokeMaterialCacheValue(configHash, static_cast<uint64>(Max(0, r_pathTracingEmissiveSurfaceSuppressMaterialId.GetInteger())));
    configHash = HashSmokeMaterialCacheValue(configHash, static_cast<uint64>(r_pathTracingTextureProbeIndex.GetInteger() + 0x80000000u));
    configHash = HashSmokeMaterialCacheValue(configHash, static_cast<uint64>(r_pathTracingTextureProbeReset.GetInteger() != 0 ? 1 : 0));
    configHash = HashSmokeMaterialCacheValue(configHash, latchedTextureProbeMaterialId);
    configHash = HashSmokeMaterialCacheValue(configHash, static_cast<uint64>(latchedTextureProbeRequestedIndex + 0x80000000u));
    configHash = HashSmokeMaterialCacheValue(configHash, static_cast<uint64>(r_pathTracingMatClassEnable.GetInteger() != 0 ? 1 : 0));
    configHash = HashSmokeMaterialCacheValue(configHash, static_cast<uint64>(r_pathTracingMatClassUseRmao.GetInteger() != 0 ? 1 : 0));
    configHash = HashSmokeMaterialCacheValue(configHash, static_cast<uint64>(r_pathTracingMatClassDriveLegacySpec.GetInteger() != 0 ? 1 : 0));
    configHash = HashSmokeMaterialCacheValue(configHash, static_cast<uint64>(idMath::ClampInt(0, 2, r_pathTracingMatClassNormalDecodeMode.GetInteger())));
    result.configSignature = configHash;
    return result;
}

void RecordSmokeMaterialTableSignatureChanges(
    const RtSmokeMaterialTableSignatureBreakdown& previous,
    const RtSmokeMaterialTableSignatureBreakdown& current)
{
    if (!previous.valid || !current.valid)
    {
        ++g_smokeMaterialTableCacheTelemetry.signatureChangedUnknown;
        return;
    }
    g_smokeMaterialTableCacheTelemetry.signatureChangedMaterialSet +=
        previous.materialIdSetSignature != current.materialIdSetSignature ? 1 : 0;
    g_smokeMaterialTableCacheTelemetry.signatureChangedConfig +=
        previous.configSignature != current.configSignature ? 1 : 0;
    g_smokeMaterialTableCacheTelemetry.signatureChangedRegistrySize +=
        previous.registrySize != current.registrySize ? 1 : 0;
    g_smokeMaterialTableCacheTelemetry.signatureChangedRegistryGeneration +=
        previous.registryGeneration != current.registryGeneration ? 1 : 0;
    g_smokeMaterialTableCacheTelemetry.signatureChangedResidentFacts +=
        previous.residentFactsGeneration != current.residentFactsGeneration ? 1 : 0;
    g_smokeMaterialTableCacheTelemetry.signatureChangedOverrides +=
        previous.overrideGeneration != current.overrideGeneration ? 1 : 0;
    g_smokeMaterialTableCacheTelemetry.signatureChangedClassifier +=
        previous.classifierGeneration != current.classifierGeneration ? 1 : 0;
}

void RecordSmokeMaterialTableCacheCall(bool hit, uint64 startMicroseconds)
{
    ++g_smokeMaterialTableCacheTelemetry.telemetryCalls;
    const uint64 elapsedMicroseconds = Sys_Microseconds() - startMicroseconds;
    if (hit)
    {
        ++g_smokeMaterialTableCacheTelemetry.telemetryHits;
        g_smokeMaterialTableCacheTelemetry.hitTotalMicroseconds += elapsedMicroseconds;
        g_smokeMaterialTableCacheTelemetry.hitMaxMicroseconds = std::max(
            g_smokeMaterialTableCacheTelemetry.hitMaxMicroseconds,
            elapsedMicroseconds);
        return;
    }
    ++g_smokeMaterialTableCacheTelemetry.telemetryMisses;
    g_smokeMaterialTableCacheTelemetry.missTotalMicroseconds += elapsedMicroseconds;
    g_smokeMaterialTableCacheTelemetry.missMaxMicroseconds = std::max(
        g_smokeMaterialTableCacheTelemetry.missMaxMicroseconds,
        elapsedMicroseconds);
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

struct RtSmokeMaterialStableSlotPlan
{
    std::vector<uint32_t> slotMaterialIds;
    std::vector<uint32_t> staticMaterialIndexes;
    std::vector<uint32_t> dynamicMaterialIndexes;
    int appendedIds = 0;
};

uint64 ComputeSmokeMaterialTableStructuralSignature(
    const RtSmokeMaterialTableSignatureBreakdown& breakdown);
uint64 ComputeSmokeMaterialRowSourceSignature(
    uint32_t materialId,
    const RtSmokeMaterialTextureInfo& info,
    const RtSmokePersistentMaterialRecord& record,
    const RtMaterialRecord* classifierRecord);
bool BuildSmokeMaterialStableSlotPlan(
    const std::vector<uint32_t>& existingSlotMaterialIds,
    const std::vector<uint32_t>& staticMaterialIds,
    const std::vector<uint32_t>& dynamicMaterialIds,
    RtSmokeMaterialStableSlotPlan& plan);
bool SmokeMaterialStableSlotPlanSelfTest();

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
    return HashPathTraceMaterialName(materialName);
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
    const uint32_t overrideBase = SmokeMaterialTextureVariantBase(materialId);
    if (SmokeMaterialHasZeroRoughnessOverride(materialId) ||
        (overrideBase && SmokeMaterialHasZeroRoughnessOverride(overrideBase)))
    {
        material.padding0 |= RT_SMOKE_MATERIAL_OVERRIDE_ZERO_ROUGHNESS;
    }
    if (SmokeMaterialHasFullMetalOverride(materialId) ||
        (overrideBase && SmokeMaterialHasFullMetalOverride(overrideBase)))
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
    OPTICK_EVENT("PT Material Table Universe Entries");

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

bool SmokeMaterialRecordHasOrderedStageSources(const RtMaterialRecord* record)
{
    return record && record->valid && !record->compositingStages.empty();
}

bool BuildSmokeMaterialActiveRowMask(
    const RtSmokeMaterialTableBuild& table,
    std::vector<bool>& activeRows)
{
    activeRows.assign(table.materialIds.size(), false);
    const auto addIndexes = [&activeRows](
        const std::vector<uint32_t>& indexes) -> bool {
        for (uint32_t tableIndex : indexes)
        {
            if (tableIndex >= activeRows.size())
            {
                return false;
            }
            activeRows[tableIndex] = true;
        }
        return true;
    };
    return addIndexes(table.staticMaterialIndexes) &&
        addIndexes(table.dynamicMaterialIndexes);
}

std::vector<int> BuildSmokeSafeMaterialIndexOrder(
    const RtSmokeMaterialTableBuild& table,
    const std::vector<RtSmokeMaterialTextureInfo>& materialInfos,
    const std::vector<bool>& activeRows)
{
    const int materialTableCount = Min(static_cast<int>(table.materialIds.size()), static_cast<int>(table.materials.size()));

    std::vector<int> safeMaterialIndexes;
    safeMaterialIndexes.reserve(materialTableCount);

    for (int tableIndex = 0; tableIndex < materialTableCount; ++tableIndex)
    {
        if (tableIndex >= static_cast<int>(activeRows.size()) ||
            !activeRows[tableIndex])
        {
            continue;
        }
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

    std::vector<bool> activeRows;
    if (!BuildSmokeMaterialActiveRowMask(table, activeRows))
    {
        return {};
    }
    return BuildSmokeSafeMaterialIndexOrder(table, materialInfos, activeRows);
}

void AccumulateSmokeGuiTextureDiagnostic(
    RtSmokeMaterialTableBuild& table,
    const idStr& imageName,
    bool safe);

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

enum RtSmokeMaterialDynamicWakeBits : uint32_t
{
    RT_SMOKE_DYNAMIC_WAKE_DYNAMIC = 1u << 0,
    RT_SMOKE_DYNAMIC_WAKE_APPEND = 1u << 1,
    RT_SMOKE_DYNAMIC_WAKE_NEW = 1u << 2,
    RT_SMOKE_DYNAMIC_WAKE_SIGNATURE = 1u << 3,
    RT_SMOKE_DYNAMIC_WAKE_BINDING = 1u << 4,
    RT_SMOKE_DYNAMIC_WAKE_ORDERED = 1u << 5,
    RT_SMOKE_DYNAMIC_WAKE_OVERRIDE = 1u << 6,
    RT_SMOKE_DYNAMIC_WAKE_FACTS = 1u << 7
};

enum RtSmokeMaterialDynamicSubclassBits : uint32_t
{
    RT_SMOKE_DYNAMIC_SUBCLASS_GUI = 1u << 0,
    RT_SMOKE_DYNAMIC_SUBCLASS_REGISTERS = 1u << 1,
    RT_SMOKE_DYNAMIC_SUBCLASS_TEXTURE_MATRIX = 1u << 2,
    RT_SMOKE_DYNAMIC_SUBCLASS_SPECTRUM = 1u << 3,
    RT_SMOKE_DYNAMIC_SUBCLASS_DECAL = 1u << 4,
    RT_SMOKE_DYNAMIC_SUBCLASS_VIDEO = 1u << 5,
    RT_SMOKE_DYNAMIC_SUBCLASS_RENDER_TARGET = 1u << 6,
    RT_SMOKE_DYNAMIC_SUBCLASS_OTHER = 1u << 7
};

struct RtSmokeMaterialPrimaryBindingProduct
{
    uint32_t descriptorIndex = UINT32_MAX;
    nvrhi::TextureHandle texture;
    uintptr_t textureIdentity = 0;
};

struct RtSmokeMaterialRowProductSnapshot
{
    bool valid = false;
    PathTraceSmokeMaterial row = {};
    RtSmokeMaterialUniverseFacts facts;
    RtPathTraceMaterialFeatureRecord feature;
    RtPathTraceMaterialFeatureParameterRecord parameters;
    RtSmokeMaterialPrimaryBindingProduct bindings[5];
};

struct RtSmokeMaterialRowProductDelta
{
    bool comparisonError = false;
    bool row = false;
    bool facts = false;
    bool feature = false;
    bool parameters = false;
    bool binding = false;

    bool Any() const
    {
        return row || facts || feature || parameters || binding;
    }
};

bool SmokeMaterialFloatBitsEqual(float lhs, float rhs)
{
    return std::memcmp(&lhs, &rhs, sizeof(float)) == 0;
}

bool SmokeMaterialVec4BitsEqual(const idVec4& lhs, const idVec4& rhs)
{
    return SmokeMaterialFloatBitsEqual(lhs.x, rhs.x) &&
        SmokeMaterialFloatBitsEqual(lhs.y, rhs.y) &&
        SmokeMaterialFloatBitsEqual(lhs.z, rhs.z) &&
        SmokeMaterialFloatBitsEqual(lhs.w, rhs.w);
}

bool SmokeMaterialRowProductsEqual(
    const PathTraceSmokeMaterial& lhs,
    const PathTraceSmokeMaterial& rhs)
{
    return std::memcmp(lhs.debugAlbedo, rhs.debugAlbedo,
            sizeof(lhs.debugAlbedo)) == 0 &&
        std::memcmp(lhs.emissiveColor, rhs.emissiveColor,
            sizeof(lhs.emissiveColor)) == 0 &&
        lhs.diffuseTextureIndex == rhs.diffuseTextureIndex &&
        lhs.alphaTextureIndex == rhs.alphaTextureIndex &&
        lhs.normalTextureIndex == rhs.normalTextureIndex &&
        lhs.specularTextureIndex == rhs.specularTextureIndex &&
        lhs.emissiveTextureIndex == rhs.emissiveTextureIndex &&
        SmokeMaterialFloatBitsEqual(lhs.alphaCutoff, rhs.alphaCutoff) &&
        lhs.flags == rhs.flags &&
        lhs.textureWidth == rhs.textureWidth &&
        lhs.textureHeight == rhs.textureHeight &&
        lhs.alphaTextureWidth == rhs.alphaTextureWidth &&
        lhs.alphaTextureHeight == rhs.alphaTextureHeight &&
        lhs.normalTextureWidth == rhs.normalTextureWidth &&
        lhs.normalTextureHeight == rhs.normalTextureHeight &&
        lhs.specularTextureWidth == rhs.specularTextureWidth &&
        lhs.specularTextureHeight == rhs.specularTextureHeight &&
        lhs.emissiveTextureWidth == rhs.emissiveTextureWidth &&
        lhs.emissiveTextureHeight == rhs.emissiveTextureHeight &&
        lhs.padding0 == rhs.padding0 && lhs.padding1 == rhs.padding1 &&
        lhs.padding2 == rhs.padding2;
}

bool SmokeMaterialFactsProductsEqual(
    const RtSmokeMaterialUniverseFacts& lhs,
    const RtSmokeMaterialUniverseFacts& rhs)
{
    return lhs.materialId == rhs.materialId &&
        lhs.universeIndex == rhs.universeIndex &&
        lhs.materialFlags == rhs.materialFlags &&
        lhs.isDynamic == rhs.isDynamic &&
        lhs.hasFallbackAlbedo == rhs.hasFallbackAlbedo &&
        SmokeMaterialVec4BitsEqual(lhs.fallbackAlbedo, rhs.fallbackAlbedo) &&
        lhs.alphaTested == rhs.alphaTested &&
        SmokeMaterialFloatBitsEqual(lhs.alphaCutoff, rhs.alphaCutoff) &&
        lhs.diffuseYCoCg == rhs.diffuseYCoCg &&
        lhs.additiveDecal == rhs.additiveDecal &&
        lhs.additiveDecalWhiteKey == rhs.additiveDecalWhiteKey &&
        lhs.filterDecal == rhs.filterDecal &&
        lhs.filterDecalBlackKey == rhs.filterDecalBlackKey &&
        lhs.detailDecal == rhs.detailDecal &&
        lhs.detailDecalDynamic == rhs.detailDecalDynamic &&
        lhs.detailDecalLiquidPool == rhs.detailDecalLiquidPool &&
        lhs.liquidFilmHasBloodSemantic == rhs.liquidFilmHasBloodSemantic &&
        lhs.liquidFilmHasWetReflectStage == rhs.liquidFilmHasWetReflectStage &&
        lhs.liquidFilmHasCoverageSource == rhs.liquidFilmHasCoverageSource &&
        lhs.liquidFilmHasWetNormalSource == rhs.liquidFilmHasWetNormalSource &&
        lhs.liquidFilmExactOverride == rhs.liquidFilmExactOverride &&
        lhs.liquidFilmCandidate == rhs.liquidFilmCandidate &&
        lhs.alphaFromDiffuseLuma == rhs.alphaFromDiffuseLuma &&
        lhs.forceFallbackAlbedo == rhs.forceFallbackAlbedo &&
        lhs.alphaFromDiffuseDarkKey == rhs.alphaFromDiffuseDarkKey &&
        lhs.alphaFromDiffuseMagentaKey == rhs.alphaFromDiffuseMagentaKey &&
        lhs.portalWindowFallback == rhs.portalWindowFallback &&
        lhs.objectGlassFallback == rhs.objectGlassFallback &&
        lhs.skyEnvironment == rhs.skyEnvironment &&
        lhs.emissive == rhs.emissive &&
        lhs.emissiveLightCandidate == rhs.emissiveLightCandidate &&
        SmokeMaterialVec4BitsEqual(lhs.emissiveColor, rhs.emissiveColor) &&
        SmokeMaterialFloatBitsEqual(lhs.emissiveLuminance, rhs.emissiveLuminance) &&
        lhs.hasDiffuseImage == rhs.hasDiffuseImage &&
        lhs.hasSafeDiffuseTexture == rhs.hasSafeDiffuseTexture &&
        lhs.hasAlphaImage == rhs.hasAlphaImage &&
        lhs.hasSafeAlphaTexture == rhs.hasSafeAlphaTexture &&
        lhs.hasNormalImage == rhs.hasNormalImage &&
        lhs.hasSafeNormalTexture == rhs.hasSafeNormalTexture &&
        lhs.hasSpecularImage == rhs.hasSpecularImage &&
        lhs.hasSafeSpecularTexture == rhs.hasSafeSpecularTexture &&
        lhs.hasEmissiveImage == rhs.hasEmissiveImage &&
        lhs.hasSafeEmissiveTexture == rhs.hasSafeEmissiveTexture &&
        lhs.guiTextureCandidate == rhs.guiTextureCandidate;
}

bool SmokeMaterialFeatureProductsEqual(
    const RtPathTraceMaterialFeatureRecord& lhs,
    const RtPathTraceMaterialFeatureRecord& rhs)
{
    return lhs.materialKind == rhs.materialKind &&
        lhs.materialCaps == rhs.materialCaps &&
        lhs.lobeCaps == rhs.lobeCaps &&
        lhs.passSupport == rhs.passSupport &&
        lhs.modifierKind == rhs.modifierKind &&
        lhs.parameterRecordIndex == rhs.parameterRecordIndex &&
        lhs.recordAbiVersion == rhs.recordAbiVersion &&
        lhs.reserved1 == rhs.reserved1;
}

bool SmokeMaterialParameterProductsEqual(
    const RtPathTraceMaterialFeatureParameterRecord& lhs,
    const RtPathTraceMaterialFeatureParameterRecord& rhs)
{
    return std::memcmp(lhs.params0, rhs.params0, sizeof(lhs.params0)) == 0 &&
        std::memcmp(lhs.params1, rhs.params1, sizeof(lhs.params1)) == 0 &&
        std::memcmp(lhs.orderedStageWords, rhs.orderedStageWords,
            sizeof(lhs.orderedStageWords)) == 0 &&
        std::memcmp(lhs.orderedStageTextureWords,
            rhs.orderedStageTextureWords,
            sizeof(lhs.orderedStageTextureWords)) == 0;
}

bool CaptureSmokeMaterialPrimaryBindingProduct(
    const RtSmokeMaterialTableBuild& table,
    uint32_t descriptorIndex,
    RtSmokeMaterialPrimaryBindingProduct& product)
{
    product.descriptorIndex = descriptorIndex;
    product.texture.Reset();
    product.textureIdentity = 0;
    if (descriptorIndex == UINT32_MAX)
    {
        return true;
    }
    if (descriptorIndex >= table.diffuseTextures.size())
    {
        return false;
    }
    product.texture = table.diffuseTextures[descriptorIndex];
    product.textureIdentity =
        reinterpret_cast<uintptr_t>(product.texture.Get());
    return true;
}

bool CaptureSmokeMaterialRowProductSnapshot(
    const RtSmokeMaterialTableBuild& table,
    int tableIndex,
    RtSmokeMaterialRowProductSnapshot& snapshot)
{
    snapshot = RtSmokeMaterialRowProductSnapshot();
    if (!SmokeMaterialTableIndexIsValid(table, tableIndex) ||
        tableIndex >= static_cast<int>(table.materialFacts.size()) ||
        tableIndex >= static_cast<int>(table.materialFeatures.size()) ||
        tableIndex >= static_cast<int>(table.materialFeatureParameters.size()))
    {
        return false;
    }
    snapshot.row = table.materials[tableIndex];
    snapshot.facts = table.materialFacts[tableIndex];
    snapshot.feature = table.materialFeatures[tableIndex];
    snapshot.parameters = table.materialFeatureParameters[tableIndex];
    const uint32_t descriptorIndexes[5] = {
        snapshot.row.diffuseTextureIndex,
        snapshot.row.alphaTextureIndex,
        snapshot.row.normalTextureIndex,
        snapshot.row.specularTextureIndex,
        snapshot.row.emissiveTextureIndex
    };
    for (int bindingIndex = 0; bindingIndex < 5; ++bindingIndex)
    {
        if (!CaptureSmokeMaterialPrimaryBindingProduct(
                table,
                descriptorIndexes[bindingIndex],
                snapshot.bindings[bindingIndex]))
        {
            return false;
        }
    }
    snapshot.valid = true;
    return true;
}

bool SmokeMaterialBindingProductsEqual(
    const RtSmokeMaterialPrimaryBindingProduct& lhs,
    const RtSmokeMaterialPrimaryBindingProduct& rhs)
{
    // Descriptor index is part of the shader product: moving the same handle to
    // another slot is intentionally a binding delta.
    return lhs.descriptorIndex == rhs.descriptorIndex &&
        lhs.textureIdentity == rhs.textureIdentity;
}

RtSmokeMaterialRowProductDelta CompareSmokeMaterialRowProducts(
    const RtSmokeMaterialRowProductSnapshot& before,
    const RtSmokeMaterialRowProductSnapshot& after)
{
    RtSmokeMaterialRowProductDelta delta;
    if (!before.valid || !after.valid)
    {
        delta.comparisonError = true;
        return delta;
    }
    delta.row = !SmokeMaterialRowProductsEqual(before.row, after.row);
    delta.facts = !SmokeMaterialFactsProductsEqual(before.facts, after.facts);
    delta.feature = !SmokeMaterialFeatureProductsEqual(
        before.feature,
        after.feature);
    delta.parameters = !SmokeMaterialParameterProductsEqual(
        before.parameters,
        after.parameters);
    for (int bindingIndex = 0; bindingIndex < 5; ++bindingIndex)
    {
        delta.binding = delta.binding ||
            !SmokeMaterialBindingProductsEqual(
                before.bindings[bindingIndex],
                after.bindings[bindingIndex]);
    }
    return delta;
}

bool SmokeMaterialStoredNameLooksRuntimeVideo(const idStr& name)
{
    return name.Find("cinematic", false) >= 0 ||
        name.Find("currentrender", false) >= 0 ||
        name.Find("scratch", false) >= 0;
}

uint32_t FinalizeSmokeMaterialDynamicSubclassMask(uint32_t mask)
{
    return mask != 0u ? mask : RT_SMOKE_DYNAMIC_SUBCLASS_OTHER;
}

uint32_t BuildSmokeMaterialDynamicSubclassMask(
    const idMaterial* material,
    const RtSmokeMaterialTextureInfo& info)
{
    uint32_t mask = 0u;
    const idStr* storedNames[] = {
        &info.materialName,
        &info.diffuseImageName,
        &info.alphaImageName,
        &info.normalImageName,
        &info.specularImageName,
        &info.emissiveImageName,
        &info.skyImageName
    };
    bool guiLike = material && material->HasGui();
    bool runtimeVideo = false;
    for (const idStr* name : storedNames)
    {
        guiLike = guiLike || IsSmokeImageNameGuiLike(name->c_str());
        runtimeVideo = runtimeVideo ||
            SmokeMaterialStoredNameLooksRuntimeVideo(*name);
    }
    mask |= guiLike ? RT_SMOKE_DYNAMIC_SUBCLASS_GUI : 0u;
    mask |= material && material->ConstantRegisters() == nullptr
        ? RT_SMOKE_DYNAMIC_SUBCLASS_REGISTERS
        : 0u;
    if (material)
    {
        for (int stageIndex = 0;
            stageIndex < material->GetNumStages();
            ++stageIndex)
        {
            const shaderStage_t* stage = material->GetStage(stageIndex);
            if (stage && stage->texture.hasMatrix)
            {
                mask |= RT_SMOKE_DYNAMIC_SUBCLASS_TEXTURE_MATRIX;
                break;
            }
        }
    }
    mask |= info.detailDecalSpectrum > 0
        ? RT_SMOKE_DYNAMIC_SUBCLASS_SPECTRUM
        : 0u;
    mask |= info.detailDecalDynamic
        ? RT_SMOKE_DYNAMIC_SUBCLASS_DECAL
        : 0u;
    mask |= runtimeVideo ? RT_SMOKE_DYNAMIC_SUBCLASS_VIDEO : 0u;
    const nvrhi::TextureHandle primaryTextures[] = {
        info.diffuseTexture,
        info.alphaTexture,
        info.normalTexture,
        info.specularTexture,
        info.emissiveTexture
    };
    for (const nvrhi::TextureHandle& texture : primaryTextures)
    {
        if (texture)
        {
            const nvrhi::TextureDesc& desc = texture->getDesc();
            if (desc.isRenderTarget || desc.isUAV)
            {
                mask |= RT_SMOKE_DYNAMIC_SUBCLASS_RENDER_TARGET;
                break;
            }
        }
    }
    return FinalizeSmokeMaterialDynamicSubclassMask(mask);
}

uint32_t SelectSmokeMaterialExclusiveWake(uint32_t wakeMask)
{
    const uint32_t priority[] = {
        RT_SMOKE_DYNAMIC_WAKE_APPEND,
        RT_SMOKE_DYNAMIC_WAKE_NEW,
        RT_SMOKE_DYNAMIC_WAKE_BINDING,
        RT_SMOKE_DYNAMIC_WAKE_FACTS,
        RT_SMOKE_DYNAMIC_WAKE_OVERRIDE,
        RT_SMOKE_DYNAMIC_WAKE_DYNAMIC,
        RT_SMOKE_DYNAMIC_WAKE_ORDERED,
        RT_SMOKE_DYNAMIC_WAKE_SIGNATURE
    };
    for (uint32_t wake : priority)
    {
        if ((wakeMask & wake) != 0u)
        {
            return wake;
        }
    }
    return 0u;
}

void RecordSmokeMaterialDynamicWakeCounters(
    RtSmokeMaterialTableCacheStats::DynamicPartition& counters,
    uint32_t wakeMask)
{
    counters.wakeDynamic +=
        (wakeMask & RT_SMOKE_DYNAMIC_WAKE_DYNAMIC) != 0u ? 1 : 0;
    counters.wakeAppend +=
        (wakeMask & RT_SMOKE_DYNAMIC_WAKE_APPEND) != 0u ? 1 : 0;
    counters.wakeNew +=
        (wakeMask & RT_SMOKE_DYNAMIC_WAKE_NEW) != 0u ? 1 : 0;
    counters.wakeSignature +=
        (wakeMask & RT_SMOKE_DYNAMIC_WAKE_SIGNATURE) != 0u ? 1 : 0;
    counters.wakeBinding +=
        (wakeMask & RT_SMOKE_DYNAMIC_WAKE_BINDING) != 0u ? 1 : 0;
    counters.wakeOrdered +=
        (wakeMask & RT_SMOKE_DYNAMIC_WAKE_ORDERED) != 0u ? 1 : 0;
    counters.wakeOverride +=
        (wakeMask & RT_SMOKE_DYNAMIC_WAKE_OVERRIDE) != 0u ? 1 : 0;
    counters.wakeFacts +=
        (wakeMask & RT_SMOKE_DYNAMIC_WAKE_FACTS) != 0u ? 1 : 0;

    switch (SelectSmokeMaterialExclusiveWake(wakeMask))
    {
        case RT_SMOKE_DYNAMIC_WAKE_DYNAMIC: ++counters.exclusiveDynamic; break;
        case RT_SMOKE_DYNAMIC_WAKE_APPEND: ++counters.exclusiveAppend; break;
        case RT_SMOKE_DYNAMIC_WAKE_NEW: ++counters.exclusiveNew; break;
        case RT_SMOKE_DYNAMIC_WAKE_SIGNATURE: ++counters.exclusiveSignature; break;
        case RT_SMOKE_DYNAMIC_WAKE_BINDING: ++counters.exclusiveBinding; break;
        case RT_SMOKE_DYNAMIC_WAKE_ORDERED: ++counters.exclusiveOrdered; break;
        case RT_SMOKE_DYNAMIC_WAKE_OVERRIDE: ++counters.exclusiveOverride; break;
        case RT_SMOKE_DYNAMIC_WAKE_FACTS: ++counters.exclusiveFacts; break;
        default: ++counters.exclusiveOther; break;
    }
}

void RecordSmokeMaterialDynamicSubclassCounters(
    RtSmokeMaterialTableCacheStats::DynamicPartition& counters,
    uint32_t subclassMask)
{
    counters.subclassGui +=
        (subclassMask & RT_SMOKE_DYNAMIC_SUBCLASS_GUI) != 0u ? 1 : 0;
    counters.subclassRegisters +=
        (subclassMask & RT_SMOKE_DYNAMIC_SUBCLASS_REGISTERS) != 0u ? 1 : 0;
    counters.subclassTextureMatrix +=
        (subclassMask & RT_SMOKE_DYNAMIC_SUBCLASS_TEXTURE_MATRIX) != 0u ? 1 : 0;
    counters.subclassSpectrum +=
        (subclassMask & RT_SMOKE_DYNAMIC_SUBCLASS_SPECTRUM) != 0u ? 1 : 0;
    counters.subclassDecal +=
        (subclassMask & RT_SMOKE_DYNAMIC_SUBCLASS_DECAL) != 0u ? 1 : 0;
    counters.subclassVideo +=
        (subclassMask & RT_SMOKE_DYNAMIC_SUBCLASS_VIDEO) != 0u ? 1 : 0;
    counters.subclassRenderTarget +=
        (subclassMask & RT_SMOKE_DYNAMIC_SUBCLASS_RENDER_TARGET) != 0u ? 1 : 0;
    counters.subclassOther +=
        (subclassMask & RT_SMOKE_DYNAMIC_SUBCLASS_OTHER) != 0u ? 1 : 0;
}

void AccumulateSmokeMaterialDynamicPartition(
    RtSmokeMaterialTableCacheStats::DynamicPartition& destination,
    const RtSmokeMaterialTableCacheStats::DynamicPartition& source)
{
#define ACCUMULATE_DYNAMIC_PARTITION_FIELD(field) destination.field += source.field
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(rebuilt);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(compareIdentical);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(comparePartial);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(compareError);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(identicalDynamic);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(dynamicRebuilt);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(runtimeCoveredCandidate);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(wakeDynamic);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(wakeAppend);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(wakeNew);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(wakeSignature);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(wakeBinding);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(wakeOrdered);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(wakeOverride);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(wakeFacts);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(exclusiveDynamic);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(exclusiveAppend);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(exclusiveNew);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(exclusiveSignature);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(exclusiveBinding);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(exclusiveOrdered);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(exclusiveOverride);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(exclusiveFacts);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(exclusiveOther);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(subclassGui);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(subclassRegisters);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(subclassTextureMatrix);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(subclassSpectrum);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(subclassDecal);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(subclassVideo);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(subclassRenderTarget);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(subclassOther);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(productRow);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(productFacts);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(productFeature);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(productParameters);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(productBinding);
    ACCUMULATE_DYNAMIC_PARTITION_FIELD(invariantFailures);
#undef ACCUMULATE_DYNAMIC_PARTITION_FIELD
}

struct RtSmokeMaterialDirtyRowWork
{
    bool allowUnboundAtTextureCapacity = false;
    uint32_t textureCapacityMisses = 0;
    int consideredRows = 0;
    int rebuiltRows = 0;
    int skippedUnchangedRows = 0;
    int dynamicRows = 0;
    int orderedStageConsideredRows = 0;
    int orderedStageSkippedRows = 0;
    int orderedStageRebuiltRows = 0;
    int orderedStageEpochEligibleRows = 0;
    int orderedStageScannedDueEpochRows = 0;
    int orderedStageAvoidedSteadyRows = 0;
    int nonPrimaryGenerationWakeRows = 0;
    int activeRows = 0;
    int classifierFinds = 0;
    int textureSlotsAppended = 0;
    int textureSlotsReused = 0;
    RtSmokeMaterialTableCacheStats::DynamicPartition dynamicPartition;
    uint64 totalMicroseconds = 0;
    uint64 tableCopyMicroseconds = 0;
    uint64 textureRefcountMicroseconds = 0;
    uint64 classifierLookupMicroseconds = 0;
    uint64 orderedFingerprintMicroseconds = 0;
    uint64 rowResolveMicroseconds = 0;
    uint64 rowRebuildMicroseconds = 0;
    std::vector<uint32_t> textureSlotReferenceCounts;
};

struct RtSmokeMaterialDirtyRowCandidate
{
    int tableIndex = -1;
    uint32_t materialId = 0;
    bool appended = false;
    bool cachedDynamic = false;
    bool newlyActive = false;
    bool orderedStage = false;
    bool dynamic = false;
    bool rebuild = false;
    uint32_t wakeMask = 0u;
    uint32_t dynamicSubclassMask = 0u;
    RtSmokeMaterialTextureInfo info;
    const RtSmokePersistentMaterialRecord* persistentRecord = nullptr;
    const RtMaterialRecord* classifierRecord = nullptr;
    uint64 rowSourceSignature = 0;
};

bool AddSmokeMaterialPatchTextureReference(
    uint32_t descriptorIndex,
    size_t textureCount,
    std::vector<uint32_t>& referenceCounts)
{
    if (descriptorIndex == UINT32_MAX)
    {
        return true;
    }
    if (descriptorIndex >= textureCount ||
        descriptorIndex >= referenceCounts.size())
    {
        return false;
    }
    ++referenceCounts[descriptorIndex];
    return true;
}

bool InitializeSmokeMaterialPatchTextureReferences(
    const RtSmokeMaterialTableBuild& table,
    const std::vector<bool>& activeRows,
    RtSmokeMaterialDirtyRowWork& work)
{
    work.textureSlotReferenceCounts.assign(table.diffuseTextures.size(), 0u);
    if (activeRows.size() != table.materials.size() ||
        table.materialFeatureParameters.size() != table.materials.size())
    {
        return false;
    }
    for (int tableIndex = 0;
        tableIndex < static_cast<int>(table.materials.size());
        ++tableIndex)
    {
        if (!activeRows[tableIndex])
        {
            continue;
        }
        const PathTraceSmokeMaterial& material = table.materials[tableIndex];
        if (!AddSmokeMaterialPatchTextureReference(
                material.diffuseTextureIndex,
                table.diffuseTextures.size(),
                work.textureSlotReferenceCounts) ||
            !AddSmokeMaterialPatchTextureReference(
                material.alphaTextureIndex,
                table.diffuseTextures.size(),
                work.textureSlotReferenceCounts) ||
            !AddSmokeMaterialPatchTextureReference(
                material.normalTextureIndex,
                table.diffuseTextures.size(),
                work.textureSlotReferenceCounts) ||
            !AddSmokeMaterialPatchTextureReference(
                material.specularTextureIndex,
                table.diffuseTextures.size(),
                work.textureSlotReferenceCounts) ||
            !AddSmokeMaterialPatchTextureReference(
                material.emissiveTextureIndex,
                table.diffuseTextures.size(),
                work.textureSlotReferenceCounts))
        {
            return false;
        }
        for (uint32_t textureWord :
            table.materialFeatureParameters[tableIndex].orderedStageTextureWords)
        {
            if ((textureWord & 0x80000000u) != 0 &&
                !AddSmokeMaterialPatchTextureReference(
                    textureWord & 0x7fffffffu,
                    table.diffuseTextures.size(),
                    work.textureSlotReferenceCounts))
            {
                return false;
            }
        }
    }
    return true;
}

bool ReleaseSmokeMaterialPatchTextureReference(
    uint32_t descriptorIndex,
    std::vector<uint32_t>& referenceCounts)
{
    if (descriptorIndex == UINT32_MAX)
    {
        return true;
    }
    if (descriptorIndex >= referenceCounts.size() ||
        referenceCounts[descriptorIndex] == 0)
    {
        return false;
    }
    --referenceCounts[descriptorIndex];
    return true;
}

bool ReleaseSmokeMaterialPatchTextureReferencesForRow(
    const RtSmokeMaterialTableBuild& table,
    int tableIndex,
    RtSmokeMaterialDirtyRowWork& work)
{
    if (!SmokeMaterialTableIndexIsValid(table, tableIndex))
    {
        return false;
    }
    const PathTraceSmokeMaterial& material = table.materials[tableIndex];
    if (!ReleaseSmokeMaterialPatchTextureReference(
            material.diffuseTextureIndex,
            work.textureSlotReferenceCounts) ||
        !ReleaseSmokeMaterialPatchTextureReference(
            material.alphaTextureIndex,
            work.textureSlotReferenceCounts) ||
        !ReleaseSmokeMaterialPatchTextureReference(
            material.normalTextureIndex,
            work.textureSlotReferenceCounts) ||
        !ReleaseSmokeMaterialPatchTextureReference(
            material.specularTextureIndex,
            work.textureSlotReferenceCounts) ||
        !ReleaseSmokeMaterialPatchTextureReference(
            material.emissiveTextureIndex,
            work.textureSlotReferenceCounts))
    {
        return false;
    }
    for (uint32_t textureWord :
        table.materialFeatureParameters[tableIndex].orderedStageTextureWords)
    {
        if ((textureWord & 0x80000000u) != 0 &&
            !ReleaseSmokeMaterialPatchTextureReference(
                textureWord & 0x7fffffffu,
                work.textureSlotReferenceCounts))
        {
            return false;
        }
    }
    return true;
}

int FindSmokeMaterialReusableTextureSlot(
    const std::vector<uint32_t>& referenceCounts)
{
    for (int textureIndex = 0;
        textureIndex < static_cast<int>(referenceCounts.size());
        ++textureIndex)
    {
        if (referenceCounts[textureIndex] == 0)
        {
            return textureIndex;
        }
    }
    return -1;
}

bool ResolveSmokeMaterialPatchTextureSlot(
    RtSmokeMaterialTableBuild& table,
    nvrhi::TextureHandle texture,
    int textureTableLimit,
    uint32_t& descriptorIndex,
    RtSmokeMaterialDirtyRowWork& work)
{
    descriptorIndex = UINT32_MAX;
    if (!texture || !IsSmokeTextureHandleSafeForDescriptor(texture))
    {
        return true;
    }
    if (textureTableLimit <= 0)
    {
        return true;
    }
    if (work.textureSlotReferenceCounts.size() !=
        table.diffuseTextures.size())
    {
        return false;
    }
    for (int textureIndex = 0;
        textureIndex < static_cast<int>(table.diffuseTextures.size());
        ++textureIndex)
    {
        if (table.diffuseTextures[textureIndex] == texture)
        {
            descriptorIndex = static_cast<uint32_t>(textureIndex);
            ++work.textureSlotReferenceCounts[textureIndex];
            ++work.textureSlotsReused;
            return true;
        }
    }
    if (static_cast<int>(table.diffuseTextures.size()) < textureTableLimit)
    {
        descriptorIndex = static_cast<uint32_t>(table.diffuseTextures.size());
        table.diffuseTextures.push_back(texture);
        work.textureSlotReferenceCounts.push_back(1u);
        ++work.textureSlotsAppended;
        return true;
    }
    const int reusableTextureIndex =
        FindSmokeMaterialReusableTextureSlot(
            work.textureSlotReferenceCounts);
    if (reusableTextureIndex >= 0)
    {
        descriptorIndex = static_cast<uint32_t>(reusableTextureIndex);
        table.diffuseTextures[reusableTextureIndex] = texture;
        work.textureSlotReferenceCounts[reusableTextureIndex] = 1u;
        ++work.textureSlotsReused;
        return true;
    }
    if (work.allowUnboundAtTextureCapacity)
    {
        ++work.textureCapacityMisses;
        return true;
    }
    return false;
}

uint32_t BuildSmokeOrderedStageTextureWord(uint32_t descriptorIndex)
{
    return descriptorIndex == UINT32_MAX
        ? 0u
        : 0x80000000u | descriptorIndex;
}

bool PopulateSmokeOrderedStageTextureSlotsForRow(
    RtSmokeMaterialTableBuild& table,
    int tableIndex,
    int textureTableLimit,
    RtSmokeMaterialDirtyRowWork& work)
{
    if (tableIndex < 0 ||
        tableIndex >= static_cast<int>(table.materialIds.size()) ||
        tableIndex >= static_cast<int>(table.materialInfos.size()) ||
        tableIndex >= static_cast<int>(table.materialFeatureParameters.size()))
    {
        return false;
    }

    RtPathTraceMaterialFeatureParameterRecord& parameters =
        table.materialFeatureParameters[tableIndex];
    for (uint32_t& textureWord : parameters.orderedStageTextureWords)
    {
        textureWord = 0u;
    }
    if (textureTableLimit <= 0 || !declManager)
    {
        return true;
    }

    const RtMaterialRecord* record =
        FindPathTraceMaterialRecord(table.materialIds[tableIndex]);
    if (!record || !record->valid)
    {
        return true;
    }
    const idMaterial* material = declManager->FindMaterial(
        table.materialInfos[tableIndex].materialName.c_str(),
        false);
    if (!material)
    {
        return true;
    }

    const int stageCount = Min(
        static_cast<int>(record->compositingStages.size()),
        RT_PATH_TRACE_ORDERED_STAGE_CAPACITY);
    for (int stageSlot = 0; stageSlot < stageCount; ++stageSlot)
    {
        const int declarationStageIndex =
            record->compositingStages[stageSlot].stageIndex;
        if (declarationStageIndex < 0 ||
            declarationStageIndex >= material->GetNumStages())
        {
            continue;
        }
        const shaderStage_t* stage = material->GetStage(declarationStageIndex);
        idImage* image = stage ? stage->texture.image : nullptr;
        const idPathTracingTextureBindingSnapshot binding = image
            ? image->GetPathTracingTextureBindingSnapshot()
            : idPathTracingTextureBindingSnapshot();
        const idImageOpts* imageOpts = image ? &image->GetOpts() : nullptr;
        const bool guiTextureOverride = image &&
            r_pathTracingAllowGuiTextures.GetInteger() != 0 &&
            IsSmokeImageNameGuiLike(image->GetName());
        const bool safeImage = image && imageOpts &&
            imageOpts->samples == 1 && imageOpts->textureType == DTT_2D &&
            (!(imageOpts->isRenderTarget || imageOpts->isUAV) || guiTextureOverride) &&
            (IsSmokeImageNameSafeForRayTracing(image->GetName()) || guiTextureOverride) &&
            IsSmokeTextureHandleSafeForDescriptor(binding.texture);
        if (!safeImage)
        {
            continue;
        }
        uint32_t descriptorIndex = UINT32_MAX;
        if (!ResolveSmokeMaterialPatchTextureSlot(
                table,
                binding.texture,
                textureTableLimit,
                descriptorIndex,
                work))
        {
            return false;
        }
        if (descriptorIndex != UINT32_MAX)
        {
            parameters.orderedStageTextureWords[stageSlot] =
                BuildSmokeOrderedStageTextureWord(descriptorIndex);
        }
    }
    return true;
}

void ResetSmokeMaterialTextureBindings(PathTraceSmokeMaterial& material)
{
    material.diffuseTextureIndex = UINT32_MAX;
    material.alphaTextureIndex = UINT32_MAX;
    material.normalTextureIndex = UINT32_MAX;
    material.specularTextureIndex = UINT32_MAX;
    material.emissiveTextureIndex = UINT32_MAX;
    material.textureWidth = 1;
    material.textureHeight = 1;
    material.alphaTextureWidth = 1;
    material.alphaTextureHeight = 1;
    material.normalTextureWidth = 1;
    material.normalTextureHeight = 1;
    material.specularTextureWidth = 1;
    material.specularTextureHeight = 1;
    material.emissiveTextureWidth = 1;
    material.emissiveTextureHeight = 1;
}

bool ResetSmokeInactiveMaterialBindings(
    RtSmokeMaterialTableBuild& table,
    const std::vector<bool>& activeRows)
{
    if (activeRows.size() != table.materials.size() ||
        table.materialFeatureParameters.size() != table.materials.size())
    {
        return false;
    }
    for (int tableIndex = 0;
        tableIndex < static_cast<int>(activeRows.size());
        ++tableIndex)
    {
        if (activeRows[tableIndex])
        {
            continue;
        }
        ResetSmokeMaterialTextureBindings(table.materials[tableIndex]);
        for (uint32_t& textureWord :
            table.materialFeatureParameters[tableIndex].orderedStageTextureWords)
        {
            textureWord = 0u;
        }
    }
    return true;
}

bool PatchSmokeMaterialTableRow(
    RtSmokeMaterialTableBuild& table,
    int tableIndex,
    uint32_t materialId,
    const RtSmokeMaterialTextureInfo& info,
    const RtSmokePersistentMaterialRecord& record,
    bool enableTextureProbe,
    int minimumTextureTableLimit,
    RtSmokeMaterialDirtyRowWork& work)
{
    if (!SmokeMaterialTableIndexIsValid(table, tableIndex) ||
        table.materialIds[tableIndex] != materialId)
    {
        return false;
    }

    table.materialInfos[tableIndex] = info;
    table.materialFacts[tableIndex] = record.facts;
    table.materials[tableIndex] =
        BuildSmokeMaterialTableMaterial(materialId, info, record);
    ResetSmokeMaterialTextureBindings(table.materials[tableIndex]);
    table.materialFeatures[tableIndex] = BuildSmokeMaterialFeatureRecord(
        record.facts,
        static_cast<uint32_t>(tableIndex));
    table.materialFeatureParameters[tableIndex] =
        BuildSmokeMaterialFeatureParameters(materialId, record.facts);

    if (!enableTextureProbe)
    {
        return true;
    }

    const int textureTableLimit =
        GetSmokeTextureTableEffectiveLimitWithMinimum(minimumTextureTableLimit);
    PathTraceSmokeMaterial& material = table.materials[tableIndex];
    uint32_t descriptorIndex = UINT32_MAX;
    if (info.hasSafeTexture &&
        !ResolveSmokeMaterialPatchTextureSlot(
            table,
            info.diffuseTexture,
            textureTableLimit,
            descriptorIndex,
            work))
    {
        return false;
    }
    material.diffuseTextureIndex = descriptorIndex;
    if (descriptorIndex != UINT32_MAX)
    {
        const nvrhi::TextureDesc& desc = info.diffuseTexture->getDesc();
        material.textureWidth = Max(1u, desc.width);
        material.textureHeight = Max(1u, desc.height);
    }

    descriptorIndex = UINT32_MAX;
    const bool needsAlphaCoverageTexture =
        info.hasAlphaTest || record.facts.liquidFilmCandidate;
    if (needsAlphaCoverageTexture && info.hasSafeAlphaTexture &&
        !ResolveSmokeMaterialPatchTextureSlot(
            table,
            info.alphaTexture,
            textureTableLimit,
            descriptorIndex,
            work))
    {
        return false;
    }
    material.alphaTextureIndex = descriptorIndex;
    if (descriptorIndex != UINT32_MAX)
    {
        const nvrhi::TextureDesc& desc = info.alphaTexture->getDesc();
        material.alphaTextureWidth = Max(1u, desc.width);
        material.alphaTextureHeight = Max(1u, desc.height);
    }

    descriptorIndex = UINT32_MAX;
    if (info.hasSafeNormalTexture &&
        !ResolveSmokeMaterialPatchTextureSlot(
            table,
            info.normalTexture,
            textureTableLimit,
            descriptorIndex,
            work))
    {
        return false;
    }
    material.normalTextureIndex = descriptorIndex;
    if (descriptorIndex != UINT32_MAX)
    {
        const nvrhi::TextureDesc& desc = info.normalTexture->getDesc();
        material.normalTextureWidth = Max(1u, desc.width);
        material.normalTextureHeight = Max(1u, desc.height);
    }

    descriptorIndex = UINT32_MAX;
    if (info.hasSafeSpecularTexture &&
        !ResolveSmokeMaterialPatchTextureSlot(
            table,
            info.specularTexture,
            textureTableLimit,
            descriptorIndex,
            work))
    {
        return false;
    }
    material.specularTextureIndex = descriptorIndex;
    if (descriptorIndex != UINT32_MAX)
    {
        const nvrhi::TextureDesc& desc = info.specularTexture->getDesc();
        material.specularTextureWidth = Max(1u, desc.width);
        material.specularTextureHeight = Max(1u, desc.height);
    }

    descriptorIndex = UINT32_MAX;
    if (info.hasSafeEmissiveTexture &&
        !ResolveSmokeMaterialPatchTextureSlot(
            table,
            info.emissiveTexture,
            textureTableLimit,
            descriptorIndex,
            work))
    {
        return false;
    }
    material.emissiveTextureIndex = descriptorIndex;
    if (descriptorIndex != UINT32_MAX)
    {
        const nvrhi::TextureDesc& desc = info.emissiveTexture->getDesc();
        material.emissiveTextureWidth = Max(1u, desc.width);
        material.emissiveTextureHeight = Max(1u, desc.height);
    }
    if (info.emissiveImage &&
        info.emissiveImage == info.diffuseImage &&
        material.emissiveTextureIndex == UINT32_MAX &&
        material.diffuseTextureIndex != UINT32_MAX)
    {
        material.emissiveTextureIndex = material.diffuseTextureIndex;
        ++work.textureSlotReferenceCounts[material.emissiveTextureIndex];
        material.emissiveTextureWidth = material.textureWidth;
        material.emissiveTextureHeight = material.textureHeight;
    }
    if (record.facts.hasEmissiveImage &&
        material.emissiveTextureIndex == UINT32_MAX &&
        r_pathTracingEmissiveFallbackWithoutTexture.GetInteger() == 0)
    {
        material.flags &= ~(RT_SMOKE_MATERIAL_EMISSIVE |
            RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE);
        material.emissiveColor[0] = 0.0f;
        material.emissiveColor[1] = 0.0f;
        material.emissiveColor[2] = 0.0f;
        material.emissiveColor[3] = 1.0f;
    }

    return PopulateSmokeOrderedStageTextureSlotsForRow(
        table,
        tableIndex,
        textureTableLimit,
        work);
}

bool AppendSmokeResidentMaterialRows(RtSmokeMaterialTableBuild& candidate,
    const std::vector<uint32_t>& materialIds, uint32_t& textureCapacityMisses)
{
    textureCapacityMisses = 0;
    if (!ValidateSmokeMaterialIndexes(candidate) ||
        candidate.materials.size() + materialIds.size() > 65536u ||
        candidate.diffuseTextures.size() > RT_SMOKE_TEXTURE_DESCRIPTOR_CAPACITY)
        return false;

    RtSmokeMaterialDirtyRowWork work;
    work.allowUnboundAtTextureCapacity = true;
    // Pin every old slot, including slots referenced by retained lighting/history.
    work.textureSlotReferenceCounts.assign(candidate.diffuseTextures.size(), 1u);
    for (uint32_t materialId : materialIds)
    {
        const size_t oldCount = candidate.materialIds.size();
        const uint32_t row = AddSmokeMaterialTableEntry(candidate, materialId);
        if (row < oldCount) continue;
        const RtSmokeMaterialTextureInfo info = candidate.materialInfos[row];
        const RtSmokePersistentMaterialRecord& record = GetSmokePersistentMaterialRecord(materialId, info);
        if (!PatchSmokeMaterialTableRow(candidate, static_cast<int>(row), materialId,
                info, record, true, RT_SMOKE_TEXTURE_DESCRIPTOR_CAPACITY, work))
            return false;
    }
    textureCapacityMisses = work.textureCapacityMisses;
    return ValidateSmokeMaterialIndexes(candidate);
}

uint64_t SmokeResidentMaterialConfigurationSignature()
{
    return HashSmokeMaterialCacheValue(ComputeSmokeMaterialTableSignatureFromIdSet(0, true,
        RT_SMOKE_TEXTURE_DESCRIPTOR_CAPACITY, 0, -1), GetPathTracingImageBindingEpoch());
}

bool RefreshSmokeResidentMaterialRows(RtSmokeMaterialTableBuild& candidate,
    std::vector<uint64_t>& sourceSignatures, uint32_t& textureCapacityMisses)
{
    textureCapacityMisses = 0;
    if (!ValidateSmokeMaterialIndexes(candidate)) return false;
    const auto breakdown = CaptureSmokeMaterialTableSignatureBreakdown(0, true,
        RT_SMOKE_TEXTURE_DESCRIPTOR_CAPACITY, 0, -1);
    const uint64 config = HashSmokeMaterialCacheValue(
        ComputeSmokeMaterialTableStructuralSignature(breakdown), breakdown.overrideGeneration);
    const size_t priorCount = sourceSignatures.size();
    sourceSignatures.resize(candidate.materialIds.size());
    uint32_t patched = 0;
    RtSmokeMaterialDirtyRowWork work;
    work.allowUnboundAtTextureCapacity = true;
    const std::vector<bool> active(candidate.materials.size(), true);
    if (!InitializeSmokeMaterialPatchTextureReferences(candidate, active, work)) return false;
    // Retained lighting/history can still refer to old descriptor slots.
    for (auto& references : work.textureSlotReferenceCounts) ++references;
    for (int row = 0; row < static_cast<int>(candidate.materialIds.size()); ++row)
    {
        const uint32_t id = candidate.materialIds[row];
        const auto info = ResolveSmokeMaterialTextureInfo(id, row);
        const auto& record = GetSmokePersistentMaterialRecord(id, info);
        const uint64 signature = HashSmokeMaterialCacheValue(config,
            ComputeSmokeMaterialRowSourceSignature(id, info, record, FindPathTraceMaterialRecord(id)));
        if (static_cast<size_t>(row) < priorCount && sourceSignatures[row] == signature) continue;

        if (!ReleaseSmokeMaterialPatchTextureReferencesForRow(candidate, row, work) ||
            !PatchSmokeMaterialTableRow(candidate, row, id, info, record, true,
                RT_SMOKE_TEXTURE_DESCRIPTOR_CAPACITY, work)) return false;
        sourceSignatures[row] = signature;
        ++patched;
    }
    OPTICK_TAG("materialRowsChecked", static_cast<uint32_t>(sourceSignatures.size()));
    OPTICK_TAG("materialRowsPatched", patched);
    textureCapacityMisses = work.textureCapacityMisses;
    return ValidateSmokeMaterialIndexes(candidate);
}

void RecomputeSmokeMaterialTableDiagnostics(RtSmokeMaterialTableBuild& table)
{
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
    table.materialsAdditiveDecals = 0;
    table.guiTextureCandidates = 0;
    table.guiTexturesAccepted = 0;
    table.guiTexturesRejected = 0;

    const int materialCount = Min(
        static_cast<int>(table.materials.size()),
        Min(static_cast<int>(table.materialInfos.size()),
            static_cast<int>(table.materialFacts.size())));
    std::vector<bool> activeRows;
    if (!BuildSmokeMaterialActiveRowMask(table, activeRows))
    {
        return;
    }
    for (int tableIndex = 0; tableIndex < materialCount; ++tableIndex)
    {
        if (tableIndex >= static_cast<int>(activeRows.size()) ||
            !activeRows[tableIndex])
        {
            continue;
        }
        const PathTraceSmokeMaterial& material = table.materials[tableIndex];
        const RtSmokeMaterialTextureInfo& info = table.materialInfos[tableIndex];
        const RtSmokeMaterialUniverseFacts& facts = table.materialFacts[tableIndex];
        table.materialsWithTextures +=
            material.diffuseTextureIndex != UINT32_MAX ? 1 : 0;
        table.materialsWithAlphaTextures +=
            material.alphaTextureIndex != UINT32_MAX ? 1 : 0;
        table.materialsWithNormalTextures +=
            material.normalTextureIndex != UINT32_MAX ? 1 : 0;
        table.materialsWithSpecularTextures +=
            material.specularTextureIndex != UINT32_MAX ? 1 : 0;
        table.materialsWithEmissiveTextures +=
            material.emissiveTextureIndex != UINT32_MAX ? 1 : 0;
        table.materialsEmissive += facts.emissive ? 1 : 0;
        table.materialsAlphaTested += facts.alphaTested ? 1 : 0;
        table.materialsAdditiveDecals +=
            (material.flags & RT_SMOKE_MATERIAL_ADDITIVE_DECAL) != 0 ? 1 : 0;
        table.materialsMissingTextures +=
            !facts.hasDiffuseImage || !info.hasTextureHandle ? 1 : 0;
        table.materialsRejectedTextures +=
            facts.hasDiffuseImage && !facts.hasSafeDiffuseTexture ? 1 : 0;
        if (facts.guiTextureCandidate)
        {
            AccumulateSmokeGuiTextureDiagnostic(
                table, info.diffuseImageName, facts.hasSafeDiffuseTexture);
            AccumulateSmokeGuiTextureDiagnostic(
                table, info.alphaImageName, facts.hasSafeAlphaTexture);
            AccumulateSmokeGuiTextureDiagnostic(
                table, info.normalImageName, facts.hasSafeNormalTexture);
            AccumulateSmokeGuiTextureDiagnostic(
                table, info.specularImageName, facts.hasSafeSpecularTexture);
            AccumulateSmokeGuiTextureDiagnostic(
                table, info.emissiveImageName, facts.hasSafeEmissiveTexture);
        }
    }
}

void PopulateSmokeOrderedStageTextureSlots(
    RtSmokeMaterialTableBuild& table,
    int textureTableLimit,
    const std::vector<bool>& activeRows)
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
        if (materialIndex >= static_cast<int>(activeRows.size()) ||
            !activeRows[materialIndex])
        {
            continue;
        }
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
            const idPathTracingTextureBindingSnapshot binding = image
                ? image->GetPathTracingTextureBindingSnapshot()
                : idPathTracingTextureBindingSnapshot();
            const idImageOpts* imageOpts = image ? &image->GetOpts() : nullptr;
            const bool guiTextureOverride = image &&
                r_pathTracingAllowGuiTextures.GetInteger() != 0 &&
                IsSmokeImageNameGuiLike(image->GetName());
            const bool safeImage = image && imageOpts &&
                imageOpts->samples == 1 && imageOpts->textureType == DTT_2D &&
                (!(imageOpts->isRenderTarget || imageOpts->isUAV) || guiTextureOverride) &&
                (IsSmokeImageNameSafeForRayTracing(image->GetName()) || guiTextureOverride) &&
                IsSmokeTextureHandleSafeForDescriptor(binding.texture);
            if (!safeImage)
            {
                continue;
            }
            const nvrhi::TextureHandle texture = binding.texture;
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
                parameters.orderedStageTextureWords[stageSlot] =
                    BuildSmokeOrderedStageTextureWord(descriptorIndex);
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
    OPTICK_EVENT("PT Material Table Populate Slots");

    const int populateStartMs = Sys_Milliseconds();
    const int materialTableCount = Min(static_cast<int>(table.materialIds.size()), static_cast<int>(table.materials.size()));
    std::vector<bool> activeRows;
    BuildSmokeMaterialActiveRowMask(table, activeRows);

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
        if (tableIndex < static_cast<int>(table.materialFeatureParameters.size()))
        {
            for (uint32_t& textureWord :
                table.materialFeatureParameters[tableIndex].orderedStageTextureWords)
            {
                textureWord = 0u;
            }
        }
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
        if (tableIndex >= static_cast<int>(activeRows.size()) ||
            !activeRows[tableIndex])
        {
            continue;
        }
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
    const std::vector<int> safeMaterialIndexes =
        BuildSmokeSafeMaterialIndexOrder(table, materialInfos, activeRows);
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
    PopulateSmokeOrderedStageTextureSlots(
        table,
        textureTableLimit,
        activeRows);
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
    {
        OPTICK_EVENT("PT Material Table Universe Entries");
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
    }

    PopulateSmokeMaterialTextureSlots(table, latchedTextureProbeMaterialId, latchedTextureProbeRequestedIndex, enableTextureProbe, minimumTextureTableLimit);
}

void BuildSmokeMaterialTableFromUniverseStableSlots(
    RtSmokeMaterialTableBuild& table,
    const RtSmokeMaterialStableSlotPlan& plan,
    uint32_t& latchedTextureProbeMaterialId,
    int& latchedTextureProbeRequestedIndex,
    bool enableTextureProbe,
    int minimumTextureTableLimit)
{
    g_smokeMaterialTableBuildStats = RtSmokeMaterialTableBuildStats();
    ++g_smokeMaterialTableBuildStats.buildCalls;
    {
        OPTICK_EVENT("PT Material Table Universe Entries");
        table = RtSmokeMaterialTableBuild();
        ReserveSmokeMaterialUniverse(plan.slotMaterialIds.size() + 64);
        table.materialIds.reserve(plan.slotMaterialIds.size());
        table.materials.reserve(plan.slotMaterialIds.size());
        table.materialInfos.reserve(plan.slotMaterialIds.size());
        table.materialFacts.reserve(plan.slotMaterialIds.size());
        table.materialFeatures.reserve(plan.slotMaterialIds.size());
        table.materialFeatureParameters.reserve(plan.slotMaterialIds.size());

        const int entryStartMs = Sys_Milliseconds();
        for (uint32_t materialId : plan.slotMaterialIds)
        {
            const uint32_t tableIndex = AddSmokeMaterialTableEntry(table, materialId);
            if (tableIndex + 1u != table.materialIds.size())
            {
                table = RtSmokeMaterialTableBuild();
                return;
            }
        }
        table.staticMaterialIndexes = plan.staticMaterialIndexes;
        table.dynamicMaterialIndexes = plan.dynamicMaterialIndexes;
        g_smokeMaterialTableBuildStats.entryMs +=
            Sys_Milliseconds() - entryStartMs;
    }
    PopulateSmokeMaterialTextureSlots(
        table,
        latchedTextureProbeMaterialId,
        latchedTextureProbeRequestedIndex,
        enableTextureProbe,
        minimumTextureTableLimit);
}

bool BuildSmokeMaterialTableCacheRowState(
    const RtSmokeMaterialTableBuild& table,
    std::unordered_map<uint32_t, uint32_t>& materialIndexLookup,
    std::vector<uint64>& rowSourceSignatures,
    std::vector<uint8_t>& hasOrderedStageSources)
{
    if (!ValidateSmokeMaterialIndexes(table))
    {
        return false;
    }
    materialIndexLookup.clear();
    materialIndexLookup.reserve(table.materialIds.size());
    rowSourceSignatures.clear();
    rowSourceSignatures.reserve(table.materialIds.size());
    hasOrderedStageSources.clear();
    hasOrderedStageSources.reserve(table.materialIds.size());
    for (int tableIndex = 0;
        tableIndex < static_cast<int>(table.materialIds.size());
        ++tableIndex)
    {
        const uint32_t materialId = table.materialIds[tableIndex];
        if (!materialIndexLookup.emplace(
                materialId,
                static_cast<uint32_t>(tableIndex)).second)
        {
            return false;
        }
        const RtSmokeMaterialTextureInfo& info = table.materialInfos[tableIndex];
        const RtSmokePersistentMaterialRecord& record =
            GetSmokePersistentMaterialRecord(materialId, info);
        const RtMaterialRecord* classifierRecord =
            FindPathTraceMaterialRecord(materialId);
        if (classifierRecord && classifierRecord->materialId != materialId)
        {
            return false;
        }
        hasOrderedStageSources.push_back(
            SmokeMaterialRecordHasOrderedStageSources(classifierRecord) ? 1u : 0u);
        rowSourceSignatures.push_back(
            ComputeSmokeMaterialRowSourceSignature(
                materialId,
                info,
                record,
                classifierRecord));
    }
    return true;
}

bool SmokeMaterialBindingEpochCanCommit(uint64 entryEpoch, uint64 postEpoch);
bool SmokeMaterialBindingEpochDriftShouldRetry(int completedAttempts);

enum class RtSmokeMaterialEpochDriftPhase
{
    Patch,
    FullBuild
};

struct RtSmokeMaterialEpochInvocationState
{
    bool epochFailClosed = false;
    int patchDrifts = 0;
    int fullBuildDrifts = 0;
    bool forcedActiveRefreshCompleted = false;

    bool NoteEpochDrift(RtSmokeMaterialEpochDriftPhase phase)
    {
        int& driftCount = phase == RtSmokeMaterialEpochDriftPhase::Patch
            ? patchDrifts
            : fullBuildDrifts;
        ++driftCount;
        if (driftCount >= 2)
        {
            epochFailClosed = true;
        }
        return !epochFailClosed;
    }

    void BeginFullBuildAttempt()
    {
        forcedActiveRefreshCompleted = false;
    }

    void NoteForcedActiveRefresh()
    {
        forcedActiveRefreshCompleted = true;
    }

    bool CanStorePatch() const
    {
        return !epochFailClosed;
    }

    bool CanStoreFullBuild(bool forcedRefreshRequired) const
    {
        return !epochFailClosed &&
            (!forcedRefreshRequired || forcedActiveRefreshCompleted);
    }
};

bool StoreSmokeMaterialTableCache(
    const RtSmokeMaterialTableBuild& table,
    uint64 signature,
    uint64 structuralSignature,
    uint64 materialIdSetSignature,
    uint64 staticMaterialIdSequenceSignature,
    uint64 dynamicMaterialIdSequenceSignature,
    const RtSmokeMaterialTableSignatureBreakdown& signatureBreakdown,
    uint64 orderedStageBindingEntryEpoch,
    bool& orderedStageBindingEpochDrifted)
{
    orderedStageBindingEpochDrifted = false;
    std::unordered_map<uint32_t, uint32_t> materialIndexLookup;
    std::vector<uint64> rowSourceSignatures;
    std::vector<uint8_t> hasOrderedStageSources;
    if (!BuildSmokeMaterialTableCacheRowState(
            table,
            materialIndexLookup,
            rowSourceSignatures,
            hasOrderedStageSources))
    {
        return false;
    }
    if (!SmokeMaterialBindingEpochCanCommit(
            orderedStageBindingEntryEpoch,
            GetPathTracingImageBindingEpoch()))
    {
        orderedStageBindingEpochDrifted = true;
        return false;
    }

    g_smokeMaterialTableCache.signature = signature;
    g_smokeMaterialTableCache.structuralSignature = structuralSignature;
    g_smokeMaterialTableCache.observedRegistryGeneration =
        signatureBreakdown.registryGeneration;
    g_smokeMaterialTableCache.observedResidentFactsGeneration =
        signatureBreakdown.residentFactsGeneration;
    g_smokeMaterialTableCache.observedOverrideGeneration =
        signatureBreakdown.overrideGeneration;
    g_smokeMaterialTableCache.observedOrderedStageBindingEpoch =
        orderedStageBindingEntryEpoch;
    g_smokeMaterialTableCache.materialIdSetSignatureValid = true;
    g_smokeMaterialTableCache.materialIdSetSignature = materialIdSetSignature;
    g_smokeMaterialTableCache.staticMaterialIdSequenceSignature =
        staticMaterialIdSequenceSignature;
    g_smokeMaterialTableCache.dynamicMaterialIdSequenceSignature =
        dynamicMaterialIdSequenceSignature;
    g_smokeMaterialTableCache.signatureBreakdown = signatureBreakdown;
    g_smokeMaterialTableCache.table = table;
    g_smokeMaterialTableCache.materialIndexLookup.swap(materialIndexLookup);
    g_smokeMaterialTableCache.rowSourceSignatures.swap(rowSourceSignatures);
    g_smokeMaterialTableCache.hasOrderedStageSources.swap(
        hasOrderedStageSources);
    g_smokeMaterialTableCache.valid = true;
    return true;
}

enum class RtSmokeMaterialDirtyRowPatchFailure
{
    None,
    Validation,
    Capacity,
    Patch
};

bool SmokeMaterialBindingEpochCanCommit(uint64 entryEpoch, uint64 postEpoch)
{
    return entryEpoch == postEpoch;
}

bool SmokeMaterialBindingEpochDriftShouldRetry(int completedAttempts)
{
    return completedAttempts == 1;
}

bool SmokeMaterialPatchCanCommit(
    bool patchSucceeded,
    bool finalStructuralCompatible,
    bool tableValid,
    bool rowStateValid)
{
    return patchSucceeded && finalStructuralCompatible &&
        tableValid && rowStateValid;
}

bool SmokeMaterialNonPrimarySourcesChanged(
    const RtSmokeMaterialTableCache& cache,
    const RtSmokeMaterialTableSignatureBreakdown& current)
{
    return cache.observedResidentFactsGeneration !=
            current.residentFactsGeneration ||
        cache.observedOverrideGeneration != current.overrideGeneration;
}

bool SmokeMaterialRowNeedsConsideration(
    bool active,
    bool primaryBindingChanged,
    bool nonPrimarySourcesChanged,
    bool cachedDynamic,
    bool appended,
    bool newlyActive,
    bool hasOrderedStageSources,
    bool orderedStageBindingEpochChanged)
{
    return active && (primaryBindingChanged || nonPrimarySourcesChanged ||
        cachedDynamic ||
        appended || newlyActive ||
        (hasOrderedStageSources && orderedStageBindingEpochChanged));
}

bool SmokeMaterialRowFingerprintNeedsRebuild(
    bool dynamicRow,
    bool appended,
    bool newlyActive,
    uint64 cachedSignature,
    uint64 currentSignature)
{
    return dynamicRow || appended || newlyActive ||
        cachedSignature != currentSignature;
}

bool PatchSmokeMaterialTableCache(
    RtSmokeMaterialTableBuild& table,
    const RtSmokeMaterialStableSlotPlan& plan,
    bool enableTextureProbe,
    int minimumTextureTableLimit,
    bool dynamicPartitionMeasurementEnabled,
    uint64 orderedStageBindingEntryEpoch,
    bool nonPrimarySourcesChanged,
    bool overrideSourcesChanged,
    bool residentFactsSourcesChanged,
    const std::unordered_set<uint32_t>& changedMaterialIds,
    RtSmokeMaterialDirtyRowWork& work,
    std::vector<uint64>& rowSourceSignatures,
    std::vector<uint8_t>& hasOrderedStageSources,
    RtSmokeMaterialDirtyRowPatchFailure& failure)
{
    OPTICK_EVENT("PT Material Table Dirty Rows");
    const uint64 patchStartMicroseconds = Sys_Microseconds();
    failure = RtSmokeMaterialDirtyRowPatchFailure::None;
    const uint64 tableCopyStartMicroseconds = Sys_Microseconds();
    {
        OPTICK_EVENT("PT Material Table Dirty Rows - Table Copy");
        table = g_smokeMaterialTableCache.table;
        rowSourceSignatures = g_smokeMaterialTableCache.rowSourceSignatures;
        hasOrderedStageSources =
            g_smokeMaterialTableCache.hasOrderedStageSources;
    }
    work.tableCopyMicroseconds =
        Sys_Microseconds() - tableCopyStartMicroseconds;
    if (!ValidateSmokeMaterialIndexes(table) ||
        g_smokeMaterialTableCache.materialIndexLookup.size() !=
            table.materialIds.size() ||
        rowSourceSignatures.size() != table.materialIds.size() ||
        hasOrderedStageSources.size() != table.materialIds.size() ||
        plan.slotMaterialIds.size() < table.materialIds.size())
    {
        failure = RtSmokeMaterialDirtyRowPatchFailure::Validation;
        return false;
    }

    for (int slotIndex = static_cast<int>(table.materialIds.size());
        slotIndex < static_cast<int>(plan.slotMaterialIds.size());
        ++slotIndex)
    {
        const uint32_t materialId = plan.slotMaterialIds[slotIndex];
        const uint32_t appendedIndex = AddSmokeMaterialTableEntry(table, materialId);
        if (appendedIndex != static_cast<uint32_t>(slotIndex))
        {
            failure = RtSmokeMaterialDirtyRowPatchFailure::Validation;
            return false;
        }
        const RtSmokeMaterialTextureInfo& info = table.materialInfos[slotIndex];
        const RtSmokePersistentMaterialRecord& record =
            GetSmokePersistentMaterialRecord(materialId, info);
        const uint64 lookupStartMicroseconds = Sys_Microseconds();
        const RtMaterialRecord* classifierRecord =
            FindPathTraceMaterialRecord(materialId);
        work.classifierLookupMicroseconds +=
            Sys_Microseconds() - lookupStartMicroseconds;
        ++work.classifierFinds;
        if (classifierRecord && classifierRecord->materialId != materialId)
        {
            failure = RtSmokeMaterialDirtyRowPatchFailure::Validation;
            return false;
        }
        hasOrderedStageSources.push_back(
            SmokeMaterialRecordHasOrderedStageSources(classifierRecord) ? 1u : 0u);
        const uint64 fingerprintStartMicroseconds = Sys_Microseconds();
        rowSourceSignatures.push_back(
            ComputeSmokeMaterialRowSourceSignature(
                materialId,
                info,
                record,
                classifierRecord));
        work.orderedFingerprintMicroseconds +=
            Sys_Microseconds() - fingerprintStartMicroseconds;
    }
    std::vector<bool> previouslyActiveRows(table.materialIds.size(), false);
    for (uint32_t tableIndex : table.staticMaterialIndexes)
    {
        if (tableIndex >= previouslyActiveRows.size())
        {
            failure = RtSmokeMaterialDirtyRowPatchFailure::Validation;
            return false;
        }
        previouslyActiveRows[tableIndex] = true;
    }
    for (uint32_t tableIndex : table.dynamicMaterialIndexes)
    {
        if (tableIndex >= previouslyActiveRows.size())
        {
            failure = RtSmokeMaterialDirtyRowPatchFailure::Validation;
            return false;
        }
        previouslyActiveRows[tableIndex] = true;
    }
    table.staticMaterialIndexes = plan.staticMaterialIndexes;
    table.dynamicMaterialIndexes = plan.dynamicMaterialIndexes;

    std::vector<bool> activeRows;
    std::vector<bool> dynamicRows(table.materialIds.size(), false);
    if (!BuildSmokeMaterialActiveRowMask(table, activeRows))
    {
        failure = RtSmokeMaterialDirtyRowPatchFailure::Validation;
        return false;
    }
    for (uint32_t tableIndex : table.dynamicMaterialIndexes)
    {
        dynamicRows[tableIndex] = true;
    }
    work.activeRows = static_cast<int>(
        std::count(activeRows.begin(), activeRows.end(), true));
    const uint64 textureRefcountStartMicroseconds = Sys_Microseconds();
    bool textureRefcountValid = false;
    {
        OPTICK_EVENT("PT Material Table Dirty Rows - Texture Refcount");
        textureRefcountValid =
            ResetSmokeInactiveMaterialBindings(table, activeRows) &&
            (!enableTextureProbe ||
                InitializeSmokeMaterialPatchTextureReferences(
                    table,
                    activeRows,
                    work));
    }
    work.textureRefcountMicroseconds =
        Sys_Microseconds() - textureRefcountStartMicroseconds;
    if (!textureRefcountValid)
    {
        failure = RtSmokeMaterialDirtyRowPatchFailure::Validation;
        return false;
    }

    const bool orderedStageBindingEpochChanged =
        g_smokeMaterialTableCache.observedOrderedStageBindingEpoch !=
            orderedStageBindingEntryEpoch;
    const int firstAppendedIndex =
        static_cast<int>(g_smokeMaterialTableCache.table.materialIds.size());
    std::vector<RtSmokeMaterialDirtyRowCandidate> candidates;
    candidates.reserve(table.materialIds.size());
    for (int tableIndex = 0;
        tableIndex < static_cast<int>(table.materialIds.size());
        ++tableIndex)
    {
        RtSmokeMaterialDirtyRowCandidate candidate;
        candidate.tableIndex = tableIndex;
        candidate.materialId = table.materialIds[tableIndex];
        const bool appended = tableIndex >= firstAppendedIndex;
        const bool cachedDynamicRow = dynamicRows[tableIndex] ||
            table.materialInfos[tableIndex].isDynamic ||
            table.materialFacts[tableIndex].isDynamic;
        const bool newlyActive = activeRows[tableIndex] &&
            !previouslyActiveRows[tableIndex];
        const bool orderedStageRow = activeRows[tableIndex] &&
            hasOrderedStageSources[tableIndex] != 0u;
        const bool primaryBindingChanged =
            changedMaterialIds.find(candidate.materialId) !=
                changedMaterialIds.end();
        work.orderedStageEpochEligibleRows += orderedStageRow ? 1 : 0;
        work.orderedStageScannedDueEpochRows +=
            orderedStageRow && orderedStageBindingEpochChanged ? 1 : 0;
        work.orderedStageAvoidedSteadyRows +=
            orderedStageRow && !orderedStageBindingEpochChanged ? 1 : 0;
        work.nonPrimaryGenerationWakeRows +=
            activeRows[tableIndex] && nonPrimarySourcesChanged ? 1 : 0;
        if (!SmokeMaterialRowNeedsConsideration(
                activeRows[tableIndex],
                primaryBindingChanged,
                nonPrimarySourcesChanged,
                cachedDynamicRow,
                appended,
                newlyActive,
                orderedStageRow,
                orderedStageBindingEpochChanged))
        {
            continue;
        }

        candidate.appended = appended;
        candidate.cachedDynamic = cachedDynamicRow;
        candidate.newlyActive = newlyActive;
        candidate.orderedStage = orderedStageRow;
        candidate.wakeMask =
            (appended ? RT_SMOKE_DYNAMIC_WAKE_APPEND : 0u) |
            (newlyActive ? RT_SMOKE_DYNAMIC_WAKE_NEW : 0u) |
            (primaryBindingChanged ? RT_SMOKE_DYNAMIC_WAKE_BINDING : 0u) |
            (orderedStageRow && orderedStageBindingEpochChanged
                ? RT_SMOKE_DYNAMIC_WAKE_ORDERED
                : 0u) |
            (overrideSourcesChanged ? RT_SMOKE_DYNAMIC_WAKE_OVERRIDE : 0u) |
            (residentFactsSourcesChanged
                ? RT_SMOKE_DYNAMIC_WAKE_FACTS
                : 0u);
        candidates.push_back(candidate);
        ++work.consideredRows;
        work.orderedStageConsideredRows += orderedStageRow ? 1 : 0;
    }

    const uint64 rowResolveStartMicroseconds = Sys_Microseconds();
    {
        OPTICK_EVENT("PT Material Table Dirty Rows - Row Resolve");
        for (RtSmokeMaterialDirtyRowCandidate& candidate : candidates)
        {
            candidate.info = ResolveSmokeMaterialTextureInfo(
                candidate.materialId,
                candidate.tableIndex);
            candidate.persistentRecord = &GetSmokePersistentMaterialRecord(
                candidate.materialId,
                candidate.info);
            candidate.dynamic = candidate.cachedDynamic ||
                candidate.info.isDynamic ||
                candidate.persistentRecord->facts.isDynamic;
            if (candidate.dynamic)
            {
                candidate.wakeMask |= RT_SMOKE_DYNAMIC_WAKE_DYNAMIC;
                if (dynamicPartitionMeasurementEnabled)
                {
                    const idMaterial* material = declManager
                        ? declManager->FindMaterial(
                            candidate.info.materialName.c_str(),
                            false)
                        : nullptr;
                    candidate.dynamicSubclassMask =
                        BuildSmokeMaterialDynamicSubclassMask(
                            material,
                            candidate.info);
                }
            }
        }
    }
    work.rowResolveMicroseconds =
        Sys_Microseconds() - rowResolveStartMicroseconds;

    const uint64 classifierLookupStartMicroseconds = Sys_Microseconds();
    bool classifierMembershipValid = true;
    {
        OPTICK_EVENT("PT Material Table Dirty Rows - Classifier Lookup");
        for (RtSmokeMaterialDirtyRowCandidate& candidate : candidates)
        {
            candidate.classifierRecord =
                FindPathTraceMaterialRecord(candidate.materialId);
            ++work.classifierFinds;
            const bool recordHasOrderedStages =
                SmokeMaterialRecordHasOrderedStageSources(
                    candidate.classifierRecord);
            if ((candidate.classifierRecord &&
                    candidate.classifierRecord->materialId !=
                        candidate.materialId) ||
                recordHasOrderedStages !=
                    (hasOrderedStageSources[candidate.tableIndex] != 0u))
            {
                classifierMembershipValid = false;
                break;
            }
        }
    }
    work.classifierLookupMicroseconds +=
        Sys_Microseconds() - classifierLookupStartMicroseconds;
    if (!classifierMembershipValid)
    {
        failure = RtSmokeMaterialDirtyRowPatchFailure::Validation;
        return false;
    }

    const uint64 fingerprintStartMicroseconds = Sys_Microseconds();
    {
        OPTICK_EVENT("PT Material Table Dirty Rows - Ordered Fingerprint");
        for (RtSmokeMaterialDirtyRowCandidate& candidate : candidates)
        {
            candidate.rowSourceSignature =
                ComputeSmokeMaterialRowSourceSignature(
                    candidate.materialId,
                    candidate.info,
                    *candidate.persistentRecord,
                    candidate.classifierRecord);
            candidate.rebuild = SmokeMaterialRowFingerprintNeedsRebuild(
                candidate.dynamic,
                candidate.appended,
                candidate.newlyActive,
                rowSourceSignatures[candidate.tableIndex],
                candidate.rowSourceSignature);
            if (!candidate.dynamic &&
                rowSourceSignatures[candidate.tableIndex] !=
                    candidate.rowSourceSignature)
            {
                candidate.wakeMask |= RT_SMOKE_DYNAMIC_WAKE_SIGNATURE;
            }
            if (candidate.dynamic)
            {
                ++work.dynamicRows;
            }
            if (!candidate.rebuild)
            {
                ++work.skippedUnchangedRows;
                work.orderedStageSkippedRows +=
                    candidate.orderedStage ? 1 : 0;
            }
        }
    }
    work.orderedFingerprintMicroseconds +=
        Sys_Microseconds() - fingerprintStartMicroseconds;

    const uint64 rowRebuildStartMicroseconds = Sys_Microseconds();
    {
        OPTICK_EVENT("PT Material Table Dirty Rows - Row Rebuild");
        for (RtSmokeMaterialDirtyRowCandidate& candidate : candidates)
        {
            if (!candidate.rebuild)
            {
                continue;
            }
            if (enableTextureProbe &&
                Max(0, r_pathTracingTextureTableStart.GetInteger()) != 0)
            {
                failure = RtSmokeMaterialDirtyRowPatchFailure::Patch;
                return false;
            }
            RtSmokeMaterialRowProductSnapshot beforeProducts;
            const bool beforeProductsValid =
                dynamicPartitionMeasurementEnabled &&
                CaptureSmokeMaterialRowProductSnapshot(
                    table,
                    candidate.tableIndex,
                    beforeProducts);
            if (enableTextureProbe &&
                !ReleaseSmokeMaterialPatchTextureReferencesForRow(
                    table,
                    candidate.tableIndex,
                    work))
            {
                failure = RtSmokeMaterialDirtyRowPatchFailure::Validation;
                return false;
            }
            if (!PatchSmokeMaterialTableRow(
                    table,
                    candidate.tableIndex,
                    candidate.materialId,
                    candidate.info,
                    *candidate.persistentRecord,
                    enableTextureProbe,
                    minimumTextureTableLimit,
                    work))
            {
                failure = RtSmokeMaterialDirtyRowPatchFailure::Capacity;
                return false;
            }
            if (dynamicPartitionMeasurementEnabled)
            {
                RtSmokeMaterialRowProductSnapshot afterProducts;
                const bool afterProductsValid =
                    CaptureSmokeMaterialRowProductSnapshot(
                        table,
                        candidate.tableIndex,
                        afterProducts);
                RtSmokeMaterialRowProductDelta productDelta;
                if (!beforeProductsValid || !afterProductsValid)
                {
                    productDelta.comparisonError = true;
                }
                else
                {
                    productDelta = CompareSmokeMaterialRowProducts(
                        beforeProducts,
                        afterProducts);
                }
                RtSmokeMaterialTableCacheStats::DynamicPartition& partition =
                    work.dynamicPartition;
                ++partition.rebuilt;
                if (productDelta.comparisonError)
                {
                    ++partition.compareError;
                }
                else if (productDelta.Any())
                {
                    ++partition.comparePartial;
                }
                else
                {
                    ++partition.compareIdentical;
                }
                partition.productRow += productDelta.row ? 1 : 0;
                partition.productFacts += productDelta.facts ? 1 : 0;
                partition.productFeature += productDelta.feature ? 1 : 0;
                partition.productParameters += productDelta.parameters ? 1 : 0;
                partition.productBinding += productDelta.binding ? 1 : 0;
                RecordSmokeMaterialDynamicWakeCounters(
                    partition,
                    candidate.wakeMask);
                if (candidate.dynamic)
                {
                    ++partition.dynamicRebuilt;
                    RecordSmokeMaterialDynamicSubclassCounters(
                        partition,
                        candidate.dynamicSubclassMask);
                    if (!productDelta.comparisonError && !productDelta.Any())
                    {
                        ++partition.identicalDynamic;
                        if ((candidate.dynamicSubclassMask &
                                (RT_SMOKE_DYNAMIC_SUBCLASS_REGISTERS |
                                    RT_SMOKE_DYNAMIC_SUBCLASS_TEXTURE_MATRIX)) != 0u)
                        {
                            // Hypothesis only: runtime register/matrix products may be
                            // consumed elsewhere even when this table row is identical.
                            ++partition.runtimeCoveredCandidate;
                        }
                    }
                }
            }
            rowSourceSignatures[candidate.tableIndex] =
                candidate.rowSourceSignature;
            ++work.rebuiltRows;
            work.orderedStageRebuiltRows +=
                candidate.orderedStage ? 1 : 0;
        }
        if (dynamicPartitionMeasurementEnabled)
        {
            const RtSmokeMaterialTableCacheStats::DynamicPartition& partition =
                work.dynamicPartition;
            const uint64 exclusiveTotal =
                partition.exclusiveDynamic + partition.exclusiveAppend +
                partition.exclusiveNew + partition.exclusiveSignature +
                partition.exclusiveBinding + partition.exclusiveOrdered +
                partition.exclusiveOverride + partition.exclusiveFacts +
                partition.exclusiveOther;
            if (partition.rebuilt != partition.compareIdentical +
                    partition.comparePartial + partition.compareError ||
                partition.rebuilt != exclusiveTotal)
            {
                ++work.dynamicPartition.invariantFailures;
            }
        }
    }
    work.rowRebuildMicroseconds =
        Sys_Microseconds() - rowRebuildStartMicroseconds;

    RecomputeSmokeMaterialTableDiagnostics(table);
    if (!ValidateSmokeMaterialIndexes(table))
    {
        failure = RtSmokeMaterialDirtyRowPatchFailure::Validation;
        return false;
    }
    work.totalMicroseconds = Sys_Microseconds() - patchStartMicroseconds;
    return true;
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
    const bool telemetryEnabled = SmokeMaterialTableCacheTelemetryEnabled();
    const uint64 telemetryStartMicroseconds = telemetryEnabled ? Sys_Microseconds() : 0;
    uint64 staticMaterialIdSequenceSignature = 0;
    uint64 dynamicMaterialIdSequenceSignature = 0;
    uint64 materialIdSetSignature = 0;
    {
        OPTICK_EVENT("PT Material Table Signature Cache");
        staticMaterialIdSequenceSignature = ComputeSmokeMaterialIdSequenceSignature(staticMaterialIds);
        dynamicMaterialIdSequenceSignature = ComputeSmokeMaterialIdSequenceSignature(dynamicMaterialIds);
        const bool reuseMaterialIdSetSignature =
            g_smokeMaterialTableCache.valid &&
            g_smokeMaterialTableCache.materialIdSetSignatureValid &&
            g_smokeMaterialTableCache.staticMaterialIdSequenceSignature == staticMaterialIdSequenceSignature &&
            g_smokeMaterialTableCache.dynamicMaterialIdSequenceSignature == dynamicMaterialIdSequenceSignature;
        materialIdSetSignature = reuseMaterialIdSetSignature
            ? g_smokeMaterialTableCache.materialIdSetSignature
            : ComputeSmokeMaterialIdSetSignature(staticMaterialIds, dynamicMaterialIds);
    }
    cacheHit = false;
    const bool residencyDirtyRowsEnabled =
        r_pathTracingMaterialCache.GetInteger() != 0 &&
        r_pathTracingResidency.GetInteger() != 0 &&
        r_pathTracingResidencyMaterial.GetInteger() != 0;
    uint64 orderedStageBindingEntryEpoch =
        GetPathTracingImageBindingEpoch();
    RtSmokeMaterialEpochInvocationState epochState;
    std::unordered_set<uint32_t> changedMaterialIds;
    bool epochRefreshTriggeredThisCall = false;
    const auto refreshActiveBindingsForEpoch =
        [&](uint64 entryEpoch, bool retry, bool forceFullBuildRefresh) {
            if (!residencyDirtyRowsEnabled)
            {
                return false;
            }
            const bool cacheWasValid = g_smokeMaterialTableCache.valid;
            const bool epochTriggered = cacheWasValid &&
                entryEpoch !=
                    g_smokeMaterialTableCache.observedOrderedStageBindingEpoch;
            if (!forceFullBuildRefresh &&
                !epochTriggered)
            {
                return false;
            }
            const RtSmokeMaterialActiveTextureRefreshResult refresh =
                RefreshSmokeMaterialTextureHandlesForActiveIds(
                    staticMaterialIds,
                    dynamicMaterialIds);
            if (forceFullBuildRefresh)
            {
                epochState.NoteForcedActiveRefresh();
                ++g_smokeMaterialTableCacheTelemetry.epochForcedFullRefreshCalls;
                g_smokeMaterialTableCacheTelemetry.epochForcedInvalidCacheRefreshCalls +=
                    cacheWasValid ? 0 : 1;
                g_smokeMaterialTableCacheTelemetry.epochForcedFullRefreshVisitedIds +=
                    refresh.visited;
                g_smokeMaterialTableCacheTelemetry.epochForcedFullRefreshChangedIds +=
                    refresh.changedMaterialIds.size();
            }
            if (epochTriggered)
            {
                ++g_smokeMaterialTableCacheTelemetry.epochTriggeredRefreshCalls;
                g_smokeMaterialTableCacheTelemetry.epochTriggeredRefreshVisitedIds +=
                    refresh.visited;
                g_smokeMaterialTableCacheTelemetry.epochTriggeredRefreshChangedIds +=
                    refresh.changedMaterialIds.size();
                g_smokeMaterialTableCacheTelemetry.epochTriggeredRefreshRetries +=
                    retry ? 1 : 0;
                epochRefreshTriggeredThisCall = true;
            }
            changedMaterialIds.insert(
                refresh.changedMaterialIds.begin(),
                refresh.changedMaterialIds.end());
            return true;
        };
    refreshActiveBindingsForEpoch(
        orderedStageBindingEntryEpoch,
        false,
        false);
    RtSmokeMaterialTableSignatureBreakdown signatureBreakdown;
    uint64 structuralSignature = 0;
    bool overrideSourcesChanged = false;
    bool residentFactsSourcesChanged = false;
    bool nonPrimarySourcesChanged = false;
    {
        OPTICK_EVENT("PT Material Table Signature Cache");
        signatureBreakdown = CaptureSmokeMaterialTableSignatureBreakdown(
            materialIdSetSignature,
            enableTextureProbe,
            minimumTextureTableLimit,
            latchedTextureProbeMaterialId,
            latchedTextureProbeRequestedIndex);
        structuralSignature =
            ComputeSmokeMaterialTableStructuralSignature(signatureBreakdown);
        overrideSourcesChanged =
            g_smokeMaterialTableCache.valid &&
            g_smokeMaterialTableCache.observedOverrideGeneration !=
                signatureBreakdown.overrideGeneration;
        residentFactsSourcesChanged =
            g_smokeMaterialTableCache.valid &&
            g_smokeMaterialTableCache.observedResidentFactsGeneration !=
                signatureBreakdown.residentFactsGeneration;
        nonPrimarySourcesChanged =
            overrideSourcesChanged || residentFactsSourcesChanged;
        signature = ComputeSmokeMaterialTableSignatureFromIdSet(materialIdSetSignature, enableTextureProbe, minimumTextureTableLimit, latchedTextureProbeMaterialId, latchedTextureProbeRequestedIndex);
    }
    static const bool stableSlotPlanSelfTestPassed =
        SmokeMaterialStableSlotPlanSelfTest();
    g_smokeMaterialTableCacheTelemetry.patchSelfTestEvaluated = true;
    g_smokeMaterialTableCacheTelemetry.patchSelfTestPassed =
        stableSlotPlanSelfTestPassed;
    RtSmokeMaterialStableSlotPlan stableSlotPlan;
    bool stableSlotPlanValid = false;
    {
        OPTICK_EVENT("PT Material Table Signature Cache");
        stableSlotPlanValid =
            g_smokeMaterialTableCache.valid &&
            BuildSmokeMaterialStableSlotPlan(
                g_smokeMaterialTableCache.table.materialIds,
                staticMaterialIds,
                dynamicMaterialIds,
                stableSlotPlan);
    }
    bool dirtyPatchForcedFull = false;
    bool preserveStableSlotsOnFullBuild = false;
    const auto buildFullTableForCurrentState = [&]() {
        if (residencyDirtyRowsEnabled &&
            preserveStableSlotsOnFullBuild &&
            stableSlotPlanValid)
        {
            BuildSmokeMaterialTableFromUniverseStableSlots(
                table,
                stableSlotPlan,
                latchedTextureProbeMaterialId,
                latchedTextureProbeRequestedIndex,
                enableTextureProbe,
                minimumTextureTableLimit);
        }
        else
        {
            BuildSmokeMaterialTableFromUniverse(
                table,
                staticMaterialIds,
                dynamicMaterialIds,
                latchedTextureProbeMaterialId,
                latchedTextureProbeRequestedIndex,
                enableTextureProbe,
                minimumTextureTableLimit);
        }
    };
    const auto buildEpochFailClosedCurrentFrame = [&]() {
        const uint64 failClosedEntryEpoch =
            GetPathTracingImageBindingEpoch();
        epochState.BeginFullBuildAttempt();
        refreshActiveBindingsForEpoch(
            failClosedEntryEpoch,
            true,
            true);
        buildFullTableForCurrentState();
        g_smokeMaterialTableCache.valid = false;
        ++g_smokeMaterialTableCacheTelemetry.epochFailClosedNoStore;
    };

    if (residencyDirtyRowsEnabled &&
        g_smokeMaterialTableCache.valid &&
        stableSlotPlanSelfTestPassed &&
        stableSlotPlanValid &&
        g_smokeMaterialTableCache.structuralSignature == structuralSignature)
    {
        RtSmokeMaterialDirtyRowWork dirtyWork;
        std::vector<uint64> rowSourceSignatures;
        std::vector<uint8_t> hasOrderedStageSources;
        RtSmokeMaterialDirtyRowPatchFailure patchFailure =
            RtSmokeMaterialDirtyRowPatchFailure::None;
        bool finalStructuralChanged = false;
        ++g_smokeMaterialTableCacheTelemetry.patchPathCalls;
        const bool generationOnlyAvoidedMiss =
            g_smokeMaterialTableCache.materialIdSetSignatureValid &&
            g_smokeMaterialTableCache.materialIdSetSignature == materialIdSetSignature &&
            g_smokeMaterialTableCache.observedOverrideGeneration ==
                signatureBreakdown.overrideGeneration &&
            (g_smokeMaterialTableCache.observedRegistryGeneration !=
                    signatureBreakdown.registryGeneration ||
                g_smokeMaterialTableCache.observedResidentFactsGeneration !=
                    signatureBreakdown.residentFactsGeneration);
        bool patchSucceeded = PatchSmokeMaterialTableCache(
                table,
                stableSlotPlan,
                enableTextureProbe,
                minimumTextureTableLimit,
                telemetryEnabled,
                orderedStageBindingEntryEpoch,
                nonPrimarySourcesChanged,
                overrideSourcesChanged,
                residentFactsSourcesChanged,
                changedMaterialIds,
                dirtyWork,
                rowSourceSignatures,
                hasOrderedStageSources,
                patchFailure);
        uint64 orderedStageBindingPostEpoch = patchSucceeded
            ? GetPathTracingImageBindingEpoch()
            : orderedStageBindingEntryEpoch;
        if (patchSucceeded &&
            !SmokeMaterialBindingEpochCanCommit(
                orderedStageBindingEntryEpoch,
                orderedStageBindingPostEpoch))
        {
            ++g_smokeMaterialTableCacheTelemetry.patchPathEpochRetries;
            epochState.NoteEpochDrift(
                RtSmokeMaterialEpochDriftPhase::Patch);
            dirtyWork = RtSmokeMaterialDirtyRowWork();
            rowSourceSignatures.clear();
            hasOrderedStageSources.clear();
            patchFailure = RtSmokeMaterialDirtyRowPatchFailure::None;
            orderedStageBindingEntryEpoch =
                GetPathTracingImageBindingEpoch();
            refreshActiveBindingsForEpoch(
                orderedStageBindingEntryEpoch,
                true,
                false);
            {
                OPTICK_EVENT("PT Material Table Signature Cache");
                signatureBreakdown = CaptureSmokeMaterialTableSignatureBreakdown(
                    materialIdSetSignature,
                    enableTextureProbe,
                    minimumTextureTableLimit,
                    latchedTextureProbeMaterialId,
                    latchedTextureProbeRequestedIndex);
                structuralSignature =
                    ComputeSmokeMaterialTableStructuralSignature(
                        signatureBreakdown);
                overrideSourcesChanged =
                    g_smokeMaterialTableCache.observedOverrideGeneration !=
                        signatureBreakdown.overrideGeneration;
                residentFactsSourcesChanged =
                    g_smokeMaterialTableCache.observedResidentFactsGeneration !=
                        signatureBreakdown.residentFactsGeneration;
                nonPrimarySourcesChanged =
                    overrideSourcesChanged || residentFactsSourcesChanged;
                signature = ComputeSmokeMaterialTableSignatureFromIdSet(
                    materialIdSetSignature,
                    enableTextureProbe,
                    minimumTextureTableLimit,
                    latchedTextureProbeMaterialId,
                    latchedTextureProbeRequestedIndex);
            }
            if (g_smokeMaterialTableCache.structuralSignature !=
                structuralSignature)
            {
                finalStructuralChanged = true;
                patchSucceeded = false;
                patchFailure = RtSmokeMaterialDirtyRowPatchFailure::Patch;
            }
            else
            {
                patchSucceeded = PatchSmokeMaterialTableCache(
                    table,
                    stableSlotPlan,
                    enableTextureProbe,
                    minimumTextureTableLimit,
                    telemetryEnabled,
                    orderedStageBindingEntryEpoch,
                    nonPrimarySourcesChanged,
                    overrideSourcesChanged,
                    residentFactsSourcesChanged,
                    changedMaterialIds,
                    dirtyWork,
                    rowSourceSignatures,
                    hasOrderedStageSources,
                    patchFailure);
            }
            orderedStageBindingPostEpoch = patchSucceeded
                ? GetPathTracingImageBindingEpoch()
                : orderedStageBindingEntryEpoch;
            if (patchSucceeded &&
                !SmokeMaterialBindingEpochCanCommit(
                    orderedStageBindingEntryEpoch,
                    orderedStageBindingPostEpoch))
            {
                ++g_smokeMaterialTableCacheTelemetry.patchPathEpochFailClosed;
                ++g_smokeMaterialTableCacheTelemetry.epochSecondDriftFailClosed;
                epochState.NoteEpochDrift(
                    RtSmokeMaterialEpochDriftPhase::Patch);
                patchSucceeded = false;
                patchFailure = RtSmokeMaterialDirtyRowPatchFailure::Patch;
            }
        }
        if (patchSucceeded)
        {
            const RtSmokeMaterialTableSignatureBreakdown finalSignatureBreakdown =
                CaptureSmokeMaterialTableSignatureBreakdown(
                    materialIdSetSignature,
                    enableTextureProbe,
                    minimumTextureTableLimit,
                    latchedTextureProbeMaterialId,
                    latchedTextureProbeRequestedIndex);
            const uint64 finalStructuralSignature =
                ComputeSmokeMaterialTableStructuralSignature(
                    finalSignatureBreakdown);
            if (finalStructuralSignature != structuralSignature)
            {
                finalStructuralChanged = true;
                patchFailure = RtSmokeMaterialDirtyRowPatchFailure::Patch;
            }
            else if (!SmokeMaterialPatchCanCommit(
                    true,
                    true,
                    ValidateSmokeMaterialIndexes(table),
                    rowSourceSignatures.size() == table.materialIds.size() &&
                        hasOrderedStageSources.size() ==
                            table.materialIds.size()))
            {
                patchFailure = RtSmokeMaterialDirtyRowPatchFailure::Validation;
            }
            else if (epochState.CanStorePatch())
            {
                signature = ComputeSmokeMaterialTableSignatureFromIdSet(
                    materialIdSetSignature,
                    enableTextureProbe,
                    minimumTextureTableLimit,
                    latchedTextureProbeMaterialId,
                    latchedTextureProbeRequestedIndex);
                std::unordered_map<uint32_t, uint32_t> materialIndexLookup;
                materialIndexLookup.reserve(table.materialIds.size());
                for (int tableIndex = 0;
                    tableIndex < static_cast<int>(table.materialIds.size());
                    ++tableIndex)
                {
                    materialIndexLookup.emplace(
                        table.materialIds[tableIndex],
                        static_cast<uint32_t>(tableIndex));
                }
                g_smokeMaterialTableCache.signature = signature;
                g_smokeMaterialTableCache.structuralSignature = finalStructuralSignature;
                g_smokeMaterialTableCache.observedRegistryGeneration =
                    finalSignatureBreakdown.registryGeneration;
                g_smokeMaterialTableCache.observedResidentFactsGeneration =
                    finalSignatureBreakdown.residentFactsGeneration;
                g_smokeMaterialTableCache.observedOverrideGeneration =
                    finalSignatureBreakdown.overrideGeneration;
                if (g_smokeMaterialTableCache.awaitingNextQuietEpochCacheHit &&
                    !epochRefreshTriggeredThisCall &&
                    orderedStageBindingEntryEpoch ==
                        g_smokeMaterialTableCache.observedOrderedStageBindingEpoch)
                {
                    ++g_smokeMaterialTableCacheTelemetry.epochNextQuietCacheHits;
                }
                g_smokeMaterialTableCache.awaitingNextQuietEpochCacheHit =
                    epochRefreshTriggeredThisCall;
                g_smokeMaterialTableCache.observedOrderedStageBindingEpoch =
                    orderedStageBindingEntryEpoch;
                g_smokeMaterialTableCache.materialIdSetSignatureValid = true;
                g_smokeMaterialTableCache.materialIdSetSignature = materialIdSetSignature;
                g_smokeMaterialTableCache.staticMaterialIdSequenceSignature =
                    staticMaterialIdSequenceSignature;
                g_smokeMaterialTableCache.dynamicMaterialIdSequenceSignature =
                    dynamicMaterialIdSequenceSignature;
                g_smokeMaterialTableCache.signatureBreakdown = finalSignatureBreakdown;
                g_smokeMaterialTableCache.table = table;
                g_smokeMaterialTableCache.materialIndexLookup.swap(materialIndexLookup);
                g_smokeMaterialTableCache.rowSourceSignatures.swap(rowSourceSignatures);
                g_smokeMaterialTableCache.hasOrderedStageSources.swap(
                    hasOrderedStageSources);
                g_smokeMaterialTableCache.valid = true;

                g_smokeMaterialTableCacheTelemetry.patchPathAppendedIds +=
                    stableSlotPlan.appendedIds;
                g_smokeMaterialTableCacheTelemetry.patchPathConsideredRows +=
                    dirtyWork.consideredRows;
                g_smokeMaterialTableCacheTelemetry.patchPathRebuiltRows +=
                    dirtyWork.rebuiltRows;
                g_smokeMaterialTableCacheTelemetry.patchPathSkippedUnchangedRows +=
                    dirtyWork.skippedUnchangedRows;
                g_smokeMaterialTableCacheTelemetry.patchPathDynamicRows +=
                    dirtyWork.dynamicRows;
                g_smokeMaterialTableCacheTelemetry.patchPathOrderedStageConsideredRows +=
                    dirtyWork.orderedStageConsideredRows;
                g_smokeMaterialTableCacheTelemetry.patchPathOrderedStageSkippedRows +=
                    dirtyWork.orderedStageSkippedRows;
                g_smokeMaterialTableCacheTelemetry.patchPathOrderedStageRebuiltRows +=
                    dirtyWork.orderedStageRebuiltRows;
                g_smokeMaterialTableCacheTelemetry.patchPathOrderedStageEpochEligibleRows +=
                    dirtyWork.orderedStageEpochEligibleRows;
                g_smokeMaterialTableCacheTelemetry.patchPathOrderedStageScannedDueEpochRows +=
                    dirtyWork.orderedStageScannedDueEpochRows;
                g_smokeMaterialTableCacheTelemetry.patchPathOrderedStageAvoidedSteadyRows +=
                    dirtyWork.orderedStageAvoidedSteadyRows;
                g_smokeMaterialTableCacheTelemetry.patchPathNonPrimaryGenerationWakeRows +=
                    dirtyWork.nonPrimaryGenerationWakeRows;
                AccumulateSmokeMaterialDynamicPartition(
                    g_smokeMaterialTableCacheTelemetry.dynamicPartition,
                    dirtyWork.dynamicPartition);
                ++g_smokeMaterialTableCacheTelemetry.patchPathCompletedCalls;
                g_smokeMaterialTableCacheTelemetry.patchPathActiveRows +=
                    dirtyWork.activeRows;
                g_smokeMaterialTableCacheTelemetry.patchPathClassifierFinds +=
                    dirtyWork.classifierFinds;
                g_smokeMaterialTableCacheTelemetry.patchPathTotalMicroseconds +=
                    dirtyWork.totalMicroseconds;
                g_smokeMaterialTableCacheTelemetry.patchPathTableCopyMicroseconds +=
                    dirtyWork.tableCopyMicroseconds;
                g_smokeMaterialTableCacheTelemetry.patchPathTextureRefcountMicroseconds +=
                    dirtyWork.textureRefcountMicroseconds;
                g_smokeMaterialTableCacheTelemetry.patchPathClassifierLookupMicroseconds +=
                    dirtyWork.classifierLookupMicroseconds;
                g_smokeMaterialTableCacheTelemetry.patchPathOrderedFingerprintMicroseconds +=
                    dirtyWork.orderedFingerprintMicroseconds;
                g_smokeMaterialTableCacheTelemetry.patchPathRowResolveMicroseconds +=
                    dirtyWork.rowResolveMicroseconds;
                g_smokeMaterialTableCacheTelemetry.patchPathRowRebuildMicroseconds +=
                    dirtyWork.rowRebuildMicroseconds;
                g_smokeMaterialTableCacheTelemetry.patchPathClassifierRecords =
                    GetPathTraceMaterialRecordCount();
                g_smokeMaterialTableCacheTelemetry.patchPathTextureSlotsAppended +=
                    dirtyWork.textureSlotsAppended;
                g_smokeMaterialTableCacheTelemetry.patchPathTextureSlotsReused +=
                    dirtyWork.textureSlotsReused;
                g_smokeMaterialTableCacheTelemetry.patchPathGenerationOnlyAvoidedMisses +=
                    generationOnlyAvoidedMiss ? 1 : 0;

                g_smokeMaterialTableBuildStats = RtSmokeMaterialTableBuildStats();
                ++g_smokeMaterialTableBuildStats.buildCalls;
                g_smokeMaterialTableBuildStats.tableMaterials =
                    static_cast<int>(table.materials.size());
                g_smokeMaterialTableBuildStats.safeMaterials =
                    table.materialsWithTextures;
                g_smokeMaterialTableBuildStats.descriptorTextures =
                    static_cast<int>(table.diffuseTextures.size());
                cacheHit = true;
                ++g_smokeMaterialTableCache.hits;
                if (telemetryEnabled)
                {
                    RecordSmokeMaterialTableCacheCall(
                        true,
                        telemetryStartMicroseconds);
                }
                return true;
            }
        }

        dirtyPatchForcedFull = true;
        preserveStableSlotsOnFullBuild =
            finalStructuralChanged ||
            patchFailure == RtSmokeMaterialDirtyRowPatchFailure::Patch;
        if (finalStructuralChanged)
        {
            ++g_smokeMaterialTableCacheTelemetry.forcedFullStructural;
        }
        else switch (patchFailure)
        {
            case RtSmokeMaterialDirtyRowPatchFailure::Validation:
                ++g_smokeMaterialTableCacheTelemetry.forcedFullValidation;
                break;
            case RtSmokeMaterialDirtyRowPatchFailure::Capacity:
                ++g_smokeMaterialTableCacheTelemetry.forcedFullCapacity;
                break;
            default:
                ++g_smokeMaterialTableCacheTelemetry.forcedFullPatch;
                break;
        }
    }
    else if (residencyDirtyRowsEnabled && g_smokeMaterialTableCache.valid)
    {
        if (!stableSlotPlanSelfTestPassed || !stableSlotPlanValid)
        {
            ++g_smokeMaterialTableCacheTelemetry.forcedFullValidation;
        }
        else
        {
            ++g_smokeMaterialTableCacheTelemetry.forcedFullStructural;
            preserveStableSlotsOnFullBuild = true;
        }
    }

    if (epochState.epochFailClosed)
    {
        ++g_smokeMaterialTableCache.misses;
        buildEpochFailClosedCurrentFrame();
        if (telemetryEnabled)
        {
            ++g_smokeMaterialTableCacheTelemetry.missRefresh;
            RecordSmokeMaterialTableCacheCall(
                false,
                telemetryStartMicroseconds);
        }
        return false;
    }

    if (!residencyDirtyRowsEnabled &&
        r_pathTracingMaterialCache.GetInteger() != 0 &&
        g_smokeMaterialTableCache.valid &&
        g_smokeMaterialTableCache.signature == signature)
    {
        {
            OPTICK_EVENT("PT Material Table Hit Remap");
            table = g_smokeMaterialTableCache.table;
        }
        if (r_pathTracingResidency.GetInteger() != 0 && r_pathTracingResidencyMaterial.GetInteger() != 0)
        {
            g_smokeMaterialTableBuildStats = RtSmokeMaterialTableBuildStats();
            ++g_smokeMaterialTableBuildStats.buildCalls;
            bool remapSucceeded = false;
            {
                OPTICK_EVENT("PT Material Table Hit Remap");
                remapSucceeded = RebuildSmokeMaterialTableCacheRemaps(
                    table,
                    staticMaterialIds,
                    dynamicMaterialIds,
                    staticMaterialIdSequenceSignature,
                    dynamicMaterialIdSequenceSignature);
            }
            if (!remapSucceeded)
            {
                if (telemetryEnabled)
                {
                    ++g_smokeMaterialTableCacheTelemetry.missRemap;
                }
                g_smokeMaterialTableCache.valid = false;
                ++g_smokeMaterialTableCache.misses;
                BuildSmokeMaterialTableFromUniverse(table, staticMaterialIds, dynamicMaterialIds, latchedTextureProbeMaterialId, latchedTextureProbeRequestedIndex, enableTextureProbe, minimumTextureTableLimit);
                if (telemetryEnabled)
                {
                    RecordSmokeMaterialTableCacheCall(false, telemetryStartMicroseconds);
                }
                return false;
            }

            g_smokeMaterialTableBuildStats.tableMaterials = static_cast<int>(table.materials.size());
            g_smokeMaterialTableBuildStats.safeMaterials = table.materialsWithTextures;
            g_smokeMaterialTableBuildStats.descriptorTextures = static_cast<int>(table.diffuseTextures.size());
            cacheHit = true;
            ++g_smokeMaterialTableCache.hits;
            if (telemetryEnabled)
            {
                if (!g_smokeMaterialTableCache.signatureBreakdown.valid)
                {
                    g_smokeMaterialTableCache.signatureBreakdown = signatureBreakdown;
                }
                RecordSmokeMaterialTableCacheCall(true, telemetryStartMicroseconds);
            }
            return true;
        }
        if (!RefreshSmokeMaterialTableFrameRecords(table))
        {
            if (telemetryEnabled)
            {
                ++g_smokeMaterialTableCacheTelemetry.missRefresh;
            }
            g_smokeMaterialTableCache.valid = false;
            ++g_smokeMaterialTableCache.misses;
            BuildSmokeMaterialTableFromUniverse(table, staticMaterialIds, dynamicMaterialIds, latchedTextureProbeMaterialId, latchedTextureProbeRequestedIndex, enableTextureProbe, minimumTextureTableLimit);
            if (telemetryEnabled)
            {
                RecordSmokeMaterialTableCacheCall(false, telemetryStartMicroseconds);
            }
            return false;
        }
        bool remapSucceeded = false;
        {
            OPTICK_EVENT("PT Material Table Hit Remap");
            remapSucceeded = RebuildSmokeMaterialTableCacheRemaps(
                table,
                staticMaterialIds,
                dynamicMaterialIds,
                staticMaterialIdSequenceSignature,
                dynamicMaterialIdSequenceSignature);
        }
        if (!remapSucceeded)
        {
            if (telemetryEnabled)
            {
                ++g_smokeMaterialTableCacheTelemetry.missRemap;
            }
            g_smokeMaterialTableCache.valid = false;
            ++g_smokeMaterialTableCache.misses;
            BuildSmokeMaterialTableFromUniverse(table, staticMaterialIds, dynamicMaterialIds, latchedTextureProbeMaterialId, latchedTextureProbeRequestedIndex, enableTextureProbe, minimumTextureTableLimit);
            if (telemetryEnabled)
            {
                RecordSmokeMaterialTableCacheCall(false, telemetryStartMicroseconds);
            }
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
        if (telemetryEnabled)
        {
            if (!g_smokeMaterialTableCache.signatureBreakdown.valid)
            {
                g_smokeMaterialTableCache.signatureBreakdown = signatureBreakdown;
            }
            RecordSmokeMaterialTableCacheCall(true, telemetryStartMicroseconds);
        }
        return true;
    }

    if (telemetryEnabled)
    {
        if (r_pathTracingMaterialCache.GetInteger() == 0)
        {
            ++g_smokeMaterialTableCacheTelemetry.missDisabled;
        }
        else if (!g_smokeMaterialTableCache.valid)
        {
            ++g_smokeMaterialTableCacheTelemetry.missNoEntry;
        }
        else if (dirtyPatchForcedFull)
        {
            ++g_smokeMaterialTableCacheTelemetry.missRefresh;
        }
        else
        {
            ++g_smokeMaterialTableCacheTelemetry.missSignature;
            RecordSmokeMaterialTableSignatureChanges(
                g_smokeMaterialTableCache.signatureBreakdown,
                signatureBreakdown);
        }
    }
    ++g_smokeMaterialTableCache.misses;

    for (int fullBuildAttempt = 0; fullBuildAttempt < 2; ++fullBuildAttempt)
    {
        const uint64 orderedStageBindingEntryEpoch =
            GetPathTracingImageBindingEpoch();
        epochState.BeginFullBuildAttempt();
        refreshActiveBindingsForEpoch(
            orderedStageBindingEntryEpoch,
            fullBuildAttempt != 0,
            true);
        buildFullTableForCurrentState();
        if (r_pathTracingMaterialCache.GetInteger() == 0 ||
            !ValidateSmokeMaterialIndexes(table))
        {
            break;
        }

        RtSmokeMaterialTableSignatureBreakdown finalSignatureBreakdown;
        uint64 finalStructuralSignature = 0;
        {
            OPTICK_EVENT("PT Material Table Signature Cache");
            finalSignatureBreakdown =
                CaptureSmokeMaterialTableSignatureBreakdown(
                    materialIdSetSignature,
                    enableTextureProbe,
                    minimumTextureTableLimit,
                    latchedTextureProbeMaterialId,
                    latchedTextureProbeRequestedIndex);
            finalStructuralSignature =
                ComputeSmokeMaterialTableStructuralSignature(
                    finalSignatureBreakdown);
            signature = ComputeSmokeMaterialTableSignatureFromIdSet(
                materialIdSetSignature,
                enableTextureProbe,
                minimumTextureTableLimit,
                latchedTextureProbeMaterialId,
                latchedTextureProbeRequestedIndex);
        }
        bool orderedStageBindingEpochDrifted = false;
        if (epochState.CanStoreFullBuild(residencyDirtyRowsEnabled) &&
            StoreSmokeMaterialTableCache(
                table,
                signature,
                finalStructuralSignature,
                materialIdSetSignature,
                staticMaterialIdSequenceSignature,
                dynamicMaterialIdSequenceSignature,
                finalSignatureBreakdown,
                orderedStageBindingEntryEpoch,
                orderedStageBindingEpochDrifted))
        {
            if (epochRefreshTriggeredThisCall)
            {
                g_smokeMaterialTableCache.awaitingNextQuietEpochCacheHit = true;
            }
            break;
        }
        if (orderedStageBindingEpochDrifted &&
            SmokeMaterialBindingEpochDriftShouldRetry(fullBuildAttempt + 1))
        {
            epochState.NoteEpochDrift(
                RtSmokeMaterialEpochDriftPhase::FullBuild);
            ++g_smokeMaterialTableCacheTelemetry.fullBuildEpochRetries;
            continue;
        }

        if (orderedStageBindingEpochDrifted)
        {
            ++g_smokeMaterialTableCacheTelemetry.fullBuildEpochStoreSkipped;
            ++g_smokeMaterialTableCacheTelemetry.epochSecondDriftFailClosed;
            epochState.NoteEpochDrift(
                RtSmokeMaterialEpochDriftPhase::FullBuild);
            buildEpochFailClosedCurrentFrame();
        }
        else if (telemetryEnabled)
        {
            ++g_smokeMaterialTableCacheTelemetry.forcedFullValidation;
        }
        g_smokeMaterialTableCache.valid = false;
        break;
    }
    if (telemetryEnabled)
    {
        RecordSmokeMaterialTableCacheCall(false, telemetryStartMicroseconds);
    }
    return false;
}

uint64 ComputeSmokeMaterialTableStructuralSignature(
    const RtSmokeMaterialTableSignatureBreakdown& breakdown)
{
    uint64 hash = breakdown.configSignature;
    hash = HashSmokeMaterialCacheValue(hash, breakdown.classifierGeneration);
    return hash;
}

uint64 HashSmokeMaterialCacheString(uint64 hash, const idStr& value)
{
    const char* cursor = value.c_str();
    while (cursor && *cursor)
    {
        hash = HashSmokeMaterialCacheValue(
            hash,
            static_cast<uint64>(static_cast<unsigned char>(*cursor++)));
    }
    return HashSmokeMaterialCacheValue(hash, 0u);
}

uint64 HashSmokeMaterialTextureBindingSemantic(
    uint64 hash,
    uintptr_t handleIdentity,
    bool hasHandle,
    bool safe,
    uint32_t width,
    uint32_t height)
{
    hash = HashSmokeMaterialCacheValue(hash, handleIdentity);
    hash = HashSmokeMaterialCacheValue(hash, hasHandle ? 1u : 0u);
    hash = HashSmokeMaterialCacheValue(hash, safe ? 1u : 0u);
    hash = HashSmokeMaterialCacheValue(hash, safe ? width : 0u);
    hash = HashSmokeMaterialCacheValue(hash, safe ? height : 0u);
    return hash;
}

uint64 HashSmokeMaterialTextureBindingSemantic(
    uint64 hash,
    const nvrhi::TextureHandle& texture,
    bool hasHandle,
    bool safe)
{
    uint32_t width = 0;
    uint32_t height = 0;
    if (texture && safe && IsSmokeTextureHandleSafeForDescriptor(texture))
    {
        const nvrhi::TextureDesc& desc = texture->getDesc();
        width = Max(1u, desc.width);
        height = Max(1u, desc.height);
    }
    return HashSmokeMaterialTextureBindingSemantic(
        hash,
        reinterpret_cast<uintptr_t>(texture.Get()),
        hasHandle,
        safe,
        width,
        height);
}

uint64 HashSmokeMaterialOrderedStageSourceSemantic(
    uint64 hash,
    uintptr_t imageIdentity,
    const idStr& imageName,
    uintptr_t handleIdentity,
    bool hasHandle,
    bool safe,
    uint32_t width,
    uint32_t height)
{
    hash = HashSmokeMaterialCacheValue(hash, imageIdentity);
    hash = HashSmokeMaterialCacheString(hash, imageName);
    return HashSmokeMaterialTextureBindingSemantic(
        hash,
        handleIdentity,
        hasHandle,
        safe,
        width,
        height);
}

uint64 HashSmokeMaterialOrderedStageSourceSemantic(
    uint64 hash,
    idImage* image)
{
    const idPathTracingTextureBindingSnapshot binding = image
        ? image->GetPathTracingTextureBindingSnapshot()
        : idPathTracingTextureBindingSnapshot();
    const idImageOpts* imageOpts = image ? &image->GetOpts() : nullptr;
    const bool guiTextureOverride = image &&
        r_pathTracingAllowGuiTextures.GetInteger() != 0 &&
        IsSmokeImageNameGuiLike(image->GetName());
    const bool safeImage = image && imageOpts &&
        imageOpts->samples == 1 && imageOpts->textureType == DTT_2D &&
        (!(imageOpts->isRenderTarget || imageOpts->isUAV) || guiTextureOverride) &&
        (IsSmokeImageNameSafeForRayTracing(image->GetName()) || guiTextureOverride) &&
        IsSmokeTextureHandleSafeForDescriptor(binding.texture);
    const nvrhi::TextureHandle texture = safeImage ? binding.texture : nullptr;
    uint32_t width = 0;
    uint32_t height = 0;
    if (texture)
    {
        width = Max(1u, binding.width);
        height = Max(1u, binding.height);
    }
    return HashSmokeMaterialOrderedStageSourceSemantic(
        hash,
        reinterpret_cast<uintptr_t>(image),
        idStr(image ? image->GetName() : ""),
        reinterpret_cast<uintptr_t>(texture.Get()),
        texture != nullptr,
        safeImage && texture != nullptr,
        width,
        height);
}

uint64 HashSmokeMaterialOrderedStageSources(
    uint64 hash,
    const RtMaterialRecord* classifierRecord,
    const RtSmokeMaterialTextureInfo& info)
{
    hash = HashSmokeMaterialCacheValue(
        hash,
        classifierRecord && classifierRecord->valid ? 1u : 0u);
    if (!classifierRecord || !classifierRecord->valid || !declManager)
    {
        return hash;
    }
    hash = HashSmokeMaterialCacheValue(hash, classifierRecord->signature);
    const idMaterial* material = declManager->FindMaterial(
        info.materialName.c_str(),
        false);
    hash = HashSmokeMaterialCacheValue(hash, material ? 1u : 0u);
    if (!material)
    {
        return hash;
    }

    const int stageCount = Min(
        static_cast<int>(classifierRecord->compositingStages.size()),
        RT_PATH_TRACE_ORDERED_STAGE_CAPACITY);
    hash = HashSmokeMaterialCacheValue(hash, static_cast<uint64>(stageCount));
    for (int stageSlot = 0; stageSlot < stageCount; ++stageSlot)
    {
        const int stageIndex =
            classifierRecord->compositingStages[stageSlot].stageIndex;
        hash = HashSmokeMaterialCacheValue(
            hash,
            static_cast<uint64>(stageIndex + 1));
        if (stageIndex < 0 || stageIndex >= material->GetNumStages())
        {
            hash = HashSmokeMaterialCacheValue(hash, 0u);
            continue;
        }
        const shaderStage_t* stage = material->GetStage(stageIndex);
        idImage* image = stage ? stage->texture.image : nullptr;
        hash = HashSmokeMaterialOrderedStageSourceSemantic(hash, image);
    }
    return hash;
}

uint64 HashSmokeMaterialOverrideState(
    uint64 hash,
    bool zeroRoughness,
    bool fullMetal)
{
    hash = HashSmokeMaterialCacheValue(hash, zeroRoughness ? 1u : 0u);
    return HashSmokeMaterialCacheValue(hash, fullMetal ? 1u : 0u);
}

uint64 ComputeSmokeMaterialRowSourceSignature(
    uint32_t materialId,
    const RtSmokeMaterialTextureInfo& info,
    const RtSmokePersistentMaterialRecord& record,
    const RtMaterialRecord* classifierRecord)
{
    uint64 hash = HashSmokeMaterialCacheValue(record.signature, materialId);
    hash = HashSmokeMaterialCacheString(hash, info.materialName);
    hash = HashSmokeMaterialCacheString(hash, info.diffuseImageName);
    hash = HashSmokeMaterialCacheString(hash, info.alphaImageName);
    hash = HashSmokeMaterialCacheString(hash, info.normalImageName);
    hash = HashSmokeMaterialCacheString(hash, info.specularImageName);
    hash = HashSmokeMaterialCacheString(hash, info.emissiveImageName);
    hash = HashSmokeMaterialCacheValue(hash,
        reinterpret_cast<uintptr_t>(info.diffuseImage));
    hash = HashSmokeMaterialCacheValue(hash,
        reinterpret_cast<uintptr_t>(info.alphaImage));
    hash = HashSmokeMaterialCacheValue(hash,
        reinterpret_cast<uintptr_t>(info.normalImage));
    hash = HashSmokeMaterialCacheValue(hash,
        reinterpret_cast<uintptr_t>(info.specularImage));
    hash = HashSmokeMaterialCacheValue(hash,
        reinterpret_cast<uintptr_t>(info.emissiveImage));
    hash = HashSmokeMaterialCacheValue(hash, info.hasDiffuseImage ? 1u : 0u);
    hash = HashSmokeMaterialCacheValue(hash, info.hasAlphaImage ? 1u : 0u);
    hash = HashSmokeMaterialCacheValue(hash, info.hasNormalImage ? 1u : 0u);
    hash = HashSmokeMaterialCacheValue(hash, info.hasSpecularImage ? 1u : 0u);
    hash = HashSmokeMaterialCacheValue(hash, info.hasEmissiveImage ? 1u : 0u);
    hash = HashSmokeMaterialTextureBindingSemantic(
        hash,
        info.diffuseTexture,
        info.hasTextureHandle,
        info.hasSafeTexture);
    hash = HashSmokeMaterialTextureBindingSemantic(
        hash,
        info.alphaTexture,
        info.hasAlphaTextureHandle,
        info.hasSafeAlphaTexture);
    hash = HashSmokeMaterialTextureBindingSemantic(
        hash,
        info.normalTexture,
        info.hasNormalTextureHandle,
        info.hasSafeNormalTexture);
    hash = HashSmokeMaterialTextureBindingSemantic(
        hash,
        info.specularTexture,
        info.hasSpecularTextureHandle,
        info.hasSafeSpecularTexture);
    hash = HashSmokeMaterialTextureBindingSemantic(
        hash,
        info.emissiveTexture,
        info.hasEmissiveTextureHandle,
        info.hasSafeEmissiveTexture);
    hash = HashSmokeMaterialCacheValue(hash, info.hasAlphaTest ? 1u : 0u);
    hash = HashSmokeMaterialCacheValue(hash, info.isDynamic ? 1u : 0u);
    hash = HashSmokeMaterialOverrideState(
        hash,
        SmokeMaterialHasZeroRoughnessOverride(materialId),
        SmokeMaterialHasFullMetalOverride(materialId));
    return HashSmokeMaterialOrderedStageSources(hash, classifierRecord, info);
}

bool BuildSmokeMaterialStableSlotPlan(
    const std::vector<uint32_t>& existingSlotMaterialIds,
    const std::vector<uint32_t>& staticMaterialIds,
    const std::vector<uint32_t>& dynamicMaterialIds,
    RtSmokeMaterialStableSlotPlan& plan)
{
    plan = RtSmokeMaterialStableSlotPlan();
    plan.slotMaterialIds = existingSlotMaterialIds;
    plan.staticMaterialIndexes.reserve(staticMaterialIds.size());
    plan.dynamicMaterialIndexes.reserve(dynamicMaterialIds.size());

    std::unordered_map<uint32_t, uint32_t> lookup;
    lookup.reserve(existingSlotMaterialIds.size() +
        staticMaterialIds.size() + dynamicMaterialIds.size());
    for (int slotIndex = 0;
        slotIndex < static_cast<int>(existingSlotMaterialIds.size());
        ++slotIndex)
    {
        if (!lookup.emplace(
                existingSlotMaterialIds[slotIndex],
                static_cast<uint32_t>(slotIndex)).second)
        {
            return false;
        }
    }

    const auto appendOrFind = [&](uint32_t materialId) -> uint32_t {
        const std::unordered_map<uint32_t, uint32_t>::const_iterator existing =
            lookup.find(materialId);
        if (existing != lookup.end())
        {
            return existing->second;
        }
        const uint32_t slotIndex =
            static_cast<uint32_t>(plan.slotMaterialIds.size());
        plan.slotMaterialIds.push_back(materialId);
        lookup.emplace(materialId, slotIndex);
        ++plan.appendedIds;
        return slotIndex;
    };

    for (uint32_t materialId : staticMaterialIds)
    {
        plan.staticMaterialIndexes.push_back(appendOrFind(materialId));
    }
    for (uint32_t materialId : dynamicMaterialIds)
    {
        plan.dynamicMaterialIndexes.push_back(appendOrFind(materialId));
    }
    return true;
}

bool SmokeMaterialDynamicPartitionSelfTest()
{
    RtSmokeMaterialRowProductSnapshot baseline;
    baseline.valid = true;
    const RtSmokeMaterialRowProductDelta equalDelta =
        CompareSmokeMaterialRowProducts(baseline, baseline);
    if (equalDelta.comparisonError || equalDelta.Any())
    {
        return false;
    }

    RtSmokeMaterialRowProductSnapshot changed = baseline;
    changed.row.flags ^= 1u;
    RtSmokeMaterialRowProductDelta delta =
        CompareSmokeMaterialRowProducts(baseline, changed);
    if (!delta.row || delta.facts || delta.feature || delta.parameters ||
        delta.binding)
    {
        return false;
    }
    changed = baseline;
    changed.facts.materialFlags ^= 1u;
    delta = CompareSmokeMaterialRowProducts(baseline, changed);
    if (!delta.facts || delta.row || delta.feature || delta.parameters ||
        delta.binding)
    {
        return false;
    }
    changed = baseline;
    changed.feature.materialCaps ^= 1u;
    delta = CompareSmokeMaterialRowProducts(baseline, changed);
    if (!delta.feature || delta.row || delta.facts || delta.parameters ||
        delta.binding)
    {
        return false;
    }
    changed = baseline;
    changed.parameters.orderedStageWords[0] ^= 1u;
    changed.parameters.orderedStageTextureWords[0] ^= 0x80000001u;
    delta = CompareSmokeMaterialRowProducts(baseline, changed);
    if (!delta.parameters || delta.row || delta.facts || delta.feature ||
        delta.binding)
    {
        return false;
    }
    baseline.row.diffuseTextureIndex = 3u;
    baseline.bindings[0].descriptorIndex = 3u;
    baseline.bindings[0].textureIdentity = 0x1000u;
    changed = baseline;
    changed.bindings[0].textureIdentity = 0x2000u;
    delta = CompareSmokeMaterialRowProducts(baseline, changed);
    if (!delta.binding || delta.row || delta.facts || delta.feature ||
        delta.parameters)
    {
        return false;
    }
    changed = baseline;
    changed.bindings[0].descriptorIndex = 4u;
    changed.row.diffuseTextureIndex = 4u;
    delta = CompareSmokeMaterialRowProducts(baseline, changed);
    if (!delta.binding || !delta.row)
    {
        return false;
    }

    RtSmokeMaterialTableBuild invalidBindingTable;
    invalidBindingTable.materialIds = { 1u };
    invalidBindingTable.materials.resize(1);
    invalidBindingTable.materialFacts.resize(1);
    invalidBindingTable.materialFeatures.resize(1);
    invalidBindingTable.materialFeatureParameters.resize(1);
    invalidBindingTable.materialInfos.resize(1);
    invalidBindingTable.materials[0].diffuseTextureIndex = 1u;
    RtSmokeMaterialRowProductSnapshot invalidBinding;
    if (CaptureSmokeMaterialRowProductSnapshot(
            invalidBindingTable,
            0,
            invalidBinding) ||
        !CompareSmokeMaterialRowProducts(invalidBinding, baseline).comparisonError)
    {
        return false;
    }

    RtSmokeMaterialTableCacheStats::DynamicPartition taxonomy;
    RecordSmokeMaterialDynamicWakeCounters(
        taxonomy,
        RT_SMOKE_DYNAMIC_WAKE_DYNAMIC |
            RT_SMOKE_DYNAMIC_WAKE_BINDING |
            RT_SMOKE_DYNAMIC_WAKE_ORDERED);
    RecordSmokeMaterialDynamicWakeCounters(
        taxonomy,
        RT_SMOKE_DYNAMIC_WAKE_DYNAMIC |
            RT_SMOKE_DYNAMIC_WAKE_APPEND);
    const uint64 taxonomyExclusive =
        taxonomy.exclusiveDynamic + taxonomy.exclusiveAppend +
        taxonomy.exclusiveNew + taxonomy.exclusiveSignature +
        taxonomy.exclusiveBinding + taxonomy.exclusiveOrdered +
        taxonomy.exclusiveOverride + taxonomy.exclusiveFacts +
        taxonomy.exclusiveOther;
    if (taxonomy.wakeDynamic != 2u || taxonomy.wakeBinding != 1u ||
        taxonomy.wakeOrdered != 1u || taxonomy.wakeAppend != 1u ||
        taxonomy.exclusiveBinding != 1u ||
        taxonomy.exclusiveAppend != 1u || taxonomyExclusive != 2u)
    {
        return false;
    }

    const uint32_t combinedSubclasses =
        RT_SMOKE_DYNAMIC_SUBCLASS_GUI |
        RT_SMOKE_DYNAMIC_SUBCLASS_REGISTERS |
        RT_SMOKE_DYNAMIC_SUBCLASS_TEXTURE_MATRIX;
    if (FinalizeSmokeMaterialDynamicSubclassMask(0u) !=
            RT_SMOKE_DYNAMIC_SUBCLASS_OTHER ||
        FinalizeSmokeMaterialDynamicSubclassMask(combinedSubclasses) !=
            combinedSubclasses)
    {
        return false;
    }

    RtSmokeMaterialTableCacheStats::DynamicPartition partition;
    partition.rebuilt = 3u;
    partition.compareIdentical = 1u;
    partition.comparePartial = 1u;
    partition.compareError = 1u;
    return partition.rebuilt == partition.compareIdentical +
        partition.comparePartial + partition.compareError;
}

bool SmokeMaterialStableSlotPlanSelfTest()
{
    RtSmokeMaterialStableSlotPlan plan;
    if (!BuildSmokeMaterialStableSlotPlan(
            { 10u, 20u },
            { 20u, 30u },
            { 10u, 30u },
            plan) ||
        plan.slotMaterialIds != std::vector<uint32_t>({ 10u, 20u, 30u }) ||
        plan.staticMaterialIndexes != std::vector<uint32_t>({ 1u, 2u }) ||
        plan.dynamicMaterialIndexes != std::vector<uint32_t>({ 0u, 2u }) ||
        plan.appendedIds != 1)
    {
        return false;
    }
    const std::vector<uint32_t> retainedSlotMaterialIds = plan.slotMaterialIds;
    if (!BuildSmokeMaterialStableSlotPlan(
            retainedSlotMaterialIds,
            { 30u },
            { 20u },
            plan) ||
        plan.slotMaterialIds != std::vector<uint32_t>({ 10u, 20u, 30u }) ||
        plan.staticMaterialIndexes != std::vector<uint32_t>({ 2u }) ||
        plan.dynamicMaterialIndexes != std::vector<uint32_t>({ 1u }) ||
        plan.appendedIds != 0)
    {
        return false;
    }
    const std::vector<uint32_t> shrunkenSlotMaterialIds = plan.slotMaterialIds;
    if (!BuildSmokeMaterialStableSlotPlan(
            shrunkenSlotMaterialIds,
            { 30u, 10u },
            { 20u },
            plan) ||
        plan.slotMaterialIds != std::vector<uint32_t>({ 10u, 20u, 30u }) ||
        plan.staticMaterialIndexes != std::vector<uint32_t>({ 2u, 0u }) ||
        plan.dynamicMaterialIndexes != std::vector<uint32_t>({ 1u }) ||
        plan.appendedIds != 0)
    {
        return false;
    }
    if (!BuildSmokeMaterialStableSlotPlan(
            { 0u, 10u },
            { 10u, 10u, 0u },
            {},
            plan))
    {
        return false;
    }
    if (plan.slotMaterialIds != std::vector<uint32_t>({ 0u, 10u }) ||
        plan.staticMaterialIndexes != std::vector<uint32_t>({ 1u, 1u, 0u }) ||
        plan.appendedIds != 0 ||
        !SmokeMaterialUniverseSemanticFingerprintSelfTest() ||
        !PathTraceMaterialRecordLookupIndexSelfTest() ||
        !SmokeMaterialActiveTextureRefreshSelfTest() ||
        !PathTracingImageBindingPublicationSelfTest() ||
        !SmokeMaterialDynamicPartitionSelfTest())
    {
        return false;
    }

    RtSmokePersistentMaterialRecord fingerprintRecord;
    fingerprintRecord.valid = true;
    fingerprintRecord.signature = 0x51a7u;
    RtSmokeMaterialTextureInfo fingerprintInfo;
    fingerprintInfo.materialName = "_rt_row_fingerprint_self_test";
    const uint64 baselineRowSignature =
        ComputeSmokeMaterialRowSourceSignature(
            0xf17e52u,
            fingerprintInfo,
            fingerprintRecord,
            nullptr);
    fingerprintInfo.hasNormalImage = true;
    fingerprintInfo.normalImageName = "textures/selftest/normal_local";
    const uint64 normalRowSignature =
        ComputeSmokeMaterialRowSourceSignature(
            0xf17e52u,
            fingerprintInfo,
            fingerprintRecord,
            nullptr);
    fingerprintInfo.hasSpecularImage = true;
    fingerprintInfo.specularImageName = "textures/selftest/specular_s";
    const uint64 specularRowSignature =
        ComputeSmokeMaterialRowSourceSignature(
            0xf17e52u,
            fingerprintInfo,
            fingerprintRecord,
            nullptr);
    fingerprintInfo.isDynamic = !fingerprintInfo.isDynamic;
    const uint64 dynamicRowSignature =
        ComputeSmokeMaterialRowSourceSignature(
            0xf17e52u,
            fingerprintInfo,
            fingerprintRecord,
            nullptr);
    if (baselineRowSignature == normalRowSignature ||
        normalRowSignature == specularRowSignature ||
        specularRowSignature == dynamicRowSignature)
    {
        return false;
    }

    const uint64 textureBase = HashSmokeMaterialTextureBindingSemantic(
        17u, 0x1000u, true, true, 64u, 64u);
    if (textureBase == HashSmokeMaterialTextureBindingSemantic(
            17u, 0x2000u, true, true, 64u, 64u) ||
        textureBase == HashSmokeMaterialTextureBindingSemantic(
            17u, 0x1000u, true, true, 128u, 64u) ||
        HashSmokeMaterialOverrideState(17u, false, false) ==
            HashSmokeMaterialOverrideState(17u, true, false))
    {
        return false;
    }

    RtMaterialRecord orderedStageRecord;
    orderedStageRecord.valid = true;
    RtMaterialRecord noOrderedStageRecord = orderedStageRecord;
    orderedStageRecord.compositingStages.push_back(
        RtMaterialCompositingStageFact());
    if (SmokeMaterialRecordHasOrderedStageSources(nullptr) ||
        SmokeMaterialRecordHasOrderedStageSources(&noOrderedStageRecord) ||
        !SmokeMaterialRecordHasOrderedStageSources(&orderedStageRecord))
    {
        return false;
    }

    const idStr orderedImageName = "textures/selftest/ordered_stage";
    const uint64 orderedSourceSeed = 0x0dd37e5a9eull;
    const uint64 orderedMissing =
        HashSmokeMaterialOrderedStageSourceSemantic(
            orderedSourceSeed,
            0x1000u,
            orderedImageName,
            0u,
            false,
            false,
            0u,
            0u);
    const uint64 orderedReady =
        HashSmokeMaterialOrderedStageSourceSemantic(
            orderedSourceSeed,
            0x1000u,
            orderedImageName,
            0x2000u,
            true,
            true,
            64u,
            64u);
    const uint64 orderedReplacement =
        HashSmokeMaterialOrderedStageSourceSemantic(
            orderedSourceSeed,
            0x1000u,
            orderedImageName,
            0x3000u,
            true,
            true,
            64u,
            64u);
    const uint64 orderedLoss =
        HashSmokeMaterialOrderedStageSourceSemantic(
            orderedSourceSeed,
            0x1000u,
            orderedImageName,
            0u,
            false,
            false,
            0u,
            0u);
    const uint64 orderedResized =
        HashSmokeMaterialOrderedStageSourceSemantic(
            orderedSourceSeed,
            0x1000u,
            orderedImageName,
            0x2000u,
            true,
            true,
            128u,
            64u);
    if (orderedMissing == orderedReady ||
        orderedReady == orderedReplacement ||
        orderedReady == orderedLoss ||
        orderedReady == orderedResized ||
        !SmokeMaterialRowFingerprintNeedsRebuild(
            false, false, false, orderedMissing, orderedReady) ||
        !SmokeMaterialRowFingerprintNeedsRebuild(
            false, false, false, orderedReady, orderedReplacement) ||
        !SmokeMaterialRowFingerprintNeedsRebuild(
            false, false, false, orderedReady, orderedLoss) ||
        !SmokeMaterialRowFingerprintNeedsRebuild(
            false, false, false, orderedReady, orderedResized) ||
        SmokeMaterialRowFingerprintNeedsRebuild(
            false, false, false, orderedReady, orderedReady) ||
        !SmokeMaterialRowFingerprintNeedsRebuild(
            false, false, true, orderedReady, orderedReady) ||
        BuildSmokeOrderedStageTextureWord(UINT32_MAX) != 0u ||
        BuildSmokeOrderedStageTextureWord(3u) != 0x80000003u ||
        BuildSmokeOrderedStageTextureWord(3u) ==
            BuildSmokeOrderedStageTextureWord(4u))
    {
        return false;
    }

    RtSmokeMaterialTableSignatureBreakdown classifierA;
    classifierA.configSignature = 3u;
    classifierA.classifierGeneration = 7u;
    RtSmokeMaterialTableSignatureBreakdown classifierB = classifierA;
    classifierB.classifierGeneration = 8u;
    if (ComputeSmokeMaterialTableStructuralSignature(classifierA) ==
        ComputeSmokeMaterialTableStructuralSignature(classifierB))
    {
        return false;
    }

    RtSmokeMaterialTableBuild maskTable;
    maskTable.materialIds = { 100u, 200u, 300u };
    maskTable.materials.resize(3);
    maskTable.materialInfos.resize(3);
    maskTable.materialFacts.resize(3);
    maskTable.materialFeatures.resize(3);
    maskTable.materialFeatureParameters.resize(3);
    maskTable.staticMaterialIndexes = { 1u, 1u };
    maskTable.dynamicMaterialIndexes = { 2u };
    maskTable.diffuseTextures.resize(1);
    for (PathTraceSmokeMaterial& material : maskTable.materials)
    {
        ResetSmokeMaterialTextureBindings(material);
    }
    maskTable.materials[0].diffuseTextureIndex = 0u;
    maskTable.materials[0].textureWidth = 512u;
    maskTable.materialFeatureParameters[0].orderedStageTextureWords[0] =
        0x80000000u;
    maskTable.materialFacts[0].emissive = true;
    maskTable.materialInfos[0].diffuseImage =
        reinterpret_cast<idImage*>(static_cast<uintptr_t>(1u));
    maskTable.materialInfos[0].diffuseImageName = "textures/selftest/a";
    maskTable.materialFacts[0].hasSafeDiffuseTexture = true;
    maskTable.materials[1].diffuseTextureIndex = 0u;
    maskTable.materialFeatureParameters[1].orderedStageTextureWords[0] =
        0x80000000u;
    maskTable.materialInfos[1].diffuseImage =
        reinterpret_cast<idImage*>(static_cast<uintptr_t>(2u));
    maskTable.materialInfos[1].diffuseImageName = "textures/selftest/b";
    maskTable.materialFacts[1].hasDiffuseImage = true;
    maskTable.materialFacts[1].hasSafeDiffuseTexture = true;
    maskTable.materials[2].diffuseTextureIndex = 0u;

    std::vector<bool> activeRows;
    if (!BuildSmokeMaterialActiveRowMask(maskTable, activeRows) ||
        activeRows != std::vector<bool>({ false, true, true }))
    {
        return false;
    }
    const std::vector<int> safeRows = BuildSmokeSafeMaterialIndexOrder(
        maskTable,
        maskTable.materialInfos,
        activeRows);
    if (safeRows != std::vector<int>({ 1 }))
    {
        return false;
    }
    if (!ResetSmokeInactiveMaterialBindings(maskTable, activeRows) ||
        maskTable.materials[0].diffuseTextureIndex != UINT32_MAX ||
        maskTable.materials[0].textureWidth != 1u ||
        maskTable.materialFeatureParameters[0].orderedStageTextureWords[0] != 0u ||
        maskTable.materials[1].diffuseTextureIndex != 0u ||
        !SmokeMaterialRowFingerprintNeedsRebuild(
            false, false, true, 0x51a7u, 0x51a7u))
    {
        return false;
    }
    RecomputeSmokeMaterialTableDiagnostics(maskTable);
    if (maskTable.materialsWithTextures != 2 ||
        maskTable.materialsEmissive != 0)
    {
        return false;
    }

    RtSmokeMaterialDirtyRowWork refWork;
    if (!InitializeSmokeMaterialPatchTextureReferences(
            maskTable,
            activeRows,
            refWork) ||
        refWork.textureSlotReferenceCounts != std::vector<uint32_t>({ 3u }) ||
        !ReleaseSmokeMaterialPatchTextureReferencesForRow(
            maskTable,
            1,
            refWork) ||
        refWork.textureSlotReferenceCounts != std::vector<uint32_t>({ 1u }) ||
        FindSmokeMaterialReusableTextureSlot(
            refWork.textureSlotReferenceCounts) != -1 ||
        !ReleaseSmokeMaterialPatchTextureReferencesForRow(
            maskTable,
            2,
            refWork) ||
        FindSmokeMaterialReusableTextureSlot(
            refWork.textureSlotReferenceCounts) != 0)
    {
        return false;
    }

    RtSmokeMaterialTableCache generationCache;
    generationCache.observedRegistryGeneration = 11u;
    generationCache.observedResidentFactsGeneration = 17u;
    generationCache.observedOverrideGeneration = 23u;
    RtSmokeMaterialTableSignatureBreakdown generationCurrent;
    generationCurrent.registryGeneration = 12u;
    generationCurrent.residentFactsGeneration = 17u;
    generationCurrent.overrideGeneration = 23u;
    if (SmokeMaterialNonPrimarySourcesChanged(
            generationCache,
            generationCurrent))
    {
        return false;
    }
    ++generationCurrent.residentFactsGeneration;
    if (!SmokeMaterialNonPrimarySourcesChanged(
            generationCache,
            generationCurrent))
    {
        return false;
    }
    generationCurrent.residentFactsGeneration = 17u;
    ++generationCurrent.overrideGeneration;
    if (!SmokeMaterialNonPrimarySourcesChanged(
            generationCache,
            generationCurrent))
    {
        return false;
    }

    RtSmokeMaterialEpochInvocationState patchFailClosedState;
    uint64 observedEpoch = 31u;
    bool fullFallbackStored = false;
    if (!patchFailClosedState.NoteEpochDrift(
            RtSmokeMaterialEpochDriftPhase::Patch) ||
        patchFailClosedState.NoteEpochDrift(
            RtSmokeMaterialEpochDriftPhase::Patch))
    {
        return false;
    }
    patchFailClosedState.BeginFullBuildAttempt();
    patchFailClosedState.NoteForcedActiveRefresh();
    if (patchFailClosedState.CanStoreFullBuild(true))
    {
        fullFallbackStored = true;
        observedEpoch = 32u;
    }
    if (!patchFailClosedState.epochFailClosed || fullFallbackStored ||
        observedEpoch != 31u)
    {
        return false;
    }

    RtSmokeMaterialEpochInvocationState fullFailClosedState;
    if (!fullFailClosedState.NoteEpochDrift(
            RtSmokeMaterialEpochDriftPhase::FullBuild) ||
        fullFailClosedState.NoteEpochDrift(
            RtSmokeMaterialEpochDriftPhase::FullBuild))
    {
        return false;
    }
    fullFailClosedState.BeginFullBuildAttempt();
    fullFailClosedState.NoteForcedActiveRefresh();
    if (fullFailClosedState.CanStoreFullBuild(true))
    {
        return false;
    }

    RtSmokeMaterialEpochInvocationState invalidCacheFullBuildState;
    uint64 publishedPrimaryHandle = 0x2000u;
    uint64 registryPrimaryHandle = 0x1000u;
    uint64 builtPrimaryHandle = 0u;
    invalidCacheFullBuildState.BeginFullBuildAttempt();
    if (invalidCacheFullBuildState.CanStoreFullBuild(true))
    {
        return false;
    }
    registryPrimaryHandle = publishedPrimaryHandle;
    invalidCacheFullBuildState.NoteForcedActiveRefresh();
    builtPrimaryHandle = registryPrimaryHandle;
    if (!invalidCacheFullBuildState.CanStoreFullBuild(true) ||
        builtPrimaryHandle != publishedPrimaryHandle)
    {
        return false;
    }

    return SmokeMaterialBindingEpochCanCommit(17u, 17u) &&
        !SmokeMaterialBindingEpochCanCommit(17u, 18u) &&
        SmokeMaterialBindingEpochDriftShouldRetry(1) &&
        !SmokeMaterialBindingEpochDriftShouldRetry(2) &&
        !SmokeMaterialRowNeedsConsideration(
            false, true, true, true, true, true, true, true) &&
        !SmokeMaterialRowNeedsConsideration(
            true, false, false, false, false, false, false, false) &&
        !SmokeMaterialRowNeedsConsideration(
            true, false, false, false, false, false, true, false) &&
        SmokeMaterialRowNeedsConsideration(
            true, false, false, false, false, false, true, true) &&
        SmokeMaterialRowNeedsConsideration(
            true, true, false, false, false, false, false, false) &&
        SmokeMaterialRowNeedsConsideration(
            true, false, true, false, false, false, false, false) &&
        SmokeMaterialRowNeedsConsideration(
            true, false, false, true, false, false, false, false) &&
        SmokeMaterialRowNeedsConsideration(
            true, false, false, false, true, false, false, false) &&
        SmokeMaterialRowNeedsConsideration(
            true, false, false, false, false, true, false, false) &&
        SmokeMaterialPatchCanCommit(true, true, true, true) &&
        !SmokeMaterialPatchCanCommit(false, true, true, true) &&
        !SmokeMaterialPatchCanCommit(true, false, true, true) &&
        !SmokeMaterialPatchCanCommit(true, true, false, true) &&
        !SmokeMaterialPatchCanCommit(true, true, true, false);
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
    RtSmokeMaterialTableCacheStats stats = g_smokeMaterialTableCacheTelemetry;
    stats.hits = g_smokeMaterialTableCache.hits;
    stats.misses = g_smokeMaterialTableCache.misses;
    return stats;
}

void DumpSmokeMaterialTableCacheTelemetryIfNeeded()
{
    if (!SmokeMaterialTableCacheTelemetryEnabled())
    {
        return;
    }
    static int lastDumpFrame = -120;
    if (tr.frameCount - lastDumpFrame < 120)
    {
        return;
    }
    lastDumpFrame = tr.frameCount;

    const RtSmokeMaterialTableCacheStats stats = GetSmokeMaterialTableCacheStats();
    const double averageHitMs = stats.telemetryHits != 0
        ? static_cast<double>(stats.hitTotalMicroseconds) /
            static_cast<double>(stats.telemetryHits) / 1000.0
        : 0.0;
    const double averageMissMs = stats.telemetryMisses != 0
        ? static_cast<double>(stats.missTotalMicroseconds) /
            static_cast<double>(stats.telemetryMisses) / 1000.0
        : 0.0;
    const double completedPatchCalls =
        static_cast<double>(stats.patchPathCompletedCalls);
    const auto averagePatchMicroseconds = [completedPatchCalls](uint64 value) {
        return completedPatchCalls != 0.0
            ? static_cast<double>(value) / completedPatchCalls
            : 0.0;
    };
    common->Printf(
        "PathTracePrimaryPass: RES materialTableCache calls=%llu hit=%llu miss=%llu reasons(disabled/noEntry/signature/refresh/remap)=%llu/%llu/%llu/%llu/%llu signatureChanges(materialSet/config/registrySize/registryGeneration/residentFacts/overrides/classifier/unknown)=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu timingMs(hitAvg/hitMax/missAvg/missMax)=%.3f/%.3f/%.3f/%.3f explicitClears=%llu reconcile=%d basis=cumulative\n",
        static_cast<unsigned long long>(stats.telemetryCalls),
        static_cast<unsigned long long>(stats.telemetryHits),
        static_cast<unsigned long long>(stats.telemetryMisses),
        static_cast<unsigned long long>(stats.missDisabled),
        static_cast<unsigned long long>(stats.missNoEntry),
        static_cast<unsigned long long>(stats.missSignature),
        static_cast<unsigned long long>(stats.missRefresh),
        static_cast<unsigned long long>(stats.missRemap),
        static_cast<unsigned long long>(stats.signatureChangedMaterialSet),
        static_cast<unsigned long long>(stats.signatureChangedConfig),
        static_cast<unsigned long long>(stats.signatureChangedRegistrySize),
        static_cast<unsigned long long>(stats.signatureChangedRegistryGeneration),
        static_cast<unsigned long long>(stats.signatureChangedResidentFacts),
        static_cast<unsigned long long>(stats.signatureChangedOverrides),
        static_cast<unsigned long long>(stats.signatureChangedClassifier),
        static_cast<unsigned long long>(stats.signatureChangedUnknown),
        averageHitMs,
        static_cast<double>(stats.hitMaxMicroseconds) / 1000.0,
        averageMissMs,
        static_cast<double>(stats.missMaxMicroseconds) / 1000.0,
        static_cast<unsigned long long>(stats.explicitClears),
        stats.telemetryCalls == stats.telemetryHits + stats.telemetryMisses &&
            stats.telemetryMisses == stats.missDisabled + stats.missNoEntry +
                stats.missSignature + stats.missRefresh + stats.missRemap
            ? 1
            : 0);
    const uint64 currentOrderedStageBindingEpoch =
        GetPathTracingImageBindingEpoch();
    const uint64 cachedOrderedStageBindingEpoch =
        g_smokeMaterialTableCache.observedOrderedStageBindingEpoch;
    const uint64 orderedStageBindingEpochDelta =
        currentOrderedStageBindingEpoch >= cachedOrderedStageBindingEpoch
            ? currentOrderedStageBindingEpoch - cachedOrderedStageBindingEpoch
            : 0u;
    common->Printf(
        "PathTracePrimaryPass: RES materialTableDirtyRows patchPath=%llu appendedIds=%llu rows(considered/rebuilt/skippedUnchanged/dynamic)=%llu/%llu/%llu/%llu orderedStage(considered/skipped/rebuilt)=%llu/%llu/%llu classifier(records/activeRows/finds)=%d/%llu/%llu timingUs(avgTotal/copy/refcount/lookup/fingerprint/resolve/rebuild)=%.3f/%.3f/%.3f/%.3f/%.3f/%.3f/%.3f completed=%llu textureSlots(appended/reused)=%llu/%llu forcedFull(structural/validation/capacity/patch)=%llu/%llu/%llu/%llu generationOnlyAvoidedMisses=%llu selfTest(evaluated/passed)=%d/%d reconcile=%d orderedStageReconcile=%d orderedStageEpoch(eligible/scannedDueEpoch/avoidedSteady)=%llu/%llu/%llu epoch(current/cached/delta/create/purge)=%llu/%llu/%llu/%llu/%llu retry(patch/failClosed/full/storeSkipped)=%llu/%llu/%llu/%llu activeRefresh(calls/visited/changed/retry/secondDrift/nextQuietHit)=%llu/%llu/%llu/%llu/%llu/%llu nonPrimaryWakeRows=%llu forcedRefresh(full/invalid/visited/changed/failClosedNoStore)=%llu/%llu/%llu/%llu/%llu epochReconcile=%d basis=cumulative\n",
        static_cast<unsigned long long>(stats.patchPathCalls),
        static_cast<unsigned long long>(stats.patchPathAppendedIds),
        static_cast<unsigned long long>(stats.patchPathConsideredRows),
        static_cast<unsigned long long>(stats.patchPathRebuiltRows),
        static_cast<unsigned long long>(stats.patchPathSkippedUnchangedRows),
        static_cast<unsigned long long>(stats.patchPathDynamicRows),
        static_cast<unsigned long long>(stats.patchPathOrderedStageConsideredRows),
        static_cast<unsigned long long>(stats.patchPathOrderedStageSkippedRows),
        static_cast<unsigned long long>(stats.patchPathOrderedStageRebuiltRows),
        stats.patchPathClassifierRecords,
        static_cast<unsigned long long>(stats.patchPathActiveRows),
        static_cast<unsigned long long>(stats.patchPathClassifierFinds),
        averagePatchMicroseconds(stats.patchPathTotalMicroseconds),
        averagePatchMicroseconds(stats.patchPathTableCopyMicroseconds),
        averagePatchMicroseconds(stats.patchPathTextureRefcountMicroseconds),
        averagePatchMicroseconds(stats.patchPathClassifierLookupMicroseconds),
        averagePatchMicroseconds(stats.patchPathOrderedFingerprintMicroseconds),
        averagePatchMicroseconds(stats.patchPathRowResolveMicroseconds),
        averagePatchMicroseconds(stats.patchPathRowRebuildMicroseconds),
        static_cast<unsigned long long>(stats.patchPathCompletedCalls),
        static_cast<unsigned long long>(stats.patchPathTextureSlotsAppended),
        static_cast<unsigned long long>(stats.patchPathTextureSlotsReused),
        static_cast<unsigned long long>(stats.forcedFullStructural),
        static_cast<unsigned long long>(stats.forcedFullValidation),
        static_cast<unsigned long long>(stats.forcedFullCapacity),
        static_cast<unsigned long long>(stats.forcedFullPatch),
        static_cast<unsigned long long>(stats.patchPathGenerationOnlyAvoidedMisses),
        stats.patchSelfTestEvaluated ? 1 : 0,
        stats.patchSelfTestPassed ? 1 : 0,
        stats.patchPathConsideredRows ==
            stats.patchPathRebuiltRows +
                stats.patchPathSkippedUnchangedRows
            ? 1
            : 0,
        stats.patchPathOrderedStageConsideredRows ==
            stats.patchPathOrderedStageSkippedRows +
                stats.patchPathOrderedStageRebuiltRows
            ? 1
            : 0,
        static_cast<unsigned long long>(
            stats.patchPathOrderedStageEpochEligibleRows),
        static_cast<unsigned long long>(
            stats.patchPathOrderedStageScannedDueEpochRows),
        static_cast<unsigned long long>(
            stats.patchPathOrderedStageAvoidedSteadyRows),
        static_cast<unsigned long long>(currentOrderedStageBindingEpoch),
        static_cast<unsigned long long>(cachedOrderedStageBindingEpoch),
        static_cast<unsigned long long>(orderedStageBindingEpochDelta),
        static_cast<unsigned long long>(GetPathTracingImageBindingCreateCount()),
        static_cast<unsigned long long>(GetPathTracingImageBindingPurgeCount()),
        static_cast<unsigned long long>(stats.patchPathEpochRetries),
        static_cast<unsigned long long>(stats.patchPathEpochFailClosed),
        static_cast<unsigned long long>(stats.fullBuildEpochRetries),
        static_cast<unsigned long long>(stats.fullBuildEpochStoreSkipped),
        static_cast<unsigned long long>(stats.epochTriggeredRefreshCalls),
        static_cast<unsigned long long>(stats.epochTriggeredRefreshVisitedIds),
        static_cast<unsigned long long>(stats.epochTriggeredRefreshChangedIds),
        static_cast<unsigned long long>(stats.epochTriggeredRefreshRetries),
        static_cast<unsigned long long>(stats.epochSecondDriftFailClosed),
        static_cast<unsigned long long>(stats.epochNextQuietCacheHits),
        static_cast<unsigned long long>(
            stats.patchPathNonPrimaryGenerationWakeRows),
        static_cast<unsigned long long>(stats.epochForcedFullRefreshCalls),
        static_cast<unsigned long long>(
            stats.epochForcedInvalidCacheRefreshCalls),
        static_cast<unsigned long long>(
            stats.epochForcedFullRefreshVisitedIds),
        static_cast<unsigned long long>(
            stats.epochForcedFullRefreshChangedIds),
        static_cast<unsigned long long>(stats.epochFailClosedNoStore),
        stats.patchPathOrderedStageEpochEligibleRows ==
            stats.patchPathOrderedStageScannedDueEpochRows +
                stats.patchPathOrderedStageAvoidedSteadyRows
            ? 1
            : 0);
    const RtSmokeMaterialTableCacheStats::DynamicPartition& partition =
        stats.dynamicPartition;
    const uint64 exclusiveTotal =
        partition.exclusiveDynamic + partition.exclusiveAppend +
        partition.exclusiveNew + partition.exclusiveSignature +
        partition.exclusiveBinding + partition.exclusiveOrdered +
        partition.exclusiveOverride + partition.exclusiveFacts +
        partition.exclusiveOther;
    common->Printf(
        "PathTracePrimaryPass: RES materialTableDynamicPartition rebuilt=%llu compare(identical/partial/error)=%llu/%llu/%llu identicalDynamic=%llu dynamicRebuilt=%llu runtimeCoveredCandidateHypothesis=%llu wakeOverlap(W-dyn/W-append/W-new/W-sig/W-bind/W-ord/W-ovr/W-facts)=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu wakeExclusive(append/new/bind/facts/ovr/dyn/ord/sig/other)=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu dynamicSubclass(D-gui/D-reg/D-texmat/D-spectrum/D-decal/D-video/D-rt/D-other)=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu productDelta(P-row/P-facts/P-feat/P-params/P-bind)=%llu/%llu/%llu/%llu/%llu invariant(comparePartition/exclusivePartition/failures)=%d/%d/%llu bindSemantics=sameIndexAndSameOwningHandle basis=cumulative\n",
        static_cast<unsigned long long>(partition.rebuilt),
        static_cast<unsigned long long>(partition.compareIdentical),
        static_cast<unsigned long long>(partition.comparePartial),
        static_cast<unsigned long long>(partition.compareError),
        static_cast<unsigned long long>(partition.identicalDynamic),
        static_cast<unsigned long long>(partition.dynamicRebuilt),
        static_cast<unsigned long long>(partition.runtimeCoveredCandidate),
        static_cast<unsigned long long>(partition.wakeDynamic),
        static_cast<unsigned long long>(partition.wakeAppend),
        static_cast<unsigned long long>(partition.wakeNew),
        static_cast<unsigned long long>(partition.wakeSignature),
        static_cast<unsigned long long>(partition.wakeBinding),
        static_cast<unsigned long long>(partition.wakeOrdered),
        static_cast<unsigned long long>(partition.wakeOverride),
        static_cast<unsigned long long>(partition.wakeFacts),
        static_cast<unsigned long long>(partition.exclusiveAppend),
        static_cast<unsigned long long>(partition.exclusiveNew),
        static_cast<unsigned long long>(partition.exclusiveBinding),
        static_cast<unsigned long long>(partition.exclusiveFacts),
        static_cast<unsigned long long>(partition.exclusiveOverride),
        static_cast<unsigned long long>(partition.exclusiveDynamic),
        static_cast<unsigned long long>(partition.exclusiveOrdered),
        static_cast<unsigned long long>(partition.exclusiveSignature),
        static_cast<unsigned long long>(partition.exclusiveOther),
        static_cast<unsigned long long>(partition.subclassGui),
        static_cast<unsigned long long>(partition.subclassRegisters),
        static_cast<unsigned long long>(partition.subclassTextureMatrix),
        static_cast<unsigned long long>(partition.subclassSpectrum),
        static_cast<unsigned long long>(partition.subclassDecal),
        static_cast<unsigned long long>(partition.subclassVideo),
        static_cast<unsigned long long>(partition.subclassRenderTarget),
        static_cast<unsigned long long>(partition.subclassOther),
        static_cast<unsigned long long>(partition.productRow),
        static_cast<unsigned long long>(partition.productFacts),
        static_cast<unsigned long long>(partition.productFeature),
        static_cast<unsigned long long>(partition.productParameters),
        static_cast<unsigned long long>(partition.productBinding),
        partition.rebuilt == partition.compareIdentical +
                partition.comparePartial + partition.compareError
            ? 1
            : 0,
        partition.rebuilt == exclusiveTotal ? 1 : 0,
        static_cast<unsigned long long>(partition.invariantFailures));
}

void ClearSmokeMaterialTableCache()
{
    ++g_smokeMaterialTableCacheTelemetry.explicitClears;
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
