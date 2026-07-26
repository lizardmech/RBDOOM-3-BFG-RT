#include "PathTraceSkinnedConsumerAudit.h"

#include <unordered_map>

namespace {

std::uint64_t JoinUint64(std::uint32_t low, std::uint32_t high)
{
    return static_cast<std::uint64_t>(low) |
        (static_cast<std::uint64_t>(high) << 32);
}

bool CheckedRange(
    std::uint64_t offset,
    std::uint64_t count,
    std::uint64_t capacity)
{
    return offset <= capacity && count <= capacity - offset;
}

bool GpuRouteMatchesCpu(
    const PtSkinnedHitRouteRecord& cpu,
    const PathTraceSkinnedHitRouteGpuRecord& gpu,
    std::uint32_t routeCount,
    std::uint32_t triangleCount)
{
    return
        cpu.sourceIndexOffset == gpu.sourceIndexOffset &&
        cpu.outputVertexOffset == gpu.outputVertexOffset &&
        cpu.previousPositionOffset == gpu.previousPositionOffset &&
        cpu.triangleMetadataOffset == gpu.triangleMetadataOffset &&
        cpu.vertexCount == gpu.vertexCount &&
        cpu.indexCount == gpu.indexCount &&
        cpu.triangleCount == gpu.triangleCount &&
        cpu.flags == gpu.flags &&
        cpu.instanceHash ==
            JoinUint64(gpu.instanceHashLo, gpu.instanceHashHi) &&
        cpu.sourceChecksum ==
            JoinUint64(gpu.sourceChecksumLo, gpu.sourceChecksumHi) &&
        cpu.sourceGpuIndexGeneration ==
            JoinUint64(
                gpu.sourceGpuIndexGenerationLo,
                gpu.sourceGpuIndexGenerationHi) &&
        cpu.outputStorageGeneration ==
            JoinUint64(
                gpu.outputStorageGenerationLo,
                gpu.outputStorageGenerationHi) &&
        gpu.routeCount == routeCount &&
        gpu.triangleMetadataCount == triangleCount;
}

bool GpuTriangleMatchesCpu(
    const PtSkinnedHitRouteTriangle& cpu,
    const PathTraceSkinnedHitRouteGpuTriangle& gpu)
{
    return
        cpu.sourcePrimitiveIndex == gpu.sourcePrimitiveIndex &&
        cpu.legacyPrimitiveIndex == gpu.legacyPrimitiveIndex &&
        cpu.materialId == gpu.materialId &&
        cpu.materialIndex == gpu.materialIndex &&
        cpu.triangleClassAndFlags == gpu.triangleClassAndFlags &&
        cpu.canonicalPrimitiveHash ==
            JoinUint64(
                gpu.canonicalPrimitiveHashLo,
                gpu.canonicalPrimitiveHashHi) &&
        cpu.emissiveIdentityHash ==
            JoinUint64(
                gpu.emissiveIdentityHashLo,
                gpu.emissiveIdentityHashHi);
}

} // namespace

bool PtSkinnedConsumerAuditStats::Accepted() const
{
    return
        missingInput == 0 &&
        routeCountMismatch == 0 &&
        routeUploadMismatch == 0 &&
        tlasRouteMismatch == 0 &&
        triangleRangeMismatch == 0 &&
        triangleUploadMismatch == 0 &&
        legacyMetadataMismatch == 0 &&
        materialTableMismatch == 0 &&
        currentRangeMismatch == 0 &&
        previousRangeMismatch == 0 &&
        primitiveIdentityInvalid == 0 &&
        emissiveIdentityInvalid == 0 &&
        emissiveIdentityCollision == 0;
}

