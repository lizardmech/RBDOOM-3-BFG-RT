#include "precompiled.h"
#pragma hdrstop
#include "PathTraceCpuProducerApplyGate.h"

#include "PathTraceAcceleration.h"
#include "PathTraceAccelerationPlan.h"
#include "PathTraceCaptureDeriveRing.h"
#include "PathTraceCaptureProduct.h"
#include "PathTraceCommittedCapture.h"
#include "PathTraceCVars.h"
#include "PathTraceDrawSurfCapture.h"
#include "PathTraceDoomMaterialClassifier.h"
#include "PathTraceDynamicMaterialState.h"
#include "PathTraceGeometryLifecycle.h"
#include "PathTraceGeometryUniverse.h"
#include "PathTraceGuiSurfaces.h"
#include "PathTraceParticleCapture.h"
#include "PathTraceDebugModes.h"
#include "PathTraceRigidIdentity.h"
#include "PathTraceR1Ledger.h"
#include "PathTraceSceneCapture.h"
#include "PathTraceSceneUniverse.h"
#include "PathTraceSkinnedHitRoute.h"
#include "PathTraceSkinning.h"
#include "PathTraceSurfaceClassification.h"
#include "PathTraceTextureRegistry.h"
#include "../RenderCommon.h"

#include <algorithm>
#include <new>
#include <unordered_set>

namespace {

void AddSmokeSurfaceSkipStats(RtSmokeSurfaceSkipStats& dst, const RtSmokeSurfaceSkipStats& src)
{
    dst.nullSurface += src.nullSurface;
    dst.missingGeometry += src.missingGeometry;
    dst.nullMaterial += src.nullMaterial;
    dst.nullSpace += src.nullSpace;
    dst.nullModel += src.nullModel;
    dst.invalidIndexCount += src.invalidIndexCount;
    dst.conditionedOff += src.conditionedOff;
    dst.alphaClipDiagnostic += src.alphaClipDiagnostic;
    dst.nonCurrentCache += src.nonCurrentCache;
    dst.limitExceeded += src.limitExceeded;
    dst.geometrySurfaceBudgetExceeded +=
        src.geometrySurfaceBudgetExceeded;
    dst.geometryByteBudgetExceeded +=
        src.geometryByteBudgetExceeded;
    dst.geometryAdmissionInvalid += src.geometryAdmissionInvalid;
    dst.geometryAdmissionOverflow += src.geometryAdmissionOverflow;
    dst.geometryAdmittedBytes += src.geometryAdmittedBytes;
    dst.geometryRejectedBytes += src.geometryRejectedBytes;
    dst.geometryAdmittedSurfaces += src.geometryAdmittedSurfaces;
    dst.geometryStaticSurfaceBudgetExceeded +=
        src.geometryStaticSurfaceBudgetExceeded;
    dst.geometryStaticByteBudgetExceeded +=
        src.geometryStaticByteBudgetExceeded;
    dst.geometryStaticAdmissionInvalid +=
        src.geometryStaticAdmissionInvalid;
    dst.geometryStaticAdmissionOverflow +=
        src.geometryStaticAdmissionOverflow;
    dst.geometryStaticAdmittedBytes +=
        src.geometryStaticAdmittedBytes;
    dst.geometryStaticRejectedBytes +=
        src.geometryStaticRejectedBytes;
    dst.geometryStaticAdmittedSurfaces +=
        src.geometryStaticAdmittedSurfaces;
    dst.zeroAreaOnly += src.zeroAreaOnly;
    dst.emptyClassBuffer += src.emptyClassBuffer;
    dst.guiSurface += src.guiSurface;
    dst.callbackEntity += src.callbackEntity;
}

uint32_t PtDynamicTriangleIdentitySeed(const drawSurf_t* drawSurf, const srfTriangles_t* tri, uint32_t materialId, uint32_t localTriangleIndex)
{
    const idRenderEntityLocal* entity = (drawSurf && drawSurf->space) ? drawSurf->space->entityDef : nullptr;
    const renderEntity_t* renderEntity = entity ? &entity->parms : nullptr;
    const int entityIndex = entity ? entity->index : -1;
    const int renderEntityNum = renderEntity ? renderEntity->entityNum : -1;
    const uintptr_t triPtr = reinterpret_cast<uintptr_t>(tri);
    uint64 hash = 14695981039346656037ull;
    hash = HashSmokeBytes(hash, &entityIndex, sizeof(entityIndex));
    hash = HashSmokeBytes(hash, &renderEntityNum, sizeof(renderEntityNum));
    hash = HashSmokeBytes(hash, &materialId, sizeof(materialId));
    hash = HashSmokeBytes(hash, &triPtr, sizeof(triPtr));
    hash = HashSmokeBytes(hash, &localTriangleIndex, sizeof(localTriangleIndex));
    const uint32_t folded = static_cast<uint32_t>(hash) ^ static_cast<uint32_t>(hash >> 32);
    return folded != 0u ? folded : 1u;
}

void CopyDrawSurfObjectToWorld(const drawSurf_t* drawSurf, float objectToWorld[16])
{
    if (drawSurf && drawSurf->space)
    {
        memcpy(objectToWorld, drawSurf->space->modelMatrix, sizeof(float) * 16);
        return;
    }

    memset(objectToWorld, 0, sizeof(float) * 16);
    objectToWorld[0] = 1.0f;
    objectToWorld[5] = 1.0f;
    objectToWorld[10] = 1.0f;
    objectToWorld[15] = 1.0f;
}

void BuildRigidNormalTexMatrix(const idMaterial* material, const float* registers, float matrix[6])
{
    const float identity[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    memcpy(matrix, identity, sizeof(identity));
    if (!material || !registers)
    {
        return;
    }

    const int registerCount = material->GetNumRegisters();
    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || stage->lighting != SL_BUMP || !stage->texture.hasMatrix)
        {
            continue;
        }

        for (int row = 0; row < 2; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                const int registerIndex = stage->texture.matrix[row][column];
                if (registerIndex >= 0 && registerIndex < registerCount)
                {
                    matrix[row * 3 + column] = registers[registerIndex];
                }
            }
        }
        return;
    }
}

uint32_t PtSourceFlagsForDrawSurf(const viewDef_t* viewDef, const drawSurf_t* drawSurf, const srfTriangles_t* tri, RtSmokeSurfaceClass surfaceClass)
{
    const viewEntity_t* space = drawSurf ? drawSurf->space : nullptr;
    const idRenderEntityLocal* entity = space ? space->entityDef : nullptr;
    const renderEntity_t* renderEntity = entity ? &entity->parms : nullptr;
    const idMaterial* material = drawSurf ? drawSurf->material : nullptr;
    RtPathTraceSourceFlagInput input;
    input.surfaceClass = surfaceClass;
    input.guiSurface = IsSmokeGuiDrawSurface(drawSurf);
    input.hasJointCache = drawSurf && drawSurf->jointCache != 0;
    input.hasStaticModelWithJoints = tri && tri->staticModelWithJoints != nullptr;
    input.hasRenderEntityJoints = renderEntity && renderEntity->joints != nullptr &&
        renderEntity->numJoints > 0;
    input.entityCallbackPresent = renderEntity && renderEntity->callback != nullptr;
    input.entityForceUpdate = renderEntity && renderEntity->forceUpdate != 0;
    input.dynamicModelPresent = entity && entity->dynamicModel != nullptr;
    input.cachedDynamicModelPresent = entity && entity->cachedDynamicModel != nullptr;
    input.materialDeformed = material && material->Deform() != DFRM_NONE;
    input.customShaderPresent = renderEntity && renderEntity->customShader != nullptr;
    input.customSkinPresent = renderEntity && renderEntity->customSkin != nullptr;
    return RtPathTraceSourceFlagsFromPod(input);
}

bool PtMirrorCanPromoteRigidEmissiveCard(
    const drawSurf_t* drawSurf,
    const srfTriangles_t* tri,
    RtSmokeSurfaceClass surfaceClass,
    const RtSmokeTranslucentClassifierInfo* classifier = nullptr)
{
    if (r_pathTracingRigidRouteEmissiveCards.GetInteger() == 0 ||
        surfaceClass != RtSmokeSurfaceClass::ParticleAlpha ||
        !drawSurf ||
        !tri ||
        !drawSurf->space ||
        !drawSurf->material)
    {
        return false;
    }

    const viewEntity_t* space = drawSurf->space;
    const idRenderEntityLocal* entity = space->entityDef;
    const renderEntity_t* renderEntity = entity ? &entity->parms : nullptr;
    const idMaterial* material = drawSurf->material;
    if (!entity ||
        IsSmokeGuiDrawSurface(drawSurf) ||
        drawSurf->jointCache != 0 ||
        tri->staticModelWithJoints != nullptr ||
        (renderEntity && renderEntity->joints != nullptr && renderEntity->numJoints > 0) ||
        (renderEntity && (renderEntity->callback != nullptr || renderEntity->forceUpdate != 0)) ||
        (entity->dynamicModel != nullptr || entity->cachedDynamicModel != nullptr) ||
        material->Deform() != DFRM_NONE ||
        space->modelDepthHack != 0.0f)
    {
        return false;
    }

    return classifier
        ? SmokeMaterialCanPromoteRigidEmissiveCard(material, *classifier)
        : SmokeMaterialCanPromoteRigidEmissiveCard(material);
}

bool PtMirrorCanPromoteRigidLiquidPoolCard(const drawSurf_t* drawSurf, const srfTriangles_t* tri, RtSmokeSurfaceClass surfaceClass)
{
    if ((r_pathTracingLiquidPoolMode.GetInteger() == 0 && r_pathTracingLiquidPoolDebug.GetInteger() == 0) ||
        surfaceClass != RtSmokeSurfaceClass::ParticleAlpha ||
        !drawSurf ||
        !tri ||
        !drawSurf->space ||
        !drawSurf->material)
    {
        return false;
    }

    const viewEntity_t* space = drawSurf->space;
    const idRenderEntityLocal* entity = space->entityDef;
    const renderEntity_t* renderEntity = entity ? &entity->parms : nullptr;
    const idMaterial* material = drawSurf->material;
    if (!entity ||
        IsSmokeGuiDrawSurface(drawSurf) ||
        drawSurf->jointCache != 0 ||
        tri->staticModelWithJoints != nullptr ||
        (renderEntity && renderEntity->joints != nullptr && renderEntity->numJoints > 0) ||
        (renderEntity && (renderEntity->callback != nullptr || renderEntity->forceUpdate != 0)) ||
        entity->dynamicModel != nullptr ||
        entity->cachedDynamicModel != nullptr ||
        material->Deform() != DFRM_NONE ||
        space->modelDepthHack != 0.0f)
    {
        return false;
    }

    const uint32_t materialId = SmokeMaterialId(material);
    const RtSmokeMaterialTextureInfo info = ResolveSmokeMaterialTextureInfo(materialId, -1);
    return info.detailDecalLiquidPool;
}

RtSmokeSurfaceClass PtMirrorEffectiveSurfaceClass(
    const drawSurf_t* drawSurf,
    const srfTriangles_t* tri,
    RtSmokeSurfaceClass surfaceClass,
    const RtSmokeTranslucentClassifierInfo* classifier = nullptr)
{
    return (PtMirrorCanPromoteRigidEmissiveCard(
                drawSurf, tri, surfaceClass, classifier) ||
        PtMirrorCanPromoteRigidLiquidPoolCard(drawSurf, tri, surfaceClass))
        ? RtSmokeSurfaceClass::RigidEntity
        : surfaceClass;
}

void BuildSceneUniverseLegacyKeySet(const RtPathTraceSceneUniverse* sceneUniverse, std::unordered_set<uint64>& keys)
{
    keys.clear();
    if (!sceneUniverse || !sceneUniverse->GetStats().valid)
    {
        return;
    }

    const std::vector<RtPathTraceSceneUniverseSurface>& surfaces = sceneUniverse->Surfaces();
    keys.reserve(surfaces.size());
    for (const RtPathTraceSceneUniverseSurface& surface : surfaces)
    {
        if (surface.legacyDrawSurfKey != 0)
        {
            keys.insert(surface.legacyDrawSurfKey);
        }
    }
}

struct RtPathTraceMaterialClassifyParityStats
{
    uint64_t mapped = 0;
    uint64_t compared = 0;
    uint64_t mismatched = 0;
    uint64_t particles = 0;
    uint64_t particleMismatches = 0;
    uint64_t mappingMismatches = 0;
    uint64_t classifierMismatches = 0;
    uint64_t surfaceClassMismatches = 0;
    uint64_t subtypeMismatches = 0;
    uint64_t emissiveMismatches = 0;
    uint64_t signatureMismatches = 0;
    int mismatchOrdinals[4] = { -1, -1, -1, -1 };
    int mismatchSampleCount = 0;
};

void RecordPathTraceMaterialClassifyMappingParity(
    RtPathTraceMaterialClassifyParityStats& stats,
    const RtPathTraceMaterialClassifyProduct& product,
    const viewDef_t* viewDef)
{
    for (int surfaceIndex = 0;
         surfaceIndex < viewDef->numDrawSurfs;
         ++surfaceIndex)
    {
        const drawSurf_t* drawSurf = viewDef->drawSurfs[surfaceIndex];
        const idMaterial* material = drawSurf ? drawSurf->material : nullptr;
        const uint64_t materialIdentity = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(material));
        const RtPathTraceMaterialClassifySurface& productSurface =
            product.surfaces[surfaceIndex];
        const bool nullMappingMatches = !material &&
            productSurface.materialIdentity == 0 &&
            productSurface.materialSlot == 0 &&
            product.materialIdentities[0] == 0;
        const bool materialMappingMatches = material &&
            productSurface.materialIdentity == materialIdentity &&
            productSurface.materialSlot < product.classifierCount &&
            product.materialIdentities[productSurface.materialSlot] ==
                materialIdentity;
        const bool mappingMatches =
            nullMappingMatches || materialMappingMatches;
        bool classifierMatches = true;
        if (mappingMatches)
        {
            const RtSmokeTranslucentClassifierInfo serialClassifier =
                BuildSmokeTranslucentClassifierInfo(material);
            classifierMatches = RtSmokeTranslucentClassifierInfoEqual(
                product.classifiers[productSurface.materialSlot],
                serialClassifier);
        }

        ++stats.mapped;
        stats.mappingMismatches += mappingMatches ? 0 : 1;
        stats.classifierMismatches += classifierMatches ? 0 : 1;
        if ((!mappingMatches || !classifierMatches) &&
            stats.mismatchSampleCount < 4)
        {
            stats.mismatchOrdinals[stats.mismatchSampleCount++] =
                surfaceIndex;
        }
    }
}

void RecordPathTraceMaterialClassifyParity(
    RtPathTraceMaterialClassifyParityStats& stats,
    int surfaceIndex,
    const drawSurf_t* drawSurf,
    const srfTriangles_t* tri,
    RtSmokeSurfaceClass classifiedSurfaceClass,
    RtSmokeSurfaceClass productSurfaceClass,
    RtSmokeTranslucentSubtype productSubtype,
    uint32_t productSignature,
    const RtSmokeTranslucentClassifierInfo& productClassifier)
{
    const idMaterial* material = drawSurf ? drawSurf->material : nullptr;
    const RtSmokeTranslucentClassifierInfo serialClassifier =
        BuildSmokeTranslucentClassifierInfo(material);
    const RtSmokeSurfaceClass serialSurfaceClass =
        PtMirrorEffectiveSurfaceClass(
            drawSurf, tri, classifiedSurfaceClass, &serialClassifier);
    const RtSmokeTranslucentSubtype serialSubtype =
        serialSurfaceClass == RtSmokeSurfaceClass::ParticleAlpha
            ? ClassifySmokeTranslucentSubtype(drawSurf, serialClassifier)
            : RtSmokeTranslucentSubtype::Unknown;
    const bool productEmissive =
        SmokeDrawSurfaceHasActiveEmissiveStage(drawSurf, productClassifier);
    const bool serialEmissive =
        SmokeDrawSurfaceHasActiveEmissiveStage(drawSurf, serialClassifier);
    const uint32_t serialSignature = SmokeMaterialRouteClassSignature(
        material, serialSurfaceClass, serialSubtype, serialClassifier);

    const bool surfaceClassMatches =
        productSurfaceClass == serialSurfaceClass;
    const bool subtypeMatches = productSubtype == serialSubtype;
    const bool emissiveMatches = productEmissive == serialEmissive;
    const bool signatureMatches = productSignature == serialSignature;
    const bool particle =
        productSurfaceClass == RtSmokeSurfaceClass::ParticleAlpha ||
        serialSurfaceClass == RtSmokeSurfaceClass::ParticleAlpha;
    const bool matches = surfaceClassMatches && subtypeMatches &&
        emissiveMatches && signatureMatches;

    ++stats.compared;
    stats.particles += particle ? 1 : 0;
    stats.surfaceClassMismatches += surfaceClassMatches ? 0 : 1;
    stats.subtypeMismatches += subtypeMatches ? 0 : 1;
    stats.emissiveMismatches += emissiveMatches ? 0 : 1;
    stats.signatureMismatches += signatureMatches ? 0 : 1;
    if (!matches)
    {
        ++stats.mismatched;
        stats.particleMismatches += particle ? 1 : 0;
        if (stats.mismatchSampleCount < 4)
        {
            stats.mismatchOrdinals[stats.mismatchSampleCount++] =
                surfaceIndex;
        }
    }
}

void AddMirrorMaterialStats(RtSmokeMaterialStats& stats, const idMaterial* material, int indexes, RtSmokeSurfaceClass surfaceClass, RtSmokeTranslucentSubtype translucentSubtype)
{
    const char* materialName = material ? material->GetName() : "<none>";
    const uint32_t materialId = HashSmokeMaterialName(materialName);
    ++stats.totalSurfaces;
    stats.totalTriangles += indexes / 3;

    const bool firstMaterial = std::find(stats.materialIds.begin(), stats.materialIds.end(), materialId) == stats.materialIds.end();
    if (firstMaterial)
    {
        stats.materialIds.push_back(materialId);
        ++stats.uniqueMaterials;
    }

    bool materialSampleFound = false;
    for (int sampleIndex = 0; sampleIndex < stats.sampleCount; ++sampleIndex)
    {
        RtSmokeMaterialSample& sample = stats.samples[sampleIndex];
        if (sample.id == materialId)
        {
            ++sample.surfaces;
            sample.triangles += indexes / 3;
            materialSampleFound = true;
            break;
        }
    }
    if (!materialSampleFound && stats.sampleCount < RT_SMOKE_MATERIAL_REASON_SAMPLES)
    {
        RtSmokeMaterialSample& sample = stats.samples[stats.sampleCount++];
        sample.id = materialId;
        sample.surfaces = 1;
        sample.triangles = indexes / 3;
        sample.name = materialName;
    }

    if (surfaceClass != RtSmokeSurfaceClass::ParticleAlpha)
    {
        return;
    }

    ++stats.translucentSurfaces;
    stats.translucentTriangles += indexes / 3;
    const bool firstTranslucentMaterial = std::find(stats.translucentMaterialIds.begin(), stats.translucentMaterialIds.end(), materialId) == stats.translucentMaterialIds.end();
    if (firstTranslucentMaterial)
    {
        stats.translucentMaterialIds.push_back(materialId);
        ++stats.translucentUniqueMaterials;
    }

    const int subtypeIndex = idMath::ClampInt(0, RT_SMOKE_TRANSLUCENT_SUBTYPE_COUNT - 1, static_cast<int>(SmokeTranslucentSubtypeId(translucentSubtype)));
    ++stats.translucentSubtypeSurfaces[subtypeIndex];
    stats.translucentSubtypeTriangles[subtypeIndex] += indexes / 3;
}

