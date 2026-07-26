#include "PathTraceSkinnedHitRoute.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <unordered_map>

namespace {

constexpr std::uint64_t kOutputVertexStrideBytes = 112;
constexpr std::uint64_t kIndexStrideBytes =
    sizeof(std::uint32_t);

bool CheckedAdd(
    std::uint64_t a,
    std::uint64_t b,
    std::uint64_t& result)
{
    if (a > std::numeric_limits<std::uint64_t>::max() - b)
    {
        result = 0;
        return false;
    }
    result = a + b;
    return true;
}

bool CheckedMul(
    std::uint64_t a,
    std::uint64_t b,
    std::uint64_t& result)
{
    if (a != 0 &&
        b > std::numeric_limits<std::uint64_t>::max() / a)
    {
        result = 0;
        return false;
    }
    result = a * b;
    return true;
}

bool FitsU32(std::uint64_t value)
{
    return value <=
        static_cast<std::uint64_t>(
            std::numeric_limits<std::uint32_t>::max());
}

std::uint64_t HashIdentityValue(
    std::uint64_t hash,
    std::uint64_t value)
{
    hash ^= value;
    hash *= 1099511628211ull;
    return hash;
}

std::uint64_t BuildEmissiveIdentity(
    std::uint64_t instanceHash,
    std::uint32_t sourcePrimitiveIndex,
    std::uint32_t materialId,
    std::uint32_t materialIndex,
    std::uint32_t triangleClassAndFlags)
{
    std::uint64_t hash = 1469598103934665603ull;
    hash = HashIdentityValue(hash, materialId);
    hash = HashIdentityValue(
        hash,
        instanceHash & 0xffffffffull);
    hash = HashIdentityValue(hash, instanceHash >> 32);
    hash = HashIdentityValue(hash, sourcePrimitiveIndex);
    hash = HashIdentityValue(hash, materialIndex);
    hash = HashIdentityValue(hash, triangleClassAndFlags);
    return hash;
}

bool ExactInstanceAlreadyAccepted(
    const std::vector<PtSkinnedHitRouteRecord>& records,
    const PtCanonicalInstanceKey& key)
{
    for (const PtSkinnedHitRouteRecord& record : records)
    {
        if (record.instanceKey == key)
        {
            return true;
        }
    }
    return false;
}

bool InstanceHashCollides(
    const std::vector<PtSkinnedHitRouteRecord>& records,
    std::uint64_t instanceHash,
    const PtCanonicalInstanceKey& key)
{
    for (const PtSkinnedHitRouteRecord& record : records)
    {
        if (record.instanceHash == instanceHash &&
            record.instanceKey != key)
        {
            return true;
        }
    }
    return false;
}

struct EmissiveIdentityOwner
{
    std::uint64_t instanceHash = 0;
    PtSkinnedHitRouteTriangle triangle;
};

PtSkinnedHitRouteResult ValidateCandidate(
    const PtSkinnedHitRouteCandidate& candidate,
    const PtSkinnedHitRouteLegacyView& legacy,
    std::uint64_t& sourceIndexBytes,
    std::uint64_t& sourceIndexEnd,
    std::uint64_t& outputVertexBytes,
    std::uint64_t& outputEnd,
    std::uint64_t& previousEnd,
    std::uint64_t& legacyVertexEnd,
    std::uint64_t& legacyIndexEnd,
    std::uint64_t& legacyTriangleEnd)
{
    std::uint64_t expectedLegacyIndexCount = 0;
    if (!candidate.dispatchReady)
    {
        return PtSkinnedHitRouteResult::DispatchNotReady;
    }
    if (!PtCanonicalInstanceKeyIsValid(
            candidate.instanceKey))
    {
        return PtSkinnedHitRouteResult::InvalidInstanceKey;
    }
    if (!PtCanonicalMeshKeyIsValid(candidate.meshKey) ||
        candidate.meshKey.sourceDomain !=
            PtCanonicalMeshSourceDomain::SkinnedBindSource ||
        candidate.meshKey.deformationClass !=
            PtCanonicalDeformationClass::Skinned)
    {
        return PtSkinnedHitRouteResult::InvalidMeshKey;
    }
    if (candidate.sourceChecksum == 0 ||
        candidate.sourceGpuIndexGeneration == 0 ||
        candidate.sourceIndexes == nullptr ||
        candidate.sourceIndexCount == 0 ||
        candidate.sourceIndexCount !=
            candidate.meshKey.indexCount ||
        candidate.sourceIndexCount % 3 != 0 ||
        candidate.sourceIndexOffsetBytes %
                kIndexStrideBytes !=
            0 ||
        !CheckedMul(
            candidate.sourceIndexCount,
            kIndexStrideBytes,
            sourceIndexBytes) ||
        !CheckedAdd(
            candidate.sourceIndexOffsetBytes,
            sourceIndexBytes,
            sourceIndexEnd) ||
        sourceIndexEnd >
            candidate.sourceIndexCapacityBytes ||
        !FitsU32(
            candidate.sourceIndexOffsetBytes /
                kIndexStrideBytes))
    {
        return PtSkinnedHitRouteResult::InvalidSourceContract;
    }
    if (candidate.outputStorageGeneration == 0 ||
        candidate.outputVertexCount == 0 ||
        candidate.outputVertexCount !=
            candidate.meshKey.vertexCount ||
        candidate.outputVertexOffsetBytes %
                kOutputVertexStrideBytes !=
            0 ||
        !CheckedMul(
            candidate.outputVertexCount,
            kOutputVertexStrideBytes,
            outputVertexBytes) ||
        !CheckedAdd(
            candidate.outputVertexOffsetBytes,
            outputVertexBytes,
            outputEnd) ||
        outputEnd > candidate.outputCapacityBytes ||
        !FitsU32(
            candidate.outputVertexOffsetBytes /
                kOutputVertexStrideBytes) ||
        !FitsU32(candidate.outputVertexCount) ||
        !FitsU32(candidate.sourceIndexCount))
    {
        return PtSkinnedHitRouteResult::InvalidOutputContract;
    }
    previousEnd = 0;
    if (candidate.previousValid &&
        (!CheckedAdd(
             candidate.previousPositionOffset,
             candidate.outputVertexCount,
             previousEnd) ||
         previousEnd > candidate.previousPositionCount ||
         !FitsU32(candidate.previousPositionOffset)))
    {
        return PtSkinnedHitRouteResult::InvalidPreviousContract;
    }
    if (candidate.legacyCapturePresent &&
        (candidate.legacyVertexCount == 0 ||
         candidate.legacyVertexCount !=
             candidate.outputVertexCount ||
         candidate.legacyIndexCount == 0 ||
         candidate.legacyTriangleCount == 0 ||
         !CheckedMul(
             candidate.legacyTriangleCount,
             3,
             expectedLegacyIndexCount) ||
         candidate.legacyIndexCount !=
             expectedLegacyIndexCount ||
         legacy.indexes == nullptr ||
         legacy.triangleClasses == nullptr ||
         legacy.triangleMaterialIds == nullptr ||
         legacy.triangleMaterialIndexes == nullptr ||
         !CheckedAdd(
             candidate.legacyVertexOffset,
             candidate.legacyVertexCount,
             legacyVertexEnd) ||
         !CheckedAdd(
             candidate.legacyIndexOffset,
             candidate.legacyIndexCount,
             legacyIndexEnd) ||
         !CheckedAdd(
             candidate.legacyTriangleOffset,
             candidate.legacyTriangleCount,
             legacyTriangleEnd) ||
         legacyIndexEnd > legacy.indexCount ||
         legacyTriangleEnd > legacy.triangleClassCount ||
         legacyTriangleEnd >
             legacy.triangleMaterialIdCount ||
         legacyTriangleEnd >
             legacy.triangleMaterialIndexCount))
    {
        return PtSkinnedHitRouteResult::InvalidLegacyRange;
    }
    if (!candidate.legacyCapturePresent)
    {
        legacyVertexEnd = 0;
        legacyIndexEnd = 0;
        legacyTriangleEnd = 0;
    }
    return PtSkinnedHitRouteResult::Accepted;
}

}

