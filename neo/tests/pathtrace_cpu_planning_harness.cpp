#include "../renderer/NVRHI/PathTraceAccelerationPlan.h"
#include "../renderer/NVRHI/PathTraceCpuWork.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct HarnessSmokeVertex
{
    float position[4];
    float normal[4];
    float texCoord[4];
    float color[4];
    float color2[4];
};

int g_failures = 0;

void Check(bool condition, const char* name)
{
    if (condition)
    {
        std::cout << "[PASS] " << name << "\n";
    }
    else
    {
        std::cout << "[FAIL] " << name << "\n";
        ++g_failures;
    }
}

std::vector<HarnessSmokeVertex> BuildTriangleVertices(float materialMarker)
{
    std::vector<HarnessSmokeVertex> vertices(3);
    vertices[0].position[0] = 0.0f;
    vertices[0].position[1] = 0.0f;
    vertices[0].position[2] = 0.0f;
    vertices[1].position[0] = 1.0f;
    vertices[1].position[1] = 0.0f;
    vertices[1].position[2] = 0.0f;
    vertices[2].position[0] = 0.0f;
    vertices[2].position[1] = 1.0f;
    vertices[2].position[2] = 0.0f;
    for (HarnessSmokeVertex& vertex : vertices)
    {
        vertex.position[3] = 1.0f;
        vertex.normal[2] = 1.0f;
        vertex.texCoord[0] = materialMarker;
        vertex.color[0] = 1.0f;
        vertex.color[1] = 1.0f;
        vertex.color[2] = 1.0f;
        vertex.color[3] = 1.0f;
    }
    return vertices;
}

RtSmokePlanStaticBlasSignatureDesc BuildSignatureDesc(
    const std::vector<HarnessSmokeVertex>& vertices,
    const std::vector<uint32_t>& indexes,
    const std::vector<uint32_t>& classes,
    const std::vector<uint32_t>& materials)
{
    RtSmokePlanStaticBlasSignatureDesc desc;
    desc.vertices = vertices.data();
    desc.vertexStride = sizeof(vertices[0]);
    desc.totalVertexCount = static_cast<int>(vertices.size());
    desc.indexes = indexes.data();
    desc.totalIndexCount = static_cast<int>(indexes.size());
    desc.triangleClasses = classes.data();
    desc.triangleMaterials = materials.data();
    desc.totalTriangleCount = static_cast<int>(classes.size());
    desc.staticRange.vertexCount = static_cast<int>(vertices.size());
    desc.staticRange.indexCount = static_cast<int>(indexes.size());
    desc.staticRange.triangleCount = static_cast<int>(classes.size());
    return desc;
}

RtSmokeAccelerationPlan BuildPlan(uint64_t previousHash, bool cacheValid, bool resourcesReady, bool cacheChanged)
{
    std::vector<HarnessSmokeVertex> vertices = BuildTriangleVertices(0.25f);
    std::vector<uint32_t> indexes = { 0, 1, 2 };
    std::vector<uint32_t> classes = { 7 };
    std::vector<uint32_t> materials = { 11 };

    RtSmokeAccelerationPlanInput input;
    input.staticSignature = BuildSignatureDesc(vertices, indexes, classes, materials);
    input.staticCache.cacheValid = cacheValid;
    input.staticCache.cacheResourcesReady = resourcesReady;
    input.staticCache.staticCacheChanged = cacheChanged;
    input.staticCache.previousSignatureHash = previousHash;
    input.staticVertexCount = static_cast<int>(vertices.size());
    input.staticIndexCount = static_cast<int>(indexes.size());
    input.dynamicVertexCount = 6;
    input.dynamicIndexCount = 6;
    return BuildSmokeAccelerationPlan(input);
}

RtSmokeAccelerationPlanInput BuildPlanInput(
    const std::vector<HarnessSmokeVertex>& vertices,
    const std::vector<uint32_t>& indexes,
    const std::vector<uint32_t>& classes,
    const std::vector<uint32_t>& materials)
{
    RtSmokeAccelerationPlanInput input;
    input.staticSignature = BuildSignatureDesc(vertices, indexes, classes, materials);
    input.staticCache.staticCacheChanged = true;
    input.staticVertexCount = static_cast<int>(vertices.size());
    input.staticIndexCount = static_cast<int>(indexes.size());
    input.dynamicVertexCount = 6;
    input.dynamicIndexCount = 6;
    return input;
}

void TestStaticSignature()
{
    std::vector<HarnessSmokeVertex> vertices = BuildTriangleVertices(0.25f);
    std::vector<uint32_t> indexes = { 0, 1, 2 };
    std::vector<uint32_t> classes = { 7 };
    std::vector<uint32_t> materials = { 11 };

    const RtSmokePlanStaticBlasSignature a =
        ComputeSmokeStaticBlasSignaturePlan(BuildSignatureDesc(vertices, indexes, classes, materials));
    const RtSmokePlanStaticBlasSignature b =
        ComputeSmokeStaticBlasSignaturePlan(BuildSignatureDesc(vertices, indexes, classes, materials));
    Check(a.hash == b.hash, "identical static inputs produce identical BLAS signatures");

    vertices[1].position[0] = 2.0f;
    const RtSmokePlanStaticBlasSignature vertexChanged =
        ComputeSmokeStaticBlasSignaturePlan(BuildSignatureDesc(vertices, indexes, classes, materials));
    Check(a.hash != vertexChanged.hash, "vertex changes alter the BLAS signature");

    vertices = BuildTriangleVertices(0.25f);
    indexes[2] = 1;
    const RtSmokePlanStaticBlasSignature indexChanged =
        ComputeSmokeStaticBlasSignaturePlan(BuildSignatureDesc(vertices, indexes, classes, materials));
    Check(a.hash != indexChanged.hash, "index changes alter the BLAS signature");

    indexes = { 0, 1, 2 };
    classes[0] = 9;
    const RtSmokePlanStaticBlasSignature classChanged =
        ComputeSmokeStaticBlasSignaturePlan(BuildSignatureDesc(vertices, indexes, classes, materials));
    Check(a.hash == classChanged.hash, "class changes do not alter the geometry BLAS signature");

    classes[0] = 7;
    materials[0] = 12;
    const RtSmokePlanStaticBlasSignature materialChanged =
        ComputeSmokeStaticBlasSignaturePlan(BuildSignatureDesc(vertices, indexes, classes, materials));
    Check(a.hash == materialChanged.hash, "material changes do not alter the geometry BLAS signature");
}

void TestStaticSignatureRanges()
{
    std::vector<HarnessSmokeVertex> vertices(6);
    for (int vertexIndex = 0; vertexIndex < static_cast<int>(vertices.size()); ++vertexIndex)
    {
        vertices[vertexIndex].position[0] = static_cast<float>(vertexIndex);
        vertices[vertexIndex].position[1] = static_cast<float>(vertexIndex * 2);
        vertices[vertexIndex].position[2] = static_cast<float>(vertexIndex * 3);
        vertices[vertexIndex].position[3] = 1.0f;
        vertices[vertexIndex].normal[2] = 1.0f;
    }
    std::vector<uint32_t> indexes = { 0, 1, 2, 3, 4, 5 };
    std::vector<uint32_t> classes = { 7, 8 };
    std::vector<uint32_t> materials = { 11, 12 };

    RtSmokePlanStaticBlasSignatureDesc rangedDesc =
        BuildSignatureDesc(vertices, indexes, classes, materials);
    rangedDesc.staticRange.vertexOffset = 3;
    rangedDesc.staticRange.vertexCount = 3;
    rangedDesc.staticRange.indexOffset = 3;
    rangedDesc.staticRange.indexCount = 3;
    rangedDesc.staticRange.triangleOffset = 1;
    rangedDesc.staticRange.triangleCount = 1;
    const RtSmokePlanStaticBlasSignature rangedSignature =
        ComputeSmokeStaticBlasSignaturePlan(rangedDesc);

    std::vector<HarnessSmokeVertex> outsideChangedVertices = vertices;
    outsideChangedVertices[0].position[0] = 99.0f;
    RtSmokePlanStaticBlasSignatureDesc outsideChangedDesc =
        BuildSignatureDesc(outsideChangedVertices, indexes, classes, materials);
    outsideChangedDesc.staticRange = rangedDesc.staticRange;
    const RtSmokePlanStaticBlasSignature outsideChangedSignature =
        ComputeSmokeStaticBlasSignaturePlan(outsideChangedDesc);
    Check(rangedSignature.hash == outsideChangedSignature.hash, "static BLAS ranged signature ignores vertices outside range");

    std::vector<HarnessSmokeVertex> insideChangedVertices = vertices;
    insideChangedVertices[4].position[0] = 99.0f;
    RtSmokePlanStaticBlasSignatureDesc insideChangedDesc =
        BuildSignatureDesc(insideChangedVertices, indexes, classes, materials);
    insideChangedDesc.staticRange = rangedDesc.staticRange;
    const RtSmokePlanStaticBlasSignature insideChangedSignature =
        ComputeSmokeStaticBlasSignaturePlan(insideChangedDesc);
    Check(rangedSignature.hash != insideChangedSignature.hash, "static BLAS ranged signature includes vertices inside range");

    std::vector<uint32_t> outsideChangedMaterials = materials;
    outsideChangedMaterials[0] = 99;
    RtSmokePlanStaticBlasSignatureDesc outsideMaterialChangedDesc =
        BuildSignatureDesc(vertices, indexes, classes, outsideChangedMaterials);
    outsideMaterialChangedDesc.staticRange = rangedDesc.staticRange;
    const RtSmokePlanStaticBlasSignature outsideMaterialChangedSignature =
        ComputeSmokeStaticBlasSignaturePlan(outsideMaterialChangedDesc);
    Check(rangedSignature.hash == outsideMaterialChangedSignature.hash, "static BLAS ranged signature ignores triangle metadata outside range");

    rangedDesc.sceneOrigin.x = 1.0f;
    const RtSmokePlanStaticBlasSignature originChangedSignature =
        ComputeSmokeStaticBlasSignaturePlan(rangedDesc);
    Check(rangedSignature.hash != originChangedSignature.hash, "static BLAS signature includes scene origin");
}

void TestCacheAndBaseTlas()
{
    const RtSmokeAccelerationPlan coldPlan = BuildPlan(0, false, false, false);
    const RtSmokeAccelerationPlan hitPlan = BuildPlan(coldPlan.staticSignature.hash, true, true, false);
    const RtSmokeAccelerationPlan changedPlan = BuildPlan(coldPlan.staticSignature.hash, true, true, true);
    Check(!coldPlan.staticCacheHit, "static cache miss when no previous cache exists");
    Check(hitPlan.staticCacheHit, "static cache hit matches unchanged signature and ready resources");
    Check(!changedPlan.staticCacheHit, "static cache miss when static cache changed");

    const RtSmokeBaseTlasPlan tlasPlan = BuildSmokeBaseTlasPlan(true, true);
    Check(tlasPlan.instanceCount == 2, "TLAS plan includes base static and dynamic instances");
    Check(tlasPlan.instances[0].instanceId == 0 && tlasPlan.instances[1].instanceId == 1, "TLAS base instance IDs are deterministic");
    Check(tlasPlan.instances[0].kind == RT_SMOKE_PLAN_TLAS_STATIC_BLAS &&
        tlasPlan.instances[0].instanceMask == 0x01 &&
        tlasPlan.instances[0].transform[0] == 1.0f &&
        tlasPlan.instances[0].transform[5] == 1.0f &&
        tlasPlan.instances[0].transform[10] == 1.0f &&
        tlasPlan.instances[0].transform[15] == 1.0f,
        "TLAS plan emits deterministic static instance metadata");
    Check(tlasPlan.instances[1].kind == RT_SMOKE_PLAN_TLAS_DYNAMIC_BLAS &&
        tlasPlan.instances[1].instanceMask == 0x01 &&
        tlasPlan.instances[1].transform[0] == 1.0f &&
        tlasPlan.instances[1].transform[5] == 1.0f &&
        tlasPlan.instances[1].transform[10] == 1.0f &&
        tlasPlan.instances[1].transform[15] == 1.0f,
        "TLAS plan emits deterministic dynamic instance metadata");

    const RtSmokeBaseTlasPlan dynamicOnlyTlasPlan = BuildSmokeBaseTlasPlan(false, true);
    Check(dynamicOnlyTlasPlan.instanceCount == 1 && dynamicOnlyTlasPlan.instances[0].instanceId == 1, "TLAS plan supports dynamic-only scenes");

    RtSmokeAccelerationSubmitPlanInput submitPlanInput;
    submitPlanInput.hasStaticBlas = true;
    submitPlanInput.hasDynamicBlas = true;
    const RtSmokeAccelerationSubmitPlan fullSubmitPlan = BuildSmokeAccelerationSubmitPlan(submitPlanInput);
    Check(fullSubmitPlan.buildStaticBlas && fullSubmitPlan.buildDynamicBlas && fullSubmitPlan.submitTlas, "acceleration submit plan builds uncached static and dynamic BLAS");
    Check(fullSubmitPlan.baseTlasPlan.instanceCount == 2, "acceleration submit plan carries base TLAS instances");

    submitPlanInput.staticBlasCacheHit = true;
    const RtSmokeAccelerationSubmitPlan cachedSubmitPlan = BuildSmokeAccelerationSubmitPlan(submitPlanInput);
    Check(!cachedSubmitPlan.buildStaticBlas && cachedSubmitPlan.buildDynamicBlas && cachedSubmitPlan.submitTlas, "acceleration submit plan skips cached static BLAS build");

    submitPlanInput.hasDynamicBlas = false;
    submitPlanInput.includeStaticBlasInTlas = false;
    submitPlanInput.hasExtraTlasInstances = true;
    const RtSmokeAccelerationSubmitPlan bucketStaticSubmitPlan =
        BuildSmokeAccelerationSubmitPlan(submitPlanInput);
    Check(
        !bucketStaticSubmitPlan.buildStaticBlas &&
            !bucketStaticSubmitPlan.buildDynamicBlas &&
            bucketStaticSubmitPlan.submitTlas,
        "acceleration submit plan retains cached static BLAS while submitting bucket-only TLAS");
    Check(
        bucketStaticSubmitPlan.baseTlasPlan.instanceCount == 0,
        "acceleration submit plan excludes monolithic static instance independently of BLAS ownership");

    submitPlanInput.hasStaticBlas = false;
    submitPlanInput.hasDynamicBlas = false;
    submitPlanInput.hasExtraTlasInstances = false;
    const RtSmokeAccelerationSubmitPlan emptySubmitPlan = BuildSmokeAccelerationSubmitPlan(submitPlanInput);
    Check(!emptySubmitPlan.buildStaticBlas && !emptySubmitPlan.buildDynamicBlas && !emptySubmitPlan.submitTlas, "acceleration submit plan rejects empty BLAS set");

    RtSmokeAccelerationPlanInput emptyInput;
    const RtSmokeAccelerationPlan emptyPlan = BuildSmokeAccelerationPlan(emptyInput);
    Check(!emptyPlan.hasStaticBlas && !emptyPlan.hasDynamicBlas, "acceleration plan handles empty BLAS ranges");

    RtSmokeAccelerationPlanInput reusedInput;
    reusedInput.staticSignature.staticRange.vertexCount = 3;
    reusedInput.staticSignature.staticRange.indexCount = 3;
    reusedInput.staticSignature.staticRange.triangleCount = 1;
    reusedInput.staticCache.cacheValid = true;
    reusedInput.staticCache.cacheResourcesReady = true;
    reusedInput.staticCache.previousSignatureHash = 0x12345678ull;
    reusedInput.staticVertexCount = 3;
    reusedInput.staticIndexCount = 3;
    const RtSmokeAccelerationPlan reusedPlan = BuildSmokeAccelerationPlan(reusedInput);
    Check(reusedPlan.staticSignatureReused && reusedPlan.staticSignature.hash == 0x12345678ull, "static cache reuse avoids source-array signature work");
    Check(reusedPlan.staticCacheHit, "static cache reuse can hit with previous signature and ready resources");
    Check(reusedPlan.staticBlas.enabled &&
        reusedPlan.staticBlas.cacheHit &&
        reusedPlan.staticBlas.vertexCount == reusedInput.staticVertexCount &&
        reusedPlan.staticBlas.indexCount == reusedInput.staticIndexCount &&
        std::strcmp(reusedPlan.staticBlas.debugName, "PathTraceSmokeStaticWorldBLAS") == 0 &&
        !reusedPlan.dynamicBlas.enabled,
        "acceleration plan emits cached static BLAS create metadata");

    RtSmokeAccelerationPlanInput dynamicCreateInput;
    dynamicCreateInput.dynamicVertexCount = 6;
    dynamicCreateInput.dynamicIndexCount = 6;
    const RtSmokeAccelerationPlan dynamicCreatePlan =
        BuildSmokeAccelerationPlan(dynamicCreateInput);
    Check(!dynamicCreatePlan.staticBlas.enabled &&
        dynamicCreatePlan.dynamicBlas.enabled &&
        !dynamicCreatePlan.dynamicBlas.cacheHit &&
        dynamicCreatePlan.dynamicBlas.vertexCount == dynamicCreateInput.dynamicVertexCount &&
        dynamicCreatePlan.dynamicBlas.indexCount == dynamicCreateInput.dynamicIndexCount &&
        std::strcmp(dynamicCreatePlan.dynamicBlas.debugName, "PathTraceSmokeDynamicCandidateBLAS") == 0,
        "acceleration plan emits dynamic BLAS create metadata");
}

void TestOwnedSnapshot()
{
    std::vector<HarnessSmokeVertex> vertices = BuildTriangleVertices(0.25f);
    std::vector<uint32_t> indexes = { 0, 1, 2 };
    std::vector<uint32_t> classes = { 7 };
    std::vector<uint32_t> materials = { 11 };

    RtSmokeAccelerationPlanInput input = BuildPlanInput(vertices, indexes, classes, materials);
    const RtSmokeAccelerationPlan directPlan = BuildSmokeAccelerationPlan(input);
    const RtSmokeAccelerationPlanSnapshot snapshot = CaptureSmokeAccelerationPlanSnapshot(input);

    vertices[1].position[0] = 99.0f;
    indexes[2] = 0;
    classes[0] = 99;
    materials[0] = 99;

    const RtSmokeAccelerationPlanResult snapshotResult = BuildSmokeAccelerationPlanResult(snapshot);
    Check(snapshotResult.valid, "owned acceleration snapshot produces a valid result");
    Check(snapshotResult.plan.staticSignature.hash == directPlan.staticSignature.hash, "owned acceleration snapshot is immutable after source mutation");
    Check(snapshotResult.plan.staticBlas.vertexCount == 3 && snapshotResult.plan.dynamicBlas.indexCount == 6, "owned acceleration snapshot preserves BLAS plan counts");

    RtSmokeAccelerationPlanInput reusedInput = input;
    reusedInput.staticCache.cacheValid = true;
    reusedInput.staticCache.cacheResourcesReady = true;
    reusedInput.staticCache.staticCacheChanged = false;
    reusedInput.staticCache.previousSignatureHash = directPlan.staticSignature.hash;
    const RtSmokeAccelerationPlanSnapshot reusedSnapshot =
        CaptureSmokeAccelerationPlanSnapshot(reusedInput);
    const RtSmokeAccelerationPlanResult reusedSnapshotResult =
        BuildSmokeAccelerationPlanResult(reusedSnapshot);
    Check(reusedSnapshot.staticSignature.vertexBytes.empty() &&
        reusedSnapshot.staticSignature.indexes.empty() &&
        reusedSnapshot.staticSignature.triangleClasses.empty() &&
        reusedSnapshot.staticSignature.triangleMaterials.empty() &&
        reusedSnapshotResult.plan.staticSignatureReused &&
        reusedSnapshotResult.plan.staticSignature.hash == directPlan.staticSignature.hash,
        "owned acceleration snapshot omits source arrays when static signature is reused");

    std::vector<HarnessSmokeVertex> rangeVertices = BuildTriangleVertices(0.25f);
    rangeVertices.insert(rangeVertices.begin(), rangeVertices[0]);
    rangeVertices.push_back(rangeVertices[0]);
    std::vector<uint32_t> rangeIndexes = { 99, 1, 2, 3, 88 };
    std::vector<uint32_t> rangeClasses = { 99, 7, 88 };
    std::vector<uint32_t> rangeMaterials = { 99, 11, 88 };
    RtSmokeAccelerationPlanInput rangedInput =
        BuildPlanInput(rangeVertices, rangeIndexes, rangeClasses, rangeMaterials);
    rangedInput.staticSignature.staticRange.vertexOffset = 1;
    rangedInput.staticSignature.staticRange.vertexCount = 3;
    rangedInput.staticSignature.staticRange.indexOffset = 1;
    rangedInput.staticSignature.staticRange.indexCount = 3;
    rangedInput.staticSignature.staticRange.triangleOffset = 1;
    rangedInput.staticSignature.staticRange.triangleCount = 1;
    const RtSmokeAccelerationPlan rangedDirectPlan = BuildSmokeAccelerationPlan(rangedInput);
    const RtSmokeAccelerationPlanSnapshot rangedSnapshot =
        CaptureSmokeAccelerationPlanSnapshot(rangedInput);
    const RtSmokeAccelerationPlanResult rangedSnapshotResult =
        BuildSmokeAccelerationPlanResult(rangedSnapshot);
    Check(rangedSnapshot.staticSignature.vertexBytes.size() ==
            static_cast<size_t>(rangedInput.staticSignature.staticRange.vertexCount) * sizeof(HarnessSmokeVertex) &&
        rangedSnapshot.staticSignature.indexes.size() ==
            static_cast<size_t>(rangedInput.staticSignature.staticRange.indexCount) &&
        rangedSnapshot.staticSignature.triangleClasses.empty() &&
        rangedSnapshot.staticSignature.triangleMaterials.empty() &&
        rangedSnapshot.staticSignature.staticRange.vertexOffset == 0 &&
        rangedSnapshot.staticSignature.staticRange.indexOffset == 0 &&
        rangedSnapshotResult.plan.staticSignature.hash == rangedDirectPlan.staticSignature.hash,
        "owned acceleration snapshot copies only ranged static geometry signature inputs with normalized offsets");
}

void TestAccelerationPlanInputToken()
{
    std::vector<HarnessSmokeVertex> vertices = BuildTriangleVertices(0.25f);
    std::vector<uint32_t> indexes = { 0, 1, 2 };
    std::vector<uint32_t> classes = { 7 };
    std::vector<uint32_t> materials = { 11 };

    RtSmokeAccelerationPlanInput input = BuildPlanInput(vertices, indexes, classes, materials);
    input.staticCache.cacheValid = true;
    input.staticCache.cacheResourcesReady = true;
    input.staticCache.staticCacheChanged = false;
    input.staticCache.previousSignatureHash = 0x1234ull;
    const uint64_t baseToken = BuildSmokeAccelerationPlanInputToken(input);
    Check(baseToken == BuildSmokeAccelerationPlanInputToken(input), "acceleration plan input token is deterministic");

    RtSmokeAccelerationPlanInput cacheChangedInput = input;
    cacheChangedInput.staticCache.cacheResourcesReady = false;
    Check(baseToken != BuildSmokeAccelerationPlanInputToken(cacheChangedInput), "acceleration plan input token tracks cache readiness");

    RtSmokeAccelerationPlanInput previousHashChangedInput = input;
    previousHashChangedInput.staticCache.previousSignatureHash = 0x5678ull;
    Check(baseToken != BuildSmokeAccelerationPlanInputToken(previousHashChangedInput), "acceleration plan input token tracks reusable previous cache signatures");

    RtSmokeAccelerationPlanInput invalidCacheInput = input;
    invalidCacheInput.staticCache.cacheValid = false;
    invalidCacheInput.staticCache.cacheResourcesReady = false;
    invalidCacheInput.staticCache.previousSignatureHash = 0x1111ull;
    const uint64_t invalidCacheToken = BuildSmokeAccelerationPlanInputToken(invalidCacheInput);
    invalidCacheInput.staticCache.hasStaticBlas = !invalidCacheInput.staticCache.hasStaticBlas;
    invalidCacheInput.staticCache.previousSignatureHash = 0x2222ull;
    Check(invalidCacheToken == BuildSmokeAccelerationPlanInputToken(invalidCacheInput),
        "acceleration plan input token ignores unusable invalid static cache state");

    RtSmokeAccelerationPlanInput changedCacheInput = input;
    changedCacheInput.staticCache.staticCacheChanged = true;
    changedCacheInput.staticCache.cacheResourcesReady = false;
    changedCacheInput.staticCache.previousSignatureHash = 0x3333ull;
    const uint64_t changedCacheToken = BuildSmokeAccelerationPlanInputToken(changedCacheInput);
    changedCacheInput.staticCache.previousSignatureHash = 0x4444ull;
    changedCacheInput.staticCache.cacheResourcesReady = true;
    Check(changedCacheToken == BuildSmokeAccelerationPlanInputToken(changedCacheInput),
        "acceleration plan input token ignores previous signatures when static cache changed");

    RtSmokeAccelerationPlanInput rangeChangedInput = input;
    rangeChangedInput.staticSignature.staticRange.indexCount = 0;
    Check(baseToken != BuildSmokeAccelerationPlanInputToken(rangeChangedInput), "acceleration plan input token tracks BLAS ranges");

    RtSmokeAccelerationPlanInput dynamicChangedInput = input;
    dynamicChangedInput.dynamicIndexCount = 0;
    Check(baseToken != BuildSmokeAccelerationPlanInputToken(dynamicChangedInput), "acceleration plan input token tracks dynamic BLAS counts");

    std::vector<HarnessSmokeVertex> vertexChangedVertices = vertices;
    vertexChangedVertices[0].position[0] += 1.0f;
    RtSmokeAccelerationPlanInput vertexChangedInput =
        BuildPlanInput(vertexChangedVertices, indexes, classes, materials);
    vertexChangedInput.staticCache = input.staticCache;
    Check(baseToken == BuildSmokeAccelerationPlanInputToken(vertexChangedInput),
        "acceleration plan input token ignores static vertex content while reusable cache supplies signature");

    std::vector<uint32_t> indexChangedIndexes = indexes;
    indexChangedIndexes[0] = 2;
    RtSmokeAccelerationPlanInput indexChangedInput =
        BuildPlanInput(vertices, indexChangedIndexes, classes, materials);
    indexChangedInput.staticCache = input.staticCache;
    Check(baseToken == BuildSmokeAccelerationPlanInputToken(indexChangedInput),
        "acceleration plan input token ignores static index content while reusable cache supplies signature");

    std::vector<uint32_t> materialChangedMaterials = materials;
    materialChangedMaterials[0] = 12;
    RtSmokeAccelerationPlanInput materialChangedInput =
        BuildPlanInput(vertices, indexes, classes, materialChangedMaterials);
    materialChangedInput.staticCache = input.staticCache;
    Check(baseToken == BuildSmokeAccelerationPlanInputToken(materialChangedInput),
        "acceleration plan input token ignores static material content while reusable cache supplies signature");

    RtSmokeAccelerationPlanInput sourceTrackedInput = input;
    sourceTrackedInput.staticCache.previousSignatureHash = 0;
    const uint64_t sourceTrackedToken = BuildSmokeAccelerationPlanInputToken(sourceTrackedInput);
    vertexChangedInput.staticCache = sourceTrackedInput.staticCache;
    Check(sourceTrackedToken != BuildSmokeAccelerationPlanInputToken(vertexChangedInput),
        "acceleration plan input token tracks static vertex content when signature is recomputed");
    indexChangedInput.staticCache = sourceTrackedInput.staticCache;
    Check(sourceTrackedToken != BuildSmokeAccelerationPlanInputToken(indexChangedInput),
        "acceleration plan input token tracks static index content when signature is recomputed");
    materialChangedInput.staticCache = sourceTrackedInput.staticCache;
    Check(sourceTrackedToken == BuildSmokeAccelerationPlanInputToken(materialChangedInput),
        "acceleration plan input token ignores static material content when signature is recomputed");
}

void TestAsyncSnapshotPlanning()
{
    std::vector<HarnessSmokeVertex> vertices = BuildTriangleVertices(0.25f);
    std::vector<uint32_t> indexes = { 0, 1, 2 };
    std::vector<uint32_t> classes = { 7 };
    std::vector<uint32_t> materials = { 11 };

    RtSmokeAccelerationPlanInput input = BuildPlanInput(vertices, indexes, classes, materials);
    const RtSmokeAccelerationPlan directPlan = BuildSmokeAccelerationPlan(input);
    const RtSmokeAccelerationPlanSnapshot snapshot = CaptureSmokeAccelerationPlanSnapshot(input);

    RtPathTraceCpuWorkState state;
    RtPathTraceCpuWorkGeneration generation;
    generation.sceneGeneration = 1;
    generation.geometryGeneration = 2;
    generation.materialGeneration = 3;
    RtPathTraceCpuWorkPublishSnapshot(state, generation);

    std::future<RtSmokeAccelerationPlanTimedResult> future = std::async(
        std::launch::async,
        [snapshot]() {
            return BuildSmokeAccelerationPlanTimedResult(snapshot);
        });

    vertices[2].position[0] = 42.0f;
    const RtPathTraceCpuWorkFrameDecision lateDecision =
        RtPathTraceCpuWorkAcceptLatest(state, generation, nullptr, true);
    Check(lateDecision.lateFallback && lateDecision.syncFallback, "late async acceleration plan requests synchronous fallback");

    RtPathTraceCpuWorkResultEnvelope envelope;
    const RtSmokeAccelerationPlanTimedResult timedResult = future.get();
    const RtSmokeAccelerationPlanResult& asyncResult = timedResult.result;
    envelope.completed = asyncResult.valid;
    envelope.generation = generation;
    envelope.timing.workerExecutionMs = timedResult.workerExecutionMs;
    RtPathTraceCpuWorkPublishCompletedResult(state, envelope);
    const RtPathTraceCpuWorkFrameDecision acceptDecision =
        RtPathTraceCpuWorkAcceptLatest(state, generation, nullptr, true);
    Check(acceptDecision.accepted, "matching async acceleration plan generation is accepted");
    Check(asyncResult.plan.staticSignature.hash == directPlan.staticSignature.hash, "async acceleration plan uses owned immutable snapshot data");
    Check(timedResult.workerExecutionMs >= 0.0, "timed acceleration worker result records execution time");

    RtPathTraceCpuWorkGeneration staleGeneration = generation;
    staleGeneration.geometryGeneration = 99;
    RtPathTraceCpuWorkResultEnvelope staleEnvelope = envelope;
    staleEnvelope.generation = staleGeneration;
    const RtPathTraceCpuWorkFrameDecision staleDecision =
        RtPathTraceCpuWorkAcceptLatest(state, generation, &staleEnvelope, true);
    Check(staleDecision.staleRejected && staleDecision.syncFallback, "stale async acceleration plan generation is rejected");
}

