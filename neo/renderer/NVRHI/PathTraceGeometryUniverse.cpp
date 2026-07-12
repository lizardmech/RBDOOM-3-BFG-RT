#include "precompiled.h"
#pragma hdrstop

#include "PathTraceGeometryUniverse.h"
#include "PathTraceInstanceUniverse.h"
#include "PathTraceGeometryLifecycle.h"
#include "PathTraceCVars.h"
#include "PathTraceAcceleration.h"
#include "PathTraceAccelerationPlan.h"
#include "PathTraceDoomMaterialClassifier.h"
#include "PathTraceDynamicMaterialState.h"
#include "PathTraceRigidIdentity.h"
#include "PathTraceSceneCapture.h"
#include "PathTraceSceneUniverse.h"
#include "PathTraceSurfaceClassification.h"
#include "../RenderCommon.h"
#include "../RenderWorld_local.h"

#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <utility>
#include <nvrhi/utils.h>

namespace {

constexpr uint64 RT_SMOKE_RIGID_BLAS_RETIRE_FRAMES = 3;

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

template< typename T >
RtSmokePlanDataSpan MakeRigidRoutePlanDataSpan(const std::vector<T>& data)
{
    RtSmokePlanDataSpan span;
    span.data = data.empty() ? nullptr : data.data();
    span.elementSize = sizeof(T);
    span.elementCount = data.size();
    return span;
}

bool IsSmokeGeometryElementRangeValid(const RtSmokeGeometryElementRange& range, int elementCount)
{
    if (range.offset < 0 || range.count < 0 || elementCount < 0)
    {
        return false;
    }

    const int64_t begin = static_cast<int64_t>(range.offset);
    const int64_t count = static_cast<int64_t>(range.count);
    const int64_t end = begin + count;
    return begin <= static_cast<int64_t>(elementCount) && end <= static_cast<int64_t>(elementCount);
}

bool IsSmokeGeometryRangeValid(const RtSmokeGeometryRangeRecord& range, int vertexCount, int indexCount, int triangleCount, int materialTriangleCount)
{
    return
        IsSmokeGeometryElementRangeValid(range.vertices, vertexCount) &&
        IsSmokeGeometryElementRangeValid(range.indexes, indexCount) &&
        IsSmokeGeometryElementRangeValid(range.triangles, triangleCount) &&
        IsSmokeGeometryElementRangeValid(range.triangles, materialTriangleCount) &&
        static_cast<int64_t>(range.indexes.count) == static_cast<int64_t>(range.triangles.count) * 3;
}

bool SmokeGeometryRangesMatchCounts(const RtSmokeGeometryRangeRecord& a, const RtSmokeGeometryRangeRecord& b)
{
    return
        a.vertices.count == b.vertices.count &&
        a.indexes.count == b.indexes.count &&
        a.triangles.count == b.triangles.count;
}

void AccumulateSmokeGeometryElementRange(const RtSmokeGeometryElementRange& range, int& offset, int& count)
{
    if (range.offset < 0 || range.count <= 0)
    {
        return;
    }

    if (offset < 0)
    {
        offset = range.offset;
        count = range.count;
        return;
    }

    const int begin = Min(offset, range.offset);
    const int end = Max(offset + count, range.offset + range.count);
    offset = begin;
    count = end - begin;
}

int ResolveRigidResidencyCurrentArea(const viewDef_t* viewDef)
{
    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    if (!renderWorld)
    {
        return -1;
    }

    int area = viewDef->areaNum;
    if (area < 0)
    {
        area = renderWorld->PointInArea(viewDef->initialViewAreaOrigin);
    }
    if (area < 0)
    {
        area = renderWorld->PointInArea(viewDef->renderView.vieworg);
    }
    return area;
}

std::vector<int> ResolveRigidResidencySeedAreas(const viewDef_t* viewDef)
{
    std::vector<int> seedAreas;
    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    if (!renderWorld)
    {
        return seedAreas;
    }

    const int areaCount = renderWorld->NumAreas();
    auto addSeedArea = [&](const int area) {
        if (area < 0 || area >= areaCount)
        {
            return;
        }
        if (std::find(seedAreas.begin(), seedAreas.end(), area) == seedAreas.end())
        {
            seedAreas.push_back(area);
        }
    };

    addSeedArea(viewDef->areaNum);
    addSeedArea(renderWorld->PointInArea(viewDef->initialViewAreaOrigin));
    addSeedArea(renderWorld->PointInArea(viewDef->renderView.vieworg));

    const idVec3& viewOrigin = viewDef->renderView.vieworg;
    const float probeDistance = 8.0f;
    addSeedArea(renderWorld->PointInArea(viewOrigin + viewDef->renderView.viewaxis[0] * probeDistance));
    addSeedArea(renderWorld->PointInArea(viewOrigin - viewDef->renderView.viewaxis[0] * probeDistance));
    addSeedArea(renderWorld->PointInArea(viewOrigin + viewDef->renderView.viewaxis[1] * probeDistance));
    addSeedArea(renderWorld->PointInArea(viewOrigin - viewDef->renderView.viewaxis[1] * probeDistance));
    addSeedArea(renderWorld->PointInArea(viewOrigin + viewDef->renderView.viewaxis[2] * probeDistance));
    addSeedArea(renderWorld->PointInArea(viewOrigin - viewDef->renderView.viewaxis[2] * probeDistance));
    return seedAreas;
}

std::vector<bool> BuildRigidResidencySelectedAreas(const viewDef_t* viewDef, int portalSteps, int* portalEdges, int* blockedPortalEdges, int* currentAreaOut)
{
    std::vector<bool> selectedAreas;
    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    const std::vector<int> seedAreas = ResolveRigidResidencySeedAreas(viewDef);
    const int currentArea = !seedAreas.empty() ? seedAreas[0] : ResolveRigidResidencyCurrentArea(viewDef);
    if (currentAreaOut)
    {
        *currentAreaOut = currentArea;
    }
    if (!renderWorld || seedAreas.empty())
    {
        return selectedAreas;
    }

    const int areaCount = renderWorld->NumAreas();

    portalSteps = idMath::ClampInt(0, 8, portalSteps);
    selectedAreas.assign(areaCount, false);
    if (r_pathTracingPortalBruteforceFullMap.GetInteger() != 0)
    {
        std::fill(selectedAreas.begin(), selectedAreas.end(), true);
        return selectedAreas;
    }

    std::vector<int> selectedDepth(areaCount, -1);
    std::vector<int> queue;
    queue.reserve(areaCount);
    for (int seedArea : seedAreas)
    {
        selectedAreas[seedArea] = true;
        selectedDepth[seedArea] = 0;
        queue.push_back(seedArea);
    }

    for (size_t queueIndex = 0; queueIndex < queue.size(); ++queueIndex)
    {
        const int area = queue[queueIndex];
        const int depth = selectedDepth[area];
        if (depth >= portalSteps)
        {
            continue;
        }

        const int portalCount = renderWorld->NumPortalsInArea(area);
        for (int portalIndex = 0; portalIndex < portalCount; ++portalIndex)
        {
            const exitPortal_t portal = renderWorld->GetPortal(area, portalIndex);
            if ((portal.blockingBits & PS_BLOCK_VIEW) != 0)
            {
                if (blockedPortalEdges)
                {
                    ++(*blockedPortalEdges);
                }
            }

            int nextArea = -1;
            if (portal.areas[0] == area)
            {
                nextArea = portal.areas[1];
            }
            else if (portal.areas[1] == area)
            {
                nextArea = portal.areas[0];
            }
            if (nextArea < 0 || nextArea >= areaCount)
            {
                continue;
            }

            if (portalEdges)
            {
                ++(*portalEdges);
            }
            if (!selectedAreas[nextArea])
            {
                selectedAreas[nextArea] = true;
                selectedDepth[nextArea] = depth + 1;
                queue.push_back(nextArea);
            }
        }
    }
    return selectedAreas;
}

bool RigidResidencyCanPromoteEmissiveCard(const idRenderEntityLocal* entity, const idMaterial* material)
{
    if (r_pathTracingRigidRouteEmissiveCards.GetInteger() == 0 || !entity || !material)
    {
        return false;
    }

    return SmokeMaterialCanPromoteRigidEmissiveCard(material);
}

bool RigidRouteEntityKeyKnownAlive(const PtRenderDefKey& key)
{
    return
        key.world != nullptr &&
        key.index >= 0 &&
        key.generation != 0 &&
        PtGeometryLifecycle::IsEntityKeyAlive(key);
}

bool RigidRouteSourceFlagsDeforming(uint32_t sourceFlags)
{
    const uint32_t deformingFlags =
        RT_PT_INSTANCE_SOURCE_SKINNED_OR_DEFORMING |
        RT_PT_INSTANCE_SOURCE_PARTICLE_OR_TRANSIENT |
        RT_PT_INSTANCE_SOURCE_CALLBACK_OR_GENERATED;
    return (sourceFlags & deformingFlags) != 0;
}

bool RigidResidencyEntityMovedWithinGrace(const idRenderEntityLocal* entity)
{
    if (!entity || entity->lastModifiedFrameNum <= 0)
    {
        return false;
    }

    const int graceFrames = idMath::ClampInt(0, 120, r_pathTracingResidencyMovingGraceFrames.GetInteger());
    const int framesSinceMove = tr.frameCount - entity->lastModifiedFrameNum;
    return framesSinceMove < 0 || framesSinceMove <= graceFrames;
}

bool RigidResidencyCanTrackEntity(const viewDef_t* viewDef, const idRenderEntityLocal* entity)
{
    const renderEntity_t* renderEntity = entity ? &entity->parms : nullptr;
    const idRenderModel* model = renderEntity ? renderEntity->hModel : nullptr;
    if (!entity || !renderEntity || !model)
    {
        return false;
    }
    const PtRenderDefKey entityKey = PtGeometryLifecycle::MakeEntityKey(entity);
    if (entityKey.generation != 0 && !RigidRouteEntityKeyKnownAlive(entityKey))
    {
        return false;
    }
    if (!r_skipSuppress.GetBool() && viewDef)
    {
        if (renderEntity->suppressSurfaceInViewID &&
            renderEntity->suppressSurfaceInViewID == viewDef->renderView.viewID)
        {
            return false;
        }
        if (renderEntity->allowSurfaceInViewID &&
            renderEntity->allowSurfaceInViewID != viewDef->renderView.viewID)
        {
            return false;
        }
    }
    if (model->IsStaticWorldModel() ||
        !model->ModelHasDrawingSurfaces() ||
        model->IsDefaultModel() ||
        model->IsDynamicModel() != DM_STATIC ||
        RigidResidencyEntityMovedWithinGrace(entity) ||
        renderEntity->callback != nullptr ||
        renderEntity->forceUpdate != 0 ||
        renderEntity->joints != nullptr ||
        renderEntity->numJoints > 0 ||
        entity->dynamicModel != nullptr ||
        entity->cachedDynamicModel != nullptr ||
        renderEntity->modelDepthHack != 0.0f)
    {
        return false;
    }
    return true;
}

bool RigidResidencyCanTrackSurface(const idRenderEntityLocal* entity, const srfTriangles_t* tri, const idMaterial* material)
{
    if (!entity || !tri || !tri->verts || !tri->indexes || tri->numVerts <= 0 || tri->numIndexes <= 0 || (tri->numIndexes % 3) != 0 || !material)
    {
        return false;
    }
    if (tri->staticModelWithJoints != nullptr)
    {
        return false;
    }
    const deform_t deform = material->Deform();
    if (deform == DFRM_SPRITE ||
        deform == DFRM_TUBE ||
        deform == DFRM_FLARE ||
        deform == DFRM_PARTICLE ||
        deform == DFRM_PARTICLE2)
    {
        return false;
    }
    if (deform != DFRM_NONE)
    {
        return false;
    }
    if (material->Coverage() == MC_TRANSLUCENT || material->GetSort() >= SS_MEDIUM)
    {
        const RtSmokeTranslucentClassifierInfo classifier = BuildSmokeTranslucentClassifierInfo(material);
        if (classifier.hasScreenTexgen ||
            classifier.hasAddDefault0200Texture ||
            classifier.nameLooksGui ||
            classifier.nameLooksParticle ||
            classifier.sortIsPostProcess ||
            classifier.sortIsGuiOrSubview)
        {
            return false;
        }
        return true;
    }
    return true;
}

int CountRigidResidencySelectedAreas(const std::vector<bool>& selectedAreas)
{
    int count = 0;
    for (bool selected : selectedAreas)
    {
        if (selected)
        {
            ++count;
        }
    }
    return count;
}

bool RigidResidencyAreaSelected(int area, const std::vector<bool>& selectedAreas)
{
    return area >= 0 &&
        area < static_cast<int>(selectedAreas.size()) &&
        selectedAreas[area];
}

bool RigidResidencyWithinDistance(
    const RtPathTraceRigidRouteInstanceObservation& instance,
    const idVec3* viewOrigin,
    float maxDistance)
{
    if (!viewOrigin || maxDistance <= 0.0f)
    {
        return true;
    }

    const float dx = instance.objectToWorld[12] - viewOrigin->x;
    const float dy = instance.objectToWorld[13] - viewOrigin->y;
    const float dz = instance.objectToWorld[14] - viewOrigin->z;
    return dx * dx + dy * dy + dz * dz <= maxDistance * maxDistance;
}

bool RigidRouteInstancePriorityLess(
    const RtPathTraceRigidRouteInstanceObservation& a,
    const RtPathTraceRigidRouteInstanceObservation& b)
{
    if (a.seenThisFrame != b.seenThisFrame)
    {
        return a.seenThisFrame;
    }
    if (a.isStable != b.isStable)
    {
        return a.isStable;
    }
    if (a.transformContinuous != b.transformContinuous)
    {
        return a.transformContinuous;
    }
    return false;
}

bool RigidRouteEntityKeyEqual(
    const RtPathTraceRigidRouteInstanceObservation& a,
    const RtPathTraceRigidRouteInstanceObservation& b)
{
    if (a.renderDefKey.world != nullptr &&
        b.renderDefKey.world != nullptr &&
        a.renderDefKey.index >= 0 &&
        b.renderDefKey.index >= 0)
    {
        return a.renderDefKey.world == b.renderDefKey.world &&
            a.renderDefKey.index == b.renderDefKey.index &&
            a.renderDefKey.generation == b.renderDefKey.generation;
    }

    if (a.entityIndex >= 0 &&
        b.entityIndex >= 0 &&
        a.renderEntityNum >= 0 &&
        b.renderEntityNum >= 0)
    {
        return a.entityIndex == b.entityIndex &&
            a.renderEntityNum == b.renderEntityNum;
    }

    return a.instanceId == b.instanceId;
}

RtPathTraceRigidRouteInstanceObservation MakeRigidRouteInstanceObservation(const RtPathTraceInstanceObservation& instance)
{
    RtPathTraceRigidRouteInstanceObservation routeInstance;
    routeInstance.instanceId = instance.instanceId;
    routeInstance.meshHash = instance.meshHash;
    routeInstance.entityIndex = instance.entityIndex;
    routeInstance.renderEntityNum = instance.renderEntityNum;
    routeInstance.drawSurfIndex = instance.drawSurfIndex;
    routeInstance.modelSurfaceIndex = instance.modelSurfaceIndex;
    routeInstance.jointIndex = instance.jointIndex;
    routeInstance.currentArea = instance.currentArea;
    routeInstance.renderDefKey = instance.renderDefKey;
    routeInstance.modelEpoch = instance.modelEpoch;
    routeInstance.materialOverrideId = instance.materialOverrideId;
    routeInstance.triangleClassAndFlags = instance.triangleClassAndFlags != 0u
        ? instance.triangleClassAndFlags
        : instance.surfaceClassId;
    routeInstance.sourceFlags = instance.sourceFlags;
    routeInstance.wasMovingWhenLastSeen =
        instance.entity &&
        RigidResidencyEntityMovedWithinGrace(instance.entity);
    routeInstance.isSkinnedOrDeforming = RigidRouteSourceFlagsDeforming(instance.sourceFlags);
    routeInstance.hasPreviousObjectToWorld = instance.hasPreviousObjectToWorld;
    routeInstance.transformContinuous = instance.transformContinuous;
    for (int elementIndex = 0; elementIndex < 16; ++elementIndex)
    {
        routeInstance.objectToWorld[elementIndex] = instance.objectToWorld[elementIndex];
        routeInstance.previousObjectToWorld[elementIndex] = instance.previousObjectToWorld[elementIndex];
    }
    routeInstance.materialName = instance.materialName;
    routeInstance.modelName = instance.modelName;
    return routeInstance;
}

void CopyRigidRouteTransformRows(float dst[12], const float objectToWorld[16])
{
    dst[0] = objectToWorld[0];
    dst[1] = objectToWorld[4];
    dst[2] = objectToWorld[8];
    dst[3] = objectToWorld[12];
    dst[4] = objectToWorld[1];
    dst[5] = objectToWorld[5];
    dst[6] = objectToWorld[9];
    dst[7] = objectToWorld[13];
    dst[8] = objectToWorld[2];
    dst[9] = objectToWorld[6];
    dst[10] = objectToWorld[10];
    dst[11] = objectToWorld[14];
}

void TransformRigidResidencyBoundsPoint(const float objectToWorld[16], const idVec3& localPoint, idVec3& worldPoint)
{
    R_LocalPointToGlobal(objectToWorld, localPoint, worldPoint);
}

bool IsRigidResidencyBoundsPointFinite(const idVec3& point)
{
    return
        point.x == point.x &&
        point.y == point.y &&
        point.z == point.z &&
        idMath::Fabs(point.x) < 100000.0f &&
        idMath::Fabs(point.y) < 100000.0f &&
        idMath::Fabs(point.z) < 100000.0f;
}

bool ValidateRigidResidencyBoundsBox(const RtPathTraceRigidResidencyBoundsBox& box)
{
    idVec3 mins = box.corners[0];
    idVec3 maxs = box.corners[0];

    for (int cornerIndex = 0; cornerIndex < 8; ++cornerIndex)
    {
        const idVec3& corner = box.corners[cornerIndex];
        if (!IsRigidResidencyBoundsPointFinite(corner))
        {
            return false;
        }
        mins.x = Min(mins.x, corner.x);
        mins.y = Min(mins.y, corner.y);
        mins.z = Min(mins.z, corner.z);
        maxs.x = Max(maxs.x, corner.x);
        maxs.y = Max(maxs.y, corner.y);
        maxs.z = Max(maxs.z, corner.z);
    }

    const idVec3 extent = maxs - mins;
    return extent.x <= 32768.0f && extent.y <= 32768.0f && extent.z <= 32768.0f;
}

idVec4 RigidResidencyBoundsColor(bool seenThisFrame, bool retainedOffscreen, bool aboutToAgeOut, bool routeReady, bool missingBlas)
{
    if (!routeReady && missingBlas)
    {
        return idVec4(1.0f, 0.82f, 0.0f, 1.0f);
    }
    if (aboutToAgeOut)
    {
        return idVec4(1.0f, 0.45f, 0.0f, 1.0f);
    }
    if (retainedOffscreen && routeReady)
    {
        return idVec4(0.0f, 1.0f, 1.0f, 1.0f);
    }
    if (seenThisFrame && routeReady)
    {
        return idVec4(0.1f, 1.0f, 0.25f, 1.0f);
    }
    return idVec4(1.0f, 0.0f, 0.9f, 1.0f);
}

idVec4 StaticSurfaceBoundsColor(bool seenThisFrame)
{
    return seenThisFrame ? idVec4(0.45f, 0.45f, 0.45f, 1.0f) : idVec4(0.0f, 0.75f, 1.0f, 1.0f);
}

bool SmokeGeometryRangeHasOffsets(const RtSmokeGeometryRangeRecord& range)
{
    return
        range.vertices.offset >= 0 &&
        range.indexes.offset >= 0 &&
        range.triangles.offset >= 0;
}

bool SmokeGeometryRecordHasDuplicateKey(const std::vector<RtSmokePersistentStaticSurfaceRecord>& records, size_t recordIndex)
{
    if (recordIndex >= records.size() || !records[recordIndex].valid)
    {
        return false;
    }

    const uint64 key = records[recordIndex].key;
    for (size_t otherIndex = 0; otherIndex < records.size(); ++otherIndex)
    {
        if (otherIndex != recordIndex && records[otherIndex].valid && records[otherIndex].key == key)
        {
            return true;
        }
    }
    return false;
}

void PrintSmokeGeometryRange(const char* label, const RtSmokeGeometryRangeRecord& range)
{
    common->Printf("%s v=%d/%d i=%d/%d t=%d/%d",
        label ? label : "range",
        range.vertices.offset,
        range.vertices.count,
        range.indexes.offset,
        range.indexes.count,
        range.triangles.offset,
        range.triangles.count);
}

uint32_t BuildRigidMeshCandidateRejectFlags(const RtPathTraceRigidMeshCandidateObservation& observation)
{
    uint32_t rejectFlags = 0;
    const RtPathTraceResidencyClass residencyClass = RtPathTraceResidencyClassForSourceFlags(observation.sourceFlags);
    if (residencyClass != RtPathTraceResidencyClass::DurableRigid &&
        (observation.sourceFlags & RT_PT_INSTANCE_SOURCE_RIGID) == 0)
    {
        rejectFlags |= RT_PT_RIGID_MESH_REJECT_NOT_RIGID;
    }
    if (observation.numVerts <= 0 || observation.numIndexes <= 0 || (observation.numIndexes % 3) != 0 || observation.meshHash == 0)
    {
        rejectFlags |= RT_PT_RIGID_MESH_REJECT_INVALID_GEOMETRY;
    }
    if (observation.tri == nullptr)
    {
        rejectFlags |= RT_PT_RIGID_MESH_REJECT_INVALID_GEOMETRY;
    }
    if (observation.materialId == 0 || observation.materialName.IsEmpty() || observation.materialName.Icmp("<none>") == 0)
    {
        rejectFlags |= RT_PT_RIGID_MESH_REJECT_MISSING_MATERIAL;
    }
    if (!observation.localSpaceValid)
    {
        rejectFlags |= RT_PT_RIGID_MESH_REJECT_NO_LOCAL_SPACE;
    }
    if ((observation.sourceFlags & RT_PT_INSTANCE_SOURCE_SKINNED_OR_DEFORMING) != 0)
    {
        rejectFlags |= RT_PT_RIGID_MESH_REJECT_SKINNED_OR_DEFORMING;
    }
    if ((observation.sourceFlags & RT_PT_INSTANCE_SOURCE_PARTICLE_OR_TRANSIENT) != 0)
    {
        rejectFlags |= RT_PT_RIGID_MESH_REJECT_PARTICLE_OR_TRANSIENT;
    }
    if ((observation.sourceFlags & RT_PT_INSTANCE_SOURCE_GUI) != 0)
    {
        rejectFlags |= RT_PT_RIGID_MESH_REJECT_GUI;
    }
    if ((observation.sourceFlags & RT_PT_INSTANCE_SOURCE_CALLBACK_OR_GENERATED) != 0)
    {
        rejectFlags |= RT_PT_RIGID_MESH_REJECT_CALLBACK_OR_GENERATED;
    }
    if ((observation.sourceFlags & RT_PT_INSTANCE_SOURCE_STATIC_WORLD) != 0)
    {
        rejectFlags |= RT_PT_RIGID_MESH_REJECT_STATIC_WORLD;
    }
    if ((observation.sourceFlags & (RT_PT_INSTANCE_SOURCE_STATIC_UNIVERSE_MATCH | RT_PT_INSTANCE_SOURCE_STATIC_CACHE_MATCH)) != 0)
    {
        rejectFlags |= RT_PT_RIGID_MESH_REJECT_STATIC_CACHE_MATCH;
    }
    return rejectFlags;
}

void AccumulateRigidMeshCandidateRejectStats(RtPathTraceRigidMeshCandidateStats& stats, uint32_t rejectFlags)
{
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_NOT_RIGID) != 0)
    {
        ++stats.rejectNotRigid;
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_INVALID_GEOMETRY) != 0)
    {
        ++stats.rejectInvalidGeometry;
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_MISSING_MATERIAL) != 0)
    {
        ++stats.rejectMissingMaterial;
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_NO_LOCAL_SPACE) != 0)
    {
        ++stats.rejectNoLocalSpace;
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_SKINNED_OR_DEFORMING) != 0)
    {
        ++stats.rejectSkinnedOrDeforming;
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_PARTICLE_OR_TRANSIENT) != 0)
    {
        ++stats.rejectParticleOrTransient;
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_GUI) != 0)
    {
        ++stats.rejectGui;
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_CALLBACK_OR_GENERATED) != 0)
    {
        ++stats.rejectCallbackOrGenerated;
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_STATIC_WORLD) != 0)
    {
        ++stats.rejectStaticWorld;
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_STATIC_CACHE_MATCH) != 0)
    {
        ++stats.rejectStaticCacheMatch;
    }
}

const char* RigidMeshCandidateRejectSummary(uint32_t rejectFlags)
{
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_NOT_RIGID) != 0)
    {
        return "not-rigid";
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_INVALID_GEOMETRY) != 0)
    {
        return "invalid-geometry";
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_MISSING_MATERIAL) != 0)
    {
        return "missing-material";
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_NO_LOCAL_SPACE) != 0)
    {
        return "no-local-space";
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_SKINNED_OR_DEFORMING) != 0)
    {
        return "skinned-or-deforming";
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_PARTICLE_OR_TRANSIENT) != 0)
    {
        return "particle-or-transient";
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_GUI) != 0)
    {
        return "gui";
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_CALLBACK_OR_GENERATED) != 0)
    {
        return "callback-or-generated";
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_STATIC_WORLD) != 0)
    {
        return "static-world";
    }
    if ((rejectFlags & RT_PT_RIGID_MESH_REJECT_STATIC_CACHE_MATCH) != 0)
    {
        return "static-cache-match";
    }
    return "eligible";
}

size_t RigidSmokeBufferRequiredBytes(size_t byteSize, uint32_t structStride)
{
    return byteSize > structStride ? byteSize : structStride;
}

bool RigidSmokeBufferHasCapacity(nvrhi::BufferHandle buffer, size_t byteSize, uint32_t structStride)
{
    return buffer && buffer->getDesc().byteSize >= RigidSmokeBufferRequiredBytes(byteSize, structStride);
}

nvrhi::BufferHandle CreateRigidSmokeBuffer(nvrhi::IDevice* device, const char* debugName, size_t byteSize, uint32_t structStride, bool vertexBuffer, bool indexBuffer)
{
    if (!device)
    {
        return nullptr;
    }

    nvrhi::BufferDesc desc;
    desc.byteSize = RigidSmokeBufferRequiredBytes(byteSize, structStride);
    desc.debugName = debugName;
    desc.structStride = structStride;
    desc.isVertexBuffer = vertexBuffer;
    desc.isIndexBuffer = indexBuffer;
    desc.isAccelStructBuildInput = true;
    desc.initialState = nvrhi::ResourceStates::Common;
    desc.keepInitialState = true;
    return device->createBuffer(desc);
}

