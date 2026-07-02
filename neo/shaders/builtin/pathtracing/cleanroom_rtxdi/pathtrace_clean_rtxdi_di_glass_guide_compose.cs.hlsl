#include "../../../vulkan.hlsli"

Texture2D<float4> CleanRtxdiDiGlassGuideCandidate0 : register(t1);
Texture2D<float4> CleanRtxdiDiGlassGuideCandidate1 : register(t2);
Texture2D<float4> CleanRtxdiDiGlassGuideCandidate2 : register(t3);

VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> CleanRtxdiDiRRGuideNormalRoughness : register(u4);
VK_IMAGE_FORMAT("r32f") RWTexture2D<float> CleanRtxdiDiRRGuideDepth : register(u5);
VK_IMAGE_FORMAT("r32ui") RWTexture2D<uint> CleanRtxdiDiRRGuideResetMask : register(u6);
VK_IMAGE_FORMAT("rg16f") RWTexture2D<float2> CleanRtxdiDiRRMotionVectors : register(u7);
VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> CleanRtxdiDiRRGuidePosition : register(u8);

cbuffer CleanRtxdiDiGlassGuideComposeConstants : register(b0)
{
    uint2 CleanRtxdiDiGlassGuideComposeDimensions;
    uint CleanRtxdiDiGlassGuideComposeEnabled;
    uint CleanRtxdiDiGlassGuideComposeResetOnReplacement;
    float CleanRtxdiDiGlassGuideComposeMinWeight;
    float3 CleanRtxdiDiGlassGuideComposePadding;
};

bool CleanRtxdiDiGlassGuideComposeFinite4(float4 value)
{
    return all(value == value);
}

bool CleanRtxdiDiGlassGuideComposeFinite3(float3 value)
{
    return all(value == value);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= CleanRtxdiDiGlassGuideComposeDimensions.x ||
        pixel.y >= CleanRtxdiDiGlassGuideComposeDimensions.y ||
        CleanRtxdiDiGlassGuideComposeEnabled == 0u)
    {
        return;
    }

    const float4 candidate0 = CleanRtxdiDiGlassGuideCandidate0[pixel];
    const float4 candidate1 = CleanRtxdiDiGlassGuideCandidate1[pixel];
    const float4 candidate2 = CleanRtxdiDiGlassGuideCandidate2[pixel];
    const float candidateWeight = saturate(candidate1.w);
    if (candidateWeight < max(CleanRtxdiDiGlassGuideComposeMinWeight, 0.0))
    {
        return;
    }
    if (!CleanRtxdiDiGlassGuideComposeFinite4(candidate0) ||
        !CleanRtxdiDiGlassGuideComposeFinite4(candidate1) ||
        !CleanRtxdiDiGlassGuideComposeFinite3(candidate2.xyz))
    {
        return;
    }

    const float depth = candidate0.w;
    if (depth <= 0.0)
    {
        return;
    }
    const float normalLength = length(candidate0.xyz);
    if (normalLength <= 1.0e-5)
    {
        return;
    }

    CleanRtxdiDiRRGuideNormalRoughness[pixel] =
        float4(candidate0.xyz / normalLength, saturate(candidate2.w));
    CleanRtxdiDiRRGuideDepth[pixel] = depth;
    CleanRtxdiDiRRMotionVectors[pixel] = candidate1.xy;
    CleanRtxdiDiRRGuidePosition[pixel] = float4(candidate2.xyz, 1.0);
    if (CleanRtxdiDiGlassGuideComposeResetOnReplacement != 0u)
    {
        CleanRtxdiDiRRGuideResetMask[pixel] = candidate1.z > 0.5 ? 0xffffffffu : 0u;
    }
}
