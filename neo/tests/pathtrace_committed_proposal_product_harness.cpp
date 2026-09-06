#include "../idlib/precompiled.h"
#pragma hdrstop

#include "../renderer/NVRHI/PathTraceCommittedProposalProduct.h"
#include "../renderer/NVRHI/PathTraceRigidIdentity.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>

idCommon* idLib::common = nullptr;

bool AssertFailed(const char*, int, const char*)
{
    return false;
}

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
    if (pointer)
    {
        std::memset(pointer, 0, size);
    }
    return pointer;
}

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

bool ResolvePathTraceCaptureSerialRigidIdentitySurfaceFromSnapshot(
    std::uint64_t, std::uint64_t, std::int32_t, std::uint64_t,
    std::int32_t&, bool&, bool& comparedCall)
{
    comparedCall = false;
    return false;
}

namespace {

int failures = 0;

void Check(bool condition, const char* label)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << label << '\n';
    failures += condition ? 0 : 1;
}

template<typename T>
bool PodVectorEqual(const std::vector<T>& lhs, const std::vector<T>& rhs)
{
    return lhs.size() == rhs.size() &&
        (lhs.empty() || std::memcmp(lhs.data(), rhs.data(),
            lhs.size() * sizeof(T)) == 0);
}

bool ProductEqual(const RtPathTraceCommittedProposalProduct& lhs,
    const RtPathTraceCommittedProposalProduct& rhs)
{
    const auto receiptEqual = [](const auto& a, const auto& b)
    {
        return a.sealedPrimaryViewToken == b.sealedPrimaryViewToken &&
            a.ownerUniverseFrameIndex == b.ownerUniverseFrameIndex &&
            a.baselineFrameIndex == b.baselineFrameIndex &&
            a.worldLifecycleGeneration == b.worldLifecycleGeneration &&
            a.mapLoadSerial == b.mapLoadSerial &&
            a.mapTimeStamp == b.mapTimeStamp &&
            std::memcmp(a.mapName, b.mapName, sizeof(a.mapName)) == 0 &&
            a.barrierGeneration == b.barrierGeneration &&
            a.materialRegistryGeneration == b.materialRegistryGeneration &&
            a.residentMaterialFactsGeneration == b.residentMaterialFactsGeneration &&
            a.configFingerprint == b.configFingerprint &&
            a.instanceUniverseGeneration == b.instanceUniverseGeneration &&
            a.geometryUniverseGeneration == b.geometryUniverseGeneration &&
            a.staticMaterialGeneration == b.staticMaterialGeneration &&
            a.canonicalSourceIndexPoolGeneration == b.canonicalSourceIndexPoolGeneration &&
            a.staticResidentPayloadGeneration == b.staticResidentPayloadGeneration &&
            a.finalizedMaterialIdHash == b.finalizedMaterialIdHash &&
            a.surfaceCount == b.surfaceCount &&
            a.finalizedMaterialIdCount == b.finalizedMaterialIdCount &&
            a.recordAllInstanceClasses == b.recordAllInstanceClasses &&
            a.complete == b.complete;
    };
    return lhs.planningEpoch.generation == rhs.planningEpoch.generation &&
        lhs.planningEpoch.frameIndex == rhs.planningEpoch.frameIndex &&
        lhs.planningEpoch.mapTimeStamp == rhs.planningEpoch.mapTimeStamp &&
        lhs.planningEpoch.mapLoadSerial == rhs.planningEpoch.mapLoadSerial &&
        std::memcmp(lhs.planningEpoch.mapName, rhs.planningEpoch.mapName,
            sizeof(lhs.planningEpoch.mapName)) == 0 &&
        lhs.planningEpoch.capturedAfterBeginFrame ==
            rhs.planningEpoch.capturedAfterBeginFrame &&
        lhs.planningEpoch.capturedAfterStaticPreload ==
            rhs.planningEpoch.capturedAfterStaticPreload &&
        lhs.planningEpoch.capturedBeforeSerialMutate ==
            rhs.planningEpoch.capturedBeforeSerialMutate &&
        std::memcmp(&lhs.capacityCounts, &rhs.capacityCounts,
            sizeof(lhs.capacityCounts)) == 0 &&
        lhs.inputOwnedBytes == rhs.inputOwnedBytes &&
        lhs.actualOwnedBytes == rhs.actualOwnedBytes &&
        lhs.provenance == rhs.provenance &&
        lhs.complete == rhs.complete &&
        receiptEqual(lhs.receiptHeader, rhs.receiptHeader) &&
        PodVectorEqual(lhs.materialInfoIntents, rhs.materialInfoIntents) &&
        PodVectorEqual(lhs.materialVariants, rhs.materialVariants) &&
        PodVectorEqual(lhs.instanceObservations, rhs.instanceObservations) &&
        PodVectorEqual(lhs.rigidCandidates, rhs.rigidCandidates) &&
        PodVectorEqual(lhs.receiptSurfaces, rhs.receiptSurfaces);
}

