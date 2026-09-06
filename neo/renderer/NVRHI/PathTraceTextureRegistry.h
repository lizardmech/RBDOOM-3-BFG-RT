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
#include <memory>

struct RtSmokeMaterialTextureInfo
{
    uint32_t materialId = 0;
    idStr materialName;
    idStr diffuseImageName;
    idStr alphaImageName;
    idStr normalImageName;
    idStr specularImageName;
    idStr emissiveImageName;
    idStr skyImageName;
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
    idImage* skyImage = nullptr;
    nvrhi::TextureHandle diffuseTexture;
    nvrhi::TextureHandle alphaTexture;
    nvrhi::TextureHandle normalTexture;
    nvrhi::TextureHandle specularTexture;
    nvrhi::TextureHandle emissiveTexture;
    nvrhi::TextureHandle skyTexture;
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
    bool hasSkyTextureHandle = false;
    bool hasSafeSkyTexture = false;
    bool hardwareOpaqueGeometry = false;
    bool hasAlphaTest = false;
    bool additiveDecal = false;
    bool additiveDecalWhiteKey = false;
    bool filterDecal = false;
    bool filterDecalBlackKey = false;
    bool detailDecal = false;
    bool detailDecalDynamic = false;
    bool detailDecalDiffuseLit = false;
    bool detailDecalLiquidPool = false;
    bool liquidFilmHasBloodSemantic = false;
    bool liquidFilmHasWetReflectStage = false;
    bool liquidFilmHasCoverageSource = false;
    bool liquidFilmHasWetNormalSource = false;
    bool liquidFilmExactOverride = false;
    bool liquidFilmCandidate = false;
    idStr liquidFilmCoverageImageName;
    idStr liquidFilmOverrideReason;
    idStr liquidFilmReason;
    bool isDynamic = true;
    int detailDecalSpectrum = 0;
    bool alphaFromDiffuseLuma = false;
    bool forceFallbackAlbedo = false;
    bool alphaFromDiffuseDarkKey = false;
    bool alphaFromDiffuseMagentaKey = false;
    bool portalWindowFallback = false;
    bool objectGlassFallback = false;
    bool emissive = false;
    bool emissiveLightCandidate = false;
    bool skyEnvironment = false;
    float alphaCutoff = 0.0f;
    idVec4 emissiveColor = idVec4(0.0f, 0.0f, 0.0f, 1.0f);
    idVec4 skyColor = idVec4(1.0f, 1.0f, 1.0f, 1.0f);
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
    // Registry-owned metadata, not authored material facts. Public mutable access
    // permanently requires exact binding checks until this registry is cleared.
    uint64_t bindingDefinitionRevision = 0;
    bool bindingMutableExposed = false;
};

struct RtSmokeMaterialTextureRegistryBumpCounts
{
    uint64 addMaterial = 0;
    uint64 updateVariantFacts = 0;
    uint64 addVariant = 0;
    uint64 clearVariants = 0;
    uint64 clearRegistry = 0;
    uint64 refreshTextureHandles = 0;
};

struct RtSmokeMaterialTextureRegistryBumpStats
{
    bool enabled = false;
    int frameNumber = -1;
    RtSmokeMaterialTextureRegistryBumpCounts thisFrame;
    RtSmokeMaterialTextureRegistryBumpCounts cumulative;
};

struct RtSmokeMaterialActiveTextureRefreshResult
{
    int visited = 0;
    std::vector<uint32_t> changedMaterialIds;
};

struct RtPathTraceMaterialTextureVariantBasePod;
struct RtPathTraceCaptureRegistryMaterialPod;

bool IsSmokeDiffuseTextureSafeForRayTracing(nvrhi::ITexture* texture);
bool IsSmokeImageNameSafeForRayTracing(const char* imageName);
bool IsSmokeImageNameGuiLike(const char* imageName);
bool IsSmokeDiffuseImageSafeForRayTracing(idImage* image);
bool IsSmokeTextureHandleSafeForDescriptor(nvrhi::TextureHandle texture);
bool SmokeTextureHandleListsEqual(const std::vector<nvrhi::TextureHandle>& lhs, const std::vector<nvrhi::TextureHandle>& rhs);
RtSmokeMaterialTextureInfo* FindSmokeMaterialTextureInfo(uint32_t materialId);
const RtSmokeMaterialTextureInfo* FindSmokeMaterialTextureInfoReadOnly(uint32_t materialId);
RtSmokeMaterialTextureInfo& PublishCompleteSmokeMaterialTextureInfo(
    RtSmokeMaterialTextureInfo&& completeInfo);
