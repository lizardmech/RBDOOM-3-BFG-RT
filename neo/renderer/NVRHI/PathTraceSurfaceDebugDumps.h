#pragma once

// Surface-targeted diagnostic dumps.
//
// Prints crosshair material and GUI surface details using the already-built
// material table. Trigger/reset policy lives in PathTraceDebugDumps.

#include "PathTraceDynamicMaterialState.h"

struct viewDef_t;
struct RtPathTraceRigidRouteBuild;

void ProcessSmokeCrosshairZeroRoughnessToggle(const viewDef_t* viewDef);
void ProcessSmokeCrosshairFullMetalToggle(const viewDef_t* viewDef);
void LogSmokeCrosshairMaterialDump(
    const viewDef_t* viewDef,
    const RtSmokeMaterialTableBuild& table,
    const std::vector<PathTraceDynamicMaterialRecord>* dynamicRecords,
    const std::vector<uint32_t>* dynamicTriangleMaterialIds,
    const std::vector<uint32_t>* dynamicTriangleMaterialIndexes,
    const std::vector<uint32_t>* staticTriangleMaterialIds,
    const std::vector<uint32_t>* staticTriangleMaterialIndexes,
    const RtPathTraceRigidRouteBuild* rigidRouteBuild);
void LogSmokeGuiSurfaceDump(const viewDef_t* viewDef, const RtSmokeMaterialTableBuild& table);
