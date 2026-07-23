#include "precompiled.h"
#pragma hdrstop

// Persistent NVRHI resource lifetime for the RT smoke path.
//
// This module creates the long-lived pipeline/output resources and commits each
// successfully built scene into PathTracePrimaryPass state. Scene build code may
// package handles here, but it should not reset or mutate persistent ownership
// directly.

#include "PathTracePrimaryPass.h"
#include "PathTraceCVars.h"
#include "PathTraceCleanRtxdiDiMaterialFeatures.h"
#include "PathTraceDoomLights.h"
#include "PathTraceDynamicMaterialState.h"
#include "PathTraceMaterialFeatureParameters.h"
#include "PathTraceMaterialFeatureBindings.h"
#include "PathTraceMaterialFeaturePasses.h"
#include "PathTraceMaterialUniverse.h"
#include "PathTraceMaterialTextureDiscovery.h"
#include "PathTraceSmokeDispatch.h"
#include "PathTraceSmokeResources.h"
#include "PathTraceTextureRegistry.h"
#include "PathTraceUnifiedLight.h"
#include "../RenderProgs.h"
#include "../../sys/DeviceManager.h"

extern DeviceManager* deviceManager;

bool RtSmokeSceneBufferHandles::IsValid() const
{
    return staticVertexBuffer && staticIndexBuffer && staticTriangleClassBuffer && staticTriangleMaterialBuffer && staticTriangleMaterialIndexBuffer &&
        dynamicVertexBuffer && dynamicIndexBuffer && dynamicTriangleClassBuffer && dynamicTriangleMaterialBuffer && dynamicTriangleMaterialIndexBuffer &&
        materialTableBuffer && materialFeatureBuffer && materialFeatureParameterBuffer && emissiveTriangleBuffer && previousEmissiveTriangleBuffer && emissiveRemapBuffer && emissiveDistributionBuffer && lightCandidateBuffer && doomAnalyticLightBuffer && doomAnalyticPreviousLightBuffer &&
        doomAnalyticCurrentIdentityBuffer && doomAnalyticPreviousIdentityBuffer && doomAnalyticRemapBuffer &&
        unifiedLightBuffer && unifiedPreviousLightBuffer && unifiedLightRemapBuffer &&
        restirLightManagerCurrentToPreviousBuffer && restirLightManagerPreviousToCurrentBuffer &&
        restirLightManagerCurrentPayloadBuffer && restirLightManagerPreviousPayloadBuffer &&
        rigidRouteVertexBuffer && rigidRouteIndexBuffer && rigidRouteTriangleMaterialBuffer && rigidRouteTriangleMaterialIndexBuffer && rigidRouteInstanceBuffer &&
        skinnedPreviousPositionBuffer && skinnedSurfaceDispatchBuffer && skinnedTriangleDispatchIndexBuffer;
}

static uint64_t HashPathTraceTransitionValue(uint64_t hash, uint64_t value)
{
    hash ^= value + 0x9e3779b97f4a7c15ull + (hash << 6) + (hash >> 2);
    return hash;
}

static uint64_t BuildPathTraceSceneTransitionSignature(const RtPathTraceSceneInputs& inputs)
{
    const RtPathTraceSceneInputSignatures& signatures = inputs.signatures;
    const RtPathTraceSceneInputPortalPolicy& portal = inputs.portalPolicy;
    const RtPathTraceSceneInputGeometry& geometry = inputs.geometry;
    const RtPathTraceSceneInputMaterials& materials = inputs.materials;
    const RtPathTraceSceneInputLights& lights = inputs.lights;

    uint64_t hash = 1469598103934665603ull;
    hash = HashPathTraceTransitionValue(hash, static_cast<uint64_t>(inputs.valid ? 1 : 0));
    hash = HashPathTraceTransitionValue(hash, static_cast<uint64_t>(inputs.sceneSource));
    hash = HashPathTraceTransitionValue(hash, signatures.geometryMembership);
    hash = HashPathTraceTransitionValue(hash, signatures.materialTable);
    hash = HashPathTraceTransitionValue(hash, signatures.lightMembership);
    hash = HashPathTraceTransitionValue(hash, signatures.outputResolution);
    hash = HashPathTraceTransitionValue(hash, signatures.reservoirScene);
    hash = HashPathTraceTransitionValue(hash, static_cast<uint64_t>(portal.viewArea));
    hash = HashPathTraceTransitionValue(hash, static_cast<uint64_t>(portal.currentArea));
    hash = HashPathTraceTransitionValue(hash, static_cast<uint64_t>(portal.selectedAreaCount));
    hash = HashPathTraceTransitionValue(hash, static_cast<uint64_t>(portal.rigidSelectedAreaCount));
    hash = HashPathTraceTransitionValue(hash, static_cast<uint64_t>(portal.bruteForceFullMap ? 1 : 0));
    hash = HashPathTraceTransitionValue(hash, static_cast<uint64_t>(portal.staticAreaPreloadSteps));
    hash = HashPathTraceTransitionValue(hash, static_cast<uint64_t>(portal.rigidResidencySteps));
    hash = HashPathTraceTransitionValue(hash, static_cast<uint64_t>(portal.lightAreaSteps));
    hash = HashPathTraceTransitionValue(hash, static_cast<uint64_t>(geometry.staticTriangleCount));
    hash = HashPathTraceTransitionValue(hash, static_cast<uint64_t>(geometry.dynamicTriangleCount));
    hash = HashPathTraceTransitionValue(hash, static_cast<uint64_t>(geometry.rigidRouteInstanceCount));
    hash = HashPathTraceTransitionValue(hash, static_cast<uint64_t>(materials.materialTableEntryCount));
    hash = HashPathTraceTransitionValue(hash, static_cast<uint64_t>(materials.materialFeatureRecordCount));
    hash = HashPathTraceTransitionValue(hash, static_cast<uint64_t>(materials.materialFeatureParameterRecordCount));
    hash = HashPathTraceTransitionValue(hash, static_cast<uint64_t>(materials.dynamicMaterialRecordCount));
    hash = HashPathTraceTransitionValue(hash, static_cast<uint64_t>(materials.materialTableGpuStable ? 1 : 0));
    hash = HashPathTraceTransitionValue(hash, static_cast<uint64_t>(materials.activeTextureCount));
    hash = HashPathTraceTransitionValue(hash, static_cast<uint64_t>(lights.emissiveTriangleCount));
    hash = HashPathTraceTransitionValue(hash, static_cast<uint64_t>(lights.lightCandidateCount));
    hash = HashPathTraceTransitionValue(hash, static_cast<uint64_t>(lights.doomAnalyticLightCount));
    return hash;
}

static bool PathTraceSceneTransitionChanged(const RtPathTraceSceneInputs& previous, const RtPathTraceSceneInputs& next)
{
    return previous.valid && next.valid && BuildPathTraceSceneTransitionSignature(previous) != BuildPathTraceSceneTransitionSignature(next);
}

static bool SmokeTextureTableChanged(const std::vector<nvrhi::TextureHandle>& oldTable, const std::vector<nvrhi::TextureHandle>& newTable)
{
    if (oldTable.size() != newTable.size())
    {
        return true;
    }
    for (size_t textureIndex = 0; textureIndex < oldTable.size(); ++textureIndex)
    {
        if (oldTable[textureIndex] != newTable[textureIndex])
        {
            return true;
        }
    }
    return false;
}

static nvrhi::TextureHandle SmokeTextureDescriptorSlotHandle(const std::vector<nvrhi::TextureHandle>& activeTextureTable, int textureSlot, nvrhi::TextureHandle fallbackTexture)
{
    const int textureTableIndex = textureSlot + 1; // activeTextureTable[0] is the descriptor fallback sentinel.
    if (textureTableIndex >= 0 && textureTableIndex < static_cast<int>(activeTextureTable.size()))
    {
        return activeTextureTable[textureTableIndex] ? activeTextureTable[textureTableIndex] : fallbackTexture;
    }
    return fallbackTexture;
}

static bool SmokeTextureDescriptorSlotChanged(const std::vector<nvrhi::TextureHandle>& oldTable, const std::vector<nvrhi::TextureHandle>& newTable, int textureSlot, nvrhi::TextureHandle fallbackTexture)
{
    return SmokeTextureDescriptorSlotHandle(oldTable, textureSlot, fallbackTexture) !=
        SmokeTextureDescriptorSlotHandle(newTable, textureSlot, fallbackTexture);
}

static bool SmokeSceneBuffersChanged(const RtSmokeSceneBufferHandles& oldBuffers, const RtSmokeSceneBufferHandles& newBuffers)
{
    return
        oldBuffers.staticVertexBuffer != newBuffers.staticVertexBuffer ||
        oldBuffers.staticIndexBuffer != newBuffers.staticIndexBuffer ||
        oldBuffers.staticTriangleClassBuffer != newBuffers.staticTriangleClassBuffer ||
        oldBuffers.staticTriangleMaterialBuffer != newBuffers.staticTriangleMaterialBuffer ||
        oldBuffers.staticTriangleMaterialIndexBuffer != newBuffers.staticTriangleMaterialIndexBuffer ||
        oldBuffers.previousStaticVertexBuffer != newBuffers.previousStaticVertexBuffer ||
        oldBuffers.previousStaticIndexBuffer != newBuffers.previousStaticIndexBuffer ||
        oldBuffers.previousStaticTriangleClassBuffer != newBuffers.previousStaticTriangleClassBuffer ||
        oldBuffers.previousStaticTriangleMaterialBuffer != newBuffers.previousStaticTriangleMaterialBuffer ||
        oldBuffers.previousStaticTriangleMaterialIndexBuffer != newBuffers.previousStaticTriangleMaterialIndexBuffer ||
        oldBuffers.dynamicVertexBuffer != newBuffers.dynamicVertexBuffer ||
        oldBuffers.dynamicIndexBuffer != newBuffers.dynamicIndexBuffer ||
        oldBuffers.dynamicTriangleClassBuffer != newBuffers.dynamicTriangleClassBuffer ||
        oldBuffers.dynamicTriangleMaterialBuffer != newBuffers.dynamicTriangleMaterialBuffer ||
        oldBuffers.dynamicTriangleMaterialIndexBuffer != newBuffers.dynamicTriangleMaterialIndexBuffer ||
        oldBuffers.materialTableBuffer != newBuffers.materialTableBuffer ||
        oldBuffers.materialFeatureBuffer != newBuffers.materialFeatureBuffer ||
        oldBuffers.materialFeatureParameterBuffer != newBuffers.materialFeatureParameterBuffer ||
        oldBuffers.dynamicMaterialBuffer != newBuffers.dynamicMaterialBuffer ||
        oldBuffers.emissiveTriangleBuffer != newBuffers.emissiveTriangleBuffer ||
        oldBuffers.previousEmissiveTriangleBuffer != newBuffers.previousEmissiveTriangleBuffer ||
        oldBuffers.emissiveRemapBuffer != newBuffers.emissiveRemapBuffer ||
        oldBuffers.emissiveDistributionBuffer != newBuffers.emissiveDistributionBuffer ||
        oldBuffers.lightCandidateBuffer != newBuffers.lightCandidateBuffer ||
        oldBuffers.doomAnalyticLightBuffer != newBuffers.doomAnalyticLightBuffer ||
        oldBuffers.doomAnalyticPreviousLightBuffer != newBuffers.doomAnalyticPreviousLightBuffer ||
        oldBuffers.doomAnalyticCurrentIdentityBuffer != newBuffers.doomAnalyticCurrentIdentityBuffer ||
        oldBuffers.doomAnalyticPreviousIdentityBuffer != newBuffers.doomAnalyticPreviousIdentityBuffer ||
        oldBuffers.doomAnalyticRemapBuffer != newBuffers.doomAnalyticRemapBuffer ||
        oldBuffers.unifiedLightBuffer != newBuffers.unifiedLightBuffer ||
        oldBuffers.unifiedPreviousLightBuffer != newBuffers.unifiedPreviousLightBuffer ||
        oldBuffers.unifiedLightRemapBuffer != newBuffers.unifiedLightRemapBuffer ||
        oldBuffers.restirLightManagerCurrentToPreviousBuffer != newBuffers.restirLightManagerCurrentToPreviousBuffer ||
        oldBuffers.restirLightManagerPreviousToCurrentBuffer != newBuffers.restirLightManagerPreviousToCurrentBuffer ||
        oldBuffers.restirLightManagerCurrentPayloadBuffer != newBuffers.restirLightManagerCurrentPayloadBuffer ||
        oldBuffers.restirLightManagerPreviousPayloadBuffer != newBuffers.restirLightManagerPreviousPayloadBuffer ||
        oldBuffers.rigidRouteVertexBuffer != newBuffers.rigidRouteVertexBuffer ||
        oldBuffers.rigidRouteIndexBuffer != newBuffers.rigidRouteIndexBuffer ||
        oldBuffers.rigidRouteTriangleMaterialBuffer != newBuffers.rigidRouteTriangleMaterialBuffer ||
        oldBuffers.rigidRouteTriangleMaterialIndexBuffer != newBuffers.rigidRouteTriangleMaterialIndexBuffer ||
        oldBuffers.rigidRouteInstanceBuffer != newBuffers.rigidRouteInstanceBuffer ||
        oldBuffers.skinnedSourceVertexBuffer != newBuffers.skinnedSourceVertexBuffer ||
        oldBuffers.skinnedCurrentOutputVertexBuffer != newBuffers.skinnedCurrentOutputVertexBuffer ||
        oldBuffers.skinnedPreviousPositionBuffer != newBuffers.skinnedPreviousPositionBuffer ||
        oldBuffers.skinnedSurfaceDispatchBuffer != newBuffers.skinnedSurfaceDispatchBuffer ||
        oldBuffers.skinnedTriangleDispatchIndexBuffer != newBuffers.skinnedTriangleDispatchIndexBuffer ||
        oldBuffers.skinnedCurrentJointMatrixBuffer != newBuffers.skinnedCurrentJointMatrixBuffer ||
        oldBuffers.skinnedPreviousJointMatrixBuffer != newBuffers.skinnedPreviousJointMatrixBuffer;
}

static bool SmokeScenePackageHandlesChanged(const RtRetiredSmokeScenePackage& oldPackage, const RtSmokeSceneResourceCommitDesc& next)
{
    return
        SmokeSceneBuffersChanged(oldPackage.buffers, next.buffers) ||
        oldPackage.staticBlas != next.staticBlas ||
        oldPackage.dynamicBlas != next.dynamicBlas ||
        oldPackage.tlas != next.tlas ||
        oldPackage.bindingSet != next.bindingSet ||
        oldPackage.textureDescriptorTable != next.textureDescriptorTable ||
        oldPackage.skyEnvironmentCube != next.skyEnvironmentCube ||
        oldPackage.skyCubeProbeBindingSet != next.skyCubeProbeBindingSet ||
        SmokeTextureTableChanged(oldPackage.activeTextureTable, next.activeTextureTable);
}

static size_t SmokeBufferRequiredBytes(size_t byteSize, uint32_t structStride)
{
    return byteSize > structStride ? byteSize : structStride;
}

static bool SmokeBufferHasCapacity(nvrhi::BufferHandle buffer, size_t byteSize, uint32_t structStride, bool unorderedAccess)
{
    return buffer &&
        buffer->getDesc().byteSize >= SmokeBufferRequiredBytes(byteSize, structStride) &&
        (!unorderedAccess || buffer->getDesc().canHaveUAVs);
}