PtSkinnedHitRouteBuild PtBuildSkinnedHitRoutes(
    const std::vector<PtSkinnedHitRouteCandidate>& candidates,
    const PtSkinnedHitRouteLegacyView& legacy,
    std::uint64_t firstShaderInstanceId)
{
    PtSkinnedHitRouteBuild build;
    build.records.reserve(candidates.size());
    build.results.reserve(candidates.size());
    build.stats.candidates = candidates.size();
    std::unordered_map<
        std::uint64_t,
        EmissiveIdentityOwner>
        emissiveIdentities;

    for (const PtSkinnedHitRouteCandidate& candidate :
        candidates)
    {
        std::uint64_t sourceIndexBytes = 0;
        std::uint64_t sourceIndexEnd = 0;
        std::uint64_t outputVertexBytes = 0;
        std::uint64_t outputEnd = 0;
        std::uint64_t previousEnd = 0;
        std::uint64_t legacyVertexEnd = 0;
        std::uint64_t legacyIndexEnd = 0;
        std::uint64_t legacyTriangleEnd = 0;
        PtSkinnedHitRouteResult result =
            ValidateCandidate(
                candidate,
                legacy,
                sourceIndexBytes,
                sourceIndexEnd,
                outputVertexBytes,
                outputEnd,
                previousEnd,
                legacyVertexEnd,
                legacyIndexEnd,
                legacyTriangleEnd);
        const std::uint64_t instanceHash =
            PtHashCanonicalInstanceKey(
                candidate.instanceKey);
        if (result == PtSkinnedHitRouteResult::Accepted &&
            ExactInstanceAlreadyAccepted(
                build.records,
                candidate.instanceKey))
        {
            result =
                PtSkinnedHitRouteResult::DuplicateInstanceKey;
            ++build.stats.duplicateInstances;
        }
        if (result == PtSkinnedHitRouteResult::Accepted &&
            InstanceHashCollides(
                build.records,
                instanceHash,
                candidate.instanceKey))
        {
            result =
                PtSkinnedHitRouteResult::CanonicalHashCollision;
            ++build.stats.canonicalHashCollisions;
        }

        const std::uint64_t shaderInstanceId64 =
            static_cast<std::uint64_t>(
                firstShaderInstanceId) +
            build.records.size();
        if (result == PtSkinnedHitRouteResult::Accepted &&
            shaderInstanceId64 >
                PT_SKINNED_HIT_ROUTE_MAX_SHADER_INSTANCE_ID)
        {
            result =
                PtSkinnedHitRouteResult::ShaderInstanceIdOverflow;
        }
        if (result != PtSkinnedHitRouteResult::Accepted)
        {
            build.results.push_back(result);
            ++build.stats.rejected;
            continue;
        }

        const std::size_t triangleStart =
            build.triangles.size();
        const std::uint64_t sourceTriangleCount =
            candidate.sourceIndexCount / 3;
        std::uint64_t legacyLocalTriangle = 0;
        std::uint64_t mappedLegacyTriangles = 0;
        std::uint64_t sourceOnlyTriangles = 0;
        std::vector<PtSkinnedHitRouteTriangle>
            candidateTriangles;
        candidateTriangles.reserve(
            static_cast<std::size_t>(
                sourceTriangleCount));
        bool invalidSourceIndex = false;
        bool invalidLegacyIndex = false;
        for (std::uint64_t sourceTriangle = 0;
            sourceTriangle < sourceTriangleCount;
            ++sourceTriangle)
        {
            const std::uint64_t sourceIndex =
                sourceTriangle * 3;
            const std::uint32_t source0 =
                candidate.sourceIndexes[sourceIndex + 0];
            const std::uint32_t source1 =
                candidate.sourceIndexes[sourceIndex + 1];
            const std::uint32_t source2 =
                candidate.sourceIndexes[sourceIndex + 2];
            if (source0 >= candidate.outputVertexCount ||
                source1 >= candidate.outputVertexCount ||
                source2 >= candidate.outputVertexCount)
            {
                invalidSourceIndex = true;
                break;
            }

            bool mapped = false;
            std::uint64_t legacyPrimitive = 0;
            if (candidate.legacyCapturePresent &&
                legacyLocalTriangle <
                candidate.legacyTriangleCount)
            {
                const std::uint64_t legacyIndex =
                    candidate.legacyIndexOffset +
                    legacyLocalTriangle * 3;
                const std::uint32_t legacyGlobal0 =
                    legacy.indexes[legacyIndex + 0];
                const std::uint32_t legacyGlobal1 =
                    legacy.indexes[legacyIndex + 1];
                const std::uint32_t legacyGlobal2 =
                    legacy.indexes[legacyIndex + 2];
                if (legacyGlobal0 <
                        candidate.legacyVertexOffset ||
                    legacyGlobal0 >= legacyVertexEnd ||
                    legacyGlobal1 <
                        candidate.legacyVertexOffset ||
                    legacyGlobal1 >= legacyVertexEnd ||
                    legacyGlobal2 <
                        candidate.legacyVertexOffset ||
                    legacyGlobal2 >= legacyVertexEnd)
                {
                    invalidLegacyIndex = true;
                    break;
                }
                mapped =
                    legacyGlobal0 -
                            candidate.legacyVertexOffset ==
                        source0 &&
                    legacyGlobal1 -
                            candidate.legacyVertexOffset ==
                        source1 &&
                    legacyGlobal2 -
                            candidate.legacyVertexOffset ==
                        source2;
                legacyPrimitive =
                    candidate.legacyTriangleOffset +
                    legacyLocalTriangle;
            }

            PtSkinnedHitRouteTriangle triangle;
            triangle.sourcePrimitiveIndex =
                static_cast<std::uint32_t>(
                    sourceTriangle);
            triangle.materialId =
                candidate.fallbackMaterialId;
            triangle.materialIndex =
                candidate.fallbackMaterialIndex;
            triangle.triangleClassAndFlags =
                candidate.fallbackTriangleClassAndFlags;
            if (mapped)
            {
                triangle.legacyPrimitiveIndex =
                    static_cast<std::uint32_t>(
                        legacyPrimitive);
                triangle.materialId =
                    legacy.triangleMaterialIds[
                        legacyPrimitive];
                triangle.materialIndex =
                    legacy.triangleMaterialIndexes[
                        legacyPrimitive];
                triangle.triangleClassAndFlags =
                    legacy.triangleClasses[
                        legacyPrimitive];
                ++legacyLocalTriangle;
                ++mappedLegacyTriangles;
            }
            else
            {
                ++sourceOnlyTriangles;
            }

            PtCanonicalPrimitiveKey primitiveKey;
            primitiveKey.mesh = candidate.meshKey;
            primitiveKey.localPrimitiveIndex =
                triangle.sourcePrimitiveIndex;
            triangle.canonicalPrimitiveHash =
                PtHashCanonicalPrimitiveKey(primitiveKey);
            triangle.emissiveIdentityHash =
                BuildEmissiveIdentity(
                    instanceHash,
                    triangle.sourcePrimitiveIndex,
                    triangle.materialId,
                    triangle.materialIndex,
                    triangle.triangleClassAndFlags);
            candidateTriangles.push_back(triangle);
        }

        if (invalidSourceIndex ||
            invalidLegacyIndex ||
            legacyLocalTriangle !=
                candidate.legacyTriangleCount)
        {
            build.triangles.resize(triangleStart);
            result = invalidSourceIndex
                ? PtSkinnedHitRouteResult::InvalidSourceIndex
                : invalidLegacyIndex
                    ? PtSkinnedHitRouteResult::InvalidLegacyIndex
                    : PtSkinnedHitRouteResult::
                        LegacyTopologyMismatch;
            build.results.push_back(result);
            ++build.stats.rejected;
            continue;
        }
        if (sourceOnlyTriangles > 0 &&
            (candidate.fallbackMaterialId ==
                    PT_SKINNED_HIT_ROUTE_INVALID_INDEX ||
             candidate.fallbackMaterialIndex ==
                    PT_SKINNED_HIT_ROUTE_INVALID_INDEX ||
             candidate.fallbackTriangleClassAndFlags == 0))
        {
            result =
                PtSkinnedHitRouteResult::
                    InvalidFallbackMetadata;
            build.results.push_back(result);
            ++build.stats.rejected;
            continue;
        }
        for (const PtSkinnedHitRouteTriangle& triangle :
            candidateTriangles)
        {
            const auto existingIdentity =
                emissiveIdentities.find(
                    triangle.emissiveIdentityHash);
            if (existingIdentity !=
                    emissiveIdentities.end() &&
                (existingIdentity->second.instanceHash !=
                        instanceHash ||
                 existingIdentity->second.triangle.
                        sourcePrimitiveIndex !=
                    triangle.sourcePrimitiveIndex ||
                 existingIdentity->second.triangle.materialId !=
                    triangle.materialId ||
                 existingIdentity->second.triangle.materialIndex !=
                    triangle.materialIndex ||
                 existingIdentity->second.triangle.
                        triangleClassAndFlags !=
                    triangle.triangleClassAndFlags))
            {
                ++build.stats.emissiveIdentityCollisions;
            }
            else
            {
                EmissiveIdentityOwner owner;
                owner.instanceHash = instanceHash;
                owner.triangle = triangle;
                emissiveIdentities.emplace(
                    triangle.emissiveIdentityHash,
                    owner);
            }
            build.triangles.push_back(triangle);
        }

        PtSkinnedHitRouteRecord record;
        record.instanceKey = candidate.instanceKey;
        record.meshKey = candidate.meshKey;
        record.instanceHash = instanceHash;
        record.sourceChecksum = candidate.sourceChecksum;
        record.sourceGpuIndexGeneration =
            candidate.sourceGpuIndexGeneration;
        record.outputStorageGeneration =
            candidate.outputStorageGeneration;
        record.shaderInstanceId =
            static_cast<std::uint32_t>(
                shaderInstanceId64);
        record.sourceIndexOffset =
            static_cast<std::uint32_t>(
                candidate.sourceIndexOffsetBytes /
                    kIndexStrideBytes);
        record.outputVertexOffset =
            static_cast<std::uint32_t>(
                candidate.outputVertexOffsetBytes /
                    kOutputVertexStrideBytes);
        record.triangleMetadataOffset =
            static_cast<std::uint32_t>(
                triangleStart);
        record.vertexCount =
            static_cast<std::uint32_t>(
                candidate.outputVertexCount);
        record.indexCount =
            static_cast<std::uint32_t>(
                candidate.sourceIndexCount);
        record.triangleCount =
            static_cast<std::uint32_t>(
                sourceTriangleCount);
        if (candidate.previousValid)
        {
            record.previousPositionOffset =
                static_cast<std::uint32_t>(
                    candidate.previousPositionOffset);
            record.flags |=
                PT_SKINNED_HIT_ROUTE_HAS_PREVIOUS;
            ++build.stats.motionReady;
        }
        else
        {
            ++build.stats.motionMissing;
        }
        if (sourceTriangleCount >
            candidate.legacyTriangleCount)
        {
            record.flags |=
                PT_SKINNED_HIT_ROUTE_HAS_SOURCE_ONLY_PRIMITIVES;
        }

        build.records.push_back(record);
        build.results.push_back(
            PtSkinnedHitRouteResult::Accepted);
        ++build.stats.accepted;
        build.stats.sourceTriangles +=
            sourceTriangleCount;
        build.stats.legacyTriangles +=
            candidate.legacyTriangleCount;
        build.stats.mappedLegacyTriangles +=
            mappedLegacyTriangles;
        build.stats.sourceOnlyTriangles +=
            sourceOnlyTriangles;
    }

    return build;
}

