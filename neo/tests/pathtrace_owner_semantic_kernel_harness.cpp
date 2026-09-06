#include "../idlib/precompiled.h"
#include "PathTraceOwnerSemanticKernel.h"
#include "PathTraceMaterialIdKernel.h"

#include <cstring>
#include <iostream>
#include <vector>

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

enum class ProviderCall { Binding, Source, Mesh, Resident };

class RecordingFacts final : public RtPathTraceCaptureSemanticFactsProvider
{
public:
    mutable std::vector<ProviderCall> calls;
    mutable std::uint32_t residentMaterialId = 0;
    mutable std::uint64_t queriedMeshHash = 0;
    bool meshReady = false;
    bool residentReady = false;
    bool supplyBinding = true;
    bool supplySource = true;
    PtGeometryIdentityBinding binding;
    PtGeometrySourceRecord source;

    bool IsRigidRouteReady(std::uint64_t meshHash) const override
    {
        calls.push_back(ProviderCall::Mesh);
        queriedMeshHash = meshHash;
        return meshReady;
    }

    bool IsRigidRouteResidentReadyForEntityMaterial(
        std::int32_t, std::int32_t, std::uint32_t materialId) const override
    {
        calls.push_back(ProviderCall::Resident);
        residentMaterialId = materialId;
        return residentReady;
    }

    const PtGeometryIdentityBinding* FindCanonicalIdentityBinding(
        const PtCanonicalInstanceKey&) const override
    {
        calls.push_back(ProviderCall::Binding);
        return supplyBinding ? &binding : nullptr;
    }

    const PtGeometrySourceRecord* FindCanonicalSourceRecord(
        const PtCanonicalMeshKey&) const override
    {
        calls.push_back(ProviderCall::Source);
        return supplySource ? &source : nullptr;
    }
};

RtPathTraceCaptureRawSurface MakeRaw(const char* materialName)
{
    RtPathTraceCaptureRawSurface raw;
    raw.safety = RtPathTraceCaptureSafetyDisposition::Ready;
    raw.entityIndex = 7;
    raw.entityNum = 11;
    raw.classify.hasEntityDef = true;
    raw.classify.material.materialPresent = true;
    raw.classify.material.deform = DFRM_NONE;
    raw.classify.material.coverage = MC_OPAQUE;
    raw.classifier.materialPresent = true;
    idStr::Copynz(raw.classify.material.materialName, materialName,
        sizeof(raw.classify.material.materialName));
    idStr::Copynz(raw.classifier.materialName, materialName,
        sizeof(raw.classifier.materialName));
    idStr::Copynz(raw.materialName, materialName, sizeof(raw.materialName));
    raw.modelMatrix[0] = raw.modelMatrix[5] = raw.modelMatrix[10] =
        raw.modelMatrix[15] = 1.0f;
    return raw;
}

bool RuntimeMaterialAbsent(const RtPathTraceCaptureRawSurface& raw)
{
    return RtPathTraceRuntimeMaterialDecisionPodIsDefault(
            raw.derivedRuntimeMaterial) &&
        !raw.derivedRuntimeMaterialPresent && raw.derivedChosenMaterialId == 0 &&
        raw.derivedBaseMaterialId == 0;
}

bool RigidIdentityDefaults(const RtPathTraceCaptureRawSurface& raw)
{
    return raw.derivedMeshHash == 0 &&
        raw.derivedMaterialClassSignature == 0 &&
        raw.derivedResolvedModelSurfaceIndex == -1 &&
        !raw.rigidIdentityPresent;
}

