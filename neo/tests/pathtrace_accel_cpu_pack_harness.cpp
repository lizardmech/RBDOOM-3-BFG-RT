#include "../idlib/precompiled.h"
#include "PathTraceAccelCpuPack.h"

#include <cstring>
#include <iostream>
#include <limits>
#include <stdexcept>

void* Mem_Alloc16(std::size_t size, memTag_t)
{
    return _aligned_malloc(size, 16);
}

void Mem_Free16(void* pointer)
{
    _aligned_free(pointer);
}

void* Mem_ClearedAlloc(std::size_t size, memTag_t tag)
{
    void* pointer = Mem_Alloc16(size, tag);
    if (pointer) std::memset(pointer, 0, size);
    return pointer;
}

namespace
{
int g_failures = 0;

void Check(bool condition, const char* name)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!condition) ++g_failures;
}

void SetIdentity(float matrix[16], float x = 0.0f)
{
    std::memset(matrix, 0, sizeof(float) * 16u);
    matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
    matrix[12] = x;
}

template<typename T>
void ReserveAndPush(std::vector<T>& values, const T& value)
{
    values.reserve(1);
    values.push_back(value);
}

RtPathTraceAccelCpuSnapshot MakeSnapshot()
{
    RtPathTraceAccelCpuSnapshot snapshot;
    snapshot.epoch.generation = 7;
    snapshot.epoch.frameIndex = 11;
    snapshot.epoch.mapTimeStamp = 13;
    snapshot.epoch.mapLoadSerial = 17;
    snapshot.epoch.capturedAfterBeginFrame = true;
    snapshot.epoch.capturedAfterStaticPreload = true;
    snapshot.epoch.capturedBeforeSerialMutate = false;
    RtPathTracePlanningCopyName(snapshot.epoch.mapName,
        sizeof(snapshot.epoch.mapName), "accel-pack-test");
    snapshot.compatibility.mapTimeStamp = snapshot.epoch.mapTimeStamp;
    snapshot.compatibility.mapLoadSerial = snapshot.epoch.mapLoadSerial;
    snapshot.compatibility.lifecycleEpoch = 19;
    snapshot.compatibility.configFingerprint = 23;
    RtPathTracePlanningCopyName(snapshot.compatibility.mapName,
        sizeof(snapshot.compatibility.mapName), snapshot.epoch.mapName);

    snapshot.acceleration.dynamicVertexCount = 3;
    snapshot.acceleration.dynamicIndexCount = 3;

    RtPathTraceRigidRouteMeshSnapshot mesh;
    mesh.routeRecordIndex = 0;
    mesh.meshHash = 0x101;
    mesh.gpuUploadSignature = 0x202;
    mesh.materialId = 31;
    mesh.surfaceClassId = 2;
    mesh.triangleClassAndFlags = 2;
    mesh.valid = true;
    mesh.routeReady = true;
    mesh.localBoundsValid = true;
    mesh.vertexCount = 3;
    mesh.indexCount = 3;
    mesh.localBounds[0].Set(-1.0f, -1.0f, -1.0f);
    mesh.localBounds[1].Set(1.0f, 1.0f, 1.0f);
    mesh.vertices.reserve(3);
    mesh.vertices.resize(3);
    mesh.indexes.reserve(3);
    mesh.indexes.push_back(0); mesh.indexes.push_back(1); mesh.indexes.push_back(2);
    snapshot.rigidRoute.meshes.reserve(1);
    snapshot.rigidRoute.meshes.push_back(std::move(mesh));
    ReserveAndPush(snapshot.rigidRoute.materialTableIds, std::uint32_t(31));

    RtSmokePlanTlasInstance instance;
    instance.kind = RT_SMOKE_PLAN_TLAS_RIGID_BLAS;
    instance.instanceId = 5;
    instance.meshHash = 0x101;
    instance.sourceInstanceId = 0x303;
    instance.materialId = 31;
    instance.routeRecordIndex = 0;
    instance.sourceSeenThisFrame = true;
    SetIdentity(instance.transform);
    SetIdentity(instance.previousTransform);
    ReserveAndPush(snapshot.rigidRoute.plan.instances, instance);
    snapshot.rigidRoute.plan.visibleInstances = 1;
    snapshot.rigidRoute.plan.rigidInstances = 1;
    snapshot.rigidRoute.plan.emittedInstances = 1;
    snapshot.rigidRoute.plan.tlasInstanceSignature = 0x505;
    ReserveAndPush(snapshot.rigidRoute.instanceEligibility,
        BuildRigidRouteInstanceEligibilityFromPod(
            snapshot.rigidRoute.plan.instances[0], snapshot.rigidRoute.meshes[0]));

    RtSmokeStaticBucketAssignmentSurface surface;
    surface.surfaceKey = 0x404;
    surface.sourceRecordIndex = 0;
    surface.portalArea = 0;
    surface.range.vertexOffset = 0;
    surface.range.vertexCount = 3;
    surface.range.indexOffset = 0;
    surface.range.indexCount = 3;
    surface.range.triangleOffset = 0;
    surface.range.triangleCount = 1;
    surface.valid = true;
    surface.active = true;
    ReserveAndPush(snapshot.staticBucket.surfaces, surface);
    snapshot.staticBucket.vertices.reserve(3);
    snapshot.staticBucket.vertices.resize(3);
    snapshot.staticBucket.indexes.reserve(3);
    snapshot.staticBucket.indexes.push_back(0);
    snapshot.staticBucket.indexes.push_back(1);
    snapshot.staticBucket.indexes.push_back(2);
    ReserveAndPush(snapshot.staticBucket.triangleClasses, std::uint32_t(1));
    ReserveAndPush(snapshot.staticBucket.triangleMaterials, std::uint32_t(41));
    snapshot.staticBucket.worldGeneration = 29;
    snapshot.staticBucket.sourceGeneration = 31;
    snapshot.staticBucket.storageGeneration = 37;
    snapshot.staticBucket.portalAreaCount = 1;
    snapshot.staticBucket.maxVerticesPerBucket = 64;
    snapshot.staticBucket.maxIndexesPerBucket = 64;
    snapshot.staticBucket.maxTrianglesPerBucket = 64;
    snapshot.staticBucket.complete = true;

    RtPathTraceAccelCpuCapacityCounts counts;
    RtPathTraceAccelCpuCapacityPlan plan;
    Check(CountPathTraceAccelCpuCapacityFromSnapshot(snapshot, counts),
        "snapshot concrete counts");
    Check(BuildPathTraceAccelCpuCapacityPlan(counts, plan),
        "snapshot concrete plan");
    snapshot.reservedProductBytes = plan.productBytes + plan.scratchBytes;
    snapshot.plannedPeakBytes = plan.plannedPeakBytes;
    snapshot.complete = true;
    snapshot.inputReceipt = BuildPathTraceAccelCpuInputReceipt(snapshot);
    return snapshot;
}