void AddMirrorSurfaceClassStats(RtSmokeSurfaceClassStats& stats, RtSmokeSurfaceClass surfaceClass, int verts, int indexes)
{
    const int triangles = indexes / 3;
    switch (surfaceClass)
    {
        case RtSmokeSurfaceClass::StaticWorld:
            ++stats.staticWorldSurfaces;
            stats.staticWorldVerts += verts;
            stats.staticWorldIndexes += indexes;
            stats.staticWorldTriangles += triangles;
            break;
        case RtSmokeSurfaceClass::RigidEntity:
            ++stats.rigidEntitySurfaces;
            stats.rigidEntityVerts += verts;
            stats.rigidEntityIndexes += indexes;
            stats.rigidEntityTriangles += triangles;
            break;
        case RtSmokeSurfaceClass::SkinnedDeformed:
            ++stats.skinnedDeformedSurfaces;
            stats.skinnedDeformedVerts += verts;
            stats.skinnedDeformedIndexes += indexes;
            stats.skinnedDeformedTriangles += triangles;
            break;
        case RtSmokeSurfaceClass::ParticleAlpha:
            ++stats.particleAlphaSurfaces;
            stats.particleAlphaVerts += verts;
            stats.particleAlphaIndexes += indexes;
            stats.particleAlphaTriangles += triangles;
            break;
        default:
            ++stats.unknownSurfaces;
            stats.unknownVerts += verts;
            stats.unknownIndexes += indexes;
            stats.unknownTriangles += triangles;
            break;
    }
}

void AddMirrorDynamicGeometryStats(RtSmokeDynamicGeometryStats& stats, RtSmokeSurfaceClass surfaceClass, const drawSurf_t* drawSurf, const srfTriangles_t* tri, int indexes)
{
    switch (surfaceClass)
    {
        case RtSmokeSurfaceClass::RigidEntity:
            ++stats.rigidSurfaces;
            stats.rigidIndexes += indexes;
            break;
        case RtSmokeSurfaceClass::SkinnedDeformed:
            if (GetSmokeRtCpuSkinningJoints(tri) != nullptr)
            {
                ++stats.skinnedRtCpuSkinnedSurfaces;
                stats.skinnedRtCpuSkinnedIndexes += indexes;
            }
            else if (SmokeSkinnedSurfaceLikelyBasePose(drawSurf, tri))
            {
                ++stats.skinnedLikelyBasePoseSurfaces;
                stats.skinnedLikelyBasePoseIndexes += indexes;
            }
            else
            {
                ++stats.skinnedCpuCurrentSurfaces;
                stats.skinnedCpuCurrentIndexes += indexes;
            }
            break;
        case RtSmokeSurfaceClass::ParticleAlpha:
            ++stats.particleAlphaSurfaces;
            stats.particleAlphaIndexes += indexes;
            break;
        case RtSmokeSurfaceClass::Unknown:
            ++stats.unknownSurfaces;
            stats.unknownIndexes += indexes;
            break;
        default:
            break;
    }
}

bool DrawSurfMirrorIsStaticMatch(const RtSmokeGeometryUniverse* geometryUniverse, const std::unordered_set<uint64>& sceneUniverseLegacyKeys, uint64 legacyStaticKey)
{
    if (!sceneUniverseLegacyKeys.empty() && sceneUniverseLegacyKeys.find(legacyStaticKey) != sceneUniverseLegacyKeys.end())
    {
        return true;
    }
    return geometryUniverse && geometryUniverse->HasStaticSurface(legacyStaticKey);
}

bool PtMirrorIsEligibleRigidCandidate(
    const RtPathTraceMeshObservation& meshObservation,
    const RtPathTraceInstanceObservation& instanceObservation)
{
    if (!RtPathTraceSourceFlagsAreDurableRigid(instanceObservation.sourceFlags))
    {
        return false;
    }
    if (meshObservation.key.tri == nullptr ||
        meshObservation.key.numVerts <= 0 ||
        meshObservation.key.numIndexes <= 0 ||
        (meshObservation.key.numIndexes % 3) != 0 ||
        meshObservation.stableHash == 0 ||
        meshObservation.key.materialId == 0 ||
        !meshObservation.localSpaceValid)
    {
        return false;
    }
    return true;
}

idVec4 PtMirrorBoundsColor(
    RtSmokeSurfaceClass surfaceClass,
    const RtPathTraceMeshObservation& meshObservation,
    const RtPathTraceInstanceObservation& instanceObservation)
{
    if ((instanceObservation.sourceFlags & RT_PT_INSTANCE_SOURCE_STATIC_CACHE_MATCH) != 0 ||
        (instanceObservation.sourceFlags & RT_PT_INSTANCE_SOURCE_STATIC_WORLD) != 0 ||
        surfaceClass == RtSmokeSurfaceClass::StaticWorld)
    {
        return colorGreen;
    }
    if (PtMirrorIsEligibleRigidCandidate(meshObservation, instanceObservation))
    {
        return colorDodgerBlue;
    }
    if ((instanceObservation.sourceFlags & RT_PT_INSTANCE_SOURCE_RIGID) != 0)
    {
        return colorRed;
    }
    if ((instanceObservation.sourceFlags & RT_PT_INSTANCE_SOURCE_SKINNED_OR_DEFORMING) != 0 ||
        surfaceClass == RtSmokeSurfaceClass::SkinnedDeformed)
    {
        return colorMagenta;
    }
    if ((instanceObservation.sourceFlags & RT_PT_INSTANCE_SOURCE_PARTICLE_OR_TRANSIENT) != 0 ||
        surfaceClass == RtSmokeSurfaceClass::ParticleAlpha)
    {
        return colorGray;
    }
    if ((instanceObservation.sourceFlags & RT_PT_INSTANCE_SOURCE_GUI) != 0)
    {
        return colorOrange;
    }
    return colorYellow;
}

void PtMirrorBoundsPointToWorld(const drawSurf_t* drawSurf, const idVec3& localPoint, idVec3& worldPoint)
{
    if (drawSurf && drawSurf->space)
    {
        R_LocalPointToGlobal(drawSurf->space->modelMatrix, localPoint, worldPoint);
        return;
    }
    worldPoint = localPoint;
}

int PtMirrorResolveDrawSurfArea(const viewDef_t* viewDef, const drawSurf_t* drawSurf, const srfTriangles_t* tri)
{
    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    if (!renderWorld || !drawSurf || !tri)
    {
        return -1;
    }

    idVec3 worldCenter;
    PtMirrorBoundsPointToWorld(drawSurf, tri->bounds.GetCenter(), worldCenter);
    return renderWorld->PointInArea(worldCenter);
}

void PtMirrorAppendBoundsOverlayLines(
    const drawSurf_t* drawSurf,
    const srfTriangles_t* tri,
    const idVec4& color,
    std::vector<RtPathTraceBoundsOverlayLine>& lines)
{
    if (!tri || tri->bounds.IsCleared() || lines.size() + 12 > RT_PT_BOUNDS_OVERLAY_MAX_LINES)
    {
        return;
    }

    idVec3 corners[8];
    for (int cornerIndex = 0; cornerIndex < 8; ++cornerIndex)
    {
        idVec3 localPoint;
        localPoint.x = tri->bounds[(cornerIndex ^ (cornerIndex >> 1)) & 1].x;
        localPoint.y = tri->bounds[(cornerIndex >> 1) & 1].y;
        localPoint.z = tri->bounds[(cornerIndex >> 2) & 1].z;
        PtMirrorBoundsPointToWorld(drawSurf, localPoint, corners[cornerIndex]);
    }

    for (int edgeIndex = 0; edgeIndex < 4; ++edgeIndex)
    {
        const int edgeStarts[3] = { edgeIndex, 4 + edgeIndex, edgeIndex };
        const int edgeEnds[3] = { (edgeIndex + 1) & 3, 4 + ((edgeIndex + 1) & 3), 4 + edgeIndex };
        for (int edgePart = 0; edgePart < 3; ++edgePart)
        {
            RtPathTraceBoundsOverlayLine line;
            line.startAndPad.Set(corners[edgeStarts[edgePart]].x, corners[edgeStarts[edgePart]].y, corners[edgeStarts[edgePart]].z, 0.0f);
            line.endAndPad.Set(corners[edgeEnds[edgePart]].x, corners[edgeEnds[edgePart]].y, corners[edgeEnds[edgePart]].z, 0.0f);
            line.color = color;
            lines.push_back(line);
        }
    }
}

void RecordPathTraceDrawSurfMirrorObservation(
    const viewDef_t* viewDef,
    RtSmokeGeometryUniverse* geometryUniverse,
    RtPathTraceInstanceUniverse& instanceUniverse,
    std::vector<RtPathTraceBoundsOverlayLine>* boundsOverlayLines,
    int boundsOverlayMode,
    int boundsOverlayMax,
    int& boundsOverlayDrawn,
    int surfaceIndex,
    const drawSurf_t* drawSurf,
    const srfTriangles_t* tri,
    RtSmokeSurfaceClass surfaceClass,
    const idMaterial* material,
    uint32_t materialId,
    uint32_t sourceKind,
    uint32_t sourceFlags,
    uint32_t surfaceClassId,
    uint32_t surfaceClassAndFlags,
    uint32_t materialClassSignature,
    std::vector<RtPathTraceRigidMeshCandidateObservation>*
        deferredRigidCandidates)
{
    const viewEntity_t* space = drawSurf ? drawSurf->space : nullptr;
    const idRenderEntityLocal* entity = space ? space->entityDef : nullptr;
    const renderEntity_t* renderEntity = entity ? &entity->parms : nullptr;
    const idRenderModel* renderModel = renderEntity ? renderEntity->hModel : nullptr;
    const char* modelName = renderModel ? renderModel->Name() : "<none>";
    const PtRenderDefKey renderDefKey = PtGeometryLifecycle::MakeEntityKey(entity);
    const uint32_t modelEpoch = (renderDefKey.world && renderDefKey.index >= 0)
        ? PtGeometryLifecycle::EntityModelEpoch(renderDefKey.world, renderDefKey.index)
        : 0;

    RtPathTraceMeshKey meshKey;
    FillPathTraceRigidRouteMeshKey(meshKey, tri, materialId, materialClassSignature, sourceKind);
    RtPathTraceRigidInstanceSnapshot rigidSnapshot;
    {
        OPTICK_EVENT("PT DrawSurf Observation Identity");
        rigidSnapshot = BuildPathTraceRigidInstanceSnapshot(
            meshKey,
            renderModel,
            tri,
            renderDefKey,
            modelEpoch,
            entity ? entity->index : -1,
            renderEntity ? renderEntity->entityNum : -1,
            drawSurf ? drawSurf->modelSurfaceIndex : -1,
            sourceFlags);
    }
    if (geometryUniverse != nullptr)
    {
        geometryUniverse->RecordA8S1RouteObservation(
            rigidSnapshot.renderDefKey,
            rigidSnapshot.modelSurfaceIndex,
            rigidSnapshot.modelSurfaceIndexValid,
            rigidSnapshot.meshHash,
            modelName,
            PtA8S1RouteProducer::VisibleDrawSurf);
    }

    RtPathTraceMeshObservation meshObservation;
    meshObservation.key = rigidSnapshot.meshKey;
    meshObservation.stableHash = rigidSnapshot.meshHash;
    meshObservation.baseMaterial = material;
    meshObservation.surfaceClassId = surfaceClassId;
    meshObservation.jointIndex = rigidSnapshot.jointIndex;
    meshObservation.materialName = material ? material->GetName() : "<none>";
    meshObservation.modelName = modelName;
    meshObservation.localSpaceValid = true;

    RtPathTraceInstanceObservation instanceObservation;
    instanceObservation.meshHash = rigidSnapshot.meshHash;
    instanceObservation.entity = entity;
    instanceObservation.entityIndex = rigidSnapshot.entityIndex;
    instanceObservation.renderEntityNum = rigidSnapshot.renderEntityNum;
    instanceObservation.drawSurfIndex = surfaceIndex;
    instanceObservation.modelSurfaceIndex = rigidSnapshot.modelSurfaceIndex;
    instanceObservation.jointIndex = rigidSnapshot.jointIndex;
    instanceObservation.currentArea = PtMirrorResolveDrawSurfArea(viewDef, drawSurf, tri);
    instanceObservation.renderDefKey = rigidSnapshot.renderDefKey;
    instanceObservation.modelEpoch = rigidSnapshot.modelEpoch;
    instanceObservation.materialOverrideId = rigidSnapshot.materialId;
    instanceObservation.surfaceClassId = surfaceClassId;
    instanceObservation.triangleClassAndFlags = surfaceClassAndFlags;
    instanceObservation.sourceFlags = rigidSnapshot.sourceFlags;
    CopyDrawSurfObjectToWorld(drawSurf, instanceObservation.objectToWorld);
    instanceObservation.instanceId = rigidSnapshot.instanceId;
    instanceObservation.materialName = meshObservation.materialName;
    instanceObservation.modelName = meshObservation.modelName;

    {
        OPTICK_EVENT("PT DrawSurf Instance Observation");
        instanceUniverse.RecordObservation(
            meshObservation,
            instanceObservation,
            surfaceClass,
            tri->numVerts,
            tri->numIndexes);
    }
    NotePathTraceCaptureSerialInstanceObservation(
        static_cast<std::uint32_t>(surfaceIndex),
        instanceObservation.instanceId,
        instanceObservation.meshHash,
        instanceObservation.objectToWorld);
    const bool eligibleRigid = PtMirrorIsEligibleRigidCandidate(meshObservation, instanceObservation);
    if ((boundsOverlayMode == 1 || boundsOverlayMode == 2) && boundsOverlayDrawn < boundsOverlayMax)
    {
        if (boundsOverlayLines && (boundsOverlayMode >= 2 || eligibleRigid))
        {
            PtMirrorAppendBoundsOverlayLines(drawSurf, tri, PtMirrorBoundsColor(surfaceClass, meshObservation, instanceObservation), *boundsOverlayLines);
            ++boundsOverlayDrawn;
        }
    }
    if (geometryUniverse && eligibleRigid)
    {
        RtPathTraceRigidMeshCandidateObservation candidateObservation;
        candidateObservation.tri = tri;
        candidateObservation.meshHash = meshObservation.stableHash;
        candidateObservation.instanceId = instanceObservation.instanceId;
        candidateObservation.vertexBufferIdentity = meshObservation.key.vertexBufferIdentity;
        candidateObservation.indexBufferIdentity = meshObservation.key.indexBufferIdentity;
        candidateObservation.sourceFlags = instanceObservation.sourceFlags;
        candidateObservation.materialId = materialId;
        candidateObservation.materialClassSignature = materialClassSignature;
        candidateObservation.surfaceClassId = surfaceClassId;
        candidateObservation.triangleClassAndFlags = surfaceClassAndFlags;
        candidateObservation.vertexFormat = meshObservation.key.vertexFormat;
        candidateObservation.drawSurfIndex = surfaceIndex;
        candidateObservation.entityIndex = instanceObservation.entityIndex;
        candidateObservation.renderEntityNum = instanceObservation.renderEntityNum;
        candidateObservation.modelSurfaceIndex =
            rigidSnapshot.modelSurfaceIndex;
        candidateObservation.modelEpoch = modelEpoch;
        candidateObservation.jointIndex = rigidSnapshot.jointIndex;
        candidateObservation.numVerts = tri->numVerts;
        candidateObservation.numIndexes = tri->numIndexes;
        candidateObservation.localSpaceValid = meshObservation.localSpaceValid;
        BuildRigidNormalTexMatrix(
            drawSurf->material,
            drawSurf->shaderRegisters ? drawSurf->shaderRegisters : drawSurf->material->ConstantRegisters(),
            candidateObservation.normalTexMatrix);
        candidateObservation.materialName = meshObservation.materialName;
        candidateObservation.modelName = meshObservation.modelName;
        {
            OPTICK_EVENT("PT DrawSurf Rigid Candidate");
            if (deferredRigidCandidates != nullptr)
            {
                OPTICK_EVENT("PT Lane A Rigid Deferred Capture");
                deferredRigidCandidates->push_back(candidateObservation);
            }
            else
            {
                geometryUniverse->RecordRigidMeshCandidate(
                    candidateObservation);
            }
            NotePathTraceCaptureSerialRigidCandidate(
                static_cast<std::uint32_t>(surfaceIndex),
                candidateObservation.meshHash,
                candidateObservation.instanceId,
                candidateObservation.materialId);
        }
    }
}

}

