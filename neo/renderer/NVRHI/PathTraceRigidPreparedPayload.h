#pragma once

#include "PathTraceGeometry.h"
#include "../../idlib/geometry/DrawVert.h"

#include <cstddef>
#include <cstdint>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <vector>

constexpr std::size_t RT_PT_RIGID_PREPARED_NAME_BYTES = 1024u;

// Worker-owned, pointer-free rigid CPU-cache publication. The vectors own the
// only variable-sized state; live renderer pointers and GPU handles are never
// allowed across this boundary.
struct RtPathTraceRigidPreparedPayload
{
    std::uint32_t surfaceOrdinal = 0;
    std::uint32_t occurrenceCount = 1;
    std::uint64_t meshHash = 0;
    std::uint64_t instanceId = 0;
    std::uintptr_t vertexBufferIdentity = 0;
    std::uintptr_t indexBufferIdentity = 0;
    std::uint32_t sourceFlags = 0;
    std::uint32_t materialId = 0;
    std::uint32_t materialClassSignature = 0;
    std::uint32_t surfaceClassId = 0;
    std::uint32_t triangleClassAndFlags = 0;
    std::uint32_t vertexFormat = 0;
    std::int32_t drawSurfIndex = -1;
    std::int32_t entityIndex = -1;
    std::int32_t renderEntityNum = -1;
    std::int32_t modelSurfaceIndex = -1;
    std::uint32_t modelEpoch = 0;
    std::int32_t jointIndex = -1;
    std::uint32_t fullTriangleVertexCount = 0;
    std::uint32_t fullTriangleIndexCount = 0;
    float normalTexMatrix[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f };
    float triangleBoundsMin[3] = {};
    float triangleBoundsMax[3] = {};
    char materialName[RT_PT_RIGID_PREPARED_NAME_BYTES] = {};
    char modelName[RT_PT_RIGID_PREPARED_NAME_BYTES] = {};
    std::vector<PathTraceSmokeVertex> localVertices;
    std::vector<std::uint32_t> localIndexes;
    std::uint64_t contentSignature = 0;
};

inline std::uint64_t RtPathTraceRigidPreparedHashBytes(
    std::uint64_t hash, const void* bytes, std::size_t count) noexcept
{
    const auto* source = static_cast<const std::uint8_t*>(bytes);
    for (std::size_t index = 0; index < count; ++index)
    {
        hash ^= source[index];
        hash *= 1099511628211ull;
    }
    return hash;
}

struct RtPathTraceRigidOwnedCpuCache
{
    std::vector<PathTraceSmokeVertex> vertices;
    std::vector<std::uint32_t> indexes;
    float boundsMin[3] = {};
    float boundsMax[3] = {};
    std::uint64_t contentSignature = 0;
};

