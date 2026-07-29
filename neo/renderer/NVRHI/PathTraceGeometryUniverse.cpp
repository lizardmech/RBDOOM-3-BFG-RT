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

nvrhi::BufferHandle CreateRigidSmokeBuffer(
    nvrhi::IDevice* device,
    const char* debugName,
    size_t byteSize,
    uint32_t structStride,
    bool vertexBuffer,
    bool indexBuffer,
    bool accelStructBuildInput = true)
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
    desc.isAccelStructBuildInput = accelStructBuildInput;
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

uint64 BuildCanonicalRigidBlasInputSignature(
    const PtGeometrySourceRecord& source,
    const PtGeometryGpuPoolRecord& gpu,
    const PtGeometryGpuPoolSet& pools)
{
    uint64 hash = 14695981039346656037ull;
    hash = HashSmokeBytes(
        hash,
        &source.sourceContentRevision,
        sizeof(source.sourceContentRevision));
    hash = HashSmokeBytes(
        hash,
        &source.sourceChecksum,
        sizeof(source.sourceChecksum));
    hash = HashSmokeBytes(hash, &gpu.positions, sizeof(gpu.positions));
    hash = HashSmokeBytes(hash, &gpu.indexes, sizeof(gpu.indexes));
    const uintptr_t positionBuffer =
        reinterpret_cast<uintptr_t>(pools.PositionBuffer().Get());
    const uintptr_t indexBuffer =
        reinterpret_cast<uintptr_t>(pools.IndexBuffer().Get());
    hash = HashSmokeBytes(
        hash,
        &positionBuffer,
        sizeof(positionBuffer));
    hash = HashSmokeBytes(
        hash,
        &indexBuffer,
        sizeof(indexBuffer));
    return hash != 0 ? hash : 1;
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

uint64 BuildCanonicalCompareTopologySignature(
    const std::vector<uint32_t>& indexes,
    size_t vertexCount)
{
    if (vertexCount == 0 || indexes.size() < 3)
    {
        return 0;
    }

    const auto hashValue = [](uint64 hash, uint64 value) -> uint64 {
        for (int byteIndex = 0; byteIndex < 8; ++byteIndex)
        {
            hash ^= static_cast<uint8_t>(value & 0xffu);
            hash *= 1099511628211ull;
            value >>= 8u;
        }
        return hash;
    };

    uint64 hash = 1469598103934665603ull;
    hash = hashValue(hash, static_cast<uint32_t>(vertexCount));
    hash = hashValue(hash, static_cast<uint32_t>(indexes.size()));
    for (uint32_t index : indexes)
    {
        hash = hashValue(hash, index);
    }
    return hash != 0 ? hash : 1;
}

bool BuildCanonicalComparePayload(
    const RtSmokeGeometryUniverse::RigidMeshCandidateRecord& record,
    PtGeometrySourcePayload& payload,
    uint64& topologySignature)
{
    payload = PtGeometrySourcePayload();
    topologySignature = 0;
    if (record.modelSurfaceIndex < 0 ||
        record.cachedLocalVertices.empty() ||
        record.cachedLocalIndexes.size() < 3 ||
        (record.cachedLocalIndexes.size() % 3) != 0 ||
        static_cast<int>(record.cachedLocalVertices.size()) !=
            record.sourceRange.vertices.count ||
        static_cast<int>(record.cachedLocalIndexes.size()) !=
            record.sourceRange.indexes.count)
    {
        return false;
    }

    payload.positions.resize(record.cachedLocalVertices.size());
    payload.attributes.resize(record.cachedLocalVertices.size());
    for (size_t vertexIndex = 0;
        vertexIndex < record.cachedLocalVertices.size();
        ++vertexIndex)
    {
        const PathTraceSmokeVertex& source =
            record.cachedLocalVertices[vertexIndex];
        PtGeometrySourcePosition& position =
            payload.positions[vertexIndex];
        position.xyz[0] = source.position[0];
        position.xyz[1] = source.position[1];
        position.xyz[2] = source.position[2];

        PtGeometrySourceAttribute& attribute =
            payload.attributes[vertexIndex];
        memcpy(attribute.normal, source.normal, sizeof(attribute.normal));
        attribute.texCoord[0] = source.texCoord[0];
        attribute.texCoord[1] = source.texCoord[1];
        memcpy(attribute.color, source.color, sizeof(attribute.color));
        memcpy(attribute.color2, source.color2, sizeof(attribute.color2));
        attribute.tangent[0] = source.tangent[0];
        attribute.tangent[1] = source.tangent[1];
        attribute.tangent[2] = source.tangent[2];
        attribute.bitangent[0] = source.bitangent[0];
        attribute.bitangent[1] = source.bitangent[1];
        attribute.bitangent[2] = source.bitangent[2];
        attribute.bitangentSign = source.tangent[3];
    }

    payload.indexes = record.cachedLocalIndexes;
    for (uint32_t index : payload.indexes)
    {
        if (index >= payload.positions.size())
        {
            payload = PtGeometrySourcePayload();
            return false;
        }
    }
    payload.triangles.resize(payload.indexes.size() / 3);
    for (PtGeometrySourceTriangle& triangle : payload.triangles)
    {
        triangle.sourceMaterialSlot =
            static_cast<uint32_t>(record.modelSurfaceIndex);
    }
    topologySignature = BuildCanonicalCompareTopologySignature(
        payload.indexes,
        payload.positions.size());
    return topologySignature != 0;
}

PtGeometrySourcePayloadView CanonicalComparePayloadView(
    const PtGeometrySourcePayload& payload)
{
    PtGeometrySourcePayloadView view;
    view.positions = payload.positions.data();
    view.positionCount = payload.positions.size();
    view.attributes = payload.attributes.data();
    view.attributeCount = payload.attributes.size();
    view.indexes = payload.indexes.data();
    view.indexCount = payload.indexes.size();
    view.triangles = payload.triangles.data();
    view.triangleCount = payload.triangles.size();
    return view;
}

bool CanonicalCompareEndpointsMatch(
    const PtGeometrySourcePayload& legacy,
    const PtGeometrySourcePayload& canonical)
{
    if (legacy.positions.empty() ||
        legacy.positions.size() != canonical.positions.size() ||
        legacy.attributes.size() != canonical.attributes.size() ||
        legacy.indexes.empty() ||
        legacy.indexes.size() != canonical.indexes.size() ||
        legacy.triangles.empty() ||
        legacy.triangles.size() != canonical.triangles.size())
    {
        return false;
    }

    return memcmp(
               &legacy.positions.front(),
               &canonical.positions.front(),
               sizeof(PtGeometrySourcePosition)) == 0 &&
        memcmp(
            &legacy.positions.back(),
            &canonical.positions.back(),
            sizeof(PtGeometrySourcePosition)) == 0 &&
        memcmp(
            &legacy.attributes.front(),
            &canonical.attributes.front(),
            sizeof(PtGeometrySourceAttribute)) == 0 &&
        memcmp(
            &legacy.attributes.back(),
            &canonical.attributes.back(),
            sizeof(PtGeometrySourceAttribute)) == 0 &&
        legacy.indexes.front() == canonical.indexes.front() &&
        legacy.indexes.back() == canonical.indexes.back() &&
        memcmp(
            &legacy.triangles.front(),
            &canonical.triangles.front(),
            sizeof(PtGeometrySourceTriangle)) == 0 &&
        memcmp(
            &legacy.triangles.back(),
            &canonical.triangles.back(),
            sizeof(PtGeometrySourceTriangle)) == 0;
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
    // Scene clears can occur while a TLAS from the previous scene is still
    // referenced by an in-flight frame. Use the same delayed-retirement path
    // as a canonical source-publication replacement instead of dropping BLAS
    // handles and their retirement queue immediately.
    ReleaseCanonicalRigidBlasScaffold();
    ReleaseStaticBucketBlasGpuScaffold();
    const uint64 canonicalRigidBlasRetired =
        m_canonicalRigidBlasStats.blasRetired;

    m_currentFrameIndex = 0;
    m_frameActive = false;
    m_staticSurfaceRecords.clear();
    m_staticSurfaceLookup.clear();
    m_staticSurfaceKeys.clear();
    m_staticVertexCache.clear();
    m_staticIndexCache.clear();
    m_staticTriangleClassCache.clear();
    m_staticTriangleMaterialCache.clear();
    m_staticBucketAssignmentPlanCache =
        RtSmokeStaticBucketAssignmentPlan();
    m_staticBucketAssignmentWorldGeneration = 0;
    m_staticBucketAssignmentSourceGeneration = 0;
    m_staticBucketAssignmentStorageGeneration = 0;
    m_staticBucketAssignmentPortalAreaCount = 0;
    m_staticBucketAssignmentMaxVertices = 0;
    m_staticBucketAssignmentMaxIndexes = 0;
    m_staticBucketAssignmentMaxTriangles = 0;
    m_staticBucketAssignmentPlanCacheValid = false;
    m_staticBucketResidentGeometryPack =
        RtSmokeStaticBucketGeometryPack();
    m_staticBucketResidentAssignmentPlanSignature = 0;
    m_staticBucketResidentGeometryGeneration = 0;
    m_staticBucketResidentMaterialGeneration = 0;
    m_staticBucketResidentGeometryPackValid = false;
    m_staticBucketMaterialIndexes.clear();
    m_staticBucketMissingMaterialIndexesByBucket.clear();
    m_staticBucketReferencedMaterialIds.clear();
    m_staticBucketReferencedMaterialIdsResidentPackSignature = 0;
    m_staticBucketMaterialIndexResidentPackSignature = 0;
    m_staticBucketMaterialIndexBindingSignature = 0;
    m_staticBucketMaterialIndexCacheValid = false;
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
    m_canonicalIdentityRegistry.Clear();
    m_canonicalRigidBlasStats =
        RtPathTraceCanonicalRigidBlasStats();
    m_canonicalRigidBlasStats.blasRetired =
        canonicalRigidBlasRetired;
    ClearRigidResidencyCaches();
    m_rigidResidencyStats = RtPathTraceRigidResidencyStats();
    m_rigidResidencyEnabled = false;
    m_rigidResidencyWorld = nullptr;
    ResetRigidMeshCandidateFrameStats();
    ++m_generation;
}

void RtSmokeGeometryUniverse::ImportCanonicalIdentitySnapshot(
    const PtGeometryIdentityTransportSnapshot* snapshot)
{
    if (snapshot == nullptr)
    {
        return;
    }
    m_canonicalIdentityRegistry.ApplySnapshot(snapshot);
}

const PtGeometryIdentityBinding*
RtSmokeGeometryUniverse::FindCanonicalIdentityBinding(
    const PtCanonicalInstanceKey& key) const
{
    return m_canonicalIdentityRegistry.Find(key);
}

const PtGeometrySourceRecord*
RtSmokeGeometryUniverse::FindCanonicalSourceRecord(
    const PtCanonicalMeshKey& key) const
{
    return m_canonicalSourceRegistry.Find(key);
}

const PtGeometryGpuPoolRecord*
RtSmokeGeometryUniverse::FindCanonicalSourceGpuRecord(
    const PtCanonicalMeshKey& key) const
{
    for (size_t recordIndex = 0;
        recordIndex <
            m_canonicalSourceGpuPools.RecordCount();
        ++recordIndex)
    {
        const PtGeometryGpuPoolRecord* record =
            m_canonicalSourceGpuPools.RecordAt(recordIndex);
        if (record != nullptr && record->key == key)
        {
            return record;
        }
    }
    return nullptr;
}

uint64
RtSmokeGeometryUniverse::CanonicalSourceIndexPoolGeneration()
    const
{
    return m_canonicalSourceGpuPoolStats.generations[2];
}

uint64
RtSmokeGeometryUniverse::CanonicalSourceIndexPoolCapacityBytes()
    const
{
    return m_canonicalSourceGpuPoolStats.capacities[2];
}

nvrhi::BufferHandle
RtSmokeGeometryUniverse::CanonicalSourceIndexBuffer() const
{
    return m_canonicalSourceGpuPools.IndexBuffer();
}

void RtSmokeGeometryUniverse::DumpCanonicalIdentityImportStats()
{
    const PtGeometryIdentityRegistryStats& stats =
        m_canonicalIdentityRegistry.Stats();
    common->Printf(
        "PathTracePrimaryPass: GEO06 backend identity transport world/pub/seq/cursor=%llu/%llu/%llu/%llu bindings(active/static/rigid/skinned/slots)=%llu/%llu/%llu/%llu/%llu interval(upsert/reuse/revise/remove/reject/bytes)=%llu/%llu/%llu/%llu/%llu/%llu authority=frontend-lifecycle route=shadow-only\n",
        static_cast<unsigned long long>(stats.worldGeneration),
        static_cast<unsigned long long>(stats.publicationGeneration),
        static_cast<unsigned long long>(stats.publicationSequence),
        static_cast<unsigned long long>(stats.importCursor),
        static_cast<unsigned long long>(stats.activeBindings),
        static_cast<unsigned long long>(stats.activeStaticBindings),
        static_cast<unsigned long long>(stats.activeRigidBindings),
        static_cast<unsigned long long>(stats.activeSkinnedBindings),
        static_cast<unsigned long long>(stats.bindingSlots),
        static_cast<unsigned long long>(stats.upserts),
        static_cast<unsigned long long>(stats.reused),
        static_cast<unsigned long long>(stats.revised),
        static_cast<unsigned long long>(stats.removed),
        static_cast<unsigned long long>(stats.rejected),
        static_cast<unsigned long long>(stats.transportBytes));
    m_canonicalIdentityRegistry.ResetIntervalStats();
}

void RtSmokeGeometryUniverse::ImportCanonicalSourceSnapshot(
    const PtGeometrySourceTransportSnapshot* snapshot)
{
    if (snapshot == nullptr)
    {
        return;
    }
    if (snapshot->worldGeneration == 0 ||
        snapshot->publicationGeneration == 0 ||
        snapshot->records == nullptr ||
        snapshot->recordCount == 0 ||
        snapshot->nextRecordIndex < snapshot->firstRecordIndex ||
        snapshot->nextRecordIndex - snapshot->firstRecordIndex !=
            snapshot->recordCount)
    {
        ++m_canonicalSourceRejected;
        return;
    }

    // Validate the entire immutable packet before changing publication
    // ownership or publishing any record. A malformed first packet for a new
    // generation must not discard the last valid registry and GPU pools.
    for (std::uint64_t recordIndex = 0;
        recordIndex < snapshot->recordCount;
        ++recordIndex)
    {
        if (PtValidateGeometrySourceTransportRecord(
                snapshot->records[recordIndex],
                snapshot->streams) !=
            PtGeometrySourceTransportResult::Success)
        {
            ++m_canonicalSourceRejected;
            return;
        }
    }

    if (m_canonicalSourceWorldGeneration != snapshot->worldGeneration ||
        m_canonicalSourcePublicationGeneration !=
            snapshot->publicationGeneration)
    {
        ReleaseCanonicalRigidBlasScaffold();
        m_canonicalSourceGpuPools.ResetForPublication(m_currentFrameIndex);
        m_canonicalSourceGpuPoolStats = PtGeometryGpuPoolStats();
        m_canonicalSourceRegistry.Clear();
        m_canonicalSourceWorldGeneration = snapshot->worldGeneration;
        m_canonicalSourcePublicationGeneration =
            snapshot->publicationGeneration;
        m_canonicalSourcePublicationSequence = 0;
        m_canonicalSourceImportCursor = 0;
    }
    if (snapshot->publicationSequence <=
            m_canonicalSourcePublicationSequence ||
        snapshot->firstRecordIndex != m_canonicalSourceImportCursor)
    {
        ++m_canonicalSourceRejected;
        return;
    }

    for (std::uint64_t recordIndex = 0;
        recordIndex < snapshot->recordCount;
        ++recordIndex)
    {
        const PtGeometrySourceTransportRecord& transport =
            snapshot->records[recordIndex];
        PtGeometrySourcePayloadView payload;
        payload.positions =
            snapshot->streams.positions + transport.positionOffset;
        payload.positionCount = transport.key.vertexCount;
        payload.attributes =
            snapshot->streams.attributes + transport.attributeOffset;
        payload.attributeCount = transport.key.vertexCount;
        payload.indexes =
            snapshot->streams.indexes + transport.indexOffset;
        payload.indexCount = transport.key.indexCount;
        payload.triangles =
            snapshot->streams.triangles + transport.triangleOffset;
        payload.triangleCount = transport.key.indexCount / 3u;
        const PtGeometrySourceObserveResult observe =
            m_canonicalSourceRegistry.Observe(
                transport.key,
                transport.sourceContentRevision,
                &payload);
        switch (observe)
        {
            case PtGeometrySourceObserveResult::Added:
                ++m_canonicalSourceImported;
                break;
            case PtGeometrySourceObserveResult::Reused:
                ++m_canonicalSourceReused;
                break;
            case PtGeometrySourceObserveResult::Revised:
                ++m_canonicalSourceRevised;
                break;
            default:
                ++m_canonicalSourceRejected;
                return;
        }
        const PtGeometrySourceRecord* imported =
            m_canonicalSourceRegistry.Find(transport.key);
        if (imported == nullptr ||
            imported->sourceChecksum != transport.sourceChecksum)
        {
            ++m_canonicalSourceRejected;
            return;
        }
    }

    m_canonicalSourcePublicationSequence = snapshot->publicationSequence;
    m_canonicalSourceImportCursor = snapshot->nextRecordIndex;
    m_canonicalSourceTransportBytes += snapshot->packedBytes;
}

void RtSmokeGeometryUniverse::UpdateCanonicalSourceGpuPools(
    nvrhi::IDevice* device,
    nvrhi::ICommandList* commandList)
{
    const PtGeometryGpuPoolStats frameStats =
        m_canonicalSourceGpuPools.Update(
            device,
            commandList,
            m_canonicalSourceRegistry,
            m_currentFrameIndex);
    m_canonicalSourceGpuPoolStats.residentRecords =
        frameStats.residentRecords;
    m_canonicalSourceGpuPoolStats.uploadedRecords +=
        frameStats.uploadedRecords;
    m_canonicalSourceGpuPoolStats.revisedRecords +=
        frameStats.revisedRecords;
    m_canonicalSourceGpuPoolStats.rejectedRecords +=
        frameStats.rejectedRecords;
    m_canonicalSourceGpuPoolStats.uploadBytes += frameStats.uploadBytes;
    m_canonicalSourceGpuPoolStats.copiedGrowthBytes +=
        frameStats.copiedGrowthBytes;
    m_canonicalSourceGpuPoolStats.buffersCreated +=
        frameStats.buffersCreated;
    m_canonicalSourceGpuPoolStats.buffersGrown +=
        frameStats.buffersGrown;
    m_canonicalSourceGpuPoolStats.retiredBuffers =
        frameStats.retiredBuffers;
    for (int poolIndex = 0; poolIndex < 4; ++poolIndex)
    {
        m_canonicalSourceGpuPoolStats.capacities[poolIndex] =
            frameStats.capacities[poolIndex];
        m_canonicalSourceGpuPoolStats.used[poolIndex] =
            frameStats.used[poolIndex];
        m_canonicalSourceGpuPoolStats.generations[poolIndex] =
            frameStats.generations[poolIndex];
    }
    m_canonicalOffsetBlasProbe.Update(
        device,
        commandList,
        m_canonicalSourceRegistry,
        m_canonicalSourceGpuPools,
        m_currentFrameIndex);
}

void RtSmokeGeometryUniverse::DumpCanonicalSourceImportStats()
{
    common->Printf(
        "PathTracePrimaryPass: GEO06 backend source import world=%llu publication=%llu:%llu cursor=%llu records=%llu retainedBytes=%llu interval(import/reuse/revise/reject/transportBytes)=%llu/%llu/%llu/%llu/%llu route=observation-only\n",
        static_cast<unsigned long long>(m_canonicalSourceWorldGeneration),
        static_cast<unsigned long long>(
            m_canonicalSourcePublicationGeneration),
        static_cast<unsigned long long>(m_canonicalSourcePublicationSequence),
        static_cast<unsigned long long>(m_canonicalSourceImportCursor),
        static_cast<unsigned long long>(m_canonicalSourceRegistry.RecordCount()),
        static_cast<unsigned long long>(
            m_canonicalSourceRegistry.Stats().retainedBytes),
        static_cast<unsigned long long>(m_canonicalSourceImported),
        static_cast<unsigned long long>(m_canonicalSourceReused),
        static_cast<unsigned long long>(m_canonicalSourceRevised),
        static_cast<unsigned long long>(m_canonicalSourceRejected),
        static_cast<unsigned long long>(m_canonicalSourceTransportBytes));
    m_canonicalSourceImported = 0;
    m_canonicalSourceReused = 0;
    m_canonicalSourceRevised = 0;
    m_canonicalSourceRejected = 0;
    m_canonicalSourceTransportBytes = 0;
}

void RtSmokeGeometryUniverse::DumpCanonicalSourceGpuPoolStats()
{
    common->Printf(
        "PathTracePrimaryPass: GEO06 backend GPU source pools records=%llu interval(upload/revise/reject/uploadBytes/copyBytes/create/grow)=%llu/%llu/%llu/%llu/%llu/%llu/%llu retired=%llu position(cap/used/gen)=%llu/%llu/%llu attribute=%llu/%llu/%llu index=%llu/%llu/%llu triangle=%llu/%llu/%llu route=shadow-only\n",
        static_cast<unsigned long long>(
            m_canonicalSourceGpuPoolStats.residentRecords),
        static_cast<unsigned long long>(
            m_canonicalSourceGpuPoolStats.uploadedRecords),
        static_cast<unsigned long long>(
            m_canonicalSourceGpuPoolStats.revisedRecords),
        static_cast<unsigned long long>(
            m_canonicalSourceGpuPoolStats.rejectedRecords),
        static_cast<unsigned long long>(
            m_canonicalSourceGpuPoolStats.uploadBytes),
        static_cast<unsigned long long>(
            m_canonicalSourceGpuPoolStats.copiedGrowthBytes),
        static_cast<unsigned long long>(
            m_canonicalSourceGpuPoolStats.buffersCreated),
        static_cast<unsigned long long>(
            m_canonicalSourceGpuPoolStats.buffersGrown),
        static_cast<unsigned long long>(
            m_canonicalSourceGpuPoolStats.retiredBuffers),
        static_cast<unsigned long long>(
            m_canonicalSourceGpuPoolStats.capacities[0]),
        static_cast<unsigned long long>(
            m_canonicalSourceGpuPoolStats.used[0]),
        static_cast<unsigned long long>(
            m_canonicalSourceGpuPoolStats.generations[0]),
        static_cast<unsigned long long>(
            m_canonicalSourceGpuPoolStats.capacities[1]),
        static_cast<unsigned long long>(
            m_canonicalSourceGpuPoolStats.used[1]),
        static_cast<unsigned long long>(
            m_canonicalSourceGpuPoolStats.generations[1]),
        static_cast<unsigned long long>(
            m_canonicalSourceGpuPoolStats.capacities[2]),
        static_cast<unsigned long long>(
            m_canonicalSourceGpuPoolStats.used[2]),
        static_cast<unsigned long long>(
            m_canonicalSourceGpuPoolStats.generations[2]),
        static_cast<unsigned long long>(
            m_canonicalSourceGpuPoolStats.capacities[3]),
        static_cast<unsigned long long>(
            m_canonicalSourceGpuPoolStats.used[3]),
        static_cast<unsigned long long>(
            m_canonicalSourceGpuPoolStats.generations[3]));
    m_canonicalSourceGpuPoolStats.uploadedRecords = 0;
    m_canonicalSourceGpuPoolStats.revisedRecords = 0;
    m_canonicalSourceGpuPoolStats.rejectedRecords = 0;
    m_canonicalSourceGpuPoolStats.uploadBytes = 0;
    m_canonicalSourceGpuPoolStats.copiedGrowthBytes = 0;
    m_canonicalSourceGpuPoolStats.buffersCreated = 0;
    m_canonicalSourceGpuPoolStats.buffersGrown = 0;
}

void RtSmokeGeometryUniverse::DumpCanonicalOffsetBlasProbeStats()
{
    const PtGeometryOffsetBlasProbeStats& stats =
        m_canonicalOffsetBlasProbe.Stats();
    common->Printf(
        "PathTracePrimaryPass: GEO06 pooled offset BLAS probe sources/eligible/selected=%llu/%llu/%llu signature=%llu stableFrames=%llu offsets(position/attribute/index/triangle)=%llu/%llu/%llu/%llu counts(v/i/p)=%llu/%llu/%llu material(first/last)=%u/%u blas(valid/create/build/retire)=%d/%llu/%llu/%llu readback(pending/pass/fail/endpointsValid)=%d/%llu/%llu/%d buildSubmitUs=%llu route=shadow-only\n",
        static_cast<unsigned long long>(stats.sourceRecords),
        static_cast<unsigned long long>(stats.eligibleRecords),
        static_cast<unsigned long long>(stats.selectedSourceIndex),
        static_cast<unsigned long long>(stats.candidateSignature),
        static_cast<unsigned long long>(stats.stableFrames),
        static_cast<unsigned long long>(stats.positionOffsetBytes),
        static_cast<unsigned long long>(stats.attributeOffsetBytes),
        static_cast<unsigned long long>(stats.indexOffsetBytes),
        static_cast<unsigned long long>(stats.triangleOffsetBytes),
        static_cast<unsigned long long>(stats.vertexCount),
        static_cast<unsigned long long>(stats.indexCount),
        static_cast<unsigned long long>(stats.primitiveCount),
        stats.firstMaterialSlot,
        stats.lastMaterialSlot,
        stats.blasValid ? 1 : 0,
        static_cast<unsigned long long>(stats.blasCreated),
        static_cast<unsigned long long>(stats.blasBuilt),
        static_cast<unsigned long long>(stats.blasRetired),
        stats.readbackPending ? 1 : 0,
        static_cast<unsigned long long>(stats.readbacksPassed),
        static_cast<unsigned long long>(stats.readbacksFailed),
        stats.endpointBytesValid ? 1 : 0,
        static_cast<unsigned long long>(stats.buildSubmitMicroseconds));
}

RtPathTraceCanonicalRigidCompareStats
RtSmokeGeometryUniverse::BuildCanonicalRigidSourceCompareStats() const
{
    RtPathTraceCanonicalRigidCompareStats stats;
    stats.frameIndex = m_currentFrameIndex;
    std::vector<bool> canonicalMatched(
        m_canonicalSourceRegistry.RecordCount(),
        false);
    for (size_t sourceIndex = 0;
        sourceIndex < m_canonicalSourceRegistry.RecordCount();
        ++sourceIndex)
    {
        const PtGeometrySourceRecord* source =
            m_canonicalSourceRegistry.RecordAt(sourceIndex);
        if (source != nullptr &&
            source->key.sourceDomain ==
                PtCanonicalMeshSourceDomain::RegisteredRenderModel &&
            source->key.deformationClass ==
                PtCanonicalDeformationClass::Rigid)
        {
            ++stats.canonicalRigidSources;
        }
    }

    const auto appendSample =
        [&stats](const RtPathTraceCanonicalRigidCompareSample& sample) {
            const uint32_t failureMask =
                RT_PT_CANONICAL_RIGID_COMPARE_INVALID_LEGACY |
                RT_PT_CANONICAL_RIGID_COMPARE_MISSING_SHAPE |
                RT_PT_CANONICAL_RIGID_COMPARE_PAYLOAD_MISMATCH |
                RT_PT_CANONICAL_RIGID_COMPARE_ENDPOINT_MISMATCH |
                RT_PT_CANONICAL_RIGID_COMPARE_SOURCE_MATERIAL_MISMATCH;
            if (stats.sampleCount <
                RT_PT_CANONICAL_RIGID_COMPARE_SAMPLES)
            {
                stats.samples[stats.sampleCount++] = sample;
                return;
            }
            if ((sample.flags & failureMask) == 0)
            {
                return;
            }
            for (int sampleIndex = stats.sampleCount - 1;
                sampleIndex >= 0;
                --sampleIndex)
            {
                if ((stats.samples[sampleIndex].flags & failureMask) == 0)
                {
                    stats.samples[sampleIndex] = sample;
                    return;
                }
            }
        };

    for (const RigidMeshCandidateRecord& record :
        m_rigidMeshCandidateRecords)
    {
        if (!record.valid ||
            (record.sourceFlags & RT_PT_INSTANCE_SOURCE_RIGID) == 0)
        {
            continue;
        }
        ++stats.legacyRigidRecords;
        if (record.materialId != 0)
        {
            ++stats.resolvedMaterialBindings;
        }
        if (record.triangleClassAndFlags != 0)
        {
            ++stats.resolvedTriangleClassBindings;
        }

        RtPathTraceCanonicalRigidCompareSample sample;
        sample.valid = true;
        sample.legacyMeshHash = record.meshHash;
        sample.modelSurfaceIndex = record.modelSurfaceIndex;
        sample.vertexCount = record.sourceRange.vertices.count;
        sample.indexCount = record.sourceRange.indexes.count;
        sample.triangleCount = record.sourceRange.triangles.count;
        sample.resolvedMaterialId = record.materialId;
        sample.resolvedTriangleClassAndFlags =
            record.triangleClassAndFlags;
        sample.materialName = record.materialName;
        sample.modelName = record.modelName;

        PtGeometrySourcePayload legacyPayload;
        uint64 topologySignature = 0;
        if (!BuildCanonicalComparePayload(
                record,
                legacyPayload,
                topologySignature))
        {
            sample.flags |=
                RT_PT_CANONICAL_RIGID_COMPARE_INVALID_LEGACY;
            appendSample(sample);
            continue;
        }
        ++stats.validLegacyPayloads;
        sample.legacyChecksum = PtChecksumGeometrySourcePayload(
            CanonicalComparePayloadView(legacyPayload));

        std::vector<size_t> exactSourceIndexes;
        const PtGeometrySourceRecord* firstShapeSource = nullptr;
        for (size_t sourceIndex = 0;
            sourceIndex < m_canonicalSourceRegistry.RecordCount();
            ++sourceIndex)
        {
            const PtGeometrySourceRecord* source =
                m_canonicalSourceRegistry.RecordAt(sourceIndex);
            if (source == nullptr ||
                source->key.sourceDomain !=
                    PtCanonicalMeshSourceDomain::RegisteredRenderModel ||
                source->key.deformationClass !=
                    PtCanonicalDeformationClass::Rigid ||
                source->key.modelSurfaceIndex !=
                    static_cast<uint32_t>(record.modelSurfaceIndex) ||
                source->key.vertexCount !=
                    legacyPayload.positions.size() ||
                source->key.indexCount !=
                    legacyPayload.indexes.size() ||
                source->key.topologySignature != topologySignature)
            {
                continue;
            }

            ++sample.shapeMatches;
            if (firstShapeSource == nullptr)
            {
                firstShapeSource = source;
                sample.canonicalChecksum = source->sourceChecksum;
                sample.sourceAssetId = source->key.sourceAssetId;
            }
            if (source->sourceChecksum == sample.legacyChecksum)
            {
                exactSourceIndexes.push_back(sourceIndex);
            }
        }

        sample.exactPayloadMatches =
            static_cast<int>(exactSourceIndexes.size());
        if (sample.shapeMatches == 0)
        {
            ++stats.missingShape;
            sample.flags |=
                RT_PT_CANONICAL_RIGID_COMPARE_MISSING_SHAPE;
            appendSample(sample);
            continue;
        }
        if (exactSourceIndexes.empty())
        {
            ++stats.payloadMismatch;
            sample.flags |=
                RT_PT_CANONICAL_RIGID_COMPARE_PAYLOAD_MISMATCH;
            appendSample(sample);
            continue;
        }

        ++stats.exactPayloadMatches;
        const PtGeometrySourceRecord* matchedSource =
            m_canonicalSourceRegistry.RecordAt(exactSourceIndexes.front());
        sample.canonicalChecksum = matchedSource
            ? matchedSource->sourceChecksum
            : 0;
        sample.sourceAssetId = matchedSource
            ? matchedSource->key.sourceAssetId
            : 0;
        if (exactSourceIndexes.size() == 1)
        {
            ++stats.uniqueContentMatches;
        }
        else
        {
            ++stats.equivalentContentMatches;
            sample.flags |=
                RT_PT_CANONICAL_RIGID_COMPARE_EQUIVALENT_CONTENT;
        }
        for (size_t sourceIndex : exactSourceIndexes)
        {
            canonicalMatched[sourceIndex] = true;
        }

        if (matchedSource == nullptr ||
            !CanonicalCompareEndpointsMatch(
                legacyPayload,
                matchedSource->payload))
        {
            ++stats.endpointMismatch;
            sample.flags |=
                RT_PT_CANONICAL_RIGID_COMPARE_ENDPOINT_MISMATCH;
        }
        else
        {
            sample.sourceMaterialFirst =
                matchedSource->payload.triangles.front().
                    sourceMaterialSlot;
            sample.sourceMaterialLast =
                matchedSource->payload.triangles.back().
                    sourceMaterialSlot;
            const uint32_t expectedSourceMaterial =
                static_cast<uint32_t>(record.modelSurfaceIndex);
            if (sample.sourceMaterialFirst != expectedSourceMaterial ||
                sample.sourceMaterialLast != expectedSourceMaterial)
            {
                ++stats.sourceMaterialMismatch;
                sample.flags |=
                    RT_PT_CANONICAL_RIGID_COMPARE_SOURCE_MATERIAL_MISMATCH;
            }
        }
        appendSample(sample);
    }

    for (size_t sourceIndex = 0;
        sourceIndex < m_canonicalSourceRegistry.RecordCount();
        ++sourceIndex)
    {
        const PtGeometrySourceRecord* source =
            m_canonicalSourceRegistry.RecordAt(sourceIndex);
        if (source != nullptr &&
            source->key.sourceDomain ==
                PtCanonicalMeshSourceDomain::RegisteredRenderModel &&
            source->key.deformationClass ==
                PtCanonicalDeformationClass::Rigid &&
            !canonicalMatched[sourceIndex])
        {
            ++stats.unmatchedCanonicalSources;
        }
    }
    return stats;
}

void RtSmokeGeometryUniverse::DumpCanonicalRigidSourceCompareStats(
    const RtPathTraceCanonicalRigidCompareStats& stats) const
{
    common->Printf(
        "PathTracePrimaryPass: GEO06 canonical rigid source compare frame=%llu canonical/legacy/valid=%d/%d/%d exact(unique/equivalent)=%d(%d/%d) failures(missingShape/payload/endpoint/sourceMaterial)=%d/%d/%d/%d bindings(material/class)=%d/%d unmatchedCanonical=%d identity=content-only route=shadow-only\n",
        static_cast<unsigned long long>(stats.frameIndex),
        stats.canonicalRigidSources,
        stats.legacyRigidRecords,
        stats.validLegacyPayloads,
        stats.exactPayloadMatches,
        stats.uniqueContentMatches,
        stats.equivalentContentMatches,
        stats.missingShape,
        stats.payloadMismatch,
        stats.endpointMismatch,
        stats.sourceMaterialMismatch,
        stats.resolvedMaterialBindings,
        stats.resolvedTriangleClassBindings,
        stats.unmatchedCanonicalSources);
    for (int sampleIndex = 0;
        sampleIndex < stats.sampleCount;
        ++sampleIndex)
    {
        const RtPathTraceCanonicalRigidCompareSample& sample =
            stats.samples[sampleIndex];
        if (!sample.valid)
        {
            continue;
        }
        common->Printf(
            "PathTracePrimaryPass: GEO06 canonical rigid compare sample %d flags=0x%x legacyMesh=%llu surface=%d sourceAsset=%llu matches(shape/exact)=%d/%d checksum(legacy/canonical)=%llu/%llu counts(v/i/t)=%d/%d/%d sourceMaterial(first/last)=%u/%u resolved(material/class)= %u/0x%x material='%s' model='%s'\n",
            sampleIndex,
            sample.flags,
            static_cast<unsigned long long>(sample.legacyMeshHash),
            sample.modelSurfaceIndex,
            static_cast<unsigned long long>(sample.sourceAssetId),
            sample.shapeMatches,
            sample.exactPayloadMatches,
            static_cast<unsigned long long>(sample.legacyChecksum),
            static_cast<unsigned long long>(sample.canonicalChecksum),
            sample.vertexCount,
            sample.indexCount,
            sample.triangleCount,
            sample.sourceMaterialFirst,
            sample.sourceMaterialLast,
            sample.resolvedMaterialId,
            sample.resolvedTriangleClassAndFlags,
            sample.materialName.c_str(),
            sample.modelName.c_str());
    }
}

RtPathTraceCanonicalRigidIdentityStats
RtSmokeGeometryUniverse::BuildCanonicalRigidIdentityStats(
    const RtPathTraceInstanceUniverse& instanceUniverse) const
{
    RtPathTraceCanonicalRigidIdentityStats stats;
    stats.frameIndex = m_currentFrameIndex;
    std::vector<RtPathTraceRigidRouteInstanceObservation> instances;
    BuildRigidRouteInstanceList(instanceUniverse, instances);
    stats.routeInstances = static_cast<int>(instances.size());

    struct LegacyPayloadSignature
    {
        bool valid = false;
        uint64 checksum = 0;
        uint64 topologySignature = 0;
        uint32_t vertexCount = 0;
        uint32_t indexCount = 0;
    };
    std::unordered_map<uint64, LegacyPayloadSignature> legacySignatures;

    const auto appendSample =
        [&stats](const RtPathTraceCanonicalRigidIdentitySample& sample) {
            if (stats.sampleCount <
                RT_PT_CANONICAL_RIGID_IDENTITY_SAMPLES)
            {
                stats.samples[stats.sampleCount++] = sample;
                return;
            }
            if (sample.flags == 0)
            {
                return;
            }
            for (int sampleIndex = stats.sampleCount - 1;
                sampleIndex >= 0;
                --sampleIndex)
            {
                if (stats.samples[sampleIndex].flags == 0)
                {
                    stats.samples[sampleIndex] = sample;
                    return;
                }
            }
        };

    for (const RtPathTraceRigidRouteInstanceObservation& instance :
        instances)
    {
        RtPathTraceCanonicalRigidIdentitySample sample;
        sample.valid = true;
        sample.instanceId = instance.instanceId;
        sample.legacyMeshHash = instance.meshHash;
        sample.modelSurfaceIndex = instance.modelSurfaceIndex;
        sample.materialName = instance.materialName;
        sample.modelName = instance.modelName;

        PtCanonicalInstanceKey canonicalInstance;
        canonicalInstance.worldGeneration =
            instance.renderDefKey.worldGeneration;
        canonicalInstance.renderDefIndex =
            instance.renderDefKey.index >= 0
                ? static_cast<uint32_t>(instance.renderDefKey.index)
                : UINT32_MAX;
        canonicalInstance.renderDefGeneration =
            instance.renderDefKey.generation;
        canonicalInstance.subInstanceKind =
            PtCanonicalSubInstanceKind::RigidSurface;
        canonicalInstance.modelSurfaceIndex =
            instance.modelSurfaceIndex >= 0
                ? static_cast<uint32_t>(instance.modelSurfaceIndex)
                : UINT32_MAX;
        canonicalInstance.jointSubmeshIndex = -1;
        sample.renderDefIndex = canonicalInstance.renderDefIndex;
        sample.renderDefGeneration =
            canonicalInstance.renderDefGeneration;
        sample.canonicalInstanceHash =
            PtHashCanonicalInstanceKey(canonicalInstance);
        if (!PtCanonicalInstanceKeyIsValid(canonicalInstance))
        {
            ++stats.invalidInstanceKeys;
            sample.flags |=
                RT_PT_CANONICAL_RIGID_IDENTITY_INVALID_INSTANCE_KEY;
            appendSample(sample);
            continue;
        }
        ++stats.validInstanceKeys;

        const PtGeometryIdentityBinding* binding =
            m_canonicalIdentityRegistry.Find(canonicalInstance);
        if (binding == nullptr)
        {
            ++stats.missingBindings;
            sample.flags |=
                RT_PT_CANONICAL_RIGID_IDENTITY_MISSING_BINDING;
            appendSample(sample);
            continue;
        }
        ++stats.identityBindings;
        sample.canonicalMeshHash = binding->meshHash;
        if (binding->meshKey.sourceDomain !=
                PtCanonicalMeshSourceDomain::RegisteredRenderModel ||
            binding->meshKey.deformationClass !=
                PtCanonicalDeformationClass::Rigid ||
            binding->meshKey.modelSurfaceIndex !=
                canonicalInstance.modelSurfaceIndex)
        {
            ++stats.invalidBindings;
            sample.flags |=
                RT_PT_CANONICAL_RIGID_IDENTITY_INVALID_BINDING;
            appendSample(sample);
            continue;
        }

        const PtGeometrySourceRecord* source =
            m_canonicalSourceRegistry.Find(binding->meshKey);
        if (source == nullptr)
        {
            ++stats.missingSources;
            sample.flags |=
                RT_PT_CANONICAL_RIGID_IDENTITY_MISSING_SOURCE;
            appendSample(sample);
            continue;
        }
        ++stats.sourceBindings;
        sample.canonicalChecksum = source->sourceChecksum;

        const auto meshIt =
            m_rigidMeshCandidateLookup.find(instance.meshHash);
        if (meshIt == m_rigidMeshCandidateLookup.end() ||
            meshIt->second >= m_rigidMeshCandidateRecords.size() ||
            !m_rigidMeshCandidateRecords[meshIt->second].valid)
        {
            ++stats.missingLegacyMeshes;
            sample.flags |=
                RT_PT_CANONICAL_RIGID_IDENTITY_MISSING_LEGACY;
            appendSample(sample);
            continue;
        }
        ++stats.legacyBindings;
        const RigidMeshCandidateRecord& legacy =
            m_rigidMeshCandidateRecords[meshIt->second];

        LegacyPayloadSignature signature;
        const auto signatureIt =
            legacySignatures.find(instance.meshHash);
        if (signatureIt != legacySignatures.end())
        {
            signature = signatureIt->second;
        }
        else
        {
            PtGeometrySourcePayload payload;
            signature.valid = BuildCanonicalComparePayload(
                legacy,
                payload,
                signature.topologySignature);
            if (signature.valid)
            {
                signature.vertexCount =
                    static_cast<uint32_t>(payload.positions.size());
                signature.indexCount =
                    static_cast<uint32_t>(payload.indexes.size());
                signature.checksum = PtChecksumGeometrySourcePayload(
                    CanonicalComparePayloadView(payload));
            }
            legacySignatures.emplace(instance.meshHash, signature);
        }
        sample.legacyChecksum = signature.checksum;

        const uint32_t expectedSourceMaterial =
            canonicalInstance.modelSurfaceIndex;
        const bool sourceMaterialsValid =
            !source->payload.triangles.empty() &&
            source->payload.triangles.front().sourceMaterialSlot ==
                expectedSourceMaterial &&
            source->payload.triangles.back().sourceMaterialSlot ==
                expectedSourceMaterial;
        if (!signature.valid ||
            legacy.modelSurfaceIndex != instance.modelSurfaceIndex ||
            source->key.vertexCount != signature.vertexCount ||
            source->key.indexCount != signature.indexCount ||
            source->key.topologySignature !=
                signature.topologySignature ||
            source->sourceChecksum != signature.checksum ||
            !sourceMaterialsValid)
        {
            ++stats.payloadMismatches;
            sample.flags |=
                RT_PT_CANONICAL_RIGID_IDENTITY_PAYLOAD_MISMATCH;
            appendSample(sample);
            continue;
        }

        ++stats.exactPayloadBindings;
        appendSample(sample);
    }
    return stats;
}

void RtSmokeGeometryUniverse::DumpCanonicalRigidIdentityStats(
    const RtPathTraceCanonicalRigidIdentityStats& stats) const
{
    common->Printf(
        "PathTracePrimaryPass: GEO06 canonical rigid identity coverage frame=%llu route/validKey/identity/source/legacy/exact=%d/%d/%d/%d/%d/%d failures(invalidKey/missingIdentity/invalidBinding/missingSource/missingLegacy/payload)=%d/%d/%d/%d/%d/%d route=shadow-only\n",
        static_cast<unsigned long long>(stats.frameIndex),
        stats.routeInstances,
        stats.validInstanceKeys,
        stats.identityBindings,
        stats.sourceBindings,
        stats.legacyBindings,
        stats.exactPayloadBindings,
        stats.invalidInstanceKeys,
        stats.missingBindings,
        stats.invalidBindings,
        stats.missingSources,
        stats.missingLegacyMeshes,
        stats.payloadMismatches);
    for (int sampleIndex = 0;
        sampleIndex < stats.sampleCount;
        ++sampleIndex)
    {
        const RtPathTraceCanonicalRigidIdentitySample& sample =
            stats.samples[sampleIndex];
        if (!sample.valid)
        {
            continue;
        }
        common->Printf(
            "PathTracePrimaryPass: GEO06 canonical rigid identity sample %d flags=0x%x instance=%llu legacyMesh=%llu canonical(instance/mesh)=%llu/%llu slot/generation/surface=%u/%u/%d checksum(legacy/canonical)=%llu/%llu material='%s' model='%s'\n",
            sampleIndex,
            sample.flags,
            static_cast<unsigned long long>(sample.instanceId),
            static_cast<unsigned long long>(sample.legacyMeshHash),
            static_cast<unsigned long long>(
                sample.canonicalInstanceHash),
            static_cast<unsigned long long>(
                sample.canonicalMeshHash),
            sample.renderDefIndex,
            sample.renderDefGeneration,
            sample.modelSurfaceIndex,
            static_cast<unsigned long long>(sample.legacyChecksum),
            static_cast<unsigned long long>(sample.canonicalChecksum),
            sample.materialName.c_str(),
            sample.modelName.c_str());
    }
}

RtSmokeGeometryUniverse::CanonicalRigidBlasRecord*
RtSmokeGeometryUniverse::FindCanonicalRigidBlasRecord(
    const PtCanonicalMeshKey& key,
    uint64 meshHash)
{
    const auto range = m_canonicalRigidBlasLookup.equal_range(meshHash);
    for (auto it = range.first; it != range.second; ++it)
    {
        if (it->second < m_canonicalRigidBlasRecords.size() &&
            m_canonicalRigidBlasRecords[it->second].key == key)
        {
            return &m_canonicalRigidBlasRecords[it->second];
        }
    }
    return nullptr;
}

uint32 RtSmokeGeometryUniverse::FindCanonicalRigidBlasRecordIndex(
    const PtCanonicalMeshKey& key,
    uint64 meshHash) const
{
    const auto range = m_canonicalRigidBlasLookup.equal_range(meshHash);
    for (auto it = range.first; it != range.second; ++it)
    {
        if (it->second < m_canonicalRigidBlasRecords.size() &&
            m_canonicalRigidBlasRecords[it->second].key == key &&
            it->second <= std::numeric_limits<uint32>::max())
        {
            return static_cast<uint32>(it->second);
        }
    }
    return std::numeric_limits<uint32>::max();
}

void RtSmokeGeometryUniverse::RetireCanonicalRigidBlas(
    CanonicalRigidBlasRecord& record)
{
    if (record.blas)
    {
        m_retiredRigidGpuResources.blases.push_back(record.blas);
        ++m_retiredRigidGpuResources.canonicalBlasCount;
        ++m_canonicalRigidBlasStats.blasRetired;
    }
    record.blas = nullptr;
    record.blasDesc = nvrhi::rt::AccelStructDesc();
    record.inputSignature = 0;
    record.buildSubmitted = false;
}

void RtSmokeGeometryUniverse::ReleaseCanonicalRigidBlasScaffold()
{
    for (CanonicalRigidBlasRecord& record :
        m_canonicalRigidBlasRecords)
    {
        RetireCanonicalRigidBlas(record);
    }
    m_canonicalRigidBlasRecords.clear();
    m_canonicalRigidBlasLookup.clear();
}

void RtSmokeGeometryUniverse::UpdateCanonicalRigidBlasScaffold(
    nvrhi::IDevice* device,
    nvrhi::ICommandList* commandList,
    const RtPathTraceInstanceUniverse& instanceUniverse,
    bool enabled)
{
    const uint64 intervalCreated = m_canonicalRigidBlasStats.blasCreated;
    const uint64 intervalBuilt = m_canonicalRigidBlasStats.blasBuilt;
    const uint64 intervalReused = m_canonicalRigidBlasStats.blasReused;
    const uint64 intervalRetired = m_canonicalRigidBlasStats.blasRetired;
    const uint64 intervalBuildMicros =
        m_canonicalRigidBlasStats.buildSubmitMicroseconds;
    m_canonicalRigidBlasStats = RtPathTraceCanonicalRigidBlasStats();
    m_canonicalRigidBlasStats.frameIndex = m_currentFrameIndex;
    m_canonicalRigidBlasStats.enabled = enabled ? 1 : 0;
    m_canonicalRigidBlasStats.blasCreated = intervalCreated;
    m_canonicalRigidBlasStats.blasBuilt = intervalBuilt;
    m_canonicalRigidBlasStats.blasReused = intervalReused;
    m_canonicalRigidBlasStats.blasRetired = intervalRetired;
    m_canonicalRigidBlasStats.buildSubmitMicroseconds =
        intervalBuildMicros;

    if (!enabled || device == nullptr || commandList == nullptr)
    {
        if (!enabled && !m_canonicalRigidBlasRecords.empty())
        {
            ReleaseCanonicalRigidBlasScaffold();
        }
        return;
    }

    std::vector<RtPathTraceRigidRouteInstanceObservation> instances;
    BuildRigidRouteInstanceList(instanceUniverse, instances);
    m_canonicalRigidBlasStats.requestedInstances =
        static_cast<int>(instances.size());

    std::vector<PtCanonicalMeshKey> requestedMeshes;
    requestedMeshes.reserve(instances.size());
    for (const RtPathTraceRigidRouteInstanceObservation& instance :
        instances)
    {
        PtCanonicalInstanceKey instanceKey;
        instanceKey.worldGeneration =
            instance.renderDefKey.worldGeneration;
        instanceKey.renderDefIndex =
            instance.renderDefKey.index >= 0
                ? static_cast<uint32_t>(instance.renderDefKey.index)
                : UINT32_MAX;
        instanceKey.renderDefGeneration =
            instance.renderDefKey.generation;
        instanceKey.subInstanceKind =
            PtCanonicalSubInstanceKind::RigidSurface;
        instanceKey.modelSurfaceIndex =
            instance.modelSurfaceIndex >= 0
                ? static_cast<uint32_t>(instance.modelSurfaceIndex)
                : UINT32_MAX;
        instanceKey.jointSubmeshIndex = -1;
        const PtGeometryIdentityBinding* binding =
            PtCanonicalInstanceKeyIsValid(instanceKey)
                ? m_canonicalIdentityRegistry.Find(instanceKey)
                : nullptr;
        if (binding == nullptr)
        {
            ++m_canonicalRigidBlasStats.missingIdentity;
            continue;
        }
        ++m_canonicalRigidBlasStats.resolvedIdentity;
        bool duplicate = false;
        for (const PtCanonicalMeshKey& requested : requestedMeshes)
        {
            if (requested == binding->meshKey)
            {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
        {
            requestedMeshes.push_back(binding->meshKey);
        }
    }
    m_canonicalRigidBlasStats.requestedUniqueMeshes =
        static_cast<int>(requestedMeshes.size());

    std::stable_sort(
        requestedMeshes.begin(),
        requestedMeshes.end(),
        [this](
            const PtCanonicalMeshKey& lhs,
            const PtCanonicalMeshKey& rhs)
        {
            const CanonicalRigidBlasRecord* lhsRecord =
                FindCanonicalRigidBlasRecord(
                    lhs,
                    PtHashCanonicalMeshKey(lhs));
            const CanonicalRigidBlasRecord* rhsRecord =
                FindCanonicalRigidBlasRecord(
                    rhs,
                    PtHashCanonicalMeshKey(rhs));
            const uint64 lhsAge =
                lhsRecord != nullptr &&
                lhsRecord->deferredSinceFrame != 0 &&
                m_currentFrameIndex >=
                    lhsRecord->deferredSinceFrame
                    ? m_currentFrameIndex -
                        lhsRecord->deferredSinceFrame
                    : 0;
            const uint64 rhsAge =
                rhsRecord != nullptr &&
                rhsRecord->deferredSinceFrame != 0 &&
                m_currentFrameIndex >=
                    rhsRecord->deferredSinceFrame
                    ? m_currentFrameIndex -
                        rhsRecord->deferredSinceFrame
                    : 0;
            return lhsAge > rhsAge;
        });

    RtSmokeAsAdmissionBudget admissionBudget;
    admissionBudget.maxOperations = idMath::ClampInt(
        1,
        256,
        r_pathTracingGeometryCanonicalRigidBlasBuildsPerFrame.GetInteger());
    const int resultBudgetKiB = idMath::ClampInt(
        0,
        1048576,
        r_pathTracingGeometryCanonicalRigidBlasResultBudgetKB.GetInteger());
    admissionBudget.maxResultBytes =
        static_cast<uint64>(resultBudgetKiB) * 1024ull;
    admissionBudget.allowOneOversizedResult = true;
    std::vector<RtSmokeAsAdmissionRequest> admissionRequests;
    admissionRequests.reserve(requestedMeshes.size());
    auto recordAdmissionDeferral =
        [this](
            CanonicalRigidBlasRecord& record,
            RtSmokeAsDeferralReason reason,
            uint64 deferredAge)
        {
            ++m_canonicalRigidBlasStats.deferredBuilds;
            if (record.deferredSinceFrame == 0)
            {
                record.deferredSinceFrame =
                    m_currentFrameIndex > 0
                        ? m_currentFrameIndex
                        : 1;
            }
            m_canonicalRigidBlasStats.maxDeferredAge =
                Max(
                    m_canonicalRigidBlasStats.maxDeferredAge,
                    deferredAge);
            switch (reason)
            {
                case RT_SMOKE_AS_DEFER_OPERATION_BUDGET:
                    ++m_canonicalRigidBlasStats.
                        deferredOperationBudget;
                    break;
                case RT_SMOKE_AS_DEFER_RESULT_BYTE_BUDGET:
                    ++m_canonicalRigidBlasStats.
                        deferredResultByteBudget;
                    break;
                case RT_SMOKE_AS_DEFER_RESULT_BYTES_UNKNOWN:
                    ++m_canonicalRigidBlasStats.
                        deferredUnknownResultBytes;
                    break;
                default:
                    break;
            }
        };
    for (const PtCanonicalMeshKey& meshKey : requestedMeshes)
    {
        const PtGeometrySourceRecord* source =
            m_canonicalSourceRegistry.Find(meshKey);
        if (source == nullptr)
        {
            ++m_canonicalRigidBlasStats.missingSource;
            continue;
        }
        ++m_canonicalRigidBlasStats.resolvedSources;

        const PtGeometryGpuPoolRecord* gpu = nullptr;
        const size_t poolRecordCount = std::min(
            m_canonicalSourceRegistry.RecordCount(),
            m_canonicalSourceGpuPools.RecordCount());
        for (size_t sourceIndex = 0;
            sourceIndex < poolRecordCount;
            ++sourceIndex)
        {
            const PtGeometryGpuPoolRecord* candidate =
                m_canonicalSourceGpuPools.RecordAt(sourceIndex);
            if (candidate != nullptr && candidate->key == meshKey)
            {
                gpu = candidate;
                break;
            }
        }
        if (gpu == nullptr)
        {
            ++m_canonicalRigidBlasStats.missingPoolRecord;
            continue;
        }
        ++m_canonicalRigidBlasStats.resolvedPoolRecords;

        const uint64 meshHash = PtHashCanonicalMeshKey(meshKey);
        CanonicalRigidBlasRecord* record =
            FindCanonicalRigidBlasRecord(meshKey, meshHash);
        if (record == nullptr)
        {
            CanonicalRigidBlasRecord added;
            added.key = meshKey;
            added.meshHash = meshHash;
            const size_t recordIndex =
                m_canonicalRigidBlasRecords.size();
            m_canonicalRigidBlasRecords.push_back(added);
            m_canonicalRigidBlasLookup.emplace(
                meshHash,
                recordIndex);
            record = &m_canonicalRigidBlasRecords.back();
        }

        const uint64 inputSignature =
            BuildCanonicalRigidBlasInputSignature(
                *source,
                *gpu,
                m_canonicalSourceGpuPools);
        const bool replacementRequired =
            record->blas &&
            record->inputSignature != inputSignature;
        if (record->blas && record->buildSubmitted &&
            !replacementRequired)
        {
            record->deferredSinceFrame = 0;
            ++m_canonicalRigidBlasStats.readyRequestedMeshes;
            ++m_canonicalRigidBlasStats.blasReused;
            continue;
        }

        const uint64 deferredAge =
            record->deferredSinceFrame != 0 &&
            m_currentFrameIndex >= record->deferredSinceFrame
                ? m_currentFrameIndex -
                    record->deferredSinceFrame
                : 0;
        RtSmokeAsAdmissionRequest admissionRequest;
        admissionRequest.kind =
            replacementRequired
                ? RT_SMOKE_AS_WORK_UPDATE
                : RT_SMOKE_AS_WORK_NEW_BUILD;
        admissionRequest.priority =
            RT_SMOKE_AS_PRIORITY_ACTIVE;
        admissionRequest.deferredAge = deferredAge;
        admissionRequests.push_back(admissionRequest);
        const RtSmokeAsAdmissionPlan operationPlan =
            BuildSmokeAsAdmissionPlan(
                admissionBudget,
                admissionRequests);
        const RtSmokeAsAdmissionDecision& operationDecision =
            operationPlan.decisions.back();
        if (operationDecision.deferralReason ==
            RT_SMOKE_AS_DEFER_OPERATION_BUDGET)
        {
            recordAdmissionDeferral(
                *record,
                operationDecision.deferralReason,
                deferredAge);
            if (record->blas && record->buildSubmitted)
            {
                ++m_canonicalRigidBlasStats.readyRequestedMeshes;
                ++m_canonicalRigidBlasStats.blasReused;
            }
            continue;
        }

        nvrhi::rt::GeometryTriangles triangles;
        triangles.vertexBuffer =
            m_canonicalSourceGpuPools.PositionBuffer();
        triangles.indexBuffer =
            m_canonicalSourceGpuPools.IndexBuffer();
        triangles.vertexFormat = nvrhi::Format::RGB32_FLOAT;
        triangles.indexFormat = nvrhi::Format::R32_UINT;
        triangles.vertexOffset = gpu->positions.offsetBytes;
        triangles.indexOffset = gpu->indexes.offsetBytes;
        triangles.vertexCount = meshKey.vertexCount;
        triangles.indexCount = meshKey.indexCount;
        triangles.vertexStride = sizeof(PtGeometrySourcePosition);
        nvrhi::rt::GeometryDesc geometry;
        geometry.setTriangles(triangles);
        nvrhi::rt::AccelStructDesc replacementBlasDesc =
            nvrhi::rt::AccelStructDesc()
            .addBottomLevelGeometry(geometry)
            .setBuildFlags(
                nvrhi::rt::AccelStructBuildFlags::PreferFastTrace)
            .setDebugName("PathTraceCanonicalRigidBLAS");
        nvrhi::rt::AccelStructHandle replacementBlas =
            device->createAccelStruct(replacementBlasDesc);
        if (!replacementBlas)
        {
            ++m_canonicalRigidBlasStats.deferredBuilds;
            ++m_canonicalRigidBlasStats.deferredAllocationFailure;
            m_canonicalRigidBlasStats.maxDeferredAge =
                Max(
                    m_canonicalRigidBlasStats.maxDeferredAge,
                    deferredAge);
            if (record->deferredSinceFrame == 0)
            {
                record->deferredSinceFrame =
                    m_currentFrameIndex > 0
                        ? m_currentFrameIndex
                        : 1;
            }
            admissionRequests.pop_back();
            if (record->blas && record->buildSubmitted)
            {
                ++m_canonicalRigidBlasStats.readyRequestedMeshes;
                ++m_canonicalRigidBlasStats.blasReused;
            }
            continue;
        }

        if (admissionBudget.maxResultBytes > 0)
        {
            const nvrhi::MemoryRequirements requirements =
                device->getAccelStructMemoryRequirements(
                    replacementBlas);
            ++m_canonicalRigidBlasStats.resultRequirementQueries;
            admissionRequests.back().resultBytes =
                requirements.size;
            admissionRequests.back().resultBytesKnown =
                requirements.size > 0;
            if (requirements.size == 0)
            {
                ++m_canonicalRigidBlasStats.
                    resultRequirementFailures;
            }
        }
        const RtSmokeAsAdmissionPlan admissionPlan =
            BuildSmokeAsAdmissionPlan(
                admissionBudget,
                admissionRequests);
        const RtSmokeAsAdmissionDecision& admissionDecision =
            admissionPlan.decisions.back();
        if (!admissionDecision.admitted)
        {
            recordAdmissionDeferral(
                *record,
                admissionDecision.deferralReason,
                deferredAge);
            if (record->blas && record->buildSubmitted)
            {
                ++m_canonicalRigidBlasStats.readyRequestedMeshes;
                ++m_canonicalRigidBlasStats.blasReused;
            }
            continue;
        }

        ++m_canonicalRigidBlasStats.blasCreated;
        if (admissionRequests.back().resultBytesKnown)
        {
            m_canonicalRigidBlasStats.admittedResultBytes +=
                admissionRequests.back().resultBytes;
        }
        if (admissionDecision.oversizedResultAdmission)
        {
            ++m_canonicalRigidBlasStats.
                oversizedResultAdmissions;
            m_canonicalRigidBlasStats.oversizedResultBytes +=
                admissionRequests.back().resultBytes;
        }
        commandList->setBufferState(
            m_canonicalSourceGpuPools.PositionBuffer(),
            nvrhi::ResourceStates::AccelStructBuildInput);
        commandList->setBufferState(
            m_canonicalSourceGpuPools.IndexBuffer(),
            nvrhi::ResourceStates::AccelStructBuildInput);
        commandList->commitBarriers();
        const auto buildStart = std::chrono::steady_clock::now();
        nvrhi::utils::BuildBottomLevelAccelStruct(
            commandList,
            replacementBlas,
            replacementBlasDesc);
        const auto buildEnd = std::chrono::steady_clock::now();
        m_canonicalRigidBlasStats.buildSubmitMicroseconds +=
            static_cast<uint64>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    buildEnd - buildStart).count());
        if (record->blas)
        {
            RetireCanonicalRigidBlas(*record);
        }
        record->blasDesc = replacementBlasDesc;
        record->blas = replacementBlas;
        record->inputSignature = inputSignature;
        record->buildSubmitted = true;
        record->deferredSinceFrame = 0;
        ++m_canonicalRigidBlasStats.blasBuilt;
        ++m_canonicalRigidBlasStats.readyRequestedMeshes;
    }

    for (const CanonicalRigidBlasRecord& record :
        m_canonicalRigidBlasRecords)
    {
        if (record.blas && record.buildSubmitted)
        {
            ++m_canonicalRigidBlasStats.activeBlas;
        }
    }
}

void RtSmokeGeometryUniverse::DumpCanonicalRigidBlasStats()
{
    common->Printf(
        "PathTracePrimaryPass: GEO06 canonical rigid BLAS frame=%llu enabled=%d requested(instances/unique)=%d/%d resolved(identity/source/pool)=%d/%d/%d ready/active/deferred=%d/%d/%d admission(deferOp/deferBytes/deferUnknown/deferAlloc/query/fail/bytes/oversized/oversizedBytes/maxAge)=%d/%d/%d/%d/%d/%d/%llu/%d/%llu/%llu missing(identity/source/pool)=%d/%d/%d interval(create/build/reuse/retire/buildUs)=%llu/%llu/%llu/%llu/%llu traversal=legacy route=shadow-only\n",
        static_cast<unsigned long long>(
            m_canonicalRigidBlasStats.frameIndex),
        m_canonicalRigidBlasStats.enabled,
        m_canonicalRigidBlasStats.requestedInstances,
        m_canonicalRigidBlasStats.requestedUniqueMeshes,
        m_canonicalRigidBlasStats.resolvedIdentity,
        m_canonicalRigidBlasStats.resolvedSources,
        m_canonicalRigidBlasStats.resolvedPoolRecords,
        m_canonicalRigidBlasStats.readyRequestedMeshes,
        m_canonicalRigidBlasStats.activeBlas,
        m_canonicalRigidBlasStats.deferredBuilds,
        m_canonicalRigidBlasStats.deferredOperationBudget,
        m_canonicalRigidBlasStats.deferredResultByteBudget,
        m_canonicalRigidBlasStats.deferredUnknownResultBytes,
        m_canonicalRigidBlasStats.deferredAllocationFailure,
        m_canonicalRigidBlasStats.resultRequirementQueries,
        m_canonicalRigidBlasStats.resultRequirementFailures,
        static_cast<unsigned long long>(
            m_canonicalRigidBlasStats.admittedResultBytes),
        m_canonicalRigidBlasStats.oversizedResultAdmissions,
        static_cast<unsigned long long>(
            m_canonicalRigidBlasStats.oversizedResultBytes),
        static_cast<unsigned long long>(
            m_canonicalRigidBlasStats.maxDeferredAge),
        m_canonicalRigidBlasStats.missingIdentity,
        m_canonicalRigidBlasStats.missingSource,
        m_canonicalRigidBlasStats.missingPoolRecord,
        static_cast<unsigned long long>(
            m_canonicalRigidBlasStats.blasCreated),
        static_cast<unsigned long long>(
            m_canonicalRigidBlasStats.blasBuilt),
        static_cast<unsigned long long>(
            m_canonicalRigidBlasStats.blasReused),
        static_cast<unsigned long long>(
            m_canonicalRigidBlasStats.blasRetired),
        static_cast<unsigned long long>(
            m_canonicalRigidBlasStats.buildSubmitMicroseconds));
    m_canonicalRigidBlasStats.blasCreated = 0;
    m_canonicalRigidBlasStats.blasBuilt = 0;
    m_canonicalRigidBlasStats.blasReused = 0;
    m_canonicalRigidBlasStats.blasRetired = 0;
    m_canonicalRigidBlasStats.buildSubmitMicroseconds = 0;
}

void RtSmokeGeometryUniverse::RetireRigidBlas(RigidMeshCandidateRecord& record)
{
    if (record.rigidBlas)
    {
        m_retiredRigidGpuResources.blases.push_back(
            record.rigidBlas);
        ++m_retiredRigidGpuResources.legacyBlasCount;
    }

    record.rigidBlas = nullptr;
    record.rigidBlasDesc = nvrhi::rt::AccelStructDesc();
    record.gpuBlasCreated = false;
    record.gpuBlasBuildSubmitted = false;
    record.gpuBlasVertexCount = 0;
    record.gpuBlasIndexCount = 0;
}

void RtSmokeGeometryUniverse::RetireRigidBuffer(
    nvrhi::BufferHandle& buffer)
{
    if (buffer)
    {
        m_retiredRigidGpuResources.buffers.push_back(buffer);
        ++m_retiredRigidGpuResources.legacyBufferCount;
        buffer = nullptr;
    }
}

void RtSmokeGeometryUniverse::RetireRigidMeshGpuResources(
    RigidMeshCandidateRecord& record)
{
    RetireRigidBlas(record);
    RetireRigidBuffer(record.rigidVertexBuffer);
    RetireRigidBuffer(record.rigidIndexBuffer);
    record.gpuUploadSignature = 0;
    record.gpuBuffersUploaded = false;
}

void RtSmokeGeometryUniverse::ClearRigidResidencyCaches()
{
    for (RigidMeshCandidateRecord& record : m_rigidMeshCandidateRecords)
    {
        RetireRigidMeshGpuResources(record);
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

void RtSmokeGeometryUniverse::BeginFrame(
    uint64 frameIndex,
    const idRenderWorldLocal* renderWorld,
    bool capturePreviousStaticSnapshot)
{
    PtGeometryLifecycle::MaybeDumpLifecycleStats(frameIndex, renderWorld);
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
    if (capturePreviousStaticSnapshot)
    {
        m_previousStaticVertexCache = m_staticVertexCache;
        m_previousStaticIndexCache = m_staticIndexCache;
        m_previousStaticTriangleClassCache =
            m_staticTriangleClassCache;
        m_previousStaticTriangleMaterialCache =
            m_staticTriangleMaterialCache;
        m_previousStaticSnapshotGeneration =
            m_staticGeometryGeneration;
        m_previousStaticSnapshotMaterialGeneration =
            m_staticMaterialGeneration;
    }
    else
    {
        // The portal-bucket universe is immutable resident source storage.
        // It never feeds the monolithic previous-static history contract, so
        // retaining and copying a second full-map CPU snapshot every frame is
        // pure bandwidth and memory overhead.
        m_previousStaticVertexCache.clear();
        m_previousStaticIndexCache.clear();
        m_previousStaticTriangleClassCache.clear();
        m_previousStaticTriangleMaterialCache.clear();
        m_previousStaticSnapshotGeneration =
            m_staticGeometryGeneration;
        m_previousStaticSnapshotMaterialGeneration =
            m_staticMaterialGeneration;
    }
    m_currentFrameIndex = frameIndex;
    m_staticMaterialDirtyTriangleOffset = -1;
    m_staticMaterialDirtyTriangleCount = 0;
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

bool RtSmokeGeometryUniverse::RefreshStaticSurfacePortalArea(
    uint64 key,
    int portalArea)
{
    RtSmokePersistentStaticSurfaceRecord* record =
        FindStaticSurfaceMutable(key);
    if (!record || !record->valid ||
        record->portalArea == portalArea)
    {
        return false;
    }

    record->portalArea = portalArea;
    ++m_generation;
    return true;
}

bool RtSmokeGeometryUniverse::RefreshStaticSurfaceBucketKey(
    uint64 key,
    uint64 bucketSurfaceKey)
{
    RtSmokePersistentStaticSurfaceRecord* record =
        FindStaticSurfaceMutable(key);
    if (!record || !record->valid ||
        bucketSurfaceKey == 0 ||
        record->bucketSurfaceKey == bucketSurfaceKey)
    {
        return false;
    }

    record->bucketSurfaceKey = bucketSurfaceKey;
    ++m_generation;
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

RtSmokeStaticSurfaceAppend RtSmokeGeometryUniverse::BeginStaticSurfaceAppend(
    uint64 key,
    uint32_t surfaceClassId,
    uint32_t materialId,
    int vertexCount,
    int indexCount,
    int portalArea,
    uint64 bucketSurfaceKey) const
{
    RtSmokeStaticSurfaceAppend append;
    append.key = key;
    append.bucketSurfaceKey =
        bucketSurfaceKey != 0
            ? bucketSurfaceKey
            : key;
    append.surfaceClassId = surfaceClassId;
    append.materialId = materialId;
    append.portalArea = portalArea;
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
    record.bucketSurfaceKey = append.bucketSurfaceKey;
    record.surfaceClassId = append.surfaceClassId;
    record.materialId = append.materialId;
    record.portalArea = append.portalArea;
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

RtSmokeStaticBucketAssignmentPlan
RtSmokeGeometryUniverse::BuildStaticBucketAssignmentPlan(
    uint64 worldGeneration,
    uint64 sourceGeneration,
    int portalAreaCount,
    int maxVerticesPerBucket,
    int maxIndexesPerBucket,
    int maxTrianglesPerBucket,
    const std::vector<bool>* activePortalAreas,
    bool* cacheHit)
{
    if (cacheHit)
    {
        *cacheHit = false;
    }

    const bool canReuseTopology =
        m_staticBucketAssignmentPlanCacheValid &&
        m_staticBucketAssignmentWorldGeneration ==
            worldGeneration &&
        m_staticBucketAssignmentSourceGeneration ==
            sourceGeneration &&
        m_staticBucketAssignmentStorageGeneration ==
            m_staticGeometryGeneration &&
        m_staticBucketAssignmentPortalAreaCount ==
            portalAreaCount &&
        m_staticBucketAssignmentMaxVertices ==
            maxVerticesPerBucket &&
        m_staticBucketAssignmentMaxIndexes ==
            maxIndexesPerBucket &&
        m_staticBucketAssignmentMaxTriangles ==
            maxTrianglesPerBucket;
    if (canReuseTopology)
    {
        RtSmokeStaticBucketAssignmentPlan plan =
            m_staticBucketAssignmentPlanCache;
        plan.stats.activeBuckets = 0;
        for (RtSmokeStaticBucketAssignmentBucket& bucket :
            plan.buckets)
        {
            bool active = false;
            if (activePortalAreas)
            {
                active =
                    bucket.portalArea ==
                        RT_SMOKE_STATIC_BUCKET_FALLBACK_AREA ||
                    (bucket.portalArea >= 0 &&
                        bucket.portalArea <
                            static_cast<int>(
                                activePortalAreas->size()) &&
                        (*activePortalAreas)[
                            bucket.portalArea]);
            }
            else
            {
                const size_t firstAssignment =
                    static_cast<size_t>(
                        bucket.firstAssignment);
                const size_t endAssignment =
                    firstAssignment +
                    static_cast<size_t>(
                        bucket.assignmentCount);
                for (size_t assignmentIndex =
                         firstAssignment;
                     assignmentIndex < endAssignment &&
                         assignmentIndex <
                             plan.assignments.size();
                     ++assignmentIndex)
                {
                    const uint32_t sourceRecordIndex =
                        plan.assignments[assignmentIndex].
                            sourceRecordIndex;
                    if (sourceRecordIndex <
                            m_staticSurfaceRecords.size() &&
                        m_staticSurfaceRecords[
                            sourceRecordIndex].
                            seenThisFrame)
                    {
                        active = true;
                        break;
                    }
                }
            }
            bucket.active = active;
            if (active)
            {
                ++plan.stats.activeBuckets;
            }
        }
        if (cacheHit)
        {
            *cacheHit = true;
        }
        return plan;
    }

    std::vector<RtSmokeStaticBucketAssignmentSurface> surfaces;
    surfaces.reserve(m_staticSurfaceRecords.size());

    const int vertexCount = static_cast<int>(m_staticVertexCache.size());
    const int indexCount = static_cast<int>(m_staticIndexCache.size());
    const int triangleCount =
        static_cast<int>(m_staticTriangleClassCache.size());
    const int materialTriangleCount =
        static_cast<int>(m_staticTriangleMaterialCache.size());
    for (size_t recordIndex = 0;
        recordIndex < m_staticSurfaceRecords.size();
        ++recordIndex)
    {
        const RtSmokePersistentStaticSurfaceRecord& record =
            m_staticSurfaceRecords[recordIndex];
        RtSmokeStaticBucketAssignmentSurface surface;
        surface.surfaceKey =
            record.bucketSurfaceKey != 0
                ? record.bucketSurfaceKey
                : record.key;
        surface.sourceRecordIndex =
            static_cast<uint32_t>(recordIndex);
        surface.portalArea = record.portalArea;
        surface.range.vertexOffset =
            record.currentRange.vertices.offset;
        surface.range.vertexCount =
            record.currentRange.vertices.count;
        surface.range.indexOffset =
            record.currentRange.indexes.offset;
        surface.range.indexCount =
            record.currentRange.indexes.count;
        surface.range.triangleOffset =
            record.currentRange.triangles.offset;
        surface.range.triangleCount =
            record.currentRange.triangles.count;
        surface.valid =
            record.valid &&
            IsSmokeGeometryRangeValid(
                record.currentRange,
                vertexCount,
                indexCount,
                triangleCount,
                materialTriangleCount);
        surface.active =
            activePortalAreas
                ? (record.portalArea ==
                        RT_SMOKE_STATIC_BUCKET_FALLBACK_AREA ||
                    (record.portalArea >= 0 &&
                        record.portalArea <
                            static_cast<int>(
                                activePortalAreas->size()) &&
                        (*activePortalAreas)[record.portalArea]))
                : record.seenThisFrame;
        surfaces.push_back(surface);
    }

    RtSmokeStaticBucketAssignmentPlanDesc desc;
    desc.surfaces = surfaces.empty() ? nullptr : surfaces.data();
    desc.surfaceCount = static_cast<int>(surfaces.size());
    desc.worldGeneration = worldGeneration;
    desc.sourceGeneration = sourceGeneration;
    desc.storageGeneration = m_staticGeometryGeneration;
    desc.portalAreaCount = portalAreaCount;
    desc.maxVerticesPerBucket = maxVerticesPerBucket;
    desc.maxIndexesPerBucket = maxIndexesPerBucket;
    desc.maxTrianglesPerBucket = maxTrianglesPerBucket;
    RtSmokeStaticBucketAssignmentPlan plan =
        ::BuildSmokeStaticBucketAssignmentPlan(desc);
    m_staticBucketAssignmentPlanCache = plan;
    m_staticBucketAssignmentWorldGeneration =
        worldGeneration;
    m_staticBucketAssignmentSourceGeneration =
        sourceGeneration;
    m_staticBucketAssignmentStorageGeneration =
        m_staticGeometryGeneration;
    m_staticBucketAssignmentPortalAreaCount =
        portalAreaCount;
    m_staticBucketAssignmentMaxVertices =
        maxVerticesPerBucket;
    m_staticBucketAssignmentMaxIndexes =
        maxIndexesPerBucket;
    m_staticBucketAssignmentMaxTriangles =
        maxTrianglesPerBucket;
    m_staticBucketAssignmentPlanCacheValid =
        plan.exactCoverage;
    return plan;
}

RtSmokeStaticBucketGeometryPack
RtSmokeGeometryUniverse::BuildStaticBucketGeometryPack(
    const RtSmokeStaticBucketAssignmentPlan& assignmentPlan) const
{
    RtSmokeStaticBucketGeometryPackDesc desc;
    desc.assignmentPlan = &assignmentPlan;
    desc.vertices =
        m_staticVertexCache.empty()
            ? nullptr
            : m_staticVertexCache.data();
    desc.vertexStride = sizeof(PathTraceSmokeVertex);
    desc.totalVertexCount =
        static_cast<int>(m_staticVertexCache.size());
    desc.indexes =
        m_staticIndexCache.empty()
            ? nullptr
            : m_staticIndexCache.data();
    desc.totalIndexCount =
        static_cast<int>(m_staticIndexCache.size());
    desc.triangleClasses =
        m_staticTriangleClassCache.empty()
            ? nullptr
            : m_staticTriangleClassCache.data();
    desc.triangleMaterials =
        m_staticTriangleMaterialCache.empty()
            ? nullptr
            : m_staticTriangleMaterialCache.data();
    desc.totalTriangleCount = static_cast<int>(
        Min(
            m_staticTriangleClassCache.size(),
            m_staticTriangleMaterialCache.size()));
    return ::BuildSmokeStaticBucketGeometryPack(desc);
}

const RtSmokeStaticBucketGeometryPack&
RtSmokeGeometryUniverse::GetOrBuildStaticBucketResidentGeometryPack(
    const RtSmokeStaticBucketAssignmentPlan& assignmentPlan,
    bool& cacheHit)
{
    RtSmokeStaticBucketResidentPackCacheInput cacheInput;
    cacheInput.assignmentPlanSignature =
        assignmentPlan.planSignature;
    cacheInput.geometryGeneration =
        m_staticGeometryGeneration;
    cacheInput.materialGeneration =
        m_staticMaterialGeneration;
    cacheInput.cachedAssignmentPlanSignature =
        m_staticBucketResidentAssignmentPlanSignature;
    cacheInput.cachedGeometryGeneration =
        m_staticBucketResidentGeometryGeneration;
    cacheInput.cachedMaterialGeneration =
        m_staticBucketResidentMaterialGeneration;
    cacheInput.assignmentExact =
        assignmentPlan.exactCoverage;
    cacheInput.cachedPackExact =
        m_staticBucketResidentGeometryPack.exact;
    cacheInput.cacheValid =
        m_staticBucketResidentGeometryPackValid;
    const RtSmokeStaticBucketResidentPackCachePlan cachePlan =
        BuildSmokeStaticBucketResidentPackCachePlan(cacheInput);

    cacheHit = cachePlan.reuse;
    if (!cachePlan.reuse)
    {
        m_staticBucketResidentGeometryPack =
            BuildStaticBucketGeometryPack(assignmentPlan);
        m_staticBucketResidentAssignmentPlanSignature =
            assignmentPlan.planSignature;
        m_staticBucketResidentGeometryGeneration =
            m_staticGeometryGeneration;
        m_staticBucketResidentMaterialGeneration =
            m_staticMaterialGeneration;
        m_staticBucketResidentGeometryPackValid =
            m_staticBucketResidentGeometryPack.exact;
        return m_staticBucketResidentGeometryPack;
    }

    if (m_staticBucketResidentGeometryPack.buckets.size() !=
        assignmentPlan.buckets.size())
    {
        m_staticBucketResidentGeometryPackValid = false;
        cacheHit = false;
        return GetOrBuildStaticBucketResidentGeometryPack(
            assignmentPlan,
            cacheHit);
    }
    for (size_t bucketIndex = 0;
        bucketIndex < assignmentPlan.buckets.size();
        ++bucketIndex)
    {
        RtSmokeStaticBucketPackedRecord& cachedBucket =
            m_staticBucketResidentGeometryPack.buckets[
                bucketIndex];
        const RtSmokeStaticBucketAssignmentBucket& sourceBucket =
            assignmentPlan.buckets[bucketIndex];
        if (cachedBucket.bucketKey != sourceBucket.bucketKey)
        {
            m_staticBucketResidentGeometryPackValid = false;
            cacheHit = false;
            return GetOrBuildStaticBucketResidentGeometryPack(
                assignmentPlan,
                cacheHit);
        }
        cachedBucket.active = sourceBucket.active;
    }
    return m_staticBucketResidentGeometryPack;
}

bool RtSmokeGeometryUniverse::GetOrBuildStaticBucketMaterialIndexes(
    const RtSmokeStaticBucketGeometryPack& geometryPack,
    const std::vector<uint32_t>& materialTableIds,
    bool& cacheHit,
    uint64& materialBindingSignature,
    const std::vector<uint32_t>*& triangleMaterialIndexes,
    const std::vector<int>*& missingMaterialIndexesByBucket)
{
    cacheHit = false;
    materialBindingSignature = 0;
    triangleMaterialIndexes = nullptr;
    missingMaterialIndexesByBucket = nullptr;

    if (!geometryPack.exact ||
        geometryPack.contentSignature == 0)
    {
        return false;
    }

    if (m_staticBucketReferencedMaterialIdsResidentPackSignature !=
            geometryPack.contentSignature)
    {
        m_staticBucketReferencedMaterialIds.clear();
        std::unordered_set<uint32_t> referencedMaterialIds;
        referencedMaterialIds.reserve(
            geometryPack.triangleMaterials.size());
        for (uint32_t materialId :
             geometryPack.triangleMaterials)
        {
            if (referencedMaterialIds.insert(materialId).second)
            {
                m_staticBucketReferencedMaterialIds.push_back(
                    materialId);
            }
        }
        m_staticBucketReferencedMaterialIdsResidentPackSignature =
            geometryPack.contentSignature;
    }

    std::unordered_map<uint32_t, uint32_t> materialIndexById;
    materialIndexById.reserve(materialTableIds.size());
    for (uint32_t materialIndex = 0;
         materialIndex < materialTableIds.size();
         ++materialIndex)
    {
        materialIndexById.emplace(
            materialTableIds[materialIndex],
            materialIndex);
    }

    materialBindingSignature = 14695981039346656037ull;
    const size_t referencedMaterialCount =
        m_staticBucketReferencedMaterialIds.size();
    materialBindingSignature = HashSmokeBytes(
        materialBindingSignature,
        &referencedMaterialCount,
        sizeof(referencedMaterialCount));
    for (uint32_t materialId :
         m_staticBucketReferencedMaterialIds)
    {
        const auto materialIndex =
            materialIndexById.find(materialId);
        const uint32_t resolvedMaterialIndex =
            materialIndex != materialIndexById.end()
                ? materialIndex->second
                : UINT32_MAX;
        materialBindingSignature = HashSmokeBytes(
            materialBindingSignature,
            &materialId,
            sizeof(materialId));
        materialBindingSignature = HashSmokeBytes(
            materialBindingSignature,
            &resolvedMaterialIndex,
            sizeof(resolvedMaterialIndex));
    }

    RtSmokeStaticBucketMaterialIndexCacheInput cacheInput;
    cacheInput.residentPackSignature =
        geometryPack.contentSignature;
    cacheInput.materialBindingSignature =
        materialBindingSignature;
    cacheInput.triangleCount = static_cast<int>(
        geometryPack.triangleMaterials.size());
    cacheInput.bucketCount = static_cast<int>(
        geometryPack.buckets.size());
    cacheInput.cachedResidentPackSignature =
        m_staticBucketMaterialIndexResidentPackSignature;
    cacheInput.cachedMaterialBindingSignature =
        m_staticBucketMaterialIndexBindingSignature;
    cacheInput.cachedTriangleCount = static_cast<int>(
        m_staticBucketMaterialIndexes.size());
    cacheInput.cachedBucketCount = static_cast<int>(
        m_staticBucketMissingMaterialIndexesByBucket.size());
    cacheInput.residentPackExact = geometryPack.exact;
    cacheInput.cacheValid =
        m_staticBucketMaterialIndexCacheValid;
    const RtSmokeStaticBucketMaterialIndexCachePlan cachePlan =
        BuildSmokeStaticBucketMaterialIndexCachePlan(cacheInput);

    cacheHit = cachePlan.reuse;
    if (!cachePlan.reuse)
    {
        m_staticBucketMaterialIndexes.assign(
            geometryPack.triangleMaterials.size(),
            UINT32_MAX);
        m_staticBucketMissingMaterialIndexesByBucket.assign(
            geometryPack.buckets.size(),
            0);

        for (size_t triangleIndex = 0;
             triangleIndex <
                geometryPack.triangleMaterials.size();
             ++triangleIndex)
        {
            const auto materialIndex = materialIndexById.find(
                geometryPack.triangleMaterials[triangleIndex]);
            if (materialIndex != materialIndexById.end())
            {
                m_staticBucketMaterialIndexes[triangleIndex] =
                    materialIndex->second;
            }
        }

        bool rangesValid = geometryPack.exact;
        for (size_t bucketIndex = 0;
             bucketIndex < geometryPack.buckets.size();
             ++bucketIndex)
        {
            const RtSmokeStaticBucketPackedRecord& bucket =
                geometryPack.buckets[bucketIndex];
            if (bucket.range.triangleOffset < 0 ||
                bucket.range.triangleCount <= 0)
            {
                rangesValid = false;
                break;
            }
            const size_t firstTriangle =
                static_cast<size_t>(
                    bucket.range.triangleOffset);
            const size_t endTriangle =
                firstTriangle +
                static_cast<size_t>(
                    bucket.range.triangleCount);
            if (endTriangle >
                m_staticBucketMaterialIndexes.size())
            {
                rangesValid = false;
                break;
            }
            m_staticBucketMissingMaterialIndexesByBucket[
                bucketIndex] = static_cast<int>(std::count(
                    m_staticBucketMaterialIndexes.begin() +
                        firstTriangle,
                    m_staticBucketMaterialIndexes.begin() +
                        endTriangle,
                    UINT32_MAX));
        }

        m_staticBucketMaterialIndexResidentPackSignature =
            geometryPack.contentSignature;
        m_staticBucketMaterialIndexBindingSignature =
            materialBindingSignature;
        m_staticBucketMaterialIndexCacheValid =
            rangesValid &&
            materialBindingSignature != 0;
        if (!m_staticBucketMaterialIndexCacheValid)
        {
            return false;
        }
    }

    triangleMaterialIndexes =
        &m_staticBucketMaterialIndexes;
    missingMaterialIndexesByBucket =
        &m_staticBucketMissingMaterialIndexesByBucket;
    return true;
}

RtPathTraceStaticBucketBlasGpuStats
RtSmokeGeometryUniverse::UpdateStaticBucketBlasGpuScaffold(
    nvrhi::IDevice* device,
    nvrhi::ICommandList* commandList,
    const RtSmokeStaticBucketGeometryPack& geometryPack,
    bool enabled,
    bool submitBuilds,
    int maxBuildsPerFrame,
    bool forceRebuild,
    bool collectResultMemory)
{
    RtPathTraceStaticBucketBlasGpuStats stats;
    stats.frameIndex = m_currentFrameIndex;
    stats.contentSignature = geometryPack.contentSignature;
    stats.uploadSignature = m_staticBucketUploadSignature;
    stats.enabled = enabled ? 1 : 0;
    stats.submitBuilds = submitBuilds ? 1 : 0;
    stats.residentBuckets =
        static_cast<int>(geometryPack.buckets.size());
    stats.vertexCount = geometryPack.stats.packedVertices;
    stats.indexCount = geometryPack.stats.packedIndexes;
    stats.triangleCount = geometryPack.stats.packedTriangles;
    stats.surfaceRecordCount =
        static_cast<int>(geometryPack.surfaceRecords.size());
    stats.staticClassMetadataWordCount =
        static_cast<int>(
            geometryPack.staticClassMetadataWords.size());
    stats.surfaceRecordWordOffset =
        static_cast<int>(geometryPack.surfaceRecordWordOffset);
    stats.vertexBytes = geometryPack.vertexBytes.size();
    stats.indexBytes =
        geometryPack.indexes.size() * sizeof(uint32_t);
    stats.surfaceRecordBytes =
        geometryPack.surfaceRecords.size() *
        sizeof(RtSmokeStaticBucketSurfaceRecord);
    stats.metadataBytes =
        geometryPack.triangleClasses.size() *
            sizeof(uint32_t) +
        geometryPack.triangleMaterials.size() *
            sizeof(uint32_t);
    for (const RtSmokeStaticBucketPackedRecord& bucket :
        geometryPack.buckets)
    {
        if (bucket.active)
        {
            ++stats.activeBuckets;
        }
    }

    if (!enabled)
    {
        ReleaseStaticBucketBlasGpuScaffold();
        return stats;
    }
    if (!geometryPack.exact ||
        geometryPack.vertexBytes.empty() ||
        geometryPack.indexes.empty() ||
        geometryPack.triangleClasses.empty() ||
        geometryPack.triangleClasses.size() !=
            geometryPack.triangleMaterials.size() ||
        geometryPack.triangleClasses.size() !=
            geometryPack.triangleIdentities.size())
    {
        ++stats.skippedInexactPack;
        return stats;
    }
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

    const size_t vertexBytes = geometryPack.vertexBytes.size();
    const size_t indexBytes =
        geometryPack.indexes.size() * sizeof(uint32_t);
    const size_t classBytes =
        geometryPack.triangleClasses.size() *
        sizeof(uint32_t);
    const size_t materialBytes =
        geometryPack.triangleMaterials.size() * sizeof(uint32_t);
    bool buffersCreated = false;
    auto ensureBuffer =
        [&](nvrhi::BufferHandle& buffer,
            const char* debugName,
            size_t byteSize,
            uint32_t structStride,
            bool vertexBuffer,
            bool indexBuffer,
            bool accelStructBuildInput) -> bool
    {
        if (RigidSmokeBufferHasCapacity(
                buffer,
                byteSize,
                structStride))
        {
            return true;
        }
        RetireStaticBucketBuffer(buffer);
        buffer = CreateRigidSmokeBuffer(
            device,
            debugName,
            byteSize,
            structStride,
            vertexBuffer,
            indexBuffer,
            accelStructBuildInput);
        if (buffer)
        {
            buffersCreated = true;
            ++stats.buffersCreated;
        }
        return buffer != nullptr;
    };

    const bool buffersValid =
        ensureBuffer(
            m_staticBucketVertexBuffer,
            "PathTraceStaticBucketVertices",
            vertexBytes,
            sizeof(PathTraceSmokeVertex),
            true,
            false,
            true) &&
        ensureBuffer(
            m_staticBucketIndexBuffer,
            "PathTraceStaticBucketIndexes",
            indexBytes,
            sizeof(uint32_t),
            false,
            true,
            true) &&
        ensureBuffer(
            m_staticBucketTriangleClassBuffer,
            "PathTraceStaticBucketTriangleClasses",
            classBytes,
            sizeof(uint32_t),
            false,
            false,
            false) &&
        ensureBuffer(
            m_staticBucketTriangleMaterialBuffer,
            "PathTraceStaticBucketTriangleMaterials",
            materialBytes,
            sizeof(uint32_t),
            false,
            false,
            false);
    if (!buffersValid)
    {
        ++stats.invalidBuckets;
        return stats;
    }

    const bool uploadRequired =
        buffersCreated ||
        m_staticBucketUploadSignature !=
            geometryPack.contentSignature;
    if (uploadRequired)
    {
        commandList->beginTrackingBufferState(
            m_staticBucketVertexBuffer,
            nvrhi::ResourceStates::Common);
        commandList->beginTrackingBufferState(
            m_staticBucketIndexBuffer,
            nvrhi::ResourceStates::Common);
        commandList->beginTrackingBufferState(
            m_staticBucketTriangleClassBuffer,
            nvrhi::ResourceStates::Common);
        commandList->beginTrackingBufferState(
            m_staticBucketTriangleMaterialBuffer,
            nvrhi::ResourceStates::Common);
        commandList->writeBuffer(
            m_staticBucketVertexBuffer,
            geometryPack.vertexBytes.data(),
            vertexBytes);
        commandList->writeBuffer(
            m_staticBucketIndexBuffer,
            geometryPack.indexes.data(),
            indexBytes);
        commandList->writeBuffer(
            m_staticBucketTriangleClassBuffer,
            geometryPack.triangleClasses.data(),
            classBytes);
        commandList->writeBuffer(
            m_staticBucketTriangleMaterialBuffer,
            geometryPack.triangleMaterials.data(),
            materialBytes);
        commandList->setBufferState(
            m_staticBucketVertexBuffer,
            nvrhi::ResourceStates::AccelStructBuildInput);
        commandList->setBufferState(
            m_staticBucketIndexBuffer,
            nvrhi::ResourceStates::AccelStructBuildInput);
        commandList->setBufferState(
            m_staticBucketTriangleClassBuffer,
            nvrhi::ResourceStates::ShaderResource);
        commandList->setBufferState(
            m_staticBucketTriangleMaterialBuffer,
            nvrhi::ResourceStates::ShaderResource);
        commandList->commitBarriers();
        stats.bufferUploads = 4;
        stats.uploadBytes =
            vertexBytes +
            indexBytes +
            classBytes +
            materialBytes;
        m_staticBucketUploadSignature =
            geometryPack.contentSignature;
    }

    for (StaticBucketBlasRecord& record :
        m_staticBucketBlasRecords)
    {
        record.seenThisUpdate = false;
    }

    int buildsRemaining =
        maxBuildsPerFrame > 0
            ? maxBuildsPerFrame
            : std::numeric_limits<int>::max();
    for (const RtSmokeStaticBucketPackedRecord& bucket :
        geometryPack.buckets)
    {
        if (bucket.range.vertexOffset < 0 ||
            bucket.range.vertexCount <= 0 ||
            bucket.range.indexOffset < 0 ||
            bucket.range.indexCount <= 0 ||
            bucket.range.triangleOffset < 0 ||
            bucket.range.triangleCount <= 0 ||
            bucket.range.indexCount !=
                bucket.range.triangleCount * 3 ||
            bucket.indexByteOffset +
                    bucket.indexByteSize >
                indexBytes)
        {
            ++stats.invalidBuckets;
            continue;
        }
        const RtSmokeStaticBucketBlasGeometryPlan geometryPlan =
            BuildSmokeStaticBucketBlasGeometryPlan(
                geometryPack,
                bucket);
        if (!geometryPlan.exact)
        {
            ++stats.invalidBuckets;
            stats.invalidSurfaceRecords +=
                Max(1, geometryPlan.invalidSurfaceRecords);
            continue;
        }
        stats.geometryDescCount +=
            static_cast<int>(geometryPlan.geometries.size());
        if (geometryPlan.geometries.size() > 1)
        {
            ++stats.multiGeometryBuckets;
        }

        StaticBucketBlasRecord* record = nullptr;
        for (StaticBucketBlasRecord& candidate :
            m_staticBucketBlasRecords)
        {
            if (candidate.bucketKey == bucket.bucketKey)
            {
                record = &candidate;
                break;
            }
        }
        if (!record)
        {
            StaticBucketBlasRecord added;
            added.bucketKey = bucket.bucketKey;
            m_staticBucketBlasRecords.push_back(added);
            record = &m_staticBucketBlasRecords.back();
        }
        record->seenThisUpdate = true;

        uint64 inputSignature = 14695981039346656037ull;
        inputSignature = HashSmokeBytes(
            inputSignature,
            &geometryPack.contentSignature,
            sizeof(geometryPack.contentSignature));
        inputSignature = HashSmokeBytes(
            inputSignature,
            &bucket.bucketKey,
            sizeof(bucket.bucketKey));
        inputSignature = HashSmokeBytes(
            inputSignature,
            &bucket.range,
            sizeof(bucket.range));
        const uintptr_t vertexBufferIdentity =
            reinterpret_cast<uintptr_t>(
                m_staticBucketVertexBuffer.Get());
        const uintptr_t indexBufferIdentity =
            reinterpret_cast<uintptr_t>(
                m_staticBucketIndexBuffer.Get());
        inputSignature = HashSmokeBytes(
            inputSignature,
            &vertexBufferIdentity,
            sizeof(vertexBufferIdentity));
        inputSignature = HashSmokeBytes(
            inputSignature,
            &indexBufferIdentity,
            sizeof(indexBufferIdentity));

        const bool rangeCompatible =
            record->range.vertexOffset ==
                bucket.range.vertexOffset &&
            record->range.vertexCount ==
                bucket.range.vertexCount &&
            record->range.indexOffset ==
                bucket.range.indexOffset &&
            record->range.indexCount ==
                bucket.range.indexCount &&
            record->range.triangleOffset ==
                bucket.range.triangleOffset &&
            record->range.triangleCount ==
                bucket.range.triangleCount &&
            record->geometryDescCount ==
                1u;
        const bool inputChanged =
            record->inputSignature != 0 &&
            record->inputSignature != inputSignature;
        if (record->blas &&
            (inputChanged ||
                !rangeCompatible ||
                buffersCreated))
        {
            RetireStaticBucketBlas(*record);
            ++stats.blasRetired;
        }

        const bool needsBuild =
            submitBuilds &&
            (forceRebuild ||
                uploadRequired ||
                !record->blas ||
                !record->buildSubmitted ||
                inputChanged);
        if (needsBuild && buildsRemaining > 0)
        {
            if (!record->blas)
            {
                record->blasDesc =
                    nvrhi::rt::AccelStructDesc()
                        .setBuildFlags(
                            nvrhi::rt::
                                AccelStructBuildFlags::
                                    PreferFastTrace)
                        .setDebugName(
                            "PathTraceStaticBucketBLAS");
                for (const
                    RtSmokeStaticBucketBlasGeometryRange&
                        geometryRange :
                    geometryPlan.geometries)
                {
                    nvrhi::rt::GeometryTriangles triangles;
                    triangles.vertexBuffer =
                        m_staticBucketVertexBuffer;
                    triangles.indexBuffer =
                        m_staticBucketIndexBuffer;
                    triangles.vertexFormat =
                        nvrhi::Format::RGB32_FLOAT;
                    triangles.indexFormat =
                        nvrhi::Format::R32_UINT;
                    triangles.vertexOffset = 0;
                    triangles.indexOffset =
                        geometryRange.indexByteOffset;
                    triangles.vertexCount =
                        geometryPack.stats.packedVertices;
                    triangles.indexCount =
                        geometryRange.indexCount;
                    triangles.vertexStride =
                        sizeof(PathTraceSmokeVertex);
                    nvrhi::rt::GeometryDesc geometry;
                    geometry.setTriangles(triangles);
                    record->blasDesc.
                        addBottomLevelGeometry(geometry);
                }
                record->blas =
                    device->createAccelStruct(
                        record->blasDesc);
                if (record->blas)
                {
                    ++stats.blasCreated;
                }
            }
            if (record->blas)
            {
                const auto buildStart =
                    std::chrono::steady_clock::now();
                const bool diagnosticMarkers =
                    r_pathTracingNsightGpuMarkers.GetInteger() != 0;
                idStr diagnosticMarkerName;
                if (diagnosticMarkers)
                {
                    diagnosticMarkerName.Format(
                        "GEO10.Accel BucketBLAS Build area=%d split=%u key=%llu",
                        bucket.portalArea,
                        bucket.splitIndex,
                        static_cast<unsigned long long>(
                            bucket.bucketKey));
                    commandList->beginMarker(
                        diagnosticMarkerName.c_str());
                }
                nvrhi::utils::BuildBottomLevelAccelStruct(
                    commandList,
                    record->blas,
                    record->blasDesc);
                if (diagnosticMarkers)
                {
                    commandList->endMarker();
                }
                const auto buildEnd =
                    std::chrono::steady_clock::now();
                stats.buildSubmitMicroseconds +=
                    static_cast<uint64>(
                        std::chrono::duration_cast<
                            std::chrono::microseconds>(
                            buildEnd - buildStart).count());
                record->buildSubmitted = true;
                ++stats.blasBuilt;
                --buildsRemaining;
            }
        }
        else if (record->blas &&
            record->buildSubmitted &&
            !needsBuild)
        {
            ++stats.blasReused;
        }

        record->inputSignature = inputSignature;
        record->range = bucket.range;
        record->firstSurfaceRecord =
            bucket.firstSurfaceRecord;
        record->surfaceRecordCount =
            bucket.surfaceRecordCount;
        record->geometryDescCount =
            static_cast<uint32_t>(
                geometryPlan.geometries.size());
    }

    for (size_t recordIndex = 0;
        recordIndex < m_staticBucketBlasRecords.size();)
    {
        if (m_staticBucketBlasRecords[recordIndex].
                seenThisUpdate)
        {
            ++recordIndex;
            continue;
        }
        if (m_staticBucketBlasRecords[recordIndex].blas)
        {
            RetireStaticBucketBlas(
                m_staticBucketBlasRecords[recordIndex]);
            ++stats.blasRetired;
        }
        m_staticBucketBlasRecords.erase(
            m_staticBucketBlasRecords.begin() +
            recordIndex);
    }
    for (const StaticBucketBlasRecord& record :
        m_staticBucketBlasRecords)
    {
        if (record.blas && record.buildSubmitted)
        {
            ++stats.readyBuckets;
            if (collectResultMemory)
            {
                const nvrhi::MemoryRequirements requirements =
                    device->getAccelStructMemoryRequirements(
                        record.blas);
                ++stats.blasResultQueries;
                if (requirements.size == 0)
                {
                    ++stats.blasResultQueryFailures;
                }
                else
                {
                    stats.blasResultBytes +=
                        requirements.size;
                    stats.blasResultMaxBytes =
                        Max(
                            stats.blasResultMaxBytes,
                            requirements.size);
                    stats.blasResultMaxAlignment =
                        Max(
                            stats.blasResultMaxAlignment,
                            requirements.alignment);
                }
                if (record.blas->isCompacted())
                {
                    ++stats.compactedBlases;
                }
            }
        }
        else
        {
            ++stats.deferredBuckets;
        }
    }
    stats.uploadSignature = m_staticBucketUploadSignature;
    return stats;
}

void RtSmokeGeometryUniverse::BuildStaticBucketTlasObservations(
    const RtSmokeStaticBucketGeometryPack& geometryPack,
    std::vector<RtSmokeStaticTlasBucketObservation>& buckets) const
{
    buckets.clear();
    buckets.reserve(geometryPack.buckets.size());
    for (size_t bucketIndex = 0;
        bucketIndex < geometryPack.buckets.size();
        ++bucketIndex)
    {
        const RtSmokeStaticBucketPackedRecord& packed =
            geometryPack.buckets[bucketIndex];
        const StaticBucketBlasRecord* blasRecord = nullptr;
        for (const StaticBucketBlasRecord& candidate :
            m_staticBucketBlasRecords)
        {
            if (candidate.bucketKey == packed.bucketKey)
            {
                blasRecord = &candidate;
                break;
            }
        }

        RtSmokeStaticTlasBucketObservation observation;
        observation.bucketKey = packed.bucketKey;
        observation.activeReasonFlags =
            packed.active
                ? RT_SMOKE_STATIC_ACTIVE_SELECTED_AREA
                : 0u;
        observation.resident = true;
        observation.active = packed.active;
        observation.hasBlas =
            blasRecord &&
            blasRecord->blas &&
            blasRecord->buildSubmitted;
        observation.routeRecordIndex =
            static_cast<uint32_t>(bucketIndex);
        observation.residentVertexOffset =
            packed.range.vertexOffset;
        observation.residentIndexOffset =
            packed.range.indexOffset;
        observation.residentTriangleOffset =
            packed.range.triangleOffset;
        observation.residentSurfaceCount =
            static_cast<int>(packed.surfaceRecordCount);
        observation.residentVertexCount =
            packed.range.vertexCount;
        observation.residentIndexCount =
            packed.range.indexCount;
        observation.residentTriangleCount =
            packed.range.triangleCount;
        if (packed.active)
        {
            observation.activeSurfaceCount =
                observation.residentSurfaceCount;
            observation.activeVertexCount =
                observation.residentVertexCount;
            observation.activeIndexCount =
                observation.residentIndexCount;
            observation.activeTriangleCount =
                observation.residentTriangleCount;
        }
        buckets.push_back(observation);
    }
}

bool RtSmokeGeometryUniverse::UpdateStaticBucketMaterialIndexGpuScaffold(
    nvrhi::IDevice* device,
    nvrhi::ICommandList* commandList,
    const RtSmokeStaticBucketGeometryPack& geometryPack,
    const std::vector<uint32_t>& triangleMaterialIndexes,
    uint64 materialBindingSignature)
{
    if (!device ||
        !commandList ||
        !geometryPack.exact ||
        triangleMaterialIndexes.size() !=
            geometryPack.triangleMaterials.size())
    {
        return false;
    }

    const size_t materialIndexBytes =
        triangleMaterialIndexes.size() * sizeof(uint32_t);
    bool buffersCreated = false;
    if (!RigidSmokeBufferHasCapacity(
            m_staticBucketTriangleMaterialIndexBuffer,
            materialIndexBytes,
            sizeof(uint32_t)))
    {
        RetireStaticBucketBuffer(
            m_staticBucketTriangleMaterialIndexBuffer);
        m_staticBucketTriangleMaterialIndexBuffer =
            CreateRigidSmokeBuffer(
                device,
                "PathTraceStaticBucketTriangleMaterialIndexes",
                materialIndexBytes,
                sizeof(uint32_t),
                false,
                false,
                false);
        buffersCreated = true;
    }
    if (!m_staticBucketTriangleMaterialIndexBuffer)
    {
        return false;
    }

    uint64 uploadSignature = 14695981039346656037ull;
    uploadSignature = HashSmokeBytes(
        uploadSignature,
        &geometryPack.contentSignature,
        sizeof(geometryPack.contentSignature));
    uploadSignature = HashSmokeBytes(
        uploadSignature,
        &materialBindingSignature,
        sizeof(materialBindingSignature));
    uploadSignature = HashSmokeBytes(
        uploadSignature,
        &materialIndexBytes,
        sizeof(materialIndexBytes));
    if (!buffersCreated &&
        uploadSignature ==
            m_staticBucketMaterialIndexUploadSignature)
    {
        return true;
    }

    commandList->beginTrackingBufferState(
        m_staticBucketTriangleMaterialIndexBuffer,
        nvrhi::ResourceStates::Common);
    if (!triangleMaterialIndexes.empty())
    {
        commandList->writeBuffer(
            m_staticBucketTriangleMaterialIndexBuffer,
            triangleMaterialIndexes.data(),
            materialIndexBytes);
    }
    commandList->setBufferState(
        m_staticBucketTriangleMaterialIndexBuffer,
        nvrhi::ResourceStates::ShaderResource);
    commandList->commitBarriers();
    m_staticBucketMaterialIndexUploadSignature =
        uploadSignature;
    return true;
}

void RtSmokeGeometryUniverse::ReleaseStaticBucketBlasGpuScaffold()
{
    RetireStaticBucketBuffer(m_staticBucketVertexBuffer);
    RetireStaticBucketBuffer(m_staticBucketIndexBuffer);
    RetireStaticBucketBuffer(m_staticBucketTriangleClassBuffer);
    RetireStaticBucketBuffer(m_staticBucketTriangleMaterialBuffer);
    RetireStaticBucketBuffer(
        m_staticBucketTriangleMaterialIndexBuffer);
    for (StaticBucketBlasRecord& record :
        m_staticBucketBlasRecords)
    {
        RetireStaticBucketBlas(record);
    }
    m_staticBucketBlasRecords.clear();
    m_staticBucketUploadSignature = 0;
    m_staticBucketMaterialIndexUploadSignature = 0;
}

void RtSmokeGeometryUniverse::RetireStaticBucketBuffer(
    nvrhi::BufferHandle& buffer)
{
    if (buffer)
    {
        m_retiredStaticBucketGpuResources.buffers.push_back(
            buffer);
        buffer = nullptr;
    }
}

void RtSmokeGeometryUniverse::RetireStaticBucketBlas(
    StaticBucketBlasRecord& record)
{
    if (record.blas)
    {
        m_retiredStaticBucketGpuResources.blases.push_back(
            record.blas);
        record.blas = nullptr;
    }
    record.blasDesc = nvrhi::rt::AccelStructDesc();
    record.buildSubmitted = false;
}

bool RtSmokeGeometryUniverse::TakeRetiredStaticBucketGpuResources(
    RtSmokeRetiredStaticBucketGpuResources& resources)
{
    resources = RtSmokeRetiredStaticBucketGpuResources();
    if (m_retiredStaticBucketGpuResources.Empty())
    {
        return false;
    }
    resources.buffers.swap(
        m_retiredStaticBucketGpuResources.buffers);
    resources.blases.swap(
        m_retiredStaticBucketGpuResources.blases);
    return true;
}

bool RtSmokeGeometryUniverse::HasStaticBucketGpuResources() const
{
    return m_staticBucketVertexBuffer ||
        m_staticBucketIndexBuffer ||
        m_staticBucketTriangleClassBuffer ||
        m_staticBucketTriangleMaterialBuffer ||
        m_staticBucketTriangleMaterialIndexBuffer ||
        !m_staticBucketBlasRecords.empty() ||
        !m_retiredStaticBucketGpuResources.Empty();
}

void RtSmokeGeometryUniverse::DumpStaticBucketBlasGpuStats(
    const RtPathTraceStaticBucketBlasGpuStats& stats) const
{
    common->Printf(
        "PathTracePrimaryPass: GEO10 static bucket GPU frame=%llu enabled/build=%d/%d signatures(content/upload)=%llu/%llu buckets(resident/active/ready/deferred/invalid/multiGeometry)=%d/%d/%d/%d/%d/%d geometry(v/i/t/cpuSurfaceRecords/descs/invalidRanges)=%d/%d/%d/%d/%d/%d metadata(cpuClassWords/gpuClassWords/legacySurfaceOffset/cpuSurfaceBytes)=%d/%d/%d/%llu bytes(v/i/meta/upload)=%llu/%llu/%llu/%llu buffers(create/upload)=%d/%d blas(create/build/reuse/retire/buildUs)=%d/%d/%d/%d/%llu result(queries/failures/bytes/max/alignment/compacted)=%d/%d/%llu/%llu/%llu/%d skips(device/cmd/pack)=%d/%d/%d storage=full-map-resident blasGeometry=one-range-per-bucket traversal=monolithic route=offline-only\n",
        static_cast<unsigned long long>(stats.frameIndex),
        stats.enabled,
        stats.submitBuilds,
        static_cast<unsigned long long>(
            stats.contentSignature),
        static_cast<unsigned long long>(
            stats.uploadSignature),
        stats.residentBuckets,
        stats.activeBuckets,
        stats.readyBuckets,
        stats.deferredBuckets,
        stats.invalidBuckets,
        stats.multiGeometryBuckets,
        stats.vertexCount,
        stats.indexCount,
        stats.triangleCount,
        stats.surfaceRecordCount,
        stats.geometryDescCount,
        stats.invalidSurfaceRecords,
        stats.staticClassMetadataWordCount,
        stats.triangleCount,
        stats.surfaceRecordWordOffset,
        static_cast<unsigned long long>(
            stats.surfaceRecordBytes),
        static_cast<unsigned long long>(stats.vertexBytes),
        static_cast<unsigned long long>(stats.indexBytes),
        static_cast<unsigned long long>(stats.metadataBytes),
        static_cast<unsigned long long>(stats.uploadBytes),
        stats.buffersCreated,
        stats.bufferUploads,
        stats.blasCreated,
        stats.blasBuilt,
        stats.blasReused,
        stats.blasRetired,
        static_cast<unsigned long long>(
            stats.buildSubmitMicroseconds),
        stats.blasResultQueries,
        stats.blasResultQueryFailures,
        static_cast<unsigned long long>(
            stats.blasResultBytes),
        static_cast<unsigned long long>(
            stats.blasResultMaxBytes),
        static_cast<unsigned long long>(
            stats.blasResultMaxAlignment),
        stats.compactedBlases,
        stats.skippedNoDevice,
        stats.skippedNoCommandList,
        stats.skippedInexactPack);
}

RtPathTraceStaticBucketActivePublication
RtSmokeGeometryUniverse::BuildStaticBucketActivePublication(
    const RtSmokeStaticBucketGeometryPack& geometryPack,
    uint64 sourceGeneration,
    uint64 storageGeneration,
    uint64 materialGeneration,
    int missingActiveMaterialIndexes,
    uint32_t instanceMask,
    const std::vector<bool>* activePortalAreas) const
{
    RtPathTraceStaticBucketActivePublication publication;
    publication.sourceGeneration = sourceGeneration;
    publication.storageGeneration = storageGeneration;
    publication.materialGeneration = materialGeneration;
    publication.missingMaterialIndexes =
        Max(0, missingActiveMaterialIndexes);
    publication.contentSignature =
        geometryPack.contentSignature;
    publication.residentBuckets =
        static_cast<int>(geometryPack.buckets.size());
    publication.activeBucketMask.assign(
        geometryPack.buckets.size(),
        0u);
    publication.activeSetSignature =
        14695981039346656037ull;

    for (size_t bucketIndex = 0;
         bucketIndex < geometryPack.buckets.size();
         ++bucketIndex)
    {
        const RtSmokeStaticBucketPackedRecord& bucket =
            geometryPack.buckets[bucketIndex];
        const bool bucketActive =
            activePortalAreas
                ? (bucket.portalArea ==
                        RT_SMOKE_STATIC_BUCKET_FALLBACK_AREA ||
                    (bucket.portalArea >= 0 &&
                        bucket.portalArea <
                            static_cast<int>(
                                activePortalAreas->size()) &&
                        (*activePortalAreas)[bucket.portalArea]))
                : bucket.active;
        publication.activeBucketMask[bucketIndex] =
            bucketActive ? 1u : 0u;
        if (bucketActive)
        {
            ++publication.activeBuckets;
            publication.activeSetSignature = HashSmokeBytes(
                publication.activeSetSignature,
                &bucket.bucketKey,
                sizeof(bucket.bucketKey));
            publication.activeSetSignature = HashSmokeBytes(
                publication.activeSetSignature,
                &bucket.range,
                sizeof(bucket.range));
            publication.activeSetSignature = HashSmokeBytes(
                publication.activeSetSignature,
                &bucket.firstSurfaceRecord,
                sizeof(bucket.firstSurfaceRecord));
            publication.activeSetSignature = HashSmokeBytes(
                publication.activeSetSignature,
                &bucket.surfaceRecordCount,
                sizeof(bucket.surfaceRecordCount));
        }
        const RtSmokeStaticBucketInstanceAddressPlan addressPlan =
            BuildSmokeStaticBucketInstanceAddressPlan(
                bucket.range,
                static_cast<int>(geometryPack.indexes.size()),
                static_cast<int>(
                    geometryPack.triangleClasses.size()));
        if (!addressPlan.rangeValid ||
            !addressPlan.indexAddressCompatible)
        {
            ++publication.invalidRanges;
            continue;
        }
        if (!addressPlan.instanceIdEncodable)
        {
            ++publication.instanceIdOverflow;
            continue;
        }
        if (!bucketActive)
        {
            continue;
        }

        const StaticBucketBlasRecord* blasRecord = nullptr;
        for (const StaticBucketBlasRecord& candidate :
            m_staticBucketBlasRecords)
        {
            if (candidate.bucketKey == bucket.bucketKey)
            {
                blasRecord = &candidate;
                break;
            }
        }
        if (!blasRecord ||
            !blasRecord->blas ||
            !blasRecord->buildSubmitted)
        {
            ++publication.missingBlas;
            continue;
        }
        ++publication.readyActiveBuckets;
    }

    publication.publicationGeneration =
        14695981039346656037ull;
    publication.publicationGeneration = HashSmokeBytes(
        publication.publicationGeneration,
        &publication.sourceGeneration,
        sizeof(publication.sourceGeneration));
    publication.publicationGeneration = HashSmokeBytes(
        publication.publicationGeneration,
        &publication.storageGeneration,
        sizeof(publication.storageGeneration));
    publication.publicationGeneration = HashSmokeBytes(
        publication.publicationGeneration,
        &publication.materialGeneration,
        sizeof(publication.materialGeneration));
    publication.publicationGeneration = HashSmokeBytes(
        publication.publicationGeneration,
        &publication.contentSignature,
        sizeof(publication.contentSignature));
    publication.publicationGeneration = HashSmokeBytes(
        publication.publicationGeneration,
        &publication.activeSetSignature,
        sizeof(publication.activeSetSignature));
    publication.publicationGeneration = HashSmokeBytes(
        publication.publicationGeneration,
        &publication.activeBuckets,
        sizeof(publication.activeBuckets));
    if (publication.publicationGeneration == 0)
    {
        publication.publicationGeneration = 1;
    }

    publication.activeSetExact =
        geometryPack.exact &&
        storageGeneration == m_staticGeometryGeneration &&
        publication.activeBuckets > 0 &&
        publication.readyActiveBuckets ==
            publication.activeBuckets &&
        publication.missingBlas == 0 &&
        publication.missingMaterialIndexes == 0 &&
        publication.invalidRanges == 0 &&
        publication.instanceIdOverflow == 0;
    if (!publication.activeSetExact)
    {
        return publication;
    }

    publication.tlasInstances.reserve(
        publication.activeBuckets);
    publication.routeRecords.reserve(
        publication.residentBuckets);
    for (size_t bucketIndex = 0;
         bucketIndex < geometryPack.buckets.size();
         ++bucketIndex)
    {
        const RtSmokeStaticBucketPackedRecord& bucket =
            geometryPack.buckets[bucketIndex];
        const bool bucketActive =
            publication.activeBucketMask[bucketIndex] != 0u;
        const RtSmokeStaticBucketInstanceAddressPlan addressPlan =
            BuildSmokeStaticBucketInstanceAddressPlan(
                bucket.range,
                static_cast<int>(geometryPack.indexes.size()),
                static_cast<int>(
                    geometryPack.triangleClasses.size()));
        if (!addressPlan.valid)
        {
            publication.tlasInstances.clear();
            publication.routeRecords.clear();
            publication.activeSetExact = false;
            ++publication.instanceIdOverflow;
            return publication;
        }
        const uint32_t instanceId = addressPlan.instanceId;
        if (bucketActive)
        {
            const StaticBucketBlasRecord* blasRecord = nullptr;
            for (const StaticBucketBlasRecord& candidate :
                m_staticBucketBlasRecords)
            {
                if (candidate.bucketKey == bucket.bucketKey)
                {
                    blasRecord = &candidate;
                    break;
                }
            }
            if (!blasRecord ||
                !blasRecord->blas ||
                !blasRecord->buildSubmitted)
            {
                publication.tlasInstances.clear();
                publication.routeRecords.clear();
                publication.activeSetExact = false;
                ++publication.missingBlas;
                return publication;
            }

            nvrhi::rt::AffineTransform transform;
            transform[0] = 1.0f;
            transform[1] = 0.0f;
            transform[2] = 0.0f;
            transform[3] = 0.0f;
            transform[4] = 0.0f;
            transform[5] = 1.0f;
            transform[6] = 0.0f;
            transform[7] = 0.0f;
            transform[8] = 0.0f;
            transform[9] = 0.0f;
            transform[10] = 1.0f;
            transform[11] = 0.0f;
            nvrhi::rt::InstanceDesc instanceDesc;
            instanceDesc
                .setInstanceID(instanceId)
                .setInstanceMask(instanceMask)
                .setInstanceContributionToHitGroupIndex(
                    0)
                .setFlags(
                    nvrhi::rt::InstanceFlags::TriangleCullDisable)
                .setTransform(transform)
                .setBLAS(blasRecord->blas);
            publication.tlasInstances.push_back(instanceDesc);
        }

        RtPathTraceStaticBucketRouteRecord route;
        route.instanceId = instanceId;
        route.vertexOffset =
            static_cast<uint32_t>(
                bucket.range.vertexOffset);
        route.indexOffset =
            static_cast<uint32_t>(
                bucket.range.indexOffset);
        route.triangleOffset =
            static_cast<uint32_t>(
                bucket.range.triangleOffset);
        route.vertexCount =
            static_cast<uint32_t>(
                bucket.range.vertexCount);
        route.indexCount =
            static_cast<uint32_t>(
                bucket.range.indexCount);
        route.triangleCount =
            static_cast<uint32_t>(
                bucket.range.triangleCount);
        route.surfaceCount = bucket.surfaceRecordCount;
        route.generationLo =
            static_cast<uint32_t>(
                publication.publicationGeneration);
        route.generationHi =
            static_cast<uint32_t>(
                publication.publicationGeneration >> 32);
        route.bucketKeyLo =
            static_cast<uint32_t>(bucket.bucketKey);
        route.bucketKeyHi =
            static_cast<uint32_t>(bucket.bucketKey >> 32);
        publication.routeRecords.push_back(route);
    }

    publication.tlasGeneration =
        publication.publicationGeneration;
    publication.routeGeneration =
        publication.publicationGeneration;
    RtSmokeStaticBucketPublicationEpochInput epochInput;
    epochInput.expectedGeneration =
        publication.publicationGeneration;
    epochInput.tlasGeneration =
        publication.tlasGeneration;
    epochInput.routeGeneration =
        publication.routeGeneration;
    epochInput.residentBuckets =
        publication.residentBuckets;
    epochInput.activeBuckets =
        publication.activeBuckets;
    epochInput.tlasInstances =
        static_cast<int>(
            publication.tlasInstances.size());
    epochInput.routeRecords =
        static_cast<int>(
            publication.routeRecords.size());
    epochInput.activeSetExact =
        publication.activeSetExact;
    const RtSmokeStaticBucketPublicationEpochPlan epochPlan =
        BuildSmokeStaticBucketPublicationEpochPlan(epochInput);
    publication.valid = epochPlan.accepted;
    publication.mixedEpochRejected =
        epochPlan.mixedEpochRejected;
    if (!publication.valid)
    {
        publication.tlasInstances.clear();
        publication.routeRecords.clear();
    }
    return publication;
}

void RtSmokeGeometryUniverse::DumpStaticBucketActivePublication(
    const RtPathTraceStaticBucketActivePublication&
        publication) const
{
    common->Printf(
        "PathTracePrimaryPass: GEO10 static bucket publication valid/exact/mixedRejected=%d/%d/%d generations(source/storage/material/publication/tlas/route)=%llu/%llu/%llu/%llu/%llu/%llu signatures(content/active)=%llu/%llu buckets(resident/active/ready)=%d/%d/%d outputs(tlas/routes)=%llu/%llu failures(missingBlas/missingMaterialIndex/invalidRange/instanceOverflow)=%d/%d/%d/%d traversal=monolithic route=shadow-only\n",
        publication.valid ? 1 : 0,
        publication.activeSetExact ? 1 : 0,
        publication.mixedEpochRejected ? 1 : 0,
        static_cast<unsigned long long>(
            publication.sourceGeneration),
        static_cast<unsigned long long>(
            publication.storageGeneration),
        static_cast<unsigned long long>(
            publication.materialGeneration),
        static_cast<unsigned long long>(
            publication.publicationGeneration),
        static_cast<unsigned long long>(
            publication.tlasGeneration),
        static_cast<unsigned long long>(
            publication.routeGeneration),
        static_cast<unsigned long long>(
            publication.contentSignature),
        static_cast<unsigned long long>(
            publication.activeSetSignature),
        publication.residentBuckets,
        publication.activeBuckets,
        publication.readyActiveBuckets,
        static_cast<unsigned long long>(
            publication.tlasInstances.size()),
        static_cast<unsigned long long>(
            publication.routeRecords.size()),
        publication.missingBlas,
        publication.missingMaterialIndexes,
        publication.invalidRanges,
        publication.instanceIdOverflow);
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
    stats.staticGeometryGeneration = m_staticGeometryGeneration;
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
        record->modelSurfaceIndex = observation.modelSurfaceIndex;
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
    record.modelSurfaceIndex = observation.modelSurfaceIndex;
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
            nvrhi::BufferHandle replacement =
                CreateRigidSmokeBuffer(device, "PathTraceRigidMeshLocalVertices", requiredVertexBytes, sizeof(PathTraceSmokeVertex), true, false);
            createdVertexBuffer = replacement != nullptr;
            if (createdVertexBuffer)
            {
                RetireRigidBuffer(record.rigidVertexBuffer);
                record.rigidVertexBuffer = replacement;
                record.gpuBuffersUploaded = false;
                ++stats.vertexBuffersCreated;
            }
        }

        if (RigidSmokeBufferHasCapacity(record.rigidIndexBuffer, requiredIndexBytes, sizeof(uint32_t)))
        {
            ++stats.indexBuffersReused;
        }
        else
        {
            nvrhi::BufferHandle replacement =
                CreateRigidSmokeBuffer(device, "PathTraceRigidMeshLocalIndices", requiredIndexBytes, sizeof(uint32_t), false, true);
            createdIndexBuffer = replacement != nullptr;
            if (createdIndexBuffer)
            {
                RetireRigidBuffer(record.rigidIndexBuffer);
                record.rigidIndexBuffer = replacement;
                record.gpuBuffersUploaded = false;
                ++stats.indexBuffersCreated;
            }
        }

        if (!RigidSmokeBufferHasCapacity(
                record.rigidVertexBuffer,
                requiredVertexBytes,
                sizeof(PathTraceSmokeVertex)) ||
            !RigidSmokeBufferHasCapacity(
                record.rigidIndexBuffer,
                requiredIndexBytes,
                sizeof(uint32_t)))
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
                if (record.rigidBlas)
                {
                    RetireRigidBlas(record);
                    ++stats.blasRecreatedForInputChange;
                }
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
        RetireRigidMeshGpuResources(record);
    }
}

void RtSmokeGeometryUniverse::ClearRetiredRigidBlas()
{
    m_retiredRigidGpuResources =
        RtSmokeRetiredRigidGpuResources();
    m_canonicalSourceGpuPools.ClearRetiredBuffers();
    m_canonicalOffsetBlasProbe.ClearRetiredBlases();
}

bool RtSmokeGeometryUniverse::HasRetiredRigidGpuResources() const
{
    return !m_retiredRigidGpuResources.Empty() ||
        m_canonicalSourceGpuPools.RetiredBufferCount() != 0 ||
        m_canonicalOffsetBlasProbe.RetiredBlasCount() != 0;
}

bool RtSmokeGeometryUniverse::TakeRetiredRigidGpuResources(
    RtSmokeRetiredRigidGpuResources& resources)
{
    const std::size_t canonicalPoolBufferCount =
        m_canonicalSourceGpuPools.TakeRetiredBuffers(
            m_retiredRigidGpuResources.buffers);
    m_retiredRigidGpuResources.canonicalPoolBufferCount +=
        static_cast<int>(canonicalPoolBufferCount);
    const std::size_t canonicalProbeBlasCount =
        m_canonicalOffsetBlasProbe.TakeRetiredBlases(
            m_retiredRigidGpuResources.blases);
    m_retiredRigidGpuResources.canonicalProbeBlasCount +=
        static_cast<int>(canonicalProbeBlasCount);
    m_canonicalSourceGpuPoolStats.retiredBuffers = 0;

    resources = RtSmokeRetiredRigidGpuResources();
    if (m_retiredRigidGpuResources.Empty())
    {
        return false;
    }
    resources.buffers.swap(
        m_retiredRigidGpuResources.buffers);
    resources.blases.swap(
        m_retiredRigidGpuResources.blases);
    resources.legacyBufferCount =
        m_retiredRigidGpuResources.legacyBufferCount;
    resources.legacyBlasCount =
        m_retiredRigidGpuResources.legacyBlasCount;
    resources.canonicalBlasCount =
        m_retiredRigidGpuResources.canonicalBlasCount;
    resources.canonicalPoolBufferCount =
        m_retiredRigidGpuResources.canonicalPoolBufferCount;
    resources.canonicalProbeBlasCount =
        m_retiredRigidGpuResources.canonicalProbeBlasCount;
    m_retiredRigidGpuResources =
        RtSmokeRetiredRigidGpuResources();
    return true;
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
        m_retiredRigidGpuResources.legacyBlasCount,
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
    std::unordered_set<const idRenderEntityLocal*> liveEntityDefs;
    liveEntityDefs.reserve(static_cast<size_t>(renderWorld->entityDefs.Num()));
    for (int entityIndex = 0; entityIndex < renderWorld->entityDefs.Num(); ++entityIndex)
    {
        const idRenderEntityLocal* liveEntity = renderWorld->entityDefs[entityIndex];
        if (liveEntity)
        {
            liveEntityDefs.insert(liveEntity);
        }
    }

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
            // Portal-area references can survive same-object map reload teardown
            // long enough to expose a freed entity pointer to this late PT walk.
            // Treat entityDefs as the authoritative live-membership set before
            // reading any field (including parms.hModel) from the reference.
            if (!entity || liveEntityDefs.find(entity) == liveEntityDefs.end())
            {
                ++m_rigidResidencyAreaWalkRejectedEntitiesThisFrame;
                continue;
            }
            if (entity->world != renderWorld ||
                entity->index < 0 ||
                entity->index >= renderWorld->entityDefs.Num() ||
                renderWorld->entityDefs[entity->index] != entity)
            {
                ++m_rigidResidencyAreaWalkRejectedEntitiesThisFrame;
                continue;
            }
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
                    candidateObservation.modelSurfaceIndex =
                        rigidSnapshot.modelSurfaceIndex;
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
                RetireRigidMeshGpuResources(record);
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
    m_rigidResidencyStats.retiredBlasPending =
        m_retiredRigidGpuResources.legacyBlasCount;
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
        const uint32_t materialId =
            plannedInstance.materialId != 0u
                ? plannedInstance.materialId
                : record.materialId;
        if (std::find(materialIds.begin(), materialIds.end(), materialId) == materialIds.end())
        {
            materialIds.push_back(materialId);
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
        observation.materialId = instance.materialOverrideId;
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
        PtCanonicalInstanceKey canonicalInstance;
        canonicalInstance.worldGeneration =
            instance.renderDefKey.worldGeneration;
        canonicalInstance.renderDefIndex =
            instance.renderDefKey.index >= 0
                ? static_cast<uint32_t>(instance.renderDefKey.index)
                : UINT32_MAX;
        canonicalInstance.renderDefGeneration =
            instance.renderDefKey.generation;
        canonicalInstance.subInstanceKind =
            PtCanonicalSubInstanceKind::RigidSurface;
        canonicalInstance.modelSurfaceIndex =
            instance.modelSurfaceIndex >= 0
                ? static_cast<uint32_t>(instance.modelSurfaceIndex)
                : UINT32_MAX;
        canonicalInstance.jointSubmeshIndex = -1;
        const PtGeometryIdentityBinding* binding =
            PtCanonicalInstanceKeyIsValid(canonicalInstance)
                ? m_canonicalIdentityRegistry.Find(canonicalInstance)
                : nullptr;
        if (binding != nullptr)
        {
            observation.canonicalMeshHash = binding->meshHash;
            observation.canonicalBlasRecordIndex =
                FindCanonicalRigidBlasRecordIndex(
                    binding->meshKey,
                    binding->meshHash);
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

RtPathTraceCanonicalRigidTlasStats
RtSmokeGeometryUniverse::BuildCanonicalRigidTlasInstanceDescs(
    const RtSmokeRigidTlasPlan& plan,
    int legacyDescriptorCount,
    std::vector<nvrhi::rt::InstanceDesc>& instanceDescs) const
{
    RtPathTraceCanonicalRigidTlasStats stats;
    stats.frameIndex = m_currentFrameIndex;
    stats.enabled =
        r_pathTracingGeometryCanonicalRigidBlas.GetInteger() != 0 ? 1 : 0;
    stats.plannedInstances = static_cast<int>(plan.instances.size());
    stats.legacyDescriptors = legacyDescriptorCount;
    if (!stats.enabled)
    {
        return stats;
    }

    for (const RtSmokePlanTlasInstance& plannedInstance :
        plan.instances)
    {
        if (!plannedInstance.sourceSeenThisFrame &&
            r_pathTracingResidencyRouteCachedTlas.GetInteger() == 0)
        {
            continue;
        }
        if (plannedInstance.canonicalBlasRecordIndex >=
            m_canonicalRigidBlasRecords.size())
        {
            ++stats.missingRecordIndex;
            continue;
        }
        const CanonicalRigidBlasRecord& record =
            m_canonicalRigidBlasRecords[
                plannedInstance.canonicalBlasRecordIndex];
        if (record.meshHash != plannedInstance.canonicalMeshHash)
        {
            ++stats.meshHashMismatch;
            continue;
        }
        ++stats.exactRecordMappings;
        if (!record.blas || !record.buildSubmitted)
        {
            ++stats.missingBlas;
            continue;
        }

        nvrhi::rt::AffineTransform transform;
        BuildRigidTlasAffineTransform(
            plannedInstance.transform,
            transform);
        const bool cachedSource =
            !plannedInstance.sourceSeenThisFrame;
        const uint32 instanceMask =
            cachedSource &&
            r_pathTracingResidencyRouteCachedTraceMask.GetInteger() == 0
                ? 0u
                : plannedInstance.instanceMask;
        nvrhi::rt::InstanceDesc instanceDesc;
        instanceDesc
            .setInstanceID(plannedInstance.instanceId)
            .setInstanceMask(instanceMask)
            .setInstanceContributionToHitGroupIndex(
                plannedInstance.hitGroupContribution)
            .setFlags(
                nvrhi::rt::InstanceFlags::TriangleCullDisable)
            .setTransform(transform)
            .setBLAS(record.blas);
        instanceDescs.push_back(instanceDesc);
        ++stats.canonicalDescriptors;
    }
    return stats;
}

void RtSmokeGeometryUniverse::DumpCanonicalRigidTlasStats(
    const RtPathTraceCanonicalRigidTlasStats& stats) const
{
    common->Printf(
        "PathTracePrimaryPass: GEO06 canonical rigid TLAS descriptors frame=%llu enabled=%d planned=%d descriptors(legacy/canonical)=%d/%d exactRecordMappings=%d failures(missingIndex/hashMismatch/missingBlas)=%d/%d/%d submit(requested/eligible/selected)=%d/%d/%d traversal=%s\n",
        static_cast<unsigned long long>(stats.frameIndex),
        stats.enabled,
        stats.plannedInstances,
        stats.legacyDescriptors,
        stats.canonicalDescriptors,
        stats.exactRecordMappings,
        stats.missingRecordIndex,
        stats.meshHashMismatch,
        stats.missingBlas,
        stats.traversalRequested,
        stats.exactParity,
        stats.selectedForSubmit,
        stats.selectedForSubmit ? "canonical" : "legacy");
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
        routeInstance.materialId =
            plannedInstance.materialId != 0u
                ? plannedInstance.materialId
                : geometryRange.materialId;
        routeInstance.materialIndex = FindRigidRouteMaterialTableIndex(
            snapshot.materialTableIds,
            routeInstance.materialId,
            build.stats.missingMaterialTableIndex);
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
        routeInstance.materialId =
            plannedInstance.materialId != 0u
                ? plannedInstance.materialId
                : geometryRange.materialId;
        routeInstance.materialIndex = FindRigidRouteMaterialTableIndex(
            materialTableIds,
            routeInstance.materialId,
            build.stats.missingMaterialTableIndex);
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