void CapturePathTraceDrawSurfMirror(
    const viewDef_t* viewDef,
    const RtPathTraceSceneUniverse* sceneUniverse,
    RtSmokeGeometryUniverse* geometryUniverse,
    RtPathTraceInstanceUniverse& instanceUniverse,
    std::vector<RtPathTraceBoundsOverlayLine>* boundsOverlayLines,
    const std::vector<RtPathTraceDrawSurfMirrorSurfaceCache>* surfaceCache)
{
    if (!viewDef || !viewDef->drawSurfs)
    {
        instanceUniverse.EndFrame();
        return;
    }

    const bool useSurfaceCache =
        surfaceCache != nullptr &&
        static_cast<int>(surfaceCache->size()) == viewDef->numDrawSurfs;
    std::unordered_set<uint64> sceneUniverseLegacyKeys;
    if (!useSurfaceCache)
    {
        OPTICK_EVENT("PT DrawSurf Mirror Static Keys");
        BuildSceneUniverseLegacyKeySet(sceneUniverse, sceneUniverseLegacyKeys);
    }

    instanceUniverse.SetObservedDrawSurfCount(viewDef->numDrawSurfs);
    const int boundsOverlayMode = IsPathTraceBoundsOverlayDebugMode(r_pathTracingDebugMode.GetInteger())
        ? Max(1, r_pathTracingSceneBoundsOverlay.GetInteger())
        : r_pathTracingSceneBoundsOverlay.GetInteger();
    const int boundsOverlayMax = Max(0, r_pathTracingSceneBoundsOverlayMax.GetInteger());
    int boundsOverlayDrawn = 0;

    {
        OPTICK_EVENT("PT DrawSurf Mirror Visible Loop");
        for (int surfaceIndex = 0; surfaceIndex < viewDef->numDrawSurfs; ++surfaceIndex)
        {
            SetPathTraceCaptureSerialOracleSurface(
                static_cast<std::uint32_t>(surfaceIndex));
            const RtPathTraceDrawSurfMirrorSurfaceCache* cachedSurface =
                useSurfaceCache ? &(*surfaceCache)[surfaceIndex] : nullptr;
            const drawSurf_t* drawSurf = cachedSurface ? cachedSurface->drawSurf : viewDef->drawSurfs[surfaceIndex];
            const srfTriangles_t* tri = nullptr;
            RtSmokeSurfaceSkipStats skipStats;
            RtSmokeSurfaceClass classifiedSurfaceClass = RtSmokeSurfaceClass::Unknown;
            RtSmokeSurfaceClass surfaceClass = RtSmokeSurfaceClass::Unknown;
            const idMaterial* material = nullptr;
            uint32_t baseMaterialId = 0;
            uint32_t materialId = 0;
            uint32_t sourceKind = 0;
            uint32_t sourceFlags = 0;
            uint32_t surfaceClassId = 0;
            uint32_t surfaceClassAndFlags = 0;
            uint32_t materialClassSignature = 0;
            uint64 legacyStaticKey = 0;
            if (cachedSurface)
            {
                if (!cachedSurface->valid)
                {
                    instanceUniverse.RecordSkippedDrawSurf(cachedSurface->skipStats);
                    continue;
                }
                tri = cachedSurface->tri;
                classifiedSurfaceClass = cachedSurface->classifiedSurfaceClass;
                surfaceClass = cachedSurface->surfaceClass;
                material = drawSurf ? drawSurf->material : nullptr;
                baseMaterialId = cachedSurface->baseMaterialId;
                materialId = cachedSurface->materialId;
                sourceKind = cachedSurface->sourceKind;
                sourceFlags = cachedSurface->sourceFlags;
                surfaceClassId = cachedSurface->surfaceClassId;
                surfaceClassAndFlags = cachedSurface->surfaceClassAndFlags;
                materialClassSignature = cachedSurface->materialClassSignature;
                legacyStaticKey = cachedSurface->legacyStaticKey;
            }
            else
            {
                if (!ValidateSmokeDrawSurface(viewDef, drawSurf, tri, &skipStats))
                {
                    instanceUniverse.RecordSkippedDrawSurf(skipStats);
                    continue;
                }

                classifiedSurfaceClass = ClassifySmokeSurface(viewDef, drawSurf, tri);
                surfaceClass = PtMirrorEffectiveSurfaceClass(drawSurf, tri, classifiedSurfaceClass);
                material = drawSurf ? drawSurf->material : nullptr;
                baseMaterialId = SmokeMaterialId(material);
                materialId = SmokeRuntimeMaterialTableIdForDrawSurf(drawSurf, baseMaterialId);
                sourceKind = SmokeSurfaceClassId(surfaceClass);
                const RtSmokeTranslucentSubtype translucentSubtype = surfaceClass == RtSmokeSurfaceClass::ParticleAlpha ? ClassifySmokeTranslucentSubtype(drawSurf) : RtSmokeTranslucentSubtype::Unknown;
                surfaceClassId = SmokeSurfaceClassAndSubtypeId(surfaceClass, translucentSubtype);
                surfaceClassAndFlags = surfaceClassId |
                    (SmokeDrawSurfaceHasActiveEmissiveStage(drawSurf) ? 0u : RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF);
                materialClassSignature = SmokeMaterialRouteClassSignature(material, surfaceClass, translucentSubtype);
                legacyStaticKey = BuildSmokeStaticSurfaceKeyForDiagnostics(drawSurf, tri);
                sourceFlags = PtSourceFlagsForDrawSurf(viewDef, drawSurf, tri, surfaceClass);
                if (!sceneUniverseLegacyKeys.empty() && sceneUniverseLegacyKeys.find(legacyStaticKey) != sceneUniverseLegacyKeys.end())
                {
                    sourceFlags |= RT_PT_INSTANCE_SOURCE_STATIC_UNIVERSE_MATCH;
                }
                if (geometryUniverse && geometryUniverse->HasStaticSurface(legacyStaticKey))
                {
                    sourceFlags |= RT_PT_INSTANCE_SOURCE_STATIC_CACHE_MATCH;
                }
            }

            if (UnifiedPtDiagnosticRemovesAlphaClipSurface(material))
            {
                ++skipStats.alphaClipDiagnostic;
                instanceUniverse.RecordSkippedDrawSurf(skipStats);
                continue;
            }

            RecordPathTraceDrawSurfMirrorObservation(
                viewDef,
                geometryUniverse,
                instanceUniverse,
                boundsOverlayLines,
                boundsOverlayMode,
                boundsOverlayMax,
                boundsOverlayDrawn,
                surfaceIndex,
                drawSurf,
                tri,
                surfaceClass,
                material,
                materialId,
                sourceKind,
                sourceFlags,
                surfaceClassId,
                surfaceClassAndFlags,
                materialClassSignature,
                nullptr);
        }
    }

    {
        OPTICK_EVENT("PT DrawSurf Mirror EndFrame");
        instanceUniverse.EndFrame();
    }
}

uint64 BuildPathTraceSkinnedCaptureViewSignature(
    const viewDef_t* viewDef)
{
    if (!viewDef || !viewDef->drawSurfs)
    {
        return 0;
    }

    std::vector<uint64> instanceHashes;
    for (int surfaceIndex = 0;
        surfaceIndex < viewDef->numDrawSurfs;
        ++surfaceIndex)
    {
        const drawSurf_t* drawSurf =
            viewDef->drawSurfs[surfaceIndex];
        const srfTriangles_t* tri = nullptr;
        if (!ValidateSmokeDrawSurface(
                viewDef,
                drawSurf,
                tri,
                nullptr) ||
            PathTraceParticleCompositeSurfaceRoute(
                viewDef,
                drawSurf,
                tri) ==
                RtPathTraceParticleSurfaceRoute::CompositeOnly)
        {
            continue;
        }
        if (UnifiedPtDiagnosticRemovesAlphaClipSurface(
                drawSurf ? drawSurf->material : nullptr))
        {
            continue;
        }
        const RtSmokeSurfaceClass classified =
            ClassifySmokeSurface(viewDef, drawSurf, tri);
        const RtSmokeSurfaceClass surfaceClass =
            PtMirrorEffectiveSurfaceClass(
                drawSurf,
                tri,
                classified);
        if (surfaceClass !=
            RtSmokeSurfaceClass::SkinnedDeformed)
        {
            continue;
        }

        const uint32_t surfaceClassId =
            SmokeSurfaceClassAndSubtypeId(
                surfaceClass,
                RtSmokeTranslucentSubtype::Unknown);
        const uint32_t baseMaterialId =
            SmokeMaterialId(
                drawSurf ? drawSurf->material : nullptr);
        const uint32_t materialId =
            SmokeRuntimeMaterialTableIdForDrawSurf(
                drawSurf,
                baseMaterialId);
        std::vector<RtSmokeSkinnedSurfaceRecord> records;
        AddSmokeSkinnedSurfaceRecord(
            &records,
            drawSurf,
            tri,
            surfaceClassId,
            materialId,
            surfaceIndex,
            -1,
            -1,
            -1,
            -1,
            tri->numVerts,
            tri->numIndexes,
            tri->numIndexes / 3);
        if (!records.empty() &&
            PtCanonicalInstanceKeyIsValid(
                records.front().canonicalInstance))
        {
            instanceHashes.push_back(
                PtHashCanonicalInstanceKey(
                    records.front().canonicalInstance));
        }
    }

    if (instanceHashes.empty())
    {
        return 0;
    }
    std::sort(instanceHashes.begin(), instanceHashes.end());
    uint64 hash = 1469598103934665603ull;
    const uint64 count =
        static_cast<uint64>(instanceHashes.size());
    hash ^= count;
    hash *= 1099511628211ull;
    for (uint64 instanceHash : instanceHashes)
    {
        hash ^= instanceHash;
        hash *= 1099511628211ull;
    }
    return hash;
}

RtPathTraceRigidInstanceSnapshot BuildAndRecordPathTraceCaptureWalkObservation(
    RtSmokeGeometryUniverse* geometryUniverse,
    const RtPathTraceMeshKey& meshKey,
    const idRenderModel* renderModel,
    const srfTriangles_t* tri,
    const PtRenderDefKey& renderDefKey,
    std::uint32_t modelEpoch,
    int entityIndex,
    int entityNum,
    int requestedSurfaceIndex,
    std::uint32_t sourceFlags)
{
    const RtPathTraceRigidInstanceSnapshot snapshot =
        BuildPathTraceRigidInstanceSnapshot(meshKey, renderModel, tri,
            renderDefKey, modelEpoch, entityIndex, entityNum,
            requestedSurfaceIndex, sourceFlags);
    geometryUniverse->RecordA8S1RouteObservation(
        snapshot.renderDefKey,
        snapshot.modelSurfaceIndex,
        snapshot.modelSurfaceIndexValid,
        snapshot.meshHash,
        renderModel ? renderModel->Name() : "<none>",
        PtA8S1RouteProducer::CaptureWalkProduct);
    return snapshot;
}

RtPathTraceRigidInstanceSnapshot BuildAndRecordPathTraceMergedCompanionObservation(
    RtSmokeGeometryUniverse* geometryUniverse,
    const RtPathTraceMeshKey& meshKey,
    const idRenderModel* renderModel,
    const srfTriangles_t* tri,
    const PtRenderDefKey& renderDefKey,
    std::uint32_t modelEpoch,
    int entityIndex,
    int entityNum,
    int requestedSurfaceIndex,
    std::uint32_t sourceFlags)
{
    const RtPathTraceRigidInstanceSnapshot snapshot =
        BuildPathTraceRigidInstanceSnapshot(meshKey, renderModel, tri,
            renderDefKey, modelEpoch, entityIndex, entityNum,
            requestedSurfaceIndex, sourceFlags);
    geometryUniverse->RecordA8S1RouteObservation(
        snapshot.renderDefKey,
        snapshot.modelSurfaceIndex,
        snapshot.modelSurfaceIndexValid,
        snapshot.meshHash,
        renderModel ? renderModel->Name() : "<none>",
        PtA8S1RouteProducer::MergedCompanion);
    return snapshot;
}

