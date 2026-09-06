#include "../idlib/precompiled.h"
#include "PathTraceCommittedSemanticFinalize.h"
#include "PathTraceOwnerSemanticKernel.h"

#include <cstring>
#include <iostream>
#include <vector>

idCommon* idLib::common = nullptr;

bool AssertFailed(const char*, int, const char*) { return false; }
void* Mem_Alloc16(std::size_t size, memTag_t) { return _aligned_malloc(size, 16); }
void Mem_Free16(void* pointer) { _aligned_free(pointer); }
void* Mem_ClearedAlloc(std::size_t size, memTag_t tag)
{
    void* pointer = Mem_Alloc16(size, tag);
    if (pointer) { std::memset(pointer, 0, size); }
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

class RecordingFalseFacts final : public RtPathTraceCaptureSemanticFactsProvider
{
public:
    mutable std::size_t rigidCalls = 0;
    mutable std::size_t residentCalls = 0;
    mutable std::size_t canonicalCalls = 0;
    bool IsRigidRouteReady(std::uint64_t) const override
    {
        ++rigidCalls;
        return false;
    }
    bool IsRigidRouteResidentReadyForEntityMaterial(
        std::int32_t, std::int32_t, std::uint32_t) const override
    {
        ++residentCalls;
        return false;
    }
    const PtGeometryIdentityBinding* FindCanonicalIdentityBinding(
        const PtCanonicalInstanceKey&) const override
    {
        ++canonicalCalls;
        return nullptr;
    }
    const PtGeometrySourceRecord* FindCanonicalSourceRecord(
        const PtCanonicalMeshKey&) const override
    {
        ++canonicalCalls;
        return nullptr;
    }
};

struct Fixture
{
    std::vector<RtPathTraceCaptureRawSurface> surfaces;
    RtPathTraceCommittedPlanningBaseline baseline;
    RtPathTraceCommittedGeometryProduct product;
    RtPathTracePrimaryViewDtoLineage lineage;
    RtPathTraceCurrentSemanticConfig current{ true, true, true, 64, 65536,
        true };
    int mode = 2;
    int mask = 5;
    bool splitGate = false;
    bool admissionRoutes = false;
    bool applyEnabled = true;
    bool acceptedSkinned = true;

    Fixture()
    {
        baseline.epoch.committedViewToken = 70;
        baseline.epoch.sealedPrimaryViewToken = 70;
        baseline.epoch.baselineFrameIndex = 700;
        baseline.epoch.ownerUniverseFrameIndex = 900;
        baseline.epoch.worldLifecycleGeneration = 3;
        baseline.epoch.mapLoadSerial = 4;
        baseline.epoch.mapTimeStamp = 5;
        std::strcpy(baseline.epoch.mapName, "maps/p2b.proc");
        baseline.epoch.barrierGeneration = 6;
        baseline.epoch.instanceUniverseGeneration = 9;
        baseline.epoch.geometryUniverseGeneration = 7;
        baseline.epoch.staticMaterialGeneration = 8;
        baseline.epoch.capturedAfterRootEndFrame = true;
        baseline.epoch.capturedAfterStaticPrune = true;
        baseline.semanticConfig.recordAllInstanceClasses = true;
        baseline.semanticConfig.removeRoutedRigidDynamic = true;
        baseline.semanticConfig.rigidRouteEmissiveCards = true;
        baseline.semanticConfig.admissionMaxSurfaces = 64;
        baseline.semanticConfig.admissionMaxBytes = 65536;
        baseline.semanticConfig.configFingerprint = 0x1111;
        baseline.semanticConfig.configComplete = true;
        const RtPathTracePlanningSnapshotEpoch mapped =
            RtPathTraceMapCommittedBaselineToPlanningEpoch(baseline.epoch);
        baseline.instanceUniverse.epoch = mapped;
        baseline.instanceUniverse.ownerGeneration = 9;
        baseline.geometryUniverse.epoch = mapped;
        baseline.geometryUniverse.ownerGeneration = 7;
        baseline.instanceUniverse.complete = true;
        baseline.geometryUniverse.complete = true;
        baseline.applyGateComplete = true;
        baseline.priorSkinnedRecordsComplete = true;
        baseline.complete = true;

        RtPathTraceCaptureRawSurface raw;
        raw.safety = RtPathTraceCaptureSafetyDisposition::Ready;
        raw.classify.hasEntityDef = true;
        raw.classify.material.materialPresent = true;
        raw.classify.material.deform = DFRM_NONE;
        raw.classify.material.coverage = MC_OPAQUE;
        raw.classifier.materialPresent = true;
        std::strcpy(raw.materialName, "models/p2b/rigid");
        surfaces.push_back(raw);

        product.sealedPrimaryViewToken = 71;
        product.complete = true;
        product.semanticDto.sealedPrimaryViewToken = 71;
        product.semanticDto.sourceDrawSurfCount = 1;
        product.semanticDto.surfaceCount = 1;
        product.semanticDto.surfaces = surfaces.data();
        product.semanticDto.complete = true;

        lineage.sealedViewToken = 71;
        lineage.predecessorCommittedViewToken = 70;
        lineage.frameIndex = 701;
        lineage.worldLifecycleGeneration = 3;
        lineage.mapLoadSerial = 4;
        lineage.mapTimeStamp = 5;
        std::strcpy(lineage.mapName, "maps/p2b.proc");
        lineage.barrierGeneration = 6;
        lineage.primaryView = true;
        lineage.complete = true;
        RefreshFingerprint();
    }

    void RefreshPointers()
    {
        product.semanticDto.surfaces = surfaces.empty() ? nullptr : surfaces.data();
        product.semanticDto.surfaceCount =
            static_cast<std::uint32_t>(surfaces.size());
        product.semanticDto.sourceDrawSurfCount = product.semanticDto.surfaceCount;
    }

    void RefreshFingerprint()
    {
        RefreshPointers();
        RtPathTracePrimarySemanticScratch scratch;
        const bool built = BuildPathTracePrimarySemanticScratch(
            product.semanticDto, product.sealedPrimaryViewToken,
            baseline.semanticConfig, scratch);
        if (!built)
        {
            product.configFingerprint = 0;
            lineage.configFingerprint = 0;
            return;
        }
        scratch.snapshot.applyGate.enabled = applyEnabled;
        scratch.snapshot.applyGate.acceptedSkinnedBuildLive = acceptedSkinned;
        product.configFingerprint = RtPathTraceOwnerSemanticConfigFingerprint(
            scratch.snapshot, mode, mask, splitGate);
        lineage.configFingerprint = product.configFingerprint;
    }

    RtPathTraceCommittedSemanticFinalizeInput Input() const
    {
        return { product, lineage, baseline, current, mode, mask, splitGate,
            admissionRoutes, applyEnabled, acceptedSkinned };
    }
};

bool RawRowsEqual(const RtPathTracePrimarySemanticScratch& lhs,
    const RtPathTracePrimarySemanticScratch& rhs)
{
    return lhs.snapshot.surfaces.size() == rhs.snapshot.surfaces.size() &&
        (lhs.snapshot.surfaces.empty() ||
            std::memcmp(lhs.snapshot.surfaces.data(),
                rhs.snapshot.surfaces.data(),
                lhs.snapshot.surfaces.size() *
                    sizeof(RtPathTraceCaptureRawSurface)) == 0);
}

void ExpectReject(Fixture& fixture,
    RtPathTraceCommittedSemanticFinalizeReject expected, const char* label,
    const RtPathTraceCommittedSemanticFinalizeTestSeam* seam = nullptr)
{
    fixture.RefreshPointers();
    RtPathTracePrimarySemanticScratch output;
    output.sealedPrimaryViewToken = 999;
    output.complete = true;
    const RtPathTraceCommittedSemanticFinalizeResult result =
        FinalizePathTraceCommittedPrimarySemanticP2b(
            fixture.Input(), output, seam);
    const bool expectsFinalizer = expected ==
            RtPathTraceCommittedSemanticFinalizeReject::FinalizerFailure ||
        expected == RtPathTraceCommittedSemanticFinalizeReject::
            CanonicalQueryViolation;
    Check(!result.accepted && result.rejection == expected &&
        output.complete && output.sealedPrimaryViewToken == 999 &&
        (expectsFinalizer || (result.finalizerCalls == 0 &&
            result.rigidReadyQueries == 0 &&
            result.residentReadyQueries == 0 &&
            result.canonicalQueryViolations == 0)),
        label);
}

void TestAcceptedParity()
{
    Fixture fixture;
    const std::vector<RtPathTraceCaptureRawSurface> sourceBefore =
        fixture.surfaces;
    const RtPathTracePrimarySemanticDto dtoBefore = fixture.product.semanticDto;
    RtPathTracePrimarySemanticScratch output;
    const RtPathTraceCommittedSemanticFinalizeResult result =
        FinalizePathTraceCommittedPrimarySemanticP2b(fixture.Input(), output);
    Check(result.accepted && result.rejection ==
            RtPathTraceCommittedSemanticFinalizeReject::None &&
        result.finalizerCalls == 1 && result.canonicalQueryViolations == 0 &&
        result.derivedSurfaceCount == fixture.surfaces.size(),
        "exact guarded input accepts once with zero canonical queries");
    Check(output.snapshot.surfaces.size() == 1 &&
        output.snapshot.surfaces[0].semanticFactsDerived &&
        output.snapshot.surfaces[0].semanticFactsPresent,
        "accepted eligible rows are explicitly derived and present");

    RtPathTracePrimarySemanticScratch direct;
    Check(BuildPathTracePrimarySemanticScratch(fixture.product.semanticDto, 71,
            fixture.baseline.semanticConfig, direct),
        "direct parity scratch builds");
    direct.snapshot.applyGate.enabled = fixture.applyEnabled;
    direct.snapshot.applyGate.acceptedSkinnedBuildLive = fixture.acceptedSkinned;
    RecordingFalseFacts facts;
    Check(FinalizePathTraceOwnerSemanticSnapshot(
            direct.snapshot, facts, nullptr, 0, false) &&
        RawRowsEqual(output, direct),
        "guarded result is byte-equivalent to recording live-equivalent finalizer");
    const RtPathTraceCaptureRawSurface& guardedRow = output.snapshot.surfaces[0];
    const RtPathTraceCaptureRawSurface& directRow = direct.snapshot.surfaces[0];
    Check(guardedRow.rigidIdentityPresent &&
        guardedRow.derivedMeshHash == directRow.derivedMeshHash &&
        guardedRow.derivedChosenMaterialId == directRow.derivedChosenMaterialId &&
        guardedRow.derivedMaterialClassSignature ==
            directRow.derivedMaterialClassSignature &&
        guardedRow.derivedResolvedModelSurfaceIndex ==
            directRow.derivedResolvedModelSurfaceIndex &&
        guardedRow.derivedBaseMaterialId == directRow.derivedBaseMaterialId &&
        guardedRow.rigidIdentityPresent == directRow.rigidIdentityPresent,
        "guarded and direct finalization persist identical rigid identity fields");
    Check(facts.canonicalCalls == 0,
        "eligible direct parity provider makes zero canonical calls");
    Check(std::memcmp(fixture.surfaces.data(), sourceBefore.data(),
            sourceBefore.size() * sizeof(RtPathTraceCaptureRawSurface)) == 0 &&
        fixture.product.semanticDto.surfaces == dtoBefore.surfaces &&
        fixture.product.configFingerprint == fixture.lineage.configFingerprint,
        "accepted finalization leaves source DTO and product immutable");
}

void TestPrecheckMatrix()
{
    { Fixture f; f.baseline.complete = false; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::BaselineInvalid,
        "baseline invalid rejects before work"); }
    { Fixture f; f.product.semanticDto.factsDerivedSurfaceCount = 1; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::SourceFactsAlreadyDerived,
        "already-derived source rejects before work"); }
    { Fixture f; f.product.sealedPrimaryViewToken = 0; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::TokenZero,
        "zero token rejects before work"); }
    { Fixture f; f.lineage.sealedViewToken = 72; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::TokenMismatch,
        "token mismatch rejects before work"); }
    { Fixture f; f.product.semanticDto.complete = false; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::SourceCarrierInvalid,
        "carrier incomplete rejects before work"); }
    { Fixture f; f.surfaces[0].ordinal = 1; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::SourceShapeInvalid,
        "shape mismatch rejects before work"); }
    { Fixture f; f.surfaces[0].derivedMeshHash = 1; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::SourceShapeInvalid,
        "source derived mesh hash rejects before candidate mutation"); }
    { Fixture f; f.surfaces[0].derivedChosenMaterialId = 1; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::SourceShapeInvalid,
        "source derived chosen material rejects before candidate mutation"); }
    { Fixture f; f.surfaces[0].derivedMaterialClassSignature = 1; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::SourceShapeInvalid,
        "source derived class signature rejects before candidate mutation"); }
    { Fixture f; f.surfaces[0].derivedResolvedModelSurfaceIndex = 0; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::SourceShapeInvalid,
        "source derived model surface rejects before candidate mutation"); }
    { Fixture f; f.surfaces[0].derivedBaseMaterialId = 1; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::SourceShapeInvalid,
        "source derived base material rejects before candidate mutation"); }
    { Fixture f; f.surfaces[0].rigidIdentityPresent = true; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::SourceShapeInvalid,
        "source rigid identity presence rejects before candidate mutation"); }
    { Fixture f; f.lineage.complete = false; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::LineageInvalid,
        "lineage incomplete rejects before work"); }
    { Fixture f; f.product.complete = false; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::LineageInvalid,
        "product incomplete rejects before work"); }
    { Fixture f; f.lineage.frameIndex = 700; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::FrameStale,
        "stale frame rejects without owner-universe conflation"); }
    { Fixture f; f.lineage.frameIndex = 702; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::FrameTooNew,
        "too-new frame rejects without owner-universe conflation"); }
    { Fixture f; f.lineage.mapName[0] = 'x'; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::MapNameMismatch,
        "map name mismatch is attributable"); }
    { Fixture f; ++f.lineage.mapTimeStamp; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::MapTimeMismatch,
        "map timestamp mismatch is attributable"); }
    { Fixture f; ++f.lineage.mapLoadSerial; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::MapLoadSerialMismatch,
        "map load serial mismatch is attributable"); }
    { Fixture f; ++f.lineage.worldLifecycleGeneration; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::WorldLifecycleMismatch,
        "world lifecycle mismatch is attributable"); }
    { Fixture f; ++f.lineage.barrierGeneration; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::BarrierMismatch,
        "barrier mismatch is attributable"); }
    { Fixture f; f.baseline.dispatchGuard.baselineSkinnedSplitGate = true; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::BaselineSplitGate,
        "baseline split gate makes zero finalizer calls"); }
    { Fixture f; f.baseline.dispatchGuard.baselineAdmissionRoutesPresent = true; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::BaselineAdmissionRoutes,
        "baseline admission routes make zero finalizer calls"); }
    { Fixture f; f.baseline.dispatchGuard.captureAllocationFailureArmed = true; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::CaptureAllocationSeam,
        "capture allocation seam makes zero finalizer calls"); }
    { Fixture f; f.baseline.dispatchGuard.producerAllocationFailureArmed = true; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::ProducerAllocationSeam,
        "producer allocation seam makes zero finalizer calls"); }
    { Fixture f; f.product.configFingerprint = 0; f.lineage.configFingerprint = 0; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::TransportedFingerprintZero,
        "zero transported fingerprint rejects before work"); }
    { Fixture f; ++f.lineage.configFingerprint; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::TransportedFingerprintMismatch,
        "product-lineage fingerprint mismatch rejects before work"); }
    { Fixture f; f.current.configComplete = false; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::CurrentConfigIncomplete,
        "incomplete current config rejects before work"); }
    { Fixture f; f.splitGate = true; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::CurrentSplitGate,
        "current split gate makes zero finalizer calls"); }
    { Fixture f; f.admissionRoutes = true; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::CurrentAdmissionRoutes,
        "current admission routes make zero finalizer calls"); }
}

