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

struct PathTraceSkinnedPreviousPosition
{
    float4 previousPosition;
};

struct PathTraceSmokeEmissiveTriangle
{
    float4 centerAndArea;
    float4 normalAndLuminance;
    float4 uvBounds;
    float4 centroidUvAndWeight;
    float4 estimatedRadianceAndLuminance;
    float4 sampleWeightAndPdf;
    uint materialIndex;
    uint instanceId;
    uint primitiveIndex;
    uint flags;
    uint emissiveTextureIndex;
    uint emissiveTextureWidth;
    uint emissiveTextureHeight;
    uint materialId;
    uint universeMaterialIndex;
    uint identityHashLo;
    uint identityHashHi;
    uint padding0;
};

struct PathTraceUnifiedLightRecord
{
    float4 positionAndRadius;
    float4 normalAndArea;
    float4 radianceAndLuminance;
    float4 uvOrDoomParams;
    uint type;
    uint sourceIndex;
    uint flags;
    uint materialOrLightId;
    uint instanceId;
    uint primitiveIndex;
    uint identityA;
    uint identityB;
    float sourcePdf;
    float sourceWeight;
    uint previousIndex;
    uint padding0;
};

struct PathTraceSkinnedEmissiveGpuWork
{
    uint currentVertexIndex0;
    uint currentVertexIndex1;
    uint currentVertexIndex2;
    uint previousPositionIndex0;
    uint previousPositionIndex1;
    uint previousPositionIndex2;
    uint currentEmissiveIndex;
    uint previousEmissiveIndex;
    uint currentUnifiedIndex;
    uint previousUnifiedIndex;
    uint currentPayloadIndex;
    uint previousPayloadIndex;
    uint workItemCount;
    uint flags;
    uint padding0;
    uint padding1;
};

static const uint PT_SKINNED_EMISSIVE_GPU_WRITE_PREVIOUS =
    1u << 0;
static const uint PT_SKINNED_EMISSIVE_GPU_WRITE_CURRENT_UNIFIED =
    1u << 1;
static const uint PT_SKINNED_EMISSIVE_GPU_WRITE_PREVIOUS_UNIFIED =
    1u << 2;
static const uint PT_SKINNED_EMISSIVE_GPU_WRITE_CURRENT_PAYLOAD =
    1u << 3;
static const uint PT_SKINNED_EMISSIVE_GPU_WRITE_PREVIOUS_PAYLOAD =
    1u << 4;
static const uint PT_SKINNED_EMISSIVE_GPU_PUBLISH_ENABLED =
    1u << 5;

StructuredBuffer<PathTraceSmokeVertex>
    SkinnedCurrentOutputVertices : register(t0);
StructuredBuffer<PathTraceSkinnedPreviousPosition>
    SkinnedPreviousPositions : register(t1);
StructuredBuffer<PathTraceSkinnedEmissiveGpuWork>
    SkinnedEmissiveWork : register(t2);
RWStructuredBuffer<PathTraceSmokeEmissiveTriangle>
    CurrentEmissiveTriangles : register(u0);
RWStructuredBuffer<PathTraceSmokeEmissiveTriangle>
    PreviousEmissiveTriangles : register(u1);
RWStructuredBuffer<PathTraceUnifiedLightRecord>
    CurrentUnifiedLights : register(u2);
RWStructuredBuffer<PathTraceUnifiedLightRecord>
    PreviousUnifiedLights : register(u3);
RWStructuredBuffer<PathTraceUnifiedLightRecord>
    CurrentLightPayloads : register(u4);
RWStructuredBuffer<PathTraceUnifiedLightRecord>
    PreviousLightPayloads : register(u5);

bool UpdateEmissiveGeometry(
    inout PathTraceSmokeEmissiveTriangle record,
    float3 p0,
    float3 p1,
    float3 p2,
    float2 uv0,
    float2 uv1,
    float2 uv2)
{
    const float3 areaVector = cross(p1 - p0, p2 - p0);
    const float doubleArea = length(areaVector);
    if (!isfinite(doubleArea) || doubleArea <= 1.0e-6)
    {
        record.centerAndArea.w = 0.0;
        record.centroidUvAndWeight.z = 0.0;
        record.sampleWeightAndPdf.x = 0.0;
        record.sampleWeightAndPdf.z = 0.0;
        return false;
    }

    const float area = doubleArea * 0.5;
    const float3 normal = areaVector / doubleArea;
    const float luminance =
        max(record.estimatedRadianceAndLuminance.w, 0.0);
    const float sampleWeight = area * luminance;
    record.centerAndArea =
        float4((p0 + p1 + p2) / 3.0, area);
    record.normalAndLuminance =
        float4(normal, luminance);
    record.uvBounds = float4(
        min(uv0.x, min(uv1.x, uv2.x)),
        min(uv0.y, min(uv1.y, uv2.y)),
        max(uv0.x, max(uv1.x, uv2.x)),
        max(uv0.y, max(uv1.y, uv2.y)));
    record.centroidUvAndWeight.xy =
        (uv0 + uv1 + uv2) / 3.0;
    record.centroidUvAndWeight.z = sampleWeight;
    record.sampleWeightAndPdf.x = sampleWeight;
    record.sampleWeightAndPdf.z = area;
    return true;
}

void UpdateUnifiedGeometry(
    inout PathTraceUnifiedLightRecord light,
    PathTraceSmokeEmissiveTriangle emissiveTriangle)
{
    light.positionAndRadius.xyz =
        emissiveTriangle.centerAndArea.xyz;
    light.normalAndArea =
        float4(
            emissiveTriangle.normalAndLuminance.xyz,
            emissiveTriangle.centerAndArea.w);
    light.radianceAndLuminance =
        emissiveTriangle.estimatedRadianceAndLuminance;
    light.uvOrDoomParams = emissiveTriangle.uvBounds;
    light.sourceWeight =
        max(
            emissiveTriangle.sampleWeightAndPdf.x,
            emissiveTriangle.centerAndArea.w > 1.0e-6
                ? 1.0e-6
                : 0.0);
}