bool CapturePathTraceDynamicFrameFromDrawSurfMirror(
    const viewDef_t* viewDef,
    const RtPathTraceSceneUniverse* sceneUniverse,
    RtSmokeGeometryUniverse* geometryUniverse,
    std::vector<PathTraceSmokeVertex>& vertexData,
    std::vector<uint32_t>& indexData,
    std::vector<uint32_t>& triangleClassData,
    std::vector<uint32_t>& triangleMaterialData,
    std::vector<uint32_t>* triangleInstanceData,
    std::vector<uint32_t>* triangleIdentityData,
    int& sourceSurfaces,
    int& sourceVerts,
    int& sourceIndexes,
    RtSmokeSurfaceClassStats& classStats,
    RtSmokeSurfaceSkipStats& skipStats,
    RtSmokeDynamicGeometryStats& dynamicStats,
    RtSmokeAttributeStats& attributeStats,
    RtSmokeMaterialStats& materialStats,
    RtSmokeBucketRanges& bucketRanges,
    RtSmokeSceneCaptureTiming& captureTiming,
    std::vector<RtSmokeSkinnedSurfaceRecord>* skinnedSurfaceRecords,
    std::vector<RtSmokeCapturedSurfaceRecord>* capturedSurfaceRecords,
    std::vector<RtPathTraceDrawSurfMirrorSurfaceCache>* surfaceCache,
    RtPathTraceInstanceUniverse* instanceUniverse,
    std::vector<RtPathTraceBoundsOverlayLine>* boundsOverlayLines,
    bool recordAllInstanceClasses,
    const std::vector<PtSkinnedHitRouteRecord>*
        skinnedCaptureAdmissionRoutes,
    std::vector<RtSmokeRigidCaptureSkipRecord>* rigidCaptureSkips,
    std::vector<uint64_t>* rigidCaptureWalked,
    std::vector<uint32_t>* rigidCaptureWalkedTriangles,
    std::vector<RtSmokeMergedWalkedRange>* mergedWalkedRanges,
    RtPathTraceOwnerHarvest* ownerHarvest,
    std::vector<RtPathTraceRigidMeshCandidateObservation>*
        deferredRigidCandidates)
{
    OPTICK_EVENT("PT Capture Dynamic Frame From DrawSurf Mirror");
    const uint64_t ownerHarvestStartUs = ownerHarvest ? Sys_Microseconds() : 0;
    if (ownerHarvest)
    {
        ownerHarvest->Clear();
    }
    const bool r1Requested = r_pathTracingCaptureFixNObserveR1.GetInteger() != 0;

    sourceSurfaces = 0;
    sourceVerts = 0;
    sourceIndexes = 0;
    classStats = RtSmokeSurfaceClassStats();
    skipStats = RtSmokeSurfaceSkipStats();
    dynamicStats = RtSmokeDynamicGeometryStats();
    attributeStats = RtSmokeAttributeStats();
    materialStats = RtSmokeMaterialStats();
    bucketRanges = RtSmokeBucketRanges();
    captureTiming = RtSmokeSceneCaptureTiming();
    if (surfaceCache)
    {
        surfaceCache->clear();
    }
    if (capturedSurfaceRecords)
    {
        capturedSurfaceRecords->clear();
    }
    if (mergedWalkedRanges)
    {
        mergedWalkedRanges->clear();
    }

    {
        OPTICK_EVENT("PT Capture Dynamic Clear Buffers");
        vertexData.clear();
        indexData.clear();
        triangleClassData.clear();
        triangleMaterialData.clear();
        if (triangleInstanceData)
        {
            triangleInstanceData->clear();
        }
        if (triangleIdentityData)
        {
            triangleIdentityData->clear();
        }
        vertexData.reserve(RT_SMOKE_INITIAL_RESERVE_VERTS);
        indexData.reserve(RT_SMOKE_INITIAL_RESERVE_INDEXES);
        triangleClassData.reserve(RT_SMOKE_INITIAL_RESERVE_INDEXES / 3);
        triangleMaterialData.reserve(RT_SMOKE_INITIAL_RESERVE_INDEXES / 3);
    }

    if (!viewDef || !viewDef->drawSurfs)
    {
        FinalizePathTraceCommittedCaptureTelemetry(viewDef);
        return false;
    }
    if (surfaceCache)
    {
        surfaceCache->assign(viewDef->numDrawSurfs, RtPathTraceDrawSurfMirrorSurfaceCache());
    }
    int boundsOverlayMode = 0;
    int boundsOverlayMax = 0;
    int boundsOverlayDrawn = 0;
    if (instanceUniverse)
    {
        instanceUniverse->SetObservedDrawSurfCount(viewDef->numDrawSurfs);
        boundsOverlayMode = IsPathTraceBoundsOverlayDebugMode(r_pathTracingDebugMode.GetInteger())
            ? Max(1, r_pathTracingSceneBoundsOverlay.GetInteger())
            : r_pathTracingSceneBoundsOverlay.GetInteger();
        boundsOverlayMax = Max(0, r_pathTracingSceneBoundsOverlayMax.GetInteger());
    }

    std::unordered_set<uint64> sceneUniverseLegacyKeys;
    {
        OPTICK_EVENT("PT Capture Dynamic Static Keys");
        BuildSceneUniverseLegacyKeySet(sceneUniverse, sceneUniverseLegacyKeys);
    }

    std::vector<PathTraceSmokeVertex> bucketVertexData[RT_SMOKE_CLASS_COUNT];
    std::vector<uint32_t> bucketIndexData[RT_SMOKE_CLASS_COUNT];
    std::vector<uint32_t> bucketTriangleClassData[RT_SMOKE_CLASS_COUNT];
    std::vector<uint32_t> bucketTriangleMaterialData[RT_SMOKE_CLASS_COUNT];
    std::vector<uint32_t> bucketTriangleInstanceData[RT_SMOKE_CLASS_COUNT];
    std::vector<uint32_t> bucketTriangleIdentityData[RT_SMOKE_CLASS_COUNT];
    {
        OPTICK_EVENT("PT Capture Dynamic Bucket Reserve");
        for (int bucketIndex = 0; bucketIndex < RT_SMOKE_CLASS_COUNT; ++bucketIndex)
        {
            bucketVertexData[bucketIndex].reserve(RT_SMOKE_INITIAL_RESERVE_VERTS / RT_SMOKE_CLASS_COUNT);
            bucketIndexData[bucketIndex].reserve(RT_SMOKE_INITIAL_RESERVE_INDEXES / RT_SMOKE_CLASS_COUNT);
            bucketTriangleClassData[bucketIndex].reserve(RT_SMOKE_INITIAL_RESERVE_INDEXES / (3 * RT_SMOKE_CLASS_COUNT));
            bucketTriangleMaterialData[bucketIndex].reserve(RT_SMOKE_INITIAL_RESERVE_INDEXES / (3 * RT_SMOKE_CLASS_COUNT));
            bucketTriangleInstanceData[bucketIndex].reserve(RT_SMOKE_INITIAL_RESERVE_INDEXES / (3 * RT_SMOKE_CLASS_COUNT));
            bucketTriangleIdentityData[bucketIndex].reserve(RT_SMOKE_INITIAL_RESERVE_INDEXES / (3 * RT_SMOKE_CLASS_COUNT));
        }
    }

    uint64 dynamicAdmissionBytes = 0;
    uint64 dynamicAdmissionSurfaces = 0;
    const RtSmokeGeometryAdmissionBudget dynamicAdmissionBudget =
        BuildSmokeDynamicGeometryAdmissionBudget();
    int skippedRoutedRigidDynamicSurfaces = 0;
    int skippedRoutedRigidDynamicIndexes = 0;
    int skippedRoutedRigidDynamicByInstance = 0;
    int routedRigidDynamicTested = 0;
    int routedRigidDynamicPromotedEmissive = 0;
    int routedRigidDynamicReadyByMesh = 0;
    int routedRigidDynamicReadyByResident = 0;
    const int requestedDebugMode = NormalizePathTraceDebugMode(idMath::ClampInt(0, 58, r_pathTracingDebugMode.GetInteger()));
    const bool routeMode18 = requestedDebugMode == 18 && r_pathTracingRigidRouteMode18.GetInteger() != 0;
    const bool routeResidencyV2Mode =
        r_pathTracingGeometryResidencyV2.GetInteger() != 0 &&
        r_pathTracingRigidResidency.GetInteger() != 0;
    const bool removeRoutedRigidDynamic =
        (routeResidencyV2Mode || PathTraceDebugModeRemovesRoutedRigidDynamic(requestedDebugMode) || routeMode18) &&
        r_pathTracingRigidRouteRemoveDynamic.GetInteger() != 0 &&
        r_pathTracingRigidTlasRoute.GetInteger() != 0 &&
        r_pathTracingRigidBlasGpuScaffold.GetInteger() != 0 &&
        r_pathTracingRigidBlasGpuBuild.GetInteger() != 0;
    const bool skinnedCaptureSplitGate =
        SmokeSkinnedCaptureSplitGateEnabled(
            skinnedCaptureAdmissionRoutes != nullptr);
    captureTiming.skinnedCaptureAdmissionRoutes =
        skinnedCaptureAdmissionRoutes != nullptr
            ? static_cast<int>(
                skinnedCaptureAdmissionRoutes->size())
            : 0;

    alignas(RtSmokeR1Audit) byte r1AuditStorage[sizeof(RtSmokeR1Audit)];
    RtSmokeR1Audit* r1Audit = nullptr;
#if USE_OPTICK
    alignas(Optick::Event) byte r1ScopeStorage[sizeof(Optick::Event)];
    Optick::Event* r1Scope = nullptr;
#endif
    if (r1Requested && !viewDef->isSubview)
    {
        r1Audit = new (r1AuditStorage) RtSmokeR1Audit{};
        r1Audit->available = viewDef->numDrawSurfs >= 0 ? 1 : 0;
        r1Audit->frameCount = tr.frameCount;
        r1Audit->vertexCacheFrame = vertexCache.currentFrame;
        r1Audit->surfaceCount = viewDef->numDrawSurfs;
        r1Audit->startUs = Sys_Microseconds();
#if USE_OPTICK
        static Optick::EventDescription* r1Description = nullptr;
        if (r1Description == nullptr)
        {
            r1Description = Optick::CreateDescription(
                __FUNCTION__, __FILE__, __LINE__, "PT Capture Fix-N-Observe R1");
        }
        r1Scope = new (r1ScopeStorage) Optick::Event(*r1Description);
#endif
    }
    if (ownerHarvest)
    {
        ownerHarvest->surfaces.assign(
            static_cast<std::size_t>(viewDef->numDrawSurfs),
            RtPathTraceOwnerHarvestSurface{});
    }

    const auto captureTerminalForR1 = [](RtSmokeR1Terminal terminal)
    {
        switch (terminal)
        {
            case RtSmokeR1Terminal::ApplyGateSkip:
                return RtPathTraceCaptureTerminal::ApplyGateSkip;
            case RtSmokeR1Terminal::StaticWorld:
            case RtSmokeR1Terminal::StaticMatch:
                return RtPathTraceCaptureTerminal::StaticMatched;
            case RtSmokeR1Terminal::RigidRouteReadyRemoved:
                return RtPathTraceCaptureTerminal::RoutedRigidReady;
            case RtSmokeR1Terminal::SkinnedCaptureOmitted:
            case RtSmokeR1Terminal::AdmissionRejectedPreAppend:
                return RtPathTraceCaptureTerminal::AdmissionRejected;
            case RtSmokeR1Terminal::AppendEmpty:
            case RtSmokeR1Terminal::AdmissionRollbackPostAppend:
                return RtPathTraceCaptureTerminal::RolledBack;
            case RtSmokeR1Terminal::Accepted:
                return RtPathTraceCaptureTerminal::Accepted;
            default:
                return RtPathTraceCaptureTerminal::SemanticRejected;
        }
    };

#define RT_SMOKE_R1_FINALIZE(terminalValue) \
    do { \
        const RtSmokeR1Terminal rtSmokeR1TerminalValue = (terminalValue); \
        serialCaptureDecision.terminal = \
            captureTerminalForR1(rtSmokeR1TerminalValue); \
        FinalizePathTraceCaptureSurfaceDecision(serialCaptureDecision); \
        NotePathTraceCaptureSerialDecision( \
            static_cast<std::uint32_t>(surfaceIndex), \
            serialCaptureDecision, serialCaptureDecision.decisionPresence); \
        if (ownerHarvest) { \
            RtPathTraceOwnerHarvestSurface& harvestSurface = \
                ownerHarvest->surfaces[static_cast<std::size_t>(surfaceIndex)]; \
            harvestSurface.decision = serialCaptureDecision; \
            harvestSurface.finalized = true; \
        } \
        if (r1Audit) { RtSmokeR1Finalize(*r1Audit, surfaceIndex, rtSmokeR1TerminalValue); } \
    } while (0)

    const RtPathTraceMaterialClassifyProduct* materialClassifyProduct = nullptr;
    bool useMaterialClassifyProduct = false;
    {
        OPTICK_EVENT("PT Material Classify Consume");
        materialClassifyProduct = viewDef->pathTraceMaterialClassifyProduct;
        useMaterialClassifyProduct =
            RtPathTraceMaterialClassifyProductShapeValid(
                materialClassifyProduct, viewDef, viewDef->numDrawSurfs);
    }
    RtPathTraceMaterialClassifyParityStats materialClassifyParity;
    const bool compareMaterialClassifyParity =
        useMaterialClassifyProduct &&
        materialClassifyProduct->parityRequested;
    if (compareMaterialClassifyParity)
    {
        RecordPathTraceMaterialClassifyMappingParity(
            materialClassifyParity,
            *materialClassifyProduct,
            viewDef);
    }
    {
        OPTICK_EVENT("PT Capture Dynamic Surface Loop");
        for (int surfaceIndex = 0; surfaceIndex < viewDef->numDrawSurfs; ++surfaceIndex)
        {
            RtPathTraceCaptureSurfaceProduct serialCaptureDecision;
            serialCaptureDecision.ordinal =
                static_cast<std::uint32_t>(surfaceIndex);
            serialCaptureDecision.sourceState =
                PathTraceCaptureSerialOracleSourceState(
                    static_cast<std::uint32_t>(surfaceIndex));
            SetPathTraceCaptureSerialOracleSurface(
                static_cast<std::uint32_t>(surfaceIndex));
            serialCaptureDecision.decisionPresence =
                RT_PT_CAPTURE_DECISION_FILTER;
            RtPathTraceDrawSurfMirrorSurfaceCache* cachedSurface =
                surfaceCache ? &(*surfaceCache)[surfaceIndex] : nullptr;
            const drawSurf_t* drawSurf = viewDef->drawSurfs[surfaceIndex];
            const srfTriangles_t* tri = nullptr;
            RtPathTraceOwnerHarvestSurface* harvestSurface = ownerHarvest
                ? &ownerHarvest->surfaces[static_cast<std::size_t>(surfaceIndex)]
                : nullptr;
            if (harvestSurface)
            {
                harvestSurface->drawSurf = drawSurf;
            }
            {
                OPTICK_EVENT("PT Dynamic Validate And Filter");
            if (cachedSurface)
            {
                cachedSurface->drawSurf = drawSurf;
            }
            if (PtCpuProducerApplyGate::ShouldSkipDrawSurf(drawSurf))
            {
                serialCaptureDecision.applyGateSkip = true;
                RT_SMOKE_R1_FINALIZE(RtSmokeR1Terminal::ApplyGateSkip);
                continue;
            }
            const int validationStartMs = Sys_Milliseconds();
            RtSmokeSurfaceSkipStats surfaceSkipStats;
            RtSmokeSurfaceSkipStats* validationSkipStats = (cachedSurface || instanceUniverse) ? &surfaceSkipStats : &skipStats;
            RtSmokeR1CacheValidationObservation* r1ValidationObservationPtr =
                r1Audit ? &r1Audit->rowObservation : nullptr;
            if (!ValidateSmokeDrawSurface(
                    viewDef, drawSurf, tri, validationSkipStats,
                    r1ValidationObservationPtr))
            {
                serialCaptureDecision.terminal =
                    RtPathTraceCaptureTerminal::SafetyRejected;
                FinalizePathTraceCaptureSurfaceDecision(serialCaptureDecision);
                NotePathTraceCaptureSerialDecision(
                    static_cast<std::uint32_t>(surfaceIndex),
                    serialCaptureDecision,
                    serialCaptureDecision.decisionPresence);
                if (harvestSurface)
                {
                    harvestSurface->tri = tri;
                    harvestSurface->decision = serialCaptureDecision;
                    harvestSurface->finalized = true;
                }
                if (r1Audit)
                {
                    RtSmokeR1ObserveCacheSample(
                        *r1Audit, surfaceIndex, *r1ValidationObservationPtr);
                    if (r1ValidationObservationPtr->disposition ==
                        RtSmokeR1ValidationDisposition::ConditionedOff)
                    {
                        const srfTriangles_t* conditionedTri =
                            drawSurf ? drawSurf->frontEndGeo : nullptr;
                        if (conditionedTri)
                        {
                            const RtSmokeCacheHandle selectedAmbient =
                                SelectSmokeDiagnosticCacheHandle(
                                    drawSurf->ambientCache,
                                    conditionedTri->ambientCache);
                            const RtSmokeCacheHandle selectedIndex =
                                SelectSmokeDiagnosticCacheHandle(
                                    drawSurf->indexCache,
                                    conditionedTri->indexCache);
                            RtSmokeR1ObserveConditionedOff(
                                *r1Audit,
                                surfaceIndex,
                                selectedAmbient,
                                selectedIndex);
                        }
                    }
                    RtSmokeR1Finalize(
                        *r1Audit,
                        surfaceIndex,
                        RtSmokeR1TerminalForValidation(
                            r1ValidationObservationPtr->disposition));
                }
                captureTiming.validationMs += Sys_Milliseconds() - validationStartMs;
                if (cachedSurface || instanceUniverse)
                {
                    AddSmokeSurfaceSkipStats(skipStats, surfaceSkipStats);
                }
                if (cachedSurface)
                {
                    cachedSurface->skipStats = surfaceSkipStats;
                }
                if (instanceUniverse)
                {
                    instanceUniverse->RecordSkippedDrawSurf(surfaceSkipStats);
                }
                continue;
            }
            captureTiming.validationMs += Sys_Milliseconds() - validationStartMs;
            if (harvestSurface)
            {
                harvestSurface->tri = tri;
            }
            // Harvest publishes source cardinality before any semantic terminal;
            // Lane A later owns only emitted geometry/cardinality.
            serialCaptureDecision.vertexCount = static_cast<std::uint32_t>(
                Max(0, tri->numVerts));
            serialCaptureDecision.indexCount = static_cast<std::uint32_t>(
                Max(0, drawSurf->numIndexes));
            serialCaptureDecision.triangleCount =
                serialCaptureDecision.indexCount / 3u;
            if (r1Audit)
            {
                RtSmokeR1ObserveCacheSample(
                    *r1Audit, surfaceIndex, *r1ValidationObservationPtr);
            }

            const RtPathTraceParticleSurfaceRoute compositeRoute =
                PathTraceParticleCompositeSurfaceRoute(viewDef, drawSurf, tri);
            serialCaptureDecision.compositeRoute =
                static_cast<std::uint32_t>(compositeRoute);
            if (compositeRoute == RtPathTraceParticleSurfaceRoute::CompositeOnly)
            {
                RT_SMOKE_R1_FINALIZE(RtSmokeR1Terminal::CompositeOnly);
                continue;
            }
            if (UnifiedPtDiagnosticRemovesAlphaClipSurface(drawSurf->material))
            {
                serialCaptureDecision.alphaDiagnosticRejected = true;
                ++skipStats.alphaClipDiagnostic;
                RT_SMOKE_R1_FINALIZE(RtSmokeR1Terminal::AlphaClipDiagnostic);
                continue;
            }
            }

            const int classifyStartMs = Sys_Milliseconds();
            RtSmokeSurfaceClass classifiedSurfaceClass;
            {
                OPTICK_EVENT("PT Merged Dynamic Classify Surface");
                classifiedSurfaceClass = ClassifySmokeSurface(viewDef, drawSurf, tri);
            }
            RtSmokeSurfaceClass surfaceClass;
            const idMaterial* material;
            RtSmokeTranslucentSubtype translucentSubtype;
            uint32_t surfaceClassId;
            uint32_t baseMaterialId;
            uint32_t materialId;
            uint32_t materialClassSignature;
            uint64 legacyStaticKey;
            uint32_t sourceFlags;
            {
                OPTICK_EVENT("PT Dynamic Route Identity");
            const RtSmokeTranslucentClassifierInfo* routeClassifier;
            {
                OPTICK_EVENT("PT Dynamic Identity Scalar Derive");
            routeClassifier =
                PathTraceMaterialClassifierForSurface(
                    materialClassifyProduct, surfaceIndex);
            surfaceClass = PtMirrorEffectiveSurfaceClass(
                drawSurf, tri, classifiedSurfaceClass, routeClassifier);
            serialCaptureDecision.liquidPoolPromoted =
                PtMirrorCanPromoteRigidLiquidPoolCard(
                    drawSurf, tri, classifiedSurfaceClass);
            captureTiming.dynamicPassClassifyMs += Sys_Milliseconds() - classifyStartMs;
            material = drawSurf ? drawSurf->material : nullptr;
            translucentSubtype = surfaceClass == RtSmokeSurfaceClass::ParticleAlpha
                ? (useMaterialClassifyProduct
                    ? ClassifySmokeTranslucentSubtype(
                        drawSurf, *routeClassifier)
                    : ClassifySmokeTranslucentSubtype(drawSurf))
                : RtSmokeTranslucentSubtype::Unknown;
            surfaceClassId = SmokeSurfaceClassAndSubtypeId(surfaceClass, translucentSubtype);
            baseMaterialId = SmokeMaterialId(material);
            materialId = SmokeRuntimeMaterialTableIdForDrawSurf(drawSurf, baseMaterialId);
            materialClassSignature = useMaterialClassifyProduct
                ? SmokeMaterialRouteClassSignature(
                    material,
                    surfaceClass,
                    translucentSubtype,
                    *routeClassifier)
                : SmokeMaterialRouteClassSignature(
                    material, surfaceClass, translucentSubtype);
            if (compareMaterialClassifyParity)
            {
                RecordPathTraceMaterialClassifyParity(
                    materialClassifyParity,
                    surfaceIndex,
                    drawSurf,
                    tri,
                    classifiedSurfaceClass,
                    surfaceClass,
                    translucentSubtype,
                    materialClassSignature,
                    *routeClassifier);
            }
            legacyStaticKey = BuildSmokeStaticSurfaceKeyForDiagnostics(drawSurf, tri);
            sourceFlags = PtSourceFlagsForDrawSurf(viewDef, drawSurf, tri, surfaceClass);
            NotePathTraceCaptureSerialDerivedSurface(
                static_cast<std::uint32_t>(surfaceIndex),
                surfaceClass,
                translucentSubtype,
                surfaceClassId,
                materialId,
                materialClassSignature);
            serialCaptureDecision.decisionPresence |=
                RT_PT_CAPTURE_DECISION_ROUTE |
                RT_PT_CAPTURE_DECISION_IDENTITY;
            serialCaptureDecision.surfaceClass = surfaceClass;
            serialCaptureDecision.translucentSubtype = translucentSubtype;
            serialCaptureDecision.surfaceClassId = surfaceClassId;
            serialCaptureDecision.materialId = materialId;
            serialCaptureDecision.materialClassSignature =
                materialClassSignature;
            if (harvestSurface)
            {
                harvestSurface->baseMaterialId = baseMaterialId;
                harvestSurface->legacyStaticKey = legacyStaticKey;
            }
            }
            {
                OPTICK_EVENT("PT Dynamic Identity Static Membership");
            if (!sceneUniverseLegacyKeys.empty() && sceneUniverseLegacyKeys.find(legacyStaticKey) != sceneUniverseLegacyKeys.end())
            {
                sourceFlags |= RT_PT_INSTANCE_SOURCE_STATIC_UNIVERSE_MATCH;
            }
            if (geometryUniverse && geometryUniverse->HasStaticSurface(legacyStaticKey))
            {
                sourceFlags |= RT_PT_INSTANCE_SOURCE_STATIC_CACHE_MATCH;
            }
            serialCaptureDecision.sourceFlags = sourceFlags;
            serialCaptureDecision.staticMatch =
                (sourceFlags & (RT_PT_INSTANCE_SOURCE_STATIC_UNIVERSE_MATCH |
                    RT_PT_INSTANCE_SOURCE_STATIC_CACHE_MATCH)) != 0;
            }
            {
                OPTICK_EVENT("PT Dynamic Identity Cache Record");
            if (cachedSurface)
            {
                cachedSurface->valid = true;
                cachedSurface->tri = tri;
                cachedSurface->classifiedSurfaceClass = classifiedSurfaceClass;
                cachedSurface->surfaceClass = surfaceClass;
                cachedSurface->translucentSubtype = translucentSubtype;
                cachedSurface->legacyStaticKey = legacyStaticKey;
                cachedSurface->baseMaterialId = baseMaterialId;
                cachedSurface->materialId = materialId;
                cachedSurface->sourceKind = SmokeSurfaceClassId(surfaceClass);
                cachedSurface->sourceFlags = sourceFlags;
                cachedSurface->surfaceClassId = surfaceClassId;
                cachedSurface->surfaceClassAndFlags = surfaceClassId |
                    ((useMaterialClassifyProduct
                        ? SmokeDrawSurfaceHasActiveEmissiveStage(
                            drawSurf, *routeClassifier)
                        : SmokeDrawSurfaceHasActiveEmissiveStage(drawSurf))
                        ? 0u
                        : RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF);
                cachedSurface->materialClassSignature = materialClassSignature;
            }
            }
            {
                OPTICK_EVENT("PT Dynamic Identity Instance Observation");
            const bool recordInstanceObservation =
                instanceUniverse &&
                (recordAllInstanceClasses || RtPathTraceSourceFlagsAreDurableRigid(sourceFlags));
            if (recordInstanceObservation)
            {
                RecordPathTraceDrawSurfMirrorObservation(
                    viewDef,
                    geometryUniverse,
                    *instanceUniverse,
                    boundsOverlayLines,
                    boundsOverlayMode,
                    boundsOverlayMax,
                    boundsOverlayDrawn,
                    surfaceIndex,
                    drawSurf,
                    tri,
                    surfaceClass,
                    material,
                    materialId,
                    SmokeSurfaceClassId(surfaceClass),
                    sourceFlags,
                    surfaceClassId,
                    cachedSurface
                        ? cachedSurface->surfaceClassAndFlags
                        : (surfaceClassId |
                            ((useMaterialClassifyProduct
                                ? SmokeDrawSurfaceHasActiveEmissiveStage(
                                    drawSurf, *routeClassifier)
                                : SmokeDrawSurfaceHasActiveEmissiveStage(drawSurf))
                                ? 0u
                                : RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF)),
                    materialClassSignature,
                    deferredRigidCandidates);
            }
            }
            {
                OPTICK_EVENT("PT Dynamic Identity Static Decision");
            if (surfaceClass == RtSmokeSurfaceClass::StaticWorld)
            {
                RT_SMOKE_R1_FINALIZE(RtSmokeR1Terminal::StaticWorld);
                continue;
            }

            if (DrawSurfMirrorIsStaticMatch(geometryUniverse, sceneUniverseLegacyKeys, legacyStaticKey))
            {
                NotePathTraceCaptureSerialStaticMembership(
                    static_cast<std::uint32_t>(surfaceIndex));
                RT_SMOKE_R1_FINALIZE(RtSmokeR1Terminal::StaticMatch);
                continue;
            }
            }
            {
                OPTICK_EVENT("PT Dynamic Identity Rigid Route Probe");
            if (removeRoutedRigidDynamic && surfaceClass == RtSmokeSurfaceClass::RigidEntity && geometryUniverse)
            {
                ++routedRigidDynamicTested;
                const viewEntity_t* space = drawSurf ? drawSurf->space : nullptr;
                const idRenderEntityLocal* entity = space ? space->entityDef : nullptr;
                const renderEntity_t* renderEntity = entity ? &entity->parms : nullptr;
                const idRenderModel* renderModel = renderEntity ? renderEntity->hModel : nullptr;
                const PtRenderDefKey renderDefKey = PtGeometryLifecycle::MakeEntityKey(entity);
                const uint32_t modelEpoch = (renderDefKey.world && renderDefKey.index >= 0)
                    ? PtGeometryLifecycle::EntityModelEpoch(renderDefKey.world, renderDefKey.index)
                    : 0;

                RtPathTraceMeshKey meshKey;
                FillPathTraceRigidRouteMeshKey(
                    meshKey, tri, materialId, materialClassSignature, SmokeSurfaceClassId(surfaceClass));
                const RtPathTraceRigidInstanceSnapshot rigidSnapshot = BuildPathTraceRigidInstanceSnapshot(
                    meshKey,
                    renderModel,
                    tri,
                    renderDefKey,
                    modelEpoch,
                    entity ? entity->index : -1,
                    renderEntity ? renderEntity->entityNum : -1,
                    drawSurf ? drawSurf->modelSurfaceIndex : -1,
                    sourceFlags);
                serialCaptureDecision.meshHash = rigidSnapshot.meshHash;
                serialCaptureDecision.instanceId = rigidSnapshot.instanceId;
                geometryUniverse->RecordA8S1RouteObservation(
                    rigidSnapshot.renderDefKey,
                    rigidSnapshot.modelSurfaceIndex,
                    rigidSnapshot.modelSurfaceIndexValid,
                    rigidSnapshot.meshHash,
                    renderModel ? renderModel->Name() : "<none>",
                    PtA8S1RouteProducer::RoutedReadyProbe);
                const uint64 meshHash = rigidSnapshot.meshHash;
                const bool routeReadyByMesh = geometryUniverse->IsRigidRouteReady(meshHash);
                serialCaptureDecision.rigidReadyByMesh = routeReadyByMesh;
                const bool promotedEmissive =
                    PtMirrorCanPromoteRigidEmissiveCard(
                        drawSurf,
                        tri,
                        classifiedSurfaceClass,
                        routeClassifier);
                if (promotedEmissive)
                {
                    ++routedRigidDynamicPromotedEmissive;
                }
                const bool routeReadyByResident =
                    !routeReadyByMesh &&
                    promotedEmissive &&
                    geometryUniverse->IsRigidRouteResidentReadyForEntityMaterial(
                        entity ? entity->index : -1,
                        renderEntity ? renderEntity->entityNum : -1,
                        materialId);
                serialCaptureDecision.rigidReadyByResident = routeReadyByResident;
                if (routeReadyByMesh || routeReadyByResident)
                {
                    NotePathTraceCaptureSerialRoutedReady(
                        static_cast<std::uint32_t>(surfaceIndex),
                        static_cast<std::uint32_t>(Max(0, tri->numVerts)),
                        static_cast<std::uint32_t>(Max(0, tri->numIndexes)));
                    if (routeReadyByMesh)
                    {
                        ++routedRigidDynamicReadyByMesh;
                    }
                    if (routeReadyByResident)
                    {
                        ++routedRigidDynamicReadyByResident;
                    }
                    ++skippedRoutedRigidDynamicSurfaces;
                    skippedRoutedRigidDynamicIndexes += tri->numIndexes;
                    if (routeReadyByResident)
                    {
                        ++skippedRoutedRigidDynamicByInstance;
                    }
                    if (rigidCaptureSkips)
                    {
                        RtSmokeRigidCaptureSkipRecord skip;
                        skip.instanceId = rigidSnapshot.instanceId;
                        skip.entityIndex = entity ? entity->index : -1;
                        skip.modelSurfaceIndex = drawSurf ? drawSurf->modelSurfaceIndex : -1;
                        skip.materialId = materialId;
                        skip.meshHash = meshHash;
                        skip.surfaceClassId = surfaceClassId;
                        rigidCaptureSkips->push_back(skip);
                    }
                    AddSmokeDynamicMaterialEvalStatsForMaterialId(materialStats, drawSurf, tri->numIndexes, materialId);
                    RT_SMOKE_R1_FINALIZE(RtSmokeR1Terminal::RigidRouteReadyRemoved);
                    continue;
                }
            }
            }
            }

            {
                OPTICK_EVENT("PT Dynamic Skinned Admission");
            if (surfaceClass ==
                    RtSmokeSurfaceClass::SkinnedDeformed &&
                skinnedSurfaceRecords != nullptr)
            {
                std::vector<RtSmokeSkinnedSurfaceRecord>
                    provisionalRecords;
                provisionalRecords.reserve(1);
                AddSmokeSkinnedSurfaceRecord(
                    &provisionalRecords,
                    drawSurf,
                    tri,
                    surfaceClassId,
                    materialId,
                    surfaceIndex,
                    -1,
                    -1,
                    -1,
                    -1,
                    tri->numVerts,
                    tri->numIndexes,
                    tri->numIndexes / 3);
                if (!provisionalRecords.empty())
                {
                    RtSmokeSkinnedSurfaceRecord& provisional =
                        provisionalRecords.front();
                    const PtSkinnedHitRouteRecord* priorRoute =
                        nullptr;
                    if (skinnedCaptureAdmissionRoutes != nullptr)
                    {
                        for (const PtSkinnedHitRouteRecord& route :
                            *skinnedCaptureAdmissionRoutes)
                        {
                            if (route.instanceKey ==
                                provisional.canonicalInstance)
                            {
                                priorRoute = &route;
                                break;
                            }
                        }
                    }
                    const PtGeometryIdentityBinding* binding =
                        geometryUniverse != nullptr
                            ? geometryUniverse->
                                FindCanonicalIdentityBinding(
                                    provisional.
                                        canonicalInstance)
                            : nullptr;
                    const PtGeometrySourceRecord* source =
                        binding != nullptr &&
                            geometryUniverse != nullptr
                            ? geometryUniverse->
                                FindCanonicalSourceRecord(
                                    binding->meshKey)
                            : nullptr;
                    PtSkinnedCaptureAdmissionInput admission;
                    admission.gate = skinnedCaptureSplitGate;
                    admission.currentInstance =
                        provisional.canonicalInstance;
                    if (binding != nullptr)
                    {
                        admission.currentMesh =
                            binding->meshKey;
                    }
                    if (source != nullptr)
                    {
                        admission.currentSourceChecksum =
                            source->sourceChecksum;
                        admission.currentVertexCount =
                            static_cast<uint32_t>(
                                source->payload.positions.
                                    size());
                        admission.currentIndexCount =
                            static_cast<uint32_t>(
                                source->payload.indexes.
                                    size());
                    }
                    admission.jointDataReady =
                        provisional.rtCpuSkinned &&
                        provisional.jointCount > 0 &&
                        provisional.jointSource != 0;
                    admission.priorRouteLive =
                        priorRoute != nullptr;
                    admission.priorRoute = priorRoute;
                    const PtSkinnedCaptureAdmissionResult
                        admissionResult =
                            PtPlanSkinnedCaptureAdmission(
                                admission);
                    serialCaptureDecision.decisionPresence |=
                        RT_PT_CAPTURE_DECISION_SKINNED;
                    serialCaptureDecision.skinnedAdmission =
                        static_cast<std::uint32_t>(admissionResult);
                    if (admissionResult ==
                        PtSkinnedCaptureAdmissionResult::
                            OmitCpuCapture)
                    {
                        provisional.cpuCaptureOmitted = true;
                        provisional.currentVertexOffset = -1;
                        provisional.currentIndexOffset = -1;
                        provisional.currentTriangleOffset = -1;
                        provisional.vertexCount =
                            static_cast<int>(
                                admission.currentVertexCount);
                        provisional.indexCount =
                            static_cast<int>(
                                admission.currentIndexCount);
                        provisional.triangleCount =
                            provisional.indexCount / 3;
                        provisional.bucketIndex = -1;
                        skinnedSurfaceRecords->push_back(
                            provisional);
                        serialCaptureDecision.skinnedCaptureOmitted = true;
                        serialCaptureDecision.decisionPresence |=
                            RT_PT_CAPTURE_DECISION_SKINNED_RECORD;
                        serialCaptureDecision.skinnedRecordPresent = true;
                        serialCaptureDecision.skinnedRecordVertexCount =
                            static_cast<std::uint32_t>(Max(0, provisional.vertexCount));
                        serialCaptureDecision.skinnedRecordIndexCount =
                            static_cast<std::uint32_t>(Max(0, provisional.indexCount));
                        serialCaptureDecision.skinnedRecordTriangleCount =
                            static_cast<std::uint32_t>(Max(0, provisional.triangleCount));

                        const int sourceIndexCount =
                            provisional.indexCount;
                        AddMirrorMaterialStats(
                            materialStats,
                            drawSurf->material,
                            sourceIndexCount,
                            surfaceClass,
                            translucentSubtype);
                        AddSmokeDynamicMaterialEvalStatsForMaterialId(
                            materialStats,
                            drawSurf,
                            sourceIndexCount,
                            materialId);
                        ++sourceSurfaces;
                        sourceVerts += provisional.vertexCount;
                        sourceIndexes += sourceIndexCount;
                        AddMirrorSurfaceClassStats(
                            classStats,
                            surfaceClass,
                            provisional.vertexCount,
                            sourceIndexCount);
                        AddMirrorDynamicGeometryStats(
                            dynamicStats,
                            surfaceClass,
                            drawSurf,
                            tri,
                            sourceIndexCount);
                        ++captureTiming.
                            skinnedCaptureOmittedSurfaces;
                        captureTiming.
                            skinnedCaptureOmittedVerts +=
                                provisional.vertexCount;
                        captureTiming.
                            skinnedCaptureOmittedIndexes +=
                                sourceIndexCount;
                        RT_SMOKE_R1_FINALIZE(RtSmokeR1Terminal::SkinnedCaptureOmitted);
                        continue;
                    }

                    switch (admissionResult)
                    {
                        case PtSkinnedCaptureAdmissionResult::
                            GateDisabled:
                            ++captureTiming.
                                skinnedCaptureFallbackGate;
                            break;
                        case PtSkinnedCaptureAdmissionResult::
                            MissingPriorRoute:
                        case PtSkinnedCaptureAdmissionResult::
                            PriorRouteNotLive:
                            ++captureTiming.
                                skinnedCaptureFallbackPriorRoute;
                            break;
                        case PtSkinnedCaptureAdmissionResult::
                            JointDataNotReady:
                            ++captureTiming.
                                skinnedCaptureFallbackJointData;
                            break;
                        default:
                            ++captureTiming.
                                skinnedCaptureFallbackCurrentContract;
                            break;
                    }
                }
            }

            const RtSmokeGeometryAdmissionPlan admissionPlan =
                PlanSmokeDynamicGeometryAdmission(
                    dynamicAdmissionBudget,
                    dynamicAdmissionBytes,
                    dynamicAdmissionSurfaces,
                    tri->numVerts,
                    tri->numIndexes);
            serialCaptureDecision.decisionPresence |=
                RT_PT_CAPTURE_DECISION_ADMISSION;
            serialCaptureDecision.admissionPrecheckPassed =
                admissionPlan.Admitted();
            if (!admissionPlan.Admitted())
            {
                RecordSmokeGeometryAdmissionRejection(
                    skipStats, admissionPlan);
                RT_SMOKE_R1_FINALIZE(RtSmokeR1Terminal::AdmissionRejectedPreAppend);
                continue;
            }
            if (ownerHarvest)
            {
                dynamicAdmissionBytes = admissionPlan.totalBytes;
                dynamicAdmissionSurfaces = admissionPlan.totalSurfaces;
                const int harvestBucketIndex = idMath::ClampInt(
                    0, RT_SMOKE_CLASS_COUNT - 1,
                    static_cast<int>(surfaceClassId &
                        RT_SMOKE_TRIANGLE_CLASS_MASK));
                serialCaptureDecision.decisionPresence |=
                    RT_PT_CAPTURE_DECISION_APPEND |
                    RT_PT_CAPTURE_DECISION_CAPTURED_RECORD |
                    RT_PT_CAPTURE_DECISION_BUCKET_PUBLICATION;
                serialCaptureDecision.appendAttempted = true;
                serialCaptureDecision.bucketIndex = harvestBucketIndex;
                serialCaptureDecision.vertexCount =
                    static_cast<std::uint32_t>(Max(0, tri->numVerts));
                serialCaptureDecision.indexCount =
                    static_cast<std::uint32_t>(Max(0, drawSurf->numIndexes));
                serialCaptureDecision.triangleCount =
                    serialCaptureDecision.indexCount / 3u;
                serialCaptureDecision.capturedRecordPresent =
                    capturedSurfaceRecords != nullptr;
                serialCaptureDecision.capturedRecordVertexCount =
                    serialCaptureDecision.vertexCount;
                serialCaptureDecision.capturedRecordIndexCount =
                    serialCaptureDecision.indexCount;
                serialCaptureDecision.capturedRecordTriangleCount =
                    serialCaptureDecision.triangleCount;
                serialCaptureDecision.bucketRangePublished = true;
                if (surfaceClass == RtSmokeSurfaceClass::SkinnedDeformed)
                {
                    serialCaptureDecision.decisionPresence |=
                        RT_PT_CAPTURE_DECISION_SKINNED_RECORD;
                    serialCaptureDecision.skinnedRecordPresent = true;
                    serialCaptureDecision.skinnedRecordVertexCount =
                        serialCaptureDecision.vertexCount;
                    serialCaptureDecision.skinnedRecordIndexCount =
                        serialCaptureDecision.indexCount;
                    serialCaptureDecision.skinnedRecordTriangleCount =
                        serialCaptureDecision.triangleCount;
                }
                harvestSurface->appendEligible = true;
                harvestSurface->decision = serialCaptureDecision;
                if (rigidCaptureWalked &&
                    surfaceClass == RtSmokeSurfaceClass::RigidEntity &&
                    geometryUniverse)
                {
                    const viewEntity_t* space = drawSurf ? drawSurf->space : nullptr;
                    const idRenderEntityLocal* entity =
                        space ? space->entityDef : nullptr;
                    const renderEntity_t* renderEntity =
                        entity ? &entity->parms : nullptr;
                    const idRenderModel* renderModel =
                        renderEntity ? renderEntity->hModel : nullptr;
                    const PtRenderDefKey renderDefKey =
                        PtGeometryLifecycle::MakeEntityKey(entity);
                    const uint32_t modelEpoch =
                        (renderDefKey.world && renderDefKey.index >= 0)
                            ? PtGeometryLifecycle::EntityModelEpoch(
                                renderDefKey.world, renderDefKey.index)
                            : 0;
                    RtPathTraceMeshKey meshKey;
                    FillPathTraceRigidRouteMeshKey(meshKey, tri, materialId,
                        materialClassSignature, SmokeSurfaceClassId(surfaceClass));
                    const RtPathTraceRigidInstanceSnapshot walkedSnap =
                        BuildAndRecordPathTraceCaptureWalkObservation(
                            geometryUniverse, meshKey, renderModel, tri,
                            renderDefKey, modelEpoch,
                            entity ? entity->index : -1,
                            renderEntity ? renderEntity->entityNum : -1,
                            drawSurf ? drawSurf->modelSurfaceIndex : -1,
                            sourceFlags);
                    harvestSurface->rigidWalkInstanceId = walkedSnap.instanceId;
                }
                if (mergedWalkedRanges && harvestBucketIndex >= 1)
                {
                    const viewEntity_t* mergedSpace =
                        drawSurf ? drawSurf->space : nullptr;
                    const idRenderEntityLocal* mergedEntity =
                        mergedSpace ? mergedSpace->entityDef : nullptr;
                    const idRenderModel* mergedModel =
                        (mergedEntity && mergedEntity->parms.hModel)
                            ? mergedEntity->parms.hModel : nullptr;
                    const modelSurface_t* mergedModelSurface = nullptr;
                    if (mergedModel && drawSurf->modelSurfaceIndex >= 0 &&
                        drawSurf->modelSurfaceIndex < mergedModel->NumSurfaces())
                    {
                        mergedModelSurface = mergedModel->Surface(
                            drawSurf->modelSurfaceIndex);
                    }
                    const RtPtFeedClass feedClass = ClassifyEntityFeedSurface(
                        mergedEntity, mergedModel, mergedModelSurface);
                    const uintptr_t triAddr = reinterpret_cast<uintptr_t>(tri);
                    uint64 mergedKey = 14695981039346656037ull;
                    mergedKey ^= static_cast<uint64>(
                        (mergedEntity ? mergedEntity->index : -1) + 1);
                    mergedKey *= 1099511628211ull;
                    mergedKey ^= static_cast<uint64>(
                        (drawSurf ? drawSurf->modelSurfaceIndex : -1) + 1);
                    mergedKey *= 1099511628211ull;
                    mergedKey ^= static_cast<uint64>(materialId);
                    mergedKey *= 1099511628211ull;
                    mergedKey ^= static_cast<uint64>(triAddr);
                    mergedKey *= 1099511628211ull;
                    mergedKey ^= 0x4d4552474544ull;
                    mergedKey |= (2ull << 62);
                    harvestSurface->mergedRangeId = mergedKey == 0 ? 1 : mergedKey;
                    harvestSurface->feedClass = static_cast<uint32_t>(feedClass);
                    harvestSurface->particle = surfaceClass ==
                        RtSmokeSurfaceClass::ParticleAlpha;
                    harvestSurface->trueDeform =
                        feedClass == RtPtFeedClass::TrueDeform;
                    if (surfaceClass == RtSmokeSurfaceClass::RigidEntity &&
                        geometryUniverse)
                    {
                        const PtRenderDefKey renderDefKey =
                            PtGeometryLifecycle::MakeEntityKey(mergedEntity);
                        const uint32_t modelEpoch =
                            (renderDefKey.world && renderDefKey.index >= 0)
                                ? PtGeometryLifecycle::EntityModelEpoch(
                                    renderDefKey.world, renderDefKey.index)
                                : 0;
                        RtPathTraceMeshKey meshKey;
                        FillPathTraceRigidRouteMeshKey(meshKey, tri, materialId,
                            materialClassSignature,
                            SmokeSurfaceClassId(surfaceClass));
                        const RtPathTraceRigidInstanceSnapshot companionSnap =
                            BuildAndRecordPathTraceMergedCompanionObservation(
                                geometryUniverse, meshKey, mergedModel, tri,
                                renderDefKey, modelEpoch,
                                mergedEntity ? mergedEntity->index : -1,
                                mergedEntity ? mergedEntity->parms.entityNum : -1,
                                drawSurf ? drawSurf->modelSurfaceIndex : -1,
                                sourceFlags);
                        harvestSurface->mergedCompanionRigidId =
                            companionSnap.instanceId;
                    }
                    serialCaptureDecision.decisionPresence |=
                        RT_PT_CAPTURE_DECISION_MERGED_WALK;
                    serialCaptureDecision.mergedWalkRecordPresent = true;
                    harvestSurface->decision = serialCaptureDecision;
                }
                RT_SMOKE_R1_FINALIZE(RtSmokeR1Terminal::Accepted);
                continue;
            }
            }

            const int bucketIndex = idMath::ClampInt(0, RT_SMOKE_CLASS_COUNT - 1, static_cast<int>(surfaceClassId & RT_SMOKE_TRIANGLE_CLASS_MASK));

            std::vector<PathTraceSmokeVertex>& bucketVertices = bucketVertexData[bucketIndex];
            std::vector<uint32_t>& bucketIndexes = bucketIndexData[bucketIndex];
            std::vector<uint32_t>& bucketClasses = bucketTriangleClassData[bucketIndex];
            std::vector<uint32_t>& bucketMaterials = bucketTriangleMaterialData[bucketIndex];
            std::vector<uint32_t>& bucketInstances = bucketTriangleInstanceData[bucketIndex];
            std::vector<uint32_t>& bucketIdentities = bucketTriangleIdentityData[bucketIndex];
            const bool usesRtCpuSkinning = GetSmokeRtCpuSkinningJoints(tri) != nullptr;
            const int bucketVertexStart = static_cast<int>(bucketVertices.size());
            const int bucketIndexStart = static_cast<int>(bucketIndexes.size());
            const int bucketTriangleStart = static_cast<int>(bucketClasses.size());
            serialCaptureDecision.decisionPresence |=
                RT_PT_CAPTURE_DECISION_APPEND;
            serialCaptureDecision.appendAttempted = true;
            serialCaptureDecision.bucketIndex = bucketIndex;
            serialCaptureDecision.preAppendVertexOffset =
                static_cast<std::uint32_t>(Max(0, bucketVertexStart));
            serialCaptureDecision.preAppendIndexOffset =
                static_cast<std::uint32_t>(Max(0, bucketIndexStart));
            serialCaptureDecision.preAppendTriangleOffset =
                static_cast<std::uint32_t>(Max(0, bucketTriangleStart));
            const int attributeClassIndex = idMath::ClampInt(
                0, RT_SMOKE_CLASS_COUNT - 1,
                static_cast<int>(surfaceClassId & RT_SMOKE_TRIANGLE_CLASS_MASK));
            const RtSmokeAttributeClassStats committedAttributeBefore =
                attributeStats.classes[attributeClassIndex];
            const int committedInvalidIndexBefore = skipStats.invalidIndexCount;
            const int committedZeroAreaBefore = skipStats.zeroAreaOnly;
            const int appendStartMs = Sys_Milliseconds();
            const uint64 appendStartUs = Sys_Microseconds();
            const int emittedIndexes = AppendSmokeSurfaceGeometry(
                drawSurf,
                tri,
                surfaceClassId,
                materialId,
                RT_SMOKE_CLASS_COUNT,
                RT_SMOKE_TRIANGLE_CLASS_MASK,
                static_cast<uint32_t>(RtSmokeSurfaceClass::ParticleAlpha),
                RT_SMOKE_TRIANGLE_FORCE_GEOMETRIC_NORMAL,
                bucketVertices,
                bucketIndexes,
                bucketClasses,
                bucketMaterials,
                skipStats,
                attributeStats);
            const int appendMs = Sys_Milliseconds() - appendStartMs;
            const uint64 appendUs =
                Sys_Microseconds() - appendStartUs;
            captureTiming.dynamicAppendMs += appendMs;
            captureTiming.appendMs += appendMs;
            NotePathTraceCaptureSerialAppend(
                static_cast<std::uint32_t>(surfaceIndex),
                skipStats.invalidIndexCount - committedInvalidIndexBefore,
                skipStats.zeroAreaOnly - committedZeroAreaBefore,
                emittedIndexes > 0
                    ? static_cast<std::uint32_t>(emittedIndexes) : 0u);
            serialCaptureDecision.invalidIndexCount =
                skipStats.invalidIndexCount - committedInvalidIndexBefore;
            serialCaptureDecision.zeroAreaTriangleCount =
                skipStats.zeroAreaOnly - committedZeroAreaBefore;
            serialCaptureDecision.indexCount = emittedIndexes > 0
                ? static_cast<std::uint32_t>(emittedIndexes) : 0u;
            serialCaptureDecision.triangleCount =
                serialCaptureDecision.indexCount / 3;
            if (viewDef->pathTraceCommittedGeometryProduct)
            {
                const RtSmokeAttributeClassStats& committedAttributeAfter =
                    attributeStats.classes[attributeClassIndex];
                RtPathTraceCommittedGeometryCounters committedCounters;
                committedCounters.invalidNormalVerts =
                    committedAttributeAfter.invalidNormalVerts -
                    committedAttributeBefore.invalidNormalVerts;
                committedCounters.invalidUvVerts =
                    committedAttributeAfter.invalidUvVerts -
                    committedAttributeBefore.invalidUvVerts;
                committedCounters.invalidNormalTriangles =
                    committedAttributeAfter.invalidNormalTriangles -
                    committedAttributeBefore.invalidNormalTriangles;
                committedCounters.invalidUvTriangles =
                    committedAttributeAfter.invalidUvTriangles -
                    committedAttributeBefore.invalidUvTriangles;
                committedCounters.forcedGeometricNormalTriangles =
                    committedAttributeAfter.forcedGeometricNormalTriangles -
                    committedAttributeBefore.forcedGeometricNormalTriangles;
                committedCounters.invalidIndexCount =
                    skipStats.invalidIndexCount - committedInvalidIndexBefore;
                committedCounters.zeroAreaOnly =
                    skipStats.zeroAreaOnly - committedZeroAreaBefore;
                const std::uint32_t serialVertexCount =
                    static_cast<std::uint32_t>(
                        bucketVertices.size() - bucketVertexStart);
                const std::uint32_t serialIndexCount =
                    static_cast<std::uint32_t>(
                        bucketIndexes.size() - bucketIndexStart);
                ComparePathTraceCommittedDynamicGeometry(
                    viewDef,
                    surfaceIndex,
                    drawSurf,
                    tri,
                    serialVertexCount > 0
                        ? bucketVertices.data() + bucketVertexStart
                        : nullptr,
                    serialVertexCount,
                    serialIndexCount > 0
                        ? bucketIndexes.data() + bucketIndexStart
                        : nullptr,
                    serialIndexCount,
                    static_cast<std::uint32_t>(bucketVertexStart),
                    committedCounters);
            }
            if (usesRtCpuSkinning)
            {
                captureTiming.rtCpuSkinningAppendMs += appendMs;
                captureTiming.rtCpuSkinningAppendUs += appendUs;
            }
            if (emittedIndexes <= 0)
            {
                serialCaptureDecision.rollbackApplied = true;
                RT_SMOKE_R1_FINALIZE(RtSmokeR1Terminal::AppendEmpty);
                continue;
            }
            {
                OPTICK_EVENT("PT Dynamic Post Append Publish");
            if (rigidCaptureWalked &&
                surfaceClass == RtSmokeSurfaceClass::RigidEntity &&
                geometryUniverse)
            {
                const viewEntity_t* space = drawSurf ? drawSurf->space : nullptr;
                const idRenderEntityLocal* entity = space ? space->entityDef : nullptr;
                const renderEntity_t* renderEntity = entity ? &entity->parms : nullptr;
                const idRenderModel* renderModel = renderEntity ? renderEntity->hModel : nullptr;
                const PtRenderDefKey renderDefKey = PtGeometryLifecycle::MakeEntityKey(entity);
                const uint32_t modelEpoch = (renderDefKey.world && renderDefKey.index >= 0)
                    ? PtGeometryLifecycle::EntityModelEpoch(renderDefKey.world, renderDefKey.index)
                    : 0;
                RtPathTraceMeshKey meshKey;
                FillPathTraceRigidRouteMeshKey(
                    meshKey, tri, materialId, materialClassSignature, SmokeSurfaceClassId(surfaceClass));
                const RtPathTraceRigidInstanceSnapshot walkedSnap =
                    BuildAndRecordPathTraceCaptureWalkObservation(
                        geometryUniverse, meshKey, renderModel, tri,
                        renderDefKey, modelEpoch,
                        entity ? entity->index : -1,
                        renderEntity ? renderEntity->entityNum : -1,
                        drawSurf ? drawSurf->modelSurfaceIndex : -1,
                        sourceFlags);
                if (walkedSnap.instanceId != 0)
                {
                    rigidCaptureWalked->push_back(walkedSnap.instanceId);
                    if (rigidCaptureWalkedTriangles)
                    {
                        rigidCaptureWalkedTriangles->push_back(
                            static_cast<uint32_t>(emittedIndexes / 3));
                    }
                }
            }
            const int emittedVertices =
                static_cast<int>(bucketVertices.size()) -
                    bucketVertexStart;
            const RtSmokeGeometryAdmissionPlan actualAdmissionPlan =
                PlanSmokeDynamicGeometryAdmission(
                    dynamicAdmissionBudget,
                    dynamicAdmissionBytes,
                    dynamicAdmissionSurfaces,
                    emittedVertices,
                    emittedIndexes);
            if (!actualAdmissionPlan.Admitted())
            {
                RecordSmokeGeometryAdmissionRejection(
                    skipStats, actualAdmissionPlan);
                bucketVertices.resize(bucketVertexStart);
                bucketIndexes.resize(bucketIndexStart);
                bucketClasses.resize(bucketTriangleStart);
                bucketMaterials.resize(bucketTriangleStart);
                NotePathTraceCaptureSerialAppend(
                    static_cast<std::uint32_t>(surfaceIndex),
                    skipStats.invalidIndexCount - committedInvalidIndexBefore,
                    skipStats.zeroAreaOnly - committedZeroAreaBefore,
                    0u);
                serialCaptureDecision.rollbackApplied = true;
                RT_SMOKE_R1_FINALIZE(RtSmokeR1Terminal::AdmissionRollbackPostAppend);
                continue;
            }
            dynamicAdmissionBytes = actualAdmissionPlan.totalBytes;
            dynamicAdmissionSurfaces =
                actualAdmissionPlan.totalSurfaces;
            serialCaptureDecision.vertexOffset =
                static_cast<std::uint32_t>(Max(0, bucketVertexStart));
            serialCaptureDecision.vertexCount =
                static_cast<std::uint32_t>(Max(0, emittedVertices));
            serialCaptureDecision.indexOffset =
                static_cast<std::uint32_t>(Max(0, bucketIndexStart));
            serialCaptureDecision.indexCount =
                static_cast<std::uint32_t>(Max(0, emittedIndexes));
            serialCaptureDecision.triangleOffset =
                static_cast<std::uint32_t>(Max(0, bucketTriangleStart));
            serialCaptureDecision.triangleCount =
                static_cast<std::uint32_t>(Max(0, emittedIndexes / 3));
            serialCaptureDecision.decisionPresence |=
                RT_PT_CAPTURE_DECISION_CAPTURED_RECORD |
                RT_PT_CAPTURE_DECISION_BUCKET_PUBLICATION;
            serialCaptureDecision.capturedRecordPresent =
                capturedSurfaceRecords != nullptr;
            serialCaptureDecision.capturedRecordVertexCount =
                serialCaptureDecision.vertexCount;
            serialCaptureDecision.capturedRecordIndexCount =
                serialCaptureDecision.indexCount;
            serialCaptureDecision.capturedRecordTriangleCount =
                serialCaptureDecision.triangleCount;
            serialCaptureDecision.bucketRangePublished = true;
            const int entityIndex = (drawSurf->space && drawSurf->space->entityDef) ? drawSurf->space->entityDef->index : -1;
            const uint32_t dynamicInstanceId = static_cast<uint32_t>(Max(1, entityIndex + 1));
            const int emittedTriangles = emittedIndexes / 3;
            bucketInstances.insert(bucketInstances.end(), emittedTriangles, dynamicInstanceId);
            for (int localTriangleIndex = 0; localTriangleIndex < emittedTriangles; ++localTriangleIndex)
            {
                bucketIdentities.push_back(PtDynamicTriangleIdentitySeed(drawSurf, tri, baseMaterialId, static_cast<uint32_t>(localTriangleIndex)));
            }
            if (surfaceClass == RtSmokeSurfaceClass::SkinnedDeformed)
            {
                AddSmokeSkinnedSurfaceRecord(
                    skinnedSurfaceRecords,
                    drawSurf,
                    tri,
                    surfaceClassId,
                    materialId,
                    surfaceIndex,
                    bucketIndex,
                    bucketVertexStart,
                    bucketIndexStart,
                    bucketTriangleStart,
                    static_cast<int>(bucketVertices.size()) - bucketVertexStart,
                    emittedIndexes,
                    emittedIndexes / 3);
                serialCaptureDecision.decisionPresence |=
                    RT_PT_CAPTURE_DECISION_SKINNED_RECORD;
                serialCaptureDecision.skinnedRecordPresent =
                    skinnedSurfaceRecords != nullptr;
                serialCaptureDecision.skinnedRecordVertexCount =
                    serialCaptureDecision.vertexCount;
                serialCaptureDecision.skinnedRecordIndexCount =
                    serialCaptureDecision.indexCount;
                serialCaptureDecision.skinnedRecordTriangleCount =
                    serialCaptureDecision.triangleCount;
            }
            AddSmokeCapturedSurfaceRecord(
                capturedSurfaceRecords,
                drawSurf,
                surfaceClassId,
                materialId,
                surfaceIndex,
                bucketIndex,
                bucketVertexStart,
                bucketIndexStart,
                bucketTriangleStart,
                static_cast<int>(bucketVertices.size()) -
                    bucketVertexStart,
                emittedIndexes,
                emittedIndexes / 3);
            if (mergedWalkedRanges && bucketIndex >= 1)
            {
                const viewEntity_t* mergedSpace = drawSurf ? drawSurf->space : nullptr;
                const idRenderEntityLocal* mergedEntity =
                    mergedSpace ? mergedSpace->entityDef : nullptr;
                const idRenderModel* mergedModel =
                    (mergedEntity && mergedEntity->parms.hModel)
                        ? mergedEntity->parms.hModel
                        : nullptr;
                const modelSurface_t* mergedModelSurface = nullptr;
                if (mergedModel &&
                    drawSurf &&
                    drawSurf->modelSurfaceIndex >= 0 &&
                    drawSurf->modelSurfaceIndex < mergedModel->NumSurfaces())
                {
                    mergedModelSurface = mergedModel->Surface(drawSurf->modelSurfaceIndex);
                }
                const RtPtFeedClass feedClass =
                    ClassifyEntityFeedSurface(mergedEntity, mergedModel, mergedModelSurface);
                uint64 mergedKey = 14695981039346656037ull;
                const int entityIndex = mergedEntity ? mergedEntity->index : -1;
                const int modelSurfaceIndex = drawSurf ? drawSurf->modelSurfaceIndex : -1;
                const uintptr_t triAddr = reinterpret_cast<uintptr_t>(tri);
                mergedKey ^= static_cast<uint64>(entityIndex + 1) + 0x9e3779b97f4a7c15ull;
                mergedKey *= 1099511628211ull;
                mergedKey ^= static_cast<uint64>(modelSurfaceIndex + 1);
                mergedKey *= 1099511628211ull;
                mergedKey ^= static_cast<uint64>(materialId);
                mergedKey *= 1099511628211ull;
                mergedKey ^= static_cast<uint64>(triAddr);
                mergedKey *= 1099511628211ull;
                mergedKey ^= 0x4d4552474544ull;
                mergedKey |= (2ull << 62);
                if (mergedKey == 0)
                {
                    mergedKey = 1;
                }
                RtSmokeMergedWalkedRange walked;
                walked.id = mergedKey;
                walked.bucketIndex = bucketIndex;
                walked.vertexBegin = bucketVertexStart;
                walked.vertexCount =
                    static_cast<int>(bucketVertices.size()) - bucketVertexStart;
                walked.indexBegin = bucketIndexStart;
                walked.indexCount = emittedIndexes;
                walked.triangleBegin = bucketTriangleStart;
                walked.triangleCount = emittedIndexes / 3;
                walked.surfaceClassId = surfaceClassId;
                walked.feedClass = static_cast<uint32_t>(feedClass);
                walked.materialId = materialId;
                walked.particle = (surfaceClass == RtSmokeSurfaceClass::ParticleAlpha);
                walked.trueDeform = (feedClass == RtPtFeedClass::TrueDeform);
                if (surfaceClass == RtSmokeSurfaceClass::RigidEntity &&
                    geometryUniverse)
                {
                    const viewEntity_t* space = drawSurf ? drawSurf->space : nullptr;
                    const idRenderEntityLocal* entity = space ? space->entityDef : nullptr;
                    const renderEntity_t* renderEntity = entity ? &entity->parms : nullptr;
                    const idRenderModel* renderModel = renderEntity ? renderEntity->hModel : nullptr;
                    const PtRenderDefKey renderDefKey = PtGeometryLifecycle::MakeEntityKey(entity);
                    const uint32_t modelEpoch = (renderDefKey.world && renderDefKey.index >= 0)
                        ? PtGeometryLifecycle::EntityModelEpoch(renderDefKey.world, renderDefKey.index)
                        : 0;
                    RtPathTraceMeshKey meshKey;
                    FillPathTraceRigidRouteMeshKey(
                        meshKey, tri, materialId, materialClassSignature, SmokeSurfaceClassId(surfaceClass));
                    const RtPathTraceRigidInstanceSnapshot companionSnap =
                        BuildAndRecordPathTraceMergedCompanionObservation(
                            geometryUniverse, meshKey, renderModel, tri,
                            renderDefKey, modelEpoch,
                            entity ? entity->index : -1,
                            renderEntity ? renderEntity->entityNum : -1,
                            drawSurf ? drawSurf->modelSurfaceIndex : -1,
                            sourceFlags);
                    walked.companionRigidId = companionSnap.instanceId;
                }
                if (surfaceClass == RtSmokeSurfaceClass::SkinnedDeformed &&
                    skinnedSurfaceRecords &&
                    !skinnedSurfaceRecords->empty())
                {
                    const RtSmokeSkinnedSurfaceRecord& rec = skinnedSurfaceRecords->back();
                    walked.companionSkinnedId =
                        PtHashCanonicalInstanceKey(rec.canonicalInstance) | (1ull << 63);
                }
                serialCaptureDecision.decisionPresence |=
                    RT_PT_CAPTURE_DECISION_MERGED_WALK;
                serialCaptureDecision.mergedWalkRecordPresent = true;
                serialCaptureDecision.mergedWalkVertexCount =
                    static_cast<std::uint32_t>(Max(0, walked.vertexCount));
                serialCaptureDecision.mergedWalkIndexCount =
                    static_cast<std::uint32_t>(Max(0, walked.indexCount));
                serialCaptureDecision.mergedWalkTriangleCount =
                    static_cast<std::uint32_t>(Max(0, walked.triangleCount));
                serialCaptureDecision.mergedCompanionRigidId =
                    walked.companionRigidId;
                serialCaptureDecision.mergedCompanionSkinnedId =
                    walked.companionSkinnedId;
                mergedWalkedRanges->push_back(walked);
            }

            AddMirrorMaterialStats(materialStats, drawSurf->material, emittedIndexes, surfaceClass, translucentSubtype);
            AddSmokeDynamicMaterialEvalStatsForMaterialId(materialStats, drawSurf, emittedIndexes, materialId);
            ++sourceSurfaces;
            sourceVerts += tri->numVerts;
            sourceIndexes += emittedIndexes;
            AddMirrorSurfaceClassStats(classStats, surfaceClass, tri->numVerts, emittedIndexes);
            AddMirrorDynamicGeometryStats(dynamicStats, surfaceClass, drawSurf, tri, emittedIndexes);
            ++bucketRanges.buckets[bucketIndex].surfaceCount;
            RT_SMOKE_R1_FINALIZE(RtSmokeR1Terminal::Accepted);
            }
        }
    }
    if (ownerHarvest)
    {
        RtPathTraceCaptureMembershipReceipt receipt;
        bool complete = true;
        for (const RtPathTraceOwnerHarvestSurface& surface :
            ownerHarvest->surfaces)
        {
            complete = complete && surface.finalized;
            AppendPathTraceCaptureMembershipReceipt(
                receipt, surface.decision);
        }
        receipt.complete = complete;
        ownerHarvest->membershipReceipt = receipt;
        ownerHarvest->complete = complete;
        ownerHarvest->harvestUs = Sys_Microseconds() - ownerHarvestStartUs;
        return complete;
    }
    if (compareMaterialClassifyParity)
    {
        common->Printf(
            "PathTracePrimaryPass: materialClassifySameFrame mapped=%llu routeCompared=%llu routeMismatch=%llu particle=%llu particleMismatch=%llu mappingMismatch=%llu classifierMismatch=%llu surfaceClass=%llu subtype=%llu emissive=%llu signature=%llu samples=%d,%d,%d,%d\n",
            static_cast<unsigned long long>(materialClassifyParity.mapped),
            static_cast<unsigned long long>(materialClassifyParity.compared),
            static_cast<unsigned long long>(materialClassifyParity.mismatched),
            static_cast<unsigned long long>(materialClassifyParity.particles),
            static_cast<unsigned long long>(materialClassifyParity.particleMismatches),
            static_cast<unsigned long long>(materialClassifyParity.mappingMismatches),
            static_cast<unsigned long long>(materialClassifyParity.classifierMismatches),
            static_cast<unsigned long long>(materialClassifyParity.surfaceClassMismatches),
            static_cast<unsigned long long>(materialClassifyParity.subtypeMismatches),
            static_cast<unsigned long long>(materialClassifyParity.emissiveMismatches),
            static_cast<unsigned long long>(materialClassifyParity.signatureMismatches),
            materialClassifyParity.mismatchOrdinals[0],
            materialClassifyParity.mismatchOrdinals[1],
            materialClassifyParity.mismatchOrdinals[2],
            materialClassifyParity.mismatchOrdinals[3]);
    }

#undef RT_SMOKE_R1_FINALIZE

    if (r1Audit)
    {
        char r1Line[RT_SMOKE_R1_OUTPUT_CAP];
        r1Line[0] = '\0';
        const uint64_t auditPreLogUs =
            Sys_Microseconds() - r1Audit->startUs;
        RtSmokeR1FormatLine(
            *r1Audit,
            auditPreLogUs,
            r1Line,
            sizeof(r1Line));
        common->Printf("%s", r1Line);
#if USE_OPTICK
        r1Scope->~Event();
#endif
        r1Audit->~RtSmokeR1Audit();
    }

    const int bucketMergeStartMs = Sys_Milliseconds();
    {
        OPTICK_EVENT("PT Dynamic Frame Bucket Merge");
        for (int bucketIndex = 0; bucketIndex < RT_SMOKE_CLASS_COUNT; ++bucketIndex)
        {
            if (bucketIndex == 0)
            {
                continue;
            }

            RtSmokeBucketRange& range = bucketRanges.buckets[bucketIndex];
            range.vertexOffset = static_cast<int>(vertexData.size());
            range.indexOffset = static_cast<int>(indexData.size());
            range.triangleOffset = static_cast<int>(triangleClassData.size());
            range.vertexCount = static_cast<int>(bucketVertexData[bucketIndex].size());
            range.indexCount = static_cast<int>(bucketIndexData[bucketIndex].size());
            range.triangleCount = static_cast<int>(bucketTriangleClassData[bucketIndex].size());
            FinalizeSmokeSkinnedSurfaceRecordOffsets(skinnedSurfaceRecords, bucketIndex, range);
            if (mergedWalkedRanges)
            {
                for (RtSmokeMergedWalkedRange& walked : *mergedWalkedRanges)
                {
                    if (walked.bucketIndex != bucketIndex)
                    {
                        continue;
                    }
                    walked.vertexBegin += range.vertexOffset;
                    walked.indexBegin += range.indexOffset;
                    walked.triangleBegin += range.triangleOffset;
                }
            }
            FinalizeSmokeCapturedSurfaceRecordOffsets(capturedSurfaceRecords, bucketIndex, range);

            const uint32_t vertexOffset = static_cast<uint32_t>(range.vertexOffset);
            vertexData.insert(vertexData.end(), bucketVertexData[bucketIndex].begin(), bucketVertexData[bucketIndex].end());
            for (uint32_t localIndex : bucketIndexData[bucketIndex])
            {
                indexData.push_back(vertexOffset + localIndex);
            }
            triangleClassData.insert(triangleClassData.end(), bucketTriangleClassData[bucketIndex].begin(), bucketTriangleClassData[bucketIndex].end());
            triangleMaterialData.insert(triangleMaterialData.end(), bucketTriangleMaterialData[bucketIndex].begin(), bucketTriangleMaterialData[bucketIndex].end());
            if (triangleInstanceData)
            {
                triangleInstanceData->insert(triangleInstanceData->end(), bucketTriangleInstanceData[bucketIndex].begin(), bucketTriangleInstanceData[bucketIndex].end());
            }
            if (triangleIdentityData)
            {
                triangleIdentityData->insert(triangleIdentityData->end(), bucketTriangleIdentityData[bucketIndex].begin(), bucketTriangleIdentityData[bucketIndex].end());
            }
        }
    }
    captureTiming.bucketMergeMs = Sys_Milliseconds() - bucketMergeStartMs;
    skipStats.geometryAdmittedBytes = dynamicAdmissionBytes;
    skipStats.geometryAdmittedSurfaces = dynamicAdmissionSurfaces;

    if (triangleClassData.empty() || triangleMaterialData.empty())
    {
        ++skipStats.emptyClassBuffer;
    }
    const bool overlapDumpRequested = r_pathTracingRigidRouteOverlapDump.GetInteger() != 0;
    if (r_pathTracingSmokeLog.GetInteger() != 0 || overlapDumpRequested)
    {
        common->Printf("PathTracePrimaryPass: PT rigid route dynamic removal mode=%d active=%d tested=%d promotedEmissive=%d ready(mesh/resident)=%d/%d removedSurfaces=%d removedIndexes=%d byInstance=%d renderPath=routedRigidPlusDynamicFallback\n",
            requestedDebugMode,
            removeRoutedRigidDynamic ? 1 : 0,
            routedRigidDynamicTested,
            routedRigidDynamicPromotedEmissive,
            routedRigidDynamicReadyByMesh,
            routedRigidDynamicReadyByResident,
            skippedRoutedRigidDynamicSurfaces,
            skippedRoutedRigidDynamicIndexes,
            skippedRoutedRigidDynamicByInstance);
        common->Printf(
            "PathTracePrimaryPass: GEO08 skinned capture split gate=%d priorRoutes=%d omitted(surfaces/verts/indexes)=%d/%d/%d cpuFallback(gate/priorRoute/currentContract/joints)=%d/%d/%d/%d lateFailurePolicy=suppress\n",
            skinnedCaptureSplitGate ? 1 : 0,
            captureTiming.skinnedCaptureAdmissionRoutes,
            captureTiming.skinnedCaptureOmittedSurfaces,
            captureTiming.skinnedCaptureOmittedVerts,
            captureTiming.skinnedCaptureOmittedIndexes,
            captureTiming.skinnedCaptureFallbackGate,
            captureTiming.skinnedCaptureFallbackPriorRoute,
            captureTiming.
                skinnedCaptureFallbackCurrentContract,
            captureTiming.skinnedCaptureFallbackJointData);
        if (overlapDumpRequested && requestedDebugMode != 24)
        {
            r_pathTracingRigidRouteOverlapDump.SetInteger(0);
        }
    }

    const bool hasDynamicGeometry = !vertexData.empty() && !indexData.empty() && !triangleClassData.empty() && !triangleMaterialData.empty();
    FinalizePathTraceCommittedCaptureTelemetry(viewDef);
    return sourceSurfaces > 0 && hasDynamicGeometry;
}