uint64 BuildRigidGpuUploadSignature(const RtSmokeGeometryUniverse::RigidMeshCandidateRecord& record)
{
    uint64 hash = 14695981039346656037ull;
    hash = HashSmokeBytes(hash, &record.meshHash, sizeof(record.meshHash));
    hash = HashSmokeBytes(hash, &record.vertexBufferIdentity, sizeof(record.vertexBufferIdentity));
    hash = HashSmokeBytes(hash, &record.indexBufferIdentity, sizeof(record.indexBufferIdentity));
    hash = HashSmokeBytes(hash, &record.materialId, sizeof(record.materialId));
    hash = HashSmokeBytes(hash, record.normalTexMatrix, sizeof(record.normalTexMatrix));
    hash = HashSmokeBytes(hash, &record.sourceRange.vertices.count, sizeof(record.sourceRange.vertices.count));
    hash = HashSmokeBytes(hash, &record.sourceRange.indexes.count, sizeof(record.sourceRange.indexes.count));
    return hash;
}

uint32_t ValidateRigidBlasInputRecord(const RtSmokeGeometryUniverse::RigidMeshCandidateRecord& record)
{
    const uint32_t expectedVertexFormat = static_cast<uint32_t>(RtSmokeGeometryBufferFormat::LegacySmokeVertex);
    uint32_t invalidFlags = 0;
    const bool hasCachedMesh =
        static_cast<int>(record.cachedLocalVertices.size()) == record.sourceRange.vertices.count &&
        static_cast<int>(record.cachedLocalIndexes.size()) == record.sourceRange.indexes.count;
    if (record.tri == nullptr && !hasCachedMesh)
    {
        invalidFlags |= RT_PT_RIGID_BLAS_INPUT_INVALID_NULL_TRI;
    }
    if (record.sourceRange.vertices.count <= 0)
    {
        invalidFlags |= RT_PT_RIGID_BLAS_INPUT_INVALID_VERTEX_COUNT;
    }
    if (record.sourceRange.indexes.count <= 0 || (record.sourceRange.indexes.count % 3) != 0)
    {
        invalidFlags |= RT_PT_RIGID_BLAS_INPUT_INVALID_INDEX_COUNT;
    }
    if (record.sourceRange.triangles.count <= 0 || record.sourceRange.triangles.count * 3 != record.sourceRange.indexes.count)
    {
        invalidFlags |= RT_PT_RIGID_BLAS_INPUT_INVALID_TRIANGLE_COUNT;
    }
    if (record.vertexFormat != expectedVertexFormat)
    {
        invalidFlags |= RT_PT_RIGID_BLAS_INPUT_INVALID_VERTEX_FORMAT;
    }
    if (record.vertexBufferIdentity == 0 || record.indexBufferIdentity == 0)
    {
        invalidFlags |= RT_PT_RIGID_BLAS_INPUT_INVALID_MISSING_SOURCE_IDENTITY;
    }
    if (record.materialId == 0)
    {
        invalidFlags |= RT_PT_RIGID_BLAS_INPUT_INVALID_MATERIAL;
    }
    if (record.tri)
    {
        if (!record.tri->verts || record.tri->numVerts < record.sourceRange.vertices.count)
        {
            invalidFlags |= RT_PT_RIGID_BLAS_INPUT_INVALID_VERTEX_COUNT;
        }
        if (!record.tri->indexes || record.tri->numIndexes < record.sourceRange.indexes.count)
        {
            invalidFlags |= RT_PT_RIGID_BLAS_INPUT_INVALID_INDEX_COUNT;
        }
    }
    return invalidFlags;
}

bool RigidMeshHasCachedRouteData(const RtSmokeGeometryUniverse::RigidMeshCandidateRecord& record)
{
    if (!(
        record.valid &&
        record.sourceRange.vertices.count > 0 &&
        record.sourceRange.indexes.count > 0 &&
        (record.sourceRange.indexes.count % 3) == 0 &&
        record.sourceRange.triangles.count > 0 &&
        record.sourceRange.triangles.count * 3 == record.sourceRange.indexes.count &&
        static_cast<int>(record.cachedLocalVertices.size()) == record.sourceRange.vertices.count &&
        static_cast<int>(record.cachedLocalIndexes.size()) == record.sourceRange.indexes.count &&
        record.localBoundsValid &&
        !record.localBounds.IsCleared()))
    {
        return false;
    }

    for (const PathTraceSmokeVertex& vertex : record.cachedLocalVertices)
    {
        const idVec3 position = SmokeVertexPosition(vertex);
        if (!SmokeVec3IsFinite(position) ||
            idMath::Fabs(position.x) >= 100000.0f ||
            idMath::Fabs(position.y) >= 100000.0f ||
            idMath::Fabs(position.z) >= 100000.0f)
        {
            return false;
        }
    }

    const uint32_t vertexCount = static_cast<uint32_t>(record.cachedLocalVertices.size());
    for (uint32_t index : record.cachedLocalIndexes)
    {
        if (index >= vertexCount)
        {
            return false;
        }
    }

    return true;
}

bool RigidMeshHasCachedRouteGpuReady(const RtSmokeGeometryUniverse::RigidMeshCandidateRecord& record)
{
    return
        RigidMeshHasCachedRouteData(record) &&
        record.rigidVertexBuffer &&
        record.rigidIndexBuffer &&
        record.rigidBlas &&
        record.gpuBuffersUploaded &&
        record.gpuBlasCreated &&
        record.gpuBlasBuildSubmitted &&
        record.gpuBlasVertexCount == static_cast<int>(record.cachedLocalVertices.size()) &&
        record.gpuBlasIndexCount == static_cast<int>(record.cachedLocalIndexes.size());
}

bool RigidPlanInstanceMatchesRecord(
    const RtSmokePlanTlasInstance& plannedInstance,
    const RtSmokeGeometryUniverse::RigidMeshCandidateRecord& record)
{
    return record.valid && record.meshHash == plannedInstance.meshHash;
}

bool RigidRouteTransformElementFinite(float value)
{
    return value == value && idMath::Fabs(value) < 100000.0f;
}

bool RigidRouteTransformUsable(const float objectToWorld[16])
{
    if (!objectToWorld)
    {
        return false;
    }

    for (int elementIndex = 0; elementIndex < 16; ++elementIndex)
    {
        if (!RigidRouteTransformElementFinite(objectToWorld[elementIndex]))
        {
            return false;
        }
    }

    const float basis0 =
        objectToWorld[0] * objectToWorld[0] +
        objectToWorld[1] * objectToWorld[1] +
        objectToWorld[2] * objectToWorld[2];
    const float basis1 =
        objectToWorld[4] * objectToWorld[4] +
        objectToWorld[5] * objectToWorld[5] +
        objectToWorld[6] * objectToWorld[6];
    const float basis2 =
        objectToWorld[8] * objectToWorld[8] +
        objectToWorld[9] * objectToWorld[9] +
        objectToWorld[10] * objectToWorld[10];
    return
        basis0 > 1.0e-8f && basis0 < 1000000.0f &&
        basis1 > 1.0e-8f && basis1 < 1000000.0f &&
        basis2 > 1.0e-8f && basis2 < 1000000.0f;
}

bool RigidCachedTlasInstanceValid(
    const RtSmokePlanTlasInstance& plannedInstance,
    const RtSmokeGeometryUniverse::RigidMeshCandidateRecord& record)
{
    if (!RigidMeshHasCachedRouteGpuReady(record) ||
        !RigidRouteTransformUsable(plannedInstance.transform))
    {
        return false;
    }

    RtPathTraceRigidResidencyBoundsBox boundsBox;
    boundsBox.valid = true;
    for (int cornerIndex = 0; cornerIndex < 8; ++cornerIndex)
    {
        idVec3 localPoint;
        localPoint.x = record.localBounds[(cornerIndex ^ (cornerIndex >> 1)) & 1].x;
        localPoint.y = record.localBounds[(cornerIndex >> 1) & 1].y;
        localPoint.z = record.localBounds[(cornerIndex >> 2) & 1].z;
        TransformRigidResidencyBoundsPoint(plannedInstance.transform, localPoint, boundsBox.corners[cornerIndex]);
    }
    return ValidateRigidResidencyBoundsBox(boundsBox);
}

void AppendRigidRoutePlaceholder(
    RtPathTraceRigidRouteBuild& build,
    const RtSmokePlanTlasInstance& plannedInstance)
{
    PathTraceRigidRouteInstance routeInstance;
    routeInstance.instanceIdLo = static_cast<uint32_t>(plannedInstance.sourceInstanceId & 0xffffffffull);
    routeInstance.instanceIdHi = static_cast<uint32_t>((plannedInstance.sourceInstanceId >> 32) & 0xffffffffull);
    if (plannedInstance.hasPreviousTransform)
    {
        routeInstance.flags |= PT_RIGID_ROUTE_HAS_PREVIOUS_TRANSFORM;
    }
    if (plannedInstance.transformContinuous)
    {
        routeInstance.flags |= PT_RIGID_ROUTE_TRANSFORM_CONTINUOUS;
    }
    if (!plannedInstance.sourceSeenThisFrame)
    {
        routeInstance.flags |= PT_RIGID_ROUTE_CACHED_SOURCE;
    }
    CopyRigidRouteTransformRows(routeInstance.currentObjectToWorld, plannedInstance.transform);
    CopyRigidRouteTransformRows(routeInstance.previousObjectToWorld, plannedInstance.hasPreviousTransform ? plannedInstance.previousTransform : plannedInstance.transform);
    build.instances.push_back(routeInstance);
    build.instanceSeenThisFrame.push_back(plannedInstance.sourceSeenThisFrame ? 1u : 0u);

    std::array<float, 16> objectToWorld = {};
    for (int elementIndex = 0; elementIndex < 16; ++elementIndex)
    {
        objectToWorld[elementIndex] = plannedInstance.transform[elementIndex];
    }
    build.instanceObjectToWorld.push_back(objectToWorld);
}

PathTraceSmokeVertex BuildRigidLocalSmokeVertex(const idDrawVert& drawVert, const float normalTexMatrix[6])
{
    idVec3 localNormal = drawVert.GetNormal();
    if (localNormal.Normalize() == 0.0f)
    {
        localNormal.Set(0.0f, 0.0f, 1.0f);
    }
    idVec3 localTangent = drawVert.GetTangent();
    if (localTangent.Normalize() == 0.0f)
    {
        localTangent.Set(1.0f, 0.0f, 0.0f);
    }
    const float bitangentSign = drawVert.GetBiTangentSign();
    idVec3 localBitangent = drawVert.GetBiTangent();
    if (localBitangent.Normalize() == 0.0f)
    {
        localBitangent.Cross(localNormal, localTangent);
        localBitangent *= bitangentSign;
        localBitangent.Normalize();
    }

    const idVec2 texCoord = drawVert.GetTexCoord();
    PathTraceSmokeVertex vertex = {};
    vertex.position[0] = drawVert.xyz.x;
    vertex.position[1] = drawVert.xyz.y;
    vertex.position[2] = drawVert.xyz.z;
    vertex.position[3] = 1.0f;
    vertex.normal[0] = localNormal.x;
    vertex.normal[1] = localNormal.y;
    vertex.normal[2] = localNormal.z;
    vertex.normal[3] = 0.0f;
    vertex.texCoord[0] = texCoord.x;
    vertex.texCoord[1] = texCoord.y;
    vertex.texCoord[2] = normalTexMatrix[0] * texCoord.x + normalTexMatrix[1] * texCoord.y + normalTexMatrix[2];
    vertex.texCoord[3] = normalTexMatrix[3] * texCoord.x + normalTexMatrix[4] * texCoord.y + normalTexMatrix[5];
    vertex.color[0] = drawVert.color[0] * (1.0f / 255.0f);
    vertex.color[1] = drawVert.color[1] * (1.0f / 255.0f);
    vertex.color[2] = drawVert.color[2] * (1.0f / 255.0f);
    vertex.color[3] = drawVert.color[3] * (1.0f / 255.0f);
    vertex.color2[0] = drawVert.color2[0] * (1.0f / 255.0f);
    vertex.color2[1] = drawVert.color2[1] * (1.0f / 255.0f);
    vertex.color2[2] = drawVert.color2[2] * (1.0f / 255.0f);
    vertex.color2[3] = drawVert.color2[3] * (1.0f / 255.0f);
    vertex.tangent[0] = localTangent.x;
    vertex.tangent[1] = localTangent.y;
    vertex.tangent[2] = localTangent.z;
    vertex.tangent[3] = bitangentSign;
    vertex.bitangent[0] = localBitangent.x;
    vertex.bitangent[1] = localBitangent.y;
    vertex.bitangent[2] = localBitangent.z;
    vertex.bitangent[3] = 0.0f;
    return vertex;
}

bool BuildRigidLocalMeshData(const RtSmokeGeometryUniverse::RigidMeshCandidateRecord& record, std::vector<PathTraceSmokeVertex>& vertices, std::vector<uint32_t>& indexes)
{
    if (record.tri == nullptr)
    {
        if (RigidMeshHasCachedRouteData(record))
        {
            vertices = record.cachedLocalVertices;
            indexes = record.cachedLocalIndexes;
            return true;
        }
        return false;
    }

    if (ValidateRigidBlasInputRecord(record) != 0)
    {
        return false;
    }

    vertices.resize(record.sourceRange.vertices.count);
    for (int vertexIndex = 0; vertexIndex < record.sourceRange.vertices.count; ++vertexIndex)
    {
        vertices[vertexIndex] = BuildRigidLocalSmokeVertex(record.tri->verts[vertexIndex], record.normalTexMatrix);
    }

    indexes.resize(record.sourceRange.indexes.count);
    for (int indexIndex = 0; indexIndex < record.sourceRange.indexes.count; ++indexIndex)
    {
        const int sourceIndex = static_cast<int>(record.tri->indexes[indexIndex]);
        if (sourceIndex < 0 || sourceIndex >= record.sourceRange.vertices.count)
        {
            return false;
        }
        indexes[indexIndex] = static_cast<uint32_t>(sourceIndex);
    }
    return true;
}

void RefreshRigidMeshCandidateCpuCache(RtSmokeGeometryUniverse::RigidMeshCandidateRecord& record)
{
    if (!record.tri ||
        !record.tri->verts ||
        !record.tri->indexes ||
        record.sourceRange.vertices.count <= 0 ||
        record.sourceRange.indexes.count <= 0 ||
        record.tri->numVerts < record.sourceRange.vertices.count ||
        record.tri->numIndexes < record.sourceRange.indexes.count)
    {
        return;
    }

    record.cachedLocalVertices.resize(record.sourceRange.vertices.count);
    for (int vertexIndex = 0; vertexIndex < record.sourceRange.vertices.count; ++vertexIndex)
    {
        record.cachedLocalVertices[vertexIndex] = BuildRigidLocalSmokeVertex(record.tri->verts[vertexIndex], record.normalTexMatrix);
    }

    record.cachedLocalIndexes.resize(record.sourceRange.indexes.count);
    for (int indexIndex = 0; indexIndex < record.sourceRange.indexes.count; ++indexIndex)
    {
        const int sourceIndex = static_cast<int>(record.tri->indexes[indexIndex]);
        if (sourceIndex < 0 || sourceIndex >= record.sourceRange.vertices.count)
        {
            record.cachedLocalIndexes.clear();
            return;
        }
        record.cachedLocalIndexes[indexIndex] = static_cast<uint32_t>(sourceIndex);
    }

    record.localBounds = record.tri->bounds;
    record.localBoundsValid = !record.localBounds.IsCleared();
}

bool BuildRigidResidencyWorldBounds(
    const RtSmokeGeometryUniverse::RigidMeshCandidateRecord& meshRecord,
    const RtPathTraceRigidRouteInstanceObservation& instance,
    idBounds& worldBounds)
{
    if (!meshRecord.localBoundsValid || meshRecord.localBounds.IsCleared())
    {
        return false;
    }

    worldBounds.Clear();
    for (int cornerIndex = 0; cornerIndex < 8; ++cornerIndex)
    {
        idVec3 localPoint;
        localPoint.x = meshRecord.localBounds[(cornerIndex ^ (cornerIndex >> 1)) & 1].x;
        localPoint.y = meshRecord.localBounds[(cornerIndex >> 1) & 1].y;
        localPoint.z = meshRecord.localBounds[(cornerIndex >> 2) & 1].z;
        idVec3 worldPoint;
        TransformRigidResidencyBoundsPoint(instance.objectToWorld, localPoint, worldPoint);
        if (!IsRigidResidencyBoundsPointFinite(worldPoint))
        {
            return false;
        }
        worldBounds.AddPoint(worldPoint);
    }
    return !worldBounds.IsCleared();
}

uint32_t FindRigidRouteMaterialTableIndex(const std::vector<uint32_t>& materialTableIds, uint32_t materialId, int& missingCount)
{
    for (size_t materialIndex = 0; materialIndex < materialTableIds.size(); ++materialIndex)
    {
        if (materialTableIds[materialIndex] == materialId)
        {
            return static_cast<uint32_t>(materialIndex);
        }
    }
    ++missingCount;
    return 0;
}

void BuildRigidTlasAffineTransform(const float objectToWorld[16], nvrhi::rt::AffineTransform& transform)
{
    transform[0] = objectToWorld[0];
    transform[1] = objectToWorld[4];
    transform[2] = objectToWorld[8];
    transform[3] = objectToWorld[12];
    transform[4] = objectToWorld[1];
    transform[5] = objectToWorld[5];
    transform[6] = objectToWorld[9];
    transform[7] = objectToWorld[13];
    transform[8] = objectToWorld[2];
    transform[9] = objectToWorld[6];
    transform[10] = objectToWorld[10];
    transform[11] = objectToWorld[14];
}

std::vector<uint32_t> UniqueSortedMaterialIds(std::vector<uint32_t> materialIds)
{
    std::sort(materialIds.begin(), materialIds.end());
    materialIds.erase(std::unique(materialIds.begin(), materialIds.end()), materialIds.end());
    return materialIds;
}

void AddMaterialSample(uint32_t* samples, int& sampleCount, uint32_t materialId)
{
    if (sampleCount >= 8)
    {
        return;
    }
    samples[sampleCount++] = materialId;
}

}

void RtSmokeGeometryUniverse::Clear()
{
    m_currentFrameIndex = 0;
    m_frameActive = false;
    m_staticSurfaceRecords.clear();
    m_staticSurfaceLookup.clear();
    m_staticSurfaceKeys.clear();
    m_staticVertexCache.clear();
    m_staticIndexCache.clear();
    m_staticTriangleClassCache.clear();
    m_staticTriangleMaterialCache.clear();
    m_staticGeometryGeneration = 1;
    m_staticMaterialGeneration = 1;
    m_previousStaticSnapshotGeneration = m_staticGeometryGeneration;
    m_previousStaticSnapshotMaterialGeneration = m_staticMaterialGeneration;
    m_staticMaterialDirtyTriangleOffset = -1;
    m_staticMaterialDirtyTriangleCount = 0;
    m_previousStaticVertexCache.clear();
    m_previousStaticIndexCache.clear();
    m_previousStaticTriangleClassCache.clear();
    m_previousStaticTriangleMaterialCache.clear();
    ClearRigidResidencyCaches();
    m_rigidResidencyStats = RtPathTraceRigidResidencyStats();
    m_rigidResidencyEnabled = false;
    m_rigidResidencyWorld = nullptr;
    ResetRigidMeshCandidateFrameStats();
    ++m_generation;
}

void RtSmokeGeometryUniverse::RetireRigidBlas(RigidMeshCandidateRecord& record)
{
    if (record.rigidBlas && r_pathTracingAsyncBvh.GetInteger() != 0)
    {
        RetiredRigidBlasRecord retired;
        retired.rigidBlas = record.rigidBlas;
        retired.retireFrame = m_currentFrameIndex;
        retired.retireGeneration = m_generation;
        m_retiredRigidBlasRecords.push_back(retired);
    }

    record.rigidBlas = nullptr;
    record.rigidBlasDesc = nvrhi::rt::AccelStructDesc();
    record.gpuBlasCreated = false;
    record.gpuBlasBuildSubmitted = false;
    record.gpuBlasVertexCount = 0;
    record.gpuBlasIndexCount = 0;
}

void RtSmokeGeometryUniverse::ReleaseExpiredRetiredRigidBlas()
{
    if (m_retiredRigidBlasRecords.empty())
    {
        return;
    }

    if (r_pathTracingAsyncBvh.GetInteger() == 0)
    {
        m_retiredRigidBlasRecords.clear();
        return;
    }

    const uint64 retireFrames = RT_SMOKE_RIGID_BLAS_RETIRE_FRAMES;
    m_retiredRigidBlasRecords.erase(
        std::remove_if(
            m_retiredRigidBlasRecords.begin(),
            m_retiredRigidBlasRecords.end(),
            [this, retireFrames](const RetiredRigidBlasRecord& retired) {
                return retired.retireFrame + retireFrames < m_currentFrameIndex;
            }),
        m_retiredRigidBlasRecords.end());
}

void RtSmokeGeometryUniverse::ClearRigidResidencyCaches()
{
    for (RigidMeshCandidateRecord& record : m_rigidMeshCandidateRecords)
    {
        RetireRigidBlas(record);
    }
    if (r_pathTracingAsyncBvh.GetInteger() == 0)
    {
        m_retiredRigidBlasRecords.clear();
    }
    m_rigidMeshCandidateRecords.clear();
    m_rigidMeshCandidateLookup.clear();
    m_frameRigidMeshCandidateHashes.clear();
    m_rigidResidentRecords.clear();
    m_rigidResidentLookup.clear();
    m_rigidResidentFrameInstances.clear();
    m_rigidResidencyAreaWalkEntitiesThisFrame = 0;
    m_rigidResidencyAreaWalkRejectedEntitiesThisFrame = 0;
    m_rigidResidencyAreaWalkSurfacesThisFrame = 0;
    m_rigidResidencyAreaWalkRejectedSurfacesThisFrame = 0;
    m_rigidResidencyAreaWalkEligibleSurfacesThisFrame = 0;
    m_rigidResidencyAreaWalkDuplicateVisibleThisFrame = 0;
    m_rigidResidencyAreaWalkDuplicateFrameThisFrame = 0;
    m_rigidResidencyAreaWalkInstancesThisFrame = 0;
}

void RtSmokeGeometryUniverse::ReserveStaticSurfaceRecords(size_t surfaceCount)
{
    if (surfaceCount <= m_staticSurfaceRecords.capacity())
    {
        return;
    }

    m_staticSurfaceRecords.reserve(surfaceCount);
    m_staticSurfaceLookup.reserve(surfaceCount);
    m_staticSurfaceKeys.reserve(surfaceCount);
}

void RtSmokeGeometryUniverse::BeginFrame(uint64 frameIndex, const idRenderWorldLocal* renderWorld)
{
    PtGeometryLifecycle::MaybeDumpLifecycleStats(frameIndex);
    if (r_pathTracingGeometryResidencyV2.GetInteger() != 0 &&
        renderWorld != nullptr &&
        m_rigidResidencyWorld != nullptr &&
        m_rigidResidencyWorld != renderWorld)
    {
        ClearRigidResidencyCaches();
        ++m_generation;
    }
    if (renderWorld == nullptr && m_rigidResidencyWorld != nullptr)
    {
        ClearRigidResidencyCaches();
        m_rigidResidencyWorld = nullptr;
        ++m_generation;
    }
    if (renderWorld != nullptr)
    {
        m_rigidResidencyWorld = renderWorld;
    }
    m_previousStaticVertexCache = m_staticVertexCache;
    m_previousStaticIndexCache = m_staticIndexCache;
    m_previousStaticTriangleClassCache = m_staticTriangleClassCache;
    m_previousStaticTriangleMaterialCache = m_staticTriangleMaterialCache;
    m_previousStaticSnapshotGeneration = m_staticGeometryGeneration;
    m_previousStaticSnapshotMaterialGeneration = m_staticMaterialGeneration;
    m_currentFrameIndex = frameIndex;
    m_staticMaterialDirtyTriangleOffset = -1;
    m_staticMaterialDirtyTriangleCount = 0;
    ReleaseExpiredRetiredRigidBlas();
    m_frameActive = true;
    ResetRigidMeshCandidateFrameStats();
    m_rigidMeshCandidateFrameStats.frameIndex = frameIndex;
    m_rigidMeshCandidateFrameStats.generation = m_generation;
    m_frameRigidMeshCandidateHashes.clear();
    for (RtSmokePersistentStaticSurfaceRecord& record : m_staticSurfaceRecords)
    {
        record.seenThisFrame = false;
        record.newlyCreatedThisFrame = false;
        record.disappearedThisFrame = false;
    }
    for (RigidMeshCandidateRecord& record : m_rigidMeshCandidateRecords)
    {
        record.seenThisFrame = false;
        record.newlyCreatedThisFrame = false;
        record.instanceCountThisFrame = 0;
        record.tri = nullptr;
    }
    for (RigidResidentInstanceRecord& record : m_rigidResidentRecords)
    {
        record.seenThisFrame = false;
    }
    m_rigidResidentFrameInstances.clear();
    m_rigidResidencyStats = RtPathTraceRigidResidencyStats();
    m_rigidResidencyStats.frameIndex = frameIndex;
    m_rigidResidencyStats.generation = m_generation;
    m_rigidResidencyAreaWalkEntitiesThisFrame = 0;
    m_rigidResidencyAreaWalkRejectedEntitiesThisFrame = 0;
    m_rigidResidencyAreaWalkSurfacesThisFrame = 0;
    m_rigidResidencyAreaWalkRejectedSurfacesThisFrame = 0;
    m_rigidResidencyAreaWalkEligibleSurfacesThisFrame = 0;
    m_rigidResidencyAreaWalkDuplicateVisibleThisFrame = 0;
    m_rigidResidencyAreaWalkDuplicateFrameThisFrame = 0;
    m_rigidResidencyAreaWalkInstancesThisFrame = 0;
}

void RtSmokeGeometryUniverse::EndFrame()
{
    if (!m_frameActive)
    {
        return;
    }

    for (RtSmokePersistentStaticSurfaceRecord& record : m_staticSurfaceRecords)
    {
        if (!record.valid)
        {
            continue;
        }

        if (!record.seenThisFrame)
        {
            record.disappearedThisFrame = record.lastSeenFrame > 0 && record.lastSeenFrame + 1 == m_currentFrameIndex;
            if (record.disappearedThisFrame)
            {
                record.previousRangeValid = false;
                record.historyValid = false;
                record.dirty = true;
            }
            else
            {
                record.dirty = false;
            }
            continue;
        }

        record.dirty = record.newlyCreatedThisFrame || !record.historyValid;
    }

    m_rigidMeshCandidateFrameStats.eligibleUniqueMeshes = static_cast<int>(m_frameRigidMeshCandidateHashes.size());
    m_rigidMeshCandidateFrameStats.persistentEligibleMeshes = static_cast<int>(m_rigidMeshCandidateRecords.size());
    m_rigidMeshCandidateFrameStats.localMeshSourceRecords = static_cast<int>(m_rigidMeshCandidateRecords.size());
    for (RigidMeshCandidateRecord& record : m_rigidMeshCandidateRecords)
    {
        if (!record.valid)
        {
            continue;
        }
        m_rigidMeshCandidateFrameStats.localMeshSourceVerts += record.sourceRange.vertices.count;
        m_rigidMeshCandidateFrameStats.localMeshSourceIndexes += record.sourceRange.indexes.count;
        m_rigidMeshCandidateFrameStats.localMeshSourceTriangles += record.sourceRange.triangles.count;
        if (record.seenThisFrame)
        {
            ++m_rigidMeshCandidateFrameStats.localMeshSourceRecordsSeenThisFrame;
        }
        else
        {
            record.tri = nullptr;
        }
    }
    m_frameActive = false;
}