void TestRigidPlan()
{
    RtSmokeRigidTlasObservation observations[4];
    observations[0].meshHash = 100;
    observations[0].instanceId = 10;
    observations[0].materialId = 123;
    observations[0].sourceFlags = 0x2;
    observations[0].hasMeshRecord = true;
    observations[0].meshSeenThisFrame = true;
    observations[0].hasBlas = true;
    observations[0].objectToWorld[0] = 1.0f;
    observations[0].objectToWorld[5] = 1.0f;
    observations[0].objectToWorld[10] = 1.0f;
    observations[0].objectToWorld[15] = 1.0f;
    observations[0].previousObjectToWorld[0] = 1.0f;
    observations[0].previousObjectToWorld[5] = 1.0f;
    observations[0].previousObjectToWorld[10] = 1.0f;
    observations[0].previousObjectToWorld[15] = 1.0f;
    observations[0].previousObjectToWorld[12] = -2.0f;
    observations[0].hasPreviousObjectToWorld = true;
    observations[0].transformContinuous = true;
    observations[0].seenThisFrame = false;
    observations[0].canonicalBlasRecordIndex = 7;
    observations[0].canonicalMeshHash = 101;

    observations[1] = observations[0];
    observations[1].meshHash = 200;
    observations[1].hasMeshRecord = false;

    observations[2] = observations[0];
    observations[2].meshHash = 300;
    observations[2].meshSeenThisFrame = false;
    observations[2].residencyEnabled = false;

    observations[3] = observations[0];
    observations[3].meshHash = 400;
    observations[3].hasBlas = false;

    RtSmokeRigidTlasPlanDesc desc;
    desc.observations = observations;
    desc.observationCount = 4;
    desc.rigidSourceMask = 0x2;
    desc.firstInstanceId = 2;
    desc.instanceMask = 0x02;

    const RtSmokeRigidTlasPlan plan = BuildSmokeRigidTlasPlan(desc);
    Check(plan.emittedInstances == 1, "rigid TLAS plan accepts valid observations");
    Check(plan.rejectedMissingMesh == 1, "rigid TLAS plan rejects missing mesh records");
    Check(plan.rejectedStaleMesh == 1, "rigid TLAS plan rejects stale observations");
    Check(plan.rejectedMissingBlas == 1, "rigid TLAS plan rejects missing BLAS handles before render submit");
    Check(plan.tlasInstanceSignature != 0, "rigid TLAS plan emits deterministic instance signature");
    Check(plan.instances[0].instanceId == 2 &&
        plan.instances[0].meshHash == 100 &&
        plan.instances[0].materialId == 123 &&
        plan.instances[0].hasPreviousTransform &&
        plan.instances[0].transformContinuous &&
        !plan.instances[0].sourceSeenThisFrame &&
        plan.instances[0].canonicalBlasRecordIndex == 7 &&
        plan.instances[0].canonicalMeshHash == 101 &&
        plan.instances[0].previousTransform[12] == -2.0f,
        "rigid TLAS emitted instance metadata is deterministic");

    RtSmokeCanonicalRigidTlasSelectionInput canonicalSelectionInput;
    canonicalSelectionInput.providerEnabled = true;
    canonicalSelectionInput.legacyDescriptors = 1;
    canonicalSelectionInput.canonicalDescriptors = 1;
    canonicalSelectionInput.exactRecordMappings = 1;
    RtSmokeCanonicalRigidTlasSelection canonicalSelection =
        BuildSmokeCanonicalRigidTlasSelection(canonicalSelectionInput);
    Check(canonicalSelection.exactParity &&
        !canonicalSelection.selectCanonical,
        "canonical rigid TLAS selection remains shadow-only by default");
    canonicalSelectionInput.traversalRequested = true;
    canonicalSelection =
        BuildSmokeCanonicalRigidTlasSelection(canonicalSelectionInput);
    Check(canonicalSelection.exactParity &&
        canonicalSelection.selectCanonical,
        "canonical rigid TLAS selection accepts exact descriptor parity");
    canonicalSelectionInput.missingBlas = 1;
    canonicalSelection =
        BuildSmokeCanonicalRigidTlasSelection(canonicalSelectionInput);
    Check(!canonicalSelection.exactParity &&
        !canonicalSelection.selectCanonical,
        "canonical rigid TLAS selection fails closed on incomplete BLAS state");
    canonicalSelectionInput.missingBlas = 0;
    canonicalSelectionInput.canonicalDescriptors = 0;
    canonicalSelection =
        BuildSmokeCanonicalRigidTlasSelection(canonicalSelectionInput);
    Check(!canonicalSelection.exactParity &&
        !canonicalSelection.selectCanonical,
        "canonical rigid TLAS selection fails closed on descriptor mismatch");

    const uint64_t baseRigidToken = BuildSmokeRigidTlasPlanInputToken(desc);
    Check(baseRigidToken == BuildSmokeRigidTlasPlanInputToken(desc),
        "rigid TLAS plan input token is deterministic");
    RtSmokeRigidTlasPlanDesc changedTransformDesc = desc;
    observations[0].previousObjectToWorld[12] = -4.0f;
    Check(baseRigidToken != BuildSmokeRigidTlasPlanInputToken(changedTransformDesc),
        "rigid TLAS plan input token tracks route transform metadata");
    const RtSmokeRigidTlasPlan changedTransformPlan = BuildSmokeRigidTlasPlan(changedTransformDesc);
    Check(plan.tlasInstanceSignature != changedTransformPlan.tlasInstanceSignature,
        "rigid TLAS plan signature tracks emitted transform metadata");
    observations[0].previousObjectToWorld[12] = -2.0f;

    observations[0].materialId = 124;
    Check(baseRigidToken != BuildSmokeRigidTlasPlanInputToken(desc),
        "rigid TLAS plan input token tracks per-instance material identity");
    const RtSmokeRigidTlasPlan changedMaterialPlan = BuildSmokeRigidTlasPlan(desc);
    Check(plan.tlasInstanceSignature != changedMaterialPlan.tlasInstanceSignature &&
        changedMaterialPlan.instances[0].materialId == 124,
        "rigid TLAS plan preserves per-instance material identity");
    observations[0].materialId = 123;

    const uint64_t baseCanonicalRouteToken =
        BuildSmokeRigidTlasPlanInputToken(desc);
    observations[0].canonicalBlasRecordIndex = 8;
    Check(baseCanonicalRouteToken != BuildSmokeRigidTlasPlanInputToken(desc),
        "rigid TLAS plan input token tracks canonical BLAS record handles");
    observations[0].canonicalBlasRecordIndex = 7;
    observations[0].canonicalMeshHash = 102;
    Check(baseCanonicalRouteToken != BuildSmokeRigidTlasPlanInputToken(desc),
        "rigid TLAS plan input token tracks canonical mesh identity");
    observations[0].canonicalMeshHash = 101;

    RtSmokeRigidTlasObservation invalidPreviousObservation = observations[0];
    invalidPreviousObservation.hasPreviousObjectToWorld = false;
    invalidPreviousObservation.transformContinuous = true;
    RtSmokeRigidTlasPlanDesc invalidPreviousDesc = desc;
    invalidPreviousDesc.observations = &invalidPreviousObservation;
    invalidPreviousDesc.observationCount = 1;
    const uint64_t invalidPreviousToken = BuildSmokeRigidTlasPlanInputToken(invalidPreviousDesc);
    invalidPreviousObservation.previousObjectToWorld[12] = -99.0f;
    invalidPreviousObservation.transformContinuous = false;
    Check(invalidPreviousToken == BuildSmokeRigidTlasPlanInputToken(invalidPreviousDesc),
        "rigid TLAS plan input token ignores invalid previous transform contents");
    const RtSmokeRigidTlasPlan invalidPreviousPlan = BuildSmokeRigidTlasPlan(invalidPreviousDesc);
    Check(invalidPreviousPlan.emittedInstances == 1 &&
        !invalidPreviousPlan.instances[0].hasPreviousTransform &&
        !invalidPreviousPlan.instances[0].transformContinuous &&
        invalidPreviousPlan.instances[0].previousTransform[12] ==
            invalidPreviousPlan.instances[0].transform[12],
        "rigid TLAS plan uses current transform when previous transform is invalid");

    const uint64_t rejectedTransformToken = BuildSmokeRigidTlasPlanInputToken(desc);
    observations[1].objectToWorld[12] = 55.0f;
    observations[1].previousObjectToWorld[12] = -55.0f;
    Check(rejectedTransformToken == BuildSmokeRigidTlasPlanInputToken(desc),
        "rigid TLAS plan input token ignores rejected transform contents");

    const uint64_t rejectedIdentityToken = BuildSmokeRigidTlasPlanInputToken(desc);
    observations[1].meshHash = 888;
    observations[1].instanceId = 889;
    observations[1].routeRecordIndex = 890;
    observations[1].hasBlas = !observations[1].hasBlas;
    observations[1].meshSeenThisFrame = !observations[1].meshSeenThisFrame;
    observations[1].seenThisFrame = !observations[1].seenThisFrame;
    Check(rejectedIdentityToken == BuildSmokeRigidTlasPlanInputToken(desc),
        "rigid TLAS plan input token ignores rejected identity fields");

    observations[1].sourceFlags = 0;
    Check(rejectedIdentityToken != BuildSmokeRigidTlasPlanInputToken(desc),
        "rigid TLAS plan input token tracks rejected category changes");
    observations[1] = observations[0];
    observations[1].meshHash = 200;
    observations[1].hasMeshRecord = false;

    const RtSmokeRigidTlasPlanSnapshot snapshot = CaptureSmokeRigidTlasPlanSnapshot(desc);
    const uint64_t snapshotRigidToken = BuildSmokeRigidTlasPlanInputToken(snapshot);
    observations[0].hasBlas = false;
    observations[0].meshHash = 999;
    const RtSmokeRigidTlasPlan snapshotPlan = BuildSmokeRigidTlasPlan(snapshot);
    Check(snapshotPlan.emittedInstances == 1 &&
        snapshotPlan.instances[0].meshHash == 100 &&
        snapshotRigidToken == baseRigidToken &&
        BuildSmokeRigidTlasPlanInputToken(desc) != snapshotRigidToken,
        "owned rigid TLAS snapshot is immutable after source mutation");

    RtSmokeRigidTlasPlanDesc emptyObservationDesc = desc;
    emptyObservationDesc.observations = nullptr;
    emptyObservationDesc.observationCount = 4;
    const RtSmokeRigidTlasPlanSnapshot emptyObservationSnapshot =
        CaptureSmokeRigidTlasPlanSnapshot(emptyObservationDesc);
    const RtSmokeRigidTlasPlan emptyObservationPlan =
        BuildSmokeRigidTlasPlan(emptyObservationDesc);
    const RtSmokeRigidTlasPlan emptyObservationSnapshotPlan =
        BuildSmokeRigidTlasPlan(emptyObservationSnapshot);
    Check(emptyObservationSnapshot.observations.empty() &&
        BuildSmokeRigidTlasPlanInputToken(emptyObservationSnapshot) ==
            BuildSmokeRigidTlasPlanInputToken(emptyObservationDesc) &&
        emptyObservationPlan.tlasInstanceSignature == emptyObservationSnapshotPlan.tlasInstanceSignature,
        "owned rigid TLAS snapshot normalizes absent observations as empty");

    observations[0].hasBlas = true;
    observations[0].meshHash = 100;
    observations[2].residencyEnabled = true;
    observations[2].hasBlas = true;
    const RtSmokeRigidTlasPlan residentPlan = BuildSmokeRigidTlasPlan(desc);
    Check(residentPlan.emittedInstances == 2 &&
        residentPlan.tlasInstanceSignature != plan.tlasInstanceSignature,
        "rigid TLAS plan accepts stale mesh when residency keeps it valid");

    observations[2].hasBlas = false;
    const RtSmokeRigidTlasPlan missingResidentBlasPlan = BuildSmokeRigidTlasPlan(desc);
    Check(missingResidentBlasPlan.emittedInstances == 1 &&
        missingResidentBlasPlan.rejectedMissingBlas == plan.rejectedMissingBlas + 1 &&
        missingResidentBlasPlan.tlasInstanceSignature != residentPlan.tlasInstanceSignature,
        "rigid TLAS plan signature tracks BLAS readiness summary");

    desc.maxInstances = 0;
    const uint64_t uncappedToken = BuildSmokeRigidTlasPlanInputToken(desc);
    const RtSmokeRigidTlasPlan uncappedPlan = BuildSmokeRigidTlasPlan(desc);
    desc.maxInstances = -4;
    const RtSmokeRigidTlasPlan negativeCapPlan = BuildSmokeRigidTlasPlan(desc);
    Check(uncappedToken == BuildSmokeRigidTlasPlanInputToken(desc) &&
        uncappedPlan.tlasInstanceSignature == negativeCapPlan.tlasInstanceSignature &&
        uncappedPlan.emittedInstances == negativeCapPlan.emittedInstances,
        "rigid TLAS plan normalizes non-positive instance caps as uncapped");

    desc.maxInstances = 1;
    const RtSmokeRigidTlasPlan cappedPlan = BuildSmokeRigidTlasPlan(desc);
    Check(cappedPlan.emittedInstances == 1 &&
        cappedPlan.instances.size() == 1 &&
        cappedPlan.tlasInstanceSignature != missingResidentBlasPlan.tlasInstanceSignature,
        "rigid TLAS plan honors max instance cap");

    const uint64_t cappedToken = BuildSmokeRigidTlasPlanInputToken(desc);
    observations[1].meshHash = 777;
    observations[1].objectToWorld[12] = 99.0f;
    Check(cappedToken == BuildSmokeRigidTlasPlanInputToken(desc),
        "rigid TLAS plan input token ignores observations after max instance cap");
    const RtSmokeRigidTlasPlanSnapshot cappedSnapshot = CaptureSmokeRigidTlasPlanSnapshot(desc);
    const RtSmokeRigidTlasPlan cappedSnapshotPlan = BuildSmokeRigidTlasPlan(cappedSnapshot);
    Check(cappedSnapshot.observations.size() == 1 &&
        BuildSmokeRigidTlasPlanInputToken(cappedSnapshot) == cappedToken &&
        cappedSnapshotPlan.tlasInstanceSignature == cappedPlan.tlasInstanceSignature,
        "owned rigid TLAS snapshot drops observations after max instance cap");
    desc.observationCount = 2;
    Check(cappedToken == BuildSmokeRigidTlasPlanInputToken(desc),
        "rigid TLAS plan input token ignores appended observations after max instance cap");

    RtSmokeRigidTlasObservation nonRigid = observations[0];
    nonRigid.sourceFlags = 0;
    RtSmokeRigidTlasPlan streamingPlan;
    RtSmokeRigidTlasPlanDesc streamingDesc;
    streamingDesc.rigidSourceMask = 0x2;
    streamingDesc.firstInstanceId = 8;
    streamingDesc.instanceMask = 0x02;
    Check(AppendSmokeRigidTlasPlanObservation(streamingPlan, streamingDesc, nonRigid), "streaming rigid TLAS planner accepts non-rigid observation without stopping");
    Check(streamingPlan.rejectedNonRigid == 1 && streamingPlan.emittedInstances == 0, "streaming rigid TLAS planner counts non-rigid rejection");
    Check(AppendSmokeRigidTlasPlanObservation(streamingPlan, streamingDesc, observations[0]), "streaming rigid TLAS planner accepts valid observation");
    Check(streamingPlan.emittedInstances == 1 && streamingPlan.instances[0].instanceId == 8, "streaming rigid TLAS planner emits deterministic instance id");
}

void TestAsyncRigidTlasPlanning()
{
    RtSmokeRigidTlasObservation observation;
    observation.meshHash = 500;
    observation.instanceId = 42;
    observation.sourceFlags = 2;
    observation.hasMeshRecord = true;
    observation.meshSeenThisFrame = true;
    observation.hasBlas = true;
    observation.routeRecordIndex = 7;
    observation.seenThisFrame = true;
    for (int transformIndex = 0; transformIndex < 16; ++transformIndex)
    {
        observation.objectToWorld[transformIndex] = 0.0f;
        observation.previousObjectToWorld[transformIndex] = 0.0f;
    }
    observation.objectToWorld[0] = 1.0f;
    observation.objectToWorld[5] = 1.0f;
    observation.objectToWorld[10] = 1.0f;
    observation.objectToWorld[15] = 1.0f;
    observation.previousObjectToWorld[0] = 1.0f;
    observation.previousObjectToWorld[5] = 1.0f;
    observation.previousObjectToWorld[10] = 1.0f;
    observation.previousObjectToWorld[15] = 1.0f;

    RtSmokeRigidTlasPlanDesc desc;
    desc.observations = &observation;
    desc.observationCount = 1;
    desc.rigidSourceMask = 2;
    desc.firstInstanceId = 12;
    desc.instanceMask = 0x02;
    desc.maxInstances = 8;
    const RtSmokeRigidTlasPlanSnapshot snapshot =
        CaptureSmokeRigidTlasPlanSnapshot(desc);

    RtPathTraceCpuWorkState state;
    RtPathTraceCpuWorkGeneration generation;
    generation.frameIndex = 77;
    generation.sceneGeneration = 3;
    generation.geometryGeneration = 900;
    generation.materialGeneration = 901;
    generation.lightGeneration = BuildSmokeRigidTlasPlanInputToken(snapshot);
    RtPathTraceCpuWorkPublishSnapshot(state, generation);

    std::future<RtSmokeRigidTlasPlanTimedResult> future = std::async(
        std::launch::async,
        [snapshot]() {
            return BuildSmokeRigidTlasPlanTimedResult(snapshot);
        });

    const RtPathTraceCpuWorkFrameDecision lateDecision =
        RtPathTraceCpuWorkAcceptLatest(state, generation, nullptr, true);
    Check(lateDecision.lateFallback &&
        lateDecision.syncFallback &&
        state.lateResultCount == 1,
        "late async rigid TLAS work requests synchronous fallback");

    const RtSmokeRigidTlasPlanTimedResult timedResult = future.get();
    RtPathTraceCpuWorkResultEnvelope envelope;
    envelope.completed = true;
    envelope.generation = generation;
    envelope.timing.workerExecutionMs = static_cast<double>(timedResult.planningTimeMicros) / 1000.0;
    RtPathTraceCpuWorkPublishCompletedResult(state, envelope);
    const RtPathTraceCpuWorkFrameDecision acceptDecision =
        RtPathTraceCpuWorkAcceptLatest(state, generation, nullptr, true);
    Check(acceptDecision.accepted &&
        timedResult.plan.emittedInstances == 1 &&
        timedResult.plan.instances[0].instanceId == 12 &&
        state.acceptedResultCount == 1,
        "matching async rigid TLAS generation is accepted");

    RtPathTraceCpuWorkResultEnvelope staleEnvelope = envelope;
    staleEnvelope.generation.lightGeneration ^= 0x1000ull;
    const RtPathTraceCpuWorkFrameDecision staleDecision =
        RtPathTraceCpuWorkAcceptLatest(state, generation, &staleEnvelope, true);
    Check(staleDecision.staleRejected && staleDecision.syncFallback,
        "stale async rigid TLAS generation is rejected");
}

void TestRigidBlasBuildPlan()
{
    RtSmokeRigidBlasBuildPlanInput input;
    input.submitBuilds = true;
    input.hasBlas = true;
    input.blasInputsCompatible = true;
    const RtSmokeRigidBlasBuildPlan unchangedPlan = BuildSmokeRigidBlasBuildPlan(input);
    Check(!unchangedPlan.createBlas && !unchangedPlan.submitBuild && unchangedPlan.skipBuild, "unchanged rigid BLAS skips build submission");

    input.uploadRequired = true;
    const RtSmokeRigidBlasBuildPlan uploadChangedPlan = BuildSmokeRigidBlasBuildPlan(input);
    Check(!uploadChangedPlan.createBlas && uploadChangedPlan.submitBuild && !uploadChangedPlan.skipBuild, "changed rigid BLAS upload requests build submission");

    input.uploadRequired = false;
    input.forceRebuild = true;
    const RtSmokeRigidBlasBuildPlan forcedPlan = BuildSmokeRigidBlasBuildPlan(input);
    Check(!forcedPlan.createBlas && forcedPlan.submitBuild && !forcedPlan.skipBuild, "rigid BLAS force rebuild requests build submission");

    input.forceRebuild = false;
    input.blasInputsCompatible = false;
    const RtSmokeRigidBlasBuildPlan incompatiblePlan = BuildSmokeRigidBlasBuildPlan(input);
    Check(incompatiblePlan.createBlas && incompatiblePlan.submitBuild, "rigid BLAS descriptor incompatibility requests BLAS recreate and build");

    input.hasBlas = false;
    input.blasInputsCompatible = false;
    const RtSmokeRigidBlasBuildPlan missingPlan = BuildSmokeRigidBlasBuildPlan(input);
    Check(missingPlan.createBlas && missingPlan.submitBuild, "missing rigid BLAS requests create and build");

    input.submitBuilds = false;
    const RtSmokeRigidBlasBuildPlan gateOffPlan = BuildSmokeRigidBlasBuildPlan(input);
    Check(!gateOffPlan.createBlas && !gateOffPlan.submitBuild && gateOffPlan.skipBuild, "rigid BLAS build gate skips build submission");
}

void TestStaticBucketBlasBuildPlan()
{
    RtSmokeStaticBucketBlasBuildPlanInput input;
    input.submitBuilds = true;
    input.hasBlas = true;
    input.blasInputsCompatible = true;
    input.signatureValid = true;
    input.previousBlasInputSignature = 100;
    input.currentBlasInputSignature = 100;
    const RtSmokeStaticBucketBlasBuildPlan unchangedPlan =
        BuildSmokeStaticBucketBlasBuildPlan(input);
    Check(!unchangedPlan.createBlas &&
        !unchangedPlan.submitBuild &&
        unchangedPlan.skipBuild &&
        !unchangedPlan.signatureChanged,
        "static bucket BLAS build plan skips unchanged compatible bucket BLAS");

    input.currentBlasInputSignature = 101;
    const RtSmokeStaticBucketBlasBuildPlan changedSignaturePlan =
        BuildSmokeStaticBucketBlasBuildPlan(input);
    Check(!changedSignaturePlan.createBlas &&
        changedSignaturePlan.submitBuild &&
        changedSignaturePlan.signatureChanged,
        "static bucket BLAS build plan rebuilds changed bucket signature");

    input.currentBlasInputSignature = 100;
    input.uploadRequired = true;
    const RtSmokeStaticBucketBlasBuildPlan uploadPlan =
        BuildSmokeStaticBucketBlasBuildPlan(input);
    Check(uploadPlan.submitBuild && !uploadPlan.createBlas, "static bucket BLAS build plan rebuilds after required upload");

    input.uploadRequired = false;
    input.forceRebuild = true;
    const RtSmokeStaticBucketBlasBuildPlan forcedPlan =
        BuildSmokeStaticBucketBlasBuildPlan(input);
    Check(forcedPlan.submitBuild && !forcedPlan.createBlas, "static bucket BLAS build plan honors force rebuild");

    input.forceRebuild = false;
    input.blasInputsCompatible = false;
    const RtSmokeStaticBucketBlasBuildPlan incompatiblePlan =
        BuildSmokeStaticBucketBlasBuildPlan(input);
    Check(incompatiblePlan.createBlas && incompatiblePlan.submitBuild, "static bucket BLAS build plan recreates incompatible BLAS");

    input.hasBlas = false;
    const RtSmokeStaticBucketBlasBuildPlan missingPlan =
        BuildSmokeStaticBucketBlasBuildPlan(input);
    Check(missingPlan.createBlas && missingPlan.submitBuild, "static bucket BLAS build plan creates missing BLAS");

    input.submitBuilds = false;
    const RtSmokeStaticBucketBlasBuildPlan gateOffPlan =
        BuildSmokeStaticBucketBlasBuildPlan(input);
    Check(!gateOffPlan.createBlas &&
        !gateOffPlan.submitBuild &&
        gateOffPlan.skipBuild,
        "static bucket BLAS build gate skips build submission");
}

void TestStaticBucketBlasBuildBatchPlan()
{
    RtSmokeStaticBucketBlasBuildObservation observations[4];
    observations[0].bucketKey = 10;
    observations[0].hasBlas = true;
    observations[0].blasInputsCompatible = true;
    observations[0].signatureValid = true;
    observations[0].previousBlasInputSignature = 100;
    observations[0].currentBlasInputSignature = 100;

    observations[1] = observations[0];
    observations[1].bucketKey = 20;
    observations[1].currentBlasInputSignature = 101;

    observations[2] = observations[0];
    observations[2].bucketKey = 30;
    observations[2].hasBlas = false;

    observations[3] = observations[0];
    observations[3].bucketKey = 40;
    observations[3].uploadRequired = true;
    observations[3].blasInputsCompatible = false;

    RtSmokeStaticBucketBlasBuildBatchPlanInput input;
    input.observations = observations;
    input.observationCount = 4;
    input.submitBuilds = true;
    const RtSmokeStaticBucketBlasBuildBatchPlan mixedPlan =
        BuildSmokeStaticBucketBlasBuildBatchPlan(input);
    Check(mixedPlan.emittedRecords == 4 &&
        mixedPlan.submitBuildRecords == 3 &&
        mixedPlan.skippedBuildRecords == 1 &&
        mixedPlan.createBlasRecords == 2 &&
        mixedPlan.signatureChangedRecords == 1 &&
        mixedPlan.uploadRequiredRecords == 1 &&
        mixedPlan.incompatibleRecords == 1 &&
        mixedPlan.missingBlasRecords == 1 &&
        mixedPlan.records[0].buildPlan.skipBuild &&
        mixedPlan.records[1].buildPlan.submitBuild &&
        mixedPlan.records[2].buildPlan.createBlas &&
        mixedPlan.planSignature != 0,
        "static bucket BLAS batch plan aggregates mixed bucket build decisions");

    input.submitBuilds = false;
    const RtSmokeStaticBucketBlasBuildBatchPlan gatedPlan =
        BuildSmokeStaticBucketBlasBuildBatchPlan(input);
    Check(gatedPlan.emittedRecords == 4 &&
        gatedPlan.submitBuildRecords == 0 &&
        gatedPlan.skippedBuildRecords == 4 &&
        gatedPlan.createBlasRecords == 0,
        "static bucket BLAS batch plan respects disabled build gate");

    input.submitBuilds = true;
    input.forceRebuild = true;
    const RtSmokeStaticBucketBlasBuildBatchPlan forcePlan =
        BuildSmokeStaticBucketBlasBuildBatchPlan(input);
    Check(forcePlan.submitBuildRecords == 4 &&
        forcePlan.skippedBuildRecords == 0 &&
        forcePlan.records[0].buildPlan.submitBuild,
        "static bucket BLAS batch plan applies force rebuild to every emitted bucket");

    input.forceRebuild = false;
    input.maxRecords = 2;
    const RtSmokeStaticBucketBlasBuildBatchPlan cappedPlan =
        BuildSmokeStaticBucketBlasBuildBatchPlan(input);
    Check(cappedPlan.emittedRecords == 2 &&
        cappedPlan.overflow &&
        cappedPlan.records[1].bucketKey == 20,
        "static bucket BLAS batch plan reports record cap overflow");

    input.maxRecords = 1;
    const RtSmokeStaticBucketBlasBuildBatchPlan cappedPrefixPlan =
        BuildSmokeStaticBucketBlasBuildBatchPlan(input);
    input.observationCount = 1;
    const RtSmokeStaticBucketBlasBuildBatchPlan singlePrefixPlan =
        BuildSmokeStaticBucketBlasBuildBatchPlan(input);
    Check(cappedPrefixPlan.records.size() == singlePrefixPlan.records.size() &&
        cappedPrefixPlan.records[0].bucketKey == singlePrefixPlan.records[0].bucketKey &&
        cappedPrefixPlan.planSignature != singlePrefixPlan.planSignature,
        "static bucket BLAS batch signature tracks capped summary state with identical emitted prefix");

    input.observationCount = 4;
    input.maxRecords = 0;
    const RtSmokeStaticBucketBlasBuildBatchPlan uncappedPlan =
        BuildSmokeStaticBucketBlasBuildBatchPlan(input);
    input.maxRecords = -7;
    const RtSmokeStaticBucketBlasBuildBatchPlan negativeCapPlan =
        BuildSmokeStaticBucketBlasBuildBatchPlan(input);
    Check(uncappedPlan.emittedRecords == negativeCapPlan.emittedRecords &&
        uncappedPlan.planSignature == negativeCapPlan.planSignature,
        "static bucket BLAS batch plan normalizes negative record caps as uncapped");

    input.observations = nullptr;
    input.observationCount = 4;
    const RtSmokeStaticBucketBlasBuildBatchPlan nullRecordsPlan =
        BuildSmokeStaticBucketBlasBuildBatchPlan(input);
    input.observationCount = 0;
    const RtSmokeStaticBucketBlasBuildBatchPlan emptyRecordsPlan =
        BuildSmokeStaticBucketBlasBuildBatchPlan(input);
    Check(nullRecordsPlan.inputRecords == 0 &&
        nullRecordsPlan.planSignature == emptyRecordsPlan.planSignature,
        "static bucket BLAS batch plan normalizes absent observations as empty");
}

void TestStaticBucketBlasBuildObservationPlan()
{
    RtSmokeStaticBvhBucketSignature currentBuckets[4];
    currentBuckets[0].bucketKey = 10;
    currentBuckets[0].blasInputSignature = 100;
    currentBuckets[0].active = true;
    currentBuckets[0].resident = true;

    currentBuckets[1] = currentBuckets[0];
    currentBuckets[1].bucketKey = 20;
    currentBuckets[1].blasInputSignature = 200;

    currentBuckets[2] = currentBuckets[0];
    currentBuckets[2].bucketKey = 30;
    currentBuckets[2].blasInputSignature = 300;
    currentBuckets[2].active = false;

    currentBuckets[3] = currentBuckets[0];
    currentBuckets[3].bucketKey = 40;
    currentBuckets[3].blasInputSignature = 400;

    RtSmokeStaticBucketBlasCacheState previousBuckets[3];
    previousBuckets[0].bucketKey = 10;
    previousBuckets[0].blasInputSignature = 100;
    previousBuckets[0].hasBlas = true;
    previousBuckets[0].blasInputsCompatible = true;

    previousBuckets[1] = previousBuckets[0];
    previousBuckets[1].bucketKey = 20;
    previousBuckets[1].blasInputSignature = 201;

    previousBuckets[2] = previousBuckets[0];
    previousBuckets[2].bucketKey = 40;
    previousBuckets[2].blasInputSignature = 400;
    previousBuckets[2].hasBlas = false;

    RtSmokeStaticBucketBlasBuildObservationPlanInput input;
    input.currentBuckets = currentBuckets;
    input.currentBucketCount = 4;
    input.previousBuckets = previousBuckets;
    input.previousBucketCount = 3;
    const RtSmokeStaticBucketBlasBuildObservationPlan plan =
        BuildSmokeStaticBucketBlasBuildObservationPlan(input);
    Check(plan.emittedObservations == 3 &&
        plan.cacheHits == 1 &&
        plan.cacheMisses == 2 &&
        plan.signatureChanged == 1 &&
        plan.uploadRequired == 2 &&
        plan.skippedInactive == 1 &&
        plan.observations[0].bucketKey == 10 &&
        !plan.observations[0].uploadRequired &&
        plan.observations[1].bucketKey == 20 &&
        plan.observations[1].uploadRequired &&
        plan.observations[2].bucketKey == 40 &&
        !plan.observations[2].hasBlas &&
        plan.planSignature != 0,
        "static bucket BLAS observation plan derives active cache hit and miss observations");

    input.maxRecords = 1;
    const RtSmokeStaticBucketBlasBuildObservationPlan cappedPlan =
        BuildSmokeStaticBucketBlasBuildObservationPlan(input);
    Check(cappedPlan.emittedObservations == 1 &&
        cappedPlan.overflow &&
        cappedPlan.observations[0].bucketKey == 10,
        "static bucket BLAS observation plan reports active record cap overflow");

    input.currentBucketCount = 1;
    const RtSmokeStaticBucketBlasBuildObservationPlan singlePrefixPlan =
        BuildSmokeStaticBucketBlasBuildObservationPlan(input);
    Check(cappedPlan.observations.size() == singlePrefixPlan.observations.size() &&
        cappedPlan.observations[0].bucketKey == singlePrefixPlan.observations[0].bucketKey &&
        cappedPlan.planSignature != singlePrefixPlan.planSignature,
        "static bucket BLAS observation signature tracks capped summary state with identical emitted prefix");

    RtSmokeStaticBvhBucketSignature firstAndInactive[2];
    firstAndInactive[0] = currentBuckets[0];
    firstAndInactive[1] = currentBuckets[2];
    input.currentBuckets = firstAndInactive;
    input.currentBucketCount = 1;
    input.maxRecords = 0;
    const RtSmokeStaticBucketBlasBuildObservationPlan activeOnlyPlan =
        BuildSmokeStaticBucketBlasBuildObservationPlan(input);
    input.currentBucketCount = 2;
    const RtSmokeStaticBucketBlasBuildObservationPlan activeAndInactivePlan =
        BuildSmokeStaticBucketBlasBuildObservationPlan(input);
    Check(activeOnlyPlan.observations.size() == activeAndInactivePlan.observations.size() &&
        activeOnlyPlan.observations[0].bucketKey == activeAndInactivePlan.observations[0].bucketKey &&
        activeOnlyPlan.planSignature != activeAndInactivePlan.planSignature,
        "static bucket BLAS observation signature tracks skipped inactive buckets");

    input.maxRecords = -3;
    const RtSmokeStaticBucketBlasBuildObservationPlan negativeCapPlan =
        BuildSmokeStaticBucketBlasBuildObservationPlan(input);
    Check(activeAndInactivePlan.emittedObservations == negativeCapPlan.emittedObservations &&
        activeAndInactivePlan.planSignature == negativeCapPlan.planSignature,
        "static bucket BLAS observation plan normalizes negative record caps as uncapped");

    input.currentBuckets = nullptr;
    input.currentBucketCount = 2;
    const RtSmokeStaticBucketBlasBuildObservationPlan nullBucketsPlan =
        BuildSmokeStaticBucketBlasBuildObservationPlan(input);
    input.currentBucketCount = 0;
    const RtSmokeStaticBucketBlasBuildObservationPlan emptyBucketsPlan =
        BuildSmokeStaticBucketBlasBuildObservationPlan(input);
    Check(nullBucketsPlan.inputBuckets == 0 &&
        nullBucketsPlan.planSignature == emptyBucketsPlan.planSignature,
        "static bucket BLAS observation plan normalizes absent buckets as empty");
}

