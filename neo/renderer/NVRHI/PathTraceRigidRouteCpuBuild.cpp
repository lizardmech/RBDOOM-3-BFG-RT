#include "precompiled.h"
#pragma hdrstop

#include "PathTraceGeometryUniverse.h"

#include <algorithm>
#include <cmath>

namespace
{
template<typename T>
RtSmokePlanDataSpan MakePlanSpan(const std::vector<T>& data)
{
    RtSmokePlanDataSpan span;
    span.data = data.empty() ? nullptr : data.data();
    span.elementSize = sizeof(T);
    span.elementCount = data.size();
    return span;
}

void CopyTransformRows(float dst[12], const float matrix[16])
{
    dst[0] = matrix[0]; dst[1] = matrix[4]; dst[2] = matrix[8]; dst[3] = matrix[12];
    dst[4] = matrix[1]; dst[5] = matrix[5]; dst[6] = matrix[9]; dst[7] = matrix[13];
    dst[8] = matrix[2]; dst[9] = matrix[6]; dst[10] = matrix[10]; dst[11] = matrix[14];
}

bool TransformUsable(const float matrix[16])
{
    for (int index = 0; index < 16; ++index)
    {
        if (!std::isfinite(matrix[index]) || std::fabs(matrix[index]) >= 100000.0f)
            return false;
    }
    const float basis0 = matrix[0] * matrix[0] + matrix[1] * matrix[1] + matrix[2] * matrix[2];
    const float basis1 = matrix[4] * matrix[4] + matrix[5] * matrix[5] + matrix[6] * matrix[6];
    const float basis2 = matrix[8] * matrix[8] + matrix[9] * matrix[9] + matrix[10] * matrix[10];
    return basis0 > 1.0e-8f && basis0 < 1000000.0f &&
        basis1 > 1.0e-8f && basis1 < 1000000.0f &&
        basis2 > 1.0e-8f && basis2 < 1000000.0f;
}

void TransformPoint(const float matrix[16], const idVec3& local, idVec3& world)
{
    world.x = local.x * matrix[0] + local.y * matrix[4] + local.z * matrix[8] + matrix[12];
    world.y = local.x * matrix[1] + local.y * matrix[5] + local.z * matrix[9] + matrix[13];
    world.z = local.x * matrix[2] + local.y * matrix[6] + local.z * matrix[10] + matrix[14];
}

const RtPathTraceRigidRouteMeshSnapshot* FindMesh(
    const std::vector<RtPathTraceRigidRouteMeshSnapshot>& meshes,
    std::uint32_t routeRecordIndex)
{
    for (const RtPathTraceRigidRouteMeshSnapshot& mesh : meshes)
        if (mesh.routeRecordIndex == routeRecordIndex) return &mesh;
    return nullptr;
}

const RtPathTraceRigidRouteGeometryRange* FindRange(
    const RtPathTraceRigidRouteBuild& build,
    const RtPathTraceRigidRouteMeshSnapshot& mesh)
{
    const std::uint32_t flags = mesh.triangleClassAndFlags != 0u
        ? mesh.triangleClassAndFlags : mesh.surfaceClassId;
    for (const RtPathTraceRigidRouteGeometryRange& range : build.geometryRanges)
    {
        if (range.meshHash == mesh.meshHash &&
            range.gpuUploadSignature == mesh.gpuUploadSignature &&
            range.vertexCount == mesh.vertexCount &&
            range.indexCount == mesh.indexCount &&
            range.materialId == mesh.materialId &&
            range.triangleClassAndFlags == flags)
            return &range;
    }
    return nullptr;
}

std::uint32_t FindMaterialIndex(
    const std::vector<std::uint32_t>& ids,
    std::uint32_t id,
    int& missing)
{
    for (std::size_t index = 0; index < ids.size(); ++index)
        if (ids[index] == id) return static_cast<std::uint32_t>(index);
    ++missing;
    return 0;
}

void AppendPlaceholder(
    RtPathTraceRigidRouteBuild& build,
    const RtSmokePlanTlasInstance& planned)
{
    PathTraceRigidRouteInstance instance;
    instance.instanceIdLo = static_cast<std::uint32_t>(planned.sourceInstanceId);
    instance.instanceIdHi = static_cast<std::uint32_t>(planned.sourceInstanceId >> 32);
    if (planned.hasPreviousTransform) instance.flags |= PT_RIGID_ROUTE_HAS_PREVIOUS_TRANSFORM;
    if (planned.transformContinuous) instance.flags |= PT_RIGID_ROUTE_TRANSFORM_CONTINUOUS;
    if (!planned.sourceSeenThisFrame) instance.flags |= PT_RIGID_ROUTE_CACHED_SOURCE;
    CopyTransformRows(instance.currentObjectToWorld, planned.transform);
    CopyTransformRows(instance.previousObjectToWorld,
        planned.hasPreviousTransform ? planned.previousTransform : planned.transform);
    build.instances.push_back(instance);
    build.instanceSeenThisFrame.push_back(planned.sourceSeenThisFrame ? 1u : 0u);
    std::array<float, 16> matrix = {};
    std::copy(planned.transform, planned.transform + 16, matrix.begin());
    build.instanceObjectToWorld.push_back(matrix);
}
}