bool RtSmokeGeometryUniverse::PruneMissingStaticSurfaces()
{
    if (m_staticSurfaceRecords.empty())
    {
        return false;
    }

    const int oldVertexCount = static_cast<int>(m_staticVertexCache.size());
    const int oldIndexCount = static_cast<int>(m_staticIndexCache.size());
    const int oldTriangleCount = static_cast<int>(m_staticTriangleClassCache.size());
    const int oldMaterialTriangleCount = static_cast<int>(m_staticTriangleMaterialCache.size());

    std::vector<RtSmokePersistentStaticSurfaceRecord> keptRecords;
    std::vector<uint64> keptKeys;
    std::unordered_map<uint64, size_t> keptLookup;
    std::vector<PathTraceSmokeVertex> keptVertices;
    std::vector<uint32_t> keptIndexes;
    std::vector<uint32_t> keptTriangleClasses;
    std::vector<uint32_t> keptTriangleMaterials;

    keptRecords.reserve(m_staticSurfaceRecords.size());
    keptKeys.reserve(m_staticSurfaceKeys.size());
    keptLookup.reserve(m_staticSurfaceLookup.size());
    keptVertices.reserve(m_staticVertexCache.size());
    keptIndexes.reserve(m_staticIndexCache.size());
    keptTriangleClasses.reserve(m_staticTriangleClassCache.size());
    keptTriangleMaterials.reserve(m_staticTriangleMaterialCache.size());

    bool changed = false;
    int invalidRangeLogCount = 0;
    int invalidIndexLogCount = 0;
    for (const RtSmokePersistentStaticSurfaceRecord& record : m_staticSurfaceRecords)
    {
        if (!record.valid || !record.seenThisFrame)
        {
            changed = true;
            continue;
        }
        if (!IsSmokeGeometryRangeValid(record.currentRange, oldVertexCount, oldIndexCount, oldTriangleCount, oldMaterialTriangleCount))
        {
            if (invalidRangeLogCount < 4)
            {
                common->Printf(
                    "PathTracePrimaryPass: PT static geometry prune dropped invalid range key=%llu frame=%llu cache v/i/t/m=%d/%d/%d/%d range v=%d/%d i=%d/%d t=%d/%d\n",
                    record.key,
                    m_currentFrameIndex,
                    oldVertexCount,
                    oldIndexCount,
                    oldTriangleCount,
                    oldMaterialTriangleCount,
                    record.currentRange.vertices.offset,
                    record.currentRange.vertices.count,
                    record.currentRange.indexes.offset,
                    record.currentRange.indexes.count,
                    record.currentRange.triangles.offset,
                    record.currentRange.triangles.count);
                ++invalidRangeLogCount;
            }
            changed = true;
            continue;
        }

        const int oldVertexOffset = record.currentRange.vertices.offset;
        const int oldIndexOffset = record.currentRange.indexes.offset;
        const uint32_t oldVertexBegin = static_cast<uint32_t>(oldVertexOffset);
        const uint32_t oldVertexEnd = oldVertexBegin + static_cast<uint32_t>(record.currentRange.vertices.count);
        bool recordIndexesValid = true;
        for (int index = 0; index < record.currentRange.indexes.count; ++index)
        {
            const uint32_t oldIndex = m_staticIndexCache[oldIndexOffset + index];
            if (oldIndex < oldVertexBegin || oldIndex >= oldVertexEnd)
            {
                recordIndexesValid = false;
                break;
            }
        }
        if (!recordIndexesValid)
        {
            if (invalidIndexLogCount < 4)
            {
                common->Printf(
                    "PathTracePrimaryPass: PT static geometry prune dropped invalid indexes key=%llu frame=%llu range v=%d/%d i=%d/%d t=%d/%d\n",
                    record.key,
                    m_currentFrameIndex,
                    record.currentRange.vertices.offset,
                    record.currentRange.vertices.count,
                    record.currentRange.indexes.offset,
                    record.currentRange.indexes.count,
                    record.currentRange.triangles.offset,
                    record.currentRange.triangles.count);
                ++invalidIndexLogCount;
            }
            changed = true;
            continue;
        }

        RtSmokePersistentStaticSurfaceRecord keptRecord = record;
        keptRecord.currentRange.vertices.offset = static_cast<int>(keptVertices.size());
        keptRecord.currentRange.indexes.offset = static_cast<int>(keptIndexes.size());
        keptRecord.currentRange.triangles.offset = static_cast<int>(keptTriangleClasses.size());

        keptVertices.insert(
            keptVertices.end(),
            m_staticVertexCache.begin() + oldVertexOffset,
            m_staticVertexCache.begin() + oldVertexOffset + record.currentRange.vertices.count);

        for (int index = 0; index < record.currentRange.indexes.count; ++index)
        {
            const uint32_t oldIndex = m_staticIndexCache[oldIndexOffset + index];
            keptIndexes.push_back(static_cast<uint32_t>(keptRecord.currentRange.vertices.offset) + (oldIndex - static_cast<uint32_t>(oldVertexOffset)));
        }

        const int oldTriangleOffset = record.currentRange.triangles.offset;
        keptTriangleClasses.insert(
            keptTriangleClasses.end(),
            m_staticTriangleClassCache.begin() + oldTriangleOffset,
            m_staticTriangleClassCache.begin() + oldTriangleOffset + record.currentRange.triangles.count);
        keptTriangleMaterials.insert(
            keptTriangleMaterials.end(),
            m_staticTriangleMaterialCache.begin() + oldTriangleOffset,
            m_staticTriangleMaterialCache.begin() + oldTriangleOffset + record.currentRange.triangles.count);

        keptRecord.dirty = true;
        keptLookup[keptRecord.key] = keptRecords.size();
        keptKeys.push_back(keptRecord.key);
        keptRecords.push_back(keptRecord);
    }

    if (!changed && keptVertices.size() == m_staticVertexCache.size() && keptIndexes.size() == m_staticIndexCache.size())
    {
        return false;
    }

    m_staticSurfaceRecords.swap(keptRecords);
    m_staticSurfaceLookup.swap(keptLookup);
    m_staticSurfaceKeys.swap(keptKeys);
    m_staticVertexCache.swap(keptVertices);
    m_staticIndexCache.swap(keptIndexes);
    m_staticTriangleClassCache.swap(keptTriangleClasses);
    m_staticTriangleMaterialCache.swap(keptTriangleMaterials);
    ++m_staticGeometryGeneration;
    ++m_generation;
    return true;
}

void RtSmokeGeometryUniverse::NotifyStaticCacheChanged()
{
    ++m_staticGeometryGeneration;
    ++m_generation;
}

bool RtSmokeGeometryUniverse::RefreshStaticSurfaceMaterial(uint64 key, uint32_t materialId)
{
    RtSmokePersistentStaticSurfaceRecord* record = FindStaticSurfaceMutable(key);
    if (!record || !record->valid || materialId == 0u)
    {
        return false;
    }

    const int triangleOffset = record->currentRange.triangles.offset;
    const int triangleCount = record->currentRange.triangles.count;
    if (triangleOffset < 0 ||
        triangleCount <= 0 ||
        triangleOffset + triangleCount > static_cast<int>(m_staticTriangleMaterialCache.size()))
    {
        return false;
    }

    bool changed = record->materialId != materialId;
    for (int triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex)
    {
        uint32_t& triangleMaterialId = m_staticTriangleMaterialCache[triangleOffset + triangleIndex];
        if (triangleMaterialId != materialId)
        {
            triangleMaterialId = materialId;
            changed = true;
        }
    }
    if (!changed)
    {
        return false;
    }

    record->materialId = materialId;
    ++m_staticGeometryGeneration;
    record->materialGeneration = ++m_staticMaterialGeneration;
    AccumulateSmokeGeometryElementRange(record->currentRange.triangles, m_staticMaterialDirtyTriangleOffset, m_staticMaterialDirtyTriangleCount);
    return true;
}

bool RtSmokeGeometryUniverse::HasStaticSurface(uint64 key) const
{
    return FindStaticSurface(key) != nullptr;
}

RtSmokePersistentStaticSurfaceRecord* RtSmokeGeometryUniverse::TouchStaticSurface(uint64 key)
{
    RtSmokePersistentStaticSurfaceRecord* record = FindStaticSurfaceMutable(key);
    if (!record || !record->valid)
    {
        return nullptr;
    }

    if (!record->seenThisFrame)
    {
        record->previousSeenFrame = record->lastSeenFrame;
        record->lastSeenFrame = m_currentFrameIndex;
        record->seenThisFrame = true;
        record->newlyCreatedThisFrame = false;
        record->disappearedThisFrame = false;
        const bool consecutiveFrame =
            record->previousSeenFrame > 0 &&
            record->previousSeenFrame + 1 == m_currentFrameIndex;
        if (consecutiveFrame)
        {
            record->previousRange = record->currentRange;
        }
        record->previousRangeValid =
            consecutiveFrame &&
            SmokeGeometryRangeHasOffsets(record->previousRange) &&
            SmokeGeometryRangesMatchCounts(record->previousRange, record->currentRange);
        record->historyValid = record->previousRangeValid;
    }

    return record;
}

bool RtSmokeGeometryUniverse::CanAppendStaticSurface(int vertexCount, int indexCount, int maxVertexCount, int maxIndexCount) const
{
    return static_cast<int>(m_staticVertexCache.size()) + vertexCount <= maxVertexCount &&
        static_cast<int>(m_staticIndexCache.size()) + indexCount <= maxIndexCount;
}

RtSmokeStaticSurfaceAppend RtSmokeGeometryUniverse::BeginStaticSurfaceAppend(uint64 key, uint32_t surfaceClassId, uint32_t materialId, int vertexCount, int indexCount) const
{
    RtSmokeStaticSurfaceAppend append;
    append.key = key;
    append.surfaceClassId = surfaceClassId;
    append.materialId = materialId;
    append.vertexOffset = static_cast<int>(m_staticVertexCache.size());
    append.indexOffset = static_cast<int>(m_staticIndexCache.size());
    append.triangleOffset = static_cast<int>(m_staticTriangleClassCache.size());
    append.requestedVertexCount = vertexCount;
    append.requestedIndexCount = indexCount;
    return append;
}

void RtSmokeGeometryUniverse::CompleteStaticSurfaceAppend(const RtSmokeStaticSurfaceAppend& append, int emittedIndexCount)
{
    if (emittedIndexCount <= 0)
    {
        return;
    }

    RtSmokePersistentStaticSurfaceRecord record;
    record.valid = true;
    record.key = append.key;
    record.surfaceClassId = append.surfaceClassId;
    record.materialId = append.materialId;
    record.currentRange.vertices.offset = append.vertexOffset;
    record.currentRange.vertices.count = static_cast<int>(m_staticVertexCache.size()) - append.vertexOffset;
    record.currentRange.indexes.offset = append.indexOffset;
    record.currentRange.indexes.count = emittedIndexCount;
    record.currentRange.triangles.offset = append.triangleOffset;
    record.currentRange.triangles.count = emittedIndexCount / 3;
    record.lastSeenFrame = m_currentFrameIndex;
    record.previousSeenFrame = 0;
    record.seenThisFrame = true;
    record.newlyCreatedThisFrame = true;
    record.disappearedThisFrame = false;
    record.previousRangeValid = false;
    record.historyValid = false;
    record.dirty = true;
    record.materialGeneration = m_staticMaterialGeneration;
    record.geometryFormat = RtSmokeGeometryBufferFormat::LegacySmokeVertex;

    const size_t recordIndex = m_staticSurfaceRecords.size();
    m_staticSurfaceRecords.push_back(record);
    m_staticSurfaceLookup[append.key] = recordIndex;
    m_staticSurfaceKeys.push_back(append.key);
    ++m_staticGeometryGeneration;
    ++m_generation;
}

const RtSmokePersistentStaticSurfaceRecord* RtSmokeGeometryUniverse::FindStaticSurface(uint64 key) const
{
    const std::unordered_map<uint64, size_t>::const_iterator it = m_staticSurfaceLookup.find(key);
    if (it == m_staticSurfaceLookup.end() || it->second >= m_staticSurfaceRecords.size())
    {
        return nullptr;
    }

    const RtSmokePersistentStaticSurfaceRecord& record = m_staticSurfaceRecords[it->second];
    return record.valid && record.key == key ? &record : nullptr;
}

RtSmokePersistentStaticSurfaceRecord* RtSmokeGeometryUniverse::FindStaticSurfaceMutable(uint64 key)
{
    const std::unordered_map<uint64, size_t>::const_iterator it = m_staticSurfaceLookup.find(key);
    if (it == m_staticSurfaceLookup.end() || it->second >= m_staticSurfaceRecords.size())
    {
        return nullptr;
    }

    RtSmokePersistentStaticSurfaceRecord& record = m_staticSurfaceRecords[it->second];
    return record.valid && record.key == key ? &record : nullptr;
}

const std::vector<RtSmokePersistentStaticSurfaceRecord>& RtSmokeGeometryUniverse::StaticSurfaceRecords() const
{
    return m_staticSurfaceRecords;
}

void RtSmokeGeometryUniverse::BuildStaticTlasBucketObservations(
    std::vector<RtSmokeStaticTlasBucketObservation>& buckets,
    bool hasStaticBlas,
    uint32_t activeReasonFlags) const
{
    buckets.clear();
    buckets.reserve(m_staticSurfaceRecords.size());

    const int vertexCount = static_cast<int>(m_staticVertexCache.size());
    const int indexCount = static_cast<int>(m_staticIndexCache.size());
    const int triangleCount = static_cast<int>(m_staticTriangleClassCache.size());
    const int materialTriangleCount = static_cast<int>(m_staticTriangleMaterialCache.size());
    for (size_t recordIndex = 0; recordIndex < m_staticSurfaceRecords.size(); ++recordIndex)
    {
        const RtSmokePersistentStaticSurfaceRecord& record = m_staticSurfaceRecords[recordIndex];
        if (!record.valid ||
            !IsSmokeGeometryRangeValid(record.currentRange, vertexCount, indexCount, triangleCount, materialTriangleCount))
        {
            continue;
        }

        RtSmokeStaticTlasBucketObservationInput input;
        input.bucketKey = record.key;
        input.routeRecordIndex = static_cast<uint32_t>(recordIndex);
        input.activeReasonFlags = activeReasonFlags;
        input.vertexOffset = record.currentRange.vertices.offset;
        input.indexOffset = record.currentRange.indexes.offset;
        input.triangleOffset = record.currentRange.triangles.offset;
        input.vertexCount = record.currentRange.vertices.count;
        input.indexCount = record.currentRange.indexes.count;
        input.triangleCount = record.currentRange.triangles.count;
        input.valid = true;
        input.seenThisFrame = record.seenThisFrame;
        input.hasBlas = hasStaticBlas;
        RtSmokeStaticTlasBucketObservation bucket;
        if (BuildSmokeStaticTlasBucketObservation(input, bucket))
        {
            buckets.push_back(bucket);
        }
    }
}

std::vector<uint64>& RtSmokeGeometryUniverse::StaticSurfaceKeys()
{
    return m_staticSurfaceKeys;
}

const std::vector<uint64>& RtSmokeGeometryUniverse::StaticSurfaceKeys() const
{
    return m_staticSurfaceKeys;
}

std::vector<PathTraceSmokeVertex>& RtSmokeGeometryUniverse::StaticVertices()
{
    return m_staticVertexCache;
}

const std::vector<PathTraceSmokeVertex>& RtSmokeGeometryUniverse::StaticVertices() const
{
    return m_staticVertexCache;
}

std::vector<uint32_t>& RtSmokeGeometryUniverse::StaticIndexes()
{
    return m_staticIndexCache;
}

const std::vector<uint32_t>& RtSmokeGeometryUniverse::StaticIndexes() const
{
    return m_staticIndexCache;
}

std::vector<uint32_t>& RtSmokeGeometryUniverse::StaticTriangleClasses()
{
    return m_staticTriangleClassCache;
}

const std::vector<uint32_t>& RtSmokeGeometryUniverse::StaticTriangleClasses() const
{
    return m_staticTriangleClassCache;
}

std::vector<uint32_t>& RtSmokeGeometryUniverse::StaticTriangleMaterials()
{
    return m_staticTriangleMaterialCache;
}

const std::vector<uint32_t>& RtSmokeGeometryUniverse::StaticTriangleMaterials() const
{
    return m_staticTriangleMaterialCache;
}

const std::vector<PathTraceSmokeVertex>& RtSmokeGeometryUniverse::PreviousStaticVertices() const
{
    return m_previousStaticVertexCache;
}

const std::vector<uint32_t>& RtSmokeGeometryUniverse::PreviousStaticIndexes() const
{
    return m_previousStaticIndexCache;
}

const std::vector<uint32_t>& RtSmokeGeometryUniverse::PreviousStaticTriangleClasses() const
{
    return m_previousStaticTriangleClassCache;
}

const std::vector<uint32_t>& RtSmokeGeometryUniverse::PreviousStaticTriangleMaterials() const
{
    return m_previousStaticTriangleMaterialCache;
}

uint64 RtSmokeGeometryUniverse::Generation() const
{
    return m_generation;
}

uint64 RtSmokeGeometryUniverse::StaticMaterialGeneration() const
{
    return m_staticMaterialGeneration;
}

RtSmokeGeometryUniverseStats RtSmokeGeometryUniverse::GetStats(bool validateRecords) const
{
    RtSmokeGeometryUniverseStats stats;
    stats.staticRecords = static_cast<int>(m_staticSurfaceRecords.size());
    stats.staticSurfaces = static_cast<int>(m_staticSurfaceKeys.size());
    stats.staticVerts = static_cast<int>(m_staticVertexCache.size());
    stats.staticIndexes = static_cast<int>(m_staticIndexCache.size());
    stats.staticTriangles = static_cast<int>(m_staticTriangleClassCache.size());
    stats.staticMaterialDirtyTriangleOffset = m_staticMaterialDirtyTriangleOffset;
    stats.staticMaterialDirtyTriangleCount = m_staticMaterialDirtyTriangleCount;
    stats.previousStaticVerts = static_cast<int>(m_previousStaticVertexCache.size());
    stats.previousStaticIndexes = static_cast<int>(m_previousStaticIndexCache.size());
    stats.previousStaticTriangles = static_cast<int>(m_previousStaticTriangleClassCache.size());
    stats.previousStaticCpuSnapshotAvailable =
        !m_previousStaticVertexCache.empty() &&
        !m_previousStaticIndexCache.empty() &&
        !m_previousStaticTriangleClassCache.empty() &&
        !m_previousStaticTriangleMaterialCache.empty();
    stats.previousStaticSnapshotGeneration = m_previousStaticSnapshotGeneration;
    stats.previousStaticSnapshotMaterialGeneration = m_previousStaticSnapshotMaterialGeneration;
    if (validateRecords && m_staticSurfaceKeys.size() != m_staticSurfaceRecords.size())
    {
        ++stats.staticKeyVectorMismatches;
    }
    for (size_t recordIndex = 0; recordIndex < m_staticSurfaceRecords.size(); ++recordIndex)
    {
        const RtSmokePersistentStaticSurfaceRecord& record = m_staticSurfaceRecords[recordIndex];
        if (record.valid && record.historyValid)
        {
            ++stats.staticHistoryValid;
        }
        if (record.valid && record.seenThisFrame)
        {
            ++stats.staticSeenThisFrame;
        }
        if (record.valid && record.newlyCreatedThisFrame)
        {
            ++stats.staticNewThisFrame;
        }
        if (record.valid && record.disappearedThisFrame)
        {
            ++stats.staticDisappearedThisFrame;
        }
        if (record.valid && record.previousRangeValid)
        {
            ++stats.staticPreviousRangeValid;
        }
        if (record.valid && record.dirty)
        {
            ++stats.staticDirty;
            AccumulateSmokeGeometryElementRange(record.currentRange.vertices, stats.staticDirtyVertexOffset, stats.staticDirtyVertexCount);
            AccumulateSmokeGeometryElementRange(record.currentRange.indexes, stats.staticDirtyIndexOffset, stats.staticDirtyIndexCount);
            AccumulateSmokeGeometryElementRange(record.currentRange.triangles, stats.staticDirtyTriangleOffset, stats.staticDirtyTriangleCount);
        }
        if (!record.valid)
        {
            continue;
        }

        if (validateRecords)
        {
            if (!IsSmokeGeometryRangeValid(record.currentRange, stats.staticVerts, stats.staticIndexes, stats.staticTriangles, static_cast<int>(m_staticTriangleMaterialCache.size())))
            {
                ++stats.staticRangeErrors;
            }
            if (record.historyValid && !record.previousRangeValid)
            {
                ++stats.staticHistoryErrors;
            }
            if (record.previousRangeValid)
            {
                if (!IsSmokeGeometryRangeValid(record.previousRange, stats.staticVerts, stats.staticIndexes, stats.staticTriangles, static_cast<int>(m_staticTriangleMaterialCache.size())))
                {
                    ++stats.staticHistoryErrors;
                }
            }
            if (recordIndex >= m_staticSurfaceKeys.size() || m_staticSurfaceKeys[recordIndex] != record.key)
            {
                ++stats.staticKeyVectorMismatches;
            }
            const std::unordered_map<uint64, size_t>::const_iterator lookupIt = m_staticSurfaceLookup.find(record.key);
            if (lookupIt == m_staticSurfaceLookup.end() || lookupIt->second != recordIndex)
            {
                ++stats.staticKeyVectorMismatches;
            }
            for (size_t otherIndex = recordIndex + 1; otherIndex < m_staticSurfaceRecords.size(); ++otherIndex)
            {
                const RtSmokePersistentStaticSurfaceRecord& otherRecord = m_staticSurfaceRecords[otherIndex];
                if (otherRecord.valid && otherRecord.key == record.key)
                {
                    ++stats.staticDuplicateKeys;
                    break;
                }
            }
        }
    }
    stats.staticValidationErrors =
        stats.staticRangeErrors +
        stats.staticDuplicateKeys +
        stats.staticHistoryErrors +
        stats.staticKeyVectorMismatches;
    const size_t staticBytes =
        m_staticSurfaceRecords.size() * sizeof(m_staticSurfaceRecords[0]) +
        m_staticSurfaceKeys.size() * sizeof(m_staticSurfaceKeys[0]) +
        m_staticVertexCache.size() * sizeof(m_staticVertexCache[0]) +
        m_staticIndexCache.size() * sizeof(m_staticIndexCache[0]) +
        m_staticTriangleClassCache.size() * sizeof(m_staticTriangleClassCache[0]) +
        m_staticTriangleMaterialCache.size() * sizeof(m_staticTriangleMaterialCache[0]);
    stats.staticBytesKB = static_cast<int>((staticBytes + 1023) / 1024);
    const size_t previousStaticBytes =
        m_previousStaticVertexCache.size() * sizeof(m_previousStaticVertexCache[0]) +
        m_previousStaticIndexCache.size() * sizeof(m_previousStaticIndexCache[0]) +
        m_previousStaticTriangleClassCache.size() * sizeof(m_previousStaticTriangleClassCache[0]) +
        m_previousStaticTriangleMaterialCache.size() * sizeof(m_previousStaticTriangleMaterialCache[0]);
    stats.previousStaticBytesKB = static_cast<int>((previousStaticBytes + 1023) / 1024);
    stats.frameIndex = m_currentFrameIndex;
    stats.generation = m_generation;
    stats.staticMaterialGeneration = m_staticMaterialGeneration;
    return stats;
}

void RtSmokeGeometryUniverse::LogStaticValidationFailures(int maxRecords) const
{
    const int recordLimit = maxRecords > 0 ? maxRecords : 0;
    const int vertexCount = static_cast<int>(m_staticVertexCache.size());
    const int indexCount = static_cast<int>(m_staticIndexCache.size());
    const int triangleCount = static_cast<int>(m_staticTriangleClassCache.size());
    const int materialTriangleCount = static_cast<int>(m_staticTriangleMaterialCache.size());
    const bool keyVectorSizeMismatch = m_staticSurfaceKeys.size() != m_staticSurfaceRecords.size();

    common->Printf("PathTracePrimaryPass: RT smoke geometry universe validation failure dump frame=%llu generation=%llu records=%d keys=%d verts=%d indexes=%d triangles=%d materialTriangles=%d maxRecords=%d\n",
        static_cast<unsigned long long>(m_currentFrameIndex),
        static_cast<unsigned long long>(m_generation),
        static_cast<int>(m_staticSurfaceRecords.size()),
        static_cast<int>(m_staticSurfaceKeys.size()),
        vertexCount,
        indexCount,
        triangleCount,
        materialTriangleCount,
        recordLimit);

    int logged = 0;
    int failedRecords = 0;
    for (size_t recordIndex = 0; recordIndex < m_staticSurfaceRecords.size(); ++recordIndex)
    {
        const RtSmokePersistentStaticSurfaceRecord& record = m_staticSurfaceRecords[recordIndex];
        if (!record.valid)
        {
            continue;
        }

        const bool currentRangeValid = IsSmokeGeometryRangeValid(record.currentRange, vertexCount, indexCount, triangleCount, materialTriangleCount);
        const bool previousRangeValid =
            !record.previousRangeValid ||
            IsSmokeGeometryRangeValid(record.previousRange, vertexCount, indexCount, triangleCount, materialTriangleCount);
        const bool historyValid = !record.historyValid || record.previousRangeValid;
        const bool keyVectorValid =
            recordIndex < m_staticSurfaceKeys.size() &&
            m_staticSurfaceKeys[recordIndex] == record.key;
        const std::unordered_map<uint64, size_t>::const_iterator lookupIt = m_staticSurfaceLookup.find(record.key);
        const bool lookupValid =
            lookupIt != m_staticSurfaceLookup.end() &&
            lookupIt->second == recordIndex;
        const bool duplicateKey = SmokeGeometryRecordHasDuplicateKey(m_staticSurfaceRecords, recordIndex);
        const bool failed =
            !currentRangeValid ||
            !previousRangeValid ||
            !historyValid ||
            !keyVectorValid ||
            !lookupValid ||
            duplicateKey;
        if (!failed)
        {
            continue;
        }

        ++failedRecords;
        if (logged >= recordLimit)
        {
            continue;
        }

        common->Printf("PathTracePrimaryPass: RT smoke geometry record index=%d key=%llu class=%u material=%u flags seen/new/gone/hist/prev/dirty=%d/%d/%d/%d/%d/%d valid current/history/keyVec/lookup/dup=%d/%d/%d/%d/%d frame last/prev=%llu/%llu format=%u ",
            static_cast<int>(recordIndex),
            static_cast<unsigned long long>(record.key),
            record.surfaceClassId,
            record.materialId,
            record.seenThisFrame ? 1 : 0,
            record.newlyCreatedThisFrame ? 1 : 0,
            record.disappearedThisFrame ? 1 : 0,
            record.historyValid ? 1 : 0,
            record.previousRangeValid ? 1 : 0,
            record.dirty ? 1 : 0,
            currentRangeValid ? 1 : 0,
            (previousRangeValid && historyValid) ? 1 : 0,
            keyVectorValid ? 1 : 0,
            lookupValid ? 1 : 0,
            duplicateKey ? 1 : 0,
            static_cast<unsigned long long>(record.lastSeenFrame),
            static_cast<unsigned long long>(record.previousSeenFrame),
            static_cast<uint32_t>(record.geometryFormat));
        PrintSmokeGeometryRange("current", record.currentRange);
        common->Printf(" ");
        PrintSmokeGeometryRange("previous", record.previousRange);
        common->Printf("\n");
        ++logged;
    }

    if (keyVectorSizeMismatch && logged < recordLimit)
    {
        common->Printf("PathTracePrimaryPass: RT smoke geometry key-vector size mismatch records=%d keys=%d\n",
            static_cast<int>(m_staticSurfaceRecords.size()),
            static_cast<int>(m_staticSurfaceKeys.size()));
    }

    common->Printf("PathTracePrimaryPass: RT smoke geometry universe validation failure dump logged=%d failedRecords=%d keyVectorSizeMismatch=%d\n",
        logged,
        failedRecords,
        keyVectorSizeMismatch ? 1 : 0);
}

