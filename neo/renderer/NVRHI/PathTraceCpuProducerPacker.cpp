#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCpuProducerPacker.h"
#include "PathTraceCpuProducerPackFormat.h"
#include "PathTraceCVars.h"
#include "PathTraceGeometryLifecycle.h"
#include "PathTraceSceneCapture.h"
#include "../RenderCommon.h"
#include "../RenderWorld_local.h"

#include <string>
#include <unordered_map>

namespace
{

const int kPackFrameCount = 3;

struct PackerState
{
	idStr dest;
	int framesCollected = 0;
	cpu_producer_pack::PackTables pack;
	std::unordered_map<uint64_t, size_t> instanceIndex;
	std::unordered_map<uint64_t, size_t> lightIndex;
};

PackerState g_packer;

void ResetPacker()
{
	g_packer = PackerState();
}

void UpsertInstance(const cpu_producer_pack::InstanceRecord& rec)
{
	auto it = g_packer.instanceIndex.find(rec.instanceId);
	if (it == g_packer.instanceIndex.end())
	{
		g_packer.instanceIndex[rec.instanceId] = g_packer.pack.instances.size();
		g_packer.pack.instances.push_back(rec);
		return;
	}
	cpu_producer_pack::InstanceRecord& dst = g_packer.pack.instances[it->second];
	if (rec.meshId != 0)
	{
		dst.meshId = rec.meshId;
	}
	dst.dirty |= rec.dirty;
	if (rec.generation > dst.generation)
	{
		dst.generation = rec.generation;
	}
	dst.live = rec.live | dst.live;
	dst.omittedSkin = rec.omittedSkin | dst.omittedSkin;
	if (rec.vertexCount > dst.vertexCount)
	{
		dst.vertexCount = rec.vertexCount;
	}
}

void UpsertMesh(const cpu_producer_pack::MeshRecord& rec)
{
	if (rec.meshId == 0)
	{
		return;
	}
	for (cpu_producer_pack::MeshRecord& dst : g_packer.pack.meshes)
	{
		if (dst.meshId == rec.meshId)
		{
			if (rec.vertexCount > dst.vertexCount)
			{
				dst.vertexCount = rec.vertexCount;
			}
			if (rec.indexCount > dst.indexCount)
			{
				dst.indexCount = rec.indexCount;
			}
			if (rec.contentChecksum != 0)
			{
				dst.contentChecksum = rec.contentChecksum;
			}
			if (rec.deformationClass != 0)
			{
				dst.deformationClass = rec.deformationClass;
			}
			return;
		}
	}
	g_packer.pack.meshes.push_back(rec);
}

void UpsertMaterial(const cpu_producer_pack::MaterialRecord& rec)
{
	if (rec.materialId == 0)
	{
		return;
	}
	for (cpu_producer_pack::MaterialRecord& dst : g_packer.pack.materials)
	{
		if (dst.materialId == rec.materialId)
		{
			if (rec.generation > dst.generation)
			{
				dst.generation = rec.generation;
			}
			if (rec.overlayGeneration > dst.overlayGeneration)
			{
				dst.overlayGeneration = rec.overlayGeneration;
			}
			dst.emissiveBase = dst.emissiveBase | rec.emissiveBase;
			return;
		}
	}
	g_packer.pack.materials.push_back(rec);
}

void UpsertLight(const cpu_producer_pack::LightRecord& rec)
{
	auto it = g_packer.lightIndex.find(rec.lightId);
	if (it == g_packer.lightIndex.end())
	{
		g_packer.lightIndex[rec.lightId] = g_packer.pack.lights.size();
		g_packer.pack.lights.push_back(rec);
		return;
	}
	g_packer.pack.lights[it->second] = rec;
}

void FinishPack(const char* dest)
{
	g_packer.pack.manifest.schema = cpu_producer_pack::kSchemaName;
	g_packer.pack.manifest.schemaVersion = cpu_producer_pack::kSchemaVersion;
	g_packer.pack.manifest.note = "phase-c";
	g_packer.pack.manifest.frameCount = static_cast<uint32_t>(g_packer.framesCollected);
	std::string err;
	if (!cpu_producer_pack::WritePack(dest, g_packer.pack, err))
	{
		common->Warning("PathTraceCpuProducerPack: write failed dir=%s err=%s", dest, err.c_str());
		return;
	}
	common->Printf(
		"PathTraceCpuProducerPack: wrote %s frames=%d instances=%d lights=%d\n",
		dest,
		g_packer.framesCollected,
		static_cast<int>(g_packer.pack.instances.size()),
		static_cast<int>(g_packer.pack.lights.size()));
}

} // namespace