void TestStaticBucketWorkPlan()
{
    RtSmokeStaticTlasBucketObservation buckets[3];
    buckets[0].bucketKey = 10;
    buckets[0].resident = true;
    buckets[0].active = true;
    buckets[0].hasBlas = true;
    buckets[0].routeRecordIndex = 0;
    buckets[0].activeReasonFlags = RT_SMOKE_STATIC_ACTIVE_VISIBLE;
    buckets[0].residentVertexOffset = 0;
    buckets[0].residentVertexCount = 12;
    buckets[0].residentIndexOffset = 0;
    buckets[0].residentIndexCount = 36;
    buckets[0].residentTriangleOffset = 0;
    buckets[0].residentTriangleCount = 12;
    buckets[0].activeVertexCount = 12;
    buckets[0].activeIndexCount = 36;
    buckets[0].activeTriangleCount = 12;

    buckets[1] = buckets[0];
    buckets[1].bucketKey = 20;
    buckets[1].active = false;
    buckets[1].routeRecordIndex = 1;
    buckets[1].residentVertexOffset = 12;
    buckets[1].residentIndexOffset = 36;
    buckets[1].residentTriangleOffset = 12;

    buckets[2] = buckets[0];
    buckets[2].bucketKey = 30;
    buckets[2].routeRecordIndex = 2;
    buckets[2].residentVertexOffset = 24;
    buckets[2].residentIndexOffset = 72;
    buckets[2].residentTriangleOffset = 24;

    RtSmokeStaticBucketBlasCacheState previousBuckets[2];
    previousBuckets[0].bucketKey = 10;
    previousBuckets[0].hasBlas = true;
    previousBuckets[0].blasInputsCompatible = true;

    RtSmokeStaticBvhBucketSignatureInput signatureInput;
    signatureInput.bucket = buckets[0];
    signatureInput.geometryContentSignature = 1000;
    signatureInput.materialGeneration = 2000;
    previousBuckets[0].blasInputSignature =
        BuildSmokeStaticBvhBucketSignature(signatureInput).blasInputSignature;

    previousBuckets[1].bucketKey = 30;
    previousBuckets[1].hasBlas = true;
    previousBuckets[1].blasInputsCompatible = true;
    previousBuckets[1].blasInputSignature = previousBuckets[0].blasInputSignature + 1;

    RtSmokeStaticBucketWorkPlanInput input;
    input.buckets = buckets;
    input.bucketCount = 3;
    input.previousBuckets = previousBuckets;
    input.previousBucketCount = 2;
    input.geometryContentSignature = 1000;
    input.materialGeneration = 2000;
    input.totalVertexCount = 36;
    input.totalIndexCount = 108;
    input.totalTriangleCount = 36;
    input.hasStaticBlas = true;
    input.submitBuilds = true;
    input.enableStaticRoutes = true;
    input.shaderSupportsStaticBucketRoutes = false;
    input.rigidRouteRecordCount = 5;
    const RtSmokeStaticBucketWorkPlan blockedPlan =
        BuildSmokeStaticBucketWorkPlan(input);
    Check(blockedPlan.bucketSignatures.size() == 3 &&
        blockedPlan.activeSetPlan.activeBuckets == 2 &&
        blockedPlan.activeSetPlan.inactiveResidentBuckets == 1 &&
        blockedPlan.activeSetPlan.inactiveResidentGeometryIncluded &&
        blockedPlan.bucketBlasPlan.emittedRecords == 2 &&
        blockedPlan.bucketBlasPlan.skippedInactive == 1 &&
        blockedPlan.traversalCompatibility.requiresShaderRouteMetadata &&
        blockedPlan.routeNamespace.staticRoutesBlocked &&
        blockedPlan.routeTablePlan.emittedRecords == 0 &&
        blockedPlan.routeTablePlan.blocked &&
        blockedPlan.buildObservationPlan.emittedObservations == 2 &&
        blockedPlan.buildObservationPlan.cacheHits == 1 &&
        blockedPlan.buildObservationPlan.cacheMisses == 1 &&
        blockedPlan.buildBatchPlan.submitBuildRecords == 1 &&
        blockedPlan.buildBatchPlan.skippedBuildRecords == 1 &&
        blockedPlan.planSignature != 0,
        "static bucket work plan composes bucket cache, build, route, and shader compatibility decisions");

    RtSmokeStaticBucketWorkPlanInput disabledRouteInput = input;
    disabledRouteInput.enableStaticRoutes = false;
    const RtSmokeStaticBucketWorkPlan disabledRoutePlan =
        BuildSmokeStaticBucketWorkPlan(disabledRouteInput);
    Check(!disabledRoutePlan.routeNamespace.staticRoutesEnabled &&
        !disabledRoutePlan.routeNamespace.staticRoutesBlocked &&
        disabledRoutePlan.planSignature != blockedPlan.planSignature,
        "static bucket work plan signature separates disabled and blocked static routes");

    input.shaderSupportsStaticBucketRoutes = true;
    const RtSmokeStaticBucketWorkPlan routedPlan =
        BuildSmokeStaticBucketWorkPlan(input);
    Check(routedPlan.routeNamespace.staticRoutesEnabled &&
        !routedPlan.routeNamespace.staticRoutesBlocked &&
        routedPlan.routeTablePlan.emittedRecords == 2 &&
        routedPlan.routeTablePlan.records[0].instanceId == 2 &&
        routedPlan.routeNamespace.rigidFirstInstanceId == 4,
        "static bucket work plan emits route table records when shader route support is available");
    Check(blockedPlan.planSignature != routedPlan.planSignature,
        "static bucket work plan signature tracks traversal route compatibility");

    input.maxRouteRecords = 1;
    const RtSmokeStaticBucketWorkPlan routeCappedPlan =
        BuildSmokeStaticBucketWorkPlan(input);
    Check(routeCappedPlan.bucketBlasPlan.emittedRecords == 2 &&
        !routeCappedPlan.bucketBlasPlan.overflow &&
        routeCappedPlan.routeTablePlan.emittedRecords == 1 &&
        routeCappedPlan.routeTablePlan.overflow,
        "static bucket work plan reports route table cap overflow independently");

    input.maxRouteRecords = 0;
    input.maxBuildRecords = 1;
    const RtSmokeStaticBucketWorkPlan buildCappedPlan =
        BuildSmokeStaticBucketWorkPlan(input);
    Check(buildCappedPlan.buildObservationPlan.emittedObservations == 1 &&
        buildCappedPlan.buildObservationPlan.overflow &&
        buildCappedPlan.buildBatchPlan.emittedRecords == 1 &&
        !buildCappedPlan.buildBatchPlan.overflow,
        "static bucket work plan reports build observation cap overflow independently");

    input.maxBuildRecords = 0;
    input.maxBucketRecords = 1;
    const RtSmokeStaticBucketWorkPlan bucketCappedPlan =
        BuildSmokeStaticBucketWorkPlan(input);
    Check(bucketCappedPlan.bucketBlasPlan.emittedRecords == 1 &&
        bucketCappedPlan.bucketBlasPlan.overflow &&
        bucketCappedPlan.routeTablePlan.emittedRecords == 1 &&
        !bucketCappedPlan.routeTablePlan.overflow,
        "static bucket work plan reports bucket BLAS cap overflow independently");

    RtSmokeStaticBucketWorkPlanInput monolithicInput;
    monolithicInput.buckets = buckets;
    monolithicInput.bucketCount = 1;
    monolithicInput.geometryContentSignature = 1000;
    monolithicInput.materialGeneration = 2000;
    monolithicInput.totalVertexCount = buckets[0].residentVertexCount;
    monolithicInput.totalIndexCount = buckets[0].residentIndexCount;
    monolithicInput.totalTriangleCount = buckets[0].residentTriangleCount;
    monolithicInput.hasStaticBlas = true;
    monolithicInput.enableStaticRoutes = true;
    monolithicInput.shaderSupportsStaticBucketRoutes = false;
    monolithicInput.rigidRouteRecordCount = 3;
    const RtSmokeStaticBucketWorkPlan monolithicPlan =
        BuildSmokeStaticBucketWorkPlan(monolithicInput);
    Check(monolithicPlan.traversalCompatibility.exactMonolithicRecord &&
        !monolithicPlan.routeNamespace.staticRoutesBlocked &&
        monolithicPlan.routeTablePlan.emittedRecords == 0 &&
        monolithicPlan.routeNamespace.rigidFirstInstanceId == 2,
        "static bucket work plan keeps exact monolithic static BLAS on the current route namespace");
}

void TestStaticBucketPublicationEpochPlan()
{
    RtSmokeStaticBucketPublicationEpochInput input;
    input.expectedGeneration = 100;
    input.tlasGeneration = 100;
    input.routeGeneration = 100;
    input.residentBuckets = 8;
    input.activeBuckets = 3;
    input.tlasInstances = 3;
    input.routeRecords = 8;
    input.activeSetExact = true;
    const RtSmokeStaticBucketPublicationEpochPlan exactPlan =
        BuildSmokeStaticBucketPublicationEpochPlan(input);
    Check(exactPlan.generationValid &&
        exactPlan.generationsMatch &&
        exactPlan.countsMatch &&
        exactPlan.accepted &&
        !exactPlan.mixedEpochRejected,
        "static bucket publication accepts active TLAS and resident route counts");

    input.activeBuckets = 2;
    input.tlasInstances = 2;
    const RtSmokeStaticBucketPublicationEpochPlan
        changedActiveSetPlan =
            BuildSmokeStaticBucketPublicationEpochPlan(input);
    Check(changedActiveSetPlan.accepted &&
        changedActiveSetPlan.countsMatch,
        "static bucket publication keeps resident routes complete when the active subset changes");
    input.activeBuckets = 3;
    input.tlasInstances = 3;

    input.routeGeneration = 101;
    const RtSmokeStaticBucketPublicationEpochPlan mixedRoutePlan =
        BuildSmokeStaticBucketPublicationEpochPlan(input);
    Check(!mixedRoutePlan.accepted &&
        mixedRoutePlan.mixedEpochRejected,
        "static bucket publication rejects a mixed route epoch");

    input.routeGeneration = 100;
    input.tlasGeneration = 99;
    const RtSmokeStaticBucketPublicationEpochPlan mixedTlasPlan =
        BuildSmokeStaticBucketPublicationEpochPlan(input);
    Check(!mixedTlasPlan.accepted &&
        mixedTlasPlan.mixedEpochRejected,
        "static bucket publication rejects a mixed TLAS epoch");

    input.tlasGeneration = 100;
    input.routeRecords = 7;
    const RtSmokeStaticBucketPublicationEpochPlan countPlan =
        BuildSmokeStaticBucketPublicationEpochPlan(input);
    Check(!countPlan.accepted &&
        countPlan.generationsMatch &&
        !countPlan.countsMatch &&
        !countPlan.mixedEpochRejected,
        "static bucket publication rejects incomplete route records");

    input.routeRecords = 8;
    input.activeSetExact = false;
    const RtSmokeStaticBucketPublicationEpochPlan inexactPlan =
        BuildSmokeStaticBucketPublicationEpochPlan(input);
    Check(!inexactPlan.accepted &&
        inexactPlan.generationsMatch &&
        inexactPlan.countsMatch,
        "static bucket publication rejects an inexact active set");

    input.activeSetExact = true;
    input.expectedGeneration = 0;
    input.tlasGeneration = 0;
    input.routeGeneration = 0;
    const RtSmokeStaticBucketPublicationEpochPlan zeroPlan =
        BuildSmokeStaticBucketPublicationEpochPlan(input);
    Check(!zeroPlan.generationValid &&
        !zeroPlan.accepted &&
        !zeroPlan.mixedEpochRejected,
        "static bucket publication rejects missing epochs without misreporting a mixed epoch");
}

void TestStaticBucketInstanceAddressPlan()
{
    RtSmokePlanGeometryRange range;
    range.vertexOffset = 4;
    range.vertexCount = 4;
    range.indexOffset = 6;
    range.indexCount = 6;
    range.triangleOffset = 2;
    range.triangleCount = 2;
    const RtSmokeStaticBucketInstanceAddressPlan exactPlan =
        BuildSmokeStaticBucketInstanceAddressPlan(
            range,
            15,
            5);
    Check(
        exactPlan.valid &&
            exactPlan.rangeValid &&
            exactPlan.indexAddressCompatible &&
            exactPlan.instanceIdEncodable &&
            exactPlan.instanceId ==
                (RT_SMOKE_STATIC_BUCKET_INSTANCE_ID_NAMESPACE | 2u),
        "static bucket InstanceID encodes the global triangle base");

    uint32_t decodedTriangleOffset = 0;
    Check(
        TryDecodeSmokeStaticBucketInstanceId(
            exactPlan.instanceId,
            decodedTriangleOffset) &&
            decodedTriangleOffset == 2,
        "static bucket InstanceID round-trips the global triangle base");

    RtSmokePlanGeometryRange mismatchedIndexRange = range;
    mismatchedIndexRange.indexOffset = 3;
    const RtSmokeStaticBucketInstanceAddressPlan mismatchedIndexPlan =
        BuildSmokeStaticBucketInstanceAddressPlan(
            mismatchedIndexRange,
            15,
            5);
    Check(
        mismatchedIndexPlan.rangeValid &&
            !mismatchedIndexPlan.indexAddressCompatible &&
            !mismatchedIndexPlan.valid,
        "static bucket InstanceID contract rejects a non-shared index and triangle base");

    RtSmokePlanGeometryRange overflowRange = range;
    overflowRange.indexOffset =
        static_cast<int>(
            RT_SMOKE_STATIC_BUCKET_TRIANGLE_OFFSET_MASK) * 3;
    overflowRange.triangleOffset =
        static_cast<int>(
            RT_SMOKE_STATIC_BUCKET_TRIANGLE_OFFSET_MASK);
    overflowRange.indexCount = 3;
    overflowRange.triangleCount = 1;
    const RtSmokeStaticBucketInstanceAddressPlan maximumPlan =
        BuildSmokeStaticBucketInstanceAddressPlan(
            overflowRange,
            overflowRange.indexOffset + overflowRange.indexCount,
            overflowRange.triangleOffset +
                overflowRange.triangleCount);
    Check(
        maximumPlan.valid &&
            maximumPlan.instanceId ==
                RT_SMOKE_SHADER_INSTANCE_ID_MASK,
        "static bucket InstanceID accepts the maximum encodable triangle base");

    overflowRange.indexCount = 6;
    overflowRange.triangleCount = 2;
    const RtSmokeStaticBucketInstanceAddressPlan overflowPlan =
        BuildSmokeStaticBucketInstanceAddressPlan(
            overflowRange,
            overflowRange.indexOffset + overflowRange.indexCount,
            overflowRange.triangleOffset +
                overflowRange.triangleCount);
    Check(
        overflowPlan.rangeValid &&
            !overflowPlan.instanceIdEncodable &&
            !overflowPlan.valid,
        "static bucket InstanceID rejects triangle bases outside the reserved namespace");

    RtSmokePlanGeometryRange secondRange = range;
    secondRange.indexOffset += secondRange.indexCount;
    secondRange.triangleOffset += secondRange.triangleCount;
    const RtSmokeStaticBucketInstanceAddressPlan secondPlan =
        BuildSmokeStaticBucketInstanceAddressPlan(
            secondRange,
            18,
            6);
    Check(
        secondPlan.valid &&
            secondPlan.instanceId != exactPlan.instanceId,
        "static bucket InstanceID is unique for disjoint nonempty resident ranges");
    Check(
        exactPlan.instanceId ==
            BuildSmokeStaticBucketInstanceAddressPlan(
                range,
                15,
                5).instanceId,
        "static bucket InstanceID remains stable when active membership is recomputed");

    uint32_t ignoredOffset = 0;
    Check(
        !TryDecodeSmokeStaticBucketInstanceId(
            RT_SMOKE_STATIC_BUCKET_INSTANCE_ID_NAMESPACE - 1,
            ignoredOffset) &&
            !TryDecodeSmokeStaticBucketInstanceId(
                RT_SMOKE_SHADER_INSTANCE_ID_MASK + 1,
                ignoredOffset),
        "static bucket InstanceID decoder rejects other namespaces and non-24-bit IDs");
}

void TestStaticBucketSurfaceAddressPlan()
{
    const RtSmokeStaticBucketSurfaceAddressPlan exactPlan =
        BuildSmokeStaticBucketSurfaceAddressPlan(7, 3, 10);
    Check(
        exactPlan.valid &&
            exactPlan.rangeValid &&
            exactPlan.instanceIdEncodable &&
            exactPlan.firstSurfaceRecord == 7 &&
            exactPlan.surfaceRecordCount == 3 &&
            exactPlan.instanceId ==
                (RT_SMOKE_STATIC_BUCKET_INSTANCE_ID_NAMESPACE | 7u),
        "static bucket surface-base InstanceID accepts an exact record range");

    uint32_t decodedFirstSurfaceRecord = 0;
    Check(
        TryDecodeSmokeStaticBucketSurfaceBaseInstanceId(
            exactPlan.instanceId,
            decodedFirstSurfaceRecord) &&
            decodedFirstSurfaceRecord == 7,
        "static bucket surface-base InstanceID round-trips the first record");

    const RtSmokeStaticBucketSurfaceAddressPlan maximumPlan =
        BuildSmokeStaticBucketSurfaceAddressPlan(
            RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_OFFSET_MASK,
            1,
            RT_SMOKE_STATIC_BUCKET_INSTANCE_ID_NAMESPACE);
    Check(
        maximumPlan.valid &&
            maximumPlan.instanceId ==
                RT_SMOKE_SHADER_INSTANCE_ID_MASK,
        "static bucket surface-base InstanceID accepts the maximum one-record range");

    const RtSmokeStaticBucketSurfaceAddressPlan overflowPlan =
        BuildSmokeStaticBucketSurfaceAddressPlan(
            RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_OFFSET_MASK,
            2,
            RT_SMOKE_STATIC_BUCKET_INSTANCE_ID_NAMESPACE + 1);
    Check(
        overflowPlan.rangeValid &&
            !overflowPlan.instanceIdEncodable &&
            !overflowPlan.valid,
        "static bucket surface-base InstanceID rejects a record range crossing 23 bits");

    const RtSmokeStaticBucketSurfaceAddressPlan outOfRangePlan =
        BuildSmokeStaticBucketSurfaceAddressPlan(7, 4, 10);
    Check(
        !outOfRangePlan.rangeValid &&
            outOfRangePlan.instanceIdEncodable &&
            !outOfRangePlan.valid,
        "static bucket surface-base contract rejects a range beyond the resident table");

    const RtSmokeStaticBucketSurfaceAddressPlan emptyPlan =
        BuildSmokeStaticBucketSurfaceAddressPlan(0, 0, 0);
    Check(
        !emptyPlan.rangeValid &&
            emptyPlan.instanceIdEncodable &&
            !emptyPlan.valid,
        "static bucket surface-base contract rejects an empty bucket");

    uint32_t ignoredSurfaceRecord = 0;
    Check(
        !TryDecodeSmokeStaticBucketSurfaceBaseInstanceId(
            RT_SMOKE_STATIC_BUCKET_INSTANCE_ID_NAMESPACE - 1,
            ignoredSurfaceRecord) &&
            !TryDecodeSmokeStaticBucketSurfaceBaseInstanceId(
                RT_SMOKE_SHADER_INSTANCE_ID_MASK + 1,
                ignoredSurfaceRecord),
        "static bucket surface-base decoder rejects other namespaces and non-24-bit IDs");
}

void TestStaticBucketResidentPackCachePlan()
{
    RtSmokeStaticBucketResidentPackCacheInput input;
    input.assignmentPlanSignature = 100;
    input.geometryGeneration = 200;
    input.materialGeneration = 300;
    input.cachedAssignmentPlanSignature = 100;
    input.cachedGeometryGeneration = 200;
    input.cachedMaterialGeneration = 300;
    input.assignmentExact = true;
    input.cachedPackExact = true;
    input.cacheValid = true;
    const RtSmokeStaticBucketResidentPackCachePlan exactPlan =
        BuildSmokeStaticBucketResidentPackCachePlan(input);
    Check(
        exactPlan.reuse && !exactPlan.rebuild,
        "static bucket resident pack cache reuses an exact unchanged universe");

    input.geometryGeneration = 201;
    Check(
        BuildSmokeStaticBucketResidentPackCachePlan(input).rebuild,
        "static bucket resident pack cache rebuilds changed geometry");
    input.geometryGeneration = 200;

    input.materialGeneration = 301;
    Check(
        BuildSmokeStaticBucketResidentPackCachePlan(input).rebuild,
        "static bucket resident pack cache rebuilds changed per-triangle material data");
    input.materialGeneration = 300;

    input.assignmentPlanSignature = 101;
    Check(
        BuildSmokeStaticBucketResidentPackCachePlan(input).rebuild,
        "static bucket resident pack cache rebuilds changed bucket topology");
    input.assignmentPlanSignature = 100;

    input.assignmentExact = false;
    Check(
        BuildSmokeStaticBucketResidentPackCachePlan(input).rebuild,
        "static bucket resident pack cache rejects inexact current assignment");
    input.assignmentExact = true;

    input.cacheValid = false;
    Check(
        BuildSmokeStaticBucketResidentPackCachePlan(input).rebuild,
        "static bucket resident pack cache rebuilds an invalid cache");
}

void TestStaticBucketMaterialIndexCachePlan()
{
    RtSmokeStaticBucketMaterialIndexCacheInput input;
    input.residentPackSignature = 100;
    input.materialBindingSignature = 200;
    input.triangleCount = 50108;
    input.bucketCount = 56;
    input.cachedResidentPackSignature = 100;
    input.cachedMaterialBindingSignature = 200;
    input.cachedTriangleCount = 50108;
    input.cachedBucketCount = 56;
    input.residentPackExact = true;
    input.cacheValid = true;
    const RtSmokeStaticBucketMaterialIndexCachePlan exactPlan =
        BuildSmokeStaticBucketMaterialIndexCachePlan(input);
    Check(
        exactPlan.reuse && !exactPlan.rebuild,
        "static bucket material-index cache reuses an exact unchanged pack and material binding");

    input.materialBindingSignature = 201;
    Check(
        BuildSmokeStaticBucketMaterialIndexCachePlan(input).rebuild,
        "static bucket material-index cache rebuilds a changed resident material binding");
    input.materialBindingSignature = 200;

    input.residentPackSignature = 101;
    Check(
        BuildSmokeStaticBucketMaterialIndexCachePlan(input).rebuild,
        "static bucket material-index cache rebuilds a changed resident pack");
    input.residentPackSignature = 100;

    input.triangleCount = 50107;
    Check(
        BuildSmokeStaticBucketMaterialIndexCachePlan(input).rebuild,
        "static bucket material-index cache rejects a triangle-count mismatch");
    input.triangleCount = 50108;

    input.bucketCount = 55;
    Check(
        BuildSmokeStaticBucketMaterialIndexCachePlan(input).rebuild,
        "static bucket material-index cache rejects a bucket-count mismatch");
    input.bucketCount = 56;

    input.residentPackExact = false;
    Check(
        BuildSmokeStaticBucketMaterialIndexCachePlan(input).rebuild,
        "static bucket material-index cache rejects an inexact resident pack");
    input.residentPackExact = true;

    input.cacheValid = false;
    Check(
        BuildSmokeStaticBucketMaterialIndexCachePlan(input).rebuild,
        "static bucket material-index cache rebuilds an invalid cache");
}

void TestStaticBucketCutoverPlan()
{
    RtSmokeStaticBucketCutoverInput input;
    input.residentBuckets = 8;
    input.activeBuckets = 3;
    input.readyBuckets = 8;
    input.tlasInstances = 3;
    input.routeRecords = 8;
    input.requested = true;
    input.consumerSupported = true;
    input.publicationValid = true;
    input.routeUploaded = true;
    const RtSmokeStaticBucketCutoverPlan exactPlan =
        BuildSmokeStaticBucketCutoverPlan(input);
    Check(exactPlan.allResidentReady &&
        exactPlan.publicationExact &&
        exactPlan.accepted,
        "static bucket cutover accepts one exact fully resident publication");

    input.consumerSupported = false;
    const RtSmokeStaticBucketCutoverPlan sentinelPlan =
        BuildSmokeStaticBucketCutoverPlan(input);
    Check(!sentinelPlan.accepted &&
        sentinelPlan.allResidentReady &&
        sentinelPlan.publicationExact,
        "static bucket cutover rejects a bucket-incapable consumer");

    input.consumerSupported = true;
    input.readyBuckets = 3;
    const RtSmokeStaticBucketCutoverPlan partialResidencyPlan =
        BuildSmokeStaticBucketCutoverPlan(input);
    Check(!partialResidencyPlan.accepted &&
        !partialResidencyPlan.allResidentReady,
        "static bucket cutover waits for every resident bucket");

    input.readyBuckets = 8;
    input.routeRecords = 7;
    const RtSmokeStaticBucketCutoverPlan incompleteRoutePlan =
        BuildSmokeStaticBucketCutoverPlan(input);
    Check(!incompleteRoutePlan.accepted &&
        !incompleteRoutePlan.publicationExact,
        "static bucket cutover rejects an incomplete resident route table");

    input.routeRecords = 8;
    input.requested = false;
    const RtSmokeStaticBucketCutoverPlan rollbackPlan =
        BuildSmokeStaticBucketCutoverPlan(input);
    Check(!rollbackPlan.accepted &&
        rollbackPlan.allResidentReady &&
        rollbackPlan.publicationExact,
        "static bucket cutover keeps the default-off rollback monolithic");
}

void TestStaticBucketRigidRouteNamespaceComposition()
{
    RtSmokeStaticTlasBucketObservation buckets[2];
    buckets[0].bucketKey = 10;
    buckets[0].resident = true;
    buckets[0].active = true;
    buckets[0].hasBlas = true;
    buckets[0].routeRecordIndex = 0;
    buckets[0].residentVertexCount = 3;
    buckets[0].residentIndexCount = 3;
    buckets[0].residentTriangleCount = 1;
    buckets[0].activeVertexCount = 3;
    buckets[0].activeIndexCount = 3;
    buckets[0].activeTriangleCount = 1;

    buckets[1] = buckets[0];
    buckets[1].bucketKey = 20;
    buckets[1].routeRecordIndex = 1;
    buckets[1].residentVertexOffset = 3;
    buckets[1].residentIndexOffset = 3;
    buckets[1].residentTriangleOffset = 1;

    RtSmokeStaticBucketWorkPlanInput staticInput;
    staticInput.buckets = buckets;
    staticInput.bucketCount = 2;
    staticInput.geometryContentSignature = 500;
    staticInput.materialGeneration = 600;
    staticInput.totalVertexCount = 6;
    staticInput.totalIndexCount = 6;
    staticInput.totalTriangleCount = 2;
    staticInput.hasStaticBlas = true;
    staticInput.enableStaticRoutes = true;
    staticInput.shaderSupportsStaticBucketRoutes = true;
    staticInput.rigidRouteRecordCount = 2;
    const RtSmokeStaticBucketWorkPlan staticPlan =
        BuildSmokeStaticBucketWorkPlan(staticInput);

    RtSmokeRigidTlasObservation rigidObservations[2];
    rigidObservations[0].meshHash = 1000;
    rigidObservations[0].instanceId = 100;
    rigidObservations[0].sourceFlags = 0x2;
    rigidObservations[0].hasMeshRecord = true;
    rigidObservations[0].meshSeenThisFrame = true;
    rigidObservations[0].hasBlas = true;
    rigidObservations[0].objectToWorld[0] = 1.0f;
    rigidObservations[0].objectToWorld[5] = 1.0f;
    rigidObservations[0].objectToWorld[10] = 1.0f;
    rigidObservations[0].objectToWorld[15] = 1.0f;
    rigidObservations[1] = rigidObservations[0];
    rigidObservations[1].meshHash = 2000;
    rigidObservations[1].instanceId = 200;

    RtSmokeRigidTlasPlanDesc rigidDesc;
    rigidDesc.observations = rigidObservations;
    rigidDesc.observationCount = 2;
    rigidDesc.rigidSourceMask = 0x2;
    rigidDesc.firstInstanceId = staticPlan.routeNamespace.rigidFirstInstanceId;
    rigidDesc.instanceMask = 0x02;
    const RtSmokeRigidTlasPlan rigidPlan = BuildSmokeRigidTlasPlan(rigidDesc);

    Check(staticPlan.routeTablePlan.emittedRecords == 2 &&
        rigidPlan.emittedInstances == 2 &&
        staticPlan.routeTablePlan.records[0].instanceId == 2 &&
        staticPlan.routeTablePlan.records[1].instanceId == 3 &&
        rigidPlan.instances[0].instanceId == 4 &&
        rigidPlan.instances[1].instanceId == 5,
        "static bucket and rigid route planners compose non-overlapping instance IDs");

    staticInput.shaderSupportsStaticBucketRoutes = false;
    const RtSmokeStaticBucketWorkPlan blockedStaticPlan =
        BuildSmokeStaticBucketWorkPlan(staticInput);
    rigidDesc.firstInstanceId = blockedStaticPlan.routeNamespace.rigidFirstInstanceId;
    const RtSmokeRigidTlasPlan blockedRigidPlan = BuildSmokeRigidTlasPlan(rigidDesc);
    Check(blockedStaticPlan.routeNamespace.staticRoutesBlocked &&
        blockedStaticPlan.routeTablePlan.emittedRecords == 0 &&
        blockedRigidPlan.instances[0].instanceId == 2,
        "blocked static routes keep current rigid route instance base");
}

void TestStaticBucketWorkPlanSnapshot()
{
    RtSmokeStaticTlasBucketObservation buckets[2];
    buckets[0].bucketKey = 10;
    buckets[0].resident = true;
    buckets[0].active = true;
    buckets[0].hasBlas = true;
    buckets[0].routeRecordIndex = 0;
    buckets[0].residentSurfaceCount = 1;
    buckets[0].residentVertexCount = 12;
    buckets[0].residentIndexCount = 36;
    buckets[0].residentTriangleCount = 12;
    buckets[0].activeVertexCount = 12;
    buckets[0].activeIndexCount = 36;
    buckets[0].activeTriangleCount = 12;

    buckets[1] = buckets[0];
    buckets[1].bucketKey = 20;
    buckets[1].routeRecordIndex = 1;
    buckets[1].residentVertexOffset = 12;
    buckets[1].residentIndexOffset = 36;
    buckets[1].residentTriangleOffset = 12;

    RtSmokeStaticBucketBlasCacheState previousBuckets[1];
    previousBuckets[0].bucketKey = 10;
    previousBuckets[0].hasBlas = true;
    previousBuckets[0].blasInputsCompatible = true;

    RtSmokeStaticBvhBucketSignatureInput signatureInput;
    signatureInput.bucket = buckets[0];
    signatureInput.geometryContentSignature = 3000;
    signatureInput.materialGeneration = 4000;
    previousBuckets[0].blasInputSignature =
        BuildSmokeStaticBvhBucketSignature(signatureInput).blasInputSignature;

    RtSmokeStaticBucketWorkPlanInput input;
    input.buckets = buckets;
    input.bucketCount = 2;
    input.previousBuckets = previousBuckets;
    input.previousBucketCount = 1;
    input.geometryContentSignature = 3000;
    input.materialGeneration = 4000;
    input.totalVertexCount = 24;
    input.totalIndexCount = 72;
    input.totalTriangleCount = 24;
    input.submitBuilds = true;
    input.enableStaticRoutes = true;
    input.shaderSupportsStaticBucketRoutes = true;
    const RtSmokeStaticBucketWorkPlanSnapshot snapshot =
        CaptureSmokeStaticBucketWorkPlanSnapshot(input);
    const RtSmokeStaticBucketWorkPlan originalPlan =
        BuildSmokeStaticBucketWorkPlan(snapshot);

    buckets[0].bucketKey = 99;
    buckets[0].active = false;
    buckets[1].residentIndexCount = 0;
    previousBuckets[0].hasBlas = false;
    previousBuckets[0].blasInputSignature = 0;
    const RtSmokeStaticBucketWorkPlan snapshotPlan =
        BuildSmokeStaticBucketWorkPlan(snapshot);
    const RtSmokeStaticBucketWorkPlan mutatedSourcePlan =
        BuildSmokeStaticBucketWorkPlan(input);

    Check(snapshot.buckets.size() == 2 &&
        snapshot.previousBuckets.size() == 1 &&
        snapshotPlan.planSignature == originalPlan.planSignature &&
        snapshotPlan.bucketBlasPlan.emittedRecords == 2 &&
        snapshotPlan.buildObservationPlan.cacheHits == 1 &&
        mutatedSourcePlan.planSignature != snapshotPlan.planSignature,
        "static bucket work snapshot owns immutable bucket and cache input data");

    RtSmokeStaticBucketWorkPlanInput emptyInput = input;
    emptyInput.buckets = nullptr;
    emptyInput.bucketCount = 2;
    emptyInput.previousBuckets = previousBuckets;
    emptyInput.previousBucketCount = 1;
    const RtSmokeStaticBucketWorkPlanSnapshot emptySnapshot =
        CaptureSmokeStaticBucketWorkPlanSnapshot(emptyInput);
    Check(emptySnapshot.buckets.empty() &&
        emptySnapshot.previousBuckets.empty() &&
        BuildSmokeStaticBucketWorkPlanInputToken(emptySnapshot) ==
            BuildSmokeStaticBucketWorkPlanInputToken(emptyInput),
        "static bucket work snapshot drops previous cache rows when buckets are absent");
}