void RtSmokeGeometryUniverse::LogStaticRangeHistory(int maxRecords) const
{
    const int recordLimit = maxRecords > 0 ? maxRecords : 0;
    const int vertexCount = static_cast<int>(m_staticVertexCache.size());
    const int indexCount = static_cast<int>(m_staticIndexCache.size());
    const int triangleCount = static_cast<int>(m_staticTriangleClassCache.size());
    const int materialTriangleCount = static_cast<int>(m_staticTriangleMaterialCache.size());
    const RtSmokeGeometryUniverseStats stats = GetStats(false);

    common->Printf("PathTracePrimaryPass: RT smoke geometry range history dump frame=%llu generation=%llu records=%d seen/new/gone/history/prev/dirty=%d/%d/%d/%d/%d/%d cache v/i/t=%d/%d/%d previousCpu v/i/t/kb=%d/%d/%d/%d available=%d dirtyRange v/i/t=%d/%d/%d/%d/%d/%d maxRecords=%d\n",
        static_cast<unsigned long long>(m_currentFrameIndex),
        static_cast<unsigned long long>(m_generation),
        stats.staticRecords,
        stats.staticSeenThisFrame,
        stats.staticNewThisFrame,
        stats.staticDisappearedThisFrame,
        stats.staticHistoryValid,
        stats.staticPreviousRangeValid,
        stats.staticDirty,
        stats.staticVerts,
        stats.staticIndexes,
        stats.staticTriangles,
        stats.previousStaticVerts,
        stats.previousStaticIndexes,
        stats.previousStaticTriangles,
        stats.previousStaticBytesKB,
        stats.previousStaticCpuSnapshotAvailable ? 1 : 0,
        stats.staticDirtyVertexOffset,
        stats.staticDirtyVertexCount,
        stats.staticDirtyIndexOffset,
        stats.staticDirtyIndexCount,
        stats.staticDirtyTriangleOffset,
        stats.staticDirtyTriangleCount,
        recordLimit);

    int logged = 0;
    for (size_t recordIndex = 0; recordIndex < m_staticSurfaceRecords.size() && logged < recordLimit; ++recordIndex)
    {
        const RtSmokePersistentStaticSurfaceRecord& record = m_staticSurfaceRecords[recordIndex];
        if (!record.valid)
        {
            continue;
        }

        const bool currentRangeValid = IsSmokeGeometryRangeValid(record.currentRange, vertexCount, indexCount, triangleCount, materialTriangleCount);
        const bool previousRangeValid =
            !record.previousRangeValid ||
            IsSmokeGeometryRangeValid(record.previousRange, vertexCount, indexCount, triangleCount, materialTriangleCount);
        const bool rangeCountsMatch =
            record.previousRangeValid &&
            SmokeGeometryRangesMatchCounts(record.currentRange, record.previousRange);

        common->Printf("PathTracePrimaryPass: RT smoke geometry range record index=%d key=%llu class=%u material=%u flags seen/new/gone/hist/prev/dirty=%d/%d/%d/%d/%d/%d valid current/previous/counts=%d/%d/%d frame last/prev=%llu/%llu format=%u ",
            static_cast<int>(recordIndex),
            static_cast<unsigned long long>(record.key),
            record.surfaceClassId,
            record.materialId,
            record.seenThisFrame ? 1 : 0,
            record.newlyCreatedThisFrame ? 1 : 0,
            record.disappearedThisFrame ? 1 : 0,
            record.historyValid ? 1 : 0,
            record.previousRangeValid ? 1 : 0,
            record.dirty ? 1 : 0,
            currentRangeValid ? 1 : 0,
            previousRangeValid ? 1 : 0,
            rangeCountsMatch ? 1 : 0,
            static_cast<unsigned long long>(record.lastSeenFrame),
            static_cast<unsigned long long>(record.previousSeenFrame),
            static_cast<uint32_t>(record.geometryFormat));
        PrintSmokeGeometryRange("current", record.currentRange);
        common->Printf(" ");
        PrintSmokeGeometryRange("previous", record.previousRange);
        common->Printf("\n");
        ++logged;
    }

    common->Printf("PathTracePrimaryPass: RT smoke geometry range history dump logged=%d\n", logged);
}

void RtSmokeGeometryUniverse::RecordRigidMeshCandidate(const RtPathTraceRigidMeshCandidateObservation& observation)
{
    ++m_rigidMeshCandidateFrameStats.observations;
    if ((observation.sourceFlags & RT_PT_INSTANCE_SOURCE_RIGID) != 0)
    {
        ++m_rigidMeshCandidateFrameStats.rigidObservations;
    }

    const uint32_t rejectFlags = BuildRigidMeshCandidateRejectFlags(observation);
    if (rejectFlags != 0)
    {
        ++m_rigidMeshCandidateFrameStats.rejectedInstances;
        AccumulateRigidMeshCandidateRejectStats(m_rigidMeshCandidateFrameStats, rejectFlags);
        AddRigidMeshCandidateSample(observation, false, rejectFlags, 0);
        return;
    }

    bool cacheHit = false;
    RigidMeshCandidateRecord* record = FindOrCreateRigidMeshCandidate(observation, cacheHit);
    int seenCount = 0;
    if (record)
    {
        record->tri = observation.tri;
        record->vertexBufferIdentity = observation.vertexBufferIdentity;
        record->indexBufferIdentity = observation.indexBufferIdentity;
        record->materialId = observation.materialId;
        record->materialClassSignature = observation.materialClassSignature;
        record->surfaceClassId = observation.surfaceClassId;
        record->triangleClassAndFlags = observation.triangleClassAndFlags != 0u ? observation.triangleClassAndFlags : observation.surfaceClassId;
        record->sourceFlags = observation.sourceFlags;
        record->vertexFormat = observation.vertexFormat;
        record->modelEpoch = observation.modelEpoch;
        record->jointIndex = observation.jointIndex;
        memcpy(record->normalTexMatrix, observation.normalTexMatrix, sizeof(record->normalTexMatrix));
        record->sourceRange.vertices.count = observation.numVerts;
        record->sourceRange.indexes.count = observation.numIndexes;
        record->sourceRange.triangles.count = observation.numIndexes / 3;
        record->materialName = observation.materialName;
        record->modelName = observation.modelName;
        RefreshRigidMeshCandidateCpuCache(*record);
        if (!record->seenThisFrame)
        {
            record->seenThisFrame = true;
            record->lastSeenFrame = static_cast<int>(m_currentFrameIndex);
            record->newlyCreatedThisFrame = !cacheHit;
        }
        ++record->seenCount;
        ++record->instanceCountThisFrame;
        seenCount = record->seenCount;
    }

    ++m_rigidMeshCandidateFrameStats.eligibleInstances;
    m_rigidMeshCandidateFrameStats.eligibleVertsThisFrame += observation.numVerts;
    m_rigidMeshCandidateFrameStats.eligibleIndexesThisFrame += observation.numIndexes;
    m_rigidMeshCandidateFrameStats.eligibleTrianglesThisFrame += observation.numIndexes / 3;
    if ((observation.sourceFlags & RT_PT_INSTANCE_SOURCE_MATERIAL_OVERRIDE) != 0)
    {
        ++m_rigidMeshCandidateFrameStats.materialOverrideEligibleInstances;
    }
    if (cacheHit)
    {
        ++m_rigidMeshCandidateFrameStats.reusedEligibleMeshObservations;
    }
    else
    {
        ++m_rigidMeshCandidateFrameStats.newlyEligibleMeshes;
    }
    m_frameRigidMeshCandidateHashes.insert(observation.meshHash);
    AddRigidMeshCandidateSample(observation, true, 0, seenCount);
}

const RtPathTraceRigidMeshCandidateStats& RtSmokeGeometryUniverse::GetRigidMeshCandidateStats() const
{
    return m_rigidMeshCandidateFrameStats;
}

void RtSmokeGeometryUniverse::RunRigidMeshCandidateDiagnostics(bool dumpRequested, int sceneSource, const RtSmokeSurfaceClassStats* sourceClassStats)
{
    const int sourceRigidTriangles = sourceClassStats ? sourceClassStats->rigidEntityTriangles : 0;
    const int sourceRigidSurfaces = sourceClassStats ? sourceClassStats->rigidEntitySurfaces : 0;
    const int estimatedRemainingRigidTriangles = sourceClassStats ? Max(0, sourceRigidTriangles - m_rigidMeshCandidateFrameStats.eligibleTrianglesThisFrame) : 0;
    if (r_pathTracingSmokeLog.GetInteger() != 0 && (m_currentFrameIndex % 120ull) == 1ull)
    {
        common->Printf("PathTracePrimaryPass: PT rigid mesh candidates source=%d observed=%d rigid=%d eligibleInstances=%d uniqueMeshes=%d localRecords=%d/%d eligibleTris=%d bakedRigid=%d/%d remainingAfterPromotion=%d rejected=%d reused=%d newMeshes=%d overrides=%d rejects nonRigid/geom/material/local/skinned/transient/gui/callback/static/cache=%d/%d/%d/%d/%d/%d/%d/%d/%d/%d\n",
            sceneSource,
            m_rigidMeshCandidateFrameStats.observations,
            m_rigidMeshCandidateFrameStats.rigidObservations,
            m_rigidMeshCandidateFrameStats.eligibleInstances,
            m_rigidMeshCandidateFrameStats.eligibleUniqueMeshes,
            m_rigidMeshCandidateFrameStats.localMeshSourceRecordsSeenThisFrame,
            m_rigidMeshCandidateFrameStats.localMeshSourceRecords,
            m_rigidMeshCandidateFrameStats.eligibleTrianglesThisFrame,
            sourceRigidSurfaces,
            sourceRigidTriangles,
            estimatedRemainingRigidTriangles,
            m_rigidMeshCandidateFrameStats.rejectedInstances,
            m_rigidMeshCandidateFrameStats.reusedEligibleMeshObservations,
            m_rigidMeshCandidateFrameStats.newlyEligibleMeshes,
            m_rigidMeshCandidateFrameStats.materialOverrideEligibleInstances,
            m_rigidMeshCandidateFrameStats.rejectNotRigid,
            m_rigidMeshCandidateFrameStats.rejectInvalidGeometry,
            m_rigidMeshCandidateFrameStats.rejectMissingMaterial,
            m_rigidMeshCandidateFrameStats.rejectNoLocalSpace,
            m_rigidMeshCandidateFrameStats.rejectSkinnedOrDeforming,
            m_rigidMeshCandidateFrameStats.rejectParticleOrTransient,
            m_rigidMeshCandidateFrameStats.rejectGui,
            m_rigidMeshCandidateFrameStats.rejectCallbackOrGenerated,
            m_rigidMeshCandidateFrameStats.rejectStaticWorld,
            m_rigidMeshCandidateFrameStats.rejectStaticCacheMatch);
    }

    if (!dumpRequested)
    {
        return;
    }

    common->Printf("PathTracePrimaryPass: PT rigid mesh universe dump source=%d frame=%llu generation=%llu observed=%d rigid=%d eligibleInstances=%d eligibleUniqueMeshes=%d persistentMeshes=%d localRecords(seen/total)=%d/%d rejected=%d reused=%d newMeshes=%d overrides=%d\n",
        sceneSource,
        static_cast<unsigned long long>(m_rigidMeshCandidateFrameStats.frameIndex),
        static_cast<unsigned long long>(m_rigidMeshCandidateFrameStats.generation),
        m_rigidMeshCandidateFrameStats.observations,
        m_rigidMeshCandidateFrameStats.rigidObservations,
        m_rigidMeshCandidateFrameStats.eligibleInstances,
        m_rigidMeshCandidateFrameStats.eligibleUniqueMeshes,
        m_rigidMeshCandidateFrameStats.persistentEligibleMeshes,
        m_rigidMeshCandidateFrameStats.localMeshSourceRecordsSeenThisFrame,
        m_rigidMeshCandidateFrameStats.localMeshSourceRecords,
        m_rigidMeshCandidateFrameStats.rejectedInstances,
        m_rigidMeshCandidateFrameStats.reusedEligibleMeshObservations,
        m_rigidMeshCandidateFrameStats.newlyEligibleMeshes,
        m_rigidMeshCandidateFrameStats.materialOverrideEligibleInstances);
    common->Printf("PathTracePrimaryPass: PT rigid mesh universe localSource frameVerts/indexes/tris=%d/%d/%d persistentVerts/indexes/tris=%d/%d/%d bakedRigidSurfaces/tris=%d/%d estimatedRigidTrisAfterPromotion=%d renderPath=dynamicFallback\n",
        m_rigidMeshCandidateFrameStats.eligibleVertsThisFrame,
        m_rigidMeshCandidateFrameStats.eligibleIndexesThisFrame,
        m_rigidMeshCandidateFrameStats.eligibleTrianglesThisFrame,
        m_rigidMeshCandidateFrameStats.localMeshSourceVerts,
        m_rigidMeshCandidateFrameStats.localMeshSourceIndexes,
        m_rigidMeshCandidateFrameStats.localMeshSourceTriangles,
        sourceRigidSurfaces,
        sourceRigidTriangles,
        estimatedRemainingRigidTriangles);
    common->Printf("PathTracePrimaryPass: PT rigid mesh universe rejects nonRigid/geom/material/local/skinned/transient/gui/callback/static/cache=%d/%d/%d/%d/%d/%d/%d/%d/%d/%d\n",
        m_rigidMeshCandidateFrameStats.rejectNotRigid,
        m_rigidMeshCandidateFrameStats.rejectInvalidGeometry,
        m_rigidMeshCandidateFrameStats.rejectMissingMaterial,
        m_rigidMeshCandidateFrameStats.rejectNoLocalSpace,
        m_rigidMeshCandidateFrameStats.rejectSkinnedOrDeforming,
        m_rigidMeshCandidateFrameStats.rejectParticleOrTransient,
        m_rigidMeshCandidateFrameStats.rejectGui,
        m_rigidMeshCandidateFrameStats.rejectCallbackOrGenerated,
        m_rigidMeshCandidateFrameStats.rejectStaticWorld,
        m_rigidMeshCandidateFrameStats.rejectStaticCacheMatch);

    for (int sampleIndex = 0; sampleIndex < m_rigidMeshCandidateFrameStats.eligibleSampleCount; ++sampleIndex)
    {
        const RtPathTraceRigidMeshCandidateSample& sample = m_rigidMeshCandidateFrameStats.eligibleSamples[sampleIndex];
        if (!sample.valid)
        {
            continue;
        }
        common->Printf("PathTracePrimaryPass: PT rigid mesh eligible sample %d surf=%d entity=%d renderEntity=%d mesh=%llu instance=%llu tri=%llu vb=%llu ib=%llu format=%u materialClass=0x%x seen=%d verts=%d indexes=%d tris=%d material=%u '%s' model='%s'\n",
            sampleIndex,
            sample.drawSurfIndex,
            sample.entityIndex,
            sample.renderEntityNum,
            static_cast<unsigned long long>(sample.meshHash),
            static_cast<unsigned long long>(sample.instanceId),
            static_cast<unsigned long long>(sample.triIdentity),
            static_cast<unsigned long long>(sample.vertexBufferIdentity),
            static_cast<unsigned long long>(sample.indexBufferIdentity),
            sample.vertexFormat,
            sample.materialClassSignature,
            sample.seenCount,
            sample.numVerts,
            sample.numIndexes,
            sample.numIndexes / 3,
            sample.materialId,
            sample.materialName.c_str(),
            sample.modelName.c_str());
    }

    for (int sampleIndex = 0; sampleIndex < m_rigidMeshCandidateFrameStats.rejectedSampleCount; ++sampleIndex)
    {
        const RtPathTraceRigidMeshCandidateSample& sample = m_rigidMeshCandidateFrameStats.rejectedSamples[sampleIndex];
        if (!sample.valid)
        {
            continue;
        }
        common->Printf("PathTracePrimaryPass: PT rigid mesh rejected sample %d reason=%s flags=0x%x surf=%d entity=%d renderEntity=%d mesh=%llu instance=%llu verts=%d indexes=%d materialClass=0x%x material=%u '%s' model='%s'\n",
            sampleIndex,
            RigidMeshCandidateRejectSummary(sample.rejectFlags),
            sample.rejectFlags,
            sample.drawSurfIndex,
            sample.entityIndex,
            sample.renderEntityNum,
            static_cast<unsigned long long>(sample.meshHash),
            static_cast<unsigned long long>(sample.instanceId),
            sample.numVerts,
            sample.numIndexes,
            sample.materialClassSignature,
            sample.materialId,
            sample.materialName.c_str(),
            sample.modelName.c_str());
    }

    r_pathTracingRigidMeshUniverseDump.SetInteger(0);
}

RtSmokeGeometryUniverse::RigidMeshCandidateRecord* RtSmokeGeometryUniverse::FindOrCreateRigidMeshCandidate(const RtPathTraceRigidMeshCandidateObservation& observation, bool& cacheHit)
{
    cacheHit = false;
    const std::unordered_map<uint64, size_t>::iterator it = m_rigidMeshCandidateLookup.find(observation.meshHash);
    if (it != m_rigidMeshCandidateLookup.end() && it->second < m_rigidMeshCandidateRecords.size())
    {
        cacheHit = true;
        return &m_rigidMeshCandidateRecords[it->second];
    }

    RigidMeshCandidateRecord record;
    record.valid = true;
    record.tri = observation.tri;
    record.meshHash = observation.meshHash;
    record.vertexBufferIdentity = observation.vertexBufferIdentity;
    record.indexBufferIdentity = observation.indexBufferIdentity;
    record.materialId = observation.materialId;
    record.materialClassSignature = observation.materialClassSignature;
    record.surfaceClassId = observation.surfaceClassId;
    record.triangleClassAndFlags = observation.triangleClassAndFlags != 0u ? observation.triangleClassAndFlags : observation.surfaceClassId;
    record.sourceFlags = observation.sourceFlags;
    record.vertexFormat = observation.vertexFormat;
    record.modelEpoch = observation.modelEpoch;
    record.jointIndex = observation.jointIndex;
    record.sourceRange.vertices.offset = 0;
    record.sourceRange.vertices.count = observation.numVerts;
    record.sourceRange.indexes.offset = 0;
    record.sourceRange.indexes.count = observation.numIndexes;
    record.sourceRange.triangles.offset = 0;
    record.sourceRange.triangles.count = observation.numIndexes / 3;
    record.firstSeenFrame = static_cast<int>(m_currentFrameIndex);
    record.lastSeenFrame = static_cast<int>(m_currentFrameIndex);
    record.materialName = observation.materialName;
    record.modelName = observation.modelName;
    RefreshRigidMeshCandidateCpuCache(record);
    const size_t recordIndex = m_rigidMeshCandidateRecords.size();
    m_rigidMeshCandidateRecords.push_back(record);
    m_rigidMeshCandidateLookup[observation.meshHash] = recordIndex;
    ++m_generation;
    m_rigidMeshCandidateFrameStats.generation = m_generation;
    return &m_rigidMeshCandidateRecords.back();
}

void RtSmokeGeometryUniverse::ResetRigidMeshCandidateFrameStats()
{
    m_rigidMeshCandidateFrameStats = RtPathTraceRigidMeshCandidateStats();
}

void RtSmokeGeometryUniverse::AddRigidMeshCandidateSample(const RtPathTraceRigidMeshCandidateObservation& observation, bool eligible, uint32_t rejectFlags, int seenCount)
{
    RtPathTraceRigidMeshCandidateSample* sample = nullptr;
    if (eligible)
    {
        if (m_rigidMeshCandidateFrameStats.eligibleSampleCount >= RT_PT_RIGID_MESH_CANDIDATE_SAMPLES)
        {
            return;
        }
        sample = &m_rigidMeshCandidateFrameStats.eligibleSamples[m_rigidMeshCandidateFrameStats.eligibleSampleCount++];
    }
    else
    {
        if (m_rigidMeshCandidateFrameStats.rejectedSampleCount >= RT_PT_RIGID_MESH_CANDIDATE_SAMPLES)
        {
            return;
        }
        sample = &m_rigidMeshCandidateFrameStats.rejectedSamples[m_rigidMeshCandidateFrameStats.rejectedSampleCount++];
    }

    sample->valid = true;
    sample->eligible = eligible;
    sample->meshHash = observation.meshHash;
    sample->instanceId = observation.instanceId;
    sample->triIdentity = reinterpret_cast<uintptr_t>(observation.tri);
    sample->vertexBufferIdentity = observation.vertexBufferIdentity;
    sample->indexBufferIdentity = observation.indexBufferIdentity;
    sample->rejectFlags = rejectFlags;
    sample->materialId = observation.materialId;
    sample->materialClassSignature = observation.materialClassSignature;
    sample->vertexFormat = observation.vertexFormat;
    sample->drawSurfIndex = observation.drawSurfIndex;
    sample->entityIndex = observation.entityIndex;
    sample->renderEntityNum = observation.renderEntityNum;
    sample->numVerts = observation.numVerts;
    sample->numIndexes = observation.numIndexes;
    sample->seenCount = seenCount;
    sample->materialName = observation.materialName;
    sample->modelName = observation.modelName;
}

RtPathTraceRigidMeshValidationStats RtSmokeGeometryUniverse::ValidateRigidMeshCandidatesAgainstDynamicPayload(
    const std::vector<uint32_t>& dynamicTriangleClassData,
    const std::vector<uint32_t>& dynamicTriangleMaterialData,
    uint32_t triangleClassMask,
    uint32_t rigidClassId) const
{
    RtPathTraceRigidMeshValidationStats stats;
    std::vector<uint32_t> bakedRigidMaterialIds;
    std::vector<uint32_t> eligibleRigidMaterialIds;

    const size_t triangleCount = Min(dynamicTriangleClassData.size(), dynamicTriangleMaterialData.size());
    bakedRigidMaterialIds.reserve(triangleCount);
    for (size_t triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex)
    {
        if ((dynamicTriangleClassData[triangleIndex] & triangleClassMask) != rigidClassId)
        {
            continue;
        }
        ++stats.bakedRigidTriangles;
        bakedRigidMaterialIds.push_back(dynamicTriangleMaterialData[triangleIndex]);
    }

    for (const RigidMeshCandidateRecord& record : m_rigidMeshCandidateRecords)
    {
        if (!record.valid || !record.seenThisFrame)
        {
            continue;
        }
        stats.eligibleRigidTriangles += record.sourceRange.triangles.count * Max(1, record.instanceCountThisFrame);
        for (int instanceIndex = 0; instanceIndex < Max(1, record.instanceCountThisFrame); ++instanceIndex)
        {
            for (int triangleIndex = 0; triangleIndex < record.sourceRange.triangles.count; ++triangleIndex)
            {
                eligibleRigidMaterialIds.push_back(record.materialId);
            }
        }
    }

    stats.triangleDelta = stats.eligibleRigidTriangles - stats.bakedRigidTriangles;
    const std::vector<uint32_t> bakedUnique = UniqueSortedMaterialIds(bakedRigidMaterialIds);
    const std::vector<uint32_t> eligibleUnique = UniqueSortedMaterialIds(eligibleRigidMaterialIds);
    stats.bakedRigidMaterialIds = static_cast<int>(bakedUnique.size());
    stats.eligibleRigidMaterialIds = static_cast<int>(eligibleUnique.size());

    size_t bakedIndex = 0;
    size_t eligibleIndex = 0;
    while (bakedIndex < bakedUnique.size() || eligibleIndex < eligibleUnique.size())
    {
        if (eligibleIndex >= eligibleUnique.size() || (bakedIndex < bakedUnique.size() && bakedUnique[bakedIndex] < eligibleUnique[eligibleIndex]))
        {
            ++stats.missingMaterialIds;
            AddMaterialSample(stats.missingMaterialSamples, stats.missingMaterialSampleCount, bakedUnique[bakedIndex]);
            ++bakedIndex;
        }
        else if (bakedIndex >= bakedUnique.size() || eligibleUnique[eligibleIndex] < bakedUnique[bakedIndex])
        {
            ++stats.extraMaterialIds;
            AddMaterialSample(stats.extraMaterialSamples, stats.extraMaterialSampleCount, eligibleUnique[eligibleIndex]);
            ++eligibleIndex;
        }
        else
        {
            ++bakedIndex;
            ++eligibleIndex;
        }
    }

    return stats;
}

void RtSmokeGeometryUniverse::DumpRigidMeshValidationStats(const RtPathTraceRigidMeshValidationStats& stats, int sceneSource) const
{
    common->Printf("PathTracePrimaryPass: PT rigid mesh validation source=%d bakedRigidTris=%d eligibleLocalTris=%d triangleDelta=%d materialIds baked/eligible=%d/%d missing=%d extra=%d renderPath=dynamicFallback\n",
        sceneSource,
        stats.bakedRigidTriangles,
        stats.eligibleRigidTriangles,
        stats.triangleDelta,
        stats.bakedRigidMaterialIds,
        stats.eligibleRigidMaterialIds,
        stats.missingMaterialIds,
        stats.extraMaterialIds);

    if (stats.missingMaterialSampleCount > 0)
    {
        char sampleText[256];
        sampleText[0] = '\0';
        for (int sampleIndex = 0; sampleIndex < stats.missingMaterialSampleCount; ++sampleIndex)
        {
            idStr::Append(sampleText, sizeof(sampleText), va("%s%u", sampleIndex == 0 ? "" : ",", stats.missingMaterialSamples[sampleIndex]));
        }
        common->Printf("PathTracePrimaryPass: PT rigid mesh validation missingMaterialIds=[%s]\n", sampleText);
    }
    if (stats.extraMaterialSampleCount > 0)
    {
        char sampleText[256];
        sampleText[0] = '\0';
        for (int sampleIndex = 0; sampleIndex < stats.extraMaterialSampleCount; ++sampleIndex)
        {
            idStr::Append(sampleText, sizeof(sampleText), va("%s%u", sampleIndex == 0 ? "" : ",", stats.extraMaterialSamples[sampleIndex]));
        }
        common->Printf("PathTracePrimaryPass: PT rigid mesh validation extraMaterialIds=[%s]\n", sampleText);
    }
}

