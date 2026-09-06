#include "precompiled.h"
#pragma hdrstop
#include "PathTraceCpuProducerApplyGate.h"
#include "PathTraceCpuProducerPublish.h"

// Doom draw-surface capture implementation for the RT smoke scene.
//
// The capture pass is intentionally conservative: it buckets surfaces into the
// few categories the prototype can render, records skip/classification reasons
// for diagnostics, and leaves unsupported dynamic material behavior hidden until
// a later material system can represent it safely.

#include "PathTraceCVars.h"
#include "PathTraceCacheGate.h"
#include "PathTraceCaptureProduct.h"
#include "PathTraceSceneCapture.h"
#include "PathTraceCpuProducerRewrite.h"
#include "PathTraceAcceleration.h"
#include "PathTraceDoomMaterialClassifier.h"
#include "PathTraceDynamicMaterialState.h"
#include "PathTraceGeometryLifecycle.h"
#include "PathTraceGuiSurfaces.h"
#include "PathTraceMaterialIdKernel.h"
#include "PathTraceMaterialTextureDiscovery.h"
#include "PathTraceOwnerSemanticKernel.h"
#include "PathTraceParticleCapture.h"
#include "PathTraceProducerLaneContract.h"
#include "PathTraceRigidIdentity.h"
#include "PathTraceSkinning.h"
#include "PathTraceSurfaceClassification.h"
#include "PathTraceTextureRegistry.h"
#include "../GLMatrix.h"
#include "../Model_local.h"
#include "../RenderCommon.h"

#include <algorithm>
#include <limits>
#include <unordered_set>

RtSmokeGeometryAdmissionBudget BuildSmokeDynamicGeometryAdmissionBudget()
{
    RtSmokeGeometryAdmissionBudget budget;
    const int budgetMB =
        Max(0, r_pathTracingGeometryDynamicFallbackBudgetMB.GetInteger());
    const int surfaceBudget =
        Max(0,
            r_pathTracingGeometryDynamicFallbackSurfaceBudget.GetInteger());
    budget.maxBytes =
        static_cast<uint64_t>(budgetMB) * 1024ull * 1024ull;
    budget.maxSurfaces = static_cast<uint64_t>(surfaceBudget);
    return budget;
}

RtSmokeGeometryAdmissionBudget BuildSmokeStaticGeometryAdmissionBudget()
{
    RtSmokeGeometryAdmissionBudget budget;
    const int budgetMB =
        Max(0, r_pathTracingGeometryStaticResidentBudgetMB.GetInteger());
    const int surfaceBudget =
        Max(0,
            r_pathTracingGeometryStaticResidentSurfaceBudget.GetInteger());
    budget.maxBytes =
        static_cast<uint64_t>(budgetMB) * 1024ull * 1024ull;
    budget.maxSurfaces = static_cast<uint64_t>(surfaceBudget);
    return budget;
}

RtSmokeGeometryAdmissionPlan PlanSmokeDynamicGeometryAdmission(
    const RtSmokeGeometryAdmissionBudget& budget,
    uint64 currentBytes,
    uint64 currentSurfaces,
    int vertexCount,
    int indexCount)
{
    RtSmokeGeometryAdmissionInput input;
    input.currentBytes = currentBytes;
    input.currentSurfaces = currentSurfaces;
    input.candidateVertexCount = vertexCount;
    input.candidateIndexCount = indexCount;
    input.vertexStride = sizeof(PathTraceSmokeVertex);
    input.indexStride = sizeof(uint32_t);
    input.triangleMetadataStride = sizeof(uint32_t) * 4ull;
    return BuildSmokeGeometryAdmissionPlan(budget, input);
}

RtSmokeGeometryAdmissionPlan PlanSmokeStaticGeometryAdmission(
    const RtSmokeGeometryAdmissionBudget& budget,
    const RtSmokeGeometryUniverse& geometryUniverse,
    int vertexCount,
    int indexCount,
    uint64 candidateSurfaceCount)
{
    RtSmokeGeometryAdmissionPlan failure;
    const size_t currentVertexCount =
        geometryUniverse.StaticVertices().size();
    const size_t currentIndexCount =
        geometryUniverse.StaticIndexes().size();
    const size_t currentTriangleCount = currentIndexCount / 3;
    if (currentVertexCount >
            static_cast<size_t>(
                std::numeric_limits<int64_t>::max()) ||
        currentIndexCount >
            static_cast<size_t>(
                std::numeric_limits<int64_t>::max()))
    {
        failure.result =
            RT_SMOKE_GEOMETRY_ADMISSION_REJECT_ARITHMETIC_OVERFLOW;
        return failure;
    }
    if ((currentIndexCount % 3) != 0 ||
        geometryUniverse.StaticTriangleClasses().size() !=
            currentTriangleCount ||
        geometryUniverse.StaticTriangleMaterials().size() !=
            currentTriangleCount)
    {
        failure.result =
            RT_SMOKE_GEOMETRY_ADMISSION_REJECT_INVALID_COUNT;
        return failure;
    }

    RtSmokeGeometryAdmissionInput currentInput;
    currentInput.currentSurfaces =
        static_cast<uint64_t>(
            geometryUniverse.StaticSurfaceRecords().size());
    currentInput.candidateVertexCount =
        static_cast<int64_t>(currentVertexCount);
    currentInput.candidateIndexCount =
        static_cast<int64_t>(currentIndexCount);
    currentInput.candidateSurfaceCount = 0;
    currentInput.vertexStride = sizeof(PathTraceSmokeVertex);
    currentInput.indexStride = sizeof(uint32_t);
    currentInput.triangleMetadataStride = sizeof(uint32_t) * 2ull;
    const RtSmokeGeometryAdmissionPlan currentPlan =
        BuildSmokeGeometryAdmissionPlan(
            RtSmokeGeometryAdmissionBudget(), currentInput);
    if (!currentPlan.Admitted())
    {
        return currentPlan;
    }

    RtSmokeGeometryAdmissionInput candidateInput;
    candidateInput.currentBytes = currentPlan.totalBytes;
    candidateInput.currentSurfaces = currentPlan.totalSurfaces;
    candidateInput.candidateVertexCount = vertexCount;
    candidateInput.candidateIndexCount = indexCount;
    candidateInput.candidateSurfaceCount = candidateSurfaceCount;
    candidateInput.vertexStride = sizeof(PathTraceSmokeVertex);
    candidateInput.indexStride = sizeof(uint32_t);
    candidateInput.triangleMetadataStride = sizeof(uint32_t) * 2ull;
    return BuildSmokeGeometryAdmissionPlan(budget, candidateInput);
}

void RecordSmokeGeometryAdmissionRejection(
    RtSmokeSurfaceSkipStats& skipStats,
    const RtSmokeGeometryAdmissionPlan& plan)
{
    ++skipStats.limitExceeded;
    switch (plan.result)
    {
        case RT_SMOKE_GEOMETRY_ADMISSION_REJECT_SURFACE_BUDGET:
            ++skipStats.geometrySurfaceBudgetExceeded;
            break;
        case RT_SMOKE_GEOMETRY_ADMISSION_REJECT_BYTE_BUDGET:
            ++skipStats.geometryByteBudgetExceeded;
            break;
        case RT_SMOKE_GEOMETRY_ADMISSION_REJECT_INVALID_COUNT:
            ++skipStats.geometryAdmissionInvalid;
            break;
        case RT_SMOKE_GEOMETRY_ADMISSION_REJECT_ARITHMETIC_OVERFLOW:
            ++skipStats.geometryAdmissionOverflow;
            break;
        default:
            return;
    }

    if (plan.candidateBytes >
        std::numeric_limits<uint64>::max() -
            skipStats.geometryRejectedBytes)
    {
        skipStats.geometryRejectedBytes =
            std::numeric_limits<uint64>::max();
    }
    else
    {
        skipStats.geometryRejectedBytes += plan.candidateBytes;
    }
}

void RecordSmokeStaticGeometryAdmissionRejection(
    RtSmokeSurfaceSkipStats& skipStats,
    const RtSmokeGeometryAdmissionPlan& plan)
{
    ++skipStats.limitExceeded;
    switch (plan.result)
    {
        case RT_SMOKE_GEOMETRY_ADMISSION_REJECT_SURFACE_BUDGET:
            ++skipStats.geometryStaticSurfaceBudgetExceeded;
            break;
        case RT_SMOKE_GEOMETRY_ADMISSION_REJECT_BYTE_BUDGET:
            ++skipStats.geometryStaticByteBudgetExceeded;
            break;
        case RT_SMOKE_GEOMETRY_ADMISSION_REJECT_INVALID_COUNT:
            ++skipStats.geometryStaticAdmissionInvalid;
            break;
        case RT_SMOKE_GEOMETRY_ADMISSION_REJECT_ARITHMETIC_OVERFLOW:
            ++skipStats.geometryStaticAdmissionOverflow;
            break;
        default:
            return;
    }

    if (plan.candidateBytes >
        std::numeric_limits<uint64>::max() -
            skipStats.geometryStaticRejectedBytes)
    {
        skipStats.geometryStaticRejectedBytes =
            std::numeric_limits<uint64>::max();
    }
    else
    {
        skipStats.geometryStaticRejectedBytes +=
            plan.candidateBytes;
    }
}

void UpdateSmokeStaticGeometryAdmissionTotals(
    RtSmokeSurfaceSkipStats& skipStats,
    const RtSmokeGeometryUniverse& geometryUniverse)
{
    const RtSmokeGeometryAdmissionPlan totals =
        PlanSmokeStaticGeometryAdmission(
            RtSmokeGeometryAdmissionBudget(),
            geometryUniverse,
            0,
            0,
            0);
    if (totals.Admitted())
    {
        skipStats.geometryStaticAdmittedBytes = totals.totalBytes;
        skipStats.geometryStaticAdmittedSurfaces =
            totals.totalSurfaces;
    }
    else
    {
        RecordSmokeStaticGeometryAdmissionRejection(
            skipStats, totals);
    }
}

void TransformSurfacePointToWorld(const drawSurf_t* drawSurf, const idVec3& localPoint, idVec3& worldPoint)
{
    if (drawSurf->space)
    {
        R_LocalPointToGlobal(drawSurf->space->modelMatrix, localPoint, worldPoint);
        return;
    }

    worldPoint = localPoint;
}

void TransformSurfaceVectorToWorld(const drawSurf_t* drawSurf, const idVec3& localVector, idVec3& worldVector)
{
    if (drawSurf->space)
    {
        R_LocalVectorToGlobal(drawSurf->space->modelMatrix, localVector, worldVector);
        return;
    }

    worldVector = localVector;
}

static bool SmokeDrawSurfaceHasAnyActiveStage(const drawSurf_t* drawSurf)
{
    const idMaterial* material = drawSurf ? drawSurf->material : nullptr;
    if (!material)
    {
        return false;
    }

    const float* regs = drawSurf->shaderRegisters ? drawSurf->shaderRegisters : material->ConstantRegisters();
    if (!regs)
    {
        return true;
    }

    const int registerCount = material->GetNumRegisters();
    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage)
        {
            continue;
        }

        const int conditionRegister = stage->conditionRegister;
        const float condition = conditionRegister >= 0 && conditionRegister < registerCount ? regs[conditionRegister] : 1.0f;
        if (condition != 0.0f)
        {
            return true;
        }
    }

    return false;
}

static bool SmokeRenderWorldContainsEntity(
    const viewDef_t* viewDef,
    const idRenderEntityLocal* entityDef)
{
    OPTICK_EVENT("PT Dynamic Validate Containment");
    if (!viewDef || !viewDef->renderWorld || !entityDef)
    {
        return false;
    }

    const idRenderWorldLocal* renderWorld = viewDef->renderWorld;
    for (int entityIndex = 0; entityIndex < renderWorld->entityDefs.Num(); ++entityIndex)
    {
        if (renderWorld->entityDefs[entityIndex] == entityDef)
        {
            return true;
        }
    }
    return false;
}

bool ValidateSmokeDrawSurface(
    const viewDef_t* viewDef,
    const drawSurf_t* drawSurf,
    const srfTriangles_t*& tri,
    RtSmokeSurfaceSkipStats* skipStats,
    RtSmokeR1CacheValidationObservation* r1Observation)
{
    if (r1Observation)
    {
        *r1Observation = {};
    }
    tri = nullptr;
    if (!drawSurf)
    {
        if (r1Observation)
        {
            r1Observation->disposition = RtSmokeR1ValidationDisposition::NullSurface;
        }
        if (skipStats)
        {
            ++skipStats->nullSurface;
        }
        return false;
    }

    if (!drawSurf->frontEndGeo)
    {
        if (r1Observation)
        {
            r1Observation->disposition = RtSmokeR1ValidationDisposition::MissingFrontEndGeo;
        }
        if (skipStats)
        {
            ++skipStats->missingGeometry;
        }
        return false;
    }

    if (!drawSurf->material)
    {
        if (r1Observation)
        {
            r1Observation->disposition = RtSmokeR1ValidationDisposition::NullMaterial;
        }
        if (skipStats)
        {
            ++skipStats->nullMaterial;
        }
        return false;
    }

    if (!SmokeDrawSurfaceHasAnyActiveStage(drawSurf))
    {
        if (r1Observation)
        {
            r1Observation->disposition = RtSmokeR1ValidationDisposition::ConditionedOff;
        }
        if (skipStats)
        {
            ++skipStats->conditionedOff;
        }
        return false;
    }

    const bool guiDrawSurface = IsSmokeGuiDrawSurface(drawSurf);
    if (guiDrawSurface && r_pathTracingAllowGuiSurfaces.GetInteger() == 0)
    {
        if (r1Observation)
        {
            r1Observation->disposition = RtSmokeR1ValidationDisposition::GuiSurface;
        }
        if (skipStats)
        {
            ++skipStats->guiSurface;
        }
        return false;
    }

    if (!drawSurf->space)
    {
        if (r1Observation)
        {
            r1Observation->disposition = RtSmokeR1ValidationDisposition::NullSpace;
        }
        if (skipStats)
        {
            ++skipStats->nullSpace;
        }
        return false;
    }

    const viewEntity_t* space = drawSurf->space;
    const idRenderEntityLocal* entityDef = space->entityDef;
    const bool worldSpace = viewDef && space == &viewDef->worldSpace;
    const bool entityLive = SmokeRenderWorldContainsEntity(viewDef, entityDef);
    if (!worldSpace && entityDef &&
        entityDef->index == r_pathTracingGeometrySuppressEntityIndex.GetInteger())
    {
        return false;
    }
    const uint32_t suppressedMaterialId = static_cast<uint32_t>(Max(
        0,
        r_pathTracingGeometrySuppressMaterialId.GetInteger()));
    if (suppressedMaterialId != 0u &&
        SmokeMaterialId(drawSurf->material) == suppressedMaterialId)
    {
        return false;
    }
    if (!guiDrawSurface && !worldSpace && !entityLive)
    {
        if (r1Observation)
        {
            r1Observation->disposition = RtSmokeR1ValidationDisposition::NullModel;
        }
        if (skipStats)
        {
            ++skipStats->nullModel;
        }
        return false;
    }
    const renderEntity_t* renderEntity = entityLive ? &entityDef->parms : nullptr;
    if (!guiDrawSurface && !worldSpace && !renderEntity->hModel)
    {
        if (r1Observation)
        {
            r1Observation->disposition = RtSmokeR1ValidationDisposition::NullModel;
        }
        if (skipStats)
        {
            ++skipStats->nullModel;
        }
        return false;
    }

    const bool riskyCallbackSurface =
        renderEntity &&
        renderEntity->callback &&
        renderEntity->customShader != nullptr;
    if (!guiDrawSurface && riskyCallbackSurface && r_pathTracingSkipCallbackEntities.GetInteger() != 0)
    {
        if (r1Observation)
        {
            r1Observation->disposition = RtSmokeR1ValidationDisposition::CallbackEntity;
        }
        if (skipStats)
        {
            ++skipStats->callbackEntity;
        }
        return false;
    }

    tri = drawSurf->frontEndGeo;
    if (!tri->verts || !tri->indexes || tri->numVerts < 3 || tri->numIndexes < 3)
    {
        if (r1Observation)
        {
            r1Observation->disposition = RtSmokeR1ValidationDisposition::MissingGeometryPayload;
        }
        if (skipStats)
        {
            ++skipStats->missingGeometry;
        }
        return false;
    }

    if ((tri->numIndexes % 3) != 0 ||
        (drawSurf->numIndexes % 3) != 0 ||
        drawSurf->numIndexes < 3 ||
        drawSurf->numIndexes > tri->numIndexes)
    {
        if (r1Observation)
        {
            r1Observation->disposition = RtSmokeR1ValidationDisposition::InvalidIndexCount;
        }
        if (skipStats)
        {
            ++skipStats->invalidIndexCount;
        }
        return false;
    }

    RtSmokeLiveCacheGateSample* liveSample = r1Observation
        ? &r1Observation->liveSample : nullptr;
    const RtSmokeLiveCacheVerdict liveVerdict = EvaluateSmokeLiveCacheGate(
        tri->ambientCache,
        tri->indexCache,
        vertexCache.currentFrame,
        liveSample);
    if (r1Observation)
    {
        r1Observation->cacheEligible = true;
        r1Observation->liveVerdict = liveVerdict;

        // Diagnostic selection retains exactly one drawSurf-first/tri-fallback
        // handle per slot. It never resamples through the live gate.
        const RtSmokeCacheHandle drawAmbient = drawSurf->ambientCache;
        const RtSmokeCacheHandle drawIndex = drawSurf->indexCache;
        const RtSmokeCacheHandle triAmbient = tri->ambientCache;
        const RtSmokeCacheHandle triIndex = tri->indexCache;
        r1Observation->diagnosticVerdict = EvaluateSmokeDiagnosticCacheGate(
            drawAmbient,
            drawIndex,
            triAmbient,
            triIndex,
            vertexCache.currentFrame,
            &r1Observation->diagnosticSample);
    }

    if (SmokeLiveCacheGateRejects(liveVerdict))
    {
        if (r1Observation)
        {
            r1Observation->disposition =
                RtSmokeR1ValidationDisposition::NonCurrentCache;
        }
        if (skipStats)
        {
            ++skipStats->nonCurrentCache;
        }
        return false;
    }

    if (r1Observation)
    {
        r1Observation->disposition = RtSmokeR1ValidationDisposition::Accepted;
    }
    return true;
}

// drawSurf_t::frontEndGeo is explicitly frontend-mutable. Dynamic model
// surfaces can therefore retain a non-null tri->indexes pointer after its
// referenced allocation has been replaced. The draw-surface index cache is
// the frame-owned backend contract and remains valid for this capture frame.
// Static cache handles cannot be CPU-mapped through the frame allocator, so
// static model geometry continues to use its persistent CPU index array.
static const triIndex_t* SmokeDrawSurfaceIndexes(const drawSurf_t* drawSurf, const srfTriangles_t* tri)
{
    if (!drawSurf || !tri)
    {
        return nullptr;
    }

    const vertCacheHandle_t indexCache = drawSurf->indexCache != 0 ? drawSurf->indexCache : tri->indexCache;
    if (indexCache != 0 &&
        !idVertexCache::CacheIsStatic(indexCache) &&
        vertexCache.CacheIsCurrent(indexCache))
    {
        return reinterpret_cast<const triIndex_t*>(vertexCache.MappedIndexBuffer(indexCache));
    }

    return tri->indexes;
}

static const idDrawVert* SmokeDrawSurfaceVertices(const drawSurf_t* drawSurf, const srfTriangles_t* tri, bool& fromFrameCache)
{
    fromFrameCache = false;
    if (!drawSurf || !tri)
    {
        return nullptr;
    }

    const vertCacheHandle_t ambientCache = drawSurf->ambientCache != 0 ? drawSurf->ambientCache : tri->ambientCache;
    if (ambientCache != 0 &&
        !idVertexCache::CacheIsStatic(ambientCache) &&
        vertexCache.CacheIsCurrent(ambientCache))
    {
        fromFrameCache = true;
        return reinterpret_cast<const idDrawVert*>(vertexCache.MappedVertexBuffer(ambientCache));
    }

    return tri->verts;
}

static const idJointMat* SmokeDrawSurfaceCpuSkinningJoints(
    const drawSurf_t* drawSurf,
    const srfTriangles_t* tri,
    bool verticesFromFrameCache)
{
    // Frame-cache vertices are already in the exact form consumed by the
    // backend. Re-reading staticModelWithJoints here both double-skins them
    // and follows frontend-owned model memory whose lifetime is not retained.
    if (verticesFromFrameCache)
    {
        return nullptr;
    }
    if (drawSurf &&
        drawSurf->jointCacheCpuSnapshot != nullptr &&
        drawSurf->jointCacheCpuSnapshotCount > 0)
    {
        return drawSurf->jointCacheCpuSnapshot;
    }
    return GetSmokeRtCpuSkinningJoints(tri);
}

static PathTraceSmokeVertex BuildSmokeSurfaceVertexFromSource(
    const drawSurf_t* drawSurf,
    const idDrawVert* sourceVertices,
    int vertexIndex,
    const idJointMat* rtCpuSkinningJoints);
static bool SmokeTryCopyMemory(void* destination, const void* source, size_t byteCount);

bool SmokeSurfaceBoundsMayHitRay(const drawSurf_t* drawSurf, const srfTriangles_t* tri, const idVec3& rayOrigin, const idVec3& rayDirection)
{
    if (!drawSurf || !tri)
    {
        return false;
    }

    idVec3 localPoints[8];
    tri->bounds.Expand(1.0f).ToPoints(localPoints);

    idBounds worldBounds;
    worldBounds.Clear();
    for (int pointIndex = 0; pointIndex < 8; ++pointIndex)
    {
        idVec3 worldPoint;
        TransformSurfacePointToWorld(drawSurf, localPoints[pointIndex], worldPoint);
        worldBounds.AddPoint(worldPoint);
    }

    float boundsHitScale = 0.0f;
    return worldBounds.RayIntersection(rayOrigin, rayDirection, boundsHitScale) && boundsHitScale >= 0.0f;
}

