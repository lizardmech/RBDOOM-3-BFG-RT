#pragma once

#include <cstdint>
#include <vector>

struct viewDef_t;
struct drawSurf_t;
struct RtSmokeSkinnedSurfaceRecord;

struct RtPtCpuProducerApplyGateSnapshot
{
	std::vector<std::uint64_t> omittedSkinKeys;
	bool enabled = false;
	bool acceptedSkinnedBuildLive = false;

	void Invalidate()
	{
		omittedSkinKeys.clear();
		enabled = false;
		acceptedSkinnedBuildLive = false;
	}

	std::size_t OwnedBytes() const
	{
		return sizeof(*this) + omittedSkinKeys.capacity() * sizeof(std::uint64_t);
	}
};

// In-engine skip of omitted-skin rediscovery only. Default off.
// Never skip rigid/static/world because lifecycle dirty==0.
// Fail closed = capture.
namespace PtCpuProducerApplyGate
{
	inline bool ShouldSkipFromMembership(
		bool enabled, bool acceptedSkinnedBuildLive, bool omittedKeyPresent)
	{
		return enabled && acceptedSkinnedBuildLive && omittedKeyPresent;
	}

	void BeginFrame(
		const viewDef_t* viewDef,
		const std::vector<RtSmokeSkinnedSurfaceRecord>* priorSkinnedRecords);
	void SetAcceptedSkinnedBuildLive(bool live);
	void CaptureSnapshot(RtPtCpuProducerApplyGateSnapshot& snapshot);
	std::size_t SnapshotKeyCount();
	bool FillSnapshotPreReserved(
		RtPtCpuProducerApplyGateSnapshot& snapshot,
		std::size_t expectedKeyCount);
	bool ShouldSkipFromPod(
		const RtPtCpuProducerApplyGateSnapshot& snapshot,
		int entityIndex,
		int modelSurfaceIndex);
	bool ShouldSkipDrawSurf(const drawSurf_t* drawSurf);
	void NoteCapture();
	void MaybeLog();
	bool Enabled();
}