void DisableUnifiedGeometry(
    inout PathTraceUnifiedLightRecord light)
{
    light.normalAndArea.w = 0.0;
    light.sourceWeight = 0.0;
}

[numthreads(64, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint workIndex = dispatchThreadId.x;
    const uint workCount =
        SkinnedEmissiveWork[0].workItemCount;
    if (workIndex >= workCount)
    {
        return;
    }

    const PathTraceSkinnedEmissiveGpuWork work =
        SkinnedEmissiveWork[workIndex];
    if ((work.flags &
            PT_SKINNED_EMISSIVE_GPU_PUBLISH_ENABLED) == 0u)
    {
        PathTraceSmokeEmissiveTriangle currentDisabled =
            CurrentEmissiveTriangles[
                work.currentEmissiveIndex];
        currentDisabled.centerAndArea.w = 0.0;
        currentDisabled.centroidUvAndWeight.z = 0.0;
        currentDisabled.sampleWeightAndPdf.x = 0.0;
        currentDisabled.sampleWeightAndPdf.z = 0.0;
        CurrentEmissiveTriangles[
            work.currentEmissiveIndex] = currentDisabled;
        if ((work.flags &
                PT_SKINNED_EMISSIVE_GPU_WRITE_CURRENT_UNIFIED) !=
            0u)
        {
            PathTraceUnifiedLightRecord light =
                CurrentUnifiedLights[
                    work.currentUnifiedIndex];
            DisableUnifiedGeometry(light);
            CurrentUnifiedLights[
                work.currentUnifiedIndex] = light;
        }
        if ((work.flags &
                PT_SKINNED_EMISSIVE_GPU_WRITE_CURRENT_PAYLOAD) !=
            0u)
        {
            PathTraceUnifiedLightRecord light =
                CurrentLightPayloads[
                    work.currentPayloadIndex];
            DisableUnifiedGeometry(light);
            CurrentLightPayloads[
                work.currentPayloadIndex] = light;
        }
        return;
    }

    const PathTraceSmokeVertex current0 =
        SkinnedCurrentOutputVertices[
            work.currentVertexIndex0];
    const PathTraceSmokeVertex current1 =
        SkinnedCurrentOutputVertices[
            work.currentVertexIndex1];
    const PathTraceSmokeVertex current2 =
        SkinnedCurrentOutputVertices[
            work.currentVertexIndex2];
    PathTraceSmokeEmissiveTriangle current =
        CurrentEmissiveTriangles[
            work.currentEmissiveIndex];
    UpdateEmissiveGeometry(
        current,
        current0.position.xyz,
        current1.position.xyz,
        current2.position.xyz,
        current0.texCoord.xy,
        current1.texCoord.xy,
        current2.texCoord.xy);
    CurrentEmissiveTriangles[
        work.currentEmissiveIndex] = current;

    if ((work.flags &
            PT_SKINNED_EMISSIVE_GPU_WRITE_CURRENT_UNIFIED) !=
        0u)
    {
        PathTraceUnifiedLightRecord light =
            CurrentUnifiedLights[
                work.currentUnifiedIndex];
        UpdateUnifiedGeometry(light, current);
        CurrentUnifiedLights[
            work.currentUnifiedIndex] = light;
    }
    if ((work.flags &
            PT_SKINNED_EMISSIVE_GPU_WRITE_CURRENT_PAYLOAD) !=
        0u)
    {
        PathTraceUnifiedLightRecord light =
            CurrentLightPayloads[
                work.currentPayloadIndex];
        UpdateUnifiedGeometry(light, current);
        CurrentLightPayloads[
            work.currentPayloadIndex] = light;
    }

    if ((work.flags &
            PT_SKINNED_EMISSIVE_GPU_WRITE_PREVIOUS) == 0u)
    {
        return;
    }

    const float3 previous0 =
        SkinnedPreviousPositions[
            work.previousPositionIndex0].
                previousPosition.xyz;
    const float3 previous1 =
        SkinnedPreviousPositions[
            work.previousPositionIndex1].
                previousPosition.xyz;
    const float3 previous2 =
        SkinnedPreviousPositions[
            work.previousPositionIndex2].
                previousPosition.xyz;
    PathTraceSmokeEmissiveTriangle previous =
        PreviousEmissiveTriangles[
            work.previousEmissiveIndex];
    UpdateEmissiveGeometry(
        previous,
        previous0,
        previous1,
        previous2,
        current0.texCoord.xy,
        current1.texCoord.xy,
        current2.texCoord.xy);
    PreviousEmissiveTriangles[
        work.previousEmissiveIndex] = previous;

    if ((work.flags &
            PT_SKINNED_EMISSIVE_GPU_WRITE_PREVIOUS_UNIFIED) !=
        0u)
    {
        PathTraceUnifiedLightRecord light =
            PreviousUnifiedLights[
                work.previousUnifiedIndex];
        UpdateUnifiedGeometry(light, previous);
        PreviousUnifiedLights[
            work.previousUnifiedIndex] = light;
    }
    if ((work.flags &
            PT_SKINNED_EMISSIVE_GPU_WRITE_PREVIOUS_PAYLOAD) !=
        0u)
    {
        PathTraceUnifiedLightRecord light =
            PreviousLightPayloads[
                work.previousPayloadIndex];
        UpdateUnifiedGeometry(light, previous);
        PreviousLightPayloads[
            work.previousPayloadIndex] = light;
    }
}
