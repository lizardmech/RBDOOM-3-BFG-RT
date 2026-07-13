#pragma once

// Runtime material table and texture-slot state for the RT smoke path.
//
// Captured material ids enter here and become the GPU-facing material table plus
// per-triangle material indexes. The cache is keyed from captured material ids
// and texture registry state; it does not own Doom materials or NVRHI textures.

#include "PathTraceEmissiveCandidates.h"
#include "PathTraceMaterialUniverse.h"
#include "PathTracePrimarySurface.h"

#include <nvrhi/nvrhi.h>

#include <vector>

class idMaterial;
class idImage;

const int RT_SMOKE_TEXTURE_DESCRIPTOR_CAPACITY = 2048;
const int RT_SMOKE_TEXTURE_EXPERIMENTAL_ACTIVE_CAP = 2048;

const uint32_t RT_SMOKE_MATERIAL_ALPHA_TEST = 0x00000001u;
const uint32_t RT_SMOKE_MATERIAL_DIFFUSE_YCOCG = 0x00000002u;
const uint32_t RT_SMOKE_MATERIAL_ADDITIVE_DECAL = 0x00000004u;
const uint32_t RT_SMOKE_MATERIAL_EMISSIVE = 0x00000008u;
const uint32_t RT_SMOKE_MATERIAL_EMISSIVE_LIGHT_CANDIDATE = 0x00020000u;
const uint32_t RT_SMOKE_MATERIAL_SKY_ENVIRONMENT = 0x00040000u;
const uint32_t RT_SMOKE_MATERIAL_FILTER_DECAL = 0x00000010u;
const uint32_t RT_SMOKE_MATERIAL_FILTER_DECAL_BLACK_KEY = 0x00000020u;
const uint32_t RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_LUMA = 0x00000040u;
const uint32_t RT_SMOKE_MATERIAL_FORCE_DEBUG_ALBEDO = 0x00000080u;
const uint32_t RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_DARK_KEY = 0x00000100u;
const uint32_t RT_SMOKE_MATERIAL_PORTAL_WINDOW_FALLBACK = 0x00000200u;
const uint32_t RT_SMOKE_MATERIAL_OBJECT_GLASS_FALLBACK = 0x00000400u;
const uint32_t RT_SMOKE_MATERIAL_ADDITIVE_DECAL_WHITE_KEY = 0x00000800u;
const uint32_t RT_SMOKE_MATERIAL_ALPHA_FROM_DIFFUSE_MAGENTA_KEY = 0x00001000u;
const uint32_t RT_SMOKE_MATERIAL_DETAIL_DECAL = 0x00002000u;
const uint32_t RT_SMOKE_MATERIAL_DETAIL_DECAL_DYNAMIC = 0x00004000u;
const uint32_t RT_SMOKE_MATERIAL_DETAIL_DECAL_DIFFUSE_LIT = 0x00008000u;
const uint32_t RT_SMOKE_MATERIAL_DETAIL_DECAL_LIQUID_POOL = 0x00010000u;
const uint32_t RT_SMOKE_MATERIAL_OVERRIDE_ZERO_ROUGHNESS = 0x00000001u;
const uint32_t RT_SMOKE_MATERIAL_OVERRIDE_FULL_METAL = 0x00001000u;
const uint32_t RT_SMOKE_MATERIAL_CLASSIFIER_DRIVE_LEGACY_SPEC = 0x00000002u;
const uint32_t RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_RUNTIME_REGS = 0x00000004u;
const uint32_t RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_COLOR = 0x00000008u;
const uint32_t RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_ALPHA = 0x00000010u;
const uint32_t RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_CONDITION = 0x00000020u;
const uint32_t RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_TEX_MATRIX = 0x00000040u;
const uint32_t RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_VIDEO = 0x00000080u;
const uint32_t RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_DECAL = 0x00000100u;
const uint32_t RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_GUI_RENDER = 0x00000200u;
const uint32_t RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_PROGRAM = 0x00000400u;
const uint32_t RT_SMOKE_MATERIAL_CLASSIFIER_DYNAMIC_FLIPBOOK = 0x00000800u;
const uint32_t RT_SMOKE_DYNAMIC_MATERIAL_RECORD_VALID = 0x00000001u;
const uint32_t RT_SMOKE_DYNAMIC_MATERIAL_RECORD_STAGE_ENABLED = 0x00000002u;
const uint32_t RT_SMOKE_DYNAMIC_MATERIAL_RECORD_SELECTED_EMISSIVE = 0x00000004u;
const uint32_t RT_SMOKE_DYNAMIC_MATERIAL_RECORD_HAS_TEX_MATRIX = 0x00000008u;
const uint32_t RT_SMOKE_DYNAMIC_MATERIAL_RECORD_HAS_ALPHA_TEST = 0x00000010u;
const uint32_t RT_SMOKE_DYNAMIC_MATERIAL_RECORD_DYNAMIC_IMAGE = 0x00000020u;
const uint32_t RT_SMOKE_DYNAMIC_MATERIAL_RECORD_CINEMATIC = 0x00000040u;
const uint32_t RT_SMOKE_DYNAMIC_MATERIAL_RECORD_GUI_RENDER_TARGET = 0x00000080u;
const uint32_t RT_SMOKE_DYNAMIC_MATERIAL_RECORD_PROGRAM = 0x00000100u;
const uint32_t RT_SMOKE_DYNAMIC_MATERIAL_RECORD_REPLACE_EMISSIVE = 0x00000200u;
const uint32_t RT_SMOKE_DYNAMIC_MATERIAL_RECORD_ORDERED_STAGE_VALUE = 0x00000400u;
const uint32_t RT_SMOKE_DYNAMIC_MATERIAL_RECORD_HAS_ORDERED_STAGE_VALUES = 0x00000800u;
const uint32_t RT_SMOKE_DYNAMIC_MATERIAL_HEADER_SELECTED_STAGE_MASK = 0x000000ffu;
const uint32_t RT_SMOKE_DYNAMIC_MATERIAL_HEADER_STAGE_COUNT_SHIFT = 8u;
const uint32_t RT_SMOKE_DYNAMIC_MATERIAL_HEADER_STAGE_COUNT_MASK = 0x00000f00u;
const uint32_t RT_SMOKE_DYNAMIC_MATERIAL_HEADER_STAGE_OVERFLOW = 0x00001000u;
const uint32_t RT_SMOKE_DYNAMIC_MATERIAL_HEADER_STAGE_OFFSET_SHIFT = 16u;
const uint32_t RT_SMOKE_DYNAMIC_MATERIAL_HEADER_STAGE_OFFSET_MASK = 0xffff0000u;