bool CopyPathTraceOwnerHarvestDecisionsToSnapshot(
    const RtPathTraceOwnerHarvest& harvest,
    RtPathTraceCaptureOwnerSnapshot& snapshot)
{
    if (!harvest.complete || harvest.surfaces.size() != snapshot.surfaces.size() ||
        harvest.surfaces.size() > snapshot.ownerDecisions.capacity())
    {
        return false;
    }
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try
    {
#endif
        snapshot.ownerDecisions.resize(harvest.surfaces.size());
        for (std::size_t index = 0; index < harvest.surfaces.size(); ++index)
        {
            const RtPathTraceOwnerHarvestSurface& surface =
                harvest.surfaces[index];
            if (!surface.finalized || surface.decision.ordinal != index)
            {
                snapshot.ownerDecisions.clear();
                return false;
            }
            snapshot.ownerDecisions[index] = surface.decision;
        }
        return AttachPathTraceOwnerDecisionTable(snapshot,
            snapshot.ownerDecisions.data(), snapshot.ownerDecisions.size(),
            harvest.membershipReceipt);
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    }
    catch (const std::bad_alloc&)
    {
        snapshot.ownerDecisions.clear();
        return false;
    }
    catch (const std::length_error&)
    {
        snapshot.ownerDecisions.clear();
        return false;
    }
#endif
}