std::uint64_t AppendDigest(
    std::uint64_t hash, const void* bytes, std::size_t count)
{
    const std::uint8_t* input = static_cast<const std::uint8_t*>(bytes);
    for (std::size_t index = 0; index < count; ++index)
    {
        hash ^= input[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

template<typename T>
void AppendVectorDigest(std::uint64_t& hash, const std::vector<T>& values)
{
    const std::size_t size = values.size();
    hash = AppendDigest(hash, &size, sizeof(size));
    if (!values.empty())
    {
        hash = AppendDigest(hash, values.data(), values.size() * sizeof(T));
    }
}

std::uint64_t ScratchDigest(const RtPathTracePrimarySemanticScratch& scratch)
{
    std::uint64_t hash = 14695981039346656037ull;
    hash = AppendDigest(hash, &scratch.sealedPrimaryViewToken,
        sizeof(scratch.sealedPrimaryViewToken));
    hash = AppendDigest(hash, &scratch.ownedBytes, sizeof(scratch.ownedBytes));
    hash = AppendDigest(hash, &scratch.complete, sizeof(scratch.complete));
    hash = AppendDigest(hash, &scratch.snapshot.recordAllInstanceClasses,
        sizeof(scratch.snapshot.recordAllInstanceClasses));
    hash = AppendDigest(hash, &scratch.snapshot.complete,
        sizeof(scratch.snapshot.complete));
    hash = AppendDigest(hash, &scratch.snapshot.registryGeneration,
        sizeof(scratch.snapshot.registryGeneration));
    hash = AppendDigest(hash, &scratch.snapshot.residentMaterialFactsGeneration,
        sizeof(scratch.snapshot.residentMaterialFactsGeneration));
    hash = AppendDigest(hash, &scratch.snapshot.capacityCounts,
        sizeof(scratch.snapshot.capacityCounts));
    AppendVectorDigest(hash, scratch.snapshot.surfaces);
    AppendVectorDigest(hash, scratch.snapshot.classifierStages);
    AppendVectorDigest(hash, scratch.snapshot.runtimeStages);
    AppendVectorDigest(hash, scratch.snapshot.registers);
    AppendVectorDigest(hash, scratch.snapshot.vertices);
    AppendVectorDigest(hash, scratch.snapshot.indexes);
    AppendVectorDigest(hash, scratch.snapshot.joints);
    AppendVectorDigest(hash, scratch.snapshot.modelTables);
    AppendVectorDigest(hash, scratch.snapshot.modelSurfaceTokens);
    AppendVectorDigest(hash, scratch.snapshot.variantBases);
    AppendVectorDigest(hash, scratch.snapshot.registryMaterials);
    return hash;
}

std::uint64_t BaselineDigest(
    const RtPathTraceCommittedPlanningBaseline& baseline)
{
    std::uint64_t hash = 14695981039346656037ull;
    hash = AppendDigest(hash, &baseline.epoch, sizeof(baseline.epoch));
    hash = AppendDigest(hash, &baseline.dispatchGuard,
        sizeof(baseline.dispatchGuard));
    hash = AppendDigest(hash, &baseline.semanticConfig,
        sizeof(baseline.semanticConfig));
    hash = AppendDigest(hash, &baseline.instanceUniverse.epoch,
        sizeof(baseline.instanceUniverse.epoch));
    hash = AppendDigest(hash, &baseline.instanceUniverse.ownerGeneration,
        sizeof(baseline.instanceUniverse.ownerGeneration));
    hash = AppendDigest(hash, &baseline.geometryUniverse.epoch,
        sizeof(baseline.geometryUniverse.epoch));
    hash = AppendDigest(hash, &baseline.geometryUniverse.ownerGeneration,
        sizeof(baseline.geometryUniverse.ownerGeneration));
    AppendVectorDigest(hash, baseline.instanceUniverse.meshes);
    AppendVectorDigest(hash, baseline.instanceUniverse.histories);
    AppendVectorDigest(hash, baseline.geometryUniverse.staticSurfaces);
    AppendVectorDigest(hash, baseline.geometryUniverse.rigidRoutes);
    AppendVectorDigest(hash, baseline.geometryUniverse.rigidResidents);
    AppendVectorDigest(hash, baseline.applyGate.omittedSkinKeys);
    AppendVectorDigest(hash, baseline.priorSkinnedRecords);
    hash = AppendDigest(hash, &baseline.complete, sizeof(baseline.complete));
    return hash;
}

RtPathTraceCaptureRawSurface MakeRow(std::uint32_t ordinal,
    std::uint32_t baseMaterialId, std::uint32_t chosenMaterialId,
    bool rigidIdentity, bool fallback)
{
    RtPathTraceCaptureRawSurface raw;
    raw.ordinal = ordinal;
    raw.safety = RtPathTraceCaptureSafetyDisposition::Ready;
    raw.semanticFactsDerived = true;
    raw.semanticFactsPresent = true;
    raw.derivedRuntimeMaterialPresent = true;
    raw.derivedBaseMaterialId = baseMaterialId;
    raw.derivedRuntimeMaterial.initialCandidateId = baseMaterialId + 1000u;
    raw.derivedRuntimeMaterial.chosenMaterialId = chosenMaterialId;
    raw.derivedRuntimeMaterial.collisionCount = fallback
        ? RT_PT_RUNTIME_MATERIAL_MAX_COLLISION_PROBES : 2u;
    raw.derivedRuntimeMaterial.fallbackUsed = fallback;
    raw.derivedChosenMaterialId = chosenMaterialId;
    raw.entityIndex = static_cast<std::int32_t>(10 + ordinal);
    raw.entityNum = static_cast<std::int32_t>(20 + ordinal);
    raw.renderWorldIdentity = 0x10000ull;
    raw.renderDefIndex = static_cast<std::int32_t>(30 + ordinal);
    raw.renderDefGeneration = 4;
    raw.modelEpoch = 5;
    raw.jointIndex = -1;
    raw.vertexCount = 3;
    raw.sourceTriIndexCount = 3;
    raw.derivedResolvedModelSurfaceIndex = rigidIdentity ? 2 : -1;
    raw.rigidIdentityPresent = rigidIdentity;
    raw.derivedMeshHash = rigidIdentity ? 0xabc000ull + ordinal : 0;
    raw.derivedMaterialClassSignature = rigidIdentity ? 0x55u : 0;
    raw.classify.material.deform = DFRM_NONE;
    raw.classify.material.materialPresent = true;
    raw.objectToWorld[0] = 1.0f;
    raw.objectToWorld[5] = 1.0f;
    raw.objectToWorld[10] = 1.0f;
    raw.objectToWorld[15] = 1.0f;
    raw.objectToWorld[12] = static_cast<float>(ordinal + 1);
    std::snprintf(raw.materialName, sizeof(raw.materialName),
        "textures/p2c2/%u", ordinal);
    return raw;
}

std::uint64_t InstanceIdFor(const RtPathTraceCaptureRawSurface& raw)
{
    RtPathTraceRigidInstanceIdentityPod identity;
    identity.meshHash = raw.derivedMeshHash;
    identity.renderWorldIdentity = raw.renderWorldIdentity;
    identity.renderDefIndex = raw.renderDefIndex;
    identity.renderDefGeneration = raw.renderDefGeneration;
    identity.modelEpoch = static_cast<std::uint32_t>(raw.modelEpoch);
    identity.entityIndex = raw.entityIndex;
    identity.renderEntityNum = raw.entityNum;
    identity.modelSurfaceIndex = raw.derivedResolvedModelSurfaceIndex;
    identity.materialId = raw.derivedChosenMaterialId;
    identity.jointIndex = raw.jointIndex;
    return BuildPathTraceRigidInstanceIdFromPod(identity);
}

struct Fixture
{
    RtPathTraceCommittedPlanningBaseline baseline;
    RtPathTracePrimarySemanticScratch scratch;

    Fixture()
    {
        baseline.epoch.committedViewToken = 69;
        baseline.epoch.sealedPrimaryViewToken = 70;
        baseline.epoch.baselineFrameIndex = 100;
        baseline.epoch.ownerUniverseFrameIndex = 900;
        baseline.epoch.worldLifecycleGeneration = 3;
        baseline.epoch.mapLoadSerial = 4;
        baseline.epoch.mapTimeStamp = 5;
        baseline.epoch.barrierGeneration = 6;
        baseline.epoch.materialRegistryGeneration = 15;
        baseline.epoch.residentMaterialFactsGeneration = 16;
        baseline.epoch.instanceUniverseGeneration = 9;
        baseline.epoch.geometryUniverseGeneration = 7;
        baseline.epoch.staticMaterialGeneration = 8;
        baseline.epoch.canonicalSourceIndexPoolGeneration = 17;
        baseline.epoch.staticResidentPayloadGeneration = 18;
        baseline.epoch.capturedAfterRootEndFrame = true;
        baseline.epoch.capturedAfterStaticPrune = true;
        RtPathTracePlanningCopyName(baseline.epoch.mapName,
            sizeof(baseline.epoch.mapName), "maps/p2c2");
        const RtPathTracePlanningSnapshotEpoch planning =
            RtPathTraceMapCommittedBaselineToPlanningEpoch(baseline.epoch);
        baseline.instanceUniverse.epoch = planning;
        baseline.instanceUniverse.ownerGeneration = 9;
        baseline.instanceUniverse.complete = true;
        baseline.geometryUniverse.epoch = planning;
        baseline.geometryUniverse.ownerGeneration = 7;
        baseline.geometryUniverse.complete = true;
        baseline.semanticConfig.recordAllInstanceClasses = false;
        baseline.semanticConfig.removeRoutedRigidDynamic = true;
        baseline.semanticConfig.rigidRouteEmissiveCards = false;
        baseline.semanticConfig.configFingerprint = 0x1234;
        baseline.semanticConfig.configComplete = true;
        baseline.applyGateComplete = true;
        baseline.priorSkinnedRecordsComplete = true;
        baseline.complete = true;

        scratch.sealedPrimaryViewToken = 71;
        scratch.snapshot.complete = true;
        scratch.snapshot.registryGeneration = 15;
        scratch.snapshot.residentMaterialFactsGeneration = 16;
        scratch.snapshot.recordAllInstanceClasses = false;
        scratch.snapshot.surfaces.push_back(MakeRow(0, 100, 200, true, false));
        scratch.snapshot.surfaces.push_back(MakeRow(1, 101, 201, false, false));
        scratch.snapshot.surfaces.push_back(MakeRow(2, 102, 102, true, true));
        scratch.snapshot.capacityCounts.surfaces =
            scratch.snapshot.surfaces.size();
        scratch.ownedBytes = sizeof(scratch) +
            scratch.snapshot.surfaces.capacity() *
                sizeof(RtPathTraceCaptureRawSurface);
        scratch.complete = true;

        for (const RtPathTraceCaptureRawSurface& raw : scratch.snapshot.surfaces)
        {
            if (!raw.rigidIdentityPresent)
            {
                continue;
            }
            RtPathTraceInstanceHistoryPod history;
            history.instanceId = InstanceIdFor(raw);
            history.lastSeenFrame = 899;
            history.lastObjectToWorld[0] = 1.0f;
            history.lastObjectToWorld[5] = 1.0f;
            history.lastObjectToWorld[10] = 1.0f;
            history.lastObjectToWorld[15] = 1.0f;
            history.lastObjectToWorld[12] = raw.objectToWorld[12] - 1.0f;
            baseline.instanceUniverse.histories.push_back(history);
        }
        std::sort(baseline.instanceUniverse.histories.begin(),
            baseline.instanceUniverse.histories.end(),
            [](const auto& lhs, const auto& rhs)
            {
                return lhs.instanceId < rhs.instanceId;
            });
    }
};

RtPathTraceCommittedProposalProduct SeedOutput()
{
    RtPathTraceCommittedProposalProduct output;
    RtPathTraceMaterialVariantRegistrationProposal sentinel;
    sentinel.surfaceOrdinal = 99;
    output.materialVariants.push_back(sentinel);
    output.actualOwnedBytes = output.OwnedBytes();
    return output;
}

void TestBuildAndOrder()
{
    Fixture fixture;
    const std::uint64_t scratchBefore = ScratchDigest(fixture.scratch);
    RtPathTraceCommittedProposalProduct first;
    const RtPathTraceCommittedProposalBuildResult result =
        BuildPathTraceCommittedProposalProductP2c2(
            fixture.scratch, fixture.baseline, first);
    Check(result.accepted &&
        first.complete && first.receiptHeader.complete &&
        first.provenance == RtPathTraceCommittedProposalProvenance::PureSemantic &&
        first.receiptSurfaces.size() == 3 &&
        first.materialInfoIntents.size() == 2 &&
        first.materialVariants.size() == 2 &&
        first.instanceObservations.size() == 2 &&
        first.rigidCandidates.size() == 2,
        "four-family exact counts build under one capacity authority");
    Check(first.materialInfoIntents.size() == first.materialVariants.size() &&
        first.materialInfoIntents[0].surfaceOrdinal == 0 &&
        first.materialInfoIntents[1].surfaceOrdinal == 1 &&
        first.instanceObservations[0].surfaceOrdinal == 0 &&
        first.instanceObservations[1].surfaceOrdinal == 2 &&
        first.rigidCandidates[0].surfaceOrdinal == 0 &&
        first.rigidCandidates[1].surfaceOrdinal == 2,
        "each uncoalesced family is stable surface-ordinal ascending");
    Check(first.materialInfoIntents.size() == first.materialVariants.size(),
        "material-info intent count remains paired with material-variant count");
    Check(first.materialVariants[0].baseMaterialId == 100 &&
        first.materialVariants[0].candidateId == 1100 &&
        first.materialVariants[0].chosenId == 200 &&
        first.materialVariants[0].collisionCount == 2 &&
        !first.materialVariants[0].fallbackUsed &&
        first.rigidCandidates[0].meshHash == 0xabc000ull &&
        first.rigidCandidates[0].materialId == 200,
        "proposal fields come exactly from persisted scratch decisions and identity");
    Check(first.materialInfoIntents[0].generation == 70 &&
        first.planningEpoch.generation == 70 &&
        first.planningEpoch.frameIndex == 900 &&
        first.instanceObservations[0].hasPrevious &&
        first.instanceObservations[0].transformContinuous &&
        first.instanceObservations[0].previousObjectToWorld[12] == 0.0f,
        "committed mapper and owner-universe frame drive local history application");
    Check(first.materialVariants.size() == 2 &&
        std::none_of(first.materialVariants.begin(), first.materialVariants.end(),
            [](const auto& value) { return value.surfaceOrdinal == 2; }) &&
        std::none_of(first.materialInfoIntents.begin(),
            first.materialInfoIntents.end(),
            [](const auto& value) { return value.surfaceOrdinal == 2; }),
        "exhausted fallback emits neither material family");
    Check(RT_PT_COMMITTED_PROPOSAL_APPLY_ORDER[0] ==
            RtPathTraceCommittedProposalFamily::MaterialInfoIntent &&
        RT_PT_COMMITTED_PROPOSAL_APPLY_ORDER[1] ==
            RtPathTraceCommittedProposalFamily::MaterialVariant &&
        RT_PT_COMMITTED_PROPOSAL_APPLY_ORDER[2] ==
            RtPathTraceCommittedProposalFamily::InstanceObservation &&
        RT_PT_COMMITTED_PROPOSAL_APPLY_ORDER[3] ==
            RtPathTraceCommittedProposalFamily::RigidCandidate,
        "documented apply order matches serial intent-variant-observation-rigid order");
    RtPathTraceCommittedProposalProduct second;
    const auto secondResult = BuildPathTraceCommittedProposalProductP2c2(
        fixture.scratch, fixture.baseline, second);
    Check(secondResult.accepted && ProductEqual(first, second),
        "identical scratch produces byte-identical deterministic proposals");
    Check(ScratchDigest(fixture.scratch) == scratchBefore,
        "accepted builder leaves P2b scratch byte-identical");
    Check(first.actualOwnedBytes == first.OwnedBytes() &&
        first.capacityCounts.receiptSurfaces ==
            fixture.scratch.snapshot.surfaces.size() &&
        first.receiptHeader.sealedPrimaryViewToken ==
            fixture.scratch.sealedPrimaryViewToken &&
        first.receiptHeader.instanceUniverseGeneration ==
            fixture.baseline.epoch.instanceUniverseGeneration,
        "receipt storage/header and actual owned bytes publish atomically");
    std::swap(first.materialVariants[0], first.materialVariants[1]);
    Check(!RtPathTraceCommittedProposalProductShapeValid(first),
        "proposal shape rejects malformed family ordinal order");
}

void TestCapacityAndAtomicFailure()
{
    RtPathTraceCaptureCapacityCounts counts;
    counts.surfaces = 1;
    counts.materialInfoIntents = 1;
    counts.materialVariantProposals = 1;
    counts.instanceObservationProposals = 1;
    counts.rigidCandidateProposals = 1;
    counts.receiptSurfaces = 1;
    RtPathTraceCommittedProposalCapacityPlan base;
    Check(PlanPathTraceCommittedProposalCapacity(0, counts, base),
        "proposal capacity base plan succeeds");
    RtPathTraceCommittedProposalCapacityPlan exact;
    const std::size_t exactInput = RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES -
        base.proposalOwnedBytes;
    Check(PlanPathTraceCommittedProposalCapacity(exactInput, counts, exact) &&
        exact.peakOwnedBytes == RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES,
        "proposal capacity accepts exactly-at-cap bytes");
    RtPathTraceCommittedProposalCapacityPlan rejected;
    Check(!PlanPathTraceCommittedProposalCapacity(exactInput + 1, counts,
            rejected),
        "proposal capacity rejects over-cap bytes");
    RtPathTraceCaptureCapacityCounts overflow = counts;
    overflow.surfaces = std::numeric_limits<std::size_t>::max();
    overflow.materialInfoIntents = overflow.surfaces;
    overflow.materialVariantProposals = overflow.surfaces;
    overflow.receiptSurfaces = overflow.surfaces;
    Check(!PlanPathTraceCommittedProposalCapacity(0, overflow, rejected),
        "proposal capacity rejects checked arithmetic overflow");

    const auto runFailure = [](RtPathTraceCommittedProposalBuildTestSeam seam,
        RtPathTraceCommittedProposalBuildReject expected, const char* label,
        std::size_t expectedReserveCalls = std::numeric_limits<std::size_t>::max())
    {
        Fixture fixture;
        const std::uint64_t scratchBefore = ScratchDigest(fixture.scratch);
        const std::uint64_t baselineBefore = BaselineDigest(fixture.baseline);
        RtPathTraceCommittedProposalProduct output = SeedOutput();
        const RtPathTraceCommittedProposalProduct before = output;
        const auto result = BuildPathTraceCommittedProposalProductP2c2(
            fixture.scratch, fixture.baseline, output, &seam);
        Check(!result.accepted && result.rejection == expected &&
            ProductEqual(output, before) &&
            ScratchDigest(fixture.scratch) == scratchBefore &&
            BaselineDigest(fixture.baseline) == baselineBefore &&
            (expectedReserveCalls == std::numeric_limits<std::size_t>::max() ||
                seam.reserveCalls == expectedReserveCalls), label);
    };
    RtPathTraceCommittedProposalBuildTestSeam badAlloc;
    badAlloc.failAtReserve = 0;
    badAlloc.failure = RtPathTraceCommittedProposalAllocationFailure::BadAlloc;
    runFailure(badAlloc, RtPathTraceCommittedProposalBuildReject::BadAlloc,
        "receipt-reserve bad_alloc preserves output, scratch, and baseline");
    RtPathTraceCommittedProposalBuildTestSeam receiptLengthError;
    receiptLengthError.failAtReserve = 0;
    receiptLengthError.failure =
        RtPathTraceCommittedProposalAllocationFailure::LengthError;
    runFailure(receiptLengthError,
        RtPathTraceCommittedProposalBuildReject::LengthError,
        "receipt-reserve length_error preserves output, scratch, and baseline",
        1);
    RtPathTraceCommittedProposalBuildTestSeam lengthError;
    lengthError.failAtReserve = 1;
    lengthError.failure =
        RtPathTraceCommittedProposalAllocationFailure::LengthError;
    runFailure(lengthError, RtPathTraceCommittedProposalBuildReject::LengthError,
        "length_error preserves prior output and scratch");
    RtPathTraceCommittedProposalBuildTestSeam inflated;
    inflated.inflateAtReserve = 0;
    inflated.inflateBy = 1;
    {
        Fixture fixture;
        RtPathTraceCommittedProposalProduct output;
        const auto result = BuildPathTraceCommittedProposalProductP2c2(
            fixture.scratch, fixture.baseline, output, &inflated);
        Check(result.accepted && inflated.reserveCalls == 5 &&
            result.actualOwnedBytes == output.OwnedBytes() &&
            output.receiptSurfaces.capacity() >
                output.capacityCounts.receiptSurfaces,
            "non-breaching reserve inflation is accepted within plan budget");
    }

    const auto runBreachingInflation = [](std::size_t reserveIndex,
        std::size_t inflateBy, const char* label)
    {
        Fixture fixture;
        const std::uint64_t scratchBefore = ScratchDigest(fixture.scratch);
        const std::uint64_t baselineBefore = BaselineDigest(fixture.baseline);
        RtPathTraceCommittedProposalProduct output = SeedOutput();
        const RtPathTraceCommittedProposalProduct before = output;
        RtPathTraceCommittedProposalBuildTestSeam seam;
        seam.inflateAtReserve = reserveIndex;
        seam.inflateBy = inflateBy;
        const auto result = BuildPathTraceCommittedProposalProductP2c2(
            fixture.scratch, fixture.baseline, output, &seam);
        Check(!result.accepted && result.rejection ==
                RtPathTraceCommittedProposalBuildReject::CapacityInvalid &&
            seam.reserveCalls == reserveIndex + 1 &&
            ProductEqual(output, before) &&
            ScratchDigest(fixture.scratch) == scratchBefore &&
            BaselineDigest(fixture.baseline) == baselineBefore, label);
    };
    runBreachingInflation(0,
        RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES /
            sizeof(RtPathTraceCommittedReferencedSurfaceReceiptRow) + 1,
        "receipt reserve breach stops after one reserve and preserves output");
    runBreachingInflation(1,
        RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES /
            sizeof(RtPathTraceMaterialInfoRegistrationIntent) + 1,
        "reserve-1 breach short-circuits before reserve 2");
    runBreachingInflation(2,
        RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES /
            sizeof(RtPathTraceMaterialVariantRegistrationProposal) + 1,
        "reserve-2 breach short-circuits before reserve 3");
    runBreachingInflation(3,
        RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES /
            sizeof(RtPathTraceInstanceObservationProposal) + 1,
        "reserve-3 breach short-circuits before reserve 4");
    runBreachingInflation(4,
        RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES /
            sizeof(RtPathTraceRigidCandidateProposal) + 1,
        "reserve-4 breach is contained atomically");
}

void TestReceiptShapeAndCompatibility()
{
    Fixture fixture;
    RtPathTraceCommittedProposalProduct product;
    const auto built = BuildPathTraceCommittedProposalProductP2c2(
        fixture.scratch, fixture.baseline, product);
    Check(built.accepted && RtPathTraceCommittedProposalProductShapeValid(product),
        "complete PureSemantic receipt product shape accepts");
    const auto tamperRejects = [&product](auto mutate, const char* label)
    {
        RtPathTraceCommittedProposalProduct changed = product;
        mutate(changed);
        Check(!RtPathTraceCommittedProposalProductShapeValid(changed), label);
    };
    tamperRejects([](auto& value) { value.receiptSurfaces.pop_back(); },
        "receipt count tampering rejects product shape");
    tamperRejects([](auto& value) { ++value.receiptSurfaces[0].surfaceOrdinal; },
        "receipt ordinal tampering rejects product shape");
    tamperRejects([](auto& value) { ++value.receiptHeader.surfaceCount; },
        "receipt header tampering rejects product shape");
    tamperRejects([](auto& value) {
        value.provenance = RtPathTraceCommittedProposalProvenance::None; },
        "default provenance rejects product shape");
    tamperRejects([](auto& value) { value.complete = false; },
        "incomplete product rejects product shape");
    tamperRejects([](auto& value) { value.receiptHeader.complete = false; },
        "incomplete receipt rejects product shape");

    RtPathTraceCommittedReferencedSetReceiptHeader current =
        product.receiptHeader;
    ++current.geometryUniverseGeneration;
    RtPathTraceCommittedSemanticFactsProvider provider(fixture.baseline);
    RtPathTraceCommittedReferencedSetValidationStats stats;
    const auto validation = ValidatePathTraceCommittedReferencedSetReceiptP2c3b(
        product.receiptHeader, product.receiptSurfaces.data(),
        product.receiptSurfaces.size(), current, product.receiptSurfaces.data(),
        product.receiptSurfaces.size(), provider, &stats);
    Check(validation.accepted && !validation.geometryPrefilterHit &&
        validation.rowsCompared == product.receiptSurfaces.size() &&
        stats.rowComparisons == product.receiptSurfaces.size(),
        "owned receipt is compatible with full fallback validator replay");
    Check(!product.receiptSurfaces[1].rigidIdentityPresent &&
        provider.RigidReadyQueryCount() == 2 &&
        provider.ResidentReadyQueryCount() == 2,
        "non-rigid owned receipt row makes no readiness query claim");
}

void TestUniverseGenerationIntegrity()
{
    const auto rejectPreservingOutput = [](auto mutate, const char* label)
    {
        Fixture fixture;
        mutate(fixture.baseline);
        RtPathTraceCommittedProposalProduct output = SeedOutput();
        const RtPathTraceCommittedProposalProduct before = output;
        const auto result = BuildPathTraceCommittedProposalProductP2c2(
            fixture.scratch, fixture.baseline, output);
        Check(!result.accepted && result.rejection ==
                RtPathTraceCommittedProposalBuildReject::InputInvalid &&
            ProductEqual(output, before), label);
    };
    rejectPreservingOutput([](auto& baseline)
        { ++baseline.instanceUniverse.ownerGeneration; },
        "instance owner-generation mismatch rejects product atomically");
    rejectPreservingOutput([](auto& baseline)
        { ++baseline.geometryUniverse.ownerGeneration; },
        "geometry owner-generation mismatch rejects product atomically");
    rejectPreservingOutput([](auto& baseline)
        { baseline.instanceUniverse.ownerGeneration = 0; },
        "zero instance owner generation rejects product atomically");
    rejectPreservingOutput([](auto& baseline)
        { baseline.geometryUniverse.ownerGeneration = 0; },
        "zero geometry owner generation rejects product atomically");
    rejectPreservingOutput([](auto& baseline)
        { baseline.epoch.instanceUniverseGeneration = 0; },
        "zero instance generation witness rejects product atomically");
    rejectPreservingOutput([](auto& baseline)
        { baseline.epoch.geometryUniverseGeneration = 0; },
        "zero geometry generation witness rejects product atomically");
}

void TestMalformedAndMissingAuthority()
{
    Fixture malformed;
    malformed.scratch.snapshot.surfaces[0].derivedChosenMaterialId ^= 1u;
    RtPathTraceCommittedProposalProduct output = SeedOutput();
    const RtPathTraceCommittedProposalProduct before = output;
    const std::uint64_t scratchBefore = ScratchDigest(malformed.scratch);
    const auto malformedResult = BuildPathTraceCommittedProposalProductP2c2(
        malformed.scratch, malformed.baseline, output);
    Check(!malformedResult.accepted && malformedResult.rejection ==
            RtPathTraceCommittedProposalBuildReject::PersistedDecisionInvalid &&
        ProductEqual(output, before) &&
        ScratchDigest(malformed.scratch) == scratchBefore,
        "malformed persisted decision rejects without partial publication");

    Fixture missingDecision;
    RtPathTraceCaptureRawSurface& missing =
        missingDecision.scratch.snapshot.surfaces[0];
    missing.derivedRuntimeMaterial = {};
    missing.derivedRuntimeMaterialPresent = false;
    missing.derivedBaseMaterialId = 0;
    missing.derivedChosenMaterialId = 0;
    output = SeedOutput();
    const RtPathTraceCommittedProposalProduct missingBefore = output;
    const auto missingResult = BuildPathTraceCommittedProposalProductP2c2(
        missingDecision.scratch, missingDecision.baseline, output);
    Check(!missingResult.accepted && missingResult.rejection ==
            RtPathTraceCommittedProposalBuildReject::PersistedDecisionInvalid &&
        ProductEqual(output, missingBefore),
        "rigid identity with missing persisted material decision rejects atomically");

    Fixture recordAll;
    recordAll.scratch.snapshot.recordAllInstanceClasses = true;
    recordAll.baseline.semanticConfig.recordAllInstanceClasses = true;
    const std::uint64_t recordScratchBefore = ScratchDigest(recordAll.scratch);
    output = SeedOutput();
    const RtPathTraceCommittedProposalProduct recordBefore = output;
    const auto recordResult = BuildPathTraceCommittedProposalProductP2c2(
        recordAll.scratch, recordAll.baseline, output);
    Check(!recordResult.accepted && recordResult.rejection ==
            RtPathTraceCommittedProposalBuildReject::
                RecordAllIdentityUnavailable &&
        ProductEqual(output, recordBefore) &&
        ScratchDigest(recordAll.scratch) == recordScratchBefore,
        "record-all missing persisted identity fails closed for serial fallback");
}

} // namespace

int main()
{
    TestBuildAndOrder();
    TestCapacityAndAtomicFailure();
    TestReceiptShapeAndCompatibility();
    TestUniverseGenerationIntegrity();
    TestMalformedAndMissingAuthority();
    return failures == 0 ? 0 : 1;
}
