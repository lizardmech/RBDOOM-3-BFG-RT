#include "precompiled.h"
#pragma hdrstop

#include "PathTraceCpuProducerApplyGate.h"
#include "PathTraceCVars.h"
#include "PathTraceSceneCapture.h"
#include "../RenderCommon.h"

#include <unordered_set>
#include <algorithm>

namespace
{

bool g_enabled = false;
bool g_acceptedSkinnedBuildLive = false;
std::unordered_set<uint64_t> g_omittedSkinKeys;
int g_omittedResident = 0;
int g_skipCount = 0;
int g_captureCount = 0;
int g_logCooldown = 0;
bool g_wasEnabled = false;

uint64_t OmittedSkinKey(int entityIndex, int modelSurfaceIndex)
{
	if (entityIndex < 0)
	{
		return 0;
	}
	const uint32_t surface = modelSurfaceIndex >= 0
		? static_cast<uint32_t>(modelSurfaceIndex)
		: 0xffffffffu;
	return (static_cast<uint64_t>(static_cast<uint32_t>(entityIndex)) << 32) |
		static_cast<uint64_t>(surface);
}

} // namespace

namespace PtCpuProducerApplyGate
{

void BeginFrame(
	const viewDef_t* viewDef,
	const std::vector<RtSmokeSkinnedSurfaceRecord>* priorSkinnedRecords)
{
	g_enabled = r_pathTracingCpuProducerApply.GetInteger() != 0;
	g_acceptedSkinnedBuildLive = false;
	g_omittedSkinKeys.clear();
	g_omittedResident = 0;
	g_skipCount = 0;
	g_captureCount = 0;
	if (!g_enabled || viewDef == nullptr || viewDef->isSubview)
	{
		g_wasEnabled = false;
		return;
	}

	// Omitted-skin keys only. Do not ingest SnapshotPackedIds dirty==0.
	if (priorSkinnedRecords != nullptr)
	{
		for (const RtSmokeSkinnedSurfaceRecord& rec : *priorSkinnedRecords)
		{
			if (!rec.cpuCaptureOmitted || rec.entityIndex < 0)
			{
				continue;
			}
			++g_omittedResident;
			g_omittedSkinKeys.insert(
				OmittedSkinKey(rec.entityIndex, rec.canonicalInstance.modelSurfaceIndex == UINT32_MAX
					? -1
					: static_cast<int>(rec.canonicalInstance.modelSurfaceIndex)));
		}
	}
}

void SetAcceptedSkinnedBuildLive(bool live)
{
	g_acceptedSkinnedBuildLive = live;
}

void CaptureSnapshot(RtPtCpuProducerApplyGateSnapshot& snapshot)
{
	snapshot.enabled = g_enabled;
	snapshot.acceptedSkinnedBuildLive = g_acceptedSkinnedBuildLive;
	snapshot.omittedSkinKeys.assign(
		g_omittedSkinKeys.begin(), g_omittedSkinKeys.end());
	std::sort(snapshot.omittedSkinKeys.begin(), snapshot.omittedSkinKeys.end());
}

std::size_t SnapshotKeyCount()
{
	return g_omittedSkinKeys.size();
}

bool FillSnapshotPreReserved(
	RtPtCpuProducerApplyGateSnapshot& snapshot,
	std::size_t expectedKeyCount)
{
	if (expectedKeyCount != g_omittedSkinKeys.size() ||
		snapshot.omittedSkinKeys.capacity() < expectedKeyCount)
	{
		return false;
	}
	snapshot.enabled = g_enabled;
	snapshot.acceptedSkinnedBuildLive = g_acceptedSkinnedBuildLive;
	snapshot.omittedSkinKeys.resize(expectedKeyCount);
	std::size_t index = 0;
	for (std::uint64_t key : g_omittedSkinKeys)
	{
		snapshot.omittedSkinKeys[index++] = key;
	}
	std::sort(snapshot.omittedSkinKeys.begin(), snapshot.omittedSkinKeys.end());
	return true;
}

bool ShouldSkipFromPod(
	const RtPtCpuProducerApplyGateSnapshot& snapshot,
	int entityIndex,
	int modelSurfaceIndex)
{
	if (!snapshot.enabled || !snapshot.acceptedSkinnedBuildLive)
	{
		return false;
	}
	const uint64_t key = OmittedSkinKey(entityIndex, modelSurfaceIndex);
	return ShouldSkipFromMembership(
		snapshot.enabled,
		snapshot.acceptedSkinnedBuildLive,
		key != 0 && std::binary_search(
			snapshot.omittedSkinKeys.begin(), snapshot.omittedSkinKeys.end(), key));
}

bool Enabled()
{
	return g_enabled;
}

bool ShouldSkipDrawSurf(const drawSurf_t* drawSurf)
{
	if (!g_enabled)
	{
		return false;
	}
	// Fail closed unless GEO-08 says the accepted skinned BLAS is Live.
	if (!g_acceptedSkinnedBuildLive)
	{
		++g_captureCount;
		return false;
	}
	if (drawSurf == nullptr || drawSurf->space == nullptr ||
		drawSurf->space->entityDef == nullptr)
	{
		++g_captureCount;
		return false;
	}
	const int entityIndex = drawSurf->space->entityDef->index;
	const int surfaceIndex = drawSurf->modelSurfaceIndex;
	const uint64_t key = OmittedSkinKey(entityIndex, surfaceIndex);
	if (ShouldSkipFromMembership(
		g_enabled,
		g_acceptedSkinnedBuildLive,
		key != 0 && g_omittedSkinKeys.find(key) != g_omittedSkinKeys.end()))
	{
		++g_skipCount;
		return true;
	}
	++g_captureCount;
	return false;
}

void NoteCapture()
{
	++g_captureCount;
}

void MaybeLog()
{
	if (!g_enabled)
	{
		g_wasEnabled = false;
		return;
	}
	if (!g_wasEnabled || g_logCooldown <= 0)
	{
		common->Printf(
			"PathTraceCpuProducerApply: skip=%d capture=%d omittedResident=%d\n",
			g_skipCount,
			g_captureCount,
			g_omittedResident);
		g_logCooldown = 60;
		g_wasEnabled = true;
		return;
	}
	--g_logCooldown;
}

} // namespace PtCpuProducerApplyGate
