#include "precompiled.h"
#pragma hdrstop

#include "PathTraceOwnerSemanticKernel.h"
#include "PathTraceCpuProducerPublish.h"
#include "PathTraceMaterialIdKernel.h"
#include "PathTraceRigidIdentity.h"

#include <algorithm>

namespace {

bool SmokeMaterialCanPromoteRigidEmissiveCardFromPodInternal(
    const RtSmokeMaterialRouteInput& material,
    bool allowSwinglightRuntimeState,
    const RtSmokeTranslucentClassifierInfo& classifier)
{
    if (!material.materialPresent || material.deform != DFRM_NONE ||
        material.coverage != MC_TRANSLUCENT)
    {
        return false;
    }
    if (!allowSwinglightRuntimeState &&
        idStr::FindText(material.materialName, "swinglight", false) >= 0)
    {
        return false;
    }
    if (classifier.hasScreenTexgen ||
        classifier.hasAddDefault0200Texture ||
        classifier.nameLooksGui ||
        classifier.nameLooksParticle ||
        classifier.nameLooksDecal ||
        classifier.nameLooksGlass ||
        classifier.sortIsPostProcess ||
        classifier.sortIsGuiOrSubview ||
        classifier.sortIsDecal ||
        classifier.polygonOffsetDecal)
    {
        return false;
    }

    const bool hasEmissiveCardStage =
        classifier.hasAdditiveBlend ||
        classifier.hasAmbientBlendStage ||
        (classifier.hasAmbientStage && !classifier.hasDiffuseStage);

    return hasEmissiveCardStage;
}

bool FillPathTraceCaptureRigidReadyFacts(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    RtPathTraceCaptureRawSurface& raw,
    const RtPathTraceCaptureSemanticFactsProvider& facts,
    bool& readyByMesh,
    bool& readyByResident)
{
    readyByMesh = false;
    readyByResident = false;
    raw.derivedRuntimeMaterial = {};
    raw.derivedRuntimeMaterialPresent = false;
    raw.derivedMeshHash = 0;
    raw.derivedChosenMaterialId = 0;
    raw.derivedMaterialClassSignature = 0;
    raw.derivedResolvedModelSurfaceIndex = -1;
    raw.derivedBaseMaterialId = 0;
    raw.rigidIdentityPresent = false;
    if (raw.classifierStageOffset > snapshot.classifierStages.size() ||
        raw.classifierStageCount >
            snapshot.classifierStages.size() - raw.classifierStageOffset ||
        raw.runtimeStageOffset > snapshot.runtimeStages.size() ||
        raw.runtimeStageCount >
            snapshot.runtimeStages.size() - raw.runtimeStageOffset ||
        raw.registerOffset > snapshot.registers.size() ||
        raw.registerCount > snapshot.registers.size() - raw.registerOffset)
    {
        return false;
    }

    const std::uint64_t* tokens = nullptr;
    std::size_t tokenCount = 0;
    if (raw.modelTableIndex != RT_PT_CAPTURE_INVALID_MODEL_TABLE)
    {
        if (raw.modelTableIndex >= snapshot.modelTables.size())
        {
            return false;
        }
        const RtPathTraceCaptureModelTokenTablePod& table =
            snapshot.modelTables[raw.modelTableIndex];
        if (table.modelBits != raw.modelBits || table.modelEpoch != raw.modelEpoch ||
            table.tokenOffset > snapshot.modelSurfaceTokens.size() ||
            table.tokenCount > snapshot.modelSurfaceTokens.size() - table.tokenOffset)
        {
            return false;
        }
        tokens = table.tokenCount != 0
            ? snapshot.modelSurfaceTokens.data() + table.tokenOffset : nullptr;
        tokenCount = table.tokenCount;
    }
    else if (raw.modelBits != 0)
    {
        return false;
    }

    RtSmokeSurfaceClassifyInput classifyInput = raw.classify;
    for (std::uint32_t stageIndex = 0;
        stageIndex < raw.runtimeStageCount; ++stageIndex)
    {
        classifyInput.material.hasAlphaTest =
            classifyInput.material.hasAlphaTest ||
            snapshot.runtimeStages[raw.runtimeStageOffset + stageIndex].hasAlphaTest;
    }
    const RtSmokeTranslucentClassifierInfo classifier =
        BuildSmokeTranslucentClassifierInfo(raw.classifier,
            raw.classifierStageCount != 0
                ? snapshot.classifierStages.data() + raw.classifierStageOffset
                : nullptr,
            static_cast<std::int32_t>(raw.classifierStageCount));
    const RtSmokeSurfaceClass classified =
        ClassifySmokeSurfaceFromPod(classifyInput, classifier);
    const std::uint32_t baseMaterialId =
        HashPathTraceMaterialName(raw.materialName);
    const float origin[3] = {
        raw.modelMatrix[0] * raw.boundsCenter[0] +
            raw.modelMatrix[4] * raw.boundsCenter[1] +
            raw.modelMatrix[8] * raw.boundsCenter[2] + raw.modelMatrix[12],
        raw.modelMatrix[1] * raw.boundsCenter[0] +
            raw.modelMatrix[5] * raw.boundsCenter[1] +
            raw.modelMatrix[9] * raw.boundsCenter[2] + raw.modelMatrix[13],
        raw.modelMatrix[2] * raw.boundsCenter[0] +
            raw.modelMatrix[6] * raw.boundsCenter[1] +
            raw.modelMatrix[10] * raw.boundsCenter[2] + raw.modelMatrix[14]
    };
    const RtPathTraceRuntimeMaterialEvalPod runtimeEval =
        BuildPathTraceRuntimeMaterialEvalFromPod(true, baseMaterialId,
            raw.runtimeStageCount != 0
                ? snapshot.runtimeStages.data() + raw.runtimeStageOffset : nullptr,
            raw.runtimeStageCount,
            raw.registersPresent && raw.registerCount != 0
                ? snapshot.registers.data() + raw.registerOffset : nullptr,
            raw.registerCount,
            SmokeMaterialUsesOpaqueSwinglightCompatibilityFromPod(
                classifyInput.material, classifier),
            origin, true);
    const RtPathTraceRuntimeMaterialVariantPod variantKey =
        BuildPathTraceRuntimeMaterialVariantKeyFromPod(
            baseMaterialId, raw.entityIndex, raw.entityNum,
            raw.requestedModelSurfaceIndex, raw.currentTriToken,
            tokens, tokenCount);
    const RtPathTraceRuntimeMaterialDecisionPod materialDecision =
        BuildPathTraceRuntimeMaterialDecisionFromPod(
            variantKey, runtimeEval,
            snapshot.variantBases.data(), snapshot.variantBases.size(),
            snapshot.registryMaterials.data(), snapshot.registryMaterials.size());
    RtPathTracePersistDerivedRuntimeMaterialDecision(
        raw, baseMaterialId, materialDecision);

    if (!snapshot.removeRoutedRigidDynamic)
    {
        return true;
    }

    const auto registryMaterial = std::lower_bound(
        snapshot.registryMaterials.begin(), snapshot.registryMaterials.end(),
        baseMaterialId,
        [](const RtPathTraceCaptureRegistryMaterialPod& lhs, std::uint32_t rhs)
        {
            return lhs.materialId < rhs;
        });
    const bool liquidPool = raw.liquidPoolEnabled &&
        registryMaterial != snapshot.registryMaterials.end() &&
        registryMaterial->materialId == baseMaterialId &&
        registryMaterial->detailDecalLiquidPool;
    const bool commonPromotionFacts =
        classified == RtSmokeSurfaceClass::ParticleAlpha &&
        classifyInput.hasEntityDef && !classifyInput.material.guiSurface &&
        !classifyInput.hasJointCache && !classifyInput.hasStaticModelWithJoints &&
        !classifyInput.hasRenderEntityJoints && !raw.entityCallbackPresent &&
        !raw.entityForceUpdate && !raw.dynamicModelPresent &&
        !raw.cachedDynamicModelPresent &&
        classifyInput.material.deform == DFRM_NONE &&
        classifyInput.modelDepthHack == 0.0f;
    const bool promotedEmissive = snapshot.rigidRouteEmissiveCards &&
        commonPromotionFacts &&
        SmokeMaterialCanPromoteRigidEmissiveCardFromPod(
            classifyInput.material, false, classifier);
    if (!(promotedEmissive || (commonPromotionFacts && liquidPool)) &&
        classified != RtSmokeSurfaceClass::RigidEntity)
    {
        return true;
    }

    const RtSmokeSurfaceClass surfaceClass = RtSmokeSurfaceClass::RigidEntity;
    const RtSmokeTranslucentSubtype subtype =
        ClassifySmokeTranslucentSubtypeFromPod(classifyInput.material, classifier);
    const std::uint32_t materialClassSignature =
        SmokeMaterialRouteClassSignatureFromPod(
            classifyInput.material, surfaceClass, subtype, classifier);
    const std::int32_t resolvedModelSurfaceIndex = variantKey.modelSurfaceIndex;
    RtPathTraceRigidMeshIdentityPod meshIdentity;
    meshIdentity.modelIdentity = raw.modelBits;
    meshIdentity.modelEpoch = static_cast<std::uint32_t>(raw.modelEpoch);
    meshIdentity.modelSurfaceIndex = resolvedModelSurfaceIndex;
    meshIdentity.jointIndex = raw.jointIndex;
    meshIdentity.vertexBufferIdentity = static_cast<std::uintptr_t>(
        cpu_producer_publish::RigidMeshVertexCacheIdentity(raw.ambientHandle));
    meshIdentity.indexBufferIdentity = static_cast<std::uintptr_t>(
        cpu_producer_publish::RigidMeshIndexCacheIdentity(raw.indexHandle));
    meshIdentity.numVerts = static_cast<int>(raw.vertexCount);
    meshIdentity.numIndexes = static_cast<int>(raw.sourceTriIndexCount);
    meshIdentity.vertexFormat = static_cast<std::uint32_t>(
        RtSmokeGeometryBufferFormat::LegacySmokeVertex);
    meshIdentity.materialId = materialDecision.chosenMaterialId;
    meshIdentity.materialClassSignature = materialClassSignature;
    meshIdentity.sourceKind = SmokeSurfaceClassId(surfaceClass);
    const std::uint64_t meshHash = BuildPathTraceRigidMeshHashFromPod(meshIdentity);
    raw.derivedMeshHash = meshHash;
    raw.derivedMaterialClassSignature = materialClassSignature;
    raw.derivedResolvedModelSurfaceIndex = resolvedModelSurfaceIndex;
    raw.rigidIdentityPresent = true;
    readyByMesh = facts.IsRigidRouteReady(meshHash);
    readyByResident = !readyByMesh && promotedEmissive &&
        facts.IsRigidRouteResidentReadyForEntityMaterial(
            raw.entityIndex, raw.entityNum, materialDecision.chosenMaterialId);
    return true;
}

bool DerivePathTraceActiveEmissiveStageFromPod(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    const RtPathTraceCaptureRawSurface& raw,
    const RtSmokeTranslucentClassifierInfo& classifier)
{
    const bool nameLooksEmissive = !classifier.hasAddDefault0200Texture &&
        (classifier.nameLooksGlow || classifier.nameLooksSignage);
    const float* registers = raw.registersPresent && raw.registerCount != 0
        ? snapshot.registers.data() + raw.registerOffset : nullptr;
    for (std::uint32_t stageIndex = 0;
        stageIndex < raw.runtimeStageCount; ++stageIndex)
    {
        const RtPathTraceRuntimeMaterialStagePod& stage =
            snapshot.runtimeStages[raw.runtimeStageOffset + stageIndex];
        if (!stage.valid || stage.lighting != SL_AMBIENT || !stage.imagePresent)
        {
            continue;
        }
        if (registers && stage.conditionRegister >= 0 &&
            static_cast<std::uint32_t>(stage.conditionRegister) < raw.registerCount &&
            registers[stage.conditionRegister] == 0.0f)
        {
            continue;
        }
        const std::uint64_t srcBlend = stage.drawStateBits & GLS_SRCBLEND_BITS;
        const std::uint64_t dstBlend = stage.drawStateBits & GLS_DSTBLEND_BITS;
        const bool additive =
            (srcBlend == GLS_SRCBLEND_ONE ||
                srcBlend == GLS_SRCBLEND_SRC_ALPHA) &&
            dstBlend == GLS_DSTBLEND_ONE;
        const bool ambientBlend = dstBlend != GLS_DSTBLEND_ZERO ||
            srcBlend == GLS_SRCBLEND_DST_COLOR ||
            srcBlend == GLS_SRCBLEND_ONE_MINUS_DST_COLOR;
        if (!additive &&
            !(nameLooksEmissive && ambientBlend && !classifier.nameLooksDecal))
        {
            continue;
        }
        float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        if (registers)
        {
            for (int component = 0; component < 4; ++component)
            {
                const int colorRegister = stage.colorRegisters[component];
                if (colorRegister >= 0 &&
                    static_cast<std::uint32_t>(colorRegister) < raw.registerCount)
                {
                    color[component] = registers[colorRegister];
                }
            }
        }
        if (Max(color[0], Max(color[1], color[2])) * Max(color[3], 0.0f) >
            1.0e-4f)
        {
            return true;
        }
    }
    return false;
}

}