bool FindCenterCameraRayAnchor(const viewDef_t* viewDef, idVec3& anchorPoint, int& anchorSurface, int& anchorTriangle, RtSmokeSceneCaptureTiming* captureTiming)
{
    const idVec3 rayOrigin = viewDef->renderView.vieworg;
    idVec3 rayDirection = viewDef->renderView.viewaxis[0];
    rayDirection.Normalize();

    bool foundHit = false;
    float closestHit = 1.0e30f;
    anchorSurface = -1;
    anchorTriangle = -1;

    for (int anchorPass = 0; anchorPass < 2 && !foundHit; ++anchorPass)
    {
        const bool useBoundsCull = anchorPass == 0;
        for (int surfaceIndex = 0; surfaceIndex < viewDef->numDrawSurfs; ++surfaceIndex)
        {
            const drawSurf_t* drawSurf = viewDef->drawSurfs[surfaceIndex];
            if (!drawSurf || !drawSurf->frontEndGeo)
            {
                continue;
            }

            const srfTriangles_t* tri = nullptr;
            if (!ValidateSmokeDrawSurface(viewDef, drawSurf, tri, nullptr))
            {
                continue;
            }

            if (captureTiming && useBoundsCull)
            {
                ++captureTiming->anchorSurfaceTests;
            }
            if (useBoundsCull && !SmokeSurfaceBoundsMayHitRay(drawSurf, tri, rayOrigin, rayDirection))
            {
                if (captureTiming)
                {
                    ++captureTiming->anchorBoundsRejects;
                }
                continue;
            }

            bool verticesFromFrameCache = false;
            const idDrawVert* sourceVertices = SmokeDrawSurfaceVertices(drawSurf, tri, verticesFromFrameCache);
            const idJointMat* rtCpuSkinningJoints = SmokeDrawSurfaceCpuSkinningJoints(drawSurf, tri, verticesFromFrameCache);
            const triIndex_t* sourceIndexes = SmokeDrawSurfaceIndexes(drawSurf, tri);
            if (!sourceVertices || !sourceIndexes)
            {
                continue;
            }
            for (int index = 0; index + 2 < drawSurf->numIndexes; index += 3)
            {
                if (captureTiming && useBoundsCull)
                {
                    ++captureTiming->anchorTriangleTests;
                }
                triIndex_t sourceTriangle[3];
                if (!SmokeTryCopyMemory(sourceTriangle, sourceIndexes + index, sizeof(sourceTriangle)))
                {
                    break;
                }
                const int i0 = sourceTriangle[0];
                const int i1 = sourceTriangle[1];
                const int i2 = sourceTriangle[2];
                if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= tri->numVerts || i1 >= tri->numVerts || i2 >= tri->numVerts)
                {
                    continue;
                }

                idVec3 p0;
                idVec3 p1;
                idVec3 p2;
                const PathTraceSmokeVertex cachedV0 = BuildSmokeSurfaceVertexFromSource(drawSurf, sourceVertices, i0, rtCpuSkinningJoints);
                const PathTraceSmokeVertex cachedV1 = BuildSmokeSurfaceVertexFromSource(drawSurf, sourceVertices, i1, rtCpuSkinningJoints);
                const PathTraceSmokeVertex cachedV2 = BuildSmokeSurfaceVertexFromSource(drawSurf, sourceVertices, i2, rtCpuSkinningJoints);
                p0.Set(cachedV0.position[0], cachedV0.position[1], cachedV0.position[2]);
                p1.Set(cachedV1.position[0], cachedV1.position[1], cachedV1.position[2]);
                p2.Set(cachedV2.position[0], cachedV2.position[1], cachedV2.position[2]);
                if (IsZeroAreaSmokeTriangle(p0, p1, p2))
                {
                    continue;
                }

                float hitDistance = 0.0f;
                if (IntersectRayTriangle(rayOrigin, rayDirection, p0, p1, p2, hitDistance) && hitDistance < closestHit)
                {
                    closestHit = hitDistance;
                    anchorPoint = rayOrigin + rayDirection * hitDistance;
                    anchorSurface = surfaceIndex;
                    anchorTriangle = index / 3;
                    foundHit = true;
                }
            }
        }
    }

    return foundHit;
}

static bool SmokeTryCopyMemory(void* destination, const void* source, size_t byteCount)
{
#if defined(_MSC_VER) && defined(_WIN32)
    __try
    {
        memcpy(destination, source, byteCount);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
#else
    memcpy(destination, source, byteCount);
    return true;
#endif
}

static PathTraceSmokeVertex BuildSmokeSurfaceVertexFromSource(const drawSurf_t* drawSurf, const idDrawVert* sourceVertices, int vertexIndex, const idJointMat* rtCpuSkinningJoints)
{
    idDrawVert drawVert;
    if (!sourceVertices || vertexIndex < 0 ||
        !SmokeTryCopyMemory(&drawVert, sourceVertices + vertexIndex, sizeof(drawVert)))
    {
        // Dynamic frontend geometry can be retired while the backend draw
        // surface still retains its CPU fallback pointer. Treat that surface as
        // degenerate for this capture instead of allowing an access violation.
        return {};
    }
    idVec3 localPosition = drawVert.xyz;
    idVec3 localNormal = drawVert.GetNormal();
    idVec3 localTangent = drawVert.GetTangent();
    idVec3 localBitangent = drawVert.GetBiTangent();
    const float bitangentSign = drawVert.GetBiTangentSign();
    if (rtCpuSkinningJoints)
    {
        localPosition = TransformSmokeSkinnedVertexPosition(drawVert, rtCpuSkinningJoints);
        localNormal = TransformSmokeSkinnedVertexNormal(drawVert, rtCpuSkinningJoints);
        localTangent = TransformSmokeSkinnedVertexTangent(drawVert, rtCpuSkinningJoints);
        localBitangent = TransformSmokeSkinnedVertexBitangent(drawVert, rtCpuSkinningJoints);
    }

    idVec3 worldPosition;
    idVec3 worldNormal;
    idVec3 worldTangent;
    idVec3 worldBitangent;
    TransformSurfacePointToWorld(drawSurf, localPosition, worldPosition);
    TransformSurfaceVectorToWorld(drawSurf, localNormal, worldNormal);
    TransformSurfaceVectorToWorld(drawSurf, localTangent, worldTangent);
    TransformSurfaceVectorToWorld(drawSurf, localBitangent, worldBitangent);
    worldNormal.Normalize();
    if (worldTangent.Normalize() == 0.0f)
    {
        worldTangent.Set(1.0f, 0.0f, 0.0f);
    }
    if (worldBitangent.Normalize() == 0.0f)
    {
        worldBitangent.Cross(worldNormal, worldTangent);
        worldBitangent *= bitangentSign;
        worldBitangent.Normalize();
    }

    const idVec2 texCoord = drawVert.GetTexCoord();
    idVec2 normalTexCoord = texCoord;
    const idMaterial* material = drawSurf ? drawSurf->material : nullptr;
    const float* materialRegisters = drawSurf && drawSurf->shaderRegisters
        ? drawSurf->shaderRegisters
        : (material ? material->ConstantRegisters() : nullptr);
    const int materialRegisterCount = material ? material->GetNumRegisters() : 0;
    if (material && materialRegisters)
    {
        for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
        {
            const shaderStage_t* stage = material->GetStage(stageIndex);
            if (!stage || stage->lighting != SL_BUMP || !stage->texture.hasMatrix)
            {
                continue;
            }

            float matrix[2][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
            for (int row = 0; row < 2; ++row)
            {
                for (int column = 0; column < 3; ++column)
                {
                    const int registerIndex = stage->texture.matrix[row][column];
                    if (registerIndex >= 0 && registerIndex < materialRegisterCount)
                    {
                        matrix[row][column] = materialRegisters[registerIndex];
                    }
                }
            }
            normalTexCoord.Set(
                matrix[0][0] * texCoord.x + matrix[0][1] * texCoord.y + matrix[0][2],
                matrix[1][0] * texCoord.x + matrix[1][1] * texCoord.y + matrix[1][2]);
            break;
        }
    }
    PathTraceSmokeVertex vertex = {};
    vertex.position[0] = worldPosition.x;
    vertex.position[1] = worldPosition.y;
    vertex.position[2] = worldPosition.z;
    vertex.position[3] = 1.0f;
    vertex.normal[0] = worldNormal.x;
    vertex.normal[1] = worldNormal.y;
    vertex.normal[2] = worldNormal.z;
    vertex.normal[3] = 0.0f;
    vertex.texCoord[0] = texCoord.x;
    vertex.texCoord[1] = texCoord.y;
    vertex.texCoord[2] = normalTexCoord.x;
    vertex.texCoord[3] = normalTexCoord.y;
    vertex.color[0] = drawVert.color[0] * (1.0f / 255.0f);
    vertex.color[1] = drawVert.color[1] * (1.0f / 255.0f);
    vertex.color[2] = drawVert.color[2] * (1.0f / 255.0f);
    vertex.color[3] = drawVert.color[3] * (1.0f / 255.0f);
    vertex.color2[0] = drawVert.color2[0] * (1.0f / 255.0f);
    vertex.color2[1] = drawVert.color2[1] * (1.0f / 255.0f);
    vertex.color2[2] = drawVert.color2[2] * (1.0f / 255.0f);
    vertex.color2[3] = drawVert.color2[3] * (1.0f / 255.0f);
    vertex.tangent[0] = worldTangent.x;
    vertex.tangent[1] = worldTangent.y;
    vertex.tangent[2] = worldTangent.z;
    vertex.tangent[3] = bitangentSign;
    vertex.bitangent[0] = worldBitangent.x;
    vertex.bitangent[1] = worldBitangent.y;
    vertex.bitangent[2] = worldBitangent.z;
    vertex.bitangent[3] = 0.0f;
    return vertex;
}

PathTraceSmokeVertex BuildSmokeSurfaceVertex(const drawSurf_t* drawSurf, const srfTriangles_t* tri, int vertexIndex, const idJointMat* rtCpuSkinningJoints)
{
    return BuildSmokeSurfaceVertexFromSource(drawSurf, tri->verts, vertexIndex, rtCpuSkinningJoints);
}

void TransformSmokeSurfaceVertexToWorld(const drawSurf_t* drawSurf, const srfTriangles_t* tri, int vertexIndex, const idJointMat* rtCpuSkinningJoints, idVec3& worldPosition)
{
    const PathTraceSmokeVertex vertex = BuildSmokeSurfaceVertex(drawSurf, tri, vertexIndex, rtCpuSkinningJoints);
    worldPosition.Set(vertex.position[0], vertex.position[1], vertex.position[2]);
}

static bool SmokeMaterialRegistersHaveActiveEmissiveStageWithClassifier(
    const idMaterial* material,
    const float* regs,
    const RtSmokeTranslucentClassifierInfo& classifier)
{
    if (!material)
    {
        return false;
    }

    const bool nameLooksEmissive = !classifier.hasAddDefault0200Texture && (classifier.nameLooksGlow || classifier.nameLooksSignage);
    const int registerCount = material->GetNumRegisters();
    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || stage->lighting != SL_AMBIENT || !stage->texture.image)
        {
            continue;
        }

        if (regs && stage->conditionRegister >= 0 && stage->conditionRegister < registerCount && regs[stage->conditionRegister] == 0.0f)
        {
            continue;
        }

        const uint64 srcBlend = stage->drawStateBits & GLS_SRCBLEND_BITS;
        const uint64 dstBlend = stage->drawStateBits & GLS_DSTBLEND_BITS;
        const bool ambientBlendStage = dstBlend != GLS_DSTBLEND_ZERO || srcBlend == GLS_SRCBLEND_DST_COLOR || srcBlend == GLS_SRCBLEND_ONE_MINUS_DST_COLOR;
        if (!SmokeStageIsAdditiveBlend(stage) && !(nameLooksEmissive && ambientBlendStage && !classifier.nameLooksDecal))
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

        const float stageLuminance = Max(stageColor.x, Max(stageColor.y, stageColor.z)) * Max(stageColor.w, 0.0f);
        if (stageLuminance > 1.0e-4f)
        {
            return true;
        }
    }

    return false;
}

static bool SmokeMaterialRegistersHaveActiveEmissiveStage(const idMaterial* material, const float* regs)
{
    if (!material)
    {
        return false;
    }

    const RtSmokeTranslucentClassifierInfo classifier = BuildSmokeTranslucentClassifierInfo(material);
    return SmokeMaterialRegistersHaveActiveEmissiveStageWithClassifier(material, regs, classifier);
}

bool SmokeDrawSurfaceHasActiveEmissiveStage(const drawSurf_t* drawSurf)
{
    const idMaterial* material = drawSurf ? drawSurf->material : nullptr;
    const float* regs = drawSurf && drawSurf->shaderRegisters ? drawSurf->shaderRegisters : (material ? material->ConstantRegisters() : nullptr);
    return SmokeMaterialRegistersHaveActiveEmissiveStage(material, regs);
}

static bool AddPathTraceCaptureCount(std::size_t& value, std::size_t add)
{
    if (add > std::numeric_limits<std::size_t>::max() - value)
    {
        return false;
    }
    value += add;
    return true;
}

static bool CapturePathTraceRuntimeMaterialStages(
    const idMaterial* material,
    RtPathTraceRuntimeMaterialStagePod* stages,
    int stageCapacity);

static bool PathTraceOwnerSnapshotModelFacts(
    const viewDef_t* viewDef,
    const drawSurf_t* drawSurf,
    const idRenderModel*& model,
    std::uint64_t& modelBits,
    std::uint64_t& modelEpoch)
{
    model = nullptr;
    modelBits = 0;
    modelEpoch = 0;
    if (!viewDef || !drawSurf || !drawSurf->frontEndGeo ||
        !drawSurf->material || !drawSurf->space)
    {
        return false;
    }
    const viewEntity_t* space = drawSurf->space;
    const idRenderEntityLocal* entity = space->entityDef;
    const bool worldSpace = space == &viewDef->worldSpace;
    const bool entityLive = SmokeRenderWorldContainsEntity(viewDef, entity);
    if (!worldSpace && !IsSmokeGuiDrawSurface(drawSurf) && !entityLive)
    {
        return false;
    }
    const renderEntity_t* renderEntity = entityLive ? &entity->parms : nullptr;
    model = renderEntity ? renderEntity->hModel : nullptr;
    if (!model)
    {
        return false;
    }
    const PtRenderDefKey key = PtGeometryLifecycle::MakeEntityKey(entity);
    modelBits = static_cast<std::uint64_t>(
        reinterpret_cast<std::uintptr_t>(model));
    modelEpoch = key.world && key.index >= 0
        ? PtGeometryLifecycle::EntityModelEpoch(key.world, key.index) : 0;
    return true;
}

static bool CountPathTraceOwnerModelTokenTables(
    const viewDef_t* viewDef,
    RtPathTraceCaptureCapacityCounts& counts)
{
    for (int ordinal = 0; ordinal < viewDef->numDrawSurfs; ++ordinal)
    {
        const idRenderModel* model = nullptr;
        std::uint64_t modelBits = 0;
        std::uint64_t modelEpoch = 0;
        if (!PathTraceOwnerSnapshotModelFacts(viewDef,
                viewDef->drawSurfs[ordinal], model, modelBits, modelEpoch))
        {
            continue;
        }
        bool duplicate = false;
        for (int previous = 0; previous < ordinal; ++previous)
        {
            const idRenderModel* previousModel = nullptr;
            std::uint64_t previousBits = 0;
            std::uint64_t previousEpoch = 0;
            if (PathTraceOwnerSnapshotModelFacts(viewDef,
                    viewDef->drawSurfs[previous], previousModel,
                    previousBits, previousEpoch) &&
                previousBits == modelBits && previousEpoch == modelEpoch)
            {
                duplicate = true;
                break;
            }
        }
        if (duplicate)
        {
            continue;
        }
        const int modelSurfaceCount = model->NumSurfaces();
        if (modelSurfaceCount < 0 ||
            !AddPathTraceCaptureCount(counts.modelTables, 1) ||
            !AddPathTraceCaptureCount(counts.modelSurfaceTokens,
                static_cast<std::size_t>(modelSurfaceCount)))
        {
            return false;
        }
    }
    return true;
}

static bool CapturePathTraceOwnerModelTokenTable(
    const idRenderModel* model,
    std::uint64_t modelBits,
    std::uint64_t modelEpoch,
    RtPathTraceCaptureOwnerSnapshot& snapshot,
    std::uint32_t& modelTableIndex)
{
    modelTableIndex = RT_PT_CAPTURE_INVALID_MODEL_TABLE;
    if (!model)
    {
        return true;
    }
    const std::uint32_t existing = FindPathTraceCaptureModelTokenTableFromPod(
        modelBits, modelEpoch, snapshot.modelTables.data(),
        snapshot.modelTables.size());
    if (existing != RT_PT_CAPTURE_INVALID_MODEL_TABLE)
    {
        modelTableIndex = existing;
        return true;
    }
    const int surfaceCount = model->NumSurfaces();
    if (surfaceCount < 0 || snapshot.modelTables.size() >=
            snapshot.modelTables.capacity() ||
        static_cast<std::size_t>(surfaceCount) >
            snapshot.modelSurfaceTokens.capacity() -
                snapshot.modelSurfaceTokens.size() ||
        snapshot.modelTables.size() > UINT32_MAX ||
        snapshot.modelSurfaceTokens.size() > UINT32_MAX)
    {
        return false;
    }
    RtPathTraceCaptureModelTokenTablePod row;
    row.modelBits = modelBits;
    row.modelEpoch = modelEpoch;
    row.tokenOffset = static_cast<std::uint32_t>(
        snapshot.modelSurfaceTokens.size());
    row.tokenCount = static_cast<std::uint32_t>(surfaceCount);
    modelTableIndex = static_cast<std::uint32_t>(snapshot.modelTables.size());
    snapshot.modelTables.push_back(row);
    for (int surfaceIndex = 0; surfaceIndex < surfaceCount; ++surfaceIndex)
    {
        const modelSurface_t* surface = model->Surface(surfaceIndex);
        snapshot.modelSurfaceTokens.push_back(static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(
                surface ? surface->geometry : nullptr)));
    }
    return true;
}

static bool CountPathTraceOwnerRawSnapshot(
    const viewDef_t* viewDef,
    RtPathTraceCaptureCapacityCounts& counts)
{
    counts.surfaces = static_cast<std::size_t>(viewDef->numDrawSurfs);
    counts.materialInfoIntents = counts.surfaces;
    counts.materialVariantProposals = counts.surfaces;
    counts.instanceObservationProposals = counts.surfaces;
    counts.rigidCandidateProposals = counts.surfaces;
    counts.preparedRigidPayloads = counts.surfaces;
    counts.receiptSurfaces = counts.surfaces;
    for (int ordinal = 0; ordinal < viewDef->numDrawSurfs; ++ordinal)
    {
        const drawSurf_t* drawSurf = viewDef->drawSurfs[ordinal];
        if (!drawSurf)
        {
            continue;
        }
        const idMaterial* material = drawSurf->material;
        if (material)
        {
            if (!AddPathTraceCaptureCount(counts.classifierStages,
                    static_cast<std::size_t>(Max(0, material->GetNumStages()))) ||
                !AddPathTraceCaptureCount(counts.runtimeStages,
                    static_cast<std::size_t>(Max(0, material->GetNumStages()))) ||
                !AddPathTraceCaptureCount(counts.registers,
                    static_cast<std::size_t>(Max(0, material->GetNumRegisters()))))
            {
                return false;
            }
        }
        const srfTriangles_t* tri = drawSurf->frontEndGeo;
        if (!tri || tri->numVerts < 0 || tri->numIndexes < 0 ||
            drawSurf->numIndexes < 0)
        {
            continue;
        }
        if (!AddPathTraceCaptureCount(counts.vertices,
                static_cast<std::size_t>(tri->numVerts)) ||
            !AddPathTraceCaptureCount(counts.indexes,
                static_cast<std::size_t>(tri->numIndexes)))
        {
            return false;
        }
        bool verticesFromFrameCache = false;
        (void)SmokeDrawSurfaceVertices(drawSurf, tri, verticesFromFrameCache);
        const idJointMat* joints = SmokeDrawSurfaceCpuSkinningJoints(
            drawSurf, tri, verticesFromFrameCache);
        if (joints)
        {
            int jointCount = 0;
            if (drawSurf->jointCacheCpuSnapshot == joints)
            {
                jointCount = drawSurf->jointCacheCpuSnapshotCount;
            }
            else if (tri->staticModelWithJoints)
            {
                jointCount = tri->staticModelWithJoints->numInvertedJoints;
            }
            if (jointCount > 0 && jointCount <= 4096 &&
                !AddPathTraceCaptureCount(counts.joints,
                    static_cast<std::size_t>(jointCount)))
            {
                return false;
            }
        }
    }
    counts.preparedRigidVertices = counts.vertices;
    counts.preparedRigidIndexes = counts.indexes;
    return CountPathTraceOwnerModelTokenTables(viewDef, counts);
}

class RtPathTraceLiveSemanticFactsProvider final :
    public RtPathTraceCaptureSemanticFactsProvider
{
public:
    explicit RtPathTraceLiveSemanticFactsProvider(
        RtSmokeGeometryUniverse& universe) : geometryUniverse(universe) {}

    bool IsRigidRouteReady(std::uint64_t meshHash) const override
    {
        return geometryUniverse.IsRigidRouteReady(meshHash);
    }
    bool IsRigidRouteResidentReadyForEntityMaterial(
        std::int32_t entityIndex, std::int32_t entityNum,
        std::uint32_t materialId) const override
    {
        return geometryUniverse.IsRigidRouteResidentReadyForEntityMaterial(
            entityIndex, entityNum, materialId);
    }
    const PtGeometryIdentityBinding* FindCanonicalIdentityBinding(
        const PtCanonicalInstanceKey& instance) const override
    {
        return geometryUniverse.FindCanonicalIdentityBinding(instance);
    }
    const PtGeometrySourceRecord* FindCanonicalSourceRecord(
        const PtCanonicalMeshKey& mesh) const override
    {
        return geometryUniverse.FindCanonicalSourceRecord(mesh);
    }

private:
    RtSmokeGeometryUniverse& geometryUniverse;
};