RtSmokeMaterialTextureInfo& AddSmokeMaterialTextureInfo(uint32_t materialId, const char* materialName);
struct RtSmokeMaterialBindingFrame;
struct RtCpuMaterialBindingPlanInput;
struct RtCpuMaterialBindingPlan;
std::shared_ptr<RtSmokeMaterialBindingFrame> SnapshotSmokeMaterialBindingFrame(RtCpuMaterialBindingPlanInput& input, uint64_t budget,
    const std::vector<uint32_t>* baseIds = nullptr);
// Owner-only. Complete owned variants need resource refresh, not authored discovery.
// Input is bounded to65535 IDs; other IDs retain their order for discovery.
std::vector<uint32_t> PrepareSmokeMaterialHydrationIds(const std::vector<uint32_t>& ids,
    const RtSmokeMaterialBindingFrame* frame = nullptr);
bool SmokeMaterialBindingNeedsPreparation(const RtSmokeMaterialBindingFrame& frame);
// Production demand-loads referenced textures on the owner command list.
// A null list is supported only by the CPU-only registry harness.
bool ResolveSmokeMaterialBindingResources(RtSmokeMaterialBindingFrame& frame,
    nvrhi::ICommandList* commandList = nullptr);
bool CompleteSmokeMaterialBindingFrame(RtSmokeMaterialBindingFrame& frame, RtCpuMaterialBindingPlan&& plan);
bool RegisterSmokeMaterialTextureVariant(uint32_t variantMaterialId, uint32_t baseMaterialId,
    const RtSmokeMaterialBindingFrame* frame = nullptr);
bool IsSmokeMaterialTextureVariant(uint32_t materialId);
uint32_t SmokeMaterialTextureVariantBase(uint32_t materialId);
int ClearSmokeMaterialTextureVariants();
int ClearSmokeMaterialTextureRegistry();
bool RefreshSmokeMaterialTextureHandleState(RtSmokeMaterialTextureInfo& info);
bool RefreshUnpublishedSmokeMaterialTextureHandleState(
    RtSmokeMaterialTextureInfo& info);
RtSmokeMaterialActiveTextureRefreshResult RefreshSmokeMaterialTextureHandlesForActiveIds(
    const std::vector<uint32_t>& staticMaterialIds,
    const std::vector<uint32_t>& dynamicMaterialIds,
    const RtSmokeMaterialBindingFrame* frame = nullptr);
bool SmokeMaterialActiveTextureRefreshSelfTest();
RtSmokeMaterialTextureInfo ResolveSmokeMaterialTextureInfo(uint32_t materialId, int tableIndex);
bool ComputeSmokeMaterialHardwareOpaqueGeometry(
    const RtSmokeMaterialTextureInfo& info);
bool SmokeMaterialCanUseHardwareOpaqueGeometry(uint32_t materialId);
bool SmokeMaterialTextureInfoHasMaterialMetadata(const RtSmokeMaterialTextureInfo& info);
const idStr& SmokeBestSafeTextureName(const RtSmokeMaterialTextureInfo& info);
int SmokeMaterialTextureRegistrySize();
uint64 SmokeMaterialTextureRegistryGeneration();
void EnumerateSmokeMaterialTextureVariantBases(
    std::vector<RtPathTraceMaterialTextureVariantBasePod>& output);
void EnumerateSmokeMaterialTextureRegistryPod(
    std::vector<RtPathTraceCaptureRegistryMaterialPod>& output);
struct RtSmokeMaterialTextureRegistryEnumerationCounts
{
    std::size_t variantBases = 0;
    std::size_t registryMaterials = 0;
};
RtSmokeMaterialTextureRegistryEnumerationCounts
    CountSmokeMaterialTextureRegistryEnumeration();
bool FillSmokeMaterialTextureRegistryEnumerationPreReserved(
    const RtSmokeMaterialTextureRegistryEnumerationCounts& counts,
    std::vector<RtPathTraceMaterialTextureVariantBasePod>& variantBases,
    std::vector<RtPathTraceCaptureRegistryMaterialPod>& registryMaterials);
RtSmokeMaterialTextureRegistryBumpStats SmokeMaterialTextureRegistryBumpStats();

#if defined(RT_PT_TEXTURE_REGISTRY_HARNESS)
bool SmokeMaterialTextureRegistryAtomicPublicationSelfTest();
bool SmokeMaterialBindingDefinitionSelfTest();
#endif

struct RtCpuRewriteMaterialIdentity;
void SnapshotSmokeMaterialIdentities(std::vector<RtCpuRewriteMaterialIdentity>& identities);