RtPathTraceCaptureRawSurface MakePromotedEmissive(
    RtPathTraceCaptureOwnerSnapshot& snapshot, const char* materialName)
{
    RtPathTraceCaptureRawSurface raw = MakeRaw(materialName);
    raw.classify.material.coverage = MC_TRANSLUCENT;
    raw.classify.material.stageCount = 1;
    raw.classifier.stageCount = 1;
    raw.classifierStageOffset = static_cast<std::uint32_t>(
        snapshot.classifierStages.size());
    raw.classifierStageCount = 1;
    RtSmokeTranslucentClassifierStageInput classifierStage;
    classifierStage.valid = true;
    classifierStage.hasImage = true;
    classifierStage.isAdditiveBlend = true;
    classifierStage.isAmbientStage = true;
    classifierStage.hasAmbientBlendStage = true;
    snapshot.classifierStages.push_back(classifierStage);

    raw.runtimeStageOffset = static_cast<std::uint32_t>(
        snapshot.runtimeStages.size());
    raw.runtimeStageCount = 1;
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
    snapshot.runtimeStages.push_back(runtimeStage);

    raw.registersPresent = true;
    raw.registerOffset = static_cast<std::uint32_t>(snapshot.registers.size());
    raw.registerCount = 5;
    snapshot.registers.insert(snapshot.registers.end(),
        { 1.0f, 2.0f, 1.0f, 0.5f, 1.0f });
    return raw;
}

void TestProviderOrderAndShortCircuit()
{
    RtPathTraceCaptureOwnerSnapshot snapshot;
    snapshot.removeRoutedRigidDynamic = true;
    snapshot.rigidRouteEmissiveCards = true;

    RtPathTraceCaptureRawSurface skinned = MakeRaw("models/test/skinned");
    skinned.classify.hasJointCache = true;
    skinned.rtCpuSkinned = true;
    skinned.jointCount = 1;
    skinned.jointSource = 1;
    snapshot.joints.resize(1);
    snapshot.surfaces.push_back(skinned);
    snapshot.surfaces.push_back(MakePromotedEmissive(snapshot,
        "lights/test_glow"));

    RecordingFacts facts;
    facts.binding.meshKey.sourceAssetId = 3;
    facts.source.payload.positions.resize(1);
    facts.source.payload.indexes.resize(3);
    Check(FinalizePathTraceOwnerSemanticSnapshot(snapshot, facts,
        nullptr, 0, true), "production finalizer accepts ordered provider case");
    const std::vector<ProviderCall> expected = {
        ProviderCall::Binding, ProviderCall::Source,
        ProviderCall::Mesh, ProviderCall::Resident };
    Check(facts.calls == expected, "provider calls are binding/source/mesh/resident");

    RtPathTraceCaptureOwnerSnapshot shortCircuit;
    shortCircuit.removeRoutedRigidDynamic = true;
    shortCircuit.rigidRouteEmissiveCards = true;
    shortCircuit.surfaces.push_back(MakePromotedEmissive(shortCircuit,
        "lights/test_glow"));
    RecordingFacts readyFacts;
    readyFacts.meshReady = true;
    Check(FinalizePathTraceOwnerSemanticSnapshot(shortCircuit, readyFacts,
        nullptr, 0, false), "mesh-ready short-circuit finalizes");
    Check(readyFacts.calls == std::vector<ProviderCall>{ ProviderCall::Mesh },
        "mesh-ready short-circuits resident lookup");
    Check(shortCircuit.surfaces[0].semanticFactsDerived &&
        shortCircuit.surfaces[0].semanticFactsPresent &&
        shortCircuit.surfaces[0].rigidReadyByMesh,
        "remove-routed true derives a positive rigid readiness fact");
    const RtPathTraceCaptureRawSurface& derived = snapshot.surfaces[1];
    const std::uint32_t expectedBaseMaterialId =
        HashPathTraceMaterialName(derived.materialName);
    const RtSmokeTranslucentClassifierInfo expectedClassifier =
        BuildSmokeTranslucentClassifierInfo(derived.classifier,
            shortCircuit.classifierStages.data() + derived.classifierStageOffset,
            static_cast<std::int32_t>(derived.classifierStageCount));
    const std::uint32_t expectedClassSignature =
        SmokeMaterialRouteClassSignatureFromPod(derived.classify.material,
            RtSmokeSurfaceClass::RigidEntity,
            ClassifySmokeTranslucentSubtypeFromPod(
                derived.classify.material, expectedClassifier),
            expectedClassifier);
    Check(derived.rigidIdentityPresent &&
        derived.derivedRuntimeMaterialPresent &&
        derived.derivedRuntimeMaterial.chosenMaterialId ==
            derived.derivedChosenMaterialId &&
        derived.derivedMeshHash == facts.queriedMeshHash &&
        derived.derivedChosenMaterialId == facts.residentMaterialId &&
        derived.derivedMaterialClassSignature == expectedClassSignature &&
        derived.derivedResolvedModelSurfaceIndex == -1 &&
        derived.derivedBaseMaterialId == expectedBaseMaterialId,
        "identity-reached row persists exact derived rigid identity fields");
}

