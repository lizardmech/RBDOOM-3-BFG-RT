// Phase D: replay packed dirty tokens. Compact records only. No drawSurf
// walk, no vertex dump, no BLAS, no oracle_us copy.
#pragma once

#include "PathTraceCpuProducerPackFormat.h"

#include <chrono>
#include <cstdint>

namespace cpu_producer_pack
{

struct ApplyResult
{
	uint64_t gatherUs = 0;
	uint64_t lastFrameDirtyApplied = 0;
	uint64_t residentInstances = 0;
	uint64_t omittedSkinResident = 0;
};

// H1: skip-all-clean is illegal until gpuResident exists. Schema-3 packs
// have no gpuResident field; it defaults to 0.
struct CompletenessStats
{
	uint32_t skipCandidates = 0;
	uint32_t gpuResident = 0;
	uint32_t omitSkin = 0;
	uint32_t skipSet = 0;
	bool wouldDrop = false;
};

inline CompletenessStats MeasureCompleteness(const PackTables& pack)
{
	CompletenessStats stats;
	for (const InstanceRecord& inst : pack.instances)
	{
		if (inst.live && inst.dirty == 0)
		{
			++stats.skipCandidates;
		}
		if (inst.omittedSkin)
		{
			++stats.omitSkin;
		}
	}
	stats.gpuResident = 0;
	// H2 skip set is omitted-skin only, never the full clean table.
	stats.skipSet = stats.omitSkin;
	stats.wouldDrop = stats.skipCandidates > stats.gpuResident;
	return stats;
}

inline bool SkipAllCleanWouldStarveGpu(const CompletenessStats& stats)
{
	return stats.wouldDrop && stats.skipCandidates > stats.omitSkin;
}

inline ApplyResult ApplyCpuProducerPack(const PackTables& pack)
{
	ApplyResult result;
	std::vector<InstanceRecord> insts = pack.instances;
	std::vector<LightRecord> lights = pack.lights;
	result.residentInstances = insts.size();
	for (const InstanceRecord& inst : insts)
	{
		if (inst.omittedSkin)
		{
			++result.omittedSkinResident;
		}
	}

	uint64_t lastUs = 0;
	uint64_t lastDirty = 0;
	for (const FrameRecord& frame : pack.frames)
	{
		const auto t0 = std::chrono::steady_clock::now();
		volatile uint64_t sink = static_cast<uint64_t>(frame.frameIndex);

		// Cheap membership: portal/area ids only, never the instance table.
		for (const MembershipRecord& member : pack.membership)
		{
			sink += static_cast<uint32_t>(member.areaNum);
		}

		uint64_t dirtyApplied = 0;
		uint32_t dirtyBudget = frame.dirtyInst;
		if (dirtyBudget > 0)
		{
		for (InstanceRecord& inst : insts)
		{
			if (!inst.live || inst.dirty == 0 || dirtyBudget == 0)
			{
				continue;
			}
			--dirtyBudget;
			uint64_t work = inst.instanceId ^ inst.meshId;
			if (inst.dirty & kDirtyXform)
			{
				work = work * 1315423911ull + inst.generation;
			}
			if (inst.dirty & kDirtyJoints)
			{
				work += static_cast<uint64_t>(inst.vertexCount) * 3ull + 1ull;
			}
			if (inst.dirty & kDirtyMaterial)
			{
				work ^= 0x9e3779b97f4a7c15ull;
			}
			if (inst.dirty & kDirtyMesh)
			{
				work += inst.generation + 1u;
			}
			inst.generation = static_cast<uint32_t>(work);
			sink += work;
			++dirtyApplied;
		}
		}
		uint32_t lightBudget = frame.dirtyLight;
		if (lightBudget > 0)
		{
		for (LightRecord& light : lights)
		{
			if (!light.live || light.dirty == 0 || lightBudget == 0)
			{
				continue;
			}
			--lightBudget;
			const uint64_t work = light.lightId + light.generation + 1u;
			light.generation = static_cast<uint32_t>(work);
			sink += work;
		}
		}
		(void)sink;

		const auto t1 = std::chrono::steady_clock::now();
		uint64_t us = static_cast<uint64_t>(
			std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
		if (us == 0 && dirtyApplied > 0)
		{
			us = 1;
		}
		lastUs = us;
		lastDirty = dirtyApplied;
	}
	result.gatherUs = lastUs;
	result.lastFrameDirtyApplied = lastDirty;
	return result;
}

// Capture-shaped if apply still tracks pixel/oracle cost instead of dirties.
inline bool GatherTracksOracleRatio(
	uint64_t gatherA, uint64_t gatherB, uint64_t oracleA, uint64_t oracleB)
{
	if (gatherB == 0 || oracleB == 0)
	{
		return false;
	}
	const double gatherRatio = static_cast<double>(gatherA) / static_cast<double>(gatherB);
	const double oracleRatio = static_cast<double>(oracleA) / static_cast<double>(oracleB);
	const double rel = (gatherRatio > oracleRatio)
		? (gatherRatio - oracleRatio) / oracleRatio
		: (oracleRatio - gatherRatio) / oracleRatio;
	return rel <= 0.20;
}

} // namespace cpu_producer_pack