RtPathTraceRigidRouteInstanceEligibility
BuildRigidRouteInstanceEligibilityFromPod(
    const RtSmokePlanTlasInstance& plannedInstance,
    const RtPathTraceRigidRouteMeshSnapshot& mesh)
{
    RtPathTraceRigidRouteInstanceEligibility result;
    result.transformUsable = TransformUsable(plannedInstance.transform);
    if (!result.transformUsable || !mesh.localBoundsValid ||
        mesh.localBounds.IsCleared()) return result;
    idVec3 mins;
    idVec3 maxs;
    bool first = true;
    for (int cornerIndex = 0; cornerIndex < 8; ++cornerIndex)
    {
        idVec3 local;
        local.x = mesh.localBounds[(cornerIndex ^ (cornerIndex >> 1)) & 1].x;
        local.y = mesh.localBounds[(cornerIndex >> 1) & 1].y;
        local.z = mesh.localBounds[(cornerIndex >> 2) & 1].z;
        idVec3 world;
        TransformPoint(plannedInstance.transform, local, world);
        if (!std::isfinite(world.x) || !std::isfinite(world.y) || !std::isfinite(world.z) ||
            std::fabs(world.x) >= 100000.0f || std::fabs(world.y) >= 100000.0f ||
            std::fabs(world.z) >= 100000.0f) return result;
        if (first) { mins = maxs = world; first = false; }
        else
        {
            mins.x = std::min(mins.x, world.x); mins.y = std::min(mins.y, world.y);
            mins.z = std::min(mins.z, world.z); maxs.x = std::max(maxs.x, world.x);
            maxs.y = std::max(maxs.y, world.y); maxs.z = std::max(maxs.z, world.z);
        }
    }
    const idVec3 extent = maxs - mins;
    result.worldBoundsValid = extent.x <= 32768.0f &&
        extent.y <= 32768.0f && extent.z <= 32768.0f;
    return result;
}

bool RemapRigidRouteMaterialIndexes(
    RtPathTraceRigidRouteBuild& build,
    const std::vector<std::uint32_t>& materialTableIds,
    bool* geometryChangedOut,
    bool* instanceChangedOut)
{
    bool geometryChanged = false;
    bool instanceChanged = false;
    int missingMaterialTableIndex = 0;
    for (RtPathTraceRigidRouteGeometryRange& range : build.geometryRanges)
    {
        const std::uint32_t materialIndex = FindMaterialIndex(
            materialTableIds, range.materialId, missingMaterialTableIndex);
        if (range.materialIndex == materialIndex) continue;

        range.materialIndex = materialIndex;
        const std::size_t triangleEnd =
            static_cast<std::size_t>(range.triangleOffset) +
            static_cast<std::size_t>(range.triangleCount);
        if (triangleEnd <= build.triangleMaterialIndexes.size())
        {
            for (std::size_t triangleIndex = range.triangleOffset;
                 triangleIndex < triangleEnd; ++triangleIndex)
            {
                build.triangleMaterialIndexes[triangleIndex] = materialIndex;
            }
        }
        geometryChanged = true;
    }
    for (PathTraceRigidRouteInstance& instance : build.instances)
    {
        const std::uint32_t materialIndex = FindMaterialIndex(
            materialTableIds, instance.materialId, missingMaterialTableIndex);
        if (instance.materialIndex == materialIndex) continue;
        instance.materialIndex = materialIndex;
        instanceChanged = true;
    }
    build.stats.missingMaterialTableIndex = missingMaterialTableIndex;
    if (geometryChangedOut != nullptr) *geometryChangedOut = geometryChanged;
    if (instanceChangedOut != nullptr) *instanceChangedOut = instanceChanged;
    return geometryChanged || instanceChanged;
}