void TestGateFalseAndAbsentDistinction()
{
    RtPathTraceCaptureOwnerSnapshot snapshot;
    RtPathTraceCaptureRawSurface skinned = MakeRaw("models/test/skinned");
    skinned.classify.hasJointCache = true;
    snapshot.surfaces.push_back(skinned);
    RecordingFacts facts;
    Check(FinalizePathTraceOwnerSemanticSnapshot(snapshot, facts,
        nullptr, 0, false), "gate-disabled skinned finalization succeeds");
    Check(facts.calls.empty(), "gate-disabled skinned path makes zero canonical calls");
    Check(snapshot.surfaces[0].semanticFactsDerived &&
        snapshot.surfaces[0].semanticFactsPresent &&
        snapshot.surfaces[0].derivedRuntimeMaterialPresent &&
        RigidIdentityDefaults(snapshot.surfaces[0]),
        "remove-gate early-out keeps material decision and no rigid identity");

    RtPathTraceCaptureOwnerSnapshot classRejected;
    classRejected.removeRoutedRigidDynamic = true;
    RtPathTraceCaptureRawSurface rejectedClass = MakeRaw("models/test/skinned");
    rejectedClass.classify.hasJointCache = true;
    classRejected.surfaces.push_back(rejectedClass);
    RecordingFacts rejectedFacts;
    Check(FinalizePathTraceOwnerSemanticSnapshot(classRejected, rejectedFacts,
            nullptr, 0, false) && rejectedFacts.calls.empty() &&
        classRejected.surfaces[0].semanticFactsDerived &&
        classRejected.surfaces[0].semanticFactsPresent &&
        classRejected.surfaces[0].derivedRuntimeMaterialPresent &&
        RigidIdentityDefaults(classRejected.surfaces[0]),
        "classification early-out keeps material decision and no rigid identity");

    RtPathTraceCaptureOwnerSnapshot falseFacts;
    falseFacts.removeRoutedRigidDynamic = true;
    falseFacts.surfaces.push_back(MakeRaw("models/test/rigid"));
    RecordingFacts notReady;
    Check(!RtPathTraceCaptureSemanticFactsReadable(falseFacts.surfaces[0]),
        "source row represents absent semantic facts");
    Check(FinalizePathTraceOwnerSemanticSnapshot(falseFacts, notReady,
        nullptr, 0, false), "false readiness finalization succeeds");
    Check(RtPathTraceCaptureSemanticFactsReadable(falseFacts.surfaces[0]) &&
        !falseFacts.surfaces[0].rigidReadyByMesh &&
        !falseFacts.surfaces[0].rigidReadyByResident,
        "derived false readiness remains distinct from absent facts");
}

