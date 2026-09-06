#pragma once

// Shared A2 / rigid-route Mesh identity. POD only -- harness-safe.
// Production persist and the shipped rigid route must hash these fields.

#include <cstddef>
#include <cstdint>

namespace cpu_producer_publish
{

inline uint64_t HashRigidMeshIdentityBytes(uint64_t hash, const void* data, size_t size)
{
	const unsigned char* bytes = static_cast<const unsigned char*>(data);
	for (size_t index = 0; index < size; ++index)
	{
		hash ^= static_cast<uint64_t>(bytes[index]);
		hash *= 1099511628211ull;
	}
	return hash;
}

// Lifecycle slot default. EntityModelEpoch returns this when no slot exists.
constexpr uint32_t kRigidMeshInitialModelEpoch = 1;

struct RigidMeshIdentityInputs
{
	uint64_t vertexBufferIdentity = 0;
	uint64_t indexBufferIdentity = 0;
	int numVerts = 0;
	int numIndexes = 0;
	uint32_t vertexFormat = 0;
	uint32_t materialId = 0;
	uint32_t materialClassSignature = 0;
	uint32_t sourceKind = 0;
	uint64_t modelIdentity = 0;
	uint32_t modelEpoch = 0;
	int modelSurfaceIndex = -1;
	int jointIndex = -1;
};

// Shipped route identities: ambientCache / indexCache, not CPU verts/indexes.
inline uint64_t RigidMeshVertexCacheIdentity(uint64_t ambientCache)
{
	return ambientCache;
}

inline uint64_t RigidMeshIndexCacheIdentity(uint64_t indexCache)
{
	return indexCache;
}

// Canonical Mesh-key builder. Persist and the rigid route must call this
// with cache identities, the routed material-class signature, and
// EntityModelEpoch (initial kRigidMeshInitialModelEpoch).
inline RigidMeshIdentityInputs MakeRigidMeshIdentity(
	uint64_t modelIdentity,
	uint32_t modelEpoch,
	int modelSurfaceIndex,
	uint64_t ambientCache,
	uint64_t indexCache,
	int numVerts,
	int numIndexes,
	uint32_t vertexFormat,
	uint32_t materialId,
	uint32_t materialClassSignature,
	uint32_t sourceKind,
	int jointIndex = -1)
{
	RigidMeshIdentityInputs in;
	in.vertexBufferIdentity = RigidMeshVertexCacheIdentity(ambientCache);
	in.indexBufferIdentity = RigidMeshIndexCacheIdentity(indexCache);
	in.numVerts = numVerts;
	in.numIndexes = numIndexes;
	in.vertexFormat = vertexFormat;
	in.materialId = materialId;
	in.materialClassSignature = materialClassSignature;
	in.sourceKind = sourceKind;
	in.modelIdentity = modelIdentity;
	in.modelEpoch = modelEpoch;
	in.modelSurfaceIndex = modelSurfaceIndex;
	in.jointIndex = jointIndex;
	return in;
}

inline uint64_t HashRigidMeshIdentity(const RigidMeshIdentityInputs& in)
{
	uint64_t hash = 14695981039346656037ull;
	hash = HashRigidMeshIdentityBytes(hash, &in.modelIdentity, sizeof(in.modelIdentity));
	hash = HashRigidMeshIdentityBytes(hash, &in.modelEpoch, sizeof(in.modelEpoch));
	hash = HashRigidMeshIdentityBytes(hash, &in.modelSurfaceIndex, sizeof(in.modelSurfaceIndex));
	hash = HashRigidMeshIdentityBytes(hash, &in.vertexBufferIdentity, sizeof(in.vertexBufferIdentity));
	hash = HashRigidMeshIdentityBytes(hash, &in.indexBufferIdentity, sizeof(in.indexBufferIdentity));
	hash = HashRigidMeshIdentityBytes(hash, &in.numVerts, sizeof(in.numVerts));
	hash = HashRigidMeshIdentityBytes(hash, &in.numIndexes, sizeof(in.numIndexes));
	hash = HashRigidMeshIdentityBytes(hash, &in.vertexFormat, sizeof(in.vertexFormat));
	hash = HashRigidMeshIdentityBytes(hash, &in.materialId, sizeof(in.materialId));
	hash = HashRigidMeshIdentityBytes(hash, &in.materialClassSignature, sizeof(in.materialClassSignature));
	hash = HashRigidMeshIdentityBytes(hash, &in.sourceKind, sizeof(in.sourceKind));
	if (in.jointIndex >= 0)
	{
		hash = HashRigidMeshIdentityBytes(hash, &in.jointIndex, sizeof(in.jointIndex));
	}
	return hash;
}

} // namespace cpu_producer_publish