static nvrhi::BufferHandle CreateSmokeGeometryBuffer(nvrhi::IDevice* device, const char* debugName, size_t byteSize, uint32_t structStride, bool vertexBuffer, bool indexBuffer, bool accelStructInput, bool unorderedAccess = false)
{
    if (!device)
    {
        return nullptr;
    }

    nvrhi::BufferDesc desc;
    desc.byteSize = SmokeBufferRequiredBytes(byteSize, structStride);
    desc.debugName = debugName;
    desc.structStride = structStride;
    desc.isVertexBuffer = vertexBuffer;
    desc.isIndexBuffer = indexBuffer;
    desc.isAccelStructBuildInput = accelStructInput;
    desc.canHaveUAVs = unorderedAccess;
    desc.initialState = nvrhi::ResourceStates::Common;
    desc.keepInitialState = true;
    return device->createBuffer(desc);
}

static nvrhi::BufferHandle ReuseOrCreateSmokeGeometryBuffer(nvrhi::IDevice* device, nvrhi::BufferHandle existingBuffer, const char* debugName, size_t byteSize, uint32_t structStride, bool vertexBuffer, bool indexBuffer, bool accelStructInput, bool unorderedAccess = false, bool shrinkWhenEmpty = false)
{
    if (SmokeBufferHasCapacity(existingBuffer, byteSize, structStride, unorderedAccess))
    {
        const bool shouldShrinkEmpty =
            shrinkWhenEmpty &&
            byteSize == 0 &&
            existingBuffer->getDesc().byteSize > SmokeBufferRequiredBytes(byteSize, structStride);
        if (!shouldShrinkEmpty)
        {
            return existingBuffer;
        }
    }

    return CreateSmokeGeometryBuffer(device, debugName, byteSize, structStride, vertexBuffer, indexBuffer, accelStructInput, unorderedAccess);
}

static nvrhi::BufferHandle ReuseOrCreateOptionalSmokeGeometryBuffer(nvrhi::IDevice* device, nvrhi::BufferHandle existingBuffer, const char* debugName, size_t byteSize, uint32_t structStride, bool unorderedAccess = false)
{
    if (byteSize == 0)
    {
        return nullptr;
    }
    return ReuseOrCreateSmokeGeometryBuffer(device, existingBuffer, debugName, byteSize, structStride, false, false, false, unorderedAccess);
}

RtSmokeSceneBufferCreateResult CreateSmokeSceneBuffers(const RtSmokeSceneBufferCreateDesc& desc)
{
    RtSmokeSceneBufferCreateResult result;
    result.buffers.staticVertexBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.staticVertexBuffer, "PathTraceSmokeStaticWorldVertices", desc.staticVertexBytes, sizeof(PathTraceSmokeVertex), true, false, true);
    result.buffers.staticIndexBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.staticIndexBuffer, "PathTraceSmokeStaticWorldIndices", desc.staticIndexBytes, sizeof(uint32_t), false, true, true);
    result.buffers.staticTriangleClassBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.staticTriangleClassBuffer, "PathTraceSmokeStaticWorldTriangleClasses", desc.staticTriangleClassBytes, sizeof(uint32_t), false, false, false);
    result.buffers.staticTriangleMaterialBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.staticTriangleMaterialBuffer, "PathTraceSmokeStaticWorldTriangleMaterials", desc.staticTriangleMaterialBytes, sizeof(uint32_t), false, false, false);
    result.buffers.staticTriangleMaterialIndexBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.staticTriangleMaterialIndexBuffer, "PathTraceSmokeStaticWorldTriangleMaterialIndexes", desc.staticTriangleMaterialIndexBytes, sizeof(uint32_t), false, false, false);
    result.buffers.previousStaticVertexBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.previousStaticVertexBuffer, "PathTraceSmokePreviousStaticWorldVertices", desc.previousStaticVertexBytes, sizeof(PathTraceSmokeVertex), false, false, false);
    result.buffers.previousStaticIndexBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.previousStaticIndexBuffer, "PathTraceSmokePreviousStaticWorldIndices", desc.previousStaticIndexBytes, sizeof(uint32_t), false, false, false);
    result.buffers.previousStaticTriangleClassBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.previousStaticTriangleClassBuffer, "PathTraceSmokePreviousStaticWorldTriangleClasses", desc.previousStaticTriangleClassBytes, sizeof(uint32_t), false, false, false);
    result.buffers.previousStaticTriangleMaterialBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.previousStaticTriangleMaterialBuffer, "PathTraceSmokePreviousStaticWorldTriangleMaterials", desc.previousStaticTriangleMaterialBytes, sizeof(uint32_t), false, false, false);
    result.buffers.previousStaticTriangleMaterialIndexBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.previousStaticTriangleMaterialIndexBuffer, "PathTraceSmokePreviousStaticWorldTriangleMaterialIndexes", desc.previousStaticTriangleMaterialIndexBytes, sizeof(uint32_t), false, false, false);
    result.buffers.dynamicVertexBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.dynamicVertexBuffer, "PathTraceSmokeDynamicCandidateVertices", desc.dynamicVertexBytes, sizeof(PathTraceSmokeVertex), true, false, true, true, true);
    result.buffers.dynamicIndexBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.dynamicIndexBuffer, "PathTraceSmokeDynamicCandidateIndices", desc.dynamicIndexBytes, sizeof(uint32_t), false, true, true, false, true);
    result.buffers.dynamicTriangleClassBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.dynamicTriangleClassBuffer, "PathTraceSmokeDynamicCandidateTriangleClasses", desc.dynamicTriangleClassBytes, sizeof(uint32_t), false, false, false, false, true);
    result.buffers.dynamicTriangleMaterialBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.dynamicTriangleMaterialBuffer, "PathTraceSmokeDynamicCandidateTriangleMaterials", desc.dynamicTriangleMaterialBytes, sizeof(uint32_t), false, false, false, false, true);
    result.buffers.dynamicTriangleMaterialIndexBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.dynamicTriangleMaterialIndexBuffer, "PathTraceSmokeDynamicCandidateTriangleMaterialIndexes", desc.dynamicTriangleMaterialIndexBytes, sizeof(uint32_t), false, false, false, false, true);
    result.buffers.materialTableBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.materialTableBuffer, "PathTraceSmokeMaterialTable", desc.materialTableBytes, sizeof(PathTraceSmokeMaterial), false, false, false);
    result.buffers.materialFeatureBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.materialFeatureBuffer, "PathTraceMaterialFeatureRecords", desc.materialFeatureBytes, sizeof(RtPathTraceMaterialFeatureRecord), false, false, false);
    result.buffers.materialFeatureParameterBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.materialFeatureParameterBuffer, "PathTraceMaterialFeatureParameters", desc.materialFeatureParameterBytes, sizeof(RtPathTraceMaterialFeatureParameterRecord), false, false, false);
    result.buffers.dynamicMaterialBuffer = ReuseOrCreateOptionalSmokeGeometryBuffer(desc.device, desc.existingBuffers.dynamicMaterialBuffer, "PathTraceDynamicMaterialRecords", desc.dynamicMaterialBytes, sizeof(PathTraceDynamicMaterialRecord));
    result.buffers.emissiveTriangleBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.emissiveTriangleBuffer, "PathTraceSmokeEmissiveTriangles", desc.emissiveTriangleBytes, sizeof(PathTraceSmokeEmissiveTriangle), false, false, false);
    result.buffers.previousEmissiveTriangleBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.previousEmissiveTriangleBuffer, "PathTraceSmokePreviousEmissiveTriangles", desc.previousEmissiveTriangleBytes, sizeof(PathTraceSmokeEmissiveTriangle), false, false, false);
    result.buffers.emissiveRemapBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.emissiveRemapBuffer, "PathTraceSmokeEmissiveRemap", desc.emissiveRemapBytes, sizeof(PathTraceEmissiveLightRemap), false, false, false);
    result.buffers.emissiveDistributionBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.emissiveDistributionBuffer, "PathTraceEmissiveDistribution", desc.emissiveDistributionBytes, sizeof(PathTraceEmissiveDistributionEntry), false, false, false);
    result.buffers.lightCandidateBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.lightCandidateBuffer, "PathTraceSmokeLightCandidates", desc.lightCandidateBytes, sizeof(PathTraceSmokeLightCandidate), false, false, false);
    result.buffers.doomAnalyticLightBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.doomAnalyticLightBuffer, "PathTraceDoomAnalyticLights", desc.doomAnalyticLightBytes, sizeof(PathTraceDoomAnalyticLightCandidate), false, false, false);
    result.buffers.doomAnalyticPreviousLightBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.doomAnalyticPreviousLightBuffer, "PathTraceDoomAnalyticPreviousLights", desc.doomAnalyticPreviousLightBytes, sizeof(PathTraceDoomAnalyticLightCandidate), false, false, false);
    result.buffers.doomAnalyticCurrentIdentityBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.doomAnalyticCurrentIdentityBuffer, "PathTraceDoomAnalyticCurrentIdentities", desc.doomAnalyticCurrentIdentityBytes, sizeof(PathTraceDoomAnalyticLightCandidateIdentity), false, false, false);
    result.buffers.doomAnalyticPreviousIdentityBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.doomAnalyticPreviousIdentityBuffer, "PathTraceDoomAnalyticPreviousIdentities", desc.doomAnalyticPreviousIdentityBytes, sizeof(PathTraceDoomAnalyticLightCandidateIdentity), false, false, false);
    result.buffers.doomAnalyticRemapBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.doomAnalyticRemapBuffer, "PathTraceDoomAnalyticRemap", desc.doomAnalyticRemapBytes, sizeof(PathTraceDoomAnalyticLightRemap), false, false, false);
    result.buffers.unifiedLightBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.unifiedLightBuffer, "PathTraceUnifiedLights", desc.unifiedLightBytes, sizeof(PathTraceUnifiedLightRecord), false, false, false);
    result.buffers.unifiedPreviousLightBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.unifiedPreviousLightBuffer, "PathTraceUnifiedPreviousLights", desc.unifiedPreviousLightBytes, sizeof(PathTraceUnifiedLightRecord), false, false, false);
    result.buffers.unifiedLightRemapBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.unifiedLightRemapBuffer, "PathTraceUnifiedLightRemap", desc.unifiedLightRemapBytes, sizeof(uint32_t), false, false, false);
    result.buffers.restirLightManagerCurrentToPreviousBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.restirLightManagerCurrentToPreviousBuffer, "PathTraceRestirLightManagerCurrentToPrevious", desc.restirLightManagerCurrentToPreviousBytes, sizeof(uint32_t), false, false, false);
    result.buffers.restirLightManagerPreviousToCurrentBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.restirLightManagerPreviousToCurrentBuffer, "PathTraceRestirLightManagerPreviousToCurrent", desc.restirLightManagerPreviousToCurrentBytes, sizeof(uint32_t), false, false, false);
    result.buffers.restirLightManagerCurrentPayloadBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.restirLightManagerCurrentPayloadBuffer, "PathTraceRestirLightManagerCurrentPayload", desc.restirLightManagerCurrentPayloadBytes, sizeof(PathTraceUnifiedLightRecord), false, false, false);
    result.buffers.restirLightManagerPreviousPayloadBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.restirLightManagerPreviousPayloadBuffer, "PathTraceRestirLightManagerPreviousPayload", desc.restirLightManagerPreviousPayloadBytes, sizeof(PathTraceUnifiedLightRecord), false, false, false);
    result.buffers.rigidRouteVertexBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.rigidRouteVertexBuffer, "PathTraceRigidRouteVertices", desc.rigidRouteVertexBytes, sizeof(PathTraceSmokeVertex), false, false, false);
    result.buffers.rigidRouteIndexBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.rigidRouteIndexBuffer, "PathTraceRigidRouteIndices", desc.rigidRouteIndexBytes, sizeof(uint32_t), false, false, false);
    result.buffers.rigidRouteTriangleMaterialBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.rigidRouteTriangleMaterialBuffer, "PathTraceRigidRouteTriangleMaterials", desc.rigidRouteTriangleMaterialBytes, sizeof(uint32_t), false, false, false);
    result.buffers.rigidRouteTriangleMaterialIndexBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.rigidRouteTriangleMaterialIndexBuffer, "PathTraceRigidRouteTriangleMaterialIndexes", desc.rigidRouteTriangleMaterialIndexBytes, sizeof(uint32_t), false, false, false);
    result.buffers.rigidRouteInstanceBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.rigidRouteInstanceBuffer, "PathTraceRigidRouteInstances", desc.rigidRouteInstanceBytes, sizeof(PathTraceRigidRouteInstance), false, false, false);
    result.buffers.skinnedSourceVertexBuffer = ReuseOrCreateOptionalSmokeGeometryBuffer(desc.device, desc.existingBuffers.skinnedSourceVertexBuffer, "PathTraceSkinnedSourceVertices", desc.skinnedSourceVertexBytes, sizeof(PathTraceSkinnedSourceVertex));
    result.buffers.skinnedCurrentOutputVertexBuffer = ReuseOrCreateOptionalSmokeGeometryBuffer(desc.device, desc.existingBuffers.skinnedCurrentOutputVertexBuffer, "PathTraceSkinnedCurrentOutputVertices", desc.skinnedCurrentOutputVertexBytes, sizeof(PathTraceSmokeVertex), true);
    result.buffers.skinnedPreviousPositionBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.skinnedPreviousPositionBuffer, "PathTraceSkinnedPreviousPositions", desc.skinnedPreviousPositionBytes, sizeof(PathTraceSkinnedPreviousPosition), false, false, false, true, true);
    result.buffers.skinnedSurfaceDispatchBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.skinnedSurfaceDispatchBuffer, "PathTraceSkinnedSurfaceDispatch", desc.skinnedSurfaceDispatchBytes, sizeof(PathTraceSkinnedSurfaceDispatchRecord), false, false, false, false, true);
    result.buffers.skinnedTriangleDispatchIndexBuffer = ReuseOrCreateSmokeGeometryBuffer(desc.device, desc.existingBuffers.skinnedTriangleDispatchIndexBuffer, "PathTraceSkinnedTriangleDispatchIndex", desc.skinnedTriangleDispatchIndexBytes, sizeof(uint32_t), false, false, false, false, true);
    result.buffers.skinnedCurrentJointMatrixBuffer = ReuseOrCreateOptionalSmokeGeometryBuffer(desc.device, desc.existingBuffers.skinnedCurrentJointMatrixBuffer, "PathTraceSkinnedCurrentJointMatrices", desc.skinnedCurrentJointMatrixBytes, sizeof(PathTraceSkinnedJointMatrix));
    result.buffers.skinnedPreviousJointMatrixBuffer = ReuseOrCreateOptionalSmokeGeometryBuffer(desc.device, desc.existingBuffers.skinnedPreviousJointMatrixBuffer, "PathTraceSkinnedPreviousJointMatrices", desc.skinnedPreviousJointMatrixBytes, sizeof(PathTraceSkinnedJointMatrix));

    if (!result.buffers.IsValid())
    {
        result.errorMessage = "failed to create RT smoke geometry buffers";
    }
    return result;
}

