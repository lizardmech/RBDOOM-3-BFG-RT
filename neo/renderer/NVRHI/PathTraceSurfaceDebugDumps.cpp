#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCVars.h"
#include "PathTraceSurfaceDebugDumps.h"
#include "PathTraceDebugDumps.h"
#include "PathTraceDoomMaterialClassifier.h"
#include "PathTraceGuiSurfaces.h"
#include "PathTraceGeometryUniverse.h"
#include "PathTraceMaterialFeatureParameters.h"
#include "PathTracePrimarySurface.h"
#include "PathTraceSceneCapture.h"
#include "PathTraceSurfaceClassification.h"
#include "PathTraceTextureRegistry.h"

#include <algorithm>

namespace {

struct RtCrosshairMaterialFeatureDebug
{
    RtPathTraceMaterialKind materialKind = RT_PATH_TRACE_MATERIAL_KIND_UNKNOWN;
    uint32_t materialCaps = RT_PATH_TRACE_MATERIAL_CAP_DEBUG_FAIL_CLOSED;
    uint32_t lobeCaps = 0;
    uint32_t passSupport = RT_PATH_TRACE_MATERIAL_PASS_DEBUG_VISUALIZER;
    RtPathTraceMaterialModifierKind modifierKind = RT_PATH_TRACE_MATERIAL_MODIFIER_NONE;
    bool transmissionCandidate = false;
    bool transmissionActive = false;
};

struct RtCrosshairOrderedCompositingProjection
{
    int alphaClipStage = -1;
    int additiveStage = -1;
    int multiplyStage = -1;
    int invertedStage = -1;
    int alphaOverStage = -1;
    int destinationPreserveStage = -1;
    int unknownStage = -1;
    bool overflow = false;
    bool needsStageTextureConsumer = false;
};

RtCrosshairOrderedCompositingProjection BuildCrosshairOrderedCompositingProjection(const RtMaterialRecord* record)
{
    RtCrosshairOrderedCompositingProjection projection;
    if (!record)
    {
        return projection;
    }

    projection.overflow = record->compositingStages.size() > 8;
    std::vector<idStr> effectImageNames;
    for (const RtMaterialCompositingStageFact& stage : record->compositingStages)
    {
        const bool destinationPreserve =
            stage.srcBlendBits == GLS_SRCBLEND_ZERO &&
            stage.dstBlendBits == GLS_DSTBLEND_ONE;
        if (destinationPreserve && projection.destinationPreserveStage < 0)
        {
            projection.destinationPreserveStage = stage.stageIndex;
        }

        switch (stage.operation)
        {
            case RtMaterialCompositingOp::AuthoredAlphaClip:
                if (projection.alphaClipStage < 0)
                {
                    projection.alphaClipStage = stage.stageIndex;
                }
                break;
            case RtMaterialCompositingOp::Additive:
                if (projection.additiveStage < 0)
                {
                    projection.additiveStage = stage.stageIndex;
                }
                if (!stage.imageName.IsEmpty() &&
                    std::find(effectImageNames.begin(), effectImageNames.end(), stage.imageName) == effectImageNames.end())
                {
                    effectImageNames.push_back(stage.imageName);
                }
                break;
            case RtMaterialCompositingOp::MultiplyFilter:
                if (projection.multiplyStage < 0)
                {
                    projection.multiplyStage = stage.stageIndex;
                }
                if (!stage.imageName.IsEmpty() &&
                    std::find(effectImageNames.begin(), effectImageNames.end(), stage.imageName) == effectImageNames.end())
                {
                    effectImageNames.push_back(stage.imageName);
                }
                break;
            case RtMaterialCompositingOp::InvertedFilterBlackKey:
                if (projection.invertedStage < 0)
                {
                    projection.invertedStage = stage.stageIndex;
                }
                if (!stage.imageName.IsEmpty() &&
                    std::find(effectImageNames.begin(), effectImageNames.end(), stage.imageName) == effectImageNames.end())
                {
                    effectImageNames.push_back(stage.imageName);
                }
                break;
            case RtMaterialCompositingOp::SourceAlphaOver:
                if (projection.alphaOverStage < 0)
                {
                    projection.alphaOverStage = stage.stageIndex;
                }
                if (!stage.imageName.IsEmpty() &&
                    std::find(effectImageNames.begin(), effectImageNames.end(), stage.imageName) == effectImageNames.end())
                {
                    effectImageNames.push_back(stage.imageName);
                }
                break;
            case RtMaterialCompositingOp::Unknown:
                if (!destinationPreserve && projection.unknownStage < 0)
                {
                    projection.unknownStage = stage.stageIndex;
                }
                break;
            default:
                break;
        }
    }

    // The packed eight-word record contains ordering and blend equations, but
    // cannot execute more than one effect image because it intentionally has no
    // per-stage texture descriptor/value payload yet.
    projection.needsStageTextureConsumer =
        effectImageNames.size() > 1 ||
        projection.unknownStage >= 0 ||
        projection.overflow;
    return projection;
}

const char* RtPathTraceMaterialKindName(const RtPathTraceMaterialKind materialKind)
{
    switch (materialKind)
    {
        case RT_PATH_TRACE_MATERIAL_KIND_OPAQUE: return "opaque";
        case RT_PATH_TRACE_MATERIAL_KIND_ALPHA_TESTED: return "alpha-tested";
        case RT_PATH_TRACE_MATERIAL_KIND_DECAL_MODIFIER: return "decal-modifier";
        case RT_PATH_TRACE_MATERIAL_KIND_LIQUID_POOL_MODIFIER: return "liquid-pool-modifier";
        case RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_GLASS: return "translucent-glass";
        case RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_PARTICLE: return "translucent-particle";
        case RT_PATH_TRACE_MATERIAL_KIND_GUI_SCREEN: return "gui-screen";
        case RT_PATH_TRACE_MATERIAL_KIND_EMISSIVE_SPECIAL: return "emissive-special";
        case RT_PATH_TRACE_MATERIAL_KIND_SKY_OR_ENVIRONMENT: return "sky-or-environment";
        default: return "unknown";
    }
}

const char* RtPathTraceMaterialModifierName(const RtPathTraceMaterialModifierKind modifierKind)
{
    switch (modifierKind)
    {
        case RT_PATH_TRACE_MATERIAL_MODIFIER_OVER: return "over";
        case RT_PATH_TRACE_MATERIAL_MODIFIER_MODULATE_FILTER: return "modulate-filter";
        case RT_PATH_TRACE_MATERIAL_MODIFIER_ADDITIVE_EMISSIVE: return "additive-emissive";
        case RT_PATH_TRACE_MATERIAL_MODIFIER_DIFFUSE_LIT: return "diffuse-lit";
        case RT_PATH_TRACE_MATERIAL_MODIFIER_LIQUID_POOL_UNION: return "liquid-pool-union";
        default: return "none";
    }
}

const char* RtPathTraceMaterialUnsupportedDebugName(const RtCrosshairMaterialFeatureDebug& feature)
{
    if (feature.materialKind == RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_GLASS)
    {
        return feature.transmissionActive ? "glass-transmission-path-only" : "glass-transmission-disabled-cyan";
    }
    if ((feature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_DEBUG_FAIL_CLOSED) == 0u)
    {
        return "supported-green";
    }
    if (feature.materialKind == RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_PARTICLE)
    {
        return "particle-unsupported-orange";
    }
    if (feature.materialKind == RT_PATH_TRACE_MATERIAL_KIND_GUI_SCREEN)
    {
        return "gui-unsupported-magenta";
    }
    return "unsupported-red";
}

RtPathTraceMaterialModifierKind BuildCrosshairMaterialModifierKind(const RtSmokeMaterialTextureInfo& info)
{
    if (info.liquidFilmCandidate)
    {
        return RT_PATH_TRACE_MATERIAL_MODIFIER_LIQUID_POOL_UNION;
    }
    if (info.detailDecalDiffuseLit)
    {
        return RT_PATH_TRACE_MATERIAL_MODIFIER_DIFFUSE_LIT;
    }
    if (info.filterDecal)
    {
        return RT_PATH_TRACE_MATERIAL_MODIFIER_MODULATE_FILTER;
    }
    if (info.additiveDecal)
    {
        return RT_PATH_TRACE_MATERIAL_MODIFIER_ADDITIVE_EMISSIVE;
    }
    if (info.detailDecal)
    {
        return RT_PATH_TRACE_MATERIAL_MODIFIER_OVER;
    }
    return RT_PATH_TRACE_MATERIAL_MODIFIER_NONE;
}

RtCrosshairMaterialFeatureDebug BuildCrosshairMaterialFeatureDebug(
    const RtSmokeSurfaceClass surfaceClass,
    const RtSmokeTranslucentSubtype translucentSubtype,
    const RtSmokeMaterialTextureInfo& info)
{
    RtCrosshairMaterialFeatureDebug feature;
    feature.modifierKind = BuildCrosshairMaterialModifierKind(info);

    const bool translucent = surfaceClass == RtSmokeSurfaceClass::ParticleAlpha;
    const bool translucentGlass =
        translucent &&
        (translucentSubtype == RtSmokeTranslucentSubtype::ObjectGlass ||
         translucentSubtype == RtSmokeTranslucentSubtype::PortalWindow);
    const bool fallbackGlass = info.objectGlassFallback || info.portalWindowFallback;
    const bool glassLike = translucentGlass || fallbackGlass;

    if (info.skyEnvironment)
    {
        feature.materialKind = RT_PATH_TRACE_MATERIAL_KIND_SKY_OR_ENVIRONMENT;
        feature.materialCaps = RT_PATH_TRACE_MATERIAL_CAP_SHADOW_OCCLUSION;
        feature.lobeCaps = RT_PATH_TRACE_MATERIAL_LOBE_EMISSIVE;
        feature.modifierKind = RT_PATH_TRACE_MATERIAL_MODIFIER_NONE;
        feature.passSupport |=
            RT_PATH_TRACE_MATERIAL_PASS_PRIMARY_SURFACE |
            RT_PATH_TRACE_MATERIAL_PASS_PATH_INTEGRATOR;
        return feature;
    }
    else if (feature.modifierKind == RT_PATH_TRACE_MATERIAL_MODIFIER_LIQUID_POOL_UNION)
    {
        // Match the production feature-row precedence: a classified liquid
        // card is a receiver modifier even when its draw surface is translucent.
        feature.materialKind = RT_PATH_TRACE_MATERIAL_KIND_LIQUID_POOL_MODIFIER;
    }
    else if (translucent && translucentSubtype == RtSmokeTranslucentSubtype::GuiScreen)
    {
        feature.materialKind = RT_PATH_TRACE_MATERIAL_KIND_GUI_SCREEN;
    }
    else if (translucent && translucentSubtype == RtSmokeTranslucentSubtype::SmokeParticle)
    {
        feature.materialKind = RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_PARTICLE;
    }
    else if (glassLike)
    {
        feature.materialKind = RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_GLASS;
    }
    else if (translucent)
    {
        feature.materialKind = RT_PATH_TRACE_MATERIAL_KIND_UNKNOWN;
    }
    else if (feature.modifierKind != RT_PATH_TRACE_MATERIAL_MODIFIER_NONE)
    {
        feature.materialKind = RT_PATH_TRACE_MATERIAL_KIND_DECAL_MODIFIER;
    }
    else if (info.hasAlphaTest)
    {
        feature.materialKind = RT_PATH_TRACE_MATERIAL_KIND_ALPHA_TESTED;
    }
    else
    {
        feature.materialKind = RT_PATH_TRACE_MATERIAL_KIND_OPAQUE;
    }

    if (feature.materialKind == RT_PATH_TRACE_MATERIAL_KIND_TRANSLUCENT_GLASS)
    {
        feature.materialCaps = RT_PATH_TRACE_MATERIAL_CAP_PATH_TRANSMISSION;
        feature.lobeCaps = RT_PATH_TRACE_MATERIAL_LOBE_SPECULAR_TRANSMISSION;
        feature.passSupport |=
            RT_PATH_TRACE_MATERIAL_PASS_PATH_INTEGRATOR |
            RT_PATH_TRACE_MATERIAL_PASS_TRANSMISSION_PRODUCER;
        feature.transmissionCandidate = true;
        feature.transmissionActive =
            r_pathTracingCleanRtxdiDiTransmissionProducer.GetInteger() != 0 &&
            r_pathTracingCleanRtxdiDiTransmissionCompose.GetInteger() != 0;
        return feature;
    }

    if (feature.materialKind == RT_PATH_TRACE_MATERIAL_KIND_LIQUID_POOL_MODIFIER)
    {
        feature.materialCaps =
            RT_PATH_TRACE_MATERIAL_CAP_RECEIVER_MODIFIER |
            RT_PATH_TRACE_MATERIAL_CAP_IDEMPOTENT_MODIFIER_BLEND;
        feature.lobeCaps = 0u;
        feature.passSupport = RT_PATH_TRACE_MATERIAL_PASS_DEBUG_VISUALIZER;
        return feature;
    }

    if (!translucent)
    {
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
        if (info.hasAlphaTest)
        {
            feature.materialCaps |= RT_PATH_TRACE_MATERIAL_CAP_VISIBILITY_RAY_ALPHA_TEST;
        }
        if (info.emissive)
        {
            feature.lobeCaps |= RT_PATH_TRACE_MATERIAL_LOBE_EMISSIVE;
        }
        if (feature.modifierKind != RT_PATH_TRACE_MATERIAL_MODIFIER_NONE)
        {
            feature.materialCaps |= RT_PATH_TRACE_MATERIAL_CAP_RECEIVER_MODIFIER;
        }
    }

    return feature;
}

} // namespace

