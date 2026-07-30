#include "precompiled.h"
#pragma hdrstop

// Doom draw-surface capture implementation for the RT smoke scene.
//
// The capture pass is intentionally conservative: it buckets surfaces into the
// few categories the prototype can render, records skip/classification reasons
// for diagnostics, and leaves unsupported dynamic material behavior hidden until
// a later material system can represent it safely.

#include "PathTraceCVars.h"
#include "PathTraceSceneCapture.h"
#include "PathTraceAcceleration.h"
#include "PathTraceDoomMaterialClassifier.h"
#include "PathTraceDynamicMaterialState.h"
#include "PathTraceGeometryLifecycle.h"
#include "PathTraceGuiSurfaces.h"
#include "PathTraceMaterialTextureDiscovery.h"
#include "PathTraceParticleCapture.h"
#include "PathTraceRigidIdentity.h"
#include "PathTraceSkinning.h"
#include "PathTraceSurfaceClassification.h"
#include "PathTraceTextureRegistry.h"
#include "../GLMatrix.h"
#include "../Model_local.h"
#include "../RenderCommon.h"

#include <algorithm>
#include <limits>

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

bool ValidateSmokeDrawSurface(const viewDef_t* viewDef, const drawSurf_t* drawSurf, const srfTriangles_t*& tri, RtSmokeSurfaceSkipStats* skipStats)
{
    tri = nullptr;
    if (!drawSurf)
    {
        if (skipStats)
        {
            ++skipStats->nullSurface;
        }
        return false;
    }

    if (!drawSurf->frontEndGeo)
    {
        if (skipStats)
        {
            ++skipStats->missingGeometry;
        }
        return false;
    }

    if (!drawSurf->material)
    {
        if (skipStats)
        {
            ++skipStats->nullMaterial;
        }
        return false;
    }

    if (!SmokeDrawSurfaceHasAnyActiveStage(drawSurf))
    {
        if (skipStats)
        {
            ++skipStats->conditionedOff;
        }
        return false;
    }

    const bool guiDrawSurface = IsSmokeGuiDrawSurface(drawSurf);
    if (guiDrawSurface && r_pathTracingAllowGuiSurfaces.GetInteger() == 0)
    {
        if (skipStats)
        {
            ++skipStats->guiSurface;
        }
        return false;
    }

    if (!drawSurf->space)
    {
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
    if (!guiDrawSurface && !worldSpace && !entityLive)
    {
        if (skipStats)
        {
            ++skipStats->nullModel;
        }
        return false;
    }
    const renderEntity_t* renderEntity = entityLive ? &entityDef->parms : nullptr;
    if (!guiDrawSurface && !worldSpace && !renderEntity->hModel)
    {
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
        if (skipStats)
        {
            ++skipStats->callbackEntity;
        }
        return false;
    }

    tri = drawSurf->frontEndGeo;
    if (!tri->verts || !tri->indexes || tri->numVerts < 3 || tri->numIndexes < 3)
    {
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
        if (skipStats)
        {
            ++skipStats->invalidIndexCount;
        }
        return false;
    }

    const bool hasAmbientCache = tri->ambientCache != 0;
    const bool hasIndexCache = tri->indexCache != 0;
    if ((hasAmbientCache && !vertexCache.CacheIsCurrent(tri->ambientCache)) ||
        (hasIndexCache && !vertexCache.CacheIsCurrent(tri->indexCache)))
    {
        if (skipStats)
        {
            ++skipStats->nonCurrentCache;
        }
        return false;
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
        const RtSmokeMaterialTextureInfo* info = FindSmokeMaterialTextureInfo(materialId);
        if (!info || !SmokeMaterialTextureInfoHasMaterialMetadata(*info))
        {
            RegisterSmokeMaterialTextureInfo(material);
            info = FindSmokeMaterialTextureInfo(materialId);
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

static bool SmokeSurfaceClassPrefersGeometricNormal(
    uint32_t surfaceClassId,
    uint32_t particleAlphaClassId)
{
    const uint32_t surfaceClass = surfaceClassId & RT_SMOKE_TRIANGLE_CLASS_MASK;
    if (surfaceClass != particleAlphaClassId)
    {
        return false;
    }

    const uint32_t translucentSubtype =
        (surfaceClassId & RT_SMOKE_TRANSLUCENT_SUBTYPE_MASK) >> RT_SMOKE_TRANSLUCENT_SUBTYPE_SHIFT;
    return
        translucentSubtype != SmokeTranslucentSubtypeId(RtSmokeTranslucentSubtype::ObjectGlass) &&
        translucentSubtype != SmokeTranslucentSubtypeId(RtSmokeTranslucentSubtype::PortalWindow);
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
        const bool preferGeometricNormal =
            invalidNormalTriangle ||
            SmokeSurfaceClassPrefersGeometricNormal(surfaceClassId, particleAlphaClassId);

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

RtSmokeDynamicEvalBuildResult BuildSmokeDynamicMaterialEvalSampleForId(const drawSurf_t* drawSurf, uint32_t materialId, RtSmokeDynamicMaterialEvalSample& surfaceSample)
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

    const int registerCount = material->GetNumRegisters();
    surfaceSample.valid = false;
    surfaceSample.id = materialId;
    surfaceSample.name = material->GetName();

    // Detail-decal channels: evaluate the first SL_DIFFUSE stage directly (the
    // generic selection below prefers the brightest stage and can pick a white
    // bump stage over the authored diffuse tint), and record a representative
    // world position for spectrum light association.
    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!stage || stage->lighting != SL_DIFFUSE)
        {
            continue;
        }
        surfaceSample.hasDiffuseStageColor = true;
        SmokeEvalRegister(regs, registerCount, stage->conditionRegister, 1.0f, surfaceSample.diffuseStageCondition);
        for (int component = 0; component < 4; ++component)
        {
            SmokeEvalRegister(regs, registerCount, stage->color.registers[component], 1.0f, surfaceSample.diffuseStageColor[component]);
        }
        break;
    }
    const srfTriangles_t* surfaceTri = drawSurf->frontEndGeo;
    if (surfaceTri)
    {
        idVec3 worldCenter;
        TransformSurfacePointToWorld(drawSurf, surfaceTri->bounds.GetCenter(), worldCenter);
        surfaceSample.hasSurfaceOrigin = true;
        surfaceSample.surfaceOrigin = worldCenter;
    }

    for (int stageIndex = 0; stageIndex < material->GetNumStages(); ++stageIndex)
    {
        const shaderStage_t* stage = material->GetStage(stageIndex);
        if (!SmokeStageUsesPerSurfaceMaterialState(material, stage))
        {
            continue;
        }

        float condition = 1.0f;
        SmokeEvalRegister(regs, registerCount, stage->conditionRegister, 1.0f, condition);
        const bool enabled = condition != 0.0f;
        ++surfaceSample.enabledStages;
        if (!enabled)
        {
            --surfaceSample.enabledStages;
            ++surfaceSample.disabledStages;
        }

        bool hasColor = false;
        float color[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        for (int component = 0; component < 4; ++component)
        {
            hasColor = SmokeEvalRegister(regs, registerCount, stage->color.registers[component], 1.0f, color[component]) || hasColor;
        }
        if (hasColor)
        {
            ++surfaceSample.colorStages;
        }
        if (hasColor && idMath::Fabs(color[3] - 1.0f) > 1.0e-4f)
        {
            ++surfaceSample.alphaStages;
        }

        float alphaTest = 0.0f;
        const bool hasAlphaTest = stage->hasAlphaTest &&
            SmokeEvalRegister(regs, registerCount, stage->alphaTestRegister, 0.0f, alphaTest);
        if (hasAlphaTest)
        {
            ++surfaceSample.alphaTestStages;
        }

        bool hasTexMatrix = false;
        float texMatrix[2][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
        if (stage->texture.hasMatrix)
        {
            hasTexMatrix = true;
            ++surfaceSample.texMatrixStages;
            for (int row = 0; row < 2; ++row)
            {
                for (int column = 0; column < 3; ++column)
                {
                    const float fallback = row == column ? 1.0f : 0.0f;
                    SmokeEvalRegister(regs, registerCount, stage->texture.matrix[row][column], fallback, texMatrix[row][column]);
                }
            }
        }
        if (stage->texture.dynamic != DI_STATIC || stage->texture.dynamicFrameCount > 0)
        {
            ++surfaceSample.dynamicImageStages;
        }
        if (stage->texture.cinematic != nullptr)
        {
            ++surfaceSample.cinematicStages;
        }
        if (stage->texture.texgen == TG_SCREEN ||
            stage->texture.texgen == TG_SCREEN2 ||
            SmokeStageIsRenderMap(stage))
        {
            ++surfaceSample.guiRenderTargetStages;
        }
        if (stage->newStage != nullptr)
        {
            ++surfaceSample.programStages;
        }

        const bool stageEmissive = SmokeDynamicEvalStageIsEmissiveLike(stage) ||
            (stageIndex == 0 && SmokeMaterialUsesOpaqueSwinglightCompatibility(material));
        if (surfaceSample.orderedStageCount < RT_SMOKE_DYNAMIC_ORDERED_STAGE_CAPACITY)
        {
            RtSmokeDynamicStageEval& orderedStage = surfaceSample.orderedStages[surfaceSample.orderedStageCount++];
            orderedStage.stageIndex = stageIndex;
            orderedStage.enabled = enabled;
            orderedStage.emissive = stageEmissive;
            orderedStage.hasAlphaTest = hasAlphaTest;
            orderedStage.hasTexMatrix = hasTexMatrix;
            orderedStage.condition = condition;
            orderedStage.alphaTest = alphaTest;
            for (int component = 0; component < 4; ++component)
            {
                orderedStage.color[component] = color[component];
            }
            for (int row = 0; row < 2; ++row)
            {
                for (int column = 0; column < 3; ++column)
                {
                    orderedStage.texMatrix[row][column] = texMatrix[row][column];
                }
            }
        }
        else
        {
            surfaceSample.orderedStageOverflow = true;
        }
        // The single-record bridge must preserve the stage that owns clipping.
        // Give authored alpha-test stages priority even while disabled so their
        // condition can turn clipping off for this entity this frame.
        const int stagePriority = (stage->hasAlphaTest ? 8 : 0) + (enabled ? 4 : 0) + (stageEmissive ? 2 : 0);
        RtSmokeDynamicMaterialEvalSample stageSample;
        stageSample.valid = true;
        stageSample.stageIndex = stageIndex;
        stageSample.stagePriority = stagePriority;
        stageSample.selectedStageEmissive = stageEmissive;
        stageSample.condition = condition;
        stageSample.alphaTest = alphaTest;
        stageSample.image = stage->texture.image;
        for (int component = 0; component < 4; ++component)
        {
            stageSample.color[component] = color[component];
        }
        if (hasTexMatrix)
        {
            for (int row = 0; row < 2; ++row)
            {
                for (int column = 0; column < 3; ++column)
                {
                    stageSample.texMatrix[row][column] = texMatrix[row][column];
                }
            }
        }
        if (SmokeDynamicEvalSampleShouldReplace(surfaceSample, stageSample))
        {
            SmokeDynamicEvalCopySelectedStage(surfaceSample, stageSample);
        }
    }

    return surfaceSample.valid ? RtSmokeDynamicEvalBuildResult::Built : RtSmokeDynamicEvalBuildResult::NoSelectedStage;
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
    const RtSmokeMaterialTextureInfo* info = FindSmokeMaterialTextureInfo(materialId);
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

uint32_t SmokeRuntimeMaterialVariantIdForDrawSurf(const drawSurf_t* drawSurf, uint32_t baseMaterialId)
{
    if (!drawSurf || !drawSurf->material || baseMaterialId == 0u)
    {
        return baseMaterialId;
    }
    if (SmokeResidencySkipsDynamicMaterialEval(baseMaterialId))
    {
        return baseMaterialId;
    }

    RtSmokeDynamicMaterialEvalSample surfaceSample;
    if (BuildSmokeDynamicMaterialEvalSampleForId(drawSurf, baseMaterialId, surfaceSample) != RtSmokeDynamicEvalBuildResult::Built)
    {
        return baseMaterialId;
    }

    const viewEntity_t* space = drawSurf->space;
    const idRenderEntityLocal* entity = space ? space->entityDef : nullptr;
    const renderEntity_t* renderEntity = entity ? &entity->parms : nullptr;
    if (!entity)
    {
        return baseMaterialId;
    }

    const idRenderModel* renderModel = renderEntity ? renderEntity->hModel : nullptr;
    const int resolvedModelSurfaceIndex = ResolvePathTraceRigidModelSurfaceIndex(
        renderModel,
        drawSurf->frontEndGeo,
        drawSurf->modelSurfaceIndex);
    if (resolvedModelSurfaceIndex >= 0)
    {
        return SmokeRuntimeMaterialVariantIdForEntitySurfaceKey(entity, resolvedModelSurfaceIndex, baseMaterialId);
    }

    uint32_t hash = 2166136261u;
    hash = SmokeRuntimeMaterialVariantHashValue(hash, 0x72747632u);
    hash = SmokeRuntimeMaterialVariantHashValue(hash, baseMaterialId);
    hash = SmokeRuntimeMaterialVariantHashValue(hash, static_cast<uint32_t>(entity ? entity->index : -1));
    hash = SmokeRuntimeMaterialVariantHashValue(hash, static_cast<uint32_t>(renderEntity ? renderEntity->entityNum : -1));
    hash = SmokeRuntimeMaterialVariantHashValue(hash, static_cast<uint32_t>(drawSurf->modelSurfaceIndex));
    if (drawSurf->modelSurfaceIndex < 0)
    {
        const uintptr_t triIdentity = reinterpret_cast<uintptr_t>(drawSurf->frontEndGeo);
        hash = SmokeRuntimeMaterialVariantHashValue(hash, static_cast<uint32_t>(triIdentity));
        hash = SmokeRuntimeMaterialVariantHashValue(hash, static_cast<uint32_t>(triIdentity >> 32));
    }

    uint32_t variantMaterialId = hash | 0x80000000u;
    if (variantMaterialId == 0u || variantMaterialId == baseMaterialId)
    {
        variantMaterialId = (hash ^ 0x5bd1e995u) | 0x80000000u;
    }
    return variantMaterialId != 0u ? variantMaterialId : baseMaterialId;
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

uint32_t SmokeRuntimeMaterialTableIdForDrawSurf(const drawSurf_t* drawSurf, uint32_t baseMaterialId)
{
    const uint32_t variantMaterialId = SmokeRuntimeMaterialVariantIdForDrawSurf(drawSurf, baseMaterialId);
    if (variantMaterialId == baseMaterialId)
    {
        return baseMaterialId;
    }
    RegisterSmokeMaterialTextureInfo(drawSurf ? drawSurf->material : nullptr);
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

bool CaptureDoomSurfacesForSmokeTest(const viewDef_t* viewDef, std::vector<PathTraceSmokeVertex>& vertexData, std::vector<uint32_t>& indexData, std::vector<uint32_t>& triangleClassData, std::vector<uint32_t>& triangleMaterialData, std::vector<uint32_t>* triangleInstanceData, std::vector<uint32_t>* triangleIdentityData, RtSmokeGeometryUniverse& geometryUniverse, bool& staticCacheChanged, idVec3& captureAnchor, int& sourceSurfaces, int& sourceVerts, int& sourceIndexes, int& anchorTriangle, RtSmokeSurfaceClassStats& classStats, RtSmokeSurfaceSkipStats& skipStats, RtSmokeDynamicGeometryStats& dynamicStats, RtSmokeAttributeStats& attributeStats, RtSmokeMaterialStats& materialStats, RtSmokeBucketRanges& bucketRanges, RtSmokeSceneCaptureTiming& captureTiming, std::vector<RtSmokeSkinnedSurfaceRecord>* skinnedSurfaceRecords, bool skipStaticWorldCapture, bool skipPromotedStaticSurfaceCapture, bool skipDynamicCapture)
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
        }
    }

    if (!skipDynamicCapture)
    {
        OPTICK_EVENT("PT Capture Dynamic Pass");
        for (int surfaceOffset = 0; surfaceOffset < viewDef->numDrawSurfs; ++surfaceOffset)
        {
            const int surfaceIndex = (anchorSurface + surfaceOffset) % viewDef->numDrawSurfs;
            const drawSurf_t* drawSurf = viewDef->drawSurfs[surfaceIndex];
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