void TestRuntimeMaterialDecisionDomain()
{
    RtPathTraceCaptureOwnerSnapshot staticWorld;
    staticWorld.removeRoutedRigidDynamic = true;
    RtPathTraceCaptureRawSurface world = MakeRaw("textures/test/static_world");
    world.classify.hasEntityDef = false;
    const RtSmokeTranslucentClassifierInfo worldClassifier =
        BuildSmokeTranslucentClassifierInfo(world.classifier, nullptr, 0);
    Check(ClassifySmokeSurfaceFromPod(world.classify, worldClassifier) ==
        RtSmokeSurfaceClass::StaticWorld,
        "static-world material fixture is not a promoted rigid row");
    staticWorld.surfaces.push_back(world);
    RecordingFacts worldFacts;
    Check(FinalizePathTraceOwnerSemanticSnapshot(staticWorld, worldFacts,
            nullptr, 0, false) && worldFacts.calls.empty() &&
        staticWorld.surfaces[0].derivedRuntimeMaterialPresent &&
        !staticWorld.surfaces[0].rigidIdentityPresent,
        "StaticWorld non-promoted row keeps material decision without rigid identity");

    RtPathTraceCaptureOwnerSnapshot exact;
    exact.removeRoutedRigidDynamic = false;
    exact.surfaces.push_back(MakePromotedEmissive(exact,
        "lights/test_material_domain"));
    const RtPathTraceCaptureRawSurface source = exact.surfaces[0];
    const RtSmokeTranslucentClassifierInfo classifier =
        BuildSmokeTranslucentClassifierInfo(source.classifier,
            exact.classifierStages.data() + source.classifierStageOffset,
            static_cast<std::int32_t>(source.classifierStageCount));
    const std::uint32_t baseMaterialId =
        HashPathTraceMaterialName(source.materialName);
    const float origin[3] = { source.modelMatrix[12], source.modelMatrix[13],
        source.modelMatrix[14] };
    const RtPathTraceRuntimeMaterialEvalPod eval =
        BuildPathTraceRuntimeMaterialEvalFromPod(true, baseMaterialId,
            exact.runtimeStages.data() + source.runtimeStageOffset,
            source.runtimeStageCount,
            exact.registers.data() + source.registerOffset,
            source.registerCount,
            SmokeMaterialUsesOpaqueSwinglightCompatibilityFromPod(
                source.classify.material, classifier),
            origin, true);
    const RtPathTraceRuntimeMaterialVariantPod key =
        BuildPathTraceRuntimeMaterialVariantKeyFromPod(baseMaterialId,
            source.entityIndex, source.entityNum,
            source.requestedModelSurfaceIndex, source.currentTriToken,
            nullptr, 0);
    const RtPathTraceRuntimeMaterialDecisionPod expected =
        BuildPathTraceRuntimeMaterialDecisionFromPod(key, eval,
            nullptr, 0, nullptr, 0);
    RecordingFacts exactFacts;
    Check(FinalizePathTraceOwnerSemanticSnapshot(exact, exactFacts,
            nullptr, 0, false) &&
        RtPathTraceRuntimeMaterialDecisionPodEqual(
            exact.surfaces[0].derivedRuntimeMaterial, expected) &&
        exact.surfaces[0].derivedBaseMaterialId == baseMaterialId &&
        exact.surfaces[0].derivedChosenMaterialId == expected.chosenMaterialId &&
        exact.surfaces[0].derivedRuntimeMaterialPresent &&
        !exact.surfaces[0].rigidIdentityPresent,
        "routing-disabled row persists the complete exact material decision once");

    RtPathTraceCaptureOwnerSnapshot badSpan;
    RtPathTraceCaptureRawSurface span = MakeRaw("textures/test/bad_span");
    span.runtimeStageOffset = 1;
    badSpan.surfaces.push_back(span);
    RecordingFacts spanFacts;
    Check(!FinalizePathTraceOwnerSemanticSnapshot(badSpan, spanFacts,
            nullptr, 0, false) &&
        RuntimeMaterialAbsent(badSpan.surfaces[0]) &&
        !badSpan.surfaces[0].semanticFactsDerived &&
        !badSpan.surfaces[0].semanticFactsPresent,
        "span failure keeps runtime material decision absent");

    RtPathTraceCaptureRawSurface zeroChosen;
    RtPathTraceRuntimeMaterialDecisionPod zeroDecision;
    zeroDecision.chosenMaterialId = 0;
    RtPathTracePersistDerivedRuntimeMaterialDecision(
        zeroChosen, 77, zeroDecision);
    Check(zeroChosen.derivedRuntimeMaterialPresent &&
        zeroChosen.derivedRuntimeMaterial.chosenMaterialId == 0 &&
        RtPathTraceDerivedRuntimeMaterialEmitsVariant(zeroChosen),
        "zero chosen material remains explicitly present without a zero sentinel");
    RtPathTraceCaptureRawSurface variant = zeroChosen;
    variant.derivedRuntimeMaterial.chosenMaterialId = 88;
    variant.derivedChosenMaterialId = 88;
    Check(RtPathTraceDerivedRuntimeMaterialEmitsVariant(variant) &&
        !RtPathTraceDerivedRuntimeMaterialEmitsVariant(
            RtPathTraceCaptureRawSurface{}),
        "variant emission predicate is presence and chosen differs from base");

    std::size_t presentWithoutRigid = 0;
    for (const RtPathTraceCaptureRawSurface* row : {
            &staticWorld.surfaces[0], &exact.surfaces[0] })
    {
        presentWithoutRigid += row->derivedRuntimeMaterialPresent &&
            !row->rigidIdentityPresent ? 1u : 0u;
    }
    Check(presentWithoutRigid == 2,
        "present-without-rigid difference set counts both valid rows");

    RtPathTraceRuntimeMaterialVariantPod fallbackKey;
    fallbackKey.baseMaterialId = 0x1234u;
    fallbackKey.entityIndex = 7;
    fallbackKey.entityNum = 11;
    RtPathTraceRuntimeMaterialEvalPod fallbackEval;
    fallbackEval.result = RtPathTraceRuntimeEvalBuildResult::Built;
    fallbackEval.materialId = fallbackKey.baseMaterialId;
    const RtPathTraceRuntimeMaterialDecisionPod fallbackDecision =
        SelectPathTraceRuntimeMaterialVariant(fallbackKey, fallbackEval,
            false, false,
            [](std::uint32_t) { return true; },
            [](std::uint32_t, std::uint32_t) { return false; });
    Check(fallbackDecision.initialCandidateId != fallbackKey.baseMaterialId &&
        fallbackDecision.collisionCount ==
            RT_PT_RUNTIME_MATERIAL_MAX_COLLISION_PROBES &&
        fallbackDecision.fallbackUsed &&
        fallbackDecision.chosenMaterialId == fallbackKey.baseMaterialId,
        "exhausted selector deterministically falls back after all probes");

    RtPathTraceCaptureRawSurface fallbackRow;
    RtPathTracePersistDerivedRuntimeMaterialDecision(
        fallbackRow, fallbackKey.baseMaterialId, fallbackDecision);
    Check(fallbackRow.derivedRuntimeMaterialPresent &&
        RtPathTraceRuntimeMaterialDecisionPodEqual(
            fallbackRow.derivedRuntimeMaterial, fallbackDecision),
        "fallback decision persists as an exact present decision");
    Check(!RtPathTraceDerivedRuntimeMaterialEmitsVariant(fallbackRow),
        "fallback does not emit a material variant when chosen equals base");

    // The serial diagnostic records selector intent from the initial candidate,
    // while apply emits only a chosen variant. Exhausted fallback deliberately
    // makes those domains diverge; compare paired counts within each domain.
    const bool serialOracleIntent =
        fallbackDecision.initialCandidateId != fallbackKey.baseMaterialId;
    const std::size_t serialIntentCount = serialOracleIntent ? 1u : 0u;
    const std::size_t serialVariantCount = serialOracleIntent ? 1u : 0u;
    const bool applyEmitsVariant =
        RtPathTraceDerivedRuntimeMaterialEmitsVariant(fallbackRow);
    const std::size_t applyIntentCount = applyEmitsVariant ? 1u : 0u;
    const std::size_t applyVariantCount = applyEmitsVariant ? 1u : 0u;
    Check(serialOracleIntent && serialIntentCount == serialVariantCount &&
        applyIntentCount == applyVariantCount &&
        serialVariantCount != applyVariantCount,
        "fallback keeps intent/variant pairs without oracle/product count equality");

    const std::size_t fallbackRows =
        fallbackRow.derivedRuntimeMaterialPresent &&
        fallbackRow.derivedRuntimeMaterial.fallbackUsed ? 1u : 0u;
    Check(fallbackRows == 1 && presentWithoutRigid == 2,
        "fallback rows are counted separately from present-without-rigid rows");
}