static bool CapturePathTraceOwnerSnapshotInternal(
    const viewDef_t* viewDef,
    RtSmokeGeometryUniverse* geometryUniverse,
    RtPathTraceInstanceUniverse* instanceUniverse,
    const RtPathTracePlanningSnapshotEpoch& epoch,
    bool recordAllInstanceClasses,
    bool removeRoutedRigidDynamic,
    bool rigidRouteEmissiveCards,
    const PtSkinnedHitRouteRecord* skinnedAdmissionRoutes,
    std::size_t skinnedAdmissionRouteCount,
    bool skinnedCaptureSplitGate,
    bool sourceOnly,
    RtPathTraceCaptureOwnerSnapshot& snapshot,
    std::size_t& slotBytes)
{
    OPTICK_EVENT("PT Producer Lanes Snapshot");
    if (!viewDef || !viewDef->drawSurfs || viewDef->numDrawSurfs < 0 ||
        (skinnedAdmissionRouteCount != 0 && !skinnedAdmissionRoutes) ||
        (!sourceOnly && (!geometryUniverse || !instanceUniverse ||
            !RtPathTracePlanningEpochValid(epoch))))
    {
        snapshot.ResetAndRelease();
        return false;
    }

    RtPathTraceCaptureOwnerSnapshot candidate;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try
    {
#endif
        candidate.epoch = epoch;
        candidate.viewIdentity = static_cast<std::uint64_t>(
            reinterpret_cast<std::uintptr_t>(viewDef));
        candidate.registryGeneration = SmokeMaterialTextureRegistryGeneration();
        candidate.residentMaterialFactsGeneration =
            SmokeResidentMaterialFactsGeneration();
        candidate.sourceDrawSurfCount =
            static_cast<std::uint32_t>(viewDef->numDrawSurfs);
        candidate.recordAllInstanceClasses = recordAllInstanceClasses;
        candidate.removeRoutedRigidDynamic = removeRoutedRigidDynamic;
        candidate.rigidRouteEmissiveCards = rigidRouteEmissiveCards;
        const RtSmokeGeometryAdmissionBudget admission =
            BuildSmokeDynamicGeometryAdmissionBudget();
        candidate.admissionMaxSurfaces = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(admission.maxSurfaces,
                static_cast<std::uint64_t>(INT_MAX)));
        candidate.admissionMaxBytes = admission.maxBytes;
        RtPathTraceInstanceUniverseSnapshotCounts instanceCounts;
        RtSmokeGeometryUniverseSnapshotCounts geometryCounts;
        const RtSmokeMaterialTextureRegistryEnumerationCounts registryCounts =
            CountSmokeMaterialTextureRegistryEnumeration();
        candidate.capacityCounts.applyGateKeys =
            PtCpuProducerApplyGate::SnapshotKeyCount();
        candidate.capacityCounts.variantBases = registryCounts.variantBases;
        candidate.capacityCounts.registryMaterials = registryCounts.registryMaterials;
        if ((!sourceOnly &&
                (!instanceUniverse->CountInstanceUniverseSnapshot(epoch, instanceCounts) ||
                 !geometryUniverse->CountGeometryUniversePlanningSnapshot(epoch, geometryCounts))) ||
            !CountPathTraceOwnerRawSnapshot(viewDef, candidate.capacityCounts))
        {
            candidate.ResetAndRelease();
            snapshot.ResetAndRelease();
            return false;
        }
        candidate.capacityCounts.instanceMeshes = instanceCounts.meshes;
        candidate.capacityCounts.instanceHistories = instanceCounts.histories;
        candidate.capacityCounts.geometryStaticSurfaces = geometryCounts.staticSurfaces;
        candidate.capacityCounts.geometryRigidRoutes = geometryCounts.rigidRoutes;
        candidate.capacityCounts.geometryRigidResidents = geometryCounts.rigidResidents;

        RtPathTraceCaptureProductCapacityPlan completePlan;
        if (!PlanPathTraceCompleteSlotCapacity(candidate.capacityCounts,
                sizeof(candidate) + slotBytes, completePlan))
        {
            candidate.ResetAndRelease();
            snapshot.ResetAndRelease();
            return false;
        }
        candidate.plannedCompleteSlotBytes = completePlan.peakSlotBytes;
        const std::size_t nonSnapshotBytes = completePlan.candidateBytes +
            completePlan.oracleBytes;
        if (!ReservePathTraceCaptureOwnerSnapshotStorage(
                candidate, slotBytes, completePlan))
        {
            candidate.ResetAndRelease();
            snapshot.ResetAndRelease();
            return false;
        }
        if ((!sourceOnly &&
                (!PtCpuProducerApplyGate::FillSnapshotPreReserved(candidate.applyGate,
                    candidate.capacityCounts.applyGateKeys) ||
                 !geometryUniverse->FillGeometryUniversePlanningSnapshotPreReserved(
                    candidate.geometryUniverse, epoch, geometryCounts) ||
                 !instanceUniverse->FillInstanceUniverseSnapshotPreReserved(
                    candidate.instanceUniverse, epoch, instanceCounts))) ||
            !FillSmokeMaterialTextureRegistryEnumerationPreReserved(
                registryCounts, candidate.variantBases, candidate.registryMaterials))
        {
            candidate.ResetAndRelease();
            snapshot.ResetAndRelease();
            return false;
        }
        candidate.historyCopyUs = candidate.instanceUniverse.historyCopyUs;
        if (candidate.OwnedBytes() + nonSnapshotBytes + slotBytes >
            RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES)
        {
            candidate.ResetAndRelease();
            snapshot.ResetAndRelease();
            return false;
        }

        for (int ordinal = 0; ordinal < viewDef->numDrawSurfs; ++ordinal)
        {
            const drawSurf_t* drawSurf = viewDef->drawSurfs[ordinal];
            RtPathTraceCaptureRawSurface raw;
            raw.ordinal = static_cast<std::uint32_t>(ordinal);
            raw.guiAllowed = r_pathTracingAllowGuiSurfaces.GetInteger() != 0;
            raw.callbackAllowed = true;
            if (!drawSurf)
            {
                raw.safety = RtPathTraceCaptureSafetyDisposition::NullSurface;
                candidate.surfaces.push_back(raw);
                continue;
            }
            const srfTriangles_t* tri = drawSurf->frontEndGeo;
            if (!tri)
            {
                raw.safety = RtPathTraceCaptureSafetyDisposition::MissingGeometry;
                candidate.surfaces.push_back(raw);
                continue;
            }
            raw.triIdentityBits = static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(tri));
            raw.currentTriToken = raw.triIdentityBits;
            raw.ambientHandle = static_cast<std::uint64_t>(tri->ambientCache);
            raw.indexHandle = static_cast<std::uint64_t>(tri->indexCache);
            raw.jointHandle = static_cast<std::uint64_t>(drawSurf->jointCache);
            raw.vertexBufferIdentity = static_cast<std::uint64_t>(
                drawSurf->ambientCache != 0 ? drawSurf->ambientCache : tri->ambientCache);
            raw.indexBufferIdentity = static_cast<std::uint64_t>(
                drawSurf->indexCache != 0 ? drawSurf->indexCache : tri->indexCache);
            raw.extraGLState = static_cast<std::uint64_t>(drawSurf->extraGLState);
            raw.requestedModelSurfaceIndex = drawSurf->modelSurfaceIndex;
            const idMaterial* material = drawSurf->material;
            if (!material)
            {
                raw.safety = RtPathTraceCaptureSafetyDisposition::MissingMaterial;
                candidate.surfaces.push_back(raw);
                continue;
            }
            raw.materialIdentityBits = static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(material));
            RtPathTracePlanningCopyName(raw.materialName,
                sizeof(raw.materialName), material->GetName());
            const viewEntity_t* space = drawSurf->space;
            if (!space)
            {
                raw.safety = RtPathTraceCaptureSafetyDisposition::MissingSpace;
                candidate.surfaces.push_back(raw);
                continue;
            }
            const idRenderEntityLocal* entity = space->entityDef;
            const bool worldSpace = space == &viewDef->worldSpace;
            const bool entityLive = SmokeRenderWorldContainsEntity(viewDef, entity);
            if (!worldSpace && !IsSmokeGuiDrawSurface(drawSurf) && !entityLive)
            {
                raw.safety =
                    RtPathTraceCaptureSafetyDisposition::WorldContainmentRejected;
                candidate.surfaces.push_back(raw);
                continue;
            }
            const renderEntity_t* renderEntity = entityLive ? &entity->parms : nullptr;
            const idRenderModel* model = renderEntity ? renderEntity->hModel : nullptr;
            raw.entityIndex = entity ? entity->index : -1;
            raw.entityNum = renderEntity ? renderEntity->entityNum : -1;
            raw.currentArea = -1;
            raw.entityDefBits = static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(entity));
            raw.modelBits = static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(model));
            const PtRenderDefKey renderDefKey =
                PtGeometryLifecycle::MakeEntityKey(entity);
            raw.renderWorldIdentity = static_cast<std::uint64_t>(
                reinterpret_cast<std::uintptr_t>(renderDefKey.world));
            raw.renderDefIndex = renderDefKey.index;
            raw.renderDefGeneration = renderDefKey.generation;
            raw.modelEpoch = (renderDefKey.world && renderDefKey.index >= 0)
                ? PtGeometryLifecycle::EntityModelEpoch(
                    renderDefKey.world, renderDefKey.index)
                : 0;
            if (!CapturePathTraceOwnerModelTokenTable(
                    model, raw.modelBits, raw.modelEpoch,
                    candidate, raw.modelTableIndex))
            {
                candidate.ResetAndRelease();
                snapshot.ResetAndRelease();
                return false;
            }
            if (model)
            {
                RtPathTracePlanningCopyName(raw.modelName,
                    sizeof(raw.modelName), model->Name());
            }
            std::memcpy(raw.modelMatrix, space->modelMatrix, sizeof(raw.modelMatrix));
            std::memcpy(raw.objectToWorld, space->modelMatrix, sizeof(raw.objectToWorld));
            raw.classify.isWorldSpace = worldSpace;
            raw.classify.hasEntityDef = entity != nullptr;
            raw.classify.hasJointCache = drawSurf->jointCache != 0;
            raw.classify.hasStaticModelWithJoints =
                tri->staticModelWithJoints != nullptr;
            raw.classify.hasRenderEntityJoints = renderEntity &&
                renderEntity->joints != nullptr && renderEntity->numJoints > 0;
            raw.classify.ambientCacheIsStatic =
                idVertexCache::CacheIsStatic(drawSurf->ambientCache);
            raw.classify.indexCacheIsStatic =
                idVertexCache::CacheIsStatic(drawSurf->indexCache);
            raw.classify.modelDepthHack = space->modelDepthHack;
            raw.weaponDepthHack = space->weaponDepthHack;
            raw.allowSurfaceInView = renderEntity &&
                renderEntity->allowSurfaceInViewID != 0;
            raw.classify.material.materialPresent = true;
            RtPathTracePlanningCopyName(raw.classify.material.materialName,
                sizeof(raw.classify.material.materialName), raw.materialName);
            raw.classify.material.coverage = static_cast<int>(material->Coverage());
            raw.classify.material.deform = static_cast<int>(material->Deform());
            raw.classify.material.stageCount = material->GetNumStages();
            raw.classify.material.sort = material->GetSort();
            raw.classify.material.guiSurface = IsSmokeGuiDrawSurface(drawSurf);
            raw.classify.material.polygonOffset = material->TestMaterialFlag(MF_POLYGONOFFSET);
            raw.classify.material.hasAlphaTest = false;
            raw.particleCompositeEnabled =
                r_pathTracingParticleComposite.GetInteger() != 0;
            raw.liquidPoolEnabled = r_pathTracingLiquidPoolMode.GetInteger() != 0 ||
                r_pathTracingLiquidPoolDebug.GetInteger() != 0;
            raw.unifiedPtEnabled = r_pathTracingUnifiedPtEnable.GetInteger() != 0;
            raw.removeAlphaClipEnabled =
                r_pathTracingUnifiedPtRemoveAlphaClipSurfaces.GetInteger() != 0;
            const idVec3 rawBoundsCenter = tri->bounds.GetCenter();
            raw.boundsCenter[0] = rawBoundsCenter.x;
            raw.boundsCenter[1] = rawBoundsCenter.y;
            raw.boundsCenter[2] = rawBoundsCenter.z;
            for (int axis = 0; axis < 3; ++axis)
            {
                raw.triangleBoundsMin[axis] = tri->bounds[0][axis];
                raw.triangleBoundsMax[axis] = tri->bounds[1][axis];
            }
            const shaderStage_t* onlyStage = material->GetNumStages() == 1
                ? material->GetStage(0) : nullptr;
            if (onlyStage)
            {
                const uint64 srcBlend = onlyStage->drawStateBits & GLS_SRCBLEND_BITS;
                const uint64 dstBlend = onlyStage->drawStateBits & GLS_DSTBLEND_BITS;
                raw.classify.material.singleStageAmbientAlphaBlend =
                    onlyStage->lighting == SL_AMBIENT &&
                    srcBlend == GLS_SRCBLEND_SRC_ALPHA &&
                    dstBlend == GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA;
            }
            raw.callbackAllowed = !(renderEntity && renderEntity->callback &&
                renderEntity->customShader != nullptr &&
                r_pathTracingSkipCallbackEntities.GetInteger() != 0);
            raw.entityCallbackPresent = renderEntity && renderEntity->callback != nullptr;
            raw.entityForceUpdate = renderEntity && renderEntity->forceUpdate != 0;
            raw.dynamicModelPresent = entity && entity->dynamicModel != nullptr;
            raw.cachedDynamicModelPresent = entity && entity->cachedDynamicModel != nullptr;
            raw.customShaderPresent = renderEntity && renderEntity->customShader != nullptr;
            raw.customSkinPresent = renderEntity && renderEntity->customSkin != nullptr;

            const int stageCount = material->GetNumStages();
            raw.classifierStageOffset =
                static_cast<std::uint32_t>(candidate.classifierStages.size());
            raw.classifierStageCount = static_cast<std::uint32_t>(Max(0, stageCount));
            const std::size_t oldStageSize = candidate.classifierStages.size();
            if (oldStageSize + raw.classifierStageCount >
                candidate.classifierStages.capacity())
            {
                candidate.ResetAndRelease();
                snapshot.ResetAndRelease();
                return false;
            }
            candidate.classifierStages.resize(oldStageSize + raw.classifierStageCount);
            if (!CaptureSmokeTranslucentClassifierInput(
                    material, raw.classifier,
                    raw.classifierStageCount != 0
                        ? candidate.classifierStages.data() + oldStageSize : nullptr,
                    static_cast<int>(raw.classifierStageCount)))
            {
                raw.safety = RtPathTraceCaptureSafetyDisposition::CopyFailed;
                candidate.surfaces.push_back(raw);
                continue;
            }
            raw.runtimeStageOffset =
                static_cast<std::uint32_t>(candidate.runtimeStages.size());
            raw.runtimeStageCount = raw.classifierStageCount;
            const std::size_t oldRuntimeStageSize = candidate.runtimeStages.size();
            if (oldRuntimeStageSize + raw.runtimeStageCount >
                candidate.runtimeStages.capacity())
            {
                candidate.ResetAndRelease();
                snapshot.ResetAndRelease();
                return false;
            }
            candidate.runtimeStages.resize(
                oldRuntimeStageSize + raw.runtimeStageCount);
            if (!CapturePathTraceRuntimeMaterialStages(
                    material,
                    raw.runtimeStageCount != 0
                        ? candidate.runtimeStages.data() + oldRuntimeStageSize
                        : nullptr,
                    static_cast<int>(raw.runtimeStageCount)))
            {
                raw.safety = RtPathTraceCaptureSafetyDisposition::CopyFailed;
                candidate.surfaces.push_back(raw);
                continue;
            }
            raw.registerOffset = static_cast<std::uint32_t>(candidate.registers.size());
            raw.registerCount = static_cast<std::uint32_t>(Max(0, material->GetNumRegisters()));
            const float* registers = drawSurf->shaderRegisters
                ? drawSurf->shaderRegisters : material->ConstantRegisters();
            raw.registersPresent = registers != nullptr;
            if (registers && raw.registerCount != 0)
            {
                const std::size_t oldRegisters = candidate.registers.size();
                if (oldRegisters + raw.registerCount > candidate.registers.capacity())
                {
                    candidate.ResetAndRelease();
                    snapshot.ResetAndRelease();
                    return false;
                }
                candidate.registers.resize(oldRegisters + raw.registerCount);
                if (!SmokeTryCopyMemory(candidate.registers.data() + oldRegisters,
                        registers, raw.registerCount * sizeof(float)))
                {
                    raw.safety = RtPathTraceCaptureSafetyDisposition::CopyFailed;
                    candidate.surfaces.push_back(raw);
                    continue;
                }
            }

            if (!tri->verts || !tri->indexes || tri->numVerts < 3 ||
                drawSurf->numIndexes < 3 || drawSurf->numIndexes > tri->numIndexes ||
                (drawSurf->numIndexes % 3) != 0 || (tri->numIndexes % 3) != 0)
            {
                raw.safety = RtPathTraceCaptureSafetyDisposition::MissingPayload;
                candidate.surfaces.push_back(raw);
                continue;
            }
            const RtSmokeLiveCacheVerdict cacheVerdict = EvaluateSmokeLiveCacheGate(
                tri->ambientCache, tri->indexCache, vertexCache.currentFrame, nullptr);
            if (SmokeLiveCacheGateRejects(cacheVerdict))
            {
                raw.safety = RtPathTraceCaptureSafetyDisposition::NonCurrentCache;
                candidate.surfaces.push_back(raw);
                continue;
            }
            bool verticesFromFrameCache = false;
            const idDrawVert* sourceVertices = SmokeDrawSurfaceVertices(
                drawSurf, tri, verticesFromFrameCache);
            const triIndex_t* sourceIndexes = SmokeDrawSurfaceIndexes(drawSurf, tri);
            if (!sourceVertices || !sourceIndexes)
            {
                raw.safety = RtPathTraceCaptureSafetyDisposition::MissingPayload;
                candidate.surfaces.push_back(raw);
                continue;
            }
            raw.vertexOffset = static_cast<std::uint32_t>(candidate.vertices.size());
            raw.vertexCount = static_cast<std::uint32_t>(tri->numVerts);
            raw.indexOffset = static_cast<std::uint32_t>(candidate.indexes.size());
            raw.indexCount = static_cast<std::uint32_t>(drawSurf->numIndexes);
            raw.sourceTriIndexCount = static_cast<std::uint32_t>(tri->numIndexes);
            // Own the full serial-rigid index authority in the same bounded
            // vector. Dynamic emission still consumes only raw.indexCount.
            const std::size_t capturedIndexCount = raw.sourceTriIndexCount;
            if (candidate.vertices.size() + raw.vertexCount >
                    candidate.vertices.capacity() ||
                candidate.indexes.size() + capturedIndexCount >
                    candidate.indexes.capacity())
            {
                candidate.ResetAndRelease();
                snapshot.ResetAndRelease();
                return false;
            }
            candidate.vertices.resize(candidate.vertices.size() + raw.vertexCount);
            candidate.indexes.resize(candidate.indexes.size() + capturedIndexCount);
            if (!SmokeTryCopyMemory(candidate.vertices.data() + raw.vertexOffset,
                    sourceVertices, raw.vertexCount * sizeof(idDrawVert)) ||
                !SmokeTryCopyMemory(candidate.indexes.data() + raw.indexOffset,
                    sourceIndexes, capturedIndexCount * sizeof(triIndex_t)))
            {
                raw.safety = RtPathTraceCaptureSafetyDisposition::CopyFailed;
                candidate.surfaces.push_back(raw);
                continue;
            }
            const idJointMat* joints = SmokeDrawSurfaceCpuSkinningJoints(
                drawSurf, tri, verticesFromFrameCache);
            int jointCount = 0;
            if (joints)
            {
                if (drawSurf->jointCacheCpuSnapshot == joints)
                {
                    jointCount = drawSurf->jointCacheCpuSnapshotCount;
                }
                else if (tri->staticModelWithJoints)
                {
                    jointCount = tri->staticModelWithJoints->numInvertedJoints;
                }
                if (jointCount <= 0 || jointCount > 4096)
                {
                    raw.safety = RtPathTraceCaptureSafetyDisposition::CopyFailed;
                    candidate.surfaces.push_back(raw);
                    continue;
                }
                raw.jointOffset = static_cast<std::uint32_t>(candidate.joints.size());
                raw.jointCount = static_cast<std::uint32_t>(jointCount);
                if (candidate.joints.size() + raw.jointCount >
                    candidate.joints.capacity())
                {
                    candidate.ResetAndRelease();
                    snapshot.ResetAndRelease();
                    return false;
                }
                candidate.joints.resize(candidate.joints.size() + raw.jointCount);
                if (!SmokeTryCopyMemory(candidate.joints.data() + raw.jointOffset,
                        joints, raw.jointCount * sizeof(idJointMat)))
                {
                    raw.safety = RtPathTraceCaptureSafetyDisposition::CopyFailed;
                    candidate.surfaces.push_back(raw);
                    continue;
                }
                raw.rtCpuSkinned = true;
            }
            raw.bumpMatrix[0] = raw.bumpMatrix[4] = 1.0f;
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
                        const int reg = stage->texture.matrix[row][column];
                        if (registers && reg >= 0 && reg < material->GetNumRegisters())
                        {
                            raw.bumpMatrix[row * 3 + column] = registers[reg];
                        }
                    }
                }
                break;
            }
            raw.materialInfoRegistrationRequested = true;
            raw.triNumVerts = tri->numVerts > 0
                ? static_cast<std::uint32_t>(tri->numVerts) : 0u;
            raw.triNumIndexes = tri->numIndexes > 0
                ? static_cast<std::uint32_t>(tri->numIndexes) : 0u;
            raw.triTriangleCount = raw.triNumIndexes / 3u;
            raw.canonicalInstance.worldGeneration = renderDefKey.worldGeneration;
            raw.canonicalInstance.renderDefIndex = renderDefKey.index >= 0
                ? static_cast<std::uint32_t>(renderDefKey.index) : UINT32_MAX;
            raw.canonicalInstance.renderDefGeneration = renderDefKey.generation;
            raw.canonicalInstance.subInstanceKind =
                PtCanonicalSubInstanceKind::SkinnedSurface;
            raw.canonicalInstance.modelSurfaceIndex =
                drawSurf->modelSurfaceIndex >= 0
                    ? static_cast<std::uint32_t>(drawSurf->modelSurfaceIndex)
                    : UINT32_MAX;
            raw.canonicalInstance.jointSubmeshIndex = -1;
            if (raw.rtCpuSkinned)
            {
                raw.jointSource = reinterpret_cast<std::uintptr_t>(
                    verticesFromFrameCache
                        ? static_cast<const void*>(renderEntity
                            ? renderEntity->joints : nullptr)
                        : (tri->staticModelWithJoints
                            ? static_cast<const void*>(tri->staticModelWithJoints)
                            : static_cast<const void*>(renderEntity
                                ? renderEntity->joints : nullptr)));
            }
            raw.safety = RtPathTraceCaptureSafetyDisposition::Ready;
            candidate.surfaces.push_back(raw);
        }

        if (!sourceOnly)
        {
            RtPathTraceLiveSemanticFactsProvider facts(*geometryUniverse);
            if (!FinalizePathTraceOwnerSemanticSnapshot(candidate, facts,
                    skinnedAdmissionRoutes, skinnedAdmissionRouteCount,
                    skinnedCaptureSplitGate))
            {
                candidate.ResetAndRelease();
                snapshot.ResetAndRelease();
                return false;
            }
        }
        candidate.lateConsumeToken.mapTimeStamp = epoch.mapTimeStamp;
        candidate.lateConsumeToken.mapLoadSerial = epoch.mapLoadSerial;
        RtPathTracePlanningCopyName(candidate.lateConsumeToken.mapName,
            sizeof(candidate.lateConsumeToken.mapName), epoch.mapName);
        candidate.lateConsumeToken.capturedAfterBeginFrame =
            epoch.capturedAfterBeginFrame;
        candidate.lateConsumeToken.capturedAfterStaticPreload =
            epoch.capturedAfterStaticPreload;
        candidate.lateConsumeToken.registryGeneration =
            candidate.registryGeneration;
        candidate.lateConsumeToken.instanceUniverseGeneration =
            candidate.instanceUniverse.ownerGeneration;
        candidate.lateConsumeToken.geometryUniverseGeneration =
            candidate.geometryUniverse.ownerGeneration;
        const int producerMode = idMath::ClampInt(
            0, 2, r_pathTracingProducerLanes.GetInteger());
        const int producerLaneMask =
            r_pathTracingProducerLaneMask.GetInteger() & 0x7;
        candidate.lateConsumeToken.configFingerprint =
            RtPathTraceOwnerSemanticConfigFingerprint(candidate,
                producerMode, producerLaneMask, skinnedCaptureSplitGate);
        candidate.complete = true;
        const std::size_t ownedBytes = candidate.OwnedBytes();
        if (ownedBytes > RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES ||
            slotBytes > RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES - ownedBytes)
        {
            snapshot.ResetAndRelease();
            return false;
        }
        snapshot = std::move(candidate);
        slotBytes += ownedBytes;
        return true;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    }
    catch (const std::bad_alloc&)
    {
        snapshot.ResetAndRelease();
        return false;
    }
    catch (const std::length_error&)
    {
        snapshot.ResetAndRelease();
        return false;
    }
#endif
}

