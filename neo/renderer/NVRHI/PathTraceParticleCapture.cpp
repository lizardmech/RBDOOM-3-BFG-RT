#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCVars.h"
#include "PathTraceDebugDumps.h"
#include "PathTraceDoomMaterialClassifier.h"
#include "PathTraceDynamicMaterialState.h"
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
    MultiplicativeDarken,
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

struct ParticleCompositeSurfaceInspection
{
    const drawSurf_t* drawSurf = nullptr;
    const srfTriangles_t* tri = nullptr;
    RtSmokeTranslucentClassifierInfo classifier;
    std::vector<ParticleAuditStage> activeStages;
    int surfaceIndex = -1;
    int supportedStageCount = 0;
    bool valid = false;
    bool supportedDeform = false;
    bool transientMaterialTrait = false;
    bool excludedGui = false;
    bool excludedGlass = false;
    bool excludedDecal = false;
    bool excludedPostOrSubview = false;
    bool excludedScreenTexgen = false;
    bool accepted = false;
    bool cardOnly = false;
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
        case ParticleAuditBlendClass::MultiplicativeDarken:
            return "multiplicative-darken";
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
    if (srcBlend == GLS_SRCBLEND_ZERO && dstBlend == GLS_DSTBLEND_ONE_MINUS_SRC_COLOR)
    {
        return ParticleAuditBlendClass::MultiplicativeDarken;
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

bool ParticleCompositeHasExplicitEffectContext(const drawSurf_t* drawSurf)
{
    const viewEntity_t* space = drawSurf ? drawSurf->space : nullptr;
    const idRenderEntityLocal* entity = space ? space->entityDef : nullptr;
    const renderEntity_t* renderEntity = entity ? &entity->parms : nullptr;
    const char* modelName = renderEntity && renderEntity->hModel ? renderEntity->hModel->Name() : nullptr;
    return (space && space->weaponDepthHack) ||
        (renderEntity && renderEntity->allowSurfaceInViewID != 0) ||
        (modelName && idStr::FindText(modelName, ".prt", false) >= 0);
}

bool ParticleCompositeSurfaceAccepted(
    const drawSurf_t* drawSurf,
    const RtSmokeTranslucentClassifierInfo& classifier,
    const std::vector<ParticleAuditStage>& activeStages,
    int& supportedStageCount)
{
    supportedStageCount = 0;
    const idMaterial* material = drawSurf ? drawSurf->material : nullptr;
    if (!drawSurf || !material || !drawSurf->frontEndGeo)
    {
        return false;
    }

    for (const ParticleAuditStage& stage : activeStages)
    {
        supportedStageCount += ParticleAuditStageSupported(stage) ? 1 : 0;
    }
    const bool transientMaterialTrait =
        ParticleAuditSupportedDeform(material->Deform()) ||
        material->Coverage() == MC_TRANSLUCENT ||
        classifier.nameLooksParticle ||
        ParticleCompositeHasExplicitEffectContext(drawSurf);
    return transientMaterialTrait &&
        supportedStageCount > 0 &&
        !IsSmokeGuiDrawSurface(drawSurf) &&
        !classifier.nameLooksGui &&
        !classifier.nameLooksGlass &&
        !classifier.sortIsDecal &&
        !classifier.polygonOffsetDecal &&
        !classifier.nameLooksDecal &&
        !classifier.sortIsPostProcess &&
        !classifier.sortIsGuiOrSubview &&
        !classifier.hasScreenTexgen;
}

ParticleCompositeSurfaceInspection ParticleCompositeInspectSurface(
    const drawSurf_t* drawSurf,
    const srfTriangles_t* tri,
    int surfaceIndex)
{
    ParticleCompositeSurfaceInspection inspection;
    inspection.surfaceIndex = surfaceIndex;
    inspection.drawSurf = drawSurf;
    inspection.tri = tri;
    const idMaterial* material = drawSurf ? drawSurf->material : nullptr;
    if (!drawSurf || !material || !tri || !tri->verts || !tri->indexes ||
        drawSurf->numIndexes < 3 || (drawSurf->numIndexes % 3) != 0)
    {
        return inspection;
    }

    inspection.valid = true;
    inspection.classifier = BuildSmokeTranslucentClassifierInfo(material);
    inspection.supportedDeform = ParticleAuditSupportedDeform(material->Deform());
        inspection.transientMaterialTrait =
            inspection.supportedDeform ||
            material->Coverage() == MC_TRANSLUCENT ||
            inspection.classifier.nameLooksParticle ||
            ParticleCompositeHasExplicitEffectContext(drawSurf);
    inspection.excludedGui = IsSmokeGuiDrawSurface(drawSurf) || inspection.classifier.nameLooksGui;
    inspection.excludedGlass = inspection.classifier.nameLooksGlass;
    inspection.excludedDecal = inspection.classifier.sortIsDecal || inspection.classifier.polygonOffsetDecal || inspection.classifier.nameLooksDecal;
    inspection.excludedPostOrSubview = inspection.classifier.sortIsPostProcess || inspection.classifier.sortIsGuiOrSubview;
    inspection.excludedScreenTexgen = inspection.classifier.hasScreenTexgen;

    bool hasActiveNonCompositeStage = false;
    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || !ParticleAuditStageActive(material, drawSurf, stage))
        {
            continue;
        }
        ParticleAuditStage auditStage = ParticleAuditInspectStage(drawSurf, stageIndex);
        if (ParticleAuditStageSupported(auditStage))
        {
            ++inspection.supportedStageCount;
        }
        else
        {
            hasActiveNonCompositeStage = true;
        }
        inspection.activeStages.push_back(auditStage);
    }