bool AppendPathTraceOwnerHarvestGeometry(
    const RtPathTraceOwnerHarvest& harvest,
    std::vector<PathTraceSmokeVertex>& vertexData,
    std::vector<uint32_t>& indexData,
    std::vector<uint32_t>& triangleClassData,
    std::vector<uint32_t>& triangleMaterialData,
    std::vector<uint32_t>* triangleInstanceData,
    std::vector<uint32_t>* triangleIdentityData,
    int& sourceSurfaces,
    int& sourceVerts,
    int& sourceIndexes,
    RtSmokeSurfaceClassStats& classStats,
    RtSmokeSurfaceSkipStats& skipStats,
    RtSmokeDynamicGeometryStats& dynamicStats,
    RtSmokeAttributeStats& attributeStats,
    RtSmokeMaterialStats& materialStats,
    RtSmokeBucketRanges& bucketRanges,
    RtSmokeSceneCaptureTiming& captureTiming,
    std::vector<RtSmokeSkinnedSurfaceRecord>* skinnedSurfaceRecords,
    std::vector<RtSmokeCapturedSurfaceRecord>* capturedSurfaceRecords,
    std::vector<uint64_t>* rigidCaptureWalked,
    std::vector<uint32_t>* rigidCaptureWalkedTriangles,
    std::vector<RtSmokeMergedWalkedRange>* mergedWalkedRanges)
{
    OPTICK_EVENT("PT Owner Harvest Geometry Append");
    if (!harvest.complete)
    {
        return false;
    }
    vertexData.clear();
    indexData.clear();
    triangleClassData.clear();
    triangleMaterialData.clear();
    if (triangleInstanceData) triangleInstanceData->clear();
    if (triangleIdentityData) triangleIdentityData->clear();

    std::vector<PathTraceSmokeVertex> bucketVertices[RT_SMOKE_CLASS_COUNT];
    std::vector<uint32_t> bucketIndexes[RT_SMOKE_CLASS_COUNT];
    std::vector<uint32_t> bucketClasses[RT_SMOKE_CLASS_COUNT];
    std::vector<uint32_t> bucketMaterials[RT_SMOKE_CLASS_COUNT];
    std::vector<uint32_t> bucketInstances[RT_SMOKE_CLASS_COUNT];
    std::vector<uint32_t> bucketIdentities[RT_SMOKE_CLASS_COUNT];
    const RtSmokeGeometryAdmissionBudget admissionBudget =
        BuildSmokeDynamicGeometryAdmissionBudget();
    uint64 admissionBytes = 0;
    uint64 admissionSurfaces = 0;

    for (const RtPathTraceOwnerHarvestSurface& harvested : harvest.surfaces)
    {
        if (!harvested.finalized || !harvested.appendEligible ||
            !harvested.drawSurf || !harvested.tri)
        {
            continue;
        }
        const drawSurf_t* drawSurf = harvested.drawSurf;
        const srfTriangles_t* tri = harvested.tri;
        const RtPathTraceCaptureSurfaceProduct& decision = harvested.decision;
        RtPathTraceCaptureSurfaceProduct appliedDecision = decision;
        const int bucketIndex = idMath::ClampInt(0,
            RT_SMOKE_CLASS_COUNT - 1, decision.bucketIndex);
        const int vertexStart = static_cast<int>(bucketVertices[bucketIndex].size());
        const int indexStart = static_cast<int>(bucketIndexes[bucketIndex].size());
        const int triangleStart = static_cast<int>(bucketClasses[bucketIndex].size());
        const int invalidIndexBefore = skipStats.invalidIndexCount;
        const int zeroAreaBefore = skipStats.zeroAreaOnly;
        const int appendStartMs = Sys_Milliseconds();
        const uint64 appendStartUs = Sys_Microseconds();
        const int emittedIndexes = AppendSmokeSurfaceGeometry(
            drawSurf, tri, decision.surfaceClassId, decision.materialId,
            RT_SMOKE_CLASS_COUNT, RT_SMOKE_TRIANGLE_CLASS_MASK,
            static_cast<uint32_t>(RtSmokeSurfaceClass::ParticleAlpha),
            RT_SMOKE_TRIANGLE_FORCE_GEOMETRIC_NORMAL,
            bucketVertices[bucketIndex], bucketIndexes[bucketIndex],
            bucketClasses[bucketIndex], bucketMaterials[bucketIndex],
            skipStats, attributeStats);
        const int appendMs = Sys_Milliseconds() - appendStartMs;
        captureTiming.dynamicAppendMs += appendMs;
        captureTiming.appendMs += appendMs;
        if (GetSmokeRtCpuSkinningJoints(tri) != nullptr)
        {
            captureTiming.rtCpuSkinningAppendMs += appendMs;
            captureTiming.rtCpuSkinningAppendUs +=
                Sys_Microseconds() - appendStartUs;
        }
        if (emittedIndexes <= 0)
        {
            appliedDecision.terminal = RtPathTraceCaptureTerminal::RolledBack;
            appliedDecision.vertexCount = 0;
            appliedDecision.indexCount = 0;
            appliedDecision.triangleCount = 0;
            appliedDecision.invalidIndexCount =
                skipStats.invalidIndexCount - invalidIndexBefore;
            appliedDecision.zeroAreaTriangleCount =
                skipStats.zeroAreaOnly - zeroAreaBefore;
            appliedDecision.rollbackApplied = true;
            appliedDecision.capturedRecordPresent = false;
            appliedDecision.skinnedRecordPresent = false;
            appliedDecision.mergedWalkRecordPresent = false;
            appliedDecision.bucketRangePublished = false;
            FinalizePathTraceCaptureSurfaceDecision(appliedDecision);
            NotePathTraceCaptureSerialDecision(appliedDecision.ordinal,
                appliedDecision, appliedDecision.decisionPresence);
            continue;
        }
        const int emittedVertices = static_cast<int>(
            bucketVertices[bucketIndex].size()) - vertexStart;
        const RtSmokeGeometryAdmissionPlan actualAdmission =
            PlanSmokeDynamicGeometryAdmission(admissionBudget,
                admissionBytes, admissionSurfaces,
                emittedVertices, emittedIndexes);
        if (!actualAdmission.Admitted())
        {
            RecordSmokeGeometryAdmissionRejection(skipStats, actualAdmission);
            bucketVertices[bucketIndex].resize(vertexStart);
            bucketIndexes[bucketIndex].resize(indexStart);
            bucketClasses[bucketIndex].resize(triangleStart);
            bucketMaterials[bucketIndex].resize(triangleStart);
            appliedDecision.terminal = RtPathTraceCaptureTerminal::RolledBack;
            appliedDecision.vertexCount = 0;
            appliedDecision.indexCount = 0;
            appliedDecision.triangleCount = 0;
            appliedDecision.invalidIndexCount =
                skipStats.invalidIndexCount - invalidIndexBefore;
            appliedDecision.zeroAreaTriangleCount =
                skipStats.zeroAreaOnly - zeroAreaBefore;
            appliedDecision.rollbackApplied = true;
            appliedDecision.capturedRecordPresent = false;
            appliedDecision.skinnedRecordPresent = false;
            appliedDecision.mergedWalkRecordPresent = false;
            appliedDecision.bucketRangePublished = false;
            FinalizePathTraceCaptureSurfaceDecision(appliedDecision);
            NotePathTraceCaptureSerialDecision(appliedDecision.ordinal,
                appliedDecision, appliedDecision.decisionPresence);
            continue;
        }
        admissionBytes = actualAdmission.totalBytes;
        admissionSurfaces = actualAdmission.totalSurfaces;
        const int entityIndex = drawSurf->space && drawSurf->space->entityDef
            ? drawSurf->space->entityDef->index : -1;
        const uint32_t dynamicInstanceId = static_cast<uint32_t>(
            Max(1, entityIndex + 1));
        const int emittedTriangles = emittedIndexes / 3;
        appliedDecision.terminal = RtPathTraceCaptureTerminal::Accepted;
        appliedDecision.vertexOffset = static_cast<std::uint32_t>(vertexStart);
        appliedDecision.vertexCount = static_cast<std::uint32_t>(emittedVertices);
        appliedDecision.indexOffset = static_cast<std::uint32_t>(indexStart);
        appliedDecision.indexCount = static_cast<std::uint32_t>(emittedIndexes);
        appliedDecision.triangleOffset = static_cast<std::uint32_t>(triangleStart);
        appliedDecision.triangleCount = static_cast<std::uint32_t>(emittedTriangles);
        appliedDecision.invalidIndexCount =
            skipStats.invalidIndexCount - invalidIndexBefore;
        appliedDecision.zeroAreaTriangleCount =
            skipStats.zeroAreaOnly - zeroAreaBefore;
        appliedDecision.rollbackApplied = false;
        appliedDecision.capturedRecordPresent = capturedSurfaceRecords != nullptr;
        appliedDecision.capturedRecordVertexCount = appliedDecision.vertexCount;
        appliedDecision.capturedRecordIndexCount = appliedDecision.indexCount;
        appliedDecision.capturedRecordTriangleCount = appliedDecision.triangleCount;
        appliedDecision.bucketRangePublished = true;
        bucketInstances[bucketIndex].insert(
            bucketInstances[bucketIndex].end(), emittedTriangles,
            dynamicInstanceId);
        for (int triangle = 0; triangle < emittedTriangles; ++triangle)
        {
            bucketIdentities[bucketIndex].push_back(
                PtDynamicTriangleIdentitySeed(drawSurf, tri,
                    harvested.baseMaterialId,
                    static_cast<uint32_t>(triangle)));
        }
        if (decision.surfaceClass == RtSmokeSurfaceClass::SkinnedDeformed)
        {
            AddSmokeSkinnedSurfaceRecord(skinnedSurfaceRecords, drawSurf, tri,
                decision.surfaceClassId, decision.materialId,
                static_cast<int>(decision.ordinal), bucketIndex,
                vertexStart, indexStart, triangleStart,
                emittedVertices, emittedIndexes, emittedTriangles);
            appliedDecision.skinnedRecordPresent =
                skinnedSurfaceRecords != nullptr;
            appliedDecision.skinnedRecordVertexCount =
                appliedDecision.vertexCount;
            appliedDecision.skinnedRecordIndexCount =
                appliedDecision.indexCount;
            appliedDecision.skinnedRecordTriangleCount =
                appliedDecision.triangleCount;
        }
        AddSmokeCapturedSurfaceRecord(capturedSurfaceRecords, drawSurf,
            decision.surfaceClassId, decision.materialId,
            static_cast<int>(decision.ordinal), bucketIndex,
            vertexStart, indexStart, triangleStart,
            emittedVertices, emittedIndexes, emittedTriangles);
        if (rigidCaptureWalked && harvested.rigidWalkInstanceId != 0)
        {
            rigidCaptureWalked->push_back(harvested.rigidWalkInstanceId);
            if (rigidCaptureWalkedTriangles)
            {
                rigidCaptureWalkedTriangles->push_back(
                    static_cast<uint32_t>(emittedTriangles));
            }
        }
        if (mergedWalkedRanges && harvested.mergedRangeId != 0)
        {
            RtSmokeMergedWalkedRange walked;
            walked.id = harvested.mergedRangeId;
            walked.bucketIndex = bucketIndex;
            walked.vertexBegin = vertexStart;
            walked.vertexCount = emittedVertices;
            walked.indexBegin = indexStart;
            walked.indexCount = emittedIndexes;
            walked.triangleBegin = triangleStart;
            walked.triangleCount = emittedTriangles;
            walked.surfaceClassId = decision.surfaceClassId;
            walked.feedClass = harvested.feedClass;
            walked.materialId = decision.materialId;
            walked.particle = harvested.particle;
            walked.trueDeform = harvested.trueDeform;
            walked.companionRigidId = harvested.mergedCompanionRigidId;
            if (decision.surfaceClass == RtSmokeSurfaceClass::SkinnedDeformed &&
                skinnedSurfaceRecords && !skinnedSurfaceRecords->empty())
            {
                walked.companionSkinnedId = PtHashCanonicalInstanceKey(
                    skinnedSurfaceRecords->back().canonicalInstance) |
                    (1ull << 63);
            }
            mergedWalkedRanges->push_back(walked);
            appliedDecision.mergedWalkRecordPresent = true;
            appliedDecision.mergedWalkVertexCount = appliedDecision.vertexCount;
            appliedDecision.mergedWalkIndexCount = appliedDecision.indexCount;
            appliedDecision.mergedWalkTriangleCount =
                appliedDecision.triangleCount;
        }
        FinalizePathTraceCaptureSurfaceDecision(appliedDecision);
        NotePathTraceCaptureSerialDecision(appliedDecision.ordinal,
            appliedDecision, appliedDecision.decisionPresence);
        AddMirrorMaterialStats(materialStats, drawSurf->material,
            emittedIndexes, decision.surfaceClass,
            decision.translucentSubtype);
        AddSmokeDynamicMaterialEvalStatsForMaterialId(
            materialStats, drawSurf, emittedIndexes, decision.materialId);
        ++sourceSurfaces;
        sourceVerts += tri->numVerts;
        sourceIndexes += emittedIndexes;
        AddMirrorSurfaceClassStats(classStats, decision.surfaceClass,
            tri->numVerts, emittedIndexes);
        AddMirrorDynamicGeometryStats(dynamicStats, decision.surfaceClass,
            drawSurf, tri, emittedIndexes);
        ++bucketRanges.buckets[bucketIndex].surfaceCount;
        (void)invalidIndexBefore;
        (void)zeroAreaBefore;
    }

    const int mergeStartMs = Sys_Milliseconds();
    for (int bucketIndex = 1; bucketIndex < RT_SMOKE_CLASS_COUNT; ++bucketIndex)
    {
        RtSmokeBucketRange& range = bucketRanges.buckets[bucketIndex];
        range.vertexOffset = static_cast<int>(vertexData.size());
        range.indexOffset = static_cast<int>(indexData.size());
        range.triangleOffset = static_cast<int>(triangleClassData.size());
        range.vertexCount = static_cast<int>(bucketVertices[bucketIndex].size());
        range.indexCount = static_cast<int>(bucketIndexes[bucketIndex].size());
        range.triangleCount = static_cast<int>(bucketClasses[bucketIndex].size());
        FinalizeSmokeSkinnedSurfaceRecordOffsets(
            skinnedSurfaceRecords, bucketIndex, range);
        FinalizeSmokeCapturedSurfaceRecordOffsets(
            capturedSurfaceRecords, bucketIndex, range);
        if (mergedWalkedRanges)
        {
            for (RtSmokeMergedWalkedRange& walked : *mergedWalkedRanges)
            {
                if (walked.bucketIndex == bucketIndex)
                {
                    walked.vertexBegin += range.vertexOffset;
                    walked.indexBegin += range.indexOffset;
                    walked.triangleBegin += range.triangleOffset;
                }
            }
        }
        const uint32_t vertexOffset = static_cast<uint32_t>(range.vertexOffset);
        vertexData.insert(vertexData.end(), bucketVertices[bucketIndex].begin(),
            bucketVertices[bucketIndex].end());
        for (uint32_t localIndex : bucketIndexes[bucketIndex])
        {
            indexData.push_back(vertexOffset + localIndex);
        }
        triangleClassData.insert(triangleClassData.end(),
            bucketClasses[bucketIndex].begin(), bucketClasses[bucketIndex].end());
        triangleMaterialData.insert(triangleMaterialData.end(),
            bucketMaterials[bucketIndex].begin(), bucketMaterials[bucketIndex].end());
        if (triangleInstanceData)
        {
            triangleInstanceData->insert(triangleInstanceData->end(),
                bucketInstances[bucketIndex].begin(),
                bucketInstances[bucketIndex].end());
        }
        if (triangleIdentityData)
        {
            triangleIdentityData->insert(triangleIdentityData->end(),
                bucketIdentities[bucketIndex].begin(),
                bucketIdentities[bucketIndex].end());
        }
    }
    captureTiming.bucketMergeMs += Sys_Milliseconds() - mergeStartMs;
    skipStats.geometryAdmittedBytes = admissionBytes;
    skipStats.geometryAdmittedSurfaces = admissionSurfaces;
    if (triangleClassData.empty() || triangleMaterialData.empty())
    {
        ++skipStats.emptyClassBuffer;
    }
    return sourceSurfaces > 0 && !vertexData.empty() && !indexData.empty() &&
        !triangleClassData.empty() && !triangleMaterialData.empty();
}

