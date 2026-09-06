#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCommittedSemanticFacts.h"

#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace {

template<typename T>
bool AddSemanticScratchBytes(std::size_t count, std::size_t& bytes)
{
    if (count > (std::numeric_limits<std::size_t>::max() - bytes) / sizeof(T))
    {
        return false;
    }
    bytes += count * sizeof(T);
    return bytes <= RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES;
}

template<typename T>
bool ReserveSemanticScratch(
    std::vector<T>& values, std::size_t count,
    RtPathTracePrimarySemanticBridgeTestSeam* seam)
{
    if (seam)
    {
        const std::size_t call = seam->allocationCalls++;
        if (call == seam->failAtAllocation)
        {
            if (seam->failure ==
                RtPathTracePrimarySemanticBridgeFailure::LengthError)
            {
                throw std::length_error("semantic bridge test seam");
            }
            if (seam->failure == RtPathTracePrimarySemanticBridgeFailure::BadAlloc)
            {
                throw std::bad_alloc();
            }
        }
    }
    values.reserve(count);
    return values.capacity() >= count;
}

template<typename T>
std::size_t SemanticScratchVectorBytes(const std::vector<T>& values)
{
    return values.capacity() * sizeof(T);
}

template<typename T>
void AssignSemanticScratch(
    std::vector<T>& destination, const T* source, std::size_t count)
{
    if (count == 0)
    {
        destination.clear();
        return;
    }
    destination.assign(source, source + count);
}

std::size_t SemanticScratchOwnedBytes(
    const RtPathTracePrimarySemanticScratch& scratch)
{
    const RtPathTraceCaptureOwnerSnapshot& snapshot = scratch.snapshot;
    return sizeof(scratch) +
        SemanticScratchVectorBytes(snapshot.variantBases) +
        SemanticScratchVectorBytes(snapshot.registryMaterials) +
        SemanticScratchVectorBytes(snapshot.modelTables) +
        SemanticScratchVectorBytes(snapshot.modelSurfaceTokens) +
        SemanticScratchVectorBytes(snapshot.surfaces) +
        SemanticScratchVectorBytes(snapshot.classifierStages) +
        SemanticScratchVectorBytes(snapshot.runtimeStages) +
        SemanticScratchVectorBytes(snapshot.registers) +
        SemanticScratchVectorBytes(snapshot.vertices) +
        SemanticScratchVectorBytes(snapshot.indexes) +
        SemanticScratchVectorBytes(snapshot.joints);
}

} // namespace

RtPathTraceCommittedSemanticFactsProvider::
RtPathTraceCommittedSemanticFactsProvider(
    const RtPathTraceCommittedPlanningBaseline& value) noexcept :
    baseline(value)
{
}

bool RtPathTraceCommittedSemanticFactsProvider::Complete() const noexcept
{
    return baseline.Valid();
}

bool RtPathTraceCommittedSemanticFactsProvider::IsRigidRouteReady(
    std::uint64_t meshHash) const
{
    ++rigidReadyQueries;
    if (!baseline.Valid() || meshHash == 0 ||
        !RtPathTraceCommittedRigidQueryEpochValid(
            baseline.epoch, baseline.epoch.ownerUniverseFrameIndex))
    {
        return false;
    }
    for (const RtSmokeRigidRouteReadyPod& row :
        baseline.geometryUniverse.rigidRoutes)
    {
        if (row.meshHash == meshHash)
        {
            return RtPathTraceRigidRouteReadyRecordFromPod(row);
        }
    }
    return false;
}

bool RtPathTraceCommittedSemanticFactsProvider::
IsRigidRouteResidentReadyForEntityMaterial(
    std::int32_t entityIndex, std::int32_t renderEntityNum,
    std::uint32_t materialId) const
{
    ++residentReadyQueries;
    if (!baseline.Valid() || entityIndex < 0 || renderEntityNum < 0 ||
        materialId == 0)
    {
        return false;
    }
    for (const RtSmokeRigidResidentReadyPod& row :
        baseline.geometryUniverse.rigidResidents)
    {
        if (row.entityIndex == entityIndex &&
            row.renderEntityNum == renderEntityNum &&
            row.materialId == materialId && IsRigidRouteReady(row.meshHash))
        {
            return true;
        }
    }
    return false;
}