RtSmokeTranslucentClassifierInfo BuildSmokeTranslucentClassifierInfo(
    const RtSmokeTranslucentClassifierInput& input,
    const RtSmokeTranslucentClassifierStageInput* stages,
    std::int32_t stageCount)
{
    if (stageCount != input.stageCount ||
        (stageCount > 0 && !stages))
    {
        return RtSmokeTranslucentClassifierInfo();
    }
    return BuildSmokeTranslucentClassifierInfoFromSource(
        input,
        input.materialName,
        stageCount,
        [stages](std::int32_t stageIndex) {
            return stages[stageIndex];
        });
}

bool SmokeMaterialCanPromoteRigidEmissiveCardFromPod(
    const RtSmokeMaterialRouteInput& material,
    bool allowSwinglightRuntimeState,
    const RtSmokeTranslucentClassifierInfo& classifier)
{
    return SmokeMaterialCanPromoteRigidEmissiveCardFromPodInternal(
        material, allowSwinglightRuntimeState, classifier);
}

bool SmokeMaterialUsesOpaqueSwinglightCompatibilityFromPod(
    const RtSmokeMaterialRouteInput& input,
    const RtSmokeTranslucentClassifierInfo& classifier)
{
    return input.materialPresent && input.coverage == static_cast<int>(MC_TRANSLUCENT) &&
        input.deform == static_cast<int>(DFRM_NONE) && input.stageCount == 1 &&
        idStr::Icmp(input.materialName,
            "models/mapobjects/swinglights/work/swinglighttex2") == 0 &&
        input.singleStageAmbientAlphaBlend &&
        classifier.hasAmbientBlendStage &&
        !classifier.hasDiffuseStage &&
        !classifier.hasAdditiveBlend &&
        !classifier.hasScreenTexgen &&
        !classifier.hasAddDefault0200Texture;
}