void TestStaticBucketWorkPlanTimedResult()
{
    RtSmokeStaticTlasBucketObservation bucket;
    bucket.bucketKey = 77;
    bucket.resident = true;
    bucket.active = true;
    bucket.hasBlas = true;
    bucket.routeRecordIndex = 0;
    bucket.residentVertexCount = 3;
    bucket.residentIndexCount = 3;
    bucket.residentTriangleCount = 1;
    bucket.activeVertexCount = 3;
    bucket.activeIndexCount = 3;
    bucket.activeTriangleCount = 1;

    RtSmokeStaticBucketWorkPlanInput input;
    input.buckets = &bucket;
    input.bucketCount = 1;
    input.geometryContentSignature = 55;
    input.materialGeneration = 66;
    input.totalVertexCount = 3;
    input.totalIndexCount = 3;
    input.totalTriangleCount = 1;
    input.submitBuilds = true;
    const RtSmokeStaticBucketWorkPlanSnapshot snapshot =
        CaptureSmokeStaticBucketWorkPlanSnapshot(input);
    const RtSmokeStaticBucketWorkPlan expectedPlan =
        BuildSmokeStaticBucketWorkPlan(snapshot);
    const RtSmokeStaticBucketWorkPlanTimedResult timedResult =
        BuildSmokeStaticBucketWorkPlanTimedResult(snapshot);
    Check(timedResult.plan.planSignature == expectedPlan.planSignature &&
        timedResult.plan.bucketBlasPlan.emittedRecords == 1,
        "timed static bucket work result preserves snapshot planning output");
}

void TestStaticBucketWorkPlanInputToken()
{
    RtSmokeStaticTlasBucketObservation buckets[2];
    buckets[0].bucketKey = 10;
    buckets[0].resident = true;
    buckets[0].active = true;
    buckets[0].hasBlas = true;
    buckets[0].routeRecordIndex = 0;
    buckets[0].residentSurfaceCount = 1;
    buckets[0].residentVertexCount = 12;
    buckets[0].residentIndexCount = 36;
    buckets[0].residentTriangleCount = 12;
    buckets[0].activeVertexCount = 12;
    buckets[0].activeIndexCount = 36;
    buckets[0].activeTriangleCount = 12;

    buckets[1] = buckets[0];
    buckets[1].bucketKey = 20;
    buckets[1].routeRecordIndex = 1;
    buckets[1].residentVertexOffset = 12;
    buckets[1].residentIndexOffset = 36;
    buckets[1].residentTriangleOffset = 12;

    RtSmokeStaticBucketBlasCacheState previousBuckets[2];
    previousBuckets[0].bucketKey = 10;
    previousBuckets[0].blasInputSignature = 100;
    previousBuckets[0].hasBlas = true;
    previousBuckets[0].blasInputsCompatible = true;
    previousBuckets[1] = previousBuckets[0];
    previousBuckets[1].bucketKey = 99;
    previousBuckets[1].blasInputSignature = 999;
    previousBuckets[1].hasBlas = false;
    previousBuckets[1].blasInputsCompatible = false;

    RtSmokeStaticBucketWorkPlanInput input;
    input.buckets = buckets;
    input.bucketCount = 2;
    input.previousBuckets = previousBuckets;
    input.previousBucketCount = 1;
    input.geometryContentSignature = 1000;
    input.materialGeneration = 2000;
    input.totalVertexCount = 24;
    input.totalIndexCount = 72;
    input.totalTriangleCount = 24;
    input.submitBuilds = true;
    input.enableStaticRoutes = true;
    input.shaderSupportsStaticBucketRoutes = false;
    input.rigidRouteRecordCount = 4;

    const uint64_t baseToken = BuildSmokeStaticBucketWorkPlanInputToken(input);
    Check(baseToken == BuildSmokeStaticBucketWorkPlanInputToken(input),
        "static bucket work input token is deterministic");

    RtSmokeStaticBucketWorkPlanInput changedInput = input;
    buckets[1].active = false;
    Check(baseToken != BuildSmokeStaticBucketWorkPlanInputToken(changedInput),
        "static bucket work input token tracks active bucket membership");

    buckets[1].active = true;
    buckets[1].residentIndexCount = 33;
    Check(baseToken != BuildSmokeStaticBucketWorkPlanInputToken(input),
        "static bucket work input token tracks bucket geometry ranges");

    buckets[1].residentIndexCount = 36;
    buckets[1].residentSurfaceCount = 2;
    Check(baseToken != BuildSmokeStaticBucketWorkPlanInputToken(input),
        "static bucket work input token tracks resident surface counts");

    buckets[1].residentSurfaceCount = 1;
    previousBuckets[0].blasInputSignature = 101;
    Check(baseToken != BuildSmokeStaticBucketWorkPlanInputToken(input),
        "static bucket work input token tracks previous bucket cache signatures");

    previousBuckets[0].blasInputSignature = 100;
    input.shaderSupportsStaticBucketRoutes = true;
    Check(baseToken != BuildSmokeStaticBucketWorkPlanInputToken(input),
        "static bucket work input token tracks route support flags");

    input.shaderSupportsStaticBucketRoutes = false;
    const uint64_t restoredToken = BuildSmokeStaticBucketWorkPlanInputToken(input);

    input.previousBucketCount = 2;
    Check(restoredToken == BuildSmokeStaticBucketWorkPlanInputToken(input),
        "static bucket work input token ignores previous cache entries outside active buckets");

    previousBuckets[1].bucketKey = 10;
    previousBuckets[1].blasInputSignature = 999;
    previousBuckets[1].hasBlas = false;
    previousBuckets[1].blasInputsCompatible = false;
    Check(restoredToken == BuildSmokeStaticBucketWorkPlanInputToken(input),
        "static bucket work input token uses first previous cache row for duplicate keys");
    const RtSmokeStaticBucketWorkPlanSnapshot duplicatePreviousSnapshot =
        CaptureSmokeStaticBucketWorkPlanSnapshot(input);
    Check(duplicatePreviousSnapshot.previousBuckets.size() == 1 &&
        duplicatePreviousSnapshot.previousBuckets[0].bucketKey == 10 &&
        duplicatePreviousSnapshot.previousBuckets[0].blasInputSignature == 100 &&
        BuildSmokeStaticBucketWorkPlanInputToken(duplicatePreviousSnapshot) == restoredToken,
        "static bucket work snapshot preserves first previous cache row for duplicate keys");

    previousBuckets[1].bucketKey = 20;
    Check(restoredToken != BuildSmokeStaticBucketWorkPlanInputToken(input),
        "static bucket work input token tracks previous cache entries for active buckets");

    buckets[1].active = false;
    input.previousBucketCount = 1;
    const uint64_t inactiveBucketToken = BuildSmokeStaticBucketWorkPlanInputToken(input);
    input.previousBucketCount = 2;
    Check(inactiveBucketToken == BuildSmokeStaticBucketWorkPlanInputToken(input),
        "static bucket work input token ignores previous cache entries for inactive buckets");
    buckets[1].routeRecordIndex = 99;
    buckets[1].activeReasonFlags = RT_SMOKE_STATIC_ACTIVE_VISIBLE | RT_SMOKE_STATIC_ACTIVE_FORCE_INCLUDE;
    Check(inactiveBucketToken == BuildSmokeStaticBucketWorkPlanInputToken(input),
        "static bucket work input token ignores inactive route and active-reason metadata");
    buckets[1].routeRecordIndex = 1;
    buckets[1].activeReasonFlags = 0;
    buckets[1].hasBlas = false;
    buckets[1].activeSurfaceCount = 7;
    buckets[1].activeVertexCount = 48;
    buckets[1].activeIndexCount = 96;
    buckets[1].activeTriangleCount = 32;
    Check(inactiveBucketToken == BuildSmokeStaticBucketWorkPlanInputToken(input),
        "static bucket work input token ignores inactive active counts and BLAS readiness");
    buckets[1].hasBlas = true;
    buckets[1].activeSurfaceCount = 0;
    buckets[1].activeVertexCount = 12;
    buckets[1].activeIndexCount = 36;
    buckets[1].activeTriangleCount = 12;

    buckets[1].active = true;
    input.previousBucketCount = 1;
    input.maxBuildRecords = 1;
    const uint64_t cappedPreviousToken = BuildSmokeStaticBucketWorkPlanInputToken(input);
    previousBuckets[1].bucketKey = 20;
    input.previousBucketCount = 2;
    Check(cappedPreviousToken == BuildSmokeStaticBucketWorkPlanInputToken(input),
        "static bucket work input token ignores previous cache entries after build observation cap");
    const RtSmokeStaticBucketWorkPlan cappedPreviousPlan =
        BuildSmokeStaticBucketWorkPlan(input);
    const RtSmokeStaticBucketWorkPlanSnapshot cappedPreviousSnapshot =
        CaptureSmokeStaticBucketWorkPlanSnapshot(input);
    const RtSmokeStaticBucketWorkPlan cappedPreviousSnapshotPlan =
        BuildSmokeStaticBucketWorkPlan(cappedPreviousSnapshot);
    Check(cappedPreviousSnapshot.previousBuckets.size() == 1 &&
        cappedPreviousSnapshot.previousBuckets[0].bucketKey == 10 &&
        BuildSmokeStaticBucketWorkPlanInputToken(cappedPreviousSnapshot) == cappedPreviousToken &&
        cappedPreviousSnapshotPlan.planSignature == cappedPreviousPlan.planSignature,
        "static bucket work snapshot drops previous cache rows after build observation cap");

    input.maxBuildRecords = 0;
    Check(restoredToken != BuildSmokeStaticBucketWorkPlanInputToken(input),
        "static bucket work input token tracks previous cache entries when build observations are uncapped");
    const uint64_t uncappedToken = BuildSmokeStaticBucketWorkPlanInputToken(input);
    const RtSmokeStaticBucketWorkPlan uncappedPlan = BuildSmokeStaticBucketWorkPlan(input);
    RtSmokeStaticBucketWorkPlanInput negativeCapInput = input;
    negativeCapInput.maxBucketRecords = -2;
    negativeCapInput.maxRouteRecords = -3;
    negativeCapInput.maxBuildRecords = -4;
    const RtSmokeStaticBucketWorkPlan negativeCapPlan = BuildSmokeStaticBucketWorkPlan(negativeCapInput);
    Check(uncappedToken == BuildSmokeStaticBucketWorkPlanInputToken(negativeCapInput) &&
        uncappedPlan.planSignature == negativeCapPlan.planSignature &&
        uncappedPlan.buildObservationPlan.emittedObservations == negativeCapPlan.buildObservationPlan.emittedObservations,
        "static bucket work input token normalizes non-positive record caps as uncapped");

    RtSmokeStaticBucketWorkPlanInput emptyInput = input;
    emptyInput.buckets = nullptr;
    emptyInput.bucketCount = 0;
    emptyInput.previousBuckets = nullptr;
    emptyInput.previousBucketCount = 0;
    const uint64_t emptyToken = BuildSmokeStaticBucketWorkPlanInputToken(emptyInput);
    const RtSmokeStaticBucketWorkPlan emptyPlan = BuildSmokeStaticBucketWorkPlan(emptyInput);
    emptyInput.bucketCount = 2;
    const uint64_t nullPositiveCountToken = BuildSmokeStaticBucketWorkPlanInputToken(emptyInput);
    const RtSmokeStaticBucketWorkPlan nullPositiveCountPlan = BuildSmokeStaticBucketWorkPlan(emptyInput);
    emptyInput.buckets = buckets;
    emptyInput.bucketCount = -2;
    const uint64_t negativeCountToken = BuildSmokeStaticBucketWorkPlanInputToken(emptyInput);
    const RtSmokeStaticBucketWorkPlan negativeCountPlan = BuildSmokeStaticBucketWorkPlan(emptyInput);
    Check(emptyToken == nullPositiveCountToken &&
        emptyToken == negativeCountToken &&
        emptyPlan.planSignature == nullPositiveCountPlan.planSignature &&
        emptyPlan.planSignature == negativeCountPlan.planSignature,
        "static bucket work input token normalizes absent bucket records as empty");

    input.previousBucketCount = 1;
    const RtSmokeStaticBucketWorkPlanSnapshot snapshot =
        CaptureSmokeStaticBucketWorkPlanSnapshot(input);
    const uint64_t snapshotToken = BuildSmokeStaticBucketWorkPlanInputToken(snapshot);
    buckets[0].bucketKey = 99;
    previousBuckets[0].hasBlas = false;
    Check(snapshotToken == BuildSmokeStaticBucketWorkPlanInputToken(snapshot) &&
        snapshotToken == restoredToken &&
        BuildSmokeStaticBucketWorkPlanInputToken(input) != snapshotToken,
        "static bucket work snapshot token is immutable after source mutation");
}

void TestAsyncStaticBucketWorkPlanning()
{
    RtSmokeStaticTlasBucketObservation bucket;
    bucket.bucketKey = 88;
    bucket.resident = true;
    bucket.active = true;
    bucket.hasBlas = true;
    bucket.routeRecordIndex = 0;
    bucket.residentVertexCount = 3;
    bucket.residentIndexCount = 3;
    bucket.residentTriangleCount = 1;
    bucket.activeVertexCount = 3;
    bucket.activeIndexCount = 3;
    bucket.activeTriangleCount = 1;

    RtSmokeStaticBucketWorkPlanInput input;
    input.buckets = &bucket;
    input.bucketCount = 1;
    input.geometryContentSignature = 700;
    input.materialGeneration = 800;
    input.totalVertexCount = 3;
    input.totalIndexCount = 3;
    input.totalTriangleCount = 1;
    input.submitBuilds = true;
    const RtSmokeStaticBucketWorkPlanSnapshot snapshot =
        CaptureSmokeStaticBucketWorkPlanSnapshot(input);

    RtPathTraceCpuWorkState state;
    RtPathTraceCpuWorkGeneration generation;
    generation.frameIndex = 42;
    generation.sceneGeneration = 1;
    generation.geometryGeneration = input.geometryContentSignature;
    generation.materialGeneration = input.materialGeneration;
    generation.lightGeneration = BuildSmokeStaticBucketWorkPlanInputToken(snapshot);
    RtPathTraceCpuWorkPublishSnapshot(state, generation);

    std::future<RtSmokeStaticBucketWorkPlanTimedResult> future = std::async(
        std::launch::async,
        [snapshot]() {
            return BuildSmokeStaticBucketWorkPlanTimedResult(snapshot);
        });

    const RtPathTraceCpuWorkFrameDecision lateDecision =
        RtPathTraceCpuWorkAcceptLatest(state, generation, nullptr, true);
    Check(lateDecision.lateFallback && lateDecision.syncFallback,
        "late async static bucket work requests synchronous fallback");

    const RtSmokeStaticBucketWorkPlanTimedResult timedResult = future.get();
    RtPathTraceCpuWorkResultEnvelope envelope;
    envelope.completed = true;
    envelope.generation = generation;
    envelope.timing.workerExecutionMs = static_cast<double>(timedResult.planningTimeMicros) / 1000.0;
    RtPathTraceCpuWorkPublishCompletedResult(state, envelope);
    const RtPathTraceCpuWorkFrameDecision acceptDecision =
        RtPathTraceCpuWorkAcceptLatest(state, generation, nullptr, true);
    Check(acceptDecision.accepted &&
        timedResult.plan.bucketBlasPlan.emittedRecords == 1 &&
        state.acceptedResultCount == 1,
        "matching async static bucket work generation is accepted");

    RtPathTraceCpuWorkResultEnvelope staleEnvelope = envelope;
    staleEnvelope.generation.lightGeneration ^= 0x1000ull;
    const RtPathTraceCpuWorkFrameDecision staleDecision =
        RtPathTraceCpuWorkAcceptLatest(state, generation, &staleEnvelope, true);
    Check(staleDecision.staleRejected && staleDecision.syncFallback,
        "stale async static bucket work generation is rejected");
}