RtSmokeBindingBuildResult CreateSmokeBindingResources(const RtSmokeBindingBuildDesc& desc, RtSmokeMaterialTableBuild& materialTable)
{
    OPTICK_EVENT("PT Binding Resources Detail");

    RtSmokeBindingBuildResult result;
    result.textureDescriptorTable = desc.existingTextureDescriptorTable;

    if (!desc.device || !desc.bindingLayout || !desc.tlas || !desc.outputTexture || !desc.accumulationTexture || !desc.restirPTReflectionTexture || !desc.rrInputColorTexture || !desc.motionVectorTexture || !desc.rrMotionVectorTexture || !desc.motionVectorMaskTexture || !desc.rrGuideAlbedoTexture || !desc.rrGuideSpecularAlbedoTexture || !desc.rrGuideNormalRoughnessTexture || !desc.rrGuideDepthTexture || !desc.rrGuideHitDistanceTexture || !desc.rrGuideResetMaskTexture || !desc.rrGuidePositionTexture || !desc.fallbackTexture || !desc.skyEnvironmentCube || !desc.constantsBuffer || !desc.boundsOverlayLineBuffer || !desc.sampler || !desc.buffers.IsValid() || !desc.primarySurfaceHistoryBuffers.IsValidFor(desc.primarySurfaceHistoryBuffers.width, desc.primarySurfaceHistoryBuffers.height))
    {
        result.errorMessage = "failed to create RT smoke binding set";
        return result;
    }

    nvrhi::BindingSetDesc bindingSetDesc;
    {
        OPTICK_EVENT("PT Binding Set Desc");
        bindingSetDesc.bindings = {
            nvrhi::BindingSetItem::RayTracingAccelStruct(0, desc.tlas),
            nvrhi::BindingSetItem::Texture_UAV(1, desc.outputTexture),
            nvrhi::BindingSetItem::ConstantBuffer(2, desc.constantsBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(3, desc.buffers.staticVertexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(4, desc.buffers.staticIndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(5, desc.buffers.staticTriangleClassBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(6, desc.buffers.dynamicVertexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(7, desc.buffers.dynamicIndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(8, desc.buffers.dynamicTriangleClassBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(9, desc.buffers.staticTriangleMaterialBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(10, desc.buffers.dynamicTriangleMaterialBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(11, desc.buffers.staticTriangleMaterialIndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(12, desc.buffers.dynamicTriangleMaterialIndexBuffer),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(13, desc.buffers.materialTableBuffer)
        };
    }

    result.activeTextureTable.push_back(desc.fallbackTexture);
    if (desc.enableTextureProbe)
    {
        if (!result.textureDescriptorTable)
        {
            OPTICK_EVENT("PT Create Texture Descriptor Table");
            result.textureDescriptorTable = desc.device->createDescriptorTable(desc.textureBindlessLayout);
            result.textureDescriptorTableCreated = result.textureDescriptorTable != nullptr;
        }
        if (!result.textureDescriptorTable)
        {
            result.errorMessage = "failed to create RT smoke texture descriptor table";
            return result;
        }

        const int textureSlotCount = idMath::ClampInt(1, desc.maxActiveTextures, Max(static_cast<int>(materialTable.diffuseTextures.size()), 1));
        result.activeTextureTable.reserve(textureSlotCount + 1);
        {
            OPTICK_EVENT("PT Build Active Texture Table");
            for (int textureSlot = 0; textureSlot < textureSlotCount; ++textureSlot)
            {
                nvrhi::TextureHandle texture = desc.fallbackTexture;
                if (!desc.forceFallbackTexture && textureSlot >= 0 && textureSlot < static_cast<int>(materialTable.diffuseTextures.size()))
                {
                    const nvrhi::TextureHandle candidateTexture = materialTable.diffuseTextures[textureSlot];
                    if (candidateTexture)
                    {
                        texture = candidateTexture;
                    }
                }
                if (!IsSmokeTextureHandleSafeForDescriptor(texture))
                {
                    ++materialTable.descriptorsReplacedWithFallback;
                    texture = desc.fallbackTexture;
                }

                result.activeTextureTable.push_back(texture);
            }
        }

        const bool textureDescriptorTableReused =
            desc.existingTextureDescriptorTable &&
            result.textureDescriptorTable == desc.existingTextureDescriptorTable &&
            !result.textureDescriptorTableCreated;
        const bool skipTextureDescriptorTableWrite =
            textureDescriptorTableReused &&
            desc.existingActiveTextureTable &&
            !SmokeTextureTableChanged(*desc.existingActiveTextureTable, result.activeTextureTable);
        if (textureDescriptorTableReused &&
            !skipTextureDescriptorTableWrite &&
            !desc.allowExistingTextureDescriptorTableWrites)
        {
            OPTICK_EVENT("PT Create Retired-Safe Texture Descriptor Table");
            result.textureDescriptorTable = desc.device->createDescriptorTable(desc.textureBindlessLayout);
            result.textureDescriptorTableCreated = result.textureDescriptorTable != nullptr;
            if (!result.textureDescriptorTable)
            {
                result.errorMessage = "failed to create RT smoke texture descriptor table";
                return result;
            }
        }
        const bool writeTextureDescriptorTableReused =
            desc.existingTextureDescriptorTable &&
            result.textureDescriptorTable == desc.existingTextureDescriptorTable &&
            !result.textureDescriptorTableCreated;
        const int textureDescriptorSlotCapacity = idMath::ClampInt(0, RT_SMOKE_TEXTURE_DESCRIPTOR_CAPACITY, desc.maxActiveTextures);
        const bool sparseTextureDescriptorWrite =
            !skipTextureDescriptorTableWrite &&
            writeTextureDescriptorTableReused &&
            desc.allowExistingTextureDescriptorTableWrites &&
            desc.existingActiveTextureTable;
        std::vector<int> textureDescriptorSlotsToWrite;
        if (sparseTextureDescriptorWrite)
        {
            OPTICK_EVENT("PT Diff Texture Descriptor Table");
            for (int textureSlot = 0; textureSlot < textureDescriptorSlotCapacity; ++textureSlot)
            {
                if (SmokeTextureDescriptorSlotChanged(*desc.existingActiveTextureTable, result.activeTextureTable, textureSlot, desc.fallbackTexture))
                {
                    textureDescriptorSlotsToWrite.push_back(textureSlot);
                }
            }
        }
        const int textureDescriptorSlotWriteCount =
            skipTextureDescriptorTableWrite
                ? 0
                : (sparseTextureDescriptorWrite ? static_cast<int>(textureDescriptorSlotsToWrite.size()) : textureDescriptorSlotCapacity);
        {
            OPTICK_EVENT("PT Write Texture Descriptor Table");
            result.textureDescriptorTableWritten = textureDescriptorSlotWriteCount > 0;
            result.textureDescriptorSlotsWritten = textureDescriptorSlotWriteCount;
            if (sparseTextureDescriptorWrite)
            {
                for (int textureSlot : textureDescriptorSlotsToWrite)
                {
                    const nvrhi::TextureHandle texture = SmokeTextureDescriptorSlotHandle(result.activeTextureTable, textureSlot, desc.fallbackTexture);
                    if (!desc.device->writeDescriptorTable(result.textureDescriptorTable, nvrhi::BindingSetItem::Texture_SRV(textureSlot, texture)))
                    {
                        result.errorMessage = "failed to write RT smoke bindless texture descriptor slot";
                        result.failedTextureSlot = textureSlot;
                        return result;
                    }
                }
            }
            else
            {
                for (int textureSlot = 0; textureSlot < textureDescriptorSlotWriteCount; ++textureSlot)
                {
                    const nvrhi::TextureHandle texture = SmokeTextureDescriptorSlotHandle(result.activeTextureTable, textureSlot, desc.fallbackTexture);
                    if (!desc.device->writeDescriptorTable(result.textureDescriptorTable, nvrhi::BindingSetItem::Texture_SRV(textureSlot, texture)))
                    {
                        result.errorMessage = "failed to write RT smoke bindless texture descriptor slot";
                        result.failedTextureSlot = textureSlot;
                        return result;
                    }
                }
            }
        }
    }

    if (!result.textureDescriptorTable)
    {
        OPTICK_EVENT("PT Create Empty Texture Descriptor Table");
        result.textureDescriptorTable = desc.device->createDescriptorTable(desc.textureBindlessLayout);
        result.textureDescriptorTableCreated = result.textureDescriptorTable != nullptr;
    }
    if (!result.textureDescriptorTable)
    {
        result.errorMessage = "failed to create RT smoke texture descriptor table";
        return result;
    }

    {
        OPTICK_EVENT("PT Final Binding Set Items");
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(14, desc.fallbackTexture));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(15, desc.accumulationTexture));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(16, desc.buffers.emissiveTriangleBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(57, desc.buffers.previousEmissiveTriangleBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(58, desc.buffers.emissiveRemapBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(46, desc.buffers.emissiveDistributionBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(17, desc.buffers.lightCandidateBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(21, desc.boundsOverlayLineBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(22, desc.buffers.rigidRouteVertexBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(23, desc.buffers.rigidRouteIndexBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(24, desc.buffers.rigidRouteTriangleMaterialBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(25, desc.buffers.rigidRouteTriangleMaterialIndexBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(26, desc.buffers.rigidRouteInstanceBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(27, desc.buffers.doomAnalyticLightBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(45, desc.buffers.doomAnalyticPreviousLightBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(30, desc.primarySurfaceHistoryBuffers.current));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(31, desc.primarySurfaceHistoryBuffers.previous));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(32, desc.buffers.skinnedPreviousPositionBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(33, desc.buffers.skinnedSurfaceDispatchBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(34, desc.buffers.previousStaticVertexBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(35, desc.buffers.previousStaticIndexBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(36, desc.buffers.previousStaticTriangleClassBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(37, desc.buffers.previousStaticTriangleMaterialBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(38, desc.buffers.previousStaticTriangleMaterialIndexBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(42, desc.buffers.doomAnalyticCurrentIdentityBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(43, desc.buffers.doomAnalyticPreviousIdentityBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(44, desc.buffers.doomAnalyticRemapBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(59, desc.buffers.unifiedLightBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(60, desc.buffers.unifiedPreviousLightBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(61, desc.buffers.unifiedLightRemapBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(64, desc.buffers.restirLightManagerCurrentToPreviousBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(65, desc.buffers.restirLightManagerPreviousToCurrentBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(66, desc.buffers.restirLightManagerCurrentPayloadBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(67, desc.buffers.restirLightManagerPreviousPayloadBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(74, desc.buffers.lightCandidateBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(75, desc.buffers.lightCandidateBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(77, desc.buffers.lightCandidateBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(41, desc.buffers.skinnedTriangleDispatchIndexBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_SRV(76, desc.buffers.dynamicMaterialBuffer ? desc.buffers.dynamicMaterialBuffer : desc.buffers.lightCandidateBuffer));
        RtPathTraceMaterialFeatureInputResources materialFeatureInputs;
        materialFeatureInputs.materialFeatureBuffer = desc.buffers.materialFeatureBuffer;
        materialFeatureInputs.materialFeatureParameterBuffer = desc.buffers.materialFeatureParameterBuffer;
        AddPathTraceMaterialFeatureInputBinding(
            bindingSetDesc,
            materialFeatureInputs,
            RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_SIDECAR);
        AddPathTraceMaterialFeatureInputBinding(
            bindingSetDesc,
            materialFeatureInputs,
            RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_PARAMETERS);
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(39, desc.motionVectorTexture));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(40, desc.motionVectorMaskTexture));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(47, desc.restirPTReflectionTexture));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(48, desc.rrGuideAlbedoTexture));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(49, desc.rrGuideNormalRoughnessTexture));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(50, desc.rrGuideDepthTexture));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(51, desc.rrGuideHitDistanceTexture));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(52, desc.rrGuideResetMaskTexture));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(53, desc.rrGuideSpecularAlbedoTexture));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(54, desc.rrInputColorTexture));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(78, desc.rrMotionVectorTexture));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_UAV(79, desc.rrGuidePositionTexture));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::StructuredBuffer_UAV(82, desc.liquidPoolStatusBuffer));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Texture_SRV(
            126,
            desc.skyEnvironmentCube,
            nvrhi::Format::UNKNOWN,
            nvrhi::AllSubresources,
            nvrhi::TextureDimension::TextureCube));
        bindingSetDesc.addItem(nvrhi::BindingSetItem::Sampler(0, desc.sampler));
    }

    {
        OPTICK_EVENT("PT Create Binding Set");
        result.bindingSet = desc.device->createBindingSet(bindingSetDesc, desc.bindingLayout);
    }
    if (!result.bindingSet)
    {
        result.errorMessage = "failed to create RT smoke binding set";
    }

    return result;
}

RtSmokeSceneResourceCommitDesc CreateSmokeSceneResourceCommitDesc(const RtSmokeSceneResourceCommitBuildDesc& desc)
{
    RtSmokeSceneResourceCommitDesc commitDesc;
    commitDesc.sceneInputs = desc.sceneInputs;
    commitDesc.buffers = desc.buffers;
    commitDesc.staticBlasDesc = desc.staticBlasDesc;
    commitDesc.staticBlas = desc.staticBlas;
    commitDesc.dynamicBlas = desc.dynamicBlas;
    commitDesc.tlas = desc.tlas;
    commitDesc.hasStaticBlas = desc.hasStaticBlas;
    commitDesc.staticBlasSignature = desc.staticBlasSignature;
    commitDesc.staticBlasGeometryGeneration = desc.staticBlasGeometryGeneration;
    commitDesc.bindingSet = desc.bindingSet;
    commitDesc.textureDescriptorTable = desc.textureDescriptorTable;
    commitDesc.skyEnvironmentCube = desc.skyEnvironmentCube;
    commitDesc.skyCubeProbeBindingSet = desc.skyCubeProbeBindingSet;
    commitDesc.textureDescriptorTableCreated = desc.textureDescriptorTableCreated;
    commitDesc.textureDescriptorTableWritten = desc.textureDescriptorTableWritten;
    if (desc.activeTextureTable)
    {
        commitDesc.activeTextureTable = *desc.activeTextureTable;
    }
    commitDesc.materialTableEntryCount = desc.materialTableEntryCount;
    commitDesc.emissiveTriangleCount = desc.emissiveTriangleCount;
    commitDesc.emissiveStaticTriangleCount = desc.emissiveStaticTriangleCount;
    commitDesc.lightCandidateCount = desc.lightCandidateCount;
    commitDesc.doomAnalyticLightCount = desc.doomAnalyticLightCount;
    commitDesc.doomAnalyticPortalRegionLightCount = desc.doomAnalyticPortalRegionLightCount;
    commitDesc.doomAnalyticPreviousLightCount = desc.doomAnalyticPreviousLightCount;
    commitDesc.doomAnalyticCurrentIdentityCount = desc.doomAnalyticCurrentIdentityCount;
    commitDesc.doomAnalyticPreviousIdentityCount = desc.doomAnalyticPreviousIdentityCount;
    commitDesc.doomAnalyticRemapCount = desc.doomAnalyticRemapCount;
    commitDesc.previousEmissiveTriangleCount = desc.previousEmissiveTriangleCount;
    commitDesc.unifiedLightCount = desc.unifiedLightCount;
    commitDesc.unifiedPreviousLightCount = desc.unifiedPreviousLightCount;
    commitDesc.unifiedLightRemapCount = desc.unifiedLightRemapCount;
    commitDesc.restirLightManagerCurrentPayloadCount = desc.restirLightManagerCurrentPayloadCount;
    commitDesc.restirLightManagerPreviousPayloadCount = desc.restirLightManagerPreviousPayloadCount;
    return commitDesc;
}

static bool LoadPathTraceSmokeShaderLibrary(nvrhi::IDevice* device, const char* shaderPath, const char* label, nvrhi::ShaderLibraryHandle& shaderLibrary)
{
    shaderLibrary = nullptr;

    void* shaderData = nullptr;
    ID_TIME_T shaderTimestamp = 0;
    const int shaderSize = fileSystem->ReadFile(shaderPath, &shaderData, &shaderTimestamp);
    if (shaderSize <= 0 || !shaderData)
    {
        common->Printf("PathTracePrimaryPass: couldn't read %s RT smoke shader %s\n", label, shaderPath);
        return false;
    }

    common->Printf("PathTracePrimaryPass: loaded %s RT smoke shader %s (%d bytes, timestamp %u)\n",
        label, shaderPath, shaderSize, static_cast<unsigned int>(shaderTimestamp));

    shaderLibrary = device->createShaderLibrary(shaderData, shaderSize);
    Mem_Free(shaderData);

    if (!shaderLibrary)
    {
        common->Printf("PathTracePrimaryPass: failed to create %s RT smoke shader library\n", label);
        return false;
    }

    return true;
}

static bool CreatePathTraceSmokeRayTracingPipeline(
    nvrhi::IDevice* device,
    nvrhi::ShaderLibraryHandle shaderLibrary,
    nvrhi::BindingLayoutHandle bindingLayout,
    nvrhi::BindingLayoutHandle textureBindlessLayout,
    const char* label,
    nvrhi::rt::PipelineHandle& pipeline,
    nvrhi::rt::ShaderTableHandle& shaderTable)
{
    pipeline = nullptr;
    shaderTable = nullptr;

    if (!shaderLibrary)
    {
        common->Printf("PathTracePrimaryPass: cannot create %s RT smoke pipeline without a shader library\n", label);
        return false;
    }

    nvrhi::ShaderHandle rayGen = shaderLibrary->getShader("RayGen", nvrhi::ShaderType::RayGeneration);
    nvrhi::ShaderHandle miss = shaderLibrary->getShader("Miss", nvrhi::ShaderType::Miss);
    nvrhi::ShaderHandle shadowMiss = shaderLibrary->getShader("ShadowMiss", nvrhi::ShaderType::Miss);
    nvrhi::ShaderHandle closestHit = shaderLibrary->getShader("ClosestHit", nvrhi::ShaderType::ClosestHit);
    nvrhi::ShaderHandle anyHit = shaderLibrary->getShader("AnyHit", nvrhi::ShaderType::AnyHit);
    nvrhi::ShaderHandle shadowClosestHit = shaderLibrary->getShader("ShadowClosestHit", nvrhi::ShaderType::ClosestHit);
    nvrhi::ShaderHandle shadowAnyHit = shaderLibrary->getShader("ShadowAnyHit", nvrhi::ShaderType::AnyHit);

    if (!rayGen || !miss || !shadowMiss || !closestHit || !anyHit || !shadowClosestHit || !shadowAnyHit)
    {
        common->Printf("PathTracePrimaryPass: %s RT smoke shader library is missing one or more required entry points\n", label);
        return false;
    }

    nvrhi::rt::PipelineDesc pipelineDesc;
    pipelineDesc.globalBindingLayouts = { bindingLayout, textureBindlessLayout };
    pipelineDesc.shaders = {
        { "", rayGen, nullptr },
        { "", miss, nullptr },
        { "", shadowMiss, nullptr }
    };
    pipelineDesc.hitGroups = {
        {
            "HitGroup",
            closestHit,
            anyHit,
            nullptr,
            nullptr,
            false
        },
        {
            "ShadowHitGroup",
            shadowClosestHit,
            shadowAnyHit,
            nullptr,
            nullptr,
            false
        }
    };
    pipelineDesc.maxPayloadSize = 64;
    pipelineDesc.maxAttributeSize = 8;
    pipelineDesc.maxRecursionDepth = 1;

    pipeline = device->createRayTracingPipeline(pipelineDesc);
    if (!pipeline)
    {
        common->Printf("PathTracePrimaryPass: failed to create %s RT smoke pipeline\n", label);
        return false;
    }

    shaderTable = pipeline->createShaderTable();
    if (!shaderTable)
    {
        common->Printf("PathTracePrimaryPass: failed to create %s RT smoke shader table\n", label);
        pipeline = nullptr;
        return false;
    }

    shaderTable->setRayGenerationShader("RayGen");
    shaderTable->addMissShader("Miss");
    shaderTable->addMissShader("ShadowMiss");
    shaderTable->addHitGroup("HitGroup");
    shaderTable->addHitGroup("ShadowHitGroup");
    return true;
}

void PathTracePrimaryPass::InitRayTracingSmokeTest()
{
    if (m_smokeTestInitialized)
    {
        return;
    }

    m_smokeTestInitialized = true;

    nvrhi::IDevice* device = deviceManager ? deviceManager->GetDevice() : nullptr;
    if (!device)
    {
        common->Printf("PathTracePrimaryPass: cannot initialize RT smoke test without an NVRHI device\n");
        return;
    }

    const char* shaderPath = nullptr;
    if (deviceManager->GetGraphicsAPI() == nvrhi::GraphicsAPI::D3D12)
    {
        shaderPath = "renderprogs2/dxil/builtin/pathtracing/pathtrace_smoke.rt.bin";
    }
    else if (deviceManager->GetGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
    {
        shaderPath = "renderprogs2/spirv/builtin/pathtracing/pathtrace_smoke.rt.bin";
    }
    else
    {
        common->Printf("PathTracePrimaryPass: RT smoke test does not support this graphics API\n");
        return;
    }

    if (!LoadPathTraceSmokeShaderLibrary(device, shaderPath, "core", m_smokeShaderLibrary))
    {
        return;
    }

    m_smokeTlas = device->createAccelStruct(nvrhi::rt::AccelStructDesc()
        .setTopLevelMaxInstances(512)
        .setBuildFlags(nvrhi::rt::AccelStructBuildFlags::PreferFastTrace)
        .setDebugName("PathTraceSmokeTLAS"));

    if (!m_smokeTlas)
    {
        common->Printf("PathTracePrimaryPass: failed to create RT smoke TLAS\n");
        return;
    }

    nvrhi::BufferDesc constantsDesc;
    constantsDesc.byteSize = GetPathTraceSmokeConstantsSize();
    constantsDesc.debugName = "PathTraceSmokeConstants";
    constantsDesc.isConstantBuffer = true;
    constantsDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    constantsDesc.keepInitialState = true;
    m_smokeConstantsBuffer = device->createBuffer(constantsDesc);

    if (!m_smokeConstantsBuffer)
    {
        common->Printf("PathTracePrimaryPass: failed to create RT smoke constants buffer\n");
        return;
    }

    nvrhi::BufferDesc liquidPoolStatusDesc;
    liquidPoolStatusDesc.byteSize = sizeof(uint32_t) * 16u;
    liquidPoolStatusDesc.structStride = sizeof(uint32_t);
    liquidPoolStatusDesc.canHaveUAVs = true;
    liquidPoolStatusDesc.debugName = "PathTraceLiquidPoolStatusCounters";
    liquidPoolStatusDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    liquidPoolStatusDesc.keepInitialState = true;
    m_liquidPoolStatusBuffer = device->createBuffer(liquidPoolStatusDesc);

    nvrhi::BufferDesc liquidPoolReadbackDesc;
    liquidPoolReadbackDesc.byteSize = sizeof(uint32_t) * 16u;
    liquidPoolReadbackDesc.structStride = sizeof(uint32_t);
    liquidPoolReadbackDesc.cpuAccess = nvrhi::CpuAccessMode::Read;
    liquidPoolReadbackDesc.debugName = "PathTraceLiquidPoolStatusReadback";
    liquidPoolReadbackDesc.initialState = nvrhi::ResourceStates::CopyDest;
    liquidPoolReadbackDesc.keepInitialState = true;
    m_liquidPoolStatusReadbackBuffer = device->createBuffer(liquidPoolReadbackDesc);
    if (!m_liquidPoolStatusBuffer || !m_liquidPoolStatusReadbackBuffer)
    {
        common->Printf("PathTracePrimaryPass: liquid-pool status telemetry unavailable; modes 1-3 fail closed\n");
    }

    nvrhi::BufferDesc cleanRtxdiDiSentinelConstantsDesc;
    cleanRtxdiDiSentinelConstantsDesc.byteSize = 480;
    cleanRtxdiDiSentinelConstantsDesc.debugName = "PathTraceCleanRtxdiDiSentinelConstants";
    cleanRtxdiDiSentinelConstantsDesc.isConstantBuffer = true;
    cleanRtxdiDiSentinelConstantsDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    cleanRtxdiDiSentinelConstantsDesc.keepInitialState = true;
    m_smokeCleanRtxdiDiSentinelConstantsBuffer = device->createBuffer(cleanRtxdiDiSentinelConstantsDesc);
    if (!m_smokeCleanRtxdiDiSentinelConstantsBuffer)
    {
        common->Printf("PathTracePrimaryPass: failed to create clean-room RTXDI DI sentinel constants buffer\n");
        return;
    }

    nvrhi::BufferDesc skySurfaceResolveConstantsDesc;
    skySurfaceResolveConstantsDesc.byteSize = 16;
    skySurfaceResolveConstantsDesc.debugName = "PathTraceSkySurfaceResolveConstants";
    skySurfaceResolveConstantsDesc.isConstantBuffer = true;
    skySurfaceResolveConstantsDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    skySurfaceResolveConstantsDesc.keepInitialState = true;
    m_smokeSkySurfaceResolveConstantsBuffer = device->createBuffer(skySurfaceResolveConstantsDesc);
    if (!m_smokeSkySurfaceResolveConstantsBuffer)
    {
        common->Printf("PathTracePrimaryPass: failed to create sky-surface resolve constants buffer\n");
        return;
    }

    nvrhi::BufferDesc materialFeatureRuntimeConstantsDesc;
    materialFeatureRuntimeConstantsDesc.byteSize = sizeof(RtPathTraceMaterialFeatureRuntimeConstants);
    materialFeatureRuntimeConstantsDesc.debugName = "PathTraceMaterialFeatureRuntimeConstants";
    materialFeatureRuntimeConstantsDesc.isConstantBuffer = true;
    materialFeatureRuntimeConstantsDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    materialFeatureRuntimeConstantsDesc.keepInitialState = true;
    m_smokeMaterialFeatureRuntimeConstantsBuffer = device->createBuffer(materialFeatureRuntimeConstantsDesc);
    if (!m_smokeMaterialFeatureRuntimeConstantsBuffer)
    {
        common->Printf("PathTracePrimaryPass: failed to create material-feature runtime constants buffer\n");
        return;
    }

    nvrhi::BufferDesc cleanRtxdiDiBoilingFilterConstantsDesc;
    cleanRtxdiDiBoilingFilterConstantsDesc.byteSize = 16;
    cleanRtxdiDiBoilingFilterConstantsDesc.debugName = "PathTraceCleanRtxdiDiBoilingFilterConstants";
    cleanRtxdiDiBoilingFilterConstantsDesc.isConstantBuffer = true;
    cleanRtxdiDiBoilingFilterConstantsDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
    cleanRtxdiDiBoilingFilterConstantsDesc.keepInitialState = true;
    m_smokeCleanRtxdiDiBoilingFilterConstantsBuffer = device->createBuffer(cleanRtxdiDiBoilingFilterConstantsDesc);
    if (!m_smokeCleanRtxdiDiBoilingFilterConstantsBuffer)
    {
        common->Printf("PathTracePrimaryPass: failed to create clean-room RTXDI DI boiling-filter constants buffer\n");
        return;
    }

    nvrhi::BufferDesc boundsOverlayDesc;
    boundsOverlayDesc.byteSize = sizeof(RtPathTraceBoundsOverlayLine) * RT_PT_BOUNDS_OVERLAY_MAX_LINES;
    boundsOverlayDesc.debugName = "PathTraceSmokeBoundsOverlayLines";
    boundsOverlayDesc.structStride = sizeof(RtPathTraceBoundsOverlayLine);
    boundsOverlayDesc.initialState = nvrhi::ResourceStates::Common;
    boundsOverlayDesc.keepInitialState = true;
    m_smokeBoundsOverlayLineBuffer = device->createBuffer(boundsOverlayDesc);

    if (!m_smokeBoundsOverlayLineBuffer)
    {
        common->Printf("PathTracePrimaryPass: failed to create RT smoke bounds overlay buffer\n");
        return;
    }

    nvrhi::BindingLayoutDesc bindingLayoutDesc;
    bindingLayoutDesc.visibility = nvrhi::ShaderType::AllRayTracing;
    bindingLayoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setConstantBufferOffset(0)
        .setUnorderedAccessViewOffset(0);
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(1));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(2));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(8));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(9));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(10));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(11));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(12));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(13));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(14));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(15));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(16));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(57));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(58));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(46));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(17));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(21));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(22));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(23));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(24));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(25));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(26));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(27));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(45));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(30));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(31));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(32));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(33));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(34));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(35));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(36));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(37));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(38));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(42));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(43));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(44));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(59));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(60));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(61));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(64));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(65));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(66));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(67));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(74));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(75));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(77));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(41));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(76));
    AddPathTraceMaterialFeatureInputLayoutBinding(
        bindingLayoutDesc,
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_SIDECAR);
    AddPathTraceMaterialFeatureInputLayoutBinding(
        bindingLayoutDesc,
        RT_MATERIAL_FEATURE_RESOURCE_MATERIAL_FEATURE_PARAMETERS);
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(39));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(40));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(47));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(48));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(49));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(50));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(51));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(52));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(53));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(54));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(78));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(79));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(82));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(126));
    bindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    m_smokeBindingLayout = device->createBindingLayout(bindingLayoutDesc);

    if (!m_smokeBindingLayout)
    {
        common->Printf("PathTracePrimaryPass: failed to create RT smoke binding layout\n");
        return;
    }

    nvrhi::BindingLayoutDesc pdfNeeVerifierBindingLayoutDesc = bindingLayoutDesc;
    pdfNeeVerifierBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(69));
    pdfNeeVerifierBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(70));
    pdfNeeVerifierBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(71));
    pdfNeeVerifierBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(73));
    m_smokePdfNeeVerifierBindingLayout = device->createBindingLayout(pdfNeeVerifierBindingLayoutDesc);
    if (!m_smokePdfNeeVerifierBindingLayout)
    {
        common->Printf("PathTracePrimaryPass: failed to create PDFNEE verifier binding layout\n");
        return;
    }

    nvrhi::BindingLayoutDesc cleanRtxdiDiSentinelBindingLayoutDesc;
    cleanRtxdiDiSentinelBindingLayoutDesc.visibility = nvrhi::ShaderType::AllRayTracing;
    cleanRtxdiDiSentinelBindingLayoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setConstantBufferOffset(0)
        .setUnorderedAccessViewOffset(0);
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(1));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(2));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(8));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(9));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(10));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(11));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(12));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(13));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(14));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(15));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(16));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(22));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(23));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(24));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(25));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(26));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(27));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(30));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(31));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(39));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(40));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(42));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(43));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(44));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(45));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(46));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(48));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(49));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(50));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(51));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(52));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(53));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(54));
    AddPathTraceCleanRtxdiDiMaterialFeatureLayoutBindings(cleanRtxdiDiSentinelBindingLayoutDesc);
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(79));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(57));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(64));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(65));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(66));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(67));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(69));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(70));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(71));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(72));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(74));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(75));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(77));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(78));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(94));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(PATH_TRACE_BLUE_NOISE_BINDING));
    cleanRtxdiDiSentinelBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    m_smokeCleanRtxdiDiSentinelBindingLayout = device->createBindingLayout(cleanRtxdiDiSentinelBindingLayoutDesc);
    if (!m_smokeCleanRtxdiDiSentinelBindingLayout)
    {
        common->Printf("PathTracePrimaryPass: failed to create clean-room RTXDI DI sentinel binding layout\n");
        return;
    }

    nvrhi::BindingLayoutDesc regirDebugBindingLayoutDesc;
    regirDebugBindingLayoutDesc.visibility = nvrhi::ShaderType::AllRayTracing;
    regirDebugBindingLayoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setConstantBufferOffset(0)
        .setUnorderedAccessViewOffset(0);
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(1));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(2));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(5));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(8));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(9));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(10));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(11));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(12));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(13));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(16));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(22));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(23));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(24));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(25));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(26));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(27));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(42));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(43));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(44));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(45));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(57));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(58));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(59));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(60));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(61));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(64));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(65));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(66));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(67));
    regirDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(72));
    m_smokeReGIRDebugBindingLayout = device->createBindingLayout(regirDebugBindingLayoutDesc);
    if (!m_smokeReGIRDebugBindingLayout)
    {
        common->Printf("PathTracePrimaryPass: failed to create ReGIR debug binding layout\n");
        return;
    }

    nvrhi::BindingLayoutDesc neeCacheDebugBindingLayoutDesc;
    neeCacheDebugBindingLayoutDesc.visibility = nvrhi::ShaderType::AllRayTracing;
    neeCacheDebugBindingLayoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setConstantBufferOffset(0)
        .setUnorderedAccessViewOffset(0);
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::RayTracingAccelStruct(0));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(1));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(2));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(4));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(6));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(7));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(16));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(22));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(23));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(26));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(27));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(42));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(43));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(44));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(45));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(57));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(58));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(59));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(60));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(61));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(64));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(65));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(66));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(67));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(PATH_TRACE_NEE_CACHE_BINDING_PROVIDER_RESULT_UAV));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(PATH_TRACE_NEE_CACHE_BINDING_CELL_UAV));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(PATH_TRACE_NEE_CACHE_BINDING_TASK_UAV));
    neeCacheDebugBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(PATH_TRACE_NEE_CACHE_BINDING_CANDIDATE_UAV));
    m_smokeNeeCacheDebugBindingLayout = device->createBindingLayout(neeCacheDebugBindingLayoutDesc);
    if (!m_smokeNeeCacheDebugBindingLayout)
    {
        common->Printf("PathTracePrimaryPass: failed to create NEE cache debug binding layout\n");
        return;
    }

    nvrhi::BindingLayoutDesc neeCachePrimarySurfaceUpdateBindingLayoutDesc;
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.visibility = nvrhi::ShaderType::Compute;
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setConstantBufferOffset(0)
        .setUnorderedAccessViewOffset(0);
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(2));
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(16));
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(27));
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(30));
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(42));
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(43));
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(44));
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(45));
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(57));
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(58));
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(59));
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(60));
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(61));
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(64));
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(65));
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(66));
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(67));
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(PATH_TRACE_NEE_CACHE_BINDING_PROVIDER_RESULT_UAV));
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(PATH_TRACE_NEE_CACHE_BINDING_CELL_UAV));
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(PATH_TRACE_NEE_CACHE_BINDING_TASK_UAV));
    neeCachePrimarySurfaceUpdateBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(PATH_TRACE_NEE_CACHE_BINDING_CANDIDATE_UAV));
    m_smokeNeeCachePrimarySurfaceUpdateBindingLayout = device->createBindingLayout(neeCachePrimarySurfaceUpdateBindingLayoutDesc);
    if (!m_smokeNeeCachePrimarySurfaceUpdateBindingLayout)
    {
        common->Printf("PathTracePrimaryPass: failed to create NEE cache primary-surface update binding layout\n");
        return;
    }
    else
    {
        const programInfo_t neeCachePrimarySurfaceUpdateProgram = renderProgManager.GetProgramInfo(BUILTIN_NEE_CACHE_PRIMARY_SURFACE_UPDATE_CS);
        m_smokeNeeCachePrimarySurfaceUpdateShader = neeCachePrimarySurfaceUpdateProgram.cs;
        if (!m_smokeNeeCachePrimarySurfaceUpdateShader)
        {
            common->Printf("PathTracePrimaryPass: NEE cache primary-surface update shader unavailable\n");
        }
        else
        {
            nvrhi::ComputePipelineDesc neeCachePrimarySurfaceUpdatePipelineDesc;
            neeCachePrimarySurfaceUpdatePipelineDesc.CS = m_smokeNeeCachePrimarySurfaceUpdateShader;
            neeCachePrimarySurfaceUpdatePipelineDesc.bindingLayouts = { m_smokeNeeCachePrimarySurfaceUpdateBindingLayout };
            m_smokeNeeCachePrimarySurfaceUpdatePipeline = device->createComputePipeline(neeCachePrimarySurfaceUpdatePipelineDesc);
            if (!m_smokeNeeCachePrimarySurfaceUpdatePipeline)
            {
                common->Printf("PathTracePrimaryPass: failed to create NEE cache primary-surface update compute pipeline\n");
            }
        }
    }

    nvrhi::BindingLayoutDesc skinningBindingLayoutDesc;
    skinningBindingLayoutDesc.visibility = nvrhi::ShaderType::Compute;
    skinningBindingLayoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setUnorderedAccessViewOffset(0);
    skinningBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0));
    skinningBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0));
    skinningBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(1));
    skinningBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1));
    skinningBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2));
    skinningBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3));
    m_smokeSkinnedGpuSkinningBindingLayout = device->createBindingLayout(skinningBindingLayoutDesc);
    if (!m_smokeSkinnedGpuSkinningBindingLayout)
    {
        common->Printf("PathTracePrimaryPass: failed to create PT skinned GPU skinning binding layout\n");
    }
    else
    {
        const programInfo_t skinningProgram = renderProgManager.GetProgramInfo(BUILTIN_PT_SKINNING_CS);
        m_smokeSkinnedGpuSkinningShader = skinningProgram.cs;
        if (!m_smokeSkinnedGpuSkinningShader)
        {
            common->Printf("PathTracePrimaryPass: PT skinned GPU skinning shader unavailable\n");
        }
        else
        {
            nvrhi::ComputePipelineDesc skinningPipelineDesc;
            skinningPipelineDesc.CS = m_smokeSkinnedGpuSkinningShader;
            skinningPipelineDesc.bindingLayouts = { m_smokeSkinnedGpuSkinningBindingLayout };
            m_smokeSkinnedGpuSkinningPipeline = device->createComputePipeline(skinningPipelineDesc);
            if (!m_smokeSkinnedGpuSkinningPipeline)
            {
                common->Printf("PathTracePrimaryPass: failed to create PT skinned GPU skinning compute pipeline\n");
            }
        }
    }

    nvrhi::BindingLayoutDesc cleanBoilingFilterBindingLayoutDesc;
    cleanBoilingFilterBindingLayoutDesc.visibility = nvrhi::ShaderType::Compute;
    cleanBoilingFilterBindingLayoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets()
        .setShaderResourceOffset(0)
        .setUnorderedAccessViewOffset(0)
        .setConstantBufferOffset(0);
    cleanBoilingFilterBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
    cleanBoilingFilterBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(1));
    cleanBoilingFilterBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(2));
    m_smokeCleanRtxdiDiBoilingFilterBindingLayout = device->createBindingLayout(cleanBoilingFilterBindingLayoutDesc);
    if (!m_smokeCleanRtxdiDiBoilingFilterBindingLayout)
    {
        common->Printf("PathTracePrimaryPass: failed to create clean-room RTXDI DI boiling-filter binding layout\n");
    }
    else
    {
        const programInfo_t cleanBoilingFilterProgram = renderProgManager.GetProgramInfo(BUILTIN_CLEAN_RTXDI_DI_BOILING_FILTER_CS);
        m_smokeCleanRtxdiDiBoilingFilterShader = cleanBoilingFilterProgram.cs;
        if (!m_smokeCleanRtxdiDiBoilingFilterShader)
        {
            common->Printf("PathTracePrimaryPass: clean-room RTXDI DI boiling-filter shader unavailable\n");
        }
        else
        {
            nvrhi::ComputePipelineDesc cleanBoilingFilterPipelineDesc;
            cleanBoilingFilterPipelineDesc.CS = m_smokeCleanRtxdiDiBoilingFilterShader;
            cleanBoilingFilterPipelineDesc.bindingLayouts = { m_smokeCleanRtxdiDiBoilingFilterBindingLayout };
            m_smokeCleanRtxdiDiBoilingFilterPipeline = device->createComputePipeline(cleanBoilingFilterPipelineDesc);
            if (!m_smokeCleanRtxdiDiBoilingFilterPipeline)
            {
                common->Printf("PathTracePrimaryPass: failed to create clean-room RTXDI DI boiling-filter compute pipeline\n");
            }
        }
    }

    nvrhi::TextureDesc skyCubeProbeOutputDesc;
    skyCubeProbeOutputDesc.width = 6;
    skyCubeProbeOutputDesc.height = 1;
    skyCubeProbeOutputDesc.mipLevels = 1;
    skyCubeProbeOutputDesc.format = nvrhi::Format::RGBA32_FLOAT;
    skyCubeProbeOutputDesc.dimension = nvrhi::TextureDimension::Texture2D;
    skyCubeProbeOutputDesc.isShaderResource = false;
    skyCubeProbeOutputDesc.isUAV = true;
    skyCubeProbeOutputDesc.initialState = nvrhi::ResourceStates::UnorderedAccess;
    skyCubeProbeOutputDesc.keepInitialState = true;
    skyCubeProbeOutputDesc.debugName = "PathTraceSkyCubeProbeOutput";
    m_smokeSkyCubeProbeOutputTexture = device->createTexture(skyCubeProbeOutputDesc);

    nvrhi::TextureDesc skyCubeProbeReadbackDesc = skyCubeProbeOutputDesc;
    skyCubeProbeReadbackDesc.isUAV = false;
    skyCubeProbeReadbackDesc.initialState = nvrhi::ResourceStates::Unknown;
    skyCubeProbeReadbackDesc.keepInitialState = false;
    skyCubeProbeReadbackDesc.debugName = "PathTraceSkyCubeProbeReadback";
    m_smokeSkyCubeProbeReadbackTexture = device->createStagingTexture(
        skyCubeProbeReadbackDesc,
        nvrhi::CpuAccessMode::Read);

    nvrhi::BindingLayoutDesc skyCubeProbeBindingLayoutDesc;
    skyCubeProbeBindingLayoutDesc.visibility = nvrhi::ShaderType::Compute;
    // ShaderMake's generic SPIR-V compute contract uses the NVRHI defaults:
    // t=0, s=128, b=256, u=384.  Do not use the zero-shift RT-library layout.
    skyCubeProbeBindingLayoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets();
    skyCubeProbeBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    skyCubeProbeBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(1));
    skyCubeProbeBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    m_smokeSkyCubeProbeBindingLayout = device->createBindingLayout(skyCubeProbeBindingLayoutDesc);
    if (!m_smokeSkyCubeProbeOutputTexture ||
        !m_smokeSkyCubeProbeReadbackTexture ||
        !m_smokeSkyCubeProbeBindingLayout)
    {
        common->Printf("PathTracePrimaryPass: failed to create isolated sky-cube probe resources\n");
    }
    else
    {
        const programInfo_t skyCubeProbeProgram = renderProgManager.GetProgramInfo(BUILTIN_PT_SKY_CUBE_PROBE_CS);
        m_smokeSkyCubeProbeShader = skyCubeProbeProgram.cs;
        if (!m_smokeSkyCubeProbeShader)
        {
            common->Printf("PathTracePrimaryPass: isolated sky-cube probe shader unavailable\n");
        }
        else
        {
            nvrhi::ComputePipelineDesc skyCubeProbePipelineDesc;
            skyCubeProbePipelineDesc.CS = m_smokeSkyCubeProbeShader;
            skyCubeProbePipelineDesc.bindingLayouts = { m_smokeSkyCubeProbeBindingLayout };
            m_smokeSkyCubeProbePipeline = device->createComputePipeline(skyCubeProbePipelineDesc);
            if (!m_smokeSkyCubeProbePipeline)
            {
                common->Printf("PathTracePrimaryPass: failed to create isolated sky-cube probe compute pipeline\n");
            }
        }
    }

    nvrhi::BindingLayoutDesc skySurfaceResolveBindingLayoutDesc;
    skySurfaceResolveBindingLayoutDesc.visibility = nvrhi::ShaderType::Compute;
    skySurfaceResolveBindingLayoutDesc.bindingOffsets = nvrhi::VulkanBindingOffsets();
    skySurfaceResolveBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::ConstantBuffer(0));
    skySurfaceResolveBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_SRV(0));
    skySurfaceResolveBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0));
    skySurfaceResolveBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(1));
    skySurfaceResolveBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(2));
    skySurfaceResolveBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Texture_UAV(3));
    skySurfaceResolveBindingLayoutDesc.addItem(nvrhi::BindingLayoutItem::Sampler(0));
    m_smokeSkySurfaceResolveBindingLayout = device->createBindingLayout(skySurfaceResolveBindingLayoutDesc);
    if (!m_smokeSkySurfaceResolveBindingLayout)
    {
        common->Printf("PathTracePrimaryPass: failed to create sky-surface resolve binding layout\n");
    }
    else
    {
        const programInfo_t skySurfaceResolveProgram = renderProgManager.GetProgramInfo(BUILTIN_PT_SKY_SURFACE_RESOLVE_CS);
        m_smokeSkySurfaceResolveShader = skySurfaceResolveProgram.cs;
        if (!m_smokeSkySurfaceResolveShader)
        {
            common->Printf("PathTracePrimaryPass: sky-surface resolve shader unavailable\n");
        }
        else
        {
            nvrhi::ComputePipelineDesc skySurfaceResolvePipelineDesc;
            skySurfaceResolvePipelineDesc.CS = m_smokeSkySurfaceResolveShader;
            skySurfaceResolvePipelineDesc.bindingLayouts = { m_smokeSkySurfaceResolveBindingLayout };
            m_smokeSkySurfaceResolvePipeline = device->createComputePipeline(skySurfaceResolvePipelineDesc);
            if (!m_smokeSkySurfaceResolvePipeline)
            {
                common->Printf("PathTracePrimaryPass: failed to create sky-surface resolve compute pipeline\n");
            }
        }
    }

    nvrhi::BindlessLayoutDesc textureBindlessLayoutDesc;
    textureBindlessLayoutDesc
        .setVisibility(nvrhi::ShaderType::AllRayTracing)
        .setFirstSlot(0)
        .setMaxCapacity(RT_SMOKE_TEXTURE_DESCRIPTOR_CAPACITY)
        .addRegisterSpace(nvrhi::BindingLayoutItem::Texture_SRV(1));
    m_smokeTextureBindlessLayout = device->createBindlessLayout(textureBindlessLayoutDesc);
    if (!m_smokeTextureBindlessLayout)
    {
        common->Printf("PathTracePrimaryPass: failed to create RT smoke bindless texture layout\n");
        return;
    }

    m_smokeTextureDescriptorTable = device->createDescriptorTable(m_smokeTextureBindlessLayout);
    if (!m_smokeTextureDescriptorTable)
    {
        common->Printf("PathTracePrimaryPass: failed to create RT smoke texture descriptor table\n");
        return;
    }

    if (!CreatePathTraceSmokeRayTracingPipeline(
        device,
        m_smokeShaderLibrary,
        m_smokeBindingLayout,
        m_smokeTextureBindlessLayout,
        "core",
        m_smokePipeline,
        m_smokeShaderTable))
    {
        return;
    }

    common->Printf("PathTracePrimaryPass: RT smoke pipeline initialized\n");
}

