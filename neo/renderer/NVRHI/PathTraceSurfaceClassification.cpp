#include "precompiled.h"
#pragma hdrstop

#include "PathTraceSurfaceClassification.h"
#include "PathTraceCVars.h"
#include "PathTraceDoomMaterialClassifier.h"
#include "PathTraceGuiSurfaces.h"
#include "PathTraceOwnerSemanticKernel.h"
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
    if (!material)
    {
        return false;
    }
    RtSmokeMaterialRouteInput input;
    input.materialPresent = true;
    idStr::Copynz(input.materialName, material->GetName(),
        sizeof(input.materialName));
    input.coverage = static_cast<int>(material->Coverage());
    input.deform = static_cast<int>(material->Deform());
    input.stageCount = material->GetNumStages();
    input.sort = material->GetSort();
    return SmokeMaterialCanPromoteRigidEmissiveCardFromPod(
        input, allowSwinglightRuntimeState, classifier);
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

bool UnifiedPtDiagnosticRemovesAlphaClipSurface(const idMaterial* material)
{
    if (!material ||
        r_pathTracingUnifiedPtEnable.GetInteger() == 0 ||
        r_pathTracingUnifiedPtRemoveAlphaClipSurfaces.GetInteger() == 0)
    {
        return false;
    }

    bool hasAlphaTest = false;
    float alphaCutoff = 0.0f;
    ResolveSmokeMaterialAlphaInfo(material, hasAlphaTest, alphaCutoff);
    (void)alphaCutoff;
    return hasAlphaTest;
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
    RtSmokeMaterialRouteInput input;
    input.materialPresent = true;
    idStr::Copynz(input.materialName, material->GetName(), sizeof(input.materialName));
    input.coverage = static_cast<int>(material->Coverage());
    input.deform = static_cast<int>(material->Deform());
    input.stageCount = material->GetNumStages();
    input.sort = material->GetSort();
    input.singleStageAmbientAlphaBlend = stage->lighting == SL_AMBIENT &&
        srcBlend == GLS_SRCBLEND_SRC_ALPHA &&
        dstBlend == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA;
    return SmokeMaterialUsesOpaqueSwinglightCompatibilityFromPod(input, classifier);
}

bool SmokeMaterialCanPromoteRigidEmissiveCard(const idMaterial* material)
{
    return SmokeMaterialCanPromoteRigidEmissiveCardInternal(material, false);
}

bool SmokeMaterialCanPromoteRigidEmissiveCard(
    const idMaterial* material,
    const RtSmokeTranslucentClassifierInfo& classifier)
{
    return SmokeMaterialCanPromoteRigidEmissiveCardWithClassifierInternal(
        material, false, classifier);
}

bool SmokeMaterialCanPromoteEntityFeedRigidEmissiveCard(const idMaterial* material)
{
    return SmokeMaterialCanPromoteRigidEmissiveCardInternal(material, true);
}

bool SmokeMaterialCanPromoteEntityFeedRigidEmissiveCard(const idMaterial* material, const RtSmokeTranslucentClassifierInfo& classifier)
{
    return SmokeMaterialCanPromoteRigidEmissiveCardWithClassifierInternal(material, true, classifier);
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

    RtSmokeSurfaceClassifyInput input;
    input.hasJointCache = drawSurf && drawSurf->jointCache != 0;
    input.hasStaticModelWithJoints = tri && tri->staticModelWithJoints != nullptr;
    input.hasRenderEntityJoints = renderEntity && renderEntity->joints != nullptr && renderEntity->numJoints > 0;
    input.isWorldSpace = viewDef && space == &viewDef->worldSpace;
    input.ambientCacheIsStatic = drawSurf && idVertexCache::CacheIsStatic(drawSurf->ambientCache);
    input.indexCacheIsStatic = drawSurf && idVertexCache::CacheIsStatic(drawSurf->indexCache);
    input.hasEntityDef = entityDef != nullptr;
    input.modelDepthHack = space ? space->modelDepthHack : 0.0f;
    input.material.materialPresent = material != nullptr;
    if (material)
    {
        idStr::Copynz(input.material.materialName, material->GetName(),
            sizeof(input.material.materialName));
        input.material.coverage = static_cast<int>(material->Coverage());
        input.material.deform = static_cast<int>(material->Deform());
        input.material.stageCount = material->GetNumStages();
        input.material.sort = material->GetSort();
        input.material.guiSurface = IsSmokeGuiDrawSurface(drawSurf);
        const shaderStage_t* stage = input.material.stageCount == 1 ? material->GetStage(0) : nullptr;
        if (stage)
        {
            const uint64 srcBlend = stage->drawStateBits & GLS_SRCBLEND_BITS;
            const uint64 dstBlend = stage->drawStateBits & GLS_DSTBLEND_BITS;
            input.material.singleStageAmbientAlphaBlend = stage->lighting == SL_AMBIENT &&
                srcBlend == GLS_SRCBLEND_SRC_ALPHA &&
                dstBlend == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA;
        }
    }
    const RtSmokeTranslucentClassifierInfo classifier =
        BuildSmokeTranslucentClassifierInfo(material);
    return ClassifySmokeSurfaceFromPod(input, classifier);
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

    const RtSmokeTranslucentClassifierInfo info = BuildSmokeTranslucentClassifierInfo(material);
    return ClassifySmokeTranslucentSubtype(drawSurf, info);
}

RtSmokeTranslucentSubtype ClassifySmokeTranslucentSubtype(
    const drawSurf_t* drawSurf,
    const RtSmokeTranslucentClassifierInfo& info)
{
    const idMaterial* material = drawSurf ? drawSurf->material : nullptr;
    if (!material)
    {
        return RtSmokeTranslucentSubtype::Unknown;
    }

    RtSmokeMaterialRouteInput input;
    input.materialPresent = true;
    idStr::Copynz(input.materialName, material->GetName(), sizeof(input.materialName));
    input.coverage = static_cast<int>(material->Coverage());
    input.deform = static_cast<int>(material->Deform());
    input.stageCount = material->GetNumStages();
    input.sort = material->GetSort();
    input.guiSurface = IsSmokeGuiDrawSurface(drawSurf);
    return ClassifySmokeTranslucentSubtypeFromPod(input, info);
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
    bool hasAlphaTest = false;
    float alphaCutoff = 0.0f;
    if (material)
    {
        ResolveSmokeMaterialAlphaInfo(material, classifier, hasAlphaTest, alphaCutoff);
    }
    RtSmokeMaterialRouteInput input;
    input.materialPresent = material != nullptr;
    idStr::Copynz(input.materialName,
        material ? material->GetName() : "", sizeof(input.materialName));
    input.coverage = material ? static_cast<int>(material->Coverage()) : 0;
    input.deform = material ? static_cast<int>(material->Deform()) : 0;
    input.stageCount = material ? material->GetNumStages() : 0;
    input.sort = material ? material->GetSort() : 0.0f;
    input.hasAlphaTest = hasAlphaTest;
    return SmokeMaterialRouteClassSignatureFromPod(
        input, surfaceClass, subtype, classifier);
}
