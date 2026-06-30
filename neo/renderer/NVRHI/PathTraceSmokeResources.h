#pragma once

// NVRHI resource descriptions and helpers for the RT smoke path.
//
// Owns creation/packaging of smoke resources: geometry buffers, binding
// resources, output/readback textures, shader state, and the final commit
// descriptor consumed by PathTracePrimaryPass state.

#include "PathTraceDynamicMaterialState.h"
#include "PathTraceReservoirs.h"
#include "PathTraceRestirPTReservoirs.h"
#include "PathTraceSceneInputs.h"

#include <nvrhi/nvrhi.h>

#include <vector>

struct RtSmokeSceneBufferHandles
{
    nvrhi::BufferHandle staticVertexBuffer;
    nvrhi::BufferHandle staticIndexBuffer;
    nvrhi::BufferHandle staticTriangleClassBuffer;
    nvrhi::BufferHandle staticTriangleMaterialBuffer;
    nvrhi::BufferHandle staticTriangleMaterialIndexBuffer;
    nvrhi::BufferHandle previousStaticVertexBuffer;
    nvrhi::BufferHandle previousStaticIndexBuffer;
    nvrhi::BufferHandle previousStaticTriangleClassBuffer;
    nvrhi::BufferHandle previousStaticTriangleMaterialBuffer;
    nvrhi::BufferHandle previousStaticTriangleMaterialIndexBuffer;
    nvrhi::BufferHandle dynamicVertexBuffer;
    nvrhi::BufferHandle dynamicIndexBuffer;
    nvrhi::BufferHandle dynamicTriangleClassBuffer;
    nvrhi::BufferHandle dynamicTriangleMaterialBuffer;
    nvrhi::BufferHandle dynamicTriangleMaterialIndexBuffer;
    nvrhi::BufferHandle materialTableBuffer;
    nvrhi::BufferHandle dynamicMaterialBuffer;
    nvrhi::BufferHandle emissiveTriangleBuffer;
    nvrhi::BufferHandle previousEmissiveTriangleBuffer;
    nvrhi::BufferHandle emissiveRemapBuffer;
    nvrhi::BufferHandle emissiveDistributionBuffer;
    nvrhi::BufferHandle lightCandidateBuffer;
    nvrhi::BufferHandle doomAnalyticLightBuffer;
    nvrhi::BufferHandle doomAnalyticPreviousLightBuffer;
    nvrhi::BufferHandle doomAnalyticCurrentIdentityBuffer;
    nvrhi::BufferHandle doomAnalyticPreviousIdentityBuffer;
    nvrhi::BufferHandle doomAnalyticRemapBuffer;
    nvrhi::BufferHandle unifiedLightBuffer;
    nvrhi::BufferHandle unifiedPreviousLightBuffer;
    nvrhi::BufferHandle unifiedLightRemapBuffer;
    nvrhi::BufferHandle restirLightManagerCurrentBuffer;
    nvrhi::BufferHandle restirLightManagerPreviousBuffer;
    nvrhi::BufferHandle restirLightManagerCurrentToPreviousBuffer;
    nvrhi::BufferHandle restirLightManagerPreviousToCurrentBuffer;
    nvrhi::BufferHandle restirLightManagerCurrentPayloadBuffer;
    nvrhi::BufferHandle restirLightManagerPreviousPayloadBuffer;
    nvrhi::BufferHandle rigidRouteVertexBuffer;
    nvrhi::BufferHandle rigidRouteIndexBuffer;
    nvrhi::BufferHandle rigidRouteTriangleMaterialBuffer;
    nvrhi::BufferHandle rigidRouteTriangleMaterialIndexBuffer;
    nvrhi::BufferHandle rigidRouteInstanceBuffer;
    nvrhi::BufferHandle skinnedSourceVertexBuffer;
    nvrhi::BufferHandle skinnedCurrentOutputVertexBuffer;
    nvrhi::BufferHandle skinnedPreviousPositionBuffer;
    nvrhi::BufferHandle skinnedSurfaceDispatchBuffer;
    nvrhi::BufferHandle skinnedTriangleDispatchIndexBuffer;
    nvrhi::BufferHandle skinnedCurrentJointMatrixBuffer;
    nvrhi::BufferHandle skinnedPreviousJointMatrixBuffer;

    bool IsValid() const;
};