void TestStaticBucketAssignmentPlan()
{
    auto makeSurface = [](
        uint64_t key,
        uint32_t recordIndex,
        int area,
        int vertexOffset,
        int vertexCount,
        int indexOffset,
        int indexCount,
        int triangleOffset,
        int triangleCount)
    {
        RtSmokeStaticBucketAssignmentSurface surface;
        surface.surfaceKey = key;
        surface.sourceRecordIndex = recordIndex;
        surface.portalArea = area;
        surface.range.vertexOffset = vertexOffset;
        surface.range.vertexCount = vertexCount;
        surface.range.indexOffset = indexOffset;
        surface.range.indexCount = indexCount;
        surface.range.triangleOffset = triangleOffset;
        surface.range.triangleCount = triangleCount;
        surface.valid = true;
        surface.active = true;
        return surface;
    };

    RtSmokeStaticBucketAssignmentSurface surfaces[3];
    surfaces[0] = makeSurface(30, 2, 1, 8, 3, 12, 3, 4, 1);
    surfaces[1] = makeSurface(10, 0, 0, 0, 4, 0, 6, 0, 2);
    surfaces[2] = makeSurface(20, 1, 0, 4, 4, 6, 6, 2, 2);

    RtSmokeStaticBucketAssignmentPlanDesc desc;
    desc.surfaces = surfaces;
    desc.surfaceCount = 3;
    desc.worldGeneration = 11;
    desc.sourceGeneration = 12;
    desc.storageGeneration = 13;
    desc.portalAreaCount = 2;
    desc.maxVerticesPerBucket = 6;
    desc.maxIndexesPerBucket = 9;
    desc.maxTrianglesPerBucket = 3;
    const RtSmokeStaticBucketAssignmentPlan plan =
        BuildSmokeStaticBucketAssignmentPlan(desc);

    Check(
        plan.exactCoverage &&
            plan.stats.inputSurfaces == 3 &&
            plan.stats.assignedSurfaces == 3 &&
            plan.stats.assignedPrimitives == 5 &&
            plan.stats.buckets == 3 &&
            plan.stats.splitBuckets == 1,
        "static bucket assignment covers every primitive and splits deterministically");
    Check(
        plan.assignments.size() == 3 &&
            plan.assignments[0].surfaceKey == 10 &&
            plan.assignments[1].surfaceKey == 20 &&
            plan.assignments[2].surfaceKey == 30 &&
            plan.assignments[0].localPrimitiveOffset == 0 &&
            plan.assignments[1].localPrimitiveOffset == 0 &&
            plan.assignments[2].localPrimitiveOffset == 0,
        "static bucket assignment order is area then stable surface key");
    Check(
        plan.buckets.size() == 3 &&
            plan.buckets[0].portalArea == 0 &&
            plan.buckets[0].splitIndex == 0 &&
            plan.buckets[1].portalArea == 0 &&
            plan.buckets[1].splitIndex == 1 &&
            plan.buckets[2].portalArea == 1 &&
            plan.buckets[2].splitIndex == 0,
        "static bucket keys retain portal area and size split tuple");

    RtSmokeStaticBucketAssignmentSurface permuted[3] = {
        surfaces[2],
        surfaces[0],
        surfaces[1]
    };
    RtSmokeStaticBucketAssignmentPlanDesc permutedDesc = desc;
    permutedDesc.surfaces = permuted;
    const RtSmokeStaticBucketAssignmentPlan permutedPlan =
        BuildSmokeStaticBucketAssignmentPlan(permutedDesc);
    Check(
        permutedPlan.planSignature == plan.planSignature &&
            permutedPlan.buckets.size() == plan.buckets.size() &&
            permutedPlan.assignments.size() == plan.assignments.size(),
        "static bucket assignment is independent of producer enumeration order");

    RtSmokeStaticBucketAssignmentPlanDesc changedGenerationDesc = desc;
    changedGenerationDesc.storageGeneration = 14;
    const RtSmokeStaticBucketAssignmentPlan changedGenerationPlan =
        BuildSmokeStaticBucketAssignmentPlan(changedGenerationDesc);
    Check(
        changedGenerationPlan.planSignature != plan.planSignature &&
            changedGenerationPlan.buckets[0].bucketKey !=
                plan.buckets[0].bucketKey,
        "static bucket identity includes storage generation");

    std::vector<HarnessSmokeVertex> packSourceVertices(11);
    for (size_t vertexIndex = 0;
        vertexIndex < packSourceVertices.size();
        ++vertexIndex)
    {
        packSourceVertices[vertexIndex].position[0] =
            static_cast<float>(vertexIndex);
        packSourceVertices[vertexIndex].position[3] = 1.0f;
    }
    std::vector<uint32_t> packSourceIndexes = {
        0, 1, 2, 0, 2, 3,
        4, 5, 6, 4, 6, 7,
        8, 9, 10
    };
    std::vector<uint32_t> packSourceClasses = {
        101, 102, 201, 202, 301
    };
    std::vector<uint32_t> packSourceMaterials = {
        11, 12, 21, 22, 31
    };
    RtSmokeStaticBucketGeometryPackDesc packDesc;
    packDesc.assignmentPlan = &plan;
    packDesc.vertices = packSourceVertices.data();
    packDesc.vertexStride = sizeof(packSourceVertices[0]);
    packDesc.totalVertexCount =
        static_cast<int>(packSourceVertices.size());
    packDesc.indexes = packSourceIndexes.data();
    packDesc.totalIndexCount =
        static_cast<int>(packSourceIndexes.size());
    packDesc.triangleClasses = packSourceClasses.data();
    packDesc.triangleMaterials = packSourceMaterials.data();
    packDesc.totalTriangleCount =
        static_cast<int>(packSourceClasses.size());
    const RtSmokeStaticBucketGeometryPack pack =
        BuildSmokeStaticBucketGeometryPack(packDesc);
    Check(
        pack.exact &&
            pack.stats.packedBuckets == 3 &&
            pack.stats.packedSurfaces == 3 &&
            pack.stats.packedVertices == 11 &&
            pack.stats.packedIndexes == 15 &&
            pack.stats.packedTriangles == 5,
        "static bucket geometry pack preserves exact surface and primitive coverage");
    Check(
        pack.buckets.size() == 3 &&
            pack.buckets[0].range.vertexOffset == 0 &&
            pack.buckets[0].range.vertexCount == 4 &&
            pack.buckets[0].range.indexOffset == 0 &&
            pack.buckets[0].range.indexCount == 6 &&
            pack.buckets[0].range.triangleOffset == 0 &&
            pack.buckets[0].range.triangleCount == 2 &&
            pack.buckets[1].range.vertexOffset == 4 &&
            pack.buckets[1].range.indexOffset == 6 &&
            pack.buckets[1].range.triangleOffset == 2 &&
            pack.buckets[2].range.vertexOffset == 8 &&
            pack.buckets[2].range.indexOffset == 12 &&
            pack.buckets[2].range.triangleOffset == 4,
        "static bucket geometry pack emits contiguous bucket-local pooled ranges");
    Check(
        pack.indexes == packSourceIndexes &&
            pack.triangleClasses == packSourceClasses &&
            pack.triangleMaterials == packSourceMaterials,
        "static bucket geometry pack preserves rebased topology, class, and material identity");
    Check(
        pack.triangleIdentities.size() == 5 &&
            pack.triangleIdentities[0].surfaceKey == 10 &&
            pack.triangleIdentities[0].sourcePrimitiveIndex == 0 &&
            pack.triangleIdentities[1].surfaceKey == 10 &&
            pack.triangleIdentities[1].sourcePrimitiveIndex == 1 &&
            pack.triangleIdentities[2].surfaceKey == 20 &&
            pack.triangleIdentities[2].sourcePrimitiveIndex == 0 &&
            pack.triangleIdentities[3].sourcePrimitiveIndex == 1 &&
            pack.triangleIdentities[4].surfaceKey == 30 &&
            pack.triangleIdentities[4].sourcePrimitiveIndex == 0,
        "static bucket geometry pack preserves surface-local primitive identity");
    const uint32_t secondSurfaceSourceTriangleOffset =
        pack.surfaceRecords[1].flags >>
        RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_SOURCE_TRIANGLE_SHIFT;
    Check(
        secondSurfaceSourceTriangleOffset == 2 &&
            secondSurfaceSourceTriangleOffset +
                pack.triangleIdentities[3].sourcePrimitiveIndex == 3,
        "static bucket surface record plus local primitive recovers resident-source identity");
    const RtSmokeStaticBucketCanonicalAddressPlan canonicalAddressPlan =
        BuildSmokeStaticBucketCanonicalAddressPlan(pack);
    Check(
        canonicalAddressPlan.exact &&
            canonicalAddressPlan.stats.triangles == 5 &&
            canonicalAddressPlan.stats.mappedTriangles == 5 &&
            canonicalAddressPlan.stats.missingTriangles == 0 &&
            canonicalAddressPlan.addresses[0].instanceId ==
                RT_SMOKE_STATIC_BUCKET_INSTANCE_ID_NAMESPACE &&
            canonicalAddressPlan.addresses[0].
                sourceTriangleIndex == 0 &&
            canonicalAddressPlan.addresses[1].instanceId ==
                RT_SMOKE_STATIC_BUCKET_INSTANCE_ID_NAMESPACE &&
            canonicalAddressPlan.addresses[1].
                sourceTriangleIndex == 1 &&
            canonicalAddressPlan.addresses[2].instanceId ==
                (RT_SMOKE_STATIC_BUCKET_INSTANCE_ID_NAMESPACE | 1u) &&
            canonicalAddressPlan.addresses[2].
                sourceTriangleIndex == 2 &&
            canonicalAddressPlan.addresses[3].instanceId ==
                (RT_SMOKE_STATIC_BUCKET_INSTANCE_ID_NAMESPACE | 1u) &&
            canonicalAddressPlan.addresses[3].
                sourceTriangleIndex == 3 &&
            canonicalAddressPlan.addresses[4].instanceId ==
                (RT_SMOKE_STATIC_BUCKET_INSTANCE_ID_NAMESPACE | 2u) &&
            canonicalAddressPlan.addresses[4].
                sourceTriangleIndex == 4,
        "static bucket canonical replay address identifies one surface record plus source triangle per packed triangle");
    RtSmokeStaticBucketGeometryPack invalidCanonicalAddressPack = pack;
    invalidCanonicalAddressPack.surfaceRecords[1].triangleOffset = 0;
    const RtSmokeStaticBucketCanonicalAddressPlan
        invalidCanonicalAddressPlan =
            BuildSmokeStaticBucketCanonicalAddressPlan(
                invalidCanonicalAddressPack);
    Check(
        !invalidCanonicalAddressPlan.exact &&
            invalidCanonicalAddressPlan.stats.
                duplicateTriangleMappings == 2 &&
            invalidCanonicalAddressPlan.stats.missingTriangles == 2,
        "static bucket canonical replay address rejects overlapping surface triangle ownership");
    const std::vector<uint8_t> activeBucketMask = {
        1u, 1u, 1u
    };
    const std::vector<
        RtSmokeStaticBucketMonolithicSurfaceBinding>
        reorderedMonolithicBindings = {
            { 30u, 7u, 1u },
            { 10u, 100u, 2u },
            { 20u, 40u, 2u }
        };
    const RtSmokeStaticBucketMonolithicPrimitiveRemap
        monolithicPrimitiveRemap =
            BuildSmokeStaticBucketMonolithicPrimitiveRemap(
                pack,
                activeBucketMask,
                reorderedMonolithicBindings);
    Check(
        monolithicPrimitiveRemap.exact &&
            monolithicPrimitiveRemap.stats.activeBuckets == 3 &&
            monolithicPrimitiveRemap.stats.activeSurfaces == 3 &&
            monolithicPrimitiveRemap.stats.activeTriangles == 5 &&
            monolithicPrimitiveRemap.stats.matchedSurfaces == 3 &&
            monolithicPrimitiveRemap.stats.missingSurfaces == 0 &&
            monolithicPrimitiveRemap.stats.duplicateBindings == 0 &&
            monolithicPrimitiveRemap.stats.mappedTriangles == 5 &&
            monolithicPrimitiveRemap.stats.missingTriangles == 0 &&
            monolithicPrimitiveRemap.primitiveIndexes ==
                std::vector<uint32_t>(
                    { 100u, 101u, 40u, 41u, 7u }),
        "static bucket canonical surface keys remap local primitives into a differently ordered monolithic cache");
    std::vector<uint32_t> monolithicClasses(102u, 0u);
    std::vector<uint32_t> monolithicMaterialIndexes(
        102u,
        0u);
    for (size_t packedTriangleIndex = 0;
         packedTriangleIndex <
            monolithicPrimitiveRemap.
                primitiveIndexes.size();
         ++packedTriangleIndex)
    {
        const uint32_t monolithicPrimitiveIndex =
            monolithicPrimitiveRemap.
                primitiveIndexes[packedTriangleIndex];
        monolithicClasses[monolithicPrimitiveIndex] =
            pack.triangleClasses[packedTriangleIndex];
        monolithicMaterialIndexes[
            monolithicPrimitiveIndex] =
                pack.triangleMaterials[
                    packedTriangleIndex];
    }
    constexpr uint32_t emissiveStageOffMask =
        0x00040000u;
    const RtSmokeStaticBucketMonolithicStateOverlay
        exactStateOverlay =
            BuildSmokeStaticBucketMonolithicStateOverlay(
                pack,
                pack.triangleMaterials,
                monolithicPrimitiveRemap,
                monolithicClasses,
                monolithicMaterialIndexes,
                emissiveStageOffMask);
    Check(
        exactStateOverlay.exact &&
            exactStateOverlay.stats.mappedTriangles == 5 &&
            exactStateOverlay.stats.
                invalidMonolithicRanges == 0 &&
            exactStateOverlay.stats.classMismatches == 0 &&
            exactStateOverlay.stats.stageStateMismatches == 0 &&
            exactStateOverlay.stats.
                nonStageClassMismatches == 0 &&
            exactStateOverlay.stats.
                materialIndexMismatches == 0 &&
            exactStateOverlay.triangleClasses ==
                pack.triangleClasses &&
            exactStateOverlay.triangleMaterialIndexes ==
                pack.triangleMaterials,
        "static bucket live-state overlay recovers exact monolithic class and material rows after storage reordering");
    monolithicClasses[100] ^= emissiveStageOffMask;
    monolithicMaterialIndexes[40] ^= 1u;
    const RtSmokeStaticBucketMonolithicStateOverlay
        changedStateOverlay =
            BuildSmokeStaticBucketMonolithicStateOverlay(
                pack,
                pack.triangleMaterials,
                monolithicPrimitiveRemap,
                monolithicClasses,
                monolithicMaterialIndexes,
                emissiveStageOffMask);
    Check(
        changedStateOverlay.exact &&
            changedStateOverlay.stats.classMismatches == 1 &&
            changedStateOverlay.stats.stageStateMismatches == 1 &&
            changedStateOverlay.stats.
                nonStageClassMismatches == 0 &&
            changedStateOverlay.stats.
                materialIndexMismatches == 1 &&
            changedStateOverlay.triangleClasses[0] ==
                monolithicClasses[100] &&
            changedStateOverlay.triangleMaterialIndexes[2] ==
                monolithicMaterialIndexes[40],
        "static bucket live-state overlay reports and publishes runtime stage and material differences");
    std::vector<
        RtSmokeStaticBucketMonolithicSurfaceBinding>
        missingMonolithicBinding =
            reorderedMonolithicBindings;
    missingMonolithicBinding.pop_back();
    const RtSmokeStaticBucketMonolithicPrimitiveRemap
        rejectedMonolithicPrimitiveRemap =
            BuildSmokeStaticBucketMonolithicPrimitiveRemap(
                pack,
                activeBucketMask,
                missingMonolithicBinding);
    Check(
        !rejectedMonolithicPrimitiveRemap.exact &&
            rejectedMonolithicPrimitiveRemap.stats.
                missingSurfaces == 1 &&
            rejectedMonolithicPrimitiveRemap.stats.
                missingTriangles == 2,
        "static bucket monolithic primitive remap rejects a missing active canonical surface");
    Check(
        !IsSmokeStaticBucketAuditReady(
            true,
            false,
            true,
            false) &&
        !IsSmokeStaticBucketAuditReady(
            true,
            true,
            true,
            false) &&
        IsSmokeStaticBucketAuditReady(
            true,
            true,
            true,
            true),
        "static bucket full-resident audit remains armed until portal and active publications are exact");
    Check(
        !IsSmokeStaticBucketAuditReady(
            false,
            true,
            false,
            false) &&
        IsSmokeStaticBucketAuditReady(
            true,
            true,
            false,
            false),
        "static bucket portal-only audit requires an explicit request and exact portal publication");
    Check(
        pack.surfaceRecords.size() == 3 &&
            pack.buckets[0].firstSurfaceRecord == 0 &&
            pack.buckets[0].surfaceRecordCount == 1 &&
            pack.buckets[1].firstSurfaceRecord == 1 &&
            pack.buckets[1].surfaceRecordCount == 1 &&
            pack.buckets[2].firstSurfaceRecord == 2 &&
            pack.buckets[2].surfaceRecordCount == 1 &&
            pack.surfaceRecords[0].indexOffset == 0 &&
            pack.surfaceRecords[0].triangleOffset == 0 &&
            pack.surfaceRecords[0].triangleCount == 2 &&
            pack.surfaceRecords[1].indexOffset == 6 &&
            pack.surfaceRecords[1].triangleOffset == 2 &&
            pack.surfaceRecords[1].triangleCount == 2 &&
            pack.surfaceRecords[2].indexOffset == 12 &&
            pack.surfaceRecords[2].triangleOffset == 4 &&
            pack.surfaceRecords[2].triangleCount == 1 &&
            pack.surfaceRecords[0].flags ==
                ((0u <<
                    RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_SOURCE_TRIANGLE_SHIFT) |
                    RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_VALID) &&
            pack.surfaceRecords[1].flags ==
                ((2u <<
                    RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_SOURCE_TRIANGLE_SHIFT) |
                    RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_VALID) &&
            pack.surfaceRecords[2].flags ==
                ((4u <<
                    RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_SOURCE_TRIANGLE_SHIFT) |
                    RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_VALID),
        "static bucket geometry pack publishes exact records with source draw-order offsets");
    Check(
        ValidateSmokeStaticBucketClassMetadataLayout(pack) &&
            pack.surfaceRecordWordOffset ==
                pack.triangleClasses.size() &&
            pack.staticClassMetadataWords.size() == 17 &&
            std::equal(
                pack.triangleClasses.begin(),
                pack.triangleClasses.end(),
                pack.staticClassMetadataWords.begin()) &&
            pack.staticClassMetadataWords[5] == 0 &&
            pack.staticClassMetadataWords[6] == 0 &&
            pack.staticClassMetadataWords[7] == 2 &&
            pack.staticClassMetadataWords[8] ==
                RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_VALID &&
            pack.staticClassMetadataWords[9] == 6 &&
            pack.staticClassMetadataWords[10] == 2 &&
            pack.staticClassMetadataWords[11] == 2 &&
            pack.staticClassMetadataWords[12] ==
                ((2u <<
                    RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_SOURCE_TRIANGLE_SHIFT) |
                    RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_VALID) &&
            pack.staticClassMetadataWords[13] == 12 &&
            pack.staticClassMetadataWords[14] == 4 &&
            pack.staticClassMetadataWords[15] == 1 &&
            pack.staticClassMetadataWords[16] ==
                ((4u <<
                    RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_SOURCE_TRIANGLE_SHIFT) |
                    RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_VALID),
        "static bucket t5 metadata preserves triangle rows and four-word records with source draw order");

    RtSmokeStaticBucketGeometryPack corruptedMetadataPack = pack;
    corruptedMetadataPack.staticClassMetadataWords[
        corruptedMetadataPack.surfaceRecordWordOffset + 6u] ^= 1u;
    Check(
        !ValidateSmokeStaticBucketClassMetadataLayout(
            corruptedMetadataPack),
        "static bucket t5 metadata validation rejects a corrupted surface-record tail");
    corruptedMetadataPack = pack;
    corruptedMetadataPack.staticClassMetadataWords[1] ^= 1u;
    Check(
        !ValidateSmokeStaticBucketClassMetadataLayout(
            corruptedMetadataPack),
        "static bucket t5 metadata validation rejects a changed triangle-class prefix");

    RtSmokeStaticBucketAssignmentSurface multiGeometrySurfaces[3] = {
        surfaces[0],
        surfaces[1],
        surfaces[2]
    };
    multiGeometrySurfaces[2].portalArea = 1;
    RtSmokeStaticBucketAssignmentPlanDesc multiGeometryDesc = desc;
    multiGeometryDesc.surfaces = multiGeometrySurfaces;
    multiGeometryDesc.maxVerticesPerBucket = 8;
    multiGeometryDesc.maxIndexesPerBucket = 12;
    multiGeometryDesc.maxTrianglesPerBucket = 4;
    const RtSmokeStaticBucketAssignmentPlan multiGeometryPlan =
        BuildSmokeStaticBucketAssignmentPlan(multiGeometryDesc);
    RtSmokeStaticBucketGeometryPackDesc multiGeometryPackDesc =
        packDesc;
    multiGeometryPackDesc.assignmentPlan = &multiGeometryPlan;
    const RtSmokeStaticBucketGeometryPack multiGeometryPack =
        BuildSmokeStaticBucketGeometryPack(multiGeometryPackDesc);
    Check(
        multiGeometryPack.exact &&
            multiGeometryPack.buckets.size() == 2 &&
            multiGeometryPack.surfaceRecords.size() == 3 &&
            multiGeometryPack.buckets[0].firstSurfaceRecord == 0 &&
            multiGeometryPack.buckets[0].surfaceRecordCount == 1 &&
            multiGeometryPack.buckets[1].firstSurfaceRecord == 1 &&
            multiGeometryPack.buckets[1].surfaceRecordCount == 2,
        "static bucket records remain bucket-contiguous for multi-geometry BLAS identity");
    const RtSmokeStaticBucketSurfaceAddressPlan secondBucketAddress =
        BuildSmokeStaticBucketSurfaceAddressPlan(
            multiGeometryPack.buckets[1].firstSurfaceRecord,
            multiGeometryPack.buckets[1].surfaceRecordCount,
            static_cast<uint32_t>(
                multiGeometryPack.surfaceRecords.size()));
    uint32_t secondBucketSurfaceBase = 0;
    Check(
        secondBucketAddress.valid &&
            TryDecodeSmokeStaticBucketSurfaceBaseInstanceId(
                secondBucketAddress.instanceId,
                secondBucketSurfaceBase) &&
            secondBucketSurfaceBase == 1 &&
            secondBucketSurfaceBase + 1 == 2 &&
            multiGeometryPack.surfaceRecords[
                secondBucketSurfaceBase + 1].triangleOffset == 4 &&
            multiGeometryPack.surfaceRecords[
                secondBucketSurfaceBase + 1].indexOffset == 12,
        "static bucket InstanceID plus GeometryIndex resolves the second surface record");
    const RtSmokeStaticBucketBlasGeometryPlan
        secondBucketGeometryPlan =
            BuildSmokeStaticBucketBlasGeometryPlan(
                multiGeometryPack,
                multiGeometryPack.buckets[1]);
    Check(
        secondBucketGeometryPlan.exact &&
            secondBucketGeometryPlan.invalidSurfaceRecords == 0 &&
            secondBucketGeometryPlan.geometries.size() == 2 &&
            secondBucketGeometryPlan.geometries[0].
                indexByteOffset == 6 * sizeof(uint32_t) &&
            secondBucketGeometryPlan.geometries[0].
                indexCount == 6 &&
            secondBucketGeometryPlan.geometries[0].
                triangleOffset == 2 &&
            secondBucketGeometryPlan.geometries[0].
                triangleCount == 2 &&
            secondBucketGeometryPlan.geometries[1].
                indexByteOffset == 12 * sizeof(uint32_t) &&
            secondBucketGeometryPlan.geometries[1].
                indexCount == 3 &&
            secondBucketGeometryPlan.geometries[1].
                triangleOffset == 4 &&
            secondBucketGeometryPlan.geometries[1].
                triangleCount == 1,
        "static bucket BLAS geometry plan preserves ordered per-surface ranges");
    const RtSmokeStaticBucketResolvedGeometryAddress
        secondSurfaceResolvedAddress =
            BuildSmokeStaticBucketResolvedGeometryAddress(
                multiGeometryPack,
                secondBucketAddress.instanceId,
                1,
                0);
    Check(
        secondSurfaceResolvedAddress.valid &&
            secondSurfaceResolvedAddress.surfaceRecordIndex == 2 &&
            secondSurfaceResolvedAddress.triangleIndex == 4 &&
            secondSurfaceResolvedAddress.sourceTriangleIndex == 4 &&
            secondSurfaceResolvedAddress.indexOffset == 12 &&
            secondSurfaceResolvedAddress.triangleCount == 1 &&
            secondSurfaceResolvedAddress.vertexIndexes[0] ==
                multiGeometryPack.indexes[12] &&
            secondSurfaceResolvedAddress.vertexIndexes[1] ==
                multiGeometryPack.indexes[13] &&
            secondSurfaceResolvedAddress.vertexIndexes[2] ==
                multiGeometryPack.indexes[14],
        "static bucket InstanceID plus GeometryIndex plus PrimitiveIndex resolves one exact geometry address");
    const RtSmokeStaticBucketSurfaceAddressPlan
        canonicalSecondSurfaceAddress =
            BuildSmokeStaticBucketSurfaceAddressPlan(
                secondSurfaceResolvedAddress.surfaceRecordIndex,
                1,
                static_cast<uint32_t>(
                    multiGeometryPack.surfaceRecords.size()));
    const RtSmokeStaticBucketSurfaceRecord&
        canonicalSecondSurfaceRecord =
            multiGeometryPack.surfaceRecords[
                secondSurfaceResolvedAddress.surfaceRecordIndex];
    const uint32_t canonicalSourceTriangleOffset =
        canonicalSecondSurfaceRecord.flags >>
        RT_SMOKE_STATIC_BUCKET_SURFACE_RECORD_SOURCE_TRIANGLE_SHIFT;
    const uint32_t canonicalSourceLocalPrimitive =
        secondSurfaceResolvedAddress.sourceTriangleIndex -
        canonicalSourceTriangleOffset;
    const RtSmokeStaticBucketResolvedGeometryAddress
        canonicalSecondSurfaceResolvedAddress =
            BuildSmokeStaticBucketResolvedGeometryAddress(
                multiGeometryPack,
                canonicalSecondSurfaceAddress.instanceId,
                0,
                canonicalSourceLocalPrimitive);
    Check(
        canonicalSecondSurfaceAddress.valid &&
            canonicalSecondSurfaceResolvedAddress.valid &&
            canonicalSecondSurfaceResolvedAddress.surfaceRecordIndex ==
                secondSurfaceResolvedAddress.surfaceRecordIndex &&
            canonicalSecondSurfaceResolvedAddress.triangleIndex ==
                secondSurfaceResolvedAddress.triangleIndex &&
            canonicalSecondSurfaceResolvedAddress.sourceTriangleIndex ==
                secondSurfaceResolvedAddress.sourceTriangleIndex &&
            canonicalSecondSurfaceResolvedAddress.indexOffset ==
                secondSurfaceResolvedAddress.indexOffset &&
            canonicalSecondSurfaceResolvedAddress.vertexIndexes[0] ==
                secondSurfaceResolvedAddress.vertexIndexes[0] &&
            canonicalSecondSurfaceResolvedAddress.vertexIndexes[1] ==
                secondSurfaceResolvedAddress.vertexIndexes[1] &&
            canonicalSecondSurfaceResolvedAddress.vertexIndexes[2] ==
                secondSurfaceResolvedAddress.vertexIndexes[2],
        "static bucket liquid candidate canonical surface and source triangle recover the exact geometry address");
    Check(
        !BuildSmokeStaticBucketResolvedGeometryAddress(
            multiGeometryPack,
            secondBucketAddress.instanceId,
            2,
            0).valid &&
        !BuildSmokeStaticBucketResolvedGeometryAddress(
            multiGeometryPack,
            secondBucketAddress.instanceId,
            1,
            1).valid &&
        !BuildSmokeStaticBucketResolvedGeometryAddress(
            multiGeometryPack,
            0,
            1,
            0).valid,
        "static bucket geometry address rejects out-of-range geometry, primitive, and namespace tuples");

    RtSmokeStaticBucketAssignmentPlan reorderedSourcePlan = plan;
    std::swap(
        reorderedSourcePlan.assignments[0].sourceRange,
        reorderedSourcePlan.assignments[1].sourceRange);
    RtSmokeStaticBucketGeometryPackDesc reorderedSourcePackDesc =
        packDesc;
    reorderedSourcePackDesc.assignmentPlan = &reorderedSourcePlan;
    const RtSmokeStaticBucketGeometryPack reorderedSourcePack =
        BuildSmokeStaticBucketGeometryPack(
            reorderedSourcePackDesc);
    const RtSmokeStaticBucketSurfaceAddressPlan
        reorderedSourceAddress =
            BuildSmokeStaticBucketSurfaceAddressPlan(
                reorderedSourcePack.buckets[0].
                    firstSurfaceRecord,
                reorderedSourcePack.buckets[0].
                    surfaceRecordCount,
                static_cast<uint32_t>(
                    reorderedSourcePack.surfaceRecords.size()));
    const RtSmokeStaticBucketResolvedGeometryAddress
        reorderedSourceResolvedAddress =
            BuildSmokeStaticBucketResolvedGeometryAddress(
                reorderedSourcePack,
                reorderedSourceAddress.instanceId,
                0,
                1);
    Check(
        reorderedSourcePack.exact &&
            reorderedSourceResolvedAddress.valid &&
            reorderedSourceResolvedAddress.triangleIndex == 1 &&
            reorderedSourceResolvedAddress.sourceTriangleIndex == 3,
        "static bucket geometry address preserves the monolithic primitive sort key after bucket reordering");

    Check(
        IsSmokeStaticBucketPrimaryOpaqueProbeSupported(
            RT_SMOKE_STATIC_BUCKET_ROUTE_PRIMARY_OPAQUE_PROBE,
            true,
            2,
            true,
            false),
        "static bucket primary probe accepts only the instrumented status isolate");
    Check(
        !IsSmokeStaticBucketPrimaryOpaqueProbeSupported(
            RT_SMOKE_STATIC_BUCKET_ROUTE_PRODUCTION,
            true,
            2,
            true,
            false) &&
        !IsSmokeStaticBucketPrimaryOpaqueProbeSupported(
            RT_SMOKE_STATIC_BUCKET_ROUTE_PRIMARY_OPAQUE_PROBE,
            true,
            17,
            true,
            false) &&
        !IsSmokeStaticBucketPrimaryOpaqueProbeSupported(
            RT_SMOKE_STATIC_BUCKET_ROUTE_PRIMARY_OPAQUE_PROBE,
            true,
            19,
            true,
            false) &&
        !IsSmokeStaticBucketPrimaryOpaqueProbeSupported(
            RT_SMOKE_STATIC_BUCKET_ROUTE_PRIMARY_OPAQUE_PROBE,
            true,
            24,
            true,
            false) &&
        !IsSmokeStaticBucketPrimaryOpaqueProbeSupported(
            RT_SMOKE_STATIC_BUCKET_ROUTE_PRIMARY_OPAQUE_PROBE,
            true,
            18,
            true,
            false) &&
        !IsSmokeStaticBucketPrimaryOpaqueProbeSupported(
            RT_SMOKE_STATIC_BUCKET_ROUTE_PRIMARY_OPAQUE_PROBE,
            true,
            25,
            true,
            false) &&
        !IsSmokeStaticBucketPrimaryOpaqueProbeSupported(
            RT_SMOKE_STATIC_BUCKET_ROUTE_PRIMARY_OPAQUE_PROBE,
            true,
            2,
            false,
            false) &&
        !IsSmokeStaticBucketPrimaryOpaqueProbeSupported(
            RT_SMOKE_STATIC_BUCKET_ROUTE_PRIMARY_OPAQUE_PROBE,
            true,
            2,
            true,
            true),
        "static bucket primary probe rejects production, unsupported views, uninstrumented, and GI-enabled routes");
    Check(
        !IsSmokeStaticBucketCleanDiSecondaryIsolationSupported(
            RT_SMOKE_STATIC_BUCKET_ROUTE_PRIMARY_OPAQUE_PROBE,
            true,
            16,
            true,
            false,
            false,
            false,
            1),
        "static bucket view-16 isolation rejects the failed pipeline-only stage one");
    Check(
        !IsSmokeStaticBucketCleanDiSecondaryIsolationSupported(
            RT_SMOKE_STATIC_BUCKET_ROUTE_PRIMARY_OPAQUE_PROBE,
            true,
            16,
            true,
            false,
            false,
            false,
            2),
        "static bucket view-16 isolation rejects stage two after stage-one device removal");
    bool secondaryIsolationStagesRejected = true;
    for (int stage = 3; stage <= 6; ++stage)
    {
        secondaryIsolationStagesRejected &=
            !IsSmokeStaticBucketCleanDiSecondaryIsolationSupported(
                RT_SMOKE_STATIC_BUCKET_ROUTE_PRIMARY_OPAQUE_PROBE,
                true,
                16,
                true,
                false,
                false,
                false,
                stage);
    }
    Check(
        secondaryIsolationStagesRejected,
        "static bucket unified-primary view-16 isolation rejects every secondary-consumer stage");
    Check(
        !IsSmokeStaticBucketCleanDiSecondaryIsolationSupported(
            RT_SMOKE_STATIC_BUCKET_ROUTE_PRODUCTION,
            true,
            16,
            true,
            false,
            false,
            false,
            1) &&
        !IsSmokeStaticBucketCleanDiSecondaryIsolationSupported(
            RT_SMOKE_STATIC_BUCKET_ROUTE_PRIMARY_OPAQUE_PROBE,
            false,
            16,
            true,
            false,
            false,
            false,
            1) &&
        !IsSmokeStaticBucketCleanDiSecondaryIsolationSupported(
            RT_SMOKE_STATIC_BUCKET_ROUTE_PRIMARY_OPAQUE_PROBE,
            true,
            2,
            true,
            false,
            false,
            false,
            1) &&
        !IsSmokeStaticBucketCleanDiSecondaryIsolationSupported(
            RT_SMOKE_STATIC_BUCKET_ROUTE_PRIMARY_OPAQUE_PROBE,
            true,
            16,
            false,
            false,
            false,
            false,
            1) &&
        !IsSmokeStaticBucketCleanDiSecondaryIsolationSupported(
            RT_SMOKE_STATIC_BUCKET_ROUTE_PRIMARY_OPAQUE_PROBE,
            true,
            16,
            true,
            true,
            false,
            false,
            1) &&
        !IsSmokeStaticBucketCleanDiSecondaryIsolationSupported(
            RT_SMOKE_STATIC_BUCKET_ROUTE_PRIMARY_OPAQUE_PROBE,
            true,
            16,
            true,
            false,
            true,
            false,
            1) &&
        !IsSmokeStaticBucketCleanDiSecondaryIsolationSupported(
            RT_SMOKE_STATIC_BUCKET_ROUTE_PRIMARY_OPAQUE_PROBE,
            true,
            16,
            true,
            false,
            false,
            true,
            1) &&
        !IsSmokeStaticBucketCleanDiSecondaryIsolationSupported(
            RT_SMOKE_STATIC_BUCKET_ROUTE_PRIMARY_OPAQUE_PROBE,
            true,
            16,
            true,
            false,
            false,
            false,
            0) &&
        !IsSmokeStaticBucketCleanDiSecondaryIsolationSupported(
            RT_SMOKE_STATIC_BUCKET_ROUTE_PRIMARY_OPAQUE_PROBE,
            true,
            16,
            true,
            false,
            false,
            false,
            7),
        "static bucket clean-DI primary isolation rejects unsafe routes, transmission, and out-of-range stages");
    const RtSmokeStaticBucketSecondaryIsolationDispatchPlan
        primaryPipelineOnlyPlan =
            BuildSmokeStaticBucketSecondaryIsolationDispatchPlan(
                true,
                true,
                1);
    const RtSmokeStaticBucketSecondaryIsolationDispatchPlan
        primaryDispatchOnlyPlan =
            BuildSmokeStaticBucketSecondaryIsolationDispatchPlan(
                true,
                true,
                2);
    Check(
        primaryPipelineOnlyPlan.active &&
            primaryPipelineOnlyPlan.stage == 1 &&
            !primaryPipelineOnlyPlan.primaryDispatch &&
            !primaryPipelineOnlyPlan.neeCachePrimaryUpdate &&
            !primaryPipelineOnlyPlan.transmissionPsr &&
            !primaryPipelineOnlyPlan.initial &&
            !primaryPipelineOnlyPlan.temporal &&
            !primaryPipelineOnlyPlan.spatial &&
            !primaryPipelineOnlyPlan.materialFeatureCompose &&
            primaryDispatchOnlyPlan.active &&
            primaryDispatchOnlyPlan.stage == 2 &&
            primaryDispatchOnlyPlan.primaryDispatch &&
            !primaryDispatchOnlyPlan.neeCachePrimaryUpdate &&
            !primaryDispatchOnlyPlan.transmissionPsr &&
            !primaryDispatchOnlyPlan.initial &&
            !primaryDispatchOnlyPlan.temporal &&
            !primaryDispatchOnlyPlan.spatial &&
            !primaryDispatchOnlyPlan.materialFeatureCompose,
        "static bucket primary isolate separates pipeline creation from DispatchRays");
    const RtSmokeStaticBucketSecondaryIsolationDispatchPlan
        activeStageZeroPlan =
            BuildSmokeStaticBucketSecondaryIsolationDispatchPlan(
                true,
                true,
                0);
    Check(
        activeStageZeroPlan.active &&
            activeStageZeroPlan.stage == 0 &&
            !activeStageZeroPlan.primaryDispatch &&
            !activeStageZeroPlan.neeCachePrimaryUpdate &&
            !activeStageZeroPlan.transmissionPsr &&
            !activeStageZeroPlan.initial &&
            !activeStageZeroPlan.temporal &&
            !activeStageZeroPlan.spatial &&
            !activeStageZeroPlan.materialFeatureCompose,
        "static bucket clean-DI active publication with an invalid stage fails closed before primary");
    const RtSmokeStaticBucketSecondaryIsolationDispatchPlan
        monolithicDispatchPlan =
            BuildSmokeStaticBucketSecondaryIsolationDispatchPlan(
                true,
                false,
                1);
    Check(
        !monolithicDispatchPlan.active &&
            monolithicDispatchPlan.primaryDispatch &&
            monolithicDispatchPlan.neeCachePrimaryUpdate &&
            monolithicDispatchPlan.transmissionPsr &&
            monolithicDispatchPlan.initial &&
            monolithicDispatchPlan.temporal &&
            monolithicDispatchPlan.spatial &&
            monolithicDispatchPlan.materialFeatureCompose,
        "static bucket clean-DI secondary dispatch plan leaves monolithic view 16 unchanged");

    RtSmokeStaticBucketGeometryPack invalidSurfaceFlagPack =
        multiGeometryPack;
    invalidSurfaceFlagPack.surfaceRecords[2].flags = 0;
    const RtSmokeStaticBucketBlasGeometryPlan invalidSurfaceFlagPlan =
        BuildSmokeStaticBucketBlasGeometryPlan(
            invalidSurfaceFlagPack,
            invalidSurfaceFlagPack.buckets[1]);
    Check(
        !invalidSurfaceFlagPlan.exact &&
            invalidSurfaceFlagPlan.invalidSurfaceRecords == 1 &&
            invalidSurfaceFlagPlan.geometries.empty(),
        "static bucket BLAS geometry plan rejects invalid surface flags");

    RtSmokeStaticBucketGeometryPack gappedSurfacePack =
        multiGeometryPack;
    ++gappedSurfacePack.surfaceRecords[2].indexOffset;
    const RtSmokeStaticBucketBlasGeometryPlan gappedSurfacePlan =
        BuildSmokeStaticBucketBlasGeometryPlan(
            gappedSurfacePack,
            gappedSurfacePack.buckets[1]);
    Check(
        !gappedSurfacePlan.exact &&
            gappedSurfacePlan.invalidSurfaceRecords == 1 &&
            gappedSurfacePlan.geometries.empty(),
        "static bucket BLAS geometry plan rejects gaps and reordered ranges");

    RtSmokeStaticBucketPackedRecord crossBucketRecordRange =
        multiGeometryPack.buckets[1];
    --crossBucketRecordRange.firstSurfaceRecord;
    const RtSmokeStaticBucketBlasGeometryPlan crossBucketPlan =
        BuildSmokeStaticBucketBlasGeometryPlan(
            multiGeometryPack,
            crossBucketRecordRange);
    Check(
        !crossBucketPlan.exact &&
            crossBucketPlan.invalidSurfaceRecords == 1 &&
            crossBucketPlan.geometries.empty(),
        "static bucket BLAS geometry plan rejects cross-bucket record bleed");

    RtSmokeStaticBucketGeometryPackDesc permutedPackDesc = packDesc;
    permutedPackDesc.assignmentPlan = &permutedPlan;
    const RtSmokeStaticBucketGeometryPack permutedPack =
        BuildSmokeStaticBucketGeometryPack(permutedPackDesc);
    Check(
        permutedPack.exact &&
            permutedPack.contentSignature == pack.contentSignature,
        "static bucket packed payload is independent of producer enumeration order");

    std::vector<uint32_t> invalidPackIndexes = packSourceIndexes;
    invalidPackIndexes[6] = 99;
    RtSmokeStaticBucketGeometryPackDesc invalidIndexPackDesc =
        packDesc;
    invalidIndexPackDesc.indexes = invalidPackIndexes.data();
    const RtSmokeStaticBucketGeometryPack invalidIndexPack =
        BuildSmokeStaticBucketGeometryPack(invalidIndexPackDesc);
    Check(
        !invalidIndexPack.exact &&
            invalidIndexPack.stats.indexRangeErrors == 1 &&
            invalidIndexPack.stats.packedBuckets == 2 &&
            invalidIndexPack.stats.packedSurfaces == 2 &&
            invalidIndexPack.surfaceRecords.size() == 2,
        "static bucket geometry pack rejects and rolls back an out-of-surface index");

    RtSmokeStaticBucketAssignmentPlan invalidOffsetPlan = plan;
    invalidOffsetPlan.assignments[1].localPrimitiveOffset = 1;
    RtSmokeStaticBucketGeometryPackDesc invalidOffsetPackDesc =
        packDesc;
    invalidOffsetPackDesc.assignmentPlan = &invalidOffsetPlan;
    const RtSmokeStaticBucketGeometryPack invalidOffsetPack =
        BuildSmokeStaticBucketGeometryPack(invalidOffsetPackDesc);
    Check(
        !invalidOffsetPack.exact &&
            invalidOffsetPack.stats.localPrimitiveOffsetErrors == 1,
        "static bucket geometry pack rejects mismatched bucket-local primitive offsets");

    RtSmokeStaticBucketAssignmentSurface exceptional[4];
    exceptional[0] = makeSurface(40, 0, -1, 0, 3, 0, 3, 0, 1);
    exceptional[1] = makeSurface(50, 1, 99, 3, 3, 3, 3, 1, 1);
    exceptional[2] = makeSurface(60, 2, 1, 6, 12, 6, 12, 2, 4);
    exceptional[3] = makeSurface(70, 3, 1, 18, 3, 18, 4, 6, 1);
    RtSmokeStaticBucketAssignmentPlanDesc exceptionalDesc = desc;
    exceptionalDesc.surfaces = exceptional;
    exceptionalDesc.surfaceCount = 4;
    const RtSmokeStaticBucketAssignmentPlan exceptionalPlan =
        BuildSmokeStaticBucketAssignmentPlan(exceptionalDesc);
    Check(
        !exceptionalPlan.exactCoverage &&
            exceptionalPlan.stats.assignedSurfaces == 3 &&
            exceptionalPlan.stats.unassignedAreaSurfaces == 1 &&
            exceptionalPlan.stats.invalidAreaSurfaces == 1 &&
            exceptionalPlan.stats.invalidRangeSurfaces == 1 &&
            exceptionalPlan.stats.oversizedSurfaces == 1 &&
            exceptionalPlan.stats.fallbackBuckets == 1,
        "static bucket assignment reports fallback, invalid, and oversized surfaces without silent drops");
    Check(
        exceptionalPlan.buckets.size() == 2 &&
            exceptionalPlan.buckets[0].portalArea ==
                RT_SMOKE_STATIC_BUCKET_FALLBACK_AREA &&
            exceptionalPlan.buckets[0].assignmentCount == 2 &&
            exceptionalPlan.buckets[1].oversized &&
            exceptionalPlan.buckets[1].assignmentCount == 1,
        "static bucket assignment keeps invalid-area surfaces in an explicit fallback bucket");

    RtSmokeStaticBucketAssignmentSurface duplicate[2] = {
        makeSurface(80, 0, 0, 0, 3, 0, 3, 0, 1),
        makeSurface(80, 1, 0, 3, 3, 3, 3, 1, 1)
    };
    RtSmokeStaticBucketAssignmentPlanDesc duplicateDesc = desc;
    duplicateDesc.surfaces = duplicate;
    duplicateDesc.surfaceCount = 2;
    const RtSmokeStaticBucketAssignmentPlan duplicatePlan =
        BuildSmokeStaticBucketAssignmentPlan(duplicateDesc);
    Check(
        !duplicatePlan.exactCoverage &&
            duplicatePlan.stats.duplicateSurfaces == 1 &&
            duplicatePlan.stats.assignedSurfaces == 1,
        "static bucket assignment rejects duplicate surface ownership");
}

void TestStaticActiveSetPlan()
{
    RtSmokeStaticTlasBucketObservation buckets[3];
    buckets[0].bucketKey = 100;
    buckets[0].resident = true;
    buckets[0].active = true;
    buckets[0].hasBlas = true;
    buckets[0].activeReasonFlags = RT_SMOKE_STATIC_ACTIVE_VISIBLE | RT_SMOKE_STATIC_ACTIVE_SELECTED_AREA;
    buckets[0].routeRecordIndex = 0;
    buckets[0].residentSurfaceCount = 1;
    buckets[0].residentVertexCount = 10;
    buckets[0].residentIndexCount = 30;
    buckets[0].residentTriangleCount = 10;
    buckets[0].activeSurfaceCount = 1;
    buckets[0].activeVertexCount = 10;
    buckets[0].activeIndexCount = 30;
    buckets[0].activeTriangleCount = 10;

    buckets[1].bucketKey = 200;
    buckets[1].resident = true;
    buckets[1].active = false;
    buckets[1].hasBlas = true;
    buckets[1].routeRecordIndex = 1;
    buckets[1].residentSurfaceCount = 1;
    buckets[1].residentVertexCount = 20;
    buckets[1].residentIndexCount = 60;
    buckets[1].residentTriangleCount = 20;

    buckets[2].bucketKey = 300;
    buckets[2].resident = true;
    buckets[2].active = true;
    buckets[2].hasBlas = true;
    buckets[2].activeReasonFlags = RT_SMOKE_STATIC_ACTIVE_FORCE_INCLUDE;
    buckets[2].routeRecordIndex = 2;
    buckets[2].residentSurfaceCount = 1;
    buckets[2].residentVertexCount = 5;
    buckets[2].residentIndexCount = 15;
    buckets[2].residentTriangleCount = 5;
    buckets[2].activeSurfaceCount = 1;
    buckets[2].activeVertexCount = 5;
    buckets[2].activeIndexCount = 15;
    buckets[2].activeTriangleCount = 5;

    RtSmokeStaticTlasActiveSetPlanDesc desc;
    desc.buckets = buckets;
    desc.bucketCount = 3;
    desc.hasStaticBlas = true;
    desc.firstInstanceId = 4;
    desc.instanceMask = 0x01;

    const RtSmokeStaticTlasActiveSetPlan monolithicPlan = BuildSmokeStaticTlasActiveSetPlan(desc);
    Check(monolithicPlan.emittedInstances == 1, "monolithic static active-set plan emits one static TLAS instance");
    Check(monolithicPlan.activeBuckets == 2 && monolithicPlan.inactiveResidentBuckets == 1, "static active-set plan separates active and resident buckets");
    Check(monolithicPlan.inactiveResidentGeometryIncluded && monolithicPlan.requiresBucketedStaticBlas, "monolithic static active-set plan reports inactive resident geometry included");

    desc.monolithicStaticBlas = false;
    const RtSmokeStaticTlasActiveSetPlan bucketPlan = BuildSmokeStaticTlasActiveSetPlan(desc);
    Check(bucketPlan.emittedInstances == 2 && bucketPlan.instances.size() == 2, "bucketed static active-set plan omits resident inactive geometry from TLAS");
    Check(bucketPlan.instances[0].kind == RT_SMOKE_PLAN_TLAS_STATIC_BUCKET_BLAS && bucketPlan.instances[0].meshHash == 100, "bucketed static active-set plan preserves first active bucket identity");
    Check(bucketPlan.instances[1].instanceId == 5 && bucketPlan.instances[1].flags == RT_SMOKE_STATIC_ACTIVE_FORCE_INCLUDE, "bucketed static active-set plan emits deterministic instance ids and reason flags");
    Check(!bucketPlan.inactiveResidentGeometryIncluded && !bucketPlan.requiresBucketedStaticBlas, "bucketed static active-set plan does not include inactive resident geometry");

    buckets[2].hasBlas = false;
    const RtSmokeStaticTlasActiveSetPlan missingBucketBlasPlan = BuildSmokeStaticTlasActiveSetPlan(desc);
    Check(missingBucketBlasPlan.activeBuckets == 2 &&
        missingBucketBlasPlan.emittedInstances == 1 &&
        bucketPlan.activeSetSignature == missingBucketBlasPlan.activeSetSignature &&
        bucketPlan.residentSetSignature == missingBucketBlasPlan.residentSetSignature &&
        bucketPlan.tlasInstanceSignature != missingBucketBlasPlan.tlasInstanceSignature,
        "bucketed static active-set plan waits for active bucket BLAS readiness");

    RtSmokeStaticTlasBucketObservation monolithicSurfaceDeltaBucket = buckets[0];
    monolithicSurfaceDeltaBucket.residentSurfaceCount = 3;
    monolithicSurfaceDeltaBucket.activeSurfaceCount = 2;
    monolithicSurfaceDeltaBucket.residentVertexCount = monolithicSurfaceDeltaBucket.activeVertexCount;
    monolithicSurfaceDeltaBucket.residentIndexCount = monolithicSurfaceDeltaBucket.activeIndexCount;
    monolithicSurfaceDeltaBucket.residentTriangleCount = monolithicSurfaceDeltaBucket.activeTriangleCount;
    RtSmokeStaticTlasActiveSetPlanDesc surfaceDeltaDesc;
    surfaceDeltaDesc.buckets = &monolithicSurfaceDeltaBucket;
    surfaceDeltaDesc.bucketCount = 1;
    surfaceDeltaDesc.monolithicStaticBlas = true;
    surfaceDeltaDesc.hasStaticBlas = true;
    const RtSmokeStaticTlasActiveSetPlan surfaceDeltaPlan = BuildSmokeStaticTlasActiveSetPlan(surfaceDeltaDesc);
    Check(surfaceDeltaPlan.inactiveResidentGeometryIncluded && surfaceDeltaPlan.requiresBucketedStaticBlas, "monolithic static active-set plan detects inactive retained surfaces even when vertex counts match");
}

