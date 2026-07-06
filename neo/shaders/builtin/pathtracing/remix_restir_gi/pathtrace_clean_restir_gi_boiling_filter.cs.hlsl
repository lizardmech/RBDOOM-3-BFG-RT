// Clean-room ReSTIR GI boiling filter + resolve consumer (RGI-08).
//
// Remix final-shading-shaped boiling filter for the shaded GI output:
// compute the threadgroup's average nonzero shaded diffuse/specular luminance
// separately and clamp outliers down to that lobe's average.
// This pass also owns the beauty resolve add so the combined output receives
// the FILTERED contribution. RR input is owned by explicit RR export paths.
//
// io_whitelist: reads GI-O-05 (shaded indirect GI) + GI-I-01 (validity only);
// writes GI-O-05 (filtered in place) and the combined beauty outputs under
// r_pathTracingCleanRestirGiResolve.

#include "../../../vulkan.hlsli"
#include "../PathTracePrimarySurface.hlsli"

cbuffer PathTraceCleanRestirGiBoilingFilterConstants : register(b0)
{
    uint CleanGiBfWidth;
    uint CleanGiBfHeight;
    float CleanGiBfThresholdMin;  // 0 disables clamping
    float CleanGiBfThresholdMax;
    uint CleanGiBfResolveEnabled; // adds shaded indirect GI into SmokeOutput
    uint CleanGiBfRrInputResolveEnabled; // legacy/debug only; normal RR export owns RR input
    uint CleanGiBfRrSpecularInputEnabled; // adds filtered eligible specular GI to RR input
    float CleanGiBfResolveGain;
};

VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> CleanGiBfIndirectDiffuse : register(u1);
VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> CleanGiBfSmokeOutput : register(u2);
VK_IMAGE_FORMAT("rgba32f") RWTexture2D<float4> CleanGiBfRRInputColor : register(u3);
StructuredBuffer<PathTracePrimarySurfaceRecord> CleanGiBfPrimarySurfaces : register(t4);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> CleanGiBfDiffuseLobe : register(u5);
VK_IMAGE_FORMAT("rgba16f") RWTexture2D<float4> CleanGiBfSpecularLobe : register(u6);

static const uint CLEAN_GI_BF_GROUP_SIZE = 8u;

groupshared float2 CleanGiBfLuminanceSum[CLEAN_GI_BF_GROUP_SIZE * CLEAN_GI_BF_GROUP_SIZE];
groupshared uint2 CleanGiBfNonzeroCount[CLEAN_GI_BF_GROUP_SIZE * CLEAN_GI_BF_GROUP_SIZE];

float CleanGiBfLuminance(float3 value)
{
    return dot(max(value, float3(0.0, 0.0, 0.0)), float3(0.2126, 0.7152, 0.0722));
}

float3 CleanGiBfSanitize(float3 value)
{
    value = max(value, float3(0.0, 0.0, 0.0));
    return all(value == value) ? value : float3(0.0, 0.0, 0.0);
}

float3 CleanGiBfClampLobe(float3 lobe, float luminance, float averageLuminance, float threshold)
{
    if (threshold > 0.0 &&
        averageLuminance > 0.0 &&
        luminance > averageLuminance * threshold)
    {
        return lobe * (averageLuminance / max(luminance, 1.0e-6));
    }
    return lobe;
}

float CleanGiBfViewDotNormal(PathTracePrimarySurfaceRecord record)
{
    if (record.header.x != RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION ||
        (record.header.y & RT_PRIMARY_SURFACE_VALID) == 0u)
    {
        return 0.0;
    }

    const float3 normal = normalize(record.geometricNormalAndRoughness.xyz);
    const float3 viewDir = normalize(record.viewDirectionAndReserved.xyz);
    const float vdotn = abs(dot(viewDir, normal));
    return vdotn == vdotn ? saturate(vdotn) : 0.0;
}

bool CleanGiBfSpecularExportEligible(float3 diffuse, float3 specular, float hitDistance)
{
    if (hitDistance <= 0.0 || hitDistance >= 1.0e8)
    {
        return false;
    }

    const float specularLuminance = CleanGiBfLuminance(specular);
    const float diffuseLuminance = CleanGiBfLuminance(diffuse);
    return specularLuminance > 1.0e-4 &&
        specularLuminance >= max(diffuseLuminance * 0.10, 1.0e-4);
}