void TestRealThreeComponentProduct()
{
    RtPathTraceAccelCpuSnapshot snapshot = MakeSnapshot();
    Check(ValidatePathTraceAccelCpuSnapshot(snapshot), "real snapshot validates");
    RtPathTraceAccelCpuProduct product;
    std::size_t peak = 0;
    Check(BuildPathTraceAccelCpuProduct(snapshot, product, &peak),
        "real three-component build");
    Check(product.complete && !product.rigidBuild.vertices.empty() &&
        product.accelerationPlan.hasDynamicBlas &&
        !product.staticAssignment.assignments.empty() && product.staticPack.exact,
        "all three components complete atomically");
    Check(peak <= RT_PT_ACCEL_CPU_SLOT_MAX_BYTES &&
        snapshot.OwnedBytes() + product.OwnedBytes() <= peak,
        "peak charges retained snapshot and product");
    const RtPathTraceAccelCpuOracle oracle = BuildPathTraceAccelCpuOracle(product);
    Check(ComparePathTraceAccelCpuProduct(product, oracle).exact,
        "real product oracle exact");
    RtPathTraceAccelCpuOracle changed = oracle;
    ++changed.staticPackSignature;
    Check(!ComparePathTraceAccelCpuProduct(product, changed).exact,
        "real compare detects component mutation");
}

RtPathTraceAccelCpuSnapshot MakeResidentSnapshot()
{
    RtPathTraceAccelCpuSnapshot source = MakeSnapshot();
    auto payload = std::make_shared<RtPathTraceAccelCpuResidentPayload>();
    payload->acceleration = std::move(source.acceleration);
    payload->rigidRoute.meshes = std::move(source.rigidRoute.meshes);
    payload->staticBucket = std::move(source.staticBucket);
    payload->generation = 41;
    payload->rigidPayloadSignature = 43;
    payload->staticPayloadSignature = 47;
    payload->complete = true;

    RtPathTraceAccelCpuSnapshot ticket;
    ticket.epoch = source.epoch;
    ticket.compatibility = source.compatibility;
    ticket.acceleration.staticCache = payload->acceleration.staticCache;
    ticket.acceleration.staticVertexCount =
        payload->acceleration.staticVertexCount;
    ticket.acceleration.staticIndexCount =
        payload->acceleration.staticIndexCount;
    ticket.acceleration.dynamicVertexCount =
        payload->acceleration.dynamicVertexCount;
    ticket.acceleration.dynamicIndexCount =
        payload->acceleration.dynamicIndexCount;
    ticket.rigidRoute.plan = std::move(source.rigidRoute.plan);
    ticket.rigidRoute.materialTableIds =
        std::move(source.rigidRoute.materialTableIds);
    ticket.rigidRoute.instanceEligibility =
        std::move(source.rigidRoute.instanceEligibility);
    ticket.rigidRoute.meshes.reserve(payload->rigidRoute.meshes.size());
    for (const RtPathTraceRigidRouteMeshSnapshot& residentMesh :
         payload->rigidRoute.meshes)
    {
        RtPathTraceRigidRouteMeshSnapshot metadata = residentMesh;
        std::vector<PathTraceSmokeVertex>().swap(metadata.vertices);
        std::vector<std::uint32_t>().swap(metadata.indexes);
        ticket.rigidRoute.meshes.push_back(std::move(metadata));
    }
    ticket.staticBucket.worldGeneration = payload->staticBucket.worldGeneration;
    ticket.staticBucket.sourceGeneration = payload->staticBucket.sourceGeneration;
    ticket.staticBucket.storageGeneration = payload->staticBucket.storageGeneration;
    ticket.staticBucket.portalAreaCount = payload->staticBucket.portalAreaCount;
    ticket.staticBucket.maxVerticesPerBucket =
        payload->staticBucket.maxVerticesPerBucket;
    ticket.staticBucket.maxIndexesPerBucket =
        payload->staticBucket.maxIndexesPerBucket;
    ticket.staticBucket.maxTrianglesPerBucket =
        payload->staticBucket.maxTrianglesPerBucket;
    ticket.staticBucket.complete = true;
    ReserveAndPush(ticket.activePortalAreas, std::uint8_t(1));
    ticket.residentPayload = payload;
    ticket.residentGeneration = payload->generation;
    ticket.rigidPayloadSignature = payload->rigidPayloadSignature;
    ticket.staticPayloadSignature = payload->staticPayloadSignature;
    ticket.residentBacked = true;
    RtPathTraceAccelCpuCapacityCounts counts;
    RtPathTraceAccelCpuCapacityPlan plan;
    Check(CountPathTraceAccelCpuCapacityFromSnapshot(ticket, counts),
        "resident ticket concrete counts");
    Check(BuildPathTraceAccelCpuCapacityPlan(counts, plan),
        "resident ticket concrete plan");
    ticket.reservedProductBytes = plan.productBytes + plan.scratchBytes;
    ticket.plannedPeakBytes = plan.plannedPeakBytes;
    ticket.complete = true;
    ticket.inputReceipt = BuildPathTraceAccelCpuInputReceipt(ticket);
    return ticket;
}