bool PathTracePrimaryPass::InitRayTracingSmokeRestirPipeline(int restirLibraryKind)
{
    auto initLibrary = [&](nvrhi::ShaderLibraryHandle& shaderLibrary,
        nvrhi::rt::PipelineHandle& pipeline,
        nvrhi::rt::ShaderTableHandle& shaderTable,
        const char* label,
        const char* dxilShaderPath,
        const char* spirvShaderPath,
        nvrhi::BindingLayoutHandle bindingLayoutOverride = nullptr) -> bool
    {
        if (shaderTable)
        {
            return true;
        }

        nvrhi::BindingLayoutHandle bindingLayout = bindingLayoutOverride ? bindingLayoutOverride : m_smokeBindingLayout;
        if (!m_smokeTestInitialized || !bindingLayout || !m_smokeTextureBindlessLayout)
        {
            return false;
        }

        nvrhi::IDevice* device = deviceManager ? deviceManager->GetDevice() : nullptr;
        if (!device)
        {
            return false;
        }

        const char* restirShaderPath = nullptr;
        if (deviceManager->GetGraphicsAPI() == nvrhi::GraphicsAPI::D3D12)
        {
            restirShaderPath = dxilShaderPath;
        }
        else if (deviceManager->GetGraphicsAPI() == nvrhi::GraphicsAPI::VULKAN)
        {
            restirShaderPath = spirvShaderPath;
        }
        else
        {
            return false;
        }

        if (!shaderLibrary &&
            !LoadPathTraceSmokeShaderLibrary(device, restirShaderPath, label, shaderLibrary))
        {
            common->Printf("PathTracePrimaryPass: %s RT smoke shader unavailable; matching modes will use the core placeholder path\n", label);
            return false;
        }

        if (!CreatePathTraceSmokeRayTracingPipeline(
            device,
            shaderLibrary,
            bindingLayout,
            m_smokeTextureBindlessLayout,
            label,
            pipeline,
            shaderTable))
        {
            common->Printf("PathTracePrimaryPass: %s RT smoke pipeline unavailable; matching modes will use the core placeholder path\n", label);
            pipeline = nullptr;
            shaderTable = nullptr;
            return false;
        }

        common->Printf("PathTracePrimaryPass: %s RT smoke pipeline initialized\n", label);
        return true;
    };

    switch (restirLibraryKind)
    {
    case 9:
    {
        const bool initialized = initLibrary(
            m_smokePrimarySurfaceProducerShaderLibrary,
            m_smokePrimarySurfaceProducerPipeline,
            m_smokePrimarySurfaceProducerShaderTable,
            "primary-surface producer",
            "renderprogs2/dxil/builtin/pathtracing/pathtrace_primary_surface_producer.rt.bin",
            "renderprogs2/spirv/builtin/pathtracing/pathtrace_primary_surface_producer.rt.bin");
        if (initialized)
        {
            common->Printf("PathTracePrimaryPass: primary-surface producer material-feature parameters bound=t81 stride=%u featureAbi=%u liquidParamAbi=%u\n",
                RT_PATH_TRACE_MATERIAL_FEATURE_PARAMETER_RECORD_STRIDE,
                RT_PATH_TRACE_MATERIAL_FEATURE_RECORD_ABI_VERSION,
                RT_PATH_TRACE_LIQUID_POOL_PARAMETER_ABI_VERSION);
        }
        return initialized;
    }
    case 15:
    {
        const bool sentinelOk = initLibrary(
            m_smokeCleanRtxdiDiSentinelShaderLibrary,
            m_smokeCleanRtxdiDiSentinelPipeline,
            m_smokeCleanRtxdiDiSentinelShaderTable,
            "clean-room RTXDI DI sentinel",
            "renderprogs2/dxil/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_sentinel.rt.bin",
            "renderprogs2/spirv/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_sentinel.rt.bin",
            m_smokeCleanRtxdiDiSentinelBindingLayout);
        const bool initialOk = initLibrary(
            m_smokeCleanRtxdiDiInitialShaderLibrary,
            m_smokeCleanRtxdiDiInitialPipeline,
            m_smokeCleanRtxdiDiInitialShaderTable,
            "clean-room RTXDI DI initial",
            "renderprogs2/dxil/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_initial.rt.bin",
            "renderprogs2/spirv/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_initial.rt.bin",
            m_smokeCleanRtxdiDiSentinelBindingLayout);
        const bool temporalOk = initLibrary(
            m_smokeCleanRtxdiDiTemporalShaderLibrary,
            m_smokeCleanRtxdiDiTemporalPipeline,
            m_smokeCleanRtxdiDiTemporalShaderTable,
            "clean-room RTXDI DI temporal",
            "renderprogs2/dxil/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_temporal.rt.bin",
            "renderprogs2/spirv/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_temporal.rt.bin",
            m_smokeCleanRtxdiDiSentinelBindingLayout);
        const bool initialProductionOk = initLibrary(
            m_smokeCleanRtxdiDiInitialProductionShaderLibrary,
            m_smokeCleanRtxdiDiInitialProductionPipeline,
            m_smokeCleanRtxdiDiInitialProductionShaderTable,
            "clean-room RTXDI DI initial production view 16",
            "renderprogs2/dxil/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_initial_production.rt.bin",
            "renderprogs2/spirv/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_initial_production.rt.bin",
            m_smokeCleanRtxdiDiSentinelBindingLayout);
        const bool temporalProductionOk = initLibrary(
            m_smokeCleanRtxdiDiTemporalProductionShaderLibrary,
            m_smokeCleanRtxdiDiTemporalProductionPipeline,
            m_smokeCleanRtxdiDiTemporalProductionShaderTable,
            "clean-room RTXDI DI temporal production view 16",
            "renderprogs2/dxil/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_temporal_production.rt.bin",
            "renderprogs2/spirv/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_temporal_production.rt.bin",
            m_smokeCleanRtxdiDiSentinelBindingLayout);
        return sentinelOk && initialOk && temporalOk && initialProductionOk && temporalProductionOk;
    }
    case 20:
    {
        const bool spatialOk = initLibrary(
            m_smokeCleanRtxdiDiSpatialShaderLibrary,
            m_smokeCleanRtxdiDiSpatialPipeline,
            m_smokeCleanRtxdiDiSpatialShaderTable,
            "clean-room RTXDI DI spatial",
            "renderprogs2/dxil/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_spatial.rt.bin",
            "renderprogs2/spirv/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_spatial.rt.bin",
            m_smokeCleanRtxdiDiSentinelBindingLayout);
        const bool spatialProductionOk = initLibrary(
            m_smokeCleanRtxdiDiSpatialProductionShaderLibrary,
            m_smokeCleanRtxdiDiSpatialProductionPipeline,
            m_smokeCleanRtxdiDiSpatialProductionShaderTable,
            "clean-room RTXDI DI spatial production view 16",
            "renderprogs2/dxil/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_spatial_production.rt.bin",
            "renderprogs2/spirv/builtin/pathtracing/cleanroom_rtxdi/pathtrace_clean_rtxdi_di_spatial_production.rt.bin",
            m_smokeCleanRtxdiDiSentinelBindingLayout);
        return spatialOk && spatialProductionOk;
    }
    case 17:
        return initLibrary(
            m_smokeReGIRDebugShaderLibrary,
            m_smokeReGIRDebugPipeline,
            m_smokeReGIRDebugShaderTable,
            "ReGIR debug",
            "renderprogs2/dxil/builtin/pathtracing/pathtrace_regir_debug.rt.bin",
            "renderprogs2/spirv/builtin/pathtracing/pathtrace_regir_debug.rt.bin",
            m_smokeReGIRDebugBindingLayout);
    case 18:
        return initLibrary(
            m_smokeRestirPdfNeeRluCurrentShaderLibrary,
            m_smokeRestirPdfNeeRluCurrentPipeline,
            m_smokeRestirPdfNeeRluCurrentShaderTable,
            "ReSTIR PDF+NEE RLU current producer",
            "renderprogs2/dxil/builtin/pathtracing/pathtrace_restir_pdf_nee_rlu_current.rt.bin",
            "renderprogs2/spirv/builtin/pathtracing/pathtrace_restir_pdf_nee_rlu_current.rt.bin",
            m_smokePdfNeeVerifierBindingLayout);
    case 19:
        return initLibrary(
            m_smokeNeeCacheDebugShaderLibrary,
            m_smokeNeeCacheDebugPipeline,
            m_smokeNeeCacheDebugShaderTable,
            "NEE cache debug",
            "renderprogs2/dxil/builtin/pathtracing/pathtrace_nee_cache_debug.rt.bin",
            "renderprogs2/spirv/builtin/pathtracing/pathtrace_nee_cache_debug.rt.bin",
            m_smokeNeeCacheDebugBindingLayout);
    default:
        return false;
    }
}