inline bool RtPathTraceBuildRigidOwnedCpuCache(
    const idDrawVert* drawVertices, std::size_t vertexCount,
    const triIndex_t* drawIndexes, std::size_t indexCount,
    const float normalTexMatrix[6],
    RtPathTraceRigidOwnedCpuCache& output) noexcept
{
    output = RtPathTraceRigidOwnedCpuCache();
    if (drawVertices == nullptr || drawIndexes == nullptr ||
        normalTexMatrix == nullptr || vertexCount == 0 || indexCount == 0 ||
        (indexCount % 3u) != 0u || vertexCount > INT32_MAX ||
        indexCount > INT32_MAX)
        return false;
    try
    {
        output.vertices.resize(vertexCount);
        for (int axis = 0; axis < 3; ++axis)
        {
            output.boundsMin[axis] = FLT_MAX;
            output.boundsMax[axis] = -FLT_MAX;
        }
        for (std::size_t vertexIndex = 0; vertexIndex < vertexCount;
             ++vertexIndex)
        {
            const idDrawVert& drawVert = drawVertices[vertexIndex];
            idVec3 normal = drawVert.GetNormal();
            if (normal.Normalize() == 0.0f) normal.Set(0.0f, 0.0f, 1.0f);
            idVec3 tangent = drawVert.GetTangent();
            if (tangent.Normalize() == 0.0f) tangent.Set(1.0f, 0.0f, 0.0f);
            const float sign = drawVert.GetBiTangentSign();
            idVec3 bitangent = drawVert.GetBiTangent();
            if (bitangent.Normalize() == 0.0f)
            {
                bitangent.Cross(normal, tangent);
                bitangent *= sign;
                bitangent.Normalize();
            }
            const idVec2 texCoord = drawVert.GetTexCoord();
            PathTraceSmokeVertex vertex = {};
            vertex.position[0] = drawVert.xyz.x;
            vertex.position[1] = drawVert.xyz.y;
            vertex.position[2] = drawVert.xyz.z;
            vertex.position[3] = 1.0f;
            vertex.normal[0] = normal.x;
            vertex.normal[1] = normal.y;
            vertex.normal[2] = normal.z;
            vertex.normal[3] = 0.0f;
            vertex.texCoord[0] = texCoord.x;
            vertex.texCoord[1] = texCoord.y;
            vertex.texCoord[2] = normalTexMatrix[0] * texCoord.x +
                normalTexMatrix[1] * texCoord.y + normalTexMatrix[2];
            vertex.texCoord[3] = normalTexMatrix[3] * texCoord.x +
                normalTexMatrix[4] * texCoord.y + normalTexMatrix[5];
            for (int component = 0; component < 4; ++component)
            {
                vertex.color[component] = drawVert.color[component] *
                    (1.0f / 255.0f);
                vertex.color2[component] = drawVert.color2[component] *
                    (1.0f / 255.0f);
            }
            vertex.tangent[0] = tangent.x;
            vertex.tangent[1] = tangent.y;
            vertex.tangent[2] = tangent.z;
            vertex.tangent[3] = sign;
            vertex.bitangent[0] = bitangent.x;
            vertex.bitangent[1] = bitangent.y;
            vertex.bitangent[2] = bitangent.z;
            vertex.bitangent[3] = 0.0f;
            for (int axis = 0; axis < 3; ++axis)
            {
                const float value = vertex.position[axis];
                if (!std::isfinite(value) || std::fabs(value) >= 100000.0f)
                    return false;
                if (value < output.boundsMin[axis]) output.boundsMin[axis] = value;
                if (value > output.boundsMax[axis]) output.boundsMax[axis] = value;
            }
            output.vertices[vertexIndex] = vertex;
        }
        output.indexes.resize(indexCount);
        for (std::size_t indexIndex = 0; indexIndex < indexCount; ++indexIndex)
        {
            const int sourceIndex = static_cast<int>(drawIndexes[indexIndex]);
            if (sourceIndex < 0 || static_cast<std::size_t>(sourceIndex) >=
                    vertexCount)
                return false;
            output.indexes[indexIndex] = static_cast<std::uint32_t>(sourceIndex);
        }
        std::uint64_t signature = 14695981039346656037ull;
        signature = RtPathTraceRigidPreparedHashBytes(signature,
            output.vertices.data(), output.vertices.size() *
                sizeof(output.vertices[0]));
        signature = RtPathTraceRigidPreparedHashBytes(signature,
            output.indexes.data(), output.indexes.size() *
                sizeof(output.indexes[0]));
        output.contentSignature = signature != 0 ? signature : 1;
        return true;
    }
    catch (...) { output = RtPathTraceRigidOwnedCpuCache(); return false; }
}

inline bool RtPathTraceValidateRigidPreparedPayload(
    const RtPathTraceRigidPreparedPayload& payload) noexcept
{
    if (payload.localVertices.empty() || payload.localIndexes.empty() ||
        payload.localVertices.size() != payload.fullTriangleVertexCount ||
        payload.localIndexes.size() != payload.fullTriangleIndexCount ||
        (payload.localIndexes.size() % 3u) != 0u)
        return false;
    float boundsMin[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
    float boundsMax[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (const PathTraceSmokeVertex& vertex : payload.localVertices)
        for (int axis = 0; axis < 3; ++axis)
        {
            const float value = vertex.position[axis];
            if (!std::isfinite(value) || std::fabs(value) >= 100000.0f)
                return false;
            if (value < boundsMin[axis]) boundsMin[axis] = value;
            if (value > boundsMax[axis]) boundsMax[axis] = value;
        }
    for (std::uint32_t index : payload.localIndexes)
        if (index >= payload.localVertices.size()) return false;
    std::uint64_t signature = 14695981039346656037ull;
    signature = RtPathTraceRigidPreparedHashBytes(signature,
        payload.localVertices.data(), payload.localVertices.size() *
            sizeof(payload.localVertices[0]));
    signature = RtPathTraceRigidPreparedHashBytes(signature,
        payload.localIndexes.data(), payload.localIndexes.size() *
            sizeof(payload.localIndexes[0]));
    if (signature == 0) signature = 1;
    return signature == payload.contentSignature &&
        std::memcmp(boundsMin, payload.triangleBoundsMin, sizeof(boundsMin)) == 0 &&
        std::memcmp(boundsMax, payload.triangleBoundsMax, sizeof(boundsMax)) == 0;
}
