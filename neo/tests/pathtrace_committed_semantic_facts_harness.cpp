#include "../idlib/precompiled.h"
#include "PathTraceCommittedSemanticFacts.h"

#include <cstring>
#include <iostream>
#include <vector>

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

namespace {

int failures = 0;

void Check(bool condition, const char* label)
{
    std::cout << (condition ? "[PASS] " : "[FAIL] ") << label << '\n';
    failures += condition ? 0 : 1;
}

RtPathTraceCommittedSemanticConfig SemanticConfig(
    bool recordAll = false, bool removeRigid = false,
    bool emissiveCards = false)
{
    RtPathTraceCommittedSemanticConfig config;
    config.recordAllInstanceClasses = recordAll;
    config.removeRoutedRigidDynamic = removeRigid;
    config.rigidRouteEmissiveCards = emissiveCards;
    config.configFingerprint = 0x12345678ull;
    config.configComplete = true;
    return config;
}

RtPathTraceCommittedPlanningBaseline MakeBaseline()
{
    RtPathTraceCommittedPlanningBaseline baseline;
    baseline.epoch.committedViewToken = 69;
    baseline.epoch.sealedPrimaryViewToken = 69;
    baseline.epoch.baselineFrameIndex = 899;
    baseline.epoch.ownerUniverseFrameIndex = 900;
    baseline.epoch.worldLifecycleGeneration = 3;
    baseline.epoch.barrierGeneration = 4;
    baseline.epoch.instanceUniverseGeneration = 4;
    baseline.epoch.geometryUniverseGeneration = 5;
    baseline.epoch.staticMaterialGeneration = 6;
    baseline.epoch.capturedAfterRootEndFrame = true;
    baseline.epoch.capturedAfterStaticPrune = true;
    const RtPathTracePlanningSnapshotEpoch planningEpoch =
        RtPathTraceMapCommittedBaselineToPlanningEpoch(baseline.epoch);
    baseline.instanceUniverse.epoch = planningEpoch;
    baseline.instanceUniverse.ownerGeneration = 4;
    baseline.geometryUniverse.epoch = planningEpoch;
    baseline.geometryUniverse.ownerGeneration = 5;
    baseline.instanceUniverse.complete = true;
    baseline.geometryUniverse.complete = true;
    baseline.semanticConfig = SemanticConfig(true, true, true);
    baseline.applyGateComplete = true;
    baseline.priorSkinnedRecordsComplete = true;
    baseline.complete = true;
    return baseline;
}

RtSmokeRigidRouteReadyPod MakeRoute(std::uint64_t meshHash, bool ready)
{
    RtSmokeRigidRouteReadyPod row;
    row.valid = true;
    row.meshHash = meshHash;
    row.vertexBufferIdentity = 100 + meshHash;
    row.indexBufferIdentity = 200 + meshHash;
    row.materialId = 17;
    row.cachedRouteDataValid = true;
    row.localBoundsValid = true;
    row.cpuMeshContentSignature = 0x1234;
    row.sourceRange.vertices.count = 3;
    row.sourceRange.indexes.count = 3;
    row.sourceRange.triangles.count = 1;
    row.cachedVertexCount = 3;
    row.cachedIndexCount = 3;
    row.gpuBlasVertexCount = 3;
    row.gpuBlasIndexCount = 3;
    row.hasRigidVertexBuffer = true;
    row.hasRigidIndexBuffer = true;
    row.hasRigidBlas = ready;
    row.gpuBuffersUploaded = true;
    row.gpuBlasCreated = true;
    row.gpuBlasBuildSubmitted = true;
    row.gpuUploadSignature = RtPathTraceRigidUploadSignatureFromPod(row);
    return row;
}

void TestProvider()
{
    RtPathTraceCommittedPlanningBaseline baseline = MakeBaseline();
    const RtSmokeRigidRouteReadyPod ready = MakeRoute(1001, true);
    const RtSmokeRigidRouteReadyPod notReady = MakeRoute(1002, false);
    baseline.geometryUniverse.rigidRoutes = { ready, notReady };
    RtSmokeRigidResidentReadyPod resident;
    resident.meshHash = 1001;
    resident.entityIndex = 7;
    resident.renderEntityNum = 11;
    resident.materialId = 17;
    baseline.geometryUniverse.rigidResidents.push_back(resident);
    RtSmokeRigidResidentReadyPod residentNotReady = resident;
    residentNotReady.meshHash = 1002;
    residentNotReady.materialId = 18;
    baseline.geometryUniverse.rigidResidents.push_back(residentNotReady);

    RtPathTraceCommittedSemanticFactsProvider facts(baseline);
    Check(facts.IsRigidRouteReady(1001) ==
        RtPathTraceRigidRouteReadyRecordFromPod(ready),
        "committed ready route matches the production POD predicate");
    Check(facts.IsRigidRouteReady(1001), "ready route is true");
    Check(!facts.IsRigidRouteReady(1002), "not-ready route is false");
    Check(!facts.IsRigidRouteReady(9999), "absent route is false");
    Check(!facts.IsRigidRouteReady(0), "zero mesh hash is false");
    Check(!facts.IsRigidRouteResidentReadyForEntityMaterial(-1, 11, 17) &&
        !facts.IsRigidRouteResidentReadyForEntityMaterial(7, -1, 17),
        "negative resident identity is false");
    Check(!facts.IsRigidRouteResidentReadyForEntityMaterial(7, 11, 0),
        "zero resident material is false");
    Check(facts.IsRigidRouteResidentReadyForEntityMaterial(7, 11, 17),
        "exact resident match with ready route is true");
    Check(!facts.IsRigidRouteResidentReadyForEntityMaterial(7, 11, 18),
        "resident match whose route is not ready is false");
    Check(!facts.IsRigidRouteResidentReadyForEntityMaterial(8, 11, 17) &&
        !facts.IsRigidRouteResidentReadyForEntityMaterial(7, 12, 17) &&
        !facts.IsRigidRouteResidentReadyForEntityMaterial(7, 11, 19),
        "wrong entity, render entity, or material is false");

    PtCanonicalInstanceKey instance;
    PtCanonicalMeshKey mesh;
    Check(facts.FindCanonicalIdentityBinding(instance) == nullptr &&
        facts.FindCanonicalSourceRecord(mesh) == nullptr &&
        facts.CanonicalQueryViolationCount() == 2,
        "canonical methods return null only after recording violations");

    RtPathTraceCommittedPlanningBaseline invalid = baseline;
    invalid.epoch.committedViewToken = 0;
    RtPathTraceCommittedSemanticFactsProvider invalidFacts(invalid);
    Check(!invalidFacts.IsRigidRouteReady(1001) &&
        !invalidFacts.IsRigidRouteResidentReadyForEntityMaterial(7, 11, 17) &&
        invalidFacts.CanonicalQueryViolationCount() == 0,
        "invalid committed epoch rejects facts without canonical queries");
}

struct SemanticDtoFixture
{
    std::vector<RtPathTraceCaptureRawSurface> surfaces;
    std::vector<RtSmokeTranslucentClassifierStageInput> classifierStages;
    std::vector<RtPathTraceRuntimeMaterialStagePod> runtimeStages;
    std::vector<float> registers;
    std::vector<idDrawVert> vertices;
    std::vector<triIndex_t> indexes;
    std::vector<idJointMat> joints;
    std::vector<RtPathTraceCaptureModelTokenTablePod> modelTables;
    std::vector<std::uint64_t> modelSurfaceTokens;
    std::vector<RtPathTraceMaterialTextureVariantBasePod> variantBases;
    std::vector<RtPathTraceCaptureRegistryMaterialPod> registryMaterials;
    RtPathTracePrimarySemanticDto dto;