    int acceptedStageCount = 0;
    inspection.accepted = ParticleCompositeSurfaceAccepted(
        drawSurf,
        inspection.classifier,
        inspection.activeStages,
        acceptedStageCount);
    inspection.cardOnly = inspection.accepted && !hasActiveNonCompositeStage;
    return inspection;
}

std::vector<ParticleCompositeSurfaceInspection> ParticleCompositeInspectSurfaces(const viewDef_t* viewDef)
{
    std::vector<ParticleCompositeSurfaceInspection> inspections;
    if (!viewDef || !viewDef->drawSurfs || viewDef->isSubview)
    {
        return inspections;
    }

    inspections.resize(viewDef->numDrawSurfs);
    for (int surfaceIndex = 0; surfaceIndex < viewDef->numDrawSurfs; ++surfaceIndex)
    {
        const drawSurf_t* drawSurf = viewDef->drawSurfs[surfaceIndex];
        inspections[surfaceIndex] = ParticleCompositeInspectSurface(
            drawSurf,
            drawSurf ? drawSurf->frontEndGeo : nullptr,
            surfaceIndex);
    }
    return inspections;
}

RtPathTraceParticleBlendClass ParticleCaptureBlendClass(ParticleAuditBlendClass blendClass)
{
    switch (blendClass)
    {
        case ParticleAuditBlendClass::AlphaEmissive:
            return RtPathTraceParticleBlendClass::AlphaEmissive;
        case ParticleAuditBlendClass::PureAdditiveEmissive:
            return RtPathTraceParticleBlendClass::PureAdditiveEmissive;
        case ParticleAuditBlendClass::MultiplicativeDarken:
            return RtPathTraceParticleBlendClass::MultiplicativeDarken;
        default:
            return RtPathTraceParticleBlendClass::AlphaLit;
    }
}

bool ParticleCaptureIsSmokepuffBlackKey(const idMaterial* material)
{
    // The smokepuff family is authored as additive black-background imagery,
    // but semantically represents environmental smoke/steam rather than emitted
    // light. Match the material identity, not the stage image: muzzle-flash
    // materials can deliberately reuse smokepuff imagery and must stay emissive.
    return material && idStr::FindText(material->GetName(), "smokepuff", false) >= 0;
}

bool ParticleCaptureIsAlphaLit(RtPathTraceParticleBlendClass blendClass)
{
    return blendClass == RtPathTraceParticleBlendClass::AlphaLit ||
        blendClass == RtPathTraceParticleBlendClass::AlphaLitBlackKey;
}

const char* ParticleCaptureDepthPolicyName(RtPathTraceParticleDepthPolicy policy)
{
    switch (policy)
    {
        case RtPathTraceParticleDepthPolicy::WeaponProjection: return "weapon-projection";
        case RtPathTraceParticleDepthPolicy::ModelProjection: return "model-projection";
        case RtPathTraceParticleDepthPolicy::WorldMuzzleNearPlane: return "world-muzzle-near-plane";
        default: return "world";
    }
}