uint32_t SmokeSurfaceClassId(RtSmokeSurfaceClass surfaceClass)
{
    switch (surfaceClass)
    {
        case RtSmokeSurfaceClass::StaticWorld:
            return 0;
        case RtSmokeSurfaceClass::RigidEntity:
            return 1;
        case RtSmokeSurfaceClass::SkinnedDeformed:
            return 2;
        case RtSmokeSurfaceClass::ParticleAlpha:
            return 3;
        default:
            return 4;
    }
}

uint32_t SmokeTranslucentSubtypeId(RtSmokeTranslucentSubtype subtype)
{
    switch (subtype)
    {
        case RtSmokeTranslucentSubtype::DecalGrime:
            return 0;
        case RtSmokeTranslucentSubtype::ObjectGlass:
            return 1;
        case RtSmokeTranslucentSubtype::SmokeParticle:
            return 2;
        case RtSmokeTranslucentSubtype::SignageGlow:
            return 3;
        case RtSmokeTranslucentSubtype::GuiScreen:
            return 5;
        case RtSmokeTranslucentSubtype::PortalWindow:
            return 4;
        default:
            return 6;
    }
}

RtSmokeSurfaceClass ClassifySmokeSurfaceFromPod(
    const RtSmokeSurfaceClassifyInput& input,
    const RtSmokeTranslucentClassifierInfo& classifier)
{
    if (input.hasJointCache || input.hasStaticModelWithJoints ||
        input.hasRenderEntityJoints)
    {
        return RtSmokeSurfaceClass::SkinnedDeformed;
    }
    const bool opaqueSwinglight =
        SmokeMaterialUsesOpaqueSwinglightCompatibilityFromPod(input.material, classifier);
    const int deform = input.material.deform;
    if (input.material.materialPresent &&
        (input.material.guiSurface ||
        (!opaqueSwinglight && input.material.coverage == static_cast<int>(MC_TRANSLUCENT)) ||
        deform == static_cast<int>(DFRM_SPRITE) ||
        deform == static_cast<int>(DFRM_TUBE) ||
        deform == static_cast<int>(DFRM_FLARE) ||
        deform == static_cast<int>(DFRM_PARTICLE) ||
        deform == static_cast<int>(DFRM_PARTICLE2) ||
        (!opaqueSwinglight && input.material.sort >= SS_MEDIUM) ||
        input.modelDepthHack != 0.0f))
    {
        return RtSmokeSurfaceClass::ParticleAlpha;
    }
    if (input.isWorldSpace || !input.hasEntityDef ||
        (input.ambientCacheIsStatic && input.indexCacheIsStatic && !input.hasEntityDef))
    {
        return RtSmokeSurfaceClass::StaticWorld;
    }
    return input.hasEntityDef ? RtSmokeSurfaceClass::RigidEntity : RtSmokeSurfaceClass::Unknown;
}