void TestConfigAndFailureMatrix()
{
    { Fixture f; f.current.removeRoutedRigidDynamic = false; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::CurrentConfigMismatch,
        "current decision flag flip rejects whole candidate"); }
    {
        Fixture f;
        f.current.removeRoutedRigidDynamic = false;
        RtPathTracePrimarySemanticBridgeTestSeam allocation;
        allocation.failAtAllocation = 0;
        allocation.failure = RtPathTracePrimarySemanticBridgeFailure::BadAlloc;
        RtPathTraceCommittedSemanticFinalizeTestSeam seam;
        seam.bridge = &allocation;
        f.RefreshPointers();
        RtPathTracePrimarySemanticScratch output;
        output.sealedPrimaryViewToken = 999;
        output.ownedBytes = 12345;
        output.snapshot.complete = true;
        output.complete = true;
        const RtPathTraceCommittedSemanticFinalizeResult result =
            FinalizePathTraceCommittedPrimarySemanticP2b(
                f.Input(), output, &seam);
        Check(!result.accepted && result.rejection ==
                RtPathTraceCommittedSemanticFinalizeReject::CurrentConfigMismatch &&
            allocation.allocationCalls == 0 && result.finalizerCalls == 0 &&
            result.rigidReadyQueries == 0 && result.residentReadyQueries == 0 &&
            result.canonicalQueryViolations == 0 && output.complete &&
            output.sealedPrimaryViewToken == 999 && output.ownedBytes == 12345 &&
            output.snapshot.complete,
            "current config mismatch rejects before bridge allocation");
    }
    { Fixture f; f.mode = 1; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::FingerprintMismatch,
        "current mode mutation rejects fingerprint"); }
    { Fixture f; f.mask ^= 1; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::FingerprintMismatch,
        "current mask mutation rejects fingerprint"); }
    { Fixture f; f.applyEnabled = false; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::FingerprintMismatch,
        "current apply-enabled mutation rejects fingerprint"); }
    { Fixture f; f.acceptedSkinned = false; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::FingerprintMismatch,
        "current accepted-skinned mutation rejects fingerprint"); }
    { Fixture f; f.surfaces[0].guiAllowed = true; ExpectReject(f,
        RtPathTraceCommittedSemanticFinalizeReject::FingerprintMismatch,
        "per-row fingerprint mutation rejects whole candidate"); }

    Fixture badAllocFixture;
    RtPathTracePrimarySemanticBridgeTestSeam badAlloc;
    badAlloc.failAtAllocation = 0;
    badAlloc.failure = RtPathTracePrimarySemanticBridgeFailure::BadAlloc;
    RtPathTraceCommittedSemanticFinalizeTestSeam badAllocSeam;
    badAllocSeam.bridge = &badAlloc;
    ExpectReject(badAllocFixture,
        RtPathTraceCommittedSemanticFinalizeReject::BridgeBadAlloc,
        "bridge bad_alloc preserves output", &badAllocSeam);
    Fixture lengthFixture;
    RtPathTracePrimarySemanticBridgeTestSeam length;
    length.failAtAllocation = 0;
    length.failure = RtPathTracePrimarySemanticBridgeFailure::LengthError;
    RtPathTraceCommittedSemanticFinalizeTestSeam lengthSeam;
    lengthSeam.bridge = &length;
    ExpectReject(lengthFixture,
        RtPathTraceCommittedSemanticFinalizeReject::BridgeLengthError,
        "bridge length_error preserves output", &lengthSeam);

    Fixture finalizerFixture;
    RtPathTraceCommittedSemanticFinalizeTestSeam invalid;
    invalid.invalidateFirstSurfaceBeforeFinalize = true;
    ExpectReject(finalizerFixture,
        RtPathTraceCommittedSemanticFinalizeReject::FinalizerFailure,
        "finalizer failure preserves output", &invalid);
    Fixture canonicalFixture;
    RtPathTraceCommittedSemanticFinalizeTestSeam canonical;
    canonical.injectCanonicalQueryViolation = true;
    ExpectReject(canonicalFixture,
        RtPathTraceCommittedSemanticFinalizeReject::CanonicalQueryViolation,
        "forced canonical violation rejects whole candidate", &canonical);

    Fixture capFixture;
    float one = 1.0f;
    capFixture.surfaces.clear();
    capFixture.RefreshPointers();
    capFixture.product.semanticDto.registers = &one;
    capFixture.product.semanticDto.registerCount = UINT32_MAX;
    capFixture.product.semanticDto.sourceDrawSurfCount = 0;
    capFixture.product.semanticDto.surfaceCount = 0;
    ExpectReject(capFixture,
        RtPathTraceCommittedSemanticFinalizeReject::BridgeFailure,
        "single bridge cap rejects and preserves output");
}
}

int main()
{
    TestAcceptedParity();
    TestPrecheckMatrix();
    TestConfigAndFailureMatrix();
    return failures == 0 ? 0 : 1;
}