bool PathTraceOwnerHarvestProductEligible(
    const RtPathTraceOwnerHarvest& harvest,
    const RtPathTraceCaptureProduct& product)
{
    if (!harvest.complete || !product.complete ||
        product.membershipReceipt.cpuSkinnedAcceptedCount != 0 ||
        !PathTraceCaptureMembershipReceiptsMatch(
            product.membershipReceipt, harvest.membershipReceipt))
    {
        return false;
    }
    const RtSmokeGeometryAdmissionBudget admissionBudget =
        BuildSmokeDynamicGeometryAdmissionBudget();
    uint64 admissionBytes = 0;
    uint64 admissionSurfaces = 0;
    for (const RtPathTraceCaptureSurfaceProduct& decision : product.surfaces)
    {
        if (decision.ordinal >= harvest.surfaces.size())
        {
            return false;
        }
        const RtPathTraceOwnerHarvestSurface& harvested =
            harvest.surfaces[decision.ordinal];
        if (!harvested.finalized)
        {
            return false;
        }
        if (decision.terminal == RtPathTraceCaptureTerminal::Accepted &&
            (!harvested.drawSurf || !harvested.tri ||
                decision.indexCount == 0 ||
                (decision.indexCount % 3u) != 0u ||
                decision.surfaceClass == RtSmokeSurfaceClass::SkinnedDeformed))
        {
            return false;
        }
        if (decision.terminal == RtPathTraceCaptureTerminal::Accepted)
        {
            const RtSmokeGeometryAdmissionPlan admission =
                PlanSmokeDynamicGeometryAdmission(admissionBudget,
                    admissionBytes, admissionSurfaces,
                    decision.vertexCount, decision.indexCount);
            if (!admission.Admitted())
            {
                return false;
            }
            admissionBytes = admission.totalBytes;
            admissionSurfaces = admission.totalSurfaces;
        }
    }
    return true;
}

