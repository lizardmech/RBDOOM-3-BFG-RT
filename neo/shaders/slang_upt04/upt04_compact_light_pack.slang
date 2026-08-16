import upt04_light_contract;
import upt04_geometry_contract;
import upt04_hit_contract;
import upt04_material_contract;
import upt04_material_emission;
import upt04_material_live_contract;

struct Upt04CompactLightPackControl
{
    uint lightCount;
    uint materialCount;
    uint logicalTextureCount;
    uint reserved0;
    float emissiveScale;
    float emissiveTexelBlackFloor;
    uint reserved1;
    uint reserved2;
};

[[vk::binding(0, 0)]] StructuredBuffer<Upt04UnifiedLightRecord> gLightSource;
[[vk::binding(1, 0)]] RWStructuredBuffer<Upt04CompactLightRecord> gCompactLights;
[[vk::binding(2, 0)]] StructuredBuffer<Upt04EmissiveReplayRecord> gEmissiveReplay;
[[vk::binding(3, 0)]] StructuredBuffer<Upt04EmissiveGeometryRecord> gEmissiveGeometry;
[[vk::binding(4, 0)]] StructuredBuffer<Upt04SmokeMaterialRecord> gMaterials;
[[vk::binding(5, 0)]] SamplerState gMaterialSampler;
[[vk::binding(0, 1)]] Texture2D<float4> gMaterialTextures[];
[[vk::push_constant]] Upt04CompactLightPackControl gPackControl;

static const uint kUpt54EmissiveGeometryValid = 0x47505545u;

float Upt54EmissiveImportance(float3 radiance)
{
    const float value = dot(
        max(radiance, float3(0.0f)),
        float3(0.2126f, 0.7152f, 0.0722f));
    return isfinite(value) ? max(value, 0.0f) : 0.0f;
}

float2 Upt54RegionProbeBarycentrics(uint region)
{
    if (region == 0u)
        return float2(1.0f / 6.0f, 1.0f / 6.0f);
    if (region == 1u)
        return float2(2.0f / 3.0f, 1.0f / 6.0f);
    if (region == 2u)
        return float2(1.0f / 6.0f, 2.0f / 3.0f);
    return float2(1.0f / 3.0f, 1.0f / 3.0f);
}

bool Upt54PrepareEmissiveRegionWeights(
    Upt04UnifiedLightRecord light,
    out float4 weights)
{
    weights = float4(-1.0f);
    if (light.type != kUpt04LightEmissiveTriangle)
        return false;

    uint replayCount;
    uint replayStride;
    uint geometryCount;
    uint geometryStride;
    uint materialBufferCount;
    uint materialStride;
    gEmissiveReplay.GetDimensions(replayCount, replayStride);
    gEmissiveGeometry.GetDimensions(geometryCount, geometryStride);
    gMaterials.GetDimensions(materialBufferCount, materialStride);
    if (light.sourceIndex >= replayCount
        || light.sourceIndex >= geometryCount)
        return false;

    const Upt04EmissiveReplayRecord replay =
        gEmissiveReplay[light.sourceIndex];
    const Upt04EmissiveGeometryRecord geometry =
        gEmissiveGeometry[light.sourceIndex];
    if (replay.instanceId != light.instanceId
        || replay.primitiveIndex != light.primitiveIndex
        || replay.materialIndex != light.materialOrLightId
        || replay.identityHashLo != light.identityA
        || replay.identityHashHi != light.identityB
        || geometry.validity != kUpt54EmissiveGeometryValid
        || geometry.identityHashLo != light.identityA
        || geometry.identityHashHi != light.identityB
        || replay.materialIndex >= gPackControl.materialCount
        || replay.materialIndex >= materialBufferCount)
        return false;

    const float2 uv0 = float2(
        geometry.position0AndUv0X.w,
        geometry.position1AndUv0Y.w);
    const float2 uv1 = float2(
        geometry.position2AndUv1X.w,
        geometry.uv1Y);
    const float2 uv2 = 3.0f * replay.centroidUvAndWeight.xy - uv0 - uv1;
    const float2 uvGradient0 = uv1 - uv0;
    const float2 uvGradient1 = uv2 - uv0;
    if (!all(isfinite(uv0)) || !all(isfinite(uv1))
        || !all(isfinite(uv2)) || !all(isfinite(uvGradient0))
        || !all(isfinite(uvGradient1)))
        return false;

    const Upt04SmokeMaterialRecord material = gMaterials[replay.materialIndex];
    float4 prepared = float4(0.0f);
    [loop]
    for (uint region = 0u; region < 4u; ++region)
    {
        const float2 barycentrics = Upt54RegionProbeBarycentrics(region);
        const float2 uv = uv0
            + barycentrics.x * uvGradient0
            + barycentrics.y * uvGradient1;
        float3 emissiveTexel = float3(1.0f);
        uint emissiveTexelValid = 1u;
        if (material.emissiveTextureIndex != kUpt04InvalidIdentity)
        {
            if (material.emissiveTextureIndex
                >= gPackControl.logicalTextureCount)
            {
                emissiveTexelValid = 0u;
            }
            else
            {
                emissiveTexel = gMaterialTextures[NonUniformResourceIndex(
                    material.emissiveTextureIndex)].SampleGrad(
                        gMaterialSampler,
                        frac(uv),
                        uvGradient0 * 0.5f,
                        uvGradient1 * 0.5f).xyz;
            }
        }
        const Upt04MaterialEmissionEvaluation emission =
            Upt04FinalizeEmissiveEndpoint(
                material,
                replay.padding0,
                emissiveTexel,
                emissiveTexelValid,
                gPackControl.emissiveScale,
                gPackControl.emissiveTexelBlackFloor);
        if (emission.valid == 0u)
            return false;
        prepared[region] = Upt54EmissiveImportance(emission.radiance);
    }
    if (!all(isfinite(prepared)) || any(prepared < 0.0f))
        return false;
    weights = prepared;
    return true;
}

[shader("compute")]
[numthreads(128, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint lightIndex = dispatchThreadId.x;
    if (lightIndex < gPackControl.lightCount)
    {
        const Upt04UnifiedLightRecord light = gLightSource[lightIndex];
        Upt04CompactLightRecord packed = Upt04PackCompactLight(light);
        if (light.type == kUpt04LightEmissiveTriangle)
        {
            float4 regionWeights;
            if (Upt54PrepareEmissiveRegionWeights(light, regionWeights))
                packed.positionAndRadius = asuint(regionWeights);
            else
                packed.positionAndRadius = asuint(float4(-1.0f));
        }
        gCompactLights[lightIndex] = packed;
    }
}