const char* PtSkinnedHitRouteResultName(
    PtSkinnedHitRouteResult result)
{
    switch (result)
    {
        case PtSkinnedHitRouteResult::Accepted:
            return "accepted";
        case PtSkinnedHitRouteResult::InvalidInstanceKey:
            return "invalid-instance-key";
        case PtSkinnedHitRouteResult::InvalidMeshKey:
            return "invalid-mesh-key";
        case PtSkinnedHitRouteResult::InvalidSourceContract:
            return "invalid-source-contract";
        case PtSkinnedHitRouteResult::InvalidOutputContract:
            return "invalid-output-contract";
        case PtSkinnedHitRouteResult::InvalidPreviousContract:
            return "invalid-previous-contract";
        case PtSkinnedHitRouteResult::InvalidLegacyRange:
            return "invalid-legacy-range";
        case PtSkinnedHitRouteResult::InvalidSourceIndex:
            return "invalid-source-index";
        case PtSkinnedHitRouteResult::InvalidLegacyIndex:
            return "invalid-legacy-index";
        case PtSkinnedHitRouteResult::LegacyTopologyMismatch:
            return "legacy-topology-mismatch";
        case PtSkinnedHitRouteResult::InvalidFallbackMetadata:
            return "invalid-fallback-metadata";
        case PtSkinnedHitRouteResult::DuplicateInstanceKey:
            return "duplicate-instance-key";
        case PtSkinnedHitRouteResult::CanonicalHashCollision:
            return "canonical-hash-collision";
        case PtSkinnedHitRouteResult::ShaderInstanceIdOverflow:
            return "shader-instance-id-overflow";
        case PtSkinnedHitRouteResult::DispatchNotReady:
            return "dispatch-not-ready";
        default:
            return "unknown";
    }
}