void TestResidentTicketBuildAndReceipt()
{
    RtPathTraceAccelCpuSnapshot ticket = MakeResidentSnapshot();
    Check(ValidatePathTraceAccelCpuSnapshot(ticket) &&
        ticket.OwnedBytes() < ticket.residentPayload->OwnedBytes() &&
        ticket.residentPayload->rigidRoute.plan.instances.empty() &&
        ticket.residentPayload->rigidRoute.plan.planTruncatedInstanceIds.empty() &&
        ticket.residentPayload->rigidRoute.materialTableIds.empty() &&
        ticket.residentPayload->rigidRoute.instanceEligibility.empty(),
        "resident Lane B ticket validates without owning world payload bytes");
    RtPathTraceAccelCpuProduct product;
    Check(BuildPathTraceAccelCpuProduct(ticket, product, nullptr) &&
        product.complete && !product.rigidBuild.vertices.empty() &&
        !product.staticPack.vertexBytes.empty(),
        "resident Lane B worker builds all three CPU products from stable spans");
    RtPathTraceAccelCpuSnapshot sameIdentity = MakeResidentSnapshot();
    ++sameIdentity.acceleration.dynamicVertexCount;
    sameIdentity.inputReceipt = BuildPathTraceAccelCpuInputReceipt(sameIdentity);
    Check(sameIdentity.rigidPayloadSignature == ticket.rigidPayloadSignature &&
        sameIdentity.staticPayloadSignature == ticket.staticPayloadSignature &&
        sameIdentity.inputReceipt != ticket.inputReceipt,
        "dynamic ticket counts change receipt without changing resident backing identity");

    RtPathTraceAccelCpuSnapshot changedTicket = MakeResidentSnapshot();
    const RtPathTraceAccelCpuResidentPayload* residentIdentity =
        changedTicket.residentPayload.get();
    changedTicket.rigidRoute.materialTableIds.insert(
        changedTicket.rigidRoute.materialTableIds.begin(), 999u);
    RtPathTraceAccelCpuCapacityCounts changedCounts;
    RtPathTraceAccelCpuCapacityPlan changedPlan;
    Check(CountPathTraceAccelCpuCapacityFromSnapshot(
              changedTicket, changedCounts) &&
        BuildPathTraceAccelCpuCapacityPlan(changedCounts, changedPlan),
        "current ticket material ordering replans without resident publication");
    changedTicket.reservedProductBytes =
        changedPlan.productBytes + changedPlan.scratchBytes;
    changedTicket.plannedPeakBytes = changedPlan.plannedPeakBytes;
    changedTicket.inputReceipt =
        BuildPathTraceAccelCpuInputReceipt(changedTicket);
    RtPathTraceAccelCpuProduct changedProduct;
    Check(changedTicket.residentPayload.get() == residentIdentity &&
        changedTicket.inputReceipt != ticket.inputReceipt &&
        BuildPathTraceAccelCpuProduct(changedTicket, changedProduct, nullptr) &&
        !changedProduct.rigidBuild.geometryRanges.empty() &&
        !changedProduct.rigidBuild.instances.empty() &&
        changedProduct.rigidBuild.geometryRanges[0].materialIndex == 1 &&
        changedProduct.rigidBuild.instances[0].materialIndex == 1,
        "worker uses current ticket material rows without republishing resident mesh backing");

    sameIdentity = MakeResidentSnapshot();
    auto changedPayload = std::make_shared<RtPathTraceAccelCpuResidentPayload>(
        *sameIdentity.residentPayload);
    changedPayload->rigidRoute.meshes[0].vertices[0].position[0] += 1.0f;
    ++changedPayload->generation;
    ++changedPayload->rigidPayloadSignature;
    sameIdentity.residentPayload = changedPayload;
    sameIdentity.residentGeneration = changedPayload->generation;
    sameIdentity.rigidPayloadSignature = changedPayload->rigidPayloadSignature;
    sameIdentity.inputReceipt = BuildPathTraceAccelCpuInputReceipt(sameIdentity);
    Check(sameIdentity.inputReceipt != ticket.inputReceipt,
        "resident heavy-byte mutation is represented by published generation/signature authority");
}

