#include "../idlib/precompiled.h"
#include "PathTraceGeometryUniverse.h"
#include "PathTraceInstanceUniverse.h"

#include <cstring>
#include <iostream>
#include <memory>
#include <type_traits>

idCommon* idLib::common = nullptr;
bool AssertFailed(const char*, int, const char*) { return false; }
void* Mem_Alloc16(std::size_t size, memTag_t) { return _aligned_malloc(size, 16); }
void Mem_Free16(void* pointer) { _aligned_free(pointer); }
void* Mem_ClearedAlloc(std::size_t size, memTag_t tag)
{
    void* pointer = Mem_Alloc16(size, tag);
    if (pointer) std::memset(pointer, 0, size);
    return pointer;
}

namespace {
int failures = 0;
void Check(bool value, const char* label)
{
    std::cout << (value ? "[PASS] " : "[FAIL] ") << label << '\n';
    failures += value ? 0 : 1;
}

RtSmokeGeometryUniverse& NewActiveUniverse(uint64 frameIndex = 1)
{
    RtSmokeGeometryUniverse& universe = *new RtSmokeGeometryUniverse();
    universe.RigidMeshCandidateBeginFrameForTest(frameIndex);
    return universe;
}

struct Fixture
{
    idDrawVert vertices[3];
    triIndex_t indexes[3] = { 0, 1, 2 };
    srfTriangles_t tri;
    RtPathTraceRigidMeshCandidateObservation observation;
    explicit Fixture(uint64 meshHash = 0x1234u)
    {
        for (idDrawVert& vertex : vertices) vertex.Clear();
        vertices[0].xyz.Set(0.0f, 0.0f, 0.0f);
        vertices[1].xyz.Set(1.0f, 0.0f, 0.0f);
        vertices[2].xyz.Set(0.0f, 1.0f, 0.0f);
        tri.numVerts = 3; tri.verts = vertices;
        tri.numIndexes = 3; tri.indexes = indexes;
        tri.bounds.Clear();
        for (const idDrawVert& vertex : vertices) tri.bounds.AddPoint(vertex.xyz);
        observation.tri = &tri;
        observation.meshHash = meshHash;
        observation.instanceId = 77;
        observation.vertexBufferIdentity = 11;
        observation.indexBufferIdentity = 12;
        observation.sourceFlags = RT_PT_INSTANCE_SOURCE_RIGID;
        observation.materialId = 5;
        observation.materialClassSignature = 9;
        observation.surfaceClassId = 2;
        observation.vertexFormat = 1;
        observation.numVerts = 3;
        observation.numIndexes = 3;
        observation.localSpaceValid = true;
        observation.materialName = "materials/rigid/prepared";
        observation.modelName = "models/rigid/prepared";
    }
};

constexpr size_t kCap = 8u * 1024u * 1024u;

RtPathTraceRigidPreparedPayload PreparedPayloadFrom(
    const Fixture& fixture, std::uint32_t occurrenceCount = 1)
{
    const RtPathTraceRigidMeshCandidateObservation& observation =
        fixture.observation;
    RtPathTraceRigidPreparedPayload payload;
    payload.occurrenceCount = occurrenceCount;
    payload.meshHash = observation.meshHash;
    payload.instanceId = observation.instanceId;
    payload.vertexBufferIdentity = observation.vertexBufferIdentity;
    payload.indexBufferIdentity = observation.indexBufferIdentity;
    payload.sourceFlags = observation.sourceFlags;
    payload.materialId = observation.materialId;
    payload.materialClassSignature = observation.materialClassSignature;
    payload.surfaceClassId = observation.surfaceClassId;
    payload.triangleClassAndFlags = observation.triangleClassAndFlags;
    payload.vertexFormat = observation.vertexFormat;
    payload.modelEpoch = observation.modelEpoch;
    payload.fullTriangleVertexCount =
        static_cast<std::uint32_t>(observation.numVerts);
    payload.fullTriangleIndexCount =
        static_cast<std::uint32_t>(observation.numIndexes);
    std::memcpy(payload.normalTexMatrix, observation.normalTexMatrix,
        sizeof(payload.normalTexMatrix));
    idStr::Copynz(payload.materialName, observation.materialName.c_str(),
        sizeof(payload.materialName));
    idStr::Copynz(payload.modelName, observation.modelName.c_str(),
        sizeof(payload.modelName));
    RtPathTraceRigidOwnedCpuCache cache;
    if (RtPathTraceBuildRigidOwnedCpuCache(fixture.vertices,
            static_cast<size_t>(observation.numVerts), fixture.indexes,
            static_cast<size_t>(observation.numIndexes),
            observation.normalTexMatrix, cache))
    {
        payload.localVertices = std::move(cache.vertices);
        payload.localIndexes = std::move(cache.indexes);
        std::memcpy(payload.triangleBoundsMin, cache.boundsMin,
            sizeof(payload.triangleBoundsMin));
        std::memcpy(payload.triangleBoundsMax, cache.boundsMax,
            sizeof(payload.triangleBoundsMax));
        payload.contentSignature = cache.contentSignature;
    }
    return payload;
}

void TestFailureAndRetry(
    RtSmokeGeometryUniverse::RigidMeshCandidatePrepareFailurePoint point,
    const char* label)
{
    RtSmokeGeometryUniverse& universe = NewActiveUniverse();
    Fixture fixture;
    const uint64 before = universe.RigidMeshCandidateSemanticFingerprintForTest();
    const size_t storageBefore = universe.RigidMeshCandidateRetainedStorageBytes();
    RtSmokeGeometryUniverse::RigidMeshCandidatePreparedDelta delta;
    RtSmokeGeometryUniverse::RigidMeshCandidatePrepareTestSeam seam;
    if (point == RtSmokeGeometryUniverse::RigidMeshCandidatePrepareFailurePoint::AfterOffsideState ||
        point == RtSmokeGeometryUniverse::RigidMeshCandidatePrepareFailurePoint::AfterAllReserves)
        seam.failAt = point;
    else
        seam.failAfterObservation = 0;
    Check(!universe.PrepareRigidMeshCandidateDelta(
            fixture.observation, 0, kCap, delta, &seam) &&
            !delta.Complete() &&
            universe.RigidMeshCandidateSemanticFingerprintForTest() == before &&
            universe.RigidMeshCandidateRecordCountForTest() == 0 &&
            universe.RigidMeshCandidateLookupCountForTest() == 0 &&
            universe.RigidMeshCandidateFrameHashCountForTest() == 0 &&
            universe.RigidMeshCandidateRetainedStorageBytes() >= storageBefore &&
            seam.retainedGrowthBytes <= kCap,
        label);
    RtSmokeGeometryUniverse::RigidMeshCandidatePrepareTestSeam retry;
    Check(universe.PrepareRigidMeshCandidateDelta(
            fixture.observation, 0, kCap, delta, &retry) &&
            universe.CommitRigidMeshCandidateDelta(delta) &&
            universe.RigidMeshCandidateHasRecordForTest(fixture.observation.meshHash) &&
            universe.RigidMeshCandidateRecordCountForTest() == 1 &&
            universe.RigidMeshCandidateLookupCountForTest() == 1 &&
            universe.RigidMeshCandidateFrameHashCountForTest() == 1,
        "retained topology is reused without stale pointer or iterator authority");
}

void TestExistingAbortCommitRepairAndReject()
{
    RtSmokeGeometryUniverse& universe = NewActiveUniverse();
    Fixture fixture;
    RtSmokeGeometryUniverse::RigidMeshCandidatePreparedDelta delta;
    Check(universe.PrepareRigidMeshCandidateDelta(
            fixture.observation, 0, kCap, delta) &&
            universe.CommitRigidMeshCandidateDelta(delta),
        "new record publishes cache, strings, lookup, hash, stats and generation");
    const uint64 committed = universe.RigidMeshCandidateSemanticFingerprintForTest();
    fixture.observation.materialId = 6;
    fixture.observation.vertexBufferIdentity = 99;
    fixture.observation.normalTexMatrix[0] = 2.0f;
    Check(universe.PrepareRigidMeshCandidateDelta(
            fixture.observation, 0, kCap, delta),
        "existing changed record and CPU-cache rebuild prepare off-side");
    universe.AbortRigidMeshCandidateDelta(delta);
    Check(universe.RigidMeshCandidateSemanticFingerprintForTest() == committed,
        "abort restores exact semantic record/cache/frame state");
    Check(universe.PrepareRigidMeshCandidateDelta(
            fixture.observation, 0, kCap, delta) &&
            universe.CommitRigidMeshCandidateDelta(delta) &&
            universe.RigidMeshCandidateSemanticFingerprintForTest() != committed &&
            universe.RigidMeshCandidateRecordCountForTest() == 1,
        "existing changed record commits without allocation");
    universe.RigidMeshCandidatePoisonLookupForTest(fixture.observation.meshHash, 99);
    Check(!universe.RigidMeshCandidateHasRecordForTest(fixture.observation.meshHash) &&
            universe.PrepareRigidMeshCandidateDelta(
                fixture.observation, 0, kCap, delta) &&
            universe.CommitRigidMeshCandidateDelta(delta) &&
            universe.RigidMeshCandidateHasRecordForTest(fixture.observation.meshHash) &&
            universe.RigidMeshCandidateRecordCountForTest() == 1,
        "lookup repair is staged and swaps atomically without duplicate record");

    RtSmokeGeometryUniverse& rejected = NewActiveUniverse();
    Fixture invalid(0x9999u);
    invalid.observation.localSpaceValid = false;
    const uint64 beforeReject = rejected.RigidMeshCandidateSemanticFingerprintForTest();
    Check(rejected.PrepareRigidMeshCandidateDelta(
            invalid.observation, 0, kCap, delta) &&
            rejected.CommitRigidMeshCandidateDelta(delta) &&
            rejected.RigidMeshCandidateRecordCountForTest() == 0 &&
            rejected.RigidMeshCandidateSemanticFingerprintForTest() != beforeReject,
        "rejected observation publishes only complete frame diagnostics");
}

void TestSerialPreparedParity()
{
    RtSmokeGeometryUniverse& serial = NewActiveUniverse();
    RtSmokeGeometryUniverse& staged = NewActiveUniverse();
    Fixture fixture;
    RtSmokeGeometryUniverse::RigidMeshCandidatePreparedDelta delta;
    serial.RecordRigidMeshCandidate(fixture.observation);
    Check(staged.PrepareRigidMeshCandidateDelta(
            fixture.observation, 0, kCap, delta) &&
            staged.CommitRigidMeshCandidateDelta(delta) &&
            serial.RigidMeshCandidateSemanticFingerprintForTest() ==
                staged.RigidMeshCandidateSemanticFingerprintForTest(),
        "serial RecordObservation and explicit prepared commit share exact new-record semantics");
    fixture.observation.materialId = 12;
    fixture.observation.vertexBufferIdentity = 44;
    fixture.observation.normalTexMatrix[4] = 3.0f;
    serial.RecordRigidMeshCandidate(fixture.observation);
    Check(staged.PrepareRigidMeshCandidateDelta(
            fixture.observation, 0, kCap, delta) &&
            staged.CommitRigidMeshCandidateDelta(delta) &&
            serial.RigidMeshCandidateSemanticFingerprintForTest() ==
                staged.RigidMeshCandidateSemanticFingerprintForTest(),
        "serial and prepared existing-record/cache/stat updates are identical");
    Fixture rejected(0x8888u);
    rejected.observation.localSpaceValid = false;
    serial.RecordRigidMeshCandidate(rejected.observation);
    Check(staged.PrepareRigidMeshCandidateDelta(
            rejected.observation, 0, kCap, delta) &&
            staged.CommitRigidMeshCandidateDelta(delta) &&
            serial.RigidMeshCandidateSemanticFingerprintForTest() ==
                staged.RigidMeshCandidateSemanticFingerprintForTest(),
        "serial and prepared rejection diagnostics are identical");
}

void TestCommitPreconditionAndDoubleCommit()
{
    RtSmokeGeometryUniverse& universe = NewActiveUniverse();
    Fixture fixture;
    RtSmokeGeometryUniverse::RigidMeshCandidatePreparedDelta delta;
    const uint64 beforeIntervening =
        universe.RigidMeshCandidateSemanticFingerprintForTest();
    Check(universe.PrepareRigidMeshCandidateDelta(
            fixture.observation, 0, kCap, delta),
        "commit-precondition fixture prepares");
    universe.RigidMeshCandidateAdvanceGenerationForTest();
    Check(!universe.CommitRigidMeshCandidateDelta(delta) &&
            universe.RigidMeshCandidateSemanticFingerprintForTest() != beforeIntervening &&
            universe.RigidMeshCandidateRecordCountForTest() == 0,
        "generation/lifecycle precondition rejects without publishing delta state");
    Check(universe.PrepareRigidMeshCandidateDelta(
            fixture.observation, 0, kCap, delta) &&
            universe.CommitRigidMeshCandidateDelta(delta) &&
            !universe.CommitRigidMeshCandidateDelta(delta),
        "successful commit invalidates delta and rejects double commit");
}

void TestSingleCapRejectsBeforeLiveReserve()
{
    RtSmokeGeometryUniverse& universe = NewActiveUniverse();
    Fixture fixture;
    RtSmokeGeometryUniverse::RigidMeshCandidatePreparedDelta delta;
    RtSmokeGeometryUniverse::RigidMeshCandidatePrepareTestSeam seam;
    const uint64 before = universe.RigidMeshCandidateSemanticFingerprintForTest();
    const size_t retained = universe.RigidMeshCandidateRetainedStorageBytes();
    Check(!universe.PrepareRigidMeshCandidateDelta(
            fixture.observation, retained, retained, delta, &seam) &&
            seam.liveReserveCalls == 0 && !delta.Complete() &&
            universe.RigidMeshCandidateSemanticFingerprintForTest() == before,
        "single peak cap rejects before the first live reserve and preserves semantics");
}

void TestFrameLifecycleRevisionAndGpuOwnership()
{
    Fixture fixture;
    RtSmokeGeometryUniverse& inactive = *new RtSmokeGeometryUniverse();
    RtSmokeGeometryUniverse::RigidMeshCandidatePreparedDelta delta;
    Check(!inactive.PrepareRigidMeshCandidateDelta(
            fixture.observation, 0, kCap, delta),
        "inactive frame rejects rigid batch preparation");

    RtSmokeGeometryUniverse& ended = NewActiveUniverse(17);
    Check(ended.PrepareRigidMeshCandidateDelta(
            fixture.observation, 0, kCap, delta),
        "active frame prepares before EndFrame invalidation");
    const uint64 endedGeneration = ended.RigidMeshCandidateGenerationForTest();
    ended.RigidMeshCandidateEndFrameForTest();
    Check(!ended.CommitRigidMeshCandidateDelta(delta) &&
            ended.RigidMeshCandidateRecordCountForTest() == 0 &&
            ended.RigidMeshCandidateGenerationForTest() == endedGeneration,
        "EndFrame invalidates a prepared rigid batch");

    RtSmokeGeometryUniverse& repeated = NewActiveUniverse(23);
    Check(repeated.PrepareRigidMeshCandidateDelta(
            fixture.observation, 0, kCap, delta),
        "same-index lifecycle fixture prepares");
    repeated.RigidMeshCandidateBeginFrameForTest(23);
    Check(!repeated.CommitRigidMeshCandidateDelta(delta),
        "same-index BeginFrame serial invalidates an old batch");

    RtSmokeGeometryUniverse& reset = NewActiveUniverse(29);
    Check(reset.PrepareRigidMeshCandidateDelta(
            fixture.observation, 0, kCap, delta),
        "reset lifecycle fixture prepares");
    const uint64 resetGeneration = reset.RigidMeshCandidateGenerationForTest();
    reset.RigidMeshCandidateResetForTest();
    Check(!reset.CommitRigidMeshCandidateDelta(delta) &&
            reset.RigidMeshCandidateRecordCountForTest() == 0 &&
            reset.RigidMeshCandidateGenerationForTest() == resetGeneration,
        "production Clear/world-reset lifecycle helper invalidates without publishing");

    RtSmokeGeometryUniverse& wrapped = NewActiveUniverse(30);
    wrapped.RigidMeshCandidateSetFrameBeginSerialForTest(UINT64_MAX);
    wrapped.RigidMeshCandidateBeginFrameForTest(30);
    Check(wrapped.RigidMeshCandidateFrameBeginSerialForTest() == 1,
        "frame-begin serial wraps by skipping zero");

    RtSmokeGeometryUniverse& revised = NewActiveUniverse(31);
    Check(revised.PrepareRigidMeshCandidateDelta(
            fixture.observation, 0, kCap, delta),
        "rigid semantic revision fixture prepares");
    revised.RigidMeshCandidateAdvanceSemanticRevisionForTest();
    Check(!revised.CommitRigidMeshCandidateDelta(delta),
        "candidate-specific revision rejects an intervening semantic mutation");

    RtSmokeGeometryUniverse& gpuOwned = NewActiveUniverse(37);
    gpuOwned.RecordRigidMeshCandidate(fixture.observation);
    RtPathTraceRigidMeshCandidateObservation changed = fixture.observation;
    changed.materialId = 0x789u;
    Check(gpuOwned.PrepareRigidMeshCandidateDelta(changed, 0, kCap, delta),
        "GPU co-owner preservation fixture prepares");
    gpuOwned.RigidMeshCandidateSetGpuSignatureForTest(
        fixture.observation.meshHash, 0xABCDEFu);
    Check(gpuOwned.CommitRigidMeshCandidateDelta(delta) &&
            gpuOwned.RigidMeshCandidateGpuSignatureForTest(
                fixture.observation.meshHash) == 0xABCDEFu,
        "CPU allowlist commit preserves intervening GPU-owned record fields");
}

void TestFrameBatchParityAndAtomicFailure()
{
    const auto parity = [](auto& fixtures, const char* label)
    {
        RtSmokeGeometryUniverse& serial = NewActiveUniverse();
        RtSmokeGeometryUniverse& batch = NewActiveUniverse();
        std::vector<RtPathTraceRigidMeshCandidateObservation> observations;
        for (Fixture& fixture : fixtures)
        {
            observations.push_back(fixture.observation);
            serial.RecordRigidMeshCandidate(fixture.observation);
        }
        RtSmokeGeometryUniverse::RigidMeshCandidatePreparedDelta delta;
        Check(batch.PrepareRigidMeshCandidateBatch(observations.data(),
                observations.size(), 0, kCap, delta) &&
                batch.CommitRigidMeshCandidateDelta(delta) &&
                batch.RigidMeshCandidateSemanticFingerprintForTest() ==
                    serial.RigidMeshCandidateSemanticFingerprintForTest(),
            label);
    };

    std::array<Fixture, 2> twoNew = { Fixture(0x1001u), Fixture(0x1002u) };
    parity(twoNew, "frame batch publishes two new records with serial parity");

    std::array<Fixture, 2> repeated = { Fixture(0x2001u), Fixture(0x2001u) };
    repeated[1].observation.materialId = 17;
    repeated[1].observation.vertexBufferIdentity = 88;
    parity(repeated,
        "frame batch coalesces repeated mesh hashes in ordinal serial order");

    std::array<Fixture, 2> validRejected = { Fixture(0x3001u), Fixture(0x3002u) };
    validRejected[1].observation.localSpaceValid = false;
    parity(validRejected,
        "frame batch preserves valid plus rejected stats/sample serial semantics");

    RtSmokeGeometryUniverse& existingSerial = NewActiveUniverse();
    RtSmokeGeometryUniverse& existingBatch = NewActiveUniverse();
    Fixture seed(0x4001u);
    existingSerial.RecordRigidMeshCandidate(seed.observation);
    existingBatch.RecordRigidMeshCandidate(seed.observation);
    Fixture changed(0x4001u);
    changed.observation.materialId = 21;
    Fixture added(0x4002u);
    std::vector<RtPathTraceRigidMeshCandidateObservation> existingNew = {
        changed.observation, added.observation
    };
    existingSerial.RecordRigidMeshCandidate(changed.observation);
    existingSerial.RecordRigidMeshCandidate(added.observation);
    RtSmokeGeometryUniverse::RigidMeshCandidatePreparedDelta existingDelta;
    Check(existingBatch.PrepareRigidMeshCandidateBatch(existingNew.data(),
            existingNew.size(), 0, kCap, existingDelta) &&
            existingBatch.CommitRigidMeshCandidateDelta(existingDelta) &&
            existingBatch.RigidMeshCandidateSemanticFingerprintForTest() ==
                existingSerial.RigidMeshCandidateSemanticFingerprintForTest(),
        "frame batch existing-plus-new path is serial-parity exact");

    RtSmokeGeometryUniverse& failed = NewActiveUniverse();
    const uint64 before = failed.RigidMeshCandidateSemanticFingerprintForTest();
    RtSmokeGeometryUniverse::RigidMeshCandidatePreparedDelta failedDelta;
    RtSmokeGeometryUniverse::RigidMeshCandidatePrepareTestSeam seam;
    seam.failAfterObservation = 1;
    Check(!failed.PrepareRigidMeshCandidateBatch(existingNew.data(),
            existingNew.size(), 0, kCap, failedDelta, &seam) &&
            seam.stagedObservationCount == 2 && !failedDelta.Complete() &&
            failed.RigidMeshCandidateSemanticFingerprintForTest() == before,
        "failure after the second observation publishes no semantic prefix");
    Check(failed.PrepareRigidMeshCandidateBatch(existingNew.data(),
            existingNew.size(), 0, kCap, failedDelta) &&
            failed.CommitRigidMeshCandidateDelta(failedDelta),
        "failed frame batch can retry and reuse cleanly");

    RtSmokeGeometryUniverse& capped = NewActiveUniverse();
    std::array<Fixture, 2> capFixtures = { Fixture(0x5001u), Fixture(0x5002u) };
    std::array<RtPathTraceRigidMeshCandidateObservation, 2> capObservations = {
        capFixtures[0].observation, capFixtures[1].observation
    };
    const auto minimumCap = [&](size_t count)
    {
        size_t low = 0;
        size_t high = kCap;
        while (low < high)
        {
            const size_t mid = low + (high - low) / 2u;
            RtSmokeGeometryUniverse::RigidMeshCandidatePreparedDelta probe;
            const bool accepted = capped.PrepareRigidMeshCandidateBatch(
                capObservations.data(), count, 0, mid, probe);
            capped.AbortRigidMeshCandidateDelta(probe);
            if (accepted) high = mid;
            else low = mid + 1u;
        }
        return low;
    };
    const size_t firstCap = minimumCap(1);
    const size_t secondCap = minimumCap(2);
    RtSmokeGeometryUniverse::RigidMeshCandidatePrepareTestSeam capSeam;
    RtSmokeGeometryUniverse::RigidMeshCandidatePreparedDelta capDelta;
    const uint64 capBefore = capped.RigidMeshCandidateSemanticFingerprintForTest();
    Check(firstCap < secondCap &&
            !capped.PrepareRigidMeshCandidateBatch(capObservations.data(), 2,
                0, firstCap, capDelta, &capSeam) &&
            capSeam.stagedObservationCount == 2 &&
            capped.RigidMeshCandidateSemanticFingerprintForTest() == capBefore,
        "second observation cap breach rejects the whole frame batch atomically");
}

void TestDeferredCompatibilityAndExactlyOnceReplay()
{
    Fixture first(0x6101u);
    Fixture second(0x6102u);
    std::vector<RtPathTraceRigidMeshCandidateObservation> partial = {
        first.observation
    };
    RtSmokeGeometryUniverse& partialReplay = NewActiveUniverse();
    size_t replayed = ReplayPathTraceDeferredRigidCandidates(
        partialReplay, true, false, partial);
    const size_t fullCaptureBegin = partial.size();
    partial.push_back(first.observation);
    partial.push_back(second.observation);
    replayed += ReplayPathTraceDeferredRigidCandidates(
        partialReplay, true, false, partial,
        fullCaptureBegin, fullCaptureBegin);
    RtSmokeGeometryUniverse& serial = NewActiveUniverse();
    serial.RecordRigidMeshCandidate(first.observation);
    serial.RecordRigidMeshCandidate(second.observation);
    Check(replayed == 2 &&
            partialReplay.RigidMeshCandidateSemanticFingerprintForTest() ==
                serial.RigidMeshCandidateSemanticFingerprintForTest(),
        "partial harvest replay plus later full capture records each occurrence exactly once");

    std::vector<RtPathTraceRigidPreparedPayload> stablePayloads;
    stablePayloads.push_back(PreparedPayloadFrom(first));
    std::vector<RtPathTraceRigidMeshCandidateObservation> stableCurrent = {
        first.observation
    };
    Check(RtPathTraceRigidPreparedPayloadsCompatible(
            stablePayloads, stableCurrent),
        "stable age-1 prepared payload is compatible with current observations");
    size_t nestedBytes = stablePayloads[0].localVertices.capacity() *
            sizeof(PathTraceSmokeVertex) +
        stablePayloads[0].localIndexes.capacity() * sizeof(std::uint32_t);
    RtSmokeGeometryUniverse& stableApply = NewActiveUniverse();
    RtSmokeGeometryUniverse& stableSerial = NewActiveUniverse();
    stableSerial.RecordRigidMeshCandidate(first.observation);
    RtSmokeGeometryUniverse::RigidPreparedApplyDelta stableDelta;
    const bool stablePrepared = stableApply.PrepareRigidPreparedPayloadApply(
        stablePayloads, stableCurrent, nestedBytes, kCap, stableDelta);
    const bool stableCommitted = stablePrepared &&
        stableApply.CommitRigidPreparedPayloadApply(stableDelta);
    Check(stablePrepared && stableCommitted,
        "stable compact fixture prepares and commits");
    Check(stablePrepared && stableCommitted &&
            stableApply.RigidMeshCandidateSemanticFingerprintForTest() ==
                stableSerial.RigidMeshCandidateSemanticFingerprintForTest(),
        "stable age-1 compact apply is exact serial parity with transferred-byte cap accounting");

    Fixture rebuilt(0x6201u);
    std::vector<RtPathTraceRigidPreparedPayload> stalePayloads;
    stalePayloads.push_back(PreparedPayloadFrom(rebuilt));
    rebuilt.observation.vertexBufferIdentity += 1;
    std::vector<RtPathTraceRigidMeshCandidateObservation> rebuiltCurrent = {
        rebuilt.observation
    };
    RtSmokeGeometryUniverse& mismatchReplay = NewActiveUniverse();
    const bool rebuiltCompatible = RtPathTraceRigidPreparedPayloadsCompatible(
        stalePayloads, rebuiltCurrent);
    const size_t rebuiltReplayCount = ReplayPathTraceDeferredRigidCandidates(
        mismatchReplay, true, rebuiltCompatible, rebuiltCurrent);
    Check(!rebuiltCompatible && rebuiltReplayCount == 1 &&
            mismatchReplay.RigidMeshCandidateRecordCountForTest() == 1,
        "age-1 rebuilt buffer identity mismatch rejects compact apply and replays current-N");

    const std::vector<RtPathTraceRigidPreparedPayload> emptyPayloads;
    RtSmokeGeometryUniverse& emptyReplay = NewActiveUniverse();
    const bool emptyCompatible = RtPathTraceRigidPreparedPayloadsCompatible(
        emptyPayloads, rebuiltCurrent);
    Check(!emptyCompatible && ReplayPathTraceDeferredRigidCandidates(
            emptyReplay, true, emptyCompatible, rebuiltCurrent) == 1,
        "accepted-empty payload with current-N rigid observations replays instead of applying");

    Fixture duplicate(0x6301u);
    std::vector<RtPathTraceRigidMeshCandidateObservation> duplicates = {
        duplicate.observation, duplicate.observation
    };
    std::vector<RtPathTraceRigidPreparedPayload> duplicatePayloads;
    duplicatePayloads.push_back(PreparedPayloadFrom(duplicate, 2));
    Check(RtPathTraceRigidPreparedPayloadsCompatible(
            duplicatePayloads, duplicates),
        "compatibility preserves duplicate occurrence counts without last-write-wins");
    duplicatePayloads[0].occurrenceCount = 1;
    Check(!RtPathTraceRigidPreparedPayloadsCompatible(
            duplicatePayloads, duplicates),
        "compatibility rejects a distinct-set occurrence-count mismatch");

    Fixture basis(0x6401u);
    const idVec3 normal(0.26726124f, 0.53452248f, 0.80178373f);
    const idVec3 tangent(0.89442719f, -0.44721359f, 0.0f);
    for (idDrawVert& vertex : basis.vertices)
    {
        vertex.SetNormal(normal);
        vertex.SetTangent(tangent);
        vertex.SetBiTangent(normal.Cross(tangent));
    }
    basis.tri.bounds.Clear();
    basis.observation.normalTexMatrix[0] = 0.75f;
    basis.observation.normalTexMatrix[1] = 0.25f;
    basis.observation.normalTexMatrix[2] = 0.125f;
    std::vector<RtPathTraceRigidMeshCandidateObservation> basisObservations = {
        basis.observation, basis.observation
    };
    std::vector<RtPathTraceRigidPreparedPayload> basisPayloads;
    basisPayloads.push_back(PreparedPayloadFrom(basis, 2));
    const size_t basisNestedBytes =
        basisPayloads[0].localVertices.capacity() * sizeof(PathTraceSmokeVertex) +
        basisPayloads[0].localIndexes.capacity() * sizeof(std::uint32_t);
    RtSmokeGeometryUniverse& basisSerial = NewActiveUniverse();
    RtSmokeGeometryUniverse& basisCompact = NewActiveUniverse();
    basisSerial.RecordRigidMeshCandidate(basis.observation);
    basisSerial.RecordRigidMeshCandidate(basis.observation);
    RtSmokeGeometryUniverse::RigidPreparedApplyDelta basisDelta;
    Check(basisCompact.PrepareRigidPreparedPayloadApply(basisPayloads,
            basisObservations, basisNestedBytes, kCap, basisDelta) &&
            basisCompact.CommitRigidPreparedPayloadApply(basisDelta) &&
            basisCompact.RigidMeshCandidateSemanticFingerprintForTest() ==
                basisSerial.RigidMeshCandidateSemanticFingerprintForTest(),
        "compact-vs-serial parity covers compressed non-axis basis, cleared tri bounds and duplicates");

    std::vector<RtPathTraceRigidPreparedPayload> tamperedPayloads;
    tamperedPayloads.push_back(PreparedPayloadFrom(basis, 2));
    tamperedPayloads[0].triangleBoundsMax[0] += 1.0f;
    RtSmokeGeometryUniverse& tampered = NewActiveUniverse();
    RtSmokeGeometryUniverse::RigidPreparedApplyDelta tamperedDelta;
    Check(!tampered.PrepareRigidPreparedPayloadApply(tamperedPayloads,
            basisObservations, basisNestedBytes, kCap, tamperedDelta) &&
            tampered.RigidMeshCandidateRecordCountForTest() == 0,
        "compact prepare rejects padded/tampered bounds before moving cache bytes");

    std::vector<RtPathTraceRigidPreparedPayload> omittedPayloads;
    omittedPayloads.push_back(PreparedPayloadFrom(basis, 2));
    std::vector<RtPathTraceRigidMeshCandidateObservation> omittedCurrent =
        basisObservations;
    omittedCurrent[1].materialClassSignature += 1;
    Check(!RtPathTraceRigidPreparedPayloadsCompatible(
            omittedPayloads, omittedCurrent),
        "age-1 omitted-field mismatch rejects the entire compact product");

    const std::vector<RtPathTraceRigidPreparedPayload> noPayloads;
    const std::vector<RtPathTraceRigidMeshCandidateObservation> noObservations;
    Check(RtPathTraceRigidPreparedPayloadsCompatible(
            noPayloads, noObservations),
        "empty payload and empty current observations are compatible");

    Fixture existing(0x6501u);
    RtSmokeGeometryUniverse& existingSerial = NewActiveUniverse();
    RtSmokeGeometryUniverse& existingCompact = NewActiveUniverse();
    existingSerial.RecordRigidMeshCandidate(existing.observation);
    existingCompact.RecordRigidMeshCandidate(existing.observation);
    existingSerial.RigidMeshCandidateSetGpuSignatureForTest(
        existing.observation.meshHash, 0xDEADBEEFu);
    existingCompact.RigidMeshCandidateSetGpuSignatureForTest(
        existing.observation.meshHash, 0xDEADBEEFu);
    existing.observation.materialId += 7;
    existing.observation.instanceId += 3;
    std::vector<RtPathTraceRigidMeshCandidateObservation> existingCurrent = {
        existing.observation, existing.observation
    };
    std::vector<RtPathTraceRigidPreparedPayload> existingPayloads;
    existingPayloads.push_back(PreparedPayloadFrom(existing, 2));
    const size_t existingNestedBytes =
        existingPayloads[0].localVertices.capacity() *
            sizeof(PathTraceSmokeVertex) +
        existingPayloads[0].localIndexes.capacity() * sizeof(std::uint32_t);
    existingSerial.RecordRigidMeshCandidate(existing.observation);
    existingSerial.RecordRigidMeshCandidate(existing.observation);
    RtSmokeGeometryUniverse::RigidPreparedApplyDelta existingPrepared;
    const bool existingDidPrepare =
        existingCompact.PrepareRigidPreparedPayloadApply(existingPayloads,
            existingCurrent, existingNestedBytes, kCap, existingPrepared);
    const bool existingDidCommit = existingDidPrepare &&
        existingCompact.CommitRigidPreparedPayloadApply(existingPrepared);
    const bool existingDoubleRejected = existingDidCommit &&
        !existingCompact.CommitRigidPreparedPayloadApply(existingPrepared);
    Check(existingDidPrepare && existingDidCommit && existingDoubleRejected,
        "existing compact fixture prepares, commits and rejects double commit");
    Check(existingDidPrepare && existingDidCommit && existingDoubleRejected &&
            existingCompact.RigidMeshCandidateGpuSignatureForTest(
                existing.observation.meshHash) == 0xDEADBEEFu &&
            existingCompact.RigidMeshCandidateSemanticFingerprintForTest() ==
                existingSerial.RigidMeshCandidateSemanticFingerprintForTest(),
        "existing duplicate compact apply preserves GPU ownership and exact serial counters/samples/generation; double commit rejects");

    Fixture invalidated(0x6601u);
    std::vector<RtPathTraceRigidMeshCandidateObservation> invalidatedCurrent = {
        invalidated.observation
    };
    std::vector<RtPathTraceRigidPreparedPayload> invalidatedPayloads;
    invalidatedPayloads.push_back(PreparedPayloadFrom(invalidated));
    const size_t invalidatedNestedBytes =
        invalidatedPayloads[0].localVertices.capacity() *
            sizeof(PathTraceSmokeVertex) +
        invalidatedPayloads[0].localIndexes.capacity() * sizeof(std::uint32_t);
    RtSmokeGeometryUniverse& invalidatedUniverse = NewActiveUniverse();
    RtSmokeGeometryUniverse::RigidPreparedApplyDelta invalidatedDelta;
    Check(invalidatedUniverse.PrepareRigidPreparedPayloadApply(
            invalidatedPayloads, invalidatedCurrent, invalidatedNestedBytes,
            kCap, invalidatedDelta),
        "compact lifecycle fixture prepares");
    invalidatedUniverse.RigidMeshCandidateAdvanceSemanticRevisionForTest();
    Check(!invalidatedUniverse.CommitRigidPreparedPayloadApply(invalidatedDelta) &&
            invalidatedUniverse.RigidMeshCandidateRecordCountForTest() == 0,
        "compact commit rejects intervening lifecycle/semantic revision");

    const std::array<RtSmokeGeometryUniverse::RigidMeshCandidatePrepareFailurePoint, 3>
        reservePoints = {
            RtSmokeGeometryUniverse::RigidMeshCandidatePrepareFailurePoint::AfterRecordReserve,
            RtSmokeGeometryUniverse::RigidMeshCandidatePrepareFailurePoint::AfterLookupReserve,
            RtSmokeGeometryUniverse::RigidMeshCandidatePrepareFailurePoint::AfterFrameHashReserve
        };
    bool reserveSeamsExact = true;
    for (size_t pointIndex = 0; pointIndex < reservePoints.size(); ++pointIndex)
    {
        Fixture reserveFixture(0x6701u + pointIndex);
        std::vector<RtPathTraceRigidMeshCandidateObservation> reserveCurrent = {
            reserveFixture.observation
        };
        std::vector<RtPathTraceRigidPreparedPayload> reservePayloads;
        reservePayloads.push_back(PreparedPayloadFrom(reserveFixture));
        const size_t reserveNestedBytes =
            reservePayloads[0].localVertices.capacity() *
                sizeof(PathTraceSmokeVertex) +
            reservePayloads[0].localIndexes.capacity() * sizeof(std::uint32_t);
        RtSmokeGeometryUniverse& reserveUniverse = NewActiveUniverse();
        RtSmokeGeometryUniverse::RigidPreparedApplyDelta reserveDelta;
        RtSmokeGeometryUniverse::RigidMeshCandidatePrepareTestSeam reserveSeam;
        reserveSeam.failAt = reservePoints[pointIndex];
        reserveSeamsExact = reserveSeamsExact &&
            !reserveUniverse.PrepareRigidPreparedPayloadApply(reservePayloads,
                reserveCurrent, reserveNestedBytes, kCap, reserveDelta,
                &reserveSeam) && reserveSeam.liveReserveCalls >= 1 &&
            reserveUniverse.RigidMeshCandidateRecordCountForTest() == 0;
    }
    Check(reserveSeamsExact,
        "compact record/map/frame-hash reserve seams retain capacity but publish no semantic prefix");

    std::array<Fixture, 8> untouchedFixtures = {
        Fixture(0x6801u), Fixture(0x6802u), Fixture(0x6803u), Fixture(0x6804u),
        Fixture(0x6805u), Fixture(0x6806u), Fixture(0x6807u), Fixture(0x6808u)
    };
    RtSmokeGeometryUniverse& untouchedUniverse = NewActiveUniverse();
    for (Fixture& fixture : untouchedFixtures)
        untouchedUniverse.RecordRigidMeshCandidate(fixture.observation);
    Fixture capTarget(0x6810u);
    std::vector<RtPathTraceRigidMeshCandidateObservation> capCurrent = {
        capTarget.observation
    };
    std::vector<RtPathTraceRigidPreparedPayload> capPayloads;
    capPayloads.push_back(PreparedPayloadFrom(capTarget));
    const size_t capNestedBytes = capPayloads[0].localVertices.capacity() *
            sizeof(PathTraceSmokeVertex) +
        capPayloads[0].localIndexes.capacity() * sizeof(std::uint32_t);
    const size_t untouchedRetained =
        untouchedUniverse.RigidMeshCandidateRetainedStorageBytes();
    RtSmokeGeometryUniverse::RigidPreparedApplyDelta capPrepared;
    RtSmokeGeometryUniverse::RigidMeshCandidatePrepareTestSeam capPrepareSeam;
    Check(!untouchedUniverse.PrepareRigidPreparedPayloadApply(capPayloads,
            capCurrent, capNestedBytes, untouchedRetained + capNestedBytes,
            capPrepared, &capPrepareSeam) &&
            capPrepareSeam.liveReserveCalls == 0 &&
            untouchedUniverse.RigidMeshCandidateRecordCountForTest() ==
                untouchedFixtures.size(),
        "compact peak cap includes large untouched live nested caches before any live reserve");

    const auto compactLifecycleRejects = [](uint64 meshHash, int mutation)
    {
        Fixture fixture(meshHash);
        std::vector<RtPathTraceRigidMeshCandidateObservation> current = {
            fixture.observation
        };
        std::vector<RtPathTraceRigidPreparedPayload> payloads;
        payloads.push_back(PreparedPayloadFrom(fixture));
        const size_t nestedBytes = payloads[0].localVertices.capacity() *
                sizeof(PathTraceSmokeVertex) +
            payloads[0].localIndexes.capacity() * sizeof(std::uint32_t);
        RtSmokeGeometryUniverse& universe = NewActiveUniverse(91);
        RtSmokeGeometryUniverse::RigidPreparedApplyDelta prepared;
        if (!universe.PrepareRigidPreparedPayloadApply(payloads, current,
                nestedBytes, kCap, prepared))
            return false;
        switch (mutation)
        {
        case 0: universe.RigidMeshCandidateEndFrameForTest(); break;
        case 1: universe.RigidMeshCandidateBeginFrameForTest(91); break;
        case 2: universe.RigidMeshCandidateResetForTest(); break;
        default: universe.RigidMeshCandidateAdvanceGenerationForTest(); break;
        }
        return !universe.CommitRigidPreparedPayloadApply(prepared) &&
            universe.RigidMeshCandidateRecordCountForTest() == 0;
    };
    Check(compactLifecycleRejects(0x6821u, 0) &&
            compactLifecycleRejects(0x6822u, 1) &&
            compactLifecycleRejects(0x6823u, 2) &&
            compactLifecycleRejects(0x6824u, 3),
        "compact commit rejects EndFrame, same-index BeginFrame, Clear/reset and shared-generation invalidation");

    std::vector<std::unique_ptr<Fixture>> peakFixtures;
    peakFixtures.reserve(72);
    for (size_t index = 0; index < 72; ++index)
        peakFixtures.emplace_back(new Fixture(0x6900u + index));
    bool recordPeakProved = false;
    bool bucketPeakProved = false;
    for (size_t seedCount = 1;
         seedCount + 1u < peakFixtures.size() && !recordPeakProved;
         ++seedCount)
    {
        RtSmokeGeometryUniverse& probeUniverse = NewActiveUniverse(92);
        for (size_t seed = 0; seed < seedCount; ++seed)
            probeUniverse.RecordRigidMeshCandidate(
                peakFixtures[seed]->observation);
        Fixture& target = *peakFixtures[seedCount];
        std::vector<RtPathTraceRigidMeshCandidateObservation> current = {
            target.observation
        };
        std::vector<RtPathTraceRigidPreparedPayload> payloads;
        payloads.push_back(PreparedPayloadFrom(target));
        const size_t nestedBytes = payloads[0].localVertices.capacity() *
                sizeof(PathTraceSmokeVertex) +
            payloads[0].localIndexes.capacity() * sizeof(std::uint32_t);
        RtSmokeGeometryUniverse::RigidPreparedApplyDelta probeDelta;
        RtSmokeGeometryUniverse::RigidMeshCandidatePrepareTestSeam probeSeam;
        if (!probeUniverse.PrepareRigidPreparedPayloadApply(payloads, current,
                nestedBytes, kCap, probeDelta, &probeSeam))
            continue;
        probeUniverse.AbortRigidPreparedPayloadApply(probeDelta);

        const size_t otherThanRecord = std::max(
            probeSeam.predictedFinalOwnedBytes,
            std::max(probeSeam.predictedLookupPeakBytes,
                probeSeam.predictedFrameHashPeakBytes));
        const size_t bucketPeak = std::max(
            probeSeam.predictedLookupPeakBytes,
            probeSeam.predictedFrameHashPeakBytes);
        const bool tryRecord = !recordPeakProved &&
            probeSeam.predictedRecordPeakBytes > otherThanRecord;
        const bool tryBucket = !bucketPeakProved &&
            bucketPeak > std::max(probeSeam.predictedFinalOwnedBytes,
                probeSeam.predictedRecordPeakBytes);
        if (!tryRecord && !tryBucket) continue;

        RtSmokeGeometryUniverse& cappedUniverse = NewActiveUniverse(92);
        for (size_t seed = 0; seed < seedCount; ++seed)
            cappedUniverse.RecordRigidMeshCandidate(
                peakFixtures[seed]->observation);
        std::vector<RtPathTraceRigidPreparedPayload> cappedPayloads;
        cappedPayloads.push_back(PreparedPayloadFrom(target));
        const size_t selectedPeak = tryRecord
            ? probeSeam.predictedRecordPeakBytes : bucketPeak;
        const size_t threshold = selectedPeak - 1u;
        RtSmokeGeometryUniverse::RigidPreparedApplyDelta cappedDelta;
        RtSmokeGeometryUniverse::RigidMeshCandidatePrepareTestSeam cappedSeam;
        const uint64 before =
            cappedUniverse.RigidMeshCandidateSemanticFingerprintForTest();
        const bool rejected =
            !cappedUniverse.PrepareRigidPreparedPayloadApply(cappedPayloads,
                current, nestedBytes, threshold, cappedDelta, &cappedSeam) &&
            cappedSeam.liveReserveCalls == 0 &&
            cappedUniverse.RigidMeshCandidateSemanticFingerprintForTest() == before;
        if (tryRecord) recordPeakProved = rejected;
        if (tryBucket) bucketPeakProved = rejected;
    }

    const auto primeRecordCapacity = [&](RtSmokeGeometryUniverse& universe)
    {
        std::vector<RtPathTraceRigidMeshCandidateObservation> current;
        std::vector<RtPathTraceRigidPreparedPayload> payloads;
        size_t nestedBytes = 0;
        for (size_t index = 0; index < 64; ++index)
        {
            current.push_back(peakFixtures[index]->observation);
            payloads.push_back(PreparedPayloadFrom(*peakFixtures[index]));
            nestedBytes += payloads.back().localVertices.capacity() *
                    sizeof(PathTraceSmokeVertex) +
                payloads.back().localIndexes.capacity() * sizeof(std::uint32_t);
        }
        RtSmokeGeometryUniverse::RigidPreparedApplyDelta prepared;
        RtSmokeGeometryUniverse::RigidMeshCandidatePrepareTestSeam seam;
        seam.failAt = RtSmokeGeometryUniverse::
            RigidMeshCandidatePrepareFailurePoint::AfterRecordReserve;
        return !universe.PrepareRigidPreparedPayloadApply(payloads, current,
                nestedBytes, kCap, prepared, &seam) &&
            seam.liveReserveCalls == 1 &&
            universe.RigidMeshCandidateRecordCountForTest() == 0;
    };
    const auto makeBucketBatch = [&]()
    {
        std::pair<std::vector<RtPathTraceRigidMeshCandidateObservation>,
            std::vector<RtPathTraceRigidPreparedPayload>> batch;
        for (size_t index = 0; index < 16; ++index)
        {
            batch.first.push_back(peakFixtures[index]->observation);
            batch.second.push_back(PreparedPayloadFrom(*peakFixtures[index]));
        }
        return batch;
    };
    RtSmokeGeometryUniverse& bucketProbeUniverse = NewActiveUniverse(93);
    if (primeRecordCapacity(bucketProbeUniverse))
    {
        auto bucketBatch = makeBucketBatch();
        size_t nestedBytes = 0;
        for (const auto& payload : bucketBatch.second)
            nestedBytes += payload.localVertices.capacity() *
                    sizeof(PathTraceSmokeVertex) +
                payload.localIndexes.capacity() * sizeof(std::uint32_t);
        RtSmokeGeometryUniverse::RigidPreparedApplyDelta prepared;
        RtSmokeGeometryUniverse::RigidMeshCandidatePrepareTestSeam seam;
        if (bucketProbeUniverse.PrepareRigidPreparedPayloadApply(
                bucketBatch.second, bucketBatch.first, nestedBytes, kCap,
                prepared, &seam))
        {
            bucketProbeUniverse.AbortRigidPreparedPayloadApply(prepared);
            const size_t bucketPeak = std::max(
                seam.predictedLookupPeakBytes,
                seam.predictedFrameHashPeakBytes);
            const size_t lowerBound = std::max(
                seam.predictedFinalOwnedBytes,
                seam.predictedRecordPeakBytes);
            const auto twoSlotModelExact = [](size_t oldBuckets,
                size_t replacementBuckets, size_t predictedTransient)
            {
                if (replacementBuckets <= oldBuckets) return true;
                const size_t bucketSum = oldBuckets + replacementBuckets;
                const size_t oldOnePointerModel = bucketSum * sizeof(void*);
                const size_t msvcTwoSlotModel = oldOnePointerModel * 2u;
                return msvcTwoSlotModel > oldOnePointerModel &&
                    predictedTransient == msvcTwoSlotModel;
            };
            const bool bucketModelExact =
                twoSlotModelExact(seam.oldLookupBucketCount,
                    seam.replacementLookupBucketCount,
                    seam.predictedLookupBucketTransientBytes) &&
                twoSlotModelExact(seam.oldFrameHashBucketCount,
                    seam.replacementFrameHashBucketCount,
                    seam.predictedFrameHashBucketTransientBytes);
            if (bucketModelExact && bucketPeak > lowerBound)
            {
                RtSmokeGeometryUniverse& capped = NewActiveUniverse(93);
                if (primeRecordCapacity(capped))
                {
                    auto cappedBatch = makeBucketBatch();
                    RtSmokeGeometryUniverse::RigidPreparedApplyDelta rejectedDelta;
                    RtSmokeGeometryUniverse::RigidMeshCandidatePrepareTestSeam rejectedSeam;
                    const uint64 before =
                        capped.RigidMeshCandidateSemanticFingerprintForTest();
                    bucketPeakProved =
                        !capped.PrepareRigidPreparedPayloadApply(
                            cappedBatch.second, cappedBatch.first, nestedBytes,
                            bucketPeak - 1u, rejectedDelta, &rejectedSeam) &&
                        rejectedSeam.liveReserveCalls == 0 &&
                        capped.RigidMeshCandidateSemanticFingerprintForTest() ==
                            before;
                }
            }
        }
    }
    Check(recordPeakProved,
        "compact cap rejects old-plus-full-new record-array peak before the first live reserve");
    Check(bucketPeakProved,
        "compact cap rejects old-plus-replacement map/set bucket peak before the first live reserve");
}
}

