#pragma once

// Phase A A3 rigid Instance record. POD / harness-safe.
// gpuResident is NOT a field. It is a derived per-frame proof (33 section 5).

#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace cpu_producer_publish
{

constexpr uint32_t kRigidInstanceDirtyXform = 1u << 0;
constexpr uint32_t kRigidInstanceDirtyMaterial = 1u << 2;
constexpr uint32_t kRigidInstanceDirtyMesh = 1u << 3;
constexpr uint32_t kOriginCaptureWalk = 1u << 0;
constexpr uint32_t kOriginRegistryHook = 1u << 1;
constexpr uint32_t kRigidInstancePresentDirty =
	kRigidInstanceDirtyXform | kRigidInstanceDirtyMaterial | kRigidInstanceDirtyMesh;

enum class RigidRegistryClass : uint32_t
{
	Unknown = 0,
	Rigid = 1,
	World = 2,
	Deforming = 3,
	Transient = 4
};

inline bool AdmitsRigidRegistryInstance(RigidRegistryClass geometryClass)
{
	return geometryClass == RigidRegistryClass::Rigid;
}

// instanceId = PtRenderDefKey (world, worldGen, index, slot.generation)
struct RigidRegistryInstanceKey
{
	const void* world = nullptr;
	uint64_t worldGeneration = 0;
	int index = -1;
	uint32_t generation = 0;
};

struct RigidRegistryXform
{
	float origin[3] = {};
	float axis[9] = {};
};

inline bool RigidRegistryXformsEqual(const RigidRegistryXform& a, const RigidRegistryXform& b)
{
	return std::memcmp(&a, &b, sizeof(RigidRegistryXform)) == 0;
}

inline void CopyRigidRegistryXform(RigidRegistryXform& dst, const RigidRegistryXform& src)
{
	dst = src;
}

struct RigidRegistryInstanceRecord
{
	RigidRegistryInstanceKey instanceId;
	// Complete ordered per-surface A2 hashes (eligible model surfaces).
	// A4 coalesces one extraTlas descriptor per (instanceId, surface meshId).
	// Do not implement A4 emit here. Not surface 0 only.
	std::vector<uint64_t> meshIds;
	RigidRegistryXform currentXform;
	RigidRegistryXform previousXform;
	uint32_t dirty = 0;
	uint32_t generation = 0;
	bool alive = false;
};

inline bool RigidRegistryMeshIdsComplete(const std::vector<uint64_t>& meshIds)
{
	if (meshIds.empty())
	{
		return false;
	}
	for (size_t index = 0; index < meshIds.size(); ++index)
	{
		if (meshIds[index] == 0)
		{
			return false;
		}
	}
	return true;
}

inline bool RigidRegistryInstanceKeyValid(const RigidRegistryInstanceKey& key)
{
	return key.world != nullptr && key.worldGeneration != 0 &&
		key.index >= 0 && key.generation != 0;
}

inline uint64_t PackRigidRegistryInstanceId(const RigidRegistryInstanceKey& key)
{
	uint64_t packed = reinterpret_cast<uint64_t>(key.world);
	packed ^= key.worldGeneration * 0x9E3779B97F4A7C15ull;
	packed ^= static_cast<uint64_t>(static_cast<uint32_t>(key.index)) *
		0xC2B2AE3D27D4EB4Full;
	packed ^= static_cast<uint64_t>(key.generation) * 0x165667B19E3779F9ull;
	return packed;
}

inline bool RigidRegistryInstanceKeysEqual(
	const RigidRegistryInstanceKey& a,
	const RigidRegistryInstanceKey& b)
{
	return RigidRegistryInstanceKeyValid(a) &&
		a.world == b.world &&
		a.worldGeneration == b.worldGeneration &&
		a.index == b.index &&
		a.generation == b.generation;
}

inline void FillRigidRegistryInstancePresent(
	RigidRegistryInstanceRecord& rec,
	const RigidRegistryInstanceKey& key,
	const std::vector<uint64_t>& meshIds,
	const RigidRegistryXform& xform)
{
	rec.instanceId = key;
	rec.meshIds = meshIds;
	rec.currentXform = xform;
	rec.previousXform = xform;
	rec.dirty = kRigidInstancePresentDirty;
	rec.generation = key.generation;
	rec.alive = true;
}

inline void FillRigidRegistryInstanceUpdate(
	RigidRegistryInstanceRecord& rec,
	const std::vector<uint64_t>& meshIds,
	const RigidRegistryXform& xform,
	bool remesh)
{
	rec.previousXform = rec.currentXform;
	rec.currentXform = xform;
	rec.dirty |= kRigidInstanceDirtyXform | kRigidInstanceDirtyMaterial;
	if (remesh && RigidRegistryMeshIdsComplete(meshIds))
	{
		rec.meshIds = meshIds;
		rec.dirty |= kRigidInstanceDirtyMesh;
	}
	rec.generation = rec.instanceId.generation;
	rec.alive = true;
}

inline void RetireRigidRegistryInstanceRecord(RigidRegistryInstanceRecord& rec)
{
	rec.alive = false;
}

inline bool PresentRigidRegistryInstance(
	std::unordered_map<uint64_t, RigidRegistryInstanceRecord>& table,
	const RigidRegistryInstanceKey& key,
	const std::vector<uint64_t>& meshIds,
	const RigidRegistryXform& xform,
	RigidRegistryClass geometryClass,
	RigidRegistryInstanceRecord* out = nullptr)
{
	if (!AdmitsRigidRegistryInstance(geometryClass) ||
		!RigidRegistryInstanceKeyValid(key) ||
		!RigidRegistryMeshIdsComplete(meshIds))
	{
		return false;
	}
	const uint64_t packed = PackRigidRegistryInstanceId(key);
	RigidRegistryInstanceRecord rec;
	FillRigidRegistryInstancePresent(rec, key, meshIds, xform);
	table[packed] = rec;
	if (out)
	{
		*out = rec;
	}
	return true;
}

inline bool UpdateRigidRegistryInstance(
	std::unordered_map<uint64_t, RigidRegistryInstanceRecord>& table,
	const RigidRegistryInstanceKey& key,
	const std::vector<uint64_t>& meshIds,
	const RigidRegistryXform& xform,
	bool remesh,
	RigidRegistryClass geometryClass,
	RigidRegistryInstanceRecord* out = nullptr)
{
	if (!AdmitsRigidRegistryInstance(geometryClass) ||
		!RigidRegistryInstanceKeyValid(key))
	{
		return false;
	}
	const uint64_t packed = PackRigidRegistryInstanceId(key);
	std::unordered_map<uint64_t, RigidRegistryInstanceRecord>::iterator it = table.find(packed);
	if (it == table.end() || !it->second.alive)
	{
		return PresentRigidRegistryInstance(table, key, meshIds, xform, geometryClass, out);
	}
	if (remesh && !RigidRegistryMeshIdsComplete(meshIds))
	{
		return false;
	}
	FillRigidRegistryInstanceUpdate(it->second, meshIds, xform, remesh);
	if (out)
	{
		*out = it->second;
	}
	return true;
}

inline bool FreeRigidRegistryInstance(
	std::unordered_map<uint64_t, RigidRegistryInstanceRecord>& table,
	const RigidRegistryInstanceKey& key)
{
	if (!RigidRegistryInstanceKeyValid(key))
	{
		return false;
	}
	const uint64_t packed = PackRigidRegistryInstanceId(key);
	std::unordered_map<uint64_t, RigidRegistryInstanceRecord>::iterator it = table.find(packed);
	if (it == table.end())
	{
		return false;
	}
	RetireRigidRegistryInstanceRecord(it->second);
	return true;
}

inline const RigidRegistryInstanceRecord* FindRigidRegistryInstance(
	const std::unordered_map<uint64_t, RigidRegistryInstanceRecord>& table,
	const RigidRegistryInstanceKey& key)
{
	if (!RigidRegistryInstanceKeyValid(key))
	{
		return nullptr;
	}
	const std::unordered_map<uint64_t, RigidRegistryInstanceRecord>::const_iterator it =
		table.find(PackRigidRegistryInstanceId(key));
	if (it == table.end())
	{
		return nullptr;
	}
	return &it->second;
}

inline bool RigidRegistryInstanceLive(
	const std::unordered_map<uint64_t, RigidRegistryInstanceRecord>& table,
	const RigidRegistryInstanceKey& key)
{
	const RigidRegistryInstanceRecord* rec = FindRigidRegistryInstance(table, key);
	return rec != nullptr && rec->alive;
}

// Submit-boundary metadata: one record per physical rigid extraTlas descriptor.
// Key = exact PtRenderDefKey + surface meshId. Survives through submit.
struct RigidSubmitBoundaryRecord
{
	RigidRegistryInstanceKey instanceId;
	uint64_t meshId = 0;
	uint32_t descriptorIndex = 0;
	uint32_t instanceID = 0;
	uint32_t instanceMask = 0;
	uint64_t submittedBlasToken = 0;
	uint32_t provenance = 0;
	bool exactIdentity = false;
	RigidRegistryXform currentXform;
};

struct RigidSubmitBoundaryList
{
	uint32_t rigidPhysicalCount = 0;
	std::vector<RigidSubmitBoundaryRecord> records;
};

inline bool RigidSubmitBoundaryExact(const RigidSubmitBoundaryRecord& rec)
{
	return rec.exactIdentity &&
		RigidRegistryInstanceKeyValid(rec.instanceId) &&
		rec.meshId != 0;
}

inline bool RigidSubmitBoundaryHasUnmappedPhysical(const RigidSubmitBoundaryList& list)
{
	if (list.rigidPhysicalCount > static_cast<uint32_t>(list.records.size()))
	{
		return true;
	}
	for (size_t index = 0; index < list.records.size(); ++index)
	{
		if (!RigidSubmitBoundaryExact(list.records[index]))
		{
			return true;
		}
	}
	return list.rigidPhysicalCount != static_cast<uint32_t>(list.records.size());
}

} // namespace cpu_producer_publish