void TestActiveStageAndUnsafeRows()
{
	RtPathTraceCaptureOwnerSnapshot snapshot;
	snapshot.surfaces.push_back(MakePromotedEmissive(snapshot,
		"lights/test_glow"));
	RtPathTraceCaptureOwnerSnapshot disabled = snapshot;
	disabled.registers[0] = 0.0f;
	RecordingFacts facts;
	Check(FinalizePathTraceOwnerSemanticSnapshot(snapshot, facts,
		nullptr, 0, false), "active-emissive stage finalization succeeds");
	Check(snapshot.surfaces[0].activeEmissiveStage,
		"active-emissive stage honors copied stage/register inputs");
	RecordingFacts disabledFacts;
	Check(FinalizePathTraceOwnerSemanticSnapshot(disabled, disabledFacts,
		nullptr, 0, false) && !disabled.surfaces[0].activeEmissiveStage,
		"zero condition register disables the same emissive stage");

    RtPathTraceCaptureOwnerSnapshot unsafe;
    RtPathTraceCaptureRawSurface nullRow;
    nullRow.safety = RtPathTraceCaptureSafetyDisposition::NullSurface;
    unsafe.surfaces.push_back(nullRow);
    RtPathTraceCaptureRawSurface rejected = MakeRaw("models/test/unsafe");
    rejected.safety = RtPathTraceCaptureSafetyDisposition::CopyFailed;
    unsafe.surfaces.push_back(rejected);
    RecordingFacts unused;
    Check(FinalizePathTraceOwnerSemanticSnapshot(unsafe, unused,
        nullptr, 0, true), "null and unsafe rows early-continue");
    Check(unused.calls.empty(), "null and unsafe rows make zero provider calls");
    Check(unsafe.surfaces[0].semanticFactsDerived &&
        unsafe.surfaces[0].semanticFactsPresent &&
        unsafe.surfaces[1].semanticFactsDerived &&
        unsafe.surfaces[1].semanticFactsPresent &&
        RuntimeMaterialAbsent(unsafe.surfaces[0]) &&
        RuntimeMaterialAbsent(unsafe.surfaces[1]) &&
        RigidIdentityDefaults(unsafe.surfaces[0]) &&
        RigidIdentityDefaults(unsafe.surfaces[1]),
        "null and unsafe early-outs keep material and rigid facts absent");

    RtPathTraceCaptureOwnerSnapshot invalidIdentity;
    invalidIdentity.removeRoutedRigidDynamic = true;
    RtPathTraceCaptureRawSurface invalidModel = MakeRaw("models/test/rigid");
    invalidModel.modelBits = 1;
    invalidIdentity.surfaces.push_back(invalidModel);
    RecordingFacts invalidFacts;
    Check(FinalizePathTraceOwnerSemanticSnapshot(invalidIdentity, invalidFacts,
            nullptr, 0, false) && invalidFacts.calls.empty() &&
        invalidIdentity.surfaces[0].safety ==
            RtPathTraceCaptureSafetyDisposition::CopyFailed &&
        invalidIdentity.surfaces[0].semanticFactsDerived &&
        invalidIdentity.surfaces[0].semanticFactsPresent &&
        RuntimeMaterialAbsent(invalidIdentity.surfaces[0]) &&
        RigidIdentityDefaults(invalidIdentity.surfaces[0]),
        "invalid model-table input keeps material absent and marks derived failure");
}