void FillResidentPublisherHeader(
    RtPathTraceAccelCpuSnapshot& ticket,
    bool valid)
{
    ticket.epoch.generation = valid ? 7 : 0;
    ticket.epoch.frameIndex = 11;
    ticket.epoch.mapTimeStamp = 13;
    ticket.epoch.mapLoadSerial = 17;
    ticket.epoch.capturedAfterBeginFrame = true;
    ticket.epoch.capturedAfterStaticPreload = true;
    ticket.epoch.capturedBeforeSerialMutate = false;
    RtPathTracePlanningCopyName(ticket.epoch.mapName,
        sizeof(ticket.epoch.mapName), "resident-publisher-test");
    ticket.compatibility.mapTimeStamp = ticket.epoch.mapTimeStamp;
    ticket.compatibility.mapLoadSerial = ticket.epoch.mapLoadSerial;
    ticket.compatibility.lifecycleEpoch = 19;
    ticket.compatibility.configFingerprint = 23;
    RtPathTracePlanningCopyName(ticket.compatibility.mapName,
        sizeof(ticket.compatibility.mapName), ticket.epoch.mapName);
}

bool PreflightResidentPublisherHeader(
    void*, RtPathTraceAccelCpuResidentCapacityPlan& plan)
{
    RtSmokeAccelerationPlanSnapshotCounts acceleration;
    RtPathTraceRigidRouteResidentMeshPayloadCounts rigid;
    RtPathTraceStaticBucketCpuSnapshotCounts staticBucket;
    return BuildPathTraceAccelCpuResidentCapacityPlan(
        acceleration, rigid, staticBucket, plan);
}

bool FillResidentPublisherHeader(
    void*,
    const RtPathTraceAccelCpuResidentCapacityPlan&,
    RtPathTraceAccelCpuResidentPayload& payload)
{
    payload.staticBucket.complete = true;
    return true;
}

void TestProductionResidentPublisherPreservesHeader()
{
    RtPathTraceAccelCpuResidentBackingStore backingStore;
    RtSmokeAccelerationPlanInput accelerationInput;
    RtPathTraceRigidRouteBuildSnapshot rigidMetadata;
    RtPathTraceAccelCpuSnapshot ticket;
    FillResidentPublisherHeader(ticket, true);
    Check(PublishPathTraceAccelCpuResidentTicket(
            backingStore, accelerationInput, rigidMetadata,
            31, 37, 41, 43, 0, 0, 0, 0, nullptr,
            PreflightResidentPublisherHeader,
            FillResidentPublisherHeader, nullptr, ticket) &&
        ticket.epoch.generation == 7 &&
        ticket.compatibility.lifecycleEpoch == 19 &&
        ticket.complete && ticket.inputReceipt != 0 &&
        ValidatePathTraceAccelCpuSnapshot(ticket),
        "real resident publisher preserves caller epoch/compatibility and publishes valid ticket");

    const RtPathTraceAccelCpuResidentPublishStats before = backingStore.Stats();
    RtPathTraceAccelCpuSnapshot invalidTicket;
    FillResidentPublisherHeader(invalidTicket, false);
    Check(!PublishPathTraceAccelCpuResidentTicket(
            backingStore, accelerationInput, rigidMetadata,
            31, 37, 41, 43, 0, 0, 0, 0, nullptr,
            PreflightResidentPublisherHeader,
            FillResidentPublisherHeader, nullptr, invalidTicket),
        "invalid resident publisher header fails closed");
    const RtPathTraceAccelCpuResidentPublishStats after = backingStore.Stats();
    Check(after.publishes == before.publishes &&
        after.reuses == before.reuses &&
        after.ticketValidationFailures ==
            before.ticketValidationFailures + 1,
        "invalid header fails before backing publish/reuse credit");

    for (int failureKind = 0; failureKind < 2; ++failureKind)
    {
        RtPathTraceAccelCpuResidentBackingStore failingStore;
        RtPathTraceAccelCpuSnapshot failingTicket;
        FillResidentPublisherHeader(failingTicket, true);
        // Ordinal zero allocates/publishes the backing; ordinal one is the
        // ticket-build boundary immediately afterward.
        PathTraceAccelCpuPackSetAllocationFailureForTest(
            1, failureKind != 0);
        Check(!PublishPathTraceAccelCpuResidentTicket(
                failingStore, accelerationInput, rigidMetadata,
                51 + failureKind, 57, 59, 61, 0, 0, 0, 0, nullptr,
                PreflightResidentPublisherHeader,
                FillResidentPublisherHeader, nullptr, failingTicket),
            failureKind == 0
                ? "post-backing ticket bad_alloc fails closed"
                : "post-backing ticket length_error fails closed");
        PathTraceAccelCpuPackSetAllocationFailureForTest(-1, false);
        const RtPathTraceAccelCpuResidentPublishStats failedStats =
            failingStore.Stats();
        Check(failedStats.publishes == 1 &&
            failedStats.ticketBuildFailures == 1 &&
            failedStats.ticketValidationFailures == 0 &&
            !failingTicket.complete && !failingTicket.residentPayload,
            failureKind == 0
                ? "post-backing ticket bad_alloc is visible and releases ticket"
                : "post-backing ticket length_error is visible and releases ticket");
    }
}

