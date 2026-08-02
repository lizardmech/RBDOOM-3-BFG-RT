import upt04_light_contract;

struct Upt04CompactLightPackControl
{
    uint lightCount;
};

[[vk::binding(0, 0)]] StructuredBuffer<Upt04UnifiedLightRecord> gLightSource;
[[vk::binding(1, 0)]] RWStructuredBuffer<Upt04CompactLightRecord> gCompactLights;
[[vk::push_constant]] Upt04CompactLightPackControl gPackControl;

[shader("compute")]
[numthreads(128, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint lightIndex = dispatchThreadId.x;
    if (lightIndex < gPackControl.lightCount)
        gCompactLights[lightIndex] = Upt04PackCompactLight(gLightSource[lightIndex]);
}
