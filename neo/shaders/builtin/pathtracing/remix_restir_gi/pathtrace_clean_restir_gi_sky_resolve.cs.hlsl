// Route-independent sky correction for clean ReSTIR GI. Trace and shade may
// execute as RT libraries, inline ray-query compute, or a combined diagnostic
// producer. All routes publish the same first-indirect candidate surface, so
// cube sampling is isolated here at a proven-safe compute boundary.

#include "../../../vulkan.hlsli"
#include "../cleanroom_common/pathtrace_first_indirect_candidate.hlsli"

cbuffer CleanRestirGiSkyResolveConstants : register(b0)
{
    uint2 CleanRestirGiSkyResolveDimensions;
    float CleanRestirGiSkyResolveBrightness;
    uint CleanRestirGiSkyResolveMode;
};

TextureCube<float4> CleanRestirGiSkyResolveCube : register(t0);
SamplerState CleanRestirGiSkyResolveSampler : register(s0);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> CleanRestirGiSkyResolveRadiance : register(u0);
RWStructuredBuffer<PathTraceFirstIndirectCandidateSurface> CleanRestirGiSkyResolveSurfaces : register(u1);

static const uint RT_SMOKE_MATERIAL_SKY_ENVIRONMENT = 0x00040000u;

float CleanRestirGiSkyResolveLinear1(float value)
{
    return value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4);
}

float3 CleanRestirGiSkyResolveLinear3(float3 value)
{
    return float3(
        CleanRestirGiSkyResolveLinear1(value.r),
        CleanRestirGiSkyResolveLinear1(value.g),
        CleanRestirGiSkyResolveLinear1(value.b));
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (pixel.x >= CleanRestirGiSkyResolveDimensions.x ||
        pixel.y >= CleanRestirGiSkyResolveDimensions.y)
    {
        return;
    }

    const uint index = pixel.y * CleanRestirGiSkyResolveDimensions.x + pixel.x;
    PathTraceFirstIndirectCandidateSurface surface = CleanRestirGiSkyResolveSurfaces[index];
    if (surface.valid == 0u ||
        (surface.materialFlags & RT_SMOKE_MATERIAL_SKY_ENVIRONMENT) == 0u)
    {
        return;
    }

    const float3 encoded = CleanRestirGiSkyResolveCube.SampleLevel(
        CleanRestirGiSkyResolveSampler,
        normalize(-surface.viewDir),
        0.0).rgb;
    const float3 multiplier = CleanRestirGiSkyResolveLinear3(encoded) *
        max(CleanRestirGiSkyResolveBrightness, 0.0);

    if (CleanRestirGiSkyResolveMode == 0u)
    {
        float4 radiance = CleanRestirGiSkyResolveRadiance[pixel];
        radiance.rgb = max(radiance.rgb * multiplier, float3(0.0, 0.0, 0.0));
        CleanRestirGiSkyResolveRadiance[pixel] = radiance;
    }
    else
    {
        surface.emissiveRadiance = max(
            surface.emissiveRadiance * multiplier,
            float3(0.0, 0.0, 0.0));
        CleanRestirGiSkyResolveSurfaces[index] = surface;
    }
}