void TestPreflightAndCapacityStamp()
{
    RtPathTraceAccelCpuCapacityCounts counts;
    RtPathTraceAccelCpuCapacityPlan plan;
    counts.values[RT_PT_ACCEL_FAMILY_ACCEL_VERTEX_BYTES] = 41887416u;
    Check(BuildPathTraceAccelCpuCapacityPlan(counts, plan) && plan.valid &&
        plan.plannedPeakBytes == 41887416u,
        "measured 41,887,416-byte Lane B slot fits 64 MiB authority");

    counts = RtPathTraceAccelCpuCapacityCounts();
    counts.values[RT_PT_ACCEL_FAMILY_ACCEL_VERTEX_BYTES] =
        RT_PT_ACCEL_CPU_SLOT_MAX_BYTES;
    Check(BuildPathTraceAccelCpuCapacityPlan(counts, plan) && plan.valid &&
        plan.plannedPeakBytes == RT_PT_ACCEL_CPU_SLOT_MAX_BYTES,
        "64 MiB Lane B boundary succeeds exactly");
    ++counts.values[RT_PT_ACCEL_FAMILY_ACCEL_VERTEX_BYTES];
    Check(!BuildPathTraceAccelCpuCapacityPlan(counts, plan) &&
        !plan.valid &&
        plan.attemptedPeakBytes == RT_PT_ACCEL_CPU_SLOT_MAX_BYTES + 1u,
        "64 MiB plus one rejects atomically before allocation");

    RtPathTraceAccelCpuSnapshot snapshot = MakeSnapshot();
    RtPathTraceAccelCpuCapacityCounts exactCounts;
    RtPathTraceAccelCpuCapacityPlan exactPlan;
    Check(CountPathTraceAccelCpuCapacityFromSnapshot(snapshot, exactCounts) &&
        BuildPathTraceAccelCpuCapacityPlan(exactCounts, exactPlan),
        "exact capacity plan rebuilt");
    snapshot.staticBucket.vertices.reserve(
        snapshot.staticBucket.vertices.capacity() + 1u);
    RtPathTraceAccelCpuCapacityStamp stamp;
    Check(StampPathTraceAccelCpuCapacities(snapshot, nullptr, stamp) &&
        !PathTraceAccelCpuCapacityStampWithinPlan(stamp, exactPlan),
        "actual capacity inflation rejected");
}

void TestRigidMaterialRemapIncludesInstances()
{
    constexpr std::uint32_t materialA = 101;
    constexpr std::uint32_t materialB = 102;
    constexpr std::uint32_t materialC = 103;
    constexpr std::uint32_t materialX = 100;
    RtPathTraceRigidRouteBuild build;
    RtPathTraceRigidRouteGeometryRange range;
    range.materialId = materialA;
    range.materialIndex = 0;
    range.triangleOffset = 0;
    range.triangleCount = 1;
    build.geometryRanges.push_back(range);
    build.triangleMaterialIndexes.push_back(0);
    PathTraceRigidRouteInstance instance;
    instance.materialId = materialA;
    instance.materialIndex = 0;
    build.instances.push_back(instance);

    bool geometryChanged = false;
    bool instanceChanged = false;
    const std::vector<std::uint32_t> liveIds{
        materialX, materialA, materialB, materialC};
    Check(RemapRigidRouteMaterialIndexes(build, liveIds,
            &geometryChanged, &instanceChanged) &&
        geometryChanged && instanceChanged &&
        build.geometryRanges[0].materialIndex == 1 &&
        build.triangleMaterialIndexes[0] == 1 &&
        build.instances[0].materialIndex == 1 &&
        build.stats.missingMaterialTableIndex == 0,
        "rigid material remap updates geometry triangles and instance ABI rows");

    build.geometryRanges[0].materialId = 9001;
    build.geometryRanges[0].materialIndex = 1;
    build.instances[0].materialId = 9002;
    build.instances[0].materialIndex = 1;
    Check(RemapRigidRouteMaterialIndexes(build, liveIds,
            &geometryChanged, &instanceChanged) &&
        geometryChanged && instanceChanged &&
        build.geometryRanges[0].materialIndex == 0 &&
        build.triangleMaterialIndexes[0] == 0 &&
        build.instances[0].materialIndex == 0 &&
        build.stats.missingMaterialTableIndex == 2,
        "rigid material remap counts missing geometry and instance material rows");
}

void TestAllocationFailureCleanup()
{
    for (int kind = 0; kind < 2; ++kind)
    {
        RtPathTraceAccelCpuSnapshot snapshot = MakeSnapshot();
        RtPathTraceAccelCpuProduct product;
        PathTraceAccelCpuPackSetAllocationFailureForTest(0, kind != 0);
        Check(!BuildPathTraceAccelCpuProduct(snapshot, product, nullptr),
            kind == 0 ? "bad_alloc fails closed" : "length_error fails closed");
        Check(!product.complete && product.OwnedBytes() == 0,
            kind == 0 ? "bad_alloc releases product" : "length_error releases product");
        PathTraceAccelCpuPackSetAllocationFailureForTest(-1, false);
    }
}

void TestSnapshotReserveFailureCleanup()
{
    PathTraceSmokeVertex vertices[3] = {};
    std::uint32_t indexes[3] = {0, 1, 2};
    std::uint32_t classes[1] = {1};
    std::uint32_t materials[1] = {2};
    RtSmokeAccelerationPlanInput input;
    input.staticSignature.vertices = vertices;
    input.staticSignature.vertexStride = sizeof(vertices[0]);
    input.staticSignature.totalVertexCount = 3;
    input.staticSignature.indexes = indexes;
    input.staticSignature.totalIndexCount = 3;
    input.staticSignature.triangleClasses = classes;
    input.staticSignature.triangleMaterials = materials;
    input.staticSignature.totalTriangleCount = 1;
    input.staticSignature.staticRange.vertexCount = 3;
    input.staticSignature.staticRange.indexCount = 3;
    input.staticSignature.staticRange.triangleCount = 1;
    RtSmokeAccelerationPlanSnapshotCounts counts;
    Check(CountSmokeAccelerationPlanSnapshot(input, counts),
        "real acceleration snapshot count");
    for (int kind = 0; kind < 2; ++kind)
    {
        RtSmokeAccelerationPlanSnapshot captured;
        PathTraceAccelCpuPackSetAllocationFailureForTest(0, kind != 0);
        Check(!FillSmokeAccelerationPlanSnapshotPreReserved(
                input, counts, captured) &&
            captured.staticSignature.vertexBytes.capacity() == 0 &&
            captured.staticSignature.indexes.capacity() == 0,
            kind == 0 ? "snapshot bad_alloc releases backing" :
                "snapshot length_error releases backing");
        PathTraceAccelCpuPackSetAllocationFailureForTest(-1, false);
    }
}

