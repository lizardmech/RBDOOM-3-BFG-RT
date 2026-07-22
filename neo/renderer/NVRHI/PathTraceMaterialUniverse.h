#pragma once

// Persistent RT smoke material records.
//
// This layer owns stable per-material facts that can outlive a single frame's
// GPU-facing material table. Per-frame descriptor indexes, visible texture
// bindings, and compact shader table row order remain owned by
// PathTraceDynamicMaterialState.
//
// Mutation is render/main-thread owned. Worker jobs may prepare immutable
// material candidates, but should not mutate the persistent record map directly.

#include "PathTraceEmissiveCandidates.h"
#include "PathTraceTextureRegistry.h"

#include <cstddef>

struct RtSmokeMaterialUniverseFacts
{
    uint32_t materialId = 0;
    uint32_t universeIndex = UINT32_MAX;
    uint32_t materialFlags = 0;
    bool isDynamic = true;
    bool hasFallbackAlbedo = false;
    idVec4 fallbackAlbedo = idVec4(0.0f, 0.0f, 0.0f, 1.0f);
    bool alphaTested = false;
    float alphaCutoff = 0.0f;
    bool diffuseYCoCg = false;
    bool additiveDecal = false;
    bool additiveDecalWhiteKey = false;
    bool filterDecal = false;
    bool filterDecalBlackKey = false;
    bool detailDecal = false;
    bool detailDecalDynamic = false;
    bool detailDecalLiquidPool = false;
    bool liquidFilmHasBloodSemantic = false;
    bool liquidFilmHasWetReflectStage = false;
    bool liquidFilmHasCoverageSource = false;
    bool liquidFilmHasWetNormalSource = false;
    bool liquidFilmExactOverride = false;
    bool liquidFilmCandidate = false;
    bool alphaFromDiffuseLuma = false;
    bool forceFallbackAlbedo = false;
    bool alphaFromDiffuseDarkKey = false;
    bool alphaFromDiffuseMagentaKey = false;
    bool portalWindowFallback = false;
    bool objectGlassFallback = false;
    bool skyEnvironment = false;
    bool emissive = false;
    bool emissiveLightCandidate = false;
    idVec4 emissiveColor = idVec4(0.0f, 0.0f, 0.0f, 1.0f);
    float emissiveLuminance = 0.0f;
    bool hasDiffuseImage = false;
    bool hasSafeDiffuseTexture = false;
    bool hasAlphaImage = false;
    bool hasSafeAlphaTexture = false;
    bool hasNormalImage = false;
    bool hasSafeNormalTexture = false;
    bool hasSpecularImage = false;
    bool hasSafeSpecularTexture = false;
    bool hasEmissiveImage = false;
    bool hasSafeEmissiveTexture = false;
    bool guiTextureCandidate = false;
};

struct RtSmokePersistentMaterialRecord
{
    bool valid = false;
    uint64 signature = 0;
    uint32_t universeIndex = UINT32_MAX;
    PathTraceSmokeMaterial material = {};
    RtSmokeMaterialUniverseFacts facts;
    int additiveDecalContribution = 0;
};

struct RtSmokeMaterialUniverseStats
{
    int records = 0;
    int universeMaterials = 0;
    int hits = 0;
    int misses = 0;
    int rebuilds = 0;
    int signatureChecks = 0;
    int frameHits = 0;
    int frameMisses = 0;
    int frameRebuilds = 0;
    int frameSignatureChecks = 0;
};

void BeginSmokeMaterialUniverseFrame();
void ReserveSmokeMaterialUniverse(size_t expectedMaterialCount);
void ClearSmokeMaterialUniverse();
const RtSmokePersistentMaterialRecord& GetSmokePersistentMaterialRecord(uint32_t materialId, const RtSmokeMaterialTextureInfo& info);
const RtSmokeMaterialUniverseFacts& GetSmokeMaterialUniverseFacts(uint32_t materialId, const RtSmokeMaterialTextureInfo& info);
RtSmokeMaterialUniverseStats GetSmokeMaterialUniverseStats();