bool PathTracePrimaryPass::ResizeRayTracingSmokeOutput(int width, int height, int outputWidth, int outputHeight)
{
    if (!m_smokeTestInitialized || !m_smokeShaderTable)
    {
        return false;
    }

    const bool alreadyValid = m_frameResources.IsValidFor(width, height, outputWidth, outputHeight);
    if (alreadyValid)
    {
        return true;
    }

    nvrhi::IDevice* device = deviceManager ? deviceManager->GetDevice() : nullptr;
    if (!m_frameResources.ResizeOutputSizedResources(device, width, height, outputWidth, outputHeight))
    {
        return false;
    }

    ResetRayTracingSmokeSceneResources();
    return true;
}

void PathTracePrimaryPass::InvalidateForBackBufferResize()
{
    ResetRayTracingSmokeSceneResources();
    m_frameResources.ResetOutputSizedResources(RT_FRAME_RESET_BACKBUFFER_RESIZE);
}

bool PathTracePrimaryPass::HasRetainableRayTracingSmokeScenePackage() const
{
    RtSmokeSceneBufferHandles buffers;
    buffers.staticVertexBuffer = m_smokeStaticVertexBuffer;
    buffers.staticIndexBuffer = m_smokeStaticIndexBuffer;
    buffers.staticTriangleClassBuffer = m_smokeStaticTriangleClassBuffer;
    buffers.staticTriangleMaterialBuffer = m_smokeStaticTriangleMaterialBuffer;
    buffers.staticTriangleMaterialIndexBuffer = m_smokeStaticTriangleMaterialIndexBuffer;
    buffers.previousStaticVertexBuffer = m_smokePreviousStaticVertexBuffer;
    buffers.previousStaticIndexBuffer = m_smokePreviousStaticIndexBuffer;
    buffers.previousStaticTriangleClassBuffer = m_smokePreviousStaticTriangleClassBuffer;
    buffers.previousStaticTriangleMaterialBuffer = m_smokePreviousStaticTriangleMaterialBuffer;
    buffers.previousStaticTriangleMaterialIndexBuffer = m_smokePreviousStaticTriangleMaterialIndexBuffer;
    buffers.dynamicVertexBuffer = m_smokeDynamicVertexBuffer;
    buffers.dynamicIndexBuffer = m_smokeDynamicIndexBuffer;
    buffers.dynamicTriangleClassBuffer = m_smokeDynamicTriangleClassBuffer;
    buffers.dynamicTriangleMaterialBuffer = m_smokeDynamicTriangleMaterialBuffer;
    buffers.dynamicTriangleMaterialIndexBuffer = m_smokeDynamicTriangleMaterialIndexBuffer;
    buffers.materialTableBuffer = m_smokeMaterialTableBuffer;
    buffers.materialFeatureBuffer = m_smokeMaterialFeatureBuffer;
    buffers.materialFeatureParameterBuffer = m_smokeMaterialFeatureParameterBuffer;
    buffers.dynamicMaterialBuffer = m_smokeDynamicMaterialBuffer;
    buffers.emissiveTriangleBuffer = m_smokeEmissiveTriangleBuffer;
    buffers.previousEmissiveTriangleBuffer = m_smokePreviousEmissiveTriangleBuffer;
    buffers.emissiveRemapBuffer = m_smokeEmissiveRemapBuffer;
    buffers.emissiveDistributionBuffer = m_smokeEmissiveDistributionBuffer;
    buffers.lightCandidateBuffer = m_smokeLightCandidateBuffer;
    buffers.doomAnalyticLightBuffer = m_smokeDoomAnalyticLightBuffer;
    buffers.doomAnalyticPreviousLightBuffer = m_smokeDoomAnalyticPreviousLightBuffer;
    buffers.doomAnalyticCurrentIdentityBuffer = m_smokeDoomAnalyticCurrentIdentityBuffer;
    buffers.doomAnalyticPreviousIdentityBuffer = m_smokeDoomAnalyticPreviousIdentityBuffer;
    buffers.doomAnalyticRemapBuffer = m_smokeDoomAnalyticRemapBuffer;
    buffers.unifiedLightBuffer = m_smokeUnifiedLightBuffer;
    buffers.unifiedPreviousLightBuffer = m_smokeUnifiedPreviousLightBuffer;
    buffers.unifiedLightRemapBuffer = m_smokeUnifiedLightRemapBuffer;
    buffers.restirLightManagerCurrentToPreviousBuffer = m_smokeRestirLightManagerCurrentToPreviousBuffer;
    buffers.restirLightManagerPreviousToCurrentBuffer = m_smokeRestirLightManagerPreviousToCurrentBuffer;
    buffers.restirLightManagerCurrentPayloadBuffer = m_smokeRestirLightManagerCurrentPayloadBuffer;
    buffers.restirLightManagerPreviousPayloadBuffer = m_smokeRestirLightManagerPreviousPayloadBuffer;
    buffers.rigidRouteVertexBuffer = m_smokeRigidRouteVertexBuffer;
    buffers.rigidRouteIndexBuffer = m_smokeRigidRouteIndexBuffer;
    buffers.rigidRouteTriangleMaterialBuffer = m_smokeRigidRouteTriangleMaterialBuffer;
    buffers.rigidRouteTriangleMaterialIndexBuffer = m_smokeRigidRouteTriangleMaterialIndexBuffer;
    buffers.rigidRouteInstanceBuffer = m_smokeRigidRouteInstanceBuffer;
    buffers.skinnedSourceVertexBuffer = m_smokeSkinnedSourceVertexBuffer;
    buffers.skinnedCurrentOutputVertexBuffer = m_smokeSkinnedCurrentOutputVertexBuffer;
    buffers.skinnedPreviousPositionBuffer = m_smokeSkinnedPreviousPositionBuffer;
    buffers.skinnedSurfaceDispatchBuffer = m_smokeSkinnedSurfaceDispatchBuffer;
    buffers.skinnedTriangleDispatchIndexBuffer = m_smokeSkinnedTriangleDispatchIndexBuffer;
    buffers.skinnedCurrentJointMatrixBuffer = m_smokeSkinnedCurrentJointMatrixBuffer;
    buffers.skinnedPreviousJointMatrixBuffer = m_smokeSkinnedPreviousJointMatrixBuffer;

    return
        buffers.IsValid() ||
        m_smokePreviousStaticVertexBuffer ||
        m_smokePreviousStaticIndexBuffer ||
        m_smokePreviousStaticTriangleClassBuffer ||
        m_smokePreviousStaticTriangleMaterialBuffer ||
        m_smokePreviousStaticTriangleMaterialIndexBuffer ||
        m_smokePreviousEmissiveTriangleBuffer ||
        m_smokeMaterialFeatureParameterBuffer ||
        m_smokeDynamicMaterialBuffer ||
        m_smokeEmissiveRemapBuffer ||
        m_smokeUnifiedLightBuffer ||
        m_smokeUnifiedPreviousLightBuffer ||
        m_smokeUnifiedLightRemapBuffer ||
        m_smokeRestirLightManagerCurrentToPreviousBuffer ||
        m_smokeRestirLightManagerPreviousToCurrentBuffer ||
        m_smokeRestirLightManagerCurrentPayloadBuffer ||
        m_smokeRestirLightManagerPreviousPayloadBuffer ||
        m_smokeSkinnedSourceVertexBuffer ||
        m_smokeSkinnedCurrentOutputVertexBuffer ||
        m_smokeSkinnedPreviousPositionBuffer ||
        m_smokeSkinnedSurfaceDispatchBuffer ||
        m_smokeSkinnedTriangleDispatchIndexBuffer ||
        m_smokeSkinnedCurrentJointMatrixBuffer ||
        m_smokeSkinnedPreviousJointMatrixBuffer ||
        m_smokeStaticBlas ||
        m_smokeDynamicBlas ||
        m_smokeTlas ||
        m_smokeBindingSet ||
        m_smokeTextureDescriptorTable ||
        m_smokeSkyEnvironmentCube ||
        m_smokeSkyCubeProbeBindingSet ||
        !m_smokeActiveTextureTable.empty();
}

