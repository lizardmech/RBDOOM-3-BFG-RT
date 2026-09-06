#pragma once

#include "PathTraceSceneCapture.h"

#include <vector>

struct viewDef_t;

// Phase C: when r_pathTracingCpuProducerPack is non-empty, capture the next
// 3 primary PT Build Scene frames into a payload pack (schema 3) and clear.
namespace PtCpuProducerPacker
{
	void MaybeCaptureFrame(
		const viewDef_t* viewDef,
		const RtSmokeSceneCaptureTiming& captureTiming,
		unsigned long long buildSceneUs,
		const std::vector<RtSmokeSkinnedSurfaceRecord>* skinnedSurfaceRecords);
}