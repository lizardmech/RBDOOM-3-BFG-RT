#ifndef RB_PATH_TRACE_CLEAN_RTXDI_DI_MATH_HLSLI
#define RB_PATH_TRACE_CLEAN_RTXDI_DI_MATH_HLSLI

uint PathTraceCleanRoomHash(uint value)
{
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

float PathTraceCleanRoomRandom01(uint seed)
{
    return (float)(PathTraceCleanRoomHash(seed) & 0x00ffffffu) * (1.0 / 16777215.0);
}

RTXDI_RandomSamplerState PathTraceCleanRoomInitDecorrelatedSampler(uint2 pixel, uint frameIndex, uint pass)
{
    uint seed = pixel.x * 0x9e3779b9u;
    seed ^= pixel.y * 0x85ebca6bu;
    seed ^= frameIndex * 0xc2b2ae35u;
    seed ^= pass * 0x27d4eb2fu;
    seed = PathTraceCleanRoomHash(seed ^ (seed >> 11u));
    return RTXDI_CreateRandomSamplerFromDirectSeed(seed, 1u);
}

float3 PathTraceCleanRoomHashColor(uint value)
{
    const uint hashValue = PathTraceCleanRoomHash(value);
    const float3 color = float3(
        (float)((hashValue >> 0u) & 255u),
        (float)((hashValue >> 8u) & 255u),
        (float)((hashValue >> 16u) & 255u)) * (1.0 / 255.0);
    return color * 0.75 + float3(0.18, 0.18, 0.18);
}

float3 PathTraceCleanRoomSafeNormalize(float3 value, float3 fallback)
{
    const float lengthSquared = dot(value, value);
    return lengthSquared > 1.0e-8 ? value * rsqrt(lengthSquared) : fallback;
}

float PathTraceCleanRoomLuminance(float3 value)
{
    return dot(max(value, float3(0.0, 0.0, 0.0)), float3(0.2126, 0.7152, 0.0722));
}

float3 PathTraceCleanRoomPerpendicular(float3 normal)
{
    const float3 axis = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(0.0, 1.0, 0.0);
    return PathTraceCleanRoomSafeNormalize(cross(axis, normal), float3(1.0, 0.0, 0.0));
}

float3 PathTraceCleanRoomSampleCone(float3 axis, float cosThetaMax, float2 uv)
{
    const float cosTheta = lerp(1.0, cosThetaMax, saturate(uv.x));
    const float sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    const float phi = 2.0 * CLEAN_RTXDI_PI * saturate(uv.y);
    const float3 tangent = PathTraceCleanRoomPerpendicular(axis);
    const float3 bitangent = PathTraceCleanRoomSafeNormalize(cross(axis, tangent), float3(0.0, 1.0, 0.0));
    return PathTraceCleanRoomSafeNormalize(axis * cosTheta + tangent * (cos(phi) * sinTheta) + bitangent * (sin(phi) * sinTheta), axis);
}

#endif
