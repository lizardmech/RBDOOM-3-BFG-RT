#include "PathTraceCanonicalGeometryIdentity.h"

#include <limits>

namespace {

const std::uint64_t PT_CANONICAL_HASH_OFFSET = 1469598103934665603ull;
const std::uint64_t PT_CANONICAL_HASH_PRIME = 1099511628211ull;

std::uint64_t HashCanonicalValue(std::uint64_t hash, std::uint64_t value)
{
    for (int byteIndex = 0; byteIndex < 8; ++byteIndex)
    {
        hash ^= static_cast<std::uint8_t>(value & 0xffu);
        hash *= PT_CANONICAL_HASH_PRIME;
        value >>= 8u;
    }
    return hash;
}

std::uint64_t HashCanonicalMeshKey(std::uint64_t hash, const PtCanonicalMeshKey& key)
{
    hash = HashCanonicalValue(hash, key.sourceAssetId);
    hash = HashCanonicalValue(hash, key.sourceAssetGeneration);
    hash = HashCanonicalValue(hash, key.topologySignature);
    hash = HashCanonicalValue(hash, static_cast<std::uint32_t>(key.sourceDomain));
    hash = HashCanonicalValue(hash, key.modelSurfaceIndex);
    hash = HashCanonicalValue(hash, key.vertexFormat);
    hash = HashCanonicalValue(hash, static_cast<std::uint32_t>(key.deformationClass));
    hash = HashCanonicalValue(hash, key.vertexCount);
    hash = HashCanonicalValue(hash, key.indexCount);
    hash = HashCanonicalValue(hash, static_cast<std::uint32_t>(key.jointSubmeshIndex));
    return hash;
}

}

bool operator==(const PtCanonicalWorldKey& lhs, const PtCanonicalWorldKey& rhs)
{
    return lhs.worldGeneration == rhs.worldGeneration;
}

bool operator!=(const PtCanonicalWorldKey& lhs, const PtCanonicalWorldKey& rhs)
{
    return !(lhs == rhs);
}

bool operator==(const PtCanonicalMeshKey& lhs, const PtCanonicalMeshKey& rhs)
{
    return lhs.sourceAssetId == rhs.sourceAssetId &&
        lhs.sourceAssetGeneration == rhs.sourceAssetGeneration &&
        lhs.topologySignature == rhs.topologySignature &&
        lhs.sourceDomain == rhs.sourceDomain &&
        lhs.modelSurfaceIndex == rhs.modelSurfaceIndex &&
        lhs.vertexFormat == rhs.vertexFormat &&
        lhs.deformationClass == rhs.deformationClass &&
        lhs.vertexCount == rhs.vertexCount &&
        lhs.indexCount == rhs.indexCount &&
        lhs.jointSubmeshIndex == rhs.jointSubmeshIndex;
}

bool operator!=(const PtCanonicalMeshKey& lhs, const PtCanonicalMeshKey& rhs)
{
    return !(lhs == rhs);
}

bool operator==(const PtCanonicalInstanceKey& lhs, const PtCanonicalInstanceKey& rhs)
{
    return lhs.worldGeneration == rhs.worldGeneration &&
        lhs.renderDefIndex == rhs.renderDefIndex &&
        lhs.renderDefGeneration == rhs.renderDefGeneration &&
        lhs.subInstanceKind == rhs.subInstanceKind &&
        lhs.modelSurfaceIndex == rhs.modelSurfaceIndex &&
        lhs.jointSubmeshIndex == rhs.jointSubmeshIndex;
}

bool operator!=(const PtCanonicalInstanceKey& lhs, const PtCanonicalInstanceKey& rhs)
{
    return !(lhs == rhs);
}

bool operator==(const PtCanonicalPrimitiveKey& lhs, const PtCanonicalPrimitiveKey& rhs)
{
    return lhs.mesh == rhs.mesh &&
        lhs.localPrimitiveIndex == rhs.localPrimitiveIndex;
}

bool operator!=(const PtCanonicalPrimitiveKey& lhs, const PtCanonicalPrimitiveKey& rhs)
{
    return !(lhs == rhs);
}