const char* ParticleCaptureSourceClassName(RtPathTraceParticleSourceClass sourceClass)
{
    switch (sourceClass)
    {
        case RtPathTraceParticleSourceClass::LocalWeapon: return "local-weapon";
        case RtPathTraceParticleSourceClass::AttachedWeaponEmitter: return "attached-weapon-emitter";
        case RtPathTraceParticleSourceClass::ProjectileTrail: return "projectile-trail";
        case RtPathTraceParticleSourceClass::Impact: return "impact";
        case RtPathTraceParticleSourceClass::Unknown: return "unknown";
        default: return "world";
    }
}

float ParticleCaptureRegisterValue(const idMaterial* material, const float* registers, int registerIndex, float fallback)
{
    return material && registers && registerIndex >= 0 && registerIndex < material->GetNumRegisters()
        ? registers[registerIndex]
        : fallback;
}

uint32_t ParticleCaptureTextureIndex(RtPathTraceParticleCapture& capture, const idImage* image)
{
    for (uint32_t textureIndex = 0; textureIndex < static_cast<uint32_t>(capture.textures.size()); ++textureIndex)
    {
        if (capture.textures[textureIndex] == image)
        {
            return textureIndex;
        }
    }
    capture.textures.push_back(image);
    return static_cast<uint32_t>(capture.textures.size() - 1);
}

uint32_t ParticleCapturePackColor(const PathTraceSmokeVertex& vertex, const shaderStage_t* stage, const idVec4& stageColor)
{
    idVec4 color(1.0f, 1.0f, 1.0f, 1.0f);
    if (stage && stage->vertexColor == SVC_MODULATE)
    {
        color.Set(vertex.color[0], vertex.color[1], vertex.color[2], vertex.color[3]);
    }
    else if (stage && stage->vertexColor == SVC_INVERSE_MODULATE)
    {
        color.Set(
            1.0f - vertex.color[0],
            1.0f - vertex.color[1],
            1.0f - vertex.color[2],
            1.0f - vertex.color[3]);
    }
    for (int component = 0; component < 4; ++component)
    {
        color[component] = idMath::ClampFloat(0.0f, 1.0f, color[component] * stageColor[component]);
    }
    return PackColor(color);
}

uint32_t ParticleCaptureMetadata(const PathTraceSmokeVertex& vertex)
{
    uint32_t metadata = 0u;
    for (int component = 0; component < 4; ++component)
    {
        const uint32_t byteValue = static_cast<uint32_t>(idMath::ClampInt(
            0,
            255,
            idMath::Ftoi(vertex.color2[component] * 255.0f + 0.5f)));
        metadata |= byteValue << (component * 8);
    }
    return IsRtPathTraceParticleMetadata(metadata) ? metadata : 0u;
}

int ParticleCaptureUniqueVertexIndexes(
    const RtPathTraceParticleCapture& capture,
    uint32_t firstIndex,
    uint32_t indexCount,
    uint32_t* uniqueVertexIndexes)
{
    int uniqueVertexCount = 0;
    for (uint32_t corner = 0; corner < indexCount; ++corner)
    {
        const uint32_t vertexIndex = capture.indexes[firstIndex + corner];
        bool found = false;
        for (int uniqueIndex = 0; uniqueIndex < uniqueVertexCount; ++uniqueIndex)
        {
            found |= uniqueVertexIndexes[uniqueIndex] == vertexIndex;
        }
        if (!found)
        {
            uniqueVertexIndexes[uniqueVertexCount++] = vertexIndex;
        }
    }
    return uniqueVertexCount;
}

