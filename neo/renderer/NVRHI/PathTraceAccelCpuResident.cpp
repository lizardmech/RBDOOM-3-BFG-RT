#include "precompiled.h"
#pragma hdrstop

#include "PathTraceAccelCpuPack.h"

#include <new>
#include <stdexcept>

namespace
{
struct ResidentBuildContext
{
    const RtSmokeAccelerationPlanInput& accelerationInput;
    const RtSmokeGeometryUniverse& rigidUniverse;
    const RtSmokeRigidTlasPlan& rigidPlan;
    const RtSmokeGeometryUniverse& staticUniverse;
    std::uint64_t staticWorldGeneration = 0;
    std::uint64_t staticSourceGeneration = 0;
    int portalAreaCount = 0;
    int maxVerticesPerBucket = 0;
    int maxIndexesPerBucket = 0;
    int maxTrianglesPerBucket = 0;
};

RtSmokeAccelerationPlanInput BuildResidentAccelerationInput(
    const RtSmokeAccelerationPlanInput& input)
{
    RtSmokeAccelerationPlanInput resident = input;
    resident.staticCache = RtSmokePlanStaticCacheInput();
    resident.dynamicVertexCount = 0;
    resident.dynamicIndexCount = 0;
    return resident;
}

bool PreflightResidentPayload(
    void* opaque,
    RtPathTraceAccelCpuResidentCapacityPlan& plan)
{
    ResidentBuildContext& context =
        *static_cast<ResidentBuildContext*>(opaque);
    RtSmokeAccelerationPlanSnapshotCounts accelerationCounts;
    RtPathTraceRigidRouteResidentMeshPayloadCounts rigidCounts;
    RtPathTraceStaticBucketCpuSnapshotCounts staticCounts;
    const RtSmokeAccelerationPlanInput residentAcceleration =
        BuildResidentAccelerationInput(context.accelerationInput);
    return CountSmokeAccelerationPlanSnapshot(
            residentAcceleration, accelerationCounts) &&
        context.rigidUniverse.CountRigidRouteResidentMeshPayload(
            context.rigidPlan, true, rigidCounts) &&
        context.staticUniverse.CountStaticBucketCpuSnapshot(staticCounts) &&
        BuildPathTraceAccelCpuResidentCapacityPlan(
            accelerationCounts, rigidCounts, staticCounts, plan);
}

bool FillResidentPayload(
    void* opaque,
    const RtPathTraceAccelCpuResidentCapacityPlan& plan,
    RtPathTraceAccelCpuResidentPayload& candidate)
{
    ResidentBuildContext& context =
        *static_cast<ResidentBuildContext*>(opaque);
    const RtSmokeAccelerationPlanInput residentAcceleration =
        BuildResidentAccelerationInput(context.accelerationInput);
    if (!FillSmokeAccelerationPlanSnapshotPreReserved(
            residentAcceleration, plan.acceleration,
            candidate.acceleration) ||
        !context.rigidUniverse.FillRigidRouteResidentMeshPayloadPreReserved(
            candidate.rigidRoute, context.rigidPlan,
            true, plan.rigidRoute) ||
        !context.staticUniverse.FillStaticBucketCpuSnapshotPreReserved(
            candidate.staticBucket, plan.staticBucket,
            context.staticWorldGeneration, context.staticSourceGeneration,
            context.portalAreaCount, context.maxVerticesPerBucket,
            context.maxIndexesPerBucket, context.maxTrianglesPerBucket,
            nullptr))
        return false;

    // The backing owns only content that is stable until the payload identity
    // changes. Per-frame cache decisions, plan instances, transforms,
    // eligibility, material ordering and portal activity live in the ticket.
    candidate.acceleration.staticCache = RtSmokePlanStaticCacheInput();
    candidate.acceleration.dynamicVertexCount = 0;
    candidate.acceleration.dynamicIndexCount = 0;
    for (RtSmokeStaticBucketAssignmentSurface& surface :
         candidate.staticBucket.surfaces)
        surface.active = false;
    candidate.staticBucket.worldGeneration = 0;
    candidate.staticBucket.sourceGeneration = 0;
    candidate.staticBucket.portalAreaCount = 0;
    candidate.staticBucket.maxVerticesPerBucket = 0;
    candidate.staticBucket.maxIndexesPerBucket = 0;
    candidate.staticBucket.maxTrianglesPerBucket = 0;
    return true;
}
}

bool RtPathTraceAccelCpuResidentPublisher::Publish(
    const RtSmokeAccelerationPlanInput& accelerationInput,
    const RtSmokeGeometryUniverse& rigidUniverse,
    const RtSmokeRigidTlasPlan& rigidPlan,
    const std::vector<std::uint32_t>& materialIds,
    const RtPathTraceRigidRouteBuildSnapshot& rigidMetadata,
    std::uint64_t rigidPayloadSignature,
    const RtSmokeGeometryUniverse& staticUniverse,
    std::uint64_t staticWorldGeneration,
    std::uint64_t staticSourceGeneration,
    std::uint64_t staticPayloadSignature,
    int portalAreaCount,
    int maxVerticesPerBucket,
    int maxIndexesPerBucket,
    int maxTrianglesPerBucket,
    const std::vector<bool>* activeAreas,
    RtPathTraceAccelCpuSnapshot& ticket)
{
    (void)materialIds;
    ResidentBuildContext context{
        accelerationInput, rigidUniverse, rigidPlan,
        staticUniverse, staticWorldGeneration, staticSourceGeneration,
        portalAreaCount, maxVerticesPerBucket, maxIndexesPerBucket,
        maxTrianglesPerBucket};
    return PublishPathTraceAccelCpuResidentTicket(
        backingStore_, accelerationInput, rigidMetadata,
        rigidPayloadSignature, staticWorldGeneration,
        staticSourceGeneration, staticPayloadSignature,
        portalAreaCount, maxVerticesPerBucket,
        maxIndexesPerBucket, maxTrianglesPerBucket,
        activeAreas, PreflightResidentPayload,
        FillResidentPayload, &context, ticket);
}

void RtPathTraceAccelCpuResidentPublisher::Reset()
{
    backingStore_.Reset();
}

RtPathTraceAccelCpuResidentPublishStats
RtPathTraceAccelCpuResidentPublisher::Stats() const
{
    return backingStore_.Stats();
}