struct RtSmokeSceneBufferCreateDesc
{
    nvrhi::IDevice* device = nullptr;
    RtSmokeSceneBufferHandles existingBuffers;
    size_t staticVertexBytes = 0;
    size_t staticIndexBytes = 0;
    size_t staticTriangleClassBytes = 0;
    size_t staticTriangleMaterialBytes = 0;
    size_t staticTriangleMaterialIndexBytes = 0;
    size_t previousStaticVertexBytes = 0;
    size_t previousStaticIndexBytes = 0;
    size_t previousStaticTriangleClassBytes = 0;
    size_t previousStaticTriangleMaterialBytes = 0;
    size_t previousStaticTriangleMaterialIndexBytes = 0;
    size_t dynamicVertexBytes = 0;
    size_t dynamicIndexBytes = 0;
    size_t dynamicTriangleClassBytes = 0;
    size_t dynamicTriangleMaterialBytes = 0;
    size_t dynamicTriangleMaterialIndexBytes = 0;
    size_t materialTableBytes = 0;
    size_t dynamicMaterialBytes = 0;
    size_t emissiveTriangleBytes = 0;
    size_t previousEmissiveTriangleBytes = 0;
    size_t emissiveRemapBytes = 0;
    size_t emissiveDistributionBytes = 0;
    size_t lightCandidateBytes = 0;
    size_t doomAnalyticLightBytes = 0;
    size_t doomAnalyticPreviousLightBytes = 0;
    size_t doomAnalyticCurrentIdentityBytes = 0;
    size_t doomAnalyticPreviousIdentityBytes = 0;
    size_t doomAnalyticRemapBytes = 0;
    size_t unifiedLightBytes = 0;
    size_t unifiedPreviousLightBytes = 0;
    size_t unifiedLightRemapBytes = 0;
    size_t restirLightManagerCurrentBytes = 0;
    size_t restirLightManagerPreviousBytes = 0;
    size_t restirLightManagerCurrentToPreviousBytes = 0;
    size_t restirLightManagerPreviousToCurrentBytes = 0;
    size_t restirLightManagerCurrentPayloadBytes = 0;
    size_t restirLightManagerPreviousPayloadBytes = 0;
    size_t rigidRouteVertexBytes = 0;
    size_t rigidRouteIndexBytes = 0;
    size_t rigidRouteTriangleMaterialBytes = 0;
    size_t rigidRouteTriangleMaterialIndexBytes = 0;
    size_t rigidRouteInstanceBytes = 0;
    size_t skinnedSourceVertexBytes = 0;
    size_t skinnedCurrentOutputVertexBytes = 0;
    size_t skinnedPreviousPositionBytes = 0;
    size_t skinnedSurfaceDispatchBytes = 0;
    size_t skinnedTriangleDispatchIndexBytes = 0;
    size_t skinnedCurrentJointMatrixBytes = 0;
    size_t skinnedPreviousJointMatrixBytes = 0;
};

struct RtSmokeSceneBufferCreateResult
{
    RtSmokeSceneBufferHandles buffers;
    const char* errorMessage = nullptr;

    bool Succeeded() const { return buffers.IsValid() && errorMessage == nullptr; }
};

struct RtSmokeBindingBuildDesc
{
    nvrhi::IDevice* device = nullptr;
    nvrhi::rt::AccelStructHandle tlas;
    nvrhi::TextureHandle outputTexture;
    nvrhi::TextureHandle accumulationTexture;
    nvrhi::TextureHandle restirPTReflectionTexture;
    nvrhi::TextureHandle rrInputColorTexture;
    nvrhi::TextureHandle motionVectorTexture;
    nvrhi::TextureHandle rrMotionVectorTexture;
    nvrhi::TextureHandle motionVectorMaskTexture;
    nvrhi::TextureHandle rrGuideAlbedoTexture;
    nvrhi::TextureHandle rrGuideSpecularAlbedoTexture;
    nvrhi::TextureHandle rrGuideNormalRoughnessTexture;
    nvrhi::TextureHandle rrGuideDepthTexture;
    nvrhi::TextureHandle rrGuideHitDistanceTexture;
    nvrhi::TextureHandle rrGuideResetMaskTexture;
    nvrhi::TextureHandle rrGuidePositionTexture;
    nvrhi::TextureHandle fallbackTexture;
    nvrhi::BufferHandle constantsBuffer;
    nvrhi::BufferHandle restirPTConstantsBuffer;
    nvrhi::BufferHandle boundsOverlayLineBuffer;
    nvrhi::BindingLayoutHandle bindingLayout;
    nvrhi::BindingLayoutHandle textureBindlessLayout;
    nvrhi::DescriptorTableHandle existingTextureDescriptorTable;
    const std::vector<nvrhi::TextureHandle>* existingActiveTextureTable = nullptr;
    nvrhi::SamplerHandle sampler;
    RtSmokeSceneBufferHandles buffers;
    RtSmokeReservoirBufferHandles reservoirBuffers;
    RtRestirPTReservoirBufferHandles restirPTReservoirBuffers;
    RtRestirPTReservoirBufferHandles restirPTDiReservoirBuffers;
    RtRestirPTReservoirBufferHandles restirPTGiReservoirBuffers;
    nvrhi::BufferHandle remixRtxdiDiReservoirBuffer;
    RtRestirPTPrimarySurfaceHistoryBufferHandles primarySurfaceHistoryBuffers;
    bool enableTextureProbe = false;
    bool forceFallbackTexture = false;
    bool allowExistingTextureDescriptorTableWrites = false;
    int maxActiveTextures = RT_SMOKE_TEXTURE_EXPERIMENTAL_ACTIVE_CAP;
};