RtPathTraceRigidBlasPlanStats RtSmokeGeometryUniverse::BuildRigidBlasPlanStats(const RtSmokeSurfaceClassStats* sourceClassStats) const
{
    RtPathTraceRigidBlasPlanStats stats;
    stats.frameIndex = m_currentFrameIndex;
    stats.generation = m_generation;
    stats.persistentMeshRecords = static_cast<int>(m_rigidMeshCandidateRecords.size());
    stats.bakedRigidSurfaces = sourceClassStats ? sourceClassStats->rigidEntitySurfaces : 0;
    stats.bakedRigidTriangles = sourceClassStats ? sourceClassStats->rigidEntityTriangles : 0;

    for (const RigidMeshCandidateRecord& record : m_rigidMeshCandidateRecords)
    {
        if (!record.valid || !record.seenThisFrame)
        {
            continue;
        }

        const int instanceCount = Max(1, record.instanceCountThisFrame);
        ++stats.meshRecords;
        stats.instances += instanceCount;
        stats.localVerts += record.sourceRange.vertices.count;
        stats.localIndexes += record.sourceRange.indexes.count;
        stats.localTriangles += record.sourceRange.triangles.count;
        stats.plannedRemoveRigidTriangles += record.sourceRange.triangles.count * instanceCount;

        if (stats.sampleCount < RT_PT_RIGID_BLAS_PLAN_SAMPLES)
        {
            RtPathTraceRigidBlasPlanSample& sample = stats.samples[stats.sampleCount++];
            sample.valid = true;
            sample.meshHash = record.meshHash;
            sample.triIdentity = reinterpret_cast<uintptr_t>(record.tri);
            sample.vertexBufferIdentity = record.vertexBufferIdentity;
            sample.indexBufferIdentity = record.indexBufferIdentity;
            sample.materialId = record.materialId;
            sample.vertexFormat = record.vertexFormat;
            sample.instanceCount = instanceCount;
            sample.verts = record.sourceRange.vertices.count;
            sample.indexes = record.sourceRange.indexes.count;
            sample.triangles = record.sourceRange.triangles.count;
            sample.materialName = record.materialName;
            sample.modelName = record.modelName;
        }
    }

    stats.estimatedRemainingRigidTriangles = Max(0, stats.bakedRigidTriangles - stats.plannedRemoveRigidTriangles);
    stats.triangleDelta = stats.plannedRemoveRigidTriangles - stats.bakedRigidTriangles;
    return stats;
}

void RtSmokeGeometryUniverse::DumpRigidBlasPlanStats(const RtPathTraceRigidBlasPlanStats& stats, int sceneSource) const
{
    common->Printf("PathTracePrimaryPass: PT rigid BLAS plan source=%d frame=%llu generation=%llu meshRecords=%d instances=%d localVerts/indexes/tris=%d/%d/%d plannedRemoveRigidTris=%d bakedRigidSurfaces/tris=%d/%d remainingDynamicRigidTris=%d triangleDelta=%d persistentMeshRecords=%d gpuBuild=0 renderPath=dynamicFallback\n",
        sceneSource,
        static_cast<unsigned long long>(stats.frameIndex),
        static_cast<unsigned long long>(stats.generation),
        stats.meshRecords,
        stats.instances,
        stats.localVerts,
        stats.localIndexes,
        stats.localTriangles,
        stats.plannedRemoveRigidTriangles,
        stats.bakedRigidSurfaces,
        stats.bakedRigidTriangles,
        stats.estimatedRemainingRigidTriangles,
        stats.triangleDelta,
        stats.persistentMeshRecords);

    for (int sampleIndex = 0; sampleIndex < stats.sampleCount; ++sampleIndex)
    {
        const RtPathTraceRigidBlasPlanSample& sample = stats.samples[sampleIndex];
        if (!sample.valid)
        {
            continue;
        }
        common->Printf("PathTracePrimaryPass: PT rigid BLAS plan sample %d mesh=%llu instances=%d tri=%llu vb=%llu ib=%llu format=%u verts=%d indexes=%d tris=%d material=%u '%s' model='%s'\n",
            sampleIndex,
            static_cast<unsigned long long>(sample.meshHash),
            sample.instanceCount,
            static_cast<unsigned long long>(sample.triIdentity),
            static_cast<unsigned long long>(sample.vertexBufferIdentity),
            static_cast<unsigned long long>(sample.indexBufferIdentity),
            sample.vertexFormat,
            sample.verts,
            sample.indexes,
            sample.triangles,
            sample.materialId,
            sample.materialName.c_str(),
            sample.modelName.c_str());
    }
}

RtPathTraceRigidBlasInputStats RtSmokeGeometryUniverse::BuildRigidBlasInputStats() const
{
    RtPathTraceRigidBlasInputStats stats;
    stats.frameIndex = m_currentFrameIndex;
    stats.generation = m_generation;

    const uint32_t expectedVertexFormat = static_cast<uint32_t>(RtSmokeGeometryBufferFormat::LegacySmokeVertex);
    for (const RigidMeshCandidateRecord& record : m_rigidMeshCandidateRecords)
    {
        if (!record.valid || !record.seenThisFrame)
        {
            continue;
        }

        const int instanceCount = Max(1, record.instanceCountThisFrame);
        uint32_t invalidFlags = 0;
        if (record.tri == nullptr)
        {
            invalidFlags |= RT_PT_RIGID_BLAS_INPUT_INVALID_NULL_TRI;
        }
        if (record.sourceRange.vertices.count <= 0)
        {
            invalidFlags |= RT_PT_RIGID_BLAS_INPUT_INVALID_VERTEX_COUNT;
        }
        if (record.sourceRange.indexes.count <= 0 || (record.sourceRange.indexes.count % 3) != 0)
        {
            invalidFlags |= RT_PT_RIGID_BLAS_INPUT_INVALID_INDEX_COUNT;
        }
        if (record.sourceRange.triangles.count <= 0 || record.sourceRange.triangles.count * 3 != record.sourceRange.indexes.count)
        {
            invalidFlags |= RT_PT_RIGID_BLAS_INPUT_INVALID_TRIANGLE_COUNT;
        }
        if (record.vertexFormat != expectedVertexFormat)
        {
            invalidFlags |= RT_PT_RIGID_BLAS_INPUT_INVALID_VERTEX_FORMAT;
        }
        if (record.vertexBufferIdentity == 0 || record.indexBufferIdentity == 0)
        {
            invalidFlags |= RT_PT_RIGID_BLAS_INPUT_INVALID_MISSING_SOURCE_IDENTITY;
        }
        if (record.materialId == 0)
        {
            invalidFlags |= RT_PT_RIGID_BLAS_INPUT_INVALID_MATERIAL;
        }

        ++stats.descriptors;
        ++stats.geometryDescs;
        stats.instances += instanceCount;
        stats.vertexCount += record.sourceRange.vertices.count;
        stats.indexCount += record.sourceRange.indexes.count;
        stats.triangleCount += record.sourceRange.triangles.count;
        stats.vertexBytes += record.sourceRange.vertices.count * static_cast<int>(sizeof(PathTraceSmokeVertex));
        stats.indexBytes += record.sourceRange.indexes.count * static_cast<int>(sizeof(uint32_t));
        if (invalidFlags == 0)
        {
            ++stats.validDescriptors;
        }
        else
        {
            ++stats.invalidDescriptors;
            if ((invalidFlags & RT_PT_RIGID_BLAS_INPUT_INVALID_NULL_TRI) != 0)
            {
                ++stats.nullTri;
            }
            if ((invalidFlags & RT_PT_RIGID_BLAS_INPUT_INVALID_VERTEX_COUNT) != 0)
            {
                ++stats.invalidVertexCount;
            }
            if ((invalidFlags & RT_PT_RIGID_BLAS_INPUT_INVALID_INDEX_COUNT) != 0)
            {
                ++stats.invalidIndexCount;
            }
            if ((invalidFlags & RT_PT_RIGID_BLAS_INPUT_INVALID_TRIANGLE_COUNT) != 0)
            {
                ++stats.invalidTriangleCount;
            }
            if ((invalidFlags & RT_PT_RIGID_BLAS_INPUT_INVALID_VERTEX_FORMAT) != 0)
            {
                ++stats.invalidVertexFormat;
            }
            if ((invalidFlags & RT_PT_RIGID_BLAS_INPUT_INVALID_MISSING_SOURCE_IDENTITY) != 0)
            {
                ++stats.missingSourceIdentity;
            }
            if ((invalidFlags & RT_PT_RIGID_BLAS_INPUT_INVALID_MATERIAL) != 0)
            {
                ++stats.invalidMaterial;
            }
        }

        if (stats.sampleCount < RT_PT_RIGID_BLAS_INPUT_SAMPLES)
        {
            RtPathTraceRigidBlasInputSample& sample = stats.samples[stats.sampleCount++];
            sample.valid = true;
            sample.meshHash = record.meshHash;
            sample.triIdentity = reinterpret_cast<uintptr_t>(record.tri);
            sample.vertexBufferIdentity = record.vertexBufferIdentity;
            sample.indexBufferIdentity = record.indexBufferIdentity;
            sample.materialId = record.materialId;
            sample.invalidFlags = invalidFlags;
            sample.vertexCount = record.sourceRange.vertices.count;
            sample.indexCount = record.sourceRange.indexes.count;
            sample.triangleCount = record.sourceRange.triangles.count;
            sample.vertexStride = static_cast<int>(sizeof(PathTraceSmokeVertex));
            sample.vertexOffsetBytes = record.sourceRange.vertices.offset * static_cast<int>(sizeof(PathTraceSmokeVertex));
            sample.indexOffsetBytes = record.sourceRange.indexes.offset * static_cast<int>(sizeof(uint32_t));
            sample.instanceCount = instanceCount;
            sample.materialName = record.materialName;
            sample.modelName = record.modelName;
        }
    }

    return stats;
}

void RtSmokeGeometryUniverse::DumpRigidBlasInputStats(const RtPathTraceRigidBlasInputStats& stats, int sceneSource) const
{
    common->Printf("PathTracePrimaryPass: PT rigid BLAS inputs source=%d frame=%llu generation=%llu descriptors=%d valid=%d invalid=%d geometryDescs=%d instances=%d verts/indexes/tris=%d/%d/%d bytes(v/i)=%d/%d vertexFormat=RGB32_FLOAT indexFormat=R32_UINT vertexStride=%d cpuDescriptorsOnly=1 gpuBuild=0 renderPath=dynamicFallback\n",
        sceneSource,
        static_cast<unsigned long long>(stats.frameIndex),
        static_cast<unsigned long long>(stats.generation),
        stats.descriptors,
        stats.validDescriptors,
        stats.invalidDescriptors,
        stats.geometryDescs,
        stats.instances,
        stats.vertexCount,
        stats.indexCount,
        stats.triangleCount,
        stats.vertexBytes,
        stats.indexBytes,
        static_cast<int>(sizeof(PathTraceSmokeVertex)));
    common->Printf("PathTracePrimaryPass: PT rigid BLAS input invalids nullTri/verts/indexes/tris/format/source/material=%d/%d/%d/%d/%d/%d/%d\n",
        stats.nullTri,
        stats.invalidVertexCount,
        stats.invalidIndexCount,
        stats.invalidTriangleCount,
        stats.invalidVertexFormat,
        stats.missingSourceIdentity,
        stats.invalidMaterial);

    for (int sampleIndex = 0; sampleIndex < stats.sampleCount; ++sampleIndex)
    {
        const RtPathTraceRigidBlasInputSample& sample = stats.samples[sampleIndex];
        if (!sample.valid)
        {
            continue;
        }
        common->Printf("PathTracePrimaryPass: PT rigid BLAS input sample %d mesh=%llu instances=%d tri=%llu vb=%llu ib=%llu invalidFlags=0x%x vertexOffsetBytes=%d indexOffsetBytes=%d vertexStride=%d verts=%d indexes=%d tris=%d material=%u '%s' model='%s'\n",
            sampleIndex,
            static_cast<unsigned long long>(sample.meshHash),
            sample.instanceCount,
            static_cast<unsigned long long>(sample.triIdentity),
            static_cast<unsigned long long>(sample.vertexBufferIdentity),
            static_cast<unsigned long long>(sample.indexBufferIdentity),
            sample.invalidFlags,
            sample.vertexOffsetBytes,
            sample.indexOffsetBytes,
            sample.vertexStride,
            sample.vertexCount,
            sample.indexCount,
            sample.triangleCount,
            sample.materialId,
            sample.materialName.c_str(),
            sample.modelName.c_str());
    }
}

RtPathTraceRigidBlasGpuStats RtSmokeGeometryUniverse::UpdateRigidBlasGpuScaffold(nvrhi::IDevice* device, nvrhi::ICommandList* commandList, bool submitBuilds)
{
    RtPathTraceRigidBlasGpuStats stats;
    stats.frameIndex = m_currentFrameIndex;
    stats.generation = m_generation;

    if (!device)
    {
        ++stats.skippedNoDevice;
        return stats;
    }
    if (!commandList)
    {
        ++stats.skippedNoCommandList;
        return stats;
    }

    std::vector<PathTraceSmokeVertex> localVertices;
    std::vector<uint32_t> localIndexes;
    const bool forceRebuild = r_pathTracingRigidBlasGpuForceRebuild.GetInteger() != 0;
    const bool prepareCachedRouteRecords =
        r_pathTracingGeometryResidencyV2.GetInteger() != 0 &&
        r_pathTracingResidencyRouteCached.GetInteger() != 0;
    const uint64 cachedRouteFramesToKeep = prepareCachedRouteRecords
        ? static_cast<uint64>(idMath::ClampInt(0, 100000, r_pathTracingResidencyMeshFramesToKeep.GetInteger()))
        : 0ull;
    std::unordered_set<uint64> cachedRouteResidentMeshHashes;
    if (prepareCachedRouteRecords)
    {
        cachedRouteResidentMeshHashes.reserve(m_rigidResidentRecords.size());
        for (const RigidResidentInstanceRecord& residentRecord : m_rigidResidentRecords)
        {
            if (residentRecord.observation.meshHash != 0)
            {
                cachedRouteResidentMeshHashes.insert(residentRecord.observation.meshHash);
            }
        }
    }

    for (RigidMeshCandidateRecord& record : m_rigidMeshCandidateRecords)
    {
        const bool cachedRouteWithinKeepWindow =
            prepareCachedRouteRecords &&
            record.lastSeenFrame + cachedRouteFramesToKeep >= m_currentFrameIndex;
        const bool cachedRouteCandidate =
            prepareCachedRouteRecords &&
            !record.seenThisFrame &&
            cachedRouteResidentMeshHashes.find(record.meshHash) != cachedRouteResidentMeshHashes.end() &&
            cachedRouteWithinKeepWindow &&
            RigidMeshHasCachedRouteData(record);
        if (!record.valid || (!record.seenThisFrame && !cachedRouteCandidate))
        {
            continue;
        }

        ++stats.meshRecords;
        const int instanceCount = Max(1, record.instanceCountThisFrame);
        stats.instances += instanceCount;
        stats.vertexCount += record.sourceRange.vertices.count;
        stats.indexCount += record.sourceRange.indexes.count;
        stats.triangleCount += record.sourceRange.triangles.count;
        const int vertexBytes = record.sourceRange.vertices.count * static_cast<int>(sizeof(PathTraceSmokeVertex));
        const int indexBytes = record.sourceRange.indexes.count * static_cast<int>(sizeof(uint32_t));
        stats.vertexBytes += vertexBytes;
        stats.indexBytes += indexBytes;

        uint32_t invalidFlags = ValidateRigidBlasInputRecord(record);
        if (invalidFlags != 0)
        {
            ++stats.invalidInputs;
            ++stats.skippedInvalid;
            continue;
        }

        localVertices.clear();
        localIndexes.clear();
        if (!BuildRigidLocalMeshData(record, localVertices, localIndexes))
        {
            invalidFlags |= RT_PT_RIGID_BLAS_INPUT_INVALID_INDEX_COUNT;
            ++stats.invalidInputs;
            ++stats.skippedInvalid;
            continue;
        }

        ++stats.validInputs;
        const size_t requiredVertexBytes = localVertices.size() * sizeof(PathTraceSmokeVertex);
        const size_t requiredIndexBytes = localIndexes.size() * sizeof(uint32_t);
        bool createdVertexBuffer = false;
        bool createdIndexBuffer = false;
        if (RigidSmokeBufferHasCapacity(record.rigidVertexBuffer, requiredVertexBytes, sizeof(PathTraceSmokeVertex)))
        {
            ++stats.vertexBuffersReused;
        }
        else
        {
            record.rigidVertexBuffer = CreateRigidSmokeBuffer(device, "PathTraceRigidMeshLocalVertices", requiredVertexBytes, sizeof(PathTraceSmokeVertex), true, false);
            record.gpuBuffersUploaded = false;
            createdVertexBuffer = record.rigidVertexBuffer != nullptr;
            if (createdVertexBuffer)
            {
                ++stats.vertexBuffersCreated;
            }
        }

        if (RigidSmokeBufferHasCapacity(record.rigidIndexBuffer, requiredIndexBytes, sizeof(uint32_t)))
        {
            ++stats.indexBuffersReused;
        }
        else
        {
            record.rigidIndexBuffer = CreateRigidSmokeBuffer(device, "PathTraceRigidMeshLocalIndices", requiredIndexBytes, sizeof(uint32_t), false, true);
            record.gpuBuffersUploaded = false;
            createdIndexBuffer = record.rigidIndexBuffer != nullptr;
            if (createdIndexBuffer)
            {
                ++stats.indexBuffersCreated;
            }
        }

        if (!record.rigidVertexBuffer || !record.rigidIndexBuffer)
        {
            ++stats.skippedInvalid;
            continue;
        }

        const uint64 uploadSignature = BuildRigidGpuUploadSignature(record);
        const bool uploadRequired = createdVertexBuffer || createdIndexBuffer || !record.gpuBuffersUploaded || record.gpuUploadSignature != uploadSignature;
        if (uploadRequired)
        {
            commandList->beginTrackingBufferState(record.rigidVertexBuffer, nvrhi::ResourceStates::Common);
            commandList->beginTrackingBufferState(record.rigidIndexBuffer, nvrhi::ResourceStates::Common);
            commandList->writeBuffer(record.rigidVertexBuffer, localVertices.data(), requiredVertexBytes);
            commandList->writeBuffer(record.rigidIndexBuffer, localIndexes.data(), requiredIndexBytes);
            commandList->setBufferState(record.rigidVertexBuffer, nvrhi::ResourceStates::AccelStructBuildInput);
            commandList->setBufferState(record.rigidIndexBuffer, nvrhi::ResourceStates::AccelStructBuildInput);
            commandList->commitBarriers();
            record.gpuUploadSignature = uploadSignature;
            record.gpuBuffersUploaded = true;
            ++stats.vertexUploads;
            ++stats.indexUploads;
            stats.uploadBytes += static_cast<int>(requiredVertexBytes + requiredIndexBytes);
        }

        bool builtThisFrame = false;
        const bool blasInputsCompatible =
            record.rigidBlas &&
            record.gpuBlasVertexCount == static_cast<int>(localVertices.size()) &&
            record.gpuBlasIndexCount == static_cast<int>(localIndexes.size());
        RtSmokeRigidBlasBuildPlanInput buildPlanInput;
        buildPlanInput.submitBuilds = submitBuilds;
        buildPlanInput.forceRebuild = forceRebuild;
        buildPlanInput.hasBlas = record.rigidBlas != nullptr;
        buildPlanInput.uploadRequired = uploadRequired;
        buildPlanInput.blasInputsCompatible = blasInputsCompatible;
        const RtSmokeRigidBlasBuildPlan buildPlan = BuildSmokeRigidBlasBuildPlan(buildPlanInput);
        if (buildPlan.createBlas)
        {
            if (record.rigidBlas)
            {
                RetireRigidBlas(record);
                ++stats.blasRecreatedForInputChange;
            }

            RtSmokeBlasCreateDesc blasCreateDesc;
            blasCreateDesc.device = device;
            blasCreateDesc.vertexBuffer = record.rigidVertexBuffer;
            blasCreateDesc.indexBuffer = record.rigidIndexBuffer;
            blasCreateDesc.vertexCount = static_cast<int>(localVertices.size());
            blasCreateDesc.indexCount = static_cast<int>(localIndexes.size());
            blasCreateDesc.debugName = "PathTraceRigidMeshLocalBLAS";
            const RtSmokeBlasCreateResult blasCreateResult = CreateSmokeBlas(blasCreateDesc);
            if (blasCreateResult.Succeeded())
            {
                record.rigidBlasDesc = blasCreateResult.accelStructDesc;
                record.rigidBlas = blasCreateResult.accelStruct;
                record.gpuBlasCreated = true;
                record.gpuBlasBuildSubmitted = false;
                record.gpuBlasVertexCount = static_cast<int>(localVertices.size());
                record.gpuBlasIndexCount = static_cast<int>(localIndexes.size());
                ++stats.blasHandlesCreated;
            }
            else
            {
                ++stats.blasBuildsSkipped;
            }
        }
        else if (record.rigidBlas)
        {
            ++stats.blasHandlesReused;
        }

        if (buildPlan.submitBuild && record.rigidBlas)
        {
            nvrhi::utils::BuildBottomLevelAccelStruct(commandList, record.rigidBlas, record.rigidBlasDesc);
            record.gpuBlasBuildSubmitted = true;
            ++stats.blasBuildsSubmitted;
            builtThisFrame = true;
        }
        else if (buildPlan.skipBuild)
        {
            ++stats.blasBuildsSkipped;
            if (!submitBuilds)
            {
                ++stats.buildGateOff;
            }
            else if (record.rigidBlas && !uploadRequired && !forceRebuild && blasInputsCompatible)
            {
                ++stats.blasBuildsSkippedUnchanged;
            }
        }

        if (stats.sampleCount < RT_PT_RIGID_BLAS_GPU_SAMPLES)
        {
            RtPathTraceRigidBlasGpuSample& sample = stats.samples[stats.sampleCount++];
            sample.valid = true;
            sample.meshHash = record.meshHash;
            sample.triIdentity = reinterpret_cast<uintptr_t>(record.tri);
            sample.vertexBufferIdentity = record.vertexBufferIdentity;
            sample.indexBufferIdentity = record.indexBufferIdentity;
            sample.materialId = record.materialId;
            sample.invalidFlags = invalidFlags;
            sample.vertexCount = record.sourceRange.vertices.count;
            sample.indexCount = record.sourceRange.indexes.count;
            sample.triangleCount = record.sourceRange.triangles.count;
            sample.vertexBytes = vertexBytes;
            sample.indexBytes = indexBytes;
            sample.instanceCount = instanceCount;
            sample.vertexBufferValid = record.rigidVertexBuffer != nullptr;
            sample.indexBufferValid = record.rigidIndexBuffer != nullptr;
            sample.blasValid = record.rigidBlas != nullptr;
            sample.uploadedThisFrame = uploadRequired;
            sample.builtThisFrame = builtThisFrame;
            sample.materialName = record.materialName;
            sample.modelName = record.modelName;
        }
    }

    return stats;
}

void RtSmokeGeometryUniverse::ReleaseRigidBlasGpuScaffold()
{
    for (RigidMeshCandidateRecord& record : m_rigidMeshCandidateRecords)
    {
        RetireRigidBlas(record);
        record.rigidVertexBuffer = nullptr;
        record.rigidIndexBuffer = nullptr;
        record.gpuUploadSignature = 0;
        record.gpuBuffersUploaded = false;
    }
}

void RtSmokeGeometryUniverse::ClearRetiredRigidBlas()
{
    m_retiredRigidBlasRecords.clear();
}

void RtSmokeGeometryUniverse::DumpRigidBlasGpuStats(const RtPathTraceRigidBlasGpuStats& stats, int sceneSource, bool scaffoldEnabled, bool submitBuilds) const
{
    common->Printf("PathTracePrimaryPass: PT rigid BLAS GPU scaffold source=%d frame=%llu generation=%llu scaffold=%d build=%d forceRebuild=%d meshRecords=%d retiredPending=%d valid=%d invalid=%d instances=%d verts/indexes/tris=%d/%d/%d bytes(v/i/upload)=%d/%d/%d buffers(v create/reuse uploads i create/reuse uploads)=%d/%d/%d %d/%d/%d blas(handles create/reuse builds/skips unchanged/recreated)=%d/%d/%d/%d/%d/%d skips noDevice/noCmd/invalid=%d/%d/%d renderPath=dynamicFallback tlasRoute=rigidResidencyRoute\n",
        sceneSource,
        static_cast<unsigned long long>(stats.frameIndex),
        static_cast<unsigned long long>(stats.generation),
        scaffoldEnabled ? 1 : 0,
        submitBuilds ? 1 : 0,
        r_pathTracingRigidBlasGpuForceRebuild.GetInteger() != 0 ? 1 : 0,
        stats.meshRecords,
        static_cast<int>(m_retiredRigidBlasRecords.size()),
        stats.validInputs,
        stats.invalidInputs,
        stats.instances,
        stats.vertexCount,
        stats.indexCount,
        stats.triangleCount,
        stats.vertexBytes,
        stats.indexBytes,
        stats.uploadBytes,
        stats.vertexBuffersCreated,
        stats.vertexBuffersReused,
        stats.vertexUploads,
        stats.indexBuffersCreated,
        stats.indexBuffersReused,
        stats.indexUploads,
        stats.blasHandlesCreated,
        stats.blasHandlesReused,
        stats.blasBuildsSubmitted,
        stats.blasBuildsSkipped,
        stats.blasBuildsSkippedUnchanged,
        stats.blasRecreatedForInputChange,
        stats.skippedNoDevice,
        stats.skippedNoCommandList,
        stats.skippedInvalid);

    for (int sampleIndex = 0; sampleIndex < stats.sampleCount; ++sampleIndex)
    {
        const RtPathTraceRigidBlasGpuSample& sample = stats.samples[sampleIndex];
        if (!sample.valid)
        {
            continue;
        }
        common->Printf("PathTracePrimaryPass: PT rigid BLAS GPU sample %d mesh=%llu instances=%d tri=%llu vb=%llu ib=%llu invalidFlags=0x%x verts=%d indexes=%d tris=%d bytes=%d/%d gpu(v/i/blas)=%d/%d/%d uploaded=%d built=%d material=%u '%s' model='%s'\n",
            sampleIndex,
            static_cast<unsigned long long>(sample.meshHash),
            sample.instanceCount,
            static_cast<unsigned long long>(sample.triIdentity),
            static_cast<unsigned long long>(sample.vertexBufferIdentity),
            static_cast<unsigned long long>(sample.indexBufferIdentity),
            sample.invalidFlags,
            sample.vertexCount,
            sample.indexCount,
            sample.triangleCount,
            sample.vertexBytes,
            sample.indexBytes,
            sample.vertexBufferValid ? 1 : 0,
            sample.indexBufferValid ? 1 : 0,
            sample.blasValid ? 1 : 0,
            sample.uploadedThisFrame ? 1 : 0,
            sample.builtThisFrame ? 1 : 0,
            sample.materialId,
            sample.materialName.c_str(),
            sample.modelName.c_str());
    }
}