void ParticleCaptureAppendPrimitives(
    const viewDef_t* viewDef,
    RtPathTraceParticleCapture& capture,
    uint32_t batchIndex,
    const ParticleCompositeBatch& batch)
{
    for (uint32_t localFirstIndex = 0; localFirstIndex + 2 < batch.indexCount;)
    {
        uint32_t uniqueVertexIndexes[6] = {};
        uint32_t primitiveIndexCount = 3;
        int uniqueVertexCount = ParticleCaptureUniqueVertexIndexes(
            capture,
            batch.firstIndex + localFirstIndex,
            primitiveIndexCount,
            uniqueVertexIndexes);
        if (localFirstIndex + 5 < batch.indexCount)
        {
            uint32_t quadVertexIndexes[6] = {};
            const int quadVertexCount = ParticleCaptureUniqueVertexIndexes(
                capture,
                batch.firstIndex + localFirstIndex,
                6,
                quadVertexIndexes);
            if (quadVertexCount == 4)
            {
                primitiveIndexCount = 6;
                uniqueVertexCount = quadVertexCount;
                for (int uniqueIndex = 0; uniqueIndex < uniqueVertexCount; ++uniqueIndex)
                {
                    uniqueVertexIndexes[uniqueIndex] = quadVertexIndexes[uniqueIndex];
                }
            }
        }
        if (uniqueVertexCount < 3)
        {
            localFirstIndex += primitiveIndexCount;
            continue;
        }

        idVec3 center = vec3_origin;
        for (int uniqueIndex = 0; uniqueIndex < uniqueVertexCount; ++uniqueIndex)
        {
            const ParticleCompositeVertex& vertex = capture.vertices[uniqueVertexIndexes[uniqueIndex]];
            center += idVec3(vertex.worldPosition[0], vertex.worldPosition[1], vertex.worldPosition[2]);
        }
        center /= static_cast<float>(uniqueVertexCount);

        const float viewDepth = viewDef ? (center - viewDef->renderView.vieworg) * viewDef->renderView.viewaxis[0] : 0.0f;
        ParticleCompositePrimitive primitive;
        primitive.centerWorld[0] = center.x;
        primitive.centerWorld[1] = center.y;
        primitive.centerWorld[2] = center.z;
        primitive.batchIndex = batchIndex;
        primitive.firstIndex = batch.firstIndex + localFirstIndex;
        primitive.indexCount = primitiveIndexCount;
        primitive.viewDepth = viewDepth;
        const uint32_t metadata = capture.vertices[uniqueVertexIndexes[0]].particleMetadata;
        if (IsRtPathTraceParticleMetadata(metadata))
        {
            primitive.stableParticleId = RtPathTraceParticleStableId(metadata);
            primitive.sourceClass = RtPathTraceParticleMetadataSource(metadata);
            primitive.depthPolicy = RtPathTraceParticleMetadataDepth(metadata);
        }

        if (ParticleCaptureIsAlphaLit(batch.blendClass))
        {
            ParticleCompositeLightingTask lightingTask;
            lightingTask.centerWorld[0] = center.x;
            lightingTask.centerWorld[1] = center.y;
            lightingTask.centerWorld[2] = center.z;
            lightingTask.stableParticleId = primitive.stableParticleId;
            const uint32_t lightingTaskIndex = static_cast<uint32_t>(capture.lightingTasks.size());
            capture.lightingTasks.push_back(lightingTask);
            for (int uniqueIndex = 0; uniqueIndex < uniqueVertexCount; ++uniqueIndex)
            {
                capture.vertices[uniqueVertexIndexes[uniqueIndex]].lightingTaskIndex = lightingTaskIndex;
            }
            ++capture.stats.alphaLitLightingTasks;
        }
        capture.primitives.push_back(primitive);

        if (primitiveIndexCount == 6)
        {
            ParticleCompositeQuad quad;
            quad.centerWorld[0] = center.x;
            quad.centerWorld[1] = center.y;
            quad.centerWorld[2] = center.z;
            quad.batchIndex = batchIndex;
            quad.firstIndex = primitive.firstIndex;
            quad.viewDepth = viewDepth;
            quad.stableParticleId = primitive.stableParticleId;
            quad.sourceClass = primitive.sourceClass;
            quad.depthPolicy = primitive.depthPolicy;
            capture.quads.push_back(quad);
            ++capture.stats.capturedDrawQuads;
            if (quad.stableParticleId != 0u)
            {
                ++capture.stats.provenanceQuads;
                capture.stats.localWeaponQuads += quad.sourceClass == RtPathTraceParticleSourceClass::LocalWeapon ? 1 : 0;
                capture.stats.attachedWeaponEmitterQuads += quad.sourceClass == RtPathTraceParticleSourceClass::AttachedWeaponEmitter ? 1 : 0;
                capture.stats.projectileTrailQuads += quad.sourceClass == RtPathTraceParticleSourceClass::ProjectileTrail ? 1 : 0;
                capture.stats.impactQuads += quad.sourceClass == RtPathTraceParticleSourceClass::Impact ? 1 : 0;
                capture.stats.worldMuzzleNearPlaneQuads += quad.depthPolicy == RtPathTraceParticleDepthPolicy::WorldMuzzleNearPlane ? 1 : 0;
            }
        }
        else
        {
            ++capture.stats.capturedTrianglePrimitives;
        }
        localFirstIndex += primitiveIndexCount;
    }
}

