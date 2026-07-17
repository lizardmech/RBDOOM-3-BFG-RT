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

Texture2D ParticleTexture : register(t1);
Texture2D<float4> GuidePositionTexture : register(t2);
SamplerState ParticleSampler : register(s0);

struct PS_IN
{
    float4 position : SV_Position;
    float2 texCoord : TEXCOORD0;
    float4 color : COLOR0;
    float viewDepth : TEXCOORD1;
};

float4 main(PS_IN input) : SV_Target0
{
    float softFade = 1.0;
    const uint depthPolicy = uint(ParticleConstants.batchInfo.x + 0.5);
    if (depthPolicy != 1u)
    {
        const float2 outputSize = max(ParticleConstants.outputAndRenderSize.xy, float2(1.0, 1.0));
        const float2 renderSize = max(ParticleConstants.outputAndRenderSize.zw, float2(1.0, 1.0));
        const int2 guidePixel = int2(clamp(input.position.xy * renderSize / outputSize, float2(0.0, 0.0), renderSize - float2(1.0, 1.0)));
        const float4 guidePosition = GuidePositionTexture.Load(int3(guidePixel, 0));
        if (guidePosition.w > 0.0)
        {
            const float guideDepth = dot(
                guidePosition.xyz - ParticleConstants.cameraOriginAndTanX.xyz,
                ParticleConstants.cameraForwardAndTanY.xyz);
            const float depthGap = guideDepth - input.viewDepth;
            if (depthPolicy == 2u)
            {
                const float zNear = max(ParticleConstants.modelInfo.y, 1.0e-4);
                const float particleProjectionDepth =
                    0.999 - zNear / max(input.viewDepth, zNear) -
                    ParticleConstants.modelInfo.x / max(input.viewDepth, zNear);
                const float guideProjectionDepth = 0.999 - zNear / max(guideDepth, zNear);
                if (particleProjectionDepth > guideProjectionDepth + 1.0e-4)
                {
                    discard;
                }
            }
            else
            {
                if (depthGap < -0.02)
                {
                    discard;
                }
                softFade = saturate(depthGap / max(ParticleConstants.batchInfo.z, 1.0e-3));
            }
        }
    }

    float4 texel = ParticleTexture.Sample(ParticleSampler, input.texCoord) * input.color;
    texel.a *= softFade;
    const uint blendClass = uint(ParticleConstants.batchInfo.y + 0.5);
    if ((blendClass != 2u && texel.a <= (1.0 / 255.0)) ||
        (blendClass == 2u && max(max(abs(texel.r), abs(texel.g)), abs(texel.b)) <= (1.0 / 255.0)))
    {
        discard;
    }

    float3 rgb;
    if (blendClass == 0u)
    {
        rgb = texel.rgb * ParticleConstants.cameraLeftAndAmbient.w;
    }
    else if (blendClass == 1u)
    {
        rgb = texel.rgb * ParticleConstants.cameraUpAndEmissiveScale.w;
    }
    else
    {
        rgb = texel.rgb * ParticleConstants.cameraUpAndEmissiveScale.w * softFade;
    }
    if (ParticleConstants.batchInfo.w > 0.5)
    {
        rgb = lerp(rgb, float3(1.0, 0.0, 1.0), 0.65);
    }
    return float4(rgb, blendClass == 2u ? 0.0 : texel.a);
}