bool CapturePathTraceOwnerSnapshot(
    const viewDef_t* viewDef,
    RtSmokeGeometryUniverse& geometryUniverse,
    RtPathTraceInstanceUniverse& instanceUniverse,
    const RtPathTracePlanningSnapshotEpoch& epoch,
    bool recordAllInstanceClasses,
    bool removeRoutedRigidDynamic,
    bool rigidRouteEmissiveCards,
    const PtSkinnedHitRouteRecord* skinnedAdmissionRoutes,
    std::size_t skinnedAdmissionRouteCount,
    bool skinnedCaptureSplitGate,
    RtPathTraceCaptureOwnerSnapshot& snapshot,
    std::size_t& slotBytes)
{
    return CapturePathTraceOwnerSnapshotInternal(viewDef, &geometryUniverse,
        &instanceUniverse, epoch, recordAllInstanceClasses,
        removeRoutedRigidDynamic, rigidRouteEmissiveCards,
        skinnedAdmissionRoutes, skinnedAdmissionRouteCount,
        skinnedCaptureSplitGate, false, snapshot, slotBytes);
}

bool CapturePathTraceOwnerSourceSnapshot(
    const viewDef_t* viewDef,
    const RtPathTraceCommittedSemanticConfig& semanticConfig,
    RtPathTraceCaptureOwnerSnapshot& snapshot,
    std::size_t& slotBytes)
{
    if (!semanticConfig.configComplete ||
        semanticConfig.configFingerprint == 0)
    {
        return false;
    }
    const RtPathTracePlanningSnapshotEpoch noSemanticEpoch{};
    return CapturePathTraceOwnerSnapshotInternal(viewDef, nullptr, nullptr,
        noSemanticEpoch, semanticConfig.recordAllInstanceClasses,
        semanticConfig.removeRoutedRigidDynamic,
        semanticConfig.rigidRouteEmissiveCards, nullptr, 0, false, true,
        snapshot, slotBytes);
}

bool SmokeSkinnedCaptureSplitGateEnabled(bool admissionRoutesAvailable)
{
    const bool auditRequested =
        r_pathTracingGeometrySkinnedConsumerAudit.GetInteger() != 0 ||
        r_pathTracingGeometrySkinnedHitAudit.GetInteger() != 0 ||
        r_pathTracingGeometrySkinnedEmissiveAudit.GetInteger() != 0 ||
        r_pathTracingGeometrySkinnedAttributeDeriveDump.GetInteger() != 0;
    return r_pathTracingGeometrySkinnedCaptureSplit.GetInteger() != 0 &&
        r_pathTracingGeometrySkinnedTlasCompare.GetInteger() != 0 &&
        r_pathTracingGpuSkinning.GetInteger() == 1 &&
        r_pathTracingGeometryAuthoritativeGpuSkinning.GetInteger() != 0 &&
        r_pathTracingGeometryShadowRegistry.GetInteger() != 0 &&
        admissionRoutesAvailable && !auditRequested;
}

bool SmokeDrawSurfaceHasActiveEmissiveStage(
    const drawSurf_t* drawSurf,
    const RtSmokeTranslucentClassifierInfo& classifier)
{
    const idMaterial* material = drawSurf ? drawSurf->material : nullptr;
    const float* regs = drawSurf && drawSurf->shaderRegisters ? drawSurf->shaderRegisters : (material ? material->ConstantRegisters() : nullptr);
    return SmokeMaterialRegistersHaveActiveEmissiveStageWithClassifier(
        material, regs, classifier);
}

bool SmokeEntitySurfaceHasActiveEmissiveStage(const viewDef_t* viewDef, const idRenderEntityLocal* entity, const idMaterial* material)
{
    if (!material)
    {
        return false;
    }

    const RtSmokeTranslucentClassifierInfo classifier = BuildSmokeTranslucentClassifierInfo(material);
    return SmokeEntitySurfaceHasActiveEmissiveStage(viewDef, entity, material, classifier);
}

bool SmokeEntitySurfaceHasActiveEmissiveStage(
    const viewDef_t* viewDef,
    const idRenderEntityLocal* entity,
    const idMaterial* material,
    const RtSmokeTranslucentClassifierInfo& classifier)
{
    if (!viewDef || !entity || !material)
    {
        return false;
    }

    auto evalRegister = [](const float* regs, int registerCount, int registerIndex, float fallback, float& value) {
        if (regs && registerIndex >= 0 && registerIndex < registerCount)
        {
            value = regs[registerIndex];
            return;
        }
        value = fallback;
    };

    const renderEntity_t& renderEntity = entity->parms;
    const float* shaderParms = renderEntity.shaderParms;
    const float* globalParms = viewDef->renderView.shaderParms;
    const int timeGroup = idMath::ClampInt(0, 1, renderEntity.timeGroup);
    const float floatTime = viewDef->renderView.time[timeGroup] * 0.001f;

    float generatedShaderParms[MAX_ENTITY_SHADER_PARMS];
    if (renderEntity.referenceShader != nullptr)
    {
        float refRegs[MAX_EXPRESSION_REGISTERS];
        renderEntity.referenceShader->EvaluateRegisters(
            refRegs,
            shaderParms,
            globalParms,
            floatTime,
            renderEntity.referenceSound);

        const shaderStage_t* referenceStage = renderEntity.referenceShader->GetStage(0);
        memcpy(generatedShaderParms, shaderParms, sizeof(generatedShaderParms));
        if (referenceStage)
        {
            const int referenceRegisterCount = renderEntity.referenceShader->GetNumRegisters();
            evalRegister(refRegs, referenceRegisterCount, referenceStage->color.registers[0], generatedShaderParms[0], generatedShaderParms[0]);
            evalRegister(refRegs, referenceRegisterCount, referenceStage->color.registers[1], generatedShaderParms[1], generatedShaderParms[1]);
            evalRegister(refRegs, referenceRegisterCount, referenceStage->color.registers[2], generatedShaderParms[2], generatedShaderParms[2]);
        }
        shaderParms = generatedShaderParms;
    }

    const float* regs = material->ConstantRegisters();
    float dynamicRegs[MAX_EXPRESSION_REGISTERS];
    if (!regs)
    {
        material->EvaluateRegisters(
            dynamicRegs,
            shaderParms,
            globalParms,
            floatTime,
            renderEntity.referenceSound);
        regs = dynamicRegs;
    }

    return SmokeMaterialRegistersHaveActiveEmissiveStageWithClassifier(material, regs, classifier);
}

static bool SmokeDynamicEvalStageIsEmissiveLike(const shaderStage_t* stage)
{
    if (!stage)
    {
        return false;
    }

    const uint64 srcBlend = stage->drawStateBits & GLS_SRCBLEND_BITS;
    const uint64 dstBlend = stage->drawStateBits & GLS_DSTBLEND_BITS;
    return (srcBlend == GLS_SRCBLEND_ONE || srcBlend == GLS_SRCBLEND_SRC_ALPHA) &&
        dstBlend == GLS_DSTBLEND_ONE;
}

static float SmokeDynamicEvalColorLuminance(const float color[4])
{
    return Max(0.0f, color[0]) * 0.2126f +
        Max(0.0f, color[1]) * 0.7152f +
        Max(0.0f, color[2]) * 0.0722f;
}

static bool SmokeDynamicEvalSampleShouldReplace(const RtSmokeDynamicMaterialEvalSample& current, const RtSmokeDynamicMaterialEvalSample& candidate)
{
    if (!current.valid)
    {
        return true;
    }
    if (candidate.stagePriority != current.stagePriority)
    {
        return candidate.stagePriority > current.stagePriority;
    }
    return SmokeDynamicEvalColorLuminance(candidate.color) > SmokeDynamicEvalColorLuminance(current.color);
}

static void SmokeDynamicEvalCopySelectedStage(RtSmokeDynamicMaterialEvalSample& dst, const RtSmokeDynamicMaterialEvalSample& src)
{
    dst.valid = src.valid;
    dst.stageIndex = src.stageIndex;
    dst.stagePriority = src.stagePriority;
    dst.selectedStageEmissive = src.selectedStageEmissive;
    dst.condition = src.condition;
    dst.alphaTest = src.alphaTest;
    dst.image = src.image;
    for (int component = 0; component < 4; ++component)
    {
        dst.color[component] = src.color[component];
    }
    for (int row = 0; row < 2; ++row)
    {
        for (int column = 0; column < 3; ++column)
        {
            dst.texMatrix[row][column] = src.texMatrix[row][column];
        }
    }
}

static void SmokeDynamicEvalAccumulateSample(RtSmokeDynamicMaterialEvalSample& sample, const RtSmokeDynamicMaterialEvalSample& surfaceSample, int indexes)
{
    ++sample.surfaces;
    sample.triangles += indexes / 3;
    sample.enabledStages += surfaceSample.enabledStages;
    sample.disabledStages += surfaceSample.disabledStages;
    sample.colorStages += surfaceSample.colorStages;
    sample.alphaStages += surfaceSample.alphaStages;
    sample.alphaTestStages += surfaceSample.alphaTestStages;
    sample.texMatrixStages += surfaceSample.texMatrixStages;
    sample.dynamicImageStages += surfaceSample.dynamicImageStages;
    sample.cinematicStages += surfaceSample.cinematicStages;
    sample.guiRenderTargetStages += surfaceSample.guiRenderTargetStages;
    sample.programStages += surfaceSample.programStages;
    if (!sample.hasDiffuseStageColor && surfaceSample.hasDiffuseStageColor)
    {
        sample.hasDiffuseStageColor = true;
        sample.diffuseStageCondition = surfaceSample.diffuseStageCondition;
        for (int component = 0; component < 4; ++component)
        {
            sample.diffuseStageColor[component] = surfaceSample.diffuseStageColor[component];
        }
    }
    if (!sample.hasSurfaceOrigin && surfaceSample.hasSurfaceOrigin)
    {
        sample.hasSurfaceOrigin = true;
        sample.surfaceOrigin = surfaceSample.surfaceOrigin;
    }
    if (SmokeDynamicEvalSampleShouldReplace(sample, surfaceSample))
    {
        SmokeDynamicEvalCopySelectedStage(sample, surfaceSample);
    }
}

static void SmokeDynamicEvalAddMaterialSample(RtSmokeMaterialStats& stats, const RtSmokeDynamicMaterialEvalSample& surfaceSample, int indexes)
{
    for (RtSmokeDynamicMaterialEvalSample& sample : stats.dynamicEvalMaterialSamples)
    {
        if (sample.id == surfaceSample.id)
        {
            SmokeDynamicEvalAccumulateSample(sample, surfaceSample, indexes);
            return;
        }
    }

    RtSmokeDynamicMaterialEvalSample sample = surfaceSample;
    sample.surfaces = 1;
    sample.triangles = indexes / 3;
    stats.dynamicEvalMaterialSamples.push_back(sample);
}