    SemanticDtoFixture()
    {
        classifierStages.resize(1);
        classifierStages[0].valid = true;
        runtimeStages.resize(1);
        runtimeStages[0].valid = true;
        registers = { 0.25f, 0.75f };
        vertices.resize(2);
        indexes = { 0, 1, 0 };
        joints.resize(1);
        modelTables.resize(1);
        modelTables[0].modelBits = 42;
        modelTables[0].tokenCount = 1;
        modelSurfaceTokens = { 0xabc };
        variantBases.resize(1);
        variantBases[0].variantId = 31;
        variantBases[0].baseId = 17;
        registryMaterials.resize(1);
        registryMaterials[0].materialId = 17;

        RtPathTraceCaptureRawSurface raw;
        raw.ordinal = 0;
        raw.safety = RtPathTraceCaptureSafetyDisposition::Ready;
        raw.classifierStageCount = 1;
        raw.runtimeStageCount = 1;
        raw.registerCount = 2;
        raw.vertexCount = 2;
        raw.indexCount = 3;
        raw.jointCount = 1;
        raw.modelTableIndex = 0;
        raw.modelBits = 42;
        raw.classify.material.materialPresent = true;
        raw.classifier.materialPresent = true;
        raw.materialIdentityBits = 0x12345678;
        surfaces.push_back(raw);

        dto.sealedPrimaryViewToken = 70;
        dto.materialRegistryGeneration = 8;
        dto.residentMaterialFactsGeneration = 9;
        dto.sourceDrawSurfCount = 1;
        dto.surfaces = surfaces.data();
        dto.surfaceCount = static_cast<std::uint32_t>(surfaces.size());
        dto.classifierStages = classifierStages.data();
        dto.classifierStageCount = static_cast<std::uint32_t>(classifierStages.size());
        dto.runtimeStages = runtimeStages.data();
        dto.runtimeStageCount = static_cast<std::uint32_t>(runtimeStages.size());
        dto.registers = registers.data();
        dto.registerCount = static_cast<std::uint32_t>(registers.size());
        dto.vertices = vertices.data();
        dto.vertexCount = static_cast<std::uint32_t>(vertices.size());
        dto.indexes = indexes.data();
        dto.indexCount = static_cast<std::uint32_t>(indexes.size());
        dto.joints = joints.data();
        dto.jointCount = static_cast<std::uint32_t>(joints.size());
        dto.modelTables = modelTables.data();
        dto.modelTableCount = static_cast<std::uint32_t>(modelTables.size());
        dto.modelSurfaceTokens = modelSurfaceTokens.data();
        dto.modelSurfaceTokenCount = static_cast<std::uint32_t>(modelSurfaceTokens.size());
        dto.variantBases = variantBases.data();
        dto.variantBaseCount = static_cast<std::uint32_t>(variantBases.size());
        dto.registryMaterials = registryMaterials.data();
        dto.registryMaterialCount = static_cast<std::uint32_t>(registryMaterials.size());
        dto.complete = true;
    }