void TestResidentUsesChosenMaterial()
{
    RtPathTraceCaptureOwnerSnapshot snapshot;
    snapshot.removeRoutedRigidDynamic = true;
    snapshot.rigidRouteEmissiveCards = true;
    snapshot.surfaces.push_back(MakePromotedEmissive(snapshot,
        "lights/test_variant_glow"));
    const std::uint32_t baseMaterialId =
        HashPathTraceMaterialName(snapshot.surfaces[0].materialName);
    RecordingFacts facts;
    Check(FinalizePathTraceOwnerSemanticSnapshot(snapshot, facts,
        nullptr, 0, false), "chosen-material resident case finalizes");
    Check(facts.residentMaterialId != 0 &&
        facts.residentMaterialId != baseMaterialId,
        "resident lookup receives chosen variant material, not base material");
}

void TestCommittedSemanticConfigFingerprint()
{
    RtPathTraceCaptureOwnerSnapshot serial;
    serial.recordAllInstanceClasses = true;
    serial.removeRoutedRigidDynamic = true;
    serial.rigidRouteEmissiveCards = true;
    serial.applyGate.enabled = true;
    serial.applyGate.acceptedSkinnedBuildLive = true;
    serial.admissionMaxSurfaces = 73;
    serial.admissionMaxBytes = 0x123456789ull;
    RtPathTraceCaptureRawSurface raw = MakeRaw("lights/config_glow");
    raw.guiAllowed = true;
    raw.callbackAllowed = true;
    raw.particleCompositeEnabled = true;
    raw.liquidPoolEnabled = true;
    raw.unifiedPtEnabled = true;
    raw.removeAlphaClipEnabled = true;
    serial.surfaces.push_back(raw);
    const RtPathTraceCaptureOwnerSnapshot source = serial;
    const std::uint64_t serialFingerprint =
        RtPathTraceOwnerSemanticConfigFingerprint(serial, 2, 5, false);
    const std::uint64_t sourceFingerprint =
        RtPathTraceOwnerSemanticConfigFingerprint(source, 2, 5, false);
    Check(serialFingerprint != 0 &&
        serialFingerprint == sourceFingerprint,
        "shared fingerprint matches serial/source for all existing inputs");
    RtPathTraceCaptureOwnerSnapshot admissionSurfacesChanged = source;
    ++admissionSurfacesChanged.admissionMaxSurfaces;
    RtPathTraceCaptureOwnerSnapshot admissionBytesChanged = source;
    ++admissionBytesChanged.admissionMaxBytes;
    Check(RtPathTraceOwnerSemanticConfigFingerprint(
            admissionSurfacesChanged, 2, 5, false) != serialFingerprint &&
        RtPathTraceOwnerSemanticConfigFingerprint(
            admissionBytesChanged, 2, 5, false) != serialFingerprint,
        "either committed admission authority changes the shared fingerprint");

    const auto derivedMutationLeavesFingerprint = [&](const auto& mutate)
    {
        RtPathTraceCaptureOwnerSnapshot mutated = source;
        mutate(mutated.surfaces[0]);
        return RtPathTraceOwnerSemanticConfigFingerprint(mutated, 2, 5, false) ==
                sourceFingerprint &&
            !RtPathTraceCaptureSourceSurfaceComplete(mutated.surfaces[0],
                mutated.classifierStages.size(), mutated.runtimeStages.size(),
                mutated.registers.size(), mutated.vertices.size(),
                mutated.indexes.size(), mutated.joints.size());
    };
    Check(derivedMutationLeavesFingerprint(
            [](RtPathTraceCaptureRawSurface& row) {
                row.derivedRuntimeMaterial.initialCandidateId = 1;
            }) &&
        derivedMutationLeavesFingerprint(
            [](RtPathTraceCaptureRawSurface& row) {
                row.derivedRuntimeMaterialPresent = true;
            }) &&
        derivedMutationLeavesFingerprint(
            [](RtPathTraceCaptureRawSurface& row) { row.derivedMeshHash = 1; }) &&
        derivedMutationLeavesFingerprint(
            [](RtPathTraceCaptureRawSurface& row) {
                row.derivedChosenMaterialId = 1;
            }) &&
        derivedMutationLeavesFingerprint(
            [](RtPathTraceCaptureRawSurface& row) {
                row.derivedMaterialClassSignature = 1;
            }) &&
        derivedMutationLeavesFingerprint(
            [](RtPathTraceCaptureRawSurface& row) {
                row.derivedResolvedModelSurfaceIndex = 0;
            }) &&
        derivedMutationLeavesFingerprint(
            [](RtPathTraceCaptureRawSurface& row) {
                row.derivedBaseMaterialId = 1;
            }) &&
        derivedMutationLeavesFingerprint(
            [](RtPathTraceCaptureRawSurface& row) {
                row.rigidIdentityPresent = true;
            }),
        "derived identity fields are excluded from config fingerprint but reject source shape");

    RtPathTraceCaptureRawSurface zeroHashIdentity;
    zeroHashIdentity.semanticFactsDerived = true;
    zeroHashIdentity.semanticFactsPresent = true;
    zeroHashIdentity.derivedMeshHash = 0;
    zeroHashIdentity.rigidIdentityPresent = true;
    Check(RtPathTraceCaptureSemanticFactsReadable(zeroHashIdentity) &&
        zeroHashIdentity.rigidIdentityPresent,
        "zero derived mesh hash is not an absence sentinel");

    RtPathTraceCommittedSemanticConfig committed;
    committed.recordAllInstanceClasses = true;
    committed.removeRoutedRigidDynamic = true;
    committed.rigidRouteEmissiveCards = true;
    committed.admissionMaxSurfaces = serial.admissionMaxSurfaces;
    committed.admissionMaxBytes = serial.admissionMaxBytes;
    committed.configFingerprint = serialFingerprint;
    committed.configComplete = true;
    Check(RtPathTraceCommittedSemanticConfigMatches(committed,
            true, true, true, serial.admissionMaxSurfaces,
            serial.admissionMaxBytes, sourceFingerprint),
        "exact committed semantic flags and fingerprint match");
    int preservedCandidate = 77;
    const bool fingerprintAccepted = RtPathTraceCommittedSemanticConfigMatches(
        committed, true, true, true, serial.admissionMaxSurfaces,
        serial.admissionMaxBytes, sourceFingerprint ^ 1ull);
    if (fingerprintAccepted)
    {
        preservedCandidate = 0;
    }
    Check(!fingerprintAccepted && preservedCandidate == 77,
        "fingerprint mismatch rejects before candidate mutation");
    const bool flippedFlagsAccepted = RtPathTraceCommittedSemanticConfigMatches(
        committed, true, false, true, serial.admissionMaxSurfaces,
        serial.admissionMaxBytes, sourceFingerprint);
    if (flippedFlagsAccepted)
    {
        preservedCandidate = 0;
    }
    Check(!flippedFlagsAccepted && preservedCandidate == 77,
        "semantic flag flip rejects before candidate mutation");

    RtPathTraceCaptureOwnerSnapshot enabled;
    enabled.removeRoutedRigidDynamic = true;
    enabled.rigidRouteEmissiveCards = true;
    enabled.surfaces.push_back(MakePromotedEmissive(enabled,
        "lights/config_promote"));
    RtPathTraceCaptureOwnerSnapshot disabled = enabled;
    disabled.removeRoutedRigidDynamic = false;
    disabled.rigidRouteEmissiveCards = false;
    RecordingFacts enabledFacts;
    enabledFacts.meshReady = true;
    RecordingFacts disabledFacts;
    Check(FinalizePathTraceOwnerSemanticSnapshot(enabled, enabledFacts,
            nullptr, 0, false) &&
        FinalizePathTraceOwnerSemanticSnapshot(disabled, disabledFacts,
            nullptr, 0, false),
        "true and false committed semantic configs finalize through shared kernel");
    Check(enabled.surfaces[0].rigidReadyByMesh &&
        !disabled.surfaces[0].rigidReadyByMesh &&
        enabledFacts.calls == std::vector<ProviderCall>{ ProviderCall::Mesh } &&
        disabledFacts.calls.empty(),
        "decision flags reproduce enabled and serial-false readiness behavior");
    Check(enabled.surfaces[0].rigidReadyByMesh &&
        enabledFacts.calls == std::vector<ProviderCall>{ ProviderCall::Mesh },
        "rigidRouteEmissiveCards true promotes emissive card into rigid readiness");
}

}

int main()
{
    TestProviderOrderAndShortCircuit();
    TestGateFalseAndAbsentDistinction();
    TestRuntimeMaterialDecisionDomain();
    TestActiveStageAndUnsafeRows();
    TestResidentUsesChosenMaterial();
    TestCommittedSemanticConfigFingerprint();
    return failures == 0 ? 0 : 1;
}