void TestStaticBucketObservation()
{
    RtSmokeStaticTlasBucketObservationInput input;
    input.bucketKey = 900;
    input.routeRecordIndex = 7;
    input.activeReasonFlags = RT_SMOKE_STATIC_ACTIVE_VISIBLE | RT_SMOKE_STATIC_ACTIVE_SELECTED_AREA;
    input.vertexOffset = 3;
    input.indexOffset = 9;
    input.triangleOffset = 3;
    input.vertexCount = 12;
    input.indexCount = 36;
    input.triangleCount = 12;
    input.valid = true;
    input.seenThisFrame = true;
    input.hasBlas = true;

    RtSmokeStaticTlasBucketObservation observation;
    Check(BuildSmokeStaticTlasBucketObservation(input, observation), "static bucket observation accepts valid resident surface range");
    Check(observation.bucketKey == 900 && observation.routeRecordIndex == 7 &&
        observation.resident && observation.active && observation.hasBlas,
        "static bucket observation preserves resident active identity");
    Check(observation.residentSurfaceCount == 1 && observation.residentVertexCount == 12 &&
        observation.residentIndexCount == 36 && observation.residentTriangleCount == 12 &&
        observation.residentVertexOffset == 3 && observation.residentIndexOffset == 9 &&
        observation.residentTriangleOffset == 3,
        "static bucket observation records resident geometry ranges");
    Check(observation.activeSurfaceCount == 1 && observation.activeVertexCount == 12 &&
        observation.activeIndexCount == 36 && observation.activeTriangleCount == 12 &&
        observation.activeReasonFlags == input.activeReasonFlags,
        "static bucket observation records active counts and reason flags");

    input.seenThisFrame = false;
    RtSmokeStaticTlasBucketObservation inactiveObservation;
    Check(BuildSmokeStaticTlasBucketObservation(input, inactiveObservation), "static bucket observation accepts inactive resident surface range");
    Check(inactiveObservation.resident && !inactiveObservation.active &&
        inactiveObservation.activeSurfaceCount == 0 && inactiveObservation.activeReasonFlags == 0,
        "static bucket observation separates inactive resident geometry from active membership");

    input.valid = false;
    RtSmokeStaticTlasBucketObservation invalidObservation;
    Check(!BuildSmokeStaticTlasBucketObservation(input, invalidObservation), "static bucket observation rejects invalid records");

    input.valid = true;
    input.indexCount = 0;
    Check(!BuildSmokeStaticTlasBucketObservation(input, invalidObservation), "static bucket observation rejects empty ranges");
}

void TestStaticBucketBlasPlan()
{
    RtSmokeStaticTlasBucketObservation buckets[3];
    buckets[0].bucketKey = 100;
    buckets[0].resident = true;
    buckets[0].active = true;
    buckets[0].activeReasonFlags = RT_SMOKE_STATIC_ACTIVE_VISIBLE;
    buckets[0].routeRecordIndex = 0;
    buckets[0].residentVertexOffset = 0;
    buckets[0].residentVertexCount = 10;
    buckets[0].residentIndexOffset = 0;
    buckets[0].residentIndexCount = 30;
    buckets[0].residentTriangleOffset = 0;
    buckets[0].residentTriangleCount = 10;

    buckets[1] = buckets[0];
    buckets[1].bucketKey = 200;
    buckets[1].active = false;
    buckets[1].activeReasonFlags = 0;
    buckets[1].routeRecordIndex = 1;
    buckets[1].residentVertexOffset = 10;
    buckets[1].residentVertexCount = 20;
    buckets[1].residentIndexOffset = 30;
    buckets[1].residentIndexCount = 60;
    buckets[1].residentTriangleOffset = 10;
    buckets[1].residentTriangleCount = 20;

    buckets[2] = buckets[0];
    buckets[2].bucketKey = 300;
    buckets[2].residentIndexCount = 0;

    RtSmokeStaticBucketBlasPlanDesc desc;
    desc.buckets = buckets;
    desc.bucketCount = 3;
    desc.activeOnly = true;
    const RtSmokeStaticBucketBlasPlan activePlan = BuildSmokeStaticBucketBlasPlan(desc);
    Check(activePlan.emittedRecords == 1 && activePlan.activeBuckets == 2 &&
        activePlan.residentBuckets == 3 && activePlan.skippedInactive == 1 &&
        activePlan.skippedInvalid == 1,
        "static bucket BLAS plan emits active valid bucket ranges");
    Check(activePlan.records[0].bucketKey == 100 &&
        activePlan.records[0].range.vertexOffset == 0 &&
        activePlan.records[0].range.vertexCount == 10 &&
        activePlan.records[0].range.indexOffset == 0 &&
        activePlan.records[0].range.indexCount == 30 &&
        activePlan.records[0].range.triangleOffset == 0 &&
        activePlan.records[0].range.triangleCount == 10 &&
        activePlan.planSignature != 0,
        "static bucket BLAS plan preserves bucket range identity");

    buckets[0].routeRecordIndex = 9;
    buckets[0].activeReasonFlags = RT_SMOKE_STATIC_ACTIVE_VISIBLE | RT_SMOKE_STATIC_ACTIVE_FORCE_INCLUDE;
    const RtSmokeStaticBucketBlasPlan routeChangedPlan = BuildSmokeStaticBucketBlasPlan(desc);
    Check(routeChangedPlan.records[0].routeRecordIndex == 9 &&
        routeChangedPlan.records[0].activeReasonFlags == buckets[0].activeReasonFlags &&
        routeChangedPlan.planSignature == activePlan.planSignature,
        "static bucket BLAS plan signature ignores route-only metadata");
    buckets[0].routeRecordIndex = 0;
    buckets[0].activeReasonFlags = RT_SMOKE_STATIC_ACTIVE_VISIBLE;

    desc.bucketCount = 1;
    const RtSmokeStaticBucketBlasPlan singleActivePlan = BuildSmokeStaticBucketBlasPlan(desc);
    Check(singleActivePlan.records.size() == activePlan.records.size() &&
        singleActivePlan.records[0].bucketKey == activePlan.records[0].bucketKey &&
        singleActivePlan.planSignature != activePlan.planSignature,
        "static bucket BLAS signature tracks skipped inactive and invalid buckets");

    desc.bucketCount = 3;
    desc.activeOnly = false;
    const RtSmokeStaticBucketBlasPlan residentPlan = BuildSmokeStaticBucketBlasPlan(desc);
    Check(residentPlan.emittedRecords == 2 && residentPlan.skippedInactive == 0 &&
        residentPlan.records[1].bucketKey == 200 && !residentPlan.records[1].active &&
        residentPlan.records[1].range.vertexOffset == 10 &&
        residentPlan.records[1].range.vertexCount == 20 &&
        residentPlan.records[1].range.indexOffset == 30 &&
        residentPlan.records[1].range.indexCount == 60 &&
        residentPlan.records[1].range.triangleOffset == 10 &&
        residentPlan.records[1].range.triangleCount == 20,
        "static bucket BLAS plan can retain inactive resident bucket ranges with offsets");

    desc.activeOnly = false;
    desc.maxRecords = 1;
    const RtSmokeStaticBucketBlasPlan cappedPlan = BuildSmokeStaticBucketBlasPlan(desc);
    Check(cappedPlan.emittedRecords == 1 && cappedPlan.overflow, "static bucket BLAS plan reports record cap overflow");

    desc.maxRecords = 0;
    const RtSmokeStaticBucketBlasPlan uncappedPlan = BuildSmokeStaticBucketBlasPlan(desc);
    desc.maxRecords = -5;
    const RtSmokeStaticBucketBlasPlan negativeCapPlan = BuildSmokeStaticBucketBlasPlan(desc);
    Check(uncappedPlan.emittedRecords == negativeCapPlan.emittedRecords &&
        uncappedPlan.planSignature == negativeCapPlan.planSignature,
        "static bucket BLAS plan normalizes negative record caps as uncapped");
}

void TestStaticBucketTraversalCompatibility()
{
    RtSmokeStaticBucketBlasRecord monolithicRecord;
    monolithicRecord.bucketKey = 100;
    monolithicRecord.range.vertexOffset = 0;
    monolithicRecord.range.vertexCount = 30;
    monolithicRecord.range.indexOffset = 0;
    monolithicRecord.range.indexCount = 90;
    monolithicRecord.range.triangleOffset = 0;
    monolithicRecord.range.triangleCount = 30;

    RtSmokeStaticBucketTraversalCompatibilityInput input;
    input.records = &monolithicRecord;
    input.recordCount = 1;
    input.totalVertexCount = 30;
    input.totalIndexCount = 90;
    input.totalTriangleCount = 30;
    const RtSmokeStaticBucketTraversalCompatibility monolithicCompatibility =
        BuildSmokeStaticBucketTraversalCompatibility(input);
    Check(monolithicCompatibility.exactMonolithicRecord &&
        monolithicCompatibility.currentStaticShaderCompatible &&
        !monolithicCompatibility.requiresShaderRouteMetadata,
        "static bucket traversal compatibility accepts exact monolithic static record");

    RtSmokeStaticBucketBlasRecord bucketRecords[2];
    bucketRecords[0] = monolithicRecord;
    bucketRecords[0].range.vertexCount = 10;
    bucketRecords[0].range.indexCount = 30;
    bucketRecords[0].range.triangleCount = 10;
    bucketRecords[1] = bucketRecords[0];
    bucketRecords[1].bucketKey = 200;
    bucketRecords[1].range.vertexOffset = 10;
    bucketRecords[1].range.indexOffset = 30;
    bucketRecords[1].range.triangleOffset = 10;
    input.records = bucketRecords;
    input.recordCount = 2;
    const RtSmokeStaticBucketTraversalCompatibility bucketCompatibility =
        BuildSmokeStaticBucketTraversalCompatibility(input);
    Check(!bucketCompatibility.exactMonolithicRecord &&
        !bucketCompatibility.currentStaticShaderCompatible &&
        bucketCompatibility.requiresShaderRouteMetadata &&
        bucketCompatibility.nonZeroOffsetRecords == 1,
        "static bucket traversal compatibility blocks bucketed static records without shader routes");

    input.shaderSupportsStaticBucketRoutes = true;
    const RtSmokeStaticBucketTraversalCompatibility routedCompatibility =
        BuildSmokeStaticBucketTraversalCompatibility(input);
    Check(routedCompatibility.currentStaticShaderCompatible &&
        !routedCompatibility.requiresShaderRouteMetadata,
        "static bucket traversal compatibility allows bucketed records when shader routes are available");

    input.records = nullptr;
    input.recordCount = 2;
    const RtSmokeStaticBucketTraversalCompatibility nullRecordsCompatibility =
        BuildSmokeStaticBucketTraversalCompatibility(input);
    input.records = bucketRecords;
    input.recordCount = -2;
    const RtSmokeStaticBucketTraversalCompatibility negativeCountCompatibility =
        BuildSmokeStaticBucketTraversalCompatibility(input);
    Check(nullRecordsCompatibility.recordCount == 0 &&
        negativeCountCompatibility.recordCount == 0 &&
        !nullRecordsCompatibility.requiresShaderRouteMetadata &&
        !negativeCountCompatibility.requiresShaderRouteMetadata,
        "static bucket traversal compatibility normalizes absent records as empty");
}

void TestRouteInstanceNamespacePlan()
{
    RtSmokeRouteInstanceNamespacePlanInput input;
    input.staticRouteRecordCount = 4;
    input.rigidRouteRecordCount = 7;
    input.firstRouteInstanceId = 2;

    const RtSmokeRouteInstanceNamespacePlan defaultPlan =
        BuildSmokeRouteInstanceNamespacePlan(input);
    Check(!defaultPlan.staticRoutesEnabled &&
        !defaultPlan.staticRoutesBlocked &&
        defaultPlan.staticRouteInstanceCount == 0 &&
        defaultPlan.rigidRouteInstanceCount == 7 &&
        defaultPlan.rigidFirstInstanceId == 2 &&
        !defaultPlan.rigidRouteBaseShifted,
        "route namespace keeps current rigid base when static routes are disabled");

    input.enableStaticRoutes = true;
    const RtSmokeRouteInstanceNamespacePlan blockedPlan =
        BuildSmokeRouteInstanceNamespacePlan(input);
    Check(!blockedPlan.staticRoutesEnabled &&
        blockedPlan.staticRoutesRequireShaderSupport &&
        blockedPlan.staticRoutesBlocked &&
        blockedPlan.staticRouteInstanceCount == 0 &&
        blockedPlan.rigidFirstInstanceId == 2 &&
        !blockedPlan.rigidRouteBaseShifted,
        "route namespace blocks static routes without shader route support");

    input.shaderSupportsStaticBucketRoutes = true;
    const RtSmokeRouteInstanceNamespacePlan routedPlan =
        BuildSmokeRouteInstanceNamespacePlan(input);
    Check(routedPlan.staticRoutesEnabled &&
        !routedPlan.staticRoutesBlocked &&
        routedPlan.staticFirstInstanceId == 2 &&
        routedPlan.staticRouteInstanceCount == 4 &&
        routedPlan.rigidFirstInstanceId == 6 &&
        routedPlan.rigidRouteBaseShifted,
        "route namespace reserves static route IDs before shifted rigid route IDs");

    input.staticRouteRecordCount = 0;
    const RtSmokeRouteInstanceNamespacePlan zeroStaticPlan =
        BuildSmokeRouteInstanceNamespacePlan(input);
    Check(!zeroStaticPlan.staticRoutesEnabled &&
        zeroStaticPlan.staticRouteInstanceCount == 0 &&
        zeroStaticPlan.rigidFirstInstanceId == 2 &&
        !zeroStaticPlan.rigidRouteBaseShifted,
        "route namespace keeps rigid base when no static route records exist");
}

void TestStaticRouteTablePlan()
{
    RtSmokeStaticBucketBlasRecord bucketRecords[3];
    bucketRecords[0].bucketKey = 10;
    bucketRecords[0].routeRecordIndex = 3;
    bucketRecords[0].activeReasonFlags = 0x1;
    bucketRecords[0].range.vertexOffset = 0;
    bucketRecords[0].range.vertexCount = 12;
    bucketRecords[0].range.indexOffset = 0;
    bucketRecords[0].range.indexCount = 36;
    bucketRecords[0].range.triangleOffset = 0;
    bucketRecords[0].range.triangleCount = 12;
    bucketRecords[1] = bucketRecords[0];
    bucketRecords[1].bucketKey = 20;
    bucketRecords[1].routeRecordIndex = 4;
    bucketRecords[1].activeReasonFlags = 0x2;
    bucketRecords[1].range.vertexOffset = 12;
    bucketRecords[1].range.indexOffset = 36;
    bucketRecords[1].range.triangleOffset = 12;
    bucketRecords[2] = bucketRecords[0];
    bucketRecords[2].bucketKey = 30;
    bucketRecords[2].range.indexCount = 0;

    RtSmokeStaticRouteTablePlanInput input;
    input.records = bucketRecords;
    input.recordCount = 3;

    RtSmokeRouteInstanceNamespacePlanInput namespaceInput;
    namespaceInput.staticRouteRecordCount = 2;
    namespaceInput.rigidRouteRecordCount = 5;
    namespaceInput.firstRouteInstanceId = 2;
    namespaceInput.enableStaticRoutes = false;
    input.routeNamespace = BuildSmokeRouteInstanceNamespacePlan(namespaceInput);
    const RtSmokeStaticRouteTablePlan disabledPlan = BuildSmokeStaticRouteTablePlan(input);
    Check(disabledPlan.emittedRecords == 0 &&
        disabledPlan.skippedDisabled == 3 &&
        !disabledPlan.blocked,
        "static route table emits no records while static routes are disabled");

    namespaceInput.enableStaticRoutes = true;
    input.routeNamespace = BuildSmokeRouteInstanceNamespacePlan(namespaceInput);
    const RtSmokeStaticRouteTablePlan blockedPlan = BuildSmokeStaticRouteTablePlan(input);
    Check(blockedPlan.emittedRecords == 0 &&
        blockedPlan.skippedDisabled == 3 &&
        blockedPlan.blocked &&
        blockedPlan.tableSignature != disabledPlan.tableSignature,
        "static route table emits no records when namespace blocks static routes");

    namespaceInput.shaderSupportsStaticBucketRoutes = true;
    input.routeNamespace = BuildSmokeRouteInstanceNamespacePlan(namespaceInput);
    const RtSmokeStaticRouteTablePlan routedPlan = BuildSmokeStaticRouteTablePlan(input);
    Check(routedPlan.emittedRecords == 2 &&
        routedPlan.skippedInvalid == 1 &&
        routedPlan.records[0].instanceId == 2 &&
        routedPlan.records[1].instanceId == 3 &&
        routedPlan.records[1].bucketKey == 20 &&
        routedPlan.records[1].range.vertexOffset == 12 &&
        routedPlan.records[1].range.vertexCount == 12 &&
        routedPlan.records[1].range.indexOffset == 36 &&
        routedPlan.records[1].range.indexCount == 36 &&
        routedPlan.records[1].range.triangleOffset == 12 &&
        routedPlan.records[1].range.triangleCount == 12 &&
        routedPlan.tableSignature != 0,
        "static route table assigns deterministic instance IDs and preserves bucket ranges");

    input.maxRecords = 1;
    const RtSmokeStaticRouteTablePlan cappedPlan = BuildSmokeStaticRouteTablePlan(input);
    Check(cappedPlan.emittedRecords == 1 &&
        cappedPlan.overflow,
        "static route table reports record cap overflow");

    input.recordCount = 1;
    const RtSmokeStaticRouteTablePlan singlePrefixPlan = BuildSmokeStaticRouteTablePlan(input);
    Check(singlePrefixPlan.records.size() == cappedPlan.records.size() &&
        singlePrefixPlan.records[0].bucketKey == cappedPlan.records[0].bucketKey &&
        singlePrefixPlan.tableSignature != cappedPlan.tableSignature,
        "static route table signature tracks capped summary state with identical emitted prefix");

    input.recordCount = 3;
    input.maxRecords = 0;
    const RtSmokeStaticRouteTablePlan uncappedPlan = BuildSmokeStaticRouteTablePlan(input);
    input.maxRecords = -2;
    const RtSmokeStaticRouteTablePlan negativeCapPlan = BuildSmokeStaticRouteTablePlan(input);
    Check(uncappedPlan.emittedRecords == negativeCapPlan.emittedRecords &&
        uncappedPlan.tableSignature == negativeCapPlan.tableSignature,
        "static route table plan normalizes negative record caps as uncapped");

    input.records = nullptr;
    input.recordCount = 3;
    const RtSmokeStaticRouteTablePlan nullRecordsPlan = BuildSmokeStaticRouteTablePlan(input);
    input.recordCount = 0;
    const RtSmokeStaticRouteTablePlan emptyRecordsPlan = BuildSmokeStaticRouteTablePlan(input);
    Check(nullRecordsPlan.inputRecords == 0 &&
        nullRecordsPlan.tableSignature == emptyRecordsPlan.tableSignature,
        "static route table plan normalizes absent records as empty");
}

void TestBvhDirtyPlan()
{
    RtSmokeBvhDirtyTokenState base;
    base.geometryContentSignature = 10;
    base.materialGeneration = 20;
    base.activeSetSignature = 30;
    base.tlasInstanceSignature = 40;

    RtSmokeBvhDirtyPlanInput input;
    input.previousValid = true;
    input.previous = base;
    input.current = base;
    const RtSmokeBvhDirtyPlan unchangedPlan = BuildSmokeBvhDirtyPlan(input);
    Check(!unchangedPlan.geometryContentChanged && !unchangedPlan.materialChanged &&
        !unchangedPlan.activeMembershipChanged && !unchangedPlan.tlasInstanceChanged &&
        !unchangedPlan.blasInputDirty && !unchangedPlan.tlasDirty,
        "BVH dirty plan keeps unchanged tokens clean");

    input.previousValid = false;
    const RtSmokeBvhDirtyPlan firstFramePlan = BuildSmokeBvhDirtyPlan(input);
    Check(firstFramePlan.geometryContentChanged && firstFramePlan.materialChanged &&
        firstFramePlan.activeMembershipChanged && firstFramePlan.tlasInstanceChanged &&
        firstFramePlan.blasInputDirty && firstFramePlan.tlasDirty,
        "BVH dirty plan treats invalid previous state as dirty");

    input.previousValid = true;
    input.current = base;
    input.current.geometryContentSignature = 11;
    const RtSmokeBvhDirtyPlan geometryPlan = BuildSmokeBvhDirtyPlan(input);
    Check(geometryPlan.geometryContentChanged && geometryPlan.blasInputDirty &&
        geometryPlan.tlasDirty && !geometryPlan.activeMembershipChanged,
        "BVH dirty plan maps geometry content changes to BLAS and TLAS work");

    input.current = base;
    input.current.materialGeneration = 21;
    const RtSmokeBvhDirtyPlan materialPlan = BuildSmokeBvhDirtyPlan(input);
    Check(materialPlan.materialChanged && !materialPlan.blasInputDirty &&
        !materialPlan.tlasDirty && !materialPlan.activeMembershipChanged,
        "BVH dirty plan keeps material generation changes out of BLAS and TLAS work");

    input.current = base;
    input.current.activeSetSignature = 31;
    const RtSmokeBvhDirtyPlan activePlan = BuildSmokeBvhDirtyPlan(input);
    Check(activePlan.activeMembershipChanged && activePlan.tlasDirty &&
        !activePlan.blasInputDirty && !activePlan.geometryContentChanged,
        "BVH dirty plan maps active-set changes to TLAS-only work");

    input.current = base;
    input.current.tlasInstanceSignature = 41;
    const RtSmokeBvhDirtyPlan tlasPlan = BuildSmokeBvhDirtyPlan(input);
    Check(tlasPlan.tlasInstanceChanged && tlasPlan.tlasDirty &&
        !tlasPlan.blasInputDirty && !tlasPlan.geometryContentChanged,
        "BVH dirty plan maps TLAS instance changes to TLAS-only work");

    RtSmokeStaticTlasBucketObservation buckets[2];
    buckets[0].bucketKey = 100;
    buckets[0].resident = true;
    buckets[0].active = true;
    buckets[0].hasBlas = true;
    buckets[0].activeReasonFlags = RT_SMOKE_STATIC_ACTIVE_VISIBLE;
    buckets[0].residentSurfaceCount = 1;
    buckets[0].residentVertexCount = 10;
    buckets[0].residentIndexCount = 30;
    buckets[0].residentTriangleCount = 10;
    buckets[0].activeSurfaceCount = 1;
    buckets[0].activeVertexCount = 10;
    buckets[0].activeIndexCount = 30;
    buckets[0].activeTriangleCount = 10;

    buckets[1].bucketKey = 200;
    buckets[1].resident = true;
    buckets[1].active = false;
    buckets[1].hasBlas = true;
    buckets[1].residentSurfaceCount = 1;
    buckets[1].residentVertexCount = 20;
    buckets[1].residentIndexCount = 60;
    buckets[1].residentTriangleCount = 20;

    RtSmokeStaticTlasActiveSetPlanDesc activeSetDesc;
    activeSetDesc.buckets = buckets;
    activeSetDesc.bucketCount = 2;
    activeSetDesc.hasStaticBlas = true;
    activeSetDesc.monolithicStaticBlas = false;
    const RtSmokeStaticTlasActiveSetPlan baseActiveSetPlan = BuildSmokeStaticTlasActiveSetPlan(activeSetDesc);

    buckets[0].activeReasonFlags = RT_SMOKE_STATIC_ACTIVE_VISIBLE | RT_SMOKE_STATIC_ACTIVE_SELECTED_AREA;
    const RtSmokeStaticTlasActiveSetPlan reasonChangedPlan = BuildSmokeStaticTlasActiveSetPlan(activeSetDesc);
    Check(baseActiveSetPlan.activeSetSignature != reasonChangedPlan.activeSetSignature &&
        baseActiveSetPlan.residentSetSignature == reasonChangedPlan.residentSetSignature,
        "static active-set signature tracks active reasons without changing resident signature");

    buckets[0].activeReasonFlags = RT_SMOKE_STATIC_ACTIVE_VISIBLE;
    buckets[1].active = true;
    buckets[1].activeReasonFlags = RT_SMOKE_STATIC_ACTIVE_RESIDENCY;
    buckets[1].activeSurfaceCount = buckets[1].residentSurfaceCount;
    buckets[1].activeVertexCount = buckets[1].residentVertexCount;
    buckets[1].activeIndexCount = buckets[1].residentIndexCount;
    buckets[1].activeTriangleCount = buckets[1].residentTriangleCount;
    const RtSmokeStaticTlasActiveSetPlan membershipChangedPlan = BuildSmokeStaticTlasActiveSetPlan(activeSetDesc);
    Check(baseActiveSetPlan.activeSetSignature != membershipChangedPlan.activeSetSignature &&
        baseActiveSetPlan.residentSetSignature == membershipChangedPlan.residentSetSignature,
        "static active-set signature tracks active membership without changing resident signature");

    buckets[1].active = false;
    buckets[0].hasBlas = false;
    const RtSmokeStaticTlasActiveSetPlan missingBlasPlan = BuildSmokeStaticTlasActiveSetPlan(activeSetDesc);
    Check(baseActiveSetPlan.activeSetSignature == missingBlasPlan.activeSetSignature &&
        baseActiveSetPlan.residentSetSignature == missingBlasPlan.residentSetSignature &&
        baseActiveSetPlan.tlasInstanceSignature != missingBlasPlan.tlasInstanceSignature,
        "static active-set TLAS signature tracks BLAS readiness without changing membership signatures");
}

void TestStaticBucketWorkDirtyToken()
{
    RtSmokeStaticTlasBucketObservation buckets[2];
    buckets[0].bucketKey = 10;
    buckets[0].resident = true;
    buckets[0].active = true;
    buckets[0].hasBlas = true;
    buckets[0].activeReasonFlags = RT_SMOKE_STATIC_ACTIVE_VISIBLE;
    buckets[0].routeRecordIndex = 0;
    buckets[0].residentVertexCount = 3;
    buckets[0].residentIndexCount = 3;
    buckets[0].residentTriangleCount = 1;
    buckets[0].activeVertexCount = 3;
    buckets[0].activeIndexCount = 3;
    buckets[0].activeTriangleCount = 1;

    buckets[1] = buckets[0];
    buckets[1].bucketKey = 20;
    buckets[1].active = false;
    buckets[1].routeRecordIndex = 1;
    buckets[1].residentVertexOffset = 3;
    buckets[1].residentIndexOffset = 3;
    buckets[1].residentTriangleOffset = 1;

    RtSmokeStaticBucketWorkPlanInput input;
    input.buckets = buckets;
    input.bucketCount = 2;
    input.geometryContentSignature = 100;
    input.materialGeneration = 200;
    input.totalVertexCount = 6;
    input.totalIndexCount = 6;
    input.totalTriangleCount = 2;
    input.hasStaticBlas = true;
    input.enableStaticRoutes = true;
    input.shaderSupportsStaticBucketRoutes = true;
    const RtSmokeStaticBucketWorkPlan basePlan = BuildSmokeStaticBucketWorkPlan(input);

    RtSmokeStaticBucketWorkDirtyTokenInput tokenInput;
    tokenInput.plan = &basePlan;
    tokenInput.materialGeneration = input.materialGeneration;
    const RtSmokeBvhDirtyTokenState baseToken = BuildSmokeStaticBucketWorkDirtyToken(tokenInput);

    RtSmokeBvhDirtyPlanInput dirtyInput;
    dirtyInput.previousValid = true;
    dirtyInput.previous = baseToken;
    dirtyInput.current = baseToken;
    const RtSmokeBvhDirtyPlan unchangedPlan = BuildSmokeBvhDirtyPlan(dirtyInput);
    Check(!unchangedPlan.geometryContentChanged &&
        !unchangedPlan.activeGeometryContentChanged &&
        !unchangedPlan.residentSetChanged &&
        !unchangedPlan.blasInputDirty &&
        !unchangedPlan.tlasDirty,
        "static bucket work dirty token keeps unchanged plan clean");

    buckets[1].residentIndexCount = 6;
    const RtSmokeStaticBucketWorkPlan inactiveResidentChangedPlan = BuildSmokeStaticBucketWorkPlan(input);
    tokenInput.plan = &inactiveResidentChangedPlan;
    dirtyInput.current = BuildSmokeStaticBucketWorkDirtyToken(tokenInput);
    const RtSmokeBvhDirtyPlan inactiveResidentDirtyPlan = BuildSmokeBvhDirtyPlan(dirtyInput);
    Check(inactiveResidentDirtyPlan.geometryContentChanged &&
        inactiveResidentDirtyPlan.residentSetChanged &&
        !inactiveResidentDirtyPlan.activeGeometryContentChanged &&
        !inactiveResidentDirtyPlan.activeMembershipChanged &&
        !inactiveResidentDirtyPlan.blasInputDirty &&
        !inactiveResidentDirtyPlan.tlasDirty,
        "static bucket work dirty token keeps inactive resident geometry out of active BLAS and TLAS work");

    buckets[1].residentIndexCount = 3;
    buckets[1].active = true;
    buckets[1].activeReasonFlags = RT_SMOKE_STATIC_ACTIVE_SELECTED_AREA;
    buckets[1].activeVertexCount = buckets[1].residentVertexCount;
    buckets[1].activeIndexCount = buckets[1].residentIndexCount;
    buckets[1].activeTriangleCount = buckets[1].residentTriangleCount;
    const RtSmokeStaticBucketWorkPlan activeChangedPlan = BuildSmokeStaticBucketWorkPlan(input);
    tokenInput.plan = &activeChangedPlan;
    dirtyInput.current = BuildSmokeStaticBucketWorkDirtyToken(tokenInput);
    const RtSmokeBvhDirtyPlan activeDirtyPlan = BuildSmokeBvhDirtyPlan(dirtyInput);
    Check(activeDirtyPlan.activeMembershipChanged &&
        activeDirtyPlan.tlasInstanceChanged &&
        activeDirtyPlan.tlasDirty &&
        !activeDirtyPlan.blasInputDirty &&
        !activeDirtyPlan.activeGeometryContentChanged &&
        !activeDirtyPlan.residentSetChanged &&
        !activeDirtyPlan.geometryContentChanged,
        "static bucket work dirty token maps active bucket changes to TLAS-only work");

    buckets[1].residentIndexCount = 6;
    buckets[1].activeIndexCount = 6;
    const RtSmokeStaticBucketWorkPlan geometryChangedPlan = BuildSmokeStaticBucketWorkPlan(input);
    tokenInput.plan = &geometryChangedPlan;
    dirtyInput.current = BuildSmokeStaticBucketWorkDirtyToken(tokenInput);
    const RtSmokeBvhDirtyPlan geometryDirtyPlan = BuildSmokeBvhDirtyPlan(dirtyInput);
    Check(geometryDirtyPlan.geometryContentChanged &&
        geometryDirtyPlan.activeGeometryContentChanged &&
        geometryDirtyPlan.blasInputDirty &&
        geometryDirtyPlan.tlasDirty,
        "static bucket work dirty token maps active resident geometry changes to BLAS work");

    buckets[1].residentIndexCount = 3;
    buckets[1].activeIndexCount = 3;
    buckets[1].active = false;
    buckets[1].activeReasonFlags = 0;
    input.materialGeneration = 201;
    const RtSmokeStaticBucketWorkPlan materialChangedPlan = BuildSmokeStaticBucketWorkPlan(input);
    tokenInput.plan = &materialChangedPlan;
    tokenInput.materialGeneration = input.materialGeneration;
    dirtyInput.current = BuildSmokeStaticBucketWorkDirtyToken(tokenInput);
    const RtSmokeBvhDirtyPlan materialDirtyPlan = BuildSmokeBvhDirtyPlan(dirtyInput);
    Check(materialDirtyPlan.materialChanged &&
        !materialDirtyPlan.geometryContentChanged &&
        !materialDirtyPlan.activeGeometryContentChanged &&
        !materialDirtyPlan.blasInputDirty &&
        !materialDirtyPlan.tlasDirty,
        "static bucket work dirty token tracks material changes without BLAS or TLAS work");

    input.materialGeneration = 200;
    buckets[1].residentIndexCount = 3;
    buckets[1].activeIndexCount = 3;
    input.shaderSupportsStaticBucketRoutes = false;
    const RtSmokeStaticBucketWorkPlan routeChangedPlan = BuildSmokeStaticBucketWorkPlan(input);
    tokenInput.plan = &routeChangedPlan;
    tokenInput.materialGeneration = input.materialGeneration;
    dirtyInput.current = BuildSmokeStaticBucketWorkDirtyToken(tokenInput);
    const RtSmokeBvhDirtyPlan routeDirtyPlan = BuildSmokeBvhDirtyPlan(dirtyInput);
    Check(routeDirtyPlan.tlasInstanceChanged &&
        routeDirtyPlan.tlasDirty &&
        !routeDirtyPlan.blasInputDirty,
        "static bucket work dirty token maps route compatibility changes to TLAS-only work");

    buckets[0].hasBlas = false;
    buckets[1].active = false;
    buckets[1].activeReasonFlags = 0;
    input.enableStaticRoutes = true;
    input.shaderSupportsStaticBucketRoutes = true;
    const RtSmokeStaticBucketWorkPlan blasMissingPlan = BuildSmokeStaticBucketWorkPlan(input);
    tokenInput.plan = &blasMissingPlan;
    tokenInput.materialGeneration = input.materialGeneration;
    dirtyInput.previous = baseToken;
    dirtyInput.current = BuildSmokeStaticBucketWorkDirtyToken(tokenInput);
    const RtSmokeBvhDirtyPlan blasMissingDirtyPlan = BuildSmokeBvhDirtyPlan(dirtyInput);
    Check(blasMissingDirtyPlan.tlasInstanceChanged &&
        blasMissingDirtyPlan.tlasDirty &&
        !blasMissingDirtyPlan.activeMembershipChanged &&
        !blasMissingDirtyPlan.blasInputDirty,
        "static bucket work dirty token maps active BLAS readiness changes to TLAS-only work");
}