void ProcessSmokeCrosshairZeroRoughnessToggle(const viewDef_t* viewDef)
{
    if (!ConsumeSmokeCrosshairZeroRoughnessToggleRequest())
    {
        return;
    }

    idVec3 hitPoint = vec3_origin;
    int surfaceIndex = -1;
    int triangleIndex = -1;
    if (!FindCenterCameraRayAnchor(viewDef, hitPoint, surfaceIndex, triangleIndex))
    {
        common->Printf("PathTracePrimaryPass: crosshair zero-roughness toggle found no center-ray hit\n");
        return;
    }

    if (!viewDef || surfaceIndex < 0 || surfaceIndex >= viewDef->numDrawSurfs)
    {
        common->Printf("PathTracePrimaryPass: crosshair zero-roughness toggle invalid hit surface=%d triangle=%d\n", surfaceIndex, triangleIndex);
        return;
    }

    const drawSurf_t* drawSurf = viewDef->drawSurfs[surfaceIndex];
    const srfTriangles_t* tri = nullptr;
    if (!ValidateSmokeDrawSurface(viewDef, drawSurf, tri, nullptr) || !drawSurf || !drawSurf->material || !tri)
    {
        common->Printf("PathTracePrimaryPass: crosshair zero-roughness toggle failed validation surface=%d triangle=%d\n", surfaceIndex, triangleIndex);
        return;
    }

    const idMaterial* material = drawSurf->material;
    const uint32_t materialId = SmokeMaterialId(material);
    const bool enabled = ToggleSmokeMaterialZeroRoughnessOverride(materialId, material->GetName());
    common->Printf("PathTracePrimaryPass: crosshair zero-roughness toggle %s surface=%d triangle=%d point=(%.2f %.2f %.2f) material='%s' id=%u\n",
        enabled ? "enabled" : "disabled",
        surfaceIndex,
        triangleIndex,
        hitPoint.x,
        hitPoint.y,
        hitPoint.z,
        material->GetName(),
        materialId);
}

