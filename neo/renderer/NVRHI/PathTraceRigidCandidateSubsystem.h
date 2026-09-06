#pragma once

#include "PathTraceGeometryUniverse.h"

uint32_t RtPathTraceBuildRigidMeshCandidateRejectFlags(
    const RtPathTraceRigidMeshCandidateObservation& observation) noexcept;
void RtPathTraceAccumulateRigidMeshCandidateRejectStats(
    RtPathTraceRigidMeshCandidateStats& stats, uint32_t rejectFlags) noexcept;
void RtPathTraceAddRigidMeshCandidateSampleToStats(
    RtPathTraceRigidMeshCandidateStats& stats,
    const RtPathTraceRigidMeshCandidateObservation& observation,
    bool eligible, uint32_t rejectFlags, int seenCount);
bool RtPathTraceRigidMeshHasCachedRouteData(
    const RtSmokeGeometryUniverse::RigidMeshCandidateRecord& record) noexcept;
bool RtPathTraceRefreshRigidMeshCandidateCpuCache(
    RtSmokeGeometryUniverse::RigidMeshCandidateRecord& record);