RtPathTraceRigidResidencyStats RtSmokeGeometryUniverse::UpdateRigidResidency(
    const viewDef_t* viewDef,
    const RtPathTraceInstanceUniverse& instanceUniverse,
    bool enabled,
    int portalSteps)
{
    m_rigidResidencyEnabled = enabled;
    m_rigidResidentFrameInstances.clear();
    m_rigidResidencyStats = RtPathTraceRigidResidencyStats();
    m_rigidResidencyStats.enabled = enabled ? 1 : 0;
    m_rigidResidencyStats.residencyV2 = r_pathTracingGeometryResidencyV2.GetInteger() != 0 ? 1 : 0;
    m_rigidResidencyStats.frameIndex = m_currentFrameIndex;
    m_rigidResidencyStats.generation = m_generation;
    m_rigidResidencyStats.portalSteps = idMath::ClampInt(0, 8, portalSteps);
    m_rigidResidencyStats.areaWalkEntities = m_rigidResidencyAreaWalkEntitiesThisFrame;
    m_rigidResidencyStats.areaWalkRejectedEntities = m_rigidResidencyAreaWalkRejectedEntitiesThisFrame;
    m_rigidResidencyStats.areaWalkSurfaces = m_rigidResidencyAreaWalkSurfacesThisFrame;
    m_rigidResidencyStats.areaWalkRejectedSurfaces = m_rigidResidencyAreaWalkRejectedSurfacesThisFrame;
    m_rigidResidencyStats.areaWalkEligibleSurfaces = m_rigidResidencyAreaWalkEligibleSurfacesThisFrame;
    m_rigidResidencyStats.areaWalkDuplicateVisible = m_rigidResidencyAreaWalkDuplicateVisibleThisFrame;
    m_rigidResidencyStats.areaWalkDuplicateFrame = m_rigidResidencyAreaWalkDuplicateFrameThisFrame;
    m_rigidResidencyStats.areaWalkRigidInstances = m_rigidResidencyAreaWalkInstancesThisFrame;

    idRenderWorldLocal* renderWorld = viewDef ? viewDef->renderWorld : nullptr;
    m_rigidResidencyStats.totalAreas = renderWorld ? renderWorld->NumAreas() : 0;

    int portalEdges = 0;
    int blockedPortalEdges = 0;
    int currentArea = -1;
    const std::vector<bool> selectedAreas = BuildRigidResidencySelectedAreas(viewDef, m_rigidResidencyStats.portalSteps, &portalEdges, &blockedPortalEdges, &currentArea);
    m_rigidResidencyStats.currentArea = currentArea;
    m_rigidResidencyStats.portalEdges = portalEdges;
    m_rigidResidencyStats.blockedPortalEdges = blockedPortalEdges;
    m_rigidResidencyStats.selectedAreas = CountRigidResidencySelectedAreas(selectedAreas);

    const std::vector<RtPathTraceInstanceObservation>& visibleInstances = instanceUniverse.FrameInstances();
    for (const RtPathTraceInstanceObservation& instance : visibleInstances)
    {
        const RtPathTraceResidencyClass residencyClass = RtPathTraceResidencyClassForSourceFlags(instance.sourceFlags);
        switch (residencyClass)
        {
            case RtPathTraceResidencyClass::StaticWorld:
                ++m_rigidResidencyStats.visibleStaticWorldInstances;
                break;
            case RtPathTraceResidencyClass::DurableRigid:
                break;
            case RtPathTraceResidencyClass::DynamicFrame:
                ++m_rigidResidencyStats.visibleDynamicFrameInstances;
                break;
            case RtPathTraceResidencyClass::TransientEffect:
                ++m_rigidResidencyStats.visibleTransientEffectInstances;
                break;
            default:
                ++m_rigidResidencyStats.visibleUnknownResidencyInstances;
                break;
        }

        if (residencyClass != RtPathTraceResidencyClass::DurableRigid)
        {
            if ((instance.sourceFlags & RT_PT_INSTANCE_SOURCE_RIGID) != 0)
            {
                switch (residencyClass)
                {
                    case RtPathTraceResidencyClass::StaticWorld:
                        ++m_rigidResidencyStats.rejectedRigidStaticWorld;
                        break;
                    case RtPathTraceResidencyClass::DynamicFrame:
                        ++m_rigidResidencyStats.rejectedRigidDynamicFrame;
                        break;
                    case RtPathTraceResidencyClass::TransientEffect:
                        ++m_rigidResidencyStats.rejectedRigidTransientEffect;
                        break;
                    default:
                        ++m_rigidResidencyStats.rejectedRigidUnknown;
                        break;
                }
            }
            continue;
        }
        ++m_rigidResidencyStats.visibleRigidInstances;
        if (instance.instanceId == 0)
        {
            continue;
        }

        const RtPathTraceRigidRouteInstanceObservation routeInstance = MakeRigidRouteInstanceObservation(instance);
        if (!RigidResidentObservationMatchesCurrentModel(routeInstance))
        {
            ++m_rigidResidencyStats.visibleRigidStaleModel;
            continue;
        }

        RecordRigidResidentObservation(routeInstance);
    }

    const idRenderMatrix* viewMvp = viewDef ? &viewDef->worldSpace.mvp : nullptr;
    const idVec3* viewOrigin = viewDef ? &viewDef->renderView.vieworg : nullptr;
    PruneRigidCachesToCurrentFrame(renderWorld, viewMvp, viewOrigin, selectedAreas);
    m_rigidResidencyStats.cachedRigidInstances = static_cast<int>(m_rigidResidentRecords.size());
    if (!enabled)
    {
        return m_rigidResidencyStats;
    }

    const bool v2 = r_pathTracingGeometryResidencyV2.GetInteger() != 0;
    for (const RigidResidentInstanceRecord& residentRecord : m_rigidResidentRecords)
    {
        if (!v2 && !residentRecord.seenThisFrame)
        {
            continue;
        }

        const RtPathTraceRigidRouteInstanceObservation& instance = residentRecord.observation;
        const bool retainedFromCache = !residentRecord.seenThisFrame;
        const bool entityFeedOwned =
            r_pathTracingEntityFeed.GetInteger() != 0 &&
            (instance.sourceFlags & RT_PT_INSTANCE_SOURCE_ENTITY_FEED) != 0;
        if (!entityFeedOwned && !retainedFromCache && instance.currentArea < 0)
        {
            ++m_rigidResidencyStats.skippedUnknownArea;
        }
        const bool selectedArea = entityFeedOwned || RigidResidencyAreaSelected(instance.currentArea, selectedAreas);
        if (!entityFeedOwned && !retainedFromCache && instance.currentArea >= 0 && !selectedArea)
        {
            ++m_rigidResidencyStats.skippedOutsideArea;
            continue;
        }

        const std::unordered_map<uint64, size_t>::const_iterator meshIt = m_rigidMeshCandidateLookup.find(instance.meshHash);
        const RigidMeshCandidateRecord* meshRecord = nullptr;
        if (meshIt != m_rigidMeshCandidateLookup.end() && meshIt->second < m_rigidMeshCandidateRecords.size())
        {
            meshRecord = &m_rigidMeshCandidateRecords[meshIt->second];
        }
        const bool hasMesh = meshRecord && meshRecord->valid;
        const bool routeReady = hasMesh && meshRecord->rigidBlas;

        ++m_rigidResidencyStats.residentInstances;
        if (residentRecord.seenThisFrame)
        {
            ++m_rigidResidencyStats.residentSeenThisFrame;
        }
        else
        {
            ++m_rigidResidencyStats.residentFromCache;
        }
        if (!hasMesh)
        {
            ++m_rigidResidencyStats.residentMissingMesh;
        }
        else if (!meshRecord->rigidBlas)
        {
            ++m_rigidResidencyStats.residentMissingBlas;
        }
        else
        {
            ++m_rigidResidencyStats.residentRouteReady;
        }

        RtPathTraceRigidRouteInstanceObservation residentFrameInstance = instance;
        residentFrameInstance.seenThisFrame = residentRecord.seenThisFrame;
        const bool cachedMaterialOverrideCompatible =
            (instance.sourceFlags & RT_PT_INSTANCE_SOURCE_MATERIAL_OVERRIDE) == 0 ||
            (meshRecord &&
                instance.materialOverrideId != 0 &&
                meshRecord->materialId == instance.materialOverrideId);
        bool emitRouteInstance = residentRecord.seenThisFrame;
        if (!emitRouteInstance &&
            v2 &&
            r_pathTracingResidencyRouteCached.GetInteger() != 0 &&
            meshRecord &&
            RigidMeshHasCachedRouteGpuReady(*meshRecord) &&
            cachedMaterialOverrideCompatible)
        {
            emitRouteInstance = true;
        }
        if (emitRouteInstance)
        {
            m_rigidResidentFrameInstances.push_back(residentFrameInstance);
        }
        AddRigidResidencySample(residentRecord, selectedArea, routeReady);
    }

    const int rigidRouteMaxInstances = idMath::ClampInt(1, 510, r_pathTracingRigidRouteMaxInstances.GetInteger());
    if (static_cast<int>(m_rigidResidentFrameInstances.size()) > rigidRouteMaxInstances)
    {
        std::partial_sort(
            m_rigidResidentFrameInstances.begin(),
            m_rigidResidentFrameInstances.begin() + rigidRouteMaxInstances,
            m_rigidResidentFrameInstances.end(),
            RigidRouteInstancePriorityLess);
        m_rigidResidentFrameInstances.resize(rigidRouteMaxInstances);
    }

    return m_rigidResidencyStats;
}

void RtSmokeGeometryUniverse::RefreshRigidResidencyAreaWalk(const viewDef_t* viewDef, const RtPathTraceInstanceUniverse& instanceUniverse, int portalSteps, bool recordResidents, RtSmokeMaterialStats* materialStats)
{
    if (!m_frameActive || !viewDef || !viewDef->renderWorld)
    {
        return;
    }

    idRenderWorldLocal* renderWorld = viewDef->renderWorld;
    const std::vector<bool> selectedAreas = BuildRigidResidencySelectedAreas(viewDef, portalSteps, nullptr, nullptr, nullptr);
    if (selectedAreas.empty())
    {
        return;
    }

    std::unordered_set<uint64> visibleRigidInstanceIds;
    const std::vector<RtPathTraceInstanceObservation>& visibleInstances = instanceUniverse.FrameInstances();
    for (const RtPathTraceInstanceObservation& instance : visibleInstances)
    {
        if (!RtPathTraceSourceFlagsAreDurableRigid(instance.sourceFlags))
        {
            continue;
        }
        if (instance.instanceId != 0)
        {
            visibleRigidInstanceIds.insert(instance.instanceId);
        }
    }

    std::unordered_set<uint64> observedInstanceIds;
    for (int areaIndex = 0; areaIndex < static_cast<int>(selectedAreas.size()); ++areaIndex)
    {
        if (!selectedAreas[areaIndex])
        {
            continue;
        }
        if (areaIndex < 0 || areaIndex >= renderWorld->numPortalAreas)
        {
            continue;
        }

        portalArea_t* area = &renderWorld->portalAreas[areaIndex];
        for (areaReference_t* ref = area->entityRefs.areaNext; ref != &area->entityRefs; ref = ref->areaNext)
        {
            idRenderEntityLocal* entity = ref ? ref->entity : nullptr;
            ++m_rigidResidencyAreaWalkEntitiesThisFrame;
            if (!RigidResidencyCanTrackEntity(viewDef, entity))
            {
                ++m_rigidResidencyAreaWalkRejectedEntitiesThisFrame;
                continue;
            }

            const renderEntity_t& renderEntity = entity->parms;
            const idRenderModel* model = renderEntity.hModel;
            for (int surfaceIndex = 0; surfaceIndex < model->NumSurfaces(); ++surfaceIndex)
            {
                const modelSurface_t* surface = model->Surface(surfaceIndex);
                const srfTriangles_t* tri = surface ? surface->geometry : nullptr;
                const idMaterial* surfaceMaterial = surface ? surface->shader : nullptr;
                const idMaterial* material = R_RemapShaderBySkin(surfaceMaterial, renderEntity.customSkin, renderEntity.customShader);
                ++m_rigidResidencyAreaWalkSurfacesThisFrame;
                if (!RigidResidencyCanTrackSurface(entity, tri, material))
                {
                    ++m_rigidResidencyAreaWalkRejectedSurfacesThisFrame;
                    continue;
                }

                const uint32_t baseMaterialId = SmokeMaterialId(material);
                const uint32_t materialId = SmokeRuntimeMaterialTableIdForEntitySurface(entity, surfaceIndex, material, baseMaterialId);
                const uint32_t rigidSurfaceClassId = SmokeSurfaceClassId(RtSmokeSurfaceClass::RigidEntity);
                const uint32_t rigidTriangleClassAndFlags = rigidSurfaceClassId |
                    (SmokeEntitySurfaceHasActiveEmissiveStage(viewDef, entity, material) ? 0u : RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF);
                if (materialStats)
                {
                    SceneUniverseAddDynamicMaterialEvalStatsForId(*materialStats, viewDef, entity, material, tri->numIndexes, materialId);
                }
                const uint32_t materialClassSignature = SmokeMaterialRouteClassSignature(material, RtSmokeSurfaceClass::RigidEntity, RtSmokeTranslucentSubtype::Unknown);
                RtPathTraceMeshKey meshKey;
                meshKey.tri = tri;
                meshKey.vertexBufferIdentity = static_cast<uintptr_t>(tri ? tri->ambientCache : 0);
                meshKey.indexBufferIdentity = static_cast<uintptr_t>(tri ? tri->indexCache : 0);
                meshKey.numVerts = tri ? tri->numVerts : 0;
                meshKey.numIndexes = tri ? tri->numIndexes : 0;
                meshKey.vertexFormat = static_cast<uint32_t>(RtSmokeGeometryBufferFormat::LegacySmokeVertex);
                meshKey.materialId = materialId;
                meshKey.materialClassSignature = materialClassSignature;
                meshKey.sourceKind = rigidSurfaceClassId;
                const PtRenderDefKey renderDefKey = PtGeometryLifecycle::MakeEntityKey(entity);
                const uint32_t modelEpoch = PtGeometryLifecycle::EntityModelEpoch(renderDefKey.world, renderDefKey.index);
                uint32_t sourceFlags = RT_PT_INSTANCE_SOURCE_RIGID;
                if (renderEntity.customShader != nullptr || renderEntity.customSkin != nullptr)
                {
                    sourceFlags |= RT_PT_INSTANCE_SOURCE_MATERIAL_OVERRIDE;
                }
                const RtPathTraceRigidInstanceSnapshot rigidSnapshot = BuildPathTraceRigidInstanceSnapshot(
                    meshKey,
                    model,
                    tri,
                    renderDefKey,
                    modelEpoch,
                    entity->index,
                    renderEntity.entityNum,
                    surfaceIndex,
                    sourceFlags);
                const uint64 meshHash = rigidSnapshot.meshHash;
                const uint64 instanceId = rigidSnapshot.instanceId;
                ++m_rigidResidencyAreaWalkEligibleSurfacesThisFrame;
                if (visibleRigidInstanceIds.find(instanceId) != visibleRigidInstanceIds.end())
                {
                    ++m_rigidResidencyAreaWalkDuplicateVisibleThisFrame;
                    continue;
                }
                if (observedInstanceIds.find(instanceId) != observedInstanceIds.end())
                {
                    ++m_rigidResidencyAreaWalkDuplicateFrameThisFrame;
                    continue;
                }
                observedInstanceIds.insert(instanceId);

                if (recordResidents)
                {
                    RtPathTraceRigidMeshCandidateObservation candidateObservation;
                    candidateObservation.tri = tri;
                    candidateObservation.meshHash = meshHash;
                    candidateObservation.instanceId = instanceId;
                    candidateObservation.vertexBufferIdentity = meshKey.vertexBufferIdentity;
                    candidateObservation.indexBufferIdentity = meshKey.indexBufferIdentity;
                    candidateObservation.sourceFlags = rigidSnapshot.sourceFlags;
                    candidateObservation.materialId = rigidSnapshot.materialId;
                    candidateObservation.materialClassSignature = rigidSnapshot.materialClassSignature;
                    candidateObservation.surfaceClassId = rigidSurfaceClassId;
                    candidateObservation.triangleClassAndFlags = rigidTriangleClassAndFlags;
                    candidateObservation.vertexFormat = meshKey.vertexFormat;
                    candidateObservation.drawSurfIndex = -1;
                    candidateObservation.entityIndex = entity->index;
                    candidateObservation.renderEntityNum = renderEntity.entityNum;
                    candidateObservation.modelEpoch = rigidSnapshot.modelEpoch;
                    candidateObservation.jointIndex = rigidSnapshot.jointIndex;
                    candidateObservation.numVerts = tri->numVerts;
                    candidateObservation.numIndexes = tri->numIndexes;
                    candidateObservation.localSpaceValid = true;
                    BuildRigidNormalTexMatrix(material, material ? material->ConstantRegisters() : nullptr, candidateObservation.normalTexMatrix);
                    candidateObservation.materialName = material ? material->GetName() : "<none>";
                    candidateObservation.modelName = model ? model->Name() : "<none>";
                    RecordRigidMeshCandidate(candidateObservation);

                    RtPathTraceRigidRouteInstanceObservation residentInstance;
                    residentInstance.instanceId = instanceId;
                    residentInstance.meshHash = meshHash;
                    residentInstance.entityIndex = entity->index;
                    residentInstance.renderEntityNum = renderEntity.entityNum;
                    residentInstance.drawSurfIndex = -1;
                    residentInstance.modelSurfaceIndex = rigidSnapshot.modelSurfaceIndex;
                    residentInstance.jointIndex = rigidSnapshot.jointIndex;
                    residentInstance.currentArea = areaIndex;
                    residentInstance.renderDefKey = rigidSnapshot.renderDefKey;
                    residentInstance.modelEpoch = rigidSnapshot.modelEpoch;
                    residentInstance.materialOverrideId = rigidSnapshot.materialId;
                    residentInstance.triangleClassAndFlags = rigidTriangleClassAndFlags;
                    residentInstance.sourceFlags = rigidSnapshot.sourceFlags;
                    residentInstance.seenThisFrame = true;
                    residentInstance.wasMovingWhenLastSeen = RigidResidencyEntityMovedWithinGrace(entity);
                    residentInstance.isSkinnedOrDeforming = RigidRouteSourceFlagsDeforming(residentInstance.sourceFlags);
                    memcpy(residentInstance.objectToWorld, entity->modelMatrix, sizeof(residentInstance.objectToWorld));
                    residentInstance.materialName = candidateObservation.materialName;
                    residentInstance.modelName = candidateObservation.modelName;
                    RecordRigidResidentObservation(residentInstance);
                }
                ++m_rigidResidencyAreaWalkInstancesThisFrame;
            }
        }
    }
}

const RtPathTraceRigidResidencyStats& RtSmokeGeometryUniverse::GetRigidResidencyStats() const
{
    return m_rigidResidencyStats;
}

void RtSmokeGeometryUniverse::DumpRigidResidencyStats(const RtPathTraceRigidResidencyStats& stats, int sceneSource, bool includeSamples) const
{
    const char* routeSource = !stats.enabled
        ? "visibleOnly"
        : (stats.residencyV2 ? "residencyV2" : "legacyAreaWalk");
    common->Printf("PathTracePrimaryPass: PT rigid residency source=%d enabled=%d v2=%d frame=%llu generation=%llu currentArea=%d totalAreas=%d portalSteps=%d selectedAreas=%d edges/blocked=%d/%d residencyClass(static/durable/dynamic/transient/unknown)=%d/%d/%d/%d/%d visibleRigid/staleModel=%d/%d rejectedRigid(static/dynamic/transient/unknown)=%d/%d/%d/%d areaWalk(entity/reject/surface/reject/eligible/dupVisible/dupFrame/addOrWouldAdd)=%d/%d/%d/%d/%d/%d/%d/%d cachedRigid=%d resident=%d seen/cache=%d/%d retainedOffscreen=%d agedOut/deleted=%d/%d meshLive/agedOut=%d/%d retiredBlas=%d keep(instance/mesh/feedCap)=%d/%d/%d antiCull=%d routeReady=%d missing(mesh/blas)=%d/%d skippedOutside/routedUnknownArea=%d/%d routeSource=%s\n",
        sceneSource,
        stats.enabled,
        stats.residencyV2,
        static_cast<unsigned long long>(stats.frameIndex),
        static_cast<unsigned long long>(stats.generation),
        stats.currentArea,
        stats.totalAreas,
        stats.portalSteps,
        stats.selectedAreas,
        stats.portalEdges,
        stats.blockedPortalEdges,
        stats.visibleStaticWorldInstances,
        stats.visibleRigidInstances,
        stats.visibleDynamicFrameInstances,
        stats.visibleTransientEffectInstances,
        stats.visibleUnknownResidencyInstances,
        stats.visibleRigidInstances,
        stats.visibleRigidStaleModel,
        stats.rejectedRigidStaticWorld,
        stats.rejectedRigidDynamicFrame,
        stats.rejectedRigidTransientEffect,
        stats.rejectedRigidUnknown,
        stats.areaWalkEntities,
        stats.areaWalkRejectedEntities,
        stats.areaWalkSurfaces,
        stats.areaWalkRejectedSurfaces,
        stats.areaWalkEligibleSurfaces,
        stats.areaWalkDuplicateVisible,
        stats.areaWalkDuplicateFrame,
        stats.areaWalkRigidInstances,
        stats.cachedRigidInstances,
        stats.residentInstances,
        stats.residentSeenThisFrame,
        stats.residentFromCache,
        stats.residentRetainedOffscreen,
        stats.residentAgedOut,
        stats.residentDeleted,
        stats.meshLive,
        stats.meshAgedOut,
        stats.retiredBlasPending,
        stats.residencyFramesToKeep,
        stats.residencyMeshFramesToKeep,
        stats.residencyEntityFeedCap,
        stats.residencyAntiCulling,
        stats.residentRouteReady,
        stats.residentMissingMesh,
        stats.residentMissingBlas,
        stats.skippedOutsideArea,
        stats.skippedUnknownArea,
        routeSource);

    if (!includeSamples)
    {
        return;
    }

    for (int sampleIndex = 0; sampleIndex < stats.sampleCount; ++sampleIndex)
    {
        const RtPathTraceRigidResidencySample& sample = stats.samples[sampleIndex];
        if (!sample.valid)
        {
            continue;
        }

        common->Printf("PathTracePrimaryPass: PT rigid residency sample %d mesh=%llu instance=%llu area=%d selected=%d seen=%d routeReady=%d lastSeen=%d surf=%d entity=%d renderEntity=%d origin=(%.2f %.2f %.2f) material='%s' model='%s'\n",
            sampleIndex,
            static_cast<unsigned long long>(sample.meshHash),
            static_cast<unsigned long long>(sample.instanceId),
            sample.area,
            sample.selectedArea ? 1 : 0,
            sample.seenThisFrame ? 1 : 0,
            sample.routeReady ? 1 : 0,
            sample.lastSeenFrame,
            sample.drawSurfIndex,
            sample.entityIndex,
            sample.renderEntityNum,
            sample.origin.x,
            sample.origin.y,
            sample.origin.z,
            sample.materialName.c_str(),
            sample.modelName.c_str());
    }
}

void RtSmokeGeometryUniverse::CollectRigidResidencyBoundsBoxes(std::vector<RtPathTraceRigidResidencyBoundsBox>& boxes, int maxBoxes) const
{
    if (!m_rigidResidencyEnabled || maxBoxes <= 0)
    {
        return;
    }

    const uint64 framesToKeep = static_cast<uint64>(idMath::ClampInt(0, 100000, r_pathTracingResidencyFramesToKeep.GetInteger()));
    for (const RigidResidentInstanceRecord& residentRecord : m_rigidResidentRecords)
    {
        if (static_cast<int>(boxes.size()) >= maxBoxes)
        {
            break;
        }

        const RtPathTraceRigidRouteInstanceObservation& instance = residentRecord.observation;
        bool isResident = false;
        for (const RtPathTraceRigidRouteInstanceObservation& residentInstance : m_rigidResidentFrameInstances)
        {
            if (residentInstance.instanceId == instance.instanceId)
            {
                isResident = true;
                break;
            }
        }
        if (!isResident)
        {
            continue;
        }

        const std::unordered_map<uint64, size_t>::const_iterator meshIt = m_rigidMeshCandidateLookup.find(instance.meshHash);
        if (meshIt == m_rigidMeshCandidateLookup.end() || meshIt->second >= m_rigidMeshCandidateRecords.size())
        {
            continue;
        }
        const RigidMeshCandidateRecord& meshRecord = m_rigidMeshCandidateRecords[meshIt->second];
        if (!meshRecord.valid || !meshRecord.localBoundsValid || meshRecord.localBounds.IsCleared())
        {
            continue;
        }

        RtPathTraceRigidResidencyBoundsBox box;
        box.valid = true;
        box.seenThisFrame = residentRecord.seenThisFrame;
        box.retainedOffscreen = !residentRecord.seenThisFrame;
        box.aboutToAgeOut =
            box.retainedOffscreen &&
            framesToKeep > 0 &&
            residentRecord.lastSeenFrame + framesToKeep <= m_currentFrameIndex + 30;
        box.routeReady = meshRecord.rigidBlas != nullptr;
        box.missingBlas = meshRecord.rigidBlas == nullptr;
        box.area = instance.currentArea;
        box.color = RigidResidencyBoundsColor(box.seenThisFrame, box.retainedOffscreen, box.aboutToAgeOut, box.routeReady, box.missingBlas);

        for (int cornerIndex = 0; cornerIndex < 8; ++cornerIndex)
        {
            idVec3 localPoint;
            localPoint.x = meshRecord.localBounds[(cornerIndex ^ (cornerIndex >> 1)) & 1].x;
            localPoint.y = meshRecord.localBounds[(cornerIndex >> 1) & 1].y;
            localPoint.z = meshRecord.localBounds[(cornerIndex >> 2) & 1].z;
            TransformRigidResidencyBoundsPoint(instance.objectToWorld, localPoint, box.corners[cornerIndex]);
        }
        if (!ValidateRigidResidencyBoundsBox(box))
        {
            continue;
        }

        boxes.push_back(box);
    }
}

