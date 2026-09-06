#include "../idlib/precompiled.h"
#include "PathTraceCaptureProduct.h"
#include "PathTraceCommittedCapture.h"
#include "PathTraceMaterialIdKernel.h"
#include "PathTracePureSemanticWorkerPipeline.h"
#include "PathTraceOwnerSemanticKernel.h"
#include "PathTraceRigidIdentity.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <limits>
#include <vector>

idCommon* idLib::common = nullptr;
bool AssertFailed(const char*, int, const char*) { return false; }
const float idMath::INFINITUM = 1.0e30f;
const float idMath::FLT_SMALLEST_NON_DENORMAL =
    std::numeric_limits<float>::min();

// precompiled.h redirects container allocation through the engine heap API.
// Keep this focused executable dependency-light while retaining the real
// engine types by supplying a harness-local, ABI-matching aligned allocator.
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
    if (pointer != nullptr)
    {
        std::memset(pointer, 0, size);
    }
    return pointer;
}

// RigidIdentity.cpp normally obtains this byte hash from the renderer
// acceleration TU.  The focused target links only the identity authority, so
// provide the same ABI for its executable parity checks without pulling in the
// renderer/device implementation.
uint64 HashSmokeBytes(uint64 hash, const void* data, size_t size)
{
    const byte* bytes = static_cast<const byte*>(data);
    for (size_t index = 0; index < size; ++index)
    {
        hash ^= static_cast<uint64>(bytes[index]);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t Sys_Microseconds()
{
    return 1;
}

idVec3 SmokeVertexPosition(const PathTraceSmokeVertex& vertex)
{
    return idVec3(vertex.position[0], vertex.position[1], vertex.position[2]);
}

idVec3 SmokeVertexNormal(const PathTraceSmokeVertex& vertex)
{
    return idVec3(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
}

bool SmokeNormalIsUsable(const idVec3& normal)
{
    return normal.LengthSqr() > 1.0e-8f;
}

bool IsZeroAreaSmokeTriangle(
    const idVec3& p0, const idVec3& p1, const idVec3& p2)
{
    return ((p1 - p0).Cross(p2 - p0)).LengthSqr() <= 1.0e-12f;
}

PathTraceSmokeVertex BuildPathTraceCommittedVertexFromOwned(
    const RtPathTraceCommittedVertexInput& input, std::uint32_t vertexIndex)
{
    const idDrawVert& source = input.vertices[vertexIndex];
    PathTraceSmokeVertex vertex = {};
    vertex.position[0] = source.xyz.x;
    vertex.position[1] = source.xyz.y;
    vertex.position[2] = source.xyz.z;
    vertex.position[3] = 1.0f;
    vertex.normal[2] = 1.0f;
    return vertex;
}

namespace
{
int g_failures = 0;

void Check(bool condition, const char* name)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << name << '\n';
    if (!condition)
    {
        ++g_failures;
    }
}

RtPathTraceCaptureCapacityCounts AllCounts()
{
    RtPathTraceCaptureCapacityCounts count;
    count.surfaces = 3;
    count.classifierStages = 2;
    count.runtimeStages = 2;
    count.registers = 4;
    count.vertices = 9;
    count.indexes = 9;
    count.joints = 2;
    count.variantBases = 2;
    count.registryMaterials = 2;
    count.modelTables = 1;
    count.modelSurfaceTokens = 3;
    count.applyGateKeys = 2;
    count.instanceMeshes = 2;
    count.instanceHistories = 2;
    count.geometryStaticSurfaces = 2;
    count.geometryRigidRoutes = 2;
    count.geometryRigidResidents = 2;
    count.receiptSurfaces = count.surfaces;
    count.materialInfoIntents = count.surfaces;
    count.materialVariantProposals = count.surfaces;
    count.instanceObservationProposals = count.surfaces;
    count.rigidCandidateProposals = count.surfaces;
    count.preparedRigidPayloads = count.surfaces;
    count.preparedRigidVertices = count.vertices;
    count.preparedRigidIndexes = count.indexes;
    return count;
}

std::size_t ProductPerSurfaceBytes()
{
    return sizeof(RtPathTraceCaptureSurfaceProduct) +
        sizeof(RtPathTraceMaterialInfoRegistrationIntent) +
        sizeof(RtPathTraceMaterialVariantRegistrationProposal) +
        sizeof(RtPathTraceInstanceObservationProposal) +
        sizeof(RtPathTraceRigidCandidateProposal) +
        sizeof(RtPathTraceCommittedReferencedSurfaceReceiptRow) +
        2 * sizeof(std::uint32_t);
}

std::size_t OraclePerSurfaceBytes()
{
    return sizeof(RtPathTraceMaterialInfoRegistrationIntent) +
        sizeof(RtPathTraceMaterialVariantRegistrationProposal) +
        sizeof(RtPathTraceInstanceObservationProposal) +
        sizeof(RtPathTraceRigidCandidateProposal) + 2 * sizeof(std::uint32_t) +
        sizeof(RtPathTraceCaptureOracleSurface);
}

bool Plan(const RtPathTraceCaptureCapacityCounts& count,
    RtPathTraceCaptureProductCapacityPlan& plan)
{
    return PlanPathTraceCompleteSlotCapacity(count,
        sizeof(RtPathTraceCaptureOwnerSnapshot), plan);
}

RtPathTraceCaptureProduct MakeCandidate(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    const RtPathTraceCaptureProductCapacityPlan& plan)
{
    RtPathTraceCaptureProduct candidate;
    const std::size_t other = snapshot.OwnedBytes() + plan.oracleBytes;
    if (!ReservePathTraceCaptureProductStorage(
            snapshot, candidate, other, plan.candidateBytes))
    {
        return candidate;
    }
    candidate.capacityCounts = snapshot.capacityCounts;
    candidate.inputOwnedBytes = snapshot.OwnedBytes();
    const RtSmokeSurfaceClass classes[3] = {
        RtSmokeSurfaceClass::ParticleAlpha,
        RtSmokeSurfaceClass::StaticWorld,
        RtSmokeSurfaceClass::RigidEntity
    };
    for (std::uint32_t surface = 0; surface < 3; ++surface)
    {
        RtPathTraceCaptureSurfaceProduct row;
        row.ordinal = surface;
        row.terminal = RtPathTraceCaptureTerminal::Accepted;
        row.surfaceClass = classes[surface];
        row.surfaceClassId = 100 + surface;
        row.bucketIndex = static_cast<int>(classes[surface]);
        row.vertexOffset = surface * 3;
        row.vertexCount = 3;
        row.indexOffset = surface * 3;
        row.indexCount = 3;
        row.triangleOffset = surface;
        row.triangleCount = 1;
        candidate.surfaces.push_back(row);

        for (std::uint32_t local = 0; local < 3; ++local)
        {
            PathTraceSmokeVertex vertex = {};
            vertex.position[0] = static_cast<float>(surface * 3 + local);
            candidate.vertices.push_back(vertex);
            candidate.indexes.push_back(surface * 3 + local);
        }
        candidate.triangleClasses.push_back(1000 + surface);
        candidate.triangleMaterials.push_back(2000 + surface);
        candidate.triangleInstances.push_back(3000 + surface);
        candidate.triangleIdentities.push_back(4000 + surface);

        RtPathTraceMaterialInfoRegistrationIntent intent;
        intent.surfaceOrdinal = surface;
        candidate.materialInfoIntents.push_back(intent);
        RtPathTraceMaterialVariantRegistrationProposal variant;
        variant.surfaceOrdinal = surface;
        variant.chosenId = 5000 + surface;
        candidate.materialVariants.push_back(variant);
        RtPathTraceInstanceObservationProposal observation;
        observation.surfaceOrdinal = surface;
        observation.instanceId = 6000 + surface;
        candidate.instanceObservations.push_back(observation);
        RtPathTraceRigidCandidateProposal rigid;
        rigid.surfaceOrdinal = surface;
        rigid.meshHash = 7000 + surface;
        candidate.rigidCandidates.push_back(rigid);
    }
    candidate.staticMembershipSurfaces.push_back(2);
    candidate.staticMembershipSurfaces.push_back(0);
    candidate.routedReadySkipSurfaces.push_back(1);
    return candidate;
}

bool ReserveFinal(
    const RtPathTraceCaptureOwnerSnapshot& snapshot,
    const RtPathTraceCaptureProductCapacityPlan& plan,
    const RtPathTraceCaptureProduct& candidate,
    RtPathTraceCaptureProduct& finalProduct)
{
    return ReservePathTraceCaptureProductStorage(snapshot, finalProduct,
        snapshot.OwnedBytes() + candidate.OwnedBytes() + plan.oracleBytes,
        plan.candidateBytes);
}

RtPathTraceCaptureProduct MakeCanonicalTwoSurfaceProduct()
{
    RtPathTraceCaptureProduct product;
    product.epoch.generation = 77;
    product.epoch.frameIndex = 77;
    product.epoch.capturedAfterBeginFrame = true;
    product.epoch.capturedAfterStaticPreload = true;
    product.epoch.capturedBeforeSerialMutate = true;
    product.viewIdentity = 79;
    product.ownerDecisionGeometryOnly = true;
    for (std::uint32_t ordinal = 0; ordinal < 2; ++ordinal)
    {
        RtPathTraceCaptureSurfaceProduct surface;
        surface.ordinal = ordinal;
        surface.terminal = RtPathTraceCaptureTerminal::Accepted;
        surface.surfaceClass = RtSmokeSurfaceClass::RigidEntity;
        surface.surfaceClassId = 1;
        surface.materialId = 11;
        surface.materialClassSignature = 13;
        surface.sourceFlags = 17;
        surface.meshHash = 19;
        surface.instanceId = 23 + ordinal;
        surface.vertexOffset = ordinal * 3;
        surface.vertexCount = 3;
        surface.indexOffset = ordinal * 3;
        surface.indexCount = 3;
        surface.triangleOffset = ordinal;
        surface.triangleCount = 1;
        surface.bucketIndex = static_cast<int>(surface.surfaceClass);
        product.surfaces.push_back(surface);
        for (std::uint32_t vertex = 0; vertex < 3; ++vertex)
        {
            product.vertices.emplace_back();
            product.indexes.push_back(ordinal * 3 + vertex);
        }
        product.triangleClasses.push_back(31 + ordinal);
        product.triangleMaterials.push_back(37 + ordinal);
        product.triangleInstances.push_back(41 + ordinal);
        product.triangleIdentities.push_back(43 + ordinal);
    }
    product.complete = true;
    return product;
}

RtPathTraceCaptureProduct MakeValidPureSemanticUnifiedProduct()
{
    RtPathTraceCaptureProduct pure;
    pure.inputOwnedBytes = 1;
    pure.provenance = RtPathTraceCaptureProductProvenance::PureSemantic;
    pure.epoch.generation = 70;
    pure.epoch.frameIndex = 900;
    pure.epoch.mapTimeStamp = 1234;
    pure.epoch.mapLoadSerial = 5678;
    RtPathTracePlanningCopyName(pure.epoch.mapName,
        sizeof(pure.epoch.mapName), "maps/test/pure_semantic");
    pure.capacityCounts.surfaces = 4;
    pure.capacityCounts.materialInfoIntents = 2;
    pure.capacityCounts.materialVariantProposals = 2;
    pure.capacityCounts.instanceObservationProposals = 2;
    pure.capacityCounts.rigidCandidateProposals = 2;
    pure.capacityCounts.preparedRigidPayloads = 2;
    pure.capacityCounts.preparedRigidVertices = 3;
    pure.capacityCounts.preparedRigidIndexes = 3;
    pure.capacityCounts.receiptSurfaces = 4;
    pure.capacityCounts.frameMaterialIds = 3;
    pure.frameMaterialIds = { 0u, 17u, 42u };
    pure.receiptHeader.finalizedMaterialIdCount = 3;
    pure.receiptHeader.finalizedMaterialIdHash =
        RtPathTraceFinalizedMaterialIdHash(pure.frameMaterialIds.data(),
            pure.frameMaterialIds.size());
    pure.finalizedMaterialIdCount = pure.receiptHeader.finalizedMaterialIdCount;
    pure.finalizedMaterialIdHash = pure.receiptHeader.finalizedMaterialIdHash;
    pure.receiptHeader.complete = true;
    pure.receiptHeader.sealedPrimaryViewToken = 71;
    pure.receiptHeader.ownerUniverseFrameIndex = 900;
    pure.receiptHeader.baselineFrameIndex = 899;
    pure.receiptHeader.worldLifecycleGeneration = 2;
    pure.receiptHeader.mapLoadSerial = pure.epoch.mapLoadSerial;
    pure.receiptHeader.mapTimeStamp = pure.epoch.mapTimeStamp;
    std::memcpy(pure.receiptHeader.mapName, pure.epoch.mapName,
        sizeof(pure.receiptHeader.mapName));
    pure.receiptHeader.barrierGeneration = 3;
    pure.receiptHeader.materialRegistryGeneration = 4;
    pure.receiptHeader.residentMaterialFactsGeneration = 5;
    pure.receiptHeader.configFingerprint = 6;
    pure.receiptHeader.instanceUniverseGeneration = 7;
    pure.receiptHeader.geometryUniverseGeneration = 8;
    pure.receiptHeader.staticMaterialGeneration = 9;
    pure.receiptHeader.canonicalSourceIndexPoolGeneration = 10;
    pure.receiptHeader.staticResidentPayloadGeneration = 11;
    pure.receiptHeader.surfaceCount = 4;
    for (std::uint32_t ordinal = 0; ordinal < 4; ++ordinal)
    {
        RtPathTraceCaptureSurfaceProduct surface;
        surface.ordinal = ordinal;
        pure.surfaces.push_back(surface);
        RtPathTraceCommittedReferencedSurfaceReceiptRow receipt;
        receipt.surfaceOrdinal = ordinal;
        pure.receiptSurfaces.push_back(receipt);
    }
    const std::uint32_t proposalOrdinals[] = { 0, 2 };
    std::uint32_t proposalIndex = 0;
    for (std::uint32_t ordinal : proposalOrdinals)
    {
        RtPathTraceMaterialInfoRegistrationIntent intent;
        intent.surfaceOrdinal = ordinal;
        intent.callPresent = true;
        intent.reason = 1;
        pure.materialInfoIntents.push_back(intent);
        RtPathTraceMaterialVariantRegistrationProposal variant;
        variant.surfaceOrdinal = ordinal;
        pure.materialVariants.push_back(variant);
        RtPathTraceInstanceObservationProposal observation;
        observation.surfaceOrdinal = ordinal;
        pure.instanceObservations.push_back(observation);
        RtPathTraceRigidCandidateProposal rigid;
        rigid.surfaceOrdinal = ordinal;
        rigid.meshHash = 900;
        rigid.instanceId = 902 + proposalIndex;
        rigid.materialId = 901;
        pure.rigidCandidates.push_back(rigid);
        pure.surfaces[ordinal].meshHash = rigid.meshHash;
        pure.surfaces[ordinal].instanceId = rigid.instanceId;
        pure.surfaces[ordinal].materialId = rigid.materialId;
        pure.surfaces[ordinal].sourceFlags = RT_PT_INSTANCE_SOURCE_RIGID;
        pure.receiptSurfaces[ordinal].persistedMeshHash = rigid.meshHash;
        pure.receiptSurfaces[ordinal].persistedChosenMaterialId = rigid.materialId;
        ++proposalIndex;
    }
    idDrawVert preparedVertices[3] = {};
    preparedVertices[0].xyz.Set(0.0f, 0.0f, 0.0f);
    preparedVertices[1].xyz.Set(1.0f, 0.0f, 0.0f);
    preparedVertices[2].xyz.Set(0.0f, 1.0f, 0.0f);
    triIndex_t preparedIndexes[3] = { 0, 1, 2 };
    RtPathTraceRigidPreparedPayload prepared;
    prepared.surfaceOrdinal = pure.rigidCandidates[0].surfaceOrdinal;
    prepared.occurrenceCount = 2;
    prepared.meshHash = pure.rigidCandidates[0].meshHash;
    prepared.instanceId = pure.rigidCandidates[0].instanceId;
    prepared.materialId = pure.rigidCandidates[0].materialId;
    prepared.sourceFlags = RT_PT_INSTANCE_SOURCE_RIGID;
    prepared.fullTriangleVertexCount = 3;
    prepared.fullTriangleIndexCount = 3;
    RtPathTraceRigidOwnedCpuCache preparedCache;
    if (RtPathTraceBuildRigidOwnedCpuCache(preparedVertices, 3,
            preparedIndexes, 3, prepared.normalTexMatrix, preparedCache))
    {
        prepared.localVertices = std::move(preparedCache.vertices);
        prepared.localIndexes = std::move(preparedCache.indexes);
        std::memcpy(prepared.triangleBoundsMin, preparedCache.boundsMin,
            sizeof(prepared.triangleBoundsMin));
        std::memcpy(prepared.triangleBoundsMax, preparedCache.boundsMax,
            sizeof(prepared.triangleBoundsMax));
        prepared.contentSignature = preparedCache.contentSignature;
        pure.preparedRigidPayloads.push_back(std::move(prepared));
    }
    pure.complete = true;
    pure.actualOwnedBytes = pure.OwnedBytes();
    return pure;
}

void TestUnifiedCaptureProductShape()
{
    RtPathTraceCaptureProduct owner;
    owner.inputOwnedBytes = 1;
    owner.membershipReceipt.complete = true;
    owner.ownerDecisionGeometryOnly = true;
    owner.provenance = RtPathTraceCaptureProductProvenance::OwnerDecisions;
    owner.complete = true;
    owner.actualOwnedBytes = owner.OwnedBytes();
    Check(RtPathTraceCaptureProductShapeValid(owner),
        "unified shape accepts receipt-free legacy OwnerDecisions authority");
    owner.provenance = RtPathTraceCaptureProductProvenance::PureSemantic;
    Check(!RtPathTraceCaptureProductShapeValid(owner),
        "provenance tamper cannot promote legacy output to PureSemantic");
    owner.provenance = RtPathTraceCaptureProductProvenance::OwnerDecisions;
    owner.receiptHeader.complete = true;
    Check(!RtPathTraceCaptureProductShapeValid(owner),
        "legacy authority cannot fabricate a committed referenced-set receipt");

    RtPathTraceCaptureProduct pure = MakeValidPureSemanticUnifiedProduct();
    Check(RtPathTraceCaptureProductShapeValid(pure),
        "unified shape accepts coherent PureSemantic proposal and receipt authority");
    Check(pure.finalizedMaterialIdHash == 0x89426f9441a77fb6ull,
        "frame material seal is portable domain-separated FNV-1a over LE uint32 bytes");

    const auto rejects = [](const char* label, const auto& mutate)
    {
        RtPathTraceCaptureProduct candidate =
            MakeValidPureSemanticUnifiedProduct();
        mutate(candidate);
        candidate.actualOwnedBytes = candidate.OwnedBytes();
        Check(!RtPathTraceCaptureProductShapeValid(candidate), label);
    };
    rejects("PureSemantic rejects dropped material-info proposal",
        [](auto& value) { value.materialInfoIntents.pop_back(); });
    rejects("PureSemantic rejects dropped material-variant proposal",
        [](auto& value) { value.materialVariants.pop_back(); });
    rejects("PureSemantic rejects dropped instance proposal",
        [](auto& value) { value.instanceObservations.pop_back(); });
    rejects("PureSemantic rejects dropped rigid proposal",
        [](auto& value) { value.rigidCandidates.pop_back(); });
    rejects("PureSemantic rejects dropped complete frame material ID",
        [](auto& value) { value.frameMaterialIds.pop_back(); });
    rejects("PureSemantic rejects duplicate frame material ID",
        [](auto& value) { value.frameMaterialIds[2] = value.frameMaterialIds[1]; });
    rejects("PureSemantic rejects unsorted frame material IDs",
        [](auto& value) { std::swap(value.frameMaterialIds[0], value.frameMaterialIds[1]); });
    rejects("PureSemantic rejects tampered frame material count",
        [](auto& value) { ++value.capacityCounts.frameMaterialIds; });
    rejects("PureSemantic rejects sorted unique material substitution with sealed count",
        [](auto& value) { value.frameMaterialIds[1] = 18u; });
    rejects("PureSemantic rejects coherently decremented public material count",
        [](auto& value)
        {
            value.frameMaterialIds.pop_back();
            --value.capacityCounts.frameMaterialIds;
            --value.finalizedMaterialIdCount;
            --value.receiptHeader.finalizedMaterialIdCount;
        });
    rejects("PureSemantic rejects replacement of authoritative zero",
        [](auto& value) { value.frameMaterialIds[0] = 1u; });
    rejects("PureSemantic rejects finalized material hash tamper",
        [](auto& value) { ++value.receiptHeader.finalizedMaterialIdHash; });
    rejects("PureSemantic rejects finalized material count tamper",
        [](auto& value) { --value.receiptHeader.finalizedMaterialIdCount; });
    rejects("PureSemantic rejects product material hash tamper",
        [](auto& value) { ++value.finalizedMaterialIdHash; });
    rejects("PureSemantic rejects product material count tamper",
        [](auto& value) { --value.finalizedMaterialIdCount; });
    rejects("PureSemantic rejects unequal material family cardinality",
        [](auto& value)
        {
            value.materialInfoIntents.pop_back();
            --value.capacityCounts.materialInfoIntents;
        });
    rejects("PureSemantic rejects unpaired material ordinals",
        [](auto& value) { value.materialVariants[1].surfaceOrdinal = 3; });
    rejects("PureSemantic rejects material-info duplicate ordinal",
        [](auto& value) { value.materialInfoIntents[1].surfaceOrdinal = 0; });
    rejects("PureSemantic rejects material-info out-of-range ordinal",
        [](auto& value) { value.materialInfoIntents[1].surfaceOrdinal = 4; });
    rejects("PureSemantic rejects material-variant duplicate ordinal",
        [](auto& value) { value.materialVariants[1].surfaceOrdinal = 0; });
    rejects("PureSemantic rejects material-variant out-of-range ordinal",
        [](auto& value) { value.materialVariants[1].surfaceOrdinal = 4; });
    rejects("PureSemantic rejects instance duplicate ordinal",
        [](auto& value) { value.instanceObservations[1].surfaceOrdinal = 0; });
    rejects("PureSemantic rejects instance out-of-range ordinal",
        [](auto& value) { value.instanceObservations[1].surfaceOrdinal = 4; });
    rejects("PureSemantic rejects rigid duplicate ordinal",
        [](auto& value) { value.rigidCandidates[1].surfaceOrdinal = 0; });
    rejects("PureSemantic rejects rigid out-of-range ordinal",
        [](auto& value) { value.rigidCandidates[1].surfaceOrdinal = 4; });
    rejects("PureSemantic rejects absent material intent call",
        [](auto& value) { value.materialInfoIntents[0].callPresent = false; });
    rejects("PureSemantic rejects non-registration material intent reason",
        [](auto& value) { value.materialInfoIntents[0].reason = 2; });
    rejects("PureSemantic rejects overflowing epoch generation",
        [](auto& value) { value.epoch.generation = UINT64_MAX; });
    rejects("PureSemantic rejects sealed token not succeeding epoch",
        [](auto& value) { ++value.receiptHeader.sealedPrimaryViewToken; });
    rejects("PureSemantic rejects owner frame not matching epoch",
        [](auto& value) { ++value.receiptHeader.ownerUniverseFrameIndex; });
    rejects("PureSemantic rejects receipt map name mismatch",
        [](auto& value) { value.receiptHeader.mapName[0] = 'x'; });
    rejects("PureSemantic rejects receipt map timestamp mismatch",
        [](auto& value) { ++value.receiptHeader.mapTimeStamp; });
    rejects("PureSemantic rejects receipt map-load serial mismatch",
        [](auto& value) { ++value.receiptHeader.mapLoadSerial; });

    pure.receiptHeader.complete = false;
    Check(!RtPathTraceCaptureProductShapeValid(pure),
        "incomplete receipt rejects an otherwise complete PureSemantic product");

    RtPathTraceCaptureProduct prior;
    prior.complete = true;
    prior.viewIdentity = 0x12345678u;
    prior.materialVariants.resize(1);
    prior.materialVariants[0].candidateId = 0xabcdefu;
    RtPathTraceCaptureOwnerSnapshot invalidSnapshot;
    Check(!BuildPathTraceCaptureProduct(invalidSnapshot, prior) &&
            prior.complete && prior.viewIdentity == 0x12345678u &&
            prior.materialVariants.size() == 1 &&
            prior.materialVariants[0].candidateId == 0xabcdefu,
        "BuildPathTraceCaptureProduct false preserves prior output atomically");
}

template<typename T>
bool PodVectorsEqual(const std::vector<T>& lhs, const std::vector<T>& rhs);

bool ReferencedReceiptHeadersEqual(
    const RtPathTraceCommittedReferencedSetReceiptHeader& lhs,
    const RtPathTraceCommittedReferencedSetReceiptHeader& rhs)
{
    return lhs.sealedPrimaryViewToken == rhs.sealedPrimaryViewToken &&
        lhs.ownerUniverseFrameIndex == rhs.ownerUniverseFrameIndex &&
        lhs.baselineFrameIndex == rhs.baselineFrameIndex &&
        lhs.worldLifecycleGeneration == rhs.worldLifecycleGeneration &&
        lhs.mapLoadSerial == rhs.mapLoadSerial &&
        lhs.mapTimeStamp == rhs.mapTimeStamp &&
        std::memcmp(lhs.mapName, rhs.mapName, sizeof(lhs.mapName)) == 0 &&
        lhs.barrierGeneration == rhs.barrierGeneration &&
        lhs.materialRegistryGeneration == rhs.materialRegistryGeneration &&
        lhs.residentMaterialFactsGeneration == rhs.residentMaterialFactsGeneration &&
        lhs.configFingerprint == rhs.configFingerprint &&
        lhs.instanceUniverseGeneration == rhs.instanceUniverseGeneration &&
        lhs.geometryUniverseGeneration == rhs.geometryUniverseGeneration &&
        lhs.staticMaterialGeneration == rhs.staticMaterialGeneration &&
        lhs.canonicalSourceIndexPoolGeneration == rhs.canonicalSourceIndexPoolGeneration &&
        lhs.staticResidentPayloadGeneration == rhs.staticResidentPayloadGeneration &&
        lhs.surfaceCount == rhs.surfaceCount &&
        lhs.recordAllInstanceClasses == rhs.recordAllInstanceClasses &&
        lhs.complete == rhs.complete;
}

static_assert(std::is_invocable<
    RtPathTracePureSemanticWorkerFunction,
    RtPathTracePrimarySemanticScratch&&,
    const RtPathTraceCommittedPlanningBaseline&,
    RtPathTraceCaptureProduct&,
    const RtPathTracePureSemanticWorkerTestSeam*>::value,
    "S3 function type must accept an explicit rvalue scratch");
static_assert(!std::is_invocable<
    RtPathTracePureSemanticWorkerFunction,
    RtPathTracePrimarySemanticScratch&,
    const RtPathTraceCommittedPlanningBaseline&,
    RtPathTraceCaptureProduct&,
    const RtPathTracePureSemanticWorkerTestSeam*>::value,
    "S3 function type must reject lvalue scratch ownership");

bool S3ScratchWasExplicitlyCleared(
    const RtPathTracePrimarySemanticScratch& scratch)
{
    return scratch.sealedPrimaryViewToken == 0 && !scratch.complete &&
        scratch.ownedBytes == 0 && !scratch.snapshot.complete &&
        scratch.snapshot.OwnedBytes() == sizeof(scratch.snapshot);
}

RtPathTraceCommittedPlanningBaseline MakeS3Baseline()
{
    RtPathTraceCommittedPlanningBaseline baseline;
    baseline.epoch.committedViewToken = 70;
    baseline.epoch.sealedPrimaryViewToken = 70;
    baseline.epoch.baselineFrameIndex = 700;
    baseline.epoch.ownerUniverseFrameIndex = 900;
    baseline.epoch.worldLifecycleGeneration = 3;
    baseline.epoch.mapLoadSerial = 4;
    baseline.epoch.mapTimeStamp = 5;
    RtPathTracePlanningCopyName(baseline.epoch.mapName,
        sizeof(baseline.epoch.mapName), "maps/s3.proc");
    baseline.epoch.barrierGeneration = 6;
    baseline.epoch.materialRegistryGeneration = 12;
    baseline.epoch.residentMaterialFactsGeneration = 13;
    baseline.epoch.instanceUniverseGeneration = 9;
    baseline.epoch.geometryUniverseGeneration = 7;
    baseline.epoch.staticMaterialGeneration = 8;
    baseline.epoch.canonicalSourceIndexPoolGeneration = 10;
    baseline.epoch.staticResidentPayloadGeneration = 11;
    baseline.epoch.capturedAfterRootEndFrame = true;
    baseline.epoch.capturedAfterStaticPrune = true;
    baseline.semanticConfig.removeRoutedRigidDynamic = true;
    baseline.semanticConfig.admissionMaxSurfaces = 16;
    baseline.semanticConfig.admissionMaxBytes = 1024 * 1024;
    baseline.semanticConfig.configFingerprint = 0x11223344;
    baseline.semanticConfig.configComplete = true;
    const RtPathTracePlanningSnapshotEpoch mapped =
        RtPathTraceMapCommittedBaselineToPlanningEpoch(baseline.epoch);
    baseline.instanceUniverse.epoch = mapped;
    baseline.instanceUniverse.ownerGeneration = 9;
    baseline.instanceUniverse.complete = true;
    baseline.geometryUniverse.epoch = mapped;
    baseline.geometryUniverse.ownerGeneration = 7;
    baseline.geometryUniverse.complete = true;
    baseline.applyGateComplete = true;
    baseline.priorSkinnedRecordsComplete = true;
    baseline.complete = true;
    return baseline;
}

bool BuildS3SourceScratch(
    const RtPathTraceCommittedPlanningBaseline& baseline,
    RtPathTracePrimarySemanticScratch& scratch)
{
    RtPathTraceCaptureRawSurface raw[2];
    raw[0].ordinal = 0;
    raw[0].safety = RtPathTraceCaptureSafetyDisposition::Ready;
    raw[0].classify.hasEntityDef = true;
    raw[0].classify.material.materialPresent = true;
    raw[0].classify.material.coverage = MC_OPAQUE;
    raw[0].classify.material.deform = DFRM_NONE;
    raw[0].classifier.materialPresent = true;
    raw[0].callbackAllowed = true;
    raw[0].guiAllowed = true;
    raw[0].entityIndex = 2;
    raw[0].entityNum = 3;
    raw[0].vertexOffset = 0;
    raw[0].vertexCount = 3;
    raw[0].indexOffset = 0;
    raw[0].indexCount = 3;
    raw[0].sourceTriIndexCount = 3;
    raw[0].triNumVerts = 3;
    raw[0].triNumIndexes = 3;
    raw[0].triTriangleCount = 1;
    raw[0].runtimeStageOffset = 0;
    raw[0].runtimeStageCount = 1;
    raw[0].registerOffset = 0;
    raw[0].registerCount = 5;
    raw[0].registersPresent = true;
    raw[0].modelMatrix[0] = raw[0].modelMatrix[5] = raw[0].modelMatrix[10] =
        raw[0].modelMatrix[15] = 1.0f;
    raw[0].bumpMatrix[0] = raw[0].bumpMatrix[4] = 1.0f;
    raw[0].objectToWorld[0] = raw[0].objectToWorld[5] = raw[0].objectToWorld[10] =
        raw[0].objectToWorld[15] = 1.0f;
    RtPathTracePlanningCopyName(raw[0].materialName,
        sizeof(raw[0].materialName), "models/s3/rigid");
    raw[1].ordinal = 1;
    idDrawVert vertices[3];
    vertices[0].xyz.Set(0.0f, 0.0f, 0.0f);
    vertices[1].xyz.Set(1.0f, 0.0f, 0.0f);
    vertices[2].xyz.Set(0.0f, 1.0f, 0.0f);
    triIndex_t indexes[3] = { 0, 1, 2 };
    RtPathTraceRuntimeMaterialStagePod runtimeStage;
    runtimeStage.stageIndex = 0;
    runtimeStage.valid = true;
    runtimeStage.usesPerSurfaceState = true;
    runtimeStage.imagePresent = true;
    runtimeStage.emissiveLike = true;
    runtimeStage.lighting = SL_AMBIENT;
    runtimeStage.drawStateBits = GLS_SRCBLEND_ONE | GLS_DSTBLEND_ONE;
    runtimeStage.conditionRegister = 0;
    runtimeStage.colorRegisters[0] = 1;
    runtimeStage.colorRegisters[1] = 2;
    runtimeStage.colorRegisters[2] = 3;
    runtimeStage.colorRegisters[3] = 4;
    float registers[5] = { 1.0f, 2.0f, 1.0f, 0.5f, 1.0f };
    RtPathTracePrimarySemanticDto dto;
    dto.sealedPrimaryViewToken = 71;
    dto.materialRegistryGeneration = 12;
    dto.residentMaterialFactsGeneration = 13;
    dto.sourceDrawSurfCount = 2;
    dto.surfaces = raw;
    dto.surfaceCount = 2;
    dto.runtimeStages = &runtimeStage;
    dto.runtimeStageCount = 1;
    dto.registers = registers;
    dto.registerCount = 5;
    dto.vertices = vertices;
    dto.vertexCount = 3;
    dto.indexes = indexes;
    dto.indexCount = 3;
    dto.complete = true;
    return BuildPathTracePrimarySemanticScratch(dto, 71,
        baseline.semanticConfig, scratch);
}

void TestPureSemanticFrameMaterialIds()
{
    const auto checkIds = [](const std::vector<std::pair<std::uint32_t,
            std::uint32_t>>& rows,
        const std::vector<std::uint32_t>& expected,
        const char* label)
    {
        RtPathTraceCaptureOwnerSnapshot snapshot;
        for (const auto& ids : rows)
        {
            RtPathTraceCaptureRawSurface row;
            row.derivedBaseMaterialId = ids.first;
            row.derivedChosenMaterialId = ids.second;
            snapshot.surfaces.push_back(row);
        }
        RtPathTraceCaptureProduct product;
        product.frameMaterialIds.reserve(expected.size());
        const bool emitted = RtPathTraceEmitPureSemanticFrameMaterialIds(
            snapshot, product);
        Check(emitted &&
                RtPathTraceCountPureSemanticFrameMaterialIds(snapshot) ==
                    expected.size() &&
                product.frameMaterialIds == expected,
            label);
    };
    checkIds({}, {}, "complete frame material IDs preserve empty-scene authority");
    checkIds({ { 5u, 5u } }, { 5u },
        "complete frame material IDs deduplicate equal base and chosen IDs");
    checkIds({ { 7u, 9u } }, { 7u, 9u },
        "complete frame material IDs include differing base and chosen IDs");
    checkIds({ { 0u, 3u }, { 3u, 0u }, { 9u, 3u } },
        { 0u, 3u, 9u },
        "complete frame material IDs retain zero and deduplicate across rows");
}

void TestPureSemanticWorkerPipelineS3()
{
    const RtPathTraceCommittedPlanningBaseline baseline = MakeS3Baseline();
    RtPathTracePrimarySemanticScratch source;
    Check(BuildS3SourceScratch(baseline, source) &&
            source.snapshot.admissionMaxSurfaces ==
                baseline.semanticConfig.admissionMaxSurfaces &&
            source.snapshot.admissionMaxBytes ==
                baseline.semanticConfig.admissionMaxBytes,
        "S3 source scratch carries exact committed admission authority");
    RtPathTraceCaptureProduct product;
    const RtPathTracePureSemanticWorkerResult built =
        BuildPathTracePureSemanticWorkerProductS3(
            std::move(source), baseline, product);
    Check(built.accepted && built.inputConsumed &&
            built.rejection == RtPathTracePureSemanticWorkerReject::None &&
            built.finalizerCalls == 1 && built.canonicalQueryViolations == 0,
        "S3 moved scratch finalizes once and atomically publishes");
    Check(S3ScratchWasExplicitlyCleared(source),
        "S3 rvalue ownership transfer explicitly clears the caller source");
    Check(product.complete && product.receiptHeader.complete &&
            product.provenance ==
                RtPathTraceCaptureProductProvenance::PureSemantic &&
            product.viewIdentity == 0 &&
            product.epoch.generation == 70 && product.epoch.frameIndex == 900 &&
            product.receiptHeader.sealedPrimaryViewToken == 71 &&
            product.receiptHeader.ownerUniverseFrameIndex == 900 &&
            product.lateConsumeToken.mapLoadSerial == 4 &&
            product.lateConsumeToken.registryGeneration ==
                baseline.epoch.materialRegistryGeneration &&
            product.lateConsumeToken.residentMaterialFactsGeneration ==
                baseline.epoch.residentMaterialFactsGeneration &&
            product.lateConsumeToken.instanceUniverseGeneration ==
                baseline.epoch.instanceUniverseGeneration &&
            product.lateConsumeToken.geometryUniverseGeneration ==
                baseline.epoch.geometryUniverseGeneration &&
            product.lateConsumeToken.configFingerprint ==
                baseline.semanticConfig.configFingerprint &&
            product.lateConsumeToken.capturedAfterBeginFrame &&
            product.lateConsumeToken.capturedAfterStaticPreload,
        "S3 reconstructs pointer-free epoch, receipt, and late token authority");
    Check(RtPathTraceCaptureProductShapeValid(product),
        "S3 seals a shape-valid unified product");
    Check(product.surfaces.size() == 2 &&
            product.receiptSurfaces.size() == 2 &&
            product.receiptSurfaces[0].rigidIdentityPresent,
        "S3 seals ordinal surface and receipt authority");
    Check(product.instanceObservations.size() == 1 &&
            product.rigidCandidates.size() == 1,
        "S3 seals observation and rigid proposal families in one product");
    Check(product.frameMaterialIds.size() == 3 &&
            product.frameMaterialIds[0] == 0u &&
            product.frameMaterialIds[1] != product.frameMaterialIds[2] &&
            product.receiptHeader.finalizedMaterialIdCount == 3u &&
            product.receiptHeader.finalizedMaterialIdHash ==
                RtPathTraceFinalizedMaterialIdHash(
                    product.frameMaterialIds.data(),
                    product.frameMaterialIds.size()),
        "S3 seals exact base/chosen material set including authoritative zero");
    Check(built.peakOwnedBytes == built.movedScratchOwnedBytes +
            built.baselineOwnedBytes + built.productOwnedBytes &&
            built.peakOwnedBytes <= RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES,
        "S3 peak is one moved scratch plus baseline plus one final product");

    RtPathTracePrimarySemanticScratch oracleScratch;
    Check(BuildS3SourceScratch(baseline, oracleScratch),
        "S3 proposal oracle source builds independently");
    RtPathTraceCommittedSemanticFactsProvider oracleFacts(baseline);
    Check(FinalizePathTraceOwnerSemanticSnapshot(
            oracleScratch.snapshot, oracleFacts, nullptr, 0, false),
        "S3 proposal oracle uses the shared semantic finalizer");
    RtPathTraceCommittedProposalProduct proposalOracle;
    const auto proposalResult = BuildPathTraceCommittedProposalProductP2c2(
        oracleScratch, baseline, proposalOracle);
    Check(proposalResult.accepted,
        "S3 approved P2c proposal oracle accepts");
    Check(PodVectorsEqual(product.materialInfoIntents,
            proposalOracle.materialInfoIntents),
        "S3 material-info intents are byte-exact to P2c oracle");
    Check(PodVectorsEqual(product.materialVariants,
            proposalOracle.materialVariants),
        "S3 material variants are byte-exact to P2c oracle");
    Check(PodVectorsEqual(product.instanceObservations,
            proposalOracle.instanceObservations),
        "S3 instance observations are byte-exact to P2c oracle");
    Check(PodVectorsEqual(product.rigidCandidates,
            proposalOracle.rigidCandidates),
        "S3 rigid candidates are byte-exact to P2c oracle");
    Check(PodVectorsEqual(product.receiptSurfaces,
            proposalOracle.receiptSurfaces),
        "S3 receipt rows are byte-exact to P2c oracle");
    Check(ReferencedReceiptHeadersEqual(product.receiptHeader,
            proposalOracle.receiptHeader),
        "S3 shared receipt authority is exact to P2c oracle");

    RtPathTraceCaptureProduct prior;
    prior.complete = true;
    prior.viewIdentity = 0xfeed;
    RtPathTracePrimarySemanticScratch stale;
    Check(BuildS3SourceScratch(baseline, stale), "S3 stale fixture builds");
    RtPathTraceCommittedPlanningBaseline staleBaseline = baseline;
    --staleBaseline.epoch.sealedPrimaryViewToken;
    const auto staleResult = BuildPathTracePureSemanticWorkerProductS3(
        std::move(stale), staleBaseline, prior);
    Check(!staleResult.accepted &&
            staleResult.rejection ==
                RtPathTracePureSemanticWorkerReject::TokenMismatch &&
            prior.complete && prior.viewIdentity == 0xfeed,
        "S3 stale token rejects without changing prior output");

    for (int authority = 0; authority < 2; ++authority)
    {
        RtPathTracePrimarySemanticScratch mismatchScratch;
        Check(BuildS3SourceScratch(baseline, mismatchScratch),
            "S3 admission-mismatch source builds");
        RtPathTraceCommittedPlanningBaseline mismatchBaseline = baseline;
        if (authority == 0)
        {
            ++mismatchBaseline.semanticConfig.admissionMaxSurfaces;
        }
        else
        {
            ++mismatchBaseline.semanticConfig.admissionMaxBytes;
        }
        RtPathTraceCaptureProduct preserved;
        preserved.complete = true;
        preserved.viewIdentity = 0xbeef;
        const auto mismatchResult = BuildPathTracePureSemanticWorkerProductS3(
            std::move(mismatchScratch), mismatchBaseline, preserved);
        Check(!mismatchResult.accepted &&
                mismatchResult.rejection ==
                    RtPathTracePureSemanticWorkerReject::ConfigMismatch &&
                preserved.complete && preserved.viewIdentity == 0xbeef,
            "S3 either admission authority mismatch rejects atomically");
    }

    for (int generation = 0; generation < 2; ++generation)
    {
        RtPathTracePrimarySemanticScratch mismatchScratch;
        Check(BuildS3SourceScratch(baseline, mismatchScratch),
            "S3 generation-mismatch source builds");
        if (generation == 0)
        {
            ++mismatchScratch.snapshot.registryGeneration;
        }
        else
        {
            ++mismatchScratch.snapshot.residentMaterialFactsGeneration;
        }
        RtPathTraceCaptureProduct preserved;
        preserved.complete = true;
        preserved.viewIdentity = 0xabcd;
        const auto mismatchResult = BuildPathTracePureSemanticWorkerProductS3(
            std::move(mismatchScratch), baseline, preserved);
        Check(!mismatchResult.accepted &&
                mismatchResult.rejection ==
                    RtPathTracePureSemanticWorkerReject::GenerationMismatch &&
                preserved.complete && preserved.viewIdentity == 0xabcd,
            "S3 material generation mismatch rejects atomically");
    }

    for (std::size_t reserve = 0; reserve < 14; ++reserve)
    {
        RtPathTracePrimarySemanticScratch failing;
        Check(BuildS3SourceScratch(baseline, failing),
            "S3 reserve-failure source builds");
        RtPathTraceCaptureProduct preserved;
        preserved.complete = true;
        preserved.viewIdentity = 0xbeef;
        RtPathTraceCaptureReserveTestSeam reserveSeam;
        reserveSeam.failAtReserve = reserve;
        reserveSeam.failure = RtPathTraceCaptureReserveFailure::BadAlloc;
        RtPathTracePureSemanticWorkerTestSeam seam;
        seam.productReserve = &reserveSeam;
        const auto failed = BuildPathTracePureSemanticWorkerProductS3(
            std::move(failing), baseline, preserved, &seam);
        Check(!failed.accepted && preserved.complete &&
                preserved.viewIdentity == 0xbeef &&
                reserveSeam.reserveCalls == reserve + 1,
            "S3 each product reserve fails atomically and short-circuits");
    }

    RtPathTracePrimarySemanticScratch ownerReserveFailure;
    Check(BuildS3SourceScratch(baseline, ownerReserveFailure),
        "S3 owner-decision reserve-failure source builds");
    RtPathTraceCaptureProduct ownerReservePrior;
    ownerReservePrior.complete = true;
    ownerReservePrior.viewIdentity = 0xcafe;
    RtPathTracePureSemanticWorkerTestSeam ownerReserveSeam;
    ownerReserveSeam.failOwnerDecisionReserve = true;
    const auto ownerReserveResult = BuildPathTracePureSemanticWorkerProductS3(
        std::move(ownerReserveFailure), baseline, ownerReservePrior,
        &ownerReserveSeam);
    Check(!ownerReserveResult.accepted && ownerReserveResult.inputConsumed &&
            ownerReserveResult.rejection ==
                RtPathTracePureSemanticWorkerReject::BadAlloc &&
            ownerReservePrior.complete && ownerReservePrior.viewIdentity == 0xcafe,
        "S3 moved-input reserve failure preserves output and forbids fallback reuse");
    Check(S3ScratchWasExplicitlyCleared(ownerReserveFailure),
        "S3 failure also leaves the transferred caller source explicitly clear");
}

template<typename T>
bool PodVectorsEqual(const std::vector<T>& lhs, const std::vector<T>& rhs)
{
    return lhs.size() == rhs.size() &&
        (lhs.empty() || std::memcmp(lhs.data(), rhs.data(),
            lhs.size() * sizeof(T)) == 0);
}

void TestProductionDirectBucketMajorParity()
{
    RtPathTraceCaptureOwnerSnapshot snapshot;
    snapshot.capacityCounts.surfaces = 7;
    snapshot.capacityCounts.vertices = 12;
    snapshot.capacityCounts.indexes = 15;
    snapshot.capacityCounts.materialInfoIntents = 1;
    snapshot.capacityCounts.materialVariantProposals = 1;
    snapshot.capacityCounts.instanceObservationProposals = 1;
    snapshot.capacityCounts.rigidCandidateProposals = 1;
    snapshot.capacityCounts.receiptSurfaces = 7;
    snapshot.surfaces.resize(7);
    snapshot.ownerDecisions.resize(7);
    snapshot.ownerDecisionsComplete = true;

    auto setIdentityMatrices = [](RtPathTraceCaptureRawSurface& raw)
    {
        raw.modelMatrix[0] = raw.modelMatrix[5] = raw.modelMatrix[10] =
            raw.modelMatrix[15] = 1.0f;
        raw.bumpMatrix[0] = raw.bumpMatrix[4] = 1.0f;
    };
    auto addTriangleVertices = [&](std::uint32_t ordinal, float x,
                                   bool degenerate)
    {
        RtPathTraceCaptureRawSurface& raw = snapshot.surfaces[ordinal];
        raw.vertexOffset = static_cast<std::uint32_t>(snapshot.vertices.size());
        raw.vertexCount = 3;
        raw.indexOffset = static_cast<std::uint32_t>(snapshot.indexes.size());
        raw.safety = RtPathTraceCaptureSafetyDisposition::Ready;
        setIdentityMatrices(raw);
        idDrawVert a = {}, b = {}, c = {};
        a.xyz.Set(x, 0.0f, 0.0f);
        b.xyz.Set(x + 1.0f, 0.0f, 0.0f);
        c.xyz.Set(x, degenerate ? 0.0f : 1.0f, 0.0f);
        snapshot.vertices.push_back(a);
        snapshot.vertices.push_back(b);
        snapshot.vertices.push_back(c);
        snapshot.indexes.push_back(0);
        snapshot.indexes.push_back(1);
        snapshot.indexes.push_back(2);
        raw.indexCount = 3;
    };
    addTriangleVertices(0, 0.0f, false);
    snapshot.indexes.push_back(0);
    snapshot.indexes.push_back(1);
    snapshot.indexes.push_back(99);
    snapshot.surfaces[0].indexCount = 6;
    addTriangleVertices(1, 10.0f, false);
    addTriangleVertices(5, 20.0f, false);
    addTriangleVertices(6, 30.0f, true);

    const RtSmokeSurfaceClass classes[7] = {
        RtSmokeSurfaceClass::RigidEntity,
        RtSmokeSurfaceClass::ParticleAlpha,
        RtSmokeSurfaceClass::StaticWorld,
        RtSmokeSurfaceClass::RigidEntity,
        RtSmokeSurfaceClass::Unknown,
        RtSmokeSurfaceClass::SkinnedDeformed,
        RtSmokeSurfaceClass::RigidEntity
    };
    for (std::uint32_t ordinal = 0; ordinal < 7; ++ordinal)
    {
        RtPathTraceCaptureRawSurface& raw = snapshot.surfaces[ordinal];
        raw.ordinal = ordinal;
        raw.entityIndex = 3;
        raw.entityNum = 4;
        raw.triIdentityBits = 0x1000u + ordinal;
        std::snprintf(raw.materialName, sizeof(raw.materialName),
            "s2_%u", ordinal);
        RtPathTraceCaptureSurfaceProduct& row = snapshot.ownerDecisions[ordinal];
        row.ordinal = ordinal;
        row.surfaceClass = classes[ordinal];
        row.surfaceClassId = static_cast<std::uint32_t>(classes[ordinal]);
        row.bucketIndex = static_cast<std::int32_t>(classes[ordinal]);
        row.materialId = 100u + ordinal;
        row.materialClassSignature = 200u + ordinal;
        row.instanceId = 300u + ordinal;
        row.vertexCount = raw.vertexCount;
        row.indexCount = raw.indexCount;
        row.triangleCount = raw.indexCount / 3u;
        row.terminal = RtPathTraceCaptureTerminal::Accepted;
    }
    snapshot.ownerDecisions[2].terminal = RtPathTraceCaptureTerminal::StaticMatched;
    snapshot.ownerDecisions[2].staticMatch = true;
    snapshot.ownerDecisions[3].terminal = RtPathTraceCaptureTerminal::RoutedRigidReady;
    snapshot.ownerDecisions[3].rigidReadyByMesh = true;
    snapshot.ownerDecisions[4].terminal = RtPathTraceCaptureTerminal::SemanticRejected;
    snapshot.ownerMembershipReceipt = BuildPathTraceCaptureMembershipReceipt(
        snapshot.ownerDecisions.data(), snapshot.ownerDecisions.size());

    RtPathTraceCaptureProductCapacityPlan plan;
    Check(Plan(snapshot.capacityCounts, plan),
        "direct parity fixture plans one complete product");
    RtPathTraceCaptureProduct direct;
    RtPathTraceCaptureProduct ordinal;
    const std::size_t other = snapshot.OwnedBytes() + plan.oracleBytes;
    const bool reserved = ReservePathTraceCaptureProductStorage(
            snapshot, direct, other, plan.candidateBytes) &&
        ReservePathTraceCaptureProductStorage(
            snapshot, ordinal, other, plan.candidateBytes);
    Check(reserved, "direct parity fixture reserves direct and legacy oracle products");
    if (!reserved)
    {
        return;
    }

    RtPathTracePlanningSnapshotEpoch epoch;
    epoch.generation = epoch.frameIndex = 71;
    epoch.mapTimeStamp = 9;
    epoch.mapLoadSerial = 4;
    RtPathTracePlanningCopyName(epoch.mapName, sizeof(epoch.mapName), "s2-parity");
    epoch.capturedAfterBeginFrame = true;
    epoch.capturedAfterStaticPreload = true;
    epoch.capturedBeforeSerialMutate = true;
    auto stamp = [&](RtPathTraceCaptureProduct& product)
    {
        product.epoch = epoch;
        product.viewIdentity = 72;
        product.lateConsumeToken.mapTimeStamp = epoch.mapTimeStamp;
        product.lateConsumeToken.mapLoadSerial = epoch.mapLoadSerial;
        RtPathTracePlanningCopyName(product.lateConsumeToken.mapName,
            sizeof(product.lateConsumeToken.mapName), epoch.mapName);
        product.lateConsumeToken.configFingerprint = 0x1234u;
        product.lateConsumeToken.capturedAfterBeginFrame = true;
        product.lateConsumeToken.capturedAfterStaticPreload = true;
        product.capacityCounts = snapshot.capacityCounts;
        product.inputOwnedBytes = snapshot.OwnedBytes();
        product.provenance = RtPathTraceCaptureProductProvenance::OwnerDecisions;
        RtPathTraceMaterialInfoRegistrationIntent info;
        std::memset(&info, 0, sizeof(info));
        info.surfaceOrdinal = 0;
        product.materialInfoIntents.push_back(info);
        RtPathTraceMaterialVariantRegistrationProposal variant;
        std::memset(&variant, 0, sizeof(variant));
        variant.surfaceOrdinal = 0;
        product.materialVariants.push_back(variant);
        RtPathTraceInstanceObservationProposal observation;
        std::memset(&observation, 0, sizeof(observation));
        observation.surfaceOrdinal = 0;
        product.instanceObservations.push_back(observation);
        RtPathTraceRigidCandidateProposal rigid;
        std::memset(&rigid, 0, sizeof(rigid));
        rigid.surfaceOrdinal = 0;
        product.rigidCandidates.push_back(rigid);
    };
    stamp(direct);
    stamp(ordinal);

    auto appendReference = [&](std::uint32_t ordinalIndex)
    {
        const RtPathTraceCaptureRawSurface& raw = snapshot.surfaces[ordinalIndex];
        RtPathTraceCaptureSurfaceProduct row = snapshot.ownerDecisions[ordinalIndex];
        row.preAppendVertexOffset = row.vertexOffset =
            static_cast<std::uint32_t>(ordinal.vertices.size());
        row.preAppendIndexOffset = row.indexOffset =
            static_cast<std::uint32_t>(ordinal.indexes.size());
        row.preAppendTriangleOffset = row.triangleOffset =
            static_cast<std::uint32_t>(ordinal.triangleClasses.size());
        RtPathTraceCommittedVertexInput input;
        input.vertices = snapshot.vertices.data() + raw.vertexOffset;
        for (std::uint32_t i = 0; i < raw.vertexCount; ++i)
        {
            ordinal.vertices.push_back(BuildPathTraceCommittedVertexFromOwned(input, i));
        }
        const std::uint32_t vertexBase = row.vertexOffset;
        const std::uint32_t indexBase = row.indexOffset;
        for (std::uint32_t i = 0; i + 2 < raw.indexCount; i += 3)
        {
            const triIndex_t a = snapshot.indexes[raw.indexOffset + i];
            const triIndex_t b = snapshot.indexes[raw.indexOffset + i + 1];
            const triIndex_t c = snapshot.indexes[raw.indexOffset + i + 2];
            if (a >= raw.vertexCount || b >= raw.vertexCount || c >= raw.vertexCount)
            {
                ++row.invalidIndexCount;
                continue;
            }
            if (IsZeroAreaSmokeTriangle(
                    SmokeVertexPosition(ordinal.vertices[vertexBase + a]),
                    SmokeVertexPosition(ordinal.vertices[vertexBase + b]),
                    SmokeVertexPosition(ordinal.vertices[vertexBase + c])))
            {
                ++row.zeroAreaTriangleCount;
                continue;
            }
            ordinal.indexes.push_back(vertexBase + a);
            ordinal.indexes.push_back(vertexBase + b);
            ordinal.indexes.push_back(vertexBase + c);
            ordinal.triangleClasses.push_back(
                BuildPathTraceCaptureTriangleClassWordFromPod(
                    row.surfaceClassId, raw.activeEmissiveStage, false));
            ordinal.triangleMaterials.push_back(row.materialId);
            ordinal.triangleInstances.push_back(4);
            ordinal.triangleIdentities.push_back(
                BuildPathTraceCaptureTriangleIdentityFromPod(
                    raw.entityIndex, raw.entityNum,
                    HashPathTraceMaterialName(raw.materialName),
                    raw.triIdentityBits,
                    static_cast<std::uint32_t>((ordinal.indexes.size() -
                        indexBase) / 3u - 1u)));
        }
        const std::uint32_t emitted = static_cast<std::uint32_t>(
            ordinal.indexes.size()) - indexBase;
        if (emitted == 0)
        {
            ordinal.vertices.resize(vertexBase);
            row.vertexOffset = row.vertexCount = 0;
            row.indexOffset = row.indexCount = 0;
            row.triangleOffset = row.triangleCount = 0;
            row.preAppendVertexOffset = 0;
            row.preAppendIndexOffset = 0;
            row.preAppendTriangleOffset = 0;
            row.terminal = RtPathTraceCaptureTerminal::RolledBack;
            row.rollbackApplied = true;
            row.capturedRecordPresent = false;
            row.skinnedRecordPresent = false;
            row.mergedWalkRecordPresent = false;
            row.bucketRangePublished = false;
        }
        else
        {
            row.vertexCount = raw.vertexCount;
            row.indexCount = emitted;
            row.triangleCount = emitted / 3u;
            row.decisionPresence |= RT_PT_CAPTURE_DECISION_CAPTURED_RECORD |
                RT_PT_CAPTURE_DECISION_BUCKET_PUBLICATION;
            row.capturedRecordPresent = row.bucketRangePublished = true;
            row.capturedRecordVertexCount = row.vertexCount;
            row.capturedRecordIndexCount = row.indexCount;
            row.capturedRecordTriangleCount = row.triangleCount;
            if (row.bucketIndex >= 1)
            {
                row.decisionPresence |= RT_PT_CAPTURE_DECISION_MERGED_WALK;
                row.mergedWalkRecordPresent = true;
                row.mergedWalkVertexCount = row.vertexCount;
                row.mergedWalkIndexCount = row.indexCount;
                row.mergedWalkTriangleCount = row.triangleCount;
                if (row.surfaceClass == RtSmokeSurfaceClass::RigidEntity)
                {
                    row.mergedCompanionRigidId = row.instanceId;
                }
            }
            if (row.surfaceClass == RtSmokeSurfaceClass::SkinnedDeformed)
            {
                row.decisionPresence |= RT_PT_CAPTURE_DECISION_SKINNED_RECORD;
                row.skinnedRecordPresent = true;
                row.skinnedRecordVertexCount = row.vertexCount;
                row.skinnedRecordIndexCount = row.indexCount;
                row.skinnedRecordTriangleCount = row.triangleCount;
            }
        }
        ordinal.surfaces[ordinalIndex] = row;
    };
    ordinal.surfaces = snapshot.ownerDecisions;
    appendReference(0);
    appendReference(1);
    appendReference(5);
    appendReference(6);
    ordinal.membershipReceipt = snapshot.ownerMembershipReceipt;
    ordinal.ownerDecisionGeometryOnly = true;
    ordinal.staticMembershipSurfaces.push_back(2);
    ordinal.routedReadySkipSurfaces.push_back(3);

    const bool directBuilt = BuildPathTraceGeometryFromOwnerDecisions(
        snapshot, direct);
    RtPathTraceCaptureProduct partitioned;
    const bool oracleReserved = ReserveFinal(
        snapshot, plan, ordinal, partitioned);
    const bool oracleBuilt = oracleReserved &&
        PartitionPathTraceCaptureProductBucketMajor(snapshot, ordinal, partitioned);
    Check(directBuilt, "actual production direct owner-decision builder completes");
    Check(oracleReserved, "retained partition destination reserves");
    Check(oracleBuilt, "retained candidate+partition oracle completes");
    Check(directBuilt && oracleBuilt,
        "actual production direct builder and retained partition oracle both complete");
    if (!directBuilt || !oracleBuilt)
    {
        return;
    }
    direct.complete = true;
    direct.actualOwnedBytes = direct.OwnedBytes();
    partitioned.provenance = RtPathTraceCaptureProductProvenance::OwnerDecisions;
    partitioned.capacityCounts = snapshot.capacityCounts;
    partitioned.inputOwnedBytes = snapshot.OwnedBytes();
    partitioned.actualOwnedBytes = partitioned.OwnedBytes();
    RtPathTraceCaptureOracle oracle;
    oracle.epoch = partitioned.epoch;
    oracle.viewIdentity = partitioned.viewIdentity;
    oracle.vertices = partitioned.vertices;
    oracle.indexes = partitioned.indexes;
    oracle.triangleClasses = partitioned.triangleClasses;
    oracle.triangleMaterials = partitioned.triangleMaterials;
    oracle.triangleInstances = partitioned.triangleInstances;
    oracle.triangleIdentities = partitioned.triangleIdentities;
    oracle.materialInfoIntents = partitioned.materialInfoIntents;
    oracle.materialVariants = partitioned.materialVariants;
    oracle.instanceObservations = partitioned.instanceObservations;
    oracle.rigidCandidates = partitioned.rigidCandidates;
    oracle.staticMembershipSurfaces = partitioned.staticMembershipSurfaces;
    oracle.routedReadySkipSurfaces = partitioned.routedReadySkipSurfaces;
    oracle.surfaces = partitioned.surfaces;
    oracle.complete = true;
    const RtPathTraceCaptureComparison comparison =
        ComparePathTraceCaptureProduct(direct, oracle);
    Check(comparison.exact,
        "direct geometry/decision arrays match the retained partition oracle");
    const bool proposalsExact =
        PodVectorsEqual(direct.materialInfoIntents,
            partitioned.materialInfoIntents) &&
        PodVectorsEqual(direct.materialVariants,
            partitioned.materialVariants) &&
        PodVectorsEqual(direct.instanceObservations,
            partitioned.instanceObservations) &&
        PodVectorsEqual(direct.rigidCandidates,
            partitioned.rigidCandidates);
    Check(proposalsExact, "direct parity preserves all four proposal families");
    const bool authorityExact = direct.staticMembershipSurfaces ==
            partitioned.staticMembershipSurfaces &&
        direct.routedReadySkipSurfaces ==
            partitioned.routedReadySkipSurfaces &&
        std::memcmp(&direct.capacityCounts, &partitioned.capacityCounts,
            sizeof(direct.capacityCounts)) == 0 &&
        std::memcmp(&direct.epoch, &partitioned.epoch,
            sizeof(direct.epoch)) == 0 &&
        std::memcmp(&direct.lateConsumeToken, &partitioned.lateConsumeToken,
            sizeof(direct.lateConsumeToken)) == 0 &&
        PathTraceCaptureMembershipReceiptsMatch(direct.membershipReceipt,
            partitioned.membershipReceipt) &&
        direct.viewIdentity == partitioned.viewIdentity &&
        direct.ownerDecisionGeometryOnly ==
            partitioned.ownerDecisionGeometryOnly &&
        direct.provenance == partitioned.provenance &&
        direct.inputOwnedBytes == partitioned.inputOwnedBytes &&
        direct.actualOwnedBytes == partitioned.actualOwnedBytes &&
        direct.OwnedBytes() == partitioned.OwnedBytes();
    if (!authorityExact)
    {
        std::cout << "[DIAG] direct/partition authority: input="
            << direct.inputOwnedBytes << "/" << partitioned.inputOwnedBytes
            << " actual=" << direct.actualOwnedBytes << "/"
            << partitioned.actualOwnedBytes << " owned=" << direct.OwnedBytes()
            << "/" << partitioned.OwnedBytes() << " provenance="
            << static_cast<int>(direct.provenance) << "/"
            << static_cast<int>(partitioned.provenance) << "\n";
    }
    Check(authorityExact,
        "direct parity preserves membership, provenance and owned-byte authority");
    Check(comparison.exact &&
            proposalsExact && authorityExact &&
            PathTraceCaptureProductSpansCanonical(direct),
        "production direct output is byte/field exact to candidate+partition oracle");
    Check(direct.surfaces[0].invalidIndexCount == 1 &&
            direct.surfaces[6].terminal == RtPathTraceCaptureTerminal::RolledBack &&
            direct.surfaces[6].zeroAreaTriangleCount == 1 &&
            direct.surfaces[5].skinnedRecordPresent &&
            direct.surfaces[5].mergedWalkRecordPresent &&
            direct.surfaces[0].mergedCompanionRigidId ==
                direct.surfaces[0].instanceId,
        "direct parity fixture covers invalid, zero-area, skinned, rigid and merged decisions");
}

RtPathTraceCaptureOwnerSnapshot MakeLiveOwnerPreparedPayloadSnapshot()
{
    RtPathTraceCaptureOwnerSnapshot snapshot;
    snapshot.capacityCounts.surfaces = 3;
    snapshot.capacityCounts.vertices = 9;
    snapshot.capacityCounts.indexes = 18;
    snapshot.capacityCounts.rigidCandidateProposals = 3;
    snapshot.capacityCounts.preparedRigidPayloads = 3;
    snapshot.capacityCounts.preparedRigidVertices = 9;
    snapshot.capacityCounts.preparedRigidIndexes = 18;
    snapshot.capacityCounts.receiptSurfaces = 3;
    snapshot.epoch.generation = 81;
    snapshot.epoch.frameIndex = 82;
    snapshot.epoch.mapTimeStamp = 83;
    snapshot.epoch.mapLoadSerial = 84;
    RtPathTracePlanningCopyName(snapshot.epoch.mapName,
        sizeof(snapshot.epoch.mapName), "live-owner-prepared");
    snapshot.epoch.capturedAfterBeginFrame = true;
    snapshot.epoch.capturedAfterStaticPreload = true;
    snapshot.epoch.capturedBeforeSerialMutate = true;
    snapshot.instanceUniverse.epoch = snapshot.epoch;
    snapshot.instanceUniverse.complete = true;
    snapshot.geometryUniverse.epoch = snapshot.epoch;
    snapshot.geometryUniverse.complete = true;
    snapshot.viewIdentity = 85;
    snapshot.surfaces.resize(3);
    snapshot.ownerDecisions.resize(3);
    for (std::uint32_t ordinal = 0; ordinal < 3; ++ordinal)
    {
        RtPathTraceCaptureRawSurface& raw = snapshot.surfaces[ordinal];
        raw.ordinal = ordinal;
        raw.entityIndex = 10 + static_cast<std::int32_t>(ordinal);
        raw.entityNum = 20 + static_cast<std::int32_t>(ordinal);
        raw.vertexOffset = static_cast<std::uint32_t>(snapshot.vertices.size());
        raw.vertexCount = 3;
        raw.indexOffset = static_cast<std::uint32_t>(snapshot.indexes.size());
        raw.indexCount = ordinal == 0 ? 3u : 6u;
        raw.sourceTriIndexCount = 6;
        raw.triNumVerts = 3;
        raw.triNumIndexes = 6;
        raw.triTriangleCount = 2;
        raw.activeEmissiveStage = ordinal == 2;
        raw.safety = RtPathTraceCaptureSafetyDisposition::Ready;
        raw.modelMatrix[0] = raw.modelMatrix[5] = raw.modelMatrix[10] =
            raw.modelMatrix[15] = 1.0f;
        raw.bumpMatrix[0] = raw.bumpMatrix[4] = 1.0f;
        RtPathTracePlanningCopyName(raw.materialName,
            sizeof(raw.materialName), "models/live/rigid");
        RtPathTracePlanningCopyName(raw.modelName,
            sizeof(raw.modelName), "models/live/rigid.lwo");
        idDrawVert a = {}, b = {}, c = {};
        const float x = ordinal < 2 ? 0.0f : 4.0f;
        a.xyz.Set(x, 0.0f, 0.0f);
        b.xyz.Set(x + 1.0f, 0.0f, 0.0f);
        c.xyz.Set(x, 1.0f, 0.0f);
        snapshot.vertices.push_back(a);
        snapshot.vertices.push_back(b);
        snapshot.vertices.push_back(c);
        snapshot.indexes.push_back(0);
        snapshot.indexes.push_back(1);
        snapshot.indexes.push_back(2);
        snapshot.indexes.push_back(0);
        snapshot.indexes.push_back(2);
        snapshot.indexes.push_back(1);

        RtPathTraceCaptureSurfaceProduct& surface =
            snapshot.ownerDecisions[ordinal];
        surface.ordinal = ordinal;
        surface.terminal = RtPathTraceCaptureTerminal::Accepted;
        surface.surfaceClass = RtSmokeSurfaceClass::RigidEntity;
        surface.surfaceClassId = static_cast<std::uint32_t>(
            RtSmokeSurfaceClass::RigidEntity);
        surface.bucketIndex = static_cast<std::int32_t>(
            RtSmokeSurfaceClass::RigidEntity);
        surface.vertexCount = 3;
        surface.indexCount = raw.indexCount;
        surface.triangleCount = raw.indexCount / 3u;
        surface.meshHash = ordinal < 2
            ? 0xabcddcba1234ull : 0xabcddcba5678ull;
        surface.instanceId = 0x1000u + ordinal;
        surface.materialId = 0x4321u;
        surface.materialClassSignature = 0x5678u;
        surface.sourceFlags = RT_PT_INSTANCE_SOURCE_RIGID;
    }
    snapshot.ownerMembershipReceipt = BuildPathTraceCaptureMembershipReceipt(
        snapshot.ownerDecisions.data(), snapshot.ownerDecisions.size());
    snapshot.ownerDecisionsComplete = true;
    snapshot.complete = true;
    return snapshot;
}

void TestLiveOwnerPreparedPayloadProduction()
{
    const RtPathTraceCaptureOwnerSnapshot snapshot =
        MakeLiveOwnerPreparedPayloadSnapshot();
    RtPathTraceCaptureProduct product;
    Check(BuildPathTraceCaptureProduct(snapshot, product) &&
            product.provenance ==
                RtPathTraceCaptureProductProvenance::OwnerDecisions &&
            product.rigidCandidates.size() == 3 &&
            product.preparedRigidPayloads.size() == 2 &&
            product.preparedRigidPayloads[0].occurrenceCount == 2 &&
            product.preparedRigidPayloads[0].localVertices.size() == 3 &&
            product.preparedRigidPayloads[0].localIndexes.size() == 6 &&
            product.indexes.size() == 15 && snapshot.indexes.size() == 18 &&
            snapshot.surfaces[0].indexCount == 3 &&
            snapshot.surfaces[0].sourceTriIndexCount == 6 &&
            product.capacityCounts.indexes == 18 &&
            product.capacityCounts.preparedRigidIndexes == 18,
        "live OwnerDecisions builder preserves partial draw output and emits canonical full-triangle cache");
    Check(product.preparedRigidPayloads.size() == 2 &&
            (product.preparedRigidPayloads[0].triangleClassAndFlags &
                RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) != 0u &&
            (product.preparedRigidPayloads[1].triangleClassAndFlags &
                RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) == 0u,
        "live prepared payload class flags match inactive and active emissive serial semantics");
    Check(RtPathTraceCaptureProductShapeValid(product) &&
            !product.receiptHeader.complete && product.receiptSurfaces.empty() &&
            product.materialInfoIntents.empty() &&
            product.materialVariants.empty() &&
            product.instanceObservations.empty(),
        "live OwnerDecisions prepared product seals without PureSemantic receipt/proposal authority");

    RtPathTraceCaptureProduct tampered = product;
    tampered.preparedRigidPayloads[0].localIndexes[0] = 99;
    tampered.actualOwnedBytes = tampered.OwnedBytes();
    Check(!RtPathTraceCaptureProductShapeValid(tampered),
        "live OwnerDecisions shape rejects canonical prepared-payload tamper");
    tampered = product;
    --tampered.preparedRigidPayloads[0].occurrenceCount;
    tampered.actualOwnedBytes = tampered.OwnedBytes();
    Check(!RtPathTraceCaptureProductShapeValid(tampered),
        "live OwnerDecisions shape rejects duplicate occurrence mismatch");
    tampered = product;
    tampered.preparedRigidPayloads.clear();
    tampered.actualOwnedBytes = tampered.OwnedBytes();
    Check(!RtPathTraceCaptureProductShapeValid(tampered),
        "live OwnerDecisions shape rejects partial prepared-payload publication");

    RtPathTraceCaptureOwnerSnapshot capped =
        MakeLiveOwnerPreparedPayloadSnapshot();
    capped.capacityCounts.preparedRigidVertices = 2;
    RtPathTraceCaptureProduct prior;
    prior.complete = true;
    prior.viewIdentity = 0xfeedu;
    Check(!BuildPathTraceCaptureProduct(capped, prior) &&
            prior.complete && prior.viewIdentity == 0xfeedu,
        "live OwnerDecisions nested prepared-vertex cap rejects atomically");

    RtPathTraceCaptureOwnerSnapshot invalid =
        MakeLiveOwnerPreparedPayloadSnapshot();
    invalid.indexes[0] = 99;
    prior = RtPathTraceCaptureProduct{};
    prior.complete = true;
    prior.viewIdentity = 0xbeefu;
    Check(!BuildPathTraceCaptureProduct(invalid, prior) &&
            prior.complete && prior.viewIdentity == 0xbeefu,
        "live OwnerDecisions prepared-cache failure preserves prior output atomically");
}
} // namespace

int main()
{
    const std::size_t cap = RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES;
    {
        RtPathTraceLateConsumeToken first;
        first.mapTimeStamp = 17;
        first.mapLoadSerial = 3;
        RtPathTracePlanningCopyName(first.mapName, sizeof(first.mapName), "late");
        first.registryGeneration = 4;
        first.instanceUniverseGeneration = 5;
        first.geometryUniverseGeneration = 6;
        first.configFingerprint = 7;
        first.capturedAfterBeginFrame = true;
        first.capturedAfterStaticPreload = true;
        RtPathTraceLateConsumeToken second = first;
        Check(CompatibleForLateConsume(first, second),
            "late-consume token accepts identical pointer-free world and config state");
        ++second.registryGeneration;
        Check(CompatibleForLateConsume(first, second),
            "late-consume token ignores per-frame registry owner revisions");
        second = first;
        ++second.geometryUniverseGeneration;
        Check(CompatibleForLateConsume(first, second),
            "late-consume token ignores per-frame universe owner revisions");
        second = first;
        ++second.configFingerprint;
        Check(!CompatibleForLateConsume(first, second),
            "late-consume token rejects consume-relevant configuration changes");

        std::vector<RtPathTraceCaptureSurfaceProduct> surfaces(2);
        surfaces[0].ordinal = 4;
        surfaces[0].terminal = RtPathTraceCaptureTerminal::Accepted;
        surfaces[0].surfaceClass = RtSmokeSurfaceClass::RigidEntity;
        surfaces[0].materialId = 11;
        surfaces[0].instanceId = 12;
        surfaces[0].meshHash = 13;
        surfaces[0].vertexCount = 3;
        surfaces[0].indexCount = 3;
        surfaces[1].ordinal = 9;
        surfaces[1].terminal = RtPathTraceCaptureTerminal::AdmissionRejected;
        surfaces[1].surfaceClass = RtSmokeSurfaceClass::SkinnedDeformed;
        surfaces[1].skinnedCaptureOmitted = true;
        surfaces[1].skinnedAdmission = static_cast<std::uint32_t>(
            PtSkinnedCaptureAdmissionResult::OmitCpuCapture);
        const RtPathTraceCaptureMembershipReceipt receipt =
            BuildPathTraceCaptureMembershipReceipt(
                surfaces.data(), surfaces.size());
        Check(receipt.complete && receipt.surfaceCount == 2 &&
                receipt.skinnedSurfaceCount == 1 &&
                receipt.cpuSkinnedAcceptedCount == 0 &&
                PathTraceCaptureMembershipReceiptsMatch(receipt, receipt),
            "membership receipt covers ordered admission, material, identity, readiness and counts");
        auto changed = surfaces;
        ++changed[0].materialId;
        Check(!PathTraceCaptureMembershipReceiptsMatch(receipt,
                BuildPathTraceCaptureMembershipReceipt(
                    changed.data(), changed.size())),
            "membership receipt rejects a material decision change");
        changed = surfaces;
        changed[0].rigidReadyByMesh = true;
        Check(!PathTraceCaptureMembershipReceiptsMatch(receipt,
                BuildPathTraceCaptureMembershipReceipt(
                    changed.data(), changed.size())),
            "membership receipt rejects a rigid-readiness decision change");
        changed = surfaces;
        changed[1].terminal = RtPathTraceCaptureTerminal::Accepted;
        changed[1].skinnedCaptureOmitted = false;
        changed[1].vertexCount = 3;
        Check(BuildPathTraceCaptureMembershipReceipt(
                changed.data(), changed.size())
                .cpuSkinnedAcceptedCount == 1,
            "membership receipt marks cross-generation CPU-skinned geometry ineligible");
    }
    {
        RtPathTraceCaptureCapacityCounts empty;
        RtPathTraceCaptureProductCapacityPlan plan;
        const std::size_t fixedProducts = sizeof(RtPathTraceCaptureProduct) +
            sizeof(RtPathTraceCaptureOracle);
        Check(PlanPathTraceCompleteSlotCapacity(empty, cap - fixedProducts, plan) &&
                plan.peakSlotBytes == cap,
            "concrete real-layout preflight accepts exactly-at-cap before allocation");
        Check(!PlanPathTraceCompleteSlotCapacity(empty,
                cap - fixedProducts + 1, plan),
            "concrete real-layout preflight rejects over-cap before allocation");
    }
    {
        RtPathTraceCaptureCapacityCounts empty;
        RtPathTraceCaptureProductCapacityPlan baseline;
        Check(Plan(empty, baseline), "concrete empty layout plans");
        struct DeltaCase
        {
            const char* name;
            void (*set)(RtPathTraceCaptureCapacityCounts&);
            std::size_t expected;
        };
        const DeltaCase cases[] = {
            { "surface", [](RtPathTraceCaptureCapacityCounts& c) {
                    c.surfaces = 1;
                    c.receiptSurfaces = 1;
                    c.materialInfoIntents = 1;
                    c.materialVariantProposals = 1;
                    c.instanceObservationProposals = 1;
                    c.rigidCandidateProposals = 1;
                },
                sizeof(RtPathTraceCaptureRawSurface) +
                    sizeof(RtPathTraceCaptureSurfaceProduct) +
                    ProductPerSurfaceBytes() +
                    OraclePerSurfaceBytes() },
            { "stage", [](RtPathTraceCaptureCapacityCounts& c) { c.classifierStages = 1; },
                sizeof(RtSmokeTranslucentClassifierStageInput) },
            { "runtime stage", [](RtPathTraceCaptureCapacityCounts& c) { c.runtimeStages = 1; },
                sizeof(RtPathTraceRuntimeMaterialStagePod) },
            { "register", [](RtPathTraceCaptureCapacityCounts& c) { c.registers = 1; }, sizeof(float) },
            { "vertex", [](RtPathTraceCaptureCapacityCounts& c) { c.vertices = 1; },
                sizeof(idDrawVert) + 2 * sizeof(PathTraceSmokeVertex) },
            { "index+triangle metadata", [](RtPathTraceCaptureCapacityCounts& c) { c.indexes = 3; },
                3 * sizeof(triIndex_t) + 6 * sizeof(std::uint32_t) +
                    2 * (3 * sizeof(std::uint32_t) + sizeof(std::uint64_t)) },
            { "joint", [](RtPathTraceCaptureCapacityCounts& c) { c.joints = 1; }, sizeof(idJointMat) },
            { "variant base", [](RtPathTraceCaptureCapacityCounts& c) { c.variantBases = 1; },
                sizeof(RtPathTraceMaterialTextureVariantBasePod) },
            { "registry material", [](RtPathTraceCaptureCapacityCounts& c) { c.registryMaterials = 1; },
                sizeof(RtPathTraceCaptureRegistryMaterialPod) },
            { "model token table", [](RtPathTraceCaptureCapacityCounts& c) { c.modelTables = 1; },
                sizeof(RtPathTraceCaptureModelTokenTablePod) },
            { "model surface token", [](RtPathTraceCaptureCapacityCounts& c) { c.modelSurfaceTokens = 1; },
                sizeof(std::uint64_t) },
            { "apply gate", [](RtPathTraceCaptureCapacityCounts& c) { c.applyGateKeys = 1; },
                sizeof(std::uint64_t) },
            { "instance mesh", [](RtPathTraceCaptureCapacityCounts& c) { c.instanceMeshes = 1; },
                sizeof(RtPathTraceInstanceMeshRecordPod) },
            { "instance history", [](RtPathTraceCaptureCapacityCounts& c) { c.instanceHistories = 1; },
                sizeof(RtPathTraceInstanceHistoryPod) },
            { "geometry static", [](RtPathTraceCaptureCapacityCounts& c) { c.geometryStaticSurfaces = 1; },
                sizeof(RtSmokeStaticSurfacePod) },
            { "geometry route", [](RtPathTraceCaptureCapacityCounts& c) { c.geometryRigidRoutes = 1; },
                sizeof(RtSmokeRigidRouteReadyPod) },
            { "geometry resident", [](RtPathTraceCaptureCapacityCounts& c) { c.geometryRigidResidents = 1; },
                sizeof(RtSmokeRigidResidentReadyPod) },
            { "prepared rigid payload", [](RtPathTraceCaptureCapacityCounts& c) {
                    c.preparedRigidPayloads = 1;
                }, sizeof(RtPathTraceRigidPreparedPayload) },
            { "prepared rigid vertex", [](RtPathTraceCaptureCapacityCounts& c) {
                    c.preparedRigidVertices = 1;
                }, sizeof(PathTraceSmokeVertex) },
            { "prepared rigid index", [](RtPathTraceCaptureCapacityCounts& c) {
                    c.preparedRigidIndexes = 1;
                }, sizeof(std::uint32_t) }
        };
        for (const DeltaCase& test : cases)
        {
            RtPathTraceCaptureCapacityCounts count;
            test.set(count);
            RtPathTraceCaptureProductCapacityPlan mapped;
            Check(Plan(count, mapped) &&
                    mapped.peakSlotBytes - baseline.peakSlotBytes == test.expected,
                test.name);
        }
    }

    RtPathTraceCaptureOwnerSnapshot snapshot;
    snapshot.capacityCounts = AllCounts();
    RtPathTraceCaptureProductCapacityPlan plan;
    Check(Plan(snapshot.capacityCounts, plan), "full concrete transaction plans");
    Check(plan.finalProductBytes == 0 &&
            plan.peakSlotBytes == plan.snapshotBytes +
                plan.candidateBytes + plan.oracleBytes,
        "complete-slot peak carries exactly one final geometry product");
    {
        RtPathTraceCaptureReserveTestSeam seam;
        Check(ReservePathTraceCaptureOwnerSnapshotStorage(snapshot, 0, plan, &seam) &&
                seam.reserveCalls == 18,
            "owner transaction reserves every snapshot family exactly once");
    }
    {
        RtPathTraceCaptureOwnerSnapshot handoff;
        handoff.capacityCounts.surfaces = 1;
        RtPathTraceCaptureProductCapacityPlan handoffPlan;
        const bool planned = Plan(handoff.capacityCounts, handoffPlan);
        const bool reserved = planned &&
            ReservePathTraceCaptureOwnerSnapshotStorage(
                handoff, 0, handoffPlan, nullptr);
        if (reserved)
        {
            handoff.complete = true;
            RtPathTraceCaptureRawSurface raw;
            raw.ordinal = 0;
            raw.rigidReadyByMesh = false;
            handoff.surfaces.push_back(raw);
            RtPathTraceCaptureSurfaceProduct authoritative;
            authoritative.ordinal = 0;
            authoritative.terminal =
                RtPathTraceCaptureTerminal::RoutedRigidReady;
            authoritative.surfaceClass = RtSmokeSurfaceClass::RigidEntity;
            authoritative.rigidReadyByMesh = true;
            const RtPathTraceCaptureMembershipReceipt receipt =
                BuildPathTraceCaptureMembershipReceipt(&authoritative, 1);
            Check(AttachPathTraceOwnerDecisionTable(
                    handoff, &authoritative, 1, receipt) &&
                    handoff.ownerDecisionsComplete &&
                    handoff.ownerDecisions[0].rigidReadyByMesh &&
                    handoff.ownerDecisions[0].terminal ==
                        RtPathTraceCaptureTerminal::RoutedRigidReady &&
                    !PathTraceOwnerDecisionEmitsGeometry(
                        handoff.ownerDecisions[0]),
                "Owner Harvest readiness supersedes the pre-Harvest raw universe fact");
        }
        else
        {
            Check(false, "Owner Harvest decision handoff storage plans");
        }
    }
    {
        for (std::size_t omitted = 0; omitted < 18; ++omitted)
        {
            RtPathTraceCaptureOwnerSnapshot value;
            value.capacityCounts = AllCounts();
            RtPathTraceCaptureReserveTestSeam seam;
            seam.skipAtReserve = omitted;
            Check(!ReservePathTraceCaptureOwnerSnapshotStorage(value, 0, plan, &seam) &&
                    value.OwnedBytes() == sizeof(value),
                "omitting any snapshot reserve fails closed and releases backing");
        }
    }
    {
        RtPathTraceCaptureOwnerSnapshot value;
        value.capacityCounts = AllCounts();
        RtPathTraceCaptureReserveTestSeam badAlloc;
        badAlloc.failAtReserve = 4;
        badAlloc.failure = RtPathTraceCaptureReserveFailure::BadAlloc;
        Check(!ReservePathTraceCaptureOwnerSnapshotStorage(value, 0, plan, &badAlloc) &&
                value.OwnedBytes() == sizeof(value),
            "real snapshot reserve bad_alloc releases all unpublished backing");
    }
    {
        RtPathTraceCaptureProduct value;
        RtPathTraceCaptureReserveTestSeam length;
        length.failAtReserve = 5;
        length.failure = RtPathTraceCaptureReserveFailure::LengthError;
        Check(!ReservePathTraceCaptureProductStorage(snapshot, value,
                snapshot.OwnedBytes() + plan.oracleBytes,
                plan.candidateBytes, &length) &&
                value.OwnedBytes() == sizeof(value),
            "real product reserve length_error releases all candidate backing");
    }
    {
        RtPathTraceCaptureProduct value;
        RtPathTraceCaptureReserveTestSeam inflated;
        inflated.inflateAtReserve = 0;
        inflated.inflateBy = 1;
        Check(!ReservePathTraceCaptureProductStorage(snapshot, value,
                snapshot.OwnedBytes() + plan.oracleBytes,
                plan.candidateBytes, &inflated) &&
                inflated.reserveCalls == 1 && value.OwnedBytes() == sizeof(value),
            "actual over-cap allocator capacity is rejected before the next reserve");
    }
    {
        RtPathTraceCaptureProduct value;
        RtPathTraceCaptureReserveTestSeam receiptInflation;
        receiptInflation.inflateAtReserve = 13;
        receiptInflation.inflateBy = 1;
        const std::size_t slackBudget = plan.candidateBytes + 4096u;
        Check(ReservePathTraceCaptureProductStorage(snapshot, value,
                snapshot.OwnedBytes() + plan.oracleBytes,
                slackBudget, &receiptInflation) &&
                receiptInflation.reserveCalls == 16,
            "non-breaching receipt reserve inflation remains within the one product plan");

        RtPathTraceCaptureReserveTestSeam receiptBreach;
        receiptBreach.inflateAtReserve = 13;
        receiptBreach.inflateBy = 1;
        RtPathTraceCaptureProduct breachedValue;
        Check(!ReservePathTraceCaptureProductStorage(snapshot, breachedValue,
                snapshot.OwnedBytes() + plan.oracleBytes,
                plan.candidateBytes, &receiptBreach) &&
                receiptBreach.reserveCalls == 14 &&
                breachedValue.OwnedBytes() == sizeof(breachedValue),
            "receipt reserve cap breach short-circuits before membership reserves");
    }
    {
        RtPathTraceCaptureProduct value;
        RtPathTraceCaptureReserveTestSeam seam;
        Check(ReservePathTraceCaptureProductStorage(snapshot, value,
                snapshot.OwnedBytes() + plan.oracleBytes,
                plan.candidateBytes, &seam) && seam.reserveCalls == 16,
            "candidate transaction reserves every product family exactly once");
        const RtPathTraceCaptureProductCapacityStamp stamp =
            CapturePathTraceProductCapacityStamp(value);
        for (std::size_t i = 0; i <= stamp.capacities[0]; ++i)
        {
            value.surfaces.push_back({});
        }
        Check(!PathTraceCaptureProductCapacityUnchanged(value, stamp),
            "candidate fill guard rejects any capacity growth before publication");
        value.ResetAndRelease();
        for (std::size_t omitted = 0; omitted < 16; ++omitted)
        {
            RtPathTraceCaptureProduct skipped;
            RtPathTraceCaptureReserveTestSeam omission;
            omission.skipAtReserve = omitted;
            const bool reserved = ReservePathTraceCaptureProductStorage(snapshot, skipped,
                snapshot.OwnedBytes() + plan.oracleBytes,
                plan.candidateBytes, &omission);
            const bool omittedUnusedFrameMaterialIds = omitted == 12u &&
                snapshot.capacityCounts.frameMaterialIds == 0u;
            Check(omittedUnusedFrameMaterialIds
                    ? reserved
                    : (!reserved && skipped.OwnedBytes() == sizeof(skipped)),
                omittedUnusedFrameMaterialIds
                    ? "omitting the unused legacy frame-material reserve is benign"
                    : "omitting any required product reserve fails before candidate fill");
        }
    }
    {
        RtPathTraceCaptureOracle oracle;
        RtPathTraceCaptureReserveTestSeam seam;
        Check(ReservePathTraceCaptureOracleStorage(snapshot.capacityCounts,
                snapshot.OwnedBytes() + plan.candidateBytes + plan.finalProductBytes,
                oracle, &seam) && seam.reserveCalls == 13,
            "oracle transaction reserves every oracle family exactly once");
        oracle.ResetAndRelease();
        RtPathTraceCaptureReserveTestSeam badAlloc;
        badAlloc.failAtReserve = 7;
        badAlloc.failure = RtPathTraceCaptureReserveFailure::BadAlloc;
        Check(!ReservePathTraceCaptureOracleStorage(snapshot.capacityCounts,
                snapshot.OwnedBytes() + plan.candidateBytes + plan.finalProductBytes,
                oracle, &badAlloc) && oracle.OwnedBytes() == sizeof(oracle),
            "real oracle reserve bad_alloc releases all oracle backing");
    }
    TestUnifiedCaptureProductShape();
    TestPureSemanticFrameMaterialIds();
    TestPureSemanticWorkerPipelineS3();
    TestProductionDirectBucketMajorParity();
    TestLiveOwnerPreparedPayloadProduction();
    {
        RtPathTraceCaptureProduct candidate = MakeCandidate(snapshot, plan);
        RtPathTraceCaptureProduct output;
        Check(PathTraceCaptureProductStorageReady(snapshot, candidate) &&
                ReserveFinal(snapshot, plan, candidate, output) &&
                PartitionPathTraceCaptureProductBucketMajor(snapshot, candidate, output),
            "real one-candidate bucket-major partition completes without growth");
        Check(PathTraceCaptureProductSpansCanonical(output),
            "post-partition validator accounts bucket zero and all dynamic buckets");
        const float expectedPositions[9] = { 3, 4, 5, 6, 7, 8, 0, 1, 2 };
        bool verticesExact = output.vertices.size() == 9;
        bool indexesExact = output.indexes.size() == 9;
        for (std::size_t i = 0; i < 9 && verticesExact && indexesExact; ++i)
        {
            verticesExact = output.vertices[i].position[0] == expectedPositions[i];
            indexesExact = output.indexes[i] == i;
        }
        Check(verticesExact && indexesExact,
            "partition preserves exact vertex order and rebases every index");
        Check(output.triangleClasses == std::vector<std::uint32_t>({ 1001, 1002, 1000 }) &&
                output.triangleMaterials == std::vector<std::uint32_t>({ 2001, 2002, 2000 }) &&
                output.triangleInstances == std::vector<std::uint64_t>({ 3001, 3002, 3000 }) &&
                output.triangleIdentities == std::vector<std::uint32_t>({ 4001, 4002, 4000 }),
            "partition keeps all triangle metadata aligned in bucket-major order");
        Check(output.surfaces.size() == 3 &&
                output.surfaces[0].vertexOffset == 6 &&
                output.surfaces[1].vertexOffset == 0 &&
                output.surfaces[2].vertexOffset == 3 &&
                output.surfaces[0].indexOffset == 6 &&
                output.surfaces[1].indexOffset == 0 &&
                output.surfaces[2].indexOffset == 3 &&
                output.surfaces[0].triangleOffset == 2 &&
                output.surfaces[1].triangleOffset == 0 &&
                output.surfaces[2].triangleOffset == 1,
            "partition rewrites every destination surface offset exactly");
        Check(output.materialInfoIntents.size() == 3 &&
                output.materialVariants.size() == 3 &&
                output.instanceObservations.size() == 3 &&
                output.rigidCandidates.size() == 3 &&
                output.materialInfoIntents[2].surfaceOrdinal == 2 &&
                output.materialVariants[1].chosenId == 5001 &&
                output.instanceObservations[0].instanceId == 6000 &&
                output.rigidCandidates[2].meshHash == 7002 &&
                output.staticMembershipSurfaces == std::vector<std::uint32_t>({ 2, 0 }) &&
                output.routedReadySkipSurfaces == std::vector<std::uint32_t>({ 1 }),
            "partition copies every proposal and membership family in source order");

        for (int family = 0; family < 4; ++family)
        {
            RtPathTraceCaptureProduct corrupt = MakeCandidate(snapshot, plan);
            if (family == 0) corrupt.triangleClasses.pop_back();
            if (family == 1) corrupt.triangleMaterials.pop_back();
            if (family == 2) corrupt.triangleInstances.pop_back();
            if (family == 3) corrupt.triangleIdentities.pop_back();
            RtPathTraceCaptureProduct rejected;
            const bool reserved = ReserveFinal(snapshot, plan, corrupt, rejected);
            Check(reserved &&
                    !PartitionPathTraceCaptureProductBucketMajor(snapshot, corrupt, rejected),
                "omitting any triangle metadata family rejects partition publication");
        }
        RtPathTraceCaptureProduct corruptOffset = MakeCandidate(snapshot, plan);
        corruptOffset.surfaces[1].indexOffset = 99;
        RtPathTraceCaptureProduct rejectedOffset;
        Check(ReserveFinal(snapshot, plan, corruptOffset, rejectedOffset) &&
                !PartitionPathTraceCaptureProductBucketMajor(
                    snapshot, corruptOffset, rejectedOffset),
            "corrupt source offset rejects partition publication");
        RtPathTraceCaptureProduct corruptRebase = MakeCandidate(snapshot, plan);
        corruptRebase.indexes[3] = 0;
        RtPathTraceCaptureProduct rejectedRebase;
        Check(ReserveFinal(snapshot, plan, corruptRebase, rejectedRebase) &&
                !PartitionPathTraceCaptureProductBucketMajor(
                    snapshot, corruptRebase, rejectedRebase),
            "corrupt source-local rebase rejects partition publication");
    }

    {
        const RtPathTraceCaptureProduct canonical =
            MakeCanonicalTwoSurfaceProduct();
        Check(PathTraceCaptureProductSpansCanonical(canonical),
            "canonical worker spans accept two ordered same-bucket surfaces");

        RtPathTraceCaptureOracle bucketLocalOracle;
        bucketLocalOracle.epoch = canonical.epoch;
        bucketLocalOracle.viewIdentity = canonical.viewIdentity;
        bucketLocalOracle.vertices = canonical.vertices;
        bucketLocalOracle.indexes = canonical.indexes;
        bucketLocalOracle.triangleClasses = canonical.triangleClasses;
        bucketLocalOracle.triangleMaterials = canonical.triangleMaterials;
        bucketLocalOracle.triangleInstances = canonical.triangleInstances;
        bucketLocalOracle.triangleIdentities = canonical.triangleIdentities;
        bucketLocalOracle.surfaces = canonical.surfaces;
        bucketLocalOracle.surfaces[1].vertexOffset = 0;
        bucketLocalOracle.surfaces[1].indexOffset = 0;
        bucketLocalOracle.surfaces[1].triangleOffset = 0;
        bucketLocalOracle.surfaces[1].runtimeMaterial.eval.materialId = 97;
        bucketLocalOracle.surfaces[1].decisionPresence = ~std::uint64_t{0};
        bucketLocalOracle.surfaces[1].appendAttempted = true;
        bucketLocalOracle.complete = true;
        const RtPathTraceCaptureComparison geometryOnly =
            ComparePathTraceCaptureProduct(canonical, bucketLocalOracle);
        Check(geometryOnly.exact && geometryOnly.geometryMismatches == 0 &&
                geometryOnly.proposalMismatches == 0,
            "canonical global worker offsets compare exact to bucket-local owner bookkeeping");
        RtPathTraceCaptureProduct strict = canonical;
        strict.ownerDecisionGeometryOnly = false;
        Check(!ComparePathTraceCaptureProduct(strict, bucketLocalOracle).exact,
            "normal strict comparison retains bookkeeping offset/runtime checks");

        std::vector<PathTraceSmokeVertex> appliedVertices;
        std::vector<std::uint32_t> appliedIndexes;
        std::vector<std::uint32_t> appliedClasses;
        std::vector<std::uint32_t> appliedMaterials;
        std::vector<std::uint32_t> appliedInstances;
        std::vector<std::uint32_t> appliedIdentities;
        RtSmokeBucketRanges appliedRanges = {};
        int appliedSurfaces = 0;
        int appliedVertexCount = 0;
        int appliedIndexCount = 0;
        Check(ApplyPathTraceCaptureProductToDynamicFrame(canonical,
                appliedVertices, appliedIndexes, appliedClasses,
                appliedMaterials, appliedInstances, appliedIdentities,
                appliedRanges, appliedSurfaces, appliedVertexCount,
                appliedIndexCount) && appliedVertices.size() == 6 &&
                appliedIndexes.size() == 6 && appliedClasses.size() == 2 &&
                appliedSurfaces == 2 && appliedVertexCount == 6 &&
                appliedIndexCount == 6,
            "real Apply accepts the validated canonical worker layout");

        RtPathTraceCaptureProduct empty;
        empty.epoch = canonical.epoch;
        empty.ownerDecisionGeometryOnly = true;
        empty.complete = true;
        Check(PathTraceCaptureProductSpansCanonical(empty),
            "empty product and empty buckets are canonical");

        const auto invalidCase = [&](RtPathTraceCaptureProduct invalid,
            const char* name)
        {
            Check(!PathTraceCaptureProductSpansCanonical(invalid), name);
            const RtPathTraceCaptureComparison comparison =
                ComparePathTraceCaptureProduct(invalid, bucketLocalOracle);
            Check(!comparison.exact && comparison.geometryMismatches != 0 &&
                    comparison.firstFail ==
                        RtPathTraceCaptureFirstFailure::SurfaceDecision,
                "invalid canonical layout is a geometry comparison failure");

            std::vector<PathTraceSmokeVertex> vertices(1);
            std::vector<std::uint32_t> indexes = { 0, 0, 0 };
            std::vector<std::uint32_t> classes = { 101 };
            std::vector<std::uint32_t> materials = { 103 };
            std::vector<std::uint32_t> instances = { 107 };
            std::vector<std::uint32_t> identities = { 109 };
            const std::size_t vertexCapacity = vertices.capacity();
            const std::size_t indexCapacity = indexes.capacity();
            const std::size_t classCapacity = classes.capacity();
            const std::size_t materialCapacity = materials.capacity();
            const std::size_t instanceCapacity = instances.capacity();
            const std::size_t identityCapacity = identities.capacity();
            RtSmokeBucketRanges ranges = {};
            for (int bucket = 0; bucket < RT_SMOKE_CLASS_COUNT; ++bucket)
            {
                ranges.buckets[bucket].vertexOffset = 113 + bucket;
                ranges.buckets[bucket].indexOffset = 127 + bucket;
                ranges.buckets[bucket].triangleOffset = 131 + bucket;
                ranges.buckets[bucket].surfaceCount = 137 + bucket;
            }
            const RtSmokeBucketRanges rangesBefore = ranges;
            const PathTraceSmokeVertex vertexBefore = vertices[0];
            int sourceSurfaces = 139;
            int sourceVertices = 149;
            int sourceIndexes = 151;
            Check(!ApplyPathTraceCaptureProductToDynamicFrame(invalid,
                    vertices, indexes, classes, materials, instances,
                    identities, ranges, sourceSurfaces, sourceVertices,
                    sourceIndexes) && vertices.size() == 1 &&
                    std::memcmp(&vertices[0], &vertexBefore,
                        sizeof(vertexBefore)) == 0 &&
                    indexes == std::vector<std::uint32_t>({ 0, 0, 0 }) &&
                    classes == std::vector<std::uint32_t>({ 101 }) &&
                    materials == std::vector<std::uint32_t>({ 103 }) &&
                    instances == std::vector<std::uint32_t>({ 107 }) &&
                    identities == std::vector<std::uint32_t>({ 109 }) &&
                    vertices.capacity() == vertexCapacity &&
                    indexes.capacity() == indexCapacity &&
                    classes.capacity() == classCapacity &&
                    materials.capacity() == materialCapacity &&
                    instances.capacity() == instanceCapacity &&
                    identities.capacity() == identityCapacity &&
                    std::memcmp(&ranges, &rangesBefore, sizeof(ranges)) == 0 &&
                    sourceSurfaces == 139 && sourceVertices == 149 &&
                    sourceIndexes == 151,
                "invalid canonical layout cannot mutate Apply outputs");
        };

        RtPathTraceCaptureProduct invalid = canonical;
        invalid.surfaces[0].vertexOffset = 1;
        invalidCase(std::move(invalid), "in-bounds nonzero first offset is rejected");
        invalid = canonical;
        invalid.surfaces[1].vertexOffset = 0;
        invalid.surfaces[1].indexOffset = 0;
        invalid.surfaces[1].triangleOffset = 0;
        invalidCase(std::move(invalid), "same-bucket overlap is rejected");
        invalid = canonical;
        invalid.surfaces[1].vertexOffset = 4;
        invalidCase(std::move(invalid), "same-bucket gap is rejected");
        invalid = canonical;
        invalid.surfaces[0].vertexOffset = 1;
        invalidCase(std::move(invalid), "wrong first offset in dynamic bucket is rejected");
        invalid = canonical;
        std::swap(invalid.surfaces[0].vertexOffset,
            invalid.surfaces[1].vertexOffset);
        std::swap(invalid.surfaces[0].indexOffset,
            invalid.surfaces[1].indexOffset);
        std::swap(invalid.surfaces[0].triangleOffset,
            invalid.surfaces[1].triangleOffset);
        invalidCase(std::move(invalid), "swapped same-bucket ordinal spans are rejected");
        invalid = canonical;
        invalid.vertices.emplace_back();
        invalidCase(std::move(invalid), "unclaimed tail geometry is rejected");
        invalid = canonical;
        invalid.surfaces[0].bucketIndex =
            (invalid.surfaces[0].bucketIndex + 1) % RT_SMOKE_CLASS_COUNT;
        invalidCase(std::move(invalid), "bucket and surface class mismatch is rejected");
        invalid = canonical;
        invalid.surfaces[0].triangleCount = 2;
        invalidCase(std::move(invalid), "index and triangle cardinality mismatch is rejected");
        invalid = canonical;
        invalid.indexes[0] = 3;
        invalidCase(std::move(invalid),
            "surface-local index escape is rejected even when product-global in bounds");
        invalid = canonical;
        invalid.surfaces[0].vertexOffset = UINT32_MAX;
        invalidCase(std::move(invalid), "uint32 offset overflow boundary is rejected");
        invalid = canonical;
        invalid.surfaces[0].terminal =
            RtPathTraceCaptureTerminal::AdmissionRejected;
        invalidCase(std::move(invalid), "non-emitting surface with a nonzero span is rejected");
    }

    {
        RtPathTraceRuntimeMaterialStagePod stage;
        stage.valid = true;
        stage.usesPerSurfaceState = true;
        stage.diffuse = true;
        stage.stageIndex = 0;
        stage.conditionRegister = 0;
        stage.colorRegisters[0] = 1;
        stage.colorRegisters[1] = 2;
        stage.colorRegisters[2] = 3;
        stage.colorRegisters[3] = 4;
        stage.hasAlphaTest = true;
        stage.alphaTestRegister = 5;
        stage.emissiveLike = true;
        const float registers[] = { 1.0f, 0.25f, 0.5f, 1.0f, 0.75f, 0.4f };
        const float origin[] = { 4.0f, 5.0f, 6.0f };
        const RtPathTraceRuntimeMaterialEvalPod eval =
            BuildPathTraceRuntimeMaterialEvalFromPod(
                true, 41, &stage, 1, registers, 6, false, origin, true);
        Check(eval.result == RtPathTraceRuntimeEvalBuildResult::Built &&
                eval.selectedStageIndex == 0 && eval.enabledStages == 1 &&
                eval.alphaStages == 1 && eval.alphaTestStages == 1 &&
                eval.selectedStageEmissive && eval.alphaTest == 0.4f &&
                eval.color[0] == 0.25f && eval.hasDiffuseStageColor &&
                eval.surfaceOrigin[2] == 6.0f,
            "shared runtime material kernel evaluates raw registers and stages");

        // A source-alpha-over swinglight is emissive only through the exact
        // compatibility predicate; its colored registers still control on/off.
        auto swingStage = stage;
        swingStage.emissiveLike = false;
        swingStage.diffuse = false;
        swingStage.hasAlphaTest = false;
        float swingRegisters[] = { 1, 0.843f, 1, 0.996f, 1, 0 };
        const auto swingOn = BuildPathTraceRuntimeMaterialEvalFromPod(
            true, 42, &swingStage, 1, swingRegisters, 6, true, origin, true);
        const auto ordinaryAlpha = BuildPathTraceRuntimeMaterialEvalFromPod(
            true, 42, &swingStage, 1, swingRegisters, 6, false, origin, true);
        swingRegisters[1] = swingRegisters[2] = swingRegisters[3] = 0;
        const auto swingOff = BuildPathTraceRuntimeMaterialEvalFromPod(
            true, 42, &swingStage, 1, swingRegisters, 6, true, origin, true);
        Check(swingOn.selectedStageEmissive && swingOn.orderedStages[0].emissive &&
            swingOn.color[0] == 0.843f && !ordinaryAlpha.selectedStageEmissive &&
            swingOff.selectedStageEmissive && swingOff.color[0] == 0 &&
            swingOff.color[1] == 0 && swingOff.color[2] == 0,
            "R4-004 swinglight compatibility retains colored on/off without promoting ordinary alpha blend");

        const std::uint64_t modelTokens[] = { 0x1111u, 0x2222u, 0x3333u };
        Check(ResolvePathTraceModelSurfaceIndexFromPod(
                7, 0x2222u, modelTokens, 3) == 7,
            "explicit model surface index is authoritative");
        Check(ResolvePathTraceModelSurfaceIndexFromPod(
                -1, 0x2222u, modelTokens, 3) == 1,
            "negative model surface index resolves first ordered token match");
        Check(ResolvePathTraceModelSurfaceIndexFromPod(
                -1, 0x9999u, modelTokens, 3) == -1,
            "negative model surface index remains unresolved without a token match");

        int liveFallbackCalls = 0;
        const auto exerciseSerialRigidBridge = [&](bool active,
            std::uint64_t snapshotModelBits,
            std::uint64_t snapshotModelEpoch,
            std::int32_t snapshotRequestedIndex,
            std::uint64_t snapshotTriToken,
            std::uint64_t currentModelBits,
            std::uint64_t currentModelEpoch,
            std::int32_t currentRequestedIndex,
            std::uint64_t currentTriToken,
            bool tokenSpanValid)
        {
            const RtPathTraceSerialRigidSnapshotResult result =
                PlanPathTraceSerialRigidSnapshotUseFromPod(
                    active,
                    snapshotModelBits, snapshotModelEpoch,
                    snapshotRequestedIndex, snapshotTriToken,
                    currentModelBits, currentModelEpoch,
                    currentRequestedIndex, currentTriToken,
                    tokenSpanValid, modelTokens, 3);
            if (result.allowLiveFallback)
            {
                ++liveFallbackCalls;
            }
            return result;
        };
        const auto inactiveBridge = exerciseSerialRigidBridge(
            false, 0xabu, 5, -1, 0x2222u, 0xabu, 5, -1, 0x2222u, true);
        Check(liveFallbackCalls == 1 && !inactiveBridge.comparedCall &&
                !inactiveBridge.useSnapshot,
            "inactive serial rigid oracle permits exactly one live fallback");
        const auto exactBridge = exerciseSerialRigidBridge(
            true, 0xabu, 5, -1, 0x2222u, 0xabu, 5, -1, 0x2222u, true);
        Check(liveFallbackCalls == 1 && exactBridge.comparedCall &&
                exactBridge.useSnapshot &&
                exactBridge.resolvedModelSurfaceIndex == 1 &&
                exactBridge.resolvedSurfaceMatchesCurrent,
            "active exact serial rigid facts use the snapshot without fallback");
        const auto modelBitsMismatch = exerciseSerialRigidBridge(
            true, 0xabu, 5, -1, 0x2222u, 0xacu, 5, -1, 0x2222u, true);
        const auto modelEpochMismatch = exerciseSerialRigidBridge(
            true, 0xabu, 5, -1, 0x2222u, 0xabu, 6, -1, 0x2222u, true);
        const auto requestedMismatch = exerciseSerialRigidBridge(
            true, 0xabu, 5, -1, 0x2222u, 0xabu, 5, 1, 0x2222u, true);
        const auto tokenMismatch = exerciseSerialRigidBridge(
            true, 0xabu, 5, -1, 0x2222u, 0xabu, 5, -1, 0x3333u, true);
        const auto invalidSpan = exerciseSerialRigidBridge(
            true, 0xabu, 5, -1, 0x2222u, 0xabu, 5, -1, 0x2222u, false);
        const auto activeFailure = [](const auto& result)
        {
            return result.comparedCall && !result.useSnapshot &&
                !result.allowLiveFallback &&
                result.state != RtPathTraceSerialRigidSnapshotState::Inactive;
        };
        Check(liveFallbackCalls == 1 && activeFailure(modelBitsMismatch) &&
                activeFailure(modelEpochMismatch) &&
                activeFailure(requestedMismatch) &&
                activeFailure(tokenMismatch) && activeFailure(invalidSpan),
            "active serial rigid mismatches and invalid token spans fail closed without fallback");

        const RtPathTraceRuntimeMaterialVariantPod workerVariantKey =
            BuildPathTraceRuntimeMaterialVariantKeyFromPod(
                41, 7, 8, -1, 0x2222u, modelTokens, 3);
        const RtPathTraceRuntimeMaterialVariantPod liveLikeVariantKey =
            BuildPathTraceRuntimeMaterialVariantKeyFromPod(
                41, 7, 8, -1, 0x2222u, modelTokens, 3);
        const RtPathTraceRuntimeMaterialVariantPod missingTokenVariantKey =
            BuildPathTraceRuntimeMaterialVariantKeyFromPod(
                41, 7, 8, -1, 0x2222u, nullptr, 0);
        Check(workerVariantKey.modelSurfaceIndex == 1 &&
                workerVariantKey.triIdentityBits == 0 &&
                std::memcmp(&workerVariantKey, &liveLikeVariantKey,
                    sizeof(workerVariantKey)) == 0,
            "live-like and worker variant keys are byte-identical on one POD table");

        const RtPathTraceRuntimeMaterialDecisionPod workerVariantDecision =
            BuildPathTraceRuntimeMaterialDecisionFromPod(
                workerVariantKey, eval, nullptr, 0, nullptr, 0);
        const RtPathTraceRuntimeMaterialDecisionPod liveLikeVariantDecision =
            BuildPathTraceRuntimeMaterialDecisionFromPod(
                liveLikeVariantKey, eval, nullptr, 0, nullptr, 0);
        const RtPathTraceRuntimeMaterialDecisionPod missingTokenVariantDecision =
            BuildPathTraceRuntimeMaterialDecisionFromPod(
                missingTokenVariantKey, eval, nullptr, 0, nullptr, 0);
        Check(workerVariantDecision.initialCandidateId ==
                liveLikeVariantDecision.initialCandidateId &&
                workerVariantDecision.chosenMaterialId ==
                liveLikeVariantDecision.chosenMaterialId,
            "variant initial and chosen IDs match from the same token table");
        Check(missingTokenVariantDecision.initialCandidateId !=
                workerVariantDecision.initialCandidateId,
            "missing token table exposes the old unresolved variant-key fork");
        RtPathTraceCaptureSurfaceProduct workerDecisionSurface;
        workerDecisionSurface.runtimeMaterial = workerVariantDecision;
        FinalizePathTraceCaptureSurfaceDecision(workerDecisionSurface);
        RtPathTraceCaptureSurfaceProduct liveDecisionSurface =
            workerDecisionSurface;
        liveDecisionSurface.runtimeMaterial = missingTokenVariantDecision;
        Check(!PathTraceCaptureSurfaceDecisionsEqual(
                workerDecisionSurface, liveDecisionSurface) &&
                PathTraceCaptureSurfaceDecisionsEqual(
                    workerDecisionSurface, workerDecisionSurface),
            "comparison mismatches without model tokens and is exact with one shared table");
        std::uint64_t mutatedTokens[] = { 0x1111u, 0x4444u, 0x3333u };
        const RtPathTraceRuntimeMaterialVariantPod mutatedVariantKey =
            BuildPathTraceRuntimeMaterialVariantKeyFromPod(
                41, 7, 8, -1, 0x2222u, mutatedTokens, 3);
        Check(mutatedVariantKey.modelSurfaceIndex == -1 &&
                mutatedVariantKey.triIdentityBits == 0x2222u &&
                BuildPathTraceRuntimeMaterialDecisionFromPod(
                    mutatedVariantKey, eval, nullptr, 0, nullptr, 0)
                    .initialCandidateId != workerVariantDecision.initialCandidateId,
            "post-snapshot token mutation fails closed as a variant mismatch");

        RtPathTraceRigidMeshIdentityPod workerMesh;
        workerMesh.modelIdentity = 0xabcdu;
        workerMesh.modelEpoch = 3;
        workerMesh.modelSurfaceIndex = workerVariantKey.modelSurfaceIndex;
        workerMesh.vertexBufferIdentity = 0x100u;
        workerMesh.indexBufferIdentity = 0x200u;
        workerMesh.numVerts = 3;
        workerMesh.numIndexes = 3;
        workerMesh.materialId = workerVariantDecision.chosenMaterialId;
        RtPathTraceRigidMeshIdentityPod liveMesh = workerMesh;
        const std::uint64_t workerMeshHash =
            BuildPathTraceRigidMeshHashFromPod(workerMesh);
        const std::uint64_t liveMeshHash =
            BuildPathTraceRigidMeshHashFromPod(liveMesh);
        RtPathTraceRigidInstanceIdentityPod workerInstance;
        workerInstance.meshHash = workerMeshHash;
        workerInstance.renderWorldIdentity = 0x300u;
        workerInstance.renderDefIndex = 4;
        workerInstance.renderDefGeneration = 5;
        workerInstance.modelEpoch = 3;
        workerInstance.entityIndex = 7;
        workerInstance.renderEntityNum = 8;
        workerInstance.modelSurfaceIndex = workerVariantKey.modelSurfaceIndex;
        workerInstance.materialId = workerVariantDecision.chosenMaterialId;
        RtPathTraceRigidInstanceIdentityPod liveInstance = workerInstance;
        Check(workerMeshHash == liveMeshHash &&
                BuildPathTraceRigidInstanceIdFromPod(workerInstance) ==
                BuildPathTraceRigidInstanceIdFromPod(liveInstance),
            "rigid mesh hash and instance ID share the resolved POD ordinal");

        RtPathTraceRuntimeMaterialVariantPod key;
        key.baseMaterialId = 41;
        key.entityIndex = 7;
        key.entityNum = 8;
        key.modelSurfaceIndex = 2;
        RtPathTraceCaptureRegistryMaterialPod registry[2];
        registry[0].materialId = 41;
        const RtPathTraceRuntimeMaterialDecisionPod absentBase =
            BuildPathTraceRuntimeMaterialDecisionFromPod(
                key, eval, nullptr, 0, nullptr, 0);
        const RtPathTraceRuntimeMaterialDecisionPod clear =
            BuildPathTraceRuntimeMaterialDecisionFromPod(
                key, eval, nullptr, 0, registry, 1);
        registry[1].materialId = clear.initialCandidateId;
        const RtPathTraceRuntimeMaterialDecisionPod collision =
            BuildPathTraceRuntimeMaterialDecisionFromPod(
                key, eval, nullptr, 0, registry, 2);
        RtPathTraceMaterialTextureVariantBasePod owned;
        owned.variantId = clear.initialCandidateId;
        owned.baseId = 41;
        const RtPathTraceRuntimeMaterialDecisionPod reusable =
            BuildPathTraceRuntimeMaterialDecisionFromPod(
                key, eval, &owned, 1, registry, 2);
        Check(clear.chosenMaterialId == clear.initialCandidateId &&
                absentBase.initialCandidateId == clear.initialCandidateId &&
                absentBase.baseMaterialRegistered && clear.baseMaterialRegistered &&
                collision.chosenMaterialId != clear.initialCandidateId &&
                collision.collisionCount == 1 &&
                reusable.chosenMaterialId == clear.initialCandidateId,
            "registry materials and variant bases both control the shared collision path");
        std::vector<RtPathTraceCaptureRegistryMaterialPod> exhausted(17);
        exhausted[0].materialId = key.baseMaterialId;
        std::uint32_t occupied = clear.initialCandidateId;
        for (std::uint32_t attempt = 0; attempt < 16; ++attempt)
        {
            exhausted[attempt + 1].materialId = occupied;
            occupied = RtPathTraceRuntimeHash32(
                occupied ^ 0x9e3779b9u, attempt + 1u) | 0x80000000u;
        }
        const RtPathTraceRuntimeMaterialDecisionPod fallback =
            BuildPathTraceRuntimeMaterialDecisionFromPod(
                key, eval, nullptr, 0, exhausted.data(), exhausted.size());
        Check(fallback.chosenMaterialId == key.baseMaterialId &&
                fallback.collisionCount == 16 && fallback.fallbackUsed,
            "shared variant authority fails closed after the complete collision chain");

        RtPathTraceCaptureSurfaceProduct safetyTerminal;
        safetyTerminal.sourceState = static_cast<std::uint32_t>(
            RtPathTraceCaptureSafetyDisposition::CopyFailed);
        FinalizePathTraceCaptureSurfaceDecision(safetyTerminal);
        Check(safetyTerminal.decisionPresence == RT_PT_CAPTURE_DECISION_FILTER,
            "safety terminal retains source state without manufacturing runtime presence");
        for (RtPathTraceRuntimeEvalBuildResult result : {
                RtPathTraceRuntimeEvalBuildResult::Built,
                RtPathTraceRuntimeEvalBuildResult::NoSelectedStage,
                RtPathTraceRuntimeEvalBuildResult::NoMaterial })
        {
            RtPathTraceCaptureSurfaceProduct runtimeTerminal;
            runtimeTerminal.runtimeMaterial.eval.materialId = 41;
            runtimeTerminal.runtimeMaterial.eval.result = result;
            FinalizePathTraceCaptureSurfaceDecision(runtimeTerminal);
            Check((runtimeTerminal.decisionPresence &
                    RT_PT_CAPTURE_DECISION_RUNTIME_MATERIAL) != 0,
                "built, static-shortcut and unavailable runtime decisions share presence rules");
        }
    }

    {
        RtPathTraceCaptureCapacityCounts modelCounts;
        modelCounts.surfaces = 2;
        modelCounts.modelTables = 1;
        modelCounts.modelSurfaceTokens = 3;
        RtPathTraceCaptureProductCapacityPlan modelPlan;
        RtPathTraceCaptureOwnerSnapshot modelSnapshot;
        modelSnapshot.capacityCounts = modelCounts;
        Check(Plan(modelCounts, modelPlan) &&
                ReservePathTraceCaptureOwnerSnapshotStorage(
                    modelSnapshot, 0, modelPlan),
            "one unique model table reserves once for multiple surfaces");
        RtPathTraceCaptureModelTokenTablePod table;
        table.modelBits = 0xabcdu;
        table.modelEpoch = 3;
        table.tokenCount = 3;
        modelSnapshot.modelTables.push_back(table);
        modelSnapshot.modelSurfaceTokens.insert(
            modelSnapshot.modelSurfaceTokens.end(), { 1u, 2u, 3u });
        RtPathTraceCaptureRawSurface first;
        first.modelBits = table.modelBits;
        first.modelEpoch = table.modelEpoch;
        first.modelTableIndex = 0;
        RtPathTraceCaptureRawSurface second = first;
        second.ordinal = 1;
        modelSnapshot.surfaces.push_back(first);
        modelSnapshot.surfaces.push_back(second);
        const std::uint32_t firstTable =
            FindPathTraceCaptureModelTokenTableFromPod(
                first.modelBits, first.modelEpoch,
                modelSnapshot.modelTables.data(),
                modelSnapshot.modelTables.size());
        const std::uint32_t secondTable =
            FindPathTraceCaptureModelTokenTableFromPod(
                second.modelBits, second.modelEpoch,
                modelSnapshot.modelTables.data(),
                modelSnapshot.modelTables.size());
        Check(firstTable == 0 && secondTable == firstTable &&
                modelSnapshot.modelTables.size() == 1 &&
                modelSnapshot.modelSurfaceTokens.size() == 3 &&
                modelSnapshot.OwnedBytes() <= modelPlan.snapshotBytes,
            "per-model dedup stores one ordered token table and charges it once");

        RtPathTraceCaptureCapacityCounts overCap;
        overCap.modelTables = 1;
        overCap.modelSurfaceTokens = RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES /
            sizeof(std::uint64_t);
        Check(!Plan(overCap, modelPlan),
            "model token bytes participate in the complete-slot cap");
    }

    {
        RtPathTraceCaptureSurfaceProduct baseline;
        baseline.ordinal = 3;
        baseline.terminal = RtPathTraceCaptureTerminal::Accepted;
        baseline.surfaceClass = RtSmokeSurfaceClass::RigidEntity;
        baseline.translucentSubtype = RtSmokeTranslucentSubtype::SignageGlow;
        baseline.surfaceClassId = 17;
        baseline.materialId = 23;
        baseline.materialClassSignature = 29;
        baseline.sourceFlags = 31;
        baseline.meshHash = 37;
        baseline.instanceId = 41;
        baseline.vertexOffset = 2;
        baseline.vertexCount = 3;
        baseline.indexOffset = 4;
        baseline.indexCount = 6;
        baseline.triangleOffset = 7;
        baseline.triangleCount = 2;
        baseline.invalidIndexCount = 1;
        baseline.zeroAreaTriangleCount = 2;
        baseline.staticMatch = true;
        baseline.rigidReadyByMesh = true;
        baseline.applyGateSkip = false;
        baseline.decisionPresence = ~std::uint64_t{0};
        baseline.runtimeMaterial.eval.result =
            RtPathTraceRuntimeEvalBuildResult::Built;
        baseline.runtimeMaterial.eval.materialId = 23;
        baseline.runtimeMaterial.eval.orderedStageCount = 1;
        baseline.runtimeMaterial.eval.orderedStages[0].stageIndex = 1;
        baseline.runtimeMaterial.initialCandidateId = 43;
        baseline.runtimeMaterial.chosenMaterialId = 47;
        baseline.compositeRoute = 1;
        baseline.skinnedAdmission = 2;
        baseline.sourceState = 3;
        baseline.bucketIndex = 1;
        baseline.preAppendVertexOffset = 5;
        baseline.capturedRecordPresent = true;
        baseline.capturedRecordIndexCount = 6;
        baseline.skinnedRecordPresent = true;
        baseline.skinnedRecordIndexCount = 6;
        baseline.mergedWalkRecordPresent = true;
        baseline.mergedWalkIndexCount = 6;
        baseline.mergedCompanionRigidId = 53;
        baseline.bucketRangePublished = true;
        baseline.admissionPrecheckPassed = true;
        baseline.appendAttempted = true;
        Check(PathTraceCaptureSurfaceDecisionsEqual(baseline, baseline),
            "complete decision comparator accepts an exact row");
#define RT_PT_MUTATION(field, value, name) \
        do { RtPathTraceCaptureSurfaceProduct changed = baseline; \
            changed.field = (value); \
            Check(!PathTraceCaptureSurfaceDecisionsEqual(baseline, changed), name); \
        } while (0)
        RT_PT_MUTATION(ordinal, 4, "ordinal mutation");
        RT_PT_MUTATION(terminal, RtPathTraceCaptureTerminal::RolledBack, "terminal mutation");
        RT_PT_MUTATION(surfaceClass, RtSmokeSurfaceClass::StaticWorld, "surface class mutation");
        RT_PT_MUTATION(translucentSubtype, RtSmokeTranslucentSubtype::ObjectGlass, "subtype mutation");
        RT_PT_MUTATION(surfaceClassId, 18, "class id mutation");
        RT_PT_MUTATION(materialId, 24, "material id mutation");
        RT_PT_MUTATION(materialClassSignature, 30, "material signature mutation");
        RT_PT_MUTATION(sourceFlags, 32, "source flags mutation");
        RT_PT_MUTATION(meshHash, 38, "mesh hash mutation");
        RT_PT_MUTATION(instanceId, 42, "instance id mutation");
        RT_PT_MUTATION(vertexOffset, 3, "vertex offset mutation");
        RT_PT_MUTATION(vertexCount, 4, "vertex count mutation");
        RT_PT_MUTATION(indexOffset, 5, "index offset mutation");
        RT_PT_MUTATION(indexCount, 7, "index count mutation");
        RT_PT_MUTATION(triangleOffset, 8, "triangle offset mutation");
        RT_PT_MUTATION(triangleCount, 3, "triangle count mutation");
        RT_PT_MUTATION(invalidIndexCount, 2, "invalid index mutation");
        RT_PT_MUTATION(zeroAreaTriangleCount, 3, "zero area mutation");
        RT_PT_MUTATION(staticMatch, false, "static match mutation");
        RT_PT_MUTATION(rigidReadyByMesh, false, "mesh readiness mutation");
        RT_PT_MUTATION(rigidReadyByResident, true, "resident readiness mutation");
        RT_PT_MUTATION(applyGateSkip, true, "apply gate mutation");
        RT_PT_MUTATION(decisionPresence, 0, "presence mutation");
        RT_PT_MUTATION(compositeRoute, 2, "composite mutation");
        RT_PT_MUTATION(skinnedAdmission, 3, "skinned admission mutation");
        RT_PT_MUTATION(sourceState, 4, "source state mutation");
        RT_PT_MUTATION(bucketIndex, 2, "bucket index mutation");
        RT_PT_MUTATION(preAppendVertexOffset, 6, "pre vertex mutation");
        RT_PT_MUTATION(preAppendIndexOffset, 1, "pre index mutation");
        RT_PT_MUTATION(preAppendTriangleOffset, 1, "pre triangle mutation");
        RT_PT_MUTATION(capturedRecordVertexCount, 1, "captured vertex mutation");
        RT_PT_MUTATION(capturedRecordIndexCount, 7, "captured index mutation");
        RT_PT_MUTATION(capturedRecordTriangleCount, 1, "captured triangle mutation");
        RT_PT_MUTATION(skinnedRecordVertexCount, 1, "skinned vertex mutation");
        RT_PT_MUTATION(skinnedRecordIndexCount, 7, "skinned index mutation");
        RT_PT_MUTATION(skinnedRecordTriangleCount, 1, "skinned triangle mutation");
        RT_PT_MUTATION(mergedWalkVertexCount, 1, "merged vertex mutation");
        RT_PT_MUTATION(mergedWalkIndexCount, 7, "merged index mutation");
        RT_PT_MUTATION(mergedWalkTriangleCount, 1, "merged triangle mutation");
        RT_PT_MUTATION(mergedCompanionRigidId, 54, "merged rigid mutation");
        RT_PT_MUTATION(mergedCompanionSkinnedId, 1, "merged skinned mutation");
        RT_PT_MUTATION(alphaDiagnosticRejected, true, "alpha diagnostic mutation");
        RT_PT_MUTATION(liquidPoolPromoted, true, "liquid promotion mutation");
        RT_PT_MUTATION(skinnedCaptureOmitted, true, "skinned omission mutation");
        RT_PT_MUTATION(capturedRecordPresent, false, "captured presence mutation");
        RT_PT_MUTATION(skinnedRecordPresent, false, "skinned presence mutation");
        RT_PT_MUTATION(mergedWalkRecordPresent, false, "merged presence mutation");
        RT_PT_MUTATION(bucketRangePublished, false, "bucket publish mutation");
        RT_PT_MUTATION(admissionPrecheckPassed, false, "admission mutation");
        RT_PT_MUTATION(appendAttempted, false, "append mutation");
        RT_PT_MUTATION(rollbackApplied, true, "rollback mutation");
        RT_PT_MUTATION(runtimeMaterial.initialCandidateId, 44, "initial candidate mutation");
        RT_PT_MUTATION(runtimeMaterial.chosenMaterialId, 48, "chosen id mutation");
        RT_PT_MUTATION(runtimeMaterial.collisionCount, 1, "collision count mutation");
        RT_PT_MUTATION(runtimeMaterial.fallbackUsed, true, "fallback mutation");
        RT_PT_MUTATION(runtimeMaterial.baseMaterialRegistered, true, "base registration mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.result, RtPathTraceRuntimeEvalBuildResult::NoSelectedStage, "eval result mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.materialId, 24, "eval material mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.selectedStageIndex, 2, "selected stage mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.selectedStagePriority, 1, "stage priority mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.enabledStages, 1, "enabled stages mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.disabledStages, 1, "disabled stages mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.colorStages, 1, "color stages mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.alphaStages, 1, "alpha stages mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.alphaTestStages, 1, "alpha test stages mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.texMatrixStages, 1, "tex matrix stages mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.dynamicImageStages, 1, "dynamic image stages mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.cinematicStages, 1, "cinematic stages mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.guiRenderTargetStages, 1, "gui target stages mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.programStages, 1, "program stages mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.selectedStageEmissive, true, "selected emissive mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.hasDiffuseStageColor, true, "diffuse presence mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.orderedStageOverflow, true, "stage overflow mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.hasSurfaceOrigin, true, "origin presence mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.condition, 0.5f, "condition mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.alphaTest, 0.5f, "alpha test mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.diffuseStageCondition, 0.5f, "diffuse condition mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.orderedStageCount, 0, "ordered stage count mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.color[0], 0.5f, "eval color[0] mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.color[1], 0.5f, "eval color[1] mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.color[2], 0.5f, "eval color[2] mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.color[3], 0.5f, "eval color[3] mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.texMatrix[0], 0.5f, "eval matrix mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.diffuseStageColor[0], 0.5f, "diffuse color mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.surfaceOrigin[0], 0.5f, "surface origin mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.orderedStages[0].stageIndex, 2, "ordered stage index mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.orderedStages[0].enabled, true, "ordered enabled mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.orderedStages[0].emissive, true, "ordered emissive mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.orderedStages[0].hasAlphaTest, true, "ordered alpha presence mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.orderedStages[0].hasTexMatrix, true, "ordered matrix presence mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.orderedStages[0].condition, 0.5f, "ordered condition mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.orderedStages[0].alphaTest, 0.5f, "ordered alpha mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.orderedStages[0].color[0], 0.5f, "ordered color mutation");
        RT_PT_MUTATION(runtimeMaterial.eval.orderedStages[0].texMatrix[0], 0.5f, "ordered matrix mutation");
        for (int index = 0; index < 4; ++index)
        {
            RtPathTraceCaptureSurfaceProduct changed = baseline;
            changed.runtimeMaterial.eval.color[index] += 1.0f;
            Check(!PathTraceCaptureSurfaceDecisionsEqual(baseline, changed),
                "every eval color element is compared");
            changed = baseline;
            changed.runtimeMaterial.eval.diffuseStageColor[index] += 1.0f;
            Check(!PathTraceCaptureSurfaceDecisionsEqual(baseline, changed),
                "every diffuse color element is compared");
            changed = baseline;
            changed.runtimeMaterial.eval.orderedStages[0].color[index] += 1.0f;
            Check(!PathTraceCaptureSurfaceDecisionsEqual(baseline, changed),
                "every ordered color element is compared");
        }
        for (int index = 0; index < 6; ++index)
        {
            RtPathTraceCaptureSurfaceProduct changed = baseline;
            changed.runtimeMaterial.eval.texMatrix[index] += 1.0f;
            Check(!PathTraceCaptureSurfaceDecisionsEqual(baseline, changed),
                "every eval matrix element is compared");
            changed = baseline;
            changed.runtimeMaterial.eval.orderedStages[0].texMatrix[index] += 1.0f;
            Check(!PathTraceCaptureSurfaceDecisionsEqual(baseline, changed),
                "every ordered matrix element is compared");
        }
        for (int index = 0; index < 3; ++index)
        {
            RtPathTraceCaptureSurfaceProduct changed = baseline;
            changed.runtimeMaterial.eval.surfaceOrigin[index] += 1.0f;
            Check(!PathTraceCaptureSurfaceDecisionsEqual(baseline, changed),
                "every surface origin element is compared");
        }
#undef RT_PT_MUTATION
    }

    {
        const auto exactPair = [&]()
        {
            std::pair<RtPathTraceCaptureProduct, RtPathTraceCaptureOracle> pair;
            pair.first.epoch.generation = 11;
            pair.first.epoch.frameIndex = 11;
            pair.first.epoch.capturedAfterBeginFrame = true;
            pair.first.epoch.capturedAfterStaticPreload = true;
            pair.first.epoch.capturedBeforeSerialMutate = true;
            pair.second.epoch = pair.first.epoch;
            pair.first.viewIdentity = 17;
            pair.second.viewIdentity = 17;
            pair.first.complete = true;
            pair.second.complete = true;
            return pair;
        };
        const auto checkDiagnostic = [&](const auto& pair,
            RtPathTraceCaptureFirstFailure firstFail,
            std::uint32_t surfaceOrdinal,
            std::uint32_t geometryMismatches,
            std::uint32_t proposalMismatches,
            const char* name)
        {
            const RtPathTraceCaptureComparison comparison =
                ComparePathTraceCaptureProduct(pair.first, pair.second);
            Check(!comparison.exact && comparison.firstFail == firstFail &&
                    comparison.firstSurfaceOrdinal == surfaceOrdinal &&
                    comparison.geometryMismatches == geometryMismatches &&
                    comparison.proposalMismatches == proposalMismatches,
                name);
        };
        auto incomplete = exactPair();
        incomplete.first.complete = false;
        checkDiagnostic(incomplete, RtPathTraceCaptureFirstFailure::Incomplete,
            UINT32_MAX, 1, 1, "first failure diagnoses incomplete product");
        auto epoch = exactPair();
        ++epoch.second.epoch.generation;
        checkDiagnostic(epoch, RtPathTraceCaptureFirstFailure::Epoch,
            UINT32_MAX, 1, 1, "first failure diagnoses epoch mismatch");
        auto view = exactPair();
        ++view.second.viewIdentity;
        checkDiagnostic(view, RtPathTraceCaptureFirstFailure::ViewIdentity,
            UINT32_MAX, 1, 1, "first failure diagnoses view identity mismatch");
        auto classes = exactPair();
        classes.first.triangleClasses.push_back(7);
        classes.second.triangleClasses.push_back(8);
        checkDiagnostic(classes, RtPathTraceCaptureFirstFailure::TriangleClasses,
            UINT32_MAX, 1, 0, "first failure diagnoses triangle class mutation");
        auto identities = exactPair();
        identities.first.triangleIdentities.push_back(7);
        identities.second.triangleIdentities.push_back(8);
        checkDiagnostic(identities,
            RtPathTraceCaptureFirstFailure::TriangleIdentities,
            UINT32_MAX, 1, 0, "first failure diagnoses triangle identity mutation");
        auto surface = exactPair();
        surface.first.surfaces.emplace_back();
        surface.second.surfaces.emplace_back();
        surface.first.surfaces[0].vertexOffset = 1;
        checkDiagnostic(surface, RtPathTraceCaptureFirstFailure::SurfaceDecision,
            0, 0, 1, "first failure diagnoses surface decision ordinal");

        auto ownerOnly = exactPair();
        ownerOnly.first.ownerDecisionGeometryOnly = true;
        ownerOnly.second.materialInfoIntents.emplace_back();
        ownerOnly.second.instanceObservations.emplace_back();
        const RtPathTraceCaptureComparison ownerOnlyComparison =
            ComparePathTraceCaptureProduct(ownerOnly.first, ownerOnly.second);
        Check(ownerOnlyComparison.exact &&
                ownerOnlyComparison.proposalMismatches == 0,
            "geometry-only Lane A comparison excludes owner-only mutation proposal streams");

        auto geometryOnly = exactPair();
        geometryOnly.first.ownerDecisionGeometryOnly = true;
        RtPathTraceCaptureSurfaceProduct authority;
        authority.ordinal = 0;
        authority.terminal = RtPathTraceCaptureTerminal::StaticMatched;
        authority.surfaceClass = RtSmokeSurfaceClass::RigidEntity;
        authority.translucentSubtype = RtSmokeTranslucentSubtype::SignageGlow;
        authority.surfaceClassId = 3;
        authority.materialId = 41;
        authority.materialClassSignature = 43;
        authority.sourceFlags = 47;
        authority.meshHash = 53;
        authority.instanceId = 59;
        authority.vertexCount = 0;
        authority.indexCount = 0;
        authority.triangleCount = 0;
        authority.invalidIndexCount = 1;
        authority.zeroAreaTriangleCount = 1;
        authority.rigidReadyByMesh = true;
        authority.compositeRoute = 2;
        authority.skinnedAdmission = 3;
        authority.skinnedCaptureOmitted = true;
        authority.liquidPoolPromoted = true;
        authority.alphaDiagnosticRejected = true;
        authority.bucketIndex = 4;
        geometryOnly.first.surfaces.push_back(authority);
        geometryOnly.second.surfaces.push_back(authority);
        RtPathTraceCaptureSurfaceProduct& bookkeeping =
            geometryOnly.second.surfaces[0];
        bookkeeping.runtimeMaterial.eval.materialId = 101;
        bookkeeping.runtimeMaterial.initialCandidateId = 103;
        bookkeeping.decisionPresence = ~std::uint64_t{0};
        bookkeeping.vertexOffset = 7;
        bookkeeping.indexOffset = 11;
        bookkeeping.triangleOffset = 13;
        bookkeeping.preAppendVertexOffset = 17;
        bookkeeping.preAppendIndexOffset = 19;
        bookkeeping.preAppendTriangleOffset = 23;
        bookkeeping.capturedRecordPresent = true;
        bookkeeping.capturedRecordVertexCount = 29;
        bookkeeping.capturedRecordIndexCount = 31;
        bookkeeping.capturedRecordTriangleCount = 37;
        bookkeeping.skinnedRecordPresent = true;
        bookkeeping.skinnedRecordVertexCount = 41;
        bookkeeping.skinnedRecordIndexCount = 43;
        bookkeeping.skinnedRecordTriangleCount = 47;
        bookkeeping.mergedWalkRecordPresent = true;
        bookkeeping.mergedWalkVertexCount = 53;
        bookkeeping.mergedWalkIndexCount = 59;
        bookkeeping.mergedWalkTriangleCount = 61;
        bookkeeping.mergedCompanionRigidId = 67;
        bookkeeping.mergedCompanionSkinnedId = 71;
        bookkeeping.bucketRangePublished = true;
        bookkeeping.admissionPrecheckPassed = true;
        bookkeeping.appendAttempted = true;
        bookkeeping.rollbackApplied = true;
        bookkeeping.sourceState = 73;
        const RtPathTraceCaptureComparison geometryOnlyComparison =
            ComparePathTraceCaptureProduct(geometryOnly.first,
                geometryOnly.second);
        Check(geometryOnlyComparison.exact &&
                geometryOnlyComparison.geometryMismatches == 0 &&
                geometryOnlyComparison.proposalMismatches == 0,
            "geometry-only parity ignores owner/phase-local bookkeeping");
        geometryOnly.first.ownerDecisionGeometryOnly = false;
        const RtPathTraceCaptureComparison fullComparison =
            ComparePathTraceCaptureProduct(geometryOnly.first,
                geometryOnly.second);
        Check(!fullComparison.exact &&
                fullComparison.firstFail ==
                    RtPathTraceCaptureFirstFailure::SurfaceDecision &&
                fullComparison.firstSurfaceOrdinal == 0,
            "normal parity still catches owner/phase-local bookkeeping");

        const auto authorityMutationFails = [&](auto mutate, const char* name)
        {
            auto changed = exactPair();
            changed.first.ownerDecisionGeometryOnly = true;
            changed.first.surfaces.push_back(authority);
            changed.second.surfaces.push_back(authority);
            mutate(changed.second.surfaces[0]);
            const RtPathTraceCaptureComparison comparison =
                ComparePathTraceCaptureProduct(changed.first, changed.second);
            Check(!comparison.exact &&
                    comparison.firstFail ==
                        RtPathTraceCaptureFirstFailure::SurfaceDecision &&
                    comparison.firstSurfaceOrdinal == 0,
                name);
        };
        authorityMutationFails([](auto& row) {
            row.terminal = RtPathTraceCaptureTerminal::RolledBack;
        }, "geometry-only parity catches terminal authority");
        authorityMutationFails([](auto& row) { ++row.materialId; },
            "geometry-only parity catches material authority");
        authorityMutationFails([](auto& row) { ++row.vertexCount; },
            "geometry-only parity catches vertex cardinality authority");
        authorityMutationFails([](auto& row) { ++row.indexCount; },
            "geometry-only parity catches index cardinality authority");
        authorityMutationFails([](auto& row) { ++row.triangleCount; },
            "geometry-only parity catches triangle cardinality authority");
        authorityMutationFails([](auto& row) {
            row.rigidReadyByMesh = !row.rigidReadyByMesh;
        }, "geometry-only parity catches rigid readiness authority");
        authorityMutationFails([](auto& row) { ++row.skinnedAdmission; },
            "geometry-only parity catches admission authority");
        authorityMutationFails([](auto& row) {
            row.surfaceClass = RtSmokeSurfaceClass::StaticWorld;
        }, "geometry-only parity catches class authority");
        authorityMutationFails([](auto& row) { ++row.bucketIndex; },
            "geometry-only parity catches bucket authority");

        auto geo08 = exactPair();
        geo08.first.vertices.resize(5325);
        geo08.second.vertices.resize(3898);
        geo08.first.surfaces.emplace_back();
        geo08.second.surfaces.emplace_back();
        geo08.first.surfaces[0].terminal = RtPathTraceCaptureTerminal::Accepted;
        geo08.second.surfaces[0].terminal = RtPathTraceCaptureTerminal::Accepted;
        geo08.second.liveCardinality.skinnedOmittedSurfaces = 1;
        geo08.second.liveCardinality.skinnedOmittedVertices = 1427;
        geo08.second.liveCardinality.skinnedOmittedIndexes = 2142;
        const RtPathTraceCaptureComparison geo08Comparison =
            ComparePathTraceCaptureProduct(geo08.first, geo08.second);
        Check(!geo08Comparison.exact && geo08Comparison.cardinalityAvailable &&
                geo08Comparison.firstFail ==
                    RtPathTraceCaptureFirstFailure::Vertices &&
                geo08Comparison.cardinality.productAcceptedSurfaces == 1 &&
                geo08Comparison.cardinality.productAcceptedVertices == 5325 &&
                geo08Comparison.cardinality.oracleAcceptedSurfaces == 1 &&
                geo08Comparison.cardinality.oracleAcceptedVertices == 3898 &&
                geo08Comparison.cardinality.afterSkinnedVertexDelta == 0 &&
                geo08Comparison.cardinality.unattributedVertexDelta == 0,
            "cardinality attribution makes the synthetic GEO08 delta exact");

        geo08.second.liveCardinality.skinnedOmittedVertices = 1000;
        const RtPathTraceCaptureComparison unexplained =
            ComparePathTraceCaptureProduct(geo08.first, geo08.second);
        Check(unexplained.cardinality.afterSkinnedVertexDelta == 427 &&
                unexplained.cardinality.unattributedVertexDelta == 427,
            "cardinality attribution preserves a nonmatching vertex delta");

        const RtPathTraceCaptureCardinalityAttribution rigidAccounted =
            RtPathTraceBuildCaptureCardinalityAttribution(
                2, 5500, 1, 3900,
                RtPathTraceCaptureLiveCardinalityAttribution{
                    1, 1000, 1500, 1, 600, 900 });
        Check(rigidAccounted.afterSkinnedVertexDelta == 600 &&
                rigidAccounted.unattributedVertexDelta == 0 &&
                rigidAccounted.live.rigidRemovedVertices == 600,
            "cardinality attribution isolates a rigid-removed residual");

        ++geo08.second.epoch.generation;
        const RtPathTraceCaptureComparison wrongGeneration =
            ComparePathTraceCaptureProduct(geo08.first, geo08.second);
        Check(wrongGeneration.firstFail == RtPathTraceCaptureFirstFailure::Epoch &&
                !wrongGeneration.cardinalityAvailable &&
                wrongGeneration.cardinality.productAcceptedVertices == 0 &&
                wrongGeneration.cardinality.oracleAcceptedVertices == 0,
            "cardinality attribution is suppressed for a mismatched generation");

        auto acceptedDiff = exactPair();
        acceptedDiff.first.vertices.resize(228);
        acceptedDiff.second.vertices.resize(4);
        acceptedDiff.first.surfaces.resize(6);
        acceptedDiff.second.surfaces.resize(6);
        for (std::size_t i = 0; i < acceptedDiff.first.surfaces.size(); ++i)
        {
            acceptedDiff.first.surfaces[i].ordinal = static_cast<std::uint32_t>(i);
            acceptedDiff.second.surfaces[i].ordinal = static_cast<std::uint32_t>(i);
            acceptedDiff.first.surfaces[i].terminal = RtPathTraceCaptureTerminal::Accepted;
            acceptedDiff.second.surfaces[i].terminal = i == 0
                ? RtPathTraceCaptureTerminal::Accepted
                : RtPathTraceCaptureTerminal::AdmissionRejected;
        }
        RtPathTraceCaptureSurfaceProduct& firstProductOnly =
            acceptedDiff.first.surfaces[1];
        RtPathTraceCaptureSurfaceProduct& firstOracleRejected =
            acceptedDiff.second.surfaces[1];
        firstProductOnly.surfaceClass = RtSmokeSurfaceClass::SkinnedDeformed;
        firstProductOnly.sourceFlags = 17;
        firstProductOnly.vertexCount = 44;
        firstProductOnly.indexCount = 90;
        firstProductOnly.rigidReadyByMesh = true;
        firstProductOnly.rigidReadyByResident = false;
        firstProductOnly.skinnedCaptureOmitted = false;
        firstProductOnly.skinnedAdmission = static_cast<std::uint32_t>(
            PtSkinnedCaptureAdmissionResult::GateDisabled);
        firstOracleRejected.surfaceClass = RtSmokeSurfaceClass::SkinnedDeformed;
        firstOracleRejected.sourceFlags = 33;
        firstOracleRejected.vertexCount = 0;
        firstOracleRejected.indexCount = 0;
        firstOracleRejected.rigidReadyByMesh = false;
        firstOracleRejected.rigidReadyByResident = true;
        firstOracleRejected.skinnedCaptureOmitted = true;
        firstOracleRejected.skinnedAdmission = static_cast<std::uint32_t>(
            PtSkinnedCaptureAdmissionResult::OmitCpuCapture);
        const RtPathTraceCaptureComparison acceptedSetComparison =
            ComparePathTraceCaptureProduct(acceptedDiff.first, acceptedDiff.second);
        Check(!acceptedSetComparison.exact &&
                acceptedSetComparison.firstFail == RtPathTraceCaptureFirstFailure::Vertices &&
                acceptedSetComparison.acceptedSetDiff.found &&
                acceptedSetComparison.acceptedSetDiff.ordinal == 1 &&
                acceptedSetComparison.acceptedSetDiff.product.present &&
                acceptedSetComparison.acceptedSetDiff.product.vertexCount == 44 &&
                acceptedSetComparison.acceptedSetDiff.product.indexCount == 90 &&
                acceptedSetComparison.acceptedSetDiff.product.rigidReadyByMesh &&
                !acceptedSetComparison.acceptedSetDiff.product.skinnedCaptureOmitted &&
                acceptedSetComparison.acceptedSetDiff.oracle.present &&
                acceptedSetComparison.acceptedSetDiff.oracle.skinnedCaptureOmitted &&
                acceptedSetComparison.acceptedSetDiff.oracle.skinnedAdmission ==
                    static_cast<std::uint32_t>(
                        PtSkinnedCaptureAdmissionResult::OmitCpuCapture),
            "accepted-set diagnostic preserves first vertex failure and all bounded scalars");
        acceptedDiff.second.surfaces.resize(1);
        const RtPathTraceCaptureComparison shortSurfaceComparison =
            ComparePathTraceCaptureProduct(acceptedDiff.first, acceptedDiff.second);
        Check(shortSurfaceComparison.acceptedSetDiff.found &&
                shortSurfaceComparison.acceptedSetDiff.ordinal == 1 &&
                !shortSurfaceComparison.acceptedSetDiff.oracle.present,
            "accepted-set diagnostic safely handles a shorter oracle surface vector");

        const auto acceptedSurface = [](std::uint32_t ordinal,
            RtPathTraceCaptureTerminal terminal)
        {
            RtPathTraceCaptureSurfaceProduct row;
            row.ordinal = ordinal;
            row.terminal = terminal;
            row.vertexCount = ordinal + 10;
            return row;
        };
        auto reordered = exactPair();
        reordered.first.surfaces = {
            acceptedSurface(5, RtPathTraceCaptureTerminal::Accepted),
            acceptedSurface(4, RtPathTraceCaptureTerminal::Accepted) };
        reordered.second.surfaces = {
            acceptedSurface(4, RtPathTraceCaptureTerminal::Accepted),
            acceptedSurface(5, RtPathTraceCaptureTerminal::Accepted) };
        Check(!ComparePathTraceCaptureProduct(
                    reordered.first, reordered.second).acceptedSetDiff.found,
            "reordered equal Accepted sets have no ordinal-keyed difference");

        auto lowest = exactPair();
        lowest.first.surfaces = {
            acceptedSurface(5, RtPathTraceCaptureTerminal::Accepted) };
        lowest.second.surfaces = {
            acceptedSurface(4, RtPathTraceCaptureTerminal::Accepted) };
        const auto lowestDiff = ComparePathTraceCaptureProduct(
            lowest.first, lowest.second).acceptedSetDiff;
        Check(lowestDiff.available && lowestDiff.found && lowestDiff.ordinal == 4 &&
                !lowestDiff.product.present && lowestDiff.oracle.present &&
                lowestDiff.oracle.vertexCount == 14,
            "accepted-set symmetric difference selects the lowest ordinal");

        auto interiorGap = exactPair();
        interiorGap.first.surfaces = {
            acceptedSurface(1, RtPathTraceCaptureTerminal::Accepted),
            acceptedSurface(3, RtPathTraceCaptureTerminal::Accepted) };
        interiorGap.second.surfaces = {
            acceptedSurface(1, RtPathTraceCaptureTerminal::Accepted),
            acceptedSurface(2, RtPathTraceCaptureTerminal::Accepted),
            acceptedSurface(3, RtPathTraceCaptureTerminal::Accepted) };
        Check(ComparePathTraceCaptureProduct(interiorGap.first, interiorGap.second)
                    .acceptedSetDiff.ordinal == 2,
            "accepted-set symmetric difference handles an interior gap");

        auto equalLengthDifferentOrdinals = exactPair();
        equalLengthDifferentOrdinals.first.surfaces = {
            acceptedSurface(8, RtPathTraceCaptureTerminal::Accepted) };
        equalLengthDifferentOrdinals.second.surfaces = {
            acceptedSurface(7, RtPathTraceCaptureTerminal::Accepted) };
        Check(ComparePathTraceCaptureProduct(equalLengthDifferentOrdinals.first,
                    equalLengthDifferentOrdinals.second).acceptedSetDiff.ordinal == 7,
            "equal-length different ordinal vectors compare by ordinal, not position");

        auto shorterNonTrailing = exactPair();
        shorterNonTrailing.first.surfaces = {
            acceptedSurface(1, RtPathTraceCaptureTerminal::Accepted),
            acceptedSurface(3, RtPathTraceCaptureTerminal::Accepted) };
        shorterNonTrailing.second.surfaces = {
            acceptedSurface(1, RtPathTraceCaptureTerminal::Accepted),
            acceptedSurface(2, RtPathTraceCaptureTerminal::Accepted),
            acceptedSurface(3, RtPathTraceCaptureTerminal::Accepted) };
        Check(ComparePathTraceCaptureProduct(shorterNonTrailing.first,
                    shorterNonTrailing.second).acceptedSetDiff.ordinal == 2,
            "shorter nontrailing Accepted vector does not pair unrelated rows");

        auto duplicateProduct = exactPair();
        duplicateProduct.first.surfaces = {
            acceptedSurface(2, RtPathTraceCaptureTerminal::Accepted),
            acceptedSurface(2, RtPathTraceCaptureTerminal::AdmissionRejected) };
        duplicateProduct.second.surfaces = {
            acceptedSurface(2, RtPathTraceCaptureTerminal::Accepted) };
        const auto duplicateProductDiff = ComparePathTraceCaptureProduct(
            duplicateProduct.first, duplicateProduct.second).acceptedSetDiff;
        Check(!duplicateProductDiff.available && !duplicateProductDiff.found &&
                duplicateProductDiff.ordinal == UINT32_MAX,
            "duplicate product ordinal makes Accepted-set diagnostic unavailable");
        auto duplicateOracle = exactPair();
        duplicateOracle.first.surfaces = {
            acceptedSurface(2, RtPathTraceCaptureTerminal::Accepted) };
        duplicateOracle.second.surfaces = {
            acceptedSurface(2, RtPathTraceCaptureTerminal::Accepted),
            acceptedSurface(2, RtPathTraceCaptureTerminal::AdmissionRejected) };
        const auto duplicateOracleDiff = ComparePathTraceCaptureProduct(
            duplicateOracle.first, duplicateOracle.second).acceptedSetDiff;
        Check(!duplicateOracleDiff.available && !duplicateOracleDiff.found,
            "duplicate oracle ordinal makes Accepted-set diagnostic unavailable");

        auto mixedTerminal = exactPair();
        mixedTerminal.first.surfaces = {
            acceptedSurface(9, RtPathTraceCaptureTerminal::Accepted) };
        mixedTerminal.second.surfaces = {
            acceptedSurface(9, RtPathTraceCaptureTerminal::AdmissionRejected) };
        const auto mixedTerminalComparison = ComparePathTraceCaptureProduct(
            mixedTerminal.first, mixedTerminal.second);
        Check(mixedTerminalComparison.acceptedSetDiff.found &&
                mixedTerminalComparison.acceptedSetDiff.ordinal == 9 &&
                mixedTerminalComparison.acceptedSetDiff.product.present &&
                mixedTerminalComparison.acceptedSetDiff.oracle.present &&
                mixedTerminalComparison.firstFail ==
                    RtPathTraceCaptureFirstFailure::SurfaceDecision &&
                mixedTerminalComparison.geometryMismatches == 0 &&
                mixedTerminalComparison.proposalMismatches == 1 &&
                !mixedTerminalComparison.exact,
            "mixed terminal at one ordinal preserves legacy comparison semantics");
    }

    {
        const std::uint32_t omit = static_cast<std::uint32_t>(
            PtSkinnedCaptureAdmissionResult::OmitCpuCapture);
        std::uint64_t omittedVertices = 0;
        std::uint64_t omittedIndexes = 0;
        for (int row = 0; row < 7; ++row)
        {
            RtPathTraceCaptureSurfaceProduct surface;
            const std::uint32_t vertices = row == 6 ? 679u : 500u;
            const std::uint32_t indexes = row == 6 ? 2736u : 2502u;
            Check(ApplyPathTraceSkinnedCaptureAdmissionFromPod(
                    omit, vertices, indexes, surface) &&
                    surface.terminal == RtPathTraceCaptureTerminal::AdmissionRejected &&
                    surface.skinnedCaptureOmitted &&
                    surface.skinnedAdmission == omit &&
                    surface.skinnedRecordPresent &&
                    surface.skinnedRecordVertexCount == vertices &&
                    surface.skinnedRecordIndexCount == indexes,
                "GEO08 worker omits an admitted skinned CPU capture row");
            omittedVertices += vertices;
            omittedIndexes += indexes;
        }
        Check(omittedVertices == 3679 && omittedIndexes == 17748,
            "seven GEO08 omission rows pin the human capture cardinality");
        const PtSkinnedCaptureAdmissionResult fallbacks[] = {
            PtSkinnedCaptureAdmissionResult::GateDisabled,
            PtSkinnedCaptureAdmissionResult::MissingPriorRoute,
            PtSkinnedCaptureAdmissionResult::PriorRouteNotLive,
            PtSkinnedCaptureAdmissionResult::CurrentInstanceMismatch,
            PtSkinnedCaptureAdmissionResult::CurrentSourceMismatch,
            PtSkinnedCaptureAdmissionResult::JointDataNotReady };
        for (PtSkinnedCaptureAdmissionResult fallback : fallbacks)
        {
            RtPathTraceCaptureSurfaceProduct surface;
            Check(!ApplyPathTraceSkinnedCaptureAdmissionFromPod(
                    static_cast<std::uint32_t>(fallback), 3, 3, surface) &&
                    !surface.skinnedCaptureOmitted,
                "non-omit GEO08 admission result preserves CPU append");
        }

        RtPathTraceCaptureRawSurface differingRaw;
        differingRaw.vertexCount = 999;
        differingRaw.sourceTriIndexCount = 3000;
        differingRaw.skinnedCaptureAdmission = omit;
        differingRaw.skinnedOmitVertexCount = 17;
        differingRaw.skinnedOmitIndexCount = 24;
        RtPathTraceCaptureSurfaceProduct canonicalCounts;
        Check(ApplyPathTraceSkinnedCaptureAdmissionFromPod(
                differingRaw.skinnedCaptureAdmission,
                differingRaw.skinnedOmitVertexCount,
                differingRaw.skinnedOmitIndexCount, canonicalCounts) &&
                canonicalCounts.skinnedRecordVertexCount == 17 &&
                canonicalCounts.skinnedRecordIndexCount == 24 &&
                canonicalCounts.skinnedRecordTriangleCount == 8 &&
                canonicalCounts.skinnedRecordVertexCount != differingRaw.vertexCount &&
                canonicalCounts.skinnedRecordIndexCount !=
                    differingRaw.sourceTriIndexCount,
            "GEO08 omission records canonical counts and excludes differing append geometry");
        const std::pair<std::uint32_t, std::uint32_t> invalidOmitCounts[] = {
            { 0u, 3u }, { 3u, 0u }, { 3u, 4u } };
        for (const auto& invalid : invalidOmitCounts)
        {
            RtPathTraceCaptureSurfaceProduct rejected;
            Check(ApplyPathTraceSkinnedCaptureAdmissionFromPod(
                    omit, invalid.first, invalid.second, rejected) &&
                    rejected.terminal == RtPathTraceCaptureTerminal::SemanticRejected &&
                    !rejected.skinnedCaptureOmitted &&
                    !rejected.skinnedRecordPresent,
                "invalid canonical omit counts fail closed before geometry append");
        }

        PtSkinnedHitRouteRecord route;
        route.instanceKey.worldGeneration = 1;
        route.instanceKey.renderDefIndex = 1;
        route.instanceKey.renderDefGeneration = 1;
        route.instanceKey.subInstanceKind = PtCanonicalSubInstanceKind::SkinnedSurface;
        route.instanceKey.modelSurfaceIndex = 0;
        route.meshKey.sourceAssetId = 1;
        route.meshKey.sourceAssetGeneration = 1;
        route.meshKey.topologySignature = 1;
        route.meshKey.sourceDomain = PtCanonicalMeshSourceDomain::SkinnedBindSource;
        route.meshKey.modelSurfaceIndex = 0;
        route.meshKey.deformationClass = PtCanonicalDeformationClass::Skinned;
        route.meshKey.vertexCount = 3;
        route.meshKey.indexCount = 3;
        route.sourceChecksum = 99;
        route.vertexCount = 3;
        route.indexCount = 3;
        route.triangleCount = 1;
        PtSkinnedCaptureAdmissionInput input;
        input.gate = true;
        input.currentInstance = route.instanceKey;
        input.currentMesh = route.meshKey;
        input.currentSourceChecksum = route.sourceChecksum;
        input.currentVertexCount = route.vertexCount;
        input.currentIndexCount = route.indexCount;
        input.jointDataReady = true;
        input.priorRouteLive = true;
        input.priorRoute = &route;
        Check(PtPlanSkinnedCaptureAdmission(input) ==
                PtSkinnedCaptureAdmissionResult::OmitCpuCapture,
            "shared planner produces the snapshot omission enum from repository POD");
        input.priorRoute = nullptr;
        Check(PtPlanSkinnedCaptureAdmission(input) ==
                PtSkinnedCaptureAdmissionResult::MissingPriorRoute,
            "empty route snapshot fails closed to CPU fallback");
        RtPathTraceCaptureRawSurface resetRaw;
        resetRaw.skinnedCaptureAdmission = omit;
        resetRaw = {};
        Check(resetRaw.skinnedCaptureAdmission == static_cast<std::uint32_t>(
                PtSkinnedCaptureAdmissionResult::GateDisabled),
            "new generation raw rows never reuse a prior admission enum");
    }

    {
        const auto vectorMismatch = [&](auto mutate, const char* name)
        {
            RtPathTraceCaptureProduct product;
            RtPathTraceCaptureOracle oracle;
            product.epoch = snapshot.epoch;
            oracle.epoch = snapshot.epoch;
            product.viewIdentity = 17;
            oracle.viewIdentity = 17;
            product.complete = true;
            oracle.complete = true;
            mutate(product);
            Check(!ComparePathTraceCaptureProduct(product, oracle).exact, name);
        };
        vectorMismatch([](auto& p) { p.vertices.emplace_back(); }, "vertex cardinality mutation");
        vectorMismatch([](auto& p) { p.indexes.push_back(0); }, "index cardinality mutation");
        vectorMismatch([](auto& p) { p.triangleClasses.push_back(0); }, "class cardinality mutation");
        vectorMismatch([](auto& p) { p.triangleMaterials.push_back(0); }, "material cardinality mutation");
        vectorMismatch([](auto& p) { p.triangleInstances.push_back(0); }, "instance cardinality mutation");
        vectorMismatch([](auto& p) { p.triangleIdentities.emplace_back(); }, "identity cardinality mutation");
        vectorMismatch([](auto& p) { p.materialInfoIntents.emplace_back(); }, "material intent cardinality mutation");
        vectorMismatch([](auto& p) { p.materialVariants.emplace_back(); }, "variant cardinality mutation");
        vectorMismatch([](auto& p) { p.instanceObservations.emplace_back(); }, "observation cardinality mutation");
        vectorMismatch([](auto& p) { p.rigidCandidates.emplace_back(); }, "rigid cardinality mutation");
        vectorMismatch([](auto& p) { p.staticMembershipSurfaces.push_back(0); }, "static membership cardinality mutation");
        vectorMismatch([](auto& p) { p.routedReadySkipSurfaces.push_back(0); }, "routed membership cardinality mutation");
        vectorMismatch([](auto& p) { p.surfaces.emplace_back(); }, "surface cardinality mutation");
    }

    {
        RtPathTraceCaptureOwnerSnapshot convergenceSnapshot;
        convergenceSnapshot.capacityCounts.surfaces = 5;
        convergenceSnapshot.capacityCounts.vertices = 6;
        convergenceSnapshot.capacityCounts.indexes = 6;
        convergenceSnapshot.capacityCounts.instanceObservationProposals = 1;
        RtPathTraceCaptureProductCapacityPlan convergencePlan;
        Check(Plan(convergenceSnapshot.capacityCounts, convergencePlan),
            "integrated convergence fixture plans one bounded slot");
        RtPathTraceCaptureProduct candidate;
        Check(ReservePathTraceCaptureProductStorage(convergenceSnapshot,
                candidate,
                convergenceSnapshot.OwnedBytes() +
                    convergencePlan.oracleBytes,
                convergencePlan.candidateBytes),
            "integrated convergence fixture reserves one candidate");

        RtPathTracePlanningSnapshotEpoch epoch;
        epoch.generation = 44;
        epoch.frameIndex = 10;
        epoch.mapTimeStamp = 1;
        epoch.mapLoadSerial = 1;
        RtPathTracePlanningCopyName(epoch.mapName, sizeof(epoch.mapName), "converge");
        epoch.capturedAfterBeginFrame = true;
        epoch.capturedAfterStaticPreload = true;
        epoch.capturedBeforeSerialMutate = true;
        candidate.epoch = epoch;
        candidate.viewIdentity = 440;

        const RtSmokeSurfaceClass acceptedClasses[2] = {
            RtSmokeSurfaceClass::ParticleAlpha,
            RtSmokeSurfaceClass::RigidEntity };
        for (std::uint32_t ordinal = 0; ordinal < 2; ++ordinal)
        {
            RtPathTraceCaptureSurfaceProduct surface;
            surface.ordinal = ordinal;
            surface.terminal = RtPathTraceCaptureTerminal::Accepted;
            surface.surfaceClass = acceptedClasses[ordinal];
            surface.surfaceClassId = static_cast<std::uint32_t>(acceptedClasses[ordinal]);
            surface.bucketIndex = static_cast<std::int32_t>(acceptedClasses[ordinal]);
            surface.vertexOffset = ordinal * 3;
            surface.vertexCount = 3;
            surface.indexOffset = ordinal * 3;
            surface.indexCount = 3;
            surface.triangleOffset = ordinal;
            surface.triangleCount = 1;
            candidate.surfaces.push_back(surface);
            for (std::uint32_t local = 0; local < 3; ++local)
            {
                PathTraceSmokeVertex vertex = {};
                vertex.position[0] = static_cast<float>(ordinal * 3 + local);
                candidate.vertices.push_back(vertex);
                candidate.indexes.push_back(ordinal * 3 + local);
            }
            candidate.triangleClasses.push_back(
                BuildPathTraceCaptureTriangleClassWordFromPod(
                    surface.surfaceClassId, ordinal != 0, ordinal == 1));
            candidate.triangleMaterials.push_back(100 + ordinal);
            candidate.triangleInstances.push_back(200 + ordinal);
            candidate.triangleIdentities.push_back(
                BuildPathTraceCaptureTriangleIdentityFromPod(
                    3, 4, 100 + ordinal, 0xabcdu + ordinal, 0));
        }
        RtPathTraceCaptureSurfaceProduct rigidReady;
        rigidReady.ordinal = 2;
        rigidReady.surfaceClass = RtSmokeSurfaceClass::RigidEntity;
        rigidReady.rigidReadyByMesh = true;
        rigidReady.terminal = PathTraceCaptureShouldOmitRoutedRigidFromPod(
                true, rigidReady.surfaceClass,
                rigidReady.rigidReadyByMesh, rigidReady.rigidReadyByResident)
            ? RtPathTraceCaptureTerminal::RoutedRigidReady
            : RtPathTraceCaptureTerminal::Accepted;
        candidate.surfaces.push_back(rigidReady);
        candidate.routedReadySkipSurfaces.push_back(2);

        RtPathTraceCaptureSurfaceProduct skinnedOmit;
        skinnedOmit.ordinal = 3;
        skinnedOmit.surfaceClass = RtSmokeSurfaceClass::SkinnedDeformed;
        Check(ApplyPathTraceSkinnedCaptureAdmissionFromPod(
                static_cast<std::uint32_t>(
                    PtSkinnedCaptureAdmissionResult::OmitCpuCapture),
                3679, 17748, skinnedOmit),
            "integrated convergence fixture applies canonical GEO08 omission");
        candidate.surfaces.push_back(skinnedOmit);

        RtPathTraceCaptureSurfaceProduct zeroArea;
        zeroArea.ordinal = 4;
        zeroArea.surfaceClass = RtSmokeSurfaceClass::RigidEntity;
        zeroArea.terminal = RtPathTraceCaptureTerminal::RolledBack;
        zeroArea.zeroAreaTriangleCount = 1;
        zeroArea.rollbackApplied = true;
        candidate.surfaces.push_back(zeroArea);

        RtPathTraceInstanceHistoryPod history;
        history.instanceId = 201;
        history.lastSeenFrame = 9;
        history.lastObjectToWorld[0] = 1.0f;
        history.lastObjectToWorld[5] = 1.0f;
        history.lastObjectToWorld[10] = 1.0f;
        history.lastObjectToWorld[15] = 1.0f;
        float currentObjectToWorld[16] = {};
        currentObjectToWorld[0] = 1.0f;
        currentObjectToWorld[5] = 1.0f;
        currentObjectToWorld[10] = 1.0f;
        currentObjectToWorld[12] = 5.0f;
        currentObjectToWorld[15] = 1.0f;
        const RtPathTraceInstanceHistoryApplication historyResult =
            ApplyInstanceHistoryRowsFromPod(
                epoch, &history, 1, history.instanceId,
                currentObjectToWorld, epoch.frameIndex);
        RtPathTraceInstanceObservationProposal observation;
        observation.surfaceOrdinal = 1;
        observation.instanceId = history.instanceId;
        observation.hasPrevious = historyResult.hasPreviousObjectToWorld;
        observation.transformContinuous = historyResult.transformContinuous;
        std::memcpy(observation.currentObjectToWorld,
            historyResult.currentObjectToWorld,
            sizeof(observation.currentObjectToWorld));
        std::memcpy(observation.previousObjectToWorld,
            historyResult.previousObjectToWorld,
            sizeof(observation.previousObjectToWorld));
        candidate.instanceObservations.push_back(observation);

        RtPathTraceCaptureProduct product;
        const bool partitioned = ReserveFinal(
                convergenceSnapshot, convergencePlan, candidate, product) &&
            PartitionPathTraceCaptureProductBucketMajor(
                convergenceSnapshot, candidate, product);
        Check(partitioned,
            "integrated convergence fixture partitions the complete candidate");
        if (!partitioned)
        {
            return 1;
        }
        product.complete = true;
        Check(product.vertices.size() == 6 && product.indexes.size() == 6 &&
                product.surfaces[2].vertexCount == 0 &&
                product.surfaces[3].vertexCount == 0 &&
                product.surfaces[4].vertexCount == 0,
            "ready, omitted, and zero-area surfaces publish no geometry");
        Check(product.triangleClasses.size() == 2 &&
                (product.triangleClasses[0] &
                    RT_SMOKE_TRIANGLE_FORCE_GEOMETRIC_NORMAL) != 0 &&
                (product.triangleClasses[1] &
                    RT_SMOKE_TRIANGLE_EMISSIVE_STAGE_OFF) != 0,
            "bucket-major output retains live emissive-off and geometric-normal flags");
        Check(product.triangleIdentities.size() == 2 &&
                product.triangleIdentities[0] ==
                    BuildPathTraceCaptureTriangleIdentityFromPod(
                        3, 4, 101, 0xabceu, 0) &&
                product.triangleIdentities[1] ==
                    BuildPathTraceCaptureTriangleIdentityFromPod(
                        3, 4, 100, 0xabcdu, 0),
            "skipped source triangles do not advance compact emitted identities");
        Check(product.surfaces[1].vertexOffset == 0 &&
                product.surfaces[0].vertexOffset == 3 &&
                product.surfaces[1].indexOffset == 0 &&
                product.surfaces[0].indexOffset == 3 &&
                product.surfaces[1].triangleOffset == 0 &&
                product.surfaces[0].triangleOffset == 1,
            "integrated convergence offsets use canonical global bucket-major space");
        Check(product.instanceObservations.size() == 1 &&
                product.instanceObservations[0].hasPrevious &&
                product.instanceObservations[0].transformContinuous &&
                product.instanceObservations[0].currentObjectToWorld[12] == 5.0f &&
                product.instanceObservations[0].previousObjectToWorld[12] == 0.0f,
            "durable observation uses current raw transform and prior snapshot history");

        RtPathTraceCaptureOracle oracle;
        oracle.epoch = product.epoch;
        oracle.viewIdentity = product.viewIdentity;
        oracle.vertices = product.vertices;
        oracle.indexes = product.indexes;
        oracle.triangleClasses = product.triangleClasses;
        oracle.triangleMaterials = product.triangleMaterials;
        oracle.triangleInstances = product.triangleInstances;
        oracle.triangleIdentities = product.triangleIdentities;
        oracle.instanceObservations = product.instanceObservations;
        oracle.routedReadySkipSurfaces = product.routedReadySkipSurfaces;
        oracle.surfaces = product.surfaces;
        oracle.complete = true;
        const RtPathTraceCaptureComparison exact =
            ComparePathTraceCaptureProduct(product, oracle);
        Check(exact.exact && !exact.acceptedSetDiff.found &&
                !exact.instanceObservationDiff.found,
            "integrated convergence fixture matches Accepted set and observation oracle");
        oracle.instanceObservations[0].transformContinuous = false;
        const RtPathTraceCaptureComparison observationMismatch =
            ComparePathTraceCaptureProduct(product, oracle);
        Check(!observationMismatch.exact &&
                observationMismatch.instanceObservationDiff.found &&
                observationMismatch.instanceObservationDiff.ordinal == 1 &&
                observationMismatch.instanceObservationDiff.productHasPrevious &&
                observationMismatch.instanceObservationDiff.productTransformContinuous &&
                observationMismatch.instanceObservationDiff.oracleHasPrevious &&
                !observationMismatch.instanceObservationDiff.oracleTransformContinuous,
            "observation first difference reports bounded continuity fields");
        oracle.instanceObservations[0].transformContinuous = true;
        oracle.instanceObservations[0].currentObjectToWorld[12] = 6.0f;
        const RtPathTraceCaptureComparison transformMismatch =
            ComparePathTraceCaptureProduct(product, oracle);
        Check(!transformMismatch.exact &&
                transformMismatch.instanceObservationDiff.found &&
                transformMismatch.instanceObservationDiff.ordinal == 1 &&
                transformMismatch.instanceObservationDiff.productHasPrevious &&
                transformMismatch.instanceObservationDiff.oracleHasPrevious,
            "observation first difference covers current-transform mismatches");
    }

    snapshot.ResetAndRelease();
    Check(snapshot.OwnedBytes() == sizeof(snapshot),
        "transaction snapshot ResetAndRelease drops all capacity");

    std::cout << (g_failures == 0
        ? "PathTrace capture product transaction harness passed\n"
        : "PathTrace capture product transaction harness FAILED\n");
    return g_failures == 0 ? 0 : 1;
}
