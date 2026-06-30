#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCVars.h"
#include "PathTraceSurfaceDebugDumps.h"
#include "PathTraceDebugDumps.h"
#include "PathTraceDoomMaterialClassifier.h"
#include "PathTraceGuiSurfaces.h"
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
    if (info.detailDecalLiquidPool)
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

    if (translucent && translucentSubtype == RtSmokeTranslucentSubtype::GuiScreen)
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
    else if (feature.modifierKind == RT_PATH_TRACE_MATERIAL_MODIFIER_LIQUID_POOL_UNION)
    {
        feature.materialKind = RT_PATH_TRACE_MATERIAL_KIND_LIQUID_POOL_MODIFIER;
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
        feature.transmissionActive = r_pathTracingTransmissionBounceLimit.GetInteger() > 0;
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


void LogSmokeCrosshairMaterialDump(const viewDef_t* viewDef, const RtSmokeMaterialTableBuild& table)
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
    const uint32_t materialId = SmokeMaterialId(material);
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

    common->Printf("PathTracePrimaryPass: RT smoke crosshair material hit surface=%d triangle=%d point=(%.2f %.2f %.2f) material='%s' id=%u tableIndex=%d class=%s subtype=%s coverage=%s sort=%.2f deform=%s cull=%d stages=%d guiSurface=%d\n",
        surfaceIndex,
        triangleIndex,
        hitPoint.x,
        hitPoint.y,
        hitPoint.z,
        material->GetName(),
        materialId,
        tableIndex,
        SmokeSurfaceClassName(surfaceClass),
        SmokeTranslucentSubtypeName(translucentSubtype),
        SmokeCoverageName(material->Coverage()),
        material->GetSort(),
        SmokeDeformName(material->Deform()),
        static_cast<int>(material->GetCullType()),
        material->GetNumStages(),
        IsSmokeGuiDrawSurface(drawSurf) ? 1 : 0);

    common->Printf("PathTracePrimaryPass: RT smoke crosshair classifiers guiSort=%d decalSort=%d postSort=%d polyOffset=%d screenTex=%d addDefault0200=%d addBlend=%d ambient=%d ambientBlend=%d diffuse=%d nameGui=%d nameParticle=%d nameDecal=%d nameGlass=%d nameGlow=%d nameSignage=%d\n",
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
        classifier.nameLooksSignage ? 1 : 0);

    common->Printf("PathTracePrimaryPass: RT smoke crosshair detail-decal isDetailDecal=%d isDynamic=%d blendKind=%s spectrum=%d compositeStage=%d\n",
        info.detailDecal ? 1 : 0,
        info.detailDecalDynamic ? 1 : 0,
        info.detailDecalLiquidPool ? "liquid-pool" : (info.detailDecalDiffuseLit ? "diffuse-lit" : (info.filterDecal ? "modulate" : (info.additiveDecal ? "additive" : "over"))),
        info.detailDecalSpectrum,
        r_pathTracingDecalComposite.GetInteger());

    const RtCrosshairMaterialFeatureDebug feature = BuildCrosshairMaterialFeatureDebug(surfaceClass, translucentSubtype, info);
    const bool directReservoirSupported =
        (feature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_OPAQUE_DIRECT) != 0u &&
        (feature.passSupport & RT_PATH_TRACE_MATERIAL_PASS_DIRECT_RESERVOIR) != 0u &&
        (feature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_DEBUG_FAIL_CLOSED) == 0u;
    const bool giReservoirSupported =
        (feature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_OPAQUE_GI) != 0u &&
        (feature.passSupport & RT_PATH_TRACE_MATERIAL_PASS_GI_RESERVOIR) != 0u &&
        (feature.materialCaps & RT_PATH_TRACE_MATERIAL_CAP_DEBUG_FAIL_CLOSED) == 0u;
    const int transmissionBounceRequest = r_pathTracingTransmissionBounceLimit.GetInteger();
    const int transmissionBounceEffective = idMath::ClampInt(0, 1, transmissionBounceRequest);
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

    common->Printf("PathTracePrimaryPass: RT smoke crosshair RT metadata diffuse='%s' usage=%s color=%s image=%d handle=%d safe=%d reason='%s' alpha='%s' usage=%s color=%s image=%d handle=%d safe=%d reason='%s' hasAlphaTest=%d cutoff=%.3f alphaFromLuma=%d alphaDarkKey=%d alphaMagentaKey=%d normal='%s' usage=%s color=%s safe=%d specular='%s' usage=%s color=%s safe=%d emissive='%s' usage=%s color=%s safe=%d emissive=%d additiveDecal=%d additiveWhiteKey=%d filterDecal=%d blackKey=%d forceAlbedo=%d portalFallback=%d objectGlassFallback=%d fallbackAlbedo=%d(%.2f %.2f %.2f)\n",
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
    }

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
        idImage* image = stage->texture.image;
        const bool imageSafe = image && IsSmokeDiffuseImageSafeForRayTracing(image);
        const textureUsage_t imageUsage = image ? image->GetUsage() : TD_DEFAULT;
        const textureColor_t imageColorFormat = image ? image->GetOpts().colorFormat : CFM_DEFAULT;
        common->Printf("PathTracePrimaryPass: RT smoke crosshair stage[%d] lighting=%s condition=%.3f color=(%.3f %.3f %.3f %.3f) drawState=0x%llx srcBlend=%llu dstBlend=%llu alphaTest=%d alphaReg=%d alphaValue=%.3f ignoreAlpha=%d alphaSemantic=%s texgen=%s dynamic=%d cinematic=%d image='%s' usage=%s color=%s safe=%d\n",
            stageIndex,
            SmokeStageLightingName(stage->lighting),
            condition,
            stageColor.x,
            stageColor.y,
            stageColor.z,
            stageColor.w,
            static_cast<unsigned long long>(stage->drawStateBits),
            static_cast<unsigned long long>((stage->drawStateBits & GLS_SRCBLEND_BITS) >> 0),
            static_cast<unsigned long long>((stage->drawStateBits & GLS_DSTBLEND_BITS) >> 3),
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