bool BuildRigidRouteBuffersFromSnapshotPreReserved(
    const RtPathTraceRigidRouteBuildSnapshot& snapshot,
    RtPathTraceRigidRouteBuild& build,
    std::vector<std::uint64_t>& emittedMeshScratch)
{
    return BuildRigidRouteBuffersFromResidentPayloadPreReserved(
        snapshot.meshes, snapshot.plan, snapshot.materialTableIds,
        snapshot.instanceEligibility, build, emittedMeshScratch);
}

bool BuildRigidRouteBuffersFromResidentPayloadPreReserved(
    const std::vector<RtPathTraceRigidRouteMeshSnapshot>& meshes,
    const RtSmokeRigidTlasPlan& plan,
    const std::vector<std::uint32_t>& materialTableIds,
    const std::vector<RtPathTraceRigidRouteInstanceEligibility>& instanceEligibility,
    RtPathTraceRigidRouteBuild& build,
    std::vector<std::uint64_t>& emittedMeshScratch)
{
    if (instanceEligibility.size() != plan.instances.size() ||
        build.vertices.capacity() < [&]() { size_t n=0; for (const auto& m:meshes) n+=m.vertices.size(); return n; }() ||
        build.indexes.capacity() < [&]() { size_t n=0; for (const auto& m:meshes) n+=m.indexes.size(); return n; }() ||
        build.instances.capacity() < plan.instances.size() ||
        emittedMeshScratch.capacity() < plan.instances.size()) return false;

    build.vertices.clear(); build.indexes.clear(); build.triangleMaterials.clear();
    build.triangleMaterialIndexes.clear(); build.triangleClassAndFlags.clear();
    build.geometryRanges.clear(); build.instances.clear();
    build.instanceObjectToWorld.clear(); build.instanceSeenThisFrame.clear();
    build.stats = RtPathTraceRigidRouteBuildStats();
    emittedMeshScratch.clear();

    for (const RtPathTraceRigidRouteMeshSnapshot& mesh : meshes)
    {
        if (!mesh.valid || !mesh.routeReady || mesh.vertices.empty() || mesh.indexes.empty() ||
            FindRange(build, mesh)) continue;
        RtPathTraceRigidRouteGeometryRange range;
        range.meshHash = mesh.meshHash;
        range.gpuUploadSignature = mesh.gpuUploadSignature;
        range.vertexOffset = static_cast<std::uint32_t>(build.vertices.size());
        range.indexOffset = static_cast<std::uint32_t>(build.indexes.size());
        range.triangleOffset = static_cast<std::uint32_t>(build.triangleMaterials.size());
        range.vertexCount = static_cast<std::uint32_t>(mesh.vertices.size());
        range.indexCount = static_cast<std::uint32_t>(mesh.indexes.size());
        range.triangleCount = static_cast<std::uint32_t>(mesh.indexes.size() / 3u);
        range.materialId = mesh.materialId;
        range.materialIndex = FindMaterialIndex(materialTableIds,
            mesh.materialId, build.stats.missingMaterialTableIndex);
        range.triangleClassAndFlags = mesh.triangleClassAndFlags != 0u
            ? mesh.triangleClassAndFlags : mesh.surfaceClassId;
        build.vertices.insert(build.vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
        build.indexes.insert(build.indexes.end(), mesh.indexes.begin(), mesh.indexes.end());
        for (std::uint32_t triangle = 0; triangle < range.triangleCount; ++triangle)
        {
            build.triangleMaterials.push_back(range.materialId);
            build.triangleMaterialIndexes.push_back(range.materialIndex);
            build.triangleClassAndFlags.push_back(range.triangleClassAndFlags);
        }
        build.geometryRanges.push_back(range);
    }

    build.stats.visibleInstances = plan.visibleInstances;
    build.stats.skippedNonRigid = plan.rejectedNonRigid;
    build.stats.skippedMissingMesh = plan.rejectedMissingMesh +
        plan.rejectedStaleMesh;
    build.stats.skippedMissingBlas = plan.rejectedMissingBlas;
    for (std::size_t plannedIndex = 0; plannedIndex < plan.instances.size(); ++plannedIndex)
    {
        const RtSmokePlanTlasInstance& planned = plan.instances[plannedIndex];
        const RtPathTraceRigidRouteMeshSnapshot* mesh = FindMesh(meshes, planned.routeRecordIndex);
        if (!mesh || !mesh->valid || mesh->meshHash != planned.meshHash)
        {
            ++build.stats.skippedMissingMesh; AppendPlaceholder(build, planned); continue;
        }
        const RtPathTraceRigidRouteInstanceEligibility& eligibility =
            instanceEligibility[plannedIndex];
        if (!mesh->routeReady || !mesh->localBoundsValid ||
            !eligibility.transformUsable || !eligibility.worldBoundsValid)
        {
            ++build.stats.skippedMissingBlas; AppendPlaceholder(build, planned); continue;
        }
        const RtPathTraceRigidRouteGeometryRange* range = FindRange(build, *mesh);
        if (!range)
        {
            ++build.stats.skippedMissingMesh; AppendPlaceholder(build, planned); continue;
        }
        PathTraceRigidRouteInstance instance;
        instance.vertexOffset = range->vertexOffset;
        instance.indexOffset = range->indexOffset;
        instance.triangleOffset = range->triangleOffset;
        instance.materialId = planned.materialId != 0u ? planned.materialId : range->materialId;
        instance.materialIndex = FindMaterialIndex(materialTableIds,
            instance.materialId, build.stats.missingMaterialTableIndex);
        instance.vertexCount = range->vertexCount;
        instance.indexCount = range->indexCount;
        instance.triangleCount = range->triangleCount;
        instance.instanceIdLo = static_cast<std::uint32_t>(planned.sourceInstanceId);
        instance.instanceIdHi = static_cast<std::uint32_t>(planned.sourceInstanceId >> 32);
        if (planned.hasPreviousTransform) { instance.flags |= PT_RIGID_ROUTE_HAS_PREVIOUS_TRANSFORM; ++build.stats.previousTransformInstances; }
        if (planned.transformContinuous) { instance.flags |= PT_RIGID_ROUTE_TRANSFORM_CONTINUOUS; ++build.stats.transformContinuousInstances; }
        if (!planned.sourceSeenThisFrame) instance.flags |= PT_RIGID_ROUTE_CACHED_SOURCE;
        CopyTransformRows(instance.currentObjectToWorld, planned.transform);
        CopyTransformRows(instance.previousObjectToWorld,
            planned.hasPreviousTransform ? planned.previousTransform : planned.transform);
        build.instances.push_back(instance);
        build.instanceSeenThisFrame.push_back(planned.sourceSeenThisFrame ? 1u : 0u);
        std::array<float, 16> matrix = {};
        std::copy(planned.transform, planned.transform + 16, matrix.begin());
        build.instanceObjectToWorld.push_back(matrix);
        ++build.stats.emittedInstances;
        if (std::find(emittedMeshScratch.begin(), emittedMeshScratch.end(), mesh->meshHash) ==
            emittedMeshScratch.end())
        {
            emittedMeshScratch.push_back(mesh->meshHash);
            ++build.stats.emittedUniqueMeshes;
        }
        if (planned.sourceSeenThisFrame) ++build.stats.emittedSeenThisFrame;
        else ++build.stats.emittedFromCache;
    }
    build.stats.vertices = static_cast<int>(build.vertices.size());
    build.stats.indexes = static_cast<int>(build.indexes.size());
    build.stats.triangles = static_cast<int>(build.triangleMaterials.size());
    return true;
}

std::uint64_t BuildRigidRouteGeometryUploadSignature(
    const RtPathTraceRigidRouteBuild& build)
{
    const RtSmokePlanDataSpan spans[] = {
        MakePlanSpan(build.vertices), MakePlanSpan(build.indexes),
        MakePlanSpan(build.triangleMaterials),
        MakePlanSpan(build.triangleMaterialIndexes)};
    return BuildSmokePlanDataSpanSignature(spans, 4);
}

std::uint64_t BuildRigidRouteInstanceUploadSignature(
    const RtPathTraceRigidRouteBuild& build)
{
    const RtSmokePlanDataSpan span = MakePlanSpan(build.instances);
    return BuildSmokePlanDataSpanSignature(&span, 1);
}
