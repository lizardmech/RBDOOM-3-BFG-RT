struct ParticleCompositeVertex
{
    float3 worldPosition;
    float2 texCoord;
    uint packedColor;
    uint particleIdLow;
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

struct VS_OUT
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
    float4 color : COLOR0;
    float viewDepth : TEXCOORD1;
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
    const float safeDepth = max(viewDepth, 1.0e-3);

    VS_OUT result;
    result.position = float4(
        -dot(relative, ParticleConstants.cameraLeftAndAmbient.xyz) / max(ParticleConstants.cameraOriginAndTanX.w, 1.0e-5),
        dot(relative, ParticleConstants.cameraUpAndEmissiveScale.xyz) / max(ParticleConstants.cameraForwardAndTanY.w, 1.0e-5),
        0.0,
        safeDepth);
    result.texCoord = vertex.texCoord;
    result.color = UnpackParticleColor(vertex.packedColor);
    result.viewDepth = viewDepth;
    return result;
}