namespace {

std::uint64_t HashGpuUploadBytes(
    std::uint64_t hash,
    const void* bytes,
    std::size_t byteCount)
{
    const std::uint8_t* input =
        static_cast<const std::uint8_t*>(bytes);
    for (std::size_t index = 0; index < byteCount; ++index)
    {
        hash ^= input[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

} // namespace

PtSkinnedHitRouteGpuUpload PtBuildSkinnedHitRouteGpuUpload(
    const PtSkinnedHitRouteBuild& build,
    std::uint32_t firstShaderInstanceId)
{
    PtSkinnedHitRouteGpuUpload upload;
    const std::uint32_t routeCount =
        static_cast<std::uint32_t>(build.records.size());
    const std::uint32_t triangleCount =
        static_cast<std::uint32_t>(build.triangles.size());
    const bool routeRangeValid =
        routeCount == 0 ||
        static_cast<std::uint64_t>(firstShaderInstanceId) +
                routeCount - 1u <=
            PT_SKINNED_HIT_ROUTE_MAX_SHADER_INSTANCE_ID;
    upload.records.reserve(std::max<std::size_t>(
        build.records.size(), 1));
    upload.triangles.reserve(std::max<std::size_t>(
        build.triangles.size(), 1));

    for (std::size_t routeIndex = 0;
         routeRangeValid &&
         routeIndex < build.records.size();
         ++routeIndex)
    {
        const PtSkinnedHitRouteRecord& source =
            build.records[routeIndex];
        PathTraceSkinnedHitRouteGpuRecord record;
        // The accepted build is a one-frame shadow used by the next upload.
        // Rigid-route residency can change its instance count in between, so
        // preserving the shadow's IDs can overlap the current rigid range.
        // Rebase the contiguous skinned range against this frame's rigid end.
        record.shaderInstanceId =
            firstShaderInstanceId +
            static_cast<std::uint32_t>(routeIndex);
        record.sourceIndexOffset = source.sourceIndexOffset;
        record.outputVertexOffset = source.outputVertexOffset;
        record.previousPositionOffset =
            source.previousPositionOffset;
        record.triangleMetadataOffset =
            source.triangleMetadataOffset;
        record.vertexCount = source.vertexCount;
        record.indexCount = source.indexCount;
        record.triangleCount = source.triangleCount;
        record.flags = source.flags;
        record.instanceHashLo =
            static_cast<std::uint32_t>(source.instanceHash);
        record.instanceHashHi =
            static_cast<std::uint32_t>(source.instanceHash >> 32);
        record.sourceChecksumLo =
            static_cast<std::uint32_t>(source.sourceChecksum);
        record.sourceChecksumHi =
            static_cast<std::uint32_t>(source.sourceChecksum >> 32);
        record.sourceGpuIndexGenerationLo =
            static_cast<std::uint32_t>(
                source.sourceGpuIndexGeneration);
        record.sourceGpuIndexGenerationHi =
            static_cast<std::uint32_t>(
                source.sourceGpuIndexGeneration >> 32);
        record.outputStorageGenerationLo =
            static_cast<std::uint32_t>(
                source.outputStorageGeneration);
        record.outputStorageGenerationHi =
            static_cast<std::uint32_t>(
                source.outputStorageGeneration >> 32);
        record.routeCount = routeCount;
        record.triangleMetadataCount = triangleCount;
        upload.records.push_back(record);
    }

    for (std::size_t triangleIndex = 0;
         routeRangeValid &&
         triangleIndex < build.triangles.size();
         ++triangleIndex)
    {
        const PtSkinnedHitRouteTriangle& source =
            build.triangles[triangleIndex];
        PathTraceSkinnedHitRouteGpuTriangle triangle;
        triangle.sourcePrimitiveIndex =
            source.sourcePrimitiveIndex;
        triangle.legacyPrimitiveIndex =
            source.legacyPrimitiveIndex;
        triangle.materialId = source.materialId;
        triangle.materialIndex = source.materialIndex;
        triangle.triangleClassAndFlags =
            source.triangleClassAndFlags;
        triangle.canonicalPrimitiveHashLo =
            static_cast<std::uint32_t>(
                source.canonicalPrimitiveHash);
        triangle.canonicalPrimitiveHashHi =
            static_cast<std::uint32_t>(
                source.canonicalPrimitiveHash >> 32);
        triangle.emissiveIdentityHashLo =
            static_cast<std::uint32_t>(
                source.emissiveIdentityHash);
        triangle.emissiveIdentityHashHi =
            static_cast<std::uint32_t>(
                source.emissiveIdentityHash >> 32);
        upload.triangles.push_back(triangle);
    }

    if (upload.records.empty())
    {
        PathTraceSkinnedHitRouteGpuRecord sentinel;
        sentinel.shaderInstanceId =
            firstShaderInstanceId;
        upload.records.push_back(sentinel);
    }
    if (upload.triangles.empty())
    {
        upload.triangles.emplace_back();
    }

    upload.signature = 1469598103934665603ull;
    upload.signature = HashGpuUploadBytes(
        upload.signature,
        upload.records.data(),
        upload.records.size() * sizeof(upload.records[0]));
    upload.signature = HashGpuUploadBytes(
        upload.signature,
        upload.triangles.data(),
        upload.triangles.size() * sizeof(upload.triangles[0]));
    return upload;
}

PtPathTraceSbtSelection PtPlanPathTraceSbtSelection(
    const PtPathTraceSbtSelectionInput& input)
{
    PtPathTraceSbtSelection selection;
    selection.instanceContribution =
        input.geometryClass == PtPathTraceSbtGeometryClass::Skinned
        ? PT_PATH_TRACE_SBT_SKINNED_INSTANCE_CONTRIBUTION
        : PT_PATH_TRACE_SBT_LEGACY_INSTANCE_CONTRIBUTION;

    if (input.rayContribution >=
        PT_PATH_TRACE_SBT_RAY_TYPE_COUNT)
    {
        selection.result =
            PtPathTraceSbtSelectionResult::
                UnsupportedRayContribution;
        return selection;
    }
    if (input.geometryContribution != 0u)
    {
        selection.result =
            PtPathTraceSbtSelectionResult::
                UnsupportedGeometryContribution;
        return selection;
    }
    if (input.geometryMultiplier != 1u)
    {
        selection.result =
            PtPathTraceSbtSelectionResult::
                InvalidGeometryMultiplier;
        return selection;
    }

    selection.recordIndex =
        selection.instanceContribution +
        input.geometryContribution *
            input.geometryMultiplier +
        input.rayContribution;
    if (selection.recordIndex >=
        input.shaderTableRecordCount)
    {
        selection.result =
            PtPathTraceSbtSelectionResult::
                MissingShaderTableRecord;
        return selection;
    }

    selection.result =
        PtPathTraceSbtSelectionResult::Accepted;
    return selection;
}

namespace {

std::uint64_t JoinRouteUint64(
    std::uint32_t low,
    std::uint32_t high)
{
    return static_cast<std::uint64_t>(low) |
        (static_cast<std::uint64_t>(high) << 32);
}

bool SkinnedTlasUploadMatchesCpu(
    const PtSkinnedHitRouteRecord& cpu,
    const PathTraceSkinnedHitRouteGpuRecord& gpu,
    std::uint32_t uploadedRouteCount,
    std::uint32_t uploadedTriangleCount)
{
    return
        cpu.sourceIndexOffset == gpu.sourceIndexOffset &&
        cpu.outputVertexOffset == gpu.outputVertexOffset &&
        cpu.previousPositionOffset ==
            gpu.previousPositionOffset &&
        cpu.triangleMetadataOffset ==
            gpu.triangleMetadataOffset &&
        cpu.vertexCount == gpu.vertexCount &&
        cpu.indexCount == gpu.indexCount &&
        cpu.triangleCount == gpu.triangleCount &&
        cpu.flags == gpu.flags &&
        cpu.instanceHash ==
            JoinRouteUint64(
                gpu.instanceHashLo,
                gpu.instanceHashHi) &&
        cpu.sourceChecksum ==
            JoinRouteUint64(
                gpu.sourceChecksumLo,
                gpu.sourceChecksumHi) &&
        cpu.sourceGpuIndexGeneration ==
            JoinRouteUint64(
                gpu.sourceGpuIndexGenerationLo,
                gpu.sourceGpuIndexGenerationHi) &&
        cpu.outputStorageGeneration ==
            JoinRouteUint64(
                gpu.outputStorageGenerationLo,
                gpu.outputStorageGenerationHi) &&
        gpu.routeCount == uploadedRouteCount &&
        gpu.triangleMetadataCount ==
            uploadedTriangleCount;
}

void IncrementSkinnedTlasFailure(
    PtSkinnedTlasRouteStats& stats,
    PtSkinnedTlasRouteResult result)
{
    switch (result)
    {
        case PtSkinnedTlasRouteResult::
            UploadRouteCountMismatch:
            ++stats.uploadRouteCountMismatch;
            break;
        case PtSkinnedTlasRouteResult::MissingCpuRoute:
            ++stats.missingCpuRoute;
            break;
        case PtSkinnedTlasRouteResult::MissingGpuRoute:
            ++stats.missingGpuRoute;
            break;
        case PtSkinnedTlasRouteResult::
            UploadContractMismatch:
            ++stats.uploadContractMismatch;
            break;
        case PtSkinnedTlasRouteResult::MissingResource:
            ++stats.missingResource;
            break;
        case PtSkinnedTlasRouteResult::
            ResourceContractMismatch:
            ++stats.resourceContractMismatch;
            break;
        case PtSkinnedTlasRouteResult::MissingBlas:
            ++stats.missingBlas;
            break;
        case PtSkinnedTlasRouteResult::
            InvalidSbtSelection:
            ++stats.invalidSbtSelection;
            break;
        case PtSkinnedTlasRouteResult::
            TlasCapacityExceeded:
            ++stats.tlasCapacityExceeded;
            break;
        default:
            break;
    }
}

}

PtSkinnedTlasRoutePlan PtPlanSkinnedTlasRoutes(
    const PtSkinnedTlasRoutePlanInput& input)
{
    PtSkinnedTlasRoutePlan plan;
    plan.stats.candidates =
        static_cast<std::uint32_t>(
            input.candidates.size());
    plan.candidateResults.assign(
        input.candidates.size(),
        PtSkinnedTlasRouteResult::Accepted);

    if (!input.gate)
    {
        plan.result =
            PtSkinnedTlasRouteResult::GateDisabled;
        return plan;
    }

    if (input.uploadedRouteCount !=
        input.candidates.size())
    {
        plan.result =
            PtSkinnedTlasRouteResult::
                UploadRouteCountMismatch;
        plan.stats.rejected = plan.stats.candidates;
        IncrementSkinnedTlasFailure(
            plan.stats,
            plan.result);
        return plan;
    }

    const std::uint64_t requestedInstanceCount =
        static_cast<std::uint64_t>(
            input.baseInstanceCount) +
        input.existingExtraInstanceCount +
        input.candidates.size();
    if (requestedInstanceCount >
        input.maxInstanceCount)
    {
        plan.result =
            PtSkinnedTlasRouteResult::
                TlasCapacityExceeded;
        plan.stats.rejected = plan.stats.candidates;
        IncrementSkinnedTlasFailure(
            plan.stats,
            plan.result);
        return plan;
    }

    PtPathTraceSbtSelectionInput sbtInput;
    sbtInput.geometryClass =
        PtPathTraceSbtGeometryClass::Skinned;
    sbtInput.geometryContribution = 0u;
    sbtInput.geometryMultiplier = 1u;
    sbtInput.shaderTableRecordCount =
        input.shaderTableRecordCount;
    sbtInput.rayContribution =
        PT_PATH_TRACE_SBT_PRIMARY_RAY_CONTRIBUTION;
    const bool primarySbtValid =
        PtPlanPathTraceSbtSelection(sbtInput).result ==
        PtPathTraceSbtSelectionResult::Accepted;
    sbtInput.rayContribution =
        PT_PATH_TRACE_SBT_SHADOW_RAY_CONTRIBUTION;
    const bool shadowSbtValid =
        PtPlanPathTraceSbtSelection(sbtInput).result ==
        PtPathTraceSbtSelectionResult::Accepted;
    if (!primarySbtValid || !shadowSbtValid)
    {
        plan.result =
            PtSkinnedTlasRouteResult::
                InvalidSbtSelection;
        plan.stats.rejected = plan.stats.candidates;
        IncrementSkinnedTlasFailure(
            plan.stats,
            plan.result);
        return plan;
    }

    std::uint64_t uploadedTriangleCount64 = 0;
    for (const PtSkinnedTlasRouteCandidate& candidate :
        input.candidates)
    {
        if (candidate.cpuRoute)
        {
            uploadedTriangleCount64 +=
                candidate.cpuRoute->triangleCount;
        }
    }
    if (uploadedTriangleCount64 > UINT32_MAX)
    {
        plan.result =
            PtSkinnedTlasRouteResult::
                UploadContractMismatch;
        plan.stats.rejected = plan.stats.candidates;
        IncrementSkinnedTlasFailure(
            plan.stats,
            plan.result);
        return plan;
    }
    const std::uint32_t uploadedTriangleCount =
        static_cast<std::uint32_t>(
            uploadedTriangleCount64);

    PtSkinnedTlasRouteResult firstFailure =
        PtSkinnedTlasRouteResult::Accepted;
    std::uint32_t previousShaderInstanceId = 0;
    bool havePreviousShaderInstanceId = false;
    plan.records.reserve(input.candidates.size());
    for (std::size_t candidateIndex = 0;
         candidateIndex < input.candidates.size();
         ++candidateIndex)
    {
        const PtSkinnedTlasRouteCandidate& candidate =
            input.candidates[candidateIndex];
        PtSkinnedTlasRouteResult result =
            PtSkinnedTlasRouteResult::Accepted;
        if (!candidate.cpuRoute)
        {
            result =
                PtSkinnedTlasRouteResult::MissingCpuRoute;
        }
        else if (!candidate.gpuRoute)
        {
            result =
                PtSkinnedTlasRouteResult::MissingGpuRoute;
        }
        else if (
            candidate.gpuRoute->shaderInstanceId >
                PT_SKINNED_HIT_ROUTE_MAX_SHADER_INSTANCE_ID ||
            (havePreviousShaderInstanceId &&
                candidate.gpuRoute->shaderInstanceId !=
                    previousShaderInstanceId + 1u) ||
            !SkinnedTlasUploadMatchesCpu(
                *candidate.cpuRoute,
                *candidate.gpuRoute,
                input.uploadedRouteCount,
                uploadedTriangleCount))
        {
            result =
                PtSkinnedTlasRouteResult::
                    UploadContractMismatch;
        }
        else if (!candidate.resourceFound)
        {
            result =
                PtSkinnedTlasRouteResult::MissingResource;
        }
        else if (!candidate.resourceContractExact)
        {
            result =
                PtSkinnedTlasRouteResult::
                    ResourceContractMismatch;
        }
        else if (!candidate.blasReady)
        {
            result =
                PtSkinnedTlasRouteResult::MissingBlas;
        }

        plan.candidateResults[candidateIndex] = result;
        if (result != PtSkinnedTlasRouteResult::Accepted)
        {
            if (firstFailure ==
                PtSkinnedTlasRouteResult::Accepted)
            {
                firstFailure = result;
            }
            IncrementSkinnedTlasFailure(
                plan.stats,
                result);
            continue;
        }

        previousShaderInstanceId =
            candidate.gpuRoute->shaderInstanceId;
        havePreviousShaderInstanceId = true;
        PtSkinnedTlasRouteRecord record;
        record.instanceKey =
            candidate.cpuRoute->instanceKey;
        record.shaderInstanceId =
            candidate.gpuRoute->shaderInstanceId;
        record.candidateIndex = candidateIndex;
        plan.records.push_back(record);
    }

    if (firstFailure !=
        PtSkinnedTlasRouteResult::Accepted)
    {
        plan.result = firstFailure;
        plan.stats.rejected = plan.stats.candidates;
        plan.records.clear();
        return plan;
    }

    plan.result = PtSkinnedTlasRouteResult::Accepted;
    plan.stats.accepted = plan.stats.candidates;
    return plan;
}

PtSkinnedCaptureAdmissionResult
PtPlanSkinnedCaptureAdmission(
    const PtSkinnedCaptureAdmissionInput& input)
{
    if (!input.gate)
    {
        return PtSkinnedCaptureAdmissionResult::GateDisabled;
    }
    if (input.priorRoute == nullptr)
    {
        return PtSkinnedCaptureAdmissionResult::MissingPriorRoute;
    }
    if (!PtCanonicalInstanceKeyIsValid(input.currentInstance) ||
        input.priorRoute->instanceKey != input.currentInstance)
    {
        return PtSkinnedCaptureAdmissionResult::
            CurrentInstanceMismatch;
    }
    if (!PtCanonicalMeshKeyIsValid(input.currentMesh) ||
        input.currentMesh.sourceDomain !=
            PtCanonicalMeshSourceDomain::SkinnedBindSource ||
        input.currentMesh.deformationClass !=
            PtCanonicalDeformationClass::Skinned ||
        input.priorRoute->meshKey != input.currentMesh ||
        input.currentSourceChecksum == 0 ||
        input.priorRoute->sourceChecksum !=
            input.currentSourceChecksum ||
        input.currentVertexCount == 0 ||
        input.currentIndexCount == 0 ||
        input.currentIndexCount % 3u != 0u ||
        input.priorRoute->vertexCount !=
            input.currentVertexCount ||
        input.priorRoute->indexCount !=
            input.currentIndexCount ||
        input.priorRoute->triangleCount !=
            input.currentIndexCount / 3u)
    {
        return PtSkinnedCaptureAdmissionResult::
            CurrentSourceMismatch;
    }
    if (!input.jointDataReady)
    {
        return PtSkinnedCaptureAdmissionResult::
            JointDataNotReady;
    }
    return PtSkinnedCaptureAdmissionResult::OmitCpuCapture;
}

const char* PtSkinnedTlasRouteResultName(
    PtSkinnedTlasRouteResult result)
{
    switch (result)
    {
        case PtSkinnedTlasRouteResult::Accepted:
            return "accepted";
        case PtSkinnedTlasRouteResult::GateDisabled:
            return "gate-disabled";
        case PtSkinnedTlasRouteResult::
            UploadRouteCountMismatch:
            return "upload-route-count-mismatch";
        case PtSkinnedTlasRouteResult::MissingCpuRoute:
            return "missing-cpu-route";
        case PtSkinnedTlasRouteResult::MissingGpuRoute:
            return "missing-gpu-route";
        case PtSkinnedTlasRouteResult::
            UploadContractMismatch:
            return "upload-contract-mismatch";
        case PtSkinnedTlasRouteResult::MissingResource:
            return "missing-resource";
        case PtSkinnedTlasRouteResult::
            ResourceContractMismatch:
            return "resource-contract-mismatch";
        case PtSkinnedTlasRouteResult::MissingBlas:
            return "missing-blas";
        case PtSkinnedTlasRouteResult::
            InvalidSbtSelection:
            return "invalid-sbt-selection";
        case PtSkinnedTlasRouteResult::
            TlasCapacityExceeded:
            return "tlas-capacity-exceeded";
        default:
            return "unknown";
    }
}

const char* PtSkinnedCaptureAdmissionResultName(
    PtSkinnedCaptureAdmissionResult result)
{
    switch (result)
    {
        case PtSkinnedCaptureAdmissionResult::
            OmitCpuCapture:
            return "omit-cpu-capture";
        case PtSkinnedCaptureAdmissionResult::GateDisabled:
            return "gate-disabled";
        case PtSkinnedCaptureAdmissionResult::
            MissingPriorRoute:
            return "missing-prior-route";
        case PtSkinnedCaptureAdmissionResult::
            CurrentInstanceMismatch:
            return "current-instance-mismatch";
        case PtSkinnedCaptureAdmissionResult::
            CurrentSourceMismatch:
            return "current-source-mismatch";
        case PtSkinnedCaptureAdmissionResult::
            JointDataNotReady:
            return "joint-data-not-ready";
        default:
            return "unknown";
    }
}
