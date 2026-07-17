#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCVars.h"
#include "PathTraceDebugDumps.h"
#include "PathTraceDoomMaterialClassifier.h"
#include "PathTraceGuiSurfaces.h"
#include "PathTraceParticleCapture.h"
#include "PathTraceSceneCapture.h"
#include "PathTraceSurfaceClassification.h"
#include "../RenderCommon.h"

#include <vector>

namespace {

enum class ParticleAuditBlendClass
{
    AlphaLit,
    AlphaEmissive,
    PureAdditiveEmissive,
    Unsupported
};

struct ParticleAuditStage
{
    int stageIndex = -1;
    ParticleAuditBlendClass blendClass = ParticleAuditBlendClass::Unsupported;
    uint64 srcBlend = 0;
    uint64 dstBlend = 0;
    idStr reason;
};

struct ParticleAuditMaterialCount
{
    idStr name;
    int surfaces = 0;
    int quads = 0;
    int stages = 0;
    int particleAlphaBvhSurfaces = 0;
    int weaponContextSurfaces = 0;
};

struct ParticleAuditEntityCount
{
    int entityIndex = -1;
    int entityNum = -1;
    idStr modelName;
    int allowSurfaceInViewId = 0;
    int surfaces = 0;
    int quads = 0;
    int weaponDepthHackSurfaces = 0;
    int modelDepthHackSurfaces = 0;
};

struct ParticleAuditDeformCount
{
    deform_t deform = DFRM_NONE;
    int surfaces = 0;
    int quads = 0;
};

struct ParticleAuditBlendCount
{
    ParticleAuditBlendClass blendClass = ParticleAuditBlendClass::Unsupported;
    uint64 srcBlend = 0;
    uint64 dstBlend = 0;
    int stages = 0;
    int surfaces = 0;
    int quads = 0;
};

const char* ParticleAuditBlendClassName(ParticleAuditBlendClass blendClass)
{
    switch (blendClass)
    {
        case ParticleAuditBlendClass::AlphaLit:
            return "alpha-lit";
        case ParticleAuditBlendClass::AlphaEmissive:
            return "alpha-emissive";
        case ParticleAuditBlendClass::PureAdditiveEmissive:
            return "pure-additive-emissive";
        default:
            return "unsupported";
    }
}

const char* ParticleAuditSrcBlendName(uint64 blend)
{
    switch (blend)
    {
        case GLS_SRCBLEND_ZERO: return "ZERO";
        case GLS_SRCBLEND_ONE: return "ONE";
        case GLS_SRCBLEND_DST_COLOR: return "DST_COLOR";
        case GLS_SRCBLEND_ONE_MINUS_DST_COLOR: return "ONE_MINUS_DST_COLOR";
        case GLS_SRCBLEND_SRC_ALPHA: return "SRC_ALPHA";
        case GLS_SRCBLEND_ONE_MINUS_SRC_ALPHA: return "ONE_MINUS_SRC_ALPHA";
        case GLS_SRCBLEND_DST_ALPHA: return "DST_ALPHA";
        case GLS_SRCBLEND_ONE_MINUS_DST_ALPHA: return "ONE_MINUS_DST_ALPHA";
        default: return "UNKNOWN";
    }
}

const char* ParticleAuditDstBlendName(uint64 blend)
{
    switch (blend)
    {
        case GLS_DSTBLEND_ZERO: return "ZERO";
        case GLS_DSTBLEND_ONE: return "ONE";
        case GLS_DSTBLEND_SRC_COLOR: return "SRC_COLOR";
        case GLS_DSTBLEND_ONE_MINUS_SRC_COLOR: return "ONE_MINUS_SRC_COLOR";
        case GLS_DSTBLEND_SRC_ALPHA: return "SRC_ALPHA";
        case GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA: return "ONE_MINUS_SRC_ALPHA";
        case GLS_DSTBLEND_DST_ALPHA: return "DST_ALPHA";
        case GLS_DSTBLEND_ONE_MINUS_DST_ALPHA: return "ONE_MINUS_DST_ALPHA";
        default: return "UNKNOWN";
    }
}

ParticleAuditBlendClass ParticleAuditClassifyBlend(uint64 srcBlend, uint64 dstBlend)
{
    if (srcBlend == GLS_SRCBLEND_SRC_ALPHA && dstBlend == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA)
    {
        return ParticleAuditBlendClass::AlphaLit;
    }
    if (srcBlend == GLS_SRCBLEND_SRC_ALPHA && dstBlend == GLS_DSTBLEND_ONE)
    {
        return ParticleAuditBlendClass::AlphaEmissive;
    }
    if (srcBlend == GLS_SRCBLEND_ONE && dstBlend == GLS_DSTBLEND_ONE)
    {
        return ParticleAuditBlendClass::PureAdditiveEmissive;
    }
    return ParticleAuditBlendClass::Unsupported;
}

bool ParticleAuditSupportedDeform(deform_t deform)
{
    return deform == DFRM_PARTICLE ||
        deform == DFRM_PARTICLE2 ||
        deform == DFRM_SPRITE ||
        deform == DFRM_TUBE ||
        deform == DFRM_FLARE;
}

bool ParticleAuditStageActive(const idMaterial* material, const drawSurf_t* drawSurf, const shaderStage_t* stage)
{
    if (!material || !stage)
    {
        return false;
    }
    const float* registers = drawSurf && drawSurf->shaderRegisters
        ? drawSurf->shaderRegisters
        : material->ConstantRegisters();
    if (!registers)
    {
        return true;
    }
    const int conditionRegister = stage->conditionRegister;
    return conditionRegister < 0 ||
        conditionRegister >= material->GetNumRegisters() ||
        registers[conditionRegister] != 0.0f;
}

ParticleAuditStage ParticleAuditInspectStage(const drawSurf_t* drawSurf, int stageIndex)
{
    ParticleAuditStage result;
    result.stageIndex = stageIndex;
    const idMaterial* material = drawSurf ? drawSurf->material : nullptr;
    const shaderStage_t* stage = material ? material->GetStage(stageIndex) : nullptr;
    if (!stage)
    {
        result.reason = "null-stage";
        return result;
    }

    const uint64 drawState = stage->drawStateBits | drawSurf->extraGLState;
    result.srcBlend = drawState & GLS_SRCBLEND_BITS;
    result.dstBlend = drawState & GLS_DSTBLEND_BITS;
    result.blendClass = ParticleAuditClassifyBlend(result.srcBlend, result.dstBlend);

    if (stage->lighting != SL_AMBIENT)
    {
        result.reason = va("lighting-%s", SmokeStageLightingName(stage->lighting));
    }
    else if (stage->texture.texgen != TG_EXPLICIT)
    {
        result.reason = va("texgen-%s", SmokeTexgenName(stage->texture.texgen));
    }
    else if (stage->texture.dynamic != DI_STATIC || stage->texture.cinematic != nullptr)
    {
        result.reason = "dynamic-or-cinematic-image";
    }
    else if (stage->newStage != nullptr)
    {
        result.reason = "custom-program-stage";
    }
    else if (stage->texture.image == nullptr)
    {
        result.reason = "missing-image";
    }
    else if (result.blendClass == ParticleAuditBlendClass::Unsupported)
    {
        result.reason = "unsupported-blend";
    }
    else
    {
        result.reason = "supported";
    }
    return result;
}

bool ParticleAuditStageSupported(const ParticleAuditStage& stage)
{
    return stage.reason.Icmp("supported") == 0;
}

ParticleAuditMaterialCount& ParticleAuditFindMaterial(std::vector<ParticleAuditMaterialCount>& counts, const char* name)
{
    for (ParticleAuditMaterialCount& count : counts)
    {
        if (count.name.Icmp(name) == 0)
        {
            return count;
        }
    }
    ParticleAuditMaterialCount count;
    count.name = name;
    counts.push_back(count);
    return counts.back();
}

ParticleAuditEntityCount& ParticleAuditFindEntity(
    std::vector<ParticleAuditEntityCount>& counts,
    int entityIndex,
    int entityNum,
    const char* modelName,
    int allowSurfaceInViewId)
{
    for (ParticleAuditEntityCount& count : counts)
    {
        if (count.entityIndex == entityIndex && count.entityNum == entityNum && count.allowSurfaceInViewId == allowSurfaceInViewId)
        {
            return count;
        }
    }
    ParticleAuditEntityCount count;
    count.entityIndex = entityIndex;
    count.entityNum = entityNum;
    count.modelName = modelName;
    count.allowSurfaceInViewId = allowSurfaceInViewId;
    counts.push_back(count);
    return counts.back();
}

ParticleAuditDeformCount& ParticleAuditFindDeform(std::vector<ParticleAuditDeformCount>& counts, deform_t deform)
{
    for (ParticleAuditDeformCount& count : counts)
    {
        if (count.deform == deform)
        {
            return count;
        }
    }
    ParticleAuditDeformCount count;
    count.deform = deform;
    counts.push_back(count);
    return counts.back();
}

ParticleAuditBlendCount& ParticleAuditFindBlend(
    std::vector<ParticleAuditBlendCount>& counts,
    ParticleAuditBlendClass blendClass,
    uint64 srcBlend,
    uint64 dstBlend)
{
    for (ParticleAuditBlendCount& count : counts)
    {
        if (count.blendClass == blendClass && count.srcBlend == srcBlend && count.dstBlend == dstBlend)
        {
            return count;
        }
    }
    ParticleAuditBlendCount count;
    count.blendClass = blendClass;
    count.srcBlend = srcBlend;
    count.dstBlend = dstBlend;
    counts.push_back(count);
    return counts.back();
}

bool ParticleAuditModelLooksWeapon(const char* modelName)
{
    return modelName &&
        (idStr::FindText(modelName, "weapon", false) >= 0 ||
         idStr::FindText(modelName, "viewmodel", false) >= 0 ||
         idStr::FindText(modelName, "hands", false) >= 0);
}

}