struct PathTraceDynamicMaterialRecord
{
    float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    float texMatrix0[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    float texMatrix1[4] = { 0.0f, 1.0f, 0.0f, 0.0f };
    uint32_t materialIndex = UINT32_MAX;
    uint32_t materialId = 0;
    uint32_t stageIndex = UINT32_MAX;
    uint32_t flags = 0;
};
static_assert((sizeof(PathTraceDynamicMaterialRecord) % 16) == 0, "PathTraceDynamicMaterialRecord must stay 16-byte aligned for HLSL StructuredBuffer reads");

struct RtSmokeMaterialTableBuild
{
    std::vector<uint32_t> materialIds;
    std::vector<PathTraceSmokeMaterial> materials;
    std::vector<RtSmokeMaterialTextureInfo> materialInfos;
    std::vector<RtSmokeMaterialUniverseFacts> materialFacts;
    std::vector<RtPathTraceMaterialFeatureRecord> materialFeatures;
    std::vector<RtPathTraceMaterialFeatureParameterRecord> materialFeatureParameters;
    std::vector<uint32_t> staticMaterialIndexes;
    std::vector<uint32_t> dynamicMaterialIndexes;
    std::vector<nvrhi::TextureHandle> diffuseTextures;
    int materialsWithTextures = 0;
    int materialsWithNormalTextures = 0;
    int materialsWithSpecularTextures = 0;
    int materialsWithEmissiveTextures = 0;
    int materialsEmissive = 0;
    int materialsMissingTextures = 0;
    int materialsRejectedTextures = 0;
    int materialsRejectedAtFinalCheck = 0;
    int descriptorsReplacedWithFallback = 0;
    int materialsOverTextureSlotLimit = 0;
    int materialsWithAlphaTextures = 0;
    int materialsAlphaTested = 0;
    int materialsAdditiveDecals = 0;
    int guiTextureCandidates = 0;
    int guiTexturesAccepted = 0;
    int guiTexturesRejected = 0;
    int textureProbeRequestedIndex = -1;
    int textureProbeBoundIndex = -1;
    uint32_t textureProbeBoundMaterialId = 0;
    bool textureProbeUsedLatch = false;
};

struct RtSmokeMaterialTableCacheStats
{
    int hits = 0;
    int misses = 0;
};

struct RtSmokeMaterialTableBuildStats
{
    int buildCalls = 0;
    int entryMs = 0;
    int populateMs = 0;
    int resetMs = 0;
    int diagnosticMs = 0;
    int safeOrderMs = 0;
    int descriptorMs = 0;
    int probeMs = 0;
    int tableMaterials = 0;
    int safeMaterials = 0;
    int descriptorTextures = 0;
};

struct RtSmokeMaterialTableCompareStats
{
    int checks = 0;
    int mismatches = 0;
    int materialCountMismatches = 0;
    int materialIdMismatches = 0;
    int materialRecordMismatches = 0;
    int staticIndexMismatches = 0;
    int dynamicIndexMismatches = 0;
    int textureCountMismatches = 0;
    int textureHandleMismatches = 0;
};

int GetSmokeTextureTableRequestedLimit();
int GetSmokeTextureTableEffectiveLimit();
int GetSmokeTextureTableEffectiveLimitWithMinimum(int minimumLimit);
uint32_t HashSmokeMaterialName(const char* materialName);
uint32_t SmokeMaterialId(const idMaterial* material);
bool ValidateSmokeMaterialIndexes(const RtSmokeMaterialTableBuild& table);
bool SmokeMaterialTableIndexIsValid(const RtSmokeMaterialTableBuild& table, int tableIndex);
bool BindSmokeMaterialRuntimeEmissiveTexture(RtSmokeMaterialTableBuild& table, int tableIndex, idImage* image, int minimumTextureTableLimit = 0);
std::vector<int> BuildSmokeSafeMaterialIndexOrder(const RtSmokeMaterialTableBuild& table);
void BuildSmokeMaterialTableFromUniverse(RtSmokeMaterialTableBuild& table, const std::vector<uint32_t>& staticMaterialIds, const std::vector<uint32_t>& dynamicMaterialIds, uint32_t& latchedTextureProbeMaterialId, int& latchedTextureProbeRequestedIndex, bool enableTextureProbe, int minimumTextureTableLimit = 0);
bool BuildSmokeMaterialTableFromUniverseCached(RtSmokeMaterialTableBuild& table, const std::vector<uint32_t>& staticMaterialIds, const std::vector<uint32_t>& dynamicMaterialIds, uint32_t& latchedTextureProbeMaterialId, int& latchedTextureProbeRequestedIndex, bool enableTextureProbe, int minimumTextureTableLimit, uint64& signature, bool& cacheHit);
bool BuildSmokeMaterialTableCached(RtSmokeMaterialTableBuild& table, const std::vector<uint32_t>& staticMaterialIds, const std::vector<uint32_t>& dynamicMaterialIds, uint32_t& latchedTextureProbeMaterialId, int& latchedTextureProbeRequestedIndex, bool enableTextureProbe, int minimumTextureTableLimit, uint64& signature, bool& cacheHit);
RtSmokeMaterialTableCompareStats CompareSmokeMaterialTables(const RtSmokeMaterialTableBuild& expected, const RtSmokeMaterialTableBuild& actual);
RtSmokeMaterialTableBuildStats GetSmokeMaterialTableBuildStats();
RtSmokeMaterialTableCacheStats GetSmokeMaterialTableCacheStats();
void ClearSmokeMaterialTableCache();
void ArmSmokeCrosshairZeroRoughnessToggle();
bool ConsumeSmokeCrosshairZeroRoughnessToggleRequest();
bool ToggleSmokeMaterialZeroRoughnessOverride(uint32_t materialId, const char* materialName);
bool SmokeMaterialHasZeroRoughnessOverride(uint32_t materialId);
int SmokeMaterialZeroRoughnessOverrideCount();
void ArmSmokeCrosshairFullMetalToggle();
bool ConsumeSmokeCrosshairFullMetalToggleRequest();
bool ToggleSmokeMaterialFullMetalOverride(uint32_t materialId, const char* materialName);
bool SmokeMaterialHasFullMetalOverride(uint32_t materialId);
int SmokeMaterialFullMetalOverrideCount();
uint32_t SmokeMaterialOverrideGeneration();