RtSmokeTranslucentSubtype ClassifySmokeTranslucentSubtypeFromPod(
    const RtSmokeMaterialRouteInput& input,
    const RtSmokeTranslucentClassifierInfo& info)
{
    if (!input.materialPresent)
    {
        return RtSmokeTranslucentSubtype::Unknown;
    }

    if (input.guiSurface || info.hasScreenTexgen || info.nameLooksGui)
    {
        return RtSmokeTranslucentSubtype::GuiScreen;
    }

    if (info.sortIsPostProcess || info.sortIsGuiOrSubview)
    {
        return RtSmokeTranslucentSubtype::PortalWindow;
    }

    if (input.deform == static_cast<int>(DFRM_PARTICLE) ||
        input.deform == static_cast<int>(DFRM_PARTICLE2) ||
        input.deform == static_cast<int>(DFRM_SPRITE) ||
        input.deform == static_cast<int>(DFRM_TUBE) ||
        input.deform == static_cast<int>(DFRM_FLARE) ||
        input.sort >= SS_ALMOST_NEAREST ||
        info.nameLooksParticle)
    {
        return RtSmokeTranslucentSubtype::SmokeParticle;
    }

    if (info.nameLooksGlass)
    {
        return RtSmokeTranslucentSubtype::ObjectGlass;
    }

    if (info.hasAddDefault0200Texture)
    {
        return RtSmokeTranslucentSubtype::Unknown;
    }

    if (info.hasAdditiveBlend ||
        (info.hasAmbientStage && !info.hasDiffuseStage && info.nameLooksGlow) ||
        (info.hasAmbientBlendStage && info.nameLooksGlow) ||
        (info.nameLooksGlow && !info.nameLooksDecal) ||
        info.nameLooksSignage)
    {
        return RtSmokeTranslucentSubtype::SignageGlow;
    }

    if (info.sortIsDecal || info.polygonOffsetDecal || info.nameLooksDecal)
    {
        return RtSmokeTranslucentSubtype::DecalGrime;
    }

    return RtSmokeTranslucentSubtype::Unknown;
}