int main()
{
    static_assert(noexcept(std::declval<RtSmokeGeometryUniverse&>()
        .CommitRigidMeshCandidateDelta(std::declval<
            RtSmokeGeometryUniverse::RigidMeshCandidatePreparedDelta&>())),
        "rigid commit must remain noexcept");
    TestFailureAndRetry(RtSmokeGeometryUniverse::RigidMeshCandidatePrepareFailurePoint::AfterOffsideState,
        "off-side construction failure preserves semantic state");
    TestFailureAndRetry(RtSmokeGeometryUniverse::RigidMeshCandidatePrepareFailurePoint::AfterRecordReserve,
        "record reserve failure permits only bounded retained capacity");
    TestFailureAndRetry(RtSmokeGeometryUniverse::RigidMeshCandidatePrepareFailurePoint::AfterLookupReserve,
        "lookup reserve failure permits only bounded retained topology");
    TestFailureAndRetry(RtSmokeGeometryUniverse::RigidMeshCandidatePrepareFailurePoint::AfterFrameHashReserve,
        "frame-hash reserve failure permits only bounded retained topology");
    TestFailureAndRetry(RtSmokeGeometryUniverse::RigidMeshCandidatePrepareFailurePoint::AfterAllReserves,
        "late prepare failure retains no semantic mutation");
    TestExistingAbortCommitRepairAndReject();
    TestSerialPreparedParity();
    TestCommitPreconditionAndDoubleCommit();
    TestSingleCapRejectsBeforeLiveReserve();
    TestFrameLifecycleRevisionAndGpuOwnership();
    TestFrameBatchParityAndAtomicFailure();
    TestDeferredCompatibilityAndExactlyOnceReplay();
    std::cout << (failures == 0
        ? "PathTrace rigid prepared-delta harness passed\n"
        : "PathTrace rigid prepared-delta harness FAILED\n");
    return failures == 0 ? 0 : 1;
}