// DECAL-02 (docs/decal_cards/04): lift detail-decal cards along their outward face
// normal so coplanar any-hit accumulation is deterministic and stacked decals
// separate. Baked into captured vertex positions; view-independent. Persistent
// cards use their static-surface key, while switchable per-frame cards use a
// stable entity/material key so both routes keep the same lift across frames.
void ApplySmokeDetailDecalNormalOffset(
    const idMaterial* material,
    uint64 surfaceOffsetKey,
    bool liquidRouteEligible,
    std::vector<PathTraceSmokeVertex>& vertices,
    const std::vector<uint32_t>& indexes,
    size_t vertexStart,
    size_t indexStart)
{
    const bool genericOffsetEnabled = r_pathTracingDecalComposite.GetInteger() > 0;
    const bool liquidOffsetRequested = liquidRouteEligible && r_pathTracingLiquidPoolMode.GetInteger() != 0;
    if (!genericOffsetEnabled && !liquidOffsetRequested)
    {
        return;
    }
    if (!material)
    {
        return;
    }
    // Cheap cost gate before the stage-scanning classifier build: this also runs
    // per-frame in the dynamic capture pass (DECAL-DYN-1).
    const float sort = material->GetSort();
    if (!(sort >= SS_DECAL && sort < SS_FAR) && !material->TestMaterialFlag(MF_POLYGONOFFSET))
    {
        return;
    }
    const RtSmokeTranslucentClassifierInfo classifier = BuildSmokeTranslucentClassifierInfo(material);
    if (!IsSmokeDetailDecalCardMaterial(material, classifier))
    {
        return;
    }
    if (!genericOffsetEnabled)
    {
        const uint32_t materialId = SmokeMaterialId(material);
        const RtSmokeMaterialTextureInfo* info = FindSmokeMaterialTextureInfoReadOnly(materialId);
        if (!info || !SmokeMaterialTextureInfoHasMaterialMetadata(*info))
        {
            RegisterSmokeMaterialTextureInfo(material);
            info = FindSmokeMaterialTextureInfoReadOnly(materialId);
        }
        if (!info || !SmokeMaterialTextureInfoHasMaterialMetadata(*info) || !info->liquidFilmCandidate)
        {
            return;
        }
    }

    const float step = Max(0.0f, r_pathTracingDecalOffsetStep.GetFloat());
    const int maxOffsetIndex = Max(1, r_pathTracingDecalMaxOffsetIndex.GetInteger());
    uint64 hash = surfaceOffsetKey;
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdull;
    hash ^= hash >> 33;
    const int offsetIndex = 1 + static_cast<int>(hash % static_cast<uint64>(maxOffsetIndex));
    const float lift = step * static_cast<float>(offsetIndex);
    if (lift <= 0.0f)
    {
        return;
    }

    std::vector<bool> vertexLifted(vertices.size() - vertexStart, false);
    for (size_t indexCursor = indexStart; indexCursor + 2 < indexes.size(); indexCursor += 3)
    {
        const uint32_t i0 = indexes[indexCursor + 0];
        const uint32_t i1 = indexes[indexCursor + 1];
        const uint32_t i2 = indexes[indexCursor + 2];
        if (i0 < vertexStart || i1 < vertexStart || i2 < vertexStart ||
            i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
        {
            continue;
        }

        const idVec3 p0 = SmokeVertexPosition(vertices[i0]);
        const idVec3 p1 = SmokeVertexPosition(vertices[i1]);
        const idVec3 p2 = SmokeVertexPosition(vertices[i2]);
        idVec3 faceNormal = (p1 - p0).Cross(p2 - p0);
        if (faceNormal.Normalize() == 0.0f)
        {
            continue;
        }
        // Use the FACE normal (planar cards stay planar) but orient it outward by
        // the authored vertex normal so the lift never pushes into the receiver.
        const idVec3 vertexNormal = SmokeVertexNormal(vertices[i0]);
        if (faceNormal * vertexNormal < 0.0f)
        {
            faceNormal = -faceNormal;
        }

        const uint32_t triangleVerts[3] = { i0, i1, i2 };
        for (int corner = 0; corner < 3; ++corner)
        {
            const size_t localVertex = triangleVerts[corner] - vertexStart;
            if (vertexLifted[localVertex])
            {
                continue;
            }
            vertexLifted[localVertex] = true;
            PathTraceSmokeVertex& vertex = vertices[triangleVerts[corner]];
            vertex.position[0] += faceNormal.x * lift;
            vertex.position[1] += faceNormal.y * lift;
            vertex.position[2] += faceNormal.z * lift;
        }
    }
}

int AppendSmokeSurfaceGeometry(
    const drawSurf_t* drawSurf,
    const srfTriangles_t* tri,
    uint32_t surfaceClassId,
    uint32_t materialId,
    int classCount,
    uint32_t triangleClassMask,
    uint32_t particleAlphaClassId,
    uint32_t forceGeometricNormalFlag,
    std::vector<PathTraceSmokeVertex>& vertices,
    std::vector<uint32_t>& indexes,
    std::vector<uint32_t>& triangleClasses,
    std::vector<uint32_t>& triangleMaterials,
    RtSmokeSurfaceSkipStats& skipStats,
    RtSmokeAttributeStats& attributeStats)
{
    OPTICK_EVENT("PT Merged Dynamic Copy Geometry");
    // Kept in the shared capture ABI for its existing callers; surface class
    // no longer overrides valid authored normals.
    (void)particleAlphaClassId;
    const size_t vertexStart = vertices.size();
    const size_t indexStart = indexes.size();
    const size_t classStart = triangleClasses.size();
    const size_t materialStart = triangleMaterials.size();
    const uint32_t indexBase = static_cast<uint32_t>(vertices.size());
    bool verticesFromFrameCache = false;
    const idDrawVert* sourceVertices = SmokeDrawSurfaceVertices(drawSurf, tri, verticesFromFrameCache);
    const idJointMat* rtCpuSkinningJoints = SmokeDrawSurfaceCpuSkinningJoints(drawSurf, tri, verticesFromFrameCache);
    const int classIndex = idMath::ClampInt(0, classCount - 1, static_cast<int>(surfaceClassId & triangleClassMask));
    const bool activeEmissiveStage = SmokeDrawSurfaceHasActiveEmissiveStage(drawSurf);
    const uint32_t perSurfaceTriangleFlags = activeEmissiveStage ? 0u : RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF;
    const triIndex_t* sourceIndexes = SmokeDrawSurfaceIndexes(drawSurf, tri);
    if (!sourceVertices || !sourceIndexes)
    {
        ++skipStats.missingGeometry;
        return 0;
    }

    {
        OPTICK_EVENT_DYNAMIC(rtCpuSkinningJoints
            ? "PT Merged Dynamic CPU Skin + Vertex Copy"
            : "PT Merged Dynamic Vertex Copy");
        for (int vertexIndex = 0; vertexIndex < tri->numVerts; ++vertexIndex)
        {
            PathTraceSmokeVertex vertex = BuildSmokeSurfaceVertexFromSource(drawSurf, sourceVertices, vertexIndex, rtCpuSkinningJoints);
            const idVec3 normal = SmokeVertexNormal(vertex);
            const idVec2 texCoord = SmokeVertexTexCoord(vertex);
            if (!SmokeNormalIsUsable(normal))
            {
                ++attributeStats.classes[classIndex].invalidNormalVerts;
                vertex.normal[0] = 0.0f;
                vertex.normal[1] = 0.0f;
                vertex.normal[2] = 0.0f;
            }
            if (!SmokeTexCoordIsUsable(texCoord))
            {
                ++attributeStats.classes[classIndex].invalidUvVerts;
                vertex.texCoord[0] = 0.0f;
                vertex.texCoord[1] = 0.0f;
            }
            vertices.push_back(vertex);
        }
    }

    {
        OPTICK_EVENT("PT Merged Dynamic Index Copy + Validate");
        for (int sourceIndex = 0; sourceIndex + 2 < drawSurf->numIndexes; sourceIndex += 3)
        {
            triIndex_t sourceTriangle[3];
            if (!SmokeTryCopyMemory(sourceTriangle, sourceIndexes + sourceIndex, sizeof(sourceTriangle)))
            {
                ++skipStats.missingGeometry;
                vertices.resize(vertexStart);
                indexes.resize(indexStart);
                triangleClasses.resize(classStart);
                triangleMaterials.resize(materialStart);
                return 0;
            }
            const int i0 = sourceTriangle[0];
            const int i1 = sourceTriangle[1];
            const int i2 = sourceTriangle[2];
            if (i0 < 0 || i1 < 0 || i2 < 0 || i0 >= tri->numVerts || i1 >= tri->numVerts || i2 >= tri->numVerts)
            {
                ++skipStats.invalidIndexCount;
                continue;
            }

        PathTraceSmokeVertex v0;
        PathTraceSmokeVertex v1;
        PathTraceSmokeVertex v2;
        const PathTraceSmokeVertex* vertexData = vertices.data();
        if (!SmokeTryCopyMemory(&v0, vertexData + indexBase + static_cast<uint32_t>(i0), sizeof(v0)) ||
            !SmokeTryCopyMemory(&v1, vertexData + indexBase + static_cast<uint32_t>(i1), sizeof(v1)) ||
            !SmokeTryCopyMemory(&v2, vertexData + indexBase + static_cast<uint32_t>(i2), sizeof(v2)))
        {
            ++skipStats.missingGeometry;
            vertices.resize(vertexStart);
            indexes.resize(indexStart);
            triangleClasses.resize(classStart);
            triangleMaterials.resize(materialStart);
            return 0;
        }
        const idVec3 p0 = SmokeVertexPosition(v0);
        const idVec3 p1 = SmokeVertexPosition(v1);
        const idVec3 p2 = SmokeVertexPosition(v2);
        if (IsZeroAreaSmokeTriangle(p0, p1, p2))
        {
            continue;
        }

        indexes.push_back(indexBase + static_cast<uint32_t>(i0));
        indexes.push_back(indexBase + static_cast<uint32_t>(i1));
        indexes.push_back(indexBase + static_cast<uint32_t>(i2));
        const bool invalidNormalTriangle =
            !SmokeNormalIsUsable(SmokeVertexNormal(v0)) ||
            !SmokeNormalIsUsable(SmokeVertexNormal(v1)) ||
            !SmokeNormalIsUsable(SmokeVertexNormal(v2));
        const bool invalidUvTriangle =
            !SmokeTexCoordIsUsable(SmokeVertexTexCoord(v0)) ||
            !SmokeTexCoordIsUsable(SmokeVertexTexCoord(v1)) ||
            !SmokeTexCoordIsUsable(SmokeVertexTexCoord(v2));
        // Doom's vertex normals already encode the model's authored smoothing
        // groups.  Do not replace them merely because a surface was routed as
        // ParticleAlpha: that route also contains alpha-tested grates, cables,
        // foliage, and other ordinary meshes whose shading must remain smooth.
        // A face-normal fallback is valid only when the captured vertex-normal
        // contract is actually unusable.
        const bool preferGeometricNormal = invalidNormalTriangle;

        if (invalidNormalTriangle)
        {
            ++attributeStats.classes[classIndex].invalidNormalTriangles;
        }
        if (invalidUvTriangle)
        {
            ++attributeStats.classes[classIndex].invalidUvTriangles;
        }
        if (preferGeometricNormal)
        {
            ++attributeStats.classes[classIndex].forcedGeometricNormalTriangles;
        }

        triangleClasses.push_back(surfaceClassId | perSurfaceTriangleFlags | (preferGeometricNormal ? forceGeometricNormalFlag : 0u));
            triangleMaterials.push_back(materialId);
        }
    }

    const int emittedIndexes = static_cast<int>(indexes.size() - indexStart);
    if (emittedIndexes <= 0 || triangleClasses.size() == classStart || triangleMaterials.size() == materialStart)
    {
        vertices.resize(vertexStart);
        indexes.resize(indexStart);
        triangleClasses.resize(classStart);
        triangleMaterials.resize(materialStart);
        ++skipStats.zeroAreaOnly;
        return 0;
    }

    return emittedIndexes;
}

void AddSmokeSkinnedSurfaceRecord(
    std::vector<RtSmokeSkinnedSurfaceRecord>* records,
    const drawSurf_t* drawSurf,
    const srfTriangles_t* tri,
    uint32_t surfaceClassId,
    uint32_t materialId,
    int drawSurfIndex,
    int bucketIndex,
    int currentVertexOffset,
    int currentIndexOffset,
    int currentTriangleOffset,
    int vertexCount,
    int indexCount,
    int triangleCount)
{
    if (!records || !drawSurf || !tri)
    {
        return;
    }

    const viewEntity_t* space = drawSurf->space;
    const idRenderEntityLocal* entityDef = space ? space->entityDef : nullptr;
    const renderEntity_t* renderEntity = entityDef ? &entityDef->parms : nullptr;

    RtSmokeSkinnedSurfaceRecord record;
    record.key.entityIndex = entityDef ? entityDef->index : -1;
    record.key.entityDef = reinterpret_cast<uintptr_t>(entityDef);
    record.key.model = reinterpret_cast<uintptr_t>(renderEntity ? renderEntity->hModel : nullptr);
    record.key.tri = reinterpret_cast<uintptr_t>(tri);
    record.key.materialId = materialId;
    record.key.surfaceClassId = surfaceClassId;
    record.historyOwner = PtGeometryLifecycle::PrimaryHistoryOwnerKey(
        entityDef ? entityDef->world : nullptr);
    const PtRenderDefKey renderDefKey =
        PtGeometryLifecycle::MakeEntityKey(entityDef);
    record.canonicalInstance.worldGeneration =
        renderDefKey.worldGeneration;
    record.canonicalInstance.renderDefIndex =
        renderDefKey.index >= 0
            ? static_cast<uint32_t>(renderDefKey.index)
            : UINT32_MAX;
    record.canonicalInstance.renderDefGeneration =
        renderDefKey.generation;
    record.canonicalInstance.subInstanceKind =
        PtCanonicalSubInstanceKind::SkinnedSurface;
    record.canonicalInstance.modelSurfaceIndex =
        drawSurf->modelSurfaceIndex >= 0
            ? static_cast<uint32_t>(drawSurf->modelSurfaceIndex)
            : UINT32_MAX;
    record.canonicalInstance.jointSubmeshIndex = -1;
    record.jointCacheHandle =
        static_cast<uint64_t>(drawSurf->jointCache);
    record.jointCacheCpuSnapshot =
        reinterpret_cast<uintptr_t>(
            drawSurf->jointCacheCpuSnapshot);
    record.jointCacheCpuSnapshotCount =
        drawSurf->jointCacheCpuSnapshotCount;
    record.currentVertexOffset = currentVertexOffset;
    record.currentIndexOffset = currentIndexOffset;
    record.currentTriangleOffset = currentTriangleOffset;
    record.vertexCount = vertexCount;
    record.indexCount = indexCount;
    record.triangleCount = triangleCount;
    bool verticesFromFrameCache = false;
    SmokeDrawSurfaceVertices(drawSurf, tri, verticesFromFrameCache);
    const idJointMat* rtCpuSkinningJoints = SmokeDrawSurfaceCpuSkinningJoints(drawSurf, tri, verticesFromFrameCache);
    record.rtCpuSkinned = rtCpuSkinningJoints != nullptr;
    record.basePoseLikely = SmokeSkinnedSurfaceLikelyBasePose(drawSurf, tri);
    record.entityIndex = record.key.entityIndex;
    record.drawSurfIndex = drawSurfIndex;
    record.modelSurfaceIndex = drawSurf->modelSurfaceIndex;
    record.modelName = renderEntity && renderEntity->hModel ? renderEntity->hModel->Name() : "<none>";
    record.materialId = materialId;
    record.triangleClassAndFlags =
        surfaceClassId |
        (SmokeDrawSurfaceHasActiveEmissiveStage(drawSurf)
            ? 0u
            : RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF);
    if (verticesFromFrameCache)
    {
        record.jointCount = renderEntity ? renderEntity->numJoints : 0;
        record.jointSource = reinterpret_cast<uintptr_t>(renderEntity ? static_cast<const void*>(renderEntity->joints) : nullptr);
    }
    else
    {
        record.jointCount =
            record.jointCacheCpuSnapshotCount > 0
                ? record.jointCacheCpuSnapshotCount
                : (tri->staticModelWithJoints ? tri->staticModelWithJoints->numInvertedJoints : (renderEntity ? renderEntity->numJoints : 0));
        record.jointSource = reinterpret_cast<uintptr_t>(tri->staticModelWithJoints ? static_cast<const void*>(tri->staticModelWithJoints) : static_cast<const void*>(renderEntity ? renderEntity->joints : nullptr));
    }
    if (record.jointCacheCpuSnapshot != 0 &&
        record.jointCacheCpuSnapshotCount == record.jointCount &&
        record.jointCount > 0)
    {
        const idJointMat* snapshot =
            reinterpret_cast<const idJointMat*>(
                record.jointCacheCpuSnapshot);
        const idJointMat* liveJoints =
            GetSmokeRtCpuSkinningJoints(tri);
        record.jointCacheCpuSourceComparable =
            liveJoints != nullptr;
        for (int jointIndex = 0;
            record.jointCacheCpuSourceComparable &&
            jointIndex < record.jointCount;
            ++jointIndex)
        {
            idJointMat liveJoint;
            if (!SmokeTryCopyMemory(
                    &liveJoint,
                    liveJoints + jointIndex,
                    sizeof(liveJoint)))
            {
                record.jointCacheCpuSourceComparable = false;
                break;
            }
            if (memcmp(
                    &liveJoint,
                    snapshot + jointIndex,
                    sizeof(liveJoint)) != 0)
            {
                record.jointCacheCpuSourceChanged = true;
                break;
            }
        }
    }
    record.bucketIndex = bucketIndex;
    if (renderEntity)
    {
        record.hasEntityOrigin = true;
        record.entityOrigin = renderEntity->origin;
    }
    if (space)
    {
        record.objectToWorld[0] = space->modelMatrix[0];
        record.objectToWorld[1] = space->modelMatrix[4];
        record.objectToWorld[2] = space->modelMatrix[8];
        record.objectToWorld[3] = space->modelMatrix[12];
        record.objectToWorld[4] = space->modelMatrix[1];
        record.objectToWorld[5] = space->modelMatrix[5];
        record.objectToWorld[6] = space->modelMatrix[9];
        record.objectToWorld[7] = space->modelMatrix[13];
        record.objectToWorld[8] = space->modelMatrix[2];
        record.objectToWorld[9] = space->modelMatrix[6];
        record.objectToWorld[10] = space->modelMatrix[10];
        record.objectToWorld[11] = space->modelMatrix[14];
    }
    else
    {
        record.objectToWorld[0] = 1.0f;
        record.objectToWorld[5] = 1.0f;
        record.objectToWorld[10] = 1.0f;
    }

    records->push_back(record);
}

void FinalizeSmokeSkinnedSurfaceRecordOffsets(
    std::vector<RtSmokeSkinnedSurfaceRecord>* records,
    int bucketIndex,
    const RtSmokeBucketRange& range)
{
    if (!records)
    {
        return;
    }

    for (RtSmokeSkinnedSurfaceRecord& record : *records)
    {
        if (record.bucketIndex != bucketIndex)
        {
            continue;
        }
        if (record.cpuCaptureOmitted)
        {
            continue;
        }
        record.currentVertexOffset += range.vertexOffset;
        record.currentIndexOffset += range.indexOffset;
        record.currentTriangleOffset += range.triangleOffset;
    }
}

void AddSmokeCapturedSurfaceRecord(
    std::vector<RtSmokeCapturedSurfaceRecord>* records,
    const drawSurf_t* drawSurf,
    uint32_t surfaceClassId,
    uint32_t materialId,
    int drawSurfIndex,
    int bucketIndex,
    int currentVertexOffset,
    int currentIndexOffset,
    int currentTriangleOffset,
    int vertexCount,
    int indexCount,
    int triangleCount)
{
    if (!records || !drawSurf)
    {
        return;
    }

    const viewEntity_t* space = drawSurf->space;
    const idRenderEntityLocal* entityDef =
        space ? space->entityDef : nullptr;
    const renderEntity_t* renderEntity =
        entityDef ? &entityDef->parms : nullptr;

    RtSmokeCapturedSurfaceRecord record;
    record.currentVertexOffset = currentVertexOffset;
    record.currentIndexOffset = currentIndexOffset;
    record.currentTriangleOffset = currentTriangleOffset;
    record.vertexCount = vertexCount;
    record.indexCount = indexCount;
    record.triangleCount = triangleCount;
    record.bucketIndex = bucketIndex;
    record.entityIndex = entityDef ? entityDef->index : -1;
    record.drawSurfIndex = drawSurfIndex;
    record.modelSurfaceIndex = drawSurf->modelSurfaceIndex;
    record.materialId = materialId;
    record.triangleClassAndFlags =
        surfaceClassId |
        (SmokeDrawSurfaceHasActiveEmissiveStage(drawSurf)
            ? 0u
            : RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF);
    record.modelName =
        renderEntity && renderEntity->hModel
            ? renderEntity->hModel->Name()
            : "<none>";
    record.materialName =
        drawSurf->material
            ? drawSurf->material->GetName()
            : "<none>";
    records->push_back(record);
}

void FinalizeSmokeCapturedSurfaceRecordOffsets(
    std::vector<RtSmokeCapturedSurfaceRecord>* records,
    int bucketIndex,
    const RtSmokeBucketRange& range)
{
    if (!records)
    {
        return;
    }
    for (RtSmokeCapturedSurfaceRecord& record : *records)
    {
        if (record.bucketIndex != bucketIndex)
        {
            continue;
        }
        record.currentVertexOffset += range.vertexOffset;
        record.currentIndexOffset += range.indexOffset;
        record.currentTriangleOffset += range.triangleOffset;
    }
}

namespace {

void AddSmokeMaterialStats(RtSmokeMaterialStats& stats, const idMaterial* material, int indexes, RtSmokeSurfaceClass surfaceClass, RtSmokeTranslucentSubtype translucentSubtype)
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

    for (int sampleIndex = 0; sampleIndex < stats.sampleCount; ++sampleIndex)
    {
        RtSmokeMaterialSample& sample = stats.samples[sampleIndex];
        if (sample.id == materialId)
        {
            ++sample.surfaces;
            sample.triangles += indexes / 3;
            return;
        }
    }

    if (stats.sampleCount < RT_SMOKE_MATERIAL_REASON_SAMPLES)
    {
        RtSmokeMaterialSample& sample = stats.samples[stats.sampleCount];
        sample.id = materialId;
        sample.surfaces = 1;
        sample.triangles = indexes / 3;
        sample.name = materialName;
        ++stats.sampleCount;
    }

    if (surfaceClass != RtSmokeSurfaceClass::ParticleAlpha)
    {
        return;
    }

    const int subtypeIndex = idMath::ClampInt(0, RT_SMOKE_TRANSLUCENT_SUBTYPE_COUNT - 1, static_cast<int>(SmokeTranslucentSubtypeId(translucentSubtype)));
    ++stats.translucentSubtypeSurfaces[subtypeIndex];
    stats.translucentSubtypeTriangles[subtypeIndex] += indexes / 3;

    bool subtypeSampleFound = false;
    for (int sampleIndex = 0; sampleIndex < stats.translucentSubtypeSampleCounts[subtypeIndex]; ++sampleIndex)
    {
        RtSmokeMaterialSample& sample = stats.translucentSubtypeSamples[subtypeIndex][sampleIndex];
        if (sample.id == materialId)
        {
            ++sample.surfaces;
            sample.triangles += indexes / 3;
            subtypeSampleFound = true;
            break;
        }
    }

    if (!subtypeSampleFound && stats.translucentSubtypeSampleCounts[subtypeIndex] < RT_SMOKE_MATERIAL_REASON_SAMPLES)
    {
        RtSmokeMaterialSample& sample = stats.translucentSubtypeSamples[subtypeIndex][stats.translucentSubtypeSampleCounts[subtypeIndex]];
        sample.id = materialId;
        sample.surfaces = 1;
        sample.triangles = indexes / 3;
        sample.name = materialName;
        ++stats.translucentSubtypeSampleCounts[subtypeIndex];
    }

    ++stats.translucentSurfaces;
    stats.translucentTriangles += indexes / 3;
    const bool firstTranslucentMaterial = std::find(stats.translucentMaterialIds.begin(), stats.translucentMaterialIds.end(), materialId) == stats.translucentMaterialIds.end();
    if (firstTranslucentMaterial)
    {
        stats.translucentMaterialIds.push_back(materialId);
        ++stats.translucentUniqueMaterials;
    }

    for (int sampleIndex = 0; sampleIndex < stats.translucentSampleCount; ++sampleIndex)
    {
        RtSmokeMaterialSample& sample = stats.translucentSamples[sampleIndex];
        if (sample.id == materialId)
        {
            ++sample.surfaces;
            sample.triangles += indexes / 3;
            return;
        }
    }

    if (stats.translucentSampleCount < RT_SMOKE_MATERIAL_REASON_SAMPLES)
    {
        RtSmokeMaterialSample& sample = stats.translucentSamples[stats.translucentSampleCount];
        sample.id = materialId;
        sample.surfaces = 1;
        sample.triangles = indexes / 3;
        sample.name = materialName;
        ++stats.translucentSampleCount;
    }
}

bool SmokeEvalRegister(const float* regs, int registerCount, int registerIndex, float fallback, float& value)
{
    if (regs && registerIndex >= 0 && registerIndex < registerCount)
    {
        value = regs[registerIndex];
        return true;
    }
    value = fallback;
    return false;
}

bool SmokeRegisterDependsOnRuntime(const idMaterial* material, int registerIndex)
{
    if (registerIndex < 0)
    {
        return false;
    }
    if (registerIndex < EXP_REG_NUM_PREDEFINED)
    {
        return true;
    }
    return material && material->ConstantRegisters() == nullptr;
}

bool SmokeStageUsesPerSurfaceMaterialState(const idMaterial* material, const shaderStage_t* stage)
{
    if (!material || !stage)
    {
        return false;
    }

    if (SmokeRegisterDependsOnRuntime(material, stage->conditionRegister))
    {
        return true;
    }
    if (stage->hasAlphaTest && SmokeRegisterDependsOnRuntime(material, stage->alphaTestRegister))
    {
        return true;
    }
    for (int component = 0; component < 4; ++component)
    {
        if (SmokeRegisterDependsOnRuntime(material, stage->color.registers[component]))
        {
            return true;
        }
    }
    if (stage->texture.hasMatrix)
    {
        for (int row = 0; row < 2; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                if (SmokeRegisterDependsOnRuntime(material, stage->texture.matrix[row][column]))
                {
                    return true;
                }
            }
        }
    }
    return stage->texture.dynamic != DI_STATIC ||
        stage->texture.dynamicFrameCount > 0 ||
        stage->texture.cinematic != nullptr ||
        stage->texture.texgen == TG_SCREEN ||
        stage->texture.texgen == TG_SCREEN2 ||
        stage->newStage != nullptr;
}

uint32_t SmokeRuntimeMaterialVariantHashValue(uint32_t hash, uint32_t value)
{
    hash ^= value & 0xffu;
    hash *= 16777619u;
    hash ^= (value >> 8) & 0xffu;
    hash *= 16777619u;
    hash ^= (value >> 16) & 0xffu;
    hash *= 16777619u;
    hash ^= (value >> 24) & 0xffu;
    hash *= 16777619u;
    return hash;
}

uint32_t SmokeDynamicTriangleIdentitySeed(const drawSurf_t* drawSurf, const srfTriangles_t* tri, uint32_t materialId, uint32_t localTriangleIndex)
{
    const idRenderEntityLocal* entity = (drawSurf && drawSurf->space) ? drawSurf->space->entityDef : nullptr;
    const renderEntity_t* renderEntity = entity ? &entity->parms : nullptr;
    uint32_t hash = 2166136261u;
    hash = SmokeRuntimeMaterialVariantHashValue(hash, static_cast<uint32_t>(entity ? entity->index : -1));
    hash = SmokeRuntimeMaterialVariantHashValue(hash, static_cast<uint32_t>(renderEntity ? renderEntity->entityNum : -1));
    hash = SmokeRuntimeMaterialVariantHashValue(hash, materialId);
    const uintptr_t triPtr = reinterpret_cast<uintptr_t>(tri);
    hash = SmokeRuntimeMaterialVariantHashValue(hash, static_cast<uint32_t>(triPtr));
    hash = SmokeRuntimeMaterialVariantHashValue(hash, static_cast<uint32_t>(triPtr >> 32));
    hash = SmokeRuntimeMaterialVariantHashValue(hash, localTriangleIndex);
    return hash != 0u ? hash : 1u;
}

static uint32_t SmokeRuntimeMaterialVariantIdForEntitySurfaceKey(const idRenderEntityLocal* entity, int modelSurfaceIndex, uint32_t baseMaterialId)
{
    if (!entity || baseMaterialId == 0u)
    {
        return baseMaterialId;
    }

    const renderEntity_t& renderEntity = entity->parms;
    uint32_t hash = 2166136261u;
    hash = SmokeRuntimeMaterialVariantHashValue(hash, 0x72746573u);
    hash = SmokeRuntimeMaterialVariantHashValue(hash, baseMaterialId);
    hash = SmokeRuntimeMaterialVariantHashValue(hash, static_cast<uint32_t>(entity->index));
    hash = SmokeRuntimeMaterialVariantHashValue(hash, static_cast<uint32_t>(renderEntity.entityNum));
    hash = SmokeRuntimeMaterialVariantHashValue(hash, static_cast<uint32_t>(modelSurfaceIndex));

    uint32_t variantMaterialId = hash | 0x80000000u;
    if (variantMaterialId == 0u || variantMaterialId == baseMaterialId)
    {
        variantMaterialId = (hash ^ 0x5bd1e995u) | 0x80000000u;
    }
    return variantMaterialId != 0u ? variantMaterialId : baseMaterialId;
}

enum class RtSmokeDynamicEvalBuildResult
{
    NoMaterial,
    NoRegisters,
    NoSelectedStage,
    Built
};

static RtSmokeDynamicEvalBuildResult SmokeMaterialSampleFromEvaluation(
    const idMaterial* material, uint32_t materialId,
    const RtPathTraceRuntimeMaterialEvalPod& evaluated,
    RtSmokeDynamicMaterialEvalSample& surfaceSample)
{
    surfaceSample.valid =
        evaluated.result == RtPathTraceRuntimeEvalBuildResult::Built;
    surfaceSample.id = materialId;
    surfaceSample.name = material->GetName();
    surfaceSample.stageIndex = evaluated.selectedStageIndex;
    surfaceSample.stagePriority = evaluated.selectedStagePriority;
    surfaceSample.enabledStages = evaluated.enabledStages;
    surfaceSample.disabledStages = evaluated.disabledStages;
    surfaceSample.colorStages = evaluated.colorStages;
    surfaceSample.alphaStages = evaluated.alphaStages;
    surfaceSample.alphaTestStages = evaluated.alphaTestStages;
    surfaceSample.texMatrixStages = evaluated.texMatrixStages;
    surfaceSample.dynamicImageStages = evaluated.dynamicImageStages;
    surfaceSample.cinematicStages = evaluated.cinematicStages;
    surfaceSample.guiRenderTargetStages = evaluated.guiRenderTargetStages;
    surfaceSample.programStages = evaluated.programStages;
    surfaceSample.selectedStageEmissive = evaluated.selectedStageEmissive;
    surfaceSample.condition = evaluated.condition;
    surfaceSample.alphaTest = evaluated.alphaTest;
    std::memcpy(surfaceSample.color, evaluated.color, sizeof(surfaceSample.color));
    std::memcpy(surfaceSample.texMatrix, evaluated.texMatrix,
        sizeof(surfaceSample.texMatrix));
    surfaceSample.hasDiffuseStageColor = evaluated.hasDiffuseStageColor;
    std::memcpy(surfaceSample.diffuseStageColor,
        evaluated.diffuseStageColor, sizeof(surfaceSample.diffuseStageColor));
    surfaceSample.diffuseStageCondition = evaluated.diffuseStageCondition;
    surfaceSample.orderedStageCount = static_cast<int>(evaluated.orderedStageCount);
    surfaceSample.orderedStageOverflow = evaluated.orderedStageOverflow;
    for (std::uint32_t index = 0; index < evaluated.orderedStageCount; ++index)
    {
        const RtPathTraceRuntimeStageEvalPod& source =
            evaluated.orderedStages[index];
        RtSmokeDynamicStageEval& destination = surfaceSample.orderedStages[index];
        destination.stageIndex = source.stageIndex;
        destination.enabled = source.enabled;
        destination.emissive = source.emissive;
        destination.hasAlphaTest = source.hasAlphaTest;
        destination.hasTexMatrix = source.hasTexMatrix;
        destination.condition = source.condition;
        destination.alphaTest = source.alphaTest;
        std::memcpy(destination.color, source.color, sizeof(destination.color));
        std::memcpy(destination.texMatrix, source.texMatrix,
            sizeof(destination.texMatrix));
    }
    surfaceSample.hasSurfaceOrigin = evaluated.hasSurfaceOrigin;
    surfaceSample.surfaceOrigin.Set(evaluated.surfaceOrigin[0],
        evaluated.surfaceOrigin[1], evaluated.surfaceOrigin[2]);
    if (evaluated.selectedStageIndex >= 0 &&
        evaluated.selectedStageIndex < material->GetNumStages())
    {
        const shaderStage_t* selected =
            material->GetStage(evaluated.selectedStageIndex);
        surfaceSample.image = selected ? selected->texture.image : nullptr;
    }
    return evaluated.result == RtPathTraceRuntimeEvalBuildResult::Built
        ? RtSmokeDynamicEvalBuildResult::Built
        : RtSmokeDynamicEvalBuildResult::NoSelectedStage;
}

RtSmokeDynamicEvalBuildResult BuildSmokeDynamicMaterialEvalSampleForId(
    const drawSurf_t* drawSurf, uint32_t materialId,
    RtSmokeDynamicMaterialEvalSample& surfaceSample,
    RtPathTraceRuntimeMaterialEvalPod* podResult = nullptr)
{
    surfaceSample = RtSmokeDynamicMaterialEvalSample();
    const idMaterial* material = drawSurf ? drawSurf->material : nullptr;
    if (!material)
    {
        return RtSmokeDynamicEvalBuildResult::NoMaterial;
    }

    const float* regs = drawSurf->shaderRegisters ? drawSurf->shaderRegisters : material->ConstantRegisters();
    if (!regs)
    {
        return RtSmokeDynamicEvalBuildResult::NoRegisters;
    }

    std::vector<RtPathTraceRuntimeMaterialStagePod> stages(
        static_cast<std::size_t>(Max(0, material->GetNumStages())));
    if (!CapturePathTraceRuntimeMaterialStages(material,
            stages.empty() ? nullptr : stages.data(),
            static_cast<int>(stages.size())))
    {
        return RtSmokeDynamicEvalBuildResult::NoSelectedStage;
    }
    float origin[3] = {};
    bool hasOrigin = false;
    const bool frameOwned = drawSurf->space && drawSurf->space->pathTraceMaterialSnapshot;
    if (frameOwned)
    {
        std::copy(drawSurf->pathTraceSurfaceOrigin, drawSurf->pathTraceSurfaceOrigin + 3, origin);
        hasOrigin = true;
    }
    const srfTriangles_t* surfaceTri = frameOwned ? nullptr : drawSurf->frontEndGeo;
    if (surfaceTri)
    {
        idVec3 worldCenter;
        TransformSurfacePointToWorld(drawSurf, surfaceTri->bounds.GetCenter(), worldCenter);
        origin[0] = worldCenter.x;
        origin[1] = worldCenter.y;
        origin[2] = worldCenter.z;
        hasOrigin = true;
    }
    // Reuse the live predicate: reconstructing only part of its route POD loses
    // the single ambient alpha-blend fact required by switchable swinglights.
    const bool opaqueCompatibility = SmokeMaterialUsesOpaqueSwinglightCompatibility(material);
    const RtPathTraceRuntimeMaterialEvalPod evaluated =
        BuildPathTraceRuntimeMaterialEvalFromPod(
            true, materialId, stages.data(), stages.size(), regs,
            static_cast<std::size_t>(Max(0, material->GetNumRegisters())),
            opaqueCompatibility, origin, hasOrigin);
    if (podResult)
    {
        *podResult = evaluated;
    }

    return SmokeMaterialSampleFromEvaluation(material, materialId, evaluated, surfaceSample);
}