void AuditPathTraceParticleCompositeCandidates(const viewDef_t* viewDef)
{
    const int dumpMode = r_pathTracingParticleDump.GetInteger();
    if (dumpMode == 0)
    {
        return;
    }
    r_pathTracingParticleDump.SetInteger(0);

    if (!viewDef)
    {
        common->Printf("PathTraceParticleAudit: no viewDef\n");
        return;
    }
    if (viewDef->isSubview)
    {
        common->Printf("PathTraceParticleAudit: skipped subview viewID=%d mirror=%d drawSurfs=%d\n",
            viewDef->renderView.viewID,
            viewDef->isMirror ? 1 : 0,
            viewDef->numDrawSurfs);
        return;
    }

    std::vector<ParticleAuditMaterialCount> materialCounts;
    std::vector<ParticleAuditEntityCount> entityCounts;
    std::vector<ParticleAuditDeformCount> deformCounts;
    std::vector<ParticleAuditBlendCount> blendCounts;

    int validSurfaces = 0;
    int effectTraitSurfaces = 0;
    int candidateSurfaces = 0;
    int candidateQuads = 0;
    int candidateTriangles = 0;
    int nonQuadCandidateSurfaces = 0;
    int candidateStages = 0;
    int unsupportedStages = 0;
    int unsupportedBlendStages = 0;
    int ambiguousSurfaces = 0;
    int particleAlphaBvhSurfaces = 0;
    int particleAlphaBvhQuads = 0;
    int otherBvhClassSurfaces = 0;
    int captureRejectedSurfaces = 0;
    int skinnedCandidateSurfaces = 0;
    int weaponDepthHackSurfaces = 0;
    int modelDepthHackSurfaces = 0;
    int allowViewIdSurfaces = 0;
    int excludedGui = 0;
    int excludedGlass = 0;
    int excludedDecal = 0;
    int excludedPostOrSubview = 0;
    int excludedScreenTexgen = 0;
    int excludedNoSupportedStage = 0;
    int blendOnlySurfaces = 0;
    int loggedSurfaces = 0;
    const int maxLoggedSurfaces = dumpMode > 1 ? 128 : 32;

    common->Printf("PathTraceParticleAudit: begin viewID=%d drawSurfs=%d mode=%d sceneSource=%d\n",
        viewDef->renderView.viewID,
        viewDef->numDrawSurfs,
        dumpMode,
        r_pathTracingSceneSource.GetInteger());

    for (int surfaceIndex = 0; surfaceIndex < viewDef->numDrawSurfs; ++surfaceIndex)
    {
        const drawSurf_t* drawSurf = viewDef->drawSurfs[surfaceIndex];
        const idMaterial* material = drawSurf ? drawSurf->material : nullptr;
        const srfTriangles_t* tri = drawSurf ? drawSurf->frontEndGeo : nullptr;
        if (!drawSurf || !material || !tri || !tri->verts || !tri->indexes || drawSurf->numIndexes < 3 || (drawSurf->numIndexes % 3) != 0)
        {
            continue;
        }
        ++validSurfaces;

        const deform_t deform = material->Deform();
        const RtSmokeTranslucentClassifierInfo classifier = BuildSmokeTranslucentClassifierInfo(material);
        const bool supportedDeform = ParticleAuditSupportedDeform(deform);
        const bool transientMaterialTrait =
            supportedDeform ||
            material->Coverage() == MC_TRANSLUCENT ||
            classifier.nameLooksParticle ||
            material->GetSort() >= SS_MEDIUM;

        std::vector<ParticleAuditStage> activeStages;
        int supportedStages = 0;
        for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
        {
            const shaderStage_t* shaderStage = material->GetStage(stageIndex);
            if (!shaderStage || !ParticleAuditStageActive(material, drawSurf, shaderStage))
            {
                continue;
            }
            ParticleAuditStage auditStage = ParticleAuditInspectStage(drawSurf, stageIndex);
            if (ParticleAuditStageSupported(auditStage))
            {
                ++supportedStages;
            }
            activeStages.push_back(auditStage);
        }

        if (!transientMaterialTrait)
        {
            if (supportedStages > 0)
            {
                ++blendOnlySurfaces;
            }
            continue;
        }
        ++effectTraitSurfaces;

        if (IsSmokeGuiDrawSurface(drawSurf) || classifier.nameLooksGui)
        {
            ++excludedGui;
            continue;
        }
        if (classifier.nameLooksGlass)
        {
            ++excludedGlass;
            continue;
        }
        if (classifier.sortIsDecal || classifier.polygonOffsetDecal || classifier.nameLooksDecal)
        {
            ++excludedDecal;
            continue;
        }
        if (classifier.sortIsPostProcess || classifier.sortIsGuiOrSubview)
        {
            ++excludedPostOrSubview;
            continue;
        }
        if (classifier.hasScreenTexgen)
        {
            ++excludedScreenTexgen;
            continue;
        }

        for (const ParticleAuditStage& stage : activeStages)
        {
            if (!ParticleAuditStageSupported(stage))
            {
                ++unsupportedStages;
                if (stage.blendClass == ParticleAuditBlendClass::Unsupported)
                {
                    ++unsupportedBlendStages;
                }
            }
        }
        if (supportedStages == 0)
        {
            ++excludedNoSupportedStage;
            if (dumpMode > 1 && loggedSurfaces < maxLoggedSurfaces)
            {
                common->Printf("PathTraceParticleAudit: unsupported surface=%d material='%s' deform=%s coverage=%s sort=%.2f activeStages=%d\n",
                    surfaceIndex,
                    material->GetName(),
                    SmokeDeformName(deform),
                    SmokeCoverageName(material->Coverage()),
                    material->GetSort(),
                    static_cast<int>(activeStages.size()));
                for (const ParticleAuditStage& stage : activeStages)
                {
                    common->Printf("PathTraceParticleAudit:   stage=%d blend=%s,%s raw=0x%llx,0x%llx class=%s reason=%s\n",
                        stage.stageIndex,
                        ParticleAuditSrcBlendName(stage.srcBlend),
                        ParticleAuditDstBlendName(stage.dstBlend),
                        static_cast<unsigned long long>(stage.srcBlend),
                        static_cast<unsigned long long>(stage.dstBlend),
                        ParticleAuditBlendClassName(stage.blendClass),
                        stage.reason.c_str());
                }
                ++loggedSurfaces;
            }
            continue;
        }

        ++candidateSurfaces;
        candidateStages += supportedStages;
        const int triangles = drawSurf->numIndexes / 3;
        const bool quadTopology = (drawSurf->numIndexes % 6) == 0;
        const int quads = quadTopology ? drawSurf->numIndexes / 6 : 0;
        candidateTriangles += triangles;
        candidateQuads += quads;
        if (!quadTopology)
        {
            ++nonQuadCandidateSurfaces;
        }
        if (supportedStages > 1)
        {
            ++ambiguousSurfaces;
        }

        const viewEntity_t* space = drawSurf->space;
        const idRenderEntityLocal* entity = space ? space->entityDef : nullptr;
        const renderEntity_t* renderEntity = entity ? &entity->parms : nullptr;
        const int entityIndex = entity ? entity->index : -1;
        const int entityNum = renderEntity ? renderEntity->entityNum : -1;
        const char* modelName = renderEntity && renderEntity->hModel ? renderEntity->hModel->Name() : "<world-or-none>";
        const int allowSurfaceInViewId = renderEntity ? renderEntity->allowSurfaceInViewID : 0;
        const bool weaponDepthHack = space && space->weaponDepthHack;
        const float modelDepthHack = space ? space->modelDepthHack : 0.0f;
        const bool weaponContext = weaponDepthHack || allowSurfaceInViewId != 0 || ParticleAuditModelLooksWeapon(modelName);
        if (weaponDepthHack)
        {
            ++weaponDepthHackSurfaces;
        }
        if (modelDepthHack != 0.0f)
        {
            ++modelDepthHackSurfaces;
        }
        if (allowSurfaceInViewId != 0)
        {
            ++allowViewIdSurfaces;
        }

        const RtSmokeSurfaceClass currentClass = ClassifySmokeSurface(viewDef, drawSurf, tri);
        const srfTriangles_t* validatedTri = nullptr;
        const bool currentCaptureAccepts = ValidateSmokeDrawSurface(viewDef, drawSurf, validatedTri, nullptr);
        const bool currentParticleAlphaBvh = currentCaptureAccepts && currentClass == RtSmokeSurfaceClass::ParticleAlpha;
        if (currentParticleAlphaBvh)
        {
            ++particleAlphaBvhSurfaces;
            particleAlphaBvhQuads += quads;
        }
        else if (!currentCaptureAccepts)
        {
            ++captureRejectedSurfaces;
        }
        else
        {
            ++otherBvhClassSurfaces;
        }
        if (currentClass == RtSmokeSurfaceClass::SkinnedDeformed)
        {
            ++skinnedCandidateSurfaces;
        }

        ParticleAuditMaterialCount& materialCount = ParticleAuditFindMaterial(materialCounts, material->GetName());
        ++materialCount.surfaces;
        materialCount.quads += quads;
        materialCount.stages += supportedStages;
        materialCount.particleAlphaBvhSurfaces += currentParticleAlphaBvh ? 1 : 0;
        materialCount.weaponContextSurfaces += weaponContext ? 1 : 0;

        ParticleAuditEntityCount& entityCount = ParticleAuditFindEntity(entityCounts, entityIndex, entityNum, modelName, allowSurfaceInViewId);
        ++entityCount.surfaces;
        entityCount.quads += quads;
        entityCount.weaponDepthHackSurfaces += weaponDepthHack ? 1 : 0;
        entityCount.modelDepthHackSurfaces += modelDepthHack != 0.0f ? 1 : 0;

        ParticleAuditDeformCount& deformCount = ParticleAuditFindDeform(deformCounts, deform);
        ++deformCount.surfaces;
        deformCount.quads += quads;

        for (const ParticleAuditStage& stage : activeStages)
        {
            if (!ParticleAuditStageSupported(stage))
            {
                continue;
            }
            ParticleAuditBlendCount& blendCount = ParticleAuditFindBlend(blendCounts, stage.blendClass, stage.srcBlend, stage.dstBlend);
            ++blendCount.stages;
            ++blendCount.surfaces;
            blendCount.quads += quads;
        }

        if (loggedSurfaces < maxLoggedSurfaces)
        {
            common->Printf("PathTraceParticleAudit: candidate surface=%d material='%s' entityIndex=%d entityNum=%d model='%s' verts=%d indexes=%d quads=%d deform=%s coverage=%s sort=%.2f class=%s particleAlphaBvh=%d stages=%d ambiguous=%d weaponDepthHack=%d modelDepthHack=%.3f allowViewID=%d currentViewID=%d\n",
                surfaceIndex,
                material->GetName(),
                entityIndex,
                entityNum,
                modelName,
                tri->numVerts,
                drawSurf->numIndexes,
                quads,
                SmokeDeformName(deform),
                SmokeCoverageName(material->Coverage()),
                material->GetSort(),
                SmokeSurfaceClassName(currentClass),
                currentParticleAlphaBvh ? 1 : 0,
                supportedStages,
                supportedStages > 1 ? 1 : 0,
                weaponDepthHack ? 1 : 0,
                modelDepthHack,
                allowSurfaceInViewId,
                viewDef->renderView.viewID);
            if (dumpMode > 1 || supportedStages > 1)
            {
                for (const ParticleAuditStage& stage : activeStages)
                {
                    const shaderStage_t* shaderStage = material->GetStage(stage.stageIndex);
                    common->Printf("PathTraceParticleAudit:   stage=%d image='%s' blend=%s,%s raw=0x%llx,0x%llx class=%s reason=%s\n",
                        stage.stageIndex,
                        shaderStage && shaderStage->texture.image ? shaderStage->texture.image->GetName() : "<none>",
                        ParticleAuditSrcBlendName(stage.srcBlend),
                        ParticleAuditDstBlendName(stage.dstBlend),
                        static_cast<unsigned long long>(stage.srcBlend),
                        static_cast<unsigned long long>(stage.dstBlend),
                        ParticleAuditBlendClassName(stage.blendClass),
                        stage.reason.c_str());
                }
            }
            ++loggedSurfaces;
        }
    }

    common->Printf("PathTraceParticleAudit: summary valid=%d effectTraits=%d candidates=%d quads=%d triangles=%d stages=%d nonQuad=%d ambiguous=%d unsupportedStages=%d unsupportedBlendStages=%d blendOnlyNeedsExplicitRecognition=%d\n",
        validSurfaces,
        effectTraitSurfaces,
        candidateSurfaces,
        candidateQuads,
        candidateTriangles,
        candidateStages,
        nonQuadCandidateSurfaces,
        ambiguousSurfaces,
        unsupportedStages,
        unsupportedBlendStages,
        blendOnlySurfaces);
    common->Printf("PathTraceParticleAudit: current-route particleAlphaBvh=%d(%d quads) otherBvhClass=%d captureRejected=%d skinnedCandidates=%d weaponDepthHack=%d modelDepthHack=%d allowViewID=%d\n",
        particleAlphaBvhSurfaces,
        particleAlphaBvhQuads,
        otherBvhClassSurfaces,
        captureRejectedSurfaces,
        skinnedCandidateSurfaces,
        weaponDepthHackSurfaces,
        modelDepthHackSurfaces,
        allowViewIdSurfaces);
    common->Printf("PathTraceParticleAudit: exclusions gui=%d glass=%d decal=%d postOrSubview=%d screenTexgen=%d noSupportedStage=%d\n",
        excludedGui,
        excludedGlass,
        excludedDecal,
        excludedPostOrSubview,
        excludedScreenTexgen,
        excludedNoSupportedStage);

    for (const ParticleAuditBlendCount& count : blendCounts)
    {
        common->Printf("PathTraceParticleAudit: blend class=%s equation=%s,%s raw=0x%llx,0x%llx stages=%d surfaceStageUses=%d quads=%d\n",
            ParticleAuditBlendClassName(count.blendClass),
            ParticleAuditSrcBlendName(count.srcBlend),
            ParticleAuditDstBlendName(count.dstBlend),
            static_cast<unsigned long long>(count.srcBlend),
            static_cast<unsigned long long>(count.dstBlend),
            count.stages,
            count.surfaces,
            count.quads);
    }
    for (const ParticleAuditDeformCount& count : deformCounts)
    {
        common->Printf("PathTraceParticleAudit: deform name=%s value=%d surfaces=%d quads=%d\n",
            SmokeDeformName(count.deform),
            static_cast<int>(count.deform),
            count.surfaces,
            count.quads);
    }
    for (const ParticleAuditEntityCount& count : entityCounts)
    {
        common->Printf("PathTraceParticleAudit: source entityIndex=%d entityNum=%d model='%s' allowViewID=%d surfaces=%d quads=%d weaponDepthHack=%d modelDepthHack=%d\n",
            count.entityIndex,
            count.entityNum,
            count.modelName.c_str(),
            count.allowSurfaceInViewId,
            count.surfaces,
            count.quads,
            count.weaponDepthHackSurfaces,
            count.modelDepthHackSurfaces);
    }
    for (const ParticleAuditMaterialCount& count : materialCounts)
    {
        common->Printf("PathTraceParticleAudit: material name='%s' surfaces=%d quads=%d stages=%d particleAlphaBvh=%d weaponContext=%d\n",
            count.name.c_str(),
            count.surfaces,
            count.quads,
            count.stages,
            count.particleAlphaBvhSurfaces,
            count.weaponContextSurfaces);
    }
    common->Printf("PathTraceParticleAudit: end loggedSurfaces=%d materialCount=%d entityCount=%d\n",
        loggedSurfaces,
        static_cast<int>(materialCounts.size()),
        static_cast<int>(entityCounts.size()));
}