RtRetiredSmokeScenePackage PathTracePrimaryPass::CaptureRetiredRayTracingSmokeScenePackage() const
{
    RtRetiredSmokeScenePackage package;
    package.buffers.staticVertexBuffer = m_smokeStaticVertexBuffer;
    package.buffers.staticIndexBuffer = m_smokeStaticIndexBuffer;
    package.buffers.staticTriangleClassBuffer = m_smokeStaticTriangleClassBuffer;
    package.buffers.staticTriangleMaterialBuffer = m_smokeStaticTriangleMaterialBuffer;
    package.buffers.staticTriangleMaterialIndexBuffer = m_smokeStaticTriangleMaterialIndexBuffer;
    package.buffers.previousStaticVertexBuffer = m_smokePreviousStaticVertexBuffer;
    package.buffers.previousStaticIndexBuffer = m_smokePreviousStaticIndexBuffer;
    package.buffers.previousStaticTriangleClassBuffer = m_smokePreviousStaticTriangleClassBuffer;
    package.buffers.previousStaticTriangleMaterialBuffer = m_smokePreviousStaticTriangleMaterialBuffer;
    package.buffers.previousStaticTriangleMaterialIndexBuffer = m_smokePreviousStaticTriangleMaterialIndexBuffer;
    package.buffers.dynamicVertexBuffer = m_smokeDynamicVertexBuffer;
    package.buffers.dynamicIndexBuffer = m_smokeDynamicIndexBuffer;
    package.buffers.dynamicTriangleClassBuffer = m_smokeDynamicTriangleClassBuffer;
    package.buffers.dynamicTriangleMaterialBuffer = m_smokeDynamicTriangleMaterialBuffer;
    package.buffers.dynamicTriangleMaterialIndexBuffer = m_smokeDynamicTriangleMaterialIndexBuffer;
    package.buffers.materialTableBuffer = m_smokeMaterialTableBuffer;
    package.buffers.materialFeatureBuffer = m_smokeMaterialFeatureBuffer;
    package.buffers.materialFeatureParameterBuffer = m_smokeMaterialFeatureParameterBuffer;
    package.buffers.dynamicMaterialBuffer = m_smokeDynamicMaterialBuffer;
    package.buffers.emissiveTriangleBuffer = m_smokeEmissiveTriangleBuffer;
    package.buffers.previousEmissiveTriangleBuffer = m_smokePreviousEmissiveTriangleBuffer;
    package.buffers.emissiveRemapBuffer = m_smokeEmissiveRemapBuffer;
    package.buffers.emissiveDistributionBuffer = m_smokeEmissiveDistributionBuffer;
    package.buffers.lightCandidateBuffer = m_smokeLightCandidateBuffer;
    package.buffers.doomAnalyticLightBuffer = m_smokeDoomAnalyticLightBuffer;
    package.buffers.doomAnalyticPreviousLightBuffer = m_smokeDoomAnalyticPreviousLightBuffer;
    package.buffers.doomAnalyticCurrentIdentityBuffer = m_smokeDoomAnalyticCurrentIdentityBuffer;
    package.buffers.doomAnalyticPreviousIdentityBuffer = m_smokeDoomAnalyticPreviousIdentityBuffer;
    package.buffers.doomAnalyticRemapBuffer = m_smokeDoomAnalyticRemapBuffer;
    package.buffers.unifiedLightBuffer = m_smokeUnifiedLightBuffer;
    package.buffers.unifiedPreviousLightBuffer = m_smokeUnifiedPreviousLightBuffer;
    package.buffers.unifiedLightRemapBuffer = m_smokeUnifiedLightRemapBuffer;
    package.buffers.restirLightManagerCurrentToPreviousBuffer = m_smokeRestirLightManagerCurrentToPreviousBuffer;
    package.buffers.restirLightManagerPreviousToCurrentBuffer = m_smokeRestirLightManagerPreviousToCurrentBuffer;
    package.buffers.restirLightManagerCurrentPayloadBuffer = m_smokeRestirLightManagerCurrentPayloadBuffer;
    package.buffers.restirLightManagerPreviousPayloadBuffer = m_smokeRestirLightManagerPreviousPayloadBuffer;
    package.buffers.rigidRouteVertexBuffer = m_smokeRigidRouteVertexBuffer;
    package.buffers.rigidRouteIndexBuffer = m_smokeRigidRouteIndexBuffer;
    package.buffers.rigidRouteTriangleMaterialBuffer = m_smokeRigidRouteTriangleMaterialBuffer;
    package.buffers.rigidRouteTriangleMaterialIndexBuffer = m_smokeRigidRouteTriangleMaterialIndexBuffer;
    package.buffers.rigidRouteInstanceBuffer = m_smokeRigidRouteInstanceBuffer;
    package.buffers.skinnedSourceVertexBuffer = m_smokeSkinnedSourceVertexBuffer;
    package.buffers.skinnedCurrentOutputVertexBuffer = m_smokeSkinnedCurrentOutputVertexBuffer;
    package.buffers.skinnedPreviousPositionBuffer = m_smokeSkinnedPreviousPositionBuffer;
    package.buffers.skinnedSurfaceDispatchBuffer = m_smokeSkinnedSurfaceDispatchBuffer;
    package.buffers.skinnedTriangleDispatchIndexBuffer = m_smokeSkinnedTriangleDispatchIndexBuffer;
    package.buffers.skinnedCurrentJointMatrixBuffer = m_smokeSkinnedCurrentJointMatrixBuffer;
    package.buffers.skinnedPreviousJointMatrixBuffer = m_smokeSkinnedPreviousJointMatrixBuffer;
    package.staticBlas = m_smokeStaticBlas;
    package.dynamicBlas = m_smokeDynamicBlas;
    package.tlas = m_smokeTlas;
    package.bindingSet = m_smokeBindingSet;
    package.textureDescriptorTable = m_smokeTextureDescriptorTable;
    package.activeTextureTable = m_smokeActiveTextureTable;
    package.skyEnvironmentCube = m_smokeSkyEnvironmentCube;
    package.skyCubeProbeBindingSet = m_smokeSkyCubeProbeBindingSet;
    return package;
}