void ProcessSmokeCrosshairFullMetalToggle(const viewDef_t* viewDef)
{
    if (!ConsumeSmokeCrosshairFullMetalToggleRequest())
    {
        return;
    }

    idVec3 hitPoint = vec3_origin;
    int surfaceIndex = -1;
    int triangleIndex = -1;
    if (!FindCenterCameraRayAnchor(viewDef, hitPoint, surfaceIndex, triangleIndex))
    {
        common->Printf("PathTracePrimaryPass: crosshair full-metal toggle found no center-ray hit\n");
        return;
    }

    if (!viewDef || surfaceIndex < 0 || surfaceIndex >= viewDef->numDrawSurfs)
    {
        common->Printf("PathTracePrimaryPass: crosshair full-metal toggle invalid hit surface=%d triangle=%d\n", surfaceIndex, triangleIndex);
        return;
    }

    const drawSurf_t* drawSurf = viewDef->drawSurfs[surfaceIndex];
    const srfTriangles_t* tri = nullptr;
    if (!ValidateSmokeDrawSurface(viewDef, drawSurf, tri, nullptr) || !drawSurf || !drawSurf->material || !tri)
    {
        common->Printf("PathTracePrimaryPass: crosshair full-metal toggle failed validation surface=%d triangle=%d\n", surfaceIndex, triangleIndex);
        return;
    }

    const idMaterial* material = drawSurf->material;
    const uint32_t materialId = SmokeMaterialId(material);
    const bool enabled = ToggleSmokeMaterialFullMetalOverride(materialId, material->GetName());
    common->Printf("PathTracePrimaryPass: crosshair full-metal toggle %s surface=%d triangle=%d point=(%.2f %.2f %.2f) material='%s' id=%u\n",
        enabled ? "enabled" : "disabled",
        surfaceIndex,
        triangleIndex,
        hitPoint.x,
        hitPoint.y,
        hitPoint.z,
        material->GetName(),
        materialId);
}