void TestStaticDisabledProduct()
{
    RtPathTraceAccelCpuSnapshot snapshot = MakeSnapshot();
    snapshot.staticBucket.ResetAndRelease();
    snapshot.staticBucket.complete = true;
    RtPathTraceAccelCpuCapacityCounts counts;
    RtPathTraceAccelCpuCapacityPlan plan;
    Check(CountPathTraceAccelCpuCapacityFromSnapshot(snapshot, counts) &&
        BuildPathTraceAccelCpuCapacityPlan(counts, plan),
        "static-disabled concrete plan");
    snapshot.reservedProductBytes = plan.productBytes + plan.scratchBytes;
    snapshot.plannedPeakBytes = plan.plannedPeakBytes;
    snapshot.inputReceipt = BuildPathTraceAccelCpuInputReceipt(snapshot);
    snapshot.complete = true;
    RtPathTraceAccelCpuProduct product;
    Check(BuildPathTraceAccelCpuProduct(snapshot, product, nullptr) &&
        product.complete && product.staticAssignment.exactCoverage &&
        product.staticPack.exact && product.staticPack.buckets.empty(),
        "static-disabled product completes with exact empty static component");
}

void TestReceiptDeterminismAndEligibility()
{
    RtPathTraceAccelCpuSnapshot lhs = MakeSnapshot();
    RtPathTraceAccelCpuSnapshot rhs = MakeSnapshot();
    RtSmokeStaticBucketAssignmentSurface paddedA;
    RtSmokeStaticBucketAssignmentSurface paddedB;
    std::memset(&paddedA, 0x11, sizeof(paddedA));
    std::memset(&paddedB, 0x77, sizeof(paddedB));
    paddedA = lhs.staticBucket.surfaces[0];
    paddedB = rhs.staticBucket.surfaces[0];
    std::memcpy(&lhs.staticBucket.surfaces[0], &paddedA, sizeof(paddedA));
    std::memcpy(&rhs.staticBucket.surfaces[0], &paddedB, sizeof(paddedB));
    Check(BuildPathTraceAccelCpuInputReceipt(lhs) ==
        BuildPathTraceAccelCpuInputReceipt(rhs),
        "receipt ignores struct padding");

    const std::uint64_t base = BuildPathTraceAccelCpuInputReceipt(lhs);
    rhs = MakeSnapshot();
    rhs.rigidRoute.meshes[0].localBoundsValid = false;
    Check(BuildPathTraceAccelCpuInputReceipt(rhs) != base,
        "local bounds validity changes receipt");
    rhs = MakeSnapshot();
    rhs.rigidRoute.meshes[0].localBounds[0].x -= 1.0f;
    Check(BuildPathTraceAccelCpuInputReceipt(rhs) != base,
        "local bounds fields change receipt");
    rhs = MakeSnapshot();
    rhs.rigidRoute.instanceEligibility[0].transformUsable = false;
    Check(BuildPathTraceAccelCpuInputReceipt(rhs) != base,
        "transform eligibility changes receipt");
    rhs = MakeSnapshot();
    rhs.rigidRoute.instanceEligibility[0].worldBoundsValid = false;
    Check(BuildPathTraceAccelCpuInputReceipt(rhs) != base,
        "world bounds eligibility changes receipt");
    rhs = MakeSnapshot();
    rhs.rigidRoute.plan.instances[0].transform[12] = 123.0f;
    ++rhs.rigidRoute.plan.tlasInstanceSignature;
    Check(BuildPathTraceAccelCpuInputReceipt(rhs) == base,
        "matrix-only movement preserves receipt");
}

struct ResidentBackingFixture
{
    std::size_t vertexCount = 3;
    std::uint64_t surfaceKey = 101;
    int portalArea = 2;
    int preflightCalls = 0;
    int fillCalls = 0;
    bool failFill = false;
    bool throwLengthError = false;
    std::size_t rigidMeshCount = 0;
    std::size_t rigidMeshVertexCount = 0;
    std::size_t rigidMeshIndexCount = 0;
};

bool PreflightResidentBackingFixture(
    void* opaque,
    RtPathTraceAccelCpuResidentCapacityPlan& plan)
{
    ResidentBackingFixture& fixture =
        *static_cast<ResidentBackingFixture*>(opaque);
    ++fixture.preflightCalls;
    RtSmokeAccelerationPlanSnapshotCounts acceleration;
    RtPathTraceRigidRouteResidentMeshPayloadCounts rigid;
    rigid.meshes = fixture.rigidMeshCount;
    rigid.meshVertices = fixture.rigidMeshVertexCount;
    rigid.meshIndexes = fixture.rigidMeshIndexCount;
    RtPathTraceStaticBucketCpuSnapshotCounts staticBucket;
    staticBucket.surfaces = 1;
    staticBucket.vertices = fixture.vertexCount;
    return BuildPathTraceAccelCpuResidentCapacityPlan(
        acceleration, rigid, staticBucket, plan);
}