uint32_t SmokeMaterialRouteClassSignatureFromPod(
    const RtSmokeMaterialRouteInput& input,
    RtSmokeSurfaceClass surfaceClass,
    RtSmokeTranslucentSubtype subtype,
    const RtSmokeTranslucentClassifierInfo& classifier)
{
    uint32_t signature =
        (SmokeSurfaceClassId(surfaceClass) & 0x0fu) |
        ((SmokeTranslucentSubtypeId(subtype) & 0x0fu) << 4);
    if (!input.materialPresent)
    {
        return signature;
    }
    const uint32_t coverage = static_cast<uint32_t>(input.coverage) & 0x0fu;
    const uint32_t deform = static_cast<uint32_t>(input.deform) & 0x0fu;
    const bool routeSortMediumOrLater = input.sort >= SS_MEDIUM;

    signature |= coverage << 8;
    signature |= deform << 12;
    signature |= input.hasAlphaTest ? (1u << 16) : 0u;
    signature |= routeSortMediumOrLater ? (1u << 17) : 0u;
    signature |= classifier.hasScreenTexgen ? (1u << 18) : 0u;
    signature |= classifier.hasAdditiveBlend ? (1u << 19) : 0u;
    signature |= classifier.hasAmbientBlendStage ? (1u << 20) : 0u;
    signature |= classifier.hasDiffuseStage ? (1u << 21) : 0u;
    signature |= classifier.hasAddDefault0200Texture ? (1u << 22) : 0u;
    signature |= classifier.nameLooksGui ? (1u << 23) : 0u;
    signature |= classifier.nameLooksParticle ? (1u << 24) : 0u;
    signature |= classifier.nameLooksDecal ? (1u << 25) : 0u;
    signature |= classifier.nameLooksGlass ? (1u << 26) : 0u;
    signature |= classifier.nameLooksGlow ? (1u << 27) : 0u;
    signature |= classifier.nameLooksSignage ? (1u << 28) : 0u;
    return signature;
}