bool FinalizePathTraceOwnerHarvestWithProduct(
    const RtPathTraceOwnerHarvest& harvest,
    const RtPathTraceCaptureProduct& product,
    int& sourceSurfaces,
    int& sourceVerts,
    int& sourceIndexes,
    RtSmokeSurfaceClassStats& classStats,
    RtSmokeSurfaceSkipStats& skipStats,
    RtSmokeDynamicGeometryStats& dynamicStats,
    RtSmokeMaterialStats& materialStats,
    RtSmokeBucketRanges& bucketRanges,
    RtSmokeSceneCaptureTiming& captureTiming,
    std::vector<uint64_t>* rigidCaptureWalked,
    std::vector<uint32_t>* rigidCaptureWalkedTriangles,
    std::vector<RtSmokeMergedWalkedRange>* mergedWalkedRanges)
{
    OPTICK_EVENT("PT Owner Harvest Product Finalize");
    if (!PathTraceOwnerHarvestProductEligible(harvest, product))
    {
        return false;
    }
    const RtSmokeGeometryAdmissionBudget admissionBudget =
        BuildSmokeDynamicGeometryAdmissionBudget();
    uint64 admissionBytes = 0;
    uint64 admissionSurfaces = 0;
    for (const RtPathTraceCaptureSurfaceProduct& decision : product.surfaces)
    {
        if (decision.ordinal >= harvest.surfaces.size())
        {
            return false;
        }
        const RtPathTraceOwnerHarvestSurface& harvested =
            harvest.surfaces[decision.ordinal];
        if (!harvested.finalized ||
            decision.terminal != RtPathTraceCaptureTerminal::Accepted)
        {
            continue;
        }
        if (!harvested.drawSurf || !harvested.tri ||
            decision.indexCount == 0 || (decision.indexCount % 3u) != 0u)
        {
            return false;
        }
        const int emittedIndexes = static_cast<int>(decision.indexCount);
        const int emittedVertices = static_cast<int>(decision.vertexCount);
        const RtSmokeGeometryAdmissionPlan admission =
            PlanSmokeDynamicGeometryAdmission(admissionBudget,
                admissionBytes, admissionSurfaces,
                emittedVertices, emittedIndexes);
        if (!admission.Admitted())
        {
            return false;
        }
        admissionBytes = admission.totalBytes;
        admissionSurfaces = admission.totalSurfaces;
        skipStats.invalidIndexCount += decision.invalidIndexCount;
        skipStats.zeroAreaOnly += decision.zeroAreaTriangleCount;
        AddMirrorMaterialStats(materialStats,
            harvested.drawSurf->material, emittedIndexes,
            decision.surfaceClass, decision.translucentSubtype);
        AddSmokeDynamicMaterialEvalStatsForMaterialId(materialStats,
            harvested.drawSurf, emittedIndexes, decision.materialId);
        AddMirrorSurfaceClassStats(classStats, decision.surfaceClass,
            emittedVertices, emittedIndexes);
        AddMirrorDynamicGeometryStats(dynamicStats, decision.surfaceClass,
            harvested.drawSurf, harvested.tri, emittedIndexes);
        const int bucketIndex = idMath::ClampInt(0,
            RT_SMOKE_CLASS_COUNT - 1, decision.bucketIndex);
        if (rigidCaptureWalked && harvested.rigidWalkInstanceId != 0)
        {
            rigidCaptureWalked->push_back(harvested.rigidWalkInstanceId);
            if (rigidCaptureWalkedTriangles)
            {
                rigidCaptureWalkedTriangles->push_back(
                    decision.indexCount / 3u);
            }
        }
        if (mergedWalkedRanges && harvested.mergedRangeId != 0)
        {
            RtSmokeMergedWalkedRange walked;
            walked.id = harvested.mergedRangeId;
            walked.bucketIndex = bucketIndex;
            walked.vertexBegin = static_cast<int>(decision.vertexOffset);
            walked.vertexCount = emittedVertices;
            walked.indexBegin = static_cast<int>(decision.indexOffset);
            walked.indexCount = emittedIndexes;
            walked.triangleBegin = static_cast<int>(decision.triangleOffset);
            walked.triangleCount = emittedIndexes / 3;
            walked.surfaceClassId = decision.surfaceClassId;
            walked.feedClass = harvested.feedClass;
            walked.materialId = decision.materialId;
            walked.particle = harvested.particle;
            walked.trueDeform = harvested.trueDeform;
            walked.companionRigidId = harvested.mergedCompanionRigidId;
            mergedWalkedRanges->push_back(walked);
        }
    }
    (void)sourceSurfaces;
    (void)sourceVerts;
    (void)sourceIndexes;
    (void)bucketRanges;
    skipStats.geometryAdmittedBytes = admissionBytes;
    skipStats.geometryAdmittedSurfaces = admissionSurfaces;
    captureTiming.bucketMergeMs += 0;
    return true;
}

void BuildSmokeSurfaceTextureMatrices(const drawSurf_t* surface, int selectedStage,
    float primary[6], float normal[6])
{
    const float identity[6] = { 1, 0, 0, 0, 1, 0 };
    std::copy(identity, identity + 6, primary);
    const idMaterial* material = surface ? surface->material : nullptr;
    const float* registers = !material ? nullptr :
        (surface->shaderRegisters ? surface->shaderRegisters : material->ConstantRegisters());
    BuildRigidNormalTexMatrix(material, registers, normal);
    if (!material || !registers) return;
    const shaderStage_t* chosen = selectedStage >= 0 && selectedStage < material->GetNumStages()
        ? material->GetStage(selectedStage) : nullptr;
    if (!chosen || !chosen->texture.hasMatrix)
    {
        chosen = nullptr;
        const auto* info = FindSmokeMaterialTextureInfoReadOnly(SmokeMaterialId(material));
        for (int i = 0; i < material->GetNumStages(); ++i)
        {
            const auto* stage = material->GetStage(i);
            if (!stage) continue;
            if (stage->lighting == SL_DIFFUSE) { chosen = stage; break; }
            if (!chosen && stage->lighting == SL_AMBIENT && info &&
                stage->texture.image == info->diffuseImage) chosen = stage;
        }
    }
    if (!chosen || !chosen->texture.hasMatrix) return;
    for (int row = 0; row < 2; ++row)
        for (int column = 0; column < 3; ++column)
        {
            const int index = chosen->texture.matrix[row][column];
            if (index >= 0 && index < material->GetNumRegisters()) primary[row * 3 + column] = registers[index];
        }
}