namespace PtCpuProducerPacker
{

void MaybeCaptureFrame(
	const viewDef_t* viewDef,
	const RtSmokeSceneCaptureTiming& captureTiming,
	unsigned long long buildSceneUs,
	const std::vector<RtSmokeSkinnedSurfaceRecord>* skinnedSurfaceRecords)
{
	const char* requested = r_pathTracingCpuProducerPack.GetString();
	if (requested == nullptr || requested[0] == '\0')
	{
		if (g_packer.framesCollected > 0)
		{
			ResetPacker();
		}
		return;
	}
	if (viewDef == nullptr || viewDef->isSubview)
	{
		return;
	}

	if (g_packer.dest.IsEmpty() || g_packer.dest.Icmp(requested) != 0)
	{
		ResetPacker();
		g_packer.dest = requested;
		common->Printf("PathTraceCpuProducerPack: capturing 3 frames to %s\n", requested);
	}

	const idRenderWorldLocal* world = viewDef->renderWorld;
	std::vector<PtGeometryLifecycle::PackedInstanceId> instances;
	std::vector<PtGeometryLifecycle::PackedLightId> lights;
	PtGeometryLifecycle::SnapshotPackedIds(world, instances, lights);
	const PtGeometryLifecycle::FrameCounters counters = PtGeometryLifecycle::PeekFrameCounters();

	cpu_producer_pack::FrameRecord frame = {};
	frame.frameIndex = static_cast<uint32_t>(g_packer.framesCollected);

	for (const PtGeometryLifecycle::PackedInstanceId& inst : instances)
	{
		cpu_producer_pack::InstanceRecord rec = {};
		rec.instanceId = inst.instanceId;
		rec.meshId = inst.meshId;
		rec.dirty = inst.dirty;
		rec.generation = inst.generation;
		rec.live = inst.live;
		UpsertInstance(rec);
		if (inst.meshId != 0)
		{
			cpu_producer_pack::MeshRecord mesh{};
			mesh.meshId = inst.meshId;
			mesh.contentChecksum = inst.meshId;
			mesh.generation = inst.generation;
			UpsertMesh(mesh);
		}
		if (inst.live && inst.dirty != 0)
		{
			++frame.dirtyInst;
			if (inst.dirty & cpu_producer_pack::kDirtyXform)
			{
				++frame.dirtyXform;
			}
			if (inst.dirty & cpu_producer_pack::kDirtyJoints)
			{
				++frame.dirtyJoints;
			}
			if (inst.dirty & cpu_producer_pack::kDirtyMaterial)
			{
				++frame.dirtyMaterial;
			}
			if (inst.dirty & cpu_producer_pack::kDirtyMesh)
			{
				++frame.dirtyMesh;
			}
		}
	}
	if (skinnedSurfaceRecords != nullptr)
	{
		for (const RtSmokeSkinnedSurfaceRecord& skin : *skinnedSurfaceRecords)
		{
			const uint32_t defIndex = skin.canonicalInstance.renderDefIndex;
			const uint32_t surfaceIndex = skin.canonicalInstance.modelSurfaceIndex;
			cpu_producer_pack::InstanceRecord rec{};
			rec.instanceId = cpu_producer_pack::PackSkinnedSurfaceId(
				skin.canonicalInstance.worldGeneration,
				defIndex,
				surfaceIndex);
			if (world != nullptr && skin.entityIndex >= 0)
			{
				rec.meshId = 0;
			}
			rec.dirty = 0;
			rec.generation = skin.canonicalInstance.renderDefGeneration;
			rec.live = 1;
			rec.omittedSkin = skin.cpuCaptureOmitted ? 1u : 0u;
			rec.vertexCount = skin.vertexCount > 0 ? static_cast<uint32_t>(skin.vertexCount) : 0u;
			// meshId from lifecycle slot if the entity is already tracked
			if (skin.entityIndex >= 0)
			{
				for (const PtGeometryLifecycle::PackedInstanceId& inst : instances)
				{
					if ((inst.instanceId & 0xffffffffull) == static_cast<uint32_t>(skin.entityIndex) &&
						inst.meshId != 0)
					{
						rec.meshId = inst.meshId;
						break;
					}
				}
			}
			UpsertInstance(rec);

			cpu_producer_pack::MeshRecord mesh{};
			mesh.meshId = rec.meshId != 0 ? rec.meshId : rec.instanceId;
			if (rec.meshId == 0)
			{
				rec.meshId = mesh.meshId;
				UpsertInstance(rec);
			}
			mesh.contentChecksum = rec.meshId;
			mesh.deformationClass = 2; // skinned-source
			mesh.generation = rec.generation;
			mesh.vertexCount = rec.vertexCount;
			mesh.indexCount = skin.indexCount > 0 ? static_cast<uint32_t>(skin.indexCount) : 0u;
			UpsertMesh(mesh);

			if (skin.materialId != 0)
			{
				cpu_producer_pack::MaterialRecord material{};
				material.materialId = skin.materialId;
				material.generation = 1;
				UpsertMaterial(material);
			}

			if (skin.cpuCaptureOmitted)
			{
				++frame.omit;
			}
			else
			{
				++frame.admit;
			}
		}
	}
	if (frame.omit == 0 && captureTiming.skinnedCaptureOmittedSurfaces > 0)
	{
		frame.omit = static_cast<uint32_t>(captureTiming.skinnedCaptureOmittedSurfaces);
	}
	if (frame.admit == 0 && captureTiming.skinnedCaptureAdmissionRoutes > 0)
	{
		frame.admit = static_cast<uint32_t>(captureTiming.skinnedCaptureAdmissionRoutes);
	}

	for (const PtGeometryLifecycle::PackedLightId& light : lights)
	{
		cpu_producer_pack::LightRecord rec = {};
		rec.lightId = light.lightId;
		rec.dirty = light.dirty;
		rec.generation = light.generation;
		rec.live = light.live;
		UpsertLight(rec);
		if (light.live && light.dirty != 0)
		{
			++frame.dirtyLight;
		}
	}
	g_packer.pack.frames.push_back(frame);

	cpu_producer_pack::OracleRecord oracle = {};
	oracle.frameIndex = frame.frameIndex;
	oracle.validationMs = captureTiming.validationMs;
	oracle.dynamicPassClassifyMs = captureTiming.dynamicPassClassifyMs;
	oracle.dynamicAppendMs = captureTiming.dynamicAppendMs;
	oracle.rtCpuSkinningAppendMs = captureTiming.rtCpuSkinningAppendMs;
	oracle.skinnedCaptureAdmissionRoutes = captureTiming.skinnedCaptureAdmissionRoutes;
	oracle.skinnedCaptureOmittedSurfaces = captureTiming.skinnedCaptureOmittedSurfaces;
	oracle.staticCachedSurfaces = captureTiming.staticCachedSurfaces;
	oracle.staticNewSurfaces = captureTiming.staticNewSurfaces;
	oracle.entityAdds = counters.entityAdds;
	oracle.entityUpdates = counters.entityUpdates;
	oracle.entityUnchanged = counters.entityUnchanged;
	oracle.entityFrees = counters.entityFrees;
	oracle.lightAdds = counters.lightAdds;
	oracle.lightUpdates = counters.lightUpdates;
	oracle.lightFrees = counters.lightFrees;
	oracle.rtCpuSkinningAppendUs = captureTiming.rtCpuSkinningAppendUs;
	oracle.buildSceneUs = static_cast<uint64_t>(buildSceneUs);
	// Prefer the explicit PT Build Scene timer. Fall back to capture-block ms.
	uint64_t captureUs = 0;
	if (captureTiming.validationMs > 0 || captureTiming.dynamicAppendMs > 0 ||
		captureTiming.dynamicPassClassifyMs > 0)
	{
		captureUs = static_cast<uint64_t>(
			captureTiming.validationMs + captureTiming.dynamicPassClassifyMs +
			captureTiming.dynamicAppendMs + captureTiming.rtCpuSkinningAppendMs) *
			1000ull;
	}
	oracle.oracleUs = oracle.buildSceneUs != 0 ? oracle.buildSceneUs : captureUs;
	oracle.skinnedCaptureOmittedVerts = captureTiming.skinnedCaptureOmittedVerts;
	oracle.skinnedCaptureOmittedIndexes = captureTiming.skinnedCaptureOmittedIndexes;
	g_packer.pack.oracles.push_back(oracle);

	if (g_packer.pack.membership.empty() && viewDef->areaNum >= 0)
	{
		cpu_producer_pack::MembershipRecord membership = {};
		membership.areaNum = viewDef->areaNum;
		g_packer.pack.membership.push_back(membership);
		g_packer.pack.manifest.membershipPresent = 1;
	}

	if (world != nullptr && g_packer.pack.manifest.saveName.empty())
	{
		g_packer.pack.manifest.saveName = world->mapName.c_str();
	}
	g_packer.pack.manifest.width = viewDef->viewport.GetWidth();
	g_packer.pack.manifest.height = viewDef->viewport.GetHeight();
	g_packer.pack.manifest.gi = r_pathTracingCleanRestirGiEnable.GetInteger();

	++g_packer.framesCollected;
	if (g_packer.framesCollected >= kPackFrameCount)
	{
		idStr dest = g_packer.dest;
		FinishPack(dest.c_str());
		ResetPacker();
		r_pathTracingCpuProducerPack.SetString("");
	}
}

} // namespace PtCpuProducerPacker