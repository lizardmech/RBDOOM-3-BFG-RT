#pragma once

// Shared GPU-facing geometry structs and pure geometry helpers.
//
// Keep these definitions small and layout-conscious: PathTraceSmokeVertex is
// consumed by NVRHI buffers and shader code, while the helpers are common math
// utilities used by capture, acceleration, and emissive inventory code.

struct PathTraceSmokeVertex
{
    float position[4];
    float normal[4];
    float texCoord[4];
    float color[4];
    float color2[4];
    float tangent[4];
    float bitangent[4];
};
static_assert(sizeof(PathTraceSmokeVertex) == 112,
    "PathTraceSmokeVertex HLSL ABI mismatch");

// Source data for the future PT-owned GPU skinning path. This deliberately
// stays separate from PathTraceSmokeVertex, which is the current rendered
// world-space output format used by BLAS builds and hit shading.
struct PathTraceSkinnedSourceVertex
{
    float localPosition[4];
    float localNormal[4];
    float localTangent[4];
    float texCoord[4];
    float color[4];
    uint32_t jointIndices[4];
    float jointWeights[4];
};

struct PathTraceSkinnedPreviousPosition
{
    float previousPosition[4];
};

struct PathTraceSkinnedJointMatrix
{
    float rows[12];
};

enum PathTraceSkinnedSurfaceDispatchFlags : uint32_t
{
    PT_SKINNED_DISPATCH_HAS_VALID_PREVIOUS = 1u << 0,
    PT_SKINNED_DISPATCH_RT_CPU_SKINNED = 1u << 1,
    PT_SKINNED_DISPATCH_SOURCE_READY = 1u << 2,
    PT_SKINNED_DISPATCH_HAS_CURRENT_JOINTS = 1u << 3,
    PT_SKINNED_DISPATCH_HAS_PREVIOUS_JOINTS = 1u << 4,
    PT_SKINNED_DISPATCH_HAS_TEX_MATRIX = 1u << 5
};

struct PathTraceSkinnedSurfaceDispatchRecord
{
    uint32_t sourceVertexOffset = 0;
    uint32_t outputVertexOffset = 0;
    uint32_t previousPositionOffset = 0;
    uint32_t vertexCount = 0;
    uint32_t currentJointOffset = 0;
    uint32_t previousJointOffset = 0;
    uint32_t surfaceRecordIndex = 0;
    uint32_t flags = 0;
    uint32_t dynamicVertexOffset = 0;
    uint32_t dynamicIndexOffset = 0;
    uint32_t dynamicTriangleOffset = 0;
    uint32_t triangleCount = 0;
    float texMatrix0[4];
    float texMatrix1[4];
    float currentObjectToWorld[12];
    float previousObjectToWorld[12];
};
static_assert(
    sizeof(PathTraceSkinnedSurfaceDispatchRecord) == 176,
    "PathTraceSkinnedSurfaceDispatchRecord HLSL ABI mismatch");

enum PathTraceRigidRouteInstanceFlags : uint32_t
{
    PT_RIGID_ROUTE_HAS_PREVIOUS_TRANSFORM = 1u << 0,
    PT_RIGID_ROUTE_TRANSFORM_CONTINUOUS = 1u << 1,
    PT_RIGID_ROUTE_CACHED_SOURCE = 1u << 2
};

struct PathTraceRigidRouteInstance
{
    uint32_t vertexOffset = 0;
    uint32_t indexOffset = 0;
    uint32_t triangleOffset = 0;
    uint32_t materialId = 0;
    uint32_t materialIndex = 0;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t triangleCount = 0;
    uint32_t flags = 0;
    uint32_t instanceIdLo = 0;
    uint32_t instanceIdHi = 0;
    uint32_t padding0 = 0;
    float currentObjectToWorld[12];
    float previousObjectToWorld[12];
};
static_assert(sizeof(PathTraceRigidRouteInstance) == 144,
    "PathTraceRigidRouteInstance HLSL ABI mismatch");

bool SmokeFloatIsFinite(float value);
bool SmokeVec2IsFinite(const idVec2& value);
bool SmokeVec3IsFinite(const idVec3& value);
idVec3 SmokeVertexPosition(const PathTraceSmokeVertex& vertex);
idVec3 SmokeVertexNormal(const PathTraceSmokeVertex& vertex);
idVec2 SmokeVertexTexCoord(const PathTraceSmokeVertex& vertex);
bool SmokeNormalIsUsable(const idVec3& normal);
bool SmokeTexCoordIsUsable(const idVec2& texCoord);
bool IsZeroAreaSmokeTriangle(const idVec3& p0, const idVec3& p1, const idVec3& p2);
bool IntersectRayTriangle(const idVec3& rayOrigin, const idVec3& rayDirection, const idVec3& p0, const idVec3& p1, const idVec3& p2, float& hitDistance);
