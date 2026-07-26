struct PathTraceSmokeVertex
{
    float4 position;
    float4 normal;
    float4 texCoord;
    float4 color;
    float4 color2;
    float4 tangent;
    float4 bitangent;
};

struct PathTraceSkinnedSourceVertex
{
    float4 localPosition;
    float4 localNormal;
    float4 localTangent;
    float4 texCoord;
    float4 color;
    uint4 jointIndices;
    float4 jointWeights;
};

struct PathTraceSkinnedJointMatrix
{
    float4 row0;
    float4 row1;
    float4 row2;
};

struct PathTraceSkinnedPreviousPosition
{
    float4 previousPosition;
};

static const uint PT_SKINNED_DISPATCH_HAS_VALID_PREVIOUS = 0x00000001u;
static const uint PT_SKINNED_DISPATCH_HAS_PREVIOUS_JOINTS = 0x00000010u;
static const uint PT_SKINNED_DISPATCH_HAS_TEX_MATRIX = 0x00000020u;
static const uint PT_SKINNED_INVALID_OFFSET = 0xffffffffu;

struct PathTraceSkinnedSurfaceDispatchRecord
{
    uint sourceVertexOffset;
    uint outputVertexOffset;
    uint previousPositionOffset;
    uint vertexCount;
    uint currentJointOffset;
    uint previousJointOffset;
    uint surfaceRecordIndex;
    uint flags;
    uint dynamicVertexOffset;
    uint dynamicIndexOffset;
    uint dynamicTriangleOffset;
    uint triangleCount;
    float4 texMatrix0;
    float4 texMatrix1;
    float4 currentObjectToWorld0;
    float4 currentObjectToWorld1;
    float4 currentObjectToWorld2;
    float4 previousObjectToWorld0;
    float4 previousObjectToWorld1;
    float4 previousObjectToWorld2;
};

StructuredBuffer<PathTraceSkinnedSourceVertex> SkinnedSourceVertices : register(t0);
RWStructuredBuffer<PathTraceSmokeVertex> SkinnedCurrentOutputVertices : register(u0);
RWStructuredBuffer<PathTraceSkinnedPreviousPosition> SkinnedPreviousPositions : register(u1);
StructuredBuffer<PathTraceSkinnedSurfaceDispatchRecord> SkinnedSurfaceDispatch : register(t1);
StructuredBuffer<PathTraceSkinnedJointMatrix> SkinnedCurrentJointMatrices : register(t2);
StructuredBuffer<PathTraceSkinnedJointMatrix> SkinnedPreviousJointMatrices : register(t3);

float3 TransformJointPosition(PathTraceSkinnedJointMatrix jointMatrix, float4 localPosition)
{
    return float3(
        dot(jointMatrix.row0, localPosition),
        dot(jointMatrix.row1, localPosition),
        dot(jointMatrix.row2, localPosition));
}

float3 TransformJointNormal(PathTraceSkinnedJointMatrix jointMatrix, float3 localNormal)
{
    return float3(
        dot(jointMatrix.row0.xyz, localNormal),
        dot(jointMatrix.row1.xyz, localNormal),
        dot(jointMatrix.row2.xyz, localNormal));
}

float3 SafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-8 ? value * rsqrt(lengthSquared) : fallback;
}

float3 TransformObjectPosition(float4 row0, float4 row1, float4 row2, float3 localPosition)
{
    const float4 local = float4(localPosition, 1.0);
    return float3(dot(row0, local), dot(row1, local), dot(row2, local));
}

