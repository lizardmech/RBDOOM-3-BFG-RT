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

#include <unordered_map>
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
    const viewDef_t* viewDef,
    const drawSurf_t* drawSurf,
    const srfTriangles_t* tri,
    int surfaceIndex)
{
    ParticleCompositeSurfaceInspection inspection;
    inspection.surfaceIndex = surfaceIndex;
    inspection.drawSurf = drawSurf;
    const srfTriangles_t* validatedTri = nullptr;
    if (!ValidateSmokeDrawSurface(viewDef, drawSurf, validatedTri, nullptr) ||
        validatedTri != tri)
    {
        return inspection;
    }

    inspection.tri = validatedTri;
    const idMaterial* material = drawSurf->material;
    inspection.valid = true;
    inspection.classifier = BuildSmokeTranslucentClassifierInfo(material);
    inspection.supportedDeform = ParticleAuditSupportedDeform(material->Deform());
        inspection.transientMaterialTrait =
            inspection.supportedDeform ||
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
            viewDef,
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

bool ParticleCaptureIsPfireSmall(const idMaterial* material)
{
    if (!material)
    {
        return false;
    }
    const char* materialName = material->GetName();
    return idStr::Icmp(materialName, "textures/particles/pfiresmall") == 0 ||
        idStr::Icmp(materialName, "textures/particles/pfiresmall2") == 0;
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
    std::unordered_map<uint32_t, uint32_t> stablePrimitiveCounts;
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

        uint32_t stablePrimitiveIndex = 0u;
        if (primitive.stableParticleId != 0u)
        {
            stablePrimitiveIndex = stablePrimitiveCounts[primitive.stableParticleId]++;
        }

        if (ParticleCaptureIsAlphaLit(batch.blendClass))
        {
            ParticleCompositeLightingTask lightingTask;
            lightingTask.centerWorld[0] = center.x;
            lightingTask.centerWorld[1] = center.y;
            lightingTask.centerWorld[2] = center.z;
            lightingTask.stableParticleId = primitive.stableParticleId;
            lightingTask.stablePrimitiveIndex = stablePrimitiveIndex;
            lightingTask.materialId = batch.materialId;
            lightingTask.compatibility =
                (static_cast<uint32_t>(batch.blendClass) & 0xffu) |
                ((static_cast<uint32_t>(primitive.sourceClass) & 0xffu) << 8u) |
                ((static_cast<uint32_t>(primitive.depthPolicy) & 0xffu) << 16u);
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
    const srfTriangles_t* validatedTri = nullptr;
    if (!ValidateSmokeDrawSurface(viewDef, drawSurf, validatedTri, &skipStats) ||
        validatedTri != tri)
    {
        ++capture.stats.droppedGeometrySurfaces;
        return false;
    }
    tri = validatedTri;
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
    const bool currentParticleAlphaBvh = currentClass == RtSmokeSurfaceClass::ParticleAlpha;

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
        batch.material = material;
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
        batch.emissiveScale = Max(0.0f, r_pathTracingParticleEmissiveScale.GetFloat()) *
            (ParticleCaptureIsPfireSmall(material)
                ? Max(0.0f, r_pathTracingParticleFireEmissiveScale.GetFloat())
                : 1.0f);
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
        batch.flags |= material->Deform() == DFRM_FLARE ? RT_PATH_TRACE_PARTICLE_BATCH_FLARE_DEFORM : 0u;

        bool batchHasStableParticleIds = false;
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
            batchHasStableParticleIds |= RtPathTraceParticleStableId(vertex.particleMetadata) != 0u;
            capture.vertices.push_back(vertex);
        }
        if (batchHasStableParticleIds)
        {
            batch.flags &= ~RT_PATH_TRACE_PARTICLE_BATCH_STABLE_ID_UNAVAILABLE;
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
    const viewDef_t* viewDef,
    const drawSurf_t* drawSurf,
    const srfTriangles_t* tri)
{
    if (r_pathTracingParticleComposite.GetInteger() == 0)
    {
        return RtPathTraceParticleSurfaceRoute::LegacyBvh;
    }
    const ParticleCompositeSurfaceInspection inspection = ParticleCompositeInspectSurface(viewDef, drawSurf, tri, -1);
    return inspection.cardOnly
        ? RtPathTraceParticleSurfaceRoute::CompositeOnly
        : RtPathTraceParticleSurfaceRoute::LegacyBvh;
}

void BuildPathTraceParticleCompositeCapture(const viewDef_t* viewDef, RtPathTraceParticleCapture& capture)
{
    capture.Clear();
    const int compositeMode = idMath::ClampInt(0, 2, r_pathTracingParticleComposite.GetInteger());
    capture.enabled = compositeMode != 0;
    capture.debugTint = compositeMode == 2;
    const std::vector<ParticleCompositeSurfaceInspection> inspections = ParticleCompositeInspectSurfaces(viewDef);
    if (capture.enabled)
    {
        CapturePathTraceParticleCompositeRecords(viewDef, inspections, capture);
    }
}