bool operator==(const PtCanonicalHistoryOwnerKey& lhs, const PtCanonicalHistoryOwnerKey& rhs)
{
    return lhs.worldGeneration == rhs.worldGeneration &&
        lhs.ownerGeneration == rhs.ownerGeneration &&
        lhs.ownerKind == rhs.ownerKind &&
        lhs.ownerRole == rhs.ownerRole;
}

bool operator!=(const PtCanonicalHistoryOwnerKey& lhs, const PtCanonicalHistoryOwnerKey& rhs)
{
    return !(lhs == rhs);
}

bool PtCanonicalWorldKeyIsValid(const PtCanonicalWorldKey& key)
{
    return key.worldGeneration != 0;
}

bool PtCanonicalMeshKeyIsValid(const PtCanonicalMeshKey& key)
{
    return key.sourceAssetId != 0 &&
        key.sourceAssetGeneration != 0 &&
        key.topologySignature != 0 &&
        key.sourceDomain != PtCanonicalMeshSourceDomain::Invalid &&
        key.sourceDomain != PtCanonicalMeshSourceDomain::UnsupportedTransient &&
        key.modelSurfaceIndex != UINT32_MAX &&
        key.deformationClass != PtCanonicalDeformationClass::Invalid &&
        key.vertexCount > 0 &&
        key.indexCount >= 3;
}

bool PtCanonicalInstanceKeyIsValid(const PtCanonicalInstanceKey& key)
{
    return key.worldGeneration != 0 &&
        key.renderDefIndex != UINT32_MAX &&
        key.renderDefGeneration != 0 &&
        key.subInstanceKind != PtCanonicalSubInstanceKind::Invalid &&
        key.modelSurfaceIndex != UINT32_MAX;
}

bool PtCanonicalPrimitiveKeyIsValid(const PtCanonicalPrimitiveKey& key)
{
    return PtCanonicalMeshKeyIsValid(key.mesh) &&
        key.localPrimitiveIndex < key.mesh.indexCount / 3u;
}

bool PtCanonicalHistoryOwnerKeyIsValid(const PtCanonicalHistoryOwnerKey& key)
{
    return key.worldGeneration != 0 &&
        key.ownerGeneration != 0 &&
        key.ownerKind != PtCanonicalHistoryOwnerKind::Invalid &&
        key.ownerRole != PtCanonicalHistoryOwnerRole::Invalid;
}

std::uint64_t PtHashCanonicalWorldKey(const PtCanonicalWorldKey& key)
{
    return HashCanonicalValue(PT_CANONICAL_HASH_OFFSET, key.worldGeneration);
}

std::uint64_t PtHashCanonicalMeshKey(const PtCanonicalMeshKey& key)
{
    return HashCanonicalMeshKey(PT_CANONICAL_HASH_OFFSET, key);
}

std::uint64_t PtHashCanonicalInstanceKey(const PtCanonicalInstanceKey& key)
{
    std::uint64_t hash = PT_CANONICAL_HASH_OFFSET;
    hash = HashCanonicalValue(hash, key.worldGeneration);
    hash = HashCanonicalValue(hash, key.renderDefIndex);
    hash = HashCanonicalValue(hash, key.renderDefGeneration);
    hash = HashCanonicalValue(hash, static_cast<std::uint32_t>(key.subInstanceKind));
    hash = HashCanonicalValue(hash, key.modelSurfaceIndex);
    hash = HashCanonicalValue(hash, static_cast<std::uint32_t>(key.jointSubmeshIndex));
    return hash;
}

std::uint64_t PtHashCanonicalPrimitiveKey(const PtCanonicalPrimitiveKey& key)
{
    std::uint64_t hash = HashCanonicalMeshKey(PT_CANONICAL_HASH_OFFSET, key.mesh);
    return HashCanonicalValue(hash, key.localPrimitiveIndex);
}