void TestBvhFrameToken()
{
    RtSmokeBvhFrameTokenInput input;
    input.staticBlasSignature = 100;
    input.geometryGeneration = 200;
    input.materialGeneration = 300;
    input.staticActiveSetSignature = 400;
    input.staticResidentSetSignature = 500;
    input.staticTlasInstanceSignature = 600;
    input.dynamicVertexCount = 10;
    input.dynamicIndexCount = 30;
    input.rigidRouteVertexCount = 20;
    input.rigidRouteIndexCount = 60;
    input.rigidRouteTriangleCount = 20;
    input.rigidRouteInstanceCount = 3;
    input.rigidRouteSeenThisFrameCount = 2;
    input.rigidRouteCachedInstanceCount = 1;
    input.rigidTlasInstanceSignature = 700;
    input.baseTlasInstanceCount = 2;
    input.rigidTlasInstanceCount = 3;
    input.hasStaticBlas = true;
    input.hasDynamicBlas = true;

    const RtSmokeBvhFrameToken baseToken = BuildSmokeBvhFrameToken(input);
    Check(baseToken.dirtyToken.geometryContentSignature != 0 &&
        baseToken.dirtyToken.activeBlasInputSignature != 0 &&
        baseToken.dirtyToken.materialGeneration == 300 &&
        baseToken.dirtyToken.activeSetSignature != 0 &&
        baseToken.dirtyToken.tlasInstanceSignature != 0 &&
        baseToken.dirtyToken.residentSetSignature == 500 &&
        baseToken.residentSetSignature == 500,
        "BVH frame token emits geometry, active-BLAS, material, active, TLAS, and resident signatures");

    RtSmokeBvhFrameTokenInput activeChangedInput = input;
    activeChangedInput.rigidRouteInstanceCount = 4;
    const RtSmokeBvhFrameToken activeChangedToken = BuildSmokeBvhFrameToken(activeChangedInput);
    Check(baseToken.dirtyToken.geometryContentSignature == activeChangedToken.dirtyToken.geometryContentSignature &&
        baseToken.dirtyToken.activeSetSignature != activeChangedToken.dirtyToken.activeSetSignature &&
        baseToken.dirtyToken.tlasInstanceSignature != activeChangedToken.dirtyToken.tlasInstanceSignature,
        "BVH frame token maps rigid active membership changes to active and TLAS signatures");

    RtSmokeBvhFrameTokenInput geometryChangedInput = input;
    geometryChangedInput.rigidRouteIndexCount = 63;
    const RtSmokeBvhFrameToken geometryChangedToken = BuildSmokeBvhFrameToken(geometryChangedInput);
    Check(baseToken.dirtyToken.geometryContentSignature != geometryChangedToken.dirtyToken.geometryContentSignature &&
        baseToken.dirtyToken.activeSetSignature == geometryChangedToken.dirtyToken.activeSetSignature,
        "BVH frame token maps geometry count changes to geometry signature only");

    RtSmokeBvhFrameTokenInput tlasChangedInput = input;
    tlasChangedInput.rigidTlasInstanceCount = 4;
    const RtSmokeBvhFrameToken tlasChangedToken = BuildSmokeBvhFrameToken(tlasChangedInput);
    Check(baseToken.dirtyToken.geometryContentSignature == tlasChangedToken.dirtyToken.geometryContentSignature &&
        baseToken.dirtyToken.activeSetSignature == tlasChangedToken.dirtyToken.activeSetSignature &&
        baseToken.dirtyToken.tlasInstanceSignature != tlasChangedToken.dirtyToken.tlasInstanceSignature,
        "BVH frame token maps submitted TLAS count changes to TLAS signature only");

    RtSmokeBvhFrameTokenInput rigidSignatureChangedInput = input;
    rigidSignatureChangedInput.rigidTlasInstanceSignature = 701;
    const RtSmokeBvhFrameToken rigidSignatureChangedToken = BuildSmokeBvhFrameToken(rigidSignatureChangedInput);
    Check(baseToken.dirtyToken.geometryContentSignature == rigidSignatureChangedToken.dirtyToken.geometryContentSignature &&
        baseToken.dirtyToken.activeSetSignature == rigidSignatureChangedToken.dirtyToken.activeSetSignature &&
        baseToken.dirtyToken.tlasInstanceSignature != rigidSignatureChangedToken.dirtyToken.tlasInstanceSignature,
        "BVH frame token maps rigid TLAS signature changes to TLAS signature only");

    RtSmokeBvhFrameTokenInput staticRouteChangedInput = input;
    staticRouteChangedInput.staticTlasInstanceSignature = 601;
    const RtSmokeBvhFrameToken staticRouteChangedToken = BuildSmokeBvhFrameToken(staticRouteChangedInput);
    Check(baseToken.dirtyToken.geometryContentSignature == staticRouteChangedToken.dirtyToken.geometryContentSignature &&
        baseToken.dirtyToken.activeSetSignature == staticRouteChangedToken.dirtyToken.activeSetSignature &&
        baseToken.dirtyToken.tlasInstanceSignature != staticRouteChangedToken.dirtyToken.tlasInstanceSignature,
        "BVH frame token maps static route-table changes to TLAS signature only");
}

void TestBvhFramePlanningResult()
{
    RtSmokeStaticTlasBucketObservation buckets[2];
    buckets[0].bucketKey = 100;
    buckets[0].resident = true;
    buckets[0].active = true;
    buckets[0].hasBlas = true;
    buckets[0].activeReasonFlags = RT_SMOKE_STATIC_ACTIVE_SELECTED_AREA;
    buckets[0].residentVertexCount = 10;
    buckets[0].residentIndexCount = 30;
    buckets[0].residentTriangleCount = 10;
    buckets[0].activeVertexCount = 10;
    buckets[0].activeIndexCount = 30;
    buckets[0].activeTriangleCount = 10;
    buckets[1] = buckets[0];
    buckets[1].bucketKey = 200;
    buckets[1].active = false;

    RtSmokeBvhFramePlanningInput input;
    input.staticBucketWorkInput.buckets = buckets;
    input.staticBucketWorkInput.bucketCount = 2;
    input.staticBucketWorkInput.geometryContentSignature = 900;
    input.staticBucketWorkInput.materialGeneration = 901;
    input.staticBucketWorkInput.totalVertexCount = 20;
    input.staticBucketWorkInput.totalIndexCount = 60;
    input.staticBucketWorkInput.totalTriangleCount = 20;
    input.staticBucketWorkInput.monolithicStaticBlas = true;
    input.staticBucketWorkInput.hasStaticBlas = true;
    input.frameTokenInput.staticBlasSignature = 902;
    input.frameTokenInput.geometryGeneration = 903;
    input.frameTokenInput.materialGeneration = 904;
    input.frameTokenInput.dynamicVertexCount = 3;
    input.frameTokenInput.dynamicIndexCount = 3;
    input.frameTokenInput.rigidRouteInstanceCount = 1;
    input.frameTokenInput.rigidTlasInstanceSignature = 905;
    input.frameTokenInput.baseTlasInstanceCount = 2;
    input.frameTokenInput.rigidTlasInstanceCount = 1;
    input.frameTokenInput.hasStaticBlas = true;
    input.frameTokenInput.hasDynamicBlas = true;

    const RtSmokeBvhFramePlanningResult firstResult =
        BuildSmokeBvhFramePlanningResult(input);
    RtSmokeStaticBucketWorkDirtyTokenInput expectedStaticDirtyTokenInput;
    expectedStaticDirtyTokenInput.plan = &firstResult.staticBucketWorkPlan;
    expectedStaticDirtyTokenInput.materialGeneration =
        input.staticBucketWorkInput.materialGeneration;
    const RtSmokeBvhDirtyTokenState expectedStaticDirtyToken =
        BuildSmokeStaticBucketWorkDirtyToken(expectedStaticDirtyTokenInput);
    RtSmokeBvhFrameTokenInput expectedFrameTokenInput = input.frameTokenInput;
    expectedFrameTokenInput.staticActiveSetSignature =
        firstResult.staticBucketWorkPlan.activeSetPlan.activeSetSignature;
    expectedFrameTokenInput.staticResidentSetSignature =
        firstResult.staticBucketWorkPlan.activeSetPlan.residentSetSignature;
    expectedFrameTokenInput.staticTlasInstanceSignature =
        expectedStaticDirtyToken.tlasInstanceSignature;
    const RtSmokeBvhFrameToken expectedFrameToken =
        BuildSmokeBvhFrameToken(expectedFrameTokenInput);
    Check(firstResult.staticBucketWorkPlan.activeSetPlan.activeBuckets == 1,
        "BVH frame planning result preserves active bucket count");
    Check(firstResult.staticBucketWorkPlan.activeSetPlan.inactiveResidentBuckets == 1,
        "BVH frame planning result preserves inactive resident bucket count");
    Check(firstResult.frameToken.dirtyToken.activeSetSignature ==
            expectedFrameToken.dirtyToken.activeSetSignature &&
        firstResult.frameToken.dirtyToken.residentSetSignature ==
            expectedFrameToken.dirtyToken.residentSetSignature,
        "BVH frame planning result composes static bucket work into frame token");
    Check(firstResult.dirtyPlan.blasInputDirty && firstResult.dirtyPlan.tlasDirty,
        "BVH frame planning result treats missing previous token as dirty");

    const uint64_t basePlanningToken = BuildSmokeBvhFramePlanningInputToken(input);
    Check(basePlanningToken == BuildSmokeBvhFramePlanningInputToken(input),
        "BVH frame planning input token is deterministic");
    RtSmokeBvhFramePlanningInput ignoredStaticFrameInput = input;
    ignoredStaticFrameInput.frameTokenInput.staticActiveSetSignature = 0x1234ull;
    ignoredStaticFrameInput.frameTokenInput.staticResidentSetSignature = 0x5678ull;
    ignoredStaticFrameInput.frameTokenInput.staticTlasInstanceSignature = 0x9ABCull;
    const RtSmokeBvhFramePlanningResult ignoredStaticFrameResult =
        BuildSmokeBvhFramePlanningResult(ignoredStaticFrameInput);
    Check(BuildSmokeBvhFramePlanningInputToken(ignoredStaticFrameInput) == basePlanningToken &&
            ignoredStaticFrameResult.frameToken.dirtyToken.activeSetSignature ==
                firstResult.frameToken.dirtyToken.activeSetSignature &&
            ignoredStaticFrameResult.frameToken.dirtyToken.residentSetSignature ==
                firstResult.frameToken.dirtyToken.residentSetSignature &&
            ignoredStaticFrameResult.frameToken.dirtyToken.tlasInstanceSignature ==
                firstResult.frameToken.dirtyToken.tlasInstanceSignature,
        "BVH frame planning input token ignores caller static frame signatures overwritten by static bucket work");
    RtSmokeBvhFramePlanningInput invalidPreviousChangedInput = input;
    invalidPreviousChangedInput.previousDirtyToken.activeSetSignature = 0x12345678ull;
    invalidPreviousChangedInput.previousDirtyToken.tlasInstanceSignature = 0x87654321ull;
    Check(BuildSmokeBvhFramePlanningInputToken(invalidPreviousChangedInput) == basePlanningToken,
        "BVH frame planning input token ignores invalid previous dirty token contents");
    RtSmokeBvhFramePlanningInput previousValidInput = input;
    previousValidInput.previousDirtyTokenValid = true;
    previousValidInput.previousDirtyToken = firstResult.frameToken.dirtyToken;
    const uint64_t previousValidPlanningToken =
        BuildSmokeBvhFramePlanningInputToken(previousValidInput);
    Check(previousValidPlanningToken != basePlanningToken,
        "BVH frame planning input token tracks previous dirty token validity");
    RtSmokeBvhFramePlanningInput previousChangedInput = previousValidInput;
    previousChangedInput.previousDirtyToken.activeSetSignature ^= 0x4000ull;
    Check(BuildSmokeBvhFramePlanningInputToken(previousChangedInput) !=
            previousValidPlanningToken,
        "BVH frame planning input token tracks previous dirty token contents");
    RtSmokeBvhFramePlanningInput rigidSignatureChangedPlanningInput = input;
    rigidSignatureChangedPlanningInput.frameTokenInput.rigidTlasInstanceSignature ^= 0x40ull;
    Check(BuildSmokeBvhFramePlanningInputToken(rigidSignatureChangedPlanningInput) !=
            basePlanningToken,
        "BVH frame planning input token tracks rigid TLAS instance signatures");
    const RtSmokeBvhFramePlanningSnapshot snapshot =
        CaptureSmokeBvhFramePlanningSnapshot(input);
    const uint64_t snapshotPlanningToken =
        BuildSmokeBvhFramePlanningInputToken(snapshot);
    buckets[0].bucketKey = 999;
    const RtSmokeBvhFramePlanningResult snapshotResult =
        BuildSmokeBvhFramePlanningResult(snapshot);
    const RtSmokeBvhFramePlanningResult mutatedInputResult =
        BuildSmokeBvhFramePlanningResult(input);
    Check(snapshotResult.frameToken.dirtyToken.activeSetSignature ==
            firstResult.frameToken.dirtyToken.activeSetSignature &&
        snapshotResult.frameToken.dirtyToken.residentSetSignature ==
            firstResult.frameToken.dirtyToken.residentSetSignature &&
        mutatedInputResult.frameToken.dirtyToken.activeSetSignature !=
            snapshotResult.frameToken.dirtyToken.activeSetSignature,
        "owned BVH frame planning snapshot is immutable after source mutation");
    Check(snapshotPlanningToken == basePlanningToken &&
        BuildSmokeBvhFramePlanningInputToken(input) != snapshotPlanningToken,
        "BVH frame planning snapshot token is immutable after source mutation");
    const RtSmokeBvhFramePlanningTimedResult timedResult =
        BuildSmokeBvhFramePlanningTimedResult(snapshot);
    Check(timedResult.result.frameToken.dirtyToken.tlasInstanceSignature ==
            snapshotResult.frameToken.dirtyToken.tlasInstanceSignature,
        "timed BVH frame planning result preserves snapshot planning output");
    buckets[0].bucketKey = 100;

    input.previousDirtyTokenValid = true;
    input.previousDirtyToken = firstResult.frameToken.dirtyToken;
    const RtSmokeBvhFramePlanningSnapshot previousTokenSnapshot =
        CaptureSmokeBvhFramePlanningSnapshot(input);
    input.previousDirtyTokenValid = false;
    const RtSmokeBvhFramePlanningResult previousTokenSnapshotResult =
        BuildSmokeBvhFramePlanningResult(previousTokenSnapshot);
    const RtSmokeBvhFramePlanningResult previousTokenMutatedInputResult =
        BuildSmokeBvhFramePlanningResult(input);
    Check(!previousTokenSnapshotResult.dirtyPlan.blasInputDirty &&
        !previousTokenSnapshotResult.dirtyPlan.tlasDirty &&
        previousTokenMutatedInputResult.dirtyPlan.blasInputDirty &&
        previousTokenMutatedInputResult.dirtyPlan.tlasDirty,
        "owned BVH frame planning snapshot preserves previous dirty token state");
    input.previousDirtyTokenValid = true;
    const RtSmokeBvhFramePlanningResult unchangedResult =
        BuildSmokeBvhFramePlanningResult(input);
    Check(!unchangedResult.dirtyPlan.blasInputDirty &&
        !unchangedResult.dirtyPlan.tlasDirty,
        "BVH frame planning result keeps unchanged inputs clean");

    buckets[1].active = true;
    const RtSmokeBvhFramePlanningResult activeChangedResult =
        BuildSmokeBvhFramePlanningResult(input);
    Check(!activeChangedResult.dirtyPlan.blasInputDirty &&
        activeChangedResult.dirtyPlan.tlasDirty,
        "BVH frame planning result maps active bucket changes to TLAS work");

    RtSmokeStaticTlasBucketObservation routeBuckets[2];
    routeBuckets[0] = buckets[0];
    routeBuckets[0].bucketKey = 300;
    routeBuckets[0].active = true;
    routeBuckets[0].routeRecordIndex = 10;
    routeBuckets[1] = routeBuckets[0];
    routeBuckets[1].bucketKey = 400;
    routeBuckets[1].routeRecordIndex = 11;
    RtSmokeBvhFramePlanningInput routeInput = input;
    routeInput.staticBucketWorkInput.buckets = routeBuckets;
    routeInput.staticBucketWorkInput.bucketCount = 2;
    routeInput.staticBucketWorkInput.monolithicStaticBlas = false;
    routeInput.staticBucketWorkInput.enableStaticRoutes = true;
    routeInput.staticBucketWorkInput.shaderSupportsStaticBucketRoutes = true;
    routeInput.staticBucketWorkInput.maxRouteRecords = 0;
    routeInput.previousDirtyTokenValid = false;
    const RtSmokeBvhFramePlanningResult routeBaseResult =
        BuildSmokeBvhFramePlanningResult(routeInput);
    routeInput.previousDirtyTokenValid = true;
    routeInput.previousDirtyToken = routeBaseResult.frameToken.dirtyToken;
    routeBuckets[1].routeRecordIndex = 12;
    const RtSmokeBvhFramePlanningResult routeChangedResult =
        BuildSmokeBvhFramePlanningResult(routeInput);
    Check(!routeChangedResult.dirtyPlan.blasInputDirty &&
        !routeChangedResult.dirtyPlan.activeMembershipChanged &&
        routeChangedResult.dirtyPlan.tlasInstanceChanged &&
        routeChangedResult.dirtyPlan.tlasDirty,
        "BVH frame planning result maps static route-table changes to TLAS-only work");
}

void TestAsyncBvhFramePlanning()
{
    RtSmokeStaticTlasBucketObservation bucket;
    bucket.bucketKey = 300;
    bucket.resident = true;
    bucket.active = true;
    bucket.hasBlas = true;
    bucket.activeReasonFlags = RT_SMOKE_STATIC_ACTIVE_SELECTED_AREA;
    bucket.residentVertexCount = 12;
    bucket.residentIndexCount = 36;
    bucket.residentTriangleCount = 12;
    bucket.activeVertexCount = 12;
    bucket.activeIndexCount = 36;
    bucket.activeTriangleCount = 12;

    RtSmokeBvhFramePlanningInput input;
    input.staticBucketWorkInput.buckets = &bucket;
    input.staticBucketWorkInput.bucketCount = 1;
    input.staticBucketWorkInput.geometryContentSignature = 1300;
    input.staticBucketWorkInput.materialGeneration = 1301;
    input.staticBucketWorkInput.totalVertexCount = 12;
    input.staticBucketWorkInput.totalIndexCount = 36;
    input.staticBucketWorkInput.totalTriangleCount = 12;
    input.staticBucketWorkInput.monolithicStaticBlas = true;
    input.staticBucketWorkInput.hasStaticBlas = true;
    input.frameTokenInput.staticBlasSignature = 1302;
    input.frameTokenInput.geometryGeneration = 1303;
    input.frameTokenInput.materialGeneration = 1304;
    input.frameTokenInput.dynamicVertexCount = 4;
    input.frameTokenInput.dynamicIndexCount = 6;
    input.frameTokenInput.baseTlasInstanceCount = 2;
    input.frameTokenInput.rigidTlasInstanceCount = 0;
    input.frameTokenInput.hasStaticBlas = true;
    input.frameTokenInput.hasDynamicBlas = true;
    const RtSmokeBvhFramePlanningSnapshot snapshot =
        CaptureSmokeBvhFramePlanningSnapshot(input);

    RtPathTraceCpuWorkState state;
    RtPathTraceCpuWorkGeneration generation;
    generation.frameIndex = 91;
    generation.sceneGeneration = 92;
    generation.geometryGeneration = input.frameTokenInput.geometryGeneration;
    generation.materialGeneration = input.frameTokenInput.materialGeneration;
    generation.lightGeneration = BuildSmokeBvhFramePlanningInputToken(snapshot);
    RtPathTraceCpuWorkPublishSnapshot(state, generation);

    std::future<RtSmokeBvhFramePlanningTimedResult> future = std::async(
        std::launch::async,
        [snapshot]() {
            return BuildSmokeBvhFramePlanningTimedResult(snapshot);
        });

    const RtPathTraceCpuWorkFrameDecision lateDecision =
        RtPathTraceCpuWorkAcceptLatest(state, generation, nullptr, true);
    Check(lateDecision.lateFallback && lateDecision.syncFallback,
        "late async BVH frame planning requests synchronous fallback");

    const RtSmokeBvhFramePlanningTimedResult timedResult = future.get();
    RtPathTraceCpuWorkResultEnvelope envelope;
    envelope.completed = true;
    envelope.generation = generation;
    envelope.timing.workerExecutionMs = static_cast<double>(timedResult.planningTimeMicros) / 1000.0;
    RtPathTraceCpuWorkPublishCompletedResult(state, envelope);
    const RtPathTraceCpuWorkFrameDecision acceptDecision =
        RtPathTraceCpuWorkAcceptLatest(state, generation, nullptr, true);
    Check(acceptDecision.accepted &&
        timedResult.result.staticBucketWorkPlan.activeSetPlan.activeBuckets == 1 &&
        state.acceptedResultCount == 1,
        "matching async BVH frame planning generation is accepted");

    RtPathTraceCpuWorkResultEnvelope staleEnvelope = envelope;
    staleEnvelope.generation.lightGeneration ^= 0x1000ull;
    const RtPathTraceCpuWorkFrameDecision staleDecision =
        RtPathTraceCpuWorkAcceptLatest(state, generation, &staleEnvelope, true);
    Check(staleDecision.staleRejected && staleDecision.syncFallback,
        "stale async BVH frame planning generation is rejected");
}

void TestBvhBucketableSignatures()
{
    RtSmokeStaticBvhBucketSignatureInput staticInput;
    staticInput.geometryContentSignature = 1000;
    staticInput.materialGeneration = 2000;
    staticInput.bucket.bucketKey = 77;
    staticInput.bucket.resident = true;
    staticInput.bucket.active = true;
    staticInput.bucket.activeReasonFlags = RT_SMOKE_STATIC_ACTIVE_VISIBLE;
    staticInput.bucket.routeRecordIndex = 3;
    staticInput.bucket.residentSurfaceCount = 2;
    staticInput.bucket.residentVertexCount = 12;
    staticInput.bucket.residentIndexCount = 36;
    staticInput.bucket.residentTriangleCount = 12;
    staticInput.bucket.activeSurfaceCount = 1;
    staticInput.bucket.activeVertexCount = 6;
    staticInput.bucket.activeIndexCount = 18;
    staticInput.bucket.activeTriangleCount = 6;
    const RtSmokeStaticBvhBucketSignature baseStaticSignature =
        BuildSmokeStaticBvhBucketSignature(staticInput);
    Check(baseStaticSignature.bucketKey == 77 && baseStaticSignature.resident &&
        baseStaticSignature.active && baseStaticSignature.residentSignature != 0 &&
        baseStaticSignature.activeSignature != 0 && baseStaticSignature.geometryInputSignature != 0 &&
        baseStaticSignature.blasInputSignature != 0,
        "static BVH bucket signature preserves bucket identity and state");

    RtSmokeStaticBvhBucketSignatureInput activeChangedInput = staticInput;
    activeChangedInput.bucket.activeReasonFlags = RT_SMOKE_STATIC_ACTIVE_VISIBLE | RT_SMOKE_STATIC_ACTIVE_FORCE_INCLUDE;
    const RtSmokeStaticBvhBucketSignature activeChangedSignature =
        BuildSmokeStaticBvhBucketSignature(activeChangedInput);
    Check(baseStaticSignature.activeSignature != activeChangedSignature.activeSignature &&
        baseStaticSignature.residentSignature == activeChangedSignature.residentSignature &&
        baseStaticSignature.geometryInputSignature == activeChangedSignature.geometryInputSignature &&
        baseStaticSignature.blasInputSignature == activeChangedSignature.blasInputSignature,
        "static BVH bucket active changes do not dirty resident or BLAS signatures");

    RtSmokeStaticBvhBucketSignatureInput routeChangedInput = staticInput;
    routeChangedInput.bucket.routeRecordIndex = 9;
    const RtSmokeStaticBvhBucketSignature routeChangedSignature =
        BuildSmokeStaticBvhBucketSignature(routeChangedInput);
    Check(baseStaticSignature.activeSignature == routeChangedSignature.activeSignature &&
        baseStaticSignature.residentSignature == routeChangedSignature.residentSignature &&
        baseStaticSignature.geometryInputSignature == routeChangedSignature.geometryInputSignature &&
        baseStaticSignature.blasInputSignature == routeChangedSignature.blasInputSignature,
        "static BVH bucket route changes do not dirty active membership or BLAS signatures");

    RtSmokeStaticBvhBucketSignatureInput inactiveInput = staticInput;
    inactiveInput.bucket.active = false;
    inactiveInput.bucket.activeReasonFlags = 0;
    inactiveInput.bucket.activeSurfaceCount = 0;
    inactiveInput.bucket.activeVertexCount = 0;
    inactiveInput.bucket.activeIndexCount = 0;
    inactiveInput.bucket.activeTriangleCount = 0;
    const RtSmokeStaticBvhBucketSignature inactiveSignature =
        BuildSmokeStaticBvhBucketSignature(inactiveInput);
    RtSmokeStaticBvhBucketSignatureInput inactiveActiveMetadataChangedInput = inactiveInput;
    inactiveActiveMetadataChangedInput.bucket.activeReasonFlags =
        RT_SMOKE_STATIC_ACTIVE_VISIBLE | RT_SMOKE_STATIC_ACTIVE_FORCE_INCLUDE;
    inactiveActiveMetadataChangedInput.bucket.activeSurfaceCount = 2;
    inactiveActiveMetadataChangedInput.bucket.activeVertexCount = 6;
    inactiveActiveMetadataChangedInput.bucket.activeIndexCount = 18;
    inactiveActiveMetadataChangedInput.bucket.activeTriangleCount = 6;
    const RtSmokeStaticBvhBucketSignature inactiveActiveMetadataChangedSignature =
        BuildSmokeStaticBvhBucketSignature(inactiveActiveMetadataChangedInput);
    Check(inactiveSignature.activeSignature == inactiveActiveMetadataChangedSignature.activeSignature &&
        inactiveSignature.residentSignature == inactiveActiveMetadataChangedSignature.residentSignature &&
        inactiveSignature.geometryInputSignature == inactiveActiveMetadataChangedSignature.geometryInputSignature &&
        inactiveSignature.blasInputSignature == inactiveActiveMetadataChangedSignature.blasInputSignature,
        "static BVH bucket signature ignores inactive active metadata");

    RtSmokeStaticBvhBucketSignatureInput geometryChangedInput = staticInput;
    geometryChangedInput.geometryContentSignature = 1001;
    const RtSmokeStaticBvhBucketSignature geometryChangedSignature =
        BuildSmokeStaticBvhBucketSignature(geometryChangedInput);
    Check(baseStaticSignature.geometryInputSignature != geometryChangedSignature.geometryInputSignature &&
        baseStaticSignature.blasInputSignature != geometryChangedSignature.blasInputSignature &&
        baseStaticSignature.activeSignature == geometryChangedSignature.activeSignature &&
        baseStaticSignature.residentSignature == geometryChangedSignature.residentSignature,
        "static BVH bucket geometry changes dirty BLAS signature without changing active membership");

    RtSmokeStaticBvhBucketSignatureInput materialChangedInput = staticInput;
    materialChangedInput.materialGeneration = 2001;
    const RtSmokeStaticBvhBucketSignature materialChangedSignature =
        BuildSmokeStaticBvhBucketSignature(materialChangedInput);
    Check(baseStaticSignature.geometryInputSignature == materialChangedSignature.geometryInputSignature &&
        baseStaticSignature.blasInputSignature == materialChangedSignature.blasInputSignature &&
        baseStaticSignature.activeSignature == materialChangedSignature.activeSignature &&
        baseStaticSignature.residentSignature == materialChangedSignature.residentSignature,
        "static BVH bucket material changes do not dirty geometry BLAS signature");

    RtSmokeStaticBvhBucketSignatureInput rangeChangedInput = staticInput;
    rangeChangedInput.bucket.residentVertexOffset = 12;
    const RtSmokeStaticBvhBucketSignature rangeChangedSignature =
        BuildSmokeStaticBvhBucketSignature(rangeChangedInput);
    Check(baseStaticSignature.geometryInputSignature != rangeChangedSignature.geometryInputSignature &&
        baseStaticSignature.blasInputSignature != rangeChangedSignature.blasInputSignature &&
        baseStaticSignature.activeSignature == rangeChangedSignature.activeSignature,
        "static BVH bucket range changes dirty BLAS signature");

    RtSmokeRigidBvhObjectSignatureInput rigidInput;
    rigidInput.meshHash = 500;
    rigidInput.instanceId = 600;
    rigidInput.geometryContentSignature = 700;
    rigidInput.materialGeneration = 800;
    rigidInput.sourceFlags = 0x2;
    rigidInput.rigidSourceMask = 0x2;
    rigidInput.routeRecordIndex = 9;
    rigidInput.vertexCount = 24;
    rigidInput.indexCount = 72;
    rigidInput.hasMeshRecord = true;
    rigidInput.meshSeenThisFrame = true;
    const RtSmokeRigidBvhObjectSignature baseRigidSignature =
        BuildSmokeRigidBvhObjectSignature(rigidInput);
    Check(baseRigidSignature.resident && baseRigidSignature.activeCandidate &&
        baseRigidSignature.objectKey != 0 && baseRigidSignature.geometryInputSignature != 0 &&
        baseRigidSignature.blasInputSignature != 0 &&
        baseRigidSignature.tlasMembershipSignature != 0,
        "rigid BVH object signature preserves object identity and active state");

    RtSmokeRigidBvhObjectSignatureInput secondRigidInstanceInput = rigidInput;
    secondRigidInstanceInput.instanceId = 601;
    const RtSmokeRigidBvhObjectSignature secondRigidInstanceSignature =
        BuildSmokeRigidBvhObjectSignature(secondRigidInstanceInput);
    Check(baseRigidSignature.objectKey != secondRigidInstanceSignature.objectKey &&
        baseRigidSignature.geometryInputSignature == secondRigidInstanceSignature.geometryInputSignature &&
        baseRigidSignature.blasInputSignature == secondRigidInstanceSignature.blasInputSignature &&
        baseRigidSignature.tlasMembershipSignature != secondRigidInstanceSignature.tlasMembershipSignature,
        "rigid BVH object signature shares mesh BLAS inputs across distinct instances");

    RtSmokeRigidBvhObjectSignatureInput rigidGeometryChangedInput = rigidInput;
    rigidGeometryChangedInput.geometryContentSignature = 701;
    const RtSmokeRigidBvhObjectSignature rigidGeometryChangedSignature =
        BuildSmokeRigidBvhObjectSignature(rigidGeometryChangedInput);
    Check(baseRigidSignature.geometryInputSignature != rigidGeometryChangedSignature.geometryInputSignature &&
        baseRigidSignature.blasInputSignature != rigidGeometryChangedSignature.blasInputSignature &&
        baseRigidSignature.tlasMembershipSignature == rigidGeometryChangedSignature.tlasMembershipSignature,
        "rigid BVH object geometry changes dirty BLAS signature without changing TLAS membership");

    RtSmokeRigidBvhObjectSignatureInput rigidEquivalentSourceInput = rigidInput;
    rigidEquivalentSourceInput.sourceFlags = 0x6;
    const RtSmokeRigidBvhObjectSignature rigidEquivalentSourceSignature =
        BuildSmokeRigidBvhObjectSignature(rigidEquivalentSourceInput);
    Check(baseRigidSignature.activeCandidate &&
        rigidEquivalentSourceSignature.activeCandidate &&
        baseRigidSignature.tlasMembershipSignature == rigidEquivalentSourceSignature.tlasMembershipSignature,
        "rigid BVH object signature ignores source flag changes with unchanged TLAS membership");

    RtSmokeRigidBvhObjectSignatureInput rigidMaterialChangedInput = rigidInput;
    rigidMaterialChangedInput.materialGeneration = 801;
    const RtSmokeRigidBvhObjectSignature rigidMaterialChangedSignature =
        BuildSmokeRigidBvhObjectSignature(rigidMaterialChangedInput);
    Check(baseRigidSignature.geometryInputSignature == rigidMaterialChangedSignature.geometryInputSignature &&
        baseRigidSignature.blasInputSignature == rigidMaterialChangedSignature.blasInputSignature &&
        baseRigidSignature.tlasMembershipSignature == rigidMaterialChangedSignature.tlasMembershipSignature,
        "rigid BVH object material changes do not dirty geometry BLAS signature");

    RtSmokeRigidBvhObjectSignatureInput staleResidentInput = rigidInput;
    staleResidentInput.meshSeenThisFrame = false;
    staleResidentInput.residencyEnabled = true;
    const RtSmokeRigidBvhObjectSignature staleResidentSignature =
        BuildSmokeRigidBvhObjectSignature(staleResidentInput);
    Check(staleResidentSignature.resident && staleResidentSignature.activeCandidate &&
        baseRigidSignature.blasInputSignature == staleResidentSignature.blasInputSignature,
        "rigid BVH object signature keeps residency separate from BLAS input identity");

    RtSmokeRigidBvhObjectSignatureInput inactiveRigidInput = rigidInput;
    inactiveRigidInput.sourceFlags = 0x4;
    const RtSmokeRigidBvhObjectSignature inactiveRigidSignature =
        BuildSmokeRigidBvhObjectSignature(inactiveRigidInput);
    Check(inactiveRigidSignature.resident && !inactiveRigidSignature.activeCandidate &&
        baseRigidSignature.blasInputSignature == inactiveRigidSignature.blasInputSignature,
        "rigid BVH object signature separates resident objects from active TLAS candidates");

    RtSmokeRigidBvhObjectSignatureInput inactiveRouteChangedInput = inactiveRigidInput;
    inactiveRouteChangedInput.routeRecordIndex = 12;
    const RtSmokeRigidBvhObjectSignature inactiveRouteChangedSignature =
        BuildSmokeRigidBvhObjectSignature(inactiveRouteChangedInput);
    Check(inactiveRigidSignature.tlasMembershipSignature ==
            inactiveRouteChangedSignature.tlasMembershipSignature &&
        baseRigidSignature.tlasMembershipSignature != inactiveRouteChangedSignature.tlasMembershipSignature,
        "rigid BVH object signature ignores route metadata for inactive TLAS candidates");
}

