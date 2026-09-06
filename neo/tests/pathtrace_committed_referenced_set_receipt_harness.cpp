#include "PathTraceCommittedReferencedSetReceipt.h"

#include <cstdio>
#include <cstring>
#include <functional>

namespace
{
int failures = 0;

void Check(bool condition, const char* label)
{
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", label);
    failures += condition ? 0 : 1;
}

struct RecordingFacts final : RtPathTraceCommittedReferencedSetFactsProvider
{
    mutable std::size_t meshCalls = 0;
    mutable std::size_t residentCalls = 0;
    bool complete = true;
    bool meshAnswer = true;
    bool residentAnswer = true;

    bool Complete() const noexcept override { return complete; }
    bool IsRigidRouteReady(std::uint64_t) const override
    {
        ++meshCalls;
        return meshAnswer;
    }
    bool IsRigidRouteResidentReadyForEntityMaterial(
        std::int32_t, std::int32_t, std::uint32_t) const override
    {
        ++residentCalls;
        return residentAnswer;
    }
};

struct Fixture
{
    RtPathTraceCommittedReferencedSetReceiptHeader receipt;
    RtPathTraceCommittedReferencedSetReceiptHeader current;
    RtPathTraceCommittedReferencedSurfaceReceiptRow receiptRows[2];
    RtPathTraceCommittedReferencedSurfaceReceiptRow currentRows[2];
    RecordingFacts facts;

    Fixture()
    {
        receipt.sealedPrimaryViewToken = 70;
        receipt.ownerUniverseFrameIndex = 900;
        receipt.baselineFrameIndex = 700;
        receipt.worldLifecycleGeneration = 3;
        receipt.mapLoadSerial = 4;
        receipt.mapTimeStamp = 5;
        std::strcpy(receipt.mapName, "maps/p2c3b.proc");
        receipt.barrierGeneration = 6;
        receipt.materialRegistryGeneration = 7;
        receipt.residentMaterialFactsGeneration = 8;
        receipt.configFingerprint = 9;
        receipt.instanceUniverseGeneration = 10;
        receipt.geometryUniverseGeneration = 11;
        receipt.staticMaterialGeneration = 12;
        receipt.canonicalSourceIndexPoolGeneration = 13;
        receipt.staticResidentPayloadGeneration = 14;
        receipt.surfaceCount = 2;
        receipt.recordAllInstanceClasses = false;
        receipt.complete = true;
        current = receipt;

        receiptRows[0].persistedMeshHash = 100;
        receiptRows[0].persistedChosenMaterialId = 17;
        receiptRows[0].surfaceOrdinal = 0;
        receiptRows[0].entityIndex = 2;
        receiptRows[0].renderEntityNum = 3;
        receiptRows[0].expectedReadyByMesh = true;
        receiptRows[0].expectedReadyByResident = true;
        receiptRows[0].rigidIdentityPresent = true;
        receiptRows[0].derivedRuntimeMaterialPresent = true;
        receiptRows[1].surfaceOrdinal = 1;
        receiptRows[1].expectedReadyByMesh = true;
        receiptRows[1].expectedReadyByResident = true;
        receiptRows[1].rigidIdentityPresent = false;
        receiptRows[1].derivedRuntimeMaterialPresent = true;
        currentRows[0] = receiptRows[0];
        currentRows[1] = receiptRows[1];
    }

    RtPathTraceCommittedReferencedSetValidationResult Validate(
        RtPathTraceCommittedReferencedSetValidationStats* stats = nullptr)
    {
        return ValidatePathTraceCommittedReferencedSetReceiptP2c3b(
            receipt, receiptRows, 2, current, currentRows, 2, facts, stats);
    }