bool FillResidentBackingFixture(
    void* opaque,
    const RtPathTraceAccelCpuResidentCapacityPlan&,
    RtPathTraceAccelCpuResidentPayload& payload)
{
    ResidentBackingFixture& fixture =
        *static_cast<ResidentBackingFixture*>(opaque);
    ++fixture.fillCalls;
    if (fixture.throwLengthError)
        throw std::length_error("resident fill seam");
    if (fixture.failFill)
        return false;
    payload.staticBucket.vertices.reserve(fixture.vertexCount);
    payload.staticBucket.vertices.resize(fixture.vertexCount);
    payload.staticBucket.surfaces.reserve(1);
    RtSmokeStaticBucketAssignmentSurface surface;
    surface.surfaceKey = fixture.surfaceKey;
    surface.portalArea = fixture.portalArea;
    surface.valid = true;
    payload.staticBucket.surfaces.push_back(surface);
    payload.staticBucket.complete = true;
    return true;
}

void TestProductionResidentBackingStore()
{
    constexpr std::size_t hugeTicketRows = 2'000'000u;
    const std::size_t excludedTicketBytes = hugeTicketRows *
        (sizeof(RtSmokePlanTlasInstance) + sizeof(std::uint32_t) * 2u +
            sizeof(RtPathTraceRigidRouteInstanceEligibility));
    ResidentBackingFixture meshOnly;
    meshOnly.rigidMeshCount = 1;
    meshOnly.rigidMeshVertexCount = 3;
    meshOnly.rigidMeshIndexCount = 3;
    RtPathTraceAccelCpuResidentCapacityPlan meshOnlyPlan;
    Check(PreflightResidentBackingFixture(&meshOnly, meshOnlyPlan) &&
        excludedTicketBytes > RT_PT_ACCEL_CPU_RESIDENT_BACKING_MAX_BYTES &&
        meshOnlyPlan.plannedBytes < RT_PT_ACCEL_CPU_RESIDENT_BACKING_MAX_BYTES &&
        meshOnlyPlan.rigidRoute.meshes == 1 &&
        meshOnlyPlan.rigidRoute.meshVertices == 3 &&
        meshOnlyPlan.rigidRoute.meshIndexes == 3,
        "huge ticket-only plan/material/eligibility rows are excluded from resident preflight");
    RtPathTraceAccelCpuResidentBackingStore meshOnlyStore;
    std::shared_ptr<const RtPathTraceAccelCpuResidentPayload> meshOnlyLease;
    Check(meshOnlyStore.Publish(7, 9, PreflightResidentBackingFixture,
            FillResidentBackingFixture, &meshOnly, meshOnlyLease) &&
        meshOnlyLease && meshOnlyLease->OwnedBytes() <= meshOnlyPlan.plannedBytes &&
        meshOnlyStore.Stats().residentCapRejects == 0,
        "small resident mesh backing publishes despite over-cap hypothetical ticket families");

    RtPathTraceAccelCpuResidentBackingStore store;
    ResidentBackingFixture fixture;
    std::shared_ptr<const RtPathTraceAccelCpuResidentPayload> first;
    Check(store.Publish(11, 21, PreflightResidentBackingFixture,
            FillResidentBackingFixture, &fixture, first) && first &&
        first->complete && fixture.preflightCalls == 1 && fixture.fillCalls == 1,
        "resident publisher initial publish uses real preflight/fill transaction");
    const RtPathTraceAccelCpuResidentPayload* firstIdentity = first.get();

    // Per-frame ticket changes do not enter either heavy identity.
    std::shared_ptr<const RtPathTraceAccelCpuResidentPayload> reused;
    Check(store.Publish(11, 21, PreflightResidentBackingFixture,
            FillResidentBackingFixture, &fixture, reused) &&
        reused.get() == firstIdentity && fixture.preflightCalls == 1 &&
        fixture.fillCalls == 1 && store.Stats().publishes == 1 &&
        store.Stats().reuses == 1,
        "resident publisher stable ticket churn reuses backing without Count or Fill");
    reused.reset();

    std::shared_ptr<const RtPathTraceAccelCpuResidentPayload> second;
    Check(store.Publish(12, 21, PreflightResidentBackingFixture,
            FillResidentBackingFixture, &fixture, second) && second &&
        second.get() != firstIdentity && store.Stats().cowPublishes == 1 &&
        store.Stats().residentBytes > 0 && store.Stats().cowBytes > 0 &&
        store.Stats().highWaterBytes <=
            RT_PT_ACCEL_CPU_RESIDENT_TOTAL_MAX_BYTES,
        "leased current mutation publishes one bounded retired-plus-current COW");
    const int preflightBeforeReject = fixture.preflightCalls;
    std::shared_ptr<const RtPathTraceAccelCpuResidentPayload> rejected;
    Check(!store.Publish(13, 21, PreflightResidentBackingFixture,
            FillResidentBackingFixture, &fixture, rejected) && !rejected &&
        fixture.preflightCalls == preflightBeforeReject &&
        store.Stats().leaseRejects == 1,
        "second mutation while retired is leased rejects before Count or allocation");

    first.reset();
    Check(store.Publish(13, 21, PreflightResidentBackingFixture,
            FillResidentBackingFixture, &fixture, rejected) && rejected &&
        store.Stats().cowPublishes == 2,
        "mutation succeeds after retired lease release");
    const std::uint64_t retainedGeneration = rejected->generation;
    store.Reset();
    Check(rejected->complete && rejected->generation == retainedGeneration &&
        store.Stats().residentBytes == 0 && store.Stats().cowBytes == 0,
        "reset invalidates store while outstanding shared lease remains stable");

    RtPathTraceAccelCpuResidentBackingStore capStore;
    ResidentBackingFixture overCap;
    overCap.vertexCount =
        RT_PT_ACCEL_CPU_RESIDENT_BACKING_MAX_BYTES /
            sizeof(PathTraceSmokeVertex) + 1;
    Check(!capStore.Publish(1, 1, PreflightResidentBackingFixture,
            FillResidentBackingFixture, &overCap, rejected) &&
        overCap.fillCalls == 0 && capStore.Stats().residentCapRejects == 1 &&
        capStore.Stats().residentBytes == 0 &&
        capStore.Stats().attemptedResidentBytes >
            RT_PT_ACCEL_CPU_RESIDENT_BACKING_MAX_BYTES,
        "resident 64 MiB plus one rejects before allocation and retains no backing");

    RtPathTraceAccelCpuResidentBackingStore allocationStore;
    ResidentBackingFixture allocationFixture;
    PathTraceAccelCpuPackSetAllocationFailureForTest(0, false);
    Check(!allocationStore.Publish(1, 1, PreflightResidentBackingFixture,
            FillResidentBackingFixture, &allocationFixture, rejected) &&
        allocationStore.Stats().allocationFailures == 1 &&
        allocationStore.Stats().residentBytes == 0,
        "resident candidate bad_alloc fails closed before publish");
    PathTraceAccelCpuPackSetAllocationFailureForTest(-1, false);

    RtPathTraceAccelCpuResidentBackingStore fillStore;
    ResidentBackingFixture fillFixture;
    fillFixture.throwLengthError = true;
    Check(!fillStore.Publish(1, 1, PreflightResidentBackingFixture,
            FillResidentBackingFixture, &fillFixture, rejected) &&
        fillStore.Stats().allocationFailures == 1 &&
        fillStore.Stats().residentBytes == 0,
        "resident fill length_error releases unpublished candidate");

    std::uint64_t staticGeneration = 1;
    RtPathTraceAccelCpuResidentBackingStore mutationStore;
    ResidentBackingFixture mutationFixture;
    std::shared_ptr<const RtPathTraceAccelCpuResidentPayload> mutationLease;
    Check(mutationStore.Publish(1, staticGeneration,
            PreflightResidentBackingFixture, FillResidentBackingFixture,
            &mutationFixture, mutationLease),
        "resident mutation fixture initial publish");
    const RtPathTraceAccelCpuResidentPayload* initialMutationBacking =
        mutationLease.get();
    mutationLease.reset();
    mutationFixture.portalArea = 7;
    AdvancePathTraceResidentPayloadGeneration(staticGeneration);
    Check(mutationStore.Publish(1, staticGeneration,
            PreflightResidentBackingFixture, FillResidentBackingFixture,
            &mutationFixture, mutationLease) &&
        mutationLease.get() != initialMutationBacking &&
        mutationLease->staticBucket.surfaces[0].portalArea == 7,
        "portal-area mutation causes exactly one new backing and changed worker input");
    const RtPathTraceAccelCpuResidentPayload* portalBacking = mutationLease.get();
    const int fillsAfterPortal = mutationFixture.fillCalls;
    mutationLease.reset();
    Check(mutationStore.Publish(1, staticGeneration,
            PreflightResidentBackingFixture, FillResidentBackingFixture,
            &mutationFixture, mutationLease) &&
        mutationLease.get() == portalBacking &&
        mutationFixture.fillCalls == fillsAfterPortal,
        "stable frame after portal mutation reuses without Fill");
    mutationLease.reset();
    mutationFixture.surfaceKey = 202;
    AdvancePathTraceResidentPayloadGeneration(staticGeneration);
    Check(mutationStore.Publish(1, staticGeneration,
            PreflightResidentBackingFixture, FillResidentBackingFixture,
            &mutationFixture, mutationLease) &&
        mutationLease.get() != portalBacking &&
        mutationLease->staticBucket.surfaces[0].surfaceKey == 202 &&
        staticGeneration == 3,
        "bucket-key mutation causes exactly one new backing and changed worker input");
}

