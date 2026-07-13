// Resolves authored sky radiance for terminal sky surfaces published by the
// thin-glass PSR pass.  Cube sampling stays in a compute pipeline, matching the
// already-proven renderer-owned cube contract rather than dereferencing a cube
// from the transmission RT library.

struct PathTracePrimarySurfaceRecord
{
    uint4 header;
    float4 worldPositionAndViewDepth;
    float4 geometricNormalAndRoughness;
    float4 shadingNormalAndOpacity;
    float4 viewDirectionAndReserved;
    float4 albedoAndAlphaCutoff;
    float4 specularF0AndReserved;
    float4 emissiveAndHeight;
    float4 previousPositionOrMotion;
    uint4 materialAndSurface;
    uint4 instancePrimitiveObject;
};

cbuffer PathTraceSkySurfaceResolveConstants : register(b0)
{
    uint2 PathTraceSkySurfaceResolveDimensions;
    float PathTraceSkySurfaceResolveBrightness;
    uint PathTraceSkySurfaceResolveEnabled;
};

TextureCube<float4> PathTraceSkySurfaceResolveCube : register(t0);
SamplerState PathTraceSkySurfaceResolveSampler : register(s0);
RWStructuredBuffer<PathTracePrimarySurfaceRecord> PathTraceSkySurfaceRecords : register(u0);
RWTexture2D<float4> PathTraceSkySurfaceSpecularAlbedo : register(u1);
RWTexture2D<float4> PathTraceSkySurfaceReflectionSidecar : register(u2);
RWTexture2D<float4> PathTraceSkySurfaceGuidePosition : register(u3);

static const uint RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION = 2u;
static const uint RT_PRIMARY_SURFACE_VALID = 0x00000001u;
static const uint RT_SMOKE_MATERIAL_SKY_ENVIRONMENT = 0x00040000u;
static const uint CLEAN_SURFACE_FLAG_TRANSMISSION_PSR_RESOLVED = 0x80000000u;
static const uint CLEAN_SURFACE_FLAG_REFLECTION_PSR_RESOLVED = 0x20000000u;
static const float PATH_TRACE_REFLECTION_SIDECAR_HYBRID_RADIANCE = 0.875;
static const float PATH_TRACE_SKY_REFLECTION_DIRECTION_MARKER = -2.0;

float PathTraceSkySurfaceLinear1(float value)
{
    return value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4);
}

float3 PathTraceSkySurfaceLinear3(float3 value)
{
    return float3(
        PathTraceSkySurfaceLinear1(value.r),
        PathTraceSkySurfaceLinear1(value.g),
        PathTraceSkySurfaceLinear1(value.b));
}

float3 PathTraceSkySurfaceSample(float3 direction)
{
    const float3 encoded = PathTraceSkySurfaceResolveCube.SampleLevel(
        PathTraceSkySurfaceResolveSampler,
        normalize(direction),
        0.0).rgb;
    return PathTraceSkySurfaceLinear3(encoded) *
        max(PathTraceSkySurfaceResolveBrightness, 0.0);
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint2 pixel = dispatchThreadId.xy;
    if (PathTraceSkySurfaceResolveEnabled == 0u ||
        pixel.x >= PathTraceSkySurfaceResolveDimensions.x ||
        pixel.y >= PathTraceSkySurfaceResolveDimensions.y)
    {
        return;
    }

    const uint recordIndex = pixel.y * PathTraceSkySurfaceResolveDimensions.x + pixel.x;
    PathTracePrimarySurfaceRecord record = PathTraceSkySurfaceRecords[recordIndex];
    if (record.header.x != RT_PATH_TRACE_PRIMARY_SURFACE_RECORD_VERSION ||
        (record.header.y & RT_PRIMARY_SURFACE_VALID) == 0u)
    {
        return;
    }

    const bool psrResolved =
        (record.header.w &
            (CLEAN_SURFACE_FLAG_TRANSMISSION_PSR_RESOLVED |
                CLEAN_SURFACE_FLAG_REFLECTION_PSR_RESOLVED)) != 0u;
    if (psrResolved &&
        (record.materialAndSurface.z & RT_SMOKE_MATERIAL_SKY_ENVIRONMENT) != 0u)
    {
        // RAB viewDir points from the surface toward the camera; environment
        // lookup requires the traced camera-to-surface direction.
        const float3 environmentDirection = -record.viewDirectionAndReserved.xyz;
        const float3 radiance = max(
            PathTraceSkySurfaceSample(environmentDirection) *
                max(record.emissiveAndHeight.rgb, float3(0.0, 0.0, 0.0)),
            float3(0.0, 0.0, 0.0));
        record.emissiveAndHeight.rgb = radiance;
        PathTraceSkySurfaceRecords[recordIndex] = record;

        const float peak = max(max(radiance.r, radiance.g), radiance.b);
        PathTraceSkySurfaceSpecularAlbedo[pixel] = float4(
            peak > 1.0e-5 ? saturate(radiance / peak) : float3(0.0, 0.0, 0.0),
            1.0);
    }

    // The transmission RT pass cannot safely dereference the renderer cube on
    // the affected Vulkan driver. It leaves the mirror direction in the RR
    // position guide for one dispatch; color the already-Fresnel-weighted
    // hybrid reflection here, then restore the guide before DI/RR consumes it.
    const float4 guidePosition = PathTraceSkySurfaceGuidePosition[pixel];
    if (guidePosition.w <= PATH_TRACE_SKY_REFLECTION_DIRECTION_MARKER + 0.25)
    {
        float4 reflectionSidecar = PathTraceSkySurfaceReflectionSidecar[pixel];
        if (abs(reflectionSidecar.a - PATH_TRACE_REFLECTION_SIDECAR_HYBRID_RADIANCE) < 0.03)
        {
            reflectionSidecar.rgb = max(
                reflectionSidecar.rgb * PathTraceSkySurfaceSample(guidePosition.xyz),
                float3(0.0, 0.0, 0.0));
            PathTraceSkySurfaceReflectionSidecar[pixel] = reflectionSidecar;
        }
        PathTraceSkySurfaceGuidePosition[pixel] = float4(
            record.worldPositionAndViewDepth.xyz,
            1.0);
    }
}
