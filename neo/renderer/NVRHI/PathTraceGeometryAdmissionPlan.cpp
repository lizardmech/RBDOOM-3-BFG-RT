#include "PathTraceGeometryAdmissionPlan.h"

#include <limits>

namespace
{

bool CheckedAddUint64(uint64_t lhs, uint64_t rhs, uint64_t& result)
{
    if (rhs > std::numeric_limits<uint64_t>::max() - lhs)
    {
        return false;
    }
    result = lhs + rhs;
    return true;
}

bool CheckedMultiplyUint64(uint64_t lhs, uint64_t rhs, uint64_t& result)
{
    if (lhs != 0 && rhs > std::numeric_limits<uint64_t>::max() / lhs)
    {
        return false;
    }
    result = lhs * rhs;
    return true;
}

} // namespace

RtSmokeGeometryAdmissionPlan BuildSmokeGeometryAdmissionPlan(
    const RtSmokeGeometryAdmissionBudget& budget,
    const RtSmokeGeometryAdmissionInput& input)
{
    RtSmokeGeometryAdmissionPlan plan;
    if (input.candidateVertexCount < 0 ||
        input.candidateIndexCount < 0 ||
        (input.candidateIndexCount % 3) != 0 ||
        input.vertexStride == 0 ||
        input.indexStride == 0)
    {
        plan.result = RT_SMOKE_GEOMETRY_ADMISSION_REJECT_INVALID_COUNT;
        return plan;
    }

    const uint64_t vertexCount =
        static_cast<uint64_t>(input.candidateVertexCount);
    const uint64_t indexCount =
        static_cast<uint64_t>(input.candidateIndexCount);
    plan.candidateTriangles = indexCount / 3;

    uint64_t vertexBytes = 0;
    uint64_t indexBytes = 0;
    uint64_t triangleBytes = 0;
    uint64_t candidateGeometryBytes = 0;
    if (!CheckedMultiplyUint64(
            vertexCount, input.vertexStride, vertexBytes) ||
        !CheckedMultiplyUint64(
            indexCount, input.indexStride, indexBytes) ||
        !CheckedMultiplyUint64(
            plan.candidateTriangles,
            input.triangleMetadataStride,
            triangleBytes) ||
        !CheckedAddUint64(
            vertexBytes, indexBytes, candidateGeometryBytes) ||
        !CheckedAddUint64(
            candidateGeometryBytes,
            triangleBytes,
            plan.candidateBytes) ||
        !CheckedAddUint64(
            input.currentBytes,
            plan.candidateBytes,
            plan.totalBytes) ||
        !CheckedAddUint64(
            input.currentSurfaces,
            input.candidateSurfaceCount,
            plan.totalSurfaces))
    {
        plan.result =
            RT_SMOKE_GEOMETRY_ADMISSION_REJECT_ARITHMETIC_OVERFLOW;
        return plan;
    }

    if (budget.maxSurfaces != 0 &&
        plan.totalSurfaces > budget.maxSurfaces)
    {
        plan.result =
            RT_SMOKE_GEOMETRY_ADMISSION_REJECT_SURFACE_BUDGET;
        return plan;
    }
    if (budget.maxBytes != 0 &&
        plan.totalBytes > budget.maxBytes)
    {
        plan.result =
            RT_SMOKE_GEOMETRY_ADMISSION_REJECT_BYTE_BUDGET;
        return plan;
    }

    plan.result = RT_SMOKE_GEOMETRY_ADMISSION_ADMITTED;
    return plan;
}