std::uint64_t PtHashCanonicalHistoryOwnerKey(const PtCanonicalHistoryOwnerKey& key)
{
    std::uint64_t hash = PT_CANONICAL_HASH_OFFSET;
    hash = HashCanonicalValue(hash, key.worldGeneration);
    hash = HashCanonicalValue(hash, key.ownerGeneration);
    hash = HashCanonicalValue(hash, static_cast<std::uint32_t>(key.ownerKind));
    hash = HashCanonicalValue(hash, static_cast<std::uint32_t>(key.ownerRole));
    return hash;
}

std::uint32_t PtCompareCanonicalIdentityObservations(
    const PtCanonicalIdentityObservation& lhs,
    const PtCanonicalIdentityObservation& rhs)
{
    std::uint32_t flags = PT_CANONICAL_IDENTITY_EXACT;
    if (!lhs.present || !rhs.present ||
        lhs.producer == PtCanonicalIdentityProducer::Invalid ||
        rhs.producer == PtCanonicalIdentityProducer::Invalid)
    {
        flags |= PT_CANONICAL_IDENTITY_MISSING_PRODUCER;
    }
    if (!lhs.registryEligible || !rhs.registryEligible ||
        lhs.mesh.sourceDomain == PtCanonicalMeshSourceDomain::UnsupportedTransient ||
        rhs.mesh.sourceDomain == PtCanonicalMeshSourceDomain::UnsupportedTransient)
    {
        flags |= PT_CANONICAL_IDENTITY_UNSUPPORTED_TRANSIENT;
    }
    if (!lhs.hasDurableWorld || !rhs.hasDurableWorld ||
        !PtCanonicalWorldKeyIsValid(lhs.world) ||
        !PtCanonicalWorldKeyIsValid(rhs.world))
    {
        flags |= PT_CANONICAL_IDENTITY_MISSING_DURABLE_WORLD;
    }
    if (!lhs.hasDurableAsset || !rhs.hasDurableAsset ||
        lhs.mesh.sourceAssetId == 0 || rhs.mesh.sourceAssetId == 0 ||
        lhs.mesh.sourceAssetGeneration == 0 || rhs.mesh.sourceAssetGeneration == 0)
    {
        flags |= PT_CANONICAL_IDENTITY_MISSING_DURABLE_ASSET;
    }
    if (lhs.world != rhs.world ||
        lhs.instance.worldGeneration != rhs.instance.worldGeneration)
    {
        flags |= PT_CANONICAL_IDENTITY_WORLD_MISMATCH;
    }
    if (lhs.instance.renderDefIndex != rhs.instance.renderDefIndex ||
        lhs.instance.renderDefGeneration != rhs.instance.renderDefGeneration ||
        lhs.instance.subInstanceKind != rhs.instance.subInstanceKind ||
        lhs.instance.jointSubmeshIndex != rhs.instance.jointSubmeshIndex)
    {
        flags |= PT_CANONICAL_IDENTITY_INSTANCE_SLOT_MISMATCH;
    }
    if (lhs.instance.modelSurfaceIndex != rhs.instance.modelSurfaceIndex ||
        lhs.mesh.modelSurfaceIndex != rhs.mesh.modelSurfaceIndex)
    {
        flags |= PT_CANONICAL_IDENTITY_SURFACE_ORDINAL_MISMATCH;
    }
    if (lhs.mesh.sourceAssetId != rhs.mesh.sourceAssetId ||
        lhs.mesh.sourceAssetGeneration != rhs.mesh.sourceAssetGeneration ||
        lhs.mesh.sourceDomain != rhs.mesh.sourceDomain)
    {
        flags |= PT_CANONICAL_IDENTITY_SOURCE_ASSET_MISMATCH;
    }
    if (lhs.mesh.topologySignature != rhs.mesh.topologySignature ||
        lhs.mesh.vertexCount != rhs.mesh.vertexCount ||
        lhs.mesh.indexCount != rhs.mesh.indexCount)
    {
        flags |= PT_CANONICAL_IDENTITY_TOPOLOGY_MISMATCH;
    }
    if (lhs.mesh.vertexFormat != rhs.mesh.vertexFormat ||
        lhs.mesh.deformationClass != rhs.mesh.deformationClass ||
        lhs.mesh.jointSubmeshIndex != rhs.mesh.jointSubmeshIndex)
    {
        flags |= PT_CANONICAL_IDENTITY_LAYOUT_OR_DEFORMATION_MISMATCH;
    }
    if (lhs.hasDerivedHashes && rhs.hasDerivedHashes)
    {
        if (lhs.meshHash == rhs.meshHash && lhs.mesh != rhs.mesh)
        {
            flags |= PT_CANONICAL_IDENTITY_MESH_HASH_COLLISION;
        }
        if (lhs.instanceHash == rhs.instanceHash && lhs.instance != rhs.instance)
        {
            flags |= PT_CANONICAL_IDENTITY_INSTANCE_HASH_COLLISION;
        }
    }

    const bool sameGeometryIdentity =
        lhs.world == rhs.world &&
        lhs.mesh == rhs.mesh &&
        lhs.instance == rhs.instance;
    if (sameGeometryIdentity &&
        lhs.materialBindingRevision != rhs.materialBindingRevision)
    {
        flags |= PT_CANONICAL_IDENTITY_MATERIAL_ONLY_DIFFERENCE;
    }
    if (sameGeometryIdentity &&
        lhs.storageGeneration != rhs.storageGeneration)
    {
        flags |= PT_CANONICAL_IDENTITY_TRANSIENT_STORAGE_ONLY_DIFFERENCE;
    }
    return flags;
}