void TestComponentFailureIsAtomic()
{
    RtPathTraceAccelCpuSnapshot snapshot = MakeSnapshot();
    snapshot.staticBucket.triangleMaterials.clear();
    snapshot.inputReceipt = BuildPathTraceAccelCpuInputReceipt(snapshot);
    RtPathTraceAccelCpuProduct product;
    Check(!BuildPathTraceAccelCpuProduct(snapshot, product, nullptr) &&
        !product.complete && product.OwnedBytes() == 0,
        "component failure publishes no mixed product");
}
}

int main()
{
    TestRealThreeComponentProduct();
    TestResidentTicketBuildAndReceipt();
    TestProductionResidentPublisherPreservesHeader();
    TestPreflightAndCapacityStamp();
    TestRigidMaterialRemapIncludesInstances();
    TestAllocationFailureCleanup();
    TestSnapshotReserveFailureCleanup();
    TestStaticDisabledProduct();
    TestReceiptDeterminismAndEligibility();
    TestProductionResidentBackingStore();
    TestComponentFailureIsAtomic();
    PathTraceAccelCpuPackSetAllocationFailureForTest(-1, false);
    std::cout << (g_failures == 0 ? "AccelCpuPack harness PASS\n" :
        "AccelCpuPack harness FAIL\n");
    return g_failures == 0 ? 0 : 1;
}