bool ParticleCaptureAppendSurface(
    const viewDef_t* viewDef,
    const drawSurf_t* drawSurf,
    const srfTriangles_t* tri,
    int surfaceIndex,
    const std::vector<ParticleAuditStage>& activeStages,
    int supportedStageCount,
    RtPathTraceParticleCapture& capture)
{
    std::vector<PathTraceSmokeVertex> baseVertices;
    std::vector<uint32_t> baseIndexes;
    std::vector<uint32_t> baseClasses;
    std::vector<uint32_t> baseMaterials;
    RtSmokeSurfaceSkipStats skipStats;
    RtSmokeAttributeStats attributeStats;
    const uint32_t materialId = SmokeMaterialId(drawSurf->material);
    const int emittedIndexes = AppendSmokeSurfaceGeometry(
        drawSurf,
        tri,
        SmokeSurfaceClassId(RtSmokeSurfaceClass::ParticleAlpha),
        materialId,
        RT_SMOKE_CLASS_COUNT,
        RT_SMOKE_TRIANGLE_CLASS_MASK,
        SmokeSurfaceClassId(RtSmokeSurfaceClass::ParticleAlpha),
        RT_SMOKE_TRIANGLE_FORCE_GEOMETRIC_NORMAL,
        baseVertices,
        baseIndexes,
        baseClasses,
        baseMaterials,
        skipStats,
        attributeStats);
    if (emittedIndexes <= 0 || baseVertices.empty() || baseIndexes.empty())
    {
        ++capture.stats.droppedGeometrySurfaces;
        return false;
    }
    const idMaterial* material = drawSurf->material;
    const float* registers = drawSurf->shaderRegisters ? drawSurf->shaderRegisters : material->ConstantRegisters();
    const viewEntity_t* space = drawSurf->space;
    const idRenderEntityLocal* entity = space ? space->entityDef : nullptr;
    const renderEntity_t* renderEntity = entity ? &entity->parms : nullptr;
    const bool weaponDepthHack = space && space->weaponDepthHack;
    const float modelDepthHack = space ? space->modelDepthHack : 0.0f;
    const RtSmokeSurfaceClass currentClass = ClassifySmokeSurface(viewDef, drawSurf, tri);
    const srfTriangles_t* validatedTri = nullptr;
    const bool currentParticleAlphaBvh =
        ValidateSmokeDrawSurface(viewDef, drawSurf, validatedTri, nullptr) &&
        currentClass == RtSmokeSurfaceClass::ParticleAlpha;

    bool appendedBatch = false;
    for (const ParticleAuditStage& auditStage : activeStages)
    {
        if (!ParticleAuditStageSupported(auditStage))
        {
            continue;
        }
        const shaderStage_t* stage = material->GetStage(auditStage.stageIndex);
        if (!stage || !stage->texture.image)
        {
            continue;
        }

        float textureMatrix[2][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
        if (stage->texture.hasMatrix)
        {
            for (int row = 0; row < 2; ++row)
            {
                for (int column = 0; column < 3; ++column)
                {
                    textureMatrix[row][column] = ParticleCaptureRegisterValue(
                        material,
                        registers,
                        stage->texture.matrix[row][column],
                        row == column ? 1.0f : 0.0f);
                }
            }
        }
        idVec4 stageColor;
        for (int component = 0; component < 4; ++component)
        {
            stageColor[component] = ParticleCaptureRegisterValue(material, registers, stage->color.registers[component], 1.0f);
        }

        ParticleCompositeBatch batch;
        batch.textureIndex = ParticleCaptureTextureIndex(capture, stage->texture.image);
        batch.firstVertex = static_cast<uint32_t>(capture.vertices.size());
        batch.vertexCount = static_cast<uint32_t>(baseVertices.size());
        batch.firstIndex = static_cast<uint32_t>(capture.indexes.size());
        batch.indexCount = static_cast<uint32_t>(baseIndexes.size());
        batch.blendClass = ParticleCaptureBlendClass(auditStage.blendClass);
        const bool additiveSmokeStage =
            batch.blendClass == RtPathTraceParticleBlendClass::AlphaEmissive ||
            batch.blendClass == RtPathTraceParticleBlendClass::PureAdditiveEmissive;
        if (additiveSmokeStage && ParticleCaptureIsSmokepuffBlackKey(material))
        {
            batch.blendClass = RtPathTraceParticleBlendClass::AlphaLitBlackKey;
        }
        batch.depthPolicy = weaponDepthHack
            ? RtPathTraceParticleDepthPolicy::WeaponProjection
            : (modelDepthHack != 0.0f ? RtPathTraceParticleDepthPolicy::ModelProjection : RtPathTraceParticleDepthPolicy::World);
        batch.sourceClass = weaponDepthHack
            ? RtPathTraceParticleSourceClass::LocalWeapon
            : RtPathTraceParticleSourceClass::World;
        batch.sourceEntityId = entity ? entity->index : -1;
        batch.allowSurfaceInViewId = renderEntity ? renderEntity->allowSurfaceInViewID : 0;
        batch.modelDepthHack = modelDepthHack;
        batch.emissiveScale = Max(0.0f, r_pathTracingParticleEmissiveScale.GetFloat());
        batch.softDepth = Max(0.0f, r_pathTracingParticleSoftDepth.GetFloat());
        batch.materialId = materialId;
        batch.surfaceIndex = surfaceIndex;
        batch.stageIndex = auditStage.stageIndex;
        batch.flags = RT_PATH_TRACE_PARTICLE_BATCH_STABLE_ID_UNAVAILABLE;
        batch.flags |= stage->texture.hasMatrix ? RT_PATH_TRACE_PARTICLE_BATCH_TEXTURE_MATRIX : 0u;
        batch.flags |= stage->vertexColor == SVC_INVERSE_MODULATE ? RT_PATH_TRACE_PARTICLE_BATCH_INVERSE_VERTEX_COLOR : 0u;
        batch.flags |= currentParticleAlphaBvh ? RT_PATH_TRACE_PARTICLE_BATCH_CURRENT_PARTICLE_ALPHA_BVH : 0u;
        batch.flags |= supportedStageCount > 1 ? RT_PATH_TRACE_PARTICLE_BATCH_AMBIGUOUS_MATERIAL : 0u;
        batch.flags |= weaponDepthHack ? RT_PATH_TRACE_PARTICLE_BATCH_WEAPON_DEPTH_HACK : 0u;
        batch.flags |= modelDepthHack != 0.0f ? RT_PATH_TRACE_PARTICLE_BATCH_MODEL_DEPTH_HACK : 0u;

        for (const PathTraceSmokeVertex& baseVertex : baseVertices)
        {
            ParticleCompositeVertex vertex;
            vertex.worldPosition[0] = baseVertex.position[0];
            vertex.worldPosition[1] = baseVertex.position[1];
            vertex.worldPosition[2] = baseVertex.position[2];
            vertex.texCoord[0] = textureMatrix[0][0] * baseVertex.texCoord[0] + textureMatrix[0][1] * baseVertex.texCoord[1] + textureMatrix[0][2];
            vertex.texCoord[1] = textureMatrix[1][0] * baseVertex.texCoord[0] + textureMatrix[1][1] * baseVertex.texCoord[1] + textureMatrix[1][2];
            vertex.packedColor = ParticleCapturePackColor(baseVertex, stage, stageColor);
            vertex.particleMetadata = ParticleCaptureMetadata(baseVertex);
            capture.vertices.push_back(vertex);
        }
        for (uint32_t baseIndex : baseIndexes)
        {
            capture.indexes.push_back(batch.firstVertex + baseIndex);
        }

        const uint32_t batchIndex = static_cast<uint32_t>(capture.batches.size());
        capture.batches.push_back(batch);
        ParticleCaptureAppendPrimitives(viewDef, capture, batchIndex, capture.batches.back());
        ++capture.stats.capturedBatches;
        capture.stats.alphaLitBatches += ParticleCaptureIsAlphaLit(batch.blendClass) ? 1 : 0;
        capture.stats.alphaEmissiveBatches += batch.blendClass == RtPathTraceParticleBlendClass::AlphaEmissive ? 1 : 0;
        capture.stats.pureAdditiveBatches += batch.blendClass == RtPathTraceParticleBlendClass::PureAdditiveEmissive ? 1 : 0;
        capture.stats.multiplicativeDarkenBatches += batch.blendClass == RtPathTraceParticleBlendClass::MultiplicativeDarken ? 1 : 0;
        appendedBatch = true;
    }
    if (appendedBatch)
    {
        ++capture.stats.capturedSurfaces;
    }
    return appendedBatch;
}

void CapturePathTraceParticleCompositeRecords(
    const viewDef_t* viewDef,
    const std::vector<ParticleCompositeSurfaceInspection>& inspections,
    RtPathTraceParticleCapture& capture)
{
    for (const ParticleCompositeSurfaceInspection& inspection : inspections)
    {
        if (!inspection.accepted)
        {
            continue;
        }
        ++capture.stats.candidateSurfaces;
        capture.stats.candidateTriangles += inspection.drawSurf->numIndexes / 3;
        capture.stats.candidateQuads += (inspection.drawSurf->numIndexes % 6) == 0 ? inspection.drawSurf->numIndexes / 6 : 0;
        capture.stats.cardOnlySurfaces += inspection.cardOnly ? 1 : 0;
        capture.stats.mixedStageSurfaces += inspection.cardOnly ? 0 : 1;
        const bool compositeOnly = capture.enabled && inspection.cardOnly;
        capture.stats.routedCompositeOnlySurfaces += compositeOnly ? 1 : 0;
        capture.stats.retainedBvhCandidateSurfaces += compositeOnly ? 0 : 1;
        if (capture.enabled && !inspection.cardOnly)
        {
            continue;
        }
        ParticleCaptureAppendSurface(
            viewDef,
            inspection.drawSurf,
            inspection.tri,
            inspection.surfaceIndex,
            inspection.activeStages,
            inspection.supportedStageCount,
            capture);
    }
}

}