bool PtCheckedAddU64(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& result)
{
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs)
    {
        result = 0;
        return false;
    }
    result = lhs + rhs;
    return true;
}

bool PtCheckedMulU64(std::uint64_t lhs, std::uint64_t rhs, std::uint64_t& result)
{
    if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs)
    {
        result = 0;
        return false;
    }
    result = lhs * rhs;
    return true;
}

PtCanonicalShaderNarrowResult PtNarrowCanonicalRangeToShaderPage(
    const PtCanonicalShaderPageRange& input,
    PtCanonicalShaderRange32& output)
{
    output = PtCanonicalShaderRange32();
    if (input.byteSize == 0)
    {
        return PtCanonicalShaderNarrowResult::EmptyRange;
    }
    if (input.elementStrideBytes == 0)
    {
        return PtCanonicalShaderNarrowResult::InvalidElementStride;
    }

    std::uint64_t pageEnd = 0;
    std::uint64_t rangeEnd = 0;
    if (!PtCheckedAddU64(input.pageBaseBytes, input.pageSizeBytes, pageEnd) ||
        !PtCheckedAddU64(input.absoluteOffsetBytes, input.byteSize, rangeEnd))
    {
        return PtCanonicalShaderNarrowResult::ArithmeticOverflow;
    }
    if (input.absoluteOffsetBytes < input.pageBaseBytes)
    {
        return PtCanonicalShaderNarrowResult::BeforePage;
    }

    const std::uint64_t localOffsetBytes = input.absoluteOffsetBytes - input.pageBaseBytes;
    if ((localOffsetBytes % input.elementStrideBytes) != 0 ||
        (input.byteSize % input.elementStrideBytes) != 0)
    {
        return PtCanonicalShaderNarrowResult::Misaligned;
    }
    if (rangeEnd > pageEnd)
    {
        return PtCanonicalShaderNarrowResult::OutsidePage;
    }

    const std::uint64_t elementOffset = localOffsetBytes / input.elementStrideBytes;
    const std::uint64_t elementCount = input.byteSize / input.elementStrideBytes;
    if (elementOffset > std::numeric_limits<std::uint32_t>::max() ||
        elementCount > std::numeric_limits<std::uint32_t>::max())
    {
        return PtCanonicalShaderNarrowResult::ShaderWidthExceeded;
    }

    output.elementOffset = static_cast<std::uint32_t>(elementOffset);
    output.elementCount = static_cast<std::uint32_t>(elementCount);
    return PtCanonicalShaderNarrowResult::Success;
}