struct RtSmokeBindingBuildResult
{
    nvrhi::BindingSetHandle bindingSet;
    nvrhi::DescriptorTableHandle textureDescriptorTable;
    std::vector<nvrhi::TextureHandle> activeTextureTable;
    bool textureDescriptorTableCreated = false;
    bool textureDescriptorTableWritten = false;
    int textureDescriptorSlotsWritten = 0;
    const char* errorMessage = nullptr;
    int failedTextureSlot = -1;

    bool Succeeded() const { return bindingSet && textureDescriptorTable && errorMessage == nullptr; }
};

struct RtSmokeSceneResourceCommitDesc
{
    RtPathTraceSceneInputs sceneInputs;
    RtSmokeSceneBufferHandles buffers;
    nvrhi::rt::AccelStructDesc staticBlasDesc;
    nvrhi::rt::AccelStructDesc dynamicBlasDesc;
    nvrhi::rt::AccelStructHandle staticBlas;
    nvrhi::rt::AccelStructHandle dynamicBlas;
    nvrhi::rt::AccelStructHandle tlas;
    bool hasStaticBlas = false;
    uint64 staticBlasSignature = 0;
    nvrhi::BindingSetHandle bindingSet;
    nvrhi::DescriptorTableHandle textureDescriptorTable;
    std::vector<nvrhi::TextureHandle> activeTextureTable;
    bool textureDescriptorTableCreated = false;
    bool textureDescriptorTableWritten = false;
    int materialTableEntryCount = 0;
    int emissiveTriangleCount = 0;
    int emissiveStaticTriangleCount = 0;
    int emissiveDynamicTriangleCount = 0;
    int lightCandidateCount = 0;
    int texturedLightCandidateCount = 0;
    int lightCandidateBytes = 0;
    int doomAnalyticLightCount = 0;
    int doomAnalyticPortalRegionLightCount = 0;
    int doomAnalyticLightBytes = 0;
    int doomAnalyticPreviousLightCount = 0;
    int doomAnalyticCurrentIdentityCount = 0;
    int doomAnalyticPreviousIdentityCount = 0;
    int doomAnalyticRemapCount = 0;
    int doomAnalyticInvalidRemapCount = 0;
    int previousEmissiveTriangleCount = 0;
    int unifiedLightCount = 0;
    int unifiedPreviousLightCount = 0;
    int unifiedLightRemapCount = 0;
    int restirLightManagerCurrentPayloadCount = 0;
    int restirLightManagerPreviousPayloadCount = 0;
    uint64 reservoirSceneSignature = 0;
};

struct RtSmokeSceneResourceCommitBuildDesc
{
    RtPathTraceSceneInputs sceneInputs;
    RtSmokeSceneBufferHandles buffers;
    nvrhi::rt::AccelStructDesc staticBlasDesc;
    nvrhi::rt::AccelStructDesc dynamicBlasDesc;
    nvrhi::rt::AccelStructHandle staticBlas;
    nvrhi::rt::AccelStructHandle dynamicBlas;
    nvrhi::rt::AccelStructHandle tlas;
    bool hasStaticBlas = false;
    uint64 staticBlasSignature = 0;
    nvrhi::BindingSetHandle bindingSet;
    nvrhi::DescriptorTableHandle textureDescriptorTable;
    const std::vector<nvrhi::TextureHandle>* activeTextureTable = nullptr;
    bool textureDescriptorTableCreated = false;
    bool textureDescriptorTableWritten = false;
    int materialTableEntryCount = 0;
    int emissiveTriangleCount = 0;
    int emissiveStaticTriangleCount = 0;
    int emissiveDynamicTriangleCount = 0;
    int lightCandidateCount = 0;
    int texturedLightCandidateCount = 0;
    int lightCandidateBytes = 0;
    int doomAnalyticLightCount = 0;
    int doomAnalyticPortalRegionLightCount = 0;
    int doomAnalyticLightBytes = 0;
    int doomAnalyticPreviousLightCount = 0;
    int doomAnalyticCurrentIdentityCount = 0;
    int doomAnalyticPreviousIdentityCount = 0;
    int doomAnalyticRemapCount = 0;
    int doomAnalyticInvalidRemapCount = 0;
    int previousEmissiveTriangleCount = 0;
    int unifiedLightCount = 0;
    int unifiedPreviousLightCount = 0;
    int unifiedLightRemapCount = 0;
    int restirLightManagerCurrentPayloadCount = 0;
    int restirLightManagerPreviousPayloadCount = 0;
    uint64 reservoirSceneSignature = 0;
};

RtSmokeSceneBufferCreateResult CreateSmokeSceneBuffers(const RtSmokeSceneBufferCreateDesc& desc);
RtSmokeBindingBuildResult CreateSmokeBindingResources(const RtSmokeBindingBuildDesc& desc, RtSmokeMaterialTableBuild& materialTable);
RtSmokeSceneResourceCommitDesc CreateSmokeSceneResourceCommitDesc(const RtSmokeSceneResourceCommitBuildDesc& desc);