void AddSmokeDynamicMaterialEvalStatsInternal(RtSmokeMaterialStats& stats, const drawSurf_t* drawSurf, int indexes, uint32_t materialId)
{
    RtSmokeDynamicMaterialEvalSample surfaceSample;
    const RtSmokeDynamicEvalBuildResult buildResult = BuildSmokeDynamicMaterialEvalSampleForId(drawSurf, materialId, surfaceSample);
    switch (buildResult)
    {
        case RtSmokeDynamicEvalBuildResult::Built:
            break;
        case RtSmokeDynamicEvalBuildResult::NoRegisters:
            ++stats.dynamicEvalNoRegisterSurfaces;
            return;
        case RtSmokeDynamicEvalBuildResult::NoSelectedStage:
            ++stats.dynamicEvalNoSelectedStageSurfaces;
            return;
        case RtSmokeDynamicEvalBuildResult::NoMaterial:
        default:
            return;
    }

    ++stats.dynamicEvalSurfaces;
    stats.dynamicEvalTriangles += indexes / 3;
    stats.dynamicEvalStages += surfaceSample.enabledStages + surfaceSample.disabledStages;
    stats.dynamicEvalEnabledStages += surfaceSample.enabledStages;
    stats.dynamicEvalDisabledStages += surfaceSample.disabledStages;
    stats.dynamicEvalColorStages += surfaceSample.colorStages;
    stats.dynamicEvalAlphaStages += surfaceSample.alphaStages;
    stats.dynamicEvalAlphaTestStages += surfaceSample.alphaTestStages;
    stats.dynamicEvalTexMatrixStages += surfaceSample.texMatrixStages;
    stats.dynamicEvalDynamicImageStages += surfaceSample.dynamicImageStages;
    stats.dynamicEvalCinematicStages += surfaceSample.cinematicStages;
    stats.dynamicEvalGuiRenderTargetStages += surfaceSample.guiRenderTargetStages;
    stats.dynamicEvalProgramStages += surfaceSample.programStages;

    for (int sampleIndex = 0; sampleIndex < stats.dynamicEvalSampleCount; ++sampleIndex)
    {
        RtSmokeDynamicMaterialEvalSample& sample = stats.dynamicEvalSamples[sampleIndex];
        if (sample.id == surfaceSample.id)
        {
            SmokeDynamicEvalAccumulateSample(sample, surfaceSample, indexes);
            SmokeDynamicEvalAddMaterialSample(stats, surfaceSample, indexes);
            return;
        }
    }

    if (stats.dynamicEvalSampleCount < RT_SMOKE_DYNAMIC_MATERIAL_REASON_SAMPLES)
    {
        RtSmokeDynamicMaterialEvalSample& sample = stats.dynamicEvalSamples[stats.dynamicEvalSampleCount++];
        sample = surfaceSample;
        sample.surfaces = 1;
        sample.triangles = indexes / 3;
    }
    SmokeDynamicEvalAddMaterialSample(stats, surfaceSample, indexes);
}

void AddSmokeTranslucentDebugSample(RtSmokeMaterialStats& stats, const drawSurf_t* drawSurf, const srfTriangles_t* tri, int surfaceIndex, RtSmokeTranslucentSubtype subtype)
{
    if (stats.translucentDebugSampleCount >= RT_SMOKE_TRANSLUCENT_REASON_SAMPLES)
    {
        return;
    }

    const idMaterial* material = drawSurf ? drawSurf->material : nullptr;
    RtSmokeTranslucentSubtypeDebugSample& sample = stats.translucentDebugSamples[stats.translucentDebugSampleCount++];
    sample.valid = true;
    sample.subtype = subtype;
    sample.surfaceIndex = surfaceIndex;
    sample.verts = tri ? tri->numVerts : 0;
    sample.indexes = tri ? tri->numIndexes : 0;
    sample.materialName = material ? material->GetName() : "<none>";
    sample.coverage = material ? material->Coverage() : MC_BAD;
    sample.sort = material ? material->GetSort() : SS_BAD;
    sample.deform = material ? material->Deform() : DFRM_NONE;
    sample.info = BuildSmokeTranslucentClassifierInfo(material);
}

void AddSmokeDynamicGeometryStats(RtSmokeDynamicGeometryStats& stats, RtSmokeSurfaceClass surfaceClass, const drawSurf_t* drawSurf, const srfTriangles_t* tri, int indexes)
{
    switch (surfaceClass)
    {
        case RtSmokeSurfaceClass::RigidEntity:
            ++stats.rigidSurfaces;
            stats.rigidIndexes += indexes;
            break;
        case RtSmokeSurfaceClass::SkinnedDeformed:
        {
            bool verticesFromFrameCache = false;
            SmokeDrawSurfaceVertices(drawSurf, tri, verticesFromFrameCache);
            if (SmokeDrawSurfaceCpuSkinningJoints(drawSurf, tri, verticesFromFrameCache) != nullptr)
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
        }
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

void AddSmokeSurfaceClassStats(RtSmokeSurfaceClassStats& stats, RtSmokeSurfaceClass surfaceClass, int verts, int indexes)
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

uint64 BuildSmokeStaticSurfaceKey(const drawSurf_t* drawSurf, const srfTriangles_t* tri)
{
    uint64 hash = 14695981039346656037ull;
    const uintptr_t triPtr = reinterpret_cast<uintptr_t>(tri);
    const uintptr_t materialPtr = reinterpret_cast<uintptr_t>(drawSurf ? drawSurf->material : nullptr);
    hash = HashSmokeBytes(hash, &triPtr, sizeof(triPtr));
    hash = HashSmokeBytes(hash, &materialPtr, sizeof(materialPtr));
    if (tri)
    {
        hash = HashSmokeBytes(hash, &tri->numVerts, sizeof(tri->numVerts));
        hash = HashSmokeBytes(hash, &tri->numIndexes, sizeof(tri->numIndexes));
        hash = HashSmokeBytes(hash, &tri->ambientCache, sizeof(tri->ambientCache));
        hash = HashSmokeBytes(hash, &tri->indexCache, sizeof(tri->indexCache));
    }
    if (drawSurf && drawSurf->space)
    {
        hash = HashSmokeBytes(hash, drawSurf->space->modelMatrix, sizeof(drawSurf->space->modelMatrix));
    }
    return hash;
}

struct RtSmokeCapturedDynamicSurfaceKey
{
    int entityIndex = -1;
    const srfTriangles_t* tri = nullptr;
    uint32_t materialId = 0;
};

bool SmokeCapturedDynamicSurfaceMatches(const RtSmokeCapturedDynamicSurfaceKey& key, int entityIndex, const srfTriangles_t* tri, uint32_t materialId)
{
    return key.entityIndex == entityIndex && key.tri == tri && key.materialId == materialId;
}

bool SmokeDynamicSurfaceAlreadyCaptured(const std::vector<RtSmokeCapturedDynamicSurfaceKey>& capturedSurfaces, int entityIndex, const srfTriangles_t* tri, uint32_t materialId)
{
    for (const RtSmokeCapturedDynamicSurfaceKey& key : capturedSurfaces)
    {
        if (SmokeCapturedDynamicSurfaceMatches(key, entityIndex, tri, materialId))
        {
            return true;
        }
    }
    return false;
}

bool SmokeBoundsWithinRadius(const idBounds& bounds, const idVec3& origin, float radius)
{
    if (radius <= 0.0f || bounds.IsCleared())
    {
        return false;
    }

    const idVec3 center = bounds.GetCenter();
    const float expandedRadius = radius + bounds.GetRadius(center);
    return (center - origin).LengthSqr() <= expandedRadius * expandedRadius;
}

const idMaterial* SmokeResolveEntitySurfaceMaterial(const idRenderEntityLocal* entityDef, const modelSurface_t* surface)
{
    const idMaterial* shader = surface ? surface->shader : nullptr;
    if (!entityDef || !shader)
    {
        return shader;
    }

    if (entityDef->parms.customShader != nullptr)
    {
        if (shader->Deform())
        {
            return nullptr;
        }
        return entityDef->parms.customShader;
    }

    if (entityDef->parms.customSkin)
    {
        shader = entityDef->parms.customSkin->RemapShaderBySkin(shader);
    }

    return shader;
}

}

bool BuildSmokeDynamicMaterialEvalSampleForDrawSurf(
    const drawSurf_t* drawSurf,
    uint32_t materialId,
    RtSmokeDynamicMaterialEvalSample& sample)
{
    return BuildSmokeDynamicMaterialEvalSampleForId(drawSurf, materialId, sample) == RtSmokeDynamicEvalBuildResult::Built;
}

uint64 BuildSmokeStaticSurfaceKeyForDiagnostics(const drawSurf_t* drawSurf, const srfTriangles_t* tri)
{
    return BuildSmokeStaticSurfaceKey(drawSurf, tri);
}

void AddSmokeDynamicMaterialEvalStats(RtSmokeMaterialStats& stats, const drawSurf_t* drawSurf, int indexes)
{
    AddSmokeDynamicMaterialEvalStatsForMaterialId(stats, drawSurf, indexes, SmokeMaterialId(drawSurf ? drawSurf->material : nullptr));
}

static bool SmokeResidencySkipsDynamicMaterialEval(uint32_t materialId)
{
    if (r_pathTracingResidency.GetInteger() == 0 || r_pathTracingResidencyMaterial.GetInteger() == 0)
    {
        return false;
    }
    const RtSmokeMaterialTextureInfo* info = FindSmokeMaterialTextureInfoReadOnly(materialId);
    return info && SmokeMaterialTextureInfoHasMaterialMetadata(*info) && !info->isDynamic;
}

void AddSmokeDynamicMaterialEvalStatsForMaterialId(RtSmokeMaterialStats& stats, const drawSurf_t* drawSurf, int indexes, uint32_t materialId)
{
    if (SmokeResidencySkipsDynamicMaterialEval(materialId))
    {
        return;
    }
    AddSmokeDynamicMaterialEvalStatsInternal(stats, drawSurf, indexes, materialId);
}

static bool BuildSmokeRuntimeMaterialDecisionInput(
    const drawSurf_t* drawSurf,
    uint32_t baseMaterialId,
    RtPathTraceRuntimeMaterialVariantPod& key,
    RtPathTraceRuntimeMaterialEvalPod& runtimeEval,
    const RtPathTraceRuntimeMaterialEvalPod* prepared = nullptr)
{
    key = {};
    runtimeEval = {};
    runtimeEval.materialId = baseMaterialId;
    if (!drawSurf || !drawSurf->material || baseMaterialId == 0u)
    {
        return false;
    }
    if (SmokeResidencySkipsDynamicMaterialEval(baseMaterialId))
    {
        return false;
    }

    if (prepared)
    {
        runtimeEval = *prepared;
        if (runtimeEval.result != RtPathTraceRuntimeEvalBuildResult::Built) return false;
    }
    else
    {
        RtSmokeDynamicMaterialEvalSample surfaceSample;
        if (BuildSmokeDynamicMaterialEvalSampleForId(drawSurf, baseMaterialId, surfaceSample, &runtimeEval) !=
            RtSmokeDynamicEvalBuildResult::Built) return false;
    }

    const viewEntity_t* space = drawSurf->space;
    if (space && space->pathTraceMaterialSnapshot)
    {
        key = BuildPathTraceRuntimeMaterialVariantKeyFromPod(baseMaterialId,
            space->pathTraceRenderDefIndex, space->pathTraceEntityNum,
            drawSurf->modelSurfaceIndex, 0, nullptr, 0);
        return true;
    }
    const idRenderEntityLocal* entity = space ? space->entityDef : nullptr;
    const renderEntity_t* renderEntity = entity ? &entity->parms : nullptr;
    if (!entity)
    {
        return false;
    }

    const int entityNum = renderEntity ? renderEntity->entityNum : -1;
    bool snapshotOracleActive = false;
    if (BuildPathTraceCaptureSerialRuntimeMaterialVariantKey(
            baseMaterialId, entity->index, entityNum,
            key, snapshotOracleActive))
    {
        return true;
    }
    if (snapshotOracleActive)
    {
        return false;
    }

    const idRenderModel* renderModel = renderEntity ? renderEntity->hModel : nullptr;
    const int resolvedModelSurfaceIndex = ResolvePathTraceRigidModelSurfaceIndex(
        renderModel, drawSurf->frontEndGeo, drawSurf->modelSurfaceIndex);
    key = BuildPathTraceRuntimeMaterialVariantKeyFromPod(
        baseMaterialId, entity->index, entityNum,
        resolvedModelSurfaceIndex,
        static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
            drawSurf->frontEndGeo)), nullptr, 0);
    return true;
}

static RtPathTraceRuntimeMaterialDecisionPod BuildSmokeRuntimeMaterialDecision(
    const drawSurf_t* drawSurf,
    uint32_t baseMaterialId)
{
    RtPathTraceRuntimeMaterialVariantPod key;
    RtPathTraceRuntimeMaterialEvalPod runtimeEval;
    (void)BuildSmokeRuntimeMaterialDecisionInput(
        drawSurf, baseMaterialId, key, runtimeEval);
    key.baseMaterialId = baseMaterialId;
    const RtSmokeMaterialTextureInfo* baseInfo =
        FindSmokeMaterialTextureInfoReadOnly(baseMaterialId);
    return SelectPathTraceRuntimeMaterialVariant(
        key, runtimeEval, baseInfo != nullptr,
        baseInfo && SmokeMaterialTextureInfoHasMaterialMetadata(*baseInfo) &&
            !baseInfo->isDynamic,
        [](std::uint32_t candidate)
        {
            return FindSmokeMaterialTextureInfoReadOnly(candidate) != nullptr;
        },
        [](std::uint32_t candidate, std::uint32_t base)
        {
            return SmokeMaterialTextureVariantBase(candidate) == base;
        });
}

uint32_t SmokeRuntimeMaterialVariantIdForDrawSurf(
    const drawSurf_t* drawSurf, uint32_t baseMaterialId)
{
    return BuildSmokeRuntimeMaterialDecision(
        drawSurf, baseMaterialId).initialCandidateId;
}

