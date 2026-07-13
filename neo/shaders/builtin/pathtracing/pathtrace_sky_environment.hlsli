#ifndef RB_PATH_TRACE_SKY_ENVIRONMENT_HLSLI
#define RB_PATH_TRACE_SKY_ENVIRONMENT_HLSLI

static const uint RT_SMOKE_TEXTURE_FLAG_SKY_CUBE_SHADER = 1u << 12u;

#if defined(RB_PATH_TRACE_FIXED_SKY_CUBE)
TextureCube<float4> PathTraceSkyEnvironmentCube : register(t126);
#endif

float PathTraceSkyEnvironmentLinear1(float value)
{
    return value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4);
}

float3 PathTraceSkyEnvironmentLinear3(float3 value)
{
    return float3(
        PathTraceSkyEnvironmentLinear1(value.r),
        PathTraceSkyEnvironmentLinear1(value.g),
        PathTraceSkyEnvironmentLinear1(value.b));
}

float3 PathTraceSampleSkyEnvironment(float3 direction, float4 textureInfo)
{
    const uint textureFlags = (uint)max(textureInfo.w, 0.0);
    if ((textureFlags & RT_SMOKE_TEXTURE_FLAG_SKY_CUBE_SHADER) == 0u)
    {
        return float3(1.0, 1.0, 1.0);
    }

#if defined(RB_PATH_TRACE_FIXED_SKY_CUBE)
    const float3 encoded = PathTraceSkyEnvironmentCube.SampleLevel(
        SmokeMaterialSampler,
        normalize(direction),
        0.0).rgb;
    return PathTraceSkyEnvironmentLinear3(encoded);
#else
    return float3(1.0, 1.0, 1.0);
#endif
}

#endif