float3 TransformObjectNormal(float4 row0, float4 row1, float4 row2, float3 localNormal)
{
    return float3(
        dot(row0.xyz, localNormal),
        dot(row1.xyz, localNormal),
        dot(row2.xyz, localNormal));
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint vertexIndex = dispatchThreadId.x;
    const uint recordIndex = dispatchThreadId.y;
    const PathTraceSkinnedSurfaceDispatchRecord dispatchRecord = SkinnedSurfaceDispatch[recordIndex];
    if (vertexIndex >= dispatchRecord.vertexCount || dispatchRecord.currentJointOffset == PT_SKINNED_INVALID_OFFSET)
    {
        return;
    }

    const PathTraceSkinnedSourceVertex sourceVertex = SkinnedSourceVertices[dispatchRecord.sourceVertexOffset + vertexIndex];
    const uint4 jointIndices = sourceVertex.jointIndices;
    const float4 jointWeights = sourceVertex.jointWeights;

    const PathTraceSkinnedJointMatrix joint0 = SkinnedCurrentJointMatrices[dispatchRecord.currentJointOffset + jointIndices.x];
    const PathTraceSkinnedJointMatrix joint1 = SkinnedCurrentJointMatrices[dispatchRecord.currentJointOffset + jointIndices.y];
    const PathTraceSkinnedJointMatrix joint2 = SkinnedCurrentJointMatrices[dispatchRecord.currentJointOffset + jointIndices.z];
    const PathTraceSkinnedJointMatrix joint3 = SkinnedCurrentJointMatrices[dispatchRecord.currentJointOffset + jointIndices.w];

    float3 skinnedPosition =
        TransformJointPosition(joint0, sourceVertex.localPosition) * jointWeights.x +
        TransformJointPosition(joint1, sourceVertex.localPosition) * jointWeights.y +
        TransformJointPosition(joint2, sourceVertex.localPosition) * jointWeights.z +
        TransformJointPosition(joint3, sourceVertex.localPosition) * jointWeights.w;

    float3 skinnedNormal =
        TransformJointNormal(joint0, sourceVertex.localNormal.xyz) * jointWeights.x +
        TransformJointNormal(joint1, sourceVertex.localNormal.xyz) * jointWeights.y +
        TransformJointNormal(joint2, sourceVertex.localNormal.xyz) * jointWeights.z +
        TransformJointNormal(joint3, sourceVertex.localNormal.xyz) * jointWeights.w;
    skinnedNormal = SafeNormalize(skinnedNormal, sourceVertex.localNormal.xyz);
    float3 skinnedTangent =
        TransformJointNormal(joint0, sourceVertex.localTangent.xyz) * jointWeights.x +
        TransformJointNormal(joint1, sourceVertex.localTangent.xyz) * jointWeights.y +
        TransformJointNormal(joint2, sourceVertex.localTangent.xyz) * jointWeights.z +
        TransformJointNormal(joint3, sourceVertex.localTangent.xyz) * jointWeights.w;
    skinnedTangent = SafeNormalize(skinnedTangent, sourceVertex.localTangent.xyz);
    const float3 localBitangent = cross(sourceVertex.localNormal.xyz, sourceVertex.localTangent.xyz) * sourceVertex.localTangent.w;
    float3 skinnedBitangent =
        TransformJointNormal(joint0, localBitangent) * jointWeights.x +
        TransformJointNormal(joint1, localBitangent) * jointWeights.y +
        TransformJointNormal(joint2, localBitangent) * jointWeights.z +
        TransformJointNormal(joint3, localBitangent) * jointWeights.w;
    skinnedBitangent = SafeNormalize(skinnedBitangent, localBitangent);

    const float3 worldPosition = TransformObjectPosition(
        dispatchRecord.currentObjectToWorld0,
        dispatchRecord.currentObjectToWorld1,
        dispatchRecord.currentObjectToWorld2,
        skinnedPosition);
    const float3 worldNormal = SafeNormalize(TransformObjectNormal(
        dispatchRecord.currentObjectToWorld0,
        dispatchRecord.currentObjectToWorld1,
        dispatchRecord.currentObjectToWorld2,
        skinnedNormal), skinnedNormal);
    const float3 worldTangent = SafeNormalize(TransformObjectNormal(
        dispatchRecord.currentObjectToWorld0,
        dispatchRecord.currentObjectToWorld1,
        dispatchRecord.currentObjectToWorld2,
        skinnedTangent), skinnedTangent);
    const float3 worldBitangent = SafeNormalize(TransformObjectNormal(
        dispatchRecord.currentObjectToWorld0,
        dispatchRecord.currentObjectToWorld1,
        dispatchRecord.currentObjectToWorld2,
        skinnedBitangent), skinnedBitangent);

    PathTraceSmokeVertex outputVertex;
    outputVertex.position = float4(worldPosition, 1.0);
    outputVertex.normal = float4(worldNormal, 0.0);
    outputVertex.texCoord = sourceVertex.texCoord;
    if ((dispatchRecord.flags & PT_SKINNED_DISPATCH_HAS_TEX_MATRIX) != 0u)
    {
        outputVertex.texCoord.xy = float2(
            dot(dispatchRecord.texMatrix0.xyz, float3(sourceVertex.texCoord.xy, 1.0)),
            dot(dispatchRecord.texMatrix1.xyz, float3(sourceVertex.texCoord.xy, 1.0)));
    }
    outputVertex.color = sourceVertex.color;
    outputVertex.color2 = float4(sourceVertex.jointWeights.xyz, sourceVertex.jointWeights.w);
    outputVertex.tangent = float4(worldTangent, sourceVertex.localTangent.w);
    outputVertex.bitangent = float4(worldBitangent, 0.0);
    SkinnedCurrentOutputVertices[dispatchRecord.outputVertexOffset + vertexIndex] = outputVertex;

    const bool hasPreviousPositionOutput =
        dispatchRecord.previousPositionOffset != PT_SKINNED_INVALID_OFFSET &&
        dispatchRecord.previousJointOffset != PT_SKINNED_INVALID_OFFSET &&
        (dispatchRecord.flags & PT_SKINNED_DISPATCH_HAS_VALID_PREVIOUS) != 0u &&
        (dispatchRecord.flags & PT_SKINNED_DISPATCH_HAS_PREVIOUS_JOINTS) != 0u;
    if (!hasPreviousPositionOutput)
    {
        return;
    }

    const PathTraceSkinnedJointMatrix previousJoint0 = SkinnedPreviousJointMatrices[dispatchRecord.previousJointOffset + jointIndices.x];
    const PathTraceSkinnedJointMatrix previousJoint1 = SkinnedPreviousJointMatrices[dispatchRecord.previousJointOffset + jointIndices.y];
    const PathTraceSkinnedJointMatrix previousJoint2 = SkinnedPreviousJointMatrices[dispatchRecord.previousJointOffset + jointIndices.z];
    const PathTraceSkinnedJointMatrix previousJoint3 = SkinnedPreviousJointMatrices[dispatchRecord.previousJointOffset + jointIndices.w];

    const float3 previousSkinnedPosition =
        TransformJointPosition(previousJoint0, sourceVertex.localPosition) * jointWeights.x +
        TransformJointPosition(previousJoint1, sourceVertex.localPosition) * jointWeights.y +
        TransformJointPosition(previousJoint2, sourceVertex.localPosition) * jointWeights.z +
        TransformJointPosition(previousJoint3, sourceVertex.localPosition) * jointWeights.w;
    const float3 previousWorldPosition = TransformObjectPosition(
        dispatchRecord.previousObjectToWorld0,
        dispatchRecord.previousObjectToWorld1,
        dispatchRecord.previousObjectToWorld2,
        previousSkinnedPosition);

    PathTraceSkinnedPreviousPosition previousOutput;
    previousOutput.previousPosition = float4(previousWorldPosition, 1.0);
    SkinnedPreviousPositions[dispatchRecord.previousPositionOffset + vertexIndex] = previousOutput;
}