PtSkinnedConsumerAuditStats PtAuditSkinnedConsumerContract(
    const PtSkinnedConsumerAuditInput& input)
{
    PtSkinnedConsumerAuditStats stats;
    if (!input.cpuBuild ||
        !input.gpuUpload ||
        (input.requireTlasPlan && !input.tlasPlan) ||
        !input.legacyTriangleClasses ||
        !input.legacyTriangleMaterialIds ||
        !input.legacyTriangleMaterialIndexes ||
        !input.materialTableIds)
    {
        stats.missingInput = 1;
        return stats;
    }

    const PtSkinnedHitRouteBuild& cpu = *input.cpuBuild;
    const PtSkinnedHitRouteGpuUpload& gpu = *input.gpuUpload;
    stats.routes = cpu.records.size();
    stats.triangles = cpu.triangles.size();

    if (cpu.records.empty() ||
        gpu.records.size() != cpu.records.size())
    {
        ++stats.routeCountMismatch;
        return stats;
    }
    if (input.requireTlasPlan &&
        (input.tlasPlan->result !=
                PtSkinnedTlasRouteResult::Accepted ||
         input.tlasPlan->records.size() !=
                cpu.records.size()))
    {
        ++stats.routeCountMismatch;
        return stats;
    }
    if (gpu.triangles.size() != cpu.triangles.size())
    {
        ++stats.triangleRangeMismatch;
        return stats;
    }

    std::unordered_map<std::uint64_t, std::size_t> emissiveOwners;
    for (std::size_t routeIndex = 0;
         routeIndex < cpu.records.size();
         ++routeIndex)
    {
        const PtSkinnedHitRouteRecord& cpuRoute =
            cpu.records[routeIndex];
        const PathTraceSkinnedHitRouteGpuRecord& gpuRoute =
            gpu.records[routeIndex];

        if (!GpuRouteMatchesCpu(
                cpuRoute,
                gpuRoute,
                static_cast<std::uint32_t>(cpu.records.size()),
                static_cast<std::uint32_t>(cpu.triangles.size())))
        {
            ++stats.routeUploadMismatch;
        }

        bool foundTlasRoute = !input.requireTlasPlan;
        if (input.requireTlasPlan)
        {
            for (const PtSkinnedTlasRouteRecord& tlasRoute :
                 input.tlasPlan->records)
            {
                if (tlasRoute.instanceKey == cpuRoute.instanceKey &&
                    tlasRoute.shaderInstanceId ==
                        gpuRoute.shaderInstanceId &&
                    tlasRoute.hitGroupContribution ==
                        PT_PATH_TRACE_SBT_SKINNED_INSTANCE_CONTRIBUTION)
                {
                    foundTlasRoute = true;
                    break;
                }
            }
        }
        if (!foundTlasRoute)
        {
            ++stats.tlasRouteMismatch;
        }

        if (!CheckedRange(
                cpuRoute.outputVertexOffset,
                cpuRoute.vertexCount,
                input.currentOutputVertexCount))
        {
            ++stats.currentRangeMismatch;
        }
        if ((cpuRoute.flags & PT_SKINNED_HIT_ROUTE_HAS_PREVIOUS) != 0)
        {
            ++stats.motionReadyRoutes;
            if (cpuRoute.previousPositionOffset ==
                    PT_SKINNED_HIT_ROUTE_INVALID_INDEX ||
                !CheckedRange(
                    cpuRoute.previousPositionOffset,
                    cpuRoute.vertexCount,
                    input.previousPositionCount))
            {
                ++stats.previousRangeMismatch;
            }
        }
        else
        {
            ++stats.motionMissingRoutes;
            if (cpuRoute.previousPositionOffset !=
                PT_SKINNED_HIT_ROUTE_INVALID_INDEX)
            {
                ++stats.previousRangeMismatch;
            }
        }

        if (!CheckedRange(
                cpuRoute.triangleMetadataOffset,
                cpuRoute.triangleCount,
                cpu.triangles.size()))
        {
            ++stats.triangleRangeMismatch;
            continue;
        }

        for (std::uint32_t localPrimitive = 0;
             localPrimitive < cpuRoute.triangleCount;
             ++localPrimitive)
        {
            const std::size_t triangleIndex =
                static_cast<std::size_t>(
                    cpuRoute.triangleMetadataOffset) +
                localPrimitive;
            const PtSkinnedHitRouteTriangle& cpuTriangle =
                cpu.triangles[triangleIndex];
            const PathTraceSkinnedHitRouteGpuTriangle& gpuTriangle =
                gpu.triangles[triangleIndex];

            if (cpuTriangle.sourcePrimitiveIndex != localPrimitive)
            {
                ++stats.triangleRangeMismatch;
            }
            if (!GpuTriangleMatchesCpu(cpuTriangle, gpuTriangle))
            {
                ++stats.triangleUploadMismatch;
            }

            if (cpuTriangle.materialIndex >=
                    input.materialTableIds->size() ||
                (*input.materialTableIds)[cpuTriangle.materialIndex] !=
                    cpuTriangle.materialId)
            {
                ++stats.materialTableMismatch;
            }

            if (cpuTriangle.legacyPrimitiveIndex ==
                PT_SKINNED_HIT_ROUTE_INVALID_INDEX)
            {
                ++stats.sourceOnlyTriangles;
            }
            else
            {
                ++stats.mappedLegacyTriangles;
                const std::uint32_t legacyPrimitive =
                    cpuTriangle.legacyPrimitiveIndex;
                if (legacyPrimitive >=
                        input.legacyTriangleClasses->size() ||
                    legacyPrimitive >=
                        input.legacyTriangleMaterialIds->size() ||
                    legacyPrimitive >=
                        input.legacyTriangleMaterialIndexes->size() ||
                    (*input.legacyTriangleClasses)[legacyPrimitive] !=
                        cpuTriangle.triangleClassAndFlags ||
                    (*input.legacyTriangleMaterialIds)[legacyPrimitive] !=
                        cpuTriangle.materialId ||
                    (*input.legacyTriangleMaterialIndexes)[legacyPrimitive] !=
                        cpuTriangle.materialIndex)
                {
                    ++stats.legacyMetadataMismatch;
                }
            }

            if (cpuTriangle.canonicalPrimitiveHash == 0)
            {
                ++stats.primitiveIdentityInvalid;
            }
            if (cpuTriangle.emissiveIdentityHash == 0)
            {
                ++stats.emissiveIdentityInvalid;
            }
            else
            {
                const auto inserted = emissiveOwners.emplace(
                    cpuTriangle.emissiveIdentityHash,
                    triangleIndex);
                if (!inserted.second &&
                    inserted.first->second != triangleIndex)
                {
                    ++stats.emissiveIdentityCollision;
                }
            }
        }
    }
    return stats;
}