void TestUploadPlan()
{
    const RtSmokeUploadPlanMetadata fullUpload = BuildSmokeVectorUploadPlanMetadata(10, 4, false, -1, 0);
    Check(!fullUpload.skip && fullUpload.byteSize == 40 && fullUpload.sourceOffsetBytes == 0, "upload plan emits full upload metadata");

    const RtSmokeUploadPlanMetadata dirtyUpload = BuildSmokeVectorUploadPlanMetadata(10, 4, false, 2, 3);
    Check(!dirtyUpload.skip && dirtyUpload.byteSize == 12 && dirtyUpload.sourceOffsetBytes == 8 && dirtyUpload.destOffsetBytes == 8, "upload plan emits dirty-range metadata");

    const RtSmokeUploadPlanMetadata invalidDirtyUpload = BuildSmokeVectorUploadPlanMetadata(10, 4, false, 8, 8);
    Check(!invalidDirtyUpload.skip && invalidDirtyUpload.byteSize == 40 && invalidDirtyUpload.sourceOffsetBytes == 0, "upload plan falls back to full upload for invalid dirty ranges");

    const RtSmokeUploadPlanMetadata skippedUpload = BuildSmokeVectorUploadPlanMetadata(10, 4, true, 2, 3);
    Check(skippedUpload.skip && skippedUpload.byteSize == 0, "upload plan preserves explicit skip");

    const RtSmokeUploadPlanMetadata zeroUpload = BuildSmokeVectorUploadPlanMetadata(0, 4, false, -1, 0);
    Check(!zeroUpload.skip && zeroUpload.byteSize == 0, "upload plan preserves zero-byte non-skip barriers");

    RtSmokeStaticDirtyUploadPlanInput dirtyPlanInput;
    dirtyPlanInput.staticCacheChanged = true;
    dirtyPlanInput.staticGeometryBuffersReused = true;
    dirtyPlanInput.staticDirtyCount = 1;
    dirtyPlanInput.dirtyVertexOffset = 2;
    dirtyPlanInput.dirtyVertexCount = 3;
    dirtyPlanInput.totalVertexCount = 10;
    dirtyPlanInput.dirtyIndexOffset = 3;
    dirtyPlanInput.dirtyIndexCount = 6;
    dirtyPlanInput.totalIndexCount = 18;
    dirtyPlanInput.dirtyTriangleOffset = 1;
    dirtyPlanInput.dirtyTriangleCount = 2;
    dirtyPlanInput.totalTriangleClassCount = 6;
    dirtyPlanInput.totalTriangleMaterialCount = 6;
    const RtSmokeStaticDirtyUploadPlan dirtyPlan = BuildSmokeStaticDirtyUploadPlan(dirtyPlanInput);
    Check(dirtyPlan.dirtyRangesValid && dirtyPlan.useDirtyRangeUploads, "static dirty upload plan accepts valid reused dirty ranges");

    RtSmokeStaticDirtyUploadPlanInput cacheHitDirtyPlanInput = dirtyPlanInput;
    cacheHitDirtyPlanInput.staticBlasCacheHit = true;
    const RtSmokeStaticDirtyUploadPlan cacheHitDirtyPlan = BuildSmokeStaticDirtyUploadPlan(cacheHitDirtyPlanInput);
    Check(cacheHitDirtyPlan.dirtyRangesValid && !cacheHitDirtyPlan.useDirtyRangeUploads, "static dirty upload plan skips dirty ranges on static BLAS cache hit");

    RtSmokeStaticDirtyUploadPlanInput unchangedDirtyPlanInput = dirtyPlanInput;
    unchangedDirtyPlanInput.staticCacheChanged = false;
    const RtSmokeStaticDirtyUploadPlan unchangedDirtyPlan = BuildSmokeStaticDirtyUploadPlan(unchangedDirtyPlanInput);
    Check(unchangedDirtyPlan.dirtyRangesValid && !unchangedDirtyPlan.useDirtyRangeUploads,
        "static dirty upload plan skips dirty ranges when static cache is unchanged");

    RtSmokeStaticDirtyUploadPlanInput emptyDirtyPlanInput = dirtyPlanInput;
    emptyDirtyPlanInput.staticDirtyCount = 0;
    const RtSmokeStaticDirtyUploadPlan emptyDirtyPlan = BuildSmokeStaticDirtyUploadPlan(emptyDirtyPlanInput);
    Check(emptyDirtyPlan.dirtyRangesValid && !emptyDirtyPlan.useDirtyRangeUploads,
        "static dirty upload plan skips dirty ranges when no dirty records exist");

    RtSmokeStaticDirtyUploadPlanInput recreatedBufferDirtyPlanInput = dirtyPlanInput;
    recreatedBufferDirtyPlanInput.staticGeometryBuffersReused = false;
    const RtSmokeStaticDirtyUploadPlan recreatedBufferDirtyPlan =
        BuildSmokeStaticDirtyUploadPlan(recreatedBufferDirtyPlanInput);
    Check(recreatedBufferDirtyPlan.dirtyRangesValid && !recreatedBufferDirtyPlan.useDirtyRangeUploads,
        "static dirty upload plan skips dirty ranges when geometry buffers are recreated");

    RtSmokeStaticDirtyUploadPlanInput invalidDirtyPlanInput = dirtyPlanInput;
    invalidDirtyPlanInput.dirtyTriangleCount = 99;
    const RtSmokeStaticDirtyUploadPlan invalidDirtyPlan = BuildSmokeStaticDirtyUploadPlan(invalidDirtyPlanInput);
    Check(!invalidDirtyPlan.dirtyRangesValid && !invalidDirtyPlan.useDirtyRangeUploads, "static dirty upload plan rejects invalid dirty ranges");

    RtSmokeStaticVertexUploadPlanInput staticVertexPlanInput;
    staticVertexPlanInput.totalVertexCount = 100;
    const RtSmokeStaticVertexUploadPlan initialStaticVertexPlan =
        BuildSmokeStaticVertexUploadPlan(staticVertexPlanInput);
    Check(initialStaticVertexPlan.fullUpload && !initialStaticVertexPlan.skipUpload &&
            initialStaticVertexPlan.elementOffset == -1 && initialStaticVertexPlan.elementCount == 0,
        "static vertex upload plan fully initializes a cache-miss buffer");

    RtSmokeStaticVertexUploadPlanInput cachedStaticVertexPlanInput = staticVertexPlanInput;
    cachedStaticVertexPlanInput.staticBlasCacheHit = true;
    const RtSmokeStaticVertexUploadPlan cachedStaticVertexPlan =
        BuildSmokeStaticVertexUploadPlan(cachedStaticVertexPlanInput);
    Check(cachedStaticVertexPlan.skipUpload && !cachedStaticVertexPlan.fullUpload,
        "static vertex upload plan skips unchanged cache-hit vertices");

    RtSmokeStaticVertexUploadPlanInput cachedTexMatrixPlanInput = cachedStaticVertexPlanInput;
    cachedTexMatrixPlanInput.texMatrixVertexCount = 4;
    cachedTexMatrixPlanInput.texMatrixFirstVertex = 20;
    cachedTexMatrixPlanInput.texMatrixLastVertex = 27;
    const RtSmokeStaticVertexUploadPlan cachedTexMatrixPlan =
        BuildSmokeStaticVertexUploadPlan(cachedTexMatrixPlanInput);
    Check(!cachedTexMatrixPlan.skipUpload && cachedTexMatrixPlan.texMatrixRangeUpload &&
            cachedTexMatrixPlan.elementOffset == 20 && cachedTexMatrixPlan.elementCount == 8,
        "static vertex upload plan limits runtime texture matrices only on a cache hit");

    RtSmokeStaticVertexUploadPlanInput rebuiltTexMatrixPlanInput = cachedTexMatrixPlanInput;
    rebuiltTexMatrixPlanInput.staticBlasCacheHit = false;
    rebuiltTexMatrixPlanInput.fullUploadOnCacheMissWithTexMatrices = true;
    const RtSmokeStaticVertexUploadPlan rebuiltTexMatrixPlan =
        BuildSmokeStaticVertexUploadPlan(rebuiltTexMatrixPlanInput);
    Check(rebuiltTexMatrixPlan.fullUpload && !rebuiltTexMatrixPlan.texMatrixRangeUpload &&
            rebuiltTexMatrixPlan.elementOffset == -1 && rebuiltTexMatrixPlan.elementCount == 0,
        "static vertex upload plan fully initializes a cache-miss buffer with runtime texture matrices");

    RtSmokeStaticVertexUploadPlanInput legacyRebuiltTexMatrixPlanInput = rebuiltTexMatrixPlanInput;
    legacyRebuiltTexMatrixPlanInput.fullUploadOnCacheMissWithTexMatrices = false;
    const RtSmokeStaticVertexUploadPlan legacyRebuiltTexMatrixPlan =
        BuildSmokeStaticVertexUploadPlan(legacyRebuiltTexMatrixPlanInput);
    Check(legacyRebuiltTexMatrixPlan.texMatrixRangeUpload && !legacyRebuiltTexMatrixPlan.fullUpload &&
            legacyRebuiltTexMatrixPlan.elementOffset == 20 && legacyRebuiltTexMatrixPlan.elementCount == 8,
        "static vertex upload plan retains the gated legacy cache-miss texture-matrix range");

    RtSmokeStaticVertexUploadPlanInput dirtyStaticVertexPlanInput = staticVertexPlanInput;
    dirtyStaticVertexPlanInput.useDirtyRangeUploads = true;
    dirtyStaticVertexPlanInput.dirtyVertexOffset = 30;
    dirtyStaticVertexPlanInput.dirtyVertexCount = 12;
    const RtSmokeStaticVertexUploadPlan dirtyStaticVertexPlan =
        BuildSmokeStaticVertexUploadPlan(dirtyStaticVertexPlanInput);
    Check(dirtyStaticVertexPlan.dirtyRangeUpload &&
            dirtyStaticVertexPlan.elementOffset == 30 && dirtyStaticVertexPlan.elementCount == 12,
        "static vertex upload plan keeps valid dirty ranges without runtime texture matrices");

    RtSmokeStaticVertexUploadPlanInput dirtyTexMatrixPlanInput = dirtyStaticVertexPlanInput;
    dirtyTexMatrixPlanInput.fullUploadOnCacheMissWithTexMatrices = true;
    dirtyTexMatrixPlanInput.texMatrixVertexCount = 4;
    dirtyTexMatrixPlanInput.texMatrixFirstVertex = 70;
    dirtyTexMatrixPlanInput.texMatrixLastVertex = 75;
    const RtSmokeStaticVertexUploadPlan dirtyTexMatrixPlan =
        BuildSmokeStaticVertexUploadPlan(dirtyTexMatrixPlanInput);
    Check(dirtyTexMatrixPlan.fullUpload && !dirtyTexMatrixPlan.dirtyRangeUpload,
        "static vertex upload plan covers disjoint dirty and texture-matrix ranges with a full upload");

    RtSmokeStaticVertexUploadPlanInput noUploadRebuildPlanInput = rebuiltTexMatrixPlanInput;
    noUploadRebuildPlanInput.forceRebuildWithoutUpload = true;
    const RtSmokeStaticVertexUploadPlan noUploadRebuildPlan =
        BuildSmokeStaticVertexUploadPlan(noUploadRebuildPlanInput);
    Check(noUploadRebuildPlan.skipUpload && !noUploadRebuildPlan.fullUpload,
        "static vertex upload plan preserves the BLAS-only diagnostic mode");

    const uint32_t spanA[] = { 1, 2, 3, 4 };
    const uint32_t spanB[] = { 5, 6 };
    const RtSmokePlanDataSpan spans[] = {
        { spanA, sizeof(spanA[0]), 4 },
        { spanB, sizeof(spanB[0]), 2 }
    };
    const uint64_t spanSignature = BuildSmokePlanDataSpanSignature(spans, 2);
    Check(spanSignature == BuildSmokePlanDataSpanSignature(spans, 2), "data span upload signature is deterministic");
    const RtSmokePlanDataSpan truncatedSpans[] = {
        { spanA, sizeof(spanA[0]), 3 },
        { spanB, sizeof(spanB[0]), 2 }
    };
    Check(spanSignature != BuildSmokePlanDataSpanSignature(truncatedSpans, 2), "data span upload signature tracks element counts");
    const RtSmokePlanDataSpan resizedSpans[] = {
        { spanA, sizeof(uint16_t), 4 },
        { spanB, sizeof(spanB[0]), 2 }
    };
    Check(spanSignature != BuildSmokePlanDataSpanSignature(resizedSpans, 2), "data span upload signature tracks element sizes");
    const uint32_t zeroSpanA[] = { 0, 0, 0, 0 };
    const RtSmokePlanDataSpan zeroDataSpans[] = {
        { zeroSpanA, sizeof(zeroSpanA[0]), 4 },
        { spanB, sizeof(spanB[0]), 2 }
    };
    const RtSmokePlanDataSpan missingDataSpans[] = {
        { nullptr, sizeof(zeroSpanA[0]), 4 },
        { spanB, sizeof(spanB[0]), 2 }
    };
    Check(BuildSmokePlanDataSpanSignature(zeroDataSpans, 2) != BuildSmokePlanDataSpanSignature(missingDataSpans, 2),
        "data span upload signature separates missing payloads from zero-filled data");
    const RtSmokePlanDataSpan emptyNullSpan[] = {
        { nullptr, sizeof(zeroSpanA[0]), 0 }
    };
    const RtSmokePlanDataSpan emptyDataSpan[] = {
        { zeroSpanA, sizeof(zeroSpanA[0]), 0 }
    };
    Check(BuildSmokePlanDataSpanSignature(emptyNullSpan, 1) == BuildSmokePlanDataSpanSignature(emptyDataSpan, 1),
        "data span upload signature ignores backing pointers for empty spans");

    RtSmokePreviousStaticSnapshotUploadPlanInput previousStaticInput;
    previousStaticInput.dataAvailable = true;
    previousStaticInput.buffersReused = true;
    previousStaticInput.previousUploadSignature = spanSignature;
    previousStaticInput.currentUploadSignature = spanSignature;
    Check(BuildSmokePreviousStaticSnapshotUploadPlan(previousStaticInput).skipUpload, "previous static snapshot upload plan skips matching reused snapshot");
    previousStaticInput.currentUploadSignature = spanSignature + 1;
    Check(!BuildSmokePreviousStaticSnapshotUploadPlan(previousStaticInput).skipUpload, "previous static snapshot upload plan uploads changed snapshot");
    previousStaticInput.currentUploadSignature = spanSignature;
    previousStaticInput.buffersReused = false;
    Check(!BuildSmokePreviousStaticSnapshotUploadPlan(previousStaticInput).skipUpload, "previous static snapshot upload plan requires reused buffers");
}

void TestGenerationEquality()
{
    RtPathTraceCpuWorkGeneration base;
    base.frameIndex = 1;
    base.sceneGeneration = 2;
    base.geometryGeneration = 3;
    base.materialGeneration = 4;
    base.lightGeneration = 5;
    Check(RtPathTraceCpuWorkGenerationEquals(base, base),
        "CPU work generation equality accepts identical tokens");

    RtPathTraceCpuWorkGeneration changed = base;
    changed.frameIndex = 9;
    Check(!RtPathTraceCpuWorkGenerationEquals(base, changed),
        "CPU work generation equality tracks frame index");

    changed = base;
    changed.sceneGeneration = 9;
    Check(!RtPathTraceCpuWorkGenerationEquals(base, changed),
        "CPU work generation equality tracks scene generation");

    changed = base;
    changed.geometryGeneration = 9;
    Check(!RtPathTraceCpuWorkGenerationEquals(base, changed),
        "CPU work generation equality tracks geometry generation");

    changed = base;
    changed.materialGeneration = 9;
    Check(!RtPathTraceCpuWorkGenerationEquals(base, changed),
        "CPU work generation equality tracks material generation");

    changed = base;
    changed.lightGeneration = 9;
    Check(!RtPathTraceCpuWorkGenerationEquals(base, changed),
        "CPU work generation equality tracks light/input generation");
}

void TestGenerationAcceptance()
{
    RtPathTraceCpuWorkState incompleteState;
    RtPathTraceCpuWorkGeneration incompleteExpected;
    incompleteExpected.frameIndex = 4;
    RtPathTraceCpuWorkPublishSnapshot(incompleteState, incompleteExpected);
    RtPathTraceCpuWorkResultEnvelope incomplete;
    incomplete.completed = false;
    incomplete.generation = incompleteExpected;
    RtPathTraceCpuWorkPublishCompletedResult(incompleteState, incomplete);
    const RtPathTraceCpuWorkFrameDecision incompleteDecision =
        RtPathTraceCpuWorkAcceptLatest(incompleteState, incompleteExpected, nullptr, true);
    Check(incompleteDecision.lateFallback &&
        incompleteDecision.syncFallback &&
        !incompleteState.currentResult.valid &&
        !incompleteState.pendingResult.valid,
        "incomplete CPU work result is ignored and falls back as late work");

    RtPathTraceCpuWorkState state;
    RtPathTraceCpuWorkGeneration expected;
    expected.frameIndex = 5;
    expected.sceneGeneration = 1;
    expected.geometryGeneration = 2;
    expected.materialGeneration = 3;
    expected.lightGeneration = 4;
    RtPathTraceCpuWorkPublishSnapshot(state, expected);

    RtPathTraceCpuWorkResultEnvelope stale;
    stale.completed = true;
    stale.generation = expected;
    stale.generation.geometryGeneration = 99;
    const RtPathTraceCpuWorkFrameDecision staleDecision =
        RtPathTraceCpuWorkAcceptLatest(state, expected, &stale, true);
    Check(staleDecision.staleRejected && staleDecision.syncFallback, "stale generation tokens are rejected");

    const RtPathTraceCpuWorkFrameDecision lateDecision =
        RtPathTraceCpuWorkAcceptLatest(state, expected, nullptr, true);
    Check(lateDecision.lateFallback && lateDecision.syncFallback, "late results fall back without blocking");

    RtPathTraceCpuWorkState noSyncLateState;
    RtPathTraceCpuWorkPublishSnapshot(noSyncLateState, expected);
    const RtPathTraceCpuWorkFrameDecision noSyncLateDecision =
        RtPathTraceCpuWorkAcceptLatest(noSyncLateState, expected, nullptr, false);
    Check(noSyncLateDecision.lateFallback &&
        !noSyncLateDecision.syncFallback &&
        noSyncLateState.lateResultCount == 1 &&
        noSyncLateState.syncFallbackCount == 0,
        "late CPU work result can be reported without requesting sync fallback");

    RtPathTraceCpuWorkResultEnvelope current;
    current.completed = true;
    current.generation = expected;
    RtPathTraceCpuWorkPublishCompletedResult(state, current);
    const RtPathTraceCpuWorkFrameDecision acceptedDecision =
        RtPathTraceCpuWorkAcceptLatest(state, expected, nullptr, true);
    Check(acceptedDecision.accepted && state.acceptedResultCount == 1, "matching generation result is accepted");
    Check(state.currentResult.valid && !state.pendingResult.valid, "accepted CPU work result becomes current and clears pending");

    const RtPathTraceCpuWorkFrameDecision reusedCurrentDecision =
        RtPathTraceCpuWorkAcceptLatest(state, expected, nullptr, true);
    Check(reusedCurrentDecision.accepted && reusedCurrentDecision.reusedCurrent && !reusedCurrentDecision.lateFallback, "matching current CPU work result is reused without late fallback");

    RtPathTraceCpuWorkGeneration nextExpected = expected;
    nextExpected.frameIndex = 6;
    RtPathTraceCpuWorkPublishSnapshot(state, nextExpected);
    RtPathTraceCpuWorkResultEnvelope nextCurrent;
    nextCurrent.completed = true;
    nextCurrent.generation = nextExpected;
    RtPathTraceCpuWorkPublishCompletedResult(state, nextCurrent);
    const RtPathTraceCpuWorkFrameDecision nextAcceptedDecision =
        RtPathTraceCpuWorkAcceptLatest(state, nextExpected, nullptr, true);
    Check(nextAcceptedDecision.accepted && state.previousResult.valid, "accepted CPU work result preserves previous result slot");
    Check(RtPathTraceCpuWorkRecordRenderSubmit(state, nextExpected, 3.5), "render submit timing records against current generation");
    Check(state.lastAcceptedTiming.renderSubmitMs == 3.5, "render submit timing is retained on accepted result");
    Check(!RtPathTraceCpuWorkRecordRenderSubmit(state, expected, 9.0), "render submit timing rejects stale generation");
    Check(state.rejectedStaleResultCount == 1 && state.lateResultCount == 1 && state.syncFallbackCount == 2, "CPU work ownership counters track stale and late fallbacks");

    RtPathTraceCpuWorkGeneration stalePendingExpected = nextExpected;
    stalePendingExpected.frameIndex = 7;
    RtPathTraceCpuWorkPublishSnapshot(state, stalePendingExpected);
    RtPathTraceCpuWorkResultEnvelope stalePending;
    stalePending.completed = true;
    stalePending.generation = nextExpected;
    RtPathTraceCpuWorkPublishCompletedResult(state, stalePending);
    const RtPathTraceCpuWorkFrameDecision stalePendingDecision =
        RtPathTraceCpuWorkAcceptLatest(state, stalePendingExpected, nullptr, true);
    Check(stalePendingDecision.staleRejected && !state.pendingResult.valid && !state.hasPending, "stale pending CPU work result is discarded after rejection");

    RtPathTraceCpuWorkState explicitStalePendingState;
    RtPathTraceCpuWorkGeneration explicitExpected = expected;
    explicitExpected.frameIndex = 8;
    RtPathTraceCpuWorkPublishSnapshot(explicitStalePendingState, explicitExpected);
    RtPathTraceCpuWorkResultEnvelope explicitStalePending;
    explicitStalePending.completed = true;
    explicitStalePending.generation = expected;
    RtPathTraceCpuWorkPublishCompletedResult(explicitStalePendingState, explicitStalePending);
    const RtPathTraceCpuWorkFrameDecision explicitStalePendingDecision =
        RtPathTraceCpuWorkAcceptLatest(explicitStalePendingState, explicitExpected, &explicitStalePending, false);
    Check(explicitStalePendingDecision.staleRejected &&
        !explicitStalePendingState.pendingResult.valid &&
        !explicitStalePendingState.hasPending &&
        !explicitStalePendingDecision.syncFallback,
        "explicit stale CPU work result clears matching pending slot without sync fallback");
}

void RunStressMode(int iterations)
{
    const int safeIterations = iterations > 0 ? iterations : 100;
    const int triangleCount = 8192;
    const int vertexCount = triangleCount * 3;
    const int indexCount = triangleCount * 3;

    std::vector<HarnessSmokeVertex> vertices(vertexCount);
    std::vector<uint32_t> indexes(indexCount);
    std::vector<uint32_t> classes(triangleCount);
    std::vector<uint32_t> materials(triangleCount);
    for (int vertexIndex = 0; vertexIndex < vertexCount; ++vertexIndex)
    {
        vertices[vertexIndex].position[0] = static_cast<float>(vertexIndex % 127) * 0.125f;
        vertices[vertexIndex].position[1] = static_cast<float>((vertexIndex / 3) % 113) * 0.25f;
        vertices[vertexIndex].position[2] = static_cast<float>(vertexIndex % 17) * 0.5f;
        vertices[vertexIndex].position[3] = 1.0f;
        vertices[vertexIndex].normal[2] = 1.0f;
        vertices[vertexIndex].texCoord[0] = static_cast<float>(vertexIndex % 31) * 0.01f;
        indexes[vertexIndex] = static_cast<uint32_t>(vertexIndex);
    }
    for (int triangleIndex = 0; triangleIndex < triangleCount; ++triangleIndex)
    {
        classes[triangleIndex] = static_cast<uint32_t>(triangleIndex & 7);
        materials[triangleIndex] = static_cast<uint32_t>((triangleIndex % 257) + 1);
    }

    RtSmokeAccelerationPlanInput input = BuildPlanInput(vertices, indexes, classes, materials);
    input.staticCache.staticCacheChanged = true;
    input.staticCache.cacheValid = true;
    input.staticCache.cacheResourcesReady = true;

    const auto planStart = std::chrono::steady_clock::now();
    uint64_t hashAccumulator = 0;
    for (int iteration = 0; iteration < safeIterations; ++iteration)
    {
        RtSmokeAccelerationPlan plan = BuildSmokeAccelerationPlan(input);
        hashAccumulator ^= plan.staticSignature.hash + static_cast<uint64_t>(iteration);
    }
    const auto planEnd = std::chrono::steady_clock::now();

    const int rigidObservationCount = 4096;
    RtSmokeRigidTlasPlanDesc rigidDesc;
    rigidDesc.rigidSourceMask = 0x2;
    rigidDesc.firstInstanceId = 2;
    rigidDesc.instanceMask = 0x02;
    rigidDesc.maxInstances = rigidObservationCount;

    const auto rigidStart = std::chrono::steady_clock::now();
    int emittedAccumulator = 0;
    for (int iteration = 0; iteration < safeIterations; ++iteration)
    {
        RtSmokeRigidTlasPlan rigidPlan;
        rigidPlan.instances.reserve(rigidObservationCount);
        for (int observationIndex = 0; observationIndex < rigidObservationCount; ++observationIndex)
        {
            RtSmokeRigidTlasObservation observation;
            observation.meshHash = static_cast<uint64_t>(observationIndex + 1);
            observation.instanceId = static_cast<uint64_t>(iteration * rigidObservationCount + observationIndex);
            observation.sourceFlags = observationIndex % 8 == 0 ? 0u : 0x2u;
            observation.hasMeshRecord = observationIndex % 11 != 0;
            observation.meshSeenThisFrame = observationIndex % 13 != 0;
            observation.residencyEnabled = observationIndex % 17 == 0;
            observation.hasBlas = observationIndex % 19 != 0;
            observation.routeRecordIndex = static_cast<uint32_t>(observationIndex);
            observation.objectToWorld[0] = 1.0f;
            observation.objectToWorld[5] = 1.0f;
            observation.objectToWorld[10] = 1.0f;
            observation.objectToWorld[15] = 1.0f;
            if (!AppendSmokeRigidTlasPlanObservation(rigidPlan, rigidDesc, observation))
            {
                break;
            }
        }
        emittedAccumulator += rigidPlan.emittedInstances;
    }
    const auto rigidEnd = std::chrono::steady_clock::now();

    const double planMs = std::chrono::duration<double, std::milli>(planEnd - planStart).count();
    const double rigidMs = std::chrono::duration<double, std::milli>(rigidEnd - rigidStart).count();
    std::cout << "[STRESS] iterations=" << safeIterations
        << " staticTriangles=" << triangleCount
        << " staticPlanMs=" << planMs
        << " staticPlanAvgMs=" << (planMs / static_cast<double>(safeIterations))
        << " rigidObservations=" << rigidObservationCount
        << " rigidPlanMs=" << rigidMs
        << " rigidPlanAvgMs=" << (rigidMs / static_cast<double>(safeIterations))
        << " hashAccumulator=" << hashAccumulator
        << " emittedAccumulator=" << emittedAccumulator
        << "\n";
}

} // namespace

int main(int argc, char** argv)
{
    TestStaticSignature();
    TestStaticSignatureRanges();
    TestCacheAndBaseTlas();
    TestOwnedSnapshot();
    TestAccelerationPlanInputToken();
    TestAsyncSnapshotPlanning();
    TestRigidPlan();
    TestAsyncRigidTlasPlanning();
    TestRigidBlasBuildPlan();
    TestStaticBucketBlasBuildPlan();
    TestStaticBucketBlasBuildBatchPlan();
    TestStaticBucketBlasBuildObservationPlan();
    TestStaticBucketWorkPlan();
    TestStaticBucketPublicationEpochPlan();
    TestStaticBucketInstanceAddressPlan();
    TestStaticBucketSurfaceAddressPlan();
    TestStaticBucketResidentPackCachePlan();
    TestStaticBucketMaterialIndexCachePlan();
    TestStaticBucketCutoverPlan();
    TestStaticBucketRigidRouteNamespaceComposition();
    TestStaticBucketWorkPlanSnapshot();
    TestStaticBucketWorkPlanTimedResult();
    TestStaticBucketWorkPlanInputToken();
    TestAsyncStaticBucketWorkPlanning();
    TestStaticBucketAssignmentPlan();
    TestStaticActiveSetPlan();
    TestStaticBucketObservation();
    TestStaticBucketBlasPlan();
    TestStaticBucketTraversalCompatibility();
    TestRouteInstanceNamespacePlan();
    TestStaticRouteTablePlan();
    TestBvhDirtyPlan();
    TestStaticBucketWorkDirtyToken();
    TestBvhFrameToken();
    TestBvhFramePlanningResult();
    TestAsyncBvhFramePlanning();
    TestBvhBucketableSignatures();
    TestUploadPlan();
    TestGenerationEquality();
    TestGenerationAcceptance();

    if (g_failures != 0)
    {
        std::cout << "PT CPU planning harness failed: " << g_failures << "\n";
        return EXIT_FAILURE;
    }

    std::cout << "PT CPU planning harness passed\n";
    if (argc >= 2 && std::string(argv[1]) == "--stress")
    {
        const int iterations = argc >= 3 ? std::atoi(argv[2]) : 100;
        RunStressMode(iterations);
    }
    return EXIT_SUCCESS;
}