void LogSmokeCrosshairMaterialDump(
    const viewDef_t* viewDef,
    const RtSmokeMaterialTableBuild& table,
    const std::vector<PathTraceDynamicMaterialRecord>* dynamicRecords,
    const std::vector<uint32_t>* dynamicTriangleMaterialIds,
    const std::vector<uint32_t>* dynamicTriangleMaterialIndexes,
    const std::vector<uint32_t>* staticTriangleMaterialIds,
    const std::vector<uint32_t>* staticTriangleMaterialIndexes,
    const RtPathTraceRigidRouteBuild* rigidRouteBuild)
{
    idVec3 hitPoint = vec3_origin;
    int surfaceIndex = -1;
    int triangleIndex = -1;
    if (!FindCenterCameraRayAnchor(viewDef, hitPoint, surfaceIndex, triangleIndex))
    {
        common->Printf("PathTracePrimaryPass: RT smoke crosshair material dump found no center-ray hit\n");
        return;
    }

    if (!viewDef || surfaceIndex < 0 || surfaceIndex >= viewDef->numDrawSurfs)
    {
        common->Printf("PathTracePrimaryPass: RT smoke crosshair material dump invalid hit surface=%d triangle=%d\n", surfaceIndex, triangleIndex);
        return;
    }

    const drawSurf_t* drawSurf = viewDef->drawSurfs[surfaceIndex];
    const srfTriangles_t* tri = nullptr;
    if (!ValidateSmokeDrawSurface(viewDef, drawSurf, tri, nullptr) || !drawSurf || !drawSurf->material || !tri)
    {
        common->Printf("PathTracePrimaryPass: RT smoke crosshair material dump failed validation surface=%d triangle=%d\n", surfaceIndex, triangleIndex);
        return;
    }

    const idMaterial* material = drawSurf->material;
    const RtSmokeSurfaceClass surfaceClass = ClassifySmokeSurface(viewDef, drawSurf, tri);
    const RtSmokeTranslucentSubtype translucentSubtype = surfaceClass == RtSmokeSurfaceClass::ParticleAlpha ? ClassifySmokeTranslucentSubtype(drawSurf) : RtSmokeTranslucentSubtype::Unknown;
    const RtSmokeTranslucentClassifierInfo classifier = BuildSmokeTranslucentClassifierInfo(material);
    const uint32_t baseMaterialId = SmokeMaterialId(material);
    const uint32_t materialId = SmokeRuntimeMaterialTableIdForDrawSurf(drawSurf, baseMaterialId);
    const RtSmokeMaterialTextureInfo info = ResolveSmokeMaterialTextureInfo(materialId, -1);

    int tableIndex = -1;
    for (int index = 0; index < static_cast<int>(table.materialIds.size()); ++index)
    {
        if (table.materialIds[index] == materialId)
        {
            tableIndex = index;
            break;
        }
    }

    common->Printf("PathTracePrimaryPass: RT smoke crosshair material hit surface=%d triangle=%d point=(%.2f %.2f %.2f) material='%s' baseId=%u runtimeId=%u variant=%d tableIndex=%d class=%s subtype=%s coverage=%s sort=%.2f deform=%s cull=%d stages=%d guiSurface=%d\n",
        surfaceIndex,
        triangleIndex,
        hitPoint.x,
        hitPoint.y,
        hitPoint.z,
        material->GetName(),
        baseMaterialId,
        materialId,
        materialId != baseMaterialId ? 1 : 0,
        tableIndex,
        SmokeSurfaceClassName(surfaceClass),
        SmokeTranslucentSubtypeName(translucentSubtype),
        SmokeCoverageName(material->Coverage()),
        material->GetSort(),
        SmokeDeformName(material->Deform()),
        static_cast<int>(material->GetCullType()),
        material->GetNumStages(),
        IsSmokeGuiDrawSurface(drawSurf) ? 1 : 0);

    common->Printf("PathTracePrimaryPass: RT smoke crosshair classifiers guiSort=%d decalSort=%d postSort=%d polyOffset=%d screenTex=%d addDefault0200=%d addBlend=%d ambient=%d ambientBlend=%d diffuse=%d nameGui=%d nameParticle=%d nameDecal=%d nameGlass=%d nameGlow=%d nameSignage=%d opaqueSwinglightCompat=%d\n",
        classifier.sortIsGuiOrSubview ? 1 : 0,
        classifier.sortIsDecal ? 1 : 0,
        classifier.sortIsPostProcess ? 1 : 0,
        classifier.polygonOffsetDecal ? 1 : 0,
        classifier.hasScreenTexgen ? 1 : 0,
        classifier.hasAddDefault0200Texture ? 1 : 0,
        classifier.hasAdditiveBlend ? 1 : 0,
        classifier.hasAmbientStage ? 1 : 0,
        classifier.hasAmbientBlendStage ? 1 : 0,
        classifier.hasDiffuseStage ? 1 : 0,
        classifier.nameLooksGui ? 1 : 0,
        classifier.nameLooksParticle ? 1 : 0,
        classifier.nameLooksDecal ? 1 : 0,
        classifier.nameLooksGlass ? 1 : 0,
        classifier.nameLooksGlow ? 1 : 0,
        classifier.nameLooksSignage ? 1 : 0,
        SmokeMaterialUsesOpaqueSwinglightCompatibility(material) ? 1 : 0);

    common->Printf("PathTracePrimaryPass: RT smoke crosshair detail-decal isDetailDecal=%d isDynamic=%d blendKind=%s spectrum=%d compositeStage=%d\n",
        info.detailDecal ? 1 : 0,
        info.detailDecalDynamic ? 1 : 0,
        info.detailDecalLiquidPool ? "liquid-pool" : (info.detailDecalDiffuseLit ? "diffuse-lit" : (info.filterDecal ? "modulate" : (info.additiveDecal ? "additive" : "over"))),
        info.detailDecalSpectrum,
        r_pathTracingDecalComposite.GetInteger());

    common->Printf("PathTracePrimaryPass: RT smoke crosshair liquid-film candidate=%d detail=%d blood=%d reflect2=%d coverage=%d coverageImage='%s' wetNormal=%d normalImage='%s' exactOverride=%d overrideReason='%s' legacyPool=%d variant=%d dynamic=%d reason='%s' mode=%d debug=%d page=%d\n",
        info.liquidFilmCandidate ? 1 : 0,
        info.detailDecal ? 1 : 0,
        info.liquidFilmHasBloodSemantic ? 1 : 0,
        info.liquidFilmHasWetReflectStage ? 1 : 0,
        info.liquidFilmHasCoverageSource ? 1 : 0,
        info.liquidFilmCoverageImageName.c_str(),
        info.liquidFilmHasWetNormalSource ? 1 : 0,
        info.normalImageName.c_str(),
        info.liquidFilmExactOverride ? 1 : 0,
        info.liquidFilmOverrideReason.c_str(),
        info.detailDecalLiquidPool ? 1 : 0,
        IsSmokeMaterialTextureVariant(materialId) ? 1 : 0,
        info.isDynamic ? 1 : 0,
        info.liquidFilmReason.c_str(),
        r_pathTracingLiquidPoolMode.GetInteger(),
        r_pathTracingLiquidPoolDebug.GetInteger(),
        r_pathTracingLiquidPoolDebugPage.GetInteger());

    const RtCrosshairMaterialFeatureDebug feature = BuildCrosshairMaterialFeatureDebug(surfaceClass, translucentSubtype, info);
    const bool directReservoirSupported =
        (feature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_OPAQUE_DIRECT) != 0u &&
        (feature.passSupport & RT_PATH_TRACE_MATERIAL_PASS_DIRECT_RESERVOIR) != 0u &&
        (feature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_DEBUG_FAIL_CLOSED) == 0u;
    const bool giReservoirSupported =
        (feature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_OPAQUE_GI) != 0u &&
        (feature.passSupport & RT_PATH_TRACE_MATERIAL_PASS_GI_RESERVOIR) != 0u &&
        (feature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_DEBUG_FAIL_CLOSED) == 0u;
    const int transmissionBounceRequest =
        (r_pathTracingCleanRtxdiDiTransmissionProducer.GetInteger() != 0 &&
         r_pathTracingCleanRtxdiDiTransmissionCompose.GetInteger() != 0) ? 1 : 0;
    const int transmissionBounceEffective = transmissionBounceRequest;
    common->Printf("PathTracePrimaryPass: RT smoke crosshair modular material kind=%s(%u) caps=0x%08x lobes=0x%08x passSupport=0x%08x modifier=%s(%u) transmissionCandidate=%d transmissionActive=%d transmissionBounceRequest=%d transmissionBounceEffective=%d directReservoir=%s giReservoir=%s unsupportedDebug=%s\n",
        RtPathTraceMaterialKindName(feature.materialKind),
        static_cast<uint32_t>(feature.materialKind),
        feature.materialCaps,
        feature.lobeCaps,
        feature.passSupport,
        RtPathTraceMaterialModifierName(feature.modifierKind),
        static_cast<uint32_t>(feature.modifierKind),
        feature.transmissionCandidate ? 1 : 0,
        feature.transmissionActive ? 1 : 0,
        transmissionBounceRequest,
        transmissionBounceEffective,
        directReservoirSupported ? "supported" : "unsupported",
        giReservoirSupported ? "supported" : "unsupported",
        RtPathTraceMaterialUnsupportedDebugName(feature));

    if (tableIndex >= 0 && tableIndex < static_cast<int>(table.materialFeatures.size()))
    {
        const RtPathTraceMaterialFeatureRecord& featureRow = table.materialFeatures[tableIndex];
        const bool parameterIndexMatches =
            featureRow.parameterRecordIndex == static_cast<uint32_t>(tableIndex) &&
            featureRow.parameterRecordIndex < table.materialFeatureParameters.size();
        common->Printf("PathTracePrimaryPass: RT smoke crosshair feature-row materialIndex=%d parameterRecordIndex=%u indexMatch=%d recordAbi=%u expectedAbi=%u kind=%s(%u) caps=0x%08x lobes=0x%08x passSupport=0x%08x modifier=%s(%u) rowCounts material/feature/params=%d/%d/%d\n",
            tableIndex,
            featureRow.parameterRecordIndex,
            parameterIndexMatches ? 1 : 0,
            featureRow.recordAbiVersion,
            RT_PATH_TRACE_MATERIAL_FEATURE_RECORD_ABI_VERSION,
            RtPathTraceMaterialKindName(static_cast<RtPathTraceMaterialKind>(featureRow.materialKind)),
            featureRow.materialKind,
            featureRow.materialCaps,
            featureRow.lobeCaps,
            featureRow.passSupport,
            RtPathTraceMaterialModifierName(static_cast<RtPathTraceMaterialModifierKind>(featureRow.modifierKind)),
            featureRow.modifierKind,
            static_cast<int>(table.materials.size()),
            static_cast<int>(table.materialFeatures.size()),
            static_cast<int>(table.materialFeatureParameters.size()));
    }

    common->Printf("PathTracePrimaryPass: RT smoke crosshair RT metadata diffuse='%s' usage=%s color=%s image=%d handle=%d safe=%d reason='%s' alpha='%s' usage=%s color=%s image=%d handle=%d safe=%d reason='%s' hasAlphaTest=%d cutoff=%.3f alphaFromLuma=%d alphaDarkKey=%d alphaMagentaKey=%d normal='%s' usage=%s color=%s safe=%d specular='%s' usage=%s color=%s safe=%d emissive='%s' usage=%s color=%s safe=%d emissive=%d lightCandidate=%d additiveDecal=%d additiveWhiteKey=%d filterDecal=%d blackKey=%d forceAlbedo=%d portalFallback=%d objectGlassFallback=%d fallbackAlbedo=%d(%.2f %.2f %.2f)\n",
        info.diffuseImageName.c_str(),
        SmokeTextureUsageName(info.diffuseUsage),
        SmokeTextureColorFormatName(info.diffuseColorFormat),
        info.hasDiffuseImage ? 1 : 0,
        info.hasTextureHandle ? 1 : 0,
        info.hasSafeTexture ? 1 : 0,
        info.fallbackReason.c_str(),
        info.alphaImageName.c_str(),
        SmokeTextureUsageName(info.alphaUsage),
        SmokeTextureColorFormatName(info.alphaColorFormat),
        info.hasAlphaImage ? 1 : 0,
        info.hasAlphaTextureHandle ? 1 : 0,
        info.hasSafeAlphaTexture ? 1 : 0,
        info.alphaReason.c_str(),
        info.hasAlphaTest ? 1 : 0,
        info.alphaCutoff,
        info.alphaFromDiffuseLuma ? 1 : 0,
        info.alphaFromDiffuseDarkKey ? 1 : 0,
        info.alphaFromDiffuseMagentaKey ? 1 : 0,
        info.normalImageName.c_str(),
        SmokeTextureUsageName(info.normalUsage),
        SmokeTextureColorFormatName(info.normalColorFormat),
        info.hasSafeNormalTexture ? 1 : 0,
        info.specularImageName.c_str(),
        SmokeTextureUsageName(info.specularUsage),
        SmokeTextureColorFormatName(info.specularColorFormat),
        info.hasSafeSpecularTexture ? 1 : 0,
        info.emissiveImageName.c_str(),
        SmokeTextureUsageName(info.emissiveUsage),
        SmokeTextureColorFormatName(info.emissiveColorFormat),
        info.hasSafeEmissiveTexture ? 1 : 0,
        info.emissive ? 1 : 0,
        info.emissiveLightCandidate ? 1 : 0,
        info.additiveDecal ? 1 : 0,
        info.additiveDecalWhiteKey ? 1 : 0,
        info.filterDecal ? 1 : 0,
        info.filterDecalBlackKey ? 1 : 0,
        info.forceFallbackAlbedo ? 1 : 0,
        info.portalWindowFallback ? 1 : 0,
        info.objectGlassFallback ? 1 : 0,
        info.hasFallbackAlbedo ? 1 : 0,
        info.fallbackAlbedo.x,
        info.fallbackAlbedo.y,
        info.fallbackAlbedo.z);

    if (info.skyEnvironment || info.hasSkyTextureHandle)
    {
        common->Printf("PathTracePrimaryPass: RT smoke crosshair sky environment=%d image='%s' handle=%d safeCube=%d color=(%.3f %.3f %.3f %.3f) terminalSurface=1 missEnvironment=0\n",
            info.skyEnvironment ? 1 : 0,
            info.skyImageName.c_str(),
            info.hasSkyTextureHandle ? 1 : 0,
            info.hasSafeSkyTexture ? 1 : 0,
            info.skyColor.x,
            info.skyColor.y,
            info.skyColor.z,
            info.skyColor.w);
    }

    if (tableIndex >= 0 && tableIndex < static_cast<int>(table.materials.size()))
    {
        const PathTraceSmokeMaterial& rtMaterial = table.materials[tableIndex];
        common->Printf("PathTracePrimaryPass: RT smoke crosshair RT material debugAlbedo=(%.2f %.2f %.2f %.2f) flags=0x%08x overrideZeroRoughness=%d overrideFullMetal=%d materialPadding0=0x%08x diffuseSlot=%d alphaSlot=%d normalSlot=%d specSlot=%d emissiveSlot=%d alphaCutoff=%.3f\n",
            rtMaterial.debugAlbedo[0],
            rtMaterial.debugAlbedo[1],
            rtMaterial.debugAlbedo[2],
            rtMaterial.debugAlbedo[3],
            rtMaterial.flags,
            SmokeMaterialHasZeroRoughnessOverride(materialId) ? 1 : 0,
            SmokeMaterialHasFullMetalOverride(materialId) ? 1 : 0,
            rtMaterial.padding0,
            rtMaterial.diffuseTextureIndex == UINT32_MAX ? -1 : static_cast<int>(rtMaterial.diffuseTextureIndex),
            rtMaterial.alphaTextureIndex == UINT32_MAX ? -1 : static_cast<int>(rtMaterial.alphaTextureIndex),
            rtMaterial.normalTextureIndex == UINT32_MAX ? -1 : static_cast<int>(rtMaterial.normalTextureIndex),
            rtMaterial.specularTextureIndex == UINT32_MAX ? -1 : static_cast<int>(rtMaterial.specularTextureIndex),
            rtMaterial.emissiveTextureIndex == UINT32_MAX ? -1 : static_cast<int>(rtMaterial.emissiveTextureIndex),
            rtMaterial.alphaCutoff);
        if (tableIndex < static_cast<int>(table.materialFeatureParameters.size()))
        {
            const RtPathTraceMaterialFeatureParameterRecord& parameters = table.materialFeatureParameters[tableIndex];
            common->Printf("PathTracePrimaryPass: RT smoke crosshair liquid-film-params active=%d paramAbi=%u params0 referenceTransmittance=(%.6f %.6f %.6f) opticalDepthScale=%.6f params1 coatRoughness=%.6f dielectricIor=%.6f authoredNormalStrength=%.6f reservedZero=%.6f valid=%d source='%s' override=0 reason='frozen-v1-default'\n",
                info.liquidFilmCandidate ? 1 : 0,
                RT_PATH_TRACE_LIQUID_POOL_PARAMETER_ABI_VERSION,
                parameters.params0[RT_PATH_TRACE_LIQUID_POOL_PARAM0_REFERENCE_TRANSMITTANCE_R],
                parameters.params0[RT_PATH_TRACE_LIQUID_POOL_PARAM0_REFERENCE_TRANSMITTANCE_G],
                parameters.params0[RT_PATH_TRACE_LIQUID_POOL_PARAM0_REFERENCE_TRANSMITTANCE_B],
                parameters.params0[RT_PATH_TRACE_LIQUID_POOL_PARAM0_OPTICAL_DEPTH_SCALE],
                parameters.params1[RT_PATH_TRACE_LIQUID_POOL_PARAM1_COAT_ROUGHNESS],
                parameters.params1[RT_PATH_TRACE_LIQUID_POOL_PARAM1_DIELECTRIC_IOR],
                parameters.params1[RT_PATH_TRACE_LIQUID_POOL_PARAM1_AUTHORED_NORMAL_STRENGTH],
                parameters.params1[RT_PATH_TRACE_LIQUID_POOL_PARAM1_RESERVED_ZERO],
                info.liquidFilmCandidate && PathTraceLiquidPoolMaterialFeatureParametersAreValid(parameters) ? 1 : 0,
                info.liquidFilmCandidate ? "candidate-row" : "not-liquid");
            common->Printf("PathTracePrimaryPass: RT smoke crosshair orderedStages words=%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x overflow=%d\n",
                parameters.orderedStageWords[0],
                parameters.orderedStageWords[1],
                parameters.orderedStageWords[2],
                parameters.orderedStageWords[3],
                parameters.orderedStageWords[4],
                parameters.orderedStageWords[5],
                parameters.orderedStageWords[6],
                parameters.orderedStageWords[7],
                (parameters.orderedStageWords[7] & (1u << 31u)) != 0u ? 1 : 0);
            common->Printf("PathTracePrimaryPass: RT smoke crosshair orderedStageTextures words=%08x/%08x/%08x/%08x/%08x/%08x/%08x/%08x\n",
                parameters.orderedStageTextureWords[0],
                parameters.orderedStageTextureWords[1],
                parameters.orderedStageTextureWords[2],
                parameters.orderedStageTextureWords[3],
                parameters.orderedStageTextureWords[4],
                parameters.orderedStageTextureWords[5],
                parameters.orderedStageTextureWords[6],
                parameters.orderedStageTextureWords[7]);
        }
    }

    if (tableIndex >= 0)
    {
        int idMatches = 0;
        int indexMatches = 0;
        if (dynamicTriangleMaterialIds)
        {
            idMatches = static_cast<int>(std::count(dynamicTriangleMaterialIds->begin(), dynamicTriangleMaterialIds->end(), materialId));
        }
        if (dynamicTriangleMaterialIndexes)
        {
            indexMatches = static_cast<int>(std::count(dynamicTriangleMaterialIndexes->begin(), dynamicTriangleMaterialIndexes->end(), static_cast<uint32_t>(tableIndex)));
        }

        if (dynamicRecords && tableIndex < static_cast<int>(dynamicRecords->size()))
        {
            const PathTraceDynamicMaterialRecord& record = (*dynamicRecords)[tableIndex];
            const bool hasOrderedStages = (record.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_HAS_ORDERED_STAGE_VALUES) != 0u;
            const uint32_t selectedStage = hasOrderedStages
                ? record.stageIndex & RT_SMOKE_DYNAMIC_MATERIAL_HEADER_SELECTED_STAGE_MASK
                : record.stageIndex;
            const uint32_t orderedStageCount = hasOrderedStages
                ? (record.stageIndex & RT_SMOKE_DYNAMIC_MATERIAL_HEADER_STAGE_COUNT_MASK) >> RT_SMOKE_DYNAMIC_MATERIAL_HEADER_STAGE_COUNT_SHIFT
                : 0u;
            const uint32_t orderedStageOffset = hasOrderedStages
                ? (record.stageIndex & RT_SMOKE_DYNAMIC_MATERIAL_HEADER_STAGE_OFFSET_MASK) >> RT_SMOKE_DYNAMIC_MATERIAL_HEADER_STAGE_OFFSET_SHIFT
                : 0u;
            const bool orderedStageOverflow = hasOrderedStages &&
                (record.stageIndex & RT_SMOKE_DYNAMIC_MATERIAL_HEADER_STAGE_OVERFLOW) != 0u;
            common->Printf("PathTracePrimaryPass: RT smoke crosshair dynamicRecord count=%d tableIndex=%d recordMaterialId=%u recordMaterialIndex=%u stage=%u flags=0x%08x orderedOffset/count/overflow=%u/%u/%d matrix=(%.4f %.4f %.4f;%.4f %.4f %.4f) condition=%.4f alpha=%.4f triangleMatches id/index=%d/%d\n",
                static_cast<int>(dynamicRecords->size()),
                tableIndex,
                record.materialId,
                record.materialIndex,
                selectedStage,
                record.flags,
                orderedStageOffset,
                orderedStageCount,
                orderedStageOverflow ? 1 : 0,
                record.texMatrix0[0], record.texMatrix0[1], record.texMatrix0[2],
                record.texMatrix1[0], record.texMatrix1[1], record.texMatrix1[2],
                record.texMatrix0[3], record.texMatrix1[3],
                idMatches,
                indexMatches);
            for (uint32_t orderedIndex = 0; orderedIndex < orderedStageCount; ++orderedIndex)
            {
                const uint32_t recordIndex = orderedStageOffset + orderedIndex;
                if (recordIndex >= dynamicRecords->size())
                {
                    common->Printf("PathTracePrimaryPass: RT smoke crosshair dynamicStage[%u] invalidRecordIndex=%u recordCount=%d\n",
                        orderedIndex,
                        recordIndex,
                        static_cast<int>(dynamicRecords->size()));
                    break;
                }
                const PathTraceDynamicMaterialRecord& stage = (*dynamicRecords)[recordIndex];
                common->Printf("PathTracePrimaryPass: RT smoke crosshair dynamicStage[%u] recordIndex=%u stage=%u flags=0x%08x color=(%.4f %.4f %.4f %.4f) matrix=(%.4f %.4f %.4f;%.4f %.4f %.4f) condition=%.4f alpha=%.4f\n",
                    orderedIndex,
                    recordIndex,
                    stage.stageIndex,
                    stage.flags,
                    stage.color[0], stage.color[1], stage.color[2], stage.color[3],
                    stage.texMatrix0[0], stage.texMatrix0[1], stage.texMatrix0[2],
                    stage.texMatrix1[0], stage.texMatrix1[1], stage.texMatrix1[2],
                    stage.texMatrix0[3], stage.texMatrix1[3]);
            }
        }
        else
        {
            common->Printf("PathTracePrimaryPass: RT smoke crosshair dynamicRecord missing tableIndex=%d recordCount=%d triangleMatches id/index=%d/%d\n",
                tableIndex,
                dynamicRecords ? static_cast<int>(dynamicRecords->size()) : 0,
                idMatches,
                indexMatches);
        }
    }

    if (rigidRouteBuild)
    {
        int runtimeInstanceMatches = 0;
        int baseInstanceMatches = 0;
        int runtimeIndexMatches = 0;
        int baseIndexMatches = 0;
        const PathTraceRigidRouteInstance* sampleInstance = nullptr;
        for (const PathTraceRigidRouteInstance& instance : rigidRouteBuild->instances)
        {
            if (instance.materialId == materialId)
            {
                ++runtimeInstanceMatches;
                if (!sampleInstance)
                {
                    sampleInstance = &instance;
                }
            }
            if (instance.materialId == baseMaterialId)
            {
                ++baseInstanceMatches;
                if (!sampleInstance)
                {
                    sampleInstance = &instance;
                }
            }
            if (tableIndex >= 0 && instance.materialIndex == static_cast<uint32_t>(tableIndex))
            {
                ++runtimeIndexMatches;
            }
            if (instance.materialIndex < table.materialIds.size() && table.materialIds[instance.materialIndex] == baseMaterialId)
            {
                ++baseIndexMatches;
            }
        }

        common->Printf(
            "PathTracePrimaryPass: RT smoke crosshair rigidRoute instances=%d runtime/baseIdMatches=%d/%d runtime/baseIndexMatches=%d/%d\n",
            static_cast<int>(rigidRouteBuild->instances.size()),
            runtimeInstanceMatches,
            baseInstanceMatches,
            runtimeIndexMatches,
            baseIndexMatches);
        if (sampleInstance)
        {
            uint32_t triangleMaterialId = UINT32_MAX;
            uint32_t triangleMaterialIndex = UINT32_MAX;
            if (sampleInstance->triangleOffset < rigidRouteBuild->triangleMaterials.size())
            {
                triangleMaterialId = rigidRouteBuild->triangleMaterials[sampleInstance->triangleOffset];
            }
            if (sampleInstance->triangleOffset < rigidRouteBuild->triangleMaterialIndexes.size())
            {
                triangleMaterialIndex = rigidRouteBuild->triangleMaterialIndexes[sampleInstance->triangleOffset];
            }
            idVec2 baseUv(0.0f, 0.0f);
            if (sampleInstance->vertexOffset < rigidRouteBuild->vertices.size())
            {
                const PathTraceSmokeVertex& vertex = rigidRouteBuild->vertices[sampleInstance->vertexOffset];
                baseUv.Set(vertex.texCoord[0], vertex.texCoord[1]);
            }
            idVec2 resolvedUv = baseUv;
            if (dynamicRecords && sampleInstance->materialIndex < dynamicRecords->size())
            {
                const PathTraceDynamicMaterialRecord& record = (*dynamicRecords)[sampleInstance->materialIndex];
                if ((record.flags & RT_SMOKE_DYNAMIC_MATERIAL_RECORD_HAS_TEX_MATRIX) != 0u)
                {
                    resolvedUv.Set(
                        record.texMatrix0[0] * baseUv.x + record.texMatrix0[1] * baseUv.y + record.texMatrix0[2],
                        record.texMatrix1[0] * baseUv.x + record.texMatrix1[1] * baseUv.y + record.texMatrix1[2]);
                }
            }
            common->Printf(
                "PathTracePrimaryPass: RT smoke crosshair rigidRoute sample materialId/index=%u/%u triangleMaterialId/index=%u/%u offsets(v/i/t)=%u/%u/%u baseUv=(%.4f %.4f) resolvedUv=(%.4f %.4f)\n",
                sampleInstance->materialId,
                sampleInstance->materialIndex,
                triangleMaterialId,
                triangleMaterialIndex,
                sampleInstance->vertexOffset,
                sampleInstance->indexOffset,
                sampleInstance->triangleOffset,
                baseUv.x,
                baseUv.y,
                resolvedUv.x,
                resolvedUv.y);
        }
    }

    const int staticRuntimeIdMatches = staticTriangleMaterialIds
        ? static_cast<int>(std::count(staticTriangleMaterialIds->begin(), staticTriangleMaterialIds->end(), materialId))
        : 0;
    const int staticBaseIdMatches = staticTriangleMaterialIds
        ? static_cast<int>(std::count(staticTriangleMaterialIds->begin(), staticTriangleMaterialIds->end(), baseMaterialId))
        : 0;
    const int staticRuntimeIndexMatches = staticTriangleMaterialIndexes && tableIndex >= 0
        ? static_cast<int>(std::count(staticTriangleMaterialIndexes->begin(), staticTriangleMaterialIndexes->end(), static_cast<uint32_t>(tableIndex)))
        : 0;
    int staticBaseIndexMatches = 0;
    if (staticTriangleMaterialIndexes)
    {
        for (uint32_t materialIndex : *staticTriangleMaterialIndexes)
        {
            if (materialIndex < table.materialIds.size() && table.materialIds[materialIndex] == baseMaterialId)
            {
                ++staticBaseIndexMatches;
            }
        }
    }
    common->Printf(
        "PathTracePrimaryPass: RT smoke crosshair staticRoute triangles=%d runtime/baseIdMatches=%d/%d runtime/baseIndexMatches=%d/%d\n",
        staticTriangleMaterialIds ? static_cast<int>(staticTriangleMaterialIds->size()) : 0,
        staticRuntimeIdMatches,
        staticBaseIdMatches,
        staticRuntimeIndexMatches,
        staticBaseIndexMatches);

    const int indexBase = triangleIndex * 3;
    if (indexBase >= 0 && indexBase + 2 < tri->numIndexes)
    {
        const int i0 = tri->indexes[indexBase + 0];
        const int i1 = tri->indexes[indexBase + 1];
        const int i2 = tri->indexes[indexBase + 2];
        if (i0 >= 0 && i1 >= 0 && i2 >= 0 && i0 < tri->numVerts && i1 < tri->numVerts && i2 < tri->numVerts)
        {
            common->Printf("PathTracePrimaryPass: RT smoke crosshair triangle indexes=%d/%d/%d vertexColors=(%u %u %u %u),(%u %u %u %u),(%u %u %u %u)\n",
                i0, i1, i2,
                tri->verts[i0].color[0], tri->verts[i0].color[1], tri->verts[i0].color[2], tri->verts[i0].color[3],
                tri->verts[i1].color[0], tri->verts[i1].color[1], tri->verts[i1].color[2], tri->verts[i1].color[3],
                tri->verts[i2].color[0], tri->verts[i2].color[1], tri->verts[i2].color[2], tri->verts[i2].color[3]);
        }
    }

    const float* regs = drawSurf->shaderRegisters ? drawSurf->shaderRegisters : material->ConstantRegisters();
    const int registerCount = material->GetNumRegisters();
    const RtMaterialRecord* materialClassRecord = FindPathTraceMaterialRecord(materialId);
    const RtCrosshairOrderedCompositingProjection compositingProjection =
        BuildCrosshairOrderedCompositingProjection(materialClassRecord);
    const uint32_t compatibilityFlags =
        tableIndex >= 0 && tableIndex < static_cast<int>(table.materials.size())
        ? table.materials[tableIndex].flags
        : 0u;
    common->Printf(
        "PathTracePrimaryPass: RT smoke crosshair declarationProjection alpha/add/multiply/invert/over/noop/unknown=%d/%d/%d/%d/%d/%d/%d overflow=%d needsStageTextureConsumer=%d compatibilityRoutes alpha/add/filter/blackKey=%d/%d/%d/%d\n",
        compositingProjection.alphaClipStage,
        compositingProjection.additiveStage,
        compositingProjection.multiplyStage,
        compositingProjection.invertedStage,
        compositingProjection.alphaOverStage,
        compositingProjection.destinationPreserveStage,
        compositingProjection.unknownStage,
        compositingProjection.overflow ? 1 : 0,
        compositingProjection.needsStageTextureConsumer ? 1 : 0,
        (compatibilityFlags & RT_SMOKE_MATERIAL_ALPHA_TEST) != 0u ? 1 : 0,
        (compatibilityFlags & RT_SMOKE_MATERIAL_ADDITIVE_DECAL) != 0u ? 1 : 0,
        (compatibilityFlags & RT_SMOKE_MATERIAL_FILTER_DECAL) != 0u ? 1 : 0,
        (compatibilityFlags & RT_SMOKE_MATERIAL_FILTER_DECAL_BLACK_KEY) != 0u ? 1 : 0);
    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage)
        {
            continue;
        }

        idVec4 stageColor(1.0f, 1.0f, 1.0f, 1.0f);
        if (regs)
        {
            for (int component = 0; component < 4; ++component)
            {
                const int colorRegister = stage->color.registers[component];
                if (colorRegister >= 0 && colorRegister < registerCount)
                {
                    stageColor[component] = regs[colorRegister];
                }
            }
        }

        const float condition = regs && stage->conditionRegister >= 0 && stage->conditionRegister < registerCount ? regs[stage->conditionRegister] : 1.0f;
        const float alphaTest = regs && stage->alphaTestRegister >= 0 && stage->alphaTestRegister < registerCount ? regs[stage->alphaTestRegister] : -1.0f;
        float texMatrix[2][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
        if (stage->texture.hasMatrix && regs)
        {
            for (int row = 0; row < 2; ++row)
            {
                for (int column = 0; column < 3; ++column)
                {
                    const int matrixRegister = stage->texture.matrix[row][column];
                    if (matrixRegister >= 0 && matrixRegister < registerCount)
                    {
                        texMatrix[row][column] = regs[matrixRegister];
                    }
                }
            }
        }
        idImage* image = stage->texture.image;
        const bool imageSafe = image && IsSmokeDiffuseImageSafeForRayTracing(image);
        const textureUsage_t imageUsage = image ? image->GetUsage() : TD_DEFAULT;
        const textureColor_t imageColorFormat = image ? image->GetOpts().colorFormat : CFM_DEFAULT;
        const RtMaterialCompositingStageFact* compositingFact = nullptr;
        if (materialClassRecord)
        {
            for (const RtMaterialCompositingStageFact& candidate : materialClassRecord->compositingStages)
            {
                if (candidate.stageIndex == stageIndex)
                {
                    compositingFact = &candidate;
                    break;
                }
            }
        }
        common->Printf("PathTracePrimaryPass: RT smoke crosshair stage[%d] lighting=%s condition=%.3f color=(%.3f %.3f %.3f %.3f) compositing=%s drawState=0x%llx srcBlendBits=0x%llx dstBlendBits=0x%llx alphaTest=%d alphaReg=%d alphaValue=%.3f ignoreAlpha=%d alphaSemantic=%s texgen=%s dynamic=%d cinematic=%d image='%s' usage=%s color=%s safe=%d\n",
            stageIndex,
            SmokeStageLightingName(stage->lighting),
            condition,
            stageColor.x,
            stageColor.y,
            stageColor.z,
            stageColor.w,
            compositingFact ? RtMaterialCompositingOpName(compositingFact->operation) : "uncompiled",
            static_cast<unsigned long long>(stage->drawStateBits),
            static_cast<unsigned long long>(stage->drawStateBits & GLS_SRCBLEND_BITS),
            static_cast<unsigned long long>(stage->drawStateBits & GLS_DSTBLEND_BITS),
            stage->hasAlphaTest ? 1 : 0,
            stage->alphaTestRegister,
            alphaTest,
            stage->ignoreAlphaTest ? 1 : 0,
            SmokeStageAlphaSemanticName(stage),
            SmokeTexgenName(stage->texture.texgen),
            static_cast<int>(stage->texture.dynamic),
            stage->texture.cinematic ? 1 : 0,
            image ? image->GetName() : "<none>",
            SmokeTextureUsageName(imageUsage),
            SmokeTextureColorFormatName(imageColorFormat),
            imageSafe ? 1 : 0);

        if (stage->texture.hasMatrix)
        {
            common->Printf("PathTracePrimaryPass: RT smoke crosshair stage[%d] texMatrix regs=(%d %d %d;%d %d %d) values=(%.4f %.4f %.4f;%.4f %.4f %.4f)\n",
                stageIndex,
                stage->texture.matrix[0][0], stage->texture.matrix[0][1], stage->texture.matrix[0][2],
                stage->texture.matrix[1][0], stage->texture.matrix[1][1], stage->texture.matrix[1][2],
                texMatrix[0][0], texMatrix[0][1], texMatrix[0][2],
                texMatrix[1][0], texMatrix[1][1], texMatrix[1][2]);

            if (stage->lighting == SL_BUMP && indexBase >= 0 && indexBase + 2 < tri->numIndexes)
            {
                const int matrixIndexes[3] = {
                    tri->indexes[indexBase + 0],
                    tri->indexes[indexBase + 1],
                    tri->indexes[indexBase + 2]
                };
                if (matrixIndexes[0] >= 0 && matrixIndexes[0] < tri->numVerts &&
                    matrixIndexes[1] >= 0 && matrixIndexes[1] < tri->numVerts &&
                    matrixIndexes[2] >= 0 && matrixIndexes[2] < tri->numVerts)
                {
                    idVec2 baseUv[3];
                    idVec2 normalUv[3];
                    for (int vertex = 0; vertex < 3; ++vertex)
                    {
                        baseUv[vertex] = tri->verts[matrixIndexes[vertex]].GetTexCoord();
                        normalUv[vertex].Set(
                            texMatrix[0][0] * baseUv[vertex].x + texMatrix[0][1] * baseUv[vertex].y + texMatrix[0][2],
                            texMatrix[1][0] * baseUv[vertex].x + texMatrix[1][1] * baseUv[vertex].y + texMatrix[1][2]);
                    }
                    common->Printf("PathTracePrimaryPass: RT smoke crosshair stage[%d] bumpUv base=(%.4f %.4f),(%.4f %.4f),(%.4f %.4f) transformed=(%.4f %.4f),(%.4f %.4f),(%.4f %.4f)\n",
                        stageIndex,
                        baseUv[0].x, baseUv[0].y, baseUv[1].x, baseUv[1].y, baseUv[2].x, baseUv[2].y,
                        normalUv[0].x, normalUv[0].y, normalUv[1].x, normalUv[1].y, normalUv[2].x, normalUv[2].y);
                }
            }
        }
    }
}