static bool SmokeMaterialUsesRuntimeMaterialState(const idMaterial* material)
{
    if (!material)
    {
        return false;
    }

    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage)
        {
            continue;
        }
        if (SmokeRegisterDependsOnRuntime(material, stage->conditionRegister))
        {
            return true;
        }
        if (stage->hasAlphaTest && SmokeRegisterDependsOnRuntime(material, stage->alphaTestRegister))
        {
            return true;
        }
        for (int component = 0; component < 4; ++component)
        {
            if (SmokeRegisterDependsOnRuntime(material, stage->color.registers[component]))
            {
                return true;
            }
        }
        if (stage->texture.hasMatrix)
        {
            for (int row = 0; row < 2; ++row)
            {
                for (int column = 0; column < 3; ++column)
                {
                    if (SmokeRegisterDependsOnRuntime(material, stage->texture.matrix[row][column]))
                    {
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

static uint32_t SmokeRuntimeMaterialVariantIdForEntitySurface(const idRenderEntityLocal* entity, int modelSurfaceIndex, const idMaterial* material, uint32_t baseMaterialId)
{
    if (!entity || !material || baseMaterialId == 0u ||
        SmokeResidencySkipsDynamicMaterialEval(baseMaterialId) ||
        !SmokeMaterialUsesRuntimeMaterialState(material))
    {
        return baseMaterialId;
    }

    return SmokeRuntimeMaterialVariantIdForEntitySurfaceKey(entity, modelSurfaceIndex, baseMaterialId);
}

static uint32_t SmokeRuntimeMaterialTableIdPrepared(const drawSurf_t* drawSurf, uint32_t baseMaterialId,
    const RtPathTraceRuntimeMaterialEvalPod* prepared, bool baseHydrated)
{
    RtPathTraceRuntimeMaterialVariantPod key;
    RtPathTraceRuntimeMaterialEvalPod runtimeEval;
    (void)BuildSmokeRuntimeMaterialDecisionInput(
        drawSurf, baseMaterialId, key, runtimeEval, prepared);
    key.baseMaterialId = baseMaterialId;
    const auto selectFromLiveRegistry = [&]()
    {
        const RtSmokeMaterialTextureInfo* baseInfo =
            FindSmokeMaterialTextureInfoReadOnly(baseMaterialId);
        return SelectPathTraceRuntimeMaterialVariant(
            key, runtimeEval, baseInfo != nullptr,
            baseInfo && SmokeMaterialTextureInfoHasMaterialMetadata(*baseInfo) &&
                !baseInfo->isDynamic,
            [](std::uint32_t candidate)
            {
                return FindSmokeMaterialTextureInfoReadOnly(candidate) != nullptr;
            },
            [](std::uint32_t candidate, std::uint32_t base)
            {
                return SmokeMaterialTextureVariantBase(candidate) == base;
            });
    };
    RtPathTraceRuntimeMaterialDecisionPod decision = selectFromLiveRegistry();
    if (decision.initialCandidateId == baseMaterialId)
    {
        NotePathTraceCaptureSerialRuntimeMaterial(UINT32_MAX, decision);
        return baseMaterialId;
    }
    if (!baseHydrated) RegisterSmokeMaterialTextureInfo(drawSurf ? drawSurf->material : nullptr);
    if (decision.chosenMaterialId != baseMaterialId &&
        RegisterSmokeMaterialTextureVariant(
            decision.chosenMaterialId, baseMaterialId))
    {
        NotePathTraceCaptureSerialRuntimeMaterial(UINT32_MAX, decision);
        NotePathTraceCaptureSerialMaterialVariant(
            baseMaterialId, decision.initialCandidateId,
            decision.chosenMaterialId, decision.collisionCount,
            decision.fallbackUsed,
            drawSurf && drawSurf->material ? drawSurf->material->GetName() : "");
        return decision.chosenMaterialId;
    }
    decision.chosenMaterialId = baseMaterialId;
    decision.fallbackUsed = true;
    NotePathTraceCaptureSerialRuntimeMaterial(UINT32_MAX, decision);
    NotePathTraceCaptureSerialMaterialVariant(
        baseMaterialId,
        decision.initialCandidateId,
        baseMaterialId,
        decision.collisionCount,
        true,
        drawSurf && drawSurf->material ? drawSurf->material->GetName() : "");
    return baseMaterialId;
}

uint32_t SmokeRuntimeMaterialTableIdForDrawSurf(const drawSurf_t* drawSurf, uint32_t baseMaterialId)
{
    return SmokeRuntimeMaterialTableIdPrepared(drawSurf, baseMaterialId, nullptr, false);
}

static bool CapturePathTraceRuntimeMaterialStages(
    const idMaterial* material,
    RtPathTraceRuntimeMaterialStagePod* stages,
    int stageCapacity)
{
    if (!material || stageCapacity != material->GetNumStages() ||
        (stageCapacity != 0 && stages == nullptr))
    {
        return false;
    }
    for (int stageIndex = 0; stageIndex < stageCapacity; ++stageIndex)
    {
        RtPathTraceRuntimeMaterialStagePod& output = stages[stageIndex];
        output = {};
        output.stageIndex = stageIndex;
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage)
        {
            continue;
        }
        output.valid = true;
        output.usesPerSurfaceState =
            SmokeStageUsesPerSurfaceMaterialState(material, stage);
        output.diffuse = stage->lighting == SL_DIFFUSE;
        output.conditionRegister = stage->conditionRegister;
        for (int component = 0; component < 4; ++component)
        {
            output.colorRegisters[component] =
                stage->color.registers[component];
        }
        output.hasAlphaTest = stage->hasAlphaTest;
        output.alphaTestRegister = stage->alphaTestRegister;
        output.hasTexMatrix = stage->texture.hasMatrix;
        for (int row = 0; row < 2; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                output.texMatrixRegisters[row * 3 + column] =
                    stage->texture.matrix[row][column];
            }
        }
        output.dynamicImage = stage->texture.dynamic != DI_STATIC ||
            stage->texture.dynamicFrameCount > 0;
        output.cinematic = stage->texture.cinematic != nullptr;
        output.guiRenderTarget = stage->texture.texgen == TG_SCREEN ||
            stage->texture.texgen == TG_SCREEN2 || SmokeStageIsRenderMap(stage);
        output.program = stage->newStage != nullptr;
        output.emissiveLike = SmokeDynamicEvalStageIsEmissiveLike(stage);
        output.drawStateBits = static_cast<std::uint64_t>(stage->drawStateBits);
        output.lighting = static_cast<std::int32_t>(stage->lighting);
        output.texgen = static_cast<std::int32_t>(stage->texture.texgen);
        output.imageDynamic = static_cast<std::int32_t>(stage->texture.dynamic);
        output.imagePresent = stage->texture.image != nullptr;
    }
    return true;
}

// Owner-local immutable numeric definitions. Revision keys never require retaining
// or later dereferencing an idMaterial pointer; images/bindings remain live elsewhere.
struct SmokeRewriteConstantMaterialRoute
{
    RtCpuRewriteMaterialFrameSurface route;
    bool eligible = false;
};
static SmokeRewriteConstantMaterialRoute BuildSmokeRewriteConstantMaterialRoute(const idMaterial* material)
{
    SmokeRewriteConstantMaterialRoute result;
    if (!material || !material->ConstantRegisters() || material->Deform()!=DFRM_NONE ||
        material->GetNumStages()<0 || material->GetNumStages()>MAX_SHADER_STAGES ||
        material->GetNumRegisters()<0 || material->GetNumRegisters()>MAX_EXPRESSION_REGISTERS) return result;
    RtCpuRewriteMaterialSource source;
    source.stages.resize(material->GetNumStages());
    if (!CapturePathTraceRuntimeMaterialStages(material,source.stages.data(),material->GetNumStages())) return result;
    source.opaqueCompatibility=SmokeMaterialUsesOpaqueSwinglightCompatibility(material);
    // With no selected runtime stage, the old route chooses its primary fallback.
    // Be conservative about ambient-only definitions whose fallback uses registry images.
    for (int i=0;i<material->GetNumStages();++i) {
        const auto* stage=material->GetStage(i);
        if (source.primaryFallbackStage<0 && stage->lighting==SL_DIFFUSE) source.primaryFallbackStage=i;
        if (source.normalStage<0 && stage->lighting==SL_BUMP && stage->texture.hasMatrix) source.normalStage=i;
    }
    if (source.primaryFallbackStage<0) return result;
    result.eligible=BuildRtCpuConstantMaterialRoute(true,SmokeMaterialId(material),source,
        material->ConstantRegisters(),material->GetNumRegisters(),result.route);
    return result;
}

bool CaptureSmokeRewriteMaterialMembership(const viewDef_t* viewDef,
    RtSmokeRewriteMaterialMembership& snapshot)
{
    OPTICK_EVENT("PT CPU Material Membership Snapshot");
    static_assert(RtCpuMaterialMembership::kMaxRows == RtCpuRewriteMaterialInput::kMaxSurfaces, "membership cap");
    snapshot = {};
    if (!viewDef || !viewDef->pathTraceRewriteRootFrame) return false;
    snapshot.membership.Reserve(viewDef->pathTraceRewriteSurfaceCount);
    snapshot.rows.reserve(std::min<size_t>(viewDef->pathTraceRewriteSurfaceCount, RtCpuMaterialMembership::kMaxRows));
    std::unordered_set<uint32_t> bases;
    for (const drawSurf_t* ds = viewDef->pathTraceRewriteSurfaces; ds; ds = ds->nextOnLight)
    {
        if (!ds->material || !ds->space || !ds->space->pathTraceMaterialSnapshot || ds->modelSurfaceIndex < 0) continue;
        if (snapshot.rows.size() == RtCpuMaterialMembership::kMaxRows) return false;
        const uint32_t base = SmokeMaterialId(ds->material);
        const uint32_t entity = static_cast<uint32_t>(ds->space->pathTraceRenderDefIndex);
        const uint32_t surface = static_cast<uint32_t>(ds->modelSurfaceIndex);
        if (!snapshot.membership.Add(entity, surface, base)) return false;
        snapshot.rows.push_back({ds, base, entity, surface});
        if (bases.insert(base).second) snapshot.baseIds.push_back(base);
    }
    snapshot.rootFrame = viewDef->pathTraceRewriteRootFrame;
    OPTICK_TAG("materialMembershipRows", static_cast<uint32_t>(snapshot.rows.size()));
    OPTICK_TAG("materialMembershipKeys", static_cast<uint32_t>(snapshot.membership.Size()));
    return true;
}

bool BuildSmokeRewriteMaterialSamples(const viewDef_t* viewDef,
    RtCpuProducerRewriteService& service, std::vector<RtSmokeDynamicMaterialEvalSample>& samples, RtCpuRewriteMaterialFrame& frame,
    std::shared_ptr<RtSmokeMaterialBindingFrame>& bindings,
    const std::function<bool()>& prepareOwnerLightInput,
    const RtSmokeRewriteMaterialMembership& membership, nvrhi::ICommandList* commandList)
{
    bool consumed = false;
    uint32_t rejectReason = 1;
    struct FrameReport
    {
        bool& consumed; uint32_t& reason;
        ~FrameReport()
        {
            OPTICK_TAG("materialFrameConsumed", consumed ? 1u : 0u);
            OPTICK_TAG("materialFrameRejected", consumed ? 0u : 1u);
            OPTICK_TAG("materialFrameRejectReason", consumed ? 0u : reason);
        }
    } frameReport { consumed, rejectReason };
    RtCpuRewriteMaterialInput input;
    input.prepareFrame = true;
    input.rootFrame = viewDef->pathTraceRewriteRootFrame;
    if (!input.rootFrame || membership.rootFrame != input.rootFrame) return false;
    OPTICK_TAG("materialMembershipSnapshotReused", 1u);
    OPTICK_TAG("materialMembershipLinkedWalksSkipped", 1u);
    std::vector<const drawSurf_t*> surfaces;
    std::vector<const idMaterial*> materials;
    std::unordered_map<const idMaterial*, uint32_t> sourceLookup;
    static thread_local std::unordered_map<uint64_t,SmokeRewriteConstantMaterialRoute> constantDefinitions;
    static thread_local uint64_t constantLifecycle=0;
    if (constantLifecycle!=service.LifecycleGeneration()) {
        constantDefinitions.clear();constantLifecycle=service.LifecycleGeneration();
    }
    uint32_t candidates=0,constantBuilt=0,constantReused=0;

    size_t charged = 0;
    const auto charge = [&](size_t bytes) {
        if (bytes > RtCpuRewriteMaterialInput::kMaxBytes - charged) return false;
        charged += bytes; return true;
    };
    {
        OPTICK_EVENT("PT CPU Material Capture");
        for (const auto& member : membership.rows)
        {
            const drawSurf_t* ds = member.surface;
            if (++candidates > RtCpuRewriteMaterialInput::kMaxSurfaces) return false;
            const idMaterial* material = ds->material;
            const uint32_t baseId=member.baseId;
            const float* constantRegisters=material->ConstantRegisters();
            if (constantRegisters && (!ds->shaderRegisters || ds->shaderRegisters==constantRegisters)) {
                const uint64_t revision=material->GetDefinitionRevision();
                auto cached=constantDefinitions.find(revision);
                if (cached==constantDefinitions.end()) {
                    auto definition=BuildSmokeRewriteConstantMaterialRoute(material);
                    if (constantDefinitions.size()>=8192) constantDefinitions.clear();
                    cached=constantDefinitions.emplace(revision,std::move(definition)).first;
                    ++constantBuilt;
                } else ++constantReused;
                const auto* info=FindSmokeMaterialTextureInfoReadOnly(baseId);
                if (cached->second.eligible && cached->second.route.baseId==baseId && info && SmokeMaterialTextureInfoHasMaterialMetadata(*info) && !info->isDynamic) {
                    if (!charge(sizeof(RtCpuRewriteMaterialFrameSurface))) return false;
                    auto row=cached->second.route;
                    row.entityIndex=member.entityIndex;
                    row.modelSurfaceIndex=member.surfaceIndex;
                    input.constantSurfaces.push_back(row);
                    continue; // no registers/stages/evaluation/variant/sample for this surface
                }
            }
            if (!charge(sizeof(RtCpuRewriteMaterialSurface) + sizeof(RtPathTraceRuntimeMaterialEvalPod))) return false;
            auto found = sourceLookup.find(material);
            uint32_t sourceIndex = 0;
            if (found == sourceLookup.end())
            {
                const int count = material->GetNumStages();
                if (count < 0 || static_cast<size_t>(count) > RtCpuRewriteMaterialInput::kMaxBytes / sizeof(RtPathTraceRuntimeMaterialStagePod) ||
                    !charge(sizeof(RtCpuRewriteMaterialSource) + static_cast<size_t>(count) * sizeof(RtPathTraceRuntimeMaterialStagePod))) return false;
                RtCpuRewriteMaterialSource source;
                source.stages.resize(count);
                if (!CapturePathTraceRuntimeMaterialStages(material, source.stages.data(), count)) return false;
                source.opaqueCompatibility = SmokeMaterialUsesOpaqueSwinglightCompatibility(material);
                sourceIndex = static_cast<uint32_t>(input.sources.size());
                sourceLookup.emplace(material, sourceIndex);
                input.sources.push_back(std::move(source));
                materials.push_back(material);
            }
            else sourceIndex = found->second;
            RtCpuRewriteMaterialSurface surface;
            surface.source = sourceIndex;
            surface.materialId = baseId;
            surface.entityIndex = static_cast<int>(member.entityIndex);
            surface.entityNum = ds->space->pathTraceEntityNum;
            surface.modelSurfaceIndex = static_cast<int>(member.surfaceIndex);
            const float* regs = ds->shaderRegisters ? ds->shaderRegisters : material->ConstantRegisters();
            const int count = regs ? material->GetNumRegisters() : 0;
            if (count < 0 || static_cast<size_t>(count) > RtCpuRewriteMaterialInput::kMaxBytes / sizeof(float) ||
                !charge(static_cast<size_t>(count) * sizeof(float))) return false;
            surface.hasRegisters = regs != nullptr;
            surface.registerBegin = static_cast<uint32_t>(input.registers.size());
            surface.registerCount = count;
            if (count) input.registers.insert(input.registers.end(), regs, regs + count);
            std::copy(ds->pathTraceSurfaceOrigin, ds->pathTraceSurfaceOrigin + 3, surface.origin);
            input.surfaces.push_back(surface);
            surfaces.push_back(ds);
        }
        OPTICK_TAG("materialCaptureCandidateSurfaces",candidates);
        OPTICK_TAG("materialConstantSurfacesSkipped",static_cast<uint32_t>(input.constantSurfaces.size()));
        OPTICK_TAG("materialConstantDefinitionsBuilt",constantBuilt);
        OPTICK_TAG("materialConstantDefinitionsReused",constantReused);
        OPTICK_TAG("materialConstantCacheEntries",static_cast<uint32_t>(constantDefinitions.size()));
        OPTICK_TAG("materialCapturedSurfaces", static_cast<uint32_t>(surfaces.size()));
        OPTICK_TAG("materialCapturedSources", static_cast<uint32_t>(materials.size()));
        OPTICK_TAG("materialCapturedBytes", static_cast<uint32_t>(charged));
    }
    // Hydrate base metadata before freezing the identity registry. Cold constant
    // materials need registration too; reload must not be their first hydration.
    // No mutable registry access or resource handle enters the frame producer.
    uint32_t firstUseDefinitions=0;
    for (size_t i = 0; i < materials.size(); ++i)
    {
        auto& source = input.sources[i];
        const auto* info = FindSmokeMaterialTextureInfoReadOnly(SmokeMaterialId(materials[i]));
        const bool needsMetadata = !info || !SmokeMaterialTextureInfoHasMaterialMetadata(*info);
        if (needsMetadata || info->isDynamic)
        {
            bool varying = source.opaqueCompatibility;
            for (const auto& stage : source.stages) varying = varying || stage.usesPerSurfaceState;
            if (needsMetadata || varying) {
                RegisterSmokeMaterialTextureInfo(materials[i]);
                if (needsMetadata) ++firstUseDefinitions;
            }
        }
        info = FindSmokeMaterialTextureInfoReadOnly(SmokeMaterialId(materials[i]));
        // Preserve the legacy primary diffuse/ambient and first matrix-bearing bump choices.
        for (int stageIndex = 0; stageIndex < materials[i]->GetNumStages(); ++stageIndex)
        {
            const auto* stage = materials[i]->GetStage(stageIndex);
            if (!stage) continue;
            if (source.normalStage < 0 && stage->lighting == SL_BUMP && stage->texture.hasMatrix)
                source.normalStage = stageIndex;
            if (stage->lighting == SL_DIFFUSE)
            {
                source.primaryFallbackStage = stageIndex;
                break;
            }
            if (source.primaryFallbackStage < 0 && stage->lighting == SL_AMBIENT && info &&
                stage->texture.image == info->diffuseImage) source.primaryFallbackStage = stageIndex;
        }
        // Bump stages may follow the diffuse stage.
        if (source.normalStage < 0)
            for (int stageIndex = 0; stageIndex < materials[i]->GetNumStages(); ++stageIndex)
            {
                const auto* stage = materials[i]->GetStage(stageIndex);
                if (stage && stage->lighting == SL_BUMP && stage->texture.hasMatrix)
                { source.normalStage = stageIndex; break; }
            }
    }
    OPTICK_TAG("materialFirstUseDefinitions", firstUseDefinitions);
    SnapshotSmokeMaterialIdentities(input.registry);
    if (!input.WithinCapacity()) return false;
    // Binding capture includes excluded constant bases; resource lifetime stays current.
    input.registryGeneration = SmokeMaterialTextureRegistryGeneration();
    const auto job = service.SubmitMaterials(std::move(input));
    rejectReason = 2;
    if (!job) return false;
    std::shared_ptr<RtCpuRewriteLightJob> bindingJob;
    // Failed capture/application must drain both slots, without publishing an old
    // product or retaining renderer resources in a worker closure.
    struct MaterialJobsGuard {
        RtCpuProducerRewriteService& service;
        const std::shared_ptr<RtCpuRewriteMaterialJob>& numeric;
        const std::shared_ptr<RtCpuRewriteLightJob>& binding;
        uint64_t root;
        bool numericJoined=false, bindingJoined=false;
        ~MaterialJobsGuard() {
            if (!numericJoined) service.FinishMaterials(numeric);
            if (binding && !bindingJoined) service.FinishMaterialBindingPreparation(binding,root);
        }
    } jobsGuard{service,job,bindingJob,viewDef->pathTraceRewriteRootFrame};
    // Synchronous owner work on independent exact-frame light bytes. Never pass
    // this renderer closure to a worker; the guard drains numeric work on failure.
    rejectReason = 9;
    if (!prepareOwnerLightInput || !prepareOwnerLightInput()) return false;
    struct BindingWork { RtCpuMaterialBindingPlanInput input; RtCpuMaterialBindingPlan output; };
    auto bindingWork=std::make_shared<BindingWork>();
    bindings = SnapshotSmokeMaterialBindingFrame(bindingWork->input, RtCpuMaterialBindingPlanInput::kMaxBytes, &membership.baseIds);
    rejectReason = 8;
    if (!bindings) return false;
    const bool prepareBindings=SmokeMaterialBindingNeedsPreparation(*bindings);
    OPTICK_TAG("materialBindingWorkerSkipped",prepareBindings ? 0u : 1u);
    if (prepareBindings) {
        bindingJob=service.SubmitMaterialBindingPreparation(viewDef->pathTraceRewriteRootFrame,
            bindingWork->input.ChargedBytes(),[owned=bindingWork] {
                const bool valid=BuildRtCpuMaterialBindingPlan(owned->input,owned->output);
                OPTICK_TAG("materialBindingPreparedRows",static_cast<uint32_t>(owned->output.rows.size()));
                OPTICK_TAG("materialBindingPreparedRules",static_cast<uint32_t>(owned->output.rules.size()));
                return valid;
            });
        if (!bindingJob) return false;
    }
    if (!ResolveSmokeMaterialBindingResources(*bindings, commandList)) return false;
    rejectReason = 2;
    const bool numericComplete=service.FinishMaterials(job);jobsGuard.numericJoined=true;
    if (!numericComplete) return false;
    rejectReason = 3;
    if (job->input.rootFrame != viewDef->pathTraceRewriteRootFrame) return false;
    rejectReason = 4;
    if (job->input.registryGeneration != SmokeMaterialTextureRegistryGeneration()) return false;
    rejectReason = 5;
    if (job->output.size() != surfaces.size() || job->frame.decisions.size() != surfaces.size()) return false;
    OPTICK_EVENT("PT CPU Material Publish");
    {
        OPTICK_EVENT("PT CPU Material Emissive Samples Apply");
        samples.resize(job->frame.emissiveSampleOrdinals.size());
        for (size_t i=0;i<samples.size();++i)
        {
            const uint32_t ordinal=job->frame.emissiveSampleOrdinals[i];
            if (ordinal>=surfaces.size()) return false;
            SmokeMaterialSampleFromEvaluation(surfaces[ordinal]->material,
                job->frame.decisions[ordinal].chosenMaterialId,job->output[ordinal],samples[i]);
        }
        OPTICK_TAG("materialOwnerFullSampleConversionsSkipped",static_cast<uint32_t>(surfaces.size()));
        OPTICK_TAG("materialOwnerEmissiveSamples",static_cast<uint32_t>(samples.size()));
    }
    rejectReason = 7;
    const bool bindingComplete=!prepareBindings || service.FinishMaterialBindingPreparation(bindingJob,viewDef->pathTraceRewriteRootFrame);
    jobsGuard.bindingJoined=true;
    if (!bindingComplete || !CompleteSmokeMaterialBindingFrame(*bindings,std::move(bindingWork->output))) return false;
    rejectReason = 6;
    OPTICK_EVENT("PT CPU Material Variants Apply");
    for (size_t i = 0; i < surfaces.size(); ++i)
    {
        const auto& decision = job->frame.decisions[i];
        const uint32_t base = job->input.surfaces[i].materialId;
        const uint32_t id = decision.chosenMaterialId;
        if (id != base && !RegisterSmokeMaterialTextureVariant(id, base, bindings.get())) return false;
        NotePathTraceCaptureSerialRuntimeMaterial(UINT32_MAX, decision);
        if (decision.initialCandidateId != base)
            NotePathTraceCaptureSerialMaterialVariant(base, decision.initialCandidateId, id,
                decision.collisionCount, decision.fallbackUsed, surfaces[i]->material->GetName());
    }
    frame = std::move(job->frame);
    consumed = true;
    OPTICK_TAG("materialSerialEvaluationsSkipped", static_cast<uint32_t>(surfaces.size()));
    OPTICK_TAG("materialFrameOwnerReplaySkipped", 1u);
    return true;
}

uint32_t SmokeRuntimeMaterialTableIdForEntitySurface(const idRenderEntityLocal* entity, int modelSurfaceIndex, const idMaterial* material, uint32_t baseMaterialId)
{
    const uint32_t variantMaterialId = SmokeRuntimeMaterialVariantIdForEntitySurface(entity, modelSurfaceIndex, material, baseMaterialId);
    if (variantMaterialId == baseMaterialId)
    {
        return baseMaterialId;
    }
    RegisterSmokeMaterialTextureInfo(material);
    uint32_t candidateMaterialId = variantMaterialId;
    for (uint32_t attempt = 0; attempt < 16u; ++attempt)
    {
        if (candidateMaterialId != 0u &&
            candidateMaterialId != baseMaterialId &&
            RegisterSmokeMaterialTextureVariant(candidateMaterialId, baseMaterialId))
        {
            return candidateMaterialId;
        }
        candidateMaterialId = SmokeRuntimeMaterialVariantHashValue(candidateMaterialId ^ 0x9e3779b9u, attempt + 1u) | 0x80000000u;
    }
    return baseMaterialId;
}

bool CaptureDoomSurfacesForSmokeTest(const viewDef_t* viewDef, std::vector<PathTraceSmokeVertex>& vertexData, std::vector<uint32_t>& indexData, std::vector<uint32_t>& triangleClassData, std::vector<uint32_t>& triangleMaterialData, std::vector<uint32_t>* triangleInstanceData, std::vector<uint32_t>* triangleIdentityData, RtSmokeGeometryUniverse& geometryUniverse, bool& staticCacheChanged, idVec3& captureAnchor, int& sourceSurfaces, int& sourceVerts, int& sourceIndexes, int& anchorTriangle, RtSmokeSurfaceClassStats& classStats, RtSmokeSurfaceSkipStats& skipStats, RtSmokeDynamicGeometryStats& dynamicStats, RtSmokeAttributeStats& attributeStats, RtSmokeMaterialStats& materialStats, RtSmokeBucketRanges& bucketRanges, RtSmokeSceneCaptureTiming& captureTiming, std::vector<RtSmokeSkinnedSurfaceRecord>* skinnedSurfaceRecords, bool skipStaticWorldCapture, bool skipPromotedStaticSurfaceCapture, bool skipDynamicCapture, std::vector<uint64_t>* staticWalkedIds, std::vector<uint32_t>* staticWalkedTriangles)
{
    OPTICK_EVENT("PT Capture Doom Surfaces Detail");

    sourceSurfaces = 0;
    sourceVerts = 0;
    sourceIndexes = 0;
    staticCacheChanged = false;
    int anchorSurface = -1;
    anchorTriangle = -1;
    classStats = RtSmokeSurfaceClassStats();
    skipStats = RtSmokeSurfaceSkipStats();
    dynamicStats = RtSmokeDynamicGeometryStats();
    attributeStats = RtSmokeAttributeStats();
    materialStats = RtSmokeMaterialStats();
    bucketRanges = RtSmokeBucketRanges();
    captureTiming = RtSmokeSceneCaptureTiming();

    if (!viewDef || !viewDef->drawSurfs)
    {
        return false;
    }

    const int anchorStartMs = Sys_Milliseconds();
    {
        OPTICK_EVENT("PT Capture Anchor");
        if (r_pathTracingAnchorRaycast.GetInteger() == 0)
        {
            captureAnchor = viewDef->renderView.vieworg;
            anchorSurface = 0;
        }
        else if (!FindCenterCameraRayAnchor(viewDef, captureAnchor, anchorSurface, anchorTriangle, &captureTiming))
        {
            captureTiming.anchorMs = Sys_Milliseconds() - anchorStartMs;
            return false;
        }
    }
    captureTiming.anchorMs = Sys_Milliseconds() - anchorStartMs;

    {
        OPTICK_EVENT("PT Capture Reserve Buffers");
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
    std::vector<PathTraceSmokeVertex>& staticVertexCache = geometryUniverse.StaticVertices();
    std::vector<uint32_t>& staticIndexCache = geometryUniverse.StaticIndexes();
    std::vector<uint32_t>& staticTriangleClassCache = geometryUniverse.StaticTriangleClasses();
    std::vector<uint32_t>& staticTriangleMaterialCache = geometryUniverse.StaticTriangleMaterials();
    geometryUniverse.ReserveStaticSurfaceRecords(geometryUniverse.StaticSurfaceRecords().size() + static_cast<size_t>(viewDef->numDrawSurfs));

    std::vector<PathTraceSmokeVertex> bucketVertexData[RT_SMOKE_CLASS_COUNT];
    std::vector<uint32_t> bucketIndexData[RT_SMOKE_CLASS_COUNT];
    std::vector<uint32_t> bucketTriangleClassData[RT_SMOKE_CLASS_COUNT];
    std::vector<uint32_t> bucketTriangleMaterialData[RT_SMOKE_CLASS_COUNT];
    std::vector<uint32_t> bucketTriangleInstanceData[RT_SMOKE_CLASS_COUNT];
    std::vector<uint32_t> bucketTriangleIdentityData[RT_SMOKE_CLASS_COUNT];
    for (int bucketIndex = 0; bucketIndex < RT_SMOKE_CLASS_COUNT; ++bucketIndex)
    {
        bucketVertexData[bucketIndex].reserve(RT_SMOKE_INITIAL_RESERVE_VERTS / RT_SMOKE_CLASS_COUNT);
        bucketIndexData[bucketIndex].reserve(RT_SMOKE_INITIAL_RESERVE_INDEXES / RT_SMOKE_CLASS_COUNT);
        bucketTriangleClassData[bucketIndex].reserve(RT_SMOKE_INITIAL_RESERVE_INDEXES / (3 * RT_SMOKE_CLASS_COUNT));
        bucketTriangleMaterialData[bucketIndex].reserve(RT_SMOKE_INITIAL_RESERVE_INDEXES / (3 * RT_SMOKE_CLASS_COUNT));
        bucketTriangleInstanceData[bucketIndex].reserve(RT_SMOKE_INITIAL_RESERVE_INDEXES / (3 * RT_SMOKE_CLASS_COUNT));
        bucketTriangleIdentityData[bucketIndex].reserve(RT_SMOKE_INITIAL_RESERVE_INDEXES / (3 * RT_SMOKE_CLASS_COUNT));
    }

    uint64 dynamicAdmissionBytes = 0;
    uint64 dynamicAdmissionSurfaces = 0;
    const RtSmokeGeometryAdmissionBudget dynamicAdmissionBudget =
        BuildSmokeDynamicGeometryAdmissionBudget();
    const RtSmokeGeometryAdmissionBudget staticAdmissionBudget =
        BuildSmokeStaticGeometryAdmissionBudget();
    std::vector<RtSmokeCapturedDynamicSurfaceKey> capturedDynamicSurfaces;
    capturedDynamicSurfaces.reserve(static_cast<size_t>(viewDef->numDrawSurfs));

    {
        OPTICK_EVENT("PT Capture Static Pass");
        for (int surfaceIndex = 0; !skipStaticWorldCapture && surfaceIndex < viewDef->numDrawSurfs; ++surfaceIndex)
        {
            const drawSurf_t* drawSurf = viewDef->drawSurfs[surfaceIndex];
            const srfTriangles_t* tri = nullptr;
            const int validationStartMs = Sys_Milliseconds();
            if (!ValidateSmokeDrawSurface(viewDef, drawSurf, tri, nullptr))
            {
                captureTiming.validationMs += Sys_Milliseconds() - validationStartMs;
                continue;
            }
            captureTiming.validationMs += Sys_Milliseconds() - validationStartMs;

            if (PathTraceParticleCompositeSurfaceRoute(viewDef, drawSurf, tri) == RtPathTraceParticleSurfaceRoute::CompositeOnly)
            {
                continue;
            }
            if (UnifiedPtDiagnosticRemovesAlphaClipSurface(drawSurf->material))
            {
                ++skipStats.alphaClipDiagnostic;
                continue;
            }

            const int classifyStartMs = Sys_Milliseconds();
            const RtSmokeSurfaceClass surfaceClass = ClassifySmokeSurface(viewDef, drawSurf, tri);
            captureTiming.staticPassClassifyMs += Sys_Milliseconds() - classifyStartMs;
            if (surfaceClass != RtSmokeSurfaceClass::StaticWorld)
            {
                continue;
            }

            const RtSmokeTranslucentSubtype translucentSubtype = RtSmokeTranslucentSubtype::Unknown;
            const uint32_t surfaceClassId = SmokeSurfaceClassAndSubtypeId(surfaceClass, translucentSubtype);
            const uint32_t baseMaterialId = SmokeMaterialId(drawSurf->material);
            const uint32_t materialId = SmokeRuntimeMaterialTableIdForDrawSurf(drawSurf, baseMaterialId);
            const uint64 staticSurfaceKey = BuildSmokeStaticSurfaceKey(drawSurf, tri);
            const int cacheLookupStartMs = Sys_Milliseconds();
            RtSmokePersistentStaticSurfaceRecord* staticSurfaceRecord = geometryUniverse.TouchStaticSurface(staticSurfaceKey);
            captureTiming.staticCacheLookupMs += Sys_Milliseconds() - cacheLookupStartMs;
            ++sourceSurfaces;
            ++bucketRanges.buckets[0].surfaceCount;
            AddSmokeMaterialStats(materialStats, drawSurf->material, tri->numIndexes, surfaceClass, translucentSubtype);
            AddSmokeDynamicMaterialEvalStats(materialStats, drawSurf, tri->numIndexes);

            if (staticSurfaceRecord)
            {
                // Authored static cards can use runtime register variants (for
                // example colored wet splats). Keep the persistent geometry's
                // material identity current even when no vertices are appended.
                geometryUniverse.RefreshStaticSurfaceMaterial(staticSurfaceKey, materialId);
                ++captureTiming.staticCachedSurfaces;
                sourceVerts += tri->numVerts;
                sourceIndexes += tri->numIndexes;
                AddSmokeSurfaceClassStats(classStats, surfaceClass, tri->numVerts, tri->numIndexes);
                if (staticWalkedIds)
                {
                    staticWalkedIds->push_back(staticSurfaceKey);
                    if (staticWalkedTriangles)
                    {
                        staticWalkedTriangles->push_back(
                            static_cast<uint32_t>(tri->numIndexes / 3));
                    }
                }
                continue;
            }

            const RtSmokeGeometryAdmissionPlan staticAdmissionPlan =
                PlanSmokeStaticGeometryAdmission(
                    staticAdmissionBudget,
                    geometryUniverse,
                    tri->numVerts,
                    tri->numIndexes);
            if (!staticAdmissionPlan.Admitted())
            {
                RecordSmokeStaticGeometryAdmissionRejection(
                    skipStats, staticAdmissionPlan);
                continue;
            }

            const RtSmokeStaticSurfaceAppend staticAppend = geometryUniverse.BeginStaticSurfaceAppend(staticSurfaceKey, surfaceClassId, materialId, tri->numVerts, tri->numIndexes);
            const int appendStartMs = Sys_Milliseconds();
            const int emittedIndexes = AppendSmokeSurfaceGeometry(
                drawSurf,
                tri,
                surfaceClassId,
                materialId,
                RT_SMOKE_CLASS_COUNT,
                RT_SMOKE_TRIANGLE_CLASS_MASK,
                static_cast<uint32_t>(RtSmokeSurfaceClass::ParticleAlpha),
                RT_SMOKE_TRIANGLE_FORCE_GEOMETRIC_NORMAL,
                staticVertexCache,
                staticIndexCache,
                staticTriangleClassCache,
                staticTriangleMaterialCache,
                skipStats,
                attributeStats);
            const int appendMs = Sys_Milliseconds() - appendStartMs;
            captureTiming.staticAppendMs += appendMs;
            captureTiming.appendMs += appendMs;
            if (emittedIndexes <= 0)
            {
                continue;
            }

            ApplySmokeDetailDecalNormalOffset(
                drawSurf->material,
                staticSurfaceKey,
                true,
                staticVertexCache,
                staticIndexCache,
                static_cast<size_t>(staticAppend.vertexOffset),
                static_cast<size_t>(staticAppend.indexOffset));
            ++captureTiming.staticNewSurfaces;
            geometryUniverse.CompleteStaticSurfaceAppend(staticAppend, emittedIndexes);
            staticCacheChanged = true;
            sourceVerts += tri->numVerts;
            sourceIndexes += emittedIndexes;
            AddSmokeSurfaceClassStats(classStats, surfaceClass, tri->numVerts, emittedIndexes);
            if (staticWalkedIds)
            {
                staticWalkedIds->push_back(staticSurfaceKey);
                if (staticWalkedTriangles)
                {
                    staticWalkedTriangles->push_back(
                        static_cast<uint32_t>(emittedIndexes / 3));
                }
            }
        }
    }

    if (!skipDynamicCapture)
    {
        OPTICK_EVENT("PT Capture Dynamic Pass");
        for (int surfaceOffset = 0; surfaceOffset < viewDef->numDrawSurfs; ++surfaceOffset)
        {
            const int surfaceIndex = (anchorSurface + surfaceOffset) % viewDef->numDrawSurfs;
            const drawSurf_t* drawSurf = viewDef->drawSurfs[surfaceIndex];
            if (PtCpuProducerApplyGate::ShouldSkipDrawSurf(drawSurf))
            {
                continue;
            }
            const srfTriangles_t* tri = nullptr;
            const int validationStartMs = Sys_Milliseconds();
            if (!ValidateSmokeDrawSurface(viewDef, drawSurf, tri, &skipStats))
            {
                captureTiming.validationMs += Sys_Milliseconds() - validationStartMs;
                continue;
            }
            captureTiming.validationMs += Sys_Milliseconds() - validationStartMs;

            if (PathTraceParticleCompositeSurfaceRoute(viewDef, drawSurf, tri) == RtPathTraceParticleSurfaceRoute::CompositeOnly)
            {
                continue;
            }
            if (UnifiedPtDiagnosticRemovesAlphaClipSurface(drawSurf->material))
            {
                ++skipStats.alphaClipDiagnostic;
                continue;
            }

            const int classifyStartMs = Sys_Milliseconds();
            const RtSmokeSurfaceClass surfaceClass = ClassifySmokeSurface(viewDef, drawSurf, tri);
            captureTiming.dynamicPassClassifyMs += Sys_Milliseconds() - classifyStartMs;
            const RtSmokeTranslucentSubtype translucentSubtype = surfaceClass == RtSmokeSurfaceClass::ParticleAlpha ? ClassifySmokeTranslucentSubtype(drawSurf) : RtSmokeTranslucentSubtype::Unknown;
            const uint32_t surfaceClassId = SmokeSurfaceClassAndSubtypeId(surfaceClass, translucentSubtype);
            const uint32_t baseMaterialId = SmokeMaterialId(drawSurf->material);
            const uint32_t materialId = SmokeRuntimeMaterialTableIdForDrawSurf(drawSurf, baseMaterialId);
            const int entityIndex = (drawSurf->space && drawSurf->space->entityDef) ? drawSurf->space->entityDef->index : -1;
            const int bucketIndex = idMath::ClampInt(0, RT_SMOKE_CLASS_COUNT - 1, static_cast<int>(surfaceClassId & RT_SMOKE_TRIANGLE_CLASS_MASK));
            const bool isStaticWorld = surfaceClass == RtSmokeSurfaceClass::StaticWorld;
            if (isStaticWorld)
            {
                continue;
            }
            if (skipPromotedStaticSurfaceCapture && geometryUniverse.HasStaticSurface(BuildSmokeStaticSurfaceKey(drawSurf, tri)))
            {
                continue;
            }

            const RtSmokeGeometryAdmissionPlan admissionPlan =
                PlanSmokeDynamicGeometryAdmission(
                    dynamicAdmissionBudget,
                    dynamicAdmissionBytes,
                    dynamicAdmissionSurfaces,
                    tri->numVerts,
                    tri->numIndexes);
            if (!admissionPlan.Admitted())
            {
                RecordSmokeGeometryAdmissionRejection(
                    skipStats, admissionPlan);
                continue;
            }

            std::vector<PathTraceSmokeVertex>& bucketVertices = bucketVertexData[bucketIndex];
            std::vector<uint32_t>& bucketIndexes = bucketIndexData[bucketIndex];
            std::vector<uint32_t>& bucketClasses = bucketTriangleClassData[bucketIndex];
            std::vector<uint32_t>& bucketMaterials = bucketTriangleMaterialData[bucketIndex];
            std::vector<uint32_t>& bucketInstances = bucketTriangleInstanceData[bucketIndex];
            std::vector<uint32_t>& bucketIdentities = bucketTriangleIdentityData[bucketIndex];
            bool verticesFromFrameCache = false;
            SmokeDrawSurfaceVertices(drawSurf, tri, verticesFromFrameCache);
            const bool usesRtCpuSkinning = SmokeDrawSurfaceCpuSkinningJoints(drawSurf, tri, verticesFromFrameCache) != nullptr;
            const int bucketVertexStart = static_cast<int>(bucketVertices.size());
            const int bucketIndexStart = static_cast<int>(bucketIndexes.size());
            const int bucketTriangleStart = static_cast<int>(bucketClasses.size());
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
            if (usesRtCpuSkinning)
            {
                captureTiming.rtCpuSkinningAppendMs += appendMs;
                captureTiming.rtCpuSkinningAppendUs += appendUs;
            }
            if (emittedIndexes <= 0)
            {
                continue;
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
                continue;
            }
            dynamicAdmissionBytes = actualAdmissionPlan.totalBytes;
            dynamicAdmissionSurfaces =
                actualAdmissionPlan.totalSurfaces;
            // DECAL-DYN-1 (docs/decal_cards/07): trigger-spawned / translucent-class
            // detail decals are captured here per frame; without the lift they
            // coplanar-z-fight exactly like an un-offset static card. The key is
            // stable per held instance (entity + material), so the offset index
            // does not churn frame to frame.
            ApplySmokeDetailDecalNormalOffset(
                drawSurf->material,
                (static_cast<uint64>(baseMaterialId) << 32) ^
                    (static_cast<uint64>(static_cast<uint32_t>(entityIndex + 1)) * 2654435761ull),
                true,
                bucketVertices,
                bucketIndexes,
                static_cast<size_t>(bucketVertexStart),
                static_cast<size_t>(bucketIndexStart));
            const uint32_t dynamicInstanceId = static_cast<uint32_t>(Max(1, entityIndex + 1));
            const int emittedTriangles = emittedIndexes / 3;
            bucketInstances.insert(bucketInstances.end(), emittedTriangles, dynamicInstanceId);
            for (int localTriangleIndex = 0; localTriangleIndex < emittedTriangles; ++localTriangleIndex)
            {
                bucketIdentities.push_back(SmokeDynamicTriangleIdentitySeed(drawSurf, tri, baseMaterialId, static_cast<uint32_t>(localTriangleIndex)));
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
            }

            AddSmokeMaterialStats(materialStats, drawSurf->material, emittedIndexes, surfaceClass, translucentSubtype);
            AddSmokeDynamicMaterialEvalStatsForMaterialId(materialStats, drawSurf, emittedIndexes, materialId);
            if (surfaceClass == RtSmokeSurfaceClass::ParticleAlpha)
            {
                AddSmokeTranslucentDebugSample(materialStats, drawSurf, tri, surfaceIndex, translucentSubtype);
            }
            ++sourceSurfaces;
            sourceVerts += tri->numVerts;
            sourceIndexes += emittedIndexes;
            AddSmokeSurfaceClassStats(classStats, surfaceClass, tri->numVerts, emittedIndexes);
            AddSmokeDynamicGeometryStats(dynamicStats, surfaceClass, drawSurf, tri, emittedIndexes);
            ++bucketRanges.buckets[bucketIndex].surfaceCount;
            if (entityIndex >= 0)
            {
                RtSmokeCapturedDynamicSurfaceKey capturedKey;
                capturedKey.entityIndex = entityIndex;
                capturedKey.tri = tri;
                capturedKey.materialId = baseMaterialId;
                capturedDynamicSurfaces.push_back(capturedKey);
            }
        }
    }

    if (!skipDynamicCapture)
    {
        OPTICK_EVENT("PT Capture Nearby Dynamic Occluders");
        const int retentionRadius = idMath::ClampInt(0, 8192, r_pathTracingDynamicOccluderRadius.GetInteger());
        const int retainedSurfaceLimit =
            Max(0, r_pathTracingDynamicOccluderMaxSurfaces.GetInteger());
        int retainedSurfaces = 0;
        if (retentionRadius > 0 && retainedSurfaceLimit > 0 && viewDef->renderWorld)
        {
            const float retentionRadiusFloat = static_cast<float>(retentionRadius);
            const float retentionRadiusSqr = retentionRadiusFloat * retentionRadiusFloat;
            idRenderWorldLocal* renderWorld = viewDef->renderWorld;
            for (int entityIndex = 0; entityIndex < renderWorld->entityDefs.Num(); ++entityIndex)
            {
                idRenderEntityLocal* entityDef = renderWorld->entityDefs[entityIndex];
                const renderEntity_t* renderEntity = entityDef ? &entityDef->parms : nullptr;
                idRenderModel* model = renderEntity ? renderEntity->hModel : nullptr;
                if (!entityDef || !renderEntity || !model || model->IsStaticWorldModel())
                {
                    continue;
                }

                // This pass is deliberately limited to rigid entities backed by ordinary model surfaces.
                // Skinned/callback/continuous effects need identity and motion handling before retention is trustworthy.
                if (model->IsDynamicModel() != DM_STATIC)
                {
                    continue;
                }
                if (renderEntity->callback && renderEntity->customShader != nullptr && r_pathTracingSkipCallbackEntities.GetInteger() != 0)
                {
                    continue;
                }

                if (!SmokeBoundsWithinRadius(entityDef->globalReferenceBounds, viewDef->renderView.vieworg, retentionRadiusFloat) &&
                    (renderEntity->origin - viewDef->renderView.vieworg).LengthSqr() > retentionRadiusSqr)
                {
                    continue;
                }

                viewEntity_t retainedSpace = {};
                retainedSpace.entityDef = entityDef;
                retainedSpace.weaponDepthHack = renderEntity->weaponDepthHack;
                retainedSpace.modelDepthHack = renderEntity->modelDepthHack;
                memcpy(retainedSpace.modelMatrix, entityDef->modelMatrix, sizeof(retainedSpace.modelMatrix));
                R_MatrixMultiply(entityDef->modelMatrix, viewDef->worldSpace.modelViewMatrix, retainedSpace.modelViewMatrix);

                for (int surfaceIndex = 0; surfaceIndex < model->NumSurfaces(); ++surfaceIndex)
                {
                    if (retainedSurfaces >= retainedSurfaceLimit)
                    {
                        break;
                    }

                    const modelSurface_t* surface = model->Surface(surfaceIndex);
                    const srfTriangles_t* tri = surface ? surface->geometry : nullptr;
                    const idMaterial* shader = SmokeResolveEntitySurfaceMaterial(entityDef, surface);
                    if (!tri || !tri->verts || !tri->indexes || tri->numVerts < 3 || tri->numIndexes < 3 || !shader || !shader->IsDrawn())
                    {
                        continue;
                    }
                    if ((tri->numIndexes % 3) != 0)
                    {
                        ++skipStats.invalidIndexCount;
                        continue;
                    }

                    const uint32_t baseMaterialId = SmokeMaterialId(shader);
                    if (SmokeDynamicSurfaceAlreadyCaptured(capturedDynamicSurfaces, entityIndex, tri, baseMaterialId))
                    {
                        continue;
                    }

                    drawSurf_t retainedDrawSurf = {};
                    retainedDrawSurf.frontEndGeo = tri;
                    retainedDrawSurf.modelSurfaceIndex = surfaceIndex;
                    retainedDrawSurf.numIndexes = tri->numIndexes;
                    retainedDrawSurf.indexCache = tri->indexCache;
                    retainedDrawSurf.ambientCache = tri->ambientCache;
                    retainedDrawSurf.jointCache = 0;
                    retainedDrawSurf.space = &retainedSpace;
                    retainedDrawSurf.extraGLState = 0;
                    R_SetupDrawSurfShader(&retainedDrawSurf, shader, renderEntity);

                    const RtSmokeSurfaceClass surfaceClass = ClassifySmokeSurface(viewDef, &retainedDrawSurf, tri);
                    if (surfaceClass == RtSmokeSurfaceClass::StaticWorld || surfaceClass == RtSmokeSurfaceClass::SkinnedDeformed)
                    {
                        continue;
                    }
                    const RtSmokeTranslucentSubtype translucentSubtype = surfaceClass == RtSmokeSurfaceClass::ParticleAlpha ? ClassifySmokeTranslucentSubtype(&retainedDrawSurf) : RtSmokeTranslucentSubtype::Unknown;
                    const uint32_t surfaceClassId = SmokeSurfaceClassAndSubtypeId(surfaceClass, translucentSubtype);
                    const uint32_t materialId = SmokeRuntimeMaterialTableIdForDrawSurf(&retainedDrawSurf, baseMaterialId);
                    const int bucketIndex = idMath::ClampInt(0, RT_SMOKE_CLASS_COUNT - 1, static_cast<int>(surfaceClassId & RT_SMOKE_TRIANGLE_CLASS_MASK));

                    const RtSmokeGeometryAdmissionPlan admissionPlan =
                        PlanSmokeDynamicGeometryAdmission(
                            dynamicAdmissionBudget,
                            dynamicAdmissionBytes,
                            dynamicAdmissionSurfaces,
                            tri->numVerts,
                            tri->numIndexes);
                    if (!admissionPlan.Admitted())
                    {
                        RecordSmokeGeometryAdmissionRejection(
                            skipStats, admissionPlan);
                        continue;
                    }

                    std::vector<PathTraceSmokeVertex>& bucketVertices = bucketVertexData[bucketIndex];
                    std::vector<uint32_t>& bucketIndexes = bucketIndexData[bucketIndex];
                    std::vector<uint32_t>& bucketClasses = bucketTriangleClassData[bucketIndex];
                    std::vector<uint32_t>& bucketMaterials = bucketTriangleMaterialData[bucketIndex];
                    std::vector<uint32_t>& bucketInstances = bucketTriangleInstanceData[bucketIndex];
                    std::vector<uint32_t>& bucketIdentities = bucketTriangleIdentityData[bucketIndex];
                    const int bucketVertexStart =
                        static_cast<int>(bucketVertices.size());
                    const int bucketIndexStart =
                        static_cast<int>(bucketIndexes.size());
                    const int bucketTriangleStart =
                        static_cast<int>(bucketClasses.size());
                    const int appendStartMs = Sys_Milliseconds();
                    const int emittedIndexes = AppendSmokeSurfaceGeometry(
                        &retainedDrawSurf,
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
                    captureTiming.dynamicAppendMs += appendMs;
                    captureTiming.appendMs += appendMs;
                    if (emittedIndexes <= 0)
                    {
                        continue;
                    }
                    const int emittedVertices =
                        static_cast<int>(bucketVertices.size()) -
                            bucketVertexStart;
                    const RtSmokeGeometryAdmissionPlan
                        actualAdmissionPlan =
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
                        continue;
                    }
                    dynamicAdmissionBytes =
                        actualAdmissionPlan.totalBytes;
                    dynamicAdmissionSurfaces =
                        actualAdmissionPlan.totalSurfaces;
                    const uint32_t dynamicInstanceId = static_cast<uint32_t>(Max(1, entityIndex + 1));
                    const int emittedTriangles = emittedIndexes / 3;
                    bucketInstances.insert(bucketInstances.end(), emittedTriangles, dynamicInstanceId);
                    for (int localTriangleIndex = 0; localTriangleIndex < emittedTriangles; ++localTriangleIndex)
                    {
                        bucketIdentities.push_back(SmokeDynamicTriangleIdentitySeed(&retainedDrawSurf, tri, baseMaterialId, static_cast<uint32_t>(localTriangleIndex)));
                    }

                    AddSmokeMaterialStats(materialStats, shader, emittedIndexes, surfaceClass, translucentSubtype);
                    AddSmokeDynamicMaterialEvalStatsForMaterialId(materialStats, &retainedDrawSurf, emittedIndexes, materialId);
                    ++sourceSurfaces;
                    ++retainedSurfaces;
                    ++dynamicStats.retainedOccluderSurfaces;
                    dynamicStats.retainedOccluderIndexes += emittedIndexes;
                    sourceVerts += tri->numVerts;
                    sourceIndexes += emittedIndexes;
                    AddSmokeSurfaceClassStats(classStats, surfaceClass, tri->numVerts, emittedIndexes);
                    AddSmokeDynamicGeometryStats(dynamicStats, surfaceClass, &retainedDrawSurf, tri, emittedIndexes);
                    ++bucketRanges.buckets[bucketIndex].surfaceCount;
                    RtSmokeCapturedDynamicSurfaceKey capturedKey;
                    capturedKey.entityIndex = entityIndex;
                    capturedKey.tri = tri;
                    capturedKey.materialId = materialId;
                    capturedDynamicSurfaces.push_back(capturedKey);
                }
            }
        }
    }

    skipStats.geometryAdmittedBytes = dynamicAdmissionBytes;
    skipStats.geometryAdmittedSurfaces = dynamicAdmissionSurfaces;
    UpdateSmokeStaticGeometryAdmissionTotals(
        skipStats, geometryUniverse);

    const int bucketMergeStartMs = Sys_Milliseconds();
    {
        OPTICK_EVENT("PT Capture Bucket Merge");
        RtSmokeBucketRange& staticRange = bucketRanges.buckets[0];
        staticRange.vertexOffset = 0;
        staticRange.indexOffset = 0;
        staticRange.triangleOffset = 0;
        staticRange.vertexCount = static_cast<int>(staticVertexCache.size());
        staticRange.indexCount = static_cast<int>(staticIndexCache.size());
        staticRange.triangleCount = static_cast<int>(staticTriangleClassCache.size());

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

    if ((staticTriangleClassCache.empty() && triangleClassData.empty()) ||
        (staticTriangleMaterialCache.empty() && triangleMaterialData.empty()))
    {
        ++skipStats.emptyClassBuffer;
    }

    const bool hasStaticGeometry = !staticVertexCache.empty() && !staticIndexCache.empty() && !staticTriangleClassCache.empty() && !staticTriangleMaterialCache.empty();
    const bool hasDynamicGeometry = !vertexData.empty() && !indexData.empty() && !triangleClassData.empty() && !triangleMaterialData.empty();
    return sourceSurfaces > 0 && (hasStaticGeometry || hasDynamicGeometry);
}
