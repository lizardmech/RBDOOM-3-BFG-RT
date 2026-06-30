#pragma once

// Frame-persistent texture metadata registry for RT smoke materials.
//
// Material texture discovery writes safe texture handles and rejection reasons
// here. Dynamic material table construction reads the registry to populate
// bindless texture slots without depending directly on Doom image internals.

#include <nvrhi/nvrhi.h>
#include "../Image.h"
#include "../Material.h"
#include <vector>

struct RtSmokeMaterialTextureInfo
{
    uint32_t materialId = 0;
    idStr materialName;
    idStr diffuseImageName;
    idStr alphaImageName;
    idStr normalImageName;
    idStr specularImageName;
    idStr emissiveImageName;
    idStr fallbackReason;
    idStr alphaReason;
    idStr normalReason;
    idStr specularReason;
    idStr emissiveReason;
    idImage* diffuseImage = nullptr;
    idImage* alphaImage = nullptr;
    idImage* normalImage = nullptr;
    idImage* specularImage = nullptr;
    idImage* emissiveImage = nullptr;
    nvrhi::TextureHandle diffuseTexture;
    nvrhi::TextureHandle alphaTexture;
    nvrhi::TextureHandle normalTexture;
    nvrhi::TextureHandle specularTexture;
    nvrhi::TextureHandle emissiveTexture;
    bool hasDiffuseImage = false;
    bool hasAlphaImage = false;
    bool hasNormalImage = false;
    bool hasSpecularImage = false;
    bool hasEmissiveImage = false;
    bool hasTextureHandle = false;
    bool hasAlphaTextureHandle = false;
    bool hasNormalTextureHandle = false;
    bool hasSpecularTextureHandle = false;
    bool hasEmissiveTextureHandle = false;
    bool hasSafeTexture = false;
    bool hasSafeAlphaTexture = false;
    bool hasSafeNormalTexture = false;
    bool hasSafeSpecularTexture = false;
    bool hasSafeEmissiveTexture = false;
    bool hasAlphaTest = false;
    bool additiveDecal = false;
    bool additiveDecalWhiteKey = false;
    bool filterDecal = false;
    bool filterDecalBlackKey = false;
    bool detailDecal = false;
    bool detailDecalDynamic = false;
    bool detailDecalDiffuseLit = false;
    bool detailDecalLiquidPool = false;
    bool isDynamic = true;
    int detailDecalSpectrum = 0;
    bool alphaFromDiffuseLuma = false;
    bool forceFallbackAlbedo = false;
    bool alphaFromDiffuseDarkKey = false;
    bool alphaFromDiffuseMagentaKey = false;
    bool portalWindowFallback = false;
    bool objectGlassFallback = false;
    bool emissive = false;
    float alphaCutoff = 0.0f;
    idVec4 emissiveColor = idVec4(0.0f, 0.0f, 0.0f, 1.0f);
    idVec4 fallbackAlbedo = idVec4(0.0f, 0.0f, 0.0f, 1.0f);
    bool hasFallbackAlbedo = false;
    textureUsage_t diffuseUsage = TD_DEFAULT;
    textureUsage_t alphaUsage = TD_DEFAULT;
    textureUsage_t normalUsage = TD_DEFAULT;
    textureUsage_t specularUsage = TD_DEFAULT;
    textureUsage_t emissiveUsage = TD_DEFAULT;
    textureColor_t diffuseColorFormat = CFM_DEFAULT;
    textureColor_t alphaColorFormat = CFM_DEFAULT;
    textureColor_t normalColorFormat = CFM_DEFAULT;
    textureColor_t specularColorFormat = CFM_DEFAULT;
    textureColor_t emissiveColorFormat = CFM_DEFAULT;
    materialCoverage_t coverage = MC_BAD;
    int tableIndex = -1;
};

bool IsSmokeDiffuseTextureSafeForRayTracing(nvrhi::ITexture* texture);
bool IsSmokeImageNameSafeForRayTracing(const char* imageName);
bool IsSmokeImageNameGuiLike(const char* imageName);
bool IsSmokeDiffuseImageSafeForRayTracing(idImage* image);
bool IsSmokeTextureHandleSafeForDescriptor(nvrhi::TextureHandle texture);
bool SmokeTextureHandleListsEqual(const std::vector<nvrhi::TextureHandle>& lhs, const std::vector<nvrhi::TextureHandle>& rhs);
RtSmokeMaterialTextureInfo* FindSmokeMaterialTextureInfo(uint32_t materialId);
RtSmokeMaterialTextureInfo& AddSmokeMaterialTextureInfo(uint32_t materialId, const char* materialName);
bool RegisterSmokeMaterialTextureVariant(uint32_t variantMaterialId, uint32_t baseMaterialId);
bool IsSmokeMaterialTextureVariant(uint32_t materialId);
uint32_t SmokeMaterialTextureVariantBase(uint32_t materialId);
int ClearSmokeMaterialTextureVariants();
int ClearSmokeMaterialTextureRegistry();
bool RefreshSmokeMaterialTextureHandleState(RtSmokeMaterialTextureInfo& info);
RtSmokeMaterialTextureInfo ResolveSmokeMaterialTextureInfo(uint32_t materialId, int tableIndex);
bool SmokeMaterialTextureInfoHasMaterialMetadata(const RtSmokeMaterialTextureInfo& info);
const idStr& SmokeBestSafeTextureName(const RtSmokeMaterialTextureInfo& info);
int SmokeMaterialTextureRegistrySize();
uint64 SmokeMaterialTextureRegistryGeneration();
