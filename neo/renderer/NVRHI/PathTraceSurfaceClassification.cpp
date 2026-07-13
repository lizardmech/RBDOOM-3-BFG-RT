#include "precompiled.h"
#pragma hdrstop

#include "PathTraceSurfaceClassification.h"
#include "PathTraceDoomMaterialClassifier.h"
#include "PathTraceGuiSurfaces.h"
#include "../RenderCommon.h"

namespace {

bool SmokeMaterialLooksTransient(const idMaterial* material, bool guiSurface, float modelDepthHack)
{
    if (!material)
    {
        return false;
    }

    const deform_t deform = material->Deform();
    const bool opaqueSwinglightCompatibility = SmokeMaterialUsesOpaqueSwinglightCompatibility(material);
    return
        guiSurface ||
        (!opaqueSwinglightCompatibility && material->Coverage() == MC_TRANSLUCENT) ||
        deform == DFRM_SPRITE ||
        deform == DFRM_TUBE ||
        deform == DFRM_FLARE ||
        deform == DFRM_PARTICLE ||
        deform == DFRM_PARTICLE2 ||
        (!opaqueSwinglightCompatibility && material->GetSort() >= SS_MEDIUM) ||
        modelDepthHack != 0.0f;
}

bool EntityFeedRigidEntityEligible(const idRenderEntityLocal* entity, const idRenderModel* model)
{
    const renderEntity_t* renderEntity = entity ? &entity->parms : nullptr;
    if (!entity || !renderEntity || !model || model->IsStaticWorldModel())
    {
        return false;
    }
    if (model->IsDynamicModel() != DM_STATIC)
    {
        return false;
    }
    if (renderEntity->joints != nullptr || renderEntity->numJoints > 0)
    {
        return false;
    }
    if (renderEntity->callback != nullptr || renderEntity->forceUpdate != 0)
    {
        return false;
    }
    if (renderEntity->weaponDepthHack || renderEntity->modelDepthHack != 0.0f)
    {
        return false;
    }
    return true;
}

bool EntityFeedSurfaceHasJointData(const idRenderEntityLocal* entity, const idRenderModel* model, const srfTriangles_t* tri)
{
    const renderEntity_t* renderEntity = entity ? &entity->parms : nullptr;
    return
        (tri && tri->staticModelWithJoints != nullptr) ||
        (renderEntity && renderEntity->joints != nullptr && renderEntity->numJoints > 0) ||
        (model && model->NumJoints() > 0);
}

bool SmokeMaterialCanPromoteRigidEmissiveCardWithClassifierInternal(
    const idMaterial* material,
    bool allowSwinglightRuntimeState,
    const RtSmokeTranslucentClassifierInfo& classifier)
{
    if (!material || material->Deform() != DFRM_NONE)
    {
        return false;
    }
    if (material->Coverage() != MC_TRANSLUCENT)
    {
        return false;
    }

    const char* materialName = material->GetName();
    if (!allowSwinglightRuntimeState && materialName && idStr::FindText(materialName, "swinglight", false) >= 0)
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

bool SmokeMaterialCanPromoteRigidEmissiveCardInternal(const idMaterial* material, bool allowSwinglightRuntimeState)
{
    if (!material || material->Deform() != DFRM_NONE)
    {
        return false;
    }
    if (material->Coverage() != MC_TRANSLUCENT)
    {
        return false;
    }

    const RtSmokeTranslucentClassifierInfo classifier = BuildSmokeTranslucentClassifierInfo(material);
    return SmokeMaterialCanPromoteRigidEmissiveCardWithClassifierInternal(material, allowSwinglightRuntimeState, classifier);
}

}

bool SmokeMaterialUsesOpaqueSwinglightCompatibility(const idMaterial* material)
{
    if (!material || material->Coverage() != MC_TRANSLUCENT || material->Deform() != DFRM_NONE || material->GetNumStages() != 1)
    {
        return false;
    }
    if (idStr::Icmp(material->GetName(), "models/mapobjects/swinglights/work/swinglighttex2") != 0)
    {
        return false;
    }

    const RtSmokeTranslucentClassifierInfo classifier = BuildSmokeTranslucentClassifierInfo(material);
    const shaderStage_t* stage = material->GetStage(0);
    if (!stage)
    {
        return false;
    }
    const uint64 srcBlend = stage->drawStateBits & GLS_SRCBLEND_BITS;
    const uint64 dstBlend = stage->drawStateBits & GLS_DSTBLEND_BITS;
    return stage->lighting == SL_AMBIENT &&
        srcBlend == GLS_SRCBLEND_SRC_ALPHA &&
        dstBlend == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA &&
        classifier.hasAmbientBlendStage &&
        !classifier.hasDiffuseStage &&
        !classifier.hasAdditiveBlend &&
        !classifier.hasScreenTexgen &&
        !classifier.hasAddDefault0200Texture;
}

bool SmokeMaterialCanPromoteRigidEmissiveCard(const idMaterial* material)
{
    return SmokeMaterialCanPromoteRigidEmissiveCardInternal(material, false);
}

bool SmokeMaterialCanPromoteEntityFeedRigidEmissiveCard(const idMaterial* material)
{
    return SmokeMaterialCanPromoteRigidEmissiveCardInternal(material, true);
}

bool SmokeMaterialCanPromoteEntityFeedRigidEmissiveCard(const idMaterial* material, const RtSmokeTranslucentClassifierInfo& classifier)
{
    return SmokeMaterialCanPromoteRigidEmissiveCardWithClassifierInternal(material, true, classifier);
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

const char* SmokeSurfaceClassName(RtSmokeSurfaceClass surfaceClass)
{
    switch (surfaceClass)
    {
        case RtSmokeSurfaceClass::StaticWorld:
            return "static";
        case RtSmokeSurfaceClass::RigidEntity:
            return "rigid-entity";
        case RtSmokeSurfaceClass::SkinnedDeformed:
            return "skinned";
        case RtSmokeSurfaceClass::ParticleAlpha:
            return "particle/alpha";
        default:
            return "unknown";
    }
}

const char* SmokeSurfaceClassNameByIndex(int classIndex)
{
    if (classIndex < 0 || classIndex >= RT_SMOKE_CLASS_COUNT)
    {
        return "invalid";
    }

    return SmokeSurfaceClassName(static_cast<RtSmokeSurfaceClass>(classIndex));
}

const char* RtPtFeedClassName(RtPtFeedClass feedClass)
{
    switch (feedClass)
    {
        case RtPtFeedClass::StaticWorld:
            return "static-world";
        case RtPtFeedClass::RigidEntity:
            return "rigid-entity";
        case RtPtFeedClass::RigidSkinned:
            return "rigid-skinned";
        case RtPtFeedClass::TrueDeform:
            return "true-deform";
        default:
            return "transient";
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

const char* SmokeTranslucentSubtypeName(RtSmokeTranslucentSubtype subtype)
{
    switch (subtype)
    {
        case RtSmokeTranslucentSubtype::DecalGrime:
            return "decal/grime";
        case RtSmokeTranslucentSubtype::ObjectGlass:
            return "object-glass";
        case RtSmokeTranslucentSubtype::SmokeParticle:
            return "smoke/particle";
        case RtSmokeTranslucentSubtype::SignageGlow:
            return "signage/glow";
        case RtSmokeTranslucentSubtype::PortalWindow:
            return "portal/window";
        case RtSmokeTranslucentSubtype::GuiScreen:
            return "gui/screen";
        default:
            return "unknown";
    }
}

const char* SmokeTranslucentSubtypeNameByIndex(int subtypeIndex)
{
    if (subtypeIndex < 0 || subtypeIndex >= RT_SMOKE_TRANSLUCENT_SUBTYPE_COUNT)
    {
        return "invalid";
    }

    return SmokeTranslucentSubtypeName(static_cast<RtSmokeTranslucentSubtype>(subtypeIndex));
}

RtSmokeSurfaceClass ClassifySmokeSurface(const viewDef_t* viewDef, const drawSurf_t* drawSurf, const srfTriangles_t* tri)
{
    const viewEntity_t* space = drawSurf ? drawSurf->space : nullptr;
    const idRenderEntityLocal* entityDef = space ? space->entityDef : nullptr;
    const renderEntity_t* renderEntity = entityDef ? &entityDef->parms : nullptr;
    const idMaterial* material = drawSurf ? drawSurf->material : nullptr;

    if ((drawSurf && drawSurf->jointCache != 0) ||
        (tri && tri->staticModelWithJoints != nullptr) ||
        (renderEntity && renderEntity->joints != nullptr && renderEntity->numJoints > 0))
    {
        return RtSmokeSurfaceClass::SkinnedDeformed;
    }

    if (material)
    {
        const deform_t deform = material->Deform();
        const bool opaqueSwinglightCompatibility = SmokeMaterialUsesOpaqueSwinglightCompatibility(material);
        if (IsSmokeGuiDrawSurface(drawSurf) ||
            (!opaqueSwinglightCompatibility && material->Coverage() == MC_TRANSLUCENT) ||
            deform == DFRM_SPRITE ||
            deform == DFRM_TUBE ||
            deform == DFRM_FLARE ||
            deform == DFRM_PARTICLE ||
            deform == DFRM_PARTICLE2 ||
            (!opaqueSwinglightCompatibility && material->GetSort() >= SS_MEDIUM) ||
            (space && space->modelDepthHack != 0.0f))
        {
            return RtSmokeSurfaceClass::ParticleAlpha;
        }
    }

    if ((viewDef && space == &viewDef->worldSpace) ||
        !entityDef ||
        (drawSurf && idVertexCache::CacheIsStatic(drawSurf->ambientCache) && idVertexCache::CacheIsStatic(drawSurf->indexCache) && !entityDef))
    {
        return RtSmokeSurfaceClass::StaticWorld;
    }

    if (entityDef)
    {
        return RtSmokeSurfaceClass::RigidEntity;
    }

    return RtSmokeSurfaceClass::Unknown;
}

bool IsEntityFeedSingleBoneSurface(const srfTriangles_t* tri)
{
    if (!tri || !tri->verts || tri->numVerts <= 0)
    {
        return false;
    }

    int surfaceJoint = -1;
    for (int vertIndex = 0; vertIndex < tri->numVerts; ++vertIndex)
    {
        const idDrawVert& vert = tri->verts[vertIndex];
        int weightedComponent = -1;
        for (int component = 0; component < 4; ++component)
        {
            if (vert.color2[component] == 0)
            {
                continue;
            }
            if (vert.color2[component] != 255 || weightedComponent >= 0)
            {
                return false;
            }
            weightedComponent = component;
        }

        if (weightedComponent < 0)
        {
            return false;
        }

        const int jointIndex = vert.color[weightedComponent];
        if (surfaceJoint < 0)
        {
            surfaceJoint = jointIndex;
        }
        else if (surfaceJoint != jointIndex)
        {
            return false;
        }
    }

    return true;
}

RtPtFeedClass ClassifyEntityFeedSurface(const idRenderEntityLocal* entity, const idRenderModel* model, const modelSurface_t* surface)
{
    if (!entity || !model)
    {
        return RtPtFeedClass::Transient;
    }

    const renderEntity_t& renderEntity = entity->parms;
    const srfTriangles_t* tri = surface ? surface->geometry : nullptr;
    const idMaterial* surfaceMaterial = surface ? surface->shader : nullptr;
    const idMaterial* material = R_RemapShaderBySkin(surfaceMaterial, renderEntity.customSkin, renderEntity.customShader);

    if (SmokeMaterialLooksTransient(material, false, renderEntity.modelDepthHack) ||
        renderEntity.callback != nullptr ||
        renderEntity.forceUpdate != 0)
    {
        return RtPtFeedClass::Transient;
    }

    if (model->IsStaticWorldModel())
    {
        return RtPtFeedClass::StaticWorld;
    }

    if (EntityFeedSurfaceHasJointData(entity, model, tri))
    {
        return IsEntityFeedSingleBoneSurface(tri) ? RtPtFeedClass::RigidSkinned : RtPtFeedClass::TrueDeform;
    }

    if (EntityFeedRigidEntityEligible(entity, model))
    {
        return RtPtFeedClass::RigidEntity;
    }

    return RtPtFeedClass::Transient;
}

RtSmokeTranslucentSubtype ClassifySmokeTranslucentSubtype(const drawSurf_t* drawSurf)
{
    const idMaterial* material = drawSurf ? drawSurf->material : nullptr;
    if (!material)
    {
        return RtSmokeTranslucentSubtype::Unknown;
    }

    const deform_t deform = material->Deform();
    const float sort = material->GetSort();
    const RtSmokeTranslucentClassifierInfo info = BuildSmokeTranslucentClassifierInfo(material);

    if (IsSmokeGuiDrawSurface(drawSurf) || info.hasScreenTexgen || info.nameLooksGui)
    {
        return RtSmokeTranslucentSubtype::GuiScreen;
    }

    if (info.sortIsPostProcess || info.sortIsGuiOrSubview)
    {
        return RtSmokeTranslucentSubtype::PortalWindow;
    }

    if (deform == DFRM_PARTICLE ||
        deform == DFRM_PARTICLE2 ||
        deform == DFRM_SPRITE ||
        deform == DFRM_TUBE ||
        deform == DFRM_FLARE ||
        sort >= SS_ALMOST_NEAREST ||
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

uint32_t SmokeSurfaceClassAndSubtypeId(RtSmokeSurfaceClass surfaceClass, RtSmokeTranslucentSubtype subtype)
{
    uint32_t id = SmokeSurfaceClassId(surfaceClass);
    if (surfaceClass == RtSmokeSurfaceClass::ParticleAlpha)
    {
        id |= (SmokeTranslucentSubtypeId(subtype) << RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT) & RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK;
    }
    return id;
}

uint32_t SmokeMaterialRouteClassSignature(const idMaterial* material, RtSmokeSurfaceClass surfaceClass, RtSmokeTranslucentSubtype subtype)
{
    if (!material)
    {
        return
            (SmokeSurfaceClassId(surfaceClass) & 0x0fu) |
            ((SmokeTranslucentSubtypeId(subtype) & 0x0fu) << 4);
    }

    const RtSmokeTranslucentClassifierInfo classifier = BuildSmokeTranslucentClassifierInfo(material);
    return SmokeMaterialRouteClassSignature(material, surfaceClass, subtype, classifier);
}

uint32_t SmokeMaterialRouteClassSignature(
    const idMaterial* material,
    RtSmokeSurfaceClass surfaceClass,
    RtSmokeTranslucentSubtype subtype,
    const RtSmokeTranslucentClassifierInfo& classifier)
{
    uint32_t signature =
        (SmokeSurfaceClassId(surfaceClass) & 0x0fu) |
        ((SmokeTranslucentSubtypeId(subtype) & 0x0fu) << 4);
    if (!material)
    {
        return signature;
    }

    bool hasAlphaTest = false;
    float alphaCutoff = 0.0f;
    ResolveSmokeMaterialAlphaInfo(material, classifier, hasAlphaTest, alphaCutoff);
    const uint32_t coverage = static_cast<uint32_t>(material->Coverage()) & 0x0fu;
    const uint32_t deform = static_cast<uint32_t>(material->Deform()) & 0x0fu;
    const bool routeSortMediumOrLater = material->GetSort() >= SS_MEDIUM;

    signature |= coverage << 8;
    signature |= deform << 12;
    signature |= hasAlphaTest ? (1u << 16) : 0u;
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
