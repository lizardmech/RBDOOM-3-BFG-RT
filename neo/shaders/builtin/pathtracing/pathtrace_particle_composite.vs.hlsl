struct ParticleCompositeVertex
{
    float3 worldPosition;
    float2 texCoord;
    uint packedColor;
    uint particleMetadata;
    uint4 lightingInfo;
};

struct ParticleCompositeConstants
{
    float4 cameraOriginAndTanX;
    float4 cameraForwardAndTanY;
    float4 cameraLeftAndAmbient;
    float4 cameraUpAndEmissiveScale;
    float4 outputAndRenderSize;
    float4 batchInfo;
    float4 modelInfo;
};

#ifdef SPIRV
[[vk::push_constant]] ConstantBuffer<ParticleCompositeConstants> ParticleConstants;
#else
ConstantBuffer<ParticleCompositeConstants> ParticleConstants : register(b0);
#endif

StructuredBuffer<ParticleCompositeVertex> ParticleVertices : register(t0);
StructuredBuffer<float4> ParticleLighting : register(t3);

struct VS_OUT
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
    float4 color : COLOR0;
    float viewDepth : TEXCOORD1;
    nointerpolation uint particleMetadata : TEXCOORD2;
    nointerpolation float3 lighting : TEXCOORD3;
};

float4 UnpackParticleColor(uint packedColor)
{
    return float4(
        float(packedColor & 0xffu),
        float((packedColor >> 8u) & 0xffu),
        float((packedColor >> 16u) & 0xffu),
        float((packedColor >> 24u) & 0xffu)) * (1.0 / 255.0);
}

VS_OUT main(uint vertexId : SV_VertexID)
{
    ParticleCompositeVertex vertex = ParticleVertices[vertexId];
    const float3 relative = vertex.worldPosition - ParticleConstants.cameraOriginAndTanX.xyz;
    const float viewDepth = dot(relative, ParticleConstants.cameraForwardAndTanY.xyz);

    VS_OUT result;
    result.position = float4(
        -dot(relative, ParticleConstants.cameraLeftAndAmbient.xyz) / max(ParticleConstants.cameraOriginAndTanX.w, 1.0e-5),
        dot(relative, ParticleConstants.cameraUpAndEmissiveScale.xyz) / max(ParticleConstants.cameraForwardAndTanY.w, 1.0e-5),
        0.0,
        viewDepth);
    result.texCoord = vertex.texCoord;
    result.color = UnpackParticleColor(vertex.packedColor);
    result.viewDepth = viewDepth;
    result.particleMetadata = vertex.particleMetadata;
    const bool forceLightingDebug = ParticleConstants.modelInfo.w > 1.5;
    const bool lightingEnabled = ParticleConstants.modelInfo.w > 0.5 && vertex.lightingInfo.x != 0xffffffffu;
    result.lighting = forceLightingDebug
        ? float3(4.0, 0.0, 0.0)
        : (lightingEnabled
            ? max(ParticleLighting[vertex.lightingInfo.x].rgb, float3(0.0, 0.0, 0.0))
            : ParticleConstants.cameraLeftAndAmbient.www);
    return result;
}