const PtGeometryIdentityBinding*
RtPathTraceCommittedSemanticFactsProvider::FindCanonicalIdentityBinding(
    const PtCanonicalInstanceKey&) const
{
    ++canonicalQueryViolations;
    return nullptr;
}

const PtGeometrySourceRecord*
RtPathTraceCommittedSemanticFactsProvider::FindCanonicalSourceRecord(
    const PtCanonicalMeshKey&) const
{
    ++canonicalQueryViolations;
    return nullptr;
}

std::size_t RtPathTraceCommittedSemanticFactsProvider::
CanonicalQueryViolationCount() const noexcept
{
    return canonicalQueryViolations;
}

std::size_t RtPathTraceCommittedSemanticFactsProvider::
RigidReadyQueryCount() const noexcept
{
    return rigidReadyQueries;
}

std::size_t RtPathTraceCommittedSemanticFactsProvider::
ResidentReadyQueryCount() const noexcept
{
    return residentReadyQueries;
}

bool BuildPathTracePrimarySemanticScratch(
    const RtPathTracePrimarySemanticDto& dto,
    std::uint64_t expectedSealedPrimaryViewToken,
    const RtPathTraceCommittedSemanticConfig& semanticConfig,
    RtPathTracePrimarySemanticScratch& output,
    RtPathTracePrimarySemanticBridgeTestSeam* testSeam) noexcept
{
    if (expectedSealedPrimaryViewToken == 0 ||
        !semanticConfig.configComplete ||
        semanticConfig.configFingerprint == 0 ||
        !RtPathTracePrimarySemanticDtoShapeComplete(
            dto, expectedSealedPrimaryViewToken))
    {
        return false;
    }

    std::size_t requestedBytes = sizeof(RtPathTracePrimarySemanticScratch);
    if (!AddSemanticScratchBytes<RtPathTraceMaterialTextureVariantBasePod>(
            dto.variantBaseCount, requestedBytes) ||
        !AddSemanticScratchBytes<RtPathTraceCaptureRegistryMaterialPod>(
            dto.registryMaterialCount, requestedBytes) ||
        !AddSemanticScratchBytes<RtPathTraceCaptureModelTokenTablePod>(
            dto.modelTableCount, requestedBytes) ||
        !AddSemanticScratchBytes<std::uint64_t>(
            dto.modelSurfaceTokenCount, requestedBytes) ||
        !AddSemanticScratchBytes<RtPathTraceCaptureRawSurface>(
            dto.surfaceCount, requestedBytes) ||
        !AddSemanticScratchBytes<RtSmokeTranslucentClassifierStageInput>(
            dto.classifierStageCount, requestedBytes) ||
        !AddSemanticScratchBytes<RtPathTraceRuntimeMaterialStagePod>(
            dto.runtimeStageCount, requestedBytes) ||
        !AddSemanticScratchBytes<float>(dto.registerCount, requestedBytes) ||
        !AddSemanticScratchBytes<idDrawVert>(dto.vertexCount, requestedBytes) ||
        !AddSemanticScratchBytes<triIndex_t>(dto.indexCount, requestedBytes) ||
        !AddSemanticScratchBytes<idJointMat>(dto.jointCount, requestedBytes))
    {
        return false;
    }

    bool built = false;
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    try
    {
#endif
        RtPathTracePrimarySemanticScratch candidate;
        candidate.sealedPrimaryViewToken = dto.sealedPrimaryViewToken;
        RtPathTraceCaptureOwnerSnapshot& snapshot = candidate.snapshot;
        snapshot.registryGeneration = dto.materialRegistryGeneration;
        snapshot.residentMaterialFactsGeneration =
            dto.residentMaterialFactsGeneration;
        snapshot.sourceDrawSurfCount = dto.sourceDrawSurfCount;
        snapshot.recordAllInstanceClasses =
            semanticConfig.recordAllInstanceClasses;
        snapshot.removeRoutedRigidDynamic =
            semanticConfig.removeRoutedRigidDynamic;
        snapshot.rigidRouteEmissiveCards =
            semanticConfig.rigidRouteEmissiveCards;
        snapshot.admissionMaxSurfaces =
            semanticConfig.admissionMaxSurfaces;
        snapshot.admissionMaxBytes = semanticConfig.admissionMaxBytes;
        snapshot.lateConsumeToken.configFingerprint =
            semanticConfig.configFingerprint;

        built =
            ReserveSemanticScratch(snapshot.surfaces, dto.surfaceCount, testSeam) &&
            ReserveSemanticScratch(snapshot.classifierStages,
                dto.classifierStageCount, testSeam) &&
            ReserveSemanticScratch(snapshot.runtimeStages,
                dto.runtimeStageCount, testSeam) &&
            ReserveSemanticScratch(snapshot.registers, dto.registerCount, testSeam) &&
            ReserveSemanticScratch(snapshot.vertices, dto.vertexCount, testSeam) &&
            ReserveSemanticScratch(snapshot.indexes, dto.indexCount, testSeam) &&
            ReserveSemanticScratch(snapshot.joints, dto.jointCount, testSeam) &&
            ReserveSemanticScratch(snapshot.modelTables,
                dto.modelTableCount, testSeam) &&
            ReserveSemanticScratch(snapshot.modelSurfaceTokens,
                dto.modelSurfaceTokenCount, testSeam) &&
            ReserveSemanticScratch(snapshot.variantBases,
                dto.variantBaseCount, testSeam) &&
            ReserveSemanticScratch(snapshot.registryMaterials,
                dto.registryMaterialCount, testSeam);
        if (built)
        {
            AssignSemanticScratch(snapshot.surfaces,
                dto.surfaces, dto.surfaceCount);
            AssignSemanticScratch(snapshot.classifierStages,
                dto.classifierStages, dto.classifierStageCount);
            AssignSemanticScratch(snapshot.runtimeStages,
                dto.runtimeStages, dto.runtimeStageCount);
            AssignSemanticScratch(snapshot.registers,
                dto.registers, dto.registerCount);
            AssignSemanticScratch(snapshot.vertices,
                dto.vertices, dto.vertexCount);
            const triIndex_t* indexes = static_cast<const triIndex_t*>(dto.indexes);
            AssignSemanticScratch(snapshot.indexes, indexes, dto.indexCount);
            AssignSemanticScratch(snapshot.joints, dto.joints, dto.jointCount);
            AssignSemanticScratch(snapshot.modelTables,
                dto.modelTables, dto.modelTableCount);
            AssignSemanticScratch(snapshot.modelSurfaceTokens,
                dto.modelSurfaceTokens, dto.modelSurfaceTokenCount);
            AssignSemanticScratch(snapshot.variantBases,
                dto.variantBases, dto.variantBaseCount);
            AssignSemanticScratch(snapshot.registryMaterials,
                dto.registryMaterials, dto.registryMaterialCount);
            snapshot.capacityCounts.surfaces = dto.surfaceCount;
            snapshot.capacityCounts.classifierStages = dto.classifierStageCount;
            snapshot.capacityCounts.runtimeStages = dto.runtimeStageCount;
            snapshot.capacityCounts.registers = dto.registerCount;
            snapshot.capacityCounts.vertices = dto.vertexCount;
            snapshot.capacityCounts.indexes = dto.indexCount;
            snapshot.capacityCounts.joints = dto.jointCount;
            snapshot.capacityCounts.modelTables = dto.modelTableCount;
            snapshot.capacityCounts.modelSurfaceTokens =
                dto.modelSurfaceTokenCount;
            snapshot.capacityCounts.variantBases = dto.variantBaseCount;
            snapshot.capacityCounts.registryMaterials = dto.registryMaterialCount;
            snapshot.complete = true;
            candidate.ownedBytes = SemanticScratchOwnedBytes(candidate);
            candidate.complete = candidate.ownedBytes <=
                RT_PT_CAPTURE_PRODUCT_SLOT_MAX_BYTES;
            built = candidate.complete;
        }
        if (built)
        {
            output = std::move(candidate);
        }
#if defined(__cpp_exceptions) || defined(_CPPUNWIND)
    }
    catch (const std::bad_alloc&)
    {
        built = false;
    }
    catch (const std::length_error&)
    {
        built = false;
    }
#endif
    return built;
}