bool FinalizePathTraceOwnerSemanticSnapshot(
    RtPathTraceCaptureOwnerSnapshot& snapshot,
    const RtPathTraceCaptureSemanticFactsProvider& facts,
    const PtSkinnedHitRouteRecord* skinnedAdmissionRoutes,
    std::size_t skinnedAdmissionRouteCount,
    bool skinnedCaptureSplitGate)
{
    if (skinnedAdmissionRouteCount != 0 && !skinnedAdmissionRoutes)
    {
        return false;
    }
    for (RtPathTraceCaptureRawSurface& raw : snapshot.surfaces)
    {
        if (!RtPathTraceCaptureSourceSurfaceComplete(raw,
                snapshot.classifierStages.size(), snapshot.runtimeStages.size(),
                snapshot.registers.size(), snapshot.vertices.size(),
                snapshot.indexes.size(), snapshot.joints.size()))
        {
            return false;
        }
        if (raw.safety != RtPathTraceCaptureSafetyDisposition::Ready)
        {
            raw.semanticFactsDerived = true;
            raw.semanticFactsPresent = true;
            continue;
        }
        const RtSmokeTranslucentClassifierInfo classifier =
            BuildSmokeTranslucentClassifierInfo(raw.classifier,
                raw.classifierStageCount != 0
                    ? snapshot.classifierStages.data() + raw.classifierStageOffset
                    : nullptr,
                static_cast<std::int32_t>(raw.classifierStageCount));
        raw.activeEmissiveStage = DerivePathTraceActiveEmissiveStageFromPod(
            snapshot, raw, classifier);
        if (skinnedCaptureSplitGate &&
            ClassifySmokeSurfaceFromPod(raw.classify, classifier) ==
                RtSmokeSurfaceClass::SkinnedDeformed)
        {
            const PtSkinnedHitRouteRecord* priorRoute = nullptr;
            for (std::size_t routeIndex = 0;
                routeIndex < skinnedAdmissionRouteCount; ++routeIndex)
            {
                if (skinnedAdmissionRoutes[routeIndex].instanceKey ==
                    raw.canonicalInstance)
                {
                    priorRoute = &skinnedAdmissionRoutes[routeIndex];
                    break;
                }
            }
            const PtGeometryIdentityBinding* binding =
                facts.FindCanonicalIdentityBinding(raw.canonicalInstance);
            const PtGeometrySourceRecord* source = binding
                ? facts.FindCanonicalSourceRecord(binding->meshKey) : nullptr;
            PtSkinnedCaptureAdmissionInput admission;
            admission.gate = skinnedCaptureSplitGate;
            admission.currentInstance = raw.canonicalInstance;
            if (binding)
            {
                admission.currentMesh = binding->meshKey;
            }
            if (source)
            {
                admission.currentSourceChecksum = source->sourceChecksum;
                admission.currentVertexCount = static_cast<std::uint32_t>(
                    source->payload.positions.size());
                admission.currentIndexCount = static_cast<std::uint32_t>(
                    source->payload.indexes.size());
            }
            admission.jointDataReady = raw.rtCpuSkinned &&
                raw.jointCount > 0 && raw.jointSource != 0;
            admission.priorRouteLive = priorRoute != nullptr;
            admission.priorRoute = priorRoute;
            const PtSkinnedCaptureAdmissionResult admissionResult =
                PtPlanSkinnedCaptureAdmission(admission);
            raw.skinnedCaptureAdmission = static_cast<std::uint32_t>(
                admissionResult);
            if (admissionResult ==
                PtSkinnedCaptureAdmissionResult::OmitCpuCapture)
            {
                raw.skinnedOmitVertexCount = admission.currentVertexCount;
                raw.skinnedOmitIndexCount = admission.currentIndexCount;
            }
        }
        if (!FillPathTraceCaptureRigidReadyFacts(snapshot, raw, facts,
                raw.rigidReadyByMesh, raw.rigidReadyByResident))
        {
            raw.safety = RtPathTraceCaptureSafetyDisposition::CopyFailed;
        }
        raw.semanticFactsDerived = true;
        raw.semanticFactsPresent = true;
    }
    return true;
}
