// Isolated validation of the renderer-owned sky cube image/view contract.
// This shader is deliberately not included by any ray-tracing entry point.

TextureCube<float4> PathTraceSkyCube : register(t0);
SamplerState PathTraceSkySampler : register(s0);
RWTexture2D<float4> PathTraceSkyProbeOutput : register(u1);

float3 PathTraceSkyProbeDirection(uint face)
{
    if (face == 0u) return float3( 1.0,  0.0,  0.0);
    if (face == 1u) return float3(-1.0,  0.0,  0.0);
    if (face == 2u) return float3( 0.0,  1.0,  0.0);
    if (face == 3u) return float3( 0.0, -1.0,  0.0);
    if (face == 4u) return float3( 0.0,  0.0,  1.0);
    return                  float3( 0.0,  0.0, -1.0);
}

[numthreads(1, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= 6u || dispatchThreadId.y != 0u || dispatchThreadId.z != 0u)
    {
        return;
    }

    PathTraceSkyProbeOutput[dispatchThreadId.xy] =
        PathTraceSkyCube.SampleLevel(PathTraceSkySampler, PathTraceSkyProbeDirection(dispatchThreadId.x), 0.0);
}
