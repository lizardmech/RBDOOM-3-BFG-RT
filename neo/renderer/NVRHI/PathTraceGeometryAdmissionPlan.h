#pragma once

#include <cstdint>

enum RtSmokeGeometryAdmissionResult : uint32_t
{
    RT_SMOKE_GEOMETRY_ADMISSION_ADMITTED = 0,
    RT_SMOKE_GEOMETRY_ADMISSION_REJECT_SURFACE_BUDGET,
    RT_SMOKE_GEOMETRY_ADMISSION_REJECT_BYTE_BUDGET,
    RT_SMOKE_GEOMETRY_ADMISSION_REJECT_INVALID_COUNT,
    RT_SMOKE_GEOMETRY_ADMISSION_REJECT_ARITHMETIC_OVERFLOW
};

struct RtSmokeGeometryAdmissionBudget
{
    // Zero means unlimited. Finite production values require runtime
    // measurement; the planner does not invent a replacement hard cap.
    uint64_t maxBytes = 0;
    uint64_t maxSurfaces = 0;
};

struct RtSmokeGeometryAdmissionInput
{
    uint64_t currentBytes = 0;
    uint64_t currentSurfaces = 0;
    int64_t candidateVertexCount = 0;
    int64_t candidateIndexCount = 0;
    uint64_t candidateSurfaceCount = 1;
    uint64_t vertexStride = 0;
    uint64_t indexStride = 0;
    // Combined bytes stored once per triangle at this capture boundary.
    uint64_t triangleMetadataStride = 0;
};

struct RtSmokeGeometryAdmissionPlan
{
    RtSmokeGeometryAdmissionResult result =
        RT_SMOKE_GEOMETRY_ADMISSION_REJECT_INVALID_COUNT;
    uint64_t candidateTriangles = 0;
    uint64_t candidateBytes = 0;
    uint64_t totalBytes = 0;
    uint64_t totalSurfaces = 0;

    bool Admitted() const
    {
        return result == RT_SMOKE_GEOMETRY_ADMISSION_ADMITTED;
    }
};

RtSmokeGeometryAdmissionPlan BuildSmokeGeometryAdmissionPlan(
    const RtSmokeGeometryAdmissionBudget& budget,
    const RtSmokeGeometryAdmissionInput& input);