void PathTracePrimaryPass::PushRetiredRayTracingSmokeScenePackage(RtRetiredSmokeScenePackage& package, uint64 currentFrame, int retireFrames)
{
    if (retireFrames <= 0)
    {
        return;
    }

    package.retireFrame = currentFrame + static_cast<uint64>(retireFrames);
    m_retiredSmokeScenePackages.push_back(package);
}

int PathTracePrimaryPass::ReleaseExpiredRetiredRayTracingSmokeScenePackages(uint64 currentFrame)
{
    int releasedCount = 0;
    while (!m_retiredSmokeScenePackages.empty() && m_retiredSmokeScenePackages.front().retireFrame <= currentFrame)
    {
        m_retiredSmokeScenePackages.pop_front();
        ++releasedCount;
    }

    return releasedCount;
}

void PathTracePrimaryPass::ResetRayTracingSmokeAsyncCpuWork()
{
    m_smokeAccelerationPlanFuture.Reset();
    m_smokeRigidTlasPlanFuture.Reset();
    m_smokeRigidRouteBuildFuture.Reset();
    m_smokeBvhFramePlanningFuture.Reset();

    m_smokeCpuWorkState = RtPathTraceCpuWorkState();
    m_smokeRigidTlasCpuWorkState = RtPathTraceCpuWorkState();
    m_smokeRigidRouteBuildCpuWorkState = RtPathTraceCpuWorkState();
    m_smokeBvhFramePlanningCpuWorkState = RtPathTraceCpuWorkState();

    m_smokeAccelerationPlanAsyncGeneration = RtPathTraceCpuWorkGeneration();
    m_smokeAccelerationPlanAsyncCachedGeneration = RtPathTraceCpuWorkGeneration();
    m_smokeAccelerationPlanAsyncTiming = RtPathTraceCpuWorkTiming();
    m_smokeAccelerationPlanAsyncCachedPlan = RtSmokeAccelerationPlan();
    m_smokeAccelerationPlanAsyncLaunchMs = 0;
    m_smokeAccelerationPlanAsyncGenerationValid = false;
    m_smokeAccelerationPlanAsyncCachedPlanValid = false;

    m_smokeRigidTlasPlanAsyncGeneration = RtPathTraceCpuWorkGeneration();
    m_smokeRigidTlasPlanAsyncCachedGeneration = RtPathTraceCpuWorkGeneration();
    m_smokeRigidTlasPlanAsyncTiming = RtPathTraceCpuWorkTiming();
    m_smokeRigidTlasPlanAsyncCachedPlan = RtSmokeRigidTlasPlan();
    m_smokeRigidTlasPlanAsyncLaunchMs = 0;
    m_smokeRigidTlasPlanAsyncGenerationValid = false;
    m_smokeRigidTlasPlanAsyncCachedPlanValid = false;

    m_smokeRigidRouteBuildAsyncGeneration = RtPathTraceCpuWorkGeneration();
    m_smokeRigidRouteBuildAsyncCachedGeneration = RtPathTraceCpuWorkGeneration();
    m_smokeRigidRouteBuildAsyncTiming = RtPathTraceCpuWorkTiming();
    m_smokeRigidRouteBuildAsyncCachedBuild = RtPathTraceRigidRouteBuild();
    m_smokeRigidRouteBuildAsyncCachedGeometryUploadSignature = 0;
    m_smokeRigidRouteBuildAsyncCachedInstanceUploadSignature = 0;
    m_smokeRigidRouteBuildAsyncLaunchMs = 0;
    m_smokeRigidRouteBuildAsyncGenerationValid = false;
    m_smokeRigidRouteBuildAsyncCachedBuildValid = false;
    m_smokeRigidRouteBuildAsyncCachedGeometryUploadSignatureValid = false;
    m_smokeRigidRouteBuildAsyncCachedInstanceUploadSignatureValid = false;

    m_smokeBvhFramePlanningAsyncGeneration = RtPathTraceCpuWorkGeneration();
    m_smokeBvhFramePlanningAsyncCachedGeneration = RtPathTraceCpuWorkGeneration();
    m_smokeBvhFramePlanningAsyncTiming = RtPathTraceCpuWorkTiming();
    m_smokeBvhFramePlanningAsyncCachedResult = RtSmokeBvhFramePlanningResult();
    m_smokeBvhFramePlanningAsyncLaunchMs = 0;
    m_smokeBvhFramePlanningAsyncGenerationValid = false;
    m_smokeBvhFramePlanningAsyncCachedResultValid = false;
}