void LogSmokeGuiSurfaceDump(const viewDef_t* viewDef, const RtSmokeMaterialTableBuild& table)
{
    if (!viewDef)
    {
        common->Printf("PathTracePrimaryPass: RT smoke GUI dump no viewDef\n");
        return;
    }

    const int maxLogged = 24;
    int guiSurfaces = 0;
    int capturedGuiSurfaces = 0;
    int logged = 0;
    common->Printf("PathTracePrimaryPass: RT smoke GUI dump drawSurfs=%d allowGuiSurfaces=%d allowGuiTextures=%d\n",
        viewDef->numDrawSurfs,
        r_pathTracingAllowGuiSurfaces.GetInteger() != 0 ? 1 : 0,
        r_pathTracingAllowGuiTextures.GetInteger() != 0 ? 1 : 0);

    for (int surfaceIndex = 0; surfaceIndex < viewDef->numDrawSurfs; ++surfaceIndex)
    {
        const drawSurf_t* drawSurf = viewDef->drawSurfs[surfaceIndex];
        if (!IsSmokeGuiDrawSurface(drawSurf))
        {
            continue;
        }

        ++guiSurfaces;
        const srfTriangles_t* tri = nullptr;
        const bool captured = ValidateSmokeDrawSurface(viewDef, drawSurf, tri, nullptr);
        if (captured)
        {
            ++capturedGuiSurfaces;
        }

        if (logged >= maxLogged)
        {
            continue;
        }

        const idMaterial* material = drawSurf ? drawSurf->material : nullptr;
        const char* materialName = material ? material->GetName() : "<none>";
        const uint32_t materialId = HashSmokeMaterialName(materialName);
        int tableIndex = -1;
        std::vector<uint32_t>::const_iterator tableIt = std::find(table.materialIds.begin(), table.materialIds.end(), materialId);
        if (tableIt != table.materialIds.end())
        {
            tableIndex = static_cast<int>(tableIt - table.materialIds.begin());
        }

        idVec4 colorMin(1.0f, 1.0f, 1.0f, 1.0f);
        idVec4 colorMax(0.0f, 0.0f, 0.0f, 0.0f);
        idVec2 uvMin(1.0e20f, 1.0e20f);
        idVec2 uvMax(-1.0e20f, -1.0e20f);
        if (tri && tri->verts)
        {
            for (int vertIndex = 0; vertIndex < tri->numVerts; ++vertIndex)
            {
                const idDrawVert& vert = tri->verts[vertIndex];
                for (int component = 0; component < 4; ++component)
                {
                    const float c = vert.color[component] * (1.0f / 255.0f);
                    colorMin[component] = Min(colorMin[component], c);
                    colorMax[component] = Max(colorMax[component], c);
                }
                const idVec2 uv = vert.GetTexCoord();
                uvMin.x = Min(uvMin.x, uv.x);
                uvMin.y = Min(uvMin.y, uv.y);
                uvMax.x = Max(uvMax.x, uv.x);
                uvMax.y = Max(uvMax.y, uv.y);
            }
        }

        const RtSmokeMaterialTextureInfo info = ResolveSmokeMaterialTextureInfo(materialId, tableIndex);
        const PathTraceSmokeMaterial* rtMaterial = tableIndex >= 0 && tableIndex < static_cast<int>(table.materials.size()) ? &table.materials[tableIndex] : nullptr;
        common->Printf("PathTracePrimaryPass: RT smoke GUI surface[%d] captured=%d table=%d id=%u material='%s' verts=%d indexes=%d colorMin=(%.2f %.2f %.2f %.2f) colorMax=(%.2f %.2f %.2f %.2f) uvMin=(%.2f %.2f) uvMax=(%.2f %.2f) diffuse='%s' safe=%d handle=%d slot=%d reason='%s'\n",
            surfaceIndex,
            captured ? 1 : 0,
            tableIndex,
            materialId,
            materialName,
            tri ? tri->numVerts : 0,
            tri ? tri->numIndexes : 0,
            colorMin.x, colorMin.y, colorMin.z, colorMin.w,
            colorMax.x, colorMax.y, colorMax.z, colorMax.w,
            uvMin.x, uvMin.y,
            uvMax.x, uvMax.y,
            info.diffuseImageName.c_str(),
            info.hasSafeTexture ? 1 : 0,
            info.hasTextureHandle ? 1 : 0,
            rtMaterial && rtMaterial->diffuseTextureIndex != UINT32_MAX ? static_cast<int>(rtMaterial->diffuseTextureIndex) : -1,
            info.fallbackReason.c_str());

        if (material)
        {
            const float* regs = drawSurf && drawSurf->shaderRegisters ? drawSurf->shaderRegisters : material->ConstantRegisters();
            const int registerCount = material->GetNumRegisters();
            for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
            {
                const shaderStage_t* stage = material->GetStage(stageIndex);
                if (!stage)
                {
                    continue;
                }

                idVec4 stageColor(1.0f, 1.0f, 1.0f, 1.0f);
                if (regs)
                {
                    for (int component = 0; component < 4; ++component)
                    {
                        const int colorRegister = stage->color.registers[component];
                        if (colorRegister >= 0 && colorRegister < registerCount)
                        {
                            stageColor[component] = regs[colorRegister];
                        }
                    }
                }
                common->Printf("PathTracePrimaryPass: RT smoke GUI surface[%d] stage[%d] lighting=%s color=(%.3f %.3f %.3f %.3f) texgen=%s dynamic=%d image='%s' safe=%d\n",
                    surfaceIndex,
                    stageIndex,
                    SmokeStageLightingName(stage->lighting),
                    stageColor.x, stageColor.y, stageColor.z, stageColor.w,
                    SmokeTexgenName(stage->texture.texgen),
                    static_cast<int>(stage->texture.dynamic),
                    stage->texture.image ? stage->texture.image->GetName() : "<none>",
                    stage->texture.image && IsSmokeDiffuseImageSafeForRayTracing(stage->texture.image) ? 1 : 0);
            }
        }

        ++logged;
    }

    common->Printf("PathTracePrimaryPass: RT smoke GUI dump summary guiSurfaces=%d captured=%d logged=%d\n",
        guiSurfaces,
        capturedGuiSurfaces,
        logged);
}