    void ForceFallback() { ++current.geometryUniverseGeneration; }
};

std::uint64_t FixtureCarrierDigest(const Fixture& fixture)
{
    std::uint64_t value = 1469598103934665603ull;
    const auto append = [&value](const void* bytes, std::size_t count)
    {
        const auto* data = static_cast<const unsigned char*>(bytes);
        for (std::size_t index = 0; index < count; ++index)
        {
            value ^= data[index];
            value *= 1099511628211ull;
        }
    };
    append(&fixture.receipt, sizeof(fixture.receipt));
    append(&fixture.current, sizeof(fixture.current));
    append(fixture.receiptRows, sizeof(fixture.receiptRows));
    append(fixture.currentRows, sizeof(fixture.currentRows));
    return value;
}

void TestHeaderAuthorityAndPrefilter()
{
    Fixture hit;
    ++hit.receiptRows[0].persistedMeshHash;
    RtPathTraceCommittedReferencedSetValidationStats hitStats;
    const auto hitResult = hit.Validate(&hitStats);
    Check(hitResult.accepted && hitResult.geometryPrefilterHit &&
        hitResult.rowsCompared == 0 && hitStats.geometryPrefilterHits == 1 &&
        hitStats.rowComparisons == 0 && hit.facts.meshCalls == 0 &&
        hit.facts.residentCalls == 0,
        "four-generation prefilter hit skips every row/provider comparison");

    const auto expectReject = [](const std::function<void(Fixture&)>& mutate,
        RtPathTraceCommittedReferencedSetReject expected, const char* label)
    {
        Fixture fixture;
        mutate(fixture);
        const std::uint64_t before = FixtureCarrierDigest(fixture);
        const auto result = fixture.Validate();
        Check(!result.accepted && result.rejection == expected &&
            fixture.facts.meshCalls == 0 && fixture.facts.residentCalls == 0 &&
            FixtureCarrierDigest(fixture) == before,
            label);
    };
    expectReject([](auto& f) { f.receipt.complete = false; },
        RtPathTraceCommittedReferencedSetReject::ReceiptIncomplete,
        "receipt completeness is unconditional");
    expectReject([](auto& f) { f.current.complete = false; },
        RtPathTraceCommittedReferencedSetReject::AuthorityIncomplete,
        "current authority completeness is unconditional");
    expectReject([](auto& f) { ++f.receipt.sealedPrimaryViewToken; },
        RtPathTraceCommittedReferencedSetReject::SealedTokenMismatch, "token mismatch");
    expectReject([](auto& f) { ++f.receipt.ownerUniverseFrameIndex; },
        RtPathTraceCommittedReferencedSetReject::OwnerFrameMismatch, "owner frame mismatch");
    expectReject([](auto& f) { ++f.receipt.baselineFrameIndex; },
        RtPathTraceCommittedReferencedSetReject::BaselineFrameMismatch, "baseline frame mismatch");
    expectReject([](auto& f) { ++f.receipt.worldLifecycleGeneration; },
        RtPathTraceCommittedReferencedSetReject::WorldLifecycleMismatch, "lifecycle mismatch");
    expectReject([](auto& f) { f.receipt.mapName[0] ^= 1; },
        RtPathTraceCommittedReferencedSetReject::MapNameMismatch, "map name mismatch");
    expectReject([](auto& f) { ++f.receipt.mapTimeStamp; },
        RtPathTraceCommittedReferencedSetReject::MapTimeMismatch, "map timestamp mismatch");
    expectReject([](auto& f) { ++f.receipt.mapLoadSerial; },
        RtPathTraceCommittedReferencedSetReject::MapLoadSerialMismatch, "map load mismatch");
    expectReject([](auto& f) { ++f.receipt.barrierGeneration; },
        RtPathTraceCommittedReferencedSetReject::BarrierMismatch, "barrier mismatch");
    expectReject([](auto& f) { ++f.receipt.materialRegistryGeneration; },
        RtPathTraceCommittedReferencedSetReject::MaterialRegistryGenerationMismatch,
        "material registry mismatch");
    expectReject([](auto& f) { ++f.receipt.residentMaterialFactsGeneration; },
        RtPathTraceCommittedReferencedSetReject::ResidentFactsGenerationMismatch,
        "resident facts mismatch");
    expectReject([](auto& f) { ++f.receipt.configFingerprint; },
        RtPathTraceCommittedReferencedSetReject::ConfigFingerprintMismatch,
        "config fingerprint mismatch");
    expectReject([](auto& f) { ++f.receipt.instanceUniverseGeneration; },
        RtPathTraceCommittedReferencedSetReject::InstanceOwnerGenerationMismatch,
        "instance owner generation mismatch");
    expectReject([](auto& f) { f.receipt.recordAllInstanceClasses = true; },
        RtPathTraceCommittedReferencedSetReject::RecordAllTrueToFalse,
        "recordAll true-to-false mismatch is distinct");
    expectReject([](auto& f) { f.current.recordAllInstanceClasses = true; },
        RtPathTraceCommittedReferencedSetReject::RecordAllFalseToTrue,
        "recordAll false-to-true mismatch is distinct");
    expectReject([](auto& f) { ++f.receipt.surfaceCount; },
        RtPathTraceCommittedReferencedSetReject::SurfaceCountMismatch,
        "surface count mismatch");
    {
        Fixture fixture;
        const auto result = ValidatePathTraceCommittedReferencedSetReceiptP2c3b(
            fixture.receipt, nullptr, 2, fixture.current,
            fixture.currentRows, 2, fixture.facts);
        Check(!result.accepted && result.rejection ==
                RtPathTraceCommittedReferencedSetReject::MissingRows,
            "missing receipt rows reject even when prefilter witnesses match");
    }

    const auto geometryFallback = [](auto mutate, const char* label)
    {
        Fixture fixture;
        mutate(fixture.current);
        RtPathTraceCommittedReferencedSetValidationStats stats;
        const auto result = fixture.Validate(&stats);
        Check(result.accepted && !result.geometryPrefilterHit &&
            result.rowsCompared == 2 && stats.rowComparisons == 2 &&
            fixture.facts.meshCalls == 1 && fixture.facts.residentCalls == 1,
            label);
    };
    geometryFallback([](auto& h) { ++h.geometryUniverseGeneration; },
        "geometry universe mismatch performs full referenced-set comparison");
    geometryFallback([](auto& h) { ++h.staticMaterialGeneration; },
        "static material mismatch performs full referenced-set comparison");
    geometryFallback([](auto& h) { ++h.canonicalSourceIndexPoolGeneration; },
        "canonical pool mismatch performs full referenced-set comparison");
    geometryFallback([](auto& h) { ++h.staticResidentPayloadGeneration; },
        "static resident mismatch performs full referenced-set comparison");

    const auto equalZeroReject = [](auto zero, const char* label)
    {
        Fixture fixture;
        zero(fixture.receipt);
        zero(fixture.current);
        const auto result = fixture.Validate();
        Check(!result.accepted && result.rejection ==
                RtPathTraceCommittedReferencedSetReject::GeometryWitnessInvalid &&
            fixture.facts.meshCalls == 0 && fixture.facts.residentCalls == 0,
            label);
    };
    equalZeroReject([](auto& h) { h.geometryUniverseGeneration = 0; },
        "equal-zero geometry universe witnesses fail closed");
    equalZeroReject([](auto& h) { h.staticMaterialGeneration = 0; },
        "equal-zero static material witnesses fail closed");
    equalZeroReject([](auto& h) { h.canonicalSourceIndexPoolGeneration = 0; },
        "equal-zero canonical pool witnesses fail closed");
    equalZeroReject([](auto& h) { h.staticResidentPayloadGeneration = 0; },
        "equal-zero static resident witnesses fail closed");

    Fixture incompleteFacts;
    incompleteFacts.ForceFallback();
    incompleteFacts.facts.complete = false;
    const auto incompleteResult = incompleteFacts.Validate();
    Check(!incompleteResult.accepted && incompleteResult.rejection ==
            RtPathTraceCommittedReferencedSetReject::FactsProviderIncomplete &&
        incompleteFacts.facts.meshCalls == 0 &&
        incompleteFacts.facts.residentCalls == 0,
        "incomplete facts provider rejects before fallback row queries");
}

void TestRows()
{
    const auto expectRowReject = [](const std::function<void(Fixture&)>& mutate,
        RtPathTraceCommittedReferencedSetReject expected, const char* label)
    {
        Fixture fixture;
        fixture.ForceFallback();
        mutate(fixture);
        const std::uint64_t before = FixtureCarrierDigest(fixture);
        const auto result = fixture.Validate();
        Check(!result.accepted && result.rejection == expected &&
            FixtureCarrierDigest(fixture) == before, label);
    };
    expectRowReject([](auto& f) { ++f.receiptRows[0].surfaceOrdinal; },
        RtPathTraceCommittedReferencedSetReject::RowOrdinalMismatch, "row ordinal mismatch");
    expectRowReject([](auto& f) { ++f.receiptRows[0].persistedMeshHash; },
        RtPathTraceCommittedReferencedSetReject::RowMeshHashMismatch, "row mesh mismatch");
    expectRowReject([](auto& f) { ++f.receiptRows[0].persistedChosenMaterialId; },
        RtPathTraceCommittedReferencedSetReject::RowChosenMaterialMismatch,
        "row chosen material mismatch");
    expectRowReject([](auto& f) { ++f.receiptRows[0].entityIndex; },
        RtPathTraceCommittedReferencedSetReject::RowEntityIndexMismatch, "row entity mismatch");
    expectRowReject([](auto& f) { ++f.receiptRows[0].renderEntityNum; },
        RtPathTraceCommittedReferencedSetReject::RowRenderEntityNumMismatch,
        "row render entity mismatch");
    expectRowReject([](auto& f) { f.receiptRows[0].rigidIdentityPresent = false; },
        RtPathTraceCommittedReferencedSetReject::RowRigidIdentityMismatch,
        "row rigid presence mismatch");
    expectRowReject([](auto& f) { f.receiptRows[0].derivedRuntimeMaterialPresent = false; },
        RtPathTraceCommittedReferencedSetReject::RowRuntimeMaterialPresenceMismatch,
        "row runtime material presence mismatch");
    expectRowReject([](auto& f) { f.receiptRows[0].expectedReadyByMesh = false; },
        RtPathTraceCommittedReferencedSetReject::RowReadyByMeshMismatch,
        "row mesh readiness mismatch");
    expectRowReject([](auto& f) { f.receiptRows[0].expectedReadyByResident = false; },
        RtPathTraceCommittedReferencedSetReject::RowReadyByResidentMismatch,
        "row resident readiness mismatch");

    Fixture noClaim;
    noClaim.ForceFallback();
    noClaim.receiptRows[1].expectedReadyByMesh = false;
    noClaim.receiptRows[1].expectedReadyByResident = false;
    const auto noClaimResult = noClaim.Validate();
    Check(noClaimResult.accepted && noClaim.facts.meshCalls == 1 &&
        noClaim.facts.residentCalls == 1,
        "non-rigid row carries no readiness claim rather than false claims");
}
}

int main()
{
    TestHeaderAuthorityAndPrefilter();
    TestRows();
    return failures == 0 ? 0 : 1;
}