void PathTracePrimaryPass::ResetRayTracingSmokeSceneResources()
{
    if (r_pathTracingWaitForIdleOnPortalChange.GetInteger() != 0 && (HasRetainableRayTracingSmokeScenePackage() || !m_retiredSmokeScenePackages.empty()))
    {
        nvrhi::IDevice* device = deviceManager ? deviceManager->GetDevice() : nullptr;
        if (device)
        {
            device->waitForIdle();
        }
    }

    m_frameResources.ResetSceneDependentState();
    m_sceneInputs = RtPathTraceSceneInputs();
    m_smokeBindingSet = nullptr;
    m_smokeSkyEnvironmentCube = nullptr;
    m_smokeSkyCubeProbeBindingSet = nullptr;
    m_smokeSkyCubeProbeReadbackQueued = false;
    m_smokeSkyCubeProbeReadbackDelayFrames = 0;
    m_staticContractShaderReadbackRequested = false;
    m_staticContractShaderReadbackQueued = false;
    m_staticContractShaderReadbackDelayFrames = 0;
    m_staticContractGeometryReadbackBuffer = nullptr;
    m_staticContractGeometryReadbackQueued = false;
    m_staticContractGeometryReadbackDelayFrames = 0;
    m_staticContractGeometryCpuVertices.clear();
    m_staticContractGeometryCpuIndexes.clear();
    m_gpuSkinningParityReadbackBuffer = nullptr;
    m_gpuSkinningParityReadbackQueued = false;
    m_gpuSkinningParityReadbackDelayFrames = 0;
    m_gpuSkinningParitySamples.clear();
    m_smokeSkyEnvironmentSourceName.Clear();
    m_smokeSceneBuilt = false;
    m_smokeTestDispatched = false;
    m_smokeStaticBlasCacheValid = false;
    m_smokeStaticBlasSignature = 0;
    m_smokeStaticBlasGeometryGeneration = 0;
    m_smokeSceneUniverseStaticBuildGeneration = 0;
    ResetRayTracingSmokeAsyncCpuWork();
    m_smokeBvhDirtyPreviousTokenValid = false;
    m_smokeBvhDirtyPreviousToken = RtSmokeBvhDirtyTokenState();
    m_smokeSceneRebuildLogged = false;
    m_smokeGeometryUniverse.Clear();
    m_particleDiagnosticFramesRemaining = 0;
    m_smokeSkinnedSurfaceRecords.clear();
    m_smokePreviousSkinnedSurfaceRecords.clear();
    m_smokePreviousSkinnedVertexData.clear();
    m_smokePreviousSkinnedJointMatrices.clear();
    m_sceneUniverse.Clear();
    m_instanceUniverse.Clear();
    ClearSmokeMaterialTextureRegistry();
    ClearSmokeResidentMaterialFacts();
    ClearSmokeMaterialUniverse();
    ClearSmokeMaterialTableCache();
    m_remixFramePrepare.Clear();
    m_remixLightManager.Clear();
    m_smokeSceneRenderWorld = nullptr;
    m_smokeSceneMapName.Clear();
    m_smokeSceneMapTimeStamp = 0;
    m_smokeSceneMapLoadSerial = 0;
    m_smokeStaticBlas = nullptr;
    m_smokeDynamicBlas = nullptr;
    m_smokeStaticVertexBuffer = nullptr;
    m_smokeStaticIndexBuffer = nullptr;
    m_smokeStaticTriangleClassBuffer = nullptr;
    m_smokeStaticTriangleMaterialBuffer = nullptr;
    m_smokeStaticTriangleMaterialIndexBuffer = nullptr;
    m_smokePreviousStaticVertexBuffer = nullptr;
    m_smokePreviousStaticIndexBuffer = nullptr;
    m_smokePreviousStaticTriangleClassBuffer = nullptr;
    m_smokePreviousStaticTriangleMaterialBuffer = nullptr;
    m_smokePreviousStaticTriangleMaterialIndexBuffer = nullptr;
    m_smokePreviousStaticTriangleMaterialIndexes.clear();
    m_smokePreviousStaticSnapshotUploadSignature = 0;
    m_smokePreviousStaticMaterialIndexUploadSignature = 0;
    m_smokeDynamicVertexBuffer = nullptr;
    m_smokeDynamicIndexBuffer = nullptr;
    m_smokeDynamicTriangleClassBuffer = nullptr;
    m_smokeDynamicTriangleMaterialBuffer = nullptr;
    m_smokeDynamicTriangleMaterialIndexBuffer = nullptr;
    m_smokeMaterialTableBuffer = nullptr;
    m_smokeMaterialFeatureBuffer = nullptr;
    m_smokeMaterialFeatureParameterBuffer = nullptr;
    m_smokeDynamicMaterialBuffer = nullptr;
    m_smokeMaterialTableUploadSignature = 0;
    m_smokeDynamicMaterialUploadSignature = 0;
    m_smokeMaterialTableUploadSignatureValid = false;
    m_smokeDynamicMaterialUploadSignatureValid = false;
    m_smokeMaterialTableMaterials.clear();
    m_smokeDynamicMaterialRecords.clear();
    m_smokeMaterialHydrationIds.clear();
    m_smokeMaterialHydrationIdsValid = false;
    m_smokeMaterialHydrationStaticGeneration = 0;
    m_smokeMaterialHydrationStaticTriangleMaterialCount = 0;
    m_smokeMaterialHydrationEmissiveSignature = 0;
    m_smokeMaterialHydrationRigidSignature = 0;
    m_smokeEmissiveTriangleBuffer = nullptr;
    m_smokePreviousEmissiveTriangleBuffer = nullptr;
    m_smokeEmissiveRemapBuffer = nullptr;
    m_smokeEmissiveDistributionBuffer = nullptr;
    m_smokeLightCandidateBuffer = nullptr;
    m_smokeDoomAnalyticLightBuffer = nullptr;
    m_smokeDoomAnalyticPreviousLightBuffer = nullptr;
    m_smokeDoomAnalyticCurrentIdentityBuffer = nullptr;
    m_smokeDoomAnalyticPreviousIdentityBuffer = nullptr;
    m_smokeDoomAnalyticRemapBuffer = nullptr;
    m_smokeUnifiedLightBuffer = nullptr;
    m_smokeUnifiedPreviousLightBuffer = nullptr;
    m_smokeUnifiedLightRemapBuffer = nullptr;
    m_smokeRestirLightManagerCurrentToPreviousBuffer = nullptr;
    m_smokeRestirLightManagerPreviousToCurrentBuffer = nullptr;
    m_smokeRestirLightManagerCurrentPayloadBuffer = nullptr;
    m_smokeRestirLightManagerPreviousPayloadBuffer = nullptr;
    m_smokeRigidRouteVertexBuffer = nullptr;
    m_smokeRigidRouteIndexBuffer = nullptr;
    m_smokeRigidRouteTriangleMaterialBuffer = nullptr;
    m_smokeRigidRouteTriangleMaterialIndexBuffer = nullptr;
    m_smokeRigidRouteInstanceBuffer = nullptr;
    for (int slotIndex = 0; slotIndex < RT_SMOKE_RIGID_ROUTE_SIDE_BUFFER_SLOTS; ++slotIndex)
    {
        m_smokeRigidRouteSideBufferSlots[slotIndex] = RtSmokeRigidRouteSideBufferSlot();
    }
    m_smokeRigidRouteSideBufferReadSlot = -1;
    m_smokeRigidRouteSideBufferWriteSlot = 0;
    m_smokeSkinnedSourceVertexBuffer = nullptr;
    m_smokeSkinnedCurrentOutputVertexBuffer = nullptr;
    m_smokeSkinnedPreviousPositionBuffer = nullptr;
    m_smokeSkinnedSurfaceDispatchBuffer = nullptr;
    m_smokeSkinnedTriangleDispatchIndexBuffer = nullptr;
    m_smokeSkinnedCurrentJointMatrixBuffer = nullptr;
    m_smokeSkinnedPreviousJointMatrixBuffer = nullptr;
    m_smokeSkinnedGpuSkinningBindingSet = nullptr;
    m_smokeCleanRtxdiDiBoilingFilterBindingSet = nullptr;
    m_smokeSkinnedGpuSkinningOutputBuffer = nullptr;
    m_smokeSkinnedGpuSkinningPreviousPositionBuffer = nullptr;
    m_smokeCleanRtxdiDiBoilingFilterInputTexture = nullptr;
    m_smokeCleanRtxdiDiBoilingFilterOutputTexture = nullptr;
    m_smokeCleanRtxdiDiCurrentReservoirBuffer = nullptr;
    m_smokeCleanRtxdiDiTemporalReservoirBuffer = nullptr;
    m_smokeCleanRtxdiDiPreviousReservoirBuffer = nullptr;
    m_smokeCleanRtxdiDiSpatialReservoirBuffer = nullptr;
    m_smokeNeeCacheState.Clear();
    m_smokeReGIRState.Clear();
    m_smokeCleanRtxdiDiCurrentReservoirCount = 0;
    m_smokeCleanRtxdiDiTemporalReservoirCount = 0;
    m_smokeCleanRtxdiDiPreviousReservoirCount = 0;
    m_smokeCleanRtxdiDiSpatialReservoirCount = 0;
    m_smokeCleanRtxdiDiCurrentReservoirBytes = 0;
    m_smokeCleanRtxdiDiTemporalReservoirBytes = 0;
    m_smokeCleanRtxdiDiPreviousReservoirBytes = 0;
    m_smokeCleanRtxdiDiSpatialReservoirBytes = 0;
    m_smokeCleanRtxdiDiPreviousReservoirValid = false;
    m_smokeCleanRtxdiDiHistorySignature = 0;
    m_smokeCleanRtxdiDiHistoryResetCount = 0;
    m_smokeCleanRtxdiDiPreviousReservoirResetReason = 1u;
    m_smokeCleanRtxdiDiBlueNoise.Release();
    m_smokeTextureDescriptorTable = nullptr;
    m_smokeActiveTextureTable.clear();
    m_smokeMaterialTableEntryCount = 0;
    m_smokeEmissiveTriangleCount = 0;
    m_smokeEmissiveStaticTriangleCount = 0;
    m_smokeLightCandidateCount = 0;
    m_smokeDoomAnalyticLightCount = 0;
    m_smokeDoomAnalyticPortalRegionLightCount = 0;
    m_smokeDoomAnalyticPreviousLightCount = 0;
    m_smokeDoomAnalyticCurrentIdentityCount = 0;
    m_smokeDoomAnalyticPreviousIdentityCount = 0;
    m_smokeDoomAnalyticRemapCount = 0;
    m_smokePreviousEmissiveTriangleCount = 0;
    m_smokeUnifiedLightCount = 0;
    m_smokeUnifiedPreviousLightCount = 0;
    m_smokeUnifiedLightRemapCount = 0;
    m_smokeRestirLightManagerCurrentPayloadCount = 0;
    m_smokeRestirLightManagerPreviousPayloadCount = 0;
    m_smokePreviousEmissiveTriangles.clear();
    m_retiredSmokeScenePackages.clear();
}

void PathTracePrimaryPass::CommitRayTracingSmokeSceneResources(const RtSmokeSceneResourceCommitDesc& desc)
{
    const RtPathTraceSceneInputs previousSceneInputs = m_sceneInputs;
    RtRetiredSmokeScenePackage previousPackage = CaptureRetiredRayTracingSmokeScenePackage();
    const bool previousPackageHasResources = HasRetainableRayTracingSmokeScenePackage();
    const bool packageHandlesChanged = previousPackageHasResources && SmokeScenePackageHandlesChanged(previousPackage, desc);
    const bool sceneTransitionChanged = PathTraceSceneTransitionChanged(previousSceneInputs, desc.sceneInputs);
    if (sceneTransitionChanged && r_pathTracingWaitForIdleOnPortalChange.GetInteger() != 0)
    {
        nvrhi::IDevice* device = deviceManager ? deviceManager->GetDevice() : nullptr;
        if (device)
        {
            device->waitForIdle();
        }
    }

    const uint64 currentFrame = static_cast<uint64>(Max(idLib::frameNumber, 0));
    const int retireFrames = idMath::ClampInt(0, 32, r_pathTracingSceneRetireFrames.GetInteger());
    ReleaseExpiredRetiredRayTracingSmokeScenePackages(currentFrame);
    if (retireFrames == 0 && !m_retiredSmokeScenePackages.empty())
    {
        m_retiredSmokeScenePackages.clear();
    }

    if ((sceneTransitionChanged || packageHandlesChanged) && previousPackageHasResources)
    {
        PushRetiredRayTracingSmokeScenePackage(
            previousPackage,
            currentFrame,
            retireFrames);
    }

    m_sceneInputs = desc.sceneInputs;
    m_smokeStaticVertexBuffer = desc.buffers.staticVertexBuffer;
    m_smokeStaticIndexBuffer = desc.buffers.staticIndexBuffer;
    m_smokeStaticTriangleClassBuffer = desc.buffers.staticTriangleClassBuffer;
    m_smokeStaticTriangleMaterialBuffer = desc.buffers.staticTriangleMaterialBuffer;
    m_smokeStaticTriangleMaterialIndexBuffer = desc.buffers.staticTriangleMaterialIndexBuffer;
    m_smokePreviousStaticVertexBuffer = desc.buffers.previousStaticVertexBuffer;
    m_smokePreviousStaticIndexBuffer = desc.buffers.previousStaticIndexBuffer;
    m_smokePreviousStaticTriangleClassBuffer = desc.buffers.previousStaticTriangleClassBuffer;
    m_smokePreviousStaticTriangleMaterialBuffer = desc.buffers.previousStaticTriangleMaterialBuffer;
    m_smokePreviousStaticTriangleMaterialIndexBuffer = desc.buffers.previousStaticTriangleMaterialIndexBuffer;
    m_smokeDynamicVertexBuffer = desc.buffers.dynamicVertexBuffer;
    m_smokeDynamicIndexBuffer = desc.buffers.dynamicIndexBuffer;
    m_smokeDynamicTriangleClassBuffer = desc.buffers.dynamicTriangleClassBuffer;
    m_smokeDynamicTriangleMaterialBuffer = desc.buffers.dynamicTriangleMaterialBuffer;
    m_smokeDynamicTriangleMaterialIndexBuffer = desc.buffers.dynamicTriangleMaterialIndexBuffer;
    m_smokeMaterialTableBuffer = desc.buffers.materialTableBuffer;
    m_smokeMaterialFeatureBuffer = desc.buffers.materialFeatureBuffer;
    m_smokeMaterialFeatureParameterBuffer = desc.buffers.materialFeatureParameterBuffer;
    m_smokeDynamicMaterialBuffer = desc.buffers.dynamicMaterialBuffer;
    m_smokeEmissiveTriangleBuffer = desc.buffers.emissiveTriangleBuffer;
    m_smokePreviousEmissiveTriangleBuffer = desc.buffers.previousEmissiveTriangleBuffer;
    m_smokeEmissiveRemapBuffer = desc.buffers.emissiveRemapBuffer;
    m_smokeEmissiveDistributionBuffer = desc.buffers.emissiveDistributionBuffer;
    m_smokeLightCandidateBuffer = desc.buffers.lightCandidateBuffer;
    m_smokeDoomAnalyticLightBuffer = desc.buffers.doomAnalyticLightBuffer;
    m_smokeDoomAnalyticPreviousLightBuffer = desc.buffers.doomAnalyticPreviousLightBuffer;
    m_smokeDoomAnalyticCurrentIdentityBuffer = desc.buffers.doomAnalyticCurrentIdentityBuffer;
    m_smokeDoomAnalyticPreviousIdentityBuffer = desc.buffers.doomAnalyticPreviousIdentityBuffer;
    m_smokeDoomAnalyticRemapBuffer = desc.buffers.doomAnalyticRemapBuffer;
    m_smokeUnifiedLightBuffer = desc.buffers.unifiedLightBuffer;
    m_smokeUnifiedPreviousLightBuffer = desc.buffers.unifiedPreviousLightBuffer;
    m_smokeUnifiedLightRemapBuffer = desc.buffers.unifiedLightRemapBuffer;
    m_smokeRestirLightManagerCurrentToPreviousBuffer = desc.buffers.restirLightManagerCurrentToPreviousBuffer;
    m_smokeRestirLightManagerPreviousToCurrentBuffer = desc.buffers.restirLightManagerPreviousToCurrentBuffer;
    m_smokeRestirLightManagerCurrentPayloadBuffer = desc.buffers.restirLightManagerCurrentPayloadBuffer;
    m_smokeRestirLightManagerPreviousPayloadBuffer = desc.buffers.restirLightManagerPreviousPayloadBuffer;
    m_smokeRigidRouteVertexBuffer = desc.buffers.rigidRouteVertexBuffer;
    m_smokeRigidRouteIndexBuffer = desc.buffers.rigidRouteIndexBuffer;
    m_smokeRigidRouteTriangleMaterialBuffer = desc.buffers.rigidRouteTriangleMaterialBuffer;
    m_smokeRigidRouteTriangleMaterialIndexBuffer = desc.buffers.rigidRouteTriangleMaterialIndexBuffer;
    m_smokeRigidRouteInstanceBuffer = desc.buffers.rigidRouteInstanceBuffer;
    m_smokeSkinnedSourceVertexBuffer = desc.buffers.skinnedSourceVertexBuffer;
    m_smokeSkinnedCurrentOutputVertexBuffer = desc.buffers.skinnedCurrentOutputVertexBuffer;
    m_smokeSkinnedPreviousPositionBuffer = desc.buffers.skinnedPreviousPositionBuffer;
    m_smokeSkinnedSurfaceDispatchBuffer = desc.buffers.skinnedSurfaceDispatchBuffer;
    m_smokeSkinnedTriangleDispatchIndexBuffer = desc.buffers.skinnedTriangleDispatchIndexBuffer;
    m_smokeSkinnedCurrentJointMatrixBuffer = desc.buffers.skinnedCurrentJointMatrixBuffer;
    m_smokeSkinnedPreviousJointMatrixBuffer = desc.buffers.skinnedPreviousJointMatrixBuffer;
    m_smokeStaticBlasDesc = desc.staticBlasDesc;
    m_smokeStaticBlas = desc.staticBlas;
    m_smokeDynamicBlas = desc.dynamicBlas;
    m_smokeTlas = desc.tlas;
    if (desc.hasStaticBlas)
    {
        m_smokeStaticBlasCacheValid = true;
        m_smokeStaticBlasSignature = desc.staticBlasSignature;
        m_smokeStaticBlasGeometryGeneration = desc.staticBlasGeometryGeneration;
    }
    m_smokeBindingSet = desc.bindingSet;
    m_smokeTextureDescriptorTable = desc.textureDescriptorTable;
    m_smokeActiveTextureTable = desc.activeTextureTable;
    m_smokeSkyEnvironmentCube = desc.skyEnvironmentCube;
    m_smokeSkyCubeProbeBindingSet = desc.skyCubeProbeBindingSet;
    m_smokeMaterialTableEntryCount = desc.materialTableEntryCount;
    m_smokeEmissiveTriangleCount = desc.emissiveTriangleCount;
    m_smokeEmissiveStaticTriangleCount = desc.emissiveStaticTriangleCount;
    m_smokeLightCandidateCount = desc.lightCandidateCount;
    m_smokeDoomAnalyticLightCount = desc.doomAnalyticLightCount;
    m_smokeDoomAnalyticPortalRegionLightCount = desc.doomAnalyticPortalRegionLightCount;
    m_smokeDoomAnalyticPreviousLightCount = desc.doomAnalyticPreviousLightCount;
    m_smokeDoomAnalyticCurrentIdentityCount = desc.doomAnalyticCurrentIdentityCount;
    m_smokeDoomAnalyticPreviousIdentityCount = desc.doomAnalyticPreviousIdentityCount;
    m_smokeDoomAnalyticRemapCount = desc.doomAnalyticRemapCount;
    m_smokePreviousEmissiveTriangleCount = desc.previousEmissiveTriangleCount;
    m_smokeUnifiedLightCount = desc.unifiedLightCount;
    m_smokeUnifiedPreviousLightCount = desc.unifiedPreviousLightCount;
    m_smokeUnifiedLightRemapCount = desc.unifiedLightRemapCount;
    m_smokeRestirLightManagerCurrentPayloadCount = desc.restirLightManagerCurrentPayloadCount;
    m_smokeRestirLightManagerPreviousPayloadCount = desc.restirLightManagerPreviousPayloadCount;
    const uint64_t uploadBytes =
        desc.sceneInputs.diagnostics.geometryUploadBytes +
        desc.sceneInputs.diagnostics.materialUploadBytes +
        desc.sceneInputs.diagnostics.lightUploadBytes;
    m_frameResources.RecordSceneResourceCommit(uploadBytes, desc.bindingSet != nullptr, desc.staticBlas != nullptr || desc.dynamicBlas != nullptr);
    m_smokeSceneBuilt = true;
}