static void AuditPathTraceParticleCompositeCandidates(
    const viewDef_t* viewDef,
    const std::vector<ParticleCompositeSurfaceInspection>& inspections,
    const RtPathTraceParticleCapture& capture)
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

    for (const ParticleCompositeSurfaceInspection& inspection : inspections)
    {
        const int surfaceIndex = inspection.surfaceIndex;
        const drawSurf_t* drawSurf = inspection.drawSurf;
        const idMaterial* material = drawSurf ? drawSurf->material : nullptr;
        const srfTriangles_t* tri = inspection.tri;
        if (!inspection.valid)
        {
            continue;
        }
        ++validSurfaces;

        const deform_t deform = material->Deform();
        const RtSmokeTranslucentClassifierInfo& classifier = inspection.classifier;
        const bool transientMaterialTrait = inspection.transientMaterialTrait;
        const std::vector<ParticleAuditStage>& activeStages = inspection.activeStages;
        const int supportedStages = inspection.supportedStageCount;

        if (!transientMaterialTrait)
        {
            if (supportedStages > 0)
            {
                ++blendOnlySurfaces;
            }
            continue;
        }
        ++effectTraitSurfaces;

        if (inspection.excludedGui)
        {
            ++excludedGui;
            continue;
        }
        if (inspection.excludedGlass)
        {
            ++excludedGlass;
            continue;
        }
        if (inspection.excludedDecal)
        {
            ++excludedDecal;
            continue;
        }
        if (inspection.excludedPostOrSubview)
        {
            ++excludedPostOrSubview;
            continue;
        }
        if (inspection.excludedScreenTexgen)
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
    common->Printf("PathTraceParticleAudit: capture enabled=%d debugTint=%d candidates=%d/%d quads=%d/%d triangles=%d/%d ownership=card-only:%d,mixed-stage:%d route=composite-only:%d,bvh-retained:%d capturedSurfaces=%d batches=%d drawQuads=%d trianglePrimitives=%d vertices=%d indexes=%d textures=%d droppedGeometry=%d droppedNonQuad=%d parity=%s\n",
        capture.enabled ? 1 : 0,
        capture.debugTint ? 1 : 0,
        capture.stats.candidateSurfaces,
        candidateSurfaces,
        capture.stats.candidateQuads,
        candidateQuads,
        capture.stats.candidateTriangles,
        candidateTriangles,
        capture.stats.cardOnlySurfaces,
        capture.stats.mixedStageSurfaces,
        capture.stats.routedCompositeOnlySurfaces,
        capture.stats.retainedBvhCandidateSurfaces,
        capture.stats.capturedSurfaces,
        capture.stats.capturedBatches,
        capture.stats.capturedDrawQuads,
        capture.stats.capturedTrianglePrimitives,
        static_cast<int>(capture.vertices.size()),
        static_cast<int>(capture.indexes.size()),
        static_cast<int>(capture.textures.size()),
        capture.stats.droppedGeometrySurfaces,
        capture.stats.droppedNonQuadSurfaces,
        capture.stats.candidateSurfaces == candidateSurfaces &&
            capture.stats.candidateQuads == candidateQuads &&
            capture.stats.candidateTriangles == candidateTriangles ? "match" : "MISMATCH");
    common->Printf("PathTraceParticleAudit: capture blends alphaLit=%d alphaEmissive=%d pureAdditive=%d multiplicativeDarken=%d lighting=%d tasks=%d candidates=%d ambient=%.3f emissiveScale=%.3f softDepth=%.3f shadowRays=%d sortMode=%d\n",
        capture.stats.alphaLitBatches,
        capture.stats.alphaEmissiveBatches,
        capture.stats.pureAdditiveBatches,
        capture.stats.multiplicativeDarkenBatches,
        r_pathTracingParticleLighting.GetBool() ? 1 : 0,
        capture.stats.alphaLitLightingTasks,
        idMath::ClampInt(1, 4096, r_pathTracingParticleLightCandidates.GetInteger()),
        Max(0.0f, r_pathTracingParticleAmbient.GetFloat()),
        Max(0.0f, r_pathTracingParticleEmissiveScale.GetFloat()),
        Max(0.0f, r_pathTracingParticleSoftDepth.GetFloat()),
        Max(0, r_pathTracingParticleShadowRays.GetInteger()),
        idMath::ClampInt(0, 1, r_pathTracingParticleSortMode.GetInteger()));
    common->Printf("PathTraceParticleAudit: provenance quads=%d source(localWeapon/attachedWeapon/projectileTrail/impact)=%d/%d/%d/%d worldMuzzleNearPlane=%d\n",
        capture.stats.provenanceQuads,
        capture.stats.localWeaponQuads,
        capture.stats.attachedWeaponEmitterQuads,
        capture.stats.projectileTrailQuads,
        capture.stats.impactQuads,
        capture.stats.worldMuzzleNearPlaneQuads);
    if (dumpMode > 1)
    {
        for (int batchIndex = 0; batchIndex < static_cast<int>(capture.batches.size()); ++batchIndex)
        {
            const ParticleCompositeBatch& batch = capture.batches[batchIndex];
            const idImage* image = batch.textureIndex < capture.textures.size() ? capture.textures[batch.textureIndex] : nullptr;
            common->Printf("PathTraceParticleAudit: capture batch=%d surface=%d stage=%d materialId=%u texture=%u('%s') blend=%u depth=%s source=%s entity=%d allowViewID=%d modelDepthHack=%.3f firstVertex=%u vertices=%u firstIndex=%u indexes=%u flags=0x%08x\n",
                batchIndex,
                batch.surfaceIndex,
                batch.stageIndex,
                batch.materialId,
                batch.textureIndex,
                image ? image->GetName() : "<none>",
                static_cast<uint32_t>(batch.blendClass),
                ParticleCaptureDepthPolicyName(batch.depthPolicy),
                ParticleCaptureSourceClassName(batch.sourceClass),
                batch.sourceEntityId,
                batch.allowSurfaceInViewId,
                batch.modelDepthHack,
                batch.firstVertex,
                batch.vertexCount,
                batch.firstIndex,
                batch.indexCount,
                batch.flags);
        }
    }
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

void RtPathTraceParticleCapture::Clear()
{
    vertices.clear();
    indexes.clear();
    batches.clear();
    quads.clear();
    primitives.clear();
    lightingTasks.clear();
    textures.clear();
    stats = RtPathTraceParticleCaptureStats();
    enabled = false;
    debugTint = false;
}

RtPathTraceParticleSurfaceRoute PathTraceParticleCompositeSurfaceRoute(
    const drawSurf_t* drawSurf,
    const srfTriangles_t* tri)
{
    if (r_pathTracingParticleComposite.GetInteger() == 0)
    {
        return RtPathTraceParticleSurfaceRoute::LegacyBvh;
    }
    const ParticleCompositeSurfaceInspection inspection = ParticleCompositeInspectSurface(drawSurf, tri, -1);
    return inspection.cardOnly
        ? RtPathTraceParticleSurfaceRoute::CompositeOnly
        : RtPathTraceParticleSurfaceRoute::LegacyBvh;
}

void BuildPathTraceParticleCompositeCapture(const viewDef_t* viewDef, RtPathTraceParticleCapture& capture)
{
    capture.Clear();
    const int compositeMode = idMath::ClampInt(0, 2, r_pathTracingParticleComposite.GetInteger());
    const bool dumpRequested = r_pathTracingParticleDump.GetInteger() != 0;
    capture.enabled = compositeMode != 0;
    capture.debugTint = compositeMode == 2;
    const std::vector<ParticleCompositeSurfaceInspection> inspections = ParticleCompositeInspectSurfaces(viewDef);
    if (capture.enabled || dumpRequested)
    {
        CapturePathTraceParticleCompositeRecords(viewDef, inspections, capture);
    }
    AuditPathTraceParticleCompositeCandidates(viewDef, inspections, capture);
}