    void RefreshPointers()
    {
        dto.surfaces = surfaces.empty() ? nullptr : surfaces.data();
        dto.surfaceCount = static_cast<std::uint32_t>(surfaces.size());
        dto.sourceDrawSurfCount = dto.surfaceCount;
        dto.classifierStages = classifierStages.empty()
            ? nullptr : classifierStages.data();
        dto.classifierStageCount = static_cast<std::uint32_t>(classifierStages.size());
        dto.runtimeStages = runtimeStages.empty() ? nullptr : runtimeStages.data();
        dto.runtimeStageCount = static_cast<std::uint32_t>(runtimeStages.size());
        dto.registers = registers.empty() ? nullptr : registers.data();
        dto.registerCount = static_cast<std::uint32_t>(registers.size());
        dto.vertices = vertices.empty() ? nullptr : vertices.data();
        dto.vertexCount = static_cast<std::uint32_t>(vertices.size());
        dto.indexes = indexes.empty() ? nullptr : indexes.data();
        dto.indexCount = static_cast<std::uint32_t>(indexes.size());
        dto.joints = joints.empty() ? nullptr : joints.data();
        dto.jointCount = static_cast<std::uint32_t>(joints.size());
        dto.modelTables = modelTables.empty() ? nullptr : modelTables.data();
        dto.modelTableCount = static_cast<std::uint32_t>(modelTables.size());
        dto.modelSurfaceTokens = modelSurfaceTokens.empty()
            ? nullptr : modelSurfaceTokens.data();
        dto.modelSurfaceTokenCount =
            static_cast<std::uint32_t>(modelSurfaceTokens.size());
        dto.variantBases = variantBases.empty() ? nullptr : variantBases.data();
        dto.variantBaseCount = static_cast<std::uint32_t>(variantBases.size());
        dto.registryMaterials = registryMaterials.empty()
            ? nullptr : registryMaterials.data();
        dto.registryMaterialCount =
            static_cast<std::uint32_t>(registryMaterials.size());
    }
};

template<typename T>
bool BytesEqual(const std::vector<T>& lhs, const std::vector<T>& rhs)
{
    return lhs.size() == rhs.size() &&
        (lhs.empty() || std::memcmp(lhs.data(), rhs.data(),
            lhs.size() * sizeof(T)) == 0);
}

void ExpectBridgeRejects(
    SemanticDtoFixture& fixture, std::uint64_t expectedToken,
    const char* label)
{
    fixture.RefreshPointers();
    RtPathTracePrimarySemanticScratch output;
    output.sealedPrimaryViewToken = 999;
    output.complete = true;
    Check(!BuildPathTracePrimarySemanticScratch(
            fixture.dto, expectedToken, SemanticConfig(), output) &&
        output.complete && output.sealedPrimaryViewToken == 999,
        label);
}

void TestShapeAuthority()
{
    const auto reject = [](const char* label, const auto& mutate)
    {
        SemanticDtoFixture fixture;
        mutate(fixture);
        ExpectBridgeRejects(fixture, 70, label);
    };

    {
        SemanticDtoFixture fixture;
        ExpectBridgeRejects(fixture, 71,
            "expected token mismatch rejects and preserves output");
    }
    {
        SemanticDtoFixture fixture;
        ExpectBridgeRejects(fixture, 0,
            "zero expected token rejects and preserves output");
    }
    reject("ordinal mismatch rejects and preserves output",
        [](SemanticDtoFixture& f) { f.surfaces[0].ordinal = 1; });
    reject("factsDerived rejects and preserves output",
        [](SemanticDtoFixture& f) { f.surfaces[0].semanticFactsDerived = true; });
    reject("factsPresent rejects and preserves output",
        [](SemanticDtoFixture& f) { f.surfaces[0].semanticFactsPresent = true; });
    reject("derived runtime material pod rejects and preserves output",
        [](SemanticDtoFixture& f) {
            f.surfaces[0].derivedRuntimeMaterial.initialCandidateId = 1;
        });
    reject("derived runtime material presence rejects and preserves output",
        [](SemanticDtoFixture& f) {
            f.surfaces[0].derivedRuntimeMaterialPresent = true;
        });
    reject("derived mesh hash rejects and preserves output",
        [](SemanticDtoFixture& f) { f.surfaces[0].derivedMeshHash = 1; });
    reject("derived chosen material rejects and preserves output",
        [](SemanticDtoFixture& f) { f.surfaces[0].derivedChosenMaterialId = 1; });
    reject("derived material class signature rejects and preserves output",
        [](SemanticDtoFixture& f) {
            f.surfaces[0].derivedMaterialClassSignature = 1;
        });
    reject("derived resolved surface rejects and preserves output",
        [](SemanticDtoFixture& f) {
            f.surfaces[0].derivedResolvedModelSurfaceIndex = 0;
        });
    reject("derived base material rejects and preserves output",
        [](SemanticDtoFixture& f) { f.surfaces[0].derivedBaseMaterialId = 1; });
    reject("rigid identity present rejects and preserves output",
        [](SemanticDtoFixture& f) { f.surfaces[0].rigidIdentityPresent = true; });

    reject("classifier offset past end rejects and preserves output",
        [](SemanticDtoFixture& f) { f.surfaces[0].classifierStageOffset = 2; });
    reject("classifier count overrun rejects and preserves output",
        [](SemanticDtoFixture& f) { f.surfaces[0].classifierStageCount = 2; });
    reject("runtime offset past end rejects and preserves output",
        [](SemanticDtoFixture& f) { f.surfaces[0].runtimeStageOffset = 2; });
    reject("runtime count overrun rejects and preserves output",
        [](SemanticDtoFixture& f) { f.surfaces[0].runtimeStageCount = 2; });
    reject("register offset past end rejects and preserves output",
        [](SemanticDtoFixture& f) { f.surfaces[0].registerOffset = 3; });
    reject("register count overrun rejects and preserves output",
        [](SemanticDtoFixture& f) { f.surfaces[0].registerCount = 3; });
    reject("vertex offset past end rejects and preserves output",
        [](SemanticDtoFixture& f) { f.surfaces[0].vertexOffset = 3; });
    reject("vertex count overrun rejects and preserves output",
        [](SemanticDtoFixture& f) { f.surfaces[0].vertexCount = 3; });
    reject("index offset past end rejects and preserves output",
        [](SemanticDtoFixture& f) { f.surfaces[0].indexOffset = 4; });
    reject("index count overrun rejects and preserves output",
        [](SemanticDtoFixture& f) { f.surfaces[0].indexCount = 4; });
    reject("joint offset past end rejects and preserves output",
        [](SemanticDtoFixture& f) { f.surfaces[0].jointOffset = 2; });
    reject("joint count overrun rejects and preserves output",
        [](SemanticDtoFixture& f) { f.surfaces[0].jointCount = 2; });

    reject("model table index bounds reject and preserve output",
        [](SemanticDtoFixture& f) { f.surfaces[0].modelTableIndex = 1; });
    reject("model token offset bounds reject and preserve output",
        [](SemanticDtoFixture& f) { f.modelTables[0].tokenOffset = 2; });
    reject("model token count total-minus-offset overrun rejects and preserves output",
        [](SemanticDtoFixture& f) {
            f.modelTables[0].tokenOffset = 1;
            f.modelTables[0].tokenCount = 1;
        });
    reject("model bits mismatch rejects and preserves output",
        [](SemanticDtoFixture& f) { f.surfaces[0].modelBits = 43; });
    reject("model epoch mismatch rejects and preserves output",
        [](SemanticDtoFixture& f) { f.surfaces[0].modelEpoch = 1; });
    reject("sentinel with nonzero model bits rejects and preserves output",
        [](SemanticDtoFixture& f) {
            f.surfaces[0].modelTableIndex = RT_PT_CAPTURE_INVALID_MODEL_TABLE;
            f.surfaces[0].modelBits = 1;
        });
    reject("unsorted registry materials reject and preserve output",
        [](SemanticDtoFixture& f) {
            f.registryMaterials.resize(2);
            f.registryMaterials[0].materialId = 2;
            f.registryMaterials[1].materialId = 1;
        });
    reject("unsorted variant bases reject and preserve output",
        [](SemanticDtoFixture& f) {
            f.variantBases.resize(2);
            f.variantBases[0].variantId = 2;
            f.variantBases[1].variantId = 1;
        });
    reject("duplicate registry material IDs reject per unique lookup contract",
        [](SemanticDtoFixture& f) {
            f.registryMaterials.resize(2);
            f.registryMaterials[0].materialId = 1;
            f.registryMaterials[1].materialId = 1;
        });
    reject("duplicate variant IDs reject per ID-keyed map contract",
        [](SemanticDtoFixture& f) {
            f.variantBases.resize(2);
            f.variantBases[0].variantId = 1;
            f.variantBases[1].variantId = 1;
        });

    {
        SemanticDtoFixture fixture;
        fixture.surfaces[0].modelTableIndex =
            RT_PT_CAPTURE_INVALID_MODEL_TABLE;
        fixture.surfaces[0].modelBits = 0;
        fixture.RefreshPointers();
        RtPathTracePrimarySemanticScratch output;
        Check(BuildPathTracePrimarySemanticScratch(
                fixture.dto, 70, SemanticConfig(), output),
            "sentinel with zero model bits passes");
    }
    {
        SemanticDtoFixture fixture;
        fixture.modelTables[0].tokenOffset = 1;
        fixture.modelTables[0].tokenCount = 0;
        fixture.RefreshPointers();
        RtPathTracePrimarySemanticScratch output;
        Check(BuildPathTracePrimarySemanticScratch(
                fixture.dto, 70, SemanticConfig(), output),
            "zero token count at token offset equal total passes");
    }
}

void TestBridge()
{
    SemanticDtoFixture source;
    const SemanticDtoFixture before = source;
    // Fixture copy changes pointers, so retain exact source pointer identities
    // independently from the byte copies above.
    const RtPathTracePrimarySemanticDto dtoBefore = source.dto;
    RtPathTracePrimarySemanticScratch scratch;
    Check(BuildPathTracePrimarySemanticScratch(
            source.dto, 70, SemanticConfig(), scratch),
        "exact-token semantic DTO builds owned scratch");
    SemanticDtoFixture emptySource;
    emptySource.surfaces.clear();
    emptySource.RefreshPointers();
    RtPathTracePrimarySemanticScratch emptyScratch;
    Check(BuildPathTracePrimarySemanticScratch(
            emptySource.dto, 70, SemanticConfig(), emptyScratch) &&
        scratch.ownedBytes ==
            emptyScratch.ownedBytes + sizeof(RtPathTraceCaptureRawSurface),
        "single cap charges actual grown raw-surface row size");
    Check(scratch.complete && scratch.sealedPrimaryViewToken == 70 &&
        scratch.snapshot.complete &&
        scratch.snapshot.registryGeneration == 8 &&
        scratch.snapshot.residentMaterialFactsGeneration == 9 &&
        scratch.snapshot.sourceDrawSurfCount == 1,
        "bridge preserves token, generations, source count, and completeness");
    Check(!scratch.snapshot.recordAllInstanceClasses &&
        !scratch.snapshot.removeRoutedRigidDynamic &&
        !scratch.snapshot.rigidRouteEmissiveCards &&
        scratch.snapshot.lateConsumeToken.configFingerprint == 0x12345678ull,
        "scratch receives false semantic config and existing witness exactly");
    RtPathTracePrimarySemanticScratch trueScratch;
    const RtPathTraceCommittedSemanticConfig trueConfig =
        SemanticConfig(true, true, true);
    Check(BuildPathTracePrimarySemanticScratch(
            source.dto, 70, trueConfig, trueScratch) &&
        trueScratch.snapshot.recordAllInstanceClasses &&
        trueScratch.snapshot.removeRoutedRigidDynamic &&
        trueScratch.snapshot.rigidRouteEmissiveCards &&
        trueScratch.snapshot.lateConsumeToken.configFingerprint ==
            trueConfig.configFingerprint,
        "scratch receives true semantic config and existing witness exactly");
    Check(BytesEqual(scratch.snapshot.surfaces, source.surfaces) &&
        BytesEqual(scratch.snapshot.classifierStages, source.classifierStages) &&
        BytesEqual(scratch.snapshot.runtimeStages, source.runtimeStages) &&
        BytesEqual(scratch.snapshot.registers, source.registers) &&
        BytesEqual(scratch.snapshot.vertices, source.vertices) &&
        BytesEqual(scratch.snapshot.indexes, source.indexes) &&
        BytesEqual(scratch.snapshot.joints, source.joints) &&
        BytesEqual(scratch.snapshot.modelTables, source.modelTables) &&
        BytesEqual(scratch.snapshot.modelSurfaceTokens, source.modelSurfaceTokens) &&
        BytesEqual(scratch.snapshot.variantBases, source.variantBases) &&
        BytesEqual(scratch.snapshot.registryMaterials, source.registryMaterials),
        "bridge round-trips every source row and associated array byte-exactly");
    Check(scratch.snapshot.capacityCounts.surfaces == source.dto.surfaceCount &&
        scratch.snapshot.capacityCounts.classifierStages ==
            source.dto.classifierStageCount &&
        scratch.snapshot.capacityCounts.runtimeStages ==
            source.dto.runtimeStageCount &&
        scratch.snapshot.capacityCounts.registers == source.dto.registerCount &&
        scratch.snapshot.capacityCounts.vertices == source.dto.vertexCount &&
        scratch.snapshot.capacityCounts.indexes == source.dto.indexCount &&
        scratch.snapshot.capacityCounts.joints == source.dto.jointCount &&
        scratch.snapshot.capacityCounts.modelTables == source.dto.modelTableCount &&
        scratch.snapshot.capacityCounts.modelSurfaceTokens ==
            source.dto.modelSurfaceTokenCount &&
        scratch.snapshot.capacityCounts.variantBases == source.dto.variantBaseCount &&
        scratch.snapshot.capacityCounts.registryMaterials ==
            source.dto.registryMaterialCount,
        "bridge preserves every DTO carrier count");
    Check(!scratch.snapshot.surfaces[0].semanticFactsDerived &&
        !scratch.snapshot.surfaces[0].semanticFactsPresent &&
        RtPathTraceRuntimeMaterialDecisionPodIsDefault(
            scratch.snapshot.surfaces[0].derivedRuntimeMaterial) &&
        !scratch.snapshot.surfaces[0].derivedRuntimeMaterialPresent &&
        scratch.snapshot.surfaces[0].derivedMeshHash == 0 &&
        scratch.snapshot.surfaces[0].derivedChosenMaterialId == 0 &&
        scratch.snapshot.surfaces[0].derivedMaterialClassSignature == 0 &&
        scratch.snapshot.surfaces[0].derivedResolvedModelSurfaceIndex == -1 &&
        scratch.snapshot.surfaces[0].derivedBaseMaterialId == 0 &&
        !scratch.snapshot.surfaces[0].rigidIdentityPresent,
        "bridge preserves explicit absent semantic facts and identity defaults");
    Check(source.dto.surfaces == dtoBefore.surfaces &&
        source.dto.classifierStages == dtoBefore.classifierStages &&
        source.dto.runtimeStages == dtoBefore.runtimeStages &&
        source.dto.registers == dtoBefore.registers &&
        source.dto.vertices == dtoBefore.vertices &&
        source.dto.indexes == dtoBefore.indexes &&
        source.dto.joints == dtoBefore.joints &&
        source.dto.modelTables == dtoBefore.modelTables &&
        source.dto.modelSurfaceTokens == dtoBefore.modelSurfaceTokens &&
        source.dto.variantBases == dtoBefore.variantBases &&
        source.dto.registryMaterials == dtoBefore.registryMaterials &&
        BytesEqual(source.surfaces, before.surfaces) &&
        BytesEqual(source.classifierStages, before.classifierStages) &&
        BytesEqual(source.runtimeStages, before.runtimeStages) &&
        BytesEqual(source.registers, before.registers) &&
        BytesEqual(source.vertices, before.vertices) &&
        BytesEqual(source.indexes, before.indexes) &&
        BytesEqual(source.joints, before.joints) &&
        BytesEqual(source.modelTables, before.modelTables) &&
        BytesEqual(source.modelSurfaceTokens, before.modelSurfaceTokens) &&
        BytesEqual(source.variantBases, before.variantBases) &&
        BytesEqual(source.registryMaterials, before.registryMaterials),
        "bridge does not mutate frontend DTO pointers or storage");

    RtPathTracePrimarySemanticScratch invalidConfigOutput;
    invalidConfigOutput.sealedPrimaryViewToken = 999;
    invalidConfigOutput.complete = true;
    RtPathTraceCommittedSemanticConfig incompleteConfig = SemanticConfig();
    incompleteConfig.configComplete = false;
    Check(!BuildPathTracePrimarySemanticScratch(source.dto, 70,
            incompleteConfig, invalidConfigOutput) &&
        invalidConfigOutput.complete &&
        invalidConfigOutput.sealedPrimaryViewToken == 999,
        "incomplete required semantic config rejects and preserves output");

    RtPathTracePrimarySemanticScratch preserved;
    preserved.sealedPrimaryViewToken = 999;
    preserved.complete = true;
    RtPathTracePrimarySemanticBridgeTestSeam badAlloc;
    badAlloc.failAtAllocation = 0;
    badAlloc.failure = RtPathTracePrimarySemanticBridgeFailure::BadAlloc;
    Check(!BuildPathTracePrimarySemanticScratch(source.dto, 70,
            SemanticConfig(), preserved, &badAlloc) && preserved.complete &&
        preserved.sealedPrimaryViewToken == 999,
        "bad_alloc is contained and preserves prior scratch");
    RtPathTracePrimarySemanticBridgeTestSeam lengthError;
    lengthError.failAtAllocation = 0;
    lengthError.failure = RtPathTracePrimarySemanticBridgeFailure::LengthError;
    Check(!BuildPathTracePrimarySemanticScratch(source.dto, 70,
            SemanticConfig(), preserved, &lengthError) && preserved.complete &&
        preserved.sealedPrimaryViewToken == 999,
        "length_error is contained and preserves prior scratch");

    RtPathTracePrimarySemanticDto overCap;
    float oneRegister = 1.0f;
    overCap.sealedPrimaryViewToken = 70;
    overCap.registers = &oneRegister;
    overCap.registerCount = UINT32_MAX;
    overCap.complete = true;
    Check(!BuildPathTracePrimarySemanticScratch(
            overCap, 70, SemanticConfig(), preserved) &&
        preserved.complete && preserved.sealedPrimaryViewToken == 999,
        "existing complete-slot cap rejects oversized scratch pre-allocation");
}

} // namespace

int main()
{
    TestProvider();
    TestShapeAuthority();
    TestBridge();
    return failures == 0 ? 0 : 1;
}