void RtSmokeGeometryUniverse::CollectStaticSurfaceBoundsBoxes(std::vector<RtPathTraceRigidResidencyBoundsBox>& boxes, int maxBoxes, bool cacheOnlyFirst) const
{
    if (maxBoxes <= 0)
    {
        return;
    }

    const auto appendStaticBox = [this, &boxes, maxBoxes](const RtSmokePersistentStaticSurfaceRecord& record) -> void
    {
        if (static_cast<int>(boxes.size()) >= maxBoxes)
        {
            return;
        }
        if (!record.valid || record.currentRange.vertices.offset < 0 || record.currentRange.vertices.count <= 0)
        {
            return;
        }

        const int vertexBegin = record.currentRange.vertices.offset;
        const int vertexEnd = vertexBegin + record.currentRange.vertices.count;
        if (vertexBegin < 0 || vertexEnd > static_cast<int>(m_staticVertexCache.size()))
        {
            return;
        }

        idVec3 mins = SmokeVertexPosition(m_staticVertexCache[vertexBegin]);
        idVec3 maxs = mins;
        bool validPosition = IsRigidResidencyBoundsPointFinite(mins);
        for (int vertexIndex = vertexBegin + 1; validPosition && vertexIndex < vertexEnd; ++vertexIndex)
        {
            const idVec3 position = SmokeVertexPosition(m_staticVertexCache[vertexIndex]);
            if (!IsRigidResidencyBoundsPointFinite(position))
            {
                validPosition = false;
                break;
            }
            mins.x = Min(mins.x, position.x);
            mins.y = Min(mins.y, position.y);
            mins.z = Min(mins.z, position.z);
            maxs.x = Max(maxs.x, position.x);
            maxs.y = Max(maxs.y, position.y);
            maxs.z = Max(maxs.z, position.z);
        }
        if (!validPosition)
        {
            return;
        }

        RtPathTraceRigidResidencyBoundsBox box;
        box.valid = true;
        box.seenThisFrame = record.seenThisFrame;
        box.routeReady = true;
        box.missingBlas = false;
        box.area = -1;
        box.color = StaticSurfaceBoundsColor(record.seenThisFrame);

        for (int cornerIndex = 0; cornerIndex < 8; ++cornerIndex)
        {
            box.corners[cornerIndex].x = ((cornerIndex ^ (cornerIndex >> 1)) & 1) != 0 ? maxs.x : mins.x;
            box.corners[cornerIndex].y = ((cornerIndex >> 1) & 1) != 0 ? maxs.y : mins.y;
            box.corners[cornerIndex].z = ((cornerIndex >> 2) & 1) != 0 ? maxs.z : mins.z;
        }
        if (!ValidateRigidResidencyBoundsBox(box))
        {
            return;
        }

        boxes.push_back(box);
    };

    const auto appendMatchingStaticBoxes = [this, &boxes, maxBoxes, &appendStaticBox](bool seenThisFrame) -> bool
    {
        for (const RtSmokePersistentStaticSurfaceRecord& record : m_staticSurfaceRecords)
        {
            if (static_cast<int>(boxes.size()) >= maxBoxes)
            {
                return true;
            }
            if (record.seenThisFrame == seenThisFrame)
            {
                appendStaticBox(record);
            }
        }
        return static_cast<int>(boxes.size()) >= maxBoxes;
    };

    if (cacheOnlyFirst)
    {
        appendMatchingStaticBoxes(false) || appendMatchingStaticBoxes(true);
    }
    else
    {
        appendMatchingStaticBoxes(true) || appendMatchingStaticBoxes(false);
    }
}

void RtSmokeGeometryUniverse::BuildRigidRouteInstanceList(const RtPathTraceInstanceUniverse& instanceUniverse, std::vector<RtPathTraceRigidRouteInstanceObservation>& instances) const
{
    instances.clear();
    if (m_rigidResidencyEnabled)
    {
        instances = m_rigidResidentFrameInstances;
        return;
    }

    const std::vector<RtPathTraceInstanceObservation>& frameInstances = instanceUniverse.FrameInstances();
    instances.reserve(frameInstances.size());
    for (const RtPathTraceInstanceObservation& instance : frameInstances)
    {
        if (!RtPathTraceSourceFlagsAreDurableRigid(instance.sourceFlags))
        {
            continue;
        }
        instances.push_back(MakeRigidRouteInstanceObservation(instance));
    }
}

void RtSmokeGeometryUniverse::AddRigidResidencySample(const RigidResidentInstanceRecord& record, bool selectedArea, bool routeReady)
{
    if (m_rigidResidencyStats.sampleCount >= RT_PT_RIGID_RESIDENCY_SAMPLES)
    {
        return;
    }

    const RtPathTraceRigidRouteInstanceObservation& instance = record.observation;
    RtPathTraceRigidResidencySample& sample = m_rigidResidencyStats.samples[m_rigidResidencyStats.sampleCount++];
    sample.valid = true;
    sample.meshHash = instance.meshHash;
    sample.instanceId = instance.instanceId;
    sample.area = instance.currentArea;
    sample.drawSurfIndex = instance.drawSurfIndex;
    sample.entityIndex = instance.entityIndex;
    sample.renderEntityNum = instance.renderEntityNum;
    sample.lastSeenFrame = static_cast<int>(record.lastSeenFrame);
    sample.seenThisFrame = record.seenThisFrame;
    sample.selectedArea = selectedArea;
    sample.routeReady = routeReady;
    sample.origin.Set(instance.objectToWorld[12], instance.objectToWorld[13], instance.objectToWorld[14]);
    sample.materialName = instance.materialName;
    sample.modelName = instance.modelName;
}

bool RtSmokeGeometryUniverse::RigidResidentObservationMatchesCurrentModel(const RtPathTraceRigidRouteInstanceObservation& instance) const
{
    const PtRenderDefKey& renderDefKey = instance.renderDefKey;
    if (renderDefKey.world == nullptr || renderDefKey.index < 0)
    {
        return true;
    }
    if (m_rigidResidencyWorld == nullptr || renderDefKey.world != m_rigidResidencyWorld)
    {
        return false;
    }

    const idRenderWorldLocal* world = static_cast<const idRenderWorldLocal*>(renderDefKey.world);
    if (renderDefKey.index >= world->entityDefs.Num())
    {
        return false;
    }

    const idRenderEntityLocal* entity = world->entityDefs[renderDefKey.index];
    const idRenderModel* model = entity ? entity->parms.hModel : nullptr;
    if (!entity || !model)
    {
        return false;
    }
    if (instance.seenThisFrame)
    {
        return true;
    }

    if (instance.modelSurfaceIndex >= 0)
    {
        return instance.modelSurfaceIndex < model->NumSurfaces();
    }

    const std::unordered_map<uint64, size_t>::const_iterator meshIt = m_rigidMeshCandidateLookup.find(instance.meshHash);
    if (meshIt == m_rigidMeshCandidateLookup.end() || meshIt->second >= m_rigidMeshCandidateRecords.size())
    {
        return true;
    }

    const RigidMeshCandidateRecord& meshRecord = m_rigidMeshCandidateRecords[meshIt->second];
    const srfTriangles_t* tri = meshRecord.tri;
    if (tri == nullptr)
    {
        return true;
    }

    for (int surfaceIndex = 0; surfaceIndex < model->NumSurfaces(); ++surfaceIndex)
    {
        const modelSurface_t* surface = model->Surface(surfaceIndex);
        if (surface && surface->geometry == tri)
        {
            return true;
        }
    }

    return false;
}

void RtSmokeGeometryUniverse::RecordRigidResidentObservation(const RtPathTraceRigidRouteInstanceObservation& instance)
{
    if (instance.instanceId == 0)
    {
        return;
    }

    std::unordered_map<uint64, size_t>::iterator found = m_rigidResidentLookup.find(instance.instanceId);
    if (found == m_rigidResidentLookup.end())
    {
        RigidResidentInstanceRecord record;
        record.observation = instance;
        record.lastSeenFrame = m_currentFrameIndex;
        record.seenCount = 1;
        record.seenThisFrame = true;
        record.observation.isStable = false;
        m_rigidResidentLookup[instance.instanceId] = m_rigidResidentRecords.size();
        m_rigidResidentRecords.push_back(record);
        return;
    }

    if (found->second >= m_rigidResidentRecords.size())
    {
        return;
    }

    RigidResidentInstanceRecord& record = m_rigidResidentRecords[found->second];
    record.observation = instance;
    record.lastSeenFrame = m_currentFrameIndex;
    ++record.seenCount;
    record.seenThisFrame = true;
    record.observation.isStable = record.seenCount > 1;
}

void RtSmokeGeometryUniverse::PruneRigidCachesToCurrentFrame(
    const idRenderWorldLocal* renderWorld,
    const idRenderMatrix* viewMvp,
    const idVec3* viewOrigin,
    const std::vector<bool>& selectedAreas)
{
    (void)renderWorld;
    const bool v2 = r_pathTracingGeometryResidencyV2.GetInteger() != 0;
    if (!v2 && !m_frameActive)
    {
        return;
    }
    const uint64 framesToKeep = static_cast<uint64>(idMath::ClampInt(0, 100000, r_pathTracingResidencyFramesToKeep.GetInteger()));
    const uint64 meshFramesToKeep = static_cast<uint64>(idMath::ClampInt(0, 100000, r_pathTracingResidencyMeshFramesToKeep.GetInteger()));
    const bool entityFeedOwnsOffscreenResidency = r_pathTracingEntityFeed.GetInteger() != 0;
    const uint64 entityFeedFramesToKeep = 2u;
    const auto ApplyEntityFeedRetentionCap =
        [entityFeedOwnsOffscreenResidency, entityFeedFramesToKeep](uint64 requestedFrames) -> uint64
        {
            return entityFeedOwnsOffscreenResidency && requestedFrames > entityFeedFramesToKeep
                ? entityFeedFramesToKeep
                : requestedFrames;
        };
    (void)viewOrigin;
    (void)selectedAreas;
    m_rigidResidencyStats.residencyFramesToKeep = static_cast<int>(framesToKeep);
    m_rigidResidencyStats.residencyMeshFramesToKeep = static_cast<int>(meshFramesToKeep);
    m_rigidResidencyStats.residencyEntityFeedCap = entityFeedOwnsOffscreenResidency ? static_cast<int>(entityFeedFramesToKeep) : 0;
    m_rigidResidencyStats.residencyAntiCulling = v2 ? 1 : 0;
    std::unordered_set<uint64> residentMeshHashes;

    if (!m_rigidResidentRecords.empty())
    {
        std::vector<RigidResidentInstanceRecord> liveResidentRecords;
        liveResidentRecords.reserve(m_rigidResidentRecords.size());
        for (const RigidResidentInstanceRecord& record : m_rigidResidentRecords)
        {
            bool keepRecord = record.seenThisFrame;
            bool retainedOffscreen = false;
            if (v2 && !record.seenThisFrame)
            {
                const uint64 recordFramesToKeep = ApplyEntityFeedRetentionCap(framesToKeep);
                const bool withinWindow = record.lastSeenFrame + recordFramesToKeep >= m_currentFrameIndex;
                if (withinWindow)
                {
                    const std::unordered_map<uint64, size_t>::const_iterator meshIt = m_rigidMeshCandidateLookup.find(record.observation.meshHash);
                    const RigidMeshCandidateRecord* meshRecord =
                        meshIt != m_rigidMeshCandidateLookup.end() && meshIt->second < m_rigidMeshCandidateRecords.size()
                            ? &m_rigidMeshCandidateRecords[meshIt->second]
                            : nullptr;
                    idBounds worldBounds;
                    const bool outsideFrustum =
                        viewMvp &&
                        meshRecord &&
                        meshRecord->valid &&
                        BuildRigidResidencyWorldBounds(*meshRecord, record.observation, worldBounds) &&
                        idRenderMatrix::CullBoundsToMVP(*viewMvp, worldBounds, false);
                    const bool eligible =
                        record.observation.isStable &&
                        !record.observation.isSkinnedOrDeforming &&
                        !record.observation.wasMovingWhenLastSeen;
                    retainedOffscreen = eligible && outsideFrustum;
                    keepRecord = retainedOffscreen;
                }
            }

            if (keepRecord)
            {
                liveResidentRecords.push_back(record);
                if (record.observation.meshHash != 0)
                {
                    residentMeshHashes.insert(record.observation.meshHash);
                }
                if (retainedOffscreen)
                {
                    ++m_rigidResidencyStats.residentRetainedOffscreen;
                }
            }
            else
            {
                ++m_rigidResidencyStats.residentAgedOut;
            }
        }
        if (liveResidentRecords.size() != m_rigidResidentRecords.size())
        {
            m_rigidResidentRecords.swap(liveResidentRecords);
            m_rigidResidentLookup.clear();
            m_rigidResidentLookup.reserve(m_rigidResidentRecords.size());
            for (size_t recordIndex = 0; recordIndex < m_rigidResidentRecords.size(); ++recordIndex)
            {
                m_rigidResidentLookup[m_rigidResidentRecords[recordIndex].observation.instanceId] = recordIndex;
            }
            ++m_generation;
            m_rigidResidencyStats.generation = m_generation;
        }
    }

    if (!m_rigidMeshCandidateRecords.empty())
    {
        std::vector<RigidMeshCandidateRecord> liveMeshRecords;
        liveMeshRecords.reserve(m_rigidMeshCandidateRecords.size());
        for (RigidMeshCandidateRecord& record : m_rigidMeshCandidateRecords)
        {
            const bool referencedByResident =
                residentMeshHashes.find(record.meshHash) != residentMeshHashes.end();
            const uint64 recordMeshFramesToKeep = ApplyEntityFeedRetentionCap(meshFramesToKeep);
            const bool keepRecord = v2
                ? record.valid && (referencedByResident || record.seenThisFrame || record.lastSeenFrame + recordMeshFramesToKeep >= m_currentFrameIndex)
                : record.valid && record.seenThisFrame;
            if (keepRecord)
            {
                if (!record.seenThisFrame)
                {
                    record.tri = nullptr;
                }
                liveMeshRecords.push_back(record);
                ++m_rigidResidencyStats.meshLive;
            }
            else
            {
                RetireRigidBlas(record);
                ++m_rigidResidencyStats.meshAgedOut;
            }
        }
        if (liveMeshRecords.size() != m_rigidMeshCandidateRecords.size())
        {
            m_rigidMeshCandidateRecords.swap(liveMeshRecords);
            m_rigidMeshCandidateLookup.clear();
            m_rigidMeshCandidateLookup.reserve(m_rigidMeshCandidateRecords.size());
            for (size_t recordIndex = 0; recordIndex < m_rigidMeshCandidateRecords.size(); ++recordIndex)
            {
                m_rigidMeshCandidateLookup[m_rigidMeshCandidateRecords[recordIndex].meshHash] = recordIndex;
            }
            ++m_generation;
            m_rigidResidencyStats.generation = m_generation;
        }
    }
    m_rigidResidencyStats.retiredBlasPending = static_cast<int>(m_retiredRigidBlasRecords.size());
}

RtPathTraceRigidTlasPlanStats RtSmokeGeometryUniverse::BuildRigidTlasPlanStats(const RtPathTraceInstanceUniverse& instanceUniverse, const RtSmokeSurfaceClassStats* sourceClassStats) const
{
    RtPathTraceRigidTlasPlanStats stats;
    stats.frameIndex = m_currentFrameIndex;
    stats.generation = m_generation;
    stats.bakedRigidSurfaces = sourceClassStats ? sourceClassStats->rigidEntitySurfaces : 0;
    stats.bakedRigidTriangles = sourceClassStats ? sourceClassStats->rigidEntityTriangles : 0;

    std::unordered_set<uint64> plannedMeshHashes;
    std::vector<RtPathTraceRigidRouteInstanceObservation> instances;
    BuildRigidRouteInstanceList(instanceUniverse, instances);
    stats.visibleInstances = static_cast<int>(instances.size());
    for (const RtPathTraceRigidRouteInstanceObservation& instance : instances)
    {
        if (!RtPathTraceSourceFlagsAreDurableRigid(instance.sourceFlags))
        {
            continue;
        }

        ++stats.rigidInstances;
        if ((instance.sourceFlags & RT_PT_INSTANCE_SOURCE_MATERIAL_OVERRIDE) != 0)
        {
            ++stats.materialOverrideInstances;
        }

        const RigidMeshCandidateRecord* record = nullptr;
        const std::unordered_map<uint64, size_t>::const_iterator it = m_rigidMeshCandidateLookup.find(instance.meshHash);
        if (it != m_rigidMeshCandidateLookup.end() && it->second < m_rigidMeshCandidateRecords.size())
        {
            record = &m_rigidMeshCandidateRecords[it->second];
        }

        bool hasMeshRecord = record && record->valid;
        bool meshSeenThisFrame = hasMeshRecord && record->seenThisFrame;
        bool meshAvailableForRoute = hasMeshRecord && (meshSeenThisFrame || m_rigidResidencyEnabled);
        bool hasGpuBuffers = hasMeshRecord && record->rigidVertexBuffer && record->rigidIndexBuffer;
        bool hasBlas = hasMeshRecord && record->rigidBlas;
        int triangleCount = 0;
        int meshInstanceCount = 0;

        if (!hasMeshRecord)
        {
            ++stats.missingMeshRecord;
        }
        else if (!meshAvailableForRoute)
        {
            ++stats.staleMeshRecord;
        }
        else
        {
            ++stats.plannedInstances;
            plannedMeshHashes.insert(instance.meshHash);
            triangleCount = record->sourceRange.triangles.count;
            meshInstanceCount = Max(1, record->instanceCountThisFrame);
            stats.plannedRigidTriangles += triangleCount;
            if (hasGpuBuffers)
            {
                ++stats.instancesWithGpuBuffers;
            }
            else
            {
                ++stats.missingGpuBuffers;
            }
            if (hasBlas)
            {
                ++stats.instancesWithBlas;
            }
            else
            {
                ++stats.missingBlas;
            }
        }

        if (stats.sampleCount < RT_PT_RIGID_TLAS_PLAN_SAMPLES)
        {
            RtPathTraceRigidTlasPlanSample& sample = stats.samples[stats.sampleCount++];
            sample.valid = true;
            sample.meshHash = instance.meshHash;
            sample.instanceId = instance.instanceId;
            sample.triIdentity = hasMeshRecord ? reinterpret_cast<uintptr_t>(record->tri) : 0;
            sample.materialId = hasMeshRecord ? record->materialId : instance.materialOverrideId;
            sample.sourceFlags = instance.sourceFlags;
            sample.drawSurfIndex = instance.drawSurfIndex;
            sample.entityIndex = instance.entityIndex;
            sample.renderEntityNum = instance.renderEntityNum;
            sample.triangles = triangleCount;
            sample.instanceCountForMesh = meshInstanceCount;
            sample.hasMeshRecord = hasMeshRecord;
            sample.meshSeenThisFrame = meshSeenThisFrame;
            sample.hasGpuBuffers = hasGpuBuffers;
            sample.hasBlas = hasBlas;
            sample.origin.Set(instance.objectToWorld[12], instance.objectToWorld[13], instance.objectToWorld[14]);
            sample.materialName = hasMeshRecord ? record->materialName : instance.materialName;
            sample.modelName = hasMeshRecord ? record->modelName : instance.modelName;
        }
    }

    stats.uniqueMeshes = static_cast<int>(plannedMeshHashes.size());
    stats.estimatedRemainingRigidTriangles = Max(0, stats.bakedRigidTriangles - stats.plannedRigidTriangles);
    stats.triangleDelta = stats.plannedRigidTriangles - stats.bakedRigidTriangles;
    return stats;
}

void RtSmokeGeometryUniverse::DumpRigidTlasPlanStats(const RtPathTraceRigidTlasPlanStats& stats, int sceneSource) const
{
    common->Printf("PathTracePrimaryPass: PT rigid TLAS plan source=%d frame=%llu generation=%llu visibleInstances=%d rigidInstances=%d plannedInstances=%d uniqueMeshes=%d gpuBuffers=%d blas=%d missing(mesh/stale/buffers/blas)=%d/%d/%d/%d overrides=%d plannedRigidTris=%d bakedRigidSurfaces/tris=%d/%d remainingDynamicRigidTris=%d triangleDelta=%d renderPath=dynamicFallback tlasRoute=rigidResidencyRoute\n",
        sceneSource,
        static_cast<unsigned long long>(stats.frameIndex),
        static_cast<unsigned long long>(stats.generation),
        stats.visibleInstances,
        stats.rigidInstances,
        stats.plannedInstances,
        stats.uniqueMeshes,
        stats.instancesWithGpuBuffers,
        stats.instancesWithBlas,
        stats.missingMeshRecord,
        stats.staleMeshRecord,
        stats.missingGpuBuffers,
        stats.missingBlas,
        stats.materialOverrideInstances,
        stats.plannedRigidTriangles,
        stats.bakedRigidSurfaces,
        stats.bakedRigidTriangles,
        stats.estimatedRemainingRigidTriangles,
        stats.triangleDelta);

    for (int sampleIndex = 0; sampleIndex < stats.sampleCount; ++sampleIndex)
    {
        const RtPathTraceRigidTlasPlanSample& sample = stats.samples[sampleIndex];
        if (!sample.valid)
        {
            continue;
        }

        common->Printf("PathTracePrimaryPass: PT rigid TLAS plan sample %d mesh=%llu instance=%llu surf=%d entity=%d renderEntity=%d tri=%llu tris=%d meshInstances=%d origin=(%.2f %.2f %.2f) sourceFlags=0x%x ready(mesh/seen/buffers/blas)=%d/%d/%d/%d material=%u '%s' model='%s'\n",
            sampleIndex,
            static_cast<unsigned long long>(sample.meshHash),
            static_cast<unsigned long long>(sample.instanceId),
            sample.drawSurfIndex,
            sample.entityIndex,
            sample.renderEntityNum,
            static_cast<unsigned long long>(sample.triIdentity),
            sample.triangles,
            sample.instanceCountForMesh,
            sample.origin.x,
            sample.origin.y,
            sample.origin.z,
            sample.sourceFlags,
            sample.hasMeshRecord ? 1 : 0,
            sample.meshSeenThisFrame ? 1 : 0,
            sample.hasGpuBuffers ? 1 : 0,
            sample.hasBlas ? 1 : 0,
            sample.materialId,
            sample.materialName.c_str(),
            sample.modelName.c_str());
    }
}

bool RtSmokeGeometryUniverse::IsRigidRouteReady(uint64 meshHash) const
{
    const std::unordered_map<uint64, size_t>::const_iterator it = m_rigidMeshCandidateLookup.find(meshHash);
    if (it == m_rigidMeshCandidateLookup.end() || it->second >= m_rigidMeshCandidateRecords.size())
    {
        return false;
    }

    const RigidMeshCandidateRecord& record = m_rigidMeshCandidateRecords[it->second];
    return RigidMeshHasCachedRouteGpuReady(record);
}

bool RtSmokeGeometryUniverse::IsRigidRouteResidentReadyForEntityMaterial(int entityIndex, int renderEntityNum, uint32_t materialId) const
{
    if (entityIndex < 0 || renderEntityNum < 0 || materialId == 0)
    {
        return false;
    }

    for (const RigidResidentInstanceRecord& residentRecord : m_rigidResidentRecords)
    {
        const RtPathTraceRigidRouteInstanceObservation& observation = residentRecord.observation;
        if (observation.entityIndex == entityIndex &&
            observation.renderEntityNum == renderEntityNum &&
            observation.materialOverrideId == materialId &&
            IsRigidRouteReady(observation.meshHash))
        {
            return true;
        }
    }
    return false;
}

std::vector<uint32_t> RtSmokeGeometryUniverse::CollectRigidRouteMaterialIds(const RtSmokeRigidTlasPlan& plan) const
{
    std::vector<uint32_t> materialIds;
    for (const RtSmokePlanTlasInstance& plannedInstance : plan.instances)
    {
        if (plannedInstance.routeRecordIndex >= m_rigidMeshCandidateRecords.size())
        {
            continue;
        }

        const RigidMeshCandidateRecord& record = m_rigidMeshCandidateRecords[plannedInstance.routeRecordIndex];
        if (!RigidPlanInstanceMatchesRecord(plannedInstance, record) ||
            !record.rigidBlas ||
            !RigidCachedTlasInstanceValid(plannedInstance, record))
        {
            continue;
        }
        if (std::find(materialIds.begin(), materialIds.end(), record.materialId) == materialIds.end())
        {
            materialIds.push_back(record.materialId);
        }
    }
    return materialIds;
}

std::vector<uint32_t> RtSmokeGeometryUniverse::CollectRigidRouteMaterialIds(const RtPathTraceInstanceUniverse& instanceUniverse, int maxInstances) const
{
    const RtSmokeRigidTlasPlanSnapshot snapshot =
        CaptureRigidTlasInstancePlanSnapshot(instanceUniverse, 2, 0x02, maxInstances);
    const RtSmokeRigidTlasPlan plan = BuildSmokeRigidTlasPlan(snapshot);
    return CollectRigidRouteMaterialIds(plan);
}