[numthreads(CLEAN_GI_BF_GROUP_SIZE, CLEAN_GI_BF_GROUP_SIZE, 1)]
void main(uint2 pixel : SV_DispatchThreadID, uint2 localIndex : SV_GroupThreadID)
{
    const bool inBounds = pixel.x < CleanGiBfWidth && pixel.y < CleanGiBfHeight;

    float3 diffuse = float3(0.0, 0.0, 0.0);
    float3 specular = float3(0.0, 0.0, 0.0);
    float specularHitDistance = 0.0;
    if (inBounds)
    {
        diffuse = CleanGiBfSanitize(CleanGiBfDiffuseLobe[pixel].rgb);
        const float4 specularAndHitDistance = CleanGiBfSpecularLobe[pixel];
        specular = CleanGiBfSanitize(specularAndHitDistance.rgb);
        specularHitDistance = specularAndHitDistance.a;
    }
    const float diffuseLuminance = CleanGiBfLuminance(diffuse);
    const float specularLuminance = CleanGiBfLuminance(specular);

    // Group reduction of the nonzero shaded luminance (portable groupshared
    // tree instead of wave intrinsics).
    const uint linearIndex = localIndex.x + localIndex.y * CLEAN_GI_BF_GROUP_SIZE;
    CleanGiBfLuminanceSum[linearIndex] = float2(diffuseLuminance, specularLuminance);
    CleanGiBfNonzeroCount[linearIndex] = uint2(
        diffuseLuminance > 0.0 ? 1u : 0u,
        specularLuminance > 0.0 ? 1u : 0u);
    GroupMemoryBarrierWithGroupSync();

    for (uint stride = (CLEAN_GI_BF_GROUP_SIZE * CLEAN_GI_BF_GROUP_SIZE) / 2u; stride > 0u; stride >>= 1u)
    {
        if (linearIndex < stride)
        {
            CleanGiBfLuminanceSum[linearIndex] += CleanGiBfLuminanceSum[linearIndex + stride];
            CleanGiBfNonzeroCount[linearIndex] += CleanGiBfNonzeroCount[linearIndex + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    const uint2 nonzeroCount = CleanGiBfNonzeroCount[0];
    const float2 averageNonzeroLuminance = float2(
        nonzeroCount.x > 0u ? CleanGiBfLuminanceSum[0].x / (float)nonzeroCount.x : 0.0,
        nonzeroCount.y > 0u ? CleanGiBfLuminanceSum[0].y / (float)nonzeroCount.y : 0.0);

    if (!inBounds)
    {
        return;
    }

    const PathTracePrimarySurfaceRecord record = CleanGiBfPrimarySurfaces[pixel.y * CleanGiBfWidth + pixel.x];
    const float thresholdMax = CleanGiBfThresholdMax > 0.0 ? CleanGiBfThresholdMax : CleanGiBfThresholdMin;
    const float threshold = lerp(
        CleanGiBfThresholdMin,
        thresholdMax,
        CleanGiBfViewDotNormal(record));

    const float3 filteredDiffuse = CleanGiBfClampLobe(
        diffuse, diffuseLuminance, averageNonzeroLuminance.x, threshold);
    const float3 filteredSpecular = CleanGiBfClampLobe(
        specular, specularLuminance, averageNonzeroLuminance.y, threshold);
    const float3 indirect = filteredDiffuse + filteredSpecular;

    if (any(filteredDiffuse != diffuse))
    {
        CleanGiBfDiffuseLobe[pixel] = float4(filteredDiffuse, 1.0);
    }
    if (any(filteredSpecular != specular))
    {
        CleanGiBfSpecularLobe[pixel] = float4(filteredSpecular, specularHitDistance);
    }
    CleanGiBfIndirectDiffuse[pixel] = float4(indirect, 1.0);

    if (CleanGiBfResolveEnabled != 0u)
    {
        if (record.header.x == RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION &&
            (record.header.y & RT_PRIMARY_SURFACE_VALID) != 0u)
        {
            const float3 resolvedIndirect = indirect * max(CleanGiBfResolveGain, 0.0);
            CleanGiBfSmokeOutput[pixel] += float4(resolvedIndirect, 0.0);
            if (CleanGiBfRrInputResolveEnabled != 0u)
            {
                CleanGiBfRRInputColor[pixel] += float4(resolvedIndirect, 0.0);
            }
        }
    }

    if (CleanGiBfRrSpecularInputEnabled != 0u &&
        CleanGiBfSpecularExportEligible(filteredDiffuse, filteredSpecular, specularHitDistance))
    {
        CleanGiBfRRInputColor[pixel] += float4(filteredSpecular, 0.0);
    }
}