RtSmokeRigidTlasPlanSnapshot RtSmokeGeometryUniverse::CaptureRigidTlasInstancePlanSnapshot(
    const RtPathTraceInstanceUniverse& instanceUniverse,
    uint32_t firstInstanceId,
    uint32_t instanceMask,
    int maxInstances) const
{
    std::vector<RtPathTraceRigidRouteInstanceObservation> instances;
    BuildRigidRouteInstanceList(instanceUniverse, instances);
    const int normalizedMaxInstances = maxInstances > 0 ? maxInstances : 0;
    if (normalizedMaxInstances > 0 && static_cast<int>(instances.size()) > normalizedMaxInstances)
    {
        struct RigidRouteSelectionGroup
        {
            std::vector<size_t> indices;
            int readyCount = 0;
            bool seenThisFrame = false;
            bool stable = false;
            bool transformContinuous = false;
            size_t firstIndex = 0;
        };

        const auto routeReadyForInstance = [this, &instances](size_t instanceIndex) -> bool
        {
            const RtPathTraceRigidRouteInstanceObservation& instance = instances[instanceIndex];
            if (!RtPathTraceSourceFlagsAreDurableRigid(instance.sourceFlags))
            {
                return false;
            }
            const std::unordered_map<uint64, size_t>::const_iterator it = m_rigidMeshCandidateLookup.find(instance.meshHash);
            if (it == m_rigidMeshCandidateLookup.end() || it->second >= m_rigidMeshCandidateRecords.size())
            {
                return false;
            }
            const RigidMeshCandidateRecord& record = m_rigidMeshCandidateRecords[it->second];
            return record.valid &&
                record.rigidBlas &&
                (m_rigidResidencyEnabled || record.seenThisFrame);
        };

        std::vector<RigidRouteSelectionGroup> groups;
        for (size_t instanceIndex = 0; instanceIndex < instances.size(); ++instanceIndex)
        {
            size_t groupIndex = groups.size();
            for (size_t testIndex = 0; testIndex < groups.size(); ++testIndex)
            {
                if (!groups[testIndex].indices.empty() &&
                    RigidRouteEntityKeyEqual(instances[groups[testIndex].indices.front()], instances[instanceIndex]))
                {
                    groupIndex = testIndex;
                    break;
                }
            }
            if (groupIndex == groups.size())
            {
                RigidRouteSelectionGroup group;
                group.firstIndex = instanceIndex;
                groups.push_back(group);
            }

            RigidRouteSelectionGroup& group = groups[groupIndex];
            group.indices.push_back(instanceIndex);
            group.seenThisFrame = group.seenThisFrame || instances[instanceIndex].seenThisFrame;
            group.stable = group.stable || instances[instanceIndex].isStable;
            group.transformContinuous = group.transformContinuous || instances[instanceIndex].transformContinuous;
            if (routeReadyForInstance(instanceIndex))
            {
                ++group.readyCount;
            }
        }

        std::stable_sort(
            groups.begin(),
            groups.end(),
            [](const RigidRouteSelectionGroup& a, const RigidRouteSelectionGroup& b)
            {
                if (a.seenThisFrame != b.seenThisFrame)
                {
                    return a.seenThisFrame;
                }
                if (a.stable != b.stable)
                {
                    return a.stable;
                }
                if (a.transformContinuous != b.transformContinuous)
                {
                    return a.transformContinuous;
                }
                return a.firstIndex < b.firstIndex;
            });

        const auto appendPartialGroup = [&instances, &routeReadyForInstance, normalizedMaxInstances](std::vector<RtPathTraceRigidRouteInstanceObservation>& selected, const RigidRouteSelectionGroup& group, int& remaining) -> void
        {
            for (size_t instanceIndex : group.indices)
            {
                const bool consumesBudget = routeReadyForInstance(instanceIndex);
                selected.push_back(instances[instanceIndex]);
                if (consumesBudget)
                {
                    --remaining;
                    if (remaining <= 0)
                    {
                        break;
                    }
                }
                if (static_cast<int>(selected.size()) >= normalizedMaxInstances)
                {
                    break;
                }
            }
        };

        std::vector<RtPathTraceRigidRouteInstanceObservation> selectedInstances;
        selectedInstances.reserve(normalizedMaxInstances);
        int remainingRouteBudget = normalizedMaxInstances;
        for (const RigidRouteSelectionGroup& group : groups)
        {
            if (remainingRouteBudget <= 0)
            {
                break;
            }

            if (group.readyCount <= remainingRouteBudget)
            {
                for (size_t instanceIndex : group.indices)
                {
                    selectedInstances.push_back(instances[instanceIndex]);
                }
                remainingRouteBudget -= group.readyCount;
            }
            else if (selectedInstances.empty())
            {
                appendPartialGroup(selectedInstances, group, remainingRouteBudget);
            }
        }
        instances.swap(selectedInstances);
    }

    RtSmokeRigidTlasPlanSnapshot snapshot;
    snapshot.rigidSourceMask = RT_PT_INSTANCE_SOURCE_RIGID;
    snapshot.firstInstanceId = firstInstanceId;
    snapshot.instanceMask = instanceMask;
    snapshot.maxInstances = maxInstances;

    const int reserveCount = maxInstances > 0 && maxInstances < static_cast<int>(instances.size())
        ? maxInstances
        : static_cast<int>(instances.size());
    snapshot.observations.reserve(reserveCount);
    for (const RtPathTraceRigidRouteInstanceObservation& instance : instances)
    {
        RtSmokeRigidTlasObservation observation;
        observation.meshHash = instance.meshHash;
        observation.instanceId = instance.instanceId;
        observation.sourceFlags = instance.sourceFlags;
        observation.residencyEnabled = m_rigidResidencyEnabled;
        observation.seenThisFrame = instance.seenThisFrame;
        observation.hasPreviousObjectToWorld = instance.hasPreviousObjectToWorld;
        observation.transformContinuous = instance.transformContinuous;
        memcpy(observation.objectToWorld, instance.objectToWorld, sizeof(observation.objectToWorld));
        memcpy(observation.previousObjectToWorld, instance.previousObjectToWorld, sizeof(observation.previousObjectToWorld));
        const std::unordered_map<uint64, size_t>::const_iterator it = m_rigidMeshCandidateLookup.find(instance.meshHash);
        if (it != m_rigidMeshCandidateLookup.end() && it->second < m_rigidMeshCandidateRecords.size())
        {
            const RigidMeshCandidateRecord& record = m_rigidMeshCandidateRecords[it->second];
            observation.hasMeshRecord = record.valid;
            observation.meshSeenThisFrame = record.seenThisFrame;
            observation.hasBlas = record.rigidBlas;
            observation.routeRecordIndex = static_cast<uint32_t>(it->second);
        }
        snapshot.observations.push_back(observation);
    }
    return snapshot;
}

RtSmokeRigidTlasPlan RtSmokeGeometryUniverse::BuildRigidTlasInstancePlan(
    const RtPathTraceInstanceUniverse& instanceUniverse,
    uint32_t firstInstanceId,
    uint32_t instanceMask,
    int maxInstances) const
{
    const RtSmokeRigidTlasPlanSnapshot snapshot =
        CaptureRigidTlasInstancePlanSnapshot(instanceUniverse, firstInstanceId, instanceMask, maxInstances);
    return BuildSmokeRigidTlasPlan(snapshot);
}

int RtSmokeGeometryUniverse::BuildRigidTlasInstanceDescs(
    const RtSmokeRigidTlasPlan& plan,
    std::vector<nvrhi::rt::InstanceDesc>& instanceDescs) const
{
    const size_t firstDesc = instanceDescs.size();
    for (const RtSmokePlanTlasInstance& plannedInstance : plan.instances)
    {
        if (!plannedInstance.sourceSeenThisFrame &&
            r_pathTracingResidencyRouteCachedTlas.GetInteger() == 0)
        {
            continue;
        }
        if (plannedInstance.routeRecordIndex >= m_rigidMeshCandidateRecords.size())
        {
            continue;
        }
        const RigidMeshCandidateRecord& record = m_rigidMeshCandidateRecords[plannedInstance.routeRecordIndex];
        if (!RigidPlanInstanceMatchesRecord(plannedInstance, record) ||
            !record.rigidBlas ||
            !RigidCachedTlasInstanceValid(plannedInstance, record))
        {
            continue;
        }

        nvrhi::rt::AffineTransform transform;
        BuildRigidTlasAffineTransform(plannedInstance.transform, transform);

        const bool cachedSource = !plannedInstance.sourceSeenThisFrame;
        const uint32_t instanceMask =
            cachedSource && r_pathTracingResidencyRouteCachedTraceMask.GetInteger() == 0
                ? 0u
                : plannedInstance.instanceMask;
        nvrhi::rt::InstanceDesc instanceDesc;
        instanceDesc
            .setInstanceID(plannedInstance.instanceId)
            .setInstanceMask(instanceMask)
            .setInstanceContributionToHitGroupIndex(plannedInstance.hitGroupContribution)
            .setFlags(nvrhi::rt::InstanceFlags::TriangleCullDisable)
            .setTransform(transform)
            .setBLAS(record.rigidBlas);
        instanceDescs.push_back(instanceDesc);
    }

    return static_cast<int>(instanceDescs.size() - firstDesc);
}

int RtSmokeGeometryUniverse::BuildRigidTlasInstanceDescs(
    const RtPathTraceInstanceUniverse& instanceUniverse,
    std::vector<nvrhi::rt::InstanceDesc>& instanceDescs,
    uint32_t firstInstanceId,
    uint32_t instanceMask,
    int maxInstances) const
{
    const RtSmokeRigidTlasPlan plan =
        BuildRigidTlasInstancePlan(instanceUniverse, firstInstanceId, instanceMask, maxInstances);
    return BuildRigidTlasInstanceDescs(plan, instanceDescs);
}

RtPathTraceRigidRouteBuildSnapshot RtSmokeGeometryUniverse::CaptureRigidRouteBuildSnapshot(
    const RtSmokeRigidTlasPlan& plan,
    const std::vector<uint32_t>& materialTableIds,
    bool captureGeometryPayload) const
{
    RtPathTraceRigidRouteBuildSnapshot snapshot;
    snapshot.plan = plan;
    snapshot.materialTableIds = materialTableIds;
    snapshot.meshes.reserve(plan.instances.size());

    std::unordered_set<uint32_t> capturedRouteRecords;
    for (const RtSmokePlanTlasInstance& plannedInstance : plan.instances)
    {
        if (plannedInstance.routeRecordIndex >= m_rigidMeshCandidateRecords.size())
        {
            continue;
        }
        if (!capturedRouteRecords.insert(plannedInstance.routeRecordIndex).second)
        {
            continue;
        }

        const RigidMeshCandidateRecord& record = m_rigidMeshCandidateRecords[plannedInstance.routeRecordIndex];
        RtPathTraceRigidRouteMeshSnapshot mesh;
        mesh.routeRecordIndex = plannedInstance.routeRecordIndex;
        mesh.valid = record.valid;
        mesh.meshHash = record.meshHash;
        mesh.gpuUploadSignature = record.gpuUploadSignature;
        mesh.materialId = record.materialId;
        mesh.surfaceClassId = record.surfaceClassId;
        mesh.triangleClassAndFlags = record.triangleClassAndFlags;
        mesh.routeReady = RigidMeshHasCachedRouteGpuReady(record);
        mesh.localBounds = record.localBounds;
        mesh.localBoundsValid = record.localBoundsValid && !record.localBounds.IsCleared();
        mesh.vertexCount = static_cast<uint32_t>(record.cachedLocalVertices.size());
        mesh.indexCount = static_cast<uint32_t>(record.cachedLocalIndexes.size());
        if (mesh.routeReady && captureGeometryPayload)
        {
            mesh.vertices = record.cachedLocalVertices;
            mesh.indexes = record.cachedLocalIndexes;
        }
        snapshot.meshes.push_back(std::move(mesh));
    }

    return snapshot;
}

static const RtPathTraceRigidRouteMeshSnapshot* FindRigidRouteMeshSnapshot(
    const RtPathTraceRigidRouteBuildSnapshot& snapshot,
    uint32_t routeRecordIndex)
{
    for (const RtPathTraceRigidRouteMeshSnapshot& mesh : snapshot.meshes)
    {
        if (mesh.routeRecordIndex == routeRecordIndex)
        {
            return &mesh;
        }
    }
    return nullptr;
}

static bool RigidSnapshotTlasInstanceValid(
    const RtSmokePlanTlasInstance& plannedInstance,
    const RtPathTraceRigidRouteMeshSnapshot& mesh)
{
    if (!mesh.valid ||
        !mesh.routeReady ||
        !mesh.localBoundsValid ||
        !RigidRouteTransformUsable(plannedInstance.transform))
    {
        return false;
    }

    RtPathTraceRigidResidencyBoundsBox boundsBox;
    boundsBox.valid = true;
    for (int cornerIndex = 0; cornerIndex < 8; ++cornerIndex)
    {
        idVec3 localPoint;
        localPoint.x = mesh.localBounds[(cornerIndex ^ (cornerIndex >> 1)) & 1].x;
        localPoint.y = mesh.localBounds[(cornerIndex >> 1) & 1].y;
        localPoint.z = mesh.localBounds[(cornerIndex >> 2) & 1].z;
        TransformRigidResidencyBoundsPoint(plannedInstance.transform, localPoint, boundsBox.corners[cornerIndex]);
    }
    return ValidateRigidResidencyBoundsBox(boundsBox);
}

RtPathTraceRigidRouteBuild BuildRigidRouteBuffersFromSnapshot(
    const RtPathTraceRigidRouteBuildSnapshot& snapshot)
{
    struct RigidRouteGeometryRange
    {
        uint32_t vertexOffset = 0;
        uint32_t indexOffset = 0;
        uint32_t triangleOffset = 0;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        uint32_t triangleCount = 0;
        uint32_t materialId = 0;
        uint32_t materialIndex = 0;
        uint32_t triangleClassAndFlags = 0;
    };

    RtPathTraceRigidRouteBuild build;
    std::unordered_map<uint64, RigidRouteGeometryRange> emittedGeometryRanges;
    std::unordered_set<uint64> emittedMeshHashes;

    build.stats.visibleInstances = snapshot.plan.visibleInstances;
    build.stats.skippedNonRigid = snapshot.plan.rejectedNonRigid;
    build.stats.skippedMissingMesh = snapshot.plan.rejectedMissingMesh + snapshot.plan.rejectedStaleMesh;
    build.stats.skippedMissingBlas = snapshot.plan.rejectedMissingBlas;
    for (const RtSmokePlanTlasInstance& plannedInstance : snapshot.plan.instances)
    {
        const RtPathTraceRigidRouteMeshSnapshot* mesh =
            FindRigidRouteMeshSnapshot(snapshot, plannedInstance.routeRecordIndex);
        if (!mesh)
        {
            ++build.stats.skippedMissingMesh;
            AppendRigidRoutePlaceholder(build, plannedInstance);
            continue;
        }
        if (!mesh->valid || mesh->meshHash != plannedInstance.meshHash)
        {
            ++build.stats.skippedMissingMesh;
            AppendRigidRoutePlaceholder(build, plannedInstance);
            continue;
        }
        if (!RigidSnapshotTlasInstanceValid(plannedInstance, *mesh))
        {
            ++build.stats.skippedMissingBlas;
            AppendRigidRoutePlaceholder(build, plannedInstance);
            continue;
        }

        const RigidRouteGeometryRange* sharedGeometry = nullptr;
        const std::unordered_map<uint64, RigidRouteGeometryRange>::const_iterator sharedIt =
            emittedGeometryRanges.find(mesh->meshHash);
        if (sharedIt != emittedGeometryRanges.end())
        {
            sharedGeometry = &sharedIt->second;
        }

        RigidRouteGeometryRange geometryRange;
        if (sharedGeometry)
        {
            geometryRange = *sharedGeometry;
        }
        else
        {
            if (mesh->vertices.empty() || mesh->indexes.empty())
            {
                ++build.stats.skippedMissingMesh;
                AppendRigidRoutePlaceholder(build, plannedInstance);
                continue;
            }

            geometryRange.vertexOffset = static_cast<uint32_t>(build.vertices.size());
            geometryRange.indexOffset = static_cast<uint32_t>(build.indexes.size());
            geometryRange.triangleOffset = static_cast<uint32_t>(build.triangleMaterials.size());
            geometryRange.vertexCount = static_cast<uint32_t>(mesh->vertices.size());
            geometryRange.indexCount = static_cast<uint32_t>(mesh->indexes.size());
            geometryRange.triangleCount = static_cast<uint32_t>(mesh->indexes.size() / 3);
            geometryRange.materialId = mesh->materialId;
            geometryRange.materialIndex = FindRigidRouteMaterialTableIndex(
                snapshot.materialTableIds,
                mesh->materialId,
                build.stats.missingMaterialTableIndex);
            geometryRange.triangleClassAndFlags =
                mesh->triangleClassAndFlags != 0u ? mesh->triangleClassAndFlags : mesh->surfaceClassId;

            build.vertices.insert(build.vertices.end(), mesh->vertices.begin(), mesh->vertices.end());
            build.indexes.insert(build.indexes.end(), mesh->indexes.begin(), mesh->indexes.end());
            for (uint32_t triangleIndex = 0; triangleIndex < geometryRange.triangleCount; ++triangleIndex)
            {
                build.triangleMaterials.push_back(geometryRange.materialId);
                build.triangleMaterialIndexes.push_back(geometryRange.materialIndex);
                build.triangleClassAndFlags.push_back(geometryRange.triangleClassAndFlags);
            }
            emittedGeometryRanges[mesh->meshHash] = geometryRange;
        }

        PathTraceRigidRouteInstance routeInstance;
        routeInstance.vertexOffset = geometryRange.vertexOffset;
        routeInstance.indexOffset = geometryRange.indexOffset;
        routeInstance.triangleOffset = geometryRange.triangleOffset;
        routeInstance.materialId = geometryRange.materialId;
        routeInstance.materialIndex = geometryRange.materialIndex;
        routeInstance.vertexCount = geometryRange.vertexCount;
        routeInstance.indexCount = geometryRange.indexCount;
        routeInstance.triangleCount = geometryRange.triangleCount;
        routeInstance.instanceIdLo = static_cast<uint32_t>(plannedInstance.sourceInstanceId & 0xffffffffull);
        routeInstance.instanceIdHi = static_cast<uint32_t>((plannedInstance.sourceInstanceId >> 32) & 0xffffffffull);
        if (plannedInstance.hasPreviousTransform)
        {
            routeInstance.flags |= PT_RIGID_ROUTE_HAS_PREVIOUS_TRANSFORM;
            ++build.stats.previousTransformInstances;
        }
        if (plannedInstance.transformContinuous)
        {
            routeInstance.flags |= PT_RIGID_ROUTE_TRANSFORM_CONTINUOUS;
            ++build.stats.transformContinuousInstances;
        }
        if (!plannedInstance.sourceSeenThisFrame)
        {
            routeInstance.flags |= PT_RIGID_ROUTE_CACHED_SOURCE;
        }
        CopyRigidRouteTransformRows(routeInstance.currentObjectToWorld, plannedInstance.transform);
        CopyRigidRouteTransformRows(routeInstance.previousObjectToWorld, plannedInstance.hasPreviousTransform ? plannedInstance.previousTransform : plannedInstance.transform);
        build.instances.push_back(routeInstance);
        build.instanceSeenThisFrame.push_back(plannedInstance.sourceSeenThisFrame ? 1u : 0u);
        std::array<float, 16> objectToWorld = {};
        for (int elementIndex = 0; elementIndex < 16; ++elementIndex)
        {
            objectToWorld[elementIndex] = plannedInstance.transform[elementIndex];
        }
        build.instanceObjectToWorld.push_back(objectToWorld);

        ++build.stats.emittedInstances;
        if (emittedMeshHashes.insert(mesh->meshHash).second)
        {
            ++build.stats.emittedUniqueMeshes;
        }
        if (plannedInstance.sourceSeenThisFrame)
        {
            ++build.stats.emittedSeenThisFrame;
        }
        else
        {
            ++build.stats.emittedFromCache;
        }
        if (!sharedGeometry)
        {
            build.stats.vertices += static_cast<int>(geometryRange.vertexCount);
            build.stats.indexes += static_cast<int>(geometryRange.indexCount);
            build.stats.triangles += static_cast<int>(geometryRange.triangleCount);
        }
    }

    return build;
}

uint64_t BuildRigidRouteGeometryUploadSignature(const RtPathTraceRigidRouteBuild& build)
{
    const RtSmokePlanDataSpan spans[] = {
        MakeRigidRoutePlanDataSpan(build.vertices),
        MakeRigidRoutePlanDataSpan(build.indexes),
        MakeRigidRoutePlanDataSpan(build.triangleMaterials),
        MakeRigidRoutePlanDataSpan(build.triangleMaterialIndexes)
    };
    return BuildSmokePlanDataSpanSignature(
        spans,
        static_cast<int>(sizeof(spans) / sizeof(spans[0])));
}

uint64_t BuildRigidRouteInstanceUploadSignature(const RtPathTraceRigidRouteBuild& build)
{
    const RtSmokePlanDataSpan spans[] = {
        MakeRigidRoutePlanDataSpan(build.instances)
    };
    return BuildSmokePlanDataSpanSignature(
        spans,
        static_cast<int>(sizeof(spans) / sizeof(spans[0])));
}

RtPathTraceRigidRouteBuildTimedResult BuildRigidRouteBuffersTimedResult(
    const RtPathTraceRigidRouteBuildSnapshot& snapshot)
{
    const auto start = std::chrono::steady_clock::now();
    RtPathTraceRigidRouteBuildTimedResult result;
    result.build = BuildRigidRouteBuffersFromSnapshot(snapshot);
    result.geometryUploadSignature = BuildRigidRouteGeometryUploadSignature(result.build);
    result.instanceUploadSignature = BuildRigidRouteInstanceUploadSignature(result.build);
    result.geometryUploadSignatureValid = true;
    result.instanceUploadSignatureValid = true;
    const auto end = std::chrono::steady_clock::now();
    result.buildTimeMicros = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());
    return result;
}

RtPathTraceRigidRouteBuild RtSmokeGeometryUniverse::BuildRigidRouteBuffers(
    const RtSmokeRigidTlasPlan& plan,
    const std::vector<uint32_t>& materialTableIds) const
{
    struct RigidRouteGeometryRange
    {
        uint32_t vertexOffset = 0;
        uint32_t indexOffset = 0;
        uint32_t triangleOffset = 0;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
        uint32_t triangleCount = 0;
        uint32_t materialId = 0;
        uint32_t materialIndex = 0;
        uint32_t triangleClassAndFlags = 0;
    };

    RtPathTraceRigidRouteBuild build;
    std::vector<PathTraceSmokeVertex> localVertices;
    std::vector<uint32_t> localIndexes;
    std::unordered_map<uint64, RigidRouteGeometryRange> emittedGeometryRanges;
    std::unordered_set<uint64> emittedMeshHashes;

    build.stats.visibleInstances = plan.visibleInstances;
    build.stats.skippedNonRigid = plan.rejectedNonRigid;
    build.stats.skippedMissingMesh = plan.rejectedMissingMesh + plan.rejectedStaleMesh;
    build.stats.skippedMissingBlas = plan.rejectedMissingBlas;
    for (const RtSmokePlanTlasInstance& plannedInstance : plan.instances)
    {
        if (plannedInstance.routeRecordIndex >= m_rigidMeshCandidateRecords.size())
        {
            ++build.stats.skippedMissingMesh;
            AppendRigidRoutePlaceholder(build, plannedInstance);
            continue;
        }

        const RigidMeshCandidateRecord& record = m_rigidMeshCandidateRecords[plannedInstance.routeRecordIndex];
        if (!RigidPlanInstanceMatchesRecord(plannedInstance, record))
        {
            ++build.stats.skippedMissingMesh;
            AppendRigidRoutePlaceholder(build, plannedInstance);
            continue;
        }
        if (!RigidCachedTlasInstanceValid(plannedInstance, record))
        {
            ++build.stats.skippedMissingBlas;
            AppendRigidRoutePlaceholder(build, plannedInstance);
            continue;
        }

        const RigidRouteGeometryRange* sharedGeometry = nullptr;
        const std::unordered_map<uint64, RigidRouteGeometryRange>::const_iterator sharedIt = emittedGeometryRanges.find(record.meshHash);
        if (sharedIt != emittedGeometryRanges.end())
        {
            sharedGeometry = &sharedIt->second;
        }

        RigidRouteGeometryRange geometryRange;
        if (sharedGeometry)
        {
            geometryRange = *sharedGeometry;
        }
        else
        {
            localVertices.clear();
            localIndexes.clear();
            if (!BuildRigidLocalMeshData(record, localVertices, localIndexes))
            {
                ++build.stats.skippedMissingMesh;
                AppendRigidRoutePlaceholder(build, plannedInstance);
                continue;
            }

            geometryRange.vertexOffset = static_cast<uint32_t>(build.vertices.size());
            geometryRange.indexOffset = static_cast<uint32_t>(build.indexes.size());
            geometryRange.triangleOffset = static_cast<uint32_t>(build.triangleMaterials.size());
            geometryRange.vertexCount = static_cast<uint32_t>(localVertices.size());
            geometryRange.indexCount = static_cast<uint32_t>(localIndexes.size());
            geometryRange.triangleCount = static_cast<uint32_t>(localIndexes.size() / 3);
            geometryRange.materialId = record.materialId;
            geometryRange.materialIndex = FindRigidRouteMaterialTableIndex(materialTableIds, record.materialId, build.stats.missingMaterialTableIndex);
            geometryRange.triangleClassAndFlags = record.triangleClassAndFlags != 0u ? record.triangleClassAndFlags : record.surfaceClassId;

            build.vertices.insert(build.vertices.end(), localVertices.begin(), localVertices.end());
            build.indexes.insert(build.indexes.end(), localIndexes.begin(), localIndexes.end());
            for (uint32_t triangleIndex = 0; triangleIndex < geometryRange.triangleCount; ++triangleIndex)
            {
                build.triangleMaterials.push_back(geometryRange.materialId);
                build.triangleMaterialIndexes.push_back(geometryRange.materialIndex);
                build.triangleClassAndFlags.push_back(geometryRange.triangleClassAndFlags);
            }
            emittedGeometryRanges[record.meshHash] = geometryRange;
        }

        PathTraceRigidRouteInstance routeInstance;
        routeInstance.vertexOffset = geometryRange.vertexOffset;
        routeInstance.indexOffset = geometryRange.indexOffset;
        routeInstance.triangleOffset = geometryRange.triangleOffset;
        routeInstance.materialId = geometryRange.materialId;
        routeInstance.materialIndex = geometryRange.materialIndex;
        routeInstance.vertexCount = geometryRange.vertexCount;
        routeInstance.indexCount = geometryRange.indexCount;
        routeInstance.triangleCount = geometryRange.triangleCount;
        routeInstance.instanceIdLo = static_cast<uint32_t>(plannedInstance.sourceInstanceId & 0xffffffffull);
        routeInstance.instanceIdHi = static_cast<uint32_t>((plannedInstance.sourceInstanceId >> 32) & 0xffffffffull);
        if (plannedInstance.hasPreviousTransform)
        {
            routeInstance.flags |= PT_RIGID_ROUTE_HAS_PREVIOUS_TRANSFORM;
            ++build.stats.previousTransformInstances;
        }
        if (plannedInstance.transformContinuous)
        {
            routeInstance.flags |= PT_RIGID_ROUTE_TRANSFORM_CONTINUOUS;
            ++build.stats.transformContinuousInstances;
        }
        if (!plannedInstance.sourceSeenThisFrame)
        {
            routeInstance.flags |= PT_RIGID_ROUTE_CACHED_SOURCE;
        }
        CopyRigidRouteTransformRows(routeInstance.currentObjectToWorld, plannedInstance.transform);
        CopyRigidRouteTransformRows(routeInstance.previousObjectToWorld, plannedInstance.hasPreviousTransform ? plannedInstance.previousTransform : plannedInstance.transform);
        build.instances.push_back(routeInstance);
        build.instanceSeenThisFrame.push_back(plannedInstance.sourceSeenThisFrame ? 1u : 0u);
        std::array<float, 16> objectToWorld = {};
        for (int elementIndex = 0; elementIndex < 16; ++elementIndex)
        {
            objectToWorld[elementIndex] = plannedInstance.transform[elementIndex];
        }
        build.instanceObjectToWorld.push_back(objectToWorld);

        ++build.stats.emittedInstances;
        if (emittedMeshHashes.insert(record.meshHash).second)
        {
            ++build.stats.emittedUniqueMeshes;
        }
        if (plannedInstance.sourceSeenThisFrame)
        {
            ++build.stats.emittedSeenThisFrame;
        }
        else
        {
            ++build.stats.emittedFromCache;
        }
        if (!sharedGeometry)
        {
            build.stats.vertices += static_cast<int>(geometryRange.vertexCount);
            build.stats.indexes += static_cast<int>(geometryRange.indexCount);
            build.stats.triangles += static_cast<int>(geometryRange.triangleCount);
        }
    }

    return build;
}

RtPathTraceRigidRouteBuild RtSmokeGeometryUniverse::BuildRigidRouteBuffers(
    const RtPathTraceInstanceUniverse& instanceUniverse,
    const std::vector<uint32_t>& materialTableIds,
    int maxInstances) const
{
    const RtSmokeRigidTlasPlan plan =
        BuildRigidTlasInstancePlan(instanceUniverse, 2, 0x02, maxInstances);
    return BuildRigidRouteBuffers(plan, materialTableIds);
}
